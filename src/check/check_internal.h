/**
 * @file check_internal.h
 * @brief Checker-internal header: state shared by the `check_*.c` files, and cross-file prototypes.
 *
 * The only public entry point is `checkModule` in `check.h`. This header holds types and
 * prototypes only; no logic lives here.
 */

#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

#include "check.h"
#include "typelayer.h"   /* the read-only type questions, shared with codegen */

/* The compilation plan does not live on this AST: storage and accessors are on the plan side
 * (`plan.h` accessors/setters plus per-node side tables), and codegen reads it only through those
 * accessors. A seam check enforces that codegen never reads a plan field directly, that no
 * moved field reappears on the AST, and that plan write-backs stay at their ceiling.
 * Held by the plan: arenaLevel/zoneLevel/arenaArg/needTemp, usesHome/mayUseArena/makesPool/
 * condAllocs, isCoro/yieldType/coroFrameType/coroNeedsZone/coroProto. Still on the AST:
 * Expr.func, FuncDef.tmpl, instName, coroKind, Stmt.forStep (half syntax: the parser and the
 * checker each own half). */
/* The checker phase. Defined here because `Checker` stores it (contract in the "phase contract"
 * section below). */
typedef enum {
    PHASE_INIT = 0,      /* before anything starts: no tables yet */
    PHASE_DECLS,         /* ckB0-ckB5: declarations, signatures, C-ABI checks */
    PHASE_BODIES,        /* ckB6-ckB7: function bodies (scopes exist) */
    PHASE_POST           /* ckB8 onward: summaries, levels, replay, recheck (**no scopes**) */
} CheckPhase;

/**
 * @brief Short name of a check phase.
 *
 * @param p  Phase value.
 * @return   Name of the phase (`init`/`decls`/`bodies`/`post`), or `"?"` when the value is not a
 *           phase.
 */
static inline const char *phaseName(CheckPhase p) {
    switch (p) {
    case PHASE_INIT:   return "init";
    case PHASE_DECLS:  return "decls";
    case PHASE_BODIES: return "bodies";
    case PHASE_POST:   return "post";
    }
    return "?";
}

/* Display name of a declaration for diagnostics: `DN(x)` takes a `StructDef *` or a
 * `TypeDef *`.
 *
 * After module mangling `name` is the internal `io$reader`, while `srcName` is the
 * `io::reader` the user wrote. Printing `->name` leaks the internal encoding into the
 * message, as in "struct `alpha$pair` has no field `zzz`".
 */
#define DN(d) ttDispName((d) ? (d)->srcName : NULL, (d) ? (d)->name : NULL)

/**
 * @brief Recover the user-written form of a rewritten function name, for diagnostics.
 *
 * The result may point at a static buffer that the next call overwrites. Returns `"?"` when
 * `name` is NULL; returns `name` unchanged when the module name is empty (root module) or the
 * name carries no `$` prefix.
 *
 * `FN(f)` is its diagnostic alias: `FN(f)` expands to `checkFnDisplay(f->name, f->modName)`.
 *
 * @param name     Rewritten name; may be NULL.
 * @param modName  Module prefix; empty or NULL in the root module.
 * @return         Display name of the form `mod::fn`; `name` when it does not fit the buffer.
 */
const char *checkFnDisplay(const char *name, const char *modName);
#define FN(f) checkFnDisplay((f) ? (f)->name : NULL, (f) ? (f)->modName : NULL)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------ shared state and shared types
 * Whatever more than one file needs, so that whoever uses it can see it. */

/* A reference-field table that grows on demand: a binding may have any number of reference fields
 * (large structs, projections such as `{ mid: { next } }`). A fixed-size table would spill to the
 * shared `otherDepth`/`otherSrc` slots, which is coarser: still conservative, but it **changes
 * promotion decisions**. */
typedef struct SymField {
    const char *name;       /* interned in the AST, so it cannot dangle */
    int         depth;      /* recorded depth of the value in this field */
    /* Which value was last written into this field, or NULL when unknown. Used only to find the
     * allocation site inside a container, never to change bookkeeping: `var h: box = { p: null,
     * q: null }` has an all-null origin that never mentions the `x` a later `h.q = x` stored. */
    Expr       *src;
    /* The **binding** behind the same source as `src` (set when the source is a binding). Flattening
 * `src` to the allocation site lets the last replay round run without `lookup`, but flattening
 * drops the intermediate bindings' field tables (`b.next = ca` is recorded on `b`), and promotion
 * along cycles/chains must reach them. The Sym pointer lives in the checker arena, so it stays
 * usable after its scope ends. */
    struct Sym *srcSym;
    /* Depth of the write that produced `src`. Compare against this, not `depth`: `depth` keeps the
 * maximum on a weak update, so after `h.q = x  h.q = null` it is still deep; comparing against it
 * would treat the null write as deeper, clear the source, and break the chain. */
    int         srcDepth;
    /* Shallowest level this field is required to reach; 0 = must outlive this frame (`out = h` writes
 * at level 1, but `return out` requires it to outlive the frame, so the site must end up in the
 * home arena; a level computed from the current destination alone is too shallow and leaves a
 * use-after-free). */
    int         minReq;
} SymField;

typedef struct Sym {
    const char *name;    /* name as written in the source */
    const char *cname;   /* name used in generated C; a shadowed `a` becomes `a__2` */
    Type       *type;    /* declared type, or the inferred one for an unannotated `let` */
    bool        mut;     /* true for `var`; false for `let` and for payload bindings */
    bool        declConst; /* a module-level `const` (immutable *and* promised at parse time) */
    /* Line that moved this binding's value out, or 0 while the name still holds one. Only
     * `@noCopy` values move (a copyable one is copied instead). Using a moved name is an error:
     * the value lives in the destination and the source name owns nothing. */
    int         movedLine;
    /* Line of an explicit `drop x` that already ran this place's hook, or 0. Like a move, the name
     * survives as a place a program may assign to again, but reading it is a compile error. */
    int         droppedLine;
    const char *movedInto;  /* destination binding, for the diagnostic */
    /* True once the address of this binding has been handed out: `ref x`, `x[..]`, or
     * passing it as a `mut ref` argument.
     *
     * With a taken address an alias can change the contents behind our back, so the
     * depth bookkeeping may only be updated weakly. Without one an entry is overwritten
     * outright, which removes the false rejections where writing null or a shallower
     * value could not lower the recorded depth. */
    bool        addressed;
    /* True when this binding holds a view whose storage this frame owns: `new T[n]` was taken
     * for it here, or it is a slice of such a view. Only such a view may be indexed for a
     * reference, because a plate behind a container can be resized out from under the view.
     * Recomputed on every assignment to the binding. */
    bool        frameStore;
    /* Per-field depths, at most four entries. To be read as one field block, not as
     * four independent ones.
     *
     * A field such as `h.p` gets a depth of its own instead of sharing one number with
     * `h`, so `h.p = null` can really overwrite that entry. Writes that cannot be
     * attributed to a single field, such as an element write, go to `otherDepth`. The
     * effective depth of the root is the maximum of `otherDepth` and the field entries. */
    SymField   *fields;     /* grown on demand in the checker's arena; no fixed cap */
    int         fieldsCap;
    /* Whether the field table is complete.
     *
     * Complete means every field of the binding has an entry, or is known to be 0; only
     * then may an entry be cleared and the root recomputed as the maximum of the
     * remaining ones. An incomplete table, such as one copied from elsewhere, can have
     * fields with values that are missing here, and lowering a depth would make those
     * values vanish, so a missing value must not be treated as 0. The default is false,
     * the conservative answer. */
    bool        fieldsComplete;
    int         nfields;    /* number of valid entries in `fields` */
    int         otherDepth; /* depth for writes that belong to no single field */
/* A store a **callee** made through this binding as a `mut ref` argument. Its own slot: the
 * "no single field" one is shared with element writes, and feeding both through it would give
 * every element write promotion pressure. The source expression is what lets `promoteFieldsAt`
 * move the site behind it: with a source the store can be promoted, without one it cannot, and
 * reading through the binding after the frame is gone dangles. */
    Expr       *callSrc;
    int         callSrcDepth;
    int         callMinReq;
/* The same facts as one `fields[]` entry, for a store that belongs to no single field: an
 * element write, or a store the callee made through a `mut ref` out-parameter. Without a source
 * expression there is nothing to promote, and a `new` handed to a callee that stores it would be
 * rejected where the same program written as a direct `b.p = n` is accepted. */
    Expr       *otherSrc;
    /* Same reason as `srcSym`: after `otherSrc` is flattened, the field table of the binding it came
 * from is still needed. */
    struct Sym *otherSrcSym;
    int         otherSrcDepth;
    int         otherMinReq;
    /* **Alias source** left by an initializer at the declaration (`var head = a`): kept in its own
 * slot because `targetDepth` reads `otherSrc`/`otherDepth` as an upper bound on binding depth, and
 * recording a declaration initializer there would count an extra level for ordinary shapes
 * (`head = p` would then be rejected). Used only by promotion: `promoteFieldsAt` follows it to
 * pass requirements on to the allocation site behind the source. **Not flattened**: `head` and
 * `a` share the pointee, so if `a` is later retargeted (`a = p`) the site to promote is still
 * known; a flattened `origin` is the source as of copy time. */
    Expr       *initSrc;
    int         initSrcDepth;
    int         initMinReq;
/* The initializer of a **view**-typed binding (`var s = v.view()`). `initSrc` above is recorded
 * for reference-typed bindings only, and a view is not a `TY_REF` (`slice<T>` is a generic
 * aggregate), so handing the binding back needs its own slot: `return s` must dangle-log like
 * `return v.view()`, and the depth model cannot see that a container's heap storage belongs to the
 * returning frame. Read by `ckReturnViewOfFrame`. */
    Expr       *viewSrc;
    int         depth;      /* lexical depth: a parameter is 0, a local in the function
                             * body is 1, and every nested block adds 1. The escape
                             * checks compare this number. */
    /* Shallowest depth a reference-typed binding **may** point at (smaller lives longer); -1 = not
 * recorded yet.
 *
 * A destination's depth wants "the pointee that may live longest": `cur` may point at a parameter
 * (0) or at a frame-local `new` (1), and reading the destination of `(*cur).f = v` as the max
 * (what the DFA gives) would put a frame-local `v` into the caller's object, dangling once the
 * call returns. Each retargeting takes the min here, biasing toward the longer-lived side (the
 * safe direction). */
    int         minPointee;
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
    /* Set when this binding was initialized from an expression whose storage is provably **not** in
 * any frame: a chain rooted at a global, or a call whose declaration says `effects Ret=0` (the
 * board's views; `mmap`-style handles). Then a **view**-typed binding answers with `refDepth`
 * instead of its slot's block depth, which is what lets
 *
 *     var v: mut slice<u8> = pl.view(off, n)!      // board memory, not in a frame
 *     c.func(v.data)                               // unsigned C calls accept it too
 *
 * work without the user signing anything. Conservative by default: an unmarked binding keeps the
 * slot depth -- a `new` block, a stack array, an arena allocation, or a callee that did not claim
 * it. */
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
    /* This slot serves **pool promotion** only: when a binding is initialized by a pool-creating call,
 * that call's expression is recorded here.
 *
 * Why not reuse `origin`: `originOf` recognizes only allocation-site shapes (`new` / `alloc`) and
 * returns NULL for a call, because an arena's level is decided at the call site rather than by the
 * call (see `origin`); a pool's level, however, **is** decided at the call site (the second
 * argument of `extc_pool_new_at`). A separate slot leaves every other consumer (the `origin`
 * family) untouched. */
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
 * The wrapper is there for type safety: the AST layer cannot name `Sym` and therefore holds
 * an opaque pointer, the same arrangement a `cname` uses, so each side casts once
 * instead of passing a bare `void *` around. */
typedef struct { Sym *sym; } IdentBinding;

/**
 * @brief Fetch the binding an `EX_IDENT` node resolved to.
 *
 * @param e  Expression to inspect.
 * @return   Binding the identifier resolved to; NULL when the node is not `EX_IDENT` or is not
 *           resolved yet.
 */
static inline Sym *identBindOf(Expr *e) {
    return (e && e->kind == EX_IDENT && e->u.ident.sym) ? ((IdentBinding *)e->u.ident.sym)->sym : NULL;
}

/**
 * @brief Convert an allocation site's arena level into a reference depth.
 *
 * Level and depth must be two spellings of the same number, and the conversion lives only here.
 * `ARENA_HOME` and non-positive levels (level not settled yet) both map to 0; a positive level k
 * maps to depth k.
 *
 * @param arenaLevel  Site's arena level: `ARENA_HOME`, a positive block level, or 0/negative when
 *                    not settled yet.
 * @return            Converted depth; a caller that must tell "not settled" from "depth 0" has to
 *                    look at `arenaLevel` itself.
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

/**
 * @brief Tell whether codegen opens a place boundary for this statement.
 *
 * The answer must cover codegen's behavior: the set that answers "yes" may only be larger, never
 * miss a block that codegen really brackets. No such boundary is opened inside a coroutine body,
 * so this returns false when `inCoro` is true.
 *
 * @param s       Statement to classify.
 * @param inCoro  Whether the statement lies inside a coroutine body.
 * @return        True when the statement contains `new` or creates a pool; false otherwise.
 */
bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro);

/**
 * @brief Tell whether a type contains the given protocol marker at any depth.
 *
 * The marker may appear on the type itself, in a type argument, or behind a view/array/reference.
 * To keep self-referential types from making the walk diverge, only those positions are followed;
 * struct fields are not expanded. When the recursion limit is exceeded this returns the
 * conservative answer, true.
 *
 * @param tt    Type table.
 * @param t     Type to check; NULL counts as not containing the marker.
 * @param name  Marker name, for example `coroutine`.
 * @return      True when the marker is found.
 */
bool typeContainsProto(TypeTable *tt, Type *t, const char *name);

/* A `yield` binding: it becomes a frame field, but the frame's Vec only exists after the body is
 * checked, so the name and type are recorded and consumed by the frame layout pass. */
typedef struct {
    struct FuncDef *fn;
    const char    *name;
    Type          *type;
    int            line;
} CoroBind;

typedef struct {
    const char *owner;   /* container the view borrows from */
    const char *view;    /* binding name of the view */
} LiveView;

/* A local that a `return` moved, with the line the mark recorded; see `Checker.moveLog`. */
typedef struct {
    Sym  *sym;
    int   line;
} MoveMark;

/** Per-block list of tracked views; see `liveViewsStmtPre`. */
typedef struct {
    LiveView *items;   /* arena-allocated, grows by doubling; NULL while empty */
    size_t    len;     /* how many are tracked */
    size_t    cap;     /* how many `items` can hold */
} LiveViews;

