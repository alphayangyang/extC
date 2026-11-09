#!/usr/bin/env python3
"""tests/gc-p0/ab.py -- 同轮交错 A/B 计时（build-lead5 vs build-rf，或同构建的不同 -D 变体）。

动机：build-rf 落地 P（按字节配额）+ B（lazy freelist）后，runtime-facts 报 sizeratio 墙钟
中位 +10.34%（Ir +6.1%），超出 G20 的 +-3% 判据；串行分开跑会把机器漂移混进差值，所以这里
在**同一轮**里交替跑 A/B（奇偶轮交换先后顺序），每形状 >= 9 reps，pin 到固定 CPU。

只测墙钟/RSS/[step]/[quota]（v2run 内核值），Ir 另用 callgrind 各跑一次（bench args，--cache-sim 关）。

用法：
  python3 tests/gc-p0/ab.py --extc-a ./build-lead5/extc --extc-b ./build-rf/extc --reps 9
  # 归因实验：同一 extc-b，只给 B 加 -D
  python3 tests/gc-p0/ab.py --extc-a ./build-rf/extc --extc-b ./build-rf/extc \
      --tag-a base --tag-b maxmark0 --extra-b "-DEXTC_RC_QUOTA_MAX_MARK=1"
"""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
V2 = os.path.join(ROOT, "bench", "gc", "v2")
sys.path.insert(0, HERE)
import g20 as G          # noqa: E402
import pause_parse as pp  # noqa: E402

ORDER = G.ORDER
SHAPES = G.SHAPES
DEFAULT_CFLAGS = "-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L"


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def med(xs):
    xs = [x for x in xs if isinstance(x, (int, float))]
    return statistics.median(xs) if xs else None


def run_variant(v2run, binp, shape, args, pin, logdir, tag, rep):
    """跑一次并解析 [step]/[histo]/[quota]；日志按 tag=rep 唯一命名，A/B 互不覆盖。"""
    os.makedirs(logdir, exist_ok=True)
    out = os.path.join(logdir, "ab_%s_%s_r%d.out" % (tag, shape, rep))
    err = os.path.join(logdir, "ab_%s_%s_r%d.err" % (tag, shape, rep))
    cmd = [v2run, "--pin", str(pin), "--out", out, "--err", err, "--", binp] + list(args)
    env = dict(os.environ)
    env["EXTC_RC_PAUSE"] = "1"
    env["EXTC_RC_STEP_CPU"] = "1"
    t0 = time.monotonic()
    r = G.run(cmd, env=env)
    tool_wall = time.monotonic() - t0
    summary = {}
    for line in (r.stdout or "").splitlines():
        if "rc=" in line:
            summary = G.parse_v2line(line)
    with open(err, "r", errors="replace") as f:
        stxt = f.read()
    pr = pp.parse_text(stxt)
    q = pr.get("quota") or {}
    return {
        "rep": rep,
        "rc": summary.get("rc", r.returncode),
        "wall_ms": summary.get("ms"),
        "rss_kb": summary.get("rss_kb"),
        "tool_wall_s": round(tool_wall, 3),
        "step_n": pr.get("step_n"),
        "step_total_us": pr.get("step_total_us"),
        "step_max_us": pr.get("step_max_us"),
        "collects": pr.get("collects"),
        "histo_counts": (pr.get("histo") or {}).get("counts"),
        "quota_hits": q.get("hits"),
        "quota_checks": q.get("checks"),
        "quota_deadline_us": q.get("deadline_us"),
        "stderr_mmax": len([ln for ln in stxt.splitlines() if ln.startswith("[mmax]")]),
        "stdout_head": "",
        "err": err,
    }


def ir_of(binp, args, cgpath):
    """callgrind 只取总 Ir（不带 --cache-sim，快）。"""
    os.makedirs(os.path.dirname(cgpath), exist_ok=True)
    r = G.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=" + cgpath,
               binp] + list(args))
    if r.returncode != 0 or not os.path.exists(cgpath):
        return None
    total = None
    with open(cgpath, "r", errors="replace") as f:
        for line in f:
            if line.startswith("summary:"):
                try:
                    total = int(line.split(":")[1].strip().split()[0])
                except (IndexError, ValueError):
                    pass
    return total


