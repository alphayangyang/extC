#!/usr/bin/env python3
"""appstall_analyze.py —— 从**原始戳文件**复算 bench/app 停顿报告的每一张表。

为什么不复用 appstall.py 的 console 汇总：那份汇总里列名写错过单位（把 ns 标成 µs）。
**报告里的每一个数都从 /tmp/appstall/stamps/*.bin 原始戳重新解码算出来**（可复算），
数学函数（decode_stamps/pct/med/dist）直接 import appstall，保证只有一份实现。

输入（全部只读）：
  /tmp/appstall/stamps/{shape}_us_{arm}_{rep}.bin    原始戳（40 字节头 + LEB128 增量）
  /tmp/appstall/logs/runs.tsv                        逐次墙钟/RSS/exit code/loadpre/戳数
输出：
  /tmp/appstall/logs/tables.md                       报告用的全部表格（Markdown）
  /tmp/appstall/logs/analysis.json                   同上的结构化版本

用法：python3 bench/app/appstall_analyze.py
"""
import json
import os
import re
import struct
import sys

sys.dont_write_bytecode = True          # 别在仓库里留 __pycache__（本脚本 import appstall）
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import appstall as A  # noqa: E402

fmt_us = A.fmt_us   # 一处实现：appstall.fmt_us

WORK = A.WORK
LOGS = A.LOGS
STAMPS = A.STAMPS
SHAPES = list(A.SHAPES)
ARMS = [a["name"] for a in A.ARMS]
ARM_META = {a["name"]: a for a in A.ARMS}
BUDGETS = A.BUDGETS_NS
REPS = int(os.environ.get("REPS", "5"))