typedef struct Checker {
    /* How many enclosing blocks will reclaim their own storage (codegen brackets them with
     * `zoneEnter`/`zoneLeaveTo` + `arena_release`). A `yield` inside one of them is a compile
     * error: that block's arena would be gone before the coroutine is resumed. */
    int      placeBoundaryDepth;

    /* Set while checking a `var`/`let` initializer: a `@noCopy` place may be moved into the new
     * binding instead of being rejected as a copy. `moveIntoName` is the destination's name, for
     * the diagnostic that tells the reader where the value went. */
    bool        moveOkHere;
    const char *moveIntoName;
    /* A `return` of a local is a move **on that path only**: recording it permanently would make
     * the rest of the function a use-after-move (`if e { return out }  out.push(x)` is legal). The
     * mark is remembered on `moveLog` and undone when the block that holds the `return` ends --
     * anything after the `return` in that block is dead, so no real use is ever lost. */
    bool        moveIsReturn;
    Vec         moveLog;       /* MoveMark: symbols a `return` moved, with the recorded line */

    /* The two operands of the `EX_BIN` node being checked, both NULL outside one. An operator
     * desugars to a method call, so whether its parameter borrows or copies is a property of the
     * declaration -- not of the operand's spelling. The operands therefore pass through the
     * value-position copy rule unmolested here (`rejectNoCopy`), and `setOpCallArgs` re-applies
     * that rule once the parameter types are known. */
    Expr       *opOperandL;
    Expr       *opOperandR;

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
    Vec        methodChecks; /* MethodCheck*: method calls on a type parameter */
    Vec        deferredUses; /* DeferredUse*: uses of such a call's result, re-checked */
    /* Explicit type arguments written at a call site (`f<i32>(...)`): the node plus the arguments.
     * The `EX_GENCALL` path rewrites itself into an ordinary `EX_CALL`, and inference has to start
     * from these written arguments; otherwise the written form would be pure decoration and would
     * work only where inference happened to succeed anyway. Kept **here** and not as a field on
     * `Expr`: that union is not zeroed for every node kind, so a field only one path sets reads as
     * garbage on the others. */
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
    /* Binding stack of the current `promoteFieldsAt` walk (cycle guard).
 *
 * Source tables can form cycles (`a = b  b = a`, or two structs' field tables pointing at each
 * other), so following the source chain would recurse forever. Visiting a binding once per walk
 * is enough: the requirement has already been applied along that path. The storage holds 64
 * entries and is used as a stack, so it needs no clearing. */
    Sym       *fsChain[64];
    int        fsDepth;
    /* The value currently being stored somewhere.
     *
     * `noteFieldDepthWrite` records which value wrote a field, and its callers already
     * hold that value; passing it through a field keeps the signature of that function
     * unchanged, which would otherwise ripple through every call site. */
    Expr      *curStoreVal;
    Vec        nameUses;    /* NameUse*: how often each name is used in the current
                             * function, for the C name generator */
    Vec        moduleNames;     /* const char*, sorted; the sort enables a compile-time
                                 * lookup optimization */
    size_t     moduleNamesFuncs;   /* the module vectors' lengths when `moduleNames` was built */
    size_t     moduleNamesStructs;
    size_t     moduleNamesTypes;
    size_t     moduleNamesGlobals;
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
     * instantiation with `T = slice<u8>` is never checked at all. The depth is therefore
     * computed when the rule is recorded, assuming that `T` may carry a reference, while
     * the scope is still open and `lookup` still works; instantiation then only has to
     * answer whether `T` really carries one.
     *
     * Deferring the depth computation itself does not work: the function scope is gone
     * by then, `lookup` cannot find the parameters, the depth comes out as 0 and the
     * check silently stops working. */
    Vec        refChecks;   /* RefCheck*: reference rules re-run at instantiation */
    /* Level-dependent rejections, recorded so the same question can be asked again once
     * the data flow and the level pass have settled the numbers. Reporting stays where it
     * was; the record is what makes the two answers comparable. */
    Vec        lvlRejects;  /* LvlRejection* */
    /* Deferred storage verdicts (writes like `(*cur).f = v` whose destination depth is not settled):
 * re-judged by min once the DFA settles. See `PendingRefStore`. */
    Vec        pendingRefStores;  /* PendingRefStore* */
    /* Borrow results whose storage reached a depth (see `BorrowSettled`): a view-returning call
     * keeps that answer even when re-deriving it from the argument's records would answer higher. */
    Vec        borrowSettled;  /* BorrowSettled* */
    /* Merging of candidate regions for "possible cycles" (see `CycleMerge`): the allocation sites a
 * possible-cycle heap reference write's host binding may point at are collected after the DFA
 * (while `lookup` still works) and merged after the global level solve. */
    Vec        cycleMerges;  /* CycleMerge* */
    Vec        callChecks;  /* CallCheck*: call sites resolved at instantiation */
    Vec       *substParams; /* type arguments substituted while an instance is being
                             * re-checked, NULL when not re-checking */
    Vec       *substArgs;
    int        noHoist;     /* non-zero while a statement prefix cannot be emitted */
    /* How many allocation sites have been created so far. A loop condition asks for the
     * difference across its own check to learn whether it allocates at all (`Stmt.condAllocs`),
     * which is what decides between the plain `while (cond)` and the per-round-release shape. */
    int        allocSites;
    CheckPhase phase;      /* see the "phase contract" above: misuse is caught by an assert */
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
    /* Depth of the expression walk right now, for the "expression too deep" guard.
     *
     * The parser bounds *syntactic* nesting (`PARSE_DEPTH_LIMIT` levels of parentheses), but a
     * flat left-associative chain (`1 + 1 + ... + 1`) builds an AST just as deep without any
     * nesting in the source, and every recursive pass over that AST -- the checker, the escape
     * side, the level pass, the code generator -- then descends once per operand. Counting the
     * walk bounds the depth of whatever the AST turns out to be, so the compiler reports a
     * diagnostic instead of overflowing the C stack. */
    int        exprDepth;
    /* Lexical depth of the innermost loop body, or 0 when the walk is not inside a loop.
     *
     * A binding whose `depth` is at least this number was declared inside that loop body,
     * so its storage is released when the iteration ends. A binding with a smaller depth
     * belongs to an enclosing block and outlives the whole loop: writing a fresh `new`
     * into it inside the loop keeps every buffer alive until that block exits, which is
     * the shape the "buffer kept across iterations" warning points at (see the `ST_ASSIGN`
     * case in `checkStmt`). */
    int        loopBodyDepth;
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
     * module-wide requirement replay and the `[store]` dump read them), but every reader asks a
     * question about **one** body, and each record carries the body it came from (`StoreSite.fn`)
     * plus the index that body's records start at (`storeBase`). A reader that wants "the
     * publications of this body" walks `[storeBase, stores.len)` and skips records whose `fn` is
     * not its own (a nested body's records fall inside the slice, hence the second test). The
     * module-wide requirement table, which is deliberately kept across bodies, is `lvlFacts`. */
    Vec        stores;      /* StoreSite* -- publications, folded over by the level pass */
    size_t     storeBase;   /* index in `stores` where the current body's records begin */
    /* ---- place-level verification (`EXTC_DBG_LVL=1`, see `check_lvl.c`) ----
     *
     * `lvlOn` caches the switch so the hot paths that record a site pay one branch, and
     * `lvlSites` collects the call sites whose arguments the callee may store. Nothing here
     * is read or written unless the switch is on; `lvlAnalyze` returns before doing any work
     * when it is off, so a normal run is byte-for-byte unchanged. */
    bool       lvlOn;       /* `EXTC_DBG_LVL`, read once when the checker is built */
    Vec        lvlSites;    /* LvlSite*, module-wide, filtered by `LvlSite.fn` when reporting */
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
     * replay would make the table grow without bound, so recording is suppressed then. */
    bool       lvlSolving;
    Vec        narrow;      /* const char*: cnames proved to be non-null */
    /* Names of the locals of the current function that can be moved out of it.
     *
     * This only affects which arena is chosen as the home arena, and the conservative
     * direction is to include too many names: that costs memory, while missing one would
     * let a live pointer dangle. */
    Vec        escapees;    /* const char* */

    Vec        narrowMarks; /* size_t: length of `narrow` when each scope was entered;
                             * popping the scope truncates `narrow` back to it */

    /* How many **domains** the checker is inside right now (`sched::run { … }`,
    * `par::pool(n) { … }`). `ext` is legal only when this is non-zero: outside a domain there is
    * nobody to hand the task to, and that is a compile error rather than a runtime surprise. */
    int        domainDepth;
    /* **The nearest domain** (`d.run { … }`), i.e. the receiver expression whose block we are
     * checking right now. The scope rule is "`ext` hands the task to the nearest domain", which is a
     * stack; saving and restoring this one field around a block *is* that stack, with no allocation.
     * NULL means "no domain in scope", and that is a compile error at the `ext`. */
    Expr      *curDom;
    /* How many lambda bodies are being checked right now. A lambda inside a lambda would have its
     * environment laid out as part of the outer one; the checker refuses that instead of laying it
     * out wrongly. */
    int        lamDepth;
    /* How many lambda environments this compilation has generated, for unique names. */
    int        lamSeq;

    /* The **return** type expected by the context of the expression being checked (the
 * `pair<i64, u8>` in `var p: pair<i64, u8> = dup(3, u8(4))`). Used only to fill in inference for
 * generic calls whose type arguments appear in the return type alone; `checkInto` replaces it when
 * descending into the arguments or subexpressions of the checked expression. NULL = no context. */
    Type      *expectRet;

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
    /* A fourth kind: an owning value stored into a **view element**.
     *
     * A container's element storage is a plain array: the store copies the bytes and no hook runs
     * for the copy, so a value that owns a reservation would be released twice. While the body is
     * a template the element type is opaque, so the decision waits for the instance -- see
     * `runRefCheck`. */
    bool        viewElem;
    /* Whether that store is a **move**: a plain name hands its ownership over to the slot (the
     * concrete decision, with the symbol marked moved, is in `checkStoreEscape`), and a
     * temporary has no owner to drop it. Only a move is allowed; anything else stays rejected. */
    bool        viewMove;
    /* A fifth kind: an owning value read out of storage the reader does not own, handed to a
     * by-value position. The destination type is opaque while the template is checked, so the
     * decision waits for the instance -- see `runRefCheck`. */
    bool        ownRead;
    /* A sixth kind: an `@value` argument passed by value from inside a generic body.
     *
     * A by-value parameter of an owning `@value` type is the callee's copy: the callee drops it
     * on the way out, so the caller has to hand over a **clone** of a place or a temporary it
     * owns. While the body is a template the parameter type is opaque (`T`), so the decision is
     * deferred to the instance -- see `runRefCheck`. */
    bool        argClone;
    Type       *declType;/* declared type the deferred rule applies to */
    int         line;    /* source line to report */
    const char *what;    /* short noun phrase naming the operation, for the diagnostic */
    FuncDef    *func;    /* the generic function this rule belongs to */
    Ctx        *ctx;     /* the file this store was written in: the diagnostic names it */
} RefCheck;

/* The requirement "this value has to fit into level `at`".
 *
 * It is kept as a fact rather than as a decision because a smaller level means a longer life,
 * so the final level of an allocation site is the smallest of all the requirements that
 * reached it, while which requirements reach it depends on the level the site currently has.
 * Replaying to a fixed point settles both. Recording a decision as soon as a `new` is seen
 * would allow movement in one direction only and make it impossible to tell afterwards whether
 * a level was decided or computed. */
typedef struct { Expr *val; int at; } LvlFact;

/* One publication: a value observed being handed to a place that may outlive it.
 *
 * Every store, return, and "may keep it" argument call is one of these. Recording is
 * the checking pass's entire involvement with arena levels; deciding which arena each
 * allocation site ends up in is a separate pass that folds over this table. Keeping the
 * two apart is what makes the decision independent of the order the checker walks the
 * tree in.
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
     * one after another, but every reader of it asks a question about **one** body: the level
     * pass folds the publications "recorded while the body was checked", and the extent of a
     * walk is a property of the body's own expressions and bindings. The owner is what lets a
     * reader say which records are its own, so the level pass of one function neither re-folds
     * every other function's publications nor enters their bindings into a level table that
     * cannot reach them. A record made outside every body has no owner and is never folded. */
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

/* One call site whose arguments the callee may store, recorded while `checkCallRefArgs`
 * decides about them (`check_lvl.c`).
 *
 * These are **not** the `StoreSite` publications: a `StoreSite` covers assignments, returns,
 * and the "may keep it" argument rules, while the call-argument rules have always judged on
 * the spot and only recorded a `LvlFact`. `EXTC_DBG_LVL` asks a different question -- "what
 * does a whole-body place-level analysis say about *this* argument, and what did the
 * check-time rule say" -- so the site, its destination level `h`, and the per-argument
 * decision are captured here, where both numbers are exact.
 *
 * Nothing reads this outside the verification pass. `decision` codes are ranked
 * (`none < skip < accept < reject`), so several rules looking at the same argument resolve
 * to the strongest answer, matching the fact that an emitted diagnostic is final. */
typedef struct {
    Expr        **kids;      /* arguments, in parameter order (copied out of the caller's Vec);
                              * named `kids` because `args` is an AST field name, and the AST-freeze
                              * gate reads any `.args =` as an AST write */
    int           nargs;     /* argument count */
    unsigned char *decision; /* per argument: 0 not a candidate, 1 accepted, 2 rejected,
                              * 3 the callee may store it but no rule judged it */
    unsigned char *role;     /* per argument: `LVL_ROLE_VALUE` (the bytes are stored into the
                              * callee) or `LVL_ROLE_DEST` (the argument is the place a
                              * reference points into) */
    int           h;         /* place level the callee may store into */
    int           place;     /* place level of the call statement itself */
    int           line;      /* source line of the call */
    const char   *fname;     /* callee name, for the report */
    FuncDef      *fn;        /* body the call was checked in, or NULL outside every body */
} LvlSite;

/** @brief Argument the callee cannot store: not a candidate for the store rule. */
#define LVL_CAND_NONE 0
/** @brief Argument the check-time rule accepted. */
#define LVL_CAND_ACCEPT 1
/** @brief Argument the check-time rule rejected (its diagnostic is final). */
#define LVL_CAND_REJECT 2
/** @brief Argument the callee may store but no rule judged (a type or generic filter). */
#define LVL_CAND_SKIP 3

/** @brief Argument whose bytes the callee may store: the store rule reads its level. */
#define LVL_ROLE_VALUE 0
/** @brief Argument that is itself a destination place: `argLvl` is the pointee's level. */
#define LVL_ROLE_DEST 1
/** @brief No role recorded yet, because no rule looked at this argument. */
#define LVL_ROLE_UNSET 255

/* Classification of "write into a reference slot" at an assignment site, recorded in the
 * statement's result slot (`anSetStoreRefKind`) and read by `computeFnAcyclic` after the body walk.
 * The four criteria:
 *   (i)   object freshly allocated in this frame (`new T` / `alloc<T>(n)`)
 *   (ii)  write of `null`
 *   (iii) the value's level is **strictly greater** than the host's (the value lives shorter, so a
 *         back edge is impossible)
 *   (i')  the value comes from a call whose callee is also acyclic **and** whose return value is
 *         provably freshly allocated (`SR_CALL`)
 *   (iv)  the reference lives only in the heap slot being counted (writing a reference-typed
 *         local/parameter other than at its declaration initializer => rejected)
 * Classification runs while scopes are still open (comparing levels needs `Sym`), because `lookup`
 * is unavailable after the body walk. */
enum {
    SR_NONE  = 0,   /* not a reference-slot store, or the walk did not reach this site (treated as rejected) */
    SR_LOCAL = 1,   /* the reference slot is on the stack (local, parameter, field of a local aggregate): (iv) rejects */
    SR_NULL  = 2,   /* (ii) */
    SR_FRESH = 3,   /* (i) */
    SR_LAYER = 4,   /* (iii) */
    SR_CALL  = 5,   /* (i'): the value comes from a call; wait for the callee's verdict */
    SR_BAD   = 6    /* none of (i)(ii)(iii)(i') holds */
};

