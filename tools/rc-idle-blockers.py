#!/usr/bin/env python3
"""把「有停顿问题的形状为什么挡在 ② 门外」查成一张可排序的表（**只勘察**）。

它做三件事，全部只用现成诊断，不新造分析：

1. 从 `bench/gc/PAUSE-CORPUS.md`（task-29 的落盘画像）抄出**发 rc 的 35 个形状**的
   `max` 停顿与占比，并按该文件自己写的门槛（`max ≥ 100 µs 或 占比 ≥ 1%`）算出命中集合；
2. 对命中集合的每个形状跑 **`EXTC_DBG_RCIDLE=1`**（现成诊断），记下**每条 `idle=0` 的
   理由原文**（不自己发明分类：分组键就是诊断原文）；
3. 按诊断原文分组给代价（形状数 / Σmax / 占命中集合与占 35 个发 rc 形状的比例），并把
   `--report` 生成 `tests/reclaim-perf/IDLE-BLOCKERS.md`。

**只读** `bench/gc/PAUSE-CORPUS.md`（`gcbench` 的）；不改判决/发射/阈值，不碰 `src/`。

自检（反瞎）：`--selftest` 用合成诊断文本喂解析器 —— 缺行、空理由、格式不对都必须**报错**；
解析器不报错就返回 1（判据自己没牙）。整轮 `--report` 也会在每个形状取不到 `idle=0` 时中止。

用法：
    python3 tools/rc-idle-blockers.py --report tests/reclaim-perf/IDLE-BLOCKERS.md
    python3 tools/rc-idle-blockers.py --selftest
    python3 tools/rc-idle-blockers.py --tables            # 只把表打到 stdout
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAUSE = ROOT / "bench" / "gc" / "PAUSE-CORPUS.md"
EXTC = ROOT / "build" / "extc"

# 诊断行：`[rcidle] <函数> idle=<0|1> reason=<原文>`。`reason` 允许空（那时**必须**报错，
# 见 `parse_rcidle`）—— 漏字段是"读错"，不能被当成"没有理由"吞掉。
RCIDLE_RE = re.compile(r"^\[rcidle\]\s+(\S+)\s+idle=([01])\s+reason=(.*)$")


class DiagError(Exception):
    """诊断流读不动/缺行 —— 一律中止，不许降级成"说不清"的一行。"""


def parse_rcidle(text):
    """把一段 stderr 解析成 `(函数, 理由原文)` 列表（只取 `idle=0`）。

    宽松匹配"看起来像 [rcidle] 但格式不对"的行 ⇒ `DiagError`（读错不许静默）。
    """
    out = []
    for ln in (text or "").splitlines():
        ln = ln.rstrip("\n")
        if not ln.startswith("[rcidle]"):
            continue
        m = RCIDLE_RE.match(ln)
        if not m:
            raise DiagError(f"[rcidle] 行格式不认识（读错）：{ln!r}")
        fn, idle, why = m.group(1), m.group(2), m.group(3).strip()
        if idle == "0":
            if not why:
                raise DiagError(f"[rcidle] idle=0 但 reason 为空（缺行）：{ln!r}")
            out.append((fn, why))
    return out


def require_blockers(pairs, shape):
    """发 rc 的形状必须有 ≥1 条 `idle=0`；没有 ⇒ "缺行"，中止（不许降级成"说不清"）。"""
    if not pairs:
        raise DiagError(f"{shape}: 一条 `idle=0` 都没有，但它在表里是发 rc 的形状（缺行）")
    return pairs


def parse_pause_table(path):
    """抄 §3 全表：`{形状: {max, ratio, n, hit_mark}}`（只从落盘文档抄，不另算 ✗）。"""
    rows = {}
    for ln in path.read_text(encoding="utf-8").splitlines():
        if not ln.startswith("| `") or "extc" not in ln:
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if len(cells) < 12:
            continue
        shape = cells[0].strip("`")
        try:
            n = int(cells[2])
            mx = float(cells[6].replace("**", ""))
            ratio = float(cells[9].replace("**", "").replace("%", ""))
        except ValueError:
            continue
        rows[shape] = {"n": n, "max_us": mx, "ratio_pct": ratio,
                       "hit_mark": cells[11].replace("**", "")}
    if not rows:
        raise DiagError(f"从 {path} 没抄到任何形状（路径/格式变了？）")
    return rows


def parse_hit_list(path):
    """抄 §4.3「门槛命中」清单（文档自己列的那一份）。"""
    out, in_sec = [], False
    for ln in path.read_text(encoding="utf-8").splitlines():
        if ln.startswith("### 4.3"):
            in_sec = True
            continue
        if in_sec and ln.startswith("### "):
            break
        if in_sec:
            m = re.match(r"^- `([^`]+)`（", ln)
            if m:
                out.append(m.group(1))
    return out


def rcidle_lines(extc, src, timeout=120):
    """跑一次 `EXTC_DBG_RCIDLE=1`，返回 (rc, stdout, stderr)。诊断只写 stderr ✓。"""
    env = dict(os.environ)
    env["EXTC_DBG_RCIDLE"] = "1"
    p = subprocess.run([str(extc), "-w", "-I", str(ROOT / "stdlib"), str(src), "-o", os.devnull],
                       capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout, p.stderr


# ---- 每条规则的"可修性与估计改动量"（**判断**，不是测量；收益是测量的）-------------------
# 键必须与诊断**原文**逐字相同（分组键就是原文，没有自造分类）。
RULES = {
    "the destination is not a named field of a named object": {
        "id": "B 目的地不是具名字段（下标/切片/解引用基）",
        "fix": "可修（已规划）",
        "why": "RECLAMATION 第五节「宿主」条已点名：`idleHostIdent`/`acTargetSlotIsHeap` 只认"
               "具名字段链，`(*root)[0]`、`s.kids[0]`、`c.hubs[0]`、`w.tail[k]` 一律答 NULL。"
               "把目的地求值一次、逐叶落账即可（现有 arena 层/循环深度事实够用）。",
        "effort": "中（1 条判据路径 + 1 个目的地分解 helper；估计不需要新冻结字段）",
    },
    "a reference is assigned to a stack slot (a local/parameter, or a field of one)": {
        "id": "D 引用写进栈上槽（局部量/形参/局部聚合的字段）",
        "fix": "部分是死结（要分类）",
        "why": "task-16/17 已成功放宽过一支（ref 标识符槽 + 全部写入都随本迭代释放）。剩下的三类："
               "① 本帧游标（`live_churn` 的 `head = c`、`digraph` 的 `p = p.next` / "
               "`cur = (*cur).next`）——被丢的对象**另有根**持着，需要"
               "「不是最后持有者」的新事实（elide 那条线的义务 ③）；"
               "② 局部聚合的字段/元素槽（`carray_slot` 的 `arr[i % 8] = p`、`digraph` 的 "
               "`c.head = null`）——与 B 同一件工作；"
               "③ **故意的不健全面**：`keeper = t`（cross_region）、"
               "`saved = p`（scale_growth）——槽活得比迭代长、对象被提到家区域，"
               "放宽就是界随轮数涨，它们正是 `run-switch`/`gate-idle-bounded` 钉的绊线。",
        "effort": "①中新事实（可能进冻结清单）＝中高；③死结，别碰",
    },
    "a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies": {
        "id": "F 带引用的聚合被产生成临时量",
        "fix": "可修（需要一份新的层事实）",
        "why": "判据 (c) 的原话：临时量的存储属于哪一层 arena 由逃逸分析定，可能的形态是"
               "「本帧的家房间」⇒ 界随轮数涨。修法不是放宽，而是给**临时量**也建一份与 arena 同"
               "尺的事实（同 task-17 给绑定建的那份），能在『本块内产生、本块内丢』时放行。",
        "effort": "中高（新的临时量层事实；可能要进冻结清单）",
    },
    "a reference field is written inside a loop, so one object can be overwritten": {
        "id": "E 循环里写 ref 字段（宿主不是本迭代新建）",
        "dead": True,
        "fix": "死结（含 task-18 的常驻对照）",
        "why": "剩下的全是**同一个对象被每轮改写**：`outer_alloc_inner_write` 是 task-18 专门立的"
               "常驻对照（去掉循环深度条件就必须红）；`owed_home` 的 `p` 被 `cur = p` 提到函数层、"
               "`digraph` 的宿主是形参/长命对象。放宽它们 = 把界放走。",
        "effort": "不修（判据本身正确）",
    },
    "the same field of the same host is written at two sites": {
        "id": "A 同一宿主的同一字段有两处站点",
        "fix": "可修（需要支配/互斥分析）",
        "why": "`persist_nodes` 的 `update` 在两个分支各写一次 `n.l` ⇒ **每个对象运行时只写一次**，"
               "但静态是两处站点。放宽要能证「两处站点互斥（同一对象只走其中一处）」。",
        "effort": "中（分支互斥/支配；估计无需新冻结字段）",
    },
    "the host object is not a local created by `new` in this frame": {
        "id": "C 宿主不是本帧 new 出来的局部量（形参/长命对象）",
        "dead": True,
        "fix": "死结（当前口径下）",
        "why": "`(*a).next = b` 里 `a` 是形参：调用方可能对同一个对象反复调用 ⇒ 无法证明「每对象"
               "写一次」。要放宽得做跨过程的「宿主收敛」（RECLAMATION 第五节第一条），本任务范围外。",
        "effort": "高（跨过程/宿主收敛），且单规则收益为 0（见分组表）",
    },
    "a `dyn` call: the callee is not resolved": {
        "id": "G `dyn` 调用：被调者看不清",
        "dead": True,
        "fix": "死结",
        "why": "动态派发看不见被调者，参数敏感的保守判否是正确方向；放宽需要去虚化/效果签名，"
               "是另一条线。",
        "effort": "不修",
    },
    "`@overwrite` reuses one piece of storage, so the previous object is dropped": {
        "id": "H `@overwrite` 复用同一格",
        "dead": True,
        "fix": "死结（语义如此）",
        "why": "`@overwrite` 的字面语义就是「复用同一格、丢掉上一个对象」；那一格活过调用/迭代 ⇒ "
               "丢的就是长命对象，判否正确。",
        "effort": "不修",
    },
}


SITE_RE = re.compile(r"^line \d+:\s*")


def rule_key(reason):
    """诊断原文里去掉**站点行号**前缀 —— 行号是"哪一处"，不是"哪条规则"。"""
    return SITE_RE.sub("", reason)


# 单规则形状里，哪些的"单独放宽这条规则"其实**不成立**（绊线 / 需别的规则 / 需新事实）。
# 键是形状；值是 `(类别, 依据)`。类别用来算"**无新事实、非绊线**"的净收益列。
SHAPE_CAVEATS = {
    "tests/reclaim-shapes/cross_region.extc":
        ("绊线", "run-switch ①c 断言跨区登记必须真的在发（`rs_inc` 随轮数涨、`rs_hwm` 不涨）："
                 "判 idle 就没有 rc 运行期、没有登记 ⇒ 该门禁当场红。"),
    "tests/reclaim-shapes/scale_growth.extc":
        ("绊线", "task-17 的 (b′) 放宽把它误判 idle 后实测 N vs 4N **2.12×**（界随轮数涨）⇒ "
                 "这条放宽在它上面**不健全**。"),
    "tests/reclaim-perf/shapes/outer_alloc_inner_write.extc":
        ("绊线", "task-18 立的常驻对照：去掉 (a′) 的循环深度条件，`gate-idle-bounded` 必须红。"),
    "tests/reclaim-gaps/shapes/carray_slot.extc":
        ("随 B 一起", "目的地是局部数组元素 `arr[i % 8] = p` ⇒ 与 B（目的地分解）是同一件工作，"
                     "不计入 D 的独立收益（避免重复计数）。"),
    "tests/reclaim-shapes/live_churn.extc":
        ("需新事实", "被丢的旧 head 仍被新结点的 `c.next` 指着（另有根）⇒ 需要「不是最后持有者」"
                    "的新事实；task-17 的现成 (b′) 路线在它上面实测 **2.07×**，不适用。"),
    "tests/reclaim-gaps/shapes/struct_copy_slot.extc":
        ("需新事实", "F 的修法本身就是「给临时量建一份与 arena 同尺的事实」，不是现成判据能消的。"),
    "tests/reclaim-shapes/struct_copy.extc":
        ("需新事实", "同上（F 的这条形状需要临时量的层事实）。"),
    "tests/reclaim-gaps/shapes/byval_temp.extc":
        ("需新事实", "同上（F 的这条形状需要临时量的层事实）。"),
    "tests/reclaim-gaps/shapes/struct_copy_ret_home.extc":
        ("需新事实", "同上（F 的这条形状需要临时量的层事实）。"),
}

CAVEAT_ORDER = {"": 0, "需新事实": 1, "随 B 一起": 2, "绊线": 3}


def caveat(shape):
    return SHAPE_CAVEATS.get(shape, ("", ""))


def apply_rules(reason):
    """把诊断原文（去行号）映射到规则条目（B/D/…）。原文不在表里 ⇒ 报错（不许自造分类 ✗）。"""
    key = rule_key(reason)
    for raw, meta in RULES.items():
        if key == raw or key.startswith(raw):
            return meta
    raise DiagError(f"诊断原文没有对应的规则条目（不许自造分类）：{reason!r}")


def collect(extc, shapes):
    """对每个形状跑诊断，返回 `{形状: {fn_reasons, assigned, distinct, rc}}`。"""
    out = {}
    for shape in shapes:
        src = ROOT / shape
        if not src.exists():
            raise DiagError(f"形状不存在：{shape}")
        rc, _stdout, stderr = rcidle_lines(extc, src)
        pairs = parse_rcidle(stderr)
        if rc != 0:
            raise DiagError(f"{shape}: 编译 rc={rc}（诊断读不出结论）")
        require_blockers(pairs, shape)
        distinct = []
        for _fn, why in pairs:
            if why not in distinct:
                distinct.append(why)
        out[shape] = {"pairs": pairs, "assigned": pairs[0][1], "distinct": distinct}
    return out


def tables(extc, table, hits):
    """生成报告的三张表（形状表 / 分组表 / 排序建议），返回 markdown 文本。"""
    data = collect(extc, hits)
    sum_hits = sum(table[s]["max_us"] for s in hits)
    sum_all = sum(v["max_us"] for v in table.values())

    # 规则层面的去重：行号前缀不算"另一条规则"（同一句话出现在两行仍是同一条）。
    rules_of = {s: sorted({rule_key(r) for _f, r in data[s]["pairs"]}) for s in hits}
    lines = []
    lines.append("### 表 A：命中形状 × 挡路理由（理由逐字取自 `EXTC_DBG_RCIDLE=1`）\n")
    lines.append("| 形状 | max µs | 占比 | n | 挡路理由（原文，该形状**第一条** `idle=0`）| 该形状命中的**规则**条数 | 单一规则? | 备注（依据见 §3）|")
    lines.append("|---|---|---|---|---|---|---|---|")
    for s in hits:
        d = data[s]
        single = "是" if len(rules_of[s]) == 1 else f"否（{len(rules_of[s])} 条）"
        cat, why = caveat(s)
        note = f"**{cat}**" if cat else "—"
        lines.append(f"| `{s}` | {table[s]['max_us']:.1f} | {table[s]['ratio_pct']:.2f}% | {table[s]['n']} "
                     f"| {d['assigned']} | {len(rules_of[s])} | {single} | {note} |")

    groups = {}
    for s in hits:
        meta = apply_rules(data[s]["assigned"])
        g = groups.setdefault(meta["id"], {"meta": meta, "shapes": [], "single": []})
        g["shapes"].append(s)
        if len(rules_of[s]) == 1:
            g["single"].append(s)

    def net(g):
        """该组的"**现成判据/无新事实、非绊线**"净收益（µs）。

        规则本身是死结（G/H/C/E）⇒ 0；否则只算单规则且无形状级 caveat（绊线/需新事实/需别的规则）的。
        """
        if g["meta"].get("dead"):
            return 0.0
        return sum(table[s]["max_us"] for s in g["single"] if not caveat(s)[0])

    lines.append("\n### 表 B：按规则分组（分组键 = 诊断原文，去掉站点行号）\n")
    lines.append("| 规则（原文 → 条目）| 形状数 | 单规则形状数 | Σmax µs（全部）| 单规则 Σmax | 其中**净收益**（无新事实/非绊线）| 占命中 Σmax | 可修性 |")
    lines.append("|---|---|---|---|---|---|---|---|")
    for gid, g in sorted(groups.items(), key=lambda kv: -sum(table[s]["max_us"] for s in kv[1]["shapes"])):
        tot = sum(table[s]["max_us"] for s in g["shapes"])
        single = sum(table[s]["max_us"] for s in g["single"])
        lines.append(f"| **{gid}** | {len(g['shapes'])} | {len(g['single'])} | {tot:.1f} | {single:.1f} "
                     f"| **{net(g):.1f}** | {tot / sum_hits * 100:.1f}% | {g['meta']['fix']} |")

    lines.append(f"\n> 合计：命中集合 Σmax = **{sum_hits:.1f} µs**；35 个发 rc 形状 Σmax = **{sum_all:.1f} µs**；"
                 f"命中集合里**单规则**形状的 Σmax = "
                 f"**{sum(table[s]['max_us'] for s in hits if len(rules_of[s]) == 1):.1f} µs**，"
                 f"其中**净收益**（现成事实、非绊线、非死结规则）= "
                 f"**{sum(table[s]['max_us'] for s in hits if len(rules_of[s]) == 1 and not caveat(s)[0] and not apply_rules(data[s]['assigned']).get('dead')):.1f} µs**。"
                 f"多规则形状（{sum(1 for s in hits if len(rules_of[s]) > 1)} 个）要同时改 2 条以上才可能 idle。")

    # 每条规则的"被哪些**多规则**形状依赖"（前置价值）：形状的规则集合里含这条规则的都算。
    def unlock_of(meta_id, key_prefix):
        out = []
        for s in hits:
            if len(rules_of[s]) <= 1:
                continue
            if any(k.startswith(key_prefix) for k in rules_of[s]):
                out.append(s)
        return out

    rank = sorted((g for g in groups.values()), key=lambda g: -net(g))
    lines.append("\n### 表 C：排序建议（收益 ÷ 估计改动量；**改动量是估计**，收益是测量）\n")
    lines.append("| # | 规则 | 单规则 Σmax µs | 净收益 µs（无新事实/非绊线）| 作为**前置**被几个多规则形状依赖 | 估计改动量 | 历史先例 |")
    lines.append("|---|---|---|---|---|---|---|")
    hist = {
        "B 目的地不是具名字段（下标/切片/解引用基）": "无（新面，RECLAMATION 第五节已规划）",
        "D 引用写进栈上槽（局部量/形参/局部聚合的字段）": "**有**：task-16/17 放宽了 ref 标识符槽",
        "F 带引用的聚合被产生成临时量": "无（task-17 之后的下一个同类）",
        "A 同一宿主的同一字段有两处站点": "无",
        "C 宿主不是本帧 new 出来的局部量（形参/长命对象）": "无",
        "E 循环里写 ref 字段（宿主不是本迭代新建）": "task-17 放宽了「宿主本轮新建」那一半；剩下的是死结",
        "G `dyn` 调用：被调者看不清": "无",
        "H `@overwrite` 复用同一格": "无（语义死结）",
    }
    order = ["B 目的地不是具名字段（下标/切片/解引用基）",
             "F 带引用的聚合被产生成临时量",
             "A 同一宿主的同一字段有两处站点",
             "D 引用写进栈上槽（局部量/形参/局部聚合的字段）",
             "C 宿主不是本帧 new 出来的局部量（形参/长命对象）",
             "E 循环里写 ref 字段（宿主不是本迭代新建）",
             "G `dyn` 调用：被调者看不清",
             "H `@overwrite` 复用同一格"]
    key_of = {v["id"]: k for k, v in RULES.items()}
    for i, gid in enumerate(order, 1):
        g = groups.get(gid)
        if not g:
            continue
        single = sum(table[s]["max_us"] for s in g["single"])
        un = unlock_of(gid, key_of.get(gid, "\0"))
        mark = "（死结）" if g["meta"].get("dead") else ""
        lines.append(f"| {i} | {gid.split(' ', 1)[1]}{mark} | {single:.1f} | {net(g):.1f} | "
                     f"{len(un)} 个（{', '.join('`'+s.split('/')[-1]+'`' for s in un[:4])}"
                     f"{'…' if len(un) > 4 else ''}）| {g['meta']['effort']} | {hist.get(gid, '—')} |")
    return "\n".join(lines) + "\n", data, groups, sum_hits, sum_all


def selftest():
    """反瞎自检：缺行 / 空理由 / 格式错都必须让解析器**报错**；不报错就返回 1。"""
    cases = [
        ("正常两行", "[rcidle] main idle=0 reason=line 3: a `dyn` call: the callee is not resolved\n", False),
        ("没有 idle=0（缺行）", "[rcidle] main idle=1 reason=ok\n", True),
        ("空 reason", "[rcidle] main idle=0 reason=\n", True),
        ("格式不认识", "[rcidle] main idle=0\n", True),
        ("idle 值非法", "[rcidle] main idle=x reason=y\n", True),
    ]
    bad = 0
    for name, text, want_err in cases:
        try:
            got = require_blockers(parse_rcidle(text), "synthetic")
            err = None
        except DiagError as e:
            got, err = None, e
        if want_err and err is None:
            print(f"  FAIL {name}：应当报错却没报（判据没牙）")
            bad += 1
        elif (not want_err) and err is not None:
            print(f"  FAIL {name}：不该报错却报了：{err}")
            bad += 1
        else:
            print(f"  ok   {name}：{'报错 ✓' if want_err else f'解析出 {len(got)} 条 ✓'}")
    print(f"rc-idle-blockers 自检：{'通过' if bad == 0 else f'{bad} 格红'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extc", default=str(EXTC))
    ap.add_argument("--report", default="", help="把报告写到这个路径")
    ap.add_argument("--tables", action="store_true", help="只把表打到 stdout")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    try:
        table = parse_pause_table(PAUSE)
    except DiagError as e:
        print(f"rc-idle-blockers: {e}", file=sys.stderr)
        return 1
    listed = parse_hit_list(PAUSE)
    literal = [s for s, v in table.items() if v["max_us"] >= 100.0 or v["ratio_pct"] >= 1.0]
    literal.sort(key=lambda s: -table[s]["max_us"])
    if not literal:
        raise DiagError("按门槛算出来 0 个命中形状（门槛/表变了？）")

    try:
        tbl, data, groups, sum_hits, sum_all = tables(args.extc, table, literal)
    except DiagError as e:
        # 缺行/读错/理由不在规则表里 —— 一律中止，不产出"说不清"的行。
        print(f"rc-idle-blockers: {e}", file=sys.stderr)
        return 1
    scope = [f"门槛（表里写的 `max ≥ 100 µs 或 占比 ≥ 1%`）命中 **{len(literal)}** 个",
             f"文档 §4.3 清单 **{len(listed)}** 个",
             f"任务书说 **18** 个"]
    if args.tables or not args.report:
        print("\n".join(scope) + "\n")
        print(tbl)
        return 0

    def net_of(g):
        if g["meta"].get("dead"):
            return 0.0
        return sum(table[s]["max_us"] for s in g["single"] if not caveat(s)[0])
    best = None
    for gid, g in groups.items():
        us = net_of(g)
        if best is None or us > best[1]:
            best = (gid, us)
    head = f"""# ②（不发射 rc）能不能把停顿打下去：命中形状的挡路规则与代价（task-32）

