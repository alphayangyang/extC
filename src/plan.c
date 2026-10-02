/* The plan side table and the accessors declared in plan.h.
 *
 * Storage: one open-addressing table keyed by node pointer. A pointer is unique for the
 * whole compilation, and the driver checks every module before generating any of them, so
 * the table is process-wide rather than per module. Slots are allocated with malloc and
 * never shrink; a slot carries the values plus a bitmask of which fields were set, so an
 * unset field reads as its default instead of as whatever calloc left there.
 *
 * A setter with a NULL node is a no-op, and a lookup for a NULL node misses: some plan
 * questions are asked about nodes that do not exist.
 *
 * Not every plan field is here yet. The ones that still live on an AST node are the
 * resolved callee (`Expr.func`) and the `for` step (`Stmt.forStep`); their accessors below
 * read the node. Do not add storage for them without also making the instance set
 * explicit (`forStep` is half syntax -- the parser owns it). */
#include "plan.h"
#include <stdlib.h>

enum { PLAN_ARENA_LEVEL = 1u << 0, PLAN_ZONE_LEVEL = 1u << 1,
       PLAN_ARENA_ARG = 1u << 2, PLAN_NEED_TEMP = 1u << 3,
       /* function-summary family */
       PLAN_USES_HOME = 1u << 4, PLAN_MAY_USE_ARENA = 1u << 5, PLAN_MAKES_POOL = 1u << 6,
       /* statement family (`forStep` is half syntax and stays on the AST) */
       PLAN_COND_ALLOCS = 1u << 7,
       /* coroutine family */
       PLAN_IS_CORO = 1u << 8, PLAN_YIELD_TYPE = 1u << 9, PLAN_CORO_FRAME_TYPE = 1u << 10,
       PLAN_CORO_NEEDS_ZONE = 1u << 11, PLAN_CORO_PROTO = 1u << 12,
       /* instance C name, the instance -> template back pointer, and "a handle of it was
        * made somewhere" (which makes it get a task even when never spawned) */
       PLAN_INST_NAME = 1u << 13, PLAN_TEMPLATE = 1u << 14, PLAN_CORO_BOXED = 1u << 15 };

typedef struct {
    int arenaLevel, zoneLevel, arenaArg;      /* Expr family */
    bool needTemp;
    bool usesHome, mayUseArena, makesPool;    /* function-summary family */
    bool condAllocs;                          /* statement family */
    bool isCoro, coroNeedsZone, coroBoxed;    /* coroutine family */
    Type *yieldType, *coroFrameType;
    int coroProto;
    const char *instName;                     /* instance C name */
    FuncDef *tmpl;                            /* instance -> template */
    unsigned setMask;
} PlanSlot;

static const void **g_keys;
static PlanSlot    *g_slots;
static size_t       g_cap, g_len;

static size_t hashKey(const void *k) {
    size_t h = (size_t)k >> 4;          /* nodes are 8/16-byte aligned: low bits carry nothing */
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

/* ---- setters (called by the checker) ----------------------------------------------- */

void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    PlanSlot *s = slotFor(e, true);
    s->arenaLevel = v; s->setMask |= PLAN_ARENA_LEVEL;
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
void planSetInstName(FuncDef *f, const char *name) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->instName = name; s->setMask |= PLAN_INST_NAME;
}

void planSetTemplate(FuncDef *f, FuncDef *tmpl) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->tmpl = tmpl; s->setMask |= PLAN_TEMPLATE;
}

void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->coroProto = v; s->setMask |= PLAN_CORO_PROTO;
}

void planSetCoroBoxed(FuncDef *f, bool v) {
    if (!f) return;
    PlanSlot *s = slotFor(f, true);
    s->coroBoxed = v; s->setMask |= PLAN_CORO_BOXED;
}

/* ---- accessors (signatures are the public surface) --------------------------------- */

FuncDef *planTemplate(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_TEMPLATE) ? s->tmpl : NULL;
}
const char *planInstName(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_INST_NAME) ? s->instName : NULL;
}

/* ---- the resolved callee, per enclosing instance -----------------------------------
 *
 * A call site inside a generic body is **one node shared by every instance** of that
 * body: `wrap<T>` with two type arguments has one `pick(x)` node, and the instance that
 * callee resolves to depends on which `wrap` is being emitted. Keeping a single pointer
 * on the node means the last resolution wins -- measured: with `wrap<i32>` and
 * `wrap<meter>` both emitted functions called the same `pick_*`, the other instance was
 * never emitted at all, and the generated C did not compile.
 *
 * So the deferred-call fixpoint records one entry per (call site, enclosing instance),
 * and code generation asks for the entry of the body it is emitting: `planEnterFunc` for
 * a function instance, `planEnterInst` for the type instance whose methods are being
 * emitted. A call site that belongs to no instance keeps the single pointer on the node,
 * which is the answer everywhere else. */
typedef struct {
    const Expr    *node;      /* the call site */
    const FuncDef *enclosing; /* the function instance whose body it was resolved in */
    const Type    *inst;      /* or the type instance, for a method body of a generic struct */
    FuncDef       *callee;    /* what the call resolves to inside that body */
} AltCallee;

static AltCallee *g_alt;
static size_t     g_altLen, g_altCap;
static const FuncDef *g_emitFunc;   /* the function instance currently being emitted */
static const Type    *g_emitInst;   /* the type instance whose methods are being emitted */

/* Does this entry describe the body code generation is emitting right now? */
static bool altMatches(const AltCallee *a) {
    if (g_emitFunc && a->enclosing == g_emitFunc) return true;
    if (g_emitInst && a->inst == g_emitInst) return true;
    return false;
}

FuncDef *planCallee(const Expr *e) {
    if (!e) return NULL;
    if (g_emitFunc || g_emitInst) {
        for (size_t i = g_altLen; i > 0; i--) {
            const AltCallee *a = &g_alt[i - 1];
            if (a->node == e && altMatches(a)) return a->callee;
        }
    }
    return e->func;
}

void planSetCallee(Expr *e, FuncDef *callee) {
    if (!e) return;
    e->func = callee;                     /* the fallback, and the answer outside instances */
}

void planSetAltCallee(Expr *e, FuncDef *enclosing, const Type *inst, FuncDef *callee) {
    if (!e || (!enclosing && !inst)) return;
    for (size_t i = 0; i < g_altLen; i++) {
        AltCallee *a = &g_alt[i];
        if (a->node == e && a->enclosing == enclosing && a->inst == inst) {
            a->callee = callee;           /* idempotent: the fixpoint replays rounds */
            return;
        }
    }
    if (g_altLen == g_altCap) {
        g_altCap = g_altCap ? g_altCap * 2 : 64;
        AltCallee *na = realloc(g_alt, g_altCap * sizeof *na);
        if (!na) abort();
        g_alt = na;
    }
    g_alt[g_altLen].node = e;
    g_alt[g_altLen].enclosing = enclosing;
    g_alt[g_altLen].inst = inst;
    g_alt[g_altLen].callee = callee;
    g_altLen++;
}

void planEnterFunc(const FuncDef *f) { g_emitFunc = f; }
void planLeaveFunc(void) { g_emitFunc = NULL; }
void planEnterInst(const Type *t) { g_emitInst = t; }
void planLeaveInst(void) { g_emitInst = NULL; }

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
bool planCoroBoxed(const FuncDef *f) {
    PlanSlot *s = slotFor(f, false);
    return s && (s->setMask & PLAN_CORO_BOXED) ? s->coroBoxed : false;
}
