#!/usr/bin/env python3
"""禁止历史词的棘轮：活文档与源码注释里不许出现「历史」。

规则见 `docs/README.md` 维护规则 1 与 `docs/COMMENT-STYLE.md`：
**文档与注释只写"现在是什么"；历史只进 `docs/CHANGELOG.md`。**

    tools/check_no_history.py            报告（列出存量与新条目）
    tools/check_no_history.py --gate     出现基线之外的新条目就失败（check.sh 调用）
    tools/check_no_history.py --write-baseline

棘轮：`tools/no-history-baseline.txt` 记录**存量**（键 = 文件|规则|整行文字，行号变动不 churn）。
基线**只许变短，不许变长** —— 清掉一条就从基线里删掉那行。

范围：活文档（`docs/**/*.md`，**不含** `archive/`、`manual/`、`CHANGELOG.md`）· 源码与脚本
（`src/**/*.{c,h}` · `stdlib/**/*.extc` · `tools/**/*.py`）。`tests/` 不在范围内。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = ROOT / "tools" / "no-history-baseline.txt"

# 规则名 -> 正则。宁可窄：误报会让人学会忽略这条闸门。
RULES = [
    ("decision-number", r"定案\s*\d+"),
    ("plan-number", r"PLAN\s*#\s*\d+"),
    ("history-word", r"以前|曾经|(?<!往)(?<!标记)回退|一开始|那次|一度|原先是|本来是|旧版|旧的实现"),
    ("measure-narrative", r"实测(过|出)|量过|测过|量出来"),
    ("audit-reference", r"\baudit\b|审计"),
    ("history-en", r"\bused to\b|\bmeasured\b|\bno longer\b|\bearlier (version|implementation|code)\b|\bpreviously\b|\boriginally\b"),
]

# 活文档里**不算**的范围（已归档 / 待归档 / 唯一历史档）。
DOC_SKIP_PARTS = {"archive", "manual"}
DOC_SKIP_NAMES = {"CHANGELOG.md"}

GLOBS = ["docs/**/*.md", "src/**/*.c", "src/**/*.h", "stdlib/**/*.extc", "tools/**/*.py"]


def files():
    out = []
    for pattern in GLOBS:
        for p in sorted(ROOT.glob(pattern)):
            if not p.is_file():
                continue
            if p.name == "check_no_history.py":
                continue          # 闸门自己的词表与说明里就是这些词
            if p.suffix == ".md":
                if DOC_SKIP_PARTS & set(p.parts) or p.name in DOC_SKIP_NAMES:
                    continue
            out.append(p)
    return out


def strip_code(text):
    """只去掉围栏代码块 —— 被禁词的「引例」应当写在围栏里。

    行内代码**不剥**：C 注释里的反引号不是代码跨度，剥了会把 ``定案 88`` 这类真违规藏起来。
    """
    return re.sub(r"```.*?```", "", text, flags=re.S)


DOC_REF = re.compile(r"[A-Za-z0-9_./-]+\.md")


def doc_ref_findings():
    """`src/` 与 `stdlib/` 里不许引用 `.md` 路径。

    注释引用文档有两个问题：文档会改名（链接变死），以及"引文档"本身不利于维护
    —— 该说的是那个事实，不是去哪本书里翻。`tools/` 不在此列：闸门确实要读写
    `docs/usage/*.md`，那是它的职责。
    """
    out = []
    for pattern in ("src/**/*.c", "src/**/*.h", "stdlib/**/*.extc"):
        for p in sorted(ROOT.glob(pattern)):
            for i, line in enumerate(p.read_text(encoding="utf-8", errors="replace").split("\n"), 1):
                m = DOC_REF.search(line)
                if m:
                    out.append((p.relative_to(ROOT).as_posix(), "doc-reference", line.strip()))
    return out


def findings():
    out = list(doc_ref_findings())
    
    for p in files():
        rel = p.relative_to(ROOT).as_posix()
        text = strip_code(p.read_text(encoding="utf-8", errors="replace"))
        for line in text.split("\n"):
            for rule, pat in RULES:
                if re.search(pat, line):
                    out.append((rel, rule, line.strip()))
    return out


def load_baseline():
    if not BASE.exists():
        return {}
    d = {}
    for l in BASE.read_text(encoding="utf-8").splitlines():
        if not l.strip() or l.startswith("#"):
            continue
        k = "\t".join(l.split("\t", 2))
        d[k] = d.get(k, 0) + 1
    return d


def main():
    fs = findings()
    base = load_baseline()
    cur = {}
    for f in fs:
        k = "\t".join(f)
        cur[k] = cur.get(k, 0) + 1

    new = sorted(k for k in cur if k not in base)
    gone = sorted(k for k in base if k not in cur)

    if "--write-baseline" in sys.argv:
        BASE.write_text(
            "# 存量的历史词（棘轮：只许变短）。键 = 文件<TAB>规则<TAB>整行文字\n"
            + "\n".join(sorted(cur)) + "\n",
            encoding="utf-8")
        print(f"已写入基线：{len(cur)} 条")
        return 0

    if "--gate" in sys.argv:
        bad = False
        if new:
            print(f"新增历史词 {len(new)} 条（活文档与注释里不许写历史；历史只进 docs/CHANGELOG.md）：")
            for k in new[:15]:
                print("  NEW   " + k.replace("\t", " | ")[:150])
            if len(new) > 15:
                print(f"  … 另有 {len(new) - 15} 条")
            bad = True
        if gone:
            print(f"基线里有 {len(gone)} 条已经不再出现 ⇒ 请从 tools/no-history-baseline.txt 删掉它们：")
            for k in gone[:10]:
                print("  FIXED " + k.replace("\t", " | ")[:150])
            bad = True
        if bad:
            sys.exit(1)
        print(f"ok  活文档与注释无新增历史词（存量 {len(cur)} 条，只许变短）")
        return 0

    by_rule = {}
    for _, r, _ in fs:
        by_rule[r] = by_rule.get(r, 0) + 1
    by_dir = {}
    for f, _, _ in fs:
        d = f.split("/")[0] if "/" in f else "(root)"
        by_dir[d] = by_dir.get(d, 0) + 1
    print("范围：活文档 + src/ + stdlib/ + tools/")
    print(f"  存量 {len(cur)} 条 · 基线 {len(base)} 条 · 新增 {len(new)} · 基线里已消失 {len(gone)}")
    for r, c in sorted(by_rule.items(), key=lambda kv: -kv[1]):
        print(f"    {r:20s} {c}")
    for d, c in sorted(by_dir.items(), key=lambda kv: -kv[1]):
        print(f"    {d:20s} {c}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
