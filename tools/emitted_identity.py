#!/usr/bin/env python3
"""Refactor invariant for the extC compiler.

A structural change that is not supposed to change behaviour must leave the emitted C
byte-identical. This tool records the md5 of the emitted C for a fixed set of sources
and configurations, and later re-checks it.

  python3 tools/emitted_identity.py --write --set core     # create the baseline
  python3 tools/emitted_identity.py --check --set core     # after a refactor step

The source path is always passed relative to the repository root, because the emitter
embeds that path in the generated C (in `#line` and in the alloc call sites).
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "emitted-identity-baseline.tsv")

CORE = [
    "bench/gc/v2/src",
    "bench/app",
    "tests/gc-torture/src",
    "tests/gc-p0/src",
    "tests/reclaim-shapes",
    "tests/reclaim-perf",
    "tests/reclaim-gaps/shapes",
    "tests/stl",
    "tests/pool",
    "tests/arena-promoted",
]
ALL = ["tests", "bench", "stdlib"]

CONFIGS = {
    "default": {},
    "reclaim": {"EXTC_RECLAIM": "1"},
}


def sources(dirs):
    out = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        if os.path.isfile(base):
            out.append(os.path.relpath(base, ROOT))
            continue
        for dirpath, _dirnames, names in os.walk(base):
            for n in names:
                if n.endswith(".extc"):
                    out.append(os.path.relpath(os.path.join(dirpath, n), ROOT))
    return sorted(out)


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def emit(rel, cfg_env, tmp):
    env = dict(os.environ)
    for k in list(env.keys()):
        if k.startswith("EXTC_"):
            del env[k]
    env["EXTC_STD"] = os.path.join(ROOT, "stdlib")
    env.update(cfg_env)
    out = os.path.join(tmp, "o.c")
    if os.path.exists(out):
        os.remove(out)
    p = subprocess.run([os.path.join(ROOT, "build", "extc"), rel, "-o", out],
                       cwd=ROOT, env=env, capture_output=True)
    if p.returncode != 0:
        err = p.stderr.decode("utf-8", "replace")
        first = err.strip().splitlines()[0] if err.strip() else ""
        return ("fail %d" % p.returncode, md5_bytes(p.stderr), 0, first[:100])
    data = open(out, "rb").read()
    return ("ok", md5_bytes(data), len(data), "")


def collect(args):
    dirs = CORE if args.set == "core" else ALL
    srcs = sources(dirs)
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for rel in srcs:
            for cname in args.configs.split(","):
                cfg = CONFIGS[cname]
                status, h, size, note = emit(rel, cfg, tmp)
                rows.append((cname, rel, status, h, size, note))
    return rows


def write_baseline(rows):
    extc = os.path.join(ROOT, "build", "extc")
    lines = ["# emitted-C identity baseline (refactor invariant)",
             "# extc md5: " + (md5_bytes(open(extc, "rb").read()) if os.path.exists(extc) else "missing"),
             "# columns: config, source, status, md5, bytes"]
    for cname, rel, status, h, size, _note in rows:
        lines.append("%s\t%s\t%s\t%s\t%d" % (cname, rel, status, h, size))
    open(BASELINE, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return len(rows)


def read_baseline():
    rows = {}
    for line in open(BASELINE, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        cfg, rel, status, h, size = line.split("\t")
        rows[(cfg, rel)] = (status, h, int(size))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--set", default="core", choices=["core", "all"])
    ap.add_argument("--configs", default="default,reclaim")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()
    if not (args.write or args.check):
        ap.error("one of --write or --check is required")
    rows = collect(args)
    if args.write:
        n = write_baseline(rows)
        print("baseline written: %d rows -> %s" % (n, os.path.relpath(BASELINE, ROOT)))
        return 0
    base = read_baseline()
    bad = []
    for cname, rel, status, h, size, note in rows:
        old = base.get((cname, rel))
        if old is None:
            bad.append(("NEW", cname, rel, "", ""))
        elif old != (status, h, size):
            bad.append(("DIFF", cname, rel, old[0] + "/" + old[1][:8], status + "/" + h[:8]))
    missing = [k for k in base if k not in {(c, r) for c, r, _s, _h, _z, _n in rows}]
    print("checked %d rows against %d baseline rows" % (len(rows), len(base)))
    if missing:
        print("baseline rows not reproduced: %d (e.g. %s)" % (len(missing), missing[0]))
    for kind, cname, rel, a, b in bad[:args.limit]:
        print("  %s [%s] %s   %s -> %s" % (kind, cname, rel, a, b))
    if bad or missing:
        print("FAIL: emitted C is not byte-identical (%d changed, %d missing)" % (len(bad), len(missing)))
        return 1
    print("ok: emitted C is byte-identical for every source in the set")
    return 0


if __name__ == "__main__":
    sys.exit(main())
