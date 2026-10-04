#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
crosscheck_z3.py —— 用 SMT（Z3）**独立**复核 ExtCRegion.lean / ExtCPool.lean 的边界结论。

定位（诚实说明）：
  * 主证明是 Lean 的（`verify.sh`），那里是无界的一般定理；
  * 本脚本是**小规模**（2 个区域 / 2 步轨迹）的独立交叉复核 ——
    它在有限宇宙上逐点实例化 `Frame` 公理，因此：
      - `sat` ⇒ 该边界条件**确实**不可省（反例真实存在）；
      - `unsat` ⇒ 在该小规模内成立（不是一般性证明，一般性由 Lean 给）。
  * 用另一个证明器、另一条推理路径复核同一批命题，能抓出"建模写错"这类错。

用法：
    PYTHONPATH=<z3 库路径> python3 crosscheck_z3.py
"""
import sys

try:
    from z3 import And, Bool, Implies, Int, Not, Or, Solver, sat, unsat
except ImportError:  # pragma: no cover
    print("缺少 z3（pip install z3-solver，或设置 PYTHONPATH 指向解包后的 wheel）")
    sys.exit(2)

REG = [0, 1]          # 0 = 值所指的目标区域；1 = 目的地（本激活的局部区域）
RESULTS = []


def check(name, hypotheses, goal, expect, note=""):
    """在 hyps ∧ ¬goal 上求解；expect 为 'unsat'（该结论在此规模成立）或 'sat'（有反例）。"""
    s = Solver()
    s.add(*hypotheses)
    s.add(Not(goal))
    r = s.check()
    got = "unsat" if r == unsat else ("sat" if r == sat else "unknown")
    ok = (got == expect)
    RESULTS.append((name, expect, got, ok, note))
    return ok, s


def frame_axioms(Local, Ground, outl, d):
    """Frame 的结构公理在有限宇宙上的逐点实例化。"""
    ax = []
    for a in REG:
        ax.append(outl[a][a])                                    # out_refl
        for b in REG:
            for c in REG:
                ax.append(Implies(And(outl[a][b], outl[b][c]), outl[a][c]))   # out_trans
            ax.append(Implies(outl[a][b], d[a] <= d[b]))                      # depth_mono
            ax.append(Implies(Ground[a], d[a] == 0))                          # depth_ground
            ax.append(Implies(And(Ground[a], Local[b]), outl[a][b]))          # ground_outlives
    return ax


def fresh_frame():
    Local = {r: Bool(f"Local_{r}") for r in REG}
    Ground = {r: Bool(f"Ground_{r}") for r in REG}
    outl = {a: {b: Bool(f"outl_{a}_{b}") for b in REG} for a in REG}
    d = {r: Int(f"d_{r}") for r in REG}
    return Local, Ground, outl, d


def main():
    # ---------- 区域档：存储规则 ----------
    Local, Ground, outl, d = fresh_frame()
    ax = frame_axioms(Local, Ground, outl, d)

    target, dest = 0, 1
    dr = Int("dr")

    alive = {r: Bool(f"alive_{r}") for r in REG}
    common = ax + [
        alive[target], alive[dest],
        Local[dest],                        # 目的地是本激活的局部地方
        d[target] <= dr,                    # INV-H：记账深度是真实深度的上界
        dr <= d[dest],                      # 检查器接受（d ≤ at）
    ]
    # ChainOK：同时活着且局部的两个区域，深度序 ⇒ 寿命序
    chain = [Implies(And(alive[a], alive[b], Local[a], Local[b], d[a] <= d[b]), outl[a][b])
             for a in REG for b in REG]
    cc = Local[target] if False else Or(Local[target], Ground[target])   # CC（点态）

    # Q1 主定理：全部边界条件在场 ⇒ 结论成立
    check("Q1 区域档主定理（CC+ChainOK+INV-H ⇒ outlives）",
          common + chain + [cc], outl[target][dest], "unsat",
          "对应 Lean: Runs.safe / Step.contains")

    # Q2 CC 必要：去掉 CC
    check("Q2 去掉 CC ⇒ 反例存在",
          common + chain, outl[target][dest], "sat",
          "对应 Lean: ForeignRegion.necessity_CC")

    # Q3 ChainOK 必要：去掉 ChainOK（目标改为局部，使 CC 成立）
    Local2 = {r: Bool(f"L2_{r}") for r in REG}
    Ground2 = {r: Bool(f"G2_{r}") for r in REG}
    outl2 = {a: {b: Bool(f"o2_{a}_{b}") for b in REG} for a in REG}
    d2 = {r: Int(f"d2_{r}") for r in REG}
    ax2 = frame_axioms(Local2, Ground2, outl2, d2)
    dr2 = Int("dr2")
    check("Q3 去掉 ChainOK ⇒ 反例存在（两个同时活着的局部区域）",
          ax2 + [alive[target], alive[dest], Local2[dest], Local2[target],
                 d2[target] <= dr2, dr2 <= d2[dest],
                 Or(Local2[target], Ground2[target])],
          outl2[target][dest], "sat",
          "对应 Lean: chainOK_independent")

    # Q4 INV-H 必要：把记账写成真值之下
    check("Q4 去掉 INV-H（记账偏小：dr < 真实深度）⇒ 反例存在",
          ax + chain + [alive[target], alive[dest], Local[dest],
                        Or(Local[target], Ground[target]),
                        d[target] > dr,      # ← 记账比真实深度小
                        dr <= d[dest]],
          outl[target][dest], "sat",
          "对应 Lean: Undershoot.necessity_INVH")

    # ---------- 池档：世代契约 ----------
    g0, g1, g2 = Int("g0"), Int("g1"), Int("g2")
    o0, o1, o2 = Int("o0"), Int("o1"), Int("o2")   # 占用者 id，-1 表示空闲
    mono = [g0 <= g1, g1 <= g2]
    contract = [Implies(o1 != o0, g0 < g1), Implies(o2 != o1, g1 < g2)]

    check("Q5 池档身份：换代契约 ⇒ 世代不变则占用者不变",
          mono + contract, Implies(g2 == g0, o2 == o0), "unsat",
          "对应 Lean: handle_does_not_rebind")

    check("Q6 去掉换代契约 ⇒ ABA（旧句柄指向另一个对象）",
          mono, Implies(g2 == g0, o2 == o0), "sat",
          "对应 Lean: necessity_generation")

    check("Q7 允许世代回绕 ⇒ ABA（远古句柄复活）",
          contract, Implies(g2 == g0, o2 == o0), "sat",
          "对应 Lean: wrap_not_good")

    # ---------- 报表 ----------
    width = max(len(n) for n, *_ in RESULTS)
    print("=" * 100)
    print("SMT 交叉复核（小规模：2 区域 / 2 步；主证明在 Lean 侧，此处只做独立旁证）")
    print("=" * 100)
    bad = 0
    for name, expect, got, ok, note in RESULTS:
        flag = "  OK " if ok else " FAIL"
        print(f"[{flag}] {name:<{width}}  期望={expect:<5} 实际={got:<5} | {note}")
        if not ok:
            bad += 1
    print("-" * 100)
    print(f"{len(RESULTS) - bad}/{len(RESULTS)} 条与 Lean 结论一致")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
