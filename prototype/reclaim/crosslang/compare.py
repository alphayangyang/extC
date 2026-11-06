#!/usr/bin/env python3
"""本设计 vs Go：无环 / 有环 两张表，全部指标。"""
import os, re, subprocess, sys, time

N = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000

def run(cmd, env=None, n=1):
    e = dict(os.environ); e.update(env or {})
    best = None; out = ""
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True, env=e)
        dt = (time.perf_counter() - t0) * 1000
        out = p.stdout + p.stderr
        best = dt if best is None else min(best, dt)
    return best, out

def rss(cmd, env=None):
    e = dict(os.environ); e.update(env or {})
    p = subprocess.run(["/usr/bin/time", "-v"] + cmd, capture_output=True, text=True, env=e)
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", p.stderr)
    return int(m.group(1)) // 1024 if m else 0

def pct(ps):
    ps = sorted(ps); n = len(ps)
    if not n: return (0.0, 0.0, 0.0, 0.0, 0)
    q = lambda t: ps[min(n - 1, int(n * t))]
    return (q(.5), q(.9), q(.99), ps[-1], n)

def ours(binary, shards, kind):
    env = {"EV": "1"}
    cmd = [binary, str(N), "1024", "1", "200000", "48" if kind == "cyc" else "24", str(shards)]
    dt, out = run(cmd, env, 1)
    m = re.search(r"耗时=\s*([0-9.]+)", out)
    t = float(m.group(1)) if m else 0.0
    ms = re.search(r"最大停顿=\s*([0-9.]+)", out)
    mx = float(ms.group(1)) if ms else 0.0
    d = re.search(r"p50=([0-9.]+) p90=([0-9.]+) p99=([0-9.]+) max=([0-9.]+) n=([0-9]+)", out)
    if d:
        p = (float(d.group(1)), float(d.group(2)), float(d.group(3)), float(d.group(4)), int(d.group(5)))
    else:
        p = (0.0, 0.0, 0.0, 0.0, 0)   # 一次清扫都没有 ⇒ 停顿全 0
    return t, rss(cmd, env), p

def go(binary, gogc):
    env = {"GODEBUG": "gctrace=1", "GOGC": str(gogc)}
    dt, out = run([binary, str(N)], env, 1)
    t = None
    m = re.search(r"耗时=([0-9.]+)", out)
    if m: t = float(m.group(1))
    ps = []
    for l in out.splitlines():
        mm = re.search(r":\s*([0-9.]+)\+([0-9.]+)\+([0-9.]+)\s*ms clock", l)
        if mm: ps.append((float(mm.group(1)) + float(mm.group(3))) * 1000)
    return t, rss([binary, str(N)], {"GOGC": str(gogc)}), pct(ps)

CY, AC = "cyc", "acy"
for title, kind, mybin, gobin in (
    ("无环（有界符号表 + 缓存 + 20 万项索引）", AC, "/tmp/final", "./bench_go"),
    ("有环（双向链表 + 带父指针的树 + 缓存 + 索引）", CY, "/tmp/stress", "./stress_go"),
):
    print(f"\n=== {title}  N={N:,} ===")
    print(f"{'实现':<22}{'耗时ms':>9}{'峰值RSSMiB':>12}{'p50us':>9}{'p90us':>9}{'p99us':>9}{'maxus':>9}{'GC次数':>8}")
    for k in (8, 16, 32):
        t, r, p = ours(mybin, k, kind)
        print(f"{'本设计 分片=' + str(k):<22}{t:>9.1f}{r:>12}{p[0]:>9.1f}{p[1]:>9.1f}{p[2]:>9.1f}{p[3]:>9.1f}{p[4]:>8}")
    for g in (100, 20, 5):
        t, r, p = go(gobin, g)
        print(f"{'Go GOGC=' + str(g):<22}{t:>9.1f}{r:>12}{p[0]:>9.1f}{p[1]:>9.1f}{p[2]:>9.1f}{p[3]:>9.1f}{p[4]:>8}")
