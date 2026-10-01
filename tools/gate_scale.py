#!/usr/bin/env python3
"""Gate 4 · 规模与嵌套：编译器对任何输入都必须**在有限时间内给出结论**。

这是审计 §7 闸门 ④。审计 §6 的九个性能问题有同一个形状：递归/循环没有预算，
输入稍大就从"慢"变成"挂住"或"栈溢出"（P0-10 两类栈溢出、P0-22 死循环、blkMax 指数）。
本闸门不要求快，只要求**有结论**：退出码 ∈ {0,1}，且在各自的时间预算内。

用法：
    python3 tools/gate_scale.py [--update-allowlist] [--jobs N]

判据（tools/gate-scale-known-bad.txt 是基线）：
    · 新增"崩溃/超时/超预算" ⇒ 红；基线里已不再失败的条目 ⇒ 红（棘轮）。
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

ALLOW = os.path.join(G.ROOT, "tools", "gate-scale-known-bad.txt")


def nested_blocks(n):
    return "fn main() -> i32 {\n" + "    {\n" * n + "    }\n" * n + "    return 0\n}\n"


def nested_parens(n):
    return ("fn main() -> i32 {\n    var x: i32 = " + "(" * n + "1" + ")" * n
            + "\n    return x\n}\n")


def long_expr(n):
    return ("fn main() -> i32 {\n    var x: i64 = 0" + " + 1" * n + "\n    return i32(x)\n}\n")


def many_stmts(n):
    body = "".join(f"    var v{i}: i64 = i64({i})\n" for i in range(n))
    return "fn main() -> i32 {\n" + body + "    return 0\n}\n"


def many_instances(n):
    body = "".join(f"    var p{i}: pair<i64> = pair<i64> {{ a: i64({i}), b: i64(0) }}\n"
                   for i in range(n))
    return ("struct pair<T> { a: T  b: T }\nfn main() -> i32 {\n" + body + "    return 0\n}\n")


def nested_types(n):
    d = "i64"
    for _ in range(n):
        d = f"box<{d}>"
    return ("struct box<T> { v: T }\nfn z<T>() -> i64 { return i64(0) }\n"
            f"fn main() -> i32 {{ return i32(z<{d}>()) }}\n")


# 每个用例：名字、源码、时间预算（秒）、内存上限（MB）。
# 预算按"最慢的合法路径"定，不是按当前实现的性能；内存上限保证闸门不会把机器拖垮
# （实测：2000 个局部变量就吃 1.3GB，20000 个不设限会 OOM 掉整个会话）。
CASES = [
    ("blocks30", nested_blocks(30), 2.0, 2048),        # blkMax 指数（审计 P2-1）
    ("blocks60", nested_blocks(60), 2.0, 2048),        # 预算内必须能编译（P0-10 的边界正例）
    ("blocks200", nested_blocks(200), 5.0, 2048),      # 同上（指数）→ 修好后应达预算
    ("blocks1000", nested_blocks(1000), 10.0, 2048),   # dataflow 栈溢出（审计 P0-10）
    ("parens2000", nested_parens(2000), 5.0, 2048),
    ("parens16000", nested_parens(16000), 10.0, 2048),  # parser 递归（审计 P0-10）
    ("expr5000", long_expr(5000), 5.0, 2048),
    ("stmts2000", many_stmts(2000), 8.0, 4096),        # 实测 1.6s / 1.3GB
    ("stmts20000", many_stmts(20000), 20.0, 4096),     # 实测给 "out of memory" 诊断（rc=1）
    ("instances3000", many_instances(3000), 10.0, 2048),
    # 审计 P0-22：dropRuntimeDefs 的 continue 不推进 ⇒ 52 行的 stdlib 文件让编译器
    # 100% CPU 永不返回（>15 分钟，无诊断）。修好后应在毫秒级完成。
    ("hashset_root", ("file", "stdlib/stl/hashSet.extc"), 3.0, 2048),
    ("types64", nested_types(64), 5.0, 2048),
    ("types65", nested_types(65), 5.0, 2048),          # 超限必须报错而不是 SIGSEGV（审计 P0-4）
]


def one_case(case, verbose=False):
    name, src, budget, mem_mb = case
    d = os.path.join(G.GATEDIR, "scale")
    os.makedirs(d, exist_ok=True)
    if verbose:
        print(f"  ... {name}（预算 {budget}s，内存上限 {mem_mb}MB）", flush=True)
    if isinstance(src, tuple) and src and src[0] == "file":
        p = os.path.join(G.ROOT, src[1])          # a real repo file: do not rewrite it
    else:
        p = os.path.join(d, name + ".extc")
        with open(p, "w", encoding="utf-8") as f:
            f.write(src)
    t0 = time.time()
    rc, out = G.run([G.EXTC, "-w", p, "-o", os.path.join(d, name + ".c")],
                    timeout=max(15, int(budget * 3)), mem_mb=mem_mb)
    dt = time.time() - t0
    if rc == 124:
        return (name, f"超时（>{int(budget * 6)}s）")
    if rc < 0 or rc > 1:
        return (name, f"退出码 {rc}（崩溃）: {' '.join(out.split())[:120]}")
    if dt > budget:
        return (name, f"耗时 {dt:.1f}s 超过预算 {budget}s（有结论但太慢）")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    G.ensure_gatedir()
    os.makedirs(os.path.join(G.GATEDIR, "scale"), exist_ok=True)
    # Sequential on purpose: 11 cases, each with its own timeout, and a pool would hide
    # progress behind ordered results where one hanging case delays every message.
    fails = {}
    for c in CASES:
        r = one_case(c, verbose=True)
        if r:
            fails[r[0]] = r[1]
            print(f"  FAIL {r[0]}: {r[1].splitlines()[0]}", flush=True)
        else:
            print(f"  ok   {c[0]}", flush=True)
    print(f"[scale] 跑了 {len(CASES)} 个规模用例（要结论，不要崩溃/挂住）")
    if a.update_allowlist:
        G.write_allow(ALLOW, fails)
        return 0
    return G.report("scale", fails, G.load_allow(ALLOW), detail_lines=2,
                    eligible={c[0] for c in CASES})


if __name__ == "__main__":
    sys.exit(main())
