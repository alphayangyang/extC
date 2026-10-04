#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
precision_gap.py —— **当前模型距离「无显式标注 + 静态分配控制」这一类上限的精度距离。**

先把三样东西定义清楚（含糊了数字就没意义）：

  程序 P：n 个**分配点** + 若干 `P a b`（把"指向 b 的指针"存进 a）+ `ite`；
          作用域按真实语义由内向外结束（隐式 exit 2,1,0）。

  每个分配点有一个**允许落点集合** allowed[i]，这是 extC 的真实结构：

    * **栈局部**：区域由**声明作用域定死** ⇒ allowed = {scope}
      （promotion 救不了它 —— 这正是"拒绝"的主要来源）
    * **new 出来的对象**：可提升到作用域及其所有祖先 ⇒ allowed = {0..scope}

  三层：

    Opt(P)      ≝ ∃ α ∈ ∏allowed，使 P 的**所有**执行都安全
                  —— 「静态分配控制 + 无标注」这一类的**上限**（允许完美推断）
    Best(P)     ≝ ∃ α ∈ ∏allowed，使**模型判据**全部放行
                  —— 与 Opt 的差 = **规则层距离**
    Inferred(P) ≝ 模型用**它自己的推断**（文档口径：从最深处出发只往下调，
                  取约束系统的最大解）能否放行 ⇒ 与 Best 的差 = **推断层距离**

  嵌套性 Inferred ⊆ Best ⊆ Opt 由脚本断言。

