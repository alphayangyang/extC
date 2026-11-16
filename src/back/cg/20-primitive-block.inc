/* ---- The runtime primitive block is emitted piece by piece ----
 *
 * The block is a table of pieces. Every definition is registered with its name, its
 * kind and its exact span as it is emitted, and the dead-code decision reads that registry
 * instead of guessing by the *shape* of a line (ends with `{`, has a `(`, starts at column
 * zero, ...). What is left in a product is decided from facts recorded at emission time,
 * and a line whose shape changes cannot silently change the answer. */
typedef enum {
    PRIM_SCAFFOLD = 0,  /* a comment, a blank line, or a `#if/#else/#endif` skeleton:
                         * always emitted, so dropping a definition leaves the scaffolding
                         * around it */
    PRIM_FUNC,          /* a function definition */
    PRIM_VAR,           /* a variable or typedef: one line */
    PRIM_MACRO          /* a `#define` line (one piece per line: the same macro may be
                         * defined once per branch of an `#if`) */
} PrimKind;

typedef struct {
    PrimKind    kind;
    const char *name;   /* NULL for scaffolding */
    const char *text;
} PrimPiece;

/* One row of the dispatcher table: a `@module` entry. `name` is the spelling used on
 * the command line, `desc` is the one-line description printed by `--help` (empty when
 * there is none). The function always has the shape `int (int, char **)`, like C's `main`. */
typedef struct {
    FuncDef    *f;
    const char *name;
    const char *desc;
} ModEntry;

/* One registered definition of the primitive block: its name, the text it consists of, and
 * what kind of definition it is. Everything the dead-code decision needs is here, so it never
 * has to look at the shape of a line to find out where a definition begins or ends. */
typedef struct {
    const char *name;
    const char *text;        /* the definition, exactly as the table spells it: this *is* its
                              * span, because the block is a table of exact pieces */
    PrimKind    kind;
    /* Not seen by the line-shape scan (it has parentheses), so it is registered by hand.
     * Keeping it out of the `[dce] prim` count keeps that count comparable with the baseline. */
    bool        quiet;
} PrimEnt;

/**
 * @brief The reclamation plan of one translation unit: every decision about *whether* and
 *        *how* the unit reclaims, in one value computed in one place (`rcPlanDecide`).
 *
 * The emission sites ask questions about this value, never switch on rules of their own: a
 * site asks "does the plan say a runtime is emitted" and never re-derives the answer from the
 * shape of the program it happens to be looking at. Two classes of past bug came from the
 * opposite arrangement -- a site that decided for itself, and a pruning step whose position in
 * the sequence was load-bearing without saying so (the v30 regression: moving the `rc.forced`
 * early return above the idle proof skipped the proof for every unit built with the switch on).
 * `tools/check_rc_plan.py` holds the line: the only functions allowed to write these fields are
 * the ones named in its table, and that table is the document.
 */
typedef struct RcPlan {
    /* The reclamation runtime (bitmap slab + holder counts + region sweep) is enabled: the
     * runtime text goes into the prologue, `new` takes its storage from an rc region claimed by
     * arena address, and reference stores emit count updates.
     *
     * **On by default**, but the runtime is emitted only when the program has an allocation site
     * the collector governs (`rcPlanDecide`: `new T`, `alloc<T>(n)`, and the single-value
     * cells of `@overwrite`). A program without such a site (400-odd golden files) therefore
     * generates byte-identical C, and a program with one gets all four layers.
     *
     * `EXTC_RECLAIM=0` turns it off (for comparison and troubleshooting); `EXTC_RECLAIM=1`
     * **forces** emission regardless of allocation sites -- the switch-oriented gates
     * (`tests/reclaim-shapes/run-switch.sh`, `tests/reclaim-perf/`) want exactly the "switch on"
     * setting, whether or not there is an allocation site. */
    bool        reclaim;
    /* set when `EXTC_RECLAIM` has a non-zero value: the last pruning in `rcPlanDecide` ("no governed
     * allocation point => no runtime") is skipped, so a switch-oriented gate gets a runtime on a shape
     * that allocates nothing the collector could reclaim. The idle proof still runs first. */
    bool        forced;
    /* Criterion (d) of the idle proof -- "the unit does not drop live **memory** either".
     *
     * Criteria (a)(b)(c) ask about references, so a unit whose only garbage is a replaced array
     * (`b = new u8[64MiB]` inside a loop, with `b` declared in the function body) has no reference
     * site at all and those three criteria hold on it. With the reference criteria alone such a unit
     * emits no runtime and every `new` lands in the frame arena, where the generation the round
     * replaced stays until the function returns: the peak then follows the round count instead of the
     * live set.
     *
     * Set by `rcPlanDecide` when such a site exists. It keeps the unit out of the idle tier (a
     * runtime is emitted) and out of the event tier: the event tier frees an object when its holder
     * count reaches zero, and an array of bytes has no holders to count, so only the sweep tier can
     * reclaim it. */
    bool        memGrowth;
    /* `EXTC_RC_SHARDS=N`: the **shard count** of the reclamation runtime (the pause knob);
     * default 1.
     *
     * A shard = one logical region (claimed by the arena's address) split into N **runtime
     * regions**: each shard has its own slab chain, its own cross-region registration bitmap
     * (`rsbit`, the per-shard in-edge set `SRS` of the prototype) and its own mark stack, so it
     * can be swept **on its own** -- the pause is proportional to the reachable set of the shard
     * being collected, independent of the size of the logical region.
     *
     * The shard number is fixed by **allocation site** (`rcSite++ % rcNShard`), not by live set
     * or object count: everything one `new` site produces (a list, a cycle, one generation of
     * tree nodes) always lands in the same shard, so lists and cycles inside a shard stay whole.
     * Splitting by live set would tear a list and a cycle into two shards; once a cross-shard
     * registration bit is set, cyclic garbage would forever be a seed and the bound would grow
     * with every round (the prototype's throughput suffered as well).
     *
     * With N == 1 a logical region still has exactly one runtime region: every emission site
     * below takes the original branch and the generated C is **byte-identical** to the one
     * without sharding. N > 1 only claims more regions; not a character of the runtime text
     * changes -- cross-region registration, source-side cancellation and per-region sweeping are
     * all existing machinery, and a shard *is* a region. */
    int         nShard;
    /* `EXTC_RC_NOCOUNT=1`: the count entry points compile to a no-op (collection relies on
     * sweeping alone). See the `#if` in reclaim.c. */
    int         noCount;
    /* `EXTC_RC_SWEEPFREE=1`: the event setting (no sweep); see the switch below. */
    int         sweepFree;
    /* Degradation flag: the static root table is not complete (more than 4096 slots),
     * so trigger points act as if every root were unenumerable. */
    bool        rootsUnreliable;
} RcPlan;

