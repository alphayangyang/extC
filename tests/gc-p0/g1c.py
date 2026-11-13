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
   细直方图（Lead R3）：`[segsteps]` 行尾的 `us<n>=<count>` 是 seg 步的逐微秒直方图（只发 >=16us 的桶）。
   B_seg 落在 [histo] 粗桶内部时（pd_bigobj 的 49.07us 落在 32..64us 桶），粗桶只能把 count(>B_seg) 夹在两个
   数量级之间 => 跨桶 N/A；细直方图把夹逼压到一两个样本，(b) 的 p99.9 因此可判（p999_any()，JSON 记 p999_source）。
   (b) 的**结构性 vacuous**（Lead R3）：[work] maxreq <= EXTC_RC_SCAN_CHUNK*8 且观测 n_seg==0
   => 段通道结构上不可达（没有对象会被切段），记 PASS(vacuous) 并写明理由；maxreq 未知/超阈则仍 N/A。
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
# ---- clock-free 每步工作量上界（Lead R7）----
# 判据不读钟：三个量都由编译期常量约束，所以宿主 steal（本机空转实测注入 88-236us）无法让它
# 假红/假绿。它是本轮堵毫秒尾的正面判据；墙钟 max 只留 OBS（见 GC-ROOFLINE 6.13）。
POP_MAX_LIMIT = 512          # EXTC_RC_MARK_STEP_POP_MAX：一步最多 pop 512 个对象
PUSH_STEP = 2048             # EXTC_RC_MARK_PUSH_STEP：一步的 push 记账额度
BATCH_POP = 200              # EXTC_RC_MARK_BATCH_POP：批路一次最多弹 200 个
FANOUT = 32                  # codegen 描述符上限（引用字段超 32 个即退回保守扫描）
MSCAP_LIMIT = 16384          # 2 * EXTC_RC_MARK_STACK_SOFT(8192)
PUSH_MAX_LIMIT = PUSH_STEP + BATCH_POP * FANOUT   # = 8448
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
            "gt10": vals.get("gt10us"), "gt50": vals.get("gt50us"), "gt64": vals.get("gt64us"),
            "fine": {int(a): int(b) for a, b in re.findall(r"\bus(\d+)=(\d+)", line)}}


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
               "total_us": src.get("total_us"),
               "pops_tot": src.get("pops_tot"),
               "gt10": src.get("gt10"), "gt50": src.get("gt50"), "gt64": src.get("gt64"),
               "fine": dict(src.get("fine") or {})}
        return dst
    dst["n"] += src["n"]
    for i in range(pp.HISTO_NB):
        dst["counts"][i] += src["counts"][i]
    if src.get("max_us") is not None:
        dst["max_us"] = max(dst["max_us"] or 0.0, src["max_us"])
    if src.get("pops_tot") is not None:
        dst["pops_tot"] = (dst.get("pops_tot") or 0) + src["pops_tot"]
    if src.get("total_us") is not None:
        dst["total_us"] = (dst.get("total_us") or 0.0) + src["total_us"]
    for k in ("gt10", "gt50", "gt64"):
        a, b = dst.get(k), src.get(k)
        dst[k] = None if (a is None or b is None) else a + b
    for y, v in (src.get("fine") or {}).items():
        dst.setdefault("fine", {})[y] = dst["fine"].get(y, 0) + v
    return dst


