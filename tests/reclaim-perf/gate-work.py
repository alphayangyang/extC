#!/usr/bin/env python3
"""闸门·工作量界：把运行期 `[work]` 的**确定性**每步工作量接成自动判据。

为什么值得有这一条：`[step]`/墙钟已被证明不可靠（同一产物 `step_smax` 能差 8×，且与
`involuntary csw` 无一致关系 ⇒ 外部干扰），**唯一确定的窗口判据就是工作量界**。
2026-10-15 那个洞（`markStep` 里种子字扫描**无界**：没有种子时把整条 slab 链每个 `rsbit`
字扫完）如果早有 `scan_max` 闸门，第一天就该红。

判据（全在下面 `main()` 里）：
1. **元判据**：`[work]` 行必须存在且 `n > 0`；缺失/为 0 ⇒ FAIL，文案写明「插桩失败/工具不兼容」，
   **不许当绿**（延续"插桩失败不许伪装成 sweeps=0"的规则）。
2. **确定性**：每形状跑 3 次，`[work]` 行必须**逐字相同**；不一致 ⇒ FAIL（工作量本应确定）。
3. **界（蕴含式，数字从**产物**里读，不硬编码）**：
   `walk_max ≤ EXTC_RC_MARK_BATCH_MAX`、`walk_gt<BATCH_MAX> == 0`（桶存在时）、
   `scan_max ≤ EXTC_RC_SEED_WORDS`、`push_max ≤ EXTC_RC_SEED_PUSHES + (字宽-1)`
   （压栈按**字**收口：外层在进字前检查，最后一个 64 位字最多多压 63）。
   三个 `#define` 与 `rsbit` 字宽**都从本树现场生成的 C 里 grep**；任何一个找不到 ⇒ FAIL
   （防"会漂的副本"进判据）。

用法：`python3 tests/reclaim-perf/gate-work.py`（在 `tools/check.py` 里登记为 `gate-reclaim-work`，
不挂 `bench` ⇒ 进 `nobench`）。环境变量：`EXTC`（默认 `./build/extc`）、`CC`、`WORK_REPS`。
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTC = os.environ.get("EXTC", os.path.join(ROOT, "build", "extc"))
CC = os.environ.get("CC", "cc")
REPS = int(os.environ.get("WORK_REPS", "3"))

# 形状取样：(标签, 源, N)。五个形状覆盖 分配密集(p5) / 跨区登记(cross) / 逐出(evict) /
# 分片(shard) / 环负载(digraph)。
SHAPES = [
    ("p5", "bench/gc/src/spectrum_p5.extc", "600000"),
    ("cross", "tests/reclaim-shapes/cross_region.extc", "800000"),
    ("shard", "tests/reclaim-shapes/shard_bands.extc", "800000"),
    ("evict", "tests/reclaim-perf/shapes/refgraph/rg_evict_cache.extc", "1000000"),
    ("digraph", "bench/gc/src/digraph.extc", "8000"),
]

pass_n = 0
fail_n = 0


def ok(msg):
    global pass_n
    pass_n += 1
    print(f"  ok   {msg}", flush=True)


def bad(msg):
    global fail_n
    fail_n += 1
    print(f"  FAIL {msg}", flush=True)


def grab_define(text, name):
    """从产物里读 `#define <name> <数字>`；读不到 ⇒ None（调用方必须红，不许兜底硬编码）。"""
    m = re.search(r"^#define\s+" + re.escape(name) + r"\s+\(?(\d+)\)?", text, re.M)
    return int(m.group(1)) if m else None


def grab_word_bits(text):
    """`rsbit` 是 uint64_t 位图 ⇒ 压栈的字粒度就是它的位宽（不硬编码 63）。"""
    m = re.search(r"\buint(\d+)_t\s+\*?rsbit\b", text)
    return int(m.group(1)) if m else None


def fields(work_line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z_]+)=(-?\d+)", work_line)}


def main():
    tmp = tempfile.mkdtemp(prefix="extc-work-")
    try:
        print("== 闸门·工作量界（[work] 确定性 + 蕴含式界，参数从产物读）==", flush=True)

        # ── 现场生成 + 编译五形状 ────────────────────────────────────────────
        bins = []
        gen_text = None
        for tag, src, n in SHAPES:
            cpath = os.path.join(tmp, tag + ".c")
            bpath = os.path.join(tmp, tag)
            env = dict(os.environ, EXTC_RECLAIM="1", EXTC_STD=os.path.join(ROOT, "stdlib"))
            g = subprocess.run([EXTC, "-w", "-I", os.path.join(ROOT, "stdlib"),
                                os.path.join(ROOT, src), "-o", cpath],
                               capture_output=True, text=True, cwd=ROOT, env=env, timeout=300)
            if g.returncode != 0:
                bad(f"生成失败（{tag}）：{g.stderr.strip().splitlines()[:1]}")
                return 1
            if gen_text is None:
                with open(cpath, encoding="utf-8", errors="replace") as f:
                    gen_text = f.read()
            c = subprocess.run([CC, "-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fwrapv",
                                "-o", bpath, cpath], capture_output=True, text=True, timeout=300)
            if c.returncode != 0:
                bad(f"cc 失败（{tag}）：{c.stderr.strip().splitlines()[:1]}")
                return 1
            bins.append((tag, bpath, n))

        # ── 判据的数字全部从产物里读（找不到任何一个 ⇒ 红，不许硬编码）────────
        p_batch = grab_define(gen_text, "EXTC_RC_MARK_BATCH_MAX")
        p_words = grab_define(gen_text, "EXTC_RC_SEED_WORDS")
        p_push = grab_define(gen_text, "EXTC_RC_SEED_PUSHES")
        word_bits = grab_word_bits(gen_text)
        missing = [n for n, v in (("EXTC_RC_MARK_BATCH_MAX", p_batch), ("EXTC_RC_SEED_WORDS", p_words),
                                  ("EXTC_RC_SEED_PUSHES", p_push), ("rsbit 字宽", word_bits)) if v is None]
        if missing:
            bad("闸门自身失效：产物里读不到 " + "、".join(missing) +
                " —— 判据的数字必须从产物 grep，不许硬编码，更不许兜底")
            return 1
        push_cap = p_push + (word_bits - 1)
        print(f"  参数（从产物读）：BATCH_MAX={p_batch} SEED_WORDS={p_words} SEED_PUSHES={p_push} "
              f"rsbit={word_bits}位 ⇒ push 允许 ≤ {push_cap}", flush=True)

        # ── 每形状：3 次；元判据 + 确定性 + 界 ───────────────────────────────
        for tag, bpath, n in bins:
            lines = []
            for _ in range(REPS):
                r = subprocess.run([bpath, n], capture_output=True, text=True,
                                   env=dict(os.environ, EXTC_RC_PAUSE="1"), timeout=1800)
                m = re.search(r"^\[work\] (.*)$", r.stderr, re.M)
                lines.append(m.group(1).strip() if m else None)
            if any(x is None for x in lines):
                bad(f"{tag}：拿不到 `[work]` 行 —— **插桩失败/工具不兼容**（不是「这一步没工作」）；"
                    f"先查 `src/back/reclaim.c` 的 `[work]` 输出与 `EXTC_RC_PAUSE` 门控")
                continue
            if len(set(lines)) != 1:
                bad(f"{tag}：{REPS} 次 `[work]` 行不一致（工作量本应确定）⇒ 探针/统计写错了：{lines}")
                continue
            f = fields(lines[0])
            if f.get("n", 0) <= 0:
                bad(f"{tag}：`[work] n=0` —— **插桩失败/工具不兼容**（没有计入任何有界热路径步）")
                continue
            d = (f"n={f['n']} walk_max={f.get('walk_max')} scan_max={f.get('scan_max')} "
                 f"push_max={f.get('push_max')}")
            viol = []
            if not (f.get("walk_max", -1) <= p_batch):
                viol.append(f"walk_max={f.get('walk_max')} > BATCH_MAX={p_batch}")
            bkey = f"walk_gt{p_batch}"
            if bkey in f and f[bkey] != 0:
                viol.append(f"{bkey}={f[bkey]} != 0")
            if not (f.get("scan_max", -1) <= p_words):
                viol.append(f"scan_max={f.get('scan_max')} > SEED_WORDS={p_words}")
            skey = f"scan_gt{p_words}"
            if skey in f and f[skey] != 0:
                viol.append(f"{skey}={f[skey]} != 0")
            if not (f.get("push_max", -1) <= push_cap):
                viol.append(f"push_max={f.get('push_max')} > SEED_PUSHES+{word_bits - 1}={push_cap}")
            if viol:
                bad(f"{tag}：工作量越界 —— " + "；".join(viol) + f"（{d}）")
            else:
                ok(f"{tag}：{d}（{REPS} 次逐字相同）")

        print(flush=True)
        print(f"通过 {pass_n}，失败 {fail_n}", flush=True)
        if fail_n:
            return 1
        print("  ok   工作量界：五形状 `[work]` 存在/确定/n>0，且 walk/scan/push 都在本树预算内", flush=True)
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
