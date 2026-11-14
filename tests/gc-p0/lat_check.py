#!/usr/bin/env python3
# tests/gc-p0/lat_check.py —— R-C 延迟屋顶形状的判据（Lead round R-C，2026-11-14）
#
# 输入：$OUT/log/lat_rand_pause_r*.err（随机链）与 lat_seq_pause_r*.err（同 pop 数的顺序链对照）。
# 判据（每条都写清为什么是这个数、余量多大、实测值是多少）：
#   LAT-a 冷档结构性：每个 rep 的 [wbudget] pop_max <= 32（活集 166 MB > EXTC_RC_MARK_COLD_BYTES=16 MiB
#         ⇒ 每步预算被压到 32）且 [step] n >= 1e5（样本量够判尾部）。缺 [wbudget] ⇒ N/A，不是 PASS。
#   LAT-b 延迟屋顶自证：tau_rand / tau_seq >= 5.0，且 tau_seq <= 12 ns/pop。
#        tau = total_us * 1000 / pops_tot（[step] 的 total_us ÷ [wbudget] 的 pops_tot）。
#        实测（v26，5 reps）：tau_rand 92-98 ns、tau_seq 6.8-7.1 ns ⇒ 比值 13.4（余量 2.7x）；
#        tau_seq 上限 12 ns 是 L3 量级（内存层级探针 4 MB 点 21.3 ns、L3 12.7-16.5 ns）取严。
#        这条是形状自证：如果随机/顺序拉不开差距，说明这个形状已经不是延迟屋顶，判据就失去意义。
#   LAT-c 尾部率：随机链 gt64 率（[histo] gt64us ÷ n）的 **min over reps** <= 5e-4。
#        实测 1.9-2.4e-4（4/5 reps 一致）⇒ 2x 余量。用 min-over-reps 与项目既有抗噪协议一致
#        （环境抢占只会让计数值变大：同一形状顺序对照组也有 0-1 步 >1 ms）。
#   LAT-d 绝对尾部：min over reps 的 [step] over1ms == 0（实测 4/5 reps 为 0，1 rep 有 1 步 5.35 ms，
#        顺序对照组同 rep 也有 3.58 ms ⇒ 环境事件）。
#   LAT-e 对照组自证：顺序链 gt64 率 <= 1e-4（实测 2-3e-5）。
#
# **本通道不判 p99.9 <= 10 µs**：单 pop 的尾部本身到 64-256 µs（b=1 时仍 5/2.44M 步 >64 µs、max 126.7 µs），
# 这是延迟屋顶的性质而不是批大小能修的；p99.9 只作为 OBS 打印，属于 R14（无机制模型）开放项。
import argparse, glob, json, os, re, sys


def parse_err(path):
    d = {}
    for line in open(path, errors="replace"):
        if line.startswith("[step]"):
            for k, v in re.findall(r"([a-z_0-9]+)=(-?[0-9.]+)", line):
                d["step_" + k] = float(v)
        elif line.startswith("[histo]"):
            for k, v in re.findall(r"([a-z_0-9]+)=(-?[0-9]+)", line):
                d["h_" + k] = float(v)
        elif line.startswith("[wbudget]"):
            for k, v in re.findall(r"([a-z_0-9]+)=(-?[0-9]+)", line):
                d["wb_" + k] = float(v)
        elif line.startswith("[pause]"):
            for k, v in re.findall(r"([a-z_0-9]+)=(-?[0-9.]+)", line):
                d["p_" + k] = float(v)
    return d


def load(d, name):
    out = []
    for p in sorted(glob.glob(os.path.join(d, name + "_pause_r*.err"))):
        r = int(re.search(r"_r([0-9]+)\.err$", p).group(1))
        out.append((r, parse_err(p)))
    return out


