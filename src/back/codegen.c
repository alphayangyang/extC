/* extC -> C code generation.
 *
 * This pass does no type inference of its own: the type checker has already
 * written its results back into the AST (Expr.type, Expr.func, Expr.field,
 * Stmt.type), so all that is left here is translation.
 *
 * The generated C is deliberately dull C: no macro tricks, no optimization
 * stunts. `#line` maps gcc diagnostics back to line numbers in the .extc file.
 */

#include "codegen.h"
#include "plan.h"          /* 计划/分析产物的唯一读口（X1）*/
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
#include "typelayer.h"   /* the read-only type questions (R3: no checker header here) */
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
 * not at all. Two earlier attempts failed for exactly those two reasons: removing the
 * declaration and the definition independently left definitions whose calls became
 * implicit declarations (28 new warnings), and an offset pair keyed by `FuncDef*`
 * framed something that was not one whole function (a cut body produced
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
     * Named `namedByDynTable`, not `dynTable`: `FuncDef` has a `dynTable` of its own (now in the plan
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
    bool           needHttpFile;      /* 文件层 shim（open/fstat/sendfile/HTTP 日期）被声明了 */
    bool           needDns;           /* `extc_dns_lookup` 被声明了（同步 getaddrinfo）*/
    bool           needDomain;       /* the program starts a task (`ext` inside a domain block) */
    bool           coroDefPrinted;
    bool           coroDeclsDone;
    bool           taskTableDone;
    bool           poolDone;
    size_t         coroDefA;  /* some coroutine is stored as a handle ⇒ emit the handle type */
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
    /* BG-1（2026-10-05）：`rt`（描述符类型 + 共享标量描述符）与 `rtPrint`（`extc_print`）曾是
     * **同一个** `needRuntime`，于是一个只需要描述符表的程序也会把 `extc_print` 发出去，
     * 而它没人调用 ⇒ `-Werror=unused-function`（clang 另报 `-Wunneeded-internal-declaration`）。
     * 两个需求不同：`needPrint` 只在**真的调用 `extc_print`** 的地方置位（结构化打印、`?` 在 main
     * 里失败时印载荷），`needRuntime` 表示"需要那张表"。 */
    bool        needRuntime;
    bool        needPrint;      /* `extc_print` 被调用过 ⇒ 才发 rtPrint */
    bool        needPar;      /* par::run/par::map 用到了线程运行期 */
    bool        needParRegion; /* 有 worker 会 new ⇒ 区域共享 arena 那半套要参与编译 */
    bool        parTls;     /* 当前函数在 parallel::run 的调用链上 ⇒ new 走 extc_tls_arena */
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
    /* 板那一层的按需发射：**一个声明一个标志**（声明了哪个才发哪个）。 */
    bool        needPlateView;    /* `extc_viewOf`：板把保留区当视图用的唯一入口 */
    bool        needPlateCopy;    /* `extc_memCopy`：板 copyIn/copyOut 的整块拷贝 */
    bool        needTime;     /* 程序用了 `extern!("extc-time")` ⇒ 发射时钟与 sleep */
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
    /* 区域回收运行期（位图 slab + 持有者计数 + 区域清扫）打开：运行期文本进序言、`new` 的
     * 存储从由 arena 地址认领的 rc 区域里取、引用赋值点发射计数。
     *
     * **默认打开**，但只在程序里真有回收管辖的分配点时才发（`scanUnitReclaim`：`new T` /
     * `alloc<T>(n)` / `@overwrite` 的单值格子）。这样没有这类分配点的程序（400 多份 golden）
     * 生成 C 一个字节都不变，而有的那些拿到全部四层。
     *
     * `EXTC_RECLAIM=0` 关掉（对照与排障）；`EXTC_RECLAIM=1` **强制**发射，不看分配点
     * —— 那些按开关口径写的门禁（`tests/reclaim-shapes/run-switch.sh`、`tests/reclaim-perf/`）
     * 要的就是"开关打开"这一档，与分配点在不在无关。 */
    bool        needReclaim;
    /* `EXTC_RECLAIM` 给了非零值时置位：跳过 `scanUnitReclaim` 的分配点判据，无条件发射。 */
    bool        rcForced;
    /* `EXTC_RC_SHARDS=N`：区域回收的**分片数**（停顿旋钮），默认 1。
     *
     * 分片 = 把一个逻辑区域（认领键是 arena 的地址）切成 N 个**运行期区域**：每片一支
     * slab 链、一张跨区登记位图（`rsbit`，即原型里每片的入边集 `SRS`）、一个标记栈，因此
     * 可以**单独清扫** —— 停顿 ∝ 被收那一片的可达集，与逻辑区域总量无关。
     *
     * 片号按**分配点**定（`rcSite++ % rcNShard`），不按存活集/对象数：同一处 `new` 造出来的
     * 东西（链、环、一代树节点）始终落在同一片，片内链条与环保持完整。按存活集均分会把
     * 链和环拆到两片，跨片登记位一置，环垃圾就永远当种子 ⇒ 界随轮数涨（原型实测吞吐也变差）。
     *
     * N == 1 时一个逻辑区域仍然只有一个运行期区域：下面每一处发射都走原来的那条分支，
     * 生成的 C 与接线前**逐字节相同**。N > 1 只是多认领几个区域，运行期文本一个字都不用改
     * —— 跨区登记、来源侧撤销、按区域清扫都是已经有的机制，片就是区域。 */
    int         rcNShard;
    /* 分配点序号：片号 = `rcSite++ % rcNShard`。每发射一个 rc 分配表达式加一，所以同一处
     * `new` 在整份生成物里始终是同一个片号（编译两次结果相同）。 */
    int         rcSite;
    /* 区域回收的编译期状态（只在 `needReclaim` 打开时有意义）。
     *
     * `rcRoots` 是**静态根表**：当前作用域里活着的引用槽的 C 表达式（`saved`、`c.a`、
     * `arr[i].next`），声明时入栈、离开块时截回去（`rcRootMarks` 是每层的截回点）。
     * `rcGlobalRoots` 是全局引用槽，每个触发点都算根。触发点把两者拼成一张根表交给
     * `extc_rc_maybeCollect`。
     *
     * `rcRootsUnreliable`：这个函数里有静态枚举不出来的引用存储（枚举负载、泛型枚举、
     * 协程帧……）。**根表不完整就一个触发都不发** —— 宁可这一轮不清扫，不可漏根误释放。 */
    Vec         rcRoots;         /* const char *：存活的引用槽表达式 */
    Vec         rcRootMarks;     /* size_t：每层块进入时的 rcRoots.len */
    Vec         rcGlobalRoots;   /* const char *：全局引用槽 */
    bool        rcGlobalsUnreliable; /* 全局量里有枚举不出来的引用存储 ⇒ 所有函数关掉触发 */
    bool        rcRootsUnreliable;
    int         rcSeq;           /* 描述符与触发的唯一编号 */
    int         rcGuardSeq;      /* 入口守卫临时变量的唯一编号 */
    /* 正在发射的函数是不是「一个地方」（入口文件的函数才是；库函数透明）。 */
    bool        zoneHere;
    /* 这个函数收不收隐藏的「家 zone」参数（= `FuncDef.makesPool`）。
     * 收了就把它往下传，不收（main、或者压根不建池的函数）就用当前那个地方的 zone。 */
    bool        funcHasZoneParam;
    /* 这个函数的帧 zone（`__extc_zm1`）发了没有。
     * 不能用 `zoneMark[1]` 问：**函数体自己**就是一个 blkLevel=1 的块，`genBlock` 会在
     * 体外把 `zoneMark[1]` 存起来、清掉，出块才恢复 ⇒ 体**内**永远读到 0
     * （而 `zoneMark[k]`（k>1）在那一层的块体内是读得到的，所以只有第 1 层有这个问题）。 */
    bool        zoneFrameMarked;
    /* Which block levels really emitted `extc_pool_zoneEnter()` (index = block level,
     * 1 = the function body). The pop has to consult this table instead of `zoneHere`
     * alone: a block's zone is emitted on demand now, and a `zoneLeaveTo(__extc_zm<lvl>)`
     * for a level that never pushed would pop the *enclosing* zone, leaving pools created
     * afterwards with no zone at all (see `zoneAtLevel` / `cgReleaseLevel`). */
    bool       *zoneMark;
    /* Definitions whose name may never be used again (`DeadDef*`, in emission
     * order); dropUnreferenced decides after the whole unit is assembled. */
    Vec         deadDefs;
    Vec         deadFuncs;      /* DeadFunc*: functions that may have no caller */
    Vec         deadLocals;     /* DeadLocal*: local declarations that may be unread */
    const char *curFuncName;    /* C name of the function being generated */
    /* `main` is not a candidate for function pruning, so it has no DeadFunc entry - but its
     * locals still need a stage to be counted on, and that is its body text. */
    const char *mainFuncName;
    const char *mainBody;
    size_t      mainOff, mainLen;
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
 * …), (x).u.some._0)` names `x` twice, so an impure operand (`f()!`) would call `f` twice.
 * A temporary in the **statement prefix** has no place to live in a `while` condition (the
 * checker rejects it: "cannot be evaluated ahead of time"). Passing the option as a function
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
 * @brief 按 printf 格式渲染一行，返回 arena 中的新字符串，行长不限。
 *
 * 先量长度（`vsnprintf(NULL, 0, ...)`）再分配，因此没有行长上限；固定大小的缓冲区
 * 不会响亮地截断，超长行会静默产出无效 C 而后仍然退出 0。
 *
 * @param g   generator 状态；字符串分配在 g->arena
 * @param fmt printf 风格格式串
 * @param ap  参数列表；本函数会消耗它（内部先 va_copy 一份用于第二次 vsnprintf）
 * @return    NUL 结尾的 arena 字符串，随 arena 存活
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
 * @brief 按当前缩进把一行格式化文本追加到生成输出。
 *
 * @param g   generator 状态；提供输出缓冲、缩进与 arena
 * @param fmt printf 风格格式串
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
 * @brief 按当前缩进把一行格式化文本追加到待发语句前缀缓冲。
 *
 * 文本先进缓冲而不直接写输出：前缀必须出现在需要它的语句之前，见 flushPrefix。
 *
 * @param g   generator 状态；提供前缀缓冲、缩进与 arena
 * @param fmt printf 风格格式串
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
 * @brief 写出缓冲着的语句前缀并清空它。
 *
 * 必须在它所属语句的第一行之前调用：前缀里的行算的正是该语句随后要读的临时量，
 * 这个先后顺序就是前缀存在的理由。
 *
 * @param g generator 状态
 */
static void flushPrefix(CG *g) {
    if (g->prefix.len == 0) return;
    bufPuts(g->out, bufCstr(&g->prefix));
    bufInit(&g->prefix, g->arena);
}

/**
 * @brief 用当前实例化上下文替换类型中的 TY_PARAM。
 *
 * @param g generator 状态；上下文取自 g->substParams 与 g->substArgs
 * @param t 待替换的类型
 * @return  替换后的类型；没有生效的实例化上下文时返回 t 本身
 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
}

/**
 * @brief 替换上下文的全部三个字段：类型形参、类型实参、方法名的属主前缀。
 *
 * `substEnter` 把进入之前的这份上下文交出来，`substLeave` 原样收回去。**进出必须成对且
 * 带上归还的那一份**：早期版本里 `substLeave` 无条件把三个字段清空，于是"探测一个类型"
 * 的嵌套进入（发射 `ExtcRcType` 描述符、收集根表叶子……都在泛型方法体里跑）会顺手抹掉
 * 外层实例的上下文，同一具方法体余下的部分就以 `T` 未替换的形态发出（`slice_T`、
 * `varArray_T`），生成的 C 根本编不过。
 */
typedef struct {
    Vec        *params;
    Vec        *args;
    const char *owner;
} SubCtx;

/**
 * @brief 进入一个泛型实例的单态化替换上下文。
 *
 * 泛型 enum 实例没有 `sdef`，其属主是 `edef`，两种情形都在这里处理。可以嵌套：把返回的
 * 上下文原样交给 `substLeave` 即可，不会有哪一层把别的层覆盖掉。
 *
 * @param g    generator 状态；上下文写入 g->substParams、g->substArgs 与 g->ownerPrefix
 * @param inst 实例类型
 * @return     进入之前的上下文，交给 `substLeave` 归还
 */
static SubCtx substEnter(CG *g, Type *inst) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };

    g->substParams = inst->sdef ? &inst->sdef->typeParams : &inst->edef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
    return sv;
}

/**
 * @brief 退出 substEnter 进入的替换上下文，把归还的那一份原样装回去。
 *
 * @param g  generator 状态
 * @param sv `substEnter` / `substEnterInst` / `substEnterFunc` 交出来的上下文
 */
static void substLeave(CG *g, SubCtx sv) {
    g->substParams = sv.params;
    g->substArgs   = sv.args;
    g->ownerPrefix = sv.owner;
}


static const char *cFuncName(CG *g, FuncDef *f);   /* defined below; `cType` needs it */

/* Register the `!` unwrap helper of one option/result instance; defined with the other
 * on-demand helpers, below `cgLine`. A `!` can appear in a body generated before that
 * point, so the name is declared here. */
static const char *unwrapHelper(CG *g, Type *ot, bool isOpt, Type *pay);

/**
 * @brief 给出 extC 类型对应的 C 拼写。
 *
 * 组合拼写分配在 g->arena。函数类型只以 typedef 名字出现：每个调用点把结果与名字拼
 * 成 `%s %s`，而 C 函数指针会把名字包进 declarator。
 *
 * @param g generator 状态；结果分配在 g->arena
 * @param t 待拼写的类型；NULL 视为 void
 * @return  C 类型拼写，如 `int32_t`、`pair_i32_u8 *`；错误或未解析的类型返回 `int`，
 *          只在程序已经过不了检查时出现
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
             * `stdlib/std/coro/scheduler.extc` compiles（缺 typedef 这一类问题）。*/
            if (t->sdef && isProtoType(t, "coroutine", 1)) { g->needCoroHandle = true; return "extc_coro"; }
            /* A synthesized coroutine frame: `struct <cname>$frame`, with the function's C name (so
             * methods and generic instances mangle the same way everywhere). */
            if (t->sdef && planCoroOf(t->sdef))
                return arenaPrintf(g->arena, "struct %s$frame", cFuncName(g, planCoroOf(t->sdef)));
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
             * `stdlib/std/coro/scheduler.extc` compiles（缺 typedef 这一类问题）。*/
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
 * @brief 给出函数类型的 C 形参列表文本。
 *
 * 无参时返回 `void` 而不是空括号：C 声明里的 `()` 表示「参数未指定」，是另一种类型，
 * 会放过穿过函数指针的任意实参。
 *
 * @param g  generator 状态；结果分配在 g->arena
 * @param ft 函数类型
 * @return   形参类型用逗号连接的字符串；无参时为 `void`
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
static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);
static const char *zoneArgRef(CG *g, Expr *e);   /* the zone argument of a pool-creating callee */
static void cgReclaimDropArena(CG *g, int lvl);  /* 帧销毁处释放区域（定义在 cgIsRefType 之后） */


    

/**
 * @brief 把 extC 符号名映射成能放进 C 标识符的片段。
 *
 * 运算符（如 `==`）在 extC 里是合法方法名，却不能直接进 C 标识符，所以在这里改名；
 * 映射表必须覆盖语言允许当方法名的每一个运算符，漏掉的名字会原样粘进生成 C 而编不过。
 * 用户名字可能撞上 C 关键字（`double` 在 extC 里不是关键字），这类名字加 `__c` 后缀；
 * 关键字集合由 `cIdentIsKeyword` 提供，与 checker 共用。
 *
 * @param g    generator 状态；新名字分配在 g->arena
 * @param name extC 符号名或标识符
 * @return     可放进 C 标识符的片段；无映射且不是关键字时返回 name 本身，否则返回 arena 中的新名字
 */
static const char *cSymName(CG *g, const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq"  }, { "!=", "ne"  },
        { "<",  "lt"  }, { "<=", "le"  }, { ">",  "gt" }, { ">=", "ge" },
        { "+",  "add" }, { "-",  "sub" }, { "*",  "mul" },
        { "/",  "div" }, { "%",  "rem" },
        { "<<", "shl" }, { ">>", "shr" },
        { "[]", "idx" }, { "[]=", "idxset" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    if (cIdentIsKeyword(name)) return arenaPrintf(g->arena, "%s__c", name);
    return name;
}

/**
 * @brief 取 `@export` 函数对外的 C 符号名，即作者写下的那个名字。
 *
 * extC 标识符里不允许 `$`，所以 `f->name` 中的 `$` 只可能是加载器的模块前缀
 * （`plugin$init`），符号名是它后面的部分。导出符号是与 C 侧的约定：本编译器需要改名
 * 的名字（如 `fn double`）在书写处就被拒绝，而不是悄悄变成 `double__c`。
 *
 * @param f 函数定义；NULL 或 name 为空时取 `?`
 * @return  模块前缀之后的符号名；没有 `$` 时是整个名字
 */
static const char *exportName(const FuncDef *f) {
    const char *n = (f && f->name) ? f->name : "?";
    const char *d = strrchr(n, '$');
    return d ? d + 1 : n;
}

/**
 * @brief 给出运算符 C 名字在属主重复定义该运算符时所需的类型后缀。
 *
 * 同一运算符可以按右操作数类型各定义一次，所以操作数类型必须进 C 名字，否则多个定义
 * 会塌成同一个符号。定义侧（`cFuncName`）与每个调用点（`cMethodName`）都要用同一份
 * 计算，因此放在一个函数里。
 *
 * 后缀必须在**被调方**的单态化上下文里算：`subst` 读的是 ambient 上下文，调用点若
 * 没有上下文，`T` 仍是 `T`，算出的名字就会和定义侧不一致。调用点因此显式传入被调方
 * 自己的类型实参。
 *
 * @param g      生成器状态；结果分配在 g->arena
 * @param f      运算符方法定义
 * @param params 用于替换的类型形参；NULL 表示用 ambient 上下文
 * @param args   与 params 等长的类型实参；params 为 NULL 时同为 NULL
 * @return       `_<mangled 操作数类型>`（可写视图另加 `mut_` 前缀）；属主只定义一份、
 *               形参不足两个或名字不是可重载运算符时返回空串
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
 * @brief 定义侧取运算符类型后缀：此时的 ambient 上下文就是该实例的。
 *
 * @param g 生成器状态
 * @param f 运算符方法定义
 * @return  同 opOverloadSuffixIn
 */
static const char *opOverloadSuffix(CG *g, FuncDef *f) {
    return opOverloadSuffixIn(g, f, NULL, NULL);
}

/**
 * @brief 给出函数在生成 C 里的名字。
 *
 * 判定有先后：导出函数先返回作者写下的名字（调用点必须与 C 侧查找的符号一致），
 * 泛型自由函数的实例返回自己的 C 名字，然后才是运算符后缀与属主前缀。方法名带属主
 * 前缀，使 `Point_eq` 与 `Board_eq` 不会撞名。
 *
 * @param g 生成器状态；结果分配在 g->arena
 * @param f 函数定义
 * @return  C 名字，分配在 generator 的 arena
 */
static const char *cFuncName(CG *g, FuncDef *f) {
    /* 导出名必须**最先**判定：调用点要与 C 侧查找的符号一致，模块前缀、实例前缀、
     * 关键字重命名都不能插进来。 */
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
 * @brief 给出调用点上方法在生成 C 里的名字。
 *
 * 前缀由接收者决定：泛型实例用自己的名字（如 `Pair_i32_u8_getFirst`），其余用方法
 * 属主。这里不能用 `cFuncName`：它加的是当前正在生成的实例前缀，而被调方可能属于另
 * 一个类型——在 `Wrapper<Point>` 里调 `Point.==` 正是这种情形。
 *
 * @param g        生成器状态；结果分配在 g->arena
 * @param recvType 接收者的静态类型
 * @param f        方法定义
 * @return         C 名字，分配在 generator 的 arena
 */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    /* 后缀必须取自**被调方**的上下文，而不是这里的 ambient：方法实例有自己的
     * typeArgs，否则用接收者实例的；本处正是知道接收者的地方。 */
    Vec *sp = NULL, *sa = NULL;
    if (planTemplate(f)) {
        sp = &planTemplate(f)->typeParams;
        sa = &f->targs;
    } else if (rb && rb->kind == TY_GENERIC && rb->sdef) {
        sp = &rb->sdef->typeParams;
        sa = &rb->targs;
    }
    const char *suffix = opOverloadSuffixIn(g, f, sp, sa);
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s%s", rb->name, cSymName(g, f->name), suffix);

    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return cSymName(g, f->name);
}

/**
 * @brief 判断类型是不是恰好一个类型实参的 `slice` 实例。
 *
 * 这是编译器对视图唯一知道的事，而且只是一条输出约定，不是容器的知识：字节视图按
 * 文本打印属于基本输出；容器的定义、字段与方法由标准库提供。
 *
 * @param t 待判断的类型
 * @return  是只有一个类型实参的 `slice<T>` 时为 true
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

/**
 * @brief 判断类型是不是字节视图，即 `slice<u8>`。
 *
 * @param t 待判断的类型
 * @return  元素类型是 `u8` 时为 true；此时 `println` 按文本而不是数字列表打印该视图
 */
static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/**
 * @brief 发出一个视图类型的下标原语 `<C 名>_index(v, i, file, line)`。
 *
 * 视图按值传入，所以下标表达式里的操作数只求值一次，值与引用没有第二条代码路径。原语自己做
 * 区间检查，越界时带 extC 源码位置 trap。prelude 的 `get`、`==`、`find` 都经 `self[i]` 走到
 * 这里，指针算术与手写检查因此留在库外。
 *
 * 发出的函数返回元素指针，由调用点解引用。返回元素值会弄坏两件事：`ref` 形参需要左值，
 * `slice<struct>::==`（写成 `self[i] != other[i]`，且 `self: ref T`）会直接编不过；`s[i] = x`
 * 与 `s[i].field = x` 这类赋值同样需要左值。视图本来就有引用语义，即 `data: ref T` 里的 `ref`。
 *
 * @param g     生成器。
 * @param inst  视图实例；`targs` 至少有一个元素，即元素类型。
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
 * @brief 按需发出一个视图类型的整块拷贝原语（供 `copyInto` 使用）。
 *
 * 一次调用只检查一次计数，再用 `memmove` 搬字节，所以检查按**调用**付费而不是按元素付费：
 * 逐字节的循环要为每个字节付一次区间检查，容器里还要多付一次陈旧池守卫。用 `memmove` 而不是
 * `memcpy`，因为常见的调用者把缓冲搬到与自身重叠的位置（`string::removeFront`）。
 *
 * 计数检查与其他越界访问一样带源码位置 trap。`unchecked` 是 `@unchecked` 变体：按写下的计数
 * 直接 `memmove`，长度检查与存储检查都不做，函数名带 `..._copyU`。
 *
 * @param g          生成器。
 * @param inst       视图实例；`targs` 至少有一个元素，即元素类型。
 * @param unchecked  为 `true` 时发出不做任何检查、名为 `<C 名>_copyU` 的变体。
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
 * @brief 按需登记一个视图类型的下标原语，并返回它的名字。
 *
 * 只能按需发出：单看类型实例表不够，泛型体里的下标用的是**实例**的名字
 * （`slice_slice_u8_index`），而表里未必有这个实例 —— 元素类型来自类型参数的视图就不在表里，
 * 那样生成的 C 会调用一个没人发出的函数，根本编不过。谁需要这个原语就由谁发出
 * （`sliceHelper` 同样按需），依赖集中在一处，名字去重后一个视图类型只产出一个函数。
 *
 * @param g     生成器；去重表是 `g->viewIdx`，新实例追加在末尾。
 * @param inst  视图实例，调用方已完成替换。
 * @return      原语的名字 `<视图实例名>_index`。
 */
static const char *viewIndexer(CG *g, Type *inst) {
    const char *name = arenaPrintf(g->arena, "%s_index", inst->name);
    for (size_t i = 0; i < g->viewIdx.len; i++)
        if (strcmp((*(Type **)vecAt(&g->viewIdx, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(&g->viewIdx) = inst;
    return name;
}

/**
 * @brief 按需登记一个视图类型的整块拷贝原语，并返回它的名字。
 *
 * 与 `viewIndexer` 一样按需登记。`@unchecked` 形式有自己的名字（`..._copyU`）和自己的表
 * `g->viewCpyU`：同一视图类型的受检调用点必须继续拿到受检定义。
 *
 * @param g          生成器。
 * @param inst       视图实例，调用方已完成替换。
 * @param unchecked  为 `true` 时登记 `..._copyU` 变体（表 `g->viewCpyU`），否则登记受检变体
 *                   （表 `g->viewCpy`）。
 * @return           原语的名字 `<视图实例名>_copy`；`unchecked` 时为 `<视图实例名>_copyU`。
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
 * @brief 登记一个类型需要自己的描述符。
 *
 * 按 C 名去重：可变视图与只读视图是同一个 C 结构体，共用一个描述符。标量、`ref` 与
 * `slice<u8>` 用共享描述符，不登记。
 *
 * @param g  生成器；登记表是 `g->descs`，新类型追加在末尾。
 * @param t  类型；`NULL` 或没有 C 名时直接返回。
 */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/**
 * @brief 登记一个类型需要 `extc_eq`。
 *
 * 按 C 名去重。比较容器会比较它的元素，所以元素也一并登记，这组需求由 `emitDescRegion` 收拢
 * 成闭包。
 *
 * @param g  生成器；登记表是 `g->eqNeed`，新类型追加在末尾。
 * @param t  类型；`NULL` 或没有 C 名时直接返回。
 */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

/**
 * @brief 返回表示某个类型的描述符的 C 表达式。
 *
 * 往外走时会把该描述符引用到的每个类型都登记上，这正是需求集成为闭包的原因。
 *
 * @param g  生成器。
 * @param t  类型；`NULL` 时返回 `&extc_desc_i32`。
 * @return   形如 `&extc_desc_i32` 或 `&list_i32_desc` 的字符串，分配在生成器的 arena 里。
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
 * @brief 记下刚发进当前缓冲的一段顶层定义，供之后判断有没有人引用它。
 *
 * 用写完之后的全文本判断有没有人点这个名字，和全局量一样。描述符池要靠它：那里的定义按需
 * 产生，但不是每个都会被引用。声明与定义是两段独立文本，注册在同一个名字下。
 *
 * @param g       生成器；记录追加到 `g->deadDefs`。
 * @param name    该定义登记用的 C 符号名。
 * @param before  该定义在 `g->out` 里的起始偏移。
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
 * @brief 发出一个结构体的描述符：字段名加 `offsetof`。
 *
 * 用 `offsetof` 而不是自己算布局：漏字段或布局算错会在编译期失败，而不是打印出垃圾。
 *
 * @param g      生成器。
 * @param cname  结构体的 C 名，也是它描述符与字段表的前缀。
 * @param disp   显示用的名字；泛型实例用模板名（`varArray { ... }`）。
 * @param sd     结构体定义；写字段表时字段类型会先替换再取描述符。
 * @param eqFn   相等适配器的 C 名；该类型没有适配器时为 `NULL`。
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
 * @brief 发出一个数组的描述符：元素个数加元素描述符。
 *
 * 数组没有字段名，所以打印成 `[1, 2, 3]`。
 *
 * @param g    生成器。
 * @param arr  数组类型；`name` 是它的 C 名，`inner` 是元素类型。
 */
static void genArrayDesc(CG *g, Type *arr) {
    size_t adb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s, NULL };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", arr->name), adb);
}

/**
 * @brief 发出一个非字节视图的描述符，它打印成 `[a, b]`。
 *
 * 字节视图到不了这里：它共用 `extc_desc_text`。
 *
 * @param g  生成器。
 * @param v  视图实例；`targs` 的第一个元素是元素类型，取描述符前先替换。
 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    size_t vdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s, NULL };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", v->name), vdb);
}

/**
 * @brief 发出一个枚举的描述符：变体名表，表外的值打印成 `<类型名>`。
 *
 * 只打印变体名，从不打印负载。
 *
 * @param g      生成器。
 * @param cname  枚举的 C 名，也是它描述符与变体表的前缀。
 * @param disp   显示用的名字。
 * @param td     枚举定义；没有变体时不发变体表，描述符里那一格为 `NULL`。
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
 * @brief 发出每个已登记描述符的定义。
 *
 * 循环条件每次重读 `len` 是刻意的：写定义的过程中 `descRef` 会追加新的依赖。
 *
 * @param g  生成器；读 `g->descs`，把定义写进 `g->out`。
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
 * @brief 判断一个类型是否需要生成或填入 `eq`。
 *
 * 按 C 名查。
 *
 * @param g  生成器；查询表是 `g->eqNeed`。
 * @param t  类型；`NULL` 或没有 C 名时为 `false`。
 * @return   登记过则 `true`。
 */
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/**
 * @brief 把 `eq` 集合向内收拢：需要相等性的容器，它的元素也需要。
 *
 * 结构体终止这趟遍历，因为它的相等性经 `d->eq` 交给用户写的 `fn ==`。
 *
 * @param g  生成器；遍历 `g->eqNeed`，循环中会往里追加元素。
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
 * @brief 把描述符池写进 `g->desc`。
 *
 * 池里装相等适配器与描述符定义，插在原型与函数体之间。`g->descs` 与 `g->eqNeed` 都空时，
 * 整块产物都不需要。
 *
 * @param g  生成器；临时把 `g->out` 指向 `g->desc`，写完还原。
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
 * @brief 找一个类型上用户定义的运算符方法。
 *
 * 走检查器自己的查找 `findOperator`，不在这里再抄一份：手写的扫描不替换泛型接收者的类型
 * 实参，遇到 `slice<u8> == slice<u8>` 会漏掉方法，让调用方退到裸 C `==` 去比较两个结构体。
 *
 * @param tt        类型表，方法查找在其中进行。
 * @param t         运算符的接收者类型；`ttBase(t)` 不是结构体或泛型实例时返回 `NULL`。
 * @param sym       要找的符号，例如 `"=="`。
 * @param rhs       右操作数的类型，参与重载解析。
 * @param fallback  `sym` 不存在时改试的符号，例如找 `"!="` 时试 `"=="`（结果由调用方取反）；
 *                  `NULL` 表示只接受 `sym`。
 * @return          找到的方法；两个符号都没定义时为 `NULL`。
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

/* Emit a binary operation.
 *
 * An overloadable operator that the checker already resolved carries the method
 * to call in `planCallee(e)`: that covers `==` `!=` and the ordering and arithmetic
 * operators. The same operators are emitted from here
 * when the operand was a type parameter inside a generic: `planNeedOp(e)` says the
 * decision was deferred, and the instance context is what makes `T` resolvable
 * now.
 *
 * Returns:
 *   A C expression; the caller is responsible for the surrounding syntax.
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
 * resolvable now. Array equality becomes one generic `extc_eq` call; integer division, modulo
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
        const char *l = genExpr(g, e->u.bin.left);
        const char *r = genExpr(g, e->u.bin.right);
        /* The left operand is the method receiver, so it obeys the receiver rule: a value
         * that is not a place, such as the result of a chained `<<`, has to be materialized. */
        l = selfOperandAsParam(g, e->u.bin.left, l, p0, e->u.bin.left->type);
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

    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(subst(g, a->type));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt) { bufPuts(&b, "0"); continue; }

        if (bt->kind == TY_ENUM || isByteView(bt) || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;
            g->needPrint   = true;
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_print(&(%s), %s)", code, descRef(g, bt));
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
                bufPrintf(&b, "extc_print(&%s, %s)", tmp, descRef(g, bt));
            }
            continue;
        }
        if (bt->kind != TY_BUILTIN) { bufPuts(&b, "0"); continue; }

        if (strcmp(bt->name, "bool") == 0) {
            bufPrintf(&b, "printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
            continue;
        }
        const PrintFmt *pf = NULL;
        for (size_t k = 0; PRINT_FMT[k].extc; k++)
            if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
        if (!pf) { bufPuts(&b, "0"); continue; }
        bufPrintf(&b, "printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
    }

    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "printf(\"\\n\")");
    } else if (args->len == 0) {
        bufPuts(&b, "printf(\"\")");
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
 * @brief 按被调用者的声明追加隐式尾参：家 arena 与家 zone。
 *
 * 两侧必须用同一个判定：声明侧（`forSignature`）写参数声明，调用侧写实参表达式，都按
 * `cgHasImplicitArgs` 给出的结论追加，否则生成的 C 会因参数个数不符而编译不过。
 *
 * @param g  生成器。
 * @param b  正在拼的调用/声明文本。
 * @param callee  被调用者；为 NULL 时不追加任何东西。
 * @param site  调用点表达式，决定 arena 层级与 zone 实参；声明侧可以是 NULL。
 * @param nargs  已经发出的**源**实参个数，用来决定分隔逗号。
 * @param forSignature  true 写参数声明拼写（`extc_arena *__extc_home` /
 *   `int64_t __extc_home_zone`），false 写调用实参。
 */
static void cgImplicitArgs(CG *g, Buf *b, const FuncDef *callee, const Expr *site,
                           size_t nargs, bool forSignature) {
    if (!callee) return;
    size_t n = nargs;
    if (funcTakesHomeArena(callee)) {
        if (n) bufPuts(b, ", ");
        bufPuts(b, forSignature ? "extc_arena *__extc_home"
                                : homeArg(g, site ? planArenaArg(site) : ARENA_HOME));
        n++;
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
 * @brief 把调用或运算符的第一个操作数变成其 `self` 参数要求的形态。
 *
 * `a.f(x)` 就是 `f(a, x)`、`a << b` 就是 `(<<)(a, b)`，两处都把对象作为第一个实参：
 * 参数是引用要交出地址，是值就交出值。
 *
 * 临时值取不了地址（`&(f())` 不是合法 C），所以先物化成单元素数组的复合字面量：
 * `(T[]){ f() }` 的类型是 `T *`（数组退化成指针）。这正是链式调用能工作的原因：
 * `cout << a << b` 里第二个 `<<` 拿到第一个 `<<` 的返回值。
 *
 * **不要改成 `&((T){ f() })`**：`(T){ x }` 在 C 里不是拷贝，它用 x 初始化第一个成员，
 * 于是会得到 `_Bool has = <option_i64>` 这类莫名其妙的错误；数组初始化才是逐元素进行，
 * `{ f() }` 真正是「用 T 初始化第 0 个元素」。
 *
 * 是地方（place）的操作数必须保住地址：对拷贝取地址会把写参数变成写临时。
 *
 * @param g  生成器。
 * @param operand  作为第一个实参传出的表达式。
 * @param code  它已生成的 C 文本。
 * @param p0  接收它的参数。
 * @param want  操作数的静态类型，物化临时时用来取 C 类型。
 * @return  该传出去的 C 文本。
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
 * @brief 生成 `EX_DYN`：把 `dyn Trait(x)` 变成值——载荷拷进 dyn 池，交出句柄。
 *
 * 载荷先过一个临时，rvalue 与 lvalue 都能用；`pfLine` 就是把语句放进当前前缀的机制。
 * 载荷的 C 类型取自 checker 记下的 `payloadType`：这个节点自身的类型此时已是 `dyn Trait`，
 * 临时必须是载荷的类型。
 *
 * @param g  生成器。
 * @param e  `EX_DYN` 节点。
 * @return   `extc_dyn_put` 的调用表达式（池里的句柄）。
 */
static const char *genDynValue(CG *g, Expr *e) {
    const char *payC = genExpr(g, e->u.dynv.payload);
    Type *pt = subst(g, e->u.dynv.payloadType);
    const char *ct = cType(g, ttBase(pt));
    const char *tmp = arenaPrintf(g->arena, "__extc_dyp%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", ct, tmp, payC);
    return arenaPrintf(g->arena,
                       "extc_dyn_put((const void *)&%s, (int64_t)sizeof(%s), &extc_vt$%s$%s)",
                       tmp, ct, e->u.dynv.traitName, ct);
}

/**
 * @brief 把方法调用生成成对接收者的普通函数调用。
 *
 * `a.f(x)` 就是 `f(a, x)`：接收者要变成第一个参数要求的形态（引用取地址、值直接传），
 * 其余实参按原样展开，最后补上被调用者声明里的隐式尾参与 `@overwrite` 单元。
 *
 * 在泛型体内，写下来的接收者类型可能还带着 `T`，checker 延迟解析的调用节点上干脆没有
 * `func`：方法在这里按**替换后**的类型解析。一个模板体有多个实例，把 `func` 记在节点上
 * 对其余实例就是错的方法。
 *
 * 接收者是协程句柄（`coroutine<T>`）时走 `next`/`value`/`send` 的句柄协议；被调用者属于
 * 协程时走驱动协议；接收者来自 `dyn Trait` 时从 trait 的统一表里取函数指针。
 *
 * @param g  生成器。
 * @param e  `EX_METHOD` 节点。
 * @return   C 调用表达式；checker 没解析出方法或该方法没有参数时返回 "0"。
 */
static const char *genMethodCall(CG *g, Expr *e) {
    /* The receiver's type as this instance sees it. Inside a generic body the written type may
     * still mention `T`, and for a deferred call (a method on a type parameter) the node carries
     * no `func` at all: the method is resolved here, on the substituted type, the same way the
     * operators are resolved. The checker deliberately leaves `planCallee(e)` alone -- one
     * template body has many instances, so a `func` stored there would be the wrong method for
     * all but the last one. */
    Type *recvT = subst(g, e->u.method.recv->type);
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
 * @brief 在结构体字面量里找出某个字段写下的初始化表达式。
 *
 * @param lit  结构体字面量节点。
 * @param fname  字段名，逐字比较。
 * @return  该字段的初始化表达式；字面量省略该字段时返回 NULL，调用方改用该字段的零值。
 */
static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

/**
 * @brief 生成结构体字面量的 C 文本：复合字面量或花括号初始化列表。
 *
 * 每个字段都写出来，省略的写成零值：留给 C 去隐式补零会把省略的 `str` 字段变成 NULL。
 *
 * 类型先整体替换：泛型实例里的字面量记的还是模板类型（`result<T,E>`），其类型实参是参数；
 * 直接用它会让零值那条路拿到光秃秃的 `T`，退化成 `0`，最后写出 `.value = 0`。
 * 字段类型同理，要用实例自己的类型实参替换。
 *
 * @param g  生成器。
 * @param e  `EX_STRUCTLIT` 节点。
 * @param staticInit  true 表示这是 C 静态对象（全局）的初始化式：此时不能写 `(T)` 复合
 *   字面量，C 要求常量初始化式，而复合字面量不是；花括号形式与它内容相同，只差这个前缀。
 * @return  C 文本，分配在生成器的 arena 里；类型不是结构体时返回 `(int){0}`。
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
 * @brief 把结构体字面量生成为一个 C 复合字面量表达式。
 *
 * 与全局初始化的写法共用 `genStructLitAs`，区别只在 `staticInit` 为假：未写出的字段
 * 用 `zeroValue` 补零。
 *
 * @param g  代码生成状态。
 * @param e  结构体字面量表达式。
 * @return   形如 `(T){ .field = v, ... }` 的 C 文本，分配在 arena 中。
 */
static const char *genStructLit(CG *g, Expr *e) { return genStructLitAs(g, e, false); }

/**
 * @brief 生成全局变量（C 静态对象）的初始化式。
 *
 * 初始化式必须是 C 能在编译期折叠的常量。结构体字面量因此写成花括号初始化列表
 * （见 `genStructLitAs`）；其余走到这里的表达式已被 checker 限制为常量。
 *
 * @param g  生成器。
 * @param e  初始化表达式。
 * @return  初始化式的 C 文本。
 */
static const char *genGlobalInit(CG *g, Expr *e) {
    return e->kind == EX_STRUCTLIT ? genStructLitAs(g, e, true) : genExpr(g, e);
}

static const char *arenaRefAt(CG *g, int level);
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
 * @brief 生成 `EX_EXT`：把一次协程调用作为一个任务交给调度域。
 *
 * 帧按普通帧的路径建成本函数的局部变量——域在所属块结束前就把它驱动完，所以它活得恰好
 * 够久——然后以 `(帧, step)` 交给域。整个表达式的值是 void。
 *
 * `genExprInner` 的 `EX_EXT` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_EXT` 节点。
 * @return   固定返回 `"(void)0"`；不满足前置条件时报编译错误。
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
 * @brief 生成 `EX_IDENT`：发出标识符，或在协程体内发出帧字段访问。
 *
 * 用作值的函数取其**地址**：C 本来也会把函数指示符转成指针，但写出 `&` 才能让表达式的
 * C 类型正是 `fn` 类型承诺的指针。
 *
 * 带隐藏家 arena/zone 的函数在这里被拒绝而不是照发：它比类型声明的参数多一个，而这两个
 * 标记（传递闭包）要等所有函数体检查完才最终确定，这里是答案第一次确定的地方。
 *
 * 协程里跨 `yield` 存活的参数与局部变量住在帧（一个普通结构体）里，对它们的每一次提及
 * 都变成字段访问；`FuncDef.coroFrame` 是 checker 排好的那张表。
 *
 * `genExprInner` 的 `EX_IDENT` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_IDENT` 节点。
 * @return  C 表达式；带隐藏实参的函数返回 "0"。
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
 * @brief 生成 `EX_CALL`：按被调用者与实参形态分派到各条调用发射路径。
 *
 * 依次处理：`domain::single()` 的域构造、板的 `viewOf` 门、`parallel::run`、协程句柄协议、
 * 装箱的协程调用、函数值调用，最后是普通函数（含被调用者声明的隐式尾参与 `@overwrite`
 * 单元）。
 *
 * `genExprInner` 的 `EX_CALL` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_CALL` 节点。
 * @return  调用表达式；没有解析出被调用者或形态不被支持时返回 "0"。
 */