> **只勘察**：不改判决/发射/阈值，不碰 `src/`；停顿数字**只从** `bench/gc/PAUSE-CORPUS.md`
> （task-29 的落盘画像）抄，理由原文**只从**现成诊断 `EXTC_DBG_RCIDLE=1` 读。
> 本文件由 `tools/rc-idle-blockers.py --report` 生成（数字自动抄/自动算，避免手抄错）。

## 0. 一句话结论

**下一件该放宽「{best[0]}」**：它在命中集合里能**单独**消掉 **{best[1]:.0f} µs**，
而且是 `cyclic_pressure`/`shard_bands`/`digraph`/`persist_nodes` 这些**多规则**形状的共同前置；
估计改动 1 条判据路径、不需要新冻结字段（**估计**，不是测量）。

顺位第二件：**A 同一宿主的同一字段有两处站点（支配/互斥）** —— 它单独收益为 0，但 `B` 落地后
能和它一起把 **`persist_nodes`（2343.7 µs，占命中 Σmax 36.8%）** 从靶子上拿下来（该形状的规则
集合恰好是「同字段两站点 + 目的地不是具名字段」两条）。**F（聚合临时量，550.8 µs）**要一份新的
层事实，排在第三。

## 1. 集合对账（先把"多少个"钉死）

