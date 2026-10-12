#!/usr/bin/env bash
# 四语言同一功能对照：有界符号表（64 桶 ×≤8，满则淘汰尾）+ 1024 槽缓存 + 20 万项索引 + 查询。
# 对手开足优化；RSS 取 /usr/bin/time -v；GC 停顿从各自日志取。
set -uo pipefail
N="${1:-2000000}"; cd "$(dirname "$0")"
rssit() { /usr/bin/time -v "$@" 2>&1 >/dev/null | awk -F': ' '/Maximum resident/{print $2}'; }
mk3() { # mk3 "标签" 若干命令...  —— 跑三次取最小耗时（打印一行）
  local label="$1"; shift; local best=""
  for _ in 1 2 3; do
    local t; t=$("$@" 2>/dev/null | head -1 | grep -oP '耗时=\s*\K[0-9.]+')
    [ -z "$t" ] && { echo "$label 跑失败"; return; }
    best=$(python3 -c "v=[float(x) for x in ['${best}','${t}'] if x.strip()]; print(min(v)) if v else print('')")
  done
  printf '%s' "$best"
}
printf '%-14s %9s %12s %13s %12s %8s\n' 语言 耗时ms 峰值RSS_MiB GC最大停顿us GC总停顿ms GC次数

# ---- C：本设计（位图 slab + 事件回收 + 区域清扫）----
export EV=1
tc=""
for _ in 1 2 3; do
  t=$(EV=1 /tmp/s_hold $N 1024 1 200000 24 2>/dev/null | head -1 | grep -oP '耗时=\s*\K[0-9.]+')
  [ -n "$t" ] && tc=$(python3 -c "v=[float(x) for x in ['${tc}','${t}'] if x.strip()]; print(min(v))")
done
rc=$(rssit /tmp/s_hold $N 1024 1 200000 24); l=$(/tmp/s_hold $N 1024 1 200000 24|head -1)
pc=$(printf '%s' "$l"|grep -oP '最大停顿=\s*\K[0-9.]+'); ptc=$(printf '%s' "$l"|grep -oP '停顿总=\s*\K[0-9.]+')
gc=$(printf '%s' "$l"|grep -oP '清扫=\s*\K[0-9]+')
printf '%-14s %9s %12s %13s %12s %8s\n' "C(本设计)" "$tc" "$(python3 -c "print(f'{$rc/1024:.0f}')")" "$pc" "$ptc" "$gc"

# ---- Go：默认 GOGC ----
tg=$(mk3 "Go" ./bench_go $N); rg=$(rssit ./bench_go $N)
GODEBUG=gctrace=1 ./bench_go $N 2>/tmp/gg.log >/dev/null
pg=$(grep -oP '\+\K[0-9.]+(?= ms clock)' /tmp/gg.log | sort -g | tail -1)
tg2=$(awk '{for(i=1;i<=NF;i++) if($i=="ms" && $(i-1)=="clock"){split($(i-3),p,"+"); s+=p[1]+p[3]}} END{printf "%.3f", s}' /tmp/gg.log); ng=$(grep -c '^gc ' /tmp/gg.log)
printf '%-14s %9s %12s %13s %12s %8s\n' "Go" "$tg" "$(python3 -c "print(f'{$rg/1024:.0f}')")" \
  "$(python3 -c "print(f'{float('${pg:-0}')*1000:.1f}')")" "$(python3 -c "print(f'{float('${tg2:-0}')*1000:.1f}')")" "$ng"

# ---- Java：G1 + 小堆（大堆会让 young 变大、停顿变长）----
JF=(-Xms256m -Xmx256m -XX:+UseG1GC -XX:MaxGCPauseMillis=5)
java "${JF[@]}" Bench 200000 >/dev/null 2>&1
tj=$(mk3 "Java" java "${JF[@]}" Bench $N); rj=$(rssit java "${JF[@]}" Bench $N)
java "${JF[@]}" -Xlog:gc:/tmp/gc.log Bench $N >/dev/null 2>&1
pj=$(grep -oP '(?<= )[0-9.]+(?=ms$)' /tmp/gc.log | sort -g | tail -1)
tj2=$(grep -oP '(?<= )[0-9.]+(?=ms$)' /tmp/gc.log | paste -sd+ | bc); nj=$(grep -c 'Pause' /tmp/gc.log)
printf '%-14s %9s %12s %13s %12s %8s\n' "Java(G1)" "$tj" "$(python3 -c "print(f'{$rj/1024:.0f}')")" \
  "$(python3 -c "print(f'{float('${pj:-0}')*1000:.1f}')")" "${tj2:-0}" "$nj"

# ---- Node：V8 默认 ----
tn=$(mk3 "Node" node bench.js $N); rn=$(rssit node bench.js $N)
node --trace-gc bench.js $N >/tmp/gn.log 2>&1
pn=$(grep -oP '[0-9.]+(?= / )' /tmp/gn.log | sort -g | tail -1)
tn2=$(grep -oP '[0-9.]+(?= / )' /tmp/gn.log | paste -sd+ | bc); nn=$(grep -c 'ms:' /tmp/gn.log)
printf '%-14s %9s %12s %13s %12s %8s\n' "Node(V8)" "$tn" "$(python3 -c "print(f'{$rn/1024:.0f}')")" \
  "$(python3 -c "print(f'{float('${pn:-0}')*1000:.1f}')")" "${tn2:-0}" "$nn"
