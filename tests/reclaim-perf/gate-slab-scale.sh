#!/usr/bin/env bash
# tests/reclaim-perf/gate-slab-scale.sh —— 分配路径的**缩放**判据（每分配 O(1)，不随存活 slab 数涨）。
#
# 由来：extc_rc_alloc 的兜底曾是「热缓存失手 ⇒ 从 r->slabs 头线性扫到第一个同尺寸且有空位的块」。
# 存活 slab 数 S 一多就是每分配 O(S)：实测 64 B×1 GiB 分配 35.7 s、16 B×32 M 分配 5.2 s（见
# docs/changelog/2026-10.md 的落地条目）。现行实现是「每尺寸类一条有空位链」（惰性弹出），
# 分配兜底 O(1)。本判据量的是**发射出来的那份运行期文本**的行为，不是另抄一份实现。
#
# 为什么是 callgrind 指令数而不是墙钟：指令数是**确定量**（同一二进制同一输入逐位相同），
# 不受机器负载/频率影响；而这条判据要的是「每分配的成本与 S 无关」这个结构性事实。
# 为了让旧实现的正比关系在**小规模**下就可见，探针用 -DEXTC_RC_SLAB_BITS=14 把 slab 压到
# 16 KiB（槽 512 B ⇒ 每块约 31 槽，N=50000 就有 ~1600 块）；旧扫描的每条目约 6 Ir，
# 每分配摊到 6·S/31 Ir ⇒ N 与 4N 两档之间「每分配指令数」会涨 ~3–4×，新实现恒定。
#
# 判据（两条）：
#   ① 每分配指令数：N 与 4N 两档之比 ≤ RATIO（默认 1.15；实测新实现 ~1.00，注入旧扫描 ~2.7）
#   ② 每分配指令数绝对值 ≤ ABS（默认 400；实测 ~198，防「比恒定但常数被放大」的回归）
#   另加一条健全性：4N 档的存活 slab 数必须是 N 档的 ~4×（探针确实在长 slab，不是空转）。
#
# --selftest：把发射文本里的「尺寸类有空位链」兜底整段换回旧的线性扫描形状，重新量一遍，
#   要求①**变红**（有牙），且真实运行时那两条判据为绿（先绿后红都验到）。
#
# 缺 valgrind：打印一行 跳过：… 后退出 0（check.py 记 skip；CI 装了 valgrind ⇒ 不许跳过）。
#
# 用法：tests/reclaim-perf/gate-slab-scale.sh [--selftest]
#   N=50000     小档分配数（4N 是大档；两条判据只对默认 N 成立）
#   RATIO=1.15  比判据线
#   ABS=400     每分配指令数绝对线
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
CC=${CC:-cc}
N=${N:-50000}
RATIO=${RATIO:-1.15}
ABS=${ABS:-400}
SLOT=512
BITS=14

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }

VG=$(command -v valgrind || true)

echo "== 分配路径缩放判据（-DEXTC_RC_SLAB_BITS=$BITS，槽 $SLOT B，N=$N / $((N * 4))）=="

if [ -z "$VG" ]; then
    skip "没有 valgrind（本机缺 callgrind）⇒ 缩放判据这轮没验"
    exit 0
fi

echo "== 发射运行期文本 =="
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I "$ROOT/src/base" -I "$ROOT/src/back" \
        "$ROOT/tests/reclaim/emit_main.c" "$ROOT/src/back/reclaim.c" "$ROOT/src/base/base.c" \
        -o "$TMP/emit" 2>"$TMP/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$TMP/emit.log"; exit 1
fi
if ! "$TMP/emit" > "$TMP/extc_reclaim_runtime.inc" 2>"$TMP/emit.err" \
        || [ ! -s "$TMP/extc_reclaim_runtime.inc" ]; then
    bad "发射运行期文本失败"; sed 's/^/    /' "$TMP/emit.err"; exit 1
fi
ok "发射运行期文本（$(wc -c < "$TMP/extc_reclaim_runtime.inc") 字节）"

build_bench() {   # $1 = 运行期文本文件, $2 = 可执行文件
    # 探针固定 include "extc_reclaim_runtime.inc" ⇒ 各档给各自的 include 目录，
    # 这样红自测注入的那份文本才真的被编进去（不能只是文件名不同）。
    local inc=$1 exe=$2 dir=$TMP/inc.$(basename "$2")
    mkdir -p "$dir" && cp "$inc" "$dir/extc_reclaim_runtime.inc" || return 1
    if ! $CC -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -fwrapv -I "$dir" \
            -DEXTC_RC_SLAB_BITS="$BITS" "$DIR/slab-scale.c" -o "$exe" 2>"$TMP/cc.log"; then
        bad "探针编译失败（$1）"; sed 's/^/    /' "$TMP/cc.log"; return 1
    fi
    return 0
}

