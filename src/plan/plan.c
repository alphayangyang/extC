/* The plan side table: accessors, setters, and the per-call-site alternative answers.
 *
 * Storage lives in `results.c`: one result slot per node, addressed by a **dense node
 * id**, so a result is never keyed by a node pointer and survives input immutability.
 * This file keeps what is specific to the plan: the meaning of each field, the setters
 * that record them, and the tables that answer "what does this call site resolve to".
 *
 * A setter with a NULL node is a no-op, and a lookup for a NULL node misses: some plan
 * questions are asked about nodes that do not exist.
 *
 * Two plan fields live on an AST node: the resolved callee (`Expr.func`) and the `for`
 * step (`Stmt.forStep`); their accessors below read the node. Do not add storage for them
 * without also making the instance set explicit (`forStep` is half syntax -- the parser
 * owns it). */
#include "plan.h"
#include "results.h"
#include <stdlib.h>
#include <string.h>

/* One bit per stored field. Macros, not an enum: a pedantic C11 compiler rejects
 * enumerator values above 31, and the analysis side has already grown past 32. */
#define PLAN_ARENA_LEVEL         0
#define PLAN_ZONE_LEVEL          1
#define PLAN_ARENA_ARG           2
#define PLAN_NEED_TEMP           3
#define PLAN_USES_HOME           4
#define PLAN_MAY_USE_ARENA       5
#define PLAN_MAKES_POOL          6
#define PLAN_COND_ALLOCS         7
#define PLAN_IS_CORO             8
#define PLAN_YIELD_TYPE          9
#define PLAN_CORO_FRAME_TYPE     10
#define PLAN_CORO_NEEDS_ZONE     11
#define PLAN_CORO_PROTO          12
#define PLAN_INST_NAME           13
#define PLAN_TEMPLATE            14
#define PLAN_CORO_BOXED          15
#define PLAN_FOR_STEP_DROPPED    16
#define PLAN_CALLEE              17
#define PLAN_USED                18
#define PLAN_CNAME               19
#define PLAN_BUILTIN_HOLDER      20
#define PLAN_CORO_OF             21
#define AN_LAM_SIG               22
#define AN_MAKES_POOL_ANY        23
#define PLAN_IMPL_TRAIT          24
#define PLAN_IMPL_TARGET         25
#define PLAN_USED_DYN            26
#define PLAN_USES_DYN            27
#define PLAN_PAR_WORKER          28
#define PLAN_DOM_NEW             29
#define PLAN_VIEW_OF             30
#define AN_REF_DEPTH             31
#define AN_HOME_DEPTH            32
#define AN_LEXICAL_LEVEL         33
#define AN_STORED_AT             34
#define PLAN_EXT_DOM             35
#define PLAN_FIELD               36
#define PLAN_ASSOC_OWNER         37
#define PLAN_DYN_RECV_VIA_REF    39
#define PLAN_CALL_VIA_FN         40
#define AN_MIN_AT                41
#define AN_REUSE                 42
#define AN_BORROWED              43
#define AN_QUALIFIED             44
#define AN_CONV_CHECK            45
#define AN_NEED_OP               46
#define AN_IS_ASSOC              47
#define AN_LAM_CHECKED           48
#define AN_LAM_INFER_RET         49
#define AN_NEEDS_HOME            50
#define AN_ALLOC_STATE           51
#define AN_MAY_PRINT_STATE       52
#define AN_FRESH_COUNT           53
#define AN_ARENA_SITES           54
#define AN_N_PARAM_SYMS          55
#define AN_PARAM_SYMS            56
#define AN_EFF_STATE             66
#define AN_EFF_COMPLETE          67
#define AN_EFF_UNKNOWN           68
#define PLAN_IS_EXT_TARGET       57
#define PLAN_IS_PAR_WORKER       58
#define AN_ADDR_MASK             59
#define AN_CONT_MASK             60
#define AN_OTHER_MASK            61
#define AN_HOME_ADDR_MASK        62
#define AN_HOME_CONT_MASK        63
#define PLAN_PAR_TLS_ARENA       62
#define PLAN_PAR_FULL_OUT        84
#define PLAN_LAM_FNPTR           85   /* `EX_LAMBDA`: this literal is used as a bare `fn(...)` pointer, not as an environment value */
#define PLAN_LAM_SCOPE           86   /* a lambda environment struct: the body it is written in */
#define PLAN_CORO_OWNED          96   /* a `var` holding a handle coroutine nothing else can reach */
#define PLAN_ANY_BOX             87   /* an expression whose value is boxed into an `any` */
#define PLAN_ANY_TO              88   /* an `any` value unboxed into a concrete type: which one */
#define PLAN_DYN_CALL            89   /* a method call on an `any`: dispatch by name at run time */
#define PLAN_COMPOUND_CALL       90   /* `x += y` where the type defines `fn +=`: a call, not an assign */
#define PLAN_NEW_AS_BUFFER       91   /* `new T[n]` whose context is a `@buffer` type: its constructor */
#define PLAN_VAR_CLONE           92   /* `var x = <place>` where the type is `@value`: the copy is `clone` */
#define PLAN_ARG_CLONE           97   /* a by-value argument / field initializer of an `@value` type: the copy is `clone` */
#define PLAN_RET_CLONE           98   /* `return <place>` of an `@value` type: the copy is `clone` */
#define PLAN_LEN_PARAM           93   /* the identifier names a `const` parameter: value comes from the instance */
#define PLAN_RET_BORROW          94   /* a `return` named the parameter its view borrows from */
#define PLAN_DEREF               63
#define PLAN_BOXED_CORO          64
#define PLAN_CORO_KIND           65
#define PLAN_DYN_TABLE           72
#define PLAN_CORO_RET_PROTO      73
#define PLAN_OW_SITES            75
#define PLAN_OW_LOCAL            76
#define PLAN_VAR_INIT_OO         77
#define PLAN_RC_NODEC            74   /* "this frame never decrefs": the basis for skipping initRef's +1 */
#define PLAN_FN_ACYCLIC          78   /* acyclicity test: the heap-reference graph this function builds has no back edge ((i)(ii)(iii)) */
#define PLAN_FN_REFS_COUNTED     79   /* acyclicity test: references in this function live only in counted rc slots ((iv)) */
#define AN_STORE_REF_KIND        80   /* acyclicity test: classification of the reference-slot store at an assignment site (SR_*) */
#define AN_ACYCLIC_WHY           81   /* acyclicity test: reason string for a rejection, printed under EXTC_DBG_ACYCLIC */
#define PLAN_FN_RECLAIM_IDLE     82   /* (2) criteria (a)(b)(c): this function never drops a live reference */
#define PLAN_PAD_KIND            83   /* f-string format specifier: which pad primitive the slot value's type selected (PlanPadKind) */
#define AN_DUMMY_PAST_64         65
#define AN_ADDR_FROM_LOCAL       64
/* The analysis half of the slot, by name: the checker's own working state, which no later
 * phase reads. Named here so the two audiences cannot quietly merge -- a field that code
 * generation starts reading must move out of this list (and get a `planXxx` accessor). */
const char *const planAnalysisFields[] = { "lamSig", "makesPoolAny",
                                           "refDepth", "homeDepth", "lexicalLevel", "storedAt",
                                           "minAt", "reuse", "borrowed", "qualified",
                                           "fnAcyclic", "fnRefsCounted", "fnReclaimIdle",
                                           "storeRefKind",
                                           "acyclicWhy" };
const size_t planAnalysisFieldCount = sizeof planAnalysisFields / sizeof planAnalysisFields[0];

/* ---- setters (called by the checker) ----------------------------------------------- */

/**
 * @brief Record the arena level an allocation site should use.
 *
 * @param e  The allocation-site expression (`new`, or a call that may allocate); NULL ignores this
 *           write.
 * @param v  The arena level: `ARENA_HOME` means the home arena, k >= 1 the arena of the block at
 *           level k.
 */
void planSetArenaLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaLevel = v; resultsSetBit(s, PLAN_ARENA_LEVEL);
}
/**
 * @brief Record the zone level a pool site should use.
 *
 * Only ever lowered: one node may be checked more than once, and a smaller level means a longer
 * lifetime, so the smaller one wins.
 *
 * @param e  The pool-site expression; NULL ignores this write.
 * @param v  The zone level: `ZONE_HOME` means "use the home zone the caller received", k >= 1 the
 *           zone of the block at level k.
 */
void planSetZoneLevel(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->zoneLevel = v; resultsSetBit(s, PLAN_ZONE_LEVEL);
}
/**
 * @brief Record the arena argument a call passes down.
 *
 * @param e  The call-site expression; NULL ignores this write.
 * @param v  The arena argument: `ARENA_HOME` means the caller's own home arena, k >= 1 the arena of
 *           the block at level k.
 */
