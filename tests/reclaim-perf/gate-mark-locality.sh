#!/usr/bin/env bash
# tests/reclaim-perf/gate-mark-locality.sh —— 标记遍历的**访存局部性**判据（批量弹出不许被拆）。
#
# 由来：标记栈是**链式**的（对象里存下一个对象的地址），弹出一个就要解引用它读指针字段 —— 这是
# 指针追逐，延迟被前一次装载卡住。现行 markStep 在栈深时一次先弹一批（EXTC_RC_MARK_BATCH_POP=200）
# 再统一处理：先攒一批、边攒边预取，处理时这些对象的头已经各自独立地在飞。这是**延迟**机制，不是
# 指令数机制 —— 拆掉批量，指令数反而**变少**（GCC 把 markStep 里那一处 ownSlab 内联了），但墙钟
# 慢 4–6 倍（bench/gc/src/spectrum_p5.extc，N=20 000 000：批量 0.53–0.86 s vs 逐个 3.29–3.55 s）。
# 所以判据不能是指令数，必须是 cache-sim 的**末级读失手（DLmr）**：拆掉批量时 P5 的 DLmr 涨到
# 586 445、D1mr 涨到 1 536 364，而指令数只 +2.3%。
#
# 现行实现有两条路：栈深够就装批（上面那套），待扫对象少到 EXTC_RC_MARK_SHALLOW（默认 8）以内
# 就单个弹出、直接扫 —— 批装机制在栈深≈1 时是纯固定开销（攒批的数组/上限/填充循环/扫批循环），
# 提前量也换不来任何东西。浅栈那条路**不许**把批量整体拆掉（深栈形状如 P5 必须还走批）。
#
# 判据（两条，都是 callgrind --cache-sim=yes 的确定量）：
#   ① DLmr ≤ LIM_DLMR（默认 190 000；现行实现 146 460，拆批量后 586 445）
#   ② D1mr ≤ LIM_D1MR（默认 700 000；现行实现 628 541，拆批量后 1 536 364）
# 只钉「局部性」这一件事：指令数/时间由 tests/reclaim-perf/gate.sh 与真实墙钟负责。
#
# --selftest：编译时把浅栈阈值调到无穷大（-DEXTC_RC_MARK_SHALLOW=4000000000u）⇒ 永远走单个处理，
#   等价于拆掉批量；要求两条判据**变红**（有牙），且真实运行期那两条为绿（先绿后红都验到）。
#
# 缺 valgrind：打印一行 跳过：… 后退出 0（check.py 记 skip；CI 装了 valgrind ⇒ 不许跳过）。
#
# 用法：tests/reclaim-perf/gate-mark-locality.sh [--selftest]
#   N=500000      P5 的轮数（判据线只对默认 N 成立）
#   SEED=...      P5 的随机种子（固定输入 ⇒ 确定量）
#   EXTC=...      指定编译器（默认 <repo>/build/extc）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
SRC=${SRC:-$ROOT/bench/gc/src/spectrum_p5.extc}
N=${N:-500000}
SEED=${SEED:-20261013}
LIM_DLMR=${LIM_DLMR:-190000}
LIM_D1MR=${LIM_D1MR:-700000}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }
VG=$(command -v valgrind || true)

echo "== 标记遍历的访存局部性判据（P5，N=$N，种子 $SEED）=="
echo "  形状   $SRC"
echo "  判据   DLmr ≤ $LIM_DLMR、D1mr ≤ $LIM_D1MR（callgrind --cache-sim=yes；批量弹出是延迟机制，不许按指令数删）"

if [ -z "$VG" ]; then
    skip "没有 valgrind（本机缺 callgrind/cache-sim）⇒ 局部性判据这轮没验"
    exit 0
fi

# extc 按自身所在目录找 stdlib ⇒ 拷到别处要显式 -I。
cp "$EXTC" "$TMP/extc"; chmod +x "$TMP/extc"

emit() {   # $1 = 生成的 C 文件
    if ! EXTC_RECLAIM=1 "$TMP/extc" -w --no-line-map -I "$ROOT/stdlib" "$SRC" -o "$1" 2>"$TMP/gen.log"; then
        bad "生成失败"; sed 's/^/    /' "$TMP/gen.log"; return 1
    fi
}

