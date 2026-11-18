/**
 * @file results.h
 * @brief The result side: analysis results live here, addressed by a dense node id.
 *
 * It is a separate layer because the syntax tree must be frozen once the parser has built it, so
 * everything later stages compute about it lives **beside** the tree, not on it. Shape:
 *
 *   - one result slot per owner, not N fields per node;
 *   - addressed by a **dense integer id**, allocated once per node, so a lookup is an array index
 *     instead of a pointer hash;
 *   - pointers cannot serve as cache keys once a node may disappear -- so a result stores the
 *     name as an id, not a pointer.
 *
 * The id is allocated the first time a node needs a result and never moves afterwards. `NodeId`
 * is deliberately a plain integer: it can be stored, compared, and logged without dragging a
 * pointer along.
 *
 * Three entry points:
 *
 *   nodeIdOf(node, create)   - the node's id (create = allocate one for a new node)
 *   resultsOf(node, create)  - the node's result slot, by pointer
 *   resultsById(id)          - the node's result slot, by id
 *
 * When not creating, a node that has no result yet is looked up as NULL, and every accessor in
 * `plan.c` reads NULL as "the default value".
 */
#ifndef EXTC_RESULTS_H
#define EXTC_RESULTS_H

#include <stdint.h>

#include "ast.h"

/* A dense node index. 0 is never handed out, so it doubles as "no id". */
typedef unsigned NodeId;

/* Which kind of node a result slot belongs to. The first writer stamps it, and every
 * later access checks it -- rustc does the same with `validate_hir_id_for_typeck_results`
 * ("this table is from function A, this node is from function B"). A mismatch is a
 * compiler bug, not a program error, so it aborts under `EXTC_DBG=1` rather than
 * returning a default. */
typedef enum { RKIND_NONE = 0, RKIND_EXPR, RKIND_STMT, RKIND_FUNC, RKIND_OTHER } ResultKind;

/* Everything the plan side knows about one node, in one object -- the "one result slot
 * per owner" shape. A node that is only a call site uses two of these fields; a function
 * uses another set; nothing forces a node to spread its results across the tree. */