typedef struct {
    Arena      *arena;
    Ctx        *ctx;
    Buf        *out;
    const char *path;
    bool        lineMap;

    TypeTable  *tt;
    Vec         structs;        /* StructDef* - non-generic ones */
    Vec         funcs;          /* FuncDef* - methods of non-generic structs + free functions */
    int         indent;

    /* Monomorphization context: while the code of one generic instance is
     * generated, T stands for the corresponding type argument. */
    Vec        *substParams;    /* const char* */
    Vec        *substArgs;      /* Type* */
    const char *ownerPrefix;    /* instance name decorating method names, e.g. "Pair_i32_u8" */

    /* Slice helpers; see the comment on SliceHelper. Their need is discovered
     * only while generating, so bodies go into `body` first and the output is
     * assembled at the end as prototypes -> helpers -> bodies. */
    Vec         helpers;        /* SliceHelper */
    /* The `!` unwrap helpers; see the comment on UnwrapHelper. Registered on
     * demand from `genExpr` and assembled with the slice helpers, for the same
     * reason: a `!` in a `while` condition has no statement prefix to hoist a
     * temporary into, and the operand must still be evaluated exactly once. */
    Vec         unwraps;        /* UnwrapHelper */
    /* Names of the view index primitives already emitted (`<view>_index`). They
     * are emitted on demand, at the first subscript that needs one, for the same
     * reason slice helpers are: the need is discovered while generating, and a
     * call site inside a generic body names an instance the list of instances
     * may not hold. */
    Vec         viewIdx;        /* Type *: view instances whose index primitive is needed */
    Vec         viewCpy;        /* Type *: view instances whose bulk-copy primitive is needed */
    Vec         viewCpyU;       /* Type *: the `@unchecked` (unchecked) copier of those */
    Buf         body;

    /* Used by `?` expansion: the return type of the current function (a
     * failure value is built from it) and the temporary counter (one per `?`,
     * unique within the function). */
    Type       *retType;
    int         tmpSeq;
    /* Statement prefix: when the subject of `??` must not be evaluated twice
     * it is first computed into a temporary, and those lines must be emitted
     * *before* the statement that uses them (C has no statement expressions).
     * `?` already did this by hand in genTryHead; here it is a general
     * mechanism. `prefixBlk` says whether we are currently inside a statement:
     * outside one (a file-scope initializer, say) nothing may be emitted. */
    Buf         prefix;
    int         prefixBlk;
    /* The **top-level value expression** of the current statement (`genTopExpr` sets it and
     * restores it on leaving the statement). "Ownership delegation" is allowed only at this
     * level: when the call site is the top-level value of a whole statement, the owner cannot
     * still be holding a reference into this region that lives only in a register (see
     * `homeOwnArg` and `rcEmitOwe`). */
    const Expr *topExpr;
    /* ---- Arenas are refined per block ----
     * `__extc_a[k]` is the arena of the block at level k; k is the block depth,
     * known at compile time, the same lexical depth the checker uses when it
     * assigns reference depths.
     * `loopLevel[]` is the block level of each enclosing loop body, so that
     * `break` and `continue` know down to which level to release. */
    int         blkLevel;

    /* Coroutine lowering. Set while the body of a coroutine is
     * being emitted: the frame is a plain C struct, `yield` stores the pc and returns, and every
     * reference to a frame field is redirected by `coroFieldRef` below. */
    const FuncDef *coroFunc;
    const char    *coroFrame;       /* the frame parameter's C name: `f` */
    int            coroYieldSeq;    /* how many `yield`s have been emitted: the next pc value */
    bool           needEvent;       /* the program calls the event layer (epoll/sockets) */
    bool           needCoroHandle;
    bool           needHttpFile;      /* the file-layer shim (open/fstat/sendfile/HTTP date) has been declared */
    bool           needDns;           /* `extc_dns_lookup` has been declared (synchronous getaddrinfo) */
    bool           needDomain;       /* the program starts a task (`ext` inside a domain block) */
    bool           coroDefPrinted;
    bool           coroDeclsDone;
    bool           taskTableDone;
    bool           poolDone;
    size_t         coroDefA;  /* a coroutine is stored as a handle => emit the handle type */
    int            coroSeq;         /* names the temporaries a spawn needs (one per spawn) */
    /* Coroutine frames and step functions go here and are appended **last**, after the passes that
     * rewrite the unit by byte offset (same reason as `vtDefs` below): those passes match names, and
     * a coroutine's `counter` matches inside its own `counter$step`. Nothing calls a step function
     * from extC yet (slice B2), so it needs no prototype and C is happy with the definition late. */
    Buf            coroDefs;
    bool        noArena;   /* This function puts nothing into its own block arena,
                            * so neither `extc_arena __extc_a[N]` nor the release
                            * calls are emitted. The checker decides this
                            * (`planMayUseArena(f)`); it saves compile time. */
    /* Level-indexed tables, one slot per block level of the function being emitted (the same
     * count `__extc_a` gets). They are sized and allocated per function in `genFunc` (`levelCap`
     * slots); a fixed `[64]` array would abort on a legal program (200 nested blocks trip the
     * assertion in `genBlockBody`) and has nothing to do with the array they index (). */
    int        *loopLevel;
    int        *loopStep;
    /* One slot per enclosing `for`: did any `continue` in its body jump to the step label? The
     * label is only emitted when it is, or `-Wunused-label` fires on every `for` loop whose body
     * never continues (most of them). */
    bool       *loopStepUsed;
    /* `rcRoots.len` on **entry** to each registered loop. It answers "was this slot declared
     * outside that loop?": a slot index below a loop's entry value means the slot was already
     * live before that loop, that is, it is a long-lived slot outside the loop. */
    int        *loopRootBase;
    int         loopLen;
    int         levelCap;
    /* Per enclosing loop: the label id of the `for` step at the end of that loop's body, or 0
     * when the loop is a plain `while` (`continue` is then emitted as C's `continue`). A `for`
     * desugars to `while (cond) { BODY; step }`, and C's `continue` would skip the step -- an
     * infinite loop () -- so for those the emitter puts `__extc_forstep_<id>: ;` in
     * front of the step and `continue` becomes `goto` that label. */
    int         forStepSeq;
    /* Is this function self-recursive, that is, does it reach itself, possibly
     * through other calls? If so the prologue increments `__extc_rec_depth`,
     * every exit decrements it, and the recursion guard turns a stack overflow
     * into a clean error. The answer comes from the call graph computed by
     * funcCallsItself: that costs compile time, while the two runtime
     * statements land only in functions that really need a guard. */
    bool        isRecursive;
    /* The `@overwrite` sites of this function (`Stmt*`, in order of
     * appearance). The prologue emits one storage cell per site - a pointer
     * plus a lazily allocated flag - because such a declaration reuses a single
     * block of storage across executions of the site.
     * A site table plus a lookup is used instead of writing an index into the
     * AST: one body is visited once per generic instance, so an index stored in
     * the AST would be overwritten by the next instance. See `owIndex`. */
    Vec         owSitesOwn;     /* this function's sites: code generation's own table */
    /* Call sites in this function whose callee needs cells (`Expr*`, in order
     * of discovery). Each of them passes one `void *` cell per callee site, so
     * the callee reuses the caller's storage when it is itself @overwrite. */
    Vec         owCalls;
    /* The region pointer (a local) for each **region base** in this function: declared at the
     * top of the block on first use as
     * `ExtcRcRegion *__extc_rcrN = extc_rc_regionOf(base, lvl);` and reused by every later
     * site. A region **cannot disappear** while the frame is alive (releasing a block does not
     * drop a region; only frame destruction or task exit does), so this value is an invariant
     * inside the function body, and walking the registry linearly on every allocation would be
     * wasted work. */
    char       *rcRegBase[40];
    char        rcRegName[40][24];
    int         rcRegN;
    /* Local objects whose home region is statically known: `var/let x = new T` (a single
     * object, allocated by the rc allocator) where x is never reassigned. The emitted C name and
     * its allocation level are recorded here => `storeRefOwned` can skip the `ownSlab` lookup for
     * the home region (the region is exactly the `regionOf` result of that allocation site, byte
     * for byte). */
    const char *rcFreshName[64];
    int         rcFreshLvl[64];
    int         rcFreshN;
    /* C names of the identifiers that are **assigned to** in this function: evidence against
     * the rule above. */
    Vec         rcAssigned;
    /* C names of identifiers **written into a heap object's reference field** in this function
     * (`head` in `(*cur).next = head`). Only a local that is `new`ed and immediately chained into
     * a heap object can be the head of a tree or of a cycle, so `rcEmitStoreRelease` uses this as
     * the **evidence** that overwriting the slot may leave a cycle in the old value
     * (criterion 4). */
    Vec         rcLinked;
    /* The arena levels of this function that **can really hold rc objects** (a bitmap, bit L =
     * `__extc_a[L]`): a level is set when an allocation expression appears (`arenaRefAt`) or when
     * it is passed to a callee as the home argument (`homeArg`). Frame destruction emits
     * `extc_rc_regionDrop` only for those levels -- an arena nothing ever allocated into has no
     * region at all, so the call would just walk the registry linearly and return (three wasted
     * walks per frame in a recursive function). */
    unsigned    rcArenaUsed;
    bool        owLocalOwn; /* Does the current function keep its @overwrite cells in
                            * its own frame? See FuncDef.owLocal. */
    Vec         insts;          /* Type* - generic instances, deduplicated by C name (see below) */

    /* ---- Descriptor tables are generated on demand ----
     * Printing, and later structural `==`, needs them, but only a type that is
     * really used needs one. The set is found by running to a fixed point:
     * `descRef` both yields `&X_desc` and registers X, and the roots are the
     * argument types of the `extc_print(&x, &x_desc)` calls that genPrint
     * emits. This keeps compile time proportional to what the program uses.
     *
     * Which types are needed is known only after the bodies are generated, so
     * the descriptor pool is written after the prototypes and before the
     * bodies - the same layout trick the slice helpers use. */
    Vec         descs;          /* Type* - types needing a table, deduplicated by C name */
    Buf         desc;           /* descriptor pool, prepended to the bodies */
    /* The descriptor pool of the reclamation runtime: kept apart from the block above (the
     * printing/equality descriptors) because the dedup key differs. A descriptor is only a table
     * of pointer offsets plus a count, so **isomorphic allocation sites can share one**; the
     * table text is the dedup key and the index is the descriptor number. One descriptor per
     * allocation site piles up isomorphic descriptors in a single slab's type table and forces
     * `extc_rc_typeIdx` on the allocation path to walk them linearly (about 40 Ir per allocation
     * across the eight node allocation sites of `shard_bands`); with one number per distinct type
     * the `lastTd` hot cache pays off as well. */
    Buf         rcDesc;         /* rc type descriptor declarations, also prepended to the bodies */
    Vec         rcDescKeys;     /* const char* - the unique offset-table text; the index is the descriptor number */
    Buf         rt;             /* table types + shared scalars; needed by print or eq */
    Buf         rtPrint;        /* `extc_print` - only if a structured type is printed */
    /* Are we generating `main`? A `?` there traps instead of returning a failure
     * which is a decision code generation has to make per
     * function. */
    bool        inMain;
    Buf         rtEq;           /* `extc_eq` - only if an array or slice is compared */
    Buf         rtRaw;          /* the raw-terminal block - only if the program uses it */
    /* The buffered console output of the stream library (ruling 90): the buffer belongs
     * to the runtime so that `extc_cout_put` can be `static inline` and the C compiler
     * inlines it into the library's `<<` -- no call per operand at all. The one thing
     * only the generator can do is flush the tail before `main` returns. */
    Buf         rtCout;
    /* `extc_cout_f64` - only appended when a call site actually reaches it. The library's
     * `<<(f64)` is its one user, so a program that never prints a float does not carry the
     * formatter. */
    Buf         rtCoutF64;
    bool        needCoutF64;
    bool        needCout;
    /* Structural `==` also goes through a descriptor table; this vector lists
     * the types for which `extc_eq` is needed. The closure propagates inwards:
     * a container that needs equality needs it for its elements too, so a
     * struct element gets a generated one-line adapter. */
    Vec         eqNeed;         /* Type* - deduplicated by C name */
    /* Whether the print runtime is emitted must not be decided from
     * `descs.len`: `slice<u8>`, that is a string literal, uses the shared
     * `extc_desc_text` and never enters `descs`, so looking only at `descs`
     * loses `extc_print` and `extc_desc_text`. That bug really happened - the
     * most ordinary line there is, `println("x = ", n)`, failed to compile.
     * genPrint raises this flag directly instead. */
    /* `rt` (the descriptor types plus the shared scalar descriptors) and `rtPrint`
     * (`extc_print`) are separate switches because the two needs differ: a program can need the
     * descriptor table and never call `extc_print`, and emitting it anyway draws
     * `-Werror=unused-function` (clang also reports `-Wunneeded-internal-declaration`).
     * `needPrint` is set only where `extc_print` is **really called** (structural printing, and
     * printing the payload when `?` fails in main); `needRuntime` means "the table is needed". */
    bool        needRuntime;
    bool        needPrint;      /* `extc_print` is called somewhere => emit rtPrint */
    bool        needAny;        /* the dynamic `any` value is used somewhere => emit its block */
    bool        needAnyEq;      /* two `any` values are compared => `extc_eq` is needed too */
    /* The JSON half of the `any` runtime formats numbers through two formatters written in
     * `std::json` (extC source). No extC code calls them -- the emitted descriptor walk does --
     * so the `planUsed` bit a call site would set is set by the scan that looks for them, and
     * that scan is also the answer to "is the JSON half reachable at all". */
    bool        needAnyJson;
    bool        needPar;      /* par::run / par::map use the thread runtime */
    bool        needParRegion; /* some worker `new`s => the arena-sharing half of the region machinery takes part in the
     * build */
    bool        parTls;     /* the current function is on a parallel::run call chain => `new` goes through extc_tls_arena */
    /* The program uses the raw-terminal primitives: the runtime then keeps a copy of the
     * terminal settings and gives them back before the process dies. Emitted on demand
     * because the block is only reachable through `extc_raw_enter`, which the library
     * declares as an `extern!` -- a program that never touches a terminal does not carry
     * it. */
    bool        needRawTerm;
    /* Does the program declare an `extern!("extc-mem")` symbol? Then the byte-search runtime
     * (`extc_memFind` / `extc_memEq`) is emitted after the bodies, exactly like the raw-terminal
     * and pool blocks. The declaration is the trigger rather than a call: the library module is
     * what names the primitive, and a program that never uses `find` must not carry it.
     *
     * The block brings its own declaration of `memmem` (see `memfind.c`), so nothing has to be
     * written ahead of the includes and a program without the flag keeps byte-identical C. */
    bool        needMemFind;
    /* On-demand emission for the plate layer: **one flag per declaration** (a declaration is emitted only when its flag is set). */
    bool        needPlateView;    /* `extc_viewOf`: the plate's only entry point for using a reserved area as a view */
    bool        needPlateCopy;    /* `extc_memCopy`: the whole-block copy behind plate copyIn/copyOut */
    bool        needTime;     /* the program uses `extern!("extc-time")` => emit the clock and sleep */
    /* Is the function being generated marked `@unchecked`? Then **no check at all** is
     * emitted for its body -- not just the element indexes: bounds, division, shift,
     * narrowing, `!`'s null/tag assertion, slice ranges and the bulk-copy count. That is
     * the whole point of the annotation, and the one signed road to undefined behaviour
     * (`FuncDef.isUnchecked`, manual 0.5 / 12.5). Saved and restored around every body, so
     * the answer is per function: the flag is the whole granularity the annotation
     * promises. */
    bool        uncheckedBody;
    /* Is the expression being generated the **target of an assignment**? Then a
     * construct whose checked form is a comma expression must not be used: a comma
     * expression is not an lvalue in C. `EX_SIGN` reads this and moves its assertion
     * into the statement prefix, returning the payload itself as the lvalue
     * (`o! = v` must keep working -- the checker lists `EX_SIGN` as a place). */
    bool        inAssignTarget;
    /* Does the program declare a pool extern? Then the registry runtime is emitted, and
     * every place that can create one carries the two hooks that key its pools to it. */
    bool        needPool;
    /* `EXTC_RC_LOCALSTORE` (**on by default**, `=0` is the kill switch): local and parameter
     * slots (`owner == 0`) go through the dedicated entry `extc_rc_storeRefLocal`, which skips the
     * two `ownSlab` calls that the non-counting setting leaves useless. */
    int         rcLocalStore;
    /* Allocation-site counter: shard number = `rcSite++ % rcNShard`. It is bumped once per
     * emitted rc allocation expression, so one `new` always keeps the same shard number in the
     * whole product (two compilations agree). */
    int         rcSite;
    /* Diagnostics (`EXTC_DBG_RCCOST=1`): count rc sites by **block level** (= region) without
     * changing what is emitted. The counts price a region statically: should it be counted or
     * swept? Reference stores and allocations each have a cell for "inside a loop" and "outside a
     * loop", because executions cancel out within one loop nest and what is left is the
     * **per-iteration density**. `rootMax` records the largest number of root slots materialized
     * at a trigger point -- the size of the marking start set, a static proxy for sweep
     * amortization. */
    int         dbgCost;
    struct {
        int64_t nRef, nRefLoop, nAlloc, nAllocLoop, rootSlotsMax;
    }           dbgRc[72];
    /* Compile-time state of the reclamation runtime (meaningful only when `rc.reclaim` is on).
     *
     * `rcRoots` is the **static root table**: the C expressions of the reference slots live in the
     * current scope (`saved`, `c.a`, `arr[i].next`). A slot is pushed when it is declared and the
     * table is truncated on leaving the block (`rcRootMarks` holds the truncation point per
     * level). `rcGlobalRoots` holds global reference slots, which count as roots at every trigger
     * point. At a trigger point both are joined into one root table handed to
     * `extc_rc_maybeCollect`.
     *
     * `rc.rootsUnreliable`: this function holds reference stores that cannot be enumerated
     * statically (enum payloads, generic enums, coroutine frames, ...). **An incomplete root table
     * is never used**: the trigger point is skipped rather than risk a missing root and a wrong
     * release. */
    Vec         rcRoots;         /* const char *: live reference-slot expressions */
    Vec         rcRootMarks;     /* size_t: `rcRoots.len` on entry to each block level */
    /* Blocks currently being emitted, outermost first. A `return` runs the `@drop` hooks of every
     * local still in scope, innermost block first, so it needs to see them. */
    Vec         dropBlocks;      /* Stmt*: enclosing blocks */
    /* Names whose `@drop` already ran because of an explicit `drop x`, so the block tail (and a
     * `return`) must not run it again. Grows at a block's start and is truncated at its end, so a
     * name reused in a sibling block is not skipped by mistake. */
    Vec         dropExplicit;    /* const char* */
    /* `dropBlocks.len` at the point each enclosing loop body started: `break`/`continue` drop only
     * the blocks they are actually leaving. */
    Vec         dropLoopBases;   /* size_t */
    /* The binding a `return x` is moving out: it is dead on the way out, so its `@drop` hook must
     * not run for it (running it would release the state the caller now owns). */
    const char      *dropSkipName;
    /* Globals whose type carries an `@drop` hook: the process must run those hooks on the way out
     * (`atexit`), or the state they guard is silently abandoned. Each entry is a `GlobalDrop`. */
    Vec         globalDrops;
    bool        needGlobalDrops;
    /* Arena levels of the enclosing loops, mirrored for `break`/`continue` (see `loopLevel`). */
    
    /* Sites already covered by the loop-entry check of a view subscript (`EX_INDEX *`, see
     * `loopHoistRangeFor`). The elements are node identities: when `exIndex` finds one it emits a
     * bare pointer access and skips the per-iteration branch. */
    Vec         hoistedIdx;
    /* StructDef*: lambda environments that are used as **bare `fn(...)` pointers**. Each gets a
     * thunk with the lambda's own signature that calls the synthesised `call` method with an empty
     * environment (see `emitLamThunk`). Collected before the prototypes are emitted. */
    Vec         lamThunks;
    /* LamVar*: one entry per (lambda environment, instance of the body it is written in) pair,
     * for environments whose layout depends on a type parameter. Collected before the prototypes
     * are emitted; see `LamVar`. */
    Vec         lamVars;
    /* Call sites of `v.m()` on an `any`: each one gets its own generated dispatcher, emitted after
     * the `any` runtime is in the unit (the dispatch body calls `extc_any_unbox` and friends). */
    Vec         dynCalls;
    /* The enclosing instance's C name while a body that may contain such a lambda is generated
     * (`calc_key`, ...); NULL everywhere else. `cType` / `cMethodName` use it to spell the layout
     * of a lambda environment whose type depends on a type parameter. */
    const char *lamCtx;
    Vec         rcGlobalRoots;   /* const char *: global reference slots */
    /* The reclamation plan of this unit: whether a runtime is emitted at all, which tier,
     * the shard count, and the two degradation flags. Written by `rcPlanDecide`; every
     * emission site below reads it as `g->rc.<field>`. */
    RcPlan      rc;
    bool        rcGlobalsUnreliable; /* a global holds reference stores that cannot be enumerated => turn triggers off in every function */
    /* Whether the function being generated takes the hidden "home arena" parameter
     * (= `planUsesHome(f)`).
     *
     * A trigger point only acts on a region **this frame owns** (see `rcFrameOwnsArena`), while
     * the region an `@overwrite` cell lives in is decided by the cell's own `home`: when the cell
     * is in this frame (`f_owLocal`) and this function does **not** take a home, `home` is this
     * frame's `&__extc_a[1]` (main's home arena is its own `&__extc_a[1]` too); when the function
     * does take a home (`home` = the caller's home), or when `f_owLocal` is false (a generic
     * instance generated during codegen), `home` lands in an outer frame. */
    bool        rcFuncUsesHome;
    int         rcSeq;           /* unique number for a descriptor and for a trigger point */
    int         rcGuardSeq;      /* unique number for the entry-guard temporary */
    /* whether the function being emitted is "one place" (only functions of an entry file are; library functions are transparent). */
    bool        zoneHere;
    /* Whether this function takes the hidden "home zone" parameter
     * (= `FuncDef.makesPool`). When it does, the parameter is passed on; when it does not (main,
     * or a function that creates no pool at all), the zone of the current place is used. */
    bool        funcHasZoneParam;
    /* Whether the frame zone of this function (`__extc_zm1`) has been emitted. `zoneMark[1]`
     * cannot answer this: **the function body itself** is a block with blkLevel=1, and `genBlock`
     * saves and clears `zoneMark[1]` around the body, restoring it on exit => inside the body it
     * always reads 0 (all higher levels `zoneMark[k]`, k>1, are readable inside their own block
     * body, so only level 1 has this problem). */
    bool        zoneFrameMarked;
    /* Which block levels really emitted `extc_pool_zoneEnter()` (index = block level,
     * 1 = the function body). The pop has to consult this table instead of `zoneHere`
     * alone: a block's zone is emitted on demand, and a `zoneLeaveTo(__extc_zm<lvl>)`
     * for a level that never pushed would pop the *enclosing* zone, leaving pools created
     * afterwards with no zone at all (see `zoneAtLevel` / `cgReleaseLevel`). */
    bool       *zoneMark;
    /* Tasks whose frame is a C local of the function being emitted, one list per block level.
     * A stack frame dies with its block, but the task driver ends the task only when the step
     * reports "done": a loop that is abandoned (`break`, or a frame dropped halfway) never takes
     * that path, and the task's arena and place then leak -- the pools a coroutine created stay
     * registered until the process ends. `extc_task_end` is idempotent, so the block's release
     * point ends whatever is left. Boxed handles are not listed: their frame lives in the task's
     * own arena and a handle may move to a longer-lived variable, so the task has to end with the
     * last owner, not with this block. */
    Vec        *coroTaskAt;
    Vec        *ownedCoro;     /* per block level: names of handles whose task this block ends */
    /* `EXTC_TASK_ROOTS` (**on by default**; `=0` restores the previous product byte for byte):
     * register the task id slots the emitter can prove alive for the whole task with the runtime's
     * task table (`extc_task_rootPush` / `extc_task_rootPop`), so `extc_task_collect` can tell a
     * live task-owned place from a dead one. See `cgTaskRootPush`. */
    int         taskRoots;
    /* Registration bookkeeping: one list per block level of the `__extc_trk<seq>` guards emitted
     * there, plus `taskRootSeq` (the guard names) and `domBodyLevel` -- the block level of the
     * `d.run { ... }` body being emitted (0 = none), whose pop has to wait for `extc_dom_run`,
     * because the block's own release text is written *before* the drive call (see ST_DOMAIN).
     * `atBlockEnd` says "this release is the normal exit of its block", the only release that may
     * be deferred. */
    Vec        *taskRootAt;
    int         taskRootSeq;
    int         domBodyLevel;
    bool        atBlockEnd;
    /* Definitions whose name may never be used again (`DeadDef*`, in emission
     * order); dropUnreferenced decides after the whole unit is assembled. */
    Vec         deadDefs;
    Vec         deadFuncs;      /* DeadFunc*: functions that may have no caller */
    Vec         deadLocals;     /* DeadLocal*: local declarations that may be unread */
    const char *curFuncName;    /* C name of the function being generated */
    const FuncDef *curFunc;     /* the function being generated (the per-function facts on the plan side need it) */
    /* `main` is not a candidate for function pruning, so it has no DeadFunc entry - but its
     * locals still need a stage to be counted on, and that is its body text. */
    const char *mainFuncName;
    const char *mainBody;
    size_t      mainOff, mainLen;
    /* `@module` entries: the C `main` of the product becomes a dispatcher; `./a -m <name>`
     * calls the named entry and `--help` lists names and descriptions. A program without
     * `@module` has `needModuleTramp` false and generates C byte-identical to a program that never
     * had the feature (an ordinary program pays nothing for it). */
    bool        needModuleTramp;
    bool        hasMainEntry;
    Vec         moduleEntries;  /* ModEntry*: the entry function, plus the display name and description shown by `--help` */
    /* The runtime primitive block is a table (`PRIM_BLOCK`): one row per definition, one per
     * piece of scaffolding. `primEmit` registers each definition in `primEnts` as the block is
     * assembled, and `primSplice` decides -- from that registry, before the block is part of
     * the unit -- which of them the program uses. clang reports an unused `static inline` where
     * gcc does not, which is why a definition nobody uses may not be emitted at all. */
    size_t      primA;          /* where the block is spliced into the unit */
    Vec         primEnts;       /* PrimEnt* - the block's definitions, in emission order */
    /* Where the body buffer ended up in the finished output: a definition inside a
     * body is recorded as an offset into that buffer, and this turns it into a
     * position in the assembled unit. */
    size_t      bodyOff;
    /* Method-table declarations: emitted with the prototypes (a body that dispatches through a
     * table needs it in scope) while the definitions are appended at the very end. */
    Buf          vtDecls;
    /* The method-table **definitions**: built with the bodies, appended last (see `vtDecls`). */
    Buf          vtDefs;
} CG;