static const char *exCall(CG *g, Expr *e) {
    /* `domain::single()`：造一个域（不透明指针）。按 checker 盖的**节点标记**认。 */
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
        /* `parallel::run(worker, 视图们…, n, threads)`：ctx 是 trampoline 那个"每视图一个字段"的
         * 结构体，只读视图原样带过去、输出的那一份在 trampoline 里按 [lo,hi) 切段；每个实参
         * **只求值一次**。chunk 传 0 让运行期自己取（约每线程 8 块，动态取块）。 */
        FuncDef *wf = planParWorker(e);
        size_t nViews = wf->params.len - 3, mutIdx = 0;
        for (size_t i = 0; i < nViews; i++)
            if ((*(Param **)vecAt(&wf->params, 3 + i))->type->mut) mutIdx = i;
        const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", wf->name);
        const char *tmp = arenaPrintf(g->arena, "__extc_pc%d", g->tmpSeq++);
        const char *ntmp = arenaPrintf(g->arena, "__extc_pn%d", g->tmpSeq++);
        Buf init;
        bufInit(&init, g->arena);
        for (size_t i = 0; i < nViews; i++)
            bufPrintf(&init, "%s.v%zu = (%s)", i ? ", " : "", i,
                      genExpr(g, *(Expr **)vecAt(&e->u.call.args, 1 + i)));
        pfLine(g, "%s %s = { %s };", ctn, tmp, bufCstr(&init));
        pfLine(g, "int64_t %s = (int64_t)(%s);", ntmp,
               genExpr(g, *(Expr **)vecAt(&e->u.call.args, 1 + nViews)));
        /* 分区按 [lo,hi) 切 ⇒ 输出比 n 短的话会切到界外。在这里挡住（worker 里的检查是另一
         * 道防线，`@unchecked` 的 worker 没有它们）。 */
        pfLine(g, "if (%s > 0 && %s.v%zu.len < %s)"
                  " extc_trapMsg(\"%s\", %d, \"parallel::run: the output view is shorter than n\");",
               ntmp, tmp, mutIdx, ntmp, g->path, e->line);
        g->needPar = true;
        /* A worker that allocates takes its memory from the run's region, so the region
         * half of the runtime has to be compiled in for this program. */
        if (planUsesHome(wf)) g->needParRegion = true;
        return arenaPrintf(g->arena,
            "((int32_t)extc_par_run(__extc_par_%s, &%s, %s, %s, (int64_t)(%s), 0))",
            wf->name, tmp, planUsesHome(wf) ? "1" : "0", ntmp,
            genExpr(g, *(Expr **)vecAt(&e->u.call.args, 2 + nViews)));
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
    if (strcmp(name, "flush") == 0) return "fflush((void *)0)";
    if (!planCallee(e)) return "0";

    /* Only `cSymName` is used, without the owner prefix: a call site names the callee itself,
     * whereas `ownerPrefix` is the instance currently being generated. This also renames a name
     * that collides with a C keyword (`fn double`). */
    name = (planCallee(e) && planInstName(planCallee(e))) ? planInstName(planCallee(e)) : cSymName(g, name);
    /* 运行期那一层：库里的 `extc_pool_new(parent)` 在收「家 zone」的函数里换成
     * `extc_pool_new_at(parent, __extc_home_zone)` —— 池因此生到**调用者选的
     * 那个地方**去。库侧一个字都不用改。 */
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
    /* 问主人**一次**就够：它把 home 与 zone 都发。分两次问（先 home，再 zone）会让两者都
     * 需要的函数各发两遍。`poolNewAt` 那条路是**例外**：名字已经换成 `extc_pool_new_at`
     * 那一扇门，只补它的第二个实参；而且别写成 `planMakesPool(planCallee(e))` ——
     * `extc_pool_new` 是 extern，工作清单不遍历没有函数体的函数 ⇒ 它的 `makesPool` 是假，
     * 只能按名字认它。 */
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
 * @brief 生成 `EX_INDEX`：数组下标与视图下标。
 *
 * 数组的长度是编译期常量，生成的检查直接带上长度。视图走 `viewIndexer` 选出的检查原语，
 * 它返回指针，解引用后是左值——读、取地址传给 `ref` 参数、赋值都能用；在这里索要它也是
 * 保证该原语定义存在的方式。
 *
 * `@unchecked` 的函数体里两边都省掉检查：数组直接 `obj.data[idx]`，视图写成
 * `*((v).data + i)` 这样的左值形态。
 *
 * `genExprInner` 的 `EX_INDEX` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_INDEX` 节点。
 * @return  C 表达式；基类型既不是数组也不是视图时返回 "0"。
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

    /* The primitive returns a pointer and the dereference is an lvalue: it can be read, addressed
     * for a `ref` parameter, and assigned to. Asking for the primitive here is also what guarantees
     * its definition exists: an index inside a generic body names the instance, and the list of
     * instances is not necessarily complete otherwise. */
    const char *ix = viewIndexer(g, ob);
    return arenaPrintf(g->arena, "(*%s(%s, (int64_t)(%s), \"%s\", %d))",
                       ix, obj, idx, g->path, e->line);
}

/**
 * @brief 生成 `EX_ASSOC`：模块限定的关联函数调用。
 *
 * 关联函数的 C 名按实例名修饰（`option_i64_some`），与方法同一条修饰规则，所以直接复用
 * `cMethodName`；随后补上隐式尾参与 `@overwrite` 单元。
 *
 * 模块限定的调用走 assoc 节点，所以域构造那条也要在这里认一次节点标记（与 `exCall` 相同）。
 *
 * `genExprInner` 的 `EX_ASSOC` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_ASSOC` 节点。
 * @return  C 调用表达式。
 */
static const char *exAssoc(CG *g, Expr *e) {
    /* 模块限定的调用走 assoc 节点 ⇒ 域构造这条也要在这里（与 `exCall` 相同，认节点标记）。 */
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
 * @brief 生成 `EX_CONV`：带检查或不带检查的数值转换。
 *
 * `@unchecked`、或 checker 没给这个转换标检查时，就是 C 的强制转换。否则查目标类型的
 * 范围，走 `extc_narrowI` / `extc_narrowU` / `extc_convFloat` 这些 `static inline` 原语，
 * 越界在运行期 trap。范围来自 checker 用的同一张表（`ttIntBits` / `ttIntSigned`）。
 * 目标是浮点时，检查通过的原语结果直接使用，不加多余的转换（见函数体说明）。
 *
 * `genExprInner` 的 `EX_CONV` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_CONV` 节点。
 * @return  转换表达式的 C 文本。
 */
static const char *exConv(CG *g, Expr *e) {
    Type *t = subst(g, e->u.conv.type);          /* target type, from the checker */
    const char *x = genExpr(g, e->u.conv.operand);
    /* `@unchecked` signs the conversion check away as well: the value is cast the
     * way C casts it, and `extc_narrowI` / `extc_convFloat` are not named. */
    if (!planConvCheck(e) || g->uncheckedBody)
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
    /* When the target is a float, the checked call's result is used directly: the context already
     * asks for a `double`, so the conversion happens implicitly, and an explicit cast applied
     * straight to a function call is what clang reports as `-Wbad-function-cast`. */
    bool toFloat = (strcmp(tn, "f32") == 0 || strcmp(tn, "f64") == 0);
    unsigned long long hi = 0;
    if (toFloat) {
        /* The conversion has to be spelled out (`-Wimplicit-int-float-conversion` is right that a
         * u64 does not become a double without losing something), but it must not be applied
         * straight to the call (`-Wbad-function-cast`). The intermediate cast names the type the
         * call already returns. */
        const char *ct = cType(g, t);
        return arenaPrintf(g->arena,
            "((%s)(uint64_t)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))",
            ct, x, g->path, e->line);
    }
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

/* ==================== 区域回收：静态根表与清扫触发点 ====================
 *
 * 触发点在**分配点**（`new T` / `new [N]T` / `alloc<T>(n)` / `@overwrite`），那里由 codegen
 * 静态发射两张东西：
 *
 *   ① 类型描述符 `ExtcRcType`：这个分配点的类型里，引用槽在哪些偏移上。存进槽里随对象走，
 *      清扫标记时按**对象自己的**描述符走指针字段（reclaim.c 的义务 4）。
 *   ② 根表：此刻活着的引用槽的**值**（机制文档第三节第一条：哪些槽是活的编译器知道）。
 *
 * 根表的来源是**词法作用域**：声明一个含引用的存储就把它的每个叶子引用槽登记进 `rcRoots`
 * （`a[i].f` 这样展开——C 层数组也是根），离开块把登记截回去。于是任一触发点的根表 =
 * 外层块还活着的局部量 + 本函数形参 + 全局量。
 *
 * 两个安全方向：
 *  · **根表不完整就一个触发都不发**（`rcRootsUnreliable`）：宁可这一轮不清扫，不可漏根把
 *    还活着的对象释放掉。枚举负载、泛型枚举、协程帧这类静态展开不出来的存储一律按不完整处理。
 *  · 偏移条目超过 4096 时同样按不完整处理，避免产物无谓地膨胀。
 *
 * 视图（`slice<T>` / `mut slice<T>`）不算根：它的 `data` 指向 arena / 板 / C 的存储，
 * 而 rc 槽只由 `new T` / `new [N]T` / `alloc<T>(n)` / `@overwrite` 产生，都不给视图。
 */

/* 一个类型里引用槽的分布。 */
typedef enum {
    RC_NO_REFS,       /* 里面没有引用槽 */
    RC_LEAF_REFS,     /* 引用槽能静态枚举出来（递归展开结构体与定长数组） */
    RC_CONSERVATIVE   /* 枚举不出来（枚举负载、泛型枚举、类型参数……）⇒ 不信任根表 */
} RcScan;

static RcScan rcScanType(CG *g, Type *t, int depth);

/**
 * @brief 扫一个结构体的字段：任一字段保守则整体保守，任一字段有引用则整体有引用。
 *
 * @param g      生成器。
 * @param fields `FieldDef*` 向量。
 * @param depth  递归深度，防类型环。
 * @return       该结构体的引用槽分布。
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
 * @brief 判断一个类型里有没有引用槽，以及能不能静态枚举。
 *
 * 视图按"没有引用槽"处理（理由见上面的块注释）；泛型结构体实例先进入实例上下文再扫字段；
 * 枚举与泛型枚举的负载不展开 ⇒ 保守。
 *
 * @param g      生成器（提供实例化上下文）。
 * @param t      要扫的类型；NULL 视为保守。
 * @param depth  递归深度，防环。
 * @return       引用槽分布。
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
    case TY_ENUM:
        return RC_CONSERVATIVE;      /* 变体负载可能带引用，静态展开不了 ⇒ 不信任根表 */
    case TY_ARRAY:
        return rcScanType(g, t->inner, depth + 1);
    case TY_STRUCT:
        return rcScanFields(g, &t->sdef->fields, depth);
    case TY_GENERIC: {
        if (isView(t)) return RC_NO_REFS;
        if (!t->sdef) return RC_CONSERVATIVE;
        /* 泛型实参里有引用槽就按枚举不出来处理：池/板容器的元素住在池自己的板上，**不在
         * C 结构体的字段里**（`vector<ref node>` 的元素引用就是这样），字段扫描看不见它们，
         * 根表会漏。宁可这一类函数一个触发都不发。 */
        for (size_t i = 0; i < t->targs.len; i++) {
            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS)
                return RC_CONSERVATIVE;
        }
        SubCtx sv = substEnter(g, t);
        RcScan s = rcScanFields(g, &t->sdef->fields, depth);
        substLeave(g, sv);
        return s;
    }
    /* kind-default: 只有上面列出的 kind 参与"有没有引用槽"的判断；任何新 kind 都可能
     * 藏着引用，落在这里一律当枚举不出来（不信任根表），这是安全方向。 */
    default:
        return RC_CONSERVATIVE;
    }
}

/**
 * @brief 把一个类型的引用叶子槽收集成"相对偏移"的 C 常量表达式。
 *
 * 表达式用 `offsetof` / `sizeof` 写出来（不自己算布局：算错会静默漏标），
 * 数组按元素展开，嵌套结构体把偏移相加。
 *
 * @param g      生成器。
 * @param t      要展开的类型。
 * @param base   当前子对象的偏移表达式；顶层传空串表示 0。
 * @param out    `const char *` 向量，接收每个引用槽的偏移表达式。
 * @param depth  递归深度，防环。
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
        /* 定长数组在 C 里是 `struct { T data[N]; }` 包一层（见数组类型的发射），
         * 元素偏移从 `offsetof(数组类型, data)` 起算。 */
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
    /* kind-default: 偏移只在 REF / ARRAY / STRUCT / GENERIC 四种形状里递归；其余 kind
     * 没有引用槽可收集。新 kind 若含引用槽，必须在这里加 case，不能靠 default 混过去。 */
    default:
        return;
    }
}

/**
 * @brief 为一次分配发射类型描述符，返回指向它的 C 表达式。
 *
 * 没有引用槽 ⇒ 返回 `(const ExtcRcType *)0`（标记到这个对象就停）；枚举不出来 ⇒
 * `EXTC_RC_CONSERVATIVE`（标记扫这个槽的每个字）。描述符是**块作用域 static const**：
 * 地址有静态存储期，槽里存得下；放在分配表达式之前（语句前缀）。
 *
 * @param g  生成器；`needReclaim` 关着时返回空描述符。
 * @param t  分配的类型。
 * @return   指向 `ExtcRcType` 的 C 表达式。
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
    int id = g->rcSeq++;
    pfLine(g, "__attribute__((unused)) static const uint32_t __extc_rco%d[] = { %s };",
           id, bufCstr(&b));
    pfLine(g, "__attribute__((unused)) static const ExtcRcType __extc_rct%d = { %zuu, __extc_rco%d };",
           id, offs.len, id);
    return arenaPrintf(g->arena, "&__extc_rct%d", id);
}

/**
 * @brief 在一个分配点发射清扫触发：`extc_rc_maybeCollect(区域, 根表, 根数)`。
 *
 * 压力判据与自学习退场在运行期（reclaim.c），这里只负责**按正确参数与时机调用**：
 * 区域取这次分配要落进去的那一只（与 `extc_rc_alloc` 同一个认领键），根表是此刻活着的
 * 引用槽。触发写在**语句前缀**里，所以它落在包含这次分配的那条语句之前——新对象还没生出来，
 * 不需要为它置根。
 *
 * **入口守卫**：根表是调用实参，C 语义要求在调用前把整张表（本形状 ~1412 项）物化出来；
 * 而 `maybeCollect` 的第一句就是 `if (!extc_rc_shouldSweep(r)) return -1;` —— 绝大多数分配点
 * 根本到不了压力线。于是这里先把判据提到调用点前面：**只有该扫时**才物化根表并调用。
 * 这是纯粹的求值顺序改动，改变的是"什么时候物化根表"，不是"什么时候收集"：
 * 判据只有 `extc_rc_shouldSweep` 这一份（`maybeCollect` 内部用的就是同一个函数），不另抄。
 *
 * @param g         生成器。
 * @param basePtr   该分配要落进去的那只区域的**基址表达式**（已经是 `(void *)` 形态），
 *                  形如 `(void *)&(__extc_a[1])`、`(void *)__owc0->home`；第 k 片的认领键是
 *                  `(char *)basePtr + k`（只比不读，见 `rcAllocCore`）。
 * @param lvl       块层级（1 = 函数体，-1 = 家区域）；分片时区域号取 `lvl * 片数 + k`。
 */
static void rcEmitTrigger(CG *g, const char *basePtr, int lvl) {
    if (!g->needReclaim || g->rcRootsUnreliable || g->prefixBlk == 0) return;
    size_t n = g->rcGlobalRoots.len + g->rcRoots.len;
    if (n > 4096) { g->rcRootsUnreliable = true; return; }
    Buf b;
    bufInit(&b, g->arena);
    if (n == 0) {
        bufPuts(&b, "(void *const *)0");
    } else {
        bufPuts(&b, "(void *[]){ ");
        size_t k = 0;
        for (size_t i = 0; i < g->rcGlobalRoots.len; i++) {
            bufPrintf(&b, "%s(void *)(%s)", k++ ? ", " : "",
                      *(const char **)vecAt(&g->rcGlobalRoots, i));
        }
        for (size_t i = 0; i < g->rcRoots.len; i++) {
            bufPrintf(&b, "%s(void *)(%s)", k++ ? ", " : "",
                      *(const char **)vecAt(&g->rcRoots, i));
        }
        bufPuts(&b, " }");
    }
    const char *rg = arenaPrintf(g->arena, "__extc_rcg%d", g->rcGuardSeq++);
    if (g->rcNShard <= 1) {
        /* 单片：与接线前逐字节相同（认领键就是区域基址）。 */
        pfLine(g, "{ ExtcRcRegion *%s = extc_rc_regionOf(%s, %d);"
                  " if (extc_rc_shouldSweep(%s)) extc_rc_maybeCollect(%s, %s, %zu); }",
               rg, basePtr, lvl, rg, rg, bufCstr(&b), n);
        return;
    }
    /* 分片：区域键是"基址 + k"（只比不读）。一次触发**只收一片**，挑的是**垃圾最多**的那片
     * （`bytes - live`，与原型同一办法）：收完它的 `bytes` 掉下来，下一次自然轮到别的片 ——
     * 不需要额外的轮转状态，也不会让某一片永远轮不上（先到先收会让第一片独占）。
     *
     * 内存压力下限**按片均分**（`EXTC_RC_FLOOR / N`，与原型 `TRIGGER / NSHARD` 同一口径）：
     * 不除的话 N 片各自守着一个全长下限，总量上界变成 N 倍，停顿旋钮就顺带改了内存界；
     * 除完总量仍是 `EXTC_RC_FLOOR`，片多只让每片收得更勤、每次更小。下限由运行期的
     * `extc_rc_regionFloor` 设（判据仍在运行期，这里只是给同一个下限除以片数）。
     * 根表仍是调用实参、仍只在真的要扫时才物化 —— 与单片的入口守卫同一条口径。 */
    const char *ck = "__extc_rck";
    const char *rb = arenaPrintf(g->arena, "__extc_rcb%d", g->rcGuardSeq);
    const char *rbg = arenaPrintf(g->arena, "__extc_rcbg%d", g->rcGuardSeq);
    pfLine(g, "{ ExtcRcRegion *%s = (ExtcRcRegion *)0; int64_t %s = 0;"
              " for (int32_t %s = 0; %s < %d; %s++) {"
              " ExtcRcRegion *%s = extc_rc_regionOf((void *)((char *)(%s) + %s), (int32_t)(%d + %s));"
              " extc_rc_regionFloor(%s, (EXTC_RC_FLOOR) / %d);"
              " if (!extc_rc_shouldSweep(%s)) continue;"
              " int64_t %s = extc_rc_bytes(%s) - extc_rc_live(%s);"
              " if (!%s || %s > %s) { %s = %s; %s = %s; } }"
              " if (%s) extc_rc_maybeCollect(%s, %s, %zu); }",
           rb, rbg, ck, ck, g->rcNShard, ck, rg, basePtr, ck, lvl * g->rcNShard, ck,
           rg, g->rcNShard, rg,
           rbg, rg, rg, rb, rbg, rbg, rb, rg, rbg, rbg,
           rb, rb, bufCstr(&b), n);
}

/**
 * @brief 发射一个 rc 分配：片号按**分配点**轮转，返回 `extc_rc_alloc(...)` 调用的文本。
 *
 * 片号 = `rcSite++ % rcNShard`：同一处 `new` 永远进同一片，所以片内链条与环完整；片与片之间
 * 的引用由已有的跨区登记（`storeRefOwned` 的 `rsInc`/`rsDec`）负责，登记位就落在**目标所在
 * 那一片**的 `rsbit` 上 —— 播种与清扫都不跨片。
 *
 * 触发与分配用**同一个**片键：先按这一片的键问一次压力（触发落在语句前缀），真正的分配再从
 * 同一片取槽。N == 1 时返回的文本与接线前逐字节相同。
 *
 * @param g          生成器。
 * @param basePtr    区域基址表达式（同 `rcEmitTrigger`）。
 * @param lvl        块层级（可 -1，只用于区域号）。
 * @param bytesExpr  字节数的 C 文本（原样拼进调用，不再加括号）。
 * @param td         类型描述符的 C 文本。
 * @return           `extc_rc_alloc(...)` 的 C 文本。
 */
static const char *rcAllocCore(CG *g, const char *basePtr, int lvl,
                               const char *bytesExpr, const char *td) {
    int nsh = g->rcNShard > 0 ? g->rcNShard : 1;
    int k = g->rcSite++ % nsh;
    rcEmitTrigger(g, basePtr, lvl);
    if (nsh == 1)
        return arenaPrintf(g->arena,
            "extc_rc_alloc(extc_rc_regionOf(%s, %d), %s, %s)",
            basePtr, lvl, bytesExpr, td);
    return arenaPrintf(g->arena,
        "extc_rc_alloc(extc_rc_regionOf((void *)((char *)(%s) + %d), %d), %s, %s)",
        basePtr, k, lvl * nsh + k, bytesExpr, td);
}

/**
 * @brief 把一个存储的每个引用叶子槽登记成根。
 *
 * 数组按元素展开，嵌套结构体点下去（`a[i].f`）。视图不登记（其 `data` 不是 rc 槽）。
 * 这条路径慢/深都不是问题：它只在**声明**处跑一次，而且只在开关打开时。
 *
 * @param g     生成器；登记追加到 `g->rcRoots`。
 * @param t     存储的类型。
 * @param base  该存储在 C 里的左值表达式（如 `saved`、`h.c`）。
 * @param depth 递归深度，防环。
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
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        for (int64_t i = 0; i < t->asize; i++) {
            if (g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; }
            /* 数组是包一层的 C 结构体：元素写 `arr.data[i]`。 */
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
        if (isView(t) || !t->sdef) return;
        SubCtx sv = substEnter(g, t);
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcRootLeaves(g, fd->type,
                         arenaPrintf(g->arena, "%s.%s", base, fd->name), depth + 1);
        }
        substLeave(g, sv);
        return;
    /* kind-default: 根只在 REF / ARRAY / STRUCT / GENERIC 四种形状里递归；其余 kind 没有
     * 引用槽可登记。新 kind 若含引用槽，必须在这里加 case，不能靠 default 混过去。 */
    default:
        return;
    }
}

/**
 * @brief 收集一个类型里引用叶子槽的 **C 左值路径**（沿值投影：字段 / 内联数组元素）。
 *
 * 与 `rcRootLeaves` 是同一个递归形状，区别只在出口：那里把叶子登记成**根**，这里把叶子的
 * 路径交给调用方 —— **整值存储**（`s.w = wrap { n: keeper }`、`var b: wrap = ...`）要按叶子
 * 逐条走引用槽的记账（`initRef` / `storeRefOwned` / `storeRefUnknown`）。整块裸拷既不 `inc`
 * 也不登记跨区边：槽被写进去了却没记账，对象会在计数归零或区域清扫时被放掉，而槽还指着它。
 *
 * `base` 必须是**可重复求值**的左值（调用方先用临时量 / `(*p)` 把目标与值各求值一次）：
 * 路径会把它拼进每一条记账调用里。
 *
 * @param g     生成器。
 * @param t     类型。
 * @param base  该值在 C 里的左值表达式（如 `(*__extc_svd0)`、`b`）；传空串则产出**相对路径**
 *              （`.f`、`.kids.data[0]`），调用方自己拼两个基 —— 这样同一份叶子集合能同时
 *              用在目标与值上，不会两边各收一次、也不会有一条只在一侧出现。
 * @param out   `const char *` 向量，依次接收每个引用叶子的路径。
 * @param depth 递归深度，防类型环。
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
        /* 定长数组在 C 里是包一层的结构体：元素写 `arr.data[i]`（与 `rcRootLeaves` 一致）。 */
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
    /* kind-default: 只有上面列出的 kind 参与引用叶子的展开；其余 kind 由 `rcScanType`
     * 归到"没有引用"或"枚举不出来"，走不到这里。 */
    default:
        return;
    }
}

/**
 * @brief 声明一个存储时登记它的引用根；枚举不出来就把本函数的触发整个关掉。
 *
 * @param g     生成器。
 * @param t     声明的类型。
 * @param base  该存储在 C 里的左值表达式。
 */
static void rcRootDecl(CG *g, Type *t, const char *base) {
    if (!g->needReclaim || !t) return;
    RcScan sc = rcScanType(g, t, 0);
    if (sc == RC_CONSERVATIVE) { g->rcRootsUnreliable = true; return; }
    if (sc == RC_NO_REFS) return;
    rcRootLeaves(g, t, base, 0);
}

/**
 * @brief 生成 `EX_NEW`：在 checker 算出的层级上分配。
 *
 * 层级可能因为值被存进活得更久的地方而被提升，这是带自动清理的内存真正被取走的地方。
 * 带计数时生成 `slice<T>` 视图，计数只求值一次：不纯的计数被 checker 标成 `needTemp`，
 * 先存进临时。
 *
 * `genExprInner` 的 `EX_NEW` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_NEW` 节点。
 * @return  分配表达式或视图字面量的 C 文本。
 */
static const char *exNew(CG *g, Expr *e) {    Type *w = subst(g, e->u.new_.type);
    /* Allocate at the level the checker computed, which may have been promoted because the value
     * is stored into a place that lives further out. This is where memory with automatic cleanup
     * is taken. */
    if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, e, "new");
    const char *ar = arenaRefAt(g, planArenaLevel(e));
    if (!e->u.new_.count) {
        /* The place of one T (or one [N]T) is simply its address. */
        /* 回收打开时，这份存储从 **rc 区域**取：`ref T` / `ref [N]T` 是引用槽，会被计数
         * 函数（`extc_rc_initRef` / `storeRef` / `register`）摸到，而计数只认 slab 槽 ——
         * 落在 arena 里的指针进 `extc_rc_slabOf` 会读到垃圾。区域按 arena 的地址认领
         * （见 `emitRuntimePrologue` 那张表），所以寿命与 arena 完全一致。
         *
         * `new T[n]`（下面那条：一个 `slice<T>` 视图）不走这里：视图**不是引用槽**，
         * 计数永远摸不到它的 data 指针，留在 arena 里即可 —— 也正好避开 slab 的尺寸上限
         * 对数组长度的那条限制。 */
        if (g->needReclaim) {
            int lvl = planArenaLevel(e);
            /* 顺序：先类型描述符（它必须在这一条语句里可见），再触发（落在语句之前），
             * 最后是分配表达式 —— 三者都按**同一个片号**（见 `rcAllocCore`）。 */
            const char *td = rcEmitTypeDesc(g, w);
            const char *base = arenaPrintf(g->arena, "(void *)&(%s)", ar);
            const char *core = rcAllocCore(g, base, lvl,
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
    return arenaPrintf(g->arena,
        "(%s){ .data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s),"
        " \"%s\", %d), .len = (int64_t)(%s) }",
        cType(g, st), cType(g, w), ar, n, cType(g, w), g->path, e->line, n);
}

/**
 * @brief 生成 `EX_GENCALL`：`alloc<T>(n)` 与池的那几个原语调用。
 *
 * `alloc<T>(n)` 向 checker 选定的 arena 层级要 n 个 T 的地，交回指针。置零不是这条路的
 * 事：`extc_arena_alloc` 给每次分配置零，语言的承诺正靠它——生命周期结束后读到的字节
 * 总是已初始化的字节。`poolSlice` / `poolResize` / `poolGive` / `copyInto` 以及它们的
 * `Raw` 变体形状相同，只是内存属于池自己的板。
 *
 * `genExprInner` 的 `EX_GENCALL` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_GENCALL` 节点。
 * @return  调用表达式的 C 文本。
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
    /* `alloc<T>(n)` 返回的 `T *` 是引用类型，会被引用槽接住 ⇒ 回收打开时它也得是 slab 槽，
     * 理由与 `exNew` 那条完全相同（区域按 arena 地址认领）。池/板那几条要的是池自己的板，
     * 不是这里的事。 */
    if (g->needReclaim) {
        int lvl = planArenaLevel(e);
        Type *et = subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0));
        /* 一块 `n` 个元素的存储：元素 1..n-1 的引用字段偏移随运行期 `n` 变，静态描述符
         * 表达不了 ⇒ 元素类型里有引用时用保守标记（扫整块的字），否则不需要描述符。 */
        const char *td = "(const ExtcRcType *)0";
        if (rcScanType(g, et, 0) != RC_NO_REFS) td = "((const ExtcRcType *)-1)";
        const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)&(%s)", ar), lvl,
            arenaPrintf(g->arena, "(int64_t)(%s) * (int64_t)sizeof(%s)", n, tn), td);
        return arenaPrintf(g->arena, "(%s *)%s", tn, core);
    }
    return arenaPrintf(g->arena,
        "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d)",
        tn, ar, n, tn, g->path, e->line);
}

