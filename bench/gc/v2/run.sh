#!/usr/bin/env bash
# bench/gc/v2/run.sh - v2 GC 大横评卡具：构建 + 跨语言对齐 + 计时。
# 用法： bash bench/gc/v2/run.sh [align|bench|build] [--lang 'extc go java rust node cpp'] [--shape '...'] [--reps N] [--pin N]
# 环境： EXTC=编译器路径 B=产物目录 PIN=核号 REPS=重复次数
set -u
V2=$(cd $(dirname $0) && pwd)
ROOT=$(cd $V2/../../.. && pwd)
B=${B:-$V2/build}
EXTC=${EXTC:-$ROOT/build/extc}
STDLIB=${STDLIB:-$ROOT/stdlib}
CC=${CC:-cc}
CXX=${CXX:-g++}
RUSTC=${RUSTC:-rustc}
GO=${GO:-go}
JAVAC=${JAVAC:-javac}
JAVA=${JAVA:-java}
NODE=${NODE:-node}
PIN=${PIN:-6}
REPS=${REPS:-5}
MODE=${1:-align}
if [ $# -gt 0 ]; then shift; fi
LANGS='extc go java rust node cpp'
SHAPES='promote sizeratio chase cycleratio'
while [ $# -gt 0 ]; do
  case $1 in
    --lang) LANGS=$2; shift 2 ;;
    --shape) SHAPES=$2; shift 2 ;;
    --reps) REPS=$2; shift 2 ;;
    --pin) PIN=$2; shift 2 ;;
    -h|--help) sed -n '2,5p' $0; exit 0 ;;
    *) echo unknown arg: $1 >&2; exit 2 ;;
  esac
done
mkdir -p $B/classes $B/out

align_args() {
  case $1 in
    promote) echo '100000 20261013' ;;
    sizeratio) echo '200000 20261013' ;;
    chase) echo '65536 20261013 200000' ;;
    cycleratio) echo '200000 20261013 100' ;;
  esac
}
bench_args() {
  case $1 in
    promote) echo '500000 20261013' ;;
    sizeratio) echo '2000000 20261013' ;;
    chase) echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
  esac
}
arms_of() {
  case $1 in
    extc) echo 'default off' ;;
    go) echo 'default off' ;;
    java) echo default ;;
    rust) echo 'default careful' ;;
    node) echo 'split bigint' ;;
    cpp) echo 'default careful' ;;
  esac
}

build_one() {
  local lang=$1 s=$2 U
  case $lang in
    extc)
      [ -x $EXTC ] || { echo ERROR: extC 编译器不可执行 $EXTC >&2; return 1; }
      $EXTC -w --no-line-map -I $STDLIB $V2/src/$s.extc -o $B/$s.extc.c &&
        $CC -O2 -D_POSIX_C_SOURCE=200809L -o $B/$s.extc $B/$s.extc.c &&
        env EXTC_RECLAIM=0 $EXTC -w --no-line-map -I $STDLIB $V2/src/$s.extc -o $B/$s.extc.off.c &&
        $CC -O2 -D_POSIX_C_SOURCE=200809L -o $B/$s.extc.off $B/$s.extc.off.c ;;
    go) $GO build -o $B/$s.go.bin $V2/src/$s.go ;;
    java) $JAVAC -d $B/classes $V2/src/$s.java ;;
    rust) $RUSTC -O --edition 2021 $V2/src/$s.rs -o $B/$s.rs.bin &&
          $RUSTC -O --edition 2021 --cfg careful $V2/src/$s.rs -o $B/$s.rs_careful.bin ;;
    node) $NODE --check $V2/src/$s.js ;;
    cpp) U=$(echo $s | tr a-z A-Z)
         $CXX -O2 -std=c++17 -D${U}_NAIVE $V2/src/$s.cpp -o $B/$s.cpp.bin &&
         $CXX -O2 -std=c++17 -D${U}_CAREFUL $V2/src/$s.cpp -o $B/$s.cpp_careful.bin ;;
    *) echo unknown lang $lang >&2; return 1 ;;
  esac
}

