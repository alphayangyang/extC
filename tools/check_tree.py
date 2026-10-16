#!/usr/bin/env python3
# tools/check_tree.py —— **跑出来的裸转储不许入库**。
#
# 为什么要有这条判据（而不是靠记性）：`tests/**/out/` 与 `bench/gc/v2/pause/` 下的
# `<形状>.on.c`、`.events`、`.hist`、`.log` 都是 runner 跑一遍就重写的转储。它们留在库里，
# 每次复跑都要 `git checkout --` 还债（否则工作树脏）；而 `tests/reclaim-gaps/out/*.on.c`
# 单这一组就 2.5 MB（38 份生成 C，多组成对同 blob）。
# 光是撤文件不够：`.gitignore` 不写，下次 `run-gaps.sh` 就把它们加回来。
# 这条判据把两件事钉在一起 —— **转储不许入库** 且 **`.gitignore` 必须覆盖它**。
#
# 口径（客观、可机检）：
#   ① `git ls-files` 里不许出现下表模式；
#   ② 下表每个模式必须被 `.gitignore` 覆盖（`git check-ignore --no-index` 问路径归属）。
#
# 入库的是**记录**不是**转储**：被文档当数字来源引用的汇总产物（`tests/reclaim-gaps/out/`
# 里那几个 `.tsv` 与 `raw.txt`、`tests/reclaim-perf/out/*.tsv`、`pause_v2.tsv`、`REPORT.md`、
# `proto-diff/*.diff` …）留在库里 —— 别往这张表里加。
#
# 用法：
#   python3 tools/check_tree.py          报告
#   python3 tools/check_tree.py --gate   有入库的转储、或 `.gitignore` 漏了模式 ⇒ exit 1
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 模式（仓库相对路径的 glob，`*` 不跨 `/`）→ 一句话理由。
RULES = [
    ("tests/reclaim-gaps/out/*.on.c", "跑 run-gaps.sh 现场写出的生成 C"),
    ("tests/*/out/*.log", "runner 的进度 / 构建日志"),
    ("tests/reclaim-idle/out/*.txt", "手跑闸门时 tee 下来的原始输出"),
    ("tests/escape-fuzz/recon/out/*.txt", "勘察探针的原始输出（run-probes.sh 重跑即得）"),
    ("tests/reclaim-unified/out/C-prototype-raw.txt", "插桩原型的原始 stdout"),
    ("bench/gc/v2/pause/*/*.events", "停顿原始事件流（聚合器的输入，跑 runner 即得）"),
    ("bench/gc/v2/pause/*/*.hist", "停顿原始直方（同上）"),
]

WITNESS = "_tree_pat_"      # 替身路径：只是为了拿它问 git"这条模式被忽略吗"


def rx(pat: str):
    """glob → 正则；`*` 只吃一段（不跨 `/`），免得模式误伤更深的同名路径。"""
    return re.compile("^" + "".join("[^/]*" if c == "*" else re.escape(c) for c in pat) + "$")


def tracked(root: pathlib.Path):
    p = subprocess.run(["git", "ls-files", "-z"], cwd=str(root), capture_output=True)
    if p.returncode != 0:
        sys.exit("git ls-files 失败（--root 指到的不是 git 仓库？）")
    return [f for f in p.stdout.decode("utf-8", "replace").split("\0") if f]


def ignored(root: pathlib.Path, path: str) -> bool:
    return subprocess.run(["git", "check-ignore", "--no-index", "-q", path],
                          cwd=str(root), capture_output=True).returncode == 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="跑出来的裸转储不许入库")
    ap.add_argument("--gate", action="store_true", help="有违规就 exit 1")
    ap.add_argument("--root", default=str(ROOT), help="换根（自测用）")
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    files = tracked(root)
    bad = []
    for pat, why in RULES:
        hits = [f for f in files if rx(pat).match(f)]
        if hits:
            bad.append(f"入库的转储 {pat}：{len(hits)} 个（{', '.join(hits[:3])}"
                       + ("，…" if len(hits) > 3 else "") + "）")
        if not ignored(root, pat.replace("*", WITNESS)):
            bad.append(f"{pat} 没被 .gitignore 覆盖 —— 下一次跑 runner 就会长回来")
        else:
            print(f"  ok  {pat:<46} {why}")

    if bad:
        print(f"[tree] 红：{len(bad)} 条", file=sys.stderr)
        for b in bad:
            print(f"  · {b}", file=sys.stderr)
        print("  → 转储：`git rm --cached <路径>`；`.gitignore` 补上模式，别只删文件。",
              file=sys.stderr)
        return 1 if args.gate else 0
    print(f"ok 树卫生：{len(RULES)} 条转储模式都没入库，且都被 .gitignore 覆盖")
    return 0


if __name__ == "__main__":
    sys.exit(main())