void planSetArenaArg(Expr *e, int v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->arenaArg = v; resultsSetBit(s, PLAN_ARENA_ARG);
}
/**
 * @brief Record that an expression needs a temporary.
 *
 * @param e  The expression; NULL ignores this write.
 * @param v  true when a temporary is needed.
 */
void planSetNeedTemp(Expr *e, bool v) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    s->needTemp = v; resultsSetBit(s, PLAN_NEED_TEMP);
}

/**
 * @brief Record whether a function receives the caller's home arena as a hidden parameter.
 *
 * @param f  The function; NULL ignores this write.
 * @param v  true when the hidden home arena parameter is needed.
 */
void planSetUsesHome(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->usesHome = v; resultsSetBit(s, PLAN_USES_HOME);
}
/**
 * @brief Record whether a function body allocates storage itself.
 *
 * @param f  The function; NULL ignores this write.
 * @param v  true when it may allocate.
 */
void planSetMayUseArena(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->mayUseArena = v; resultsSetBit(s, PLAN_MAY_USE_ARENA);
}
/**
 * @brief Record whether a function creates a pool, either directly or through a callee.
 *
 * @param f  The function; NULL ignores this write.
 * @param v  true when it creates a pool.
 */
void planSetMakesPool(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->makesPool = v; resultsSetBit(s, PLAN_MAKES_POOL);
}
/**
 * @brief Record which parameter a returned view borrows its storage from.
 *
 * A call site that sees a record treats the result as storage that already exists: it has to reach
 * the level of that storage instead of the level of the call's own block. `RET_BORROW_CONFLICT` in
 * `arg` means two `return` statements named different sources, and the call site keeps the
 * conservative rule that reads every argument.
 *
 * @param f      The function; NULL ignores this write.
 * @param arg    Parameter index the storage comes from, or `RET_BORROW_CONFLICT`.
 * @param field  Field of that parameter, -1 for the parameter's own storage.
 */
void planSetRetBorrow(FuncDef *f, int arg, int field) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->an.retBorrowArg = arg;
    s->an.retBorrowField = field;
    resultsSetBit(s, PLAN_RET_BORROW);
}

/**
 * @brief Parameter a returned view borrows its storage from.
 *
 * @param f  The function to query.
 * @return   The parameter index, `RET_BORROW_CONFLICT`, or -1 when no source is recorded.
 */
int planRetBorrowArg(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_RET_BORROW) ? s->an.retBorrowArg : -1;
}

/**
 * @brief Field of the parameter a returned view borrows its storage from.
 *
 * @param f  The function to query.
 * @return   The field index, or -1 for the parameter's own storage and when no source is recorded.
 */
int planRetBorrowField(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_RET_BORROW) ? s->an.retBorrowField : -1;
}

/**
 * @brief Record whether a loop's condition allocates storage.
 *
 * @param st  The loop statement; NULL ignores this write.
 * @param v   true when the condition allocates.
 */
void planSetCondAllocs(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->condAllocs = v; resultsSetBit(s, PLAN_COND_ALLOCS);
}

/**
 * @brief Record whether a function is a coroutine.
 *
 * @param f  The function; NULL ignores this write.
 * @param v  true when it is a coroutine.
 */
void planSetIsCoro(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->isCoro = v; resultsSetBit(s, PLAN_IS_CORO);
}
/**
 * @brief Record the type of the value a coroutine `yield`s.
 *
 * @param f  The coroutine function; NULL ignores this write.
 * @param v  The yielded value's type.
 */
void planSetYieldType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->yieldType = v; resultsSetBit(s, PLAN_YIELD_TYPE);
}
/**
 * @brief Record the C struct type of a coroutine frame.
 *
 * @param f  The coroutine function; NULL ignores this write.
 * @param v  The frame struct type.
 */
void planSetCoroFrameType(FuncDef *f, Type *v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroFrameType = v; resultsSetBit(s, PLAN_CORO_FRAME_TYPE);
}
/**
 * @brief Record whether a coroutine needs a place (zone) of its own.
 *
 * @param f  The coroutine function; NULL ignores this write.
 * @param v  true when it needs its own zone.
 */
void planSetCoroNeedsZone(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroNeedsZone = v; resultsSetBit(s, PLAN_CORO_NEEDS_ZONE);
}
/**
 * @brief Record the C name used when emitting an instance.
 *
 * The name is built from the template name followed by each type argument's rewritten string, is
 * written once at instantiation, and is used both for the emitted definition and for the call sites.
 * Only the pointer is stored, so the string must stay valid until it is read later.
 *
 * @param f     The instance function definition; NULL ignores this write.
 * @param name  The C name.
 */
void planSetInstName(FuncDef *f, const char *name) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->instName = name; resultsSetBit(s, PLAN_INST_NAME);
}

/**
 * @brief Record which template function an instance was instantiated from.
 *
 * The template pointer is kept in a plan side table rather than in a node field: the same pointer
 * decides the instance's name, its type arguments, and which body it shares. Instantiation writes it
 * after the shallow copy of the instance, because the shallow copy cannot carry this pointer.
 *
 * @param f     The instance function definition; NULL ignores this write.
 * @param tmpl  The template function definition used for the instantiation.
 */
void planSetTemplate(FuncDef *f, FuncDef *tmpl) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->tmpl = tmpl; resultsSetBit(s, PLAN_TEMPLATE);
}

/**
 * @brief Record the protocol number of a synthesised protocol method.
 *
 * @param f  The synthesised protocol method; NULL ignores this write.
 * @param v  The protocol number: next/value/send on the frame are 1/2/5, and on the handle 3/4/6.
 */
void planSetCoroProto(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroProto = v; resultsSetBit(s, PLAN_CORO_PROTO);
}

/**
 * @brief Record whether a coroutine was ever boxed into a handle.
 *
 * This bit is written where a frame value is forced into a `coroutine<T>` handle; once set, the
 * coroutine always gets a task, even without an explicit spawn, and therefore counts as needing a
 * zone.
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param v  true when it was ever boxed into a handle.
 */
void planSetCoroBoxed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    s->coroBoxed = v; resultsSetBit(s, PLAN_CORO_BOXED);
}

/* ---- accessors (signatures are the public surface) --------------------------------- */

/**
 * @brief Get the template an instance was generated from.
 *
 * @param f  The target function.
 * @return   The template function; NULL when `f` is NULL or is not an instance.
 */
FuncDef *planTemplate(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_TEMPLATE) ? s->tmpl : NULL;
}
/**
 * @brief Get an instance's C name.
 *
 * @param f  The target function.
 * @return   The C name; NULL when there is no record.
 */
const char *planInstName(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_INST_NAME) ? s->instName : NULL;
}

/* ---- the resolved callee, per enclosing instance -----------------------------------
 *
 * A call site inside a generic body is **one node shared by every instance** of that
 * body: `wrap<T>` with two type arguments has one `pick(x)` node, and the instance that
 * callee resolves to depends on which `wrap` is being emitted. Keeping a single pointer
 * on the node means the last resolution wins: with `wrap<i32>` and
 * `wrap<meter>`, both emitted functions call the same `pick_*`, the other instance is
 * never emitted at all, and the generated C does not compile.
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

/**
 * @brief Tell whether a candidate recorded per enclosing body belongs to the function body or type
 * instance currently being emitted.
 *
 * @param a  One candidate record.
 * @return   true when its enclosing body matches the current emission context; false when there is
 *           no emission context.
 */
static bool altMatches(const AltCallee *a) {
    if (g_emitFunc && a->enclosing == g_emitFunc) return true;
    if (g_emitInst && a->inst == g_emitInst) return true;
    return false;
}

/**
 * @brief Get the callee a call site currently resolves to.
 *
 * A call site inside a generic body is shared by every instance of that body. Code generation
 * brackets the function body or type instance it is emitting with `planEnterFunc` /
 * `planEnterInst`, and this function prefers the resolution recorded for that body. A call site that
 * belongs to no instance returns the one recorded on the node.
 *
 * @param e  The call-site (or function-value) expression.
 * @return   The resolved function definition; NULL for a builtin, an operator, an unresolved site,
 *           or when there is no record.
 */
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

/**
 * @brief Record the callee a call site resolves to, as the default answer.
 *
 * A call site inside a generic body is shared by all instances of that body, and each instance's
 * answer is recorded per body by `planSetAltCallee`; this function writes the one read when the site
 * belongs to no instance, and `planCallee` returns it when there is no emission context.
 *
 * @param e       The call-site (or function-value) expression; NULL ignores this write.
 * @param callee  The resolved function definition; may be NULL when there is no resolution.
 */
void planSetCallee(Expr *e, FuncDef *callee) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (r) {
        r->func = callee;
        resultsSetBit(r, PLAN_CALLEE);
    }
}

