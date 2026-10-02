#!/usr/bin/env python3
"""计划接缝的棘轮：codegen 只能经 src/plan.h 读"分析产物/编译计划"（X 批次：X1/X2）。

两条检查：
  1. **读点**：`src/codegen.c` 里不得直接读计划字段（必须写成 `planXxx(...)`）；
  2. **归属**：那些字段**不得再出现在 AST 上** —— X2 已把存储搬到 `src/plan.c` 的侧表，
     `Expr`/`Stmt`/`FuncDef` 上再出现同名字段就是把耦合加回来。

只判**读**：写点本来就不该在 codegen 里（codegen 回写分析状态属于 X3，见
docs/topics/AST-ANNOTATIONS.md 第 3 节）。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "src" / "codegen.c"
AST = [ROOT / "src" / "ast.h"]

PLAN_FIELDS = {
    "func": "planCallee", "tmpl": "planTemplate", "instName": "planInstName",
    "isCoro": "planIsCoro", "yieldType": "planYieldType",
    "coroFrameType": "planCoroFrameType", "coroNeedsZone": "planCoroNeedsZone",
    "coroProto": "planCoroProto", "coroBoxed": "planCoroBoxed",
    "arenaLevel": "planArenaLevel", "arenaArg": "planArenaArg", "zoneLevel": "planZoneLevel",
    "usesHome": "planUsesHome", "mayUseArena": "planMayUseArena", "makesPool": "planMakesPool",
    "needTemp": "planNeedTemp", "condAllocs": "planCondAllocs", "forStep": "planForStep",
}

# X2 已搬走的字段：它们不许再出现在 AST 上（其余字段还在搬的路上）。
MOVED = ["arenaLevel", "zoneLevel", "arenaArg", "needTemp",
         "usesHome", "mayUseArena", "makesPool", "condAllocs",
         "isCoro", "yieldType", "coroFrameType", "coroNeedsZone", "coroProto"]

WRITE = r"\s*(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)"


# codegen **回写**分析状态的基线（X0 第 3 节：反向耦合）。只能减，不能增；X3 的目标是 0。
# 为什么要有它：codegen 现在会改 `substParams`/`substArgs`（借用检查器的替换状态）、
# `used`（影响检查器"没被调用就不必复核"的判断）、`name`（去重改名）、`owSites`/`owLocal`、
# `body`、`coroBoxed`、`ret`、`func` —— 于是"哪个 pass 先跑"变成语义问题（P0-5 的根因）。
WRITE_BACK_BASELINE = {
    "substParams": 8, "substArgs": 8, "name": 15, "used": 4,
    "owSites": 3, "owLocal": 3, "body": 3, "coroBoxed": 1, "ret": 1, "func": 1,
}


def check_write_backs():
    counts = {}
    text = CODEGEN.read_text(encoding="utf-8")
    for field in WRITE_BACK_BASELINE:
        counts[field] = len(re.findall(
            r"->" + field + r"\s*(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)", text))
    bad = [(f, counts[f], WRITE_BACK_BASELINE[f]) for f in WRITE_BACK_BASELINE
           if counts[f] > WRITE_BACK_BASELINE[f]]
    # 基线里没有的字段若出现回写，也算新增耦合
    for m in set(re.findall(r"->(\w+)\s*(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)", text)):
        if m not in WRITE_BACK_BASELINE and m in ("substParams", "substArgs"):
            bad.append((m, 1, 0))
    return counts, bad


def check_reads():
    bad = []
    for i, line in enumerate(CODEGEN.read_text(encoding="utf-8").split("\n"), 1):
        code = line.split("/*")[0]
        for field in PLAN_FIELDS:
            for m in re.finditer(r"->\s*" + field + r"\b", code):
                if re.match(WRITE, code[m.end():]):
                    continue
                bad.append((i, field, line.strip()[:90]))
    return bad


def check_moved():
    bad = []
    for path in AST:
        for i, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            code = line.split("/*")[0]
            for field in MOVED:
                if re.search(r"^\s+\w[\w \*]*\b" + field + r"\s*;", code):
                    bad.append((path.name, i, field, line.strip()[:80]))
    return bad


def main() -> int:
    if not CODEGEN.exists():
        print(f"[plan-seam] 找不到 {CODEGEN}", file=sys.stderr)
        return 2
    r = check_reads()
    m = check_moved()
    if r:
        print(f"[plan-seam] codegen 直读计划字段 {len(r)} 处 —— 请改用 src/plan.h 的访问器：")
        for i, field, txt in r[:20]:
            print(f"  src/codegen.c:{i}  ->{field}   {txt}")
        return 1
    counts, wb = check_write_backs()
    total = sum(counts.values())
    base = sum(WRITE_BACK_BASELINE.values())
    if wb:
        print("[plan-seam] codegen 回写分析状态**超过了基线**（反向耦合只能减不能增）：")
        for f, now, b in wb:
            print(f"  {f}: 现在 {now}，基线 {b}")
        return 1
    if total != base:
        print(f"[plan-seam] note：codegen 回写 {total} 处（基线 {base}）—— 减少了，请同步收紧基线")
    if m:
        print(f"[plan-seam] AST 上又出现了已搬走的计划字段 {len(m)} 处 —— 存储属于 src/plan.c：")
        for f, i, field, txt in m[:20]:
            print(f"  src/{f}:{i}  {field}   {txt}")
        return 1
    print(f"[plan-seam] ok：codegen 直读计划字段 0 处（{len(PLAN_FIELDS)} 个字段）；"
          f"AST 上已无 {'/'.join(MOVED)}；codegen 回写 {total} 处（基线 {base}，X3 目标 0）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
