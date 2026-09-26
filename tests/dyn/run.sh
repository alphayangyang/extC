#!/usr/bin/env bash
# dyn Trait 第二期 · 阶段 1 判据（DYN.md §5）
#   ① 正例：构造 + 立即调用，两个实现各一次
#   ② 存储被拒（阶段 1 只允许"构造即调用"）
#   ③ 字段访问被拒
#   ④ 生成物：派发**经表**（受控间接调用）、恰好 N 次、且过 -std=c11 合同编译零告警
set -u
EXTC=${EXTC:-./build/extc}
pass=0; fail=0
ok()   { echo "  ok   $1  ->  $2"; pass=$((pass+1)); }
bad()  { echo "  FAIL $1  ->  $2"; fail=$((fail+1)); }

out=$("$EXTC" -w --run tests/dyn/dyn_call.extc 2>&1)
[ "$out" = "dyn=7,5" ] && ok dyn_call "$out" || bad dyn_call "期望 dyn=7,5，实得：$out"

err=$("$EXTC" -w -o /dev/null tests/dyn/errors/dyn_store.extc 2>&1)
case "$err" in
    *"must be called immediately"*) ok dyn_store "拒绝存储（阶段 1 只允许构造即调用）" ;;
    *) bad dyn_store "$err" ;;
esac

err=$("$EXTC" -w -o /dev/null tests/dyn/errors/dyn_field.extc 2>&1)
case "$err" in
    *"must be followed by a method call"*) ok dyn_field "拒绝字段访问" ;;
    *) bad dyn_field "$err" ;;
esac

tmp=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$tmp/d.c" tests/dyn/dyn_call.extc >/dev/null 2>&1; then
    calls=$(grep -o 'extc_vt\$Tag\$[a-z0-9_$]*\.tag(' "$tmp/d.c" | wc -l | tr -d " ")
    [ "$calls" = 2 ] && ok dyn_table_calls "派发经表：$calls 处" \
                     || bad dyn_table_calls "期望 2 处经表派发，实得 $calls"
    if gcc -std=c11 -fwrapv -Wall -Werror -fsyntax-only "$tmp/d.c" 2>"$tmp/e"; then
        ok dyn_contract "生成物合同编译零告警（-std=c11 -fwrapv -Wall -Werror）"
    else
        bad dyn_contract "$(head -2 "$tmp/e" | tr '\n' ' ')"
    fi
else
    bad dyn_codegen "dyn_call.extc 编译失败"
fi
rm -rf "$tmp"

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ]
