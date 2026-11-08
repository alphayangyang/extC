#!/usr/bin/env python3
"""把 place 层级见证（`EXTC_DBG_LVL`）跑成一道闸门。

这条闸门不做判定，只统计：同一个形状下，见证的结论与检查器实际结论不一致多少条。
棘轮只有两个方向 —— 相等或更少；涨了就红，并打印涨的那些行。

为什么需要它：把 `promoteInto2` 的手写 `switch(kind)` 换成不动点之前，必须先证明
"同一个问题在所有写法上答案相同"。见证只观察（`src/check/check_lvl.c`），它跟检查器
不一致的地方就是还没对齐的地方。

`argLvl=unknown` 的行是"见证没有意见"（参数、`calleeMakesPool` 未收敛的调用），
不是不一致，单独计数、不进棘轮。
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "lvl-witness-baseline.txt"
EXTC = ROOT / "build" / "extc"
PATTERN = re.compile(r"^\[lvl\] .* suggested=(accept|reject)$")


def corpus():
    out = subprocess.run(["git", "ls-files", "tests/*.extc", "tests/**/*.extc",
                          "examples/*.extc"],
                         cwd=ROOT, capture_output=True, text=True, check=True)
    return [p for p in out.stdout.split() if p.endswith(".extc")]


def witness(path):
    env = {"EXTC_DBG_LVL": "1", "PATH": "/usr/bin:/bin"}
    run = subprocess.run([str(EXTC), "-w", "--check-only", path], cwd=ROOT, env=env,
                         capture_output=True, text=True)
    lines = []
    for line in (run.stdout + run.stderr).splitlines():
        if not PATTERN.match(line):
            continue
        if "argLvl=unknown" in line:
            continue
        lines.append(line.strip())
    return lines


def main():
    if not EXTC.exists():
        print("  skip  没有 build/extc（先 make）")
        return 0
    found = []
    for path in corpus():
        found.extend(witness(path))
    found.sort()
    if "--write" in sys.argv:
        BASELINE.write_text("\n".join(found) + ("\n" if found else ""))
        print(f"  写入基线：{len(found)} 条")
        return 0
    known = set()
    if BASELINE.exists():
        known = {ln.strip() for ln in BASELINE.read_text().splitlines() if ln.strip()}
    new = [ln for ln in found if ln not in known]
    gone = sorted(known - set(found))
    print(f"  见证差异 {len(found)} 条（基线 {len(known)}，新增 {len(new)}，消失 {len(gone)}）")
    if gone:
        print(f"  收紧棘轮：把 tools/lvl-witness-baseline.txt 重写（--write）")
    if new:
        for line in new[:10]:
            print(f"  NEW  {line}")
        print("  ✗ 见证与检查器的结论不一致，变多了")
        return 1
    print("  ok  见证差异没有增加")
    return 0


if __name__ == "__main__":
    sys.exit(main())