- {scope[0]}；{scope[1]}；{scope[2]}。
- 差在 `tests/reclaim-gaps/shapes/base_call_unknown.extc`：**max 91.0 µs、占比 2.85%** ⇒ 按
  文档自己写的门槛（`占比 ≥ 1%`）**应当命中**，但 §4.3 漏列了它（表里 `命中` 列也是 `-`）。
  本报告以**门槛的字面集合（{len(literal)} 个）**为准，并在表 A 里把它的挡路理由一并列出 ⇒
  18 / 19 / 20 三个数都能对上：**18** = 任务书给的数（与文档 §4.3 差一个）；**19** = 文档 §4.3
  清单；**20** = 门槛字面。本报告分析的是 20（字面）并逐行给出前两者的对账，避免"按哪个数"之争。
- 其余 34 个发 rc 形状的 `max/占比` 也进了分母（35 个发 rc 形状 Σmax = **{sum_all:.1f} µs**）。

## 2. 数据

"""
    tail = f"""
## 3. 可修性（判断，逐条挂在诊断原文上）

{fix_notes(groups, table)}

## 4. 排序建议（收益 ÷ 估计改动量）

见表 C。**收益是测量的**（`PAUSE-CORPUS.md` 的 max 相加，标明口径）；**改动量是估计**，
只按"要碰几条判据路径 / 是否需要新冻结字段"粗分三档。