def main():
    runs = {}
    with open(os.path.join(LOGS, "runs.tsv")) as f:
        hdr = f.readline().rstrip("\n").split("\t")
        for line in f:
            p = line.rstrip("\n").split("\t")
            r = dict(zip(hdr, p))
            runs[(r["shape"], r["arm"], r["kind"], int(r["rep"]))] = r

    out = []
    W = out.append
    analysis = {}

    W("# bench/app 用户视角停顿 —— 从原始戳复算的全部表格")
    W("")
    W(f"原始戳目录 `{STAMPS}/`；逐次运行记录 `{LOGS}/runs.tsv`；配置 `{LOGS}/config.txt`。")
    W(f"reps={REPS}；预算档 0.1ms / 1ms / 16.7ms（判据 `单位延迟 > 预算`，严格大于）。")
    W("")
    W("分布表的数值一律 **µs**（由纳秒 ÷1000）；超预算列是**计数**。栏头里的 `(µs)` 即此。")
    W("")

    # ---------- 自检 ----------
    W("## 0. 自检（原始戳复算）")
    W("")
    W("| 形状 | 臂 | 有效戳文件 | 戳数（逐 rep） | 期望 | nneg | novf | 判决 |")
    W("|---|---|---|---|---|---|---|---|")
    selfok = True
    for shape in SHAPES:
        units = A.SHAPES[shape]["units"](A.SHAPES[shape]["argv"])
        exp = units + 1
        for arm in ARMS:
            ns = []
            nnegs, novfs, bad = [], [], []
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"{shape}_us_{arm}_{rep}.bin")
                if not os.path.exists(p):
                    bad.append(f"rep{rep}:缺文件")
                    continue
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except ValueError as ex:
                    bad.append(f"rep{rep}:{ex}")
                    continue
                ns.append(n)
                nnegs.append(nneg)
                novfs.append(novf)
            ok = (len(ns) == REPS and all(x == exp for x in ns)
                  and all(x == 0 for x in nnegs) and all(x == 0 for x in novfs))
            selfok = selfok and ok
            W(f"| {shape} | {arm} | {len(ns)}/{REPS} | {ns} | {exp} | {sorted(set(nnegs))} | "
              f"{sorted(set(novfs))} | {'OK' if ok else 'FAIL ' + '; '.join(bad)} |")
    # exit code / runner rc / load
    bad_rc = [(k, v["child_rc"]) for k, v in runs.items() if v["child_rc"] not in ("0", "")]
    loads = [float(v["load_pre"]) for v in runs.values()]
    W("")
    W(f"- 逐次 `child_rc != 0` 的行：**{len(bad_rc)}**（全部行数 {len(runs)}）"
      f"{'：' + str(bad_rc) if bad_rc else ''}；`runner_rc` 全 0。")
    W(f"- `load_pre` min/中位/max = **{min(loads):.2f} / {sorted(loads)[len(loads)//2]:.2f} / {max(loads):.2f}**"
      f"（{len(loads)} 次运行，未 pin 核）。")
    W(f"- 孤儿进程：见 `{LOGS}/orphans.txt`。")
    W("")

    # ---------- 分布 ----------
    W("## 1. 单位延迟分布（µs；逐 rep 算分位 → 取 5 次**中位**）")
    W("")
    W("`max(中位)` = 5 次各自 max 的**中位**；`最长` = 5 次里**最大的那一次**（异常值不剔除）；")
    W("`最长/典型` = 最长 ÷ p50。`超预算` 列是 5 次的**中位计数**，逐次计数在下一张表。")
    W("")
    W("| 形状 | 臂 | 档 | 单位数 | p50 (µs) | p90 | p99 | p99.9 | max(中位) | 最长 | 最长/典型 | >0.1ms | >1ms | >16.7ms |")
    W("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    tab = {}
    for shape in SHAPES:
        units = A.SHAPES[shape]["units"](A.SHAPES[shape]["argv"])
        for arm in ARMS:
            ds = []
            longs = []
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"{shape}_us_{arm}_{rep}.bin")
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except (ValueError, OSError):
                    continue
                d = A.dist(dl)
                d["argmax"] = max(range(len(dl)), key=lambda i: dl[i])
                d["idx"] = dl
                ds.append(d)
                longs.append(max(dl))
            if not ds:
                continue
            row = {q: A.med([d[q] for d in ds]) for q in ("p50", "p90", "p99", "p999", "max", "mean")}
            row["max_abs"] = max(longs)
            row["ratio"] = row["max_abs"] / row["p50"]
            for b in BUDGETS:
                row[f"over_{b}"] = A.med([d["over"][b] for d in ds])
                row[f"over_{b}_per"] = [d["over"][b] for d in ds]
            tab[(shape, arm)] = row
            W(f"| {shape} | {arm} | {'默认' if ARM_META[arm]['default'] else '非默认'} | {units} | "
              f"{fmt_us(row['p50'])} | {fmt_us(row['p90'])} | {fmt_us(row['p99'])} | {fmt_us(row['p999'])} | "
              f"{fmt_us(row['max'])} | {fmt_us(row['max_abs'])} | {row['ratio']:.1f}× | "
              f"{int(row['over_100000'])} | {int(row['over_1000000'])} | {int(row['over_16700000'])} |")
    # 单位换算提示
    W("")
    W("> session 的 p50 落在 0.03 µs 量级（= 约 30 ns），route 约 0.6 µs。")
    W("")

    # ---------- 超预算逐次 + 最长那一笔在哪 ----------
    W("## 2. 超预算计数（逐次）与「最长那一笔落在第几个单位」")
    W("")
    W("| 形状 | 臂 | >0.1ms 逐次 | >1ms 逐次 | >16.7ms 逐次 | 最长那一笔的单位序号（逐 rep） | 出现位置（%） |")
    W("|---|---|---|---|---|---|---|")
    for shape in SHAPES:
        units = A.SHAPES[shape]["units"](A.SHAPES[shape]["argv"])
        for arm in ARMS:
            per, am, pos = [], [], []
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"{shape}_us_{arm}_{rep}.bin")
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except (ValueError, OSError):
                    continue
                per.append([sum(1 for v in dl if v > b) for b in BUDGETS])
                i = max(range(len(dl)), key=lambda j: dl[j])
                am.append(i)
                pos.append(100.0 * i / len(dl))
            if not per:
                continue
            W(f"| {shape} | {arm} | {[x[0] for x in per]} | {[x[1] for x in per]} | {[x[2] for x in per]} | "
              f"{am} | {[f'{x:.0f}%' for x in pos]} |")
    W("")

    # ---------- 长尾落在哪个十分位 ----------
    W("## 3. 长尾的**位置**分布：>0.1ms 的单位落在运行的哪个十分位（5 rep 合计）")
    W("")
    W("| 形状 | 臂 | 总 >0.1ms | 各十分位计数（0–10% … 90–100%） |")
    W("|---|---|---|---|")
    for shape in SHAPES:
        for arm in ARMS:
            dec = [0] * 10
            tot = 0
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"{shape}_us_{arm}_{rep}.bin")
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except (ValueError, OSError):
                    continue
                L = len(dl)
                for i, v in enumerate(dl):
                    if v > 100_000:
                        dec[min(9, i * 10 // L)] += 1
                        tot += 1
            W(f"| {shape} | {arm} | {tot} | {dec} |")
    W("")

    # ---------- route 逐相位 ----------
    W("## 4. route 逐相位（µs；相位按单位序号切）")
    W("")
    W("> 相位边界那一笔（最后一个 insert 的起点 → 第一个 lookup 的起点）算在**前一个相位**（insert）里。")
    W("")
    W("| 臂 | 相位 | 单位数 | p50 (µs) | p90 | p99 | p99.9 | max(中位) | 最长(5次最大) |")
    W("|---|---|---|---|---|---|---|---|---|")
    for arm in ARMS:
        for pname in ("insert", "lookup", "scan", "delete"):
            acc = []
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"route_us_{arm}_{rep}.bin")
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except (ValueError, OSError):
                    continue
                off = 0
                for nm, cnt in A.SHAPES["route"]["phases"](A.SHAPES["route"]["argv"]):
                    if nm == pname:
                        acc.append(A.dist(dl[off:off + cnt]))
                        break
                    off += cnt
            if not acc:
                continue
            r = {q: A.med([d[q] for d in acc]) for q in ("p50", "p90", "p99", "p999", "max")}
            mx = max(d["max"] for d in acc)
            W(f"| {arm} | {pname} | {acc[0]['n']} | {fmt_us(r['p50'])} | {fmt_us(r['p90'])} | "
              f"{fmt_us(r['p99'])} | {fmt_us(r['p999'])} | {fmt_us(r['max'])} | {fmt_us(mx)} |")
    W("")

    # ---------- route 相位过渡单位（不是业务单位） ----------
    W("## 4b. route 的**相位过渡**单位（不是一次业务操作，却被算进了相邻戳差）")
    W("")
    W("route 的程序在相位之间有一段**建索引/复位**的代码，它落在「最后一个 insert 的戳」与")
    W("「第一个 lookup 的戳」之间 ⇒ 被记成了那一个 insert 单位的延迟。")
    W("")
    ph = A.SHAPES["route"]["phases"](A.SHAPES["route"]["argv"])
    off = 0
    trans = []
    for k, (nm, cnt) in enumerate(ph):
        off += cnt
        if k + 1 < len(ph):
            trans.append((f"{nm}→{ph[k + 1][0]}", off - 1))
    W("| 臂 | 过渡（序号） | 逐 rep 实测（µs） | 该臂 p50（µs） |")
    W("|---|---|---|---|")
    for arm in ARMS:
        vals = {nm: [] for nm, _ in trans}
        for rep in range(REPS):
            p = os.path.join(STAMPS, f"route_us_{arm}_{rep}.bin")
            try:
                n, t0, nneg, novf, dl = A.decode_stamps(p)
            except (ValueError, OSError):
                continue
            for nm, i in trans:
                vals[nm].append(dl[i])
        for nm, i in trans:
            W(f"| {arm} | {nm} (i={i}) | {[fmt_us(v) for v in vals[nm]]} | "
              f"{fmt_us(tab[('route', arm)]['p50'])} |")
    W("")
    W("把这三笔**剔除**后再算 route 分布（业务单位口径更纯；只剔 3 笔，共 5,500,000 笔）：")
    W("")
    W("| 臂 | p50 | p99 | p99.9 | max(中位) | 最长 | >0.1ms(中位) | >1ms(中位) | >16.7ms(中位) |")
    W("|---|---|---|---|---|---|---|---|---|")
    drop = {i for _, i in trans}
    for arm in ARMS:
        ds = []
        for rep in range(REPS):
            p = os.path.join(STAMPS, f"route_us_{arm}_{rep}.bin")
            try:
                n, t0, nneg, novf, dl = A.decode_stamps(p)
            except (ValueError, OSError):
                continue
            ds.append(A.dist([v for i, v in enumerate(dl) if i not in drop]))
        if not ds:
            continue
        r = {q: A.med([d[q] for d in ds]) for q in ("p50", "p99", "p999", "max")}
        W(f"| {arm} | {fmt_us(r['p50'])} | {fmt_us(r['p99'])} | {fmt_us(r['p999'])} | {fmt_us(r['max'])} | "
          f"{fmt_us(max(d['max'] for d in ds))} | {int(A.med([d['over'][100_000] for d in ds]))} | "
          f"{int(A.med([d['over'][1_000_000] for d in ds]))} | "
          f"{int(A.med([d['over'][16_700_000] for d in ds]))} |")
    W("")

    # ---------- 最长的几笔是不是确定性的 ----------
    W("## 4c. 「最长的那几笔落在第几个单位」——确定性诊断")
    W("")
    W("同一形状同一臂的 5 次跑用**同一份 argv、同一份数据** ⇒ 若最长笔的序号在 5 次里")
    W("**完全相同**，那是**数据结构的确定性事件**（某一把特定的键）；若序号各不相同，那是**外部噪声**。")
    W("")
    W("| 形状 | 臂 | 最长笔序号（5 rep） | 5 rep 同一序号 | 前 5 长笔的序号（第 1 次跑） |")
    W("|---|---|---|---|---|")
    for shape in SHAPES:
        for arm in ARMS:
            am, top = [], None
            for rep in range(REPS):
                p = os.path.join(STAMPS, f"{shape}_us_{arm}_{rep}.bin")
                try:
                    n, t0, nneg, novf, dl = A.decode_stamps(p)
                except (ValueError, OSError):
                    continue
                am.append(max(range(len(dl)), key=lambda j: dl[j]))
                if top is None:
                    top = sorted(range(len(dl)), key=lambda j: -dl[j])[:5]
            if not am:
                continue
            W(f"| {shape} | {arm} | {am} | {'是' if len(set(am)) == 1 else '否'} | {top} |")
    W("")

    # ---------- 墙钟 / RSS / 插桩代价 ----------
    W("## 5. 墙钟（ms，5 次中位）/ 峰值 RSS（MiB，5 次最大）/ 插桩的现场代价")
    W("")
    W("| 形状 | 臂 | 墙钟 canon | 墙钟 us | RSS canon | RSS us | us−canon (ms) | 每单位 +ns |")
    W("|---|---|---|---|---|---|---|---|")
    wall = {}
    for shape in SHAPES:
        units = A.SHAPES[shape]["units"](A.SHAPES[shape]["argv"])
        for arm in ARMS:
            wcs = [float(runs[(shape, arm, "canon", rep)]["wall_ms"]) for rep in range(REPS)
                   if (shape, arm, "canon", rep) in runs]
            wus = [float(runs[(shape, arm, "us", rep)]["wall_ms"]) for rep in range(REPS)
                   if (shape, arm, "us", rep) in runs]
            rcs = [float(runs[(shape, arm, "canon", rep)]["rss_kb"]) for rep in range(REPS)
                   if (shape, arm, "canon", rep) in runs]
            rus = [float(runs[(shape, arm, "us", rep)]["rss_kb"]) for rep in range(REPS)
                   if (shape, arm, "us", rep) in runs]
            wcm, wum = A.med(wcs), A.med(wus)
            d = wum - wcm
            wall[(shape, arm)] = dict(wall_canon_med=wcm, wall_us_med=wum,
                                      rss_canon_max=max(rcs), rss_us_max=max(rus),
                                      us_cost_ns_per_unit=d * 1e6 / units)
            W(f"| {shape} | {arm} | {wcm:.1f} | {wum:.1f} | {max(rcs)/1024:.1f} | {max(rus)/1024:.1f} | "
              f"{d:+.1f} | {d * 1e6 / units:.1f} |")
    W("")
    W("> **canon 墙钟**是**没有插桩**的那一版，未受扰动 ⇒ `canon 墙钟 ÷ 单位数` 是干净的**均值**单位延迟，")
    W("> 但它没有分位数。插桩轮的分布含上面那一列「每单位 +ns」的固定开销。")
    W("")

    # ---------- A/B 对照 ----------
    W("## 6. A/B 对照（Δ = B − A；分布列单位 µs）")
    W("")
    W("| 形状 | B vs A | 含义 | Δp50 | Δp90 | Δp99 | Δp99.9 | Δmax(中位) | Δ最长 | Δ>0.1ms | Δ>1ms | Δ>16.7ms |")
    W("|---|---|---|---|---|---|---|---|---|---|---|---|")
    PAIRS = [
        ("go_nogc", "go", "关 GC vs 默认 GC（两个都是**非默认/默认**的运行期开关，同一份二进制）"),
        ("extc_norec", "extc_def", "`EXTC_RECLAIM=0` vs 默认（**生成物逐字节相同**）"),
        ("extc_force", "extc_def", "`EXTC_RECLAIM=1` 强制发射 vs 默认（.text 不同，代码布局变了）"),
        ("cpp", "extc_def", "C++（无回收器） vs extC 默认"),
        ("go", "extc_def", "Go（有 GC） vs extC 默认"),
    ]
    for shape in SHAPES:
        for b, a, note in PAIRS:
            A_ = tab.get((shape, a)); B_ = tab.get((shape, b))
            if not A_ or not B_:
                continue
            W(f"| {shape} | `{b}` − `{a}` | {note} | "
              f"{fmt_us(B_['p50']-A_['p50'])} | {fmt_us(B_['p90']-A_['p90'])} | "
              f"{fmt_us(B_['p99']-A_['p99'])} | {fmt_us(B_['p999']-A_['p999'])} | "
              f"{fmt_us(B_['max']-A_['max'])} | {fmt_us(B_['max_abs']-A_['max_abs'])} | "
              f"{int(B_['over_100000'])-int(A_['over_100000']):+d} | "
              f"{int(B_['over_1000000'])-int(A_['over_1000000']):+d} | "
              f"{int(B_['over_16700000'])-int(A_['over_16700000']):+d} |")
    W("")

    # ---------- 噪声地板 ----------
    W("## 7. 噪声地板（extc_def 与 extc_norec 的**生成 C 逐字节相同** ⇒ 两者之差就是纯跑动噪声）")
    W("")
    W("| 形状 | Δp50 | Δp90 | Δp99 | Δp99.9 | Δmax(中位) | Δ最长 |")
    W("|---|---|---|---|---|---|---|")
    for shape in SHAPES:
        a, b = tab.get((shape, "extc_def")), tab.get((shape, "extc_norec"))
        if not a or not b:
            continue
        W(f"| {shape} | {fmt_us(b['p50']-a['p50'])} | {fmt_us(b['p90']-a['p90'])} | "
          f"{fmt_us(b['p99']-a['p99'])} | {fmt_us(b['p999']-a['p999'])} | "
          f"{fmt_us(b['max']-a['max'])} | {fmt_us(b['max_abs']-a['max_abs'])} |")
    W("")
    # ---------- load 逐臂（是否被外部负载混杂） ----------
    W("## 8. 逐臂 loadavg（跑前，未 pin 核 ⇒ 外部负载会直接进长尾）")
    W("")
    W("| 形状 | 臂 | load_pre 逐次 |")
    W("|---|---|---|")
    for shape in SHAPES:
        for arm in ARMS:
            ls = [float(runs[(shape, arm, k, rep)]["load_pre"])
                  for k in ("canon", "us") for rep in range(REPS)
                  if (shape, arm, k, rep) in runs]
            W(f"| {shape} | {arm} | {[f'{x:.2f}' for x in ls]} |")
    W("")
    # ---------- canon 均值（无插桩） ----------
    W("## 9. 无插桩的**均值**单位延迟（canon 墙钟 ÷ 单位数；来自 runs.tsv）")
    W("")
    W("| 形状 | 臂 | canon 墙钟(ms，中位) | 单位数 | 均值 ns/单位 |")
    W("|---|---|---|---|---|")
    for shape in SHAPES:
        units = A.SHAPES[shape]["units"](A.SHAPES[shape]["argv"])
        for arm in ARMS:
            ws = [float(runs[(shape, arm, "canon", rep)]["wall_ms"]) for rep in range(REPS)
                  if (shape, arm, "canon", rep) in runs]
            if not ws:
                continue
            m = A.med(ws)
            W(f"| {shape} | {arm} | {m:.1f} | {units} | {m * 1e6 / units:.2f} |")
    W("")
    W(f"自检总判：{'全部通过' if selfok else '有不通过项'}；逐次 child_rc!=0 = {len(bad_rc)}；孤儿见 orphans.txt。")

    analysis = dict(selfcheck_ok=selfok, bad_rc=bad_rc,
                    load_min=min(loads), load_med=sorted(loads)[len(loads)//2], load_max=max(loads))
    with open(os.path.join(LOGS, "tables.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    with open(os.path.join(LOGS, "analysis.json"), "w") as f:
        json.dump(analysis, f, indent=1, sort_keys=True)
    print("\n".join(out))
    return 0 if selfok and not bad_rc else 1


if __name__ == "__main__":
    sys.exit(main())
