#!/usr/bin/env python3
"""Turn the raw four-axis benchmark output into RESULTS.md.

Inputs (all produced by run.sh):
  RESULTS.tsv        workload, language, run, work_ms, peak_rss_kb
  out/serve-*.tsv    serve per-arm: language, run, ms, bad=, reqps=, server_cs=

The report is ratio-first: every table carries the absolute number and the
ratio to the C++ reference, because absolute times move with the host.
"""

import glob
import os
import statistics
import sys

LANGS = ["cpp", "extc", "go", "rust"]
LABEL = {"cpp": "C++", "extc": "extC", "go": "Go", "rust": "Rust"}
TW = {"cpp": 24, "extc": 24, "go": 24, "rust": 24}


def read_tsv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("workload"):
                continue
            parts = line.split("\t")
            if len(parts) < 5:
                continue
            rows.append(parts)
    return rows


def medians(rows):
    """workload -> language -> median work_ms, and peak RSS."""
    work = {}
    peak = {}
    for r in rows:
        workload, lang, run, wms, kb = r[0], r[1], r[2], r[3], r[4]
        if run == "peak":
            peak[(workload, lang)] = kb
            continue
        try:
            work.setdefault(workload, {}).setdefault(lang, []).append(float(wms))
        except ValueError:
            pass
    med = {w: {l: statistics.median(v) for l, v in d.items()} for w, d in work.items()}
    return med, peak


def fmt(x, nd=1):
    return "n/a" if x is None else f"{x:.{nd}f}"


def table_single(med, peak, out):
    out.append("| workload | language | work_ms (median) | vs C++ | peak RSS |")
    out.append("| --- | --- | ---: | ---: | ---: |")
    for workload in ("etl", "sessions"):
        if workload not in med:
            continue
        ref = med[workload].get("cpp")
        for lang in LANGS:
            if lang not in med[workload]:
                continue
            v = med[workload][lang]
            ratio = (v / ref) if ref else None
            kb = peak.get((workload, lang), "-")
            kb = f"{int(kb) // 1024} MB" if kb not in (None, "-") else "-"
            out.append(f"| {workload} | {LABEL[lang]} | {fmt(v)} | {fmt(ratio, 2)}x | {kb} |")


def table_parmix(med, out):
    ts = sorted({int(w.split("/")[1]) for w in med if w.startswith("parmix/")})
    if not ts:
        return
    out.append("| threads | " + " | ".join(LABEL[l] for l in LANGS) + " | extC vs C++ |")
    out.append("| ---: | " + " | ".join("---:" for _ in LANGS) + " | ---: |")
    base = {}
    for t in ts:
        workload = f"parmix/{t}"
        row = []
        for lang in LANGS:
            v = med.get(workload, {}).get(lang)
            if lang == "cpp" and v:
                base[t] = v
            row.append(fmt(v))
        ratio = None
        if base.get(t) and med.get(workload, {}).get("extc"):
            ratio = med[workload]["extc"] / base[t]
        out.append(f"| {t} | " + " | ".join(row) + f" | {fmt(ratio, 2)}x |")
    out.append("")
    out.append("Scaling of each arm against its own single-threaded run:")
    out.append("")
    out.append("| threads | " + " | ".join(LABEL[l] for l in LANGS) + " |")
    out.append("| ---: | " + " | ".join("---:" for _ in LANGS) + " |")
    for t in ts:
        row = []
        for lang in LANGS:
            one = med.get("parmix/1", {}).get(lang)
            v = med.get(f"parmix/{t}", {}).get(lang)
            row.append(f"{one / v:.2f}x" if one and v else "n/a")
        out.append(f"| {t} | " + " | ".join(row) + " |")


def serve_rows(out):
    rows = []
    for path in sorted(glob.glob("out/serve-*.tsv")):
        with open(path) as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 7:
                    continue
                lang = parts[1]
                ms = float(parts[3])
                bad = parts[4].split("=")[1]
                reqps = parts[5].split("=")[1]
                srvcs = parts[6].split("=")[1]
                rows.append((lang, ms, bad, float(reqps), srvcs))
    return rows


def table_serve(out):
    rows = serve_rows(out)
    if not rows:
        return
    by = {}
    for lang, ms, bad, reqps, srvcs in rows:
        by.setdefault(lang, []).append((ms, bad, reqps, srvcs))
    by = {l: (statistics.median(x[0] for x in v),
              v[-1][1],
              statistics.median(x[2] for x in v),
              v[-1][3]) for l, v in by.items()}
    ref = by.get("cpp")
    out.append("| language | completion (ms) | throughput (req/s) | bad replies | server checksum | vs C++ throughput |")
    out.append("| --- | ---: | ---: | ---: | ---: | ---: |")
    for lang in LANGS:
        if lang not in by:
            continue
        ms, bad, reqps, srvcs = by[lang]
        ratio = f"{ref[2] / reqps:.2f}x" if ref else "n/a"
        out.append(f"| {LABEL[lang]} | {ms:.1f} | {reqps:,.0f} | {bad} | {srvcs} | {ratio} |")


