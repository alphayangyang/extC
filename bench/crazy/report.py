#!/usr/bin/env python3
"""Turn the per-run TSV into RESULTS.md: min per phase, thread scaling, checksum check, LOC."""
import sys
from collections import defaultdict

NAMES = {"extc": "extC", "node": "Node 24", "python": "Python 3.14", "rust": "Rust 1.93",
         "viper": "Viper 1.0.6", "viperopt": "Viper 1.0.6 + opt -O2"}
ORDER = ["extc", "rust", "viper", "viperopt", "node", "python"]
SRC = {"extc": "bench/crazy/extc/main.extc", "node": "bench/crazy/node/main.js",
       "python": "bench/crazy/python/main.py", "rust": "bench/crazy/rust/main.rs",
       "viper": "bench/crazy/viper/main.vp", "viperopt": "bench/crazy/viper/main.vp"}


def count_lines(path):
    n, in_block = 0, False
    for raw in open(path, encoding="utf-8"):
        s = raw.strip()
        if in_block:
            in_block = "*/" not in s
            continue
        if s.startswith("/*"):
            in_block = "*/" not in s
            continue
        if not s or s.startswith("//") or s.startswith("#"):
            continue
        n += 1
    return n


single, par8, checks = defaultdict(list), defaultdict(list), defaultdict(set)
for line in open(sys.argv[1]):
    parts = line.rstrip("\n").split("\t")
    if parts[0] == "single" and len(parts) == 7:
        single[parts[1]].append(tuple(int(x) for x in parts[2:6]))
        checks[parts[1]].add(parts[6])
    elif parts[0] == "par8" and len(parts) == 3:
        par8[parts[1]].append(int(parts[2]))

ref = checks.get("extc", set())
bad = [l for l in checks if checks[l] != ref]
if bad:
    print("!! checksum mismatch:", ", ".join(bad))
    for l in bad:
        print("  ", l, sorted(checks[l]))
    sys.exit(1)

best = {l: tuple(min(r[i] for r in rs) for i in range(4)) for l, rs in single.items()}
loc = {l: count_lines(SRC[l]) for l in SRC}
base = best["rust"]

out = ["# bench/crazy：同一份输入、五种写法", "",
       "五语言读同一份 **150 万行 / 44 MB** 的 TSV，做同样四件事：解析、按用户分组聚合、",
       "把每 16 行拼成 JSON（extC 用运行期 `any` + 描述符编码，另外几家手工/内置编码）、加权归约。",
       "**六份输出行逐字节相同**。时间单位 ms，`taskset -c 0`、5 轮取最小。", "",
       "| 实现 | 代码行 | 解析 | 聚合 | JSON | 归约(1 线程) | 总耗时 | 相对 Rust |",
       "|---|---|---|---|---|---|---|---|"]
for lang in ORDER:
    if lang not in best:
        continue
    p, a, j, r = best[lang]
    tot = p + a + j + r
    out.append(f"| {NAMES[lang]} | {loc.get(lang, '?')} | {p/1000:.0f} | {a/1000:.0f} | {j/1000:.0f} | "
               f"{r/1000:.0f} | **{tot/1000:.0f}** | {sum(base)/tot:.2f}× |")

out += ["",
        "Viper 两行是同一份 `bench/crazy/viper/main.vp` 的两次编译：`Viper 1.0.6` 走 `viperc` 的",
        "Linux 流程（`mlir-opt` → `mlir-translate` → `llc`），`+ opt -O2` 只在 llc 之前插入 LLVM",
        "中间端优化（`bench/viper/llcopt/llc` 把 `opt -O2` 放在 `llc` 前）。extC 走 `cc -O2`、Rust 走",
        "`rustc -O`，所以带 opt 的那一行才是同口径比较；不带 opt 的那行是 ViperC 出厂 Linux 行为。",
        "",
        "## 并行归约（extC `par` vs Rust `std::thread`）", "",
        "同一份行数据、同样的 8 轮累加，单线程 vs 8 线程（`taskset -c 0` 对 `taskset -c 0-7`）：", "",
        "| 实现 | 1 线程 | 8 线程 | 加速比 |", "|---|---|---|---|"]
for lang in ("extc", "rust"):
    one = best[lang][3]
    eight = min(par8.get(lang, [one]))
    out.append(f"| {NAMES[lang]} | {one/1000:.0f} ms | {eight/1000:.0f} ms | **{one/eight:.2f}×** |")

out += ["", "Node、Python 与 Viper 这一轴只跑单线程：Node 的 worker 要额外序列化，Python 的 GIL 让线程帮不上",
        "（要用多进程 = 每个进程各自重新解析，不是同一件事），Viper 的 `vthreading` 是裸线程库、没有",
        "对应的结构化归约端口，所以都不列。", "",
        f"checksums（六者相同）：`{sorted(ref)[0]}`", ""]
print("\n".join(out))
open("bench/crazy/RESULTS.md", "w").write("\n".join(out) + "\n")