/* One slice helper: the bounds check and the view construction for `a[lo..hi]`
 * collected into a function. A function rather than an expression so that lo
 * and hi are each evaluated exactly once: written inline, `hi - lo` would
 * mention both bounds twice. */
typedef struct {
    const char *name;
    char       *text;
} SliceHelper;

/* One `!` unwrap helper: the runtime variant check plus the payload read, collected into a
 * function so the operand is an **argument** and is therefore evaluated exactly once.
 *
 * Why a function and not the comma expression: `(extc_tagAssert((x).tag,
 * ...), (x).u.some._0)` names `x` twice, so an impure operand (`f()!`) would call `f` twice.
 * A temporary in the **statement prefix** has no place to live in a `while` condition (the
 * checker rejects it: "cannot be evaluated ahead of time"). Passing the operand as a function
 * argument evaluates it once and works in any expression position, which is what `!` is: a
 * postfix operator on a value.
 *
 * The helper is per `option`/`result` instance, so the C type of the tag constant is known
 * when its text is built (inside the substitution context) and no template survives. */
typedef struct {
    const char *name;
    char       *text;
} UnwrapHelper;

/**
 * @brief Render one line with a printf format and return it as a new arena string; line length
 *        is unlimited.
 *
 * The length is computed first (`vsnprintf(NULL, 0, ...)`) and only then allocated, so there
 * is no line-length cap; a fixed-size buffer would not truncate loudly, it would silently
 * produce invalid C and still exit 0.
 *
 * @param g   Generator state; the string is allocated in g->arena.
 * @param fmt printf-style format string.
 * @param ap  Argument list; the function consumes it (it takes a va_copy for the second
 *            vsnprintf).
 * @return    NUL-terminated arena string; it lives as long as the arena.
 */