run_arm() {
  local lang=$1 kind=$2 s=$3
  shift 3
  case $lang:$kind in
    extc:default) $B/$s.extc $@ ;;
    extc:off) $B/$s.extc.off $@ ;;
    go:default) env GOMAXPROCS=1 $B/$s.go.bin $@ ;;
    go:off) env GOMAXPROCS=1 GOGC=off $B/$s.go.bin $@ ;;
    java:default) $JAVA -cp $B/classes $s $@ ;;
    rust:default) $B/$s.rs.bin $@ ;;
    rust:careful) $B/$s.rs_careful.bin $@ ;;
    node:split) env JS_MODE=split $NODE $V2/src/$s.js $@ ;;
    node:bigint) env JS_MODE=bigint $NODE $V2/src/$s.js $@ ;;
    cpp:default) $B/$s.cpp.bin $@ ;;
    cpp:careful) $B/$s.cpp_careful.bin $@ ;;
    *) echo unknown arm $lang:$kind >&2; return 1 ;;
  esac
}

wrapper() {
  local lang=$1 kind=$2 s=$3 w=$B/wrap_$1_$2_$3 cmd=
  case $lang:$kind in
    extc:default) cmd=$B/$s.extc ;;
    extc:off) cmd=$B/$s.extc.off ;;
    go:default) cmd='env GOMAXPROCS=1 '$B/$s.go.bin ;;
    go:off) cmd='env GOMAXPROCS=1 GOGC=off '$B/$s.go.bin ;;
    java:default) cmd=$JAVA' -cp '$B/classes' '$s ;;
    rust:default) cmd=$B/$s.rs.bin ;;
    rust:careful) cmd=$B/$s.rs_careful.bin ;;
    node:split) cmd='env JS_MODE=split '$NODE' '$V2/src/$s.js ;;
    node:bigint) cmd='env JS_MODE=bigint '$NODE' '$V2/src/$s.js ;;
    cpp:default) cmd=$B/$s.cpp.bin ;;
    cpp:careful) cmd=$B/$s.cpp_careful.bin ;;
  esac
  printf '#!/usr/bin/env bash\nexec %s "$@"\n' "$cmd" > $w
  chmod +x $w
  echo $w
}

field() { printf '%s' "$1" | awk -v k="$2" '{for(i=1;i<=NF;i++){n=split($i,p,"="); if(p[1]==k) print p[2]}}'; }

do_build() {
  local bad=0 s lang
  for s in $SHAPES; do for lang in $LANGS; do
    if build_one $lang $s; then echo '  ok   build' $lang $s; else echo '  FAIL build' $lang $s; bad=1; fi
  done; done
  return $bad
}

do_align() {
  local bad=0 s lang kind args ref out
  for s in $SHAPES; do
    args=$(align_args $s)
    build_one extc $s || { echo '   FAIL build extc' $s; bad=1; }
    ref=$(run_arm extc default $s $args 2>/dev/null)
    echo '== '$s' ('$args')'
    for lang in $LANGS; do
      if ! build_one $lang $s; then echo '   FAIL build' $lang $s; bad=1; continue; fi
      for kind in $(arms_of $lang); do
        out=$(run_arm $lang $kind $s $args 2>/dev/null)
        if [ x"$out" = x"$ref" ]; then echo '   ok   '$lang:$kind; else
          echo '   FAIL '$lang:$kind
          printf '%s\n' "$ref" > $B/out/ref_$s.txt
          printf '%s\n' "$out" > $B/out/got_${lang}_${kind}_$s.txt
          diff $B/out/ref_$s.txt $B/out/got_${lang}_${kind}_$s.txt | sed 's/^/        /'
          bad=1
        fi
      done
    done
  done
  if [ $bad = 0 ]; then echo '对齐：全部逐字节相同'; else echo '对齐：有 FAIL'; fi
  return $bad
}

