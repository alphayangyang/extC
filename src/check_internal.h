#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

/* Internal header of the checker: the state shared between the check_*.c files and the
 * prototypes that cross between them.
 *
 * The only public entry point is `checkModule` from `check.h`. This header holds types
 * and prototypes only, never logic.
 */

#include "check.h"

/* Display name of a declaration for diagnostics: `DN(x)` takes a `StructDef *` or a
 * `TypeDef *`.
 *
 * After module mangling `name` is the internal `io$reader`, while `srcName` is the
 * `io::reader` the user wrote. Printing `->name` leaks the internal encoding into the
 * message, as in "struct `alpha$pair` has no field `zzz`".
 */
#define DN(d) ttDispName((d) ? (d)->srcName : NULL, (d) ? (d)->name : NULL)

/* Display name of a function, for diagnostics and debug output: `FN(f)`.
 *
 * After mangling `f->name` is `pair$make` while the user wrote `pair::make`, which the
 * effect dump used to print verbatim. In the root module, or in a single-file program,
 * `modName` is empty and `name` already is the source name. The implementation replaces
 * the `mod$` prefix with `mod::`, and that prefix is always `modName`.
 */
const char *checkFnDisplay(const char *name, const char *modName);
#define FN(f) checkFnDisplay((f) ? (f)->name : NULL, (f) ? (f)->modName : NULL)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------ shared state and shared types
 * Whatever more than one file needs, so that whoever uses it can see it. */

typedef struct {
    const char *name;    /* name as written in the source */
    const char *cname;   /* name used in generated C; a shadowed `a` becomes `a__2` */
    Type       *type;    /* declared type, or the inferred one for an unannotated `let` */
    bool        mut;     /* true for `var`; false for `let` and for payload bindings */
    /* True once the address of this binding has been handed out: `ref x`, `x[..]`, or
     * passing it as a `mut ref` argument.
     *
     * With a taken address an alias can change the contents behind our back, so the
     * depth bookkeeping may only be updated weakly. Without one an entry is overwritten
     * outright, which removes the false rejections where writing null or a shallower
     * value could not lower the recorded depth. */
    bool        addressed;
    /* Per-field depths, at most four entries. To be read as one field block, not as
     * four independent ones.
     *
     * A field such as `h.p` gets a depth of its own instead of sharing one number with
     * `h`, so `h.p = null` can really overwrite that entry. Writes that cannot be
     * attributed to a single field, such as an element write, go to `otherDepth`. The
     * effective depth of the root is the maximum of `otherDepth` and the field entries. */
    struct { const char *name; int depth;  /* field name; recorded depth of the value in it */
             /* Which value was last written into this field, or NULL when that is
              * unknown. It is used only to find the allocation site inside a container,
              * never to change any bookkeeping.
              *
              * It is needed because `var h: box = { p: null, q: null }` has an origin
              * made of nulls which never mentions the `x` that a later `h.q = x` stored,
              * so walking origins would never reach that site. */
             Expr *src;
             /* Depth of the write that produced `src`. Compare against this number, not
              * against `depth`: `depth` is merged with the maximum on a weak update, so
              * after `h.q = x  h.q = null` it is still deep, and comparing with it would
              * call the null write the deeper one, clear the source and break the chain. */
             int   srcDepth;
             /* The shallowest level this field has been required to reach; 0 means it
              * must outlive the frame. `out = h` stores at level 1, but `return out`
              * needs the frame-outliving level, so the site has to end up in the home
              * arena. Looking only at the current destination computes a level that is
              * too shallow and leaves the site there, which dangles. */
             int   minReq; } fields[4];
    /* Whether the field table is complete.
     *
     * Complete means every field of the binding has an entry, or is known to be 0; only
     * then may an entry be cleared and the root recomputed as the maximum of the
     * remaining ones. An incomplete table, such as one copied from elsewhere, can have
     * fields with values that are missing here, and lowering a depth would make those
     * values vanish; that is how a stack-use-after-scope was introduced. The default is
     * false, the conservative answer. */
    bool        fieldsComplete;
    int         nfields;    /* number of valid entries in `fields` */
    int         otherDepth; /* depth for writes that belong to no single field */
    /* A store a **callee** made through this binding as a `mut ref` argument. Its own slot: the
     * "no single field" one is shared with element writes, and feeding both through it gave every
     * element write promotion pressure (three red promotion corpora). The source expression is what
     * lets `promoteFieldsAt` move the site behind it -- the difference between accepting
     * `stash(ref b, n)` with `n = new node` (audit P0-16d) and reading freed memory. */
    Expr       *callSrc;
    int         callSrcDepth;
    int         callMinReq;
    /* The same facts as one `fields[]` entry, for a store that belongs to no single field: an
     * element write, or a store the callee made through a `mut ref` out-parameter. Without a
     * source expression there is nothing to promote, and a `new` handed to a callee that stores it
     * would be rejected where the same program written as a direct `b.p = n` is accepted
     * (audit P0-16). */
    Expr       *otherSrc;
    int         otherSrcDepth;
    int         otherMinReq;
    int         depth;      /* lexical depth: a parameter is 0, a local in the function
                             * body is 1, and every nested block adds 1. The escape
                             * checks compare this number. */
    /* For a binding that holds a reference: how deep is the thing it points at?
     *
     * This differs from `depth`. In `var cur: ?ref node = head` the slot holding the
     * reference is in this frame, at depth 1, while the node it points at is outside,
     * at depth 0. The rule "a reference may not outlive what it points at" asks about
     * the pointee, so it needs this number. Using the slot depth everywhere falsely
     * rejected the most ordinary list search, `while cur != null { ... return cur }`,
     * as pointing at a local that dies.
     *
     * It is meaningful for a reference-typed binding, and it doubles as an upper bound
     * on where the references inside a reference-carrying value point. The default is
     * the slot depth, the conservative answer; a declaration or a retarget narrows it
     * to the real depth of the value. */
    int         refDepth;
    /* Set when this binding was initialized from an expression whose storage is provably **not**
     * in any frame: a chain rooted at a global, or a call whose declaration says `effects Ret=0`
     * (the plate's views; `mmap`-style handles). Then a **view**-typed binding answers with
     * `refDepth` instead of its slot's block depth, which is what lets
     *
     *     var v: mut slice<u8> = pl.view(off, n)!      // 板的内存，不在帧里
     *     c.func(v.data)                               // 未签字的 C 调用也接受
     *
     * work without the user signing anything (`std::heap` signed `Ret=0` once). Conservative by
     * default: an unmarked binding keeps the slot depth -- a `new` block, a stack array, an arena
     * allocation, or a callee that did not claim it. */
    bool        outOfFrame;
    /* The value this binding was initialized with, or NULL when it had no initializer.
     *
     * Promotion walks origins backwards:
     *     var n = new node        // the origin of `n` is this `new`
     *     head = n                // stored into an outer place, so follow `n` back and
     *                             // raise that `new` to the level of `head`
     * Only the initializer is followed, and only through explicit assignments; aliases
     * are not chased, so a site that cannot be reached keeps its depth error. */
    Expr       *origin;
    /* The expression that last gave this binding its value, kept unflattened.
     *
     * `origin` answers "where does the storage inside this binding come from" and flattens
     * a chain of bindings to the expression at its root, which is what the arena-level walk
     * needs. The level pass asks a narrower question -- which expression is this binding
     * holding -- and there the root of a chain is the wrong answer, because it belongs to
     * whichever binding the chain happened to end at.
     *
     * It still describes only the last assignment, so it cannot stand for a binding written
     * in both arms of an `if`; the publication records are the authority for that, and this
     * field is a shortcut for the chain-of-assignments case. */
    Expr       *heldSrc;
    /* 这一格是**池的提权**专用的：绑定由一次"建池的调用"初始化时，把那次调用的
     * 表达式记在这里（PLAN #87）。
     *
     * 为什么不复用 `origin`：`originOf` 只认分配站点的形状（`new` / `alloc`），
     * 对调用返回 NULL，因为 arena 的层级不由调用决定而在调用点决定（见 `origin` 的注释）；
     * 而池的层级恰恰**就是**由调用点决定的（`extc_pool_new_at` 的第二个实参）。
     * 单开一格，别的消费者（`origin` 那一族）一个都不受影响。 */
    Expr       *poolSite;

    int         line;      /* source line of the declaration, for diagnostics */
    /* The module this binding belongs to; set for globals, NULL for locals. Name
     * resolution uses it to reject an unqualified reference to another module's name. */
    const char *modName;
} Sym;

