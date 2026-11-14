#!/usr/bin/env python3
"""sample.py --- 采样一个子进程的 VmRSS / VmHWM（2 ms 粒度），输出一行 JSON。

用法: python3 sample.py --interval-ms 2 -- JSON 参数... -- CMD [ARGS...]
    python3 sample.py -- CMD [ARGS...]

输出: {"rc":0,"wall_s":0.09,"peak_rss_kb":89396,"last_rss_kb":12345,"samples":41,
       "max_delta_pct":0.4,"grew_after_quarter":1234}

为什么不用 /usr/bin/time 的 %M: 那是**峰值**；容量归还类判据要看**稳态末值**是否回落，
峰值只用来验「不许超过 3*live+slack」。两者都要，取一次运行同时拿到。
"""
import argparse, json, os, subprocess, sys, time


def read_rss(pid):
    try:
        with open("/proc/%d/status" % pid, "r") as f:
            rss = hwm = None
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    hwm = int(line.split()[1])
            return rss, hwm
    except (FileNotFoundError, ProcessLookupError):
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval-ms", type=float, default=2.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        print("sample.py: no command", file=sys.stderr)
        return 2
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    rss_series = []
    hwm = 0
    while p.poll() is None:
        rss, h = read_rss(p.pid)
        if rss is not None:
            rss_series.append(rss)
            if h and h > hwm:
                hwm = h
        time.sleep(a.interval_ms / 1000.0)
    try:
        out, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    wall = time.monotonic() - t0
    rss, h = read_rss(p.pid)
    last = rss_series[-1] if rss_series else 0
    peak = max([hwm] + rss_series) if rss_series else hwm
    q = len(rss_series) // 4
    grew = 0
    delta_pct = 0.0
    if q >= 1 and len(rss_series) >= 8:
        base = rss_series[q]
        grew = last - base
        if base > 0:
            delta_pct = 100.0 * grew / base
    rec = {"rc": p.returncode, "wall_s": round(wall, 4), "peak_rss_kb": peak,
           "last_rss_kb": last, "samples": len(rss_series),
           "grew_after_quarter_kb": grew, "max_delta_pct": round(delta_pct, 3),
           "stdout": out.decode("utf-8", "replace"),
           "stderr_tail": err.decode("utf-8", "replace")[-4000:]}
    print(json.dumps(rec))
    return 0


if __name__ == "__main__":
    sys.exit(main())