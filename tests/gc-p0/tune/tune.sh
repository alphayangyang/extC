#!/usr/bin/env bash
# Trigger-multiplier and slice-granularity probe on bench/gc/v2 shape promote.
# Variants are patched copies of the emitted C (same compiler build), so only one knob differs.
set -e
V2=${V2:-/tmp/v2}            # holds new/promote.extc[.c] built by bench/gc/v2/run.sh
CC_BIN=${CC_BIN:-cc}
CF='-O2 -std=c11 -D_POSIX_C_SOURCE=200809L'
mkdir -p $V2/tune/bin $V2/tune/res
$CC_BIN $CF $V2/new/promote.extc.c -o $V2/tune/bin/p_base
# thr = max(2*live, floor) -> 4*live and 8*live
sed 's/int64_t thr = r->live \* 2;/int64_t thr = r->live * 4;/' $V2/new/promote.extc.c > $V2/tune/m4.c
sed 's/int64_t thr = r->live \* 2;/int64_t thr = r->live * 8;/' $V2/new/promote.extc.c > $V2/tune/m8.c
$CC_BIN $CF $V2/tune/m4.c -o $V2/tune/bin/p_m4
$CC_BIN $CF $V2/tune/m8.c -o $V2/tune/bin/p_m8
for v in p_base p_m4 p_m8; do
  echo "-- $v"
  for i in 1 2 3 4 5; do EXTC_RC_M_MAX=33554432 $V2/v2run --pin 6 -- $V2/tune/bin/$v 500000 20261013; done
  EXTC_RC_M_MAX=33554432 EXTC_RC_PAUSE=1 taskset -c 6 $V2/tune/bin/$v 500000 20261013 >/dev/null 2>$V2/tune/res/$v.err
  grep -E '^\[(step|work|pause)\]' $V2/tune/res/$v.err
done
# slice granularity
$CC_BIN $CF -DEXTC_RC_SWEEP_BUDGET=64  $V2/new/promote.extc.c -o $V2/tune/bin/p_sb64
$CC_BIN $CF -DEXTC_RC_SWEEP_BUDGET=256 $V2/new/promote.extc.c -o $V2/tune/bin/p_sb256
for v in p_sb64 p_sb256; do
  EXTC_RC_PAUSE=1 taskset -c 6 $V2/tune/bin/$v 500000 20261013 >/dev/null 2>$V2/tune/res/$v.err
  echo "-- $v"; grep -E '^\[step\]' $V2/tune/res/$v.err
done
