#!/usr/bin/env python3
"""Boundedness: a loop whose live set does not grow, run for 3 orders of magnitude of rounds.

Each round allocates one node and stores it into the single long-lived slot, so the previous
node becomes garbage in the same round.  Peak RSS of the reclaiming build must not depend on
the number of rounds; with reclamation off the same program accumulates every node.

Usage: python3 paper/figures/measure_bounded.py [--out paper/data/bounded.tsv]
"""
import argparse, os, shutil, statistics, subprocess, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTC = os.environ.get("EXTC", os.path.join(ROOT, "build", "extc"))
PIN = os.environ.get("PIN", "6")
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bounded.extc")
ROUNDS = ["125000", "1000000", "8000000"]
REPS = 3

def generate(out, reclaim):
    env = dict(os.environ)
    if not reclaim:
        env["EXTC_RECLAIM"] = "0"
    subprocess.run([EXTC, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"), SRC, "-o", out],
                   check=True, env=env, capture_output=True)

def build(tmp, variant):
    c = os.path.join(tmp, variant + ".c")
    generate(c, reclaim=(variant == "def"))
    exe = os.path.join(tmp, variant + ".bin")
    subprocess.run(["cc", "-O2", "-D_POSIX_C_SOURCE=200809L", c, "-o", exe], check=True,
                   capture_output=True)
    return exe

def run_once(exe, rounds, tmp):
    tfile = os.path.join(tmp, "rss.txt")
    prefix = ["taskset", "-c", PIN] if shutil.which("taskset") else []
    t0 = time.perf_counter()
    p = subprocess.run(prefix + ["/usr/bin/time", "-f", "%M", "-o", tfile, exe, rounds],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    ms = (time.perf_counter() - t0) * 1000.0
    rss = int(open(tfile).read().strip().splitlines()[-1])
    return ms, rss, p.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "paper", "data", "bounded.tsv"))
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="extc-bounded-")
    exes = {v: build(tmp, v) for v in ("def", "off")}
    rows = []
    for rounds in ROUNDS:
        outs = {v: run_once(exes[v], rounds, tmp)[2] for v in ("def", "off")}
        same = int(outs["def"] == outs["off"])
        acc = {v: [] for v in ("def", "off")}
        rss = {v: 0 for v in ("def", "off")}
        for r in range(REPS):
            order = ("def", "off") if r % 2 == 0 else ("off", "def")
            for v in order:
                ms, kb, _ = run_once(exes[v], rounds, tmp)
                acc[v].append(ms)
                rss[v] = max(rss[v], kb)
        for v in ("def", "off"):
            rows.append((rounds, v, min(acc[v]), statistics.median(acc[v]), rss[v], same, REPS))
        print(rounds, "done", flush=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("rounds\tvariant\tmin_ms\tmed_ms\trss_kb\tsame\tn\n")
        for r in rows:
            f.write("%s\t%s\t%.1f\t%.1f\t%d\t%d\t%d\n" % r)
    print("wrote", args.out)

if __name__ == "__main__":
    main()
