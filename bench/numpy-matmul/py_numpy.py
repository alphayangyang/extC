#!/usr/bin/env python3
"""Python + numpy 做矩阵乘法（基准的一根柱子）。打印一行：n / 最好一次的时间 / 峰值 RSS。"""
import resource
import sys
import time

import numpy as np


def main() -> int:
    n = int(sys.argv[1])
    reps = int(sys.argv[2])
    a = np.ones((n, n))
    b = np.ones((n, n))
    c = np.zeros((n, n))
    np.matmul(a, b, out=c)                      # 预热（含第一次进 BLAS 的代价）
    best = float("inf")
    for _ in range(3):
        t0 = time.perf_counter()
        for _ in range(reps):
            np.matmul(a, b, out=c)
        best = min(best, (time.perf_counter() - t0) * 1000.0 / reps)
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss      # Linux: KB
    print(f"n={n} best_ms={best:.6f} peak_rss_kb={rss}")
    return 0


sys.exit(main())
