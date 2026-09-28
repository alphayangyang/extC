#!/usr/bin/env bash
# tests/cabi/run.sh —— **C-ABI 线**的常设验收（docs/topics/C-ABI.md §9）。
#
# 第 ① 块：`@export` —— extC 函数变成 C 符号（`extern!` 的另一面）。
#   判据：① C 侧**真的**用这些符号：一份产物既当普通 C 文件**链接**进手写宿主（host.c），
#         又单独编成 `.so` 用 `nm -D` 查（`dlsym` 那条路的前提）；
#        ② 按值返回 `@frozen` 结构体也走通（C 侧手写一份等价结构体）；
#        ③ 十种"没有那一个 C 符号"的形状各有一条反例，且都**编译期**报出来。
# 后面的块（`fn` 字段的 effects 签字 · `std::dl` · 表的分发）加在这一套里。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

echo "== ① 产物是给 C 用的（链接 + 共享对象符号）=="
if "$EXTC" -w --no-line-map -o "$tmp/plugin.c" tests/cabi/plugin.extc >/dev/null 2>&1; then
    # 三个导出都必须是**外部链接**（定义与原型两处都不能是 static）
    bad=0
    for fn in triple mid makePair; do
        grep -qE "^[a-z0-9_]+ $fn\(.*\);$" "$tmp/plugin.c"      || { echo "  FAIL 原型不是外部链接：$fn"; bad=1; }
        grep -qE "^[a-z0-9_]+ $fn\(.*\) \{$" "$tmp/plugin.c"     || { echo "  FAIL 定义不是外部链接：$fn"; bad=1; }
    done
    if [ "$bad" = 0 ]; then echo "  ok   export-linkage  ->  triple / mid / makePair 都是外部链接的 C 符号（模块前缀被去掉）"
    else fail=1; fi

    if gcc -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/host" tests/cabi/host.c "$tmp/plugin.c" 2>"$tmp/err"; then
        if out=$("$tmp/host"); then
            ok=1
            for p in "triple=42" "mid=7" "pair=(3,4)"; do
                echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }
            done
            if [ "$ok" = 1 ]; then echo "  ok   export-host  ->  C 宿主链接并调用：$(echo "$out" | tr '\n' '|')"
            else echo "  FAIL export-host  ->  输出对不上"; fail=1; fi
        else echo "  FAIL export-host  ->  跑不起来"; fail=1; fi
    else
        echo "  FAIL export-host  ->  C 宿主链接失败"; head -4 "$tmp/err" | sed 's/^/        /'; fail=1
    fi

    if gcc -std=c11 -fPIC -shared -o "$tmp/plugin.so" "$tmp/plugin.c" 2>"$tmp/err2"; then
        bad=0
        for fn in triple mid makePair; do
            nm -D "$tmp/plugin.so" | grep -qE " T $fn\$" || { echo "  FAIL .so 里没有导出符号 $fn"; bad=1; }
        done
        if [ "$bad" = 0 ]; then echo "  ok   export-so  ->  共享对象里 nm -D 看得见 T triple / T mid / T makePair（dlsym 的前提）"
        else fail=1; fi
    else
        echo "  FAIL export-so  ->  编共享对象失败"; head -3 "$tmp/err2" | sed 's/^/        /'; fail=1
    fi
else
    echo "  FAIL 生成产物失败（plugin.extc）"; fail=1
fi

echo "== ② 反例（十种"没有那一个 C 符号"的形状，都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err tests/cabi/errors/generic.extc       '`@export` on a generic function'
check_err tests/cabi/errors/method.extc        'goes on a free function, not on a method'
check_err tests/cabi/errors/extern_export.extc 'and `extern!` contradict each other'
check_err tests/cabi/errors/keyword.extc       'that name is a C keyword'
check_err tests/cabi/errors/slice_param.extc   'which C cannot pass'
check_err tests/cabi/errors/hidden_arena.extc  'takes a hidden arena parameter'
check_err tests/cabi/errors/twice.extc         '`@export` appears twice'
check_err tests/cabi/errors/on_struct.extc     '`@export` goes on a function'
check_err tests/cabi/errors/on_global.extc     '`@export` goes on a function'
check_err tests/cabi/errors/dup_main.extc      'is already taken by another exported function'

echo '== ② 表槽的签名（fn 字段上的 effects）=='
if out=$("$EXTC" --run tests/cabi/slots.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/cabi/slots.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS=' ' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }; done
    if [ "$ok" = 1 ]; then echo "  ok   slot-signed  ->  $(echo "$out" | tr '\n' '|')（帧内指针经签字的槽放行，且调用真的发生）"
    else echo "  FAIL slot-signed  ->  输出对不上"; fail=1; fi
else
    echo "  FAIL slot-signed  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -5; fail=1
fi

echo "== ② 反例（签名是承诺：没签字 · 拷出槽 · 写错地方，都得挡住）=="
check_err tests/cabi/errors/slot_unsigned.extc   'argument 1 of `copy` may be kept by C forever'
check_err tests/cabi/errors/slot_copied_out.extc 'may be kept by C forever'
check_err tests/cabi/errors/slot_on_nonfn.extc   'which is not a function pointer'
check_err tests/cabi/errors/slot_thread.extc     '`Thread=` on a field is not used yet'

echo "== ③ std::dl（真的 dlopen 一个 .so：dlsym + 显式转换 + 调用）=="
if gcc -std=c11 -fPIC -shared -o build/cabi-probe.so tests/cabi/dlprobe.c 2>"$tmp/err3"; then
    if out=$("$EXTC" --run tests/cabi/dlmain.extc 2>&1); then
        ok=1
        for p in "triple=42" "add=35" "closed=ok"; do
            echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }
        done
        if [ "$ok" = 1 ]; then echo "  ok   dl-runtime  ->  $(echo "$out" | tr '\n' '|')（dlopen/dlsym/显式转换/调用/close 全通）"
        else echo "  FAIL dl-runtime  ->  输出对不上"; fail=1; fi
    else
        echo "  FAIL dl-runtime  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -5; fail=1
    fi
else
    echo "  FAIL 编 .so 失败"; head -3 "$tmp/err3" | sed 's/^/        /'; fail=1
fi

echo "== ③ 反例（代码指针这条转换只能朝一个方向、且必须非空）=="
check_err tests/cabi/errors/conv_nullable.extc 'it may be null'
check_err tests/cabi/errors/conv_notptr.extc   'cannot convert `i64` to `fn(i64) -> i64`'
check_err tests/cabi/errors/conv_no_ret.extc   'needs its return type'

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
