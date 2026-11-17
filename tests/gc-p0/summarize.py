#!/usr/bin/env python3
"""tests/gc-p0/summarize.py —— 把 runs.jsonl / pauses.jsonl / noise.txt 变成 G1/G4/G8/G9 的门禁表。

判据（2026-11-09 修订，见 run.sh 头与 MEASUREMENTS-GC-P0.md §1.3）：
  G1① over1ms 步数 = 0；
  G1② >50µs 步占比 ≤ 1e-4（常数阈；空转基线仅参考，Lead 2026-11-09 裁决）；
       n<1e5 的用例豁免为 N/A 并报 max（1 步即 2e-3，比例统计不可判）。
       注意：现有插桩只有 over16/64/256/1ms 四个桶（src/back/reclaim.c:2678-2714），
       **没有 >50µs 的精确计数**。over64/n 是 >50µs 占比的**下界**（>64 ⇒ >50），
       over16/n 是**上界**（>16 ⊃ >50）。计时默认墙钟，EXTC_RC_STEP_CPU=1 时为线程 CPU；
       上界通过即可判 PASS；上界不通过只能记 N/A（无法区分 50–64µs 与 >64µs 的步）。
  G1③ p99.9 ≤ 50µs：需 P1-6 直方图；未落地记 **N/A**，不得拿 max 代替。
  G12/G13 计数档 soundness（编译期 EXTC_RC_NOCOUNT=0）：stdout 逐字等于期望且 exit 0；
      默认档（NOCOUNT=1）只记录观测值、不判（无 freeSlot 入口，本机实测不可达）。

只做读数与判据对比，不做调参：不达标记 RED，缺数据记 n/a（结构性不可测也写清原因）。
"""
import json, os, re, statistics, sys, bisect

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pause_parse as pp  # [histo] 桶表唯一来源（v4 定版 15 桶，Lead m02523）

def load(path):
    out = []
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def fmt(v, unit="", nd=2):
    return "n/a" if v is None else f"{v:.{nd}f}{unit}"

def read_text(p):
    return open(p, errors="replace").read() if os.path.exists(p) else ""

def normalize_histo(h):
    """把一份 [histo] 归一化成 v4 定版 15 桶（pause_parse.HISTO_NAMES）+ over1ms。

    兼容三种来源：v4 dict（counts 已 15 桶）/ v4 原始字符串 / 旧 7 桶 dict 或字符串。
    旧桶按「上界 µs→ns」二分映射到新桶下标，旧 bge1ms（>1ms）计入 over1ms。
    **over1ms 一律重算为 sum(counts[b4ms 起])**，不信任外部写入的字段（v4 在途版本曾错位，
    见 MEAS-GC-P0 §10）。返回 None 表示没有直方图。
    """
    if not h:
        return None
    if isinstance(h, str):
        if "bge64ms" in h or "b2us" in h:
            z = pp.parse_histo_line(h)
            return {"n": z["n"], "counts": z["counts"],
                    "over1ms": sum(z["counts"][pp.OVER1MS_FROM:]), "legacy": False}
        m = re.search(r"\bn=(\d+)", h)
        buckets = [[int(num) * (1000 if unit == "ms" else 1), int(cnt)]
                   for num, unit, cnt in re.findall(r"\bb(\d+)(us|ms)=(\d+)", h)]
        ge = re.search(r"\bbge1ms=(\d+)", h)
        return _normalize_legacy(int(m.group(1)) if m else None,
                                 buckets, int(ge.group(1)) if ge else 0)
    if isinstance(h, dict):
        if h.get("counts") and len(h["counts"]) == pp.HISTO_NB:
            c = [int(x) for x in h["counts"]]
            return {"n": h.get("n"), "counts": c,
                    "over1ms": sum(c[pp.OVER1MS_FROM:]), "legacy": False}
        if h.get("buckets"):
            return _normalize_legacy(h.get("n"),
                                     [[int(u), int(c)] for u, c in h["buckets"]],
                                     int(h.get("ge1ms") or 0))
    return None

def _normalize_legacy(n, buckets, ge1ms):
    counts = [0] * pp.HISTO_NB
    for upper_us, cnt in buckets:
        ns = int(upper_us) * 1000
        idx = bisect.bisect_right(pp.HISTO_EDGES_NS, ns) - 1
        counts[max(0, min(idx, pp.HISTO_NB - 2))] += int(cnt)
    return {"n": n, "counts": counts,
            "over1ms": sum(counts[pp.OVER1MS_FROM:]) + int(ge1ms), "legacy": True}

def normalize_histo_counts(hs):
    """多 rep 合并：返回 {n, counts[15], over1ms, legacy}。"""
    n = 0
    counts = [0] * pp.HISTO_NB
    o1 = 0
    legacy = False
    any_ = False
    for h in hs:
        z = normalize_histo(h)
        if not z:
            continue
        any_ = True
        n += (z.get("n") or 0)
        o1 += z["over1ms"]        # lenient：legacy 的 bge1ms 只体现在这里
        legacy = legacy or z["legacy"]
        for i, c in enumerate(z["counts"]):
            counts[i] += c
    return {"n": n if any_ else 0, "counts": counts,
            "over1ms": o1, "legacy": legacy}

def cum_le_ns(counts, ns):
    """<= ns 的累计数（bge64ms 无上界，永不计入）。"""
    return sum(c for i, c in enumerate(counts)
               if i < len(pp.HISTO_EDGES_NS) and pp.HISTO_EDGES_NS[i] <= ns)

def q_upper_us(counts, n, frac):
    """分位点所在桶的上界（µs）；落在 bge64ms 返回 inf。"""
    if not n:
        return None
    c = 0
    for i, cnt in enumerate(counts):
        c += cnt
        if c >= frac * n:
            return pp.HISTO_EDGES_NS[i] / 1000.0 if i < len(pp.HISTO_EDGES_NS) else float("inf")
    return float("inf")

def over1ms_authoritative(cp):
    """G1① 的权威读数：有 [histo] 就以 histo 的「严格 >1ms 桶和」为准，否则退回 [step] over1ms。"""
    z = normalize_histo_counts([p.get("histo") for p in cp])
    step = sum(p.get("over1ms") or 0 for p in cp)
    if z["n"]:
        return z["over1ms"], z, "histo", step
    return step, z, "step", step

def g1_c1_verdict(over1ms):
    return "PASS" if not over1ms else "RED"

def load_noise(out):
    """空转基准：per-line 字段；v2 有 wall_*/cpu_* 双口径，v1 只有单口径（=wall）。取最差 rep。"""
    reps = []
    for line in read_text(os.path.join(out, "noise.txt")).splitlines():
        d = dict(re.findall(r"([a-z0-9_]+)=([0-9eE.+-]+)", line))
        if "wall_over50_ratio" in d or "over50_ratio" in d:
            reps.append(d)
    if not reps:
        return None
    res = {"reps": len(reps), "n": reps[0].get("n")}
    for pre in ("wall", "cpu"):
        keys = [pre + "_max_us", pre + "_over50", pre + "_over50_ratio", pre + "_p999_us"]
        if all(k in reps[0] for k in keys):
            for k in keys:
                res[k] = max(float(r[k]) for r in reps if k in r)
    if "wall_max_us" not in res and "over50_ratio" in reps[0]:  # v1 旧格式：单口径 = wall
        for k in ("max_us", "over50", "over50_ratio", "p999_us"):
            res["wall_" + k] = max(float(r[k]) for r in reps if k in r)
    return res

def asan_status(path_out, path_err):
    """从 run.sh 写的 asan rc=0 与 ASAN stderr 判 PASS。"""
    out = read_text(path_out)
    m = re.search(r"asan rc=(-?\d+)", out)
    err = read_text(path_err)
    report = "AddressSanitizer" in err or "ERROR: LeakSanitizer" in err
    rc = int(m.group(1)) if m else None
    return rc, report, ("PASS" if (rc == 0 and not report) else ("SKIP" if rc is None else "RED"))

