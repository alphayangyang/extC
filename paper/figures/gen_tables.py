#!/usr/bin/env python3
"""Generate the evaluation tables and figures from the recorded measurements.

Inputs (all committed under paper/data):
  crosslang.tsv     44-arm cross-language benchmark (bench/gc/v2/matrix.tsv)
  pause.tsv         step-pause distributions per shape and runtime (bench/gc/v2/pause_v2.tsv)
  ablate.tsv        mechanism ablation, wall clock and peak RSS
  ablate-pause.tsv  mechanism ablation, step-pause distributions
  bounded.tsv       boundedness at 3 orders of magnitude of rounds
  census.tsv        tier census over every .extc program in tests/
  roofline-pareto.tsv  numbers behind the roofline plane

Outputs: paper/tables/*.tex and paper/figures/*.pdf/.png

Usage: python3 paper/figures/gen_tables.py
"""
import collections, csv, os

R = chr(92)          # LaTeX backslash, kept symbolic so this file needs no escapes
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAPER = os.path.join(ROOT, "paper")
DATA = os.path.join(PAPER, "data")
TAB = os.path.join(PAPER, "tables")
FIG = os.path.join(PAPER, "figures")
SHAPES = ["promote", "sizeratio", "chase", "cycleratio"]

def load(name):
    with open(os.path.join(DATA, name)) as f:
        return list(csv.DictReader(f, delimiter="\t"))

def fnum(x, nd=1):
    return ("%." + str(nd) + "f") % float(x)

def mb(kb):
    return fnum(int(kb) / 1024.0, 1)

def row(*cells):
    return " & ".join(str(c) for c in cells) + " " + R + R

def rule(cmd="midrule"):
    return R + cmd

