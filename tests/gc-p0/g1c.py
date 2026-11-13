#!/usr/bin/env python3
"""tests/gc-p0/g1c.py -- G1② 的**通道拆分**量具（Lead m04215 / task-29 口径）。

为什么需要它：v4 的 [histo] 只给**全体** step 的 15 桶直方图，把三类性质完全不同的
工作混在一个分母里：
  (a) 常规步 seg=0      —— mark 的普通推进 + sweep 的 word 步（热路径，应 <=5us 级）
  (b) 大对象续扫 seg>0  —— mark 的 resume 段，一段 <= SCAN_CHUNK 字（结构性慢，单独给界）
  (c) regionFree / collect / 配额记账 —— 每次 drop/collect 一次，1ms 级（G3b）
把 (a)(b) 混在一起，p99.9 与 >50us 占比既不能证明常规路径便宜，也不能证明大对象有界。

数据契约（Lead m04242 定版；runtime-facts 在 task-26 实现）：
    [segsteps] n=<seg>0 的 step 数> pops_tot=<这些步的 pops 之和> max_us=<这些步最大 cpu us>
               b1us=... bge64ms=...        （15 桶与 [histo] 同界、cpu 口径、全 0 也打印）
    [histo]    仍是全体 step 的 15 桶；[top] 提供 rank/kind/why/dur_us/cpu_us/freed/words/seg/pops/.../chunk
        (a) 计数 = [histo] - [segsteps]（逐桶相减，负值 = 插桩不一致，直接 ERROR）
        (b) 计数 = [segsteps]
        n_seg <= n_histo 必须成立
    缺 [segsteps] 行 => (a)(b) 记 N/A(no-segsteps)，**不是 PASS**；(c) 仍可从 [chan] 判。
        (a) 口径注（AUDIT-P1 C9）：[segsteps] 只统计 `markResume` 真正推进了字的段步；`scanObj` 非空但
        本步 resume 返回 0 的 stale-cursor 步既不在 [segsteps]、也不单列，它们被算进 (a) 的分母
        => (a) 的 over-50 占比对这类步是**偏保守**（分母偏大），方向安全，不要据此上调判据。
    pops_tot 校验：|pops_tot - n_seg| > 0 => **WARN**（不是 ERROR；Lead m04242 预期
    seg>0 步结构上 pops=1：markStep 在 scanObj 时 resume 后 budget=0，pop 循环不跑）。

判据（Lead m04215 + m04242 + m04448，全部 cpu 口径）：
  (a-warm) 常规步 seg=0，warm 形状（cyc/pc/pb 等）：>50us 占比 <=1e-4 且 p99.9 <=10us；
           n<1e5 => N/A（不是 PASS）。max 不作判据（只作 OBS）。
           为什么 warm 的 5us 与 dense 的 B_seg 是两条契约：warm 形状的步是热路径上的
           普通推进/word 步，其成本由指令与 L1/L2 命中决定（实测 p99.9 落在 <=16us 桶内），
           "单步 <=5us 级" 是产品级停顿承诺；dense 形状的慢步是**冷对象图上的 pop/续扫**，
           其成本由对象规模与每 word/每对象成本决定（结构性 300-500us），只能按实测
           每 word 成本给闭式界，不能套 5us。
  (a-cold) cold-dense 形状（默认 pd_pointer_dense）：**不参与** 5us/10us 门禁（E29：冷 pop
           实测 1.3-1.9us/obj，b=256 时结构性 300-500us）。判 tau_t = 步时/pops <= 2us
           （取 [top] 里 **pops>=2** 的最慢步，cpu_us/pops；pops=1 的 seg>0 步是续扫段，
           归 (b)，见 Lead m04448）、p99.9 <= 1ms、over1ms=0；
           >50us 占比只作 OBS，并在报告里归因到 [top]。
           另加 OBS：pops>=1（含 pops=1）的每 pop 成本单独打印、**不判**。
  (b) 续扫通道 seg>0（Lead m04591 + m04755 定版）：
       **判据 = p99.9 <= min(B_seg(shape), 64us) 且 over1ms=0；max 只记 OBS**
       - B_seg(shape) = 1.5 * c_seg(shape) * chunk + 1us，chunk 锁 8192（P1-RQ3-CHUNK.md:3）；
       - 64us 是**桶界兜底**（m04755）：50us 落在 b64us 桶内部、15 桶直方图无法证明到 50us，
         故兜底取该桶上界 64us；要精确到 50us 需精确分位插桩（后续，不改 [histo] 桶界）；
       - max 尾部与 chunk 无关、机制未定位 ⇒ task-32 的 [bd] 子相位归因完成前不当判据
         （这是**有意的判据收缩**，不是放宽）；over1ms=0 恒判（直接量，任何 n 可判）。
       - 样本量：MIN_N=1e5 不降（m04755）⇒ p1 after 里 pd_bigobj / pd_pointer_dense 各跑 5 reps
         （每 rep 约 2.5-3.3 万段步，合并 12.7-16.3 万过线）。warm 形状 n=0 ⇒ 该通道 N/A。
       c_seg(shape) = [segsteps] p99.9_us / chunk，取自 tests/gc-p0/bseg-cal.tsv
       （列 shape/chunk/c_seg_ns/p99.9_us/p99.9_min_us/p99.9_max_us/run_id/src_md5/extc_md5；
        c_seg 用 3 reps 中位、spread 记 min/max；溯源以源码 md5 为准）。
       --c-seg/--c-scan-ns/--seg-max-us 只是手工覆盖，报告须标注非门禁口径。
       未标定 => p99.9 记 N/A「上界未标定」，**不得**退回 5us/10us 旧占位。
       OBS：[top] 里 pops=1 & seg>0 的冷整段步单独打印（次数/最长），不进 (b) 判据。
       参考标定值（pct 探针，chunk=8192/dl800）：pd_bigobj 34.08us（第二 rep 47.53）⇒ 4.160 ns/word；
       pd_pointer_dense 56.40us（第二 rep 65.05）⇒ 6.885 ns/word ⇒ 正式标定必须 3 reps 中位。
  (c) 通道：max <=1000us（G3b），目标 <=200us（观察，不判）。
  v17 精确计数：`[histo]`/`[segsteps]` 行尾若带 `gt10us=`/`gt50us=`/`gt64us=`（严格 >10/50/64 µs 的
  精确步数），则 (a) 的 >50us 占比与 (a)(b) 的 p99.9 改用**精确计数**判定，不再受桶界跨桶 N/A 限制：
      over50: PASS iff gt50/n <= 1e-4；RED iff gt50/n - p_noise > 1e-4；其余记噪声带 N/A；
      p99.9: 能证明 count(>L) <= 0.001n 则 PASS、count(>L) > 0.001n 则 RED（L 恰为 10/50/64 us 时精确；
             其它 L 由 {>t} 的包含关系给上下界；无上界可给 => 退回桶界逻辑）。
  旧产物无这三个字段 => 全部按桶界逻辑判（行为与 v16 一致）。
  噪声：判据须「扣噪声后仍成立」=> 下界比值先减 p_noise、上界比值加 p_noise 再比；
        噪声取 3 reps 的 cpu_over50_ratio / cpu_p999_us 的最大值（noise.txt）。
  桶界（HISTO_EDGES_NS，pause_parse 硬编码）：50us 落在 b64us 桶（32-64us）内部，
  1ms 恰好落在 b1ms 桶（<=1ms => 不违 G3b）——两者都是**跨桶**，只能给界；见 --selftest。

用法：
  python3 tests/gc-p0/g1c.py --selftest
  python3 tests/gc-p0/g1c.py --dir OUT/log --noise OUT/noise.txt [--json OUT/g1c.json]
  python3 tests/gc-p0/g1c.py OUT/log/cyc_pause_r1.err [...] --noise OUT/noise.txt
  G1C_JSON=OUT/g1c.json G1C_LOG='OUT/log/cyc_pause_r*.err' ...
"""
import argparse, glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import pause_parse as pp

MIN_N = 100000              # Lead m04215：步数 <1e5 的分位判据记 N/A（不是 PASS）
OVER50_US = 50.0
P999_REG_US = 10.0          # (a-warm) 常规通道
P999_COLD_US = 1000.0       # (a-cold) cold-dense 形状的 p99.9（Lead m04242）
PER_POP_US = 2.0            # (a-cold) tau_t = 步时/pops 上限（Lead m04242；E29 实测 1.3-1.9us/obj）
CHAN_MAX_US = 1000.0        # (c) G3b
CHAN_TARGET_US = 200.0      # (c) 目标（观察）
RATIO_LIMIT = 1e-4          # (a) >50us 占比
# (b) 续扫闭式上界（Lead m04242 + m04448）：B_seg(shape) = 1.5 * c_seg(shape) * chunk + 1us。
# c_seg 必须来自**该形状自己**的实测（不得跨形状借），由 --c-seg NAME=LO[:HI] 传入；未标定
# => (b) 的 p99.9/max 记 N/A（不得退回旧 5us/10us 占位）。
SEG_MARGIN = 1.5
SEG_ADD_US = 1.0
C_SCAN_NS_PER_WORD = None   # 旧的全局值（仅当未给 --c-seg 时兜底，报告会标注来源）
# 参考（v6 日志 chunk=8192 反推；Lead m04448）：不要当默认值用，正式跑必须按形状传实测。
C_SEG_HINT = {"pd_pointer_dense": (34.0, 52.0), "pd_bigobj": (9.5, 12.0)}
COLD_DENSE_DEFAULT = ("pd_pointer_dense",)   # Lead m04242：按 shape 归类，不用 5us 门禁判它
# 混合通道形状（Lead m03564）：每轮 64 个 16 B leaf + 1 个 8 MiB big，插桩不按 n 归因 ⇒ (a) 的
# <=5us/占比 判据结构性不可判，只能记 N/A 并列出可判定界；大分配走 (b)、通道走 (c)。
MIXED_DEFAULT = ("pd_bigobj", "pd_bigobj60", "pd")
# Lead m04591（(b) 的最终口径，取代 m04448 的待定态）：
#  - chunk 锁 8192（内存优先的最终决定，docs/analysis/P1-RQ3-CHUNK.md:3/:19-22）；
#  - c_seg(shape) = [segsteps] p99.9_us / chunk，来自 tests/gc-p0/bseg-cal.tsv（每个 build 标定一次）；
#    不用 [top] 的 278-424us 粗段（那是 pops=1 冷对象整段，含冷 pop 成本，不是纯扫描成本）。
#  - (b) 判据 = p99.9 <= min(1.5*c_seg*chunk+1us, 64us 桶界兜底) 且 over1ms=0；
#    max 只 OBS（尾部与 chunk 无关、机制未定位，task-32 归因后再议）。
#  - 兜底为什么是 64us（Lead m04755 裁决）：p99.9 是 [segsteps] 15 桶直方图读出的界，50us 落在
#    b64us 桶（edge=64000ns）**内部** ⇒ 直方图无法证明到 50us，只能证明到 64us；真实值在 (50,64] 会被判 PASS。
#    不改 [histo]/[segsteps] 桶界（跨轮可比性优先；真正的第一道界是 B_seg 闭式，兜底只在闭式被绕过时防灾难）；
#    要精确到 50us 需精确分位插桩（列为后续）。
BSEG_CHUNK = 8192
BSEG_CAL_DEFAULT = os.path.join(HERE, "bseg-cal.tsv")
P999_SEG_CAP_US = 64.0
P999_SEG_ABS_US = P999_SEG_CAP_US   # 兼容旧名（selftest/json 里出现过）


# ---------------------------------------------------------------------------
# 解析
# ---------------------------------------------------------------------------
def parse_segsteps(txt):
    """解析 [segsteps] 行（契约见文件头）。缺行 => None（旧产物/未插桩）。"""
    m = re.search(r"^\[segsteps\][^\n]*", txt, re.M)
    if not m:
        return None
    line = m.group(0)
    nm = re.search(r"\bn=(\d+)", line)
    mm = re.search(r"max_us=([\d.]+)", line)
    pm = re.search(r"\bpops_tot=(\d+)", line)
    vals = {k: int(v) for k, v in re.findall(r"\b([A-Za-z][A-Za-z0-9]*)=(\d+)", line)}
    return {"line": line.strip(), "n": int(nm.group(1)) if nm else 0,
            "pops_tot": int(pm.group(1)) if pm else None,
            "max_us": float(mm.group(1)) if mm else None,
            "counts": [vals.get(x, 0) for x in pp.HISTO_NAMES],
            "gt10": vals.get("gt10us"), "gt50": vals.get("gt50us"), "gt64": vals.get("gt64us")}


TOP_KEYS_INT = ("rank", "freed", "words", "seg", "pops", "coll", "step", "first", "trans",
                "minflt", "thr", "mmax", "scale", "pressure", "chunk")


