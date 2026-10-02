/* 编译计划访问器 —— 见 plan.h 的说明（X1：只有接缝，没有搬家）。 */
#include "plan.h"

/* ---- ④ 身份 / 实例 ------------------------------------------------------------------ */

FuncDef *planCallee(const Expr *e) { return e ? e->func : NULL; }
FuncDef *planTemplate(const FuncDef *f) { return f ? f->tmpl : NULL; }

const char *planInstName(const FuncDef *f) { return f ? f->instName : NULL; }

/* ---- ③ arena / zone 计划 ------------------------------------------------------------ */

int planArenaLevel(const Expr *e) { return e ? e->plan.arenaLevel : 0; }
int planArenaArg(const Expr *e) { return e ? e->plan.arenaArg : 0; }
int planZoneLevel(const Expr *e) { return e ? e->plan.zoneLevel : 0; }

bool planUsesHome(const FuncDef *f) { return f ? f->usesHome : false; }
bool planMayUseArena(const FuncDef *f) { return f ? f->mayUseArena : false; }
bool planMakesPool(const FuncDef *f) { return f ? f->makesPool : false; }

bool planCondAllocs(const Stmt *s) { return s ? s->condAllocs : false; }
Stmt *planForStep(const Stmt *s) { return s ? s->forStep : NULL; }
bool planNeedTemp(const Expr *e) { return e ? e->plan.needTemp : false; }

/* ---- ③ 协程族 ---------------------------------------------------------------------- */

bool planIsCoro(const FuncDef *f) { return f ? f->isCoro : false; }
Type *planYieldType(const FuncDef *f) { return f ? f->yieldType : NULL; }
Type *planCoroFrameType(const FuncDef *f) { return f ? f->coroFrameType : NULL; }
bool planCoroNeedsZone(const FuncDef *f) { return f ? f->coroNeedsZone : false; }
int planCoroProto(const FuncDef *f) { return f ? f->coroProto : 0; }
bool planCoroBoxed(const FuncDef *f) { return f ? f->coroBoxed : false; }