/* Pins the binding a name resolved to onto the AST node that used it.
 *
 * The pass that runs after all bodies have been checked cannot look a local up by name,
 * because its scope is gone by then; the same trap is described in `noteOrigin`. What
 * the pinning avoids is a re-check that silently uses the depth frozen before the
 * solver ran and rejects a correct program.
 *
 * A binding pointer is stored rather than a name, because one name in different scopes
 * is a different binding: the answer at the moment of resolution is the authoritative
 * one, while looking the name up again later can find another binding that happens to
 * have the same name. The lifetime is not a problem, because bindings are
 * arena-allocated, so leaving a scope only removes them from visibility.
 *
 * The wrapper is there for type safety: `ast.h` cannot name `Sym` and therefore holds
 * an opaque pointer, the same arrangement a `cname` uses, so each side casts once
 * instead of passing a bare `void *` around. */
typedef struct { Sym *sym; } IdentBinding;

/* The binding an `EX_IDENT` node resolved to, or NULL when it has not been resolved. */
static inline Sym *identBindOf(Expr *e) {
    return (e && e->kind == EX_IDENT && e->u.ident.sym) ? ((IdentBinding *)e->u.ident.sym)->sym : NULL;
}

/* The one place that converts between "which level an allocation site lives at" and
 * "how deep the references inside it are".
 *
 * The same conversion used to be written five times across four files and the variants
 * did not agree: two of them computed `(arenaLevel == HOME) ? 0 : arenaLevel`, while
 * another preferred a non-zero `refDepth` and could return a depth that contradicted
 * the level of the site. The bug surfaced on a site whose `refDepth` was 0 while its
 * level said the depth should be 1.
 *
 * `refDepth` and `arenaLevel` have to be two spellings of the same number, and that
 * holds only while the conversion lives in one place. Deriving the answer from the
 * level is also strictly more accurate, since `refDepth` is not authoritative while a
 * level is still undecided.
 *
 * Params:
 *   arenaLevel - arena level of a site: `ARENA_HOME` for storage that outlives this
 *                frame, k >= 1 for the k-th nested block, 0 or negative while the
 *                level is not decided yet
 *
 * Returns:
 *   The equivalent depth: 0 for the home arena and for an undecided level, k for block
 *   level k. A caller that has to tell "undecided" from "depth 0" must look at
 *   `arenaLevel` itself.
 */
static inline int arenaDepthOf(int arenaLevel) {
    if (arenaLevel == ARENA_HOME) return 0;
    return arenaLevel > 0 ? arenaLevel : 0;
}

/* How often a name is used in the current function; the C name generator uses the count
 * to decide whether a rename is needed. */
typedef struct { const char *name; int count; } NameUse;

/* One lexical scope: the bindings declared in it. The innermost scope is last. */
typedef struct { Vec syms; } Scope;

/* A call site whose choice of home arena depends on which locals escape.
 *
 * Everything needed to recompute that choice is recorded here, so the pass that runs
 * after all bodies have been checked does not have to look anything up in a scope that
 * is gone by then. */
typedef struct {
    Expr  *call;         /* the EX_CALL / EX_METHOD / EX_ASSOC node; its arenaArg is
                          * rewritten by the later pass */
    char  *argRoot[8];   /* root name of each `mut ref` argument, NULL when that position
                          * is not a `mut ref` argument */
    int    argDepth[8];  /* depth of that argument at the call site; 0 = a parameter or a
                          * global, which already outlives this frame */
    int    n;            /* number of slots in use, at most 8 */
    bool   overflow;     /* more arguments than slots, so the picture is incomplete and
                          * the call is treated as receiving the home arena: conservative
                          * but sound */
} EArenaSite;

/* `stmtNeedsPlaceBoundary` (check_top.c): will codegen bracket this block with a place? The
 * checker's answer is a **conservative superset** -- it must never say "no" for a block codegen
 * does bracket -- and rule 3 of CONCURRENCY.md 12 rests on it (`yield` may not sit inside). */
bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro);

/* `typeContainsProto` (check_lookup.c): is the `coroutine<T>` marker anywhere inside this type?
 * `coroutine<T>` is a return type, not storage (docs/topics/CONCURRENCY.md 4.4). */
bool typeContainsProto(TypeTable *tt, Type *t, const char *name);

/* A `yield` binding: it becomes a frame field, but the frame's Vec only exists after the body is
 * checked, so the name and type are recorded and consumed by the frame layout pass. */
typedef struct {
    struct FuncDef *fn;
    const char    *name;
    Type          *type;
    int            line;
} CoroBind;

