#!/usr/bin/env bash
# tests/reclaim-perf/gate-pin-scale.sh —— 钉住表（宿主证不出来的槽）的**缩放**判据（每钉一个 O(1)，不随表长涨）。
#
# 由来：宿主证不出来的槽（栈上槽、视图/arena 里的槽）最初按「目标区域 noSweep」处理 ⇒ 那只区域
# 永久退出清扫（OOM 根因，见 docs/changelog/2026-10.md）。现行语义是把值**钉成根**：钉住表
# 记住 (值 → 所属区域)，标记开始按区域播种、区域死时清死指针。钉住表的第一版是「数组 + 线性扫」，
# 每次登记/撤销都扫全表 ⇒ 钉 n 个是 O(n²)：实测 5 万 0.26 s、20 万 5.23 s（4× 规模 20× 时间）。
# 现行实现是开放寻址哈希表（装载 ≤ 1/2、Knuth 回移删除）⇒ 每钉一个 O(1)。
#
# 为什么是 callgrind 指令数而不是墙钟：指令数是**确定量**（同一二进制同一输入逐位相同），
# 不受机器负载/频率影响；判据要的是「每钉一个的成本与表长无关」这个结构性事实。
#
# 判据（两条）：
#   ① 每钉一个指令数：N 与 4N 两档之比 ≤ RATIO（默认 1.15；实测 ~0.82，注入线性预扫 ~3.97）
#   ② 每钉一个指令数绝对值 ≤ ABS（默认 400；实测 ~285，防「比恒定但常数被放大」的回归）
#   另加一条健全性：4N 档的表项数必须是 N 档的 ~4×（探针确实在钉，不是空转）。
#
# --selftest：往 pinAdd 里注入一次**线性预扫**（语义等价、成本 O(表长)），重新量一遍，
#   要求①**变红**（有牙），且真实运行期那两条判据为绿（先绿后红都验到）。
#
# 缺 valgrind：打印一行 跳过：… 后退出 0（check.py 记 skip；CI 装了 valgrind ⇒ 不许跳过）。
#
# 用法：tests/reclaim-perf/gate-pin-scale.sh [--selftest]
#   N=4000      小档钉住数（4N 是大档；两条判据只对默认 N 成立）
#   RATIO=1.15  比判据线
#   ABS=400     每钉一个指令数绝对线
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
CC=${CC:-cc}
N=${N:-4000}
RATIO=${RATIO:-1.15}
ABS=${ABS:-400}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }

VG=$(command -v valgrind || true)

echo "== 钉住表缩放判据（N=$N / $((N * 4))）=="

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
            "$DIR/pin-scale.c" -o "$exe" 2>"$TMP/cc.log"; then
        bad "探针编译失败（$1）"; sed 's/^/    /' "$TMP/cc.log"; return 1
    fi
    return 0
}

measure() {   # $1 = 可执行文件, $2 = N, 输出 "<Ir> <pins>"
    local exe=$1 n=$2 tag=$TMP/cg.$$
    if ! "$VG" --tool=callgrind --callgrind-out-file="$tag" "$exe" "$n" \
            >"$TMP/run.log" 2>"$TMP/vg.log"; then
        bad "callgrind 跑失败（N=$n）"; tail -5 "$TMP/vg.log" | sed 's/^/    /'; return 1
    fi
    local ir pins
    ir=$(grep '^summary:' "$tag" | awk '{print $2}')
    pins=$(sed -n 's/^pins=\([0-9]*\).*/\1/p' "$TMP/run.log")
    if [ -z "${ir:-}" ] || [ -z "${pins:-}" ]; then
        bad "callgrind/探针输出缺 summary 或 pins（N=$n）"; return 1
    fi
    echo "$ir $pins"
    rm -f "$tag"
    return 0
}

# ---- 真实运行期：先绿 --------------------------------------------------------------
if ! build_bench "$TMP/extc_reclaim_runtime.inc" "$TMP/probe"; then exit 1; fi
read -r IR1 PI1 <<< "$(measure "$TMP/probe" "$N")"          || exit 1
read -r IR4 PI4 <<< "$(measure "$TMP/probe" "$((N * 4))")"  || exit 1

awk -v ir1="$IR1" -v ir4="$IR4" -v n="$N" -v p1="$PI1" -v p4="$PI4" \
    -v ratio="$RATIO" -v abs="$ABS" '
BEGIN {
    a = ir1 / n; b = ir4 / (4 * n); r = b / a; g = p4 / p1
    printf "  N=%-7d 每钉一个 %6.1f Ir（表项 %d）\n", n, a, p1
    printf "  N=%-7d 每钉一个 %6.1f Ir（表项 %d）\n", 4 * n, b, p4
    printf "  比    %.3f（线 ≤ %s）；表项增长 %.2f×（应 ~4×）；绝对线 ≤ %s\n", r, ratio, g, abs
    exit !((r <= ratio) && (b <= abs) && (g >= 3.5) && (g <= 4.5))
}'
rc=$?
if [ $rc -eq 0 ]; then
    awk -v ratio="$RATIO" 'BEGIN { printf "  ok   钉住表缩放成立：每钉一个指令数与表长无关（比 ≤ %s）\n", ratio }'
else
    bad "每钉一个的成本随表长上涨（线性扫回来了），或探针没在钉"
    exit 1
fi

# ---- --selftest：注入线性预扫，判据必须变红 ----------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    echo "== 红自测：注入线性预扫（成本 O(表长)、语义等价）=="
    if ! python3 - "$TMP/extc_reclaim_runtime.inc" "$TMP/linear.inc" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
anchor = ("    extc_rc_pinLockAcq();\n"
          "    if ((extc_rc_pinN + 1) * 2 > extc_rc_pinCap")
if t.count(anchor) != 1:
    raise SystemExit("注入失败：pinAdd 的锁后锚点不唯一（发射文本形状变了？）")
ins = ("    extc_rc_pinLockAcq();\n"
       "    for (size_t _q = 0; _q < extc_rc_pinCap; _q++) {   /* 注入：线性预扫 */\n"
       "        if (extc_rc_pins[_q].v && extc_rc_pins[_q].v == v) {\n"
       "            extc_rc_pins[_q].n++; extc_rc_pinUnlock(); return 1;\n"
       "        }\n"
       "    }\n"
       "    if ((extc_rc_pinN + 1) * 2 > extc_rc_pinCap")
open(dst, 'w').write(t.replace(anchor, ins))
PY
    then
        bad "红自测注入失败"; exit 1
    fi
    if ! build_bench "$TMP/linear.inc" "$TMP/probe_linear"; then exit 1; fi
    read -r LIR1 LPI1 <<< "$(measure "$TMP/probe_linear" "$N")"          || exit 1
    read -r LIR4 LPI4 <<< "$(measure "$TMP/probe_linear" "$((N * 4))")"  || exit 1
    awk -v ir1="$LIR1" -v ir4="$LIR4" -v n="$N" -v ratio="$RATIO" '
    BEGIN {
        a = ir1 / n; b = ir4 / (4 * n); r = b / a
        printf "  注入线性预扫：每钉一个 %6.1f Ir → %6.1f Ir（比 %.3f）\n", a, b, r
        exit !(r > ratio)
    }'
    rc=$?
    if [ $rc -eq 0 ]; then
        awk -v ratio="$RATIO" 'BEGIN { printf "  ok   红自测：注入线性预扫后比 > %s（判据有牙）\n", ratio }'
    else
        bad "红自测失败：注入线性预扫后判据仍为绿（判据量不到回退）"
        exit 1
    fi
fi

exit 0