/**
 * @brief 生成 `EX_COALESCE`：`??` 的空值判断与备选值。
 *
 * 主语不纯时（`f() ?? -1`）先求值进临时再判断，避免读两次有副作用。备选值那一侧按结果
 * 类型显式转型，因为 C 的 `?:` 会对两臂做通常算术转换（见函数体说明）。
 *
 * `genExprInner` 的 `EX_COALESCE` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_COALESCE` 节点。
 * @return  条件表达式的 C 文本。
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
 * @brief 生成 `EX_ENUMVAL`：枚举变体值的构造。
 *
 * 无载荷的枚举就是 C 枚举，变体自身是常量；带载荷的枚举在 C 里是 `struct { tag; union }`，
 * 所以连无载荷变体也要构造，带载荷的按 `._0`、`._1`… 填入 union。
 *
 * 类型名要认泛型实例：实例化后枚举的 C 名（`maybe_i64`）不在类型表里，checker 解析出的
 * 类型记在 `assocOwner` 里，优先用它。
 *
 * `genExprInner` 的 `EX_ENUMVAL` 分支落到这里。
 *
 * @param g  生成器。
 * @param e  `EX_ENUMVAL` 节点。
 * @return  枚举常量或构造表达式的 C 文本。
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

/**
 * @brief 按表达式节点分派，生成该节点对应的一段 C 表达式文本。
 *
 * 每个 `case` 返回的都是一段可嵌入更外层表达式的 C 代码；未覆盖的节点落到末尾返回
 * `"0"`，所以新增 Expr 种类必须在这里补一支。
 *
 * @param g  生成器状态：arena 与缓冲区从这里取，错误也报给它。
 * @param e  要生成的表达式节点；调用方保证非 NULL。
 * @return   生成出来的 C 表达式文本，内存属于 `g->arena`。
 */
