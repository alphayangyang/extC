#!/usr/bin/env bash
# bench/gc/v2/pause.sh - extC 侧停顿形状：逐形状跑一次 EXTC_RC_PAUSE=1，汇总 [step]/[pause]。
#
# 用法： bash bench/gc/v2/pause.sh [out.tsv]     默认 bench/gc/v2/pause.tsv
# 前置： bash bench/gc/v2/run.sh build （二进制在 $B；默认 B=/tmp/v2matrix/build）
set -u
V2=$(cd "$(dirname "$0")" && pwd)
B=${B:-/tmp/v2matrix/build}
OUT=${1:-$V2/pause.tsv}
PIN=${PIN:-6}

shape_args() {
  case $1 in
    promote)    echo '500000 20261013' ;;
    sizeratio)  echo '2000000 20261013' ;;
    chase)      echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
  esac
}

g() { printf '%s' "$1" | sed -n "s/$2/\1/p"; }

printf 'shape\tslices\tstep_total_us\tstep_mean_us\tstep_max_us\tover16\tover64\tover256\tover1ms\tcollects\tpause_total_us\n' > $OUT
for s in promote sizeratio chase cycleratio; do
  [ -x $B/$s.extc ] || { echo "缺少 $B/$s.extc（先跑 bash bench/gc/v2/run.sh build）" >&2; exit 1; }
  args=$(shape_args $s)
  err=$(env EXTC_RC_PAUSE=1 taskset -c $PIN $B/$s.extc $args 2>&1 >/dev/null)
  st=$(printf '%s\n' "$err" | grep '^\[step\]' | tail -1)
  pa=$(printf '%s\n' "$err" | grep '^\[pause\]' | tail -1)
  n=$(g "$st" '.* n=\([0-9]*\).*')
  tot=$(g "$st" '.* total_us=\([0-9.]*\).*')
  mx=$(g "$st" '.* max_us=\([0-9.]*\).*')
  o16=$(g "$st" '.* over16=\([0-9]*\).*')
  o64=$(g "$st" '.* over64=\([0-9]*\).*')
  o256=$(g "$st" '.* over256=\([0-9]*\).*')
  o1ms=$(g "$st" '.* over1ms=\([0-9]*\).*')
  cols=$(g "$pa" '.* collects=\([0-9]*\).*')
  ptot=$(g "$pa" '.* total_us=\([0-9.]*\).*')
  mean=$(awk -v a="$tot" -v b="$n" 'BEGIN{ if (b>0) printf "%.3f", a/b; else printf "0" }')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $s $n $tot $mean $mx $o16 $o64 $o256 $o1ms $cols $ptot >> $OUT
done
echo "停顿表：$OUT"