/* A **deferred** storage verdict: the destination depth of `(*cur).f = v` is read through
 * `anRefDepth(cur)`, which is written in lexical order during the body walk with no join, whereas
 * `dfAnalyze` runs only after the walk. The verdict is recorded here and the diagnostic is held
 * back; once the DFA settles it is re-judged using the **shallowest** level the pointee may have
 * (`Sym.minPointee`), so word order does not affect the answer.
 *
 * The record keeps the `at`/`depth` seen at the time: when the DFA overflows (its whole result is
 * discarded) the verdict is reported as-is, keeping the conservative behavior. */
typedef struct {
    Expr          *val;
    Expr          *target;
    Sym           *destSym;  /* the reference binding the destination reaches through `*` (resolved at the time) */
    int            line;
    int            at;      /* destination depth used at the time */
    int            depth;   /* depth of the value seen at the time */
    const char    *what;    /* operation name used in diagnostics ("this reference" / "this assignment") */
    const FuncDef *fn;      /* owning function: only records from the current round are re-judged */
} PendingRefStore;

/* A settled borrow result: the call hands back a view of storage that lives in one of its
 * arguments, and that storage reached `depth` (a smaller number is a longer life). The record is
 * written only after the promotion succeeded, so an answer kept here is a fact about the storage,
 * not an estimate; later requirements can only lower it. */
typedef struct {
    Expr          *site;   /* the view-returning call site */
    int            depth;  /* depth its storage reached */
} BorrowSettled;

/* One record of the "possible cycle" candidate-region merge (route C).
 *
 * The trigger is a **possibly cyclic** heap reference write -- classified `SR_BAD` by the acyclic
 * criteria (the value is neither null nor freshly allocated nor younger than the host). The cycle
 * may then span two regions: the objects the host (the `*cur` of `(*cur).f`) may point at are
 * spread over several regions, and the value sits in a third. Region marks are precise iff the
 * partition cuts no strongly connected component, so a cross-region seed is dropped only when the
 * slot holding it is released; a cyclic garbage cycle therefore pins two regions to each other and
 * neither gets swept.
 *
 * The fix is to **merge these candidate regions into the shallowest one** (equivalent to "never
 * cut a possible cycle"): every allocation site the host may point at is moved to the same level.
 * The direction is sound (it only lengthens lifetimes, so it only retains more).
 *
 * Two-phase: `base`/`value` and the candidate sites are recorded **on the spot** during the check
 * walk (while `lookup` works); the merge runs **after the global level solve**
 * (`applyCycleMerges`), when the sites' final levels are settled. All scopes are closed by then, so
 * only pointers are stored here and names are never looked up again. */
typedef struct {
    Sym     *base;       /* host binding: the `cur` of `(*cur).f = v` (or the `p` of `p.f = v`) */
    Expr    *value;      /* the value written */
    int      line;
    FuncDef *fn;         /* owning function */
    Vec      sites;      /* Expr*: allocation sites `base` may point at (collected on the spot, while `lookup` works) */
} CycleMerge;

/* An operator use on a type parameter, deferred to instantiation: recorded while an
 * expression is checked and consumed by the pass that runs afterwards.
 *
 * On the template `T` is opaque, so whether it supports the operator is only answerable per
 * instance; the comparison, ordering and arithmetic operators all use this record. `op` carries
 * which operator it was, and the consumer re-asks the same predicate the concrete-type path
 * uses.
 *
 * `func` is the function the expression belongs to, which is how the consumer asks
 * whether that function was ever called. */
typedef struct { Expr *node; StructDef *owner; const char *op; FuncDef *func; } OpCheck;

/* A method call on a type parameter, deferred to instantiation.
 *
 * While a template is checked `T` is opaque, so `k.hash()` has no method to resolve: which
 * method it means is decided by whatever the instance substitutes for `K`. Recording it here
 * and resolving per instance is also the only place a good diagnostic can come from -- only
 * the instance knows whether its type defines the method at all.
 *
 * The template pass hands back the error type for such a call: the statement keeps checking
 * and `checkAssignable` waves an error type through, while the instance pass is the one that
 * decides -- and it names the instance, exactly as `OpCheck` does.
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

/* A use of a deferred method call's RESULT, re-checked at instantiation.
 *
 * On the template, `k.hash()` hands back the error type (`MethodCheck` explains why), and
 * `checkAssignable` waves an error type through, which keeps the template pass from inventing
 * errors. But "no error here" must not be the last word: `fn f<T>(x: T) -> i8 { return
 * x.hash() }` must be rejected as a lossy conversion when the instance's `hash` returns `i64`,
 * exactly as the same code written on a concrete type is.
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
 * The prototypes that cross between the `check_*.c` files. Each signature is written here
 * once, and its definition lives in exactly one of those files. */

/**
 * @brief Build an integer constant expression node.
 *
 * The type is always `i32`. The checker uses it to fill in slice bounds the user omitted, so that
 * codegen sees two literal bounds and knows the range needs no run-time check.
 *
 * @param c     Checker; the node is allocated from its arena.
 * @param v     Constant value.
 * @param line  Source line number.
 * @return      The new `EX_INT` node.
 */
 Expr *intLit (Checker *c, long long int v, int line);
/**
 * @brief Find a field by name in a struct definition.
 *
 * @param sd    Struct definition to search.
 * @param name  Field name.
 * @return      The matching field definition; NULL when no field has that name.
 */
 FieldDef *findField (StructDef *sd, const char *name);
/**
 * @brief Find a module-level function by name.
 *
 * Instances of generic functions are not names a program can write, so they never match; the
 * template itself may.
 *
 * @param c     Checker.
 * @param name  Already-flattened function name.
 * @return      The matching template or ordinary function; NULL when there is none.
 */
 FuncDef *findFunc (Checker *c, const char *name);
/**
 * @brief Find a method by name on a type.
 *
 * Methods hang off the type's own definition body, so free functions of the same name are not
 * considered; the methods in `Type.mholder` are more specific than the definition body, so they
 * are searched first.
 *
 * @param st    Type to search.
 * @param name  Method name.
 * @return      The matching method definition; NULL when there is none.
 */
 FuncDef *findMethod (Type *st, const char *name);
/**
 * @brief Find a type's operator method, falling back to another name when necessary.
 *
 * `!=` falls back to `==` when it has no method of its own, and codegen implements it by
 * negating that.
 *
 * @param tt        Type table; arguments of a generic receiver are substituted here.
 * @param b         Type with outer wrappers already stripped.
 * @param sym       Operator name to find, such as `==`, `<`, `<<`.
 * @param rhs       Right operand type; the operator must match it exactly.
 * @param fallback  Operator name to try when `sym` is absent; NULL when no fallback is wanted.
 * @return          The matching method definition; NULL when neither name matches the operand.
 */
 FuncDef *findOp (TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);
/**
 * @brief Find the operator method on a type whose right operand type matches exactly.
 *
 * A type may define several methods under the same operator name; the signature shape is fixed to
 * `self: ref T` plus one operand, so only the operand type tells them apart. Methods of generic
 * types are written with the defining type's parameters, so the receiver's type arguments are
 * substituted before comparing.
 *
 * @param tt    Type table, for substituting a generic receiver's arguments.
 * @param st    Type whose methods are searched.
 * @param name  Operator name, such as `<<`.
 * @param rhs   Right operand type; NULL means match by name only and take the first method.
 * @return      The matching method definition; NULL when the type has no such operator.
 */
 FuncDef *findOperator (TypeTable *tt, Type *st, const char *name, Type *rhs);
/**
 * @brief Fetch the struct definition behind a type.
 *
 * Both `TY_STRUCT` and `TY_GENERIC` point at a `StructDef`; other types have none.
 *
 * @param t  Type to inspect.
 * @return   The corresponding struct definition; NULL when the type is NULL or of another kind.
 */
 StructDef *structOf (Type *t);

/**
 * @brief Warn when a binding position copies a `@sharesStorage` type by value.
 *
 * Such a copy gives a second name to one piece of storage: a write through one is visible through
 * the other, and releasing one leaves the other dangling. Only shapes that copy an existing value
 * are reported, not newly constructed values; copies in the library's own code are not reported.
 *
 * @param c  Checker, for reporting the diagnostic.
 * @param e  Expression in value position.
 * @param t  Its type.
 */
void warnSharedCopy(Checker *c, Expr *e, Type *t);

/**
 * @brief Warn when an existing `@sharesStorage` value is handed over by value.
 *
 * Handing over an existing value makes the caller and this frame share the same storage; a value
 * freshly constructed in this function body is a transfer and is not reported. The test is the
 * position of the binding declaration's line number relative to the signature's line number.
 * Returns in the library's own code are not reported.
 *
 * @param c  Checker, for reporting the diagnostic.
 * @param e  Expression being returned.
 * @param t  Its type.
 */
void warnSharedReturn(Checker *c, Expr *e, Type *t);
/**
 * @brief Declare a binding in the innermost scope and return it.
 *
 * A reference-typed binding starts with its slot's depth as the point-to depth, which the party
 * that knows the initializer then narrows. The caller is responsible for writing the generated C
 * name back into the AST.
 *
 * @param c       Checker.
 * @param name    Name in the source; the generated C name is derived from it.
 * @param t       Declared type.
 * @param mut     `var` is true, `let` is false.
 * @param shadow  Whether a binding of the same name may shadow one already in this scope.
 * @param line    Source line number, for diagnostics.
 * @param depth   Lexical depth of the new binding.
 * @return        The new binding; the existing one when a duplicate `var` is rejected.
 */
 Sym *declare (Checker *c, const char *name, Type *t, _Bool mut, _Bool shadow, int line, int depth);
/**
 * @brief Resolve a binding by name, searching the innermost scope first and module-level bindings
 *        last.
 *
 * A later `let` in the same scope wins, so the scan runs backwards. Module-level bindings live in
 * their own table rather than taking a scope, otherwise every function body would gain a level
 * from the globals.
 *
 * @param c     Checker.
 * @param name  Source name to resolve.
 * @return      The innermost binding of that name; NULL when there is none.
 */
 Sym *lookup (Checker *c, const char *name);

/** A view bound in a block, tracked so growth underneath it can be rejected. */

/**
 * @brief Report growth of a container that a still-used view of the block borrows from.
 *
 * @param c      Checker.
 * @param lv     Views bound so far in this block.
 * @param stmts  The block's statements.
 * @param i      Index of the statement about to be checked.
 */
/**
 * @brief Record that a `@noCopy` binding is moved into a new name.
 *
 * The annotated form passes through `rejectNoCopy`, which honours `Checker.moveOkHere`; the
 * unannotated form takes `checkExpr`, which never looks at copies, so the transfer is recorded
 * here. Only a plain name moves.
 *
 * @param c     Checker.
 * @param e     Initializer of the new binding.
 * @param dest  Name of the destination binding, for the diagnostic.
 * @return      `true` when a transfer was recorded.
 */
bool ckMoveNoCopyInto(Checker *c, Expr *e, const char *dest);

/**
 * @brief Reject a copy of a `@noCopy` value, with the same diagnostic the annotated form gives.
 *
 * The unannotated binding path checks its initializer as an expression, which never looks at
 * copies, so an initializer that is a **place inside an aggregate** -- a field or an element of
 * a view -- would be copied silently: two names for one plate, and the second destructor to run
 * hands the storage back twice. A plain name is a transfer (`ckMoveNoCopyInto`) and a call result
 * is a fresh value, so only the aggregate-place case reaches the rejection.
 *
 * @param c  Checker, for reporting.
 * @param e  Initializer of the new binding.
 * @param t  Its type.
 * @return   `true` when an error was reported.
 */
bool ckRejectNoCopyCopy(Checker *c, Expr *e, Type *t);
/**
 * @brief Reject a read of an owned value out of storage the reader does not own.
 *
 * `outer.view()[0]`, `box.c` and `*p` all produce a value whose bytes belong to an
 * aggregate that still frees them; handing that value to a by-value position would leave two
 * names owning one storage, and the second destructor to run hands it back a second time.
 *
 * @param c  Checker, for reporting.
 * @param e  Expression in value position.
 * @param t  Its type.
 * @return   `true` when an error was reported.
 */
bool ckRejectOwningRead(Checker *c, Expr *e, Type *t);

/**
 * @brief Defer an owning-read rejection to instantiation.
 *
 * A generic body is checked once on the template, where a parameter type is opaque: `T` may be
 * an owning type or a plain number, so the rule cannot be decided there. The record is replayed
 * for every instance, with the value and the destination substituted.
 *
 * @param c     Checker; the record is appended to `c->refChecks`.
 * @param dst   Destination type, as written in the template body.
 * @param a     Value read out of storage.
 * @param what  Short noun phrase naming the operation, for the diagnostic.
 */
void ckDeferOwningRead(Checker *c, Type *dst, Expr *a, const char *what);
/**
 * @brief Defer deciding that a by-value argument has to be cloned until the template is
 *        instantiated.
 *
 * Inside a template the parameter type is still `T`, so `structOf` has nothing to test and the
 * code generator cannot know either: it decides per instance with the substituted type, and the
 * hook has to be recorded as used here or reachability pruning drops its body and the emitted C
 * names a function nobody wrote.
 *
 * @param c    Checker; the record is appended to `c->refChecks`.
 * @param dst  Parameter type, as written in the template body.
 * @param a    The argument expression.
 */
void ckDeferArgClone(Checker *c, Type *dst, Expr *a);
/**
 * @brief Reject handing an owning value to a by-value position that cannot transfer it.
 *
 * Every position that stores its operand inside a new owner (a by-value argument, an enum
 * payload, a struct-literal field) transfers a `@noCopy` value instead of copying it. A plain
 * name transfers; a field, an element or a dereference cannot be taken out of the aggregate it
 * belongs to, so copying it would give one storage two owners.
 *
 * @param c     The checker.
 * @param dst   The type of the destination.
 * @param a     The expression handed over.
 * @param what  Short name of the destination, for the transfer bookkeeping.
 */
void ckOwningArgMoveOnly(Checker *c, Type *dst, Expr *a, const char *what);

/**
 * @brief Whether an expression produces a view of storage that its container can move.
 *
 * @param c      The checker; unused by the predicate itself, kept for uniformity.
 * @param e      The expression to classify.
 * @param owner  Receives the container's type name, or NULL.
 * @return       true when the value is a view of movable storage.
 */
bool ckHoldsMovingView(Checker *c, Expr *e, const char **owner);

/** @brief The variable a moving view is borrowed from, when the receiver is a plain name. */
bool ckHoldsMovingViewOf(Checker *c, Expr *e, const char **varName);

/**
 * @brief Whether storage of this type can be handed back while a view of it is still live.
 *
 * @param c  The checker.
 * @param t  Type to classify.
 * @return   true when a view of this type can outlive the storage it points at.
 */
bool ckStorageCanMove(Checker *c, Type *t);

/**
 * @brief Does the value about to be returned hand out a view of storage this frame owns?
 *
 * @param c     Checker.
 * @param e     Returned value.
 * @param line  Source line of the return.
 * @return      true when a diagnostic was reported.
 */
bool ckReturnViewOfFrame(Checker *c, Expr *e, int line);