static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_EXT: return exExt(g, e);
        case EX_LAMBDA: {
            /* The environment, built where the lambda is written: checker 把字段值放在节点
             * 上，所以这里手写一个 struct 字面量。按名字走的字面量路径用不了 —— lambda 的
             * 类型在源码里没有名字可查。 */
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
            /* `f32` 字面量写成它本来的 float：`%g` 印出的是 `double`，把它赋给 `float` 是
             * 隐式收窄，clang 以 `-Wimplicit-float-conversion` 报出来。值不变，转换只是
             * 把这件事写明白。 */
            {
                bool isF32 = e->type && e->type->kind == TY_BUILTIN && e->type->name &&
                             strcmp(e->type->name, "f32") == 0;
                /* 溢出 `double` 的字面量（`1e400`）没有对应的 C 字面量，而 `%g` 会把它印成
                 * `inf` —— 在 C 里那是个**未声明标识符**（`-1e400` 印成 `-inf`，gcc 就此报错）。
                 * `__builtin_inf()` / `__builtin_nan("")` 说同样的话，又不必拉进 `<math.h>`：
                 * 那个 include 会改掉每一个产出文件的字节。 */
                const char *special = NULL;
                if (e->u.fval != e->u.fval)                       special = "__builtin_nan(\"\")";
                else if (e->u.fval != 0.0 && e->u.fval * 0.5 == e->u.fval)
                    special = e->u.fval < 0 ? "-__builtin_inf()" : "__builtin_inf()";
                if (special)
                    return isF32 ? arenaPrintf(g->arena, "(float)%s", special) : (char *)special;
                /* 要**可往返**，不能用 `%g`：`%g` 只留六位有效数字，会悄悄改掉任何需要更多
                 * 位数的字面量的值（`4294967295.0` 印成 `4.29497e+09`，于是
                 * `x - 4294967296.0` 得 0 而不是 -1，对该字面量的转换随后会在它本来落在的
                 * 区间的边界外触发陷阱）。17 位足以往返一个 `double`，9 位足以往返一个
                 * `float`（下面的转换才让 f32 字面量变成 float，所以它的位数只需撑过那一
                 * 步）。`%g` 仍会去掉尾随零，所以普通字面量（`2.5`、`0.5`）不变。 */
                if (isF32) return arenaPrintf(g->arena, "(float)%.9g", e->u.fval);
                /* 整数值的 `f64` 印出来没有小数点也没有指数（`1.0` -> `1`，`-0.0` -> `-0`），
                 * 在 C 里那是个*整数*常量：字面量换了类型，于是 `1.0 / 2.0` 变成整数除法
                 * `(1 / 2)` = 0、`1e9 * 1e9` 溢出 `int`、`-0.0` 丢掉符号（`1.0 / z` 印成
                 * `inf` 而不是 `-inf`），而且这些都没有任何诊断。文本本身不是浮点常量时
                 * 就强制补成浮点常量。上面的 f32 分支不受影响：它的 `(float)` 转换已经定死
                 * 了类型。 */
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
         * the signature itself is **checked at runtime** (2026-10-08): the merit
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
         * A function marked `@unchecked` keeps the old raw behaviour: it is the
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
             * itself is returned as the lvalue. The checker guarantees the operand of
             * an assignment target is a place (check_stmt.c, `assignTargetWritable`),
             * so no temporary is needed and nothing is evaluated twice. */
            if (g->inAssignTarget) {
                const char *v = genExpr(g, e->u.sign.operand);
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
 * @brief 生成一个值位置上的表达式，并在需要时补上自动解引用。
 *
 * checker 把一个值位置上的 `ref T` 当成 `T` 用，并给该节点打上 `deref`；那一步解引用
 * 在这里补上。于是 `let y = p` 复制值、`p + 1` 做数值加法，而目标能不能写仍由类型里的
 * `ref` 与 `mut ref` 决定。
 *
 * 包在 genExprInner 外面是这条规则完整的原因：每一条生成表达式值的路径（含递归调用）
 * 都经过这里，没有哪条路径会漏掉解引用。
 *
 * @param g  生成器状态。
 * @param e  表达式节点，不得为 NULL（调用方违反即为调用方的 bug）。
 * @return   生成出来的 C 表达式文本，内存属于 `g->arena`。
 */
static const char *genExpr(CG *g, Expr *e) {
    /* 每一个调用者都传一个表达式节点：这个值位置上 AST 没有可选字段，NULL 就是调用方的
     * bug。写出来让这个不变量对读代码的人和静态分析器都可见。 */
    assert(e != NULL);
    const char *s = genExprInner(g, e);
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
 * @brief 把要放进 `f32` 槽位的值写成真正的 float。
 *
 * `let e: f32 = 3.14` 直接发 `float e = 3.14;`：字面量是 `double`，赋值处发生隐式收窄，
 * clang 会为此报 `-Wimplicit-float-conversion`。加一个显式转换后值不变，只是不再隐式。
 *
 * @param g       生成器状态，用于在 arena 上拼转换文本。
 * @param target  目标槽位的类型；只有它是内建 `f32` 才会加转换。
 * @param src     源表达式，用它的类型判断是否需要转换；NULL 表示按原样返回。
 * @param val     已经生成好的源值文本，必须只求值一次。
 * @return        `(float)val`，或原样返回 `val`（目标不是 `f32`，或源不是 `f64` 时）。
 */
static const char *asF32(CG *g, Type *target, Expr *src, const char *val) {
    if (!target || target->kind != TY_BUILTIN || !target->name || strcmp(target->name, "f32") != 0)
        return val;
    if (!src || !src->type || src->type->kind != TY_BUILTIN || !src->type->name) return val;
    if (strcmp(src->type->name, "f64") != 0) return val;
    return arenaPrintf(g->arena, "(float)%s", val);
}

/**
 * @brief 去掉恰好包住整条条件的多余一对圆括号。
 *
 * 语句自己带括号，表达式打印又会给比较加一对，于是 `if x == y` 印成 `if ((x == y))`，
 * clang 以 `-Wparentheses-equality` 报出来。
 *
 * 只剥包住整条条件的那一对：在字符串字面量里遇到、并不在末尾闭合的 `)` 会让原文原样返回
 * ——代价是一对多余括号，含义不变。
 *
 * @param g  生成器状态，剥括号时借它的 arena 拼新串。
 * @param e  条件文本；NULL 或首字符不是 `(` 时原样返回。
 * @return   剥掉外层括号后的文本，或原样返回 `e`。
 */
static const char *cgCond(CG *g, const char *e) {
    if (!e || e[0] != '(') return e;
    int depth = 0;
    for (size_t i = 0; e[i]; i++) {
        if (e[i] == '(') depth++;
/**
 * @brief 找到一个恰好闭合最外层 `(` 的 `)`，即条件的末尾。
 *
 * 这个 `)` 之后若还有字符，说明这一对括号并不包住整条条件，原样返回；否则去掉这对
 * 括号并交回余下文本。
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
 * @brief 给出成功侧 payload 的 C 路径。
 *
 * 就是 `x.u.some._0` 或 `x.u.success._0`：`option` 与 `result` 在 C 里都是
 * `struct { tag; union }`，payload 固定放在 `u.<成功变体>._0`。
 *
 * @param g   生成器状态。
 * @param ti  已被 genTryHead 填好的描述符：用 `tmp` 与 `okVar` 拼路径。
 * @return    该 payload 的 C 左值表达式。
 */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/**
 * @brief 构造 `?` 的操作数失败时应当返回的值。
 *
 * 失败值按外层函数的返回类型构造。对 `option` 就是它的零值，也就是 `none`；对 `result`
 * 是一个携带内层错误的失败变体。
 *
 * @param g   生成器状态。
 * @param ti  `?` 操作数的描述符；`isOpt` 区分 `option` 与 `result`，`tmp` / `failVar`
 *            用来搬内层错误。
 * @return    返回类型上的 C 表达式；外层函数没有返回类型时返回 `"0"`。
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
 * @brief 在一条退出路径上递减递归深度计数器。
 *
 * 递减必须排在返回值计算之后，绝不能排在它前面：先递减会把 `return spin(n)` 变成
 * `--depth; return spin(n);`，于是尾调用每一帧都把计数器归零，守卫永远不触发（-O0 下
 * gcc 会把这种形状折成尾调用，程序立刻无输出退出）。
 *
 * `return` 不是唯一的出口：函数体末尾的隐式返回也是出口，所以函数体结束处也要递减；
 * 已经通过 cgReturn 递减过的显式 return 不再减第二次。
 *
 * @param g  生成器状态；函数不是自递归时不发任何东西。
 */
static void cgRecLeave(CG *g) {
    if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
}

/**
 * @brief 从当前函数返回一个值。
 *
 * 每条出口都汇到同一个尾声，而不是在每个 return 处重抄一遍 arena 释放序列：值先存进
 * `__extc_ret_v`，递减递归深度，然后跳到 `__extc_ret`，那段释放序列在那里只出现一次。
 *
 * 值在 arena 释放之前算出来，比“先释放再求值”略保守。
 *
 * @param g    生成器状态。
 * @param val  要返回的值的 C 表达式；NULL 表示返回空（`void` 出口）。带 `noArena` 的
 *             函数直接返回，它不分配，也没有 arena 数组要释放。
 */
static void cgReturn(CG *g, const char *val) {
    /* The buffered console tail leaves here (ruling 90): every `return` of `main` is a
     * place where the process is about to end and whatever is still buffered would be
     * lost. It is a no-op when the buffer is empty. */
    if (g->inMain && g->needCout) cgLine(g, "extc_cout_flush();");
    if (g->noArena) {
        if (g->isRecursive) {
            if (val) cgLine(g, "{ int64_t __r = (int64_t)(%s); --__extc_rec_depth; return __r; }", val);
            else     cgLine(g, "{ --__extc_rec_depth; return; }");
        } else if (val) cgLine(g, "return %s;", val);
        else            cgLine(g, "return;");
        return;
    }
    if (val) cgLine(g, "__extc_ret_v = %s;", val);
    cgRecLeave(g);
    cgLine(g, "goto __extc_ret;");
}

/**
 * @brief 发出“把 `?` 的操作数求值一次、失败即返回”的语句，并把结果描述给调用者。
 *
 * 失败分支的 return 已经写在这一步发出的行里，所以“释放掉所有外层 arena 层级”这件事
 * 必须在这行之前定下来。
 *
 * @param g  `?` 所在的生成器状态。
 * @param e  `EX_TRY` 节点。
 * @return   描述操作数临时量、其类型的 C 名与变体名的 TryInfo，调用者随后由它读 payload。
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
        /* `?` in `main`: the failure is fatal, so it is reported and the program stops
         *. The payload is printed through its descriptor, which is
         * the same machinery `println(e)` uses -- so `ioError.readFailed(5)` still says
         * which fd failed rather than "something failed" ✓ A `none` has no payload. */
        if (!ti.isOpt) {
            Type *et = subst(g, *(Type **)vecAt(&ot->targs, 1));
            g->needRuntime = true;               /* the descriptor table ... */
            g->needPrint   = true;               /* ... and `extc_print`, called just below */
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: a step in `main` failed: \","
                      " \"%s\", %d);", g->path, e->line);
            cgLine(g, "extc_print(&(%s.u.%s._0), %s);", ti.tmp, ti.failVar, descRef(g, et));
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
 * @brief 发一条 `#line` 指令，指回该语句对应的 .extC 源位置。
 *
 * @param g  生成器状态；行映射关闭时不发任何东西。
 * @param s  语句；`s->line` 为 0（语句不带行号）时同样不发。
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
 * @brief 给出这个调用点要递给被调者的 zone 实参，也就是“池该生在哪个地方”。
 *
 * 与 homeArg 平行的一条：arena 那边递的是“调用者选的那只 arena”，这里递的是“调用者选
 * 的那个地方的 zone 下标”。
 *
 *   - 本函数自己就收了这个参数（`funcHasZoneParam`）⇒ 原样往下传：
 *     `vector<i32>::new` → `withCap` → `pool<V>::withParent` → `extc_pool_new_at`
 *     这条链上，用户在最外面选的那个地方一路传到运行期；
 *   - 否则（入口文件里的 main / 普通函数）⇒ 递当前这个地方的 zone，也就是最近一层压过的
 *     `__extc_zm<lvl>` —— 与“池生在建它的那个块里”完全一致。
 *
 * 站点自己记了层级（checker 的 `zoneLevel`）：`ZONE_HOME` = 用本函数收到的家 zone（一路
 * 往下传），k>=1 = 第 k 层那个地方的 `__extc_zm<k>`。
 *
 * 兜底是运行期自己的“我此刻所在的地方” `extc_zoneTop`：它与
 * `(extc_pool_zoneDepth() - 1)` 完全等价（运行期里 `zoneDepth() = zoneTop + 1`），但它
 * 的含义是直接的，不像后者随 `zoneDepth()` 的定义漂移。单栈下它就是调用者所在的地方。
 * 只有 `noArena` 的函数会走到这里（它没压过任何 zone）；`stmtMakesPool` 对“未解析的被调者”
 * 保守算真时也会走到这里，而那个函数其实不建池，于是既没有 zone 也没有运行期，调用点
 * 仍然要凑出第二个实参。这一支会引用运行期的 zone 表，所以这里把池运行期标成需要；
 * 运行期文本在函数体之后才发（`if (g->needPool) poolsEmitRuntime(...)`），来得及。
 *
 * @param g  生成器状态。
 * @param e  调用点表达式；NULL 或没有 zone 层级时走兜底。
 * @return   可以直接当函数实参用的 C 表达式，命名一只 zone。
 */
static const char *zoneArgRef(CG *g, Expr *e) {
    /* 站点自己记了层级（checker 的 `zoneLevel`）就按它发：`ZONE_HOME` = 用本函数收到的家
     * zone（一路往下传），k>=1 = 第 k 层那个地方的 `__extc_zm<k>`。 */
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
            /* `main` 的 C 签名是固定的，收不到家 zone 参数 ⇒ 它的"家"就是**它自己那个体**
             * （与家 arena 的 `__extc_home = &__extc_a[1]` 同一条口径）。
             *
             * 这里直接落到下面那条"最近一层压过的 zone"是**错的地方**：
             * 一个在内层块里升级出来的池会登记到内层块的 zone 上，出块就被回收，
             * 而接收者还活着。SSO 的 `push` 就是第一个"为接收者建池"的方法：
             * 循环里 push 到第 17 个字节升级 ⇒ 每轮出块收掉池 ⇒ 下一次 `pStale` 把串毒成空，
             * `len=3` 而不是 20；串声明在循环外面时更糟 —— 池已还给 malloc，读到的是复用的内存。
             * 家 zone 更长寿，代价只是内存（与逃逸分析"宁可多算"的方向一致）。 */
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
    /* 兜底会引用运行期的那张 zone 表，所以这里要把池运行期标成"需要" —— 运行期文本在函数体
     * 之后才发（`if (g.needPool) poolsEmitRuntime(...)`），还来得及。走到这一支的典型情形是
     * `stmtMakesPool` 对"未解析的被调者"保守算真：那个函数其实不建池，于是既没有 zone 也没
     * 有运行期，而调用点仍然要凑出第二个实参。 */
    g->needPool = true;
    return "extc_zoneTop";
}

/**
 * @brief 把 checker 选定的 arena 层级写成一个 arena 实参。
 *
 * 这个结果是要当实参递出去的指针本身，别和 arenaRefAt 混：后者的结果还会被调用处套上
 * `&`，所以它给的是 `(*__extc_home)` 这种解引用形式。
 *
 * @param g         生成器状态，借它的 arena 拼下标形式。
 * @param arenaArg  arena 层级：`ARENA_HOME`（函数自己的家 arena），或 1 以上的块层级。
 * @return          `__extc_home` 或 `&__extc_a[arenaArg]`。
 */
static const char *homeArg(CG *g, int arenaArg) {
    if (arenaArg == ARENA_HOME) return "__extc_home";
    return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);
}

/**
 * @brief 给出某个层级上的分配点所指的那只 arena。
 *
 * 层级由 checker 算好并记在 `Expr.arenaLevel` 上：起点是语句所在的块，值被存进活得更久的
 * 位置时已经提升过；在有家 arena 的函数里，每个站点都已经是 `ARENA_HOME`。
 *
 * 这里没有一处兜底去替 checker 重新判断层级：如果函数有家 arena 就一律返回
 * `(*__extc_home)`，那就成了第二个权威——checker 算一个层级、codegen 又定一个。
 * checker 在闭包建好后按 `FuncDef.arenaSites` 重写每个站点，这里不需要做决定。
 *
 * 调用处会在这个结果外面加 `&`，所以这里给的是 `extc_arena *` 的解引用形式。
 *
 * @param g      生成器状态。
 * @param level  `ARENA_HOME`，或 1 以上的块层级。
 * @return       `(*__extc_home)`（调用者选的、活得最久的那只），或 `__extc_a[level]`
 *               （第 `level` 层块结束时释放）；平行线程链上用线程本地的
 *               `(*extc_tls_arena)`，协程的 step 用帧里的 `arena<level>`。
 */
static const char *arenaRefAt(CG *g, int level) {
    /* parallel::run 的调用链上（worker 与它调用的每个函数）⇒ 用**线程本地** arena：
     * 帧 arena（`__extc_a[]`）属于主线程，两个 worker 同时用就是数据竞争；而 worker 的签名
     * 固定（只写 out、只返回 i64）⇒ 分配逃不出这个 worker ⇒ 线程本地是安全的。 */
    if (g->parTls) return "(*extc_tls_arena)";
    if (level == ARENA_HOME) return "(*__extc_home)";
    /* A step has no C-stack arena array: the block arenas live **in the frame** (one field per level,
     * `arena1`..`arenaN`), because a block in a coroutine can span a suspension -- the allocation has
     * to survive the resume, and the block's own release point still reclaims it. */
    if (g->coroFunc)
        return arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, level > 0 ? level : 1);
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/**
 * @brief 报告 arena 层级与当前正在生成的块之间的一致性问题。
 *
 * 这是给 checker 算出的层级用的漂移探测器：它必须与 codegen 此刻所在的块一致，不一致
 * 就说明两边又开始分头计算层级了。只在环境变量 `EXTC_DBG_ARENA` 置上时生效，不改变任何
 * 输出。
 *
 * 两条判据是单向的、故意放松的：
 *   - 层级不能是 0：0 表示“还没定”，出现就说明 checker 漏了一个站点；
 *   - 层级不能比当前块更深。提升只把分配移向更长的寿命，所以比当前块更浅是合法的。
 *
 * @param g     生成器状态，提供当前块层级与路径。
 * @param e     被检查的分配站点表达式。
 * @param what  站点名字，只用于打印诊断。
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
 * @brief 判断一个块是不是需要有自己的 zone。
 *
 * zone 是一个 `place` 里建出来的池的侧链头。唯一需要它的是 `extc_pool_new`：它把新池
 * 登记到调用时当前的那个 zone 上，没有 zone 就返回 -1 ——而没登记过的池永远不会被丢弃，
 * 它的槽位也永远不会被重用，`extc_pool_live()` 就一直数着一条谁都够不着的记录。所以问题
 * 就是“这个块的代码能不能走到 `extc_pool_new`？”。
 *
 * 只问块自己的语句，不往里走嵌套块：嵌套块本身就是一个地方、有自己的 zone，它为自己
 * 回答。循环体就是这样一个块，好处正在这里 —— 一个只调 `v.push` 的循环体不必每轮迭代
 * 都要自己的 zone。
 *
 * 结论对被调者是可传递的（`FuncDef.makesPool`，由 checker 按最小不动点算出）：库函数对
 * `place` 是透明的 —— 调 `extc_pool_new` 的正是 `vector<i32>::withCap`，而它把池登记到
 * 调用*它*的那个块的 zone 上。
 *
 * 多发是安全方向，少发不是。
 *
 * @param b  即将发出的块语句；NULL 或不是 `ST_BLOCK` 时为假。
 * @return   真表示这个块需要用 `zoneEnter` / `zoneLeaveTo` 对夹起来。
 */
static bool blockMakesPool(Stmt *b) {
    if (!b || b->kind != ST_BLOCK) return false;
    for (size_t i = 0; i < b->u.block.stmts.len; i++)
        if (stmtMakesPool(*(Stmt **)vecAt(&b->u.block.stmts, i), false)) return true;
    return false;
}

/**
 * @brief 把块 arena 释放到某一层（含该层）。
 *
 * 离开一个地方时弹回它开始时的深度（mark），而不是盲目弹一个：早退路径
 * （return/break/continue）因此不会把 zone 栈弄歪。
 *
 * 发射与弹出靠 `zoneMark` 配对，`zoneHere` 只说明“这个函数是个地方”：块的 zone 是按需压
 * 的，所以某层没压却弹一次，弹掉的就是**外层**的 zone —— 外层建的池随后就没有 zone，
 * `extc_pool_new` 返回 -1，记录不再被回收。表在进入块时写、这里读，两者是同一份事实。
 *
 * 区域先放、arena 后放：池的内存是它自己的 malloc 块，两者独立，但释放属于这个块的池
 * 正是这个块的释放点要做的事。只关内存，文件是程序自己的事，所以这个唯一的块释放点
 * 没有任何东西要关。
 *
 * @param g    生成器状态。
 * @param lvl  要释放到的最内层；0 或更小什么都不释放 —— 层级 0 不是块 arena。带
 *             `noArena` 的函数（协程 step 除外）什么都不发：它从不分配。
 */
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    /* A step keeps `noArena` (its prologue has no `__extc_a` and its returns take the direct path),
     * but it still releases block levels -- out of the frame's per-level arenas. */
    if (g->noArena && !g->coroFunc) return;
    /* Memory only: files are the program's business (decision 79), so the one place a
     * block release is emitted has nothing to close. */
    /* Regions first, then the arena: a pool's memory is its own malloc block, so the two are
     * independent, but releasing the pools that belong to this block is what the block's
     * release point is for. */
    /* 离开一个地方：弹回它开始时的深度（mark），而不是盲目弹一个 —— 早退路径
     * （return/break/continue）因此不会把 zone 栈弄歪。
     *
     * 发射与弹出靠 `zoneMark` 配对，`zoneHere` 只说明"这个函数是个地方"：块的 zone 现在
     * 是按需压的，所以某层没压却弹一次，弹掉的就是**外层**的 zone（外层建的池随后就没有
     * zone ⇒ `extc_pool_new` 返回 -1 ⇒ 记录不再被回收）。表在进入块时写，这里读，两者
     * 是同一份事实，不可能走散。 */
    if (lvl < g->levelCap && g->zoneMark[lvl])
        cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lvl);
    /* A coroutine body owns no place of its own, so it must not release the **caller's** arena levels
     * -- that release would free, at every resume, exactly what the next resume still needs. Its own
     * block arenas live in the frame (see `arenaRefAt`), and a block's exit still reclaims its level. */
    /* **块的释放不动回收区域**：区域按 arena 的地址认领，而同一层的兄弟块共用一只 arena、
     * 循环体每轮又复用同一只 —— 在这里把区域丢掉，下一轮 `regionOf` 就新建一个**1 MiB 的
     * slab**，分配与清零的代价落在每一轮上，长循环直接跑不动。区域活到帧销毁
     * （`extc_arena_destroy` 那一处才 `regionDrop`）：块里的对象全部不可达，清扫按压力把它们
     * 收回空闲链，槽于是跨轮复用 —— 内存有界，且每轮没有额外开销。 */
    if (g->coroFunc)
        cgLine(g, "extc_arena_release(&%s->arena%d);", g->coroFrame, lvl);
    else
        cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/**
 * @brief 在一个协程结束处销毁它帧里各块层的 arena。
 *
 * 步骤函数每次 `yield` 都 `return true`，从不走共享尾声；协程体自己那一层活到任务结束而不是活到步骤返回，所以跑完的协程仍持有的层必须在这里释放，且要在驱动的 `extc_task_end` 释放帧本身**之前**。中途被丢弃、从未跑完的协程不会走到这里，它帧 arena 仍持有的块不会在此释放，泄漏量受延迟上界的限制。
 *
 * @param g  代码生成状态；`g->coroFunc` 为空（当前不是协程体）时什么都不发。
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
 * @brief 按源码顺序收集一个函数体里的全部 `@overwrite` 站点。
 *
 * **顺序**是编号稳定的前提：序言按同一顺序为每个站点发一个存储单元，`owIndex` 按位置查站点。域块 `d.run { … }` 的接收者是表达式，交给 `collectOwCallsExpr` 收集。
 *
 * @param s    要遍历的语句；为 NULL 时直接返回。
 * @param out  依次追加每个站点的 `Stmt *`。
 */
static void collectOwSites(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.overwrite) *(Stmt **)vecPush(out) = s;
        return;
    /* `d.run { … }`: the receiver is an expression and the block is a body -- collect both, the
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
 * @brief 返回一个 `@overwrite` 站点在当前函数里的序号。
 *
 * 序号是**查**出来的，不存进 AST：同一个函数体每实例化一个泛型就重走一遍，存下来的序号会被下一个实例覆盖。序言按 `g->owSitesOwn` 的顺序发单元，两侧因此一致。
 *
 * @param g  代码生成状态，查其中的 `g->owSitesOwn`。
 * @param s  站点语句。
 * @return   它在 `g->owSitesOwn` 里的位置；没有登记时为 -1。
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
 * @brief 收集一棵表达式里被调函数需要 `@overwrite` 单元的调用站点。
 *
 * **每一种表达式都要走到**：漏掉一种，那个调用站点就没有单元，被调函数收到 NULL —— 它有兜底，程序仍然有定义，但存储会在每次调用时重新分配而不是复用。`dyn Trait(x)` 的载荷是子表达式，必须进去；各分支以 `default:` 收尾，所以漏掉一种不会触发 `-Wswitch`。
 *
 * @param e    要遍历的表达式；为 NULL 时直接返回。
 * @param out  依次追加每个站点的 `Expr *`。
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
 * @brief 收集一棵语句树里被调函数需要单元的调用站点。
 *
 * 表达式（含 `d.run { … }` 的接收者、条件与被匹配表达式）交给 `collectOwCallsExpr`，语句体继续递归；哪些表达式种类必须走到由 `collectOwCallsExpr` 负责。
 *
 * @param s    要遍历的语句；为 NULL 时直接返回。
 * @param out  依次追加每个站点的 `Expr *`。
 */
static void collectOwCallsStmt(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_YIELD:  collectOwCallsExpr(s->u.yield_.value, out); return;
    case ST_TRAP:   collectOwCallsExpr(s->u.trap_.msg, out); return;
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
 * @brief 返回一个调用站点在当前函数里的序号。
 *
 * 序言按 `g->owCalls` 的顺序发单元，两侧因此一致；站点没有登记时返回 -1，调用点便不传单元，被调函数走兜底。
 *
 * @param g  代码生成状态，查其中的 `g->owCalls`。
 * @param e  调用站点表达式。
 * @return   它在 `g->owCalls` 里的位置；没有登记时为 -1。
 */
static int owCallIndex(CG *g, Expr *e) {
    for (size_t i = 0; i < g->owCalls.len; i++)
        if (*(Expr **)vecAt(&g->owCalls, i) == e) return (int)i;
    return -1;
}

/**
 * @brief 判断当前函数的 `@overwrite` 单元是否留在自己的栈帧里。
 *
 * `main` 与递归函数为真：它们的帧一直活着，答案记在 FuncDef.owLocal。
 *
 * @param g  代码生成状态，读 `g->owLocalOwn`。
 * @param s  未使用；答案属于整具函数，与具体语句无关。
 * @return   单元在本帧内为 true，否则为 false。
 */
static bool f_owLocal(CG *g, Stmt *s) { (void)s; return g->owLocalOwn; }

/**
 * @brief 把一个调用站点的 `@overwrite` 单元实参追加到实参表尾部。
 *
 * 站点没有登记时什么都不追加：被调函数对此有兜底，程序仍然有定义。
 *
 * @param g        代码生成状态，用来查站点序号。
 * @param b        正在拼的实参表缓冲，不含右括号。
 * @param e        调用站点表达式。
 * @param nargs    表里已有的实参数，决定要不要前置逗号。
 * @param hasHome  是否已经追加过 home arena 实参；它也影响逗号。
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
 * @brief 返回一个块内部最深的块层号。
 *
 * @param block  块语句；`NULL` 或不是 `ST_BLOCK` 时结果同 `blkMaxLevel(block)`，也就是 0。
 * @return       块内部达到的最深层号，不含块自己加的那一层；没有块时为 0。
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
 * @brief 返回一条语句内部达到的最深块层号。
 *
 * 块自己算一层，其中的循环、条件与 `match` 各自再加层，所以返回值就是包住它的函数需要的 arena 槽数（`__extc_a` 的长度由此决定）。
 *
 * @param s  语句；为 NULL 时为 0。
 * @return   块为 `1 + blkMaxOfBlock(s)`；`if`/`match` 取各分支的最大值；条件会分配空间的 `while` 比它的体块多一层。
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
 * @brief 发出一个块的体：进入时清掉它那一层的 arena，退出时释放。
 *
 * 循环体也是块，所以每一轮进入都清一次 —— 循环占用的内存因此被限制在一轮，而不是迭代次数。
 *
 * @param g      代码生成状态。
 * @param block  块语句，其语句按顺序发出；调用点保证它是 `ST_BLOCK`（有断言）。
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
    /* 静态根表的块边界：进入时记下当前登记深度，离开时截回去 —— 块里的局部量出了块就不再
     * 是根（机制文档第三节第一条：触发点上的活槽由编译器给）。 */
    *(size_t *)vecPush(&g->rcRootMarks) = g->rcRoots.len;
    if (!g->noArena) {
        if (!g->coroFunc) {  /* a coroutine's storage is the task's, not this block's */
            /* 进入块只清 arena（它的块留给下一轮复用）；回收区域不在这里丢，理由见
             * `cgReleaseLevel`：每轮丢一次区域就是每轮新建一个 1 MiB slab。 */
            cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);   /* clear */
        }
        /* 进入一个地方：压一个 zone，标记按层号命名（同级块不嵌套，名字不冲突）。
         * 只在这个块**可能建池**时才压（`blockMakesPool`）：只调用 `v.push` 这类不建池的
         * 库函数的循环体不需要自己的 zone，省下每轮一次的压/弹。
         * 弹出发射与否记进 `zoneMark`，`cgReleaseLevel` 查同一张表。 */
        g->zoneMark[g->blkLevel] = false;
        if (g->zoneHere && g->blkLevel > 1 && blockMakesPool(block)) {
            if (!g->coroFunc)      /* see the note on the release below: the task owns the place */
                cgLine(g, "int64_t __extc_zm%d = extc_pool_zoneEnter();", g->blkLevel);
            g->zoneMark[g->blkLevel] = true;
        }
    }
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt *st = *(Stmt **)vecAt(&block->u.block.stmts, i);
        /* The `for` step: `continue` jumps here, so the label has to be immediately in front of
         * it. Emitted from the same walk that emits the statement, which is what keeps the label
         * and the `goto` in agreement (the `goto` is only emitted when ST_WHILE found this exact
         * statement in the block). */
        if (planForStep(block) && st == planForStep(block) && g->loopLen > 0 && g->loopStep[g->loopLen - 1])
            cgLine(g, "__extc_forstep_%d: ;", g->loopStep[g->loopLen - 1]);
        genStmt(g, st);
    }
    cgReleaseLevel(g, g->blkLevel);
    g->zoneMark[g->blkLevel] = savedZoneMark;
    /* 离开块：这个块声明的引用槽不再是根。 */
    assert(g->rcRootMarks.len > 0);
    g->rcRoots.len = *(size_t *)vecAt(&g->rcRootMarks, g->rcRootMarks.len - 1);
    g->rcRootMarks.len--;
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/**
 * @brief 发出一条语句，允许在它前后带语句前缀。
 *
 * 语句需要提前求值的东西写进前缀；`prefixBlk` 非零表示正处在一条语句内部，前缀机制据此允许往那里发代码。语句本身由 `genStmtInner` 发出。
 *
 * @param g  代码生成状态。
 * @param s  要发出的语句。
 */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

/**
 * @brief 这个槽是不是引用类型（`ref T` / `?ref T`）？
 *
 * 计数的判据只有这一条：**只挂在"指向关系改变"上**。数据字段赋值（`p.val = x`）一律
 * 不动计数，所以先问类型再谈发射。实例化上下文里的 `T` 先替换掉，否则泛型实例会拿
 * 参数类型去比 `TY_REF`。
 *
 * @param g  生成器状态，提供实例化上下文。
 * @param t  槽的声明类型；可以为 NULL。
 * @return   是引用类型为 true。
 */
static bool cgIsRefType(CG *g, Type *t) {
    Type *s = subst(g, t);
    return s && s->kind == TY_REF;
}

/**
 * @brief 一个**纯地方**表达式：重复求值安全（没有调用、没有下标求值）。
 *
 * 跨区登记要在同一处再求值一次"装着这条槽的对象"（`a->f = v` 要多传一个 `a`），所以只认
 * 绑定、字段链与解引用：它们的求值没有副作用，重复读同一个值。带下标的投影（`a[i]`）要把
 * 下标也重复求值才安全，代价大于收益，直接不登记 —— 那些位置由触发点的根表覆盖。
 *
 * @param g  生成器状态。
 * @param e  候选表达式。
 * @return   是纯地方时为 true。
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

/* 一条引用槽的宿主：装着这条槽的那个**本运行期对象**。 */
typedef enum {
    RC_HOST_OBJ,      /* 找到了宿主对象表达式（还要过 cgPurePlace 才能重复求值） */
    RC_HOST_LOCAL,    /* 槽在静态声明的存储里（局部/全局/形参）：触发点的根表已覆盖 */
    RC_HOST_UNKNOWN   /* 宿主追不到（视图元素、调用结果……）：必须走保守路径 */
} RcHost;

/**
 * @brief 沿**值投影**找一条引用槽的宿主对象。
 *
 * 跨区登记位要置在**目标**对象上，而"来源"是装着这条槽的对象：`a->f` 是 `a`、`*p` 是 `p`、
 * 内联数组元素 `w.arr[i]` 是 `w`（元素存储就在对象载荷里，与下标无关）、`ref [N]T` 的
 * `p[i]` 是 `p`。所以这里只沿**值字段**（`EX_FIELD` 里基类型不是引用的那一层）、内联数组与
 * `ref [N]T` 的元素往外走，走到一个 `ref` 基或解引用为止。
 *
 * 三态的理由：追不到宿主时**绝不能照常清扫**（种子不完整会提前释放）。调用方按返回值选路：
 * `RC_HOST_OBJ` 登记、`RC_HOST_LOCAL` 什么都不做（根表覆盖）、`RC_HOST_UNKNOWN` 走保守路径。
 *
 * @param g    生成器状态（要 `subst` 泛型实参）。
 * @param t    赋值的目标表达式。
 * @param obj  出口：`RC_HOST_OBJ` 时的宿主对象表达式。
 * @return     见 `RcHost`。
 */
static RcHost cgSlotHost(CG *g, Expr *t, Expr **obj) {
    *obj = NULL;
    for (;;) {
        if (!t) return RC_HOST_UNKNOWN;
        switch (t->kind) {
        case EX_DEREF:
            /* `*p = v`：槽就是 p 指着的那个对象的载荷。 */
            *obj = t->u.deref.operand;
            return RC_HOST_OBJ;
        case EX_FIELD: {
            Expr *b = t->u.field.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            /* `p.f = v`（p 是引用，字段访问自动解引用）：槽在 *p 的载荷里。 */
            if (bt && bt->kind == TY_REF) { *obj = b; return RC_HOST_OBJ; }
            /* 值字段：槽的存储仍属于 b 的宿主，继续往外走。 */
            t = b;
            continue;
        }
        case EX_INDEX: {
            Expr *b = t->u.index.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            if (bt && bt->kind == TY_REF) {
                Type *bi = bt->inner ? ttBase(subst(g, bt->inner)) : NULL;
                /* `p[i] = v` 且 p 是 `ref [N]T`：元素内联在 *p 里。 */
                if (bi && bi->kind == TY_ARRAY) { *obj = b; return RC_HOST_OBJ; }
                /* ref 到视图：元素在视图 data 指向的存储里，追不到。 */
                return RC_HOST_UNKNOWN;
            }
            /* 内联数组（值）：元素存储属于数组的宿主。 */
            if (bt && bt->kind == TY_ARRAY) { t = b; continue; }
            /* 视图元素：data 可能指到板/池/栈/C 存储，追不到宿主。 */
            return RC_HOST_UNKNOWN;
        }
        case EX_IDENT:
            /* 变量自己的存储：静态根表把它（连同值字段/内联数组）登记成根。 */
            return RC_HOST_LOCAL;
        /* kind-default: 追不到宿主的投影一律按 UNKNOWN（安全方向）。新 kind 若可能成为
         * 引用槽的宿主，必须在这里加 case，不能靠 default 混过去。 */
        default:
            return RC_HOST_UNKNOWN;
        }
    }
}

/**
 * @brief 整值存储携带引用叶子时，**逐叶**发引用槽的记账。
 *
 * `s.w = wrap { n: keeper }`、`var b: wrap = wrap { n: keeper }` 这类**按值**的整块拷贝
 * （结构体 / 定长数组）里可能藏着引用叶子，而 C 的整块赋值是一条裸复合字面量拷贝
 * （`s->w = (wrap){.n = keeper};`）：它**既不 `inc`、也不登记跨区边**。于是槽被写进去了却
 * 没记账 —— 一条路是计数根本不知道这个持有者（唯一可见持有者一放手就归零回收，槽成悬垂），
 * 另一条路是清扫的种子不全（跨区入边没登记 ⇒ 目标区域的清扫把还被指着的对象当垃圾放掉，
 * 与 `cross_region_elem` 同一个洞的另一半）。这里把一整块拆回**逐条引用槽赋值**：新槽走
 * `initRef`（只写 + 加一，绝不读旧值），覆盖槽走 `storeRefOwned` / `storeRefUnknown`
 * （读旧减一、新值加一；宿主证不出来时让目标区域退出清扫）—— 与直接引用槽赋值同一条路。
 *
 * 调用方负责：目标与值**各求值一次**（引用进临时量），因为每条记账都会把路径再拼一次。
 *
 * @param g          生成器。
 * @param t          整值存储的类型（已 `subst` 过的调用方上下文里取）。
 * @param dstBase    目标存储的 C 左值表达式（如 `(*__extc_svd3)`、`b`）。
 * @param srcBase    值的 C 左值表达式（如 `__extc_svv3`）；`init` 为真时不读。
 * @param init       true = 新槽（`var ...` 的初始化）：只加一；false = 覆盖：读旧减一。
 * @param owner      "装着这条槽的对象"（同 `storeRefOwned` 第三个实参）；局部/静态槽传 `0`。
 * @param hostUnknown 宿主证不出来 ⇒ 每条叶子走保守路径（目标区域退出清扫）。
 */
static void cgValueRefStore(CG *g, Type *t, const char *dstBase, const char *srcBase,
                            bool init, const char *owner, bool hostUnknown) {
    /* 只收一次**相对**路径（`.n`、`.kids.data[0]`、顶层引用是空串），两个基各拼一次 ——
     * 目标与值的叶子集合按构造必然同形，不会有一条只在一边出现。 */
    Vec rel;
    vecInit(&rel, g->arena, sizeof(const char *));
    rcLeafPaths(g, t, "", &rel, 0);
    for (size_t i = 0; i < rel.len; i++) {
        const char *p = *(const char **)vecAt(&rel, i);
        const char *d = arenaPrintf(g->arena, "%s%s", dstBase, p);
        if (init) {
            /* 值已经在目标里（声明就是初始化），自读自写一次：只写 + 目标加一。 */
            cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", d, d);
        } else if (hostUnknown) {
            cgLine(g, "extc_rc_storeRefUnknown((void **)&(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p));
        } else {
            cgLine(g, "extc_rc_storeRefOwned((void **)&(%s), (void *)(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p), owner);
        }
    }
}

/**
 * @brief 在**帧销毁**处发射区域释放（`extc_arena_destroy` 旁边）。
 *
 * 认领键是 arena 的地址（见 `emitRuntimePrologue` 里那张表），所以这里只发地址，不需要
 * 知道区域本身。协程的 step 用的是帧里的 `arena<k>`，拼法与 arena 的释放保持一致。
 * 分片时一个逻辑区域认领了 N 只运行期区域（键 = 地址 + k），所以这里要**逐片丢弃** ——
 * 漏掉一片就等于那一片的 slab 与登记位永不释放。
 *
 * 只在帧销毁处调用：块释放（`cgReleaseLevel`）不丢区域，否则循环体每轮都会新建一个 1 MiB
 * 的 slab。区域比块活得久是安全的 —— 块里的对象全部不可达，清扫按压力收回它们。
 *
 * @param g    生成器状态；`needReclaim` 关着时什么都不发。
 * @param lvl  块层级（1 = 函数体）。
 */
static void cgReclaimDropArena(CG *g, int lvl) {
    if (!g->needReclaim) return;
    if (g->rcNShard <= 1) {
        /* 单片：与接线前逐字节相同。 */
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
 * @brief 发出 `ST_VAR`：普通声明、协程 spawn、`@overwrite` 单元与 `f()?` 初始化的声明。
 *
 * 协程 spawn 分支把帧当成本地的普通值，参数就地初始化形参（恢复时没有参数可再传一次）；带 `var h: coroutine<T>` 注解时帧放进任务自己的地方，普通写法把帧留在调用者栈上、零分配。`@overwrite` 分支只保留一块存储：首次执行时惰性分配，之后只清零复用。协程体内变量不是 C 局部量而是帧的字段，所以初始化发成赋值。
 *
 * @param g  代码生成状态。
 * @param s  变量语句。
 */
static void stVar(CG *g, Stmt *s) {
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
        /* 协程帧里可能有引用槽，而静态根表不枚举帧 ⇒ 本函数的触发一个都不发（安全方向）。 */
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
    /* 帧是值局部量，可能含引用槽；静态根表不枚举合成帧 ⇒ 本函数关掉触发（安全方向）。 */
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
            const char *v = genExpr(g, s->u.var.init);
            flushPrefix(g);
            /* 帧字段是**既有槽**（帧是复合字面量，指针字段先读作 0），resume 会重跑这条
             * 初始化 ⇒ 覆盖语义，走 storeRef：旧值减一、新值加一。 */
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
/* `global var x: T` / `global let x: T = c`：发 **C 的块作用域 `static`** ——
 * 静态存储期 + 名字只在这个块里可见，语义正好对上。
 * 额外的两个好处：这个声明被**重复执行**也没关系（循环、协程 resume 都跳到语句中间），
 * C 保证静态对象只在程序启动时初始化一次 ✓。初始化式必须是常量，检查器已经拦住。 */
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
            /* 回收打开时这一份存储也从 rc 区域取：`@overwrite` 的绑定同样可以是引用类型，
             * 落在 arena 里的指针进计数函数会读到垃圾。区域按 cell 的家 arena 认领，
             * 与 arena 同寿。 */
            if (g->needReclaim) {
                /* 类型描述符 + 清扫触发，落在分配那条语句之前：这份存储也从 rc 区域取，
                 * 描述符随槽存下，触发用的区域认领键与下面的 `extc_rc_alloc` 一致（同一片号，
                 * 见 `rcAllocCore`）。 */
                const char *td = rcEmitTypeDesc(g, st_t);
                const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)%s->home", owcv),
                                               planArenaLevel(nx),
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
        /* `@overwrite` 的绑定也是引用槽：出了这一条语句它活着，直到块结束。 */
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
    cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, tryPayloadPath(g, &ti));
    localDef(g, lb, nm, 1);
    rcRootDecl(g, s->type, nm);
    return;
}
const char *init = s->u.var.init ? genExpr(g, s->u.var.init)
                                 : zeroInit(g, s->type);
flushPrefix(g);
size_t lb = g->out->len;
cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, asF32(g, s->type, s->u.var.init, init));
localDef(g, lb, nm, 1);
/* 引用槽的计数。**新槽**走 initRef：它只写 + 目标加一，**绝不读旧值** —— 一个刚声明的
 * 槽里躺着的"旧值"是空闲链的槽号（slab 分配器的空闲链就存在槽头），当指针解引用会崩。
 * 值从刚写好的变量里再读一次，所以求值只有一次。
 *
 * 全局声明跳过：它是 `static`，语句每次执行到这个位置都会**再跑一遍**，计数会被重复加；
 * 而且全局引用是进程级根，归根表那一批（留待下一步）。
 *
 * 跨区存储（对象在更外层的区域、槽在这个块里）再登记一位：目标对象所在位置置一位，
 * 清扫播种时按位图顺序扫。多登记是安全方向（只让对象多活一会儿），少登记才是漏根。 */
if (g->needReclaim && !s->u.var.isGlobal && cgIsRefType(g, s->type)) {
    cgLine(g, "extc_rc_initRef((void **)&%s, (void *)%s);", nm, nm);
} else if (g->needReclaim && !s->u.var.isGlobal && s->u.var.init) {
    /* **整值**声明的引用叶子（`var b: wrap = wrap { n: keeper }`）同样要记账：裸的 C 初始化
     * 一条 `inc` 都不发，`b.n` 就是"槽写了但没记账"的又一个出口（与赋值那条同一个洞）。
     * 没有初始化式时值是**零**（`zeroInit`）⇒ 每个引用叶子都是 null，`inc` 无事可做，
     * 整片 `initRef` 只会在生成物里白占几百行（`world` 那种 64+1024 槽的局部量）。 */
    RcScan vsc = rcScanType(g, s->type, 0);
    if (vsc == RC_LEAF_REFS) {
        cgValueRefStore(g, s->type, nm, nm, true, "0", false);
    } else if (vsc == RC_CONSERVATIVE) {
        /* 叶子枚举不出来（枚举负载 / 泛型实参带引用）：逐字当候选引用，只加不减。局部量这一档
         * 没有可撤销的来源、根表也枚举不出这些叶子 ⇒ `hostKnown = 0`，目标区域退出清扫。 */
        cgLine(g, "extc_rc_storeStructCons((void *)&(%s), sizeof(%s), (void *)0, 0);",
               nm, cType(g, s->type));
    }
}
/* 这个声明从此刻起是**根**：触发点的根表要列出它（结构与定长数组按叶子引用槽展开）。
 * 全局声明也登记：它是块作用域 `static`，在这一块里看得见，多留一会儿是安全方向。 */
rcRootDecl(g, s->type, nm);
return;
}

/**
 * @brief 发出 `ST_WHILE`：普通 `while (...)` 或自占一层的 `while (1)` 形状。
 *
 * 条件求值可能把临时量写进语句前缀（数组 `==` 的右操作数、接收者或 `??` 主语）。把前缀冲在 `while (...)` **前面**只会求值一次，而条件每轮都读那个临时量，`while a == mk()` 就会永远拿第一次的值比较；有前缀时条件因此挪进循环体，求值次数与源码一致，体不动。无前缀（绝大多数情况）保持 `while (cond) {`。
 *
 * 条件自己会分配空间时它单独占一层 arena，在每轮开头释放；`g->blkLevel` 在整个循环期间抬高，让条件与体和检查器分配到的层一致（`blkMaxLevel` 对这种形状多算一层）。
 *
 * `for` 的步进语句记在体块上；步进仍在体内时给这个循环一个标签 id，`genBlockBody` 发标签、`ST_CONTINUE` 跳过去。
 *
 * @param g  代码生成状态。
 * @param s  `while` 语句；体必须是块。
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
if (condTemps || ownLevel) {
    cgLine(g, "while (1) {");
    g->indent++;
    if (ownLevel) g->blkLevel++;
    /* 条件自己的那一层每轮清 arena；回收区域留到帧销毁（见 `cgReleaseLevel`）。 */
    if (ownLevel) cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);
    flushPrefix(g);               /* the temporaries, now once per round */
    cgLine(g, "if (!(%s)) break;", cnd);
} else {
    cgLine(g, "while (%s) {", cnd);
    g->indent++;
}
g->loopLevel[g->loopLen] = g->blkLevel + 1;   /* the body is the next level */
g->loopStep[g->loopLen] = stepLabel;
g->loopLen++;
genBlockBody(g, s->u.whiles.body);
g->loopLen--;
if (ownLevel) g->blkLevel--;
g->indent--;
cgLine(g, "}");
return;
}

/**
 * @brief 发出 `ST_MATCH`：分支发成 if/else 链，需要时先绑定载荷。
 *
 * 被匹配表达式只求值一次（枚举带载荷时先落进临时量），因为分支体读 `.u.<variant>`，第二次求值会有副作用。泛型实例里先把记录的类型 `subst` 成实例类型，否则分支会拿模板类型去比一个不存在的常量。
 *
 * 用 if/else 链而**不是**变体常量上的 `switch`：分支体里的 `break`/`continue` 属于外层循环，在 `switch` 里只会跳出 switch。检查器保证 match 穷尽，所以不需要收尾的 `else`。
 *
 * @param g  代码生成状态。
 * @param s  `match` 语句。
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
 * No final `else` is needed: the checker guarantees that the match
 * is exhaustive, and a missing variant would not compile at all. */
flushPrefix(g);
for (size_t i = 0; i < s->u.match.arms.len; i++) {
    MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
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
        /* 负载绑定是**这一支里**的 C 局部量：它的作用域只到 `if` 块结束，静态根表按块登记的
         * 机制覆盖不到它。因此只要绑定的类型含引用槽，本函数就按"根表不完整"处理，触发一个
         * 都不发（安全方向）。 */
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
 * @brief 发出语句本身，是真正的分派表；前缀记账在 `genStmt` 里。
 *
 * `ST_VAR`、`ST_WHILE`、`ST_MATCH` 分别由 `stVar`、`stWhile`、`stMatch` 发出，这张表因此读起来像一张表。
 *
 * @param g  代码生成状态。
 * @param s  要发出的语句。
 */
static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_TRAP: {
            /* `trap(msg)`: **同一个出口**，编译器自己插的那些 trap 走的就是它 ——
             * `extc_trapMsg` 印 `文件:行号: trap: 消息` 到 stderr，然后 `extc_die(1)`
             * （临终钩子照跑 ⇒ 缓冲输出不丢）。也就是说"程序坏了"在这门语言里
             * 只有一种说法，不管那句话是编译器说的还是写代码的人说的。 */
            Expr *m = s->u.trap_.msg;
            /* Nothing from `rt`/`rtPrint` here: `extc_trapMsg` and `extc_die` are primitives of
             * the runtime block, and `trap` prints no structured payload. */
            if (m->kind == EX_STR) {
                /* 字面量：走短路径，消息作为**参数**而不是格式串（用户的消息里有 `%` 也安全）。 */
                cgLine(g, "extc_trapMsg(\"%s\", %d, \"%s\");", g->path, s->line, m->u.str.text);
                return;
            }
            Type *mt = m->type;
            if (mt && mt->kind == TY_REF) {
                cgLine(g, "extc_trapMsg(\"%s\", %d, (const char *)(%s));",
                       g->path, s->line, genExpr(g, m));
                return;
            }
            /* 运行时切片：求值一次（可能有副作用）再印长度那么大一片。 */
            const char *t = arenaPrintf(g->arena, "__extc_trap%d", g->tmpSeq++);
            cgLine(g, "%s %s = %s;", cType(g, mt), t, genExpr(g, m));
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: %%.*s\\n\", \"%s\", %d,"
                      " (int)(%s).len, (const char *)(%s).data);",
                   g->path, s->line, t, t);
            cgLine(g, "extc_die(1);");
            return;
        }
        case ST_DOMAIN: {
            /* `d.run { … }`: the block ends by **running its tasks to completion** -- that is what
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
             * (`ext f(x)` ⇒ `extc_dom_add`) and what the caller's locals hold. Only then does the
             * domain drive them to completion -- "the block cannot end while a task it started is
             * still unfinished" is exactly this order, and it is also why the drive call belongs
             * **inside** the block: each `ext` declares its frame as a local of this block
             * (`EX_EXT`), so running after the closing brace would hand the domain pointers to
             * stack slots that are already out of scope. */
            Stmt *body = s->u.domain_.body;
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
            g->inAssignTarget = true;
            const char *tgt = genExpr(g, s->u.assign.target);
            g->inAssignTarget = false;
            const char *val = genExpr(g, s->u.assign.value);
            /* 引用槽的赋值是**覆盖**语义（槽里已经有旧值），所以走 storeRef：读旧值减一、
             * 新值加一。数据字段（`p.val = x`）不是引用类型 ⇒ 一个字都不发。
             *
             * 跨区边（`a->f = v` 且 `a` 与 `v` 不在同一区域）**要登记**：目标区域的清扫从
             * 根 + 登记位播种，堆→堆的跨区入边没有别的地方能到。登记位的清理由来源侧记账
             * 负责（`rscnt` 计数 + 来源对象/来源区域死亡时撤销），所以它**不是粘性的** ——
             * 只置不清会让循环里每轮新建的对象永远当种子（内存随轮数线性涨）。
             *
             * 第三个实参是"装着这条槽的对象"：`a->f` 传 `a`，`*p` 传 `p`，局部变量/全局的槽
             * 传空（那些是根，触发点的根表覆盖）。对象表达式必须是**纯地方**，否则会在同一处
             * 求值两次（`f()->f = v` 会把 `f()` 调两次）。 */
            const bool refStore = g->needReclaim && !s->u.assign.op
                                  && cgIsRefType(g, s->u.assign.target->type);
            /* **整值**存储（结构体 / 定长数组按值拷贝）：目标槽本身不是引用类型，可它的载荷里
             * 可能藏着引用叶子（`s.w = wrap { n: keeper }`）。这种赋值不能再走裸的 C 整块拷贝
             * —— 那样槽被写进去却没有 `inc`、也没有跨区登记（`a7e2642` 是同一类"槽写了但没
             * 记账"，只是走另一个机制）。三态与引用槽一致：枚举得出来 ⇒ 逐叶走
             * `storeRefOwned`/`storeRefUnknown`；枚举不出来（枚举负载、泛型实参带引用）⇒
             * 逐字保守路径（只加不减 + 登记/退出清扫）。 */
            Type *rawTT = s->u.assign.target ? s->u.assign.target->type : NULL;
            const RcScan valScan = (g->needReclaim && !s->u.assign.op && !refStore && rawTT)
                                       ? rcScanType(g, rawTT, 0) : RC_NO_REFS;
            const bool valueStore = valScan != RC_NO_REFS;
            const char *owner = "0";
            bool hostUnknown = false;
            bool hostProven = false;   /* 宿主是**追到区域**的对象（不是局部/静态槽） */
            if ((refStore || valueStore) && s->u.assign.target) {
                Expr *t = s->u.assign.target;
                /* 宿主的三种结局（见 `cgSlotHost`）：纯地方基 ⇒ 登记跨区边；静态声明的
                 * 存储 ⇒ 根表覆盖；追不到 ⇒ 保守路径。`c.a = v`（`c` 是结构体**值**）里的
                 * 槽属于局部/arena，不是本运行期的槽；`a[i]` 的容器若在内联数组里（`w.arr[i]`）
                 * 宿主就是 `w`，若在视图里则追不到。 */
                Expr *base = NULL;
                RcHost h = cgSlotHost(g, t, &base);
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
                if (s->u.assign.opExpr && planCallee(s->u.assign.opExpr))
                    cgLine(g, "%s = %s;", tgt, genBin(g, s->u.assign.opExpr));
                else
                    cgLine(g, "%s %s %s;", tgt, s->u.assign.op, val);
                return;
            }
            if (refStore) {
                if (hostUnknown) {
                    /* 宿主证不出来：这条槽必须仍然是种子，但静态根表里没有它、来源侧也无从
                     * 撤销。于是把**目标所在的区域**标成"不清扫"（安全方向：宁可少收）——
                     * 区域从此只靠计数归零回收，绝不在种子不完整时照常扫。 */
                    cgLine(g, "extc_rc_storeRefUnknown((void **)&(%s), (void *)(%s));", tgt, val);
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
                    /* 叶子枚举不出来：整块先拷（语义），再把每个"是活槽"的字加一次计数，
                     * 并登记到目标区域或让它退出清扫（见 `extc_rc_storeStructCons`）。
                     * 只有**证出区域**的宿主才走登记；局部/静态槽这一档没有可撤销的来源，
                     * 而保守类型的局部量连根表都枚举不出来 ⇒ 走保守侧（目标区域退出清扫）。 */
                    cgLine(g, "{ %s *%s = &(%s); %s %s = %s; *%s = %s;"
                              " extc_rc_storeStructCons((void *)%s, sizeof *%s, (void *)(%s), %d); }",
                           ct, dv, tgt, ct, sv, val, dv, sv, dv, dv, owner, hostProven ? 1 : 0);
                    return;
                }
                /* 枚举得出来：目标与值各求值一次（`genExpr` 有副作用也只跑一次），然后逐叶
                 * 走引用槽的覆盖语义，最后整块拷贝补齐非引用字段。 */
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
            const char *v = genExpr(g, s->u.ret.value);
            flushPrefix(g);              /* must precede the return */
            cgReturn(g, v);
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            /* `break` and `continue` leave these block levels behind, so the
             * arenas are released first, down to the level of the loop body.
             * The body's own arena is cleared when the next iteration enters
             * it. */
            int to = g->loopLen ? g->loopLevel[g->loopLen - 1] : 1;
            for (int lv = g->blkLevel; lv >= to; lv--) cgReleaseLevel(g, lv);
            if (s->kind == ST_BREAK) { cgLine(g, "break;"); return; }
            /* `continue` in a `for` must still run the step (C's `for` does); the desugared
             * `while` has it at the end of the body, behind a label. */
            int step = g->loopLen ? g->loopStep[g->loopLen - 1] : 0;
            if (step) cgLine(g, "goto __extc_forstep_%d;", step);
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
            const char *ex = genExpr(g, s->u.expr.expr);
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
 * @brief 拼出一个函数的 C 形参表。
 *
 * 分配可能逃逸的函数在末尾多收一个隐藏的 arena 指针：其中的 `new` 分配到调用者选定的 arena。`@overwrite` 单元以不透明的 `extc_owcell *` 同样从末尾传进来，调用者因此不必知道被调函数站点的类型，泛型实例也不需要特例。`main` 的 C 签名固定，不收隐藏形参；语言写的参数（`main(args)`）也不是 C 形参。
 *
 * 隐式形参只在 `cgImplicitArgs` 里发**一次**（`usesHome` 与 `makesPool` 各一个）；这里再补一手会让只建池的函数多出一个形参。
 *
 * @param g  代码生成状态。
 * @param f  要拼形参表的函数定义。
 * @return   形参表文本，不含外层圆括号。
 */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* C fixes the signature of `main`, so it takes no hidden parameter; its
     * home arena is the local `extc_arena *__extc_home = &__extc_a[1]` in its
     * own body. A parameter the language wrote is not a C parameter either:
     * `main(args)` is built from `argc`/`argv` in the prologue, so C still sees
     * exactly the two arguments it insists on. */
    if (!f->owner && strcmp(f->name, "main") == 0) {
        bufPuts(&sig, f->params.len ? "int argc, char **argv" : "void");
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
    cgImplicitArgs(g, &sig, f, NULL, f->params.len, true);   /* 声明拼法，同一个主人 */
    /* 建池的函数多收一个隐藏的「家 zone」——arena 那个隐藏参数的镜像
     *（`makesPool` 是可传递的最小不动点，所以整条链上每个函数都收得到）。
     * 容器"生在哪里"因此由**调用者**决定，而不是由库内部那一层词法块决定。 */
    /* 注意：**不要**在这里再补一次 `makesPool` —— 上面那一次 `cgImplicitArgs` 已经把两种隐式形参
     * 都发了（`usesHome` 与 `makesPool` 各一次）；这里多写一手就是"只建池的函数多一个形参"，
     * 产物会变成非法 C。 */
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
 * @brief 判断一条语句是否**一定**会返回。
 *
 * 只认最简单的形状：`return` 本身，或最后一条语句是这种形状的块。
 *
 * @param s  语句；为 NULL 时为 false。
 * @return   确定会返回时为 true；拿不准一律 false。这个方向是刻意的：漏判只多发一个 `return`（死代码但能编译），反过来会留下缺失的 return，gcc 报错。
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
 * @brief 初始化一个空的函数指针集合，槽数组取自 arena。
 *
 * 容量固定为 64，且必须是 2 的幂：查找与插入都用 `cap - 1` 做掩码定位。
 * 集合本身不释放；扩容换下来的旧槽数组留在同一个 arena 里。
 *
 * @param s  要初始化的集合，原有内容被覆盖。
 * @param a  分配槽数组的 arena。
 */
static void funcSetInit(FuncSet *s, Arena *a) {
    s->cap = 64; s->len = 0;
    s->slot = arenaAllocZero(a, s->cap * sizeof *s->slot);
}
/**
 * @brief 把一个函数指针散列成集合的散列值。
 *
 * 指针按 16 字节对齐，先右移 4 位丢掉恒定为零的低位，再乘一个奇数散开。
 *
 * @param p  函数定义指针。
 * @return   散列值；调用方用 `cap - 1` 掩码截成槽下标。
 */
static size_t funcSetHash(FuncDef *p) { return ((size_t)(uintptr_t)p >> 4) * 2654435761u; }
/**
 * @brief 判断某个函数指针是否已在集合里。
 *
 * 开放寻址加线性探测：走到空槽就说明不在。前提是 `s` 已由 `funcSetInit`
 * 初始化，且表里始终有空槽——`funcSetAdd` 在装载率达到一半时就会扩容来保证。
 *
 * @param s  已初始化的集合。
 * @param p  要找的函数指针。
 * @return   在集合里为 true，否则 false。
 */
static bool funcSetHas(FuncSet *s, FuncDef *p) {
    for (size_t i = funcSetHash(p) & (s->cap - 1); ; i = (i + 1) & (s->cap - 1)) {
        if (!s->slot[i]) return false;
        if (s->slot[i] == p) return true;
    }
}
/**
 * @brief 把集合容量翻倍，并把全部元素重新散列进新槽数组。
 *
 * @param s  已初始化的集合，容量与槽数组就地更新。
 * @param a  分配新槽数组的 arena；旧数组不回收。
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
 * @brief 把一个函数指针加入集合；已经在集合里就什么都不做。
 *
 * 容量到一半满时先扩容，所以探测总能遇到空槽。
 *
 * @param s  已初始化的集合。
 * @param a  扩容时用的 arena。
 * @param p  要加入的函数指针。
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
 * @brief 从 `f` 出发深度优先遍历调用图，把落在环上的函数标进 `g_cycOn`。
 *
 * 灰色表示还在当前递归栈上，黑色表示已结束。一条指向灰色节点的边就是回边，
 * 此时把 `g_cycStack` 里从该节点到栈顶的整段都标成「在环上」。颜色跨根节点
 * 保留，对判环是安全的：一个环要么已被更早的根找到，要么所有节点仍是白色。
 * `g_cycStack` 必须已由 `cycleSaysRecursive` 初始化。
 *
 * @param g  代码生成上下文，同时提供 arena。
 * @param f  遍历起点；为 NULL 或没有函数体（extern 声明）时直接返回。
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
 * @brief 判断一个函数是否落在调用图的某个环上，即直接或间接递归到自己。
 *
 * 首次调用初始化四个集合与遍历栈，之后对没见过的 `f` 补一次从它出发的遍历；
 * 查询本身只是查 `g_cycOn`。状态保存在文件级静态变量里，都从 `g->arena` 分配。
 *
 * @param g  代码生成上下文，同时提供 arena。
 * @param f  要查询的函数。
 * @return   落在环上为 true，否则 false。
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
 * @brief 判断一个函数是否会直接或间接调用到自己。
 *
 * 答案决定要不要发递归深度守卫。判定只用检查期已记好的调用图（`f->callees`），
 * 不再走一遍 AST，而且是传递的：互相递归的 `g -> h -> g` 里，`g` 与 `h` 都算。
 *
 * @param g  代码生成上下文。
 * @param f  要判断的函数。
 * @return   有路径回到自身为 true；`f` 为 NULL 或没有函数体时为 false。
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
 * @brief 判断一组类型实参是否还提到类型参数，即实例尚未具体化。
 *
 * 口径与检查期判 provisional instance 的一致：只要有一个实参提到类型参数，
 * 这个实例（`gen_T`）就不发射。
 *
 * @param targs  类型实参列表；为 NULL 视为不含类型参数。
 * @return       有实参提到类型参数为 true，否则 false。
 */
static bool coroProvisional(Vec *targs) {
    if (!targs) return false;
    for (size_t i = 0; i < targs->len; i++)
        if (mentionsParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/**
 * @brief 判断一个协程是否真的会发射成 C 函数。
 *
 * 只有具体实例才发射：自身还有类型参数的泛型模板不发射，类型实参仍提到类型参数的
 * provisional 实例（`gen_T`）也不发射。实例必须发射——`genFunc` 生成的就是实例，
 * 句柄调度表按它生成的 `$step`/`$next` 分派。
 *
 * @param f  候选函数。
 * @return   是协程且已具体化为 true；`f` 为 NULL、不是协程或还是模板时为 false。
 */
static bool coroEmitted(FuncDef *f) {
    if (!f || !planIsCoro(f)) return false;
    if (!planTemplate(f)) return f->typeParams.len == 0;
    return planTemplate(f)->typeParams.len == 0 ? true : !coroProvisional(&f->targs);
}

/**
 * @brief 发射协程句柄类型所需的前置声明与两族辅助函数。
 *
 * `g->needCoroHandle` 为假时什么都不发。发出：任务存活查询的原型；每个已发射协程的
 * `$step` 原型，需要 zone 时再加 `$next` 原型；共享的 `extc_trap_dead_handle` 与
 * `extc_coro_next` 分派器；最后按 yield 类型各发一份 `value`/`send` 辅助函数
 * （同一种 yield 类型只发一份）。驱动一个任务已结束的句柄会走 trap，而不是读已释放的帧。
 *
 * @param g  代码生成上下文。
 * @param m  当前模块；从 `m->funcs` 里挑已发射的协程。
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
         * starts (`isExtTarget`, known at check time) ⇒ every other product is byte-identical. */
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
 * @brief 按需发射协程运行时与任务表，并声明每个协程的步骤函数。
 *
 * 先判断任务表要不要：有需要 zone 的协程，或 extC 侧声明了 `extc_task_*` 外部函数，
 * 都算要——需要时顺带拉进池 zone 运行时并发出协程运行时。随后调用
 * `genCoroHandleDecls`，再给每个具体协程发 `$step` 原型；需要 zone 的还就地定义
 * `$next`：它把帧里的 place 设为当前再进步骤，返回后恢复，步骤报告完成时整份释放。
 *
 * @param g  代码生成上下文。
 * @param m  当前模块。
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
 * @brief 判断是否打开 codegen 子阶段计时。
 *
 * @return   `EXTC_DBG_TIME` 已设置时为真；环境变量只读一次并缓存。
 */
static int cgTimeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }
/**
 * @brief 取计时用的当前时钟读数。
 *
 * @return   进程 CPU 时间，单位秒。
 */
static double cgNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
typedef struct { const char *name; double sec; } CgAcc;
static CgAcc cgAccs[8];
/**
 * @brief 把一次子阶段耗时累加进按名字索引的计时表。
 *
 * `EXTC_DBG_TIME` 没开时直接返回。表最多 8 项；新名字占用第一个空槽，放不下就丢弃。
 * 表里只存 `name` 指针，不拷贝字符串，所以调用点传的是字面量。
 *
 * @param name  阶段名。
 * @param sec   本次耗时，单位秒。
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
 * @brief 把局部变量扫描的计数器打到 stderr。
 *
 * `EXTC_DBG_TIME` 没开时什么都不做。
 */
static void locReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[locals] n=%ld copy=%lld reads=%lld lines=%lld cuts=%ld\n",
            g_locN, (long long)g_locCopy, (long long)g_locRead, (long long)g_locLine, g_cuts);
}
/**
 * @brief 打出代码生成的计时与扫描计数报告。
 *
 * `EXTC_DBG_TIME` 没开时什么都不做。依次输出字符串扫描计数、局部变量计数，
 * 以及每个已累计的子阶段耗时。
 */
static void cgReport(void) {
    if (!cgTimeOn()) return;
    strReport(); locReport();
    for (size_t i = 0; i < 8 && cgAccs[i].name; i++)
        fprintf(stderr, "[time] %-9s %.3f s\n", cgAccs[i].name, cgAccs[i].sec);
}

/**
 * @brief 发射一个完整的函数定义：序言、函数体与共享的收尾。
 *
 * 序言备好块 arena 数组、`@overwrite` 的存储单元、返回值经过的槽和递归守卫，然后生成
 * 函数体，最后由同一段收尾释放每层 arena 并返回。改到的每项 per-function 状态都在入口
 * 保存、出口恢复，因为一个泛型会连着生成多具函数。入口有三种形状：`main` 用 C 固定的
 * 签名（`argc`/`argv` 在序言里包成语言声明的视图），协程步骤发成 `$step`，其余为普通函数。
 *
 * @param g  代码生成上下文。
 * @param f  要发射的函数。
 */
static void genFunc(CG *g, FuncDef *f) {
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
    /* `main(args)`: the one parameter the language may declare is not a C
     * parameter (C fixes the entry signature), so `argc`/`argv` come in and the
     * view is built in the prologue. */
    bool mainArgs = isMain && f->params.len > 0;
    if (isMain) {
        /* C fixes the signature of `main`, so it takes no hidden parameter; its
         * home arena is one of its own block arenas. */
        cgLine(g, "int main(%s) {", mainArgs ? "int argc, char **argv" : "void");
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
    g->inMain  = isMain;
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
    if (isMain && planUsesHome(f)) g->noArena = false;
    /* Same reason, for pools: the frame mark and the per-block release hooks hang off the
     * arena's lifetime, so a `main` that only *uses* a pool (it allocates nothing itself)
     * still needs the frame. Phase 1 keys pools to `main`'s frame; when `new (r) T[n]`
     * lands, every function that can create one gets the same treatment. */
    if (isMain && g->needPool) g->noArena = false;
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    /* The frame mark: pools created while this frame runs are linked to it, so leaving
     * the frame drops them whatever their block did. Emitted where
     * the arena exists, because hanging pools off an arena level is what this frame does. */
    /* 这个函数是不是「一个地方」：只有入口文件的函数是，库函数对地方透明
     * （`pool<T>::withCap` 建的池属于它的调用者所在的地方）。判据是两个名字里都没有 `$` ——
     * loader 把模块的函数改成 `mod$name`，泛型实例还把带 `$` 的名字放在 `instName` 里。 */
    {
        bool lib = (f->name && strchr(f->name, '$')) ||
                   (planInstName(f) && strchr(planInstName(f), '$'));
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zone] name=%s mod=%s inst=%s owner=%s line=%d\n",
                    f->name ? f->name : "-", f->modName ? f->modName : "-",
                    planInstName(f) ? planInstName(f) : "-", f->owner ? "yes" : "-", f->line);
        /* 「一个地方」= 入口文件里的函数。`modName` 是 loader 给的模块名：
         * 库方法有它，入口文件为空，所以库函数对地方透明 ——
         * `pool<T>::withCap` 里建的池属于它的调用者所在的地方。 */
        (void)lib;
        g->zoneHere = g->needPool && (!f->modName || !*f->modName);
        /* main 的 C 签名是固定的，收不了隐藏参数 ⇒ 它用当前那个地方的 zone（今天的行为）。 */
        g->funcHasZoneParam = planMakesPool(f) && !isMain;
    }
    /* 函数体的 zone（`__extc_zm1`）同样按需发射：没有这个函数，就没有任何池会登记在它的帧上，
     * 压了也是空压。`zoneMark[1]` 记下这次决定，收尾的 `zoneLeaveTo(__extc_zm1)` 查它 ——
     * 没压却弹会弹掉**调用者**的 zone。 */
    g->zoneMark[1] = false;
    g->zoneFrameMarked = false;
    if (dbgOn("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonehook] %s zoneHere=%d noArena=%d makesPool=%d needPool=%d isMain=%d\n",
                f->name ? f->name : "-", (int)g->zoneHere, (int)g->noArena,
                (int)planMakesPool(f), (int)g->needPool, (int)isMain);
    /* A step is not a place of its own: the task's place (slice C) is what its storage hangs off, so
     * it must not open a zone of its own here -- that mark also went unused in the generated C. */
    if (g->zoneHere && !g->noArena && planMakesPool(f) && !g->coroFunc) {
        cgLine(g, "int64_t __extc_zm1 = extc_pool_zoneEnter();   /* 函数体是一个地方 */");
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
        /* `self` 常常用不到 ⇒ 明确 `(void)` 掉：接收者是**签名**的一部分，
         * 用不用是函数体的事，生成物不该因此刷警告。 */
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
    g->curFuncName = cFuncName(g, f);
    bool savedParTls = g->parTls;
    g->parTls = planParTlsArena(f);
    if (isMain && planUsesHome(f)) {
        size_t hb = g->out->len;
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* main's home arena is its own body */");
        localDef(g, hb, "__extc_home", 0);   /* the declaration sits above the body */
    }
    /* The buffered console tail has one guaranteed exit: `main` flushes on every explicit
     * `return` and in the arena epilogue, but a body that runs off its end passes through
     * neither. A trap exits from wherever it stands, and so does `sys::proc::exit`; one
     * `atexit` covers every way out, and the explicit flushes stay for the ordering they give
     * inside the program. Registered only when the program uses that console (`needCout`). */
    if (isMain && g->needCout) cgLine(g, "atexit(extc_cout_flush);");
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
    /* 回收的静态根表按函数重来：形参里含引用的槽从函数体第一行起就是根。
     * 协程 step 的形参住在帧里（`f->name`），静态枚举不出来 ⇒ 整个 step 关掉触发。 */
    g->rcRoots.len = 0;
    g->rcRootsUnreliable = g->needReclaim && (g->coroFunc != NULL || g->rcGlobalsUnreliable);
    if (g->needReclaim && !g->coroFunc) {
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            rcRootDecl(g, p->type, p->cname ? p->cname : p->name);
        }
    }
    size_t mb = g->out->len;
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
    g->parTls = savedParTls;
        g->parTls = savedParTls;
        g->retType = savedRet;
        g->inMain  = false;
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
        if (fallsOff && g->noArena) {
            if (retVoid) cgLine(g, "return;");
            else         cgLine(g, "return __extc_ret_v;");
        }
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
        if (g->mainFuncName && f->name && strcmp(g->mainFuncName, f->name) == 0)
            cgLine(g, "return 0;");
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
        /* `destroy`, not `release`: this is the frame's last exit, so the block the
         * arena kept for the next round has to go too. Emitting `release` here would
         * leak one block per arena at every return -- bounded, but a leak. */
        for (int lv = 1; lv <= maxLv; lv++) {
            cgReclaimDropArena(g, lv);   /* 回收区域与 arena 同寿，早退也从这里出去 */
            cgLine(g, "extc_arena_destroy(&__extc_a[%d]);", lv);
        }
        /* 弹回到最外层那个压过的 zone：早退（`goto __extc_ret`）和走到函数末尾都从这里出去，
         * 而跳出去的路径可能把若干个块 zone 留在栈上，所以从最深的开始每层补一次。
         * 多弹是幂等的（`zoneLeaveTo` 弹到某个 mark 为止），少弹会让池留在一个已经被压掉的
         * 层级上。`zoneMark[1]`（帧自己的 zone）由序言决定：没压当然不弹。 */
        for (int lv = maxLv; lv >= 1; lv--)
            if (lv < g->levelCap && g->zoneMark[lv])
                cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lv);
        if (isMain) {
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
 * @brief 判断一个函数是不是程序的入口点。
 *
 * 判据是自由函数（没有 owner）且名字逐字为 `main`。
 *
 * @param f  候选函数；可以为 NULL。
 * @return   是入口点为 true，否则 false。
 */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

/* Emit the forward declaration of one function, so that definitions may appear
 * in any order.
 */
/**
 * @brief 发射 `parallel::run` worker 的 trampoline 和它的 ctx 类型。
 *
 * worker 的形参是 `(id, lo, hi, 视图们…)`，其中恰好一个 `mut slice<T>` 是输出：
 * trampoline 按 `[lo, hi)` 把它切成只属于本 worker 的 `sub`，其余视图原样共享——
 * 谁都不写它们，所以不需要分区。ctx 是每个视图一个字段的结构体，按值传进去。
 * worker 需要家 arena（体内有分配）时尾参补一个线程本地 arena：它的分配逃不出这次
 * 调用，两个 worker 也不会去抢主线程的帧 arena。
 *
 * @param g  代码生成上下文。
 * @param f  worker 函数；形参前三个是 `id`/`lo`/`hi`，其后全是视图。
 */
static void genParTramp(CG *g, FuncDef *f) {
    size_t nViews = f->params.len - 3;
    size_t mutIdx = 0;
    for (size_t i = 0; i < nViews; i++)
        if ((*(Param **)vecAt(&f->params, 3 + i))->type->mut) mutIdx = i;
    {   /* ctx 类型：字段名 v0..vN，类型就是各视图的 C 名（mut 与否同名，见 ttViewMut 的注释） */
        Buf ct;
        bufInit(&ct, g->arena);
        bufPrintf(&ct, "typedef struct {");
        for (size_t i = 0; i < nViews; i++)
            bufPrintf(&ct, " %s v%zu;",
                      cType(g, (*(Param **)vecAt(&f->params, 3 + i))->type), i);
        bufPrintf(&ct, " } __extc_parctx_%s;", f->name);
        cgLine(g, "%s", bufCstr(&ct));
    }
    Type *vt = (*(Param **)vecAt(&f->params, 3 + mutIdx))->type;
    const char *vn = cType(g, vt);
    const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", f->name);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp);", f->name);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp) {", f->name);
    cgLine(g, "    %s *c = (%s *)ctxp;", ctn, ctn);
    cgLine(g, "    %s sub = { .data = c->v%zu.data + lo, .len = hi - lo };", vn, mutIdx);
    {   /* 实参：只读视图原样，输出的那一份换成切好的 sub */
        Buf args;
        bufInit(&args, g->arena);
        for (size_t i = 0; i < nViews; i++)
            bufPrintf(&args, "%s%s", i ? ", " : "",
                      i == mutIdx ? "sub" : arenaPrintf(g->arena, "c->v%zu", i));
        /* worker 体内只要有 `new`，它就会多一个隐藏的 home 形参（needsHome 那套，尾参），
         * 所以这里补传 **线程本地 arena**：worker 只写自己那段输出、只返回 i64 ⇒ 分配逃不出
         * 这个 worker ⇒ 线程本地安全，而且两个 worker 不会去抢主线程的帧 arena。 */
        if (planUsesHome(f)) bufPrintf(&args, "%sextc_tls_arena", nViews ? ", " : "");
        cgLine(g, "    return (int64_t)%s(id, lo, hi, %s);", f->name, bufCstr(&args));
    }
    cgLine(g, "}");
}

/**
 * @brief 生成一个函数的前置声明，使函数定义的先后顺序不受限制。
 *
 * 协程不发同名原型：它的产物是 `name$frame` 与 `name$step`，两者一起输出，frame 必然先于使用；
 * 发一个没有定义的 `name` 原型还会破坏后续按字节偏移做的扫描——`name` 会匹配到 `name$step`
 * 里面，把中间那段文本抹掉。`@inline` 必须同时写在原型和定义上。
 *
 * @param g  生成器状态，输出与分配都取自它。
 * @param f  要声明的函数；`main` 与导出函数不加 `static`。
 */
static void genFuncProto(CG *g, FuncDef *f) {
    if (planIsCoro(f)) return;
    Buf sig;
    bufInit(&sig, g->arena);
    bufPrintf(&sig, "%s%s %s(%s);",
              (cgIsMain(f) || f->isExport) ? "" : (f->isInline ? "EXTC_INLINE " : "static "),
              cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
}

/**
 * @brief 为全局变量初始化式里提到的每个函数补发前置声明。
 *
 * 函数地址是常量，所以 `var t: api = api { alloc: hostAlloc }` 是合法的全局初始化式；但 C 要求
 * 先见过声明：全局变量在原型池之前输出，缺少这里的声明，`static api t = { .alloc = &hostAlloc };`
 * 就会报 `hostAlloc` 未声明。原型池之后还会再发一次，C 允许重复声明。
 *
 * @param ctx  生成器状态 `CG *`；用 `void *` 传是为了适配访问回调的签名。
 * @param e    访问到的表达式；只处理 `EX_IDENT` 且能解析出被调函数的节点。
 * @return     恒为 true，让子节点遍历继续。
 */
static bool globalFnProto(void *ctx, Expr *e) {
    if (e->kind == EX_IDENT && planCallee(e)) genFuncProto((CG *)ctx, planCallee(e));
    return true;
}

/**
 * @brief 遍历一条语句的子节点，把其中的函数引用交给 globalFnProto 处理。
 *
 * @param ctx  生成器状态 `CG *`，原样转交给表达式回调。
 * @param s    要遍历子节点的语句。
 * @return     astWalkStmtChildren 的返回值：子节点遍历是否未被打断。
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
 * @brief 取一个定义单元（结构体、实例、数组或带载荷的枚举）的 C 名字。
 *
 * @param u  定义单元；泛型结构体实例与泛型枚举实例上 `inst` 和 `sd`/`td` 会同时有值。
 * @return   `inst` 非空时优先用实例名（数组与泛型枚举实例都走这条），否则 `td` 非空时用带载荷
 *           枚举的名字，再否则用结构体名。
 */
static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* instance or array, enum instances included */
    if (u->td) return u->td->name;          /* non-generic enum with payloads */
    return u->sd->name;
}

/**
 * @brief 找出定义某个类型的单元在 `units` 中的下标。
 *
 * 泛型实例按 C 名匹配而不是按指针：`mut slice<T>` 是与只读视图同名、同 C 结构体的影子类型，
 * 比指针永远找不到它，依赖排序就会漏掉它，`struct reader { chunk: mut slice<u8> }` 因此栽在
 * 类型不完整上；而且一个 C 名可能对应两个实例，一个可写视图、一个只读视图。
 *
 * @param units  单元列表，元素是 `SUnit *`。
 * @param t      要查找的类型；为 NULL 时直接返回 -1。
 * @return       该单元的下标；`units` 里没有对应单元时返回 -1。
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
 * @brief 输出一个结构体、数组或枚举的 C 定义。
 *
 * 带载荷的枚举输出成 `struct { tag; union }`，并且和其他结构体一样参与依赖排序：载荷里可以
 * 含别的结构体，漏掉排序就会让生成的 C 出现未知类型名。
 *
 * @param g  生成器状态；泛型实例与泛型枚举实例会先进入替换上下文，输出完再退出。
 * @param u  要输出的定义单元。
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
 * @brief 登记一个 `option` / `result` 实例的解包辅助函数，返回它的名字。
 *
 * 以 option 类型的 C 名去重：同一实例上的每个 `!` 都指向同一个辅助函数，只生成一份定义。
 * `@unchecked` 的函数体不走这里（它们直接取载荷），所以辅助函数只可能来自带检查的 `!`。
 *
 * @param g      生成器状态。
 * @param ot     操作数类型，一个 `option<T>` 或 `result<T, E>` 实例。
 * @param isOpt  true 表示 `option`（标签 `some`），false 表示 `result`（标签 `success`）。
 * @param pay    载荷类型 `T`，也是辅助函数的返回类型。
 * @return       辅助函数名；调用点用它来传操作数、extC 源路径与行号。
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
 * @brief 登记某个基类型上的切片辅助函数，返回它的名字。
 *
 * 相同基类型、相同种类重复登记会得到同一个名字，只输出一份函数。`unchecked` 的辅助函数单独
 * 命名，带检查的调用者仍旧用带检查的那一个：两者不能共用一份定义。
 *
 * @param g          生成器状态。
 * @param ob         被切的类型：一个数组或一个视图。
 * @param st         结果切片类型。
 * @param tail       true 表示切到末尾（`s[lo..]`），省略的上界取基类型的长度。
 * @param unchecked  调用者函数体是 `@unchecked` 时为 true：辅助函数跳过 `extc_checkedRange`，
 *                   直接采用 `lo` 的写法。
 * @return           辅助函数名，由基类型与种类拼成；同一基类型只会切成同一种切片，名字不会撞车。
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
 * @brief 生成 `a[lo..hi]` 得到的视图表达式。
 *
 * 编译期能证明的切片不留运行时痕迹：基是定长数组、两个边界都是字面量时（省略的边界由检查器
 * 补上），范围在检查阶段就已知、越界早已报错，生成的 C 直接是 `&a.data[2]` 加 `.len = 3`；
 * 其余情况交给辅助函数在运行时检查范围，并以 extC 源位置捕获错误。
 *
 * @param g  生成器状态。
 * @param e  切片表达式；`e->type` 是结果视图类型，基是 `ref [N]T` 时它在 C 里是指针，取 `.data`
 *           要多一次解引用。
 * @return   视图表达式；类型无法生成时先报错，再返回占位表达式 "0"。
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
 * @brief 把一个类型提到的泛型实例补进单元列表，已有的按 C 名跳过。
 *
 * @param arena  分配新单元用的 arena。
 * @param units  单元列表，就地追加。
 * @param t      要检查的类型；不是结构体或枚举的泛型实例时直接返回。
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
 * @brief 扫描一个类型，把它提到的每个泛型实例补进单元列表。
 *
 * 顺着 `inner` 与泛型实参走，`ref`、slice 与数组元素都能跟到。函数类型的各部分在 `params` /
 * `ret` 上而不在 `inner` / `targs` 上，所以还要单独走一遍：`fn(pair<i32, u8>) -> void` 写在
 * 结构体里就是对 `pair<i32, u8>` 实例的一次提及，漏掉它那个结构体就没人输出。
 *
 * @param arena  分配新单元用的 arena。
 * @param units  单元列表，就地追加。
 * @param t      要扫描的类型；为 NULL 时直接返回。
 * @param depth  递归深度，从 0 起；超过 12 就停下，用来兜住自引用类型。
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
 * @brief 扫描一个单元提到的所有类型，把它依赖的泛型实例补进单元列表。
 *
 * 走三处：字段；方法签名（形参与返回类型都算，实例就藏在这里）；枚举变体的载荷。单元本身是
 * 泛型实例时，先把自己的类型实参替换进去。
 *
 * @param arena  分配新单元用的 arena。
 * @param tt     类型表，用来做泛型参数替换。
 * @param units  单元列表，就地追加。
 * @param u      要扫描的单元。
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
 * @brief 判断一个实例的签名里是否还残留类型参数。
 *
 * 这种 FuncDef 是检查器的产物：泛型体里的泛型调用会造出一个以 `T` 自身为实参的实例，好让函数
 * 体能被检查，它从来不是 C 函数，发射循环会跳过它。
 *
 * @param f  要检查的函数实例。
 * @return   形参类型或返回类型里还提到类型参数时为 true，否则为 false。
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
 * @brief 判断一个字节能否续接 C 标识符。
 *
 * 用来把 `e` 这个名字和 `self`、`true` 里的 `e` 区分开：单字母局部变量按名字计数，纯子串
 * 查找会到处命中，一个定义都丢不掉。
 *
 * @param c  要判断的字节。
 * @return   是字母、数字、`_` 或 `$` 时为 true，否则为 false。
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
 * @brief 记下一次文本扫描的扫描点与字节数，只在计时打开时累加。
 *
 * @param who  扫描点名字，覆盖式地记为最近一次扫描的来源。
 * @param n    本次扫描的字节数，累加进总量。
 */
static void strAccount(const char *who, size_t n) {
    if (!cgTimeOn()) return;
    g_scanCalls++; g_scanBytes += (long)n; g_scanWho = who;
}
/**
 * @brief 在 stderr 上报告累计的扫描调用次数、字节数与最近一次扫描点。
 */
static void strReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[scan] calls=%ld bytes=%lld (last site: %s)\n",
            g_scanCalls, (long long)g_scanBytes, g_scanWho ? g_scanWho : "-");
}

/**
 * @brief 统计 `needle` 在 `hay` 里作为完整标识符出现的次数。
 *
 * 字符串字面量里的出现也计入，那只会让一个定义多活一会儿。
 *
 * @param hay     要扫描的文本。
 * @param needle  要计数的名字。
 * @return        `needle` 作为完整名字出现的次数。
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
 * @brief 把一处已生成的函数定义与同一 C 名的声明配对，记下它在正文缓冲里的偏移与长度。
 *
 * 键取 C 名而不是 `FuncDef` 指针：同一个泛型函数的实例方法会按实例各生成一份，指针不唯一；
 * 两个已生成的同名定义又无法编译，所以名字在这批定义里唯一。找不到同名声明时直接返回，
 * 该定义保持未配对，后续阶段不会删它：留下一个没人调用的函数只花一条编译告警，不影响正确性。
 *
 * @param g    生成上下文；`deadFuncs` 是配对表，名字与 `f` 相同的那条就是配对对象。
 * @param f    参与配对的函数定义。
 * @param off  该定义在正文缓冲里的起始偏移，调用点从当前输出缓冲的末尾算出。
 * @param len  该定义的字节长度。
 */
static void deadFuncBody(CG *g, FuncDef *f, size_t off, size_t len) {
    const char *name = cFuncName(g, f);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (df->body || df->len || strcmp(df->name, name) != 0) continue;
        /* 只记偏移、不在这里切文本：正文中途可能临时指向别的缓冲区，此刻切下的片段在最终
         * 单元里不连续，之后按内容再也找不到。文本等 `g.body` 收齐后再从那里取。 */
        df->off = off;
        df->len = len;
        df->namedByDynTable = planDynTable(f);   /* see the field */
        return;
    }
}