typedef struct {
    /* expression sites */
    int  arenaLevel, zoneLevel, arenaArg;
    bool needTemp;
    /* function summary */
    bool usesHome, mayUseArena, makesPool;
    /* statements: this loop's condition allocates (released per round) */
    bool condAllocs;
    /* This declaration's initializer names storage that outlives every frame (a global, a
     * `global` declaration, an `effects Ret=0` result, a string literal). A **result**, not
     * syntax: the checker computes it while the scopes are live, and `coroCheckDeferred` reads it
     * after the body -- by then `exprOutOfFrame` cannot resolve a function-local name, so asking
     * it again would answer "no". */
    bool varInitOO;
    /* A desugared `for` carries its step statement as **syntax** (`Stmt.forStep`, written by
     * the parser). The checker drops that step when it retargets the loop onto an iterator,
     * and that decision is a result, not syntax -- so it is recorded here. */
    bool forStepDropped;
    /* coroutines */
    bool isCoro, coroNeedsZone, coroBoxed;
    Type *yieldType, *coroFrameType;
    int  coroProto;
    /* identity */
    const char *instName;
    FuncDef    *tmpl;          /* instance -> template */
    FuncDef    *func;          /* the callee a call site / function value resolves to */
    bool        used;          /* was this function called? the emission gate reads it */
    /* "This frame never settles the account": this function and all of its callees write no
     * reference slot, have no unresolved callees, no extern, and no concurrency => **no `dec`
     * executes** anywhere in this function's dynamic execution range. Therefore its local
     * reference bindings (`var p = t`) can drop that `+1` -- dropping `+1` only makes hold
     * smaller, and only a `dec` that drives hold to 0 frees on the spot (`freeSlot` in
     * reclaim.c). Default **false** (unknown => count as usual); only the whole-call-graph
     * fixpoint on the check side sets it true. */
    bool        rcNoDec;
    /* "The heap reference graph this function builds has no back edges (no cycles)" and
     * "references in this function live only in counted rc slots". Together these are the static
     * precondition for "the event band (counting) needs no sweep": the first is (i)(ii)(iii), the
     * second is (iv), and `planFnAcyclic` answers true only when both hold. They are stored
     * separately because the closures over the call graph differ ((i') requires the callees to be
     * acyclic as a whole, (iv) only requires the callees' (iv)). Default false (unknown => keep
     * sweeping). */
    bool        fnAcyclic;
    bool        fnRefsCounted;
    /* "No live reference is dropped in this function": criteria (a) each ref field is written at
     * most once per object, (b) ref-typed locals and parameters are not reassigned, (c) no
     * reference-dropping operation (container delete/replace, `@overwrite` reusing the same cell,
     * free-class builtins, and callees that transitively violate (a)/(b)/(c)). The conjunction
     * holds for the whole unit => reclaim has nothing to do => emit no rc runtime
     * (`unitReclaimIdle` in codegen.c). Default false (not computed / does not hold => keep
     * reclaim), which is the safe direction. */
    bool        fnReclaimIdle;
    const char *cname;         /* the C name a binding is emitted under */
    /* The "may form a cycle" group this allocation site belongs to (`CycleMerge`'s site
     * union-find connected component); 0 = not part of a cross-site possible cycle. codegen.c
     * uses it to pin the sites of one group into the same reclaim shard: shard boundaries must
     * line up with structural boundaries, cutting a cycle leaves the cross-shard registration bit
     * as a seed for cyclic garbage, and the bound grows with each round. */
    int         cycleGroup;
    /* The unit this function belongs to has a **cross-site** possible cycle (the component
     * contains >= 2 allocation sites) => sharding would cut a cycle, the cross-shard registration
     * bit becomes a seed for cyclic garbage => the whole unit refuses sharding (`scanUnitReclaim`
     * in codegen.c). A single-site self-loop (each chain of `shard_bands`) does not set this bit:
     * that site is in one shard to begin with. */
    bool        fnCrossSiteCycle;
    /* ---- plan facts about a struct definition ---- */
    bool        builtinHolder; /* the struct behind a builtin name needs no C definition */
    FuncDef    *coroOf;        /* this frame struct belongs to that coroutine */
    /* A synthesized lambda environment: the function whose body the literal is written in. Code
     * generation needs it to lay the environment out once per *instance* of that body, and the
     * deferred-recheck passes need it because a check recorded inside a lambda body carries the
     * lambda's `call` method -- not the enclosing function -- as its `func`. */
    FuncDef    *lamScope;
    TraitDef   *implTrait;     /* the trait an `impl` block implements (NULL = inherent) */
    Type       *implTarget;    /* the type an `impl` block resolves to */
    FuncDef    *parWorker;     /* `parallel::run(worker, ...)`: the worker, for the trampoline */
    bool        domNew;        /* `sys::domain::single()`: the assoc builtin with no callee */
    bool        viewOf;        /* `std::sys::heap::extc_viewOf`: the plate layer's primitive */
    bool        isExtTarget;   /* an extern target the ABI calls directly */
    bool        isParWorker;   /* used as a `parallel::run` worker */
    bool        parTlsArena;   /* a worker that may use the thread-local arena */
    bool        parFullOut;    /* the worker gets the whole output view, not a [lo,hi) slice */
    bool        deref;         /* value position, but the type is `ref T`: emit `*(…)` */
    int         coroKind;      /* the index codegen's prepass gave this coroutine */
    bool        dynTable;      /* its body is named by a `dyn` vtable thunk, from outside */
    Type       *coroRetProto;  /* the declared return type of a coroutine, before substitution */
    bool        boxedCoro;     /* a coroutine call stored as a handle: codegen boxes it */
    /* A local handle whose task nothing outside its own block can reach: codegen ends the task
     * when the block is left, so an abandoned handle releases its zone and the pools born in it. */
    bool        coroOwned;
    /* A lambda literal whose environment is empty and that is used where a `fn(...)` value is
     * expected: the value is the code pointer, so codegen emits a plain function and not the
     * (empty) environment literal. Set by the checker when it accepts the conversion. */
    bool        lamFnPtr;
    /* `any` conversions the checker accepted. Boxing needs no payload (the source type is the
     * operand's own type); unboxing needs the type the context asked for, because the value is
     * checked against it at run time and that check's message names it. */
    const Type *anyTo;
    bool        anyBox;
    bool        dynCall;        /* a method call on an `any`: dispatch by the receiver's descriptor */
    /* `x += y` on a type that defines `fn +=`: the statement is a call, so codegen emits the call and
     * writes nothing back (assigning the result would need the hook to return a value). */
    bool        compoundCall;
    /* PLAN_LEN_PARAM: which `const` parameter this identifier names, or -1. */
    int         lenParam;
    /* `new T[n]` in the context of a `@buffer` type: the initializer is that type's one-argument
     * constructor, not an allocation. Set by the checker, read by codegen. */
    bool        newAsBuffer;
    /* `var x = <place>` where the type is marked `@value`: the copy is the type's `clone` method,
     * not a field copy. A temporary initializer is moved and leaves this false. */
    bool        varClone;
    /* `@value` copies the checker accepted at the other three copy points: a by-value argument or
     * struct-literal field (`argClone`) and a `return` (`retClone`). The declaration case above is
     * `varClone`. */
    bool        argClone;
    bool        retClone;
    uint64_t    addrMask;
    /* ---- call / receiver facts ---- */
    Expr       *extDom;        /* `EX_EXT`: the domain this task is handed to */
    FieldDef   *field;         /* the field an `EX_FIELD` resolved to */
    Type       *assocOwner;    /* `EX_ASSOC`: the instance type it resolved to */
    bool        dynRecvViaRef; /* the receiver of a `dyn` call arrives through a reference */
    bool        callViaFn;     /* the call goes through a function value, not a declaration */
    int         padKind;       /* `__extc_pad`: the padder the slot's value type picked (PlanPadKind) */
    bool        usedDyn;       /* a trait named by a `dyn` form (only then is its vtable emitted) */
    bool        usesDyn;       /* a module that saw a `dyn` form (the runtime is needed) */

    /* ---- the checker's own analysis facts (code generation never reads these) ----
     *
     * Same storage shape, different audience: these are working state the checker needs to
     * carry between its passes, and no later phase may depend on them. Keeping them in the
     * same slot keeps one id space and one arena; the field list in `plan.c`
     * (ANALYSIS_FIELDS) is what keeps the two audiences from quietly merging. */
    struct {
        const char *lamSig;    /* a lambda's written signature, for diagnostics */
        bool        makesPoolAny; /* some site of it builds a place */
        /* depth and origin facts. `refDepth` exists on an `Expr` **and** on the checker's
         * `Sym`: one question, one slot, one accessor for both. */
        int  refDepth;
        int  homeDepth;
        int  lexicalLevel;
        int  storedAt;
        /* operator / comparison facts */
        int  minAt;            /* the block level a borrow must outlive */
        bool reuse;            /* the value may be reused at the same site */
        bool borrowed;         /* the value is borrowed (mirrors `Sym.borrowed`) */
        bool qualified;        /* the user wrote a qualified name (`a::b`) */
        bool convCheck;        /* a narrowing conversion whose runtime check is emitted */
        bool needOp;           /* a comparison over a type parameter, resolved at instantiation */
        /* per-function facts (staged: call sites still use the fields) */
        bool isAssoc; bool lamChecked; bool lamInferRet; bool needsHome;
        int  allocState; int mayPrintState; int freshCount;
        Vec  arenaSites; int nParamSyms; void *paramSyms[64]; /* capacity 64: see the
                                                            * static assertion below */
        /* The `return`-side borrow summary: which parameter a returned view takes its storage
         * from, and which field of that parameter holds it. `PLAN_RET_BORROW` says the pair was
         * recorded; `CHECK_RET_BORROW_CONFLICT` in the first slot means two `return` statements
         * named different sources. */
        int  retBorrowArg;
        int  retBorrowField;
        /* The effect summary itself: one bit per parameter, "a call to
         * this function may store that parameter's address / contents into the caller's
         * frames (`…Addr` / `…Cont`), into this function's home arena (`home…`), or
         * somewhere else (`otherMask`)". `addrFromLocal` is the same question for a value
         * whose storage is a local. Read only by the checker. */
        uint64_t addrMask, contMask, otherMask;
        uint64_t homeAddrMask, homeContMask;
        bool     addrFromLocal;
        int  owSites;          /* sites the write-once pass recorded */
        bool owLocal;          /* the write-once local pass applies to this function */
        /* The effect closure's state. `effState` is **work state**: it is
         * written and read only while the closure runs (`EFF_IN_PROGRESS` marks a cycle on the
         * way down), and nothing outside `computeEffectsTransitive` reads it. It lives here
         * rather than in a local table because the closure is a memoised recursion over a
         * shared call graph -- the variant a per-call table would need is a `FuncDef*` hash,
         * which is what this slot already is. `effComplete`/`effUnknown` are real results:
         * later passes (and the closure itself) ask whether the summary can be trusted. */
        int  effState;
        bool effComplete;
        bool effUnknown;
        /* At an `ST_ASSIGN` site, how the "write into a reference slot" is classified (`SR_*`;
         * the definition and the criteria live in `noteAcyclicStore` in check_stmt.c). 0 = not a
         * reference-slot store, or not recorded yet. */
        int  storeRefKind;
        /* The **reason string** for this function being judged "has a cycle / references do not
         * live only in heap slots" (in the arena, outliving this pass); `EXTC_DBG_ACYCLIC=1`
         * prints it verbatim. NULL = no reason (it was judged true). */
        const char *acyclicWhy;
    } an;
    /* which of the fields above were written; an unset field reads as its default */
    /* Which fields of this slot were written. **Two words on purpose**: the plan side alone
     * has grown past 64 facts (`PLAN_DEREF` is bit 63), and a single word means the next fact
     * cannot be added without rewriting every bit test. Bits are `(hi << 6) | lo` -- see
     * `resultsBit` / `resultsSetBit` in results.c. */
    uint64_t loMask, hiMask;
    ResultKind kind;           /* which kind of node this slot belongs to (see ResultKind) */
    const FuncDef *owner;      /* the body whose results this slot holds (NULL = module level) */
    bool shared;               /* more than one body legitimately reaches this slot */
    int line;                  /* where the first write happened, for the guard's message */
} NodeResults;

