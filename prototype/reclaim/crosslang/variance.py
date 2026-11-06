#!/usr/bin/env python3
"""跨轮方差：同一配置跑 R 轮，报中位数[最小-最大]。"""
import os, re, subprocess, sys, statistics as st
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000
R = int(sys.argv[2]) if len(sys.argv) > 2 else 5

def run(cmd, env=None):
    e = dict(os.environ); e.update(env or {})
    p = subprocess.run(cmd, capture_output=True, text=True, env=e)
    return p.stdout + p.stderr

def ours(binary, shards, kind):
    env = {"EV": "1"}
    cmd = [binary, str(N), "1024", "1", "200000", "48" if kind == "cyc" else "24", str(shards)]
    ts, mx, rss_l = [], [], []
    for _ in range(R):
        out = run(cmd, env)
        m = re.search(r"耗时=\s*([0-9.]+)", out); ts.append(float(m.group(1)) if m else 0)
        d = re.search(r"max=([0-9.]+) n=([0-9]+)", out)
        mx.append(float(d.group(1)) if d else 0.0)
        q = subprocess.run(["/usr/bin/time", "-v"] + cmd, capture_output=True, text=True, env=env)
        mm = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", q.stderr)
        rss_l.append(int(mm.group(1)) // 1024 if mm else 0)
    return ts, rss_l, mx

def go(binary, gogc):
    env = {"GODEBUG": "gctrace=1", "GOGC": str(gogc)}
    ts, mx, rss_l = [], [], []
    for _ in range(R):
        out = run([binary, str(N)], env)
        m = re.search(r"耗时=([0-9.]+)", out); ts.append(float(m.group(1)) if m else 0)
        ps = [ (float(a)+float(c))*1000 for a,b,c in re.findall(r":\s*([0-9.]+)\+([0-9.]+)\+([0-9.]+)\s*ms clock", out) ]
        mx.append(max(ps) if ps else 0.0)
        q = subprocess.run(["/usr/bin/time", "-v", binary, str(N)], capture_output=True, text=True, env={"GOGC": str(gogc), "PATH": os.environ["PATH"]})
        mm = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", q.stderr)
        rss_l.append(int(mm.group(1)) // 1024 if mm else 0)
    return ts, rss_l, mx

def fmt(v):
    med = st.median(v); return f"{med:.1f}[{min(v):.1f}-{max(v):.1f}]"

for title, kind, mybin, gobin in (
    ("无环", "acy", "/tmp/final", "./bench_go"),
    ("有环", "cyc", "/tmp/stress", "./stress_go"),
):
    print(f"\n=== {title}  N={N:,}  每格 {R} 轮：中位数[最小-最大] ===")
    print(f"{'实现':<20}{'耗时ms':>22}{'峰值RSSMiB':>18}{'max停顿us':>22}")
    for k in (8, 16):
        ts, rss_l, mx = ours(mybin, k, kind)
        print(f"{'本设计 分片=' + str(k):<20}{fmt(ts):>22}{fmt(rss_l):>18}{fmt(mx):>22}")
    for g in (100, 20):
        ts, rss_l, mx = go(gobin, g)
        print(f"{'Go GOGC=' + str(g):<20}{fmt(ts):>22}{fmt(rss_l):>18}{fmt(mx):>22}")
