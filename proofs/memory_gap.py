#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
memory_gap.py —— **内存**精度差距：所有情况类别 + 各自出现的比例。

背景（作者给的例子）：`for` 循环里 `new`，对象**可能**逃逸 ⇒ 站点被提升
⇒ 每轮分配的对象都留在函数级区域，直到函数结束（arena 只能整块释放）
⇒ Θ(n) 内存。而"逐路径完美知识"下只有**真正逃逸的轮次**需要长命区域。

差距的定律（ExtCMemGap.lean 两侧都证了）：

    内存差距 ≈ n / (e + 1)        n = 循环轮数，e = 真正逃逸的轮数

    差距超常数  ⟺  e = o(n)

本脚本把**所有情况类别**枚举出来：把"逃逸发生的轮次集合 E"参数化，
算出模型内存与三种上限内存，再对 n 做 log-log 拟合定出差距的**阶**。

四种内存口径：

  M_model     模型：站点"可能逃逸"就提升 ⇒ n·s（全部累积）
  M_opt_path  逐路径最优（完美知道哪几轮逃逸）⇒ |E|·s + s          ← 类内上限
  M_opt_site  站点级最优（同一站点只能选一只区域）= M_opt_path（同站点内 E 相同）
  M_opt_obj   逐对象理想（tracing GC：只留可达对象）⇒ 可达峰值 + 1

用法：python3 memory_gap.py
"""
import math
import sys

NS = (64, 256, 1024, 4096)          # 循环轮数
# 逃逸轮次集合 E(n) 的族 —— 这才是决定"阶"的东西
CONDS = {
    "从不逃逸 E=∅":        lambda n: frozenset(),
    "每轮都逃逸 E=[n]":     lambda n: frozenset(range(1, n + 1)),
    "一半逃逸 E=n/2":      lambda n: frozenset(range(1, n + 1, 2)),
    "√n 轮逃逸":           lambda n: frozenset(range(1, max(2, int(math.isqrt(n))) + 1)),
    "log n 轮逃逸":        lambda n: frozenset(range(1, max(2, int(math.log2(n))) + 1)),
    "只第一轮逃逸 E={1}":   lambda n: frozenset({1}),
    "只最后一轮逃逸 E={n}": lambda n: frozenset({n}),
}


def mem_model(E, n, s=1):
    """模型：E 非空 ⇒ 分析认为"可能逃逸" ⇒ 站点提升 ⇒ 所有 n 个对象都留到函数末。"""
    if not E:
        return s                                   # 没逃逸 ⇒ 不提升 ⇒ 每轮回收
    return n * s


def mem_opt_path(E, n, s=1):
    """逐路径最优：只有 |E| 个逃逸对象需要常驻长命区域，其余每轮回收。"""
    if not E:
        return s
    return len(E) * s + s


def mem_opt_obj(E, n, s=1, overwrite=True):
    """逐对象理想：只算**可达**对象。
    overwrite=True（覆盖式使用）⇒ 长命单元里只有 1 个可达；
    overwrite=False（累积成链）⇒ |E| 个可达。"""
    reach = (1 if overwrite else len(E)) if E else 0
    return reach * s + s


def classify(ratios, ns):
    """对 (n, ratio) 做 log-log 拟合，返回阶的名字。"""
    pts = [(math.log(n), math.log(r)) for n, r in zip(ns, ratios) if r > 0]
    if len(pts) < 2:
        return "?"
    (x1, y1), (x2, y2) = pts[0], pts[-1]
    slope = (y2 - y1) / (x2 - x1) if x2 != x1 else 0.0

    def near(v, t, tol=0.25):
        return abs(v - t) < tol

    if near(slope, 0.0):
        return "Θ(1)"
    if slope < 0.75:
        return "Θ(n^%.2f)" % slope
    if near(slope, 1.0):
        return "Θ(n)"
    return "Θ(n^%.2f)" % slope


def main():
    rows = []
    for name, f in CONDS.items():
        for overwrite in (True, False):
            ratios_path, ratios_obj = [], []
            for n in NS:
                E = f(n)
                mm = mem_model(E, n)
                ratios_path.append(mm / mem_opt_path(E, n))
                ratios_obj.append(mm / mem_opt_obj(E, n, overwrite=overwrite))
            rows.append((name, overwrite, classify(ratios_path, NS),
                         classify(ratios_obj, NS), ratios_path[-1], ratios_obj[-1]))

    print("=" * 100)
    print("Arena 内存精度差距：所有情况类别（n = 64 … 4096，对数-对数拟合定阶）")
    print("=" * 100)
    print(f"{'逃逸集合 E':<22}{'长命单元用法':<14}{'vs 逐路径上限':<16}{'vs 逐对象理想':<16}"
          f"{'比值@n=4096':>14}")
    print("-" * 100)
    for name, ow, cp, co, rp, ro in rows:
        use = "覆盖式(只留1)" if ow else "累积成链(留|E|)"
        print(f"{name:<22}{use:<14}{cp:<16}{co:<16}{rp:>8.1f} /{ro:>7.1f}")

    print()
    print("=" * 100)
    print("阈值定律：差距 ≈ n / (|E| + 1) ⇒ **超常数 ⟺ |E| = o(n)**")
    print("=" * 100)
    print("""
  类别                        条件                        vs 类内上限      出现比例*
  ─────────────────────────────────────────────────────────────────────────────
  ① 从不逃逸                  E = ∅                       Θ(1)            1/7
  ② 经常逃逸                  |E| = Θ(n)                  Θ(1)            2/7
  ③ 罕见逃逸（首/末轮）        |E| = Θ(1)                  Θ(n)            2/7
  ④ 次线性逃逸                |E| = Θ(√n) 或 Θ(log n)     Θ(√n) / Θ(n/log n)  2/7
  ─────────────────────────────────────────────────────────────────────────────
  * "出现比例"= 在本脚本枚举的这 7 种逃逸集合族里，等权计数的占比。
    真实程序里的比例取决于逃逸频率的分布 —— 见下面的诚实说明。
""")
    print("⇒ **差距 > O(1) 的判据**：逃逸**可能发生**（E ≠ ∅）但发生次数 **o(n)**。")
    print("   两端的 Θ(1)（从不逃逸 / 经常逃逸）都不构成长尾浪费。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