def parse_top(txt):
    """解析 [top] rank=... kind=... why=... dur_us=... cpu_us=... pops=... seg=... chunk=...
    （Emitter: src/back/reclaim.c:3445）。返回 list[dict]；缺行 => []。"""
    out = []
    for ln in txt.splitlines():
        if not ln.startswith("[top]"):
            continue
        kv = dict(re.findall(r"([A-Za-z_]+)=([^\s]+)", ln))
        rec = {"kind": kv.get("kind"), "why": kv.get("why")}
        for k in ("dur_us", "cpu_us"):
            try:
                rec[k] = float(kv[k]) if k in kv else None
            except ValueError:
                rec[k] = None
        for k in TOP_KEYS_INT:
            try:
                rec[k] = int(kv[k]) if k in kv else None
            except ValueError:
                rec[k] = None
        out.append(rec)
    return out


def _merge(dst, src):
    if dst is None:
        dst = {"n": src["n"], "counts": list(src["counts"]), "max_us": src.get("max_us"),
               "pops_tot": src.get("pops_tot"),
               "gt10": src.get("gt10"), "gt50": src.get("gt50"), "gt64": src.get("gt64")}
        return dst
    dst["n"] += src["n"]
    for i in range(pp.HISTO_NB):
        dst["counts"][i] += src["counts"][i]
    if src.get("max_us") is not None:
        dst["max_us"] = max(dst["max_us"] or 0.0, src["max_us"])
    if src.get("pops_tot") is not None:
        dst["pops_tot"] = (dst.get("pops_tot") or 0) + src["pops_tot"]
    for k in ("gt10", "gt50", "gt64"):
        a, b = dst.get(k), src.get(k)
        dst[k] = None if (a is None or b is None) else a + b
    return dst


def load_errs(paths):
    """读取多个 .err，聚合 [histo]/[segsteps]/[chan]/[top]。
    返回 (hist, seg, chan, tops, files)；tops 按 cpu_us 降序保留前 8 条（跨 rep）。"""
    hist = seg = None
    chan = {}
    tops = []
    for p in paths:
        with open(p, "r", errors="replace") as f:
            txt = f.read()
        rec = pp.parse_text(txt)
        h = rec.get("histo")
        if h and h.get("n") is not None:
            hist = _merge(hist, {"n": h["n"], "counts": h["counts"],
                                 "max_us": rec.get("step_max_us"),
                                 "gt10": h.get("gt10"), "gt50": h.get("gt50"), "gt64": h.get("gt64")})
        s = parse_segsteps(txt)
        if s:
            seg = _merge(seg, s)
        for c in rec.get("chan") or []:
            name = c.get("name") or "unknown"
            d = chan.setdefault(name, {"n": 0, "total_us": 0.0, "max_us": 0.0,
                                       "work_max": 0.0, "counts": [0] * pp.HISTO_NB})
            d["n"] += int(c.get("n") or 0)
            d["total_us"] += float(c.get("total_us") or 0.0)
            d["max_us"] = max(d["max_us"], float(c.get("max_us") or 0.0))
            d["work_max"] = max(d["work_max"], float(c.get("work_max") or 0.0))
            for i, nm in enumerate(pp.HISTO_NAMES):
                d["counts"][i] += int(c.get(nm) or 0)
        tops += parse_top(txt)
    tops.sort(key=lambda t: (t.get("cpu_us") is not None, t.get("cpu_us") or 0.0), reverse=True)
    return hist, seg, chan, tops[:8], len(paths)


def load_noise(paths):
    """解析 noise_floor.c 输出行（cpu 口径）。返回 p_noise/p999/明细/reps。"""
    rows = []
    for p in paths:
        try:
            lines = open(p, "r", errors="replace").read().splitlines()
        except OSError:
            continue
        for ln in lines:
            if not ln.startswith("noise "):
                continue
            def num(key):
                m = re.search(key + r"=([0-9.eE+-]+)", ln)
                return float(m.group(1)) if m else None
            rows.append({"cpu_over50_ratio": num("cpu_over50_ratio"),
                         "cpu_p999_us": num("cpu_p999_us"),
                         "cpu_max_us": num("cpu_max_us"),
                         "wall_over50_ratio": num("wall_over50_ratio")})
    rows = [r for r in rows if r["cpu_over50_ratio"] is not None]
    if not rows:
        return {"reps": 0, "p_noise": 0.0, "p999_noise_us": 0.0, "rows": []}
    return {"reps": len(rows),
            "p_noise": max(r["cpu_over50_ratio"] for r in rows),
            "p999_noise_us": max(r["cpu_p999_us"] or 0.0 for r in rows),
            "rows": rows}


# ---------------------------------------------------------------------------
# 桶界工具
# ---------------------------------------------------------------------------
def bucket_of_ns(ns):
    """返回 ns 落在的桶下标（桶 i 表示 d <= edges[i]；末桶 bge64ms 为 >64ms）。"""
    for i, e in enumerate(pp.HISTO_EDGES_NS):
        if ns <= e:
            return i
    return pp.HISTO_NB - 1


def cum(counts, i):
    return sum(counts[:i + 1])


def sum_from(counts, i):
    return sum(counts[i:])


def over_thr_bounds(counts, thr_us):
    """严格 >thr 的计数**下界/上界**（桶粒度）。
    下界 = 整个桶都 >thr 的桶和（桶下界 >thr）；上界 = 桶上界 >thr 的桶和。"""
    thr_ns = thr_us * 1000.0
    lo_from = pp.HISTO_NB - 1
    for i in range(pp.HISTO_NB):
        lower = 0 if i == 0 else pp.HISTO_EDGES_NS[i - 1]
        if lower >= thr_ns:
            lo_from = i
            break
    hi_from = pp.HISTO_NB - 1
    for i in range(pp.HISTO_NB):
        upper = pp.HISTO_EDGES_NS[i] if i < len(pp.HISTO_EDGES_NS) else None
        if upper is None or upper > thr_ns:
            hi_from = i
            break
    return sum_from(counts, lo_from), sum_from(counts, hi_from)


# ---------------------------------------------------------------------------
# 精确超阈计数（v17）
# ---------------------------------------------------------------------------
GT_THRESHOLDS_US = (10, 50, 64)


def gt_of(rec):
    """取 [histo] / [segsteps] 的精确超阈计数 {10:..,50:..,64:..}。
    三个字段缺一 => None（旧产物 => 调用方退回桶界逻辑，行为与 v16 一致）。"""
    if not rec:
        return None
    g = {}
    for t, k in ((10, "gt10"), (50, "gt50"), (64, "gt64")):
        v = rec.get(k)
        if v is None:
            return None
        g[t] = int(v)
    return g


def gt_sub(a, b):
    """(a) 通道的精确计数 = [histo] - [segsteps]（与逐桶相减同口径）。
    任一缺 => None；出现负值 => "ERROR"（插桩不一致，调用方不得当 0 用）。"""
    if a is None or b is None:
        return None
    d = {t: a[t] - b[t] for t in GT_THRESHOLDS_US}
    return "ERROR" if any(v < 0 for v in d.values()) else d


def exact_over_bounds(gt, limit_us):
    """严格 >limit_us 的步数**精确上下界**（靠 {>t} 的包含关系）。
    上界：{>L} 是 {>t}（t<=L 中最大者）的子集；无 t<=L => None（没有上界）。
    下界：{>t}（t>L 中最小者）是 {>L} 的子集；无 t>L => 0。
    L 恰等于某个 t 时上下界相等 = 精确计数。"""
    for t in GT_THRESHOLDS_US:
        if limit_us == t:                          # L 恰为阈值 => gt[t] 就是精确值
            return gt[t], gt[t]
    hi = None
    for t in GT_THRESHOLDS_US:
        if t <= limit_us:
            hi = gt[t]
    lo = 0
    for t in GT_THRESHOLDS_US:
        if t > limit_us:
            lo = gt[t]
            break
    return lo, hi


def p999_exact(gt, n, limit_us):
    """精确计数下的 p99.9<=limit 判定（整数比较，避开浮点边界）。
    PASS iff count(>L)*1000 <= n（上界可证）；RED iff count(>L)*1000 > n（下界可证）；否则 N/A。"""
    if not n:
        return "N/A", "n=0"
    lo, hi = exact_over_bounds(gt, limit_us)
    if hi is not None and hi * 1000 <= n:
        return "PASS", "count(>%gus) <= %d；%d*1000 <= n=%d（精确计数）" % (limit_us, hi, hi, n)
    if lo * 1000 > n:
        return "RED", "count(>%gus) >= %d；%d*1000 > n=%d（精确计数）" % (limit_us, lo, lo, n)
    return "N/A", "count(>%gus) 只能给到 [%d, %s]，相对 0.001*n 跨线" % (
        limit_us, lo, "无上界" if hi is None else str(hi))


def p999_bounds(counts, n, limit_us):
    """p99.9 <= limit 的**可判性**：返回 (可判结果, 说明)。
    可判 PASS：累计到 limit 所在桶的**前一桶**已 >= 0.999（p99.9 <= 前桶上界 < limit）；
    可判 RED ：累计到 limit 所在桶仍 < 0.999（p99.9 > 该桶上界 > limit）；
    否则跨桶 N/A。"""
    idx = bucket_of_ns(limit_us * 1000.0)
    lo_cum = cum(counts, idx - 1) if idx > 0 else 0
    hi_cum = cum(counts, idx)
    lo_edge = (pp.HISTO_EDGES_NS[idx - 1] / 1000.0) if idx > 0 else 0.0
    hi_edge = (pp.HISTO_EDGES_NS[idx] / 1000.0) if idx < len(pp.HISTO_EDGES_NS) else None
    hi_s = ">64ms" if hi_edge is None else "%.0fus" % hi_edge
    if n and lo_cum / n >= 0.999:
        return "PASS", "p99.9 <= %s <= %gus（桶界可判）" % (_fmt_us(lo_edge), limit_us)
    if n and hi_cum / n < 0.999:
        return "RED", "p99.9 落在 %s 之上 > %gus（桶界可判）" % (hi_s, limit_us)
    return "N/A", "p99.9 落在 %s..%s 的跨桶区间，不能判 %gus" % (_fmt_us(lo_edge), hi_s, limit_us)


def _fmt_us(v):
    return ("%.0f" % v) if v is not None and v >= 1 else ("%.3f" % (v or 0.0))


# ---------------------------------------------------------------------------
# 判定
# ---------------------------------------------------------------------------
def seg_bound_us(chunk, c_lo, c_hi=None):
    """(b) 闭式上界 B_seg(shape) = 1.5 * c_seg(shape) * chunk + 1us（Lead m04448）。
    c_seg 是该形状自己的实测区间（ns/word）；未标定（缺 chunk 或 c_seg）=> None（不是 PASS）。
    返回 {"lo":us, "hi":us, "note":...}：c_seg 为区间时 lo/hi 分别给出（lo 用于 PASS、hi 用于 RED）。"""
    if c_lo is None:
        return None, "该形状无 c_seg 实测（不得跨形状借；等 runtime-facts 报 chunk 后标定）"
    if not chunk:
        return None, "缺 [top] chunk= 字段（无法算 B_seg）"
    hi = c_lo if c_hi is None else c_hi
    lo_us = SEG_MARGIN * min(c_lo, hi) * chunk / 1000.0 + SEG_ADD_US
    hi_us = SEG_MARGIN * max(c_lo, hi) * chunk / 1000.0 + SEG_ADD_US
    note = "B_seg = 1.5 * c_seg[%.2f..%.2f ns/word] * %d words + 1us" % (min(c_lo, hi), max(c_lo, hi), chunk)
    return {"lo": lo_us, "hi": hi_us, "note": note}, note


def load_bseg_cal(path):
    """读 tests/gc-p0/bseg-cal.tsv（Lead m04591+m04755 的可审计标定表）。
    列（TAB 分隔；# 注释）：
      shape / chunk / c_seg_ns / p99.9_us / p99.9_min_us / p99.9_max_us / run_id / src_md5 / extc_md5
    c_seg_ns = 3 reps 的 p99.9_us 中位 ÷ chunk × 1000（ns/word）；spread 记 min/max；
    src_md5 = reclaim.c 的 md5 + codegen.c 的 md5（**溯源以此为准**）；extc_md5 = 编译器二进制 md5。
    兼容旧 6 列（shape/chunk/c_seg_ns/p99.9_us/run_id/build_md5；min=max=p99.9、src_md5 空）。
    文件缺失/无行 => {}（=> (b) 记 N/A，不猜、不退回旧占位）。"""
    rows = {}
    if not path or not os.path.exists(path):
        return rows
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = [x.strip() for x in ln.split("\t")]
            if parts[0].lower() == "shape":
                continue
            if len(parts) < 4:
                continue
            try:
                chunk = int(float(parts[1]))
                c_seg = float(parts[2])
                p999 = float(parts[3])
            except ValueError:
                continue
            if len(parts) >= 9:
                lo = float(parts[4]) if parts[4] else p999
                hi = float(parts[5]) if parts[5] else p999
                run_id, src_md5, extc_md5 = parts[6], parts[7], parts[8]
            else:                                   # 兼容旧 6 列
                lo = hi = p999
                run_id = parts[4] if len(parts) > 4 else ""
                src_md5, extc_md5 = "", (parts[5] if len(parts) > 5 else "")
            rows[parts[0]] = {
                "chunk": chunk, "c_seg_ns": c_seg, "p99.9_us": p999,
                "p99.9_min_us": lo, "p99.9_max_us": hi,
                "run_id": run_id, "src_md5": src_md5,
                "extc_md5": extc_md5, "build_md5": extc_md5,   # 旧键兼容
            }
    return rows


