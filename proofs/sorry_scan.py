#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sorry_scan.py —— 剥掉 Lean 注释后扫描未完成的证明。

`grep sorry` 会被注释里的说明文字误伤（本仓库的文档风格里到处在讲"不含 sorry"），
所以这里先把块注释（支持嵌套）与行注释去掉，再找 `sorry` / `admit` / 自定义 `axiom`。
"""
import re
import sys

FILES = ["ExtCRegion.lean", "ExtCPool.lean", "ExtCModel.lean", "ExtCPrecision.lean", "ExtCPath.lean", "ExtCGap.lean", "ExtCMemGap.lean", "ExtCMechanism.lean", "ExtCArenaPool.lean"]


def strip_comments(src: str) -> str:
    out = []
    depth = 0
    i = 0
    n = len(src)
    while i < n:
        if src.startswith("/-", i):
            depth += 1
            i += 2
            continue
        if src.startswith("-/", i) and depth:
            depth -= 1
            i += 2
            continue
        if depth == 0 and src.startswith("--", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if depth == 0:
            out.append(src[i])
        i += 1
    return "".join(out)


def main() -> int:
    bad = 0
    for f in FILES:
        code = strip_comments(open(f, encoding="utf-8").read())
        for kw in ("sorry", "admit"):
            for m in re.finditer(r"\b" + kw + r"\b", code):
                line = code[: m.start()].count("\n") + 1
                print(f"       {f}:{line} 出现 {kw}")
                bad = 1
        if re.search(r"(?m)^\s*axiom\b", code):
            print(f"       {f} 出现自定义 axiom")
            bad = 1
    return bad


if __name__ == "__main__":
    sys.exit(main())
