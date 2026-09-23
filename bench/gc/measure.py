#!/usr/bin/env python3
"""测量一个二进制/命令：best-of-N 墙钟时间 + 峰值 RSS。

口径与 bench/oi/run.sh 一致（时间 = best of N，RSS = /usr/bin/time 的 Maximum resident set size），
因为它已经是这个仓库的横评口径 ✓
"""
import subprocess, sys, time, os, resource, statistics

def run_once(cmd, stdin_file=None):
    t0 = time.perf_counter()
    fin = open(stdin_file, "rb") if stdin_file else subprocess.DEVNULL
    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    p = subprocess.run(cmd, stdin=fin, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    after = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if stdin_file: fin.close()
    return time.perf_counter() - t0, max(after - before, 0) / 1024.0, p.returncode

def measure(cmd, n=5, stdin_file=None):
    ts, rss, rc = [], 0, 0
    for _ in range(n):
        t, m, rc = run_once(cmd, stdin_file)
        ts.append(t)
        rss = max(rss, m)
    return min(ts), rss, rc

if __name__ == "__main__":
    n = int(os.environ.get("N", "5"))
    stdin_file = os.environ.get("STDIN_FILE") or None
    cmd = sys.argv[1:]
    t, m, rc = measure(cmd, n, stdin_file)
    print(f"{t*1000:.1f}\t{m:.1f}\t{rc}")
