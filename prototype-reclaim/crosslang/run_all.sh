#!/usr/bin/env bash
# 最终大横评：同一份功能（有界符号表 64×≤8 + 1024 槽缓存 + 20 万项跨区索引 + 淘汰 + 查询）
set -uo pipefail
N="${1:-2000000}"; cd "$(dirname "$0")"
rss() { /usr/bin/time -v "$@" 2>&1 >/dev/null | awk -F': ' '/Maximum resident/{print $2}'; }
b3() { local b=""; for _ in 1 2 3; do local t; t=$("$@" 2>/dev/null|head -1|grep -oP '耗时=\s*\K[0-9.]+'); \
  [ -n "$t" ] && b=$(python3 -c "v=[float(x) for x in ['${b}','${t}'] if x.strip()];print(min(v))"); done; printf '%s' "$b"; }
row() { printf '%-24s %9s %11s %13s\n' "$1" "$2" "$3" "$4"; }
printf '%-24s %9s %11s %13s\n' 实现 耗时ms 峰值RSS_MiB GC最大停顿us

t=$(b3 ./bench_manual $N); r=$(rss ./bench_manual $N); row "C(手写引用计数)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "0"
t=$(b3 ./bench_cpp $N);    r=$(rss ./bench_cpp $N);    row "C++(shared_ptr RAII)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "0"
t=$(b3 ./bench_rc $N);     r=$(rss ./bench_rc $N);     row "Rust(Rc,标准写法)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "0"
t=$(b3 ./bench_rust $N);   r=$(rss ./bench_rust $N);   row "Rust(索引arena,不回收)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "0"
for k in 4 8 16; do
  t=$(b3 env EV=1 /tmp/final $N 1024 1 200000 24 $k); r=$(rss env EV=1 /tmp/final $N 1024 1 200000 24 $k)
  p=$(EV=1 /tmp/final $N 1024 1 200000 24 $k 2>/dev/null|head -1|grep -oP '最大停顿=\s*\K[0-9.]+')
  row "C(本设计) 分片=$k" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "$p"
done
t=$(b3 python3 bench.py $N); r=$(rss python3 bench.py $N)
p=$(python3 bench.py $N 2>/dev/null|grep -oP '环GC最大停顿us=\K[0-9.]+')
row "Python(CPython 3.14)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "$p"
t=$(b3 ./bench_go $N); r=$(rss ./bench_go $N)
GODEBUG=gctrace=1 ./bench_go $N 2>/tmp/gg.log >/dev/null
row "Go 1.26" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "$(grep -oP '\+\K[0-9.]+(?= ms clock)' /tmp/gg.log|sort -g|tail -1|awk '{printf "%.1f",$1*1000}')"
JF=(-Xms256m -Xmx256m -XX:+UseG1GC -XX:MaxGCPauseMillis=5); java "${JF[@]}" Bench 200000 >/dev/null 2>&1
t=$(b3 java "${JF[@]}" Bench $N); r=$(rss java "${JF[@]}" Bench $N)
java "${JF[@]}" -Xlog:gc:/tmp/gc.log Bench $N >/dev/null 2>&1
row "Java 25 (G1)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "$(grep -oP '(?<= )[0-9.]+(?=ms$)' /tmp/gc.log|sort -g|tail -1|awk '{printf "%.1f",$1*1000}')"
t=$(b3 node bench.js $N); r=$(rss node bench.js $N)
node --trace-gc bench.js $N >/tmp/gn.log 2>&1
row "Node 24 (V8)" "$t" "$(python3 -c "print(f'{$r/1024:.0f}')")" "$(grep -oP '[0-9.]+(?= / )' /tmp/gn.log|sort -g|tail -1|awk '{printf "%.1f",$1*1000}')"
