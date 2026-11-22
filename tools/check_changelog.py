#!/usr/bin/env python3
"""历史按月分页与索引一致 —— 免得开发历史又长回一个没人读的巨型文件。

历史已经随文档独立成项目：正文按月分页在 `$EXTC_DOCS/devlog/changelog/YYYY-MM.md`
（缺省 `~/extc-docs`），索引页是同目录的 `README.md`。

    tools/check_changelog.py           报告（默认文档项目）
    tools/check_changelog.py --write   重写索引里的月份表（条目数、日期范围）
    tools/check_changelog.py --gate    不一致就失败（check.sh 调用）
    tools/check_changelog.py --root DIR --index FILE   显式指定月份页目录与索引页

缺文档项目 ⇒ 按跳过协议退出 0（文档独立维护）。

分工：

* 正文按月份分页在 `devlog/changelog/YYYY-MM.md`。每条 `## YYYY-MM-DD · 标题`（日期后可带
  `（…）` 修饰）必须落在其日期所属的那一页；同一个标题不许出现在两页。
* 索引页只存月份表与写法，不存正文。表由 `--write` 生成，`--gate` 对着月份页核对
  条目数与日期范围。

为什么索引也要判据：加了新月份页却忘了登表 ⇒ 那一页从索引里消失，等于没有；条目数与
日期范围写进表里，还让「有没有人往旧月份悄悄补条目」这件事在 diff 里可见。
"""
import argparse
import re
import sys
import pathlib

import docroot

ROOT = pathlib.Path(__file__).resolve().parent.parent

NAME = re.compile(r"^(\d{4})-(\d{2})\.md$")
ENTRY = re.compile(r"^## (\d{4})-(\d{2})-(\d{2})(?:（[^）]*）)?\s*·\s*\S")
HEAD = re.compile(r"^## ")
# 索引与月份页同目录时链接是 `YYYY-MM.md`；索引在别处时是 `changelog/YYYY-MM.md`（旧布局）。
ROW = re.compile(r"^\|\s*\[(\d{4}-\d{2})\]\((?:changelog/)?(\d{4}-\d{2})\.md\)\s*\|\s*(\d+)\s*\|\s*([^|]*?)\s*\|\s*$")
BEGIN = "<!-- 月份表：`python3 tools/check_changelog.py --write` 重新生成 -->"
END = "<!-- /月份表 -->"


def rel_label(path, base):
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return str(path)


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


def survey(dir_path, index):
    """→ (pages, want_rows, bad)；pages = 月份 → (条数, 日期)；want_rows 供 --write 与 --gate。"""
    pages, rows, bad = {}, [], []
    prefix = "" if index.parent == dir_path else "changelog/"
    for path in sorted(dir_path.glob("*.md"), reverse=True):
        if path.resolve() == index.resolve():
            continue                    # 索引页本身也在月份页目录里
        if not NAME.match(path.name):
            bad.append(f"{rel_label(path, dir_path.parent)}: 文件名必须是 YYYY-MM.md")
            continue
        n, dates, errs = scan(path)
        bad += errs
        if not n:
            bad.append(f"{rel_label(path, dir_path.parent)}: 一条 `## 日期 · 标题` 都没有")
            continue
        pages[path.stem] = (n, dates)
        rows.append((path.stem, n, f"{min(dates)} → {max(dates)}",
                     f"| [{path.stem}]({prefix}{path.name}) | {n} | {min(dates)} → {max(dates)} |"))
    return pages, rows, bad


def dup_headings(dir_path):
    seen, dup = {}, []
    for path in sorted(dir_path.glob("*.md")):
        if path.name == "README.md":
            continue                    # 索引页不存条目正文
        for line in path.read_text(encoding="utf-8").split("\n"):
            if HEAD.match(line):
                if line in seen:
                    dup.append(f"{line[:70]}（{seen[line]} 与 {path.name} 都有一条）")
                seen[line] = path.name
    return dup


def parse_table(text):
    """→ (rows, bad)：索引月份表里的行 [(月份, 条数, 日期范围)]；缺标记返回 (None, [])。"""
    if BEGIN not in text or END not in text:
        return None, []
    block = text.split(BEGIN, 1)[1].split(END, 1)[0]
    rows = []
    for line in block.split("\n"):
        s = line.strip()
        if not s:
            continue
        m = ROW.match(s)
        if not m:
            return None, [f"索引月份表里有无法解析的行：{s[:80]}"]
        rows.append((m.group(2), int(m.group(3)), m.group(4).strip()))
    return rows, []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", action="store_true", help="不一致就 exit 1")
    ap.add_argument("--write", action="store_true", help="重写索引里的月份表")
    ap.add_argument("--root", help="月份页目录（缺省 $EXTC_DOCS/devlog/changelog）")
    ap.add_argument("--index", help="索引页（缺省 <root>/README.md）")
    args = ap.parse_args()

    if args.root:
        DIR = pathlib.Path(args.root).expanduser()
    else:
        docroot.require_or_skip()
        DIR = docroot.doc("devlog/changelog")
    if not DIR.is_dir():
        if args.root:
            print(f"月份页目录不存在：{DIR}", file=sys.stderr)
            return 1
        docroot.skip("文档项目 " + docroot.display() + " 里还没有 devlog/changelog/ 目录")
    INDEX = pathlib.Path(args.index).expanduser() if args.index else DIR / "README.md"
    if not INDEX.is_file():
        if args.index:
            print(f"索引页不存在：{INDEX}", file=sys.stderr)
            return 1
        docroot.skip("文档项目 " + docroot.display() + " 里还没有 devlog/changelog/README.md")
    idx_label = rel_label(INDEX, DIR.parent)

    pages, want_rows, bad = survey(DIR, INDEX)
    if not pages:
        print(f"{rel_label(DIR, DIR.parent)}/ 下没有任何月份页", file=sys.stderr)
        return 1
    bad += dup_headings(DIR)
    total = sum(n for n, _ in pages.values())
    want = [(ym, n, rng) for ym, n, rng, _text in want_rows]

    text = INDEX.read_text(encoding="utf-8")
    got, bad_rows = parse_table(text)
    bad += bad_rows
    if got is None:
        bad.append(f"{idx_label} 里找不到月份表的开始/结束标记（见本脚本的 BEGIN/END）")
    for line in text.split("\n"):
        if ENTRY.match(line):
            bad.append(f"{idx_label} 不该出现条目正文：{line[:60]}")
            break

    if args.write:
        if BEGIN not in text or END not in text:
            print(f"FAIL {idx_label} 里找不到月份表标记，无法 --write", file=sys.stderr)
            return 1
        head, rest = text.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        table = "\n".join(r[3] for r in want_rows)
        INDEX.write_text(f"{head}{BEGIN}\n{table}\n{END}{tail}", encoding="utf-8")
        print(f"write {idx_label} 月份表：{len(want_rows)} 行")
        return 0

    if got is not None and got != want:
        bad.append(f"索引里的月份表与月份页对不上（跑 tools/check_changelog.py --write）")
        for line in got[:6]:
            bad.append(f"  表里：{line}")
        for line in want[:6]:
            if line not in got:
                bad.append(f"  应为：{line}")

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
