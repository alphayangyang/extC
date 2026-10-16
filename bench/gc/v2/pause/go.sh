#!/usr/bin/env bash
# bench/gc/v2/pause/go.sh - Go 侧 GC 停顿事件（每个 GC 周期两段 STW）。
#
# 事件来源： GODEBUG=gctrace=1 写到 stderr 的 `gc N @...s ..%: A+B+C ms clock, ...` 行。
#   每周期取两段 STW：A = sweep termination，C = mark termination；
#   中间 B 是并发标记（不是停顿），不进事件。
#   单位换算：gctrace 打的是 ms（%.3f ⇒ 1µs 分辨率）⇒ A*1000 / C*1000 得到 µs。
# 用法： bash bench/gc/v2/pause/go.sh [outdir]     默认 bench/gc/v2/pause/go
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
V2=$(cd "$HERE/.." && pwd)
B=${B:-/tmp/v2matrix/build}
PIN=${PIN:-6}
OUT=${1:-$HERE/go}
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

# 公共阶梯（µs），与 README §3 / extc.sh 相同；聚合器仍会重分档。
hist_of() {
  awk 'BEGIN{ n=split("0 1 2 5 10 20 50 100 200 500 1000 2000 5000 10000",b," ") }
       { d=$1+0; for(i=1;i<=n;i++){ lo=b[i]; hi=(i<n?b[i+1]:-1);
           if(hi<0){ if(d>=lo){c[i]++; break} } else if(d>=lo && d<hi){c[i]++; break} } }
       END{ for(i=1;i<=n;i++){ hi=(i<n?b[i+1]:"inf"); printf "%s\t%s\t%d\n", b[i], hi, c[i]+0 } }'
}

for s in promote sizeratio chase cycleratio; do
  [ -x "$B/$s.go.bin" ] || { echo "缺少 $B/$s.go.bin（先跑 bash bench/gc/v2/run.sh build）" >&2; exit 1; }
  args=$(shape_args $s)

  # 不带诊断：GOMAXPROCS=1，与 run.sh 的 go:default 臂一致
  p0=$(date +%s%N)
  env GOMAXPROCS=1 taskset -c "$PIN" "$B/$s.go.bin" $args > "$OUT/.$s.plain.out" 2>/dev/null
  prc=$?
  p1=$(date +%s%N)
  [ $prc = 0 ] || { echo "FAIL 运行(plain) $s rc=$prc" >&2; exit 1; }
  wall_plain=$(awk -v a=$p0 -v b=$p1 'BEGIN{printf "%.3f", (b-a)/1000000}')

  bestw=
  for r in $(seq 1 "$REPS"); do
    t0=$(date +%s%N)
    env GOMAXPROCS=1 GODEBUG=gctrace=1 taskset -c "$PIN" "$B/$s.go.bin" $args \
        > "$OUT/.$s.diag.out" 2> "$OUT/.$s.err"
    rc=$?
    t1=$(date +%s%N)
    [ $rc = 0 ] || { echo "FAIL 运行(diag) $s rc=$rc" >&2; exit 1; }
    cmp -s "$OUT/.$s.plain.out" "$OUT/.$s.diag.out" ||
      { echo "FAIL $s 带诊断的 stdout 与不带诊断不同" >&2; exit 1; }
    w=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", (b-a)/1000000}')
    if [ -z "$bestw" ] || awk -v a=$w -v b=$bestw 'BEGIN{exit !(a<b)}'; then
      bestw=$w; cp "$OUT/.$s.err" "$OUT/.$s.err.best"
    fi
  done
  err="$OUT/.$s.err.best"

  awk '
    /^gc / {
      if (!match($0, /[0-9][0-9.]*\+[0-9][0-9.]*\+[0-9][0-9.]* ms clock/)) next;
      seg = substr($0, RSTART, RLENGTH);
      sub(/ ms clock$/, "", seg);
      n = split(seg, p, "+");
      if (n != 3) next;
      sw = p[1]*1000; mk = p[3]*1000;
      printf "%.3f\n%.3f\n", sw, mk;
      cyc++; es++; ts += sw; em++; tm += mk;
      all += (p[1]+p[2]+p[3])*1000;
    }
    END { printf("%d %.3f %d %.3f %d %.3f\n", es+0, ts+0, em+0, tm+0, cyc+0, all+0) > (ENVIRON["OUT"] "/.stat") }
  ' "$err" > "$OUT/$s.events"

  read es ts em tm cyc gcall < "$OUT/.stat"
  events=$(wc -l < "$OUT/$s.events")
  total=$(awk '{s+=$1} END{printf "%.3f", s+0}' "$OUT/$s.events")
  hist_of < "$OUT/$s.events" > "$OUT/$s.hist"

  cat > "$OUT/$s.meta" <<EOF
lang=go
arm=go
shape=$s
args=$args
events=$events
total_us=$total
wall_ms=$bestw
wall_plain_ms=$wall_plain
source=GODEBUG=gctrace=1 stderr 'gc N ... A+B+C ms clock' -> A (sweep termination STW) + C (mark termination STW), *1000 = us
gc_cycles=$cyc
events_sweepterm=$es
total_us_sweepterm=$ts
events_markterm=$em
total_us_markterm=$tm
total_gc_us=$gcall
notes=单次运行；REPS=$REPS 取带诊断墙钟最小的一次，events 取自该次；events=2*gc_cycles；total_gc_us=Σ(A+B+C) 即全部 GC 阶段的墙钟之和（GOMAXPROCS=1 ⇒ 并发标记也占同一条核，可与 extC 的 Σ[step] 对照；total_us 只是其中 A+C 两段 STW）
EOF

  rm -f "$OUT"/."$s".plain.out "$OUT"/."$s".diag.out "$OUT"/."$s".err "$OUT"/."$s".err.best "$OUT"/.stat
  printf 'ok  go %s events=%s total_us=%s wall_ms=%s\n' "$s" "$events" "$total" "$bestw"
done
echo "停顿事件：$OUT"