def stats(reps, key):
    xs = [x.get(key) for x in reps]
    xs = [x for x in xs if isinstance(x, (int, float))]
    if not xs:
        return None, None, None
    return med(xs), min(xs), max(xs)


def pct(a, b):
    if not a or not b:
        return None
    return (b - a) / a * 100.0


def cmd_ab(a):
    out = a.out
    for sub in ("bin", "c", "log", "cg", "ab_a", "ab_b", "ab_v"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    v2run = G.build_v2run(a.cc, out)
    res = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "root": ROOT, "pin": a.pin, "reps": a.reps, "cflags": a.cflags,
        "a": {"tag": a.tag_a, "extc": os.path.abspath(a.extc_a), "md5": md5(a.extc_a),
              "extra": a.extra_a},
        "b": {"tag": a.tag_b, "extc": os.path.abspath(a.extc_b), "md5": md5(a.extc_b),
              "extra": a.extra_b},
        "shapes": {},
    }
    print("A(%s) md5=%s extra=%s" % (a.tag_a, res["a"]["md5"][:16], a.extra_a or "-"))
    print("B(%s) md5=%s extra=%s" % (a.tag_b, res["b"]["md5"][:16], a.extra_b or "-"))
    print("reps=%d pin=%d cflags=%s" % (a.reps, a.pin, a.cflags))

    for s in ORDER:
        if a.shapes and s not in a.shapes:
            continue
        cfa = a.cflags + (" " + a.extra_a if a.extra_a else "")
        cfb = a.cflags + (" " + a.extra_b if a.extra_b else "")
        bin_a, _ = G.build_shape(a.extc_a, a.cc, cfa, s, os.path.join(out, "ab_a"))
        bin_b, _ = G.build_shape(a.extc_b, a.cc, cfb, s, os.path.join(out, "ab_b"))
        args = SHAPES[s]["args"]
        reps = {"a": [], "b": []}
        for r in range(1, a.reps + 1):
            order = ("a", "b") if r % 2 == 1 else ("b", "a")
            for tag in order:
                binp = bin_a if tag == "a" else bin_b
                lab = a.tag_a if tag == "a" else a.tag_b
                one = run_variant(v2run, binp, s, args, a.pin,
                                  os.path.join(out, "log", "ab_" + tag), lab, r)
                reps[tag].append(one)
            pair = [x["wall_ms"] for x in reps["a"][-1:]] + [x["wall_ms"] for x in reps["b"][-1:]]
            print("  %-10s r%d  a=%s  b=%s ms" % (s, r, reps["a"][-1]["wall_ms"],
                                                   reps["b"][-1]["wall_ms"]))
        row = {}
        for tag in ("a", "b"):
            wm, wmin, wmax = stats(reps[tag], "wall_ms")
            rm, rmin, rmax = stats(reps[tag], "rss_kb")
            sm, smin, smax = stats(reps[tag], "step_max_us")
            tm, _, _ = stats(reps[tag], "step_total_us")
            hits = [x.get("quota_hits") for x in reps[tag] if isinstance(x.get("quota_hits"), int)]
            over1ms = 0
            for x in reps[tag]:
                h = x.get("histo_counts") or []
                if len(h) >= 15:
                    over1ms += sum(h[11:])
            row[tag] = {
                "tag": a.tag_a if tag == "a" else a.tag_b,
                "wall_ms": {"med": wm, "min": wmin, "max": wmax},
                "rss_kb": {"med": rm, "min": rmin, "max": rmax},
                "step_max_us": {"med": sm, "min": smin, "max": smax},
                "step_total_us": {"med": tm},
                "quota_hits_sum": sum(hits) if hits else None,
                "over1ms_sum": over1ms,
                "rc_all0": all(x["rc"] == 0 for x in reps[tag]),
                "mmax_lines": sum(x.get("stderr_mmax") or 0 for x in reps[tag]),
                "reps": reps[tag],
            }
        # Ir（各一次，bench args）
        ir_a = ir_of(bin_a, args, os.path.join(out, "cg", "ab_%s_%s.out" % (a.tag_a, s)))
        ir_b = ir_of(bin_b, args, os.path.join(out, "cg", "ab_%s_%s.out" % (a.tag_b, s)))
        row["ir"] = {"a": ir_a, "b": ir_b, "delta_pct": pct(ir_a, ir_b)}
        row["wall_delta_pct"] = pct(row["a"]["wall_ms"]["med"], row["b"]["wall_ms"]["med"])
        row["rss_delta_pct"] = pct(row["a"]["rss_kb"]["med"], row["b"]["rss_kb"]["med"])
        row["step_total_delta_pct"] = pct(row["a"]["step_total_us"]["med"],
                                          row["b"]["step_total_us"]["med"])
        # one-sided: a speed-up must never be judged red (doc: after <= +3%)
        row["verdict"] = ("PASS" if row["wall_delta_pct"] is not None
                          and row["wall_delta_pct"] <= 3.0 else "RED")
        res["shapes"][s] = row
        print("  => %-10s wall a=%s b=%s delta=%s%%  Ir delta=%s%%  rss delta=%s%%  %s"
              % (s, row["a"]["wall_ms"]["med"], row["b"]["wall_ms"]["med"],
                 ("%.2f" % row["wall_delta_pct"]) if row["wall_delta_pct"] is not None else "n/a",
                 ("%.2f" % row["ir"]["delta_pct"]) if row["ir"]["delta_pct"] is not None else "n/a",
                 ("%.2f" % row["rss_delta_pct"]) if row["rss_delta_pct"] is not None else "n/a",
                 row["verdict"]))

    print()
    print("  shape      | wall med A / B        | min..max A         | min..max B         | dWall  | Ir A / B (delta)         | dRSS  | stepMax med A/B | qHits A/B | over1ms")
    for s in ORDER:
        if s not in res["shapes"]:
            continue
        r = res["shapes"][s]
        irs = ("%s / %s (%s%%)" % (r["ir"]["a"], r["ir"]["b"],
                                   ("%.2f" % r["ir"]["delta_pct"]) if r["ir"]["delta_pct"] is not None else "n/a")
               if r["ir"]["a"] else "n/a")
        print("  %-10s | %8.2f / %8.2f     | %7.1f..%7.1f   | %7.1f..%7.1f   | %+6.2f%% | %-24s | %+5.2f%% | %6s / %6s | %s / %s | %d"
              % (s, r["a"]["wall_ms"]["med"], r["b"]["wall_ms"]["med"],
                 r["a"]["wall_ms"]["min"], r["a"]["wall_ms"]["max"],
                 r["b"]["wall_ms"]["min"], r["b"]["wall_ms"]["max"],
                 r["wall_delta_pct"], irs, r["rss_delta_pct"],
                 r["a"]["step_max_us"]["med"], r["b"]["step_max_us"]["med"],
                 r["a"]["quota_hits_sum"], r["b"]["quota_hits_sum"],
                 r["a"]["over1ms_sum"] + r["b"]["over1ms_sum"]))
    with open(os.path.join(out, "ab.json"), "w") as f:
        json.dump(res, f, indent=1)
    print()
    print("JSON: " + os.path.join(out, "ab.json"))
    bad = [s for s in res["shapes"] if res["shapes"][s]["verdict"] != "PASS"]
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extc-a", default=os.path.join(ROOT, "build-lead5", "extc"))
    ap.add_argument("--extc-b", default=os.path.join(ROOT, "build-rf", "extc"))
    ap.add_argument("--tag-a", default="before")
    ap.add_argument("--tag-b", default="after")
    ap.add_argument("--extra-a", default="")
    ap.add_argument("--extra-b", default="")
    ap.add_argument("--out", default="/tmp/gc-ab")
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--pin", type=int, default=6)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--cflags", default=DEFAULT_CFLAGS)
    ap.add_argument("--shapes", nargs="*", default=[])
    a = ap.parse_args()
    return cmd_ab(a)


if __name__ == "__main__":
    sys.exit(main())