def resolve_bseg_cal(cal, case):
    """按用例名子串**最长匹配**选标定行（同 --c-seg）；返回 (key, row) 或 (None, None)。"""
    best = None
    for k in cal:
        if k.lower() in (case or "").lower():
            if best is None or len(k) > len(best):
                best = k
    if best is None:
        return None, None
    return best, cal[best]


def seg_bound_from_cal(key, row):
    """由标定行算 B_seg = 1.5*c_seg*chunk + 1us（Lead m04591；chunk 固定 BSEG_CHUNK）。
    标定行里的 p99.9_us 只作审计凭证；c_seg_ns 与 p99.9/chunk 的偏差 >20% 时 WARN（不篡改）。"""
    chunk = row.get("chunk") or BSEG_CHUNK
    c_seg = row.get("c_seg_ns")
    if not c_seg or not chunk:
        return None, "标定行缺 c_seg/chunk"
    us = SEG_MARGIN * c_seg * chunk / 1000.0 + SEG_ADD_US
    warn = ""
    p999 = row.get("p99.9_us")
    if p999:
        derived = p999 * 1000.0 / chunk
        if derived > 0 and abs(derived - c_seg) / derived > 0.20:
            warn = (" WARN: c_seg_ns=%.3f 与 p99.9_us/chunk=%.3f ns/word 偏差 >20%%"
                    % (c_seg, derived))
    spread = ""
    if row.get("p99.9_min_us") is not None and row.get("p99.9_max_us") is not None:
        spread = "p99.9=%.2f..%.2fus" % (row["p99.9_min_us"], row["p99.9_max_us"])
    else:
        spread = "p99.9=%.2fus" % (p999 or 0.0)
    note = ("B_seg(shape) = 1.5 * c_seg[%.3f ns/word] * %d words + 1us = %.2fus（标定：%s / %s / "
            "chunk=%d / %s / src_md5=%s / extc_md5=%s）%s" % (
                c_seg, chunk, us, key, row.get("run_id") or "-", chunk, spread,
                (row.get("src_md5") or "-"), (row.get("extc_md5") or "-"), warn))
    return {"lo": us, "hi": us, "note": note, "cal": {"shape": key, **row}}, note


def _bnd_pair(b):
    """把 seg_bound（float 或 {'lo','hi'}）归一成 (lo, hi)。"""
    if b is None:
        return None, None
    if isinstance(b, dict):
        return b.get("lo"), b.get("hi")
    return float(b), float(b)


def _bnd_str(b):
    """把 seg_bound（float/{'lo','hi'}/None）格式化成 'lo..hius' 或 'N/A'。"""
    if isinstance(b, dict):
        return "%.1f..%.1fus" % (b["lo"], b["hi"])
    if b is None:
        return "N/A"
    return "%.1fus" % b


def _bound_cell(value_us, b_lo, b_hi):
    """把单步时长对闭式区间判 PASS/RED/N/A：<=lo => PASS、>hi => RED、中间 => N/A。"""
    if value_us is None:
        return "N/A", "无该量"
    if value_us <= b_lo:
        return "PASS", "%.1fus <= B_seg_lo=%.1fus" % (value_us, b_lo)
    if value_us > b_hi:
        return "RED", "%.1fus > B_seg_hi=%.1fus（per-word 成本超该形状实测区间，不调参掩盖）" % (
            value_us, b_hi)
    return "N/A", "%.1fus 落在 B_seg 区间 %.1f..%.1fus 内，跨区间不可判" % (value_us, b_lo, b_hi)


def verdict_step_channel(name, n, counts, max_us, p_noise, mode="warm",
                         p999_limit_us=None, over_limit_us=OVER50_US,
                         ratio_limit=RATIO_LIMIT, min_n=MIN_N, gt=None):
    """(a-warm)/(b) 的通道判定。返回 dict（json 友好）。
    warm：>50us 占比（扣噪声）+ p99.9<=10us；max 只 OBS，不进判定。
    seg ：p99.9 与 max 用闭式上界 B_seg（未标定 => N/A）；over1ms=0 恒判；>50us 只 OBS。"""
    out = {"name": name, "n": n, "max_us": max_us, "mode": mode,
           "over_limit_us": over_limit_us, "ratio_limit": ratio_limit,
           "p999_limit_us": p999_limit_us, "criteria": {}}
    if not n:
        out["verdict"] = "N/A"
        out["note"] = "n=0（该通道没有样本）"
        return out
    exact = gt is not None
    out["exact_over"] = exact
    if exact:
        lo = hi = int(gt[50])          # >50us 的步数就是 gt[50]（v17 精确计数）
        r_lo = r_hi = lo / n
    else:
        lo, hi = over_thr_bounds(counts, over_limit_us)
        r_lo, r_hi = lo / n, hi / n
    out["over_lo_count"], out["over_hi_count"] = lo, hi
    out["over_lo_ratio"], out["over_hi_ratio"] = r_lo, r_hi
    out["p_noise"] = p_noise
    lo_net = max(0.0, r_lo - p_noise)
    hi_net = r_hi - p_noise
    out["ratio_lo_net"] = lo_net
    big = n >= min_n
    obs50 = (("精确：%d/%d = %.2e（gt50us）" % (lo, n, r_lo)) if exact else
             "观测：%.2e / %.2e（%s 不判该项）" % (r_lo, r_hi, "warm" if mode == "warm" else "seg"))
    if mode == "warm":
        if not big:
            out["criteria"]["over50"] = {"verdict": "N/A",
                "why": "n=%d < 1e5 => 分位不可判（不是 PASS；观测 %.2e/%.2e）" % (n, r_lo, r_hi)}
            out["criteria"]["p999"] = {"verdict": "N/A", "why": "n<1e5"}
            out["note"] = "n<1e5：分位判据 N/A（不是 PASS）"
        else:
            if exact and lo_net > ratio_limit:
                out["criteria"]["over50"] = {"verdict": "RED",
                    "why": "精确 gt50=%d（%.2e）扣噪声 %.2e > %.0e" % (lo, r_lo, lo_net, ratio_limit)}
            elif exact and r_lo <= ratio_limit:
                out["criteria"]["over50"] = {"verdict": "PASS",
                    "why": "精确 gt50=%d（%.2e）<= %.0e" % (lo, r_lo, ratio_limit)}
            elif exact:
                out["criteria"]["over50"] = {"verdict": "N/A",
                    "why": "噪声带：精确 %.2e 落在 (%.0e, ...]，cpu 噪声 %.2e（不是 PASS）"
                           % (r_lo, ratio_limit, p_noise)}
            elif lo_net > ratio_limit:
                out["criteria"]["over50"] = {"verdict": "RED",
                    "why": "扣噪声后下界 %.2e > %.0e（上界 %.2e）" % (lo_net, ratio_limit, r_hi)}
            elif hi_net <= ratio_limit:
                out["criteria"]["over50"] = {"verdict": "PASS",
                    "why": "上界 %.2e - 噪声 %.2e <= %.0e" % (r_hi, p_noise, ratio_limit)}
            else:
                out["criteria"]["over50"] = {"verdict": "N/A",
                    "why": "跨桶：下界 %.2e <= %.0e < 上界 %.2e（%gus 落在桶内）"
                           % (r_lo, ratio_limit, r_hi, over_limit_us)}
            if exact:
                pv, pw = p999_exact(gt, n, p999_limit_us)
            else:
                pv, pw = p999_bounds(counts, n, p999_limit_us)
            out["criteria"]["p999"] = {"verdict": pv, "why": pw}
        out["criteria"]["max"] = {"verdict": "OBS",
            "why": "max=%s us（warm 通道 max 不作判据）" % (_fmt_us(max_us) if max_us is not None else "n/a")}
    else:  # seg —— Lead m04591+m04755：p99.9 <= min(1.5*c_seg*chunk+1us, 64us 桶界兜底) 且 over1ms=0；max 只 OBS
        out["criteria"]["over50_info"] = {"verdict": "OBS", "why": obs50}
        b_lo, b_hi = _bnd_pair(p999_limit_us)
        if b_lo is None:
            out["criteria"]["p999"] = {"verdict": "N/A",
                "why": "B_seg(shape) 未标定（bseg-cal.tsv 缺该形状；1.5*c_seg(shape)*chunk+1us；"
                       "不退回 5us 旧占位）"}
        elif not big:
            out["criteria"]["p999"] = {"verdict": "N/A", "why": "n<1e5"}
        else:
            if exact:
                v_lo, w_lo = p999_exact(gt, n, b_lo)
                v_hi, w_hi = p999_exact(gt, n, b_hi)
                v_abs, w_abs = p999_exact(gt, n, P999_SEG_ABS_US)
            else:
                v_lo, w_lo = p999_bounds(counts, n, b_lo)
                v_hi, w_hi = p999_bounds(counts, n, b_hi)
                v_abs, w_abs = p999_bounds(counts, n, P999_SEG_ABS_US)
            reds = []
            if v_hi == "RED":
                reds.append("p99.9 > B_seg_hi=%.1fus（%s）" % (b_hi, w_hi))
            if v_abs == "RED":
                reds.append("p99.9 > 桶界兜底 %.0fus（Lead m04755：50us 落在 b64us 桶内，兜底精度取桶上界；%s）"
                            % (P999_SEG_CAP_US, w_abs))
            if reds:
                out["criteria"]["p999"] = {"verdict": "RED", "why": "；".join(reds)}
            elif v_lo == "PASS" and v_abs == "PASS":
                out["criteria"]["p999"] = {"verdict": "PASS",
                    "why": "p99.9 <= B_seg_lo=%.1fus 且 <= %.0fus 桶界兜底（桶界可判）"
                           % (b_lo, P999_SEG_CAP_US)}
            else:
                cross = []
                if v_lo != "PASS":
                    cross.append(w_lo)
                if v_abs != "PASS":
                    cross.append(w_abs)
                out["criteria"]["p999"] = {"verdict": "N/A",
                    "why": "跨桶不可判（B_seg=%.1f..%.1fus、桶界兜底 %.0fus）：%s"
                           % (b_lo, b_hi, P999_SEG_CAP_US, "；".join(cross))}
        out["criteria"]["max"] = {"verdict": "OBS",
            "why": "max=%s us（Lead m04591：max 只 OBS 不作判据；尾部与 chunk 无关、机制未定位，"
                   "task-32 归因后再议）" % (_fmt_us(max_us) if max_us is not None else "n/a")}
    over1 = sum_from(counts, pp.OVER1MS_FROM)
    out["over1ms"] = over1
    out["criteria"]["over1ms"] = {"verdict": "PASS" if over1 == 0 else "RED",
        "why": "over1ms=%d（严格 >1ms 桶和）" % over1}
    vs = [c["verdict"] for c in out["criteria"].values() if c["verdict"] != "OBS"]
    if "RED" in vs:
        out["verdict"] = "RED"
    elif "N/A" in vs:
        out["verdict"] = "N/A"
    else:
        out["verdict"] = "PASS"
    if out["verdict"] == "N/A":
        n_na = [k for k, c in out["criteria"].items() if c["verdict"] == "N/A"]
        out["note"] = (out.get("note", "") + " N/A（非 PASS）：%s 不可判" % "/".join(n_na)).strip()
    return out


def verdict_mixed(n, counts, max_us, tops):
    """混合通道形状（pd_bigobj / pd_bigobj60 / G1 的 pd）的 (a) 行（Lead m03564）。

    这些形状每轮既有 64 个 16 B 常规分配、又有 1 个 8 MiB 大分配，而 [histo]/[segsteps] 不按 n
    归因 ⇒ 「常规通道新增回收切片 <=5us / >50us 占比」在它们身上**不可判**，只能记 N/A 并列出
    可判定界（>4us / >8us / >1ms 步数）。大分配通道走 (b)（B_seg 闭式 + over1ms=0），
    通道耗时走 (c)。N/A ≠ PASS（Lead m03746④）。
    """
    out = {"name": "regular(seg=0) mixed", "n": n, "max_us": max_us, "mode": "mixed",
           "criteria": {}}
    if not n:
        out["verdict"] = "N/A"
        out["note"] = "n=0（该通道没有样本）"
        return out
    gt4 = sum_from(counts, 3)                      # > 4us（b8us 起）
    gt8 = sum_from(counts, 4)                      # > 8us（b16us 起）
    over1 = sum_from(counts, pp.OVER1MS_FROM)      # 严格 > 1ms
    lo, hi = over_thr_bounds(counts, OVER50_US)
    out["over_lo_ratio"], out["over_hi_ratio"] = lo / n, hi / n
    out["criteria"]["over50_info"] = {"verdict": "OBS",
        "why": "观测：%.2e / %.2e（混合形状不判；>4us=%d >8us=%d >1ms=%d，n=%d）"
               % (lo / n, hi / n, gt4, gt8, over1, n)}
    out["criteria"]["slice_le5"] = {"verdict": "N/A",
        "why": "混合形状（每轮 64 个 16 B leaf + 1 个 8 MiB big；插桩不计 n）⇒ 切片 <=5us 不可判"
               "（Lead m03564）；N/A ≠ PASS"}
    out["verdict"] = "N/A"
    out["note"] = "混合通道：>4us=%d >8us=%d（n=%d）；(a) 按 m03564 记 N/A，大分配看 (b)" % (
        gt4, gt8, n)
    return out


