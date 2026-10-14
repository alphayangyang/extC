#!/usr/bin/env python3
"""tools/check_doc_refs.py —— 守住「验牙的注入不许没写进文档」。

## 为什么需要这个门禁

`tools/inject.py` 里的每条注入都证明着某条判据**有牙**（哪条判据、打在哪、红在什么上）。
这些知识写在 `docs/RECLAMATION.md` 的两张表（义务表 / 跳过表）的"判据"列里。一张表的价值取决于
**引用是否完整**：新写一条注入却不进表，读表的人就会以为那条路径"没有牙"（或者反过来，
以为某条旧注入还在）。义务表、跳过表各自有同步门禁，这条是它们共同的**证据来源**门禁：

> 注入集里的每个名字，都必须在 `docs/RECLAMATION.md` 里出现。

只查这一个方向（注入 → 文档）：文档里出现的**散文**标识符没法可靠区分"注入名/判据名/函数名"，
硬查反向会造出一堆误报。名字从 `python3 tools/inject.py --list` 读，不在检查器里再抄一份。
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = "docs/RECLAMATION.md"


def injection_names() -> list:
    p = subprocess.run([sys.executable, "tools/inject.py", "--list"],
                       cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    names = re.findall(r"^\s{2}([a-z0-9][a-z0-9-]*)\s", p.stdout, re.M)
    return sorted(set(names))


SELFTEST_CASES = [
    ("注入名在文档里 ⇒ 必须绿", ["count-nosat"], "见 `count-nosat`", False),
    ("注入名不在文档里 ⇒ 必须红", ["count-nosat"], "（文档里什么都没写）", True),
]


def selftest() -> int:
    bad = 0
    for name, names, doc, expect_bad in SELFTEST_CASES:
        missing = [n for n in names if n not in doc]
        ok = bool(missing) == expect_bad
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
        bad += 0 if ok else 1
    print(f"自检：{'全部通过' if not bad else f'{bad} 条失败'}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--doc", default=None, help="换一份文档（演示/外部检查用）")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    names = injection_names()
    if not names:
        print("读不到注入集（tools/inject.py --list 没输出名字）", file=sys.stderr)
        return 1
    with open(args.doc or f"{ROOT}/{DOC}", encoding="utf-8") as f:
        doc = f.read()
    missing = [n for n in names if n not in doc]
    if missing:
        for n in missing:
            print(f"FAIL 注入 `{n}` 没有出现在 `{DOC}` 里（它的牙就没人知道）")
        return 1
    print(f"ok  注入集的 {len(names)} 条都写进了 `{DOC}`：{', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