def main():
    out = sys.argv[1]
    runs = load(os.path.join(out, "runs.jsonl"))
    pauses = load(os.path.join(out, "pauses.jsonl"))
    # [clock] 自证（v4）：EXTC_RC_STEP_CPU=1 但 runtime 报 cpu=unavailable（strict-ANSI 下 cpuNow()<0）
    # 时，[step] 实际是墙钟，run.sh 记的 clock=cpu 不可信 ⇒ 归到 wall 并标注。
    for p in pauses:
        cf = p.get("clock_fields") or {}
        if p.get("clock") == "cpu" and (cf.get("cpu") == "unavailable" or cf.get("step") == "wall"):
            p["clock"] = "wall"
            p["clock_fallback"] = True
    by = {}
    for r in runs:
        by.setdefault(r["case"], []).append(r)

    print(f"# gc-p0 门禁汇总  out={out}")
    print()

    # ---------------- G8 ----------------
    print("## G8 档 1 完整性（chain2，N=2,000,000）")
    t1 = by.get("g8_t1", [])
    t2 = by.get("g8_t2", [])
    p1 = med([r["vmhwm_kb"] for r in t1])
    p2 = med([r["vmhwm_kb"] for r in t2])
    ratio = (p1 / p2) if (p1 and p2) else None
    verdict = "PASS" if (ratio is not None and ratio <= 1.1) else "RED"
    print(f"  T1 VmHWM median = {p1} kB    T2 = {p2} kB    T1/T2 = {fmt(ratio, 'x', 3)}  [{verdict} <=1.10x]")
    print(f"  T1 exit = {[r['exit'] for r in t1]}  T2 exit = {[r['exit'] for r in t2]}")
    r1, rep1, v1 = asan_status(os.path.join(out, "log", "g8_t1_asan.out"),
                               os.path.join(out, "log", "g8_t1_asan.err"))
    r2, rep2, v2 = asan_status(os.path.join(out, "log", "g8_t2_asan.out"),
                               os.path.join(out, "log", "g8_t2_asan.err"))
    asan_v = "PASS" if (v1 == "PASS" and v2 == "PASS") else ("SKIP" if "SKIP" in (v1, v2) else "RED")
    print(f"  ASAN: T1 rc={r1} 报告={'有' if rep1 else '无'}；T2 rc={r2} 报告={'有' if rep2 else '无'}"
          f"  [{asan_v} 无 UAF/双重释放]")
    print()

    # ---------------- G9 ----------------
    print("## G9 任务根完整性（collect 于活任务存在时）")
    g9 = by.get("g9", [])
    want = {"live0": "1", "freed": "0", "live1": "1", "got": "20", "steps": "5", "tag": "777"}
    if not g9:
        print("  n/a（未跑）")
    else:
        for r in g9:
            p = os.path.join(out, "log", f"g9_r{r['rep']}.out")
            full = read_text(p).strip()
            w = "n/a（csv 重建，非真实墙钟）" if r.get("reconstructed_from_csv") else f"{r['wall_s']}s"
            print(f"  rep{r['rep']}: exit={r['exit']} wall={w}")
            print("    stdout 全文：")
            for ln in (full.splitlines() or ["<空>"]):
                print(f"      {ln}")
            fields = dict(re.findall(r"(live0|freed|live1|got|steps|tag)=(\d+)", full))
            missing = [k for k in want if k not in fields]
            bad = {k: fields[k] for k in want if k in fields and fields[k] != want[k]}
            ok = bool(full) and not missing and not bad and r["exit"] == 0
            extra = ""
            if missing or bad:
                extra = f"  缺字段={missing} 不符={bad}"
            print(f"    判据（after 目标）: live0=1 freed=0 live1=1 got=20 steps=5 tag=777 exit=0"
                  f"  ⇒ [{'PASS' if ok else 'RED'}]" + extra)
        rc9, rep9, v9 = asan_status(os.path.join(out, "log", "g9_asan.out"),
                                    os.path.join(out, "log", "g9_asan.err"))
        print(f"  ASAN: rc={rc9} 报告={'有' if rep9 else '无'}  [{v9}]")
    print()

    # ---------------- G12/G13 ----------------
    print("## G12/G13 计数档/事件档 soundness（由产物宏反推档位；G12 只判强制计数档，G13 两档都判）")

    def tier_of(case, tier):
        txt = read_text(os.path.join(out, "log", f"{case}_{tier}.defines"))
        m = dict(re.findall(r"EXTC_RC_(NOCOUNT|SWEEP_FREE|TIER)\s+(\d+)", txt))
        n, s = m.get("NOCOUNT"), m.get("SWEEP_FREE")
        if (n, s) == ("0", "1"):
            return "T1 事件档(count+sweep-off)"
        if (n, s) == ("1", "0"):
            return "T2 清扫档(sweep,无计数)"
        if (n, s) == ("0", "0"):
            return "T3 计数+清扫档(产物 TIER 3；判据从 NOCOUNT/SWEEP_FREE 反推)"
        if (n, s) == ("1", "1"):
            return "不可达组合(NOCOUNT 1/SWEEP_FREE 1；TIER 宏也不会这样标)"
        return (f"宏缺失(NOCOUNT={n},SWEEP_FREE={s};TIER={m.get('TIER')})")

    def outs(case, tier, rs):
        rows = []
        for r in rs:
            p = os.path.join(out, "log", f"{case}_{tier}_r{r['rep']}.out")
            rows.append((r.get("rep"), r.get("exit"), read_text(p).strip().replace("\n", "")))
        return rows

    # G12：参数槽覆盖 —— 只在强制计数档判；默认档 T2 无 freeSlot 入口 ⇒ PASS-by-design
    rs = by.get("g12_param_count", [])
    if rs:
        rows = outs("g12_param", "count", rs)
        ok = all(e == 0 and o == "alias check b.v=999" for _, e, o in rows)
        print(f"  G12 参数槽覆盖 计数档[{tier_of('g12_param','count')}]: {len(rs)} reps"
              f" exit={[e for _, e, _ in rows]} stdout[rep1]={rows[0][2]!r}"
              f"  [{'PASS' if ok else 'RED'} 期望 'alias check b.v=999']  修复前=111")
    else:
        print("  G12 计数档: n/a（未跑）")
    rs = by.get("g12_param_dflt", [])
    if rs:
        rows = outs("g12_param", "dflt", rs)
        by_design = all(e == 0 and o == "alias check b.v=999" for _, e, o in rows)
        print(f"  G12 参数槽覆盖 默认档[{tier_of('g12_param','dflt')}]（观测，不判）:"
              f" {len(rs)} reps exit={[e for _, e, _ in rows]} stdout[rep1]={rows[0][2]!r}"
              f"  [{'PASS-by-design(不可达)' if by_design else '注意:默认档也异常'}]")

    # G13：copyInto 逐字搬移 —— 两档都判（默认档经 ref 绕道进 T1，本机实测输出 555 ⇒ S0）
    for tier, label in (("dflt", "默认档"), ("count", "强制计数档")):
        rs = by.get(f"g13_bulk_{tier}", [])
        if not rs:
            print(f"  G13 copyInto {label}: n/a（未跑）")
            continue
        rows = outs("g13_bulk", tier, rs)
        ok = all(e == 0 and o == "d0r=7" for _, e, o in rows)
        note = "  ⇒ 默认路径可达（S0）" if tier == "dflt" else ""
        print(f"  G13 copyInto {label}[{tier_of('g13_bulk', tier)}]: {len(rs)} reps"
              f" exit={[e for _, e, _ in rows]} stdout[rep1]={rows[0][2]!r}"
              f"  [{'PASS' if ok else 'RED'} 期望 'd0r=7']  修复前=555{note}")
    print()

    # ---------------- G14（Lead m01688 裁决 (b)：绝对增量 + 对照） ----------------
    print("## G14 默认档循环 copyInto 引用元素（P0-7b；判据 800k ≤1.02×对照 且 ≤4 MB）")
    g14 = {}
    tm_path = os.path.join(out, "timemax.txt")
    for line in read_text(tm_path).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "g14":
            toks = parts[1:-2]                       # argv tokens between name and (maxrss, wall)
            ctl = "ctl" in toks
            key = "ctl" if ctl else (toks[0] if toks else "?")
            g14[key] = (int(parts[-2]), float(parts[-1]))  # -> (maxrss_kb, wall_s)
    grs = by.get("g14", [])
    exits = [r["exit"] for r in grs]
    c_needed = ("50000" in g14 and "100000" in g14 and "800000" in g14 and "ctl" in g14)
    if c_needed:
        k5, k10, k8, kc = g14["50000"][0], g14["100000"][0], g14["800000"][0], g14["ctl"][0]
        ratio = k8 / k5 if k5 else None
        over = k8 / kc if kc else None
        ok = (kc > 0 and k8 <= 1.02 * kc and k8 <= 4096
              and len(exits) == 3 and all(e == 0 for e in exits))
        print(f"  maxRSS 50k={k5} kB / 100k={k10} / 800k={k8} / 800k(对照 ctl)={kc} kB")
        print(f"  ratio(800k/50k) = {fmt(ratio, 'x', 3)}（观测，不作判据）"
              f" / 800k 对照比 = {fmt(over, 'x', 3)}  阈值 1.02")
        print(f"  exit={exits}  [{'PASS' if ok else 'RED'} 800k ≤1.02×对照 且 ≤4096 kB]  （before 6.68x ⇒ RED）")
        if exits and any(e != 0 for e in exits):
            print("  ⚠ 有非 0 退出（trap/失败）⇒ 低 RSS 可能是早退，判 RED；stderr 见 log/g14_r*.err")
        # 平坦 ≠ 有界：若产物里没有 storeStruct* 记账，copyInto 根本没走计数路径（HEAD 即如此）
        ctxt = read_text(os.path.join(out, "c", "g14.c"))
        # 只数**调用点**：运行期表里 extc_rc_storeStructCons/Rooted 的*定义*恒在（且带 unused），
        # 不能把定义当成"发射了记账"（HEAD 生成物有 2 个定义、0 个调用点）。
        acct = len(re.findall(r"^(?!.*static void).*extc_rc_storeStruct[A-Za-z]*\s*\(", ctxt, re.M))
        if acct == 0:
            print("  ⚠ 产物 g14.c 中 extc_rc_storeStruct* 出现 0 次 ⇒ 无 P0-7b 记账，"
                  "此处的平坦**不能**证明有界（before 红必须在带记账的集成构建上量）")
        else:
            print(f"  记账自证：产物 g14.c 中 extc_rc_storeStruct* 出现 {acct} 次")
    else:
        print(f"  n/a（timemax.txt 缺 g14 的 50k/100k/800k/800k(ctl)；现有={sorted(g14)}）")
    print()

    # ---------------- G15（硬判据：R4b 之后不再 XFAIL） ----------------
    print("## G15 pool 槽里的引用赋值 ⇒ 外部边表兜住（**硬判据** maxRSS(800k) ≤ 1.20 × maxRSS(50k)）")
    # Lead R4b：这条从 XFAIL 提升成硬判据。池块与缓冲区一样反查不到宿主，落进同一条
    # extc_rc_storeRefUnknown；R4b 之后那张表按**槽位**记（池块被复用，槽地址不变 ⇒ 表项只会被
    # 改正、不会随轮数增长），插桩实测 pin 表 50k 与 800k 都是 n=35 / cap=128、RSS 3,764 kB 两档相同。
    # 判据不变、只变口径：红了就是外部边表没兜住池块路径，不是已知欠账。
    g15 = {}
    for line in read_text(os.path.join(out, "timemax.txt")).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "g15":
            g15[parts[1]] = (parts[-2], parts[-1])  # args -> (maxrss_kb, wall_s)
    grs = by.get("g15", [])
    exits = [r["exit"] for r in grs]
    if "50000" in g15 and "800000" in g15:
        a = int(g15["50000"][0]); b = int(g15["800000"][0])
        ratio = b / a if a else None
        ok = ratio is not None and ratio <= 1.20 and len(exits) == 2 and all(e == 0 for e in exits)
        verdict = "PASS" if ok else "RED（随轮数增长 ⇒ 外部边表没兜住池块路径）"
        print(f"  50k maxRSS = {a} kB / 800k maxRSS = {b} kB  ratio = {fmt(ratio, 'x', 3)}"
              f"  exit={exits}  [{'PASS' if ok else 'RED'} <=1.20x]  {verdict}")
        if exits and any(e != 0 for e in exits):
            print(f"  ⚠ 有非 0 退出（trap/失败）⇒ 低 RSS 可能是早退，判 RED；stderr 见 log/g15_r*.err")
        ctxt = read_text(os.path.join(out, "c", "g15.c"))
        slot = len(re.findall(r"^(?!.*static inline).*extc_rc_storeRefSlot\s*\(", ctxt, re.M))
        if slot == 0:
            print("  ⚠ 产物 g15.c 中 extc_rc_storeRefSlot 调用点 0 次 ⇒ 形状没有落到 pin 路径，"
                  "此处的平坦/增长与 L10-a 无关")
        else:
            print(f"  记账自证：产物 g15.c 中 extc_rc_storeRefSlot 调用点 {slot} 次")
    else:
        print(f"  n/a（timemax.txt 缺 g15 的 50000/800000 两档；现有={sorted(g15)}）")
    print()


    # ---------------- G21：缓冲区存储里的引用赋值 ----------------
    # Lead R4b。硬判据：两档 stdout 逐字正确 + exit 0 + maxRSS(800k)/maxRSS(50k) <= 1.20。
    # self-proof：产物里 extc_buf_releaseRefs 调用点必须 > 0（形状确实走到整段撤销路径）。
    print('## G21 缓冲区存储里的引用赋值 ⇒ 结构动作撤销外部边（硬判据 maxRSS(800k) <= 1.20 × maxRSS(50k)）')
    g21 = {}
    for line in read_text(os.path.join(out, 'timemax.txt')).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == 'g21':
            g21[parts[1]] = (parts[-2], parts[-1])
    g21r = by.get('g21', [])
    g21exits = [r['exit'] for r in g21r]
    if '50000' in g21 and '800000' in g21:
        a = int(g21['50000'][0]); b = int(g21['800000'][0])
        ratio = b / a if a else None
        want = {'50000': 'G21 n=50000 sum=1400000', '800000': 'G21 n=800000 sum=22400000'}
        seen = {}
        for i in range(1, len(g21r) + 1):
            p = os.path.join(out, 'log', 'g21_r%d.out' % i)
            if not os.path.exists(p):
                continue
            t = read_text(p).strip()
            for k in want:
                if t == want[k]:
                    seen[k] = True
        outs_ok = len(seen) == 2
        ok = (ratio is not None and ratio <= 1.20 and len(g21exits) == 2
              and all(e == 0 for e in g21exits) and outs_ok)
        print('  50k maxRSS = %d kB / 800k maxRSS = %d kB  ratio = %s  exit=%s  stdout_ok=%s  [%s <=1.20x]'
              % (a, b, fmt(ratio, 'x', 3), g21exits, outs_ok, 'PASS' if ok else 'RED'))
        ctxt = read_text(os.path.join(out, 'c', 'g21.c'))
        rel = len(re.findall(r'^(?!.*static ).*extc_buf_releaseRefs\s*\(', ctxt, re.M))
        if rel == 0:
            print('  ⚠ 产物 g21.c 中 extc_buf_releaseRefs 调用点 0 次 ⇒ 形状没走到撤销路径，'
                  '此处的平坦**不能**证明边被撤销了')
        else:
            print('  记账自证：产物 g21.c 中 extc_buf_releaseRefs 调用点 %d 次' % rel)
    else:
        print('  n/a（timemax.txt 缺 g21 的 50000/800000 两档；现有=%s）' % sorted(g21))
    print()
    # ---------------- G22：长串周转的内存上界（XFAIL 棘轮） ----------------
    # Lead R4c。池底 string 的「共享拷贝」把内存按构造过的总量算：k 条 1 MiB 长串顺序构造、
    # 每条用完即弃（活集恒为一条），峰值却随 k 线性涨。判据 maxRSS(16)/maxRSS(4) <= 1.5。
    # 迁移前是 XFAIL（已知欠账，不算门禁红），string 迁到 buf 之后必须转 PASS 并提成硬判据。
    print("## G22 长串周转的内存上界（位移判据 maxRSS(k=16) <= 1.5 × maxRSS(k=4)；迁移前 XFAIL）")
    g22 = {}
    for line in read_text(os.path.join(out, "timemax.txt")).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "g22":
            try:
                g22[parts[1]] = int(parts[2])
            except ValueError:
                pass
    g22r = by.get("g22", [])
    if "4" in g22 and "16" in g22:
        a, b = g22["4"], g22["16"]
        ratio = b / a if a else None
        want = {"4": "G22 k=4 sum=4194304", "16": "G22 k=16 sum=16777216"}
        seen = {}
        for i in range(1, len(g22r) + 1):
            p = os.path.join(out, "log", "g22_r%d.out" % i)
            if not os.path.exists(p):
                continue
            t = read_text(p).strip()
            for kk in want:
                if t == want[kk]:
                    seen[kk] = True
        outs_ok = len(seen) == 2
        ok = (ratio is not None and ratio <= 1.5 and outs_ok
              and all(r.get("exit") == 0 for r in g22r))
        print("  k=4 maxRSS = %d kB / k=16 maxRSS = %d kB  ratio = %s  stdout_ok=%s  [%s <=1.5x]"
              % (a, b, fmt(ratio, "x", 3), outs_ok,
                 "PASS" if ok else "XFAIL(已知：string 还在池上，只还世代号不还板块；迁移后必须转 PASS)"))
    else:
        print("  n/a（timemax.txt 缺 g22 的 4/16 两档；现有=%s）" % sorted(g22))
    print()
    # ---------------- G16 回归：sweep 中新建 slab 的错误标黑 ----------------
    print("## G16 回归：sweep 中新建 slab 的错误标黑（原 bug ⇒ 2M 挂死；判据 3 reps 全 exit 0 且输出正确）")
    g16 = by.get("g16", [])
    if g16:
        exits = [r["exit"] for r in g16]
        tos = [bool(r.get("timed_out")) for r in g16]
        walls = [r.get("wall_s") or 0.0 for r in g16]
        outs = []
        for i in range(1, len(g16) + 1):
            outs.append(read_text(os.path.join(out, "log", f"g16_r{i}.out")).strip())
        need_cnt = all("cnt=2000000" in o for o in outs)
        need_sum = all("sum=1999999000000" in o for o in outs)
        ok = (len(g16) == 3 and all(e == 0 for e in exits) and not any(tos)
              and need_cnt and need_sum and max(walls) < 5.0)
        print(f"  exit={exits} timed_out={tos} wall_max={fmt(max(walls), ' s', 3)}"
              f"  cnt_ok={need_cnt} sum_ok={need_sum}"
              f"  [{'PASS' if ok else 'RED'} 完成且输出正确且 <5 s]")
        print(f"  stdout[rep1] = {outs[0] if outs else '(missing)'}")
        if any(tos):
            print("  ⇒ 命中 --timeout(25 s)：这是修复前旧构建的挂死症状（RED）")
    else:
        print("  n/a（runs.jsonl 缺 g16）")
    print()

    # ---------------- G18：pd_bigobj 60/200 轮 ----------------
    # 口径（Lead m03358，按 doc-audit 复核采纳）：live 从 [work] 的 M_max 反读（M_max = 3·live）；
    # 硬判据 peak200 ≤ 3·live + 2·SLAB_SIZE 且 peak200 ≤ peak60×1.05（规模无关）；RSS 一律取
    # timemax 的 ru_maxrss（/usr/bin/time -f %M），stdout/exit 取 run_pause 进程（两进程两口径）。
    print("## G18 大对象反复分配（pd_bigobj：60/200 轮 × 8 MiB；硬判据 3·live+2·SLAB 与 peak60×1.05）")
    g18_tm = {}
    for line in read_text(os.path.join(out, "timemax.txt")).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "pd_bigobj":
            try:
                g18_tm[parts[1]] = int(parts[2])
            except ValueError:
                pass
    p60, p200 = g18_tm.get("60"), g18_tm.get("200")
    exp = {"60": "D n=60 made=60 s=0", "200": "D n=200 made=200 s=0"}
    # Lead m04755 A：200 轮的 case 是 pd_bigobj（G18_PD_REPS 个 rep），60 轮对照单独叫 pd_bigobj60
    # （单 rep）——两者都要 check exit/stdout，且 200 轮的每个 rep 都必须逐字等于期望。
    g18_runs = [r for r in runs
                if r.get("case") in ("pd_bigobj", "pd_bigobj60") and r.get("tag") == "pause"]
    # 同一 OUT 目录会被 g18 跑多次：runs.jsonl 只追加、日志按 rep 覆盖 ⇒ 每个 (case, rep)
    # 只算最后一次，否则旧跑次的失败 spawn（exit=None）会把 G18 判成 RED。
    _latest = {}
    for _r in g18_runs:
        _latest[(_r.get("case"), _r.get("rep"))] = _r
    g18_runs = [_latest[k] for k in sorted(_latest, key=lambda x: (str(x[0]), x[1] or 0))]
    g18_ok_out, g18_exits, g18_reps = {}, [], {}
    for r in g18_runs:
        arg = (r.get("args") or "").strip()
        g18_exits.append(r.get("exit"))
        case = "pd_bigobj60" if r.get("case") == "pd_bigobj60" else "pd_bigobj"
        got = read_text(os.path.join(out, "log", "%s_pause_r%d.out" % (case, r["rep"]))).strip()
        g18_ok_out[arg] = g18_ok_out.get(arg, True) and (got == exp.get(arg))
        g18_reps[arg] = g18_reps.get(arg, 0) + 1
    out_ok = all(g18_ok_out.values()) if g18_ok_out else False
    ex_ok = bool(g18_exits) and all(e == 0 for e in g18_exits)

    g18 = {}
    g18p = os.path.join(out, "g18.json")
    if os.path.exists(g18p):
        g18 = json.load(open(g18p))
    rounds = g18.get("rounds", {}) if isinstance(g18, dict) else {}
    # 单位：M_max/live 是字节，ru_maxrss 是 kB（旧版把 M_max/3 当 kB 用，上界放大 1024 倍）
    live_bytes = None
    for nn in ("60", "200"):
        rd = rounds.get(nn) or {}
        v = rd.get("live_bytes")
        if v is None and rd.get("live_kb"):
            v = int(rd["live_kb"]) * 1024        # 兼容旧 g18.json（那时 live_kb 其实是字节）
        if v:
            live_bytes = int(v) if live_bytes is None else max(live_bytes, int(v))
    if live_bytes is None:
        live_bytes = int(float(os.environ.get("G18_LIVE_MIB", "8")) * 1024) * 1024
        live_src = "兜底 G18_LIVE_MIB（[work] M_max 未读到）"
    else:
        live_src = "[work] M_max/3 反读（字节）"
    live_kb = live_bytes // 1024
    slab_kb = 1024  # EXTC_RC_SLAB_SIZE = 1<<20
    bound_override = os.environ.get("G18_BOUND_MIB")
    if bound_override:
        bound_kb = int(float(bound_override) * 1024)
        bound_src = "G18_BOUND_MIB 覆盖（经验值 3L+2MiB）"
    else:
        bound_kb = 3 * live_bytes / 1024 + 2 * slab_kb
        bound_src = "3·live + 2·SLAB_SIZE（SLAB=1 MiB）"
    scale_ok = (p60 and p200) and p200 <= p60 * 1.05
    mem_ok = p200 is not None and p200 <= bound_kb
    g18_pass = mem_ok and scale_ok and out_ok and ex_ok
    print("  60 轮 ru_maxrss = %s kB  200 轮 = %s kB  ratio = %s（仅观测）"
          % (p60 if p60 is not None else "n/a", p200 if p200 is not None else "n/a",
             ("%.3fx" % (p200 / p60)) if (p60 and p200) else "n/a"))
    print("  live = %d kB（%s）；上界 = %d kB（%s）⇒ 内存项 %s"
          % (live_kb, live_src, int(bound_kb), bound_src, "PASS" if mem_ok else "RED"))
    print("  规模无关：peak200=%s ≤ peak60×1.05=%s ⇒ %s"
          % (p200, ("%.1f" % (p60 * 1.05)) if p60 else "n/a", "PASS" if scale_ok else "RED"))
    print("  exit = %s  stdout逐字 = %s（%s）（stdout 来自 run_pause 进程、RSS 来自 timemax 进程）⇒ G18 %s"
          % (g18_exits, "ok" if out_ok else "MISMATCH/缺", g18_ok_out, "PASS" if g18_pass else "RED"))
    if rounds:
        names = getattr(pp, "HISTO_NAMES", None) or ["b%d" % i for i in range(15)]
        edges = getattr(pp, "HISTO_EDGES_NS", None)
        for nn in ("60", "200"):
            r = rounds.get(nn, {})
            print("  [pause] %s 轮（%s reps 合并）：step_n=%s  step_total=%s us  step_max=%s us  collects=%s"
                  "  deadline_hits=%s"
                  % (nn, r.get("n_reps"), r.get("step_n"), r.get("step_total_us"),
                     r.get("step_max_us"), r.get("collects"), r.get("deadline_hits")))
            c = r.get("histo_counts") or []
            if c:
                print("    [histo] " + " ".join("%s=%d" % (nm, v) for nm, v in zip(names, c)))
                h = r.get("histo_stats") or {}
                tot = h.get("n") or sum(c)
                if tot >= 1000 and edges:
                    print("    [histo] n=%d 中位=%s us  p99.9=%s us" % (tot, h.get("median_us"), h.get("p999_us")))
                else:
                    print("    [histo] n=%d <1000 ⇒ 中位/p99.9 不可判，只报 max=%s us" % (tot, r.get("step_max_us")))
            for t in (r.get("top3") or []):
                print("    " + str(t.get("raw")))
        # Lead m03449：分通道（① 常规 n<=64 KiB：切片 <=5us；② 大分配 n>64 KiB：p99.9+over1ms=0 + P_step 闭式）
        def _bnd(c):
            cc = [int(x) for x in c]
            return (sum(cc), sum(cc[:3]), sum(cc[3:]), sum(cc[4:]), sum(cc[11:]))

        print("  [通道] 轮数     n        <=4us    >4us    >8us    >1ms   step_max_us  deadline_hits")
        rows = []
        for nn in ("60", "200"):
            c = rounds.get(nn, {}).get("histo_counts") or []
            if not c:
                print("  [通道] %-6s n/a（无 [histo]）" % nn)
                continue
            n_, le4, gt4, gt8, o1 = _bnd(c)
            rows.append((n_, le4, gt4, gt8, o1))
            print("  [通道] %-6s %-8d %-8d %-7d %-7d %-6d %-12s %s"
                  % (nn, n_, le4, gt4, gt8, o1,
                     rounds.get(nn, {}).get("step_max_us"),
                     rounds.get(nn, {}).get("deadline_hits")))
        if rows:
            n_, le4, gt4, gt8, o1 = rows[-1]
            # Lead m03564：本形状是混合通道（每轮 64 个 16 B leaf + 1 个 8 MiB big，插桩不计 n），
            # ① 只能在纯常规形状（G1 的 cyc/pc/pb）上判；这里写 N/A 并把可判定界作参考，
            # 不据此判产品红。大分配通道走 ②。
            print("  ① 常规通道（n<=64KiB，切片 <=5us）：N/A（混合形状不可判）—— 判据只对纯常规"
                  "形状有效；参考界：%d 步 >8us、%d 步 <=4us（n=%d）" % (gt8, le4, n_))
            print("     ⇒ 15 桶直方图上 <=5us 不可证（4-8us 桶非空时只能给上界）；N/A ≠ PASS（Lead m03746④）")
            if o1 > 0:
                ch2 = "RED（over1ms=%d >0）" % o1
            elif n_ >= 1000:
                ch2 = "PASS（over1ms=0；n>=1e3，p99.9 见表）"
            else:
                ch2 = "N/A（over1ms=0；n=%d <1e3 ⇒ p99.9 不可判，只报 max）" % n_
            print("  ② 大分配通道（n>64KiB）：%s" % ch2)
            cf = g18.get("pstep_closed_form") or {}
            if cf:
                print("  P_step 闭式（Lead m03746 修正：相加式、紧上界、c_a=0）："
                      "W_s=%s（%s）words、W_m=%s units、deadline=%s us [%s]"
                      % (cf.get("W_s"), cf.get("W_s_source"), cf.get("W_m"),
                         cf.get("deadline_us"), cf.get("deadline_source")))
                print("    t_sweep=%s~%s us + c_u*W_m=%s us + c_a=0 ⇒ 标称 %.1f~%.1f us；"
                      "物理天花板≈%s us（deadline + 2 个 8192-unit 批次，DL6 保守）"
                      % (cf.get("t_sweep_lo_us"), cf.get("t_sweep_hi_us"), cf.get("mark_us"),
                         cf.get("lo_us"), cf.get("hi_us"), cf.get("ceiling_us")))
                print("    W_s_bounds=%s（cap 8192 不可达）；[pstep] 无发射器 ⇒ 不得声称已按 sweep/mark 通道归因"
                      % (cf.get("W_s_bounds"),))
        d = g18.get("pointer_dense") or {}
        if d and d.get("step_n") is not None:
            hb = d.get("bounds") or {}
            st = d.get("histo_stats") or {}
            print("  [观察] pd_pointer_dense（8 MiB + 1M 非空指针 x 200 轮；不参与 G18 判定）：")
            print("    ru_maxrss=%s kB  exit=%s  stdout=%s（%s reps）  step_n=%s  step_max=%s us  over1ms=%s"
                  "  deadline_hits=%s"
                  % (d.get("ru_maxrss_kb"), d.get("exit"),
                     "ok" if d.get("stdout") == d.get("stdout_expected") else "MISMATCH",
                     d.get("n_reps"), d.get("step_n"), d.get("step_max_us"), hb.get("over1ms"),
                     d.get("deadline_hits")))
            print("    [histo] 尾部：%s" % (d.get("tail"),))
            if st.get("n"):
                print("    p99.9=%s us  中位=%s us（n=%s）；>4us=%s >8us=%s >256us=%s"
                      % (st.get("p999_us"), st.get("median_us"), st.get("n"),
                         hb.get("gt4_us"), hb.get("gt8_us"), hb.get("over256_us")))
            if (hb.get("over1ms") or 0) > 0:
                print("    ⇒ RED（观察）：P 的停顿通道对大指针稠密对象不成立，需要按 n 分档的准入")
            elif hb.get("over1ms") == 0:
                print("    ⇒ PASS（观察）：over1ms=0（max 只观察，不作判据）")
            for t in (d.get("top3") or []):
                print("    " + str(t.get("raw")))
        seg_hits, kinds = [], set()
        for nn in ("60", "200"):
            for t in (rounds.get(nn, {}).get("top3") or []):
                if t.get("kind"):
                    kinds.add(str(t["kind"]))
                if t.get("kind") in ("mark", "sweep") and isinstance(t.get("seg"), (int, float)) and t["seg"] > 0:
                    seg_hits.append("r%s %s/%s seg=%s dur=%s" % (nn, t.get("kind"), t.get("why"), t.get("seg"), t.get("dur_us")))
        print("  [top] kind=%s；kind∈{mark,sweep} 且 seg>0 的条数=%d ⇒ 对应本次大分配：%s"
              % (sorted(kinds) or "空", len(seg_hits), "PASS" if seg_hits else ("RED" if kinds else "n/a")))
        if seg_hits:
            print("    " + " | ".join(seg_hits[:3]))
    else:
        print("  [pause] n/a（run.sh g18 会写 g18.json；跨构建对照："
              "python3 tests/gc-p0/g18.py compare out/p0-before/g18.json out/p0-after/g18.json）")
    print("  参考：before 锚点（build-lead5，Lead m03207）60 轮 493,400 kB / 200 轮 1,067,288 kB（≈130 个 8 MiB big 未回收）。")
    print()

    # ---------------- G20：P0-B 停顿收益 ----------------
    print("## G20 P0-B 停顿收益（bench/gc/v2 四形状，PAUSE=1 + STEP_CPU=1；跨 before/after 用 g20 compare）")
    g20p = os.path.join(out, "g20.json")
    if os.path.exists(g20p):
        g20 = json.load(open(g20p))
        print("  tag=%s  extc md5=%s  reps=%s  pin=%s  cflags=%s"
              % (g20.get("tag"), g20.get("extc_md5"), g20.get("reps"), g20.get("pin"), g20.get("cflags")))
        print("  %-10s %10s %9s %13s %11s %10s" % ("shape", "wall_med", "rss_med", "stepTot_med", "stepMax_med", "collects"))
        for s in ["promote", "sizeratio", "chase", "cycleratio"]:
            m = g20.get("shapes", {}).get(s, {}).get("median", {})
            print("  %-10s %10s %9s %13s %11s %10s"
                  % (s, m.get("wall_ms"), m.get("rss_kb"), m.get("step_total_us"),
                     m.get("step_max_us"), m.get("collects")))
        print("  判据（Lead m03207）：promote stepTot ≤-10%、sizeratio ≤-40%、cycleratio ≤+3%；"
              "promote stepMax median ≤60us（观察口径）；四形状 wall/RSS ±3%。")
    else:
        print("  n/a（没有 %s；先跑 run.sh g20，再 python3 tests/gc-p0/g20.py compare before.json after.json）" % g20p)
    print()

    # ---------------- G17 内存墙双配置峰值 ----------------
    print("## G17 内存墙双配置峰值（A 默认隐式 3·L / D 16MiB / B 64MiB / C 1TB；观察项，不判 PASS/RED）")
    # Lead m01949①：峰值应随 M_max 单调；若 A≈B≈C ⇒ 墙未真正约束。形状见 src/peak_cfg.extc。
    pkf = os.path.join(out, "timemax_peak.txt")
    if os.path.exists(pkf):
        groups = {}
        for line in open(pkf):
            parts = line.split()
            if len(parts) >= 5 and parts[0] == "peak":
                groups.setdefault(parts[1], []).append(int(parts[3]))
        label = {"A": "默认(隐式3L)", "D": "16MiB 显式", "B": "64MiB 显式", "C": "1TB 显式"}
        vals = {}
        for cfg in ("A", "D", "B", "C"):
            if cfg in groups:
                vals[cfg] = med(groups[cfg])
                print(f"  cfg={cfg:2s} M_max={label[cfg]:14s} VmHWM median={vals[cfg]} kB"
                      f" = {fmt((vals[cfg] or 0)/1024, ' MB', 1)}（{len(groups[cfg])} reps）")
        for cfg in ("A", "D", "B", "C"):
            errf = os.path.join(out, "log", f"peak_{cfg}_r1.err")
            txt = read_text(errf)
            m = re.search(r"^\[work\].*thr_max=(\d+).*mmax=(\d+).*pressure_steps=(\d+).*M_max=(\d+).*M_max_now=(\d+).*M_max_implicit=(\d)", txt, re.M)
            warn = "  [mmax] " + txt.split("[mmax] ", 1)[1].splitlines()[0] if "[mmax] " in txt else ""
            if m:
                print(f"    cfg={cfg:2s} [work] thr_max={m.group(1)} mmax={m.group(2)} pressure_steps={m.group(3)}"
                      f" M_max={m.group(4)} M_max_now={m.group(5)} implicit={m.group(6)}{warn}")
            elif warn:
                print(f"    cfg={cfg:2s}{warn}")
        if "A" in vals and "B" in vals and "C" in vals:
            if vals["A"] < vals["B"] < vals["C"] * 0.98:
                print("  ⇒ 单调 A<B<C：内存墙确实约束峰值")
            elif vals["A"] < vals["B"] and abs(vals["B"] - vals["C"]) <= max(1024, vals["B"] * 0.02):
                print("  ⇒ A<B≈C：峰值随显式墙上升，但在 B 以上饱和——饱和原因是触发阈值 thr（自学习 scale）"
                      "先于 M_max 成为约束，不是墙没生效；判据『A<B<C』在本形状下不成立，如实记录")
            else:
                print("  ⇒ 未见单调性，墙未真正约束（需检查 thr/clamp 路径）")
    else:
        print("  n/a（未跑；run.sh peak）")
    print()

    # ---------------- L10：线性泄漏用例（tests/gc-p0/l10_check.py 判定，这里只转述）----------------
    print("## L10：线性泄漏用例（VmHWM 800k/50k <= 1.25x 且 rs_live <= 8；判定在 l10_check.py）")
    l10 = None
    path = os.path.join(out, "l10.json")
    if os.path.exists(path):
        try:
            l10 = json.loads(read_text(path))
        except ValueError:
            l10 = None
    if l10:
        for case in sorted(l10.get("cases", {})):
            r = l10["cases"][case]
            n = r.get("numbers", {})
            detail = ""
            if "ratio" in n:
                detail = ("50k=%d kB / 800k=%d kB  ratio=%.2fx  rs_live=%s/%s"
                          % (n["hwm_50k_kb"], n["hwm_800k_kb"], n["ratio"],
                             n.get("rs_live_50000"), n.get("rs_live_800000")))
            print("  %s %s  %s" % (r.get("verdict"), r.get("name"), detail))
            for c in r.get("checks", []):
                print("        · " + c)
        print("  ⇒ " + str(l10.get("verdict")))
    else:
        print("  n/a（未跑；run.sh l10）")
    print()

    # ---------------- G4 ----------------
    print("## G4 内存界（只看非 pause 行；pause 行是 50M 的 G1 负载，口径不同）")
    def gate_runs(case):
        return [r for r in by.get(case, []) if r.get("tag", "") == ""]
    pb = gate_runs("pb")
    if pb:
        per = med([(r["vmhwm_kb"] * 1024.0) / 100000000 for r in pb])
        ok = per is not None and 18 <= per <= 22
        print(f"  pb(100M): per-node = {fmt(per, ' B/node', 2)}  [{'PASS' if ok else 'RED'} 20±10% = 18-22]")
    pc = gate_runs("pc")
    if pc:
        s = med([r["vmhwm_kb"] for r in pc])
        ok = s is not None and s <= 4096
        print(f"  pc(100M): VmHWM median = {s} kB = {fmt((s or 0)/1024, ' MB', 2)}  [{'PASS' if ok else 'RED'} <=4 MB]")
    cyc = gate_runs("cyc")
    if cyc:
        c20 = [r for r in cyc if r["rep"] == 1]
        c80 = [r for r in cyc if r["rep"] == 2]
        if c20 and c80:
            a, b = med([r["vmhwm_kb"] for r in c20]), med([r["vmhwm_kb"] for r in c80])
            ratio = (b / a) if (a and b) else None
            print(f"  cyc N=20M = {a} kB / N=80M = {b} kB  ratio = {fmt(ratio, 'x', 3)}"
                  f"  [{'PASS' if (ratio is not None and ratio < 1.5) else 'RED'} <1.5x]")
    # M_max：优先看产物宏（compile-facts 落地后会发射 EXTC_RC_M_MAX / EXTC_RC_MMAX），
    # 其次看 pauses.jsonl 的 [work] 行（runtime-facts 可能在运行期打出），最后看环境变量。
    mmax_src = None
    for fn in sorted(os.listdir(os.path.join(out, "log"))) if os.path.isdir(os.path.join(out, "log")) else []:
        if fn.endswith(".defines"):
            m = re.search(r"#define\s+EXTC_RC_M_?MAX\s+(\d+)", read_text(os.path.join(out, "log", fn)))
            if m:
                mmax_src = (f"{fn}: EXTC_RC_M_MAX={m.group(1)}", int(m.group(1)))
                break
    if mmax_src is None:
        # Lead m03278 备注：pd_bigobj 的 [work] 行没有 m_max（读出 0），且 G18 的 [work] 只描述
        # 大对象分配，不代表内存墙取值 ⇒ 只接受 >0 的候选，并取各用例中最大的正值。
        cands = []
        for p in pauses:
            w = p.get("work") or {}
            if "m_max" in w or "M_max" in w:
                try:
                    v = int(w.get("m_max", w.get("M_max")))
                except (TypeError, ValueError):
                    continue
                if v > 0:
                    cands.append((v, p.get("case")))
        if cands:
            v, cs = max(cands)
            mmax_src = (f"[work] m_max={v}（{cs}；各用例最大正值）", v)
    env_mmax = os.environ.get("EXTC_RC_M_MAX", "")
    if mmax_src is not None:
        print(f"  正式判据『4× 注入 ≤ M_max+8 MiB』：M_max 实际取值 {mmax_src[1]} B（{mmax_src[0]}）"
              f" ⇒ 可判（需另跑 4× 注入形状；本表只有 cyc 代理）")
    elif env_mmax:
        print(f"  正式判据『4× 注入 ≤ M_max+8 MiB』：EXTC_RC_M_MAX={env_mmax}（来自环境） ⇒ 可判（需另跑 4× 注入形状）")
    else:
        print("  正式判据『4× 注入 ≤ M_max+8 MiB』：M_max 未实现/未在产物或 [work] 出现"
              " ⇒ 当前不可判（cyc N vs 4N 仅代理）")
    # P0-3 的 pressure 触发（P1-2 插桩；旧产物无此字段）
    pw = [p for p in pauses if (p.get("work") or {}).get("pressure") is not None]
    if pw:
        tot = sum(int((p.get("work") or {}).get("pressure") or 0) for p in pw)
        per = {p["case"]: int((p.get("work") or {}).get("pressure") or 0) for p in pw}
        print(f"  [work] pressure=1 触发次数（P0-3 releaseOld 预算耗尽改靠 sweep）：合计 {tot}；按用例 {per}")
    else:
        print("  [work] pressure 字段：n/a（P1-2 插桩未落地，本 build 的 [work] 行不含 pressure）")
    print()

    # ---------------- G4 正式验收（stress.py） ----------------
    print("## G4 正式验收：注入 4×L 循环垃圾（tests/gc-p0/stress.py；判据 Lead m02258）")
    g4s = os.path.join(out, "g4_stress.json")
    if os.path.exists(g4s):
        d = json.load(open(g4s))
        for line in d.get("detail", []):
            print("  " + line)
        print(f"  ⇒ G4-Stress：{'PASS' if d.get('verdict') == 'PASS' else 'RED'}"
              f"（形状 live={d['args']['live']} garb={d['args']['garb']} rounds={d['args']['rounds']}，"
              f"{d['args']['reps']} reps/档）")
    else:
        print("  n/a（未跑；run.sh stress。注意：上面的 g4 段只是 G6/G7 代理，**不能**当 G4 通过）")
    print()

    # ---------------- P0-3 ----------------
    print("## P0-3 权衡形状：10×400k 环峰值（**已知权衡，不判 PASS/RED**）")
    p3 = [r for r in by.get("p03_ring_release", []) if r.get("tag", "") == ""]
    if p3:
        peak = med([r["vmhwm_kb"] for r in p3])
        print(f"  峰值 VmHWM median = {peak} kB = {fmt((peak or 0)/1024, ' MB', 1)}（{len(p3)} reps）")
        print("  参照（Lead m01373 / runtime-facts 自测）：HEAD 同形状 45.8 MB；after 预期 ~107 MB（2.3×）"
              " —— releaseOld 预算耗尽后改靠 sweep 的已知代价，**不是回归判据**。")
    else:
        print("  n/a（未跑；run.sh p03）")
    print()

    # ---------------- G1 ----------------
    print("## G1 停顿尾部（2026-11-09 裁决：① over1ms=0；② >50µs 占比 ≤1e-4 常数阈"
          "（n<1e5 ⇒ N/A+max；空转仅参考）；③ p99.9≤50µs）")
    print("   口径：clock=wall 为墙钟（含抢占），clock=cpu 为 EXTC_RC_STEP_CPU=1 的线程 CPU；"
          "判据用 cpu 口径（无 cpu 数据时退回 wall 并标注）。")
    nz = load_noise(out)
    if nz:
        nn = int(nz["n"]) if nz.get("n") else "?"
        for pre in ("wall", "cpu"):
            if nz.get(pre + "_max_us") is None:
                continue
            print(f"  空转基准 noise_floor {pre}（n={nn} inner=200，{nz['reps']} reps 取最差）："
                  f" max={fmt(nz[pre + '_max_us'], ' us', 1)} over50={fmt(nz[pre + '_over50'], '', 0)}/{nn}"
                  f" ratio={nz[pre + '_over50_ratio']:.2e} p999={fmt(nz[pre + '_p999_us'], ' us', 1)}")
    else:
        print("  空转基准 noise_floor：n/a（未跑；先跑 tests/gc-p0/run.sh noise）")
    bound = 1e-4  # 常数阈（Lead 裁决）；空转 over50 率只作参考，不参与判定
    hist_any = False
    verdicts = []
    for case in ("cyc", "pc", "pb", "pd"):
        ps = [p for p in pauses if p["case"] == case]
        if not ps:
            continue
        clocks = [c for c in ("cpu", "wall") if any((p.get("clock") or "wall") == c for p in ps)]
        for clock in clocks:
            cp = [p for p in ps if (p.get("clock") or "wall") == clock]
            steps = sum(p.get("step_n") or 0 for p in cp)
            o16 = sum(p.get("over16") or 0 for p in cp)
            o64 = sum(p.get("over64") or 0 for p in cp)
            o1ms = sum(p.get("over1ms") or 0 for p in cp)
            mx = max([p["step_max_us"] for p in cp if p["step_max_us"] is not None] or [0])
            mn = med([p["step_mean_us"] for p in cp if p["step_mean_us"] is not None])
            wk = (cp[0].get("worst_kind"), cp[0].get("worst_why"), cp[0].get("worst_dur_us"))
            # ---- [histo]：按 v4 定版 15 桶表（pause_parse.HISTO_*）硬编码归一化；
            #      旧 7 桶产物在 normalize_histo 里做 legacy 映射（bge1ms 计入 over1ms）。
            hz = normalize_histo_counts([p.get("histo") for p in cp])
            hn, hc = hz["n"], hz["counts"]
            o1ms_used, o1ms_src = (hz["over1ms"], "histo") if hn else (o1ms, "step")
            lo = hi = None
            p50_up = p99_up = p999_up = None
            if hn:
                hist_any = True
                lo = (hn - cum_le_ns(hc, 64000)) / hn   # P(>64µs) ⇒ >50µs 占比的**下界**
                hi = (hn - cum_le_ns(hc, 32000)) / hn   # P(>32µs) ⇒ 上界（比 over16 更紧）
                p50_up = q_upper_us(hc, hn, 0.5)
                p99_up = q_upper_us(hc, hn, 0.99)
                p999_up = q_upper_us(hc, hn, 0.999)
            else:
                lo = (o64 / steps) if steps else None
                hi = (o16 / steps) if steps else None
            lo_s = "n/a" if lo is None else ("%.2e" % lo)
            hi_s = "n/a" if hi is None else ("%.2e" % hi)
            if o1ms_used > 0:
                v = "RED"
            elif lo is None or steps == 0:
                v = "n/a"
            elif steps < 100000:
                v = "N/A(n<1e5)"
            elif lo > bound:
                v = "RED(下界>阈)"
            elif hi <= bound:
                v = "PASS(上界<=阈)"
            else:
                v = "N/A(区间跨阈)"
            # ③ p99.9 ≤50µs：桶界 32/64 ⇒ ≤32 可判 PASS；==64 只能记 N/A（50–64 不可分）；>64 判 RED
            if p999_up is None:
                v3 = "N/A(无直方图)"
            elif p999_up <= 32:
                v3 = "PASS(<=32us)"
            elif p999_up <= 64:
                v3 = "N/A(桶界32-64)"
            else:
                v3 = "RED(>64us)"
            verdicts.append((clock, v, v3))
            clk = cp[0].get("clock_fields") or {}
            clk_s = ""
            if clk:
                clk_s = ("  [clock] step=%s pause=%s chan=%s" % (
                    clk.get("step", "?"), clk.get("pause", "?"), clk.get("chan", "?")))
                if clk.get("cpu") == "unavailable":
                    clk_s += " cpu=unavailable（runtime 已退回墙钟，本行按 wall 口径）"
                if clk.get("pause") == "clock":
                    clk_s += "（pause=clock 表示 [pause] 是进程 CPU 时间，不是墙钟）"
            elif clock == "cpu" and not any(p.get("clock_line") for p in cp):
                clk_s = "  ⚠ 无 [clock] 行（旧 build）⇒ 口径不可自证"
            elif any(p.get("clock_fallback") for p in cp):
                clk_s = "  ⚠ cpu=unavailable ⇒ 已按 wall 口径统计"
            p999s = ("%g us（桶上界）" % p999_up) if p999_up is not None else "N/A"
            p999s = p999s.replace("inf us（桶上界）", ">64000 us")
            p50s = ("%g" % p50_up) if p50_up is not None else "n/a"
            p99s = ("%g" % p99_up) if p99_up is not None else "n/a"
            o1s = ("" if o1ms_src == "histo" else "（无 histo，用 [step]）")
            if o1ms_src == "histo" and o1ms != o1ms_used:
                o1s += " ⚠ [step].over1ms=%d 与 histo 不一致（以 histo 为准）" % o1ms
            if hz["legacy"]:
                o1s += "（histo 为旧 7 桶 legacy 映射）"
            print(f"  {case:4s} clock={clock:4s}: steps={steps}({len(cp)} reps 合并, n/rep≈{steps // max(len(cp), 1)})"
                  f" over16={o16} over64={o64}"
                  f" (>50µs 占比∈[{lo_s},{hi_s}] 阈={bound:.1e}) over1ms={o1ms_used}({o1ms_src}){o1s}"
                  f"  step max={fmt(mx, ' us', 1)} mean={fmt(mn, ' us', 3)} worst={wk}"
                  f"  p50≤{p50s}us p99≤{p99s}us p99.9={p999s}"
                  f"  ⇒ [①{g1_c1_verdict(o1ms_used)} ②{v} ③{v3}]{clk_s}")
    if verdicts:
        cpu_v = [x for x in verdicts if x[0] == "cpu"]
        use = cpu_v if cpu_v else verdicts
        scope = "cpu 口径" if cpu_v else "wall 口径（无 cpu 数据）"
        v2s = [x[1] for x in use]
        v3s = [x[2] for x in use]
        if any(v.startswith("RED") for v in v2s):
            agg = "RED"
        elif any(v.startswith("N/A") for v in v2s):
            agg = "N/A（结构性不可判，见 §1.3 第 6 条）"
        else:
            agg = "PASS"
        if any(v.startswith("RED") for v in v3s):
            agg3 = "RED"
        elif v3s and all(v.startswith("PASS") for v in v3s):
            agg3 = "PASS"
        elif v3s and all(v.startswith("N/A") for v in v3s):
            agg3 = "N/A（无直方图或桶界 32–64 不可分；不得拿 max 代替）"
        else:
            agg3 = "N/A（部分用例桶界不可分：%s）" % "/".join(v3s)
        print(f"  ⇒ G1② 汇总（{scope}，**legacy 混合分母，仅对照**）：{agg}；③ p99.9：{agg3}")
        cpu_ps = [p for p in pauses if (p.get("clock") or "wall") == "cpu"]
        if cpu_ps and not any(p.get("clock_line") for p in cpu_ps):
            print("  ⚠ clock=cpu 但 stderr 无 [clock] 行 ⇒ 运行时可能不支持 EXTC_RC_STEP_CPU（旧 build），"
                  "该口径不可信，请重建后再跑 g1cpu。")
    # ---- G1② 通道拆分（**新判据**：tests/gc-p0/g1c.py；Lead m04215 / task-29）
    print()
    print("## G1② 通道拆分（**新判据**，tests/gc-p0/g1c.py；Lead m04215+m04242+m04591+m04755）："
          "(a-warm) seg=0：>50us 占比 ≤1e-4（扣噪声）且 p99.9 ≤10us；"
          "(a-cold) cold-dense（默认 pd_pointer_dense）不判 5us 门禁，只判 tau_t=步时/pops ≤2us（pops≥2）、"
          "p99.9 ≤1ms、over1ms=0，>50us 只作 OBS；pops≥1 每 pop 成本只作 OBS；"
          "(a-mixed) 混合通道（pd_bigobj/pd_bigobj60/G1 的 pd：每轮 64×16B + 1×8MiB，插桩不按 n 归因）"
          "(a) 记 **N/A**（Lead m03564，N/A≠PASS），>50us 只作 OBS，大分配看 (b)；"
          "(b) seg>0：p99.9 ≤ min(B_seg(shape)=1.5×c_seg×chunk+1us, 64us 桶界兜底) **且** over1ms=0；"
          "64us 是 b64us 桶界精度（非 50us 精确值；v17 起用 [histo]/[segsteps] 的 gt10us/gt50us/gt64us 精确计数判，旧产物退回桶界）；"
          "c_seg 取自 tests/gc-p0/bseg-cal.tsv（按形状，不得借；未标定⇒N/A，不退回 5/10us）；"
          "(b).max **只 OBS 不判**（尾部与 chunk 无关、机制未定位，task-32 归因后再议）；"
          "(c) regionfree/release/trigger-step（v16 及更早叫 collect）：max ≤1ms（目标 ≤0.2ms）")
    g1cp = os.path.join(out, "g1c.json")
    gj = None
    if os.path.exists(g1cp):
        try:
            gj = json.load(open(g1cp))
        except Exception as e:
            print("  n/a（g1c.json 解析失败：%s）" % e)
    if gj:
        nz2 = gj.get("noise") or {}
        if nz2:
            print("  噪声扣除（noise_floor 各 rep 最差，cpu 口径）：p_noise=%s、p999=%sus"
                  % (fmt(nz2.get("p_noise"), "", 2), fmt(nz2.get("p999_noise_us"), "", 3)))
        for case in sorted(gj.get("cases", {})):
            r = gj["cases"][case]
            a = r.get("a") or {}
            b = r.get("b") or {}
            ch = r.get("channels") or {}
            def cell(d):
                if d.get("verdict") in ("N/A", "ERROR") and "over_lo_ratio" not in d:
                    return "%s" % d.get("verdict")
                ov = ("%.2e/%.2e" % (d["over_lo_ratio"], d["over_hi_ratio"])) if "over_lo_ratio" in d else "-"
                p9 = (d.get("criteria") or {}).get("p999", {}).get("verdict", "-")
                mx = fmt(d.get("max_us"), "", 1)
                return "%s n=%s >50us=%s p99.9=%s max=%s" % (d.get("verdict"), d.get("n"), ov, p9, mx)
            bound = r.get("seg_bound_us")
            if isinstance(bound, dict):
                bstr = "%.2f..%.2fus" % (bound["lo"], bound["hi"])
            elif bound is not None:
                bstr = fmt(bound, " us", 2)
            else:
                bstr = "N/A"
            print("  %-17s class=%s  B_seg=%s  ← %s" % (
                case, r.get("class"), bstr, r.get("seg_bound_note") or "-"))
            print("        (a) %s%s" % (cell(a) if a else "n/a",
                  ("  [tau_t=%sus]" % fmt(a.get("per_pop_us"), "", 3))
                  if a.get("per_pop_us") is not None else ""))
            print("        (b) %s" % (cell(b) if b else "n/a"))
            if r.get("n_seg") is not None:
                print("        [segsteps] n_seg=%s pops_tot=%s" % (r.get("n_seg"), r.get("pops_tot")))
            if r.get("class") == "mixed" and a.get("mode") != "mixed":
                print("        (a-mixed) N/A by design（Lead m03564：混合通道插桩不按 n 归因；"
                      "N/A≠PASS）——上面 (a) 行显示 N/A 是这条 + 括号里的原因")
            if a.get("mode") == "mixed":
                print("        (a-mixed) %s" % (a.get("note") or "N/A（混合通道不可判，Lead m03564）"))
                ov = a.get("criteria", {}).get("over50_info", {}).get("why")
                if ov:
                    print("        OBS（不判）%s" % ov)
            for t in (a.get("top_attrib") or [])[:3] if a.get("mode") == "cold" else []:
                print("        [top] kind=%s why=%s cpu_us=%.1f pops=%s seg=%s => tau=%.3fus/pop" % (
                    t.get("kind"), t.get("why"), t.get("cpu_us") or 0.0, t.get("pops"),
                    t.get("seg"), t.get("tau_us") or 0.0))
            ob = a.get("per_pop_obs_all") if a.get("mode") == "cold" else None
            if ob:
                print("        OBS（不判）pops>=1 最慢步：kind=%s why=%s cpu_us=%.1f pops=%s seg=%s "
                      "=> %.3fus/pop%s" % (
                          ob.get("kind"), ob.get("why"), ob.get("cpu_us") or 0.0, ob.get("pops"),
                          ob.get("seg"), ob.get("tau_us") or 0.0,
                          "（pops=1 & seg>0 续扫段 => 归 (b)）" if ob.get("is_seg_resume") else ""))
            if ch:
                print("        (c) " + "；".join(
                    "%s=%s(n=%s,max=%s)" % (k, v.get("verdict"), v.get("n"), fmt(v.get("max_us"), "", 1))
                    for k, v in ch.items()))
            for w in (r.get("warnings") or []):
                print("        ⚠ %s" % w)
        print("  ⇒ G1②（通道拆分）最差：**%s**" % gj.get("verdict"))
        if gj.get("verdict") == "N/A":
            print("     N/A ≠ PASS：缺 [segsteps] 行 / 该通道未触发 / n<1e5 时不得记绿")
    else:
        print("  n/a（未跑；run.sh g1c 或 all。注意：上面 ②/③ 行是混合分母的 legacy 读数，不作判据）")
    print()
    chan = [p for p in pauses if p.get("chan")]
    if chan:
        print("  通道分布（[chan] 按 case+clock+name 合并 reps；看 release/regionfree/collect 谁贡献尾巴）：")
        agg = {}
        for p in chan:
            for c in (p.get("chan") or []):
                k = (p["case"], p.get("clock", "wall"), c.get("name", ""))
                a = agg.setdefault(k, {"n": 0.0, "max": 0.0, "tot": 0.0, "reps": 0})
                a["n"] += c.get("n") or 0
                a["max"] = max(a["max"], c.get("max_us") or 0)
                a["tot"] += c.get("total_us") or 0
                a["reps"] += 1
        for (case, clock, name), a in sorted(agg.items()):
            print(f"    {case:4s} clock={clock:4s} {name:12s} n={a['n']:.0f}（{a['reps']} reps 合并）"
                  f" max={a['max']:.1f}us total={a['tot']:.1f}us")
    print()
    # ---------------- 交叉量具 ----------------
    tm = os.path.join(out, "timemax.txt")
    if os.path.exists(tm):
        print("## 交叉量具（/usr/bin/time -f %M；父进程小，ru_maxrss 不被 python 污染）")
        for line in open(tm):
            parts = line.split()
            if len(parts) >= 4:
                print(f"  {parts[0]:8s} args={' '.join(parts[1:-2]):10s} maxrss={parts[-2]} kB wall={parts[-1]} s")
        print()
    print("## 说明")
    print("  · VmHWM 取 /proc/<pid>/status 的 VmHWM（含采样末值），不是 ru_maxrss（WSL2 有 ~11 MB 地板）。")
    print("  · 每个数是三次的中位数；原始时间序列在 csv/，stdout/stderr 在 log/。")
    print("  · A/B 对照必须用同一台机器、同一个编译器目录（build-emp/extc）与同一组开关。")