用法：python3 precision_gap.py
"""
import itertools
import sys
import time

PARENT = {0: None, 1: 0, 2: 1}
DEPTH = {0: 0, 1: 1, 2: 2}
REGIONS = (0, 1, 2)
EXIT_ORDER = (2, 1, 0)
# 分配点的"允许落点集合"：栈局部（作用域 0/1/2）与可提升的 new（作用域 1/2）
ALLOWED_OPTIONS = [
    (0,),           # 栈局部，作用域 0
    (1,),           # 栈局部，作用域 1
    (0, 1),         # new，作用域 1
    (2,),           # 栈局部，作用域 2
    (0, 1, 2),      # new，作用域 2
]


def ancestors(r):
    out = []
    while r is not None:
        out.append(r)
        r = PARENT[r]
    return out


SUB = {r: frozenset(s for s in REGIONS if r in ancestors(s)) for r in REGIONS}


def alive(r, dead):
    return all(a not in dead for a in ancestors(r))


# ---------------------------------------------------------------- 具体语义
def safe_state(layout, ptr, dead):
    for a, b in ptr.items():
        if b is None:
            continue
        if alive(layout[a], dead) and not alive(layout[b], dead):
            return False
    return True


def run(layout, ops, ptr, dead, out):
    if not ops:
        out.append(True); return
    op, rest = ops[0], ops[1:]
    if op[0] == 'P':
        _, a, b = op
        ptr = dict(ptr); ptr[a] = b
        if not safe_state(layout, ptr, dead):
            out.append(False); return
        run(layout, rest, ptr, dead, out)
    elif op[0] == 'X':
        dead2 = dead | SUB[op[1]]
        if not safe_state(layout, ptr, dead2):
            out.append(False); return
        run(layout, rest, ptr, dead2, out)
    else:
        _, T, E = op
        run(layout, list(T) + list(rest), dict(ptr), dead, out)
        run(layout, list(E) + list(rest), dict(ptr), dead, out)


def concretely_safe(layout, body, n):
    out = []
    run(layout, list(body) + [('X', r) for r in EXIT_ORDER],
        {i: None for i in range(n)}, frozenset(), out)
    return all(out)


# ---------------------------------------------------------------- 模型判据
def stores_of(body):
    for op in body:
        if op[0] == 'P':
            yield op
        elif op[0] == 'I':
            yield from stores_of(op[1])
            yield from stores_of(op[2])


def model_accepts(layout, body):
    for _, a, b in stores_of(body):
        if DEPTH[layout[b]] > DEPTH[layout[a]]:
            return False
    return True


def solve(body, allowed, n):
    """文档口径的推断：约束 α(b) ≤ α(a)（对每个 P a b），
    从最深（最省内存）出发只往下调，取不动点 = 允许集内的最大解。
    越出允许集下界 ⇒ 无解（返回 None）。"""
    alpha = [max(allowed[i]) for i in range(n)]
    for _ in range(4 * n + 4):
        changed = False
        for _, a, b in stores_of(body):
            if alpha[b] > alpha[a]:
                if alpha[a] < min(allowed[b]):
                    return None                    # 栈对象提升不了 ⇒ 无解
                alpha[b] = alpha[a]
                changed = True
        if not changed:
            break
    return tuple(alpha)


def model_inferred(body, allowed, n):
    a = solve(body, allowed, n)
    return a is not None and model_accepts(a, body)


# ---------------------------------------------------------------- 枚举
def bodies(n, maxlen):
    simple = [('P', a, b) for a in range(n) for b in range(n)]
    res = []

    def build(cur):
        if cur:
            res.append(tuple(cur))
        if len(cur) >= maxlen:
            return
        for op in simple:
            cur.append(op); build(cur); cur.pop()
        for t in simple:
            for e in simple:
                cur.append(('I', (t,), (e,))); build(cur); cur.pop()

    build([])
    return res


def main():
    t0 = time.time()
    rows = []
    for n, maxlen in ((2, 3), (3, 2)):
        for allowed in itertools.product(ALLOWED_OPTIONS, repeat=n):
            cands = list(itertools.product(*allowed))
            for body in bodies(n, maxlen):
                opt = any(concretely_safe(l, body, n) for l in cands)
                best = any(model_accepts(l, body) for l in cands)
                inf = model_inferred(body, allowed, n)
                rows.append((n, allowed, body, opt, best, inf))

    bad = [r for r in rows if (r[5] and not r[4]) or (r[4] and not r[3])]
    n_opt = sum(1 for r in rows if r[3])
    n_best = sum(1 for r in rows if r[4])
    n_inf = sum(1 for r in rows if r[5])
    g_rule = sum(1 for r in rows if r[3] and not r[4])
    g_inf = sum(1 for r in rows if r[4] and not r[5])
    g_tot = sum(1 for r in rows if r[3] and not r[5])

    print("=" * 94)
    print("当前模型 ↔ 「无显式标注 + 静态分配控制」类上限 的精度距离（穷举）")
    print("=" * 94)
    print("程序空间：分配点 2（长度≤3）与 3（长度≤2），区域链 0⊃1⊃2，操作 P/ite")
    print("          每个分配点带『允许落点集合』：栈局部={scope}，new={0..scope}")
    print(f"程序实例数 : {len(rows)}     耗时 {time.time()-t0:.1f}s")
    print()
    print(f"  Opt(P)      （∃ 允许集内的安全分配） : {n_opt:>8}")
    print(f"  Best(P)     （模型判据 + 神谕分配）  : {n_best:>8}")
    print(f"  Inferred(P) （模型判据 + 自身推断）  : {n_inf:>8}")
    print()
    print(f"  嵌套性 Inferred ⊆ Best ⊆ Opt 违例 : {len(bad)}   {'✅' if not bad else '❌'}")
    print()
    print(f"  ① **规则层距离** |Opt \\ Best|     = {g_rule:>8}   占上限 {100.0*g_rule/max(n_opt,1):.2f}%")
    print(f"  ② **推断层距离** |Best \\ Inferred| = {g_inf:>8}   占上限 {100.0*g_inf/max(n_opt,1):.2f}%")
    print(f"  =  **总距离**    |Opt \\ Inferred| = {g_tot:>8}   占上限 {100.0*g_tot/max(n_opt,1):.2f}%")
    print()
    print(f"  ⇒ 当前模型相对这一类上限的精度 = Inferred/Opt = {100.0*n_inf/max(n_opt,1):.2f}%")

    def witness(pred, k=3):
        outs = []
        for n, allowed, body, opt, best, inf in rows:
            if pred(opt, best, inf):
                outs.append((n, allowed, body))
                if len(outs) >= k:
                    break
        return outs

    print()
    print("① 规则层距离的最小见证（存在安全分配，模型判据在任何允许分配下都拒）：")
    for n, al, b in witness(lambda o, be, i: o and not be):
        print(f"    n={n} allowed={al} body={b}")
    print("② 推断层距离的最小见证（神谕分配能过，模型自己的推断过不了）：")
    for n, al, b in witness(lambda o, be, i: be and not i):
        print(f"    n={n} allowed={al} body={b}  推断出={solve(b, al, n)}")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