measure() {   # $1 = 可执行文件, $2 = N, 输出 "<Ir> <slabs>"
    local exe=$1 n=$2 tag=$TMP/cg.$$
    if ! "$VG" --tool=callgrind --callgrind-out-file="$tag" "$exe" "$n" "$SLOT" \
            >"$TMP/run.log" 2>"$TMP/vg.log"; then
        bad "callgrind 跑失败（N=$n）"; tail -5 "$TMP/vg.log" | sed 's/^/    /'; return 1
    fi
    local ir slabs
    ir=$(grep '^summary:' "$tag" | awk '{print $2}')
    slabs=$(sed -n 's/^slabs=//p' "$TMP/run.log")
    if [ -z "${ir:-}" ] || [ -z "${slabs:-}" ]; then
        bad "callgrind/探针输出缺 summary 或 slabs（N=$n）"; return 1
    fi
    echo "$ir $slabs"
    rm -f "$tag"
    return 0
}

# ---- 真实运行期：先绿 --------------------------------------------------------------
if ! build_bench "$TMP/extc_reclaim_runtime.inc" "$TMP/probe"; then exit 1; fi
read -r IR1 SL1 <<< "$(measure "$TMP/probe" "$N")"          || exit 1
read -r IR4 SL4 <<< "$(measure "$TMP/probe" "$((N * 4))")"  || exit 1

awk -v ir1="$IR1" -v ir4="$IR4" -v n="$N" -v sl1="$SL1" -v sl4="$SL4" \
    -v ratio="$RATIO" -v abs="$ABS" '
BEGIN {
    p1 = ir1 / n; p4 = ir4 / (4 * n); r = p4 / p1; g = sl4 / sl1
    printf "  N=%-7d 每分配 %6.1f Ir（slab %d）\n", n, p1, sl1
    printf "  N=%-7d 每分配 %6.1f Ir（slab %d）\n", 4 * n, p4, sl4
    printf "  比    %.3f（线 ≤ %s）；slab 增长 %.2f×（应 ~4×）；绝对线 ≤ %s\n", r, ratio, g, abs
    exit !((r <= ratio) && (p4 <= abs) && (g >= 3.5) && (g <= 4.5))
}'
rc=$?
if [ $rc -eq 0 ]; then
    awk -v ratio="$RATIO" 'BEGIN { printf "  ok   分配路径缩放成立：每分配指令数与存活 slab 数无关（比 ≤ %s）\n", ratio }'
else
    bad "每分配成本随存活 slab 数上涨（回退扫描回来了），或探针没在长 slab"
    exit 1
fi

# ---- --selftest：把兜底换回旧线性扫描，判据必须变红 --------------------------------
if [ "${1:-}" = "--selftest" ]; then
    echo "== 红自测：注入旧的线性扫描兜底 =="
    if ! python3 - "$TMP/extc_reclaim_runtime.inc" "$TMP/oldscan.inc" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
block = re.search(r'        ExtcRcClass \*c = extc_rc_classSlot\(r, ss\);.*?        \}\n', t, re.S)
if not block:
    raise SystemExit("注入失败：找不到尺寸类兜底段落（发射文本形状变了？）")
old = ('        s = r->slabs;\n'
       '        while (s && (s->ss != ss ||\n'
       '                    (s->freelist < 0 && s->bump >= s->nslots))) s = s->next;\n')
open(dst, 'w').write(t[:block.start()] + old + t[block.end():])
PY
    then
        bad "红自测注入失败"; exit 1
    fi
    if ! build_bench "$TMP/oldscan.inc" "$TMP/probe_old"; then exit 1; fi
    read -r OIR1 OSL1 <<< "$(measure "$TMP/probe_old" "$N")"          || exit 1
    read -r OIR4 OSL4 <<< "$(measure "$TMP/probe_old" "$((N * 4))")"  || exit 1
    awk -v ir1="$OIR1" -v ir4="$OIR4" -v n="$N" -v ratio="$RATIO" '
    BEGIN {
        p1 = ir1 / n; p4 = ir4 / (4 * n); r = p4 / p1
        printf "  注入旧扫描：每分配 %6.1f Ir → %6.1f Ir（比 %.3f）\n", p1, p4, r
        exit !(r > ratio)
    }'
    rc=$?
    if [ $rc -eq 0 ]; then
        awk -v ratio="$RATIO" 'BEGIN { printf "  ok   红自测：注入旧线性扫描后比 > %s（判据有牙）\n", ratio }'
    else
        bad "红自测失败：注入旧线性扫描后判据仍为绿（判据量不到回退扫描）"
        exit 1
    fi
fi

exit 0
