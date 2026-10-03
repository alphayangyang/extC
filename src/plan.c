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
#define PLAN_EXT_DOM             (1ull << 35)
#define PLAN_FIELD               (1ull << 36)
#define PLAN_ASSOC_OWNER         (1ull << 37)
#define PLAN_DYN_RECV_VIA_REF    (1ull << 39)
#define PLAN_CALL_VIA_FN         (1ull << 40)
#define AN_MIN_AT                (1ull << 41)
#define AN_REUSE                 (1ull << 42)
#define AN_BORROWED              (1ull << 43)
#define AN_QUALIFIED             (1ull << 44)
#define AN_CONV_CHECK            (1ull << 45)
#define AN_NEED_OP               (1ull << 46)
#define AN_IS_ASSOC              (1ull << 47)
#define AN_LAM_CHECKED           (1ull << 48)
#define AN_LAM_INFER_RET         (1ull << 49)
#define AN_NEEDS_HOME            (1ull << 50)
#define AN_ALLOC_STATE           (1ull << 51)
#define AN_MAY_PRINT_STATE       (1ull << 52)
#define AN_FRESH_COUNT           (1ull << 53)
#define AN_ARENA_SITES           (1ull << 54)
#define AN_N_PARAM_SYMS          (1ull << 55)
#define AN_PARAM_SYMS            (1ull << 56)
#define PLAN_IS_EXT_TARGET       (1ull << 57)
#define PLAN_IS_PAR_WORKER       (1ull << 58)
#define AN_ADDR_MASK             (1ull << 59)
#define AN_CONT_MASK             (1ull << 60)
#define AN_OTHER_MASK            (1ull << 61)
#define AN_HOME_ADDR_MASK        (1ull << 62)
#define AN_HOME_CONT_MASK        (1ull << 63)
#define PLAN_PAR_TLS_ARENA       (1ull << 62)
#define PLAN_DEREF               (1ull << 63)
#define PLAN_BOXED_CORO          (1ull << 64)
#define AN_DUMMY_PAST_64         (1ull << 65)
#define AN_ADDR_FROM_LOCAL       (1ull << 64)


/* The analysis half of the slot, by name: the checker's own working state, which no later
 * phase reads. Named here so the two audiences cannot quietly merge -- a field that code
 * generation starts reading must move out of this list (and get a `planXxx` accessor). */
const char *const planAnalysisFields[] = { "lamSig", "makesPoolAny",
                                           "refDepth", "homeDepth", "lexicalLevel", "storedAt",
                                           "minAt", "reuse", "borrowed", "qualified" };
const size_t planAnalysisFieldCount = sizeof planAnalysisFields / sizeof planAnalysisFields[0];

/* ---- setters (called by the checker) ----------------------------------------------- */

void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaLevel = v; resultsSetBit(s, PLAN_ARENA_LEVEL);
}
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->zoneLevel = v; resultsSetBit(s, PLAN_ZONE_LEVEL);
}
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaArg = v; resultsSetBit(s, PLAN_ARENA_ARG);
}
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->needTemp = v; resultsSetBit(s, PLAN_NEED_TEMP);
}

void planSetUsesHome(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->usesHome = v; resultsSetBit(s, PLAN_USES_HOME);
}
void planSetMayUseArena(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->mayUseArena = v; resultsSetBit(s, PLAN_MAY_USE_ARENA);
}
void planSetMakesPool(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->makesPool = v; resultsSetBit(s, PLAN_MAKES_POOL);
}
void planSetCondAllocs(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->condAllocs = v; resultsSetBit(s, PLAN_COND_ALLOCS);
}

void planSetIsCoro(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->isCoro = v; resultsSetBit(s, PLAN_IS_CORO);
}
void planSetYieldType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->yieldType = v; resultsSetBit(s, PLAN_YIELD_TYPE);
}
void planSetCoroFrameType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroFrameType = v; resultsSetBit(s, PLAN_CORO_FRAME_TYPE);
}
void planSetCoroNeedsZone(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroNeedsZone = v; resultsSetBit(s, PLAN_CORO_NEEDS_ZONE);
}
void planSetInstName(FuncDef *f, const char *name) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->instName = name; resultsSetBit(s, PLAN_INST_NAME);
}