/* `paramSyms` in a function's result slot is a fixed array; the effect summary caps the number of
 * parameters at `EFF_MAX_PARAMS` when the function is declared. Keep the two in step at compile
 * time: raising the macro without resizing the array would overrun it. */
_Static_assert(EFF_MAX_PARAMS <= 64, "NodeResults.paramSyms has room for 64 parameters");

/**
 * @brief Get `node`'s id, allocating one for it when `create` is true.
 *
 * @param node    The node; NULL has no id.
 * @param create  When true, allocate one if the node has no id yet (dense, starting at 1).
 * @return        The node's id; 0 when `node` is NULL, or when it has no id and none is created.
 */
NodeId nodeIdOf(const void *node, bool create);

/* ---- Owners: partitioning the result space ------------------------------------------
 *
 * Ids are dense, and a dense id is only useful when the scope is structured: an id belongs to one
 * function body (its "owner"), and access across bodies is a compiler bug, not a program error.
 */

/**
 * @brief Enter a function body; result slots created afterwards carry its owner stamp.
 *
 * @param owner  The function body being checked now; NULL means "in no function body"
 *               (module-level work), where slots have no owner and no access to them ever
 *               mismatches.
 */
void resultsEnterOwner(const FuncDef *owner);

/**
 * @brief Leave the current function body and go back to module-level work.
 */