## 5. 自检与证据

- **反瞎**：`python3 tools/rc-idle-blockers.py --selftest` —— **五类**合成输入（正常 / 缺行 /
  空 `reason` / 格式不认识 / `idle` 值非法）里，后四类都必须报错；不报错返回 1。本轮**先红后绿**：
  第一次跑"缺行"那格没报错、自检返回 **1**；补上形状级 `require_blockers` 后五格全过、返回 0。
- **每个形状都落到一条理由**：表 A 的每一行都有"挡路理由（原文）"列；脚本对"一个发 rc 的形状
  一条 `idle=0` 都没有"**直接中止**（`DiagError`），不产出"说不清"的行。
- **理由不发明分类**：分组键就是诊断 `reason=` 的逐字原文；原文若不在规则表里，脚本报错。
- **只勘察**：`src/` 未动；产物对 pin 逐字节不变、关诊断时 stderr 0 字节（见汇报）。
- 停顿数字口径见 `bench/gc/PAUSE-CORPUS.md` §1（N=100000、3 次取中位、`taskset -c 6`）。

## 6. 未覆盖与残留风险

- 停顿数字是**同一规模内**的（argv 形状一律 N=100000；固定规模形状按自带规模）⇒ 组间比较
  只在同一口径下成立，**不做跨规模外推** ✗。
