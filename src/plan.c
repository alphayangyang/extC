/* 编译计划访问器 + 计划侧表 —— 见 plan.h 的说明。
 *
 * X2 第二步：`Expr.plan` 的**存储**开始搬到这张按节点寻址的侧表上。现在是**过渡态**：
 *   · 写入方（检查器）改经 setter —— setter 同时写侧表**与** AST 字段；
 *   · 读取方（codegen）仍读 AST 字段（权威未变 ⇒ 行为不变）；
 *   · 访问器在 `EXTC_DBG=1` 下比较"侧表里应有的值"与"AST 字段里的值"，**并且**在
 *     "字段非默认值却没有侧表记录"时响亮 —— 那正是"某个写点没走 setter"。
 * 下一步（X2 第三步）把权威翻到侧表、删掉 AST 字段。
 *
 * 生命周期：驱动是"先查完所有模块，再逐模块 codegen"（main.c），所以侧表**跨模块**存在、
 * 按 `Expr*` 寻址（指针在整个编译里唯一），不按模块重置。用 malloc/realloc 自己管。*/
#include "plan.h"
#include "dbg.h"
#include <stdlib.h>

enum { PLAN_ARENA_LEVEL = 1u << 0, PLAN_ZONE_LEVEL = 1u << 1,
       PLAN_ARENA_ARG = 1u << 2, PLAN_NEED_TEMP = 1u << 3 };

typedef struct {
    int arenaLevel, zoneLevel, arenaArg;
    unsigned setMask;
    bool needTemp;
} PlanSlot;

static const void **g_keys;
static PlanSlot    *g_slots;
static size_t       g_cap, g_len;

static size_t hashKey(const void *k) {
    size_t h = (size_t)k >> 4;          /* 节点是 8/16 字节对齐的：低位没有信息 */
    h *= 0x9E3779B97F4A7C15ull;
    return h;
}

static void grow(void) {
    size_t ncap = g_cap ? g_cap * 2 : 256;
    const void **nk = calloc(ncap, sizeof *nk);
    PlanSlot *ns = calloc(ncap, sizeof *ns);
    if (!nk || !ns) abort();
    for (size_t i = 0; i < g_cap; i++) {
        if (!g_keys[i]) continue;
        size_t j = hashKey(g_keys[i]) & (ncap - 1);
        while (nk[j]) j = (j + 1) & (ncap - 1);
        nk[j] = g_keys[i];
        ns[j] = g_slots[i];
    }
    free((void *)g_keys);
    free(g_slots);
    g_keys = nk; g_slots = ns; g_cap = ncap;
}

static PlanSlot *slotFor(const void *key, bool create) {
    if (!key) return NULL;
    if (g_cap == 0 || (create && (g_len + 1) * 10 >= g_cap * 7)) grow();
    size_t j = hashKey(key) & (g_cap - 1);
    while (g_keys[j]) {
        if (g_keys[j] == key) return &g_slots[j];
        j = (j + 1) & (g_cap - 1);
    }
    if (!create) return NULL;
    g_keys[j] = key;
    g_len++;
    return &g_slots[j];
}

/* ---- 写入方（检查器）的 setter ------------------------------------------------------ */

void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->arenaLevel = v; s->setMask |= PLAN_ARENA_LEVEL;
    e->plan.arenaLevel = v;                       /* 过渡期：两处都写 */
}
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->zoneLevel = v; s->setMask |= PLAN_ZONE_LEVEL;
    e->plan.zoneLevel = v;
}
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->arenaArg = v; s->setMask |= PLAN_ARENA_ARG;
    e->plan.arenaArg = v;
}
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->needTemp = v; s->setMask |= PLAN_NEED_TEMP;
    e->plan.needTemp = v;
}

/* ---- 读取方（codegen）—— 过渡期的一致性看守 ---------------------------------------- */

static void checkShadow(const Expr *e, unsigned which, int fieldVal, const char *what) {
    PlanSlot *s = slotFor(e, false);
    if (s && (s->setMask & which)) {
        int sv = (which == PLAN_ARENA_LEVEL) ? s->arenaLevel
               : (which == PLAN_ZONE_LEVEL) ? s->zoneLevel
               : (which == PLAN_ARENA_ARG) ? s->arenaArg : (s->needTemp ? 1 : 0);
        EXTC_DBG_ASSERT_MSGF(sv == fieldVal,
                             "plan side table has %s=%d but Expr.plan has %d", what, sv, fieldVal);
        return;
    }
    EXTC_DBG_ASSERT_MSGF(fieldVal == 0,
                         "%s=%d has no plan-side record: a write site skipped its setter",
                         what, fieldVal);
}

/* ---- 访问器（对外签名不变） -------------------------------------------------------- */

FuncDef *planCallee(const Expr *e) { return e ? e->func : NULL; }
FuncDef *planTemplate(const FuncDef *f) { return f ? f->tmpl : NULL; }
const char *planInstName(const FuncDef *f) { return f ? f->instName : NULL; }

int planArenaLevel(const Expr *e) {
    int v = e ? e->plan.arenaLevel : 0;
    if (e && extcDbgOn()) checkShadow(e, PLAN_ARENA_LEVEL, v, "arenaLevel");   /* 过渡期看守；发布版零开销 */
    return v;
}
int planZoneLevel(const Expr *e) {
    int v = e ? e->plan.zoneLevel : 0;
    if (e && extcDbgOn()) checkShadow(e, PLAN_ZONE_LEVEL, v, "zoneLevel");   /* 过渡期看守；发布版零开销 */
    return v;
}
int planArenaArg(const Expr *e) {
    int v = e ? e->plan.arenaArg : 0;
    if (e && extcDbgOn()) checkShadow(e, PLAN_ARENA_ARG, v, "arenaArg");   /* 过渡期看守；发布版零开销 */
    return v;
}
bool planNeedTemp(const Expr *e) {
    int v = (e && e->plan.needTemp) ? 1 : 0;
    if (e && extcDbgOn()) checkShadow(e, PLAN_NEED_TEMP, v, "needTemp");   /* 过渡期看守；发布版零开销 */
    return v != 0;
}

bool planUsesHome(const FuncDef *f) { return f ? f->usesHome : false; }
bool planMayUseArena(const FuncDef *f) { return f ? f->mayUseArena : false; }
bool planMakesPool(const FuncDef *f) { return f ? f->makesPool : false; }
bool planCondAllocs(const Stmt *s) { return s ? s->condAllocs : false; }
Stmt *planForStep(const Stmt *s) { return s ? s->forStep : NULL; }

bool planIsCoro(const FuncDef *f) { return f ? f->isCoro : false; }
Type *planYieldType(const FuncDef *f) { return f ? f->yieldType : NULL; }
Type *planCoroFrameType(const FuncDef *f) { return f ? f->coroFrameType : NULL; }
bool planCoroNeedsZone(const FuncDef *f) { return f ? f->coroNeedsZone : false; }
int planCoroProto(const FuncDef *f) { return f ? f->coroProto : 0; }
bool planCoroBoxed(const FuncDef *f) { return f ? f->coroBoxed : false; }
