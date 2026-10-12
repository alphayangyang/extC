#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gap_to_ideal.py —— 四条口径下的残余差距：到底还剩多少。

口径（逐条对应一个可证/可实现的东西）：

  静态           今天的做法：站点"可能逃逸"⇒ 提到长命区域 ⇒ E≠∅ 时 n 份全留
  路径敏感       静态按路径选落点（ExtCPath.lean）；代价 = 三条护栏（条件必须可重放）
  运行期事件     在**真实**逃逸点提权、在**真实**杀点上回收（本文档验的机制）
                 · 它就是路径敏感，但**不需要重放条件** ⇒ 不需要护栏
                 · 每次动作 O(1)、无 STW（块转交/释放都是指针操作，见 ExtCBlock.lean）
                 · **不上引用计数**（extC 的设计选择）⇒ 环不构成独立类别

理想 = 逐对象可达（tracing GC 会留的）。差距 = 该口径常驻 / 理想。

形状轴只有两个，都是**语义**性质，不是概率：

  kill 可观察？    杀点是不是运行期事件（覆盖 / 容器摘除 / 置空）——可观察就能当场回收
  多持有者？       对象是否可能有多于一个持有者——是则"一个杀点"不等于"死亡"
                   这一轴底下"环"不是独立类别：环 = 持有者在**另一个对象内部**，与
                   "两个名字都持有"同一件事（③′）。没有引用计数就没有环这个残差。

用法：python3 gap_to_ideal.py
"""
import math
import sys

NS = (64, 256, 1024, 4096)
POLICIES = ("静态", "路径敏感", "运行期事件")


def retained(policy, n, e, single_holder, cumulative):
    """长命区域常驻份数。e = |E| 真正逃逸的轮数。
    cumulative=True 表示对象累积进容器且仍可达 ⇒ 任何口径都得留着（不是浪费）。"""
    if e == 0:
        return 0
    if cumulative:
        return e
    if policy == "静态":
        return n                                  # 只要可能逃逸 ⇒ 全部留
    if policy == "路径敏感":
        return e                                  # 只有真逃逸的留（静态重放条件）
    if policy == "运行期事件":
        return 1 if single_holder else e           # 单持有 ⇒ 覆盖即回收；多持有 ⇒ 留着
    raise AssertionError(policy)


def ideal(e, cumulative):
    """逐对象理想的常驻：覆盖式 = 任意时刻 1 份；累积进容器 = |E| 份（它们确实可达）。"""
    if e == 0:
        return 0
    return e if cumulative else 1


def gap(policy, n, e, single_holder, cumulative):
    i = ideal(e, cumulative)
    return retained(policy, n, e, single_holder, cumulative) / i if i else 1.0


def order(vals):
    pts = [(math.log(n), math.log(v)) for n, v in zip(NS, vals) if v > 0]
    if len(pts) < 2:
        return "Θ(1)"
    (x1, y1), (x2, y2) = pts[0], pts[-1]
    sl = (y2 - y1) / (x2 - x1) if x2 != x1 else 0.0
    if abs(sl) < 0.25:
        return "Θ(1)"
    if abs(sl - 1) < 0.25:
        return "Θ(n)"
    return "Θ(n^%.2f)" % sl


# 名字 -> (说明, 单持有者, 有环, |E| 随 n 的取法)
SHAPES = {
    "① 具名·唯一引用": ("`if c { outer = p }` 覆盖式，只有一个名字", True, "rare", False),
    "② 具名·别名已置空": ("`other = p` 后 `other = null`", True, "rare", False),
    "③ 二选一持有": ("`if g { ok = p } else { err = p }` 两个名字可能持有", False, "rare", False),
    "③′ 持有者在结构内部": ("`a.next = b`（环是它的极端：互指）——与 ③ 同类", False, "rare", False),
    "④ 容器·没被摘掉": ("`arr.push(p)`，元素还在容器里（确实可达）", True, "every", True),
    "④′ 容器·已摘掉": ("`arr.push(p)` 后 `arr.pop()`，摘除是运行期事件", True, "rare", False),
}

E_OF = {"rare": lambda n: max(2, int(math.isqrt(n))), "every": lambda n: n}


def main():
    bad = []
    e_of = lambda name, n: E_OF[SHAPES[name][2]](n)
    cum_of = lambda name: SHAPES[name][3]

    print("=" * 112)
    print("残余差距（该口径常驻 / 逐对象理想）")
    print("=" * 112)
    print(f"{'形状':<20}{'判据':<34}" + "".join(f"{p:>13}" for p in POLICIES))
    print("-" * 112)
    rows = {}
    for name, (desc, single, _, cum) in SHAPES.items():
        cells = []
        for p in POLICIES:
            vals = [gap(p, n, e_of(name, n), single, cum) for n in NS]
            rows[(name, p)] = vals
            cells.append(f"{order(vals):>13}")
        print(f"{name:<20}{desc:<34}" + "".join(cells))

    print()
    # A1：运行期事件在「单持有者」形状上 = 理想（不需要护栏、不需要计数）
    for name in ("① 具名·唯一引用", "② 具名·别名已置空", "④′ 容器·已摘掉"):
        for n in NS:
            g = gap("运行期事件", n, e_of(name, n), True, False)
            if abs(g - 1.0) > 1e-9:
                bad.append(f"{name} 运行期事件 n={n}: gap={g}，应为 1")
        print(f"  A1 {name}：运行期事件 ⇒ gap ≡ 1 ✓（O(1)/次事件、无 STW、无护栏）")

    # A2：④（元素还在容器里）本来就不该回收 ⇒ 四条口径都 = 理想
    for p in POLICIES:
        for n in NS:
            g = gap(p, n, e_of("④ 容器·没被摘掉", n), True, True)
            if abs(g - 1.0) > 1e-9:
                bad.append(f"④ {p} n={n}: gap={g}，应为 1")
    print("  A2 ④ 容器·没被摘掉：元素确实可达（理想也是 n）⇒ 四条口径 gap ≡ 1 ✓")

    # A3：③ / ③′（多持有者）—— 运行期事件不够，残差留在这里
    for name in ("③ 二选一持有", "③′ 持有者在结构内部"):
        v = rows[(name, "运行期事件")]
        if order(v) == "Θ(1)":
            bad.append(f"{name} 运行期事件不应为 Θ(1)：{order(v)}")
        print(f"  A3 {name}：运行期事件 {order(v)} —— 残差（多持有者，事件不足以判定「是不是最后一个」）")

    print()
    if bad:
        print("断言失败：")
        for b in bad:
            print("  ·", b)
        return 1
    print("★ 无 rc 时的结论：")
    print("  · 运行期事件关掉 ① ② ④′（唯一引用 + 事件可观察）⇒ Θ(1)，O(1)/次事件、无 STW、无护栏")
    print("  · 残差只剩**多持有者**（③ 别名、③′ 持有者在结构内部——**环只是它的极端情形**）")
    print("  · 无 rc ⇒ 没有「环」这个独立类别：环就是「持有者在别的对象里」，与 ③ 同一件事")
    print("⚠️ |E| 的取值是参数化的（同 memory_gap.py）；形状轴（可观察/多持有）是语义性质。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
