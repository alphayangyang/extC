#!/usr/bin/env bash
# bench/gc/v2/pause/node.sh - Node 侧停顿事件：V8 每次 GC 的 stop-the-world pause。
#
# 事件来源： node --trace-gc-nvp 每行里的 pause=<ms> 字段（×1000 → µs）。
#   pause 是该次 GC（Scavenge / Mark-Compact / Mark-Sweep）承认的 STW 时长。
# 注意： --trace-gc-nvp 把 trace 写到 stdout（与程序 stdout 同流，受 C stdio 缓冲影响
#   可能与程序输出交错）。所以带诊断那次把 raw stdout 落到 /tmp，去掉以 [ 开头的 V8
#   trace 行后，再与不带诊断的程序 stdout 做 cmp；trace 只进 /tmp，不进仓库产物目录。
# 用法： bash bench/gc/v2/pause/node.sh [outdir]     默认 bench/gc/v2/pause/node
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
V2=$(cd "$HERE/.." && pwd)
B=${B:-/tmp/v2matrix/build}
NODE=${NODE:-node}
PIN=${PIN:-6}
JS_MODE=${JS_MODE:-split}
REPS=${REPS:-1}
OUT=${1:-$HERE/node}
ARM=${ARM:-node}
command -v "$NODE" >/dev/null 2>&1 || { echo "缺少 node 可执行文件：$NODE" >&2; exit 1; }

# 预检：node 臂没有编译产物，缺的是仓库里的 .js 源文件
for s in promote sizeratio chase cycleratio; do
  src="$V2/src/$s.js"
  [ -f "$src" ] || { echo "缺少 $src（node 臂没有编译产物；源文件应在仓库内）" >&2; exit 1; }
  "$NODE" --check "$src" >/dev/null 2>&1 || { echo "FAIL node --check $src" >&2; exit 1; }
done

mkdir -p "$OUT"
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

shape_args() {
  case $1 in
    promote)    echo '500000 20261013' ;;
    sizeratio)  echo '2000000 20261013' ;;
    chase)      echo '1048576 20261013 4000000' ;;
    cycleratio) echo '400000 20261013 100' ;;
  esac
}

# 公共阶梯（µs，README §3 / extc.sh 一致）：重分档无损。
hist_of() {
  awk 'BEGIN{ n=split("0 1 2 5 10 20 50 100 200 500 1000 2000 5000 10000",b," ") }
       { d=$1+0; for(i=1;i<=n;i++){ lo=b[i]; hi=(i<n?b[i+1]:-1);
           if(hi<0){ if(d>=lo){c[i]++; break} } else if(d>=lo && d<hi){c[i]++; break} } }
       END{ for(i=1;i<=n;i++){ hi=(i<n?b[i+1]:"inf"); printf "%s\t%s\t%d\n", b[i], hi, c[i]+0 } }'
}