typedef struct Checker {
    /* How many enclosing blocks will reclaim their own storage (codegen brackets them with
     * `zoneEnter`/`zoneLeaveTo` + `arena_release`). A `yield` inside one of them is rule 3 of
     * docs/topics/CONCURRENCY.md 12: that block's arena is gone before the coroutine is resumed. */
    int      placeBoundaryDepth;

    Vec      coroDeferred;
    Vec      coroBinds;        /* `var x = yield e` bindings (CoroBind), consumed by coroFrameLay */

    Ctx       *ctx;         /* parser and module context, for diagnostics and lookup */
    Arena     *arena;       /* arena the checker allocates its own bookkeeping from */
    TypeTable *tt;          /* type table of the module being checked */
    Module    *m;           /* the module being checked */
    Vec        scopes;      /* Scope*, innermost last */
    FuncDef   *curFunc;     /* body being checked, or NULL outside a function body */
    Vec       *curParams;   /* names of the type parameters in scope, NULL outside a
                             * generic context */
    Vec        opChecks;    /* OpCheck*: overloadable operators deferred to instantiation */
    Vec        methodChecks; /* MethodCheck*: method calls on a type parameter (`#57`) */
    Vec        deferredUses; /* DeferredUse*: uses of such a call's result, re-checked (`#79`) */
    /* Explicit type arguments written at a call site (`f<i32>(...)`): the node plus the arguments.
     * The `EX_GENCALL` path rewrites itself into an ordinary `EX_CALL` and the inference there used
     * to start from scratch, so the written form was pure decoration -- it worked only where
     * inference happened to succeed anyway. Kept **here** and not as a field on `Expr`: that union
     * is not zeroed for every node kind, so a field only one path sets reads as garbage on the
     * others (docs/topics/HARDENING.md 三点十三). */
    Vec        explicitTargs; /* ExplicitTargs* */
    /* Instances of generic free functions: `fn f<T>` gets one instance per set of type
     * arguments. They are created here, at the call site that infers the arguments, and
     * the code generator emits them as ordinary functions. */
    Vec        funcInsts;   /* FuncDef*, with tmpl/targs/instName filled in */
    Vec        globals;     /* Sym* for module-level bindings (depth 0). They are not in
                             * `scopes`; see the comment in `lookup` */
    /* Every binding the checker has declared, appended by `declare`.
     *
     * The self-check mode walks this list to verify that the `refDepth` of a binding
     * agrees with the allocation site it came from. */
    Vec        allSyms;
    /* The value currently being stored somewhere.
     *
     * `noteFieldDepthWrite` records which value wrote a field, and its callers already
     * hold that value; passing it through a field keeps the signature of that function
     * unchanged, which would otherwise ripple through every call site. */
    Expr      *curStoreVal;
    Vec        nameUses;    /* NameUse*: how often each name is used in the current
                             * function, for the C name generator */
    Vec        moduleNames;     /* const char*, sorted; see check.c for the compile-time
                                 * optimization it enables */
    size_t     moduleNamesSize;
    /* Non-null narrowing for `?ref T`: which bindings are known not to be null at the
     * current position.
     *
     * A nullable reference may not be dereferenced before it has been shown to be
     * non-null, and the only way to show that is a comparison with null inside an `if`.
     * Narrowing is lexical, so it unwinds together with pushScope and popScope, and it
     * generates no runtime check at all: what the compiler can prove leaves no trace in
     * the generated code. */
    /* Non-zero while the current position cannot emit a statement prefix, which makes
     * every `??` that needs one an error. Two positions behave that way: the condition
     * of a `while`, where a hoisted prefix would be evaluated once before the loop
     * instead of once per round, and a global initializer, which has no statement to
     * hang a prefix on. */
    /* Checks deferred out of a generic body.
     *
     * A generic body is checked once, as a template, with `T` opaque, so every rule of
     * the form "only check this when the type carries a reference" returns early and an
     * instantiation with `T = slice<u8>` is never checked at all. The fix is to compute
     * the depth when the rule is recorded, assuming that `T` may carry a reference,
     * while the scope is still open and `lookup` still works; instantiation then only
     * has to answer whether `T` really carries one.
     *
     * Deferring the depth computation itself does not work: the function scope is gone
     * by then, `lookup` cannot find the parameters, the depth comes out as 0 and the
     * check silently stops working. */
    Vec        refChecks;   /* RefCheck*: reference rules re-run at instantiation */
    /* Level-dependent rejections, recorded so the same question can be asked again once
     * the data flow and the level pass have settled the numbers. Reporting stays where it
     * was; the record is what makes the two answers comparable. */
    Vec        lvlRejects;  /* LvlRejection* */
    Vec        callChecks;  /* CallCheck*: call sites resolved at instantiation */
    Vec       *substParams; /* type arguments substituted while an instance is being
                             * re-checked, NULL when not re-checking */
    Vec       *substArgs;
    int        noHoist;     /* non-zero while a statement prefix cannot be emitted */
    /* How many allocation sites have been created so far. A loop condition asks for the
     * difference across its own check to learn whether it allocates at all (`Stmt.condAllocs`),
     * which is what decides between the plain `while (cond)` and the per-round-release
     * shape (定案 101②). */
    int        allocSites;
    /* How many subexpressions with a side effect have already been checked in the
     * current statement.
     *
     * A `??` whose subject is not pure has to be evaluated into a temporary, and that
     * prefix is emitted before the whole statement, which would move it ahead of earlier
     * side effects in the same statement. Once a side effect has been seen and this `??`
     * still needs a temporary, the program is rejected and the user is asked to split
     * the line. Only calls count, because the relative order of allocations, literals
     * and conversions is not observable. */
    int        stmtFx;
    /* Nodes in the current function body whose arena assignment has to be decided again
     * once every effect summary is closed: `new` sites, and the call sites whose
     * allocation arena is still open. After the body has been checked they move to
     * `FuncDef.arenaSites`, and their levels are fixed once the pass that computes
     * `needsHome` has run. */
    Vec        curArenaSites;
    /* Call sites whose choice of home arena depends on which locals escape.
     *
     * That choice has to wait until the effect summaries of all functions are closed,
     * because the rule "pass my home arena when the callee may move the argument out of
     * this function" depends on the set of escaping locals, and computing that set needs
     * the summary of the callee. The summary is not closed while bodies are still being
     * checked, so with an empty summary the call would look like one that never
     * publishes its argument, the home arena would be taken to be the caller's own
     * frame, and everything the callee stored in that container would be released as
     * soon as the call returned. The call sites are recorded here and recomputed by the
     * final pass. */
    Vec        eSites;      /* EArenaSite* */
    /* The table of "this value was stored at level k" requirements.
     *
     * Every entry is handed over by an escape check that was going to run anyway, so no
     * extra traversal is written to find the store sites. The final pass replays them to
     * a fixed point, and the level of an allocation site ends up as the smallest of all
     * the requirements that reached it, since a smaller level lives longer.
     *
     * The fixed point is needed because one requirement only moves the sites it reaches,
     * and once those live longer, further requirements start to reach further sites.
     * This has the same shape as the call-site arena decision and as the transitive
     * closure of `needsHome`. */
    Vec        lvlFacts;    /* LvlFact* */
    /* Publications, appended while the bodies are checked one after another.
     *
     * The vector is **module-wide** (a body's records have to survive the body, because the
     * module-wide requirement replay and the `[store]` dump read them), but every reader
     * asks a question about **one** body, and each record carries the body it came from
     * (`StoreSite.fn`) plus the index that body's records start at (`storeBase`). Folding
     * the whole vector instead was quadratic in the length of a call chain: the level pass
     * of function k re-folded the publications of functions 0..k-1, which also put their
     * bindings into k's level table -- measured on a chain of N functions with one loop
     * each, 4.5e9 table comparisons at N=800, and the front end growing by 4-7x per
     * doubling of N. A reader that wants "the publications of this body" walks
     * `[storeBase, stores.len)` and skips records whose `fn` is not its own (a nested
     * body's records fall inside the slice, hence the second test). The module-wide
     * requirement table, which is deliberately kept across bodies, is `lvlFacts`. */
    Vec        stores;      /* StoreSite* -- publications, folded over by the level pass */
    size_t     storeBase;   /* index in `stores` where the current body's records begin */
    /* ---- `EXTC_DBG_FX=1`: what the level pass and the effect closures cost ----
     *
     * Cheap counters, in the style of the other `EXTC_DBG_*` switches: each one is bumped
     * next to work that already walks a value, so reading them costs nothing that the pass
     * was not already doing, and they are printed once by `checkModule` when the switch is
     * on. They exist because "the front end is quadratic in the length of a call chain"
     * cannot be told apart from "the front end is linear and slow" without knowing which
     * quantity is growing: the counters below separate the records a pass **visits** from
     * the records that are its own, and the table **comparisons** from the calls that make
     * them. `fxOn` caches the switch so the per-element loops never call `dbgOn`. */
    bool       fxOn;          /* `EXTC_DBG_FX`, read once per pass */
    long       fxLvlPasses;   /* level-pass runs (one per checked body) */
    long       fxStoresSeen;  /* store records the level-pass loops looked at */
    long       fxStoresOwn;   /* ... of those, records of the body being solved */
    long       fxTailRecords; /* sum over bodies of the records each body owns */
    long       fxSymCalls;    /* `symLevel` + `setSymLevel` calls */
    long       fxSymCmp;      /* level-table entries compared by those calls */
    long       fxTblSum;      /* sum over passes of the table size at the end */
    long       fxTblMax;      /* largest table a single pass built */
    long       fxLvlRounds1;  /* rounds the "binding level" fixed point ran */
    long       fxLvlRounds2;  /* rounds the "site level" fixed point ran */
    long       fxLvlMoved;    /* bindings whose level really moved */
    long       fxAliasWalks;  /* extra values walked by the join rule of step two */
    long       fxDedupSteps;  /* backward scans of `recordStore`'s duplicate test */
    long       fxPreBodyStores;  /* records made before the first body was walked */
    long       fxAltScan;     /* store records the "every assignment of `dst`" loop looked at */
    long       fxReplayRounds, fxReplayFacts;   /* the module-wide fact replay */
    long       fxEffCalls, fxEffEdges;          /* `computeEffectsTransitive` */
    long       fxEffCached;   /* those calls answered from the memo instead of recursing */
    long       fxReachRounds;                   /* rounds of the `closeReach` driver */
    long       fxReachVisits;                   /* functions a round of `closeReach` considered */
    /* True while requirements are being replayed. Recording a new requirement during a
     * replay would make the table grow without bound, which once ended with the compiler
     * killed by the memory limit. */
    bool       lvlSolving;
    Vec        narrow;      /* const char*: cnames proved to be non-null */
    /* Names of the locals of the current function that can be moved out of it.
     *
     * This only affects which arena is chosen as the home arena, and the conservative
     * direction is to include too many names: that costs memory, while missing one would
     * let a live pointer dangle. */
    Vec        escapees;    /* const char* */
    int        escapeesFor; /* function the set was computed for, -1 when not yet */

    Vec        narrowMarks; /* size_t: length of `narrow` when each scope was entered,
                             * used to unwind it on scope exit */

    /* How many **domains** the checker is inside right now (`sched::run { … }`,
    * `par::pool(n) { … }`). `ext` is legal only when this is non-zero: outside a domain there is
    * nobody to hand the task to, and that is a compile error rather than a runtime surprise. */
    int        domainDepth;
    /* **The nearest domain** (`d.run { … }`), i.e. the receiver expression whose block we are
     * checking right now. The scope rule is "`ext` hands the task to the nearest domain", which is a
     * stack; saving and restoring this one field around a block *is* that stack, with no allocation.
     * NULL means "no domain in scope", and that is a compile error at the `ext` (see check_expr.c). */
    Expr      *curDom;
    /* How many lambda bodies are being checked right now. A lambda inside a lambda would have its
     * environment laid out as part of the outer one; version 1 refuses that instead of laying it
     * out wrongly (docs/topics/LAMBDA.md section 7). */
    int        lamDepth;
    /* How many lambda environments this compilation has generated, for unique names. */
    int        lamSeq;

    Type *tI32, *tI64, *tF64, *tBool;  /* cached primitive types */
    StructDef *sliceDef;/* declaration of `slice<T>` from the prelude, which is the view
                         * protocol */
    Type *tSliceU8;     /* type of a string literal: `slice<u8>` */
} Checker;

