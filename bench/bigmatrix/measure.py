#!/usr/bin/env python3
"""bench/bigmatrix/measure.py -- best-of-N wall time + peak RSS for one command.

Why this exists instead of reusing bench/gc/measure.py: that one derives RSS from
the RUSAGE_CHILDREN maximum before and after each run.  Two problems with that
here:

  * RUSAGE_CHILDREN's ru_maxrss is a running maximum over *all* waited-for
    children, so only the first run can make it grow; later runs measure nothing.
  * the fork before exec briefly maps the parent Python's pages, so a small
    program reports the interpreter's footprint instead -- every low-memory
    shape came out at a flat ~13 MB floor, which is not the program's RSS.

`/usr/bin/time -f %M` reports the child's own peak, which is what the matrix
wants.  Wall time stays on perf_counter (the same for every language, and it
includes process startup, which is deliberate: Java's JVM startup is part of
Java's number).

Usage:  STDIN_FILE=in/radix.txt N=5 python3 measure.py <command...>
Prints: "<best_ms>\t<peak_rss_mb>\t<returncode>"
"""
import os
import subprocess
import sys
import time

TIME = "/usr/bin/time"


def run_once(cmd, stdin_file):
    with open(stdin_file, "rb") as fh:
        t0 = time.perf_counter()
        p = subprocess.run([TIME, "-f", "%M"] + cmd, stdin=fh, capture_output=True)
        dt = (time.perf_counter() - t0) * 1000.0
    rss = 0
    for line in reversed(p.stderr.decode(errors="replace").splitlines()):
        line = line.strip()
        if line.isdigit():
            rss = int(line)          # %M prints kilobytes
            break
    return dt, rss / 1024.0, p.returncode


def main():
    n = int(os.environ.get("N", "5"))
    stdin_file = os.environ.get("STDIN_FILE")
    cmd = sys.argv[1:]
    if not cmd or not stdin_file:
        print(__doc__)
        return 2
    best = None
    peak = 0.0
    rc = 0
    for _ in range(n):
        dt, rss, rc = run_once(cmd, stdin_file)
        best = dt if best is None else min(best, dt)
        peak = max(peak, rss)
    print(f"{best:.1f}\t{peak:.1f}\t{rc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