/**
 * @brief Whether an argument hands over a view whose container can move its storage.
 *
 * @param c      The checker.
 * @param e      The argument expression.
 * @param owner  Receives a printable name for the container, or NULL.
 * @return       true when the argument is a view of movable storage.
 */
bool ckViewOfMovableStorage(Checker *c, Expr *e, const char **owner);

/**
 * @brief Note the views bound by statement `i` before it is checked.
 *
 * @param c     Checker.
 * @param lv    Per-block list of live views.
 * @param stmts Block statements.
 * @param i     Index of the statement about to be checked.
 */
void liveViewsStmtPre (Checker *c, LiveViews *lv, Vec *stmts, size_t i);

/**
 * @brief Remember a view bound by the statement that was just checked.
 *
 * @param c   Checker; unused, kept for a uniform signature.
 * @param lv  Views of the enclosing block.
 * @param s   The statement that was just checked.
 */
void liveViewsStmtPost(Checker *c, LiveViews *lv, Stmt *s);

/**
 * @brief Whether a callee's body may move the storage of a container it was handed.
 *
 * @param f  Callee; a body-less function cannot grow anything.
 * @return   true when the body contains a growing call at any depth.
 */
bool ckCalleeMayGrow(FuncDef *f);

/**
 * @brief Whether this expression is a call shape that carries a callee.
 *
 * The three call kinds plus the two operators: after checking, an operator that resolved to a
 * method has exactly the shape of a method call -- `planCallee` points at the selected operator
 * method, the hidden arena/zone arguments live on the same node, and codegen emits the same call.
 * Every walk that asks for the callee must accept all five; the pool-level bugs that motivated
 * this predicate all came from one walk accepting `EX_CALL`/`EX_METHOD` and not the operators.
 *
 * `EX_NEW` carries a callee only when the context asked for a `@buffer` type: then the `new`
 * stands for that type's constructor, and the closures must see the edge.
 *
 * @param e  Expression; NULL allowed.
 * @return   True when the node names a callee (possibly one that is not resolved yet).
 */
bool exprCarriesCallee(const Expr *e);

/**
 * @brief Whether this node can only be a call: `EX_CALL`, `EX_METHOD`, `EX_ASSOC`.
 *
 * The narrower companion of `exprCarriesCallee`, for questions about a call whose callee is *not
 * resolved*. An operator with no callee is not "unknown" -- it is a builtin arithmetic node with
 * no hidden arguments -- so treating it as an unresolved call marks the enclosing function's
 * effects incomplete and turns into false rejections at every call of that function.
 *
 * @param e  Expression; NULL allowed.
 * @return   True for the three call kinds.
 */
bool exprIsCallKind(const Expr *e);

/**
 * @brief Whether a call to this method name can move the receiver's storage.
 *
 * @param n  Method name, or NULL.
 * @return   true for the names `growMethodName` lists (`push`, `append`, `grow`, ...).
 */
bool ckGrowMethodName(const char *n);
/* The binding at the root of a place, found by walking through fields, indexes and slices; NULL
 * when the place is not rooted in a binding. */
/* ---------------------------------------------------------------- Phase contract
 *
 * `checkModule` is a **pipeline**, and the phase is explicit state (`Checker.phase`):
 *
 *   | Phase | When | What it sees | What it **cannot** see => what must remember it |
 *   |---|---|---|---|
 *   | `PHASE_DECLS` | struct/enum/trait/`@export`/C-ABI checks | declaration tables, type table | any fact about function bodies |
 *   | `PHASE_BODIES` | one function body at a time | **scopes** (`lookup`/`placeRoot` work), `stores`, every `recordXxx` recorder | effect summaries (not computed yet, or only from signatures), `makesPool` |
 *   | `PHASE_POST` | effect summaries -> deferred-call fixpoint -> operator/protocol/result-type/reference-rule replay -> arena and zone channels of `eSites` -> `lvlFacts` replay -> `allSyms` promotion -> union fixpoint of escape sets | all tables; the deferred machinery (`refChecks`/`opChecks`/`lvlFacts`) exists for this phase | **scopes** (all closed) => resolving a **local** name necessarily fails or hits an unrelated global |
 *
 * So anything in `PHASE_POST` that must identify something has to use information **recorded on
 * the node or symbol** (`identBindOf`, `FuncDef.paramSyms`), never `lookup`. `lookup`/`placeRoot`
 * require scopes: in a debug build (`EXTC_DBG=1`), calling them from `PHASE_POST` aborts on the
 * spot and names the offending line instead of silently returning an "unrecognized" answer.
 * Conversely, `PHASE_BODIES` must not read quantities that only hold after that phase (such as
 * `makesPool`): facts that are not computable yet belong in a deferred mechanism such as
 * `recordLvlFact`. */

/* ---------------------------------------------------------------- Depth / identity: four questions
 *
 * The single authority for **which question, which function, which polarity**.
 *
 * **Unknown (⊤)**: `DEPTH_UNKNOWN` -- it means "**cannot prove it lives long enough**", so on the
 * **comparison** side it must lead to **rejection**. It is not `0`: `0` in this repository means
 * "lives longest" = the **accept** direction. It may appear only in comparisons/records and must
 * **not** be passed as a level to promotion (the `want` of `promoteInto2` is "which level to
 * promote to", not a depth).
 *   ① how deep is the storage? (the **places** `a`, `a.f`, `a[i]`)         -> `slotDepth`
 *   ② how deep is the thing the value points at? (the **values** `p`, `f(x)`) -> `targetDepth`
 *   ③ what is the **binding** at this root? (identity, not spelling)      -> `placeRoot` (scope lookup)
 *                                                                         -> `rootSymNoScope`/`identBindOf` (node only)
 *   ④ what is the **name** at this root? (diagnostics)                    -> `placeRootName`
 *
 * **Polarity contract (the most important one)**: ①② return a **depth**, where `0` = "lives
 * longest" (globals, the parameter's pointee, caller-chosen places); **a smaller number lives
 * longer**. Any "cannot tell" fallback must land on the **longer-lived** side, or reject outright.
 *
 * One fact, one name: **new code must not** write a fourth "take the root of an expression" loop;
 * when another step combination is needed, pass an `EXTC_ROOT_*` mask to `extcRootLeaf`. The step
 * differences between the three entry points are exactly these masks:
 *
 *   | Entry point | Steps | Hop limit | How `EX_IDENT` resolves |
 *   |---|---|---|---|
 *   | `placeRoot`      | FIELD INDEX SLICE | unlimited | scope lookup (`lookup`) |
 *   | `placeRootName`  | FIELD INDEX DEREF | unlimited | **syntactic name** (no resolution) |
 *   | `rootSymNoScope` | FIELD INDEX DEREF SIGN | 16 | binding on the node (`identBindOf`) |
 */
/* `planRetBorrowArg` answer meaning "the body named more than one source": the call site cannot
 * use a single argument, so it keeps the rule that reads every argument. */
#define RET_BORROW_CONFLICT  (-2)

/* Unknown depth (⊤): see the polarity contract in "Depth / identity" above -- comparisons treat it
 * as "deeper" (=> reject), and it must never be passed as a level to promotion. It is the safe
 * fallback at comparison sites: when it cannot prove life is long enough, it assumes it is not. */
#define DEPTH_UNKNOWN  (1 << 24)
/* Is this number "unknown"? */
#define depthIsUnknown(d)  ((d) == DEPTH_UNKNOWN || (d) > DEPTH_UNKNOWN)

#define EXTC_ROOT_FIELD  (1u << 0)
#define EXTC_ROOT_INDEX  (1u << 1)
#define EXTC_ROOT_SLICE  (1u << 2)
#define EXTC_ROOT_DEREF  (1u << 3)
#define EXTC_ROOT_SIGN   (1u << 4)

/**
 * @brief Walk down to the identifier node at the root, following only the steps a mask allows.
 *
 * Only the syntactic chain is followed: names are not resolved and scopes are not consulted, and
 * the returned node is guaranteed to be `EX_IDENT`. An empty mask or one with unknown bits is
 * rejected by an assert in debug builds; when no root is reached the caller conservatively treats
 * the expression as "not a place".
 *
 * @param e        Starting expression.
 * @param steps    Allowed step mask: an OR of `EXTC_ROOT_*` bits.
 * @param maxHops  Hop limit; <= 0 means unlimited.
 * @return         The identifier node at the root; NULL when no root is reached or the hop limit
 *                 is exceeded.
 */
Expr *extcRootLeaf(Expr *e, unsigned steps, int maxHops);

/**
 * @brief Resolve the binding at the root of a place expression.
 *
 * Walks fields, indexes, and slices down to the root identifier, then resolves it by scope lookup;
 * it therefore requires at least one open scope (asserted in debug builds). Called after every
 * scope is closed, it can only reach module-level bindings.
 *
 * @param c  Checker, which supplies the scope stack.
 * @param e  Place expression.
 * @return   The binding for the root identifier; NULL when no identifier is reached or the name
 *           does not resolve.
 */
 Sym *placeRoot (Checker *c, Expr *e);
/**
 * @brief Check an expression and write its type back onto the node, without dereferencing
 *        references.
 *
 * This is the entry point the rest of the checker uses; the result also stays in `e->type`. When
 * the expression contains a call, the statement effect flags are set so that `??` can decide
 * whether temporaries must be hoisted early.
 *
 * @param c  Checker.
 * @param e  Expression; NULL directly yields the error type.
 * @return   The expression's type; the error type when it cannot be determined, so a caller never
 *           sees a null type.
 */
 Type *checkExpr (Checker *c, Expr *e);
/**
 * @brief Check an expression against an expected type.
 *
 * When the expected type is option or result, bare constructors are completed first; when it is a
 * value type, copies of `@noCopy` values and borrowed results of stream operators are rejected. A
 * reference in a non-reference expected position is an error.
 *
 * @param c     Checker.
 * @param want  Type the context expects; NULL means any value will do.
 * @param e     Expression to check.
 * @return      The expression's type; the pointee type when a reference appears in value position
 *              (an error has been reported by then).
 */
 Type *checkInto (Checker *c, Type *want, Expr *e);
/**
 * @brief Check an expression that may be `e?`.
 *
 * `?` is legal only in a few statement positions; those positions come through this entry point,
 * and every other expression is checked in value position.
 *
 * @param c  Checker.
 * @param e  Expression, possibly an `EX_TRY` node.
 * @return   The payload type unwrapped by `?`, otherwise the value-position type.
 */
 Type *checkMaybeTry (Checker *c, Expr *e);

/**
 * @brief Reject keeping a borrowed result of a stream operator.
 *
 * @param c  Checker.
 * @param e  Initializer, argument, or return expression.
 * @return   True when it is a stream borrow and the error has been reported; false otherwise.
 */
 bool rejectStreamBorrow (Checker *c, Expr *e);

/**
 * @brief Tell whether an expression is the result of a stream operator that returns a reference.
 *
 * `fin >> x` hands back the same stream object rather than a new value; a shift's result type is
 * an integer, so it never matches.
 *
 * @param e  Expression to check.
 * @return   True when the expression is a binary operation starting with `<` or `>` whose type is
 *           a reference.
 */
 bool isStreamBorrow (Expr *e);
/**
 * @brief Check one argument of `print` or `println`; references are auto-dereferenced here.
 *
 * Printing a reference can only mean printing the value it points at, so this is the one remaining
 * value position that keeps automatic dereference.
 *
 * @param c  Checker.
 * @param e  Argument expression.
 * @return   Type of the printed value; when the argument is a reference, its pointee type, with
 *           the node marked as needing a dereference.
 */
 Type *checkPrintArg (Checker *c, Expr *e);
/**
 * @brief Check `e?` and forward failure to the enclosing function.
 *
 * The operand must be `option` or `result`, and the enclosing function must return the same kind;
 * for `result`, the error type must be identical as well. In `main`, with an operand that really
 * carries a payload, it unwraps directly and traps on failure.
 *
 * @param c  Checker.
 * @param e  The `?` expression.
 * @return   The unwrapped payload type; the error type when the operand or the enclosing return
 *           type does not fit.
 */
 Type *checkTryInner (Checker *c, Expr *e);
/**
 * @brief Check an expression in value position; a reference must be written as `*p`.
 *
 * Expressions that require a reference themselves, such as `ref x` and `alloc`, keep their
 * reference type; member selection navigates rather than reads a value, so it still sees through
 * references. `@noCopy` values are rejected here when copied.
 *
 * @param c  Checker.
 * @param e  Expression in value position.
 * @return   The value's type; the pointee type when a reference appears in value position (an
 *           error has been reported by then).
 */
 Type *checkValue (Checker *c, Expr *e);
/**
 * @brief Fetch the type of the k-th payload of a variant.
 *
 * When the enum is a generic instance, type parameters inside the payload type are substituted
 * with the instance's type arguments: in `maybe<i64>`, the payload of `just` is `i64`, not `T`.
 *
 * @param tt  Type table, used for substitution.
 * @param et  Enum instance type; a type that is not an instance yields the payload type as written.
 * @param v   Variant definition.
 * @param k   Payload index, starting at 0.
 * @return    Type of the payload at that position.
 */
 Type *payloadType (TypeTable *tt, Type *et, Variant *v, size_t k);
/**
 * @brief Build a `slice<elem>` type from the prelude's declaration.
 *
 * @param c     Checker, which holds the prelude's slice definition.
 * @param elem  Element type.
 * @return      The constructed view type; the error type when the prelude has no slice definition.
 */
 Type *sliceOf (Checker *c, Type *elem);
/**
 * @brief Desugar a whole-array assignment statement (`out = a + b` into an element-wise write
 *        loop); returns NULL when it does not apply.
 *
 * Neither the target nor the right-hand side has been checked yet: this function consults only the
 * name table, and on a match replaces the statement with a "length guard + write loop" block. The
 * caller writes the statement back into the statement table and then checks the replacement. Both
 * check paths, function body and block body, must call it.
 *
 * @param c  Checker.
 * @param s  Candidate statement.
 * @return   Replacement statement; NULL when it should not be desugared (the caller checks the
 *           original).
 */
Stmt *arrayAssignLower(Checker *c, Stmt *s);
/**
 * @brief Substitute the current instance's type arguments into a type.
 *
 * @param c  Checker; the substitution context is its parameter and argument tables.
 * @param t  Type to substitute into.
 * @return   The substituted type; `t` unchanged when no instantiation is in progress or `t` is
 *           NULL.
 */
 Type *tsub (Checker *c, Type *t);
/**
 * @brief Fetch the element type of a view type.
 *
 * The view protocol is two members, `data` and `len`; this recognizes it by the name `slice` with
 * exactly one type argument.
 *
 * @param t  Type to inspect.
 * @return   The element type; NULL when `t` is not a single-argument slice instance.
 */
 Type *viewElemOf (Type *t);

/**
 * @brief Record that method/operator `m` was used on this receiver type (needed only for generic
 *        struct receivers).
 *
 * A generic method body is shared by all instances, so a template-level use bit cannot tell which
 * instance really used it; the checker's instance replay and codegen both expand per instance, so
 * the concrete instance is recorded here.
 *
 * @param m  Template definition of the method/operator.
 * @param t  Receiver type (`ttBase` is taken; recorded only when it is a concrete instance of that
 *           struct).
 */
void ckNoteInstUse (FuncDef *m, Type *t);

