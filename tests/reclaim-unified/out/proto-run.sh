#!/usr/bin/env bash
# tests/reclaim-unified/out/proto-run.sh —— 复跑 RECON.md §C.1 的原型判据（不改仓库）
#
# 前置：/tmp/rcproto = 仓库 src/ + stdlib/ + Makefile 的最小副本，按 out/proto-diff/ 打好补丁、
#       `make -j8` 出 /tmp/rcproto/build/extc，形状拷到 /tmp/rcproto/shapes/。
# 用法：bash tests/reclaim-unified/out/proto-run.sh [形状…]
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
P=${PROTO:-/tmp/rcproto}
EXTC=$P/build/extc
[ -x "$EXTC" ] || { echo "缺少原型编译器 $EXTC（见 RECON.md §C.0）"; exit 2; }
S=${*:-"hole_c caller_frame persist_nodes owed_home cross_region cyclic_pressure"}
mkdir -p "$P/out"
for s in $S; do
    EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$P/shapes/$s.extc" -o "$P/out/$s.c" 2>"$P/out/$s.gen" \
        || { echo "$s GENFAIL"; head -3 "$P/out/$s.gen"; continue; }
    cc -std=c11 -fwrapv -O1 -o "$P/out/$s" "$P/out/$s.c" 2>"$P/out/$s.cc" \
        || { echo "$s CCFAIL"; head -3 "$P/out/$s.cc"; continue; }
    out=$(timeout 300 "$P/out/$s" 2>&1); rc=$?
    printf '%-16s rc=%-3s out=%s\n' "$s" "$rc" "$(printf '%s' "$out" | head -c 90 | tr '\n' '|')"
    # 清扫次数与 rs_*（插桩；只对会产生清扫的形状有意义）
    python3 "$ROOT/tools/rc-pause-probe.py" "$P/out/$s.c" > "$P/out/${s}_p.c" 2>/dev/null
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$P/out/${s}_p" "$P/out/${s}_p.c" 2>/dev/null
    (cd "$P/out" && EXTC_RC_STATS=1 timeout 300 "./${s}_p" 2>&1 >/dev/null \
        | grep -oE 'PROBE sweeps=[0-9]+|rs_(inc|dec|live|hwm)=[0-9]+' | tr '\n' ' ') | sed 's/^/                 /'
    echo
done
