#!/usr/bin/env bash
# tests/fs/run.sh —— **块拥有文件描述符（定案 78）的常设验收**
#
# 判据四条：
#   ① 正例：循环里就地 open、出块即关 ⇒ **fd 号恒定**（1000 轮拿到同一个号）✓
#   ② 判据有牙：**不 ownFd 的同一段程序必须失败**（否则"恒定"可能只是假绿 ✗）
#   ③ 反例：`ownFd(计算)` ⇒ **编译期**挡住（生成的 C 把实参写两次 ⇒ 会跑两次 ✗）
#   ④ ASan 干净：fd 节点住在块的 arena 里，释放顺序是"先关资源、再放内存" ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例：块拥有 ⇒ 循环里 fd 号恒定 =="
if out=$("$EXTC" --run tests/fs/fd-constant.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "fd 恒定 = "      || ok=0
    echo "$out" | grep -qF "出块即可复用"     || ok=0
    if [ "$ok" = 1 ]; then echo "  ok   fd-constant  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL fd-constant  ->  $(echo "$out" | tr '\n' '|')"; fail=1; fi
else
    echo "  FAIL fd-constant  ->  跑不起来"; echo "$out" | head -4 | sed 's/^/        /'; fail=1
fi

echo "== 判据有牙（canary）：不 ownFd 的同一段程序**必须**失败 =="
out=$("$EXTC" --run tests/fs/leak-canary.extc 2>&1); rc=$?
if [ "$rc" != 0 ] && echo "$out" | grep -qF "fd 漂了"; then
    echo "  ok   leak-canary  ->  $(echo "$out" | head -1 | tr -d '\n')  ✓ 判据会响"
else
    echo "  FAIL leak-canary  ->  泄漏时判据没响（rc=$rc）：$(echo "$out" | tr '\n' '|')"; fail=1
fi

echo '== 反例：ownFd(计算) 必须编译期挡住 =='
want=$(grep -m1 '^// expect-error:' tests/fs/errors/ownfd_not_repeatable.extc | sed 's|^// expect-error: *||')
if out=$("$EXTC" tests/fs/errors/ownfd_not_repeatable.extc -o /dev/null 2>&1); then
    echo "  FAIL ownfd-not-repeatable  ->  应该报错但通过了"; fail=1
elif [ -n "${want:-}" ] && ! echo "$out" | grep -qF "$want"; then
    echo "  FAIL ownfd-not-repeatable  ->  报错了但不是期望的那条：想要「$want」"; fail=1
else
    echo "  ok   ownfd-not-repeatable ->  $(echo "$out" | head -1 | sed 's/^[^ ]*: //' | cut -c1-58)"
fi

echo "== ASan：fd 节点的归属与释放顺序必须干净 =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/probe.c"
if gcc -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" >/dev/null 2>&1; then
    if "$EXTC" tests/fs/fd-constant.extc -o "$TMP/fd.c" >/dev/null 2>&1 \
       && gcc -O1 -g -fsanitize=address -o "$TMP/fd" "$TMP/fd.c" >/dev/null 2>&1; then
        if out=$("$TMP/fd" 2>&1); then
            echo "  ok   fd-constant  ->  ASan 干净（$(echo "$out" | tr '\n' '|')）"
        else
            echo "  FAIL fd-constant  ->  ASan 报错：$(echo "$out" | head -3 | tr '\n' ' ')"; fail=1
        fi
    else
        echo "  FAIL fd-constant  ->  ASan 构建失败"; fail=1
    fi
else
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
fi
rm -rf "$TMP"

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