/**
 * @brief Get the C name a binding or variable statement is emitted under.
 *
 * Parameters are not here: they are stored by value (a coroutine frame copies them) and the slots,
 * indexed by node pointer, cannot follow a copy, so a parameter carries its own field. This function
 * does not accept a parameter node.
 *
 * @param node  The binding expression or variable declaration statement node.
 * @return      The recorded C name; NULL when there is no record or the record is an empty string, in
 *              which case the caller falls back to the declaration's own name.
 */
const char *planCName(const void *node) {
    NodeResults *r = resultsOf(node, false);
    if (!r || !resultsBit(r, PLAN_CNAME)) return NULL;
    /* An empty name means "no name of its own": callers fall back to the declaration's name
     * (`planCName(x) ? planCName(x) : x->name`), and the checker does write "" for a binding
     * it cannot name. Returning "" would win that fallback and emit C with no identifier. */
    return r->cname && r->cname[0] ? r->cname : NULL;
}

/**
 * @brief Record the C name a binding or variable statement is emitted under.
 *
 * @param node  The binding expression or variable declaration statement node; NULL ignores this
 *              write.
 * @param name  The C name.
 * @param kind  The node's kind (`RKIND_EXPR` or `RKIND_STMT`), used for checking the slot type.
 */
void planSetCName(void *node, const char *name, ResultKind kind) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, kind, __LINE__);
    if (!r) return;
    r->cname = name;
    resultsSetBit(r, PLAN_CNAME);
}

/* ---- struct-definition facts ---- */

/**
 * @brief Tell whether this struct definition is the synthesised placeholder for a builtin type.
 *
 * Such a placeholder has no fields and no layout, and no struct is declared for it when emitting C;
 * its methods are still emitted as ordinary functions.
 *
 * @param sd  The struct definition.
 * @return    true when it is a builtin placeholder; false otherwise.
 */
bool planBuiltinHolder(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, PLAN_BUILTIN_HOLDER) ? r->builtinHolder : false;
}
/**
 * @brief Record whether this struct definition is the synthesised placeholder for a builtin type.
 *
 * @param sd  The struct definition; NULL ignores this write.
 * @param v   true when it is a builtin placeholder.
 */
void planSetBuiltinHolder(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->builtinHolder = v; resultsSetBit(r, PLAN_BUILTIN_HOLDER);
}

/**
 * @brief Get the coroutine function a coroutine frame struct belongs to.
 *
 * The frame struct is synthesised by the checker for each coroutine; when emitting C this recorded
 * function name ties the frame type together with its `$step` / `$next` functions.
 *
 * @param sd  The struct definition (a synthesised frame struct).
 * @return    The coroutine function it belongs to; NULL when the struct is not a coroutine frame.
 */
FuncDef *planCoroOf(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, PLAN_CORO_OF)) ? r->coroOf : NULL;
}
/**
 * @brief Record the coroutine function a coroutine frame struct belongs to.
 *
 * @param sd  The synthesised frame struct; NULL ignores this write.
 * @param f   The coroutine function it belongs to.
 */
void planSetCoroOf(StructDef *sd, FuncDef *f) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->coroOf = f; resultsSetBit(r, PLAN_CORO_OF);
}

/**
 * @brief Get the function whose body a lambda environment struct is laid out in.
 *
 * A lambda written inside a generic body sees the body's type parameters, so an environment with a
 * parameter-dependent field has to exist once per *instance* of that body. This records which body
 * that is: code generation re-emits the environment (and its `call` method) inside each instance's
 * substitution, and the deferred-recheck passes use it to find those instances, because a check
 * recorded inside a lambda body names the lambda's `call` method rather than the enclosing function.
 *
 * @param sd  The struct definition (a synthesized lambda environment struct).
 * @return    The function the literal is written in; NULL when the struct is not a lambda
 *            environment.
 */
FuncDef *planLamScope(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, PLAN_LAM_SCOPE)) ? r->lamScope : NULL;
}

/**
 * @brief Record the function whose body a lambda environment struct is laid out in.
 *
 * @param sd  The synthesized environment struct; NULL ignores this write.
 * @param f   The function the literal is written in.
 */
void planSetLamScope(StructDef *sd, FuncDef *f) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->lamScope = f; resultsSetBit(r, PLAN_LAM_SCOPE);
}

/**
 * @brief Get the user-written signature recorded on a lambda environment struct.
 *
 * The environment struct is compiler-synthesised; diagnostics show this string, so the user sees the
 * `fn(i64) -> i64` they wrote rather than the generated type name.
 *
 * @param sd  The struct definition (a lambda environment struct).
 * @return    The recorded signature string; NULL when it is not a lambda environment struct or there
 *            is no record.
 */
const char *anLamSig(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return (r && resultsBit(r, AN_LAM_SIG)) ? r->an.lamSig : NULL;
}
/**
 * @brief Record the user-written signature on a lambda environment struct.
 *
 * @param sd   The lambda environment struct; NULL ignores this write.
 * @param sig  The signature string the user wrote.
 */
void anSetLamSig(StructDef *sd, const char *sig) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.lamSig = sig; resultsSetBit(r, AN_LAM_SIG);
}

/**
 * @brief Tell whether any method of this struct creates a pool.
 *
 * This is the struct-level summary: when one method creates a pool, its associated functions (the
 * constructor family) may create pools too, and the pool-level and hidden-zone decisions have to
 * account for that layer.
 *
 * @param sd  The struct definition.
 * @return    true when a method creates a pool; false when there is no record.
 */
bool anMakesPoolAny(const StructDef *sd) {
    NodeResults *r = resultsOf(sd, false);
    return r && resultsBit(r, AN_MAKES_POOL_ANY) ? r->an.makesPoolAny : false;
}
/**
 * @brief Record whether any method of this struct creates a pool.
 *
 * @param sd  The struct definition; NULL ignores this write.
 * @param v   true when a method creates a pool.
 */
void anSetMakesPoolAny(StructDef *sd, bool v) {
    if (!sd) return;
    NodeResults *r = resultsAs(sd, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->an.makesPoolAny = v; resultsSetBit(r, AN_MAKES_POOL_ANY);
}

/* ---- the effect closure's state -----------------------------------------------------
 *
 * `effState` is work state; `effComplete`/`effUnknown` are results. None of the three is
 * read by code generation, so all three sit on the analysis side. */
/**
 * @brief Tell whether a function's effect summary is trustworthy.
 *
 * When the closure merges callees' summaries and any callee cannot be finished (it is on a cycle, or
 * it calls something that never resolved), this is set to false; a call site then takes the most
 * conservative tier of checks.
 *
 * @param f  The function definition.
 * @return   true when the summary is trustworthy; false when there is no record.
 */
bool anEffComplete(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_COMPLETE) ? r->an.effComplete : false;
}
/**
 * @brief Record whether a function's effect summary is trustworthy.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the summary is trustworthy.
 */
void anSetEffComplete(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effComplete = v; resultsSetBit(r, AN_EFF_COMPLETE);
}
/**
 * @brief Tell whether a function's effect summary holds an unresolved call.
 *
 * When it does, the summary can never be completed and a call site takes the most conservative tier.
 *
 * @param f  The function definition.
 * @return   true when there is an unresolved call; false when there is no record.
 */
bool anEffUnknown(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_UNKNOWN) ? r->an.effUnknown : false;
}
/**
 * @brief Record whether a function's effect summary holds an unresolved call.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  true when there is an unresolved call.
 */
void anSetEffUnknown(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effUnknown = v; resultsSetBit(r, AN_EFF_UNKNOWN);
}
/**
 * @brief Get the computation state of the effect closure.
 *
 * The value is `EFF_NONE` (not computed yet), `EFF_DONE` (computed; trustworthiness is
 * `anEffComplete`) or `EFF_IN_PROGRESS` (being computed -- meeting it again on recursion means a
 * cycle, and the summary is treated as incomplete). Only the effect closure itself reads and writes
 * this value.
 *
 * @param f  The function definition.
 * @return   The state value; `EFF_NONE` when there is no record.
 */
int anEffState(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, AN_EFF_STATE) ? r->an.effState : 0;
}
/**
 * @brief Record the computation state of the effect closure.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The state value: `EFF_NONE`, `EFF_DONE` or `EFF_IN_PROGRESS`.
 */
void anSetEffState(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.effState = v; resultsSetBit(r, AN_EFF_STATE);
}

/* ---- effect summary ----------------------------------------------------------------
 *
 * Pure results (the checker writes them, only the checker reads them): the transitively
 * closed masks and `addrFromLocal`. The **state** that drives the closure
 * (`effState`/`effComplete`/`effUnknown`) and the call graph it walks (`callees`) are
 * pass state, not a per-node result. */
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