void planSetTemplate(FuncDef *f, FuncDef *tmpl) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->tmpl = tmpl; resultsSetBit(s, PLAN_TEMPLATE);
}

void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroProto = v; resultsSetBit(s, PLAN_CORO_PROTO);
}

void planSetCoroBoxed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroBoxed = v; resultsSetBit(s, PLAN_CORO_BOXED);
}

/* ---- accessors (signatures are the public surface) --------------------------------- */

FuncDef *planTemplate(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_TEMPLATE) ? s->tmpl : NULL;
}
const char *planInstName(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_INST_NAME) ? s->instName : NULL;
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
    return (r && resultsBit(r, PLAN_CALLEE)) ? r->func : NULL;
}

void planSetCallee(Expr *e, FuncDef *callee) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (r) {
        r->func = callee;
        resultsSetBit(r, PLAN_CALLEE);
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
    if (!r || !resultsBit(r, PLAN_CNAME)) return NULL;
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
    resultsSetBit(r, PLAN_CNAME);
}

/* ---- struct-definition facts (T4, first family) ---- */

bool planBuiltinHolder(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, PLAN_BUILTIN_HOLDER) ? r->builtinHolder : false;
}
void planSetBuiltinHolder(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->builtinHolder = v; resultsSetBit(r, PLAN_BUILTIN_HOLDER);
}

FuncDef *planCoroOf(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, PLAN_CORO_OF)) ? r->coroOf : NULL;
}
void planSetCoroOf(StructDef *sd, FuncDef *f) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->coroOf = f; resultsSetBit(r, PLAN_CORO_OF);
}

const char *anLamSig(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, AN_LAM_SIG)) ? r->an.lamSig : NULL;
}
void anSetLamSig(StructDef *sd, const char *sig) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.lamSig = sig; resultsSetBit(r, AN_LAM_SIG);
}

bool anMakesPoolAny(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, AN_MAKES_POOL_ANY) ? r->an.makesPoolAny : false;
}
void anSetMakesPoolAny(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.makesPoolAny = v; resultsSetBit(r, AN_MAKES_POOL_ANY);
}

/* ---- effect summary (T4, eighth family) --------------------------------------------
 *
 * Pure results (the checker writes them, only the checker reads them): the transitively
 * closed masks and `addrFromLocal`. The **state** that drives the closure
 * (`effState`/`effComplete`/`effUnknown`) and the call graph it walks (`callees`) are a
 * different question -- they are pass state, not a per-node result, and they move in their
 * own step. */
#define AEF_GET(fn, type, field, bit, dflt)                                \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AEF_SET(fn, type, field, bit)                                      \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }

AEF_GET(anAddrMask, uint64_t, addrMask, AN_ADDR_MASK, 0)
AEF_SET(anSetAddrMask, uint64_t, addrMask, AN_ADDR_MASK)
AEF_GET(anContMask, uint64_t, contMask, AN_CONT_MASK, 0)
AEF_SET(anSetContMask, uint64_t, contMask, AN_CONT_MASK)
AEF_GET(anOtherMask, uint64_t, otherMask, AN_OTHER_MASK, 0)
AEF_SET(anSetOtherMask, uint64_t, otherMask, AN_OTHER_MASK)
AEF_GET(anHomeAddrMask, uint64_t, homeAddrMask, AN_HOME_ADDR_MASK, 0)
AEF_SET(anSetHomeAddrMask, uint64_t, homeAddrMask, AN_HOME_ADDR_MASK)
AEF_GET(anHomeContMask, uint64_t, homeContMask, AN_HOME_CONT_MASK, 0)
AEF_SET(anSetHomeContMask, uint64_t, homeContMask, AN_HOME_CONT_MASK)
AEF_GET(anAddrFromLocal, bool, addrFromLocal, AN_ADDR_FROM_LOCAL, false)
AEF_SET(anSetAddrFromLocal, bool, addrFromLocal, AN_ADDR_FROM_LOCAL)

/* ---- per-function analysis facts (T4, seventh family) -------------------------------
 *
 * **Instances**: `funcInstance` builds an instance with `*in = *tmpl` -- a shallow copy.
 * Plain fields travel with that copy; a slot keyed by node pointer does not. So every fact
 * here is inherited explicitly in one place (`planInheritFuncFacts`, right after the copy).
 *
 * Staged on purpose: the accessors **prefer the slot and fall back to the field**, and the
 * setters write both. That keeps the two representations in step while the call sites move
 * over one at a time -- the same order that worked for `Expr.func` / `FuncDef.used` (T1). */