def verdict_cold_dense(n, counts, max_us, tops, p_noise):
    """cold-dense 形状（默认 pd_pointer_dense）的 (a) 行（Lead m04242）：
    **不参与** 5us/10us 门禁（E29：冷 pop 1.3-1.9us/obj，b=256 时结构性 300-500us）。
    判 tau_t = 步时/pops <= 2us（[top] 里 **pops>=2** 的最慢步，cpu_us/pops；pops=1 的 seg>0
    步是续扫段，归 (b)，Lead m04448）、p99.9 <= 1ms、over1ms=0；
    >50us 占比只作 OBS，并把慢尾归因到 [top]（pops/seg/words）。
    另加 OBS：pops>=1（含 pops=1）的每 pop 成本，单独打印不判（Lead m04448）。"""
    out = {"name": "regular(seg=0) cold-dense", "n": n, "max_us": max_us, "mode": "cold",
           "p999_limit_us": P999_COLD_US, "per_pop_limit_us": PER_POP_US, "criteria": {}}
    if not n:
        out["verdict"] = "N/A"
        out["note"] = "n=0（该通道没有样本）"
        return out
    lo, hi = over_thr_bounds(counts, OVER50_US)
    out["over_lo_ratio"], out["over_hi_ratio"] = lo / n, hi / n
    out["criteria"]["over50_info"] = {"verdict": "OBS",
        "why": "观测：%.2e / %.2e（cold-dense 不判 5us/50us 门禁）" % (lo / n, hi / n)}
    # tau_t 只对 **pop-bound 步** 有意义（pops>=2）：seg>0 的续扫步归 (b) 通道，
    # 它的时长由 chunk 决定、与 pops 无关（本机 p0-after 实测 pd_pointer_dense 的
    # [top] 前 3 名都是 pops=1 seg=8192 dur 278-424us，把它算进 tau_t 会把 (b) 的量
    # 误记成"冷 pop 税"）。pops=1 且 seg=0 的单对象步时长 = 一次冷 pop，仍不构成 tau_t。
    cand = [t for t in tops if (t.get("pops") or 0) >= 2 and t.get("cpu_us") is not None]
    taus = []
    for t in cand[:3]:
        taus.append({"kind": t.get("kind"), "why": t.get("why"), "cpu_us": t["cpu_us"],
                     "pops": t["pops"], "seg": t.get("seg"), "words": t.get("words"),
                     "chunk": t.get("chunk"), "tau_us": t["cpu_us"] / t["pops"]})
    out["top_attrib"] = taus
    if not cand:
        out["criteria"]["tau_t"] = {"verdict": "N/A",
            "why": "无 [top] pops>=2 样本（需要 EXTC_RC_PAUSE=1；慢步可能全是 seg>0 续扫 => 归 (b)）"}
        out["per_pop_us"] = None
    else:
        worst = max(t["cpu_us"] / t["pops"] for t in cand)
        out["per_pop_us"] = worst
        out["criteria"]["tau_t"] = {
            "verdict": "PASS" if worst <= PER_POP_US else "RED",
            "why": "tau_t=max(步时/pops)=%.3fus %s %.1fus（pops>0 的 [top] 最慢步）"
                   % (worst, "<=" if worst <= PER_POP_US else ">", PER_POP_US)}
    # OBS（Lead m04448）：pops>=1（含 pops=1）的每 pop 成本，单独打印、不判。
    all_cand = [t for t in tops if (t.get("pops") or 0) >= 1 and t.get("cpu_us") is not None]
    if all_cand:
        worst_all = max(all_cand, key=lambda t: t["cpu_us"] / t["pops"])
        out["per_pop_obs_all"] = {
            "kind": worst_all.get("kind"), "why": worst_all.get("why"),
            "cpu_us": worst_all["cpu_us"], "pops": worst_all["pops"],
            "seg": worst_all.get("seg"), "words": worst_all.get("words"),
            "chunk": worst_all.get("chunk"),
            "tau_us": worst_all["cpu_us"] / worst_all["pops"],
            "is_seg_resume": (worst_all.get("pops") == 1 and (worst_all.get("seg") or 0) > 0),
        }
    if n < MIN_N:
        out["criteria"]["p999"] = {"verdict": "N/A", "why": "n<1e5"}
        out["note"] = "n<1e5：p99.9 记 N/A（不是 PASS）"
    else:
        pv, pw = p999_bounds(counts, n, P999_COLD_US)
        out["criteria"]["p999"] = {"verdict": pv, "why": pw}
    over1 = sum_from(counts, pp.OVER1MS_FROM)
    out["over1ms"] = over1
    out["criteria"]["over1ms"] = {"verdict": "PASS" if over1 == 0 else "RED",
        "why": "over1ms=%d（严格 >1ms 桶和）" % over1}
    out["criteria"]["max"] = {"verdict": "OBS",
        "why": "max=%s us（cold-dense max 只 OBS）" % (_fmt_us(max_us) if max_us is not None else "n/a")}
    vs = [c["verdict"] for c in out["criteria"].values() if c["verdict"] != "OBS"]
    if "RED" in vs:
        out["verdict"] = "RED"
    elif "N/A" in vs:
        out["verdict"] = "N/A"
    else:
        out["verdict"] = "PASS"
    return out


def verdict_chan(name, d):
    """(c) 单个 [chan] 通道：G3b max <=1ms（目标 <=200us 观察）。"""
    n, mx = d["n"], d["max_us"]
    over1 = sum_from(d["counts"], pp.OVER1MS_FROM)
    out = {"name": name, "n": n, "max_us": mx, "over1ms": over1,
           "target_us": CHAN_TARGET_US, "criteria": {}}
    if not n:
        out["verdict"] = "N/A"
        out["note"] = "n=0（该通道未触发）"
        return out
    out["criteria"]["g3b"] = {"verdict": "PASS" if mx <= CHAN_MAX_US else "RED",
        "why": "max=%.1fus %s %.0fus" % (mx, "<=" if mx <= CHAN_MAX_US else ">", CHAN_MAX_US)}
    out["criteria"]["over1ms"] = {"verdict": "PASS" if over1 == 0 else "RED",
        "why": "over1ms=%d" % over1}
    out["target_met"] = (mx <= CHAN_TARGET_US)
    vs = [c["verdict"] for c in out["criteria"].values()]
    out["verdict"] = "RED" if "RED" in vs else "PASS"
    return out


def analyze(hist, seg, chan, noise, case=None, cold_dense=COLD_DENSE_DEFAULT,
            seg_bound=None, seg_bound_note=None, tops=None, mixed_shapes=MIXED_DEFAULT):
    """把聚合后的结构变成判定；返回 dict（写 json + 打印都用它）。
    case：用例名（决定 warm / cold-dense / mixed 归类，Lead m04242 + m03564）；
    seg_bound：B_seg 闭式上界（us），None => (b) 的 p99.9/max 记 N/A（未标定，不退回旧占位）。"""
    tops = tops or []
    if _is_cold_dense(case, cold_dense):
        cls = "cold-dense"
    elif _is_mixed(case, mixed_shapes):
        cls = "mixed"
    else:
        cls = "warm"
    res = {"min_n": MIN_N, "ratio_limit": RATIO_LIMIT, "case": case, "class": cls,
           "noise": noise, "channels": {}, "verdict": None, "warnings": [],
           "seg_bound_us": seg_bound, "seg_bound_note": seg_bound_note,
           "per_pop_limit_us": PER_POP_US, "p999_cold_us": P999_COLD_US}
    p_noise = noise.get("p_noise", 0.0)
    if hist is None:
        res["a"] = {"name": "regular(seg=0)", "verdict": "N/A",
                    "note": "没有 [histo] 行（旧插桩或未开 EXTC_RC_PAUSE=1）"}
        res["b"] = {"name": "resume(seg>0)", "verdict": "N/A", "note": "没有 [histo] 行"}
        res["verdict"] = "N/A"
    else:
        n_all, c_all = hist["n"], hist["counts"]
        g_all = gt_of(hist)
        if seg is None:
            res["a"] = {"name": "regular(seg=0)", "verdict": "N/A",
                        "note": "缺 [segsteps] 行 => (a)(b) 不可分（不是 PASS）"}
            res["b"] = {"name": "resume(seg>0)", "verdict": "N/A",
                        "note": "缺 [segsteps] 行 => 不可分（不是 PASS）"}
        else:
            n_seg = seg["n"]
            popst = seg.get("pops_tot")
            if popst is None:
                res["warnings"].append("缺 pops_tot 字段（旧插桩）；无法校验「seg>0 步 pops=1」")
            elif abs(popst - n_seg) > 0:
                res["warnings"].append(
                    "pops_tot 校验 WARN：pops_tot=%d != n_seg=%d（差 %+d；Lead m04242 预期 seg>0 步结构上 pops=1）"
                    % (popst, n_seg, popst - n_seg))
            res["pops_tot"] = popst
            if n_seg > n_all:
                res["a"] = {"name": "regular(seg=0)", "verdict": "ERROR",
                            "note": "n_seg=%d > n_histo=%d：插桩不一致" % (n_seg, n_all)}
                res["b"] = dict(res["a"])
            else:
                diff = [c_all[i] - seg["counts"][i] for i in range(pp.HISTO_NB)]
                if any(x < 0 for x in diff):
                    res["a"] = {"name": "regular(seg=0)", "verdict": "ERROR",
                                "note": "逐桶相减出现负值：插桩不一致（histo 未包含 segsteps 子集）"}
                    res["b"] = dict(res["a"])
                else:
                    n_reg = n_all - n_seg
                    g_seg = gt_of(seg)
                    g_reg = gt_sub(g_all, g_seg)
                    if g_reg == "ERROR":
                        res["warnings"].append(
                            "精确计数负值：[histo] 的 gt* 小于 [segsteps] 的 gt*（插桩不一致）")
                        g_reg = None
                    if cls == "cold-dense":
                        res["a"] = verdict_cold_dense(n_reg, diff, hist.get("max_us"), tops, p_noise)
                    elif cls == "mixed":
                        # Lead m03564：混合形状的 (a) 不可判（插桩不按 n 归因）⇒ N/A + 可判定界
                        res["a"] = verdict_mixed(n_reg, diff, hist.get("max_us"), tops)
                    else:
                        res["a"] = verdict_step_channel(
                            "regular(seg=0)", n_reg, diff, None, p_noise,
                            mode="warm", p999_limit_us=P999_REG_US, gt=g_reg)
                    res["b"] = verdict_step_channel(
                        "resume(seg>0)", n_seg, seg["counts"], seg.get("max_us"), p_noise,
                        mode="seg", p999_limit_us=seg_bound, gt=g_seg)
                    for ch in ("a", "b"):
                        cr = res.get(ch, {}).get("criteria", {}).get("p999")
                        if cr is not None and res.get(ch, {}).get("exact_over"):
                            cr["why"] = "精确计数口径；" + cr.get("why", "")
                    res["segsteps_line"] = seg.get("line")
        res["n_total"] = n_all
        res["histo_counts"] = c_all
        if seg is not None and res["b"].get("verdict") != "ERROR":
            res["n_seg"] = seg["n"]
        # OBS（Lead m04591）：[top] 里 pops=1 & seg>0 的冷整段步（含冷 pop + 一段扫描），
        # 单独记录、**不**参与 (b) 判据。注意 [top] 只保留前 K 个最慢步 => 采样，不是全量。
        coarse = [t for t in tops if (t.get("pops") == 1 and (t.get("seg") or 0) > 0)]
        if coarse:
            worst = max(coarse, key=lambda t: t.get("cpu_us") or 0.0)
            res["coarse_seg_obs"] = {
                "n_sampled": len(coarse), "max_cpu_us": worst.get("cpu_us"),
                "seg": worst.get("seg"), "words": worst.get("words"),
                "kind": worst.get("kind"), "why": worst.get("why"),
                "note": "OBS（[top] 采样，非判据）：pops=1 & seg>0 的冷整段步，不参与 (b) 判据（Lead m04591）"}
    # trigger-step = v17 起的名字（v16 及更早叫 collect；两个都收，旧产物仍可判）
    for name in ("regionfree", "release", "trigger-step", "collect", "account"):
        if name in chan:
            res["channels"][name] = verdict_chan(name, chan[name])
    vs = [res.get("a", {}).get("verdict"), res.get("b", {}).get("verdict")]
    vs += [c["verdict"] for c in res["channels"].values()]
    if "ERROR" in vs:
        res["verdict"] = "ERROR"
    elif "RED" in vs:
        res["verdict"] = "RED"
    elif "N/A" in vs or not res["channels"]:
        res["verdict"] = "N/A"
    else:
        res["verdict"] = "PASS"
    return res


