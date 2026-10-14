#!/usr/bin/env python3
# tools/check_gate_wiring.py —— **判据必须从电池出发可达**。
#
# 为什么要有这条判据（而不是靠记性）：`tests/reclaim-shapes/run-switch.sh` 自称门禁、
# `tests/reclaim-shapes/README.md` 也这么写，但它**从来没进 `tools/check.py`**。于是
# "回收不生效 ⇒ 峰值随轮数涨"这一整类回归在 `nobench` 全绿的情况下溜了进来（实测：
# `storeRefOwned` 的宿主兜底让 `rs_inc` 从 0 涨到 46 万且只增不减，四个形状峰值随规模涨）。
# **判据不进电池 = 没有判据。**
#
# 口径（客观、可机检）：
#   ① 从 `tools/check.py` 出发，把每个被引用的脚本当种子，**沿脚本之间的引用做可达性闭包**
#      （脚本里提到别的脚本的 basename 就算引用）；
#   ② 遍历 `tests/**/run*.sh`、`tests/**/gate*`、`tools/**/run*.sh`、`tools/gate*.py`
#      （只算**可执行**文件，库文件如 `gatecommon.py` 不算）：
#        · 可达 ⇒ 通过（在电池里）；
#        · 不可达但文件开头自报 `# manual: <理由>` ⇒ 豁免（理由打印出来，人审）；
#        · 不可达且没自报 ⇒ **红**。
#
# 用法：
#   python3 tools/check_gate_wiring.py
#   python3 tools/check_gate_wiring.py --root DIR --check-py FILE   # 自测/换根
import argparse
import pathlib
import re
import sys
import collections

MANUAL = re.compile(r"^#\s*manual:\s*(\S.*)$", re.M)
SCAN_LINES = 30


def scripts_in(root: pathlib.Path):
    """候选：名字像 run*/gate* 的 `.sh`/`.py` 文件。

    不看可执行位：门禁常用 `python3 x.py` 调（无 +x）—— 只看位会把这类**引用**漏掉，
    于是 `gate.py → run-gaps.sh` 这条链断开、判据误报（实测踩过）。库文件（如
    `gatecommon.py`）靠"被 import 到"自然落在可达闭包里。"""
    out = []
    for sub in ("tests", "tools"):
        base = root / sub
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file():
                continue
            n = p.name
            if not n.endswith((".sh", ".py")):
                continue
            if n.startswith("run") or n.startswith("gate"):
                out.append(p)
    return out


INVOKE_PREFIX = ("./", "tests/", "tools/", "bash", "sh", "python3", "python", "env")


def invoked(text: str, names, py: bool = False):
    """文本里**调用了**哪些脚本（只看非注释行，且要像调用而不只是提及）。

    为什么不用"出现过这个名字"：`run-switch.sh` 当年被别处的注释/输出字符串提到过好几次，
    按提及判就会把"没接线"判成"接线" —— 判据必须认**调用形态**：
    token 以 `./` / `tests/` / `tools/` 开头，或前一个 token 是 `bash`/`sh`/`python3`/`env`。
    """
    hit = set()
    for line in text.split("\n"):
        st = line.strip()
        if st.startswith("#"):
            continue
        toks = st.replace('"', " ").replace("'", " ").replace("(", " ").replace(")", " ").split()
        for i, t in enumerate(toks):
            if t.startswith("-"):
                continue
            base = t.rsplit("/", 1)[-1]
            # Python 里 `import gatecommon` 用的是**模块名**（不带 .py），也要认。
            if py and base + ".py" in names:
                base = base + ".py"
            if base not in names:
                continue
            prev = toks[i - 1] if i else ""
            # Python 里没有 shell 式的"命令词"：非注释行里出现某个脚本名，只能是字符串字面量 ⇒
            # 就是 subprocess/run 的实参（`gate.py` 调 `run-gaps.sh` 正是这种形态）。
            if py or t.startswith(INVOKE_PREFIX) or prev in ("bash", "sh", "python3", "python", "env"):
                hit.add(base)
    return hit


def closure(root: pathlib.Path, seeds, by_name):
    """从种子出发做可达闭包。"""
    seen = set()
    q = collections.deque(seeds)
    while q:
        n = q.popleft()
        if n in seen:
            continue
        seen.add(n)
        p = by_name.get(n)
        if not p:
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in invoked(text, by_name.keys(), p.suffix == ".py"):
            if m not in seen:
                q.append(m)
    return seen


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    ap.add_argument("--check-py", default=None)
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()
    check_py = pathlib.Path(args.check_py) if args.check_py else root / "tools" / "check.py"
    if not check_py.is_file():
        print(f"找不到 {check_py}", file=sys.stderr)
        return 2

    cand = scripts_in(root)
    by_name = {p.name: p for p in cand}
    # 种子里也要认 check.py 引用的**文件名**（含不可执行/不在候选里的）
    cp_text = check_py.read_text(encoding="utf-8", errors="replace")
    seeds = invoked(cp_text, by_name.keys(), check_py.suffix == ".py")
    reach = closure(root, seeds, by_name)

    bad, exempt, wired = [], [], []
    for p in cand:
        n = p.name
        if n in reach:
            wired.append(p)
            continue
        head = "\n".join(p.read_text(encoding="utf-8", errors="replace").split("\n")[:SCAN_LINES])
        m = MANUAL.search(head)
        if m:
            exempt.append((p, m.group(1)))
        else:
            bad.append(p)

    print(f"[wiring] 电池可达 {len(wired)} 个；自报 manual 豁免 {len(exempt)} 个；未接线 {len(bad)} 个")
    for p, why in exempt:
        print(f"  [wiring] manual  {p} —— {why}")
    for p in bad:
        print(f"  [wiring] 未接线  {p} —— 不在电池可达闭包里，且文件开头没有 `# manual: <理由>`")
    if bad:
        print(f"[wiring] FAIL：{len(bad)} 个脚本既不在电池里也没自报 manual —— 判据不进电池 = 没有判据")
        return 1
    print("[wiring] ok 每个 run*/gate* 脚本要么在电池可达闭包里，要么自报了 manual 理由")
    return 0


if __name__ == "__main__":
    sys.exit(main())