static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle);
static char  *funcDefStart(char *text, DeadFunc *df, size_t *lenOut);

/**
 * @brief 登记一处刚生成的局部声明，供随后判断它有没有被读过。
 *
 * 文本从 `before` 一直取到当前输出缓冲的末尾，调用点要保证那里正是这条声明的剩余部分。
 * 所属函数取 `g->curFuncName`。
 *
 * @param g       生成上下文；记录追加到 `g->deadLocals`。
 * @param before  这条声明所在行在 `g->out` 中的起始偏移。
 * @param name    局部变量的 C 名，指向比缓冲活得更久的存储。
 * @param own     声明位置：1 在函数体内，0 在函数体之前的序言里（`__extc_home` 就是这样）。
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
 * @brief 判断一行是不是 `#define`，是则返回宏名在行内的起点。
 *
 * `#define X` 与 `#  define X` 是同一条指令（前奏会把 `#` 与 `define` 之间补上空格做对齐），
 * 两种写法都要认；`define` 与宏名之间至少要有一个空格或制表符。
 *
 * @param ln  一行的起始处，不要求以 NUL 结尾。
 * @param ll  这一行的字节数。
 * @return    宏名在这行里的指针；不是 `#define`、或行里没有宏名时为 `NULL`。
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
 * @brief 判断这段文本里是否出现调用（只看有没有 `(`）。
 *
 * 右值带调用的赋值语句不能整条删掉：调用本身就是它存在的意义，返回值没人读也一样
 * （`uint64_t skip = pcg32_next(&r)` 会推进生成器）。括号不等于调用，字面量或注释里的
 * `(` 也会算数，这样只会多留语句，不会改语义。
 *
 * @param s  待检查的文本片段。
 * @param n  片段字节数，不要求以 NUL 结尾。
 * @return   出现 `(` 时为 `true`，否则 `false`。
 */
