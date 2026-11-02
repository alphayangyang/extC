#!/usr/bin/env bash
# bench/memstudy/run.sh - extC 回收机制消融卡具。
# 用法： bash bench/memstudy/run.sh build|align|bench|pause|scale
# 环境： EXTC=编译器 B=产物目录 PIN=核号 REPS=重复 TSV=输出 REPS_SCALE 等。
set -u
D=$(cd $(dirname $0) && pwd)
ROOT=$(cd $D/../.. && pwd)
B=${B:-$D/build}
EXTC=${EXTC:-$ROOT/build/extc}
STDLIB=${STDLIB:-$ROOT/stdlib}
CC=${CC:-cc}
PIN=${PIN:-6}
REPS=${REPS:-5}
TSV=${TSV:-$D/out/ablate.tsv}
PAUSEDIR=${PAUSEDIR:-$D/out/pause}
V2=$ROOT/bench/gc/v2/src
MODE=${1:-bench}
mkdir -p $B/out $PAUSEDIR

SHAPES=${SHAPES:-'promote sizeratio chase cycleratio condsave mutate'}
ARMS=${ARMS:-'off default count sweepfree forsweep shard4 shard16 shard64 budget2k slab256k localstore0'}

shape_src() {
  case $1 in
    promote|sizeratio|chase|cycleratio) echo $V2/$1.extc ;;
    *) echo $D/src/$1.extc ;;
  esac
}
bench_args() {
  case $1 in
    promote) echo '500000 20261013' ;;
    sizeratio) echo '2000000 20261013' ;;
    chase) echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
    condsave) echo '2000000 20261013' ;;
    mutate) echo '40000000 20261013' ;;
  esac
}
arm_env() {
  case $1 in
    off) echo 'EXTC_RECLAIM=0' ;;
    default) echo '' ;;
    count) echo 'EXTC_RC_NOCOUNT=0' ;;
    sweepfree) echo 'EXTC_RC_SWEEPFREE=1' ;;
    forsweep) echo 'EXTC_RC_SWEEPFREE=0' ;;
    shard4) echo 'EXTC_RC_SHARDS=4' ;;
    shard16) echo 'EXTC_RC_SHARDS=16' ;;
    shard64) echo 'EXTC_RC_SHARDS=64' ;;
    budget2k) echo '' ;;
    slab256k) echo '' ;;
    localstore0) echo 'EXTC_RC_LOCALSTORE=0' ;;
    *) echo unknown arm $1 >&2; return 1 ;;
  esac
}
arm_ccflags() {
  case $1 in
    budget2k) echo '-DEXTC_RC_WORK_BUDGET=2000' ;;
    slab256k) echo '' ;;
    *) echo '' ;;
  esac
}

build_one() {
  local s=$1 arm=$2 src envs flags
  src=$(shape_src $s); envs=$(arm_env $arm); flags=$(arm_ccflags $arm)
  [ -x "$EXTC" ] || { echo "ERROR: no extc at $EXTC" >&2; return 1; }
  env $envs "$EXTC" -w --no-line-map -I "$STDLIB" "$src" -o "$B/out/$s.$arm.c" || return 1
  if [ "$arm" = slab256k ]; then
    # 生成的 C 里这个宏是无条件 #define（-D 会被覆盖）=> 生成后改常量
    sed -i 's/#define EXTC_RC_SLAB_CHUNK_MIN ((size_t)64 \* 1024)/#define EXTC_RC_SLAB_CHUNK_MIN ((size_t)256 * 1024)/' "$B/out/$s.$arm.c"
    grep -q '((size_t)256 \* 1024)' "$B/out/$s.$arm.c" || { echo "  sed slab failed $s"; return 1; }
  fi
  $CC -O2 -D_POSIX_C_SOURCE=200809L $flags -o "$B/$s.$arm" "$B/out/$s.$arm.c" || return 1
}

do_build() {
  local bad=0 s arm
  for s in $SHAPES; do for arm in $ARMS; do
    if build_one $s $arm; then :; else echo "  FAIL build $s $arm"; bad=1; fi
  done; done
  $CC -O2 -o $B/mrun $ROOT/bench/gc/v2/shim.c || bad=1
  [ $bad = 0 ] && echo "build ok: $SHAPES | $ARMS"
  return $bad
}

field() { printf '%s' "$1" | awk -v k="$2" '{for(i=1;i<=NF;i++){n=split($i,p,"="); if(p[1]==k) print p[2]}}'; }

do_align() {
  local bad=0 s arm ref out
  for s in $SHAPES; do
    ref=$($B/$s.default $(bench_args $s) 2>/dev/null)
    for arm in $ARMS; do
      out=$($B/$s.$arm $(bench_args $s) 2>/dev/null)
      # off/sweepfree 在有环形状上输出相同但内存不同；输出必须一致
      [ x"$out" = x"$ref" ] || { echo "  FAIL align $s $arm"; bad=1; }
    done
  done
  [ $bad = 0 ] && echo 'align: 所有臂 stdout 逐字节相同'
  return $bad
}