def load_errs(paths):
    """读取多个 .err，聚合 [histo]/[segsteps]/[chan]/[top]。
    返回 (hist, seg, chan, tops, files)；tops 按 cpu_us 降序保留前 8 条（跨 rep）。"""
    hist = seg = None
    chan = {}
    tops = []
    work_maxreq = None   # [work] maxreq：本 run 记录到的最大分配请求字节（(b) 结构性 vacuous 判定用，Lead R3）
    # [wbudget] 高水位聚合（Lead R7）：pop_max/push_max/mscap_max 取跨 rep 最大值，
    # pops_tot 求和（每步的 pop 数总量，供 cold-dense 的聚合 tau 用）。
    wb = None
    for p in paths:
        with open(p, "r", errors="replace") as f:
            txt = f.read()
        rec = pp.parse_text(txt)
        _wl = rec.get("wbudget_line")
        if _wl:
            _f = {}
            for _k, _v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", _wl):
                _f[_k] = int(_v)
            if "pop_max" in _f:
                if wb is None:
                    wb = {"pop_max": 0, "push_max": 0, "mscap_max": 0, "msp_max": 0,
                          "tot_max": 0, "pops_tot": 0, "reps": 0}
                for _k in ("pop_max", "push_max", "mscap_max", "msp_max", "tot_max"):
                    if _k in _f:
                        wb[_k] = max(wb[_k], _f[_k])
                wb["pops_tot"] += _f.get("pops_tot") or 0
                wb["reps"] += 1
        _wr = (rec.get("work") or {}).get("maxreq")
        if _wr is not None and _wr >= 0:
            work_maxreq = _wr if work_maxreq is None else max(work_maxreq, _wr)
        h = rec.get("histo")
        if h and h.get("n") is not None:
            hist = _merge(hist, {"n": h["n"], "counts": h["counts"],
                                 "max_us": rec.get("step_max_us"),
                                 "total_us": rec.get("step_total_us"),
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
    return hist, seg, chan, tops[:8], len(paths), work_maxreq, wb


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


def p999_exact_net(gt, n, limit_us, env=0.0):
    """p99.9<=limit 的判定，**先扣掉外部环境地板** env（本 run 期间机器固有的停顿次数）。

    env 来自同机同核相邻时间窗的 stall 探针（noise_floor.c --rate：纯计算循环里 >10us 的窗口个数
    除以秒数），再乘本 rep 的墙钟时长；前后两次探针取**较小值**（最少扣减 = 最严）。依据：宿主抢占
    按时间到达，与本步预算无关，一步被打中就记一条慢步 —— 那是量具误差，不是回收工作。缺探针数据
    时 env=0，退回原始口径（不变宽）。判据数字一个字不改：净 count(>L)*1000 <= n。"""
    if not n:
        return "N/A", "n=0"
    lo, hi = exact_over_bounds(gt, limit_us)
    env_i = int(round(env)) if env and env > 0 else 0
    lo_n = max(0, lo - env_i)
    hi_n = None if hi is None else max(0, hi - env_i)
    tag = ""
    if env_i:
        raw = hi if hi is not None else lo
        tag = "（原始 count(>%gus)=%d，扣环境地板 %d）" % (limit_us, raw, env_i)
    if hi_n is not None and hi_n * 1000 <= n:
        return "PASS", "净 count(>%gus)=%d；%d*1000 <= n=%d%s" % (limit_us, hi_n, hi_n, n, tag)
    if lo_n * 1000 > n:
        return "RED", "净 count(>%gus)>=%d；%d*1000 > n=%d%s" % (limit_us, lo_n, lo_n, n, tag)
    return "N/A", "净 count(>%gus) 只能给到 [%d, %s]，相对 0.001*n 跨线%s" % (
        limit_us, lo_n, "无上界" if hi_n is None else str(hi_n), tag)

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


def fine_over_bounds(fine, limit_us):
    """用 [segsteps] 的逐微秒直方图给出 count(>limit) 的严格上下界。
    fine: {整微秒 y: 计数}，桶 y 装的是落在 [y, y+1) us 的步（运行时只发射 >=16us 的桶，故 limit>=16 才适用）。
    count(>L) 下界 = sum(y >= floor(L)+1)（这些桶整桶都 > L）；
    上界 = 下界 + 桶 floor(L)（该桶只有一段 > L，不能整桶算，也不能整桶丢）。"""
    if not fine or limit_us is None:
        return 0, None
    import math
    fl = int(math.floor(limit_us))
    lower = sum(v for y, v in fine.items() if y >= fl + 1)
    return lower, lower + int(fine.get(fl, 0))


def p999_any(counts, n, limit_us, gt=None, fine=None):
    """(b) 通道的 p99.9 判定：**先**用逐微秒细直方图，再退回精确计数 / 桶界。
    为什么需要细直方图：B_seg（pd_bigobj 49.07us）落在 [histo] 的 32us..64us 桶内，粗桶只能把
    count(>B_seg) 夹在两个数量级之间 => 跨桶不可判；逐微秒桶把夹逼压到一两个样本。
    返回 (verdict, why, source)，source in {"fine","exact","bucket"}，写进 JSON 便于审计口径。"""
    if n and fine and limit_us is not None and limit_us >= 16:
        lo, hi = fine_over_bounds(fine, limit_us)
        if hi is not None and hi * 1000 <= n:
            return "PASS", "细直方图：count(>%.2fus) <= %d；%d*1000 <= n=%d" % (
                limit_us, hi, hi, n), "fine"
        if lo * 1000 > n:
            return "RED", "细直方图：count(>%.2fus) >= %d；%d*1000 > n=%d" % (
                limit_us, lo, lo, n), "fine"
    if gt is not None:
        v, w = p999_exact(gt, n, limit_us)
        return v, w, "exact"
    v, w = p999_bounds(counts, n, limit_us)
    return v, w, "bucket"


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
                         ratio_limit=RATIO_LIMIT, min_n=MIN_N, gt=None, fine=None,
                         env_p999=0.0, env_p50=0.0):
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
            # Lead R3：>50us 通道同样扣**环境地板**（宿主停顿按时间到达，落进任一步都会把它推过 50us，
            # 与本步预算无关）。判据数字一个不改：净 count(>50us)/n <= 1e-4；扣 min(pre,post)*wall。
            env_i50 = int(round(env_p50)) if (env_p50 and env_p50 > 0) else 0
            if env_i50:
                out["env_p50"] = env_i50
            if exact and env_i50:
                lo_c = max(0, lo - env_i50)
                r_c = lo_c / n
                out["over_lo_count_net"] = lo_c
                out["over_lo_ratio_net"] = r_c
                if r_c - p_noise > ratio_limit:
                    out["criteria"]["over50"] = {"verdict": "RED",
                        "why": "精确 gt50=%d 扣环境地板 %d 次（min(pre,post)×wall）⇒ 净 %d（%.2e）减噪声 %.2e > %.0e"
                               % (lo, env_i50, lo_c, r_c, p_noise, ratio_limit)}
                else:
                    out["criteria"]["over50"] = {"verdict": "PASS",
                        "why": "精确 gt50=%d 扣环境地板 %d 次（min(pre,post)×wall）⇒ 净 %d（%.2e）<= %.0e"
                               % (lo, env_i50, lo_c, r_c, ratio_limit)}
            elif exact and lo_net > ratio_limit:
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
                pv, pw = p999_exact_net(gt, n, p999_limit_us, env_p999)
                if env_p999:
                    out["env_p999"] = env_p999
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
            v_lo, w_lo, s_lo = p999_any(counts, n, b_lo, gt, fine)
            v_hi, w_hi, s_hi = p999_any(counts, n, b_hi, gt, fine)
            v_abs, w_abs, s_abs = p999_any(counts, n, P999_SEG_ABS_US, gt, fine)
            out["p999_source"] = {"lo": s_lo, "hi": s_hi, "abs": s_abs}
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
                    "why": "p99.9 <= B_seg_lo=%.1fus 且 <= %.0fus 兜底（口径 %s/%s；%s）"
                           % (b_lo, P999_SEG_CAP_US, s_lo, s_abs, w_lo)}
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
    out["criteria"]["slice_le5"] = {"verdict": "N/A", "structural": True,
        "why": "混合形状（每轮 64 个 16 B leaf + 1 个 8 MiB big；插桩不计 n）⇒ 切片 <=5us 不可判"
               "（Lead m03564）；N/A ≠ PASS"}
    # 已裁决的**结构性不可判**（不是缺证据/样本不足）：不进用例聚合，但仍逐条打印。
    out["structural_na"] = True
    out["verdict"] = "N/A"
    out["note"] = "混合通道：>4us=%d >8us=%d（n=%d）；(a) 按 m03564 记 N/A，大分配看 (b)" % (
        gt4, gt8, n)
    return out


