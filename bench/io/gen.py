#!/usr/bin/env python3
"""生成横评数据（这个脚本本身不参与计时 ✓）。

形状：N 行，每行两个整数（`a b\n`），和大多数"竞赛/日志"数据一个样子 ✓
同时给出**字节数**，好让每种语言都能算出 MB/s ✓
"""
import sys, random
n = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
out = sys.argv[2] if len(sys.argv) > 2 else "bench/io/data/big.txt"
random.seed(42)
with open(out, "w") as f:
    for i in range(n):
        f.write("%d %d\n" % (random.randrange(1, 100000), random.randrange(1, 100000)))
print("wrote", out, n, "lines")