/**
 * @brief Bookkeeping for a `new T[n]` that becomes a `@buffer` constructor call.
 *
 * @param c     The checker.
 * @param e     The `new` expression standing for the call.
 * @param ctor  The associated function taking the element count.
 * @param owner The type instance the call is made on.
 * @param args  A one-element vector holding the element-count expression.
 */
void ckNewAsBufferCall (Checker *c, Expr *e, FuncDef *ctor, Type *owner, Vec *args);
/**
 * @brief Find a variant by name in an enum definition.
 *
 * @param td    Enum definition to search.
 * @param name  Variant name.
 * @return      The matching variant; NULL when `td` is empty or has no such name.
 */
 Variant *findVariant (TypeDef *td, const char *name);
/**
 * @brief Read an expression as an integer literal.
 *
 * An integer literal wrapped in unary minus counts too, so a negative slice bound can be rejected
 * at compile time instead of at run time.
 *
 * @param e    Expression to read.
 * @param out  Receives the literal's value on success.
 * @return     True and writes `out` when `e` is an integer literal (possibly with unary minus);
 *             false otherwise.
 */
 _Bool asIntLit (Expr *e, long long int *out);
/**
 * @brief Tell whether a statement always leaves the enclosing block.
 *
 * This is what recognizes guards such as `if p == null { return }`: reaching the code after the
 * `if` proves the condition did not hold. For a block, the last statement decides recursively; an
 * empty block does not exit.
 *
 * @param s  Statement, usually the then-branch of an `if`.
 * @return   True when the statement is return, break, or continue, or when a block's last
 *           statement is.
 */
 _Bool blockExits (Stmt *s);
/**
 * @brief Check that a value of type `got` may be assigned where type `want` is expected.
 *
 * Only lossless conversions are implicit: widening, non-null reference to nullable reference,
 * dropping `mut`, and so on; every other conversion must be written by the user. This is the
 * single gate for assignments, arguments, field initializers, and payload values. An assignment
 * whose type mentions a type parameter is recorded and re-checked against the real type at
 * instantiation.
 *
 * @param c     Checker, for rendering types and reporting errors.
 * @param want  Expected type.
 * @param got   Type of the expression being assigned.
 * @param node  That expression, used for the line number and literal adaptation.
 * @param what  Description of the destination (such as "argument", "field `x`"); appears in the
 *              diagnostic.
 * @return      True when the assignment is legal; false when it is not (an error is reported).
 */
 _Bool checkAssignable (Checker *c, Type *want, Type *got, Expr *node, const char *what);
/**
 * @brief Check that a reference in a value does not live longer than the storage it is placed
 *        into.
 *
 * All allocation points in the value are promoted first, and only then is the depth comparison
 * made, because an early return on "shallow enough to look fine" would leave the allocation points
 * unconstrained. When the value type mentions a type parameter, a deferred check is recorded and
 * false is returned; it is decided at instantiation.
 *
 * @param c     Checker.
 * @param val   Value being stored or returned.
 * @param at    Destination depth; 0 means it must outlive this frame.
 * @param line  Source line number, for diagnostics.
 * @param what  The operation being checked; appears in the diagnostic.
 * @return      True when the value would leave a dangling reference (an error is reported);
 *              false otherwise.
 */
 _Bool checkEscape (Checker *c, Expr *val, int at, int line, const char *what);
/**
 * @brief Check every escape rule for one store.
 *
 * Beyond the depth rules, a borrowed value must not be stored where it outlives this call. When
 * the destination depth is 0 and the value traces back to a parameter and the destination also
 * traces back to a parameter, lifetime is judged at the call site instead. A value type that
 * mentions a type parameter gets a deferred check.
 *
 * @param c       Checker.
 * @param val     Value being stored.
 * @param target  Place it is stored into.
 * @param line    Source line number, for diagnostics.
 * @return        True when the store is rejected; false when it is allowed.
 */
 _Bool checkStoreEscape (Checker *c, Expr *val, Expr *target, int line);

/**
 * @brief Record one publication: a value is placed at level `at`; no verdict is made here.
 *
 * The check pass only records; a later fold solves the levels together with the depth fixpoint, so
 * no verdict depends on traversal order. Recording is skipped when the same value and destination
 * have already been recorded at a level no deeper. Records hang off `c->stores` and carry their
 * function body for the per-body fold.
 *
 * @param c       Checker; the record is appended to its publication table.
 * @param val     Value being published.
 * @param target  Destination; NULL means a return-value publication.
 * @param at      Destination level; 0 means it must outlive this frame.
 * @param line    Source line number, for diagnostics.
 */
 void recordStore (Checker *c, Expr *val, Expr *target, int at, int line);

/**
 * @brief Record a level-related rejection for later re-checking.
 *
 * No error is reported here; only the value, the level, and the depth seen by the verdict are
 * kept.
 *
 * @param c      Checker; the record is appended to its rejection table.
 * @param val    Rejected value.
 * @param at     Depth used by the verdict.
 * @param depth  Depth seen at check time.
 * @param line   Source line number.
 */
 void recordLvlRejection (Checker *c, Expr *val, int at, int depth, int line);

/**
 * @brief Depth verdict with a destination: when the destination is reached through a named
 *        reference, the rejection is deferred and re-judged after the DFA.
 *
 * @param c       Checker.
 * @param val     Value being stored.
 * @param at      Destination depth.
 * @param line    Source line number.
 * @param what    Operation name for the diagnostic.
 * @param target  Place it is stored into; NULL behaves like `checkEscape`.
 * @return        True when the verdict is a rejection.
 */
bool checkEscapeDefer(Checker *c, Expr *val, int at, int line, const char *what, Expr *target);

/**
 * @brief Tell whether an expression contains `new` (or a generic `alloc<T>(n)`).
 *
 * The question the "buffer kept across iterations" warning asks: an assignment whose value
 * allocates is the one that can leave a buffer behind, while an assignment of an existing value
 * cannot. The walk shares the AST traversal with the allocation analyses, so a new expression
 * kind is covered in one place.
 *
 * @param e  Expression; NULL returns false.
 * @return   True when some node in the tree is `new` or `EX_GENCALL`.
 */
bool exprContainsNew(Expr *e);

/**
 * @brief Tell whether the write target is reached **through a named reference** (`(*a).f` / `*a`)
 *        and, if so, return that binding.
 *
 * The field slot of `(*a).f = v` hangs off `a` (`placeRoot` does not step through a dereference),
 * and promotion along a cycle or chain must reach from there into the field table of the object
 * being stored into.
 *
 * @param c  Checker.
 * @param e  Place expression of the write target.
 * @return   The named reference binding behind the final `*`; NULL otherwise.
 */
Sym *destDerefSym(Checker *c, Expr *e);

/**
 * @brief Re-judge deferred storage verdicts once the DFA settles; report only what is still too
 *        shallow.
 *
 * The destination depth here is the **shallowest** level the pointee may have (`Sym.minPointee`),
 * not the DFA's max: the max would let dangling references of the `q2_dualpointee` kind (possibly
 * a parameter (0) or a frame-local `new` (1)) through.
 *
 * @param c           Checker; scopes must still be open.
 * @param dfaSettled  Whether the DFA produced a usable result (on overflow, the recorded numbers
 *                    are reported as-is).
 */
void recheckPendingRefStores(Checker *c, bool dfaSettled);

/**
 * @brief Record a "possibly cyclic" heap reference write for the candidate-region merge after the
 *        level solve.
 *
 * Recorded only when the acyclic classification is `SR_BAD` and the host is reached through a
 * named reference (a stack slot is not this case). Records are deduplicated by
 * `(base, value, fn)`: the driver checks the same site over several rounds. The allocation sites
 * the host may point at are collected on the spot (scopes are still open, so `lookup` works); the
 * merge itself waits until after the global level solve (see `applyCycleMerges`).
 *
 * @param c      Checker (scopes still open).
 * @param base   Host binding (the `cur` of `(*cur).f = v`).
 * @param value  Value being written.
 * @param line   Source line number.
 */
void noteCycleMerge(Checker *c, Sym *base, Expr *value, int line);

/**
 * @brief After the global level solve: merge the candidate regions of "possible cycles" into the
 *        shallowest one.
 *
 * Runs before placement and only changes `anMinAt` (placement then writes `arenaLevel`/`refDepth`
 * from it). As soon as any candidate is in the home region (`needsHome` is settled) nothing is
 * moved: the home-region decision cannot be changed, and that is the boundary of this step.
 *
 * @param c  Checker.
 */
void applyCycleMerges(Checker *c);

/**
 * @brief Re-check recorded rejections against the numbers solved by each pass.
 *
 * Only the recorded later depth is updated and a line may optionally be printed; diagnostics
 * already reported are not changed.
 *
 * @param c  Checker.
 */
 void recheckLevelRejections (Checker *c);

/**
 * @brief Promote the allocation points reachable through a binding's field table to the level
 *        they must live to.
 *
 * Only the promotion half is done here: only slots with a recorded source are promoted, and only
 * after a promotion succeeds may that slot's accounting drop to the destination level. A binding
 * whose address was taken is still promoted (promotion only moves the allocation into a
 * longer-lived arena); its accounting just skips the drop.
 *
 * @param c     Checker.
 * @param sy    Binding whose field table is walked.
 * @param at    Depth the container is stored at; 0 means it must outlive this frame.
 * @param hops  Number of carrier steps already taken.
 * @return      True when every field that has a source was promoted; false when some field cannot
 *              be promoted, in which case the caller reports the depth error.
 */
 bool promoteFieldsAt (Checker *c, Sym *sy, int at, int hops);
/**
 * @brief Tell whether C can compare values of this type directly.
 *
 * Numbers, `bool`, and payload-free enums (plain integers in C) can be compared directly; `str` is
 * excluded because its `==` would degrade to pointer comparison; an enum with payloads is a tagged
 * struct in C, and structs have no `==`.
 *
 * @param t  Type to classify.
 * @return   True when C's `==` compares values rather than addresses.
 */
 _Bool cmpIsNative (Type *t);
/**
 * @brief Tell whether an enum has at least one variant with a payload.
 *
 * @param td  Enum definition.
 * @return    True when some variant has a payload; false when `td` is empty or all variants are
 *            payload-free.
 */
 _Bool enumHasPayload (TypeDef *td);
/**
 * @brief Tell whether a name is a comparison operator.
 *
 * @param op  Operator name.
 * @return    True for one of `==`, `!=`, `<`, `<=`, `>`, `>=`.
 */
 _Bool isCmpOp (const char *op);
/**
 * @brief Tell whether a name is an equality operator.
 *
 * `==` and `!=` share one native-comparison rule, and `!=` is also rewritten as a negated `==`.
 *
 * @param op  Operator name.
 * @return    True for `==` or `!=`.
 */
 _Bool isEqualityOp (const char *op);
/**
 * @brief Tell whether a name is an overloadable operator that a type may define more than once.
 *
 * The overloadable set is the language's operator set and also the set the parser accepts as
 * method names; one predicate keeps the parser, the duplicate-definition check, and lookups from
 * disagreeing.
 *
 * @param name  Method name.
 * @return      True for comparison operators, arithmetic operators, and the stream operators
 *              `<<` and `>>`.
 */
 _Bool isOverloadableOp (const char *name);
/** @brief Is `op` one of the compound assignment forms (`+=` `-=` `*=` `/=` `%=`)?
 *
 *  @param op  Operator spelling.
 *  @return    true when it is a compound form.
 */
_Bool isCompoundOp     (const char *op);
/**
 * @brief Tell whether a name is an overloadable arithmetic operator.
 *
 * `-` means binary subtraction only: extC has no unary operator overloading, so a method named `-`
 * is reached only by `a - b`.
 *
 * @param op  Operator name.
 * @return    True for one of `+`, `-`, `*`, `/`, `%`.
 */
 _Bool isArithOp (const char *op);
/**
 * @brief Tell whether a name is a logical operator.
 *
 * @param op  Operator name.
 * @return    True for `&&` or `||`.
 */
 _Bool isLogicOp (const char *op);
/**
 * @brief Tell whether the address of this expression may be taken.
 *
 * A place is a binding, a field, `*p`, an element of a fixed-size array, or an element of a view
 * whose storage the current frame owns (`new T[n]` taken here). A container-owned view is refused:
 * growing the container resizes or replaces the plate behind it.
 *
 * @param c  Checker; storage provenance is looked up through it.
 * @param e  Expression.
 * @return   True when the expression is a place a reference may point at.
 */
 _Bool isLvalue (Checker *c, Expr *e);
/**
 * @brief Tell whether this expression names storage the current frame owns.
 *
 * @param c  Checker.
 * @param e  Expression.
 * @return   True for `new T[n]`, a slice of such a block, and a binding that holds one.
 */
 _Bool exprIsFrameStorage (Checker *c, Expr *e);
/**
 * @brief Tell whether a generated name is proven non-null at the current position.
 *
 * Non-null proofs follow bindings rather than the names written in the source, so the query uses
 * the binding's generated C name.
 *
 * @param c      Checker, which holds the proof table.
 * @param cname  The binding's generated C name.
 * @return       True when the proof is still valid; false when `cname` is NULL or was never
 *               proven.
 */
 _Bool isNarrowed (Checker *c, const char *cname);
/**
 * @brief Tell whether an expression is an integer or floating-point literal.
 *
 * @param e  Expression; must be non-NULL.
 * @return   True for `EX_INT` or `EX_FLOAT`, whose types are decided by context.
 */
 _Bool isNumericLit (Expr *e);
/**
 * @brief Tell whether an expression is a place.
 *
 * A place is a chain of bindings, fields, indexes, and slices. Slicing a fixed-size array requires
 * a place as its operand, because it takes the element's address; otherwise it would slice the
 * temporary holding the return value. Slicing a string literal is the exception: its bytes have
 * static storage duration.
 *
 * @param e  Expression.
 * @return   True when the chain is rooted at a binding and every step is a field, index, or
 *           slice; false otherwise.
 */
 _Bool isPlace (Expr *e);
/**
 * @brief Does copying this expression into a `@value` place have to build a new value?
 *
 * True when the destination is a `@value` struct with a `clone` method and the source is a **place**:
 * the source keeps its storage, so the copy calls that method instead of copying the bytes. A
 * temporary is a move and answers false. A true answer also records the plan bit and marks the hook
 * as used, so the call site needs no second decision.
 *
 * @param c    The checker state.
 * @param dst  The destination type.
 * @param src  The source expression.
 * @return     True when the caller must mark `src` for cloning.
 */
 bool ckValueCopyNeedsClone (Checker *c, Type *dst, Expr *src);
/**
 * @brief Tell whether a value of this type may be passed to `print` or `println`.
 *
 * Structs, generic instances, and arrays are printed recursively by compiler-generated `_debug`
 * helpers; a payload-free enum always has a name to print.
 *
 * @param t  Argument type.
 * @return   True when a way to print it exists; false when `t` is empty or of an unsupported type.
 */
 _Bool isPrintable (Type *t);
/**
 * @brief Tell whether a type is the prelude container with both a matching name and argument count.
 *
 * @param t      Type to classify.
 * @param name   Reserved container name, such as `option`, `result`.
 * @param nargs  Required number of type arguments.
 * @return       True when the declaration name of a generic struct or generic enum equals `name`
 *               and its type-argument count equals `nargs`.
 */
 _Bool isProtoType (Type *t, const char *name, size_t nargs);
