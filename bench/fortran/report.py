#!/usr/bin/env python3
"""bench/fortran/report.py —— 把 raw.txt 生成为 RESULTS.md（别手改 RESULTS.md）。

口径见同目录 SPEC.md；数值全部来自 raw.txt，本脚本不重跑实验。
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw.txt")
OUT = os.path.join(HERE, "RESULTS.md")

ARMS = ["extc", "c", "fortran"]
LABEL = {"extc": "extC", "c": "C", "fortran": "Fortran"}


def load():
    meta, rows = {}, []
    for line in open(RAW, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "data":
            _, arm, level, n, ms, rss, cs = parts
            rows.append(dict(arm=arm, level=level, n=int(n),
                             ms=float(ms), rss=int(rss), cs=int(cs)))
        else:
            meta[parts[0]] = parts[1] if len(parts) > 1 else ""
    return meta, rows


def grid(rows, level):
    """level -> {n: {arm: row}}"""
    g = {}
    for r in rows:
        if r["level"] != level:
            continue
        g.setdefault(r["n"], {})[r["arm"]] = r
    return dict(sorted(g.items()))


def rat(a, b, nd=2):
    return "—" if not b else f"{a / b:.{nd}f}"


def table(g, iters):
    out = ["| n | extC ms | C ms | Fortran ms | extC ns/cell | extC÷C | extC÷Fortran | Fortran÷C | cs |",
           "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for n, a in g.items():
        e, c, f = (a.get(k) for k in ARMS)
        nsc = e["ms"] * 1e6 / (n * n * iters)
        out.append(
            f"| {n} | {e['ms']:.2f} | {c['ms']:.2f} | {f['ms']:.2f} | {nsc:.2f} "
            f"| {rat(e['ms'], c['ms'])} | {rat(e['ms'], f['ms'])} | {rat(f['ms'], c['ms'])} | {e['cs']} |")
    return out


def mean_ratio(g, num, den):
    xs = [a[num]["ms"] / a[den]["ms"] for a in g.values() if num in a and den in a]
    return sum(xs) / len(xs) if xs else 0.0


def main():
    meta, rows = load()
    iters = int(meta.get("iters", "0") or 0)
    g2, g3 = grid(rows, "O2"), grid(rows, "O3")

    L = ["# RESULTS —— 2D 5 点 Jacobi 模板：extC / C / Fortran", "",
         "> 由 `bench/fortran/report.py` 从 `raw.txt` 生成，**别手改**；口径与判据见 `SPEC.md`。", "",
         "## 口径", "",
         "| 项 | 值 |", "| --- | --- |"]
    for k, label in [("machine", "CPU"), ("kernel", "内核"), ("nproc", "核数"),
                     ("gcc", "C 编译器"), ("gfortran", "Fortran 编译器"),
                     ("extc", "extC 前端"), ("pin", "钉核"), ("iters", "iters"),
                     ("runs", "RUNS（取 best）"), ("sizes", "n")]:
        if k in meta:
            L.append(f"| {label} | `{meta[k]}` |")
    for k in ("flags_O2", "flags_O3"):
        if k in meta:
            L.append(f"| {k.replace('flags_', '')} 旗子 | {meta[k]} |")
    if "vec_O3_extc" in meta:
        L.append(f"| 向量化（-O3，`loop vectorized` 处数） | extC 生成的 C：{meta['vec_O3_extc']}"
                 f" · 手写 C：{meta.get('vec_O3_c', '?')} |")
    L += ["",
          "三臂同算法、同参数、同 stdout 字节（cs 列逐字节相同才算做了等价工作）；时间由外部",
          "`time.perf_counter` 包整个进程，预热 1 次，`RUNS` 次取 best；慢 > 1。", "",
          "## 主表 -O2（契约口径）", ""]
    L += table(g2, iters)
    L += ["", "## 旁证 -O3 -march=native", ""]
    L += table(g3, iters)

    L += ["", "## 怎么读", ""]
    if g2:
        ec = mean_ratio(g2, "extc", "c")
        ef = mean_ratio(g2, "extc", "fortran")
        fc = mean_ratio(g2, "fortran", "c")
        L.append(f"- 主表（-O2）平均：extC 是 C 的 **{ec:.2f}×**，是 Fortran 的 **{ef:.2f}×**，"
                 f"Fortran 是 C 的 **{fc:.2f}×**。")
        L.append("- `extC÷C` 读作 extC 相对同一份 C 后端（同一套 `-O2 -std=c11 -fwrapv`）的"
                 "语言实现余量；`extC÷Fortran` 是本次 headline；`Fortran÷C` 是前端差异。")
        L.append("- 小规模时进程启动与首次触页占比不可忽略，看比值别只看绝对值；规模增大后"
                 "受内存带宽与末级缓存容量约束（两块 buffer 约 2·8·n² 字节），比值会向 1 收敛。")
    if g2 and g3:
        def speed(g1, g0, arm):
            xs = [g0[n][arm]["ms"] / g1[n][arm]["ms"] for n in g1 if n in g0]
            return sum(xs) / len(xs) if xs else 0.0
        L.append(f"- 从 -O2 到 -O3 -march=native，平均提速：extC "
                 f"**{speed(g3, g2, 'extc'):.2f}×**、C **{speed(g3, g2, 'c'):.2f}×**、"
                 f"Fortran **{speed(g3, g2, 'fortran'):.2f}×**。")
    if "vec_O3_extc" in meta:
        try:
            ve, vc = int(meta["vec_O3_extc"]), int(meta.get("vec_O3_c", "0"))
        except ValueError:
            ve = vc = -1
        L += ["",
              f"- 机制旁证（GCC `-fopt-info-vec`，-O3 档）：手写 C 标出 `loop vectorized` {vc} 处，"
              f"extC 生成的 C {ve} 处。这与「extC 的 -O3 档相比 -O2 几乎不提速，而 C/Fortran 明显提速」"
              "的现象一致；此处只记录编译器自报与秒表，不推断因果。"]
    L += ["",
          "## 读这份数字时要记住的", "",
          "- 单机、单形状、双 buffer 内存带宽型 kernel；不代表编译期、泛型、字符串、GC 等维度。",
          "- RSS 是各自运行时的常驻内存（kbytes），三臂的运行时/分配器不同，只作量级参考，不当排名。",
          "- 有意没做：`@unchecked`（关边界检查）变体、多线程、cache-blocked 变体；见 `SPEC.md` §4。",
          ""]
    open(OUT, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print("\n".join(L))


if __name__ == "__main__":
    main()