def tau(d):
    """ns per pop, or None"""
    pops = d.get("wb_pops_tot", 0.0)
    tot = d.get("step_total_us", 0.0)
    if pops <= 0 or tot <= 0:
        return None
    return tot * 1000.0 / pops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--json", default="")
    ap.add_argument("--min-n", type=float, default=1e5)
    ap.add_argument("--tau-seq-max", type=float, default=12.0)
    ap.add_argument("--ratio-min", type=float, default=5.0)
    ap.add_argument("--gt64-max", type=float, default=5e-4)
    ap.add_argument("--gt64-seq-max", type=float, default=1e-4)
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.dir:
        print("lat_check: need --dir (or --selftest)")
        return 2

    rnd = load(a.dir, "lat_rand")
    seq = load(a.dir, "lat_seq")
    res = {"rand": [], "seq": [], "checks": []}
    if not rnd:
        # 缺文件不是 N/A：选了 lat 目标就必须有产物，否则是 harness 坏了（只严不放宽）。
        print("LAT-a RED no lat_rand_pause_r*.err under %s" % a.dir)
        return 1

    def row(tag, reps):
        rows = []
        for r, d in reps:
            pops = d.get("wb_pops_tot", 0.0)
            n = d.get("step_n", 0.0)
            rows.append({
                "rep": r,
                "n": n,
                "pops": pops,
                "pop_max": d.get("wb_pop_max"),
                "tau_ns": tau(d),
                "mean_step_us": (d["step_total_us"] / n) if n else None,
                "max_us": d.get("step_max_us"),
                "over1ms": d.get("step_over1ms"),
                "over256": d.get("step_over256"),
                "gt64": d.get("h_gt64us"),
                "gt64_rate": (d["h_gt64us"] / n) if n and "h_gt64us" in d else None,
                "rss_kb": d.get("rss_kb"),
                "collects": d.get("p_collects"),
            })
        return rows

    R = row("rand", rnd)
    S = row("seq", seq) if seq else []
    res["rand"], res["seq"] = R, S

    def add(cid, verdict, detail):
        res["checks"].append({"id": cid, "verdict": verdict, "detail": detail})
        print("LAT-%s %s %s" % (cid, verdict, detail))

    # LAT-a
    bad_pop = [x["rep"] for x in R if x["pop_max"] is None or x["pop_max"] > 32]
    min_n = min(x["n"] for x in R)
    if bad_pop:
        add("a", "RED", "pop_max > 32 (or missing) at reps %s; cold tier is not active" % bad_pop)
    elif min_n < a.min_n:
        add("a", "N/A", "min n=%.0f < %.0f (sample too small to judge the tail)" % (min_n, a.min_n))
    else:
        add("a", "PASS", "pop_max=%d all reps, n=%.0f-%.0f" % (R[0]["pop_max"], min_n, max(x["n"] for x in R)))

    # LAT-b
    taus = [x["tau_ns"] for x in R if x["tau_ns"]]
    taus_s = [x["tau_ns"] for x in S if x["tau_ns"]]
    if not taus or not taus_s:
        add("b", "N/A", "tau not computable (need [step] total_us and [wbudget] pops_tot); ran lat_seq?")
    else:
        tr, ts = min(taus), min(taus_s)
        ratio = tr / ts if ts else 0.0
        ok = ratio >= a.ratio_min and ts <= a.tau_seq_max
        add("b", "PASS" if ok else "RED",
            "tau_rand=%.1f ns/pop tau_seq=%.1f ns/pop ratio=%.2f (need >=%.1f and tau_seq<=%.1f)"
            % (tr, ts, ratio, a.ratio_min, a.tau_seq_max))

    # LAT-c
    rates = [x["gt64_rate"] for x in R if x["gt64_rate"] is not None]
    if not rates:
        add("c", "N/A", "no [histo] gt64us line")
    else:
        mn = min(rates)
        add("c", "PASS" if mn <= a.gt64_max else "RED",
            "min over reps gt64_rate=%.2e (limit %.1e); per-rep %s"
            % (mn, a.gt64_max, ["%.2e" % x for x in rates]))

    # LAT-d
    o1 = [x["over1ms"] for x in R if x["over1ms"] is not None]
    if not o1:
        add("d", "N/A", "no [step] over1ms")
    else:
        add("d", "PASS" if min(o1) == 0 else "RED",
            "min over reps over1ms=%.0f (per-rep %s); max_us per-rep %s"
            % (min(o1), [int(x) for x in o1], ["%.1f" % (x["max_us"] or 0) for x in R]))

    # LAT-e
    rates_s = [x["gt64_rate"] for x in S if x["gt64_rate"] is not None]
    if not rates_s:
        add("e", "N/A", "no sequential-control samples")
    else:
        mx = max(rates_s)
        add("e", "PASS" if mx <= a.gt64_seq_max else "RED",
            "max over reps seq gt64_rate=%.2e (limit %.1e)" % (mx, a.gt64_seq_max))

    # OBS（不判）
    for tag, rows in (("rand", R), ("seq", S)):
        if not rows:
            continue
        p999 = None
        gt128 = [x for x in rows if x["n"]]
        print("LAT-obs %s mean_step_us=%s tau_ns=%s max_us=%s over256=%s p99.9_BUCKET(>=128us)=%s"
              % (tag,
                 "%.2f" % (sum(x["mean_step_us"] for x in gt128 if x["mean_step_us"]) / max(1, len(gt128))),
                 "%.1f" % (sum(x["tau_ns"] for x in rows if x["tau_ns"]) / max(1, len([1 for x in rows if x["tau_ns"]]))),
                 "/".join("%.0f" % (x["max_us"] or 0) for x in rows),
                 "/".join("%.0f" % (x["over256"] or 0) for x in rows),
                 "n/a"))

    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    hard = [c for c in res["checks"] if c["verdict"] == "RED"]
    print("LAT verdict: %s (%d RED of %d checks)"
          % ("RED" if hard else "PASS", len(hard), len(res["checks"])))
    return 1 if hard else 0