/* ---- per-function analysis facts ---------------------------------------------------
 *
 * **Instances**: `funcInstance` builds an instance with `*in = *tmpl` -- a shallow copy.
 * Plain fields travel with that copy; a slot keyed by node pointer does not. So every fact
 * here is inherited explicitly in one place (`planInheritFuncFacts`, right after the copy). */
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
/**
 * @brief Get the list of allocation sites recorded for a function.
 *
 * The list holds every expression in the body that may allocate: `new`, plus calls that may allocate
 * storage. The allocation-placement stage decides the arena level per site from it; this is the
 * checker's own working state, and later stages do not read it. A generic instance does not collect
 * its own list and points at its template's directly.
 *
 * @param f  The function definition.
 * @return   The allocation-site vector; an empty vector when `f` is NULL or there is no record.
 */
Vec anArenaSites(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return (r && resultsBit(r, AN_ARENA_SITES)) ? r->an.arenaSites : (Vec){0};
}
/**
 * @brief Record a function's list of pending arena sites.
 *
 * The list holds both allocation-point expressions and call sites whose arena argument is not
 * decided yet; it is handed to a pass after the analysis finishes so that the level can be decided
 * per site. An instantiated function copies the same list over from its template.
 *
 * @param f  The target function.
 * @param v  The site vector, with `Expr *` elements.
 */
void anSetArenaSites(FuncDef *f, Vec v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.arenaSites = v; resultsSetBit(r, AN_ARENA_SITES);
}
/**
 * @brief Get the writable array a function stores its parameter symbols in.
 *
 * The array has a fixed capacity; the first `anParamSymCount` elements are live. The call also builds
 * the result slot on the way.
 *
 * @param f  The target function.
 * @return   The array's first address; NULL when `f` is NULL or the slot cannot be built.
 */
void **anParamSyms(FuncDef *f) {
    if (!f) return NULL;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return NULL;
    resultsSetBit(r, AN_PARAM_SYMS);
    return r->an.paramSyms;
}

/**
 * @brief Carry a template function's per-function analysis facts onto a newly materialised
 * instance.
 *
 * The instance is a shallow copy of the template (`*in = *tmpl`), and result slots are indexed by
 * node pointer, so the copy does not bring them along; this function must therefore be called right
 * after the copy to merge the template's analysis block and written-bit masks into the instance's
 * slot.
 *
 * @param in    The instance created by the shallow copy.
 * @param tmpl  The template function.
 */
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

/* The write-once pass's two facts. (The call graph itself stays a mutable vector on the
 * AST: `vecPush`'s contract forbids holding a `Vec *` across a push, and an accessor
 * handing one back invites exactly that.) */
/**
 * @brief Record how many `@overwrite` reuse sites this function body holds.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The number of sites.
 */
void planSetOwSites(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.owSites = v; resultsSetBit(r, PLAN_OW_SITES);
}
/**
 * @brief Record whether the storage for `@overwrite` reuse sites sits in this function's own frame.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  true when it sits in this function's frame.
 */
void planSetOwLocal(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.owLocal = v; resultsSetBit(r, PLAN_OW_LOCAL);
}
/**
 * @brief Get how many `@overwrite` reuse sites this function body holds.
 *
 * Code generation uses this count to decide whether it has to prepare hidden storage for the reuse
 * sites and pass it along. Only the checker writes it.
 *
 * @param f  The function definition.
 * @return   The number of sites; 0 when there is no record.
 */
int planOwSites(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_OW_SITES) ? r->an.owSites : 0;
}
/**
 * @brief Tell whether the storage for `@overwrite` reuse sites sits in this function's own frame.
 *
 * True means each call has a block of storage of its own (`main`, or a function that recurses into
 * itself); false means the storage is held by the caller's frame and passed in as an opaque hidden
 * parameter.
 *
 * @param f  The function definition.
 * @return   true when the storage sits in this function's frame; false when there is no record.
 */
bool planOwLocal(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_OW_LOCAL) ? r->an.owLocal : false;
}
/**
 * @brief Tell whether this function's body is named from outside by dyn vtable thunks.
 *
 * Such a reference is invisible to the "emit only what some name mentions" reachability pruning, so
 * it is recorded here and the body must be kept.
 *
 * @param f  The function definition.
 * @return   true when a dyn vtable names it; false when there is no record.
 */
bool planDynTable(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_DYN_TABLE) ? r->dynTable : false;
}
/**
 * @brief Record whether this function's body is named from outside by dyn vtable thunks.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  true when a dyn vtable names it.
 */
void planSetDynTable(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->dynTable = v; resultsSetBit(r, PLAN_DYN_TABLE);
}
/**
 * @brief Get a coroutine declaration's return type (the one not yet substituted with type
 * arguments).
 *
 * Materialising a generic instance uses it to substitute the type arguments into the instance's real
 * return type.
 *
 * @param f  The coroutine function definition (the template).
 * @return   The declared return type; NULL when it is not a coroutine or there is no record.
 */
Type *planCoroRetProto(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_CORO_RET_PROTO) ? r->coroRetProto : NULL;
}
/**
 * @brief Record a coroutine declaration's return type (the one not yet substituted with type
 * arguments).
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param t  The declared return type; NULL when it is not a coroutine.
 */
void planSetCoroRetProto(FuncDef *f, Type *t) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->coroRetProto = t; resultsSetBit(r, PLAN_CORO_RET_PROTO);
}
/**
 * @brief Get the number the codegen prelude assigns to this coroutine.
 *
 * Both the kind field inside a handle and the scheduling domain's task table use this number to
 * dispatch to the matching `$step` / `$next`.
 *
 * @param f  The coroutine function definition.
 * @return   The number; 0 when there is no record.
 */
int planCoroKind(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_CORO_KIND) ? r->coroKind : 0;
}
/**
 * @brief Record the number the codegen prelude assigns to this coroutine.
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param v  The number.
 */
void planSetCoroKind(FuncDef *f, int v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->coroKind = v; resultsSetBit(r, PLAN_CORO_KIND);
}
/**
 * @brief Tell whether this expression needs one dereference layer when emitted.
 *
 * The value's type is `ref T` while its position needs a `T`, so the generated C writes `*(...)`.
 *
 * @param e  The expression.
 * @return   true when a dereference is needed; false when there is no record.
 */
bool planDeref(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DEREF) ? r->deref : false;
}
/**
 * @brief Record whether this expression needs one dereference layer when emitted.
 *
 * @param e  The expression; NULL ignores this write.
 * @param v  Whether a dereference is needed.
 */
void planSetDeref(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->deref = v;
    resultsSetBit(r, PLAN_DEREF);
}
/**
 * @brief Tell whether this coroutine call has to be boxed into a handle for storage.
 *
 * True means the coroutine's value is stored somewhere that outlives the current C scope: the
 * generated C puts the frame into the task's own place and makes the value a `{frame, kind, task}`
 * handle.
 *
 * @param e  The coroutine call expression.
 * @return   true when boxing is needed; false when there is no record.
 */
bool planBoxedCoro(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_BOXED_CORO) ? r->boxedCoro : false;
}
/**
 * @brief Record whether this coroutine call has to be boxed into a handle for storage.
 *
 * @param e  The coroutine call expression; NULL ignores this write.
 * @param v  Whether boxing is needed.
 */
void planSetBoxedCoro(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->boxedCoro = v;
    resultsSetBit(r, PLAN_BOXED_CORO);
}
/**
 * @brief Record that a `var` holds a handle coroutine nothing outside its block can reach.
 *
 * The checker sets this only when every later use in the same block is a drive call
 * (`next`, `value`, `send`) on the handle itself: no copy, no argument, no store, no return.
 * Codegen then ends the task when the block is left, so an abandoned handle releases its
 * zone (and the pools born in it) instead of leaking them.
 *
 * @param node  The `var` statement; NULL ignores this write.
 * @param v     Whether the task can be ended at block exit.
 */
void planSetCoroOwned(void *node, bool v) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->coroOwned = v;
    resultsSetBit(r, PLAN_CORO_OWNED);
}

/**
 * @brief Whether codegen should end this `var`'s task when its block is left.
 *
 * @param node  The `var` statement.
 * @return      `true` when the checker proved nothing can reach the handle afterwards.
 */
bool planCoroOwned(const void *node) {
    NodeResults *r = resultsOf(node, false);
    return r && resultsBit(r, PLAN_CORO_OWNED) ? r->coroOwned : false;
}

PLF_GET(planIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET, false)
PLF_SET(planSetIsExtTarget, bool, isExtTarget, PLAN_IS_EXT_TARGET)
PLF_GET(planIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER, false)
PLF_SET(planSetIsParWorker, bool, isParWorker, PLAN_IS_PAR_WORKER)
PLF_GET(planParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA, false)
PLF_SET(planSetParTlsArena, bool, parTlsArena, PLAN_PAR_TLS_ARENA)
PLF_GET(planParFullOut, bool, parFullOut, PLAN_PAR_FULL_OUT, false)
PLF_SET(planSetParFullOut, bool, parFullOut, PLAN_PAR_FULL_OUT)

