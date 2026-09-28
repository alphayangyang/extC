#!/usr/bin/env python3
"""每个 `EXTC_*` 开关：写进 `--help`，或者在 `UNDOCUMENTED` 里挂着（棘轮：只能降，不能升）。

`--help` 曾经写着"这些开关从不改变输出"，而实测有两个会改（`EXTC_NO_LEVELPASS` 动竞技场放置、
`EXTC_SELFCHECK` 能把构建弄红）—— 文档与代码打架比没有文档更坏。这条闸门把"诚实的文档"变成可检验的：
新加一个开关却忘了写进 `--help`，就红；而**改产物/改接受**的开关必须在 `--help` 里点名。
挂着的那些是历史存量，迁移一个就从 `UNDOCUMENTED` 划掉一个（和 `check_walkers.py` 的 RATCHET 同一形状）。
"""
import re, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 读得到、但还没写进 `--help` 的开关。**只能减，不能增**：加新开关时请直接写进 `--help`。
UNDOCUMENTED = {
    "EXTC_DBG_ARENA_VERBOSE",
    "EXTC_DBG_AT",
    "EXTC_DBG_CORO",
    "EXTC_DBG_DEFER",
    "EXTC_DBG_DFA",
    "EXTC_DBG_FACT",
    "EXTC_DBG_FS",
    "EXTC_DBG_FX",
    "EXTC_DBG_FX_VERBOSE",
    "EXTC_DBG_LOCAL",
    "EXTC_DBG_LV",
    "EXTC_DBG_MINAT",
    "EXTC_DBG_MOD",
    "EXTC_DBG_PAR",
    "EXTC_DBG_PRIM",
    "EXTC_DBG_REFARGS",
    "EXTC_DBG_RET3",
    "EXTC_DBG_RHO",
    "EXTC_DBG_S3",
    "EXTC_DBG_SITE2",
    "EXTC_DBG_STORES",
    "EXTC_DBG_TIME",
    "EXTC_DBG_ZONE",
    "EXTC_DUMP_LVL",
    "EXTC_DUMP_OW",
    "EXTC_DUMP_POOL",
    "EXTC_EXPLAIN_MEMORY",
    "EXTC_EXPLAIN_ROOT",
    "EXTC_SELFCHECK_VERBOSE",
    "EXTC_STD",
}
# 改产物 / 改接受的开关**必须**在 `--help` 里点名（它们违反了"只诊断"的默认）。
OUTPUT_AFFECTING = ["EXTC_NO_LEVELPASS", "EXTC_SELFCHECK"]


def switches_in_code() -> set:
    found = set()
    for path in sorted((ROOT / "src").glob("*.c")):
        found |= set(re.findall(r'"(EXTC_[A-Z0-9_]+)"', path.read_text(encoding="utf-8")))
    return found


def help_text() -> str:
    r = subprocess.run([str(ROOT / "build" / "extc"), "--help"], capture_output=True, text=True)
    return r.stdout + r.stderr


def main() -> int:
    code = switches_in_code()
    doc = help_text()
    bad = 0
    undocumented = sorted(s for s in code if s not in doc)
    new = [s for s in undocumented if s not in UNDOCUMENTED]
    if new:
        print("  新开关没写进 `--help`（也不在 UNDOCUMENTED 里）：")
        for s in new:
            print(f"    {s}")
        bad = 1
    gone = sorted(s for s in UNDOCUMENTED if s not in code)
    for s in gone:
        print(f"  UNDOCUMENTED 里的 `{s}` 代码里已经没有了")
        bad = 1
    for s in OUTPUT_AFFECTING:
        if s not in doc:
            print(f"  改产物/改接受的开关 `{s}` 没在 `--help` 里点名")
            bad = 1
    if bad:
        print("  ✗ 见上")
        return 1
    print(f"  {len(code)} 个 EXTC_ 开关：{len(code) - len(undocumented)} 个写进 --help，"
          f"{len(undocumented)} 个挂在 UNDOCUMENTED（棘轮，只能降）；改产物/改接受的两个都点了名")
    return 0


if __name__ == "__main__":
    sys.exit(main())
