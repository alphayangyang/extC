#!/usr/bin/env python3
"""tests/gc-p0/sample.py —— 统一量具：墙钟 / CPU / VmHWM / VmRSS 时间序列。

为什么自己写而不用 /usr/bin/time -v：
  · 需要在进程运行期间按固定间隔读 /proc/<pid>/status 的 VmRSS/VmHWM（time -v 只给峰值）；
  · 需要把时间序列落成 CSV（G4 的证据），而不只是一个数；
  · ru_maxrss 有 ~11 MB 地板（WSL2），短进程必须以 VmHWM 为准（MEAS §2 口径）。

用法：
  python3 tests/gc-p0/sample.py --bin ./bin/x --args 1000000 --interval-ms 20 \
      --csv out/x.csv --env EXTC_RC_PAUSE=1 --timeout 120
stdout 最后一行是一个 JSON（另加 runs.jsonl 由 run.sh 负责追加）。
退出码：被测进程的退出码（trap 70 原样透传）；量具自身失败返回 2。
"""
import argparse, json, os, signal, sys, time

def read_vm(pid, key):
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return int(line.split()[1])  # kB
    except (FileNotFoundError, ProcessLookupError):
        return None
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--args", nargs="*", default=[])
    ap.add_argument("--interval-ms", type=float, default=20.0)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--stdout", default=None)
    ap.add_argument("--stderr", default=None)
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()

    env = os.environ.copy()
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    sout = open(a.stdout, "wb") if a.stdout else None
    serr = open(a.stderr, "wb") if a.stderr else None
    argv = [a.bin] + a.args
    t0 = time.monotonic()
    try:
        pid = os.posix_spawn(a.bin, argv, env,
                             file_actions=([(os.POSIX_SPAWN_DUP2, sout.fileno(), 1),
                                            (os.POSIX_SPAWN_DUP2, serr.fileno(), 2)]
                                           if sout is not None and serr is not None else []))
    except OSError as e:
        print(json.dumps({"error": str(e)}))
        return 2

    samples = []          # (t_ms, rss_kb, vmhwm_kb)
    status = None
    ru = None
    timed_out = False
    while True:
        wpid, st, rusage = os.wait4(pid, os.WNOHANG)
        t_ms = (time.monotonic() - t0) * 1000.0
        rss = read_vm(pid, "VmRSS")
        hwm = read_vm(pid, "VmHWM")
        if rss is not None or hwm is not None:
            samples.append((t_ms, rss, hwm))
        if wpid == pid:
            status, ru = st, rusage
            break
        if t_ms / 1000.0 > a.timeout:
            timed_out = True
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            wpid, st, rusage = os.wait4(pid, 0)
            status, ru = st, rusage
            break
        time.sleep(max(a.interval_ms, 1.0) / 1000.0)

    wall = time.monotonic() - t0
    if sout: sout.close()
    if serr: serr.close()
    exit_code = os.waitstatus_to_exitcode(status)
    sig = -exit_code if exit_code < 0 else 0
    peak_sampled = max((s[1] for s in samples if s[1] is not None), default=0)
    hwm_sampled = max((s[2] for s in samples if s[2] is not None), default=0)
    rec = {
        "exit": exit_code if exit_code >= 0 else 128 + sig,
        "signal": sig,
        "timed_out": timed_out,
        "wall_s": round(wall, 4),
        "user_s": round(ru.ru_utime, 4) if ru else None,
        "sys_s": round(ru.ru_stime, 4) if ru else None,
        # WSL2 上 os.posix_spawn 的 ru_maxrss 会**污染成父进程（python）的峰值**（实测 /bin/true
        # 也报 9916 kB = python 自己的 ru_maxrss）—— 所以 ru_maxrss 只作为参考字段，
        # G8/G4 一律用 /proc/<pid>/status 的 VmHWM 采样值（单调，末次采样≈峰值）。
        "maxrss_kb": ru.ru_maxrss if ru else None,
        "ru_maxrss_polluted": bool(ru and hwm_sampled and ru.ru_maxrss > hwm_sampled + 2048),
        "vmhwm_kb": max(hwm_sampled, peak_sampled),
        "peak_vmrss_kb": peak_sampled,
        "first_rss_kb": samples[0][1] if samples else None,
        "last_rss_kb": samples[-1][1] if samples else None,
        "n_samples": len(samples),
        "interval_ms": a.interval_ms,
    }
    if a.csv:
        os.makedirs(os.path.dirname(a.csv) or ".", exist_ok=True)
        with open(a.csv, "w") as f:
            f.write("t_ms,rss_kb,vmhwm_kb\n")
            for t, r, h in samples:
                f.write(f"{t:.1f},{'' if r is None else r},{'' if h is None else h}\n")
    print(json.dumps(rec))
    return rec["exit"] if rec["exit"] >= 0 else 128 + sig

if __name__ == "__main__":
    sys.exit(main())