/* ---- operator / comparison facts -----------------------------------------------------
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
/**
 * @brief Tell whether an explicit conversion needs a range check at run time.
 *
 * An integer widening, a float-to-float conversion, and a reference to a 64-bit integer need no
 * check; every other narrowing, sign change, and float-to-integer conversion does.
 *
 * @param e  The conversion expression.
 * @return   `true` when a runtime check has to be emitted; `false` when there is no record.
 */
bool planConvCheck(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_CONV_CHECK) ? r->an.convCheck : false;
}
/**
 * @brief Record whether an explicit conversion needs a range check at run time.
 *
 * @param e  The conversion expression.
 * @param v  `true` when a check is needed.
 */
void planSetConvCheck(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.convCheck = v; resultsSetBit(r, AN_CONV_CHECK);
}
/**
 * @brief Tell whether a binary operation is deferred to instantiation because an operand is a type
 * parameter.
 *
 * @param e  The binary expression.
 * @return   `true` when it has to be re-resolved; `false` when there is no record.
 */
bool planNeedOp(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, AN_NEED_OP) ? r->an.needOp : false;
}
/**
 * @brief Record that a binary operation has to be deferred to instantiation for re-resolution.
 *
 * @param e  The binary expression.
 * @param v  `true` when re-resolution is needed.
 */
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

/* ---- call / receiver facts -----------------------------------------------------------
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
PLAN_GET(planLamFnPtr, bool, lamFnPtr, PLAN_LAM_FNPTR, false)
PLAN_SET(planSetLamFnPtr, bool, lamFnPtr, PLAN_LAM_FNPTR)
PLAN_GET(planAnyBox, bool, anyBox, PLAN_ANY_BOX, false)
PLAN_SET(planSetAnyBox, bool, anyBox, PLAN_ANY_BOX)
PLAN_GET(planAnyTo, const Type *, anyTo, PLAN_ANY_TO, NULL)
PLAN_SET(planSetAnyTo, const Type *, anyTo, PLAN_ANY_TO)
PLAN_GET(planDynCall, bool, dynCall, PLAN_DYN_CALL, false)
PLAN_SET(planSetDynCall, bool, dynCall, PLAN_DYN_CALL)
/**
 * @brief Is this compound assignment a call of the type's `fn +=` hook?
 *
 * `x += y` is a **statement**, so these two accessors are written out instead of using the
 * expression macros: the plan table separates statement slots from expression slots, and a cast
 * would land in the wrong one.
 *
 * @param s  The assignment statement.
 * @return   true when the statement is an in-place call rather than `x = x + y`.
 */
bool planCompoundCall(const Stmt *s) {
    NodeResults *r = resultsOf(s, false);
    return r && resultsBit(r, PLAN_COMPOUND_CALL) && r->compoundCall;
}

/**
 * @brief Record that a compound assignment is an in-place call.
 *
 * @param s  The assignment statement.
 * @param v  true when the type defines the compound form and the statement is a call.
 */
void planSetCompoundCall(Stmt *s, bool v) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->compoundCall = v;
    resultsSetBit(r, PLAN_COMPOUND_CALL);
}

/**
 * @brief @see plan.h
 */
int planLenParam(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    if (!r || !resultsBit(r, PLAN_LEN_PARAM)) return -1;
    return r->lenParam;
}

/**
 * @brief @see plan.h
 */
void planSetLenParam(Expr *e, int idx) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->lenParam = idx;
    resultsSetBit(r, PLAN_LEN_PARAM);
}
/**
 * @brief Is this declaration's initializer a copy that must go through `clone`?
 *
 * `@value` types own storage, so copying one from a place would make two values share it. The
 * temporary case is a move, not a copy, and is not marked here.
 *
 * @param s  The declaration statement.
 * @return   true when codegen must emit `<type> x = Type_clone(&(init));`.
 */
bool planVarClone(const Stmt *s) {
    NodeResults *r = resultsOf(s, false);
    return r && resultsBit(r, PLAN_VAR_CLONE) && r->varClone;
}

/**
 * @brief Record that a declaration's initializer is a `clone` call.
 *
 * @param s  The declaration statement.
 * @param v  true when the type is `@value` and the initializer is a place.
 */
void planSetVarClone(Stmt *s, bool v) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->varClone = v;
    resultsSetBit(r, PLAN_VAR_CLONE);
}

/**
 * @brief Is this argument (or struct-literal field initializer) a copy that must go through
 *        `clone`?
 *
 * A by-value parameter of an `@value` type owns its storage, so handing it a place would make two
 * values share it: the copy is built by the type's `clone` instead. A temporary is moved and is
 * never marked.
 *
 * @param e  The argument / field initializer expression.
 * @return   true when codegen must emit `Type_clone(&(e))` for this argument.
 */
bool planArgClone(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_ARG_CLONE) && r->argClone;
}

/**
 * @brief Record that a by-value argument or field initializer must be copied with `clone`.
 *
 * @param e  The argument / field initializer expression.
 * @param v  true when the target type is `@value` and this expression is a place.
 */
void planSetArgClone(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->argClone = v;
    resultsSetBit(r, PLAN_ARG_CLONE);
}

/**
 * @brief Is this `return` value a copy that must go through `clone`?
 *
 * The caller receives an independent value, so returning a place of an `@value` type would leave
 * two owners of one storage. `clone` builds the caller's copy.
 *
 * @param s  The `return` statement.
 * @return   true when codegen must emit `return Type_clone(&(e));`.
 */
bool planRetClone(const Stmt *s) {
    NodeResults *r = resultsOf(s, false);
    return r && resultsBit(r, PLAN_RET_CLONE) && r->retClone;
}

/**
 * @brief Record that a `return` value must be copied with `clone`.
 *
 * @param s  The `return` statement.
 * @param v  true when the return type is `@value` and the value is a place.
 */
void planSetRetClone(Stmt *s, bool v) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->retClone = v;
    resultsSetBit(r, PLAN_RET_CLONE);
}

/**
 * @brief Is this `new` expression the constructor of a `@buffer` type?
 *
 * `let b: buf<T> = new T[n]` asks for the owning form of the buffer, so the initializer builds the
 * value through that type's one-argument constructor instead of allocating a `slice<T>` view.
 *
 * @param e  The `new` expression.
 * @return   true when codegen must emit the constructor call.
 */
bool planNewAsBuffer(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_NEW_AS_BUFFER) && r->newAsBuffer;
}

/**
 * @brief Record that a `new` initializer builds a `@buffer` type.
 *
 * @param e  The `new` expression.
 * @param v  true when the annotation asked for the owning form.
 */
void planSetNewAsBuffer(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->newAsBuffer = v;
    resultsSetBit(r, PLAN_NEW_AS_BUFFER);
}
PLAN_GET(planPadKind, PlanPadKind, padKind, PLAN_PAD_KIND, PLAN_PAD_NONE)
PLAN_SET(planSetPadKind, PlanPadKind, padKind, PLAN_PAD_KIND)

/* `storedAt` is the one field of this group whose **unset value is not the type's zero**:
 * "has this value been seen being published?" is answered `-1` (not yet) or a level `>= 0`
 * (the level it was published at), and `0` is a real answer. Seeding it from `exprNew`
 * would make the AST constructor reach into the plan's storage -- a dependency pointing up
 * from the AST layer into the plan layer. The default belongs to the reader, not to the
 * constructor: a node nothing has published is "not yet published", always. */
#define AN_STORED_AT_GET(fn)                                               \
    int fn(const void *node) {                                             \
        NodeResults *r = resultsOf((node), false);                         \
        return (r && resultsBit(r, AN_STORED_AT)) ? r->an.storedAt : -1;   \
    }
AN_STORED_AT_GET(anStoredAt)
/**
 * @brief Record the level a value is published to.
 *
 * `-1` means it was never published, `>= 0` is the level it was published at; `0` is a real level, so
 * only the reader can supply the default for "not set".
 *
 * @param node  The target value node; NULL ignores this write.
 * @param v     The publication level: `-1` (never published) or `>= 0`.
 */
void anSetStoredAt(void *node, int v) {
    if (!node) return;
    NodeResults *r = resultsAs(node, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->an.storedAt = v; resultsSetBit(r, AN_STORED_AT);
}

/* ---- depth and origin facts ----------------------------------------------------------
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

/* ---- call-site markers ---------------------------------------------------------------
 *
 * Three builtin forms have no `Expr.func` to recognise them by: an assoc builtin call and
 * a body-less declaration reach code generation with a NULL callee, so keying on the
 * callee can never fire. Each gets its own marker, written by the checker at the call
 * site and read by code generation. */

/**
 * @brief Get the worker function in a `parallel::run(worker, ...)` call.
 *
 * @param e  The call node.
 * @return   The worker function; NULL when it is not such a call.
 */
FuncDef *planParWorker(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return (r && resultsBit(r, PLAN_PAR_WORKER)) ? r->parWorker : NULL;
}
/**
 * @brief Record the worker function in a `parallel::run(worker, ...)` call.
 *
 * @param e   The call node; NULL ignores this write.
 * @param wf  The worker function.
 */
void planSetParWorker(Expr *e, FuncDef *wf) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->parWorker = wf; resultsSetBit(r, PLAN_PAR_WORKER);
}