static char *cgFormat(CG *g, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    size_t len = (n > 0) ? (size_t)n : 0;
    char *p = (char *)arenaAlloc(g->arena, len + 1);
    vsnprintf(p, len + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

/**
 * @brief Append one formatted line to the generated output at the current indent.
 *
 * @param g   Generator state; supplies the output buffer, the indent and the arena.
 * @param fmt printf-style format string.
 */
static void cgLine(CG *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *tmp = cgFormat(g, fmt, ap);
    va_end(ap);

    for (int i = 0; i < g->indent; i++) bufPuts(g->out, "    ");
    bufPuts(g->out, tmp);
    bufPutc(g->out, '\n');
}


/**
 * @brief Append one formatted line to the pending statement-prefix buffer at the current indent.
 *
 * The text goes into the buffer instead of straight into the output: a prefix has to appear before
 * the statement that needs it; see flushPrefix.
 *
 * @param g   Generator state; supplies the prefix buffer, the indent and the arena.
 * @param fmt printf-style format string.
 */
static void pfLine(CG *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *tmp = cgFormat(g, fmt, ap);   /* the same no-limit formatter as cgLine */
    va_end(ap);
    for (int i = 0; i < g->indent; i++) bufPuts(&g->prefix, "    ");
    bufPuts(&g->prefix, tmp);
    bufPutc(&g->prefix, '\n');
}


/**
 * @brief Write out the buffered statement prefix and clear it.
 *
 * It must be called before the first line of the statement it belongs to: the lines in the prefix
 * compute exactly the temporaries that statement is about to read, and that ordering is the reason
 * the prefix exists.
 *
 * @param g  Generator state.
 */
static void flushPrefix(CG *g) {
    if (g->prefix.len == 0) return;
    bufPuts(g->out, bufCstr(&g->prefix));
    bufInit(&g->prefix, g->arena);
}

/**
 * @brief Substitute TY_PARAM in a type using the current instantiation context.
 *
 * @param g  Generator state; the context comes from g->substParams and g->substArgs.
 * @param t  The type to substitute in.
 * @return   The substituted type; `t` itself when no instantiation context is in effect.
 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
}

/**
 * @brief Install all three fields of the substitution context: type parameters, type arguments and
 *        the owner prefix used in method names.
 *
 * `substEnter` hands out the context as it was before entry and `substLeave` puts that same context
 * back. **The pair must be balanced and the returned copy must be passed back**: a `substLeave`
 * that cleared the three fields would let a nested entry which merely probes a type (emitting an
 * `ExtcRcType` descriptor, collecting root-table leaves, ... all of them run inside a generic
 * method body) wipe the enclosing instance's context, after which the rest of that one method body
 * is emitted with `T` unsubstituted (`slice_T`, `varArray_T`) -- C that does not compile.
 */
typedef struct {
    Vec        *params;
    Vec        *args;
    const char *owner;
} SubCtx;

/**
 * @brief Enter the monomorphization substitution context of one generic instance.
 *
 * A generic enum instance has no `sdef`; its owner is the `edef`. Both cases are handled here.
 * Entries may nest: give the returned context back to `substLeave` and no level overwrites another.
 *
 * @param g     Generator state; the context is written to g->substParams, g->substArgs and
 *              g->ownerPrefix.
 * @param inst  The instance type.
 * @return      The context from before entry; give it back to `substLeave`.
 */
static SubCtx substEnter(CG *g, Type *inst) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };

    g->substParams = inst->sdef ? &inst->sdef->typeParams : &inst->edef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
    return sv;
}

