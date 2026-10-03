/* The plan side table: accessors, setters, and the per-call-site alternative answers.
 *
 * Storage now lives in `results.c`: one result slot per node, addressed by a **dense
 * node id** (see results.h for why: input immutability, id-not-pointer addressing, one
 * result object per owner). This file keeps what is specific to the plan: the meaning of
 * each field, the setters that record them, and the two tables that answer "what does
 * this call site resolve to".
 *
 * A setter with a NULL node is a no-op, and a lookup for a NULL node misses: some plan
 * questions are asked about nodes that do not exist.
 *
 * Not every plan field is here yet. The ones that still live on an AST node are the
 * resolved callee (`Expr.func`) and the `for` step (`Stmt.forStep`); their accessors below
 * read the node. Do not add storage for them without also making the instance set
 * explicit (`forStep` is half syntax -- the parser owns it). */
#include "plan.h"
#include "results.h"
#include <stdlib.h>

/* One bit per stored field. Macros, not an enum: a pedantic C11 compiler rejects
 * enumerator values above 31, and the analysis side has already grown past 32. */
#define PLAN_ARENA_LEVEL         (1ull << 0)
#define PLAN_ZONE_LEVEL          (1ull << 1)
#define PLAN_ARENA_ARG           (1ull << 2)
#define PLAN_NEED_TEMP           (1ull << 3)
#define PLAN_USES_HOME           (1ull << 4)
#define PLAN_MAY_USE_ARENA       (1ull << 5)
#define PLAN_MAKES_POOL          (1ull << 6)
#define PLAN_COND_ALLOCS         (1ull << 7)
#define PLAN_IS_CORO             (1ull << 8)
#define PLAN_YIELD_TYPE          (1ull << 9)
#define PLAN_CORO_FRAME_TYPE     (1ull << 10)
#define PLAN_CORO_NEEDS_ZONE     (1ull << 11)
#define PLAN_CORO_PROTO          (1ull << 12)
#define PLAN_INST_NAME           (1ull << 13)
#define PLAN_TEMPLATE            (1ull << 14)
#define PLAN_CORO_BOXED          (1ull << 15)
#define PLAN_FOR_STEP_DROPPED    (1ull << 16)
#define PLAN_CALLEE              (1ull << 17)
#define PLAN_USED                (1ull << 18)
#define PLAN_CNAME               (1ull << 19)
#define PLAN_BUILTIN_HOLDER      (1ull << 20)
#define PLAN_CORO_OF             (1ull << 21)
#define AN_LAM_SIG               (1ull << 22)
#define AN_MAKES_POOL_ANY        (1ull << 23)
#define PLAN_IMPL_TRAIT          (1ull << 24)
#define PLAN_IMPL_TARGET         (1ull << 25)
#define PLAN_USED_DYN            (1ull << 26)
#define PLAN_USES_DYN            (1ull << 27)
#define PLAN_PAR_WORKER          (1ull << 28)
#define PLAN_DOM_NEW             (1ull << 29)
#define PLAN_VIEW_OF             (1ull << 30)
#define AN_REF_DEPTH             (1ull << 31)
#define AN_HOME_DEPTH            (1ull << 32)
#define AN_LEXICAL_LEVEL         (1ull << 33)
#define AN_STORED_AT             (1ull << 34)


/* The analysis half of the slot, by name: the checker's own working state, which no later
 * phase reads. Named here so the two audiences cannot quietly merge -- a field that code
 * generation starts reading must move out of this list (and get a `planXxx` accessor). */
const char *const planAnalysisFields[] = { "lamSig", "makesPoolAny",
                                           "refDepth", "homeDepth", "lexicalLevel", "storedAt" };
const size_t planAnalysisFieldCount = sizeof planAnalysisFields / sizeof planAnalysisFields[0];

/* ---- setters (called by the checker) ----------------------------------------------- */

void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaLevel = v; s->setMask |= PLAN_ARENA_LEVEL;
}
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->zoneLevel = v; s->setMask |= PLAN_ZONE_LEVEL;
}
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaArg = v; s->setMask |= PLAN_ARENA_ARG;
}
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->needTemp = v; s->setMask |= PLAN_NEED_TEMP;
}

void planSetUsesHome(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->usesHome = v; s->setMask |= PLAN_USES_HOME;
}
void planSetMayUseArena(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->mayUseArena = v; s->setMask |= PLAN_MAY_USE_ARENA;
}
void planSetMakesPool(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->makesPool = v; s->setMask |= PLAN_MAKES_POOL;
}
void planSetCondAllocs(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->condAllocs = v; s->setMask |= PLAN_COND_ALLOCS;
}

void planSetIsCoro(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->isCoro = v; s->setMask |= PLAN_IS_CORO;
}
void planSetYieldType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->yieldType = v; s->setMask |= PLAN_YIELD_TYPE;
}
void planSetCoroFrameType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroFrameType = v; s->setMask |= PLAN_CORO_FRAME_TYPE;
}
void planSetCoroNeedsZone(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroNeedsZone = v; s->setMask |= PLAN_CORO_NEEDS_ZONE;
}
void planSetInstName(FuncDef *f, const char *name) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->instName = name; s->setMask |= PLAN_INST_NAME;
}