/**
 * @brief Tell whether this call is the builtin form that builds a domain.
 *
 * Such a form has no callee pointer to rely on, so it is recognised by a mark on the call node.
 *
 * @param e  The call node.
 * @return   `true` when it is; `false` when there is no record.
 */
bool planDomNew(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_DOM_NEW) ? r->domNew : false;
}
/**
 * @brief Mark this call as the builtin form that builds a domain.
 *
 * @param e  The call node; NULL ignores this write.
 * @param v  `true` when it is.
 */
void planSetDomNew(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->domNew = v; resultsSetBit(r, PLAN_DOM_NEW);
}

/**
 * @brief Tell whether this call is the `extc_viewOf` builtin form, which has no function body.
 *
 * The declaration has no body and therefore no callee pointer to rely on, so the call is recognised
 * by a mark on the call node.
 *
 * @param e  The call node.
 * @return   `true` when it is; `false` when there is no record.
 */
bool planViewOf(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r && resultsBit(r, PLAN_VIEW_OF) ? r->viewOf : false;
}
/**
 * @brief Mark this call as the `extc_viewOf` builtin form.
 *
 * @param e  The call node; NULL ignores this write.
 * @param v  `true` when it is.
 */
void planSetViewOf(Expr *e, bool v) {
    if (!e) return;
    NodeResults *r = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (!r) return;
    r->viewOf = v; resultsSetBit(r, PLAN_VIEW_OF);
}

/* ---- impl / trait / module facts ---------------------------------------------------- */

/**
 * @brief Get the trait an `impl` block implements.
 *
 * @param im  The `impl` block definition.
 * @return    The implemented trait; NULL for an inherent `impl` or before resolution.
 */
TraitDef *planImplTrait(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TRAIT)) ? r->implTrait : NULL;
}
/**
 * @brief Record the trait an `impl` block implements.
 *
 * @param im  The `impl` block definition; NULL ignores this write.
 * @param tr  The implemented trait.
 */
void planSetImplTrait(ImplDef *im, TraitDef *tr) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTrait = tr; resultsSetBit(r, PLAN_IMPL_TRAIT);
}

/**
 * @brief Get the concrete type an `impl` block resolves to.
 *
 * @param im  The `impl` block definition.
 * @return    The target type; NULL when there is no record.
 */
Type *planImplTarget(const ImplDef *im) {
    NodeResults *r = resultsOf(im, false);
    return (r && resultsBit(r, PLAN_IMPL_TARGET)) ? r->implTarget : NULL;
}
/**
 * @brief Record the concrete type an `impl` block resolves to.
 *
 * @param im  The `impl` block definition; NULL ignores this write.
 * @param t   The target type.
 */
void planSetImplTarget(ImplDef *im, Type *t) {
    if (!im) return;
    NodeResults *r = resultsAs(im, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->implTarget = t; resultsSetBit(r, PLAN_IMPL_TARGET);
}

/**
 * @brief Tell whether a trait is used by a `dyn` form.
 *
 * Only when it is is its vtable generated.
 *
 * @param tr  The target trait.
 * @return    `true` when it is used; `false` when there is no record.
 */
bool planUsedDyn(const TraitDef *tr) {
    NodeResults *r = resultsOf(tr, false);
    return r && resultsBit(r, PLAN_USED_DYN) ? r->usedDyn : false;
}
/**
 * @brief Record that a trait is used by a `dyn` form.
 *
 * @param tr  The target trait; NULL ignores this write.
 * @param v   `true` when it is used.
 */
void planSetUsedDyn(TraitDef *tr, bool v) {
    if (!tr) return;
    NodeResults *r = resultsAs(tr, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usedDyn = v; resultsSetBit(r, PLAN_USED_DYN);
}

/**
 * @brief Tell whether a `dyn` form appears in a module.
 *
 * The dyn runtime is generated only when one does.
 *
 * @param m  The target module.
 * @return   `true` when one appears; `false` when there is no record.
 */
bool planUsesDyn(const Module *m) {
    NodeResults *r = resultsOf(m, false);
    return r && resultsBit(r, PLAN_USES_DYN) ? r->usesDyn : false;
}
/**
 * @brief Record that a `dyn` form appears in a module.
 *
 * @param m  The target module; NULL ignores this write.
 * @param v  `true` when one appears.
 */
void planSetUsesDyn(Module *m, bool v) {
    if (!m) return;
    NodeResults *r = resultsAs(m, true, RKIND_OTHER, __LINE__);
    if (!r) return;
    r->usesDyn = v; resultsSetBit(r, PLAN_USES_DYN);
}

/**
 * @brief Tell whether a function is ever called.
 *
 * Code generation emits only the functions that were called, plus the roots `main`, `@export`, and the
 * dyn vtable entries. When no call was recorded the function reads as unused, which is the safe
 * direction: a missed call site never leaves a body nobody references in the output.
 *
 * @param f  The function definition.
 * @return   true when a call was recorded; false when there is no record.
 */
bool planUsed(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_USED) ? r->used : false;
}

/**
 * @brief Record whether a function is ever called.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it is called.
 */
void planSetUsed(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->used = v;
        resultsSetBit(r, PLAN_USED);
    }
}

/* One entry per (method template, receiver type instance) that was really used. A linear
 * table on purpose: the counts are tiny (a handful of methods times the instances a program
 * actually mentions), and the lookup happens in the replay loops, not in a hot path. Keys are
 * copied because a call site's type name may live in a different arena than the next one. */
typedef struct { const FuncDef *m; char *recv; } PlanInstUse;
static PlanInstUse *gInstUses;
static size_t gInstUsesLen;
static size_t gInstUsesCap;

/**
 * @brief Record that method `m` was used on this receiver type instance (idempotent; repeats keep
 * one entry).
 *
 * @param m          The template definition of the method or operator; NULL ignores this call.
 * @param recvName   The receiver type instance's name; NULL ignores this call.
 */
void planSetInstUsed(const FuncDef *m, const char *recvName) {
    if (!m || !recvName) return;
    for (size_t i = 0; i < gInstUsesLen; i++)
        if (gInstUses[i].m == m && strcmp(gInstUses[i].recv, recvName) == 0) return;
    if (gInstUsesLen == gInstUsesCap) {
        size_t cap = gInstUsesCap ? gInstUsesCap * 2 : 16;
        PlanInstUse *grown = realloc(gInstUses, cap * sizeof *grown);
        if (!grown) return;                       /* a failed record only emits extra instance bodies, never a wrong one */
        gInstUses = grown;
        gInstUsesCap = cap;
    }
    size_t n = strlen(recvName) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, recvName, n);
    gInstUses[gInstUsesLen].m = m;
    gInstUses[gInstUsesLen].recv = copy;
    gInstUsesLen++;
}

/**
 * @brief Was method `m` used on this receiver type instance (reads as false when there is no record)?
 *
 * @param m          The template definition of the method or operator.
 * @param recvName   The receiver type instance's name.
 * @return           true when it was recorded.
 */
/**
 * @brief How many "(method, instance) used" entries have been recorded -- the caller uses it to tell
 * whether the closure is still growing.
 *
 * @return  The number of entries.
 */
size_t planInstUseCount(void) { return gInstUsesLen; }

/**
 * @brief Was this "(method template, receiver instance)" really used?
 *
 * The checker records it at concrete use sites and during instance replay, and replay and code
 * generation use it to decide whether a type instance needs its method body re-checked or emitted:
 * `slice<u8> ==` must not drag in `slice<map::bnode<...>>`.
 *
 * @param m          The template definition of the method or operator; NULL returns false.
 * @param recvName   The receiver type instance's name; NULL returns false.
 * @return           true when it was used.
 */
bool planInstUsed(const FuncDef *m, const char *recvName) {
    if (!m || !recvName) return false;
    for (size_t i = 0; i < gInstUsesLen; i++)
        if (gInstUses[i].m == m && strcmp(gInstUses[i].recv, recvName) == 0) return true;
    return false;
}

/**
 * @brief "Nothing in this function's dynamic extent does a `dec`" -- the basis for skipping
 * `initRef`'s `+1`.
 *
 * The check side computes it as the **least fixpoint** over the whole call graph (this function
 * writes no reference slot, has no unresolved callee, no extern or concurrency, and every callee
 * qualifies). Default `false`: when unknown, count as usual.
 *
 * @param f  The function definition.
 * @return   `true` when the `+1` can be skipped; `false` when there is no record.
 */
