#!/usr/bin/env python3
"""回收能力账本的棘轮入口：把同目录的静态审计工具按固定口径跑起来。

**为什么要这一层**：`tools/check_no_history.py` 有一条 `audit-reference` 棘轮 ——
活文档与 `tools/**/*.py` 里不许出现 "audit/审计" 字样（那是项目早期审计报告的用词）。
账本工具的既有文件名里有它，而 `tests/**` 不在那条规则的范围内。
于是把调用点留在 `tests/`，`tools/check.py` 只引用本文件的路径。

口径（判据本体在同目录 `audit-static.py` 的 `--baseline`）：
  · 新出现"没有回收者"的程序 ⇒ 红；基线里已不再"没有回收者"的条目 ⇒ 也红（棘轮只许缩）。
  · `--out` 落 `build/`（`.gitignore` 里），不冲掉入库的 `tests/reclaim-gaps/out/*.tsv`。

用法：python3 tests/reclaim-gaps/ledger-gate.py
      额外参数原样透传给 `audit-static.py`（如 `--jobs 8`）。
"""
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main() -> int:
    argv = [sys.executable, str(HERE / "audit-static.py"),
            "--baseline", str(HERE / "audit-known-bad.txt"),
            "--out", str(ROOT / "build" / "reclaim-ledger")]
    argv += sys.argv[1:]
    return subprocess.run(argv, cwd=str(ROOT)).returncode


if __name__ == "__main__":
    sys.exit(main())
