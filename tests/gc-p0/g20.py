#!/usr/bin/env python3
"""tests/gc-p0/g20.py —— G20：P0-B（惰性 freelist）的停顿收益 + 墙钟/RSS 不回归。

口径（Lead m03207，2026-11-09）：
- 形状：bench/gc/v2 四形状，bench 档参数（promote 500000 20261013 / sizeratio 2000000 20261013 /
  chase 1048576 20261013 4000000 / cycleratio 400000 20261013 100）。
- 开关：运行期 EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1（[step]/[histo] 走线程 CPU 口径，避免抢占污染）。
- 量具：用 bench/gc/v2/shim.c 现场编一个 v2run（fork+wait4 取内核值：墙钟 ms、VmHWM rss_kb、minflt），
  **不依赖 /tmp/v2/v2run**；--pin 固定 CPU。
- 判据：
    promote    : [step] total_us 较 before 降 >=10%（硬）。
    sizeratio  : [step] total_us 降 >=40%（硬）。
    cycleratio : [step] total_us 不回归（after <= before*1.03，硬）。
    promote 的 [step] max：**median across reps <= 60 us** —— 单次 max 不作判据
                 （WSL2 时钟噪声地板 78–181 us，见 MEASUREMENTS-GC-P0.md §1.3）；
                 原型实测 160.4 -> 57.5 us，故 median 口径仍能区分 P0-B 前后的台阶。
    四形状     : 墙钟中位、RSS 中位相对 before 均在 ±3% 内（硬）。
- 原始 stderr 落 <out>/log/g20_<shape>_r<k>.err（[step]/[histo]/[work]/[pause] 全在里面）；
  汇总写 <out>/g20.json；每个 rep 的子进程 stdout 落 <out>/log/g20_<shape>_r<k>.out。

用法：
  python3 tests/gc-p0/g20.py run --extc ./build-lead5/extc --out tests/gc-p0/out/p0-before --tag before
  python3 tests/gc-p0/g20.py run --extc ./build-rf/extc     --out tests/gc-p0/out/p0-after  --tag after
  python3 tests/gc-p0/g20.py compare tests/gc-p0/out/p0-before/g20.json tests/gc-p0/out/p0-after/g20.json
  python3 tests/gc-p0/g20.py --selftest

串行纪律（Lead 2026-11-09）：计时必须串行，跑前确认无其他 agent 在跑程序；本脚本每次只跑一个进程。
"""
import argparse, hashlib, json, os, shutil, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # tests/gc-p0 -> repo root
V2 = os.path.join(ROOT, "bench", "gc", "v2")

SHAPES = {
    "promote":    {"args": ["500000", "20261013"],                 "allocs": 4550000, "mib": 109.2},
    "sizeratio":  {"args": ["2000000", "20261013"],                "allocs": 2000000, "mib": 160.0},
    "chase":      {"args": ["1048576", "20261013", "4000000"],     "allocs": 1111076, "mib": 53.3, "steps": 4000000},
    "cycleratio": {"args": ["400000", "20261013", "100"],          "allocs": 1821441, "mib": 43.2},
}
ORDER = ["promote", "sizeratio", "chase", "cycleratio"]


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, **kw):
    kw.setdefault("timeout", 900)
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build_v2run(cc, outdir):
    """现场编 bench/gc/v2/shim.c -> <outdir>/bin/v2run（不依赖 /tmp/v2）。"""
    binp = os.path.join(outdir, "bin", "v2run")
    os.makedirs(os.path.dirname(binp), exist_ok=True)
    r = run([cc, "-O2", "-o", binp, os.path.join(V2, "shim.c")])
    if r.returncode != 0:
        raise SystemExit("FAIL cc v2run: " + (r.stderr or r.stdout)[-800:])
    return binp