#define ANF_GET(fn, type, side, field, bit, dflt)                          \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->side . field : (dflt);     \
    }
#define ANF_SET(fn, type, side, field, bit)                                \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->side . field = v; resultsSetBit(r, bit);                          \
    }

ANF_GET(anIsAssoc, bool, an, isAssoc, AN_IS_ASSOC, false)
ANF_SET(anSetIsAssoc, bool, an, isAssoc, AN_IS_ASSOC)
ANF_GET(anLamChecked, bool, an, lamChecked, AN_LAM_CHECKED, false)
ANF_SET(anSetLamChecked, bool, an, lamChecked, AN_LAM_CHECKED)
ANF_GET(anLamInferRet, bool, an, lamInferRet, AN_LAM_INFER_RET, false)
ANF_SET(anSetLamInferRet, bool, an, lamInferRet, AN_LAM_INFER_RET)
ANF_GET(anNeedsHome, bool, an, needsHome, AN_NEEDS_HOME, false)
ANF_SET(anSetNeedsHome, bool, an, needsHome, AN_NEEDS_HOME)
ANF_GET(anAllocState, int, an, allocState, AN_ALLOC_STATE, 0)
ANF_SET(anSetAllocState, int, an, allocState, AN_ALLOC_STATE)
ANF_GET(anMayPrintState, int, an, mayPrintState, AN_MAY_PRINT_STATE, 0)
ANF_SET(anSetMayPrintState, int, an, mayPrintState, AN_MAY_PRINT_STATE)
ANF_GET(anFreshCount, int, an, freshCount, AN_FRESH_COUNT, 0)
ANF_SET(anSetFreshCount, int, an, freshCount, AN_FRESH_COUNT)
ANF_GET(anParamSymCount, int, an, nParamSyms, AN_N_PARAM_SYMS, 0)
ANF_SET(anSetParamSymCount, int, an, nParamSyms, AN_N_PARAM_SYMS)
Vec anArenaSites(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return (r && resultsBit(r, AN_ARENA_SITES)) ? r->an.arenaSites : (Vec){0};
}
void anSetArenaSites(FuncDef *f, Vec v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.arenaSites = v; resultsSetBit(r, AN_ARENA_SITES);
}
void **anParamSyms(FuncDef *f) {
    if (!f) return NULL;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return NULL;
    resultsSetBit(r, AN_PARAM_SYMS);
    return r->an.paramSyms;
}

/* The one place that carries per-function facts across `*in = *tmpl`. */
void planInheritFuncFacts(FuncDef *in, const FuncDef *tmpl) {
    if (!in || !tmpl) return;
    NodeResults *t = resultsOf(tmpl, false);
    if (!t) return;
    NodeResults *r = resultsAs(in, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    /* The `an` block is copied wholesale (it is one struct), so its bits come along too --
     * which is exactly what "a slot keyed by node pointer does not travel with the shallow
     * copy, so inherit it here" means in practice. */
    uint64_t lo = t->loMask, hi = t->hiMask;
    r->an = t->an;
    r->loMask |= lo;
    r->hiMask |= hi;
}

/* The three that are read outside the checker (the ABI and the parallel runtime) live in
 * the plan half of the slot, so they need their own accessors -- the `an.` macros cannot
 * reach them. */
#define PLF_GET(fn, type, field, bit, dflt)                                \
    type fn(const FuncDef *f) {                                            \
        NodeResults *r = resultsOf(f, false);                              \
        return (r && resultsBit(r, bit)) ? r->field : (dflt);            \
    }
#define PLF_SET(fn, type, field, bit)                                      \
    void fn(FuncDef *f, type v) {                                          \
        if (!f) return;                                                    \
        NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);         \
        if (!r) return;                                                    \
        r->field = v; resultsSetBit(r, bit);                                 \
    }

/* `deref` / `boxedCoro` are stamped on an **expression** (the three above are stamped on a
 * function), so they take an `Expr *` rather than going through the `FuncDef` macros. */