bool planRcNoDec(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_RC_NODEC) ? r->rcNoDec : false;
}

/**
 * @brief Record the conclusion "this function never decrefs" (written only by the check side's
 * call-graph fixpoint).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetRcNoDec(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->rcNoDec = v;
        resultsSetBit(r, PLAN_RC_NODEC);
    }
}

/**
 * @brief "The heap-reference graph this function builds has no back edge, and references live only
 * in counted rc slots".
 *
 * Static premises of this criterion: when it holds, the event band (counting) is **complete** and no
 * sweep is needed. All three acyclicity criteria (i)(ii)(iii) and "references land only in counted
 * heap slots" (iv) have to hold; the criteria, the reasons, and the coverage table are in
 * `noteAcyclicStore` in `check_stmt.c` and `computeFnAcyclic` in `check_top.c`. Default `false`:
 * unknown (generic template / instance, extern, no body) or not holding both keep the sweep, which is
 * the safe direction.
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnAcyclic(FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_FN_ACYCLIC) ? r->fnAcyclic : false;
}

/**
 * @brief Record "the heap-reference graph this function builds has no back edge" (criteria
 * (i)(ii)(iii)).
 *
 * Written only by the check side's call-graph fixpoint; `planFnAcyclic` still conjoins it with the
 * (iv) conclusion.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnAcyclic(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->fnAcyclic = v;
        resultsSetBit(r, PLAN_FN_ACYCLIC);
    }
}

/**
 * @brief "References in this function live only in counted rc slots" (criterion (iv)).
 *
 * The sibling of `planSetFnAcyclic`: it is stored separately because the closure over the call graph
 * differs -- (i') looks at whether a callee is wholly acyclic, whereas (iv) looks only at a callee's
 * (iv).
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnRefsCounted(FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_FN_REFS_COUNTED) ? r->fnRefsCounted : false;
}

/**
 * @brief Record "references in this function live only in counted rc slots" (criterion (iv)).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnRefsCounted(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->fnRefsCounted = v;
        resultsSetBit(r, PLAN_FN_REFS_COUNTED);
    }
}

/**
 * @brief "This function never drops a live reference": criteria (a)(b)(c).
 *
 * (a) every heap ref field this function writes is written **at most once per object**
 * (initialisation, not overwrite); (b) a local or parameter of ref type is not reassigned (a
 * declaration initialiser does not count); (c) there is no reference-dropping operation: container
 * removal or replacement, `@overwrite` reusing the same cell, free-class builtins, and any callee that
 * (transitively) violates (a)/(b)/(c) -- the transitive half is not computed here but covered by
 * the **whole-unit conjunction** (`unitReclaimIdle` in `codegen.c`), since every callee is judged on
 * its own as well.
 *
 * When it holds the function performs no action that makes an object unreachable early; when the
 * whole unit holds as well, objects inside a region disappear only together with the region, so
 * there is nothing to reclaim and the rc runtime is not emitted. Default `false`: unknown (generic
 * template / instance / extern) or not holding both emit as usual, which is the safe direction (a
 * wrong judgement under-collects, growing memory rather than dangling).
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnReclaimIdle(FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && resultsBit(r, PLAN_FN_RECLAIM_IDLE) ? r->fnReclaimIdle : false;
}

/**
 * @brief Record "this function never drops a live reference" (criteria (a)(b)(c)).
 *
 * Written only by `computeFnAcyclic` in `check_top.c`; `codegen.c` reads it only through
 * `planFnReclaimIdle`.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnReclaimIdle(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (r) {
        r->fnReclaimIdle = v;
        resultsSetBit(r, PLAN_FN_RECLAIM_IDLE);
    }
}

/**
 * @brief The "may form a cycle" group number of an allocation site; 0 means it belongs to no
 * cross-site possible cycle.
 *
 * The criterion is in `applyCycleMerges` in `check_escape.c`: sites in the same connected component
 * may form cycles with each other, so the reclaim shards must not separate them.
 *
 * @param e  The allocation-site expression.
 * @return   The group number (starting at 1); 0 when there is no record.
 */
int planSiteCycleGroup(const Expr *e) {
    NodeResults *r = resultsOf(e, false);
    return r ? r->cycleGroup : 0;
}

/**
 * @brief Record an allocation site's "may form a cycle" group number (as above).
 *
 * @param e  The allocation-site expression; NULL ignores this write.
 * @param g  The group number (starting at 1).
 */
void planSetSiteCycleGroup(Expr *e, int g) {
    if (!e) return;
    NodeResults *s = resultsAs(e, true, RKIND_EXPR, __LINE__);
    if (s) s->cycleGroup = g;
}

/**
 * @brief Does the unit this function belongs to have a **cross-site** possible cycle (yes means that
 * unit rejects sharding)?
 *
 * @param f  The function definition.
 * @return   `true` when there is a cross-site possible cycle; `false` when there is no record.
 */
bool planFnCrossSiteCycle(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return r && r->fnCrossSiteCycle;
}

/**
 * @brief Record "the unit this function belongs to has a cross-site possible cycle" (as above).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnCrossSiteCycle(FuncDef *f, bool v) {
    if (!f) return;
    NodeResults *s = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (s) s->fnCrossSiteCycle = v;
}

/**
 * @brief The classification of "a write into a reference slot" at an assignment site (`SR_*`).
 *
 * The checker records it **while the scope is still alive** (the level comparison needs a `Sym`), and
 * `computeFnAcyclic` reads it after checking the body. `0` (`SR_NONE`) means it is not a reference-slot
 * store, or that the walk did not visit this site -- the latter is judged as a rejection.
 *
 * @param s  The assignment statement.
 * @return   The classification; `0` when there is no record.
 */
int anStoreRefKind(const Stmt *s) {
    NodeResults *r = resultsOf(s, false);
    return (r && resultsBit(r, AN_STORE_REF_KIND)) ? r->an.storeRefKind : 0;
}

/**
 * @brief Record the reference-slot store classification of an assignment site.
 *
 * @param s  The assignment statement; NULL ignores this write.
 * @param v  The `SR_*` classification.
 */
void anSetStoreRefKind(Stmt *s, int v) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->an.storeRefKind = v;
    resultsSetBit(r, AN_STORE_REF_KIND);
}

/**
 * @brief The reason string for which this function was rejected (printed directly under
 * `EXTC_DBG_ACYCLIC=1`).
 *
 * @param f  The function definition.
 * @return   The reason string; NULL when the function was accepted or not computed yet.
 */
const char *anAcyclicWhy(const FuncDef *f) {
    NodeResults *r = resultsOf(f, false);
    return (r && resultsBit(r, AN_ACYCLIC_WHY)) ? r->an.acyclicWhy : NULL;
}

/**
 * @brief Record the reason string for which this function was rejected.
 *
 * @param f    The function definition; NULL ignores this write.
 * @param why  The reason string (should be a constant string from the arena).
 */
void anSetAcyclicWhy(FuncDef *f, const char *why) {
    if (!f) return;
    NodeResults *r = resultsAs(f, true, RKIND_FUNC, __LINE__);
    if (!r) return;
    r->an.acyclicWhy = why;
    resultsSetBit(r, AN_ACYCLIC_WHY);
}

/**
 * @brief Record the callee a call site resolves to, per enclosing function body or type instance.
 *
 * One call site inside a generic body is shared by all instances of that body, and each instance may
 * resolve to a different function (`wrap<i32>` calls `pick_i32`, `wrap<meter>` calls `pick_meter`),
 * so the answer is recorded per (call site, enclosing body) pair and at least one of `enclosing` and
 * `inst` has to be given. Repeating the write for the same pair only overwrites the callee, because
 * the deferred-call closure replays several rounds.
 *
 * @param e          The call-site expression; NULL ignores this write.
 * @param enclosing  The function instance containing the call site; NULL when resolving inside a
 *                   method body.
 * @param inst       The type instance containing the call site; NULL when resolving inside a free
 *                   function body.
 * @param callee     The function definition resolved inside that enclosing body.
 */
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

/**
 * @brief Announce which function instance is emitted next, so `planCallee` picks the resolution
 * recorded per body.
 *
 * @param f  The function instance to be emitted.
 */
void planEnterFunc(const FuncDef *f) { g_emitFunc = f; }
/**
 * @brief End the function instance's emission context.
 */
void planLeaveFunc(void) { g_emitFunc = NULL; }
/**
 * @brief Announce which type instance's methods are emitted next, so `planCallee` picks the
 * resolution recorded per body.
 *
 * @param t  The type instance whose methods are to be emitted.
 */
void planEnterInst(const Type *t) { g_emitInst = t; }
/**
 * @brief End the type instance's emission context.
 */
void planLeaveInst(void) { g_emitInst = NULL; }

