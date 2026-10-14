#!/usr/bin/env python3
"""按"细分"（narrowing）而不是"整条规则"来重扫：每个形状需要哪几条细分才 idle，六条对照是否仍发 rc。

背景（task-34/36 的教训）：`task-34` 的探针把"放宽"实现成**整条规则不存在**，对 5 个形状得出
"stuck"；但 `task-36` 证明真正可做的是**给规则加更窄的条件**（F：只在循环外放行）。本工具测的
就是这些**细分**：

* `NA`  把"同一叶槽两处写"整条当互斥（**上界，不是可信细分** —— 用来量"若这么做会怎样"）；
* `NA2` 只当**被覆盖的旧值另有根**（值是形参的投影、且本帧不写该宿主的字段）时放行；
* `NB`  目的地允许**非字面量下标**（键记 `base[]`；B 从"字面量下标"放宽到"任意下标"）；
* `NE2` 循环里的**元素槽序列**（最外一层是非字面量下标、下标是标识符）不再要求"宿主本迭代新建"；
* `NE`  `NE2` 的上界：任何元素目的地都不再要求（**不可信，仅作对照**）。

用法（需要一个带 `EXTC_PROBE_NARROW` 开关的**副本**编译器，工作树编译器没有这个开关）：
    python3 tools/rc-narrow-probe.py --extc /tmp/narrow/build/extc \\
            --report tests/reclaim-perf/NARROW-PROBE.md
    python3 tools/rc-narrow-probe.py --selftest
"""
import argparse
import importlib.util
import itertools
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAUSE = ROOT / "bench" / "gc" / "PAUSE-CORPUS.md"

# 六条对照：必须**仍然发 rc**（前三条是 D 家族绊线，后三条是门禁里的 CONTROLS）
CONTROLS = [
    "tests/reclaim-shapes/cross_region.extc",
    "tests/reclaim-shapes/scale_growth.extc",
    "tests/reclaim-shapes/live_churn.extc",
    "tests/reclaim-shapes/cyclic_pressure.extc",
    "tests/reclaim-perf/shapes/outer_alloc_inner_write.extc",
    "tests/reclaim-gaps/shapes/byval_temp.extc",
]
# 细分的"可信度"与估计改动量：`trusted=False` 的只作上界/对照，不算兑现。
NARROW = {
    "NA2": (True, 4, "A：只放行『被覆盖的旧值另有根』（值 = 形参的投影、本帧不写该宿主字段）"),
    "NB":  (True, 2, "B：目的地允许非字面量下标（键 `base[]`；同叶槽两处写仍按 A 判）"),
    "NE2": (True, 3, "E：元素槽**序列**（下标是标识符）不再要求『宿主本迭代新建』"),
    "NA":  (False, 0, "A 的上界：两处写一律当互斥（不可信，只量『会怎样』）"),
    "NE":  (False, 0, "E 的上界：任何元素目的地都放行（不可信）"),
}
ORDER = ["NA2", "NB", "NE2", "NA", "NE"]


class DiagError(Exception):
    pass


def load(mod_name, filename):
    spec = importlib.util.spec_from_file_location(mod_name, str(ROOT / "tools" / filename))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


RIB = load("rib", "rc-idle-blockers.py")


def rc_of(extc, shape, narrow):
    env = dict(os.environ)
    env["EXTC_PROBE_NARROW"] = ",".join(narrow)
    out = Path(tempfile.gettempdir()) / f"narrow-probe-{os.getpid()}.c"
    p = subprocess.run([str(extc), "-w", "--no-line-map", "-I", str(ROOT / "stdlib"),
                        str(ROOT / shape), "-o", str(out)],
                       capture_output=True, text=True, timeout=180, env=env)
    if p.returncode != 0:
        raise DiagError(f"{shape}（narrow={narrow}）：编译 rc={p.returncode}")
    try:
        return out.read_text(encoding="utf-8", errors="replace").count("extc_rc_")
    finally:
        try:
            out.unlink()
        except OSError:
            pass