def build_shape(extc, cc, cflags, shape, outdir):
    cdir = os.path.join(outdir, "c")
    bdir = os.path.join(outdir, "bin")
    os.makedirs(cdir, exist_ok=True); os.makedirs(bdir, exist_ok=True)
    cpath = os.path.join(cdir, "g20_%s.c" % shape)
    binp = os.path.join(bdir, "g20_%s" % shape)
    r = run([extc, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"),
             os.path.join(V2, "src", shape + ".extc"), "-o", cpath])
    if r.returncode != 0:
        raise SystemExit("FAIL extc %s: %s" % (shape, (r.stderr or r.stdout)[-800:]))
    r = run([cc] + cflags.split() + ["-o", binp, cpath])
    if r.returncode != 0:
        raise SystemExit("FAIL cc %s: %s" % (shape, (r.stderr or r.stdout)[-800:]))
    return binp, cpath


def parse_v2line(line):
    """v2run 输出：ms=... rss_kb=... utime_ms=... stime_ms=... minflt=... majflt=... nvcsw=... nivcsw=... rc=..."""
    d = {}
    for tok in line.split():
        if "=" not in tok:
            continue
        k, _, v = tok.partition("=")
        try:
            d[k] = float(v) if "." in v else int(v)
        except ValueError:
            d[k] = v
    return d


def run_one(v2run, binp, shape, args, pin, logdir, rep):
    out = os.path.join(logdir, "g20_%s_r%d.out" % (shape, rep))
    err = os.path.join(logdir, "g20_%s_r%d.err" % (shape, rep))
    cmd = [v2run, "--pin", str(pin), "--out", out, "--err", err, "--", binp] + args
    # 插桩开关必须在这里注入（v2run 用 execvp，继承环境）：[step]/[histo]/[top]/[work] 全靠它，
    # STEP_CPU=1 让 [step] 走线程 CPU 口径（WSL2 墙钟会被抢占污染，见 MEASUREMENTS-GC-P0.md §1.3）。
    env = dict(os.environ)
    env["EXTC_RC_PAUSE"] = "1"
    env["EXTC_RC_STEP_CPU"] = "1"
    t0 = time.monotonic()
    r = run(cmd, env=env)
    wall = time.monotonic() - t0
    summary = {}
    for line in (r.stdout or "").splitlines():
        if "rc=" in line:
            summary = parse_v2line(line)
    rc = summary.get("rc")
    if rc is None:
        rc = r.returncode
    with open(err, "r", errors="replace") as f:
        stxt = f.read()
    sys.path.insert(0, HERE)
    import pause_parse as pp
    pr = pp.parse_text(stxt)
    extra = [ln for ln in stxt.splitlines() if ln.startswith("[") is False and ln.strip()]
    mmax_lines = [ln for ln in stxt.splitlines() if ln.startswith("[mmax]")]
    sout = ""
    if os.path.exists(out):
        with open(out, "r", errors="replace") as f:
            sout = f.read()
    return {
        "rep": rep,
        "rc": rc,
        "wall_ms": summary.get("ms"),
        "rss_kb": summary.get("rss_kb"),
        "utime_ms": summary.get("utime_ms"),
        "stime_ms": summary.get("stime_ms"),
        "minflt": summary.get("minflt"),
        "majflt": summary.get("majflt"),
        "nvcsw": summary.get("nvcsw"),
        "tool_wall_s": round(wall, 3),
        "step_total_us": pr.get("step_total_us"),
        "step_max_us": pr.get("step_max_us"),
        "step_n": pr.get("step_n"),
        "collects": pr.get("collects"),
        "histo_counts": (pr.get("histo") or {}).get("counts"),
        "work": pr.get("work") or {},
        "stderr_bytes": len(stxt.encode()),
        "stderr_mmax_lines": len(mmax_lines),
        "stderr_extra_lines": len(extra),
        "stdout_sha256": hashlib.sha256(sout.encode()).hexdigest(),
        "stdout_head": sout.splitlines()[0] if sout else "",
    }


def med(xs):
    xs = [x for x in xs if isinstance(x, (int, float))]
    return statistics.median(xs) if xs else None