/**
 * @brief Get the arena level an allocation point should use.
 *
 * The `ARENA_HOME` sentinel means "use the home arena the caller passed"; a larger number means a
 * shorter lifetime.
 *
 * @param e  The allocation-point expression.
 * @return   The recorded level; 0 when there is no record.
 */
int planArenaLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ARENA_LEVEL)) ? s->arenaLevel : 0;
}
/**
 * @brief Get the zone level recorded for a pool site.
 *
 * `ZONE_HOME` means "use the home zone the caller received" (library functions are transparent about
 * place and pass the choice to the caller), k >= 1 means the zone of the block at level k; when the
 * site has no record, 0 is returned.
 *
 * @param e  The expression to query (the call site that creates the pool).
 * @return   The recorded zone level; 0 when there is no record.
 */
int planZoneLevel(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ZONE_LEVEL)) ? s->zoneLevel : 0;
}
/**
 * @brief Get the arena argument a call passes down.
 *
 * `-1` means the caller's own home arena, `>= 1` that block level, and `0` the current block.
 *
 * @param e  The call node.
 * @return   The recorded argument; 0 when there is no record.
 */
int planArenaArg(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_ARENA_ARG)) ? s->arenaArg : 0;
}
/**
 * @brief Record whether a call site's arena argument still waits for the final round.
 *
 * Once it has landed (promotion has already given a longer-lived level, or the final round has run),
 * it has to be cleared, otherwise the final round treats it as "not computed yet" and lifts it back
 * to the home arena.
 *
 * @param e  The call-site expression; NULL ignores this write.
 * @param v  true when it still waits for the final round.
 */
void planSetArenaArgPending(Expr *e, bool v) {
    if (e) e->arenaArgPending = v;
}

/**
 * @brief Tell whether an expression must first evaluate some part into a temporary.
 *
 * The expression (or its arguments, or its element count) appears more than once in the generated C,
 * or has to be evaluated only once outside a loop, so it is recorded as true and code generation
 * emits a temporary declaration first.
 *
 * @param e  The expression to query.
 * @return   true when a temporary is needed; false when there is no record.
 */
bool planNeedTemp(const Expr *e) {
    NodeResults *s = resultsAs(e, false, RKIND_EXPR, __LINE__);
    return (s && resultsBit(s, PLAN_NEED_TEMP)) ? s->needTemp : false;
}

/**
 * @brief Tell whether a function receives the caller's home arena as a hidden parameter.
 *
 * The answer rests on "the body really reads the home arena it was given, or passes the home arena
 * down to a callee", and its value is only final once the summary closure has run.
 *
 * @param f  The function to query.
 * @return   true when the hidden home arena parameter is needed; false when there is no record.
 */
bool planUsesHome(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_USES_HOME) ? s->usesHome : false;
}
/**
 * @brief Tell whether a function body allocates storage itself.
 *
 * When false the body allocates nothing, so code generation declares neither an arena array for it
 * nor a release call.
 *
 * @param f  The function to query.
 * @return   true when it may allocate; false when there is no record.
 */
bool planMayUseArena(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAY_USE_ARENA) ? s->mayUseArena : false;
}
/**
 * @brief Tell whether a function creates a pool, either directly or through a callee.
 *
 * The value is only final once the call-graph closure has run.
 *
 * @param f  The function to query.
 * @return   true when it creates a pool; false when there is no record.
 */
bool planMakesPool(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_MAKES_POOL) ? s->makesPool : false;
}
/**
 * @brief Tell whether a loop's condition allocates storage.
 *
 * When true, code generation wraps the loop in an arena reclaimed every round, so allocations in the
 * condition return at the end of the round.
 *
 * @param st  The loop statement to query.
 * @return    true when the condition allocates; false when there is no record.
 */
bool planCondAllocs(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && resultsBit(s, PLAN_COND_ALLOCS) ? s->condAllocs : false;
}
/**
 * @brief Record whether a declaration's initialiser points at storage outside the frame.
 *
 * Storage outside the frame covers globals, `global` declarations, the results of `effects Ret=0`,
 * and string literals; this result is written while the scope is still alive so it can be asked about
 * after the function body has been checked.
 *
 * @param st  The variable declaration statement.
 * @param v   true when the initialiser points outside the frame.
 */
void planSetVarInitOutOfFrame(Stmt *st, bool v) {
    if (!st) return;
    NodeResults *s = resultsAs(st, true, RKIND_STMT, __LINE__);
    s->varInitOO = v; resultsSetBit(s, PLAN_VAR_INIT_OO);
}
/**
 * @brief Read back whether a declaration's initialiser points at storage outside the frame.
 *
 * @param st  The variable declaration statement to query.
 * @return    true when the initialiser points outside the frame; false when there is no record.
 */
bool planVarInitOutOfFrame(const Stmt *st) {
    NodeResults *s = resultsAs(st, false, RKIND_STMT, __LINE__);
    return s && resultsBit(s, PLAN_VAR_INIT_OO) ? s->varInitOO : false;
}
/**
 * @brief Get the step statement of a desugared `for`.
 *
 * The step statement is syntax and is stored on the loop body, because `continue` must reach it; once
 * the loop is rewritten into an iterator there is no step any more, and this returns NULL.
 *
 * @param s  The loop-body statement to query.
 * @return   The step statement; NULL when the step has been dropped, `s` is NULL, or there is no
 *           step.
 */
Stmt *planForStep(const Stmt *s) {
    if (!s) return NULL;
    NodeResults *r = resultsOf(s, false);
    if (r && resultsBit(r, PLAN_FOR_STEP_DROPPED) && r->forStepDropped) return NULL;
    return s->forStep;
}

/**
 * @brief Record that the step statement of a desugared `for` is dropped.
 *
 * Once the loop is rewritten into an iterator it is advanced by the iterator's `next()`, `continue`
 * does not have to jump to the step label any more, and the label is not emitted either.
 *
 * @param s  The loop-body statement that carried the step statement.
 */
void planSetForStepDropped(Stmt *s) {
    if (!s) return;
    NodeResults *r = resultsAs(s, true, RKIND_STMT, __LINE__);
    if (!r) return;
    r->forStepDropped = true;
    resultsSetBit(r, PLAN_FOR_STEP_DROPPED);
}

/**
 * @brief Tell whether a function is a coroutine.
 *
 * A coroutine's value is a frame handle rather than an ordinary C function.
 *
 * @param f  The function to query.
 * @return   true when it is a coroutine; false when there is no record.
 */
bool planIsCoro(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_IS_CORO) ? s->isCoro : false;
}
/**
 * @brief Get the type of the value a coroutine `yield`s.
 *
 * For `coroutine<A, B>` this is the last type argument; for the single-argument `coroutine<T>` it is
 * that one argument.
 *
 * @param f  The function to query.
 * @return   The yielded value's type; NULL when it is not a coroutine or there is no record.
 */
Type *planYieldType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_YIELD_TYPE) ? s->yieldType : NULL;
}
/**
 * @brief Get the C struct type of a coroutine frame.
 *
 * The frame struct's field layout matches the fields code generation emits, and the protocol methods
 * driving the coroutine hang off it.
 *
 * @param f  The function to query.
 * @return   The frame struct type; NULL when it is not a coroutine or the frame is not set up yet.
 */
Type *planCoroFrameType(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_FRAME_TYPE) ? s->coroFrameType : NULL;
}
/**
 * @brief Tell whether a coroutine needs a place (zone) of its own.
 *
 * A coroutine that creates a pool, or whose frame was ever boxed into a handle, always has a task,
 * and its place is held by the task; when it does not, the step function is driven directly.
 *
 * @param f  The function to query.
 * @return   true when it needs its own zone; false when there is no record.
 */
bool planCoroNeedsZone(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_NEEDS_ZONE) ? s->coroNeedsZone : false;
}
/**
 * @brief Get the protocol number of a synthesised protocol method.
 *
 * A protocol method has no body and is expanded inline at the call site by code generation; the
 * number says which method it is: next/value/send on the frame are 1/2/5, and on the handle they are
 * 3/4/6.
 *
 * @param f  The function to query.
 * @return   The protocol number; 0 for an ordinary function.
 */
int planCoroProto(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_PROTO) ? s->coroProto : 0;
}
/**
 * @brief Tell whether a coroutine's frame is ever converted into a handle at an assignment.
 *
 * A boxed coroutine always gets a task, so it counts as needing a zone of its own; this fact is
 * written wherever the conversion happens.
 *
 * @param f  The function to query.
 * @return   true when the frame was ever boxed into a handle; false when there is no record.
 */
bool planCoroBoxed(const FuncDef *f) {
    NodeResults *s = resultsAs(f, false, RKIND_FUNC, __LINE__);
    return s && resultsBit(s, PLAN_CORO_BOXED) ? s->coroBoxed : false;
}
