#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pathsensitivity.py —— **实测**：引入（分支）路径敏感之后，接受集变不变？

结论（与 ExtCPath.lean §1 的定理一致）：**检查侧完全不变**。

为什么必须加 `C` 操作才测得出东西：`P a b` 的检查读的是"b 所在区域的深度"，
那是**静态**的，分支根本影响不到它。只有当被存的值是**从别处读出来的指针**
（`C a c`：把单元 c 的内容拷进 a）时，检查才读"分析记下的数"，
而那个数在分支合流处会被取 max —— 这才是路径敏感可能起作用的地方。

两种分析：
  join 版：分支处在抽象状态上取逐点 max（may-合流）
  path 版：分支各走各的，最后要求**每条路径都通过**

另附具体语义（含隐式、由内向外的作用域结束）作为真值。

用法：python3 pathsensitivity.py
"""
import itertools
import sys

PARENT = {0: None, 1: 0, 2: 1}
DEPTH = {0: 0, 1: 1, 2: 2}
REGIONS = (0, 1, 2)
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
        out.append(True)
        return
    op, rest = ops[0], ops[1:]
    if op[0] == 'P':
        _, a, b = op
        ptr = dict(ptr); ptr[a] = b
        if not safe_state(layout, ptr, dead):
            out.append(False); return
        run(layout, rest, ptr, dead, out)
    elif op[0] == 'C':
        _, a, c = op
        ptr = dict(ptr); ptr[a] = ptr[c]
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


def concretely_safe(layout, body):
    out = []
    run(layout, list(body), {i: None for i in range(len(layout))}, frozenset(), out)
    return all(out)


def well_scoped(layout, body):
    ok = [True]

    def go(ops, dead):
        if not ops or not ok[0]:
            return
        op, rest = ops[0], ops[1:]
        if op[0] == 'P':
            _, a, b = op
            if not (alive(layout[a], dead) and alive(layout[b], dead)):
                ok[0] = False; return
            go(rest, dead)
        elif op[0] == 'C':
            _, a, c = op
            if not (alive(layout[a], dead) and alive(layout[c], dead)):
                ok[0] = False; return
            go(rest, dead)
        elif op[0] == 'X':
            go(rest, dead | SUB[op[1]])
        else:
            go(list(op[1]) + list(rest), dead)
            go(list(op[2]) + list(rest), dead)

    go(list(body), frozenset())
    return ok[0]


# ---------------------------------------------------------------- 抽象分析
def joinD(d1, d2):
    return {c: (d1[c] if d2[c] is None else (d2[c] if d1[c] is None else max(d1[c], d2[c])))
            for c in d1}


def analyze(layout, body, Ds, mode):
    """Ds = 各路径的抽象状态。返回 None 表示拒绝，否则返回新的 Ds。"""
    for op in body:
        if op[0] == 'P':
            _, a, b = op
            if DEPTH[layout[b]] > DEPTH[layout[a]]:
                return None
            Ds = [{**D, a: DEPTH[layout[b]] if D[a] is None else max(D[a], DEPTH[layout[b]])}
                  for D in Ds]
        elif op[0] == 'C':
            _, a, c = op
            for D in Ds:                       # 检查读的是**分析记下的数**
                if D[c] is not None and D[c] > DEPTH[layout[a]]:
                    return None
            Ds = [{**D, a: D[c] if D[a] is None else (D[a] if D[c] is None else max(D[a], D[c]))}
                  for D in Ds]
        elif op[0] == 'X':
            pass                               # 释放对深度抽象不可见
        else:
            _, T, E = op
            DT = analyze(layout, T, [dict(D) for D in Ds], mode)
            if DT is None:
                return None
            DE = analyze(layout, E, [dict(D) for D in Ds], mode)
            if DE is None:
                return None
            Ds = DT + DE if mode == 'path' else [joinD(DT[0], DE[0])]
    return Ds


def accepts(layout, body, mode):
    D0 = {i: None for i in range(len(layout))}
    return analyze(layout, list(body), [D0], mode) is not None


# ---------------------------------------------------------------- 枚举
def bodies(ncells, maxlen):
    simple = ([('P', a, b) for a in range(ncells) for b in range(ncells)] +
              [('C', a, b) for a in range(ncells) for b in range(ncells)])
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
    rows = []
    for ncells, maxlen in ((2, 2),):
        for layout in itertools.product(REGIONS, repeat=ncells):
            for body in bodies(ncells, maxlen):
                full = tuple(body) + tuple(('X', r) for r in (2, 1, 0))
                if not well_scoped(layout, full):
                    continue
                rows.append((layout, full,
                             accepts(layout, full, 'join'),
                             accepts(layout, full, 'path'),
                             concretely_safe(layout, full)))

    diff = [r for r in rows if r[2] != r[3]]
    j_sound = sum(1 for r in rows if r[2] and not r[4])
    p_sound = sum(1 for r in rows if r[3] and not r[4])
    safe = [r for r in rows if r[4]]
    j_acc = sum(1 for r in safe if r[2])
    p_acc = sum(1 for r in safe if r[3])

    print("=" * 88)
    print("路径敏感 vs 合流：接受集是否改变（含 C 操作，检查会读到合流后的记账）")
    print("=" * 88)
    print(f"程序空间：单元数 2，区域链 0⊃1⊃2，操作 P/C/ite，长度 ≤ 2")
    print(f"总程序数 : {len(rows)}      具体安全: {len(safe)}")
    print()
    print(f"两种分析判定**不一致**的程序数 : {len(diff)}   {'✅ 完全一致' if not diff else '❌'}")
    print(f"合流版  健全性缺口（放行 ∧ 不安全）: {j_sound}")
    print(f"路径版  健全性缺口（放行 ∧ 不安全）: {p_sound}")
    print(f"合流版  精度（安全中被接受）: {j_acc}/{len(safe)} = {100.0*j_acc/len(safe):.2f}%")
    print(f"路径版  精度（安全中被接受）: {p_acc}/{len(safe)} = {100.0*p_acc/len(safe):.2f}%")
    print()
    print("⇒ 结论：**检查侧引入路径敏感不改变接受集**（既不更健全也不更宽），")
    print("   与 ExtCPath.lean 的 joinAccepts_iff_pathAccepts 一致。")
    print("   它真正的收益在**分配区域的选择**（§2 的定理：扩大接受集 + 省内存），")
    print("   代价是三条新护栏（条件的可重放/纯净/作用域）。")
    return 0 if (not diff and j_sound == 0 and p_sound == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