/**
 * @brief Leave the substitution context entered by substEnter and install the returned copy
 *        verbatim.
 *
 * @param g   Generator state.
 * @param sv  The context handed out by `substEnter` / `substEnterInst` / `substEnterFunc`.
 */
static void substLeave(CG *g, SubCtx sv) {
    g->substParams = sv.params;
    g->substArgs   = sv.args;
    g->ownerPrefix = sv.owner;
}


static const char *cFuncName(CG *g, FuncDef *f);   /* defined below; `cType` needs it */

/* One layout of a lambda's environment, for one instance of the body the literal is written in.
 *
 * A lambda written inside a generic body can capture a value whose type is a type parameter, so the
 * environment's field types are only concrete per instance (see `planLamScope`). The checker builds
 * one environment per literal; this lays that environment out once per instance, as a struct type of
 * its own (`<env>$<instance>`) with its own `call` method, so `$lam$calc$0` becomes
 * `$lam$calc$0$calc_key` with a `key` field and `$lam$calc$0$calc_item` with an `item` field.
 *
 * The fields themselves are shared with the checker's struct: substitution happens while printing
 * a field's type, so one `FieldDef` reads as `key` in one layout and `item` in the other. */
typedef struct {
    StructDef  *sd;         /* the environment as the checker built it (the template) */
    StructDef  *vsd;        /* this layout: the enclosing instance's type parameters, its own C name */
    Type       *vt;         /* `vsd` as a type; `vt->name` is the C name of this layout */
    Type       *vtNamed;    /* the same type as a `TY_STRUCT`, for signatures (see `lamVarAdd`) */
    FuncDef    *vcall;      /* `call` for this layout: the template's body, `self` typed as `ref vt` */
    const char *ctxName;    /* the enclosing instance's C name, e.g. `calc_key` */
    FuncDef    *ownerFn;    /* the enclosing function instance, or NULL */
    Type       *ownerInst;  /* the enclosing type instance, or NULL */
    bool        thunk;      /* some literal of this environment is taken as a bare `fn(...)` value */
} LamVar;