def _is_cold_dense(case, patterns):
    """cold-dense 归类（Lead m04242：按 shape 归类，不用 5us 门禁判它）。"""
    cl = (case or "").lower()
    return any(str(p).lower() in cl for p in (patterns or ()))


def _is_mixed(case, patterns):
    """混合通道归类（Lead m03564）：pd_bigobj / pd_bigobj60 / G1 的 pd。

    注意顺序：analyze 先判 cold-dense（pd_pointer_dense）再判 mixed，否则 "pd" 会把 dense
    也吞进 mixed（前者要判 tau_t，后者只判 (b)/(c)）。
    """
    cl = (case or "").lower()
    return any(str(p).lower() in cl for p in (patterns or ()))


# ---------------------------------------------------------------------------
# 打印
# ---------------------------------------------------------------------------
def _row(label, d):
    if d.get("verdict") in ("N/A", "ERROR") and "over_lo_ratio" not in d:
        return "  | %s | - | - | - | - | **%s**（%s） |" % (
            label, d.get("verdict"), d.get("note", ""))
    over = ("%.2e / %.2e" % (d["over_lo_ratio"], d["over_hi_ratio"])) if "over_lo_ratio" in d else "-"
    p999 = d.get("criteria", {}).get("p999", {})
    cell = p999.get("verdict", "-")
    if p999.get("why"):
        cell += "（%s）" % p999["why"]
    mx = ("%.1f" % d["max_us"]) if d.get("max_us") is not None else "-"
    return "  | %s | %d | %s | %s | %s | **%s** |" % (
        label, d.get("n") or 0, over, cell, mx, d.get("verdict"))


def print_report(res, label=None):
    nz = res["noise"]
    cls = res.get("class") or "warm"
    print("== G1② 通道拆分%s（cpu 口径；判据 Lead m04215+m04242；class=%s；min_n=%d）=="
          % ((" · " + label) if label else "", cls, res["min_n"]))
    print("  噪声地板：%d reps，cpu over50 最大 %.2e，cpu p99.9 最大 %.3fus"
          % (nz.get("reps", 0), nz.get("p_noise", 0.0), nz.get("p999_noise_us", 0.0))
          + ("" if nz.get("reps", 0) >= 3 else "  ⚠ 不足 3 reps"))
    print("  判据：warm 判 >50us 占比 <=%.0e（扣噪声）+ p99.9<=%.0fus；"
          "cold-dense 判 tau_t<=%.1fus + p99.9<=%.0fus + over1ms=0（不判 5us 门禁）；"
          "mixed 形状 (a) 记 N/A（Lead m03564：插桩不按 n 归因，N/A != PASS）"
          % (res["ratio_limit"], P999_REG_US, PER_POP_US, P999_COLD_US))
    print("  (b) 判据（Lead m04591+m04755）：p99.9 <= min(1.5*c_seg(shape)*chunk+1us, %.0fus 桶界兜底) 且 "
          "over1ms=0；max 只 OBS（尾部与 chunk 无关、机制未定位，task-32 归因后再议）"
          % P999_SEG_CAP_US)
    print("  (b) 续扫上界：%s" % (res.get("seg_bound_note") or "未标定 => p99.9/max 记 N/A"))
    b = res.get("seg_bound_us")
    if isinstance(b, dict):
        print("       B_seg = %.2f .. %.2f us（lo 判 PASS / hi 判 RED，中间记 N/A）" % (b["lo"], b["hi"]))
    elif b is not None:
        print("       B_seg = %.2f us" % b)
    print("  | 通道 | n | >50us 下界/上界 | p99.9 | max_us | 判定 |")
    print("  |---|---|---|---|---|---|")
    print(_row("(a) 常规 seg=0", res.get("a", {})))
    print(_row("(b) 续扫 seg>0", res.get("b", {})))
    for name, d in res["channels"].items():
        if not d["n"]:
            print("  | (c) %s | 0 | - | - | - | **N/A**（该通道未触发） |" % name)
        else:
            print("  | (c) %s | %d | - | - | %.1f | **%s**（目标 <=%.0fus %s） |"
                  % (name, d["n"], d["max_us"], d["verdict"], CHAN_TARGET_US,
                     "达成" if d.get("target_met") else "未达成"))
    if cls == "mixed" and (res.get("a") or {}).get("mode") != "mixed":
        print("  mixed 口径（Lead m03564）：本形状属混合通道（每轮 64 个 16 B leaf + 1 个 8 MiB big），"
              "(a) 结构性不可判 => 恒 N/A（不是 PASS）；上面 (a) 行显示 N/A 的原因另见括号")
    if cls == "mixed" and (res.get("a") or {}).get("mode") == "mixed":
        a = res.get("a", {})
        print("  mixed 口径（Lead m03564）：每轮 64 个 16 B leaf + 1 个 8 MiB big，插桩不按 n 归因"
              " ⇒ (a) 的 <=5us / >50us 占比结构性不可判，记 N/A（不是 PASS）；")
        print("    大分配通道看 (b)（B_seg 闭式 + over1ms=0）；>50us 占比只 OBS = %.2e / %.2e；%s"
              % (a.get("over_lo_ratio") or 0.0, a.get("over_hi_ratio") or 0.0, a.get("note") or ""))
    if cls == "cold-dense" and (res.get("a") or {}).get("mode") == "cold":
        a = res.get("a", {})
        print("  cold-dense 口径（Lead m04242）：不参与 5us/10us 门禁（E29 冷 pop 1.3-1.9us/obj，"
              "b=256 时结构性 300-500us）；判 tau_t=步时/pops<=%.1fus、p99.9<=%.0fus、over1ms=0。"
              % (PER_POP_US, P999_COLD_US))
        print("    >50us 占比（OBS）= %.2e / %.2e；tau_t=%s us；[top] 归因（pops>0 的最慢步）："
              % (a.get("over_lo_ratio") or 0.0, a.get("over_hi_ratio") or 0.0,
                 ("%.3f" % a["per_pop_us"]) if a.get("per_pop_us") is not None else "n/a"))
        for t in (a.get("top_attrib") or [])[:3]:
            print("      kind=%s why=%s cpu_us=%.1f pops=%s seg=%s words=%s chunk=%s => tau=%.3fus/pop"
                  % (t.get("kind"), t.get("why"), t.get("cpu_us") or 0.0, t.get("pops"),
                     t.get("seg"), t.get("words"), t.get("chunk"), t.get("tau_us") or 0.0))
        ob = a.get("per_pop_obs_all")
        if ob:
            print("      OBS（不判，Lead m04448）pops>=1 最慢步：kind=%s why=%s cpu_us=%.1f pops=%s "
                  "seg=%s => %.3fus/pop%s"
                  % (ob.get("kind"), ob.get("why"), ob.get("cpu_us") or 0.0, ob.get("pops"),
                     ob.get("seg"), ob.get("tau_us") or 0.0,
                     "  ⚠ 该步是 pops=1 & seg>0 的续扫段（归 (b)，不是冷 pop 税）"
                     if ob.get("is_seg_resume") else ""))
    if res.get("n_seg") is not None:
        print("  [segsteps] n_seg=%d pops_tot=%s（校验 |pops_tot-n_seg|=0 期望）"
              % (res["n_seg"], res.get("pops_tot")))
    cs = res.get("coarse_seg_obs")
    if cs:
        print("  OBS（不判）[top] 冷整段（pops=1 & seg>0）采样 %d 条，最长 %.1fus（seg=%s words=%s kind=%s why=%s）"
              "——[top] 只采样前 K 慢步，不是全量；按 m04591 不进 (b) 判据"
              % (cs["n_sampled"], cs.get("max_cpu_us") or 0.0, cs.get("seg"), cs.get("words"),
                 cs.get("kind"), cs.get("why")))
    for w in res.get("warnings") or []:
        print("  ⚠ %s" % w)
    print("  ⇒ G1② 通道拆分：**%s**" % res["verdict"])
    if res["verdict"] == "N/A":
        print("     注意：N/A ≠ PASS（缺 [segsteps]/[chan]/未标定上界/样本不足时不得记绿）")
    for key in ("a", "b"):
        d = res.get(key, {})
        for k, c in (d.get("criteria") or {}).items():
            if c.get("verdict") not in ("PASS", "OBS"):
                print("     %s.%s: %s —— %s" % (key, k, c["verdict"], c.get("why", "")))


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------
def _hist_text(n, counts, tag="histo", max_us=None):
    head = "[%s] n=%d" % (tag, n)
    if max_us is not None:
        head += " max_us=%.1f" % max_us
    return head + " " + " ".join("%s=%d" % (nm, c) for nm, c in zip(pp.HISTO_NAMES, counts))


def _chan_text(name, n, counts, max_us):
    return ("[chan] name=%s n=%d max_us=%.1f total_us=%.1f work_max=0 work_sum=0 " % (name, n, max_us, max_us * n)
            + " ".join("%s=%d" % (nm, c) for nm, c in zip(pp.HISTO_NAMES, counts)))


def _zero():
    return [0] * pp.HISTO_NB