for s in promote sizeratio chase cycleratio; do
  src="$V2/src/$s.js"
  args=$(shape_args "$s")

  # 不带诊断：参考 stdout + wall_plain_ms
  p0=$(date +%s%N)
  taskset -c "$PIN" env JS_MODE="$JS_MODE" "$NODE" "$src" $args > "$TMPD/$s.plain.out" 2>"$TMPD/$s.plain.err"
  rc=$?
  p1=$(date +%s%N)
  [ $rc = 0 ] || { echo "FAIL 运行 $s (plain) rc=$rc" >&2; exit 1; }
  wall_plain=$(awk -v a=$p0 -v b=$p1 'BEGIN{printf "%.3f",(b-a)/1000000}')

  # 带诊断：REPS 次，逐次 cmp 程序 stdout，取墙钟最小那次的事件
  bestw=; bestraw=
  for r in $(seq 1 "$REPS"); do
    t0=$(date +%s%N)
    taskset -c "$PIN" env JS_MODE="$JS_MODE" "$NODE" --trace-gc-nvp "$src" $args \
        > "$TMPD/$s.diag.raw" 2> "$TMPD/$s.err"
    rc=$?
    t1=$(date +%s%N)
    [ $rc = 0 ] || { echo "FAIL 运行 $s (diag) rc=$rc" >&2; exit 1; }
    grep -v '^\[' "$TMPD/$s.diag.raw" > "$TMPD/$s.diag.prog" || true
    cmp -s "$TMPD/$s.plain.out" "$TMPD/$s.diag.prog" || {
      echo "FAIL $s 带诊断的 stdout 与不带诊断不同" >&2
      echo "--- plain ---" >&2; cat "$TMPD/$s.plain.out" >&2
      echo "--- diag(程序行) ---" >&2; cat "$TMPD/$s.diag.prog" >&2
      exit 1
    }
    w=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f",(b-a)/1000000}')
    if [ -z "$bestw" ] || awk -v a=$w -v b=$bestw 'BEGIN{exit !(a<b)}'; then
      bestw=$w; bestraw="$TMPD/$s.best.raw"; cp "$TMPD/$s.diag.raw" "$bestraw"
    fi
  done

  # 最优那次：抽 pause（ms → µs），一行一个事件
  grep -o 'pause=[0-9.]*' "$bestraw" | sed 's/^pause=//' | awk '{printf "%.3f\n", $1*1000}' > "$OUT/$s.events" || true
  events=$(wc -l < "$OUT/$s.events")
  total=$(awk '{s+=$1} END{printf "%.3f", s+0}' "$OUT/$s.events")
  hist_of < "$OUT/$s.events" > "$OUT/$s.hist"
  eminor=$(grep -c 'gc=s' "$bestraw" || true)
  emajor=$((events - eminor))
  # total_gc_us = 该形状上可观测到的全部 GC 时间。nvp 行里：
  #   gc=s 的 scavenge=<ms> 是整段 scavenge（含并行部分）；major 行只有 pause= 是 STW 片段，
  #   并发标记不在行里 ⇒ 对 major 这只是下界，故另存 gc_us_major_pause_only 并在 notes 里点名。
  awk '{ p=""; sc="";
         if (match($0,/pause=[0-9.]+/)) p=substr($0,RSTART+6,RLENGTH-6)+0;
         if (match($0,/scavenge=[0-9.]+/)) sc=substr($0,RSTART+9,RLENGTH-9)+0;
         if ($0 ~ /gc=s/) { if (sc=="") sc=p; scav+=sc } else { majp+=p; majn++ }
       }
       END { printf "%.3f %.3f %d\n", scav*1000, majp*1000, majn+0 }' "$bestraw" > "$TMPD/$s.gcstat"
  read scav majp majn < "$TMPD/$s.gcstat"
  totgc=$(awk -v a=$scav -v b=$majp 'BEGIN{printf "%.3f", a+b}')

  cat > "$OUT/$s.meta" <<EOF
lang=node
arm=$ARM
shape=$s
args=$args
events=$events
total_us=$total
wall_ms=$bestw
wall_plain_ms=$wall_plain
source=node --trace-gc-nvp -> per-GC 'pause' field (ms) x1000 to us; V8 STW pause of Scavenge/Mark-Compact/Mark-Sweep
events_minor=$eminor
events_major=$emajor
total_gc_us=$totgc
gc_us_scavenge=$scav
gc_us_major_pause_only=$majp
js_mode=$JS_MODE
reps=$REPS
notes=plain x1 + diag x$REPS on cpu $PIN; events/wall from the min-wall diag run; V8 trace goes to stdout so diag stdout is normalized by dropping '^[' lines before cmp; intermediates kept in /tmp only; total_gc_us = sum(scavenge= over gc=s lines) + sum(pause= over major lines) -- major 行的并发标记不在 nvp 行里，该部分对 major 只是下界; pause 字段只有 1 位小数 ms = 100us 分辨率（亚 100us 的 scavenge 记 0）
EOF
  printf 'ok  %s %s events=%s total_us=%s wall_ms=%s\n' "$ARM" "$s" "$events" "$total" "$bestw"
done
echo "停顿事件：$OUT"