/** @brief Whether a lambda environment's layout depends on a type parameter (see `LamVar`). */
static bool lamEnvNeedsVariants(const StructDef *sd);
/** @brief The layout of `sd` for the instance `g->lamCtx` names, or NULL when there is none. */
static LamVar *lamVarFor(CG *g, const StructDef *sd);
/** @brief The C name of `sd`'s layout for the current instance, or NULL (call `cType` then). */
static const char *lamVarName(CG *g, const StructDef *sd);

/* Register the `!` unwrap helper of one option/result instance; defined with the other
 * on-demand helpers, below `cgLine`. A `!` can appear in a body generated before that
 * point, so the name is declared here. */
static const char *unwrapHelper(CG *g, Type *ot, bool isOpt, Type *pay);

/**
 * @brief Give the C spelling of an extC type.
 *
 * Composite spellings are allocated in g->arena. A function type appears only as a typedef name:
 * every call site joins the result with that name as `%s %s`, and a C function pointer would wrap
 * the name into a declarator.
 *
 * @param g  Generator state; the result is allocated in g->arena.
 * @param t  The type to spell; `NULL` counts as void.
 * @return   The C type spelling, e.g. `int32_t` or `pair_i32_u8 *`; an erroneous or unresolved type
 *           yields `int`, which only happens once the program has already failed the checker.
 */
