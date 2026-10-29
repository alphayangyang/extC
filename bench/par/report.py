#!/usr/bin/env python3
"""bench/par/report.py —— 读 bench/par/raw.txt，写 bench/par/RESULTS.md。

raw.txt 由 run.sh 生成（原始记录，别手改）；本脚本只做排版与比值计算，不改数字。
"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
raw_path = os.path.join(here, "raw.txt")
out_path = os.path.join(here, "RESULTS.md")

meta = {}
rows = []
for line in open(raw_path, encoding="utf-8"):
    line = line.rstrip("\n")
    if not line or line.startswith("#"):
        continue
    parts = line.split("\t")
    if parts[0] == "data":
        _, arm, n, ms, rss, cs = parts
        rows.append({"arm": arm, "n": int(n), "ms": float(ms), "rss": int(rss), "cs": int(cs)})
    else:
        meta[parts[0]] = parts[1] if len(parts) > 1 else ""

sizes = sorted({r["n"] for r in rows})
arms = []
for r in rows:
    if r["arm"] not in arms:
        arms.append(r["arm"])

def get(arm, n):
    for r in rows:
        if r["arm"] == arm and r["n"] == n:
            return r
    return None

def fmt_ms(ms):
    return f"{ms:9.2f}"

def nscell(ms, n):
    return f"{ms * 1e6 / n:7.3f}"

def gbps(ms, n):
    return f"{2.0 * n * 8 / (ms / 1e3) / 1e9:6.2f}"

L = []
L.append("# bench/par —— 数据并行（extC `par`）基准")
L.append("")
L.append("> 本文件由 `bench/par/report.py` 从 `bench/par/raw.txt` 生成，**别手改**；")
L.append("> 重跑写数字请改 `raw.txt` 的来源（`bash bench/par/run.sh`）。")
L.append("")
L.append("## 机器与口径")
L.append("")
L.append("| 项 | 值 |")
L.append("|---|---|")
for k, label in [
    ("machine", "CPU"),
    ("kernel", "内核"),
    ("nproc", "nproc"),
    ("gcc", "C 编译器"),
    ("extc", "extC 前端"),
    ("pin", "钉核（taskset）"),
    ("runs", "RUNS（取 best）"),
    ("sizes", "n"),
    ("chain", "递推链长度"),
    ("flags", "旗子"),
]:
    if k in meta:
        L.append(f"| {label} | `{meta[k]}` |")
L.append("")
L.append("workload 见 `bench/par/SPEC.md`：逐元素递推链（计算受限），每臂输出 `cs=<checksum> n=<n>`，")
L.append("所有臂 stdout 逐字节一致才算做了等价工作。计时是外部 `time.perf_counter` 包整个进程、")
L.append("预热一次、RUNS 次取 best；有效带宽按每格读 `in`、写 `out` 两个 8 字节计。")
L.append("")
L.append("## 结果")
L.append("")
head = "| 臂 | n | best ms | ns/cell | 有效 GB/s | RSS KB | cs |"
L.append(head)
L.append("|---|---:|---:|---:|---:|---:|---:|")
for n in sizes:
    for arm in arms:
        r = get(arm, n)
        if not r:
            continue
        L.append(
            f"| `{arm}` | {n} | {fmt_ms(r['ms'])} | {nscell(r['ms'], n)} | "
            f"{gbps(r['ms'], n)} | {r['rss']} | {r['cs']} |"
        )
L.append("")
L.append("## 怎么读")
L.append("")
L.append("- **ns/cell** 是每个元素的耗时。这张 workload 是计算受限的，所以 `par` 的 ns/cell 应随线程数下降；")
L.append("  如果它不降，说明工作没有真的摊开（检查 `taskset` 是否只绑了一个核，或者 `EXTC_PAR_THREADS`）。")
L.append("- **有效 GB/s** 是内存侧旁证，不是这张表的主指标：每格只读 `in`、写 `out` 各一次，")
L.append("  瓶颈在递推链而不是 DRAM；把它抄进报告是为了说明「同一台机器、同一口径」而不是主张带宽。")
L.append("- **加速比基线是 `seq`（或 `par1`）**：`c` 是不检查下标、且被 GCC 把外层循环向量化到 2 宽的手写 C，")
L.append("  它比 `seq` 快是「安全检查挡住向量化」这条已知差距（bench/fortran 里已量过），不是 `par` 的税。")
L.append("- 线程数只影响速度，不影响 `cs`：表里每个臂的 `cs` 必须相同（不同 = harness 坏了）。")
L.append("- 绝对值会随宿主状态漂；**比同一张表里的比值**，别和别的会话的毫秒数比。")
L.append("")
open(out_path, "w", encoding="utf-8").write("\n".join(L))
print(f"wrote {out_path} ({len(rows)} 行)")