def main():
    rows = read_tsv("RESULTS.tsv")
    if not rows and not glob.glob("out/serve-*.tsv"):
        sys.exit("no RESULTS.tsv -- run ./run.sh first")
    med, peak = medians(rows)
    out = []
    out.append("# Four-axis cross-language benchmark: extC against C++, Go, and Rust")
    out.append("")
    out.append("Generated by `report.py` from `RESULTS.tsv` and `out/serve-*.tsv`; regenerate with")
    out.append("`./run.sh && python3 report.py`. Every number below is measured on the same dataset,")
    out.append("the same machine, and, for the network axis, by the same client.")
    out.append("")
    out.append("The dataset is `data/log.tsv`: 2,000,000 rows of a synthetic request log")
    out.append("(timestamp, user, site, path, status, latency, bytes), 71 MB, generated once by")
    out.append("`gen/gen.c` and read by every arm. The four axes are the four workloads:")
    out.append("")
    out.append("- `etl` -- single-threaded scan, parse, and aggregate the whole file. The")
    out.append("  conventional axis: no allocation past the read buffer, one pass, tight loop.")
    out.append("- `sessions` -- the same scan plus a per-request message object and a session table")
    out.append("  keyed by user with a 300 s TTL, where expired sessions are evicted. The reclamation")
    out.append("  axis: roughly three quarters of the users seen go out of the window.")
    out.append("- `parmix` -- a parallel pass over the same records: every record goes through 64")
    out.append("  rounds of a mixing function and the results are folded into one checksum. The")
    out.append("  parallel axis, at 1, 8, and 24 threads.")
    out.append("- `serve` -- a TCP request/response loop over the per-site aggregates computed from the")
    out.append("  file, measured with the shared C client at 64 connections x 8 in flight and 200,000")
    out.append("  requests. The concurrency axis.")
    out.append("")
    out.append("Timings separate `read_ms` (ingest the 71 MB file) from `work_ms` (the workload), and")
    out.append("the tables compare `work_ms` only. Single-threaded workloads are pinned with `taskset`,")
    out.append("`parmix` is pinned to the same number of cores as threads, and `serve` runs the server")
    out.append("on one core with the client on three others.")
    out.append("")
    if med:
        out.append("## Conventional and reclamation axes")
        out.append("")
        table_single(med, peak, out)
        out.append("")
        out.append("`work_ms` is the workload only; peak RSS is the maximum resident set of the whole")
        out.append("process. Lower is better in both columns.")
        out.append("")
        out.append("## Parallel axis")
        out.append("")
        table_parmix(med, out)
        out.append("")
    out.append("## Concurrency axis")
    out.append("")
    table_serve(out)
    out.append("")
    out.append("## What each arm does differently")
    out.append("")
    out.append("- **C++** (`cpp/main.cpp`): a hand-written state machine over the mapped bytes; one")
    out.append("  `new`/`delete` message object per record in `sessions` and a `std::unordered_map` of")
    out.append("  session pointers; epoll for the server; `std::thread` over fixed offsets for `parmix`.")
    out.append("- **extC** (`extc/main.extc`): the same state machine; the message objects are arena")
    out.append("  allocations that are reclaimed as a block when the loop frame exits, and the session")
    out.append("  table is a flat arena array indexed by user id, where eviction only marks an entry dead")
    out.append("  (extC has no per-object free, so this axis is measured as \"no reclamation work inside")
    out.append("  the loop\" rather than as \"more efficient reclamation\"); `par` for `parmix`; one")
    out.append("  coroutine per connection on the `std::coro` scheduler for the server; replies of one")
    out.append("  read burst are buffered per connection and flushed with one `send` before the")
    out.append("  coroutine waits for more input, which is the same flush-per-burst shape as the")
    out.append("  C++ arm.")
    out.append("- **Go** (`go/main.go`): the same shape with `&event{}` per record and a `map` of session")
    out.append("  pointers, so the reclamation axis is the Go garbage collector; goroutines for both the")
    out.append("  parallel and the network axis.")
    out.append("- **Rust** (`rust/main.rs`): the same shape with `Box<event>` per record and a `HashMap` of")
    out.append("  sessions, so the reclamation axis is dropping boxes; `std::thread` for `parmix` and")
    out.append("  threads for the server.")
    out.append("")
    out.append("Every arm prints the same checksums, which is what makes the table comparable: a row")
    out.append("that disagreed on `cs=` would be measuring a different amount of work. The `serve`")
    out.append("checksums are the server's own, and the client independently verifies every reply.")
    out.append("")
    out.append("## Caveats")
    out.append("")
    out.append("- `read_ms` is excluded from the comparison, but it is in the logs: it is dominated by")
    out.append("  the syscalls, not by the language.")
    out.append("- WSL2 does not expose CPU frequency or power state, so absolute times drift between")
    out.append("  runs; only ratios taken in the same session should be compared.")
    out.append("- The `sessions` axis is deliberately not symmetric: it measures what each language's")
    out.append("  ordinary allocation discipline costs on the same workload, not one allocation strategy")
    out.append("  ported four times.")
    out.append("")
    text = "\n".join(out) + "\n"
    with open("RESULTS.md", "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