def verdict_cold_dense(n, counts, max_us, tops, p_noise, step_total_us=None, pops_tot=None):
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
    # 聚合口径（Lead R7）：tau_agg = 总步时 / 总 pop。判据用它；[top] 最慢步的 tau_t 降为 OBS。
    # 理由：本机空转微基准实测 max 88-236us 的注入停顿，[top] 采样一旦被 steal 打中，tau_t 就是
    # 200-600us/32pop 的假值；n 达 3e7 步的聚合值把噪声摊到 1/n。上限 PER_POP_US=2.0us 一个字没放宽，
    # 另外新增 clock-free 的 pop/push/栈高水位上界（verdict_workbound）。缺数据时退回旧口径（1 rep）。
    if step_total_us is not None and pops_tot:
        tau_agg = float(step_total_us) / float(pops_tot)
        out["tau_agg_us"] = tau_agg
        out["pops_tot"] = pops_tot
        out["criteria"]["tau_agg"] = {
            "verdict": "PASS" if tau_agg <= PER_POP_US else "RED",
            "why": "tau_agg=总步时/总pop=%.4fus %s %.1fus（去噪聚合口径；单步被宿主 steal 打中不改变它）"
                   % (tau_agg, "<=" if tau_agg <= PER_POP_US else ">", PER_POP_US)}
        if "tau_t" in out["criteria"]:
            out["criteria"]["tau_t"]["verdict"] = "OBS"
            out["criteria"]["tau_t"]["why"] = ("OBS（[top] 最慢步采样，受宿主 steal 支配，不作判据）："
                                              + (out["criteria"]["tau_t"].get("why") or ""))
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


def load_stallrates(path):
    """解析外部停顿率探针（noise_floor --rate 的输出；run.sh 的 stallfloor 加了 case=/rep=/when= 前缀）。
    返回 {(case, rep): {"pre": (rate10, rate50), "post": (rate10, rate50)}}；文件缺失 => {}。"""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if "stallfloor " not in line or "rate10_per_s=" not in line:
                continue
            t = line.split("stallfloor ", 1)[1]
            d = {}
            for tok in t.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    d[k] = v
            try:
                r10 = float(d.get("rate10_per_s", "nan"))
                r50 = float(d.get("rate50_per_s", "nan"))
            except ValueError:
                continue
            if r10 != r10:  # nan
                continue
            case = d.get("case")
            try:
                rep = int(d.get("rep", "0"))
            except ValueError:
                continue
            when = d.get("when", "pre")
            out.setdefault((case, rep), {})[when] = (r10, r50)
    return out


def load_runs_wall(path):
    """runs.jsonl -> {(case, rep): wall_s}。同一 (case, rep) 有多条记录时优先 tag==pause 的那条。"""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("wall_s") is None:
                continue
            try:
                key = (r.get("case"), int(r.get("rep")))
            except (TypeError, ValueError):
                continue
            if key in out and r.get("tag") != "pause":
                continue
            out[key] = float(r["wall_s"])
    return out


def env_floor(stall, walls, case):
    """本 case 的外部停顿地板次数。每个 rep：min(pre 速率, post 速率) × 该 rep 墙钟秒数，求和。
    返回 (n10, n50, note)；缺探针或缺墙钟 => 对应项不扣（返回 0，退回原始口径）。"""
    n10 = n50 = 0.0
    parts = []
    reps = sorted({rep for (c, rep) in stall if c == case})
    for rep in reps:
        d = stall.get((case, rep)) or {}
        rates = [v for v in (d.get("pre"), d.get("post")) if v]
        wall = walls.get((case, rep))
        if not rates or wall is None or wall <= 0:
            parts.append("r%d 缺数据(不扣)" % rep)
            continue
        r10 = min(v[0] for v in rates)
        r50 = min(v[1] for v in rates)
        n10 += r10 * wall
        n50 += r50 * wall
        parts.append("r%d %.1f/s×%.2fs=%.0f" % (rep, r10, wall, r10 * wall))
    note = "；".join(parts)
    return n10, n50, note

def load_relsites(path):
    """读 run.sh 写的 relsite.txt：每用例生成 C 里 extc_rc_storeRefOwnedRelease 的调用点数。

    调用点 0 处 ⇒ (c) release 通道在该形状**结构性不可达**（n==0 是必然而非采样不足），
    g1c 据此记 vacuous PASS；缺证据（没有该文件）时保持 N/A —— 不许无证据 vacuous。"""
    out = {}
    if not path or not os.path.exists(path):
        return out
    for ln in open(path, encoding="utf-8", errors="replace"):
        d = {}
        for tok in ln.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                d[k] = v
        if "case" in d and "sites" in d:
            try:
                out[d["case"]] = int(d["sites"])
            except ValueError:
                pass
    return out


def verdict_workbound(wb, required=False):
    """clock-free 每步工作量上界（Lead R7）：只读 [wbudget] 的高水位，不读钟。
    pop_max <= EXTC_RC_MARK_STEP_POP_MAX(512)、
    push_max <= EXTC_RC_MARK_PUSH_STEP(2048) + BATCH_POP(200)*FANOUT(32) = 8448、
    mscap_max <= 2*EXTC_RC_MARK_STACK_SOFT = 16384。
    这三个量决定「一步最多做多少工作」，是本轮把毫秒尾堵在机制层的正面判据；
    墙钟 max 在本宿主上不可判（空转微基准 max 88-236us），只留 OBS。"""
    out = {"name": "work-bound(clock-free)", "criteria": {}, "required": required,
           "pop_limit": POP_MAX_LIMIT, "push_limit": PUSH_MAX_LIMIT,
           "mscap_limit": MSCAP_LIMIT}
    if not wb or "pop_max" not in wb:
        out["verdict"] = "N/A"
        out["note"] = ("缺 [wbudget] pop_max 字段（旧插桩）：工作量上界无法校验"
                       + ("；--require-workbound 要求可判（不记 N/A）" if required else ""))
        return out
    for key, lim, why in (("pop_max", POP_MAX_LIMIT, "EXTC_RC_MARK_STEP_POP_MAX"),
                          ("push_max", PUSH_MAX_LIMIT, "PUSH_STEP + BATCH_POP*FANOUT"),
                          ("mscap_max", MSCAP_LIMIT, "2*EXTC_RC_MARK_STACK_SOFT")):
        v = wb.get(key) or 0
        out["criteria"][key] = {"verdict": "PASS" if v <= lim else "RED",
                                "why": "%s=%d %s %d（%s）"
                                       % (key, v, "<=" if v <= lim else ">", lim, why)}
    out["pop_max"], out["push_max"], out["mscap_max"] = wb.get("pop_max"), wb.get("push_max"), wb.get("mscap_max")
    out["pops_tot"], out["reps"] = wb.get("pops_tot"), wb.get("reps")
    vs = [c["verdict"] for c in out["criteria"].values()]
    out["verdict"] = "RED" if "RED" in vs else "PASS"
    return out


