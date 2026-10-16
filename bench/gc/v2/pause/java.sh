#!/usr/bin/env bash
# bench/gc/v2/pause/java.sh - Java 侧停顿事件：-Xlog:gc 里每条 STW GC 暂停事件的墙钟时长。
#
# 事件来源： -Xlog:gc:file=<tmp> 输出中含 " Pause " 的行，取行尾 <N>ms 字段（ms -> us）。
#   在 taskset 单核下 JVM 人体工学选 SerialGC；java_serial 臂显式加 -XX:+UseSerialGC。
# 用法： bash bench/gc/v2/pause/java.sh [outdir]     默认 bench/gc/v2/pause/java
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=${B:-/tmp/v2matrix/build}
JAVA=${JAVA:-java}
PIN=${PIN:-6}
OUT=${1:-$HERE/java}
ARM=$(basename "$OUT")
REPS=${REPS:-1}
case $REPS in ''|0) REPS=1 ;; esac
mkdir -p "$OUT"

TMP=$(mktemp -d /tmp/java_pause.XXXXXX) || { echo "FAIL mktemp" >&2; exit 1; }
trap 'rm -rf "$TMP"' EXIT

shape_args() {
  case $1 in
    promote)    echo '500000 20261013' ;;
    sizeratio)  echo '2000000 20261013' ;;
    chase)      echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
  esac
}

# java_serial 臂显式 SerialGC；java 臂与 run.sh 的 java:default 完全一致（不加 GC flag）。
gc_flags() {
  case $ARM in
    java_serial) echo '-XX:+UseSerialGC' ;;
    *) echo '' ;;
  esac
}

# 公共阶梯（us）：[0,1) [1,2) [2,5) ... [5000,10000) [10000,inf)
hist_of() {
  awk 'BEGIN{ n=split("0 1 2 5 10 20 50 100 200 500 1000 2000 5000 10000",b," ") }
       { d=$1+0; for(i=1;i<=n;i++){ lo=b[i]; hi=(i<n?b[i+1]:-1);
           if(hi<0){ if(d>=lo){c[i]++; break} } else if(d>=lo && d<hi){c[i]++; break} } }
       END{ for(i=1;i<=n;i++){ hi=(i<n?b[i+1]:"inf"); printf "%s\t%s\t%d\n", b[i], hi, c[i]+0 } }'
}

command -v "$JAVA" >/dev/null 2>&1 || [ -x "$JAVA" ] ||
  { echo "缺少 java 可执行（JAVA=$JAVA）" >&2; exit 1; }
for s in promote sizeratio chase cycleratio; do
  [ -f "$B/classes/$s.class" ] ||
    { echo "缺少 $B/classes/$s.class（先跑 bash bench/gc/v2/run.sh build）" >&2; exit 1; }
done

for s in promote sizeratio chase cycleratio; do
  args=$(shape_args "$s")
  gf=$(gc_flags)
  # 不带诊断
  t0=$(date +%s%N)
  taskset -c "$PIN" "$JAVA" -cp "$B/classes" $s $args > "$TMP/$s.plain" 2> "$TMP/$s.plain.err"
  rc=$?
  t1=$(date +%s%N)
  [ $rc = 0 ] || { echo "FAIL 运行(plain) $s rc=$rc" >&2; exit 1; }
  wall_plain=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f",(b-a)/1000000}')
  bestw=
  for r in $(seq 1 "$REPS"); do
    rm -f "$TMP/$s.gc.log"
    t0=$(date +%s%N)
    taskset -c "$PIN" "$JAVA" $gf -Xlog:gc:file="$TMP/$s.gc.log" -cp "$B/classes" $s $args \
        > "$TMP/$s.diag" 2> "$TMP/$s.diag.err"
    rc=$?
    t1=$(date +%s%N)
    [ $rc = 0 ] || { echo "FAIL 运行(diag) $s rc=$rc" >&2; exit 1; }
    [ -f "$TMP/$s.gc.log" ] || { echo "FAIL 未产生 GC 日志 $s" >&2; exit 1; }
    cmp -s "$TMP/$s.plain" "$TMP/$s.diag" ||
      { echo "FAIL $s 带诊断的 stdout 与不带诊断不同" >&2; exit 1; }
    w=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f",(b-a)/1000000}')
    if [ -z "$bestw" ] || awk -v a=$w -v b=$bestw 'BEGIN{exit !(a<b)}'; then
      bestw=$w
      cp "$TMP/$s.gc.log" "$TMP/$s.gc.best"
    fi
  done
  log="$TMP/$s.gc.best"
  # 逐事件：含 " Pause " 的 GC 日志行 -> 行内 <N>ms -> us
  awk '/ Pause /{
         d="";
         if (match($0,/[0-9]+\.[0-9]+ms/)) d=substr($0,RSTART,RLENGTH-2)+0;
         else if (match($0,/[0-9]+ms/)) d=substr($0,RSTART,RLENGTH-2)+0;
         if (d=="") next;
         printf "%.3f\n", d*1000;
       }' "$log" > "$OUT/$s.events"
  events=$(wc -l < "$OUT/$s.events")
  total=$(awk '{s+=$1} END{printf "%.3f", s+0}' "$OUT/$s.events")
  hist_of < "$OUT/$s.events" > "$OUT/$s.hist"
  gc_algo=$(grep -m1 ' Using ' "$log" | sed -n 's/.* Using \([A-Za-z0-9]*\).*/\1/p')
  [ -n "$gc_algo" ] || gc_algo=unknown
  src="-Xlog:gc:file=<tmp> ' Pause ' lines; trailing <N>ms field -> us (per STW GC pause event)"
  # total_gc_us = 该形状上可观测到的「全部 GC 时间」。SerialGC 没有并发阶段 ⇒ 与 total_us 同值；
  # 若日志里真出现并发阶段行（G1/ZGC 之类），就不瞎凑，写 '-' 并在 notes 里点名。
  conc=$(grep -c 'Concurrent' "$log" 2>/dev/null || true); conc=${conc:-0}
  if [ "$conc" = 0 ]; then totgc="$total"; gcnote="SerialGC 无并发阶段（全部 GC 都是 STW）⇒ total_gc_us 与 total_us 同值"
  else totgc="-"; gcnote="日志含 $conc 行并发阶段，total_gc_us 未计入（要 -Xlog:gc+phases 才补齐），故写 '-'"; fi
  {
    echo "lang=java"
    echo "arm=$ARM"
    echo "shape=$s"
    echo "args=$args"
    echo "events=$events"
    echo "total_us=$total"
    echo "wall_ms=$bestw"
    echo "wall_plain_ms=$wall_plain"
    echo "source=$src"
    echo "total_gc_us=$totgc"
    echo "gc_algo=$gc_algo"
    echo "reps=$REPS"
    echo "concurrent_lines=$conc"
    echo "notes=单次运行；REPS=$REPS 取带诊断墙钟最小的一次，events/hist 取自该次；taskset -c $PIN 单核下 JVM 人体工学选 $gc_algo；$gcnote"
  } > "$OUT/$s.meta"
  printf 'ok  %s %s events=%s total_us=%s wall_ms=%s\n' "$ARM" "$s" "$events" "$total" "$bestw"
done
echo "停顿事件：$OUT"
