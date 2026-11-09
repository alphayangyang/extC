#!/usr/bin/env python3
"""tests/gc-p0/pause_parse.py —— 解析 EXTC_RC_PAUSE=1 的 stderr 摘要，输出一个 JSON。

口径演进（详见 MEASUREMENTS-GC-P0.md §10）：
- 旧插桩（FREEZE v3 及更早）只有 max/total + over16/64/256/1ms 累计桶，p50/p99 结构性不可得。
- v4 定版 `[histo]`（Lead m02523 口径，与 src/back/reclaim.c 的 extc_rc_stepBucket edges 一致）：
  EXTC_RC_STEP_NB=15，桶名 b1us b2us b4us b8us b16us b32us b64us b128us b256us b512us
  b1ms b4ms b16ms b64ms bge64ms；bn = 该桶**上界**（ns 见 HISTO_EDGES_NS），
  bge64ms 是 >64ms 开区间；**over1ms = 严格 >1e6 ns 的桶和（b4ms 起），不是「最后一个桶」**。
  本脚本按这张表**硬编码**解析（不再用通用正则猜测桶名，避免 b1ms/b1024us 之类串味）。
  旧 7 桶产物（含 bge1ms）走 legacy 归一化：上界 µs→ns 查表映射，bge1ms 计入 over1ms。
- `[clock]` 行（:3226-3229）自证口径：step/pause/chan = cpu|wall，strict-ANSI 下 cpu 不可用时带
  `cpu=unavailable`（此时运行时会退回墙钟，summarize.py 会做 effective-clock 修正）。
"""
import json, re, sys

# ---------------------------------------------------------------------------
# v4 定版 [histo] 桶表（Lead m02523 给定口径；与 src/back/reclaim.c 的
# extc_rc_stepBucket 的 edges 数组一一对应）。**不要改成通用解析**：
# 通用正则会把 b1ms 当成 1000us 并和 b1024us 混排（v4 在途版本曾出现 b1024us/b4096us/b1ms/
# bge1ms 与 edges 错位，导致 over1ms 只数 ≥1s ⇒ G1① 恒真假 PASS，见 MEAS-GC-P0 §10）。
# ---------------------------------------------------------------------------
HISTO_EDGES_NS = [1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000,
                  256000, 512000, 1000000, 4000000, 16000000, 64000000]
HISTO_NAMES = ["b1us", "b2us", "b4us", "b8us", "b16us", "b32us", "b64us", "b128us",
               "b256us", "b512us", "b1ms", "b4ms", "b16ms", "b64ms", "bge64ms"]
OVER1MS_FROM = 11          # b4ms 起：严格 >1e6 ns（1ms 桶本身是 <=1e6 ns，不算）
HISTO_NB = len(HISTO_NAMES)  # 15 == EXTC_RC_STEP_NB


def parse_histo_line(line):
    """按 v4 桶表解析一行 [histo]。返回 counts[15] / over1ms / buckets(上界 µs)。"""
    hm = re.search(r"\bn=(\d+)", line)
    vals = {}
    for name, cnt in re.findall(r"\b([A-Za-z][A-Za-z0-9]*)=(\d+)", line):
        vals[name] = int(cnt)
    counts = [vals.get(n, 0) for n in HISTO_NAMES]
    return {"n": int(hm.group(1)) if hm else None,
            "counts": counts,
            "over1ms": sum(counts[OVER1MS_FROM:]),
            # 第 i 桶上界 = edges[i]（i<14）；最后一个 bge64ms 无上界（记 None）
            "buckets": [[(HISTO_EDGES_NS[i] / 1000.0) if i < len(HISTO_EDGES_NS) else None,
                         counts[i]] for i in range(HISTO_NB)],
            "ge64ms": counts[-1],
            "table": "v4-led-m02523"}


