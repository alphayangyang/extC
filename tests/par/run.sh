#!/usr/bin/env bash
# `par`：数据并行的唯一动词（见 docs/PARALLEL.md）。
#
# 判据：
#   ① 等价：`par` 的结果与顺序 for 逐位相同（stdout 逐字节）；
#   ② 线程数只影响速度，不影响结果（`EXTC_PAR_THREADS=1` 与默认值输出一致）；
#   ③ 拒绝：限制集每条一个用例，必须编译失败且文案稳定；
#   ④ 空区间不 trap；`par` 仍是形状识别的软关键字（可以当变量名）。
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
pass=0; fail=0
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1"; fail=$((fail+1)); }

# build <name> <source>  -> 编 C + 编可执行；失败时打印前两行并返回 1
build() {
    local name=$1 src=$2
    if ! "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$src" >"$tmp/$name.err" 2>&1; then
        bad "$name -> 编译失败：$(head -2 "$tmp/$name.err" | tr '\n' ' ')"
        return 1
    fi
    if ! "$CC" -std=c11 -fwrapv -O2 -pthread -o "$tmp/$name" "$tmp/$name.c" 2>"$tmp/$name.cerr"; then
        bad "$name -> C 编译失败：$(head -2 "$tmp/$name.cerr" | tr '\n' ' ')"
        return 1
    fi
    return 0
}

# case_run <name> <source> <expected stdout> [env assignment...]
case_run() {
    local name=$1 src=$2 want=$3; shift 3
    build "$name" "$src" || return
    local got
    got=$(env "$@" "$tmp/$name" 2>"$tmp/$name.runerr"); local rc=$?
    if [ "$rc" = 0 ] && [ "$got" = "$want" ]; then
        ok "$name -> $got"
    else
        bad "$name -> 退出码 $rc，stdout [$got]，stderr [$(head -1 "$tmp/$name.runerr")]"
    fi
}

# case_neg <name> <source> <expected stderr fragment>
case_neg() {
    local name=$1 src=$2 want=$3
    if "$EXTC" -w --no-line-map -o "$tmp/$name.c" "$src" >"$tmp/$name.err" 2>&1; then
        bad "$name -> 应该编译失败，却通过了"
        return
    fi
    if grep -qF -- "$want" "$tmp/$name.err"; then
        ok "$name -> $want"
    else
        bad "$name -> 文案不符：$(head -2 "$tmp/$name.err" | tr '\n' ' ')"
    fi
}

# ---- ① 等价 + ② 线程数不影响结果 ----
case_run par_for       tests/par/par_for.extc     "bsum=2997000"
case_run par_for_t1    tests/par/par_for.extc     "bsum=2997000" EXTC_PAR_THREADS=1
case_run par_for_t4    tests/par/par_for.extc     "bsum=2997000" EXTC_PAR_THREADS=4
case_run par_helper    tests/par/par_helper.extc  "sum=999000"
case_run par_captures  tests/par/par_captures.extc "sum=249250500"
case_run par_captures_t1 tests/par/par_captures.extc "sum=249250500" EXTC_PAR_THREADS=1

# 确定性：同一用例跑两次，stdout 必须逐字节相同
if build par_det tests/par/par_captures.extc; then
    one=$(EXTC_PAR_THREADS=1 "$tmp/par_det"); many=$(EXTC_PAR_THREADS=8 "$tmp/par_det")
    if [ "$one" = "$many" ]; then ok "par_det -> 1 线程与 8 线程同输出：$one"
    else bad "par_det -> 线程数改变了输出：[$one] vs [$many]"; fi
fi

# ---- ④ 空区间 / 软关键字 ----
case_run par_empty  tests/par/par_empty.extc    "b0=7"
case_run par_softkey tests/par/par_softkey.extc "par=3"

# ---- ⑤ 标量捕获 / 二维折叠 / 归约路线 ----
case_run par_scalar   tests/par/par_scalar.extc "s=108"
case_run par_scalar_t1 tests/par/par_scalar.extc "s=108" EXTC_PAR_THREADS=1
case_run par_2d       tests/par/par_2d.extc     "same=true s=18090"
case_run par_2d_t1    tests/par/par_2d.extc     "same=true s=18090" EXTC_PAR_THREADS=1
case_run par_reduce   tests/par/par_reduce.extc "same=true s=16773120"
case_run par_reduce_t1 tests/par/par_reduce.extc "same=true s=16773120" EXTC_PAR_THREADS=1
if build par_2d_det tests/par/par_2d.extc; then
    one=$(EXTC_PAR_THREADS=1 "$tmp/par_2d_det"); many=$(EXTC_PAR_THREADS=8 "$tmp/par_2d_det")
    if [ "$one" = "$many" ]; then ok "par_2d_det -> 1 线程与 8 线程同输出：$one"
    else bad "par_2d_det -> 线程数改变了输出：[$one] vs [$many]"; fi
fi

# ---- ③ 限制集（每条一个必须编译失败的用例） ----
case_neg neg_call      tests/par/neg_call.extc      "cannot call a function inside a par loop"
case_neg neg_new       tests/par/neg_new.extc       "cannot allocate inside a par loop"
case_neg neg_two_views tests/par/neg_two_views.extc "may write only its own output slot"
case_neg neg_subscript tests/par/neg_subscript.extc "may write only its own output slot"
case_neg neg_alias     tests/par/neg_alias.extc     "aliases a read-only view"
case_neg neg_scalar    tests/par/neg_scalar.extc    "only read-only views and scalar values can be captured by a par loop"
case_neg neg_nest3     tests/par/neg_nest3.extc     "cannot nest \`par\` more than two levels"
case_neg neg_escape    tests/par/neg_escape.extc    "cannot escape a par loop"
case_neg neg_generic   tests/par/neg_generic.extc   "cannot use \`par\` inside a generic function"
case_neg neg_no_output tests/par/neg_no_output.extc "needs exactly one output view"

echo "par：通过 $pass，失败 $fail"
[ "$fail" = 0 ]
