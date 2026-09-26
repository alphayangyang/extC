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

err=$("$EXTC" -w -o /dev/null tests/dyn/errors/dyn_self_return.extc 2>&1)
case "$err" in
    *"returns \`Self\` and cannot be dispatched"*) ok dyn_object_safety "object safety ③：返回 Self 不可经 dyn 派发" ;;
    *) bad dyn_object_safety "$err" ;;
esac

err=$("$EXTC" -w -o /dev/null tests/dyn/errors/dyn_field.extc 2>&1)
case "$err" in
    *"must be followed by a method call"*) ok dyn_field "拒绝字段访问" ;;
    *) bad dyn_field "$err" ;;
esac

tmp=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$tmp/d.c" tests/dyn/dyn_call.extc >/dev/null 2>&1; then
    # 阶段 2：载荷进池、值经槽派发。三条一起判：
    #   ① 建池必须是**对象表模式**（extc_pool_new_table）——挡住"忘了用对象表"这类事故；
    #   ② 构造走 extc_dyn_put、校验走 extc_dyn_slot；
    #   ③ 派发形状必须是 `((const struct extc_vt$T$T_t *)s->vt)->m(s->addr …)`：
    #      表和接收者都来自**已校验的槽**（这正是 O5 的结构性保证）。
    puts=$(grep -o '= extc_dyn_put(' "$tmp/d.c" | wc -l | tr -d " ")
    slots=$(grep -o '= extc_dyn_slot(' "$tmp/d.c" | wc -l | tr -d " ")
    tbl=$(grep -c 'extc_pool_new_table' "$tmp/d.c" || true)
    disp=$(grep -o '\->vt)->tag(' "$tmp/d.c" | wc -l | tr -d " ")
    addr=$(grep -o 'tag(__extc_ds[0-9]*->addr' "$tmp/d.c" | wc -l | tr -d " ")
    if [ "$puts" = 2 ] && [ "$slots" = 2 ] && [ "$disp" = 2 ] && [ "$addr" = 2 ]; then
        ok dyn_pool_dispatch "进池 + 经槽派发：put=$puts slot=$slots dispatch=$disp（接收者取自槽）"
    else
        bad dyn_pool_dispatch "put=$puts slot=$slots dispatch=$disp addr=$addr（期望各 2）"
    fi
    if [ "$tbl" -ge 1 ]; then
        ok dyn_object_table "建池用对象表模式（extc_pool_new_table ×$tbl）"
    else
        bad dyn_object_table "生成物里没有 extc_pool_new_table"
    fi
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
