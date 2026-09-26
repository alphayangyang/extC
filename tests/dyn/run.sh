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

# 用途向的例子：混存不同形状求总面积（与 Tag 同一套机制，名字更能说明用途）
out=$("$EXTC" -w --run tests/dyn/dyn_shape_area.extc 2>&1)
[ "$out" = "rect=12 circle=12 total=24" ] && ok dyn_shape_area "混存 rect/circle 求总面积（$out）" \
                                          || bad dyn_shape_area "期望 rect=12 circle=12 total=24，实得：$out"

# 阶段 3：dyn 进泛型容器 varArray<T>；以及"容器清空后 dyn 值仍有效"（保守语义，见夹具注释）
out=$("$EXTC" -w --run tests/dyn/dyn_in_varArray.extc 2>&1)
[ "$out" = "va=13" ] && ok dyn_in_varArray "dyn 进 varArray<T>（$out）" || bad dyn_in_varArray "期望 va=13，实得：$out"
out=$("$EXTC" -w --run tests/dyn/dyn_container_clear.extc 2>&1)
[ "$out" = "afterClear=9" ] && ok dyn_container_clear "容器清空后 dyn 值仍有效（保守：载荷在 dyn 池，回收随 place）" \
                            || bad dyn_container_clear "期望 afterClear=9，实得：$out"

# 阶段 3：dyn 进**字段**与**容器**（句柄是普通值，所以它们"本来就能用"）
out=$("$EXTC" -w --run tests/dyn/dyn_in_field.extc 2>&1)
[ "$out" = "field=5" ] && ok dyn_in_field "dyn 作结构体字段并派发（$out）" || bad dyn_in_field "期望 field=5，实得：$out"
out=$("$EXTC" -w --run tests/dyn/dyn_in_array.extc 2>&1)
[ "$out" = "arr=106" ] && ok dyn_in_array "dyn 进定长数组，两种实现混存（$out）" || bad dyn_in_array "期望 arr=106，实得：$out"

# 值逃出 place 的**正确**处置：能提升就提升（home-zone 提升），提升不了才在派发时 trap
out=$("$EXTC" -w --run tests/dyn/dyn_promoted_return.extc 2>&1)
[ "$out" = "escaped=88" ] && ok dyn_promoted_return "函数返回的 dyn 值被提升进调用者的 place（$out）" \
                          || bad dyn_promoted_return "期望 escaped=88，实得：$out"

# 阶段 3：值形式合法（判据翻面 —— 旧判据断言"保存被拒"，保存现在是语言的一部分）
out=$("$EXTC" -w --run tests/dyn/dyn_stored.extc 2>&1)
[ "$out" = "stored=0" ] && ok dyn_stored "值形式可存下并作参数传递" \
                        || bad dyn_stored "期望 stored=0，实得：$out"
d=$(mktemp -d)
if "$EXTC" -w --no-line-map -o "$d/v.c" tests/dyn/dyn_stored.extc >/dev/null 2>&1 \
   && grep -q '= extc_dyn_put(' "$d/v.c"; then
    ok dyn_stored_codegen "生成物把载荷拷进池（= extc_dyn_put(）"
else
    bad dyn_stored_codegen "生成物里没有 extc_dyn_put"
fi
rm -rf "$d"

# 槽回收（墓碑 + 惰性清扫 + 复用）：界必须是**同时存活数**，不是累计创建数。
# 2000 次调用后 live 应当是个小常数（容量翻倍的余量），而不是 2000。
out=$("$EXTC" -w --run tests/dyn/dyn_slot_reclaim.extc 2>&1)
live=$(printf '%s' "$out" | sed -n 's/.*live=\([0-9]*\).*/\1/p')
case "$out" in *sum=14000*) sumok=1 ;; *) sumok=0 ;; esac
if [ "$sumok" = 1 ] && [ -n "$live" ] && [ "$live" -le 32 ]; then
    ok dyn_slot_reclaim "槽回收生效：2000 次创建后 live=$live（界是同时存活数）"
else
    bad dyn_slot_reclaim "期望 sum=14000 且 live<=32，实得：$out"
fi

# 阶段 3 核心：**存储值**派发 —— 两个不同具体类型走同一张统一签名的表（thunk 的回报）
out=$("$EXTC" -w --run tests/dyn/dyn_stored_call.extc 2>&1)
[ "$out" = "calls=7,105" ] && ok dyn_stored_call "存储值派发：box 与 pt 经同一张表（$out）" \
                           || bad dyn_stored_call "期望 calls=7,105，实得：$out"

err=$("$EXTC" -w -o /dev/null tests/dyn/errors/dyn_wrong_trait.extc 2>&1)
case "$err" in
    *"does not implement \`Mark\`"*) ok dyn_implements "载荷未实现被点名的 trait ⇒ 直指根因（而非发一张不存在的表）" ;;
    *) bad dyn_implements "$err" ;;
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
# 运行期判据（O5 核心）：陈旧 dyn 值必须 **trap**，而不是派发到别的实现。
#
# 阶段 3 的语法（保存 dyn 值）还没落地，所以这条判据直接测**运行期**：把生成物的 main 改名
# （编译期 -Dmain=…），接上自己的 main，用运行期助手走一遍"存值 → 离开 place → 派发"。
# 判据不仅要求"退出码非 0 + 提示 stale"，还要求**方法体没有被执行**（stdout 里没有标记行）——
# 这正是"绝不调错实现"那一半。
if "$EXTC" -w --no-line-map -o "$tmp/r.c" tests/dyn/dyn_call.extc >/dev/null 2>&1; then
    cp "$tmp/r.c" "$tmp/rt.c"
    cat >> "$tmp/rt.c" <<'EOF'
#undef main
int main(void) {
    int64_t z = extc_pool_zoneEnter();
    box b; b.v = 7;
    ExtcDynHandle h = extc_dyn_put((const void *)&b, (int64_t)sizeof(box), &extc_vt$Tag$box);
    ExtcDynSlot *s = extc_dyn_slot(h, "runtime-judge", 1);
    printf("alive=%lld\n", (long long)*((int64_t *)s->addr));
    fflush(stdout);
    extc_pool_zoneLeaveTo(z);            /* the place that owned the pool is gone */
    ExtcDynSlot *s2 = extc_dyn_slot(h, "runtime-judge", 2);   /* must trap here */
    printf("NOT-REACHED %p\n", (void *)s2);
    return 0;
}
EOF
    if gcc -std=c11 -fwrapv -Wall -Werror -Dmain=extc_gen_main "$tmp/rt.c" -o "$tmp/rt" 2>"$tmp/cc"; then
        "$tmp/rt" >"$tmp/out" 2>"$tmp/err"; rc=$?
        if [ "$rc" -ne 0 ] && grep -q 'stale `dyn` value' "$tmp/err" && grep -q 'alive=7' "$tmp/out" \
           && ! grep -q 'NOT-REACHED' "$tmp/out"; then
            ok dyn_o5_stale_trap "陈旧值 trap（rc=$rc），且**方法体未被执行**（O5 核心）"
        else
            bad dyn_o5_stale_trap "rc=$rc out=[$(tr '\n' ' ' <"$tmp/out")] err=[$(head -1 "$tmp/err")]"
        fi
    else
        bad dyn_o5_stale_trap "单测编译失败：$(head -2 "$tmp/cc" | tr '\n' ' ')"
    fi
else
    bad dyn_o5_stale_trap "生成 C 失败"
fi

rm -rf "$tmp"

echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ]
