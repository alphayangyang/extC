#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
precision.py —— **计算** extC 区域模型在有限程序空间上的精度。

"精度"先定义清楚，否则数字没有内容：

    具体语义   Safe(P)   = P 的**所有**执行路径都不出现"活单元指向已死区域"
    模型判定   Accept(P) = 分析在每一处存储检查都放行
    健全性缺口 = |Accept ∧ ¬Safe|      （放行了不安全程序 —— 必须是 0）
    精度损失   = |¬Accept ∧ Safe|      （拒绝了安全程序）
    精度       = |Accept ∧ Safe| / |Safe|

再加一条更有信息量的量：**被迫损失**。
被拒的安全程序 P，若存在 P'，使得
  ① 检查器看到的**抽象轨迹完全相同**（⇒ P' 也必被拒），且
  ② P' 具体执行**不安全**，
则这个拒绝**不可能靠改规则消除** —— 任何只看得见同一抽象轨迹的健全分析都必须拒绝它。
（定理形式见 ExtCPrecision.lean 的 `store_reject_iff` / `store_loss_forced`。）

建模片段（与 Lean 模型同构）：
  * 区域 0 ⊃ 1 ⊃ 2（嵌套链，深度 = 下标；越深越短命）
  * 单元创建时固定在某个区域；每个单元最多持有一个指针
  * P a b   —— 把"指向单元 b 的指针"放进单元 a；**唯一带检查的操作**：
              检查 `depth(region b) ≤ depth(region a)`
  * X r     —— 释放 subtree(r)（向下封闭）
  * ite T E —— 分支在编译期未知
  * 分析：每单元记一个"内容所指区域的深度"`D`；P 之后 `D[a] := max(D[a], depth b)`（弱更新）；
          ite 两支逐点 max（合流）；**X 不改变 D** ⇒ 它对深度抽象不可见，
          这正是"被迫损失"的来源。