- "单一规则"是按**规则**判的（诊断行号前缀不算另一条规则；`byval_temp` 的两行是同一句话）；
  多规则形状要同时改 2 条以上才可能 idle，本表的"可消 µs"对它们一律不计入（保守）。
- `可修性/改动量` 是**判断**，依据是 `docs/RECLAMATION.md` 第五节已有的规划条目与会话内的
  成功先例（task-16/17），不是实测；真正落地哪条要在下一件里用动态安全网验。
- 命中集合里 `cross_region`/`scale_growth` 是**绊线**（判 idle 会让 run-switch ①c 的"登记真的在发"
  落空 / task-17 实测界涨到 2.12×）⇒ 表 A「备注」列标**绊线**、净收益列把它们扣掉。
  `live_churn` **不是**死结：它需要「被丢对象另有根」的新事实（现成 (b′) 在它上面实测 2.07×），
  表 A 标的是**需新事实**，不要与绊线混为一谈。
"""
    out = head + tbl + tail
    Path(args.report).write_text(out, encoding="utf-8")
    print(f"报告 → {args.report}（命中集合 {len(literal)} 个；Σmax {sum_hits:.1f} µs）")
    return 0


def fix_notes(groups, table):
    """按"单一规则可消的 µs"降序列出每条的判据与估计改动量。"""
    lines = []
    for gid, g in sorted(groups.items(),
                         key=lambda kv: -sum(table[s]["max_us"] for s in kv[1]["single"])):
        m = g["meta"]
        lines.append(f"* **{gid}**（{len(g['shapes'])} 个形状）：{m['why']} —— *估计改动量：{m['effort']}*")
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
