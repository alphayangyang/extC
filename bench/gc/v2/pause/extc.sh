#!/usr/bin/env bash
# bench/gc/v2/pause/extc.sh - extC 侧停顿事件：每个有界回收步（mark / sweep）的墙钟时长。
#
# 事件来源： EXTC_RC_PAUSE=1 EXTC_RC_SLOWSTEP=0.001 时生成物打的 `[slow]` 行
#   （阈值单位是 µs，0.001 µs = 1 ns ⇒ 每个步都留档）。
# 用法： bash bench/gc/v2/pause/extc.sh [outdir]     默认 bench/gc/v2/pause/extc
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
V2=$(cd "$HERE/.." && pwd)
B=${B:-/tmp/v2matrix/build}
PIN=${PIN:-6}
OUT=${1:-$HERE/extc}
REPS=${REPS:-1}
export OUT
mkdir -p "$OUT"

shape_args() {
  case $1 in
    promote)    echo '500000 20261013' ;;
    sizeratio)  echo '2000000 20261013' ;;
    chase)      echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
  esac
}

# 公共阶梯（µs）：[0,1) [1,2) [2,5) [5,10) [10,20) [20,50) [50,100) [100,200) [200,500) [500,1000) [1000,2000) [2000,5000) [5000,10000) [10000,inf)
hist_of() {
  awk 'BEGIN{ n=split("0 1 2 5 10 20 50 100 200 500 1000 2000 5000 10000",b," ") }
       { d=$1+0; for(i=1;i<=n;i++){ lo=b[i]; hi=(i<n?b[i+1]:-1);
           if(hi<0){ if(d>=lo){c[i]++; break} } else if(d>=lo && d<hi){c[i]++; break} } }
       END{ for(i=1;i<=n;i++){ hi=(i<n?b[i+1]:"inf"); printf "%s\t%s\t%d\n", b[i], hi, c[i]+0 } }'
}

for s in promote sizeratio chase cycleratio; do
  [ -x "$B/$s.extc" ] || { echo "缺少 $B/$s.extc（先跑 bash bench/gc/v2/run.sh build）" >&2; exit 1; }
  args=$(shape_args $s)
  p0=$(date +%s%N)
  taskset -c "$PIN" "$B/$s.extc" $args > "$OUT/.$s.plain.out" 2>/dev/null
  p1=$(date +%s%N)
  wall_plain=$(awk -v a=$p0 -v b=$p1 'BEGIN{printf "%.3f", (b-a)/1000000}')
  bestw=
  for r in $(seq 1 "$REPS"); do
    t0=$(date +%s%N)
    env EXTC_RC_PAUSE=1 EXTC_RC_SLOWSTEP=0.001 taskset -c "$PIN" "$B/$s.extc" $args \
        > "$OUT/.$s.diag.out" 2> "$OUT/.$s.err"
    rc=$?
    t1=$(date +%s%N)
    [ $rc = 0 ] || { echo "FAIL 运行 $s rc=$rc" >&2; exit 1; }
    cmp -s "$OUT/.$s.plain.out" "$OUT/.$s.diag.out" ||
      { echo "FAIL $s 带诊断的 stdout 与不带诊断不同" >&2; exit 1; }
    w=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", (b-a)/1000000}')
    if [ -z "$bestw" ] || awk -v a=$w -v b=$bestw 'BEGIN{exit !(a<b)}'; then
      bestw=$w; cp "$OUT/.$s.err" "$OUT/.$s.err.best"
    fi
  done
  err="$OUT/.$s.err.best"
  awk '/^\[slow\]/{
         d=""; k="";
         if (match($0,/kind=[a-z]+/)) k=substr($0,RSTART+5,RLENGTH-5);
         if (match($0,/dur_us=[0-9.]+/)) d=substr($0,RSTART+7,RLENGTH-7);
         if (d=="") next;
         print d;
         if (k=="mark"){ em++; tm+=d; print d > (ENVIRON["OUT"] "/.ev" "mark") }
         else { es++; ts+=d; print d > (ENVIRON["OUT"] "/.ev" "sweep") }
       }
       END{ printf("%d %.3f %d %.3f\n", em+0, tm+0, es+0, ts+0) > (ENVIRON["OUT"] "/.stat") }' \
      "$err" > "$OUT/$s.events"
  read em tm es ts < "$OUT/.stat"
  events=$(wc -l < "$OUT/$s.events")
  total=$(awk '{s+=$1} END{printf "%.3f", s+0}' "$OUT/$s.events")
  hist_of < "$OUT/$s.events" > "$OUT/$s.hist"
  st=$(grep '^\[step\]' "$err" | tail -1)
  pa=$(grep '^\[pause\]' "$err" | tail -1)
  slices=$(printf '%s' "$st" | sed -n 's/.* n=\([0-9]*\).*/\1/p')
  stot=$(printf '%s' "$st" | sed -n 's/.* total_us=\([0-9.]*\).*/\1/p')
  cols=$(printf '%s' "$pa" | sed -n 's/.* collects=\([0-9]*\).*/\1/p')
  ptot=$(printf '%s' "$pa" | sed -n 's/.* total_us=\([0-9.]*\).*/\1/p')
  cat > "$OUT/$s.meta" <<EOF
lang=extc
arm=extc
shape=$s
args=$args
events=$events
total_us=$total
wall_ms=$bestw
wall_plain_ms=$wall_plain
source=EXTC_RC_SLOWSTEP=0.001 -> [slow] dur_us= lines (per bounded step: mark / sweep)
events_mark=$em
total_us_mark=$tm
events_sweep=$es
total_us_sweep=$ts
slices=$slices
step_total_us=$stot
total_gc_us=$stot
collects=$cols
pause_total_us=$ptot
notes=单次运行；REPS=$REPS 取墙钟最小的一次，events 取自该次
EOF
  # 分族子集：[step] 是两个总体（mark 走图、sweep 走字），分位分开读才有意义。
  for kd in mark sweep; do
    d="$HERE/extc_$kd"; mkdir -p "$d"
    [ -s "$OUT/.ev$kd" ] || : > "$OUT/.ev$kd"
    hist_of < "$OUT/.ev$kd" > "$d/$s.hist"
    if [ $kd = mark ]; then kn=$em; kt=$tm; else kn=$es; kt=$ts; fi
    cat > "$d/$s.meta" <<EOF
lang=extc
arm=extc_$kd
shape=$s
args=$args
events=$kn
total_us=$kt
wall_ms=$bestw
wall_plain_ms=$wall_plain
source=EXTC_RC_SLOWSTEP=0.001 -> [slow] lines with kind=$kd
slices=$slices
collects=$cols
step_total_us=$stot
pause_total_us=$ptot
notes=extc 全体的 kind=$kd 子集；只存 hist（分位按档上界近似），逐事件表在 extc/ 下
EOF
  done
  rm -f "$OUT"/."$s".plain.out "$OUT"/."$s".diag.out "$OUT"/."$s".err "$OUT"/."$s".err.best "$OUT"/.stat "$OUT"/.evmark "$OUT"/.evsweep
  printf 'ok  extc %s events=%s total_us=%s wall_ms=%s\n' "$s" "$events" "$total" "$bestw"
done
echo "停顿事件：$OUT"