build() {  # $1 = C 文件, $2 = 可执行文件；$3... = 额外编译参数
    local c=$1 exe=$2; shift 2
    if ! $CC -std=c11 -fwrapv -O1 -g -D_POSIX_C_SOURCE=200809L "$@" -o "$exe" "$c" 2>"$TMP/cc.log"; then
        bad "生成物编译失败（$c）"; sed 's/^/    /' "$TMP/cc.log"; return 1
    fi
}

measure() {  # $1 = 可执行文件；输出 "<Ir> <D1mr> <DLmr>"（按 events 行的列名取，不硬编码列号）
    local exe=$1 tag=$TMP/cg.$$
    if ! "$VG" --tool=callgrind --cache-sim=yes --callgrind-out-file="$tag" "$exe" "$N" "$SEED" \
            >"$TMP/run.log" 2>"$TMP/vg.log"; then
        bad "callgrind 跑失败"; tail -5 "$TMP/vg.log" | sed 's/^/    /'; return 1
    fi
    awk '/^events:/{ for (i = 2; i <= NF; i++) col[$i] = i }
         /^summary:/{ printf "%s %s %s\n", $(col["Ir"]), $(col["D1mr"]), $(col["DLmr"]) }' "$tag"
    rm -f "$tag"
    return 0
}

# ---- 真实运行期：先绿 --------------------------------------------------------------
if ! emit "$TMP/p5.c"; then exit 1; fi
if ! build "$TMP/p5.c" "$TMP/p5"; then exit 1; fi
out=$(measure "$TMP/p5") || exit 1
read -r IR D1MR DLMR <<< "$out"
if [ -z "${IR:-}" ] || [ -z "${D1MR:-}" ] || [ -z "${DLMR:-}" ]; then
    bad "callgrind 输出缺 summary/events（拿不到 Ir/D1mr/DLmr）"; exit 1
fi
awk -v ir="$IR" -v d1="$D1MR" -v dl="$DLMR" -v l1="$LIM_D1MR" -v ll="$LIM_DLMR" 'BEGIN {
    printf "  Ir %d · D1mr %d（线 ≤ %d）· DLmr %d（线 ≤ %d）\n", ir, d1, l1, dl, ll
    exit !((d1 <= l1) && (dl <= ll))
}'
rc=$?
if [ $rc -eq 0 ]; then
    ok "标记遍历的访存局部性成立：批量弹出把指针追逐的末级读失手摊薄（DLmr/D1mr 在线上）"
else
    bad "标记遍历的局部性退化（批量弹出被拆/预取距离变 1？）：P5 的末级读失手超线"
    exit 1
fi

# ---- --selftest：浅栈阈值调到无穷大 = 永远逐个处理，判据必须变红 ---------------------
if [ "${1:-}" = "--selftest" ]; then
    echo "== 红自测：把浅栈阈值调到无穷大（永远逐个处理，等价拆掉批量）=="
    if ! build "$TMP/p5.c" "$TMP/p5.nb" -DEXTC_RC_MARK_SHALLOW=4000000000u; then exit 1; fi
    out=$(measure "$TMP/p5.nb") || exit 1
    read -r NIR ND1 NDL <<< "$out"
    awk -v ir="$NIR" -v d1="$ND1" -v dl="$NDL" -v b1="$D1MR" -v bl="$DLMR" -v l1="$LIM_D1MR" -v ll="$LIM_DLMR" 'BEGIN {
        printf "  拆批量：Ir %d · D1mr %d（%.2f×）· DLmr %d（%.2f×）\n", ir, d1, d1 / b1, dl, dl / bl
        exit !((d1 > l1) && (dl > ll))
    }'
    rc=$?
    if [ $rc -eq 0 ]; then
        ok "红自测：拆掉批量后 DLmr/D1mr 都超线（判据有牙）"
    else
        bad "红自测失败：拆掉批量后判据仍为绿（cache-sim 判据量不到局部性退化）"
        exit 1
    fi
fi

exit 0