void planSetTemplate(FuncDef *f, FuncDef *tmpl) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->tmpl = tmpl; s->setMask |= PLAN_TEMPLATE;
}

void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroProto = v; s->setMask |= PLAN_CORO_PROTO;
}

void planSetCoroBoxed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroBoxed = v; s->setMask |= PLAN_CORO_BOXED;
}

/* ---- accessors (signatures are the public surface) --------------------------------- */

FuncDef *planTemplate(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_TEMPLATE) ? s->tmpl : NULL;
}
const char *planInstName(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
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
    NodeResults *r = resultsOf(e, false);
    return (r && (r->setMask & PLAN_CALLEE)) ? r->func : NULL;
}

void planSetCallee(Expr *e, FuncDef *callee) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (r) {
        r->func = callee;
        r->setMask |= PLAN_CALLEE;
    }
}

/* Was this function called? Storage is the plan side table (it used to be `FuncDef.used`).
 * The default is false: a function nothing recorded a call for is not emitted, which is the
 * safe direction -- a missing call site would otherwise emit a body nothing references. */
/* The C name a binding is emitted under. The checker decides it while binding a name to
 * its declaration (a `Sym`); code generation prints it. Storage is the plan side table; it
 * used to be `Expr.u.ident.cname` and `Stmt.u.var.cname`.
 *
 * **Not for `Param`**: a parameter keeps its own field, because a `Param` is stored by
 * value (the coroutine frame copies them) and a table addressed by node pointer cannot
 * follow a copy. Measured: putting it here made every coroutine entry function lose its
 * parameter identifier and the generated C stopped compiling. */
const char *planCName(const void *node) {
    NodeResults *r = resultsOf(node, false);
    if (!r || !(r->setMask & PLAN_CNAME)) return NULL;
    /* An empty name means "no name of its own": callers fall back to the declaration's name
     * (`planCName(x) ? planCName(x) : x->name`), and the checker does write "" for a binding
     * it cannot name. Returning "" would win that fallback and emit C with no identifier. */
    return r->cname && r->cname[0] ? r->cname : NULL;
}

void planSetCName(void *node, const char *name, ResultKind kind) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, kind, __LINE__);
    if (!r) return;
    r->cname = name;
    r->setMask |= PLAN_CNAME;
}

/* ---- struct-definition facts (T4, first family) ---- */

bool planBuiltinHolder(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && (r->setMask & PLAN_BUILTIN_HOLDER) ? r->builtinHolder : false;
}
void planSetBuiltinHolder(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->builtinHolder = v; r->setMask |= PLAN_BUILTIN_HOLDER;
}

FuncDef *planCoroOf(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && (r->setMask & PLAN_CORO_OF)) ? r->coroOf : NULL;
}
void planSetCoroOf(StructDef *sd, FuncDef *f) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->coroOf = f; r->setMask |= PLAN_CORO_OF;
}

const char *anLamSig(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && (r->setMask & AN_LAM_SIG)) ? r->an.lamSig : NULL;
}
void anSetLamSig(StructDef *sd, const char *sig) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.lamSig = sig; r->setMask |= AN_LAM_SIG;
}

bool anMakesPoolAny(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && (r->setMask & AN_MAKES_POOL_ANY) ? r->an.makesPoolAny : false;
}
void anSetMakesPoolAny(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.makesPoolAny = v; r->setMask |= AN_MAKES_POOL_ANY;
}

/* ---- depth and origin facts (T4, fourth family) -------------------------------------
 *
 * `void *` on purpose: `refDepth` lives on an `Expr` **and** on the checker's `Sym`, and one
 * accessor answers both. */
#define AN_D_GET(fn, field, bit, dflt)                                     \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && (r->setMask & (bit))) ? r->an.field : (dflt);         \
    }
/* One kind for the whole group: `refDepth` is stamped on an `Expr` **and** on the checker's
 * `Sym`, and a slot has one kind. Both are pointer-stable, so nothing else collides --
 * `RKIND_EXPR` is the honest label (`Sym` is not an AST node, but it is not a `Stmt` either,
 * and a slot is written as one kind for its whole life). */
#define AN_D_SET(fn, field, bit)                                           \
    void fn(void *node, int v) {                                           \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; r->setMask |= (bit);                              \
    }
AN_D_GET(anRefDepth, refDepth, AN_REF_DEPTH, 0)
AN_D_SET(anSetRefDepth, refDepth, AN_REF_DEPTH)
AN_D_GET(anHomeDepth, homeDepth, AN_HOME_DEPTH, 0)
AN_D_SET(anSetHomeDepth, homeDepth, AN_HOME_DEPTH)
AN_D_GET(anLexicalLevel, lexicalLevel, AN_LEXICAL_LEVEL, 0)
AN_D_SET(anSetLexicalLevel, lexicalLevel, AN_LEXICAL_LEVEL)
AN_D_GET(anStoredAt, storedAt, AN_STORED_AT, 0)
AN_D_SET(anSetStoredAt, storedAt, AN_STORED_AT)