def selftest():
    """反瞎：开关名不认识 / 空开关表都不许静默当"没有细分"。"""
    bad = 0
    if any(k not in NARROW for k in ORDER):
        print("  FAIL 开关表与顺序不一致"); bad += 1
    else:
        print("  ok   开关表：NA2/NB/NE2(可信) + NA/NE(上界) ✓")
    if not any(NARROW[k][0] for k in NARROW):
        print("  FAIL 没有可信细分"); bad += 1
    else:
        print("  ok   至少有一条可信细分 ✓")
    print(f"rc-narrow-probe 自检：{'通过' if bad == 0 else f'{bad} 格红'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extc", default="/tmp/narrow/build/extc")
    ap.add_argument("--report", default="")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    table = RIB.parse_pause_table(PAUSE)
    hits = sorted((s for s, v in table.items() if v["max_us"] >= 100.0 or v["ratio_pct"] >= 1.0),
                  key=lambda s: -table[s]["max_us"])
    # 对照组是否仍发 rc：按组合缓存（每个组合只跑一次）
    ctrl_cache = {}

    def controls_ok(combo):
        key = tuple(combo)
        if key not in ctrl_cache:
            bad = []
            for c in CONTROLS:
                try:
                    if rc_of(args.extc, c, combo) == 0:
                        bad.append(c.rsplit("/", 1)[-1])
                except DiagError as e:
                    raise DiagError(str(e))
            ctrl_cache[key] = bad
        return ctrl_cache[key]

    rows = []
    # 组合按"可信优先 + 条数"排序：先试只用可信细分、条数少的
    combos = []
    for n in range(0, len(ORDER) + 1):
        for c in itertools.combinations(ORDER, n):
            combos.append(list(c))
    combos.sort(key=lambda c: (sum(0 if NARROW[x][0] else 1 for x in c), len(c), c))

    try:
        for s in hits:
            found = None
            for combo in combos:
                if rc_of(args.extc, s, combo) == 0:
                    found = combo
                    break
            if found is None:
                rows.append({"shape": s, "idle": False, "combo": None,
                             "max": table[s]["max_us"], "ratio": table[s]["ratio_pct"]})
                print(f"  {s}: 没有细分组合能让它 idle（含上界 NA/NE）")
                continue
            bad = controls_ok(found)
            rows.append({"shape": s, "idle": True, "combo": found, "ctrl_bad": bad,
                         "max": table[s]["max_us"], "ratio": table[s]["ratio_pct"]})
            print(f"  {s}: idle with {found}  controls={'OK' if not bad else 'BROKEN:' + ','.join(bad)}")
    except DiagError as e:
        print(f"rc-narrow-probe: {e}", file=sys.stderr)
        return 1

    text = render(rows, table, args.extc)
    if args.report:
        Path(args.report).write_text(text, encoding="utf-8")
        print(f"报告 → {args.report}")
    else:
        print(text)
    return 0


def effort(combo):
    return sum(NARROW[x][1] for x in combo)