/* One reference rule deferred to instantiation. */
typedef struct {
    Expr       *val;     /* the value being checked */
    Expr       *target;  /* non-NULL when the value is being stored into this target */
    int         at;      /* destination depth, computed at record time while the scope
                          * still existed */
    int         depth;   /* depth of the references in the value, computed the same way
                          * and assuming `T` may carry a reference */
    bool        borrowed;/* whether the value is borrowed, computed the same way */
    /* A second kind of deferred rule: the zero value.
     *
     * `var local: T` with no initializer looks harmless while `T` is opaque, but the
     * instantiation `T = slice<u8>` produces a null reference, and the language promises
     * that a `ref` is never null. */
    bool        isZero;
    /* A third kind: the size in `new T[n]`.
     *
     * While a body is a template, `T` has no size, so the check can neither pass nor
     * fail; it is recorded and then run for every concrete instantiation. */
    bool        isNewSize;
    Type       *declType;/* declared type the deferred rule applies to */
    int         line;    /* source line to report */
    const char *what;    /* short noun phrase naming the operation, for the diagnostic */
    FuncDef    *func;    /* the generic function this rule belongs to */
} RefCheck;

/* The requirement "this value has to fit into level `at`".
 *
 * It is kept as a fact rather than as a decision because a smaller level means a longer
 * life, so the final level of an allocation site is the smallest of all the
 * requirements that reached it, while which requirements reach it depends on the level
 * the site currently has. Replaying to a fixed point is therefore necessary. Deciding a
 * level as soon as a `new` is seen allows movement in one direction only and makes it
 * impossible to tell afterwards whether a level was decided or computed. */
typedef struct { Expr *val; int at; } LvlFact;

/* One publication: a value observed being handed to a place that may outlive it.
 *
 * Every store, return, and "may keep it" argument call is one of these. Recording is
 * the checking pass's entire involvement with arena levels; deciding which arena each
 * allocation site ends up in is a separate pass that folds over this table. Keeping the
 * two apart is what makes the decision independent of the order the checker walks the
 * tree in, which is the property the previous destructive update could not have.
 *
 * Params are captured rather than derived later, because `at` needs the scope stack and
 * because the binding's origin may legitimately be retargeted later in the body. */
typedef struct {
    Expr       *value;   /* the value being published */
    Expr       *target;  /* the place it is published into; for a return, the returned value */
    int         at;      /* arena level of the destination; 0 = beyond this frame */
    int         line;    /* for diagnostics */
    /* The body this publication came out of (`Checker.curFunc` at the time), or NULL for one
     * recorded outside every body.
     *
     * The record table is per module, because it is filled while the functions are checked
     * one after another, but every reader of it asks a question about **one** body: the
     * level pass folds the publications "recorded while the body was checked", and the
     * extent of a walk is a property of the body's own expressions and bindings. The owner
     * is what lets a reader say which records are its own -- without it, the level pass of
     * every function re-folded every earlier function's publications, which also entered
     * the bindings of those bodies into the level table of a body that cannot reach them
     * (measured: 4.5e9 table comparisons on the N=800 chain, O(N^2) for what is O(1) per
     * body). A record made outside every body has no owner and is never folded. */
    FuncDef    *fn;
} StoreSite;