do_bench() {
  local bad=0 s lang kind args refbase w outf line ms rss f min med max rel REF ut st mf mj nc ni
  $CC -O2 -o $B/v2run $V2/shim.c || return 1
  if [ -n "${TSV:-}" ]; then
    printf 'shape\tarm\tms_min\tms_med\tms_max\trss_kb\tutime_ms\tstime_ms\tminflt\tmajflt\tnvcsw\tnivcsw\n' > $TSV
  fi
  for s in $SHAPES; do
    args=$(bench_args $s)
    for lang in $LANGS; do for kind in $(arms_of $lang); do
      build_one $lang $s || { echo '   FAIL build' $lang $s; bad=1; }
    done; done
    refbase=$B/out/ref_$s.txt
    run_arm extc default $s $args > $refbase 2>/dev/null
    if [ ! -s $refbase ]; then echo '   FAIL extC 参考输出为空' $s; bad=1; continue; fi
    echo
    echo '== '$s' ('$args')'
    printf '%-18s %9s %9s %9s %11s %7s\n' arm ms_min ms_med ms_max rss_kb rel
    for lang in $LANGS; do for kind in $(arms_of $lang); do
      f=$B/out/ms_${lang}_${kind}_$s.txt
      : > $f
      : > $f.raw
    done; done
    # 交错测量：同一轮里每个臂各跑一次（噪声突发不会整段压在某一臂的连续 REPS 次上）
    for r in $(seq 1 $REPS); do
      for lang in $LANGS; do for kind in $(arms_of $lang); do
        w=$(wrapper $lang $kind $s)
        f=$B/out/ms_${lang}_${kind}_$s.txt
        outf=$B/out/got_${lang}_${kind}_$s.txt
        line=$($B/v2run --pin $PIN --out $outf -- $w $args)
        ms=$(field "$line" ms)
        if [ -z "$ms" ]; then echo '   FAIL run' $lang:$kind; bad=1; continue; fi
        echo $ms >> $f
        echo "$line" >> $f.raw
        cmp -s $outf $refbase || { echo '   BAD  stdout 与 extC 不同' $lang:$kind; bad=1; }
      done; done
    done
    REF=
    for lang in $LANGS; do for kind in $(arms_of $lang); do
      f=$B/out/ms_${lang}_${kind}_$s.txt
      [ -s $f ] || continue
      min=$(sort -g $f | head -1)
      med=$(sort -g $f | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
      max=$(sort -g $f | tail -1)
      line=$(awk '{v=$1; sub(/^ms=/,"",v); if (bl=="" || v+0<best+0) {best=v; bl=$0}} END{print bl}' $f.raw)
      rss=$(field "$line" rss_kb)
      ut=$(field "$line" utime_ms); st=$(field "$line" stime_ms)
      mf=$(field "$line" minflt); mj=$(field "$line" majflt)
      nc=$(field "$line" nvcsw); ni=$(field "$line" nivcsw)
      if [ -n "$REF" ]; then rel=$(awk -v a=$min -v b=$REF 'BEGIN{printf "%.2fx", a/b}'); else rel=-; fi
      printf '%-18s %9s %9s %9s %11s %7s\n' $lang:$kind $min $med $max $rss $rel
      if [ -n "${TSV:-}" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $s $lang:$kind $min $med $max $rss $ut $st $mf $mj $nc $ni >> $TSV
      fi
      if [ $lang = extc ] && [ $kind = default ]; then REF=$min; fi
    done; done
  done
  if [ $bad = 0 ]; then echo; echo '计时表：stdout 全部与 extC 相同'; else echo; echo '计时表：有 FAIL/BAD'; fi
  return $bad
}
case $MODE in
  build) do_build ;;
  align) do_align ;;
  bench) do_bench ;;
  *) echo unknown mode $MODE >&2; exit 2 ;;
esac