def write(name, lines):
    os.makedirs(TAB, exist_ok=True)
    with open(os.path.join(TAB, name), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote tables/" + name)

SHRINK = (R + "resizebox{" + R + "ifdim" + R + "width>" + R + "linewidth" + R + "linewidth" +
          R + "else" + R + "width" + R + "fi}{!}{")

def table(label, caption, colspec, head, body, size="small", wide=False):
    env = "table*" if wide else "table"
    lines = [
        R + "begin{" + env + "}[t]",
        R + "centering",
        R + "setlength{" + R + "tabcolsep}{4pt}",
        R + size,
        R + "caption{" + caption + "}",
        R + "label{" + label + "}",
        SHRINK,
        R + "begin{tabular}{" + colspec + "}",
        R + "toprule",
    ]
    lines += head
    lines.append(R + "midrule")
    lines += body
    lines += [R + "bottomrule", R + "end{tabular}}",
              R + "end{" + env + "}"]
    return lines

def crosslang():
    rows = load("crosslang.tsv")
    d = {}
    for r in rows:
        d[(r["shape"], r["arm"])] = r
    def get(shape, arm):
        return d[(shape, arm)]
    def best(shape, arms, key="ms_min"):
        return min(arms, key=lambda a: float(get(shape, a)[key]))
    cpp = ["cpp:default", "cpp:careful"]
    rust = ["rust:default", "rust:careful"]
    other = ["go:default", "java:default", "node:split", "node:bigint"]
    body = []
    for s in SHAPES:
        e = get(s, "extc:default")
        c = best(s, cpp)
        rbest = best(s, rust)
        o = best(s, other)
        bc = get(s, c)
        br = get(s, rbest)
        bo = get(s, o)
        ratio_c = float(bc["ms_min"]) / float(e["ms_min"])
        ratio_r = float(br["ms_min"]) / float(e["ms_min"])
        body.append(row(s,
                        fnum(e["ms_min"]) + " / " + fnum(e["ms_med"]),
                        fnum(bc["ms_min"]) + " / " + fnum(bc["ms_med"]),
                        fnum(br["ms_min"]) + " / " + fnum(br["ms_med"]),
                        fnum(bo["ms_min"]) + " / " + fnum(bo["ms_med"]),
                        "$" + ("%.2f" % ratio_c) + R + "times$",
                        "$" + ("%.2f" % ratio_r) + R + "times$",
                        mb(e["rss_kb"]) + " / " + mb(bc["rss_kb"]) + " / " + mb(br["rss_kb"])))
    head = [row("shape",
                R + "multicolumn{2}{c}{extC}",
                R + "multicolumn{2}{c}{C++ (best)}",
                R + "multicolumn{2}{c}{Rust (best)}",
                R + "multicolumn{2}{c}{other (best)}",
                R + "multicolumn{2}{c}{time relative to extC}",
                R + "multicolumn{3}{c}{peak RSS (MB)}"),
            R + "cmidrule(lr){2-3}" + R + "cmidrule(lr){4-5}" + R + "cmidrule(lr){6-7}" +
            R + "cmidrule(lr){8-9}" + R + "cmidrule(lr){10-11}" + R + "cmidrule(lr){12-14}",
            row("", "min", "med", "min", "med", "min", "med", "min", "med",
                "C++ / extC", "Rust / extC", "extC", "C++", "Rust")]
    cap = ("Wall clock in milliseconds (minimum and median of seven interleaved runs, one core,",
           " 44 arms, stdout of every arm byte-identical to the reference) and peak resident set.",
           " Each competitor column is the better of its two configurations.")
    write("crosslang.tex", table("tab:crosslang", " ".join(cap),
                                 "@{}lrrrrrrrrrrrrr@{}", head, body, size="scriptsize", wide=True))

    # full dump for the appendix
    arms = ["extc:default", "extc:off", "cpp:default", "cpp:careful", "rust:default",
            "rust:careful", "go:default", "go:off", "java:default", "node:split", "node:bigint"]
    body = []
    for a in arms:
        cells = [a.replace(":", R + "texttt{:}")]
        for s in SHAPES:
            r = d[(s, a)]
            cells.append(fnum(r["ms_min"]) + " / " + fnum(r["ms_med"]) + " / " + mb(r["rss_kb"]))
        body.append(row(*cells))
    head = [row("arm", *SHAPES)]
    cap = "All eleven arms: milliseconds (minimum / median) and peak RSS in MB, per shape."
    write("allarms.tex", table("tab:allarms", cap, "@{}lrrrr@{}", head, body, size="scriptsize", wide=True))

def pause_table():
    rows = load("pause.tsv")
    d = {(r["shape"], r["arm"]): r for r in rows}
    body = []
    for s in SHAPES:
        e = d[(s, "extc")]
        body.append(row(s, e["events"], fnum(float(e["total_us"]) / 1000.0, 1),
                        fnum(e["share_pct"], 1) + R + "%", fnum(e["p50_us"], 2), fnum(e["p99_us"], 2),
                        fnum(e["p999_us"], 2), fnum(e["max_us"], 1), e["ge1ms"]))
    head = [row("shape", "steps", "total (ms)", R + "% of wall", "$p_{50}$", "$p_{99}$",
                "$p_{99.9}$", "max", "$" + R + "ge 1$~ms")]
    cap = ("Step pauses of the reclaiming build, in microseconds. Every sub-step the mutator",
           " thread spends inside the runtime is one sample; the last column counts samples above",
           " one millisecond.")
    write("pause.tex", table("tab:pause", " ".join(cap), "@{}lrrrrrrrr@{}", head, body, size="small"))

    # cross-language pause comparison
    body = []
    for s in SHAPES:
        cells = [s]
        for arm in ["extc", "go", "java", "node"]:
            r = d[(s, arm)]
            if r["events"] == "0":
                cells.append("--")
            else:
                cells.append(r["events"] + " / " + fnum(r["p999_us"], 1) + " / " + fnum(r["max_us"], 1))
        body.append(row(*cells))
    head = [row("shape", "extC", "Go", "Java", "node")]
    cap = ("Longest pauses in microseconds for the four runtimes, as steps / $p_{99.9}$ / max.",
           " A runtime that hides work from its stop-the-world window reports fewer, smaller",
           " samples: the populations are not comparable, only the reported numbers are.")
    write("pauseruntimes.tex", table("tab:pauseruntimes", " ".join(cap),
                                     "@{}lrrrr@{}", head, body, size="footnotesize"))

def ablation():
    rows = load("ablate.tsv")
    d = {(r["shape"], r["variant"]): r for r in rows}
    variants = ["def", "off", "b17", "b20", "chunk1k", "chunk1m", "seg0", "segchk64", "nofin"]
    body = []
    for s in SHAPES:
        cells = [s]
        for v in variants:
            r = d.get((s, v))
            cells.append("-" if r is None else fnum(r["min_ms"]))
        body.append(row(*cells))
    head = [row("shape", *variants)]
    cap = ("Mechanism ablation: wall clock in milliseconds (minimum of seven interleaved runs).",
           " Every column is the same generated C with one constant changed, so a difference is",
           " attributable to the mechanism named by the column: b17/b20 slab geometry, chunk1k/1m",
           " scan chunk, seg0 no segment deadline, segchk64 a check every 64 words, nofin no",
           " sweep-finish credit, off no reclamation at all.")
    write("ablate.tex", table("tab:ablate", " ".join(cap), "@{}lrrrrrrrrr@{}", head, body, size="scriptsize", wide=True))

    rows = load("ablate-pause.tsv")
    d = {(r["shape"], r["variant"]): r for r in rows}
    variants = ["def", "chunk1m", "seg0", "segchk64", "nofin"]
    body = []
    for s in SHAPES:
        cells = [s]
        for v in variants:
            r = d.get((s, v))
            cells.append("-" if r is None or r["n"] == "0" else
                        fnum(r["p999"], 1) + " / " + fnum(r["max"], 1) + " / " + r["gt50"])
        body.append(row(*cells))
    head = [row("shape", *variants)]
    cap = ("The same ablation seen from the pause side. Each cell is $p_{99.9}$, then the maximum,",
           " then the number of steps above 50~" + R + "us, all in microseconds except the count.",
           " Removing the segment deadline (seg0) or enlarging the scan chunk (chunk1m) is what turns a",
           " bounded step into a millisecond-scale one.")
    write("ablatepause.tex", table("tab:ablatepause", " ".join(cap),
                                    "@{}lrrrrr@{}", head, body, size="scriptsize", wide=True))

def bounded():
    rows = load("bounded.tsv")
    d = {(r["rounds"], r["variant"]): r for r in rows}
    body = []
    for rounds in ["125000", "1000000", "8000000"]:
        a = d[(rounds, "def")]
        b = d[(rounds, "off")]
        body.append(row("{:,}".format(int(rounds)).replace(",", "{,}"),
                        fnum(a["min_ms"]), mb(a["rss_kb"]),
                        fnum(b["min_ms"]), mb(b["rss_kb"])))
    head = [row("rounds", R + "multicolumn{2}{c}{reclamation on}",
                R + "multicolumn{2}{c}{reclamation off}"),
            R + "cmidrule(lr){2-3}" + R + "cmidrule(lr){4-5}",
            row("", "ms", "peak RSS (MB)", "ms", "peak RSS (MB)")]
    cap = ("A loop whose live set does not grow, run for three orders of magnitude of rounds.",
           " Peak RSS of the reclaiming build is flat; without reclamation the same program",
           " accumulates every node it ever allocated.")
    write("bounded.tex", table("tab:bounded", " ".join(cap), "@{}lrrrrr@{}", head, body, size="small"))

def census():
    rows = load("census.tsv")
    n = len(rows)
    c = collections.Counter()
    for r in rows:
        if r["status"] != "ok":
            c["fail"] += 1
        elif int(r["rc_marks"]) == 0:
            c["tier0"] += 1
        else:
            c["tier" + r["tier"]] += 1
    ok = n - c["fail"]
    neg = sum(1 for r in rows if r["status"] != "ok" and ("/errors/" in r["path"] or "/neg_" in r["path"]))
    body = [
        row("programs examined", "{:,}".format(n).replace(",", "{,}"), "-"),
        row("compiled, no runtime emitted (T0)", "{:,}".format(c["tier0"]).replace(",", "{,}"),
            "$" + "%.1f" % (100.0 * c["tier0"] / ok) + R + "%$"),
        row("compiled, counting tier (T1)", c["tier1"], "$" + "%.1f" % (100.0 * c["tier1"] / ok) + R + "%$"),
        row("compiled, sweep tier (T2)", "{:,}".format(c["tier2"]).replace(",", "{,}"),
            "$" + "%.1f" % (100.0 * c["tier2"] / ok) + R + "%$"),
        row("compiled, total", "{:,}".format(ok).replace(",", "{,}"), "100" + R + "%"),
        row("did not compile standalone", c["fail"],
            "(" + str(neg) + " in negative-test directories)"),
    ]
    head = [row("class", "programs", "share of compiled")]
    cap = ("Tier census: every " + str(n) + " program under " + R + "texttt{tests/}, compiled",
           " one file at a time and classified by the tier mark in the generated C.  A program that",
           " does not compile is counted separately; most of those are negative tests.",
           " 544 of the compiled programs emit no reclamation code at all.")
    write("census.tex", table("tab:census", " ".join(cap), "@{}lrr@{}", head, body, size="small"))

def figures():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = load("crosslang.tsv")
    d = {(r["shape"], r["arm"]): r for r in rows}
    cpp = ["cpp:default", "cpp:careful"]
    rust = ["rust:default", "rust:careful"]
    names, ratios_c, ratios_r = [], [], []
    for s in SHAPES:
        e = float(d[(s, "extc:default")]["ms_min"])
        c = min(float(d[(s, a)]["ms_min"]) for a in cpp)
        r = min(float(d[(s, a)]["ms_min"]) for a in rust)
        names.append(s)
        ratios_c.append(e / c)
        ratios_r.append(e / r)
    fig, ax = plt.subplots(figsize=(5.4, 2.4), dpi=200)
    xs = range(len(names))
    w = 0.38
    ax.bar([x - w / 2 for x in xs], ratios_c, w, label="extC / best C++", color="#2b6cb0")
    ax.bar([x + w / 2 for x in xs], ratios_r, w, label="extC / best Rust", color="#c05621")
    ax.axhline(1.0, color="black", lw=0.8, ls="--")
    ax.set_xticks(list(xs))
    ax.set_xticklabels(["promote 0.5M", "sizeratio 2M", "chase 1M/4M", "cycleratio 0.4M"], fontsize=7)
    ax.set_ylabel("wall-clock ratio (lower is better)", fontsize=8)
    ax.tick_params(labelsize=7)
    ax.set_ylim(0, 1.6)
    ax.legend(fontsize=7, frameon=False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "ratio.pdf"))
    fig.savefig(os.path.join(FIG, "ratio.png"))
    plt.close(fig)

    rows = load("pause.tsv")
    d = {(r["shape"], r["arm"]): r for r in rows}
    fig, ax = plt.subplots(figsize=(5.4, 2.4), dpi=200)
    keys = ["p50_us", "p99_us", "p999_us", "max_us"]
    labels = ["p50", "p99", "p99.9", "max"]
    width = 0.2
    for i, s in enumerate(SHAPES):
        e = d[(s, "extc")]
        vals = [float(e[k]) for k in keys]
        ax.bar([i + (j - 1.5) * width for j in range(4)], vals, width, label=None)
    ax.set_yscale("log")
    ax.axhline(1000.0, color="black", lw=0.8, ls="--")
    ax.text(3.4, 1100.0, "1 ms", fontsize=6, ha="right")
    ax.set_xticks(range(len(SHAPES)))
    ax.set_xticklabels(SHAPES, fontsize=7)
    ax.set_ylabel("pause (" + R + "us, log scale)", fontsize=8)
    ax.tick_params(labelsize=7)
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(label=l) for l in labels], fontsize=6, frameon=False, ncol=4,
              loc="upper left")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "pause.pdf"))
    fig.savefig(os.path.join(FIG, "pause.png"))
    plt.close(fig)
    print("wrote figures")

def main():
    os.makedirs(TAB, exist_ok=True)
    crosslang()
    pause_table()
    ablation()
    bounded()
    census()
    figures()

if __name__ == "__main__":
    main()