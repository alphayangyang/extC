#!/usr/bin/env bash
# tests/reclaim-perf/gate-tax.sh —— **回收税**判据：不需要回收的形状，回收开销应当趋近 0。
#
# 目标口径（用户目标）：像 `bt` 这种**根本不产生垃圾/环**的形状（整棵树的死期就是所在区域的
# 死期，帧内不需要"提前回收"换内存），回收档与关闭档的差距应当**趋近 0**。
#
# 度量：同一个程序源，`EXTC_RECLAIM=1` 与 `=0` 各编一份（`cc -O3 -march=native -fwrapv`），
# 小输入下取 callgrind 总指令数，算
#        税 = 开档 Ir / 关档 Ir      （1.00× = 完全不花钱）
# 与 占比 = 1 − 关档/开档。
#
# 判据：`bt` 的税 ≤ `BT_TAX_LIMIT`（棘轮：**只许往下收**）。现在远没到 1.0×，
# 这条线的意义是"每次收窄落地就把它拧下来"，而不是现在就绿。
# `rebuild` / `cyclic_pressure` 只**打印**不判：前者是分配密集（提前回收换内存是真需求），
# 后者是特意造环（回收本身就是它的目的）—— 它们的高占比不违反目标。
#
# 用法：tests/reclaim-perf/gate-tax.sh
#   EXTC=… / CC=…         换编译器
#   BT_TAX_LIMIT=…        覆盖 bt 的税上限（自检"闸门有牙"用）
#
# 缺 valgrind：本脚本的判据整条没验 ⇒ 打印 `  --   跳过：…`、退出码 0（check.py 记 skip、不计数）。
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
BIG=$ROOT/bench/bigmatrix
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
# 现行线：bt **1.000×**（「区域不需要回收 ⇒ 整段不发射 rc」落地后，开/关档产物逐字节相同）× 1.05。
# 这条线的意义：判据一旦退化（该省的没省）比值立刻回到 1.1× 以上 ⇒ 当场红。
BT_TAX_LIMIT=${BT_TAX_LIMIT:-1050}      # 千分之一为单位：1050 = 1.050×
# 「遍历占主导」口径：同一形状、更大的输入（`12 14 200`）。用户目标（不需要回收 ⇒ 占比接近 0）
# 要在这个口径上判，因为 in-small 是**分配占主导**的（残余主要是每对象的结构开销，不是回收占比）。
BT_MID_TAX_LIMIT=${BT_MID_TAX_LIMIT:-1050}   # 实测 1.000×（同一档产物）× 1.05

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== 回收税判据（不需要回收的形状应当趋近 0；棘轮只许往下）=="

# 判据**全部**建在 callgrind 上 ⇒ 缺 valgrind 时明说"没验"并退 0（不许静默假装判过）：
# check.py 认这一行 ⇒ 记 `skip`、不计数（CI 装了 valgrind ⇒ 不许跳过）。
if ! command -v valgrind >/dev/null 2>&1; then
    printf '  --   跳过：没有 valgrind ⇒ 回收税判据（bt / bt-mid 两条棘轮）这轮没验\n'
    exit 0
fi

[ -f "$BIG/in-small/bt.txt" ] || {
    ( cd "$BIG" && cc -O2 -o build/gen gen.c && ./build/gen small in-small ) >/dev/null 2>&1 \
        || { bad "小输入不在且生成失败（bench/bigmatrix/gen.c）"; exit 1; }
}

ir_of() { # <标签> <源> <是否开档> <输入或 -> <程序参数…> → 打印 Ir
    local tag=$1 src=$2 on=$3 inp=$4; shift 4
    local rc=0; [ "$on" = 1 ] && rc=1
    EXTC_RECLAIM=$rc "$EXTC" -I "$ROOT/stdlib" "$src" -o "$TMP/$tag.c" >/dev/null 2>&1 \
        && $CC -O3 -march=native -fwrapv -o "$TMP/$tag" "$TMP/$tag.c" >/dev/null 2>&1 \
        || { echo ERR; return; }
    if [ "$inp" = "-" ]; then
        valgrind --tool=callgrind --callgrind-out-file="$TMP/$tag.cg" "$TMP/$tag" "$@" >/dev/null 2>&1
    else
        valgrind --tool=callgrind --callgrind-out-file="$TMP/$tag.cg" "$TMP/$tag" "$@" < "$inp" >/dev/null 2>&1
    fi || { echo ERR; return; }
    grep '^summary:' "$TMP/$tag.cg" | awk '{print $2}'
}

show() { # <形状> <源> <输入或 -> <是否判> [线] [程序参数…]
    local name=$1 src=$2 inp=$3 judge=$4 lim=${5:-}; shift 4; [ $# -gt 0 ] && shift 1
    local on off
    on=$(ir_of "$name.on" "$src" 1 "$inp" "$@"); off=$(ir_of "$name.off" "$src" 0 "$inp" "$@")
    if [ "$on" = ERR ] || [ "$off" = ERR ]; then bad "$name：生成/编译/callgrind 失败"; return 1; fi
    local per_mille pct
    per_mille=$(awk -v a="$on" -v b="$off" 'BEGIN{printf "%d", 1000.0*a/b}')
    pct=$(awk -v a="$on" -v b="$off" 'BEGIN{printf "%.1f", 100.0*(a-b)/a}')
    printf '  %-16s 税 %s.%03d×   回收占比 %s%%   （开档 %s / 关档 %s Ir）\n' \
           "$name" "$((per_mille/1000))" "$((per_mille%1000))" "$pct" "$on" "$off"
    if [ "$judge" = 1 ]; then
        if [ "$per_mille" -le "$lim" ]; then
            ok "$name：税 ≤ $(printf '%d.%03d' $((lim/1000)) $((lim%1000)))×（线只许往下拧）"
        else
            bad "$name：税 $(printf '%d.%03d' $((per_mille/1000)) $((per_mille%1000)))× > 上限 $(printf '%d.%03d' $((lim/1000)) $((lim%1000)))×"
            return 1
        fi
    fi
    return 0
}

fail=0
show bt "$BIG/src/bt.extc" "$BIG/in-small/bt.txt" 1 "$BT_TAX_LIMIT" || fail=1
# 遍历占主导的口径：输入恰好是 `深度 深度 轮数`，就地写一个中等规模（比 in-small 大两个量级的遍历/分配比）。
printf '12 14 200\n' >"$TMP/bt-mid.txt"
show bt-mid "$BIG/src/bt.extc" "$TMP/bt-mid.txt" 1 "$BT_MID_TAX_LIMIT" || fail=1
if [ "${TAX_EXTRA:-0}" = 1 ]; then
    # 只打印、不判：rebuild 靠提前回收换内存、cyclic_pressure 回收就是目的，高占比不违反目标。
    show rebuild "$BIG/src/rebuild.extc" "$BIG/in-small/rebuild.txt" 0 || fail=1
    show cyclic_pressure "$ROOT/tests/reclaim-shapes/cyclic_pressure.extc" - 0 "" 200000 || fail=1
fi
echo "  （被判的只有 bt 这类「不需要回收」的形状；--extra 用 TAX_EXTRA=1 看另外两个的对照值）"
exit "$fail"