/**
 * @brief Tell whether this expression may be written through.
 *
 * Write permission has three sources: bindings follow `var` versus `let`, parameters follow
 * `mut ref` versus `ref`, and fields and elements follow their root. `*p` is judged separately,
 * with permission coming from the reference type itself; a read-only view also blocks writes.
 *
 * @param c  Checker.
 * @param e  Place expression; NULL is not writable.
 * @return   True when the write is allowed; false when the path crosses a read-only reference, the
 *           view is read-only, or the root is a `let`.
 */
 _Bool isWritablePlace (Checker *c, Expr *e);
/**
 * @brief Tell whether a literal's value fits the destination type.
 *
 * A literal has no type of its own until context assigns one, so `3.14` becomes `f32` in an `f32`
 * context and `200` becomes `u8` in a `u8` context. An unsigned destination rejects a negative
 * integer literal; the error type accepts any literal so that one diagnostic does not cascade into
 * more.
 *
 * @param e     Literal: `EX_INT`, `EX_FLOAT`, or `EX_BOOL`.
 * @param want  Type the context expects.
 * @return      True when the literal's value is representable in `want`.
 */
 _Bool literalFits (Expr *e, Type *want);
/**
 * @brief Tell whether the name a struct literal writes refers to the struct behind the context
 *        type.
 *
 * @param sd       Struct behind the context type.
 * @param written  Name written in the literal.
 * @return         True when they are the same struct.
 */
_Bool structLitNameMatches (const struct StructDef *sd, const char *written);
/**
 * @brief Run one S1 element-chain fusion pass over a function body (function bodies do not go
 *        through `checkBlockBody`).
 *
 * @param c     Checker.
 * @param body  Function body block statement.
 */
void checkFuseFuncBody (Checker *c, Stmt *body);
/**
 * @brief Hoist `?` nested in a statement's value positions into temporary declarations, leaving
 *        `*pi` pointing at the original statement.
 *
 * The expansion of `e?` is "evaluate, test for null, return on failure", and C has no statement
 * expressions, so it can only land where a value position can be rewritten. Here `e?` in a
 * position that is **always evaluated** is hoisted to `var __extc_tqN = e?` inserted before the
 * statement (same failure semantics as a hand-written temporary), and the original position is
 * replaced with a reference to the temporary. Positions where this would change the number or
 * timing of evaluations are left alone.
 *
 * @param c      Checker.
 * @param stmts  Statement table (inserted declarations are written into this table).
 * @param pi     Index of the current statement; on return it points at the original statement
 *               (the inserted declaration comes before it).
 */
void checkHoistStatementTrys (Checker *c, Vec *stmts, size_t *pi);

/**
 * @brief Tell whether a type mentions a type parameter directly or indirectly.
 *
 * @param t  Type to check.
 * @return   True when the type itself is `TY_PARAM` or contains a type parameter inside.
 */
 _Bool mentionsParam (Type *t);
/**
 * @brief Tell whether every reference crossed on the path to this place is `mut ref`.
 *
 * There are only two ways to cross: explicitly through `*p`, or implicitly when `p` in `p.f` or
 * `v[i]` is a reference, landing in the storage it points at. The place's own type does not count,
 * so retargeting a binding is still writable.
 *
 * @param e        Place expression.
 * @param crossed  Out parameter: set to true when the storage lies behind a crossed reference.
 * @return         True when every crossed reference is `mut ref`; false on a read-only reference.
 */
 _Bool pathRefsAllMut (Expr *e, _Bool *crossed);
/**
 * @brief Tell whether the path to this place crosses or lands on a read-only reference.
 *
 * Not the same question as `pathRefsAllMut`: this one also counts the place's own type, as when
 * `c` in `c.bump()` is a `ref counter`. Both answers are needed because the diagnostics are worded
 * differently.
 *
 * @param e  Place expression.
 * @return   True when the place's own type or any reference on the path is read-only.
 */
 _Bool pathHasReadonlyRef (Expr *e);
/**
 * @brief Reject dereferencing a nullable reference without a non-null proof.
 *
 * A nullable reference cannot be dereferenced directly; it must first be compared against null, so
 * the proof lives in the syntax rather than in inference.
 *
 * @param c     Checker.
 * @param t     Type being dereferenced.
 * @param node  That expression, used for the line number.
 * @param what  The operation being checked; appears in the diagnostic.
 * @return      True when `t` is a nullable reference (an error is reported); false otherwise.
 */
 _Bool rejectNullableDeref (Checker *c, Type *t, Expr *node, const char *what);
/**
 * @brief Tell whether re-evaluating this expression is unobservable.
 *
 * The body of the conditional expression generated by `??` appears twice, so a body with side
 * effects would run twice. The test is a conservative syntactic one: any expression containing a
 * call, `?`, or `??` is rejected rather than reasoning about purity.
 *
 * @param e  Expression to check.
 * @return   True when the expression consists only of literals, bindings, fields, indexes, slices,
 *           and unary/binary operations.
 */
 _Bool repeatablePure (Expr *e);
/**
 * @brief Check that a place may be written.
 *
 * Each of the three causes has its own diagnostic: writing `*p` needs `mut ref`; a path crossing a
 * read-only reference or read-only view needs `mut` in its type; reassigning a `let` binding is
 * forbidden outright.
 *
 * @param c     Checker.
 * @param e     Place being written.
 * @param line  Source line number, for diagnostics.
 * @param what  The operation being checked; appears in the diagnostic.
 * @return      True when the write is rejected and the error has been reported; false when it is
 *              allowed.
 */
 _Bool requireMutable (Checker *c, Expr *e, int line, const char *what);
/**
 * @brief Tell whether a type may carry a reference.
 *
 * A reference counts directly, and so does one reached through an array element, the payload of
 * any variant, or a struct field. Recursion is depth-limited and answers "yes" once the limit is
 * exceeded, conservatively. Generic instances are handled by substituting the type arguments
 * first; otherwise `T` looks harmless and the check would let a reference through.
 *
 * @param tt  Type table, for substituting the arguments of generic instances.
 * @param t   Type to check; NULL carries no reference.
 * @return    True when any part of the type can hold a reference.
 */
 _Bool typeContainsRef (TypeTable *tt, Type *t);

/** @brief Tell whether a type is, or contains, a `@noCopy` struct (boxing would copy an identity). */
 _Bool typeHoldsNoCopy (TypeTable *tt, Type *t);
/** @brief Tell whether a type owns every piece of storage it points at (containers do). */
 _Bool typeOwnsStorage (TypeTable *tt, Type *t);
/**
 * @brief Tell whether a type may hold a **counted rc reference** (`typeContainsRef` without the
 *        view-handle layer).
 *
 * One difference from `typeContainsRef`: the **view handle itself does not count** (`data` of
 * `slice<T>` is not a counted rc slot; the storage a view points at is managed by its slab, pool,
 * or region), but a reference inside the view's type argument still counts. Region acyclicity rule
 * (iv) uses this so that a shape found everywhere, such as "print one line of a string", is not
 * treated as holding a heap reference.
 *
 * @param tt  Type table.
 * @param t   Type to check; NULL holds no reference.
 * @return    True when the type can hold a counted rc reference.
 */
 _Bool typeHoldsCountedRef (TypeTable *tt, Type *t);
/**
 * @brief Tell whether a type is a function type or contains one internally.
 *
 * Same walk family as `typeContainsRef`; it exists for the wording of one diagnostic:
 * `fn(i64) -> i64` has no zero value, but not because of a reference. Recursion is depth-limited
 * and answers "yes" once the limit is exceeded, conservatively.
 *
 * @param tt  Type table, for substituting the arguments of generic instances.
 * @param t   Type to check; NULL contains no function type.
 * @return    True when the type itself is TY_FN, or an array, variant payload, or struct field
 *            holds one.
 */
 _Bool typeContainsFn (TypeTable *tt, Type *t);
/**
 * @brief Tell whether a type has no zero value.
 *
 * A non-null reference and a function type both lack one (zeroing the latter yields a null function
 * pointer), and so does an aggregate containing either. An enum with payloads only looks at its
 * first variant, because its zero value is tag 0 with the payload zeroed, so variant order matters.
 * Recursion is depth-limited and answers "yes" once the limit is exceeded, conservatively.
 *
 * @param tt  Type table, for substituting the arguments of generic instances.
 * @param t   Type to check.
 * @return    True when zeroing this type would produce a null reference or null function pointer.
 */
 _Bool typeLacksZeroValue (TypeTable *tt, Type *t);
/**
 * @brief Tell whether dropping a value of this type runs a hook.
 *
 * True when the type has its own `@drop` hook or one of its fields does. The code generator has a
 * twin of this walk (`typeNeedsDrop`) and the two must agree: this one decides whether an element
 * store is legal, the other decides whether a drop call is emitted.
 *
 * @param c      Checker (for the instantiation context).
 * @param t      Type to check; may be NULL.
 * @param depth  Nesting already walked, capped against a pathological type.
 * @return       True when a drop has an effect.
 */
bool typeHasDrop (Checker *c, Type *t, int depth);
/**
 * @brief Tell whether a value of this type may take part in an operation with operator `op`.
 *
 * Native rules are decided per family: `==` and `!=` are native for numbers, `bool`, and
 * payload-free enums; ordering comparisons are native for numbers only; arithmetic is native for
 * numbers, with `%` for integers only. Array `==` is derived by the compiler, but requires the
 * right operand to be the same array type and the elements to be comparable; everything else looks
 * up an operator method matching the operand type exactly.
 *
 * @param tt   Type table, for substituting the arguments of a generic receiver.
 * @param t    Type the operator acts on.
 * @param op   Operator name, such as `==`, `<`, `+`, or `<<`.
 * @param rhs  Type of the right operand.
 * @return     True when the operation is native, or when the type defines a matching operator
 *             method.
 */
 _Bool typeSupportsOp (TypeTable *tt, Type *t, const char *op, Type *rhs);
/**
 * @brief Give a source name the name it will have in the generated C.
 *
 * A `let` only names a value rather than allocating storage, so a second `let` with the same name
 * in one scope is renamed in the generated C while the source name stays as written. Suffixes
 * increase monotonically within a function and are never reused; module-level names and C keywords
 * must be avoided, or a local would shadow a function name or keyword in the generated C. The
 * returned string lives in the checker's arena.
 *
 * @param c     Checker, which holds the per-function name-use counts.
 * @param name  Name as written in the source.
 * @return      Name to use in the generated C; the original name on first use.
 */
 const char *cNameFor (Checker *c, const char *name);
/**
 * @brief Fetch the binding a condition proves non-null.
 *
 * `p != null` holds in the true branch and `p == null` holds in the false branch; the latter is
 * what turns `if p == null { return }` into a guard. A field path `h.p` only holds while the root
 * is a local whose address was not taken, since otherwise an alias could change it.
 *
 * @param c         Checker.
 * @param cond      Condition expression to inspect.
 * @param whenTrue  Out parameter: set true when the proof holds in the true branch, false when it
 *                  holds in the false branch.
 * @return          The binding's generated C name, or the field-path key `root.field`; NULL when
 *                  the condition proves nothing.
 */
 const char *narrowTarget (Checker *c, Expr *cond, _Bool *whenTrue);
/**
 * @brief Render a type as the source text the user writes, for diagnostics.
 *
 * A lambda's type is a generated closure struct while the user wrote a signature, so diagnostics
 * show the signature.
 *
 * @param c  Checker; the text is allocated in its arena.
 * @param t  Type to render; NULL renders as `void`.
 * @return   NUL-terminated string that stays valid for the rest of the run.
 */
 const char *typeStr (Checker *c, Type *t);
/* Check a whole module: the declarations, the function bodies, the deferred rules, and the arena
 * decisions that wait for closed effect summaries. */
/**
 * @brief Tell whether a local name is in the escape set.
 *
 * The escape set E is an over-approximation of the local names that may leave this frame, closed to
 * a fixpoint; one extra name only costs memory, one missing name leaves a dangling pointer.
 *
 * @param c  Checker; the escape set is its `escapees`.
 * @param n  Binding name.
 * @return   True when the name is in the set; false when `n` is NULL or absent.
 */
 bool isEscapeeName (Checker *, const char *);
/**
 * @brief Transitively close a function's effect summary.
 *
 * A summary is computed and cached only when actually needed; a function whose summary is being
 * computed means a cycle, and it is marked incomplete; an unresolvable call keeps the summary
 * incomplete forever. A summary collected from a signature for an extern declaration counts as
 * complete and does not become empty for lack of a body. Only a summary that returns true may be
 * trusted to skip checks.
 *
 * @param c  Checker.
 * @param f  Function to close and cache; NULL is unusable.
 * @return   True when the summary is complete; false when there is a cycle, an unresolved call, or
 *           an incomplete callee.
 */
 bool computeEffectsTransitive (Checker *, FuncDef *);
/**
 * @brief Fetch the depth slot of a field in a binding's field table.
 *
 * The table grows on demand; `otherDepth` is the conservative fallback for writes that cannot be
 * attributed to a named field (element writes and whole-value writes). Slot names come from the
 * AST and outlive the binding record, so the pointers cannot dangle.
 *
 * @param c       Checker; unused, kept for the checker-parameter convention.
 * @param s       Binding to look up.
 * @param field   Field name.
 * @param create  Whether to create the slot when it is missing.
 * @return        Pointer to the field's depth slot; NULL when `create` is false or the binding is
 *                NULL.
 */
 int *fieldDepthEntry (Checker *, Sym *, const char *, bool);
/**
 * @brief Path key for writing and reading field slots (the whole field chain from the root binding
 *        to this projection).
 *
 * The key string is built in the checker's arena: `bx.mid.next` is recorded as `mid.next`, and a
 * constant element index is written into the name (`arr[0]` becomes `[0]`). Reads and writes of
 * the field table must use the same key, or the source of a two-level projection cannot be
 * recorded and cannot be promoted.
 *
 * @param c  Checker.
 * @param e  Projection expression (`EX_FIELD` / `EX_INDEX` chain).
 * @return   The path key; NULL when it cannot be named (a dereference or slice step is included).
 */
 const char *fieldSlotKey (Checker *, Expr *);
/**
 * @brief Record the depth of one write to a binding's field or element.
 *
 * A non-empty field name updates its slot; a binding whose address was taken may only take the
 * maximum, a complete field table may be updated strongly, and an incomplete table forbids any
 * decrease. Element writes and whole-value writes go through `otherDepth`; the depth a reference
 * binding itself points at may only rise, so that "what I point at" is not overwritten by "what is
 * inside the object I point at".
 *
 * @param c      Checker; the written expression is held on `c->curStoreVal`.
 * @param root   Binding whose field table is updated; may be NULL.
 * @param field  Field name; NULL means a whole-value write or an element write.
 * @param d2     Depth of the value being written.
 */
 void noteFieldDepthWrite (Checker *, Sym *, const char *, int);
/**
 * @brief Record a whole-value assignment and keep its per-field sources.
 *
 * When the destination's address was not taken, the source is a named binding, and its field table
 * is complete, the whole field table is copied over, so every field's allocation site is still
 * reachable from the container; otherwise this degrades to a conservative whole-value update.
 *
 * @param c      Checker.
 * @param dst    Destination binding.
 * @param value  Source expression.
 * @param d2     Depth of this write.
 */
 void noteWholeValueDepthWrite (Checker *, Sym *, Expr *, int);
