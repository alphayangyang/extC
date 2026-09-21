#!/usr/bin/env bash
# 重负载横评（extC vs C，**同算法同参数**，输出必须逐位相同 ✓）
#   bt  binary-trees（分配 + 递归）   ← arena 的 bump 分配在这里最能体现
#   rs  radix sort（内存带宽 + 数组）
#   mb  mandelbrot（浮点 + 写内存）
set -u
cd "$(dirname "$0")/../.."
mkdir -p build/heavy
mkdir -p /tmp/hvbin
for p in bt rs mb; do
    ./build/extc bench/heavy/$p.extc -o /tmp/hvbin/$p.c >/dev/null || { echo "$p: extC 编不过"; continue; }
    gcc -O2 /tmp/hvbin/$p.c -o /tmp/hvbin/${p}_extc
    gcc -O2 bench/heavy/$p.c -o /tmp/hvbin/${p}_c
    echo "== $p =="
    for v in extc c; do
        o=$( { /usr/bin/time -f "TIME %e RSS %M" /tmp/hvbin/${p}_$v; } 2>&1 )
        printf "  %-5s %-42s %s\n" "$v" "$(echo "$o" | grep -v TIME | head -1)" "$(echo "$o" | grep TIME)"
    done
done