bool planDeref(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DEREF) ? r->deref : false;
}
void planSetDeref(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->deref = v;
    resultsSetBit(r, PLAN_DEREF);
}
bool planBoxedCoro(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_BOXED_CORO) ? r->boxedCoro : false;
}
void planSetBoxedCoro(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->boxedCoro = v;
    resultsSetBit(r, PLAN_BOXED_CORO);
}
PLF_GET(planIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET, false)
PLF_SET(planSetIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET)
PLF_GET(planIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER, false)
PLF_SET(planSetIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER)
PLF_GET(planParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA, false)
PLF_SET(planSetParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA)

/* ---- operator / comparison facts (T4, sixth family) ---------------------------------
 *
 * `needOp` and `convCheck` are read by code generation (not to make a decision, but to know
 * that a deferred operator still has to be re-resolved, and that a narrowing conversion gets
 * a runtime check); the rest are the checker's own state. */
#define AN_I_GET(fn, field, bit, dflt)                                     \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AN_I_SET(fn, field, bit)                                           \
    void fn(void *node, int v) {                                           \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }
#define AN_B_GET(fn, field, bit, dflt)                                     \
    bool fn(const void *node) {                                            \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
    }
#define AN_B_SET(fn, field, bit)                                           \
    void fn(void *node, bool v) {                                          \
        if (!node) return;                                                 \
        NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);      \
        if (!r) return;                                                    \
        r->an.field = v; resultsSetBit(r, bit);                              \
    }
/* `convCheck` / `needOp` take an `Expr *` (they are only ever stamped on an expression),
 * so they are written out rather than generated by the macros above. */
bool planConvCheck(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_CONV_CHECK) ? r->an.convCheck : false;
}
void planSetConvCheck(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.convCheck = v; resultsSetBit(r, AN_CONV_CHECK);
}
bool planNeedOp(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_NEED_OP) ? r->an.needOp : false;
}
void planSetNeedOp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.needOp = v; resultsSetBit(r, AN_NEED_OP);
}

AN_I_GET(anMinAt, minAt, AN_MIN_AT, 0)
AN_I_SET(anSetMinAt, minAt, AN_MIN_AT)
AN_B_GET(anReuse, reuse, AN_REUSE, false)
AN_B_SET(anSetReuse, reuse, AN_REUSE)
AN_B_GET(anBorrowed, borrowed, AN_BORROWED, false)
AN_B_SET(anSetBorrowed, borrowed, AN_BORROWED)
AN_B_GET(anQualified, qualified, AN_QUALIFIED, false)
AN_B_SET(anSetQualified, qualified, AN_QUALIFIED)

/* ---- call / receiver facts (T4, fifth family) ---------------------------------------
 *
 * All six are read by code generation (that is why they are here and not in `an`): where a
 * task is handed (`extDom`), which field an `EX_FIELD` resolved to, the instance type an
 * `EX_ASSOC` resolved to, and how a `dyn` call reaches its receiver. */

#define PLAN_GET(fn, type, field, bit, dflt)                               \
    type fn(const Expr *e) {                                               \
        NodeResults *r = resultsOf(e, false);                              \
        return (r && resultsBit(r, bit)) ? r->field : (dflt);            \
    }
#define PLAN_SET(fn, type, field, bit)                                     \
    void fn(Expr *e, type v) {                                             \
        if (!e) return;                                                    \
        NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);         \
        if (!r) return;                                                    \
        r->field = v; resultsSetBit(r, bit);                                 \
    }

PLAN_GET(planExtDom, Expr *, extDom, PLAN_EXT_DOM, NULL)
PLAN_SET(planSetExtDom, Expr *, extDom, PLAN_EXT_DOM)
PLAN_GET(planField, FieldDef *, field, PLAN_FIELD, NULL)
PLAN_SET(planSetField, FieldDef *, field, PLAN_FIELD)
PLAN_GET(planAssocOwner, Type *, assocOwner, PLAN_ASSOC_OWNER, NULL)
PLAN_SET(planSetAssocOwner, Type *, assocOwner, PLAN_ASSOC_OWNER)
PLAN_GET(planDynRecvViaRef, bool, dynRecvViaRef, PLAN_DYN_RECV_VIA_REF, false)
PLAN_SET(planSetDynRecvViaRef, bool, dynRecvViaRef, PLAN_DYN_RECV_VIA_REF)
PLAN_GET(planCallViaFn, bool, callViaFn, PLAN_CALL_VIA_FN, false)
PLAN_SET(planSetCallViaFn, bool, callViaFn, PLAN_CALL_VIA_FN)

