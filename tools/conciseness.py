#!/usr/bin/env python3
"""比较 docs/conciseness/ 里成对练习的 extC 与 Python 写法。

用法：
    python3 tools/conciseness.py            # 表格 + 合计
    python3 tools/conciseness.py --check    # 再核对每一对的 stdout 逐字节相同
    python3 tools/conciseness.py --json     # 机读

一对练习 = 同名文件 `<name>.extc` + `<name>.py`。三个指标：
    lines  原始行数（含空行与注释）
    code   去掉空行与纯注释行之后的行数
    tokens 词元数（标识符 / 数字 / 每个非空白非单词字符各算一个）
只统计，不做判断：比率要结合每个练习的具体形状读。
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "docs" / "conciseness"

TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|[0-9]+(?:\.[0-9]+)?|[^\sA-Za-z0-9_]")


def strip_block_comments(text: str) -> str:
    """把 /* ... */ 整块挖掉（保留行数），这样 code 行数对两种语言同一个口径。"""
    out = []
    i = 0
    in_block = False
    while i < len(text):
        if not in_block and text.startswith("/*", i):
            in_block = True
            out.append("\n")
            i += 2
            continue
        if in_block and text.startswith("*/", i):
            in_block = False
            i += 2
            continue
        out.append("\n" if in_block and text[i] == "\n" else ("" if in_block else text[i]))
        i += 1
    return "".join(out)


def count(path: Path, lang: str) -> dict:
    raw = path.read_text(encoding="utf-8")
    if lang == "extc":
        raw = strip_block_comments(raw)
        comment = "//"
    else:
        comment = "#"
    lines = raw.splitlines()
    code = 0
    for line in lines:
        s = line.strip()
        if not s or s.startswith(comment):
            continue
        code += 1
    return {
        "lines": len(lines),
        "code": code,
        "tokens": len(TOKEN.findall(raw)),
    }


def check_pairs(rows: list[dict]) -> int:
    """每一对都跑一遍，stdout 必须逐字节相同（判据与基准里的对齐判据同一个）。"""
    bad = 0
    for r in rows:
        extc = CORPUS / f"{r['name']}.extc"
        py = CORPUS / f"{r['name']}.py"
        a = subprocess.run([str(ROOT / "build" / "extc"), "--run", str(extc)],
                           capture_output=True, text=True)
        b = subprocess.run([sys.executable, str(py)], capture_output=True, text=True)
        if a.returncode != 0:
            print(f"FAIL {r['name']}: extc --run exited {a.returncode}: {a.stderr.strip()[:200]}")
            bad += 1
            continue
        if a.stdout != b.stdout:
            print(f"FAIL {r['name']}: stdout differs\n  extc: {a.stdout!r}\n  py:   {b.stdout!r}")
            bad += 1
    print(f"pairs: {len(rows)} checked, {bad} differ")
    return 1 if bad else 0


def main() -> int:
    if not CORPUS.is_dir():
        print(f"no corpus at {CORPUS}", file=sys.stderr)
        return 1
    rows = []
    for extc in sorted(CORPUS.glob("*.extc")):
        py = extc.with_suffix(".py")
        if not py.is_file():
            print(f"warning: {extc.name} has no .py twin", file=sys.stderr)
            continue
        e = count(extc, "extc")
        p = count(py, "py")
        rows.append({"name": extc.stem, "extc": e, "py": p})
    if not rows:
        print(f"no pairs in {CORPUS}", file=sys.stderr)
        return 1
    if "--check" in sys.argv:
        return check_pairs(rows)

    if "--json" in sys.argv:
        print(json.dumps(rows, indent=2))
        return 0

    def ratio(a: int, b: int) -> str:
        return f"{a / b:.2f}x" if b else "-"

    head = f"{'program':<12} {'lines':>13} {'code':>13} {'tokens':>15}"
    print(head)
    print("-" * len(head))
    tot = {"lines": [0, 0], "code": [0, 0], "tokens": [0, 0]}
    for r in rows:
        e, p = r["extc"], r["py"]
        print(
            f"{r['name']:<12} "
            f"{e['lines']:>3}/{p['lines']:<3} {ratio(e['lines'], p['lines']):>6} "
            f"{e['code']:>3}/{p['code']:<3} {ratio(e['code'], p['code']):>6} "
            f"{e['tokens']:>5}/{p['tokens']:<5} {ratio(e['tokens'], p['tokens']):>6}"
        )
        for k in tot:
            tot[k][0] += e[k]
            tot[k][1] += p[k]
    print("-" * len(head))
    print(
        f"{'total':<12} "
        f"{tot['lines'][0]:>3}/{tot['lines'][1]:<3} "
        f"{ratio(*tot['lines']):>6} "
        f"{tot['code'][0]:>3}/{tot['code'][1]:<3} "
        f"{ratio(*tot['code']):>6} "
        f"{tot['tokens'][0]:>5}/{tot['tokens'][1]:<5} "
        f"{ratio(*tot['tokens']):>6}"
    )
    print("\n每格是 extC/Python；x 表示 extC 是 Python 的多少倍。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