def cmd_run(a):
    outdir = a.out
    for sub in ("bin", "c", "log"):
        os.makedirs(os.path.join(outdir, sub), exist_ok=True)
    extc = a.extc
    if not os.path.exists(extc):
        raise SystemExit("FATAL: no extc at " + extc)
    v2run = build_v2run(a.cc, outdir)
    cmap = {}
    for s in ORDER:
        cmap[s] = build_shape(extc, a.cc, a.cflags, s, outdir)
    res = {"tag": a.tag, "extc": os.path.abspath(extc), "extc_md5": md5(extc),
           "cflags": a.cflags, "pin": a.pin, "reps": a.reps,
           "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "root": ROOT, "shapes": {}}
    for s in ORDER:
        binp, _c = cmap[s]
        reps = []
        for r in range(1, a.reps + 1):
            one = run_one(v2run, binp, s, SHAPES[s]["args"], a.pin, os.path.join(outdir, "log"), r)
            reps.append(one)
            print("  %-10s r%d rc=%s wall=%s ms rss=%s kB step_total=%s us step_max=%s us"
                  % (s, r, one["rc"], one["wall_ms"], one["rss_kb"], one["step_total_us"], one["step_max_us"]))
        res["shapes"][s] = {
            "args": SHAPES[s]["args"],
            "allocs": SHAPES[s]["allocs"], "mib": SHAPES[s]["mib"], "steps": SHAPES[s].get("steps"),
            "reps": reps,
            "median": {
                "wall_ms": med([x["wall_ms"] for x in reps]),
                "rss_kb": med([x["rss_kb"] for x in reps]),
                "minflt": med([x["minflt"] for x in reps]),
                "step_total_us": med([x["step_total_us"] for x in reps]),
                "step_max_us": med([x["step_max_us"] for x in reps]),
                "step_max_us_worst": max([x["step_max_us"] for x in reps
                                         if isinstance(x["step_max_us"], (int, float))] or [None]),
                "step_n": med([x["step_n"] for x in reps]),
                "collects": med([x["collects"] for x in reps]),
            },
            "stdout_sha256": sorted({x["stdout_sha256"] for x in reps}),
            "stderr_mmax_lines": max([x["stderr_mmax_lines"] for x in reps] or [0]),
            "stderr_bytes_max": max([x["stderr_bytes"] for x in reps] or [0]),
            "stderr_extra_lines": max([x["stderr_extra_lines"] for x in reps] or [0]),
            "rc_all": [x["rc"] for x in reps],
        }
        st = res["shapes"][s]
        print("  %-10s median wall=%s ms rss=%s kB step_total=%s us step_max=%s us collects=%s"
              % (s, st["median"]["wall_ms"], st["median"]["rss_kb"],
                 st["median"]["step_total_us"], st["median"]["step_max_us"], st["median"]["collects"]))
    path = os.path.join(outdir, "g20.json")
    with open(path, "w") as f:
        json.dump(res, f, indent=1)
    print("G20 %s: wrote %s (extc md5 %s, reps=%d, pin=%s)"
          % (a.tag, path, res["extc_md5"], a.reps, a.pin))
    return 0


def _sh(r, key):
    return (r.get("shapes", {}).get(key, {}) or {}).get("median", {}).get(
        {"promote": "wall_ms", "sizeratio": "wall_ms"}.get(key, "wall_ms"))


def compare(a, b):
    """a=before, b=after。返回 (lines, ok)。"""
    lines, ok = [], True
    lines.append("G20 compare: before=%s (%s)  after=%s (%s)"
                 % (a.get("tag"), a.get("extc_md5"), b.get("tag"), b.get("extc_md5")))
    lines.append("  reps: before=%d after=%d  pin=%s  cflags=%s"
                 % (a.get("reps"), b.get("reps"), a.get("pin"), a.get("cflags")))
    tbl = []
    for s in ORDER:
        sb = a["shapes"].get(s, {}); sa = b["shapes"].get(s, {})
        mb = sb.get("median", {}); ma = sa.get("median", {})

        def d(k, pct=False):
            x, y = mb.get(k), ma.get(k)
            if not isinstance(x, (int, float)) or not isinstance(y, (int, float)) or x == 0:
                return None
            return (y - x) / x * 100.0 if pct else (y - x)

        tw, tr = d("wall_ms", True), d("rss_kb", True)
        tt = d("step_total_us", True)
        mxb, mxa = mb.get("step_max_us"), ma.get("step_max_us")
        tbl.append((s, mb.get("wall_ms"), ma.get("wall_ms"), tw, mb.get("rss_kb"), ma.get("rss_kb"), tr,
                    mb.get("step_total_us"), ma.get("step_total_us"), tt,
                    mxb, mxa, mb.get("step_max_us_worst"), ma.get("step_max_us_worst")))
    lines.append("  %-10s %10s %10s %8s | %9s %9s %8s | %12s %12s %8s | %8s %8s"
                 % ("shape", "wall_b", "wall_a", "d%", "rss_b", "rss_a", "d%",
                    "stepTot_b", "stepTot_a", "d%", "max_b", "max_a"))
    for t in tbl:
        s = t[0]
        def f(x, nd=1):
            return "-" if x is None else (("%.*f" % (nd, x)) if isinstance(x, float) else str(x))
        def pc(x):
            return "-" if x is None else "%+.1f%%" % x
        lines.append("  %-10s %10s %10s %8s | %9s %9s %8s | %12s %12s %8s | %8s %8s"
                     % (s, f(t[1]), f(t[2]), pc(t[3]), f(t[4], 0), f(t[5], 0), pc(t[6]),
                        f(t[7]), f(t[8]), pc(t[9]), f(t[10]), f(t[11])))
    # x 轴判据
    def get(s, k):
        return b["shapes"].get(s, {}).get("median", {}).get(k)
    def gets(s, k):
        return a["shapes"].get(s, {}).get("median", {}).get(k)

    def judge(name, cond, detail):
        nonlocal ok
        ok = ok and cond
        lines.append("  [%s] %-9s %s" % ("PASS" if cond else "RED", name, detail))

    tp_b, tp_a = gets("promote", "step_total_us"), get("promote", "step_total_us")
    if isinstance(tp_b, (int, float)) and isinstance(tp_a, (int, float)) and tp_b > 0:
        judge("G20P0-promote-total", tp_a <= tp_b * 0.90,
              "step_total %s -> %s us (%.1f%%，判据 <=-10%%)" % (tp_b, tp_a, (tp_a - tp_b) / tp_b * 100))
    else:
        judge("G20P0-promote-total", False, "缺 [step] total（是否 PAUSE=1/STEP_CPU=1？）")
    sB, sA = gets("sizeratio", "step_total_us"), get("sizeratio", "step_total_us")
    if isinstance(sB, (int, float)) and isinstance(sA, (int, float)) and sB > 0:
        judge("G20P0-sizeratio-total", sA <= sB * 0.60,
              "step_total %s -> %s us (%.1f%%，判据 <=-40%%)" % (sB, sA, (sA - sB) / sB * 100))
    else:
        judge("G20P0-sizeratio-total", False, "缺 [step] total")
    cB, cA = gets("cycleratio", "step_total_us"), get("cycleratio", "step_total_us")
    if isinstance(cB, (int, float)) and isinstance(cA, (int, float)) and cB > 0:
        judge("G20P0-cycleratio-noreg", cA <= cB * 1.03,
              "step_total %s -> %s us (%.1f%%，判据 <=+3%%)" % (cB, cA, (cA - cB) / cB * 100))
    else:
        judge("G20P0-cycleratio-noreg", False, "缺 [step] total")
    # 停顿 max：median 口径（单次 max 不作判据，见文件头）
    mp = get("promote", "step_max_us")
    judge("G20-promote-max", isinstance(mp, (int, float)) and mp <= 60.0,
          "promote step max median=%s us（观察口径，判据 <=60；单次 worst=%s us 不作判据）"
          % (mp, get("promote", "step_max_us_worst")))
    # 墙钟/RSS ±3%
    for s in ORDER:
        for name, key in (("wall", "wall_ms"), ("rss", "rss_kb")):
            x, y = gets(s, key), get(s, key)
            if isinstance(x, (int, float)) and isinstance(y, (int, float)) and x != 0:
                dd = (y - x) / x * 100
                judge("G20-%s-%s" % (s, name), dd <= 3.0,
                      "%s %s -> %s (%+.1f%%，判据 <=+3%%：只罚变慢，不罚变快)" % (key, x, y, dd))
            else:
                judge("G20-%s-%s" % (s, name), False, "缺 %s" % key)
    # stdout 一致
    for s in ORDER:
        sb = set(a["shapes"].get(s, {}).get("stdout_sha256", []))
        sa = set(b["shapes"].get(s, {}).get("stdout_sha256", []))
        judge("G20-%s-stdout" % s, bool(sb) and sb == sa,
              "before=%s after=%s" % ([x[:8] for x in sb], [x[:8] for x in sa]))
    lines.append("  ⇒ G20 %s" % ("PASS" if ok else "RED"))
    return lines, ok


def cmd_compare(a):
    before = json.load(open(a.before))
    after = json.load(open(a.after))
    lines, ok = compare(before, after)
    print("\n".join(lines))
    return 0 if ok else 1


def _selftest():
    """判据逻辑自测：构造 before/after 两套 JSON，验证三条收益判据与 ±3%% 边界。"""
    def mk(tag, totals, maxs, walls, rss, sha="x"):
        base = {}
        for s in ORDER:
            base[s] = {"args": SHAPES[s]["args"], "median": {
                "wall_ms": walls.get(s), "rss_kb": rss.get(s), "minflt": 0,
                "step_total_us": totals.get(s), "step_max_us": maxs.get(s),
                "step_max_us_worst": maxs.get(s), "step_n": 1000, "collects": 1},
                "stdout_sha256": [sha]}
        return {"tag": tag, "extc_md5": tag + "-md5", "reps": 3, "pin": 6, "cflags": "-O2", "shapes": base}
    b = mk("before", {"promote": 17752, "sizeratio": 2581, "cycleratio": 471.8, "chase": 5627},
           {"promote": 160.4, "sizeratio": 51.8, "cycleratio": 39.0, "chase": 100},
           {"promote": 40.8, "sizeratio": 23.4, "cycleratio": 15.2, "chase": 173.2},
           {"promote": 6492, "sizeratio": 3300, "cycleratio": 3036, "chase": 73692})
    good = mk("after", {"promote": 15528, "sizeratio": 1247, "cycleratio": 432, "chase": 5627},
              {"promote": 57.5, "sizeratio": 9.5, "cycleratio": 16.9, "chase": 100},
              {"promote": 41.2, "sizeratio": 23.6, "cycleratio": 15.3, "chase": 174.0},
              {"promote": 6500, "sizeratio": 3310, "cycleratio": 3040, "chase": 73700})
    bad = mk("bad", {"promote": 17752, "sizeratio": 2581, "cycleratio": 471.8, "chase": 5627},
             {"promote": 160.4, "sizeratio": 51.8, "cycleratio": 39.0, "chase": 100},
             {"promote": 40.8, "sizeratio": 23.4, "cycleratio": 15.2, "chase": 173.2},
             {"promote": 6492, "sizeratio": 3300, "cycleratio": 3036, "chase": 73692})
    l1, ok1 = compare(b, good)
    l2, ok2 = compare(b, bad)
    fails = []
    if not ok1:
        fails.append("good 未被判 PASS")
    if ok2:
        fails.append("bad 未被判 RED")
    for ln in l1:
        print(ln)
    print("  good ⇒ %s；bad ⇒ %s" % ("PASS" if ok1 else "RED", "PASS" if ok2 else "RED"))
    if fails:
        print("  ⇒ SELFTEST FAIL: " + "; ".join(fails))
        return 1
    print("  ⇒ SELFTEST PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(description="G20: P0-B 停顿收益 + 墙钟/RSS 不回归")
    sub = ap.add_subparsers(dest="cmd")
    pr = sub.add_parser("run")
    pr.add_argument("--extc", required=True)
    pr.add_argument("--cc", default="cc")
    pr.add_argument("--cflags", default="-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L")
    pr.add_argument("--out", required=True)
    pr.add_argument("--reps", type=int, default=5)
    pr.add_argument("--pin", type=int, default=6)
    pr.add_argument("--tag", default="run")
    pc = sub.add_parser("compare")
    pc.add_argument("before"); pc.add_argument("after")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if a.cmd == "run":
        return cmd_run(a)
    if a.cmd == "compare":
        return cmd_compare(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
