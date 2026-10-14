#!/usr/bin/env python3
"""闸门·idle 投影（规则 P）：目标形状必须判 idle，护栏形状必须仍付钱。

## 它守什么

"纯税族"（建好—遍历—块退出整片丢）现在付 1.0–2.5× 的墙钟却不省内存，原因是 idle 判据卡在
D/E/C 三条上。要放宽它们需要一条"**覆盖一个槽不丢根**"的规则（投影，见
[`IDLE-PROJECTION-RECON.md`](IDLE-PROJECTION-RECON.md) 的规则 P）。这条闸门把"放宽到什么程度"
钉成两侧：

* **红集**（目标 = 判 idle）：默认档产物必须与 `EXTC_RECLAIM=0` **逐字节相同**（= 不发射回收
  运行期）。今天全部不相等 ⇒ 红。
* **护栏**（必须仍付钱）：真的丢引用的形状 **不许**判 idle（否则那些垃圾就没人收），且默认档
  N vs 4N 峰值比 <1.5×、语义与关档逐字相同。
* 语义：两侧都要"默认档 stdout == 关档 stdout"。

判 idle 为什么不需要额外的内存界判据：判 idle ⇒ 产物与关档**逐字节相同** ⇒ 内存就是程序自己的
活集（不可能因判错而多攒）。所以"护栏不许 idle"就是那条安全边界本身。

## 棘轮

`tools/gate-idle-projection-known-bad.txt` 记今天还不成立的 `形状<TAB>判据`：只许缩。
红集转绿要删行；护栏变红（idle / 超界 / 语义）立刻红 —— 那条是安全问题，不是欠账。

用法：
    python3 tests/reclaim-perf/gate-idle-projection.py
    python3 tests/reclaim-perf/gate-idle-projection.py --update-baseline
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gatecommon as G  # noqa: E402

BASE = os.path.join(ROOT, "tools", "gate-idle-projection-known-bad.txt")
TMP = "/tmp/extc-idle-projection"
CC = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fwrapv", "-O2"]

# (相对路径, 小规模, 大规模)。红集里活集随 N 涨的形状只判"是否 idle"，不判峰值比。
RED = [
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_chain.extc", 200000, 800000),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_tree.extc", 200000, 800000),
    ("tests/reclaim-perf/shapes/refgraph/rg_scan_parent.extc", 200000, 800000),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_buckets.extc", 200000, 800000),
    ("tests/reclaim-perf/shapes/refgraph/rg_insert_hash.extc", 200000, 800000),
    ("tests/reclaim-perf/shapes/refgraph/rg_grow_tail.extc", 100000, 400000),
    ("tests/reclaim-perf/shapes/refgraph/rg_scan_graph.extc", 100000, 400000),
    ("bench/gc/src/spectrum_p2.extc", 500000, 2000000),
    ("bench/gc/src/spectrum_p4.extc", 5000, 20000),
]

# 护栏：真的丢引用 ⇒ 必须仍 PAYS 且默认档有界。第二列 = 活集是否与规模无关（决定判不判峰值比）。
GUARD = [
    ("tests/reclaim-perf/shapes/refgraph/rg_relink_delete.extc", 100000, 400000, False),
    ("tests/reclaim-perf/shapes/refgraph/rg_overwrite_ring.extc", 500000, 2000000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_array.extc", 500000, 2000000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_evict_cache.extc", 500000, 2000000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_drop_component.extc", 100000, 400000, True),
    ("tests/reclaim-gaps/shapes/ring_per_round.extc", 20000, 80000, True),
]
RSS_LIMIT = 1.5


def gen(src, dst, extra):
    env = dict(os.environ)
    env.update(extra)
    return subprocess.run([G.EXTC, "-w", "-I", os.path.join(ROOT, "stdlib"), src, "-o", dst],
                          capture_output=True, text=True, env=env, timeout=180)


def build(src, tag, extra):
    c, b = os.path.join(TMP, tag + ".c"), os.path.join(TMP, tag)
    if gen(src, c, extra).returncode != 0:
        return None, None
    if subprocess.run(CC + ["-o", b, c], capture_output=True).returncode != 0:
        return None, None
    return b, c


def run(binary, n, limit=180):
    try:
        p = subprocess.run([binary, str(n)], capture_output=True, text=True, timeout=limit)
        return p.stdout, p.returncode
    except subprocess.TimeoutExpired:
        return "", 124


def peak_rss(binary, n, limit=180):
    try:
        p = subprocess.run(["/usr/bin/time", "-f", "%M", binary, str(n)],
                           capture_output=True, text=True, timeout=limit)
    except subprocess.TimeoutExpired:
        return None
    last = (p.stderr.strip().splitlines() or [""])[-1].strip()
    return int(last) if last.isdigit() else None


def classify(src, name):
    """返回 (是否 idle, 默认档二进制, 关档二进制, 默认 C 文本, 关档 C 文本)。"""
    d, dc = build(src, name + ".def", {})
    o, oc = build(src, name + ".off", {"EXTC_RECLAIM": "0"})
    if not d or not o:
        return None, None, None, None, None
    hd = hashlib.md5(open(dc, "rb").read()).hexdigest()
    ho = hashlib.md5(open(oc, "rb").read()).hexdigest()
    return hd == ho, d, o, dc, oc


def main():
    os.makedirs(TMP, exist_ok=True)
    update = "--update-baseline" in sys.argv
    failures = []
    print("== 红集：目标 = 判 idle（默认档与关档逐字节相同）==")
    for rel, _a, _b in RED:
        src = os.path.join(ROOT, rel)
        name = os.path.basename(rel)[:-5]
        idle, d, o, _, _ = classify(src, name)
        if idle is None:
            failures.append((rel, "build")); print(f"  FAIL {name}: 编译失败"); continue
        if not idle:
            failures.append((rel, "not_idle"))
            print(f"  FAIL {name}: 还没判 idle（仍发射回收运行期）")
        else:
            print(f"  ok   {name}: idle")
    print("== 护栏：必须仍 PAYS + 有界 + 语义一致 ==")
    for rel, small, large, bounded in GUARD:
        src = os.path.join(ROOT, rel)
        name = os.path.basename(rel)[:-5]
        idle, d, o, _, _ = classify(src, name)
        if idle is None:
            failures.append((rel, "build")); print(f"  FAIL {name}: 编译失败"); continue
        if idle:
            failures.append((rel, "guard_idle"))
            print(f"  FAIL {name}: 被判 idle —— 它真的丢引用，护栏被放行")
            continue
        so, _ = run(d, large); so2, _ = run(o, large)
        if so != so2:
            failures.append((rel, "sem_same")); print(f"  FAIL {name}: 默认档与关档输出不同")
        if bounded:
            r1, r2 = peak_rss(d, small), peak_rss(d, large)
            if r1 is None or r2 is None or r2 / max(r1, 1) >= RSS_LIMIT:
                failures.append((rel, "unbounded"))
                print(f"  FAIL {name}: 峰值 {r1}→{r2} KB（判据 <{RSS_LIMIT}×）")
            else:
                print(f"  ok   {name}: PAYS 且有界（{r2/max(r1,1):.2f}×）")
        else:
            print(f"  ok   {name}: PAYS（语义一致）")
    failures.sort()
    known = load()
    if update:
        with open(BASE, "w") as f:
            f.write("# gate-idle-projection 的已知欠账：`形状<TAB>判据`。红集转绿要删行；\n")
            f.write("# 护栏那侧的 guard_idle / unbounded / sem_same 是安全问题，不该出现在这里。\n")
            for rel, why in failures:
                f.write(f"{rel}\t{why}\n")
        print(f"已更新基线：{BASE}（{len(failures)} 条）")
        return 0
    new = [f for f in failures if f not in known]
    fixed = [f for f in known if f not in failures]
    for rel, why in new:  print(f"  NEW  {rel}  {why}")
    for rel, why in fixed: print(f"  转绿 {rel}  {why}（棘轮：请删掉这行）")
    if new or fixed:
        print(f"失败 {len(new)} 条新增 / {len(fixed)} 条该删")
        return 1
    print(f"通过（已知欠账 {len(known)} 条，棘轮只许缩）")
    return 0


def load():
    if not os.path.exists(BASE):
        return []
    out = []
    for line in open(BASE):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        p = line.split("\t")
        if len(p) >= 2:
            out.append((p[0], p[1]))
    return out


if __name__ == "__main__":
    sys.exit(main())