/* A rejection that depends on a level, recorded so it can be re-judged against the
 * numbers that are final rather than the ones that happened to hold while the body was
 * being walked. */
typedef struct {
    Expr       *val;    /* the value that was rejected */
    int         at;     /* the depth it was judged against */
    int         depth;  /* the depth the check saw */
    int         line;   /* source line */
    int         late;   /* depth the settled numbers gave */
} LvlRejection;

/* An operator use on a type parameter, deferred to instantiation: recorded while an
 * expression is checked and consumed by the pass that runs afterwards.
 *
 * `==` / `!=` were the first family to need this; the ordering and arithmetic operators
 * use the same record: on the template `T` is opaque, so whether it
 * supports the operator is only answerable per instance. `op` carries which operator it
 * was, and the consumer re-asks the same predicate the concrete-type path uses.
 *
 * `func` is the function the expression belongs to, which is how the consumer asks
 * whether that function was ever called. */
typedef struct { Expr *node; StructDef *owner; const char *op; FuncDef *func; } OpCheck;

/* A method call on a type parameter, deferred to instantiation (`#57`).
 *
 * While a template is checked `T` is opaque, so `k.hash()` has no method to resolve: which
 * method it means is decided by whatever the instance substitutes for `K`. Recording it here
 * and resolving per instance is also the only place a good diagnostic can come from -- only
 * the instance knows whether its type defines the method at all.
 *
 * The template pass hands back the error type for such a call. That is what the error type is
 * for (`ast.h`: "dummy type for a failed check, so errors do not cascade"): the statement keeps
 * checking and `checkAssignable` waves an error type through, while the instance pass is the one
 * that decides -- and it names the instance, exactly as `OpCheck` does.
 *
 * `nargs` is the number of arguments written at the call, so the instance pass can report an
 * arity mismatch instead of letting it reach the C compiler. `func` is the template holding the
 * call, which is how the driver picks the instances this record applies to. */
typedef struct {
    Expr       *node;     /* the EX_METHOD node */
    StructDef  *owner;    /* enclosing generic struct/enum, or NULL inside a free function */
    const char *name;     /* the method name being called */
    size_t      nargs;    /* arguments written at the call site */
    FuncDef    *func;     /* the template function the call sits in */
} MethodCheck;

/* A call written `f<i32>(...)`: what the rewrite to `EX_CALL` must hand to inference. */
typedef struct {
    Expr *node;      /* the call node the arguments belong to */
    Vec   targs;     /* Type*: the written arguments, in declaration order */
} ExplicitTargs;

/* A use of a deferred method call's RESULT, re-checked at instantiation (`#79`).
 *
 * On the template, `k.hash()` hands back the error type (`MethodCheck` explains why), and
 * `checkAssignable` waves an error type through -- which is what keeps the template pass from
 * inventing errors. But "no error here" must not be the last word: `fn f<T>(x: T) -> i8 { return
 * x.hash() }` compiled and SILENTLY TRUNCATED when the instance's `hash` returned `i64`, where the
 * same code written on a concrete type is rejected as a lossy conversion.
 *
 * The expected type IS known at the use site, so it is recorded here and checked again per
 * instance, once the real signature is resolvable. `want` is that expected type; `call` is the
 * deferred call the value came from. */
typedef struct {
    Expr      *call;      /* the expression the value came from: a method call deferred to
                           * instantiation, or any expression whose type is a **type parameter**
                           * (the two shapes `checkAssignable` records below) */
    Type      *want;      /* the type the context expects */
    StructDef *owner;     /* enclosing generic struct/enum, or NULL in a free function */
    FuncDef   *func;      /* the template holding the use, which picks the instances */
} DeferredUse;

/* A call site whose resolution is deferred to instantiation.
 *
 * In `fn twice<T>(x: T) -> T { return idOf(x) }` the template is checked with `T`
 * opaque, so the type argument of `idOf` is `T` itself and no correct instance can be
 * built at that moment: the only one available would be `idOf_T`, an instance that
 * still takes a type parameter. This is the same family of problem as `RefCheck` and
 * `OpCheck` and is solved the same way: record it while the template is checked and
 * resolve it again for each instantiation.
 *
 * Skipping the checks that follow is not an option, because the rest of the template
 * body uses the type of `e->func` to check arguments and to compute the return type, so
 * some signature has to be available. A provisional instance with `T` as its argument
 * is therefore created, which is self-consistent because `T` is used as an opaque type,
 * and instantiation later points `e->func` at the concrete instance.
 *
 * The difference from `RefCheck` is that this rewrites `e->func` in the AST, so it has
 * to run in the context of the instance being re-checked. That context already exists,
 * because the re-check pass sets `c.substParams` and `c.substArgs`. */
typedef struct {
    Expr    *node;      /* the EX_CALL / EX_METHOD / EX_ASSOC node */
    FuncDef *tmpl;      /* the template being called: a free function or a method of a
                         * generic type */
    Vec      targs;     /* arguments inferred while the template was checked; they may
                         * contain `T` and have to be passed through `tsub` */
    FuncDef *func;      /* the template function this entry belongs to, which decides
                         * the instances it applies to */
} CallCheck;

/* -------------------------------------------------- cross-file prototypes
 *
 * Generated by running `gcc -aux-info` over the single check.c that these files were
 * split from and taking each signature verbatim, with `static` removed: writing sixty
 * signatures by hand would only introduce typos. */

/* Build an integer literal node; the compiler uses it to fill in a slice bound the user omitted. */
 Expr *intLit (Checker *c, long long int v, int line);
/* Find a field by name in a struct definition, or NULL. */
 FieldDef *findField (StructDef *sd, const char *name);
/* Find a module-level function by name; template instances do not match, templates do. */
 FuncDef *findFunc (Checker *c, const char *name);
/* Find a method declared in the body of a struct or generic type. */
 FuncDef *findMethod (Type *st, const char *name);
/* Find the operator method for `sym`, falling back to `fallback` (`!=` falls back to `==`). */
 FuncDef *findOp (TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);
/* The operator method of a type that takes exactly `rhs` on the right (see the
 * definition for why operators are the one overloadable name). */
 FuncDef *findOperator (TypeTable *tt, Type *st, const char *name, Type *rhs);
/* The StructDef behind a TY_STRUCT or TY_GENERIC type, or NULL. */
 StructDef *structOf (Type *t);
void warnSharedCopy(Checker *c, Expr *e, Type *t);
void warnSharedReturn(Checker *c, Expr *e, Type *t);
/* Declare a binding in the innermost scope and return it; the caller writes the generated C name
 * back into the AST. Unless `shadow` is set, a name already declared in the same scope is an
 * error, because two `var`s of the same name in one scope are almost always a typo. */
 Sym *declare (Checker *c, const char *name, Type *t, _Bool mut, _Bool shadow, int line, int depth);