def _selftest():
    """量具自身可测：0.5ms→b1ms、1.5ms→b4ms、70ms→bge64ms ⇒ over1ms=2。"""
    bad = []
    names = " ".join(HISTO_NAMES)
    line = ("n=3 b1us=0 b2us=0 b4us=0 b8us=0 b16us=0 b32us=0 b64us=0 b128us=0 "
            "b256us=0 b512us=0 b1ms=1 b4ms=1 b16ms=0 b64ms=0 bge64ms=1")
    h = parse_histo_line(line)
    if h["counts"][10] != 1:
        bad.append("0.5ms 未落 b1ms")
    if h["counts"][11] != 1:
        bad.append("1.5ms 未落 b4ms")
    if h["counts"][14] != 1:
        bad.append("70ms 未落 bge64ms")
    if h["over1ms"] != 2:
        bad.append(f"over1ms={h['over1ms']} 期望 2")
    zero = parse_histo_line("n=0 " + " ".join(f"{n}=0" for n in HISTO_NAMES))
    if zero["over1ms"] != 0:
        bad.append(f"全 0 桶 over1ms={zero['over1ms']} 期望 0")
    # [quota] 两类行（Lead m03775）：hits 只取终值行 total，per-round 不得覆盖（doc-audit N-2）
    qtxt = ("[quota] round=1 round_hits=3 round_checks=7\n"
            "[quota] round=2 round_hits=9 round_checks=11\n"
            "[quota] deadline_us=800 max_mark=1048576 shift=3 checks=18 hits=12\n")
    qrec = parse_text(qtxt)["quota"]
    if qrec.get("hits") != 12:
        bad.append("quota hits=%r 期望 12（终值行 total，不被 per-round 覆盖）" % (qrec.get("hits"),))
    if qrec.get("deadline_us") != 800:
        bad.append("quota deadline_us=%r 期望 800（g18 天花板要用它）" % (qrec.get("deadline_us"),))
    if qrec.get("checks") != 18:
        bad.append("quota checks=%r 期望 18" % (qrec.get("checks"),))
    if qrec.get("round_hits_last") != 9 or qrec.get("round_hits_max") != 9:
        bad.append("quota round_hits_last=%r / max=%r 期望 9"
                   % (qrec.get("round_hits_last"), qrec.get("round_hits_max")))
    oldq = parse_text("[quota] deadline_us=250 hits=4\n")["quota"]
    if oldq.get("hits") != 4 or oldq.get("deadline_us") != 250:
        bad.append("旧格式 [quota] deadline_us/hits 解析失败: %r" % (oldq,))
    print(f"pause_parse selftest: 桶表 {HISTO_NB} 桶 edges_ns={HISTO_EDGES_NS}")
    print(f"  构造行(0.5ms/1.5ms/70ms 各 1): over1ms={h['over1ms']}（期望 2）")
    print(f"  全 0 行: over1ms={zero['over1ms']}（期望 0）")
    print(f"  [quota] 终值 hits={qrec.get('hits')} deadline_us={qrec.get('deadline_us')}"
          f" round_hits_last={qrec.get('round_hits_last')}（期望 12/800/9）")
    if bad:
        print("  ⇒ SELFTEST FAIL: " + "; ".join(bad))
        return 1
    print("  ⇒ SELFTEST PASS")
    return 0


