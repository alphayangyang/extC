#!/usr/bin/env python3
"""历史按月分页与索引一致 —— 免得 `docs/CHANGELOG.md` 又长回一个没人读的巨型文件。

    tools/check_changelog.py           报告
    tools/check_changelog.py --write   重写索引里的月份表（条目数、日期范围）
    tools/check_changelog.py --gate    不一致就失败（check.sh 调用）

分工：

* 正文按月份分页在 `docs/changelog/YYYY-MM.md`。每条 `## YYYY-MM-DD · 标题`（日期后可带
  `（…）` 修饰）必须落在其日期所属的那一页；同一个标题不许出现在两页。
* `docs/CHANGELOG.md` 是索引页：只有月份表与写法，不存正文。表由 `--write` 生成，
  `--gate` 对着月份页核对条目数与日期范围。

为什么索引也要判据：加了新月份页却忘了登表 ⇒ 那一页从索引里消失，等于没有；条目数与
日期范围写进表里，还让「有没有人往旧月份悄悄补条目」这件事在 diff 里可见。
"""
import argparse
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
INDEX = ROOT / "docs" / "CHANGELOG.md"
DIR = ROOT / "docs" / "changelog"

NAME = re.compile(r"^(\d{4})-(\d{2})\.md$")
ENTRY = re.compile(r"^## (\d{4})-(\d{2})-(\d{2})(?:（[^）]*）)?\s*·\s*\S")
HEAD = re.compile(r"^## ")
ROW = re.compile(r"^\|\s*\[(\d{4}-\d{2})\]\(changelog/(\d{4}-\d{2})\.md\)\s*\|\s*(\d+)\s*\|\s*([^|]*?)\s*\|\s*$")
BEGIN = "<!-- 月份表：`python3 tools/check_changelog.py --write` 重新生成 -->"
END = "<!-- /月份表 -->"


def scan(path):
    """→ (条目数, 日期列表, 违规)。"""
    bad, dates = [], []
    for i, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        if not HEAD.match(line):
            continue
        m = ENTRY.match(line)
        if not m:
            bad.append(f"{path.name}:{i}: 标题不是 `## YYYY-MM-DD · 标题`：{line[:60]}")
            continue
        ym = f"{m.group(1)}-{m.group(2)}"
        if ym != path.stem:
            bad.append(f"{path.name}:{i}: 条目的月份是 {ym}，却放在 {path.name}")
        dates.append(f"{m.group(1)}-{m.group(2)}-{m.group(3)}")
    return len(dates), dates, bad


def survey():
    """→ (pages, rows, bad)；pages = 月份 → (条数, 日期)，rows = 表里该有的行（新月份在上）。"""
    pages, rows, bad = {}, [], []
    for path in sorted(DIR.glob("*.md"), reverse=True):
        if not NAME.match(path.name):
            bad.append(f"docs/changelog/{path.name}: 文件名必须是 YYYY-MM.md")
            continue
        n, dates, errs = scan(path)
        bad += errs
        if not n:
            bad.append(f"docs/changelog/{path.name}: 一条 `## 日期 · 标题` 都没有")
            continue
        pages[path.stem] = (n, dates)
        rows.append(f"| [{path.stem}](changelog/{path.name}) | {n} | {min(dates)} → {max(dates)} |")
    return pages, rows, bad


def dup_headings():
    seen, dup = {}, []
    for path in sorted(DIR.glob("*.md")):
        for line in path.read_text(encoding="utf-8").split("\n"):
            if HEAD.match(line):
                if line in seen:
                    dup.append(f"{line[:70]}（{seen[line]} 与 {path.name} 都有一条）")
                seen[line] = path.name
    return dup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", action="store_true", help="不一致就 exit 1")
    ap.add_argument("--write", action="store_true", help="重写索引里的月份表")
    args = ap.parse_args()

    pages, rows, bad = survey()
    if not pages:
        print("docs/changelog/ 下没有任何月份页", file=sys.stderr)
        return 1
    bad += dup_headings()
    total = sum(n for n, _ in pages.values())

    text = INDEX.read_text(encoding="utf-8")
    if BEGIN not in text or END not in text:
        bad.append("docs/CHANGELOG.md 里找不到月份表的开始/结束标记（见本脚本的 BEGIN/END）")
    for line in text.split("\n"):
        if ENTRY.match(line):
            bad.append(f"docs/CHANGELOG.md 不该出现条目正文：{line[:60]}")
            break

    if args.write:
        head, rest = text.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        table = "\n".join(rows)
        INDEX.write_text(f"{head}{BEGIN}\n{table}\n{END}{tail}", encoding="utf-8")
        print(f"write docs/CHANGELOG.md 月份表：{len(rows)} 行")
        return 0

    if BEGIN in text and END in text:
        want = BEGIN + "\n" + "\n".join(rows) + "\n" + END
        if want not in text:
            got = text.split(BEGIN, 1)[1].split(END, 1)[0].strip().split("\n")[2:]
            bad.append("索引里的月份表与月份页对不上（跑 tools/check_changelog.py --write）")
            for line in got[:6]:
                bad.append(f"  表里：{line}")

    if bad:
        print(f"FAIL 历史分页/索引有 {len(bad)} 处不对", file=sys.stderr)
        for b in bad:
            print(f"  · {b}", file=sys.stderr)
        return 1
    detail = " · ".join(f"{ym}: {n}" for ym, (n, _) in sorted(pages.items(), reverse=True))
    print(f"ok  历史分页：{len(pages)} 页 / {total} 条，月份表一致（{detail}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