/* Resolve a name to a binding, innermost scope first and module-level bindings last. */
 Sym *lookup (Checker *c, const char *name);
/* The binding at the root of a place, found by walking through fields, indexes and slices; NULL
 * when the place is not rooted in a binding. */
/* ---------------------------------------------------------------- 深度 / 身份：四个问题
 *
 * 这个检查器里"这个值活多深 / 它是谁"被问了一百多次，曾经由**三套各写一遍的根遍历**回答，
 * 差异（走不走 `SLICE`/`SIGN`、跳数上限、取名字还是取符号）只藏在三份循环里 —— R1/R3 批次里
 * 5 条 bug 正是发生在两个入口"读数不一致"的地方（P0-1/P0-2/P0-6c/P0-6d/P0-16）。
 *
 * 从 U2 起，这里就是唯一权威：**问哪件事，用哪个函数，极性是什么**。
 *
 *   ① 存储有多深？（`a`、`a.f`、`a[i]` 这些**地方**）      → `placeDepth`
 *   ② 值指向的东西有多深？（`p`、`f(x)` 这些**值**）        → `exprRefDepth`
 *   ③ 这个根的**绑定**是谁？（要身份，不要拼写）             → `placeRoot`（查作用域）
 *                                                           → `rootSymNoScope`/`identBindOf`（只看节点）
 *   ④ 这个根的**名字**？（诊断与老接口）                     → `placeRootName`
 *
 * **极性契约（最重要的一条）**：①② 返回的是**深度**，`0` = "活得最久"（全局、参数所指、
 * 调用者选的地方）；**数越小活得越久**。因此任何"认不出来"的兜底都必须落在**更长寿**那一侧，
 * 或者干脆拒绝 —— 反过来（兜底给 0）就是把保守方向搞反，P0-6c 就是这么来的。
 *
 * 一个事实一个指称：**新代码不要**再写第四份"从表达式取根"的循环；需要别的步进组合时，
 * 把 `EXTC_ROOT_*` 掩码传给 `extcRootLeaf`。历史上三个入口的差异就是下面这些掩码：
 *
 *   | 入口 | 步进 | 跳数 | `EX_IDENT` 怎么解 |
 *   |---|---|---|---|
 *   | `placeRoot`      | FIELD INDEX SLICE | 不限 | 查作用域（`lookup`） |
 *   | `placeRootName`  | FIELD INDEX DEREF | 不限 | 取**语法名字**（不解析） |
 *   | `rootSymNoScope` | FIELD INDEX DEREF SIGN | 16 | 取节点上的绑定（`identBindOf`） |*/
#define EXTC_ROOT_FIELD  (1u << 0)
#define EXTC_ROOT_INDEX  (1u << 1)
#define EXTC_ROOT_SLICE  (1u << 2)
#define EXTC_ROOT_DEREF  (1u << 3)
#define EXTC_ROOT_SIGN   (1u << 4)

/* 沿掩码允许的步进走到根**标识符节点**（不解析、不查作用域）；`maxHops <= 0` 表示不限跳数。
 * 返回的节点保证是 `EX_IDENT`，由调用者决定要名字还是要绑定。*/
Expr *extcRootLeaf(Expr *e, unsigned steps, int maxHops);
 Sym *placeRoot (Checker *c, Expr *e);
/* Check an expression in a place position and return its type; a reference is not dereferenced. */
 Type *checkExpr (Checker *c, Expr *e);
/* Check an expression against an expected type; a reference is dereferenced unless the expected
 * type is itself a reference. */
 Type *checkInto (Checker *c, Type *want, Expr *e);
/* Check an expression that may be `e?`, which is legal in statement positions. */
 Type *checkMaybeTry (Checker *c, Expr *e);
 bool rejectStreamBorrow (Checker *c, Expr *e);
 bool isStreamBorrow (Expr *e);
/* Check an argument of `print` or `println`, which dereferences a reference because printing an
 * address is never what the user means. */
 Type *checkPrintArg (Checker *c, Expr *e);
/* Check `e?`: the operand must be an `option` or a `result`, and the enclosing function must
 * return the same kind with the same error type. */
 Type *checkTryInner (Checker *c, Expr *e);
/* Check an expression in a value position; a reference here is an error whose message asks for
 * an explicit `*p`. */
 Type *checkValue (Checker *c, Expr *e);
/* The type of the k-th payload of a variant, substituted for the type arguments when the enum is
 * an instance of a generic type. */
 Type *payloadType (TypeTable *tt, Type *et, Variant *v, size_t k);
/* Build the type `slice<elem>` from the declaration in the prelude. */
 Type *sliceOf (Checker *c, Type *elem);
/* Substitute the type arguments of the instance being re-checked into `t`; a type that mentions
 * no parameter comes back unchanged. */
 Type *tsub (Checker *c, Type *t);
/* The element type of a view, or NULL when the type is not a view. */
 Type *viewElemOf (Type *t);
/* Find a variant of an enum by name, or NULL. */
 Variant *findVariant (TypeDef *td, const char *name);
/* Read an expression as a literal integer, accepting a negated literal, so that a negative slice
 * bound is rejected at compile time. */
 _Bool asIntLit (Expr *e, long long int *out);
/* True when a statement always leaves its enclosing block (return, break or continue); it is how
 * the `if p == null { return }` guard is recognised. */
 _Bool blockExits (Stmt *s);
/* Report an error when a value of type `got` cannot be assigned where `want` is expected;
 * returns true when the assignment is rejected. */
 _Bool checkAssignable (Checker *c, Type *want, Type *got, Expr *node, const char *what);
/* Report an error when a value would outlive the place it is being stored into. */
 _Bool checkEscape (Checker *c, Expr *val, int at, int line, const char *what);
/* Run every check a store needs: the depth rule plus "a borrowed value may not be stored where
 * it outlives the call". */
 _Bool checkStoreEscape (Checker *c, Expr *val, Expr *target, int line);

/* Record that `val` is being published at level `at`; decides nothing. The checking
 * pass only records; one pass folds over the records and settles the levels. */
 void recordStore (Checker *c, Expr *val, Expr *target, int at, int line);
 void recordLvlRejection (Checker *c, Expr *val, int at, int depth, int line);
 void recheckLevelRejections (Checker *c);
 bool promoteFieldsAt (Checker *c, Sym *sy, int at, int hops);
/* True when C can compare values of this type directly; `str` is excluded because its `==` would
 * compare pointers. */
 _Bool cmpIsNative (Type *t);
/* True when at least one variant carries a payload; such an enum is a tagged struct in C and
 * cannot be compared with `==`. */
 _Bool enumHasPayload (TypeDef *td);
/* True for `==`, `!=`, `<`, `<=`, `>` and `>=`. */
 _Bool isCmpOp (const char *op);
/* True for `==` and `!=`, the pair that shares a native rule and the `!=` -> `==` fallback. */
 _Bool isEqualityOp (const char *op);
/* True for every name a type may define more than once: the comparison, arithmetic and
 * stream operators. */
 _Bool isOverloadableOp (const char *name);
