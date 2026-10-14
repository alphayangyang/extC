#!/usr/bin/env python3
"""闸门·事件档的内存界：被证明无环的区域靠计数回收时，**垃圾不许随规模涨**。

## 它守什么

事件档（`EXTC_RC_SWEEPFREE=1`：计数开、清扫关）是原型 `ev` 的那条路：不可达对象靠
"最后一个持有者消失"当场释放，一次清扫都不做。它的前提是**计数完备**——如果某处丢弃
没有销账，对象就带着一个幽灵持有者永不归零，垃圾随规模线性涨。

这条闸门把"完备性"变成可判的：拿一组**活集与规模无关**的形状（覆盖式写入、容器淘汰、
滑动窗口），在事件档下跑 N 与 4N，峰值 RSS 必须 <1.5×。语义还要与 `EXTC_RECLAIM=0` 档
逐字相同（用关档当参照，不用默认档：默认档在尾插这类形状上自己就是平方，跑不完）。

另外两条结构性判据（不需要计时，判据不会被噪声带跑）：

* `sweepfree_on`：事件档产物里 `#define EXTC_RC_SWEEP_FREE` 必须是 1（开关没生效 = 空跑）；
* `no_release_site`：事件档产物里不许有 `extc_rc_storeRefOwnedRelease(` 调用点 ——
  定点释放不看计数（从其余根做全量标记），清扫恒不触发时它只有成本没有收益。

## 棘轮

今天不成立的那些 `形状|判据` 记在 `tools/gate-event-bounded-known-bad.txt`：**只许缩**。
新增失败 ⇒ 红（exit 1）；基线里有一条已经转绿 ⇒ 也红（要求把它删掉，避免基线腐烂）。

用法：
    python3 tests/reclaim-acyclic/gate-event-bounded.py            # 判据
    python3 tests/reclaim-acyclic/gate-event-bounded.py --update-baseline
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gatecommon as G  # noqa: E402

BASE = os.path.join(ROOT, "tools", "gate-event-bounded-known-bad.txt")
TMP = "/tmp/extc-event-bounded"
os.makedirs(TMP, exist_ok=True)

# (相对路径, 小规模, 大规模, 是否判"峰值有界")
# "峰值有界"只对**活集与规模无关**的形状成立：覆盖式写入、容器淘汰、滑动窗口。
# 链表/图这类活集本身就随 N 涨的形状只判结构性两条（否则判据会在测"活数据"而不是"垃圾"）。
CORPUS = [
    ("tests/reclaim-perf/shapes/refgraph/rg_overwrite_ring.extc", 200000, 800000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_evict_cache.extc", 200000, 800000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_array.extc", 200000, 800000, True),
    ("bench/gc/src/spectrum_p5.extc", 300000, 1200000, True),
    ("tests/reclaim-perf/shapes/refgraph/rg_grow_tail.extc", 10000, 40000, False),
    ("tests/reclaim-perf/shapes/refgraph/rg_scan_graph.extc", 10000, 40000, False),
    ("tests/reclaim-perf/shapes/refgraph/rg_relink_delete.extc", 100000, 400000, False),
]

CC_FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fwrapv", "-O2"]
RSS_LIMIT = 1.5


def gen(src, dst, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    return subprocess.run([G.EXTC, "-w", "-I", os.path.join(ROOT, "stdlib"), src, "-o", dst],
                          capture_output=True, text=True, env=env, timeout=120)


def build(src, tag, env_extra):
    c = os.path.join(TMP, f"{tag}.c")
    b = os.path.join(TMP, f"{tag}")
    if gen(src, c, env_extra).returncode != 0:
        return None, None
    if subprocess.run(["cc"] + CC_FLAGS + ["-o", b, c], capture_output=True).returncode != 0:
        return None, None
    return b, c


def run_capture(binary, n, limit=120):
    """返回 (stdout 文本, 是否超时)。"""
    try:
        p = subprocess.run([binary, str(n)], capture_output=True, text=True, timeout=limit)
        return p.stdout, False
    except subprocess.TimeoutExpired:
        return "", True


def peak_rss(binary, n, limit=120):
    """峰值 RSS（KB）：/usr/bin/time 只包住被测程序本身。

    **不能用 fork+wait4 写在 Python 里**：父进程 fork 出来的子进程在 exec 之前继承父进程的
    页表，`ru_maxrss` 会把父进程的峰值一并记进来 —— 父进程读过的生成物文本会让小规模的读数
    被抬到十几 MB，比值失真（实测 200k 读成 15.7 MB，真值 5.9 MB），泄漏就看不见了。
    """
    try:
        p = subprocess.run(["/usr/bin/time", "-f", "%M", binary, str(n)],
                           capture_output=True, text=True, timeout=limit)
    except subprocess.TimeoutExpired:
        return None
    for line in reversed(p.stderr.strip().splitlines()):
        line = line.strip()
        if line.isdigit():
            return int(line)
    return None


def main():
    update = "--update-baseline" in sys.argv
    failures = []
    print("== 事件档（计数开 + 清扫关）—— 释放点 / 语义 / 峰值界 ==")
    for rel, n_small, n_large, check_bounded in CORPUS:
        src = os.path.join(ROOT, rel)
        name = os.path.basename(rel)[:-5]
        ev_bin, ev_c = build(src, name + ".ev", {"EXTC_RC_SWEEPFREE": "1"})
        if not ev_bin:
            failures.append((rel, "event_build"))
            print(f"  FAIL {name}: 事件档编译失败")
            continue
        ctext = open(ev_c).read()
        if "#define EXTC_RC_SWEEP_FREE 1" not in ctext:
            failures.append((rel, "sweepfree_on"))
            print(f"  FAIL {name}: 事件档开关没生效（SweepFree != 1）")
        if "storeRefOwnedRelease((void" in ctext:
            failures.append((rel, "no_release_site"))
            print(f"  FAIL {name}: 事件档下仍有定点释放调用点")
        ref_bin, _ = build(src, name + ".off", {"EXTC_RECLAIM": "0"})
        if not ref_bin:
            failures.append((rel, "off_build"))
            print(f"  FAIL {name}: 关档编译失败")
            continue
        ev_out, ev_to = run_capture(ev_bin, n_large)
        ref_out, ref_to = run_capture(ref_bin, n_large)
        if ev_to or ref_to:
            failures.append((rel, "sem_same"))
            print(f"  FAIL {name}: 语义对照跑不完（事件={ev_to} 关档={ref_to}）")
        elif ev_out != ref_out:
            failures.append((rel, "sem_same"))
            print(f"  FAIL {name}: 事件档与关档输出不同")
        if check_bounded:
            r1 = peak_rss(ev_bin, n_small)
            r2 = peak_rss(ev_bin, n_large)
            if r1 is None or r2 is None:
                failures.append((rel, "bounded"))
                print(f"  FAIL {name}: 峰值量不到（超时）")
                continue
            ratio = r2 / max(r1, 1)
            mark = "ok  " if ratio < RSS_LIMIT else "FAIL"
            print(f"  {mark} {name}: 峰值 {r1}→{r2} KB（{ratio:.2f}×，判据 <{RSS_LIMIT}）")
            if ratio >= RSS_LIMIT:
                failures.append((rel, "bounded"))
        else:
            print(f"  ok   {name}: 结构性两条（释放点/语义）")
    failures.sort()
    print(f"失败 {len(failures)} 条（未入基线 {sum(1 for f in failures if f not in load_baseline())} 条）")

    known = load_baseline()
    if update:
        write_baseline(failures)
        print(f"已更新基线：{BASE}（{len(failures)} 条）")
        return 0
    new = [f for f in failures if f not in known]
    fixed = [f for f in known if f not in failures]
    if new:
        print("新增失败（判据红）：")
        for rel, why in new:
            print(f"  NEW  {rel}  {why}")
    if fixed:
        print("基线里已经转绿（棘轮：请删掉这些行）：")
        for rel, why in fixed:
            print(f"  DONE {rel}  {why}")
    if new or fixed:
        return 1
    print(f"通过（已知欠账 {len(known)} 条，棘轮只许缩）")
    return 0


def load_baseline():
    if not os.path.exists(BASE):
        return []
    out = []
    for line in open(BASE):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            out.append((parts[0], parts[1]))
    return out


def write_baseline(failures):
    with open(BASE, "w") as f:
        f.write("# gate-event-bounded 的已知欠账：`形状<TAB>判据`。只许缩短，"
                "转绿必须删行（见脚本头部）。\n")
        f.write("# 判据：sweepfree_on / no_release_site / sem_same / bounded / event_build / default_build\n")
        for rel, why in failures:
            f.write(f"{rel}\t{why}\n")


if __name__ == "__main__":
    sys.exit(main())
