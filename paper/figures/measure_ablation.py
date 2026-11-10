#!/usr/bin/env python3
"""Mechanism ablation for the reclamation runtime.

Every variant is the SAME generated C compiled with a different -D constant, or the SAME
.extc source compiled with EXTC_RECLAIM=0 (which removes the runtime entirely).  Nothing
else changes, so a difference is attributable to the mechanism named by the flag.

Runs are interleaved (all variants once per round, order reversed on odd rounds), pinned
to one core, and peak RSS comes from /usr/bin/time -f %M.  The stdout of every variant is
compared byte for byte against the default build.

Usage: python3 paper/figures/measure_ablation.py [--reps 7] [--shapes promote,chase]
       [--out paper/data/ablate.tsv] [--fast]
"""
import argparse, os, re, shutil, statistics, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTC = os.environ.get("EXTC", os.path.join(ROOT, "build", "extc"))
PIN = os.environ.get("PIN", "6")

SHAPES = {
    "promote":    ["500000", "20261013"],
    "sizeratio":  ["2000000", "20261013"],
    "chase":      ["1048576", "20261013", "4000000"],
    "cycleratio": ["400000", "20261013", "100"],
}

# variant -> -D flags on the generated C (empty = artifact default)
VARIANTS = {
    "def":      [],
    "b17":      ["EXTC_RC_SLAB_BITS=17"],
    "b20":      ["EXTC_RC_SLAB_BITS=20"],
    "chunk1k":  ["EXTC_RC_SCAN_CHUNK=1024"],
    "chunk1m":  ["EXTC_RC_SCAN_CHUNK=1048576"],
    "seg0":     ["EXTC_RC_SEG_DEADLINE_US=0"],
    "segchk64": ["EXTC_RC_SEG_CHECK_WORDS=64"],
    "nofin":    ["EXTC_RC_SWEEP_FINISH_MAX=0"],
}

def gen_c(shape, out, reclaim=True):
    env = dict(os.environ)
    if not reclaim:
        env["EXTC_RECLAIM"] = "0"
    cmd = [EXTC, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"),
           os.path.join(ROOT, "bench", "gc", "v2", "src", shape + ".extc"), "-o", out]
    subprocess.run(cmd, check=True, env=env, capture_output=True)

def build(shape, variant, tmp):
    c = os.path.join(tmp, "%s.%s.c" % (shape, variant))
    if variant == "off":
        gen_c(shape, c, reclaim=False)
    else:
        gen_c(shape, c, reclaim=True)
    exe = os.path.join(tmp, "%s.%s.bin" % (shape, variant))
    flags = ["-O2", "-D_POSIX_C_SOURCE=200809L"] + ["-D" + f for f in VARIANTS.get(variant, [])]
    subprocess.run(["cc"] + flags + [c, "-o", exe, "-lm"], check=True, capture_output=True)
    return exe

def run_once(exe, args, tmp):
    tfile = os.path.join(tmp, "rss.txt")
    prefix = ["taskset", "-c", PIN] if shutil.which("taskset") else []
    with open(tfile, "wb") as ferr:
        t0 = time.perf_counter()
        p = subprocess.run(prefix + ["/usr/bin/time", "-f", "%M", "-o", "/dev/stderr", exe] + args,
                           stdout=subprocess.PIPE, stderr=ferr)
        ms = (time.perf_counter() - t0) * 1000.0
    with open(tfile) as f:
        rss = int(f.read().strip().splitlines()[-1])
    return ms, rss, p.stdout

def pause_run(exe, args, tmp):
    env = dict(os.environ)
    env["EXTC_RC_PAUSE"] = "1"
    env["EXTC_RC_SLOWSTEP"] = "0.001"
    prefix = ["taskset", "-c", PIN] if shutil.which("taskset") else []
    p = subprocess.run(prefix + [exe] + args, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    durs = sorted(float(m) for m in re.findall(r"dur_us=([0-9.]+)", p.stderr.decode("utf-8", "replace")))
    if not durs:
        return 0, 0.0, 0.0, 0.0, 0.0, 0, 0, 0.0
    def q(f):
        return durs[min(len(durs) - 1, int(f * len(durs)))]
    gt50 = sum(1 for d in durs if d > 50.0)
    gt1000 = sum(1 for d in durs if d > 1000.0)
    return len(durs), q(0.5), q(0.99), q(0.999), durs[-1], gt50, gt1000, sum(durs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--shapes", default="promote,sizeratio,chase,cycleratio")
    ap.add_argument("--out", default=os.path.join(ROOT, "paper", "data", "ablate.tsv"))
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--pause", action="store_true", help="measure the step-pause distribution instead of wall clock")
    args = ap.parse_args()
    shapes = [s for s in args.shapes.split(",") if s]
    reps = 2 if args.fast else args.reps
    variants = ["def", "off"] + [v for v in VARIANTS if v != "def"]
    if args.fast:
        variants = ["def", "off", "chunk1m"]
    tmp = tempfile.mkdtemp(prefix="extc-ablate-")
    if args.pause:
        rows = []
        for shape in shapes:
            exes = {v: build(shape, v, tmp) for v in variants}
            for v in variants:
                rows.append((shape, v) + pause_run(exes[v], SHAPES[shape], tmp))
            print("%-10s pause done" % shape, flush=True)
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w") as f:
            f.write("shape\tvariant\tn\tp50\tp99\tp999\tmax\tgt50\tgt1000\ttotal_us\n")
            for r in rows:
                f.write("%s\t%s\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%d\t%d\t%.1f\n" % r)
        print("wrote", args.out)
        return
    rows = []
    for shape in shapes:
        args_s = SHAPES[shape]
        exes = {v: build(shape, v, tmp) for v in variants}
        outs = {}
        for v in variants:
            outs[v] = run_once(exes[v], args_s, tmp)[2]
        same = {v: int(outs[v] == outs["def"]) for v in variants}
        acc = {v: [] for v in variants}
        rssv = {v: 0 for v in variants}
        for r in range(reps):
            order = variants if r % 2 == 0 else list(reversed(variants))
            for v in order:
                ms, rss, _ = run_once(exes[v], args_s, tmp)
                acc[v].append(ms)
                rssv[v] = max(rssv[v], rss)
        for v in variants:
            rows.append((shape, v, min(acc[v]), statistics.median(acc[v]), rssv[v], same[v], reps))
        print("%-10s done" % shape, flush=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("shape\tvariant\tmin_ms\tmed_ms\trss_kb\tsame\tn\n")
        for r in rows:
            f.write("%s\t%s\t%.2f\t%.2f\t%d\t%d\t%d\n" % r)
    print("wrote", args.out)

if __name__ == "__main__":
    main()