/* ---- depth and origin facts (T4, fourth family) -------------------------------------
 *
 * `void *` on purpose: `refDepth` lives on an `Expr` **and** on the checker's `Sym`, and one
 * accessor answers both. */
#define AN_D_GET(fn, field, bit, dflt)                                     \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, bit)) ? r->an.field : (dflt);         \
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
        r->an.field = v; resultsSetBit(r, bit);                              \
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
    return (r && resultsBit(r, PLAN_PAR_WORKER)) ? r->parWorker : NULL;
}
void planSetParWorker(Expr *e, FuncDef *wf) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->parWorker = wf; resultsSetBit(r, PLAN_PAR_WORKER);
}

bool planDomNew(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DOM_NEW) ? r->domNew : false;
}
void planSetDomNew(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->domNew = v; resultsSetBit(r, PLAN_DOM_NEW);
}

bool planViewOf(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_VIEW_OF) ? r->viewOf : false;
}
void planSetViewOf(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->viewOf = v; resultsSetBit(r, PLAN_VIEW_OF);
}

/* ---- impl / trait / module facts (T4, second family) -------------------------------- */

TraitDef *planImplTrait(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TRAIT)) ? r->implTrait : NULL;
}
void planSetImplTrait(ImplDef *im, TraitDef *tr) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTrait = tr; resultsSetBit(r, PLAN_IMPL_TRAIT);
}

Type *planImplTarget(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TARGET)) ? r->implTarget : NULL;
}
void planSetImplTarget(ImplDef *im, Type *t) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTarget = t; resultsSetBit(r, PLAN_IMPL_TARGET);
}

bool planUsedDyn(const TraitDef *tr) {
    NodeResults *r = resultsOf(tr, false);
    return r && resultsBit(r, PLAN_USED_DYN) ? r->usedDyn : false;
}
void planSetUsedDyn(TraitDef *tr, bool v) {
    if (!tr) return;
    NodeResults *r = resultsAs(tr, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usedDyn = v; resultsSetBit(r, PLAN_USED_DYN);
}

bool planUsesDyn(const Module *m) {
    NodeResults *r = resultsOf(m, false);
    return r && resultsBit(r, PLAN_USES_DYN) ? r->usesDyn : false;
}
void planSetUsesDyn(Module *m, bool v) {
    if (!m) return;
    NodeResults *r = resultsAs(m, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usesDyn = v; resultsSetBit(r, PLAN_USES_DYN);
}

bool planUsed(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_USED) ? r->used : false;
}

void planSetUsed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->used = v;
        resultsSetBit(r, PLAN_USED);
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
    return (s && resultsBit(s, PLAN_ARENA_LEVEL)) ? s->arenaLevel : 0;
}
int planZoneLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ZONE_LEVEL)) ? s->zoneLevel : 0;
}
int planArenaArg(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ARENA_ARG)) ? s->arenaArg : 0;
}
bool planNeedTemp(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_NEED_TEMP)) ? s->needTemp : false;
}

bool planUsesHome(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_USES_HOME) ? s->usesHome : false;
}
bool planMayUseArena(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAY_USE_ARENA) ? s->mayUseArena : false;
}
bool planMakesPool(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAKES_POOL) ? s->makesPool : false;
}
bool planCondAllocs(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && resultsBit(s, PLAN_COND_ALLOCS) ? s->condAllocs : false;
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
    if (r && resultsBit(r, PLAN_FOR_STEP_DROPPED) && r->forStepDropped) return NULL;
    return s->forStep;
}

void planSetForStepDropped(Stmt *s) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->forStepDropped = true;
    resultsSetBit(r, PLAN_FOR_STEP_DROPPED);
}

bool planIsCoro(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_IS_CORO) ? s->isCoro : false;
}
Type *planYieldType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_YIELD_TYPE) ? s->yieldType : NULL;
}
Type *planCoroFrameType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_FRAME_TYPE) ? s->coroFrameType : NULL;
}
bool planCoroNeedsZone(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_NEEDS_ZONE) ? s->coroNeedsZone : false;
}
int planCoroProto(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_PROTO) ? s->coroProto : 0;
}
bool planCoroBoxed(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_BOXED) ? s->coroBoxed : false;
}
