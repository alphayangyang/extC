#!/usr/bin/env python3
"""tests/gc-p0/g18.py --- G18 的内存判据 + 分通道停顿列（Lead m03278 / m03358 / m03449）。

G18 形状：tests/gc-p0/src/pd_bigobj.extc，每轮 new 一个 8 MiB 的 big 后丢弃，活集恒 1 个 big。
本脚本不自己跑程序，只把 run.sh g18 段已采集的产物变成可审计的表：
  1) 内存：timemax 的 ru_maxrss（/usr/bin/time -f %M，单位 kB）；
  2) 停顿：分两个通道报告（Lead m03449）：
       ① 常规分配通道（n <= 64 KiB）：新增回收切片必须 <= 5 us（与 G1 同口径）；
       ② 大分配通道（n > 64 KiB）：p99.9（n>=1e3 时）+ over1ms = 0，
          并给 P_step 闭式对照（见下）。
  3) 差分行：before -> after 的 Δslices（step_n）与 ΔGCus（step_total_us）。

口径（Lead m03358 内存 + m03449 停顿）：
  - live 不写死：从 [work] 的 M_max 反读（M_max = 3*live => live = M_max/3）；
    读不到时用 --live-kb 或 G18_LIVE_MIB=8 兜底。
  - 内存主判据（硬）：peak200 <= 3*live + 2*SLAB_SIZE（SLAB=1 MiB）
                     且 peak200 <= peak60 * 1.05（规模无关）；
                     G18_BOUND_MIB 可覆盖上界（标注"经验值 = 3L+2MiB"）。
                     RSS 来自 timemax 进程（ru_maxrss）；stdout/exit 来自 run_pause 进程 —— 两进程两口径。
  - P_step 闭式（Lead m03449，源码 src/back/reclaim.c:977-997 / 1008-1012）：
        W_s = 8*pm + min(n>>6, EXTC_RC_QUOTA_MAX_WORDS=8192)     words（清扫切片）
        W_m = min(n>>3, EXTC_RC_QUOTA_MAX_MARK=1<<20)            work units（mark 配额）
        b   = 256*pm                                             每步 advance 的 pop 上限
        seg <= EXTC_RC_SCAN_CHUNK=8192 words（scanObj 挂起时付一整段）
        pm 取硬上限 16（源码 "8 x 16 sweep words and 256 x 16 mark objects is the hard ceiling"）；
        c_word = 9.5~12 ns/word，c_unit = 17.5 ns（标定值）。
        => 8 MiB 大分配：W_s=8320、W_m=524288（被 256*pm=4096 的每步 b 限制）、seg=8192
           => 闭式 0.23~0.27 ms（Lead m03449 参考值 0.22~0.26 ms，差异见正文"存疑"）。
  - 追加观察用例 pd_pointer_dense（Lead m03381/m03449）：8 MiB 里 1M 个非空指针 x 200 轮；
        只报 peak/[histo] 尾部/over1ms/[top]，over1ms>0 记 RED（"P 的停顿通道对大指针稠密对象
        不成立，需要按 n 分档的准入"），不参与 G18 判定。

用法：
  python3 tests/gc-p0/g18.py json --out tests/gc-p0/out/p0-after --tag after
  python3 tests/gc-p0/g18.py compare tests/gc-p0/out/p0-before/g18.json tests/gc-p0/out/p0-after/g18.json
  python3 tests/gc-p0/g18.py --selftest
"""
import argparse, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pause_parse as pp

MIB = 1024                      # kB
SLAB_MIB = 1.0                  # EXTC_RC_SLAB_SIZE = 1<<20
EMP_BOUND_MIB = 26.0            # 经验值 = 3L + 2 MiB（L=8 MiB）
CHUNK_WORDS = 8192              # EXTC_RC_SCAN_CHUNK 默认
TOP_N = 3

# P_step 闭式常数（Lead m03564 新语义 + m03746 修正；src/back/reclaim.c:1000-1017, 1348-1382）
PM_CAP = 16                     # 压力乘子硬上限（默认 4；闭式取硬上限）
C_WORD_NS = (9.5, 12.0)         # 每个扫描/清扫 word（实测 9.5~12 ns）
C_UNIT_NS = 17.5                # 每个 mark work unit（pop / bitmap word）
# Lead m03746：原 C_A_NS=(0,18000ns) 是为把闭式凑进 0.22~0.26 ms 的**拟合残差**，不是测量值，
# 不可伪证 ⇒ 置 0；若将来要报，必须拆成具名项（首个 advance / 描述符字段扫描）并单独标定。
C_A_NS = (0.0, 0.0)
QUOTA_MAX_WORDS = 8192          # EXTC_RC_QUOTA_MAX_WORDS：硬上限；合法 slab 上**不可达**（见下）
# Lead m03746 的紧上界：slotSize 下限 16 B ⇒ 合法 slab 的 nslots < 4096 ⇒ ceil(nslots/64) <= 64。
# ⇒ W_s = 8*pm + 64 <= 192 words（默认 pm=4 时 96 words），远小于 cap 8192。
# 存疑：当前源码 src/back/reclaim.c:1005 的注释写「合法 slab nslots<=131072 ⇒ ceil/64<=2048」，
# 与 Lead 的 4096 假设不符；两档都写在 W_s_bounds 里，主判据用 Lead 的 64，等 runtime-facts 确认。
Q_WORDS_TIGHT = 64              # Lead m03746
Q_WORDS_SRC_COMMENT = 2048      # src/back/reclaim.c:1005 旧注释推出的上界：过松旧估计（Lead m03891），仅附注用，不进主判据
QUOTA_MAX_MARK = 1 << 20        # EXTC_RC_QUOTA_MAX_MARK：mark 配额字节额度硬上限
W_M_BATCH = 8192                # Lead m03564：deadline 每 8192 work unit 检查一次 ⇒ 单批 W_m=8192
QUOTA_DEADLINE_US_DEFAULT = 800 # EXTC_RC_QUOTA_DEADLINE_US 源码默认（Lead m03746）；实际值从 [quota] 读
BIG_BYTES = 8 << 20             # G18 的 big payload = 8 MiB
BIG_THRESHOLD = 64 << 10        # 通道阈值：n > 64 KiB 才算大分配通道