/**
 * @brief Fetch the identifier name at the root of a place expression.
 *
 * Walks to the root identifier through fields, indexes, and dereferences only; it neither resolves
 * names nor consults scopes, so unresolved nodes still work. That is the key difference from
 * `placeRoot`.
 *
 * @param e  Place expression; may be NULL.
 * @return   The root identifier's name; NULL when the expression is not such a chain.
 */
 const char *placeRootName (Expr *);
/**
 * @brief Compute the arena depth a call passes, from its `mut ref` arguments.
 *
 * Whatever the callee allocates must live at least as long as the object it may be stored into, so
 * this takes the depth of the object behind the shallowest `mut ref` argument. When that argument
 * is this function's parameter or a global (depth 0), or a local in the escape set, the caller's
 * home arena is passed instead. The answer depends on the escape set, so the site is recorded into
 * `eSites` and recomputed by the last pass after summaries close.
 *
 * @param c         Checker.
 * @param args      Argument expressions in call order.
 * @param params    Declared parameters of the callee, in the same order.
 * @param callNode  Call expression; NULL means no site is recorded.
 * @return          Arena depth to pass; 0 when there is no `mut ref` argument, -1 when the
 *                  caller's home arena is passed.
 */
  int callHomeDepth (Checker *c, Vec *args, Vec *params, Expr *callNode);
/**
 * @brief Compute the reference depth of the value a store records.
 *
 * Takes the larger of the reference depth and the structural upper bound, so a value carrying a
 * live pointer is never underestimated. The structural upper bound is consulted only when the type
 * may carry a reference, so a pure value copy is not constrained by the lifetime of the source
 * object and storing bytes in a container does not drag an allocation site into the caller's arena.
 *
 * @param c  Checker.
 * @param e  Value being stored.
 * @return   Upper bound on the reference depth carried by the stored bytes.
 */
int valDepthForStore (Checker *, Expr *);
/**
 * @brief Compute how deep the storage a value's references point at is.
 *
 * Smaller numbers live longer; 0 means a parameter or a global, that is, outside this frame. That
 * number is what the reference rules compare against the destination's store depth, so every
 * approximation must err large: too large only rejects a valid program, too small lets a dangling
 * reference through. The result may be cached on the node, except for nodes whose type mentions a
 * type parameter.
 *
 * @param c  Checker.
 * @param e  Expression to check.
 * @return   Depth of the references in the value; 0 when the type cannot carry a reference.
 */
 int targetDepth (Checker *c, Expr *e);
/**
 * @brief Compute the depth of the storage a place expression refers to.
 *
 * A local gets its block depth; a parameter or global gets 0 (outside this frame); a reference
 * binding gets the depth of the object it points at rather than the depth of its own slot; `*p`
 * creates no storage and takes the reference's own depth. A shape whose root cannot be named falls
 * back to `targetDepth`, so the fallback lands on the longer-lived side instead of recording
 * depth 0.
 *
 * @param c  Checker, which provides the scope stack and the type table.
 * @param e  Place expression.
 * @return   Lexical depth of the storage; 0 when `e` is NULL.
 */
 int slotDepth (Checker *c, Expr *e);
/**
 * @brief Tell whether an expression's storage is provably outside every frame.
 *
 * Only two sources qualify: a name chain rooted at a global (static storage outlives every frame),
 * and a call whose declaration carries `effects Ret=0` ("the reference I return does not point into
 * the caller's frame"). Everything else answers no, keeping the conservative rule: `new`, arena
 * allocations, stack arrays, and callees without such a declaration may all disappear with the
 * frame.
 *
 * @param c  Checker.
 * @param e  Initializer or right-hand side of a binding.
 * @return   True when the storage definitely outlives this frame; false otherwise.
 */
 bool exprOutOfFrame (Checker *c, Expr *e);
/**
 * @brief Tell whether an initializer expression is a constant expression.
 *
 * Only the shape matters: literals, negation, arithmetic between literals, enum constants, struct
 * literals whose fields are all constant, function addresses, `null`, and conversions that need no
 * run-time check all count as constant.
 *
 * @param e  Initializer expression to classify; may be NULL.
 * @return   True for a constant expression; false for NULL and every other shape.
 */
 bool isConstInit (Expr *e);
/**
 * @brief Find the arena layer of the storage a write target names.
 *
 * Not the same question as `slotDepth`: this asks how long the storage itself lives, not how long
 * the object a reference points at lives.
 *
 * @param c  Checker.
 * @param e  Place expression of the write target.
 * @return   Layer the storage is in: a local binding gets the depth of its block, a copy-in
 *           parameter gets 1, a global gets 0; a projection from a reference or a parameter falls
 *           back to `slotDepth`'s answer.
 */
 int storeLayer (Checker *c, Expr *e);
/**
 * @brief Tell whether a value's root comes from one of a function's parameters.
 *
 * If so, the lifetime obligation passes to the call site, which knows how long the argument lives.
 *
 * @param c    Checker; unused here, kept for shape consistency with the sibling predicates.
 * @param f    Function definition providing the parameter list.
 * @param val  Value to trace.
 * @return     True when the value's root is a parameter name of `f`; false when the value is NULL,
 *             the function is NULL, or the root cannot be named.
 */
 _Bool valTracesToParam (Checker *c, FuncDef *f, Expr *val);
/**
 * @brief Promote every allocation site inside a value to the level it must reach.
 *
 * Walks only the value's carriers (bindings, `!`, `??`, struct literals, enum payloads, array
 * literals); it does not chase aliases or reason across functions, and stops after 32 steps.
 * Promotion only lengthens a lifetime, so it never turns an unsafe store into an accepted one.
 *
 * @param c    Checker.
 * @param val  Value being stored or returned.
 * @param at   Level the value must reach; 0 means it must outlive this frame.
 * @return     True when every allocation site in the value reached `at`; false when some carrier
 *             cannot be promoted, in which case the caller falls back to the depth rule and
 *             reports the error.
 */
 _Bool promoteInto (Checker *c, Expr *val, int at);
/**
 * @brief Record the source of a binding's value and flatten the carrier chain to its root.
 *
 * The source must be flattened: a binding in the middle of the chain may already be out of scope by
 * the time the chain is traced again. A binding whose address was taken is not recorded.
 *
 * @param c    Checker.
 * @param sy   Binding being assigned or initialized.
 * @param val  Expression to record as its source.
 */
 void noteOrigin (Checker *c, Sym *sy, Expr *val);
/**
 * @brief Find a value's root source (flattening the binding chain to the root expression).
 *
 * The sources written into the field table are flattened this way: a middle binding may be out of
 * scope by the time promotion runs, so `lookup` would fail, the source chain would break, and the
 * site could not be promoted. A root expression (such as `new`) has no scope problem.
 *
 * @param c     Checker.
 * @param val   Expression to find the source of.
 * @param hops  Binding steps already walked; stops once the limit is exceeded.
 * @return      The root expression; `val` itself when there is no source to trace.
 */
 Expr *originOf (Checker *c, Expr *val, int hops);
/**
 * @brief Give the type the context expects to expressions that cannot state their own.
 *
 * Only bare `null`, array literals, and unqualified struct literals are handled, and only when the
 * context determines the type uniquely; `null` is accepted only in a nullable-reference context,
 * since it has no way to know the target type.
 *
 * @param e     Expression to fill in the type of.
 * @param want  Type the context expects.
 */
 void adoptContextType (Expr *e, Type *want);
/**
 * @brief Check that a call's reference arguments outlive the call, and fix the arena level from
 *        that.
 *
 * Returns immediately when the argument and parameter counts differ. An extern callee is a black
 * box on the C side: the signature's effect bits are read position by position to decide which
 * parameters may be saved, and a saved reference argument must live to level 0. A callee with a
 * body first gets its effects closed, then its address flow and its content flow are judged
 * separately, each deciding whether to run; a promotable value is promoted first and only an
 * unpromotable one is an error. A `mut ref` argument grants write permission, and its non-null
 * proof is invalidated here.
 *
 * @param c           Checker.
 * @param callee      Callee definition; NULL means the callee is unresolved and treated
 *                    conservatively.
 * @param args        Argument expression table.
 * @param params      Callee's parameter table, in the same order as `args`.
 * @param homeDepth   Arena level declared by the callee; less than or equal to 0 means none.
 * @param line        Source line for the diagnostic.
 * @param fname       Callee name, for the diagnostic.
 */
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
/**
 * @brief Check an operator method's signature at its definition.
 *
 * Only comparison and arithmetic operators in the overloadable set (including `<<` and `>>`) are
 * checked; every other name is let through. Exactly two parameters are required, and the first must
 * be a reference to the owning type; a comparison operator must return `bool`, an arithmetic
 * operator must return the owning type's value, and only `<<` and `>>` may return a reference to
 * that type.
 *
 * @param c  Checker.
 * @param f  Function definition to check.
 */
 void checkOperatorSig (Checker *c, FuncDef *f);
/**
 * @brief Check one statement and apply the depth and narrowing consequences it has.
 *
 * Clears this statement's side-effect counter on entry, then dispatches by kind: a declaration
 * resolves its annotations and registers bindings, an assignment goes through the write-point
 * entry, `if` / `while` record the facts their condition proves and check their branches, `match`
 * opens a scope per arm, and blocks and domain blocks each take their own scope path.
 *
 * @param c  Checker.
 * @param s  Statement to check.
 */
 void checkStmt (Checker *c, Stmt *s);
/**
 * @brief Re-check a worker body synthesized by `par` desugaring, rejecting what is outside the
 *        restricted set.
 *
 * The desugaring happens in the checker, and the synthesized worker goes through the ordinary
 * function check path, so it gets exactly the types and plan marks a hand-written worker gets. An
 * ordinary function may call, allocate, and return early; a worker may not -- this re-checks the
 * body **after** it has been checked, against its resolved shape: a form such as `io::cout << x`
 * only becomes a method call in the checker, and the shape at desugaring time does not reveal it.
 *
 * @param c  Checker.
 * @param f  Worker that has just been checked; the caller guarantees `planIsParWorker(f)`.
 */
 void parWorkerAudit (Checker *c, FuncDef *f);
/**
 * @brief Resolve the type names in a signature to type-table entries in place.
 *
 * Runs before any function body is checked, so later checks need not resolve type names again. A
 * method sees its owning type's type parameters, a free function sees its own, and type names are
 * resolved in the context of the file the declaration lives in. A `void` return type becomes NULL.
 *
 * @param c  Checker.
 * @param f  Function whose parameter and return types are resolved in place.
 */
 void resolveSignature (Checker *c, FuncDef *f);
/**
 * @brief Report a fatal compile error at one source line.
 *
 * The message is rendered into a fixed-size buffer and truncated rather than overflowed when too
 * long. Reporting follows "the first error wins": once the error flag is set, later errors are
 * dropped, so a cascade of diagnostics does not flood the user.
 *
 * @param c     Checker collecting the diagnostics.
 * @param line  Source line the diagnostic points at; 0 when the position is unknown.
 * @param note  Extra note printed under the message, or NULL.
 * @param fmt   printf format of the message.
 * @param ...   Format arguments for `fmt`.
 */
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
/**
 * @brief Report a warning at one source line.
 *
 * The only difference from reporting an error is that a warning does not set the error flag:
 * compilation continues and the exit status is unchanged.
 *
 * @param c     Checker issuing the warning.
 * @param line  Source line the warning points at.
 * @param note  Extra note printed under the message, or NULL.
 * @param fmt   printf format of the message.
 * @param ...   Format arguments for `fmt`.
 */
 void ckWarn  (Checker *c, int line, const char *note, const char *fmt, ...);
/**
 * @brief Resolve a compound assignment's operator and return the type of `target op value`.
 *
 * The statement stays as written and the operation goes through the same arithmetic check `x + y`
 * takes, so arithmetic rules, literal adaptation, reference rejection, and user-defined operators
 * each have exactly one answer. A user-defined operator takes the receiver as a reference, so the
 * generated call must name the target again; when the target contains a call this is an error that
 * suggests splitting it into two lines.
 *
 * @param c       Checker.
 * @param op      Compound operator as written; its first character is the binary operator.
 * @param target  Left-hand target, already checked.
 * @param value   Right-hand value, already checked.
 * @param tgt     Target type.
 * @param val     Value type.
 * @param out     Receives the resolved `EX_BIN` node.
 * @return        Type of `target op value`; the error type when the operator does not apply or the
 *                target contains a call.
 */
 Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                       Type *tgt, Type *val, Expr **out);
/**
 * @brief Rewrite a bare constructor name in place into a qualified variant construction.
 *
 * Only the four names `success` / `failure` / `some` / `none` are recognized, and the expected type
 * must be the matching `result` or `option` instance; a name the user defined as a function stays as
 * written. `none` is the only one whose parentheses may be omitted.
 *
 * @param c     Checker.
 * @param e     Expression to rewrite; left untouched when it is not such a shape.
 * @param want  Expected type, for telling whether the name is a known constructor.
 */
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
/**
 * @brief Require a condition expression to have type `bool`, reporting an error otherwise.
 *
 * extC has no implicit truthiness: an integer or reference used as a condition is reported rather
 * than converted.
 *
 * @param c     Checker.
 * @param t     Type of the condition.
 * @param node  Condition expression, used for the source position.
 */
 void expectBool (Checker *c, Type *t, Expr *node);

/* Mark the call as allocating into the caller's home arena when the value it produces escapes
 * the current frame. */
/* ---------------------------------------------------------------- Write point: one entry
 *
 * One write does four things at once, and the **order must not change**:
 *   1 `recordStore`  -- record that this value is published at level `at` (the level solver folds
 *                      on this)
 *   2 `promoteInto`  -- try to promote the allocation sites behind the value to that level (stay
 *                      conservative when that fails)
 *   3 `markCallHomeIfEscaping` -- the escape-set half
 *   4 field table    -- a whole-value write goes through `noteWholeValueDepthWrite`, a field or
 *                      element write through `noteFieldDepthWrite` (the latter records `src`
 *                      internally so that later promotion can find the site)
 *
 * Every write that can go through this entry does; only writes with genuinely different semantics
 * (return values, argument publication, proof invalidation `unNarrow`, fresh-set invalidation
 * `freshDrop`) combine the parts themselves, and each such call site states why. */
/* ---------------------------------------------------------------- Write-point invalidation
 *
 * A write does more than publish: it must also **invalidate the derived facts about the written
 * root**. Those facts live in two passes, so "unifying" them here can only mean "one concept, and
 * two implementations that name each other", not a single forced function:
 *
 *   | fact | who invalidates it | which pass it lives in | consequence of not invalidating |
 *   |---|---|---|---|
 *   | non-null proof (`?ref T` proven non-null, including the field path `h.p`) | `unNarrow(c, cname)` | check time (the `Checker.narrow` stack) | after a `mut ref` method call the proof survives => the generated code dereferences NULL |
 *   | fresh set (this name holds a frame-local allocation) | `freshDrop(fresh, name)` | summary time (a local `Vec` used by the summary) | after a retarget the set survives => the summary calls it "naturally safe" => the whole call-site check is skipped |
 *
 * The two **trigger conditions differ** and are written at their own definitions; only their common
 * shape is recorded here: "make this root, and every path through it, stop holding any fact that
 * still stands".
 * When a new fact of this kind is added, add a row to this table and trigger it at the **same
 * write point**, so that "write" and "invalidation" appear as a pair. */