def _selftest():
    bad = []
    noise = {"reps": 3, "p_noise": 0.0, "p999_noise_us": 0.3,
             "rows": [{"cpu_over50_ratio": 0.0, "cpu_p999_us": 0.3}]}
    zero = _zero()

    def ck(cond, msg):
        if not cond:
            bad.append(msg)

    # S1: seg 混合 —— (a) = histo - seg 逐桶；pops_tot == n_seg（无 WARN）
    ch = _zero(); ch[0] = 190000; ch[3] = 5000; ch[4] = 3000; ch[5] = 1500; ch[7] = 400; ch[8] = 100
    cs = _zero(); cs[0] = 19900; cs[4] = 100
    hist = {"n": sum(ch), "counts": ch, "max_us": 300.0}
    seg = {"n": sum(cs), "counts": cs, "max_us": 9.0, "pops_tot": sum(cs)}
    r = analyze(hist, seg, {"collect": {"n": 10, "max_us": 120.0, "total_us": 300.0,
                                        "work_max": 1.0, "counts": zero}}, noise,
                seg_bound=20.0)
    ck(r["a"]["n"] == 180000 and r["b"]["n"] == 20000,
       "S1 分母错: a.n=%r b.n=%r（期望 180000/20000）" % (r["a"].get("n"), r["b"].get("n")))
    ck(r["a"].get("over_lo_count") == sum(ch[7:]), "S1 (a) 下界计数错: %r" % (r["a"].get("over_lo_count"),))
    ck(not r["warnings"], "S1 pops_tot==n_seg 时不应有 WARN: %r" % (r["warnings"],))
    ck(r["class"] == "warm", "S1 默认 class 应为 warm")

    # S2: 恰好 50us => 跨桶 N/A（不能判 PASS，也不能判 RED）
    ch = _zero(); ch[0] = 199899; ch[6] = 101
    ck(bucket_of_ns(50000) == 6, "S2 50us 未落 b64us(idx6): %d" % bucket_of_ns(50000))
    r = analyze({"n": sum(ch), "counts": ch, "max_us": 64.0},
                {"n": 0, "counts": zero, "max_us": 0.0}, {}, noise, seg_bound=20.0)
    v = r["a"]["criteria"]["over50"]["verdict"]
    ck(v == "N/A", "S2 恰好 50us 边界应为 N/A（跨桶），实得 %s" % v)
    ck(v != "PASS", "S2 跨桶被误判 PASS")

    # S3: 恰好 1ms 边界 => 落 b1ms(idx10) => G3b PASS、over1ms=0
    cc = _zero(); cc[10] = 1
    ck(bucket_of_ns(1000000) == 10, "S3 1ms 未落 b1ms(idx10): %d" % bucket_of_ns(1000000))
    r = analyze(None, None, {"collect": {"n": 1, "max_us": 1000.0, "total_us": 1000.0,
                                         "work_max": 1.0, "counts": cc}}, noise)
    ck(r["channels"]["collect"]["max_us"] <= CHAN_MAX_US, "S3 恰好 1ms 应满足 <=1ms")
    ck(r["channels"]["collect"]["criteria"]["g3b"]["verdict"] == "PASS", "S3 恰好 1ms G3b 应为 PASS")
    ck(r["channels"]["collect"]["criteria"]["over1ms"]["verdict"] == "PASS", "S3 恰好 1ms 不应计入 over1ms")

    # S4: n<1e5 => N/A（不是 PASS）
    ch = _zero(); ch[0] = 50000
    r = analyze({"n": 50000, "counts": ch, "max_us": 8.0},
                {"n": 0, "counts": zero, "max_us": 0.0}, {}, noise, seg_bound=20.0)
    ck(r["a"]["verdict"] == "N/A" and "1e5" in r["a"].get("note", ""),
       "S4 n<1e5 应 N/A 并说明，实得 %r" % (r["a"].get("verdict"),))

    # S5: 扣噪声后仍成立 —— 不扣 RED，扣了 PASS
    ch = _zero(); ch[0] = 199970; ch[7] = 30      # 1.5e-4
    h = {"n": sum(ch), "counts": ch, "max_us": 200.0}
    r0 = analyze(h, {"n": 0, "counts": zero, "max_us": 0.0}, {}, {"reps": 3, "p_noise": 0.0, "p999_noise_us": 0.3, "rows": []}, seg_bound=20.0)
    r1 = analyze(h, {"n": 0, "counts": zero, "max_us": 0.0}, {}, {"reps": 3, "p_noise": 5e-5, "p999_noise_us": 0.3, "rows": []}, seg_bound=20.0)
    ck(r0["a"]["criteria"]["over50"]["verdict"] == "RED",
       "S5 无噪声时应 RED，实得 %r" % (r0["a"]["criteria"]["over50"]["verdict"],))
    ck(r1["a"]["criteria"]["over50"]["verdict"] == "PASS",
       "S5 扣 5e-5 噪声后应 PASS，实得 %r" % (r1["a"]["criteria"]["over50"]["verdict"],))

    # S6: seg 计数 > histo 计数 => ERROR（不静默）
    ch = _zero(); ch[0] = 100
    seg = _zero(); seg[0] = 200
    r = analyze({"n": 100, "counts": ch, "max_us": 1.0},
                {"n": 200, "counts": seg, "max_us": 1.0, "pops_tot": 200}, {}, noise, seg_bound=20.0)
    ck(r["a"]["verdict"] == "ERROR", "S6 插桩不一致应 ERROR，实得 %r" % (r["a"]["verdict"],))

    # S7（Lead m04591）：(b) 判 p99.9 <= B_seg；max 只 OBS。B_seg=20us
    cs = _zero(); cs[0] = 299999; cs[4] = 1          # p99.9 落在 <=16us 桶 => 可判 PASS
    ch7 = _zero(); ch7[0] = 1999999; ch7[4] = 1      # histo = seg + 常规，保证差分非负
    mku = lambda mx: {"n": 300000, "counts": cs, "max_us": mx, "pops_tot": 300000}
    r = analyze({"n": 2000000, "counts": ch7, "max_us": 1.0}, mku(12.0), {}, noise, seg_bound=20.0)
    ck(r["b"]["criteria"]["p999"]["verdict"] == "PASS",
       "S7 p99.9<=B_seg=20us 应 PASS，实得 %r" % (r["b"]["criteria"]["p999"],))
    ck(r["b"]["criteria"]["max"]["verdict"] == "OBS",
       "S7 max 应只记 OBS（m04591），实得 %r" % (r["b"]["criteria"]["max"],))
    cs_red = _zero(); cs_red[0] = 299600; cs_red[6] = 400   # >32us 占 0.13% > 1e-3 => RED
    ch7r = _zero(); ch7r[0] = 1999600; ch7r[6] = 400        # histo >= seg（逐桶差分非负）
    r = analyze({"n": 2000000, "counts": ch7r, "max_us": 1.0},
                {"n": 300000, "counts": cs_red, "max_us": 25.0, "pops_tot": 300000},
                {}, noise, seg_bound=20.0)
    ck(r["b"]["criteria"]["p999"]["verdict"] == "RED",
       "S7 p99.9>B_seg 应 RED，实得 %r" % (r["b"]["criteria"]["p999"],))
    ck(r["b"]["criteria"]["over1ms"]["verdict"] == "PASS", "S7 over1ms=0 应 PASS")

    # S8: 未标定 B_seg => (b) p99.9 记 N/A（**不得**退回 5us/10us 旧占位）；max 恒 OBS
    r = analyze({"n": 2000000, "counts": ch7, "max_us": 1.0}, mku(4.0), {}, noise)
    ck(r["b"]["criteria"]["max"]["verdict"] == "OBS",
       "S8 max 恒 OBS（m04591），实得 %r" % (r["b"]["criteria"]["max"],))
    ck(r["b"]["verdict"] == "N/A", "S8 未标定 B_seg 时 (b) 整体应 N/A，实得 %r" % (r["b"]["verdict"],))
    ck("B_seg" in r["b"]["criteria"]["p999"].get("why", ""), "S8 未标定原因应提到 B_seg")

    # S9: p99.9 跨桶 N/A 与可判 RED
    ch = _zero(); ch[0] = 199000; ch[4] = 1000          # 全在 <=16us，8-16 跨桶
    r = analyze({"n": 200000, "counts": ch, "max_us": 16.0},
                {"n": 0, "counts": zero, "max_us": 0.0}, {}, noise, seg_bound=20.0)
    ck(r["a"]["criteria"]["p999"]["verdict"] == "N/A",
       "S9 p99.9 跨桶应 N/A，实得 %r" % (r["a"]["criteria"]["p999"]["verdict"],))
    ch = _zero(); ch[0] = 199700; ch[5] = 300           # >16us 占 1.5e-3 > 1e-3
    r = analyze({"n": 200000, "counts": ch, "max_us": 32.0},
                {"n": 0, "counts": zero, "max_us": 0.0}, {}, noise, seg_bound=20.0)
    ck(r["a"]["criteria"]["p999"]["verdict"] == "RED",
       "S9 p99.9>16us 应 RED，实得 %r" % (r["a"]["criteria"]["p999"]["verdict"],))

    # S10: pops_tot != n_seg => WARN（不是 ERROR）
    seg_w = {"n": 30000, "counts": cs, "max_us": 9.0, "pops_tot": 30000 + 7}
    r = analyze({"n": 200000, "counts": ch7, "max_us": 1.0}, seg_w, {}, noise, seg_bound=20.0)
    ck(any("pops_tot" in w for w in r["warnings"]), "S10 pops_tot 不一致应 WARN: %r" % (r["warnings"],))
    ck(r["b"]["verdict"] != "ERROR", "S10 pops_tot 不一致不应 ERROR，实得 %r" % (r["b"]["verdict"],))

    # S11: cold-dense 归类：不判 5us/10us 门禁；tau_t=步时/pops<=2us、p99.9<=1ms、over1ms=0
    ch = _zero(); ch[0] = 199600; ch[5] = 350; ch[7] = 50      # 全在 <=256us 内
    hist = {"n": sum(ch), "counts": ch, "max_us": 420.0}
    tops = [{"kind": "mark", "why": "advance", "cpu_us": 420.0, "pops": 256, "seg": 0,
             "words": 0, "chunk": 128},
            {"kind": "mark", "why": "advance", "cpu_us": 250.0, "pops": 256, "seg": 0,
             "words": 0, "chunk": 128}]
    r = analyze(hist, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_pointer_dense", tops=tops, seg_bound=20.0)
    ck(r["class"] == "cold-dense", "S11 class 应为 cold-dense，实得 %r" % (r["class"],))
    ck(abs(r["a"]["per_pop_us"] - 420.0 / 256) < 1e-9,
       "S11 tau_t 应为 420/256=%.4fus，实得 %r" % (420 / 256, r["a"].get("per_pop_us")))
    ck(r["a"]["criteria"]["tau_t"]["verdict"] == "PASS",
       "S11 tau_t=%.3fus <=2us 应 PASS，实得 %r" % (420 / 256, r["a"]["criteria"]["tau_t"],))
    ck("over50" not in r["a"]["criteria"], "S11 cold-dense 不应有 >50us 判定项（只 OBS）")
    ck(r["a"]["criteria"]["over50_info"]["verdict"] == "OBS", "S11 >50us 应记 OBS")

    # S12: cold-dense 慢 pop（tau=3us/pop）=> RED，即便 p99.9<=1ms
    tops2 = [{"kind": "mark", "why": "advance", "cpu_us": 768.0, "pops": 256, "seg": 0,
              "words": 0, "chunk": 128}]
    r = analyze(hist, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_pointer_dense", tops=tops2, seg_bound=20.0)
    ck(r["a"]["criteria"]["tau_t"]["verdict"] == "RED",
       "S12 tau_t=3us/pop 应 RED，实得 %r" % (r["a"]["criteria"]["tau_t"],))
    ck(r["a"]["criteria"]["p999"]["verdict"] != "RED", "S12 p99.9 不应误判 RED")

    # S12b: 只有 pops=1 的 seg>0 慢步 => tau_t N/A（那属于 (b) 通道，不是冷 pop 税）
    tops3 = [{"kind": "mark", "why": "advance", "cpu_us": 424.0, "pops": 1, "seg": 8192,
              "words": 0, "chunk": 8192}]
    r = analyze(hist, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_pointer_dense", tops=tops3, seg_bound=333.0)
    ck(r["a"]["criteria"]["tau_t"]["verdict"] == "N/A",
       "S12b 仅 pops=1 时 tau_t 应 N/A（属 (b) 续扫），实得 %r" % (r["a"]["criteria"]["tau_t"],))

    # S13: 闭式 B_seg = c_seg*chunk*1.5 + 1us；chunk 缺 => N/A
    b, note = seg_bound_us(128, 27.0)
    want = 27.0 * 128 * 1.5 / 1000.0 + 1.0
    ck(b is not None and abs(b["lo"] - want) < 1e-9 and abs(b["hi"] - want) < 1e-9,
       "S13 B_seg 计算错: %r（期望 %.3f）" % (b, want))
    ck(b["hi"] <= 10.0, "S13 chunk=128、c_seg=27ns 时 B_seg 应 <=10us（实得 %.3fus）" % b["hi"])
    b2, note2 = seg_bound_us(None, 27.0)
    ck(b2 is None and "chunk" in note2, "S13 缺 chunk 应 N/A: %r %r" % (b2, note2))
    b3, note3 = seg_bound_us(128, None)
    ck(b3 is None and "c_seg" in note3, "S13 缺 c_seg 应 N/A（不退回旧占位）: %r %r" % (b3, note3))

    # S14: 文本解析往返（[segsteps] 含 pops_tot + [histo] + [chan] + [top]）
    ch = _zero(); ch[0] = 199000; ch[8] = 1000
    cs = _zero(); cs[0] = 900; cs[8] = 100
    txt = (_hist_text(sum(ch), ch) + "\n"
           + _hist_text(sum(cs), cs, "segsteps", max_us=7.5) .replace(
               "[segsteps] n=", "[segsteps] pops_tot=%d n=" % sum(cs))
           + "\n" + _chan_text("collect", 3, _zero(), 150.0) + "\n"
           + "[top] rank=1 kind=mark why=advance dur_us=420.9 cpu_us=420.9 freed=0 words=0 "
             "seg=0 pops=256 coll=1 step=7 first=0 trans=0 minflt=0 thr=8 mmax=8 scale=1 "
             "pressure=0 chunk=128\n")
    s = parse_segsteps(txt)
    ck(s and s["n"] == 1000 and s["counts"][8] == 100 and s["max_us"] == 7.5,
       "S14 [segsteps] 解析失败: %r" % (s,))
    ck(s and s.get("pops_tot") == 1000, "S14 pops_tot 解析失败: %r" % (s,))
    rec = pp.parse_text(txt)
    ck(rec["histo"]["n"] == 200000, "S14 [histo] 未被 pause_parse 识别")
    tt = parse_top(txt)
    ck(tt and tt[0]["pops"] == 256 and tt[0]["cpu_us"] == 420.9 and tt[0]["chunk"] == 128
       and tt[0]["kind"] == "mark", "S14 [top] 解析失败: %r" % (tt,))

    hist_c = hist   # cold-dense 用例的 [histo]（S11 构造，p99.9<=1ms 侧）
    # S15（m04591 改口径）：c_seg 区间只影响 p99.9 的兜底界；max 恒 OBS
    bnd, _ = seg_bound_us(128, 34.0, 52.0)
    ck(abs(bnd["lo"] - (1.5 * 34.0 * 128 / 1000.0 + 1.0)) < 1e-9, "S15 B_seg_lo 计算错: %r" % (bnd,))
    ck(abs(bnd["hi"] - (1.5 * 52.0 * 128 / 1000.0 + 1.0)) < 1e-9, "S15 B_seg_hi 计算错: %r" % (bnd,))
    ck(abs(bnd["lo"] - 7.528) < 1e-3 and abs(bnd["hi"] - 10.984) < 1e-3,
       "S15 chunk=128、c_seg=34..52 时 B_seg 应为 7.53..10.98us: %r" % (bnd,))
    vsc = lambda c, mx: verdict_step_channel("resume(seg>0)", 100000, c, mx, 0.0,
                                            mode="seg", p999_limit_us=bnd)
    z15 = _zero(); z15[0] = 100000
    r15 = vsc(z15, 9.0)
    ck(r15["criteria"]["p999"]["verdict"] == "PASS" and r15["criteria"]["max"]["verdict"] == "OBS",
       "S15 p99.9<=lo 应 PASS 且 max OBS，实得 %r / %r"
       % (r15["criteria"]["p999"], r15["criteria"]["max"]))
    z15b = _zero(); z15b[0] = 99850; z15b[4] = 150
    r15 = vsc(z15b, 9.0)
    ck(r15["criteria"]["p999"]["verdict"] == "N/A",
       "S15 p99.9 跨 7.53..10.98 桶界应 N/A，实得 %r" % (r15["criteria"]["p999"],))
    z15c = _zero(); z15c[0] = 99800; z15c[5] = 200
    r15 = vsc(z15c, 12.0)
    ck(r15["criteria"]["p999"]["verdict"] == "RED",
       "S15 p99.9>hi 应 RED，实得 %r" % (r15["criteria"]["p999"],))
    # S16: --c-seg 解析（按形状，可逗号分隔多个；带区间/单值）
    csg = parse_cseg(["pd_pointer_dense=34:52,pd_bigobj=9.5"])
    ck(len(csg) == 2 and csg[0] == ("pd_pointer_dense", 34.0, 52.0)
       and csg[1] == ("pd_bigobj", 9.5, 9.5), "S16 parse_cseg 失败: %r" % (csg,))
    try:
        parse_cseg(["oops"])
        ck(False, "S16 无 = 的 --c-seg 应 ValueError")
    except ValueError:
        pass

    # S17: pops>=1 的 OBS（含 pops=1）：单独记录、不判；pops=1&seg>0 标 is_seg_resume
    tops4 = [{"kind": "mark", "why": "advance", "cpu_us": 424.0, "pops": 1, "seg": 8192,
              "words": 0, "chunk": 8192}]
    r = analyze(hist_c, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_pointer_dense", tops=tops4, seg_bound=bnd)
    ob = r["a"].get("per_pop_obs_all")
    ck(ob and abs(ob["tau_us"] - 424.0) < 1e-9 and ob["is_seg_resume"] is True,
       "S17 pops>=1 OBS 应记录 tau=424us 且标 is_seg_resume: %r" % (ob,))
    ck(r["a"]["criteria"]["tau_t"]["verdict"] == "N/A",
       "S17 tau_t 仍应 N/A（[top] 无 pops>=2 样本）")

    # S18（Lead m04591）：p99.9 在 1.5x 界内、max 超 50us => (b) PASS（max 只 OBS）
    z18 = _zero(); z18[0] = 100000
    b18 = {"lo": 20.0, "hi": 20.0}
    v18 = verdict_step_channel("resume(seg>0)", 100000, z18, 300.0, 0.0, mode="seg", p999_limit_us=b18)
    ck(v18["criteria"]["p999"]["verdict"] == "PASS" and v18["verdict"] == "PASS",
       "S18 p99.9<=B_seg => (b) 应 PASS，实得 %r" % (v18.get("verdict"),))
    ck(v18["criteria"]["max"]["verdict"] == "OBS",
       "S18 max=300us 超 50us 但应只记 OBS，实得 %r" % (v18["criteria"]["max"],))
    # S19：p99.9 超 1.5x 闭式 => RED
    z19 = _zero(); z19[0] = 99800; z19[6] = 200
    v19 = verdict_step_channel("resume(seg>0)", 100000, z19, 40.0, 0.0, mode="seg", p999_limit_us=b18)
    ck(v19["criteria"]["p999"]["verdict"] == "RED" and "B_seg" in v19["criteria"]["p999"]["why"],
       "S19 p99.9>B_seg 应 RED（提 B_seg），实得 %r" % (v19["criteria"]["p999"],))
    # S20（m04755 改口径）：闭式很宽（100us）但 p99.9 落在 b128us 桶 => 超 64us 桶界兜底 => RED
    z20 = _zero(); z20[0] = 99800; z20[7] = 200
    b20 = {"lo": 100.0, "hi": 100.0}
    v20 = verdict_step_channel("resume(seg>0)", 100000, z20, 90.0, 0.0, mode="seg", p999_limit_us=b20)
    ck(v20["criteria"]["p999"]["verdict"] == "RED" and "桶界兜底" in v20["criteria"]["p999"]["why"],
       "S20 p99.9>64us 桶界兜底应 RED，实得 %r" % (v20["criteria"]["p999"],))
    # S20b：p99.9 落在 b64us 桶（56.4us）且闭式 85.6us => 桶界只能证明到 <=64us => PASS（**已知盲区**，m04755）
    z20b = _zero(); z20b[0] = 99900; z20b[6] = 100
    b20b = {"lo": 85.6, "hi": 85.6}
    v20b = verdict_step_channel("resume(seg>0)", 100000, z20b, 60.0, 0.0, mode="seg", p999_limit_us=b20b)
    ck(v20b["criteria"]["p999"]["verdict"] == "PASS",
       "S20b b64us 桶内 p99.9 在 64us 兜底下应 PASS（盲区：真实值可能 >50us），实得 %r"
       % (v20b["criteria"]["p999"],))
    # S21：bseg-cal.tsv 新 9 列解析 + 最长名匹配 + B_seg 闭式 + spread/src_md5 进 note + 偏差 WARN
    import tempfile
    tp = os.path.join(tempfile.mkdtemp(), "cal.tsv")
    with open(tp, "w") as f:
        f.write("# shape\tchunk\tc_seg_ns\tp99.9_us\tp99.9_min_us\tp99.9_max_us\trun_id\tsrc_md5\textc_md5\n")
        f.write("pd_bigobj\t8192\t4.160\t34.08\t34.08\t47.53\trun-x\t"
                "reclaim.c=aaaa,codegen.c=bbbb\texc=deadbeef\n")
        f.write("pd_pointer_dense\t8192\t6.885\t56.40\t56.40\t65.05\trun-y\t"
                "reclaim.c=cccc,codegen.c=dddd\texc=cafebabe\n")
    cal = load_bseg_cal(tp)
    ck(len(cal) == 2, "S21 标定表应解析 2 行，实得 %r" % (cal,))
    k21, row21 = resolve_bseg_cal(cal, "pd_pointer_dense")
    ck(k21 == "pd_pointer_dense", "S21 最长名匹配应选 pd_pointer_dense，实得 %r" % (k21,))
    ck(abs(row21.get("p99.9_min_us", 0) - 56.40) < 1e-6
       and abs(row21.get("p99.9_max_us", 0) - 65.05) < 1e-6,
       "S21 spread 列解析错：%r" % (row21,))
    ck(row21.get("src_md5", "") == "reclaim.c=cccc,codegen.c=dddd",
       "S21 src_md5 列解析错：%r" % (row21.get("src_md5"),))
    b21, n21 = seg_bound_from_cal(k21, row21)
    want21 = 1.5 * 6.885 * 8192 / 1000.0 + 1.0
    ck(b21 is not None and abs(b21["lo"] - want21) < 1e-6,
       "S21 B_seg 应=%.3fus，实得 %r" % (want21, b21))
    ck("run-y" in n21 and "chunk=8192" in n21, "S21 标定来源应进 note：%r" % (n21,))
    ck("56.40..65.05us" in n21 and "src_md5=" in n21, "S21 spread 与 src_md5 应进 note：%r" % (n21,))
    badrow = dict(row21); badrow["c_seg_ns"] = 1.0
    _, n21b = seg_bound_from_cal(k21, badrow)
    ck("WARN" in n21b, "S21 c_seg 与 p99.9/chunk 偏差应 WARN：%r" % (n21b,))
    # S21b：兼容旧 6 列表（min=max=p99.9、src_md5 空）
    tp2 = os.path.join(tempfile.mkdtemp(), "cal_legacy.tsv")
    with open(tp2, "w") as f:
        f.write("pd_bigobj\t8192\t4.160\t34.08\trun-old\tdeadbeef\n")
    cal2 = load_bseg_cal(tp2)
    ck(len(cal2) == 1 and abs(cal2["pd_bigobj"]["p99.9_min_us"] - 34.08) < 1e-6
       and cal2["pd_bigobj"]["src_md5"] == "" and cal2["pd_bigobj"]["extc_md5"] == "deadbeef",
       "S21b 旧 6 列兼容解析失败：%r" % (cal2,))

    # S22（Lead m03564）：混合通道形状（pd_bigobj / pd_bigobj60 / pd）
    # (a) 必须记 N/A（插桩不按 n 归因），不得套 warm 的 >50us <=1e-4 + p99.9<=10us；
    # over50 只 OBS；(b) 仍按 B_seg 判；(a) 的 N/A 不能让整体结论变 PASS。
    ch22 = _zero(); ch22[0] = 150000; ch22[3] = 4; ch22[5] = 500; ch22[11] = 0
    hist22 = {"n": sum(ch22), "counts": ch22, "max_us": 216.9}
    r = analyze(hist22, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_bigobj", seg_bound=145.2)
    ck(r["class"] == "mixed", "S22 pd_bigobj 应归 mixed，实得 %r" % (r["class"],))
    ck(r["a"]["verdict"] == "N/A", "S22 混合形状 (a) 应 N/A，实得 %r" % (r["a"]["verdict"],))
    ck(r["a"]["criteria"]["slice_le5"]["verdict"] == "N/A",
       "S22 (a) 的 <=5us 切片应 N/A（m03564），实得 %r" % (r["a"]["criteria"]["slice_le5"],))
    ck("over50" not in r["a"]["criteria"] and r["a"]["criteria"]["over50_info"]["verdict"] == "OBS",
       "S22 混合形状 >50us 应只 OBS，实得 %r" % (r["a"]["criteria"],))
    ck(r["a"]["criteria"]["over50_info"]["why"].find(">4us=504") >= 0,
       "S22 OBS 文案应带 >4us 计数（b8us 起 4 步 + b32us 500 步 = 504）：%r"
       % (r["a"]["criteria"]["over50_info"]["why"],))
    r = analyze(hist22, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_bigobj60", seg_bound=145.2)
    ck(r["class"] == "mixed", "S22 pd_bigobj60 应归 mixed，实得 %r" % (r["class"],))
    r = analyze(hist22, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="pd_pointer_dense", seg_bound=85.6)
    ck(r["class"] == "cold-dense",
       "S22 归类顺序错：pd_pointer_dense 必须仍归 cold-dense（先判 cold 再判 mixed），实得 %r"
       % (r["class"],))
    r = analyze(hist22, {"n": 0, "counts": zero, "max_us": 0.0, "pops_tot": 0}, {}, noise,
                case="cyc_pause", seg_bound=52.1)
    ck(r["class"] == "warm", "S22 warm 形状不应被误归 mixed，实得 %r" % (r["class"],))

    # S23（v17）：同一份 [histo]，桶界是跨桶 N/A，精确计数（gt50us）可判 PASS —— 判据可判化
    cs23 = _zero(); cs23[6] = 30            # 30 步落在 b64us（32-64us）桶内，桶界分不出 <=50us 的占比
    h23 = {"n": 200000, "counts": cs23, "max_us": 60.0,
           "gt10": 30, "gt50": 10, "gt64": 0}
    s23 = {"n": 0, "counts": _zero(), "max_us": None, "pops_tot": 0,
           "gt10": 0, "gt50": 0, "gt64": 0}
    r23 = analyze(h23, s23, {}, noise, case="cyc")
    ck(r23["a"]["exact_over"] is True, "S23 有 gt* 字段时 exact_over 应 True")
    ck(r23["a"]["criteria"]["over50"]["verdict"] == "PASS",
       "S23 精确 gt50=10/2e5=5e-5 应 PASS，实得 %r" % (r23["a"]["criteria"]["over50"],))
    ck(r23["a"]["criteria"]["p999"]["why"].startswith("精确计数口径"),
       "S23 p99.9 的 why 应标注精确计数口径：%r" % (r23["a"]["criteria"]["p999"],))
    h23b = {k: v for k, v in h23.items() if k not in ("gt10", "gt50", "gt64")}
    r23b = analyze(h23b, s23, {}, noise, case="cyc")
    ck(r23b["a"]["criteria"]["over50"]["verdict"] == "N/A",
       "S23 对照：同一桶数据去掉 gt* 后桶界应 N/A，实得 %r" % (r23b["a"]["criteria"]["over50"],))
    # S24：精确计数把 RED 也判出来（桶界同输入只能 N/A）
    h24 = dict(h23); h24["gt50"] = 30       # 30/2e5 = 1.5e-4 > 1e-4
    r24 = analyze(h24, s23, {}, noise, case="cyc")
    ck(r24["a"]["criteria"]["over50"]["verdict"] == "RED",
       "S24 精确 gt50=30/2e5 应 RED，实得 %r" % (r24["a"]["criteria"]["over50"],))
    # S25：非阈值 L 只能给界（靠 {>t} 的包含关系）；L 恰为阈值时上下界相等
    g25 = {10: 100, 50: 40, 64: 7}
    ck(exact_over_bounds(g25, 50) == (40, 40), "S25 L=50 应精确 (40,40)")
    ck(exact_over_bounds(g25, 40) == (40, 100), "S25 L=40 应为 [gt50, gt10]=(40,100)")
    ck(exact_over_bounds(g25, 5) == (100, None), "S25 L=5 无上界 => (gt10, None)")
    ck(exact_over_bounds(g25, 70) == (0, 7), "S25 L=70 => 下界 0、上界 gt64")
    ck(gt_sub({10: 1, 50: 1, 64: 0}, {10: 2, 50: 0, 64: 0}) == "ERROR",
       "S25 histo-segsteps 出现负值应 ERROR")
    ck(gt_of({"gt10": 1, "gt50": 0}) is None, "S25 缺 gt64 应 None（退回桶界）")
    # S26：(b) 通道的精确计数判定（64us 桶界兜底改为精确 gt64）
    v26 = verdict_step_channel("resume(seg>0)", 200000, _zero(), 70.0, 0.0, mode="seg",
                               p999_limit_us=50.0, gt={10: 5, 50: 2, 64: 0})
    ck(v26["criteria"]["p999"]["verdict"] == "PASS",
       "S26 seg 精确 gt64=0 时 p99.9<=50us(与 64us 兜底) 应 PASS，实得 %r" % (v26["criteria"]["p999"],))
    v26b = verdict_step_channel("resume(seg>0)", 200000, _zero(), 70.0, 0.0, mode="seg",
                                p999_limit_us=50.0, gt={10: 400, 50: 300, 64: 300})
    ck(v26b["criteria"]["p999"]["verdict"] == "RED",
       "S26 seg 精确 gt64=300/2e5=1.5e-3 应 RED（64us 兜底），实得 %r" % (v26b["criteria"]["p999"],))
    # S27：[segsteps] 的 gt* 字段解析 + (a) 精确计数 = histo - segsteps
    s27 = parse_segsteps("[segsteps] n=1000 pops_tot=1000 max_us=3.0 b1us=1000 gt10us=2 gt50us=1 gt64us=0")
    ck((s27["gt10"], s27["gt50"], s27["gt64"]) == (2, 1, 0),
       "S27 [segsteps] gt* 解析失败：%r" % (s27,))
    ck(gt_sub({10: 30, 50: 10, 64: 0}, {10: 2, 50: 1, 64: 0}) == {10: 28, 50: 9, 64: 0},
       "S27 (a) 精确计数差分错")

    print("g1c selftest:")
    print("  S1 seg 差分+pops_tot 一致 / S2 恰好 50us=N/A / S3 恰好 1ms=PASS / S4 n<1e5=N/A")
    print("  S5 扣噪声 RED->PASS / S6 不一致=ERROR / S7 (b) 闭式 12us PASS vs 25us RED")
    print("  S8 未标定 B_seg => N/A（不退回旧 5us/10us） / S9 p99.9 跨桶 N/A 与 >16us RED")
    print("  S10 pops_tot 不一致=WARN（非 ERROR） / S11 cold-dense 只判 tau_t+1ms+over1ms")
    print("  S12 cold-dense tau=3us/pop => RED / S12b 仅 pops=1 => tau_t N/A")
    print("  S13 B_seg 闭式与缺参 / S14 文本解析往返")
    print("  S15 c_seg 区间 => max 区间内 N/A、<=lo PASS、>hi RED / S16 --c-seg 解析")
    print("  S17 pops>=1 OBS（含 pops=1 续扫段标记）")
    print("  S18 max>50us 只 OBS（(b) PASS） / S19 p99.9>1.5x => RED / S20 p99.9>64us 桶界兜底 => RED")
    print("  S20b p99.9 在 b64us 桶 ⇒ 兜底只能给 PASS（已知盲区，m04755）")
    print("  S21 bseg-cal 9 列解析+最长名匹配+B_seg+spread/src_md5/偏差 WARN / S21b 旧 6 列兼容")
    print("  S22 混合通道（pd_bigobj/pd_bigobj60）=> class=mixed、(a) N/A、>50us 只 OBS、"
          "dense 仍 cold-dense、warm 不误归")
    print("  S23/S24 精确计数（gt50us）把桶界 N/A 变成 PASS/RED / S25 包含关系与 ERROR")
    print("  S26 seg 用 gt64 判 64us 兜底 / S27 [segsteps] gt* 解析 + (a) 差分")
    if bad:
        print("  ⇒ SELFTEST FAIL: " + "; ".join(bad))
        return 1
    print("  ⇒ SELFTEST PASS")
    return 0
def parse_cseg(items):
    """解析 --c-seg：'name=LO[:HI]'（每项可逗号分隔多个）。返回 [(name, lo, hi)]。
    c_seg 必须按形状给（Lead m04448：不得跨形状借）；name 按用例名子串匹配（取最长匹配）。"""
    out = []
    for it in items or []:
        for part in str(it).split(","):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                raise ValueError("--c-seg 需要 NAME=LO[:HI] 形式，got %r" % part)
            name, val = part.split("=", 1)
            if ":" in val:
                lo_s, hi_s = val.split(":", 1)
                lo, hi = float(lo_s), float(hi_s)
            else:
                lo = hi = float(val)
            out.append((name.strip(), lo, hi))
    return out


def group_paths(paths):
    """按用例分组：文件名 token_pause_rN.err => 用例名 token（不同负载不能混在一个分母里）。"""
    groups = {}
    for p in paths:
        base = os.path.basename(p)
        case = base.split("_pause_r")[0] if "_pause_r" in base else base.rsplit(".", 1)[0]
        groups.setdefault(case, []).append(p)
    return groups


_ORDER = {"ERROR": 3, "RED": 2, "N/A": 1, "PASS": 0}


def worst_verdict(vs):
    """汇总判定 = 最差（ERROR > RED > N/A > PASS）；空 => N/A。"""
    if not vs:
        return "N/A"
    return max(vs, key=lambda v: _ORDER.get(v, 1))


def main(argv=None):
    ap = argparse.ArgumentParser(description="G1② 通道拆分（[histo]-[segsteps] + [chan] + [top]）")
    ap.add_argument("errs", nargs="*", help="pause stderr 文件（*.err）")
    ap.add_argument("--dir", help="从目录里取全部 *.err（递归一层）")
    ap.add_argument("--glob", default="*.err", help="配合 --dir 的 glob（默认 *.err）")
    ap.add_argument("--noise", action="append", default=[], help="noise_floor 输出文件（可多次）")
    ap.add_argument("--noise-dir", help="目录下全部 noise*.txt")
    ap.add_argument("--json", help="把结果写到该 json")
    ap.add_argument("--cold-dense", action="append", default=[],
                    help="cold-dense 归类用的用例名子串（可多次；默认 pd_pointer_dense）")
    ap.add_argument("--mixed", action="append", default=[],
                    help="混合通道归类用的用例名子串（可多次；默认 pd_bigobj/pd_bigobj60/pd）。"
                         "这类形状 (a) 记 N/A（Lead m03564），只判 (b)/(c)")
    ap.add_argument("--c-seg", action="append", default=[],
                    help="(b) 按形状的实测每 word 成本 NAME=LO[:HI]（ns/word，可逗号分隔多个）；"
                         "必须来自该形状自己的实测（不得跨形状借）")
    ap.add_argument("--c-scan-ns", type=float, default=C_SCAN_NS_PER_WORD,
                    help="(b) 旧的全局 c_seg（仅当未给 --c-seg 时兜底；报告会标注可能跨形状）")
    ap.add_argument("--seg-max-us", type=float, default=None,
                    help="(b) 直接给 B_seg（覆盖 C_scan*chunk*1.5+1 的计算）")
    ap.add_argument("--bseg-cal", default=BSEG_CAL_DEFAULT,
                    help="(b) 标定表 TSV（默认 tests/gc-p0/bseg-cal.tsv；Lead m04591 的可审计口径）")
    ap.add_argument("--src-md5", default=None,
                    help="当前源码 md5（reclaim.c+codegen.c，溯源以它为准），只用于标定行告警（不参与判定）")
    ap.add_argument("--build-md5", default=None,
                    help="当前编译器二进制 md5（辅助溯源），只用于标定行告警（不参与判定）")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return _selftest()
    paths = list(a.errs)
    if a.dir:
        paths += sorted(glob.glob(os.path.join(a.dir, a.glob)))
    if not paths:
        print("g1c: 没有输入 .err（--dir/位置参数）；无数据 => N/A（不是 PASS）")
        return 0
    npaths = []
    for p in a.noise:
        npaths.append(p)
    if a.noise_dir:
        npaths += sorted(glob.glob(os.path.join(a.noise_dir, "noise*.txt")))
    noise = load_noise(npaths)
    cold = tuple(a.cold_dense) if a.cold_dense else COLD_DENSE_DEFAULT
    mixed = tuple(a.mixed) if a.mixed else MIXED_DEFAULT
    cseg = parse_cseg(a.c_seg)
    groups = group_paths(paths)
    per_case = {}
    for case in sorted(groups):
        hist, seg, chan, tops, nfiles = load_errs(groups[case])
        chunk = None
        for t in tops:
            if t.get("chunk"):
                chunk = max(chunk or 0, int(t["chunk"]))
        c_lo = c_hi = None
        csrc = ""
        best = None
        for nm, lo, hi in cseg:
            if nm.lower() in case.lower() and (best is None or len(nm) > len(best[0])):
                best = (nm, lo, hi)
        if best is not None:
            c_lo, c_hi = best[1], best[2]
            csrc = "--c-seg %s=%.2f..%.2f ns/word（该形状实测）" % (best[0], c_lo, c_hi)
        elif a.c_scan_ns is not None:
            c_lo = c_hi = a.c_scan_ns
            csrc = "--c-scan-ns %.2f ns/word（全局兜底：可能跨形状借，报告须标注）" % a.c_scan_ns
        cal_key, cal_row = (None, None)
        if a.bseg_cal:
            cal_key, cal_row = resolve_bseg_cal(load_bseg_cal(a.bseg_cal), case)
        if a.seg_max_us is not None:
            bound = {"lo": float(a.seg_max_us), "hi": float(a.seg_max_us)}
            bnote = "B_seg=%.2fus（--seg-max-us 直接给定）" % a.seg_max_us
            csrc = csrc or "--seg-max-us 手工给定"
        elif c_lo is not None:
            # --c-seg / --c-scan-ns 手工覆盖：保留给 selftest 与临时排查，**不是**门禁口径。
            bound, bnote = seg_bound_us(chunk, c_lo, c_hi)
            if bound is not None:
                bnote = "%s（chunk=%d 取自 [top]）；c_seg 来源：%s（手工覆盖，门禁口径见 bseg-cal.tsv）" % (
                    bnote, chunk, csrc)
        elif cal_key is not None:
            bound, bnote = seg_bound_from_cal(cal_key, cal_row)
            if bound is not None:
                csrc = "bseg-cal.tsv[%s]" % cal_key
                cal_src = (cal_row.get("src_md5") or "").strip()
                if a.src_md5 and cal_src and not cal_src.startswith("unknown") and cal_src != a.src_md5:
                    bnote += (" ⚠ 标定 src_md5=%s 与当前 --src-md5=%s 不一致（标定行陈旧 ⇒ 必须重标定）"
                              % (cal_src, a.src_md5))
                elif not cal_src or cal_src.startswith("unknown"):
                    bnote += " ⚠ 标定行 src_md5 未知（ref 数据）⇒ 正式门禁前必须用本 build 重标定"
                cal_extc = (cal_row.get("extc_md5") or "").strip()
                if a.build_md5 and cal_extc and not cal_extc.startswith("unknown") and cal_extc != a.build_md5:
                    bnote += " （辅助：标定 extc_md5=%s ≠ 当前 --build-md5=%s）" % (cal_extc, a.build_md5)
        else:
            bound = None
            bnote = ("B_seg(shape) 未标定：bseg-cal.tsv 缺该形状（1.5*c_seg(shape)*chunk+1us；"
                     "不退回 5us 旧占位）")
        res = analyze(hist, seg, chan, noise, case=case, cold_dense=cold,
                      seg_bound=bound, seg_bound_note=bnote, tops=tops, mixed_shapes=mixed)
        res["files"] = groups[case]
        res["case"] = case
        res["chunk"] = chunk
        res["c_seg_source"] = csrc
        per_case[case] = res
        print_report(res, label=case)
        print("")
    overall = worst_verdict([r["verdict"] for r in per_case.values()])
    if len(per_case) > 1:
        print("== G1② 汇总（逐用例判，取最差；不同负载不混分母）==")
        for case in sorted(per_case):
            r = per_case[case]
            b = r.get("b", {})
            print("  %-17s class=%-10s => %-5s（a=%s b=%s c=%s；B_seg=%s）" % (
                case, r.get("class"), r["verdict"],
                r.get("a", {}).get("verdict"), b.get("verdict"),
                ",".join("%s=%s" % (k, v["verdict"]) for k, v in r["channels"].items()) or "-",
                _bnd_str(r.get("seg_bound_us"))))
        print("  ⇒ G1② 判定（最差）：**%s**" % overall)
    if a.json:
        out = {"verdict": overall, "noise": noise, "noise_files": npaths, "min_n": MIN_N,
               "cold_dense": list(cold), "c_scan_ns": a.c_scan_ns, "c_seg_arg": a.c_seg,
               "seg_max_us_arg": a.seg_max_us, "bseg_cal": a.bseg_cal,
               "src_md5": a.src_md5, "build_md5": a.build_md5, "bseg_chunk": BSEG_CHUNK,
               "p999_seg_cap_us": P999_SEG_CAP_US, "cases": per_case}
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1, sort_keys=True)
        print("  （json → %s）" % a.json)
    if overall in ("RED", "ERROR"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