/* ---- call-site markers (T4, third family) -------------------------------------------
 *
 * Three builtin forms have no `Expr.func` to recognise them by -- measured: an assoc
 * builtin call and a body-less declaration both reached code generation with a NULL
 * callee, so keying on the callee could never fire. Each gets its own marker, written by
 * the checker at the call site and read by code generation. */

FuncDef *planParWorker(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return (r && (r->setMask & PLAN_PAR_WORKER)) ? r->parWorker : NULL;
}
void planSetParWorker(Expr *e, FuncDef *wf) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->parWorker = wf; r->setMask |= PLAN_PAR_WORKER;
}

bool planDomNew(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && (r->setMask & PLAN_DOM_NEW) ? r->domNew : false;
}
void planSetDomNew(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->domNew = v; r->setMask |= PLAN_DOM_NEW;
}

bool planViewOf(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && (r->setMask & PLAN_VIEW_OF) ? r->viewOf : false;
}
void planSetViewOf(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->viewOf = v; r->setMask |= PLAN_VIEW_OF;
}

/* ---- impl / trait / module facts (T4, second family) -------------------------------- */

TraitDef *planImplTrait(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && (r->setMask & PLAN_IMPL_TRAIT)) ? r->implTrait : NULL;
}
void planSetImplTrait(ImplDef *im, TraitDef *tr) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTrait = tr; r->setMask |= PLAN_IMPL_TRAIT;
}

Type *planImplTarget(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && (r->setMask & PLAN_IMPL_TARGET)) ? r->implTarget : NULL;
}
void planSetImplTarget(ImplDef *im, Type *t) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTarget = t; r->setMask |= PLAN_IMPL_TARGET;
}

bool planUsedDyn(const TraitDef *tr) {
    NodeResults *r = resultsOf(tr, false);
    return r && (r->setMask & PLAN_USED_DYN) ? r->usedDyn : false;
}
void planSetUsedDyn(TraitDef *tr, bool v) {
    if (!tr) return;
    NodeResults *r = resultsAs(tr, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usedDyn = v; r->setMask |= PLAN_USED_DYN;
}

bool planUsesDyn(const Module *m) {
    NodeResults *r = resultsOf(m, false);
    return r && (r->setMask & PLAN_USES_DYN) ? r->usesDyn : false;
}
void planSetUsesDyn(Module *m, bool v) {
    if (!m) return;
    NodeResults *r = resultsAs(m, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usesDyn = v; r->setMask |= PLAN_USES_DYN;
}

bool planUsed(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && (r->setMask & PLAN_USED) ? r->used : false;
}

void planSetUsed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->used = v;
        r->setMask |= PLAN_USED;
    }
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
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && (s->setMask & PLAN_ARENA_LEVEL)) ? s->arenaLevel : 0;
}
int planZoneLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && (s->setMask & PLAN_ZONE_LEVEL)) ? s->zoneLevel : 0;
}
int planArenaArg(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && (s->setMask & PLAN_ARENA_ARG)) ? s->arenaArg : 0;
}
bool planNeedTemp(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && (s->setMask & PLAN_NEED_TEMP)) ? s->needTemp : false;
}

bool planUsesHome(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_USES_HOME) ? s->usesHome : false;
}
bool planMayUseArena(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_MAY_USE_ARENA) ? s->mayUseArena : false;
}
bool planMakesPool(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_MAKES_POOL) ? s->makesPool : false;
}
bool planCondAllocs(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && (s->setMask & PLAN_COND_ALLOCS) ? s->condAllocs : false;
}
/* The step statement of a desugared `for` -- **syntax**, written by the parser on the loop
 * body, because `continue` has to reach it (C's `for` runs the step on the way out of a
 * `continue`; without this the desugared form looped forever, audit P0-15).
 *
 * The one thing analysis decides about it is that it disappeared: when the loop is
 * retargeted onto an iterator, the iterator's `next()` advances instead, so a plain
 * `continue` is right again and the label must not be emitted. That decision is recorded
 * as a result (`planSetForStepDropped`), never by editing the tree. */
Stmt *planForStep(const Stmt *s) {
    if (!s) return NULL;
    NodeResults *r = resultsOf(s, false);
    if (r && (r->setMask & PLAN_FOR_STEP_DROPPED) && r->forStepDropped) return NULL;
    return s->forStep;
}

void planSetForStepDropped(Stmt *s) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->forStepDropped = true;
    r->setMask |= PLAN_FOR_STEP_DROPPED;
}

bool planIsCoro(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_IS_CORO) ? s->isCoro : false;
}
Type *planYieldType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_YIELD_TYPE) ? s->yieldType : NULL;
}
Type *planCoroFrameType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_CORO_FRAME_TYPE) ? s->coroFrameType : NULL;
}
bool planCoroNeedsZone(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_CORO_NEEDS_ZONE) ? s->coroNeedsZone : false;
}
int planCoroProto(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_CORO_PROTO) ? s->coroProto : 0;
}
bool planCoroBoxed(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && (s->setMask & PLAN_CORO_BOXED) ? s->coroBoxed : false;
}