void resultsLeaveOwner(void);

/**
 * @brief Get the current owner.
 * @return  The function body being checked; `NULL` for module-level work.
 */
const FuncDef *resultsCurrentOwner(void);

/**
 * @brief How many accesses came from outside the function body that created the slot.
 *
 * Every one is counted (one branch) so the compiler can use it as a compile-time signal. The
 * first crossing marks the slot `shared` and counts it; afterwards nothing is reported any more: a
 * node inside a generic body is reached by every instance of that body, which is by design -- the
 * tree is shared, so the answers live beside the tree. The check still catches the dangerous
 * variety: a slot only one body would reach, touched by a second one.
 *
 * @return  The cumulative number of first-crossing accesses.
 */
size_t resultsOwnerMismatches(void);

/**
 * @brief Get `node`'s result slot.
 *
 * @param node    The node.
 * @param create  When true, create a slot if there is none.
 * @return        The result slot; `NULL` when there is none and none is created.
 */
NodeResults *resultsOf(const void *node, bool create);

/**
 * @brief Get `node`'s result slot and declare which node kind the caller believes it is.
 *
 * The first write stamps `kind` and the then-current `owner` onto the slot, and every later
 * access checks them: a slot dressed as another node kind, or a slot whose owner differs from the
 * current function body, are compiler bugs rather than program errors -- abort under
 * `EXTC_DBG=1`, ignore in release builds (the value is returned as usual, behaviour unchanged).
 *
 * A crossing is a **first-class result, not a failure**: a node inside a generic body is reached
 * by every instance of that body, so the first crossing marks the slot `shared` and counts it and
 * nothing is reported afterwards -- the same source produces the same set of shared slots, which
 * is deterministic.
 *
 * @param node    The node.
 * @param create  When true, create a slot if there is none.
 * @param kind    The node kind the caller assumes; `RKIND_NONE` skips the check.
 * @param line    Where the first write happened, for the guard's message.
 * @return        The result slot; `NULL` when there is none and none is created.
 */
NodeResults *resultsAs(const void *node, bool create, ResultKind kind, int line);

/**
 * @brief Read bit `bit` of the mask.
 *
 * The bit is `(hi << 6) | lo`: `bit < 64` lands in `loMask` and the rest in `hiMask`, so callers
 * need not know which word they land in.
 *
 * @param r    The result slot; `NULL` counts as "no record", so every bit reads as 0.
 * @param bit  Bit number, using the same constants as the setter.
 * @return     `true` when the bit is 1.
 */
bool resultsBit(const NodeResults *r, uint64_t bit);

/**
 * @brief Set bit `bit` of the mask.
 *
 * @param r    The result slot; a `NULL` slot has no record to set, so the call does nothing.
 * @param bit  Bit number.
 */
void resultsSetBit(NodeResults *r, uint64_t bit);

/**
 * @brief Get a result slot by id.
 * @param id  The node id.
 * @return    The result slot; `NULL` when `id` is 0 or beyond the range handed out so far.
 */
NodeResults *resultsById(NodeId id);

/**
 * @brief How many ids have been handed out (the length of the dense range).
 *
 * Diagnostics and owner-partitioned work read it; nothing in the compiler depends on this number.
 *
 * @return  The number of ids handed out.
 */
size_t resultsCount(void);

#endif /* EXTC_RESULTS_H */