def closed_form(n_bytes=BIG_BYTES, pm=PM_CAP, nslots=None, deadline_us=None):
    """P_step 闭式（Lead m03564 相加式 + m03746 修正）。

        P_step <= t_sweep(W_s) + c_u * W_m + c_a      （c_a 置 0：拟合残差不是测量）
        W_s = 8*pm + max(1, min(ceil(cursor_slab->nslots/64), 8192))  swept words
              紧上界 W_s <= 8*16 + 64 = 192 words（Lead m03746；cap 8192 在合法 slab 上不可达）
        W_m = 8192 work units（deadline 每 8192 units 检查一次）
        物理天花板 = deadline_us + 2*batch_us（DL6：首个 advance 在时钟起点之前，再加触发 deadline
              的 in-flight advance ⇒ 保守多算一段；源码 src/back/reclaim.c:1012-1015 注释同义）

    nslots = 当前扫掠游标 slab 的槽数（[top]/[quota] 都不打印，需 /tmp 插桩实测）。
    nslots=None ⇒ 用 Lead 的紧上界 64（仍标注未实测），同时给 W_s_bounds 供存疑档参考。
    deadline_us = [quota] deadline_us= 的实测值；None ⇒ 源码默认 800（Lead m03746）。
    """
    if nslots:
        q = max(1, min((int(nslots) + 63) // 64, Q_WORDS_TIGHT))
        wsrc = ("游标 slab nslots=%d ⇒ ceil(nslots/64)=%d words（紧上界 %d）"
                % (int(nslots), q, Q_WORDS_TIGHT))
    else:
        q = Q_WORDS_TIGHT
        wsrc = ("nslots 未实测 ⇒ 用 Lead m03746 紧上界 ceil(nslots/64)<=%d words"
                "（cap %d 在合法 slab 上不可达）" % (Q_WORDS_TIGHT, QUOTA_MAX_WORDS))
    W_s = 8 * pm + q
    W_m = W_M_BATCH
    t_sweep_lo = C_WORD_NS[0] * W_s / 1000.0
    t_sweep_hi = C_WORD_NS[1] * W_s / 1000.0
    mark_us = C_UNIT_NS * W_m / 1000.0
    lo = t_sweep_lo + mark_us + C_A_NS[0] / 1000.0
    hi = t_sweep_hi + mark_us + C_A_NS[1] / 1000.0
    batch_us = C_UNIT_NS * W_M_BATCH / 1000.0
    dl = QUOTA_DEADLINE_US_DEFAULT if deadline_us is None else int(deadline_us)
    ceiling = dl + 2.0 * batch_us
    alt_ws = 8 * pm + Q_WORDS_SRC_COMMENT   # 括号附注档：源码 :1005 旧注释（nslots<=131072）已被 Lead m03891 判为过松
    # 主判据只用 q<=64（chunk/slotSize>=SLOTS_MIN=1024、slotSize>=16B => nslots<4096 => ceil/64<=64）
    return {"n_bytes": n_bytes, "pm": pm, "nslots": (int(nslots) if nslots else None),
            "W_s_fixed": 8 * pm, "W_s_q": q, "W_s": W_s, "W_s_source": wsrc,
            "W_s_bounds": {"tight_lead_m03746": 8 * pm + Q_WORDS_TIGHT,
                           "src_comment_1005": alt_ws,
                           "cap_8192_unreachable": 8 * pm + QUOTA_MAX_WORDS},
            "W_s_alt_us": (round(C_WORD_NS[0] * alt_ws / 1000.0, 1),
                           round(C_WORD_NS[1] * alt_ws / 1000.0, 1)),
            "W_m": W_m, "W_m_note": "deadline 每 8192 units 检查一次；源码配额上限 min(n>>3,1<<20)",
            "b": 256 * pm, "seg": CHUNK_WORDS,
            "t_sweep_lo_us": round(t_sweep_lo, 2), "t_sweep_hi_us": round(t_sweep_hi, 2),
            "mark_us": round(mark_us, 2), "lo_us": round(lo, 1), "hi_us": round(hi, 1),
            "deadline_us": dl, "batch_us": round(batch_us, 1),
            "ceiling_us": round(ceiling, 1),
            "deadline_source": ("[quota] deadline_us=" if deadline_us is not None
                                else "源码默认 %d（未观测到 [quota]）" % QUOTA_DEADLINE_US_DEFAULT)}


def pstep_bound_us():
    cf = closed_form()
    return (cf["lo_us"], cf["hi_us"])


def _read(path):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        return f.read()


def _top_lines(txt):
    out = []
    for ln in txt.splitlines():
        if not ln.startswith("[top]"):
            continue
        d = {}
        for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([-+0-9.A-Za-z_]+)", ln):
            try:
                d[k] = float(v) if "." in v else int(v)
            except ValueError:
                d[k] = v
        out.append({"raw": ln, "kind": d.get("kind"), "why": d.get("why"),
                    "dur_us": d.get("dur_us"), "cpu_us": d.get("cpu_us"),
                    "seg": d.get("seg"), "words": d.get("words"), "pops": d.get("pops")})
    return out


def _pstep_lines(txt):
    return [ln for ln in txt.splitlines() if ln.startswith("[pstep]")]


def _runs(out, case=None):
    recs = []
    for ln in _read(os.path.join(out, "runs.jsonl")).splitlines():
        ln = ln.strip()
        if not ln:
            continue
        try:
            r = json.loads(ln)
        except ValueError:
            continue
        if case is None or r.get("case") == case:
            recs.append(r)
    return recs


def _timemax(out, case):
    d = {}
    for ln in _read(os.path.join(out, "timemax.txt")).splitlines():
        p = ln.split()
        if len(p) >= 3 and p[0] == case:
            try:
                d[p[1]] = int(p[2])
            except ValueError:
                pass
    return d


def histo_counts(pr):
    return list((pr.get("histo") or {}).get("counts") or [])


def histo_stats(counts):
    """n / 中位 / p99.9 的桶上界（us）。桶上界取 pause_parse 的定版 edges。"""
    if not counts:
        return {"n": 0}
    n = sum(int(c) for c in counts)
    edges = getattr(pp, "HISTO_EDGES_NS", None)
    names = getattr(pp, "HISTO_NAMES", None)

    def q(qq):
        if n <= 0:
            return None
        target = qq * n
        cum = 0
        for i, c in enumerate(counts):
            cum += int(c)
            if cum >= target:
                if edges and i < len(edges):
                    return round(edges[i] / 1000.0, 3)      # ns -> us
                return None
        return None

    return {"n": n, "median_us": q(0.5), "p999_us": q(0.999),
            "names": names,
            "edges_us": [round(e / 1000.0, 3) for e in edges] if edges else None}


def histo_bounds(counts):
    """分通道要用到的可判定界的计数（15 桶定版）。"""
    if not counts:
        return None
    c = [int(x) for x in counts]
    n = sum(c)
    # idx0 b1us <=1us, idx1 b2us <=2us, idx2 b4us <=4us, idx3 b8us <=8us, idx4 b16us ...
    return {
        "n": n,
        "le4_us": sum(c[:3]),          # 精确：<= 4 us 的步数
        "gt4_us": sum(c[3:]),          # 精确：> 4 us 的步数
        "gt8_us": sum(c[4:]),          # 精确：> 8 us 的步数
        "over256_us": sum(c[9:]) if len(c) > 9 else None,   # > 256 us（b512us 起）
        "over1ms": sum(c[11:]) if len(c) > 11 else None,    # 严格 > 1 ms（b4ms 起）
    }


def ch1_verdict(h):
    """① 常规分配通道：新增回收切片必须 <= 5 us。返回 (verdict, 说明)。"""
    if not h:
        return "n/a", "无 [histo]（未带 PAUSE=1？）"
    if h["gt8_us"] > 0:
        return "RED", "存在 %d/%d 步 > 8 us ⇒ 上界 > 5 us 必然被打破" % (h["gt8_us"], h["n"])
    if h["gt4_us"] == 0:
        return "PASS", "全部 %d 步 <= 4 us ⇒ 必然 <= 5 us" % h["n"]
    return ("N/A", "%d/%d 步落在 4-8 us 桶（跨 5 us），需按 n 分桶插桩才能精确判"
            % (h["gt4_us"], h["n"]))


def ch2_verdict(h, cf):
    """② 大分配通道：over1ms=0 + p99.9（n>=1e3 时）。返回 (verdict, 说明)。"""
    if not h:
        return "n/a", "无 [histo]"
    if h["over1ms"] and h["over1ms"] > 0:
        return "RED", "over1ms=%d > 0（严格 >1 ms 的步）" % h["over1ms"]
    st = histo_stats(h["counts"]) if "counts" in h else {"n": h["n"]}
    if h["n"] < 1000:
        return "N/A", ("over1ms=0；n=%d < 1e3 ⇒ p99.9 不可判；闭式 %.1f~%.1f us"
                       "（含 deadline 层物理天花板 %.1f us）vs max 见上表"
                       % (h["n"], cf["lo_us"], cf["hi_us"], cf["ceiling_us"]))
    p999 = st.get("p999_us")
    if p999 is None:
        return "N/A", "over1ms=0；p99.9 落在尾桶"
    if p999 <= cf["ceiling_us"]:
        return ("PASS", "over1ms=0；p99.9=%.1f us <= deadline 层天花板 %.1f us"
                        "（标称闭式 %.1f~%.1f us）"
                        % (p999, cf["ceiling_us"], cf["lo_us"], cf["hi_us"]))
    return ("RED", "over1ms=0 但 p99.9=%.1f us > deadline 层天花板 %.1f us"
                   % (p999, cf["ceiling_us"]))


def _load_rep(out, case, rep):
    """读一个 rep 的 stderr/stdout 并解析（[histo]/[step]/[work]/[quota]/[top]）。"""
    errp = os.path.join(out, "log", "%s_pause_r%d.err" % (case, rep))
    outp = os.path.join(out, "log", "%s_pause_r%d.out" % (case, rep))
    txt = _read(errp)
    return (pp.parse_text(txt) if txt else {}), txt, _read(outp).strip()


def _pick_runs(runs, case, arg=None):
    hits = [r for r in runs if r.get("case") == case]
    if arg is not None:
        hits = [r for r in hits if (r.get("args") or "").split()[:1] == [str(arg)]]
    # 同一 OUT 目录会被 g18 跑多次：runs.jsonl 只追加、日志按 rep 覆盖 ⇒ 每个 rep 只保留
    # 最后一次记录，否则 n_reps/exits 会把旧跑次（乃至失败的 spawn）也算进去。
    by_rep = {}
    for r in hits:
        by_rep[r.get("rep") or 0] = r
    return [by_rep[k] for k in sorted(by_rep)]


def _agg_reps(out, case, hits, expected):
    """把同一用例的多个 rep 合并成一份统计（Lead m04755 A：5 reps 合并才过 MIN_N=1e5）。

    - [histo] 桶计数**逐桶相加** ⇒ 合并后的 n 用于 p99.9（单 rep 只有 2.5–3.3 万步会 N/A）
    - [step] n/total/collects 求和、max 取最大（时钟口径由 [clock] 行决定，本函数不解释）
    - [work] 数值字段取最大（同形状确定性，跨 rep 应相同；M_max 用于反读 live）
    - [quota] deadline_us 取首个非空、hits/checks 求和（终值行 per-rep，合并即总和）
    - [top] 跨 rep 汇总按 cpu_us 降序取前 TOP_N
    - stdout：**每个 rep 都要逐字等于期望**；任一不匹配就把 mismatch 标出来（不然早退会假 PASS）
    """
    counts = None
    acc = {"step_n": None, "step_total_us": None, "step_max_us": None, "collects": None}
    work, quota, tops, outs, exits = {}, {}, [], [], []
    stderr_bytes = 0
    reps = []
    for h in hits:
        rep = h.get("rep")
        reps.append(rep)
        pr, txt, so = _load_rep(out, case, rep)
        c = histo_counts(pr)
        if c:
            counts = list(c) if counts is None else [a + b for a, b in zip(counts, c)]
        for k in ("step_n", "step_total_us", "collects"):
            v = pr.get(k)
            if v is not None:
                acc[k] = (acc[k] or 0) + v
        m = pr.get("step_max_us")
        if m is not None:
            acc["step_max_us"] = m if acc["step_max_us"] is None else max(acc["step_max_us"], m)
        for k, v in (pr.get("work") or {}).items():
            if isinstance(v, (int, float)):
                work[k] = v if k not in work else max(work[k], v)
            else:
                work.setdefault(k, v)
        q = pr.get("quota") or {}
        if q:
            if q.get("deadline_us") is not None and quota.get("deadline_us") is None:
                quota["deadline_us"] = q.get("deadline_us")
            for k in ("hits", "checks"):
                if q.get(k) is not None:
                    quota[k] = (quota.get(k) or 0) + q.get(k)
            for k, v in q.items():
                if k not in quota and k not in ("round", "round_hits", "round_checks"):
                    quota[k] = v
        tops.extend(_top_lines(txt))
        outs.append(so)
        exits.append(h.get("exit"))
        stderr_bytes += len(txt.encode())
    tops.sort(key=lambda t: -float(t.get("cpu_us") or t.get("dur_us") or 0))
    all_ok = bool(outs) and all(o == expected for o in outs)
    mmax = work.get("M_max", work.get("m_max"))
    # M_max 单位是字节；live = M_max/3 也是字节（旧版误当 kB 用，导致上界放大 1024 倍）
    live_bytes = int(mmax) // 3 if isinstance(mmax, (int, float)) and mmax else None
    return {
        "reps": reps,
        "n_reps": len(hits),
        "ru_maxrss_kb": None,       # 由调用方填 timemax 的值
        "exit": (None if not exits else (0 if all(e == 0 for e in exits) else next(
            e for e in exits if e != 0))),
        "exits": exits,
        "stdout": outs[0] if all_ok else " | ".join(outs),
        "stdout_all_ok": all_ok,
        "stdout_expected": expected,
        "work_mmax": mmax,
        "work": work,
        "live_bytes": live_bytes,
        "live_kb": (live_bytes // 1024) if live_bytes else None,
        "step_n": acc["step_n"],
        "step_total_us": acc["step_total_us"],
        "step_max_us": acc["step_max_us"],
        "collects": acc["collects"],
        "histo_counts": counts or None,
        "histo_stats": histo_stats(counts),
        "top3": tops[:TOP_N],
        "pstep_lines": [],
        "quota": quota,
        "deadline_hits": quota.get("hits"),
        "stderr_bytes": stderr_bytes,
    }


def collect_one(out, n, tm, runs):
    """G18 的两档：60 轮（单 rep，case=pd_bigobj60）与 200 轮（G18_PD_REPS 个 rep 合并）。"""
    if n == 60:
        case = "pd_bigobj60"
        hits = _pick_runs(runs, case)
    else:
        case = "pd_bigobj"
        hits = _pick_runs(runs, case, n)
    res = _agg_reps(out, case, hits, "D n=%d made=%d s=0" % (n, n))
    res["n"] = n
    res["case"] = case
    res["ru_maxrss_kb"] = tm.get(str(n))
    return res


DENSE_EXPECT = "PD n=200 made=200 s=1048575"


def collect_dense(out, tm_dense, runs):
    """pd_pointer_dense 的观测数据（Lead m03381/m03449/m04755）：只报，不参与 G18 判定。

    同 200 轮口径：G18_PD_REPS（默认 5）个 rep 的 [histo] 合并后 n≥1e5，(b) 通道才可判。
    """
    hits = _pick_runs(runs, "pd_pointer_dense")
    res = _agg_reps(out, "pd_pointer_dense", hits, DENSE_EXPECT)
    counts = res.get("histo_counts") or []
    names = getattr(pp, "HISTO_NAMES", None) or ["b%d" % i for i in range(len(counts))]
    res.update({
        "case": "pd_pointer_dense",
        "rounds": 200,
        "ru_maxrss_kb": tm_dense.get("200"),
        "tail": dict(zip(names[10:], counts[10:])) if counts else None,
        "bounds": histo_bounds(counts),
    })
    res.pop("pstep_lines", None)
    res["pstep_lines"] = []
    return res


def _ts():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def cmd_json(a):
    out = a.out
    tm = _timemax(out, "pd_bigobj")
    runs = _runs(out)
    # 游标 slab 的 nslots 必须实测（[top]/[quota] 都不打印）；测得后用它重跑 json：
    #   G18_SLAB_NSLOTS=<n> python3 tests/gc-p0/g18.py json --out <OUT> --tag <tag>
    try:
        nslots = int(os.environ.get("G18_SLAB_NSLOTS", "") or 0) or None
    except ValueError:
        nslots = None
    # 闭式必须在收到 rounds 之后算：deadline_us 从 [quota] 实测（Lead m03746①：源码默认 800，
    # 硬编码 250 已过时；天花板 = deadline_us + 2*batch_us）。
    res = {"tag": a.tag, "out": os.path.abspath(out), "ts": _ts(),
           "rss_source": "timemax ru_maxrss (/usr/bin/time -f %M, kB)",
           "stdout_source": "run_pause 子进程（sample.py）",
           "slab_mib": SLAB_MIB, "emp_bound_mib": EMP_BOUND_MIB,
           "rounds": {}}
    for n in (60, 200):
        res["rounds"][str(n)] = collect_one(out, n, tm, runs)
    dl = None
    for n in ("200", "60"):
        qd = (res["rounds"].get(n) or {}).get("quota") or {}
        if qd.get("deadline_us") is not None:
            dl = qd.get("deadline_us")
            break
    cf = closed_form(nslots=nslots, deadline_us=dl)
    res["pstep_closed_form_us"] = (cf["lo_us"], cf["hi_us"])
    res["pstep_closed_form"] = cf
    res["pstep_closed_form_note"] = (
        "P_step <= t_sweep(W_s)+c_u*W_m+c_a（c_a=0）；W_s=%d words（%s）、W_m=%d units、"
        "deadline=%d us、标称 %.1f~%.1f us、天花板≈%.1f us"
        % (cf["W_s"], cf["W_s_source"], cf["W_m"], cf["deadline_us"],
           cf["lo_us"], cf["hi_us"], cf["ceiling_us"]))
    res["pointer_dense"] = collect_dense(out, _timemax(out, "pd_pointer_dense"), _runs(out))
    path = os.path.join(out, "g18.json")
    with open(path, "w") as f:
        json.dump(res, f, indent=1)
    print("G18 %s: wrote %s" % (a.tag, path))
    return 0


def _live_bytes(b, a, override_kb=None):
    """live 活集字节数（= M_max/3）。override_kb 仍按 kB 传入（兼容 --live-kb）。"""
    if override_kb:
        return int(override_kb) * 1024
    vals = []
    for r in (b, a):
        for n in ("60", "200"):
            rd = r.get("rounds", {}).get(n) or {}
            v = rd.get("live_bytes")
            if v is None and rd.get("live_kb"):
                v = int(rd["live_kb"]) * 1024      # 兼容旧 g18.json（那时 live_kb 其实是字节）
            if v:
                vals.append(int(v))
    if vals:
        return max(vals)
    env = os.environ.get("G18_LIVE_MIB")
    if env:
        return int(float(env) * MIB)
    return 8 * MIB * 1024          # bytes（MIB 是 kB/单位）


def _chan_rows(ra, cf):
    """两个通道的表 + 判定（Lead m03449 / m03564）。

    ① 在 pd_bigobj 这种**混合通道**形状上不可判（每轮 64 个 16 B leaf + 1 个 8 MiB big，
    插桩不计 n）⇒ 按 Lead m03564 明确写 N/A，并把可判定界作为参考值列出；
    大分配通道走 ②（p99.9 + over1ms=0 + P_step 闭式）。纯常规形状的 ① 见 G1（cyc/pc/pb）。
    """
    L, v = [], {}
    L.append("  [通道] 轮数     n       中位us   p99.9us   >4us     >8us    >1ms   step_max_us  deadline_hits")
    for n in ("60", "200"):
        r = ra.get(n, {})
        c = r.get("histo_counts")
        h = histo_bounds(c)
        st = r.get("histo_stats") or {}
        if not h:
            L.append("  [通道] %-6s n/a（无 [histo]）" % n)
            continue
        nn = h["n"]
        med = st.get("median_us") if nn >= 1000 else None
        p999 = st.get("p999_us") if nn >= 1000 else None
        L.append("  [通道] %-6s %-7d %-7s %-9s %-8d %-7d %-6s %-12s %s"
                 % (n, nn, med if med is not None else "n/a",
                    p999 if p999 is not None else "n/a",
                    h["gt4_us"], h["gt8_us"],
                    h["over1ms"], r.get("step_max_us"), r.get("deadline_hits")))
    c200 = ra.get("200", {}).get("histo_counts")
    h200 = histo_bounds(c200)
    if h200:
        h200 = dict(h200)
        h200["counts"] = c200
    c2, why2 = ch2_verdict(h200, cf)
    v["G18-ch1"] = "N/A-mixed"
    v["G18-ch2"] = c2
    if h200:
        L.append("  ① 常规分配通道（n <= 64 KiB，切片 <= 5 us）：N/A（混合形状不可判）—— "
                 "本形状每轮 64 个 16 B leaf + 1 个 8 MiB big，插桩不计 n，无法把 slice 归因到通道；"
                 "判据只对纯常规形状有效，纯样例见 MEASUREMENTS-GC-P0.md §8 的 G1（cyc/pc/pb）。"
                 "参考界：%d/%d 步 >8 us、%d 步 <=4 us。"
                 % (h200["gt8_us"], h200["n"], h200["le4_us"]))
        L.append("     ⇒ 15 桶直方图上 <=5 us 不可证（4-8 us 桶非空时只能给上界）；N/A ≠ PASS（Lead m03746④）")
    else:
        L.append("  ① 常规分配通道：n/a（无 [histo]）")
    L.append("  ② 大分配通道（n > 64 KiB）：%s —— %s" % (c2, why2))
    L.append("     P_step 闭式（Lead m03746 修正：相加式、紧上界、c_a=0；src/back/reclaim.c:1000-1018, 1340-1385）：")
    L.append("       P_step <= t_sweep(W_s) + c_u*W_m + c_a（c_a 取 0：原 0~18 us 只是拟合残差，不可伪证）")
    L.append("       W_s = 8*pm + max(1, ceil(游标 slab->nslots/64)) = %d+%d = %d words（cap 8192 不可达："
             "slotSize>=16 B ⇒ nslots<4096 ⇒ ceil/64<=64）"
             % (cf["W_s_fixed"], cf["W_s_q"], cf["W_s"]))
    L.append("       W_s 来源：%s" % cf["W_s_source"])
    L.append("       W_m=%d units（%s）；t_sweep=%.1f~%.1f us（~9.5-12 ns/word）、c_u*W_m=%.1f us"
             % (cf["W_m"], cf["W_m_note"], cf["t_sweep_lo_us"], cf["t_sweep_hi_us"], cf["mark_us"]))
    L.append("       ⇒ 标称闭式 %.1f~%.1f us（≈%.3f ms）；deadline=%d us [%s] + 一个 8192-unit 批次 %.1f us"
             "（DL6 保守多一段）⇒ 物理天花板 ≈%.1f us"
             % (cf["lo_us"], cf["hi_us"], cf["lo_us"] / 1000.0, cf["deadline_us"],
                cf["deadline_source"], cf["batch_us"], cf["ceiling_us"]))
    L.append("       [pstep] 全仓无发射器（grep src/ = 0 处）⇒ pstep_lines 必空；按 sweep/mark 通道拆单步耗"
             "须等 [top] 带 w_s/w_m，未拿到前不声称已按通道归因（m03746⑤）")
    L.append("     deadline_hits（[quota] 行）是「内存界成立且 hits 不持续增长」的可观测量；"
             "本形状 200 轮）：%s / 60 轮：%s"
             % (ra.get("200", {}).get("deadline_hits"), ra.get("60", {}).get("deadline_hits")))
    return L, v


def check(b, a, live_kb=None, bound_override_mib=None):
    L, v = [], {}
    rb = b.get("rounds", {})
    ra = a.get("rounds", {})
    dl = None
    for n in ("200", "60"):
        qd = (ra.get(n) or {}).get("quota") or {}
        if qd.get("deadline_us") is not None:
            dl = qd.get("deadline_us")
            break
    cf = closed_form(deadline_us=dl)
    L.append("  %-5s %13s %13s %10s %13s %14s %13s %16s" %
             ("轮数", "ru_maxrss_bef", "ru_maxrss_aft", "比", "stepN b->a", "GCus b->a",
              "stepMax b->a", "deadline_hits b->a"))
    for n in ("60", "200"):
        x, y = rb.get(n, {}), ra.get(n, {})
        pb, pa = x.get("ru_maxrss_kb"), y.get("ru_maxrss_kb")
        L.append("  %-5s %13s %13s %10s %13s %14s %13s %16s" % (
            n, pb, pa,
            ("%.3fx" % (pa / pb)) if (pb and pa) else "n/a",
            "%s->%s" % (x.get("step_n"), y.get("step_n")),
            "%s->%s" % (x.get("step_total_us"), y.get("step_total_us")),
            "%s->%s" % (x.get("step_max_us"), y.get("step_max_us")),
            "%s->%s" % (x.get("deadline_hits"), y.get("deadline_hits"))))
    live_b = _live_bytes(b, a, live_kb)
    live = live_b // 1024                      # 仅用于打印（kB）
    bound_kb = 3 * live_b / 1024 + 2 * int(SLAB_MIB * MIB)   # kB：3·live + 2·SLAB
    if bound_override_mib:
        bound_kb = int(bound_override_mib * MIB)
        src = "G18_BOUND_MIB 覆盖（经验值 3L+2MiB）"
    else:
        src = "3*L + 2*SLAB_SIZE（L 由 [work] M_max/3 反读）"
    p60 = ra.get("60", {}).get("ru_maxrss_kb")
    p200 = ra.get("200", {}).get("ru_maxrss_kb")
    outs_ok = all(ra.get(n, {}).get("stdout") == ra.get(n, {}).get("stdout_expected") for n in ("60", "200"))
    ex_ok = all(ra.get(n, {}).get("exit") == 0 for n in ("60", "200"))
    mem_ok = p200 is not None and p200 <= bound_kb
    scale_ok = (p60 and p200) and p200 <= p60 * 1.05
    v["G18-mem"] = "PASS" if (mem_ok and scale_ok and outs_ok and ex_ok) else "RED"
    L.append("  [硬] live=%s kB（=M_max/3）；200 轮 ru_maxrss=%s kB vs 上界 %d kB [%s] ⇒ %s"
             % (live, p200, int(bound_kb), src, "PASS" if mem_ok else "RED"))
    L.append("  [硬] 规模无关：peak200=%s <= peak60*1.05=%s ⇒ %s"
             % (p200, (round(p60 * 1.05, 1) if p60 else "n/a"), "PASS" if scale_ok else "RED"))
    L.append("  [硬] stdout 逐字=%s；exit=%s ⇒ %s"
             % ("ok" if outs_ok else "MISMATCH",
                [ra.get(n, {}).get("exit") for n in ("60", "200")],
                "PASS" if (outs_ok and ex_ok) else "RED"))
    L.extend(_chan_rows(ra, cf)[0])
    v.update(_chan_rows(ra, cf)[1])
    # ---- 追加观察用例 pd_pointer_dense（Lead m03381；不参与 G18 判定）----
    d = a.get("pointer_dense") or {}
    if d:
        L.append("  [观察] pd_pointer_dense（8 MiB dense + 1M 非空指针 x 200 轮；不参与 G18 判定）：")
        L.append("    ru_maxrss=%s kB  exit=%s  stdout=%s  step_n=%s  step_max=%s us  collects=%s"
                 "  deadline_hits=%s"
                 % (d.get("ru_maxrss_kb"), d.get("exit"),
                    "ok" if d.get("stdout") == d.get("stdout_expected") else ("MISMATCH(%r)" % d.get("stdout")),
                    d.get("step_n"), d.get("step_max_us"), d.get("collects"),
                    d.get("deadline_hits")))
        L.append("    [histo] 尾部：%s" % (d.get("tail"),))
        hb = d.get("bounds") or {}
        L.append("    >4us=%s  >8us=%s  >256us=%s  over1ms=%s"
                 % (hb.get("gt4_us"), hb.get("gt8_us"), hb.get("over256_us"), hb.get("over1ms")))
        hd = dict(hb)
        if d.get("histo_counts"):
            hd["counts"] = d["histo_counts"]
        if hb.get("over1ms") is None:
            v["G18-dense-over1ms"] = "n/a"
            L.append("    ⇒ n/a（无 [histo]，可能没带 PAUSE=1）")
        elif hb["over1ms"] > 0:
            v["G18-dense-over1ms"] = "RED"
            L.append("    ⇒ RED（观察）：P 的停顿通道对大指针稠密对象不成立，需要按 n 分档的准入"
                     "（over1ms=%d > 0）" % hb["over1ms"])
        else:
            v["G18-dense-over1ms"] = "PASS（观察）"
            L.append("    ⇒ PASS（观察）：over1ms=0；单步 max=%s us vs 闭式 %.0f~%.0f us"
                     % (d.get("step_max_us"), cf["lo_us"], cf["hi_us"]))
        st = d.get("histo_stats") or {}
        if (st.get("n") or 0) >= 1000:
            L.append("    p99.9=%s us  中位=%s us（n=%d）" % (st.get("p999_us"), st.get("median_us"), st.get("n")))
        for t in (d.get("top3") or []):
            L.append("    " + str(t.get("raw")))
        bd = b.get("pointer_dense") or {}
        if bd:
            L.append("    before→after：ru_maxrss %s→%s kB，step_n %s→%s，over1ms %s→%s"
                     % (bd.get("ru_maxrss_kb"), d.get("ru_maxrss_kb"), bd.get("step_n"), d.get("step_n"),
                        (bd.get("bounds") or {}).get("over1ms"), hb.get("over1ms")))
    else:
        L.append("  [观察] pd_pointer_dense：无数据（run.sh g18 未跑该用例）")
    # 差分
    L.append("  增量（after-before，正=after 更贵）：")
    for n in ("60", "200"):
        x, y = rb.get(n, {}), ra.get(n, {})
        L.append("    %-3s 轮：Δslices=%s  ΔGCus=%s  Δmax_us=%s"
                 % (n, _d(y.get("step_n"), x.get("step_n")),
                    _d(y.get("step_total_us"), x.get("step_total_us")),
                    _d(y.get("step_max_us"), x.get("step_max_us"))))
    L.append("  [histo] 差分（after-before，200 轮，只列变化桶）：")
    hb2 = dict(_histo_pairs(rb.get("200", {})))
    ha2 = dict(_histo_pairs(ra.get("200", {})))
    diffs = [(k, hb2.get(k, 0), ha2.get(k, 0)) for k in ha2 if ha2.get(k, 0) != hb2.get(k, 0)]
    L.append("    " + ("  ".join("%s:%s->%s" % dd for dd in diffs) if diffs else "无变化（或 before 无 histo）"))
    L.append("  [top] 前 3 条：")
    for tag, rr in (("before", rb.get("200", {})), ("after", ra.get("200", {}))):
        tos = rr.get("top3") or []
        if not tos:
            L.append("    %-6s （无 [top] 行）" % tag)
        for t in tos:
            L.append("    %-6s %s" % (tag, t.get("raw")))
    return L, v


def _d(a, b):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return "%+g" % (a - b)
    return "n/a"


def _histo_pairs(r):
    c = r.get("histo_counts") or []
    names = getattr(pp, "HISTO_NAMES", None) or ["b%d" % i for i in range(len(c))]
    return list(zip(names, c))


def cmd_compare(a):
    b = json.load(open(a.before))
    af = json.load(open(a.after))
    print("G18 对照（before tag=%s / after tag=%s；RSS 均为 ru_maxrss）"
          % (b.get("tag"), af.get("tag")))
    L, v = check(b, af, live_kb=a.live_kb, bound_override_mib=a.bound_mib)
    for ln in L:
        print(ln)
    print("  ⇒ " + "  ".join("%s=%s" % (k, val) for k, val in v.items())
          + "   （G18-mem 为硬判据；①② 通道、pstep、dense 均为观察通道）")
    return 0 if v.get("G18-mem") == "PASS" else 1


def _mk(peak, stepn, tot, mx, kinds, counts=None, seg=8192):
    rounds = {}
    for n in ("60", "200"):
        rounds[n] = {"n": int(n), "ru_maxrss_kb": peak, "exit": 0,
                     "stdout": "D n=%s made=%s s=0" % (n, n),
                     "stdout_expected": "D n=%s made=%s s=0" % (n, n),
                     "work_mmax": 3 * 8 * MIB * 1024, "live_bytes": 8 * MIB * 1024, "live_kb": 8 * MIB,
                     "step_n": stepn, "step_total_us": tot, "step_max_us": mx,
                     "collects": 1, "histo_counts": counts or [0] * 15,
                     "histo_stats": histo_stats(counts or [0] * 15),
                     "top3": [{"raw": "[top] kind=%s why=advance dur_us=%s seg=%s" % (k, mx, seg),
                               "kind": k, "why": "advance", "dur_us": mx, "seg": seg}
                              for k in kinds],
                     "pstep_lines": [],
                     "quota": {"deadline_us": 800, "hits": 3 if n == "200" else 1},
                     "deadline_hits": 3 if n == "200" else 1}
    return {"tag": "t", "rounds": rounds}


def _mkd(counts, peak=140000):
    return {"ru_maxrss_kb": peak, "exit": 0, "stdout": DENSE_EXPECT,
            "stdout_expected": DENSE_EXPECT, "step_n": sum(counts) or 1,
            "step_total_us": 1000.0, "step_max_us": 400.0, "collects": 100,
            "histo_counts": counts, "histo_stats": histo_stats(counts),
            "tail": None, "bounds": histo_bounds(counts), "top3": [], "pstep_lines": []}


def _selftest():
    ok = True
    goodc = [0] * 15
    goodc[0] = 500000
    goodc[10] = 10          # 10 步 512us-1ms
    good = _mk(9000, 421, 4200, 90, ["mark"], goodc)
    bad = _mk(32000, 900, 50000, 2400, ["mark"])
    L, v = check(good, good)
    c1 = (v.get("G18-mem") == "PASS")
    print("  selftest good: mem=%s ch1=%s ch2=%s" % (v.get("G18-mem"), v.get("G18-ch1"), v.get("G18-ch2")))
    L2, v2 = check(bad, bad)
    print("  selftest bad : mem=%s" % v2.get("G18-mem"))
    ok = ok and c1 and v2.get("G18-mem") == "RED"
    # scale：200 轮涨 20% 必须 RED
    b2 = _mk(9000, 421, 4200, 90, ["mark"])
    a2 = _mk(9000, 421, 4200, 90, ["mark"])
    a2["rounds"]["200"]["ru_maxrss_kb"] = int(9000 * 1.2)
    L3, v3 = check(b2, a2)
    print("  selftest scale: %s" % v3.get("G18-mem"))
    ok = ok and v3.get("G18-mem") == "RED"
    # 通道①：全部 <=1us ⇒ PASS；有一半在 b16us 桶 ⇒ RED（>8us）
    c_ok = [0] * 15
    c_ok[0] = 1000
    h = histo_bounds(c_ok)
    print("  selftest ch1 ok: %s" % (ch1_verdict(h)[0],))
    ok = ok and ch1_verdict(h)[0] == "PASS"
    c_bad = [0] * 15
    c_bad[0] = 900
    c_bad[4] = 10
    h2 = histo_bounds(c_bad)
    print("  selftest ch1 bad: %s" % (ch1_verdict(h2)[0],))
    ok = ok and ch1_verdict(h2)[0] == "RED"
    # 通道②：over1ms>0 ⇒ RED
    c_ms = [0] * 15
    c_ms[0] = 999
    c_ms[11] = 1
    h3 = histo_bounds(c_ms)
    print("  selftest ch2 over1ms: %s" % (ch2_verdict(h3, closed_form())[0],))
    ok = ok and ch2_verdict(h3, closed_form())[0] == "RED"
    # dense over1ms>0 ⇒ RED 观察
    a3 = _mk(9000, 421, 4200, 90, ["mark"])
    a3["pointer_dense"] = _mkd(c_ms)
    L4, v4 = check(a3, a3)
    print("  selftest dense: %s" % (v4.get("G18-dense-over1ms"),))
    ok = ok and v4.get("G18-dense-over1ms") == "RED"
    # 通道①跨桶 ⇒ N/A（4-8 us 桶非空，15 桶直方图上 <=5 us 只能给上界；N/A 不得记 PASS）
    c_amb = [0] * 15
    c_amb[0] = 990
    c_amb[3] = 10           # b8us：>4 us 且 <=8 us
    amb = ch1_verdict(histo_bounds(c_amb))[0]
    print("  selftest ch1 cross-bucket: %s" % (amb,))
    ok = ok and amb == "N/A"
    # 闭式常数自证（Lead m03746 修正后：pm=16 ⇒ W_s=8*16+64=192 words、t_sweep 1.8~2.3 us、
    # W_m=8192 units ⇒ 143.4 us、c_a=0 ⇒ 标称 145.2~145.7 us；天花板 = 800 + 2*143.4 = 1086.7 us）
    cf = closed_form()
    print("  selftest closed_form: W_s=%d W_m=%d b=%d seg=%d => %.1f~%.1f us"
          % (cf["W_s"], cf["W_m"], cf["b"], cf["seg"], cf["lo_us"], cf["hi_us"]))
    print("  selftest closed_form: deadline=%d us（%s）天花板=%.1f us；W_s_bounds=%s"
          % (cf["deadline_us"], cf["deadline_source"], cf["ceiling_us"], cf["W_s_bounds"]))
    ok = ok and cf["W_s"] == 192 and 140.0 <= cf["lo_us"] <= 150.0
    ok = ok and cf["ceiling_us"] >= 1000.0
    cfd = closed_form(deadline_us=800)
    ok = ok and cfd["deadline_source"].startswith("[quota]")
    ok = ok and abs(cfd["ceiling_us"] - 1086.7) < 1.0
    # _agg_reps（Lead m04755 A）：多 rep 合并 —— 5 reps 的 [histo] 必须逐桶相加（单 rep <1e5 会 N/A）；
    # [quota] 终值行 hits/checks 求和、deadline 取首个；[work] 数值取最大；任一 rep stdout 不匹配 ⇒
    # stdout_all_ok=False（早退/输出错不得因合并而蒙混过关）。
    import tempfile
    td = tempfile.mkdtemp()
    os.makedirs(os.path.join(td, "log"), exist_ok=True)
    names = getattr(pp, "HISTO_NAMES", None) or ["b%d" % i for i in range(15)]
    def _wr(rep, counts, hits, checks, mmax, out):
        line = "[histo] n=%d " % sum(counts) + " ".join(
            "%s=%d" % (names[i], counts[i]) for i in range(len(counts)))
        txt = ("[clock] step=cpu pause=wall chan=cpu\n" + line + "\n"
               "[step] n=10 max_us=%.1f total_us=%.1f over16=0 over64=0 over256=0 over1ms=0\n"
               "[work] n=10 walk_max=0 M_max=%d pressure_steps=1\n"
               "[quota] round=1 round_hits=%d round_checks=%d\n"
               "[quota] deadline_us=800 max_mark=1048576 shift=3 checks=%d hits=%d\n"
               "[top] rank=1 kind=mark why=advance dur_us=1.0 cpu_us=1.0 seg=8192 pops=1\n"
               % (float(rep), float(rep) * 10.0, mmax, hits, checks, checks, hits))
        with open(os.path.join(td, "log", "pd_bigobj_pause_r%d.err" % rep), "w") as f:
            f.write(txt)
        with open(os.path.join(td, "log", "pd_bigobj_pause_r%d.out" % rep), "w") as f:
            f.write(out)
    c1 = [0] * 15; c1[0] = 26000
    c2 = [0] * 15; c2[0] = 25900; c2[4] = 100
    for i, (h, ch, mx) in ((1, (3, 18, 24576)), (2, (4, 20, 24580))):
        _wr(i, c1 if i == 1 else c2, h, ch, mx, "D n=200 made=200 s=0")
    hits = [{"case": "pd_bigobj", "rep": 1, "args": "200", "exit": 0},
            {"case": "pd_bigobj", "rep": 2, "args": "200", "exit": 0}]
    agg = _agg_reps(td, "pd_bigobj", hits, "D n=200 made=200 s=0")
    okc = [a + b for a, b in zip(c1, c2)]
    c_agg = (agg["histo_counts"] == okc and (agg["histo_stats"] or {}).get("n") == 52000
             and agg["n_reps"] == 2 and agg["reps"] == [1, 2])
    c_quota = agg["quota"].get("hits") == 7 and agg["quota"].get("checks") == 38
    c_work = agg["work_mmax"] == 24580 and agg["step_total_us"] == 30.0
    print("  selftest agg_reps: buckets=%s n=%s quota_hits=%s work_mmax=%s"
          % (c_agg, (agg["histo_stats"] or {}).get("n"), agg["quota"].get("hits"),
             agg["work_mmax"]))
    ok = ok and c_agg and c_quota and c_work and agg["stdout_all_ok"]
    _wr(2, c2, 4, 20, 24580, "D n=200 made=200 s=BOGUS")
    agg2 = _agg_reps(td, "pd_bigobj", hits, "D n=200 made=200 s=0")
    print("  selftest agg_reps stdout 失配: all_ok=%s stdout=%r"
          % (agg2["stdout_all_ok"], agg2["stdout"][:20]))
    ok = ok and not agg2["stdout_all_ok"] and " | " in agg2["stdout"]
    print("SELFTEST: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    q = sub.add_parser("json")
    q.add_argument("--out", required=True)
    q.add_argument("--tag", default="")
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    c.add_argument("--live-kb", type=int, default=None)
    c.add_argument("--bound-mib", type=float, default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if a.cmd == "json":
        return cmd_json(a)
    if a.cmd == "compare":
        return cmd_compare(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
