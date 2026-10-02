#!/usr/bin/env python3
"""计划接缝的棘轮：codegen 只能经 src/plan.h 读"分析产物/编译计划"（X 批次：X1）。

为什么需要它：AST 上的这些字段是**检查器写、codegen 读**的耦合面（清单见
docs/topics/AST-ANNOTATIONS.md 第 2 节）。X1 把 codegen 的 180+ 个读点收口到 plan.h，
X2 会把存储搬走；在那之前，一次不经意的 `x->arenaLevel` 就会把接缝重新打穿，
而**编译器不会报错**（字段还在）。所以这里用一条棘轮把它钉住。

只检查**读**：`x->field = …` / `++` / `--` / `+=` 是写点（codegen 回写分析状态属于 X3，
见 AST-ANNOTATIONS.md 第 3 节），本脚本不判它们。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "src" / "codegen.c"

# 字段 -> 应有的访问器（读点必须写成 访问器(基座)）
PLAN_FIELDS = {
    "func": "planCallee", "tmpl": "planTemplate", "instName": "planInstName",
    "isCoro": "planIsCoro", "yieldType": "planYieldType",
    "coroFrameType": "planCoroFrameType", "coroNeedsZone": "planCoroNeedsZone",
    "coroProto": "planCoroProto", "coroBoxed": "planCoroBoxed",
    "arenaLevel": "planArenaLevel", "arenaArg": "planArenaArg", "zoneLevel": "planZoneLevel",
    "usesHome": "planUsesHome", "mayUseArena": "planMayUseArena", "makesPool": "planMakesPool",
    "needTemp": "planNeedTemp", "condAllocs": "planCondAllocs", "forStep": "planForStep",
}

WRITE = r"\s*(?:=[^=]|\+\+|--|\+=|-=|\|=|&=)"


def main() -> int:
    if not TARGET.exists():
        print(f"[plan-seam] 找不到 {TARGET}", file=sys.stderr)
        return 2
    bad = []
    for i, line in enumerate(TARGET.read_text(encoding="utf-8").split("\n"), 1):
        code = line.split("/*")[0]          # 注释里的旧写法不算（也不该改注释的历史）
        for field in PLAN_FIELDS:
            for m in re.finditer(r"->\s*" + field + r"\b", code):
                rest = code[m.end():]
                if re.match(WRITE, rest):
                    continue                 # 写点：X3 的范围
                bad.append((i, field, line.strip()[:90]))
    if bad:
        print(f"[plan-seam] codegen 直读计划字段 {len(bad)} 处 —— 请改用 src/plan.h 的访问器：")
        for i, field, txt in bad[:20]:
            print(f"  src/codegen.c:{i}  ->{field}   {txt}")
        print(f"  （字段 → 访问器对照见 tools/check_plan_seam.py 与 src/plan.h）")
        return 1
    print(f"[plan-seam] ok：codegen 直读计划字段 0 处（{len(PLAN_FIELDS)} 个字段，读点全部经 src/plan.h）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
