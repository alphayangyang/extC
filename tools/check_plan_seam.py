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
MOVED = ["arenaLevel", "zoneLevel", "arenaArg", "needTemp"]

WRITE = r"\s*(?:=(?!=)|\+\+|--|\+=|-=|\|=|&=)"


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
    if m:
        print(f"[plan-seam] AST 上又出现了已搬走的计划字段 {len(m)} 处 —— 存储属于 src/plan.c：")
        for f, i, field, txt in m[:20]:
            print(f"  src/{f}:{i}  {field}   {txt}")
        return 1
    print(f"[plan-seam] ok：codegen 直读计划字段 0 处（{len(PLAN_FIELDS)} 个字段）；"
          f"AST 上已无 {'/'.join(MOVED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