static bool textHasCall(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == '(') return true;
    return false;
}

/**
 * @brief 数 `name` 在这段文本里被读取的次数。
 *
 * 字符串/字符字面量与注释整段跳过；紧跟单个 `=`（且不是 `==`）的出现是写入，不算读，
 * 所以 `name += 1` 这类复合赋值计为读。判名要求两侧都不是标识符字节。
 *
 * @param hay   待扫描的文本片段。
 * @param n     片段字节数，不要求以 NUL 结尾。
 * @param name  变量名，以 NUL 结尾。
 * @return      读取次数；只被写或不出现时为 0。
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
 * @brief 算 NUL 结尾字符串的 FNV-1a 散列，供 `BodyMap`、`BpMap` 的开放寻址取槽。
 *
 * @param p  以 NUL 结尾的 C 字符串。
 * @return   散列值，调用方再与掩码相与定槽。
 */
static size_t bodyHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211u; }
    return h;
}
/**
 * @brief 初始化 `BodyMap` 的槽数组，容量取不小于 `2n` 的 2 的幂。
 *
 * 槽由 arena 清零分配，空槽以 `name` 为 `NULL` 认；容量必须是 2 的幂，`mask = cap - 1`
 * 的定槽才成立。表不扩容。
 *
 * @param m  待初始化的表。
 * @param a  分配槽数组的 arena。
 * @param n  预计装入的条目数，调用点给 `g->deadFuncs.len + 1`。
 */
