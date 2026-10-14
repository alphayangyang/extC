#!/usr/bin/env python3
"""tools/check_invariant_table.py —— 守住「不变量清单不许漏项」。

## 为什么需要这个门禁

`docs/RECLAMATION.md` 的「不变量与它们的牙」表，是把运行期里每一条**义务**（代码注释里编号的
那几条）对上"坏掉的方向 / 在哪强制 / 哪条判据证明它有牙"。这份表的价值全在**完整**上：
新写一条义务却忘了进表，表就从"地图"退化成"抽查记录"，而读者（和以后改这块代码的人）
会以为没进表的东西不存在。

清单本身没法自动生成（"在哪强制""怎么证明"要人写），但**完整性**可以机械检查：
发射出来的运行期文本里出现的每个 `义务 N`，都必须在 `docs/RECLAMATION.md` 里出现。
这条门禁就是干这个 —— 它不检查表写得好不好，只检查**没有漏项**。

用法：`python3 tools/check_invariant_table.py`（`--selftest` 用合成文本证明它**能红**）。
"""
import argparse
import re
import sys
import tempfile

import check_note_part  # 同目录：复用"把运行期文本发射出来"的那段

DOC = "docs/RECLAMATION.md"
RE_OBLIGATION = re.compile(r"义务\s*(\d+(?:\s*/\s*\d+)*)")


def obligations(text: str) -> set:
    """文本里出现的义务编号集合；`义务 2/5` 记成 {2, 5}。"""
    out = set()
    for m in RE_OBLIGATION.finditer(text):
        for part in re.split(r"\s*/\s*", m.group(1)):
            out.add(int(part))
    return out


def missing(code_text: str, doc_text: str) -> set:
    return obligations(code_text) - obligations(doc_text)


SELFTEST_CASES = [
    ("代码有、表里没有 ⇒ 必须红", "义务 1 … 义务 8", "义务 1", True),
    ("都在表里（含 `2/5` 这种合写）⇒ 必须绿", "义务 1 … 义务 2 … 义务 5", "义务 1 / 义务 2/5", False),
    ("表里多写了编号（历史遗留）⇒ 不算错", "义务 1", "义务 1 / 义务 9", False),
]


def selftest() -> int:
    bad = 0
    for name, code, doc, expect_bad in SELFTEST_CASES:
        got = missing(code, doc)
        ok = bool(got) == expect_bad
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"（{sorted(got)}）" if got else ""))
        bad += 0 if ok else 1
    print(f"自检：{'全部通过' if not bad else f'{bad} 条失败'}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--doc", default=None, help="换一份清单文本（演示/外部检查用）")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    root = check_note_part.ROOT
    with tempfile.TemporaryDirectory() as tmp:
        code_text = check_note_part.emit_runtime(tmp)
    if not code_text:
        return 1
    with open(args.doc or f"{root}/{DOC}", encoding="utf-8") as f:
        doc_text = f.read()
    code_obs = obligations(code_text)
    gone = missing(code_text, doc_text)
    if gone:
        for n in sorted(gone):
            print(f"FAIL 义务 {n}：运行期里有，`{DOC}` 的不变量表里没有")
        print(f"清单漏了 {len(gone)} 条义务")
        return 1
    print(f"ok  不变量清单完整：运行期里的义务 {sorted(code_obs)} 都在 `{DOC}` 里")
    return 0


if __name__ == "__main__":
    sys.exit(main())
