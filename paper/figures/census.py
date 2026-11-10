#!/usr/bin/env python3
"""Tier census: compile every .extc test program and classify the emitted tier.

For each program we compile with the artifact compiler and read the tier mark out of
the generated C header (EXTC_RC_TIER, EXTC_RC_NOCOUNT, EXTC_RC_SWEEP_FREE).  A program
that fails to compile is counted separately: a large share of them are negative tests
whose failure is the expected outcome.

Usage: python3 paper/figures/census.py [--root tests] [--out paper/data/census.tsv]
"""
import argparse, os, re, subprocess, sys, tempfile

EXTC = os.environ.get("EXTC", "build/extc")
TIER_RE = re.compile(r"^#define EXTC_RC_TIER (\d+)", re.M)

def compile_one(path, out):
    cmd = [EXTC, "-w", "--no-line-map", "-I", "stdlib", path, "-o", out]
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None, "timeout", 0, 0, 0
    if p.returncode != 0 or not os.path.exists(out):
        return None, "fail", 0, 0, 0
    with open(out, "r", errors="replace") as f:
        text = f.read()
    m = TIER_RE.search(text)
    tier = int(m.group(1)) if m else -1
    return tier, "ok", len(text.encode()), text.count("\n"), text.count("extc_rc_")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="tests")
    ap.add_argument("--out", default="paper/data/census.tsv")
    args = ap.parse_args()
    files = []
    for dirpath, _dirnames, filenames in os.walk(args.root):
        for fn in filenames:
            if fn.endswith(".extc"):
                files.append(os.path.join(dirpath, fn))
    files.sort()
    tmp = tempfile.mkdtemp(prefix="extc-census-")
    rows = []
    for i, path in enumerate(files):
        tier, status, size, lines, marks = compile_one(path, os.path.join(tmp, "out.c"))
        if status != "ok":
            rows.append((path, status, -1, 0, 0, 0))
        else:
            cls = "tier0" if marks == 0 else "tier%d" % tier
            rows.append((path, "ok", tier, size, lines, marks))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("path\tstatus\ttier\tbytes\tlines\trc_marks\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    counts = {}
    for r in rows:
        key = "fail" if r[1] != "ok" else ("tier0" if r[5] == 0 else "tier%d" % r[2])
        counts[key] = counts.get(key, 0) + 1
    print("programs:", len(rows))
    for k in sorted(counts):
        print("  %-8s %5d" % (k, counts[k]))

if __name__ == "__main__":
    main()
