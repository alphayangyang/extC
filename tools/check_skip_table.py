#!/usr/bin/env python3
"""tools/check_skip_table.py —— 守住「跳过的那一刀」的清单与代码同步。

## 为什么需要这个门禁

运行期里有很多"为了省事/省时直接返回"的地方（`if (…) return …;`）。每一处都依赖一个前提，
前提破了可能往"多留"（保守）或"少算"（不安全）倒。`docs/RECLAMATION.md` 的「快路径与跳过」
表把这些前提、方向、判据列了出来 —— 但**表是散文**，没有东西拦得住"新加一处跳过却不进表"。
义务表有 `gate-invariant-table`，这里就是跳过表的对应物。

## 它检查三件事

1. **代码里每处跳过都带标记**：`if (…) return …;` 这一行的同一行必须有 `/* skip:<id> */`。
   未标记的数必须为 **0** —— 这是个只许往下拧的棘轮：新增一处未标记的跳过就会红。
2. **标记 → 表**：代码里出现的每个 `skip:<id>` 都必须在 `docs/RECLAMATION.md` 里出现。
3. **表 → 标记**：表里提到的每个 `skip:<id>` 都必须在代码里存在（删掉跳过却留着表行 = 红）。

**跨档取并集**：运行期是按档位发射的（默认 / 计数 / 时间 / 事件 / 本地存储 / 帧根链），
某个标记可能只活在一档的产物里（计数档的饱和跳过、事件档的恒不扫、帧根链档的链前提）。
核对必须把这些档都发一遍取并集，只看默认档会把那些表行误判成"陈旧"。

覆盖两种写法：单行 `if (…) return …;`，以及 `if (…)` / `if (…) {` 后面紧跟 `return` 的跨行写法。
标记可以落在 `if` 那一行，也可以落在紧跟的 `return` 那一行。
"""
import argparse
import re
import sys
import tempfile

import check_note_part  # 同目录：复用"把运行期文本发射出来"的那段

DOC = "docs/RECLAMATION.md"
RE_TAG = re.compile(r"skip:([a-z0-9][a-z0-9-]*)")
RE_EARLY = re.compile(r"^\s*if \(.*\)\s*return")
RE_OPEN = re.compile(r"^\s*if \(.*\)\s*\{?\s*$")

# 未标记的早退数上限：棘轮，只许往下拧（当前为 0 ⇒ 新增一处未标记的跳过立刻红）。
MAX_UNTAGGED = 0

# 发射档位：标记按档切开发，核对要把每一档的产物都看一遍再取并集。
MODES = [
    ("默认", {}),
    ("计数档", {"EXTC_RC_NOCOUNT": "0"}),
    ("时间档", {"EXTC_RC_TIME": "1"}),
    ("事件档", {"EXTC_RC_SWEEPFREE": "1"}),
    ("本地存储档", {"EXTC_RC_LOCALSTORE": "1"}),
    ("帧根链档", {"EXTC_RC_FRAMEROOTS": "1"}),
]


def collect_mismatch(code_text: str, doc_text: str):
    """返回 (未标记早退的行, 代码有表没有的标记, 表有代码没有的标记)。

    早退有两种写法，都要数进来：单行 `if (…) return …;`，以及 `if (…)` / `if (…) {`
    后面紧跟 `return` 的跨行写法（棘轮若只数单行，跨行写法就是它的盲区）。
    """
    lines = code_text.split("\n")
    untagged = []
    for i, line in enumerate(lines, 1):
        if RE_EARLY.search(line):                       # 单行写法：if (…) return …;
            if not RE_TAG.search(line):
                untagged.append((i, line.strip()[:90]))
            continue
        if RE_OPEN.search(line):                        # 跨行写法：if (…) / if (…) { 后紧跟 return
            nxt = lines[i] if i < len(lines) else ""
            if re.match(r"^\s*return\b", nxt) and not (RE_TAG.search(line) or RE_TAG.search(nxt)):
                untagged.append((i, line.strip()[:90]))
    code_tags = set(RE_TAG.findall(code_text))
    doc_tags = set(RE_TAG.findall(doc_text))
    return untagged, sorted(code_tags - doc_tags), sorted(doc_tags - code_tags)


SELFTEST_CASES = [
    ("代码有标记、表里没有 ⇒ 必须红",
     'if (!s) return;   /* skip:not-own */', "（表里什么都没写）", True),
    ("表里有、代码里没有 ⇒ 必须红（陈旧表行）",
     "int f(void) { return 0; }", "见 `skip:occ-free`", True),
    ("有早退但没标记 ⇒ 必须红（棘轮）",
     "if (!s) return 0;\n/* skip:x-none */", "`skip:x-none`", True),
    ("跨行早退没标记 ⇒ 必须红",
     "if (!s) {\n    return;\n}\n/* skip:x-none */", "`skip:x-none`", True),
    ("跨行早退带标记 ⇒ 必须绿",
     "if (!s) {   /* skip:not-own */\n    return;\n}", "见 `skip:not-own`", False),
    ("全部对上 ⇒ 必须绿",
     'if (!s) return;   /* skip:not-own */', "见 `skip:not-own`", False),
]


def selftest() -> int:
    bad = 0
    for name, code, doc, expect_bad in SELFTEST_CASES:
        untagged, only_code, only_doc = collect_mismatch(code, doc)
        got = bool(untagged) or bool(only_code) or bool(only_doc)
        ok = got == expect_bad
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
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
    with open(args.doc or f"{root}/{DOC}", encoding="utf-8") as f:
        doc_text = f.read()
    untagged = []          # [(档位, 行号, 行文本)]
    code_tags = set()
    with tempfile.TemporaryDirectory() as tmp:
        for label, env in MODES:
            code_text = check_note_part.emit_runtime(tmp, env)
            if not code_text:
                print(f"FAIL 发射器在「{label}」下发不出运行期文本", file=sys.stderr)
                return 1
            part_untagged, _, _ = collect_mismatch(code_text, doc_text)
            untagged += [(label, ln, text) for ln, text in part_untagged]
            code_tags |= set(RE_TAG.findall(code_text))
    doc_tags = set(RE_TAG.findall(doc_text))
    only_code = sorted(code_tags - doc_tags)
    only_doc = sorted(doc_tags - code_tags)
    bad = 0
    if len(untagged) > MAX_UNTAGGED:
        for label, ln, text in untagged[:8]:
            print(f"FAIL 未标记的跳过（{label}）：运行期第 {ln} 行 —— {text}")
        print(f"未标记的早退 {len(untagged)} 处 > 上限 {MAX_UNTAGGED}："
              f"请给每处补 `/* skip:<id> */` 并把它写进 `{DOC}` 的跳过表")
        bad = 1
    for tag in only_code:
        print(f"FAIL `skip:{tag}` 在运行期里有，`{DOC}` 的跳过表里没有")
        bad = 1
    for tag in only_doc:
        print(f"FAIL `skip:{tag}` 在 `{DOC}` 的跳过表里，运行期里已经没有了（陈旧表行）")
        bad = 1
    if bad:
        return 1
    print(f"ok  跳过清单与代码同步：{len(code_tags)} 类标记（{len(MODES)} 档取并集），"
          f"未标记早退 {len(untagged)} 处（上限 {MAX_UNTAGGED}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
