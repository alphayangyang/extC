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
# ⭐ 输出断言：例子文件里写 `// expect: <片段>` 的行 ⇒ 输出**必须包含**它 ✓
# （以前只查退出码 ⇒ "算出来是错的数据但正常退出"这种**抓不到** ✗ ——
#   PLAN #31 那个 UB 就是这种形状：打印 [1002, 0] 而不是 [42, 43]，退出码还是 0）
for f in examples/*.extc; do
    name=$(basename "$f" .extc)
    if out=$($EXTC --run "$f" 2>&1); then
        if ! grep -q '// expect:' "$f"; then
            ok "$name  ->  $(echo "$out" | tr '\n' '|')"
        elif want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1) \
             && echo "$out" | grep -qF -- "$want"; then
            ok "$name  ->  含「$want」（$(echo "$out" | wc -l) 行输出）"
        else
            bad "$name （输出里没有「$want」）"
            echo "$out" | sed 's/^/        /'
        fi
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
            # ⭐ 顺便咬住"**带源码位置**"这条（PLAN #6 那种退化就再也回不来了 ✓）
            if echo "$out" | grep -qE "extc|\.extc:[0-9]+: trap:"; then
                ok "$name  ->  $(echo "$out" | grep -o 'trap:.*' | head -1)"
            else
                bad "$name （trap 消息没有源码位置）"
                echo "$out" | sed 's/^/        /'
            fi
        else
            bad "$name （退出了，但没有 trap 消息）"
            echo "$out" | sed 's/^/        /'
        fi
    done
fi

echo "== arena：按块细化（150MB 上限下不许涨）=="
if [ -x tests/arena/run.sh ]; then
    if out=$(tests/arena/run.sh 2>&1); then
        echo "$out" | grep -E "ok |FAIL" | while read -r line; do echo "$line"; done
        pass=$((pass + 1))
    else
        echo "$out"
        bad "arena 用例"
    fi
fi

echo "== 警告：该响的响、正例语料上**零误报**、\`-w\` 能关 =="
if [ -x tests/warnings/run.sh ]; then
    if out=$(tests/warnings/run.sh 2>&1); then
        echo "$out" | grep -E "ok |FAIL" | while read -r line; do echo "$line"; done
        pass=$((pass + 1))
    else
        echo "$out"
        bad "警告用例"
    fi
fi

echo "== ASan：内存安全形状必须**真的**跑得干净（不是"编过了"就算）=="
if [ -x tests/asan/run.sh ]; then
    if out=$(tests/asan/run.sh 2>&1); then
        echo "$out" | grep -E "ok |FAIL|skip" | while read -r line; do echo "$line"; done
        pass=$((pass + 1))
    else
        echo "$out"
        bad "ASan 用例"
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
