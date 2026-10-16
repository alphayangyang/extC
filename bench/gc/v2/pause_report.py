#!/usr/bin/env python3
"""bench/gc/v2/pause_report.py - 把 pause/<arm>/<shape>.{events,hist,meta} 聚合成表 + 图。

用法： python3 bench/gc/v2/pause_report.py [pausedir] [out.md] [out.png] [out.tsv]

口径见 bench/gc/v2/pause/README.md：每个臂报的是**它自己运行期承认的停顿事件**，
定义逐臂不同（meta 的 source 键），能对齐的是形状、分位、超阈占比、频度与占墙钟比例。
"""
import os
import sys

LADDER = [(0, 1), (1, 2), (2, 5), (5, 10), (10, 20), (20, 50), (50, 100),
          (100, 200), (200, 500), (500, 1000), (1000, 2000), (2000, 5000),
          (5000, 10000), (10000, float("inf"))]
SHAPES = ["promote", "sizeratio", "chase", "cycleratio"]
SHAPE_ARGS = {"promote": "500000 20261013", "sizeratio": "2000000 20261013",
              "chase": "1048576 20261013 4000000", "cycleratio": "400000 20261013 100"}


def read_meta(path):
    d = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def read_events(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(float(line))
            except ValueError:
                pass
    return out


def read_hist(path):
    buckets = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.strip().split("\t")
            if len(parts) != 3:
                continue
            lo, hi, cnt = parts
            hi = float("inf") if hi == "inf" else float(hi)
            buckets.append((float(lo), hi, int(cnt)))
    return buckets


def rebin(buckets):
    """把原生分档按重叠长度摊到公共阶梯上（+Inf 档整体计入含其下界的阶梯档）。"""
    counts = [0.0] * len(LADDER)
    for lo, hi, cnt in buckets:
        if cnt == 0:
            continue
        for i, (blo, bhi) in enumerate(LADDER):
            if hi == float("inf"):
                if blo <= lo < bhi:
                    counts[i] += cnt
                continue
            ov = min(hi, bhi) - max(lo, blo)
            if ov > 0:
                counts[i] += cnt * ov / (hi - lo)
    return counts


def quantile(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = q * (len(sorted_vals) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = pos - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def hist_quantile(counts, q):
    total = sum(counts)
    if total <= 0:
        return 0.0
    want = q * total
    acc = 0.0
    for i, c in enumerate(counts):
        acc += c
        if acc >= want:
            return LADDER[i][1] if LADDER[i][1] != float("inf") else LADDER[i][0]
    return LADDER[-1][0]


def collect(pausedir):
    rows = []
    for arm in sorted(os.listdir(pausedir)):
        adir = os.path.join(pausedir, arm)
        if not os.path.isdir(adir):
            continue
        for s in SHAPES:
            mp = os.path.join(adir, s + ".meta")
            if not os.path.exists(mp):
                continue
            meta = read_meta(mp)
            ep = os.path.join(adir, s + ".events")
            hp = os.path.join(adir, s + ".hist")
            ev = read_events(ep) if os.path.exists(ep) else []
            hb = read_hist(hp) if os.path.exists(hp) else []
            counts = rebin(hb)
            if ev and not hb:
                for v in ev:
                    for i, (lo, hi) in enumerate(LADDER):
                        if lo <= v < hi:
                            counts[i] += 1
                            break
            n = int(meta.get("events", len(ev)) or len(ev))
            total = float(meta.get("total_us", sum(ev)) or sum(ev))
            wall = float(meta.get("wall_plain_ms", meta.get("wall_ms", 0)) or 0)
            if ev:
                sv = sorted(ev)
                p50, p90, p99, p999 = (quantile(sv, .5), quantile(sv, .9),
                                       quantile(sv, .99), quantile(sv, .999))
                mx = sv[-1]
            else:
                p50, p90, p99, p999 = (hist_quantile(counts, .5), hist_quantile(counts, .9),
                                       hist_quantile(counts, .99), hist_quantile(counts, .999))
                mx = max((hi for lo, hi, c in hb if c and hi != float("inf")), default=0.0)
            ge1ms = int(round(sum(counts[i] for i, (lo, hi) in enumerate(LADDER) if lo >= 1000)))
            share = (total / (wall * 1000.0) * 100.0) if wall > 0 else 0.0
            try:
                tg = float(meta.get("total_gc_us", ""))
            except ValueError:
                tg = None
            gc_share = (tg / (wall * 1000.0) * 100.0) if (tg is not None and wall > 0) else None
            rows.append(dict(shape=s, arm=arm, events=n, total_us=total, share_pct=share,
                             total_gc_us=tg, gc_share_pct=gc_share,
                             p50=p50, p90=p90, p99=p99, p999=p999, max_us=mx,
                             ge1ms=ge1ms, counts=counts, wall_ms=wall,
                             slices=meta.get("slices", ""), collects=meta.get("collects", ""),
                             source=meta.get("source", "-"), notes=meta.get("notes", "-")))
    return rows


TSV_COLS = ["shape", "arm", "events", "total_us", "share_pct", "p50_us", "p90_us",
            "p99_us", "p999_us", "max_us", "ge1ms", "slices", "collects",
            "total_gc_us", "gc_share_pct"]


def write_tsv(rows, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\t".join(TSV_COLS) + "\n")
        for r in rows:
            f.write("%s\t%s\t%d\t%.1f\t%.3f\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%d\t%s\t%s\t%.1f\t%.3f\n" % (
                r["shape"], r["arm"], r["events"], r["total_us"], r["share_pct"],
                r["p50"], r["p90"], r["p99"], r["p999"], r["max_us"], r["ge1ms"],
                r["slices"], r["collects"],
                r["total_gc_us"] if r["total_gc_us"] is not None else -1.0,
                r["gc_share_pct"] if r["gc_share_pct"] is not None else -1.0))


def fmt_us(v):
    if v >= 1000:
        return "%.2f ms" % (v / 1000.0)
    return "%.2f µs" % v


def write_md(rows, path, tsvinfo):
    srcs = {}
    for r in rows:
        srcs.setdefault(r["arm"], r["source"])
    lines = [
        "# 跨语言停顿分布（v2）",
        "",
        "回答的问题：同一形状下，各语言的 GC 停顿**事件有多长、分布是什么形状**——不只报 max。",
        "",
        "口径：每个臂报的是**它自己运行期承认的停顿事件**，定义逐臂不同，读表前先看下面这栏。",
        "能对齐的是形状、分位、超阈占比、事件频度与占墙钟比例；绝对时长跨定义比较要谨慎。",
        "Rust / C++ 手工释放，没有 GC 停顿，不进本表。",
        "",
        "| 臂 | 事件来源（meta.source） |",
        "|---|---|",
    ]
    for arm in sorted(srcs):
        lines.append("| %s | %s |" % (arm, srcs[arm]))
    lines += ["## 读法（先看这五条，再看数）", "",
              "1. **事件数不可直接比**：extC 每条有界回收步算一个事件（上万条），Go 每个 GC 周期两段、Java 每条 GC 暂停、Node 每次 GC 各算一个（个位数到两位数）。",
              "   频率差异是「细粒度增量 vs 整轮 STW」的口径差，不是谁更忙。",
              "2. **样本量与分辨率**：Java 0–3 条、Go 4–102 条 ⇒ 它们的 p99/max 基本是单点噪声；Node 的 `pause` 只有 1 位小数 ms（100 µs 分辨率），",
              "   亚 100 µs 的 scavenge 记成 0 ⇒ `[0,100)` 那些细档别读；三个托管语言都是新进程、无预热。",
              "3. **extC 的「停顿」是在 mutator 线程上做的 GC 工作**（没有独立 GC 线程）⇒ 它的 `total_us` 与 `total_gc_us` 同值（见下一节）；",
              "   拿它的 p50 去和 Go 的 STW p50 排名次是错的，要比的是「mutator 被占住多少时间」= 下一节的 Σ 表。",
              "4. **max 是抢占敏感量**：extC chase 的 2.36 ms 是一次被抢占（`over1ms=1`），不是某个更贵的回收单元；",
              "   仓库纪律是用超阈计数与分位（`≥1ms` 列、p99/p99.9）而不是 max。",
              "5. **取数必须串行**：所有臂都钉同一条核（见 `pause/README.md` 第 2 节），并发跑会互相抢占（同一臂两次数值可差数倍）。", ""]
    lines += ["", "## 分形状", ""]
    for s in SHAPES:
        rs = [r for r in rows if r["shape"] == s]
        if not rs:
            continue
        lines += ["### %s（%s）" % (s, SHAPE_ARGS[s]), "",
                  "| 臂 | 事件数 | 事件/s | 总停顿 | 占墙钟 | p50 | p90 | p99 | p99.9 | max | ≥1ms |",
                  "|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in sorted(rs, key=lambda x: x["p99"]):
            rate = (r["events"] / (r["wall_ms"] / 1000.0)) if r["wall_ms"] else 0
            lines.append("| %s | %d | %.0f | %s | %.2f%% | %s | %s | %s | %s | %s | %d |" % (
                r["arm"], r["events"], rate, fmt_us(r["total_us"]), r["share_pct"],
                fmt_us(r["p50"]), fmt_us(r["p90"]), fmt_us(r["p99"]), fmt_us(r["p999"]),
                fmt_us(r["max_us"]), r["ge1ms"]))
        lines.append("")
    lines += ["## Sigma GC time (not the same quantity as the pause table above)", "",
              "上表是停顿（各语言定义不同，见 README 第 0 节）；这里是该次运行里可观测到的全部 GC 时间",
              "（口径见 README 第 1.2 节，- 表示该臂没提供）。extC 没有独立 GC 线程，它的停顿步就是全部 GC 工作；",
              "Go/Java/Node 的并发阶段不计入停顿，但计入这里。", ""]
    for s in SHAPES:
        rs = [r for r in rows if r["shape"] == s and r["total_gc_us"] is not None]
        if not rs:
            continue
        lines += ["### %s（%s）" % (s, SHAPE_ARGS[s]), "",
                  "| 臂 | 停顿合计 | 停顿/墙钟 | GC 工作合计 | GC/墙钟 | 墙钟 |",
                  "|---|---|---|---|---|---|"]
        for r in sorted(rs, key=lambda x: x["gc_share_pct"]):
            lines.append("| %s | %s | %.2f%% | %s | %.2f%% | %.1f ms |" % (
                r["arm"], fmt_us(r["total_us"]), r["share_pct"],
                fmt_us(r["total_gc_us"]), r["gc_share_pct"], r["wall_ms"]))
        lines.append("")
    lines += ["## 图", "", "![停顿分布](%s)" % os.path.basename(tsvinfo["png"]), "",
              "每格 = 一个形状，横轴为停顿时长（对数），纵轴为该臂**事件占比**（%，同一臂内归一），",
              "柱上标注事件总数。绝对值不可跨定义直读，看的应是「分布有多宽、尾巴有多长、事件有多少」。", ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def write_png(rows, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception as e:  # pragma: no cover
        print("跳过图（%s）" % e, file=sys.stderr)
        return False
    arms = sorted({r["arm"] for r in rows})
    centers = [max(lo, 0.5) if hi == float("inf") else (lo + hi) / 2.0 for lo, hi in LADDER]
    labels = ["%g" % c for c in centers]
    fig, axes = plt.subplots(2, 2, figsize=(13, 8))
    for ax, s in zip(axes.ravel(), SHAPES):
        rs = [r for r in rows if r["shape"] == s]
        if not rs:
            ax.set_visible(False)
            continue
        n = len(arms)
        width = 0.8 / max(n, 1)
        x = np.arange(len(LADDER))
        for i, arm in enumerate(arms):
            r = next((r for r in rs if r["arm"] == arm), None)
            if r is None:
                continue
            tot = sum(r["counts"]) or 1
            y = [c / tot * 100.0 for c in r["counts"]]
            ax.bar(x + (i - (n - 1) / 2) * width, y, width,
                   label="%s (%d)" % (arm, r["events"]))
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=7)
        ax.set_title("%s (%s)" % (s, SHAPE_ARGS[s]), fontsize=10)
        ax.set_xlabel("pause µs (log bins)")
        ax.set_ylabel("% events")
        ax.grid(True, axis="y", alpha=.3)
        ax.legend(fontsize=7)
    fig.suptitle("GC pause distribution by shape (per-arm normalized)", fontsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    print("图：%s" % path)
    return True


def main():
    v2 = os.path.dirname(os.path.abspath(__file__))
    pausedir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(v2, "pause")
    md = sys.argv[2] if len(sys.argv) > 2 else os.path.join(v2, "PAUSE.md")
    png = sys.argv[3] if len(sys.argv) > 3 else os.path.join(v2, "pause.png")
    tsv = sys.argv[4] if len(sys.argv) > 4 else os.path.join(v2, "pause_v2.tsv")
    rows = collect(pausedir)
    if not rows:
        print("没找到任何 .meta（检查 %s）" % pausedir, file=sys.stderr)
        return 1
    write_tsv(rows, tsv)
    write_md(rows, md, {"png": png})
    write_png(rows, png)
    print("表：%s（%d 行）" % (tsv, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