def parse_text(txt):
    """解析一段 EXTC_RC_PAUSE=1 的 stderr 文本，返回 dict（供 g20.py 等直接 import）。"""
    rec = {"collects": None, "pause_max_us": None, "pause_mean_us": None,
           "step_n": None, "step_max_us": None, "step_mean_us": None,
           "over16": None, "over64": None, "over256": None, "over1ms": None,
           "worst_kind": None, "worst_why": None, "worst_budget": None,
           "worst_dur_us": None, "over1ms_total": None, "guard": None}
    m = re.search(r"^\[pause\] collects=(\d+) max_us=([\d.]+) total_us=([\d.]+)", txt, re.M)
    if m:
        rec["collects"] = int(m.group(1))
        rec["pause_max_us"] = float(m.group(2))
        rec["pause_total_us"] = float(m.group(3))
        rec["pause_mean_us"] = round(float(m.group(3)) / max(int(m.group(1)), 1), 3)
    m = re.search(r"^\[step\] n=(\d+) max_us=([\d.]+) total_us=([\d.]+) "
                  r"over16=(\d+) over64=(\d+) over256=(\d+) over1ms=(\d+)", txt, re.M)
    if m:
        rec["step_n"] = int(m.group(1))
        rec["step_max_us"] = float(m.group(2))
        rec["step_total_us"] = float(m.group(3))
        rec["step_mean_us"] = round(float(m.group(3)) / max(int(m.group(1)), 1), 4)
        rec["over16"], rec["over64"] = int(m.group(4)), int(m.group(5))
        rec["over256"], rec["over1ms"] = int(m.group(6)), int(m.group(7))
    m = re.search(r"^\[worst\] kind=(\S+) why=(\S+) budget=(-?\d+) dur_us=([\d.]+)", txt, re.M)
    if m:
        rec["worst_kind"], rec["worst_why"] = m.group(1), m.group(2)
        rec["worst_budget"], rec["worst_dur_us"] = int(m.group(3)), float(m.group(4))
    m = re.search(r"^\[guard\] r4_wb_open=(\d+)", txt, re.M)
    if m:
        rec["guard"] = int(m.group(1))
    # ---- [work] 原始行 + 解析（P1-2 的 pressure / seg_words / M_max 等新字段都在这一行）----
    # 旧插桩没有这些字段 ⇒ 缺省 None/{}，summarize 侧统一按"未落地"处理。
    rec["work_line"] = None
    rec["work"] = {}
    m = re.search(r"^\[work\][^\n]*", txt, re.M)
    if m:
        rec["work_line"] = m.group(0).strip()
        for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", rec["work_line"]):
            rec["work"][k] = int(v)
    rec["top_line"] = None
    m = re.search(r"^\[top\][^\n]*", txt, re.M)
    if m:
        rec["top_line"] = m.group(0).strip()
    rec["wbudget_line"] = None
    m = re.search(r"^\[wbudget\][^\n]*", txt, re.M)
    if m:
        rec["wbudget_line"] = m.group(0).strip()
    # ---- 2026-11-09 新增插桩（runtime-facts）：[clock]/[histo]/[chan]，旧产物里没有则为空 ----
    # [clock] step=cpu|wall pause=wall|clock|wall chan=cpu|wall [cpu=unavailable]
    # （v4 起 runtime 在 strict-ANSI 且 cpuNow()<0 时打 cpu=unavailable 并退回墙钟，见 MEAS-GC-P0 §7-12）
    rec["clock_line"] = None
    rec["clock_fields"] = {}
    m = re.search(r"^\[clock\][^\n]*", txt, re.M)
    if m:
        rec["clock_line"] = m.group(0).strip()
        for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([A-Za-z0-9_.-]+)", rec["clock_line"]):
            rec["clock_fields"][k] = v
    # [histo]：v4 定版 15 桶，按 HISTO_NAMES 硬编码表解析（见文件顶部与 MEAS-GC-P0 §10）。
    # 旧 7 桶产物（b1us/b4us/.../bge1ms）在 summarize.normalize_histo 里做 legacy 归一化，
    # 本解析器同时保留 buckets(list) 以便旧路径仍能用；over1ms 一律按严格 >1e6 ns 重算。
    rec["histo_line"] = None
    rec["histo"] = None
    m = re.search(r"^\[histo\][^\n]*", txt, re.M)
    if m:
        rec["histo_line"] = m.group(0).strip()
        rec["histo"] = parse_histo_line(rec["histo_line"])
        rec["over1ms_histo"] = rec["histo"]["over1ms"]
        rec["histo_n"] = rec["histo"]["n"]
    rec["chan"] = []
    for cm in re.finditer(r"^\[chan\] ([^\n]*)", txt, re.M):
        body = cm.group(1)
        d = {"name": (re.search(r"name=(\S+)", body).group(1) if re.search(r"name=(\S+)", body) else None)}
        for k, v in re.findall(r"(\w+)=(-?[0-9.eE+]+)", body):
            try:
                d[k] = float(v)
            except ValueError:
                pass
        rec["chan"].append(d)
    # ---- [quota]（P0 配额/deadline 契约，Lead m03564 + m03775）----
    # 两类行（runtime-facts 改造后）：
    #  (a) 终值行 "[quota] deadline_us=800 max_mark=1048576 shift=3 checks=N hits=<total>"
    #      （atexit 摘要；hits 是**总计**）
    #  (b) per-round 行 "[quota] round=<k> round_hits=<r> round_checks=<c>"
    # 规则（Lead m03775，修 doc-audit N-2）：hits **只**从终值行取 total；per-round 只写
    # round_hits/round_checks；两行都出现时**不得**用 per-round 覆盖 total。
    # 旧格式（"[quota] deadline_us=250 hits=N"，reclaim.c:2027 一带）仍兼容（无 round 键 ⇒ 终值行）。
    rec["quota_lines"] = []
    rec["quota"] = {}
    rec["quota_rounds"] = []
    for qm in re.finditer(r"^\[quota\] [^\n]*", txt, re.M):
        ln = qm.group(0).strip()
        rec["quota_lines"].append(ln)
        kv = {k: int(v) for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", ln)}
        if "round" in kv:
            rec["quota_rounds"].append(kv)   # (b) per-round：只留 round/round_hits/round_checks
            continue
        rec["quota"].update(kv)              # (a) 终值行（total）
    if rec["quota_rounds"]:
        last = rec["quota_rounds"][-1]
        rec["quota"]["round_hits_last"] = last.get("round_hits")
        rec["quota"]["round_checks_last"] = last.get("round_checks")
        rec["quota"]["round_hits_max"] = max(
            [r.get("round_hits", 0) for r in rec["quota_rounds"]])
    return rec


def main():
    path = sys.argv[1]
    txt = open(path, "r", errors="replace").read()
    print(json.dumps(parse_text(txt)))

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(_selftest())
    main()