do_bench() {
  local bad=0 s arm args rep line ms f r min med max
  printf 'shape\tarm\tms_min\tms_med\tms_max\trss_kb\tutime_ms\tstime_ms\tminflt\tnivcsw\n' > $TSV
  for s in $SHAPES; do
    args=$(bench_args $s)
    ref=$($B/$s.default $args 2>/dev/null)
    echo "== $s ($args)"
    for arm in $ARMS; do : > $B/out/ms.$s.$arm; done
    for rep in $(seq 1 $REPS); do
      for arm in $ARMS; do
        line=$($B/mrun --pin $PIN --out $B/out/got.$s.$arm -- $B/$s.$arm $args 2>/dev/null)
        ms=$(field "$line" ms); f=$B/out/ms.$s.$arm
        if [ -z "$ms" ]; then echo "   FAIL run $s $arm"; bad=1; continue; fi
        echo "$ms" >> $f
        cmp -s $B/out/got.$s.$arm <(printf '%s\n' "$ref") || { echo "   BAD stdout $s $arm"; bad=1; }
        echo "$line" >> $B/out/raw.$s.$arm
      done
    done
    for arm in $ARMS; do
      f=$B/out/ms.$s.$arm; [ -s $f ] || continue
      min=$(sort -g $f | head -1)
      med=$(sort -g $f | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
      max=$(sort -g $f | tail -1)
      # 取该臂最后一次 rusage（RSS 在这里稳定）
      r=$(tail -1 $B/out/raw.$s.$arm)
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$s" "$arm" "$min" "$med" "$max" \
        "$(field "$r" rss_kb)" "$(field "$r" utime_ms)" "$(field "$r" stime_ms)" \
        "$(field "$r" minflt)" "$(field "$r" nivcsw)" >> $TSV
      printf '  %-11s min=%9s med=%9s rss=%8s KB\n' "$arm" "$min" "$med" "$(field "$r" rss_kb)"
    done
  done
  echo "tsv: $TSV"
  return $bad
}

do_pause() {
  local bad=0 s arm args rep line
  local reps=${PAUSE_REPS:-3}
  for s in $SHAPES; do
    args=$(bench_args $s)
    for arm in $ARMS; do
      : > $B/out/pause-raw.$s.$arm
      for rep in $(seq 1 "$reps"); do
        line=$(env EXTC_RC_PAUSE=1 EXTC_RC_STATS=1 \
          $B/mrun --pin $PIN --out $B/out/pause.$s.$arm.r$rep \
          --err $PAUSEDIR/$s.$arm.r$rep.err -- "$B/$s.$arm" $args 2>/dev/null)
        if [ -z "$(field "$line" ms)" ]; then echo "   FAIL pause $s $arm r$rep"; bad=1; continue; fi
        echo "$line" >> $B/out/pause-raw.$s.$arm
      done
      # 旧文件名保留为第 1 次的副本，不破坏已有引用
      cp -f $PAUSEDIR/$s.$arm.r1.err $PAUSEDIR/$s.$arm.err
      grep -q '\[step\]' $PAUSEDIR/$s.$arm.err || true
    done
  done
  echo "pause: $PAUSEDIR (reps=$reps, pin=$PIN)"
  return $bad
}

do_scale() {
  local bad=0 s=condsave arm ns n args
  printf 'shape\tarm\tn\tms_min\trss_kb\n' > $D/out/scale.tsv
  for n in 125000 250000 500000 1000000 2000000 4000000 8000000; do
    for arm in off default count; do
      : > $B/out/sc.$arm.$n
      for rep in 1 2 3; do
        line=$($B/mrun --pin $PIN --out $B/out/got.$s.$arm -- $B/$s.$arm $n 2>/dev/null)
        echo "$(field "$line" ms)" >> $B/out/sc.$arm.$n
        echo "$line" >> $B/out/scraw.$arm.$n
      done
      printf 'condsave\t%s\t%s\t%s\t%s\n' "$arm" "$n" \
        "$(sort -g $B/out/sc.$arm.$n | head -1)" "$(field "$(tail -1 $B/out/scraw.$arm.$n)" rss_kb)" >> $D/out/scale.tsv
    done
  done
  echo "scale: $D/out/scale.tsv"
  return $bad
}

case $MODE in
  build) do_build ;;
  align) do_align ;;
  bench) do_bench ;;
  pause) do_pause ;;
  scale) do_scale ;;
  *) echo "usage: $0 build|align|bench|pause|scale" >&2; exit 2 ;;
esac