def _synth(pop_max=32, n=250000, total_us=800000.0, pops=9500000, gt64=50,
           over1ms=0, max_us=90.0, over256=0, with_wb=True):
    out = ("[step] n=%.0f total_us=%.1f max_us=%.1f over256=%.0f over1ms=%.0f words_max=256 seg_max=1\n"
           % (n, total_us, max_us, over256, over1ms))
    out += "[histo] n=%.0f gt64us=%.0f over1ms=%.0f\n" % (n, gt64, over1ms)
    if with_wb:
        out += "[wbudget] pop_max=%.0f push_max=0 pops_tot=%.0f mscap_max=16384\n" % (pop_max, pops)
    return out


def _mk(d, name, reps, **kw):
    for r in range(1, reps + 1):
        open(os.path.join(d, "%s_pause_r%d.err" % (name, r)), "w").write(_synth(**kw))


def _run(d):
    import subprocess
    p = subprocess.run([sys.executable, os.path.abspath(__file__), "--dir", d],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def selftest():
    """每条判据都要能被合成数据推成 RED；否则判据没有牙（红前绿后）。"""
    import shutil, tempfile
    cases = [
        ("S1 全好", dict(rand={}, seq=dict(total_us=60000.0, gt64=10)), 0, "LAT verdict: PASS"),
        ("S2 pop_max=512", dict(rand=dict(pop_max=512), seq=dict(total_us=60000.0)), 1, "LAT-a RED"),
        ("S3 随机/顺序无差", dict(rand={}, seq={}), 1, "LAT-b RED"),
        ("S4 gt64 率 4e-3", dict(rand=dict(gt64=1000), seq=dict(total_us=60000.0)), 1, "LAT-c RED"),
        ("S5 over1ms=1", dict(rand=dict(over1ms=1), seq=dict(total_us=60000.0)), 1, "LAT-d RED"),
        ("S6 对照 gt64 率 8e-4", dict(rand={}, seq=dict(total_us=60000.0, gt64=200)), 1, "LAT-e RED"),
        ("S7 缺 [wbudget]", dict(rand=dict(with_wb=False), seq=dict(total_us=60000.0)), 1, "LAT-a RED"),
        ("S8 无产物", None, 1, "LAT-a RED"),
    ]
    bad = 0
    for tag, spec, want_rc, want_txt in cases:
        d = tempfile.mkdtemp(prefix="latst-")
        try:
            if spec is not None:
                _mk(d, "lat_rand", 3, **spec["rand"])
                _mk(d, "lat_seq", 3, **spec["seq"])
            rc, out = _run(d)
        finally:
            shutil.rmtree(d, ignore_errors=True)
        ok = (rc == want_rc) and (want_txt in out)
        print("selftest %-22s %s  (rc=%d want=%d, %r)" % (tag, "ok" if ok else "FAIL", rc, want_rc, want_txt))
        if not ok:
            bad += 1
            print("---- output ----")
            print(out)
    print("selftest: %d/%d ok" % (len(cases) - bad, len(cases)))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