/* True for `+`, `-`, `*`, `/` and `%`, the arithmetic operators that can be overloaded. */
 _Bool isArithOp (const char *op);
/* True for `&&` and `||`. */
 _Bool isLogicOp (const char *op);
/* True when an address can be taken of this expression: a binding, a field or a dereference. */
 _Bool isLvalue (Expr *e);
/* True when this generated name has been proved non-null at the current position. */
 _Bool isNarrowed (Checker *c, const char *cname);
/* True for an integer or a float literal. */
 _Bool isNumericLit (Expr *e);
/* True when the expression is a place, that is a chain of bindings, fields, indexes and slices.
 * Slicing a fixed array requires one, because the address of an element is taken. */
 _Bool isPlace (Expr *e);
/* True when a value of this type can be printed; structs, enums, arrays and views are printed by
 * a generated helper that recurses into them. */
 _Bool isPrintable (Type *t);
/* True when the type is the named container from the prelude with exactly `nargs` arguments,
 * such as `option<T>` or `result<T, E>`. */
 _Bool isProtoType (Type *t, const char *name, size_t nargs);
/* True when this place can be written: its root is `var` and no read-only reference or read-only
 * view is crossed on the way. */
 _Bool isWritablePlace (Checker *c, Expr *e);
/* True when a literal's type can adapt to `want`, because a literal takes its type from the
 * target rather than the other way round. */
 _Bool literalFits (Expr *e, Type *want);
/* True when the type mentions a type parameter, directly or inside an aggregate; such a type
 * cannot be decided before instantiation. */
 _Bool mentionsParam (Type *t);
/* Report whether every reference crossed on the way to this place is a `mut ref`, and set
 * `*crossed` when the storage lives behind such a reference. */
 _Bool pathRefsAllMut (Expr *e, _Bool *crossed);
/* True when the path to this place crosses a read-only reference, including the type of the
 * place itself. */
 _Bool pathHasReadonlyRef (Expr *e);
/* Report an error and return true when a nullable reference is dereferenced without a preceding
 * proof that it is not null. */
 _Bool rejectNullableDeref (Checker *c, Type *t, Expr *node, const char *what);
/* True when evaluating this expression twice is observably the same; `??` needs that, because
 * its subject appears twice in the generated C. */
 _Bool repeatablePure (Expr *e);
/* Report an error when writing to this place is not allowed; returns true when the write is
 * rejected. */
 _Bool requireMutable (Checker *c, Expr *e, int line, const char *what);
/* True when the type can carry a reference, directly or inside an array, a struct, or a payload
 * of any variant. */
 _Bool typeContainsRef (TypeTable *tt, Type *t);
/* True when the type is a function type (`fn(A) -> R`) or contains one inside an array, a struct, or
 * a payload of any variant. Asked only to word one diagnostic correctly: a `fn` has no zero value,
 * but the reason is not a reference. */
 _Bool typeContainsFn (TypeTable *tt, Type *t);
/* True when the type has no zero value, which holds for a non-nullable reference, for a function
 * type (whose zero value would be a null code pointer), and for any aggregate that contains one. A
 * payload-carrying enum looks only at its first variant, because its zero value is tag 0 with the
 * payload zeroed. */
 _Bool typeLacksZeroValue (TypeTable *tt, Type *t);
/* True when values of this type can be compared with `op`: the comparison is native, or the type
 * is an array of a comparable element, or it defines the operator method. */
 _Bool typeSupportsOp (TypeTable *tt, Type *t, const char *op, Type *rhs);
/* The name used in generated C for a source name, renamed when it would collide with a binding
 * of the same name in the same scope. */
 const char *cNameFor (Checker *c, const char *name);
/* The name a condition proves non-null, or NULL; `*whenTrue` says whether the proof holds on the
 * taken branch. */
 const char *narrowTarget (Checker *c, Expr *cond, _Bool *whenTrue);
/* Render a type as source text, for a diagnostic. */
 const char *typeStr (Checker *c, Type *t);
/* Check a whole module: the declarations, the function bodies, the deferred rules, and the arena
 * decisions that wait for closed effect summaries. */
/* True when this local of the current function can be moved out of it. */
 bool isEscapeeName (Checker *, const char *);
/* Compute the effect summary of a function, following the callees, and report whether the
 * summary is complete; only a complete summary may be used to skip a check. */
 bool computeEffectsTransitive (Checker *, FuncDef *);
/* The field-table entry for a field, created on demand when `create` is set, or NULL when there
 * is none. */
 int *fieldDepthEntry (Checker *, Sym *, const char *, bool);
/* Record that a value of depth `d2` was stored into a field, updating the field entry and the
 * effective depth of the root. */
 void noteFieldDepthWrite (Checker *, Sym *, const char *, int);
/* Record a whole-value assignment into a binding. When the source has a complete field table,
 * copy that table so later promotion can still reach each field's allocation site. */
 void noteWholeValueDepthWrite (Checker *, Sym *, Expr *, int);
/* The name of the binding at the root of a place, or NULL when the place is not rooted in one. */
 const char *placeRootName (Expr *);
/* The depth of the home arena to pass at this call site, derived from the arguments and the
 * parameters of the callee. */
  int callHomeDepth (Checker *c, Vec *args, Vec *params, Expr *callNode);
/* Depth to record for a value that is being stored, taking the references and the structure of
 * the value into account. */
int valDepthForStore (Checker *, Expr *);
/* Depth of the references a value can carry, read off its type and its shape. */
 int exprRefDepth (Checker *c, Expr *e);
/* Depth of the storage a place expression denotes, as opposed to the depth of what a reference
 * stored there points at. */
 int placeDepth (Checker *c, Expr *e);
/* Does this expression's storage provably live outside every frame? (`effects Ret=0` calls, and
 * chains rooted at a global.) Used where a binding is initialized, to mark it (`Sym.outOfFrame`). */
 bool exprOutOfFrame (Checker *c, Expr *e);
/* Arena level at which the storage of a place lives, which is a different question from
 * `placeDepth`. */
 int storeLayer (Checker *c, Expr *e);
/* True when the root of the value is one of the function's parameters, so that the call site can
 * decide its lifetime. */
 _Bool valTracesToParam (Checker *c, FuncDef *f, Expr *val);
/* Raise every allocation inside a value to the level it is being stored at, instead of rejecting
 * the store. */
 _Bool promoteInto (Checker *c, Expr *val, int at);
/* Remember where a binding's value came from, flattened to the root of the chain. */
 void noteOrigin (Checker *c, Sym *sy, Expr *val);
/* Give a bare `{}` or a bare constructor the type the surrounding context expects. */
 void adoptContextType (Expr *e, Type *want);
/* Check the `ref` and `mut ref` arguments of a call: each has to outlive the call, and the arena
 * argument is decided here. */
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
/* Check the signature of an operator method at the place where it is defined. */
 void checkOperatorSig (Checker *c, FuncDef *f);
/* Check one statement and apply the depth and narrowing consequences it has. */
 void checkStmt (Checker *c, Stmt *s);
