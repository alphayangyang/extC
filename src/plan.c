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
#include <stdlib.h>

enum { PLAN_ARENA_LEVEL = 1u << 0, PLAN_ZONE_LEVEL = 1u << 1,
       PLAN_ARENA_ARG = 1u << 2, PLAN_NEED_TEMP = 1u << 3,
       /* FuncDef 的 arena/pool 族（X2 第四步）*/
       PLAN_USES_HOME = 1u << 4, PLAN_MAY_USE_ARENA = 1u << 5, PLAN_MAKES_POOL = 1u << 6,
       /* Stmt 族（X2 第四步；`forStep` 仍是半语法，留在 AST 上）*/
       PLAN_COND_ALLOCS = 1u << 7,
       /* 协程族（X2 第五步；`coroBoxed` 因为 codegen 会写它，留到 X3）*/
       PLAN_IS_CORO = 1u << 8, PLAN_YIELD_TYPE = 1u << 9, PLAN_CORO_FRAME_TYPE = 1u << 10,
       PLAN_CORO_NEEDS_ZONE = 1u << 11, PLAN_CORO_PROTO = 1u << 12 };

typedef struct {
    int arenaLevel, zoneLevel, arenaArg;      /* Expr 族 */
    bool needTemp;
    bool usesHome, mayUseArena, makesPool;    /* FuncDef 的 arena/pool 族 */
    bool condAllocs;                          /* Stmt 族 */
    bool isCoro, coroNeedsZone;               /* 协程族 */
    Type *yieldType, *coroFrameType;
    int coroProto;
    unsigned setMask;
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
    /* X2 第三步：权威在侧表；AST 字段已删除 */
}
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->zoneLevel = v; s->setMask |= PLAN_ZONE_LEVEL;
}
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->arenaArg = v; s->setMask |= PLAN_ARENA_ARG;
}
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->needTemp = v; s->setMask |= PLAN_NEED_TEMP;
}

void planSetUsesHome(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->usesHome = v; s->setMask |= PLAN_USES_HOME;
}
void planSetMayUseArena(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->mayUseArena = v; s->setMask |= PLAN_MAY_USE_ARENA;
}
void planSetMakesPool(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->makesPool = v; s->setMask |= PLAN_MAKES_POOL;
}
void planSetCondAllocs(Stmt *st, bool v) {
    if (!st) return;
    PlanSlot *s = slotFor(st, true);
    s->condAllocs = v; s->setMask |= PLAN_COND_ALLOCS;
}

void planSetIsCoro(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->isCoro = v; s->setMask |= PLAN_IS_CORO;
}
void planSetYieldType(FuncDef *f, Type *v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->yieldType = v; s->setMask |= PLAN_YIELD_TYPE;
}
void planSetCoroFrameType(FuncDef *f, Type *v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->coroFrameType = v; s->setMask |= PLAN_CORO_FRAME_TYPE;
}
void planSetCoroNeedsZone(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->coroNeedsZone = v; s->setMask |= PLAN_CORO_NEEDS_ZONE;
}
void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->coroProto = v; s->setMask |= PLAN_CORO_PROTO;
}

/* ---- 访问器（对外签名不变） -------------------------------------------------------- */

FuncDef *planCallee(const Expr *e) { return e ? e->func : NULL; }
FuncDef *planTemplate(const FuncDef *f) { return f ? f->tmpl : NULL; }
const char *planInstName(const FuncDef *f) { return f ? f->instName : NULL; }

int planArenaLevel(const Expr *e) {
    PlanSlot *s = slotFor(e, false);
    return (s && (s->setMask & PLAN_ARENA_LEVEL)) ? s->arenaLevel : 0;
}
int planZoneLevel(const Expr *e) {
    PlanSlot *s = slotFor(e, false);
    return (s && (s->setMask & PLAN_ZONE_LEVEL)) ? s->zoneLevel : 0;
}
int planArenaArg(const Expr *e) {
    PlanSlot *s = slotFor(e, false);
    return (s && (s->setMask & PLAN_ARENA_ARG)) ? s->arenaArg : 0;
}
bool planNeedTemp(const Expr *e) {
    PlanSlot *s = slotFor(e, false);
    return (s && (s->setMask & PLAN_NEED_TEMP)) ? s->needTemp : false;
}

bool planUsesHome(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_USES_HOME) ? s->usesHome : false;
}
bool planMayUseArena(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_MAY_USE_ARENA) ? s->mayUseArena : false;
}
bool planMakesPool(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_MAKES_POOL) ? s->makesPool : false;
}
bool planCondAllocs(const Stmt *st) {
    PlanSlot *s = slotFor(st, false);
    return s && (s->setMask & PLAN_COND_ALLOCS) ? s->condAllocs : false;
}
Stmt *planForStep(const Stmt *s) { return s ? s->forStep : NULL; }

bool planIsCoro(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_IS_CORO) ? s->isCoro : false;
}
Type *planYieldType(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_YIELD_TYPE) ? s->yieldType : NULL;
}
Type *planCoroFrameType(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_CORO_FRAME_TYPE) ? s->coroFrameType : NULL;
}
bool planCoroNeedsZone(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_CORO_NEEDS_ZONE) ? s->coroNeedsZone : false;
}
int planCoroProto(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_CORO_PROTO) ? s->coroProto : 0;
}
bool planCoroBoxed(const FuncDef *f) { return f ? f->coroBoxed : false; }