def _selftest():
    """量具自检（Lead m02523 要求）：直方图边界与 G1① 判定必须自身可测。

    - 构造行含 0.5ms（b1ms）/1.5ms（b4ms）/70ms（bge64ms）各 1 ⇒ over1ms 必须=2、G1① 判 RED；
    - 全 0 桶 ⇒ over1ms=0、G1① 判 PASS；
    - 旧 7 桶行（bge1ms=2）⇒ legacy 归一化后 over1ms=2、G1① 判 RED；
    - 尾部落在 bge64ms ⇒ p99.9 报 >64ms（inf），不得当成小值。
    """
    bad = []
    names = " ".join(pp.HISTO_NAMES)
    v4 = ("n=3 b1us=0 b2us=0 b4us=0 b8us=0 b16us=0 b32us=0 b64us=0 b128us=0 "
          "b256us=0 b512us=0 b1ms=1 b4ms=1 b16ms=0 b64ms=0 bge64ms=1")
    zero = "n=0 " + " ".join(f"{n}=0" for n in pp.HISTO_NAMES)
    legacy = "n=10 b1us=5 b4us=3 b16us=0 b64us=0 b256us=0 b1ms=0 bge1ms=2"
    cases = [("v4 构造行(0.5ms/1.5ms/70ms)", v4, 2, "RED"),
             ("v4 全 0", zero, 0, "PASS"),
             ("旧 7 桶 legacy(bge1ms=2)", legacy, 2, "RED")]
    for label, line, want_o, want_v in cases:
        o, z, src, _ = over1ms_authoritative([{"histo": line, "over1ms": 0, "step_n": 3}])
        v = g1_c1_verdict(o)
        ok = (o == want_o and v == want_v)
        print(f"  {label}: over1ms={o} src={src} ⇒ G1① {v}（期望 {want_o}/{want_v}）")
        if not ok:
            bad.append(label)
    # 尾部桶 ⇒ p99.9 = inf（>64ms），G1③ 必须 RED 而不是当成小值
    z = normalize_histo_counts([v4])
    p999 = q_upper_us(z["counts"], z["n"], 0.999)
    print(f"  尾部桶分位: p99.9={p999}（期望 inf ⇒ 报 >64000us）")
    if p999 != float("inf"):
        bad.append("尾部桶分位")
    print(f"  over1ms 语义: 桶表 {pp.HISTO_NB} 桶，over1ms=sum(counts[{pp.OVER1MS_FROM}:])（严格 >1ms）")
    if bad:
        print("  ⇒ SELFTEST FAIL: " + "; ".join(bad))
        return 1
    print("  ⇒ SELFTEST PASS")
    return 0

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(_selftest())
    main()