static void bodyMapInit(BodyMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief 往 `BodyMap` 里放一条 `name -> body`，同名只留第一次放进去的那条。
 *
 * 先到先得：调用点按 `deadFuncs` 的顺序插入，第一条正是要留下的那条。名字已存在时直接
 * 返回，不覆盖。
 *
 * @param m     目标表。
 * @param name  C 名，以 NUL 结尾；表只存指针，它必须比表活得久。
 * @param body  该名字对应的定义正文指针。
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
 * @brief 初始化 `BpMap` 的槽数组，容量取不小于 `2n` 的 2 的幂。
 *
 * 与 `BodyMap` 同构：槽由 arena 清零分配，空槽以 `name` 为 `NULL` 认，表不扩容。
 *
 * @param m  待初始化的表。
 * @param a  分配槽数组的 arena。
 * @param n  预计装入的函数名个数。
 */
static void bpMapInit(BpMap *m, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->slot = arenaAllocZero(a, cap * sizeof *m->slot);
    m->mask = cap - 1;
}
/**
 * @brief 取（没有就新建）`name` 在 `BpMap` 里的缓存条目。
 *
 * 新建的条目 `filled == 0`、`bp == NULL`，调用方填好搜索结果后自己把 `filled` 置 1。表不
 * 扩容，返回的条目地址在表的生命期内稳定。
 *
 * @param m     目标表。
 * @param name  函数 C 名，以 NUL 结尾；表只存指针，它必须比表活得久。
 * @return      指向表内条目的指针，随表存活。
 */
static BpEntry *bpMapAt(BpMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        BpEntry *s = &m->slot[i];
        if (!s->name) { s->name = name; return s; }
        if (strcmp(s->name, name) == 0) return s;
    }
}

/**
 * @brief 在 `BodyMap` 里查 `name` 对应的定义正文。
 *
 * 返回的是发射时记下的指针，文本一旦被重写就可能不再指向当前内容：调用方必须再用 `strstr`
 * 在当前文本里确认它还在，找不到就退化为整段搜索。
 *
 * @param m     目标表。
 * @param name  C 名，以 NUL 结尾。
 * @return      正文指针；表里没有这个名字时为 `NULL`。
 */
static const char *bodyMapGet(BodyMap *m, const char *name) {
    for (size_t i = bodyHash(name) & m->mask; ; i = (i + 1) & m->mask) {
        NameBody *s = &m->slot[i];
        if (!s->name) return NULL;
        if (strcmp(s->name, name) == 0) return s->body;
    }
}

/**
 * @brief 就地删除当前文本里没人读取的局部声明及其赋值。
 *
 * 判定以整个函数体为单位：某个名字在那段正文里只出现一次时，那一次就是声明本身。
 * `own == 0` 的声明坐在序言里（第一个函数体之前），不归属任何函数体，另按序言范围找。
 *
 * 每轮只删一处，删完从头重扫：重写会让此前的偏移全部失效。被删语句的右值若含调用，调用
 * 必须留下并显式丢弃它的值，否则 `-Werror` 会因 `unused-value` 失败。
 *
 * @param g      生成上下文；读 `deadLocals`、`deadFuncs`、`mainBody`。
 * @param out    文本所在的缓冲区；每处删除都重建它的内容。
 * @param textp  进入时是文本起点（`out->data`），返回时换成重写后的起点。
 * @param lenp   进入时是文本字节数，返回时换成重写后的字节数。
 */
static void dropUnusedLocals(CG *g, Buf *out, char **textp, size_t *lenp) {
    char  *text = *textp;
    size_t len  = *lenp;
    /* `own == 0` 的声明坐在序言里（第一个函数体之前），firstBody 就是那个上界；
     * `DeadFunc.body` 也只在文本尚未被重写时才指向当前文本。 */
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
            /* 这一趟的代价在内存延迟上，真正的停顿是追指针进一个新的 DeadLocal；基指针仍经
             * `vecAt` 重读，vecPush 触发的扩容不会被漏掉。 */
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
                bp = strstr(text, body);           /* 文本已被重写：每轮重新按内容搜索 */
                bl = strlen(body);
            }
            /* `g->mainBody` 是取好的 `main` 副本，而每次删除都会重建文本：删过之后按内容可能
             * 再也找不到它，于是退化为在整个单元里搜。整单元搜索在这里是安全的 —— 局部变量名
             * 只在自己函数的作用域内有效，范围放宽只会多留一个局部。 */
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
            /* 收集要删的片段：声明本身，以及每一处对它的赋值（右值含调用时只留下调用）。 */
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
                        /* 保留调用，但必须显式丢弃它的值：右值常是解包（`var f = fs::openWrite(p)!`），
                         * 剩下的裸载荷表达式会被 gcc 判为 `unused-value`，在 `-Werror` 下直接失败。 */
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
 * @brief 数 `name` 在生成的代码里作为整名出现的次数。
 *
 * 字符串/字符字面量与注释整段跳过：单字母局部名也会出现在字面量里（例如字符串里写
 * `v = `），那不是使用。判名要求两侧都不是标识符字节，所以 `e` 不会命中 `self` 里那个 `e`。
 *
 * @param hay   待扫描的文本片段。
 * @param n     片段字节数，不要求以 NUL 结尾。
 * @param name  要数的名字，以 NUL 结尾。
 * @return      出现次数。
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
 * @brief 在一段不以 NUL 结尾的文本里数 `name` 的出现次数，按代码计。
 *
 * 字面量与注释跳过，判名要求两侧不是标识符字节；名字是一段字节而不是 C 字符串，
 * 所以正文缓冲可以直接切片传进来。
 *
 * @param g       生成上下文；本函数不使用它。
 * @param hay     待扫描的文本片段。
 * @param n       片段字节数，不要求以 NUL 结尾。
 * @param needle  要数的名字，以 NUL 结尾。
 * @return        出现次数。
 */
static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle) {
    strAccount("countMentionsIn", n);
    (void)g;
    return countCodeMentions(hay, n, needle);
}

/**
 * @brief 数 `name` 出现在这些区间之外的次数。
 *
 * 走一遍 `strstr` 并跳过给定区间，比复制整个单元、把片段挖掉再数便宜。区间是右开的
 * `[starts[k], ends[k])`；名字按整名判定，字面量与注释里的提及也计入 —— 那只会多留
 * 定义，是安全的方向。
 *
 * @param hay     以 NUL 结尾的整段文本。
 * @param name    要数的名字，以 NUL 结尾。
 * @param starts  区间起点数组。
 * @param ends    区间终点数组，右开。
 * @param nspans  区间个数。
 * @return        落在所有区间之外的整名出现次数。
 */
static size_t countMentionsOutside(const char *hay, const char *name,
                                   const size_t *starts, const size_t *ends, size_t nspans) {
    size_t len = strlen(name), count = 0;
    /* `strstr` 从一个出现直接跳到下一个（向量化），逐字节配 `strncmp` 要贵得多。字面量或注释
     * 里的提及也计入 —— 这只会多留定义，是安全的方向。 */
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
 * @brief 对一段字节算 FNV-1a 散列，供 `CountTable` 的开放寻址取槽。
 *
 * 长度由 `n` 给出，不要求以 NUL 结尾，所以名字可以是文本里的一段。
 *
 * @param p  字节起点。
 * @param n  字节数。
 * @return   散列值，调用方再与掩码相与定槽。
 */
static size_t countHash(const char *p, size_t n) {
    size_t h = 1469598103934665603u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211u; }
    return h;
}
/**
 * @brief 初始化 `CountTable` 的槽数组，容量取不小于 `2n` 的 2 的幂。
 *
 * 槽由 arena 清零分配，空槽以 `name` 为 `NULL` 认。表只装调用方先登记过的名字，之后计数
 * 不再插入，所以不会填满。
 *
 * @param t  待初始化的表。
 * @param a  分配槽数组的 arena。
 * @param n  预计登记的名字个数。
 */
static void countInit(CountTable *t, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    t->slot = arenaAllocZero(a, cap * sizeof *t->slot);
    t->mask = cap - 1;
}
/**
 * @brief 把一个名字登记进名字计数表，计数从 0 开始。
 *
 * 表在 `countInit` 时按候选名字数一次性开好，所以这里只探测空槽，绝不会填满；
 * 表项直接存调用者的名字指针，不拷贝文本，该指针必须比表活得久。
 *
 * @param t  名字计数表；必须以 `countInit` 初始化过。
 * @param p  名字文本，长度 `n`，不必以 `\0` 结尾。
 * @param n  名字的字节数。
 */
static void countPut(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) { s->name = p; s->len = n; s->count = 0; return; }
        if (s->len == n && memcmp(s->name, p, n) == 0) return;
    }
}
/**
 * @brief 把某个名字的计数加上 `delta`。
 *
 * 名字不在表里就直接返回：表里只有被问到过的名字，扫描不会插入新项。
 *
 * @param t      名字计数表。
 * @param p      名字文本，长度 `n`。
 * @param n      名字的字节数。
 * @param delta  计数增量，可为负（文本被删掉时减去其中的出现次数）。
 */
static void countBump(CountTable *t, const char *p, size_t n, long delta) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return;
        if (s->len == n && memcmp(s->name, p, n) == 0) { s->count += delta; return; }
    }
}
/**
 * @brief 取某个名字当前的计数。
 *
 * @param t  名字计数表。
 * @param p  名字文本，长度 `n`。
 * @param n  名字的字节数。
 * @return   表里的计数；名字不在表里返回 0。
 */
static long countGet(CountTable *t, const char *p, size_t n) {
    for (size_t i = countHash(p, n) & t->mask; ; i = (i + 1) & t->mask) {
        NameCount *s = &t->slot[i];
        if (!s->name) return 0;
        if (s->len == n && memcmp(s->name, p, n) == 0) return s->count;
    }
}
/**
 * @brief 按 `delta` 调整一段文本里出现过的每个名字的计数。
 *
 * 只按标识符切分：被切在区间两端的名字不算，它属于区间外的那个标识符。
 * 用于从计数里减去一段即将被删掉的文本。
 *
 * @param t      名字计数表。
 * @param text  整段文本，至少要覆盖 `[start, start + len)`。
 * @param start 区间的起始偏移。
 * @param len   区间的字节数。
 * @param delta 计数增量，可为负。
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
 * @brief 从拼好的生成单元里删掉不再被引用的定义、原型和局部声明。
 *
 * 名字在成品文本里只出现在自己的定义处，就算没人用；判断做成不动点，一轮可以有
 * 定义只被另一个即将被删的定义提到，一轮删不干净。偏移会随删除移动，所以每一趟
 * 都重新按文本定位。判在保守一侧：任何别的提及都留下定义，哪怕那段代码自己也是死的。
 *
 * @param g    代码生成状态，`deadDefs` / `deadFuncs` / `deadLocals` 里的记录是判断依据。
 * @param out  生成的 C 单元；就地删除，`out->len` 随之缩短。
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
 * @brief 确保 `EXTC_UNUSED` 属性宏还定义在待插入属性的缓冲区里。
 *
 * 属性宏的裁剪先于这里跑，看不到任何使用就会把定义删掉，之后插入的属性会指向一个
 * 不存在的宏。定义插回它原本所在的 `#if defined(__GNUC__)` 块里；连锚点都不在了就
 * 返回 false，由调用方放弃插入，而不是发出编译器不认识的属性。
 *
 * @param g   代码生成状态，提供临时缓冲区的 arena。
 * @param nb  待插入属性的单元文本；就地插入定义。
 * @return    定义已在文本里或本次插入成功为 true；三个锚点一个都找不到为 false。
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
 * @brief 在生成文本里定位一个函数的定义起点和终点。
 *
 * 先整段匹配记下的函数文本；它被前面的改写趟动过时，退回按签名行找。定义结束在
 * 第零列的那个 `}` —— 生成的每个定义都这么收尾。定位不到的返回 NULL，调用方
 * 因此跳过该函数而不是删错东西。
 *
 * @param text    拼好的生成单元文本，可写。
 * @param df      函数记录；`df->body` 为 NULL（定义没发出）时返回 NULL。
 * @param lenOut  输出定义文本的字节数，从起点到收尾的 `}`。
 * @return        定义起点；没找到返回 NULL。
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
 * @brief 给没有任何调用点的函数声明加上 `EXTC_UNUSED`。
 *
 * 属性是对“没人调用”的断言，加在被调用的函数上 clang 会报 `-Wused-but-marked-unused`，
 * 一个调用点一条。名字在成品文本里只出现于自己的声明和定义就满足条件。
 * 插入点先全部收集、从尾到头应用，插入本身在最后做，否则先插一个会挪掉后面所有偏移。
 *
 * @param g    代码生成状态，`deadFuncs` 是候选来源。
 * @param out  生成的 C 单元；就地插入属性。
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
    for (size_t k = at.len; k-- > 0; ) {
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
 * @brief 给函数体从不读取的参数加上 `EXTC_UNUSED`。
 *
 * 参数删不掉，它是签名的一部分，属性是唯一能说明“这个参数是故意不用的”的办法。
 * 参数表的范围是定义里第一个 `(` 到配对的 `)`，按顶层逗号切分，每个名字只在**本**
 * 函数的函数体里计数 —— 数到文件尾会把别的函数同名参数算成使用。属性写在参数的
 * 起始处而不是名字前，写在前面的类型后会把属性挂到指针类型上，gcc 照样报参数未用。
 * 插入点先收集、从尾到头应用，插入本身在最后做。
 *
 * @param g    代码生成状态，`deadFuncs` 的每个函数体是要检查的文本。
 * @param out  生成的 C 单元；就地插入属性。
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
    for (size_t k = at.len; k-- > 0; ) {
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
 * @brief 给出一个类型的方法集合（方法表挂靠的那个结构体）。
 *
 * 类型自己没有函数体时用 `Type.mholder` —— 内建标量（`impl i64 { ... }`）和泛型**实例**
 * （`impl slice<u8> { ... }`）都属于这种；否则用声明的 `sdef`。凡是问“这个类型有没有
 * 这个方法”的趟都必须走这里，少看 `mholder` 会把这两类方法漏掉。
 *
 * @param t  类型；可为 NULL。
 * @return   方法集合的结构体定义；`t` 为 NULL 或两者都为空时返回 NULL。
 */
static StructDef *methodSetOf(Type *t) {
    if (!t) return NULL;
    return t->mholder ? t->mholder : t->sdef;
}

/**
 * @brief 判断一个函数是否被某个 trait impl 的方法表点到。
 *
 * 这类函数是可达性分析的根：程序里没有调用点，但有表引用它，所以必须发出、不能被当作
 * 没人用裁掉。先比方法集合（`methodSetOf`），把同名的、别的类型的方法挡在外面；名字
 * 再比一次，因为 codegen 可能持有声明自己的副本（实例、替换后的签名）。方法表和函数
 * 相同就算命中。
 *
 * @param m  trait impl 所在的模块，方法表从 `m->impls` 取。
 * @param f  候选函数；`f->owner` 为 NULL（自由函数）时直接返回 false。
 * @return   方法表里点到 `f` 时为 true。
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
 * @brief 把运行期原语块登记成一张按发射顺序排列的表。
 *
 * `PRIM_BLOCK` 是块本身：脚手架与定义交替。这里逐条登记，每条定义带上名字与种类，
 * 后面的死活判定读登记而不是按行形状猜；宏行的名字从 `#define` 行本身读出来。
 *
 * @param g  代码生成状态，登记写进 `g->primEnts`。
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
 * @brief 数出一个名字在一片脚手架文本里出现了几次。
 *
 * 守卫行（`#ifndef X` / `#ifdef X`）只是问一个宏有没有定义，不算使用，所以
 * `skipGuards` 为真时这些行整行跳过：`#ifndef EXTC_REC_LIMIT` 不能把宏保活。
 *
 * @param g           代码生成状态，透传给 `countMentionsIn`。
 * @param text        一片脚手架文本。
 * @param name        要数的标识符，按整个标识符匹配。
 * @param skipGuards  是否跳过 `#ifndef` / `#ifdef` 行。
 * @return            出现次数。
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
 * @brief 决定运行期原语块里哪些定义本程序用得上，并把留下的文本拼进单元。
 *
 * 块在单元之外组装，所以判定发生在它成为生成 C 的一部分**之前**：单元、块的脚手架、
 * 还活着的定义彼此都搜一遍，做成不动点。名字只出现在自己里面的定义被丢掉 —— 与文本
 * 层那趟相同的判据，只是改为问登记表。脚手架（注释、空行、`#if/#else/#endif` 骨架）
 * 永远发射，产物因此与“先整块发出、再删定义”的结果逐字节一致。
 *
 * @param g    代码生成状态，`primEmit` 填好的 `primEnts` 是判定依据。
 * @param out  生成的 C 单元；块插在 `g->primA`，其前其后原样保留。
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
 * @brief 发出生成文件的开头：特性宏、标准头文件、以及原型要用到的那几个 C 类型。
 *
 * 特性宏只认**第一个 include 之前**的位置，所以它们必须写在这里、头文件块之上，并且
 * 只给真正需要那套 POSIX/GNU 接口的程序开：`g->needTime` / `g->needDns` 决定
 * `_POSIX_C_SOURCE`，`g->needHttpFile` 决定 `_GNU_SOURCE`（`clock_gettime`、`nanosleep`、
 * `getaddrinfo` 在严格 c11 下不可见）。dyn 句柄与协程句柄的 typedef 也在这里声明：
 * 原型和实例结构体会先提到它们，而运行期在最终拼装时才接进来。
 *
 * @param g    代码生成状态；`needTime`、`needDns`、`needHttpFile`、`needCoroHandle` 是发射开关。
 * @param out  生成的 C 单元，从头往后写。
 * @param m    模块；`planUsesDyn` 由此判断要不要发 dyn 句柄类型。
 */
