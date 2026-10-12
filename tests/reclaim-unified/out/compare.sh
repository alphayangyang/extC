#!/usr/bin/env bash
# tests/reclaim-unified/out/compare.sh —— 复跑 RECON.md §C.3 的 base vs proto 表
# base = 仓库 HEAD 的 ./build/extc；proto = /tmp/rcproto 的原型
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
P=${PROTO:-/tmp/rcproto}
T=$P/cmp; mkdir -p "$T"
run() { # <tag> <extc> <shape> <N>
    local tag=$1 E=$2 s=$3 N=$4
    EXTC_RECLAIM=1 "$E" -w --no-line-map "$P/shapes/$s.extc" -o "$T/$tag-$s.c" 2>/dev/null \
        || { echo "$tag $s GENFAIL"; return; }
    cc -std=c11 -fwrapv -O1 -o "$T/$tag-$s" "$T/$tag-$s.c" 2>/dev/null \
        || { echo "$tag $s CCFAIL"; return; }
    python3 "$ROOT/tools/rc-pause-probe.py" "$T/$tag-$s.c" > "$T/$tag-${s}_p.c" 2>/dev/null
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$T/$tag-${s}_p" "$T/$tag-${s}_p.c" 2>/dev/null
    local out rc peak hash sw rs
    out=$(timeout 600 "$T/$tag-$s" ${N:+$N} 2>&1); rc=$?
    peak=$(/usr/bin/time -f %M timeout 600 "$T/$tag-$s" ${N:+$N} 2>&1 >/dev/null | tail -1)
    hash=$(printf '%s' "$out" | md5sum | cut -c1-8)
    sw=$(cd "$T" && timeout 600 "./$tag-${s}_p" ${N:+$N} 2>&1 | grep -o 'PROBE sweeps=[0-9]*' | head -1)
    rs=$(cd "$T" && EXTC_RC_STATS=1 timeout 600 "./$tag-${s}_p" ${N:+$N} 2>&1 \
        | grep -o 'rs_inc=[0-9]* rs_dec=[0-9]* rs_live=[0-9]* rs_hwm=[0-9]*')
    printf '%s\t%s\tN=%s\trc=%s\tpeakKB=%s\tout=%s\t%s\t%s\n' \
        "$tag" "$s" "${N:--}" "$rc" "$peak" "$hash" "$sw" "$rs"
}
# 清扫次数/rs_* 与现状一致的两档（原型与 base 都跑得完）
for s in cross_region scale_growth; do
    for n in 200000 800000; do
        run base  "$ROOT/build/extc" "$s" "$n"
        run proto "$P/build/extc"    "$s" "$n"
    done
done
# cyclic_pressure：原型只在很小规模上跑得完（RECON.md §C.4-1 的实测）
for n in 2000; do
    run base  "$ROOT/build/extc" cyclic_pressure "$n"
    run proto "$P/build/extc"    cyclic_pressure "$n"
done
