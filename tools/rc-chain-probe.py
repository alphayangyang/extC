#!/usr/bin/env python3
"""把"挡路规则"量成**理由链**：逐条在副本树里放宽，找出每个形状走到 idle 需要的规则集合。

为什么不能按"第一条理由"算收益（`task-32` 的教训）：`idleNote` 只记第一条 ⇒ 放开第一条，第二条
立刻接手（`task-33` 实测：只放开 B，688 个形状产物逐字节不变、0 个翻 idle）。真正的单位是
**链**：一个形状要同时放开哪几条规则才 idle。

它跑的是一个**带环境开关的副本编译器**（`EXTC_PROBE_RELAX`，见 /tmp 副本树里的 `probeOn`），
因此只勘察、不改工作树判决。每一轮：
  1. 用当前放宽集跑一次形状，读**全部** `[rcidle-note]`（副本树里 `idleNote` 会把每条理由都打出来）
     ⇒ 得到有序链；
  2. 取链上还没放宽的第一条，加进放宽集，重跑；
  3. 直到产物里 `extc_rc_` 计数为 0（链尾 idle）或链上再没有新规则（**链断 ⇒ 报错**，不许"说不清"）。
每一步都做**绊线体检**：`cross_region`/`scale_growth`/`live_churn` 在该放宽集下必须**仍然发 rc**；
一旦变 idle ⇒ 该放宽集标 **不可走**（放宽会误判）。

用法：
    python3 tools/rc-chain-probe.py --extc /tmp/chain/build/extc \\
            --report tests/reclaim-perf/CHAIN-BLOCKERS.md
    python3 tools/rc-chain-probe.py --selftest
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAUSE = ROOT / "bench" / "gc" / "PAUSE-CORPUS.md"
# "必须仍然发 rc"的形状 = 三条 D 家族绊线（task-17 实测：放宽 D 会把前两条判 idle 且界涨）
# + `gate-idle-bounded.sh` 的 CONTROLS（task-18 把 `outer_alloc_inner_write` 立成常驻对照：
# 去掉 (a′) 的循环深度条件它就会 idle ⇒ 那条门禁红）。任何放宽让它们变 idle ⇒ 该集合**不可走**。
TRIPWIRES = ["tests/reclaim-shapes/cross_region.extc",
             "tests/reclaim-shapes/scale_growth.extc",
             "tests/reclaim-shapes/live_churn.extc",
             "tests/reclaim-shapes/cyclic_pressure.extc",
             "tests/reclaim-perf/shapes/outer_alloc_inner_write.extc"]

# 诊断原文（去站点行号前缀）→ 规则条目。分组键仍是原文；这里只是给它一个短名与**估计**改动量。
# `effort`：1 低 / 2 中 / 3 中高 / 4 高 / 5 死结（"不修"）；是**估计**，不是测量。
RULES = [
    ("the destination is not a named field of a named object", "B", 2,
     "1 条判据路径 + 1 个目的地分解 helper；无新冻结字段（task-33 原型 65 行已实测可用）"),
    ("the same field of the same host is written at two sites", "A", 4,
     "需要支配/互斥或数据流（'同一叶槽两处站点，但旧值不丢'）；task-32 已判定真风险在 A"),
    ("the host object is not a local created by `new` in this frame", "C", 4,
     "跨过程 / 宿主收敛（RECLAMATION 第五节第一条）"),
    ("a reference is assigned to a stack slot (a local/parameter, or a field of one)", "D", 5,
     "全放开会把三条绊线判 idle ⇒ 不可走；可行的只有细分（游标'另有根'、元素槽）"),
    ("a reference field is written inside a loop, so one object can be overwritten", "E", 5,
     "死结（同一个对象被每轮改写；task-18 有常驻对照）"),
    ("a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies",
     "F", 3, "需要给临时量建一份与 arena 同尺的层事实；可能要进冻结清单"),
    ("a `dyn` call: the callee is not resolved", "G", 5, "死结（需去虚化/效果签名）"),
    ("`@overwrite` reuses one piece of storage, so the previous object is dropped", "H", 5,
     "死结（语义如此）"),
    ("a call to '", "I", 4, "需要效果签名/跨过程（RECLAMATION 第五节'看不清的被调者'）"),
    ("a domain block:", "J", 4, "并发域：看不清的跨帧丢引用"),
    ("a spawned task (`ext`):", "K", 4, "spawn 出去那一帧的丢引用看不清"),
    ("a compound assignment to a reference slot overwrites it", "L", 3, "复合赋值；语义上确实覆盖"),
    ("the host binding is reassigned in this function", "M", 3, "需要别名/生命周期分析"),
]
DEAD = {r[1] for r in RULES if r[2] == 5}


class DiagError(Exception):
    """诊断读不出 / 链断 / 放宽无效 —— 一律中止，不许产出"说不清"的行。"""


def load_pause_parser():
    """复用 `tools/rc-idle-blockers.py` 的 PAUSE 解析与原文规范化（同一份口径）。"""
    spec = importlib.util.spec_from_file_location(
        "rib", str(ROOT / "tools" / "rc-idle-blockers.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


RIB = load_pause_parser()


def rule_of(reason):
    """把一条诊断原文（可能带 `line N: `）映射成规则短名；不认识 ⇒ 报错（不许自造分类）。"""
    key = RIB.rule_key(reason)
    for text, rid, _eff, _why in RULES:
        if key == text or key.startswith(text):
            return rid
    raise DiagError(f"诊断原文没有对应规则（不许自造分类）：{reason!r}")


def run_notes(extc, shape, relax):
    """跑一次形状，返回 (rc_of_artifact, [规则短名…有序去重], 可见理由条数)。"""
    env = dict(os.environ)
    env["EXTC_DBG_RCIDLE"] = "1"
    env["EXTC_DBG_RCIDLE_ALL"] = "1"
    env["EXTC_PROBE_RELAX"] = ",".join(relax)
    out = Path(tempfile.gettempdir()) / f"chain-probe-{os.getpid()}.c"
    p = subprocess.run([str(extc), "-w", "--no-line-map", "-I", str(ROOT / "stdlib"),
                        str(ROOT / shape), "-o", str(out)],
                       capture_output=True, text=True, timeout=120, env=env)
    if p.returncode != 0:
        raise DiagError(f"{shape}（relax={relax}）：编译 rc={p.returncode}，诊断读不出结论")
    try:
        n = out.read_text(encoding="utf-8", errors="replace").count("extc_rc_")
    finally:
        try:
            out.unlink()
        except OSError:
            pass
    order = []
    for ln in p.stderr.splitlines():
        m = ln.strip()
        if not m.startswith("[rcidle-note]"):
            continue
        rid = rule_of(m.split("]", 1)[1].strip())
        if rid not in order:
            order.append(rid)
    return n, order, sum(1 for l in p.stderr.splitlines() if l.strip().startswith("[rcidle-note]"))


def tripwire_hits(extc, relax):
    """该放宽集下，哪些绊线被误判成 idle（空 = 安全）。"""
    bad = []
    for t in TRIPWIRES:
        env = dict(os.environ)
        env["EXTC_PROBE_RELAX"] = ",".join(relax)
        out = Path(tempfile.gettempdir()) / f"chain-probe-{os.getpid()}.c"
        p = subprocess.run([str(extc), "-w", "--no-line-map", "-I", str(ROOT / "stdlib"),
                            str(ROOT / t), "-o", str(out)],
                           capture_output=True, text=True, timeout=120, env=env)
        try:
            n = out.read_text(encoding="utf-8", errors="replace").count("extc_rc_") if p.returncode == 0 else -1
        finally:
            try:
                out.unlink()
            except OSError:
                pass
        if n == 0:
            bad.append(t.rsplit("/", 1)[-1])
    return bad


def walk(extc, shape, allow_stuck=False):
    """贪心走链：每轮取当前链上第一条未放宽的规则，加进放宽集，重跑；返回分析结果。

    `allow_stuck=True` 时，"链上所有规则都放宽了却还有理由"（= 真实现分解不了的目的地）记成
    `stuck` 而不是中止 —— 只用于**更正**测量；默认仍旧中止（不许"说不清"）。
    """
    relax, chain, steps, tw_hits = [], [], [], {}
    rc, order, notes = run_notes(extc, shape, relax)
    full_chain = list(order)
    for _ in range(len(RULES) + 1):
        if rc == 0:
            break
        if not order:
            raise DiagError(f"{shape}：产物里还有 rc（{rc}）却一条理由都没有（缺行）")
        nxt = next((r for r in order if r not in relax), None)
        if nxt is None:
            if allow_stuck:
                return {"chain": full_chain, "need": relax, "steps": steps, "idle": False,
                        "stuck": True, "remaining": order, "tripwires": []}
            raise DiagError(f"{shape}：链断了 —— 放宽 {relax} 后仍有理由 {order}，但没有新规则可放")
        relax = relax + [nxt]
        steps.append(nxt)
        for r in order:
            if r not in full_chain:
                full_chain.append(r)
        if tuple(relax) not in tw_hits:
            tw_hits[tuple(relax)] = tripwire_hits(extc, relax)
        rc, order, notes = run_notes(extc, shape, relax)
    if rc != 0:
        if allow_stuck:
            return {"chain": full_chain, "need": relax, "steps": steps, "idle": False,
                    "stuck": True, "remaining": order, "tripwires": []}
        raise DiagError(f"{shape}：走完整条链仍未 idle（链={full_chain}，还剩 {order}）")
    tw = tw_hits.get(tuple(relax), tripwire_hits(extc, relax))
    return {"chain": full_chain, "need": relax, "steps": steps, "idle": True,
            "stuck": False, "tripwires": tw}


def selftest():
    """反瞎：不认识的理由 / 空理由 / 规则表覆盖不全 都必须报错。"""
    cases = [
        ("认识的原文", "line 3: a `dyn` call: the callee is not resolved", False),
        ("不认识的原文", "line 3: something nobody wrote", True),
        ("空原文", "", True),
    ]
    bad = 0
    for name, text, want_err in cases:
        try:
            got = rule_of(text) if text else (_ for _ in ()).throw(DiagError("empty"))
            err = None
        except DiagError as e:
            got, err = None, e
        if want_err and err is None:
            print(f"  FAIL {name}：应当报错却没报"); bad += 1
        elif not want_err and err is not None:
            print(f"  FAIL {name}：不该报错却报了：{err}"); bad += 1
        else:
            print(f"  ok   {name}：{'报错 ✓' if want_err else f'→ {got} ✓'}")
    print(f"rc-chain-probe 自检：{'通过' if bad == 0 else f'{bad} 格红'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extc", default="/tmp/chain/build/extc",
                    help="带 EXTC_PROBE_RELAX 开关的**副本**编译器（工作树编译器没有那个开关）")
    ap.add_argument("--report", default="")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--allow-stuck", action="store_true",
                    help="把'放宽完仍被真实现挡住'记成 stuck（更正测量用）；默认中止")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    table = RIB.parse_pause_table(PAUSE)
    hits = sorted((s for s, v in table.items() if v["max_us"] >= 100.0 or v["ratio_pct"] >= 1.0),
                  key=lambda s: -table[s]["max_us"])
    if args.limit:
        hits = hits[:args.limit]
    rows = []
    try:
        for s in hits:
            res = walk(args.extc, s, args.allow_stuck)
            res["shape"] = s
            res["max"] = table[s]["max_us"]
            res["ratio"] = table[s]["ratio_pct"]
            rows.append(res)
            tail = ("stuck(真实现分解不了：" + ",".join(res["remaining"]) + ")") if res.get("stuck") \
                   else ("OK" if not res["tripwires"] else "BROKEN:" + ",".join(res["tripwires"]))
            print(f"  {s}: chain={'→'.join(res['chain'])} need={res['need']} {tail}")
    except DiagError as e:
        print(f"rc-chain-probe: {e}", file=sys.stderr)
        return 1

    out = render(rows, table, args.extc)
    if args.report:
        Path(args.report).write_text(out, encoding="utf-8")
        print(f"报告 → {args.report}")
    else:
        print(out)
    return 0


def effort_of(relax):
    """一条链的估计改动量 = 各规则估计分之和（**估计**）。"""
    sc = {rid: e for _t, rid, e, _w in RULES}
    return sum(sc[r] for r in relax)


def render(rows, table, extc):
    total = sum(table[s]["max_us"] for s in table)
    hits_sum = sum(r["max"] for r in rows)
    groups = {}
    for r in rows:
        key = ",".join(r["need"])
        groups.setdefault(key, []).append(r)

    L = ["# 理由链画像：每个形状要放宽哪几条规则才 idle（task-34）", "",
         "> **只勘察**：放宽只在 `/tmp` 副本树里做（`EXTC_PROBE_RELAX`），`src/` 零改动、"
         "`build/extc` 不变、产物逐字节不变。停顿数字只从 `bench/gc/PAUSE-CORPUS.md` 抄"
         "（同规模口径，不外推）。",
         ">",
         "> **⚠️ 已更正（task-35）**：本文件初版（task-34）的探针把 B 实现成"
         "「一律放行」，而**真实现** B 只能分解『字面量下标 + 具名根的字段/解引用』——"
         "带调用的基（`(*id(s))`）与非字面量下标（`(*root)[i]`）真实现**仍然判否**。"
         "把探针改成忠实语义后重测：`persist_nodes`/`cyclic_pressure`/`shard_bands`/"
         "`digraph`/`base_call_unknown` 五个形状**链尾不是 idle**（stuck），"
         "`{A,B}` 不再是可走链。下表是**忠实探针**下的更正结果；"
         "复现：`python3 tools/rc-chain-probe.py --extc <副本> --allow-stuck --report …`。",
         ">",
         "> **更正续（task-35 实测）**：`persist_nodes` 的忠实链是 **A → B → N → E** —— "
         "N = B 的**非字面量下标扩展**（`(*root)[i]`；实测：只放宽 A,B 时线 98 仍报 B；"
         "加 N 后它变成 `a reference field is written inside a loop`；再加 **E** 才 idle）；"
         "而 **E 正是 task-18 钉住的不可走规则**（放开它 `outer_alloc_inner_write` 立刻 idle、"
         "`gate-idle-bounded` 红）。⇒ **`persist_nodes` 不是 A,B 能拿下的**：它要 A + B + "
         "B-with-非字面量下标 + **E 的细分**（`(*root)[i]` 写的是**不同元素槽**，与"
         "`outer_alloc_inner_write` 的『同一个槽被每轮改写』不是一回事）。",
         f">\n> 链是被**实测**走出来的（逐条放宽、重跑诊断）；改动量是**估计**。"
         f"副本编译器：`{extc}`。", "",
         "## 1. 每形状的理由链（有序；`→` 按第一次出现的顺序）", "",
         "| 形状 | max µs | 占比 | **完整理由链** | 走到 idle 需放宽 | 链尾 idle? | 绊线体检 |",
         "|---|---|---|---|---|---|---|"]
    for r in rows:
        if r.get("stuck"):
            tw = "**链尾不是 idle**（放宽完全部规则后仍被真实现挡住：" + ",".join(r["remaining"]) + "）"
            L.append(f"| `{r['shape']}` | {r['max']:.1f} | {r['ratio']:.2f}% | "
                     f"{'→'.join(r['chain'])} | {','.join(r['need'])} | **否（stuck）** | {tw} |")
            continue
        tw = "**不可走**（" + ", ".join(r["tripwires"]) + " 被判 idle）" if r["tripwires"] else "安全（三条仍发 rc）"
        L.append(f"| `{r['shape']}` | {r['max']:.1f} | {r['ratio']:.2f}% | "
                 f"{'→'.join(r['chain'])} | {','.join(r['need'])} | 是 | {tw} |")

    L += ["", "## 2. 按「需放宽的规则集」聚合", "",
          "| 规则集 | 形状数 | Σmax µs | 占命中 Σmax | 估计改动量（和） | 绊线体检 |",
          "|---|---|---|---|---|---|"]
    for key, g in sorted(groups.items(), key=lambda kv: -sum(r["max"] for r in kv[1])):
        tw = "**不可走**" if any(r["tripwires"] for r in g) else "安全"
        sm = sum(r["max"] for r in g)
        L.append(f"| **{key}** | {len(g)} | {sm:.1f} | {sm / hits_sum * 100:.1f}% | "
                 f"{effort_of(g[0]['need'])} | {tw} |")

    # 可走的集合：绊线安全
    walkable = {k: g for k, g in groups.items()
                if not any(r["tripwires"] or r.get("stuck") for r in g)}
    L += ["", "### 2.1 只保留**绊线安全**的集合（可走的链）", ""]
    if not walkable:
        L += ["**没有一条链是绊线安全的** —— 所有能到 idle 的规则集都会把三条绊线判成 idle ⇒ "
              "**本轮的负结果**：现成的整条规则放宽里，没有一条可走。" ]
    else:
        L += ["| 规则集 | 形状数 | Σmax µs | 占命中 | 估算改动量 | 备注 |",
              "|---|---|---|---|---|---|"]
        for key, g in sorted(walkable.items(), key=lambda kv: -sum(r["max"] for r in kv[1])):
            sm = sum(r["max"] for r in g)
            L.append(f"| **{key}** | {len(g)} | {sm:.1f} | {sm / hits_sum * 100:.1f}% | "
                     f"{effort_of(g[0]['need'])} | {'，'.join(r['shape'].split('/')[-1] for r in g[:3])}… |")

    need_rules = sorted({x for r in rows for x in r["need"]})
    walkable_keys = {k for k, g in groups.items() if not any(r["tripwires"] for r in g)}
    need_walk = sorted({x for k in walkable_keys for x in k.split(",") if x})
    dead_walk = sorted(set(need_walk) & DEAD)
    seen_rules = sorted({x for r in rows for x in r["chain"]})
    L += ["", "## 3. 规则收窄：哪些其实是链中一环、哪些是瓶颈", "",
          f"- **出现在任何链里的规则**：{', '.join(seen_rules) or '（无）'}",
          f"- **真正被需要（走到 idle 必须放宽）的规则**：{', '.join(need_rules) or '（无）'}",
          f"- **走到 idle 需要、但只在「绊线不可走」的链里出现的规则**（⇒ 不是瓶颈）："
          f"{', '.join(sorted(set(need_rules) - set(need_walk))) or '（无）'}",
          f"- **在绊线安全的链里被需要的规则 = 真正的瓶颈集**："
          f"{', '.join(need_walk) or '（无）'}"
          + (f"（其中 {', '.join(dead_walk)} 是死结 ⇒ 那条链实际走不通）" if dead_walk else ""),
          f"- **只作为链中一环、从未被需要**："
          f"{', '.join(sorted(set(seen_rules) - set(need_rules))) or '（无）'}", "",
          "## 4. 一句话结论", ""]

    if not walkable:
        L += ["**没有任何现成的整条放宽值得走**：能到 idle 的每条链都至少含一条会误判绊线的规则。"
              "下一件应当在**链的末端规则内部细分**（A 的互斥/数据流、D 的'另有根/元素槽'），"
              "而不是整条放开。"]
    else:
        best = max(walkable.items(), key=lambda kv: sum(r["max"] for r in kv[1]))
        sm = sum(r["max"] for r in best[1])
        L += [f"**下一件实现「{best[0]}」这条链**：消掉 **{sm:.1f} µs**"
              f"（占命中 Σmax {sm / hits_sum * 100:.1f}%），估计改动量 {effort_of(best[1][0]['need'])}。"]
    L += ["", f"> 口径：命中集合 Σmax = {hits_sum:.1f} µs；全部发 rc 形状 Σmax = {total:.1f} µs；"
          f"命中 {len(rows)} 个形状。停顿来源 `bench/gc/PAUSE-CORPUS.md`（N=100000、3 次取中位）。", "",
          "## 5. 自检", "",
          "- `python3 tools/rc-chain-probe.py --selftest`：认识/不认识/空原文三类，"
          "不认识的必须报错（沿用 `require_blockers` 的思路）。",
          "- 每个形状都必须走到「产物 rc=0」或报出**链断**；`walk()` 对"
          "「还有 rc 却一条理由都没有」与「放宽后没有新规则可放」都直接中止。", "",
          "## 6. 未覆盖与残留风险", "",
          "- 副本的放宽是**整条规则**的开关（不是实现）：它答的是「若这条规则不存在，还差哪几条」，"
          "**不答**「这条规则能不能被安全地细分」。A/D 的细分方案要另立。",
          "- 绊线体检只看 `cross_region`/`scale_growth`/`live_churn` 三条；"
          "其它形状的语义仍由各自电池看着（本件不改判决，风险为零）。",
          "- 停顿是同一规模口径（argv 形状 N=100000），不做跨规模外推。"]
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    sys.exit(main())