/* Resolve a signature's type names into the type table (in place). A synthesized function -- a
 * lambda's `call` method -- has to go through the same step a declared one does, or its parameter
 * types are the parser's raw nodes and `i64 + i64` stops type checking (measured). */
 void resolveSignature (Checker *c, FuncDef *f);
/* Report a compile error at a line, with an optional note suggesting a fix. */
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
/* Report a warning at a line. A warning is recorded but does not stop the compilation. */
 void ckWarn  (Checker *c, int line, const char *note, const char *fmt, ...);
/* Resolve the operator of a compound assignment; returns the type of `target op value`. */
 Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                       Type *tgt, Type *val, Expr **out);
/* Rewrite a bare constructor into the qualified variant construction. */
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
/* Report an error unless the type is `bool`; the language has no implicit truthiness. */
 void expectBool (Checker *c, Type *t, Expr *node);

/* Mark the call as allocating into the caller's home arena when the value it produces escapes
 * the current frame. */
/* ---------------------------------------------------------------- 写点：一个入口（U3）
 *
 * 一次写要同时做四件事，**顺序不能变**：
 *   ① `recordStore`  —— 记下"这个值被发布到了 `at` 这一层"（电平求解器据此折叠）
 *   ② `promoteInto`  —— 试着把值背后的分配站点提到那一层（提不动就保持保守）
 *   ③ `markCallHomeIfEscaping` —— 逃逸集合相关的那一半
 *   ④ 字段表          —— 整值写走 `noteWholeValueDepthWrite`，字段/元素写走 `noteFieldDepthWrite`
 *                        （后者内部会记 `src`，让后续提权找得到站点）
 *
 * ①②③④ 过去散在各站点手写（7 种入口、30 处调用），于是"少写一样"就是一类洞 ——
 * P0-16（出参写不发布）与 P0-17/P1-5（事实不失效）都长在这种接缝上。
 * 从 U3 起，**能走这个入口的写一律走它**；只有语义确实不同的（返回值、实参发布、
 * 证明失效 `unNarrow`、fresh 集合失效 `freshDrop`）才自己组合，并在调用点写明理由。*/
 void noteStore (Checker *c, Expr *value, Expr *target, int at, int line);
/* 返回值那一种发布：`recordStore(v, v, 0)` —— "交给调用方的东西按层 0 发布"。两个返回站点
 * （`e?` 分支与正常路径）共享这一半，各自的后续步骤不同（一个接 `promoteInto`+`recordLvlFact`，
 * 一个接 `checkEscape`），所以只把共享的这半收成一个名字。*/
 void noteReturnPublish (Checker *c, Expr *value, int line);
 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
/* Resolve the home depth of a call site into the arena argument the code generator emits. */
 void setCallArenaArg (Checker *c, Expr *e);
 void setCallZoneArg (Checker *c, Expr *e);
 bool calleeMakesPool (FuncDef *f);
 /* 运行期**建池**的那一族按名字认（extern 没有函数体，效果推断看不穿它）。规则**只有这一处**：
 *   · `calleeMakesPool` / `calleeCreatesPool` 问"这一次调用会不会建池" ⇒ 全族；
 *   · codegen 的改写点只给**不带 zone 参数**的那两个补 home zone（`_at` 自己带着）。
 * 漏登记一个名字的后果（实测）：`extc_pool_new_table` 拿不到 zone ⇒
 * `extc_pool_new_at(parent, extc_zoneTop)` 撞上 `extc_zoneTop == -1` ⇒ **返回 -1**，
 * 表现成"对象表池作为程序里第一只池时创建失败"。判据：`tests/pool/rt_table_first.extc`。 */
static inline bool isPoolCtorName(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_at") == 0 ||
                 strcmp(n, "extc_pool_new_table") == 0);
}
static inline bool poolCtorNeedsZone(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_table") == 0);
}

/* The six pool primitives: `check_expr.c`'s `checkPoolPrim` validates their arity, `codegen.c`
 * lowers them by hand. **One list, one spelling** -- the two files used to write the six names out
 * separately (review F11), so a name added on one side would leave the other reading arguments it
 * never checked and re-deriving an arity of its own. */
static inline bool isPoolPrimitiveName(const char *n) {
    return n && (strcmp(n, "poolSlice") == 0 || strcmp(n, "poolSliceRaw") == 0 ||
                 strcmp(n, "poolResize") == 0 || strcmp(n, "poolResizeRaw") == 0 ||
                 strcmp(n, "poolGive") == 0 || strcmp(n, "copyInto") == 0);
}

bool calleeCreatesPool (FuncDef *f);
 void setPoolCalleeResolver (FuncDef *(*fn)(Expr *e));
 void recordLvlFact (Checker *c, Expr *val, int at);
/* Report an error when a name of another module is used without its module qualifier. */
 void checkCtorSig (Checker *c, FuncDef *f);
 void requireQualified (Checker *c, const char *what, const char *whatMod, bool qualified, int line);
/* Did an import bring this name into scope? `use mod::*` or `use mod::{name}` (check_lookup.c) */
 bool nameBrought (Module *m, const char *importer, const char *opened, const char *what);
/* The type parameters visible in this function: those of the function itself, or those of the
 * generic type that owns it. */
 Vec *funcTParams (FuncDef *f);
/* The instance of a generic function for one set of type arguments, created on first use. */
 FuncDef *funcInstance (Checker *c, FuncDef *tmpl, Vec *targs, int line);
/* Fill in the type arguments of a generic call by unifying the parameter types with the argument
 * types; false means they cannot be inferred and have to be written out. */
 _Bool unifyTParams (TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got);
/* Record every non-null fact a condition proves, including both sides of an `&&`. */
 void narrowFactsOf (Checker *c, Expr *cond);
/* Leave the innermost scope: drop its bindings and unwind the narrowing facts it established. */
 void popScope (Checker *c);
/* Record that a binding has been proved non-null at the current position. */
 void pushNarrow (Checker *c, const char *cname);
/* Enter a new lexical scope; the bindings declared in it disappear at the matching `popScope`. */
 void pushScope (Checker *c);
/* Defer the size check of `new T[n]` to instantiation, for a body whose type parameter has no
 * size yet. */
 void recordNewSizeCheck (Checker *c, Type *t, int line);
/* Defer the zero-value check of a declaration to instantiation. */
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
/* Drop the non-null proof of a binding, and of the paths that begin with it, which an assignment
 * invalidates. */
 void unNarrow (Checker *c, const char *cname);
/* Can this statement, directly or through the functions it calls, create a pool (`extc_pool_new`)?
 *
 * The question belongs to the checker because it reads `e->func`, which checking resolves, and
 * because the answer is transitive: `FuncDef.makesPool` is its least fixed point over the call
 * graph. Codegen asks the same question one block at a time, with `descendBlocks` false, to
 * decide whether a block needs a pool zone of its own.
 *
 * `descendBlocks` false stops at a nested block: a block is a `place` of its own, and that block
 * emits (or declines) its own hook. */
 bool stmtMakesPool (Stmt *s, bool descendBlocks);

#endif /* EXTC_CHECK_INTERNAL_H */