"""
import itertools
import sys
import time

PARENT = {0: None, 1: 0, 2: 1}
DEPTH = {0: 0, 1: 1, 2: 2}
REGIONS = (0, 1, 2)


def ancestors(r):
    out = []
    while r is not None:
        out.append(r)
        r = PARENT[r]
    return out


def subtree(r):
    return frozenset(s for s in REGIONS if r in ancestors(s))


SUB = {r: subtree(r) for r in REGIONS}


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
        ptr = dict(ptr)
        ptr[a] = b
        if not safe_state(layout, ptr, dead):
            out.append(False)
            return
        run(layout, rest, ptr, dead, out)
    elif op[0] == 'X':
        dead2 = dead | SUB[op[1]]
        if not safe_state(layout, ptr, dead2):
            out.append(False)
            return
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
    """良作用域：每条路径上，P a b 执行时 a 与 b 所在区域都还活着。
    真实语言里写不出违反它的程序（名字已出作用域）——
    扁平化模型为了枚举方便才允许，它们是**建模假象**，须先滤掉。"""
    ok = [True]

    def go(ops, dead):
        if not ops or not ok[0]:
            return
        op, rest = ops[0], ops[1:]
        if op[0] == 'P':
            _, a, b = op
            if not (alive(layout[a], dead) and alive(layout[b], dead)):
                ok[0] = False
                return
            go(rest, dead)
        elif op[0] == 'X':
            go(rest, dead | SUB[op[1]])
        else:
            _, T, E = op
            go(list(T) + list(rest), dead)
            go(list(E) + list(rest), dead)

    go(list(body), frozenset())
    return ok[0]


# ---------------------------------------------------------------- 抽象分析
def analyze(layout, body, D, trace):
    for op in body:
        if op[0] == 'P':
            _, a, b = op
            dr = DEPTH[layout[b]]
            trace.append(('check', dr, DEPTH[layout[a]]))
            if dr > DEPTH[layout[a]]:
                return False, D
            D = dict(D)
            D[a] = dr if D[a] is None else max(D[a], dr)   # 弱更新
        elif op[0] == 'X':
            trace.append(('exit', op[1]))                  # 对深度抽象不可见
        else:
            _, T, E = op
            okT, DT = analyze(layout, T, dict(D), trace)
            okE, DE = analyze(layout, E, dict(D), trace)
            if not okT or not okE:
                return False, D
            D = {c: (DT[c] if DE[c] is None else (DE[c] if DT[c] is None else max(DT[c], DE[c])))
                 for c in D}
    return True, D


def accepted(layout, body):
    ok, _ = analyze(layout, list(body), {i: None for i in range(len(layout))}, [])
    return ok


def check_trace(layout, body):
    """检查器做决定所依据的抽象轨迹：只含检查点。"""
    tr = []
    analyze(layout, list(body), {i: None for i in range(len(layout))}, tr)
    return tuple(e for e in tr if e[0] == 'check')


# ---------------------------------------------------------------- 枚举
def bodies(ncells, maxlen):
    sets = [('P', a, b) for a in range(ncells) for b in range(ncells)]
    # 真实作用域严格由内向外结束 ⇒ 不再放显式 X，只靠末尾的隐式收尾
    exits = []
    simple = sets + exits
    res = []

    def build(cur):
        if cur:
            res.append(tuple(cur))
        if len(cur) >= maxlen:
            return
        for op in simple:
            cur.append(op)
            build(cur)
            cur.pop()
        for t in simple:
            for e in simple:
                cur.append(('I', (t,), (e,)))
                build(cur)
                cur.pop()

    build([])
    return res


def forced_twin(layout, body):
    """在**第一个被拒的检查点**上构造"同一抽象轨迹 + 具体不安全"的孪生。

    该点的记数 `dr` 是单元 b 内容可达深度的上确界；把深度为 dr 的那只区域释放掉，
    store 就真的把一个指向已死区域的指针写进了活着的 a —— 具体不安全，
    而抽象轨迹一字未变（X 对深度抽象不可见）。
    """
    res = []

    def walk(ops, D):
        if not ops:
            return None
        op, rest = ops[0], ops[1:]
        if op[0] == 'P':
            _, a, b = op
            dr = DEPTH[layout[b]]
            if dr > DEPTH[layout[a]]:
                r = dr                       # DEPTH 在本模型上单射
                body2 = tuple(list(res) + [op, ('X', r)])
                if check_trace(layout, body2) != check_trace(layout, tuple(list(res) + [op])):
                    return None
                if concretely_safe(layout, body2):
                    return None
                return body2
            D = dict(D)
            D[a] = dr if D[a] is None else max(D[a], dr)
            res.append(op)
            return walk(rest, D)
        elif op[0] == 'X':
            res.append(op)
            return walk(rest, D)
        else:
            _, T, E = op
            for br in (T, E):
                save = list(res)
                got = walk(list(br) + list(rest), dict(D))
                if got is not None:
                    return got
                res[:] = save
            return None

    return walk(list(body), {i: None for i in range(len(layout))})


def main():
    t0 = time.time()
    rows = []
    for ncells, maxlen in ((2, 3), (3, 2)):
        for layout in itertools.product(REGIONS, repeat=ncells):
            for body in bodies(ncells, maxlen):
                # 补上**隐式的作用域结束**：嵌套链由深到浅依次退出。
                # 真实语言里内层块一定会结束 —— 少了这一步，
                # "永远不退内层作用域"的程序会被误当成安全的（建模假象）。
                full = tuple(body) + tuple(('X', r) for r in (2, 1, 0))
                if not well_scoped(layout, full):
                    continue          # 写进已出作用域的单元：真实语言写不出
                rows.append((layout, full, accepted(layout, full), concretely_safe(layout, full)))

    acc_safe = sum(1 for r in rows if r[2] and r[3])
    acc_unsafe = sum(1 for r in rows if r[2] and not r[3])
    rej_safe = [r for r in rows if (not r[2]) and r[3]]
    rej_unsafe = sum(1 for r in rows if (not r[2]) and not r[3])
    safe_total = acc_safe + len(rej_safe)

    forced = sum(1 for (l, b, _, _) in rej_safe if forced_twin(l, b) is not None)
    unforced = [(l, b) for (l, b, _, _) in rej_safe if forced_twin(l, b) is None]

    print("=" * 92)
    print("extC 区域模型在有限程序空间上的精度（穷举）")
    print("=" * 92)
    print(f"程序空间：单元数 2（长度≤3）与 3（长度≤2），区域链 0⊃1⊃2，操作 P/X/ite")
    print("（已滤掉『写进已出作用域单元』的建模假象：真实语言里写不出那种程序）")
    print(f"总程序数 : {len(rows)}        耗时 {time.time()-t0:.1f}s")
    print(f"具体安全 : {safe_total}")
    print()
    print("                   ┌─── 具体安全 ───┬── 具体不安全 ──┐")
    print(f"  模型放行         │ {acc_safe:>14} │ {acc_unsafe:>14} │  ← 右上角必须是 0")
    print(f"  模型拒绝         │ {len(rej_safe):>14} │ {rej_unsafe:>14} │")
    print("                   └────────────────┴────────────────┘")
    print()
    print(f"健全性缺口（放行 ∧ 不安全） : {acc_unsafe}  {'✅' if acc_unsafe == 0 else '❌'}")
    if safe_total:
        print(f"精度（安全中被接受）       : {acc_safe}/{safe_total} = {100.0*acc_safe/safe_total:.2f}%")
        print(f"精度损失（拒绝 ∧ 安全）    : {len(rej_safe)}  ({100.0*len(rej_safe)/safe_total:.2f}% 的安全程序)")
    print(f"其中**被迫**（存在同一抽象轨迹的不安全孪生）: {forced}/{len(rej_safe)}")
    print(f"⇒ 可去掉的精度损失（真正可改进的）        : {len(unforced)}")
    if unforced[:3]:
        print("  未被判为被迫的例子：")
        for l, b in unforced[:3]:
            print(f"    layout={l} body={b}")

    nb = [r for r in rows if all(op[0] != 'I' for op in r[1])]
    nb_bad = sum(1 for r in nb if r[2] != r[3])
    print()
    print(f"无分支片段（{len(nb)} 个程序）：|接受 ⟺ 安全| 的例外数 = {nb_bad} "
          f"{'✅ 该片段内模型完全精确' if nb_bad == 0 else '❌'}")
    return 0 if (acc_unsafe == 0 and not unforced) else 1


if __name__ == "__main__":
    sys.exit(main())