static void emitPreamble(CG *g, Buf *out, Module *m) {
    /* `-std=c11` is strict ISO, so the POSIX half of `<time.h>` (`clockid_t`, `CLOCK_MONOTONIC`,
     * `nanosleep`) is hidden. A feature-test macro only counts **before the first include** --
     * defining it later has no effect -- so it goes here, above the include block, and only for a
     * program that actually asks for the time (`needTime` is set by the declaration scan above,
     * which is why that scan has to run first). */
    /* `clock_gettime`/`nanosleep` 是 POSIX；`getaddrinfo`/`inet_ntop` 也是（严格 c11 下 `struct
    * addrinfo` 直接不可见）。宏必须在**第一个 include 之前**。 */
    if (g->needTime || g->needDns)
        bufPuts(out, "#define _POSIX_C_SOURCE 200809L   /* clock_gettime/nanosleep/getaddrinfo */\n");
    /* 文件层的 HTTP 日期用 `strptime`/`timegm`/`gmtime_r`：前两个不是 ISO C ⇒ 要 `_GNU_SOURCE`
    * （`timegm` 连 POSIX 都不是）。只在这个程序真的声明了那几个 shim 时才开。 */
    if (g->needHttpFile)
        bufPuts(out, "#define _GNU_SOURCE 1   /* strptime/timegm/gmtime_r：HTTP 日期那三个 */\n");
    bufPuts(out,
        "/* Generated by the extC compiler -- do not edit by hand.\n"
        " * The generated C is deliberately boring: no macro tricks, no clever optimizations.\n"
        " * `#line` directives map the C compiler's errors back to the .extc source lines.\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"      /* offsetof, needed by the descriptor tables */
        "#include <stdio.h>\n"
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
 * @brief 记录运行期块的拼接点，并在单元末尾发出 `@overwrite` 信元类型。
 *
 * 原语块（`primEmit`）与协程运行期**紧接在单元之后**组装，它们进不进产物由后面的
 * `primSplice` 决定，所以这里只记下拼接点 `g->primA`：块之前的字节没有任何一趟会删，
 * 这个偏移到扫描时仍然准确。`@overwrite` 的信元类型含 `extc_arena *home`，必须排在
 * arena 之后，所以本函数里只算 `needOw`，类型本身发在 arena 那一段。
 *
 * @param g    代码生成状态。
 * @param out  生成的 C 单元；这里可能追加信元类型。
 * @param m    模块。
 */
static void emitRuntimePrologue(CG *g, Buf *out, Module *m) {
    /* The reclamation runtime: the slab allocator, the holder counts and the region sweep.
     * It is emitted **before** any code that could name it -- its functions are `static` -- and
     * only when the program has a governed allocation point (`g->needReclaim`, narrowed by
     * `scanUnitReclaim` from that criterion; `EXTC_RECLAIM=1` forces it on, `=0` off). */
    if (g->needReclaim) {
        reclaimEmitRuntime(g->arena, out);
        /* 区域由 **arena 的地址**认领。分配点本来就知道自己落在哪只 arena 上
         * （`arenaRefAt`），于是区域的寿命与那只 arena 完全一致 —— 包括被调者用调用者的
         * 家 arena 分配的那种情形（家 arena 是**同一个地址**一路传下来的），也包括递归里
         * 同一层不同帧的 arena。释放点在 `extc_arena_destroy` 旁边发射
         * （`cgReclaimDropArena`）：块释放不动区域，区域活到帧销毁。
         *
         * 表满时新区域**不入表**：宁可泄漏一个区域，也不提前释放 —— 与计数"到顶粘性"
         * 同一条守则（`reclaim.c` 的义务 3）。
         *
         * 放在运行期文本之后、原语块之前：它只引用 `ExtcRcRegion` 与 `extc_rc_regionNew`
         * /`extc_rc_regionFree`，三个都由上面那段文本提供了。`unused` 属性是因为
         * "打开了开关但程序里没有分配点"是合法输入：那时两个函数没人调用，而生成 C 在
         * `-Wall -Werror` 下仍要编得过。 */
        bufPuts(out,
            "#define EXTC_RC_REG_MAX 512\n"
            "static void *extc_rc_regKey[EXTC_RC_REG_MAX];\n"
            "static ExtcRcRegion *extc_rc_regVal[EXTC_RC_REG_MAX];\n"
            "static size_t extc_rc_regN;\n"
            "__attribute__((unused)) static ExtcRcRegion *extc_rc_regionOf(void *key, int32_t rid) {\n"
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
 * @brief 组装描述符表、通用打印器与结构相等比较的运行期文本。
 *
 * 文本写进各自的缓冲（`g->rt`、`g->rtPrint`、`g->rtEq`）而不是直接写进单元：
 * 只有标量描述符行登记进 `deadDefs`，没人提到的行到后面才被删；`rtPrint` 只在程序
 * 真的调用打印时才发出去；超长字符串字面量按 C99 的 4095 字符上限拆成多次 `bufPuts`。
 *
 * @param g  代码生成状态，运行期缓冲挂在它上面。
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
        /* 这一段注释里**不要写出那个打印函数的名字**：它在运行时块文本里是按"没人提到就删"裁的，而那个判定是**文本**计数 —— 注释里提一次就等于"有人用"。 */
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
        /* 这里不挂 `__attribute__((unused))`：这一段是**整块发射**的，而那个打印函数
        * 只有程序真的打印了结构化类型时才会被调用 ⇒ 凡是因为别的理由需要运行时块、又不打印
        * 结构化类型的程序都会撞 `-Werror=unused-function`；而 `static inline` 不是解
        * （GCC 认、Clang 反手报 `-Wunneeded-internal-declaration`）。
        * 这一段**按需发射**：`rtPrint` 只在真的有它的调用点时发出去（`needPrint`），
        * 所以属性可以摘 —— 没人调用的函数根本不进产物。 */
        "static void extc_print(const void *p, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:   printf(\"%d\", (int)*(const int8_t *)p); return;\n"
        "    case EXTC_D_I16:  printf(\"%d\", (int)*(const int16_t *)p); return;\n"
        "    case EXTC_D_I32:  printf(\"%d\", (int)*(const int32_t *)p); return;\n"
        "    case EXTC_D_I64:  printf(\"%lld\", (long long)*(const int64_t *)p); return;\n"
        "    case EXTC_D_U8:   printf(\"%u\", (unsigned)*(const uint8_t *)p); return;\n"
        "    case EXTC_D_U16:  printf(\"%u\", (unsigned)*(const uint16_t *)p); return;\n"
        "    case EXTC_D_U32:  printf(\"%u\", (unsigned)*(const uint32_t *)p); return;\n"
        "    case EXTC_D_U64:  printf(\"%llu\", (unsigned long long)*(const uint64_t *)p); return;\n"
        "    case EXTC_D_F32:  printf(\"%g\", (double)*(const float *)p); return;\n"
        "    case EXTC_D_F64:  printf(\"%g\", (double)*(const double *)p); return;\n"
        "    case EXTC_D_BOOL: printf(\"%s\", *(const bool *)p ? \"true\" : \"false\"); return;\n"
        "    case EXTC_D_REF:  printf(\"<ref>\"); return;\n"
        "    case EXTC_D_TEXT: {\n"
        "        int64_t n = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "        printf(\"%.*s\", (int)n, (const char *)*(const void *const *)p);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ENUM: {\n"
        "        const char *const *names = (const char *const *)d->table;\n"
        "        int tag = *(const int *)p;\n"
        "        if (tag < 0 || (size_t)tag >= d->count) printf(\"<%s>\", d->name);\n"
        "        else                                    printf(\"%s\", names[tag]);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *f = (const ExtcField *)d->table;\n"
        "        printf(\"%s { \", d->name);\n"
        "        for (size_t i = 0; i < d->count; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            printf(\"%s: \", f[i].name);\n"
        "            extc_print((const char *)p + f[i].off, f[i].desc);\n"
        "        }\n"
        "        printf(\" }\");\n"
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
        "        printf(\"[\");\n"
        "        for (size_t i = 0; i < n; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            extc_print(data + i * d->size, d->elem);\n"
        "        }\n"
        "        printf(\"]\");\n"
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
 * @brief 以依赖顺序发出本单元需要的全部 C 类型定义。
 *
 * 枚举先于使用它的结构体：C11 无法前向声明 enum tag。带载荷的枚举只先发 tag 常量，
 * `struct { tag; union }` 定义留到按依赖排序的那一段，因为载荷可以按值含另一个结构体。
 * 泛型实例的闭包在这里展开到不动点，超过 64 轮即以错误退出，而不是少发类型。
 *
 * @param g     代码生成上下文，类型文本写进它的输出缓冲并登记待剪枝项。
 * @param m     当前模块，提供 `types`、`structs`、`funcs`。
 * @param tt    类型表，提供 `instances`、`enumInstances`、`fnTypes`。
 * @param arena 暂存分配器，用于收集本轮的类型单元与临时文本。
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
     * its own. */
    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
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
 * @brief 为模块里的全局与常量发出 `static` 定义，并把每行登记进未引用剪枝表。
 *
 * 定长全局不需要 arena；C 自己把静态存储清零，所以没有初始化式的全局不写初始化式。
 * `static` 是有意为之：外部链接会让 gcc 放弃外层循环向量化。全局初始化式里可以出现
 * 函数名（`fn` 字段表），C 要求它们先声明，所以先走一遍初始化式发原型。
 *
 * @param g 代码生成上下文，`out` 接收定义文本，`deadDefs` 接收可剪枝条目。
 * @param m 当前模块，`globals` 是待发全局列表。
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
        /* 回收打开时，全局引用槽是**进程级根**：每个触发点的根表都要列上它。全局量是文件级
         * `static`，名字在每条函数体里都看得见。类型里引用枚举不出来 ⇒ 所有函数关掉触发。 */
        if (g->needReclaim && gd->ann) {
            RcScan sc = rcScanType(g, gd->ann, 0);
            if (sc == RC_CONSERVATIVE) g->rcGlobalsUnreliable = true;
            else if (sc == RC_LEAF_REFS)
                *(const char **)vecPush(&g->rcGlobalRoots) = gd->name;
        }
    }
    if (m->globals.len) cgLine(g, "");
}

/**
 * @brief 发出全单元的原型池：所有函数体之前、且只能出现原型。
 *
 * 这里绝不能出现定义：定义先于被调函数的原型时，C 会把它当成隐式声明的 `int()`，
 * 后面的真原型就成了冲突类型。参数表必须与定义用同一个 `cgParamList`：带 home arena
 * 的函数多一个隐藏参数，少了它 C 类型对不上。实例方法只发被调用的（`planUsed`），
 * 协程协议不在这里发，它们在调用点内联。
 *
 * @param g     代码生成上下文，`out` 接收原型，`deadFuncs` 接收可剪枝条目。
 * @param m     当前模块。
 * @param arena 暂存分配器，用于拼装签名文本。
 */
static void emitProtos(CG *g, Module *m, Arena *arena) {
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
        const char *ret = cgIsMain(f) ? "int" : cType(g, f->ret);
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
                  (cgIsMain(f) || f->isExtern || f->isExport) ? ""
                                               : (f->isInline ? "EXTC_INLINE " : "static "),
                  ret, cFuncName(g, f), cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
        /* An exported function is registered like `main`: nowhere. The pruners drop a definition
         * only when its name occurs nowhere but its own declaration and definition, and a symbol
         * C calls is mentioned by nobody in this file -- so registering it would delete it. */
        if (!cgIsMain(f) && !f->isExtern && !f->isExport && !inTraitTable(m, f)) {
            DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
            df->name  = cFuncName(g, f);
            df->proto = bufCstr(&sig);
            *(DeadFunc **)vecPush(&g->deadFuncs) = df;
        }
        substLeave(g, sv);
    }
    if (g->structs.len || g->insts.len || g->funcs.len) cgLine(g, "");
}

/**
 * @brief 初始化代码生成状态：实例表、协程 kind 预扫与事件层标志。
 *
 * 每个被发射的协程函数拿到递增的 `kind`（装箱点与 handle 分发必须一致），只要有一个
 * 协程的帧被提升为 handle，`needCoroHandle` 就置位。事件层只在这个程序用到
 * `extc_epoll_*`、`extc_sock_*` 或 `extc_tcp_*` 之一时进场。
 *
 * @param g     代码生成上下文，其向量与缓冲在这里初始化。
 * @param m     当前模块，`funcs` 是协程预扫的来源。
 * @param tt    类型表，`instances` 里仍提到类型参数的实例不发 C。
 * @param arena 分配器，CG 内所有向量与缓冲都从它分配。
 */
static void initCGState(CG *g, Module *m, TypeTable *tt, Arena *arena) {
    vecInit(&g->structs, arena, sizeof(void *));
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
    /* 回收的编译期状态：三个向量都从空开始，靠 `vecInit` 而不是靠 CG 被清零。 */
    vecInit(&g->rcRoots, arena, sizeof(const char *));
    vecInit(&g->rcRootMarks, arena, sizeof(size_t));
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
 * @brief 扫描模块里的声明，决定运行期哪些可选块需要发射。
 *
 * 触发点是声明而不是调用：`extern!` 或 `@builtin` 原语出现在模块里，就说明程序用了对应的
 * 运行期。池还多一条来源：检查器算好的 `makesPool`，因为 `extc_pool_*` 的声明在库模块里，
 * 入口模块的函数表看不到。这个扫描必须在 preamble 之前跑，`_GNU_SOURCE` 只对第一个
 * include 之前有效。
 *
 * @param g 代码生成上下文，置位 `needRawTerm`、`needPool`、`needCout`、`needMemFind`、
 *          `needPlateCopy`、`needPlateView`、`needTime`、`needHttpFile`、`needDns`。
 * @param m 当前模块，`funcs` 与 `structs` 的声明是全部来源。
 */
static void scanUnitNeeds(CG *g, Module *m) {
    /* 回收的开关：默认打开，`EXTC_RECLAIM=0` 关掉；给了非零值则**强制**打开（跳过下面的
     * 分配点判据）。判据本身不在这里 —— 这一趟只按环境变量置意图，等 `collectEmitted` 把
     * 真正要发射的函数挑出来之后，`scanUnitReclaim` 再按"有没有回收管辖的分配点"收窄。 */
    const char *rcEnv = getenv("EXTC_RECLAIM");
    g->rcForced = rcEnv && *rcEnv && strcmp(rcEnv, "0") != 0;
    g->needReclaim = !(rcEnv && *rcEnv && strcmp(rcEnv, "0") == 0);
    /* `EXTC_RC_SHARDS=N`：回收的**分片数**（停顿旋钮），默认 1。
     * 只有 > 1 才会把每个逻辑区域切成 N 只运行期区域；1、未设、空串、非数、越界一律当 1
     * （单片行为）。上限 64：再往上每片至少一只 1 MiB slab 的底噪会盖过停顿收益。 */
    g->rcNShard = 1;
    {
        const char *sh = getenv("EXTC_RC_SHARDS");
        if (sh && *sh) {
            long v = strtol(sh, NULL, 10);
            if (v > 1) g->rcNShard = (int)(v > 64 ? 64 : v);
        }
    }
    /* Does the program use the raw-terminal primitives? They arrive as an `extern!` the
     * library declares (`extc_raw_enter` in `std::sys::term`), which is the one thing
     * codegen can see from here. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExtern || !f->name) continue;
        if (strcmp(f->name, "extc_raw_enter") == 0) g->needRawTerm = true;
        /* The pool registry, on the same footing: the library declares these through
         * `extern!`, so their presence is what says the program uses pools at all. */
        if (f->name && strncmp(f->name, "extc_pool_", strlen("extc_pool_")) == 0) g->needPool = true;
        if (strcmp(f->name, "extc_cout_put") == 0)  g->needCout    = true;
        /* The byte-search runtime, on the same footing: `std::sys::mem` is the only module
         * that declares these, and its declarations are what says the program wants `memmem`
         * instead of a byte loop. The flag is read again before the includes (the `_GNU_SOURCE`
         * block below), so this scan has to run before the preamble -- it does. */
        if (f->externLib && strcmp(f->externLib, "extc-mem") == 0)  g->needMemFind = true;
        /* 板的运行期与 memfind 同一形状：触发点是**声明**（只有特权层 std::sys::heap 会声明它）。 */
        if (f->externLib && strcmp(f->externLib, "extc-heap") == 0) {
            if (plateIsMemCopyName(f->name)) g->needPlateCopy = true;
            else                             g->needPlateView = true;
        }
        if (f->externLib && strcmp(f->externLib, "extc-time") == 0)  g->needTime = true;
        /* 文件层那五个 shim：与 net 共用 `extc-runtime` 库标签 ⇒ 只能按**名字**认。
         * 这个扫描必须在 preamble 之前跑（`_GNU_SOURCE` 只对第一个 include 之前有效），
         * 所以标志在这里置，而不是在 `genCoroDecls` 里。 */
        if (f->name && (strncmp(f->name, "extc_file_", 10) == 0 ||
                        strncmp(f->name, "extc_sendfile", 13) == 0 ||
                        strncmp(f->name, "extc_http_", 10) == 0))  g->needHttpFile = true;
        if (f->name && strcmp(f->name, "extc_dns_lookup") == 0)  g->needDns = true;
    }
    /* 池还多一条来源：**检查器早就算好的 `makesPool`**。只看"这个模块里声明了 `extc_pool_*`
     * 的 extern"是不够的 —— 那些声明在**库模块**里，而 `m` 是入口模块，于是 main 的 prologue
     * 那一刻 `needPool` 还是假 ⇒ 帧的 zone（`__extc_zm1`）不发 ⇒ 提权到第 1 层也没法表达
     * （`zoneArgRef` 会静默退回动态兜底）。`makesPool` 是可传递最小不动点，任何函数（含方法）
     * 为真都说明这个程序用池 —— 这正是这一格要问的问题。 */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f && planMakesPool(f)) g->needPool = true;
        /* `@builtin` 的那一个原语同理：声明在特权层（`std::sys::heap`），编译器给函数体。
         * 它不是 `extern!`，所以上面那条按 `externLib` 的扫描看不见它。 */
        if (f && f->isBuiltin && plateIsViewOfName(f->name)) g->needPlateView = true;
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (!f || planCoroProto(f)) continue;
            if (planMakesPool(f)) g->needPool = true;
        }
    }
}

/* ==================== 区域回收：这一趟要不要发 ====================
 *
 * 回收管辖的堆存储只有三处产生点，都在 codegen 里：
 *
 *   · `new T`（单值或定长 `new [N]T`）—— `exNew` 的不带长度那一支；
 *   · `alloc<T>(n)` —— `exGencall` 的 arena 那一支；
 *   · `@overwrite` 的**单值**格子 —— 那一支也走 `new T`，所以初始化式本身就是上面第一条。
 *
 * 不带长度的 `new T[n]` 留在 arena 里（视图不是引用槽），`@overwrite` 的变长格子同理，所以
 * 两者都不算。`@overwrite` 不需要单独的判据：它的初始化式就是 `new T`，走一遍表达式就看得见。
 *
 * 这一趟扫的是**将要发射**的那些函数体（`g->funcs` 已由 `collectEmitted` 剪过）加泛型实例的
 * 方法体加全局量的初始化式，于是"程序里出现"指的是**产物里出现**：一个只有死函数里才有 `new`
 * 的程序不带运行期。反过来多认一两个也不出错（只是多带一块没人调的运行期）—— 判据宁可宽一点。
 */

/** @brief 语句回调：转发给表达式/语句的通用遍历（先声明，`rcAllocInExpr` 的访问器要引它）。 */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s);

/** @brief 表达式回调：命中"回收管辖的分配点"就返回 false 停下整趟走。 */
static bool rcAllocInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (!e) return true;
    if (e->kind == EX_NEW) return e->u.new_.count != NULL;      /* 带长度 ⇒ 视图，不归回收管 */
    if (e->kind == EX_GENCALL)
        return !(e->u.gencall.name && strcmp(e->u.gencall.name, "alloc") == 0);
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief 判断一个函数体里有没有回收管辖的分配点。
 *
 * @param body 函数体；NULL（extern 之类）返回 false。
 * @return     有则 true。
 */
static bool rcBodyHasGovernedAlloc(Stmt *body) {
    if (!body) return false;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return !astWalkStmtChildren(body, &v);
}

/** @brief 语句回调：把语句的每个子节点都走一遍（`rcBodyHasGovernedAlloc` 的访问器）。 */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s) {
    (void)ctx;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 收窄 `needReclaim`：没有回收管辖的分配点就不发运行期。
 *
 * `EXTC_RECLAIM=1`（`rcForced`）时整趟跳过 —— 门禁按"开关打开"这一档写，与分配点在不在无关。
 *
 * 必须在 `collectEmitted` 之后、`emitPreamble`/`emitRuntimePrologue` 之前跑：判据要用
 * `g->funcs`（剪枝后的函数表），而运行期文本按这个标志进序言。
 *
 * @param g 生成器；`funcs`、`insts` 已填好。
 * @param m 模块，全局量的初始化式从这里看。
 */
static void scanUnitReclaim(CG *g, Module *m) {
    if (!g->needReclaim || g->rcForced) return;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && rcBodyHasGovernedAlloc(f->body)) return;
    }
    /* 泛型实例的方法体：模板本身不进 `g->funcs`（类型参数还在），实例体由 `emitInstanceBodies`
     * 发，所以这里按实例的 `sdef` 补一遍。 */
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
 * @brief 初始化 CG 的各运行期缓冲，并把固定的运行期文本写进对应缓冲。
 *
 * 这些文本在这里一次写好；是否进入最终产物由各自的 `need*` 标志或需求表决定，例如
 * `rt` 只在有打印、结构相等或描述符行时才拼接。控制台缓冲、`%g` 格式化器与原始终端
 * 恢复块各自带静态状态，必须整块发射。
 *
 * @param g     代码生成上下文，其缓冲在这里初始化并预填。
 * @param arena 分配器，缓冲存储来自它。
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
    bufInit(&g->desc, arena);
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
        "        return;\n"
        "    }\n"
        "    if (extc_cout_len + n > (int64_t)sizeof extc_cout_buf) extc_cout_flush();\n"
        "    memcpy(extc_cout_buf + extc_cout_len, p, (size_t)n);\n"
        "    extc_cout_len += n;\n"
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

/**
 * @brief 用可达性剪枝筛出真正要发射的结构体、方法与自由函数。
 *
 * 判据是检查器在调用点打下的 `used`，加上几类根：`main`、`@export` 与 trait 表里的成员。
 * 三类东西不发：泛型模板本身（`T` 未替换会生成编不过的 C）、签名里仍提到类型参数的临时
 * 实例，以及只由协程协议表示的 `next`/`value`（它们在调用点内联）。
 *
 * @param g 代码生成上下文，筛选结果推进 `structs` 与 `funcs`。
 * @param m 当前模块，`structs` 与 `funcs` 是候选来源。
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
        if (!planUsed(f) && !cgIsMain(f) && !f->isExport && !inTraitTable(m, f)) continue;
        /* The coroutine protocols (`next`/`value`) are emitted inline at their call sites and have no
         * body of their own -- emitting one here hit genBlockBody with a null body. */
        if (planCoroProto(f)) continue;
        *(FuncDef **)vecPush(&g->funcs) = f;
    }
}

/**
 * @brief 发出泛型实例的方法体，并把每段登记进未引用剪枝表。
 *
 * 只发被调用的方法（`planUsed`）；`planEnterInst` 告诉计划层当前发的是哪个实例，因为
 * 泛型体里的一个调用点由所有实例共享。方法体总是登记：原型循环已经建好带 `proto` 的
 * 条目，`deadFuncBody` 只补 `off`/`len`，缺了体会让 `markUnusedParams` 跳过它，
 * 于是忽略 `self` 的方法在 `-Werror=unused-parameter` 下编不过。
 *
 * @param g 代码生成上下文，`insts` 是待发实例，`out` 接收方法体。
 */
static void emitInstanceBodies(CG *g) {
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (inst->kind != TY_GENERIC) continue;
        SubCtx sv = substEnter(g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (planCoroProto(md)) continue;   /* emitted inline at the call site */
            if (!planUsed(md)) continue;      /* called methods only */
            planEnterInst(inst);          /* a method body is shared by every type instance */
            size_t fb = g->out->len;
            CGACC("cg-emit", genFunc(g, md));
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
 * @brief 为 checker 见过 `dyn` 形式的每个 trait 生成统一方法表。
 *
 * 接收者被擦成 `void *`，这是存储的 `dyn` 值能通过表调用的前提：调用点不知道具体类型。
 * 每个 (trait, 类型, 方法) 生成一个适配实现的 thunk，表名按具体类型的 `cType` 拼出，
 * 所以一个泛型目标的每个实例各有一张表。无法分发的槽（无接收者、泛型、返回 `Self`）
 * 保留位置（顺序即 ABI），类型写成 `void *` 且填 NULL，checker 不会从它分发。
 * 声明写进 `vtDecls` 随原型一起发出（分发点需要结构体在作用域内），定义留在 `vtDefs`，
 * 由 `finishUnit` 在所有按字节偏移重写的趟之后追加。
 *
 * @param g     代码生成上下文，`vtDecls`/`vtDefs` 是输出缓冲。
 * @param out   单位文本，本函数把 `vtDecls` 追加到它末尾。
 * @param m     当前模块，提供 `traits` 与 `impls`。
 * @param arena 分配器，用于拼装 thunk 名与声明文本。
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
 * @brief 为 `parallel::run` 的 worker 发出 trampoline 与原型，并置位 `needPar`。
 *
 * 位置是这段的关键：产物顺序是 includes → 原型 → 运行期 → 函数体，trampoline 既要
 * 在调用点的声明之前，又要在 worker 原型之后，所以函数体循环的开头是唯一正确的位置。
 * worker 的原型用同一个 `genFuncProto` 补一份，重复声明合法且文本一致。
 *
 * @param g 代码生成上下文，`funcs` 里 `planIsParWorker` 为真的函数各得一份 trampoline。
 */
static void emitParTramps(CG *g) {
    /* `parallel::run` 的 trampoline 必须落在**任何函数体之前**：调用点要看到它的声明，
     * 它自己要看到 worker 的原型。产物顺序是 includes → 原型 → 运行期 → 函数体，
     * 所以函数体循环的开头是唯一正确的位置；worker 原型用同一个 genFuncProto 补一份，
     * 重复声明合法且文本一致。 */
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
            g->needPar = true;               /* 运行期里那两个 pthread 原型与 extc_par_run 才会发射 */
        }
        if (parAny) cgLine(g, "");
    }
}

/**
 * @brief 发出 `funcs` 里每个非 extern 函数的函数体。
 *
 * 发之前先 `substEnterFunc`（实例需要替换）与 `planEnterFunc`（泛型体里的调用点由当前
 * 实例决定调到什么），发完把这个字节区间交给 `deadFuncBody` 登记。
 *
 * @param g 代码生成上下文，`funcs` 已由 `collectEmitted` 筛过，`out` 接收文本。
 */
static void emitFuncBodies(CG *g) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f->isExtern) continue;              /* an external declaration has no body */
        SubCtx sv = substEnterFunc(g, f);      /* instances need substitution */
        /* Tell the plan which instance is being emitted: a call site inside a generic
         * body is shared by every instance, and what it calls depends on this. */
        planEnterFunc(f);
        size_t fb = g->out->len;
        CGACC("cg-emit", genFunc(g, f));
        deadFuncBody(g, f, fb, g->out->len - fb);
        planLeaveFunc();
        substLeave(g, sv);
        cgLine(g, "");
    }
}

/**
 * @brief 把协程 handle 定义、池运行期与协程运行期拼进单位文本的协程段。
 *
 * 只在需要 handle、协程段存在、且尚未拼全时动作：先取 `coroDefA` 之前的文本，依次插入
 * handle 结构、池运行期（若还没发）与协程运行期，再接上 `coroDefA` 之后的部分。拼接后
 * 文本变长，所以整个 `Buf` 重新组出来，不能就地写回 `out->data`。
 *
 * @param g     代码生成上下文，`needCoroHandle`/`coroDefA`/`coroDefPrinted`/`taskTableDone`
 *              决定拼什么，`poolDone` 与 `needPool` 会被就地更新。
 * @param out   单位文本，整体被替换为拼接结果。
 * @param arena 分配器，拼接用临时缓冲从它分配。
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
 * @brief 按需把 `par::run` / `par::map` 的并行运行期文本追加到单位末尾。
 *
 * 语言没有函数指针，回调来自生成的静态 trampoline，这段只知道普通 C 函数指针。范围由
 * 一个原子 chunk 计数器分发，所以互不重叠；每个 worker 拿自己的 arena
 * （`extc_tls_arena`），所以一个 region 只有一个所有者。`withRegion` 为真时开 region，
 * 下一次运行会关掉上一次的。线程创建失败不是错误：范围由更少的 worker 干完。
 *
 * @param g   代码生成上下文，`needParRegion` 决定是否发 `EXTC_PAR_REGION`，`needPar`
 *            （或环境变量 `EXTC_DBG_PAR`）决定是否发整块运行期。
 * @param out 单位文本，运行期文本追加到这里。
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
            "    for (int64_t i = 0; i < r->workers; i++) extc_arena_destroy(&r->arenas[i]);\n"
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
            "        int64_t rc = j->fn(a->id, lo, hi, j->ctx);\n"
            "        if (rc != 0 && __atomic_load_n(&j->first_err, __ATOMIC_RELAXED) == 0)\n"
            "            __atomic_store_n(&j->first_err, rc, __ATOMIC_RELAXED);\n"
            "    }\n"
            "    extc_tls_arena = NULL;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    if (!r)\n"
            "#endif\n"
            "        extc_arena_destroy(&local);   /* a region's arenas are the parent's to free */\n"
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
            "}\n");
    }
}

/**
 * @brief 把按需运行期块、辅助函数与视图原语追加到单位文本，并登记可剪枝条目。
 *
 * 拼接顺序就是 C 的定义先于使用：描述符表、打印/相等运行期、控制台、池、文件层、DNS、
 * memfind、板视图/拷贝、时间、事件、域、dyn 运行期、原始终端，再是各 helper 与视图
 * 索引器/拷贝器。helper 与视图原语的需求是保守的，所以它们和描述符一样登记进
 * `deadDefs`，由 `dropUnreferenced` 收回没人命名的部分。
 *
 * @param g     代码生成上下文，各 `need*` 标志与 `helpers`/`unwraps`/`viewIdx`/`viewCpy`/
 *              `viewCpyU`/`descs` 决定发什么。
 * @param out   单位文本，追加目标。
 * @param m     当前模块，域适配器按 `funcs` 里的 `ext` 目标生成。
 * @param arena 分配器，生成视图原语用的临时缓冲与登记文本从它分配。
 */
static void emitLateRuntime(CG *g, Buf *out, Module *m, Arena *arena) {
    /* The table is what the descriptors `emitDescRegion` may have written are made of, so it
     * goes out whenever anything could name it: a print, a structural `==`, or a descriptor row
     * that exists at all (`descs`). `rtPrint` goes out only where `extc_print` is actually
     * called -- that separation is BG-1's fix. */
    if (g->needRuntime || g->eqNeed.len || g->descs.len) bufPuts(out, bufCstr(&g->rt));
    if (g->needPrint)                                  bufPuts(out, bufCstr(&g->rtPrint));
    if (g->needCout) bufPuts(out, bufCstr(&g->rtCout));
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
    if (g->eqNeed.len)                  bufPuts(out, bufCstr(&g->rtEq));
    bufPuts(out, bufCstr(&g->desc));
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
 * @brief 收尾整个单元：接上函数体、反复剪掉没人命名的定义、再补上协程与方法表定义。
 *
 * 未引用剪枝要跑到文本长度稳定为止：掉一个定义会让下一个定义也变成没人命名。运行时原语块
 * （`primSplice`）在剪枝之后才拼进文本，因为这个问题只能对定稿文本问一次。
 * `coroDefs` 与 `vtDefs` 最后追加：必须在所有按字节偏移重写的趟之后，且帧类型要排在
 * 用到它的 `$step` 之前。`g->bodyOff` 记录函数体起点。
 *
 * @param g     代码生成上下文，`body`/`deadFuncs`/`mainOff`/`mainLen` 在这里被消费。
 * @param out   单位文本，函数体与尾部定义追加到这里。
 * @param arena 分配器，切分定义文本用的临时缓冲从它分配。
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
 * @brief 把已检查的模块生成一份完整的 C 翻译单元。
 *
 * 顺序固定：初始化状态 → 扫描需要哪些运行期 → 类型定义 → 全局 → 原型 → 函数体（写进
 * 临时缓冲）→ 协程/描述符/运行期按需拼接 → 未引用剪枝 → 尾部定义。入口处先做两项命名
 * 检查：泛型实例名不得与用户声明的结构体重名；`@export` 的名字不得带隐藏参数、不得是
 * C 关键字、两个导出不得同名。任一失败即报错并返回 false。
 *
 * @param ctx     编译上下文，错误经它报告，`hasError` 决定返回值。
 * @param arena   分配器，CG 状态与产物缓冲都从它分配。
 * @param tt      类型表，提供实例与函数类型。
 * @param m       当前模块。
 * @param lineMap 为真时在产物里保留行号映射。
 * @param out     产物缓冲，完整 C 文本写到这里。
 * @return        没有报错时为 true；命名冲突或导出契约被破坏时报告错误并返回 false。
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
    scanUnitNeeds(&g, m);
    initRuntimeText(&g, arena);
    collectEmitted(&g, m);
    scanUnitReclaim(&g, m);
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
    /* Final assembly: the pieces whose need is discovered during generation are
     * spliced in ahead of the bodies - the print runtime and the descriptor
     * table, emitted on demand according to what genPrint printed, and the slice
     * helpers. The order is prototypes, then descriptors, then bodies, which is
     * what the define-before-use rule of C requires. */
    g.out = out;
    assembleCoroutineRuntime(&g, out, arena);
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
    finishUnit(&g, out, arena);
    return !ctx->hasError;
}
