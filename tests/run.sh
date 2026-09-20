#!/usr/bin/env bash
# extC week-0 回归测试：例子能跑通，坏代码能被编译期挡掉。
set -u
cd "$(dirname "$0")/.."

EXTC=./build/extc
pass=0
fail=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail + 1)); }

echo "== 构建 =="
if make -s 2>/tmp/extc-build.log; then ok "make"; else bad "make"; cat /tmp/extc-build.log; exit 1; fi

echo "== 正例：extC -> C -> gcc -> 运行 =="
for f in examples/*.extc; do
    name=$(basename "$f" .extc)
    if out=$($EXTC --run "$f" 2>&1); then
        ok "$name  ->  $(echo "$out" | tr '\n' '|')"
    else
        bad "$name"; echo "$out" | sed 's/^/        /'
    fi
done

echo "== 反例：必须被编译期挡掉 =="
if [ -d tests/errors ]; then
    for f in tests/errors/*.extc; do
        name=$(basename "$f" .extc)
        if out=$($EXTC "$f" 2>&1); then
            bad "$name （应该报错但通过了）"
        else
            msg=$(echo "$out" | head -1 | sed 's/^[^ ]*: //')
            ok "$name  ->  $msg"
        fi
    done
fi

echo "== 反例·运行时：必须 trap（带源码位置）=="
if [ -d tests/traps ]; then
    for f in tests/traps/*.extc; do
        name=$(basename "$f" .extc)
        out=$($EXTC --run "$f" 2>&1)
        status=$?
        if [ $status -eq 0 ]; then
            bad "$name （应该 trap，却正常退出了）"
        elif echo "$out" | grep -q "trap:"; then
            ok "$name  ->  $(echo "$out" | grep -o 'trap:.*' | head -1)"
        else
            bad "$name （退出了，但没有 trap 消息）"
            echo "$out" | sed 's/^/        /'
        fi
    done
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
