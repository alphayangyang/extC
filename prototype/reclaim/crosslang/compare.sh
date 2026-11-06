#!/usr/bin/env bash
# 本设计 vs Go：无环 / 有环 两张表，全部指标
set -uo pipefail; cd "$(dirname "$0")"; N="${1:-2000000}"
rss(){ /usr/bin/time -v "$@" 2>&1 >/dev/null|awk -F': ' '/Maximum resident/{print $2}'; }
gostat(){ # $1 二进制 $2 GOGC  -> "耗时 p50 p90 p99 max n"
  local out pauses
  out=$(GODEBUG=gctrace=1 GOGC=$2 "$1" $N 2>&1 >/dev/null)
  pauses=$(echo "$out"|grep -oP ':\s*[0-9.]+\+[0-9.]+\+[0-9.]+\s*ms clock'|grep -oP '[0-9.]+(?=\+[0-9.]+ ms clock)|(?<=\+)[0-9.]+(?= ms clock)' 2>/dev/null)
  echo "$out" | python3 -c "
import sys,re
ps=[]
for l in sys.stdin:
    m=re.search(r':\s*([0-9.]+)\+([0-9.]+)\+([0-9.]+)\s*ms clock', l)
    if m: ps.append((float(m.group(1))+float(m.group(3)))*1000)
ps.sort(); n=len(ps)
q=lambda p: ps[min(n-1,int(n*p))] if n else 0
print(f'{q(.5):.1f} {q(.9):.1f} {q(.99):.1f} {ps[-1] if n else 0:.1f} {n}')"
}
mytab(){ # $1 = 可执行 $2 = N $3 = 分片 ... 其余参数
  awk '{print}' >/dev/null
}
printf '%-22s %8s %9s %8s %8s %8s %8s %7s\n' 实现 耗时ms 峰值RSSMiB p50us p90us p99us maxus GC次数
# ---- 无环 ----
for k in 8 16 32; do
  o=$(EV=1 /tmp/final $N 1024 1 200000 24 $k 2>/dev/null)
  line=$(echo "$o"|grep -oP 'p50=[0-9.]+ p90=[0-9.]+ p99=[0-9.]+ max=[0-9.]+ n=[0-9]+'|head -1)
  r=$(rss env EV=1 /tmp/final $N 1024 1 200000 24 $k)
  printf '%-22s %8s %9s %8s %8s %8s %8s %7s\n' "本设计 分片=$k" "$(echo "$o"|grep -oP '耗时=\s*\K[0-9.]+')" "$(python3 -c "print(f'{$r/1024:.0f}')")" \
    "$(echo "$line"|grep -oP 'p50=\K[0-9.]+'||echo 0)" "$(echo "$line"|grep -oP 'p90=\K[0-9.]+'||echo 0)" "$(echo "$line"|grep -oP 'p99=\K[0-9.]+'||echo 0)" "$(echo "$line"|grep -oP 'max=\K[0-9.]+'||echo 0)" "$(echo "$line"|grep -oP 'n=\K[0-9]+'||echo 0)"
done
for g in 100 20 5; do
  st=$(gostat ./bench_go $g); t=$(GOGC=$g ./bench_go $N|grep -oP '耗时=\K[0-9.]+'); r=$(rss env GOGC=$g ./bench_go $N)
  printf '%-22s %8s %9s %8s %8s %8s %8s %7s\n' "Go GOGC=$g" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" $st
done
