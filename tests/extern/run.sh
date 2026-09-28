#!/usr/bin/env bash
# tests/extern/run.sh —— **`extern!` + 信任声明（定案 72）的常设验收**
#
# 判据：① 签了字的声明**能用**（真调 libc 并看见效果 ✓）
#       ② 没签字的声明**退回最保守**（传本帧地址 = 编译错误 ✓）
#       ③ 跨边界形状不对（slice / struct）⇒ 编译错误 ✓
#       ④ `owned` 还没实现 ⇒ 明确报错（不许静默泄漏 ✗）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（签字之后能用：getpid / write）=="
if out=$(cd tests/extern && "$OLDPWD/$EXTC" --run main.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "pid 是一个正数 ✓" || ok=0
    echo "$out" | grep -qF "写了 5 字节 ✓"    || ok=0
    if [ "$ok" = 1 ]; then echo "  ok   extern-libc  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL extern-libc  ->  输出对不上"; fail=1; fi
else
    echo "  FAIL extern-libc  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo '== 指针过界（C-ABI.md §9.6 第 2 步：ref T / ref void / ?ref void）=='
if out=$(cd tests/extern && "$OLDPWD/$EXTC" --run capi-main.extc 2>&1); then
    ok=1
    for want in "timeval=ok" "void=ok" "indirect=ok"; do
        echo "$out" | grep -qF "$want" || { ok=0; echo "  FAIL 输出里缺「$want」"; }
    done
    if [ "$ok" = 1 ]; then echo "  ok   跨边界指针  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL 跨边界指针  ->  输出对不上"; fail=1; fi
else
    echo "  FAIL 跨边界指针  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 反例（都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-86)"
}
check_err tests/extern/errors/no_effects.extc  'may be kept by C forever'
check_err tests/extern/errors/slice_param.extc 'cannot cross the C boundary'
check_err tests/extern/errors/owned.extc       'not implemented yet'
# 第 2 步（指针放宽）的三条边界：不签字的声明照旧挡住帧内指针（指针放宽之后这条**更重要**，
# 因为 `ref T` 还能让 C 从我们内存里读出一个指针留着 ⇒ Cont 那一半）· `void *` 看不进去 ·
# **按值**传结构体仍然不开（那是另一格：布局不一致是静默错）
check_err tests/extern/errors/ptr_unsigned.extc   'may be kept by C forever'
check_err tests/extern/errors/void_deref.extc     'void` has no size'
check_err tests/extern/errors/struct_by_value.extc 'cannot cross the C boundary'

exit $fail