/**
 * @brief Single entry point for a write: publication, promotion, escape marking, and the field
 *        table in a fixed order.
 *
 * The order must not change: `recordStore` comes before `promoteInto` so the solver sees the
 * requirement first, and the field table comes after promotion because promoting the field table
 * reads it. A whole-value write goes through `noteWholeValueDepthWrite`, a field or element write
 * through `noteFieldDepthWrite`; a write that involves no reference returns directly.
 *
 * @param c       Checker.
 * @param value   Value being written.
 * @param target  Target place; with no target, only publication and promotion are done.
 * @param at      Arena level of the target; 0 means it must outlive this frame.
 * @param line    Source line, for diagnostics.
 */
 void noteStore (Checker *c, Expr *value, Expr *target, int at, int line);
/**
 * @brief Publish a return value at level 0.
 *
 * The target is the value itself: anything handed back to the caller is published as "outlives this
 * frame". The two return sites share this half and differ in their later steps.
 *
 * @param c      Checker.
 * @param value  Value being returned.
 * @param line   Source line, for diagnostics.
 */
 void noteReturnPublish (Checker *c, Expr *value, int line);
/**
 * @brief Record which parameter a returned view borrows its storage from.
 *
 * The call site reads the record to give the result the depth of that storage rather than the
 * depth of the call's own block. A body whose `return` statements name different sources keeps no
 * single answer, and the call site then keeps the conservative rule.
 *
 * @param c      Checker.
 * @param value  Returned value.
 */
 void noteReturnBorrow (Checker *c, Expr *value);

/**
 * @brief When a call's result escapes the current block, mark the call as using the caller's home
 *        arena.
 *
 * Handles only calls with a concrete callee that needs a home arena. When the destination is
 * shallower than the current scope (`at` less than the scope count), `homeDepth` becomes -1;
 * otherwise it becomes the current block's depth. `setCallArenaArg` is then called right away so that
 * `homeDepth` and the arena actually passed stay in sync.
 *
 * @param c   Checker, which reads the scope stack.
 * @param v   Call expression whose result is stored; other shapes are ignored.
 * @param at  Scope depth of the place receiving the result; 0 means the result is returned.
 */
 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
/**
 * @brief Resolve a call's `homeDepth` into the arena argument it actually passes.
 *
 * `homeDepth` -1 passes the home arena; 1 or more passes the corresponding block's arena;
 * otherwise no `mut ref` argument justifies it, so the current block is recorded and
 * `arenaArgPending` is set, to be decided by the last pass after every function body has been
 * checked. codegen reads only what is written here.
 *
 * @param c  Checker, which reads the scope stack.
 * @param e  Call expression whose `homeDepth` is already set.
 */
 void setCallArenaArg (Checker *c, Expr *e);

/**
 * @brief Decide which zone level this pool-creating call uses.
 *
 * Inside a library function (`modName` non-empty) the library is transparent about places and the
 * choice goes to the caller (`ZONE_HOME`); a function in the entry file uses the current block by
 * default. It only lowers and never raises: the same node may be checked more than once, and
 * promotion may already have lowered this cell.
 *
 * @param c  Checker, which reads the current function and the scope stack.
 * @param e  Call expression.
 */
 void setCallZoneArg (Checker *c, Expr *e);

/**
 * @brief Tell whether a callee's result may live in a place.
 *
 * This asks about the type's nature: a closure that resolves to `makesPool` counts, and the `new` /
 * `withCap` / `withParent` associated-function family relaxes through a mark on the struct. A
 * generic method's template and its instance are two `FuncDef`s, so that mark is what carries the
 * answer from one to the other.
 *
 * @param f  Callee definition; NULL counts as no.
 * @return   True when the closure finds that the function makes a place, or it is an associated
 *           function of a place-making type; false otherwise.
 */
 bool calleeMakesPool (FuncDef *f);
/* Place sites are decided without a name table: the closure answers `makesPool`, and the
 * `new` / `withCap` / `withParent` family relaxes through a mark on the struct. An unresolved
 * callee is not a site -- an `extern!` helper, a builtin primitive, or an associated function on a
 * type parameter cannot build a place. */

/**
 * @brief Tell whether this call may land its result in a place.
 *
 * A different question from "may the result live in a place": this asks only whether the callee
 * side creates one, for the block-level zone hook. An unresolved callee returns false: what stays
 * unresolved is an `extern!` helper, a builtin primitive, or an associated function on a type
 * parameter, and none of them can build a place.
 *
 * @param f  Callee definition; NULL (unresolved) returns false.
 * @return   True when the closure finds that the function makes a place, or it is an associated
 *           function of a place-making type; false otherwise.
 */
bool calleeCreatesPool (FuncDef *f);

/**
 * @brief Install the hook that resolves a callee from an expression.
 *
 * A protocol method on a type parameter inside a generic body deliberately records no callee on the
 * node (one template body, many instances); the hook is called with the type arguments substituted
 * during the instance round to find it again. Without the hook the answer stays conservative.
 *
 * @param fn  Resolver; NULL removes the hook.
 */
 void setPoolCalleeResolver (FuncDef *(*fn)(Expr *e));

/**
 * @brief Record the level fact "this value must be placed into a certain level".
 *
 * Recorded on the site rather than on the binding: the replay runs after every function body has
 * been checked, and local bindings cannot be looked up by then. An identifier value is
 * replaced by its source site or its pool-creation site. Nothing is recorded while the level solve
 * runs, so the fact table cannot grow without bound.
 *
 * @param c    Checker; the record is appended to `c->lvlFacts`.
 * @param val  Value being stored or returned.
 * @param at   Level the value must be placed into; negative values are ignored.
 */
 void recordLvlFact (Checker *c, Expr *val, int at);
/**
 * @brief Record a call site whose arguments the callee may store (`check_lvl.c`).
 *
 * Verification only: returns NULL when `EXTC_DBG_LVL` is unset, so the recording costs one
 * branch in a normal run. The argument vector is copied into the arena, because a method
 * call passes a temporary `Vec` of its own.
 *
 * @param c      Checker.
 * @param args   Argument vector, in parameter order.
 * @param h      Place level the callee may store into.
 * @param line   Source line of the call.
 * @param fname  Callee name, for the report.
 * @return       The recorded site, or NULL when the analysis is off.
 */
 LvlSite *lvlSiteNew (Checker *c, Vec *args, int h, int line, const char *fname);
/**
 * @brief Record what the existing checker decided about one argument of a call site.
 *
 * Decision codes are ranked (`none < skip < accept < reject`), so a rejection is never
 * overwritten by an acceptance.
 *
 * @param s     Site returned by `lvlSiteNew`; NULL does nothing.
 * @param j     Argument index.
 * @param what  One of `LVL_CAND_NONE`, `LVL_CAND_ACCEPT`, `LVL_CAND_REJECT`, `LVL_CAND_SKIP`.
 */
 void lvlSiteMark (LvlSite *s, size_t j, unsigned char what);
/**
 * @brief Record what an argument's expression *is* at a call site.
 *
 * A destination argument (`ref *a`, a `mut ref` parameter) asks "does the place it points
 * into live long enough"; a value argument asks "do the bytes being stored live long
 * enough". The two are reported apart because only the second is a store of the argument's
 * own bytes.
 *
 * @param s     Site returned by `lvlSiteNew`; NULL does nothing.
 * @param j     Argument index.
 * @param role  `LVL_ROLE_VALUE` or `LVL_ROLE_DEST`.
 */
 void lvlSiteRole (LvlSite *s, size_t j, unsigned char role);
/**
 * @brief Run the verification-only place-level data flow over one checked body.
 *
 * Returns immediately unless `EXTC_DBG_LVL` is set; it reads the tree, prints disagreements
 * to stderr, and never writes a diagnostic, a plan bit, or a generated byte.
 *
 * @param c  Checker.
 * @param f  Function whose body has just been checked.
 */
 void lvlAnalyze (Checker *c, FuncDef *f);
/**
 * @brief Check a constructor's signature at its definition.
 *
 * Only the associated function of the owning type named `new` is checked. The return type must be
 * the owning type itself, or `result<T, E>` whose first type argument is the owning type (a fallible
 * constructor); otherwise the error is reported at the definition, even if nobody calls it yet.
 *
 * @param c  Checker.
 * @param f  Function definition to check.
 */
 void checkCtorSig (Checker *c, FuncDef *f);

/**
 * @brief Report using another module's name without the module qualifier.
 *
 * The same file, the current module, and the auto-imported prelude are exempt; a name brought into
 * the current module scope by `use mod::*` or `use mod::{name}` is let through too. `@private` is
 * still enforced at the declaration, and opening a module is not a way around it.
 *
 * @param c          Checker.
 * @param what       Name being referenced.
 * @param whatMod    Module that owns the name.
 * @param qualified  Whether the reference came from a qualified name; qualified names are rewritten
 *                   into flat names at load time, so this flag is the only trace of how the
 *                   reference was written.
 * @param line       Source line for the error.
 */
 void requireQualified (Checker *c, const char *what, const char *whatMod, bool qualified, int line);
/**
 * @brief Tell whether an import brought a name into the current module's scope.
 *
 * A wildcard entry covers every public name of the module; a listed entry covers only the one it
 * names. The comparison is on the module-name pair the loader recorded, so a `use` in one file does
 * not leak into another file. Scans the `opens` table linearly.
 *
 * @param m         Merged module whose `opens` table the loader filled in.
 * @param importer  Module doing the reference; NULL for the root file.
 * @param opened    Module that owns the name.
 * @param what      Name to look for.
 * @return          True when the module brought the name in with `use opened::*` or
 *                  `use opened::{what}`; false otherwise, including when `m` is NULL.
 */
 bool nameBrought (Module *m, const char *importer, const char *opened, const char *what);
/**
 * @brief Fetch the type parameter table visible to a function.
 *
 * A method defined in a struct sees its owning type's type parameters, a free function sees its own;
 * both answers come from here.
 *
 * @param f  Function or method to inspect; may be NULL.
 * @return   The function's own type parameters, or its owning type's; NULL when it is not generic.
 */
 Vec *funcTParams (FuncDef *f);
/**
 * @brief Fetch a concrete instance of a generic free function, creating one when absent.
 *
 * One template plus one set of type arguments maps to exactly one instance; an existing match is
 * returned. A new instance is a shallow copy of the template, points back at it, has its type
 * arguments filled in and its C name adapted to them, its parameter and return types substituted one
 * by one, and is appended to the checker's instance table and the module's function table. It
 * reports an internal error and returns NULL when the type arguments nest too deeply or the total
 * instance count exceeds the limit.
 *
 * @param c      Checker; the instance is appended to `c->funcInsts` and the module's function table.
 * @param tmpl   Generic template; NULL returns NULL.
 * @param targs  Concrete type arguments.
 * @param line   Source line of the call site, for the limit diagnostic.
 * @return       A standalone `FuncDef` that codegen emits like an ordinary function; NULL when the
 *               template is empty or a limit is hit.
 */
 FuncDef *funcInstance (Checker *c, FuncDef *tmpl, Vec *targs, int line);
/**
 * @brief Unify parameter types with argument types, filling the type arguments into `targs`.
 *
 * A type parameter fills its slot directly, and an already-filled slot must equal the new type;
 * references and generic instances are descended level by level. A shape mismatch that still
 * mentions a type parameter counts as inference failure.
 *
 * @param tt     Type table.
 * @param tp     Type parameter names, in the same order as `targs`.
 * @param targs  One slot per type parameter, filled in during unification.
 * @param want   Parameter type that may mention type parameters.
 * @param got    Argument type.
 * @return       False when inference fails and the call site must spell out the type arguments;
 *               true otherwise.
 */
 _Bool unifyTParams (TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got);
/**
 * @brief Record every non-null fact a condition proves.
 *
 * Both sides of `&&` are walked: reaching the branch where the condition holds proves both sides.
 * Only proofs for the branch where it is true are recorded.
 *
 * @param c     Checker.
 * @param cond  Condition to take facts from; NULL is ignored.
 */
 void narrowFactsOf (Checker *c, Expr *cond);
/**
 * @brief Leave the innermost lexical scope, dropping its bindings and revoking the non-null facts it
 *        established.
 *
 * The narrowing table is truncated back to the water mark recorded when the scope was entered, so
 * narrowing is lexically scoped like every other binding.
 *
 * @param c  Checker.
 */
 void popScope (Checker *c);
/**
 * @brief Record that a binding is proven non-null at the current position.
 *
 * The binding's generated C name is recorded rather than the name written in the source, because
 * narrowing follows bindings. Recording the same name twice changes nothing.
 *
 * @param c      Checker.
 * @param cname  The binding's generated C name; NULL is ignored.
 */
 void pushNarrow (Checker *c, const char *cname);
/**
 * @brief Enter a new lexical scope.
 *
 * Also records the current narrowing-table length as a water mark, which restores the table when the
 * block ends: a nullable-reference fact proven inside the block disappears with it.
 *
 * @param c  Checker.
 */
 void pushScope (Checker *c);
/**
 * @brief Defer the size check of `new T[n]` until instantiation.
 *
 * It is recorded only when currently inside a generic body (the visible type parameters are
 * non-empty); a concrete type can be decided on the spot, and deferring would only make the error
 * arrive later.
 *
 * @param c     Checker; the record is appended to `c->refChecks`.
 * @param t     Declared element type.
 * @param line  Source line, for diagnostics.
 */
 void recordNewSizeCheck (Checker *c, Type *t, int line);
/**
 * @brief Defer the zero-value check of a declaration without an initializer until instantiation.
 *
 * Whether `T` has a zero value is only known from the type arguments in a generic body, so it is
 * recorded here and decided at instantiation. Recorded only when the visible type parameters are
 * non-empty.
 *
 * @param c     Checker; the record is appended to `c->refChecks`.
 * @param t     Declared type.
 * @param line  Source line, for diagnostics.
 * @param name  Binding name, for diagnostics.
 */
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
/**
 * @brief Drop the non-null proofs of a binding and of every path prefixed by it.
 *
 * Proofs are kept on a stack, and everything from this binding's entry onward is truncated: writing
 * to the root or taking its address invalidates path facts such as `h.p`. Does nothing when the
 * binding was never proven.
 *
 * @param c      Checker.
 * @param cname  The binding's generated C name; NULL is ignored.
 */
 void unNarrow (Checker *c, const char *cname);
/**
 * @brief Tell whether a statement can create a pool, directly or through a function it calls.
 *
 * The answer reads the callee, so the call site must have resolved it first; a block and a domain
 * block are each a "place", so a false `descendBlocks` stops here and a true one walks in, for
 * transitive closure.
 *
 * @param s              Statement to check; NULL counts as no.
 * @param descendBlocks  Whether to walk into nested blocks and domain blocks.
 * @return               True when a pool-constructing call appears in the statement; false otherwise.
 */
 bool stmtMakesPool (Stmt *s, bool descendBlocks);

/** @brief Depth of a borrowing call result, for the store rule (see check_escape.c). */
bool borrowDepthOf(Checker *c, Expr *e, int *out);
/** @brief Does any part of this value borrow storage? `out` gets the deepest borrowed depth. */
bool borrowDepthAnywhere(Checker *c, Expr *e, int *out);

#endif /* EXTC_CHECK_INTERNAL_H */