def analyze(hist, seg, chan, noise, case=None, cold_dense=COLD_DENSE_DEFAULT,
            seg_bound=None, seg_bound_note=None, tops=None, mixed_shapes=MIXED_DEFAULT,
            work_maxreq=None, scan_chunk=None,
            env_p999=0.0, env_p999_note="", env_p50=0.0, rel_sites=None, chan_only=None,
            wb=None, require_workbound=False):
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
           "env_p999": env_p999, "env_p999_note": env_p999_note,
           "env_p50": env_p50,
           "seg_bound_us": seg_bound, "seg_bound_note": seg_bound_note,
           "per_pop_limit_us": PER_POP_US, "p999_cold_us": P999_COLD_US}
    res["workbound"] = verdict_workbound(wb, require_workbound)
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
                        res["a"] = verdict_cold_dense(n_reg, diff, hist.get("max_us"), tops, p_noise,
                                                      step_total_us=hist.get("total_us"),
                                                      pops_tot=(wb or {}).get("pops_tot"))
                    elif cls == "mixed":
                        # Lead m03564：混合形状的 (a) 不可判（插桩不按 n 归因）⇒ N/A + 可判定界
                        res["a"] = verdict_mixed(n_reg, diff, hist.get("max_us"), tops)
                    else:
                        res["a"] = verdict_step_channel(
                            "regular(seg=0)", n_reg, diff, None, p_noise,
                            mode="warm", p999_limit_us=P999_REG_US, gt=g_reg,
                            env_p999=env_p999, env_p50=env_p50)
                    res["b"] = verdict_step_channel(
                        "resume(seg>0)", n_seg, seg["counts"], seg.get("max_us"), p_noise,
                        mode="seg", p999_limit_us=seg_bound, gt=g_seg, fine=seg.get("fine"))
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
    # (b) 的**结构性 vacuous**（Lead R3）：该形状在结构上不可能出现 seg>0 步 —— 生成物把长对象按
    # EXTC_RC_SCAN_CHUNK 个指针字切段（reclaim.c：words > SCAN_CHUNK 时才置 scanObj），所以只要
    # 本 run 的 [work] maxreq <= SCAN_CHUNK*sizeof(void*) 且观测 n_seg==0，段通道必然永不触发；
    # 这是「结构上不可达」，不是「样本不足掩盖」。maxreq 未知 / 超阈 / n_seg>0 时一律不 vacuous（保持 N/A）。
    _bv = res.get("b", {})
    if (_bv.get("verdict") == "N/A" and work_maxreq is not None and scan_chunk
            and (res.get("n_seg") or 0) == 0):
        _thr = int(scan_chunk) * 8
        if work_maxreq <= _thr:
            res["b"] = {"name": "resume(seg>0)", "verdict": "PASS", "vacuous": True,
                        "note": ("vacuously PASS（结构性不可达，非样本不足）：本 run 最大分配请求 maxreq=%d B "
                                 "<= EXTC_RC_SCAN_CHUNK*8=%d B ⇒ 标记时没有对象会被切段，n_seg==0 是必然而非"
                                 "采样不足；(b) 的 B_seg 判据在该形状上无定义" % (work_maxreq, _thr))}
    # trigger-step = v17 起的名字（v16 及更早叫 collect；两个都收，旧产物仍可判）
    for name in ("regionfree", "release", "trigger-step", "collect", "account"):
        if name in chan:
            ch = verdict_chan(name, chan[name])
            # (c) release 的**结构性 vacuous**（Lead R3）：发射器要求「循环体内把本帧新分配、
            # 已写进某个堆对象引用字段、从未重绑的局部量写进本帧根表的长命槽」四条判据同时成立，
            # 多数负载根本不发射调用点（cyc/pb/pc/pd_bigobj 与 digraph 的生成 C 均为 0 处）。
            # 调用点 0 处 ⇒ 该通道在本形状不可能触发，n==0 是必然，不是「样本不足」。
            # 有调用点却 n==0 时不 vacuous（保持 N/A，由用例判定或下一轮修）。
            if (name == "release" and rel_sites == 0 and not (ch.get("n") or 0)):
                ch = {"name": "release", "verdict": "PASS", "vacuous": True, "n": 0,
                      "note": ("vacuously PASS（结构性不可达，非样本不足）：生成 C 中 "
                               "extc_rc_storeRefOwnedRelease 调用点 0 处（relsite 证据）⇒ 该通道在"
                               "本形状不可能触发；机制本身由 tests/gc-p0/src/rel_overwrite.extc 探针实测")}
            res["channels"][name] = ch
    if chan_only:
        # 通道探针用例（Lead R3）：只判列出的 (c) 通道；步分布不由它判（四个 warm 形状覆盖）。
        # 与 N/A 的区别是**不留未验证项**：列出的通道必须真有样本（n>0 或结构性 vacuous），
        # 否则判 RED（探针没打到机制），而不是记 N/A。
        res["a"] = {"name": "regular(seg=0)", "verdict": "SKIP",
                    "note": "channel-probe 用例：步分布不由本用例判（四个 warm 形状覆盖）"}
        res["b"] = {"name": "resume(seg>0)", "verdict": "SKIP",
                    "note": "channel-probe 用例：步分布不由本用例判（四个 warm 形状覆盖）"}
        vs = []
        for nm in chan_only:
            d = res["channels"].get(nm)
            if d is None:
                res["channels"][nm] = {"name": nm, "n": 0, "verdict": "RED",
                                       "note": "探针要求该通道有样本，但日志里没有 [chan] 行（未触发且无证据）"}
                vs.append("RED")
            elif not (d.get("n") or 0):
                d["verdict"] = "RED"
                d["note"] = ("探针要求该通道 n>0，实测 n=0（通道从未触发）"
                             + ("；" + d["note"] if d.get("note") else ""))
                vs.append("RED")
            else:
                vs.append(d["verdict"])
    else:
        # 结构性不可判（已裁决，如 mixed 形状的 (a)）不进聚合：它既不是 PASS 也不是缺证据，
        # 逐条打印 + 记在 res["structural_na"] 里；缺证据的 N/A（无 [histo]/未标定/n<1e5）仍会让用例 N/A。
        _a = res.get("a", {})
        vs = []
        if _a.get("structural_na") and _a.get("verdict") == "N/A":
            res["structural_na"] = labels = res.get("structural_na") or []
            labels.append("a")
        else:
            vs.append(_a.get("verdict"))
        vs.append(res.get("b", {}).get("verdict"))
        vs += [c["verdict"] for c in res["channels"].values()]
    # 工作量界（clock-free，Lead R7）进用例聚合：RED 一律计入；PASS 计入（不透支）；
    # N/A（旧插桩缺 [wbudget]）只在 --require-workbound 时计入，避免 selftest 的合成用例被打成 N/A。
    _wbv = (res.get("workbound") or {}).get("verdict")
    if _wbv == "RED":
        vs.append("RED")
    elif _wbv == "PASS":
        vs.append("PASS")
    elif require_workbound:
        vs.append("N/A")
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
    if d.get("verdict") in ("N/A", "ERROR", "SKIP") and "over_lo_ratio" not in d:
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
    if res.get("env_p999"):
        print("  环境地板扣减（仅 p99.9）：count(>%.0fus) 扣 %.0f 次（%s）"
              % (P999_REG_US, res["env_p999"], res.get("env_p999_note") or ""))
    if res.get("env_p50"):
        print("  环境地板扣减（>%.0fus 通道）：count 扣 %d 次（min(pre,post)×wall，判据不改）"
              % (OVER50_US, res["env_p50"]))
    if res.get("structural_na"):
        print("  结构性 N/A（已裁决、不进用例聚合，逐条仍打印）：%s"
              % "/".join(res["structural_na"]))
    print("  判据：warm 判 >50us 占比 <=%.0e（扣噪声）+ p99.9<=%.0fus；"
          "cold-dense 判 tau_agg(总步时/总pop)<=%.1fus + p99.9<=%.0fus + over1ms=0（不判 5us 门禁）；"
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
    _wb = res.get("workbound") or {}
    if _wb:
        _wbtxt = "；".join(c.get("why", "") for c in (_wb.get("criteria") or {}).values())
        print("  工作量界（clock-free，Lead R7）：**%s**（%s）"
              % (_wb.get("verdict"), _wbtxt or _wb.get("note") or ""))
    print("  | 通道 | n | >50us 下界/上界 | p99.9 | max_us | 判定 |")
    print("  |---|---|---|---|---|---|")
    print(_row("(a) 常规 seg=0", res.get("a", {})))
    print(_row("(b) 续扫 seg>0", res.get("b", {})))
    if (res.get("b") or {}).get("vacuous"):
        print("       %s" % res["b"].get("note", ""))
    for name, d in res["channels"].items():
        if not d["n"]:
            print("  | (c) %s | 0 | - | - | - | **%s**（%s） |"
                  % (name, d["verdict"], d.get("note") or "该通道未触发"))
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
        if a.get("tau_agg_us") is not None:
            print("    聚合口径（判据）：tau_agg=总步时/总pop=%.4fus（pops_tot=%d）"
                  % (a["tau_agg_us"], a.get("pops_tot") or 0))
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

    # S28（Lead R3）：(b) 的**结构性 vacuous** —— [work] maxreq <= SCAN_CHUNK*8 且 n_seg==0
    # => PASS(vacuous)；maxreq 超阈 / 未知 / n_seg>0 => 一律仍 N/A（不许 vacuous 掩盖未触发通道）
    ch28 = _zero(); ch28[0] = 200000
    h28 = {"n": 200000, "counts": ch28, "max_us": 4.0}
    s28z = {"n": 0, "counts": zero, "max_us": 0.0}
    r = analyze(h28, s28z, {}, noise, seg_bound=None, work_maxreq=40, scan_chunk=8192)
    ck(r["b"]["verdict"] == "PASS" and r["b"].get("vacuous"),
       "S28 maxreq=40<=64KiB 且 n_seg=0 => (b) 应 vacuous PASS，实得 %r" % (r["b"].get("verdict"),))
    r = analyze(h28, s28z, {}, noise, seg_bound=None, work_maxreq=8388608, scan_chunk=8192)
    ck(r["b"]["verdict"] == "N/A",
       "S28 maxreq=8MiB 超阈时 (b) 必须仍 N/A（不得 vacuous 掩盖），实得 %r" % (r["b"].get("verdict"),))
    r = analyze(h28, s28z, {}, noise, seg_bound=None)
    ck(r["b"]["verdict"] == "N/A",
       "S28 maxreq 未知时 (b) 必须仍 N/A，实得 %r" % (r["b"].get("verdict"),))

    # S29（Lead R3）：逐微秒细直方图的解析 + 合并 + (b) 的夹逼解算（粗桶在 B_seg 处跨桶不可判的老问题）
    f29 = parse_segsteps("[segsteps] n=5000 pops_tot=5000 max_us=64.0 b1us=0 gt10us=0 gt50us=102 gt64us=0 us49=10 us50=102 us64=3")["fine"]
    ck(f29 == {49: 10, 50: 102, 64: 3}, "S29 fine 解析失败：%r" % (f29,))
    ck(fine_over_bounds(f29, 49.07) == (105, 115), "S29 fine_over_bounds(49.07) 应为 (105,115)（>=50us 桶全算，外加 49 桶），实得 %r" % (fine_over_bounds(f29, 49.07),))
    ck(fine_over_bounds(f29, 50.0) == (3, 105), "S29 整数阈值口径：下界只算 >50us 的桶(us64=3)、上界含 50 桶，实得 %r" % (fine_over_bounds(f29, 50.0),))
    v, w, src = p999_any([], 228518, 49.07, None, {49: 10, 50: 102})
    ck(v == "PASS" and src == "fine", "S29 count(>49.07)<=112 => 应 fine PASS，实得 %r/%s" % (v, src))
    ck(p999_any([], 228518, 49.07, None, {49: 10, 50: 300})[0] == "RED", "S29 count(>49.07)>=300 > 228.5 => 应 fine RED")
    ck(p999_any([], 228518, 49.07, None, None)[2] == "bucket", "S29 无细直方图应退回桶界口径")
    a29 = {"n": 100, "counts": [0] * pp.HISTO_NB, "max_us": 1.0, "fine": {50: 3}}
    b29 = {"n": 100, "counts": [0] * pp.HISTO_NB, "max_us": 1.0, "fine": {50: 4, 64: 1}}
    ck(_merge(_merge(None, a29), b29)["fine"] == {50: 7, 64: 1}, "S29 _merge 丢/错 fine 桶")

    # S30（Lead R3）：环境地板扣减 —— 宿主抢占的停顿按时间到达，与本步预算无关，一步被打中就记一条慢步，
    # 那是量具误差不是回收工作。判据数字一个不改：净 count(>10us)*1000 <= n；扣 min(pre,post)*wall；缺数据不扣。
    g30 = {10: 500, 50: 10, 64: 5}
    ck(p999_exact_net(g30, 200000, 10.0, 0.0)[0] == "RED",
       "S30 不扣环境地板：500*1000 > 200000 => 应 RED，实得 %r" % (p999_exact_net(g30, 200000, 10.0, 0.0),))
    ck(p999_exact_net(g30, 200000, 10.0, 400.0)[0] == "PASS",
       "S30 扣 400 次环境地板 => 净 100*1000 <= 200000 => 应 PASS，实得 %r" % (p999_exact_net(g30, 200000, 10.0, 400.0),))
    ck(p999_exact_net(g30, 200000, 10.0, 900.0)[0] == "PASS",
       "S30 扣减超过计数时净数夹 0（不许变成负的 PASS/RED 混线）")
    ck(p999_exact_net(g30, 200000, 10.0, 100.0)[0] == "RED",
       "S30 只扣 100 次仍 400*1000 > 200000 => RED（扣减不足不能翻盘）")
    st30 = {("cyc", 1): {"pre": (100.0, 10.0), "post": (80.0, 8.0)},
            ("cyc", 2): {"pre": (50.0, 5.0)}}
    wl30 = {("cyc", 1): 0.5}
    n10, n50, note30 = env_floor(st30, wl30, "cyc")
    ck(abs(n10 - 40.0) < 1e-9, "S30 env_floor：取 min(pre,post)=80 × wall 0.5 => 40，实得 %r" % (n10,))
    ck("r2 缺数据" in note30, "S30 缺 wall 的 rep 必须标注且不扣，实得 %r" % (note30,))
    ck(load_stallrates("/nonexistent-xyz") == {}, "S30 缺探针文件 => 空表（不扣减，不是 PASS）")

    # S31（Lead R3）：(c) release 的**结构性 vacuous**（生成 C 调用点 0 处）+ 通道探针用例
    # （--chan-only 只判列出的通道；列出的通道 n==0 判 RED，不许记 N/A）。
    ch31 = {"n": 0, "max_us": 0.0, "counts": [0] * pp.HISTO_NB}
    r = analyze(h28, s28z, {"release": dict(ch31)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, rel_sites=0)
    ck(r["channels"]["release"]["verdict"] == "PASS" and r["channels"]["release"].get("vacuous"),
       "S31 rel_sites=0 且 n=0 => release 应 vacuous PASS，实得 %r" % (r["channels"]["release"].get("verdict"),))
    ck(r["verdict"] == "PASS", "S31 vacuous release 不应把用例拉成 N/A，实得 %r" % (r["verdict"],))
    r = analyze(h28, s28z, {"release": dict(ch31)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, rel_sites=None)
    ck(r["channels"]["release"]["verdict"] == "N/A" and r["verdict"] == "N/A",
       "S31 缺 relsite 证据时 release n=0 仍记 N/A（不许无证据 vacuous）")
    r = analyze(h28, s28z, {"release": dict(ch31)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, rel_sites=3)
    ck(r["channels"]["release"]["verdict"] == "N/A",
       "S31 有调用点（sites=3）而 n=0 时不 vacuous（保持 N/A）")
    ch31b = {"n": 3, "max_us": 8.1, "counts": [0] * pp.HISTO_NB}
    r = analyze(h28, s28z, {"release": dict(ch31b)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, rel_sites=0)
    ck(r["channels"]["release"]["verdict"] == "PASS" and not r["channels"]["release"].get("vacuous"),
       "S31 有实测样本时按正常判（max=8.1us <=1ms => PASS），不被 sites 证据覆盖")
    r = analyze(h28, s28z, {"release": dict(ch31)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, chan_only=("release",))
    ck(r["a"]["verdict"] == "SKIP" and r["b"]["verdict"] == "SKIP",
       "S31 chan_only 用例的 (a)/(b) 记 SKIP（不判，不是 N/A）")
    ck(r["channels"]["release"]["verdict"] == "RED" and r["verdict"] == "RED",
       "S31 chan_only 列出的通道 n=0 => RED（探针没打到机制），实得 %r" % (r["verdict"],))
    r = analyze(h28, s28z, {"release": dict(ch31b)}, noise, seg_bound=None,
                work_maxreq=40, scan_chunk=8192, chan_only=("release",))
    ck(r["verdict"] == "PASS", "S31 chan_only 通道有样本且 max<=1ms => PASS，实得 %r" % (r["verdict"],))
    r = analyze(h28, s28z, {}, noise, seg_bound=None, chan_only=("release",))
    ck(r["verdict"] == "RED", "S31 chan_only 列出的通道连 [chan] 行都没有 => RED（不留 N/A）")
    ck(load_relsites("/nonexistent-xyz") == {}, "S31 缺 relsite 文件 => 空表（不 vacuous）")

    # S32（Lead R3）：>50us 通道也扣环境地板（同样 min(pre,post)*wall；判据 1e-4 不改）
    g32 = {10: 0, 50: 30, 64: 0}
    c32 = [0] * pp.HISTO_NB
    v = verdict_step_channel("regular(seg=0)", 200000, c32, 1.0, 0.0, mode="warm",
                             p999_limit_us=10.0, gt=g32, env_p50=0.0)
    ck(v["criteria"]["over50"]["verdict"] == "RED",
       "S32 不扣环境地板：30/200000=1.5e-4 > 1e-4 => RED，实得 %r" % (v["criteria"]["over50"],))
    v = verdict_step_channel("regular(seg=0)", 200000, c32, 1.0, 0.0, mode="warm",
                             p999_limit_us=10.0, gt=g32, env_p50=20.0)
    ck(v["criteria"]["over50"]["verdict"] == "PASS" and v["env_p50"] == 20,
       "S32 扣 20 次环境地板 => 净 10/200000=5e-5 => PASS，实得 %r" % (v["criteria"]["over50"],))
    v = verdict_step_channel("regular(seg=0)", 200000, c32, 1.0, 0.0, mode="warm",
                             p999_limit_us=10.0, gt=g32, env_p50=5.0)
    ck(v["criteria"]["over50"]["verdict"] == "RED",
       "S32 只扣 5 次 => 净 25/200000=1.25e-4 > 1e-4 => RED（扣减不足不翻盘）")
    v = verdict_step_channel("regular(seg=0)", 200000, c32, 1.0, 0.0, mode="warm",
                             p999_limit_us=10.0, gt=g32, env_p50=500.0)
    ck(v["criteria"]["over50"]["verdict"] == "PASS", "S32 扣减超过计数时净数夹 0（不许变负数）")
    # 噪声带：精确 1.35e-4 落在 (1e-4, ...]，再扣 cpu 噪声 3.5e-5 = 1.0e-4（不 > 限）
    g32b = {10: 0, 50: 27, 64: 0}
    v = verdict_step_channel("regular(seg=0)", 200000, c32, 1.0, 3.5e-05, mode="warm",
                             p999_limit_us=10.0, gt=g32b, env_p50=0.0)
    ck(v["criteria"]["over50"]["verdict"] == "N/A",
       "S32 无地板数据且落噪声带 => 仍 N/A（不影响旧行为），实得 %r" % (v["criteria"]["over50"],))

    # S33（Lead R3）：mixed 形状的 (a) 是**已裁决的结构性 N/A**，不进用例聚合（不像缺证据的 N/A）；
    # 其余通道判完即用例 PASS，并把 "a" 记进 res["structural_na"]。
    r = analyze(h28, s28z, {"release": {"n": 0, "max_us": 0.0, "counts": [0] * pp.HISTO_NB}},
                noise, case="pd_bigobj", seg_bound={"lo": 49.07, "hi": 49.07},
                work_maxreq=40, scan_chunk=8192, rel_sites=0)
    ck(r["class"] == "mixed" and r["a"]["verdict"] == "N/A" and r["a"].get("structural_na"),
       "S33 mixed 的 (a) 应记结构性 N/A，实得 %r" % (r["a"].get("verdict"),))
    ck(r["verdict"] == "PASS" and r.get("structural_na") == ["a"],
       "S33 结构性 N/A 不进聚合 => 用例 PASS 且记 structural_na=[a]，实得 %r/%r"
       % (r["verdict"], r.get("structural_na")))
    ck(r["b"]["verdict"] == "PASS" and r["b"].get("vacuous"), "S33 (b) 仍按 maxreq 结构性 vacuous")
    r2 = analyze(h28, s28z, {}, noise, case="pd_bigobj", seg_bound=None,
                 work_maxreq=None, scan_chunk=8192)
    ck(r2["verdict"] == "N/A" and r2.get("structural_na") == ["a"],
       "S33 (b) 缺证据时仍 N/A（结构性 N/A 不放行缺证据项），实得 %r" % (r2["verdict"],))
    # S34（Lead R7）：clock-free 工作量界 —— [wbudget] 的 pop_max/push_max/mscap_max 三个高水位；
    # 判据不读钟，所以宿主 steal 无法影响它。缺字段 => N/A；--require-workbound 时 N/A 计入聚合（不静默跳过）。
    wb34 = {"pop_max": 512, "push_max": 8448, "mscap_max": 16384, "pops_tot": 1000, "reps": 3}
    v34 = verdict_workbound(wb34)
    ck(v34["verdict"] == "PASS", "S34 三个高水位恰在界内 => PASS，实得 %r" % (v34["verdict"],))
    ck(verdict_workbound(dict(wb34, pop_max=513))["criteria"]["pop_max"]["verdict"] == "RED",
       "S34 pop_max 超 POP_MAX(512) 必须 RED")
    ck(verdict_workbound(dict(wb34, push_max=8449))["criteria"]["push_max"]["verdict"] == "RED",
       "S34 push_max 超 8448（PUSH_STEP+BATCH_POP*FANOUT）必须 RED")
    ck(verdict_workbound(dict(wb34, mscap_max=16385))["criteria"]["mscap_max"]["verdict"] == "RED",
       "S34 mscap_max 超 2*STACK_SOFT 必须 RED")
    ck(verdict_workbound(None)["verdict"] == "N/A", "S34 缺 [wbudget] => N/A（不是 PASS）")
    ch34 = _zero(); ch34[0] = 200000
    h34 = {"n": 200000, "counts": ch34, "max_us": 4.0, "total_us": 400.0}
    s34z = {"n": 0, "counts": zero, "max_us": 0.0}
    r34 = analyze(h34, s34z, {}, noise, wb=wb34)
    ck(r34["workbound"]["verdict"] == "PASS", "S34 wb 在界内 => workbound PASS")
    r34b = analyze(h34, s34z, {}, noise, wb=dict(wb34, pop_max=600))
    ck(r34b["verdict"] == "RED" and r34b["workbound"]["verdict"] == "RED",
       "S34 workbound RED 必须把用例判成 RED，实得 %r" % (r34b["verdict"],))
    r34c = analyze(h34, s34z, {}, noise, wb=None)
    ck(r34c["workbound"]["verdict"] == "N/A", "S34 无 wb => workbound N/A")
    r34d = analyze(h34, s34z, {}, noise, wb=None, require_workbound=True)
    ck(r34d["verdict"] == "N/A",
       "S34 --require-workbound 下缺 wb 必须记 N/A（不静默跳过），实得 %r" % (r34d["verdict"],))

    # S35（Lead R7）：cold-dense 的 tau 判据改成**聚合口径**（总步时/总 pop），[top] 最慢步降为 OBS。
    # 这正是 pd_pointer_dense 的现状：宿主 steal 打中一步 => tau_t=5.756us/pop(假 RED)，而
    # 总步时/总 pop = 4.6ns/pop。判据上限 PER_POP_US 未动。
    ch35 = _zero(); ch35[0] = 2000000
    h35 = {"n": 2000000, "counts": ch35, "max_us": 180.0, "total_us": 1200.0}
    s35z = {"n": 0, "counts": zero, "max_us": 0.0}
    tops35 = [{"kind": "mark", "why": "advance", "cpu_us": 184.2, "pops": 32, "seg": 0},
              {"kind": "mark", "why": "advance", "cpu_us": 160.0, "pops": 32, "seg": 0}]
    r35 = analyze(h35, s35z, {}, noise, case="pd_pointer_dense", tops=tops35,
                  wb={"pop_max": 512, "push_max": 0, "mscap_max": 16384, "pops_tot": 260000000, "reps": 1})
    a35 = r35["a"]
    ck(a35.get("tau_agg_us") is not None and a35["tau_agg_us"] < 1e-3,
       "S35 聚合 tau 应 = 1200us/2.6e8 pop ≈ 4.6ns，实得 %r" % (a35.get("tau_agg_us"),))
    ck(a35["criteria"]["tau_agg"]["verdict"] == "PASS", "S35 聚合 tau 在 2us 内 => PASS")
    ck(a35["criteria"]["tau_t"]["verdict"] == "OBS",
       "S35 [top] 最慢步 tau_t 必须降为 OBS（不作判据），实得 %r" % (a35["criteria"]["tau_t"]["verdict"],))
    ck(a35["per_pop_us"] and a35["per_pop_us"] > 5.0, "S35 OBS 仍应报出 5.75us/pop 的假 tau")
    # 2.6e8 pop × 2.3us = 5.98e8 us 的总步时 => 聚合 tau > 2us/pop，必须 RED
    r35b = analyze(dict(h35, total_us=5.98e8), s35z, {}, noise, case="pd_pointer_dense",
                   tops=tops35, wb={"pop_max": 512, "pops_tot": 260000000})
    ck(r35b["a"]["criteria"]["tau_agg"]["verdict"] == "RED",
       "S35 聚合 tau 超 2us/pop 必须 RED（判据没有被取消）")

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
    print("  S28 (b) 结构性 vacuous（maxreq<=SCAN_CHUNK*8 且 n_seg=0 => PASS）vs 超阈/未知 => N/A")
    print("  S29 [segsteps] us<n>= 细直方图解析/合并 + (b) 的 fine 夹逼解算")
    print("  S30 环境地板扣减（仅 p99.9；扣 min(pre,post)*wall；缺数据不扣、负数夹 0、扣减不足不翻盘）")
    print("  S31 (c) release 结构性 vacuous（sites=0 才 vacuous）+ --chan-only 探针用例（n==0 判 RED，不留 N/A）")
    print("  S32 >50us 通道同样扣环境地板（min(pre,post)*wall；判据 1e-4 不改，扣减不足不翻盘）")
    print("  S33 mixed 的 (a) 结构性 N/A 不进用例聚合（逐条仍打印，记 structural_na）")
    print("  S34 clock-free 工作量界（pop_max<=512 / push_max<=8448 / mscap_max<=16384；缺 wb=>N/A）")
    print("  S35 cold-dense 判聚合 tau_agg=总步时/总pop（[top] 最慢步 tau_t 降为 OBS；判据上限 2us 不变）")
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
    ap.add_argument("--stallrate", default=None,
                    help="外部停顿率探针（noise_floor --rate；run.sh 的 stallfloor 写入 $OUT/stallrate.txt）；"
                         "只有 p99.9 通道用它做环境地板扣减；缺文件 => 不扣（退回原始口径）")
    ap.add_argument("--runs", default=None,
                    help="runs.jsonl（sample.py 记录，含 wall_s）；与环境地板速率相乘得到扣减次数")
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
    ap.add_argument("--relsite", default=None,
                    help="run.sh 写的 relsite.txt（每用例生成 C 里 extc_rc_storeRefOwnedRelease 调用点数；"
                         "0 处 ⇒ (c) release 结构性 vacuous，不记 N/A；缺文件则不 vacuous）")
    ap.add_argument("--chan-only", action="append", default=[],
                    help="CASE:chan[,chan...] —— 通道探针用例只判这些 (c) 通道，步分布记 SKIP（不是放宽："
                         "列出的通道 n==0 判 RED）")
    ap.add_argument("--require-workbound", action="store_true",
                    help="要求 [wbudget] 工作量界可判（门禁用；缺字段记 N/A 而不是静默跳过）")
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
    stall = load_stallrates(a.stallrate)
    walls = load_runs_wall(a.runs)
    cold = tuple(a.cold_dense) if a.cold_dense else COLD_DENSE_DEFAULT
    mixed = tuple(a.mixed) if a.mixed else MIXED_DEFAULT
    cseg = parse_cseg(a.c_seg)
    groups = group_paths(paths)
    relsites = load_relsites(a.relsite)
    chan_only = {}
    for spec in a.chan_only:
        if ":" in spec:
            cname, chans = spec.split(":", 1)
            chan_only[cname] = tuple(x for x in chans.split(",") if x)
    per_case = {}
    for case in sorted(groups):
        hist, seg, chan, tops, nfiles, work_maxreq, wb = load_errs(groups[case])
        env10, env50, envnote = env_floor(stall, walls, case)
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
                      seg_bound=bound, seg_bound_note=bnote, tops=tops, mixed_shapes=mixed,
                      work_maxreq=work_maxreq, scan_chunk=chunk,
                      env_p999=env10, env_p999_note=envnote, env_p50=env50,
                      rel_sites=relsites.get(case), chan_only=chan_only.get(case),
                      wb=wb, require_workbound=a.require_workbound)
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
