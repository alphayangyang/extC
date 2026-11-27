#!/usr/bin/env bash
# tests/features/run.sh —— 语言细节的四条判据（u64 字面量、数组 .len、位操作、导入 const 当长度）
#
# 判据：
#   ① `len`：定长数组的 `.len` 在一维 / 多维 / 结构体字段 / 循环界都给出编译期长度；写它被拒
#   ② `u64`：0xFFFF... / 2^63 字面量可用，f-string 插值（含 `:04` 说明符与流位置）打出无符号十进制
#   ③ `bits`：u64 的 popcount / ctz / clz（含 0 与最高位）
#   ④ `importconst`：另一个模块的 `const` 可以当定长数组长度（名单 / 通配 / 限定名三条路）；
#      同名歧义、负值、浮点、@private 四种错法各自报对
#   ⑤ views：返回本帧容器的视图被静态拒绝（直接返回 / 先绑再返回 / string 容器三种），
#      借参数、snapshot()、返回容器本身、字符串字面量、整数组赋值五种正确形状必须通过；
#      视图整体赋值（dst[..] = src[..]）从静默无操作改为编译错
#   ⑥ borrow：`ref T` / `mut ref T` 形参在调用点自动取借（var/let/元素/泛型/容器/const），
#      显式 `ref ...` 仍然可用；let 不能传 mut ref、临时值不能当借用实参；
#      借用诊断里不再出现 depth / arena 这类编译器内部词汇
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-14s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-14s %s\n' "$1" "$2"; }

out=$("$EXTC" -w --no-line-map --run tests/features/len.extc 2>"$tmp/e" | tr '\n' ' ')
want="len=4 inner=3 outer=2 field=3 sum=6 "
if [ "$out" = "$want" ]; then ok len "→ $out"; else bad len "期望 [$want] 实得 [$out] $(head -2 "$tmp/e")"; fi

if "$EXTC" -w --no-line-map -o "$tmp/lb.c" tests/features/len_bad.extc 2>"$tmp/e"; then
    bad len_assign "应当被拒（定长数组长度不可写），却编过了"
elif grep -q 'cannot assign `len` of a fixed array' "$tmp/e"; then
    ok len_assign "写定长数组的 .len 被拒"
else
    bad len_assign "报文不对：$(head -2 "$tmp/e")"
fi

out=$("$EXTC" -w --no-line-map --run tests/features/u64.extc 2>"$tmp/e" | tr '\n' ' ')
want="all=18446744073709551615 same=18446744073709551615 half=9223372036854775808 back=-9223372036854775808 pad=[18446744073709551615] plain=42 18446744073709551615 stream=18446744073709551615 "
if [ "$out" = "$want" ]; then ok u64 "字面量与 f-string → 逐字相同"
else bad u64 "期望 [$want] 实得 [$out] $(head -2 "$tmp/e")"; fi

out=$("$EXTC" -w --no-line-map --run tests/features/bits.extc 2>"$tmp/e" | tr '\n' ' ')
want="pop 0 1 1 3 ctz 64 0 63 1 clz 64 63 0 58 "
if [ "$out" = "$want" ]; then ok bits "→ $out"; else bad bits "期望 [$want] 实得 [$out] $(head -2 "$tmp/e")"; fi

if "$EXTC" --build -o "$tmp/ic" tests/features/importconst/main.extc >"$tmp/o" 2>"$tmp/e"; then
    out=$("$tmp/ic" | tr '\n' ' ')
    want="n=5 wide=4 qual=6 cell=7 "
    if [ "$out" = "$want" ]; then ok importconst "名单 / 通配 / 限定名三条路 → $out"
    else bad importconst "期望 [$want] 实得 [$out]"; fi
else
    bad importconst "编不过：$(head -3 "$tmp/e")"
fi

check_reject() {
    name=$1; dir=$2; needle=$3; note=$4
    if "$EXTC" -w --no-line-map -o "$tmp/$name.c" "tests/features/$dir/main.extc" 2>"$tmp/e"; then
        bad "$name" "应当被拒（$note），却编过了"
    elif grep -q "$needle" "$tmp/e"; then
        ok "$name" "$note"
    else
        bad "$name" "报文不对：$(head -2 "$tmp/e")"
    fi
}
check_reject amb err_amb 'declared by more than one imported module' '两个模块都有 N，裸名歧义'
check_reject neg err_neg 'array length must be positive, got -2' '负值 const 不能当长度'
check_reject float err_float 'not a `const` with an integer literal initializer' 'f64 const 不能当长度'
check_reject priv err_private 'not a `const` with an integer literal initializer' '@private const 不跨模块导出'

out=$("$EXTC" -w --no-line-map --run tests/features/views/main.extc 2>"$tmp/e" | tr '\n' ' ')
want="param=7 copy=7 hold=7 ret=7 lit=5 assign=1 4 "
if [ "$out" = "$want" ]; then ok views "借参数 / snapshot / 返回容器 / 字面量 / 整数组赋值 → $out"
else bad views "期望 [$want] 实得 [$out] $(head -2 "$tmp/e")"; fi

check_reject view_local views/err_local 'whose storage this frame releases when it ends' '返回局部容器的视图被拒并提 snapshot'
check_reject view_binding views/err_binding 'whose storage this frame releases when it ends' '先绑定再返回同一个悬垂视图也被拒'
check_reject view_string views/err_string 'view of .string::string.' 'string 容器的视图同样被拒'
check_reject view_assign views/err_assign 'cannot assign to a view as a whole' '视图整体赋值被拒并提 copyInto'

out=$("$EXTC" -w --no-line-map --run tests/features/borrow/main.extc 2>"$tmp/e" | tr '\n' ' ')
want="x=5 y=6 w=15 explicit=6 elem=3 elemw=12 gen=6 vec=1 constn=9 "
if [ "$out" = "$want" ]; then ok borrow "调用点自动取借（var/let/元素/泛型/容器/const）+ 显式 ref 仍通 → $out"
else bad borrow "期望 [$want] 实得 [$out] $(head -2 "$tmp/e")"; fi

check_reject borrow_mut_let borrow/err_mut_let 'a `mut ref T` is write access' 'let 传 mut ref 报错：写明要用 var'
check_reject borrow_temp borrow/err_temp 'a reference has to point at an existing place' '临时值不能当借用实参，提示先存进变量'

check_nodepth() {
    name=$1; path=$2
    if "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$path" 2>"$tmp/e"; then
        bad "$name" "应当被拒，却编过了"
    elif grep -qi 'depth\|arena' "$tmp/e"; then
        bad "$name" "诊断里还有 depth/arena 字样：$(grep -m1 -i 'depth\|arena' "$tmp/e")"
    else
        ok "$name" "拒绝信息是行为描述，不含 depth/arena"
    fi
}
check_nodepth nodepth_ret tests/reclaim-shapes/reject_deep_store.extc
check_nodepth nodepth_arg tests/reclaim-shapes/reject_deep_store_call.extc
check_nodepth nodepth_eff tests/zone/neg_loop_new_view.extx
printf 'features: 通过 %d 个，失败 %d 个\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
