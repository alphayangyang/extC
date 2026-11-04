/**
 * @file codegen.c
 * @brief Translation of a checked module into C source text.
 *
 * This pass does no type inference of its own: the type checker has already
 * written its results back into the AST (Expr.type, Expr.func, Expr.field,
 * Stmt.type), so all that is left here is translation.
 *
 * The generated C is deliberately dull C: no macro tricks, no optimization
 * stunts. `#line` maps gcc diagnostics back to line numbers in the .extc file.
 */

#include "codegen.h"
#include "dbg.h"         /* EXTC_DBG_ASSERT: contract violations are compiler bugs */
#include "plan.h"          /* the one read port for the plan: the side table the analysis passes record into */
#include "pools.h"
#include "coroutine.h"
#include "domain.h"      /* the pool registry runtime */
#include "memfind.h"        /* the byte-search runtime (std::sys::mem), emitted on demand */
#include "plate.h"          /* the plate runtime (std::sys::heap), emitted on demand */
#include "reclaim.h"        /* the reclamation runtime (slab + holder counts + region sweep) */
/* The checker owns the rules this pass has to agree with, so it includes the checker's
 * header rather than restating them: `typeSupportsOp` decides whether an operator applied
 * to an instantiated type is native, and `isEqualityOp` answers the `==` / `!=` pair.
 * `isProtoType` and `enumHasPayload` were duplicated here as static copies of the
 * checker's - the same hazard in a smaller form - and are gone. */
#include "typelayer.h"   /* the read-only type questions (no checker header here) */
#include "extctime.h"

#include <assert.h>     /* the entry-point contracts below */
#include <stdarg.h>
#include <stdlib.h>   /* exit: exceeding the closure limit must fail loudly */
#include <stdio.h>
#include <string.h>

/* Everything from types.h arrives through codegen.h. Including it a second time here
 * would redeclare each of its prototypes: a repeated textual include is processed
 * before the header's own guard can take effect. */

/* ---------------------------------------------------------------- type mapping */

typedef struct { const char *extc; const char *c; } TypeMap;

static const TypeMap C_TYPES[] = {
    { "i8",  "int8_t"  }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" },
    { "u8",  "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" },
    { "f32", "float"   }, { "f64", "double"  },
    { "bool", "bool"   },
    { NULL, NULL }
};

typedef struct { const char *extc; const char *fmt; const char *cast; } PrintFmt;

/* Format-free printing: the compiler picks the format from the static type,
 * so a user never writes "%d". */
static const PrintFmt PRINT_FMT[] = {
    { "i8",  "%d",   "(int)"       }, { "i16", "%d",   "(int)"       },
    { "i32", "%d",   "(int)"       }, { "i64", "%lld", "(long long)" },
    { "u8",  "%u",   "(unsigned)"  }, { "u16", "%u",   "(unsigned)"  },
    { "u32", "%u",   "(unsigned)"  }, { "u64", "%llu", "(unsigned long long)" },
    { "f32", "%g",   "(double)"    }, { "f64", "%g",   "(double)"    },
    { NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- state */

/* A definition that the program may never name: a top-level global, or a row of
 * the scalar descriptor table.
 *
 * The decision cannot be made where the definition is emitted, because what
 * refers to it may be generated much later (a body, or the descriptor table
 * itself). It is made once the whole translation unit is assembled, on the text
 * alone: the name is counted in the finished output, and a definition whose name
 * occurs exactly once - in that definition and nowhere else - is dropped. The
 * direction is conservative in the only way that matters: any other mention,
 * even from code that is itself dead, keeps the definition. */
/* A function the program may never call.
 *
 * Every emitted function is declared once and defined once, so a name with exactly
 * those two mentions has no caller anywhere - what gcc and clang report as
 * `-Wunused-function`. The decision is made on the finished text, like DeadDef, so a
 * mention from a descriptor row (the `eq` adapter of a struct, say) counts as a use
 * and keeps the function. `main` is never a candidate: it is called from outside the
 * unit.
 *
 * Both halves are kept as *text*, not as offsets, and they are removed together or
 * not at all. Keeping both halves as text rules out the two failure modes: removing
 * the declaration and the definition independently leaves definitions whose calls
 * become implicit declarations (28 new warnings), and an offset pair keyed by
 * `FuncDef*` frames something that is not one whole function (a cut body produces
 * `-Wreturn-type`). A name is unique per emitted function, so it is the key. */
typedef struct {
    const char *name;    /* C name */
    const char *proto;   /* the declaration, exactly as emitted */
    const char *body;    /* the definition; filled in once the body buffer is complete */
    size_t      off, len;   /* where it sits in `g.body`, until `body` is filled in */
    /* Named from **outside** its instance -- a `dyn` table's thunk calls it, and the table is spliced
     * in **after** the "definitions nothing names" pass has run, so that pass cannot see the mention
     * and would remove a method instance nothing else calls.
     *
     * Named `namedByDynTable`, not `dynTable`: `FuncDef` has a `dynTable` of its own (which lives in the plan
     * side table), and the two are different facts on different structs. The textual freeze checker
     * cannot tell them apart, so the name has to. */
    bool        namedByDynTable;
} DeadFunc;

/* A local declaration that may never be read.
 *
 * A local name that occurs exactly once in its own function body cannot be used: that one
 * occurrence is the declaration. The stage is therefore the body text - the same capture
 * the function pruning already keeps - so no byte offsets have to survive anything, and
 * the answer is sound by construction.
 *
 * What is deliberately *not* done here: guessing declarations from indentation and a
 * trailing `;`. That matches assignments too (`foo = f();` is a statement with a side
 * effect) and deleting one would drop the call. Only lines the generator itself emitted as
 * a declaration are registered. */
typedef struct {
    const char *name;      /* C name of the local */
    const char *text;      /* the declaration line, exactly as emitted */
    const char *funcName;  /* C name of the function it belongs to */
    size_t      own;       /* mentions the declaration itself contributes to that stage:
                            * 1 when it sits inside the body, 0 when it is in the prologue
                            * (`__extc_home` is declared before the body, and the body text
                            * is the stage, so nothing there mentions it) */
} DeadLocal;

typedef struct {
    const char *name;   /* the C name, as written into the output */
    const char *text;   /* the definition, exactly as emitted, newline included */
    /* A definition inside a function body is decided on a smaller stage: what
     * can name it is that piece of code and nothing else. `scopeB > scopeA`
     * marks such a definition, and the two offsets bound the code that could
     * mention it, inside the body buffer. `off` is where the definition sits in
     * that same buffer. */
    size_t      off, scopeA, scopeB;
    bool        scoped;  /* true when the two offsets above are the stage to count on */
} DeadDef;

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
    /* The reclamation runtime (bitmap slab + holder counts + region sweep) is enabled: the
     * runtime text goes into the prologue, `new` takes its storage from an rc region claimed by
     * arena address, and reference stores emit count updates.
     *
     * **On by default**, but the runtime is emitted only when the program has an allocation site
     * the collector governs (`scanUnitReclaim`: `new T`, `alloc<T>(n)`, and the single-value
     * cells of `@overwrite`). A program without such a site (400-odd golden files) therefore
     * generates byte-identical C, and a program with one gets all four layers.
     *
     * `EXTC_RECLAIM=0` turns it off (for comparison and troubleshooting); `EXTC_RECLAIM=1`
     * **forces** emission regardless of allocation sites -- the switch-oriented gates
     * (`tests/reclaim-shapes/run-switch.sh`, `tests/reclaim-perf/`) want exactly the "switch on"
     * setting, whether or not there is an allocation site. */
    bool        needReclaim;
    /* set when `EXTC_RECLAIM` has a non-zero value: skip the allocation-site test in `scanUnitReclaim` and emit unconditionally */
    bool        rcForced;
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
    int         rcNShard;
    /* `EXTC_RC_NOCOUNT=1`: the count entry points compile to a no-op (collection relies on
     * sweeping alone). See the `#if` in reclaim.c. */
    int         rcNoCount;
    /* `EXTC_RC_LOCALSTORE` (**on by default**, `=0` is the kill switch): local and parameter
     * slots (`owner == 0`) go through the dedicated entry `extc_rc_storeRefLocal`, which skips the
     * two `ownSlab` calls that the non-counting setting leaves useless. */
    int         rcLocalStore;
    /* `EXTC_RC_SWEEPFREE=1`: the event setting (no sweep); see the switch below. */
    int         rcSweepFree;
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
    /* Compile-time state of the reclamation runtime (meaningful only when `needReclaim` is on).
     *
     * `rcRoots` is the **static root table**: the C expressions of the reference slots live in the
     * current scope (`saved`, `c.a`, `arr[i].next`). A slot is pushed when it is declared and the
     * table is truncated on leaving the block (`rcRootMarks` holds the truncation point per
     * level). `rcGlobalRoots` holds global reference slots, which count as roots at every trigger
     * point. At a trigger point both are joined into one root table handed to
     * `extc_rc_maybeCollect`.
     *
     * `rcRootsUnreliable`: this function holds reference stores that cannot be enumerated
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
    bool        rcGlobalsUnreliable; /* a global holds reference stores that cannot be enumerated => turn triggers off in every function */
    bool        rcRootsUnreliable;
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

/* ---------------------------------------------------------------- expressions */

static const char *genExpr(CG *g, Expr *e);

/**
 * @brief Generate the **top-level value expression** of a statement and record "we are at statement
 *        top level" in `g->topExpr`.
 *
 * Only ownership delegation looks at this marker (`homeOwnArg`): when the call site is the
 * top-level value of a whole statement, the owner cannot still be holding a reference into this
 * region that lives only in a register -- and that is exactly what the callee's sweep, seeded from
 * its static root table, would miss. Subexpressions never delegate (they fall back to the owed
 * collection path of `rcEmitOwe`).
 *
 * @param g  The generator.
 * @param e  The statement's value expression.
 * @return   The C text of that expression.
 */
static const char *genTopExpr(CG *g, Expr *e) {
    const Expr *saved = g->topExpr;
    g->topExpr = e;
    const char *code = genExpr(g, e);
    g->topExpr = saved;
    return code;
}

static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);
static bool cgIsEntry(CG *g, const FuncDef *f);       /* `main`, or a `@module` entry */
static const char *cgEntryCName(CG *g, const FuncDef *f);
static const char *zoneArgRef(CG *g, Expr *e);   /* the zone argument of a pool-creating callee */
static void cgReclaimDropArena(CG *g, int lvl);  /* release the region at frame destruction (defined after cgIsRefType) */
/* The collection trigger gate and the owed-collection accounting (defined further below):
 * `cgImplicitArgs` needs them at a call site to decide whether this frame is eligible, or has to,
 * collect on itself.
 *
 * The trigger route has three states: this frame owns the region => sweep now (`RC_OWN_YES`); it
 * does not own it => only record what is owed (`RC_OWN_NO`); thread-local arena (parTls) => at run
 * time `extc_rc_tlsDepth` decides whether this is a worker entry frame, and an entry frame sweeps
 * now while an inner frame records what is owed (`RC_OWN_TLS`, see `rcOwnKind`). */
typedef enum { RC_OWN_NO = 0, RC_OWN_YES = 1, RC_OWN_TLS = 2 } RcOwn;

static bool rcFrameOwnsArena(CG *g, int lvl);
static RcOwn rcOwnKind(CG *g, int lvl);
static void rcEmitOwedCheck(CG *g, const char *basePtr, int lvl, RcOwn own);


    

/**
 * @brief Map an extC symbol name onto a fragment usable inside a C identifier.
 *
 * An operator such as `==` is a legal method name in extC but cannot go into a C identifier, so it
 * is renamed here; the mapping table must cover every operator the language allows as a method
 * name, because one it misses is pasted into the generated C verbatim and fails to compile. A user
 * name can collide with a C keyword (`double` is not a keyword in extC); those get a `__c` suffix,
 * with the keyword set supplied by `cIdentIsKeyword`, shared with the checker.
 *
 * @param g     Generator state; the new name is allocated in g->arena.
 * @param name  extC symbol name or identifier.
 * @return      The fragment usable in a C identifier; `name` itself when there is no mapping and it
 *              is not a keyword, otherwise the new name in the arena.
 */
static const char *cSymName(CG *g, const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq"  }, { "!=", "ne"  },
        { "<",  "lt"  }, { "<=", "le"  }, { ">",  "gt" }, { ">=", "ge" },
        { "+",  "add" }, { "-",  "sub" }, { "*",  "mul" },
        { "/",  "div" }, { "%",  "rem" },
        { "<<", "shl" }, { ">>", "shr" },
        { "[]", "idx" }, { "[]=", "idxset" },
        /* The compound assignment hooks are separate names from their value-building cousins, so
         * they get their own C fragments (`+=` is not `add`). */
        { "+=", "iadd" }, { "-=", "isub" }, { "*=", "imul" },
        { "/=", "idiv" }, { "%=", "irem" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    if (cIdentIsKeyword(name)) return arenaPrintf(g->arena, "%s__c", name);
    return name;
}

/**
 * @brief Take the external C symbol name of an `@export` function, that is, the name the author
 *        wrote.
 *
 * An extC identifier may not contain `$`, so a `$` in `f->name` can only be the loader's module
 * prefix (`plugin$init`) and the symbol name is the part after it. An exported symbol is a contract
 * with the C side: names this compiler would have to rename (such as `fn double`) are rejected
 * where they are written instead of quietly becoming `double__c`.
 *
 * @param f  Function definition; `?` when it is NULL or its name is empty.
 * @return   The symbol name after the module prefix; the whole name when there is no `$`.
 */
static const char *exportName(const FuncDef *f) {
    const char *n = (f && f->name) ? f->name : "?";
    const char *d = strrchr(n, '$');
    return d ? d + 1 : n;
}

/**
 * @brief Give the type suffix an operator's C name needs when its owner defines that operator more
 *        than once.
 *
 * The same operator may be defined once per right-operand type, so the operand type must go into
 * the C name or several definitions collapse onto one symbol. The definition side (`cFuncName`) and
 * every call site (`cMethodName`) must use the same computation, so it lives in one function.
 *
 * The suffix must be computed in the **callee's** monomorphization context: `subst` reads the
 * ambient context, and a call site without one leaves `T` as `T`, so the names would disagree with
 * the definition side. A call site therefore passes the callee's own type arguments explicitly.
 *
 * @param g       Generator state; the result is allocated in g->arena.
 * @param f       The operator method definition.
 * @param params  Type parameters to substitute; `NULL` means use the ambient context.
 * @param args    Type arguments as long as params; also `NULL` when params is.
 * @return        `_<mangled operand type>` (a writable view adds the `mut_` prefix); the empty
 *                string when the owner defines the operator once only, when there are fewer than
 *                two parameters, or when the name is not an overloadable operator.
 */
static const char *opOverloadSuffixIn(CG *g, FuncDef *f, Vec *params, Vec *args) {
    if (!f->owner || f->params.len < 2 || !isOverloadableOp(f->name)) return "";
    size_t same = 0;
    for (size_t i = 0; i < f->owner->methods.len; i++)
        if (strcmp((*(FuncDef **)vecAt(&f->owner->methods, i))->name, f->name) == 0) same++;
    if (same <= 1) return "";
    Param *p1 = *(Param **)vecAt(&f->params, 1);
    Type *rt = (params && args) ? ttSubstitute(g->tt, p1->type, params, args) : subst(g, p1->type);
    /* A writable view and the read-only one are **different types** to the operator
     * rule (`ttEquals` compares `mut`) but the **same C struct**, so `ttMangle` gives
     * both the name `slice_u8`; the suffix has to carry the mutability itself, or the
     * exact-type dispatch that makes overloading decidable is not expressible in C. */
    const char *mangled = ttMangle(g->tt, rt);
    if (rt && rt->mut) mangled = arenaPrintf(g->arena, "mut_%s", mangled);
    return arenaPrintf(g->arena, "_%s", mangled);
}

/**
 * @brief Definition-side operator type suffix: here the ambient context is the instance's own.
 *
 * @param g  Generator state.
 * @param f  The operator method definition.
 * @return   Same as opOverloadSuffixIn.
 */
static const char *opOverloadSuffix(CG *g, FuncDef *f) {
    return opOverloadSuffixIn(g, f, NULL, NULL);
}

/**
 * @brief Give a function's name in the generated C.
 *
 * The tests run in a fixed order: an exported function returns the name the author wrote first (a
 * call site must match the symbol the C side looks up), a generic free function's instance returns
 * its own C name, and only then come the operator suffix and the owner prefix. A method name
 * carries its owner prefix so that `Point_eq` and `Board_eq` do not collide.
 *
 * @param g  Generator state; the result is allocated in g->arena.
 * @param f  Function definition.
 * @return   The C name, allocated in the generator's arena.
 */
static const char *cFuncName(CG *g, FuncDef *f) {
    /* The export name must be decided **first**: a call site has to match the symbol the C side
     * looks up, and no module prefix, instance prefix or keyword rename may get in front of it. */
    if (f->isExport) return exportName(f);
    /* An instance of a generic free function carries its own C name (eq2_i32). */
    if (planInstName(f)) return planInstName(f);
    const char *suffix = opOverloadSuffix(g, f);
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s%s", g->ownerPrefix, cSymName(g, f->name), suffix);
    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return arenaPrintf(g->arena, "%s%s", cSymName(g, f->name), suffix);
}

/**
 * @brief Give a method's name in the generated C at a call site.
 *
 * The prefix comes from the receiver: a generic instance uses its own name (such as
 * `Pair_i32_u8_getFirst`), everything else uses the method's owner. `cFuncName` cannot be used
 * here: it adds the prefix of the instance currently being generated, while the callee may belong
 * to another type -- calling `Point.==` from inside `Wrapper<Point>` is exactly that case.
 *
 * @param g         Generator state; the result is allocated in g->arena.
 * @param recvType  The receiver's static type.
 * @param f         The method definition.
 * @return          The C name, allocated in the generator's arena.
 */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    /* The suffix must come from the **callee's** context, not from the ambient one here: a method
     * instance has its own typeArgs and otherwise the receiver instance's are used; this is the
     * place that knows the receiver. */
    Vec *sp = NULL, *sa = NULL;
    if (planTemplate(f)) {
        sp = &planTemplate(f)->typeParams;
        sa = &f->targs;
    } else if (rb && rb->kind == TY_GENERIC && rb->sdef) {
        sp = &rb->sdef->typeParams;
        sa = &rb->targs;
    }
    const char *suffix = opOverloadSuffixIn(g, f, sp, sa);
    /* A lambda environment's `call`: an environment laid out per instance has one `call` per
     * instance, named after that layout (`LamVar`). */
    if (f->owner) {
        LamVar *v = lamVarFor(g, f->owner);
        if (v) return arenaPrintf(g->arena, "%s_%s%s", v->vt->name, cSymName(g, f->name), suffix);
    }
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s%s", rb->name, cSymName(g, f->name), suffix);

    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return cSymName(g, f->name);
}

/**
 * @brief Test whether a type is a `slice` instance of exactly one type argument.
 *
 * This is the only thing the compiler knows about views, and it is an output convention rather than
 * knowledge of the container: printing a byte view as text belongs to basic output, while the
 * container's definition, fields and methods come from the standard library.
 *
 * @param t  The type to test.
 * @return   true when it is `slice<T>` with exactly one type argument.
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

/**
 * @brief Test whether a type is a byte view, that is, `slice<u8>`.
 *
 * @param t  The type to test.
 * @return   true when the element type is `u8`; `println` then prints the view as text rather than
 *           as a list of numbers.
 */
static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/**
 * @brief Emit the subscript primitive `<C name>_index(v, i, file, line)` of a view type.
 *
 * A view is passed by value, so the operand of a subscript is evaluated once and the value and
 * reference forms have no second code path. The primitive does its own bounds check and traps with
 * the extC source position when the index is out of range. The prelude's `get`, `==` and `find` all
 * reach it through `self[i]`, which keeps pointer arithmetic and hand-written checks out of the
 * library.
 *
 * The emitted function returns a pointer to the element and the call site dereferences it.
 * Returning the element value would break two things: a `ref` parameter needs an lvalue, and
 * `slice<struct>::==` (written `self[i] != other[i]`, with `self: ref T`) would not compile at all;
 * assignments such as `s[i] = x` and `s[i].field = x` need an lvalue just as much. A view already
 * has reference semantics, namely the `ref` in `data: ref T`.
 *
 * @param g     The generator.
 * @param inst  The view instance; `targs` has at least one element, the element type.
 */
static void genViewIndexer(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    SubCtx sv = substEnter(g, inst);
    cgLine(g, "static inline %s *%s_index(%s v, int64_t i, const char *file, int line) {",
           cType(g, elem), inst->name, inst->name);
    g->indent++;
    /* A view whose storage is gone has no `i` that could be in range: the `!v.data` test must
     * come before the index test, since `data == NULL` with a non-zero `len` would otherwise pass
     * the bounds check and store straight through the null pointer. The message names what
     * happened instead of blaming the index. */
    cgLine(g, "if (!v.data) extc_trapMsg(file, line, \"the view has no storage\");");
    cgLine(g, "if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);");
    cgLine(g, "return &v.data[i];");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g, sv);
}

/**
 * @brief Emit on demand the whole-block copy primitive of a view type (used by `copyInto`).
 *
 * One call checks the count once and then moves bytes with `memmove`, so the check is paid per
 * **call** instead of per element: a byte loop would pay a range check per byte, plus a stale-pool
 * guard for containers. `memmove` rather than `memcpy`, because the common caller moves a buffer to
 * a position that overlaps it (`string::removeFront`).
 *
 * The count check traps with a source position like any other out-of-range access. `unchecked` is
 * the `@unchecked` variant: it `memmove`s exactly the written count with no length check and no
 * storage check, and its function name carries `..._copyU`.
 *
 * @param g          The generator.
 * @param inst       The view instance; `targs` has at least one element, the element type.
 * @param unchecked  When `true`, emit the unchecked variant named `<C name>_copyU`.
 */
static void genViewCopier(CG *g, Type *inst, bool unchecked) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    SubCtx sv = substEnter(g, inst);
    cgLine(g, "static inline int64_t %s_copy%s(%s d, %s s, int64_t n, const char *file, int line) {",
           inst->name, unchecked ? "U" : "", inst->name, inst->name);
    g->indent++;
    if (!unchecked) {
        cgLine(g, "if (n < 0 || n > d.len || n > s.len)");
        cgLine(g, "    extc_trapMsg(file, line, \"copyInto: the count is beyond a slice length\");");
        /* A view whose storage is gone has no byte to copy, so it traps. Conditional on `n > 0`:
         * copying zero elements from an empty view is legal and must stay legal. */
        cgLine(g, "if (n > 0 && (!d.data || !s.data))");
        cgLine(g, "    extc_trapMsg(file, line, \"the view has no storage\");");
    } else {
        cgLine(g, "(void)file; (void)line;");
    }
    cgLine(g, "if (n > 0) memmove((void *)d.data, (const void *)s.data, (size_t)n * sizeof(%s));",
           cType(g, elem));
    cgLine(g, "return n;");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g, sv);
}

/**
 * @brief Register a view type's subscript primitive on demand and return its name.
 *
 * It can only be emitted on demand: the type-instance table alone is not enough, because a
 * subscript inside a generic body uses the name of the **instance** (`slice_slice_u8_index`), and
 * the table need not hold that instance -- a view whose element type came from a type parameter is
 * not in it, and the generated C would call a function nobody emitted and fail to compile. Whoever
 * needs the primitive emits it (`sliceHelper` works the same way), the dependency is centralized in
 * one place, and after name dedup one view type produces exactly one function.
 *
 * @param g     The generator; the dedup table is `g->viewIdx` and new instances are appended at the
 *              end.
 * @param inst  The view instance, already substituted by the caller.
 * @return      The primitive's name, `<view instance name>_index`.
 */
static const char *viewIndexer(CG *g, Type *inst) {
    const char *name = arenaPrintf(g->arena, "%s_index", inst->name);
    for (size_t i = 0; i < g->viewIdx.len; i++)
        if (strcmp((*(Type **)vecAt(&g->viewIdx, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(&g->viewIdx) = inst;
    return name;
}

/**
 * @brief Register a view type's whole-block copy primitive on demand and return its name.
 *
 * Registered on demand like `viewIndexer`. The `@unchecked` form has its own name (`..._copyU`) and
 * its own table `g->viewCpyU`: checked call sites of the same view type must keep getting the
 * checked definition.
 *
 * @param g          The generator.
 * @param inst       The view instance, already substituted by the caller.
 * @param unchecked  When `true`, register the `..._copyU` variant (table `g->viewCpyU`), otherwise
 *                   the checked variant (table `g->viewCpy`).
 * @return           The primitive's name, `<view instance name>_copy`; `<view instance name>_copyU`
 *                   when `unchecked`.
 */
static const char *viewCopier(CG *g, Type *inst, bool unchecked) {
    const char *name = arenaPrintf(g->arena, "%s_copy%s", inst->name, unchecked ? "U" : "");
    Vec *list = unchecked ? &g->viewCpyU : &g->viewCpy;
    for (size_t i = 0; i < list->len; i++)
        if (strcmp((*(Type **)vecAt(list, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(list) = inst;
    return name;
}

/* ---------------------------------------------------------------- type descriptors
 *
 * Printing derives no code any more: every type yields one `static const
 * ExtcDesc`, which is pure data, and the whole program shares a single
 * `extc_print`, the runtime part emitted at the top of the generated file.
 *
 * The abandoned scheme, kept here only as a warning, generated a whole printf
 * sequence per struct: a synthetic stress program grew 2028 such functions,
 * 22.9% of the generated C, and printed none of them.
 *
 * `descRef` maps an extC type to the address of the constant describing it:
 *   - scalar, ref, slice<u8>: one of the few descriptors shared by the whole
 *     program, which cost no extra space
 *   - struct, generic instance, array, enum: its own `<C name>_desc`
 *
 * Notes:
 *   - Descriptors reference one another; `struct s { xs: slice<s> }` is a cycle.
 *     That is why the pool opens with a `static const ExtcDesc X_desc;`
 *     tentative definition for every descriptor, which makes the definition
 *     order irrelevant.
 */
/**
 * @brief Register that a type needs a descriptor of its own.
 *
 * Deduplicated by C name: a mutable and a read-only view are one C struct and share one descriptor.
 * Scalars, `ref` and `slice<u8>` use the shared descriptor and are not registered.
 *
 * @param g  The generator; the registry is `g->descs` and new types are appended at the end.
 * @param t  The type; returns immediately when it is `NULL` or has no C name.
 */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/**
 * @brief Register that a type needs `extc_eq`.
 *
 * Deduplicated by C name. Comparing a container compares its elements, so the element type is
 * registered too; `emitDescRegion` closes this demand set into a closure.
 *
 * @param g  The generator; the registry is `g->eqNeed` and new types are appended at the end.
 * @param t  The type; returns immediately when it is `NULL` or has no C name.
 */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

/**
 * @brief Return the C expression of the descriptor of a type.
 *
 * Walking outwards registers every type the descriptor refers to, which is why the demand set
 * becomes a closure.
 *
 * @param g  The generator.
 * @param t  The type; `NULL` yields `&extc_desc_i32`.
 * @return   A string such as `&extc_desc_i32` or `&list_i32_desc`, allocated in the generator's
 *           arena.
 */
static const char *descRef(CG *g, Type *t) {
    if (!t) return "&extc_desc_i32";
    needDesc(g, t);
    if (t->kind == TY_BUILTIN) return arenaPrintf(g->arena, "&extc_desc_%s", t->name);
    if (t->kind == TY_REF)     return "&extc_desc_ref";
    if (isByteView(t))         return "&extc_desc_text";
    /* Everything else has one of its own; a generic instance uses its own C
     * name, as in list_i32_desc. */
    return arenaPrintf(g->arena, "&%s_desc", t->name);
}

/**
 * @brief Record a top-level definition just emitted into the current buffer, so that it can later
 *        be judged for references.
 *
 * As with globals, the finished text decides whether anything names the definition. The descriptor
 * pool depends on this: its definitions are produced on demand and not every one of them is
 * referenced. The declaration and the definition are two separate texts registered under the same
 * name.
 *
 * @param g       The generator; the record is appended to `g->deadDefs`.
 * @param name    The C symbol name the definition is registered under.
 * @param before  The offset of the definition in `g->out`.
 */
static void deadDefAdd(CG *g, const char *name, size_t before) {
    Buf l;
    bufInit(&l, g->arena);
    bufPutn(&l, g->out->data + before, g->out->len - before);
    DeadDef *d = arenaAllocZero(g->arena, sizeof *d);
    d->name = name;
    d->text = bufCstr(&l);
    *(DeadDef **)vecPush(&g->deadDefs) = d;
}

/**
 * @brief Emit the descriptor of a struct: field names plus `offsetof`.
 *
 * `offsetof` instead of computing the layout here: a missed field or a wrong layout fails at
 * compile time instead of printing garbage.
 *
 * @param g      The generator.
 * @param cname  The struct's C name, also the prefix of its descriptor and field table.
 * @param disp   The name to display; a generic instance uses the template name
 *               (`varArray { ... }`).
 * @param sd     The struct definition; field types are substituted before their descriptors are
 *               taken when the field table is written.
 * @param eqFn   The C name of the equality adapter; `NULL` when the type has none.
 */
static void genStructDesc(CG *g, const char *cname, const char *disp, StructDef *sd,
                          const char *eqFn) {
    size_t n = sd->fields.len;
    if (n) {
        size_t ftb = g->out->len;              /* the whole table, up to its `};` */
        cgLine(g, "static const ExtcField %s_fields[] = {", cname);
        g->indent++;
        for (size_t i = 0; i < n; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            cgLine(g, "{ \"%s\", offsetof(%s, %s), %s },", fd->name, cname, fd->name,
                   descRef(g, subst(g, fd->type)));
        }
        g->indent--;
        cgLine(g, "};");
        /* Registered like a descriptor: the row that names it may itself be dropped. */
        deadDefAdd(g, arenaPrintf(g->arena, "%s_fields", cname), ftb);
    }
    size_t sdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_STRUCT, \"%s\", sizeof(%s), %zu, %s, NULL, %s };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_fields", cname) : "NULL",
           eqFn ? eqFn : "NULL");
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", cname), sdb);
}

/**
 * @brief Emit the descriptor of an array: element count plus element descriptor.
 *
 * An array has no field names, so it prints as `[1, 2, 3]`.
 *
 * @param g    The generator.
 * @param arr  The array type; `name` is its C name and `inner` the element type.
 */
static void genArrayDesc(CG *g, Type *arr) {
    size_t adb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s, NULL };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", arr->name), adb);
}

/**
 * @brief Emit the descriptor of a non-byte view; it prints as `[a, b]`.
 *
 * A byte view never reaches here: it shares `extc_desc_text`.
 *
 * @param g  The generator.
 * @param v  The view instance; the first element of `targs` is the element type, substituted before
 *           its descriptor is taken.
 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    size_t vdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s, NULL };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", v->name), vdb);
}

/**
 * @brief Emit the descriptor of an enum: the variant-name table, with a value outside the table
 *        printed as `<type name>`.
 *
 * Only variant names are printed, never payloads.
 *
 * @param g      The generator.
 * @param cname  The enum's C name, also the prefix of its descriptor and variant table.
 * @param disp   The name to display.
 * @param td     The enum definition; with no variants the variant table is not emitted and that
 *               cell of the descriptor is `NULL`.
 */
static void genEnumDesc(CG *g, const char *cname, const char *disp, TypeDef *td) {
    size_t n = td->variants.len;
    if (n) {
        Buf b;
        bufInit(&b, g->arena);
        for (size_t j = 0; j < n; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&b, ", ");
            bufPrintf(&b, "\"%s\"", v->name);
        }
        size_t vtb = g->out->len;
        cgLine(g, "static const char *const %s_variants[] = { %s };", cname, bufCstr(&b));
        deadDefAdd(g, arenaPrintf(g->arena, "%s_variants", cname), vtb);
    }
    size_t edb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ENUM, \"%s\", sizeof(%s), %zu, %s, NULL, NULL };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_variants", cname) : "NULL");
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", cname), edb);
}

/* ------------------------------------------------------- descriptors on demand
 *
 * Only a type that is really printed gets a descriptor. In the synthetic stress
 * program with N=1000 no struct was printed at all, and the descriptor pool
 * came out completely empty.
 *
 * The roots are the argument types of the `extc_print(&x, &x_desc)` calls that
 * genPrint emits, registered through `descRef`; the closure follows the
 * references between descriptors - struct fields, array elements, slice
 * elements - until it reaches a fixed point.
 *
 * Two things this has to get right:
 *   1. Order. Which descriptors are needed is known only after the function
 *      bodies are generated, because `genPrint` runs inside them, while C wants
 *      definitions before uses. The pool is therefore written after the
 *      prototypes and before the bodies, and spliced back in at the end, the
 *      same way the slice helpers are.
 *   2. Cycles. `struct s { xs: slice<s> }` gives `s_desc -> slice_s_desc ->
 *      s_desc`. Every descriptor is therefore forward-declared first, with a C
 *      tentative definition, which makes the order of the definitions
 *      irrelevant. Collecting that set needs a dry pass first; it has no side
 *      effects, because the descriptor emitters write only to `g->out` and to
 *      the arena.
 */
static bool eqNeeded(CG *g, Type *t);
static void closeEqNeeds(CG *g);
static FuncDef *findOpMethod(TypeTable *tt, Type *t, const char *sym, Type *rhs, const char *fallback);
static void genEqAdapter(CG *g, Type *t, FuncDef *m);

/**
 * @brief Emit the definition of every registered descriptor.
 *
 * Re-reading `len` in the loop condition is deliberate: `descRef` appends new dependencies while
 * definitions are being written.
 *
 * @param g  The generator; reads `g->descs` and writes the definitions into `g->out`.
 */
static void emitDescDefs(CG *g) {
    for (size_t i = 0; i < g->descs.len; i++) {
        Type *t = *(Type **)vecAt(&g->descs, i);
        /* Only a struct that `extc_eq` really recurses into fills this slot; others stay NULL. */
        const char *eqFn = eqNeeded(g, t) ? arenaPrintf(g->arena, "%s_eqD", t->name)
                                          : NULL;
        if (t->kind == TY_STRUCT && t->sdef) {
            genStructDesc(g, t->name, t->name, t->sdef, eqFn);
        } else if (t->kind == TY_ENUM && t->edef) {
            genEnumDesc(g, t->name, t->name, t->edef);
        } else if (t->kind == TY_ARRAY) {
            genArrayDesc(g, t);
        } else if (t->kind == TY_GENERIC && t->sdef) {
            SubCtx sv = substEnter(g, t);  /* T in the fields stands for the type arguments */
            if (isView(t)) genViewDesc(g, t);
            else           genStructDesc(g, t->name, t->sdef->name, t->sdef, eqFn);
            substLeave(g, sv);
        } else {
            /* A missing case must fail loudly: silently emitting no descriptor
             * would leave the generated C referring to a symbol that does not
             * exist. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: no descriptor generator for this type");
        }
    }
}

/**
 * @brief Test whether a type needs an `eq` generated or filled in.
 *
 * Looked up by C name.
 *
 * @param g  The generator; the lookup table is `g->eqNeed`.
 * @param t  The type; `false` when it is `NULL` or has no C name.
 * @return   `true` when it has been registered.
 */
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/**
 * @brief Close the `eq` set inwards: a container that needs equality needs it for its elements too.
 *
 * A struct stops this walk, because its equality goes through `d->eq` to the user's `fn ==`.
 *
 * @param g  The generator; walks `g->eqNeed`, to which the loop appends elements.
 */
static void closeEqNeeds(CG *g) {
    for (size_t i = 0; i < g->eqNeed.len; i++) {   /* reread: entries are appended */
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind == TY_ARRAY) { needEq(g, t->inner); continue; }
        if (t->kind == TY_GENERIC && isView(t) && !isByteView(t))
            needEq(g, *(Type **)vecAt(&t->targs, 0));
    }
}

/**
 * @brief Write the descriptor pool into `g->desc`.
 *
 * The pool holds the equality adapters and the descriptor definitions and is inserted between the
 * prototypes and the function bodies. When both `g->descs` and `g->eqNeed` are empty the whole
 * block is unnecessary.
 *
 * @param g  The generator; temporarily points `g->out` at `g->desc` and restores it afterwards.
 */
static void emitDescRegion(CG *g) {
    /* Nothing structured was printed and no array was compared, so the whole
     * pool - descriptors, printing and comparison - is unnecessary. */
    if (g->descs.len == 0 && g->eqNeed.len == 0) return;
    closeEqNeeds(g);
    Buf *saved = g->out;
    g->out = &g->desc;
    emitDescDefs(g);                       /* dry pass: collect dependencies, drop text */
    bufInit(&g->desc, g->arena);

    /* ---- Equality adapters for structural `==` ----
     * They must precede the descriptors, which take their address. Only a
     * struct that `extc_eq` really recurses into needs one: that struct's own
     * `fn ==` is arbitrary user code, so the runtime can only delegate to it.
     * Every other kind is recursed into by `extc_eq` itself. */
    for (size_t i = 0; i < g->eqNeed.len; i++) {
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind != TY_STRUCT && t->kind != TY_GENERIC) continue;
        if (t->kind == TY_GENERIC && isView(t)) continue;   /* extc_eq recurses into views */
        FuncDef *m = findOpMethod(g->tt, t, "==", t, NULL);
        if (!m) {
            /* Unreachable: the checker permits a comparison only when the
             * element type is comparable. If it is reached anyway, fail loudly
             * rather than emit C that does not compile. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: structural equality needs a `fn ==` on this type");
            continue;
        }
        genEqAdapter(g, t, m);
    }
    if (g->eqNeed.len) cgLine(g, "");

    cgLine(g, "/* ---- Type descriptor table, emitted on demand: only types that are used. ---- */");
    for (size_t i = 0; i < g->descs.len; i++) {  /* all forward-declared: order does not matter */
        const char *tn = (*(Type **)vecAt(&g->descs, i))->name;
        size_t pb = g->out->len;
        cgLine(g, "static const ExtcDesc %s_desc;", tn);
        deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", tn), pb);
    }
    cgLine(g, "");
    emitDescDefs(g);                       /* real pass: emit the definitions */
    cgLine(g, "");
    g->out = saved;
}

/* Per-type print functions are gone: printing is descriptor data plus one
 * `extc_print`, shared by the whole program. No `_debug`, `_writeText` or
 * `_name` is derived per type.
 */

/* The operator rule comes from `typeSupportsOp`, the same predicate the checker applies to
 * concrete types, so there is one rule instead of two that can disagree. Answering "can C
 * compare this type natively with `==`?" by kind alone says yes for every builtin and every
 * enum, so an instance of a generic that applied `%` to an `f64` - which the checker rejects,
 * because `%` is for integers - would be emitted as C `%` and fail inside gcc, in a file the
 * user cannot read.
 */

/**
 * @brief Find a user-defined operator method on a type.
 *
 * It goes through the checker's own lookup, `findOperator`, instead of keeping a second copy here: a
 * hand-written scan does not substitute a generic receiver's type arguments and would miss methods
 * on `slice<u8> == slice<u8>`, leaving the caller to compare two structs with bare C `==`.
 *
 * @param tt        The type table the method lookup runs in.
 * @param t         The operator's receiver type; `NULL` when `ttBase(t)` is neither a struct nor a
 *                  generic instance.
 * @param sym       The symbol to find, for example `"=="`.
 * @param rhs       The right operand's type, which takes part in overload resolution.
 * @param fallback  The symbol to try when `sym` does not exist, for example `"=="` while looking
 *                  for `"!="` (the caller negates the result); `NULL` means `sym` only.
 * @return          The method found; `NULL` when neither symbol is defined.
 */
static FuncDef *findOpMethod(TypeTable *tt, Type *t, const char *sym, Type *rhs, const char *fallback) {
    Type *b = ttBase(t);
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return NULL;
    /* Use the checker's own lookup rather than scanning the method list here: a hand-rolled scan
     * that compares the operand type without substituting a generic receiver's arguments would
     * miss `slice<u8> == slice<u8>` and let the caller fall back to a bare C `==` on two structs. */
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}

/* `typeHasEq`, which recursively decided whether an element type could be
 * compared, is gone: code generation derives no `_eq` for arrays, so
 * nothing on this side asks the question. Comparability is decided by the
 * checker, which reports ``[2]p` cannot be compared: its element type `p` does
 * not define `==` `` when it is violated. One copy of the rule cannot drift out
 * of sync with itself, so that class of bug is gone by construction.
 */

/* ------------------------------------------------------------- structural `==`
 *
 * Every type goes through the generic `extc_eq` plus its descriptor; no
 * `<T>_eq` function is derived per array type.
 *
 * `extc_eq(a, b, desc)` takes addresses, so each operand must be a place; an
 * operand that is not one is first stored in a temporary through the statement
 * prefix, for the same reason `println` does it.
 */
/**
 * @brief Return the address of an operand to pass to `extc_eq`.
 *
 * `extc_eq` takes addresses, so the operand must be a place. One that is not a place is copied
 * into a fresh temporary through the statement prefix, which is what makes the copy happen
 * before the enclosing statement runs.
 *
 * @param g  The generator; it supplies the temporary counter.
 * @param x  The operand expression.
 * @param t  The static type of `x`; it types the temporary.
 * @return A C expression of pointer-to-`t` type: `&(code)` for a place, otherwise the address
 *         of the temporary.
 */
static const char *eqOperand(CG *g, Expr *x, Type *t) {
    const char *code = genExpr(g, x);
    if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
    const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);
    return arenaPrintf(g->arena, "&%s", tmp);
}

/**
 * @brief Emit a structural equality test as one `extc_eq` call.
 *
 * Both sides are evaluated exactly once and in source order, temporaries included.
 *
 * @param g    The generator.
 * @param e    The binary `==` or `!=` expression.
 * @param arr  The static type of the operands, that is the type being compared.
 * @return The call expression; the caller negates it for `!=`.
 */
static const char *genEqCall(CG *g, Expr *e, Type *arr) {
    const char *l = eqOperand(g, e->u.bin.left, arr);
    const char *r = eqOperand(g, e->u.bin.right, arr);
    return arenaPrintf(g->arena, "extc_eq(%s, %s, %s)", l, r, descRef(g, arr));
}

/**
 * @brief Emit the equality adapter of a struct.
 *
 * The adapter connects the user-written `fn ==` to the `bool (*)(const void *, const void *)`
 * shape `extc_eq` expects. Only a struct needs one: its comparison is arbitrary user code that
 * can only be delegated to.
 *
 * Taking the address of an operand must follow the method signature exactly: each of the first
 * two parameters is passed by address when it is declared `ref`, and by value otherwise.
 *
 * @param g  The generator.
 * @param t  The struct type.
 * @param m  Its `==` method; its first two parameters are the operands.
 */
static void genEqAdapter(CG *g, Type *t, FuncDef *m) {
    const char *tn = cType(g, t);
    cgLine(g, "static bool %s_eqD(const void *a, const void *b) {", t->name);
    g->indent++;
    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    const char *a = p0->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)a", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)a", tn);
    const char *b = p1->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)b", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)b", tn);
    cgLine(g, "return %s(%s, %s);", cMethodName(g, t, m), a, b);
    g->indent--;
    cgLine(g, "}");
}

/* Array equality goes through genEqCall and the generic `extc_eq`, and struct
 * or slice equality is emitted where the user's method is called, in genBin
 * below.
 */

/* The single owner of the implicit trailing arguments (home arena / zone). Declared up here because
 * the operator path -- an operator is a method call too -- asks it from inside `genBin`, which sits
 * above the definition. */
static void cgImplicitArgs(CG *g, Buf *b, const FuncDef *callee, const Expr *site, size_t nargs,
                           bool forSignature);

/**
 * @brief Wrap an emitted integer expression back to its declared width when that width is
 *        narrower than C's `int`.
 *
 * The checker types `a + a` for `a: i8` as `i8`, but C promotes both operands to `int`, so the
 * arithmetic value and the stored value would disagree and `a + a == x` would be false. Narrow
 * arithmetic therefore wraps at its own width, the rule the 32/64-bit widths already get from
 * `-fwrapv`.
 *
 * Only 8- and 16-bit results are touched: wider types are handled by `-fwrapv`, and a
 * comparison's type is `bool`, so it never reaches here. The cast is free in C.
 *
 * @param g     The generator.
 * @param t     The declared result type; NULL and non-integer types pass through unchanged.
 * @param code  The C expression to wrap.
 * @return `code` cast to the declared type, or `code` unchanged when no wrap is needed.
 */
static const char *narrowWrap(CG *g, Type *t, const char *code) {
    t = t ? subst(g, t) : NULL;
    if (!t || t->kind != TY_BUILTIN || !t->name) return code;
    const char *nm = t->name;
    if (nm[0] != 'i' && nm[0] != 'u') return code;
    int bits = 0;
    for (const char *q = nm + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
    if (bits != 8 && bits != 16) return code;
    return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), code);
}

/**
 * @brief Emit a binary operation.
 *
 * An overloadable operator the checker already resolved carries the method to call in
 * `planCallee(e)`, which covers `==` `!=` and the ordering and arithmetic operators. The same
 * operators are emitted from here when the operand was a type parameter inside a generic:
 * `planNeedOp(e)` says the decision was deferred, and the instance context is what makes `T`
 * resolvable at that point. Array equality becomes one generic `extc_eq` call; integer division, modulo
 * and shifts go through the trapping helpers unless `@unchecked` is in effect.
 *
 * @param g  The generator.
 * @param e  The binary expression.
 * @return A C expression; the caller supplies the surrounding syntax.
 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;
    /* An array has no `sdef` and therefore no method: `==` and `!=` on arrays are emitted here
     * through `extc_eq`. The condition must not also require `!planNeedOp(e)`: a comparison
     * inside a generic is resolved at instantiation, and after substitution the element type
     * can well turn out to be an array - comparing two rows of a `slice<[6]i32>` is that case -
     * and adding it would report "`array_6_i32` needs to define `!=`" instead. */
    if (!planCallee(e)) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ANY && isEqualityOp(op)) {
            g->needAny  = true;
            g->needAnyEq = true;
            const char *l = eqOperand(g, e->u.bin.left, lt);
            const char *r = eqOperand(g, e->u.bin.right, lt);
            const char *call = arenaPrintf(g->arena, "extc_any_eq(%s, %s)", l, r);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
        if (lt && lt->kind == TY_ARRAY &&
            isEqualityOp(op)) {
            needEq(g, lt);                 /* register: needs to be comparable via extc_eq */
            const char *call = genEqCall(g, e, lt);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
    }

    if (planCallee(e) || planNeedOp(e)) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        FuncDef *m = planCallee(e)
                     ? planCallee(e)
                     : findOpMethod(g->tt, lt, op, ttBase(subst(g, e->u.bin.right->type)),
                                    strcmp(op, "!=") == 0 ? "==" : NULL);

        if (!m) {
            /* `typeSupportsOp` is the checker's own predicate for both halves, so the
             * operator rules are not decided again here. `m` is NULL and the lookup already
             * tried the `!=` -> `==` fallback, so a true answer here means "native". */
            if (typeSupportsOp(g->tt, lt, op, ttBase(subst(g, e->u.bin.right->type))))
                return arenaPrintf(g->arena, "(%s %s %s)",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right));

            ctxError(g->ctx, e->line, 1,
                     arenaPrintf(g->arena,
                        "`%s` inside a generic is checked at instantiation, not on the template -- the price of having no traits. "
                        "Add a `fn %s` to that type.", op, op),
                     "`%s` needs to define `%s`", cType(g, lt), op);
            return "0";
        }

        Param *p0 = *(Param **)vecAt(&m->params, 0);
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        /* Evaluation order: C leaves argument order unspecified (gcc goes right to left), and here one
         * operator is one call. So in `cout << "x" << a() << b()` b() would run first, and
         * `<< dl::close(h)` would release before use. **The left half is materialized into a
         * statement first**, so the side effects of every link happen in source order, left to right.
         *
         * The left half is not materialized when it is a place: its evaluation has no side effect,
         * and keeping its address is what makes the write parameters (such as `+=`) land on the
         * original object. A receiver that is a place still goes to selfOperandAsParam. */
        const char *l = genExpr(g, e->u.bin.left);
        Type *lT = subst(g, e->u.bin.left->type);
        const bool lIsPlace = printArgIsPlace(e->u.bin.left);
        const bool wantRef0 = p0->type && p0->type->kind == TY_REF;
        const bool haveRef0 = lT && lT->kind == TY_REF;
        if (!lIsPlace && g->prefixBlk > 0 && !(lT && lT->kind == TY_ARRAY)) {
            const char *tmp = arenaPrintf(g->arena, "__extc_op%d", g->tmpSeq++);
            pfLine(g, "%s %s = %s;", cType(g, lT), tmp, l);
            if (wantRef0 && !haveRef0)      l = arenaPrintf(g->arena, "&%s", tmp);
            else if (!wantRef0 && haveRef0) l = arenaPrintf(g->arena, "*(%s)", tmp);
            else                            l = tmp;
        } else {
            /* The left operand is the method receiver, so it obeys the receiver rule: a value
             * that is not a place, such as the result of a chained `<<`, has to be materialized. */
            l = selfOperandAsParam(g, e->u.bin.left, l, p0, e->u.bin.left->type);
        }
        const char *r = genExpr(g, e->u.bin.right);
        if (p1->type->kind == TY_REF) r = arenaPrintf(g->arena, "&(%s)", r);

        /* An operator is a method call, so it asks the same single owner for the hidden
         * arguments (`cgImplicitArgs`); the checker already decided the level for it
         * (`setOpCallArgs`). Both halves must be emitted, or the generated C calls the method
         * with too few arguments. */
        Buf cb;
        bufInit(&cb, g->arena);
        bufPrintf(&cb, "%s(%s, %s", cMethodName(g, e->u.bin.left->type, m), l, r);
        cgImplicitArgs(g, &cb, m, e, 2, false);   /* 2 = the two operands already emitted */
        bufPutc(&cb, ')');
        const char *call = bufCstr(&cb);
        return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
    }

    /* Division by zero and an over-wide shift are undefined behaviour in C, so
     * they trap with a source position instead of passing silently. Each
     * operand is evaluated exactly once, which is why they go through a helper
     * rather than a comma expression such as `(check(r), l op r)` that would
     * evaluate one operand twice.
     *
     * `@unchecked` signs the checks away: the raw C operator is emitted and the
     * helper is never named (so it is not even defined in the product). */
    if (!g->uncheckedBody) {
        Type *lt   = ttBase(subst(g, e->u.bin.left->type));
        bool  sint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'i';
        bool  uint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'u';

        if ((sint || uint) && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)) {
            const char *fn = strcmp(op, "/") == 0 ? (sint ? "extc_divI" : "extc_divU")
                                                  : (sint ? "extc_modI" : "extc_modU");
            const char *ity = sint ? "int64_t" : "uint64_t";
            return arenaPrintf(g->arena, "(%s)%s((%s)(%s), (%s)(%s), \"%s\", %d)",
                               cType(g, lt), fn, ity, genExpr(g, e->u.bin.left),
                               ity, genExpr(g, e->u.bin.right), g->path, e->line);
        }
        if ((sint || uint) && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)) {
            int bits = 0;
            for (const char *q = lt->name + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
            if (bits > 0)
                return narrowWrap(g, e->type,
                                  arenaPrintf(g->arena, "(%s %s extc_shiftCount((int64_t)(%s), %d, \"%s\", %d))",
                                              genExpr(g, e->u.bin.left), op,
                                              genExpr(g, e->u.bin.right), bits, g->path, e->line));
        }
    }

    return narrowWrap(g, e->type,
                      arenaPrintf(g->arena, "(%s %s %s)",
                                  genExpr(g, e->u.bin.left), op, genExpr(g, e->u.bin.right)));
}

/* C expression for the zero value of a type.
 *
 * Zero initialization cannot always be `{0}`: `str` is non-nullable, and `{0}`
 * turns a `const char *` into NULL, where `printf("%s", NULL)` is undefined
 * behaviour (glibc happens to print "(null)" and hides the mistake). A struct
 * that contains a `str` therefore spells its zero value out field by field.
 *
 * Returns:
 *   The C initializer expression.
 */
static const char *zeroValue(CG *g, Type *t);

/**
 * @brief Report whether a type contains a `ref`, directly or nested inside a struct.
 *
 * Such a type must not be zero-initialized with `{0}`, which would build a null reference.
 *
 * @param t  The type to inspect; NULL contains no reference.
 * @return true when a `ref` appears anywhere inside the type.
 */
static bool needsExplicitZero(Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    /* An enum's zero is one of its variants, not the integer 0. Naming that variant would
     * remove clang's `-Wimplicit-int-enum-cast`, at the cost of nesting the initializer one
     * level deeper, so the warning is documented and accepted instead. */
    if (t->kind == TY_ENUM || t->kind == TY_ARRAY) return false;
    if (t->kind != TY_STRUCT || !t->sdef) return false;
    for (size_t i = 0; i < t->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
        if (needsExplicitZero(fd->type)) return true;
    }
    return false;
}

/**
 * @brief Enter the substitution context of the fields of a generic instance.
 *
 * The fields must be substituted with that instance's own type arguments, never with whatever
 * context the caller happens to leave behind: `subst` would then return the type unchanged, and
 * zero-value generation would recurse into itself until the stack overflows.
 *
 * @param g      The generator; the previous context is saved into the out parameters.
 * @param sd     The generic struct definition.
 * @param targs  Its type arguments in this instance.
 * @return       The previous context, for `substLeave`.
 */
static SubCtx substEnterInst(CG *g, StructDef *sd, Vec *targs) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };
    g->substParams = &sd->typeParams;
    g->substArgs   = targs;
    return sv;
}

/**
 * @brief Enter the substitution context of a generic free function instance.
 *
 * Without it a `T` in the body of the instance - the `T` of `a == b`, say - stays `TY_PARAM`,
 * which either reports a false error or generates wrong C.
 *
 * @param g      The generator; the previous context is saved into the returned value.
 * @param f      The instance; without a template this changes nothing.
 * @return       The previous context, for `substLeave`.
 */
static SubCtx substEnterFunc(CG *g, FuncDef *f) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };
    if (f && planTemplate(f)) {
        g->substParams = &planTemplate(f)->typeParams;
        g->substArgs   = &f->targs;
    }
    return sv;
}

/* Emit the zero value of a type.
 *
 * This is the recursive implementation; the declaration above records why a
 * `{0}` is not always good enough.
 */
/**
 * @brief Emit a null pointer with the type the context expects.
 *
 * `((void *)0)` is a fine null in C, but assigning it to a typed pointer is an implicit `void *`
 * conversion that clang's `-Weverything` reports; `(node *)0` says the same thing in the type
 * the reader expects.
 *
 * @param g  The generator.
 * @param t  The reference type; a non-reference type yields the untyped null.
 * @return `((inner *)0)` for a reference to `inner`, `((void *)0)` otherwise.
 */
static const char *nullValue(CG *g, Type *t) {
    if (t && t->kind == TY_REF && t->inner) return arenaPrintf(g->arena, "((%s *)0)", cType(g, t->inner));
    return "((void *)0)";
}

/**
 * @brief Return the braces a zero value of type `t` needs when it sits inside another
 *        initializer.
 *
 * An array type is a struct wrapping `T data[N]`, so it brings two levels here (its own braces
 * and the member array's); a struct brings one. The outermost level costs nothing: it is the
 * compound literal's own brace, which is why `zeroValue` writes `(T){ { ... } }` for an array.
 *
 * The counts answer gcc's `-Wmissing-braces`, which `-Wall` enables and which asks for a brace
 * per aggregate; clang accepts any of them.
 *
 * @param g  The generator.
 * @param t  The element type; NULL yields the scalar `0`.
 * @return A C initializer fragment for one element of type `t`.
 */
static const char *elemBraces(CG *g, Type *t) {
    if (!t) return "0";
    if (t->kind == TY_ARRAY) return arenaPrintf(g->arena, "{ { %s } }", elemBraces(g, t->inner));
    /* An enum with payload is a `struct { tag; union }` in the generated C, so it brings a brace
     * like any other struct: `option<i64>` as an element needs `{ { { 0 } } }` around it. */
    if (t->kind == TY_ENUM && enumHasPayload(t->edef)) return "{ 0 }";
    if ((t->kind == TY_STRUCT || t->kind == TY_GENERIC) && t->sdef && t->sdef->fields.len > 0) {
        FieldDef *f0 = *(FieldDef **)vecAt(&t->sdef->fields, 0);
        if (f0->type && (f0->type->kind == TY_ARRAY ||
                         ((f0->type->kind == TY_STRUCT || f0->type->kind == TY_GENERIC) &&
                          f0->type->sdef && f0->type->sdef->fields.len > 0)))
            return arenaPrintf(g->arena, "{ %s }", elemBraces(g, f0->type));
        return "{ 0 }";        /* a struct that starts with a scalar: one brace, then the zero */
    }
    return "0";                /* scalar, enum, bool, a reference: the element itself */
}

/**
 * @brief Emit the zero value of a type.
 *
 * `{0}` is not always a valid initializer: a type that carries a non-nullable `ref` would get a
 * null reference, so it is spelled out field by field instead. A generic instance is always
 * spelled out, because its field types have to be substituted with that instance's own type
 * arguments.
 *
 * @param g  The generator.
 * @param t  The type to zero-initialize; it is substituted before it is inspected.
 * @return A C initializer expression allocated in the generator's arena.
 */
static const char *zeroValue(CG *g, Type *t) {
    /* Substitute first, exactly as `cType` does: `t` can still carry the template type of a
     * generic function (`box<T>`, whose `targs` is `[T]`), and the instance context below would
     * substitute `T` with `T`, leaving the initializer named after an undeclared type while the
     * declaration beside it names the instance. */
    t = subst(g, t);
    if (!t) return "0";
    /* The zero value of an enum with payloads is tag 0 with a cleared payload,
     * which is exactly what `(shape){0}` means: C clears the tag and the whole
     * union, and the tag decides which member is the meaningful one. A `ref`
     * inside the payload of tag 0 is reported by the checker as a type that
     * cannot be zero-initialized. */
    if (t->kind == TY_ENUM)
        return enumHasPayload(t->edef) ? arenaPrintf(g->arena, "(%s){0}", t->name) : "0";
    /* An array is a struct wrapping `data[N]`, so its braces have to follow the nesting or gcc
     * asks for them (`-Wmissing-braces`; clang takes any of these). The element's zero value is
     * the ordinary one, already braced for structs by the cases below. */
    if (t->kind == TY_ARRAY)
        return arenaPrintf(g->arena, "(%s){ { %s } }", t->name, elemBraces(g, t->inner));

    if (t->kind == TY_PARAM) {
        Type *a = subst(g, t);
        if (a == t) return "0";     /* no context: cannot happen, but never loop forever */
        return zeroValue(g, a);
    }

    /* A generic instance substitutes its own type arguments into the field
     * types and then spells the zero value out field by field. */
    if (t->kind == TY_GENERIC && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", cType(g, t));

        SubCtx sv = substEnterInst(g, sd, &t->targs);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", cType(g, t));
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");

        substLeave(g, sv);
        return bufCstr(&b);
    }

    if (t->kind == TY_STRUCT && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0 || !needsExplicitZero(t))
            return arenaPrintf(g->arena, "(%s){0}", sd->name);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", sd->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");
        return bufCstr(&b);
    }

    if (ttIs(t, "bool")) return "false";

    /* Defensive: a non-nullable `ref` has no zero value, so an identifier that does not exist
     * is emitted and the C compiler reports it instead of a null reference being inserted
     * quietly. The checker refuses zero-initialization of a struct that contains such a field,
     * so this is unreachable unless another path slips through. A `?ref T` does have one: `null`. */
    if (t->kind == TY_REF) return t->nullable ? nullValue(g, t)
                                              : "__extc_reference_has_no_zero_value__";

    /* Same defensive shape as the `ref` case, for a stronger reason: the `0` that would fall out
     * below is a null function pointer, and calling one jumps to address zero. The checker
     * refuses to zero-initialize a `fn`, so this is unreachable; the C compiler names the
     * missing identifier instead. */
    if (t->kind == TY_FN) return "__extc_fn_has_no_zero_value__";

    return "0";
}

/**
 * @brief Return the zero value of a type as an initializer.
 *
 * @param g  The generator.
 * @param t  The type to zero-initialize.
 * @return The initializer expression produced by `zeroValue`.
 */
static const char *zeroInit(CG *g, Type *t) {
    return zeroValue(g, t);
}

/**
 * @brief Report whether the generated C expression of a `println` argument is an lvalue.
 *
 * This is the one owner of that question, and it answers for the C form, not the extC form: the
 * slice expression `s[0..5]` is a place in extC, but its C form is a function call, and `&` on
 * a call is illegal; an enum variant such as `color.red` has already been replaced by a
 * constructor. `selfOperandAsParam` asks the same question.
 *
 * @param e  The argument expression.
 * @return false whenever the answer is uncertain: a wrong false merely costs one extra copy,
 *         while a wrong true produces C that does not compile.
 */
static bool printArgIsPlace(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;                                  /* root of `x` / `p.f` */
    case EX_FIELD: return printArgIsPlace(e->u.field.obj);
    case EX_INDEX: return printArgIsPlace(e->u.index.obj);
    default:       return false;
    }
}

/**
 * @brief Emit the expression that implements one `print` call.
 *
 * A structured argument - enum, byte view, struct, generic instance or array - is printed
 * through its descriptor as `extc_print(&x, &x_desc)`, so per-type print code disappears and
 * one descriptor per type is left. That call needs an address, so an argument that is not a
 * place is first stored in a temporary through the statement prefix. A C compound literal
 * cannot be used instead: C refuses to initialize an aggregate from an expression of the same
 * type.
 *
 * @param g        The generator.
 * @param args     The argument expressions of the call.
 * @param newline  Append a trailing newline as well.
 * @return The generated expression, allocated in the generator's arena.
 *
 * Sets `needRuntime` and `needPrint`, which pull the descriptor table and `extc_print` into the
 * output.
 */
static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    /* Built-in `print`/`println` and `io::cout` go through the **same buffer** (`extc_printf` /
     * `extc_cout_put`), so mixed writes keep source order by construction and need no `fflush`.
     * Only structured types (enum/struct/array/non-byte view) still go to the descriptor printer,
     * which writes the same buffer. */
    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(subst(g, a->type));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt) { bufPuts(&b, "0"); continue; }

        if (bt->kind == TY_BUILTIN) {
            g->needCout = true;
            if (strcmp(bt->name, "bool") == 0) {
                bufPrintf(&b, "extc_printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
                continue;
            }
            const PrintFmt *pf = NULL;
            for (size_t k = 0; PRINT_FMT[k].extc; k++)
                if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
            if (!pf) { bufPuts(&b, "0"); continue; }
            bufPrintf(&b, "extc_printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
            continue;
        }

        /* byte views (`slice<u8>` and string literals): write these bytes directly instead of printing element by element */
        if (isByteView(bt)) {
            g->needCout = true;
            const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
            pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
            bufPrintf(&b, "extc_cout_put((uint8_t *)(%s).data, (%s).len)", tmp, tmp);
            continue;
        }

        /* An `any` prints through its own descriptor, which is exactly the type information the
         * program carries at that point; the value may be an empty slot, and that reads as
         * `<none>` rather than nothing at all. */
        if (bt->kind == TY_ANY) {
            g->needAny     = true;
            g->needRuntime = true;
            g->needPrint   = true;
            g->needCout    = true;
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_any_print(&(%s))", code);
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "ExtcAny %s = %s;", tmp, code);
                bufPrintf(&b, "extc_any_print(&%s)", tmp);
            }
            continue;
        }

        if (bt->kind == TY_ENUM || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;
            g->needPrint   = true;
            /* The descriptor printer writes the cout buffer too (`extc_print` is all `extc_printf`) =>
             * that part of the runtime must be emitted with it, or `extc_printf` has no definition. */
            g->needCout    = true;
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_print(&(%s), %s)", code, descRef(g, bt));
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
                bufPrintf(&b, "extc_print(&%s, %s)", tmp, descRef(g, bt));
            }
            continue;
        }
        bufPuts(&b, "0");
    }

    g->needCout = true;
    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "extc_cout_put((uint8_t *)\"\\n\", 1)");
    } else if (args->len == 0) {
        bufPuts(&b, "extc_cout_put((uint8_t *)\"\", 0)");
    }

    bufPutc(&b, ')');
    return bufCstr(&b);
}

static const char *homeArg(CG *g, int marked);   /* defined below */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome);
/* ---- the implicit trailing arguments: **one owner** ----
 *
 * Two kinds exist -- the home arena (`usesHome`, `extc_arena *__extc_home`) and the zone
 * (`makesPool`, `int64_t __extc_home_zone`) -- and **both sides have to agree**: a call site must
 * pass what the definition's signature declares, or the generated C does not even compile
 * (`too few arguments to function ...`). Splitting that agreement across the call emitters, the
 * assoc path, the coroutine path and the signature emitter is what lets a definition with
 * `__extc_home` and a call site that passes nothing drift apart.
 *
 * Every site asks these two functions instead. The compiler's own speed is irrelevant at this
 * scale; the emitted C is what runs, and decoupling the convention from its consumers is what keeps
 * this file maintainable.
 *
 * `nargs` is how many **source** arguments were already emitted, so the separating comma lands in
 * the right place when there are none. `forSignature` picks the declaration spelling; otherwise
 * `site` supplies the arena level (`homeArg` explains why the level matters, and `zoneArgRef` the
 * zone's). Returns nothing: `cgHasImplicitArgs` is the predicate the `@overwrite` cell pass needs.
 *
 */
/**
 * @brief Report whether a function takes the implicit trailing arguments.
 *
 * @param callee  The function definition; NULL takes none.
 * @return true when `cgImplicitArgs` would append a home arena or a zone argument.
 */
static bool cgHasImplicitArgs(const FuncDef *callee) {
    return callee && (funcTakesHomeArena(callee) || funcTakesHomeZone(callee));
}

/**
 * @brief Generate the C text of the "ownership delegation" argument: does the owner of this home
 *        arena have a live reference slot right now?
 *
 * The home arena is the same one handed down from the caller, and a kernel frame's static root table
 * (`rcFrameOwnsArena` false) cannot see the live references of the calling frame, so a kernel frame
 * does not sweep and only records what it owes (`rcEmitOwe`). But when the **owner itself** has just
 * handed the arena out and holds no live reference slot at this moment, the callee's root table
 * (parameters + locals + globals) is a complete seed: an object live in that region can only be
 * reachable from the callee's parameters, so the right to sweep can be delegated to the callee on
 * the spot instead of owing the collection until the owner's next allocation. Ownership of `parTls`
 * is decided by the runtime depth of `RcOwn` and does not take this route.
 *
 * Only one call-site shape allows delegation: **the call site is the top-level value expression of a
 * whole statement** (`g->topExpr`). A subexpression may still hold a reference into this region that
 * was created earlier in the same expression and lives only in a register
 * (`f(mk(1).v, deep(x))`); the callee sweeping with its own root table would then release a live
 * object as garbage.
 *
 * @param g     The generator.
 * @param site  The call-site expression (`NULL` => conservative).
 * @param hlvl  Which arena level this call hands out (`ARENA_HOME`, or 1 and above).
 * @return      `0` (do not delegate) / `1` (this frame is the owner) / `__extc_homeOwn` (pass on the
 *              value that was received).
 */
static const char *homeOwnArg(CG *g, const Expr *site, int hlvl) {
    if (!g->needReclaim || g->parTls || g->coroFunc) return "0";
    if (g->rcRootsUnreliable) return "0";
    if (g->rcGlobalRoots.len + g->rcRoots.len > 0) return "0";
    if (!site || site != g->topExpr) return "0";
    if (g->inMain) return "1";                      /* main's arena belongs to main itself */
    if (hlvl == ARENA_HOME && g->rcFuncUsesHome) return "__extc_homeOwn";
    return "0";
}
/**
 * @brief Append the implicit trailing arguments, home arena and home zone, according to the callee's
 *        declaration.
 *
 * Both sides must use the same test: the declaration side (`forSignature`) writes the parameter
 * declarations and the call side writes the argument expressions, both appending exactly what
 * `cgHasImplicitArgs` decides; otherwise the generated C has the wrong argument count and does not
 * compile.
 *
 * @param g             The generator.
 * @param b             The call/declaration text being assembled.
 * @param callee        The callee; nothing is appended when it is NULL.
 * @param site          The call-site expression, which decides the arena level and the zone
 *                      argument; may be NULL on the declaration side.
 * @param nargs         The number of **source** arguments already emitted, which decides the
 *                      separating comma.
 * @param forSignature  true writes the parameter spellings (`extc_arena *__extc_home` /
 *                      `int64_t __extc_home_zone`), false writes call arguments.
 */
static void cgImplicitArgs(CG *g, Buf *b, const FuncDef *callee, const Expr *site,
                           size_t nargs, bool forSignature) {
    if (!callee) return;
    size_t n = nargs;
    if (funcTakesHomeArena(callee)) {
        int hlvl = site ? planArenaArg(site) : ARENA_HOME;
        if (n) bufPuts(b, ", ");
        if (forSignature) {
            bufPuts(b, "extc_arena *__extc_home");
        } else {
            const char *harg = homeArg(g, hlvl);
            bufPuts(b, harg);
            /* The owed-collection point of subset B: when an arena **this frame owns** is passed out as the
             * callee's home arena, the callee can only record "one collection owed" on that region
             * (its static root table is incomplete for it). This is the moment, "the previous batch
             * of callees has returned and the next has not been entered", to settle that debt: no
             * callee subtree of this frame is active, so this frame's static root table is a complete
             * seed (see the invariant of `rcEmitOwedCheck`). `main`'s home arena is
             * `&__extc_a[1]` (subset A), so `main` is in this state too. */
            RcOwn own = rcOwnKind(g, hlvl);
            if (own != RC_OWN_NO)   /* the TLS state is emitted; ownership is decided at run time */
                rcEmitOwedCheck(g, arenaPrintf(g->arena, "(void *)(%s)", harg), hlvl, own);
        }
        n++;
        /* The ownership-delegation flag: it follows the home arena argument and both sides must emit it
         * (see `homeOwnArg`). The callee uses it to decide whether its own static root table
         * qualifies to sweep on the spot, so it exists only in a translation unit with
         * `needReclaim`. */
        if (g->needReclaim) {
            if (n) bufPuts(b, ", ");
            bufPuts(b, forSignature ? "EXTC_UNUSED int32_t __extc_homeOwn"
                                    : homeOwnArg(g, site, hlvl));
            n++;
        }
    }
    if (funcTakesHomeZone(callee)) {
        if (n) bufPuts(b, ", ");
        bufPuts(b, forSignature ? "int64_t __extc_home_zone" : zoneArgRef(g, (Expr *)site));
        n++;
    }
}
static bool f_owLocal(CG *g, Stmt *s);
static const char *zoneArgRef(CG *g, Expr *e);

/**
 * @brief Shape the first operand of a call or operator the way the target's `self` parameter
 *        requires.
 *
 * `a.f(x)` is `f(a, x)` and `a << b` is `(<<)(a, b)`: both pass the object as the first
 * argument, as an address when the parameter is a reference and as a value when it is a value.
 *
 * A temporary has no address (`&(f())` is not legal C), so it is materialized first into a
 * one-element array compound literal: `(T[]){ f() }` has type `T *` (the array decays). That is
 * exactly what makes chained calls work: in `cout << a << b` the second `<<` receives the value
 * returned by the first.
 *
 * **Do not write `&((T){ f() })`**: `(T){ x }` is not a copy in C, it initializes the first
 * member with x, which yields a confusing error such as `_Bool has = <option_i64>`; an array
 * initializer is applied per element, so `{ f() }` really is "initialize element 0 with T".
 *
 * An operand that is a place must keep its address: taking the address of a copy turns a write
 * parameter into a write to a temporary.
 *
 * @param g        The generator.
 * @param operand  The expression passed out as the first argument.
 * @param code     Its generated C text.
 * @param p0       The parameter that receives it.
 * @param want     The operand's static type, used for the C type when a temporary is
 *                 materialized.
 * @return         The C text to pass out.
 */
static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want) {
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = want && want->kind == TY_REF;
    if (wantRef && !haveRef) {
        /* The question is whether the **generated C** is an lvalue, not whether the operand is a
         * place in extC: a slice expression is a place in extC while its C form is a call. That is
         * `printArgIsPlace`'s question, so `isPlaceExpr` must not be asked here -- it would emit
         * `&(slice_i32_slice(...))`, which C rejects. */
        if (printArgIsPlace(operand)) return arenaPrintf(g->arena, "&(%s)", code);
        return arenaPrintf(g->arena, "(%s[]){ %s }", cType(g, subst(g, want)), code);
    }
    if (!wantRef && haveRef) return arenaPrintf(g->arena, "*(%s)", code);
    return code;
}

/**
 * @brief Generate `EX_DYN`: turn `dyn Trait(x)` into a value -- copy the payload into the dyn
 *        pool and hand out the handle.
 *
 * The payload goes through a temporary first, so both rvalues and lvalues work; `pfLine` is the
 * mechanism that puts a statement into the current prefix. The payload's C type comes from the
 * checker's `payloadType`: the node's own type is `dyn Trait` by then, while the temporary must
 * have the payload's type.
 *
 * The fourth argument is the zone the pool is registered in: a `dyn` payload pool lives at a
 * **place**, and the place is fixed by the checker's `zoneLevel` (promoted to a longer-lived
 * level when it escapes). The runtime cannot substitute "the current top of stack": after a
 * promotion the construction point is in an inner block while the pool has to live in the outer
 * one, and those are two different things.
 *
 * @param g  The generator.
 * @param e  The `EX_DYN` node.
 * @return   The call expression of `extc_dyn_put` (the handle in the pool).
 */
static const char *genDynValue(CG *g, Expr *e) {
    const char *payC = genExpr(g, e->u.dynv.payload);
    Type *pt = subst(g, e->u.dynv.payloadType);
    const char *ct = cType(g, ttBase(pt));
    const char *tmp = arenaPrintf(g->arena, "__extc_dyp%d", g->tmpSeq++);
    const char *zi  = zoneArgRef(g, e);
    pfLine(g, "%s %s = %s;", ct, tmp, payC);
    return arenaPrintf(g->arena,
                       "extc_dyn_put((const void *)&%s, (int64_t)sizeof(%s), &extc_vt$%s$%s, %s)",
                       tmp, ct, e->u.dynv.traitName, ct, zi);
}

/**
 * @brief Emit the format specifier of an f-string slot: `padText` / `padInt` / `padF` according
 *        to the padding primitive the checker recorded.
 *
 * Arguments are matched by **parameter name** (`padF` has `prec` before `fill` / `align`, so
 * passing by position would misplace them). Width, fill, align and precision are literals by the
 * time this runs; when align is absent (-1) the default follows the value's kind: text to the
 * left, numbers to the right. The type code only exists for the checker's consistency check and
 * is not emitted.
 *
 * @param g  The generator.
 * @param e  The reserved-name EX_METHOD; arguments are the slot value, width, fill byte, align,
 *           precision and type code, in that order.
 * @return   A C call expression; `"0"` when nothing is recorded on the plan side or the method
 *           does not match.
 */
static const char *genPadCall(CG *g, Expr *e) {
    static const char *const padNames[] = {"", "padText", "padInt", "padF"};
    PlanPadKind kind = planPadKind(e);
    if (kind <= PLAN_PAD_NONE || kind > PLAN_PAD_FLOAT) return "0";
    Type *recvT = subst(g, e->u.method.recv->type);
    FuncDef *f = findMethod(ttBase(recvT), padNames[kind]);
    if (!f || f->params.len < 2) return "0";
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);
    recvC = selfOperandAsParam(g, e->u.method.recv, recvC, p0, recvT);
    const char *fname = cMethodName(g, recvT, f);
    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 1; i < f->params.len; i++) {
        Param *pm = *(Param **)vecAt(&f->params, i);
        int slot = -1;
        if (pm->name && strcmp(pm->name, "v") == 0) slot = 0;
        else if (pm->name && strcmp(pm->name, "width") == 0) slot = 1;
        else if (pm->name && strcmp(pm->name, "fill") == 0) slot = 2;
        else if (pm->name && strcmp(pm->name, "align") == 0) slot = 3;
        else if (pm->name && strcmp(pm->name, "prec") == 0) slot = 4;
        if (slot < 0 || slot >= (int)e->u.method.args.len) return "0";
        Expr *ae = *(Expr **)vecAt(&e->u.method.args, slot);
        if (slot == 3 && ae->kind == EX_INT && ae->u.ival < 0) {
            /* align absent: text to the left, numbers to the right */
            bufPrintf(&b, ", %s", kind == PLAN_PAD_TEXT ? "0" : "1");
            continue;
        }
        const char *at = genExpr(g, ae);
        if (slot == 0 && kind == PLAN_PAD_INT) bufPrintf(&b, ", (int64_t)(%s)", at);
        else if (slot == 0 && kind == PLAN_PAD_FLOAT) bufPrintf(&b, ", (double)(%s)", at);
        else bufPrintf(&b, ", %s", at);
    }
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Emit a call to a dynamically dispatched method: `v.m()` where `v` is an `any`.
 *
 * Nothing here knows which method runs: the helper named by this site switches on the receiver's
 * descriptor and calls that type's method directly (no table of function pointers). The result is
 * boxed into an `any`, because the checker typed the call as `any` -- the caller unboxes it, and a
 * wrong belief traps there.
 *
 * The site is appended to `g->dynCalls` and its dispatcher is generated by `emitDynCalls` once the
 * `any` runtime exists in the unit. One dispatcher per emission: a generic body containing such a
 * call is emitted once per instance, and each instance has its own candidate list.
 *
 * @param g  The generator.
 * @param e  The `EX_METHOD` call; its receiver is an `any`.
 * @return   The C expression that yields the boxed result.
 */
static const char *genDynCall(CG *g, Expr *e) {
    const char *recvC = genExpr(g, e->u.method.recv);
    const char *rtmp = arenaPrintf(g->arena, "__extc_dr%zu", g->tmpSeq++);
    pfLine(g, "ExtcAny %s = %s;", rtmp, recvC);
    size_t idx = g->dynCalls.len;
    *(Expr **)vecPush(&g->dynCalls) = e;
    g->needAny = true;
    return arenaPrintf(g->arena, "extc_dyn_%zu(%s, &%s)", idx, homeArg(g, ARENA_HOME), rtmp);
}

/**
 * @brief Generate a method call as an ordinary function call on the receiver.
 *
 * `a.f(x)` is `f(a, x)`: the receiver is shaped to what the first parameter requires (an
 * address for a reference, the value itself for a value), the remaining arguments are expanded
 * as they are, and the callee's implicit trailing arguments and `@overwrite` unit are appended
 * last.
 *
 * Inside a generic body the written receiver type may still contain `T`, and a call node the
 * checker deferred carries no `func` at all: the method is resolved here against the
 * **substituted** type. One template body has many instances, so a `func` recorded on the node
 * would be the wrong method for the others.
 *
 * A coroutine handle receiver (`coroutine<T>`) goes through the `next`/`value`/`send` handle
 * protocol; a callee that belongs to a coroutine goes through the driver protocol; a `dyn Trait`
 * receiver takes the function pointer from the trait's uniform table.
 *
 * @param g  The generator.
 * @param e  The `EX_METHOD` node.
 * @return   The C call expression; `"0"` when the checker resolved no method or the method has
 *           no parameters.
 */
static const char *genMethodCall(CG *g, Expr *e) {
    /* The receiver's type as this instance sees it. Inside a generic body the written type may
     * still mention `T`, and for a deferred call (a method on a type parameter) the node carries
     * no `func` at all: the method is resolved here, on the substituted type, the same way the
     * operators are resolved. The checker deliberately leaves `planCallee(e)` alone -- one
     * template body has many instances, so a `func` stored there would be the wrong method for
     * all but the last one. */
    Type *recvT = subst(g, e->u.method.recv->type);
    /* An f-string slot's format specifier is a reserved-name call the parser desugars: the padding
     * primitive is recorded on the plan side and emitted from that choice directly, not through
     * ordinary method resolution. */
    if (e->u.method.name && strcmp(e->u.method.name, "__extc_pad") == 0)
        return genPadCall(g, e);
    /* `v.m()` where `v` is an `any`: the descriptor picks the method at run time. */
    if (planDynCall(e)) return genDynCall(g, e);
    /* `f.call(x)` where `f` holds a `fn(...)` pointer: a deferred call whose instance turned out to
     * be a bare code pointer. There is no method to name -- C calls through the pointer. The
     * parentheses keep a compound receiver from binding to the argument list. */
    if (e->u.method.name && strcmp(e->u.method.name, "call") == 0 && ttBase(recvT) &&
        ttBase(recvT)->kind == TY_FN) {
        Buf fb;
        bufInit(&fb, g->arena);
        bufPrintf(&fb, "(%s)(", genExpr(g, e->u.method.recv));
        for (size_t i = 0; i < e->u.method.args.len; i++)
            bufPrintf(&fb, "%s%s", i ? ", " : "",
                      genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
        bufPuts(&fb, ")");
        return bufCstr(&fb);
    }
    FuncDef *f = planCallee(e);
    if (!f) f = findMethod(ttBase(recvT), e->u.method.name);
    /* The **handle** protocol: the receiver is a `coroutine<T>` (the marker type, whatever the
     * resolved method's owner looks like -- instantiated copies carry a decorated name) and the
     * emitted call dispatches on `kind`. The frame protocol below is unaffected: a frame's type is
     * not a coroutine marker. This test comes before the other early returns so nothing else can
     * claim the call. */
    if (e->u.method.name && f &&
        (strcmp(e->u.method.name, "next") == 0 || strcmp(e->u.method.name, "value") == 0 ||
         strcmp(e->u.method.name, "send") == 0) &&
        isProtoType(ttBase(recvT), "coroutine", 1)) {
        const char *rr = genExpr(g, e->u.method.recv);
        const bool viaRef = e->u.method.recv->type && e->u.method.recv->type->kind == TY_REF;
        g->needCoroHandle = true;
        if (strcmp(e->u.method.name, "next") == 0)
            return arenaPrintf(g->arena, "extc_coro_next(%s%s, \"%s\", %d)",
                               viaRef ? "" : "&", rr, g->path, e->line);
        if (strcmp(e->u.method.name, "send") == 0) {
            Type *rb2 = ttBase(recvT);
            Type *yt2 = (rb2 && rb2->targs.len) ? subst(g, *(Type **)vecAt(&rb2->targs, 0)) : f->ret;
            const char *vv = e->u.method.args.len
                                 ? genExpr(g, *(Expr **)vecAt(&e->u.method.args, 0)) : "0";
            return arenaPrintf(g->arena, "extc_coro_send_%s(%s%s, %s, \"%s\", %d)",
                               cType(g, yt2), viaRef ? "" : "&", rr, vv, g->path, e->line);
        }
        /* `T` comes from the receiver's own type: the resolved instance method's `ret` may still be
         * the error type, and the helper is named after the coroutine's yield type. */
        Type *rb = ttBase(recvT);
        Type *yt = (rb && rb->targs.len) ? subst(g, *(Type **)vecAt(&rb->targs, 0)) : f->ret;
        return arenaPrintf(g->arena, "extc_coro_value_%s(%s%s, \"%s\", %d)",
                           cType(g, yt), viaRef ? "" : "&", rr, g->path, e->line);
    }
    if (f && planCoroProto(f) && f->owner && planCoroOf(f->owner)) {
        /* Driving a coroutine: the two protocol methods are the state machine's calling
         * convention, emitted inline. Whether the receiver is the frame itself or a reference to
         * it decides `&x`/`x.ret` versus `x`/`x->ret`, which is what lets a coroutine be driven
         * through a `mut ref` parameter. */
        Type *rt0 = ttBase(subst(g, e->u.method.recv->type));
        bool viaRef = rt0 && rt0->kind == TY_REF;
        const char *rc = genExpr(g, e->u.method.recv);
        if (planCoroProto(f) == 5) {
            /* `send(v)`: put the value in the frame's `in` slot, then resume exactly like `next`. */
            const char *vv = e->u.method.args.len
                                 ? genExpr(g, *(Expr **)vecAt(&e->u.method.args, 0)) : "0";
            pfLine(g, "%s%s in = %s;", rc, viaRef ? "->" : ".", vv);
        }
        if (planCoroProto(f) == 1 || planCoroProto(f) == 5) {
            /* With a task place the step is driven through the driver (it owns the place); without
             * one there is nothing to set up, so the step itself is the driver. */
            FuncDef *cf = planCoroOf(f->owner);
            return planCoroNeedsZone(cf)
                       ? arenaPrintf(g->arena, "%s$next(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc)
                       : arenaPrintf(g->arena, "%s$step(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc);
        }
        return arenaPrintf(g->arena, "%s%sret", rc, viaRef ? "->" : ".");
    }
    if (!f) return "0";

    /* The checker refuses a receiver-less method through `dyn`, so this only keeps the generator
     * from indexing an empty parameter list. */
    if (f->params.len == 0) return "0";
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* A stored `dyn` value is a handle, not a payload: it is not re-materialised, and its callee
     * is named by the trait's uniform table instead of a per-type C name. Which source it is is
     * decided by the receiver's **type**, not its node kind: the parser rewrites the immediate
     * form's receiver to the payload, so both forms arrive as ordinary expressions and only the
     * type tells them apart (`dyn Tag` is the stored value). */
    bool dynStored = e->dynTrait && ttBase(recvT) && ttBase(recvT)->kind == TY_DYN;
    if (!dynStored)
        recvC = selfOperandAsParam(g, e->u.method.recv, recvC, p0, recvT);

    const char *fname = dynStored ? NULL : cMethodName(g, recvT, f);
    /* `dyn Trait(x).m(...)`: the callee comes from the trait's uniform table instead of a per-type
     * C name. The field **is** the implementation, so the call keeps the same signature --
     * including hidden parameters, which the table's field types picked up when it was emitted.
     * Same receiver, same arguments, same extra parameters, a different callee expression.
     *
     * Two sources: `dyn Trait(x).m(...)` copies the payload into the pool here; `d.m(...)` on a
     * stored value **is** a handle already, so only the checked slot lookup is emitted. Either way
     * the table and the receiver both come from the **checked slot**. */
    if (e->dynTrait) {
        Buf dynSym;
        bufInit(&dynSym, g->arena);
        if (dynStored) {
            /* The receiver may be reached **through a reference** (`fn f(r: ref dyn Tag)`), in
             * which case the C expression is a pointer to the handle while `extc_dyn_slot` takes
             * the handle **by value**: the pointer has to be dereferenced here. */
            const char *handleC = recvC;
            if (planDynRecvViaRef(e))
                handleC = arenaPrintf(g->arena, "(*%s)", recvC);
            const char *dynS = arenaPrintf(g->arena, "__extc_ds%d", g->tmpSeq++);
            pfLine(g, "ExtcDynSlot *%s = extc_dyn_slot(%s, \"%s\", %d);",
                   dynS, handleC, g->path, e->line);
            bufPrintf(&dynSym, "((const struct extc_vt$%s_t *)%s->vt)->%s",
                      e->dynTrait, dynS, e->u.method.name);
            recvC = arenaPrintf(g->arena, "%s->addr", dynS);
        } else {
            const char *dynT = cType(g, ttBase(recvT));
            bufPrintf(&dynSym, "((const struct extc_vt$%s_t *)&extc_vt$%s$%s)->%s",
                      e->dynTrait, e->dynTrait, dynT, e->u.method.name);
            /* The payload is copied into a stack temporary before the call even though nothing
             * owns it: a method that mutates its receiver (`mut ref Self`) would otherwise write
             * through to the **source object**, while the stored form mutates the pool's copy, so
             * the same expression `dyn Tag(b)` would mean two different things. `recvC` is already
             * a pointer to the payload, so this is one copy of `sizeof(T)`. */
            const char *slot = arenaPrintf(g->arena, "__extc_dynp%d", g->tmpSeq++);
            pfLine(g, "%s %s = *(%s);", dynT, slot, recvC);
            recvC = arenaPrintf(g->arena, "(void *)&%s", slot);
        }
        fname = bufCstr(&dynSym);
    }

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
    /* The receiver counts as the shallowest mutable reference argument, so it is the first
     * argument for both the implicit tail parameters and the `@overwrite` cells. */
    cgImplicitArgs(g, &b, f, e, e->u.method.args.len + 1, false);
    owPassCells(g, &b, e, e->u.method.args.len + 1, cgHasImplicitArgs(f));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Find the initializer expression written for a field in a struct literal.
 *
 * @param lit    The struct literal node.
 * @param fname  The field name, compared verbatim.
 * @return       The field's initializer; `NULL` when the literal omits the field, in which case
 *               the caller uses the field's zero value.
 */
static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

/**
 * @brief Generate the C text of a struct literal: a compound literal or a brace initializer
 *        list.
 *
 * Every field is written out and an omitted one becomes its zero value: leaving the omission to
 * C's implicit zeroing would turn an omitted `str` field into NULL.
 *
 * The type is substituted as a whole first: a literal inside a generic instance still records
 * the template type (`result<T,E>`) with parameters as its type arguments, and using it
 * directly makes the zero-value path see a bare `T`, degrade to `0` and write `.value = 0`.
 * Field types are substituted with the instance's own type arguments for the same reason.
 *
 * @param g           The generator.
 * @param e           The `EX_STRUCTLIT` node.
 * @param staticInit  true means this initializes a C static object (a global): `(T)` compound
 *                    literals cannot be used there because C requires a constant initializer and
 *                    a compound literal is not one; the brace form has the same content and
 *                    differs only in that prefix.
 * @return            C text allocated in the generator's arena; `(int){0}` when the type is not
 *                    a struct.
 */
static const char *genStructLitAs(CG *g, Expr *e, bool staticInit) {
    /* Substitute over the whole type first: inside a generic instance the literal still records
     * the template type (`result<T,E>`), whose type arguments are parameters. Taking that type as
     * it is sends the zero-value path a bare `T`, which degrades to `0` and ends up as
     * `.value = 0` in the generated C. */
    Type *t = subst(g, e->type);
    StructDef *sd = (t && (t->kind == TY_STRUCT || t->kind == TY_GENERIC)) ? t->sdef : NULL;
    if (!sd) return "(int){0}";

    const char *cname = cType(g, t);       /* an instance gets its decorated name */
    if (sd->fields.len == 0)
        return staticInit ? "{0}" : arenaPrintf(g->arena, "(%s){0}", cname);

    /* A literal of a generic instance substitutes its own type arguments into
     * the field types as well. */
    Vec *sp = g->substParams, *sa = g->substArgs;
    const char *sn = g->ownerPrefix;
    if (t->kind == TY_GENERIC) {
        g->substParams = &sd->typeParams;
        g->substArgs   = &t->targs;
    }

    Buf b;
    bufInit(&b, g->arena);
    if (staticInit) bufPuts(&b, "{");
    else            bufPrintf(&b, "(%s){", cname);
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        Type *ft = subst(g, fd->type);
        Expr *v = litValueFor(e, fd->name);
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, ".%s = %s", fd->name,
                  v ? genExpr(g, v) : zeroValue(g, ft));
    }
    bufPuts(&b, "}");

    g->substParams = sp;
    g->substArgs   = sa;
    g->ownerPrefix = sn;
    return bufCstr(&b);
}

/**
 * @brief Generate a struct literal as a C compound literal expression.
 *
 * Shares `genStructLitAs` with global initialization and differs only in `staticInit` being
 * false: unwritten fields are filled with `zeroValue`.
 *
 * @param g  The generator.
 * @param e  The struct literal expression.
 * @return   C text of the form `(T){ .field = v, ... }`, allocated in the arena.
 */
static const char *genStructLit(CG *g, Expr *e) { return genStructLitAs(g, e, false); }

/**
 * @brief Generate the initializer of a global variable (a C static object).
 *
 * The initializer must be a constant C can fold at compile time. A struct literal is therefore
 * written as a brace initializer list (see `genStructLitAs`); every other expression reaching
 * here has already been restricted to constants by the checker.
 *
 * @param g  The generator.
 * @param e  The initializer expression.
 * @return   The C text of the initializer.
 */
static const char *genGlobalInit(CG *g, Expr *e) {
    return e->kind == EX_STRUCTLIT ? genStructLitAs(g, e, true) : genExpr(g, e);
}

static const char *arenaRefAt(CG *g, int level);
static const char *rcRegionRef(CG *g, const char *basePtr);
static void arenaDriftCheck(CG *g, Expr *e, const char *what);   /* the arena is picked per level */

/* Emit the C expression for one expression node.
 *
 * Every caller goes through genExpr, which wraps this function to add the
 * automatic dereference; this one only dispatches on the node kind.
 *
 * Returns:
 *   A C expression, or "0" for a node that must not reach a value position.
 */
/**
 * @brief Generate `EX_EXT`: hand a coroutine call to the scheduling domain as one task.
 *
 * The frame is built along the ordinary frame path as a local of this function -- the domain
 * drives it to completion before the enclosing block ends, so it lives exactly long enough --
 * and is then handed to the domain as `(frame, step)`. The value of the whole expression is
 * void.
 *
 * The `EX_EXT` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_EXT` node.
 * @return   Always `"(void)0"`; a compile error is reported when the preconditions do not hold.
 */
static const char *exExt(CG *g, Expr *e) {
    /* `ext f(x)`: start one task -- build the frame as a plain local (the domain drives it
     * before the enclosing block ends, so it stays alive exactly as long as it is needed), then
     * hand it to the domain as `(frame, step)`. The expression's value is void. */
    Expr *in = e->u.ext_.call;
    FuncDef *cf = (in && in->kind == EX_CALL) ? planCallee(in) : NULL;
    if (!cf || !planIsCoro(cf) || !planExtDom(e) || g->coroFunc) {
        ctxError(g->ctx, e->line, 1, NULL,
                 "domain task: only a plain-function context with a coroutine callee is"
                 " implemented so far (nesting a task inside a coroutine is the next step)");
        return "0";
    }
    const char *cn = cFuncName(g, cf);
    const char *tmp = arenaPrintf(g->arena, "__extc_dt%d", g->tmpSeq++);
    const char *sv = arenaPrintf(g->arena, "__extc_dtsv%d", g->tmpSeq++);
    const char *id = arenaPrintf(g->arena, "__extc_dtid%d", g->tmpSeq++);
    const char *zn = arenaPrintf(g->arena, "__extc_dtzn%d", g->tmpSeq++);
    Buf init;
    bufInit(&init, g->arena);
    if (planCoroNeedsZone(cf)) {
        flushPrefix(g);
        cgLine(g, "int64_t %s = extc_zoneTop;", sv);
        cgLine(g, "int64_t %s = extc_task_begin();", id);
        cgLine(g, "int64_t %s = extc_task_zone(%s);", zn, id);
        g->needCoroHandle = true;
        g->needPool = true;
    }
    bufPrintf(&init, "struct %s$frame %s = (struct %s$frame){ .pc = 0", cn, tmp, cn);
    if (planCoroNeedsZone(cf)) bufPrintf(&init, ", .zone = %s, .task = %s", zn, id);
    for (size_t i = 0; i < cf->params.len; i++) {
        Param *pp = *(Param **)vecAt(&cf->params, i);
        Expr *arg = (in->u.call.args.len > i) ? *(Expr **)vecAt(&in->u.call.args, i) : NULL;
        bufPrintf(&init, ", .%s = %s", pp->cname, arg ? genExpr(g, arg) : "0");
    }
    bufPrintf(&init, " };");
    flushPrefix(g);
    cgLine(g, "%s", bufCstr(&init));
    if (planCoroNeedsZone(cf)) cgLine(g, "extc_zoneTop = %s;", sv);
    g->needDomain = true;
    flushPrefix(g);
    cgLine(g, "extc_dom_add(%s, (void *)&%s, %s$domstep);", genExpr(g, planExtDom(e)), tmp, cn);
    return "(void)0";
}

/**
 * @brief Generate `EX_IDENT`: emit the identifier, or a frame field access inside a coroutine
 *        body.
 *
 * A function used as a value contributes its **address**: C would decay a function designator to
 * a pointer anyway, but only writing `&` makes the expression's C type exactly the pointer the
 * `fn` type promises.
 *
 * A function with a hidden home arena/zone is rejected here instead of being emitted: it has one
 * parameter more than the type declares, and those two markers (the call-graph closure) are only
 * final once every function body has been checked -- this is the first point where the answer is
 * final.
 *
 * Parameters and locals that stay live across a `yield` in a coroutine live in the frame (an
 * ordinary struct), and every mention of them becomes a field access; `FuncDef.coroFrame` is the
 * table the checker prepared.
 *
 * The `EX_IDENT` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_IDENT` node.
 * @return   The C expression; `"0"` for a function with hidden arguments.
 */
static const char *exIdent(CG *g, Expr *e) {
    /* A function used as a value: the expression is its **address**. C would also convert a
     * function designator to a pointer by itself, but writing the `&` out is what makes the
     * expression's C type the pointer the `fn` type promises.
     *
     * A function whose C signature cannot be the type's is refused here rather than emitted: a
     * function that takes a hidden home arena or zone has one parameter more than the type says,
     * and those two flags are settled only after every body has been checked (they are transitive
     * closures), so this is the first place the answer is final. A differing pointer type would
     * otherwise be reported against generated code instead of against the line the author wrote. */
    if (planCallee(e)) {
        if (funcTakesHomeArena(planCallee(e)) || funcTakesHomeZone(planCallee(e))) {
            ctxError(g->ctx, e->line, 1,
                     "A function that takes a home arena or zone is compiled with one hidden"
                     " parameter more than its source signature, so its address does not have"
                     " the type this `fn` says. Call it, or write a free function that wraps"
                     " it without the hidden argument.",
                     "`%s` takes a hidden arena parameter, so its address is not a `fn` value",
                     planCallee(e)->name ? planCallee(e)->name : "?");
            return "0";
        }
        return arenaPrintf(g->arena, "&%s", cFuncName(g, planCallee(e)));
    }
    /* `for i in 0..N`: `N` is a `const` parameter, so the number is the instance's argument -- the
     * array type the caller passed. The body is shared, the emission is not. */
    {
        int lp = planLenParam(e);
        if (lp >= 0 && g->curFunc && (size_t)lp < g->curFunc->targs.len) {
            Type *b = *(Type **)vecAt(&g->curFunc->targs, (size_t)lp);
            if (b && b->kind == TY_ARRAY && b->asize >= 0)
                return arenaPrintf(g->arena, "%lld", (long long)b->asize);
            ctxError(g->ctx, e->line, 1, NULL,
                     "the length `%s` could not be inferred from the call: pass an array whose"
                     " length is known", e->u.ident.name);
            return "0";
        }
    }
    const char *cn = planCName(e) ? planCName(e) : e->u.ident.name;
    /* A coroutine's parameters and its live-across-`yield` locals live in the frame, which is a
     * plain struct: every mention of one becomes a field access (`FuncDef.coroFrame` is the list
     * the checker laid out). */
    if (g->coroFunc && cn) {
        for (size_t i = 0; i < g->coroFunc->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
            if (p->cname && strcmp(p->cname, cn) == 0)
                return arenaPrintf(g->arena, "%s->%s", g->coroFrame, cn);
        }
    }
    return cn;
}

/**
 * @brief Generate `EX_CALL`: dispatch to the emission path for the callee's and arguments'
 *        shapes.
 *
 * Handled in order: `domain::single()` domain construction, the plate `viewOf` gate,
 * `parallel::run`, the coroutine handle protocol, a boxed coroutine call, a function-value call,
 * and finally an ordinary function (including the callee's implicit trailing arguments and
 * `@overwrite` unit).
 *
 * The `EX_CALL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_CALL` node.
 * @return   The call expression; `"0"` when no callee was resolved or the shape is unsupported.
 */
static const char *exCall(CG *g, Expr *e) {
    /* `domain::single()`: build a domain (an opaque pointer). Recognized by the **node marker** the
     * checker stamps on it. */
    if (planDomNew(e)) { g->needDomain = true; return "extc_dom_new()"; }
    /* The plate layer's one primitive: a call to the helper the declaration triggers. The callee
     * is a `@builtin` with no body, so there is no `FuncDef` to name -- hence the flag, exactly as
     * `domNew` above. */
    if (planViewOf(e)) {
        Buf vb;
        bufInit(&vb, g->arena);
        bufPuts(&vb, "extc_viewOf(");
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            if (i) bufPuts(&vb, ", ");
            bufPuts(&vb, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
        }
        bufPutc(&vb, ')');
        return bufCstr(&vb);
    }
    if (planParWorker(e)) {
        /* `parallel::run(worker, views..., n, threads)`: ctx is the trampoline's "one field per
         * view" struct, read-only views are passed through as they are and the output one is split
         * into [lo,hi) chunks inside the trampoline; every argument is evaluated **exactly once**.
         * Passing 0 for the chunk lets the runtime pick (about 8 chunks per thread, taken
         * dynamically). */
        FuncDef *wf = planParWorker(e);
        /* Parameter layout: `(id, lo, hi, captures..., out)`. A capture is either a read-only view or
         * a scalar captured by value (only `par` desugaring produces scalars; hand-written workers
         * are all views); the output is the **only** `mut` view among them and is not necessarily
         * last (a hand-written worker may put it between captures). */
        /* Invariant: only the desugared `(id, lo, hi, captures..., out)` shape reaches this
         * branch, so there are at least four parameters. An underflow would walk the parameter
         * vector off its end, so the shape is asserted instead of assumed. */
        EXTC_DBG_ASSERT(wf->params.len >= 4);
        size_t nCaps = wf->params.len - 4, outIdx = 0;
        for (size_t i = 0; i <= nCaps; i++)
            if ((*(Param **)vecAt(&wf->params, 3 + i))->type->mut) outIdx = i;
        const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", wf->name);
        const char *tmp = arenaPrintf(g->arena, "__extc_pc%d", g->tmpSeq++);
        const char *ntmp = arenaPrintf(g->arena, "__extc_pn%d", g->tmpSeq++);
        Buf init;
        bufInit(&init, g->arena);
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&init, "%s.v%zu = (%s)", i ? ", " : "", i,
                      genExpr(g, *(Expr **)vecAt(&e->u.call.args, 1 + i)));
        pfLine(g, "%s %s = { %s };", ctn, tmp, bufCstr(&init));
        pfLine(g, "int64_t %s = (int64_t)(%s);", ntmp,
               genExpr(g, *(Expr **)vecAt(&e->u.call.args, 2 + nCaps)));
        /* Chunks are cut by [lo,hi) => an output shorter than n would be cut out of bounds. It is
         * rejected here (the checks inside the worker are a second line of defence; an
         * `@unchecked` worker has none). */
        pfLine(g, "if (%s > 0 && %s.v%zu.len < %s)"
                  " extc_trapMsg(\"%s\", %d, \"parallel::run: the output view is shorter than n\");",
               ntmp, tmp, outIdx, ntmp, g->path, e->line);
        g->needPar = true;
        /* A worker that allocates takes its memory from the run's region, so the region
         * half of the runtime has to be compiled in for this program. */
        if (planUsesHome(wf)) g->needParRegion = true;
        /* `par` desugaring writes the threads count as the `-1` sentinel: no thread count means "as
         * many cores as this machine can use". At run time a value <= 1 would silently run
         * sequentially, so it is replaced here by `extc_par_default_threads()`. */
        if (e->u.call.args.len > 3 + nCaps) {
            Expr *th = *(Expr **)vecAt(&e->u.call.args, 3 + nCaps);
            if (th && th->kind == EX_INT && th->u.ival == -1)
                return arenaPrintf(g->arena,
                    "extc_par_run(__extc_par_%s, &%s, %s, %s,"
                    " extc_par_default_threads(), 0)",
                    wf->name, tmp, planUsesHome(wf) ? "1" : "0", ntmp);
        }
        return arenaPrintf(g->arena,
            "extc_par_run(__extc_par_%s, &%s, %s, %s, (int64_t)(%s), 0)",
            wf->name, tmp, planUsesHome(wf) ? "1" : "0", ntmp,
            genExpr(g, *(Expr **)vecAt(&e->u.call.args, 3 + nCaps)));
    }
    /* A handle-protocol call can arrive here too (a `ref coroutine<T>` receiver resolves as
     * an ordinary call, not as EX_METHOD), so both paths share the dispatch. */
    if (planCallee(e) && !planIsCoro(planCallee(e)) && e->u.call.callee->kind == EX_FIELD) {
        /* `self` is implicit, so a `ref` receiver arrives as a call with **no** arguments and
         * the receiver sits in the callee's field expression. The receiver's type decides
         * whether this is the handle protocol. */
        Expr *rf = e->u.call.args.len > 0
                       ? *(Expr **)vecAt(&e->u.call.args, 0)
                       : (e->u.call.callee->kind == EX_FIELD ? e->u.call.callee->u.field.obj
                                                             : NULL);
        int hp = 0;
        if (rf && isProtoType(ttBase(subst(g, rf->type)), "coroutine", 1))
            hp = (planCallee(e)->name && strcmp(planCallee(e)->name, "value") == 0) ? 4
               : (planCallee(e)->name && strcmp(planCallee(e)->name, "send") == 0)  ? 5 : 3;
        if (hp) {
            const char *rc = genExpr(g, rf);
            const bool viaRef = rf->type && rf->type->kind == TY_REF;
            g->needCoroHandle = true;
            if (hp == 3)
                return arenaPrintf(g->arena, "extc_coro_next(%s%s, \"%s\", %d)",
                                   viaRef ? "" : "&", rc, g->path, e->line);
            if (hp == 5) {
                /* `self` implicit: the value sits in `args[0]`; explicit: after the receiver. */
                size_t vi = (e->u.call.callee->kind == EX_FIELD) ? 0 : 1;
                const char *vv = (vi < e->u.call.args.len)
                                     ? genExpr(g, *(Expr **)vecAt(&e->u.call.args, vi)) : "0";
                Type *rb2 = ttBase(subst(g, rf->type));
                Type *yt2 = (rb2 && rb2->targs.len)
                                ? subst(g, *(Type **)vecAt(&rb2->targs, 0)) : NULL;
                return arenaPrintf(g->arena, "extc_coro_send_%s(%s%s, %s, \"%s\", %d)",
                                   yt2 ? cType(g, yt2) : "int64_t",
                                   viaRef ? "" : "&", rc, vv, g->path, e->line);
            }
            return arenaPrintf(g->arena, "extc_coro_value_%s(%s%s, \"%s\", %d)",
                               cType(g, subst(g, planCallee(e)->ret)), viaRef ? "" : "&", rc,
                               g->path, e->line);
        }
    }
    if (planCallee(e) && planIsCoro(planCallee(e)) && planBoxedCoro(e)) {
        /* **Boxing**: this coroutine value is stored somewhere that outlives the current C scope,
         * so its frame goes into the task's own place and the value is a `{frame, kind, task}`
         * handle -- a plain value, safe to copy and to store in a container. The setup statements
         * must precede the statement that reads the handle, so they go to the prefix (C11 has no
         * statement expressions). */
        FuncDef *cf = planCallee(e);
        const char *cn = cFuncName(g, cf);
        int sq = g->coroSeq++;
        g->needCoroHandle = true;
        pfLine(g, "int64_t __extc_czh%d = extc_task_begin();", sq);
        pfLine(g, "int64_t __extc_czz%d = extc_task_zone(__extc_czh%d);", sq, sq);
        pfLine(g, "struct %s$frame *__extc_czf%d = (struct %s$frame *)extc_task_alloc("
                   "__extc_czh%d, (int64_t)sizeof(struct %s$frame), \"%s\", %d);",
               cn, sq, cn, sq, cn, g->path, e->line);
        Buf init;
        bufInit(&init, g->arena);
        bufPrintf(&init, "*__extc_czf%d = (struct %s$frame){ .pc = 0, .zone = __extc_czz%d,"
                         " .task = __extc_czh%d", sq, cn, sq, sq);
        for (size_t i = 0; i < cf->params.len; i++) {
            Param *p = *(Param **)vecAt(&cf->params, i);
            Expr *a = i < e->u.call.args.len ? *(Expr **)vecAt(&e->u.call.args, i) : NULL;
            bufPrintf(&init, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
        }
        bufPrintf(&init, " };");
        pfLine(g, "%s", bufCstr(&init));
        g->needCoroHandle = true;
        return arenaPrintf(g->arena,
                           "((extc_coro){ .frame = __extc_czf%d, .kind = %d,"
                           " .task = __extc_czh%d })", sq, planCoroKind(cf), sq);
    }
    if (planCallee(e) && planIsCoro(planCallee(e))) {
        /* A coroutine call that is neither boxed nor driven is a loud failure at the C compiler
         * rather than a silent miscompile -- except under `EXTC_CORO_B1_HARNESS`, which a small C
         * harness defines so it can drive the generated `$step` itself. */
        cgLine(g, "#ifndef EXTC_CORO_B1_HARNESS");
        cgLine(g, "#error \"calling a coroutine lands in slice B2"
                   "\"");
        cgLine(g, "#endif");
        return "0";
    }
    /* A call **through a value** of function type: the callee is an expression holding a code
     * pointer, so there is no C name to print -- C calls through the pointer directly. The
     * checker has already matched every argument against the signature the type carries; this only
     * prints the expression. The parentheses are not decoration: a callee that is a compound
     * expression or a dereference must not bind to the argument list. */
    if (planCallViaFn(e)) {
        Buf fb;
        bufInit(&fb, g->arena);
        bufPutc(&fb, '(');
        bufPuts(&fb, genExpr(g, e->u.call.callee));
        bufPuts(&fb, ")(");
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            if (i) bufPuts(&fb, ", ");
            bufPuts(&fb, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
        }
        bufPutc(&fb, ')');
        return bufCstr(&fb);
    }
    if (e->u.call.callee->kind != EX_IDENT) return "0";
    const char *name = e->u.call.callee->u.ident.name;
    if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
    if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);
    /* `flush()` becomes `fflush(NULL)`; <stdio.h> is already included
     * by the runtime. */
    /* `flush()` only has to drain the cout buffer (the built-in printing writes to that buffer
     * too); `fflush` stays for external code that writes bytes through stdio. */
    if (strcmp(name, "flush") == 0) {
        g->needCout = true;
        return "(extc_cout_flush(), fflush((void *)0))";
    }
    if (!planCallee(e)) return "0";

    /* Only `cSymName` is used, without the owner prefix: a call site names the callee itself,
     * whereas `ownerPrefix` is the instance currently being generated. This also renames a name
     * that collides with a C keyword (`fn double`).
     *
     * `@export` has to be asked first: the definition names the C symbol after the author's
     * spelling with the module prefix dropped (`cFuncName`'s export branch), while a cross-module
     * call site spells the identifier `lib$exported`. Printing that identifier verbatim calls a
     * symbol nobody defines. */
    FuncDef *ce = planCallee(e);
    if (ce && ce->isExport)
        name = exportName(ce);
    else
        name = (ce && planInstName(ce)) ? planInstName(ce) : cSymName(g, name);
    /* The runtime layer: the library's `extc_pool_new(parent)` becomes
     * `extc_pool_new_at(parent, __extc_home_zone)` inside a function that receives a home zone
     * => the pool is created at **the place the caller chose**. Not one word of the library
     * changes. */
    bool poolNewAt = (planCallee(e) && planCallee(e)->body == NULL
                      && poolCtorNeedsZone(name));
    if (poolNewAt && g->funcHasZoneParam) name = "extc_pool_new_at";
    if (strcmp(name, "extc_cout_f64") == 0) g->needCoutF64 = true;
    Buf b;
    bufInit(&b, g->arena);
    bufPuts(&b, name);
    bufPutc(&b, '(');
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
    }
    /* Asking the owner **once** is enough: it hands out both home and zone. Asking twice (home
     * first, then zone) would emit both into every function that needs both. The `poolNewAt` path
     * is the **exception**: the name is already rewritten to `extc_pool_new_at`, so only its
     * second argument is appended; and it must not be written as `planMakesPool(planCallee(e))` --
     * `extc_pool_new` is an extern, the work list does not walk functions without a body => its
     * `makesPool` is false and it can only be recognized by name. */
    cgImplicitArgs(g, &b, planCallee(e), e, e->u.call.args.len, false);
    if (poolNewAt && g->funcHasZoneParam) {
        bufPuts(&b, ", ");
        bufPuts(&b, zoneArgRef(g, e));
    }
    owPassCells(g, &b, e, e->u.call.args.len, cgHasImplicitArgs(planCallee(e)));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Test whether this `EX_INDEX` site is already covered by the entry range check of an
 *        enclosing range-for.
 *
 * Looks up the table the generator keeps by node identity (`loopHoistRangeFor` pushes on it
 * before entering the loop body and truncates it on the way out). On a hit `exIndex` emits a bare
 * pointer access instead of checking the bound per element.
 *
 * @param g  The generator.
 * @param e  The `EX_INDEX` node.
 * @return   true when covered.
 */
static bool idxHoisted(CG *g, Expr *e);

/**
 * @brief Generate `EX_INDEX`: array subscript and view subscript.
 *
 * An array's length is a compile-time constant, so the emitted check carries the length
 * directly. A view goes through the checking primitive that `viewIndexer` selects; it returns a
 * pointer whose dereference is an lvalue -- reads, taking the address to pass to a `ref`
 * parameter and assignments all work, and asking for it here is also what guarantees the
 * primitive has a definition.
 *
 * Inside an `@unchecked` function both sides drop the check: an array becomes
 * `obj.data[idx]` and a view is written in the lvalue form `*((v).data + i)`.
 *
 * The `EX_INDEX` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_INDEX` node.
 * @return   The C expression; `"0"` when the base type is neither an array nor a view.
 */
static const char *exIndex(CG *g, Expr *e) {
    Type *ot = e->u.index.obj->type;
    Type *ob = ttBase(subst(g, ot));
    const char *obj = genExpr(g, e->u.index.obj);
    const char *idx = genExpr(g, e->u.index.index);

    /* Arrays: the length is a compile-time constant, so obj appears
     * exactly once and cannot be evaluated twice. */
    if (ob && ob->kind == TY_ARRAY) {
        /* With a `ref [N]T` base, `obj` is a pointer in C and must be dereferenced one level
         * first, or `p.data[..]` would be a subscript on a pointer. */
        if (ot && ot->kind == TY_REF)
            obj = arenaPrintf(g->arena, "(*%s)", obj);
        /* `@unchecked`: the length is still known here, but the programmer has signed for the
         * range, so the index is used as written. No `extc_checkedIndex` call means no trap
         * position -- that is the cost of the annotation. */
        if (g->uncheckedBody)
            return arenaPrintf(g->arena, "%s.data[%s]", obj, idx);
        return arenaPrintf(g->arena,
            "%s.data[extc_checkedIndex((int64_t)(%s), %lld, \"%s\", %d)]",
            obj, idx, (long long)ob->asize, g->path, e->line);
    }
    if (!isView(ob)) return "0";
    /* The primitive takes the view by value, so a reference is
     * dereferenced. */
    if (ot && ot->kind == TY_REF) obj = arenaPrintf(g->arena, "*(%s)", obj);

    /* `@unchecked`: the view's element is reached through its data pointer directly. The shape is
     * the checked primitive's, `&v.data[i]` written out as an lvalue (`*(p + i)`), so a read, an
     * assignment and a `ref` argument all still work, and the `file`/`line` arguments have nowhere
     * to go because there is no check. The checked primitive is *not* emitted for this body:
     * nothing in it names that one. */
    if (g->uncheckedBody)
        return arenaPrintf(g->arena, "(*((%s).data + (int64_t)(%s)))", obj, idx);

    /* This subscript is already covered by the entry range check of an enclosing `for` (see
     * `loopHoistRangeFor`): the index is necessarily in bounds, so go through the bare pointer and
     * save the per-iteration null/bounds branch. The shape is the same as an `@unchecked` view
     * subscript, so reads, assignments and taking a `ref` still work. */
    if (idxHoisted(g, e))
        return arenaPrintf(g->arena, "(*((%s).data + (int64_t)(%s)))", obj, idx);

    /* The primitive returns a pointer and the dereference is an lvalue: it can be read, addressed
     * for a `ref` parameter, and assigned to. Asking for the primitive here is also what guarantees
     * its definition exists: an index inside a generic body names the instance, and the list of
     * instances is not necessarily complete otherwise. */
    const char *ix = viewIndexer(g, ob);
    return arenaPrintf(g->arena, "(*%s(%s, (int64_t)(%s), \"%s\", %d))",
                       ix, obj, idx, g->path, e->line);
}

/**
 * @brief Generate `EX_ASSOC`: a module-qualified associated function call.
 *
 * An associated function's C name is decorated with the instance name (`option_i64_some`) under
 * the same rule as a method's, so `cMethodName` is reused directly; the implicit trailing
 * arguments and the `@overwrite` unit are appended afterwards.
 *
 * A module-qualified call goes through an assoc node, so domain construction has to recognize the
 * node marker here as well (the same as in `exCall`).
 *
 * The `EX_ASSOC` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_ASSOC` node.
 * @return   The C call expression.
 */
static const char *exAssoc(CG *g, Expr *e) {
    /* A module-qualified call goes through an assoc node => domain construction is handled here
     * too (as in `exCall`, by node marker). */
    if (planDomNew(e)) { g->needDomain = true; return "extc_dom_new()"; }
    /* An associated function decorates its C name with the instance
     * name (`option_i64_some`), which is the same decoration rule
     * methods follow, so cMethodName is reused directly. */
    Buf b;
    bufInit(&b, g->arena);
    bufPuts(&b, cMethodName(g, planAssocOwner(e), planCallee(e)));
    bufPutc(&b, '(');
    for (size_t i = 0; i < e->u.assoc.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.assoc.args, i)));
    }
    /* An associated function such as `Type::make()` allocates, so the
     * home arena argument is appended. */
    cgImplicitArgs(g, &b, planCallee(e), e, e->u.assoc.args.len, false);
    owPassCells(g, &b, e, e->u.assoc.args.len, cgHasImplicitArgs(planCallee(e)));   /* @overwrite cells */
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Generate `EX_CONV`: a checked or unchecked numeric conversion.
 *
 * Under `@unchecked`, or when the checker did not mark this conversion checked, it is a C cast.
 * Otherwise the target type's range is checked through the `static inline` primitives
 * `extc_narrowI` / `extc_narrowU` / `extc_convFloat`, whose out-of-range case traps at run time.
 * The range comes from the same table the checker uses (`ttIntBits` / `ttIntSigned`). When the
 * target is floating point the primitive's result is used as it is, without an extra cast (see
 * the function body).
 *
 * The `EX_CONV` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_CONV` node.
 * @return   The C text of the conversion expression.
 */
static const char *exConv(CG *g, Expr *e) {
    Type *t = subst(g, e->u.conv.type);          /* target type, from the checker */
    const char *x = genExpr(g, e->u.conv.operand);
    /* `@unchecked` signs the conversion check away as well: the value is cast the
     * way C casts it, and `extc_narrowI` / `extc_convFloat` are not named. */
    if (!planConvCheck(e) || g->uncheckedBody)
        return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

    /* The target is floating point (int->float, f32->f64): one C cast is the whole semantics --
     * round to nearest, and an integer is always inside the target float's range. This arm must
     * come **before** the integer range checks below: those read the operand as unsigned
     * (`(uint64_t)`), which turns a negative value into a huge one near 2^64 (guard:
     * tests/ops/conv_int_float.extc). */
    if (ttIsFloat(t))
        return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

    /* A checked conversion: look the range up in a table and go
     * through one of the `static inline` helpers. */
    const char *tn = t->name;
    bool isF = ttIsFloat(subst(g, e->u.conv.operand->type));
    if (isF) {
        /* The range comes from the same table the checker uses (`ttIntBits` / `ttIntSigned`)
         * instead of a hand-written chain per type name. The two 64-bit ends cannot be written as
         * `%lldLL` (`INT64_MIN` / `INT64_MAX` / `UINT64_MAX` are macros), so they take the macro
         * spelling. */
        int bits = ttIntBits(t);
        if (bits == 0)                          /* not an integer: the plain cast is the whole conversion */
            return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);
        if (bits == 64) {
            if (!ttIntSigned(t))        /* u64: the helper's bounds are int64_t, so it gets its own */
                return arenaPrintf(g->arena,
                    "((%s)extc_convFloatU((double)(%s), \"%s\", %d))",
                    cType(g, t), x, g->path, e->line);
            return arenaPrintf(g->arena,
                "((%s)extc_convFloat((double)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                cType(g, t), x, g->path, e->line);
        }
        int64_t lo = ttIntSigned(t) ? -((int64_t)1 << (bits - 1)) : 0;
        int64_t hi = ttIntSigned(t) ? (((int64_t)1 << (bits - 1)) - 1)
                                    : (((int64_t)1 << bits) - 1);
        return arenaPrintf(g->arena,
            "((%s)extc_convFloat((double)(%s), %lldLL, %lldLL, \"%s\", %d))",
            cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
    }
    bool sign = (tn[0] == 'i');
    if (sign) {
        int64_t lo = 0, hi = 0;
        if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
        else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
        else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
        else
            return arenaPrintf(g->arena,
                "((%s)extc_narrowI((int64_t)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                cType(g, t), x, g->path, e->line);
        return arenaPrintf(g->arena,
            "((%s)extc_narrowI((int64_t)(%s), %lldLL, %lldLL, \"%s\", %d))",
            cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
    }
    /* Target is not floating point => only the integer range check remains (unsigned targets,
     * including a sign change). A floating-point target returned the C cast above. */
    unsigned long long hi = 0;
    if (strcmp(tn,"u8")==0)  hi = 255ULL;
    else if (strcmp(tn,"u16")==0) hi = 65535ULL;
    else if (strcmp(tn,"u32")==0) hi = 4294967295ULL;
    else
        return arenaPrintf(g->arena,
            "((%s)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))",
            cType(g, t), x, g->path, e->line);
    return arenaPrintf(g->arena,
        "((%s)extc_narrowU((uint64_t)(%s), %lluULL, \"%s\", %d))",
        cType(g, t), x, hi, g->path, e->line);
}

/* ==================== Reclamation: static root tables and sweep triggers ====================
 *
 * The trigger point is an **allocation point** (`new T` / `new [N]T` / `alloc<T>(n)` /
 * `@overwrite`), where codegen statically emits two things:
 *
 *   1. A type descriptor `ExtcRcType`: the offsets at which the reference slots of this
 *      allocation point's type live. It is stored into the slot and travels with the object, and
 *      marking follows pointer fields through the **object's own** descriptor (obligation 4 of
 *      the sweep).
 *   2. A root table: the **values** of the reference slots that are live at this moment (which
 *      slots are live is something the compiler knows).
 *
 * Root tables come from **lexical scope**: declaring a storage that contains references
 * registers each of its leaf reference slots in `rcRoots` (`a[i].f` is expanded like that -- a
 * C-level array is a root too), and leaving the block truncates the registrations. So the root
 * table at any trigger point is: locals of the enclosing blocks that are still live + this
 * function's parameters + the globals.
 *
 * Two safety directions:
 *  - **Emit no trigger at all while the root table is incomplete** (`rcRootsUnreliable`):
 *    skipping a sweep is better than missing a root and freeing a live object. Storage that
 *    cannot be expanded statically, such as enum payloads, generic enums and coroutine frames,
 *    counts as incomplete.
 *  - An offset entry count above 4096 is treated as incomplete too, to keep the output from
 *    growing needlessly.
 *
 * A third safety direction (the **C hole**: the root table is complete but a calling frame still
 * holds a live reference):
 *
 *  - **Sweep only a region this frame owns** (`rcFrameOwnsArena` / `frameOwned` in
 *    `rcAllocCore`). The static root table is **lexical**: it covers this frame's locals and the
 *    globals and **structurally cannot see the live references of the other frames on the call
 *    stack**. The home arena (`(*__extc_home)`) is handed down from the caller and the
 *    thread-local arena (`extc_tls_arena`) is shared by a whole worker call chain -- when an
 *    inner frame allocates into either, an object in that region may also be pointed at by an
 *    **outer frame's** locals; one trigger from the inner frame would then use a root table built
 *    only from its own lexical scope, an **incomplete** seed, and free a whole live subtree as
 *    garbage (observed: `nroots=0`, and **every one** of the 65537 freed slots had `hold>0`).
 *    Therefore a region this frame does not own gets no trigger at all. This frame's own block
 *    arena (`&__extc_a[lvl]`, lvl >= 1) still triggers as usual: an outer frame cannot obtain a
 *    reference into it (the checker blocks cross-frame storage, and a value that escapes is
 *    placed in the **home arena** when it is allocated), so this frame's root table is complete
 *    for it -- which is also why the sweeping capability is not disabled as a whole.
 *
 *    **Taking part of that capability back (subsets A+B)**:
 *    - **A -- `main` is the home-arena exception**: `main` has no caller and its home arena is
 *      its own `&__extc_a[1]` (the `__extc_home = &__extc_a[1]` line in `genFunc`) => the root
 *      table is complete for it, and `rcFrameOwnsArena` returns true for `ARENA_HOME` in `main`.
 *    - **B -- owed flag + owner settlement**: every other function's home arena belongs to the
 *      **frame that owns it**. An inner frame does not sweep that region and only records "one
 *      collection owed" (`rcEmitOwe` => the runtime `extc_rc_owe`, whose bookkeeping carries the
 *      `shouldSweep` pressure test); the owning frame settles it with its own static root table
 *      **before its next call** (`rcEmitOwedCheck` => `extc_rc_checkOwed`).
 *
 *    **The soundness invariant of B (this is its whole justification)**: when a collection
 *    happens the collector must have no active callee subtree -- at that moment its static root
 *    table (this frame's locals + parameters + globals) is a **complete** seed. So a settlement
 *    point can only sit before the call where "the previous batch of callees has returned and
 *    the next has not been entered"; for the same reason a frame-level "roots unreliable" marker
 *    is **not** needed (that is only required by a whole runtime root stack). An owed flag that is
 *    never settled (the owner never calls again) makes that region wait for frame destruction --
 *    a looser bound, not a dangling pointer.
 *
 *    The cost, stated as it is: shapes that only allocate into the home arena and whose owner
 *    never calls again afterwards (`hole_c`, the one-shot entry of `persist_nodes`) still wait for
 *    frame destruction; a shape where the owner repeatedly calls callees in a loop
 *    (`bench/stress/stress.extc`) still peaks at 10.86-10.92 MB.
 *
 * A view (`slice<T>` / `mut slice<T>`) is not a root: its `data` points into an arena, a plate or
 * C storage, and rc slots are only produced by `new T` / `new [N]T` / `alloc<T>(n)` /
 * `@overwrite`, none of which produces a view.
 */

/* How reference slots are distributed in a type. */
typedef enum {
    RC_NO_REFS,       /* no reference slots inside */
    RC_LEAF_REFS,     /* reference slots can be enumerated statically (structs and fixed-length arrays expand
     * recursively) */
    RC_CONSERVATIVE   /* not enumerable (enum payloads, generic enums, type parameters, ...) => do not trust the root
     * table */
} RcScan;

static RcScan rcScanType(CG *g, Type *t, int depth);

/**
 * @brief Scan a struct's fields: a conservative field makes the whole struct conservative, and a
 *        field with references makes the whole struct have references.
 *
 * @param g       The generator.
 * @param fields  The `FieldDef*` vector.
 * @param depth   Recursion depth, guarding against type cycles.
 * @return        The struct's reference-slot distribution.
 */
static RcScan rcScanFields(CG *g, Vec *fields, int depth) {
    bool any = false;
    for (size_t i = 0; i < fields->len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(fields, i);
        RcScan s = rcScanType(g, fd->type, depth + 1);
        if (s == RC_CONSERVATIVE) return RC_CONSERVATIVE;
        if (s == RC_LEAF_REFS) any = true;
    }
    return any ? RC_LEAF_REFS : RC_NO_REFS;
}

/**
 * @brief Determine whether a type contains reference slots and whether it can be enumerated
 *        statically.
 *
 * A view counts as having no reference slots (see the block comment above); a generic struct
 * instance enters the instance context before its fields are scanned; an enum's or a generic
 * enum's payload is not expanded => conservative.
 *
 * @param g      The generator (supplies the instantiation context).
 * @param t      The type to scan; NULL counts as conservative.
 * @param depth  Recursion depth, guarding against cycles.
 * @return       The reference-slot distribution.
 */
static RcScan rcScanType(CG *g, Type *t, int depth) {
    if (!t || depth > 16) return RC_CONSERVATIVE;
    t = subst(g, t);
    if (!t) return RC_CONSERVATIVE;
    switch (t->kind) {
    case TY_REF:
        return RC_LEAF_REFS;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return RC_NO_REFS;
    case TY_ANY:
        /* What a dynamic value holds is not visible here, so no offset table can describe it. */
        return RC_CONSERVATIVE;
    case TY_ENUM:
        return RC_CONSERVATIVE;      /* A variant payload may carry references and cannot be expanded statically => do not trust the
     * root table */
    case TY_ARRAY:
        return rcScanType(g, t->inner, depth + 1);
    case TY_STRUCT:
        return rcScanFields(g, &t->sdef->fields, depth);
    case TY_GENERIC: {
        if (isView(t)) return RC_NO_REFS;
        if (!t->sdef) return RC_CONSERVATIVE;
        /* A pool/plate container's elements live on the container's own plate and **not in the C
         * struct's fields** (`pool<ref node>` and `vector<ref node>` element references are exactly
         * this): the descriptor fields are three views and the elements are written as bare slots
         * on the plate. When the field scan finds **not one reference leaf**, this descriptor has
         * no reference slots of its own -- the root table owes nothing here and a trigger is
         * emitted as usual; references inside elements are the responsibility of the write-site
         * barrier (`storeRefSlot` accounts by slot when it can find an rc slot and otherwise pins
         * the value as a root). Only when references are **also** visible in the fields (inline
         * arrays, structs embedded by value) does the type still count as not enumerable, going
         * down the conservative path of `rcEnumRootable`. */
        bool targRefs = false;
        for (size_t i = 0; i < t->targs.len; i++) {
            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS) {
                targRefs = true;
                break;
            }
        }
        SubCtx sv = substEnter(g, t);
        RcScan s = rcScanFields(g, &t->sdef->fields, depth);
        substLeave(g, sv);
        if (targRefs && s != RC_NO_REFS) return RC_CONSERVATIVE;
        return s;
    }
    /* kind-default: only the kinds listed above take part in "has reference slots"; any new kind
     * may hide a reference, so falling through to here counts as not enumerable (do not trust the
     * root table), which is the safe direction. */
    default:
        return RC_CONSERVATIVE;
    }
}

/**
 * @brief Collect a type's reference leaf slots as "relative offset" C constant expressions.
 *
 * The expressions are written with `offsetof` / `sizeof` (the layout is not computed here: a
 * wrong computation silently misses a mark), arrays are expanded per element and nested structs
 * add their offsets.
 *
 * @param g      The generator.
 * @param t      The type to expand.
 * @param base   The current sub-object's offset expression; the top level passes an empty string
 *               meaning 0.
 * @param out    A `const char *` vector that receives the offset expression of every reference
 *               slot.
 * @param depth  Recursion depth, guarding against cycles.
 */
static void rcCollectOffsets(CG *g, Type *t, const char *base, Vec *out, int depth) {
    if (!t || depth > 16 || out->len > 4096) return;
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(out) = (*base) ? base : "0u";
        return;
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        /* A fixed-length array is `struct { T data[N]; }` in C (see the array type's emission), so the
         * element offset starts at `offsetof(array type, data)`. */
        const char *d = (*base) ? arenaPrintf(g->arena, "(%s + offsetof(%s, data))",
                                              base, cType(g, t))
                                : arenaPrintf(g->arena, "offsetof(%s, data)", cType(g, t));
        for (int64_t i = 0; i < t->asize && out->len <= 4096; i++) {
            const char *off = arenaPrintf(g->arena, "(uint32_t)(%s + %lld * sizeof(%s))",
                                          d, (long long)i, cType(g, e));
            rcCollectOffsets(g, e, off, out, depth + 1);
        }
        return;
    }
    case TY_STRUCT: {
        const char *cn = cType(g, t);
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            const char *off = (*base)
                ? arenaPrintf(g->arena, "(%s + offsetof(%s, %s))", base, cn, fd->name)
                : arenaPrintf(g->arena, "offsetof(%s, %s)", cn, fd->name);
            rcCollectOffsets(g, fd->type, off, out, depth + 1);
        }
        return;
    }
    case TY_GENERIC:
        if (isView(t) || !t->sdef) return;
        SubCtx sv = substEnter(g, t);
        {
            const char *cn = cType(g, t);
            for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
                const char *off = (*base)
                    ? arenaPrintf(g->arena, "(%s + offsetof(%s, %s))", base, cn, fd->name)
                    : arenaPrintf(g->arena, "offsetof(%s, %s)", cn, fd->name);
                rcCollectOffsets(g, fd->type, off, out, depth + 1);
            }
        }
        substLeave(g, sv);
        return;
    /* kind-default: offsets only recurse through the REF / ARRAY / STRUCT / GENERIC shapes; the
     * other kinds have no reference slots to collect. A new kind that contains reference slots
     * must add a case here and must not slide through the default. */
    default:
        return;
    }
}

/**
 * @brief Emit the type descriptor for one allocation and return a C expression pointing at it.
 *
 * No reference slots => `(const ExtcRcType *)0` (marking stops at this object); not enumerable =>
 * `EXTC_RC_CONSERVATIVE` (marking scans every word of the slot).
 *
 * The descriptor is a **file-scope static const** written into `g->rcDesc` and spliced into the
 * unit before the function bodies like any other on-demand definition (the address has static
 * storage duration, so it fits in the slot). Deduplicated by the offset-table text: one type is
 * emitted once even when it appears at many allocation points -- one per point would pile
 * isomorphic descriptors into the same slab's type table and make the allocation path's `typeIdx`
 * scan them linearly (about 40 Ir per allocation across the eight node-allocation points of
 * `shard_bands`, a third of that shape's cost).
 *
 * @param g  The generator; returns an empty descriptor while `needReclaim` is off.
 * @param t  The allocated type.
 * @return   A C expression pointing at an `ExtcRcType`.
 */
static const char *rcEmitTypeDesc(CG *g, Type *t) {
    if (!g->needReclaim || g->prefixBlk == 0) return "(const ExtcRcType *)0";
    RcScan sc = rcScanType(g, t, 0);
    if (sc == RC_NO_REFS) return "(const ExtcRcType *)0";
    if (sc == RC_CONSERVATIVE) return "((const ExtcRcType *)-1)";
    Vec offs;
    vecInit(&offs, g->arena, sizeof(const char *));
    rcCollectOffsets(g, t, "", &offs, 0);
    if (offs.len == 0) return "(const ExtcRcType *)0";
    if (offs.len > 4096) return "((const ExtcRcType *)-1)";
    Buf b;
    bufInit(&b, g->arena);
    for (size_t i = 0; i < offs.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, "%s", *(const char **)vecAt(&offs, i));
    }
    /* Dedup: the key is the offset-table text (it decides both the pointer count and every offset,
     * so equal keys are isomorphic). A descriptor is pure data, sharing one has no semantic
     * difference, and the per-point cost sits on the allocation path (see the function comment
     * above). */
    const char *key = bufCstr(&b);
    int id = -1;
    for (size_t i = 0; i < g->rcDescKeys.len; i++) {
        if (strcmp(*(const char **)vecAt(&g->rcDescKeys, i), key) == 0) { id = (int)i; break; }
    }
    if (id < 0) {
        id = (int)g->rcDescKeys.len;
        *(const char **)vecPush(&g->rcDescKeys) = key;
        bufPrintf(&g->rcDesc,
                  "__attribute__((unused)) static const uint32_t __extc_rco%d[] = { %s };\n"
                  "__attribute__((unused)) static const ExtcRcType __extc_rct%d = { %zuu, __extc_rco%d };\n",
                  id, key, id, offs.len, id);
    }
    return arenaPrintf(g->arena, "&__extc_rct%d", id);
}

/**
 * @brief Whether the region this allocation goes into is **owned by this frame** (its static
 *        root table is a complete seed for it).
 *
 * The test (see "third safety direction" above): this frame's own block arena
 * (`&__extc_a[lvl]`, `lvl >= 1`) counts; the home arena (`ARENA_HOME`: the `(*__extc_home)`
 * handed down from the caller) generally does not; the thread-local arena (`parTls`) does not
 * either -- it is likewise shared by a whole worker call chain, for the same reason as the home
 * arena. A coroutine step's block arena lives in the frame (`f->arena<lvl>`) and belongs to the
 * frame itself: `genFunc` registers the fields listed in `coroFrame` as roots, and the checker
 * forbids yielding a value that carries references (its `yield` rule for coroutines) => the
 * caller cannot hold a reference into that region, so the root table is complete for it.
 *
 * **`main` is the home-arena exception (subset A)**: `main` has no caller and its home arena is
 * its own `&__extc_a[1]` (`__extc_home = &__extc_a[1]` in `genFunc`) => the static root table is
 * complete for it, so it counts. Every other function's home arena belongs to **the frame that
 * owns it**: an inner frame does not sweep it and only records an owed collection (`rcEmitOwe`),
 * and the owner settles it with its own static root table while it has no active callee subtree
 * (`rcEmitOwedCheck`).
 *
 * @param g    The generator.
 * @param lvl  `ARENA_HOME` (-1) or a block level of 1 and above.
 * @return     true = owned by this frame, so a sweep may be triggered on this region.
 */
static bool rcFrameOwnsArena(CG *g, int lvl) {
    if (g->parTls) return false;
    if (lvl == ARENA_HOME) return g->inMain;
    return true;
}

/**
 * @brief Which trigger route an allocation point takes (three states, see `RcOwn`).
 *
 * The static test only reaches "owns / does not own"; ownership of the thread-local arena is only
 * known at **run time**: a whole worker call chain shares `extc_tls_arena`, the entry frame (the
 * one where `extc_rc_tlsDepth++ == 0`) has only the C runtime as its caller and its static root
 * table is a complete seed => it owns; an inner frame's root table cannot see the calling frame's
 * live references => it only records what is owed. A `noArena` frame emits no prologue (see
 * below), so the TLS state must be excluded -- otherwise a trigger point would reference an
 * `__extc_tlsOwner` that does not exist. A coroutine step does not take part in the TLS state
 * (same reason), but its block arena belongs to the frame itself (see `rcFrameOwnsArena`).
 *
 * @param g    The generator.
 * @param lvl  `ARENA_HOME` (-1) or a block level of 1 and above.
 * @return     `RC_OWN_NO` / `RC_OWN_YES` / `RC_OWN_TLS`.
 */
static RcOwn rcOwnKind(CG *g, int lvl) {
    /* The prologue is only emitted in a frame with `parTls && !noArena && !coroFunc` (see
     * `genFunc`) => the same test here, or a trigger point would reference an `__extc_tlsOwner`
     * that does not exist in this frame. */
    if (g->parTls) return (g->noArena || g->coroFunc) ? RC_OWN_NO : RC_OWN_TLS;
    return rcFrameOwnsArena(g, lvl) ? RC_OWN_YES : RC_OWN_NO;
}

/** @brief Record one rc site (diagnostic `EXTC_DBG_RCCOST=1`; statistics only, never changes
 *         what is emitted).
 *
 * @param g     The generator.
 * @param lvl   Block level (= region); a negative value is merged into 0.
 * @param kind  0 = reference store, 1 = allocation.
 */
static void dbgRcSite(CG *g, int lvl, int kind) {
    if (!g->dbgCost) return;
    int i = lvl < 0 ? 0 : (lvl > 71 ? 71 : lvl);
    if (kind == 0) { g->dbgRc[i].nRef++;   if (g->loopLen > 0) g->dbgRc[i].nRefLoop++; }
    else           { g->dbgRc[i].nAlloc++; if (g->loopLen > 0) g->dbgRc[i].nAllocLoop++; }
}

/** @brief Print the per-level rc site table to stderr (diagnostic `EXTC_DBG_RCCOST=1`; read
 *         only, never changes what is emitted).
 *
 * Purpose: statically price whether a region should be counted or swept. `nRefLoop/nAllocLoop`
 * are the per-iteration densities under the same loop nesting (execution counts cancel out), and
 * `rootSlotsMax` is the largest root-table slot count at a trigger point = the marking start-up
 * size.
 *
 * @param g  The generator.
 */
static void dbgRcCostReport(CG *g) {
    if (!g->dbgCost) return;
    fprintf(stderr, "[rccost] unit needReclaim=%d noCount=%d sweepFree=%d shards=%d\n",
            g->needReclaim, g->rcNoCount, g->rcSweepFree, g->rcNShard > 0 ? g->rcNShard : 1);
    fprintf(stderr, "[rccost] %-4s %8s %10s %8s %12s %9s\n",
            "lvl", "ref", "refInLoop", "alloc", "allocInLoop", "rootMax");
    int64_t tr = 0, trl = 0, ta = 0, tal = 0, trm = 0;
    for (int i = 0; i < 72; i++) {
        if (!g->dbgRc[i].nRef && !g->dbgRc[i].nAlloc && !g->dbgRc[i].rootSlotsMax) continue;
        fprintf(stderr, "[rccost] %-4d %8lld %10lld %8lld %12lld %9lld\n", i,
                (long long)g->dbgRc[i].nRef, (long long)g->dbgRc[i].nRefLoop,
                (long long)g->dbgRc[i].nAlloc, (long long)g->dbgRc[i].nAllocLoop,
                (long long)g->dbgRc[i].rootSlotsMax);
        tr += g->dbgRc[i].nRef;   trl += g->dbgRc[i].nRefLoop;
        ta += g->dbgRc[i].nAlloc; tal += g->dbgRc[i].nAllocLoop;
        if (g->dbgRc[i].rootSlotsMax > trm) trm = g->dbgRc[i].rootSlotsMax;
    }
    fprintf(stderr, "[rccost] TOTAL ref=%lld refInLoop=%lld alloc=%lld allocInLoop=%lld rootMax=%lld\n",
            (long long)tr, (long long)trl, (long long)ta, (long long)tal, (long long)trm);
}

static size_t rcRootTableTextSkip(CG *g, Buf *b, size_t skipLocal);

/**
 * @brief Assemble the current static root table (global reference slots + this frame's local and
 *        parameter reference slots) into one compound literal.
 *
 * The table is just the **reference-slot expressions live at this moment**, materialized on the
 * spot by value: `(void *[]){ ... }`. With `n == 0` it is a null pointer constant -- the runtime's
 * seeding loop only looks at `nroots` and does not need an empty table.
 *
 * @param g  The generator.
 * @param b  The buffer the text is appended to.
 * @return   The root count (the number of slots in the table).
 */
static size_t rcRootTableText(CG *g, Buf *b) {
    return rcRootTableTextSkip(g, b, (size_t)-1);
}

/**
 * @brief Like `rcRootTableText`, but **skips slot `skipLocal` of this frame's root table**.
 *
 * The point release of an overwrite point wants the **other** roots: the skipped slot still holds
 * the old value at that moment, so counting it in the seed would prove the old structure live and
 * release nothing. Excluding that slot is what "overwriting it = judging it dead" means.
 *
 * @param g          The generator.
 * @param b          The buffer the text is appended to.
 * @param skipLocal  The slot of `g->rcRoots` to skip; `(size_t)-1` skips none.
 * @return           The number of slots actually written into the table.
 */
static size_t rcRootTableTextSkip(CG *g, Buf *b, size_t skipLocal) {
    size_t n = g->rcGlobalRoots.len + g->rcRoots.len;
    if (g->dbgCost) {
        int i = g->blkLevel < 0 ? 0 : (g->blkLevel > 71 ? 71 : g->blkLevel);
        if ((int64_t)n > g->dbgRc[i].rootSlotsMax) g->dbgRc[i].rootSlotsMax = (int64_t)n;
    }
    if (n == 0) {
        bufPuts(b, "(void *const *)0");
        return 0;
    }    bufPuts(b, "(void *[]){ ");
    size_t k = 0;
    for (size_t i = 0; i < g->rcGlobalRoots.len; i++)
        bufPrintf(b, "%s(void *)(%s)", k++ ? ", " : "",
                  *(const char **)vecAt(&g->rcGlobalRoots, i));
    for (size_t i = 0; i < g->rcRoots.len; i++) {
        if (i == skipLocal) continue;
        bufPrintf(b, "%s(void *)(%s)", k++ ? ", " : "",
                  *(const char **)vecAt(&g->rcRoots, i));
    }
    bufPuts(b, " }");
    return k;
}

/**
 * @brief The bookkeeping for when this frame **does not own** the region: record "one collection
 *        owed" on it (subset B).
 *
 * The reason (see `rcFrameOwnsArena` and "third safety direction" in the block comment above):
 * the home arena is the same one handed down from the caller, and this frame's static root table
 * cannot see the live references of the other frames on the call stack => sweeping such a region
 * as usual frees live objects as garbage (the C hole). So an inner frame does not sweep and only
 * records the debt; **the frame that owns the region** settles it with its own static root table
 * while it has no active callee subtree (`rcEmitOwedCheck`).
 *
 * The bookkeeping itself carries the pressure test (`extc_rc_shouldSweep`): an allocation below
 * the pressure line does not even record the debt, so the owner's check is "read one bit" and
 * does not materialize a root table at every call site.
 *
 * Ownership of the thread-local arena (`parTls`) is only known at run time: a whole worker call
 * chain shares `extc_tls_arena`, the worker entry frame (the one that reads 0 from
 * `extc_rc_tlsDepth`) owns it and an inner frame records an owed collection (`RC_OWN_TLS`). This
 * function is therefore the body of the bookkeeping **without a parTls guard**; the non-TLS entry
 * `rcEmitOwe` is the one that guards.
 *
 * @param g        The generator.
 * @param basePtr  The region's base address expression (in `(void *)` form).
 * @param lvl      The block level (`ARENA_HOME` or 1 and above); with sharding the region number
 *                 is `lvl * shard count + k`.
 */
static void rcEmitOweNow(CG *g, const char *basePtr, int lvl) {
    if (g->rcNShard <= 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (extc_rc_shouldSweep(%s)) extc_rc_owe(%s); }", rr, rr);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *__extc_rcw = extc_rc_regionOf(%s, %d);"
                  " if (extc_rc_shouldSweep(__extc_rcw)) extc_rc_owe(__extc_rcw); }",
               basePtr, lvl);
        return;
    }
    /* Sharding: the region key is "base address + k". This allocation can only land in one of the
     * shards, but every shard is asked here (marking several is harmless, the owner settles per
     * shard) -- no new sharding state is introduced. */
    pfLine(g, "{ for (int32_t __extc_rcwk = 0; __extc_rcwk < %d; __extc_rcwk++) {"
              " ExtcRcRegion *__extc_rcw = extc_rc_regionOf((void *)((char *)(%s) + __extc_rcwk),"
              " (int32_t)(%d + __extc_rcwk));"
              " if (extc_rc_shouldSweep(__extc_rcw)) extc_rc_owe(__extc_rcw); } }",
           g->rcNShard, basePtr, lvl * g->rcNShard);
}

/** @brief Entry point for the bookkeeping when the region is not owned; the TLS state does not
 *         take this path (see the three states of `rcEmitTrigger`). */
static void rcEmitOwe(CG *g, const char *basePtr, int lvl) {
    if (g->parTls) return;   /* TLS ownership is left to the run-time branch of `RC_OWN_TLS` */
    rcEmitOweNow(g, basePtr, lvl);
}

/**
 * @brief The owner frame's settlement point: collect the sweep owed on the outer region with its
 *        **own static root table at this moment** (subset B).
 *
 * The soundness invariant (the whole justification of B): **when a collection happens the
 * collector must have no active callee subtree** -- at that moment its static root table (this
 * frame's locals + parameters + globals) is a complete seed. The check is therefore only emitted
 * **before a call** where this frame owns the arena and really passes it out as the callee's home
 * arena: the previous batch of callees has returned, this batch has not been entered and no deep
 * frame is around. => A frame-level "roots unreliable" marker is **not** needed (a whole runtime
 * root stack would need one).
 *
 * At the vast majority of call sites where the owed flag is false this costs one `regionOf`
 * (a table hit) plus one bit read; the root table is an argument of `extc_rc_checkOwed` and is
 * only materialized when `extc_rc_owed` is true (the same rule as the entry guard of a trigger
 * point).
 *
 * @param g        The generator.
 * @param basePtr  The base address expression of the region this frame owns (in `(void *)`
 *                 form).
 * @param lvl      The block level (`main`'s home arena is `ARENA_HOME`, everything else is 1 and
 *                 above).
 * @param own      The trigger route (with `RC_OWN_TLS` a run-time ownership test
 *                 `if (__extc_tlsOwner && ...)` wraps the whole settlement point: an inner
 *                 frame's root table is incomplete and cannot settle).
 */
static void rcEmitOwedCheck(CG *g, const char *basePtr, int lvl, RcOwn own) {
    if (!g->needReclaim || g->rcRootsUnreliable || g->prefixBlk == 0) return;
    if (g->rcGlobalRoots.len + g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; }
    Buf b;
    bufInit(&b, g->arena);
    size_t n = rcRootTableText(g, &b);
    const char *ownCond = own == RC_OWN_TLS ? "__extc_tlsOwner && " : "";
    if (g->rcNShard <= 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (%sextc_rc_owed(%s)) extc_rc_checkOwed(%s, %s, %zu); }",
                   ownCond, rr, rr, bufCstr(&b), n);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *__extc_rcc = extc_rc_regionOf(%s, %d);"
                  " if (%sextc_rc_owed(__extc_rcc)) extc_rc_checkOwed(__extc_rcc, %s, %zu); }",
               basePtr, lvl, ownCond, bufCstr(&b), n);
        return;
    }
    pfLine(g, "{ for (int32_t __extc_rcck = 0; __extc_rcck < %d; __extc_rcck++) {"
              " ExtcRcRegion *__extc_rcc = extc_rc_regionOf((void *)((char *)(%s) + __extc_rcck),"
              " (int32_t)(%d + __extc_rcck));"
              " if (%sextc_rc_owed(__extc_rcc)) extc_rc_checkOwed(__extc_rcc, %s, %zu); } }",
           g->rcNShard, basePtr, lvl * g->rcNShard, ownCond, bufCstr(&b), n);
}

/**
 * @brief Emit a sweep trigger at an allocation point: `extc_rc_maybeCollect(region, roots, nroots)`.
 *
 * The pressure test and the self-learning exit live at run time (reclaim.c); this function only
 * **calls with the right arguments at the right moment**: the region is the one this allocation
 * lands in (the same claim key as `extc_rc_alloc`) and the root table is the reference slots live
 * at this moment. The trigger goes into the **statement prefix**, so it lands before the
 * statement containing the allocation -- the new object does not exist yet and needs no root.
 *
 * **Entry guard**: the root table is a call argument, and C semantics require materializing the
 * whole table (about 1412 entries in this shape) before the call; but the first statement of
 * `maybeCollect` is `if (!extc_rc_shouldSweep(r)) return -1;` -- the vast majority of allocation
 * points never reach the pressure line. So the test is lifted in front of the call site: the root
 * table is materialized and the call made **only when a sweep is due**. This changes when the
 * root table is materialized, not when collection happens: there is one copy of the test
 * (`extc_rc_shouldSweep`, the very function `maybeCollect` uses inside) and none is duplicated.
 *
 * **Only the "this frame owns it" side is taken here**: when that is false the path becomes the
 * owed bookkeeping (`rcEmitOwe`) -- the root table cannot see the calling frames' live
 * references, and sweeping such a region as usual frees live objects as garbage. The three-state
 * dispatch is in `rcEmitTrigger`.
 *
 * @param g          The generator.
 * @param basePtr    The **base address expression** of the region this allocation lands in
 *                   (already in `(void *)` form), such as `(void *)&(__extc_a[1])` or
 *                   `(void *)__owc0->home`; the claim key of shard k is
 *                   `(char *)basePtr + k` (compared only, never read -- see `rcAllocCore`).
 * @param lvl        The block level (1 = function body, -1 = home region); with sharding the
 *                   region number is `lvl * shard count + k`.
 */
static void rcEmitTriggerNow(CG *g, const char *basePtr, int lvl) {
    size_t n = g->rcGlobalRoots.len + g->rcRoots.len;
    if (n > 4096) { g->rcRootsUnreliable = true; return; }
    Buf b;
    bufInit(&b, g->arena);
    n = rcRootTableText(g, &b);
    const char *rg = arenaPrintf(g->arena, "__extc_rcg%d", g->rcGuardSeq++);
    if (g->rcNShard <= 1) {
        /* Single shard: byte-identical to the non-sharded wiring (the claim key is the region base
     * address). */
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (extc_rc_shouldSweep(%s)) extc_rc_maybeCollect(%s, %s, %zu); }",
                   rr, rr, bufCstr(&b), n);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *%s = extc_rc_regionOf(%s, %d);"
                  " if (extc_rc_shouldSweep(%s)) extc_rc_maybeCollect(%s, %s, %zu); }",
               rg, basePtr, lvl, rg, rg, bufCstr(&b), n);
        return;
    }
    /* Sharding: the region key is "base address + k" (compared only, never read). One trigger
     * collects **one shard**, the one with the **most garbage** (`bytes - live`, the same approach
     * as the prototype): once it is collected its `bytes` drops, so the next trigger picks another
     * shard -- no extra rotation state is needed and no shard can starve (always taking the first
     * would let shard 0 monopolize).
     *
     * The memory pressure floor is **divided per shard** (`EXTC_RC_FLOOR / N`, the same rule as the
     * prototype's `TRIGGER / NSHARD`): without the division N shards each guard the full floor and
     * the total bound becomes N times larger, which would let the pause knob change the memory
     * bound as a side effect; divided, the total stays `EXTC_RC_FLOOR` and more shards only mean
     * more frequent, smaller collections. The floor is set at run time by
     * `extc_rc_regionFloor` (the test stays at run time; only the same floor is divided by the
     * shard count here). The root table is still a call argument and still only materialized when
     * a sweep really happens -- the same rule as the single-shard entry guard. */
    const char *ck = "__extc_rck";
    const char *rb = arenaPrintf(g->arena, "__extc_rcb%d", g->rcGuardSeq);
    const char *rbg = arenaPrintf(g->arena, "__extc_rcbg%d", g->rcGuardSeq);
    /* Garbage bytes of this round's shard: it **must be kept separate** from "the best so far", or
     * `gd > rbg` degenerates into comparing a value with itself and `rbg = gd` into self-assignment
     * => one trigger always collects the first shard above the line, the later shards starve and
     * each grows without bound. */
    const char *gd = arenaPrintf(g->arena, "__extc_rcgd%d", g->rcGuardSeq);
    pfLine(g, "{ ExtcRcRegion *%s = (ExtcRcRegion *)0; int64_t %s = 0;"
              " for (int32_t %s = 0; %s < %d; %s++) {"
              " ExtcRcRegion *%s = extc_rc_regionOf((void *)((char *)(%s) + %s), (int32_t)(%d + %s));"
              " extc_rc_regionFloor(%s, (EXTC_RC_FLOOR) / %d);"
              " if (!extc_rc_shouldSweep(%s)) continue;"
              " int64_t %s = extc_rc_garbageHint(%s);"
              " if (!%s || %s > %s) { %s = %s; %s = %s; } }"
              " if (%s) extc_rc_maybeCollect(%s, %s, %zu); }",
           rb, rbg, ck, ck, g->rcNShard, ck, rg, basePtr, ck, lvl * g->rcNShard, ck,
           rg, g->rcNShard, rg,
           gd, rg, rb, gd, rbg, rb, rg, rbg, gd,
           rb, rb, bufCstr(&b), n);
}

/**
 * @brief Three-state dispatch of a trigger point (see `RcOwn`): owns => sweep now; does not own
 *        => only record the owed collection; TLS => choose at run time between "this is the
 *        worker entry frame" and not.
 *
 * In the TLS state the root table is only materialized in the owner branch (the compound literal
 * is an argument of `rcEmitTriggerNow`, which sits entirely inside `if (__extc_tlsOwner)`) => an
 * inner frame does not pay for a second copy of the root table.
 *
 * @param g        The generator.
 * @param basePtr  The region's base address expression (as in `rcEmitTriggerNow`).
 * @param lvl      The block level (as in `rcEmitTriggerNow`).
 * @param own      The trigger route.
 */
static void rcEmitTrigger(CG *g, const char *basePtr, int lvl, RcOwn own) {
    if (!g->needReclaim || g->rcRootsUnreliable || g->prefixBlk == 0) return;
    if (own == RC_OWN_YES) { rcEmitTriggerNow(g, basePtr, lvl); return; }
    if (own == RC_OWN_NO) {
        /* The other half of ownership delegation (`homeOwnArg`): this frame does not own the home arena,
         * but the caller's flag may say "the owner holds no live reference slot right now, your own
         * root table is a complete seed". The root table is only materialized in the owner branch --
         * the compound literal is an argument of `rcEmitTriggerNow` and stays inside the `if`. */
        if (lvl == ARENA_HOME && g->rcFuncUsesHome && !g->parTls && !g->coroFunc) {
            pfLine(g, "if (__extc_homeOwn) {");
            g->indent++;
            rcEmitTriggerNow(g, basePtr, lvl);
            g->indent--;
            pfLine(g, "} else {");
            g->indent++;
            rcEmitOwe(g, basePtr, lvl);
            g->indent--;
            pfLine(g, "}");
            return;
        }
        rcEmitOwe(g, basePtr, lvl);
        return;
    }
    /* RC_OWN_TLS: the whole worker call chain shares one region, so ownership is a run-time
     * matter. */
    pfLine(g, "if (__extc_tlsOwner) {");
    g->indent++;
    rcEmitTriggerNow(g, basePtr, lvl);
    g->indent--;
    pfLine(g, "} else {");
    g->indent++;
    rcEmitOweNow(g, basePtr, lvl);
    g->indent--;
    pfLine(g, "}");
}

/**
 * @brief Emit one rc allocation: the shard index rotates per **allocation point**, and the text
 *        of the `extc_rc_alloc(...)` call is returned.
 *
 * Shard index = `rcSite++ % rcNShard`: the same `new` always lands in the same shard, so chains
 * and cycles inside a shard stay complete; references between shards are handled by the existing
 * cross-region registration (`rsInc`/`rsDec` of `storeRefOwned`), whose bit sits on the `rsbit`
 * of the **target's** shard -- seeding and sweeping never cross a shard.
 *
 * Trigger and allocation use the **same** shard key: the pressure is asked once with this shard's
 * key (the trigger lands in the statement prefix) and the real allocation then takes its slot
 * from the same shard. With N == 1 the returned text is byte-identical to the non-sharded
 * wiring.
 *
 * @param g          The generator.
 * @param basePtr    The region's base address expression (as in `rcEmitTrigger`).
 * @param lvl        The block level (may be -1; only used for the region number).
 * @param own        The trigger route (as in `rcEmitTrigger`; `RC_OWN_NO` => no sweep, only the
 *                   owed bookkeeping).
 * @param bytesExpr  The C text of the byte count (spliced into the call as it is, with no extra
 *                   parentheses).
 * @param td         The C text of the type descriptor.
 * @return           The C text of the `extc_rc_alloc(...)` call.
 */
static const char *rcAllocCore(CG *g, const char *basePtr, int lvl, RcOwn own,
                               const char *bytesExpr, const char *td) {
    dbgRcSite(g, lvl, 1);
    int nsh = g->rcNShard > 0 ? g->rcNShard : 1;
    int k = g->rcSite++ % nsh;
    rcEmitTrigger(g, basePtr, lvl, own);
    if (nsh == 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) return arenaPrintf(g->arena, "extc_rc_alloc(%s, %s, %s)", rr, bytesExpr, td);
        return arenaPrintf(g->arena,
            "extc_rc_alloc(extc_rc_regionOf(%s, %d), %s, %s)",
            basePtr, lvl, bytesExpr, td);
    }
    return arenaPrintf(g->arena,
        "extc_rc_alloc(extc_rc_regionOf((void *)((char *)(%s) + %d), %d), %s, %s)",
        basePtr, k, lvl * nsh + k, bytesExpr, td);
}

/**
 * @brief Register every reference leaf slot of one storage as a root.
 *
 * Arrays are expanded per element and nested structs are followed down (`a[i].f`). A view is not
 * registered (its `data` is not an rc slot). Being slow or deep is not a concern: this runs once,
 * at the **declaration**, and only while the switch is on.
 *
 * @param g      The generator; registrations are appended to `g->rcRoots`.
 * @param t      The storage's type.
 * @param base   The storage's lvalue expression in C (such as `saved` or `h.c`).
 * @param depth  Recursion depth, guarding against cycles.
 */
static void rcRootLeaves(CG *g, Type *t, const char *base, int depth) {
    if (!t || depth > 16) return;
    if (g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; }
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(&g->rcRoots) = base;
        return;
    case TY_ENUM: {
        /* An enum payload: the reference leaves of **all** variants are registered (no pick by run-time
         * tag). Reading `u.<v>._<k>` of an inactive variant yields garbage, and a root-table entry
         * goes through `extc_rc_owns` in `markBegin` => a wrong guess retains more, never less. So a
         * local whose leaf slots cannot be enumerated (`var m: maybe = ...`) can be a root too, and
         * the storage side does not have to take the target region out of sweeping (see
         * `extc_rc_storeStructRooted`). */
        if (!t->edef) return;
        for (size_t i = 0; i < t->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&t->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; }
                /* A generic enum instance must substitute its arguments into the payload types (the same
     * technique as the binding in `stMatch`). */
                if (t->edef->typeParams.len > 0 &&
                    t->targs.len == t->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &t->edef->typeParams, &t->targs);
                rcRootLeaves(g, pt,
                             arenaPrintf(g->arena, "%s.u.%s._%zu", base, v->name, k),
                             depth + 1);
            }
        }
        return;
    }
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        for (int64_t i = 0; i < t->asize; i++) {
            if (g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; }
            /* An array is a wrapping C struct: its elements are written as `arr.data[i]`. */
            rcRootLeaves(g, e, arenaPrintf(g->arena, "%s.data[%lld]", base, (long long)i), depth + 1);
        }
        return;
    }
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcRootLeaves(g, fd->type,
                         arenaPrintf(g->arena, "%s.%s", base, fd->name), depth + 1);
        }
        return;
    case TY_GENERIC:
        /* A view's `data` word points at the block that holds its elements. For `new T[n]` on the
         * reclaiming path that block is an rc slot, and this root is what keeps it alive; a pointer
         * into an arena, a plate or the stack fails `extc_rc_owns` in the marker, so a view whose
         * storage is not rc contributes nothing. */
        if (isView(t)) {
            *(const char **)vecPush(&g->rcRoots) = arenaPrintf(g->arena, "%s.data", base);
            return;
        }
        if (!t->sdef) return;
        SubCtx sv = substEnter(g, t);
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcRootLeaves(g, fd->type,
                         arenaPrintf(g->arena, "%s.%s", base, fd->name), depth + 1);
        }
        substLeave(g, sv);
        return;
    /* kind-default: roots only recurse through the REF / ARRAY / STRUCT / GENERIC shapes; the other
     * kinds have no reference slots to register. A new kind that contains reference slots must add
     * a case here and must not slide through the default. */
    default:
        return;
    }
}

/**
 * @brief Collect the **C lvalue paths** of a type's reference leaf slots (projected along
 *        values: fields / inline array elements).
 *
 * Same recursive shape as `rcRootLeaves`; only the exit differs: there the leaf is registered as
 * a **root**, here the leaf's path is handed to the caller -- **whole-value storage**
 * (`s.w = wrap { n: keeper }`, `var b: wrap = ...`) has to run the reference-slot bookkeeping
 * (`initRef` / `storeRefOwned` / `storeRefSlot`) leaf by leaf. A bare block copy neither `inc`s
 * nor registers a cross-region edge: the slot is written with no bookkeeping, so the object is
 * freed when its count reaches zero or the region is swept while the slot still points at it.
 *
 * `base` must be an lvalue that is **safe to evaluate repeatedly** (the caller first evaluates
 * target and value once into a temporary / `(*p)`): the path splices it into every bookkeeping
 * call.
 *
 * @param g      The generator.
 * @param t      The type.
 * @param base   The value's lvalue expression in C (such as `(*__extc_svd0)` or `b`); an empty
 *               string produces **relative paths** (`.f`, `.kids.data[0]`) and lets the caller
 *               splice both bases itself -- one leaf set can then be used for target and value at
 *               once, without collecting either twice or having a leaf on one side only.
 * @param out    A `const char *` vector that receives the path of every reference leaf.
 * @param depth  Recursion depth, guarding against type cycles.
 */
static void rcLeafPaths(CG *g, Type *t, const char *base, Vec *out, int depth) {
    if (!t || depth > 16 || out->len > 4096) return;
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(out) = base;
        return;
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        /* A fixed-length array is a wrapping struct in C: its elements are written as `arr.data[i]`
     * (the same as `rcRootLeaves`). */
        for (int64_t i = 0; i < t->asize && out->len <= 4096; i++)
            rcLeafPaths(g, e, arenaPrintf(g->arena, "%s.data[%lld]", base, (long long)i),
                        out, depth + 1);
        return;
    }
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcLeafPaths(g, fd->type, arenaPrintf(g->arena, "%s.%s", base, fd->name),
                        out, depth + 1);
        }
        return;
    case TY_GENERIC:
        if (isView(t) || !t->sdef) return;
        SubCtx sv = substEnter(g, t);
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcLeafPaths(g, fd->type, arenaPrintf(g->arena, "%s.%s", base, fd->name),
                        out, depth + 1);
        }
        substLeave(g, sv);
        return;
    /* kind-default: only the kinds listed above take part in expanding reference leaves; the other
     * kinds are classified by `rcScanType` as "no references" or "not enumerable" and never reach
     * here. */
    default:
        return;
    }
}

/**
 * @brief The class of conservative types whose leaves are in fact enumerable: their references
 *        are hidden only in **enum payloads**.
 *
 * `rcScanType` returns `RC_CONSERVATIVE` for every TY_ENUM because it would have to pick one
 * branch by run-time tag; but "do not trust the root table" is not the only way out -- registering
 * the payload leaves of **all** variants as roots, with garbage from inactive variants filtered by
 * `extc_rc_owns`, errs on the retaining side (the same as the TY_ENUM branch of `rcRootLeaves`).
 * A local can then be a root, trigger points are emitted as usual, and the storage side does not
 * has to take the target region out of sweeping -- that is what closes the gap left by
 * "a `match` binding a reference payload".
 *
 * Only conservative types whose **shape really is enumerable** count: TY_ENUM alone is
 * "reading an inactive variant is harmless". A generic instance whose arguments carry references
 * (elements live on the container's own plate and the field scan cannot see them) and any kind
 * added later return false and still do not trust the root table.
 *
 * @param g        The generator.
 * @param t        The type to check.
 * @param depth    Recursion depth, guarding against cycles.
 * @param hasRefs  Output: whether this type has reference leaves at all. When every payload is an
 *                 enum without references (`var x: option<i64>`) there is no root to register and
 *                 returning true would be pointless -- the caller uses this output to leave that
 *                 class on the conservative path (no useless roots).
 * @return         true = every reference leaf can be statically expanded into a root, variant by
 *                 variant.
 */
static bool rcEnumRootable(CG *g, Type *t, int depth, bool *hasRefs) {
    if (!t || depth > 16) return false;
    t = subst(g, t);
    if (!t) return false;
    switch (t->kind) {
    case TY_REF:
        *hasRefs = true;
        return true;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return true;
    case TY_ENUM:
        if (!t->edef) return false;
        for (size_t i = 0; i < t->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&t->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (t->edef->typeParams.len > 0 &&
                    t->targs.len == t->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &t->edef->typeParams, &t->targs);
                if (!rcEnumRootable(g, pt, depth + 1, hasRefs)) return false;
            }
        }
        return true;
    case TY_ARRAY:
        return rcEnumRootable(g, t->inner, depth + 1, hasRefs);
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            if (!rcEnumRootable(g, fd->type, depth + 1, hasRefs)) return false;
        }
        return true;
    case TY_GENERIC: {
        /* A view is a root word in its own right: its `data` pointer may be the only edge to a
         * `new T[n]` block, so the storage that holds the view must be registered even though the
         * view has no reference slots (`rcScanType` answers `RC_NO_REFS` for it). */
        if (isView(t)) {
            *hasRefs = true;
            return true;
        }
        if (!t->sdef) return false;
        for (size_t i = 0; i < t->targs.len; i++) {
            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS)
                return false;
        }
        SubCtx sv = substEnter(g, t);
        bool ok = true;
        for (size_t i = 0; i < t->sdef->fields.len && ok; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            ok = rcEnumRootable(g, fd->type, depth + 1, hasRefs);
        }
        substLeave(g, sv);
        return ok;
    }
    /* kind-default: aligned with the default branch of `rcScanType` -- anything unrecognized counts
     * as not enumerable. */
    default:
        return false;
    }
}

/**
 * @brief Register a storage's reference roots when it is declared; when they cannot be
 *        enumerated, turn off every trigger in this function.
 *
 * @param g     The generator.
 * @param t     The declared type.
 * @param base  The storage's lvalue expression in C.
 */
static void rcRootDeclImpl(CG *g, Type *t, const char *base, bool isParam) {
    if (!g->needReclaim || !t) return;
    /* A view slot is a root in its own right: its `data` word may point at an rc block taken by
     * `new T[n]`, and `rcScanType` reports a view as having no reference slots (the elements live
     * behind the pointer, not in the C struct). */
    if (isView(t)) {
        /* A *parameter* view needs no root of its own: the caller's copy of the same view is what
         * keeps the block alive, and the caller's root table is the one a delegated sweep uses. Only
         * a local view can be the last word naming an rc block. */
        if (isParam) return;
        rcRootLeaves(g, t, base, 0);
        return;
    }
    RcScan sc = rcScanType(g, t, 0);
    if (sc == RC_CONSERVATIVE) {
        bool hasRefs = false;
        if (rcEnumRootable(g, t, 0, &hasRefs) && hasRefs) {
            rcRootLeaves(g, t, base, 0);
            return;
        }
        g->rcRootsUnreliable = true;
        return;
    }
    if (sc == RC_NO_REFS) {
        /* No reference slots -- but the storage may still hold a view, whose `data` word is a root
         * (`new T[n]` blocks hang off it). Register those and nothing else. */
        bool hasRoots = false;
        if (rcEnumRootable(g, t, 0, &hasRoots) && hasRoots) rcRootLeaves(g, t, base, 0);
        return;
    }
    rcRootLeaves(g, t, base, 0);
}

/**
 * @brief Register the reference roots that live in one declaration.
 *
 * @param g        The generator.
 * @param t        Declared type; NULL is a no-op.
 * @param base     C spelling of the declared object.
 * @param isParam  True for a parameter declaration, whose view needs no root of its own.
 */
static void rcRootDecl(CG *g, Type *t, const char *base) {
    rcRootDeclImpl(g, t, base, false);
}

/**
 * @brief Generate `EX_NEW`: allocate at the level the checker computed.
 *
 * The level may have been promoted because the value is stored somewhere longer-lived; this is
 * where memory with automatic reclamation is really taken. With a count a `slice<T>` view is
 * generated and the count is evaluated once: an impure count is marked `needTemp` by the checker
 * and stored into a temporary first.
 *
 * The `EX_NEW` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_NEW` node.
 * @return   The C text of the allocation expression or of the view literal.
 */
static const char *exNew(CG *g, Expr *e) {
    /* The context asked for the **owning** form of this buffer (`@buffer`), so the initializer is
     * that type's constructor with the element count: `buf<T>::withCap(n)`, named through the same
     * instance decoration methods use. */
    if (planNewAsBuffer(e) && planCallee(e)) {
        Buf nb;
        bufInit(&nb, g->arena);
        bufPuts(&nb, cMethodName(g, planAssocOwner(e), planCallee(e)));
        bufPutc(&nb, '(');
        bufPuts(&nb, genExpr(g, e->u.new_.count));
        cgImplicitArgs(g, &nb, planCallee(e), e, 1, false);
        bufPutc(&nb, ')');
        return bufCstr(&nb);
    }
    Type *w = subst(g, e->u.new_.type);
    /* Allocate at the level the checker computed, which may have been promoted because the value
     * is stored into a place that lives further out. This is where memory with automatic cleanup
     * is taken. */
    if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, e, "new");
    const char *ar = arenaRefAt(g, planArenaLevel(e));
    if (!e->u.new_.count) {
        /* The place of one T (or one [N]T) is simply its address. */
        /* With reclamation on, this storage comes from an **rc region**: `ref T` / `ref [N]T` are
         * reference slots that the counting functions (`extc_rc_initRef` / `storeRef` /
         * `register`) can reach, and counting only knows slab slots -- a pointer that lands in an
         * arena makes `extc_rc_slabOf` read garbage. A region is claimed by the arena's address
         * (see the table in `emitRuntimePrologue`), so its lifetime matches the arena's exactly.
         *
         * `new T[n]` (the next case) produces a `slice<T>` view, which is **not** a reference
         * slot: counting cannot reach its data pointer. The block is an rc slot all the same; the
         * view's `data` word is what keeps it alive, because `rcRootLeaves` registers `<base>.data`
         * for a view slot and the marker filters that word through `extc_rc_owns`. A large block
         * does not run into a slab size limit: `extc_rc_alloc` serves it from a slab of its own
         * size class. */
        if (g->needReclaim) {
            int lvl = planArenaLevel(e);
            /* Order: the type descriptor first (it must be visible in this statement), then the trigger
             * (it lands before the statement), and the allocation expression last -- all three use
             * the **same shard index** (see `rcAllocCore`).
             * Trigger route: ownership of the home arena / thread-local arena is decided by
 * `rcOwnKind` (the root table cannot see the calling frame's live references => the side that
 * does not own only records the owed collection). */
            const char *td = rcEmitTypeDesc(g, w);
            const char *base = arenaPrintf(g->arena, "(void *)&(%s)", ar);
            const char *core = rcAllocCore(g, base, lvl, rcOwnKind(g, lvl),
                arenaPrintf(g->arena, "(int64_t)sizeof(%s)", cType(g, w)), td);
            return arenaPrintf(g->arena, "((%s *)%s)", cType(g, w), core);
        }
        return arenaPrintf(g->arena,
            "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), \"%s\", %d))",
            cType(g, w), ar, cType(g, w), g->path, e->line);
    }
    /* `T[n]` becomes a view `{ data, len }`, with the count evaluated
     * exactly once: an impure count is marked `needTemp` by the checker
     * and stored in a temporary first. */
    const char *n;
    if (planNeedTemp(e)) {
        const char *tmp = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
        pfLine(g, "int64_t %s = (int64_t)(%s);", tmp, genExpr(g, e->u.new_.count));
        n = tmp;
    } else {
        n = genExpr(g, e->u.new_.count);
    }
    Type *st = subst(g, e->type);      /* the `slice<T>` the checker produced */
    /* Only a block at the **function-body level** goes through the rc region. That level is the one
     * where a replaced buffer has nowhere to go: an inner `{}` releases its arena when the block exits,
     * and `ARENA_HOME` means the *caller* named the release point, so both are bounded without the
     * collector. Routing those would pay for the runtime and reclaim nothing -- and it would pull units
     * that only hand arrays back to their caller out of the idle tier. */
    if (g->needReclaim && planArenaLevel(e) == 1) {
        /* The block is an rc slot in the same region the arena would have used; the view's `data`
         * word is registered as a root (see `rcRootLeaves`), which is how the marker reaches it.
         * The reference-field offsets of elements 1..n-1 vary with the run-time count, so a static
         * descriptor cannot express them: use the conservative marker (scan every word) when the
         * element type contains references, and no descriptor otherwise -- the same rule as
         * `alloc<T>(n)` in `exGencall`. */
        int lvl = planArenaLevel(e);
        const char *td = "(const ExtcRcType *)0";
        if (rcScanType(g, w, 0) != RC_NO_REFS) td = "((const ExtcRcType *)-1)";
        const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)&(%s)", ar), lvl,
            rcOwnKind(g, lvl),
            arenaPrintf(g->arena, "(int64_t)(%s) * (int64_t)sizeof(%s)", n, cType(g, w)), td);
        return arenaPrintf(g->arena, "(%s){ .data = (%s *)%s, .len = (int64_t)(%s) }",
            cType(g, st), cType(g, w), core, n);
    }
    return arenaPrintf(g->arena,
        "(%s){ .data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s),"
        " \"%s\", %d), .len = (int64_t)(%s) }",
        cType(g, st), cType(g, w), ar, n, cType(g, w), g->path, e->line, n);
}

/**
 * @brief Generate `EX_GENCALL`: `alloc<T>(n)` and the pool primitive calls.
 *
 * `alloc<T>(n)` asks the arena level the checker chose for room for n Ts and returns a pointer.
 * Zeroing is not this path's job: `extc_arena_alloc` zeroes every allocation, and the language's
 * promise rests on it -- bytes read after a lifetime ended are always initialized bytes.
 * `poolSlice` / `poolResize` / `poolGive` / `copyInto` and their `Raw` variants have the same
 * shape; only the memory belongs to the pool's own plate.
 *
 * The `EX_GENCALL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_GENCALL` node.
 * @return   The C text of the call expression.
 */
static const char *exGencall(CG *g, Expr *e) {
    /* `alloc<T>(n)`: ask the arena of the level the checker chose for the place of n values of T,
     * and hand back a pointer to it.
     *
     * Zeroing is not this path's job: `extc_arena_alloc` zeroes every allocation, and that is what
     * makes the language's promise hold -- a byte read after its lifetime ends is always an
     * initialized byte. It does so for the cheapest sufficient set of bytes: a block obtained from
     * `calloc` is zero throughout, so only a reused block (one the spare handed back) needs the
     * `memset`. */
    /* `poolSlice<T>(rid, n)` / `poolGive<T>(rid, s)`: the same shape, but the memory belongs to
     * the pool's own plate rather than to an arena. `poolGive` is how `grow` hands the replaced
     * buffer back, so a doubling container stops leaving every old generation behind.
     * `poolSliceRaw` / `poolResizeRaw` are the same two calls with the zeroing left out, for a
     * column whose every slot is written before it is read. */
    const char *gcName = e->u.gencall.name;
    if (isPoolPrimitiveName(gcName)) {
        const char *rid = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
        if (strcmp(gcName, "poolGive") == 0) {
            return arenaPrintf(g->arena,
                "extc_pool_give((int64_t)(%s), (void *)(%s).data)",
                rid, genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)));
        }
        const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
        Type *st = subst(g, e->type);      /* the `mut slice<T>` the checker produced */
        /* The count appears twice below (bytes and `.len`), so an impure one was
         * computed into a temporary by the checker. */
        size_t nIdx = (strcmp(gcName, "poolResize") == 0
                       || strcmp(gcName, "poolResizeRaw") == 0) ? 2 : 1;
        const char *n;
        if (planNeedTemp(e)) {
            const char *tmp = arenaPrintf(g->arena, "__extc_pn%d", g->tmpSeq++);
            pfLine(g, "int64_t %s = (int64_t)(%s);", tmp,
                   genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, nIdx)));
            n = tmp;
        } else {
            n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, nIdx));
        }
        if (strcmp(gcName, "copyInto") == 0) {
            /* `copyInto<T>(dst, src, n)`: one checked `memmove`. The view type comes
             * from the DESTINATION argument, not from `e->type` -- this call's type is
             * `i64` (the count it returns), so the slice has to be read off the argument
             * the checker already typed. `@unchecked` names the `..._copyU` variant,
             * which checks neither the count nor the storage. */
            Expr *dArg = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *vt = subst(g, dArg->type);
            return arenaPrintf(g->arena,
                "%s((%s)(%s), (%s)(%s), (int64_t)(%s), \"%s\", %d)",
                viewCopier(g, vt, g->uncheckedBody), cType(g, vt),
                genExpr(g, dArg),
                cType(g, vt),
                genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)),
                genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 2)),
                g->path, e->line);
        }
        if (strcmp(gcName, "poolResize") == 0 || strcmp(gcName, "poolResizeRaw") == 0) {
            return arenaPrintf(g->arena,
                "(%s){ .data = (%s *)extc_pool_%s((int64_t)(%s), (void *)(%s).data,"
                " (int64_t)(%s) * (int64_t)sizeof(%s)), .len = (int64_t)(%s) }",
                cType(g, st), tn,
                strcmp(gcName, "poolResizeRaw") == 0 ? "resize_raw" : "resize",
                rid, genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)), n, tn, n);
        }
        return arenaPrintf(g->arena,
            "(%s){ .data = (%s *)extc_pool_%s((int64_t)(%s),"
            " (int64_t)(%s) * (int64_t)sizeof(%s)), .len = (int64_t)(%s) }",
            cType(g, st), tn,
            strcmp(gcName, "poolSliceRaw") == 0 ? "take_raw" : "take",
            rid, n, tn, n);
    }
    const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
    const char *n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
    /* The level comes from the checker as well, with `alloc` meaning
     * the current block, so `g->blkLevel` is not counted here. */
    const char *ar = arenaRefAt(g, planArenaLevel(e));
    /* The `T *` returned by `alloc<T>(n)` is a reference type and is caught by reference slots =>
     * with reclamation on it must be a slab slot too, for exactly the reason given at `exNew` (a
     * region is claimed by the arena's address). The pool/plate cases want the pool's own plate
     * and are not this path's concern. */
    if (g->needReclaim) {
        int lvl = planArenaLevel(e);
        Type *et = subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0));
        /* A block of `n` elements: the reference-field offsets of elements 1..n-1 vary with the
         * run-time `n`, which a static descriptor cannot express => use the conservative marker
         * (scan every word of the block) when the element type contains references, and no
         * descriptor otherwise. */
        const char *td = "(const ExtcRcType *)0";
        if (rcScanType(g, et, 0) != RC_NO_REFS) td = "((const ExtcRcType *)-1)";
        const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)&(%s)", ar), lvl,
            rcOwnKind(g, lvl),
            arenaPrintf(g->arena, "(int64_t)(%s) * (int64_t)sizeof(%s)", n, tn), td);
        return arenaPrintf(g->arena, "(%s *)%s", tn, core);
    }
    return arenaPrintf(g->arena,
        "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d)",
        tn, ar, n, tn, g->path, e->line);
}

/**
 * @brief Generate `EX_COALESCE`: the `??` null test and fallback value.
 *
 * When the subject is impure (`f() ?? -1`), it is evaluated into a temporary before the test so a
 * side effect cannot happen twice. The fallback side is explicitly cast to the result type because
 * C's `?:` applies the usual arithmetic conversions to both arms (see the function body).
 *
 * The `EX_COALESCE` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_COALESCE` node.
 * @return   The C text of the conditional expression.
 */
static const char *exCoalesce(CG *g, Expr *e) {
    Type *mt = e->u.coalesce.main->type;
    const char *m;
    if (planNeedTemp(e)) {
        /* An impure subject (`f() ?? -1`) is computed once into a temporary, and the conditional
         * then reads that variable, so nothing is evaluated twice. `flushPrefix` emits these lines
         * before the enclosing statement. */
        const char *tmp = arenaPrintf(g->arena, "__extc_c%d", g->tmpSeq++);
        pfLine(g, "%s %s = %s;", cType(g, mt), tmp, genExpr(g, e->u.coalesce.main));
        m = tmp;
    } else {
        m = genExpr(g, e->u.coalesce.main);
    }
    const char *fb = genExpr(g, e->u.coalesce.fallback);

    /* The fallback side is cast to the result type explicitly, because C's `?:` applies the usual
     * arithmetic conversions to its two arms: an `int64_t` and an `int` together can silently widen
     * the result, an f32 payload next to a double literal being the typical case. The checker
     * already guarantees that the fallback fits the result type, so this only makes the intended
     * type visible in the generated C.
     *
     * Only a builtin scalar is cast this way: C cannot cast to an array type, so
     * `option<[3]i32> ?? arr` would stop compiling.
     *
     * The cast goes to the result type, the payload T, not to `option<T>` itself. */
    Type *rt = mt;
    if (mt && mt->kind != TY_REF && mt->targs.len > 0)
        rt = *(Type **)vecAt(&mt->targs, 0);
    bool scalar = rt && (rt->kind == TY_BUILTIN || rt->kind == TY_REF);
    const char *rhs = scalar
        ? arenaPrintf(g->arena, "((%s)(%s))", cType(g, rt), fb) : fb;

    if (mt && mt->kind == TY_REF) {
        /* A `?ref T` is a plain pointer in C, so a null test is enough. */
        return arenaPrintf(g->arena, "((%s) != ((void *)0) ? (%s) : %s)", m, m, rhs);
    }
    bool isOpt = isProtoType(mt, "option", 1);
    const char *tag = isOpt ? "some" : "success";
    return arenaPrintf(g->arena, "((%s).tag == %s_%s ? (%s).u.%s._0 : %s)",
                       m, cType(g, mt), tag, m, tag, rhs);
}

/**
 * @brief Generate `EX_ENUMVAL`: constructing an enum variant value.
 *
 * An enum without payloads is a C enum and the variant itself is a constant; an enum with
 * payloads is `struct { tag; union }` in C, so even a payloadless variant is constructed, and a
 * payload variant fills `._0`, `._1`, ... in the union.
 *
 * The type name must account for a generic instance: the C name of an instantiated enum
 * (`maybe_i64`) is not in the type table, so the type the checker resolved, recorded in
 * `assocOwner`, is preferred.
 *
 * The `EX_ENUMVAL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_ENUMVAL` node.
 * @return   The C text of the enum constant or of the construction expression.
 */
static const char *exEnumval(CG *g, Expr *e) {
    const char *tn = e->u.enumval.typeName;
    const char *vn = e->u.enumval.variant;
    /* The instantiated name of a generic enum (`maybe_i64`) is not in
     * the type table, so the type the checker resolved, recorded in
     * `assocOwner`, is preferred. */
    Type *et = planAssocOwner(e);
    if (!et && g->tt) et = ttFromName(g->tt, tn);
    /* Inside a generic instance `tn` is the template's name (`option_T`), while the instance's C
     * name is `option_i32`. With an `assocOwner` present, `subst` plus `cType` gives the real name;
     * without it the emitted name does not exist. */
    if (et) {
        Type *rt = subst(g, et);
        if (rt && rt->name) tn = rt->name;
    }
    bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);

    /* An enum without payloads is a plain C enum, so the variant is
     * itself a constant. */
    if (!payload)
        return arenaPrintf(g->arena, "%s_%s", tn, vn);

    /* An enum with payloads is a `struct { tag; union }` in C, so even
     * a payloadless variant has to be constructed:
     * `(shape){ .tag = shape_dot }`. */
    if (e->u.enumval.args.len == 0)
        return arenaPrintf(g->arena, "(%s){ .tag = %s_%s }", tn, tn, vn);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){ .tag = %s_%s, .u.%s = {", tn, tn, vn, vn);
    for (size_t i = 0; i < e->u.enumval.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, "._%zu = %s", i, genExpr(g, *(Expr **)vecAt(&e->u.enumval.args, i)));
    }
    bufPuts(&b, "} }");
    return bufCstr(&b);
}

/* ------------------------------------------------------- lambda function pointers ----
 * `fn(...)` is a bare code pointer. A lambda that captures nothing has an empty environment, so its
 * value can be that pointer; the environment's `call` method cannot be used directly, because its
 * first parameter is `self`. Each such literal therefore gets a **thunk**: a static function with
 * the lambda's own signature that calls `call` with a static empty environment. `cc -O2` inlines the
 * thunk, so the only cost left is the indirect jump every function-pointer call already has.
 *
 * The environment is empty by construction (the checker only accepts the conversion when no capture
 * was made), so the dummy is never read; it exists so that the callee is not handed a null pointer.
 * --------------------------------------------------------------------------------------- */

/**
 * @brief The C name of the thunk that turns a lambda's `call` method into a bare function pointer.
 *
 * @param g   The generator state (names are arena-allocated).
 * @param sd  The lambda's environment struct; it must have exactly one method.
 * @return    The thunk's C identifier, owned by `g->arena`.
 */
static const char *lamThunkName(CG *g, StructDef *sd) {
    /* A layout made for one instance (`LamVar`) has its own `call`, and therefore its own thunk: the
     * signature is the instance's, not the template's. */
    LamVar *v = lamVarFor(g, sd);
    if (v) return arenaPrintf(g->arena, "%s$fn", cFuncName(g, v->vcall));
    FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
    return arenaPrintf(g->arena, "%s$fn", cFuncName(g, md));
}

/**
 * @brief Print a thunk's parameter list: the lambda's own parameters, without the receiver.
 *
 * @param g   The generator state.
 * @param md  The lambda's `call` method (its first parameter is the environment).
 * @param b   The buffer to append to.
 */
static void lamThunkParams(CG *g, FuncDef *md, Buf *b) {
    if (md->params.len <= 1) { bufPuts(b, "void"); return; }
    for (size_t i = 1; i < md->params.len; i++) {
        Param *p = *(Param **)vecAt(&md->params, i);
        bufPrintf(b, "%s%s %s", i > 1 ? ", " : "", cType(g, p->type), p->cname);
    }
}

/**
 * @brief Emit one thunk definition.
 *
 * @param g   The generator state; output goes to the current buffer.
 * @param sd  The lambda's environment struct.
 */
static void emitLamThunk(CG *g, StructDef *sd) {
    FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
    Buf ps;
    bufInit(&ps, g->arena);
    lamThunkParams(g, md, &ps);
    bool vret = !md->ret || md->ret->kind == TY_VOID;
    cgLine(g, "");
    cgLine(g, "/* A `fn(...)` value for a lambda that captures nothing: the empty environment is a");
    cgLine(g, " * static dummy, so a call site never has to invent one. */");
    cgLine(g, "static %s %s(%s) {", vret ? "void" : cType(g, md->ret), lamThunkName(g, sd),
           bufCstr(&ps));
    {
        Param *selfP = *(Param **)vecAt(&md->params, 0);
        cgLine(g, "    static %s __env;", cType(g, selfP->type->inner));
    }
    Buf args;
    bufInit(&args, g->arena);
    for (size_t i = 1; i < md->params.len; i++) {
        Param *p = *(Param **)vecAt(&md->params, i);
        bufPrintf(&args, "%s%s", i > 1 ? ", " : "", p->cname);
    }
    if (vret)
        cgLine(g, "    %s(&__env%s%s);", cFuncName(g, md), args.len ? ", " : "", bufCstr(&args));
    else
        cgLine(g, "    return %s(&__env%s%s);", cFuncName(g, md), args.len ? ", " : "",
               bufCstr(&args));
    cgLine(g, "}");
}

/**
 * @brief Dispatch on the expression node and generate the C expression text for it.
 *
 * Every `case` returns a piece of C that can be embedded in an enclosing expression; an uncovered
 * node falls through to the trailing `"0"`, so a new Expr kind must add an arm here.
 *
 * @param g  The generator state: the arena and buffer come from it and errors are reported to it.
 * @param e  The expression node to generate; the caller guarantees it is not NULL.
 * @return   The generated C expression text, owned by `g->arena`.
 */
static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_EXT: return exExt(g, e);
        case EX_LAMBDA: {
            /* Used as a bare `fn(...)` value: the value is the code pointer of the thunk that wraps
             * the synthesised `call` method (the checker accepted this only for a lambda that
             * captures nothing, so the environment is empty). */
            if (planLamFnPtr(e) && e->u.lambda.sdef && e->u.lambda.sdef->methods.len == 1)
                return lamThunkName(g, e->u.lambda.sdef);
            /* The environment, built where the lambda is written: the checker puts the field values on the
             * node, so a struct literal is written by hand here. The by-name literal path is
             * unusable -- the lambda's type has no name in the source to look up. */
            StructDef *sd = e->type ? e->type->sdef : NULL;
            if (!sd) {
                ctxError(g->ctx, e->line, 1, NULL, "lambda: the checker left no environment type");
                return "0";
            }
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){", cType(g, e->type));
            for (size_t i = 0; i < e->u.lambda.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lambda.inits, i);
                bufPrintf(&b, "%s.%s = %s", i ? ", " : "", fi->name, genExpr(g, fi->value));
            }
            bufPuts(&b, "}");
            return bufCstr(&b);
        }
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT:
            /* An `f32` literal is written as the float it is: `%g` prints a `double`, and assigning that
             * to a `float` is an implicit narrowing that clang reports as
             * `-Wimplicit-float-conversion`. The value does not change; the cast only makes the
             * narrowing explicit. */
            {
                bool isF32 = e->type && e->type->kind == TY_BUILTIN && e->type->name &&
                             strcmp(e->type->name, "f32") == 0;
                /* A literal that overflows `double` (`1e400`) has no corresponding C literal, and `%g` would
                 * print it as `inf` -- in C that is an **undeclared identifier** (`-1e400` prints as
                 * `-inf` and gcc errors on it). `__builtin_inf()` / `__builtin_nan("")` say the same
                 * thing without pulling in `<math.h>`: that include would change the bytes of every
                 * output file. */
                const char *special = NULL;
                if (e->u.fval != e->u.fval)                       special = "__builtin_nan(\"\")";
                else if (e->u.fval != 0.0 && e->u.fval * 0.5 == e->u.fval)
                    special = e->u.fval < 0 ? "-__builtin_inf()" : "__builtin_inf()";
                if (special)
                    return isF32 ? arenaPrintf(g->arena, "(float)%s", special) : (char *)special;
                /* It must **round-trip**, so `%g` is not usable: `%g` keeps six significant digits and would
                 * silently change the value of any literal needing more (`4294967295.0` prints as
                 * `4.29497e+09`, so `x - 4294967296.0` yields 0 instead of -1, and the conversion of
                 * that literal then traps just outside the bound of the range it actually lies in).
                 * 17 digits round-trip a `double` and 9 digits round-trip a `float` (the cast below
                 * is what turns an f32 literal into a float, so its digit count only has to survive
                 * that step). `%g` still drops trailing zeros, so ordinary literals (`2.5`, `0.5`)
                 * are unchanged. */
                if (isF32) return arenaPrintf(g->arena, "(float)%.9g", e->u.fval);
                /* An integral-valued `f64` prints with no decimal point and no exponent (`1.0` -> `1`, `-0.0`
                 * -> `-0`), and in C that is an *integer* constant: the literal changes type, so
                 * `1.0 / 2.0` becomes integer division `(1 / 2)` = 0, `1e9 * 1e9` overflows `int` and
                 * `-0.0` loses its sign (`1.0 / z` prints `inf` instead of `-inf`), all with no
                 * diagnostic. Force a floating constant when the text itself is not one. The f32
                 * branch above is unaffected: its `(float)` cast already fixes the type. */
                {
                    char *txt = arenaPrintf(g->arena, "%.17g", e->u.fval);
                    if (!strpbrk(txt, ".eE"))
                        txt = arenaPrintf(g->arena, "%s.0", txt);
                    return txt;
                }
            }
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:
            /* `"abc"` becomes a byte view of the literal in read-only memory.
             * The length comes from `sizeof("...") - 1`, so C handles escapes
             * and nothing has to parse them here. */
            return arenaPrintf(g->arena,
                "(%s){ .data = (uint8_t *)\"%s\", .len = sizeof(\"%s\") - 1 }",
                cType(g, e->type), e->u.str.text, e->u.str.text);
        case EX_IDENT: return exIdent(g, e);

        /* `*p`: an explicit dereference is a C dereference; whether the target
         * may be written is the checker's business. */
        case EX_DEREF:
            return arenaPrintf(g->arena, "(*(%s))", genExpr(g, e->u.deref.operand));

        case EX_BIN: return genBin(g, e);

        case EX_UN:
            /* `-a` / `~a` on a narrow type promotes to `int` in C; the result must come back to the
             * operand's declared width, same rule as the binary operators (). `!` has
             * type `bool`, which `narrowWrap` leaves alone. */
            return narrowWrap(g, e->type,
                              arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand)));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: return exCall(g, e);

        case EX_INDEX: return exIndex(g, e);

        case EX_ARRAYLIT: {
            Type *t = e->type;
            if (!t || t->kind != TY_ARRAY) return "0";
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .data = {", cType(g, t));
            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
            }
            /* A trailing `...` needs nothing extra: a C initializer zero-fills
             * the remaining elements by itself. */
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }

        case EX_SLICE: return genSlice(g, e);

        case EX_TRY:
            /* `?` must not reach here: it expands at statement level, where
             * genStmt handles it directly. Arriving here means the checker's
             * position rule has a hole; report it rather than silently generate
             * wrong C. */
            ctxError(g->ctx, e->line, 1, NULL,
                     "internal: `?` reached expression codegen (position check missed it)");
            return "0";

        case EX_ASSOC: return exAssoc(g, e);

        /* Numeric conversion. Without `convCheck` the checker has proven the
         * value fits and a plain cast is enough; with it the value is checked
         * against a range and traps with a source position. */
        case EX_CONV: return exConv(g, e);

        /* `new T`, `new [N]T` and `new T[n]`: allocate into a block arena and
         * zero the storage. The runtime allocation zeroes on its own, so extC
         * has exactly one rule about fresh memory. */
        case EX_NEW: return exNew(g, e);

        case EX_GENCALL: return exGencall(g, e);

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_DYN:       return genDynValue(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);

        case EX_REF:
            return arenaPrintf(g->arena, "&(%s)", genExpr(g, e->u.ref.operand));

        /* `a ?? b`: the value if there is one, otherwise the fallback.
         *
         *   - A pure subject - a variable, field or index - becomes a C
         *     conditional directly: `x.tag == t_some ? x.u.some._0 : b`.
         *   - An impure subject, as in `f() ?? -1`, is marked `needTemp` by the
         *     checker and computed into a temporary first; flushPrefix emits
         *     those lines before the enclosing statement, which the
         *     `planNeedTemp(e)` branch below relies on.
         *
         * A C conditional evaluates only one side, so the fallback's side
         * effects do not run when the value is present. */
        case EX_COALESCE: return exCoalesce(g, e);

        /* `e!` asserts that the value is present -- the author signs for it, but
         * the signature itself is **checked at runtime**: the merit
         * of `!` is that it is short, not that it is silent. So:
         *   - `p!` on a `?ref T` calls `extc_refAssert` and yields the pointer;
         *   - `opt!` / `r!` call `extc_tagAssert` first and only then read the
         *     payload, so a `none` / failed arm traps with a position instead of
         *     handing back a zero value.
         * The helpers return what they checked, so the operand is evaluated once;
         * `opt!` / `r!` go one step further and become a call to a per-instance
         * **unwrap helper** (`unwrapHelper`), so the operand is a function argument
         * and is named exactly once. That is what lets `!` stand in a `while`
         * condition, where a statement-prefix temporary cannot live.
         * A function marked `@unchecked` gets the raw behaviour: it is the
         * one signed road to undefined behaviour (manual 0.5 / 12.5). */
        case EX_SIGN: {
            Type *ot = subst(g, e->u.sign.operand->type);
            bool isRef = ot && ot->kind == TY_REF;
            bool isOpt = !isRef && isProtoType(ot, "option", 1);
            const char *var = isOpt ? "some" : "success";
            if (g->uncheckedBody) {
                const char *v = genExpr(g, e->u.sign.operand);
                if (isRef) return v;
                return arenaPrintf(g->arena, "(%s).u.%s._0", v, var);
            }
            /* `o! = v` writes the payload, so the comma expression below cannot be
             * used: C11 6.5.17p3 says its result is not an lvalue. The assertion goes
             * into the statement prefix (the same mechanism `??` uses) and the payload
             * itself is returned as the lvalue.
             *
             * A call operand (`f()!.field = v` on a `?ref T`) is not a C lvalue, so the
             * expression may not appear twice -- once in the assert, once in the
             * assignment: evaluate it into a temporary of the same type and assert that.
             * A place operand (`p! = v`) stays exactly as it is, because the payload must
             * remain the lvalue the assignment writes. */
            if (g->inAssignTarget) {
                const char *v = genExpr(g, e->u.sign.operand);
                if (!printArgIsPlace(e->u.sign.operand)) {
                    const char *tmp = arenaPrintf(g->arena, "__extc_sig%d", g->tmpSeq++);
                    pfLine(g, "%s %s = %s;", cType(g, e->type), tmp, v);
                    v = tmp;
                }
                if (isRef) {
                    pfLine(g, "(void)extc_refAssert((void *)(%s), \"%s\", %d);",
                           v, g->path, e->line);
                    return v;
                }
                pfLine(g, "(void)extc_tagAssert((int)(%s).tag, %s_%s, \"%s\", %d);",
                       v, cType(g, ot), var, g->path, e->line);
                return arenaPrintf(g->arena, "(%s).u.%s._0", v, var);
            }
            if (isRef) {
                /* A nullable reference is a plain pointer: the assert itself returns what
                 * it checked, so a single mention of the operand is already evaluated once
                 * and no helper is needed. */
                return arenaPrintf(g->arena,
                    "((%s)extc_refAssert((void *)(%s), \"%s\", %d))",
                    cType(g, e->type), genExpr(g, e->u.sign.operand), g->path, e->line);
            }
            const char *fn = unwrapHelper(g, ot, isOpt, e->type);
            return arenaPrintf(g->arena, "%s(%s, \"%s\", %d)",
                               fn, genExpr(g, e->u.sign.operand), g->path, e->line);
        }

        /* `null`, the zero value of a nullable reference, is a null pointer in
         * C. The checker has already proven that a null test narrows the value
         * before any use, so no runtime check is generated: what the compiler
         * can prove leaves no trace at runtime. */
        case EX_NULL:
            return nullValue(g, e->type);

        case EX_ENUMVAL: return exEnumval(g, e);
    }
    return "0";
}

/**
 * @brief Whether a value of this type may contain a view, so boxing it must copy the bytes it names.
 *
 * A view is a pointer plus a length: copying the view copies an address. An `any` owns its payload,
 * so the payload copied into the box has to be the data, not somebody else's address -- otherwise a
 * box returned from a function would name a buffer that died with the frame.
 *
 * The answer is computed from the **static** type at the boxing site, so types without views pay
 * nothing: code generation passes a constant to `extc_any_box`, and the memcpy path stays.
 *
 * @param g  The generator (field types are substituted in an instance's context).
 * @param t  The type of the value being boxed.
 * @return   true when the payload has to be deepened.
 */
static bool typeNeedsAnyDup(CG *g, Type *t) {
    t = ttBase(subst(g, t));
    if (!t) return false;
    switch (t->kind) {
    case TY_ARRAY:
        return true;                       /* an array is stored inline; its elements may be views */
    case TY_GENERIC:
        if (ttIsViewType(t)) return true;  /* `slice<T>` / the byte view: the data is elsewhere */
        /* fall through: an instance is laid out like a struct */
        /* FALLTHROUGH */
    case TY_STRUCT: {
        StructDef *sd = t->sdef;
        for (size_t i = 0; sd && i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (typeNeedsAnyDup(g, fd->type)) return true;
        }
        return false;
    }
    case TY_ENUM:
        return true;                       /* a variant's payload may hold a view; be conservative */
    case TY_REF:
        return false;                      /* borrows are rejected at the boxing site, not deepened */
    /* kind-default: only views, arrays, aggregates and enums can carry bytes that live somewhere
     * else. A scalar, a function value or a borrow is copied as it stands, so the memcpy path is
     * right for every kind that reaches here -- and a future kind has to be looked at, because
     * "the bytes are the value" is exactly what this predicate decides. */
    default:
        return false;
    }
}

/**
 * @brief Emit the boxing of a value into an `any`.
 *
 * The value is evaluated once, into a temporary of its own type, and that temporary is copied by
 * `extc_any_box` into the arena the `any` owns its bytes in. Copying (rather than pointing at the
 * operand) is what makes the `any` a **value**: `var x: any = 1`, then `x = "abc"`, keeps working
 * because each assignment replaces the bytes.
 *
 * @param g     The generator.
 * @param e     The source expression (the one the checker marked with `planAnyBox`).
 * @param code  Its generated C value.
 * @return      The name of the `ExtcAny` temporary holding the boxed value.
 */
static const char *emitAnyBox(CG *g, Expr *e, const char *code) {
    Type *t = ttBase(subst(g, e->type));
    g->needAny = true;
    g->needRuntime = true;
    const char *tmp = arenaPrintf(g->arena, "__extc_any%d", g->tmpSeq++);
    /* The arena is the **home** one: the value a boxed `any` owns may outlive this frame (it can be
     * returned or stored), so the copy must not sit in a block that dies at the end of the
     * statement. The checker registers the site so this function is handed a home arena. */
    const char *src = arenaPrintf(g->arena, "__extc_anyc%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), src, code);
    pfLine(g, "ExtcAny %s = extc_any_box(%s, %s, (const void *)&%s, (int64_t)sizeof(%s), %d);",
           tmp, homeArg(g, ARENA_HOME), descRef(g, t), src, cType(g, t),
           typeNeedsAnyDup(g, t) ? 1 : 0);
    return tmp;
}

/**
 * @brief Emit the unboxing of an `any` into a concrete type.
 *
 * The check happens at run time and traps with a source position when the `any` holds another
 * type -- that is the whole of the "implicit unboxing" rule: the compiler believes the source, the
 * run time verifies it, and a wrong belief stops the program at the line that had it.
 *
 * @param g     The generator.
 * @param e     The `any` expression the checker marked with `planAnyTo`.
 * @param want  The type the context expects.
 * @param code  The generated C value of the `any`.
 * @return      The name of the temporary holding the unboxed value.
 */
static const char *emitAnyUnbox(CG *g, Expr *e, Type *want, const char *code) {
    g->needAny = true;
    g->needRuntime = true;
    const char *addr;
    if (printArgIsPlace(e)) {
        addr = arenaPrintf(g->arena, "&(%s)", code);
    } else {
        const char *t = arenaPrintf(g->arena, "__extc_anyb%d", g->tmpSeq++);
        pfLine(g, "ExtcAny %s = %s;", t, code);
        addr = arenaPrintf(g->arena, "&%s", t);
    }
    const char *tmp = arenaPrintf(g->arena, "__extc_anyv%d", g->tmpSeq++);
    pfLine(g, "%s %s = *((const %s *)extc_any_unbox(%s, %s, \"%s\", %d));",
           cType(g, want), tmp, cType(g, want), addr, descRef(g, want), g->path, e->line);
    return tmp;
}

/**
 * @brief Generate an expression in value position, adding an automatic dereference when needed.
 *
 * The checker treats a `ref T` in value position as `T` and stamps the node with `deref`; that
 * dereference is added here. So `let y = p` copies the value and `p + 1` is numeric addition,
 * while whether the target is writable still depends on the `ref` and `mut ref` in the type.
 *
 * Wrapping it around genExprInner is what makes the rule complete: every path that generates an
 * expression value (recursive calls included) goes through here, so no path can miss the
 * dereference.
 *
 * @param g  The generator state.
 * @param e  The expression node; must not be NULL (a caller that violates this has a bug).
 * @return   The generated C expression text, owned by `g->arena`.
 */
static const char *genExpr(CG *g, Expr *e) {
    /* Every caller passes an expression node: this value position has no optional AST field, so
     * NULL is a caller bug. The check documents the invariant for both readers and static
     * analyzers. */
    assert(e != NULL);
    const char *s = genExprInner(g, e);
    /* The two conversions the checker accepted around this expression. Both are marked on the
     * operand, not on a new node: the value keeps its own type and only the emission site knows
     * that a wrapping or unwrapping happens here. */
    if (planAnyBox(e)) {
        s = emitAnyBox(g, e, s);
    } else if (planAnyTo(e)) {
        s = emitAnyUnbox(g, e, subst(g, (Type *)planAnyTo(e)), s);
    }
    if (planDeref(e)) return arenaPrintf(g->arena, "*(%s)", s);
    return s;
}

/* ---------------------------------------------------------------- debug printing
 *
 * Recursively printing every field of a struct is something only the compiler
 * can do, the equivalent of Rust's `#[derive(Debug)]`: extC has no reflection,
 * so "walk all fields" cannot be written in the language at all.
 *
 * This is not the same as putting the standard library inside the compiler. What
 * the compiler emits is data plus one generic printer, not one printer per type:
 * see genStructDesc above and the `extc_print` at the top of the generated file.
 * The boundary is the rule that whatever can be expressed in extC belongs in
 * extC, not in the compiler.
 */

/* ---------------------------------------------------------------- statements */

static void genStmt(CG *g, Stmt *s);

/* Declared here because statements register their local declarations as they are emitted
 * (see DeadLocal): the definition sits with the other unreferenced-definition helpers. */
static void localDef(CG *g, size_t before, const char *name, size_t own);

/**
 * @brief Write a value destined for an `f32` slot as a real float.
 *
 * `let e: f32 = 3.14` would otherwise emit `float e = 3.14;`: the literal is a `double`, the
 * assignment narrows implicitly and clang reports `-Wimplicit-float-conversion`. An explicit cast
 * leaves the value unchanged and makes the narrowing non-implicit.
 *
 * @param g       The generator state, whose arena the cast text is built in.
 * @param target  The target slot's type; the cast is added only for a built-in `f32`.
 * @param src     The source expression, whose type decides whether a cast is needed; NULL means
 *                return as is.
 * @param val     The already generated source value text, which must be evaluated only once.
 * @return        `(float)val`, or `val` unchanged (when the target is not `f32` or the source is
 *                not `f64`).
 */
static const char *asF32(CG *g, Type *target, Expr *src, const char *val) {
    if (!target || target->kind != TY_BUILTIN || !target->name || strcmp(target->name, "f32") != 0)
        return val;
    if (!src || !src->type || src->type->kind != TY_BUILTIN || !src->type->name) return val;
    if (strcmp(src->type->name, "f64") != 0) return val;
    return arenaPrintf(g->arena, "(float)%s", val);
}

/**
 * @brief Strip the one redundant pair of parentheses that wraps a whole condition.
 *
 * A statement carries its own parentheses and the expression printer adds another pair around a
 * comparison, so `if x == y` prints as `if ((x == y))` and clang reports
 * `-Wparentheses-equality`.
 *
 * Only the pair wrapping the whole condition is stripped: a `)` met inside a string literal that
 * does not close at the end makes the text come back unchanged -- the cost is one redundant pair
 * of parentheses and the meaning is unchanged.
 *
 * @param g  The generator state; the new string is built in its arena while stripping.
 * @param e  The condition text; returned as is when it is NULL or does not start with `(`.
 * @return   The text without the outer parentheses, or `e` unchanged.
 */
static const char *cgCond(CG *g, const char *e) {
    if (!e || e[0] != '(') return e;
    int depth = 0;
    for (size_t i = 0; e[i]; i++) {
        if (e[i] == '(') depth++;
/**
 * @brief Find the `)` that exactly closes the outermost `(`, that is, the end of the condition.
 *
 * If characters remain after that `)`, the pair does not wrap the whole condition and the text is
 * returned unchanged; otherwise the pair is removed and the rest handed back.
 */
        else if (e[i] == ')' && --depth == 0) {
            if (e[i + 1]) return e;                  /* not the whole condition */
            Buf b;
            bufInit(&b, g->arena);
            bufPutn(&b, e + 1, i - 1);
            return bufCstr(&b);
        }
    }
    return e;
}

/* ------------------------------------------------------------- `?` expansion
 *
 * `?` works at statement level, because C has no statement expressions, so it
 * expands into
 *     <evaluate once>  ->  <return on failure>  ->  <use the payload>
 *
 * What the compiler knows here is the protocol of the two result types, in the
 * same spirit as the `data` and `len` of a view: the struct, its methods and its
 * constructors all live in the prelude, and only a few names are fixed -
 * `some` and `none` for `option`, `success` and `failure` for `result`, with the
 * payload at `u.<variant>._0`. main.c verifies that those names exist where they
 * are needed.
 *
 * `option` and `result` are ordinary enums here, so everything below goes
 * through a tag comparison and a payload path; there are no `has`/`ok` fields
 * to read and rebuild.
 */
typedef struct {
    const char *tmp;      /* holds the operand, evaluated exactly once */
    const char *inst;     /* C name of the instance: option_i64, result_unit_gameError */
    const char *okVar;    /* success variant: some / success */
    const char *failVar;  /* failure variant: none / failure */
    Type       *payload;  /* payload type T, carried by the success variant */
    bool        isOpt;    /* true for option, false for result */
} TryInfo;

/* Is this type an instance of a given generic type with a given arity?
 *
 * The definition name is compared, not the instantiated C name, so `option<i64>`
 * is still recognized as `option`.
 *
 * Params:
 *   name  - name of the generic definition, such as "option"
 *   nargs - required number of type arguments
 *
 * Returns:
 *   true for a generic struct (`slice<T>`, `varArray<T>`) or a generic enum
 *   (`option<T>`, `result<T,E>`) with that name and arity.
 */
/**
 * @brief Give the C path of the success-side payload.
 *
 * It is `x.u.some._0` or `x.u.success._0`: `option` and `result` are both
 * `struct { tag; union }` in C and the payload is always at `u.<success variant>._0`.
 *
 * @param g   The generator state.
 * @param ti  The descriptor already filled in by genTryHead: `tmp` and `okVar` build the path.
 * @return    The C lvalue expression of that payload.
 */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/**
 * @brief Construct the value a `?` operand's failure has to return.
 *
 * The failure value is constructed according to the outer function's return type: for `option`
 * it is its zero value, that is, `none`; for `result` it is a failure variant carrying the inner
 * error.
 *
 * @param g   The generator state.
 * @param ti  The descriptor of the `?` operand; `isOpt` distinguishes `option` from `result`,
 *            and `tmp` / `failVar` move the inner error.
 * @return    The C expression of the return type; `"0"` when the outer function has no return
 *            type.
 */
static const char *genTryFail(CG *g, TryInfo *ti) {
    Type *rt = subst(g, g->retType);
    if (!rt) return "0";
    if (ti->isOpt) return zeroValue(g, rt);
    /* For a `result` the inner error is copied across as it is: the error type
     * E is the same, so the C member is read directly. */
    return arenaPrintf(g->arena,
                       "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s.u.%s._0 } }",
                       cType(g, rt), rt->name, ti->failVar, ti->failVar,
                       ti->tmp, ti->failVar);
}

/**
 * @brief Decrement the recursion depth counter on one exit path.
 *
 * The decrement must come after the return value is computed and never before it: decrementing
 * first turns `return spin(n)` into `--depth; return spin(n);`, so every tail-call frame resets
 * the counter and the guard never fires (at -O0 gcc folds that shape into a tail call and the
 * program exits at once with no output).
 *
 * A `return` is not the only exit: the implicit return at the end of the body is one too, so the
 * depth is also decremented where the body ends; an explicit return that already decremented
 * through cgReturn does not decrement a second time.
 *
 * @param g  The generator state; nothing is emitted when the function is not self-recursive.
 */
static void cgRecLeave(CG *g) {
    if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
}

/** @brief Run the `@drop` hooks of every local still in scope (see the definition). */
static void genReturnDrops(CG *g);

/**
 * @brief Return a value from the current function.
 *
 * Every exit funnels into one epilogue instead of repeating the arena release sequence at each
 * return: the value is stored into `__extc_ret_v`, the recursion depth is decremented, and control
 * jumps to `__extc_ret`, where that release sequence appears once.
 *
 * The value is computed before the arenas are released, slightly more conservative than
 * "release, then evaluate".
 *
 * @param g    The generator state.
 * @param val  The C expression of the value to return; NULL means a bare return (a `void` exit).
 *             A function with `noArena` returns directly: it does not allocate and has no arena
 *             array to release.
 */
static void cgReturn(CG *g, const char *val) {
    /* The buffered console tail leaves here (ruling 90): every `return` of `main` is a
     * place where the process is about to end and whatever is still buffered would be
     * lost. It is a no-op when the buffer is empty. */
    if (g->inMain && g->needCout) cgLine(g, "extc_cout_flush();");
    if (g->noArena) {
        genReturnDrops(g);
        if (g->isRecursive) {
            /* The return type need not be scalar: a self-recursive function may return `option<T>` or a
             * struct. The temporary is therefore declared with the real return type and **no cast**
             * is added (C does not allow a cast to a struct type, and a scalar assignment converts
             * implicitly anyway). */
            if (val) cgLine(g, "{ %s __r = %s; --__extc_rec_depth; return __r; }",
                            cType(g, g->curFunc->ret), val);
            else     cgLine(g, "{ --__extc_rec_depth; return; }");
        } else if (val) cgLine(g, "return %s;", val);
        else            cgLine(g, "return;");
        return;
    }
    if (val) cgLine(g, "__extc_ret_v = %s;", val);
    /* `@drop` hooks run after the value is stored and before the jump: a local that owns state
     * hands it back on the way out, innermost block first. */
    genReturnDrops(g);
    cgRecLeave(g);
    cgLine(g, "goto __extc_ret;");
}

/**
 * @brief Emit the statement that "evaluates the `?` operand once and returns on failure", and
 *        describe the result to the caller.
 *
 * The failure branch's return is already written into the line emitted here, so "release every
 * enclosing arena level" must be settled before that line.
 *
 * @param g  The generator state of the `?`.
 * @param e  The `EX_TRY` node.
 * @return   The TryInfo describing the operand's temporary, its C type name and the variant name;
 *           the caller reads the payload through it.
 */
static TryInfo genTryHead(CG *g, Expr *e) {
    TryInfo ti;
    Type *ot = ttBase(subst(g, e->u.try_.operand->type));
    ti.isOpt = isProtoType(ot, "option", 1);
    ti.inst = cType(g, ot);
    ti.okVar = ti.isOpt ? "some" : "success";
    ti.failVar = ti.isOpt ? "none" : "failure";
    ti.payload = subst(g, *(Type **)vecAt(&ot->targs, 0));
    ti.tmp = arenaPrintf(g->arena, "__extc_try%d", g->tmpSeq++);

    const char *operand = genExpr(g, e->u.try_.operand);
    flushPrefix(g);
    cgLine(g, "%s %s = %s;", ti.inst, ti.tmp, operand);
    /* A failure returns, so every enclosing arena level is released, exactly as
     * for a `return` statement, through the shared epilogue. That decision has
     * to be taken before this line is written, because the line already
     * contains the return. */
    cgLine(g, "if (%s.tag != %s_%s) {", ti.tmp, ti.inst, ti.okVar);
    g->indent++;
    if (g->inMain) {
        /* `?` in `main`: the failure is fatal, so it is reported and the program stops. The payload is
         * printed through its descriptor, which is the same machinery `println(e)` uses -- so
         * `ioError.readFailed(5)` still says which fd failed rather than "something failed". A
         * `none` has no payload. */
        if (!ti.isOpt) {
            Type *et = subst(g, *(Type **)vecAt(&ot->targs, 1));
            g->needRuntime = true;               /* the descriptor table ... */
            g->needPrint   = true;               /* ... and `extc_print`, called just below */
            g->needCout    = true;               /* the descriptor printer writes the console buffer */
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: a step in `main` failed: \","
                      " \"%s\", %d);", g->path, e->line);
            cgLine(g, "extc_print(&(%s.u.%s._0), %s);", ti.tmp, ti.failVar, descRef(g, et));
            /* The payload is written through the **console buffer** and the process is about to call
             * `extc_die(1)`: it must be flushed here, or the trap message keeps only its first half
             * (`extc_die` does not go through the exit registered with `atexit`). */
            cgLine(g, "extc_cout_flush();");
            cgLine(g, "fprintf(stderr, \"\\n\");");
        } else {
            cgLine(g, "extc_trapMsg(\"%s\", %d, \"a step in `main` returned `none`\");",
                   g->path, e->line);
        }
        cgLine(g, "extc_die(1);");
    } else {
        cgReturn(g, genTryFail(g, &ti));
    }
    g->indent--;
    cgLine(g, "}");
    return ti;
}


/**
 * @brief Emit one `#line` directive pointing back at the .extC source position of this statement.
 *
 * @param g  The generator state; nothing is emitted while line mapping is off.
 * @param s  The statement; nothing is emitted either when `s->line` is 0 (the statement carries
 *           no line number).
 */
static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

/* Return the arena argument for a call to a callee that needs a home arena.
 *
 * Which arena to pass has already been decided by the checker and recorded in
 * `Expr.arenaArg`: `ARENA_HOME` means this function's own home arena, and a
 * value of 1 or more means the block arena at that level. Code generation only
 * translates that decision and never makes one; a fallback that second-guessed
 * the checker here would be a second authority on the same question.
 *
 * Returns:
 *   A C expression denoting the arena, usable as a function argument.
 */
/**
 * @brief Give the zone argument this call site passes to the callee, that is, "which place the
 *        pool should be created in".
 *
 * Parallel to homeArg: the arena side passes "the arena the caller chose" and this side passes
 * "the zone index of the place the caller chose".
 *
 *   - When this function itself receives the parameter (`funcHasZoneParam`) it is passed straight
 *     down: along `vector<i32>::new` -> `withCap` -> `pool<V>::withParent` -> `extc_pool_new_at`
 *     the place the user chose outermost reaches the runtime;
 *   - otherwise (`main` in the entry file, an ordinary function) it passes the current place's
 *     zone, that is, the most recently pushed `__extc_zm<lvl>` -- exactly "the pool is created in
 *     the block that creates it".
 *
 * The site records its own level (the checker's `zoneLevel`): `ZONE_HOME` = the home zone this
 * function received (passed down), k>=1 = `__extc_zm<k>` of the place at level k.
 *
 * The fallback is the runtime's own "the place I am in right now", `extc_zoneTop`: it is exactly
 * equivalent to `(extc_pool_zoneDepth() - 1)` (in the runtime `zoneDepth() = zoneTop + 1`), but
 * its meaning is direct instead of drifting with the definition of `zoneDepth()`. On a single
 * stack it is the place the caller is in. Only a `noArena` function reaches this (it pushed no
 * zone); so does `stmtMakesPool` when it conservatively answers true for an unresolved callee that
 * does not actually create a pool, in which case there is neither a zone nor a runtime yet the
 * call site still has to produce the second argument. This arm references the runtime's zone
 * table, so the pool runtime is marked as needed here; the runtime text is only emitted after the
 * function bodies (`if (g->needPool) poolsEmitRuntime(...)`), so there is time.
 *
 * @param g  The generator state.
 * @param e  The call-site expression; NULL or no zone level takes the fallback.
 * @return   A C expression usable directly as a function argument, naming a zone.
 */
static const char *zoneArgRef(CG *g, Expr *e) {
    /* When the site records its own level (the checker's `zoneLevel`), use it: `ZONE_HOME` = the
     * home zone this function received (passed down), k>=1 = `__extc_zm<k>` of the place at level
     * k. */
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonearg] %s node=%p lvl=%d mark1=%d mark2=%d blk=%d\n",
                g->curFuncName ? g->curFuncName : "-", (void *)e, e ? planZoneLevel(e) : 0,
                (int)g->zoneMark[1], (int)g->zoneMark[2], g->blkLevel);
    /* Inside a coroutine's step the place is **read from the frame**: rule 1 of the design says a
     * resume must not re-derive it from the environment, and a step has nothing to re-derive it from
     * anyway -- no hidden parameters, no zone mark of its own. */
    if (g->coroFunc) return "f->zone";
    if (e && planZoneLevel(e) != 0) {
        if (planZoneLevel(e) == ZONE_HOME) {
            if (g->funcHasZoneParam) return "__extc_home_zone";
            /* `main`'s C signature is fixed and it receives no home-zone parameter => its "home" is **its
             * own body** (the same rule as `__extc_home = &__extc_a[1]` for the home arena).
             *
             * Falling through to "the most recently pushed zone" below is the **wrong** thing here:
             * a pool upgraded inside an inner block is registered on that block's zone and collected
             * when the block ends, while the receiver is still alive. SSO's `push` is the first
             * method that creates a pool for its receiver: a loop pushing past byte 17 upgrades =>
             * each iteration collects the pool at block end => the next `pStale` poisons the string
             * into an empty one, `len=3` instead of 20; when the string is declared outside the loop
             * it is worse -- the pool has been returned to malloc and the read sees reused memory.
             * The home zone lives longer and only costs memory (the same direction as escape
             * analysis preferring to over-retain). */
            if (g->zoneFrameMarked) return "__extc_zm1";
        }
        if (planZoneLevel(e) == 1 && g->zoneFrameMarked) return "__extc_zm1";
        if (planZoneLevel(e) > 1
            && planZoneLevel(e) < g->levelCap
            && g->zoneMark[planZoneLevel(e)])
            return arenaPrintf(g->arena, "__extc_zm%d", planZoneLevel(e));
    }
    if (g->funcHasZoneParam) return "__extc_home_zone";
    for (int lvl = g->blkLevel; lvl >= 1; lvl--) {
        if (lvl < g->levelCap && g->zoneMark[lvl])
            return arenaPrintf(g->arena, "__extc_zm%d", lvl);
    }
    /* The fallback references the runtime zone table, so the pool runtime is marked as "needed"
     * here -- the runtime text is only emitted after the function bodies
     * (`if (g.needPool) poolsEmitRuntime(...)`), so there is time. The typical way to reach this
     * arm is `stmtMakesPool` conservatively answering true for an unresolved callee: that function
     * does not actually create a pool, so there is neither a zone nor a runtime, yet the call site
     * still has to produce the second argument. */
    g->needPool = true;
    return "extc_zoneTop";
}

/**
 * @brief Write the arena level the checker chose as one arena argument.
 *
 * The result is the pointer itself, passed as an argument, and must not be confused with
 * arenaRefAt: the caller wraps that one in `&`, so it yields the dereferenced form
 * `(*__extc_home)`.
 *
 * @param g         The generator state; the subscript form is built in its arena.
 * @param arenaArg  The arena level: `ARENA_HOME` (the function's own home arena) or a block
 *                  level of 1 and above.
 * @return          `__extc_home` or `&__extc_a[arenaArg]`.
 */
static const char *homeArg(CG *g, int arenaArg) {
    if (arenaArg == ARENA_HOME) return "__extc_home";
    if (arenaArg >= 1) g->rcArenaUsed |= 1u << arenaArg;   /* the callee may allocate into this arena */
    return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);
}

/**
 * @brief Predeclare the "region pointer for this region base address" local and return its name
 *        (or just the name when it is already declared).
 *
 * The declaration must be in a scope that **covers every use site**: some `regionOf` call sites
 * sit inside `{ ... }` statement blocks (trigger / owed check) and others inside allocation
 * expressions, so every declaration is emitted at the **top of the function body**
 * (`rcPredeclare`, from the base-address set collected by a first scan over the body's `EX_NEW`
 * nodes). The local is recorded in `localDef` (`own = 1`): one that no site reads is removed by
 * the dead-local pass.
 *
 * `rid` is only stored into the struct by `regionNew` and has no reader (see
 * `src/back/reclaim.c`), so passing the level number of the first encounter does not change
 * behavior.
 *
 * @param g        The generator state.
 * @param basePtr  The region base-address expression (the caller has already applied the
 *                 `(void *)` / `&` form).
 * @param lvl      The block level (only used at the moment the region is created).
 * @return         The local's name; NULL when this base address was not predeclared (the caller
 *                 falls back to the inline call).
 */
static const char *rcRegionDeclare(CG *g, const char *basePtr, int lvl) {
    for (int i = 0; i < g->rcRegN; i++)
        if (strcmp(g->rcRegBase[i], basePtr) == 0) return g->rcRegName[i];
    if (g->rcRegN >= 40) return NULL;
    int i = g->rcRegN++;
    g->rcRegBase[i] = arenaPrintf(g->arena, "%s", basePtr);
    snprintf(g->rcRegName[i], sizeof g->rcRegName[i], "__extc_rcr%d", i);
    size_t before = g->out->len;
    cgLine(g, "ExtcRcRegion *%s = extc_rc_regionOf(%s, %d);",
           g->rcRegName[i], basePtr, lvl);
    localDef(g, before, g->rcRegName[i], 1);   /* declared inside the function body: removed when nothing reads it */
    return g->rcRegName[i];
}

/**
 * @brief Find the already predeclared region pointer local; NULL when it was not declared (the
 *        caller falls back to the inline call).
 *
 * @param g        The generator state.
 * @param basePtr  The region base-address expression.
 * @return         The local's name, or NULL.
 */
static const char *rcRegionRef(CG *g, const char *basePtr) {
    for (int i = 0; i < g->rcRegN; i++)
        if (strcmp(g->rcRegBase[i], basePtr) == 0) return g->rcRegName[i];
    return NULL;
}

typedef struct { CG *g; } RcPreCtx;
static bool rcPreStmt(void *ctx, Stmt *s);
static bool rcPreExpr(void *ctx, Expr *e);

/**
 * @brief First scan over the function body: convert the arena level of every `new` point into a
 *        "region pointer" local.
 *
 * Only `EX_NEW` is scanned: rc allocation happens only at those points (the trigger / owed check
 * and the allocation expression share the same base-address text). The base-address text is
 * **character for character** the text `exNew` generates -- both sides use
 * `arenaPrintf("(void *)&(%s)", arenaRefAt(...))`, because lookup compares by text.
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcPredeclare(CG *g, Stmt *body) {
    if (!g->needReclaim || !body) return;
    /* A coroutine body is a `switch (pc)` + `goto` resume state machine: resuming can **jump into**
     * the middle of the body, so a local computed once at function entry is uninitialized on the
     * path that skips it (gcc reports `-Wmaybe-uninitialized`). Allocations inside a coroutine
     * frame stay inline at the call site and are not hoisted. */
    if (g->coroFunc) return;
    RcPreCtx c = { g };
    AstVisit v = { rcPreExpr, rcPreStmt, &c };
    astWalkStmtChildren(body, &v);
}

/**
 * @brief Statement callback: keep walking (`new` may be in any statement).
 *
 * @param ctx  The prescan context (`RcPreCtx *`).
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcPreStmt(void *ctx, Stmt *s) {
    RcPreCtx *c = ctx;
    AstVisit v = { rcPreExpr, rcPreStmt, c };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Expression callback: on `EX_NEW`, register its arena base address as a "region pointer"
 *        local.
 *
 * @param ctx  The prescan context (`RcPreCtx *`).
 * @param e    The child expression being visited.
 * @return     Always true.
 */
static bool rcPreExpr(void *ctx, Expr *e) {
    RcPreCtx *c = ctx;
    CG *g = c->g;
    if (e && e->kind == EX_NEW) {
        int lvl = planArenaLevel(e);
        if (lvl == ARENA_HOME || lvl >= 1) {
            const char *base = arenaPrintf(g->arena, "(void *)&(%s)", arenaRefAt(g, lvl));
            rcRegionDeclare(g, base, lvl);
        }
    }
    AstVisit v = { rcPreExpr, rcPreStmt, c };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Give the arena an allocation point at some level refers to.
 *
 * The level is computed by the checker and recorded in `Expr.arenaLevel`: the starting point is
 * the block the statement is in, promoted when the value is stored somewhere longer-lived; in a
 * function with a home arena every site is already `ARENA_HOME`.
 *
 * There is not one fallback here that re-decides the level for the checker: returning
 * `(*__extc_home)` for every function that has a home arena would make a second authority -- the
 * checker computes a level and codegen picks another. Once the closure is built the checker
 * rewrites every site from `FuncDef.arenaSites`, so no decision is needed here.
 *
 * The caller wraps this result in `&`, so what is given here is in the dereferenced form of
 * `extc_arena *`.
 *
 * @param g      The generator state.
 * @param level  `ARENA_HOME`, or a block level of 1 and above.
 * @return       `(*__extc_home)` (the caller-chosen, longest-lived arena), or `__extc_a[level]`
 *               (freed when the block at level `level` ends); a parallel worker call chain uses
 *               the thread-local `(*extc_tls_arena)` and a coroutine step uses the frame's
 *               `arena<level>`.
 */
static const char *arenaRefAt(CG *g, int level) {
    /* On the parallel::run call chain (the worker and every function it calls) use the
     * **thread-local** arena: the frame arena (`__extc_a[]`) belongs to the main thread and two
     * workers using it at once is a data race; a worker's signature is fixed (it only writes out
     * and returns i64) => an allocation cannot escape the worker => thread-local is safe. */
    if (g->parTls) return "(*extc_tls_arena)";
    if (level == ARENA_HOME) return "(*__extc_home)";
    /* A step has no C-stack arena array: the block arenas live **in the frame** (one field per level,
     * `arena1`..`arenaN`), because a block in a coroutine can span a suspension -- the allocation has
     * to survive the resume, and the block's own release point still reclaims it. */
    if (g->coroFunc) {
        int lv = level > 0 ? level : 1;
        g->rcArenaUsed |= 1u << lv;
        return arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, lv);
    }
    if (level >= 1) g->rcArenaUsed |= 1u << level;   /* this arena will hold rc objects */
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/**
 * @brief Report a consistency problem between an arena level and the block currently being
 *        generated.
 *
 * This is a drift detector for the level the checker computed: it must agree with the block
 * codegen is in at this moment, and a disagreement means the two sides are computing levels
 * separately again. It only runs with the `EXTC_DBG_ARENA` environment variable set and changes
 * no output.
 *
 * The two tests are one-sided and deliberately relaxed:
 *   - the level must not be 0: 0 means "not determined yet", so seeing it means the checker
 *     missed a site;
 *   - the level must not be deeper than the current block. Promotion only moves an allocation
 *     toward a longer lifetime, so being shallower than the current block is legal.
 *
 * @param g     The generator state, supplying the current block level and path.
 * @param e     The allocation-site expression being checked.
 * @param what  The site name, used only in the diagnostic.
 */
static void arenaDriftCheck(CG *g, Expr *e, const char *what) {
    if (dbgOn("EXTC_DBG_ARENA_VERBOSE"))
        fprintf(stderr, "[arena-ok?] %s: level=%d block=%d %s:%d\n",
                what, planArenaLevel(e), g->blkLevel, g->path, e->line);
    if (planArenaLevel(e) == 0)
        fprintf(stderr, "[arena!] %s has level 0 (undecided)  %s:%d\n", what, g->path, e->line);
    else if (planArenaLevel(e) != ARENA_HOME && planArenaLevel(e) > g->blkLevel)
        fprintf(stderr, "[arena!] %s has level %d, deeper than block %d: the two answers disagree  %s:%d\n",
                what, planArenaLevel(e), g->blkLevel, g->path, e->line);
}

/**
 * @brief The scan state for "would this expression create its pool in a given place?".
 */
typedef struct {
    int  lvl;   /**< The block level whose zone is being questioned. */
    bool hit;   /**< Some call inside the expression names that place (or names none at all). */
} ZoneUse;

static bool zoneUseExpr(ZoneUse *u, Expr *e);

/** @brief Child-expression callback that keeps walking until the answer is known. */
static bool zoneUseChild(void *ctx, Expr *e) {
    return !zoneUseExpr((ZoneUse *)ctx, e);
}

/**
 * @brief Would any call inside this expression create a pool in zone @p u->lvl ?
 *
 * `planZoneLevel` is the checker's answer to "which place this call's pool is born in": a level
 * of `k >= 1` names `__extc_zm<k>`, `ZONE_HOME` names the place this function received, and 0
 * means the checker recorded nothing -- `zoneArgRef` then passes the innermost zone that is
 * actually pushed, which is never the block that would skip its own push. Only `u->lvl` asks for
 * this block's zone.
 *
 * @param u  Scan state; `u->hit` is set when the answer is yes.
 * @param e  Expression to walk; NULL leaves the answer alone.
 * @return   `u->hit` after the walk.
 */
static bool zoneUseExpr(ZoneUse *u, Expr *e) {
    if (!e) return u->hit;
    if (e->kind == EX_DYN) {
        /* A `dyn Trait(x)` construction registers the payload pool on the place the checker
         * recorded for the site (`setCallZoneArg`), and the emission passes that same zone to
         * `extc_dyn_put`. When it is this block, the block has to push it. */
        if (planZoneLevel(e) == u->lvl) u->hit = true;
    }
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        /* Only an explicit "this block" needs this block's zone. A site that recorded no level
         * resolves through `zoneArgRef`'s fallback to the innermost zone that **is** pushed -- the
         * function body or an enclosing block -- so its pool outlives us anyway.
         *
         * The level alone is not enough: every call in the block carries one (`recByte` does), and
         * counting those would keep the zone for exactly the loops this scan exists to skip. It
         * has to be a call that **creates** a pool, which is the same question `stmtMakesPool`
         * asks -- an unresolved callee stays conservative here. */
        FuncDef *f = planCallee(e);
        const char *nm = (f && f->name) ? f->name : NULL;
        /* `extc_pool_new` / `extc_pool_new_table` read the place they run in from the runtime
         * (`extc_zoneTop`), so they need whichever block the call sits in to have pushed its
         * zone. The `_at` variant carries its own place and does not. */
        if (poolCtorNeedsZone(nm)) u->hit = true;
        else if (f && calleeCreatesPool(f) && (planZoneLevel(e) == u->lvl || planZoneLevel(e) == 0))
            u->hit = true;
    }
    AstVisit v = { zoneUseChild, NULL, u };
    astWalkExprChildren(e, &v);
    return u->hit;
}

/**
 * @brief May this block create a pool **in its own zone**?
 *
 * The block-level question, and the only one `genBlockBody` asks: a block pushes
 * `extc_pool_zoneEnter()` so that pools created in it are released when it ends. But a call whose
 * pool is born elsewhere -- `ZONE_HOME`, that is, the place the caller received -- does not need
 * it: `string::push` upgrades its receiver's SSO buffer through `grow`, which asks for the
 * **home** zone, so in a loop body of `s.push(c)` the place would guard pools that never land in
 * it: one push and one pop per byte, both wasted.
 *
 * Only the statements' own expressions count: a nested block is a place of its own and decides
 * for itself when it is generated.
 *
 * @param b    Block statement; anything else answers false.
 * @param lvl  The block level (= the level of the zone the body would push).
 * @return     true when the block must be wrapped in a zone enter / leave pair.
 */
static bool blockMakesPoolAtLevel(Stmt *b, int lvl) {
    if (!b || b->kind != ST_BLOCK) return false;
    for (size_t i = 0; i < b->u.block.stmts.len; i++) {
        Stmt *st = *(Stmt **)vecAt(&b->u.block.stmts, i);
        /* The conservative "may reach extc_pool_new" answer first: only a statement that can
         * create a pool at all is worth scanning for where it would be created. */
        if (!stmtMakesPool(st, false)) continue;
        ZoneUse u = { lvl, false };
        bool hit = false;
        switch (st->kind) {
        case ST_VAR:    hit = zoneUseExpr(&u, st->u.var.init); break;
        case ST_ASSIGN: hit = zoneUseExpr(&u, st->u.assign.value) || zoneUseExpr(&u, st->u.assign.target); break;
        case ST_EXPR:   hit = zoneUseExpr(&u, st->u.expr.expr); break;
        case ST_RETURN: hit = zoneUseExpr(&u, st->u.ret.value); break;
        case ST_YIELD:  hit = zoneUseExpr(&u, st->u.yield_.value); break;
        case ST_TRAP:   hit = zoneUseExpr(&u, st->u.trap_.msg); break;
        case ST_DROP:   hit = zoneUseExpr(&u, st->u.drop_.target); break;
        case ST_IF:     hit = zoneUseExpr(&u, st->u.ifs.cond); break;
        case ST_WHILE:  hit = zoneUseExpr(&u, st->u.whiles.cond); break;
        case ST_MATCH:  hit = zoneUseExpr(&u, st->u.match.scrutinee); break;
        default:        hit = false; break;
        }
        if (hit) return true;
    }
    return false;
}

static void cgZoneLeave(CG *g, int lvl);
static void cgArenaRelease(CG *g, int lvl);

/**
 * @brief Release the block arenas down to and including one level.
 *
 * Leaving a place pops back to the depth it started at (mark) instead of popping blindly: early
 * exits (return/break/continue) therefore cannot skew the zone stack.
 *
 * Emission and popping are paired through `zoneMark`, and `zoneHere` only says "this function is
 * a place": a block's zone is pushed on demand, so popping a level that pushed none pops the
 * **outer** one -- the pool created by the outer level then has no zone, `extc_pool_new` returns
 * -1 and the record is never reclaimed. The table is written when the block is entered and read
 * here, so both sides see one fact.
 *
 * The region goes first and the arena afterwards: a pool's memory is its own malloc block and the
 * two are independent, but releasing the pools that belong to this block is exactly what this
 * block's release point is for. Only memory is closed; files are the program's own business, so
 * this single block release point has nothing to close.
 *
 * @param g    The generator state.
 * @param lvl  The innermost level to release down to; 0 or below releases nothing -- level 0 is
 *             not a block arena. A function with `noArena` (other than a coroutine step) emits
 *             nothing: it never allocates.
 */
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    if (g->noArena && !g->coroFunc) return;
    /* Memory only: files are the program's business (decision 79), so the one place a
     * block release is emitted has nothing to close. */
    /* Regions first, then the arena: a pool's memory is its own malloc block, so the two are
     * independent, but releasing the pools that belong to this block is what the block's
     * release point is for. */
    cgZoneLeave(g, lvl);
    cgArenaRelease(g, lvl);
}

/**
 * @brief Emit the zone pop of this level (only when this level pushed a zone; `zoneMark` is that
 *        table).
 *
 * Leaving a place pops back to the depth it started at (mark) instead of popping blindly: early
 * exits (return/break/continue) therefore cannot skew the zone stack.
 *
 * Emission and popping are paired through `zoneMark`, and `zoneHere` only says "this function is a
 * place": a block's zone is pushed on demand, so popping a level that pushed none pops the
 * **outer** one (the pool created by the outer level then has no zone => `extc_pool_new` returns
 * -1 => the record is never reclaimed). The table is written when the block is entered and read
 * here, so both sides see one fact and cannot drift apart.
 *
 * @param g    The generator state.
 * @param lvl  The block level; out of range or a level that pushed no zone emits nothing.
 */
static void cgZoneLeave(CG *g, int lvl) {
    if (lvl <= 0 || lvl >= g->levelCap || !g->zoneMark[lvl]) return;
    cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lvl);
}

/**
 * @brief Emit the release of this level's arena.
 *
 * On an empty arena it is nearly free, but **it is a call**: inside an inner loop body that alone
 * is enough to block vectorization. The call site therefore first asks whether this block touched
 * this level (`arenaLevelInText`) and emits nothing when it did not -- the same holds for clearing
 * at entry.
 *
 * A coroutine's block levels live in the frame (`<frame>->arenaN`): a coroutine body does not own
 * its place, so it must not release the **caller's** arena levels -- that would release, on every
 * resume, what the next resume still needs. Its block arenas die with the frame, and this block's
 * release point reclaims its own level as usual.
 *
 * **A block's release does not drop the reclamation region**: a region is claimed by the arena's
 * address, and sibling blocks at one level share a single arena while a loop body reuses it every
 * iteration -- dropping the region here would make the next iteration's `regionOf` create a new
 * **1 MiB slab**, paying allocation and zeroing on every iteration, and a long loop would stop
 * making progress. The region lives until the frame is destroyed (only `extc_arena_destroy` calls
 * `regionDrop`): every object in the block is unreachable, sweeping returns them to the free list
 * by pressure and the slots are reused across iterations -- memory stays bounded with no
 * per-iteration cost.
 *
 * @param g    The generator state.
 * @param lvl  The block level; 0 or below emits nothing -- level 0 is not a block arena. A
 *             function with `noArena` (other than a coroutine step) emits nothing: it never
 *             allocates.
 */
static void cgArenaRelease(CG *g, int lvl) {
    if (lvl <= 0) return;
    /* A step keeps `noArena` (its prologue has no `__extc_a` and its returns take the direct path),
     * but it still releases block levels -- out of the frame's per-level arenas. */
    if (g->noArena && !g->coroFunc) return;
    if (g->coroFunc)
        cgLine(g, "extc_arena_release(&%s->arena%d);", g->coroFrame, lvl);
    else
        cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/**
 * @brief Destroy the block-level arenas of a coroutine's frame where it finishes.
 *
 * A step function does `return true` on every `yield` and never takes the shared epilogue; the
 * coroutine body's own level lives until the task ends rather than until the step returns, so the
 * levels a finished coroutine still owns must be released here, and before the driver's
 * `extc_task_end` releases the frame itself. A coroutine discarded halfway and never finished does
 * not reach this, so the blocks its frame arenas still hold are not released here; the leak is
 * bounded by a delay bound.
 *
 * @param g  The generator state; nothing is emitted when `g->coroFunc` is null (not inside a
 *           coroutine body).
 */
static void cgCoroArenaDestroy(CG *g) {
    if (!g->coroFunc) return;
    int n = 1 + blkMaxOfBlock(((FuncDef *)g->coroFunc)->body);
    for (int lv = 1; lv <= n; lv++) {
        if (g->needReclaim)
            cgLine(g, "extc_rc_regionDrop((void *)&%s->arena%d);", g->coroFrame, lv);
        cgLine(g, "extc_arena_destroy(&%s->arena%d);", g->coroFrame, lv);
    }
}

/* Collect every `@overwrite` site of one function body, in source order.
 *
 * The order is what makes the numbering stable: the prologue emits one storage
 * cell per site in this same order, and owIndex looks a site up by position.
 */
/* Defined below; `collectOwSites` walks a domain block's receiver with it. */
static void collectOwCallsExpr(Expr *e, Vec *out);

/**
 * @brief Collect every `@overwrite` site in a function body in source order.
 *
 * **Order** is what makes the numbering stable: the prologue emits one storage cell per site in
 * the same order and `owIndex` looks the site up by position. A `d.run { ... }` domain block's
 * receiver is an expression and is collected by `collectOwCallsExpr`.
 *
 * @param s    The statement to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Stmt *` in order.
 */
static void collectOwSites(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.overwrite) *(Stmt **)vecPush(out) = s;
        return;
    /* `d.run { ... }`: the receiver is an expression and the block is a body -- collect both, the
     * same shape as `ST_IF`. */
    case ST_DOMAIN: collectOwCallsExpr(s->u.domain_.callee, out); collectOwSites(s->u.domain_.body, out); return;    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwSites(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_IF:
        collectOwSites(s->u.ifs.thenBody, out);
        collectOwSites(s->u.ifs.elseBody, out);
        return;
    case ST_WHILE: collectOwSites(s->u.whiles.body, out); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwSites((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;
    }
}

/**
 * @brief Return a site's index within the current function.
 *
 * The index is **looked up**, not stored in the AST: one generic body is walked again for every
 * instance, so a stored index would be overwritten by the next instance. The prologue emits cells
 * in the order of `g->owSitesOwn`, so both sides agree.
 *
 * @param g  The generator state; looked up in `g->owSitesOwn`.
 * @param s  The site statement.
 * @return   Its position in `g->owSitesOwn`; -1 when it is not registered.
 */
static int owIndex(CG *g, Stmt *s) {
    for (size_t i = 0; i < g->owSitesOwn.len; i++)
        if (*(Stmt **)vecAt(&g->owSitesOwn, i) == s) return (int)i;
    return -1;
}

/* Collect the call sites whose callee needs @overwrite cells.
 *
 * Every expression kind has to be covered. A missed one leaves that call site
 * without cells, so the callee receives NULL; it has a fallback for that, so the
 * program stays defined, but the storage is then allocated afresh on every call
 * instead of being reused.
 */
static void collectOwCallsExpr(Expr *e, Vec *out);
static void collectOwCallsStmt(Stmt *s, Vec *out);

/**
 * @brief Collect, from one expression, the call sites whose callee needs an `@overwrite` cell.
 *
 * **Every expression kind must be covered**: a missed kind leaves that call site with no cell and
 * the callee receives NULL -- it has a fallback, the program stays defined, but the storage is
 * reallocated on every call instead of reused. The payload of `dyn Trait(x)` is a subexpression
 * and must be entered; each branch ends with `default:`, so a missed kind does not trigger
 * `-Wswitch`.
 *
 * @param e    The expression to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Expr *` in order.
 */
static void collectOwCallsExpr(Expr *e, Vec *out) {
    if (!e) return;
    switch (e->kind) {

    case EX_DYN:
        collectOwCallsExpr(e->u.dynv.payload, out);
        return;
    case EX_CALL:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.call.args, i), out);
        return;
    case EX_METHOD:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        collectOwCallsExpr(e->u.method.recv, out);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.method.args, i), out);
        return;
    case EX_ASSOC:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.assoc.args, i), out);
        return;
    case EX_BIN:      collectOwCallsExpr(e->u.bin.left, out); collectOwCallsExpr(e->u.bin.right, out); return;
    case EX_UN:       collectOwCallsExpr(e->u.un.operand, out); return;
    case EX_FIELD:    collectOwCallsExpr(e->u.field.obj, out); return;
    case EX_INDEX:    collectOwCallsExpr(e->u.index.obj, out); collectOwCallsExpr(e->u.index.index, out); return;
    case EX_SLICE:    collectOwCallsExpr(e->u.slice.obj, out);
                      collectOwCallsExpr(e->u.slice.lo, out);
                      collectOwCallsExpr(e->u.slice.hi, out);
                      collectOwCallsExpr(e->u.slice.lo, out); collectOwCallsExpr(e->u.slice.hi, out); return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            collectOwCallsExpr((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, out);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            collectOwCallsExpr((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value, out);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.arraylit.elems, i), out);
        return;
    case EX_REF:      collectOwCallsExpr(e->u.ref.operand, out); return;
    case EX_EXT: collectOwCallsExpr(e->u.ext_.call, out); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:    collectOwCallsExpr(e->u.deref.operand, out); return;
    case EX_SIGN:     collectOwCallsExpr(e->u.sign.operand, out); return;
    case EX_CONV:     collectOwCallsExpr(e->u.conv.operand, out); return;
    case EX_TRY:      collectOwCallsExpr(e->u.try_.operand, out); return;
    case EX_NEW:      collectOwCallsExpr(e->u.new_.count, out); return;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.gencall.args, i), out);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.enumval.args, i), out);
        return;
    case EX_COALESCE: collectOwCallsExpr(e->u.coalesce.main, out);
                      collectOwCallsExpr(e->u.coalesce.fallback, out); return;
    default: return;      /* literals, bindings, null */
    }
}

/**
 * @brief Collect, from one statement tree, the call sites whose callee needs a cell.
 *
 * Expressions (including a `d.run { ... }` receiver, a condition and a matched expression) go to
 * `collectOwCallsExpr` and statement bodies keep recursing; which expression kinds must be covered
 * is `collectOwCallsExpr`'s responsibility.
 *
 * @param s    The statement to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Expr *` in order.
 */
static void collectOwCallsStmt(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_YIELD:  collectOwCallsExpr(s->u.yield_.value, out); return;
    case ST_TRAP:   collectOwCallsExpr(s->u.trap_.msg, out); return;
    case ST_DROP:   collectOwCallsExpr(s->u.drop_.target, out); return;
    case ST_VAR:    collectOwCallsExpr(s->u.var.init, out); return;
    case ST_ASSIGN: collectOwCallsExpr(s->u.assign.target, out); collectOwCallsExpr(s->u.assign.value, out); return;
    case ST_EXPR:   collectOwCallsExpr(s->u.expr.expr, out); return;
    case ST_RETURN: collectOwCallsExpr(s->u.ret.value, out); return;
    case ST_DOMAIN: collectOwCallsExpr(s->u.domain_.callee, out);
                    collectOwCallsStmt(s->u.domain_.body, out); return;
    case ST_IF:     collectOwCallsExpr(s->u.ifs.cond, out);
                    collectOwCallsStmt(s->u.ifs.thenBody, out); collectOwCallsStmt(s->u.ifs.elseBody, out); return;
    case ST_WHILE:  collectOwCallsExpr(s->u.whiles.cond, out); collectOwCallsStmt(s->u.whiles.body, out); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwCallsStmt(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_MATCH:
        collectOwCallsExpr(s->u.match.scrutinee, out);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwCallsStmt((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;      /* break, continue */
    }
}

/**
 * @brief Return a call site's index within the current function.
 *
 * The prologue emits cells in the order of `g->owCalls`, so both sides agree; a site that is not
 * registered yields -1 and the call site passes no cell, leaving the callee on its fallback.
 *
 * @param g  The generator state; looked up in `g->owCalls`.
 * @param e  The call-site expression.
 * @return   Its position in `g->owCalls`; -1 when it is not registered.
 */
static int owCallIndex(CG *g, Expr *e) {
    for (size_t i = 0; i < g->owCalls.len; i++)
        if (*(Expr **)vecAt(&g->owCalls, i) == e) return (int)i;
    return -1;
}

/**
 * @brief Decide whether the current function's `@overwrite` cell stays in its own stack frame.
 *
 * True for `main` and for recursive functions: their frames live on, and the answer is recorded
 * in FuncDef.owLocal.
 *
 * @param g  The generator state; reads `g->owLocalOwn`.
 * @param s  Unused; the answer belongs to the whole function and does not depend on a statement.
 * @return   true when the cell is in this frame, false otherwise.
 */
static bool f_owLocal(CG *g, Stmt *s) { (void)s; return g->owLocalOwn; }

/**
 * @brief Append one call site's `@overwrite` cell argument at the end of the argument list.
 *
 * Nothing is appended when the site is not registered: the callee has a fallback for that and the
 * program stays defined.
 *
 * @param g        The generator state, for the site-index lookup.
 * @param b        The argument-list buffer being assembled, without the closing parenthesis.
 * @param e        The call-site expression.
 * @param nargs    The number of arguments already in the list, deciding whether to prepend a
 *                 comma.
 * @param hasHome  Whether a home arena argument was already appended; it also affects the comma.
 */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome) {
    if (!planCallee(e) || planOwSites(planCallee(e)) == 0 || planOwLocal(planCallee(e))) return;
    int j = owCallIndex(g, e);
    if (j < 0) return;                       /* not registered; the callee has a fallback */
    for (int k = 0; k < planOwSites(planCallee(e)); k++)
        bufPrintf(b, "%s&__extc_owc%d_%d", (nargs || hasHome || k) ? ", " : "", j, k);
}

/* How many block levels one function can use at the same time.
 *
 * The answer sizes the `__extc_a` array, which is a fixed-length array on the
 * stack, so setting up the block arenas needs no allocation.
 */
int blkMaxLevel(Stmt *s);

/**
 * @brief Return the deepest block level inside a block.
 *
 * @param block  The block statement; when it is `NULL` or not `ST_BLOCK` the result equals
 *               `blkMaxLevel(block)`, that is, 0.
 * @return       The deepest level reached inside the block, not counting the level the block adds
 *               itself; 0 when there is no block.
 */
int blkMaxOfBlock(Stmt *block) {
    int m = 0;
    if (!block || block->kind != ST_BLOCK) return blkMaxLevel(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        /* One call per statement, into a local: calling `blkMaxLevel` twice (once to compare,
         * once to assign) doubles the work at **every** level, so n nested blocks cost 2^n. */
        int d = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));
        if (d > m) m = d;
    }
    return m;
}
/**
 * @brief Return the deepest block level reached inside one statement.
 *
 * A block counts as one level, and the loops, conditionals and `match` inside it add their own, so
 * the result is the number of arena slots the enclosing function needs (it decides the length of
 * `__extc_a`).
 *
 * @param s  The statement; 0 when it is NULL.
 * @return   `1 + blkMaxOfBlock(s)` for a block; the maximum of the branches for `if`/`match`; a
 *           `while` whose condition allocates space one level more than its body block.
 */
int blkMaxLevel(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_BLOCK: return 1 + blkMaxOfBlock(s);
    case ST_IF: {
        int a = 1 + blkMaxOfBlock(s->u.ifs.thenBody);
        int b = s->u.ifs.elseBody
                  ? 1 + (s->u.ifs.elseBody->kind == ST_BLOCK
                           ? blkMaxOfBlock(s->u.ifs.elseBody)
                           : blkMaxLevel(s->u.ifs.elseBody))
                  : 0;
        return a > b ? a : b;
    }
    case ST_WHILE:
        /* A condition that allocates gets a level of its own (see the emitter), so that shape
         * needs one more than the body's own block. */
        return (planCondAllocs(s) ? 2 : 1) + blkMaxOfBlock(s->u.whiles.body);
    /* `d.run { ... }` opens a block and `genStmtInner` emits its body through `genBlockBody`,
     * which takes one arena level; the count has to include that level, or `__extc_a[maxLv + 1]`
     * is too small for the `&__extc_a[k+1]` the body uses. */
    case ST_DOMAIN: return 1 + blkMaxOfBlock(s->u.domain_.body);
    case ST_MATCH: {
        int m = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            int d = 1 + blkMaxOfBlock(a->body);
            if (d > m) m = d;
        }
        return m;
    }
    default: return 0;
    }
}

/**
 * @brief Whether the generated block-body text mentions some arena level (`__extc_a[<lvl>]`).
 *
 * Clearing at block entry and releasing at block exit both reclaim only what this block itself
 * allocated in **this level**. Every allocation in the text
 * (`extc_arena_alloc(&__extc_a[k], ...)`), every hand-down of this level as a callee's home
 * (`&__extc_a[k]`) and every region pointer base carries this text; so "not mentioned" is "this
 * block never touched this level" and both can be skipped. Recognizing one too many (say the
 * string inside a literal) only emits too much, which is the safe direction; recognizing too few is
 * impossible -- every emission point that uses this level draws its spelling from `arenaRefAt`.
 *
 * @param b    The block-body text.
 * @param lvl  The block level.
 * @return     true when this level appears in the text.
 */
static bool arenaLevelInText(const Buf *b, int lvl) {
    char needle[32];
    int n = snprintf(needle, sizeof needle, "__extc_a[%d]", lvl);
    if (n <= 0 || (size_t)n >= sizeof needle) return true;   /* when the text cannot be assembled, treat the level as touched */
    size_t nl = (size_t)n;
    if (!b->data || b->len < nl) return false;
    for (size_t i = 0; i + nl <= b->len; i++)
        if (b->data[i] == '_' && memcmp(b->data + i, needle, nl) == 0) return true;
    return false;
}


/**
 * @brief Run the `@drop` hooks of a block's own locals, in reverse declaration order.
 *
 * Emitted at the tail of the block and again before a `return`, so both the fall-through and the
 * early-exit path release once. `break`/`continue` are not covered yet: they leak rather than run a
 * hook twice.
 *
 * @param g      The generator state.
 * @param block  Block whose locals are leaving scope.
 */
static void genBlockTailDrops(CG *g, Stmt *block);

/**
 * @brief Run the `@drop` hooks of every local still in scope, before a `return`.
 *
 * Innermost block first, so a local drops before the locals of the blocks enclosing it. Emitted
 * into the current output buffer; a `return` inside a nested statement is part of that statement's
 * text, so this is the same place the return itself goes.
 *
 * @param g  The generator state.
 */
static void genReturnDrops(CG *g) {
    for (size_t i = g->dropBlocks.len; i-- > 0;)
        genBlockTailDrops(g, *(Stmt **)vecAt(&g->dropBlocks, i));
}

/** @brief Run the hooks of the blocks above `base` (innermost first); used by `break`/`continue`. */
static void genDropBlocksDownTo(CG *g, size_t base) {
    for (size_t i = g->dropBlocks.len; i-- > base;)
        genBlockTailDrops(g, *(Stmt **)vecAt(&g->dropBlocks, i));
}

/**
 * @brief Does dropping a value of this type do anything?
 *
 * True when the type has its own `@drop` hook, or when one of its fields does. A field of a `@drop`
 * type dies with the value that contains it -- the same rule as C++ members and Rust's `Drop` on
 * fields -- so the walk is recursive. References and views are not followed: they do not own the
 * value they point at, and following them would drop something that is still alive.
 *
 * @param g      The generator state (for the instantiation context).
 * @param t      The type; may be NULL.
 * @param depth  Nesting already walked, capped so a pathological type cannot loop.
 * @return       True when a drop has an effect.
 */
static bool typeNeedsDrop(CG *g, Type *t, int depth) {
    if (!t || depth > 8) return false;
    Type *st = subst(g, t);
    /* A reference or a view names someone else's storage, so it owns nothing to hand back. Without
     * this test `ttBase` unwraps it and the field is dropped as if it were the pointee, which makes a
     * guard struct holding `mut ref buf<T>` emit a call with the wrong pointer type. */
    if (st && (st->kind == TY_REF || ttIsViewType(st))) return false;
    Type *b = ttBase(st);
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd) return false;
    if (sd->dropHook) return true;
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (fd && typeNeedsDrop(g, fd->type, depth + 1)) return true;
    }
    return false;
}

/**
 * @brief Emit the drop of one value: its own hook first, then its fields in declaration order.
 *
 * The hook runs before the fields so it can still read them (a wrapper delegating to a field keeps
 * working). Fields are visited in declaration order, which is what makes a value's death the mirror
 * image of its construction.
 *
 * @param g      The generator state.
 * @param path   C expression naming the value, e.g. `a` or `a.inner`.
 * @param t      The value's type.
 * @param depth  Nesting already walked, capped like `typeNeedsDrop`.
 */
static void genDropValue(CG *g, const char *path, Type *t, int depth) {
    if (!t || depth > 8) return;
    Type *b = ttBase(subst(g, t));
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd) return;
    if (sd->dropHook) cgLine(g, "%s(&(%s));", cMethodName(g, b, sd->dropHook), path);
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (!fd || !typeNeedsDrop(g, fd->type, depth + 1)) continue;
        const char *sub = arenaPrintf(g->arena, "%s.%s", path, fd->name);
        genDropValue(g, sub, fd->type, depth + 1);
    }
}

/**
 * @brief Emit the drop of every by-value parameter that owns storage, at the function's exit.
 *
 * A parameter passed by value is the callee's copy: for a type with `@drop` (or with fields that
 * have one) the callee owns it, so leaving the function has to hand the storage back -- the same
 * rule that makes a local's death the mirror image of its construction. A `ref` or a view parameter
 * names the caller's storage and drops nothing.
 *
 * The drops run right after the shared epilogue label and **before** the block arenas are destroyed:
 * a hook may still read through the value (its storage is a plate, not an arena, but the reads go
 * through the same struct).
 *
 * @param g  The generator state.
 * @param f  The function being emitted.
 */
static void genParamDrops(CG *g, FuncDef *f) {
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p || !p->type || !p->cname) continue;
        Type *pt = subst(g, p->type);
        if (!pt || pt->kind == TY_REF || ttIsViewType(pt)) continue;
        if (!typeNeedsDrop(g, pt, 0)) continue;
        genDropValue(g, p->cname, pt, 0);
    }
}

/** @brief Emit the hooks for one block's locals; see the declaration for the timing rules. */
static void genBlockTailDrops(CG *g, Stmt *block) {
    for (size_t i = block->u.block.stmts.len; i-- > 0;) {
        Stmt *st = *(Stmt **)vecAt(&block->u.block.stmts, i);
        if (!st || st->kind != ST_VAR || !st->u.var.init) continue;
        if (!typeNeedsDrop(g, st->type, 0)) continue;
        const char *nm = planCName(st) ? planCName(st) : st->u.var.name;
        if (!nm) continue;
        /* `return x` moves `x` out; compare both spellings, because inside a generic instance the
         * generated name carries the instance suffix while the source name does not. */
        if (g->dropSkipName &&
            (strcmp(nm, g->dropSkipName) == 0 ||
             (st->u.var.name && strcmp(st->u.var.name, g->dropSkipName) == 0)))
            continue;
        /* An explicit `drop x` already ran the hook for this name in this block. */
        bool alreadyDropped = false;
        for (size_t k = 0; k < g->dropExplicit.len; k++) {
            const char *n = *(const char **)vecAt(&g->dropExplicit, k);
            if (n && (strcmp(n, nm) == 0 || (st->u.var.name && strcmp(n, st->u.var.name) == 0))) {
                alreadyDropped = true;
                break;
            }
        }
        if (alreadyDropped) continue;
        genDropValue(g, nm, st->type, 0);
    }
}

/**
 * @brief Emit one block body: clear its arena level on entry and release it on exit.
 *
 * A loop body is a block too, so each iteration clears once -- the memory a loop uses is therefore
 * bounded by one iteration, not by the iteration count.
 *
 * Both only reclaim what this block allocated in this level. The body is emitted into a temporary
 * buffer first and the decision is made after a scan: when this level was not touched both are
 * skipped. Releasing an empty arena is nearly free, but **it is a call** and inside an inner loop
 * body that alone blocks vectorization -- and a loop body that allocated nothing is exactly the
 * one to vectorize. The emission order is unchanged: clear -> zoneEnter -> body -> zoneLeave ->
 * release.
 *
 * @param g      The generator state.
 * @param block  The block statement, whose statements are emitted in order; the caller guarantees
 *               it is `ST_BLOCK` (asserted).
 */
static void genBlockBody(CG *g, Stmt *block) {
    /* Only functions with a body reach here (`genFunc` returns early for an
     * external declaration, which has none), and every other caller passes a
     * block statement it just inspected. */
    assert(block != NULL && block->kind == ST_BLOCK);
    g->blkLevel++;
    /* `genFunc` sizes `zoneMark`, `loopLevel` and `loopStep` by `levelCap`; state the bound
     * here rather than walk off the array. */
    assert(g->blkLevel < g->levelCap);
    /* Entering a block overwrites this level's mark, so the one that was there is saved
     * and put back on the way out. Level 1 is the function body, whose mark the prologue
     * set (it pushed `__extc_zm1`, and the epilogue reads the mark to decide whether to
     * pop it): a plain `= false` here would erase that decision. Two sibling blocks at the
     * same level both write it, and nothing looks at a level after its block has been
     * left, but the save costs a byte and removes the question. */
    bool savedZoneMark = g->zoneMark[g->blkLevel];
    /* Names dropped explicitly inside this block: the record is cut back when the block ends, so a
     * sibling block may reuse the name and still get its own hook. */
    size_t savedDropExplicit = g->dropExplicit.len;
    /* Block boundary of the static root table: the current registration depth is recorded on entry
     * and truncated on exit -- a local in the block stops being a root once the block ends (which
     * slots are live at a trigger point is something the compiler supplies). */
    *(size_t *)vecPush(&g->rcRootMarks) = g->rcRoots.len;
    *(Stmt **)vecPush(&g->dropBlocks) = block;
    /* A coroutine's block level lives in the frame and its early-exit path winds differently; a
     * `noArena` function skips both anyway. Both cases are emitted as they are, and only "one block
     * arena level in an ordinary function" is worth asking about first. */
    const bool probe = !g->coroFunc && !g->noArena;
    Buf  scratch;
    Buf *savedOut = g->out;
    bool bodyTouches = true;
    bool zoneHere = false;
    if (!g->noArena) {
        /* Entering a place: push a zone, and the mark is named by level number (blocks at one level do
         * not nest, so the names cannot collide). It is only pushed when this block **may create a
         * pool in this place** (`blockMakesPoolAtLevel`): a loop body whose pools are born in the
         * caller's place (the SSO upgrade in `string::push` is one) needs no zone of its own,
         * saving two calls per iteration. Whether a
         * pop is emitted is recorded in `zoneMark`, which `cgReleaseLevel` reads from the same
         * table. */
        g->zoneMark[g->blkLevel] = false;
        if (g->zoneHere && g->blkLevel > 1 && blockMakesPoolAtLevel(block, g->blkLevel)) {
            g->zoneMark[g->blkLevel] = true;
            zoneHere = true;
        }
    }
    if (probe) {   /* The body is emitted into a temporary buffer first and moved back to the output after the
     * scan: all three enter/exit lines sit outside the body. `g->out` points at the **local**
     * `scratch` only inside this window and is restored to `savedOut` before the function returns,
     * so the pointer never outlives the object it names. */
        bufInit(&scratch, g->arena);
        g->out = &scratch;
    }
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt *st = *(Stmt **)vecAt(&block->u.block.stmts, i);
        /* The `for` step: `continue` jumps here, so the label has to be immediately in front of
         * it. Emitted from the same walk that emits the statement, which is what keeps the label
         * and the `goto` in agreement (the `goto` is only emitted when ST_WHILE found this exact
         * statement in the block). */
        if (planForStep(block) && st == planForStep(block) && g->loopLen > 0 && g->loopStep[g->loopLen - 1]
            && g->loopStepUsed[g->loopLen - 1])
            cgLine(g, "__extc_forstep_%d: ;", g->loopStep[g->loopLen - 1]);
        genStmt(g, st);
    }
    /* `@drop` runs while the block's own storage is still alive. */
    genBlockTailDrops(g, block);
    if (probe) {
        g->out = savedOut;
        bodyTouches = arenaLevelInText(&scratch, g->blkLevel);
        /* Entering a block only clears the arena (its block is reused for the next iteration); the
         * reclamation region is not dropped here, for the reason given in `cgReleaseLevel`: dropping
         * it every iteration would mean creating a new 1 MiB slab every iteration. */
        if (bodyTouches) cgArenaRelease(g, g->blkLevel);   /* clear */
        if (zoneHere) cgLine(g, "int64_t __extc_zm%d = extc_pool_zoneEnter();", g->blkLevel);
        bufPutn(g->out, scratch.data, scratch.len);        /* the body itself */
        if (zoneHere) cgZoneLeave(g, g->blkLevel);
        if (bodyTouches) cgArenaRelease(g, g->blkLevel);
    } else {
        if (!g->noArena && !g->coroFunc)   /* a coroutine's storage is the task's, not this block's */
            cgArenaRelease(g, g->blkLevel);   /* clear */
        if (zoneHere && !g->coroFunc)      /* see the note on the release below: the task owns the place */
            cgLine(g, "int64_t __extc_zm%d = extc_pool_zoneEnter();", g->blkLevel);
        cgReleaseLevel(g, g->blkLevel);
    }
    g->zoneMark[g->blkLevel] = savedZoneMark;
    /* Leaving the block: the reference slots declared in it stop being roots. */
    assert(g->rcRootMarks.len > 0);
    g->rcRoots.len = *(size_t *)vecAt(&g->rcRootMarks, g->rcRootMarks.len - 1);
    g->rcRootMarks.len--;
    g->dropBlocks.len--;
    g->dropExplicit.len = savedDropExplicit;
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/**
 * @brief Emit one statement, with an optional statement prefix before and after it.
 *
 * What the statement needs evaluated early is written into the prefix; a non-zero `prefixBlk`
 * means a statement is being generated, and the prefix mechanism then permits emitting there. The
 * statement itself is emitted by `genStmtInner`.
 *
 * @param g  The generator state.
 * @param s  The statement to emit.
 */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

/**
 * @brief Is this slot a reference type (`ref T` / `?ref T`)?
 *
 * This is the only counting test: counting hangs **only on a change of pointed-to relation**. An
 * assignment to a data field (`p.val = x`) never touches a count, so the type is asked before
 * emission is discussed. A `T` in an instantiation context is substituted first, or a generic
 * instance would compare a parameter type against `TY_REF`.
 *
 * @param g  The generator state, supplying the instantiation context.
 * @param t  The slot's declared type; may be NULL.
 * @return   true when it is a reference type.
 */
static bool cgIsRefType(CG *g, Type *t) {
    Type *s = subst(g, t);
    return s && s->kind == TY_REF;
}

/**
 * @brief A **pure place** expression: safe to evaluate repeatedly (no call, no subscript
 *        evaluation).
 *
 * Cross-region registration has to evaluate "the object holding this slot" once more at the same
 * place (`a->f = v` passes an extra `a`), so only bindings, field chains and dereferences count:
 * evaluating them has no side effect and re-reads the same value. A subscript projection (`a[i]`)
 * would have to evaluate the index again to be safe, which costs more than it saves, so it is not
 * registered -- those positions are covered by the trigger point's root table.
 *
 * @param g  The generator state.
 * @param e  The candidate expression.
 * @return   true when it is a pure place.
 */
static bool cgPurePlace(CG *g, Expr *e) {
    (void)g;
    for (; e; ) {
        if (e->kind == EX_IDENT) return true;
        if (e->kind == EX_FIELD) { e = e->u.field.obj; continue; }
        if (e->kind == EX_DEREF) { e = e->u.deref.operand; continue; }
        return false;
    }
    return false;
}

/* The host of one reference slot: the **run-time object** that holds it. */
typedef enum {
    RC_HOST_OBJ,      /* the host object expression was found (it still has to pass cgPurePlace to be evaluated
     * repeatedly) */
    RC_HOST_LOCAL,    /* the slot lives in statically declared storage (local/global/parameter): the trigger point's
     * root table already covers it */
    RC_HOST_UNKNOWN   /* the host cannot be traced (view element, call result, ...): the conservative path is the only
     * option */
} RcHost;

/* ---- local objects whose host region can be given statically (see CG.rcFresh*) --------------- */

/** @brief Statement callback of the assignment scan: note the identifiers that are rebound and
 *         keep walking. */
static bool rcAsgnStmt(void *ctx, Stmt *s);
/** @brief Expression callback of the assignment scan: keep walking. */
static bool rcAsgnExpr(void *ctx, Expr *e);

/**
 * @brief Statement callback: note the identifiers that are **directly** assigned (rebound) and
 *        keep walking.
 *
 * @param ctx  The generator state.
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcAsgnStmt(void *ctx, Stmt *s) {
    CG *g = (CG *)ctx;
    /* The statement kind is tested first: in the union, the pointers of other statement kinds would
     * be misread as assignment targets. */
    Expr *tgt = (s && s->kind == ST_ASSIGN) ? s->u.assign.target : NULL;
    /* Only a **direct** assignment to an identifier counts as a rebinding: `t.f = v`, `a[i] = v`
     * and `*p = v` change a slot inside the object or something elsewhere, not which object this
     * local points to. */
    if (tgt && tgt->kind == EX_IDENT) {
        const char *nm = planCName(tgt) ? planCName(tgt) : tgt->u.ident.name;
        if (nm) *(const char **)vecPush(&g->rcAssigned) = nm;
    }
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/** @brief Expression callback: keep walking (an assignment target can also be nested inside
 *         an expression statement). */
static bool rcAsgnExpr(void *ctx, Expr *e) {
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Scan the function body once: which identifiers are assigned (the complement of "is never
 *        rebound").
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcScanAssigned(CG *g, Stmt *body) {
    vecInit(&g->rcAssigned, g->arena, sizeof(const char *));
    if (!body) return;
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, g };
    astWalkStmtChildren(body, &v);
}

/** @brief Was this name assigned? */
static bool rcIsAssigned(CG *g, const char *nm) {
    for (size_t i = 0; i < g->rcAssigned.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcAssigned, i), nm) == 0) return true;
    return false;
}

/**
 * @brief Register a "freshly allocated and never rebound" local object (`stVar` calls this while
 *        emitting the declaration).
 *
 * The conditions are: the initializer is a **single** `EX_NEW`, reclamation is on (such an
 * allocation always goes through rc), the level is the home or a block level, and the name is
 * never assigned. A coroutine body registers nothing (region locals are not hoisted there).
 *
 * @param g  The generator state.
 * @param s  The `ST_VAR` statement.
 */
static void rcNoteFreshLocal(CG *g, Stmt *s) {
    /* When sharding, **do not emit**: the region key is "base + shard number" and objects at one
     * level land on different shards by allocation point -- the statically given region need not be
     * the one holding the host (`cross_region_elem` peaks at 2.77x with SHARDS=8, so the
     * registration missed it). A coroutine body is the same (region locals are not hoisted). */
    if (!g->needReclaim || g->coroFunc || g->rcNShard > 1) return;
    if (g->rcFreshN >= 64) return;
    Expr *init = s->u.var.init;
    if (!init || init->kind != EX_NEW || init->u.new_.count) return;
    const char *nm = planCName(s);
    if (!nm || rcIsAssigned(g, nm)) return;
    int lvl = planArenaLevel(init);
    if (!(lvl == ARENA_HOME || lvl >= 1)) return;
    g->rcFreshName[g->rcFreshN] = nm;
    g->rcFreshLvl[g->rcFreshN] = lvl;
    g->rcFreshN++;
}

/**
 * @brief When the host expression is such a local object, return its region local's name,
 *        otherwise NULL.
 *
 * The region local was declared at the top of the function body by `rcPredeclare` (which collects
 * it from every `EX_NEW` point); here it is looked up by a **byte-identical** base-address text.
 *
 * @param g     The generator state.
 * @param base  The host object's C expression node.
 * @return      The region local's name, or NULL (fall back to `storeRefOwned`).
 */
static const char *rcFreshHostRegion(CG *g, Expr *base) {
    if (!base || base->kind != EX_IDENT) return NULL;
    const char *nm = planCName(base) ? planCName(base) : base->u.ident.name;
    if (!nm) return NULL;
    for (int i = 0; i < g->rcFreshN; i++) {
        if (strcmp(g->rcFreshName[i], nm) != 0) continue;
        const char *bt = arenaPrintf(g->arena, "(void *)&(%s)", arenaRefAt(g, g->rcFreshLvl[i]));
        const char *rr = rcRegionRef(g, bt);
        return rr;
    }
    return NULL;
}

/** @brief Which cell this C name occupies in this frame's root table; -1 when it is not in the
 *         table (or only in the global root table). */
static int rcRootIndexOf(CG *g, const char *nm) {
    if (!nm) return -1;
    for (size_t i = 0; i < g->rcRoots.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcRoots, i), nm) == 0) return (int)i;
    return -1;
}

/** @brief Has this identifier been written into the reference field of some heap object (see
 *         `g->rcLinked`)? */
static bool rcLinkedHas(CG *g, const char *nm) {
    if (!nm) return false;
    for (size_t i = 0; i < g->rcLinked.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcLinked, i), nm) == 0) return true;
    return false;
}

/**
 * @brief Overwriting a reference slot: is it worth a **point fix** release
 *        (`extc_rc_storeRefOwnedRelease`)?
 *
 * This is the shape "build one tree per round, the next round overwrites it through the same
 * long-lived slot" (`ring_per_round`: the ring head `head` is stored into `keep`, and the next
 * round overwrites `keep`). All four criteria must hold.
 *
 *   1. This assignment is inside some loop body (outside a loop there is no "drop a tree per
 *      round").
 *   2. The target slot is a reference slot of this frame (in `rcRoots`) and is declared outside
 *      **all** of the loops in the registry -- it is overwritten at the same slot every round. A
 *      cursor inside a loop (`cur = t`) fails 2 and is excluded.
 *   3. The value written is a local object this frame **freshly allocated and never rebound**
 *      (`rcFreshName`), and the region it lives in is **owned by this frame**: only there is this
 *      frame's root table a complete seed (the same criterion as the trigger at the allocation
 *      point).
 *   4. This object has already been written into the reference field of some heap object
 *      (`(*cur).next = head`) -- only a fresh local chained into a heap object can be the head of
 *      a tree or a ring.
 *
 * **The static criteria only decide whether it is worth it**; the real work happens at run time
 * -- marking starts from **the other roots plus the cross-region seeds** and frees only the slots
 * reachable **from the old value alone**. Being wrong on every criterion cannot dangle (see the
 * note on `extc_rc_releaseOld`).
 *
 * When it cannot be emitted (unreliable root table, sharded, no static region, target not in the
 * root table, ...) it returns false and **falls back to `storeRefOwned` plus the region sweep**;
 * with reclamation off (`EXTC_RECLAIM=0`) the output is byte-identical.
 *
 * @param g        The generator state.
 * @param tgt      The target slot's C lvalue text.
 * @param tgtName  The target slot's C name (the root table is looked up by name).
 * @param val      The C text of the value being written.
 * @param value    The expression of the value being written.
 * @return         true when a point-fix release was emitted.
 */
static bool rcEmitStoreRelease(CG *g, const char *tgt, const char *tgtName,
                               const char *val, Expr *value) {
    /* In the event tier (`EXTC_RC_SWEEPFREE`) the sweep never triggers, so this mechanism has
     * nothing to do: its value is "a ring does not wait for a sweep", and the event tier is only
     * used for units proved **acyclic**, where every unreachable object is released on the spot
     * when its count reaches zero. This mechanism also **ignores the count** (`releaseOld` marks
     * the whole live set from the other roots), so keeping it in the event tier costs without
     * paying: shapes such as tail insertion would pay a full marking on every assignment. The
     * criterion hangs on the event tier's memory gate (`gate-event-bounded`). */
    if (g->rcSweepFree == 1) return false;
    if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false;
    /* Sharding (`EXTC_RC_SHARDS` > 1): marking starts only from the roots of **this one shard
     * region** plus the cross-region seeds, so an object that only live objects in another shard
     * point at cannot be marked => it would be taken for "reachable from the old value alone" and
     * released (dangling). Under sharding `rcNoteFreshLocal` registers nothing anyway => nothing
     * can be emitted here; blocking it explicitly keeps the **code and this comment from one
     * source**, instead of relying on a side effect elsewhere. */
    if (g->rcNShard > 1) return false;
    if (g->loopLen <= 0) return false;                       /* 1 */
    if (!value || value->kind != EX_IDENT) return false;
    const char *vn = planCName(value) ? planCName(value) : value->u.ident.name;
    if (!vn || !rcLinkedHas(g, vn)) return false;             /* 4 */
    int idx = rcRootIndexOf(g, tgtName);                     /* 2 */
    if (idx < 0) return false;   /* the target is not a reference slot of this frame (neither globals nor the fields of a local
     * aggregate are emitted) */
    for (int i = 0; i < g->loopLen; i++)
        if (idx >= g->loopRootBase[i]) return false;         /* declared inside some enclosing loop */
    const char *vreg = NULL;
    for (int i = 0; i < g->rcFreshN; i++) {
        if (strcmp(g->rcFreshName[i], vn) != 0) continue;
        if (!rcFrameOwnsArena(g, g->rcFreshLvl[i])) return false;   /* 3 */
        vreg = rcFreshHostRegion(g, value);
        break;
    }
    if (!vreg) return false;
    if (g->rcGlobalRoots.len + g->rcRoots.len > 4096) return false;
    Buf b;
    bufInit(&b, g->arena);
    size_t n = rcRootTableTextSkip(g, &b, (size_t)idx);
    cgLine(g, "extc_rc_storeRefOwnedRelease((void **)&(%s), (void *)(%s), (void *)0, %s, %s, %zu);",
           tgt, val, vreg, bufCstr(&b), n);
    return true;
}

/**
 * @brief Walk the **value projections** to find the object hosting a reference slot.
 *
 * The cross-region registration bit is set on the **target** object, while the "source" is the
 * object holding this slot: `a->f` is `a`, `*p` is `p`, the inline array element `w.arr[i]` is `w`
 * (the element storage lives in the object's payload, independent of the index), and `p[i]` of a
 * `ref [N]T` is `p`. So the walk goes outwards only along **value fields** (the `EX_FIELD` levels
 * whose base type is not a reference), inline arrays and the elements of a `ref [N]T`, until it
 * reaches a `ref` base or a dereference.
 *
 * Why three states: when the host cannot be traced, **sweeping as usual is out of the question**
 * (an incomplete seed releases too early). The caller picks the path from the return value:
 * `RC_HOST_OBJ` registers, `RC_HOST_LOCAL` does nothing (the root table covers it), and
 * `RC_HOST_UNKNOWN` takes the conservative path.
 *
 * @param g    The generator state (needs `subst` for generic arguments).
 * @param t    The assignment's target expression.
 * @param obj  Out: the host object expression for `RC_HOST_OBJ`.
 * @return     See `RcHost`.
 */
static RcHost cgSlotHost(CG *g, Expr *t, Expr **obj) {
    *obj = NULL;
    for (;;) {
        if (!t) return RC_HOST_UNKNOWN;
        switch (t->kind) {
        case EX_DEREF:
            /* `*p = v`: the slot is the payload of the object p points to. */
            *obj = t->u.deref.operand;
            return RC_HOST_OBJ;
        case EX_FIELD: {
            Expr *b = t->u.field.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            /* `p.f = v` (p is a reference, field access dereferences automatically): the slot is in *p's
     * payload. */
            if (bt && bt->kind == TY_REF) { *obj = b; return RC_HOST_OBJ; }
            /* value field: the slot's storage still belongs to b's host, keep walking outwards */
            t = b;
            continue;
        }
        case EX_INDEX: {
            Expr *b = t->u.index.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            if (bt && bt->kind == TY_REF) {
                Type *bi = bt->inner ? ttBase(subst(g, bt->inner)) : NULL;
                /* `p[i] = v` where p is `ref [N]T`: the element is inline in *p. */
                if (bi && bi->kind == TY_ARRAY) { *obj = b; return RC_HOST_OBJ; }
                /* a ref to a view: the element is in the storage the view's data points to, untraceable. */
                return RC_HOST_UNKNOWN;
            }
            /* inline array (a value): the element storage belongs to the array's host. */
            if (bt && bt->kind == TY_ARRAY) { t = b; continue; }
            /* view element: data may point into a plate, a pool, the stack or C storage -- the host cannot
     * be traced. */
            return RC_HOST_UNKNOWN;
        }
        case EX_IDENT:
            /* a variable's own storage: the static root table registers it (together with its value fields
     * and inline arrays) as a root. */
            return RC_HOST_LOCAL;
        /* kind-default: a projection whose host cannot be traced is always UNKNOWN (the safe
         * direction). A new kind that can host a reference slot must add a case here instead of
         * slipping through default. */
        default:
            return RC_HOST_UNKNOWN;
        }
    }
}

static bool rcLinkStmt(void *ctx, Stmt *s);
static bool rcLinkExpr(void *ctx, Expr *e);

/**
 * @brief Statement callback: record the identifiers "written into a heap object's reference
 *        field" in `g->rcLinked`.
 *
 * Only a **heap host** (`RC_HOST_OBJ`: `(*cur).next = head`, `p.f = v`) counts as "chained into a
 * tree"; a local's slot (a local array in `a[i] = v`, a local struct field) is the tier the root
 * table covers and does not count.
 *
 * @param ctx  The generator state.
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcLinkStmt(void *ctx, Stmt *s) {
    CG *g = (CG *)ctx;
    Expr *tgt = (s && s->kind == ST_ASSIGN) ? s->u.assign.target : NULL;
    Expr *ve  = (s && s->kind == ST_ASSIGN) ? s->u.assign.value : NULL;
    if (tgt && ve && !s->u.assign.op && ve->kind == EX_IDENT && g->needReclaim &&
        tgt->type && cgIsRefType(g, tgt->type)) {
        Expr *base = NULL;
        if (cgSlotHost(g, tgt, &base) == RC_HOST_OBJ) {
            const char *nm = planCName(ve) ? planCName(ve) : ve->u.ident.name;
            if (nm) *(const char **)vecPush(&g->rcLinked) = nm;
        }
    }
    AstVisit v = { rcLinkExpr, rcLinkStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/** @brief Expression callback: keep walking (an assignment can also be nested inside an
 *         expression statement or a condition). */
static bool rcLinkExpr(void *ctx, Expr *e) {
    AstVisit v = { rcLinkExpr, rcLinkStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Scan the function body once: which local names have been written into a heap object's
 *        reference field (the evidence set behind criterion 4).
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcScanLinked(CG *g, Stmt *body) {
    vecInit(&g->rcLinked, g->arena, sizeof(const char *));
    if (!body || !g->needReclaim) return;
    AstVisit v = { rcLinkExpr, rcLinkStmt, g };
    astWalkStmtChildren(body, &v);
}

/**
 * @brief When a whole-value store carries reference leaves, emit the reference-slot accounting
 *        **leaf by leaf**.
 *
 * A **by-value** whole-block copy (struct or fixed-size array) such as `s.w = wrap { n: keeper }`
 * or `var b: wrap = wrap { n: keeper }` can hide reference leaves, and C's whole-block assignment
 * is a bare compound-literal copy (`s->w = (wrap){.n = keeper};`): it neither `inc`s nor registers
 * a cross-region edge. The slot is therefore written without any accounting -- and then either
 * the count never learns of this holder (the only visible holder lets go, the count reaches zero
 * and the slot dangles), or the sweep's seed is incomplete (no cross-region in-edge was
 * registered => the target region's sweep releases objects that are still pointed at, the other
 * half of the hole that `cross_region_elem` shows). So the whole block is split back into **one
 * reference-slot assignment per leaf**: a new slot goes through `initRef` (write only and
 * increment, never read the old value), an overwritten slot through `storeRefOwned` /
 * `storeRefSlot` (decrement the old value, increment the new one; when the host cannot be proved,
 * first reverse-look-up by **slot address** which region the slot falls in, and only when that
 * fails make the target region drop out of sweeping) -- the same path as a direct reference-slot
 * assignment.
 *
 * The caller is responsible for evaluating the target and the value **once each** (into
 * temporaries), because every accounting entry assembles the path once more.
 *
 * @param g           The generator state.
 * @param t           The type of the whole-value store (taken in the caller's substituted
 *                    context).
 * @param dstBase     The C lvalue expression of the target storage (such as `(*__extc_svd3)`,
 *                    `b`).
 * @param srcBase     The C lvalue expression of the value (such as `__extc_svv3`); not read when
 *                    `init` is true.
 * @param init        true = new slot (the initialization of a `var ...`): increment only; false =
 *                    overwrite: read the old value and decrement.
 * @param owner       "the object holding this slot" (the third argument of `storeRefOwned`); pass
 *                    `0` for a local or static slot.
 * @param hostUnknown The host cannot be proved => every leaf takes `storeRefSlot` (reverse lookup
 *                    of the region by slot address), and only when the slot is not in an rc slab
 *                    either does the conservative path apply (the target region drops out of
 *                    sweeping).
 */
static void cgValueRefStore(CG *g, Type *t, const char *dstBase, const char *srcBase,
                            bool init, const char *owner, bool hostUnknown) {
    /* Collect the **relative** path only once (`.n`, `.kids.data[0]`, empty string for a top-level
     * reference) and assemble it against each of the two bases -- the target's and the value's leaf
     * sets are necessarily isomorphic by construction, so no path exists on one side only. */
    Vec rel;
    vecInit(&rel, g->arena, sizeof(const char *));
    rcLeafPaths(g, t, "", &rel, 0);
    for (size_t i = 0; i < rel.len; i++) {
        const char *p = *(const char **)vecAt(&rel, i);
        const char *d = arenaPrintf(g->arena, "%s%s", dstBase, p);
        if (init) {
            /* the value is already in the target (the declaration is the initialization): read and write the
     * same storage once, which is a write plus an increment of the target */
            dbgRcSite(g, g->blkLevel, 0);
            cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", d, d);
        } else if (hostUnknown) {
            /* When the host expression cannot be proved, the **slot address** itself may carry the answer
             * (it falls inside an rc slot => that is the memory of some region): try the reverse lookup
             * first, and fall back to the conservative path only when the slot is not in an rc slab
             * either (view data, C or pool buffers). */
            cgLine(g, "extc_rc_storeRefSlot((void **)&(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p));
        } else {
            cgLine(g, "extc_rc_storeRefOwned((void **)&(%s), (void *)(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p), owner);
        }
    }
}

/**
 * @brief Emit the region release at **frame destruction** (next to `extc_arena_destroy`).
 *
 * The claim key is the arena's address (see the table in `emitRuntimePrologue`), so only the
 * address is emitted here and the region itself need not be known. A coroutine step uses the
 * frame's `arena<k>` and spells it exactly as the arena release does. Under sharding one logical
 * region claims N run-time regions (key = address + k), so every shard must be **dropped
 * individually** -- missing one leaves that shard's slab and registration bit unreleased forever.
 *
 * Called only at frame destruction: a block release (`cgReleaseLevel`) does not drop the region,
 * or a loop body would create a new 1 MiB slab on every iteration. A region outliving its block is
 * safe -- every object in the block is unreachable and the sweep reclaims them by pressure.
 *
 * @param g    The generator state; nothing is emitted while `needReclaim` is off.
 * @param lvl  The block level (1 = the function body).
 */
static void cgReclaimDropArena(CG *g, int lvl) {
    if (!g->needReclaim) return;
    /* This function never referenced this level (it neither allocated nor handed it out as a home)
     * => this arena cannot have a region and the `regionDrop` would only scan the registry for
     * nothing. Skip it. */
    if (lvl >= 1 && !(g->rcArenaUsed & (1u << lvl))) return;
    if (g->rcNShard <= 1) {
        /* single shard: one region, one drop -- the same text either way */
        if (g->coroFunc)
            cgLine(g, "extc_rc_regionDrop((void *)&%s->arena%d);", g->coroFrame, lvl);
        else
            cgLine(g, "extc_rc_regionDrop((void *)&__extc_a[%d]);", lvl);
        return;
    }
    const char *ar = g->coroFunc ? arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, lvl)
                                 : arenaPrintf(g->arena, "__extc_a[%d]", lvl);
    for (int k = 0; k < g->rcNShard; k++)
        cgLine(g, "extc_rc_regionDrop((void *)((char *)&%s + %d));", ar, k);
}

/**
 * @brief Emit `ST_VAR`: an ordinary declaration, a coroutine spawn, an `@overwrite` unit, and a
 *        declaration initialized by `f()?`.
 *
 * The coroutine spawn branch treats the frame as an ordinary local value and initializes the
 * parameters in place (a resume has no arguments to pass again); with a `var h: coroutine<T>`
 * annotation the frame goes into the task's own place, while the plain form leaves it on the
 * caller's stack with no allocation. The `@overwrite` branch keeps a single storage cell: allocated
 * lazily on first execution, then only zeroed and reused. A variable inside a coroutine body is not
 * a C local but a field of the frame, so its initialization is emitted as an assignment.
 *
 * @param g  The code generation state.
 * @param s  The variable statement.
 */
static void stVar(CG *g, Stmt *s) {
    rcNoteFreshLocal(g, s);
/* `let c = counter(args)`: **spawn**. The frame is a plain value living here, and the
 * arguments initialize the parameters -- a resume has none to pass them again. */
if (planCName(s) && s->u.var.init && s->u.var.init->kind == EX_CALL &&
    planCallee(s->u.var.init) && planIsCoro(planCallee(s->u.var.init))) {
    FuncDef *cf = planCallee(s->u.var.init);
    const char *fr = cType(g, s->type);      /* the synthesized frame type */
    /* The **task's own place**: entered here (lazily, at the spawn), stored in the frame,
     * and left again so the caller keeps its own place current. Everything the coroutine
     * creates goes in there and outlives every suspension; the task's end releases it in
     * one go (see the `$next` driver in `genCoroDecls`). */
    /* `var h: coroutine<T> = f(...)`: the annotation *is* the handle type, so the frame
     * goes into the task's own place and the variable holds a 24-byte handle. This is the
     * escape-driven choice in its simplest form -- the same source shape without the
     * annotation keeps the frame on the caller's stack (zero allocation). */
    if (s->type && isProtoType(s->type, "coroutine", 1)) {
        const char *cn = cFuncName(g, cf);
        int sq = g->coroSeq++;
        flushPrefix(g);
        g->needCoroHandle = true;
        cgLine(g, "int64_t __extc_czh%d = extc_task_begin();", sq);
        cgLine(g, "int64_t __extc_czz%d = extc_task_zone(__extc_czh%d);", sq, sq);
        cgLine(g, "struct %s$frame *__extc_czf%d = (struct %s$frame *)extc_task_alloc("
                  "__extc_czh%d, (int64_t)sizeof(struct %s$frame), \"%s\", %d);",
               cn, sq, cn, sq, cn, g->path, s->line);
        Buf bx;
        bufInit(&bx, g->arena);
        bufPrintf(&bx, "*__extc_czf%d = (struct %s$frame){ .pc = 0, .zone = __extc_czz%d,"
                      " .task = __extc_czh%d", sq, cn, sq, sq);
        for (size_t i = 0; i < cf->params.len; i++) {
            Param *p = *(Param **)vecAt(&cf->params, i);
            Expr *a = i < s->u.var.init->u.call.args.len
                          ? *(Expr **)vecAt(&s->u.var.init->u.call.args, i) : NULL;
            bufPrintf(&bx, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
        }
        bufPrintf(&bx, " };");
        cgLine(g, "%s", bufCstr(&bx));
        cgLine(g, "extc_coro %s = (extc_coro){ .frame = __extc_czf%d, .kind = %d,"
                  " .task = __extc_czh%d };", planCName(s), sq, planCoroKind(cf), sq);
        g->needCoroHandle = true;
        g->needPool = true;      /* the task table brings the zone runtime with it */
        /* A coroutine frame can hold reference slots, and the static root table does not enumerate the
     * frame => no trigger is emitted for this function (the safe direction). */
        if (g->needReclaim) g->rcRootsUnreliable = true;
        return;
    }
    const bool hasZone = planCoroNeedsZone(cf);
    const int  sq = g->coroSeq++;
    if (hasZone) {
        flushPrefix(g);
        cgLine(g, "int64_t __extc_czsv%d = extc_zoneTop;", sq);
        cgLine(g, "int64_t __extc_czid%d = extc_task_begin();", sq);
        cgLine(g, "int64_t __extc_czm%d = extc_task_zone(__extc_czid%d);", sq, sq);
    }
    /* `var d = inner(args)` **inside a coroutine**: the nested frame is a *field* of this
     * frame, not a C local -- a resume jumps past the declaration, so the initializer has
     * to be an assignment to the field (same rule as every other frame-field local below). */
    bool framed = false;
    if (g->coroFunc) {
        for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
            framed = p->cname && strcmp(p->cname, planCName(s)) == 0;
        }
    }
    Buf init;
    bufInit(&init, g->arena);
    if (framed)
        bufPrintf(&init, "%s->%s = (%s){ .pc = 0", g->coroFrame, planCName(s), fr);
    else
        bufPrintf(&init, "%s %s = (%s){ .pc = 0", fr, planCName(s), fr);
    if (hasZone) bufPrintf(&init, ", .zone = __extc_czm%d, .task = __extc_czid%d", sq, sq);
    for (size_t i = 0; i < cf->params.len; i++) {
        Param *p = *(Param **)vecAt(&cf->params, i);
        Expr *a = i < s->u.var.init->u.call.args.len
                      ? *(Expr **)vecAt(&s->u.var.init->u.call.args, i) : NULL;
        bufPrintf(&init, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
    }
    bufPrintf(&init, " };");
    flushPrefix(g);
    cgLine(g, "%s", bufCstr(&init));
    if (hasZone) cgLine(g, "extc_zoneTop = __extc_czsv%d;", sq);
    /* The frame is a value local and can hold reference slots; the static root table does not
     * enumerate the synthesized frame => this function turns triggers off (the safe direction). */
    if (g->needReclaim) g->rcRootsUnreliable = true;
    return;
}
/* A frame field is not declared here: the frame struct holds it, and the assignment is
 * what a resume re-runs -- the initializer runs on every execution, as a C local's
 * would. */
if (g->coroFunc && planCName(s)) {
    bool framed = false;
    for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
        const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
        framed = p->cname && strcmp(p->cname, planCName(s)) == 0;
    }
    if (framed) {
        if (s->u.var.init) {
            const char *v = genTopExpr(g, s->u.var.init);
            flushPrefix(g);
            /* A frame field is an **existing** slot (the frame is a compound literal, so pointer fields
             * read as 0) and a resume re-runs this initialization => overwrite semantics, through
             * storeRef: decrement the old value, increment the new one. */
            if (g->needReclaim && cgIsRefType(g, s->type))
                cgLine(g, "extc_rc_storeRef((void **)&%s->%s, (void *)(%s));",
                       g->coroFrame, planCName(s), v);
            else
                cgLine(g, "%s->%s = %s;", g->coroFrame, planCName(s), v);
        }
        return;
    }
}
/* `cname` is the name the checker decided on; a shadowed one
 * carries a `__2` suffix. */
const char *nm = planCName(s) ? planCName(s) : s->u.var.name;
/* `global var x: T` / `global let x: T = c`: emit **C block-scope `static`** -- static storage
 * duration with the name visible only in this block, which is exactly the semantics needed.
 * Two further benefits: it is fine for the declaration to be **executed repeatedly** (a loop or a
 * coroutine resume both jump into the middle of the statement), because C initializes a static
 * object once, at program start. The initializer must be constant, and the checker already
 * enforces that. */
const char *stg = s->u.var.isGlobal ? "static " : "";
/* `@overwrite` keeps exactly one block of storage. It is allocated
 * the first time this statement runs and only cleared and reused
 * afterwards. What comes out is a few lines of inline C,
 * `extc_arena_alloc` plus `memset`, so no new runtime support is
 * needed. */
if (s->u.var.overwrite) {
    /* One block of storage is reused; `extc_owcell`, a block plus
     * its capacity, covers all three shapes - a single value, a
     * fixed-size buffer and a runtime length. The first execution
     * allocates and lazily at that, every later one clears and
     * reuses, and a reused cell reads as the zero value `new` promises.
     *
     * Where the cell comes from: a site of this function uses a cell
     * in its own frame, a site of a callee uses the cell passed in
     * by the call site. */
    int k = owIndex(g, s);
    Expr *nx = s->u.var.init;
    if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, nx, "@overwrite new");
    Type *st_t = subst(g, nx->u.new_.type);
    bool loc = f_owLocal(g, s);
    if (k >= 0) {
        const char *cell = loc ? arenaPrintf(g->arena, "&__extc_ow%d", k)
                               : arenaPrintf(g->arena, "__extc_owarg%d", k);
        /* The storage lives where the cell's own `home` says:
         * with a home it belongs to `__extc_home`, otherwise to
         * level 1 of this frame. `arenaRefAt(planArenaLevel(nx))` must
         * not be used here: that number is the callee's own level,
         * while the cell lives elsewhere, and the two do not share a
         * lifetime. */
        const char *owcv = arenaPrintf(g->arena, "__owc%d", k);
        const char *ar = arenaPrintf(g->arena, "(*%s->home)", owcv);
        (void)planArenaLevel(nx);
        const char *ct = cType(g, st_t);
        flushPrefix(g);
        /* The site index has to be part of the name: two
         * `@overwrite` sites in one function would otherwise be a
         * redefinition of the same variable. */
        cgLine(g, "extc_owcell *%s = %s;", owcv, cell);
        if (!nx->u.new_.count) {
            /* A single value, or a fixed-size `new [N]T`, has a
             * constant size. The binding has to be declared first,
             * because a declaration inside the if or the else branch
             * would go out of scope at its closing brace. */
            cgLine(g, "%s %s;", cType(g, s->type), nm);
            /* The temporary must not be named `p`: with
             * `@overwrite var p = ...` a `void *p` would shadow the
             * outer `array_T *p`, turning `p = (array_T *)p` into a
             * self-assignment and leaving the binding null. */
            const char *owtp = arenaPrintf(g->arena, "__owp%d", k);
            /* With reclamation on, this storage also comes from an rc region: an `@overwrite` binding can
             * have a reference type too, and a pointer into the arena reaching the counting functions
             * would read garbage. The region is claimed by the cell's home arena and lives as long as
             * that arena. */
            if (g->needReclaim) {
                /* Type descriptor + sweep trigger, placed before the allocating statement: this storage also comes
                 * from an rc region, the descriptor is stored with the slot, and the region claim key the
                 * trigger uses matches the `extc_rc_alloc` below (the same shard number, see
                 * `rcAllocCore`). */
                const char *td = rcEmitTypeDesc(g, st_t);
                /* Is this region (the cell's `home`) **owned by this frame**: the cell is in this frame
                 * (`f_owLocal`) and this function does not receive a home, so `home` is this frame's
                 * `&__extc_a[1]`. When this function does receive a home (`planUsesHome`, so the cell's
                 * `home` is the caller's arena), or `owLocal` is false (a generic instance generated
                 * during codegen), both land on an **outer frame** -- this frame's static root table
                 * cannot see the outer frame's live references, so this tier only records what is owed
                 * (see `rcEmitOwe`). **`main` is the exception (subset A)**: the home `main` receives
                 * is its own `&__extc_a[1]`, so it counts (the same criterion as
                 * `rcFrameOwnsArena`). */
                bool own = loc && (!g->rcFuncUsesHome || g->inMain);
                /* The thread-local arena tier: the cell's `home` is `extc_tls_arena` or this frame's arena, and
                 * ownership is decided at run time (`RC_OWN_TLS`) => an inner frame only records what is
                 * owed. */
                RcOwn owOwn = g->parTls ? RC_OWN_TLS : (own ? RC_OWN_YES : RC_OWN_NO);
                const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)%s->home", owcv),
                                               planArenaLevel(nx), owOwn,
                                               arenaPrintf(g->arena, "(int64_t)sizeof(%s)", ct), td);
                flushPrefix(g);
                cgLine(g, "if (!%s || !%s->p) { void *%s = %s;  if (%s) %s->p = %s;"
                          "  %s = (%s)%s; }",     /* the C type is already a pointer */
                        owcv, owcv, owtp, core,
                        owcv, owcv, owtp, nm, cType(g, s->type), owtp);
            } else
                cgLine(g, "if (!%s || !%s->p) { void *%s = extc_arena_alloc(&%s,"
                          " (int64_t)sizeof(%s), \"%s\", %d);  if (%s) %s->p = %s;"
                          "  %s = (%s)%s; }",     /* the C type is already a pointer */
                        owcv, owcv, owtp, ar, ct, g->path, nx->line,
                        owcv, owcv, owtp, nm, cType(g, s->type), owtp);
            cgLine(g, "else { memset(%s->p, 0, (size_t)sizeof(%s));"
                      "  %s = (%s)%s->p; }",
                    owcv, ct, nm, cType(g, s->type), owcv);
        } else {
            /* A runtime length uses `{ptr, cap}` and doubles the
             * capacity when it grows, so the memory stays within
             * twice the largest length seen and does not depend on
             * the number of iterations. */
            const char *cnt = genExpr(g, nx->u.new_.count);
            if (planNeedTemp(nx)) {
                const char *t = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
                pfLine(g, "int64_t %s = (int64_t)(%s);", t, cnt);
                cnt = t;
            }
            cgLine(g, "int64_t __owk = (int64_t)(%s);", cnt);
            cgLine(g, "if (__owk < 0) { extc_trapMsg(\"%s\", %d,"
                      " \"negative length\"); }", g->path, nx->line);
            cgLine(g, "%s %s;", cType(g, s->type), nm);
            cgLine(g, "if (!%s) { %s = (%s){ .data = (%s *)extc_arena_alloc(&%s,"
                      " __owk * (int64_t)sizeof(%s), \"%s\", %d), .len = __owk }; }",
                    owcv, nm, cType(g, s->type), ct, ar, ct, g->path, nx->line);
            cgLine(g, "else { if (__owk > %s->cap) { int64_t c = %s->cap * 2;"
                      " if (c < __owk) c = __owk;"
                      "  %s->p = extc_arena_alloc(&%s, c * (int64_t)sizeof(%s),"
                      " \"%s\", %d);  %s->cap = c; }"
                      "  %s = (%s){ .data = (%s *)%s->p, .len = __owk }; }",
                    owcv, owcv, owcv, ar, ct, g->path, nx->line, owcv,
                    nm, cType(g, s->type), ct, owcv);
            cgLine(g, "memset(%s.data, 0, (size_t)(__owk * (int64_t)sizeof(%s)));",
                    nm, ct);
        }
        /* An `@overwrite` binding is a reference slot too: it stays live past this statement, until the
     * block ends. */
        rcRootDecl(g, s->type, nm);
        return;
    }
}
/* `let q = f()?` and `var q = f()?` are one of the legal positions
 * of `?`. The shape matches the assignment case, because the generated
 * C is a declaration followed by an initialization. */
if (s->u.var.init && s->u.var.init->kind == EX_TRY) {
    TryInfo ti = genTryHead(g, s->u.var.init);
    flushPrefix(g);
    size_t lb = g->out->len;
          /* `@value`: copying from a place would share storage, so the value is built through the
       * type's `clone` (a temporary was moved and never reaches this branch). */
      if (planVarClone(s) && s->u.var.init) {
          StructDef *vsd = structOf(ttBase(subst(g, s->type)));
          const char *fn = vsd && vsd->cloneHook ? cMethodName(g, subst(g, s->type), vsd->cloneHook)
                                                 : NULL;
          const char *src = genExpr(g, s->u.var.init);
          if (fn && src)
              cgLine(g, "%s%s %s = %s(&(%s));", stg, cType(g, s->type), nm, fn, src);
          else
              cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, tryPayloadPath(g, &ti));
      } else
          cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, tryPayloadPath(g, &ti));
    localDef(g, lb, nm, 1);
    rcRootDecl(g, s->type, nm);
    return;
}
const char *init = s->u.var.init ? genTopExpr(g, s->u.var.init)
                                 : zeroInit(g, s->type);
flushPrefix(g);
size_t lb = g->out->len;
/* `@value`: a copy from a place goes through the type's `clone` (a temporary was moved and is
 * not marked, so this branch is skipped). */
{
    StructDef *vsd = s->type ? structOf(ttBase(subst(g, s->type))) : NULL;
    const char *fn = (planVarClone(s) && vsd && vsd->cloneHook)
                         ? cMethodName(g, subst(g, s->type), vsd->cloneHook) : NULL;
    if (fn && s->u.var.init && init) {
        /* A method that returns an aggregate takes the same implicit arguments as any other call
         * (`int64_t __extc_home_zone`), so the call site has to append them. */
        Buf cl;
        bufInit(&cl, g->arena);
        bufPrintf(&cl, "%s(&(%s)", fn, asF32(g, s->type, s->u.var.init, init));
        cgImplicitArgs(g, &cl, vsd->cloneHook, s->u.var.init, 1, false);
        bufPuts(&cl, ")");
        cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, bufCstr(&cl));
    }
    else
        cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, asF32(g, s->type, s->u.var.init, init));
}
localDef(g, lb, nm, 1);
/* Counting for reference slots. A **new slot** goes through initRef: it writes and increments the
 * target, and **never reads the old value** -- the "old value" lying in a freshly declared slot is a
 * free-list slot number (the slab allocator keeps its free list in the slot head), and dereferencing
 * it as a pointer crashes. The value is read again from the variable just written, so evaluation
 * happens only once.
 *
 * Global declarations are skipped: they are `static`, so the statement **runs again** every time
 * control reaches it and the count would be incremented repeatedly; and a global reference is a
 * process-level root that belongs with the root table (left for the next step).
 *
 * A cross-region store (the object is in an outer region, the slot in this block) registers one more
 * bit: a bit on the target object's position, and the sweep walks the bitmap in order when seeding.
 * Registering too much is the safe direction (it only keeps an object alive longer); registering too
 * little is a missed root. */
if (g->needReclaim && !s->u.var.isGlobal && cgIsRefType(g, s->type)) {
    dbgRcSite(g, g->blkLevel, 0);
    if (planRcNoDec(g->curFunc))
        cgLine(g, "extc_rc_initRefNoHold((void **)&%s, (void *)%s);", nm, nm);
    else
        cgLine(g, "extc_rc_initRef((void **)&%s, (void *)%s);", nm, nm);
} else if (g->needReclaim && !s->u.var.isGlobal && s->u.var.init) {
    /* A **whole-value** declaration's reference leaves (`var b: wrap = wrap { n: keeper }`) need
     * accounting too: a bare C initialization emits no `inc` at all, so `b.n` is another way for "the
     * slot was written without accounting" (the same hole as the assignment side). With no
     * initializer the value is **zero** (`zeroInit`) => every reference leaf is null, `inc` has
     * nothing to do, and the whole `initRef` would only place a few hundred lines into the output for
     * nothing (a local such as `world` with 64+1024 slots). */
    RcScan vsc = rcScanType(g, s->type, 0);
    if (vsc == RC_LEAF_REFS) {
        cgValueRefStore(g, s->type, nm, nm, true, "0", false);
    } else if (vsc == RC_CONSERVATIVE) {
        bool enumRefs = false;
        if (rcEnumRootable(g, s->type, 0, &enumRefs) && enumRefs) {
            /* References hidden only in enum payloads: `rcRootDecl` then registers the payload leaves of
             * **every** variant as roots (garbage words read from an inactive variant are filtered by
             * `extc_rc_owns`), so the store side only has to "count each word once as a candidate
             * reference". Together the two claim this value completely, so the target region is **not**
             * let out of sweeping -- this is exactly the plug for the permanent noSweep region in the
             * "match binding of a reference payload" hole. */
            cgLine(g, "extc_rc_storeStructRooted((void *)&(%s), sizeof(%s));",
                   nm, cType(g, s->type));
        } else {
            /* The leaves cannot be enumerated (a generic argument carrying a reference, an unrecognized kind):
             * count each word as a candidate reference, increment only and never decrement. There is no
             * source to undo from in the local tier and the root table cannot enumerate these leaves
             * either => `hostKnown = 0`, the target region drops out of sweeping (safe direction: better
             * to reclaim too little). */
            cgLine(g, "extc_rc_storeStructCons((void *)&(%s), sizeof(%s), (void *)0, 0);",
                   nm, cType(g, s->type));
        }
    }
}
/* This declaration is a **root** from this point on: the trigger point's root table has to list it
 * (a struct or fixed-size array is expanded as leaf reference slots). A global declaration is
 * registered too: it is block-scope `static`, visible in this block, and keeping it a bit longer is
 * the safe direction. */
rcRootDecl(g, s->type, nm);
return;
}

/* --------------------------------------- view-subscript loop-entry check
 *
 * `a[i]` calls a checking primitive on every access (null pointer + bounds, trapping out of range)
 * and then returns a pointer. The check itself is cheap, but it is a **branch** inside an inner loop
 * body, which alone blocks vectorization of the whole loop; and the most common subscript in a
 * `for k in lo..hi` body is exactly affine in k (`a[r*n + c]`), with the loop's whole trip count
 * known before entry (the parser desugars the upper bound into an `__extc_hi` temporary, evaluated
 * once).
 *
 * Once that is recognized, the body's view subscripts affine in k are collected and one range check
 * per (view, coef, base) is emitted before the `while`, with the body switching to bare pointer
 * access: the checked range is the two endpoint values of `coef*k + base` over k in [lo, hi), and
 * run time compares which is larger. Out of range still traps, with the same message
 * `trap: index %lld out of range (length %lld)` -- it is just reported at loop entry.
 *
 * Only this conservative tier is done; when it cannot be proved the whole loop falls back to
 * per-element checks:
 *   - the body contains a call / allocation / trap / early exit / conditionally evaluated
 *     expression -- don't touch it;
 *   - the subscript is not affine scalar arithmetic, or the index type is not i64 (i32 wraps, so
 *     recomputing is not equivalent) -- not accepted;
 *   - a quantity in coef/base is assigned in the body -- not accepted;
 *   - the view object is not a plain identifier, the view itself is rebound in the body, or the
 *     site is inside a conditional branch -- not accepted;
 *   - an `@unchecked` body (the author turned checks off) or a coroutine body (a state machine
 *     shape) -- don't touch it.
 *
 * Bare access is recorded by node identity: `g->hoistedIdx` holds the `EX_INDEX` nodes already
 * covered, and `exIndex` consults the table to decide between the checking primitive and a bare
 * pointer. Nested loops append their own entries and truncate the length back to the entry `mark`
 * when leaving the body.
 */

/** @brief One view-subscript site covered by a loop-entry check. */
typedef struct {
    const char *view;   /* the view object's C name */
    const char *coef;   /* the coef of the subscript = coef*k + base (a C expression) */
    const char *base;   /* as above, base (a C expression without k) */
    int         line;   /* the site's line number (for the trap) */
    Expr       *site;   /* the covered EX_INDEX node */
} HoistedIdx;

/** @brief The text form of `coef*k + base` (a coef of "0" means it does not depend on k). */
typedef struct {
    const char *coef;
    const char *base;
} Affine;

/**
 * @brief The C name of an identifier: when the checker renamed it (shadowing, or a suffix on
 *        `__extc_hi`), the renamed one is used.
 *
 * @param e  The expression; returns NULL when it is not `EX_IDENT`.
 */
static const char *identCName(const Expr *e) {
    if (!e || e->kind != EX_IDENT) return NULL;
    const char *cn = planCName(e);
    return cn ? cn : e->u.ident.name;
}

/**
 * @brief Is this expression one whose "evaluating once more at loop entry" does not change the
 *        program's behavior: no call, no allocation, no assignment?
 *
 * @param e  The expression to judge.
 */
static bool hoistPureExpr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_IDENT: return true;
    case EX_BIN: return hoistPureExpr(e->u.bin.left) && hoistPureExpr(e->u.bin.right);
    case EX_UN: return hoistPureExpr(e->u.un.operand);
    default: return false;
    }
}

/**
 * @brief Does `e` contain an identifier whose C name is `name`?
 *
 * @param e     The expression to search (only the node kinds a pure expression can have).
 * @param name  The C name to compare against.
 */
static bool exprHasIdent(const Expr *e, const char *name) {
    if (!e || !name) return false;
    switch (e->kind) {
    case EX_IDENT: {
        const char *cn = identCName(e);
        return cn && strcmp(cn, name) == 0;
    }
    case EX_BIN: return exprHasIdent(e->u.bin.left, name) || exprHasIdent(e->u.bin.right, name);
    case EX_UN: return exprHasIdent(e->u.un.operand, name);
    default: return false;
    }
}

/**
 * @brief Try to split the expression into `coef*k + base`; returns false when it cannot be split.
 *
 * `coef` and `base` are **text**: a subexpression independent of k is emitted as it is and evaluated
 * once at entry. Only scalar arithmetic (`+`, `-`, `*`, unary `-`) is recognized; no symbolic prover
 * is brought in.
 *
 * @param g    The generator (temporary text is allocated in its arena).
 * @param e    The subscript expression.
 * @param var  The loop variable's C name.
 * @param out  The split-out coefficient and constant term.
 */
static bool affineIn(CG *g, Expr *e, const char *var, Affine *out) {
    if (!e || !var) return false;
    /* a pure expression independent of k is used whole as the constant term: emitted as it is and
     * evaluated once at loop entry. */
    if (hoistPureExpr(e) && !exprHasIdent(e, var)) {
        out->coef = "0";
        out->base = genExpr(g, e);
        return true;
    }
    if (e->kind == EX_IDENT) {
        const char *cn = identCName(e);
        if (cn && strcmp(cn, var) == 0) { out->coef = "1"; out->base = "0"; return true; }
        return false;
    }
    if (e->kind == EX_BIN && e->u.bin.op) {
        const char *op = e->u.bin.op;
        if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
            Affine l, r;
            if (!affineIn(g, e->u.bin.left, var, &l)) return false;
            if (!affineIn(g, e->u.bin.right, var, &r)) return false;
            const bool sub = strcmp(op, "-") == 0;
            if (strcmp(l.coef, "0") == 0 && strcmp(r.coef, "0") == 0) return false;
            out->coef = arenaPrintf(g->arena, "(%s %s %s)", l.coef, sub ? "-" : "+", r.coef);
            out->base = arenaPrintf(g->arena, "(%s %s %s)", l.base, sub ? "-" : "+", r.base);
            return true;
        }
        if (strcmp(op, "*") == 0) {
            /* only one side may contain k: the other side is multiplied across entirely. Both sides containing
     * k is nonlinear and not accepted. */
            const bool lk = exprHasIdent(e->u.bin.left, var);
            const bool rk = exprHasIdent(e->u.bin.right, var);
            if (lk == rk) return false;
            Expr *kSide = lk ? e->u.bin.left : e->u.bin.right;
            Expr *cSide = lk ? e->u.bin.right : e->u.bin.left;
            Affine kk;
            if (!affineIn(g, kSide, var, &kk)) return false;
            const char *factor = genExpr(g, cSide);
            out->coef = arenaPrintf(g->arena, "(%s * %s)", kk.coef, factor);
            out->base = arenaPrintf(g->arena, "(%s * %s)", kk.base, factor);
            return true;
        }
        return false;
    }
    if (e->kind == EX_UN && e->u.un.op && strcmp(e->u.un.op, "-") == 0) {
        Affine kk;
        if (!affineIn(g, e->u.un.operand, var, &kk)) return false;
        out->coef = arenaPrintf(g->arena, "(-%s)", kk.coef);
        out->base = arenaPrintf(g->arena, "(-%s)", kk.base);
        return true;
    }
    return false;
}

/**
 * @brief Collect the C names of the identifiers in `e` other than the loop variable; returns false
 *        when one of them is not a built-in scalar type.
 *
 * Only built-in scalars are let through: struct fields, views, references and "something a function
 * returns" cannot be hoisted as a constant term.
 *
 * @param g      The generator (needs `subst`).
 * @param e      The subscript expression.
 * @param var    The loop variable's C name (its type need not be a built-in scalar).
 * @param names  The collected result (`const char *`).
 */
static bool hoistCollectIdents(CG *g, const Expr *e, const char *var, Vec *names) {
    if (!e) return true;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: return true;
    case EX_IDENT: {
        const char *cn = identCName(e);
        if (cn && var && strcmp(cn, var) == 0) return true;
        Type *t = e->type ? ttBase(subst(g, e->type)) : NULL;
        if (!t || t->kind != TY_BUILTIN) return false;
        if (!cn) return false;
        *(const char **)vecPush(names) = cn;
        return true;
    }
    case EX_BIN:
        return hoistCollectIdents(g, e->u.bin.left, var, names)
            && hoistCollectIdents(g, e->u.bin.right, var, names);
    case EX_UN: return hoistCollectIdents(g, e->u.un.operand, var, names);
    default: return false;
    }
}

/** @brief Is `name` already in `names` (`const char *`)? */
static bool nameInVec(const Vec *names, const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < names->len; i++)
        if (strcmp(*(const char **)vecAt((Vec *)names, i), name) == 0) return true;
    return false;
}

/** @brief Does the body contain something that hoisting the check would change: a call, an
 *         allocation, a trap, an early exit, a lambda... */
static bool hoistForbiddenExpr(const Expr *e);

/**
 * @brief Does the statement tree contain a construct forbidden for hoisting the check (an early
 *        exit, a call, a trap, a conditional evaluation)?
 *
 * An early exit is hard-forbidden: moving the check of `a[k]` to loop entry means trapping before a
 * `break`/`return`. A conditionally evaluated expression (`??`, short-circuit evaluation, a call) is
 * the same: evaluating it once at entry changes how many times its side effects happen.
 *
 * @param s  The statement.
 */
static bool hoistForbiddenStmt(const Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR: return hoistForbiddenExpr(s->u.var.init);
    case ST_ASSIGN:
        return hoistForbiddenExpr(s->u.assign.target) || hoistForbiddenExpr(s->u.assign.value)
            || hoistForbiddenExpr(s->u.assign.opExpr);
    case ST_EXPR: return hoistForbiddenExpr(s->u.expr.expr);
    case ST_IF:
        return hoistForbiddenExpr(s->u.ifs.cond) || hoistForbiddenStmt(s->u.ifs.thenBody)
            || hoistForbiddenStmt(s->u.ifs.elseBody);
    case ST_WHILE:
        return hoistForbiddenExpr(s->u.whiles.cond) || hoistForbiddenStmt(s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (hoistForbiddenStmt(*(Stmt **)vecAt((Vec *)&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH: {
        if (hoistForbiddenExpr(s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt((Vec *)&s->u.match.arms, i);
            if (arm && hoistForbiddenStmt(arm->body)) return true;
        }
        return false;
    }
    /* Only the allowed kinds are listed: `break`/`continue`/`return`/`trap`/`yield`/`domain` and any
     * statement kind added later fall to default and are always forbidden (fall back to per-element
     * checks, semantics unchanged). */
    default: return true;
    }
}

/** @brief Does `e` contain a forbidden construct (only the node kinds that can appear in a pure
 *         expression are examined; anything else is forbidden)?
 *
 * A conversion is pure: it turns one value into another and cannot trap, call, or be skipped, so
 * the scan looks through it. Without that, `i64(v[i])` -- the shape nearly every byte loop has --
 * would read as a side effect and no loop containing one would ever hoist its bounds test. */
static bool hoistForbiddenExpr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_IDENT: case EX_NULL: return false;
    case EX_BIN:
        /* An operator may be resolved by the checker into a method call (`cout << x`, a user-defined
         * operator, array `==` going through `extc_eq`), or deferred until instantiation -- all
         * calls, so none of them is a pure expression. */
        if (planCallee(e) || planNeedOp(e)) return true;
        return hoistForbiddenExpr(e->u.bin.left) || hoistForbiddenExpr(e->u.bin.right);
    case EX_UN:
        if (planCallee(e) || planNeedOp(e)) return true;
        return hoistForbiddenExpr(e->u.un.operand);
    case EX_CONV: return hoistForbiddenExpr(e->u.conv.operand);
    case EX_FIELD: return hoistForbiddenExpr(e->u.field.obj);
    case EX_INDEX: return hoistForbiddenExpr(e->u.index.obj) || hoistForbiddenExpr(e->u.index.index);
    case EX_DEREF: return hoistForbiddenExpr(e->u.deref.operand);
    case EX_SIGN: return hoistForbiddenExpr(e->u.sign.operand);
    case EX_ARRAYLIT: {
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (hoistForbiddenExpr(*(Expr **)vecAt((Vec *)&e->u.arraylit.elems, i))) return true;
        return false;
    }
    default: return true;
    }
}

/** @brief Collect the view-subscript sites that certainly execute every round (does not cross
 *         conditional branches or inner loops). */
static void hoistSitesExpr(Expr *e, Vec *sites);

/**
 * @brief Collect the view-subscript sites that certainly execute every round along a statement
 *        chain.
 *
 * Only unconditional statements are crossed: the statement chain of `ST_BLOCK`, the initializer of
 * `ST_VAR`, both sides of an assignment, expression statements. Sites inside `if`/`match` branches
 * and inner `while` loops do not count as "executed every round" (not entered); a skipped site keeps
 * going through the per-element check, semantics unchanged.
 *
 * @param g      The generator.
 * @param s      The statement.
 * @param sites  The collected result (`Expr *`).
 */
static void hoistSitesStmt(CG *g, Stmt *s, Vec *sites) {
    (void)g;
    if (!s) return;
    switch (s->kind) {
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;   /* the step of a for runs only at the end of each round and is not collected here */
            hoistSitesStmt(g, one, sites);
        }
        return;
    }
    case ST_VAR: hoistSitesExpr(s->u.var.init, sites); return;
    case ST_ASSIGN:
        hoistSitesExpr(s->u.assign.target, sites);
        hoistSitesExpr(s->u.assign.value, sites);
        return;
    case ST_EXPR: hoistSitesExpr(s->u.expr.expr, sites); return;
    /* An `if` condition is evaluated every round, so its subscripts are covered too; the two
     * branches are not (a subscript there runs only on the rounds the branch is taken). */
    case ST_IF: hoistSitesExpr(s->u.ifs.cond, sites); return;
    default: return;
    }
}

/** @brief The view-subscript sites in an expression (only pure expressions are crossed;
 *         side-effecting nodes are already blocked as forbidden). */
static void hoistSitesExpr(Expr *e, Vec *sites) {
    if (!e) return;
    switch (e->kind) {
    case EX_INDEX:
        *(Expr **)vecPush(sites) = e;
        hoistSitesExpr(e->u.index.obj, sites);
        hoistSitesExpr(e->u.index.index, sites);
        return;
    case EX_BIN:
        hoistSitesExpr(e->u.bin.left, sites);
        hoistSitesExpr(e->u.bin.right, sites);
        return;
    case EX_UN: hoistSitesExpr(e->u.un.operand, sites); return;
    case EX_FIELD: hoistSitesExpr(e->u.field.obj, sites); return;
    case EX_CONV: hoistSitesExpr(e->u.conv.operand, sites); return;
    case EX_DEREF: hoistSitesExpr(e->u.deref.operand, sites); return;
    default: return;
    }
}

/** @brief Collect the C names of the identifiers assigned in the body (a for step does not count:
 *         it only advances the loop variable). */
static void hoistWrites(Stmt *s, Vec *names) {
    if (!s) return;
    switch (s->kind) {
    case ST_ASSIGN: {
        Expr *t = s->u.assign.target;
        if (t && t->kind == EX_IDENT) {
            const char *cn = identCName(t);
            if (cn) *(const char **)vecPush(names) = cn;
        }
        return;
    }
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;
            hoistWrites(one, names);
        }
        return;
    }
    case ST_IF:
        hoistWrites(s->u.ifs.thenBody, names);
        hoistWrites(s->u.ifs.elseBody, names);
        return;
    case ST_WHILE: hoistWrites(s->u.whiles.body, names); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
            if (arm) hoistWrites(arm->body, names);
        }
        return;
    default: return;
    }
}

/** @brief Collect the names declared in the body (`var`, match bindings; a for step declares no new
 *         name).
 *
 * The entry check is emitted **before** the loop, where names declared in the body are not in scope
 * yet: using them as the constant term of an affine form would make the generated C reference an
 * undeclared identifier (`off` in `a[off]` is exactly that). They are collected once and the sites
 * are rejected by name -- the check falls back to per-element, semantics unchanged, only without the
 * hoisting.
 */
static void hoistDecls(Stmt *s, Vec *names) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR: {
        const char *cn = planCName(s);
        if (cn) *(const char **)vecPush(names) = cn;
        return;
    }
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;
            hoistDecls(one, names);
        }
        return;
    }
    case ST_IF:
        hoistDecls(s->u.ifs.thenBody, names);
        hoistDecls(s->u.ifs.elseBody, names);
        return;
    case ST_WHILE: hoistDecls(s->u.whiles.body, names); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
            if (!arm) continue;
            for (size_t k = 0; k < arm->binds.len; k++) {
                const char *cn = *(const char **)vecAt(&arm->binds, k);
                if (cn) *(const char **)vecPush(names) = cn;
            }
            hoistDecls(arm->body, names);
        }
        return;
    default: return;
    }
}

/**
 * @brief Recognize the range-for shape and do entry range checks for the body's view subscripts
 *        (emit nothing when it cannot be proved).
 *
 * The shape is fixed by the checker's desugaring: `for k in lo..hi` is emitted as
 * `k = lo; while (k < __extc_hi) { body; k += 1 }`, with the upper bound evaluated once **outside**
 * the loop. The entry check therefore holds only for the trip range [lo, hi), and the out-of-range
 * decision for an array or view is exactly the per-element one (trapping early: only when the body
 * would really execute).
 *
 * @param g  The generator (uses `tmpSeq`, `arena`, `path`).
 * @param s  The `while` statement; the caller guarantees `!condTemps && !ownLevel` and that the
 *           function body has not turned checks off.
 */
static void loopHoistRangeFor(CG *g, Stmt *s) {
    if (!g || !s || g->uncheckedBody || g->coroFunc) return;
    Stmt *body = s->u.whiles.body;
    Expr *cond = s->u.whiles.cond;
    if (!body || body->kind != ST_BLOCK || !cond || cond->kind != EX_BIN) return;
    if (!cond->u.bin.op || strcmp(cond->u.bin.op, "<") != 0) return;
    Expr *lv = cond->u.bin.left, *rv = cond->u.bin.right;
    if (!lv || lv->kind != EX_IDENT || !rv || rv->kind != EX_IDENT) return;
    const char *var = identCName(lv);
    const char *hi = identCName(rv);
    if (!var || !hi || strncmp(hi, "__extc_hi", 9) != 0) return;
    if (!planForStep(body)) return;    /* a redirected step (iterator for) is not this trip range */
    if (hoistForbiddenStmt(body)) return;

    /* who is assigned in the body: the loop variable's trip, the upper bound, the constant terms and
     * the view object itself must not change. */
    Vec writes; vecInit(&writes, g->arena, sizeof(const char *));
    hoistWrites(body, &writes);
    if (nameInVec(&writes, var) || nameInVec(&writes, hi)) return;

    /* names declared in the body are not in scope at entry and cannot be the constant term of an
     * affine form. */
    Vec decls; vecInit(&decls, g->arena, sizeof(const char *));
    hoistDecls(body, &decls);

    Vec sites; vecInit(&sites, g->arena, sizeof(Expr *));
    hoistSitesStmt(g, body, &sites);

    Vec picks; vecInit(&picks, g->arena, sizeof(HoistedIdx));
    for (size_t i = 0; i < sites.len; i++) {
        Expr *site = *(Expr **)vecAt(&sites, i);
        Expr *obj = site->u.index.obj;
        Expr *ix = site->u.index.index;
        if (!obj || obj->kind != EX_IDENT || !ix || idxHoisted(g, site)) continue;
        Type *ot = obj->type;
        if (!ot || ot->kind == TY_REF) continue;
        if (!isView(ttBase(subst(g, ot)))) continue;
        Type *it = ix->type;
        if (!it) continue;
        it = ttBase(subst(g, it));
        if (!it || !ttIs(it, "i64")) continue;
        const char *view = identCName(obj);
        if (!view || nameInVec(&writes, view)) continue;
        Vec ids; vecInit(&ids, g->arena, sizeof(const char *));
        if (!hoistCollectIdents(g, ix, var, &ids)) continue;
        bool bad = false;
        for (size_t k = 0; k < ids.len; k++) {
            const char *one = *(const char **)vecAt(&ids, k);
            if (nameInVec(&writes, one) || nameInVec(&decls, one)) { bad = true; break; }
        }
        if (bad) continue;
        Affine af;
        if (!affineIn(g, ix, var, &af)) continue;
        HoistedIdx *h = vecPush(&picks);
        h->view = view;
        h->coef = af.coef;
        h->base = af.base;
        h->line = site->line;
        h->site = site;
    }
    if (picks.len == 0) return;

    /* First emit all the empty-view checks (with the same message as the checking primitive), then one
     * range check per deduplicated affine form. Wrapped in `k < hi`: when the trip range is empty the
     * loop body never executes and the check must not trap. */
    cgLine(g, "if ((int64_t)(%s) < (int64_t)(%s)) {", var, hi);
    g->indent++;
    for (size_t i = 0; i < picks.len; i++) {
        HoistedIdx *h = vecAt(&picks, i);
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++)
            if (strcmp(((HoistedIdx *)vecAt(&picks, k))->view, h->view) == 0) seen = true;
        if (!seen)
            cgLine(g, "if (!(%s).data) extc_trapMsg(\"%s\", %d, \"the view has no storage\");",
                   h->view, g->path, h->line);
    }
    for (size_t i = 0; i < picks.len; i++) {
        HoistedIdx *h = vecAt(&picks, i);
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++) {
            HoistedIdx *p = vecAt(&picks, k);
            if (strcmp(p->view, h->view) == 0 && strcmp(p->coef, h->coef) == 0
                && strcmp(p->base, h->base) == 0) seen = true;
        }
        if (seen) continue;
        const int id = g->tmpSeq++;
        cgLine(g, "int64_t __extc_rk%d_c = (int64_t)(%s);", id, h->coef);
        cgLine(g, "int64_t __extc_rk%d_b = (int64_t)(%s);", id, h->base);
        cgLine(g, "int64_t __extc_rk%d_i = __extc_rk%d_c * (int64_t)(%s) + __extc_rk%d_b;",
               id, id, var, id);
        cgLine(g, "int64_t __extc_rk%d_j = __extc_rk%d_c * ((int64_t)(%s) - 1) + __extc_rk%d_b;",
               id, id, hi, id);
        cgLine(g, "if (__extc_rk%d_i > __extc_rk%d_j) { int64_t __extc_rk%d_t = __extc_rk%d_i; "
                  "__extc_rk%d_i = __extc_rk%d_j; __extc_rk%d_j = __extc_rk%d_t; }",
               id, id, id, id, id, id, id, id);
        cgLine(g, "if (__extc_rk%d_i < 0 || __extc_rk%d_j >= (int64_t)(%s).len) "
                  "extc_trap(\"%s\", %d, __extc_rk%d_i < 0 ? __extc_rk%d_i : __extc_rk%d_j, "
                  "(int64_t)(%s).len);",
               id, id, h->view, g->path, h->line, id, id, id, h->view);
    }
    g->indent--;
    cgLine(g, "}");

    for (size_t i = 0; i < picks.len; i++)
        *(Expr **)vecPush(&g->hoistedIdx) = ((HoistedIdx *)vecAt(&picks, i))->site;
}

/** @brief Is this view-subscript site already covered by the entry check of some outer loop? */
static bool idxHoisted(CG *g, Expr *e) {
    if (!g || g->hoistedIdx.len == 0) return false;
    for (size_t i = 0; i < g->hoistedIdx.len; i++)
        if (*(Expr **)vecAt(&g->hoistedIdx, i) == e) return true;
    return false;
}


/**
 * @brief Emit `ST_WHILE`: an ordinary `while (...)` or a `while (1)` shape that takes a level of its
 *        own.
 *
 * Evaluating the condition can write temporaries into the statement prefix (the right operand of an
 * array `==`, a method receiver, or the subject of `??`). Flushing the prefix **before** the
 * `while (...)` would evaluate it once only, while the condition reads that temporary every round,
 * so `while a == mk()` would keep comparing the first value; when there is a prefix the condition is
 * therefore moved into the loop body, where the number of evaluations matches the source and the
 * body is unchanged. With no prefix (the vast majority) `while (cond) {` is kept.
 *
 * When the condition allocates space itself it takes a level of its own, released at the start of
 * every round; `g->blkLevel` is raised for the whole loop so that the condition and the body agree
 * with the level the checker assigned (`blkMaxLevel` counts one level more for this shape).
 *
 * A `for`'s step statement is recorded on the body block; while the step is still inside the body,
 * the loop gets a label id, `genBlockBody` emits the label and `ST_CONTINUE` jumps to it.
 *
 * @param g  The code generation state.
 * @param s  The `while` statement; its body must be a block.
 */
static void stWhile(CG *g, Stmt *s) {
const char *cnd = cgCond(g, genExpr(g, s->u.whiles.cond));
/* Generating the condition may have written temporaries to the statement prefix:
 * `eqOperand` materializes the right operand of an array `==` there, and a receiver or a
 * `??` subject can do the same. Flushing that prefix **in front of** `while (...)` runs it
 * once, while the condition reads the temporary on every round -- `while a == mk()` would
 * compare against the first call's value forever. When there is a prefix, put the condition
 * inside the loop instead: the same number of evaluations as the source, and the body is
 * untouched. No prefix (the overwhelmingly common case) keeps the plain `while (cond) {`
 * shape. */
const bool condTemps = g->prefix.len != 0;
/* A `for` loop records its step on the body block; give this loop a label id if that
 * step is still part of the body (the iterator retargeting clears it, see
 * `forRetargetToIterator`). `genBlockBody` emits the label, `ST_CONTINUE` jumps to it. */
Stmt *wbody = s->u.whiles.body;
int stepLabel = 0;
if (wbody && planForStep(wbody)) {
    for (size_t i = 0; i < wbody->u.block.stmts.len; i++)
        if (*(Stmt **)vecAt(&wbody->u.block.stmts, i) == planForStep(wbody)) { stepLabel = ++g->forStepSeq; break; }
}
/* A condition that **allocates** gets its own arena level, released at the top of every
 * round: the checker put its sites one scope deeper for exactly this. Without
 * it the condition's allocations lived until the enclosing block ended, so the loop grew
 * with the number of rounds. `g->blkLevel` is raised for the whole loop so the condition
 * and the body agree with what the checker assigned; the body block then sits one level
 * deeper than before, which is why `blkMaxLevel` counts an extra level for this shape. */
const bool ownLevel = planCondAllocs(s);
/* Hoist the view-subscript checks to loop entry. Only done in the shape where the condition and the
 * prefix are both "clean": the other path moves the condition into the body, where the check would
 * rerun every round and would also trap wrongly when the condition is false. */
const size_t hoistMark = g->hoistedIdx.len;
if (!condTemps && !ownLevel) loopHoistRangeFor(g, s);
if (condTemps || ownLevel) {
    cgLine(g, "while (1) {");
    g->indent++;
    if (ownLevel) g->blkLevel++;
    /* the condition's own level clears its arena every round; the reclamation region is left until
 * frame destruction (see `cgReleaseLevel`). */
    if (ownLevel) cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);
    flushPrefix(g);               /* the temporaries, now once per round */
    cgLine(g, "if (!(%s)) break;", cnd);
} else {
    cgLine(g, "while (%s) {", cnd);
    g->indent++;
}
g->loopLevel[g->loopLen] = g->blkLevel + 1;   /* the body is the next level */
g->loopStep[g->loopLen] = stepLabel;
g->loopStepUsed[g->loopLen] = false;
/* The reference slots live when the loop is entered: slots with a subscript smaller than this cell
 * are the long-lived slots "live before the loop starts" (criterion 2 of `rcEmitStoreRelease`). The
 * loop body's block has not been entered yet, so the root table is exactly the slots live outside
 * the loop. */
g->loopRootBase[g->loopLen] = (int)g->rcRoots.len;
g->loopLen++;
*(size_t *)vecPush(&g->dropLoopBases) = g->dropBlocks.len;
genBlockBody(g, s->u.whiles.body);
g->dropLoopBases.len--;
g->hoistedIdx.len = hoistMark;   /* restore the outer loop's site table: this loop's ranges hold only inside this loop */
g->loopLen--;
if (ownLevel) g->blkLevel--;
g->indent--;
cgLine(g, "}");
return;
}

/**
 * @brief Emit `ST_MATCH`: the arms become an if/else chain, binding payloads first when needed.
 *
 * The matched expression is evaluated once (with a payload-carrying enum it lands in a temporary
 * first), because the arm bodies read `.u.<variant>` and a second evaluation would repeat side
 * effects. Inside a generic instance the record's type is `subst`ed to the instance type first, or
 * the arms would compare a template type against a constant that does not exist.
 *
 * An if/else chain is used rather than a `switch` on the variant constant: a `break`/`continue` in
 * an arm body belongs to the enclosing loop, and inside a `switch` it would only leave the switch.
 * The trailing `else` appears only when the last arm is the `_` fallback arm; otherwise the checker
 * has already guaranteed the match is exhaustive.
 *
 * @param g  The code generation state.
 * @param s  The `match` statement.
 */
static void stMatch(CG *g, Stmt *s) {
/* The scrutinee is evaluated exactly once, into a temporary when
 * the enum has a payload, because an arm reads `.u.<variant>` and
 * evaluating the expression a second time - as in
 * `match f() { ... }` - would be wrong. */
/* `subst` matters here for the same reason it does on the construction side
 * (`EX_ENUMVAL`): inside the body of a generic instance the scrutinee's
 * recorded type is the *template's* type, so `et->name` would be `option_V` and the
 * arm would compare against `option_V_none`, a constant that does not exist.
 * Substituting first gives the instance's name (`option_i64`), and the payload types
 * below are then already concrete. */
Type *et = ttBase(subst(g, s->u.match.scrutinee->type));
bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);
const char *subj = genExpr(g, s->u.match.scrutinee);
if (payload) {
    const char *tmp = arenaPrintf(g->arena, "__extc_m%d", g->tmpSeq++);
    cgLine(g, "%s %s = %s;", cType(g, et), tmp, subj);
    subj = tmp;
}

/* The arms become an if/else chain, deliberately not a `switch`
 * over the variant constants: a `break` or `continue` in an arm
 * body belongs to the enclosing loop, but inside a `switch` it
 * would only leave the switch, so a `while true` with a `break`
 * would never terminate.
 *
 * A final `else` appears only when the last arm is the `_` catch-all;
 * otherwise the checker guarantees that the match is exhaustive, and a
 * missing variant would not compile at all. */
flushPrefix(g);
for (size_t i = 0; i < s->u.match.arms.len; i++) {
    MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
    if (arm->wildcard) {
        /* the `_` arm has no payload to bind: it is the trailing else of the if chain (when the first arm is
     * `_`, a block is emitted). */
        cgLine(g, "%s", i == 0 ? "{" : "} else {");
        g->indent++;
        genBlockBody(g, arm->body);
        g->indent--;
        continue;
    }
    cgLine(g, "%s (%s%s == %s_%s) {", i == 0 ? "if" : "} else if",
           subj, payload ? ".tag" : "", et ? et->name : "?", arm->variant);
    g->indent++;
    /* Bind the payload: `circle(r) => ...` becomes
     * `double r = tmp.u.circle._0;`. */
    size_t firstBind = g->deadDefs.len;
    for (size_t k = 0; k < arm->binds.len; k++) {
        Variant *v = et && et->edef ? NULL : NULL;
        (void)v;
        Type *bt = NULL;
        if (et && et->edef) {
            for (size_t j = 0; j < et->edef->variants.len; j++) {
                Variant *vv = *(Variant **)vecAt(&et->edef->variants, j);
                if (strcmp(vv->name, arm->variant) != 0) continue;
                bt = *(Type **)vecAt(&vv->types, k);
                /* A generic enum instance substitutes its type
                 * arguments into the payload type, so `just(T)`
                 * with `T = slice<u8>` yields the instance type. */
                if (et->edef->typeParams.len > 0 &&
                    et->targs.len == et->edef->typeParams.len)
                    bt = ttSubstitute(g->tt, bt, &et->edef->typeParams, &et->targs);
                break;
            }
        }
        flushPrefix(g);              /* nothing pending may end up in the span */
        /* A payload binding is a C local **inside this arm**: its scope ends with the `if` block, and the
         * block-registered mechanism of the static root table cannot cover it. So when the bound type
         * contains a reference slot, this function is treated as having an incomplete root table and
         * emits no trigger at all (the safe direction). */
        if (g->needReclaim && bt && rcScanType(g, bt, 0) != RC_NO_REFS)
            g->rcRootsUnreliable = true;
        size_t beforeBind = g->out->len;
        cgLine(g, "%s %s = %s.u.%s._%zu;", cType(g, bt), *(const char **)vecAt(&arm->binds, k),
               subj, arm->variant, k);
        /* An arm that never reads its payload - `none => 0` written with a
         * binding it does not use, or an arm that only ignores the value it
         * matched - should not pay for the copy. The line is remembered, and
         * the arm body below says whether anything names it. */
        Buf bl;
        bufInit(&bl, g->arena);
        bufPutn(&bl, g->out->data + beforeBind, g->out->len - beforeBind);
        DeadDef *bd = arenaAllocZero(g->arena, sizeof *bd);
        bd->name = *(const char **)vecAt(&arm->binds, k);
        bd->text = bufCstr(&bl);
        bd->off  = beforeBind;
        bd->scoped = true;   /* counted in the arm body only */
        *(DeadDef **)vecPush(&g->deadDefs) = bd;
    }
    size_t bodyA = g->out->len;
    genBlockBody(g, arm->body);
    size_t bodyB = g->out->len;
    /* Only the bindings of *this* arm: a nested match's bindings already know their own
     * stage, and a larger scope here would count them as mentioned. */
    for (size_t k = firstBind; k < firstBind + arm->binds.len && k < g->deadDefs.len; k++) {
        DeadDef *bd = *(DeadDef **)vecAt(&g->deadDefs, k);
        bd->scopeA = bodyA;      /* what could read the binding: this arm body */
        bd->scopeB = bodyB;
    }
    g->indent--;
}
cgLine(g, "}");
return;
}

/**
 * @brief Emit the statement itself; this is the real dispatch table, while prefix bookkeeping lives
 *        in `genStmt`.
 *
 * `ST_VAR`, `ST_WHILE` and `ST_MATCH` are emitted by `stVar`, `stWhile` and `stMatch` respectively,
 * which is what makes this table read like a table.
 *
 * @param g  The code generation state.
 * @param s  The statement to emit.
 */
static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_TRAP: {
            /* `trap(msg)`: **the same exit** -- the traps the compiler inserts itself go through it too.
             * `extc_trapMsg` prints `file:line: trap: message` to stderr and then calls `extc_die(1)`
             * (the dying hooks still run => buffered output is not lost). So "the program is broken"
             * has exactly one spelling in this language, whether the compiler says it or the author
             * does. */
            Expr *m = s->u.trap_.msg;
            /* Nothing from `rt`/`rtPrint` here: `extc_trapMsg` and `extc_die` are primitives of
             * the runtime block, and `trap` prints no structured payload. */
            if (m->kind == EX_STR) {
                /* literal: take the short path, with the message as an **argument** rather than a format string
     * (so a `%` in a user's message is safe) */
                cgLine(g, "extc_trapMsg(\"%s\", %d, \"%s\");", g->path, s->line, m->u.str.text);
                return;
            }
            Type *mt = m->type;
            if (mt && mt->kind == TY_REF) {
                cgLine(g, "extc_trapMsg(\"%s\", %d, (const char *)(%s));",
                       g->path, s->line, genExpr(g, m));
                return;
            }
            /* run-time slice: evaluate once (it may have side effects), then print a slice as long as its
     * length */
            const char *t = arenaPrintf(g->arena, "__extc_trap%d", g->tmpSeq++);
            cgLine(g, "%s %s = %s;", cType(g, mt), t, genExpr(g, m));
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: %%.*s\\n\", \"%s\", %d,"
                      " (int)(%s).len, (const char *)(%s).data);",
                   g->path, s->line, t, t);
            cgLine(g, "extc_die(1);");
            return;
        }
        case ST_DROP: {
            /* `drop x`: run the hook **here**, and remember the name so the block tail does not run it
             * a second time. The checker has already rejected a second drop and any later use. */
            Expr *target = s->u.drop_.target;
            const char *nm = planCName(target);
            if (!nm && target->kind == EX_IDENT) nm = target->u.ident.name;
            if (nm && target->type) genDropValue(g, nm, target->type, 0);
            if (nm) {
                *(const char **)vecPush(&g->dropExplicit) = nm;
                if (target->kind == EX_IDENT)
                    *(const char **)vecPush(&g->dropExplicit) = target->u.ident.name;
            }
            return;
        }
        case ST_DOMAIN: {
            /* `d.run { ... }`: the block ends by **running its tasks to completion** -- that is what
             * makes the block structured (nothing it started is still unfinished when it exits).
             * The domain itself is not freed here: the caller owns it (`single()` handed it over) and
             * may run another block on it; its lifetime is its own step. */
            Expr *recv = (s->u.domain_.callee && s->u.domain_.callee->kind == EX_FIELD)
                             ? s->u.domain_.callee->u.field.obj : NULL;
            if (!recv) {
                ctxError(g->ctx, s->line, 1, NULL, "domain block: no receiver to drive");
                return;
            }
            /* The block's own statements come **first**: they are what registers the tasks
             * (`ext f(x)` => `extc_dom_add`) and what the caller's locals hold. Only then does the
             * domain drive them to completion -- "the block cannot end while a task it started is
             * still unfinished" is exactly this order, and it is also why the drive call belongs
             * **inside** the block: each `ext` declares its frame as a local of this block
             * (`EX_EXT`), so running after the closing brace would hand the domain pointers to
             * stack slots that are already out of scope. */
            Stmt *body = s->u.domain_.body;
            /* The surface syntax is always a braced block (`d.run { ... }`), and the checker keeps
             * that shape, so `body` is a block here. */
            EXTC_DBG_ASSERT(body != NULL && body->kind == ST_BLOCK);
            if (body && body->kind == ST_BLOCK) {
                cgLine(g, "{");
                g->indent++;
                genBlockBody(g, body);
                const char *dv = genExpr(g, recv);
                g->needDomain = true;
                flushPrefix(g);
                cgLine(g, "extc_dom_run(%s);", dv);
                g->indent--;
                cgLine(g, "}");
                return;
            }
            genStmt(g, body);
            const char *dv = genExpr(g, recv);
            g->needDomain = true;
            flushPrefix(g);
            cgLine(g, "extc_dom_run(%s);", dv);
            return;
        }
        case ST_VAR: stVar(g, s); return;

        case ST_ASSIGN:
            /* An assignment whose value is a `?` reads the payload; the failure
             * path has already returned by then. */
            if (s->u.assign.value && s->u.assign.value->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.assign.value);
                g->inAssignTarget = true;
                const char *at = genExpr(g, s->u.assign.target);
                g->inAssignTarget = false;
                flushPrefix(g);
                cgLine(g, "%s = %s;", at, tryPayloadPath(g, &ti));
                return;
            }
            /* Overwriting a binding that owns its state runs its `@drop` hook first: the old value
             * is going away, and this is the last moment it can hand that state back. Skipped for
             * `a = a`, where dropping first would release the very storage being read. */
            if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
                struct { bool same; } guard = { false };
                if (s->u.assign.value && s->u.assign.value->kind == EX_IDENT)
                    guard.same = strcmp(s->u.assign.value->u.ident.name,
                                        s->u.assign.target->u.ident.name) == 0;
                const char *hn = planCName(s->u.assign.target);
                const char *src0 = s->u.assign.target->u.ident.name;
                /* A place that an explicit `drop` already emptied holds nothing to hand back, and the
                 * assignment makes it a live place again -- so the name leaves the "already dropped"
                 * list here. */
                bool wasDropped = false;
                if (src0) {
                    size_t keep = g->dropExplicit.len;
                    for (size_t k = 0; k < g->dropExplicit.len; k++) {
                        const char *n = *(const char **)vecAt(&g->dropExplicit, k);
                        if (n && strcmp(n, src0) == 0) { wasDropped = true; continue; }
                        *(const char **)vecAt(&g->dropExplicit, keep++) = n;
                    }
                    g->dropExplicit.len = keep;
                }
                if (!guard.same && !wasDropped && hn &&
                    typeNeedsDrop(g, s->u.assign.target->type, 0))
                    genDropValue(g, hn, s->u.assign.target->type, 0);
            }
            g->inAssignTarget = true;
            const char *tgt = genExpr(g, s->u.assign.target);
            g->inAssignTarget = false;
            const char *val = genTopExpr(g, s->u.assign.value);
            /* Assigning a reference slot has **overwrite** semantics (the slot already holds an old value), so
             * it goes through storeRef: decrement the old value, increment the new one. A data field
             * (`p.val = x`) is not a reference type => not a word is emitted.
             *
             * A cross-region edge (`a->f = v` where `a` and `v` are not in the same region) **must be
             * registered**: the target region's sweep seeds from roots plus registration bits, and a
             * heap-to-heap cross-region in-edge has nowhere else to come from. Clearing the
             * registration bit is the source side's job (`rscnt` counting plus withdrawal when the
             * source object or source region dies), so it is **not sticky** -- setting without
             * clearing would make the object created each round of a loop a seed forever (memory
             * grows linearly with the round count).
             *
             * The third argument is "the object holding this slot": `a->f` passes `a`, `*p` passes
             * `p`, and a local or global slot passes nothing (those are roots covered by the trigger
             * point's root table). The object expression must be a **pure place**, or it is evaluated
             * twice at the same spot (`f()->f = v` would call `f()` twice). */
            const bool refStore = g->needReclaim && !s->u.assign.op
                                  && cgIsRefType(g, s->u.assign.target->type);
            /* A **whole-value** store (a struct or fixed-size array copied by value): the target slot is not a
             * reference type itself, but its payload can hold reference leaves
             * (`s.w = wrap { n: keeper }`). Such an assignment cannot take the bare C whole-block copy
             * -- that writes the slot without `inc` and without cross-region registration (the same
             * class of "the slot was written but not accounted", reached through another mechanism).
             * The three states match a reference slot: enumerable => one
             * `storeRefOwned`/`storeRefSlot` per leaf; not enumerable (enum payloads, a generic
             * argument carrying a reference) => the conservative per-word path (increment only, plus
             * registration or dropping out of sweeping). */
            Type *rawTT = s->u.assign.target ? s->u.assign.target->type : NULL;
            const RcScan valScan = (g->needReclaim && !s->u.assign.op && !refStore && rawTT)
                                       ? rcScanType(g, rawTT, 0) : RC_NO_REFS;
            const bool valueStore = valScan != RC_NO_REFS;
            const char *owner = "0";
            bool hostUnknown = false;
            bool hostProven = false;   /* the host is an object the **region was traced to** (not a local or static slot) */
            Expr *hostBase = NULL;     /* the host object expression: the store point uses it to decide "the region can be given
     * statically" */
            if ((refStore || valueStore) && s->u.assign.target) {
                Expr *t = s->u.assign.target;
                /* The three outcomes for the host (see `cgSlotHost`): a pure place base => register the
                 * cross-region edge; statically declared storage => the root table covers it; not
                 * traceable => the conservative path. In `c.a = v` (`c` is a struct **value**) the slot
                 * belongs to a local or the arena, not to a slot of this run time; if the container of
                 * `a[i]` is an inline array (`w.arr[i]`) the host is `w`, while in a view it cannot be
                 * traced. */
                Expr *base = NULL;
                RcHost h = cgSlotHost(g, t, &base);
                hostBase = base;
                if (h == RC_HOST_OBJ && base && cgPurePlace(g, base)) {
                    owner = genExpr(g, base);
                    hostProven = true;
                } else if (h != RC_HOST_LOCAL) {
                    hostUnknown = true;
                }
            }
            flushPrefix(g);
            /* `x += y` is emitted as `x += y`: C evaluates the target **once**, which is
             * exactly the meaning the statement has, and it is what a reader expects to
             * find. A user-defined operator is a method, so that one case becomes the plain
             * assignment of its call - the checker has already required a target with no
             * call in it, because the call names the target a second time. */
            if (s->u.assign.op) {
                if (planCompoundCall(s) && s->u.assign.opExpr)
                    cgLine(g, "%s;", genBin(g, s->u.assign.opExpr));
                else if (s->u.assign.opExpr && planCallee(s->u.assign.opExpr))
                    cgLine(g, "%s = %s;", tgt, genBin(g, s->u.assign.opExpr));
                else
                    cgLine(g, "%s %s %s;", tgt, s->u.assign.op, val);
                return;
            }
            if (refStore) {
                dbgRcSite(g, g->blkLevel, 0);
                if (hostUnknown) {
                    /* The host expression cannot be proved (`(*id(s)).kids[0] = v`, a write to a view element): this
                     * slot must still be a seed, but the static root table does not hold it and the source
                     * side has nothing to withdraw either. The **slot address** itself usually still
                     * carries the answer -- a slot falling inside an rc slot tells which slab (hence
                     * which region) holds it, so the accounting proceeds as if the host were known
                     * (cross-region registration and old-value withdrawal both happen). Only when the
                     * slot is not in an rc slab either (view data, C or pool buffers) does the
                     * conservative direction apply: mark **the region the target lives in** as not
                     * swept, and from then on the region is reclaimed only by the count reaching
                     * zero. */
                    cgLine(g, "extc_rc_storeRefSlot((void **)&(%s), (void *)(%s));", tgt, val);
                    return;
                }
                const char *hreg = rcFreshHostRegion(g, hostBase);
                if (hreg) {
                    /* The host is a **fresh local of this frame** (its arena level is known while this line is
                     * generated). Check whether the value is the same: if so both are in one region =>
                     * this edge cannot be cross-region => registration, the region comparison and both
                     * `ownSlab` lookups can all be skipped (`EXTC_RC_DBG_LEVEL=1` prints the decision).
                     *
                     * Only done in the no-count tier: with counting on, `storeRefOwnedReg` still needs
                     * those two lookups for the holder count. A wrong decision means "missed
                     * registration => dangling"; the guardrail is the cross-region registration count in
                     * the shape ratchet (`gate-reclaim-switch`'s cross-region shape turns red the moment
                     * a registration is missed). */
                    int hlvl = 0, vlvl = -2;          /* -2 = the value is not a fresh local */
                    if (hostBase && hostBase->kind == EX_IDENT) {
                        const char *hn = planCName(hostBase) ? planCName(hostBase)
                                                             : hostBase->u.ident.name;
                        for (int i = 0; i < g->rcFreshN; i++)
                            if (hn && strcmp(g->rcFreshName[i], hn) == 0) hlvl = g->rcFreshLvl[i];
                    }
                    Expr *ve = s->u.assign.value;
                    if (ve && ve->kind == EX_IDENT) {
                        const char *vn = planCName(ve) ? planCName(ve) : ve->u.ident.name;
                        for (int i = 0; i < g->rcFreshN; i++)
                            if (vn && strcmp(g->rcFreshName[i], vn) == 0) vlvl = g->rcFreshLvl[i];
                    }
                    if (getenv("EXTC_RC_DBG_LEVEL")) {
                        int plvl = planArenaLevel(ve);
                        fprintf(stderr, "[rclevel] host=%s hlvl=%d planLvl=%d eqPlan=%d freshLvl=%d eqFresh=%d\n",
                                hreg, hlvl, plvl, hlvl == plvl, vlvl, vlvl == hlvl);
                    }
                    if (g->rcNoCount && vlvl != -2 && vlvl == hlvl)
                        cgLine(g, "extc_rc_storeRefSameRegion((void **)&(%s), (void *)(%s));", tgt, val);
                    else
                        cgLine(g, "extc_rc_storeRefOwnedReg((void **)&(%s), (void *)(%s), %s);",
                               tgt, val, hreg);
                    return;
                }
                /* Overwriting a reference slot of this frame (`hostBase == NULL`: the slot of a local or
                 * parameter, not the field of a heap object): whether it is worth a point-fix release is
                 * left to the four criteria of `rcEmitStoreRelease`; if none applies, the usual path is
                 * taken. */
                if (hostBase == NULL && s->u.assign.target &&
                    s->u.assign.target->kind == EX_IDENT) {
                    const char *atn = planCName(s->u.assign.target)
                                          ? planCName(s->u.assign.target)
                                          : s->u.assign.target->u.ident.name;
                    if (rcEmitStoreRelease(g, tgt, atn, val, s->u.assign.value)) return;
                }
                /* A local or parameter slot (`hostBase == NULL` => `owner` is `"0"`): in the no-count tier the
                 * gated block of `storeRefOwned` does nothing for `so == 0` => emit the write-only entry
                 * point. Placed after `rcEmitStoreRelease`: the point-fix release path goes first,
                 * semantics unchanged. */
                if (g->rcLocalStore && g->rcNoCount && hostBase == NULL) {
                    cgLine(g, "extc_rc_storeRefLocal((void **)&(%s), (void *)(%s));", tgt, val);
                    return;
                }
                cgLine(g, "extc_rc_storeRefOwned((void **)&(%s), (void *)(%s), (void *)(%s));",
                       tgt, val, owner);
                return;
            }
            if (valueStore) {
                Type *tt = subst(g, rawTT);
                const char *ct = cType(g, tt);
                const int id = g->tmpSeq++;
                const char *dv = arenaPrintf(g->arena, "__extc_svd%d", id);
                const char *sv = arenaPrintf(g->arena, "__extc_svv%d", id);
                if (valScan == RC_CONSERVATIVE) {
                    /* The leaves cannot be enumerated: copy the whole block first (semantics), then count each word
                     * that is a live slot once, and register it on the target region or make that region
                     * drop out of sweeping (see `extc_rc_storeStructCons`). Only a host whose region was
                     * **proved** goes through registration; the local or static slot tier has no source
                     * to withdraw from, and a local of a conservative type cannot even be enumerated by
                     * the root table => take the conservative side (the target region drops out of
                     * sweeping). */
                    cgLine(g, "{ %s *%s = &(%s); %s %s = %s; *%s = %s;"
                              " extc_rc_storeStructCons((void *)%s, sizeof *%s, (void *)(%s), %d); }",
                           ct, dv, tgt, ct, sv, val, dv, sv, dv, dv, owner, hostProven ? 1 : 0);
                    return;
                }
                /* Enumerable: evaluate the target and the value once each (`genExpr` runs only once even with side
                 * effects), then apply the overwrite semantics of a reference slot leaf by leaf, and
                 * finally copy the whole block to fill in the non-reference fields. */
                cgLine(g, "{ %s *%s = &(%s); %s %s = %s;", ct, dv, tgt, ct, sv, val);
                cgValueRefStore(g, tt, arenaPrintf(g->arena, "(*%s)", dv), sv,
                                false, owner, hostUnknown);
                cgLine(g, "*%s = %s; }", dv, sv);
                return;
            }
            cgLine(g, "%s = %s;", tgt, asF32(g, s->u.assign.target ? s->u.assign.target->type : NULL,
                                              s->u.assign.value, val));
            return;

        case ST_IF: {
            const char *cnd = cgCond(g, genExpr(g, s->u.ifs.cond));
            flushPrefix(g);
            cgLine(g, "if (%s) {", cnd);
            g->indent++;
            genBlockBody(g, s->u.ifs.thenBody);
            g->indent--;
            if (!s->u.ifs.elseBody) {
                cgLine(g, "}");
                return;
            }
            cgLine(g, "} else {");
            g->indent++;
            if (s->u.ifs.elseBody->kind == ST_BLOCK) genBlockBody(g, s->u.ifs.elseBody);
            else                                     genStmt(g, s->u.ifs.elseBody);
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_WHILE: stWhile(g, s); return;

        case ST_YIELD: {
            /* `yield e`: store the return slot and the pc, then return. The `case` label right after
             * the `return` is where a resume jumps back in -- a label inside whatever block the
             * `yield` sits in, which is legal C11 (the shape is Duff's device). */
            const int k = ++g->coroYieldSeq;
            const char *v = s->u.yield_.value ? genExpr(g, s->u.yield_.value) : "0";
            flushPrefix(g);
            cgLine(g, "%s->ret = %s;", g->coroFrame, v);
            cgLine(g, "%s->pc = %d;", g->coroFrame, k);
            cgLine(g, "return true;");
            cgLine(g, "case %d: ;", k);
            if (s->u.yield_.bindCName)      /* `var x = yield e`: the resume's value lands here */
                cgLine(g, "%s->%s = %s->in;", g->coroFrame, s->u.yield_.bindCName, g->coroFrame);
            return;
        }
        case ST_RETURN: {
            /* Every return goes through the shared epilogue: only
             * `__extc_ret_v = ...; goto __extc_ret;` is emitted here, and the
             * release sequence appears once, at the end of the function. */
            if (!s->u.ret.value) {
                if (g->coroFunc) {
                    /* Finishing early: a pc no `case` matches, so the next resume falls through to
                     * the end of the step and reports "done". */
                    flushPrefix(g);
                    cgCoroArenaDestroy(g);      /* finished here: give the frame arenas back */
                    cgLine(g, "%s->pc = -1;", g->coroFrame);
                    cgLine(g, "return false;");
                    return;
                }
                cgReturn(g, NULL);           /* shared epilogue */
                return;
            }
            if (s->u.ret.value->kind == EX_TRY) {
                /* `return e?`: on success the payload is wrapped in the
                 * enclosing return type. */
                TryInfo ti = genTryHead(g, s->u.ret.value);
                Type *rt = subst(g, g->retType);
                Buf rb;
                bufInit(&rb, g->arena);
                bufPrintf(&rb, "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s } }",
                          cType(g, rt), rt->name, ti.okVar, ti.okVar,
                          tryPayloadPath(g, &ti));
                cgReturn(g, bufCstr(&rb));
                return;
            }
            const char *v = genTopExpr(g, s->u.ret.value);
            flushPrefix(g);              /* must precede the return */
            /* A plain local handed back is **moved out**: it is dead on the way out, so marking it
             * here keeps `cgReturn`'s drop sweep from releasing the state the caller now owns.
             * The name recorded is the source spelling; the sweep compares that too, because
             * inside a generic instance the generated name carries the instance suffix. */
            g->dropSkipName = (s->u.ret.value->kind == EX_IDENT) ? s->u.ret.value->u.ident.name : NULL;
            cgReturn(g, v);
            g->dropSkipName = NULL;
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            /* `break` and `continue` leave these block levels behind, so the
             * arenas are released first, down to the level of the loop body.
             * The body's own arena is cleared when the next iteration enters
             * it. */
            /* Locals of the blocks being left run their hooks first: the hook may still need the
             * arena the slot lives in, which is released two lines down. */
            if (g->dropLoopBases.len > 0) genDropBlocksDownTo(g, *(size_t *)vecAt(&g->dropLoopBases,
                                                                                g->dropLoopBases.len - 1));
            int to = g->loopLen ? g->loopLevel[g->loopLen - 1] : 1;
            for (int lv = g->blkLevel; lv >= to; lv--) cgReleaseLevel(g, lv);
            if (s->kind == ST_BREAK) { cgLine(g, "break;"); return; }
            /* `continue` in a `for` must still run the step (C's `for` does); the desugared
             * `while` has it at the end of the body, behind a label. */
            int step = g->loopLen ? g->loopStep[g->loopLen - 1] : 0;
            if (step) {
                g->loopStepUsed[g->loopLen - 1] = true;
                cgLine(g, "goto __extc_forstep_%d;", step);
            }
            else      cgLine(g, "continue;");
            return;
        }

        case ST_EXPR:
            /* `f()?` as a statement of its own needs only the two lines that
             * evaluate it once and return on failure; nothing reads the
             * payload. */
            if (s->u.expr.expr->kind == EX_TRY) {
                (void)genTryHead(g, s->u.expr.expr);
                return;
            }
            const char *ex = genTopExpr(g, s->u.expr.expr);
            flushPrefix(g);
            /* A discarded **unwrap** (`f()!` as a statement) is the one shape that needs `(void)`:
             * it lowers to a union field read (`(g()).u.success._0;`), which gcc reports as
             * `unused-value` -- an error under `-Werror`. The parser builds `x!` as an `EX_SIGN`
             * node, the same node the prefix `!x` uses, distinguished only by position; here the
             * **type** tells them apart: a negation always yields `bool`, an unwrap never does.
             * Wrapping every discarded value instead would rewrite most generated files for
             * nothing. */
            Type *xt = s->u.expr.expr->type;
            bool isBool = xt && xt->kind == TY_BUILTIN && xt->name && strcmp(xt->name, "bool") == 0;
            bool unwrap = s->u.expr.expr->kind == EX_SIGN && xt && xt->kind != TY_VOID && !isBool;
            cgLine(g, unwrap ? "(void)(%s);" : "%s;", ex);
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_MATCH: stMatch(g, s); return;
    }
}

/* ---------------------------------------------------------------- top level */

/**
 * @brief Assemble a function's C parameter list.
 *
 * A function whose allocations may escape takes one extra hidden arena pointer at the end: its `new`
 * allocates into the arena the caller chose. An `@overwrite` cell is passed in the same way, as an
 * opaque `extc_owcell *`, so the caller need not know the type of the callee's site and a generic
 * instance needs no special case. `main`'s C signature is fixed and takes no hidden parameter; a
 * parameter written in the language (`main(args)`) is not a C parameter either.
 *
 * The implicit parameters are emitted **once**, in `cgImplicitArgs` (one each for `usesHome` and
 * `makesPool`); adding another here would give a function that only creates pools one parameter too
 * many.
 *
 * @param g  The code generation state.
 * @param f  The function definition whose parameter list is assembled.
 * @return   The parameter-list text, without the enclosing parentheses.
 */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* C fixes the signature of `main`, so it takes no hidden parameter; its
     * home arena is the local `extc_arena *__extc_home = &__extc_a[1]` in its
     * own body. A parameter the language wrote is not a C parameter either:
     * `main(args)` is built from `argc`/`argv` in the prologue, so C still sees
     * exactly the two arguments it insists on. */
    if (!f->owner && (strcmp(f->name, "main") == 0 || f->isModuleEntry)) {
        /* with a dispatcher, every entry point takes the same two C parameters: the table stores one kind
     * of function pointer */
        bufPuts(&sig, (g->needModuleTramp || f->params.len) ? "int argc, char **argv" : "void");
        return bufCstr(&sig);
    }
    if (f->params.len == 0 && !planUsesHome(f) && !planMakesPool(f) && planOwLocal(f)) {   /* no hidden parameters follow */
        bufPuts(&sig, "void"); return bufCstr(&sig);
    }
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (i) bufPuts(&sig, ", ");
        bufPrintf(&sig, "%s %s", cType(g, p->type), p->cname ? p->cname : p->name);
    }
    cgImplicitArgs(g, &sig, f, NULL, f->params.len, true);   /* declaration spelling, the same owner */
    /* A function that creates pools takes one extra hidden "home zone" -- the mirror of the hidden
     * arena parameter (`makesPool` is a transitive least fixed point, so every function along the chain
     * receives it). Where a container is "born" is therefore decided by the **caller**, not by the
     * lexical block inside the library. */
    /* Note: do **not** add `makesPool` again here -- the `cgImplicitArgs` call above already emitted
     * both kinds of implicit parameter (one each for `usesHome` and `makesPool`); one more here is "a
     * function that only creates pools gets one parameter too many" and the output becomes invalid
     * C. */
    /* @overwrite cells live in the frame of the call site and are passed in as
     * opaque `extc_owcell *`: opaque means a caller does not need to know the
     * types of the callee's sites, and a generic instance needs no special
     * case. */
    if (!planOwLocal(f)) {
        for (int i = 0; i < planOwSites(f); i++) {
            if (f->params.len || planUsesHome(f) || planMakesPool(f) || i) bufPuts(&sig, ", ");
            bufPrintf(&sig, "extc_owcell *__extc_owarg%d", i);
        }
    }
    return bufCstr(&sig);
}

/**
 * @brief Does a statement **certainly** return?
 *
 * Only the simplest shapes are recognized: `return` itself, or a block whose last statement has such
 * a shape.
 *
 * @param s  The statement; false when it is NULL.
 * @return   true when it certainly returns; anything uncertain is false. This direction is deliberate:
 *           a missed judgment only emits one extra `return` (dead code that still compiles), while the
 *           opposite would leave a missing return that gcc rejects.
 */
static bool stmtIsDefiniteReturn(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_RETURN) return true;
    if (s->kind == ST_BLOCK && s->u.block.stmts.len > 0)
        return stmtIsDefiniteReturn(*(Stmt **)vecAt(&s->u.block.stmts,
                                                    s->u.block.stmts.len - 1));
    return false;
}

/* Does this function reach itself, directly or through other calls?
 *
 * The answer decides whether a recursion guard is emitted. Unguarded runaway
 * recursion is silent: gcc either folds it into an endless loop that prints
 * nothing, or the stack overflows and the OS reports a segmentation fault,
 * which is neither an extC message nor a position in the source. Only a
 * self-recursive function pays for the guard, so an ordinary call costs one
 * increment and nothing else.
 *
 * The reachability uses the call graph the checker already recorded in
 * `planCallee(e)`, so no second AST walker is needed, and it is transitive: in the
 * mutual recursion `g -> h -> g`, `g` counts as self-recursive. That shape is the
 * one the guard is for: unguarded, it would run for 60 seconds without printing
 * anything.
 *
 * Returns:
 *   true when some path through the call graph leads back to f.
 *
 * Notes:
 *   - The graph is small and the answer is computed once per generated function,
 *     so the plainest fixed-point iteration is good enough.
 */
/* Is `f` reachable from itself? The walk itself is fine; the *dedup* is what a linear scan
 * would make cubic: scanning `seen` for every edge makes the walk from a function that reaches
 * k others cost O(k^2), paid once per emitted function. A pointer-keyed set answers the same
 * question with the same result -- only the lookup changes, never the verdict. */
typedef struct { FuncDef **slot; size_t cap, len; } FuncSet;

/**
 * @brief Initialize an empty function-pointer set, taking the slot array from an arena.
 *
 * The capacity is fixed at 64 and must be a power of two: both lookup and insertion locate a slot by
 * masking with `cap - 1`. The set itself is never freed; the old slot array replaced by a growth
 * stays in the same arena.
 *
 * @param s  The set to initialize; its previous contents are overwritten.
 * @param a  The arena the slot array is allocated from.
 */
static void funcSetInit(FuncSet *s, Arena *a) {
    s->cap = 64; s->len = 0;
    s->slot = arenaAllocZero(a, s->cap * sizeof *s->slot);
}
/**
 * @brief Hash a function pointer into a hash value for the set.
 *
 * Pointers are 16-byte aligned, so the low four bits that are always zero are shifted away first and
 * the result is multiplied by an odd number to spread it.
 *
 * @param p  The function definition pointer.
 * @return   The hash value; the caller masks it with `cap - 1` into a slot subscript.
 */
static size_t funcSetHash(FuncDef *p) { return ((size_t)(uintptr_t)p >> 4) * 2654435761u; }
/**
 * @brief Is a function pointer already in the set?
 *
 * Open addressing with linear probing: reaching an empty slot means it is absent. The premise is that
 * `s` was initialized by `funcSetInit` and that the table always keeps an empty slot -- `funcSetAdd`
 * grows it once the load factor reaches one half.
 *
 * @param s  The already initialized set.
 * @param p  The function pointer to find.
 * @return   true when it is in the set, false otherwise.
 */
static bool funcSetHas(FuncSet *s, FuncDef *p) {
    for (size_t i = funcSetHash(p) & (s->cap - 1); ; i = (i + 1) & (s->cap - 1)) {
        if (!s->slot[i]) return false;
        if (s->slot[i] == p) return true;
    }
}
/**
 * @brief Double the set's capacity and rehash every element into the new slot array.
 *
 * @param s  The already initialized set; its capacity and slot array are updated in place.
 * @param a  The arena the new slot array is allocated from; the old array is not reclaimed.
 */
static void funcSetGrow(FuncSet *s, Arena *a) {
    FuncSet bigger;
    funcSetInit(&bigger, a);
    bigger.cap = s->cap * 2;
    bigger.slot = arenaAllocZero(a, bigger.cap * sizeof *bigger.slot);
    for (size_t i = 0; i < s->cap; i++) {
        if (!s->slot[i]) continue;
        FuncDef *p = s->slot[i];
        for (size_t j = funcSetHash(p) & (bigger.cap - 1); ; j = (j + 1) & (bigger.cap - 1))
            if (!bigger.slot[j]) { bigger.slot[j] = p; bigger.len++; break; }
    }
    *s = bigger;
}
/**
 * @brief Add a function pointer to the set; when it is already there, do nothing.
 *
 * The capacity is doubled first once the set is half full, so probing always meets an empty slot.
 *
 * @param s  The already initialized set.
 * @param a  The arena used when growing.
 * @param p  The function pointer to add.
 */
static void funcSetAdd(FuncSet *s, Arena *a, FuncDef *p) {
    if (s->len * 2 >= s->cap) funcSetGrow(s, a);
    for (size_t i = funcSetHash(p) & (s->cap - 1); ; i = (i + 1) & (s->cap - 1))
        if (!s->slot[i]) { s->slot[i] = p; s->len++; return; }
        else if (s->slot[i] == p) return;
}

/* "Does f reach itself?" = "is f on a cycle of the call graph" -- a static property, but asking
 * it per emitted function over that function's whole reachable set is O(N^2). One DFS over the
 * graph marks every function that lies on a cycle -- a back edge to a still-grey node marks
 * exactly the stack slice from that node up -- and each query is then a lookup. Colours persist
 * across roots, which is sound for cycle detection: a cycle either was found from an earlier
 * root or is still all white. */
static FuncSet g_cycSeen, g_cycOn, g_cycGrey, g_cycBlack;
static int    g_cycReady;
static Vec    g_cycStack;

/**
 * @brief Depth-first walk of the call graph from `f`, marking the functions on a cycle in `g_cycOn`.
 *
 * Grey means still on the current recursion stack, black means finished. An edge to a grey node is a
 * back edge, and then the whole span of `g_cycStack` from that node up to the top is marked "on a
 * cycle". Colors are kept across root nodes, which is safe for cycle detection: a cycle is either
 * already found by an earlier root or all its nodes are still white. `g_cycStack` must already be
 * initialized by `cycleSaysRecursive`.
 *
 * @param g  The code generation context, which also provides the arena.
 * @param f  The walk's starting point; returns immediately when it is NULL or has no function body
 *           (an extern declaration).
 */
static void cycDfs(CG *g, FuncDef *f) {
    if (!f || !f->body) return;
    if (funcSetHas(&g_cycSeen, f)) return;
    funcSetAdd(&g_cycSeen, g->arena, f);
    funcSetAdd(&g_cycGrey, g->arena, f);
    *(FuncDef **)vecPush(&g_cycStack) = f;
    for (size_t j = 0; j < f->callees.len; j++) {
        FuncDef *nx = *(FuncDef **)vecAt(&f->callees, j);
        if (!nx) continue;
        if (funcSetHas(&g_cycBlack, nx)) {
            continue;                                        /* black: finished, never a back edge */
        } else if (funcSetHas(&g_cycGrey, nx)) {              /* grey ancestor: back edge */
            size_t at = 0;
            for (size_t k = 0; k < g_cycStack.len; k++)
                if (*(FuncDef **)vecAt(&g_cycStack, k) == nx) { at = k; break; }
            for (size_t k = at; k < g_cycStack.len; k++)
                funcSetAdd(&g_cycOn, g->arena, *(FuncDef **)vecAt(&g_cycStack, k));
        } else {
            cycDfs(g, nx);
        }
    }
    g_cycStack.len--;                                        /* pop */
    funcSetAdd(&g_cycBlack, g->arena, f);                     /* black: finished, never a back edge */
}

/**
 * @brief Is a function on some cycle of the call graph, that is, directly or indirectly recursive?
 *
 * The first call initializes the four sets and the walk stack; afterwards an unseen `f` gets one walk
 * from itself, and the query itself only reads `g_cycOn`. The state lives in file-level statics, all
 * allocated from `g->arena`.
 *
 * @param g  The code generation context, which also provides the arena.
 * @param f  The function to query.
 * @return   true when it is on a cycle, false otherwise.
 */
static bool cycleSaysRecursive(CG *g, FuncDef *f) {
    if (!g_cycReady) {
        g_cycReady = 1;
        funcSetInit(&g_cycSeen, g->arena);
        funcSetInit(&g_cycOn, g->arena);
        funcSetInit(&g_cycGrey, g->arena);
        funcSetInit(&g_cycBlack, g->arena);
        vecInit(&g_cycStack, g->arena, sizeof(FuncDef *));
    }
    if (!funcSetHas(&g_cycSeen, f)) cycDfs(g, f);
    return funcSetHas(&g_cycOn, f);
}

/**
 * @brief Does a function directly or indirectly call itself?
 *
 * The answer decides whether a recursion-depth guard is emitted. The decision only uses the call
 * graph already recorded by the checker (`f->callees`) instead of walking the AST again, and it is
 * transitive: in the mutual recursion `g -> h -> g`, both `g` and `h` count.
 *
 * @param g  The code generation context.
 * @param f  The function to judge.
 * @return   true when a path leads back to itself; false when `f` is NULL or has no function body.
 */
static bool funcCallsItself(CG *g, FuncDef *f) {
    if (!f || !f->body) return false;
    /* One DFS from `f` covers its whole reachable subgraph: a cycle through `f` needs no second walk. */
    return cycleSaysRecursive(g, f);
}

/* Emit one complete function definition.
 *
 * The prologue creates the block arena array, the `@overwrite` cells, the slot
 * the value is returned through and the recursion guard; then the body is
 * generated; and finally a single shared epilogue releases every arena level and
 * returns. Every piece of per-function state it changes is saved and restored,
 * because the instances of a generic generate several functions in a row.
 *
 * Params:
 *   f - the function to emit
 */
/* Every coroutine in the module declares its step **before** the bodies: a body may drive one, and
 * `main` is emitted before the other bodies. The frame itself comes from the type channel (it is a
 * unit), and the step's definition comes from `genFunc` like any other function -- which is what
 * registers the generic instances its body calls. */
/* The handle type and its two helpers. Emitted only when a handle exists somewhere; the helpers are
 * EXTC_UNUSED so that a `value` helper for an unused yield type stays quiet under -Werror. */
/**
 * @brief Do a set of type arguments still mention a type parameter, that is, is the instance not yet
 *        concrete?
 *
 * The rule matches the checker's test for a provisional instance: as soon as one argument mentions a
 * type parameter, this instance (`gen_T`) is not emitted.
 *
 * @param targs  The type argument list; treated as containing no type parameter when NULL.
 * @return       true when some argument mentions a type parameter, false otherwise.
 */
static bool coroProvisional(Vec *targs) {
    if (!targs) return false;
    for (size_t i = 0; i < targs->len; i++)
        if (mentionsParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/**
 * @brief Will a coroutine really be emitted as a C function?
 *
 * Only a concrete instance is emitted: a generic template that still has type parameters is not, and
 * neither is a provisional instance (`gen_T`) whose type arguments still mention a type parameter. An
 * instance must be emitted -- what `genFunc` generates is the instance, and the handle dispatch table
 * dispatches on the `$step`/`$next` it generates.
 *
 * @param f  The candidate function.
 * @return   true when it is a coroutine and has become concrete; false when `f` is NULL, is not a
 *           coroutine, or is still a template.
 */
static bool coroEmitted(FuncDef *f) {
    if (!f || !planIsCoro(f)) return false;
    if (!planTemplate(f)) return f->typeParams.len == 0;
    return planTemplate(f)->typeParams.len == 0 ? true : !coroProvisional(&f->targs);
}

/**
 * @brief Emit the forward declarations and the two families of helper functions that a coroutine
 *        handle type needs.
 *
 * Nothing is emitted when `g->needCoroHandle` is false. What is emitted: the prototype of the task
 * liveness query; the `$step` prototype of every emitted coroutine, plus a `$next` prototype when it
 * needs a zone; the shared `extc_trap_dead_handle` and the `extc_coro_next` dispatcher; and finally
 * one `value`/`send` helper per yield type (one only per distinct yield type). Driving a handle whose
 * task has finished goes through the trap instead of reading a freed frame.
 *
 * @param g  The code generation context.
 * @param m  The current module; the emitted coroutines are picked from `m->funcs`.
 */
static void genCoroHandleDecls(CG *g, Module *m) {
    if (!g->needCoroHandle) return;
    cgLine(g, "/* The coroutine **handle**: one struct for every `coroutine<T>`, which is what lets");
    cgLine(g, " * handles from different coroutines live in the same container. `task` is the safety");
    cgLine(g, " * token: driving a handle whose task has ended traps loudly instead of touching freed");
    cgLine(g, " * memory (the frame lives in that task's place). */");
    (void)0;   /* the typedef itself is emitted early, before every type that mentions it */
    /* The task table is spliced in during final assembly, so name what the helpers need here --
     * the same reason the pool primitives get a prototype in `coroutine.c`. */
    cgLine(g, "int64_t extc_task_alive(int64_t id);");
    /* C allows a declaration to be repeated, and these helpers may precede the definitions below. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf)) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);", cn, cn);
        if (planCoroNeedsZone(cf))
            cgLine(g, "EXTC_UNUSED static bool %s$next(struct %s$frame *f);", cn, cn);
        /* The domain's task table stores `bool (*)(void *)`, while `$step`/`$next` take a concrete
         * frame pointer -- calling one through the other's type is undefined behaviour, so a program
         * that starts tasks gets this typed adapter. Emitted **only** for coroutines that some `ext`
         * starts (`isExtTarget`, known at check time) => every other product is byte-identical. */
    }
    /* One trap for all three helpers: every trap in this project carries a source position, and a
     * handle outliving its task is the one thing a `coroutine<T>` can get wrong. */
    cgLine(g, "EXTC_UNUSED static void extc_trap_dead_handle(const char *f, int l) {");
    cgLine(g, "    fprintf(stderr, \"%%s:%%d: trap: driving a coroutine whose task has already\"");
    cgLine(g, "                    \" ended (its frame was released with the task)\\n\", f, l);");
    cgLine(g, "    exit(70);");
    cgLine(g, "}");
    cgLine(g, "EXTC_UNUSED static bool extc_coro_next(extc_coro *h, const char *f, int l) {");
    cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
    cgLine(g, "    switch (h->kind) {");
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf)) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "    case %d: return %s%s((struct %s$frame *)h->frame);",
               planCoroKind(cf), cn, planCoroNeedsZone(cf) ? "$next" : "$step", cn);
    }
    cgLine(g, "    }");
    cgLine(g, "    return false;");
    cgLine(g, "}");
    /* One `value` helper per yield type that a handle can carry. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf) || !planYieldType(cf)) continue;
        const char *yt = cType(g, planYieldType(cf));
        bool done = false;
        for (size_t k = 0; k < i && !done; k++) {
            FuncDef *prev = *(FuncDef **)vecAt(&m->funcs, k);
            if (coroEmitted(prev) && planYieldType(prev) &&
                strcmp(cType(g, planYieldType(prev)), yt) == 0) done = true;
        }
        if (done) continue;
        cgLine(g, "EXTC_UNUSED static %s extc_coro_value_%s(extc_coro *h, const char *f, int l) {",
               yt, yt);
        /* `value()` reads the frame's `ret`, so it needs the same liveness check as `next`: reading a
         * released frame is a use-after-free, not a stale number. */
        cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
        cgLine(g, "    switch (h->kind) {");
        for (size_t k = 0; k < m->funcs.len; k++) {
            FuncDef *ck2 = *(FuncDef **)vecAt(&m->funcs, k);
            if (!coroEmitted(ck2) || !planYieldType(ck2)) continue;
            if (strcmp(cType(g, planYieldType(ck2)), yt) != 0) continue;
            const char *cn2 = cFuncName(g, ck2);
            cgLine(g, "    case %d: return ((struct %s$frame *)h->frame)->ret;", planCoroKind(ck2), cn2);
        }
        cgLine(g, "    }");
        /* Unreachable in type-safe code (a handle's kind always matches its `T`); written as the
         * first case's expression so it needs no zero value of `T`. */
        cgLine(g, "    return ((struct %s$frame *)h->frame)->ret;", cFuncName(g, cf));
        cgLine(g, "}");
        /* `send`: hand a value to the resume (the frame's `in` slot), then drive it like `next`. */
        cgLine(g, "EXTC_UNUSED static bool extc_coro_send_%s(extc_coro *h, %s v, const char *f, int l) {",
               yt, yt);
        cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
        cgLine(g, "    switch (h->kind) {");
        for (size_t k = 0; k < m->funcs.len; k++) {
            FuncDef *ck3 = *(FuncDef **)vecAt(&m->funcs, k);
            if (!coroEmitted(ck3) || !planYieldType(ck3)) continue;
            if (strcmp(cType(g, planYieldType(ck3)), yt) != 0) continue;
            const char *cn3 = cFuncName(g, ck3);
            cgLine(g, "    case %d: ((struct %s$frame *)h->frame)->in = v; return %s%s("
                      "(struct %s$frame *)h->frame);",
                   planCoroKind(ck3), cn3, cn3, planCoroNeedsZone(ck3) ? "$next" : "$step", cn3);
        }
        cgLine(g, "    }");
        cgLine(g, "    return false;");
        cgLine(g, "}");
    }
}

/**
 * @brief Emit the coroutine runtime and the task table on demand, and declare every coroutine's step
 *        function.
 *
 * The task table is considered first: having any coroutine that needs a zone, or having `extc_task_*`
 * external functions declared on the extC side, both count as needing it -- and then the pool zone
 * runtime is pulled in along with it and the coroutine runtime is emitted. `genCoroHandleDecls` is
 * called next, then a `$step` prototype is emitted for every concrete coroutine; one that needs a
 * zone also gets its `$next` defined right there: it installs the frame's place as the current one
 * before entering the step, restores it afterwards, and frees the whole thing when the step reports
 * completion.
 *
 * @param g  The code generation context.
 * @param m  The current module.
 */
static void genCoroDecls(CG *g, Module *m) {
    bool taskTable = false;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *c0 = *(FuncDef **)vecAt(&m->funcs, i);
        if (c0 && planIsCoro(c0) && !planTemplate(c0) && planCoroNeedsZone(c0)) { taskTable = true; break; }
        /* A scheduler written in extC reaches the table through `std::sys::coroutine`; those
         * declarations are ordinary externs, so their presence is what pulls the runtime in. */
        if (c0 && c0->isExtern && c0->name && strncmp(c0->name, "extc_task_", 10) == 0) {
            taskTable = true; break;
        }
    }
    /* The task table, defined before every body that may spawn or drive: the pool prototypes are
     * already in the prototype region above, so this only needs to precede its users. */
    g->taskTableDone = taskTable;
    if (taskTable) {
        /* The table sits on the pool zone runtime (enter/leave), so a program that only spawns
         * coroutines has to pull that in too -- the same rule the zone-argument fallback follows. */
        g->needPool = true;
        coroutineEmitRuntime(g->arena, g->out);
    }
    g->coroDeclsDone = g->needCoroHandle;
    genCoroHandleDecls(g, m);
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* Instances emit too: skipping `planTemplate(f)` here would leave a generic coroutine
         * with no `$next` at all, and the handle dispatcher would then call `$step` directly,
         * which neither makes the task's place current nor ends the task. A *template* still has
         * type parameters left, which is what the second test excludes. */
        if (!f || !planIsCoro(f) || f->typeParams.len > 0) continue;
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);",
               cFuncName(g, f), cFuncName(g, f));
        /* The **task driver**, defined here (before every body that may drive it): the place is read
         * from the frame -- never re-derived at a resume (rule 1) -- made current for the step, and
         * restored afterwards so the caller's place is untouched. When the step reports "done", the
         * task's whole place goes in one release: the author's B decision for slice C. */
        if (planCoroNeedsZone(f)) {
            cgLine(g, "static bool %s$next(struct %s$frame *f) {", cFuncName(g, f), cFuncName(g, f));
            cgLine(g, "    int64_t __sv = extc_zoneTop;");
            cgLine(g, "    extc_zoneTop = f->zone;");
            cgLine(g, "    bool __r = %s$step(f);", cFuncName(g, f));
            cgLine(g, "    extc_zoneTop = __sv;");
            cgLine(g, "    if (!__r) extc_task_end(f->task);   /* ran to completion: same release as a drop */");
            cgLine(g, "    return __r;");
            cgLine(g, "}");
        }
    }
}

#include <time.h>

/* Sub-phase timers for codegen, off unless EXTC_DBG_TIME=1. The front-end timers say codegen
 * is 91% of a big build; these say which part of it, because the profile's leaf shares (vecAt
 * 60%) do not name the loop behind the calls. Accumulated per name and reported once. */
/**
 * @brief Is codegen sub-phase timing enabled?
 *
 * @return   true when `EXTC_DBG_TIME` is set; the environment variable is read once and cached.
 */
static int cgTimeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }
/**
 * @brief Read the current clock for timing.
 *
 * @return   Process CPU time in seconds.
 */
static double cgNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
typedef struct { const char *name; double sec; } CgAcc;
static CgAcc cgAccs[8];
/**
 * @brief Add one sub-phase duration to the timing table indexed by name.
 *
 * Returns immediately when `EXTC_DBG_TIME` is off. The table holds at most 8 entries; a new name takes
 * the first empty slot and is dropped when there is none. The table stores only the `name` pointer
 * without copying the string, so call sites pass literals.
 *
 * @param name  The phase name.
 * @param sec   This duration in seconds.
 */
static void cgAcc(const char *name, double sec) {
    if (!cgTimeOn()) return;
    for (size_t i = 0; i < 8; i++) {
        if (!cgAccs[i].name) { cgAccs[i].name = name; cgAccs[i].sec = sec; return; }
        if (strcmp(cgAccs[i].name, name) == 0) { cgAccs[i].sec += sec; return; }
    }
}
#define CGACC(name, call) do { double t_ = cgNow(); call; cgAcc((name), cgNow() - t_); } while (0)
static void strReport(void);   /* defined with the scan counters, below */
/* What dropUnusedLocals actually does per local, in bytes: the pass is still x4 per doubling and
 * the per-local work is all O(body), which should be linear -- so one of these four counters must
 * be growing with the program. */
static long g_locN, g_locCopy, g_locRead, g_locLine, g_cuts;
/**
 * @brief Print the local-variable scan counters to stderr.
 *
 * Does nothing when `EXTC_DBG_TIME` is off.
 */
static void locReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[locals] n=%ld copy=%lld reads=%lld lines=%lld cuts=%ld\n",
            g_locN, (long long)g_locCopy, (long long)g_locRead, (long long)g_locLine, g_cuts);
}
/**
 * @brief Print the code generation timing and scan counter report.
 *
 * Does nothing when `EXTC_DBG_TIME` is off. It prints the string scan counters, the local variable
 * counters, and the accumulated duration of every sub-phase in turn.
 */
static void cgReport(void) {
    if (!cgTimeOn()) return;
    strReport(); locReport();
    for (size_t i = 0; i < 8 && cgAccs[i].name; i++)
        fprintf(stderr, "[time] %-9s %.3f s\n", cgAccs[i].name, cgAccs[i].sec);
}

/**
 * @brief Emit one complete function definition: prologue, body and shared epilogue.
 *
 * The prologue prepares the block arena array, the `@overwrite` storage cells, the slot the return
 * value passes through and the recursion guard; the body is generated next, and finally the same
 * epilogue releases every arena level and returns. Every piece of per-function state it touches is
 * saved on entry and restored on exit, because one generic generates several functions in a row. The
 * entry has three shapes: `main` uses C's fixed signature (`argc`/`argv` wrapped into the
 * language-declared view in the prologue), a coroutine step is emitted as `$step`, and everything else
 * is an ordinary function.
 *
 * @param g  The code generation context.
 * @param f  The function to emit.
 */
static void genFuncInner(CG *g, FuncDef *f);

/**
 * @brief Emit one function definition, with `g->path` set to the file the body was written in.
 *
 * Every run-time check the generated code can raise prints a `file:line` pair. The line travels
 * with the checked node, so the file has to travel with the function too: a check emitted inside a
 * library helper must name the library file, not whichever entry file happened to include it.
 * Restoring the entry path afterwards keeps the rest of the emission (whole-program thunks and
 * tables) addressing the program the user compiled.
 *
 * @param g  Code generator.
 * @param f  Function to emit; its `srcPath` may be NULL for compiler-synthesised functions.
 */
static void genFunc(CG *g, FuncDef *f) {
    const char *saved = g->path;
    if (f->srcPath) g->path = f->srcPath;
    genFuncInner(g, f);
    g->path = saved;
}

/**
 * @brief Body of `genFunc`: the emitter proper.
 *
 * @param g  Code generator.
 * @param f  Function to emit.
 */
static void genFuncInner(CG *g, FuncDef *f) {
    /* The coroutine protocols (`next`/`value`) have no body: they are emitted inline at their call
     * sites (see genMethodCall). Never emit one as a function, whichever path got here. */
    if (planCoroProto(f)) return;
    /* A coroutine's step is generated **through this flow**:
     * emitting a body is what registers the generic instances it calls, and a definition produced
     * outside this flow called helpers nothing ever emitted. Only two places differ -- the signature
     * below and the ending at the epilogue. */
    if (planIsCoro(f)) {
        g->coroFunc     = f;
        g->coroFrame    = "f";
        g->coroYieldSeq = 0;
    }
    bool isMain = cgIsMain(f);
    /* An entry point: `main`, or a function marked `@module`. C (or the dispatcher, when there is one)
     * hands the command line to both, and both return `int`; with a dispatcher the real `main` is also
     * renamed to a static helper and C's `main` is the dispatcher. */
    bool isEntry = isMain || (f && f->isModuleEntry);
    /* `main(args)`: the one parameter the language may declare is not a C
     * parameter (C fixes the entry signature), so `argc`/`argv` come in and the
     * view is built in the prologue. */
    bool mainArgs = isEntry && f->params.len > 0;
    if (isEntry) {
        /* C fixes the signature of `main`, so it takes no hidden parameter; its
         * home arena is one of its own block arenas. */
        cgLine(g, "%sint %s(%s) {", g->needModuleTramp ? "static " : "",
               cgEntryCName(g, f), cgParamList(g, f));
        if (g->needModuleTramp && !mainArgs) {
            cgLine(g, "    (void)argc;");   /* the dispatcher always passes two C parameters; whether they are used is this function's business */
            cgLine(g, "    (void)argv;");
        }
    } else if (g->coroFunc) {
        /* The step's signature: it takes **the frame**, not the coroutine's source parameters -- a
         * resume has no arguments to pass them again. `switch (f->pc)` is Duff's device: every `yield`
         * stores the pc and returns, and the `case` label right after it is where a resume jumps in. */
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f) {",
               cFuncName(g, (FuncDef *)g->coroFunc), cFuncName(g, (FuncDef *)g->coroFunc));
        cgLine(g, "    switch (f->pc) {");
        cgLine(g, "    case 0: ;");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        /* `@export`: external linkage, exactly its own name -- the same decision the prototype
         * makes (see `genFuncProto`), and the two have to agree or C sees two types. */
        bufPrintf(&sig, "%s%s %s(%s) {",
                  f->isExport ? "" : (f->isInline ? "EXTC_INLINE " : "static "),
                  cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    /* `?` needs the return type to build the value to return on failure. The
     * temporary counter restarts in every function, so each function has its own
     * `__extc_try0` and they cannot collide. */
    Type *savedRet = g->retType;
    int   savedSeq = g->tmpSeq;
    g->retType = subst(g, f->ret);
    g->inMain  = isEntry;
    /* The reclamation trigger only operates on a region this frame owns; which arena an `@overwrite` cell
     * lives in depends on whether this function receives the hidden "home arena" parameter (see the
     * note on the `rcFuncUsesHome` field). */
    g->rcFuncUsesHome = planUsesHome(f);
    g->tmpSeq = 0;
    /* `@unchecked` is a property of **this body**, so it is set on entry and restored on the way
     * out. A coroutine's step function is generated from its own `FuncDef` (the one the
     * annotation sits on), so the flag covers `yield`-split bodies as well. */
    bool savedUnchecked = g->uncheckedBody;
    g->uncheckedBody = f->isUnchecked;
    /* ---- One arena per block level ----
     * The number of levels is known at compile time, being the maximum nesting
     * depth of the blocks, so the array has a fixed length and lives on the
     * stack: setting up the arenas needs no allocation. `{0}` is enough, since
     * an `extc_arena` holds only a `top` pointer and NULL means empty. The
     * function body itself is level 1; genBlockBody increments on entry. */
    int maxLv = 1 + blkMaxOfBlock(f->body);
    /* One table slot per block level, exactly like `__extc_a[maxLv + 1]` below. Allocated per
     * function because the level count is a property of this body. */
    g->levelCap = maxLv + 2;
    g->zoneMark  = (bool *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(bool));
    g->loopLevel = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    g->loopStep  = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    g->loopStepUsed = (bool *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(bool));
    g->loopRootBase = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    /* A function that puts nothing into its own block arenas does not even emit
     * the array. The checker decides that (`planMayUseArena(f)`: the body contains a
     * `new`, or calls a function that has a home arena), and about half of the
     * functions in real programs are pure computation. cgReleaseLevel skips the
     * release for them as well. */
    bool savedNoArena = g->noArena;
    bool savedOwLocal = g->owLocalOwn;
    g->owLocalOwn = planOwLocal(f);
    /* The @overwrite storage cells, one per site, declared in the prologue.
     * The order matters: a cell is initialized with `&__extc_a[1]` (see `home`
     * below), so the list of sites has to be collected before the decision
     * whether to emit the arena array at all can be taken. */
    Vec savedOw = g->owSitesOwn;              /* by value: no arena exists for the first function */
    Vec owNow; vecInit(&owNow, g->arena, sizeof(Stmt *));
    collectOwSites(f->body, &owNow);
    g->owSitesOwn = owNow;
    Vec savedOwCalls = g->owCalls;
    Vec owcNow; vecInit(&owcNow, g->arena, sizeof(Expr *));
    collectOwCallsStmt(f->body, &owcNow);
    g->owCalls = owcNow;
    /* A cell is initialized with `&__extc_a[1]`, so the arena array must be emitted whenever
     * there is any cell, even in a function that allocates nothing. */
    g->noArena = !planMayUseArena(f) && !(owNow.len > 0 || owcNow.len > 0);
    /* A step owns no arena array: it is not a place, and what it allocates belongs to the task's own
     * place. Emitting one also made it unused, which
     * `-Wall -Werror` rejects. */
    if (g->coroFunc) g->noArena = true;
    /* `main(args)` builds the view of `argv` in `&__extc_a[1]`, so the arena
     * array is needed even when the body allocates nothing. A program that only
     * prints its arguments is exactly that case. */
    if (mainArgs) g->noArena = false;
    /* `main`'s home arena **is** `&__extc_a[1]` (emitted below): it has no caller to hand
     * one in. When `main` only passes that arena down, its body holds no `new` of its own and
     * `mayUseArena` is false while the prologue still points at the array -- so force it here.
     * Every other function receives `__extc_home` as a parameter and needs no array of its own. */
    if (isEntry && planUsesHome(f)) g->noArena = false;
    /* Same reason, for pools: the frame mark and the per-block release hooks hang off the
     * arena's lifetime, so a `main` that only *uses* a pool (it allocates nothing itself)
     * still needs the frame. Phase 1 keys pools to `main`'s frame; when `new (r) T[n]`
     * lands, every function that can create one gets the same treatment. */
    if (isEntry && g->needPool) g->noArena = false;
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    /* The frame mark: pools created while this frame runs are linked to it, so leaving
     * the frame drops them whatever their block did. Emitted where
     * the arena exists, because hanging pools off an arena level is what this frame does. */
    /* Is this function "a place": only functions in the entry file are, and library functions are
     * transparent with respect to place (`pool<T>::withCap` creates a pool that belongs to its caller's
     * place). The criterion is that neither name contains `$` -- the loader turns a module's functions
     * into `mod$name`, and a generic instance also keeps a `$`-carrying name in `instName`. */
    {
        bool lib = (f->name && strchr(f->name, '$')) ||
                   (planInstName(f) && strchr(planInstName(f), '$'));
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zone] name=%s mod=%s inst=%s owner=%s line=%d\n",
                    f->name ? f->name : "-", f->modName ? f->modName : "-",
                    planInstName(f) ? planInstName(f) : "-", f->owner ? "yes" : "-", f->line);
        /* "A place" = a function in the entry file. `modName` is the module name the loader gives: library
         * methods have it, the entry file's is empty, so library functions are transparent with respect
         * to place -- a pool created in `pool<T>::withCap` belongs to its caller's place. */
        (void)lib;
        g->zoneHere = g->needPool && (isEntry || !f->modName || !*f->modName);
        /* main's C signature is fixed and cannot take a hidden parameter => it uses the current place's
         * zone. The entry helper in dispatcher mode also has a fixed C signature and follows the same
         * rule. */
        g->funcHasZoneParam = planMakesPool(f) && !isEntry;
    }
    /* The function body's zone (`__extc_zm1`) is emitted on demand for the same reason: without this
     * function, no pool would register on its frame, and pushing would push an empty zone anyway.
     * `zoneMark[1]` records that decision, and the closing `zoneLeaveTo(__extc_zm1)` consults it --
     * popping when nothing was pushed would pop **the caller's** zone. */
    g->zoneMark[1] = false;
    g->zoneFrameMarked = false;
    if (dbgOn("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonehook] %s zoneHere=%d noArena=%d makesPool=%d needPool=%d isMain=%d\n",
                f->name ? f->name : "-", (int)g->zoneHere, (int)g->noArena,
                (int)planMakesPool(f), (int)g->needPool, (int)isMain);
    /* A step is not a place of its own: the task's place (slice C) is what its storage hangs off, so
     * it must not open a zone of its own here -- that mark also went unused in the generated C. */
    if (g->zoneHere && !g->noArena && planMakesPool(f) && !g->coroFunc) {
        cgLine(g, "int64_t __extc_zm1 = extc_pool_zoneEnter();   /* the function body is one place */");
        g->zoneMark[1] = true;
        g->zoneFrameMarked = true;
    }
    /* ---- `main(args)`: wrap argc/argv into the view the language declared ----
     * The user wrote `fn main(args: slice<slice<u8>>) -> i32`; C hands in
     * `argc`/`argv`, so this is where the two meet. Three properties matter:
     *
     *   - the bytes are **not copied**. An `argv` string lives in the argument
     *     block of the process, which outlives every arena, so a view pointing
     *     at it is sound for as long as the program runs -- the same reason a
     *     string literal in read-only memory can be viewed.
     *   - the array of views does live in main's own home arena
     *     (`&__extc_a[1]`, the frame level), because it is main that owns it.
     *   - `args[0]` is the program name, exactly as C defines it, and the
     *     length is `argc`: no `+ 1` and no sentinel, so `args.len` is the
     *     truth. */
    if (mainArgs) {
        Param *p  = *(Param **)vecAt(&f->params, 0);
        Type  *pt = subst(g, p->type);                     /* slice<slice<u8>> */
        Type  *et = pt && pt->targs.len ? subst(g, *(Type **)vecAt(&pt->targs, 0)) : NULL;
        const char *at = cType(g, pt);
        const char *en = cType(g, et);                     /* slice<u8> */
        const char *nm = p->cname ? p->cname : p->name;
        cgLine(g, "%s %s;   /* built from argc/argv below */", at, nm);
        cgLine(g, "{");
        g->indent++;
        /* `self` often goes unused => mark it `(void)` explicitly: the receiver is part of the
         * **signature**, whether the body uses it is another matter, and the output should not
         * grow a warning over it. */
        if (f->params.len > 0 && strcmp((*(Param **)vecAt(&f->params, 0))->name, "self") == 0)
            cgLine(g, "(void)self;");
        cgLine(g, "int64_t __extc_argn = (int64_t)argc;");
        cgLine(g, "%s *__extc_argp = (%s *)extc_arena_alloc(&__extc_a[1],"
                   " (int64_t)(__extc_argn > 0 ? __extc_argn : 1) * (int64_t)sizeof(%s),"
                   " \"%s\", %d);", en, en, en, g->path, f->line);
        cgLine(g, "for (int64_t __extc_i = 0; __extc_i < __extc_argn; __extc_i++) {");
        g->indent++;
        cgLine(g, "const char *__extc_s = argv[__extc_i];");
        cgLine(g, "int64_t __extc_l = 0;");
        cgLine(g, "while (__extc_s[__extc_l]) __extc_l++;");
        cgLine(g, "__extc_argp[__extc_i].data = (uint8_t *)__extc_s;");
        cgLine(g, "__extc_argp[__extc_i].len = __extc_l;");
        g->indent--;
        cgLine(g, "}");
        cgLine(g, "%s.data = __extc_argp;", nm);
        cgLine(g, "%s.len = __extc_argn;", nm);
        g->indent--;
        cgLine(g, "}");
    }
    if (planOwLocal(f))
        for (size_t i = 0; i < owNow.len; i++)
            /* `home` says which arena the storage belongs to: with a home, the
             * home arena, which outlives this call and therefore really is
             * reused; otherwise level 1 of this frame, which certainly outlives
             * the statement. It is never NULL. */
            cgLine(g, "extc_owcell __extc_ow%zu = { 0, 0, %s };", i,
                   planUsesHome(f) ? "__extc_home" : "&__extc_a[1]");
    (void)owcNow;   /* no cells are prepared for a callee: they live in its frame */
    /* `owSites` must not be restored here: the body has not been generated yet,
     * and restoring it early made every lookup answer -1 and silently fall back
     * to allocating on every iteration. The restore happens at the end of
     * genFunc, together with `noArena`. */
    /* The slot the shared epilogue returns through; it is needed only when this
     * function really releases an arena on the way out. */
    bool retVoid = !g->retType || g->retType->kind == TY_VOID;
    /* `main` returns through it as well, and that is the point: its value is the process
     * exit code, so it has to survive the release sequence like any other return. The slot is
     * skipped only for `noArena`, where `cgReturn` returns the value directly and the
     * declaration would be unused. */
    bool retSlot = !g->noArena && !retVoid;
    if (retSlot)
        cgLine(g, "%s __extc_ret_v;", cType(g, g->retType));
    g->blkLevel = 0;
    g->loopLen  = 0;
    g->forStepSeq = 0;
    const char *savedFuncName = g->curFuncName;
    const FuncDef *savedFuncDef = g->curFunc;
    g->curFuncName = cFuncName(g, f);
    g->curFunc = f;
    g->rcArenaUsed = 0;
    g->rcRegN = 0;
    g->rcFreshN = 0;
    bool savedParTls = g->parTls;
    g->parTls = planParTlsArena(f);
    if (isEntry && planUsesHome(f)) {
        size_t hb = g->out->len;
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* the entry function's home arena is its own body */");
        g->rcArenaUsed |= 1u;   /* main's home arena is level 1: it can carry rc objects */
        localDef(g, hb, "__extc_home", 0);   /* the declaration sits above the body */
    }
    /* The buffered console tail has one guaranteed exit: `main` flushes on every explicit
     * `return` and in the arena epilogue, but a body that runs off its end passes through
     * neither. A trap exits from wherever it stands, and so does `sys::proc::exit`; one
     * `atexit` covers every way out, and the explicit flushes stay for the ordering they give
     * inside the program. Registered only when the program uses that console (`needCout`). */
    /* with a dispatcher, the dispatcher registers it: the entry helper is just a function that gets
     * called, and registering twice prints twice */
    if (isMain && g->needCout && !g->needModuleTramp) cgLine(g, "atexit(extc_cout_flush);");
    /* Registered after the flush, so atexit's LIFO order runs the hooks first and the flush still
     * gets the last word on anything they printed. */
    if (isMain && g->needGlobalDrops && !g->needModuleTramp) cgLine(g, "atexit(extc_drop_globals);");
    /* Only a self-recursive function needs the depth guard; see
     * funcCallsItself. Entering increments, every exit decrements, and
     * `extc_rec_enter` traps with a position once the limit is passed.
     *
     * The position reported is the line of the function declaration. The line of
     * the call site is what a user would rather see, but recursion comes back
     * around, so pointing at the declaration is the most readable choice. */
    g->isRecursive = funcCallsItself(g, f);
    if (g->isRecursive)
        cgLine(g, "extc_rec_enter(\"%s\", %d);", g->path, f->line);
    /* The owning depth of the thread-local arena (see `rcOwnKind`): a whole worker call chain
     * shares `extc_tls_arena`, and the entry frame reads 0 => it owns that region (its caller is
     * only the C runtime, so the static root table is the complete seed), while an inner frame only
     * records what it owes. The depth is maintained by each worker chain's prologue and epilogue;
     * trap/exit end the process outright and need no compensation. Frames with `noArena` take no
     * part (their epilogue emits no decrement and their prologue no increment). */
    bool tlsOwnerFrame = g->needReclaim && g->parTls && !g->noArena && !g->coroFunc;
    if (tlsOwnerFrame)
        cgLine(g, "EXTC_UNUSED int __extc_tlsOwner = (extc_rc_tlsDepth++ == 0);");
    /* The reclamation root table starts over per function: a parameter slot that contains a
     * reference is a root from the first line of the body. A coroutine step's only C parameter is
     * the frame (`f`): the parameters and locals that live across a `yield` live in frame fields
     * (`f->name`, see `exIdent`), so the reference leaves of those fields are registered as roots
     * using the `coroFrame` table the checker prepared. The frame is built by the caller as a
     * compound literal and zero-initialized at its starting point => an unassigned field reads 0, and
     * the root table entries pass `extc_rc_owns` in `markBegin` => a misjudgment only keeps more
     * alive. The checker guarantees that a yielded value carries no reference (the coroutine yield
     * type check in `src/check/check_top.c`) => the frame's references cannot escape past the yield
     * exit point into the caller's root table, and the seed is complete when the step sweeps only its
     * own block arena. */
    g->rcRoots.len = 0;
    g->rcRootsUnreliable = g->needReclaim && g->rcGlobalsUnreliable;
    if (g->needReclaim && !g->coroFunc) {
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            rcRootDeclImpl(g, p->type, p->cname ? p->cname : p->name, true);
        }
    } else if (g->needReclaim) {
        const FuncDef *cf = g->coroFunc;
        for (size_t i = 0; i < cf->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&cf->coroFrame, i);
            if (!p->cname) continue;
            rcRootDeclImpl(g, p->type,
                       arenaPrintf(g->arena, "%s->%s", g->coroFrame, p->cname), true);
        }
    }
    size_t mb = g->out->len;
    rcScanAssigned(g, f->body);   /* which locals were assigned to (the opposite side of the "never reassigned" judgment) */
    rcScanLinked(g, f->body);     /* which locals were linked into a heap object (the evidence set for criterion 4 of point-fix
     * release) */
    rcPredeclare(g, f->body);   /* region pointer locals: declared at the top of the function, filled at the use point */
    genBlockBody(g, f->body);
    if (g->coroFunc) {
        /* The step's ending, **after** the body: falling off it finishes the coroutine. The pc gets a
         * value no `case` matches, so a further resume falls through to the closing `return false;`.
         * The switch and the function close here; the epilogue below belongs to a normal function. */
        cgCoroArenaDestroy(g);          /* falling off the body finishes the coroutine */
        cgLine(g, "f->pc = %d;", g->coroYieldSeq + 1);
        cgLine(g, "return false;");
        cgLine(g, "    }");
        cgLine(g, "return false;");
        g->curFuncName = savedFuncName;
    g->curFunc = savedFuncDef;
    g->parTls = savedParTls;
        g->parTls = savedParTls;
        g->retType = savedRet;
        g->inMain  = false;
        g->rcFuncUsesHome = false;
        g->tmpSeq = savedSeq;
        g->noArena = savedNoArena;
        g->owSitesOwn = savedOw;
        g->owCalls = savedOwCalls;
        g->owLocalOwn = savedOwLocal;
        g->coroFunc  = NULL;
        g->coroFrame = NULL;
        g->indent--;
        cgLine(g, "}");
        return;
    }
    if (isMain) {
        g->mainFuncName = g->curFuncName;      /* the text is taken once the buffer is complete */
        g->mainOff = mb;
        g->mainLen = g->out->len - mb;
    }
    g->curFuncName = savedFuncName;
    g->curFunc = savedFuncDef;
    /* Falling off the end of the body is an exit too, so the depth is
     * decremented here as well; otherwise the depth would only grow and a
     * legitimate program would be reported as a stack overflow. A function with
     * `noArena` has no epilogue below, so it must return on its own here, or the
     * trailing return would never be reached and the counter would never come
     * down. */
    bool fallsOff = !(f->body && f->body->kind == ST_BLOCK && f->body->u.block.stmts.len > 0
                      && stmtIsDefiniteReturn(*(Stmt **)vecAt(&f->body->u.block.stmts,
                                                              f->body->u.block.stmts.len - 1)));
    if (g->isRecursive) {
        cgRecLeave(g);
        /* This return is emitted only when the body can fall off its end. When
         * the last statement already returns, it is unreachable, and in a
         * function with `noArena` it would name `__extc_ret_v`, which is not
         * declared there, so gcc reported an error; in every other case it is
         * dead code and is left out. */
        if (fallsOff && g->noArena && retVoid) cgLine(g, "return;");
        /* When a non-void `noArena` function falls off the end, `__extc_ret_v` does not exist there: the
         * `extc_trapMsg` below (noreturn) closes the function instead -- do not invent a
         * `return __extc_ret_v;`. */
    }
    /* A body that runs off its end must not fall through: a non-void one would return whatever
     * the register happened to hold, or -- with an arena -- the return slot that no return
     * statement ever wrote. clang reports that as `-Wreturn-type`, and one shape it cannot see
     * through is a `match` whose arms all return: the arms become an `if / else if` chain,
     * which says nothing about being exhaustive. `extc_trapMsg` is `noreturn`, so the C
     * compiler stops asking for a value there. */
    if (fallsOff && !retVoid) {
        /* `main` is the one function C gives an implicit `return 0` for, and the checker's rule
         * for it says as much ("the return type may be absent or `i32`"): trapping there would
         * kill a program every C programmer expects to exit 0. Still a trap everywhere else:
         * the value there is genuinely undefined. */
        if (isEntry) {
            /* Falling off the end of `main` is a normal exit too: the console buffer has to be flushed
             * here. This step matters especially in an **arena-less** main -- that shape emits no
             * epilogue (the `!g->noArena` branch), and without it "the program prints nothing". */
            if (g->needCout) cgLine(g, "extc_cout_flush();");
            cgLine(g, "return 0;");
        }
        else
            cgLine(g, "extc_trapMsg(\"%s\", %d, \"a non-void function reached its end without returning\");",
                   g->path, f->line);
    }
    /* The shared epilogue: the release sequence appears once, and falling off
     * the end of the body goes through it as well. The `goto` guarantees that
     * the label has a user, so there is no -Wunused-label warning.
     *
     * Every level is released, because a return may jump out of a deeper block;
     * releasing an already empty arena is a no-op. */
    /* A `noArena` main emits no epilogue, but the "falls off the end" exit must flush the console
     * buffer as well; emitted only when the function really can fall off (`fallsOff`), otherwise the
     * line sits after a `return` as dead code. */
    if (g->noArena && isEntry && fallsOff && g->needCout)
        cgLine(g, "extc_cout_flush();");
    if (!g->noArena) {
        /* The fall-through entry. A body that cannot run off its end reaches the label through
         * its own returns, so this line would be dead code - clang says `code will never be
         * executed` for it and for the release that follows. `fallsOff` is the same answer the
         * epilogue trap uses, and the label keeps its users either way: a body that cannot fall
         * off ends with a return, and a return jumps to this label. */
        if (fallsOff) cgLine(g, "goto __extc_ret;");
        g->indent--;
        cgLine(g, "__extc_ret:");
        g->indent++;
        genParamDrops(g, f);   /* a by-value parameter is the callee's copy: hand its storage back */
        if (tlsOwnerFrame) cgLine(g, "extc_rc_tlsDepth--;");   /* paired with the prologue's increment */
        /* `destroy`, not `release`: this is the frame's last exit, so the block the
         * arena kept for the next round has to go too. Emitting `release` here would
         * leak one block per arena at every return -- bounded, but a leak. */
        for (int lv = 1; lv <= maxLv; lv++) {
            cgReclaimDropArena(g, lv);   /* the reclamation region lives as long as the arena; early exits leave through here too */
            cgLine(g, "extc_arena_destroy(&__extc_a[%d]);", lv);
        }
        /* Pop back to the outermost zone that was pushed: early exits (`goto __extc_ret`) and reaching
         * the end of the function both leave through here, and a jump out may leave several block
         * zones on the stack, so each level is popped from the deepest one. Popping too much is
         * idempotent (`zoneLeaveTo` pops until a mark), popping too little leaves a pool on a level
         * that has already been popped. `zoneMark[1]` (the frame's own zone) is decided by the
         * prologue: nothing pushed, nothing popped. */
        for (int lv = maxLv; lv >= 1; lv--)
            if (lv < g->levelCap && g->zoneMark[lv])
                cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lv);
        if (isEntry) {
            if (g->needCout) cgLine(g, "extc_cout_flush();");
            /* The C entry point returns an `int`, so the value is cast - and it **is** the
             * value: `fn main() -> i32` sets the exit code. A `main` that returns nothing
             * keeps the plain `return 0;`, which is also what a C programmer expects. */
            cgLine(g, retVoid ? "return 0;" : "return (int)__extc_ret_v;");
        }
        else if (retVoid) cgLine(g, "return;");
        else              cgLine(g, "return __extc_ret_v;");
    }
    g->retType = savedRet;
    g->inMain  = false;
    g->rcFuncUsesHome = false;
    g->tmpSeq = savedSeq;
    g->uncheckedBody = savedUnchecked;
    g->noArena = savedNoArena;
    g->owSitesOwn = savedOw;          /* restored last, see the note above */
    g->owCalls = savedOwCalls;
    g->owLocalOwn = savedOwLocal;
    g->indent--;
    cgLine(g, "}");
}

/* ------------------------------------------------------- generic instances
 *
 * The checker verifies a template once; code generation emits one copy of the C
 * per instance. The source of a container is written in extC, in the prelude,
 * and all the compiler does is substitute the type arguments for T.
 */

/* Everything generated is `static`, except `main`, whose linkage C fixes.
 *
 * This is not style but performance. extC emits a single .c file, one
 * translation unit, so external linkage buys nothing while forcing gcc to assume
 * that other code may call the function and that pointers may alias, which makes
 * it reject outer-loop vectorization outright. Internal linkage also brings
 * cross-function inlining and stronger constant propagation for free, and
 * `-Wl,--gc-sections` stops being the only safety net.
 *
 * Should extC ever support several translation units, which it does not today,
 * this has to become "export only what another unit uses".
 */

/**
 * @brief Is a function the program's entry point?
 *
 * The criterion is a free function (no owner) whose name is exactly `main`.
 *
 * @param f  The candidate function; may be NULL.
 * @return   true when it is the entry point, false otherwise.
 */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

/**
 * @brief Is a function an **entry point**: C's main, or a free function marked @module?
 *
 * Both are treated the same in the product: the return value is C's int, no hidden parameters are
 * taken, the home arena is the function body's own, and falling off the body returns 0. The only
 * difference is that with a dispatcher C's main is the dispatcher and the entry itself becomes a
 * static helper.
 *
 * @param g  The code generation state (the criterion does not depend on it; the parameter is kept so
 *           call sites read consistently).
 * @param f  The candidate function; may be NULL.
 * @return   true when it is an entry point.
 */
static bool cgIsEntry(CG *g, const FuncDef *f) {
    (void)g;
    return cgIsMain(f) || (f && f->isModuleEntry);
}

/**
 * @brief The C name of an entry function in the product.
 *
 * With a dispatcher every entry is renamed to __extc_entry_<C name>: C's main already belongs to the
 * dispatcher, and the entry still needs a name with internal linkage for the table to take its
 * address. Without a dispatcher it is exactly main (a normal program's output is unchanged).
 *
 * @param g  The code generation state; needModuleTramp decides whether the prefix is used.
 * @param f  The function definition; a non-entry returns its C name unchanged.
 * @return   The C identifier in the product.
 */
static const char *cgEntryCName(CG *g, const FuncDef *f) {
    if (!f) return "";
    const char *base = cFuncName(g, (FuncDef *)f);
    if (g->needModuleTramp && cgIsEntry(g, f))
        return arenaPrintf(g->arena, "__extc_entry_%s", base);
    return base;
}

/**
 * @brief Emit a string that has **already been lexed** (a name or a description) as a C string
 *        literal, including the quotes.
 *
 * A string token's text has its surrounding quotes stripped and **its escapes kept as written in the
 * source** (the escape spellings were validated by the lexer's `lexEscape`, and C accepts the same
 * set), so it is copied between quotes here without escaping again. Escaping again would turn `"\""`
 * in the source into `\"` in the output (one extra backslash), and a TAB in a description into two
 * characters.
 *
 * @param b  The buffer receiving the text.
 * @param s  The string token's text, NUL-terminated; NULL is treated as the empty string.
 */
static void emitStringToken(Buf *b, const char *s) {
    bufPutc(b, '"');
    bufPuts(b, s ? s : "");
    bufPutc(b, '"');
}

/**
 * @brief A module's key: the same key is the same module (the entry file's is the empty string).
 */
static const char *moduleEntryKey(const FuncDef *f) {
    return (f->modName && *f->modName) ? f->modName : "";
}

/**
 * @brief Collect the @module entries, decide the names shown by --help, and validate uniqueness.
 *
 * Two rules are checked here because they are **command-line semantics** rather than type errors: a
 * module has exactly one entry (which is what -m selects), and an -m name is globally unique
 * (otherwise the command line cannot choose). An entry that fails a check does not enter the table
 * and the rest of the program is generated as usual.
 *
 * @param g  The code generation state; moduleEntries/needModuleTramp/hasMainEntry are filled in.
 * @param m  The current module; funcs holds all candidates.
 */
static void scanModuleEntries(CG *g, Module *m) {
    g->hasMainEntry = false;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (cgIsMain(f)) g->hasMainEntry = true;
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f->isModuleEntry) continue;
        const char *key = moduleEntryKey(f);
        bool sameModule = false;
        for (size_t j = 0; j < i && !sameModule; j++) {
            FuncDef *o = *(FuncDef **)vecAt(&m->funcs, j);
            if (o->isModuleEntry && strcmp(moduleEntryKey(o), key) == 0) sameModule = true;
        }
        if (sameModule) {
            ctxError(g->ctx, f->line ? f->line : 1, 1,
                     "One module has one entry: -m names a module, not a function. Move this"
                     " one into another module, or drop @module from it.",
                     "module %s already has an @module entry", key[0] ? key : "<root>");
            continue;
        }
        const char *name = f->moduleEntryName;
        if (!name || !*name) name = key[0] ? key : "main";
        bool dup = false;
        for (size_t j = 0; j < g->moduleEntries.len && !dup; j++) {
            ModEntry *e = (ModEntry *)vecAt(&g->moduleEntries, j);
            if (strcmp(e->name, name) == 0) dup = true;
        }
        if (!dup && g->hasMainEntry && strcmp(name, "main") == 0) dup = true;
        if (dup) {
            ctxError(g->ctx, f->line ? f->line : 1, 1,
                     "-m names how the command line picks an entry, so two entries cannot"
                     " share one name. Give this one its own: @module(\"other\").",
                     "duplicate module entry name %s", name);
            continue;
        }
        ModEntry *e = (ModEntry *)vecPush(&g->moduleEntries);
        e->f = f;
        e->name = name;
        e->desc = f->moduleDesc ? f->moduleDesc : "";
    }
    g->needModuleTramp = g->moduleEntries.len > 0;
}

/**
 * @brief Emit C's main: a table dispatched by -m, plus --help.
 *
 * A program without @module emits none of it (needModuleTramp is false), so an ordinary program pays
 * nothing. Dispatch is a plain table lookup: no dlopen, no cross-module ABI, because all entries live
 * in one compilation unit. `-m x a b` collapses a b into a new argv in place (argv[0] is still the
 * program name), so the entry sees the same command line as when it is the main program itself.
 *
 * @param g  The code generation state; moduleEntries has been filled in by scanModuleEntries.
 */
static void emitModuleTramp(CG *g) {
    cgLine(g, "/* ---- module dispatcher: ./a -m <name> args... ---- */");
    cgLine(g, "typedef struct ExtcModuleEntry { const char *name; const char *desc; int (*fn)(int, char **); } ExtcModuleEntry;");
    cgLine(g, "static const ExtcModuleEntry extc_module_entries[] = {");
    for (size_t i = 0; i < g->moduleEntries.len; i++) {
        ModEntry *e = (ModEntry *)vecAt(&g->moduleEntries, i);
        Buf row;
        bufInit(&row, g->arena);
        bufPuts(&row, "    { ");
        emitStringToken(&row, e->name);
        bufPuts(&row, ", ");
        emitStringToken(&row, e->desc);
        bufPrintf(&row, ", %s },", cgEntryCName(g, e->f));
        cgLine(g, "%s", bufCstr(&row));
    }
    cgLine(g, "};");
    cgLine(g, "static const size_t extc_module_count = sizeof(extc_module_entries) / sizeof(extc_module_entries[0]);");
    cgLine(g, "");
    cgLine(g, "static void extc_module_usage(const char *prog) {");
    cgLine(g, "    printf(\"usage: %%s [-m <module>] [args...]\\n\", prog);");
    cgLine(g, "    printf(\"       %%s --help\\n\", prog);");
    if (g->hasMainEntry)
        cgLine(g, "    printf(\"\\nWithout -m it runs the program's own entry.\\n\");");
    else
        cgLine(g, "    printf(\"\\nPass -m to pick one of the compiled-in modules.\\n\");");
    cgLine(g, "    printf(\"\\nmodules:\\n\");");
    cgLine(g, "    for (size_t i = 0; i < extc_module_count; i++)");
    cgLine(g, "        printf(\"  -m %%-16s %%s\\n\", extc_module_entries[i].name, extc_module_entries[i].desc);");
    cgLine(g, "}");
    cgLine(g, "");
    cgLine(g, "int main(int argc, char **argv) {");
    cgLine(g, "    if (argc > 1 && (strcmp(argv[1], \"--help\") == 0 || strcmp(argv[1], \"-h\") == 0)) {");
    cgLine(g, "        extc_module_usage(argv[0]);");
    cgLine(g, "        return 0;");
    cgLine(g, "    }");
    cgLine(g, "    if (argc > 1 && strcmp(argv[1], \"-m\") == 0) {");
    cgLine(g, "        if (argc < 3) {");
    cgLine(g, "            fprintf(stderr, \"%%s: -m needs a module name\\n\", argv[0]);");
    cgLine(g, "            extc_module_usage(argv[0]);");
    cgLine(g, "            return 2;");
    cgLine(g, "        }");
    cgLine(g, "        for (size_t i = 0; i < extc_module_count; i++)");
    cgLine(g, "            if (strcmp(argv[2], extc_module_entries[i].name) == 0) {");
    cgLine(g, "                for (int k = 3; k < argc; k++) argv[k - 2] = argv[k];");
    cgLine(g, "                return extc_module_entries[i].fn(argc - 2, argv);");
    cgLine(g, "            }");
    cgLine(g, "        fprintf(stderr, \"%%s: unknown module '%%s'\\n\", argv[0], argv[2]);");
    cgLine(g, "        extc_module_usage(argv[0]);");
    cgLine(g, "        return 2;");
    cgLine(g, "    }");
    if (g->hasMainEntry)
        cgLine(g, "    return __extc_entry_main(argc, argv);");
    else {
        cgLine(g, "    extc_module_usage(argv[0]);");
        cgLine(g, "    return 2;");
    }
    cgLine(g, "}");
    cgLine(g, "");
}

/* Emit the forward declaration of one function, so that definitions may appear
 * in any order.
 */
/**
 * @brief Emit the trampoline of a `parallel::run` worker and its ctx type.
 *
 * A worker's parameters are `(id, lo, hi, views...)`, exactly one `mut slice<T>` of which is the
 * output: the trampoline slices it into a `sub` that belongs to this worker alone by `[lo, hi)`, and
 * the other views are shared as they are -- nobody writes them, so they need no partitioning. The
 * ctx is a struct with one field per view, passed by value.
 *
 * When the worker needs a home arena (its body allocates), one thread-local arena is appended as the
 * last parameter: its allocations cannot escape this call, and neither worker contends for the main
 * thread's frame arena.
 *
 * @param g  The code generation context.
 * @param f  The worker function; the first three parameters are `id`/`lo`/`hi` and all others are
 *           views.
 */
static void genParTramp(CG *g, FuncDef *f) {
    size_t nCaps = f->params.len - 4, outIdx = 0;
    for (size_t i = 0; i <= nCaps; i++)
        if ((*(Param **)vecAt(&f->params, 3 + i))->type->mut) outIdx = i;
    {   /* the ctx type: fields v0..vN whose types are the C names of the parameters (views and scalars
     * follow the same naming rule as in the cType notes) */
        Buf ct;
        bufInit(&ct, g->arena);
        bufPrintf(&ct, "typedef struct {");
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&ct, " %s v%zu;",
                      cType(g, (*(Param **)vecAt(&f->params, 3 + i))->type), i);
        bufPrintf(&ct, " } __extc_parctx_%s;", f->name);
        cgLine(g, "%s", bufCstr(&ct));
    }
    Type *vt = (*(Param **)vecAt(&f->params, 3 + outIdx))->type;
    const char *vn = cType(g, vt);
    const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", f->name);
    /* A worker of a two-dimensional fold writes `out[r * cols + c]` in absolute row numbers, and
     * slicing the range would shift them, so that shape passes the whole output view
     * (`planParFullOut`). */
    bool full = planParFullOut(f);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp);", f->name);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp) {", f->name);
    cgLine(g, "    %s *c = (%s *)ctxp;", ctn, ctn);
    if (!full)
        cgLine(g, "    %s sub = { .data = c->v%zu.data + lo, .len = hi - lo };", vn, outIdx);
    {   /* arguments: the captures (views/scalars) as they are, the output one replaced by the sliced
     * sub (as it is in whole-view mode) */
        Buf args;
        bufInit(&args, g->arena);
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&args, "%s%s", i ? ", " : "",
                      (i == outIdx && !full) ? "sub" : arenaPrintf(g->arena, "c->v%zu", i));
        /* As soon as the worker body has a `new`, it takes one extra hidden home parameter (the
         * needsHome scheme, last parameter), so **the thread-local arena** is passed here: the worker
         * writes only its own piece of the output and returns only an i64 => its allocations cannot
         * escape this worker => the thread-local arena is safe, and neither worker contends for the
         * main thread's frame arena. */
        if (planUsesHome(f)) {
            bufPrintf(&args, "%sextc_tls_arena", ", ");
            /* The ownership-delegation flag is always 0: a thread-local arena's ownership is decided by the
             * run-time depth (`RC_OWN_TLS`), and delegation means "the owner hands the right to sweep to
             * the callee on the spot", which does not apply to the worker chain. */
            if (g->needReclaim) bufPuts(&args, ", 0");
        }
        cgLine(g, "    return (int64_t)%s(id, lo, hi, %s);", f->name, bufCstr(&args));
    }
    cgLine(g, "}");
}

/**
 * @brief Emit a function's forward declaration so that definition order does not matter.
 *
 * A coroutine gets no prototype under its own name: its product is `name$frame` and `name$step`,
 * emitted together, and the frame necessarily precedes every use; an undefined `name` prototype would
 * also break the later byte-offset scans -- `name` would match inside `name$step` and erase the text
 * in between. `@inline` must be written on both the prototype and the definition.
 *
 * @param g  The generator state; output and allocation come from it.
 * @param f  The function to declare; `main` and exported functions get no `static`.
 */
static void genFuncProto(CG *g, FuncDef *f) {
    if (planIsCoro(f)) return;
    Buf sig;
    bufInit(&sig, g->arena);
    bool entry = cgIsEntry(g, f);
    bufPrintf(&sig, "%s%s %s(%s);",
              entry ? (g->needModuleTramp ? "static " : "")
                    : (f->isExport ? "" : (f->isInline ? "EXTC_INLINE " : "static ")),
              entry ? "int" : cType(g, f->ret), cgEntryCName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
}

/**
 * @brief Emit a forward declaration for every function mentioned in a global variable's initializer.
 *
 * A function address is a constant, so `var t: api = api { alloc: hostAlloc }` is a legal global
 * initializer; but C requires the declaration to be seen first. Globals are emitted before the
 * prototype pool, so without this declaration `static api t = { .alloc = &hostAlloc };` reports
 * `hostAlloc` undeclared. The prototype pool emits it again later, and C allows repeated
 * declarations.
 *
 * @param ctx  The generator state (`CG *`); passed as `void *` to fit the access callback's
 *             signature.
 * @param e    The expression reached; only `EX_IDENT` nodes that resolve to a called function are
 *             handled.
 * @return     Always true, so the child traversal continues.
 */
static bool globalFnProto(void *ctx, Expr *e) {
    if (e->kind == EX_IDENT && planCallee(e)) genFuncProto((CG *)ctx, planCallee(e));
    return true;
}

/**
 * @brief Walk a statement's children and hand the function references found there to globalFnProto.
 *
 * @param ctx  The generator state (`CG *`), passed through to the expression callback.
 * @param s    The statement whose children are walked.
 * @return     astWalkStmtChildren's return value: whether the child traversal was not interrupted.
 */
static bool globalFnProtoStmt(void *ctx, Stmt *s) {
    AstVisit v = { globalFnProto, globalFnProtoStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/* ------------------------------------------------------- struct definition order
 *
 * Ordinary structs and generic instances contain each other - a `player` holds a
 * `slice<u8>`, whose fields are ordinary `i64`s - so emitting all ordinary
 * structs first and the instances afterwards does not work. Definitions are
 * emitted in dependency order instead, which also covers types such as
 * `array<point>` that do not exist yet.
 *
 * A cycle can only close through a `ref`, that is through a pointer, and a
 * pointer needs nothing but the typedef of the type it points at, so all
 * typedefs are emitted first.
 */

typedef struct {
    StructDef *sd;      /* an ordinary struct; NULL for an instance or an array */
    Type      *inst;    /* a generic instance or an array; NULL for a struct */
    TypeDef   *td;      /* an enum with payloads; NULL otherwise */
    Vec        deps;    /* int* - indices of the units this one depends on */
    bool       done;
} SUnit;

/**
 * @brief The C name of a definition unit (a struct, an instance, an array or an enum with payload).
 *
 * @param u  The definition unit; a generic struct instance and a generic enum instance both have
 *           `inst` and `sd`/`td` set.
 * @return   The instance name when `inst` is set (both arrays and generic enum instances take this
 *           path), otherwise the payload-carrying enum's name when `td` is set, otherwise the struct
 *           name.
 */
static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* instance or array, enum instances included */
    if (u->td) return u->td->name;          /* non-generic enum with payloads */
    return u->sd->name;
}

/**
 * @brief Find the index in `units` of the unit that defines a type.
 *
 * A generic instance is matched by C name rather than by pointer: `mut slice<T>` is a shadow type with
 * the same name and the same C struct as the read-only view, so a pointer comparison never finds it,
 * the dependency sort misses it, and `struct reader { chunk: mut slice<u8> }` then fails on an
 * incomplete type; besides, one C name can correspond to two instances, one writable view and one
 * read-only view.
 *
 * @param units  The unit list whose elements are `SUnit *`.
 * @param t      The type to find; returns -1 when NULL.
 * @return       The unit's index; -1 when `units` holds no matching unit.
 */
static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_ENUM && u->td && t->edef == u->td) return (int)i;
        if ((t->kind == TY_GENERIC || t->kind == TY_ARRAY) && u->inst &&
            strcmp(u->inst->name, t->name) == 0) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

/**
 * @brief Emit the C definition of a struct, an array or an enum.
 *
 * An enum with payload is emitted as `struct { tag; union }` and takes part in the dependency sort
 * like any other struct: a payload can contain other structs, and missing the sort would leave an
 * unknown type name in the generated C.
 *
 * @param g  The generator state; a generic instance and a generic enum instance enter the
 *           substitution context first and leave it after the output.
 * @param u  The definition unit to emit.
 */
static void unitBody(CG *g, SUnit *u) {
    if (u->td) {
        /* A generic enum instance such as `option<i64>` enters the
         * substitution context, so that `cType` substitutes the payload
         * types. */
        SubCtx sv = {0};
        if (u->inst) sv = substEnter(g, u->inst);
        cgLine(g, "struct %s {", unitName(u));
        g->indent++;
        cgLine(g, "int tag;");
        cgLine(g, "union {");
        g->indent++;
        for (size_t j = 0; j < u->td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&u->td->variants, j);
            if (v->types.len == 0) continue;
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "struct { ");
            for (size_t k = 0; k < v->types.len; k++) {
                if (k) bufPuts(&b, " ");
                bufPrintf(&b, "%s _%zu;", cType(g, *(Type **)vecAt(&v->types, k)), k);
            }
            bufPrintf(&b, " } %s;", v->name);
            cgLine(g, "%s", bufCstr(&b));
        }
        g->indent--;
        cgLine(g, "} u;");
        g->indent--;
        cgLine(g, "};");
        cgLine(g, "");
        if (u->inst) substLeave(g, sv);
        return;
    }

    bool generic = u->inst && u->inst->kind == TY_GENERIC;
    SubCtx gsv = {0};
    if (generic) gsv = substEnter(g, u->inst);

    cgLine(g, "struct %s {", unitName(u));
    g->indent++;
    if (u->inst && u->inst->kind == TY_ARRAY && u->inst->asize < 0) {
        /* A type whose length is still a `const` parameter is a template, not a value: nothing can
         * be laid out for it, and instantiation replaces it before any declaration mentions it. */
        g->indent--;
        cgLine(g, "};");
        return;
    }
    if (u->inst && u->inst->kind == TY_ARRAY) {
        /* An array is a struct holding a bare C array; that is where its value
         * semantics come from. */
        cgLine(g, "%s data[%lld];", cType(g, u->inst->inner),
               (long long)u->inst->asize);
    } else {
        for (size_t i = 0; i < u->sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "%s %s;", cType(g, fd->type), fd->name);
        }
        /* C does not allow an empty struct: `struct unit { };` is a GNU
         * extension, and `(unit){0}` then warns about excess elements. A struct
         * with no fields therefore gets one placeholder byte. A user never sees
         * it and `(T){0}` stays legal; `unit`, the payload type of
         * `result<unit, E>`, exists thanks to this. */
        if (u->sd->fields.len == 0) cgLine(g, "char __extc_empty;");
    }
    g->indent--;
    cgLine(g, "};");

    /* `@frozen`: the struct's bytes are laid out exactly as C lays out this field list --
     * declaration order, C's alignment and padding, nothing else inside. The two assertions below
     * make a reordered or slipped-in field fail in the **generated** C instead of at a call site
     * reading the wrong bytes: neighbours come out in increasing offset order (declaration order,
     * never sorted by size), and `sizeof` stays within the end of the last field plus the struct's
     * own alignment, which is the trailing padding C may add -- so an added field shows up as a
     * larger size unless it fits in that padding. Every number is derived from C (`offsetof`,
     * `sizeof`, `_Alignof`); a hard-coded `== 8` would break a legitimate build on a platform with
     * different alignment rules. */
    if (u->sd && u->sd->frozen && u->sd->fields.len > 0) {
        const char *tn = unitName(u);
        for (size_t i = 1; i < u->sd->fields.len; i++) {
            FieldDef *a = *(FieldDef **)vecAt(&u->sd->fields, i - 1);
            FieldDef *b = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "_Static_assert(offsetof(%s, %s) < offsetof(%s, %s),", tn, a->name, tn, b->name);
            cgLine(g, "               \"extC @frozen: the fields stay in declaration order\");");
        }
        FieldDef *last = *(FieldDef **)vecAt(&u->sd->fields, u->sd->fields.len - 1);
        cgLine(g, "_Static_assert(sizeof(%s) <= offsetof(%s, %s) + sizeof(((%s *)0)->%s)"
                   " + _Alignof(%s),",
               tn, tn, last->name, tn, last->name, tn);
        cgLine(g, "               \"extC @frozen: nothing may be added to a frozen struct\");");
    }
    cgLine(g, "");
    if (generic) substLeave(g, gsv);
}

/* The derived `_eq` for arrays is gone: array equality now goes through
 * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
 * implementation. The old shape emitted a loop per array type, which cost 2450
 * lines, 32% of the generated C, for 200 types in a stress program.
 */

/**
 * @brief Register the unwrapping helper of an `option` / `result` instance and return its name.
 *
 * Deduplicated by the option type's C name: every `!` on one instance points at the same helper and
 * only one definition is emitted. An `@unchecked` body does not come here (it takes the payload
 * directly), so the helper can only come from a checked `!`.
 *
 * @param g      The generator state.
 * @param ot     The operand type, an `option<T>` or `result<T, E>` instance.
 * @param isOpt  true for `option` (tag `some`), false for `result` (tag `success`).
 * @param pay    The payload type `T`, which is also the helper's return type.
 * @return       The helper's name; the call site uses it to pass the operand, the extC source path
 *               and the line number.
 */
static const char *unwrapHelper(CG *g, Type *ot, bool isOpt, Type *pay) {
    const char *otName = cType(g, ot);
    const char *name = arenaPrintf(g->arena, "extc_unwrap$%s", otName);
    for (size_t i = 0; i < g->unwraps.len; i++)
        if (strcmp(((UnwrapHelper *)vecAt(&g->unwraps, i))->name, name) == 0)
            return name;

    const char *var = isOpt ? "some" : "success";
    Buf b;
    bufInit(&b, g->arena);
    /* The payload type is rendered **here**, inside the instance's substitution context:
     * at assembly time the context is long gone and a `T` would print as `int`. */
    bufPrintf(&b, "static %s %s(%s o, const char *f, int l) {\n",
              cType(g, pay), name, otName);
    bufPrintf(&b, "    (void)extc_tagAssert((int)o.tag, %s_%s, f, l);\n", otName, var);
    bufPrintf(&b, "    return o.u.%s._0;\n}\n", var);

    UnwrapHelper *h = (UnwrapHelper *)vecPush(&g->unwraps);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/**
 * @brief Register the slicing helper of a base type and return its name.
 *
 * Registering again with the same base type and the same kind returns the same name and only one
 * function is emitted. The `unchecked` helper has a name of its own, and a checked caller keeps using
 * the checked one: the two cannot share a definition.
 *
 * @param g          The generator state.
 * @param ob         The sliced type: an array or a view.
 * @param st         The resulting slice type.
 * @param tail       true when the slice runs to the end (`s[lo..]`), where the omitted upper bound
 *                   takes the base type's length.
 * @param unchecked  true when the caller's body is `@unchecked`: the helper skips
 *                   `extc_checkedRange` and uses the written form of `lo` directly.
 * @return           The helper's name, built from the base type and the kind; one base type is cut
 *                   into only one kind of slice, so the names cannot collide.
 */
static const char *sliceHelper(CG *g, Type *ob, Type *st, bool tail, bool unchecked) {
    const char *name = arenaPrintf(g->arena, "extc_slice%s%s_%s",
                                   tail ? "To" : "", unchecked ? "U" : "", ob->name);
    for (size_t i = 0; i < g->helpers.len; i++)
        if (strcmp(((SliceHelper *)vecAt(&g->helpers, i))->name, name) == 0)
            return name;

    const char *ret = cType(g, st);
    Buf b;
    bufInit(&b, g->arena);
    if (ob->kind == TY_ARRAY) {
        /* A fixed-size array: the length is a compile-time constant. */
        bufPrintf(&b, "static %s %s(%s *a, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, %lld, f, ln);\n",
                      (long long)ob->asize);
        bufPrintf(&b, "    return (%s){ .data = &a->data[s], .len = hi - lo };\n}\n", ret);
    } else if (tail) {
        /* Slicing to the end: hi is v.len. It is taken as a parameter instead
         * of being expanded in place, so the view is evaluated once. */
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, const char *f, int ln) {\n",
                  ret, name, ob->name);
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, v.len, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = v.len - lo };\n}\n", ret);
    } else {
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = hi - lo };\n}\n", ret);
    }

    SliceHelper *h = (SliceHelper *)vecPush(&g->helpers);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/**
 * @brief Generate the view expression produced by `a[lo..hi]`.
 *
 * A slice the compiler can prove leaves no run-time trace: with a fixed-size array as the base and
 * both bounds as literals (an omitted bound is filled in by the checker), the range is known during
 * checking and an out-of-range one has already been reported, so the generated C is directly
 * `&a.data[2]` plus `.len = 3`; every other case goes to the helper, which checks the range at run
 * time and reports the error with the extC source position.
 *
 * @param g  The generator state.
 * @param e  The slice expression; `e->type` is the resulting view type, and when the base is
 *           `ref [N]T` it is a pointer in C, so taking `.data` needs one extra dereference.
 * @return   The view expression; when the type cannot be generated, reports the error first and
 *           returns the placeholder expression "0".
 */
static const char *genSlice(CG *g, Expr *e) {
    Type *ob = ttBase(subst(g, e->u.slice.obj->type));
    Type *st = subst(g, e->type);
    if (!ob || !st || !ob->name) {
        ctxError(g->ctx, e->line, 1, NULL, "cannot generate a slice of this type");
        return "0";
    }

    const char *obj = genExpr(g, e->u.slice.obj);
    Expr *lo = e->u.slice.lo, *hi = e->u.slice.hi;
    /* When the base is a `ref [N]T`, which is a pointer in C, reading `.data`
     * needs one dereference, as in `(*p).data[..]`, but the argument passed to
     * the slice helper is that pointer itself, since the helper takes the
     * `array_*` type. Without the dereference the result is `p.data[..]`,
     * and gcc answers "'p' is a pointer". */
    bool objIsRef = e->u.slice.obj->type &&
                    subst(g, e->u.slice.obj->type)->kind == TY_REF;

    if (ob->kind == TY_ARRAY && lo && hi &&
        lo->kind == EX_INT && hi->kind == EX_INT) {
        const char *arr = objIsRef ? arenaPrintf(g->arena, "(*%s)", obj) : obj;
        return arenaPrintf(g->arena, "(%s){ .data = &(%s.data[%lld]), .len = %lld }",
                           cType(g, st), arr, lo->u.ival, hi->u.ival - lo->u.ival);
    }

    const char *loS = lo ? genExpr(g, lo) : "0";
    const char *arg = ob->kind != TY_ARRAY ? obj
                      : (objIsRef ? obj : arenaPrintf(g->arena, "&(%s)", obj));
    if (!hi) {
        /* Only a view base can reach here: for an array base the checker has
         * already filled the omitted bound in as a literal. */
        const char *fn = sliceHelper(g, ob, st, true, g->uncheckedBody);
        return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), \"%s\", %d)",
                           fn, arg, loS, g->path, e->line);
    }
    const char *fn = sliceHelper(g, ob, st, false, g->uncheckedBody);
    const char *hiS = genExpr(g, hi);
    return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), (int64_t)(%s), \"%s\", %d)",
                       fn, arg, loS, hiS, g->path, e->line);
}


/* Close the set of generated instances transitively.
 *
 * An instance mentioned only in a method signature never reached `units` unless
 * the program used it directly, so `varArray<i64>::get()` returning `option<i64>`
 * produced C that named an unknown type. The user had written nothing wrong; the
 * compiler had simply not finished its own accounting.
 *
 * The set is closed by walking the fields, the method signatures and the enum
 * payloads of every unit, substituting the type arguments of a generic instance,
 * and adding every instance met on the way until nothing new appears. The
 * iteration count is capped, and passing the cap is reported loudly rather than
 * quietly generating too little.
 */
/**
 * @brief Add the generic instances a type mentions to the unit list; existing ones are skipped by C
 *        name.
 *
 * @param arena  The arena for allocating new units.
 * @param units  The unit list, appended to in place.
 * @param t      The type to check; returns immediately when it is not a generic struct or enum
 *               instance.
 */
static void addInstanceUnit(Arena *arena, Vec *units, Type *t) {
    if (!t) return;
    bool isStructInst = (t->kind == TY_GENERIC && t->sdef);          /* varArray<i32> */
    bool isEnumInst   = (t->kind == TY_ENUM && t->edef && t->edef->typeParams.len > 0);
    if (!isStructInst && !isEnumInst) return;
    /* Deduplication must be by C name and cannot go through unitFind, which
     * compares a generic enum instance by its template: `option_i64` would match
     * an existing `option_i32`, since the two share the template, be considered
     * already present, and never be generated, leaving the generated C with an
     * unknown type name. */
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (u->inst && t->name && strcmp(u->inst->name, t->name) == 0) return;
        if (u->td && t->kind == TY_ENUM && u->inst == NULL && u->td == t->edef) return;
    }
    SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
    if (isStructInst) u->sd = t->sdef; else u->td = t->edef;
    u->inst = t;
    vecInit(&u->deps, arena, sizeof(int));
    *(SUnit **)vecPush(units) = u;
}

/**
 * @brief Scan a type and add every generic instance it mentions to the unit list.
 *
 * It follows `inner` and the generic arguments, so `ref`, slices and array elements are all reached.
 * A function type keeps its parts on `params` / `ret` rather than on `inner` / `targs`, so it gets a
 * second walk: `fn(pair<i32, u8>) -> void` written inside a struct is a mention of the
 * `pair<i32, u8>` instance, and missing it leaves that struct unemitted.
 *
 * @param arena  The arena for allocating new units.
 * @param units  The unit list, appended to in place.
 * @param t      The type to scan; returns immediately when NULL.
 * @param depth  The recursion depth, starting at 0; the walk stops above 12, which bounds
 *               self-referential types.
 */
static void scanTypeForUnits(Arena *arena, Vec *units, Type *t, int depth) {
    if (!t || depth > 12) return;                                    /* bounds self-reference */
    addInstanceUnit(arena, units, t);
    if (t->inner) scanTypeForUnits(arena, units, t->inner, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        scanTypeForUnits(arena, units, *(Type **)vecAt(&t->targs, i), depth + 1);
    /* A function type mentions its parts through `params` / `ret`, not through `inner` /
     * `targs`, so it needs its own walk: `f: fn(pair<i32, u8>) -> void` inside a struct is a
     * mention of the instance `pair_i32_u8`, and without this walk that struct is never
     * emitted. */
    if (t->kind == TY_FN) {
        scanTypeForUnits(arena, units, t->ret, depth + 1);
        for (size_t i = 0; i < t->params.len; i++)
            scanTypeForUnits(arena, units, *(Type **)vecAt(&t->params, i), depth + 1);
    }
}

/**
 * @brief Scan every type a unit mentions and add the generic instances it depends on to the unit
 *        list.
 *
 * Three places are walked: the fields; the method signatures (both parameters and return type count,
 * and an instance hides there); and the payloads of the enum variants. When the unit is itself a
 * generic instance, its own type arguments are substituted in first.
 *
 * @param arena  The arena for allocating new units.
 * @param tt     The type table used for generic parameter substitution.
 * @param units  The unit list, appended to in place.
 * @param u      The unit to scan.
 */
static void scanUnitForUnits(Arena *arena, TypeTable *tt, Vec *units, SUnit *u) {
    Vec *tps = NULL; Vec *tas = NULL;
    StructDef *sd = u->sd ? u->sd : (u->inst ? u->inst->sdef : NULL);
    if (u->inst && sd) { tps = &sd->typeParams; tas = &u->inst->targs; }
    /* 1. fields */
    if (sd) for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (tps && ft) ft = ttSubstitute(tt, ft, tps, tas);
        scanTypeForUnits(arena, units, ft, 0);
    }
    /* 2. method signatures: parameters and return type */
    if (sd) for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&sd->methods, i);
        /* The coroutine protocols (`next`/`value`, on the frame and on the handle) are emitted
         * inline at their call sites, never as real functions. */
        if (planCoroProto(f)) continue;
        for (size_t j = 0; j < f->params.len; j++) {
            Type *pt = (*(Param **)vecAt(&f->params, j))->type;
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
        Type *rt = f->ret;
        if (tps && rt) rt = ttSubstitute(tt, rt, tps, tas);
        scanTypeForUnits(arena, units, rt, 0);
    }
    /* 3. enum payloads */
    if (u->td) for (size_t i = 0; i < u->td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&u->td->variants, i);
        for (size_t k = 0; k < v->types.len; k++) {
            Type *pt = *(Type **)vecAt(&v->types, k);
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
    }
}

/* Generate the whole C translation unit for a module.
 *
 * Params:
 *   lineMap - whether to emit `#line` directives
 *   out     - receives the generated text
 *
 * Returns:
 *   false when the checker recorded an error, true otherwise. Reporting a
 *   consistency problem never stops generation, so that the caller sees the
 *   error count and not a damaged output buffer.
 */
/**
 * @brief Does an instance's signature still mention a type parameter?
 *
 * Such a FuncDef is the checker's product: a generic call inside a generic body creates an instance
 * whose argument is `T` itself so that the body can be checked; it is never a C function and the
 * emission loop skips it.
 *
 * @param f  The function instance to check.
 * @return   true when a parameter type or the return type still mentions a type parameter.
 */
static bool funcSignatureMentionsParam(FuncDef *f) {
    if (f->ret && mentionsParam(f->ret)) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (p->type && mentionsParam(p->type)) return true;
    }
    return false;
}

/* ------------------------------------------------- unreferenced definitions
 * A definition nobody names is dead weight: the C compiler warns about it, and
 * the reader of the generated C has to skip it. Which definitions those are is
 * decided here, on the finished text, because a use may be generated long after
 * the definition (a body, or the descriptor table).
 */

/**
 * @brief Can a byte continue a C identifier?
 *
 * Used to tell the name `e` from the `e` inside `self` or `true`: single-letter locals are counted by
 * name, and a plain substring search would hit everywhere, which only ever keeps a definition alive
 * too long, never drops one.
 *
 * @param c  The byte to test.
 * @return   true for a letter, a digit, `_` or `$`.
 */
static bool identByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* How many times does the name `needle` occur in the finished output, as a whole
 * name? A mention inside a string literal still counts, which only ever keeps a
 * definition alive. */
/* Which text scan is actually running, and over how many bytes. The leaf profile says 53% of a
 * big build sits in `strstr`, but its caller chains have been wrong three times in this session
 * (strcmp to the parser, vecAt to rwStmt, strstr to dropUnusedLocals -- all disproved by the
 * phase timers). These counters answer it directly under EXTC_DBG_TIME=1. */
static long g_scanCalls, g_scanBytes;
static const char *g_scanWho;
/**
 * @brief Record the scan point and byte count of one text scan; accumulated only when timing is on.
 *
 * @param who  The scan point's name, recorded overwriting the previous one as the source of the
 *             latest scan.
 * @param n    This scan's byte count, added to the total.
 */
static void strAccount(const char *who, size_t n) {
    if (!cgTimeOn()) return;
    g_scanCalls++; g_scanBytes += (long)n; g_scanWho = who;
}
/**
 * @brief Report the accumulated scan call count, byte count and latest scan point on stderr.
 */
static void strReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[scan] calls=%ld bytes=%lld (last site: %s)\n",
            g_scanCalls, (long long)g_scanBytes, g_scanWho ? g_scanWho : "-");
}

/**
 * @brief Count how many times `needle` occurs in `hay` as a whole identifier.
 *
 * Occurrences inside string literals count too, which only keeps a definition alive a little longer.
 *
 * @param hay     The text to scan.
 * @param needle  The name to count.
 * @return        The number of occurrences of `needle` as a whole name.
 */
static size_t countMentions(const char *hay, const char *needle) {
    strAccount("countMentions", strlen(hay));
    size_t n = 0, len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) {
        if (p > hay && identByte(p[-1])) continue;
        if (identByte(p[len])) continue;
        n++;
    }
    return n;
}

/**
 * @brief Pair a generated function definition with the declaration of the same C name and record its
 *        offset and length in the body buffer.
 *
 * The key is the C name rather than the `FuncDef` pointer: the instance methods of one generic
 * function are generated once per instance, so pointers are not unique, whereas two generated
 * definitions with the same name cannot compile, so names are unique within this batch. When no
 * declaration of the same name is found the function returns and the definition stays unpaired, so
 * later stages do not delete it: leaving an uncalled function costs one compiler warning and does not
 * affect correctness.
 *
 * @param g    The generation context; `deadFuncs` is the pairing table and the entry with the same
 *             name as `f` is the partner.
 * @param f    The function definition taking part in the pairing.
 * @param off  The definition's starting offset in the body buffer, computed by the call site from the
 *             current output buffer's end.
 * @param len  The definition's byte length.
 */
static void deadFuncBody(CG *g, FuncDef *f, size_t off, size_t len) {
    const char *name = cFuncName(g, f);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (df->body || df->len || strcmp(df->name, name) != 0) continue;
        /* Only the offset is recorded, the text is not cut here: mid-body the output may temporarily point
         * at another buffer, the piece cut at this moment is not contiguous in the final unit, and it
         * can never be found by content afterwards. The text is taken from `g.body` once that is
         * complete. */
        df->off = off;
        df->len = len;
        df->namedByDynTable = df->namedByDynTable || planDynTable(f);   /* see the field */
        return;
    }
}

static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle);
static char  *funcDefStart(char *text, DeadFunc *df, size_t *lenOut);

/**
 * @brief Record a generated local declaration so that it can later be judged for reads.
 *
 * The text runs from `before` to the current output buffer's end, and the call site must guarantee
 * that this is exactly the rest of the declaration. The owning function is `g->curFuncName`.
 *
 * @param g       The generation context; the record is appended to `g->deadLocals`.
 * @param before  The starting offset in `g->out` of the line holding this declaration.
 * @param name    The local variable's C name, pointing at storage that outlives the buffer.
 * @param own     Where it is declared: 1 inside the function body, 0 in the prologue before the body
 *                (`__extc_home` is one of those).
 */
static void localDef(CG *g, size_t before, const char *name, size_t own) {
    Buf l;
    bufInit(&l, g->arena);
    bufPutn(&l, g->out->data + before, g->out->len - before);
    DeadLocal *d = arenaAllocZero(g->arena, sizeof *d);
    d->name     = name;
    d->text     = bufCstr(&l);
    d->funcName = g->curFuncName;
    d->own      = own;
    *(DeadLocal **)vecPush(&g->deadLocals) = d;
}

/**
 * @brief Is a line a `#define`, and if so where does the macro name start in the line?
 *
 * `#define X` and `#  define X` are the same directive (the prelude pads a space between `#` and
 * `define` for alignment), so both spellings must be recognized; at least one space or tab is
 * required between `define` and the macro name.
 *
 * @param ln  The start of a line; no terminating NUL is required.
 * @param ll  The byte length of the line.
 * @return    A pointer to the macro name inside the line; `NULL` when it is not a `#define` or the
 *            line has no macro name.
 */
static const char *macroDefineName(const char *ln, size_t ll) {
    size_t i = 0;
    if (i >= ll || ln[i] != '#') return NULL;
    i++;
    while (i < ll && (ln[i] == ' ' || ln[i] == '\t')) i++;
    if (i + 6 > ll || strncmp(ln + i, "define", 6) != 0) return NULL;
    i += 6;
    if (i >= ll || (ln[i] != ' ' && ln[i] != '\t')) return NULL;
    while (i < ll && (ln[i] == ' ' || ln[i] == '\t')) i++;
    return (i < ll) ? ln + i : NULL;
}

/* Drop local declarations that nothing reads. The stage is the whole function body: a name
 * that occurs there exactly once is the declaration itself. */
/* A statement to remove, or to shorten to its right-hand side. */
typedef struct {
    size_t      start, end;   /* the span in the output text */
    const char *repl;         /* what takes its place, or NULL to drop it entirely */
} LocalCut;

/**
 * @brief Does this text contain a call (looking only for `(`)?
 *
 * An assignment whose right-hand side contains a call cannot be deleted wholesale: the call is the
 * reason it exists, and it matters even when nobody reads the return value (`uint64_t skip =
 * pcg32_next(&r)` advances the generator). A parenthesis is not a call, so a `(` inside a literal or
 * a comment counts as well; that only keeps statements, it never changes semantics.
 *
 * @param s  The text fragment to check.
 * @param n  The fragment's byte count; no terminating NUL is required.
 * @return   `true` when a `(` occurs.
 */
static bool textHasCall(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == '(') return true;
    return false;
}

/**
 * @brief Count how many times `name` is read in this text.
 *
 * String and character literals and comments are skipped whole; an occurrence directly followed by a
 * single `=` (and not by `==`) is a write and does not count as a read, so a compound assignment such
 * as `name += 1` counts as a read. The name must be bounded by non-identifier bytes on both sides.
 *
 * @param hay   The text fragment to scan.
 * @param n     The fragment's byte count; no terminating NUL is required.
 * @param name  The variable name, NUL-terminated.
 * @return      The read count; 0 when it is only written or does not occur.
 */
static size_t countReads(const char *hay, size_t n, const char *name) {
    size_t len = strlen(name), reads = 0;
    for (size_t i = 0; i < n; ) {
        char c = hay[i];
        if (c == '"' || c == '\'') { char q = c; i++;
            while (i < n) { if (hay[i] == '\\' && i + 1 < n) { i += 2; continue; }
                            if (hay[i] == q) { i++; break; } i++; } continue; }
        if (c == '/' && i + 1 < n && hay[i + 1] == '/') { while (i < n && hay[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && hay[i + 1] == '*') { i += 2;
            while (i + 1 < n && !(hay[i] == '*' && hay[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n; continue; }
        if (i + len <= n && strncmp(hay + i, name, len) == 0 &&
            (i == 0 || !identByte(hay[i - 1])) && (i + len >= n || !identByte(hay[i + len]))) {
            size_t j = i + len;
            while (j < n && hay[j] == ' ') j++;
            if (!(j < n && hay[j] == '=' && (j + 1 >= n || hay[j + 1] != '='))) reads++;
            i = j;
            continue;
        }
        i++;
    }
    return reads;
}

/* name -> body for the per-local lookup below. That lookup is a linear scan over `deadFuncs` for
 * every local -- 3N locals against N functions is the quadratic this pass still has (cg-locals is
 * 41% of the build at N=3200 and grows x4 per doubling). The map reproduces the scan exactly: the
 * **first** entry with a non-NULL body, stale pointer and all, because that pointer is what the
 * later strstr(text, body) is applied to. */
typedef struct { const char *name; const char *body; } NameBody;
typedef struct { NameBody *slot; size_t mask; } BodyMap;
/**
 * @brief Compute the FNV-1a hash of a NUL-terminated string for the open addressing of `BodyMap` and
 *        `BpMap`.
 *
 * @param p  A NUL-terminated C string.
 * @return   The hash value; the caller masks it to pick a slot.
 */
static size_t bodyHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211u; }
    return h;
}
/**
 * @brief Initialize `BodyMap`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * The slots are zero-allocated from the arena and an empty slot is recognized by `name == NULL`; the
 * capacity must be a power of two for slot selection by `mask = cap - 1` to work. The table never
 * grows.
 *
 * @param m  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of entries; the call site passes `g->deadFuncs.len + 1`.
 */
static void bodyMapInit(BodyMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief Put one `name -> body` entry into `BodyMap`; a duplicate name keeps only the first entry.
 *
 * First come, first served: the call site inserts in `deadFuncs` order, and the first one is the one
 * to keep. When the name already exists the function returns without overwriting.
 *
 * @param m     The target table.
 * @param name  The C name, NUL-terminated; the table stores only the pointer, so it must outlive the
 *              table.
 * @param body  The pointer to the definition body for that name.
 */
static void bodyMapPut(BodyMap *m, const char *name, const char *body) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        NameBody *s = &m->slot[i];
        if (!s->name) { s->name = name; s->body = body; return; }
        if (strcmp(s->name, name) == 0) return;                 /* first one wins: so does the scan */
    }
}
/* Content-search results, cached per function name. `DeadFunc.body` is a pointer from emission
 * time and the text has been rewritten since (a definition removed by an earlier phase moves
 * everything after it), so it is usually
 * stale and the lookup falls back to strstr(text, body) -- for **every local** of that function.
 * callgrind put that site at 71.67% of all instructions (117,643 calls, 9.33 G instructions).
 * All locals of one function resolve the same body, so one search per name is enough: the cached
 * value is exactly what the search would return while the text is unchanged. */
typedef struct { const char *name; char *bp; size_t bl; int filled; } BpEntry;
typedef struct { BpEntry *slot; size_t mask; } BpMap;
/**
 * @brief Initialize `BpMap`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * Isomorphic to `BodyMap`: the slots are zero-allocated from the arena, an empty slot is recognized
 * by `name == NULL`, and the table never grows.
 *
 * @param m  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of function names.
 */
static void bpMapInit(BpMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief Get (or create) the cache entry of `name` in `BpMap`.
 *
 * A new entry has `filled == 0` and `bp == NULL`; the caller fills in the search result and then sets
 * `filled` to 1 itself. The table never grows, so the returned entry's address is stable for the
 * table's lifetime.
 *
 * @param m     The target table.
 * @param name  The function's C name, NUL-terminated; the table stores only the pointer, so it must
 *              outlive the table.
 * @return      A pointer to the entry inside the table, which lives as long as the table.
 */
static BpEntry *bpMapAt(BpMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        BpEntry *s = &m->slot[i];
        if (!s->name) { s->name = name; return s; }
        if (strcmp(s->name, name) == 0) return s;
    }
}

/**
 * @brief Look up the definition body of `name` in `BodyMap`.
 *
 * What is returned is the pointer recorded at emission time, and once the text is rewritten it may no
 * longer point at the current content: the caller must confirm with `strstr` that it is still inside
 * the current text and fall back to a whole-text search when it is not.
 *
 * @param m     The target table.
 * @param name  The C name, NUL-terminated.
 * @return      The body pointer; `NULL` when the table has no such name.
 */
static const char *bodyMapGet(BodyMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        NameBody *s = &m->slot[i];
        if (!s->name) return NULL;
        if (strcmp(s->name, name) == 0) return s->body;
    }
}

/**
 * @brief Delete, in place, local declarations that nothing reads, together with their assignments.
 *
 * The judgment works per whole function body: when a name occurs only once in that body, that
 * occurrence is the declaration itself. A declaration with `own == 0` sits in the prologue (before the
 * first function body) and belongs to no body, so it is looked for inside the prologue's range
 * instead.
 *
 * One site is deleted per round and the text is rescanned from the start afterwards: a rewrite
 * invalidates every earlier offset. When the right-hand side of a deleted statement contains a call,
 * the call must stay and its value be discarded explicitly, or `-Werror` fails on `unused-value`.
 *
 * @param g      The generation context; reads `deadLocals`, `deadFuncs` and `mainBody`.
 * @param out    The buffer holding the text; its contents are rebuilt for every deletion.
 * @param textp  On entry the text's start (`out->data`); on return the start after the rewrite.
 * @param lenp   On entry the text's byte count; on return the byte count after the rewrite.
 */
static void dropUnusedLocals(CG *g, Buf *out, char **textp, size_t *lenp) {
    char  *text = *textp;
    size_t len  = *lenp;
    /* A declaration with `own == 0` sits in the prologue (before the first function body), and
     * firstBody is that upper bound; `DeadFunc.body` also points into the current text only while
     * the text has not been rewritten. */
    size_t firstBody = len;
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->body && df->body >= text && df->body < text + len) {
            size_t o = (size_t)(df->body - text);
            if (o < firstBody) firstBody = o;
        }
    }
    BpMap bpCache;
    bpMapInit(&bpCache, g->arena, g->deadFuncs.len + 1);
    BodyMap bodyMap;
    bodyMapInit(&bodyMap, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->body) bodyMapPut(&bodyMap, df->name, df->body);
    }
    int textIsOriginal = 1;
    for (;;) {
        bool cut = false;
        for (size_t i = 0; i < g->deadLocals.len && !cut; i++) {
            /* This pass costs memory latency: the real stall is chasing a pointer into a new DeadLocal; the
             * base pointer is still re-read through `vecAt`, so a growth triggered by vecPush is never
             * missed. */
            if (i + 8 < g->deadLocals.len)
                __builtin_prefetch(*(DeadLocal **)vecAt(&g->deadLocals, i + 8), 0, 0);
            DeadLocal *d = *(DeadLocal **)vecAt(&g->deadLocals, i);
            if (!d->text || !d->funcName) continue;
            const char *body = NULL;
            if (textIsOriginal) body = bodyMapGet(&bodyMap, d->funcName);
            if (!body) {
                for (size_t k = 0; k < g->deadFuncs.len; k++) {
                    DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
                    if (df->body && strcmp(df->name, d->funcName) == 0) { body = df->body; break; }
                }
            }
            if (!body && g->mainBody && strcmp(d->funcName, g->mainFuncName) == 0)
                body = g->mainBody;                    /* main has no DeadFunc entry */
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s func=%-12s mainFunc=%-12s body=%s\n", d->name,
                        d->funcName ? d->funcName : "(null)",
                        g->mainFuncName ? g->mainFuncName : "(null)", body ? "yes" : "no");
            if (!body) continue;                       /* its function is gone already */
            g_locN++;
            char  *bp = NULL;
            size_t bl = 0;
            if (textIsOriginal) {
                BpEntry *e = bpMapAt(&bpCache, d->funcName);
                if (!e->filled) { e->bp = strstr(text, body); e->bl = strlen(body); e->filled = 1; }
                bp = e->bp; bl = e->bl;
            } else {
                bp = strstr(text, body);           /* the text has been rewritten: search by content again each round */
                bl = strlen(body);
            }
            /* `g->mainBody` is a pre-taken copy of `main`, and every deletion rebuilds the text: after a
             * deletion the copy cannot be found by content, so the search falls back to the
             * whole unit. A whole-unit search is safe here -- a local variable name is only valid
             * inside its own function's scope, so widening the range only keeps one more local
             * alive. */
            if (!bp) { bp = (char *)text; bl = len; }
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s bp=%s cnt=%zu own=%zu\n", d->name,
                        bp ? "hit" : "miss", bp ? countMentionsIn(g, bp, bl, d->name) : 0, d->own);
            /* The declaration's own mention is not a read of the variable, so the body is
             * examined without that line. Any other read keeps everything. */
            double t_bm = cgNow();
            Buf bm;
            bufInit(&bm, g->arena);
            size_t declAt = 0;
            bool   declInside = false;
            g_locCopy += (long long)bl;
            if (d->own == 1) {
                char *dl = strstr(bp, d->text);
                if (!dl) continue;
                declAt = (size_t)(dl - bp);
                declInside = true;
                bufPutn(&bm, bp, declAt);
                bufPutn(&bm, dl + strlen(d->text), bl - declAt - strlen(d->text));
            } else {
                bufPutn(&bm, bp, bl);
            }
            cgAcc("cg-loc-bm", cgNow() - t_bm);
            g_locRead += (long long)bm.len;
            if (countReads(bufCstr(&bm), bm.len, d->name) != 0) continue;   /* it is read */
            (void)declInside;
            (void)declAt;
            /* collect the pieces to delete: the declaration itself and every assignment to it (when the
     * right-hand side contains a call, only the call is kept) */
            Vec cuts;
            vecInit(&cuts, g->arena, sizeof(LocalCut));
            char *dl;
            if (d->own == 0) {
                dl = NULL;
                size_t dtl = strlen(d->text);
                if (textIsOriginal) {
                    for (char *p2 = text; (size_t)(p2 - text) + dtl <= firstBody; p2++)
                        if (memcmp(p2, d->text, dtl) == 0) { dl = p2; break; }
                } else {
                    dl = strstr(text, d->text);
                }
            } else {
                dl = strstr(bp, d->text);
            }
            if (!dl || (d->own != 0 && dl >= bp + bl)) continue;
            {
                size_t ls = (size_t)(dl - text);
                while (ls > 0 && text[ls - 1] != '\n') ls--;
                size_t le = (size_t)(dl - text);
                while (le < len && text[le] != '\n') le++;
                if (le < len) le++;
                char *assign = NULL;
                for (size_t k = (size_t)(dl - text); k < le; k++)
                    if (text[k] == '=' && (k + 1 >= le || text[k + 1] != '=')) { assign = text + k; break; }
                LocalCut c;
                c.start = ls; c.end = le; c.repl = NULL;
                if (assign) {
                    size_t ws = 0;
                    while (ls + ws < (size_t)(assign - text) &&
                           (text[ls + ws] == ' ' || text[ls + ws] == '\t')) ws++;
                    size_t rhs = (size_t)(assign - text) + 1;
                    while (rhs < le && (text[rhs] == ' ' || text[rhs] == '\t')) rhs++;
                    if (textHasCall(text + rhs, le - rhs)) {
                        /* Keep the call, but its value must be discarded explicitly: the right-hand side is usually an
                         * unwrapping (`var f = fs::openWrite(p)!`), and the bare payload expression left
                         * behind is judged `unused-value` by gcc, which fails outright under
                         * `-Werror`. */
                        size_t re = le;
                        while (re > rhs && (text[re - 1] == ';' || text[re - 1] == '\n' ||
                                            text[re - 1] == ' ' || text[re - 1] == '\t')) re--;
                        Buf r;
                        bufInit(&r, g->arena);
                        bufPutn(&r, text + ls, ws);
                        bufPuts(&r, "(void)(");
                        bufPutn(&r, text + rhs, re - rhs);
                        bufPuts(&r, ");\n");
                        c.repl = bufCstr(&r);
                    }
                }
                *(LocalCut *)vecPush(&cuts) = c;
            }
            g_locLine += (long long)bl;
            for (char *p = bp; p < bp + bl; ) {
                char  *nl2 = memchr(p, '\n', (size_t)(bp + bl - p));
                size_t ll2 = nl2 ? (size_t)(nl2 - p) : (size_t)(bp + bl - p);
                size_t k = 0;
                while (k < ll2 && (p[k] == ' ' || p[k] == '\t')) k++;
                size_t nlen2 = strlen(d->name);
                if (ll2 > k + nlen2 && strncmp(p + k, d->name, nlen2) == 0) {
                    char *q = p + k + nlen2;
                    while (q < p + ll2 && *q == ' ') q++;
                    if (q < p + ll2 && *q == '=' && (q + 1 >= p + ll2 || q[1] != '=')) {
                        size_t ls = (size_t)(p - text), le = ls + ll2 + (nl2 ? 1 : 0);
                        size_t rhs = (size_t)(q - text) + 1;
                        while (rhs < ls + ll2 && (text[rhs] == ' ' || text[rhs] == '\t')) rhs++;
                        LocalCut c;
                        c.start = ls; c.end = le; c.repl = NULL;
                        if (textHasCall(text + rhs, le - rhs)) {
                            Buf r;
                            bufInit(&r, g->arena);
                            bufPutn(&r, text + ls, k);          /* the indentation */
                            bufPutn(&r, text + rhs, le - rhs);  /* the call, its `;` and the newline */
                            c.repl = bufCstr(&r);
                        }
                        *(LocalCut *)vecPush(&cuts) = c;
                    }
                }
                if (!nl2) break;
                p = nl2 + 1;
            }
            /* One assembly, in order: the spans are disjoint and ascending, and the result is
             * never longer than what it replaces, so it fits in the same buffer. */
            g_cuts++;
            double t_rw = cgNow();
            Buf nb;
            bufInit(&nb, g->arena);
            size_t prev = 0;
            for (size_t ci = 0; ci < cuts.len; ci++) {
                LocalCut *c = (LocalCut *)vecAt(&cuts, ci);
                if (c->start < prev) continue;                 /* overlapping: skip, stay safe */
                bufPutn(&nb, text + prev, c->start - prev);
                if (c->repl) bufPuts(&nb, c->repl);
                prev = c->end;
            }
            bufPutn(&nb, text + prev, len - prev);
            memcpy(out->data, bufCstr(&nb), nb.len);
            out->data[nb.len] = 0;
            out->len = nb.len;
            len = nb.len;
            text = out->data;
            textIsOriginal = 0;            /* positions are stale from here on: search again */
            cgAcc("cg-rewrite", cgNow() - t_rw);
            d->text = NULL;
            cut = true;
        }
        if (!cut) break;
    }
    *textp = text;
    *lenp = len;
}

/**
 * @brief Count how many times `name` occurs as a whole name in the generated code.
 *
 * String and character literals and comments are skipped whole: single-letter local names occur
 * inside literals too (a string containing `v = `, for instance), and that is not a use. The name
 * must be bounded by non-identifier bytes on both sides, so `e` does not hit the `e` inside `self`.
 *
 * @param hay   The text fragment to scan.
 * @param n     The fragment's byte count; no terminating NUL is required.
 * @param name  The name to count, NUL-terminated.
 * @return      The occurrence count.
 */
static size_t countCodeMentions(const char *hay, size_t n, const char *name) {
    strAccount("countCodeMentions", n);
    size_t len = strlen(name), count = 0;
    for (size_t i = 0; i < n; ) {
        char c = hay[i];
        if (c == '"' || c == '\'') {                 /* skip the literal */
            char q = c;
            i++;
            while (i < n) {
                if (hay[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (hay[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && hay[i + 1] == '/') {
            while (i < n && hay[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && hay[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(hay[i] == '*' && hay[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (i + len <= n && strncmp(hay + i, name, len) == 0 &&
            (i == 0 || !identByte(hay[i - 1])) && (i + len >= n || !identByte(hay[i + len]))) {
            count++;
            i += len;
            continue;
        }
        i++;
    }
    return count;
}

/**
 * @brief Count `name`'s occurrences in a text that is not NUL-terminated, counting code only.
 *
 * Literals and comments are skipped and the name must be bounded by non-identifier bytes; the name is
 * a byte range rather than a C string, so a body buffer can be sliced into it directly.
 *
 * @param g       The generation context; this function does not use it.
 * @param hay     The text fragment to scan.
 * @param n       The fragment's byte count; no terminating NUL is required.
 * @param needle  The name to count, NUL-terminated.
 * @return        The occurrence count.
 */
static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle) {
    strAccount("countMentionsIn", n);
    (void)g;
    return countCodeMentions(hay, n, needle);
}

/**
 * @brief Count how many times `name` occurs outside these spans.
 *
 * One `strstr` pass that skips the given spans is cheaper than copying the whole unit, cutting the
 * fragments out and counting in the copy. The spans are half-open, `[starts[k], ends[k])`; the name
 * must be whole, and mentions inside literals and comments count too -- that only keeps definitions
 * alive, which is the safe direction.
 *
 * @param hay     The whole NUL-terminated text.
 * @param name    The name to count, NUL-terminated.
 * @param starts  The span start array.
 * @param ends    The span end array, half-open.
 * @param nspans  The number of spans.
 * @return        The number of whole-name occurrences outside every span.
 */
static size_t countMentionsOutside(const char *hay, const char *name,
                                   const size_t *starts, const size_t *ends, size_t nspans) {
    size_t len = strlen(name), count = 0;
    /* `strstr` jumps from one occurrence to the next (vectorized); matching byte by byte with
     * `strncmp` would be much more expensive. Mentions inside literals or comments count too -- that
     * only keeps definitions alive, which is the safe direction. */
    for (const char *p = hay; (p = strstr(p, name)) != NULL; p += len) {
        size_t i = (size_t)(p - hay);
        if (i > 0 && identByte(hay[i - 1])) continue;
        if (identByte(p[len])) continue;
        bool inside = false;
        for (size_t k = 0; k < nspans; k++)
            if (i >= starts[k] && i < ends[k]) { inside = true; break; }
        if (!inside) count++;
    }
    return count;
}

/* Drop every recorded definition whose name occurs exactly once in the finished
 * output - that one occurrence being the definition itself.
 *
 * The text is searched for the definition rather than for a byte offset: the
 * unit is assembled by splicing several buffers together (prototypes, then the
 * descriptor pool, then the bodies), so an offset taken at emission time does
 * not survive. The definition text does.
 *
 * A name that occurs twice or more is kept, whoever mentions it - including
 * mentions that sit inside code that is itself unreachable. That keeps the
 * decision on the safe side: the worst case is a definition that is still
 * emitted, never a name that nothing defines. */
/* Occurrence counts of the names this phase decides on. `countMentions(text, name)` runs once per
 * candidate over the whole unit -- the scan counters put that at 11.4 GB of scanning for N=3200
 * (x3.85 per doubling, the largest single cost left). Same numbers, maintained instead of
 * re-scanned: one pass fills them and every removal subtracts the runs inside the text it deletes,
 * so the decisions -- and the emitted C -- are byte for byte the same. Only the names asked about
 * live in the table, so a scan never inserts and the table cannot fill up. */
typedef struct { const char *name; size_t len; long count; } NameCount;
typedef struct { NameCount *slot; size_t mask; } CountTable;
/**
 * @brief Compute the FNV-1a hash of a byte range for the open addressing of `CountTable`.
 *
 * The length is given by `n` and no terminating NUL is required, so a name can be a slice of the text.
 *
 * @param p  The start of the bytes.
 * @param n  The byte count.
 * @return   The hash value; the caller masks it to pick a slot.
 */
static size_t countHash(const char *p, size_t n) {
    size_t h = 1469598103934665603u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211u; }
    return h;
}
/**
 * @brief Initialize `CountTable`'s slot array with a power-of-two capacity of at least `2n`.
 *
 * The slots are zero-allocated from the arena and an empty slot is recognized by `name == NULL`. The
 * table only holds the names the caller registered first and no insertion happens while counting
 * afterwards, so it cannot fill up.
 *
 * @param t  The table to initialize.
 * @param a  The arena the slot array is allocated from.
 * @param n  The expected number of registered names.
 */
static void countInit(CountTable *t, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    t->slot = arenaAllocZero(a, cap * sizeof *t->slot);
    t->mask = cap - 1;
}
/**
 * @brief Register a name in the name counter table, starting its count at 0.
 *
 * The table is sized once in `countInit` from the number of candidate names, so this function only
 * probes empty slots and can never fill up; an entry stores the caller's name pointer without copying
 * the text, so that pointer must outlive the table.
 *
 * @param t  The name counter table; must have been initialized by `countInit`.
 * @param p  The name text, `n` bytes long, not necessarily NUL-terminated.
 * @param n  The name's byte count.
 */
static void countPut(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) { s->name = p; s->len = n; s->count = 0; return; }
        if (s->len == n && memcmp(s->name, p, n) == 0) return;
    }
}
/**
 * @brief Add `delta` to one name's count.
 *
 * A name not in the table returns immediately: the table only holds names that have been registered,
 * and the scan never inserts a new entry.
 *
 * @param t      The name counter table.
 * @param p      The name text, `n` bytes long.
 * @param n      The name's byte count.
 * @param delta  The count increment, possibly negative (subtracting the occurrences from a text that
 *               is about to be deleted).
 */
static void countBump(CountTable *t, const char *p, size_t n, long delta) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return;
        if (s->len == n && memcmp(s->name, p, n) == 0) { s->count += delta; return; }
    }
}
/**
 * @brief Get a name's current count.
 *
 * @param t  The name counter table.
 * @param p  The name text, `n` bytes long.
 * @param n  The name's byte count.
 * @return   The count in the table; 0 when the name is not there.
 */
static long countGet(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return 0;
        if (s->len == n && memcmp(s->name, p, n) == 0) return s->count;
    }
}
/**
 * @brief Adjust every name occurring in a text by `delta`.
 *
 * Only identifiers are split: a name cut at either end of the range does not count, it belongs to the
 * identifier outside. Used to subtract a piece of text that is about to be deleted from the counts.
 *
 * @param t      The name counter table.
 * @param text   The whole text, covering at least `[start, start + len)`.
 * @param start  The range's starting offset.
 * @param len    The range's byte count.
 * @param delta  The count increment, possibly negative.
 */
static void countSpan(CountTable *t, const char *text, size_t start, size_t len, long delta) {
    size_t i = 0;
    while (i < len) {
        if (!identByte(text[start + i])) { i++; continue; }
        size_t s0 = i;
        while (i < len && identByte(text[start + i])) i++;
        if (s0 == 0 && start > 0 && identByte(text[start - 1])) continue;
        if (i == len && identByte(text[start + len])) continue;
        countBump(t, text + start + s0, i - s0, delta);
    }
}

/**
 * @brief Delete definitions, prototypes and local declarations that nothing references from the
 *        assembled generated unit.
 *
 * A name that occurs in the finished text only at its own definition is unused. The judgment is a
 * fixed point: in one round a definition may be mentioned only by another definition that is itself
 * about to be deleted, so one round is not enough. Offsets move with every deletion, so the text is
 * located again on each pass. The judgment errs on the conservative side: any other mention keeps the
 * definition, even when that piece of code is dead itself.
 *
 * @param g    The code generation state; the records in `deadDefs` / `deadFuncs` / `deadLocals` are
 *             the evidence.
 * @param out  The generated C unit; deleted in place, with `out->len` shrinking accordingly.
 */
static void dropUnreferenced(CG *g, Buf *out) {
    char  *text = bufCstr(out);              /* terminate: the searches below are C strings */
    size_t len  = out->len;
    /* Definitions inside a body first, from the end backwards, so that removing one
    * leaves the offsets of the ones still to come untouched. What can name such a
    * definition is the enclosing piece of code and nothing else - a payload binding
    * is read by its arm - so the count happens inside that scope: `e` and `n` are
    * common names, and counting them across the whole unit would always find more
    * than one. */
    for (size_t i = g->deadDefs.len; i-- > 0; ) {
        DeadDef *d = *(DeadDef **)vecAt(&g->deadDefs, i);
        if (!d->scoped) continue;                                      /* top-level: later */
        if (countMentionsIn(g, g->body.data + d->scopeA, d->scopeB - d->scopeA, d->name)) continue;
        size_t tl = strlen(d->text);
        size_t at = g->bodyOff + d->off;
        if (at + tl > len || memcmp(text + at, d->text, tl) != 0) continue;   /* be sure */
        memmove(text + at, text + at + tl, len - at - tl + 1);
        if (dbgOn("EXTC_DBG_DCE")) fprintf(stderr, "[dce] scoped %s\n", d->name);
        len -= tl;
        out->len = len;
    }
    /* Local declarations that nothing reads: after the offset-based phase, before the
     * phases that remove whole functions (a local of a function that is gone needs no
     * decision). */
    CGACC("cg-locals", dropUnusedLocals(g, out, &text, &len));
    /* Functions nobody calls. Both halves are located by their own text, so what is
    * removed is exactly what was captured - never a piece of a function. The
    * definition sits after the declaration, so it goes first and the declaration's
    * position stays valid.
    *
    * A use is a mention *outside* the declaration and the definition, so a function
    * whose only other mention is a call to itself is not used. A mutually recursive
    * pair with no entry point has no such mention either way and is kept. */
    CountTable counts;
    countInit(&counts, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        if (k + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, k + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (df->name) countPut(&counts, df->name, strlen(df->name));
    }
    countSpan(&counts, text, 0, len, 1);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        if (i + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, i + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;                                   /* no definition emitted */
        if (df->namedByDynTable) continue;   /* named by a `dyn` table, which is spliced in later */
        size_t nlen = strlen(df->name);
        long   total = countGet(&counts, df->name, nlen);
        if (total < 2) continue;                                   /* no declaration + definition */
        char  *pt = strstr(text, df->proto);
        /* The definition is located by its signature line: the passes above rewrite the inside of
        * bodies, so the copy taken at generation time can stop matching the text. */
        size_t blen = 0;
        char  *bd = funcDefStart(text, df, &blen);
        if (!pt || !bd || bd < pt || !blen) continue;               /* not both, in order */
        size_t bl = blen, pl = strlen(df->proto);
        /* `total == 2` is the fast path: declaration and definition, nothing else names it. A
         * larger count only reaches the decision when the extra mentions are all inside the two
         * pieces - that is the self-call case above. */
        if (total != 2) {
            size_t inside = countMentionsIn(g, bd, bl, df->name)
                          + countMentionsIn(g, pt, pl, df->name);
            if ((size_t)total != inside) continue;                 /* someone else names it */
        }
        countSpan(&counts, text, (size_t)(bd - text), bl, -1);
        countSpan(&counts, text, (size_t)(pt - text), pl, -1);
        memmove(bd, bd + bl, len - (size_t)(bd - text) - bl + 1);    /* definition first */
        len -= bl;
        memmove(pt, pt + pl, len - (size_t)(pt - text) - pl + 1);    /* then declaration */
        len -= pl;
        if (g->bodyOff > (size_t)(pt - text)) g->bodyOff -= pl;
        out->len = len;
        if (dbgOn("EXTC_DBG_DCE")) fprintf(stderr, "[dce] func %s\n", df->name);
        df->body = NULL;
    }
    /* The runtime primitive block is not decided here: it is assembled beside the unit and
    * decided from the registry (`primSplice`, called from generateC once the rest of the
    * pipeline has settled). Nothing below this line scans for a definition. */
    /* Top-level definitions, one name at a time.
    *
    * The test is on a scratch copy of the unit: every piece registered under the name is cut
    * out of the copy, and if the name is not mentioned anywhere else, the pieces are the only
    * places it appears - so nothing uses it and they can go.
    *
    * Everything the name appears in is its own pieces: nothing uses it. */
    bool dropped = true;
    while (dropped) {
        dropped = false;
        for (size_t i = 0; i < g->deadDefs.len; i++) {
            if (i + 8 < g->deadDefs.len) __builtin_prefetch(*(DeadDef **)vecAt(&g->deadDefs, i + 8), 0, 0);
            DeadDef *d = *(DeadDef **)vecAt(&g->deadDefs, i);
            if (d->scoped || !d->text) continue;            /* inside a body, or already gone */
            size_t starts[8], ends[8], pieces = 0;
            bool   allThere = true;
            for (size_t k = 0; k < g->deadDefs.len; k++) {
                if (k + 8 < g->deadDefs.len) __builtin_prefetch(*(DeadDef **)vecAt(&g->deadDefs, k + 8), 0, 0);
                DeadDef *o = *(DeadDef **)vecAt(&g->deadDefs, k);
                if (o->scoped || !o->text || strcmp(o->name, d->name) != 0) continue;
                char  *p = strstr(text, o->text);
                if (!p) { allThere = false; break; }        /* a piece is not in the unit */
                if (pieces < 8) {                            /* more than eight: leave it alone */
                    starts[pieces] = (size_t)(p - text);
                    ends[pieces]   = starts[pieces] + strlen(o->text);
                }
                pieces++;
            }
            if (!allThere || pieces == 0 || pieces > 8) continue;
            /* Everything the name appears in is its own pieces: nothing uses it. */
            if (countMentionsOutside(text, d->name, starts, ends, pieces) != 0) continue;
            for (size_t k = 0; k < g->deadDefs.len; k++) {  /* remove them for real */
                DeadDef *o = *(DeadDef **)vecAt(&g->deadDefs, k);
                if (o->scoped || !o->text || strcmp(o->name, d->name) != 0) continue;
                size_t ol = strlen(o->text);
                char  *at = strstr(text, o->text);
                if (at) {
                    memmove(at, at + ol, len - (size_t)(at - text) - ol + 1);
                    len -= ol;
                    out->len = len;
                    text = bufCstr(out);
                }
                o->text = NULL;
            }
            dropped = true;
            break;                                          /* the text moved: start over */
        }
    }
    out->data[len] = '\0';
}

/**
 * @brief Make sure the `EXTC_UNUSED` attribute macro is still defined in the buffer the attribute is
 *        inserted into.
 *
 * The attribute macro is pruned before this runs, and with no use in sight it deletes the definition,
 * after which the inserted attribute would point at a macro that is gone. The definition is
 * inserted back into the `#if defined(__GNUC__)` block that holds it; when even the anchor is gone the
 * function returns false and the caller gives up the insertion instead of emitting an attribute the
 * compiler does not know.
 *
 * @param g   The code generation state, which provides the arena for the temporary buffer.
 * @param nb  The unit text the attribute is inserted into; the definition is inserted in place.
 * @return    true when the definition is already in the text or was inserted by this call; false when
 *            none of the three anchors is found.
 */
static bool ensureUnusedMacro(CG *g, Buf *nb) {
    if (strstr(nb->data, "#define EXTC_UNUSED")) return true;
    const char *anchors[3];
    anchors[0] = "#define EXTC_INLINE";          /* preferred: right next to its sibling */
    anchors[1] = "#if defined(__GNUC__)";        /* the block it belongs to */
    anchors[2] = NULL;
    for (int i = 0; anchors[i]; i++) {
        char *a = strstr(nb->data, anchors[i]);
        if (!a) continue;
        char *nl = strchr(a, '\n');
        if (!nl) continue;
        size_t ins = (size_t)(nl - nb->data) + 1;
        Buf t;
        bufInit(&t, g->arena);
        bufPutn(&t, nb->data, ins);
        bufPuts(&t, "#define EXTC_UNUSED __attribute__((unused))\n");
        bufPutn(&t, nb->data + ins, nb->len - ins);
        nb->len = 0;
        bufPutn(nb, t.data, t.len);
        return true;
    }
    return false;
}

/**
 * @brief Locate a function definition's start and end in the generated text.
 *
 * The recorded function text is matched as a whole first; when an earlier rewrite pass has moved it,
 * the search falls back to the signature line. The definition ends at the `}` in column zero -- every
 * generated definition closes that way. When it cannot be located the function returns NULL, so the
 * caller skips that function instead of deleting the wrong thing.
 *
 * @param text    The assembled generated unit's text, writable.
 * @param df      The function record; returns NULL when `df->body` is NULL (the definition was not
 *                emitted).
 * @param lenOut  Receives the definition text's byte count, from the start to the closing `}`.
 * @return        The definition's start; NULL when it is not found.
 */
static char *funcDefStart(char *text, DeadFunc *df, size_t *lenOut) {
    if (!df->body) return NULL;
    char *at = strstr(text, df->body);              /* untouched: the whole text is the key */
    if (!at) {
        /* Touched by the passes above, so only the signature line can be relied on. */
        const char *nl = strchr(df->body, '\n');
        size_t sigLen = nl ? (size_t)(nl - df->body) : strlen(df->body);
        if (!sigLen) return NULL;
        for (char *p = text; *p; p++) {
            if (p != text && p[-1] != '\n') continue;
            if (strncmp(p, df->body, sigLen) == 0) { at = p; break; }
        }
    }
    if (!at) return NULL;
    char *e = at;
    for (char *p = at; *p; p++)   /* `p != at`: `p[-1]` below must not read before the buffer */
        if (p != at && p[0] == '}' && p[-1] == '\n') { e = p + 1; break; }
    *lenOut = (size_t)(e - at);
    return at;
}

/**
 * @brief Order insertion points from the end of the unit to its start.
 *
 * One inserted attribute moves every later offset, so insertion has to run backwards. The points are
 * collected function by function, which is not their order in the text (a prototype emitted early
 * can describe a body emitted late).
 *
 * @param a  A `size_t` offset.
 * @param b  Another `size_t` offset.
 * @return   Negative when `a` comes later in the text.
 */
static int cmpInsertDesc(const void *a, const void *b) {
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;
    return x < y ? 1 : (x > y ? -1 : 0);
}

/**
 * @brief Add `EXTC_UNUSED` to the declaration of a function that has no call site at all.
 *
 * The attribute asserts "nobody calls this", and on a function that is called clang reports
 * `-Wused-but-marked-unused`, once per call site. The condition holds when the name occurs in the
 * finished text only in its own declaration and definition. All insertion points are collected first
 * and applied from the end backwards, with the insertion itself done last, because inserting one moves
 * every later offset.
 *
 * @param g    The code generation state; `deadFuncs` is the source of candidates.
 * @param out  The generated C unit; the attributes are inserted in place.
 */
static void markUncalledFunctions(CG *g, Buf *out) {
    char  *text = out->data;
    size_t len  = out->len;
    Vec    at;
    vecInit(&at, g->arena, sizeof(size_t));
    /* One pass over the finished text fills the counts the loop below asks for. Counting by
     * scanning per function is O(N^2): the strstr site histogram put 23.4 GB of scanning on the
     * per-name query here at N=1600, and the per-function instruction diff showed the whole run is
     * 75% inside strstr. The text does not change while the loop runs -- it only collects offsets
     * -- so the counts are the same numbers, byte for byte. */
    CountTable counts;
    countInit(&counts, g->arena, g->deadFuncs.len + 1);
    for (size_t k = 0; k < g->deadFuncs.len; k++) {
        if (k + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, k + 8), 0, 0);
        DeadFunc *dfk = *(DeadFunc **)vecAt(&g->deadFuncs, k);
        if (dfk->name) countPut(&counts, dfk->name, strlen(dfk->name));
    }
    countSpan(&counts, text, 0, len, 1);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        if (i + 8 < g->deadFuncs.len) __builtin_prefetch(*(DeadFunc **)vecAt(&g->deadFuncs, i + 8), 0, 0);
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;
        size_t blen = 0;
        char *bd = funcDefStart(text, df, &blen);
        if (!bd) continue;
        char *pt = strstr(text, df->proto);
        size_t own = 1;                                  /* the definition always mentions it */
        if (pt && pt < bd) own++;
        if (bd) (void)blen;
        if (((size_t)countGet(&counts, df->name, strlen(df->name))) != own) continue;   /* something calls it: leave it alone */
        /* The attribute goes at the start of the declaration. Looking for the literal `static `
        * would miss every `@inline` function: those are declared through the `EXTC_INLINE` macro,
        * so no `static` is spelled out. */
        char *decl = pt ? pt : bd;
        while (*decl == ' ') decl++;
        if (decl >= text + len) continue;
        *(size_t *)vecPush(&at) = (size_t)(decl - text);
    }
    if (!at.len) return;

    if (!strstr(text, "#define EXTC_UNUSED") && !strstr(text, "#define EXTC_INLINE") &&
        !strstr(text, "#if defined(__GNUC__)")) return;
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, text, len);
    qsort(at.data, at.len, sizeof(size_t), cmpInsertDesc);
    for (size_t k = 0; k < at.len; k++) {
        size_t off = *(size_t *)vecAt(&at, k);
        Buf tmp;
        bufInit(&tmp, g->arena);
        bufPutn(&tmp, nb.data, off);
        bufPuts(&tmp, "EXTC_UNUSED ");
        bufPutn(&tmp, nb.data + off, nb.len - off);
        nb.len = 0;
        bufPutn(&nb, tmp.data, tmp.len);
    }
    ensureUnusedMacro(g, &nb);
    out->data = bufCstr(&nb);
    out->len  = nb.len;
    out->cap  = nb.len + 1;
}

/**
 * @brief Add `EXTC_UNUSED` to a parameter the function body never reads.
 *
 * A parameter cannot be deleted, it is part of the signature, and the attribute is the only way to say
 * "this parameter is deliberately unused". The parameter list's range is the first `(` of the
 * definition up to its matching `)`, split at top-level commas, and each name is counted only inside
 * **this** function's body -- counting to the end of the file would take another function's parameter
 * of the same name as a use. The attribute is written at the parameter's start rather than before the
 * name: after a preceding type it would attach to a pointer type and gcc would still report the
 * parameter as unused. Insertion points are collected first and applied from the end backwards, with
 * the insertion itself done last.
 *
 * @param g    The code generation state; every body in `deadFuncs` is a text to check.
 * @param out  The generated C unit; the attributes are inserted in place.
 */
static void markUnusedParams(CG *g, Buf *out) {
    char  *text = out->data;
    size_t len  = out->len;
    Vec    at;
    vecInit(&at, g->arena, sizeof(size_t));
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;
        size_t bl = 0;
        char  *fn = funcDefStart(text, df, &bl);
        if (!fn) continue;
        char  *op = memchr(fn, '(', bl);
        if (!op) continue;
        int    depth = 0;
        char  *cl = NULL;
        for (char *p = op; p < fn + bl; p++) {
            if (*p == '(') depth++;
            else if (*p == ')' && --depth == 0) { cl = p; break; }
        }
        if (!cl) continue;
        char *ps = op + 1;
        int   d2 = 0;
        for (char *p = ps; p <= cl; p++) {
            if (*p == '(' || *p == '[') d2++;
            else if (*p == ')' || *p == ']') d2--;
            if (!((p == cl) || (*p == ',' && d2 == 0))) continue;
            char *ns = p;                                   /* the parameter is [ps, p) */
            while (ns > ps && ns[-1] == ' ') ns--;
            char *ne = ns;
            while (ne > ps && identByte(ne[-1])) ne--;
            if (ne < ns && (size_t)(ns - ne) < 64) {
                char nm[64];
                memcpy(nm, ne, (size_t)(ns - ne));
                nm[ns - ne] = 0;
                /* `f(void)` has no parameters, and `void` is not one: marking it makes clang say
                 * the attribute cannot be applied to a `void` parameter. */
                if (strcmp(nm, "void") == 0) { ps = p + 1; continue; }
                /* Counted inside *this* function's body, not to the end of the file: another
                 * function's own `__extc_home` parameter would otherwise count as a use here. */
                if (countCodeMentions(cl + 1, (size_t)((fn + bl) - (cl + 1)), nm) == 0) {
                    /* At the *start* of the parameter, not before the name: written as
                     * `int32_t * __attribute__((unused)) b` the attribute lands on the pointer
                     * type and gcc still reports the parameter. */
                    char *pstart = ps;
                    while (pstart < p && *pstart == ' ') pstart++;
                    *(size_t *)vecPush(&at) = (size_t)(pstart - text);
                }
            }
            ps = p + 1;
        }
    }
    if (!at.len) return;

    if (!strstr(text, "#define EXTC_UNUSED") && !strstr(text, "#define EXTC_INLINE") &&
        !strstr(text, "#if defined(__GNUC__)")) return;
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, text, len);
    qsort(at.data, at.len, sizeof(size_t), cmpInsertDesc);
    for (size_t k = 0; k < at.len; k++) {
        size_t off = *(size_t *)vecAt(&at, k);
        Buf tmp;
        bufInit(&tmp, g->arena);
        bufPutn(&tmp, nb.data, off);
        bufPuts(&tmp, "EXTC_UNUSED ");
        bufPutn(&tmp, nb.data + off, nb.len - off);
        nb.len = 0;
        bufPutn(&nb, tmp.data, tmp.len);
    }
    ensureUnusedMacro(g, &nb);
    out->data = bufCstr(&nb);
    out->len  = nb.len;
    out->cap  = nb.len + 1;
}

/* Is this method an implementation named by a static method table?
 *
 * Such a function is a **root**: nothing in the program calls it, yet the table refers to it, so
 * it has to be emitted and must not be pruned as unreferenced. The reachability filter below only
 * keeps `used` functions (plus `main`), which is why the first tables emitted nothing that
 * resolved -- `'box_zeta' undeclared` in the generated C was how that showed up. Each of the two
 * seeding loops (methods, free functions) asks this same question, so it lives here once.
 *
 * The owner is compared first, which keeps a same-named method of another type out; the name
 * comparison is there because codegen can hold its own copy of a declaration (instances,
 * substituted signatures). */
/**
 * @brief A type's method set (the struct the method table hangs off).
 *
 * When the type has no function body of its own, `Type.mholder` is used -- that covers the built-in
 * scalars (`impl i64 { ... }`) and generic **instances** (`impl slice<u8> { ... }`); otherwise the
 * declared `sdef` is used. Every pass that asks "does this type have this method" must go through
 * here, and overlooking `mholder` misses those two classes of methods.
 *
 * @param t  The type; may be NULL.
 * @return   The method set's struct definition; NULL when `t` is NULL or both are empty.
 */
static StructDef *methodSetOf(Type *t) {
    if (!t) return NULL;
    return t->mholder ? t->mholder : t->sdef;
}

/**
 * @brief Is a function pointed to by some trait impl's method table?
 *
 * Such a function is a root of the reachability analysis: the program has no call site for it, but a
 * table references it, so it must be emitted and cannot be pruned as unused. The method set is
 * compared first (`methodSetOf`), which keeps same-named methods of other types out; then the name is
 * compared again, because codegen may hold a copy of the declaration (an instance, a substituted
 * signature). The function matches when both the method set and the name match.
 *
 * @param m  The module holding the trait impls; the method tables come from `m->impls`.
 * @param f  The candidate function; returns false immediately when `f->owner` is NULL (a free
 *           function).
 * @return   true when a method table points at `f`.
 */
static bool inTraitTable(Module *m, FuncDef *f) {
    if (!f->owner) return false;
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        Type *bt = ttBase(planImplTarget(im));
        if (!bt || methodSetOf(bt) != f->owner) continue;
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            if (mth == f) return true;
            if (mth->name && f->name && strcmp(mth->name, f->name) == 0) return true;
        }
    }
    return false;
}


static const PrimPiece PRIM_BLOCK[] = {
    { PRIM_SCAFFOLD, NULL, "/* Every path that ends the process on purpose goes through here. A trap path is\n * the only code that still runs when a program dies, so this is where anything the\n * process has to give back is given back. Today that is the terminal: a program\n * that traps in raw mode would otherwise leave the user's shell unable to echo\n * what they type. The hook is installed by `extc_raw_enter` (emitted only when the\n * program uses the raw-terminal primitives) and cleared before it runs, so it\n * cannot run twice. */\n" },
    { PRIM_VAR, "__extc_dying", "static int32_t (*__extc_dying)(void);\n" },
    { PRIM_SCAFFOLD, NULL, "#if defined(__GNUC__) || defined(__clang__)\n" },
    { PRIM_MACRO, NULL, "#  define EXTC_NORETURN __attribute__((noreturn))\n" },
    { PRIM_SCAFFOLD, NULL, "#else\n" },
    { PRIM_MACRO, NULL, "#  define EXTC_NORETURN _Noreturn\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_die", "EXTC_NORETURN static inline void extc_die(int code) {\n    if (__extc_dying) { int32_t (*f)(void) = __extc_dying; __extc_dying = 0; (void)f(); }\n    exit(code);\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Every primitive below is `static inline`, and that is not a style choice:\n * without inlining, gcc at -O1 cannot see the body of a check, so it can neither\n * eliminate the check nor turn `i % 7` into a multiply and shift. Measured: the\n * remainder operator was 4.6x slower and a matrix multiply 1.5x slower; with\n * inlining both match C. */\n/* Out-of-range trap; the position comes from the call site via `#line`. */\n" },
    { PRIM_FUNC, "extc_trap", "EXTC_NORETURN static inline void extc_trap(const char *file, int line, int64_t i, int64_t n) {\n    fprintf(stderr, \"%s:%d: trap: index %lld out of range (length %lld)\\n\",\n            file, line, (long long)i, (long long)n);\n    extc_die(1);\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Arithmetic failures must be loud: division by zero, the overflowing division, and\n * an over-wide shift are all undefined behaviour in C. Each one traps with a\n * source position instead of computing a wrong answer or leaving UB behind. */\n" },
    { PRIM_FUNC, "extc_trapMsg", "EXTC_NORETURN static inline void extc_trapMsg(const char *file, int line, const char *msg) {\n    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, msg);\n    extc_die(1);\n}" },
    /* The two `!` assertions. Each carries its comment **inside** its definition: a separate
     * comment row would be emitted unconditionally and change the generated C of every
     * program, including the ones that never write `!`. The comment names no primitive, so
     * it cannot keep one alive through the mention count. */
    { PRIM_FUNC, "extc_refAssert", "/* `!` on a nullable reference: the signature is checked at runtime, and a wrong\n * one traps with a source position. The pointer is returned, so the operand is\n * evaluated once. An `@unchecked` body never calls this at all. */\nstatic inline void *extc_refAssert(void *p, const char *file, int line) {\n    if (!p) extc_trapMsg(file, line, \"the nullable reference was asserted non-null but is null\");\n    return p;\n}" },
    { PRIM_FUNC, "extc_tagAssert", "/* `!` on an `option` / `result`: the tag is asserted before the payload is read,\n * so a `none` / failed arm traps with a position instead of handing back a zero\n * value. The tag is returned, so the operand is evaluated once. */\nstatic inline int extc_tagAssert(int tag, int want, const char *file, int line) {\n    if (tag != want) extc_trapMsg(file, line, \"the variant was asserted to hold a value but is none or failure\");\n    return tag;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n#ifndef EXTC_REC_LIMIT\n" },
    { PRIM_MACRO, NULL, "#define EXTC_REC_LIMIT 100000\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_VAR, "__extc_rec_depth", "static int64_t __extc_rec_depth = 0;\n" },
    { PRIM_FUNC, "extc_rec_enter", "static inline void extc_rec_enter(const char *f, int l) {\n    if (++__extc_rec_depth > EXTC_REC_LIMIT)\n        extc_trapMsg(f, l, \"recursion too deep (unbounded recursion?)\");\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_divI", "static inline int64_t extc_divI(int64_t a, int64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n    return a / b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_modI", "static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n    return a % b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_divU", "static inline uint64_t extc_divU(uint64_t a, uint64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    return a / b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_modU", "static inline uint64_t extc_modU(uint64_t a, uint64_t b, const char *f, int l) {\n    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n    return a % b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Shifts: shifting by the width or more, or by a negative amount, is undefined\n * behaviour in C, so the count is checked and returned (evaluated once). */\n" },
    { PRIM_FUNC, "extc_shiftCount", "static inline int64_t extc_shiftCount(int64_t b, int64_t w, const char *f, int l) {\n    if (b < 0 || b >= w) extc_trapMsg(f, l, \"shift count out of range\");\n    return b;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Checked index: the index is returned, so the caller evaluates it only once. */\n" },
    { PRIM_FUNC, "extc_checkedIndex", "static inline int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line) {\n    if (i < 0 || i >= n) extc_trap(file, line, i, n);\n    return i;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Checked slice: requires 0 <= lo <= hi <= n, and returns `lo`. */\n" },
    { PRIM_FUNC, "extc_checkedRange", "static inline int64_t extc_checkedRange(int64_t lo, int64_t hi, int64_t n,\n                          const char *file, int line) {\n    if (lo < 0 || hi < lo || hi > n) {\n        fprintf(stderr, \"%s:%d: trap: slice %lld..%lld is out of range (length %lld)\\n\",\n                file, line, (long long)lo, (long long)hi, (long long)n);\n        extc_die(1);\n    }\n    return lo;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n\n" },
    { PRIM_VAR, "extc_ablock", "typedef struct extc_ablock { struct extc_ablock *prev; int fresh; int64_t cap, used; char data[1]; } extc_ablock;\n" },
    { PRIM_VAR, "extc_arena", "typedef struct extc_arena { extc_ablock *top; extc_ablock *spare; } extc_arena;\n" },
    /* `any` is a by-value type in signatures (`fn log(v: any)`), so its typedef has to be in the
     * preamble, ahead of the prototypes; the struct itself is emitted with the rest of the `any`
     * runtime, before any body uses it. One row, mentioned by name, so a program without `any`
     * never carries it. */
    { PRIM_VAR, "ExtcAny", "typedef struct ExtcDesc ExtcDesc;\ntypedef struct ExtcAny { const struct ExtcDesc *desc; uint64_t u; } ExtcAny;\n" },
    { PRIM_SCAFFOLD, NULL, "#ifndef EXTC_ARENA_SPARE_MAX\n" },
    { PRIM_MACRO, NULL, "#define EXTC_ARENA_SPARE_MAX (1 << 20)\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_arena_init", "static inline void extc_arena_init(extc_arena *a) { a->top = NULL; a->spare = NULL; }\n" },
    { PRIM_FUNC, "extc_arena_release", "static inline void extc_arena_release(extc_arena *a) {\n    while (a->top) {\n        extc_ablock *p = a->top->prev;\n        /* Always keep one block back. Guarding this with \"the arena had more\n         * than one block\" would look like a saving and be the opposite: the\n         * block that pays off is precisely the single-block arena of a tight\n         * loop, where reusing one block avoids a malloc and a free per\n         * iteration. */\n        if (!a->spare && a->top->cap <= EXTC_ARENA_SPARE_MAX) {\n            a->spare = a->top; a->spare->prev = NULL; a->spare->fresh = 0;\n        } else {\n            free(a->top);\n        }\n        a->top = p;\n    }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_arena_destroy", "static inline void extc_arena_destroy(extc_arena *a) {\n    extc_arena_release(a);\n    if (a->spare) { free(a->spare); a->spare = NULL; }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* Explicit conversions: narrowing, sign change, or float-to-integer. A value that\n * does not fit traps, reporting the source position. */\n" },
    { PRIM_FUNC, "extc_narrowI", "static inline int64_t extc_narrowI(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {\n    if (v < lo || v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n    return v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_narrowU", "static inline uint64_t extc_narrowU(uint64_t v, uint64_t hi, const char *f, int l) {\n    if (v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n    return v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n" },
    { PRIM_FUNC, "extc_convFloat", "static inline int64_t extc_convFloat(double v, int64_t lo, int64_t hi, const char *f, int l) {\n    if (!(v >= (double)lo && v <= (double)hi)) extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n    return (int64_t)v;   /* truncation toward zero, as in C */\n}" },
    { PRIM_SCAFFOLD, NULL, "\n/* float -> u64: the range cannot be expressed with the int64_t bounds above\n * (UINT64_MAX does not fit in int64_t), so it is its own helper. 2^64 is\n * exactly representable as a double, which is why the upper test is `<`. */\n" },
    { PRIM_FUNC, "extc_convFloatU", "static inline uint64_t extc_convFloatU(double v, const char *f, int l) {\n    if (!(v >= 0.0 && v < 18446744073709551616.0)) extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n    return (uint64_t)v;\n}" },
    { PRIM_SCAFFOLD, NULL, "\n#if defined(__GNUC__) || defined(__clang__)\n" },
    { PRIM_MACRO, NULL, "#define EXTC_INLINE static inline __attribute__((always_inline))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_UNUSED __attribute__((unused))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_MALLOC __attribute__((malloc))\n" },
    { PRIM_MACRO, NULL, "#define EXTC_NOINLINE __attribute__((noinline))\n" },
    { PRIM_SCAFFOLD, NULL, "#else\n" },
    { PRIM_MACRO, NULL, "#define EXTC_INLINE static inline\n" },
    { PRIM_MACRO, NULL, "#define EXTC_MALLOC\n" },
    { PRIM_MACRO, NULL, "#define EXTC_NOINLINE\n" },
    { PRIM_SCAFFOLD, NULL, "#endif\n" },
    { PRIM_FUNC, "extc_arena_alloc", "static EXTC_MALLOC EXTC_NOINLINE void *extc_arena_alloc(extc_arena *a, int64_t n, const char *f, int l) {\n    if (n <= 0) n = 1;\n    n = (n + 7) & ~(int64_t)7;\n    /* Adopt the spare only when the arena is empty.\n     *\n     * This is one test on a pointer that is NULL in the common case, which\n     * matters: an allocation in a loop runs this code millions of times, and\n     * asking 'is the spare big enough for n' on every call costs extra work for\n     * no benefit -- the spare\n     * is a whole block, so `cap >= n` is already true whenever the arena is\n     * empty enough to want it. */\n    if (!a->top && a->spare) { a->top = a->spare; a->spare = NULL; a->top->used = 0; }\n    if (!a->top || a->top->cap - a->top->used < n) {\n        /* Block size: the request, grown by doubling, with a small floor.\n         *\n         * A flat floor costs that much per **task**: a coroutine's frame is ~100 bytes, so a\n         * flat 4096 floor costs 4 KB per task. Doubling keeps the malloc count of an allocation loop the same\n         * after a few blocks while a one-shot allocation (the common case in a coroutine)\n         * pays only for what it asks. */\n        int64_t cap = n > 64 ? n : 64;\n        if (a->top && a->top->cap < (INT64_C(1) << 20) && cap < a->top->cap * 2)\n            cap = a->top->cap * 2;\n        extc_ablock *b = (extc_ablock *)calloc(1, sizeof(extc_ablock) + (size_t)cap);\n        if (!b) { fprintf(stderr, \"%s:%d: trap: out of arena memory\"\n                        \" (this allocation wanted %lld bytes)\\n\",\n                        f, l, (long long)n); extc_die(1); }\n        b->prev = a->top; b->cap = cap; b->used = 0; b->fresh = 1;\n        /* `fresh` goes before `cap`/`used`: right before `data` it pushed the payload to\n         * offset 28, every allocation became 4-byte aligned, and storing an int64_t there\n         * is a misaligned store. */\n        a->top = b;\n    }\n    {\n        void *p = a->top->data + a->top->used;\n        a->top->used += n;\n        /* A block straight from `calloc` is zero **throughout**, and only the bytes not yet\n         * handed out are ever looked at again, so those need no clearing; a block that came\n         * back from the spare was written to in its previous life and does. The promise is\n         * unchanged either way: a byte read after its lifetime ends is an initialized byte\n         * (SPEC section 0.6). Skipping it matters for the one-shot large buffer: a 32 MiB\n         * `new` pays only for what it touches, since a fresh block is\n         * already zero and needs no `memset`. */\n        if (!a->top->fresh) memset(p, 0, (size_t)n);\n        return p;\n    }\n}" },
    { PRIM_SCAFFOLD, NULL, "\n\n" },
};

/**
 * @brief Register the run-time primitive blocks as a table in emission order.
 *
 * `PRIM_BLOCK` is the block itself: scaffolding and definitions alternate. Each definition is
 * registered here with its name and kind, so the later dead-alive judgment reads the registration
 * instead of guessing from line shapes; a macro line's name is read out of the `#define` line itself.
 *
 * @param g  The code generation state; the registrations are written to `g->primEnts`.
 */
static void primEmit(CG *g) {
    for (size_t i = 0; i < sizeof PRIM_BLOCK / sizeof *PRIM_BLOCK; i++) {
        const PrimPiece *p = &PRIM_BLOCK[i];
        /* A macro's row carries no name: it is read from the `#define` line itself, which is
         * the one thing every macro row has. (Keeping it out of the table also keeps a bare
         * `EXTC_*` string literal out of the source: `tools/check_switches.py` reads any of
         * those as a compiler switch that has to be documented, and a macro of the *generated
         * C* is not one.) A function or variable row spells its name out. */
        const char *name = p->name;
        if (p->kind == PRIM_MACRO) {
            const char *mn = macroDefineName(p->text, strlen(p->text));
            size_t nl = 0;
            if (mn) while (identByte(mn[nl])) nl++;
            if (nl) {
                char *cp = arenaAlloc(g->arena, nl + 1);
                memcpy(cp, mn, nl);
                cp[nl] = '\0';
                name = cp;
            }
        }
        if (!name) continue;
        PrimEnt *e = arenaAllocZero(g->arena, sizeof *e);
        e->name = name;
        e->text = p->text;
        e->kind = p->kind;
        e->quiet = strcmp(name, "__extc_dying") == 0;
        *(PrimEnt **)vecPush(&g->primEnts) = e;
    }
}

/**
 * @brief Count how many times a name occurs in one piece of scaffolding text.
 *
 * A guard line (`#ifndef X` / `#ifdef X`) only asks whether a macro is defined, which is not a use, so
 * those lines are skipped whole when `skipGuards` is true: `#ifndef EXTC_REC_LIMIT` must not keep the
 * macro alive.
 *
 * @param g           The code generation state, passed through to `countMentionsIn`.
 * @param text        One piece of scaffolding text.
 * @param name        The identifier to count, matched as a whole identifier.
 * @param skipGuards  Whether to skip `#ifndef` / `#ifdef` lines.
 * @return            The occurrence count.
 */
static size_t primScaffoldMentions(CG *g, const char *text, const char *name, bool skipGuards) {
    size_t n = 0, len = strlen(text);
    for (size_t p = 0; p < len; ) {
        const char *ln = text + p;
        const char *nl = memchr(ln, '\n', len - p);
        size_t ll = nl ? (size_t)(nl - ln) : (len - p);
        bool guard = (ll > 8 && (strncmp(ln, "#ifndef ", 8) == 0 || strncmp(ln, "#ifdef ", 7) == 0));
        if (!(skipGuards && guard)) n += countMentionsIn(g, ln, ll, name);
        if (!nl) break;
        p += ll + 1;
    }
    return n;
}

/**
 * @brief Decide which definitions in the run-time primitive blocks this program needs, and splice the
 *        surviving text into the unit.
 *
 * The blocks are assembled outside the unit, so the decision happens **before** they become part of
 * the generated C: the unit, the blocks' scaffolding and the live definitions are searched against
 * each other to a fixed point. A definition whose name occurs only inside itself is dropped -- the
 * same criterion as the text-level pass, only asked of the registration table. The scaffolding
 * (comments, blank lines, the `#if/#else/#endif` skeleton) is always emitted, so the product is
 * byte-identical to "emit the whole block, then delete definitions".
 *
 * @param g    The code generation state; the `primEnts` filled in by `primEmit` are the evidence.
 * @param out  The generated C unit; the block is spliced at `g->primA`, with what precedes and
 *             follows it left as it is.
 */
static void primSplice(CG *g, Buf *out) {
    const size_t ntab = sizeof PRIM_BLOCK / sizeof *PRIM_BLOCK;
    const size_t np   = g->primEnts.len;
    if (!np) return;
    char *unit = bufCstr(out);                       /* the unit, without the block */
    bool *live = arenaAllocZero(g->arena, np * sizeof *live);
    for (size_t i = 0; i < np; i++) live[i] = true;
    for (;;) {
        bool changed = false;
        for (size_t i = 0; i < np; i++) {
            PrimEnt *e = *(PrimEnt **)vecAt(&g->primEnts, i);
            if (!live[i]) continue;
            size_t total = countMentions(unit, e->name);
            for (size_t p = 0; p < ntab; p++) {      /* the scaffolding is always there */
                if (PRIM_BLOCK[p].kind != PRIM_SCAFFOLD) continue;
                total += primScaffoldMentions(g, PRIM_BLOCK[p].text, e->name, e->kind == PRIM_MACRO);
            }
            for (size_t j = 0; j < np; j++) {
                if (!live[j]) continue;
                PrimEnt *o = *(PrimEnt **)vecAt(&g->primEnts, j);
                total += countMentionsIn(g, o->text, strlen(o->text), e->name);
            }
            size_t inside;
            if (e->kind == PRIM_MACRO) {
                /* What the macro consists of: every `#define` line of that name still standing
                 * (a macro may be defined once per branch of an `#if`). */
                inside = 0;
                for (size_t j = 0; j < np; j++) {
                    if (!live[j]) continue;
                    PrimEnt *o = *(PrimEnt **)vecAt(&g->primEnts, j);
                    if (o->kind != PRIM_MACRO || strcmp(o->name, e->name) != 0) continue;
                    inside += countMentionsIn(g, o->text, strlen(o->text), e->name);
                }
            } else {
                inside = countMentionsIn(g, e->text, strlen(e->text), e->name);
            }
            if (total != inside) continue;           /* something outside names it: keep */
            live[i] = false;
            changed = true;
            if (!e->quiet && dbgOn("EXTC_DBG_DCE"))
                fprintf(stderr, "[wl] prim %s\n", e->name);
        }
        if (!changed) break;
    }
    /* The block as it will be written: scaffolding plus the definitions that survived. */
    Buf blk;
    bufInit(&blk, g->arena);
    for (size_t p = 0, ei = 0; p < ntab; p++) {
        /* `kind` decides, not `name`: a macro row carries no name of its own (primEmit reads
         * it from the `#define` line), and reading `name == NULL` as scaffolding would emit
         * every macro unconditionally and shift every index after it. */
        if (PRIM_BLOCK[p].kind == PRIM_SCAFFOLD) { bufPuts(&blk, PRIM_BLOCK[p].text); continue; }
        if (live[ei++]) bufPuts(&blk, PRIM_BLOCK[p].text);
    }
    /* Splice it in at `primA`: the preamble before it, everything else after, so the block
    * lands in front of the coroutine runtime. */
    Buf nb;
    bufInit(&nb, g->arena);
    bufPutn(&nb, out->data, g->primA);
    bufPuts(&nb, bufCstr(&blk));
    bufPutn(&nb, out->data + g->primA, out->len - g->primA);
    *out = nb;
}

/* One row per piece of the runtime primitive block, in emission order.
 * A row with `name` is a definition the worklist may leave out; a row without
 * one is scaffolding (comments, `#if/#else/#endif` skeletons) that is always
 * emitted, so a product whose primitive is unused looks exactly as it did when
 * the text-level pass removed the definition afterwards. Generated from the
 * primitive text; verified byte-for-byte. */

/**
 * @brief Emit the beginning of the generated file: feature macros, standard headers, and the few C
 *        types the prototypes need.
 *
 * Feature macros are honored only **before the first include**, so they must be written here, above
 * the header block, and only for programs that really need that POSIX/GNU interface: `g->needTime` /
 * `g->needDns` decide `_POSIX_C_SOURCE`, and `g->needHttpFile` decides `_GNU_SOURCE` (`clock_gettime`,
 * `nanosleep` and `getaddrinfo` are not visible under strict c11). The dyn and coroutine handle
 * typedefs are declared here too: prototypes and instance structs mention them first, and the runtime
 * is spliced in only at final assembly.
 *
 * @param g    The code generation state; `needTime`, `needDns`, `needHttpFile` and `needCoroHandle`
 *             are the emission switches.
 * @param out  The generated C unit, written from the beginning.
 * @param m    The module; `planUsesDyn` decides whether the dyn handle type is emitted.
 */
static void emitPreamble(CG *g, Buf *out, Module *m) {
    /* `-std=c11` is strict ISO, so the POSIX half of `<time.h>` (`clockid_t`, `CLOCK_MONOTONIC`,
     * `nanosleep`) is hidden. A feature-test macro only counts **before the first include** --
     * defining it later has no effect -- so it goes here, above the include block, and only for a
     * program that actually asks for the time (`needTime` is set by the declaration scan above,
     * which is why that scan has to run first). */
    /* `clock_gettime`/`nanosleep` are POSIX; `getaddrinfo`/`inet_ntop` are too (under strict c11
    * `struct addrinfo` is not even visible). The macro must come **before the first include**. */
    if (g->needTime || g->needDns)
        bufPuts(out, "#define _POSIX_C_SOURCE 200809L   /* clock_gettime/nanosleep/getaddrinfo */\n");
    /* The file-level HTTP date support uses `strptime`/`timegm`/`gmtime_r`: the first two are not
    * ISO C => `_GNU_SOURCE` is needed (`timegm` is not even POSIX). Enabled only when the program
    * really declares those shims. */
    if (g->needHttpFile)
        bufPuts(out, "#define _GNU_SOURCE 1   /* strptime/timegm/gmtime_r: the three HTTP date helpers */\n");
    bufPuts(out,
        "/* Generated by the extC compiler -- do not edit by hand.\n"
        " * The generated C is deliberately boring: no macro tricks, no clever optimizations.\n"
        " * `#line` directives map the C compiler's errors back to the .extc source lines.\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"      /* offsetof, needed by the descriptor tables */
        "#include <stdio.h>\n"
        "#include <stdarg.h>\n"   /* `extc_printf` (built-in printing and descriptor printing share the cout buffer) */
        "#include <string.h>\n"
        "#include <stdlib.h>\n\n");

    /* The dyn handle's C type is needed by **prototypes**: a `dyn Trait` parameter is a value of
    * this type, and prototypes are emitted before the runtime that defines it (the pool runtime is
    * spliced in during final assembly). Declaring it here closes that gap; C11 allows a typedef to
    * be repeated when it names the same type, so the runtime keeps its own copy. */
    {
        if (planUsesDyn(m))
            bufPuts(out,
                "#define EXTC_DYN_HANDLE_DEFINED 1\n"
                "struct ExtcDynHandleS { int64_t pid, slot, gen, pgen; };\n"
                "typedef struct ExtcDynHandleS ExtcDynHandle;\n\n");
        /* The coroutine handle, for the same reason as the dyn one: prototypes and the instance
         * structs (`vector<coroutine<T>>`) mention it before the runtime could define it. */
        if (g->needCoroHandle)
            bufPuts(out,
                "#define EXTC_CORO_HANDLE_DEFINED 1\n"
                "struct ExtcCoroS { void *frame; int64_t kind; int64_t task; };\n"
                "typedef struct ExtcCoroS extc_coro;\n\n");
    }
}

/**
 * @brief Record the splice point of the run-time block and emit the `@overwrite` cell type at the end
 *        of the unit.
 *
 * The primitive blocks (`primEmit`) and the coroutine runtime are assembled **right after** the unit,
 * and whether they enter the product is decided by `primSplice` later, so only the splice point
 * `g->primA` is recorded here: no pass deletes bytes before the block, so this offset is still
 * accurate at scan time. The `@overwrite` cell type contains `extc_arena *home` and must come after
 * the arena, so this function only computes `needOw` and the type itself is emitted in the arena
 * section.
 *
 * @param g    The code generation state.
 * @param out  The generated C unit; the cell type may be appended here.
 * @param m    The module.
 */
static void emitRuntimePrologue(CG *g, Buf *out, Module *m) {
    /* The reclamation runtime: the slab allocator, the holder counts and the region sweep.
     * It is emitted **before** any code that could name it -- its functions are `static` -- and
     * only when the program has a governed allocation point (`g->needReclaim`, narrowed by
     * `scanUnitReclaim` from that criterion; `EXTC_RECLAIM=1` forces it on, `=0` off). */
    if (g->needReclaim) {
        /* The frame root chain (the [rt:chain] section in reclaim.c) has no emitter pushing into the chain
         * yet, so pushing code would be all dead => passing 0 emits nothing. The place to wire it up
         * is this 0: it becomes the corresponding codegen flag. */
        reclaimEmitRuntime(g->arena, out, g->rcNoCount, getenv("EXTC_RC_TIME") != NULL,
                           g->rcSweepFree == 1, g->rcLocalStore == 1, 0);
        /* A region is claimed by **the arena's address**. The allocation point already knows which arena
         * it lands on (`arenaRefAt`), so the region's lifetime coincides exactly with that arena's --
         * including the case where the callee allocates with the caller's home arena (the home arena
         * is **one and the same address** handed all the way down), and including different frames of
         * the same level in recursion. The release point is emitted next to `extc_arena_destroy`
         * (`cgReclaimDropArena`): a block release does not touch the region, which lives until frame
         * destruction.
         *
         * When the table is full a new region is **not entered**: leaking a region is preferred over
         * freeing it early -- the same rule as "saturate and stick" for the count (obligation 3 in
         * `reclaim.c`).
         *
         * Placed after the runtime text and before the primitive block: it references only
         * `ExtcRcRegion` and `extc_rc_regionNew`/`extc_rc_regionFree`, all three provided by that text
         * above. The `unused` attribute is there because "the switch is on but the program has no
         * allocation point" is a legal input: neither function is called then, and the generated C must
         * still compile under `-Wall -Werror`. */
        bufPuts(out,
            "#define EXTC_RC_REG_MAX 512\n"
            "static void *extc_rc_regKey[EXTC_RC_REG_MAX];\n"
            "static ExtcRcRegion *extc_rc_regVal[EXTC_RC_REG_MAX];\n"
            "static size_t extc_rc_regN;\n"
            "/* Every frame looks up its home arena once, in the prologue: the table usually holds\n"
            " * two or three entries, so a linear scan is enough, but the call/return itself is a third\n"
            " * of the cost, so the lookup is inlined into the prologue (`static inline` inlines at\n"
            " * -O2). */\n"
            "__attribute__((unused)) static inline ExtcRcRegion *extc_rc_regionOf(void *key, int32_t rid) {\n"
            "    for (size_t i = 0; i < extc_rc_regN; i++)\n"
            "        if (extc_rc_regKey[i] == key) return extc_rc_regVal[i];\n"
            "    ExtcRcRegion *r = extc_rc_regionNew(rid);\n"
            "    if (extc_rc_regN < EXTC_RC_REG_MAX) {\n"
            "        extc_rc_regKey[extc_rc_regN] = key;\n"
            "        extc_rc_regVal[extc_rc_regN] = r;\n"
            "        extc_rc_regN++;\n"
            "    }\n"
            "    return r;\n"
            "}\n"
            "__attribute__((unused)) static void extc_rc_regionDrop(void *key) {\n"
            "    for (size_t i = 0; i < extc_rc_regN; i++) {\n"
            "        if (extc_rc_regKey[i] != key) continue;\n"
            "        extc_rc_regionFree(extc_rc_regVal[i]);\n"
            "        extc_rc_regN--;\n"
            "        extc_rc_regKey[i] = extc_rc_regKey[extc_rc_regN];\n"
            "        extc_rc_regVal[i] = extc_rc_regVal[extc_rc_regN];\n"
            "        return;\n"
            "    }\n"
            "}\n");
    }
    /* The dying hook goes between the includes and the trap paths that call it: `int32_t`
     * has to be known, and the definition has to precede every use. It is its own block
     * because the prologue around it is already at the 4095-byte limit C99 guarantees for
     * a string literal. */
    /* The dying hook is captured here, with the block, so the unreferenced-definition pass
    * can drop it together with the traps that call it: it must be part of the same block. */
    /* The @overwrite cell type is emitted only when it is really used: emitted
     * unconditionally it would add a line to the generated C of every program
     * and fill the golden files with noise. The type contains an
     * `extc_arena *home`, so it must come after `extc_arena`; therefore only
     * `needOw` is computed here and the typedef itself is emitted further down,
     * inside the arena section. */
    bool needOw = false;
    {
        for (size_t i = 0; i < m->funcs.len && !needOw; i++)
            if (planOwSites(*(FuncDef **)vecAt(&m->funcs, i)) > 0) needOw = true;
        for (size_t i = 0; i < m->structs.len && !needOw; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                if (planOwSites(*(FuncDef **)vecAt(&sd->methods, j)) > 0) { needOw = true; break; }
        }
        (void)needOw;   /* the typedef needs extc_arena, so it comes later */
    }
    /* The runtime primitive block: the dying hook first (every trap below calls it), then
     * the primitives themselves. It is assembled **beside** the unit, not into it: whether a
     * definition is used is decided from the registry (`primEmit`), just before the block is
     * spliced in, so a definition nothing uses never becomes part of the generated C. */
    g->primA = out->len;                 /* where the block will be spliced in */
    primEmit(g);


    g->coroDefPrinted = g->needCoroHandle;
    g->coroDefA = out->len;              /* the coroutine runtime goes after the block */


    if (needOw)
        bufPuts(out, "typedef struct extc_owcell { void *p; int64_t cap; extc_arena *home; } extc_owcell;\n");
}

/**
 * @brief Assemble the runtime text for the descriptor table, the generic printer and the structural
 *        equality comparison.
 *
 * The text goes into its own buffers (`g->rt`, `g->rtPrint`, `g->rtEq`) rather than straight into the
 * unit: only scalar descriptor lines are registered in `deadDefs` and unmentioned lines are deleted
 * later; `rtPrint` is emitted only when the program really calls printing; and an over-long string
 * literal is split into several `bufPuts` calls at C99's 4095-character limit.
 *
 * @param g  The code generation state, which owns the runtime buffers.
 */
static void buildRuntimeText(CG *g) {
    /* ==================================================================
    * The descriptor table and the single generic printer.
    *
    * One static descriptor per type, and the printing logic exists once per program:
    * this is not runtime reflection. The types, field offsets and variant names in the
    * descriptors are compile-time constants gcc can see through, so nothing is weakened
    * about a failure being loud when it cannot be proven impossible.
    * ================================================================== */
    bufPuts(&g->rt,
        "/* ---- Type descriptor table ----\n"
        /* Do **not** write the printing function's name inside this comment: in the runtime block text it is pruned by "delete when nobody mentions it", and that judgment counts **text** -- one mention in a comment is as good as a use. */
        " * One static entry per type; the printer exists once per program.\n"
        " * Meaning of `size`: for a scalar or struct it is sizeof(T); for an array or\n"
        " * view it is the element stride. A view always has the C layout\n"
        " * `{ T *data; int64_t len; }`.\n"
        " */\n"
        "enum {\n"
        "    EXTC_D_I8, EXTC_D_I16, EXTC_D_I32, EXTC_D_I64,\n"
        "    EXTC_D_U8, EXTC_D_U16, EXTC_D_U32, EXTC_D_U64,\n"
        "    EXTC_D_F32, EXTC_D_F64, EXTC_D_BOOL,\n"
        "    EXTC_D_ENUM,      /* table = const char *const[]; the tag sits at offset 0 */\n"
        "    EXTC_D_STRUCT,    /* table = ExtcField[] */\n"
        "    EXTC_D_ARRAY,     /* fixed-size array: `count` elements of `elem` */\n"
        "    EXTC_D_SLICE,     /* view: { T *data; int64_t len; } */\n"
        "    EXTC_D_TEXT,      /* slice<u8>: printed as text, unlike any other slice */\n"
        "    EXTC_D_REF        /* ref / ?ref: always printed as <ref> */\n"
        "};\n"
        "\n"
        "typedef struct ExtcDesc ExtcDesc;\n"
        "typedef struct { const char *name; size_t off; const ExtcDesc *desc; } ExtcField;\n"
        "\n"
        "struct ExtcDesc {\n"
        "    int             kind;\n"
        "    const char     *name;    /* display name of a struct or enum, for printing */\n"
        "    size_t          size;    /* scalar/struct = sizeof(T); array/view = element stride */\n"
        "    size_t          count;   /* number of fields, variants, or array elements */\n"
        "    const void     *table;   /* ExtcField[] or const char *const[] */\n"
        "    const ExtcDesc *elem;    /* element descriptor of an array or view */\n"
        "    /* Used by structural `==`; filled in for structs only, because a struct's `==`\n"
        "     * is user code (or library code) and can only be delegated to, never\n"
        "     * re-derived. codegen emits a one-line adapter for it. Every other kind is\n"
        "     * compared recursively by `extc_eq`, so this slot stays NULL. It sits last on\n"
        "     * purpose: an initializer that omits it still zero-fills it. */\n"
        "    bool          (*eq)(const void *a, const void *b);\n"
        "};\n"
        "\n"
        "/* Type-independent descriptors (reference, byte view, scalars), shared per program. */\n");
    /* Type-independent descriptors: one row per scalar kind, handed out by `descRef`
     * on demand. A program that never prints a `u64`, or never compares a `ref`, has
     * no use for that row - and a row nobody names is a warning in every unit that
     * carries it. Each row is emitted here and remembered: dropUnreferenced takes it
     * back out once the finished unit says nothing names it. */
    static const struct { const char *name; const char *row; } SCALAR_DESC[] = {
        { "extc_desc_ref", "static const ExtcDesc extc_desc_ref  = { EXTC_D_REF,  \"ref\",  sizeof(void *), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_text", "static const ExtcDesc extc_desc_text = { EXTC_D_TEXT, \"slice<u8>\", 1, 0, NULL, NULL, NULL };\n" },
        { "extc_desc_bool", "static const ExtcDesc extc_desc_bool = { EXTC_D_BOOL, \"bool\", sizeof(bool), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i8", "static const ExtcDesc extc_desc_i8  = { EXTC_D_I8,  \"i8\",  sizeof(int8_t),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_i16", "static const ExtcDesc extc_desc_i16 = { EXTC_D_I16, \"i16\", sizeof(int16_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i32", "static const ExtcDesc extc_desc_i32 = { EXTC_D_I32, \"i32\", sizeof(int32_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_i64", "static const ExtcDesc extc_desc_i64 = { EXTC_D_I64, \"i64\", sizeof(int64_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u8", "static const ExtcDesc extc_desc_u8  = { EXTC_D_U8,  \"u8\",  sizeof(uint8_t),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_u16", "static const ExtcDesc extc_desc_u16 = { EXTC_D_U16, \"u16\", sizeof(uint16_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u32", "static const ExtcDesc extc_desc_u32 = { EXTC_D_U32, \"u32\", sizeof(uint32_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_u64", "static const ExtcDesc extc_desc_u64 = { EXTC_D_U64, \"u64\", sizeof(uint64_t), 0, NULL, NULL, NULL };\n" },
        { "extc_desc_f32", "static const ExtcDesc extc_desc_f32 = { EXTC_D_F32, \"f32\", sizeof(float),  0, NULL, NULL, NULL };\n" },
        { "extc_desc_f64", "static const ExtcDesc extc_desc_f64 = { EXTC_D_F64, \"f64\", sizeof(double), 0, NULL, NULL, NULL };\n" },
    };
    for (size_t i = 0; i < sizeof SCALAR_DESC / sizeof *SCALAR_DESC; i++) {
        size_t rowOff = g->rt.len;
        bufPuts(&g->rt, SCALAR_DESC[i].row);
        Buf row;
        bufInit(&row, g->arena);
        bufPutn(&row, g->rt.data + rowOff, g->rt.len - rowOff);
        DeadDef *dd = arenaAllocZero(g->arena, sizeof *dd);
        dd->name = SCALAR_DESC[i].name;
        dd->text = bufCstr(&row);
        *(DeadDef **)vecPush(&g->deadDefs) = dd;
    }
    bufPuts(&g->rt, "\n");
    /* Split into two calls: C99 only guarantees support for string literals of
     * 4095 characters, and one large literal would trigger -Woverlength-strings.
     * That is not an error, but there is no reason to keep the noise. */
    bufPuts(&g->rtPrint,
        "/* Generic recursive printer. The output format must stay byte-for-byte identical to\n"
        " * the `_debug` helpers it replaces; the truth table lives in\n"
        " * tools/print-formats.txt (floats use %g, [N]u8 prints numerically, and\n"
        " * slice<u8> prints as text). */\n"
        /* No `__attribute__((unused))` here: this section is emitted **as a whole block**, and the printing
        * function is called only when the program really prints a structured type => any program that
        * needs the runtime block for another reason but prints no structured type would hit
        * `-Werror=unused-function`; `static inline` is no way out either (GCC accepts it, Clang counters
        * with `-Wunneeded-internal-declaration`).
        * This section is emitted **on demand**: `rtPrint` goes out only when there really is a call
        * site for it (`needPrint`), so the attribute can be dropped -- a function nobody calls never
        * enters the product. */
        "static void extc_print(const void *p, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:   extc_printf(\"%d\", (int)*(const int8_t *)p); return;\n"
        "    case EXTC_D_I16:  extc_printf(\"%d\", (int)*(const int16_t *)p); return;\n"
        "    case EXTC_D_I32:  extc_printf(\"%d\", (int)*(const int32_t *)p); return;\n"
        "    case EXTC_D_I64:  extc_printf(\"%lld\", (long long)*(const int64_t *)p); return;\n"
        "    case EXTC_D_U8:   extc_printf(\"%u\", (unsigned)*(const uint8_t *)p); return;\n"
        "    case EXTC_D_U16:  extc_printf(\"%u\", (unsigned)*(const uint16_t *)p); return;\n"
        "    case EXTC_D_U32:  extc_printf(\"%u\", (unsigned)*(const uint32_t *)p); return;\n"
        "    case EXTC_D_U64:  extc_printf(\"%llu\", (unsigned long long)*(const uint64_t *)p); return;\n"
        "    case EXTC_D_F32:  extc_printf(\"%g\", (double)*(const float *)p); return;\n"
        "    case EXTC_D_F64:  extc_printf(\"%g\", (double)*(const double *)p); return;\n"
        "    case EXTC_D_BOOL: extc_printf(\"%s\", *(const bool *)p ? \"true\" : \"false\"); return;\n"
        "    case EXTC_D_REF:  extc_printf(\"<ref>\"); return;\n"
        "    case EXTC_D_TEXT: {\n"
        "        int64_t n = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "        extc_printf(\"%.*s\", (int)n, (const char *)*(const void *const *)p);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ENUM: {\n"
        "        const char *const *names = (const char *const *)d->table;\n"
        "        int tag = *(const int *)p;\n"
        "        if (tag < 0 || (size_t)tag >= d->count) extc_printf(\"<%s>\", d->name);\n"
        "        else                                    extc_printf(\"%s\", names[tag]);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *f = (const ExtcField *)d->table;\n"
        "        extc_printf(\"%s { \", d->name);\n"
        "        for (size_t i = 0; i < d->count; i++) {\n"
        "            if (i) extc_printf(\", \");\n"
        "            extc_printf(\"%s: \", f[i].name);\n"
        "            extc_print((const char *)p + f[i].off, f[i].desc);\n"
        "        }\n"
        "        extc_printf(\" }\");\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *data;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t len = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "            data = (const char *)*(const void *const *)p;\n"
        "            n = len > 0 ? (size_t)len : 0;\n"
        "        } else {\n"
        "            data = (const char *)p;\n"
        "            n = d->count;\n"
        "        }\n"
        "        extc_printf(\"[\");\n"
        "        for (size_t i = 0; i < n; i++) {\n"
        "            if (i) extc_printf(\", \");\n"
        "            extc_print(data + i * d->size, d->elem);\n"
        "        }\n"
        "        extc_printf(\"]\");\n"
        "        return;\n"
        "    }\n"
        "    }\n"
        "}\n\n");

    /* Structural `==`, sharing the same descriptor table as `extc_print`.
     *
     * The semantics have to match the old per-array-type `_eq` bit for bit:
     *   - a scalar or an enum compares with `==` directly; an enum compares its
     *     tag only, and an enum with payloads is not comparable at all, which
     *     the checker rejects long before this point
     *   - an array recurses element by element, and a slice compares lengths
     *     first and then recurses element by element, exactly like
     *     `slice<T>::==` in the prelude
     *   - a struct delegates to the `fn ==` written by the user or the library,
     *     which is the `eq` slot of its descriptor, filled in by a one-line
     *     adapter that code generation emits
     * So one `_eq` function per array type became one unit of data per type,
     * rather than derived code per array type.
     */
    bufPuts(&g->rtEq,
        "static bool extc_eq(const void *a, const void *b, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:  return *(const int8_t *)a  == *(const int8_t *)b;\n"
        "    case EXTC_D_I16: return *(const int16_t *)a == *(const int16_t *)b;\n"
        "    case EXTC_D_I32: return *(const int32_t *)a == *(const int32_t *)b;\n"
        "    case EXTC_D_I64: return *(const int64_t *)a == *(const int64_t *)b;\n"
        "    case EXTC_D_U8:  return *(const uint8_t *)a  == *(const uint8_t *)b;\n"
        "    case EXTC_D_U16: return *(const uint16_t *)a == *(const uint16_t *)b;\n"
        "    case EXTC_D_U32: return *(const uint32_t *)a == *(const uint32_t *)b;\n"
        "    case EXTC_D_U64: return *(const uint64_t *)a == *(const uint64_t *)b;\n"
        "    case EXTC_D_F32: return *(const float *)a  == *(const float *)b;\n"
        "    case EXTC_D_F64: return *(const double *)a == *(const double *)b;\n"
        "    case EXTC_D_BOOL: return *(const bool *)a == *(const bool *)b;\n"
        "    case EXTC_D_REF:  return *(const void *const *)a == *(const void *const *)b;\n"
        "    case EXTC_D_ENUM: return *(const int *)a == *(const int *)b;\n"
        "    case EXTC_D_TEXT: {\n"
        "        const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "        const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "        if (la != lb) return false;\n"
        "        if (la <= 0) return true;      /* empty view: `data` may be null, so nothing to compare */\n"
        "        return memcmp(*(const void *const *)a, *(const void *const *)b, (size_t)la) == 0;\n"
        "    }\n"
        "    case EXTC_D_STRUCT:\n"
        "        /* The user-written `fn ==` through its adapter; a NULL slot means the\n"
        "         * two values should never have been compared. */\n"
        "        return d->eq ? d->eq(a, b) : false;\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *pa, *pb;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "            const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "            if (la != lb) return false;\n"
        "            pa = (const char *)*(const void *const *)a;\n"
        "            pb = (const char *)*(const void *const *)b;\n"
        "            n = la > 0 ? (size_t)la : 0;\n"
        "        } else {\n"
        "            pa = (const char *)a;\n"
        "            pb = (const char *)b;\n"
        "            n = d->count;\n"
        "        }\n"
        "        for (size_t i = 0; i < n; i++)\n"
        "            if (!extc_eq(pa + i * d->size, pb + i * d->size, d->elem)) return false;\n"
        "        return true;\n"
        "    }\n"
        "    }\n"
        "    return false;\n"
        "}\n\n");
}

/**
 * @brief Emit every C type definition this unit needs, in dependency order.
 *
 * An enum precedes the structs that use it: C11 cannot forward-declare an enum tag. A payload-carrying
 * enum emits only its tag constants here and leaves the `struct { tag; union }` definition to the
 * dependency-sorted section, because a payload can contain another struct by value. The closure of
 * generic instances is expanded to a fixed point here, exiting with an error after 64 rounds rather
 * than emitting fewer types.
 *
 * @param g     The code generation context; type text goes into its output buffer and pending-prune
 *              records are registered there.
 * @param m     The current module, which provides `types`, `structs` and `funcs`.
 * @param tt    The type table, which provides `instances`, `enumInstances` and `fnTypes`.
 * @param arena The scratch allocator for this round's type units and temporary text.
 */
static void emitTypes(CG *g, Module *m, TypeTable *tt, Arena *arena) {
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        if (!enumHasPayload(td)) {
            bufPuts(&b, "typedef enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&b, ", ");
                bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&b, " } %s;", td->name);
            cgLine(g, "%s", bufCstr(&b));
            cgLine(g, "");
        } else if (td->typeParams.len > 0) {
            continue;      /* generic enum: the instances own constants and definition */
        } else {
            /* With payloads: the tag constants can be emitted now, since they
             * depend on nothing, while the `struct { tag; union }` definition is
             * left to the dependency-ordered section below, because a payload
             * may contain another struct, as in a variant holding a
             * `slice<u8>`. */
            Buf e2;
            bufInit(&e2, arena);
            bufPrintf(&e2, "enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&e2, ", ");
                bufPrintf(&e2, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&e2, " };");
            cgLine(g, "%s", bufCstr(&e2));
            cgLine(g, "");
            continue;               /* the name table is deferred too: it reads the tag */
        }

        /* Nothing follows: an enum has textual names automatically, and they
         * come from the array of variant names in its descriptor, the `table`
         * of `<Type>_desc`, rather than from a derived `<Type>_name` function
         * emitted here. */
    }

    /* Collect everything that becomes a struct in C: ordinary structs, generic
     * instances, arrays, and enums with payloads. */
    Vec units;
    vecInit(&units, arena, sizeof(void *));
    /* Deduplicated by C name: `slice<mut slice<T>>` and `slice<slice<T>>` are
     * both `slice_slice_T`, while `ttEquals` counts `mut` as part of the
     * identity, so two instances can share one name - emitting both would
     * redefine the struct. */
    for (size_t i = 0; i < g->structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g->structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = it->sdef;              /* NULL for an array */
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* An enum with payloads belongs here too: its union holds the payload
     * types by value. */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!enumHasPayload(td)) continue;
        if (td->typeParams.len > 0) continue;    /* generic enum: instances below */
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = td;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* Each instance of a generic enum (`option<i64>`) is a struct definition of
     * its own. Two instances can share one C name -- `option<mut slice<u8>>` and
     * `option<slice<u8>>` are both `option_slice_u8`, because `mut` changes the
     * extC type but not the C layout -- and emitting both redefines the struct,
     * the typedef and the tag enum. The first one wins, which is the same rule
     * `addInstanceUnit` applies to instances reached through method signatures. */
    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        bool dup = false;
        for (size_t k = 0; k < units.len && !dup; k++)
            dup = strcmp(unitName(*(SUnit **)vecAt(&units, k)), it->name) == 0;
        if (dup) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = it->edef;
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* Coroutine frames are synthesized structs but real types: scanning their
     * fields is what makes the instances those fields mention (`vector<i32>`,
     * behind a container) get generated at all. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || planTemplate(cf) || !planCoroFrameType(cf) || !planCoroFrameType(cf)->sdef) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = planCoroFrameType(cf)->sdef;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&g->funcs, i);
        if (!cf || !planIsCoro(cf) || !planTemplate(cf) || !planCoroFrameType(cf) || !planCoroFrameType(cf)->sdef) continue;
        if (coroProvisional(&cf->targs)) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = planCoroFrameType(cf)->sdef;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    /* Run to a fixed point, adding the instances that method signatures
     * mention. The round count is capped, and passing the cap means something
     * unexpected is happening, so it is reported loudly rather than quietly
     * generating too little. */
    {
        bool grew = true;
        int round = 0;
        const char *srcName = NULL, *newName = NULL;   /* names the expansion */
        while (grew && round < 64) {
            grew = false;
            round++;
            size_t cur = units.len;                 /* only this round's entries */
            for (size_t i = 0; i < cur; i++) {
                SUnit *u = *(SUnit **)vecAt(&units, i);
                size_t before = units.len;
                scanUnitForUnits(arena, tt, &units, u);
                if (units.len != before) {
                    grew = true;
                    srcName = unitName(u);
                    for (size_t k = before; k < units.len; k++)
                        newName = unitName(*(SUnit **)vecAt(&units, k));
                }
            }
        }
        if (grew) {
            /* The cap guards against instances that nest without end; passing
             * it also names which instance expanded into which. */
            fprintf(stderr,
                    "extc: error: generic instance closure did not settle in 64 rounds.\n"
                    "      last expansion: `%s` mentions `%s`, which needs more instances.\n"
                    "      This usually means instances nest without bound (such as\n"
                    "      `option<option<option<...>>>`). Use a concrete type, or report\n"
                    "      this program if you think it should compile.\n",
                    srcName ? srcName : "?", newName ? newName : "?");
            exit(1);
        }
    }

    /* Every typedef comes first: a pointer field (`ref T`) needs nothing more. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        cgLine(g, "typedef struct %s %s;", unitName(u), unitName(u));
    }
    if (units.len) cgLine(g, "");

    /* The function-type typedefs (`fn(A) -> R`), one per signature, go after the
     * enum typedefs and the struct forward typedefs (a signature names the types
     * it takes and returns) and before the struct definitions (a field of
     * function type names its typedef). A function type cannot be spelled as a
     * prefix of a declaration, so it is spelled by a typedef and `cType` answers
     * with the typedef's name. `tt->fnTypes` is the demand set, and a nested
     * signature is always interned before the one that mentions it. */
    if (tt->fnTypes.len) {
        cgLine(g, "/* ---- function types (`fn(A) -> R`): one typedef per signature ---- */");
        for (size_t i = 0; i < tt->fnTypes.len; i++) {
            Type *ft = *(Type **)vecAt(&tt->fnTypes, i);
            if (!ft->name) continue;
            /* Two distinct signatures must never share one C name; a collision is
             * reported rather than emitted as C that does not compile. */
            bool clash = false;
            for (size_t j = 0; j < i && !clash; j++)
                clash = strcmp((*(Type **)vecAt(&tt->fnTypes, j))->name, ft->name) == 0;
            for (size_t j = 0; j < m->structs.len && !clash; j++)
                clash = strcmp((*(StructDef **)vecAt(&m->structs, j))->name, ft->name) == 0;
            for (size_t j = 0; j < m->types.len && !clash; j++)
                clash = strcmp((*(TypeDef **)vecAt(&m->types, j))->name, ft->name) == 0;
            if (clash) {
                Buf tb;
                bufInit(&tb, g->arena);
                ttRender(ft, &tb);
                ctxError(g->ctx, 1, 1,
                         "The compiler names the C typedef of a function type `extc_fn_<return>__"
                         "<argument>...`. Give this declaration another name.",
                         "`%s` is a name the compiler needs for the function type `%s`",
                         ft->name, bufCstr(&tb));
                continue;
            }
            cgLine(g, "typedef %s (*%s)(%s);", cType(g, ft->ret), ft->name, fnParamList(g, ft));
        }
        cgLine(g, "");
    }

    /* Compute the dependencies: another struct-like type held by value in a
     * field. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (u->td) {                    /* enum: payloads live in the union by value */
            for (size_t j = 0; j < u->td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&u->td->variants, j);
                for (size_t k = 0; k < v->types.len; k++) {
                    Type *pt = *(Type **)vecAt(&v->types, k);
                    if (u->inst)                 /* instance: substitute the arguments */
                        pt = ttSubstitute(tt, pt, &u->td->typeParams, &u->inst->targs);
                    if (pt->kind == TY_REF) continue;
                    int d = unitFind(&units, pt);
                    if (d >= 0 && d != (int)i) *(int *)vecPush(&u->deps) = d;
                }
            }
            continue;
        }
        if (u->inst && u->inst->kind == TY_ARRAY) {
            int j = unitFind(&units, u->inst->inner);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
            continue;
        }
        for (size_t k = 0; k < u->sd->fields.len; k++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, k);
            Type *ft = fd->type;
            if (u->inst) ft = ttSubstitute(tt, ft, &u->sd->typeParams, &u->inst->targs);
            if (ft->kind == TY_REF) continue;
            int j = unitFind(&units, ft);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
        }
    }

    /* Emit the definitions in dependency order. A cycle, which would have to go
     * through a value and which C forbids anyway, is emitted regardless and left
     * to the C compiler to report. */
    for (;;) {
        bool progressed = false, allDone = true;
        for (size_t i = 0; i < units.len; i++) {
            SUnit *u = *(SUnit **)vecAt(&units, i);
            if (u->done) continue;
            allDone = false;
            bool ready = true;
            for (size_t k = 0; k < u->deps.len && ready; k++)
                ready = (*(SUnit **)vecAt(&units, *(int *)vecAt(&u->deps, k)))->done;
            if (!ready) continue;
            unitBody(g, u);
            u->done = true;
            progressed = true;
        }
        if (allDone || !progressed) break;
    }
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (!u->done) { unitBody(g, u); u->done = true; }
    }


    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        TypeDef *td = it->edef;
        /* Skip a repeat of a C name (see the unit list above): its tag constants
         * were emitted by the first instance, and a second enum with the same
         * enumerators is a redefinition. */
        bool dup = false;
        for (size_t k = 0; k < i && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&tt->enumInstances, k))->name, it->name) == 0;
        if (dup) continue;
        /* The tag constants of the instance (`maybe_i64_nothing = 0`) carry the
         * same values as the base type, both following the variant order, so the
         * rule that the zero value has tag 0 holds for an instance as well. */
        Buf tb;
        bufInit(&tb, arena);
        bufPuts(&tb, "enum { ");
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&tb, ", ");
            bufPrintf(&tb, "%s_%s = %zu", it->name, v->name, j);
        }
        bufPuts(&tb, " };");
        cgLine(g, "%s", bufCstr(&tb));
    }
}

/**
 * @brief One module-level binding whose type carries an `@drop` hook.
 *
 * Collected by `emitGlobals` so the exit hook can walk them; see `CG.globalDrops`.
 */
typedef struct {
    const char *name;    /**< The C name of the `static` object. */
    Type       *declT;   /**< Its declared type, for the drop walk. */
} GlobalDrop;

/**
 * @brief Emit `static` definitions for the module's globals and constants, registering every line in
 *        the unreferenced-pruning table.
 *
 * A fixed-size global needs no arena; C zeroes static storage itself, so a global without an
 * initializer gets none written. `static` is deliberate: external linkage makes gcc give up vectorizing
 * the outer loop. A global initializer can mention function names (a `fn` field table), and C requires
 * them to be declared first, so initializers are walked once to emit their prototypes beforehand.
 *
 * @param g  The code generation context; `out` receives the definition text and `deadDefs` the
 *           prunable entries.
 * @param m  The current module; `globals` is the list to emit.
 */
static void emitGlobals(CG *g, Module *m) {
    /* Globals and constants are plain C static objects; a global of fixed size
     * needs no arena, and C zeroes a static object by itself. `static` is
     * required for performance: external linkage makes gcc reject outer-loop
     * vectorization. */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(g, gd->ann);
        /* A global may name functions (a table of `fn` fields); C needs them declared first. */
        if (gd->init) {
            AstVisit v = { globalFnProto, globalFnProtoStmt, g };
            astWalkExprChildren(gd->init, &v);
        }
        size_t before = g->out->len;                 /* recorded for dropUnreferenced */
        if (gd->init) {
            cgLine(g, "static %s %s = %s;", ct, gd->name, genGlobalInit(g, gd->init));
        } else {
            cgLine(g, "static %s %s;", ct, gd->name);
        }
        /* A global the program never names costs a warning in every translation
         * unit that carries it - and a `use std::io::*` carries the library's
         * streams and constants into every program. The line just written is
         * remembered; dropUnreferenced takes it back out if nothing names it. */
        Buf line;
        bufInit(&line, g->arena);
        bufPutn(&line, g->out->data + before, g->out->len - before);
        DeadDef *d = arenaAllocZero(g->arena, sizeof *d);
        d->name = gd->name;
        d->text = bufCstr(&line);
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        /* With reclamation on, a global reference slot is a **process-level root**: every trigger point's
         * root table must list it. A global is file-level `static`, so its name is visible in every
         * function body. When a type's references cannot be enumerated => every function's trigger is
         * turned off. */
        if (g->needReclaim && gd->ann) {
            RcScan sc = rcScanType(g, gd->ann, 0);
            if (sc == RC_CONSERVATIVE) g->rcGlobalsUnreliable = true;
            else if (sc == RC_LEAF_REFS)
                *(const char **)vecPush(&g->rcGlobalRoots) = gd->name;
        }
        /* A global that owns state gets its hook run at exit; see `CG.globalDrops`. */
        if (gd->ann && typeNeedsDrop(g, gd->ann, 0)) {
            GlobalDrop *d = arenaAllocZero(g->arena, sizeof *d);
            d->name = gd->name;
            d->declT = gd->ann;
            *(GlobalDrop *)vecPush(&g->globalDrops) = *d;
            g->needGlobalDrops = true;
        }
    }
    if (m->globals.len) cgLine(g, "");
}

/* The scan state: the generator plus the visitor, so the callbacks can recurse (the `astWalk*`
 * helpers visit one level of children, and a function-pointer use can sit any number of levels
 * down). */
typedef struct {
    CG       *g;
    AstVisit  v;
} LamThunkScan;

static bool lamThunkScanExpr(void *ctx, Expr *e);
static bool lamThunkScanStmt(void *ctx, Stmt *s);

/**
 * @brief Inspect one expression and recurse into its children.
 *
 * @param ctx  The scan state.
 * @param e    The expression to inspect.
 * @return     Always true: the walk never stops early.
 */
static bool lamThunkScanExpr(void *ctx, Expr *e) {
    LamThunkScan *sc = (LamThunkScan *)ctx;
    if (e && e->kind == EX_LAMBDA && planLamFnPtr(e)) {
        StructDef *sd = e->u.lambda.sdef;
        /* An environment with one layout per instance gets its thunk from that layout
         * (`emitLamVars`), not from the unit-wide list: its signature is the instance's. */
        if (sd && sd->methods.len == 1 && !lamEnvNeedsVariants(sd)) {
            bool seen = false;
            for (size_t i = 0; i < sc->g->lamThunks.len && !seen; i++)
                seen = *(StructDef **)vecAt(&sc->g->lamThunks, i) == sd;
            if (!seen) *(StructDef **)vecPush(&sc->g->lamThunks) = sd;
        }
    }
    return astWalkExprChildren(e, &sc->v);
}

/**
 * @brief Inspect one statement's children.
 *
 * @param ctx  The scan state.
 * @param s    The statement to walk.
 * @return     Always true: the walk never stops early.
 */
static bool lamThunkScanStmt(void *ctx, Stmt *s) {
    LamThunkScan *sc = (LamThunkScan *)ctx;
    return astWalkStmtChildren(s, &sc->v);
}

/**
 * @brief Collect the lambda environments that need a function-pointer thunk.
 *
 * The prototypes have to be emitted before any body is generated, and a use site can sit in any
 * body, so the whole unit is scanned first. `g->funcs` is the set that will be emitted, which is
 * exactly the set whose bodies can mention a thunk.
 *
 * @param g  The generator state; `lamThunks` is filled in.
 */
static void collectLamThunks(CG *g) {
    LamThunkScan sc;
    sc.g = g;
    sc.v.expr = lamThunkScanExpr;
    sc.v.stmt = lamThunkScanStmt;
    sc.v.ctx  = &sc;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && f->body) astWalkStmtChildren(f->body, &sc.v);
    }
}

/**
 * @brief Emit every collected lambda thunk (prototypes were already emitted).
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamThunks(CG *g) {
    for (size_t i = 0; i < g->lamThunks.len; i++)
        emitLamThunk(g, *(StructDef **)vecAt(&g->lamThunks, i));
}

/* ---- lambda environments laid out per instance ---------------------------------------------
 *
 * A lambda written inside a generic body can capture a value whose type is a type parameter, and its
 * own signature can mention one too. The environment's C layout is then only concrete per instance,
 * so it is emitted once per instance (`LamVar`) instead of once per literal: `$lam$calc$0` becomes
 * `$lam$calc$0$calc_key` (an `i64` field holding a `key`) and `$lam$calc$0$calc_item` (holding an
 * `item`), each with its own `call` method and, when the literal is used as a bare `fn(...)` value,
 * its own thunk.
 *
 * The field list is shared with the checker's struct -- substitution happens while a field's type is
 * printed -- so one `FieldDef` reads as `key` in one layout and as `item` in the other.
 */

/**
 * @brief Whether a lambda environment's C layout depends on a type parameter.
 *
 * The layout has to be repeated per instance exactly when a field or the lambda's own signature
 * mentions one. A lambda inside a generic body that only captures concrete values keeps one layout
 * for every instance, which is what almost every closure in practice is.
 *
 * @param sd  The lambda's synthesized environment struct.
 * @return    true when the environment has to be laid out once per instance.
 */
static bool lamEnvNeedsVariants(const StructDef *sd) {
    if (!sd || !planLamScope(sd)) return false;
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt((Vec *)&sd->fields, i);
        if (fd && fd->type && mentionsParam(fd->type)) return true;
    }
    if (sd->methods.len == 1) {
        FuncDef *md = *(FuncDef **)vecAt((Vec *)&sd->methods, 0);
        if (md && funcSignatureMentionsParam(md)) return true;
    }
    return false;
}

/**
 * @brief The layout of a lambda environment for the instance currently being generated.
 *
 * @param g   The generator state; `g->lamCtx` names the instance.
 * @param sd  The lambda's synthesized environment struct.
 * @return    The layout, or NULL when `sd` is not an environment that depends on a type parameter or
 *            the current instance has no layout for it.
 */
static LamVar *lamVarFor(CG *g, const StructDef *sd) {
    if (!g->lamCtx || !sd) return NULL;
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        if (v->sd == sd && strcmp(v->ctxName, g->lamCtx) == 0) return v;
    }
    return NULL;
}

/**
 * @brief The C name of a lambda environment inside the instance currently being generated.
 *
 * @param g   The generator state.
 * @param sd  The lambda's synthesized environment struct.
 * @return    The layout's name; NULL when the environment keeps one layout (use the struct name) or
 *            the current instance has no layout for it.
 */
static const char *lamVarName(CG *g, const StructDef *sd) {
    LamVar *v = lamVarFor(g, sd);
    return v ? v->vt->name : NULL;
}

/* Scan state for `collectLamVariants`: one enclosing instance plus the walk callbacks. */
typedef struct {
    CG      *g;
    Vec     *params;      /* the enclosing instance's type parameter names (borrowed) */
    Vec     *targs;       /* its type arguments (borrowed) */
    const char *ctxName;  /* its C name */
    FuncDef *ownerFn;     /* the enclosing function instance, or NULL */
    Type    *ownerInst;   /* the enclosing type instance, or NULL */
    AstVisit v;
} LamVarScan;

static bool lamVarScanExpr(void *ctx, Expr *e);
static bool lamVarScanStmt(void *ctx, Stmt *s);

/**
 * @brief Record the layout of one lambda environment for the instance being scanned.
 *
 * The environment struct, its fields and its `call` body are shared with the checker's products; what
 * is made here is the per-instance shell: a struct type named `<env>$<instance>`, a `call` method
 * whose `self` has that type, and -- when the literal is used as a bare pointer -- a thunk.
 *
 * @param sc  The scan state; the enclosing instance comes from it.
 * @param e   The `EX_LAMBDA` node.
 */
static void lamVarAdd(LamVarScan *sc, Expr *e) {
    CG *g = sc->g;
    StructDef *sd = e->u.lambda.sdef;
    if (!sd || sd->methods.len != 1 || !lamEnvNeedsVariants(sd)) return;
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *have = *(LamVar **)vecAt(&g->lamVars, i);
        if (have->sd == sd && strcmp(have->ctxName, sc->ctxName) == 0) {
            have->thunk = have->thunk || planLamFnPtr(e);
            return;
        }
    }
    FuncDef *tcall = *(FuncDef **)vecAt(&sd->methods, 0);
    const char *envName = arenaPrintf(g->arena, "%s$%s", sd->name, sc->ctxName);

    StructDef *vsd = structDefNamedCopy(g->arena, sd, envName);
    /* The plan facts are keyed by pointer, so the shell has to carry the one that marks it as a
     * lambda environment: `lamEnvNeedsVariants` reads it back. */
    planSetLamScope(vsd, planLamScope(sd));
    vecInit(&vsd->typeParams, g->arena, sizeof(const char *));
    if (sc->params)
        for (size_t i = 0; i < sc->params->len; i++)
            *(const char **)vecPush(&vsd->typeParams) = *(const char **)vecAt(sc->params, i);

    Type *vt = arenaAllocZero(g->arena, sizeof *vt);
    vt->kind = TY_GENERIC;                       /* printed as its name; the struct is emitted as a unit */
    vt->name = envName;
    vt->sdef = vsd;
    vecInit(&vt->targs, g->arena, sizeof(Type *));
    if (sc->targs)
        for (size_t i = 0; i < sc->targs->len; i++)
            *(Type **)vecPush(&vt->targs) = *(Type **)vecAt(sc->targs, i);

    /* The `call` method of this layout: the template's body and result type, with `self` typed as this
     * layout. The body is shared, so one statement tree serves every instance. */
    FuncDef *vcall = arenaAllocZero(g->arena, sizeof *vcall);
    *vcall = *tcall;
    vcall->owner = vsd;
    /* Signatures spell the layout through a `TY_STRUCT` of the same name. A `TY_GENERIC` would be
     * substituted by `cType` into a *new* instance named after its type arguments
     * (`<env>_key`), which is not the struct that was emitted. */
    Type *vtNamed = arenaAllocZero(g->arena, sizeof *vtNamed);
    *vtNamed = *vt;
    vtNamed->kind = TY_STRUCT;
    Type *selfT = arenaAllocZero(g->arena, sizeof *selfT);
    selfT->kind = TY_REF;
    selfT->inner = vtNamed;
    selfT->mut = false;                          /* `call` reads the environment */
    Param *self = paramWithType(g->arena, *(Param **)vecAt(&tcall->params, 0), selfT);
    vecInit(&vcall->params, g->arena, sizeof(Param *));
    *(Param **)vecPush(&vcall->params) = self;
    for (size_t i = 1; i < tcall->params.len; i++)
        *(Param **)vecPush(&vcall->params) = *(Param **)vecAt(&tcall->params, i);
    vecInit(&vsd->methods, g->arena, sizeof(FuncDef *));
    *(FuncDef **)vecPush(&vsd->methods) = vcall;
    /* Both loops that emit an instance's methods keep only what the program used; this layout exists
     * exactly because something used it. */
    planSetUsed(vcall, true);
    planSetInstUsed(vcall, envName);
    /* The hidden parameters of a `call` method (`home`, `zone`, `@overwrite` cells) are decided from
     * plan facts recorded on the *template* function: the lambda body may allocate, may create a pool
     * or may write an `@overwrite` cell, and the call site passes the arguments according to those
     * facts. The clone has to answer the same way or the signature and the call disagree. */
    planSetUsesHome(vcall, planUsesHome(tcall));
    planSetMakesPool(vcall, planMakesPool(tcall));
    planSetOwLocal(vcall, planOwLocal(tcall));
    planSetOwSites(vcall, planOwSites(tcall));

    LamVar *v = arenaAllocZero(g->arena, sizeof *v);
    v->sd = sd;
    v->vsd = vsd;
    v->vt = vt;
    v->vtNamed = vtNamed;
    v->vcall = vcall;
    v->ctxName = sc->ctxName;
    v->ownerFn = sc->ownerFn;
    v->ownerInst = sc->ownerInst;
    v->thunk = planLamFnPtr(e);
    *(LamVar **)vecPush(&g->lamVars) = v;
    /* The struct definition is emitted with the other units (`emitTypes`), which is also what gives it
     * its forward typedef and dependency-ordered position. */
    *(Type **)vecPush(&g->insts) = vt;
}

/** @brief Expression callback of the per-instance lambda scan. */
static bool lamVarScanExpr(void *ctx, Expr *e) {
    LamVarScan *sc = (LamVarScan *)ctx;
    if (e && e->kind == EX_LAMBDA) lamVarAdd(sc, e);
    return astWalkExprChildren(e, &sc->v);
}

/** @brief Statement callback of the per-instance lambda scan. */
static bool lamVarScanStmt(void *ctx, Stmt *s) {
    LamVarScan *sc = (LamVarScan *)ctx;
    return astWalkStmtChildren(s, &sc->v);
}

/**
 * @brief Find every lambda environment that needs one layout per instance, and make those layouts.
 *
 * Only bodies that are really emitted can contain such a lambda, so the scan walks the function
 * instances in `g->funcs` and the methods of the type instances in `g->insts` -- the same sets the
 * two body-emission loops walk. Must run after `collectEmitted` (which fills those sets) and before
 * the prototypes, because a layout is a type and a method that both have to be declared.
 *
 * @param g  The generator state; `lamVars` and the instance list grow.
 */
static void collectLamVariants(CG *g) {
    LamVarScan sc;
    sc.g = g;
    sc.v.expr = lamVarScanExpr;
    sc.v.stmt = lamVarScanStmt;
    sc.v.ctx  = &sc;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (!f || !f->body || !planTemplate(f)) continue;
        sc.params = &planTemplate(f)->typeParams;
        sc.targs  = &f->targs;
        sc.ctxName = planInstName(f);
        sc.ownerFn = f;
        sc.ownerInst = NULL;
        if (!sc.ctxName) continue;
        astWalkStmtChildren(f->body, &sc.v);
    }
    /* `g->insts` grows as layouts are added, so the count is taken first: a layout's own fields are
     * already substituted, and no layout can contain a lambda. */
    size_t n = g->insts.len;
    for (size_t i = 0; i < n; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (!inst || inst->kind != TY_GENERIC || !inst->sdef) continue;
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (!md || !md->body || !planUsed(md)) continue;
            /* The same gate as `emitInstanceBodies`: a method body is emitted for the instances that
             * used it. */
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;
            sc.params = &inst->sdef->typeParams;
            sc.targs  = &inst->targs;
            sc.ctxName = inst->name;
            sc.ownerFn = NULL;
            sc.ownerInst = inst;
            astWalkStmtChildren(md->body, &sc.v);
        }
    }
}

/**
 * @brief Drop the one-layout forms of the environments that are now emitted per instance.
 *
 * An environment that needs one layout per instance must not also be emitted once, unsubstituted:
 * its field would say `int` where the instance writes a `key`, and its template `call` would be
 * generated with a `T` in a signature. Both are removed here, after `collectLamVariants` decided
 * which environments they are -- whether or not any layout was made (a generic body that is never
 * instantiated emits nothing either way).
 *
 * @param g  The generator state; `structs` and `funcs` are compacted in place.
 */
static void dropLamTemplates(CG *g) {
    size_t w = 0;
    for (size_t i = 0; i < g->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&g->structs, i);
        if (lamEnvNeedsVariants(sd)) continue;
        *(StructDef **)vecAt(&g->structs, w++) = sd;
    }
    g->structs.len = w;
    w = 0;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f->owner && lamEnvNeedsVariants(f->owner)) continue;
        *(FuncDef **)vecAt(&g->funcs, w++) = f;
    }
    g->funcs.len = w;
}

/**
 * @brief Emit the prototypes of the per-instance lambda layouts: their `call` methods and, where the
 *        layout is used as a bare `fn(...)` value, their thunks.
 *
 * A use site may precede any definition, and the signature is the instance's, so it is printed
 * inside that instance's substitution. The `call` prototype goes through `genFuncProto` like any other
 * body, which is also what gives `deadFuncBody` the entry it fills in later and what keeps a `call`
 * method that ignores `self` compilable under `-Werror=unused-parameter`.
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamVarProtos(CG *g) {
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        SubCtx sv = substEnter(g, v->vt);
        const char *savedCtx = g->lamCtx;
        g->lamCtx = v->ctxName;
        /* Register the prototype the way the instance loop does: `deadFuncBody` pairs by name, and
         * only a paired body reaches `markUnusedParams` -- without it a `call` that ignores its
         * `home` parameter fails `-Werror=unused-parameter`. */
        size_t pb = g->out->len;
        genFuncProto(g, v->vcall);
        {
            Buf pt;
            bufInit(&pt, g->arena);
            bufPutn(&pt, g->out->data + pb, g->out->len - pb);
            DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
            df->name  = cFuncName(g, v->vcall);
            df->proto = bufCstr(&pt);
            *(DeadFunc **)vecPush(&g->deadFuncs) = df;
        }
        g->lamCtx = savedCtx;
        if (v->thunk) {
            Buf ps;
            bufInit(&ps, g->arena);
            lamThunkParams(g, v->vcall, &ps);
            bool vret = !v->vcall->ret || v->vcall->ret->kind == TY_VOID;
            cgLine(g, "static %s %s(%s);", vret ? "void" : cType(g, v->vcall->ret),
                   arenaPrintf(g->arena, "%s$fn", cFuncName(g, v->vcall)), bufCstr(&ps));
        }
        substLeave(g, sv);
    }
}

/**
 * @brief Emit the definitions of the per-instance lambda layouts: each `call` method and, when the
 *        layout is used as a bare pointer, its thunk.
 *
 * The body is generated inside the instance's substitution context, with `lamCtx` naming the
 * instance: a lambda literal in that body becomes a literal of *this* layout, and a closure call
 * reaches *this* layout's `call`.
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamVars(CG *g) {
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        SubCtx sv = substEnter(g, v->vt);
        const char *savedCtx = g->lamCtx;
        g->lamCtx = v->ctxName;
        if (v->ownerInst) planEnterInst(v->ownerInst);
        else if (v->ownerFn) planEnterFunc(v->ownerFn);
        size_t fb = g->out->len;
        CGACC("cg-emit", genFunc(g, v->vcall));
        deadFuncBody(g, v->vcall, fb, g->out->len - fb);
        if (v->ownerInst) planLeaveInst();
        else if (v->ownerFn) planLeaveFunc();
        if (v->thunk) {
            const char *callName = cFuncName(g, v->vcall);
            Buf ps;
            bufInit(&ps, g->arena);
            lamThunkParams(g, v->vcall, &ps);
            bool vret = !v->vcall->ret || v->vcall->ret->kind == TY_VOID;
            cgLine(g, "");
            cgLine(g, "/* A `fn(...)` value for a lambda that captures nothing, laid out for one");
            cgLine(g, " * instance: the empty environment is a static dummy. */");
            cgLine(g, "static %s %s$fn(%s) {", vret ? "void" : cType(g, v->vcall->ret), callName,
                   bufCstr(&ps));
            cgLine(g, "    static %s __env;", cType(g, v->vtNamed));
            Buf args;
            bufInit(&args, g->arena);
            for (size_t k = 1; k < v->vcall->params.len; k++) {
                Param *p = *(Param **)vecAt(&v->vcall->params, k);
                bufPrintf(&args, "%s%s", k > 1 ? ", " : "", p->cname);
            }
            if (vret)
                cgLine(g, "    %s(&__env%s%s);", callName, args.len ? ", " : "", bufCstr(&args));
            else
                cgLine(g, "    return %s(&__env%s%s);", callName, args.len ? ", " : "",
                       bufCstr(&args));
            cgLine(g, "}");
        }
        g->lamCtx = savedCtx;
        substLeave(g, sv);
        cgLine(g, "");
    }
}


/**
 * @brief Emit the function that runs the `@drop` hooks of module-level bindings at process exit.
 *
 * A global is a `static` object whose life is the whole process, so nothing in the block machinery
 * ever reaches it: without this, a global of a type with an `@drop` hook would abandon whatever the
 * hook is supposed to hand back. `main` registers it with `atexit`, which runs it before the console
 * flush (LIFO) so anything it prints still makes it out.
 *
 * @param g  Generator; emits nothing when the module has no such global.
 */
static void emitGlobalDrops(CG *g) {
    if (!g->needGlobalDrops) return;
    cgLine(g, "static void extc_drop_globals(void) {");
    for (size_t i = 0; i < g->globalDrops.len; i++) {
        GlobalDrop *d = (GlobalDrop *)vecAt(&g->globalDrops, i);
        genDropValue(g, d->name, d->declT, 0);
    }
    cgLine(g, "}");
    cgLine(g, "");
}

/**
 * @brief Emit the unit-wide prototype pool: before every function body, and prototypes only.
 *
 * No definition may appear here: a definition preceding the prototype of a function it calls makes C
 * treat the call as an implicitly declared `int()` and the real prototype then becomes a conflicting
 * type. The parameter list must be built by the same `cgParamList` as the definition: a function with
 * a home arena has one extra hidden parameter, and without it the C types do not match. Instance
 * methods are emitted only when called (`planUsed`), and the coroutine protocol is not emitted here --
 * it is inlined at the call site.
 *
 * @param g     The code generation context; `out` receives the prototypes and `deadFuncs` the
 *              prunable entries.
 * @param m     The current module.
 * @param arena The scratch allocator for assembling signature text.
 */
static void emitProtos(CG *g, Module *m, Arena *arena) {
    /* The thunks that turn a lambda that captures nothing into a bare function pointer. They are
     * declared with everything else: a use site may precede any definition. */
    for (size_t i = 0; i < g->lamThunks.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&g->lamThunks, i);
        FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
        Buf ps;
        bufInit(&ps, g->arena);
        lamThunkParams(g, md, &ps);
        bool vret = !md->ret || md->ret->kind == TY_VOID;
        cgLine(g, "static %s %s(%s);", vret ? "void" : cType(g, md->ret), lamThunkName(g, sd),
               bufCstr(&ps));
    }
    emitLamVarProtos(g);
    /* The prototype pool: every function is declared before any body, and only
     * a prototype may appear here. A definition emitted before the prototype of
     * a function it calls makes C take that function as implicitly declared
     * `int()`, and the later real prototype is then a conflicting type.
     *
     * Printing and structural `==` go through the descriptor table, so none of
     * the derived functions needs a forward declaration here. */
    /* The prototypes of instance methods; an array has no sdef and no methods. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (inst->kind != TY_GENERIC) continue;
        if (inst->sdef && lamEnvNeedsVariants(inst->sdef)) continue;   /* see emitLamVarProtos */
        SubCtx sv = substEnter(g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            /* Only a method that is really called is emitted, which keeps the
             * generated C smaller. The flag is a conservative approximation: it
             * is set whenever the template body mentions a call, so a closure
             * such as `push` calling `grow` is included automatically, and
             * emitting too much is the safe direction. */
            if (planCoroProto(md)) continue;   /* emitted inline at the call site */
            if (!planUsed(md)) continue;
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;   /* no other instance has used it: its element type may not have the operator this method needs */

            /* Instance methods are candidates too: a library method of a generic
             * instance is marked used even when only dead code mentions it, and
             * without an entry here nothing takes it back out. The pairing is by
             * name, so one node backing several instances is not a problem. */
            size_t pb = g->out->len;
            genFuncProto(g, md);
            {
                Buf pt;
                bufInit(&pt, g->arena);
                bufPutn(&pt, g->out->data + pb, g->out->len - pb);
                DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
                df->name  = cFuncName(g, md);
                df->proto = bufCstr(&pt);
                *(DeadFunc **)vecPush(&g->deadFuncs) = df;
            }
        }
        substLeave(g, sv);
    }
    /* Methods of ordinary structs and free functions: their order does not
     * matter, and mutual calls are covered by the prototypes above. */
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        SubCtx sv = substEnterFunc(g, f);
        bool entry = cgIsEntry(g, f);
        const char *ret = entry ? "int" : cType(g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        /* The parameter list must come from the same cgParamList the definition
         * uses: a function with a home arena has one hidden parameter more, and
         * leaving it out of the prototype produced a C type mismatch, which was
         * hit for real. */
        /* An external declaration is not `static`, so that the linker can see
         * it, and only its prototype is emitted. */
        /* `@inline` has to be on the prototype too, or the attribute on the definition
         * alone does not bind: C takes the first declaration as the function's type. */
        if (planIsCoro(f)) continue;   /* a coroutine emits `$frame` + `$step` last */
        bufPrintf(&sig, "%s%s %s(%s);",
                  entry ? (g->needModuleTramp ? "static " : "")
                        : ((f->isExtern || f->isExport) ? ""
                                                        : (f->isInline ? "EXTC_INLINE " : "static ")),
                  ret, cgEntryCName(g, f), cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
        /* An exported function is registered like `main`: nowhere. The pruners drop a definition
         * only when its name occurs nowhere but its own declaration and definition, and a symbol
         * C calls is mentioned by nobody in this file -- so registering it would delete it. */
        if (!entry && !f->isExtern && !f->isExport) {
            DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
            df->name  = cFuncName(g, f);
            df->proto = bufCstr(&sig);
            /* A method a `dyn` table names is still registered: the table is spliced in after the
             * "definitions nothing names" pass, so that pass cannot see the reference, and
             * `namedByDynTable` is how it is told to keep the body. Registering it also matters for
             * the parameter pass -- an entry with no body is skipped there, and then a method that
             * ignores `self` (the ordinary shape of a trait implementation) fails the generated C
             * with `-Werror=unused-parameter`. */
            df->namedByDynTable = planDynTable(f) || inTraitTable(m, f);
            *(DeadFunc **)vecPush(&g->deadFuncs) = df;
        }
        substLeave(g, sv);
    }
    if (g->structs.len || g->insts.len || g->funcs.len) cgLine(g, "");
    if (g->needGlobalDrops) cgLine(g, "static void extc_drop_globals(void);");
}

/**
 * @brief Initialize the code generation state: instance tables, the coroutine-kind pre-scan and the
 *        event layer flags.
 *
 * Every emitted coroutine function gets an increasing `kind` (the boxing points and the handle dispatch
 * must agree), and `needCoroHandle` is set as soon as one coroutine's frame is promoted to a handle.
 * The event layer comes in only when the program uses one of `extc_epoll_*`, `extc_sock_*` or
 * `extc_tcp_*`.
 *
 * @param g     The code generation context, whose vectors and buffers are initialized here.
 * @param m     The current module; `funcs` is the source of the coroutine pre-scan.
 * @param tt    The type table; an instance that still mentions a type parameter is not emitted as C.
 * @param arena The allocator every vector and buffer in CG is allocated from.
 */
static void initCGState(CG *g, Module *m, TypeTable *tt, Arena *arena) {
    vecInit(&g->structs, arena, sizeof(void *));
    vecInit(&g->hoistedIdx, arena, sizeof(Expr *));
    bufInit(&g->prefix, arena);          /* statement prefix; uninitialized, it segfaults */
    /* The instance table is deduplicated by C name: a writable and a read-only
     * view are one C struct, so the type table can hold two instances under one
     * name and neither may be generated twice. */
    /* Coroutine handle prepass: one `kind` per coroutine function (the boxing site and the handle
     * dispatch must agree on it), and whether this program needs the handle type at all -- the
     * checker records the "a frame was coerced into a handle" fact through `planSetCoroBoxed`. */
    {
        int ck = 0;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
            if (!coroEmitted(cf)) continue;
            /* Derived here rather than written back into the shared analysis
             * state: the instance's own flag counts, and so does the template's. */
            bool boxed = planCoroBoxed(cf)
                      || (planTemplate(cf) && planCoroBoxed(planTemplate(cf)));
            planSetCoroKind(cf, ck++);
            if (boxed) g->needCoroHandle = true;
        }
        /* The event layer is needed if anything used calls one of its entry points. */
        for (size_t fi = 0; fi < m->funcs.len; fi++) {
            FuncDef *ef = *(FuncDef **)vecAt(&m->funcs, fi);
            if (ef && planUsed(ef) && ef->name &&
                (strncmp(ef->name, "extc_epoll_", 11) == 0 || strncmp(ef->name, "extc_sock_", 10) == 0 ||
                 strncmp(ef->name, "extc_tcp_", 9) == 0))
                g->needEvent = true;
        }
    }
    vecInit(&g->insts, arena, sizeof(void *));
    vecInit(&g->moduleEntries, arena, sizeof(ModEntry));
    /* Reclamation's compile-time state: all three vectors start empty and are set up by `vecInit`
     * rather than by CG being zeroed. */
    vecInit(&g->rcRoots, arena, sizeof(const char *));
    vecInit(&g->rcRootMarks, arena, sizeof(size_t));
    vecInit(&g->dropBlocks, arena, sizeof(Stmt *));
    vecInit(&g->dropExplicit, arena, sizeof(const char *));
    vecInit(&g->dropLoopBases, arena, sizeof(size_t));
    vecInit(&g->globalDrops, arena, sizeof(GlobalDrop));
    vecInit(&g->rcGlobalRoots, arena, sizeof(const char *));
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->instances, i);
        /* A type that still mentions a type parameter gets no C: `[2]box<T>`
         * inside a generic body is interned unconditionally, and emitting its
         * struct would name the undefined `box_T`. */
        if (!it || mentionsParam(it)) continue;
        bool dup = false;
        for (size_t k = 0; k < g->insts.len && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&g->insts, k))->name, it->name) == 0;
        if (!dup) *(Type **)vecPush(&g->insts) = it;
    }
}

/**
 * @brief Scan the module's declarations and decide which optional runtime blocks have to be emitted.
 *
 * The trigger is a declaration rather than a call: an `extern!` or `@builtin` primitive appearing in
 * the module means the program uses the corresponding runtime. Pools have one more source: the
 * `makesPool` the checker computed, because the `extc_pool_*` declarations live in the library module
 * and the entry module's function table cannot see them. This scan must run before the preamble,
 * because `_GNU_SOURCE` is honored only before the first include.
 *
 * @param g  The code generation context; sets `needRawTerm`, `needPool`, `needCout`, `needMemFind`,
 *           `needPlateCopy`, `needPlateView`, `needTime`, `needHttpFile` and `needDns`.
 * @param m  The current module; the declarations in `funcs` and `structs` are the only sources.
 */
static void scanUnitNeeds(CG *g, Module *m) {
    /* The reclamation switch: on by default, `EXTC_RECLAIM=0` turns it off, and a nonzero value forces it
     * **on** (skipping the allocation-point criterion below). The criterion itself is not here -- this
     * pass only sets the intent from the environment, and once `collectEmitted` has picked the functions
     * that really get emitted, `scanUnitReclaim` narrows it to "is there a reclamation-governed
     * allocation point". */
    const char *rcEnv = getenv("EXTC_RECLAIM");
    g->rcForced = rcEnv && *rcEnv && strcmp(rcEnv, "0") != 0;
    g->needReclaim = !(rcEnv && *rcEnv && strcmp(rcEnv, "0") == 0);
    /* `EXTC_RC_SHARDS=N`: the reclamation **shard count** (the pause knob), 1 by default. Only a value
     * above 1 cuts each logical region into N run-time regions; 1, unset, empty, non-numeric and
     * out-of-range all mean 1 (single-shard behavior). The ceiling is 64: beyond that the baseline of
     * at least one 1 MiB slab per shard outweighs the pause benefit. */
    g->rcNShard = 1;
    /* The default is the **no-count** fast path; `EXTC_RC_NOCOUNT=0` restores counting. */
    g->rcNoCount = 1;
    {
        const char *sh = getenv("EXTC_RC_SHARDS");
        if (sh && *sh) {
            long v = strtol(sh, NULL, 10);
            if (v > 1) g->rcNShard = (int)(v > 64 ? 64 : v);
        }
    }
    /* `EXTC_RC_LOCALSTORE`: a dedicated entry point for local and parameter slots. **On by default**;
     * `=0` is the kill switch, and with it off the corpus output is byte-identical (the two paths
     * produce the same text on every shape). It only affects the no-count tier (see the emission
     * point). */
    g->rcLocalStore = 1;
    {
        const char *ls = getenv("EXTC_RC_LOCALSTORE");
        if (ls && *ls && strcmp(ls, "0") == 0) g->rcLocalStore = 0;
    }
    /* `EXTC_RC_NOCOUNT`: the holder-counting switch. **Default 1 = no counting** (reclaimed by sweeping
     * alone): on the same 31 shape ratchets, ASan corpora, gaps sets, pause probe and absolute peak of a
     * large-live-set churn, the no-count tier is no worse and uses strictly fewer instructions.
     * `=0` restores "count plus early reclamation" (the count-narrowing analysis is meaningful only in
     * that mode too). */
    {
        const char *nc = getenv("EXTC_RC_NOCOUNT");
        if (nc && *nc && strcmp(nc, "0") == 0) g->rcNoCount = 0;
    }
    /* `EXTC_RC_SWEEPFREE`: the **event tier** -- a program without cycles has nothing to sweep: every
     * unreachable object is freed on the spot when its last holder disappears (the count-reaches-zero
     * fast path). So counting is turned on and `shouldSweep` is forced to 0 (see
     * `EXTC_RC_SWEEP_FREE` in `src/back/reclaim.c`).
     *
     * It also serves as a **diagnostic switch** (off by default, byte-identical product); the acyclicity
     * decision itself belongs to the analysis side, and a wrong decision errs towards "should have
     * swept but did not => memory grows", not towards dangling -- so the criteria come first: the sweep
     * count must be 0, peak RSS must be bounded, and forcing the decision to true on cyclic shapes must
     * blow up. */
    /* Three states: unset = decided by analysis (`unitAllAcyclic`); `=1` = forced on (the criteria's red
     * proof uses it); `=0` = forced off (for comparison and A/B -- otherwise, once the analysis decides
     * it holds, there is no way to compare against the other behavior). */
    {
        const char *sf = getenv("EXTC_RC_SWEEPFREE");
        g->rcSweepFree = 0;
        if (sf && *sf) g->rcSweepFree = (strcmp(sf, "0") == 0) ? -1 : 1;
        if (g->rcSweepFree == 1) g->rcNoCount = 0;
    }
    /* `EXTC_DBG_RCCOST=1`: count rc sites per block level (diagnostic, byte-identical product). */
    if (getenv("EXTC_DBG_RCCOST")) g->dbgCost = 1;
    /* Does the program use the raw-terminal primitives? They arrive as an `extern!` the
     * library declares (`extc_raw_enter` in `std::sys::term`), which is the one thing
     * codegen can see from here. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExtern || !f->name) continue;
        if (strcmp(f->name, "extc_raw_enter") == 0) g->needRawTerm = true;
        /* The pool registry, on the same footing: the library declares these through
         * `extern!`, so their presence is what says the program uses pools at all. */
        if (strcmp(f->name, "extc_cout_put") == 0)  g->needCout    = true;
        /* The byte-search runtime, on the same footing: `std::sys::mem` is the only module
         * that declares these, and its declarations are what says the program wants `memmem`
         * instead of a byte loop. The flag is read again before the includes (the `_GNU_SOURCE`
         * block below), so this scan has to run before the preamble -- it does. */
        if (f->externLib && strcmp(f->externLib, "extc-mem") == 0)  g->needMemFind = true;
        /* The plate runtime is shaped like memfind: the trigger is a **declaration** (only the privileged
     * layer std::sys::heap declares it). */
        if (f->externLib && strcmp(f->externLib, "extc-heap") == 0) {
            if (plateIsMemCopyName(f->name)) g->needPlateCopy = true;
            else                             g->needPlateView = true;
        }
        if (f->externLib && strcmp(f->externLib, "extc-time") == 0)  g->needTime = true;
        /* The five file-level shims: they share the `extc-runtime` library tag with net => they can only be
         * recognized **by name**. This scan must run before the preamble (`_GNU_SOURCE` is honored only
         * before the first include), so the flags are set here rather than in `genCoroDecls`. */
        if (f->name && (strncmp(f->name, "extc_file_", 10) == 0 ||
                        strncmp(f->name, "extc_sendfile", 13) == 0 ||
                        strncmp(f->name, "extc_http_", 10) == 0))  g->needHttpFile = true;
        if (f->name && strcmp(f->name, "extc_dns_lookup") == 0)  g->needDns = true;
    }
    /* Pools have one more source: **`makesPool`, which the checker has already computed**. Looking only
     * at "this module declares an extern of `extc_pool_*`" is not enough -- those declarations live in
     * the **library module** while `m` is the entry module, so at main's prologue `needPool` would still
     * be false => the frame's zone (`__extc_zm1`) is not emitted => raising to level 1 cannot be
     * expressed (`zoneArgRef` silently falls back to the dynamic fallback). `makesPool` is a transitive
     * least fixed point: any function (methods included) being true means the program uses pools --
     * which is exactly the question this cell has to answer. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* Only a function that is actually emitted can put a pool in the program: the prelude
         * declares `varArray`, whose methods make pools, in every program; counting those would
         * charge the pool runtime to programs that never touch it. */
        /* The same holds for the one `@builtin` primitive: it is declared in the privileged layer
         * (`std::sys::heap`) and the compiler supplies the body. It is not `extern!`, so the
         * `externLib`-based scan above cannot see it. */
        if (f && f->isBuiltin && plateIsViewOfName(f->name)) g->needPlateView = true;
    }
    /* `makesPool` over the functions the program actually reaches is the honest signal: the
     * prelude declares `varArray`, whose methods make pools, in every program, so their mere
     * presence must not pull the runtime in. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* `main`/`@export`/a module entry are roots the pruning keeps without a `planUsed` bit. */
        if (f && !f->isExtern && (planUsed(f) || cgIsMain(f) || f->isExport || f->isModuleEntry) &&
            planMakesPool(f))
            g->needPool = true;
    }
}

/* ==================== Region reclamation: whether this pass emits at all ====================
 *
 * Reclamation governs heap storage at exactly three creation points, all inside codegen:
 *
 *   - `new T` (single value or fixed-size `new [N]T`) -- the branch of `exNew` without a length;
 *   - `alloc<T>(n)` -- the arena branch of `exGencall`;
 *   - a **single-value** `@overwrite` cell -- that branch also goes through `new T`, so its
 *     initializer is the first case above.
 *
 * A `new T[n]` without a length stays in the arena (a view is not a reference slot), and so does a
 * variable-length `@overwrite` cell, so neither counts. `@overwrite` needs no separate criterion: its
 * initializer is `new T`, and walking the expression sees it.
 *
 * This pass scans the function bodies that are **about to be emitted** (`g->funcs`, already pruned by
 * `collectEmitted`) plus the method bodies of generic instances plus global initializers, so "appears
 * in the program" means **appears in the product**: a program whose `new` exists only in dead functions
 * carries no runtime. Erring the other way is harmless too (it only carries a runtime block nobody
 * calls) -- the criterion prefers to be wide.
 */

/** @brief Statement callback: forwards to the generic expression/statement walk (declared first
 * because `rcAllocInExpr`'s visitor references it). */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s);

/** @brief Expression callback: returns false and stops the whole walk when a "reclamation-governed
 * allocation point" is hit. */
static bool rcAllocInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (!e) return true;
    if (e->kind == EX_NEW) {
        if (!e->u.new_.count) return false;   /* a single object is an rc slot */
        /* A block of them is an rc slot only where `exNew` puts it there: the function-body level.
         * Elsewhere the arena's release point (an inner block, or the home arena the caller picked)
         * already bounds it, so it is not a reclamation-governed allocation. */
        return planArenaLevel(e) != 1;
    }
    if (e->kind == EX_GENCALL)
        return !(e->u.gencall.name && strcmp(e->u.gencall.name, "alloc") == 0);
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Does a function body contain a reclamation-governed allocation point?
 *
 * @param body  The function body; false when it is NULL (an extern, for instance).
 * @return      true when one is present.
 */
static bool rcBodyHasGovernedAlloc(Stmt *body) {
    if (!body) return false;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return !astWalkStmtChildren(body, &v);
}

/** @brief Statement callback: walks every child of the statement
 * (`rcBodyHasGovernedAlloc`'s visitor). */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s) {
    (void)ctx;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Can the whole unit **skip emitting the rc runtime entirely**: every used function judged
 *        "loses no live reference"?
 *
 * The criteria (a)(b)(c) are computed by the checker (`planFnReclaimIdle`; the per-site scan is
 * `idleWalkStmt` in `check_top.c`): (a) every heap `ref` field is written at most once per object;
 * (b) a local or parameter of a ref type is never reassigned; (c) there is no reference-dropping
 * operation (container removal/replacement, `@overwrite` reusing one cell, the free-like builtins, an
 * opaque callee taking a reference). All this function does is conjoin over the whole unit -- **one
 * unsatisfied site => the whole unit is judged false**, which is the conservative direction (false
 * means the rc runtime is emitted as usual). Transitivity is therefore covered automatically: a callee
 * that drops a reference is itself an emitted function, and the conjunction sees it (an opaque callee
 * has already been judged false locally, per parameter).
 *
 * A global's initializer sits in no function body and the criteria cannot reach it => as soon as it
 * has a reclamation-governed allocation, return 0 conservatively (the same stance as
 * `unitAllAcyclic`).
 *
 * Failure direction: a wrong judgment under-collects => **memory grows**, it does not dangle (rc only
 * frees objects the markers do not reach). Safety nets: peak RSS bounded with the switch on and off on
 * the same shape, plus N vs 4N < 1.5x (safety cell 7 of `tests/reclaim-shapes` and
 * `tests/reclaim-acyclic`), and a red proof with `tools/inject.py reclaim-idle-always` forcing the
 * judgment to true.
 */
static int unitReclaimIdle(CG *g, Module *m) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        /* A function without a body (an extern declaration): the same load-bearing assumption as
         * `unitAllAcyclic` -- an extern has no body, and the references it receives are constrained by
         * the escape/borrow rules at the call site (the caller's reference outlives the call), so it
         * cannot be the place where "the last root is dropped". */
        if (f && f->body && !planFnReclaimIdle(f)) return 0;
    }
    /* The method bodies of generic instances: the template itself does not enter `g->funcs`; instance
     * bodies are emitted by `emitInstanceBodies`. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && md->body && !planFnReclaimIdle(md)) return 0;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return 0;
    }
    return 1;
}

/**
 * @brief Can the whole unit take the "event tier": every used function judged acyclic and references
 *        living only in counted slots?
 *
 * The criteria are computed by the checker (`planFnAcyclic`, which covers both "acyclic" and "every
 * position that can drop a reference is counted"); all this function does is conjoin over the whole
 * unit. A global's initializer sits in no function body and the criteria cannot reach it => as soon as
 * it has a reclamation-governed allocation, return 0 conservatively.
 *
 * Failure direction: a wrong judgment means "should have swept but did not" => memory grows, it does
 * not dangle => the forced tiers and the criteria are in `tests/reclaim-acyclic/` (the sweep count must
 * be 0 **and** peak RSS bounded; forcing the judgment to true on a cyclic shape must turn red).
 */
static int unitAllAcyclic(CG *g, Module *m) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        /* A function without a body (an extern declaration): `collectEmitted` pushes `planUsed` externs into
         * `g->funcs` as well, and they never have an analysis verdict => they must not turn the whole
         * unit false, or **any program that calls a C extern could not enter the event tier** (`io`'s
         * printing path pulls in `extc_cout_*`; the criterion group's success shapes are exactly what
         * this would block).
         *
         * Load-bearing assumption: an extern has no body of its own => it never writes our reference
         * slots; the references it receives are constrained by the escape/borrow rules at the **call
         * site**, and the caller's reference outlives the call => it cannot make an object in the region
         * unreachable. The assumption is recorded here, and its failure direction is "should have swept
         * but did not => memory grows" (not dangling), watched by `gate-acyclic`'s RSS bound and red
         * proof. */
        if (f && f->body && !planFnAcyclic(f)) return 0;
    }
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && md->body && !planFnAcyclic(md)) return 0;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return 0;
    }
    return 1;
}

/**
 * @brief Narrow `needReclaim`: no reclamation-governed allocation point means no runtime is emitted.
 *
 * When `EXTC_RECLAIM=1` (`rcForced`) the whole pass is skipped -- the gate is written for the "switch
 * on" tier regardless of whether an allocation point exists.
 *
 * Must run after `collectEmitted` and before `emitPreamble`/`emitRuntimePrologue`: the criterion needs
 * `g->funcs` (the pruned function list), and the runtime text enters the preamble based on this flag.
 *
 * @param g  The generator; `funcs` and `insts` are already filled in.
 * @param m  The module; global initializers are inspected from it.
 */

static void scanUnitReclaim(CG *g, Module *m) {
    if (!g->needReclaim) return;
    /* (2) "skip the rc runtime entirely": the whole unit is proved to have no reclamation work => no
     * runtime is emitted **in any tier**.
     *
     * This is a **proof**, not an optimization switch: when the proof holds, emitting the runtime would
     * just spin. So `EXTC_RECLAIM=1` means "**allow** reclamation / do not force it off", not "force
     * emission". The criteria (a)(b)(c) and the "one unsatisfied site => whole unit false" conjunction
     * are in `unitReclaimIdle`.
     *
     * Failure direction: under-collection => memory grows, it does not dangle; the safety nets are in
     * the note on `unitReclaimIdle`.
     *
     * (This is a **compile-time** early exit: it is not in the emitted runtime text, so it does not
     * belong in the "fast path and skips" table of the reclamation notes -- that table is kept aligned
     * in both directions with the runtime text alone by `tools/check_skip_table.py`.) */
    if (unitReclaimIdle(g, m)) {
        g->needReclaim = false;
        return;
    }
    /* (3) "this unit has a possible cross-site cycle" => **sharding is refused** (`EXTC_RC_SHARDS > 1`
     * degrades to one shard in this unit). Reason: shard boundaries must align with structural
     * boundaries -- cutting a cycle apart turns the cross-shard registration bits into seeds for cyclic
     * garbage, and the bound grows with the round count (`digraph` with `SHARDS=4`: 4N/N = 3.84x, the
     * same order as naive RC). Pinning only "the sites of the same cycle group" is not enough: a site
     * outside the group (a container, say) still lives in another shard, and a cross-shard reference
     * still pins the whole cycle. A single-site self-cycle (one allocation point per chain in
     * `shard_bands`) is unaffected => the pause benefit of the shard knob survives. */
    for (size_t i = 0; i < g->funcs.len && g->rcNShard > 1; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && f->body && planFnCrossSiteCycle(f)) {
            if (dbgOn("EXTC_DBG_ZONE"))
                fprintf(stderr,
                        "[shard] unit has a cycle across sites (fn=%s); refusing to shard\n",
                        f->name ? f->name : "?");
            g->rcNShard = 1;
        }
    }
    for (size_t i = 0; i < g->insts.len && g->rcNShard > 1; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && planFnCrossSiteCycle(md)) { g->rcNShard = 1; break; }
        }
    }
    if (g->rcForced) return;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && rcBodyHasGovernedAlloc(f->body)) return;
    }
    /* The method bodies of generic instances: the template itself does not enter `g->funcs` (its type
     * parameters are still there), and instance bodies are emitted by `emitInstanceBodies`, so they are
     * covered here from each instance's `sdef`. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && rcBodyHasGovernedAlloc(md->body)) return;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return;
    }
    g->needReclaim = false;
}

/**
 * @brief Initialize CG's runtime buffers and write the fixed runtime text into them.
 *
 * The text is written once here; whether it enters the final product is decided by the individual
 * `need*` flags or demand tables, e.g. `rt` is spliced only when there is printing, structural equality
 * or a descriptor line. The console buffer, the `%g` formatter and the raw-terminal restore block each
 * carry static state and must be emitted as a whole.
 *
 * @param g     The code generation context, whose buffers are initialized and pre-filled here.
 * @param arena The allocator the buffer storage comes from.
 */
static void initRuntimeText(CG *g, Arena *arena) {
    vecInit(&g->funcs, arena, sizeof(void *));
    vecInit(&g->helpers, arena, sizeof(SliceHelper));
    vecInit(&g->unwraps, arena, sizeof(UnwrapHelper));
    vecInit(&g->viewIdx, arena, sizeof(Type *));
    vecInit(&g->viewCpy, arena, sizeof(Type *));
    vecInit(&g->viewCpyU, arena, sizeof(Type *));
    vecInit(&g->descs, arena, sizeof(void *));
    vecInit(&g->eqNeed, arena, sizeof(void *));
    vecInit(&g->deadDefs, arena, sizeof(DeadDef *));
    vecInit(&g->deadFuncs, arena, sizeof(DeadFunc *));
    vecInit(&g->deadLocals, arena, sizeof(DeadLocal *));
    vecInit(&g->primEnts, arena, sizeof(PrimEnt *));
    vecInit(&g->lamThunks, arena, sizeof(StructDef *));
    vecInit(&g->lamVars, arena, sizeof(LamVar *));
    vecInit(&g->dynCalls, arena, sizeof(Expr *));
    g->lamCtx = NULL;
    bufInit(&g->desc, arena);
    bufInit(&g->rcDesc, arena);
    vecInit(&g->rcDescKeys, arena, sizeof(const char *));
    bufInit(&g->rt, arena);              /* print/compare runtime: emitted only if needed */
    bufInit(&g->rtPrint, arena);
    bufInit(&g->rtEq, arena);
    bufInit(&g->rtCout, arena);
    bufPuts(&g->rtCout,
        "/* Buffered console output (ruling 90). The buffer lives in the runtime rather than\n"
        " * in the library so that the flush has a home the generator can call; the library\n"
        " * formats and calls `extc_cout_put`, which the C compiler inlines into it (same\n"
        " * translation unit, small body). */\n"
        "static uint8_t extc_cout_buf[1 << 18];\n"
        "static int64_t extc_cout_len = 0;\n"
        /* Prototypes ahead of the definitions: these two have external linkage on purpose (the
         * library reaches them through `extern!`, so they cannot be `static`), and clang asks for a
         * declaration in view before the definition (`-Wmissing-prototypes`). */
        "void extc_cout_flush(void);\n"
        "void extc_cout_put(uint8_t *p, int64_t n);\n"
        "void extc_cout_f64(double v);\n"
        "void extc_cout_flush(void) {\n"
        "    if (extc_cout_len <= 0) return;\n"
        "    /* Through stdio: `unistd.h` cannot be included here (the library declares\n"
        "     * `read`/`write` with its own prototypes, and the two clash). stdio also keeps\n"
        "     * the order with the deprecated print path and flushes at exit by itself. */\n"
        "    fwrite(extc_cout_buf, 1, (size_t)extc_cout_len, stdout);\n"
        "    /* `fwrite` alone only fills stdio's own buffer, which stays invisible while stdout is\n"
        "     * a file or a pipe (fully buffered) until the process exits: a program that prints,\n"
        "     * calls `io::flushCout` and then keeps running would show nothing to a reader. */\n"
        "    fflush(stdout);\n"
        "    extc_cout_len = 0;\n"
        "}\n"
        "void extc_cout_put(uint8_t *p, int64_t n) {\n"
        "    if (n <= 0) return;\n"
        "    /* A single write **larger than the whole buffer** goes straight out, after the buffer\n"
        "     * is drained so the order is kept. Without this the memcpy below runs past the end of\n"
        "     * `extc_cout_buf`: `io::cout << <300 KB slice>` aborts with glibc's\n"
        "     * \"buffer overflow detected\" (ASan reports a global-buffer-overflow WRITE of the\n"
        "     * whole length). Flushing only when the buffer is *nearly* full does nothing for a\n"
        "     * write bigger than the buffer itself; this branch takes a single write of a whole\n"
        "     * buffer (256 KiB) or more. */\n"
        "    if (n >= (int64_t)sizeof extc_cout_buf) {\n"
        "        extc_cout_flush();\n"
        "        fwrite(p, 1, (size_t)n, stdout);\n"
        "        fflush(stdout);\n"
        "        return;\n"
        "    }\n"
        "    if (extc_cout_len + n > (int64_t)sizeof extc_cout_buf) extc_cout_flush();\n"
        "    memcpy(extc_cout_buf + extc_cout_len, p, (size_t)n);\n"
        "    extc_cout_len += n;\n"
        "}\n"
        /* A `printf` stand-in: it **appends the formatted result to the same cout buffer**, so built-in
         * `print`/`println`, descriptor printing and `io::cout` have one output path and one buffer, and
         * mixed writes are ordered by construction (no `fflush` needed). extC itself has no variadics,
         * but the generated C can: this is purely an internal detail of the compiler. */
        "void extc_printf(const char *fmt, ...) {\n"
        "    char b[256];\n"
        "    va_list ap;\n"
        "    int n;\n"
        "    va_start(ap, fmt);\n"
        "    n = vsnprintf(b, sizeof b, fmt, ap);\n"
        "    va_end(ap);\n"
        "    if (n <= 0) return;\n"
        "    if ((size_t)n > sizeof b - 1) n = (int)(sizeof b - 1);\n"
        "    extc_cout_put((uint8_t *)b, (int64_t)n);\n"
        "}\n");
    /* The one formatter the library cannot write itself: extC has no variadics,
     * so `snprintf` is unreachable and this is the fixed-arity door to it. `%g`
     * fits `char[32]` for every double, and `snprintf` truncates rather than
     * overruns if it does not.
     *
     * It lives in a buffer of its own because it is appended only when a call
     * site reaches it (`needCoutF64`): a program that never prints a float
     * should not carry the formatter. */
    bufInit(&g->rtCoutF64, arena);
    bufPuts(&g->rtCoutF64,
        "void extc_cout_f64(double v) {\n"
        "    char b[32];\n"
        "    int n = snprintf(b, sizeof b, \"%g\", v);\n"
        "    if (n > 0) extc_cout_put((uint8_t *)b, (int64_t)n);\n"
        "}\n");
    bufInit(&g->rtRaw, arena);
    bufPuts(&g->rtRaw,
        "/* ---- raw terminal: give it back even when the program dies ----\n"
        " * The library turns raw mode on through `extc_raw_enter`, handing over a copy of\n"
        " * the settings it found. The trap path then restores them through the dying hook\n"
        " * above: a program that traps in raw mode still issues\n"
        " * the `TCSETS` that puts the terminal back before it exits.\n"
        " * The buffer is opaque bytes; the layout of `struct termios` is never named, and\n"
        " * `cfmakeraw` on the library side is what decides what raw means. */\n"
        "static int32_t __extc_raw_fd = -1;\n"
        "static uint8_t __extc_raw_saved[256];\n"
        "static int32_t __extc_raw_on;\n"
        "extern int32_t tcsetattr(int32_t fd, int32_t action, uint8_t *buf);\n"
        "int32_t extc_raw_leave(void) {\n"
        "    int32_t r;\n"
        "    if (!__extc_raw_on) return 0;\n"
        "    __extc_raw_on = 0;\n"
        "    if (__extc_raw_fd < 0) return 0;\n"
        "    r = tcsetattr(__extc_raw_fd, 0 /* TCSANOW */, __extc_raw_saved);\n"
        "    __extc_raw_fd = -1;\n"
        "    return r;\n"
        "}\n"
        "void extc_raw_enter(int32_t fd, uint8_t *buf, int64_t n) {\n"
        "    int64_t i;\n"
        "    if (n > (int64_t)sizeof __extc_raw_saved) n = (int64_t)sizeof __extc_raw_saved;\n"
        "    for (i = 0; i < n; i++) __extc_raw_saved[i] = buf[i];\n"
        "    __extc_raw_fd = fd;\n"
        "    __extc_raw_on = 1;\n"
        "    __extc_dying  = extc_raw_leave;   /* if we die before restore(), put it back */\n"
        "}\n");
    bufInit(&g->body, arena);
    bufInit(&g->coroDefs, arena);    /* appended last: see `CG.coroDefs` */
    g->tmpSeq = 0;
}

/** @brief Expression callback: will this program use the console buffer (built-in printing or
 * `io::cout`)? */
static bool coutScanExpr(void *ctx, Expr *e) {
    bool *hit = ctx;
    if (!e) return true;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (n && (strcmp(n, "print") == 0 || strcmp(n, "println") == 0 ||
                  strcmp(n, "flush") == 0))
            *hit = true;
    }
    FuncDef *callee = planCallee(e);
    if (callee && callee->name && strstr(callee->name, "ostream_shl")) *hit = true;
    return true;
}

/** @brief Statement callback: check the statement itself first, then let the generic walk descend
 * into its children. */
static bool coutScanStmt(void *ctx, Stmt *s) {
    bool *hit = ctx;
    if (!s || *hit) return !*hit;
    AstVisit v = { coutScanExpr, coutScanStmt, ctx };
    return astWalkStmtChildren(s, &v) || !*hit;
}

/**
 * @brief Decide in advance whether the program writes to the console buffer.
 *
 * main's prologue registers `atexit(extc_cout_flush)` only when `needCout` is true **at that moment**,
 * and `needCout` is normally set while statements are being emitted (`println(...)`, `io::cout <<`). A
 * program that `exit(7)`s halfway or hits a trap would therefore lose the bytes in the buffer. This
 * scans the function bodies that are about to be emitted before any body is generated, turning the
 * switch on early.
 *
 * @param g  The code generation state; the scan result only writes `g->needCout`.
 */
static void scanConsoleUse(CG *g) {
    bool hit = false;
    for (size_t i = 0; i < g->funcs.len && !hit; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (!f || !f->body) continue;
        AstVisit v = { coutScanExpr, coutScanStmt, &hit };
        astWalkStmtChildren(f->body, &v);
    }
    if (hit) g->needCout = true;
}

/**
 * @brief Filter out, by reachability pruning, the structs, methods and free functions that really get
 *        emitted.
 *
 * The criteria are the `used` bits the checker set at call sites, plus a few roots: `main`, `@export`
 * and the members of trait tables. Three kinds are not emitted: a generic template itself (with `T`
 * unsubstituted it generates C that does not compile), a provisional instance whose signature still
 * mentions a type parameter, and the `next`/`value` protocol functions of a coroutine (they are inlined
 * at the call site).
 *
 * @param g  The code generation context; the filtering result is pushed into `structs` and `funcs`.
 * @param m  The current module; `structs` and `funcs` are the candidates.
 */

static void collectEmitted(CG *g, Module *m) {
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (sd->typeParams.len > 0) continue;   /* generic: generated per instance */
        /* A synthetic holder for `impl i64 { ... }` has no fields and no layout,
         * so it is no C struct; its methods are emitted by the loop below like
         * any other function. */
        if (!planBuiltinHolder(sd)) *(StructDef **)vecPush(&g->structs) = sd;
        /* Methods are functions too and share the prototype and definition table. */
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&sd->methods, j);
            if (planCoroProto(md)) continue;      /* emitted inline at the call site, not here */
            /* Reachability pruning: a method nobody calls is not emitted. The
             * checker already marks every call site with `planUsed`, so a small
             * program does not drag in the whole prelude and library. */
            if ((!planUsed(md)) && !inTraitTable(m, md)) continue;
            *(FuncDef **)vecPush(&g->funcs) = md;
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* The generic template itself is not generated: `T` is still a
         * parameter, so the result would not compile. Only the instances of a
         * template (`planTemplate(f) != NULL`) are emitted. */
        if (f->typeParams.len > 0) continue;
        /* Nor is an instance whose signature still mentions a type parameter.
         * A generic call inside a generic body builds one (`firstOr_T` with `T`
         * as the argument) so the template body can be checked at all; every
         * real call site is repointed to a concrete instance at instantiation,
         * and the provisional one has parameter and return types with no C name. */
        if (planTemplate(f) && funcSignatureMentionsParam(f)) continue;
        /* Free functions are pruned the same way; `main` is always kept, and
         * `@export` is a root too because nothing on the extC side names it. */
        if (!planUsed(f) && !cgIsMain(f) && !f->isModuleEntry && !f->isExport && !inTraitTable(m, f)) continue;
        /* The coroutine protocols (`next`/`value`) are emitted inline at their call sites and have no
         * body of their own -- emitting one here hit genBlockBody with a null body. */
        if (planCoroProto(f)) continue;
        *(FuncDef **)vecPush(&g->funcs) = f;
    }
}

/**
 * @brief Emit the method bodies of generic instances and register each piece in the
 *        unreferenced-pruning table.
 *
 * Only called methods are emitted (`planUsed`); `planEnterInst` tells the plan layer which instance is
 * being emitted, because one call site inside a generic body is shared by all instances. A method body
 * is always registered: the prototype loop has already created an entry carrying `proto`, and
 * `deadFuncBody` only fills in `off`/`len`; without the body, `markUnusedParams` would skip it, and a
 * method that ignores `self` would then fail to compile under `-Werror=unused-parameter`.
 *
 * @param g  The code generation context; `insts` holds the instances to emit and `out` receives the
 *           bodies.
 */
static void emitInstanceBodies(CG *g) {
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (inst->kind != TY_GENERIC) continue;
        /* A lambda environment laid out per instance: `emitLamVars` emits its `call` under the
         * instance's own name (`lamCtx`), which this loop does not set up. */
        if (inst->sdef && lamEnvNeedsVariants(inst->sdef)) continue;
        SubCtx sv = substEnter(g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (planCoroProto(md)) continue;   /* emitted inline at the call site */
            if (!planUsed(md)) continue;      /* called methods only */
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;   /* no other instance has used it: its element type may not have the operator this method needs */

            planEnterInst(inst);          /* a method body is shared by every type instance */
            /* Name the instance for the lambda-environment layout code: a literal whose type depends
             * on a type parameter is this instance's layout (`LamVar`). */
            const char *savedLamCtx = g->lamCtx;
            g->lamCtx = inst->name;
            size_t fb = g->out->len;
            CGACC("cg-emit", genFunc(g, md));
            g->lamCtx = savedLamCtx;
            planLeaveInst();
            /* A method named by a `dyn` table's thunk is referenced from outside
             * its own scope, and the "definitions nothing names" pass cannot see
             * that reference, so it has to be registered. */
            /* Always register the body: the prototype loop above already created this entry (with
             * `proto`), and `deadFuncBody` only fills `off`/`len` on it. An entry left without a
             * body makes `markUnusedParams` skip it, so a method that ignores `self` makes the
             * generated C fail `-Werror=unused-parameter`. */
            deadFuncBody(g, md, fb, g->out->len - fb);
            cgLine(g, "");
        }
        substLeave(g, sv);
    }
}

/**
 * @brief Emit a uniform method table for every trait the checker has seen in `dyn` form.
 *
 * The receiver is erased to `void *`, which is what lets a stored `dyn` value be called through the
 * table: the call site does not know the concrete type. One thunk adapting the implementation is
 * generated per (trait, type, method), and the table name is built from the concrete type's `cType`, so
 * each instance of a generic target gets its own table. A slot that cannot be dispatched (no receiver,
 * generic, returns `Self`) keeps its place (the order is the ABI), with type `void *` and NULL, and the
 * checker never dispatches from it.
 * The declaration goes into `vtDecls` and is emitted with the prototypes (a dispatch point needs the
 * struct in scope); the definition stays in `vtDefs` and is appended by `finishUnit` after every
 * byte-offset-rewriting pass.
 *
 * @param g     The code generation context; `vtDecls`/`vtDefs` are the output buffers.
 * @param out   The unit text; this function appends `vtDecls` to its end.
 * @param m     The current module, which provides `traits` and `impls`.
 * @param arena The allocator for assembling thunk names and declaration text.
 */
static void emitVtTables(CG *g, Buf *out, Module *m, Arena *arena) {
    /* Uniform method tables, one per trait. The receiver is erased to `void *`,
     * which is what lets a stored `dyn` value call through a table at all: its
     * concrete type is not known at the call site. Each (trait, type, method)
     * gets a thunk adapting the implementation to the uniform signature, and the
     * instance keeps the stable key `extc_vt$Trait$Type`.
     *
     * Only traits the checker saw in a `dyn` form (`usedDyn`) get a table. A
     * method that cannot be dispatched (no receiver, generic, or returning
     * `Self`) keeps its slot -- order is ABI -- but gets a `void *` type and a
     * NULL entry, which the checker refuses to dispatch through. */
    bufInit(&g->vtDefs, arena);
    bufInit(&g->vtDecls, arena);
    for (size_t ti = 0; ti < m->traits.len; ti++) {
        TraitDef *tr = *(TraitDef **)vecAt(&m->traits, ti);
        if (!planUsedDyn(tr)) continue;
        bufPrintf(&g->vtDecls, "struct extc_vt$%s_t {", tr->name);
        for (size_t k = 0; k < tr->methods.len; k++) {
            FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
            bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
            bufPrintf(&g->vtDecls, " %s (*%s)(void *",
                      unsafe ? "void *" : (want->ret ? cType(g, ttBase(want->ret)) : "void"),
                      want->name);
            for (size_t pi = 1; pi < want->params.len; pi++)
                bufPrintf(&g->vtDecls, ", %s",
                          cType(g, ttBase((*(Param **)vecAt(&want->params, pi))->type)));
            bufPuts(&g->vtDecls, ");");
        }
        bufPuts(&g->vtDecls, " };\n");
        for (size_t i = 0; i < m->impls.len; i++) {
            ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
            if (planImplTrait(im) != tr || !planImplTarget(im)) continue;
            Type *bt = ttBase(planImplTarget(im));
            /* The methods of `bt` live in **its own** method set: `Type.mholder`
             * when the type has no body of its own -- a builtin scalar
             * (`impl Codec for i64`) or a generic instance (`impl Codec for
             * slice<u8>`) -- and `sdef` otherwise. */
            StructDef *tsd = bt ? (bt->mholder ? bt->mholder : bt->sdef) : NULL;
            if (!tsd) continue;
            /* The **same** derivation the construction and dispatch sites use
             * (`cType`): the table is named after the concrete type
             * (`extc_vt$Codec$slice_u8`), not after `sdef->name`. */
            /* A generic target is spelled once per instance, so
             * `impl<T> Tag for pair<T>` needs `extc_vt$Tag$pair_i64` for every
             * used instance of `pair`; emitting the thunk inside `substEnter` is
             * also what makes the body pool name `pair_i64_tag` and keeps that
             * body alive. */
            Vec spellings;
            vecInit(&spellings, arena, sizeof(Type *));
            if (bt->kind == TY_GENERIC && bt->sdef) {
                for (size_t ii = 0; ii < g->insts.len; ii++) {
                    Type *cand = *(Type **)vecAt(&g->insts, ii);
                    if (cand->kind == TY_GENERIC && cand->sdef == bt->sdef)
                        *(Type **)vecPush(&spellings) = cand;
                }
            } else {
                *(Type **)vecPush(&spellings) = bt;
            }
            for (size_t vi = 0; vi < spellings.len; vi++) {
            Type *vb = *(Type **)vecAt(&spellings, vi);
            StructDef *vsd = (vb == bt) ? tsd : (vb->mholder ? vb->mholder : vb->sdef);
            if (!vsd) continue;
            SubCtx vsv = {0};
            if (vb != bt) vsv = substEnter(g, vb);
            const char *vtKey = cType(g, vb);
            bufPrintf(&g->vtDecls, "static const struct extc_vt$%s_t extc_vt$%s$%s;\n",
                      tr->name, tr->name, vtKey);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                FuncDef *have = NULL;
                for (size_t j = 0; j < vsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&vsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                if (!have) continue;                 /* completeness is the checker's business */
                if (!funcIsMethod(want) || (want->ret && mentionsParam(want->ret))) continue;
                bufPrintf(&g->vtDefs, "static %s extc_th$%s$%s$%s(void *self",
                          want->ret ? cType(g, ttBase(want->ret)) : "void",
                          tr->name, vtKey, want->name);
                for (size_t pi = 1; pi < want->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&want->params, pi);
                    bufPrintf(&g->vtDefs, ", %s %s", cType(g, ttBase(pp->type)), pp->name);
                }
                bufPrintf(&g->vtDefs, ") { %s%s((%s *)self", want->ret ? "return " : "",
                          cFuncName(g, have), cType(g, vb));
                for (size_t pi = 1; pi < want->params.len; pi++)
                    bufPrintf(&g->vtDefs, ", %s", (*(Param **)vecAt(&want->params, pi))->name);
                bufPuts(&g->vtDefs, "); }\n");
            }
            bufPrintf(&g->vtDefs, "static const struct extc_vt$%s_t __attribute__((unused)) "
                               "extc_vt$%s$%s = {", tr->name, tr->name, vtKey);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
                FuncDef *have = NULL;
                for (size_t j = 0; j < vsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&vsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                bufPrintf(&g->vtDefs, "%s %s", k ? "," : "",
                          (unsafe || !have) ? "NULL"
                                            : arenaPrintf(arena, "extc_th$%s$%s$%s",
                                                          tr->name, vtKey, want->name));
            }
            bufPuts(&g->vtDefs, " };\n");
            if (vb != bt) substLeave(g, vsv);
            }   /* one table per concrete spelling */
        }
    }
    bufPuts(out, bufCstr(&g->vtDecls));
}

/**
 * @brief Emit the trampoline and prototype of a `parallel::run` worker, and set `needPar`.
 *
 * The position is the point of this section: the product's order is includes -> prototypes -> runtime
 * -> function bodies, and a trampoline must both precede the declaration at the call site and follow
 * the worker's prototype, so the start of the body loop is the only correct place. The worker's
 * prototype is emitted once more by the same `genFuncProto`, and a repeated declaration is legal and
 * textually identical.
 *
 * @param g  The code generation context; every function in `funcs` for which `planIsParWorker` is true
 *           gets one trampoline.
 */
static void emitParTramps(CG *g) {
    /* A `parallel::run` trampoline must land **before any function body**: the call site must see its
     * declaration and it must see the worker's prototype. The product's order is includes -> prototypes
     * -> runtime -> function bodies, so the start of the body loop is the only correct place; the worker
     * prototype is emitted once more by the same genFuncProto, and a repeated declaration is legal and
     * textually identical. */
    {
        bool parAny = false;
        for (size_t i = 0; i < g->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
            if (!planIsParWorker(f)) continue;
            if (!parAny) {
                cgLine(g, "/* ---- parallel::run trampolines (stdlib/std/parallel.extc) ---- */");
                parAny = true;
            }
            genFuncProto(g, f);
            genParTramp(g, f);
            g->needPar = true;               /* the two pthread prototypes only go out when extc_par_run does */
        }
        if (parAny) cgLine(g, "");
    }
}

/**
 * @brief Emit the body of every non-extern function in `funcs`.
 *
 * Before emitting, `substEnterFunc` (an instance needs substitution) and `planEnterFunc` (what a call
 * site inside a generic body resolves to is decided by the current instance) run; afterwards the byte
 * range is handed to `deadFuncBody` for registration.
 *
 * @param g  The code generation context; `funcs` has been filtered by `collectEmitted` and `out`
 *           receives the text.
 */
static void emitFuncBodies(CG *g) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f->isExtern) continue;              /* an external declaration has no body */
        SubCtx sv = substEnterFunc(g, f);      /* instances need substitution */
        /* Tell the plan which instance is being emitted: a call site inside a generic
         * body is shared by every instance, and what it calls depends on this. */
        planEnterFunc(f);
        /* A generic function instance may contain a lambda whose environment depends on the
         * instance's type arguments (`LamVar`), so the body is generated under its name. */
        const char *savedLamCtx = g->lamCtx;
        g->lamCtx = planTemplate(f) ? planInstName(f) : NULL;
        size_t fb = g->out->len;
        CGACC("cg-emit", genFunc(g, f));
        g->lamCtx = savedLamCtx;
        deadFuncBody(g, f, fb, g->out->len - fb);
        planLeaveFunc();
        substLeave(g, sv);
        cgLine(g, "");
    }
    /* the dispatcher is written at the end of g->body: finishUnit concatenates the body first and prunes
     * afterwards, so the table's references to the entries still count */
    if (g->needModuleTramp) emitModuleTramp(g);
}

/**
 * @brief Splice the coroutine handle definitions, the pool runtime and the coroutine runtime into the
 *        unit text's coroutine section.
 *
 * It acts only when a handle is needed, the coroutine section exists and it is not fully spliced yet:
 * the text before `coroDefA` is taken, the handle structs, the pool runtime (when not emitted yet) and
 * the coroutine runtime are inserted in order, and the part after `coroDefA` is appended. The spliced
 * text is longer, so the whole `Buf` is reassembled; it cannot be written back in place into
 * `out->data`.
 *
 * @param g     The code generation context; `needCoroHandle`/`coroDefA`/`coroDefPrinted`/
 *              `taskTableDone` decide what is spliced, and `poolDone` and `needPool` are updated in
 *              place.
 * @param out   The unit text, replaced as a whole by the splice result.
 * @param arena The allocator the splice's temporary buffer is allocated from.
 */
static void assembleCoroutineRuntime(CG *g, Buf *out, Arena *arena) {
    if (g->needCoroHandle && g->coroDefA && (!g->coroDefPrinted || !g->taskTableDone)) {
        Buf nb;
        bufInit(&nb, arena);
        bufPutn(&nb, out->data, g->coroDefA);
        if (!g->coroDefPrinted)
            bufPuts(&nb,
                    "#define EXTC_CORO_HANDLE_DEFINED 1\n"
                    "struct ExtcCoroS { void *frame; int64_t kind; int64_t task; };\n"
                    "typedef struct ExtcCoroS extc_coro;\n\n");
        if (!g->taskTableDone) {
            if (!g->poolDone) {
                Buf pr;
                bufInit(&pr, arena);
                poolsEmitRuntime(arena, &pr);
                bufPuts(&nb, bufCstr(&pr));
                g->poolDone = true;
            }
            Buf rt;
            bufInit(&rt, arena);
            coroutineEmitRuntime(arena, &rt);
            bufPuts(&nb, bufCstr(&rt));
            g->needPool = true;
        }
        bufPutn(&nb, out->data + g->coroDefA, out->len - g->coroDefA);
        /* Rebuild through `Buf` instead of `memcpy`-ing into `out->data`: the
         * spliced text makes the unit longer, so writing it in place runs past
         * the buffer's end. */
        *out = nb;
    }
}

/**
 * @brief Append the parallel runtime text of `par::run` / `par::map` to the end of the unit on demand.
 *
 * The language has no function pointers, so the callbacks come from generated static trampolines, and
 * this text only knows plain C function pointers. Ranges are distributed by an atomic chunk counter, so
 * they never overlap; each worker gets its own arena (`extc_tls_arena`), so a region has exactly one
 * owner. With `withRegion` set a region is opened, and the next run closes the previous one. Failing to
 * create a thread is not an error: the range is finished by fewer workers.
 *
 * @param g    The code generation context; `needParRegion` decides whether `EXTC_PAR_REGION` is
 *             emitted, and `needPar` (or the `EXTC_DBG_PAR` environment variable) decides whether the
 *             whole runtime is.
 * @param out  The unit text the runtime text is appended to.
 */
static void emitParallelRuntime(CG *g, Buf *out) {
    if (g->needParRegion) bufPuts(out, "#define EXTC_PAR_REGION 1\n");
    if (g->needPar || getenv("EXTC_DBG_PAR"))
    {
        bufPuts(out,
            "extern int pthread_create(unsigned long *th, const void *attr,\n"
            "                          void *(*fn)(void *), void *arg);\n"
            "extern int pthread_join(unsigned long th, void **ret);\n"
            "\n"
            "typedef struct extc_par_job {\n"
            "    int64_t (*fn)(int64_t id, int64_t lo, int64_t hi, void *ctx);\n"
            "    void   *ctx;\n"
            "    int64_t n, chunk, next, first_err;\n"
            "    void   *region;   /* extc_par_region*, or NULL when no worker allocates */\n"
            "} extc_par_job;\n"
            "\n"
            "/* Where `new` inside a worker takes its memory from; step 3 wires the analysis\n"
            " * to it. NULL outside a worker, so the checker can say so loudly. */\n"
            "_Thread_local extc_arena *extc_tls_arena = NULL;\n"
            "\n"
            "#ifdef EXTC_PAR_REGION\n"
            "/* Region-shared arena for one parallel run: one arena per worker,\n"
            " * owned by the region -- the parent reads worker allocations after the join. */\n"
            "typedef struct extc_par_region {\n"
            "    int64_t     workers;\n"
            "    extc_arena *arenas;\n"
            "} extc_par_region;\n"
            "/* The region the last parallel run in this thread produced into: the caller reads\n"
            " * worker allocations through it after the run. Closed by the next run (or never). */\n"
            "_Thread_local extc_par_region *extc_tls_region = NULL;\n"
            "\n"
            "EXTC_UNUSED static extc_par_region *extc_par_open(int64_t workers) {\n"
            "    if (workers <= 0) workers = 1;\n"
            "    extc_par_region *r = (extc_par_region *)calloc(1, sizeof *r\n"
            "        + (size_t)workers * sizeof(extc_arena));\n"
            "    if (!r) return NULL;\n"
            "    r->workers = workers;\n"
            "    r->arenas  = (extc_arena *)(r + 1);\n"
            "    for (int64_t i = 0; i < workers; i++) extc_arena_init(&r->arenas[i]);\n"
            "    return r;\n"
            "}\n"
            "\n"
            "/* Release: hand every worker arena back. */\n"
            "EXTC_UNUSED static void extc_par_close(extc_par_region *r) {\n"
            "    if (!r) return;\n"
            "    for (int64_t i = 0; i < r->workers; i++) {\n"
            );
        /* The reclamation region lives as long as the worker arena: the TLS region shared by the whole worker
         * chain is discarded here (at the start of the next run) together with the arena -- exactly
         * consistent with the lifetime assumption "readable after the parent frame joins, destroyed
         * before the next run". Without reclamation this line does not exist. */
        if (g->needReclaim)
            bufPuts(out,
            "        extc_rc_regionDrop((void *)&r->arenas[i]);\n");
        bufPuts(out,
            "        extc_arena_destroy(&r->arenas[i]);\n"
            "    }\n"
            "    free(r);\n"
            "}\n"
            "#endif\n"
            "\n"
            "typedef struct extc_par_arg { extc_par_job *job; int64_t id; } extc_par_arg;\n"
            "\n"
            "static void *extc_par_worker(void *p) {\n"
            "    extc_par_arg *a = (extc_par_arg *)p;\n"
            "    extc_par_job *j = a->job;\n"
            "    extc_arena local;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    void *r = j->region;\n"
            "    if (r)   /* the parent reads it after the join, so it is not destroyed here */\n"
            "        extc_tls_arena = &((extc_par_region *)r)->arenas[a->id];\n"
            "    else\n"
            );
            bufPuts(out,
            "#endif\n"
            "    { extc_arena_init(&local); extc_tls_arena = &local; }\n"
            "    for (;;) {\n"
            "        int64_t k = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);\n"
            "        int64_t lo = k * j->chunk;\n"
            "        if (lo >= j->n) break;\n"
            "        int64_t hi = lo + j->chunk;\n"
            "        if (hi > j->n) hi = j->n;\n"
            );
        if (g->needReclaim)
            bufPuts(out,
            "        /* Worker entry: the only caller is the C runtime. When the job runs inline on the\n"
            "         * same thread (threads <= 1, or a thread could not be started) the outer frame's\n"
            "         * depth is still in TLS; clearing it here makes the entry frame the owner, and every\n"
            "         * region it allocates is new in this round, so no outer frame can hold a reference\n"
            "         * into it. */\n"
            "        int32_t __extc_depth = extc_rc_tlsDepth;\n"
            "        extc_rc_tlsDepth = 0;\n");
        bufPuts(out,
            "        int64_t rc = j->fn(a->id, lo, hi, j->ctx);\n"
            );
        if (g->needReclaim)
            bufPuts(out,
            "        extc_rc_tlsDepth = __extc_depth;\n");
        bufPuts(out,
            "        if (rc != 0 && __atomic_load_n(&j->first_err, __ATOMIC_RELAXED) == 0)\n"
            "            __atomic_store_n(&j->first_err, rc, __ATOMIC_RELAXED);\n"
            "    }\n"
            "    extc_tls_arena = NULL;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    if (!r) {\n"
            "#else\n"
            "    {\n"
            "#endif\n"
            );
        if (g->needReclaim)
            bufPuts(out,
            "        extc_rc_regionDrop((void *)&local);   /* the reclamation region lives as long as this arena */\n");
        bufPuts(out,
            "        extc_arena_destroy(&local);   /* a region's arenas are the parent's to free */\n"
            "    }\n"
            "    return NULL;\n"
            "}\n"
            "\n"
            "/* Run [0,n) with `threads` workers in `chunk`-sized pieces; returns the first\n"
            " * non-zero worker result (0 = all fine). threads <= 1, or a thread that cannot be\n"
            " * created, is not an error: the range is simply finished on fewer workers. */\n"
            "static int64_t extc_par_run(int64_t (*fn)(int64_t, int64_t, int64_t, void *),\n"
            "                            void *ctx, int64_t withRegion,\n"
            "                            int64_t n, int64_t threads, int64_t chunk) {\n"
            "    extc_par_job job;\n"
            "    unsigned long th[256];\n"
            "    extc_par_arg args[256];\n"
            "    int64_t started = 0, i;\n"
            "    job.fn = fn; job.ctx = ctx; job.n = n;\n"
            "    job.chunk = chunk > 0 ? chunk : (n / (threads * 8) > 0 ? n / (threads * 8) : 1);\n"
            "    job.next = 0; job.first_err = 0; job.region = NULL;\n"
            "    (void)withRegion;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    if (withRegion) {\n"
            "        /* One region per thread, refreshed per run: the previous one is closed here,\n"
            "         * which is exactly the moment the caller stops being able to read it. */\n"
            "        extc_par_close(extc_tls_region);\n"
            "        extc_tls_region = extc_par_open(threads > 0 ? threads : 1);\n"
            "        job.region = extc_tls_region;\n"
            "    }\n"
            "#endif\n"
            "    if (n <= 0) return 0;\n"
            "    if (threads > 256) threads = 256;\n"
            "    for (i = 1; i < threads; i++) {\n"
            "        args[i].job = &job; args[i].id = i;\n"
            "        if (pthread_create(&th[i], NULL, extc_par_worker, &args[i]) != 0) break;\n"
            "        started++;\n"
            "    }\n"
            "    { extc_par_arg self; self.job = &job; self.id = 0; extc_par_worker(&self); }\n"
            "    for (i = 1; i <= started; i++) { void *r = NULL; pthread_join(th[i], &r); }\n"
            "    return job.first_err;\n"
            "}\n"
            "\n"
            "/* Default worker count: when `par` names no thread count, use the cores this machine\n"
            " * makes available. `sysconf` is declared here instead of including unistd.h, because that\n"
            " * header collides with the library's own read/write declarations. */\n"
            "extern long sysconf(int);\n"
            "EXTC_UNUSED static int64_t extc_par_default_threads(void) {\n"
            "    const char *e = getenv(\"EXTC_PAR_THREADS\");\n"
            "    long t = e ? atol(e) : -1;\n"
            "    if (t <= 0) t = sysconf(84 /* _SC_NPROCESSORS_ONLN */);\n"
            "    if (t <= 0) t = 4;\n"
            "    if (t > 256) t = 256;\n"
            "    return t;\n"
            "}\n");
    }
}

/**
 * @brief Emit the dynamic-value runtime: the operations on `ExtcAny`.
 *
 * `any` is a value whose type is known only when the program runs. The invariant that makes it
 * sound is that a value has **exactly one** type at a time: rebinding replaces the old value, it
 * never stacks a second type on top. So the representation is a pair -- a descriptor and the bytes
 * -- and copy, print, compare and mark are each one walk over that descriptor.
 *
 * The bytes live inline when the value is eight bytes or less, and in a copy made in the caller's
 * arena otherwise. Which of the two is live travels in the **low bit of the descriptor pointer**
 * (`extc_any_type` clears it): descriptor objects are aligned, so the bit is free, and it keeps the
 * value at 16 bytes instead of 24. `desc == NULL` means the slot holds nothing.
 *
 * `desc->size` is *not* the size of the value -- for a byte view it is 1, the element size -- so the
 * emission site passes `sizeof(<T>)` to `extc_any_box` and that number decides the layout, while the
 * descriptor stays the thing printing and comparison walk.
 *
 * Emitted only when `g->needAny`, and after `rtPrint`/`rtEq`, because recursing into a field is
 * `extc_print`/`extc_eq` on the descriptor rather than new code here.
 *
 * @param arena     The allocator for the text.
 * @param out       The unit text, the append target.
 * @param withPrint Whether `extc_print` is in the unit; the print helper recurses through it.
 * @param withEq    Whether `==` on two `any` values is used; comparing recurses into `extc_eq`,
 *                  which is emitted with the equality runtime, so that helper is only defined when
 *                  that runtime is present.
 */
static void anyEmitRuntime(Arena *arena, Buf *out, bool withPrint, bool withEq) {
    (void)arena;
    bufPuts(out,
        "/* ---- `any`: one value whose type is known only at run time ----\n"
        " *\n"
        " * 16 bytes, the size of a `slice`: a descriptor and the bytes. Which half of `u` is live is\n"
        " * kept in the low bit of `desc` (`extc_any_type` clears it): descriptor objects are aligned,\n"
        " * so the bit is free, and it saves growing the value to 24 bytes. `desc == NULL` means the\n"
        " * slot holds nothing. The number that decides inline or boxed is the size of the value, which\n"
        " * the emission site knows and the descriptor does not (`desc->size` is the *element* size for\n"
        " * a view).\n"
        " */\n"
        "static const ExtcDesc *extc_any_type(const ExtcAny *a) {\n"
        "    return (const ExtcDesc *)((uintptr_t)a->desc & ~(uintptr_t)1);\n"
        "}\n"
        "\n"
        "static bool extc_any_boxed(const ExtcAny *a) {\n"
        "    return ((uintptr_t)a->desc & (uintptr_t)1) != 0;\n"
        "}\n"
        "\n"
        "EXTC_UNUSED static const void *extc_any_payload(const ExtcAny *a) {\n"
        "    return extc_any_boxed(a) ? (const void *)(uintptr_t)a->u : (const void *)&a->u;\n"
        "}\n"
        "\n"
        "/* Boxing copies the value in: that is what makes an `any` a value rather than a view, and\n"
        " * what lets one slot hold different types over its life. */\n"
        "/* Deepening a boxed value: a view is a pointer plus a length, so copying the view copies an\n"
        " * address, not the bytes it names. A payload that contains one is therefore copied field by\n"
        " * field with `extc_any_fixup`, which allocates fresh backing storage in the same arena. That\n"
        " * is what makes an `any` *own* its value: the box can outlive the frame the view pointed\n"
        " * into. Types without views take the memcpy path and pay nothing for this. */\n"
        "EXTC_UNUSED static void extc_any_fixup(extc_arena *arena, const ExtcDesc *d, void *value,\n"
        "                                      int depth);\n"
        "\n"
        "EXTC_UNUSED static void extc_any_fixup_elems(extc_arena *arena, const ExtcDesc *elem,\n"
        "                                            void *base, int64_t n, int depth) {\n"
        "    for (int64_t i = 0; i < n; i++)\n"
        "        extc_any_fixup(arena, elem, (char *)base + (size_t)i * elem->size, depth + 1);\n"
        "}\n"
        "\n"
        "EXTC_UNUSED static void extc_any_fixup(extc_arena *arena, const ExtcDesc *d, void *value,\n"
        "                                      int depth) {\n"
        "    if (!d || !value || depth > 32) return;   /* a cycle deeper than that is left as it is */\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *fs = (const ExtcField *)d->table;\n"
        "        for (size_t i = 0; fs && i < d->count; i++)\n"
        "            extc_any_fixup(arena, fs[i].desc, (char *)value + fs[i].off, depth + 1);\n"
        "        break;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "        extc_any_fixup_elems(arena, d->elem, value, (int64_t)d->count, depth);\n"
        "        break;\n"
        "    case EXTC_D_SLICE:\n"
        "    case EXTC_D_TEXT: {\n"
        "        /* Every view is two machine words: a data pointer then an element count. Reading\n"
        "         * them through `void **` / `int64_t *` avoids naming the view layout here. */\n"
        "        void **vw = (void **)value;\n"
        "        int64_t len = ((int64_t *)value)[1];\n"
        "        if (!vw[0] || len <= 0) break;\n"
        "        size_t stride = d->kind == EXTC_D_TEXT ? 1 : (d->elem ? d->elem->size : d->size);\n"
        "        size_t bytes = (size_t)len * stride;\n"
        "        void *copy = extc_arena_alloc(arena, (int64_t)bytes, \"any\", 0);\n"
        "        memcpy(copy, vw[0], bytes);\n"
        "        vw[0] = copy;\n"
        "        if (d->elem) extc_any_fixup_elems(arena, d->elem, copy, len, depth);\n"
        "        break;\n"
        "    }\n"
        "    case EXTC_D_REF: {\n"
        "        void **p = (void **)value;\n"
        "        if (!*p || !d->elem) break;\n"
        "        void *copy = extc_arena_alloc(arena, (int64_t)d->elem->size, \"any\", 0);\n"
        "        memcpy(copy, *p, d->elem->size);\n"
        "        *p = copy;\n"
        "        extc_any_fixup(arena, d->elem, copy, depth + 1);\n"
        "        break;\n"
        "    }\n"
        "    default:\n"
        "        break;   /* scalars, enums and the like hold no views of their own */\n"
        "    }\n"
        "}\n"
        "\n");
        bufPuts(out,
        "/* `dup` is a compile-time constant: the boxing site knows whether the static type can contain\n"
        " * a view, so the memcpy-only programs keep the fast path. */\n"
        "EXTC_UNUSED static ExtcAny extc_any_box(extc_arena *arena, const ExtcDesc *d,\n"
        "                                       const void *src, int64_t size, int64_t dup) {\n"
        "    ExtcAny v;\n"
        "    v.desc = d;\n"
        "    /* The payload word is always written, even when nothing is copied into it: readers take\n"
        "     * its address and read a whole struct through it, which the uninitialized-value analysis\n"
        "     * cannot follow. One store, and the diagnostic stays quiet. */\n"
        "    v.u = 0;\n"
        "    if (size <= 8 && !dup) {\n"
        "        memcpy(&v.u, src, (size_t)size);\n"
        "        return v;\n"
        "    }\n"
        "    void *p = size <= 8 ? (void *)&v.u : extc_arena_alloc(arena, size, \"any\", 0);\n"
        "    memcpy(p, src, (size_t)size);\n"
        "    if (dup) extc_any_fixup(arena, d, p, 0);\n"
        "    if (size <= 8) return v;\n"
        "    v.u = (uint64_t)(uintptr_t)p;\n"
        "    v.desc = (const ExtcDesc *)((uintptr_t)d | (uintptr_t)1);\n"
        "    return v;\n"
        "}\n"
        "\n"
        "/* Unboxing: the one implicit conversion in extC that can fail, so the check is the feature.\n"
        " * A mismatch traps at the line that asked for the other type; reinterpreting the bits\n"
        " * silently would be worse than stopping. */\n"
        "EXTC_UNUSED static const void *extc_any_unbox(const ExtcAny *a, const ExtcDesc *want,\n"
        "                                              const char *file, int line) {\n"
        "    if (!a->desc)\n"
        "        extc_trapMsg(file, line, \"this `any` holds no value\");\n"
        "    if (extc_any_type(a) != want) {\n"
        "        const ExtcDesc *got = extc_any_type(a);\n"
        "        fprintf(stderr, \"%s:%d: trap: this `any` holds a `%s`, not a `%s`\\n\",\n"
        "                file, line, got->name ? got->name : \"?\",\n"
        "                want->name ? want->name : \"?\");\n"
        "        extc_die(1);\n"
        "    }\n"
        "    return extc_any_payload(a);\n"
        "}\n");
    /* Printing an `any` is the descriptor printer with a run-time descriptor, so it is emitted only
     * when the program prints one: an unused `static` here would draw `-Wunused-function`, and
     * printing alone cannot be the reason `extc_print` exists. */
    if (withPrint)
        bufPuts(out,
            "\n"
            "EXTC_UNUSED static void extc_any_print(const ExtcAny *a) {\n"
            "    if (!a->desc) { extc_cout_put((uint8_t *)\"<none>\", 6); return; }\n"
            "    extc_print(extc_any_payload(a), extc_any_type(a));\n"
            "}\n");
    /* `==` compares through the same descriptor walk as printing, which is `extc_eq` -- so this half
     * waits for `rtEq` to be in the unit (the caller gates both on the same flag). */
    if (withEq)
        bufPuts(out,
            "\n"
            "EXTC_UNUSED static bool extc_any_eq(const ExtcAny *a, const ExtcAny *b) {\n"
            "    if (extc_any_type(a) != extc_any_type(b)) return false;\n"
            "    if (!a->desc) return true;\n"
            "    return extc_eq(extc_any_payload(a), extc_any_payload(b), extc_any_type(a));\n"
            "}\n");
    /* JSON from the same descriptor: an `any` is the only thing that can carry a run-time type, so
     * this half is part of the `any` runtime rather than of the printer. */
    bufPuts(out,
            "\n"
            "/* ---- JSON from a descriptor ----\n"
            " * The same walk the printer does, writing into a caller-supplied buffer instead of the\n"
            " * console. No allocation, no recursion through user code: a field whose type is a\n"
            " * struct is a nested descriptor, so the walk is depth-limited by the type graph. */\n"
            "EXTC_UNUSED static int64_t extc_json_raw(const char *s, int64_t n, uint8_t *out,\n"
            "                                        int64_t cap, int64_t o) {\n"
            "    for (int64_t i = 0; i < n; i++) { if (o < cap) out[o] = (uint8_t)s[i]; o++; }\n"
            "    return o;\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_text(const uint8_t *s, int64_t n, uint8_t *out,\n"
            "                                          int64_t cap, int64_t o) {\n"
            "    o = extc_json_raw(\"\\\"\", 1, out, cap, o);\n"
            "    for (int64_t i = 0; i < n; i++) {\n"
            "        uint8_t c = s[i];\n"
            "        if (c == '\"' || c == '\\\\') { o = extc_json_raw(\"\\\\\", 1, out, cap, o);\n"
            "            o = extc_json_raw((const char *)&c, 1, out, cap, o); }\n"
            "        else if (c == '\\n') o = extc_json_raw(\"\\\\n\", 2, out, cap, o);\n"
            "        else if (c == '\\t') o = extc_json_raw(\"\\\\t\", 2, out, cap, o);\n"
            "        else if (c < 0x20) {\n"
            "            char esc[7]; int k = snprintf(esc, sizeof esc, \"\\\\u%04x\", (unsigned)c);\n"
            "            o = extc_json_raw(esc, k, out, cap, o);\n"
            "        } else o = extc_json_raw((const char *)&c, 1, out, cap, o);\n"
            "    }\n"
            "    return extc_json_raw(\"\\\"\", 1, out, cap, o);\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_num(const ExtcDesc *d, const void *p, uint8_t *out,\n"
            "                                         int64_t cap, int64_t o) {\n"
            "    char b[64]; int n = 0;\n"
            "    switch (d->kind) {\n"
            "    case EXTC_D_I8:  n = snprintf(b, sizeof b, \"%d\", (int)*(const int8_t *)p); break;\n"
            "    case EXTC_D_I16: n = snprintf(b, sizeof b, \"%d\", (int)*(const int16_t *)p); break;\n"
            "    case EXTC_D_I32: n = snprintf(b, sizeof b, \"%d\", (int)*(const int32_t *)p); break;\n"
            "    case EXTC_D_I64: n = snprintf(b, sizeof b, \"%lld\", (long long)*(const int64_t *)p); break;\n"
            "    case EXTC_D_U8:  n = snprintf(b, sizeof b, \"%u\", (unsigned)*(const uint8_t *)p); break;\n"
            "    case EXTC_D_U16: n = snprintf(b, sizeof b, \"%u\", (unsigned)*(const uint16_t *)p); break;\n"
            "    case EXTC_D_U32: n = snprintf(b, sizeof b, \"%u\", (unsigned)*(const uint32_t *)p); break;\n"
            "    case EXTC_D_U64: n = snprintf(b, sizeof b, \"%llu\", (unsigned long long)*(const uint64_t *)p); break;\n"
            "    case EXTC_D_F32: n = snprintf(b, sizeof b, \"%.9g\", (double)*(const float *)p); break;\n"
            "    case EXTC_D_F64: n = snprintf(b, sizeof b, \"%.17g\", *(const double *)p); break;\n"
            "    case EXTC_D_BOOL: return extc_json_raw(*(const bool *)p ? \"true\" : \"false\",\n"
            "                                          *(const bool *)p ? 4 : 5, out, cap, o);\n"
            "    default: return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    }\n"
            "    return extc_json_raw(b, n, out, cap, o);\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_val(const ExtcDesc *d, const void *p, uint8_t *out,\n"
            "                                         int64_t cap, int64_t o, int depth) {\n"
            "    if (!d || depth > 32) return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    switch (d->kind) {\n"
            "    case EXTC_D_STRUCT: {\n"
            "        const ExtcField *fs = (const ExtcField *)d->table;\n"
            "        o = extc_json_raw(\"{\", 1, out, cap, o);\n"
            "        for (size_t i = 0; fs && i < d->count; i++) {\n"
            "            if (i) o = extc_json_raw(\",\", 1, out, cap, o);\n"
            "            o = extc_json_text((const uint8_t *)fs[i].name,\n"
            "                               (int64_t)strlen(fs[i].name), out, cap, o);\n"
            "            o = extc_json_raw(\":\", 1, out, cap, o);\n"
            "            o = extc_json_val(fs[i].desc, (const char *)p + fs[i].off, out, cap, o, depth + 1);\n"
            "        }\n"
            "        return extc_json_raw(\"}\", 1, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_ENUM: {\n"
            "        const char *const *vs = (const char *const *)d->table;\n"
            "        int32_t tag = *(const int32_t *)p;\n"
            "        if (vs && tag >= 0 && (size_t)tag < d->count)\n"
            "            return extc_json_text((const uint8_t *)vs[tag], (int64_t)strlen(vs[tag]),\n"
            "                                  out, cap, o);\n"
            "        return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_ARRAY:\n"
            "    case EXTC_D_SLICE:\n"
            "    case EXTC_D_TEXT: {\n"
            "        const uint8_t *data;\n"
            "        int64_t n;\n"
            "        if (d->kind == EXTC_D_ARRAY) { data = (const uint8_t *)p; n = (int64_t)d->count; }\n"
            "        else { data = *(const uint8_t *const *)p; n = ((const int64_t *)p)[1]; }\n"
            "        if (d->kind == EXTC_D_TEXT) return extc_json_text(data, n, out, cap, o);\n"
            "        size_t stride = d->kind == EXTC_D_ARRAY ? d->size : (d->elem ? d->elem->size : d->size);\n"
            "        o = extc_json_raw(\"[\", 1, out, cap, o);\n"
            "        for (int64_t i = 0; i < n; i++) {\n"
            "            if (i) o = extc_json_raw(\",\", 1, out, cap, o);\n"
            "            o = extc_json_val(d->elem, data + (size_t)i * stride, out, cap, o, depth + 1);\n"
            "        }\n"
            "        return extc_json_raw(\"]\", 1, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_REF:\n"
            "        return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    default:\n"
            "        return extc_json_num(d, p, out, cap, o);\n"
            "    }\n"
            "}\n"
            "int64_t extc_any_json(ExtcAny *a, uint8_t *out, int64_t cap) {\n"
            "    if (!a->desc) return extc_json_raw(\"null\", 4, out, cap, 0);\n"
            "    return extc_json_val(extc_any_type(a), extc_any_payload(a), out, cap, 0, 0);\n"
            "}\n"
            "\n"
            "/* The type's name, copied out: a `ref u8` cannot be walked from extC source. */\n"
            "int64_t extc_any_type_name(ExtcAny *a, uint8_t *out, int64_t cap) {\n"
            "    const ExtcDesc *d = a->desc ? extc_any_type(a) : NULL;\n"
            "    const char *n = d && d->name ? d->name : (a->desc ? \"?\" : \"<none>\");\n"
            "    return extc_json_raw(n, (int64_t)strlen(n), out, cap, 0);\n"
            "}\n");
}

/**
 * @brief Ask for the descriptors a dynamic call needs, **before** the descriptor region is emitted.
 *
 * `descRef` registers a type's descriptor row for emission, and the rows are written out before the
 * bodies; a dispatcher emitted afterwards can therefore not add one. This pass walks the recorded
 * call sites while that is still possible and registers every candidate type, plus the type each
 * candidate returns (a return value is boxed, which needs its descriptor too).
 *
 * @param g  The generator; `g->dynCalls` holds the sites.
 * @param m  The module, the source of the candidate types.
 */
static void collectDynCallDescs(CG *g, Module *m) {
    for (size_t i = 0; i < g->dynCalls.len; i++) {
        Expr *e = *(Expr **)vecAt(&g->dynCalls, i);
        const char *name = e->u.method.name;
        for (size_t k = 0; k < m->structs.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, k);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *md = *(FuncDef **)vecAt(&sd->methods, j);
                if (md->params.len != 1 || strcmp(md->name, name) != 0 || !md->body) continue;
                (void)descRef(g, sd->type);
                if (md->ret && md->ret->kind != TY_VOID) (void)descRef(g, md->ret);
                break;
            }
        }
    }
}

/**
 * @brief Emit one resolver per `any` dynamic-call site, keyed by the receiver's descriptor.
 *
 * A chain of descriptor comparisons grows with the number of candidate types (2.7 ns at 16, 25 ns at
 * 256 in the prototype), so above `chainMax` candidates the resolver searches a sorted table instead
 * and jumps on the index. The candidates are fixed at compile time but their *addresses* are not, so
 * the table is sorted once, on the first call, and every later call searches it.
 *
 * @param g    The generator; `g->dynCalls` holds the sites, in emission order.
 * @param m    The module whose struct methods are the candidates.
 * @param out  The unit text to append to.
 */
static void emitDynCalls(CG *g, Module *m, Buf *out) {
    const size_t chainMax = 8;
    for (size_t i = 0; i < g->dynCalls.len; i++) {
        Expr *e = *(Expr **)vecAt(&g->dynCalls, i);
        const char *name = e->u.method.name;
        /* Collect the candidates first: the emission shape depends on how many there are. */
        Vec cands;
        vecInit(&cands, g->arena, sizeof(StructDef *));
        for (size_t k = 0; k < m->structs.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, k);
            if (sd->typeParams.len > 0) continue;      /* an instance needs its own type argument */
            FuncDef *md = NULL;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (cand->params.len == 1 && strcmp(cand->name, name) == 0) { md = cand; break; }
            }
            if (!md || !md->body) continue;
            *(StructDef **)vecPush(&cands) = sd;
        }
        bufPrintf(out, "\n/* `v.%s()` on an `any`, resolved by the receiver's descriptor. */\n", name);
        const bool wide = cands.len > chainMax;
        if (wide) {
            bufPrintf(out, "static struct { const ExtcDesc *key; int idx; } extc_dyn_%zu_map[] = {\n", i);
            for (size_t k = 0; k < cands.len; k++) {
                StructDef *sd = *(StructDef **)vecAt(&cands, k);
                bufPrintf(out, "    { %s, %zu },\n", descRef(g, sd->type), k);
            }
            bufPuts(out, "};\n");
            bufPrintf(out, "static int extc_dyn_%zu_sorted = 0;\n", i);
            bufPrintf(out, "static void extc_dyn_%zu_sort(void) {\n", i);
            bufPrintf(out, "    size_t __n = sizeof extc_dyn_%zu_map / sizeof extc_dyn_%zu_map[0];\n", i, i);
            bufPrintf(out, "    for (size_t __i = 1; __i < __n; __i++) {\n");
            bufPrintf(out, "        const ExtcDesc *__k = extc_dyn_%zu_map[__i].key; int __v = extc_dyn_%zu_map[__i].idx;\n", i, i);
            bufPrintf(out, "        size_t __j = __i;\n");
            bufPrintf(out, "        while (__j > 0 && (uintptr_t)extc_dyn_%zu_map[__j - 1].key > (uintptr_t)__k) {\n", i);
            bufPrintf(out, "            extc_dyn_%zu_map[__j] = extc_dyn_%zu_map[__j - 1]; __j--; }\n", i, i);
            bufPrintf(out, "        extc_dyn_%zu_map[__j].key = __k; extc_dyn_%zu_map[__j].idx = __v;\n", i, i);
            bufPuts(out, "    }\n");
            bufPrintf(out, "    extc_dyn_%zu_sorted = 1;\n", i);
            bufPuts(out, "}\n");
        }
        bufPrintf(out, "static ExtcAny extc_dyn_%zu(extc_arena *__extc_home,"
                       " const ExtcAny *__recv) {\n", i);
        bufPuts(out, "    const ExtcDesc *__t = extc_any_type(__recv);\n");
        if (wide) {
            bufPrintf(out, "    if (!extc_dyn_%zu_sorted) extc_dyn_%zu_sort();\n", i, i);
            bufPrintf(out, "    size_t __n = sizeof extc_dyn_%zu_map / sizeof extc_dyn_%zu_map[0];\n", i, i);
            bufPuts(out, "    size_t __lo = 0, __hi = __n;\n");
            bufPuts(out, "    while (__lo < __hi) {\n");
            bufPuts(out, "        size_t __mid = __lo + (__hi - __lo) / 2;\n");
            bufPrintf(out, "        if ((uintptr_t)extc_dyn_%zu_map[__mid].key < (uintptr_t)__t) __lo = __mid + 1;\n", i);
            bufPuts(out, "        else __hi = __mid;\n");
            bufPuts(out, "    }\n");
            bufPrintf(out, "    if (__lo < __n && extc_dyn_%zu_map[__lo].key == __t) {\n", i);
            bufPrintf(out, "        switch (extc_dyn_%zu_map[__lo].idx) {\n", i);
        }
        for (size_t k = 0; k < cands.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&cands, k);
            FuncDef *md = NULL;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (cand->params.len == 1 && strcmp(cand->name, name) == 0) { md = cand; break; }
            }
            const char *ct = cType(g, sd->type);
            /* The branch is picked by the *receiver's* descriptor: its type has to be described
             * (it is a struct, and printing or comparison may never ask for it otherwise). */
            if (wide) bufPrintf(out, "        case %zu: {\n", k);
            else      bufPrintf(out, "    if (__t == %s) {\n", descRef(g, sd->type));
            /* The payload is copied into a properly typed local instead of being cast: the box
             * stores either bytes or a pointer, and reading it back through a cast is the kind of
             * type punning that makes an optimizer report `-Wmaybe-uninitialized` on the callers.
             * One copy of at most a few words, and the dispatch stays in the same nanosecond. */
            bufPrintf(out, "            %s __self;\n", ct);
            bufPrintf(out, "            memcpy(&__self, extc_any_payload(__recv), sizeof __self);\n");
            if (md->ret && md->ret->kind != TY_VOID) {
                const char *rct = cType(g, md->ret);
                bufPrintf(out, "            %s __r = %s(&__self);\n", rct, cFuncName(g, md));
                bufPrintf(out, "            return extc_any_box(__extc_home, %s,"
                               " (const void *)&__r, (int64_t)sizeof(__r), %d);\n",
                          descRef(g, md->ret), typeNeedsAnyDup(g, md->ret) ? 1 : 0);
            } else {
                bufPrintf(out, "            %s(&__self);\n", cFuncName(g, md));
                bufPuts(out, "            ExtcAny __empty;\n");
                bufPuts(out, "            __empty.desc = NULL;\n");
                bufPuts(out, "            __empty.u = 0;\n");
                bufPuts(out, "            return __empty;\n");
            }
            bufPuts(out, wide ? "        }\n" : "    }\n");
        }
        if (wide) bufPuts(out, "        default: break;\n        }\n    }\n");
        bufPrintf(out, "    fprintf(stderr, \"trap: no `%s` for a `%%s`\\n\","
                       " __t ? __t->name : \"?\");\n", name);
        bufPuts(out, "    extc_die(1);\n");
        bufPuts(out, "    ExtcAny __none;\n");
        bufPuts(out, "    __none.desc = NULL;\n");
        bufPuts(out, "    __none.u = 0;\n");
        bufPuts(out, "    return __none;\n");
        bufPuts(out, "}\n");
    }
}

/**
 * @brief Append the on-demand runtime blocks, the helpers and the view primitives to the unit text, and
 *        register the prunable entries.
 *
 * The splice order is C's definition-before-use: descriptor table, printing/equality runtime, console,
 * pools, file layer, DNS, memfind, plate view/copy, time, events, domains, dyn runtime, raw terminal,
 * then the individual helpers and the view indexers/copiers. The demand for helpers and view primitives
 * is conservative, so they are registered in `deadDefs` like the descriptors and `dropUnreferenced`
 * reclaims the parts nobody names.
 *
 * @param g     The code generation context; the `need*` flags and `helpers`/`unwraps`/`viewIdx`/
 *              `viewCpy`/`viewCpyU`/`descs` decide what is emitted.
 * @param out   The unit text, the append target.
 * @param m     The current module; domain adapters are generated for the `ext` targets in `funcs`.
 * @param arena The allocator the temporary buffers and registration texts for the view primitives come
 *              from.
 */
static void emitLateRuntime(CG *g, Buf *out, Module *m, Arena *arena) {
    /* The table is what the descriptors `emitDescRegion` may have written are made of, so it
     * goes out whenever anything could name it: a print, a structural `==`, or a descriptor row
     * that exists at all (`descs`). `rtPrint` goes out only where `extc_print` is actually
     * called -- that separation keeps the larger block out of a product that never prints. */
    if (g->needRuntime || g->eqNeed.len || g->descs.len) bufPuts(out, bufCstr(&g->rt));
    /* `rtCout` goes first: `extc_print` inside `rtPrint` writes the same buffer through `extc_printf`, so
     * it must see the declarations of `extc_cout_put` / `extc_printf` first. */
    if (g->needCout || g->needPrint) bufPuts(out, bufCstr(&g->rtCout));
    if (g->needPrint)                                  bufPuts(out, bufCstr(&g->rtPrint));
    if (g->needCoutF64) bufPuts(out, bufCstr(&g->rtCoutF64));
    if (g->needPool && !g->poolDone) poolsEmitRuntime(arena, out);
    if (g->needHttpFile) fileLayerEmitRuntime(arena, out);
    if (g->needDns) dnsEmitRuntime(arena, out);
    /* The byte-search runtime, for a program that declares `extern!("extc-mem")`. Its trigger
     * also decided the `_GNU_SOURCE` preamble above; here only the bodies are appended. */
    if (g->needMemFind) memfindEmitRuntime(arena, out);
    if (g->needPlateView) plateEmitViewOf(arena, out);
    if (g->needPlateCopy) plateEmitMemCopy(arena, out);
    if (g->needTime) timeEmitRuntime(arena, out);
    if (g->needEvent) eventEmitRuntime(arena, out);
    if (g->needDomain) {
        /* The typed adapters the task table needs. They have to be declared **here**, in the
         * preamble: a call site sits inside a function body, while the coroutine section (where
         * `$step`/`$next` are defined) is appended last. `EXTC_UNUSED` because only the coroutines
         * some `ext` actually starts get one. */
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
            if (!planIsExtTarget(cf) || !planIsCoro(cf)) continue;
            const char *cn = cFuncName(g, cf);
            bufPrintf(out, "typedef struct %s$frame %s$frame;\n", cn, cn);
            bufPrintf(out, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);\n", cn, cn);
            if (planCoroNeedsZone(cf))
                bufPrintf(out, "EXTC_UNUSED static bool %s$next(struct %s$frame *f);\n", cn, cn);
            bufPrintf(out, "EXTC_UNUSED static bool %s$domstep(void *p)"
                           " { return %s$%s((struct %s$frame *)p); }\n",
                      cn, cn, planCoroNeedsZone(cf) ? "next" : "step", cn);
        }
        domainEmitRuntime(arena, out);
    }
    /* The dyn half is separate so a pool-only program keeps byte-identical generated C. */
    if (planUsesDyn(m)) poolsEmitDynRuntime(arena, out);
    if (g->needRawTerm) {
        /* The raw-terminal block. `tcsetattr` is declared with the same prototype the
         * library declares for it, so the two declarations agree and the call reaches
         * libc; the layout of `struct termios` is never named. */
        bufPuts(out, bufCstr(&g->rtRaw));
    }
    if (g->eqNeed.len || g->needAnyEq)  bufPuts(out, bufCstr(&g->rtEq));
    if (g->needAny)                     anyEmitRuntime(arena, out, g->needPrint, g->needAnyEq);
    bufPuts(out, bufCstr(&g->desc));
    bufPuts(out, bufCstr(&g->rcDesc));   /* the rc type descriptors: they must come before the function bodies too */
    for (size_t i = 0; i < g->helpers.len; i++) {
        /* A slice helper is emitted on demand, and the demand is conservative: a
         * helper whose only user is unused is still emitted. It is registered
         * here, and dropUnreferenced takes it back out when the finished unit
         * does not name it. */
        SliceHelper *h = (SliceHelper *)vecAt(&g->helpers, i);
        DeadDef *d = arenaAllocZero(arena, sizeof *d);
        d->name = h->name;
        d->text = h->text;
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        bufPuts(out, h->text);
    }
    /* The `!` unwrap helpers, registered in the order they were first needed. Same
     * placement rule as the slice helpers: a definition has to precede its uses, and the
     * need is discovered while the bodies are generated. */
    for (size_t i = 0; i < g->unwraps.len; i++) {
        UnwrapHelper *h = (UnwrapHelper *)vecAt(&g->unwraps, i);
        DeadDef *d = arenaAllocZero(arena, sizeof *d);
        d->name = h->name;
        d->text = h->text;
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        bufPuts(out, h->text);
    }
    /* The view index primitives, in the order they were first needed. They go
     * here, before the bodies, for the same reason the slice helpers do: a
     * definition has to precede its uses, and the need for one is discovered
     * while the bodies are generated. */
    for (size_t i = 0; i < g->viewIdx.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewIdx, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewIndexer(g, inst);
        substLeave(g, sv);
        g->out = save;
        /* Registered like a descriptor: an index primitive is emitted on demand, and the
         * demand is conservative - a view whose subscripts all sit in dead code still gets one.
         * dropUnreferenced takes it back out when nothing names it. */
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *idd = arenaAllocZero(arena, sizeof *idd);
        idd->name = arenaPrintf(arena, "%s_index", inst->name);
        idd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = idd;
    }
    for (size_t i = 0; i < g->viewCpy.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewCpy, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewCopier(g, inst, false);
        substLeave(g, sv);
        g->out = save;
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *cdd = arenaAllocZero(arena, sizeof *cdd);
        cdd->name = arenaPrintf(arena, "%s_copy", inst->name);
        cdd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = cdd;
    }
    /* The `@unchecked` copiers: same shape, no count/storage check. They get their
     * own names so a checked call site of the same view type keeps the checked one. */
    for (size_t i = 0; i < g->viewCpyU.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewCpyU, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewCopier(g, inst, true);
        substLeave(g, sv);
        g->out = save;
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *cdd = arenaAllocZero(arena, sizeof *cdd);
        cdd->name = arenaPrintf(arena, "%s_copyU", inst->name);
        cdd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = cdd;
    }
}

/**
 * @brief Finish the whole unit: attach the function bodies, repeatedly prune unnamed definitions, then
 *        add the coroutine and method-table definitions.
 *
 * Unreferenced pruning runs until the text length stops changing: dropping one definition can leave the
 * next one unnamed as well. The runtime primitive blocks (`primSplice`) are spliced in only after
 * pruning, because that question can only be asked of the final text. `coroDefs` and `vtDefs` are
 * appended last: they must come after every byte-offset-rewriting pass, and a frame type must precede
 * the `$step` that uses it. `g->bodyOff` records where the function bodies start.
 *
 * @param g     The code generation context; `body`/`deadFuncs`/`mainOff`/`mainLen` are consumed here.
 * @param out   The unit text the bodies and the tail definitions are appended to.
 * @param arena The allocator the temporary buffers for slicing definition text come from.
 */
static void finishUnit(CG *g, Buf *out, Arena *arena) {
    g->bodyOff = out->len;                    /* where the bodies start in the finished unit */
    bufPuts(out, bufCstr(&g->body));
    /* The body buffer is complete and contiguous now, so the texts the unreferenced-definition
     * passes work on come from it: a slice cut during generation would not survive the view-index
     * helpers, which write into a different buffer for a moment. */
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body && df->len) {
            Buf b;
            bufInit(&b, arena);
            bufPutn(&b, g->body.data + df->off, df->len);
            df->body = bufCstr(&b);
        }
    }
    if (g->mainLen) {
        Buf b;
        bufInit(&b, arena);
        bufPutn(&b, g->body.data + g->mainOff, g->mainLen);
        g->mainBody = bufCstr(&b);
    }

    /* The unit is complete: the definitions nobody names can go, repeatedly
     * until the text stops shrinking. One pass is not enough, because dropping
     * one definition makes the next one unnamed. */
    for (int round = 0; round < 8; round++) {
        size_t before = out->len;
        CGACC("cg-prune", dropUnreferenced(g, out));
        cgReport();
        if (out->len == before) break;
    }
    /* The runtime primitive block is decided here from the registry, and only
     * then becomes part of the unit: this question is asked once, of the settled
     * text. Nothing before `primA` is ever removed, so that offset is still
     * exact. */
    primSplice(g, out);
    /* A function nothing calls says so; then the parameters a body never reads. */
    markUncalledFunctions(g, out);
    markUnusedParams(g, out);

    /* The method-table definitions come last so no pass that rewrites the unit
     * by byte offset sees them; the declarations emitted earlier are what lets a
     * dispatch site in a body refer to them. Coroutine frames and step functions
     * go before them, so a frame type is defined above the step function using
     * it. */
    bufPuts(out, bufCstr(&g->coroDefs));
    bufPuts(out, bufCstr(&g->vtDefs));
}

/**
 * @brief Generate one complete C translation unit for a checked module.
 *
 * The order is fixed: initialize the state -> scan which runtime is needed -> type definitions ->
 * globals -> prototypes -> function bodies (into a temporary buffer) ->
 * coroutines/descriptors/runtime spliced on demand -> unreferenced pruning -> tail definitions. Two
 * naming checks run on entry: a generic instance name must not collide with a user-declared struct, and
 * an `@export` name must carry no hidden parameter, must not be a C keyword, and no two exports may
 * share a name. Any failure reports an error and returns false.
 *
 * @param ctx     The compilation context; errors are reported through it and `hasError` decides the
 *                return value.
 * @param arena   The allocator both the CG state and the product buffer are allocated from.
 * @param tt      The type table, which provides instances and function types.
 * @param m       The current module.
 * @param lineMap When true, the line-number mapping is kept in the product.
 * @param out     The product buffer; the complete C text is written here.
 * @return        true when nothing was reported; false after reporting an error for a naming collision
 *                or a broken export contract.
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out) {
    /* A generated instance's C name is `<template>_<argument>...` (`pair` + `i64`
     * = `pair_i64`). A program may define that very name, and the unit would then
     * hold two definitions of `struct pair_i64`. The compiler cannot rename one
     * side, so it says which name it needs. Checked here rather than in the
     * checker: the last instantiation wave happens after the checker's passes. */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (!inst || inst->kind != TY_GENERIC || !inst->sdef || !inst->name) continue;
        for (size_t j = 0; j < m->structs.len; j++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, j);
            /* Skip the holders the compiler builds for generic instances itself:
             * they carry the very name being checked (`pair<i64>`'s method holder
             * is `pair_i64`) and are not user declarations. */
            if (sd == inst->sdef || !sd->name) continue;
            /* A compiler-built holder points at the **instance** as its type; a
             * user declaration points at itself. */
            if (sd->type && sd->type->sdef != sd) continue;
            if (strcmp(sd->name, inst->name) != 0) continue;
            ctxError(ctx, sd->line ? sd->line : 1, 1,
                     "The compiler names an instance of a generic type `<type>_<argument>`;"
                     " give this declaration another name.",
                     "`%s` is the name the compiler needs for an instance of `%s`, and the"
                     " program already defines it", inst->name, inst->sdef->name);
            return false;
        }
    }
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.tt = tt;

    /* `@export` promises the C side a symbol. Three things can break that promise, and all three are
     * decided here because this is where the answers are final: the hidden parameters (a transitive
     * closure the checker settles in its last pass), the name (this file's business), and uniqueness
     * among exports. A fourth -- that the signature is one C can write -- is the checker's, next to
     * `extern!`'s, so that the two halves of the boundary keep using one predicate (`ttCrossesC`). */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExport) continue;
        const char *en = exportName(f);
        if (funcTakesHomeArena(f) || funcTakesHomeZone(f))
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "A function that takes a home arena or zone is compiled with one hidden"
                     " parameter more than its source signature, so C's call would not match. Let it"
                     " allocate in its own block arenas instead, or export a wrapper that does.",
                     "`@export` on `%s`, which takes a hidden arena parameter", en);
        if (cSymName(&g, en) != en)
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "An exported symbol is a contract with the C side: the name has to be the one C"
                     " will look up, so this compiler will not rename it. Pick another name.",
                     "`@export` on `%s`: that name is a C keyword, so the symbol would have to be"
                     " renamed", en);
        for (size_t j = 0; j < i; j++) {
            FuncDef *o = *(FuncDef **)vecAt(&m->funcs, j);
            if (!o || !o->isExport) continue;
            if (strcmp(exportName(o), en) != 0) continue;
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "Two exported functions with one C name would be one symbol with two bodies."
                     " The module prefix is stripped from an exported name, so the two declarations"
                     " have to differ in the name they write.",
                     "`@export` name `%s` is already taken by another exported function", en);
        }
    }

    initCGState(&g, m, tt, arena);
    /* the entry table must be in place before emitProtos: cgParamList decides an entry's C parameters
     * from needModuleTramp */
    scanModuleEntries(&g, m);
    scanUnitNeeds(&g, m);
    initRuntimeText(&g, arena);
    collectEmitted(&g, m);
    /* Lambda environments whose layout depends on a type parameter get one layout per instance of
     * the body they are written in; the one-layout forms must then not be emitted at all. */
    collectLamVariants(&g);
    dropLamTemplates(&g);
    collectLamThunks(&g);
    /* Settle "this program writes the console" first: main's prologue registers
     * `atexit(extc_cout_flush)` from it, and without it a program that exits or traps halfway loses the
     * bytes in the buffer. */
    scanConsoleUse(&g);
    scanUnitReclaim(&g, m);
    /* The event tier (the automatic half of `EXTC_RC_SWEEPFREE`): the whole unit is proved "acyclic and
     * references live only in counted slots" => every unreachable object is eventually freed on the spot
     * when its count reaches zero => sweeping has nothing to do. `EXTC_RC_SWEEPFREE=1` still overrides
     * it (the criteria's red proof uses that). */
    if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) {
        g.rcSweepFree = 1;
        g.rcNoCount = 0;
    }
    emitPreamble(&g, out, m);
    emitRuntimePrologue(&g, out, m);
    buildRuntimeText(&g);
    emitTypes(&g, m, tt, arena);
    /* ------------------------------------------------------------------
     * The descriptor table is not emitted here. It goes between the prototypes
     * and the function bodies, because which descriptors are needed is known
     * only from the bodies, through what `genPrint` prints; see emitDescRegion.
     * ------------------------------------------------------------------ */

    emitGlobals(&g, m);
    emitProtos(&g, m, arena);
    /* ================= the body pool: definitions from here on ========== */

    /* Bodies are written into a temporary buffer first, because a slice helper is
     * discovered to be needed only while generating, and C wants definitions
     * before uses. Everything is spliced together at the end as prototypes, then
     * helpers, then bodies. */
    g.out = &g.body;

    /* Coroutine frames and step prototypes, before any body that may spawn one or drive it. */
    genCoroDecls(&g, m);

    /* The index primitives of the views are emitted on demand, at the first
     * subscript that needs one (`viewIndexer`). Printing and comparison derive no
     * function at all, see the descriptor table. */

    emitInstanceBodies(&g);
    emitVtTables(&g, out, m, arena);
    emitParTramps(&g);
    emitFuncBodies(&g);
    emitLamThunks(&g);
    emitLamVars(&g);
    emitGlobalDrops(&g);
    dbgRcCostReport(&g);
    /* Final assembly: the pieces whose need is discovered during generation are
     * spliced in ahead of the bodies - the print runtime and the descriptor
     * table, emitted on demand according to what genPrint printed, and the slice
     * helpers. The order is prototypes, then descriptors, then bodies, which is
     * what the define-before-use rule of C requires. */
    g.out = out;
    assembleCoroutineRuntime(&g, out, arena);
    /* Descriptors a dispatcher names have to be registered while rows are still being collected. */
    collectDynCallDescs(&g, m);
    emitDescRegion(&g);
    if (g.needCoroHandle && !g.coroDeclsDone) genCoroHandleDecls(&g, m);
    /* Descriptor types and shared scalar descriptors: needed by printing or by
     * comparison, whichever comes first. */
    /* Parallel execution over a range: the runtime half of `par::run` / `par::map`.
     *
     * The language has no function pointers, so the callback comes from a generated
     * static trampoline; this block only knows a plain C function pointer. It is
     * emitted on demand (`needPar`, or `EXTC_DBG_PAR=1` while it is being built).
     *
     * What the caller and this runtime keep between them:
     *   - ranges are disjoint by construction: one atomic chunk counter hands out
     *     [lo,hi) and a worker only writes what it was handed;
     *   - every worker gets its own arena (`extc_tls_arena`), released whole when the
     *     worker ends, so two threads never touch the same block;
     *   - nothing here is shared mutable state, so the block is re-entrant.
     *
     * POSIX only, and deliberately without `pthread.h`: generated C carries only the
     * few headers it wants, and these two prototypes match the glibc ABI
     * (`pthread_t` is `unsigned long` there, and the attribute may be NULL). */
    /* One source text, two configurations: without the define the region half is not
     * compiled at all. */
    emitParallelRuntime(&g, out);
    emitLateRuntime(&g, out, m, arena);
    /* Last, because a dispatcher boxes its result: the `any` runtime has to be in the unit already. */
    emitDynCalls(&g, m, out);
    finishUnit(&g, out, arena);
    return !ctx->hasError;
}
