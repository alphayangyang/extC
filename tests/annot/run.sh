#!/usr/bin/env bash
# 注解的常设验收：`@inline` 要真的生效，错的注解要**编译期报错**。
#
# 判据分两类：
#   · 正例 —— 必须编过，而且生成的 C 里要**真的**有 EXTC_INLINE（"接受了但什么也没做"是最坏的一种）
#   · 反例 —— 必须被编译期挡掉，而且消息里要能认出问题（不是"生成的文件第几行"）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
pass=0; fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }

echo "== 正例：@inline 必须真的落到生成的 C 上 =="
for f in tests/annot/*.extc; do
    case "$(basename "$f")" in no_*) continue ;; esac
    n=$(basename "$f" .extc)
    if ! out=$($EXTC "$f" 2>&1); then bad "$n （应该编过）"; echo "$out" | head -3 | sed 's/^/        /'; continue; fi
    if echo "$out" | grep -q "EXTC_INLINE"; then ok "$n  ->  EXTC_INLINE 出现在生成的 C 里"
    else bad "$n （编过了，但生成代码里没有内联属性 ⇒ 静默失效）"; fi
done

echo "== 反例：错的注解必须编译期报错 =="
for f in tests/annot/no_*.extc; do
    n=$(basename "$f" .extc)
    want=$(grep -m1 '^// expect-error:' "$f" | sed 's|^// expect-error: *||')
    if out=$($EXTC "$f" -o /dev/null 2>&1); then bad "$n （应该报错但通过了）"; continue; fi
    if echo "$out" | grep -qF -- "$want"; then ok "$n  ->  $(echo "$out" | head -1 | sed 's/^[^ ]*: //' | cut -c1-58)"
    else bad "$n （报错了，但不是期望的那条：想要「$want」）"; echo "$out" | head -2 | sed 's/^/        /'; fi
done
echo "通过 $pass，失败 $fail"
[ $fail -eq 0 ]