def render(rows, table, extc):
    hits_sum = sum(r["max"] for r in rows)
    L = ["# 按「细分」重扫：每条细分能安全兑现多少停顿（task-37）", "",
         "> **只勘察**：细分全在 `/tmp` 副本树里（`EXTC_PROBE_NARROW`）；`src/` 零改动、"
         "`build/extc` 不变、产物逐字节不变。停顿数字抄自 `bench/gc/PAUSE-CORPUS.md`（同规模口径，不外推）。",
         f"> 副本编译器：`{extc}`。**每格都是实测**（编译产物数 `extc_rc_`），不是判断。", "",
         "## 0. 细分假设（每条写清「窄在哪里」）", "",
         "| 细分 | 可信 | 估计改动量 | 窄在哪里 |", "|---|---|---|---|"]
    for k in ORDER:
        trusted, eff, why = NARROW[k]
        L.append(f"| `{k}` | {'是' if trusted else '**否（上界）**'} | {eff} | {why} |")
    L += ["",
          "## 1. 每形状：需要哪几条细分才 idle（实测）+ 六条对照体检", "",
          "| 形状 | max µs | 占比 | 需细分 | 细分后 idle | 六条对照仍发 rc |",
          "|---|---|---|---|---|---|"]
    for r in rows:
        if not r["idle"]:
            L.append(f"| `{r['shape']}` | {r['max']:.1f} | {r['ratio']:.2f}% | — | **否** | — |")
        else:
            ok = "是" if not r["ctrl_bad"] else f"**否**（{','.join(r['ctrl_bad'])} 被判 idle）"
            L.append(f"| `{r['shape']}` | {r['max']:.1f} | {r['ratio']:.2f}% | "
                     f"{'+'.join(r['combo']) or '（无需细分：现状）'} | 是 | {ok} |")

    groups = {}
    for r in rows:
        if r["idle"] and not r["ctrl_bad"]:
            groups.setdefault(tuple(r["combo"]), []).append(r)
    L += ["", "## 2. 按细分组合聚合（**只算安全兑现**：对照全部仍发 rc）", "",
          "| 细分组合 | 形状数 | Σmax µs | 占命中 Σmax | 估计改动量 | 形状 |", "|---|---|---|---|---|---|"]
    for combo, g in sorted(groups.items(), key=lambda kv: -sum(r["max"] for r in kv[1])):
        sm = sum(r["max"] for r in g)
        L.append(f"| **{'+'.join(combo) or '（现状）'}** | {len(g)} | {sm:.1f} | "
                 f"{sm / hits_sum * 100:.1f}% | {effort(combo)} | "
                 f"{'，'.join('`' + r['shape'].split('/')[-1] + '`' for r in g[:4])}"
                 f"{'…' if len(g) > 4 else ''} |")
    L += ["", "## 3. 一句话结论", ""]
    if not groups:
        L += ["**没有任何细分能安全兑现**（含上界在内都没形状 idle）—— 负结果。"]
    else:
        # 排序：Σ停顿 ÷ 估计改动量
        def score(kv):
            combo, g = kv
            sm = sum(r["max"] for r in g)
            e = max(1, effort(combo))
            return -(sm / e)
        best = [kv for kv in sorted(groups.items(), key=score) if kv[0]]
        parts = []
        for combo, g in best[:3]:
            sm = sum(r["max"] for r in g)
            parts.append(f"`{'+'.join(combo)}`（Σ {sm:.1f} µs，改 {effort(combo)}）")
        L.append("**下一件实现（按 Σmax ÷ 估计改动量）**：" + "；".join(parts) + "。")
        L.append("")
        L.append("> 收益 = 这些形状的**停顿归零**（②不发射 rc 运行期）；收益数是 `PAUSE-CORPUS.md` 的 "
                 "max 之和，改动量是**估计**。")
    L += ["", "## 4. 未覆盖与残留风险", "",
          "- `NA`/`NE` 是**上界**（把整条条件去掉），只用来标出「最多能到哪」；它们**不是可实现的细分**"
          "（`NA` 会放行真正会丢根的覆盖，`NE` 会放行任意元素目的地）。",
          "- 每条细分的**安全**只由六条对照 + 语料产物不变来判；通用性仍靠 `gate-idle-bounded` 的 "
          "N vs 4N 动态界兜（实现时要把它加进 SHAPES）。",
          "- 只扫了 20 个命中形状；未命中形状的细分收益未量。",
          "- 需要新冻结字段的细分（若有）单列在 §5。", "",
          "## 5. 需要新冻结字段？（影响面待评估）", ""]
    L += ["本轮的细分都只用**只读**事实（作用域深度、形参绑定、目的地表达式形态），"
          "**没有一条需要新冻结字段** —— 若实现时发现要加，先报 Lead 评估。"]
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    sys.exit(main())