static const char *cType(CG *g, Type *t) {
    if (!t) return "void";

    /* Substitute once over the whole type: both a ref and a generic instance
     * may have T nested inside. Handling a bare TY_PARAM is not enough, since
     * the outermost part of `ref Pair<A, B>` is the ref. */
    t = subst(g, t);

    if (t->kind == TY_PARAM) return "int";  /* no context to substitute in; cannot happen */

    switch (t->kind) {
        case TY_REF:   return arenaPrintf(g->arena, "%s *", cType(g, t->inner));
        case TY_VOID:  return "void";
        case TY_STRUCT:
            /* The **handle** for `coroutine<T>`: one shared struct whatever `T` is -- that is what
             * lets handles from different coroutines live in the same container. */
            /* `domain` is opaque to extC: one pointer, produced by the runtime. */
            if (t->sdef && t->sdef->name && strcmp(t->sdef->name, "domain") == 0) return "void *";
            /* Printing the handle's name **is** a use of it: whoever renders this type needs the
             * typedef. Registering the demand here (rather than only at the sites that create or
             * drive a coroutine) is what makes a data-position-only mention work: `struct holder
             * { h: coroutine<i64> }` emits `extc_coro h;` and the typedef with it, so
             * `stdlib/std/coro/scheduler.extc` compiles (a missing typedef is exactly this class of
             * problem). */
            if (t->sdef && isProtoType(t, "coroutine", 1)) { g->needCoroHandle = true; return "extc_coro"; }
            /* A synthesized coroutine frame: `struct <cname>$frame`, with the function's C name (so
             * methods and generic instances mangle the same way everywhere). */
            if (t->sdef && planCoroOf(t->sdef))
                return arenaPrintf(g->arena, "struct %s$frame", cFuncName(g, planCoroOf(t->sdef)));
            /* A lambda environment whose layout depends on a type parameter: inside an instance the
             * type is the layout made for that instance (`LamVar`). Outside one there is no single
             * layout, and only code that is never emitted (the generic template) can ask. */
            {
                const char *vn = lamVarName(g, t->sdef);
                if (vn) return vn;
            }
            return t->name;
        /* A `dyn` value is the runtime's handle: the same `{pid, slot, gen}` triple the pool
         * runtime defines, which is what `extc_dyn_put` returns and `extc_dyn_slot` checks. */
        case TY_DYN:    return "ExtcDynHandle";
        case TY_GENERIC:
            /* `coroutine<T>` instances are handles too (the decorated name is not emitted). */
            /* `domain` is opaque to extC: one pointer, produced by the runtime. */
            if (t->sdef && t->sdef->name && strcmp(t->sdef->name, "domain") == 0) return "void *";
            /* Printing the handle's name **is** a use of it: whoever renders this type needs the
             * typedef. Registering the demand here (rather than only at the sites that create or
             * drive a coroutine) is what makes a data-position-only mention work: `struct holder
             * { h: coroutine<i64> }` emits `extc_coro h;` and the typedef with it, so
             * `stdlib/std/coro/scheduler.extc` compiles (a missing typedef is exactly this class of
             * problem). */
            if (t->sdef && isProtoType(t, "coroutine", 1)) { g->needCoroHandle = true; return "extc_coro"; }
            return t->name;                 /* already a decorated name */
        case TY_ARRAY:  return t->name;     /* likewise: array_15_i32 */
        /* A function type is spelled by a **typedef name**, never by a declarator: every call site
         * of `cType` concatenates the result with a name ("%s %s"), and a C function pointer wraps
         * that name inside the declarator (`int64_t (*f)(int64_t)`). `t->name` is the mangle
         * (`extc_fn_i64__i64`), and the typedef itself is emitted once, ahead of every struct
         * definition. */
        case TY_FN:    return t->name ? t->name : "void *";
        case TY_ENUM:  return t->name;      /* a plain enum typedef in C */
        case TY_ANY:   return "ExtcAny";
        case TY_ERROR: return "int";
        case TY_BUILTIN:
            for (size_t i = 0; C_TYPES[i].extc; i++)
                if (strcmp(C_TYPES[i].extc, t->name) == 0) return C_TYPES[i].c;
            return "int";
        case TY_UNRESOLVED:
            return "int";       /* cannot appear once the checker has run */
        case TY_PARAM:
            return "int";       /* already handled above; this only silences -Wswitch */
    }
    return "int";
}

/**
 * @brief Give the C parameter-list text of a function type.
 *
 * With no parameters it yields `void`, not empty parentheses: `()` in a C declaration means
 * "parameters unspecified", which is a different type and would accept arbitrary arguments through
 * the function pointer.
 *
 * @param g   Generator state; the result is allocated in g->arena.
 * @param ft  The function type.
 * @return    The parameter types joined with commas; `void` when there are none.
 */
static const char *fnParamList(CG *g, Type *ft) {
    if (ft->params.len == 0) return "void";
    Buf b;
    bufInit(&b, g->arena);
    for (size_t i = 0; i < ft->params.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPuts(&b, cType(g, *(Type **)vecAt(&ft->params, i)));
    }
    return bufCstr(&b);
}

