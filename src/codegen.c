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
#include "pools.h"
#include "coroutine.h"      /* the pool registry runtime (POOLS.md, now POOLS.md) */
#include "memfind.h"        /* the byte-search runtime (std::sys::mem), emitted on demand */
/* The checker owns the rules this pass has to agree with, so it includes the checker's
 * header rather than restating them: `typeSupportsOp` decides whether an operator applied
 * to an instantiated type is native, and `isEqualityOp` answers the `==` / `!=` pair.
 * `isProtoType` and `enumHasPayload` were duplicated here as static copies of the
 * checker's - the same hazard in a smaller form - and are gone. */
#include "check_internal.h"

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
    /* Names of the view index primitives already emitted (`<view>_index`). They
     * are emitted on demand, at the first subscript that needs one, for the same
     * reason slice helpers are: the need is discovered while generating, and a
     * call site inside a generic body names an instance the list of instances
     * may not hold. */
    Vec         viewIdx;        /* Type *: view instances whose index primitive is needed */
    Vec         viewCpy;        /* Type *: view instances whose bulk-copy primitive is needed */
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

    /* Coroutine lowering (docs/topics/CONCURRENCY.md 4.4). Set while the body of a coroutine is
     * being emitted: the frame is a plain C struct, `yield` stores the pc and returns, and every
     * reference to a frame field is redirected by `coroFieldRef` below. */
    const FuncDef *coroFunc;
    const char    *coroFrame;       /* the frame parameter's C name: `f` */
    int            coroYieldSeq;    /* how many `yield`s have been emitted: the next pc value */
    bool           needEvent;       /* the program calls the event layer (epoll/sockets) */
    bool           needCoroHandle;  /* some coroutine is stored as a handle ⇒ emit the handle type */
    int            coroSeq;         /* names the temporaries a spawn needs (one per spawn) */
    /* Coroutine frames and step functions go here and are appended **last**, after the passes that
     * rewrite the unit by byte offset (same reason as `vtDefs` below): those passes match names, and
     * a coroutine's `counter` matches inside its own `counter$step`. Nothing calls a step function
     * from extC yet (slice B2), so it needs no prototype and C is happy with the definition late. */
    Buf            coroDefs;
    bool        noArena;   /* This function puts nothing into its own block arena,
                            * so neither `extc_arena __extc_a[N]` nor the release
                            * calls are emitted. The checker decides this
                            * (`f->mayUseArena`); it saves compile time. */
    int         loopLevel[64];
    int         loopLen;
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
    Vec         owSites;
    /* Call sites in this function whose callee needs cells (`Expr*`, in order
     * of discovery). Each of them passes one `void *` cell per callee site, so
     * the callee reuses the caller's storage when it is itself @overwrite. */
    Vec         owCalls;
    bool        owLocal;   /* Does the current function keep its @overwrite cells in
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
     * (docs/DECISIONS.md 89), which is a decision code generation has to make per
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
     * formatter (measured: 9 lines of every `io::cout` program's generated C). */
    Buf         rtCoutF64;
    bool        needCoutF64;
    bool        needCout;
    Buf         rtDie;          /* `extc_die`, the dying hook - ahead of the trap bodies */
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
    bool        needRuntime;
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
    /* Is the function being generated marked `@unchecked`? Then its element indexes are lowered
     * without the bounds check (see `FuncDef.isUnchecked`). Saved and restored around every body,
     * so the answer is per function: the flag is the whole granularity the annotation promises. */
    bool        uncheckedIdx;
    /* Does the program declare a pool extern? Then the registry runtime is emitted, and
     * every place that can create one carries the two hooks that key its pools to it. */
    bool        needPool;
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
    bool        zoneMark[64];
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
    /* The runtime primitive block, captured whole as it is emitted. clang reports an
     * unused `static inline` where gcc does not, so this is the one place where the two
     * compilers disagree about the same text. The block is scanned afterwards instead of
     * being recorded definition by definition: its primitives share three long string
     * literals with the arena code between them, and splitting those is the text surgery
     * that has failed here before. */
    const char *primText;
    /* Where the body buffer ended up in the finished output: a definition inside a
     * body is recorded as an offset into that buffer, and this turns it into a
     * position in the assembled unit. */
    size_t      bodyOff;
    /* Method-table declarations: emitted with the prototypes (a body that dispatches through a
     * table needs it in scope) while the definitions are appended at the very end. */
    Buf          vtDecls;
} CG;


/* One slice helper: the bounds check and the view construction for `a[lo..hi]`
 * collected into a function. A function rather than an expression so that lo
 * and hi are each evaluated exactly once: written inline, `hi - lo` would
 * mention both bounds twice. */
typedef struct {
    const char *name;
    char       *text;
} SliceHelper;

/* Append one formatted line to the generated output at the current indent.
 *
 * Params:
 *   g   - generator state; supplies the output buffer, the indent and the arena
 *   fmt - printf-style format
 *
 * Notes:
 *   - The formatted line is truncated at 4096 bytes; every caller stays far
 *     below that.
 */
static void cgLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    for (int i = 0; i < g->indent; i++) bufPuts(g->out, "    ");
    bufPuts(g->out, tmp);
    bufPutc(g->out, '\n');
}

/* Append one formatted line to the pending statement prefix.
 *
 * The text is buffered rather than written to the output, because a prefix must
 * appear before the statement that needs it; see flushPrefix.
 */

static void pfLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    for (int i = 0; i < g->indent; i++) bufPuts(&g->prefix, "    ");
    bufPuts(&g->prefix, tmp);
    bufPutc(&g->prefix, '\n');
}

/* Emit the buffered statement prefix and clear it.
 *
 * Must be called before the first line of the statement the prefix belongs to:
 * those lines compute the temporaries the statement then reads, and that
 * ordering is the whole point of the prefix.
 */

static void flushPrefix(CG *g) {
    if (g->prefix.len == 0) return;
    bufPuts(g->out, bufCstr(&g->prefix));
    bufInit(&g->prefix, g->arena);
}

/* Substitute the TY_PARAM types of the current instance context.
 *
 * Returns:
 *   The substituted type, or t itself when no instance context is active.
 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
}

/* Enter the monomorphization context of one generic instance.
 *
 * Params:
 *   inst - the instance type; a generic enum instance has no `sdef`, its owner
 *          is `edef`, so both cases are handled here
 */
static void substEnter(CG *g, Type *inst) {
    /* A generic enum instance has no `sdef`; its owner is `edef`. */
    g->substParams = inst->sdef ? &inst->sdef->typeParams : &inst->edef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
}

/* Leave the substitution context entered by substEnter. */
static void substLeave(CG *g) {
    g->substParams = NULL;
    g->substArgs   = NULL;
    g->ownerPrefix = NULL;
}


/* Return the C spelling of an extC type.
 *
 * Returns:
 *   A string such as "int32_t" or "pair_i32_u8 *"; a composite spelling is built
 *   in the generator's arena. An error or unresolved type yields "int", which
 *   only matters for a program that has already failed to check.
 */
static const char *cFuncName(CG *g, FuncDef *f);   /* defined below; `cType` needs it */

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
            if (t->sdef && isProtoType(t, "coroutine", 1)) return "extc_coro";
            /* A synthesized coroutine frame: `struct <cname>$frame`, with the function's C name (so
             * methods and generic instances mangle the same way everywhere). */
            if (t->sdef && t->sdef->coroOf)
                return arenaPrintf(g->arena, "struct %s$frame", cFuncName(g, t->sdef->coroOf));
            return t->name;
        /* A `dyn` value is the runtime's handle: the same `{pid, slot, gen}` triple the pool
         * runtime defines, which is what `extc_dyn_put` returns and `extc_dyn_slot` checks. */
        case TY_DYN:    return "ExtcDynHandle";
        case TY_GENERIC:
            /* `coroutine<T>` instances are handles too (the decorated name is not emitted). */
            if (t->sdef && isProtoType(t, "coroutine", 1)) return "extc_coro";
            return t->name;                 /* already a decorated name */
        case TY_ARRAY:  return t->name;     /* likewise: array_15_i32 */
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

/* ---------------------------------------------------------------- expressions */

static const char *genExpr(CG *g, Expr *e);
static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);
static const char *zoneArgRef(CG *g, Expr *e);   /* the zone argument of a pool-creating callee */

static bool isPlaceExpr(const Expr *e);

    

/* Map an extC symbol name to a fragment usable inside a C identifier.
 *
 * An operator is spelled `==` in extC but cannot be spelled that way in C, so
 * every operator that can be a method name is renamed here. The set below has
 * to cover every operator the language lets a method be named after: a name
 * that fell through would be pasted into the generated C as-is, and `ver_<`
 * does not compile. The error would then point into the generated C, where the
 * user cannot see their own source - the failure mode this function exists to
 * prevent.
 *
 * A user name can also collide with a C keyword; writing tests hit this with
 * `fn double(...)`. In extC `double` is not a keyword (the float types are
 * `f32` and `f64`), so it is a perfectly legal user name, yet the generated
 * `int32_t double(int32_t);` does not compile - and the error points into the
 * generated C, where the user cannot see their own source. A `__c` suffix
 * avoids that without changing a single character of the extC source. The
 * keyword set lives in `cIdentIsKeyword` (base.c) and is shared with the
 * checker.
 *
 * Params:
 *   name - the extC symbol or identifier
 *
 * Returns:
 *   The C identifier fragment; freshly built names live in the generator's
 *   arena, so the result outlives the call.
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

/* Name of a function in the generated C.
 *
 * A method name is prefixed with its owner so that `Point_eq` and `Board_eq`
 * cannot collide; in extC they were already two distinct names.
 *
 * Returns:
 *   The C name, allocated in the generator's arena.
 */
/* The suffix an operator's C name needs when a type defines it more than once.
 *
 * An operator may be defined once per right-operand type, so the operand type has to be
 * part of the C name or the definitions collapse into one symbol. This lives in one
 * function because both the definition side (`cFuncName`) and every call site
 * (`cMethodName`) have to agree -- they did not, once: the definition was emitted as
 * `out_shl_i64` while the call asked for `out_shl`, and the C compiler reported the
 * missing function rather than anything about operators.
 *
 * Returns:
 *   `_<mangled operand type>` when the owner defines this operator more than once, and an
 *   empty string otherwise, so a single definition keeps the name it always had. */
static const char *opOverloadSuffix(CG *g, FuncDef *f) {
    if (!f->owner || f->params.len < 2 || !isOverloadableOp(f->name)) return "";
    size_t same = 0;
    for (size_t i = 0; i < f->owner->methods.len; i++)
        if (strcmp((*(FuncDef **)vecAt(&f->owner->methods, i))->name, f->name) == 0) same++;
    if (same <= 1) return "";
    Param *p1 = *(Param **)vecAt(&f->params, 1);
    Type *rt = subst(g, p1->type);
    /* A writable view and the read-only one are **different types** to the operator
     * rule (`ttEquals` compares `mut`) but the **same C struct**, so `ttMangle` gives
     * both the name `slice_u8` and two overloads of `<<` collided in C:
     *     redefinition of `fs$outStream_shl_slice_u8`
     * The suffix has to carry the mutability itself, or the exact-type dispatch that
     * makes overloading decidable is not expressible in C. */
    const char *mangled = ttMangle(g->tt, rt);
    if (rt && rt->mut) mangled = arenaPrintf(g->arena, "mut_%s", mangled);
    return arenaPrintf(g->arena, "_%s", mangled);
}

static const char *cFuncName(CG *g, FuncDef *f) {
    /* An instance of a generic free function carries its own C name (eq2_i32). */
    if (f->instName) return f->instName;
    const char *suffix = opOverloadSuffix(g, f);
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s%s", g->ownerPrefix, cSymName(g, f->name), suffix);
    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return arenaPrintf(g->arena, "%s%s", cSymName(g, f->name), suffix);
}

/* Name of a method in the generated C, at a call site.
 *
 * The receiver picks the prefix: a generic instance uses its own name
 * (`Pair_i32_u8_getFirst`), anything else uses the owner of the method.
 *
 * Params:
 *   recvType - static type of the receiver
 *
 * Returns:
 *   The C name, allocated in the generator's arena.
 *
 * Notes:
 *   - cFuncName must not be used here: it prefixes the instance currently being
 *     generated, whereas the callee may belong to a different type - calling
 *     Point.== from inside Wrapper<Point> is exactly that case.
 */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    const char *suffix = opOverloadSuffix(g, f);
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s%s", rb->name, cSymName(g, f->name), suffix);
    /* cFuncName cannot be used here: it prefixes the instance currently being
     * generated, but the callee may belong to another type (calling Point.==
     * from inside Wrapper<Point>). */
    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return cSymName(g, f->name);
}

/* Is this type a slice instance?
 *
 * This is the only thing the compiler knows about slices, and it is an output
 * convention rather than knowledge of the container: a view of bytes prints as
 * text, which is basic output, not a property of the container. The definition,
 * fields and methods of the container all live in stdlib/prelude.extc.
 *
 * Returns:
 *   true for `slice<T>` with exactly one type argument.
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

/* Is this a view of bytes, that is `slice<u8>`?
 *
 * Returns:
 *   true when the element type is `u8`; `println` then prints the view as text
 *   instead of as a list of numbers.
 */
static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/* Emit the index primitive of one view type: `<C name>_index(v, i, file, line)`.
 *
 * The view is taken by value, so a subscript expression evaluates its operands
 * exactly once and there is no second code path for values and references. The
 * primitive bounds-checks and traps with the extC position when the index is
 * out of range. The `get`, `==` and `find` functions of the prelude all reach
 * it through `self[i]`, which is what keeps pointer arithmetic and hand-written
 * bounds checks out of the library entirely.
 *
 * Notes:
 *   - The emitted function returns a pointer to the element and the call site
 *     dereferences it. Returning the element by value would break two things: a
 *     `ref` parameter needs an lvalue, so `slice<struct>::==` - written as
 *     `self[i] != other[i]` with `self: ref T` - would not compile at all (a
 *     bug that really happened), and an assignment such as `s[i] = x` or
 *     `s[i].field = x` needs an lvalue as well. Returning a pointer covers
 *     both, and a view has reference semantics anyway: that is the `ref` in
 *     `data: ref T`.
 */
static void genViewIndexer(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    substEnter(g, inst);
    cgLine(g, "static inline %s *%s_index(%s v, int64_t i, const char *file, int line) {",
           cType(g, elem), inst->name, inst->name);
    g->indent++;
    cgLine(g, "if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);");
    cgLine(g, "return &v.data[i];");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g);
}

/* Emit the bulk-copy primitive of a view type on demand (`copyInto`).
 *
 * One call checks the count ONCE and then moves the bytes with `memmove`, so the check is
 * paid per **call** rather than per element -- which is the whole reason this primitive
 * exists (a byte-at-a-time loop pays a bounds check, and in the containers also a stale-pool
 * guard, for every byte). `memmove` rather than `memcpy`: the common caller shifts a buffer
 * onto itself (`string::removeFront`), where the ranges overlap.
 *
 * The count check traps with a source position, like every other out-of-range access. */
static void genViewCopier(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    substEnter(g, inst);
    cgLine(g, "static inline int64_t %s_copy(%s d, %s s, int64_t n, const char *file, int line) {",
           inst->name, inst->name, inst->name);
    g->indent++;
    cgLine(g, "if (n < 0 || n > d.len || n > s.len)");
    cgLine(g, "    extc_trapMsg(file, line, \"copyInto: the count is beyond a slice length\");");
    cgLine(g, "if (n > 0) memmove((void *)d.data, (const void *)s.data, (size_t)n * sizeof(%s));",
           cType(g, elem));
    cgLine(g, "return n;");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g);
}

/* Emit the index primitive of a view type on demand and return its name.
 *
 * The primitive used to be emitted from the list of type instances alone, and a call site
 * simply assumed it was there. That holds only while the instance is in the list: an index
 * inside a generic body is lowered with the *instance's* name (`slice_slice_u8_index`),
 * while the list is what decided whether the definition existed -- and a view whose element
 * type came from a type parameter was not in the list, so the generated C called a function
 * nobody had emitted and did not compile at all.
 *
 * Emitting on demand is what this file already does for slice helpers (`sliceHelper`), and it
 * keeps the dependency in one place: whoever needs the primitive emits it, and the name is
 * deduplicated so one view type yields one function.
 *
 * Params:
 *   g    - generator
 *   inst - the view instance, already substituted by the caller
 *
 * Returns:
 *   The name of the primitive, `<view name>_index`. */
static const char *viewIndexer(CG *g, Type *inst) {
    const char *name = arenaPrintf(g->arena, "%s_index", inst->name);
    for (size_t i = 0; i < g->viewIdx.len; i++)
        if (strcmp((*(Type **)vecAt(&g->viewIdx, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(&g->viewIdx) = inst;
    return name;
}

/* Same on-demand story as `viewIndexer`, for the bulk-copy primitive. */
static const char *viewCopier(CG *g, Type *inst) {
    const char *name = arenaPrintf(g->arena, "%s_copy", inst->name);
    for (size_t i = 0; i < g->viewCpy.len; i++)
        if (strcmp((*(Type **)vecAt(&g->viewCpy, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(&g->viewCpy) = inst;
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
/* Register that this type needs a descriptor of its own.
 *
 * Deduplication is by C name, because a mutable and a read-only view are the
 * same C struct and share one descriptor. Scalars, `ref` and `slice<u8>` use
 * the shared descriptors and are not registered.
 */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/* Register that this type needs `extc_eq`.
 *
 * Deduplication is by C name. Comparing a container compares its elements, so
 * the elements are registered as well and the closure is closed in
 * emitDescRegion.
 */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

/* Return the C expression denoting the descriptor of a type.
 *
 * Every type this descriptor refers to is registered on the way out, which is
 * what makes the demand set a closure.
 *
 * Returns:
 *   A string such as `&extc_desc_i32` or `&list_i32_desc`, allocated in the
 *   generator's arena.
 */
static const char *descRef(CG *g, Type *t) {
    if (!t) return "&extc_desc_i32";
    needDesc(g, t);                       /* registering the dependency closes the set */
    if (t->kind == TY_BUILTIN) return arenaPrintf(g->arena, "&extc_desc_%s", t->name);
    if (t->kind == TY_REF)     return "&extc_desc_ref";
    if (isByteView(t))         return "&extc_desc_text";
    /* Everything else has one of its own; a generic instance uses its own C
     * name, as in list_i32_desc. */
    return arenaPrintf(g->arena, "&%s_desc", t->name);
}

/* Emit the descriptor of a struct: field names plus `offsetof`.
 *
 * `offsetof` instead of a hand-computed layout means a forgotten field or a
 * wrong layout fails to compile rather than printing garbage.
 *
 * Params:
 *   cname - C name of the struct, which is also the prefix of its descriptor
 *   disp  - name to display; a generic instance displays its template name
 *           (`varArray { ... }`), as it always did
 *   eqFn  - C name of the equality adapter, or NULL when the type has none
 */
/* Remember a top-level definition emitted into the current buffer; `before` is where it
 * started. The finished text decides whether anything names it, exactly like a global. Used in
 * the descriptor pool - definitions that are asked for on demand but not always referenced -
 * and a declaration and a definition are two separate pieces, registered under one name. */
static void deadDefAdd(CG *g, const char *name, size_t before) {
    Buf l;
    bufInit(&l, g->arena);
    bufPutn(&l, g->out->data + before, g->out->len - before);
    DeadDef *d = arenaAllocZero(g->arena, sizeof *d);
    d->name = name;
    d->text = bufCstr(&l);
    *(DeadDef **)vecPush(&g->deadDefs) = d;
}

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

/* Emit the descriptor of an array: an element count and an element descriptor.
 *
 * An array has no field names, so it prints as `[1, 2, 3]`.
 */
static void genArrayDesc(CG *g, Type *arr) {
    size_t adb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s, NULL };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", arr->name), adb);
}

/* Emit the descriptor of a non-byte view, which prints as `[a, b]`.
 *
 * A byte view never reaches here: it shares `extc_desc_text`.
 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    size_t vdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s, NULL };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", v->name), vdb);
}

/* Emit the descriptor of an enum: its variant names, and `<type name>` for a
 * value outside the table.
 *
 * Only the variant name is printed, never the payload, and the text is
 * byte-for-byte what the previous per-type name function produced.
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

/* Emit the definition of every registered descriptor.
 *
 * Notes:
 *   - The loop condition rereads `len` on purpose: `descRef` appends new
 *     dependencies while the definitions are written.
 */
static void emitDescDefs(CG *g) {
    /* The condition rereads `len` on purpose: `descRef` appends dependencies
     * while the definitions are written. */
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
            substEnter(g, t);              /* T in the fields stands for the type arguments */
            if (isView(t)) genViewDesc(g, t);
            else           genStructDesc(g, t->name, t->sdef->name, t->sdef, eqFn);
            substLeave(g);
        } else {
            /* A missing case must fail loudly: silently emitting no descriptor
             * would leave the generated C referring to a symbol that does not
             * exist. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: no descriptor generator for this type");
        }
    }
}

/* Does this type need `eq` generated or filled in? The lookup is by C name. */
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/* Close the `eq` set inwards: a container that needs equality needs it for its
 * elements too.
 *
 * A struct ends the walk, because its equality is delegated through `d->eq` to
 * the `fn ==` the user wrote.
 */
static void closeEqNeeds(CG *g) {
    for (size_t i = 0; i < g->eqNeed.len; i++) {   /* reread: entries are appended */
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind == TY_ARRAY) { needEq(g, t->inner); continue; }
        if (t->kind == TY_GENERIC && isView(t) && !isByteView(t))
            needEq(g, *(Type **)vecAt(&t->targs, 0));
    }
}

/* Write the descriptor pool into `g->desc`.
 *
 * The pool holds the equality adapters and the descriptor definitions and is
 * spliced in between the prototypes and the function bodies.
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

/* Per-type print functions are gone: `_debug`, `_writeText` and `_name` used to
 * be derived here, one printf sequence per type, which a synthetic stress
 * program turned into 2028 functions and 22.9% of its generated C without ever
 * printing anything. What remains is descriptor data plus one `extc_print`.
 */

/* `nativeCmp` used to live here: a second, coarser copy of "can C compare this type
 * natively with `==`?" that answered by kind alone. It said yes for every builtin and
 * every enum, so an instance of a generic that applied `%` to an `f64` - which the
 * checker rejects, because `%` is for integers - was emitted as C `%` and failed inside
 * gcc, in a file the user cannot read. The operator rule now comes from `typeSupportsOp`,
 * the same predicate the checker applies to concrete types, so there is one rule instead
 * of two that can disagree.
 */

/* Find the user-defined operator method of a type.
 *
 * Params:
 *   sym      - symbol to look for, for example "=="
 *   fallback - symbol to accept when `sym` is absent, for example "==" when
 *              looking for "!=" (whose result is then negated); NULL to accept
 *              `sym` only
 *
 * Returns:
 *   The method, or NULL when neither symbol is defined for this type.
 */
static FuncDef *findOpMethod(TypeTable *tt, Type *t, const char *sym, Type *rhs, const char *fallback) {
    Type *b = ttBase(t);
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return NULL;
    /* The checker's lookup, not a second copy of it. This file used to scan the method
     * list itself and compare the operand type without substituting a generic receiver's
     * arguments, while the checker did substitute them -- so for `slice<u8> == slice<u8>`
     * the checker saw the method and code generation did not, took the "native" branch and
     * emitted a bare C `==` on two structs. One lookup, one answer. */
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}

/* `typeHasEq`, which recursively decided whether an element type could be
 * compared, is gone: code generation derives no `_eq` for arrays any more, so
 * nothing on this side asks the question. Comparability is decided by the
 * checker, which reports ``[2]p` cannot be compared: its element type `p` does
 * not define `==` `` when it is violated. The rule used to exist in two places
 * that had to be kept in sync by hand - a real bug came from exactly that - and
 * with one copy left, that class of bug is gone by construction.
 */

/* ------------------------------------------------------------- structural `==`
 *
 * Structural comparison used to derive one `<T>_eq` function per array type -
 * 200 array types meant 2450 lines, 32% of the generated C in one stress
 * program. Now every type goes through the generic `extc_eq` plus its
 * descriptor.
 *
 * `extc_eq(a, b, desc)` takes addresses, so each operand must be a place; an
 * operand that is not one is first stored in a temporary through the statement
 * prefix, for the same reason `println` does it.
 */
/* Return the address of an operand to pass to `extc_eq`.
 *
 * Params:
 *   x - the operand expression
 *   t - its static type, needed when a temporary must be declared
 *
 * Returns:
 *   A C expression of type pointer to t: `&(code)` for a place, the address of
 *   a fresh temporary `__extc_q<N>` otherwise.
 *
 * Notes:
 *   - The temporary goes through the statement prefix, so it is computed before
 *     the enclosing statement runs.
 */
static const char *eqOperand(CG *g, Expr *x, Type *t) {
    const char *code = genExpr(g, x);
    if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
    const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);
    return arenaPrintf(g->arena, "&%s", tmp);
}

/* Emit a structural equality test as one `extc_eq` call.
 *
 * Params:
 *   e   - the binary `==` or `!=` expression
 *   arr - static type of the operands, that is the type being compared
 *
 * Returns:
 *   The call expression; the caller negates it for `!=`.
 *
 * Notes:
 *   - Both sides are evaluated exactly once and in source order, including the
 *     temporaries that land in the statement prefix.
 */
static const char *genEqCall(CG *g, Expr *e, Type *arr) {
    /* Both sides are evaluated once each, in source order, temporaries included. */
    const char *l = eqOperand(g, e->u.bin.left, arr);
    const char *r = eqOperand(g, e->u.bin.right, arr);
    return arenaPrintf(g->arena, "extc_eq(%s, %s, %s)", l, r, descRef(g, arr));
}

/* Emit the equality adapter of a struct.
 *
 * It connects the user-written `fn ==` to the `bool (*)(const void *, const
 * void *)` shape that `extc_eq` expects. Only a struct needs an adapter,
 * because its comparison is arbitrary user code that can only be delegated to.
 *
 * Params:
 *   t - the struct type
 *   m - its `==` method; its first two parameters are the operands
 *
 * Notes:
 *   - Taking the address of an operand must follow the method signature exactly:
 *     each of the first two parameters is passed by address when it is declared
 *     `ref`, and by value otherwise.
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

/* The recursive element-comparison expression used to be built here; it has no
 * callers any more. Array equality goes through genEqCall and the generic
 * `extc_eq`, and struct or slice equality is emitted where the user's method is
 * called, in genBin below.
 */

/* Emit a binary operation.
 *
 * An overloadable operator that the checker already resolved carries the method
 * to call in `e->func`: that covers `==` `!=` and the ordering and arithmetic
 * operators. The same operators are emitted from here
 * when the operand was a type parameter inside a generic: `e->needOp` says the
 * decision was deferred, and the instance context is what makes `T` resolvable
 * now.
 *
 * Returns:
 *   A C expression; the caller is responsible for the surrounding syntax.
 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;

    /* Arrays: the compiler provides `==` and `!=`, because an array has no
     * `sdef` and therefore no method to find.
     *
     * Since the descriptor table exists, no `_eq` function is derived per array
     * type any more; the comparison becomes the generic
     * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
     * implementation. The semantics are unchanged: recurse element by element,
     * and call the user's `fn ==` for a struct element.
     *
     * The condition must not also require `!e->needOp`: a comparison inside a
     * generic is resolved at instantiation, and after substitution the element
     * type can well turn out to be an array - comparing two rows of a
     * `slice<[6]i32>` is that case. That guard used to be here, so such a
     * comparison fell through to the method lookup below and reported
     * "`array_6_i32` needs to define `!=`". It was a false error. */
    if (!e->func) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ARRAY &&
            isEqualityOp(op)) {
            needEq(g, lt);                 /* register: needs to be comparable via extc_eq */
            const char *call = genEqCall(g, e, lt);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
    }

    if (e->func || e->needOp) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        FuncDef *m = e->func
                     ? e->func
                     : findOpMethod(g->tt, lt, op, ttBase(subst(g, e->u.bin.right->type)),
                                    strcmp(op, "!=") == 0 ? "==" : NULL);

        if (!m) {
            /* No method: either C compares the instance natively, or the instance cannot
             * take this operator at all. `typeSupportsOp` is the checker's own predicate
             * for both halves, so this file does not decide the operator rules again.
             * `m` was NULL and the lookup above already tried the `!=` -> `==` fallback,
             * so a true answer here means "native". */
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
        /* The left operand is the object, exactly as a method receiver is, so it goes
         * through the same rule -- a value it must materialize (a chained `<<` passes the
         * result of the previous one is not a place) cannot be addressed directly. */
        l = selfOperandAsParam(g, e->u.bin.left, l, p0, e->u.bin.left->type);
        if (p1->type->kind == TY_REF) r = arenaPrintf(g->arena, "&(%s)", r);

        /* The hidden arguments, exactly as the method-call path emits them (`f->usesHome` /
         * `f->makesPool`): an operator IS a method call, and the checker decided the level
         * for it (`setOpCallArgs`). Only the zone is emitted here; the arena half is PLAN #83
         * and still open, so an operator that allocates and returns the value keeps failing
         * loudly in the C compiler instead of silently leaking. */
        Buf cb;
        bufInit(&cb, g->arena);
        bufPrintf(&cb, "%s(%s, %s", cMethodName(g, e->u.bin.left->type, m), l, r);
        if (m->makesPool) bufPrintf(&cb, ", %s", zoneArgRef(g, e));
        bufPutc(&cb, ')');
        const char *call = bufCstr(&cb);
        return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
    }

    /* Division by zero and an over-wide shift are undefined behaviour in C, so
     * they trap with a source position instead of passing silently. Each
     * operand is evaluated exactly once, which is why they go through a helper
     * rather than a comma expression such as `(check(r), l op r)` that would
     * evaluate one operand twice. */
    {
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
                return arenaPrintf(g->arena, "(%s %s extc_shiftCount((int64_t)(%s), %d, \"%s\", %d))",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right), bits, g->path, e->line);
        }
    }

    return arenaPrintf(g->arena, "(%s %s %s)",
                       genExpr(g, e->u.bin.left), op, genExpr(g, e->u.bin.right));
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

/* Does this type contain a `ref`, directly or nested inside a struct?
 *
 * Such a type must not be zero-initialized with `{0}`, which would build a null
 * reference. This is the defensive branch that lets the C compiler report the
 * mistake instead.
 *
 * Returns:
 *   true when a `ref` appears anywhere inside the type.
 */
static bool needsExplicitZero(Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    /* An enum's zero is one of its variants, not the integer 0, so a struct that starts with an
     * enum has to be initialized field by field (`board b = (board){0};` is an `int`-to-enum
     * conversion, which clang reports as `-Wimplicit-int-enum-cast`). */
    /* Naming the enum's first variant instead of writing 0 removes clang's
     * `-Wimplicit-int-enum-cast`, and it was measured: `examples/tour.extc` grew a nested initializer
     * -
     *     board b = (board){ .cell = (array_4_array_4_color){ { (array_4_color){ { color_red } } } }, ... };
     * against `(board){0}` - which cost 492 bytes of generated code and is much harder to read. The
     * warning is in the documented list instead (docs/WARNINGS.md section 3.1). */
    if (t->kind == TY_ENUM || t->kind == TY_ARRAY) return false;
    if (t->kind != TY_STRUCT || !t->sdef) return false;
    for (size_t i = 0; i < t->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
        if (needsExplicitZero(fd->type)) return true;
    }
    return false;
}

/* Enter the substitution context of the fields of a generic instance.
 *
 * The fields must be substituted with that instance's own type arguments, never
 * with whatever context the caller happens to leave behind: `subst` would then
 * return the type unchanged, and zero-value generation would recurse into
 * itself until the stack overflows.
 *
 * Params:
 *   sd    - the generic struct definition
 *   targs - its type arguments in this instance
 *   saveP - out: the previous `substParams`
 *   saveA - out: the previous `substArgs`
 *   saveN - out: the previous `ownerPrefix`
 */
static void substEnterInst(CG *g, StructDef *sd, Vec *targs, Vec **saveP, Vec **saveA, const char **saveN) {
    *saveP = g->substParams;
    *saveA = g->substArgs;
    *saveN = g->ownerPrefix;
    g->substParams = &sd->typeParams;
    g->substArgs   = targs;
}

/* Enter the substitution context of a generic free function instance.
 *
 * Without it a `T` in the body of the instance - the `T` of `a == b`, say -
 * stays TY_PARAM forever, which either reports a false error or generates wrong
 * C.
 *
 * Params:
 *   f     - the instance; without a template this is a no-op
 *   saveP - out: the previous `substParams`
 *   saveA - out: the previous `substArgs`
 */
static void substEnterFunc(CG *g, FuncDef *f, Vec **saveP, Vec **saveA) {
    *saveP = g->substParams;
    *saveA = g->substArgs;
    if (f && f->tmpl) {
        g->substParams = &f->tmpl->typeParams;
        g->substArgs   = &f->targs;
    }
}
/* Restore the substitution context saved by substEnterFunc. */
static void substLeaveFunc(CG *g, Vec *saveP, Vec *saveA) {
    g->substParams = saveP;
    g->substArgs   = saveA;
}

/* Restore the substitution context saved by substEnterInst. */
static void substLeaveInst(CG *g, Vec *saveP, Vec *saveA, const char *saveN) {
    g->substParams = saveP;
    g->substArgs   = saveA;
    g->ownerPrefix = saveN;
}

/* Emit the zero value of a type.
 *
 * This is the recursive implementation; the declaration above records why a
 * `{0}` is not always good enough.
 */
/* A null pointer with the type the context wants.
 *
 * `((void *)0)` is a perfectly good null in C, and clang's `-Weverything` reports every place one
 * is assigned to a typed pointer: `implicit conversion when initializing 'node *' with an
 * expression of type 'void *' ... is not permitted in C++`. Twenty-six of those across the corpus
 * came from this one spelling. Emitting `(node *)0` says the same thing and says it in the type
 * the reader expects.
 */
static const char *nullValue(CG *g, Type *t) {
    if (t && t->kind == TY_REF && t->inner) return arenaPrintf(g->arena, "((%s *)0)", cType(g, t->inner));
    return "((void *)0)";
}

/* The braces a zero value of type `t` needs when it sits *inside* another initializer.
 *
 * An array type is a struct wrapping `T data[N]`, so it brings two levels here (its own braces and
 * the member array's); a struct brings one. The outermost level costs nothing: it is the compound
 * literal's own brace, which is why `zeroValue` writes `(T){ { ... } }` for an array.
 *
 * The counts were measured against gcc's `-Wmissing-braces`, which is enabled by `-Wall` and asks
 * for a brace per aggregate:
 *     [1025]i64      -> (T){ { 0 } }
 *     [4]?i64        -> (T){ { { 0 } } }
 *     [3][8]i32      -> (T){ { { { 0 } } } }
 *     [5]cell        -> (T){ { { 0 } } }
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

static const char *zeroValue(CG *g, Type *t) {
    if (!t) return "0";
    /* The zero value of an enum with payloads is tag 0 with a cleared payload,
     * which is exactly what `(shape){0}` means: C clears the tag and the whole
     * union, and the tag decides which member is the meaningful one. A `ref`
     * inside the payload of tag 0 is reported by the checker as a type that
     * cannot be zero-initialized. */
    if (t->kind == TY_ENUM)
        return enumHasPayload(t->edef) ? arenaPrintf(g->arena, "(%s){0}", t->name) : "0";
    /* An array is a struct wrapping `data[N]`, so the braces have to follow the nesting or gcc
     * asks for them (`-Wmissing-braces`, gcc only - clang takes any of these):
     *   `[N]i64`  -> `{{0}}`   one brace for the struct, one for `data`, then the first element
     *   `[N][M]i32` -> the element is an array itself, so its own zero value goes inside:
     *                  `{{{0}}}` and so on. The element's zero value is the ordinary one, which is
     *   already braced for structs by the cases below. */
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
        if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", t->name);

        Vec *sp, *sa;
        const char *sn;
        substEnterInst(g, sd, &t->targs, &sp, &sa, &sn);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", t->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");

        substLeaveInst(g, sp, sa, sn);
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

    /* Defensive: a `ref` has no zero value. The checker guarantees that this
     * line is unreachable - a struct containing a `ref` cannot be
     * zero-initialized and such a field cannot be omitted - but should a later
     * path slip through, an identifier that does not exist is emitted and the C
     * compiler reports it, rather than a null reference being inserted quietly.
     */
    /* The zero value of a `?ref T` is `null`, because a nullable reference does
     * have one. Treating every reference as valueless used to make the
     * zero-initialization of a struct with a `?ref` field emit that
     * non-existent identifier; a real bug, found when `var l: list` failed to
     * compile. */
    if (t->kind == TY_REF) return t->nullable ? nullValue(g, t)
                                              : "__extc_reference_has_no_zero_value__";

    return "0";
}

/* Return the zero value of a type as an initializer; see zeroValue. */
static const char *zeroInit(CG *g, Type *t) {
    return zeroValue(g, t);
}

/* Is the generated C expression of a `println` argument an lvalue, that is, can
 * its address be taken?
 *
 * This is not the question isPlaceExpr answers, and confusing the two breaks
 * code:
 *   - the slice expression `s[0..5]` is a place in extC, but its C form is
 *     `slice_u8_slice(s, 0, 5, "...", 54)`, a function call, and `&` on a call
 *     is illegal. That really happened: examples/slices.extc and
 *     euler-sieve.extc stopped compiling.
 *   - the enum variant `color.red` looks like a field access in extC, but the
 *     checker has already replaced it with a constructor, which is not a place,
 *     so this correctly answers false.
 *
 * Returns:
 *   false whenever the answer is uncertain. A wrong false merely costs one
 *   extra copy - what passing the argument by value to the old `_debug` did -
 *   while a wrong true produces C that does not compile.
 */
static bool printArgIsPlace(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;                                  /* root of `x` / `p.f` */
    case EX_FIELD: return printArgIsPlace(e->u.field.obj);
    case EX_INDEX: return printArgIsPlace(e->u.index.obj);
    default:       return false;
    }
}

/* Emit the expression that implements one `print` call.
 *
 * A structured argument - enum, byte view, struct, generic instance or array -
 * is printed through its descriptor as `extc_print(&x, &x_desc)`. That removes
 * per-type print code entirely and leaves one descriptor per type instead. The
 * call needs an address, so an argument that is not a place is first stored in
 * a temporary through the statement prefix: `println("literal")` and
 * `println(makePoint())` both take that path. A C compound literal cannot be
 * used instead, because C refuses to initialize an aggregate from an expression
 * of the same type and gcc answers "incompatible types".
 *
 * Params:
 *   args    - the argument expressions of the call
 *   newline - append a trailing newline as well
 *
 * Returns:
 *   The generated expression, allocated in the generator's arena.
 *
 * Notes:
 *   - Sets `needRuntime` as a side effect; that flag is what pulls the print
 *     runtime into the output.
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

        /* Structured types - enum, byte view, struct, generic instance, array -
         * all print through their descriptor: `extc_print(&x, &x_desc)`. That
         * is what removes per-type print code and leaves only per-type data.
         *
         * The call takes an address, so the argument must be a place. When it is
         * not - `println("literal")` or `println(makePoint())` - the value is
         * first stored in a temporary and that line is emitted before the
         * enclosing statement through the statement prefix. A compound literal
         * `(T){expr}` was tried and does not work: C will not initialize an
         * aggregate from an expression of the same type, and gcc reports
         * "incompatible types". */
        if (bt->kind == TY_ENUM || isByteView(bt) || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;              /* the print runtime must be emitted too */
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

/* Is this expression a place in extC, meaning its address can be taken in C?
 *
 * The same predicate the checker uses, repeated here for a different reason: the
 * generated `&(f())` would not be legal C.
 *
 * Returns:
 *   true for an identifier, for a field of a place, for an index into a place
 *   and for a slice of a place.
 */
static bool isPlaceExpr(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlaceExpr(e->u.field.obj);
    case EX_INDEX: return isPlaceExpr(e->u.index.obj);
    case EX_SLICE: return isPlaceExpr(e->u.slice.obj);
    default:       return false;
    }
}

static const char *homeArg(CG *g, int marked);   /* defined below */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome);
static bool f_owLocal(CG *g, Stmt *s);
static const char *zoneArgRef(CG *g, Expr *e);

/* Emit a method call as a plain function call on its receiver.
 *
 * `a.f(x)` means exactly `f(a, x)`; the only work is making the receiver match
 * the first parameter, which may be a `ref` or a value.
 *
 * Returns:
 *   The call expression, or "0" when the checker left no resolved method.
 */
/* Turn the first operand of a call into what its `self` parameter demands.
 *
 * `a.f(x)` is `f(a, x)` and `a << b` is `(<<)(a, b)`, so both pass the object as the first
 * argument, and both have to make it an address when the parameter is a reference.
 *
 * A temporary operand cannot be addressed -- `&(f())` is not legal C -- so it is
 * materialized first as a compound literal of a one-element array: `(T[]){ f() }` has type
 * `T *`, because the array decays to a pointer. This is what makes a chained call work
 * (`cout << a << b`: the second `<<` receives the value the first one returned), the same
 * trick Rust plays for `next().unwrap()`.
 *
 * Do not write `&((T){ f() })` instead: `(T){ x }` is not a copy in C, it initializes the
 * first member from x, which produces nonsense errors such as `_Bool has = <option_i64>`.
 * Array initialization does go element by element, so `{ f() }` really is "initialize
 * element 0 with one T".
 *
 * A place operand keeps its address, and that matters: taking an address of a copy would
 * turn a write through the parameter into a write into a temporary.
 *
 * Params:
 *   g      - generator
 *   operand- the expression being passed as the first argument
 *   code   - its generated C text
 *   p0     - the receiving parameter
 *   want   - the operand's static type, used when a temporary has to be materialized
 *
 * Returns:
 *   The C text to pass. */
static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want) {
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = want && want->kind == TY_REF;
    if (wantRef && !haveRef) {
        if (isPlaceExpr(operand)) return arenaPrintf(g->arena, "&(%s)", code);
        return arenaPrintf(g->arena, "(%s[]){ %s }", cType(g, subst(g, want)), code);
    }
    if (!wantRef && haveRef) return arenaPrintf(g->arena, "*(%s)", code);
    return code;
}

/* `dyn Trait(x)` as a value: copy the payload into the dyn pool and hand back the handle.
 *
 * The payload goes through a temporary so an rvalue works as well as an lvalue -- this is what
 * `pfLine`, the existing prefix-statement mechanism, is for. The type comes from the checker
 * (`payloadType`): this node's own type is `dyn Trait` by now, and the temporary has to be the
 * payload's C type. */
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

static const char *genMethodCall(CG *g, Expr *e) {
    /* The receiver's type as this instance sees it. Inside a generic body the written type may
     * still mention `T`, and for a call the checker deferred (`#57`, a method on a type
     * parameter) the node carries no `func` at all: the method is resolved here, on the
     * substituted type, the same way the operators are resolved a few functions down. The
     * checker deliberately leaves `e->func` alone -- one template body has many instances, so a
     * `func` stored there would be the wrong method for all but the last one. */
    Type *recvT = subst(g, e->u.method.recv->type);
    FuncDef *f = e->func;
    if (!f) f = findMethod(ttBase(recvT), e->u.method.name);
    /* ---- The **handle** protocol ----
     * The receiver is a `coroutine<T>` (the marker type, whatever the resolved method's owner looks
     * like -- instantiated copies carry a decorated name) and the emitted call dispatches on `kind`.
     * The frame protocol below is unaffected: a frame's type is not a coroutine marker. Placed here,
     * before this function's other early returns, so nothing else can claim the call. */
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
    if (f && f->coroProto && f->owner && f->owner->coroOf) {
        /* Driving a coroutine. The two protocol methods are the state machine's calling convention,
         * so they are emitted inline -- and whether the receiver is the frame itself or a reference
         * to it decides `&x`/`x.ret` versus `x`/`x->ret`. That is what lets a coroutine be driven
         * through a `mut ref` parameter (`fn drive<C>(c: mut ref C)`), which is the shape the
         * generic machinery resolves at instantiation. */
        Type *rt0 = ttBase(subst(g, e->u.method.recv->type));
        bool viaRef = rt0 && rt0->kind == TY_REF;
        const char *rc = genExpr(g, e->u.method.recv);
        if (f->coroProto == 5) {
            /* `send(v)`: put the value in the frame's `in` slot, then resume exactly like `next`. */
            const char *vv = e->u.method.args.len
                                 ? genExpr(g, *(Expr **)vecAt(&e->u.method.args, 0)) : "0";
            pfLine(g, "%s%s in = %s;", rc, viaRef ? "->" : ".", vv);
        }
        if (f->coroProto == 1 || f->coroProto == 5) {
            /* With a task place the step is driven through the driver (it owns the place); without
             * one there is nothing to set up, so the step itself is the driver. */
            FuncDef *cf = f->owner->coroOf;
            return cf->coroNeedsZone
                       ? arenaPrintf(g->arena, "%s$next(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc)
                       : arenaPrintf(g->arena, "%s$step(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc);
        }
        return arenaPrintf(g->arena, "%s%sret", rc, viaRef ? "->" : ".");
    }
    if (!f) return "0";

    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* A stored `dyn` value is a handle, not a payload: it is not re-materialised, and its callee is
     * named by the trait's uniform table instead of a per-type C name. */
    /* Which source is it? By the receiver's **type**, not its node kind: the parser rewrites the
     * immediate form's receiver to the payload, so both forms arrive as ordinary expressions and
     * only the type tells them apart (`dyn Tag` is the stored value). */
    bool dynStored = e->dynTrait && ttBase(recvT) && ttBase(recvT)->kind == TY_DYN;
    if (!dynStored)
        recvC = selfOperandAsParam(g, e->u.method.recv, recvC, p0, recvT);

    const char *fname = dynStored ? NULL : cMethodName(g, recvT, f);
    /* `dyn Trait(x).m(...)`: the callee comes from the trait's table instead of a per-type C
     * name. The field **is** the implementation, so the call keeps the same signature --
     * including hidden parameters, which the table's field types picked up when it was emitted.
     * Nothing else in this function changes: same receiver, same arguments, same extra
     * parameters, a different callee expression. */
    if (e->dynTrait) {
        /* Dispatch through the trait's uniform table (see the table emitter). Two sources:
         *   - `dyn Trait(x).m(...)`: the payload is copied into the pool here;
         *   - `d.m(...)` on a stored value: it **is** a handle already, so only the checked slot
         *     lookup is emitted.
         * Either way the table and the receiver both come from the **checked slot**. */
        /* Two sources, two costs (DYN.md §11, route C):
         *   - `dyn Trait(x).m(...)`: the payload is a **temporary of this expression**, so it
         *     needs no owner at all -- the table is a compile-time address and the receiver is
         *     that temporary. No pool put, no slot, no generation checks: nothing can be stale
         *     inside one expression, and nothing can escape into another (the method takes the
         *     receiver by reference, so the depth rules already forbid storing it).
         *   - `d.m(...)` on a stored value: the value **is** a handle, so the table and the
         *     receiver both come from the **checked slot**. */
        Buf dynSym;
        bufInit(&dynSym, g->arena);
        if (dynStored) {
            /* The receiver may be reached **through a reference** (`fn f(r: ref dyn Tag)`), in
             * which case the C expression is a pointer to the handle: `extc_dyn_slot` takes the
             * handle **by value**, so the pointer has to be dereferenced here. Without this the
             * compiler accepted `ref dyn Tag` and emitted C that did not build
             * (`incompatible type for argument 1 of 'extc_dyn_slot'`). */
            const char *handleC = recvC;
            if (e->dynRecvViaRef)
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
            /* **The boundary of route C.** The payload must be copied into a stack temporary
             * before the call, even though nothing owns it: a method that mutates its receiver
             * (`mut ref Self`) would otherwise write through to the **source object**, while the
             * stored form mutates the pool's copy -- the same expression `dyn Tag(b)` would mean
             * two different things. `recvC` is already a pointer to the payload, so this is one
             * copy of `sizeof(T)`; the optimiser removes it when the payload is a literal. */
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
    /* A method passes the home arena too; the receiver counts as the
     * shallowest mutable reference argument. */
    if (f->usesHome) bufPrintf(&b, ", %s", homeArg(g, e->arenaArg));
    if (f->makesPool) bufPrintf(&b, ", %s", zoneArgRef(g, e));
    /* A method passes the @overwrite cells as well; the receiver counts as the
     * first argument, and the comma handling follows that. */
    owPassCells(g, &b, e, e->u.method.args.len + 1, f->usesHome || f->makesPool);
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/* Find the value written for one field of a struct literal.
 *
 * Returns:
 *   The initializer expression, or NULL when the literal omits the field; the
 *   caller then uses the field's zero value.
 */
static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

/* Emit a struct literal as a C compound literal.
 *
 * Every field is spelled out, an omitted one as its zero value: leaving it to
 * C's implicit zero-fill would turn an omitted `str` field into NULL.
 *
 * Returns:
 *   The compound literal, allocated in the generator's arena; `(int){0}` when
 *   the type is not a struct at all.
 */
/* The body of a struct literal, as either a compound literal or a brace
 * initializer.
 *
 * Params:
 *   g          - generator
 *   e          - the EX_STRUCTLIT node
 *   staticInit - true when this stands as the initializer of a C static object (a
 *                global), where the `(T)` cast is not allowed: C wants a constant
 *                initializer there, and a compound literal is not one. The brace
 *                form is the same list, so the spelling differs by exactly that
 *                prefix.
 *
 * Returns:
 *   The C text, allocated in the generator's arena.
 */
static const char *genStructLitAs(CG *g, Expr *e, bool staticInit) {
    /* Substitute over the whole type first: inside a generic instance the
     * literal still records the template type (`result<T,E>`), whose type
     * arguments are parameters. Taking that type as it is sends the zero-value
     * path a bare `T`, which degrades to `0` and ends up as `.value = 0` in the
     * generated C. This really happened with `result<unit, E>::failure`. */
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

    /* Every field is written out, an omitted one as its zero value: letting C
     * zero-fill it would turn an omitted `str` field into NULL. */
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

static const char *genStructLit(CG *g, Expr *e) { return genStructLitAs(g, e, false); }

/* The initializer of a global, which is a C static object: its initializer has to be
 * one C can fold at compile time. A struct literal is emitted as a brace initializer
 * for that reason (see genStructLitAs); everything else that reaches here was already
 * restricted to constants by the checker. */
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
static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT:
            /* An `f32` literal is written as the float it is: `%g` prints a `double`, and assigning
             * that to a `float` is an implicit narrowing conversion clang reports
             * (`-Wimplicit-float-conversion`; `float e = 3.14;` in `examples/types.extc`). The value
             * is the same either way - the cast just says so. */
            if (e->type && e->type->kind == TY_BUILTIN && e->type->name &&
                strcmp(e->type->name, "f32") == 0)
                return arenaPrintf(g->arena, "(float)%g", e->u.fval);
            return arenaPrintf(g->arena, "%g", e->u.fval);
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:
            /* `"abc"` becomes a byte view of the literal in read-only memory.
             * The length comes from `sizeof("...") - 1`, so C handles escapes
             * and nothing has to parse them here. */
            return arenaPrintf(g->arena,
                "(%s){ .data = (uint8_t *)\"%s\", .len = sizeof(\"%s\") - 1 }",
                cType(g, e->type), e->u.str.text, e->u.str.text);
        case EX_IDENT: {
            const char *cn = e->u.ident.cname ? e->u.ident.cname : e->u.ident.name;
            /* A coroutine's parameters and its live-across-`yield` locals live in the frame, which
             * is a plain struct: every mention of them becomes a field access ✓ (one place decides
             * this, `FuncDef.coroFrame` is the list the checker laid out). */
            if (g->coroFunc && cn) {
                for (size_t i = 0; i < g->coroFunc->coroFrame.len; i++) {
                    const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
                    if (p->cname && strcmp(p->cname, cn) == 0)
                        return arenaPrintf(g->arena, "%s->%s", g->coroFrame, cn);
                }
            }
            return cn;
        }

        /* `*p`: an explicit dereference is a C dereference; whether the target
         * may be written is the checker's business. */
        case EX_DEREF:
            return arenaPrintf(g->arena, "(*(%s))", genExpr(g, e->u.deref.operand));

        case EX_BIN: return genBin(g, e);

        case EX_UN:
            return arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: {
            /* Calling a coroutine is slice B2 (the `coroutine<T>` representation and its
             * `next`/`value`). Until then this is a loud failure at the C compiler, never a silent
             * miscompile -- and a coroutine *definition* alone still compiles, which is what the
             * slice-B1 test drives from a small C harness. */
            /* A handle-protocol call can arrive here too (a `ref coroutine<T>` receiver resolves as
             * an ordinary call, not as EX_METHOD), so both paths share the dispatch. */
            if (e->func && !e->func->isCoro && e->u.call.callee->kind == EX_FIELD) {
                /* `self` is implicit, so a `ref` receiver arrives as a call with **no** arguments and
                 * the receiver sits in the callee's field expression. The receiver's type decides
                 * whether this is the handle protocol. */
                Expr *rf = e->u.call.args.len > 0
                               ? *(Expr **)vecAt(&e->u.call.args, 0)
                               : (e->u.call.callee->kind == EX_FIELD ? e->u.call.callee->u.field.obj
                                                                     : NULL);
                int hp = 0;
                if (rf && isProtoType(ttBase(subst(g, rf->type)), "coroutine", 1))
                    hp = (e->func->name && strcmp(e->func->name, "value") == 0) ? 4
                       : (e->func->name && strcmp(e->func->name, "send") == 0)  ? 5 : 3;
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
                                       cType(g, subst(g, e->func->ret)), viaRef ? "" : "&", rc,
                                       g->path, e->line);
                }
            }
            if (e->func && e->func->isCoro && e->boxedCoro) {
                /* **Boxing**: this coroutine value is stored somewhere that outlives the current C
                 * scope, so its frame goes into the task's own place and the value is a
                 * `{frame, kind, task}` handle -- plain value, 24 bytes, safe to copy and to store in
                 * a container. The setup statements must precede the statement that reads the handle,
                 * so they go to the prefix (C11 has no statement expressions). */
                FuncDef *cf = e->func;
                const char *cn = cFuncName(g, cf);
                int sq = g->coroSeq++;
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
                                   " .task = __extc_czh%d })", sq, cf->coroKind, sq);
            }
            if (e->func && e->func->isCoro) {
                /* A coroutine call that is neither boxed nor driven: still a loud failure at the C
                 * compiler rather than a silent miscompile -- except under `EXTC_CORO_B1_HARNESS`,
                 * which the slice-B1 judge defines so it can drive the generated `$step` from a small
                 * C harness. */
                cgLine(g, "#ifndef EXTC_CORO_B1_HARNESS");
                cgLine(g, "#error \"calling a coroutine lands in slice B2"
                           " (docs/topics/CONCURRENCY.md 4.4)\"");
                cgLine(g, "#endif");
                return "0";
            }
            if (e->u.call.callee->kind != EX_IDENT) return "0";
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
            if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);
            /* `flush()` becomes `fflush(NULL)`; <stdio.h> is already included
             * by the runtime. */
            if (strcmp(name, "flush") == 0) return "fflush((void *)0)";
            /* There is no `ownFd`/`closeFd` here any more: a file descriptor is not owned
             * by the block, so nothing registers one and nothing closes one behind the
             * program's back. `std::fs` closes with the `close(2)` it declares itself (see
             * `docs/topics/IO.md` section 5, decision 79). */
            if (!e->func) return "0";

            /* Only `cSymName` is used, without the owner prefix: a call site
             * names the callee itself, whereas `ownerPrefix` is the instance
             * currently being generated. cFuncName has to tell the two apart,
             * as its comment explains. This also renames a name that collides
             * with a C keyword (`fn double`). */
            name = (e->func && e->func->instName) ? e->func->instName : cSymName(g, name);
            /* 运行期那一层：库里的 `extc_pool_new(parent)` 在收「家 zone」的函数里换成
             * `extc_pool_new_at(parent, __extc_home_zone)` —— 池因此生到**调用者选的
             * 那个地方**去（POOLS.md §3.1 的提权落点，PLAN #87）。库侧一个字都不用改。 */
            bool poolNewAt = (e->func && e->func->body == NULL
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
            /* The callee needs a home arena, so mine is passed down; the
             * current block's arena would be tighter. */
            if (e->func->usesHome) {
                if (e->u.call.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->arenaArg));
            }
            /* `extc_pool_new` 是运行期那一层：在收「家 zone」的函数里换成 `_at` 那一扇门
             * （POOLS.md §3.1 的提权落点，PLAN #87）。库侧因此一个字都不用改。 */
            if (poolNewAt) {
                /* 名字已经换成 `_at` 那一扇门，这里只补第二个实参。
                 * 注意别写成 `e->func->makesPool && ...`：`extc_pool_new` 是 extern，
                 * 工作清单不遍历没有函数体的函数 ⇒ 它的 `makesPool` **是假**的
                 *（被标记的是调用它的那些函数），所以只能按名字认它。 */
                if (g->funcHasZoneParam) {
                    bufPuts(&b, ", ");
                    bufPuts(&b, zoneArgRef(g, e));
                }
            } else if (e->func->makesPool) {
                if (e->u.call.args.len || e->func->usesHome) bufPuts(&b, ", ");
                bufPuts(&b, zoneArgRef(g, e));
            }
            /* The callee needs @overwrite cells, so cells of my own frame are
             * passed down. */
            owPassCells(g, &b, e, e->u.call.args.len, e->func->usesHome || e->func->makesPool);
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        case EX_INDEX: {
            Type *ot = e->u.index.obj->type;
            Type *ob = ttBase(subst(g, ot));
            const char *obj = genExpr(g, e->u.index.obj);
            const char *idx = genExpr(g, e->u.index.index);

            /* Arrays: the length is a compile-time constant, so obj appears
             * exactly once and cannot be evaluated twice. */
            if (ob && ob->kind == TY_ARRAY) {
                /* With a `ref [N]T` base, `obj` is a pointer in C and must be
                 * dereferenced one level first. Otherwise `p[..]` or `p[i]` on
                 * a `ref [8]u8` produced `p.data[..]`, and gcc answered
                 * "'p' is a pointer; did you mean to use '->'?" */
                if (ot && ot->kind == TY_REF)
                    obj = arenaPrintf(g->arena, "(*%s)", obj);
                /* `@unchecked`: the length is still known here, but the programmer has signed
                 * for the range, so the index is used as written. No `extc_checkedIndex` call
                 * means no trap position -- that is the cost of the annotation, and the manual
                 * says so (18-modules.md 12.5). */
                if (g->uncheckedIdx)
                    return arenaPrintf(g->arena, "%s.data[%s]", obj, idx);
                return arenaPrintf(g->arena,
                    "%s.data[extc_checkedIndex((int64_t)(%s), %lld, \"%s\", %d)]",
                    obj, idx, (long long)ob->asize, g->path, e->line);
            }
            if (!isView(ob)) return "0";
            /* The primitive takes the view by value, so a reference is
             * dereferenced. */
            if (ot && ot->kind == TY_REF) obj = arenaPrintf(g->arena, "*(%s)", obj);

            /* `@unchecked`: the view's element is reached through its data pointer directly.
             * The shape is the checked primitive's, `&v.data[i]` written out as an lvalue
             * (`*(p + i)`), so a read, an assignment and a `ref` argument all still work, and
             * the `file`/`line` arguments have nowhere to go because there is no check. The
             * primitive is *not* emitted for this body: nothing in it names the checked one,
             * which is what the generated-C criterion in tests/annot greps for. */
            if (g->uncheckedIdx)
                return arenaPrintf(g->arena, "(*((%s).data + (int64_t)(%s)))", obj, idx);

            /* The primitive returns a pointer and the dereference is an
             * lvalue: it can be read, addressed for a `ref` parameter, and
             * assigned to. Asking for it here is what guarantees the definition
             * exists: an index inside a generic body names the instance, while
             * the list of instances is not necessarily complete. */
            const char *ix = viewIndexer(g, ob);
            return arenaPrintf(g->arena, "(*%s(%s, (int64_t)(%s), \"%s\", %d))",
                               ix, obj, idx, g->path, e->line);
        }

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

        case EX_ASSOC: {
            /* An associated function decorates its C name with the instance
             * name (`option_i64_some`), which is the same decoration rule
             * methods follow, so cMethodName is reused directly. */
            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, cMethodName(g, e->assocOwner, e->func));
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.assoc.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.assoc.args, i)));
            }
            /* An associated function such as `Type::make()` allocates, so the
             * home arena argument is appended. */
            if (e->func->usesHome) {
                if (e->u.assoc.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->arenaArg));
            }
            if (e->func->makesPool) {
                if (e->u.assoc.args.len || e->func->usesHome) bufPuts(&b, ", ");
                bufPuts(&b, zoneArgRef(g, e));
            }
            owPassCells(g, &b, e, e->u.assoc.args.len, e->func->usesHome || e->func->makesPool);   /* @overwrite cells */
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        /* Numeric conversion. Without `convCheck` the checker has proven the
         * value fits and a plain cast is enough; with it the value is checked
         * against a range and traps with a source position. */
        case EX_CONV: {
            Type *t = subst(g, e->u.conv.type);          /* target type, from the checker */
            const char *x = genExpr(g, e->u.conv.operand);
            if (!e->convCheck)
                return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

            /* A checked conversion: look the range up in a table and go
             * through one of the `static inline` helpers. */
            const char *tn = t->name;
            bool isF = ttIsFloat(subst(g, e->u.conv.operand->type));
            if (isF) {
                int64_t lo = 0, hi = 0;
                if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
                else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
                else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
                else if (strcmp(tn,"i64")==0) { lo = 1; hi = 0; }   /* macro form, see below */
                else if (strcmp(tn,"u8")==0)  { lo = 0; hi = 255; }
                else if (strcmp(tn,"u16")==0) { lo = 0; hi = 65535; }
                else if (strcmp(tn,"u32")==0) { lo = 0; hi = 4294967295LL; }
                else { lo = 0; hi = INT64_MAX; }        /* u64 from a float: checked as i64 */
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
            /* When the target is a float, the checked call's result is used directly: the context
             * already asks for a `double`, so the conversion happens implicitly and the explicit
             * cast would be one applied straight to a function call - which clang reports as
             * `-Wbad-function-cast` (a cast around a call does not do what a reader expects). */
            bool toFloat = (strcmp(tn, "f32") == 0 || strcmp(tn, "f64") == 0);
            unsigned long long hi = 0;
            if (toFloat) {
                /* The conversion has to be spelled out (`-Wimplicit-int-float-conversion` is right
                 * that a u64 does not become a double without losing something), but it must not be
                 * applied straight to the call (`-Wbad-function-cast`). The intermediate cast names
                 * the type the call already returns, which is exactly what it is for. */
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

        /* `new T`, `new [N]T` and `new T[n]`: allocate into a block arena and
         * zero the storage. The runtime allocation zeroes on its own, so extC
         * has exactly one rule about fresh memory. */
        case EX_NEW: {
            Type *w = subst(g, e->u.new_.type);
            /* Allocate at the level the checker computed, which may have been
             * promoted because the value is stored into a place that lives
             * further out. This is where memory with automatic cleanup is
             * actually taken. */
            if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, e, "new");
            const char *ar = arenaRefAt(g, e->arenaLevel);
            if (!e->u.new_.count) {
                /* The place of one T (or one [N]T) is simply its address. */
                return arenaPrintf(g->arena,
                    "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), \"%s\", %d))",
                    cType(g, w), ar, cType(g, w), g->path, e->line);
            }
            /* `T[n]` becomes a view `{ data, len }`, with the count evaluated
             * exactly once: an impure count is marked `needTemp` by the checker
             * and stored in a temporary first. */
            const char *n;
            if (e->needTemp) {
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

        case EX_GENCALL: {
            /* `alloc<T>(n)`: ask the arena of the level the checker chose for the place of
             * n values of T, and hand back a pointer to it.
             *
             * Zeroing is not this path's job and never was: `extc_arena_alloc` memsets every
             * allocation, and that is what makes the language's promise hold -- a byte read
             * after its lifetime ends is always an initialized byte (SPEC section 0.6).
             * `allocSlice<T>(n)` used to sit here and gave the zeroing as its reason for
             * existing; it only spelled `new T[n]`, so it was removed (decision 81). */
            /* `poolSlice<T>(rid, n)` / `poolGive<T>(rid, s)`: the same shape, but the memory
             * belongs to the pool's own plate rather than to an arena (POOLS.md 2.1, PLAN
             * #85). `poolGive` is how `grow` hands the replaced buffer back, so a doubling
             * container stops leaving every old generation behind.
             * `poolSliceRaw` / `poolResizeRaw` are the same two calls with the zeroing left
             * out, for a column whose every slot is written before it is read -- see the
             * long note on `checkPoolPrim` in check_expr.c for what may and may not use
             * them, and what the two canaries in tests/pool/run.sh pin down. */
            const char *gcName = e->u.gencall.name;
            if (strcmp(gcName, "poolSlice") == 0 || strcmp(gcName, "poolSliceRaw") == 0
                || strcmp(gcName, "poolResize") == 0 || strcmp(gcName, "poolResizeRaw") == 0
                || strcmp(gcName, "poolGive") == 0 || strcmp(gcName, "copyInto") == 0) {
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
                if (e->needTemp) {
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
                     * the checker already typed. */
                    Expr *dArg = *(Expr **)vecAt(&e->u.gencall.args, 0);
                    Type *vt = subst(g, dArg->type);
                    return arenaPrintf(g->arena,
                        "%s((%s)(%s), (%s)(%s), (int64_t)(%s), \"%s\", %d)",
                        viewCopier(g, vt), cType(g, vt),
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
                return arenaPrintf(g->arena,
                    "(%s){ .data = (%s *)extc_pool_take((int64_t)(%s),"
                    " (int64_t)(%s) * (int64_t)sizeof(%s)), .len = (int64_t)(%s) }",
                    cType(g, st), tn, rid, n, tn, n);
            }
            const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
            const char *n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
            /* The level comes from the checker as well, with `alloc` meaning
             * the current block, so `g->blkLevel` is not counted here. */
            const char *ar = arenaRefAt(g, e->arenaLevel);
            return arenaPrintf(g->arena,
                "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d)",
                tn, ar, n, tn, g->path, e->line);
        }

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
         *     `e->needTemp` branch below relies on.
         *
         * A C conditional evaluates only one side, so the fallback's side
         * effects do not run when the value is present. */
        case EX_COALESCE: {
            Type *mt = e->u.coalesce.main->type;
            const char *m;
            if (e->needTemp) {
                /* An impure subject (`f() ?? -1`) is computed once into a
                 * temporary, and the conditional then reads that variable;
                 * reading one twice has no side effect. flushPrefix emits these
                 * lines before the enclosing statement. */
                const char *tmp = arenaPrintf(g->arena, "__extc_c%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, mt), tmp, genExpr(g, e->u.coalesce.main));
                m = tmp;
            } else {
                m = genExpr(g, e->u.coalesce.main);
            }
            const char *fb = genExpr(g, e->u.coalesce.fallback);

            /* The fallback side is cast to the result type explicitly, because
             * C's `?:` applies the usual arithmetic conversions to its two
             * arms: an `int64_t` and an `int` together can silently widen the
             * result, an f32 payload next to a double literal being the typical
             * case. The checker already guarantees that the fallback fits the
             * result type, so this is belt and braces that also makes the
             * intended type visible in the generated C.
             *
             * Only a builtin scalar is cast this way: C cannot cast to an array
             * type, so `option<[3]i32> ?? arr` would stop compiling, as was
             * measured.
             *
             * The cast goes to the result type, the payload T, not to
             * `option<T>` itself. Casting by the option's type is wrong and was
             * the first version of this code, which produced no cast at all. */
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

        /* `e!` asserts that the value is present and leaves no runtime trace:
         *   - `opt!` and `r!` read the payload, a union member, without testing
         *     the tag
         *   - `p!` on a `?ref T` is that pointer itself; nothing is emitted */
        case EX_SIGN: {
            Type *ot = e->u.sign.operand->type;
            if (ot && ot->kind == TY_REF) return genExpr(g, e->u.sign.operand);
            bool isOpt = isProtoType(ot, "option", 1);
            const char *var = isOpt ? "some" : "success";
            return arenaPrintf(g->arena, "(%s).u.%s._0",
                               genExpr(g, e->u.sign.operand), var);
        }

        /* `null`, the zero value of a nullable reference, is a null pointer in
         * C. The checker has already proven that a null test narrows the value
         * before any use, so no runtime check is generated: what the compiler
         * can prove leaves no trace at runtime. */
        case EX_NULL:
            return nullValue(g, e->type);

        case EX_ENUMVAL: {
            const char *tn = e->u.enumval.typeName;
            const char *vn = e->u.enumval.variant;
            /* The instantiated name of a generic enum (`maybe_i64`) is not in
             * the type table, so the type the checker resolved, recorded in
             * `assocOwner`, is preferred. */
            Type *et = e->assocOwner;
            if (!et && g->tt) et = ttFromName(g->tt, tn);
            /* Inside a generic instance `tn` is the template's name
             * (`option_T`), while the instance's C name is `option_i32`. With
             * an `assocOwner` present, `subst` plus `cType` gives the real
             * name; without that, `varArray<i32>::get()` referred to the
             * non-existent type `option_T`, which was a real bug. */
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

            /* Construction with a payload:
             * `(shape){ .tag = shape_circle, .u.circle = { ._0 = 2.0 } }`. */
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
    }
    return "0";
}

/* Add the automatic dereference of a value position.
 *
 * The checker treats a `ref T` in value position as a `T` and marks that node
 * with `deref`; the dereference itself is added here. So `let y = p` copies the
 * value and `p + 1` adds values, while whether the target may be written stays
 * with `ref` versus `mut ref` in the type.
 *
 * Wrapping genExprInner is what makes this complete: every path that generates
 * an expression, recursive calls included, goes through this function, so no
 * path can forget the dereference.
 */
static const char *genExpr(CG *g, Expr *e) {
    /* The contract is that every caller passes an expression node. Every path that
     * generates a value goes through here, and no field of the AST is optional in
     * that position, so a NULL would be a caller bug; stating it makes the
     * invariant visible to readers and to static analysers alike. */
    assert(e != NULL);
    const char *s = genExprInner(g, e);
    if (e->deref) return arenaPrintf(g->arena, "*(%s)", s);
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

/* A value on its way into an `f32` slot, written as the float it is.
 *
 * `let e: f32 = 3.14` emits `float e = 3.14;`: the literal is a `double`, so the assignment narrows
 * it, and clang says so (`-Wimplicit-float-conversion`, `examples/types.extc`). The value is the same
 * with the cast - it only stops being implicit. */
static const char *asF32(CG *g, Type *target, Expr *src, const char *val) {
    if (!target || target->kind != TY_BUILTIN || !target->name || strcmp(target->name, "f32") != 0)
        return val;
    if (!src || !src->type || src->type->kind != TY_BUILTIN || !src->type->name) return val;
    if (strcmp(src->type->name, "f64") != 0) return val;
    return arenaPrintf(g->arena, "(float)%s", val);
}

/* -------------------------------------------------------------- conditions
 * A statement brings its own parentheses, and the expression printer adds a pair
 * around a comparison: `if x == y` therefore came out as `if ((x == y))`, which
 * clang reports as `-Wparentheses-equality`. Strip a pair that wraps the whole
 * condition, and only such a pair: a `)` that does not close at the very end
 * (one inside a string literal, say) leaves the text alone, which costs one
 * redundant pair and never changes what the condition means.
 */
static const char *cgCond(CG *g, const char *e) {
    if (!e || e[0] != '(') return e;
    int depth = 0;
    for (size_t i = 0; e[i]; i++) {
        if (e[i] == '(') depth++;
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
 * `option` and `result` used to be structs with `has` and `ok` fields, which
 * this code read and rebuilt field by field. They are ordinary enums now, so
 * everything below goes through a tag comparison and a payload path.
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
/* Return the C path of the payload on the success side: `x.u.some._0` or
 * `x.u.success._0`.
 */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/* Build the value to return when the operand of `?` failed.
 *
 * The failure value is constructed in the return type of the enclosing function.
 * For an `option` that is its zero value, which is `none`; for a `result` it is
 * a failure variant carrying the inner error.
 *
 * Returns:
 *   A C expression of that return type, or "0" when there is no return type.
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

/* Decrement the recursion depth counter on one exit path.
 *
 * Params:
 *   g - generator state; emits nothing when the function is not self-recursive
 *
 * Notes:
 *   - The decrement must follow the computation of the return value, never
 *     precede it. Writing it first turned `return spin(n)` into
 *     `--depth; return spin(n);`, which resets the counter to zero in every
 *     frame of a tail call, so the guard never fires. At -O0 gcc really does
 *     fold that into a tail call and the program exits at once with no output,
 *     and at -O2 it loops forever; both are wrong.
 *   - A `return` statement is not the only exit. Hanging the decrement on
 *     cgReturn alone missed the implicit return at the end of a function body:
 *     the function `cdq` of bench/oi/p3810.extc has no explicit return, so its
 *     depth only ever grew and a legitimate program reported "recursion too
 *     deep" after a hundred thousand frames. The end of the body therefore
 *     decrements as well, and an explicit return that already decremented
 *     through cgReturn does not do it a second time.
 */
static void cgRecLeave(CG *g) {
    if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
}

/* Return a value from the current function.
 *
 * Every exit goes through one shared epilogue rather than repeating the release
 * sequence of the arenas at each return. The value is stored in `__extc_ret_v`,
 * the recursion depth is decremented, and control jumps to `__extc_ret`, where
 * that sequence appears once. Repeating it at every exit used to be one of the
 * largest single costs of gcc time on the stress programs, because each copy had
 * to be analysed on its own.
 *
 * Params:
 *   val - C expression of the value to return, or NULL to return nothing
 *
 * Notes:
 *   - A function with `noArena` returns directly: it allocates nothing and has
 *     no arena array to release.
 *   - The value is computed before the arenas are released, which is slightly
 *     more conservative than releasing first and evaluating afterwards.
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

/* Emit the statements that evaluate the operand of `?` once and return on
 * failure, and describe the result to the caller.
 *
 * Returns:
 *   A TryInfo holding the temporary with the operand, the C name of its type and
 *   the variant names; the caller reads the payload through it.
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
         * (docs/DECISIONS.md 89). The payload is printed through its descriptor, which is
         * the same machinery `println(e)` uses -- so `ioError.readFailed(5)` still says
         * which fd failed rather than "something failed" ✓ A `none` has no payload. */
        if (!ti.isOpt) {
            Type *et = subst(g, *(Type **)vecAt(&ot->targs, 1));
            g->needRuntime = true;               /* extc_print lives in the print runtime */
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


/* Emit a `#line` directive pointing back at the .extC source of a statement.
 *
 * Emits nothing when line mapping is off or the statement carries no line.
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
 * the checker here used to be a second authority on the same question.
 *
 * Returns:
 *   A C expression denoting the arena, usable as a function argument.
 */
/* 这个调用点要把哪个 zone 递给被调者 —— 也就是"池该生在哪个地方"。
 *
 * 与 homeArg 平行的一条：arena 那边递的是"调用者选的那只 arena"，
 * 这里递的是"调用者选的那个地方的 zone 下标"。
 *
 *   - 我自己就收了这个参数（`funcHasZoneParam`）⇒ 把它原样往下传：
 *     `vector<i32>::new` → `withCap` → `pool<V>::withParent` → `extc_pool_new_at`
 *     这条链上，用户在最外面选的那个地方一路传到运行期；
 *   - 否则（入口文件里的 main / 普通函数）⇒ 递当前这个地方的 zone，
 *     也就是最近一层压过的 `__extc_zm<lvl>` —— 这与"池生在建它的那个块里"完全一致。
 *
 * 兜底用 `extc_pool_zoneDepth() - 1`（当前顶）：只有 `noArena` 的函数才会走到
 * （它没有压过任何 zone），那正是今天的行为。 */
static const char *zoneArgRef(CG *g, Expr *e) {
    /* 站点自己记了层级（checker 的 `zoneLevel`）就按它发：`ZONE_HOME` = 用我这个函数收到的
     * 家 zone（一路往下传），k>=1 = 第 k 层那个地方的 `__extc_zm<k>`（提权之后这里会变小）。 */
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonearg] %s node=%p lvl=%d mark1=%d mark2=%d blk=%d\n",
                g->curFuncName ? g->curFuncName : "-", (void *)e, e ? e->zoneLevel : 0,
                (int)g->zoneMark[1], (int)g->zoneMark[2], g->blkLevel);
    /* Inside a coroutine's step the place is **read from the frame**: rule 1 of the design says a
     * resume must not re-derive it from the environment, and a step has nothing to re-derive it from
     * anyway -- no hidden parameters, no zone mark of its own (docs/topics/CONCURRENCY.md 4.4). */
    if (g->coroFunc) return "f->zone";
    if (e && e->zoneLevel != 0) {
        if (e->zoneLevel == ZONE_HOME) {
            if (g->funcHasZoneParam) return "__extc_home_zone";
            /* `main` 的 C 签名是固定的，收不到家 zone 参数 ⇒ 它的"家"就是**它自己那个体**
             * （与家 arena 的 `__extc_home = &__extc_a[1]` 同一条口径）。
             *
             * 这里以前直接落到下面那条"最近一层压过的 zone"，而那是个**错的地方**：
             * 一个在内层块里升级出来的池会登记到内层块的 zone 上，出块就被回收，
             * 而接收者还活着。实测（SSO 的 `push` 是第一个"为接收者建池"的方法）：
             * 循环里 push 到第 17 个字节升级 ⇒ 每轮出块收掉池 ⇒ 下一次 `pStale` 把串毒成空，
             * `len=3` 而不是 20；串声明在循环外面时更糟 —— 池已还给 malloc，读到的是复用的内存。
             * 家 zone 更长寿，代价只是内存（与逃逸分析"宁可多算"的方向一致）。 */
            if (g->zoneFrameMarked) return "__extc_zm1";
        }
        if (e->zoneLevel == 1 && g->zoneFrameMarked) return "__extc_zm1";
        if (e->zoneLevel > 1
            && e->zoneLevel < (int)(sizeof g->zoneMark / sizeof g->zoneMark[0])
            && g->zoneMark[e->zoneLevel])
            return arenaPrintf(g->arena, "__extc_zm%d", e->zoneLevel);
    }
    if (g->funcHasZoneParam) return "__extc_home_zone";
    for (int lvl = g->blkLevel; lvl >= 1; lvl--) {
        if (lvl < (int)(sizeof g->zoneMark / sizeof g->zoneMark[0]) && g->zoneMark[lvl])
            return arenaPrintf(g->arena, "__extc_zm%d", lvl);
    }
    /* 兜底 = "我此刻所在的那个地方"，运行期自己的名字就是 `extc_zoneTop` ——
     * 与 `(extc_pool_zoneDepth() - 1)` **完全等价**（运行期里 `zoneDepth() = zoneTop + 1`），
     * 但不再是一步**派生算术**：`extc_zoneTop` 的含义是直接的，`zoneDepth() - 1` 的含义随
     * `zoneDepth()` 的定义漂移。规则 1 关心的正是这种"从环境现算"的形状：
     * 单栈下它就是调用者所在的地方 ✓；**任务体内则不许用它**（那时必须从帧字段读）——
     * `tools/check_concurrency_guards.py` 的 J1 记账并把计数钉住 ✓ */
    /* 兜底会引用运行期的那张 zone 表，所以这里要把池运行期标成"需要" ——
     * 运行期文本在函数体之后才发（`if (g.needPool) poolsEmitRuntime(...)`），还来得及。
     * 走到这一支的典型情形是 `stmtMakesPool` 对"未解析的被调者"保守算真：那个函数其实
     * 不建池，于是既没有 zone 也没有运行期，而调用点仍然要凑出第二个实参。 */
    g->needPool = true;
    return "extc_zoneTop";
}

static const char *homeArg(CG *g, int arenaArg) {
    /* Passed as an argument, so this is the pointer itself. Do not confuse it
     * with arenaRefAt, whose result is wrapped in `&` and which therefore
     * returns `(*__extc_home)`. */
    if (arenaArg == ARENA_HOME) return "__extc_home";
    return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);
}

/* Return the arena that an allocation site at a given level refers to.
 *
 * The level is computed by the checker and recorded in `Expr.arenaLevel`. It
 * starts as the block the statement lives in and has already been promoted when
 * the value is stored into a place that lives further out; inside a function that
 * has a home arena every site is `ARENA_HOME` already.
 *
 * Params:
 *   level - ARENA_HOME, or a block level of 1 or more
 *
 * Returns:
 *   `(*__extc_home)`, the arena the caller chose and the longest lived one, or
 *   `__extc_a[level]`, which is released when block `level` ends.
 *
 * Notes:
 *   - A fallback that returned `(*__extc_home)` whenever the function had a home
 *     arena was removed here. It was a second authority on the level, with the
 *     checker computing one and code generation deciding another; it existed
 *     only because the checker used to compute levels before the call graph was
 *     known. Now the checker rewrites every site from `FuncDef.arenaSites` once
 *     the closure is built, so the fallback is unnecessary, and removing it left
 *     the generated output byte-for-byte identical, which shows the two
 *     authorities agreed all along.
 */
static const char *arenaRefAt(CG *g, int level) {
    if (level == ARENA_HOME) return "(*__extc_home)";
    /* A step has no C-stack arena array: the block arenas live **in the frame** (one field per level,
     * `arena1`..`arenaN`), because a block in a coroutine can span a suspension -- the allocation has
     * to survive the resume, and the block's own release point still reclaims it. */
    if (g->coroFunc)
        return arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, level > 0 ? level : 1);
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/* Report a disagreement between the arena level and the block being generated.
 *
 * This is a drift detector for the level the checker computed: it must agree
 * with the block code generation is currently inside, and a disagreement means
 * the two have started computing the level separately again. It is active only
 * when EXTC_DBG_ARENA is set in the environment, and it changes no output.
 *
 * The two conditions are one-directional and deliberately loose:
 *   - the level must not be 0, which means "not yet decided" and would show
 *     that the checker missed a site
 *   - the level must not be deeper than the current block; promotion only ever
 *     moves an allocation towards a longer lifetime, so a level shallower than
 *     the current block is legal
 */
static void arenaDriftCheck(CG *g, Expr *e, const char *what) {
    if (dbgOn("EXTC_DBG_ARENA_VERBOSE"))
        fprintf(stderr, "[arena-ok?] %s: level=%d block=%d %s:%d\n",
                what, e->arenaLevel, g->blkLevel, g->path, e->line);
    if (e->arenaLevel == 0)
        fprintf(stderr, "[arena!] %s has level 0 (undecided)  %s:%d\n", what, g->path, e->line);
    else if (e->arenaLevel != ARENA_HOME && e->arenaLevel > g->blkLevel)
        fprintf(stderr, "[arena!] %s has level %d, deeper than block %d: the two answers disagree  %s:%d\n",
                what, e->arenaLevel, g->blkLevel, g->path, e->line);
}

/* Does this block need a zone of its own?
 *
 * A zone is the head of the side chain of pools created in one `place`. The only thing
 * that needs one is `extc_pool_new`, which registers the new pool in the zone current at
 * the call and returns -1 when there is none - and a pool that was never registered is
 * never dropped, so its slot is never reused and `extc_pool_live()` counts a record that
 * nothing can reach. The question is therefore "can this block's code reach
 * `extc_pool_new`?".
 *
 * Params:
 *   b - the block statement about to be emitted
 *
 * Returns:
 *   True when a `zoneEnter` / `zoneLeaveTo` pair has to bracket the block.
 *
 * Notes:
 *   - Only the block's *own* statements are asked about, and a nested block is not walked
 *     into: a nested block is a place of its own with its own zone, and it answers for
 *     itself. A loop body is such a block, which is where the win is - a body that only
 *     calls `v.push` used to push and pop a zone per iteration.
 *   - The summary is transitive through the callee (`FuncDef.makesPool`, computed by the
 *     checker as a least fixed point), because a library function is transparent to
 *     `place`: `vector<i32>::withCap` is what calls `extc_pool_new`, and it registers its
 *     pool in the zone of the block that called *it*.
 *   - Over-emitting is the safe direction, under-emitting is not; see `stmtMakesPool` in
 *     check_top.c for how an unresolved callee is treated.
 */
static bool blockMakesPool(Stmt *b) {
    if (!b || b->kind != ST_BLOCK) return false;
    for (size_t i = 0; i < b->u.block.stmts.len; i++)
        if (stmtMakesPool(*(Stmt **)vecAt(&b->u.block.stmts, i), false)) return true;
    return false;
}

/* Release block arenas down to and including one level.
 *
 * Params:
 *   lvl - innermost level to release; 0 or less releases nothing, since level 0
 *         is not a block arena
 *
 * Notes:
 *   - A function with `noArena` emits nothing: it never allocated.
 */
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    /* A step keeps `noArena` (its prologue has no `__extc_a` and its returns take the direct path),
     * but it still releases block levels -- out of the frame's per-level arenas. */
    if (g->noArena && !g->coroFunc) return;   /* never allocates, so nothing to release */
    /* Memory only. A block used to close the descriptors it owned first, which is why
     * this is the one place a block release is emitted -- but files are the program's
     * business now (decision 79), so there is nothing to close here. */
    /* Regions first, then the arena: a pool's memory is its own malloc block, so the two are
     * independent, but releasing the pools that belong to this block is what the block's
     * release point is for (POOLS.md section 3.4 - leaving a block takes its subtree). */
    /* 离开一个地方：弹回它开始时的深度（mark），而不是盲目弹一个 —— 早退路径
     * （return/break/continue）因此不会把 zone 栈弄歪。
     *
     * 发射与弹出靠 `zoneMark` 配对，`zoneHere` 只说明"这个函数是个地方"：块的 zone 现在
     * 是按需压的，所以某层没压却弹一次，弹掉的就是**外层**的 zone（外层建的池随后就没有
     * zone ⇒ `extc_pool_new` 返回 -1 ⇒ 记录不再被回收）。表在进入块时写，这里读，两者
     * 是同一份事实，不可能走散。 */
    if (lvl < (int)(sizeof g->zoneMark / sizeof g->zoneMark[0]) && g->zoneMark[lvl])
        cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lvl);
    /* A coroutine body owns no place of its own, so it must not release the **caller's** arena levels
     * -- that release would free, at every resume, exactly what the next resume still needs. Its own
     * block arenas live in the frame (see `arenaRefAt`), and a block's exit still reclaims its level. */
    if (g->coroFunc)
        cgLine(g, "extc_arena_release(&%s->arena%d);", g->coroFrame, lvl);
    else
        cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/* Destroy the frame's block arenas, at the one point where a coroutine is finished.
 *
 * A step never reaches the shared epilogue (it returns `true` at every yield), and the body's own
 * level lives until the task ends rather than until the step returns -- so the levels a finished
 * coroutine still holds have to go here, **before** the driver's `extc_task_end` frees the frame
 * itself. A dropped, never-finished coroutine leaks the blocks its frame arenas still hold; that is
 * bounded by the deferral and noted in docs/topics/CONCURRENCY.md 4.4. */
static void cgCoroArenaDestroy(CG *g) {
    if (!g->coroFunc) return;
    int n = 1 + blkMaxOfBlock(((FuncDef *)g->coroFunc)->body);
    for (int lv = 1; lv <= n; lv++)
        cgLine(g, "extc_arena_destroy(&%s->arena%d);", g->coroFrame, lv);
}

/* Collect every `@overwrite` site of one function body, in source order.
 *
 * The order is what makes the numbering stable: the prologue emits one storage
 * cell per site in this same order, and owIndex looks a site up by position.
 */
static void collectOwSites(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.overwrite) *(Stmt **)vecPush(out) = s;
        return;
    case ST_BLOCK:
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

/* Return the index of one `@overwrite` site within the current function.
 *
 * Returns:
 *   Its position in `g->owSites`, or -1 when it is not registered.
 *
 * Notes:
 *   - The prologue emits the cells in the same order, so the two sides agree.
 *     The index is looked up rather than stored in the AST, because one body is
 *     visited once per generic instance and a stored index would be overwritten
 *     by the next instance.
 */
static int owIndex(CG *g, Stmt *s) {
    for (size_t i = 0; i < g->owSites.len; i++)
        if (*(Stmt **)vecAt(&g->owSites, i) == s) return (int)i;
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

/* Collect the cell-needing call sites of one expression tree; the declaration
 * above records why every expression kind must be covered.
 */
static void collectOwCallsExpr(Expr *e, Vec *out) {
    if (!e) return;
    switch (e->kind) {
/* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every
     * walker has to look inside it -- the walkers end in `default:`, which is why `-Wswitch`
     * never pointed at the omission. */
    case EX_DYN:
        collectOwCallsExpr(e->u.dynv.payload, out);
        return;
    case EX_CALL:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.call.args, i), out);
        return;
    case EX_METHOD:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
            *(Expr **)vecPush(out) = e;
        collectOwCallsExpr(e->u.method.recv, out);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.method.args, i), out);
        return;
    case EX_ASSOC:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
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
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.arraylit.elems, i), out);
        return;
    case EX_REF:      collectOwCallsExpr(e->u.ref.operand, out); return;
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

/* Collect the call sites of one statement tree whose callee needs cells.
 *
 * Params:
 *   out - receives the Expr* of every such call site, in discovery order
 */
static void collectOwCallsStmt(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_YIELD:  collectOwCallsExpr(s->u.yield_.value, out); return;
    case ST_VAR:    collectOwCallsExpr(s->u.var.init, out); return;
    case ST_ASSIGN: collectOwCallsExpr(s->u.assign.target, out); collectOwCallsExpr(s->u.assign.value, out); return;
    case ST_EXPR:   collectOwCallsExpr(s->u.expr.expr, out); return;
    case ST_RETURN: collectOwCallsExpr(s->u.ret.value, out); return;
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

/* Return the index of one call site within the current function.
 *
 * Returns:
 *   Its position in `g->owCalls`, or -1 when it is not registered. The prologue
 *   emits the cells in the same order, so the two sides agree.
 */
static int owCallIndex(CG *g, Expr *e) {
    for (size_t i = 0; i < g->owCalls.len; i++)
        if (*(Expr **)vecAt(&g->owCalls, i) == e) return (int)i;
    return -1;
}

/* Does the current function keep its `@overwrite` cells in its own frame?
 *
 * Yes for `main` and for recursive functions, whose frame is the one that stays
 * alive; FuncDef.owLocal carries the answer. The statement argument is unused,
 * because the answer is a property of the function.
 */
static bool f_owLocal(CG *g, Stmt *s) { (void)s; return g->owLocal; }

/* Append the cell arguments of one call site whose callee needs `@overwrite`
 * storage.
 *
 * Params:
 *   b       - buffer holding the argument list, without the closing parenthesis
 *   e       - the call site
 *   nargs   - number of arguments already in the list, used for the comma
 *   hasHome - whether a home arena argument was already appended
 *
 * Notes:
 *   - A call site that was not registered passes nothing; the callee has a
 *     fallback for that, so the program stays defined.
 */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome) {
    if (!e->func || e->func->owSites == 0 || e->func->owLocal) return;
    int j = owCallIndex(g, e);
    if (j < 0) return;                       /* not registered; the callee has a fallback */
    for (int k = 0; k < e->func->owSites; k++)
        bufPrintf(b, "%s&__extc_owc%d_%d", (nargs || hasHome || k) ? ", " : "", j, k);
}

/* How many block levels one function can use at the same time.
 *
 * The answer sizes the `__extc_a` array, which is a fixed-length array on the
 * stack, so setting up the block arenas needs no allocation.
 */
int blkMaxLevel(Stmt *s);

/* Return the deepest block level inside one statement that is a block.
 *
 * Returns:
 *   The deepest level reached inside the block, not counting the level the block
 *   itself adds; 0 when there is no block.
 */
int blkMaxOfBlock(Stmt *block) {
    int m = 0;
    if (!block || block->kind != ST_BLOCK) return blkMaxLevel(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        if (blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i)) > m)
            m = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));
    return m;
}
/* Return the deepest block level reached inside one statement.
 *
 * A block counts as one level, and the loops, conditionals and matches inside it
 * add their own, so the result is the number of arena slots the enclosing
 * function needs.
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
    case ST_WHILE: return 1 + blkMaxOfBlock(s->u.whiles.body);
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

/* Emit the body of one block, clearing its arena on entry and releasing it on
 * exit.
 *
 * Params:
 *   block - the block statement; its statements are generated in order
 *
 * Notes:
 *   - A loop body is a block too, so every iteration clears the arena on entry.
 *     That is what bounds the memory of a loop by a single iteration instead of
 *     by the number of iterations.
 */
static void genBlockBody(CG *g, Stmt *block) {
    /* Only functions with a body reach here (`genFunc` returns early for an
     * external declaration, which has none), and every other caller passes a
     * block statement it just inspected. */
    assert(block != NULL && block->kind == ST_BLOCK);
    g->blkLevel++;
    /* `zoneMark` and `loopLevel` both have room for this depth: the prologue asked for
     * `1 + blkMaxOfBlock(f->body)` levels. Say so rather than walk off the array if that
     * ever stops being true. */
    assert(g->blkLevel < (int)(sizeof g->zoneMark / sizeof g->zoneMark[0]));
    /* Entering a block overwrites this level's mark, so the one that was there is saved
     * and put back on the way out. Level 1 is the function body, whose mark the prologue
     * set (it pushed `__extc_zm1`, and the epilogue reads the mark to decide whether to
     * pop it): a plain `= false` here would erase that decision. Two sibling blocks at the
     * same level both write it, and nothing looks at a level after its block has been
     * left, but the save costs a byte and removes the question. */
    bool savedZoneMark = g->zoneMark[g->blkLevel];
    if (!g->noArena) {
        if (!g->coroFunc)   /* a coroutine's storage is the task's, not this block's */
            cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);   /* clear */
        /* 进入一个地方：压一个 zone，标记按层号命名（同级块不嵌套，名字不冲突）。
         * 只在这个块**可能建池**时才压（`blockMakesPool`）：只调用 `v.push` 这类不建池的
         * 库函数的循环体不需要自己的 zone，而每个循环体压/弹一次正是这轮要消掉的开销。
         * 弹出发射与否记进 `zoneMark`，`cgReleaseLevel` 查同一张表。 */
        g->zoneMark[g->blkLevel] = false;
        if (g->zoneHere && g->blkLevel > 1 && blockMakesPool(block)) {
            if (!g->coroFunc)      /* see the note on the release below: the task owns the place */
                cgLine(g, "int64_t __extc_zm%d = extc_pool_zoneEnter();", g->blkLevel);
            g->zoneMark[g->blkLevel] = true;
        }
    }
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
    cgReleaseLevel(g, g->blkLevel);
    g->zoneMark[g->blkLevel] = savedZoneMark;
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/* Emit one statement, with a statement prefix allowed around it.
 *
 * Everything the statement needs evaluated early is written to the prefix here,
 * before the statement itself, and the prefix block counter tells the prefix
 * mechanism that it is inside a statement and may emit lines at all.
 */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

/* Emit the statement itself; the prefix bookkeeping lives in genStmt. */
static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_VAR: {
            /* `let c = counter(args)`: **spawn**. The frame is a plain value living here, and the
             * arguments initialize the parameters -- a resume has none to pass them again. */
            if (s->u.var.cname && s->u.var.init && s->u.var.init->kind == EX_CALL &&
                s->u.var.init->func && s->u.var.init->func->isCoro) {
                FuncDef *cf = s->u.var.init->func;
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
                              " .task = __extc_czh%d };", s->u.var.cname, sq, cf->coroKind, sq);
                    g->needCoroHandle = true;
                    g->needPool = true;      /* the task table brings the zone runtime with it */
                    return;
                }
                const bool hasZone = cf->coroNeedsZone;
                const int  sq = g->coroSeq++;
                if (hasZone) {
                    flushPrefix(g);
                    cgLine(g, "int64_t __extc_czsv%d = extc_zoneTop;", sq);
                    cgLine(g, "int64_t __extc_czid%d = extc_task_begin();", sq);
                    cgLine(g, "int64_t __extc_czm%d = extc_task_zone(__extc_czid%d);", sq, sq);
                }
                /* `var d = inner(args)` **inside a coroutine**: the nested frame is a *field* of this
                 * frame, not a C local -- a resume jumps past the declaration, so the initializer has
                 * to be an assignment to the field (same rule as every other frame-field local
                 * below). Emitting a local here left the field uninitialized and drew an
                 * "unused variable" warning from the generated C. */
                bool framed = false;
                if (g->coroFunc) {
                    for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
                        const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
                        framed = p->cname && strcmp(p->cname, s->u.var.cname) == 0;
                    }
                }
                Buf init;
                bufInit(&init, g->arena);
                if (framed)
                    bufPrintf(&init, "%s->%s = (%s){ .pc = 0", g->coroFrame, s->u.var.cname, fr);
                else
                    bufPrintf(&init, "%s %s = (%s){ .pc = 0", fr, s->u.var.cname, fr);
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
                return;
            }
            /* A frame field is not declared here: the frame struct holds it, and the assignment is
             * what a resume re-runs (the initializer runs on every execution, exactly like the C
             * local it replaces). */
            if (g->coroFunc && s->u.var.cname) {
                bool framed = false;
                for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
                    const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
                    framed = p->cname && strcmp(p->cname, s->u.var.cname) == 0;
                }
                if (framed) {
                    if (s->u.var.init) {
                        const char *v = genExpr(g, s->u.var.init);
                        flushPrefix(g);
                        cgLine(g, "%s->%s = %s;", g->coroFrame, s->u.var.cname, v);
                    }
                    return;
                }
            }
            /* `cname` is the name the checker decided on; a shadowed one
             * carries a `__2` suffix. */
            const char *nm = s->u.var.cname ? s->u.var.cname : s->u.var.name;
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
                 * reuses, and the zero-value contract of `new` is unchanged.
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
                     * level 1 of this frame. `arenaRefAt(nx->arenaLevel)` must
                     * not be used here: that number is the callee's own level,
                     * while the cell lives elsewhere, and the two do not share a
                     * lifetime. Using it made a later call memset a block that
                     * had already been freed - an ASan use-after-free, and
                     * without ASan glibc handed the block back and the program
                     * merely looked as if it worked. */
                    const char *owcv = arenaPrintf(g->arena, "__owc%d", k);
                    const char *ar = arenaPrintf(g->arena, "(*%s->home)", owcv);
                    (void)nx->arenaLevel;
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
                         * self-assignment and leaving the binding null, which
                         * showed up as an ASan segmentation fault. */
                        const char *owtp = arenaPrintf(g->arena, "__owp%d", k);
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
                        if (nx->needTemp) {
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
                    return;
                }
            }
            /* `let q = f()?` and `var q = f()?` are one of the legal positions
             * of `?`. This case was missing once: the checker allowed it while
             * code generation expanded `?` only in an assignment, a return and
             * an expression statement, so it fell through to the EX_TRY branch
             * of genExpr and reported an internal error, which reproduced every
             * time. The shape matches the assignment case, because the generated
             * C is a declaration followed by an initialization. */
            if (s->u.var.init && s->u.var.init->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.var.init);
                flushPrefix(g);
                size_t lb = g->out->len;
                cgLine(g, "%s %s = %s;", cType(g, s->type), nm, tryPayloadPath(g, &ti));
                localDef(g, lb, nm, 1);
                return;
            }
            const char *init = s->u.var.init ? genExpr(g, s->u.var.init)
                                             : zeroInit(g, s->type);
            flushPrefix(g);
            size_t lb = g->out->len;
            cgLine(g, "%s %s = %s;", cType(g, s->type), nm, asF32(g, s->type, s->u.var.init, init));
            localDef(g, lb, nm, 1);
            return;
        }

        case ST_ASSIGN:
            /* An assignment whose value is a `?` reads the payload; the failure
             * path has already returned by then. */
            if (s->u.assign.value && s->u.assign.value->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.assign.value);
                const char *at = genExpr(g, s->u.assign.target);
                flushPrefix(g);
                cgLine(g, "%s = %s;", at, tryPayloadPath(g, &ti));
                return;
            }
            const char *tgt = genExpr(g, s->u.assign.target);
            const char *val = genExpr(g, s->u.assign.value);
            flushPrefix(g);
            /* `x += y` is emitted as `x += y`: C evaluates the target **once**, which is
             * exactly the meaning the statement has, and it is what a reader expects to
             * find. A user-defined operator is a method, so that one case becomes the plain
             * assignment of its call - the checker has already required a target with no
             * call in it, because the call names the target a second time. */
            if (s->u.assign.op) {
                if (s->u.assign.opExpr && s->u.assign.opExpr->func)
                    cgLine(g, "%s = %s;", tgt, genBin(g, s->u.assign.opExpr));
                else
                    cgLine(g, "%s %s %s;", tgt, s->u.assign.op, val);
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

        case ST_WHILE: {
            const char *cnd = cgCond(g, genExpr(g, s->u.whiles.cond));
            flushPrefix(g);
            cgLine(g, "while (%s) {", cnd);
            g->indent++;
            g->loopLevel[g->loopLen++] = g->blkLevel + 1;   /* the body is the next level */
            genBlockBody(g, s->u.whiles.body);
            g->loopLen--;
            g->indent--;
            cgLine(g, "}");
            return;
        }

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
            cgLine(g, "%s", s->kind == ST_BREAK ? "break;" : "continue;");
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
            cgLine(g, "%s;", ex);
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_MATCH: {
            /* The scrutinee is evaluated exactly once, into a temporary when
             * the enum has a payload, because an arm reads `.u.<variant>` and
             * evaluating the expression a second time - as in
             * `match f() { ... }` - would be wrong. */
            /* `subst` matters here for the same reason it does on the construction side
             * (`EX_ENUMVAL`): inside the body of a generic instance the scrutinee's
             * recorded type is the *template's* type, so `et->name` was `option_V` and the
             * arm compared against `option_V_none`, a constant that does not exist -- the
             * generated C did not compile at all. Substituting first gives the instance's
             * name (`option_i64`), and the payload types below are then already concrete. */
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
             * would never terminate. A read loop hung on exactly that.
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
                /* Only the bindings of *this* arm: the body may contain nested matches whose
                 * bindings already know their own stage, and overwriting theirs with this
                 * larger one made a nested `e` count as mentioned as soon as a sibling nested
                 * arm used its own `e` - which kept lines that nothing reads. */
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
    }
}

/* ---------------------------------------------------------------- top level */

/* Build the C parameter list of a function.
 *
 * A function whose allocations may escape takes one hidden arena pointer at the
 * end: the `new` expressions inside it allocate into the arena chosen by the
 * caller. `@overwrite` cells are passed in the same way, as opaque
 * `extc_owcell *`, so a caller does not need to know their types and a generic
 * instance needs no special case.
 *
 * Returns:
 *   The parameter list text, without the surrounding parentheses.
 */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* C fixes the signature of `main`, so it takes no hidden parameter; its
     * home arena is the local `extc_arena *__extc_home = &__extc_a[1]` in its
     * own body. A parameter the language wrote is not a C parameter either:
     * `main(args)` (docs/topics/IO.md section 7) is built from `argc`/`argv` in
     * the prologue, so C still sees exactly the two arguments it insists on. */
    if (!f->owner && strcmp(f->name, "main") == 0) {
        bufPuts(&sig, f->params.len ? "int argc, char **argv" : "void");
        return bufCstr(&sig);
    }
    if (f->params.len == 0 && !f->usesHome && !f->makesPool && f->owLocal) {   /* no hidden parameters follow */
        bufPuts(&sig, "void"); return bufCstr(&sig);
    }
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (i) bufPuts(&sig, ", ");
        bufPrintf(&sig, "%s %s", cType(g, p->type), p->cname ? p->cname : p->name);
    }
    if (f->usesHome) {
        if (f->params.len) bufPuts(&sig, ", ");
        bufPuts(&sig, "extc_arena *__extc_home");
    }
    /* 建池的函数多收一个隐藏的「家 zone」——arena 那个隐藏参数的镜像
     *（`makesPool` 是可传递的最小不动点，所以整条链上每个函数都收得到）。
     * 容器"生在哪里"因此由**调用者**决定，而不是由库内部那一层词法块决定。 */
    if (f->makesPool) {
        if (f->params.len || f->usesHome) bufPuts(&sig, ", ");
        bufPuts(&sig, "int64_t __extc_home_zone");
    }
    /* @overwrite cells live in the frame of the call site and are passed in as
     * opaque `extc_owcell *`: opaque means a caller does not need to know the
     * types of the callee's sites, and a generic instance needs no special
     * case. */
    if (!f->owLocal) {
        for (int i = 0; i < f->owSites; i++) {
            if (f->params.len || f->usesHome || f->makesPool || i) bufPuts(&sig, ", ");
            bufPrintf(&sig, "extc_owcell *__extc_owarg%d", i);
        }
    }
    return bufCstr(&sig);
}

/* Does this statement always return?
 *
 * Only the simplest shapes are recognized: a `return`, or a block whose last
 * statement is one.
 *
 * Returns:
 *   true only when the statement definitely returns; an unsure case answers
 *   false. That is deliberately conservative: a false negative merely emits one
 *   more `return`, which is dead code but compiles, whereas the opposite mistake
 *   leaves a missing return that gcc reports as an error.
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
 * The answer decides whether a recursion guard is emitted. Runaway recursion
 * used to be silent: gcc either folded it into an endless loop that printed
 * nothing, or the stack overflowed and the OS reported a segmentation fault,
 * which is neither an extC message nor a position in the source. Only a
 * self-recursive function pays for the guard, so an ordinary call costs one
 * increment and nothing else.
 *
 * The reachability uses the call graph the checker already recorded in
 * `e->func`, so no second AST walker is needed, and it is transitive: in the
 * mutual recursion `g -> h -> g`, `g` counts as self-recursive. That shape is
 * the one that used to run for 60 seconds without printing anything.
 *
 * Returns:
 *   true when some path through the call graph leads back to f.
 *
 * Notes:
 *   - The graph is small and the answer is computed once per generated function,
 *     so the plainest fixed-point iteration is good enough.
 */
static bool funcCallsItself(CG *g, FuncDef *f) {
    if (!f || !f->body) return false;
    /* Direct calls, `EX_ASSOC` and `EX_METHOD` included; both carry `e->func`. */
    Vec seen;  vecInit(&seen, g->arena, sizeof(FuncDef *));
    Vec work;  vecInit(&work, g->arena, sizeof(FuncDef *));
    *(FuncDef **)vecPush(&work) = f;
    *(FuncDef **)vecPush(&seen) = f;
    bool hit = false;
    for (size_t i = 0; i < work.len; i++) {
        FuncDef *cur = *(FuncDef **)vecAt(&work, i);
        if (!cur || !cur->body) continue;
        if (cur == f && i > 0) { hit = true; break; }   /* reached again: recursive */
        for (size_t j = 0; j < cur->callees.len; j++) {
            FuncDef *nx = *(FuncDef **)vecAt(&cur->callees, j);
            if (!nx) continue;
            if (nx == f) { hit = true; break; }
            bool dup = false;
            for (size_t k = 0; k < seen.len && !dup; k++)
                if (*(FuncDef **)vecAt(&seen, k) == nx) dup = true;
            if (!dup) { *(FuncDef **)vecPush(&seen) = nx;
                        *(FuncDef **)vecPush(&work) = nx; }
        }
        if (hit) break;
    }
    return hit;
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
 * registers the generic instances its body calls (docs/topics/CONCURRENCY.md 4.4, slice C). */
/* The handle type and its two helpers. Emitted only when a handle exists somewhere; the helpers are
 * EXTC_UNUSED so that a `value` helper for an unused yield type stays quiet under -Werror. */
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
        if (!cf || !cf->isCoro || cf->tmpl) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);", cn, cn);
        if (cf->coroNeedsZone)
            cgLine(g, "EXTC_UNUSED static bool %s$next(struct %s$frame *f);", cn, cn);
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
        if (!cf || !cf->isCoro || cf->tmpl) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "    case %d: return %s%s((struct %s$frame *)h->frame);",
               cf->coroKind, cn, cf->coroNeedsZone ? "$next" : "$step", cn);
    }
    cgLine(g, "    }");
    cgLine(g, "    return false;");
    cgLine(g, "}");
    /* One `value` helper per yield type that a handle can carry. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !cf->isCoro || cf->tmpl || !cf->yieldType) continue;
        const char *yt = cType(g, cf->yieldType);
        bool done = false;
        for (size_t k = 0; k < i && !done; k++) {
            FuncDef *prev = *(FuncDef **)vecAt(&m->funcs, k);
            if (prev && prev->isCoro && !prev->tmpl && prev->yieldType &&
                strcmp(cType(g, prev->yieldType), yt) == 0) done = true;
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
            if (!ck2 || !ck2->isCoro || ck2->tmpl || !ck2->yieldType) continue;
            if (strcmp(cType(g, ck2->yieldType), yt) != 0) continue;
            const char *cn2 = cFuncName(g, ck2);
            cgLine(g, "    case %d: return ((struct %s$frame *)h->frame)->ret;", ck2->coroKind, cn2);
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
            if (!ck3 || !ck3->isCoro || ck3->tmpl || !ck3->yieldType) continue;
            if (strcmp(cType(g, ck3->yieldType), yt) != 0) continue;
            const char *cn3 = cFuncName(g, ck3);
            cgLine(g, "    case %d: ((struct %s$frame *)h->frame)->in = v; return %s%s("
                      "(struct %s$frame *)h->frame);",
                   ck3->coroKind, cn3, cn3, ck3->coroNeedsZone ? "$next" : "$step", cn3);
        }
        cgLine(g, "    }");
        cgLine(g, "    return false;");
        cgLine(g, "}");
    }
}

static void genCoroDecls(CG *g, Module *m) {
    bool taskTable = false;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *c0 = *(FuncDef **)vecAt(&m->funcs, i);
        if (c0 && c0->isCoro && !c0->tmpl && c0->coroNeedsZone) { taskTable = true; break; }
        /* A scheduler written in extC reaches the table through `std::sys::coroutine`; those
         * declarations are ordinary externs, so their presence is what pulls the runtime in. */
        if (c0 && c0->isExtern && c0->name && strncmp(c0->name, "extc_task_", 10) == 0) {
            taskTable = true; break;
        }
    }
    /* The task table, defined before every body that may spawn or drive: the pool prototypes are
     * already in the prototype region above, so this only needs to precede its users. */
    if (taskTable) {
        /* The table sits on the pool zone runtime (enter/leave), so a program that only spawns
         * coroutines has to pull that in too -- the same rule the zone-argument fallback follows. */
        g->needPool = true;
        coroutineEmitRuntime(g->arena, g->out);
    }
    genCoroHandleDecls(g, m);
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isCoro || f->tmpl) continue;     /* instances: per-instance frames come later */
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);",
               cFuncName(g, f), cFuncName(g, f));
        /* The **task driver**, defined here (before every body that may drive it): the place is read
         * from the frame -- never re-derived at a resume (rule 1) -- made current for the step, and
         * restored afterwards so the caller's place is untouched. When the step reports "done", the
         * task's whole place goes in one release: the author's B decision for slice C. */
        if (f->coroNeedsZone) {
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

static void genFunc(CG *g, FuncDef *f) {
    /* The coroutine protocols (`next`/`value`) have no body: they are emitted inline at their call
     * sites (see genMethodCall). Never emit one as a function, whichever path got here. */
    if (f->coroProto) return;
    /* A coroutine's step is generated **through this flow** (docs/topics/CONCURRENCY.md 4.4, slice C):
     * emitting a body is what registers the generic instances it calls, and a definition produced
     * outside this flow called helpers nothing ever emitted. Only two places differ -- the signature
     * below and the ending at the epilogue. */
    if (f->isCoro) {
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
        bufPrintf(&sig, "%s%s %s(%s) {", f->isInline ? "EXTC_INLINE " : "static ",
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
    bool savedUnchecked = g->uncheckedIdx;
    g->uncheckedIdx = f->isUnchecked;
    /* ---- One arena per block level ----
     * The number of levels is known at compile time, being the maximum nesting
     * depth of the blocks, so the array has a fixed length and lives on the
     * stack: setting up the arenas needs no allocation. `{0}` is enough, since
     * an `extc_arena` holds only a `top` pointer and NULL means empty. The
     * function body itself is level 1; genBlockBody increments on entry. */
    int maxLv = 1 + blkMaxOfBlock(f->body);
    /* A function that puts nothing into its own block arenas does not even emit
     * the array. The checker decides that (`f->mayUseArena`: the body contains a
     * `new`, or calls a function that has a home arena), and about half of the
     * functions in real programs are pure computation. cgReleaseLevel skips the
     * release for them as well. */
    bool savedNoArena = g->noArena;
    bool savedOwLocal = g->owLocal;
    g->owLocal = f->owLocal;
    /* The @overwrite storage cells, one per site, declared in the prologue.
     * The order matters: a cell is initialized with `&__extc_a[1]` (see `home`
     * below), so the list of sites has to be collected before the decision
     * whether to emit the arena array at all can be taken. */
    Vec savedOw = g->owSites;              /* by value: no arena exists for the first function */
    Vec owNow; vecInit(&owNow, g->arena, sizeof(Stmt *));
    collectOwSites(f->body, &owNow);
    g->owSites = owNow;
    Vec savedOwCalls = g->owCalls;
    Vec owcNow; vecInit(&owcNow, g->arena, sizeof(Expr *));
    collectOwCallsStmt(f->body, &owcNow);
    g->owCalls = owcNow;
    /* A cell is initialized with `&__extc_a[1]`, so the arena array must be
     * emitted whenever there is any cell, even in a function that allocates
     * nothing. An @overwrite variable in `main` is exactly that case, and
     * missing it left `__extc_a` undeclared. */
    g->noArena = !f->mayUseArena && !(owNow.len > 0 || owcNow.len > 0);
    /* A step owns no arena array: it is not a place, and what it allocates belongs to the task's own
     * place (docs/topics/CONCURRENCY.md 4.4, slice C). Emitting one also made it unused, which
     * `-Wall -Werror` rejects. */
    if (g->coroFunc) g->noArena = true;
    /* `main(args)` builds the view of `argv` in `&__extc_a[1]`, so the arena
     * array is needed even when the body allocates nothing. A program that only
     * prints its arguments is exactly that case. */
    if (mainArgs) g->noArena = false;
    /* `main`'s home arena **is** `&__extc_a[1]` (emitted below): it has no caller to hand
     * one in, so the storage is its own body. When `main` only passes that arena down -
     * `fn main() -> i32 { return caller() }` over a generic that allocates - its body holds
     * no `new` of its own, `mayUseArena` is false, and the prologue still pointed at an
     * array that was never declared: gcc reported ``__extc_a' undeclared` on
     * `tests/arena-promoted/R2a_generic_instance_first.extc`. Every other function receives
     * `__extc_home` as a parameter and needs no array of its own. */
    if (isMain && f->usesHome) g->noArena = false;
    /* Same reason, for pools: the frame mark and the per-block release hooks hang off the
     * arena's lifetime, so a `main` that only *uses* a pool (it allocates nothing itself)
     * still needs the frame. Phase 1 keys pools to `main`'s frame; when `new (r) T[n]`
     * lands, every function that can create one gets the same treatment. */
    if (isMain && g->needPool) g->noArena = false;
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    /* The frame mark: pools created while this frame runs are linked to it, so leaving
     * the frame drops them whatever their block did (POOLS.md section 3.5). Emitted where
     * the arena exists, because hanging pools off an arena level is what this frame does. */
    /* 这个函数是不是「一个地方」：只有入口文件的函数是，库函数对地方透明
     * （`pool<T>::withCap` 建的池属于它的调用者所在的地方）。判据是两个名字里都没有 `$` ——
     * loader 把模块的函数改成 `mod$name`，泛型实例还把带 `$` 的名字放在 `instName` 里。 */
    {
        bool lib = (f->name && strchr(f->name, '$')) ||
                   (f->instName && strchr(f->instName, '$'));
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zone] name=%s mod=%s inst=%s owner=%s line=%d\n",
                    f->name ? f->name : "-", f->modName ? f->modName : "-",
                    f->instName ? f->instName : "-", f->owner ? "yes" : "-", f->line);
        /* 「一个地方」= 入口文件里的函数。`modName` 是 loader 给的模块名：
         * 库方法有它（实测 `withCap` 是 `mod=pool`），入口文件为空，所以库函数对地方透明 ——
         * `pool<T>::withCap` 里建的池属于它的调用者所在的地方。 */
        (void)lib;
        g->zoneHere = g->needPool && (!f->modName || !*f->modName);
        /* main 的 C 签名是固定的，收不了隐藏参数 ⇒ 它用当前那个地方的 zone（今天的行为）。 */
        g->funcHasZoneParam = f->makesPool && !isMain;
    }
    /* 函数体的 zone（`__extc_zm1`）同样按需发射：没有这个函数，就没有任何池会登记在它的帧上，
     * 压了也是空压。`zoneMark[1]` 记下这次决定，收尾的 `zoneLeaveTo(__extc_zm1)` 查它 ——
     * 没压却弹会弹掉**调用者**的 zone。 */
    g->zoneMark[1] = false;
    g->zoneFrameMarked = false;
    if (dbgOn("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonehook] %s zoneHere=%d noArena=%d makesPool=%d needPool=%d isMain=%d\n",
                f->name ? f->name : "-", (int)g->zoneHere, (int)g->noArena,
                (int)f->makesPool, (int)g->needPool, (int)isMain);
    /* A step is not a place of its own: the task's place (slice C) is what its storage hangs off, so
     * it must not open a zone of its own here -- that mark also went unused in the generated C. */
    if (g->zoneHere && !g->noArena && f->makesPool && !g->coroFunc) {
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
        /* `self` 常常用不到（库里 16 处 ✓）⇒ 明确 `(void)` 掉：接收者是**签名**的一部分，
         * 用不用是函数体的事 ✓ 生成物不该因此刷警告 ✗ */
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
    if (f->owLocal)
        for (size_t i = 0; i < owNow.len; i++)
            /* `home` says which arena the storage belongs to: with a home, the
             * home arena, which outlives this call and therefore really is
             * reused; otherwise level 1 of this frame, which certainly outlives
             * the statement. It is never NULL. */
            cgLine(g, "extc_owcell __extc_ow%zu = { 0, 0, %s };", i,
                   f->usesHome ? "__extc_home" : "&__extc_a[1]");
    (void)owcNow;   /* no cells are prepared for a callee: they live in its frame */
    /* `owSites` must not be restored here: the body has not been generated yet,
     * and restoring it early made every lookup answer -1 and silently fall back
     * to allocating on every iteration. The restore happens at the end of
     * genFunc, together with `noArena`. */
    /* The slot the shared epilogue returns through; it is needed only when this
     * function really releases an arena on the way out. */
    bool retVoid = !g->retType || g->retType->kind == TY_VOID;
    /* `main` returns through it as well, and that is the point: its value is the process
     * exit code, so it has to survive the release sequence like any other return. It used
     * to be thrown away here - `fn main() -> i32 { var p = new i32  return i32(*p) }`
     * emitted `(void)(*p)` and the process exited 0 - which CONTRADICTS the manual ("`main`
     * 的返回值就是进程退出码"). The slot is skipped only for `noArena`, where `cgReturn`
     * returns the value directly and the declaration would be unused. */
    bool retSlot = !g->noArena && !retVoid;
    if (retSlot)
        cgLine(g, "%s __extc_ret_v;", cType(g, g->retType));
    g->blkLevel = 0;
    g->loopLen  = 0;
    const char *savedFuncName = g->curFuncName;
    g->curFuncName = cFuncName(g, f);
    if (isMain && f->usesHome) {
        size_t hb = g->out->len;
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* main's home arena is its own body */");
        localDef(g, hb, "__extc_home", 0);   /* the declaration sits above the body */
    }
    /* The buffered console tail has one guaranteed exit.
     *
     * `main` flushes on every explicit `return` and in the arena epilogue, but a body that
     * simply runs off its end passes through neither: a program whose last statement is
     * `io::cout << "qw\n"` printed nothing at all. A trap exits from wherever it stands,
     * and so does `sys::proc::exit`. One `atexit` covers every way out, including those
     * two, and the explicit flushes stay for the ordering they give inside the program.
     * Registered only when the program uses that console at all (`needCout`), so a program
     * without it does not pull the flush function in. */
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
        g->retType = savedRet;
        g->inMain  = false;
        g->tmpSeq = savedSeq;
        g->noArena = savedNoArena;
        g->owSites = savedOw;
        g->owCalls = savedOwCalls;
        g->owLocal = savedOwLocal;
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
    /* A function without an arena has no epilogue below, so nothing catches a body
     * that runs off its end - and a non-void one would then return whatever the
     * register happened to hold. clang reports exactly that as `-Wreturn-type`, and
     * one shape it cannot see through is a `match` whose arms all return: the arms
     * become an `if / else if` chain, which says nothing about being exhaustive.
     * The trap is the honest answer for the path that no arm matched, and because
     * `extc_trapMsg` is `noreturn` the C compiler stops asking for a value there.
     *
     * It applies to a function with an arena as much as to one without: the arena only
     * changes where the value is parked on the way out, and without the trap that path
     * returned the slot the return statements never wrote. Measured on
     * `fn f(x: bool) -> i32 { var p = new i32  if x { return i32(*p) } }` called with
     * `false`: the caller received whatever the stack held. */
    if (fallsOff && !retVoid)
        cgLine(g, "extc_trapMsg(\"%s\", %d, \"a non-void function reached its end without returning\");",
               g->path, f->line);
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
        for (int lv = 1; lv <= maxLv; lv++)
            cgLine(g, "extc_arena_destroy(&__extc_a[%d]);", lv);
        /* 弹回到最外层那个压过的 zone：早退（`goto __extc_ret`）和走到函数末尾都从这里出去，
         * 而跳出去的路径可能把若干个块 zone 留在栈上，所以从最深的开始每层补一次。
         * 多弹是幂等的（`zoneLeaveTo` 弹到某个 mark 为止），少弹会让池留在一个已经被压掉的
         * 层级上。`zoneMark[1]`（帧自己的 zone）由序言决定：没压当然不弹。 */
        for (int lv = maxLv; lv >= 1; lv--)
            if (lv < (int)(sizeof g->zoneMark / sizeof g->zoneMark[0]) && g->zoneMark[lv])
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
    g->uncheckedIdx = savedUnchecked;
    g->noArena = savedNoArena;
    g->owSites = savedOw;          /* restored last, see the note above */
    g->owCalls = savedOwCalls;
    g->owLocal = savedOwLocal;
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
 * it reject outer-loop vectorization outright.
 *
 * Measured on a matrix multiply of size n=1000 at OI scale, 10^9 multiply-adds,
 * with the same code differing only in linkage:
 *     static 208 ms   vs   extern 660 ms   => 3.2x
 * The reports from -fopt-info-vec-missed were "unsupported outerloop form" for
 * the extern version and "outer-loop already vectorized" for the static one.
 * Internal linkage also brings cross-function inlining and stronger constant
 * propagation for free, and `-Wl,--gc-sections` stops being the only safety net.
 *
 * Should extC ever support several translation units, which it does not today,
 * this has to become "export only what another unit uses".
 */

/* Is this function the entry point of the program?
 *
 * Returns:
 *   true for a free function named `main`.
 */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

/* Emit the forward declaration of one function, so that definitions may appear
 * in any order.
 */
static void genFuncProto(CG *g, FuncDef *f) {
    /* A coroutine emits no prototype for its own name: there is no C function called `counter`, the
     * artifact is `counter$frame` + `counter$step` (emitted together, so the frame is defined before
     * it is used). Emitting an undefined prototype also poisons the byte-offset passes later on --
     * `counter` matches inside `counter$step` and they blank the text in between. */
    if (f->isCoro) return;
    Buf sig;
    bufInit(&sig, g->arena);
    /* `@inline` has to appear on the prototype as well as on the definition. */
    bufPrintf(&sig, "%s%s %s(%s);",
              cgIsMain(f) ? "" : (f->isInline ? "EXTC_INLINE " : "static "),
              cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
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

/* Return the C name of one definition unit.
 *
 * Returns:
 *   The instance name of a generic instance or array, including a generic enum
 *   instance, the name of a non-generic enum with payloads, or the struct name.
 */
static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* instance or array, enum instances included */
    if (u->td) return u->td->name;          /* non-generic enum with payloads */
    return u->sd->name;
}

/* Find the unit that defines a type.
 *
 * Returns:
 *   The index of that unit in `units`, or -1 when there is none.
 *
 * Notes:
 *   - A generic instance is matched by C name, not by pointer, for two reasons.
 *     A `mut slice<T>` is a shadow sharing its name and its C struct with the
 *     read-only view, so comparing pointers never finds it, the dependency
 *     ordering misses it, and `struct reader { chunk: mut slice<u8> }` fails
 *     with an incomplete type. And one C name can have two instances, one for
 *     the writable and one for the read-only view.
 */
static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_ENUM && u->td && t->edef == u->td) return (int)i;
        /* A generic instance is matched by C name, not by pointer. A `mut
         * slice<T>` is a shadow sharing its name and its C struct with the
         * read-only view, so comparing pointers never finds it and the
         * dependency ordering misses it, which made
         * `struct reader { chunk: mut slice<u8> }` fail with an incomplete
         * type. And one C name can have two instances, one for the writable and
         * one for the read-only view. */
        if ((t->kind == TY_GENERIC || t->kind == TY_ARRAY) && u->inst &&
            strcmp(u->inst->name, t->name) == 0) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

/* Emit the definition of one struct, array or enum.
 *
 * An enum with payloads becomes `struct { tag; union }` and takes part in the
 * dependency ordering like any other struct, because a payload may contain
 * another struct: a variant holding a `slice<u8>` that was emitted before
 * `slice_u8` made gcc report an unknown type name, a trap that was hit for real.
 *
 * Params:
 *   u - the unit to emit
 */
static void unitBody(CG *g, SUnit *u) {
    if (u->td) {
        /* A generic enum instance such as `option<i64>` enters the
         * substitution context, so that `cType` substitutes the payload
         * types. */
        if (u->inst) substEnter(g, u->inst);
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
        if (u->inst) substLeave(g);
        return;
    }

    bool generic = u->inst && u->inst->kind == TY_GENERIC;
    if (generic) substEnter(g, u->inst);

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
    cgLine(g, "");
    if (generic) substLeave(g);
}

/* The derived `_eq` for arrays is gone: array equality now goes through
 * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
 * implementation. The old shape emitted a loop per array type, which cost 2450
 * lines, 32% of the generated C, for 200 types in a stress program.
 */

/* Register the slice helper of one base type and return its name.
 *
 * Registration deduplicates: the same base and kind registered twice yields the
 * same name and one emitted function.
 *
 * Params:
 *   ob   - type being sliced: an array or a view
 *   st   - resulting slice type
 *   tail - true for "slice to the end" (`s[lo..]`), where the omitted bound is
 *          the length of the base
 *
 * Returns:
 *   The helper name, `extc_slice` or `extc_sliceTo` plus the C name of the base.
 *   One base type is only ever sliced into one kind of slice, so the names
 *   cannot collide. The concatenated name is deliberately not spelled out here:
 *   a repository-wide check rejects the C name pattern of a slice type in the
 *   source, comments included.
 */
static const char *sliceHelper(CG *g, Type *ob, Type *st, bool tail) {
    const char *name = arenaPrintf(g->arena, "extc_slice%s_%s",
                                   tail ? "To" : "", ob->name);
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
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, %lld, f, ln);\n",
                  (long long)ob->asize);
        bufPrintf(&b, "    return (%s){ .data = &a->data[s], .len = hi - lo };\n}\n", ret);
    } else if (tail) {
        /* Slicing to the end: hi is v.len. It is taken as a parameter instead
         * of being expanded in place, so the view is evaluated once. */
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, const char *f, int ln) {\n",
                  ret, name, ob->name);
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, v.len, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = v.len - lo };\n}\n", ret);
    } else {
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = hi - lo };\n}\n", ret);
    }

    SliceHelper *h = (SliceHelper *)vecPush(&g->helpers);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/* Emit the view produced by `a[lo..hi]`.
 *
 * What the compiler can prove leaves no trace at runtime, and that splits this
 * in two:
 *   1. The base is a fixed-size array and both bounds are literals, with the
 *      checker filling in an omitted bound. The range is then known when the
 *      program is checked and an out-of-range one was reported there, so the
 *      generated C is `&a.data[2]` with `.len = 3` and contains no check at all.
 *   2. Anything else gets a helper that checks at runtime and traps with the
 *      extC position.
 *
 * Returns:
 *   The expression for the view, or "0" after reporting an error when the types
 *   make no sense.
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
        const char *fn = sliceHelper(g, ob, st, true);
        return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), \"%s\", %d)",
                           fn, arg, loS, g->path, e->line);
    }
    const char *fn = sliceHelper(g, ob, st, false);
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
/* Add the unit of a generic instance that a type mentions, if it is missing.
 *
 * Params:
 *   units - the unit list, appended to in place
 *   t     - a type; anything that is not an instance is ignored
 *
 * Notes:
 *   - Deduplication is by C name and cannot use unitFind; see the comment at the
 *     lookup below.
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
     * unknown type name. The original collection path walked the instance list
     * of the type table and so never exposed this. */
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

/* Scan one type, adding every instance it mentions.
 *
 * References, slice and array elements, and generic arguments are all followed.
 *
 * Params:
 *   depth - recursion guard: nesting deeper than 12 stops the walk, which is
 *           what bounds a self-referential type
 */
static void scanTypeForUnits(Arena *arena, Vec *units, Type *t, int depth) {
    if (!t || depth > 12) return;                                    /* bounds self-reference */
    addInstanceUnit(arena, units, t);
    if (t->inner) scanTypeForUnits(arena, units, t->inner, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        scanTypeForUnits(arena, units, *(Type **)vecAt(&t->targs, i), depth + 1);
}

/* Scan everything one unit mentions and add the instances found.
 *
 * Three places are walked: the fields, the signatures of the methods - both the
 * parameters and the return type, which is where the closure used to miss an
 * instance - and the payloads of an enum's variants. A generic instance
 * substitutes its own type arguments first.
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
    /* 2. method signatures, parameters and return type: the place that was missed */
    if (sd) for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&sd->methods, i);
        /* The coroutine protocols (`next`/`value`, on the frame and on the handle) are emitted
         * inline at their call sites, never as real functions. */
        if (f->coroProto) continue;
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
/* Does this instance's signature still mention a type parameter?
 *
 * Such a FuncDef is a checker artifact -- a generic call inside a generic body creates an
 * instance with `T` itself as the argument, so the body can be checked -- and never a C
 * function. The emission loop below skips these; see the comment there.
 *
 * Params:
 *   f - the function instance to inspect
 *
 * Returns:
 *   true when any parameter type or the return type still mentions a type parameter.
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

/* Does this byte continue a C name? Used to tell a mention of `e` from the `e`
 * inside `self` or `true`: a one-letter local is counted by name, so a plain
 * substring search would find it everywhere and never drop anything. */
static bool identByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* How many times does the name `needle` occur in the finished output, as a whole
 * name? A mention inside a string literal still counts, which only ever keeps a
 * definition alive. */
static size_t countMentions(const char *hay, const char *needle) {
    size_t n = 0, len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) {
        if (p > hay && identByte(p[-1])) continue;
        if (identByte(p[len])) continue;
        n++;
    }
    return n;
}

/* Pair a definition with the declaration already recorded for the same function. The
 * key is the C name: a `FuncDef*` is not unique per emission (an instance method is
 * emitted once per instance), while a name is - two emitted definitions sharing one
 * name would not compile. A definition with no declaration to pair with is dropped
 * from consideration: keeping a function nobody calls costs a warning, never
 * correctness. */
static void deadFuncBody(CG *g, FuncDef *f, size_t off, size_t len) {
    const char *name = cFuncName(g, f);
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (df->body || df->len || strcmp(df->name, name) != 0) continue;
        /* Only offsets are recorded here. The text is taken from `g.body` once that buffer is
         * complete: a view-index helper emitted in the middle of a body points the generator at
         * another buffer for a moment, so a slice cut now is not contiguous in the finished unit
         * and `strstr` never finds it again. That is why the home-arena parameter of
         * `examples/out-param.extc` kept its warning. */
        df->off = off;
        df->len = len;
        return;
    }
}

static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle);
static char  *funcDefStart(char *text, DeadFunc *df, size_t *lenOut);

/* Remember a local declaration: `before` is where its line started in the current buffer. */
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

/* Is this line a `#define`, and where does the macro name start?
 *
 * `#define X` and `#  define X` are the same directive - the prelude pads them for alignment - and
 * recognizing only the unspaced form left `EXTC_NORETURN` behind in every program whose traps had
 * all been dropped (`macro is not used`). */
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

/* Drop the runtime definitions nothing uses.
 *
 * The block is captured as one piece of text and scanned in the style it was written:
 * a definition starts at column zero and ends with a `}` at column zero, a variable is
 * one line ending in `;`. The test for every candidate is the same sound one: *every*
 * mention of the name in the finished unit lies inside the candidate itself. A first
 * attempt used "no mention outside this whole block", which is unsound - a call from
 * another primitive inside the block counts as inside, yet that caller is still there
 * (dropping `extc_trap` while `extc_checkedIndex` still called it turned a clean
 * translation unit into one that did not compile, and a compile error stops clang from
 * reporting warnings at all, so the damage first looked like a win).
 *
 * Dropping a definition removes the calls it makes, so the counts fall as the scan goes
 * on and chains (`extc_arena_destroy` calls `extc_arena_release`) unravel by themselves.
 *
 * Only the `extc_`/`__extc_` namespace is touched: user code and the library are not this
 * pass's business. */
static void dropRuntimeDefs(CG *g, Buf *out, char **textp, size_t *lenp) {
    char  *text = *textp;
    size_t len  = *lenp;
    if (!g->primText) return;
    const char *nl = strchr(g->primText, '\n');
    size_t al = nl ? (size_t)(nl - g->primText) : strlen(g->primText);
    char anchor[256];
    if (al == 0 || al >= sizeof anchor) return;
    memcpy(anchor, g->primText, al);
    anchor[al] = 0;
    size_t rl0 = strlen(g->primText);
    for (;;) {
        char  *rp = strstr(text, anchor);
        if (!rp) break;
        size_t rl = rl0;
        bool   cut = false;
        for (char *ln = rp; ln < rp + rl && !cut; ) {
            char  *eol = memchr(ln, '\n', (size_t)(rp + rl - ln));
            size_t ll  = eol ? (size_t)(eol - ln) : (size_t)(rp + rl - ln);
            /* A definition opens a body, and a short one fits on a single line, which
             * ends with `}` instead - `extc_arena_init` is written that way, and it was
             * skipped until the debug switch showed it never reached the decision. */
            bool   opensBody = (ll > 8 && ln[ll - 1] == '{' && memchr(ln, '(', ll));
            bool   wholeBody = (ll > 8 && ln[ll - 1] == '}' && memchr(ln, '(', ll));
            /* A long parameter list wraps, so the opening line can end with a comma:
             * `extc_checkedRange` is written that way, and it never reached the decision
             * either - the debug switch showed an empty log for it. */
            bool   wrapsHead = (ll > 8 && ln[ll - 1] == ',' && memchr(ln, '(', ll));
            bool   isDef = (opensBody || wholeBody || wrapsHead) &&
                           ln[0] != ' ' && ln[0] != '/' && ln[0] != '#' && ln[0] != '*';
            bool   isVar = (ll > 8 && ln[0] != ' ' && ln[0] != '/' && ln[0] != '#' &&
                            ln[0] != '*' && ln[ll - 1] == ';' && !memchr(ln, '(', ll));
            /* A prelude macro is emitted with the machinery that needs it, and that machinery
             * is dropped when nothing uses it - leaving `#define EXTC_REC_LIMIT 100000` behind
             * for clang to report as an unused macro. A macro is the same kind of candidate:
             * its name appears in its own line, and anywhere else means something uses it. */
            bool   isMacro = (ll > 10 && macroDefineName(ln, ll) != NULL);
            if (isDef || isVar || isMacro) {
                /* The name: the identifier before the first `(`, or the last one before
                 * the `;` / `=` of a variable. */
                char *ns, *ne;
                if (isMacro) {
                    ns = (char *)macroDefineName(ln, ll);
                    ne = ns;
                    while (ne < ln + ll && identByte(*ne)) ne++;
                } else if (isDef) {
                    ne = memchr(ln, '(', ll);
                    ns = ne;
                    while (ns > ln && identByte(ns[-1])) ns--;
                } else {
                    /* The name of a variable: before the `=` of an initializer, else
                     * before the `;`. Reading back from the `;` would pick up the
                     * initializer (`static int64_t __extc_rec_depth = 0;` has `0` there). */
                    char *eq = memchr(ln, '=', ll);
                    ne = eq ? eq : ln + ll - 1;
                    while (ne > ln && (ne[-1] == ' ' || ne[-1] == '=')) ne--;
                    ns = ne;
                    while (ns > ln && identByte(ns[-1])) ns--;
                }
                size_t nlen = (size_t)(ne - ns);
                long   totalOverride = -1;      /* macros: guard lines do not count as uses */
                long   insideOverride = -1;     /* macros: every `#define` of the name is a piece */
                char   name[128];
                if (nlen > 5 && nlen < sizeof name &&
                    (strncmp(ns, "extc_", 5) == 0 || strncmp(ns, "__extc_", 7) == 0 ||
                     strncmp(ns, "EXTC_", 5) == 0)) {
                    memcpy(name, ns, nlen);
                    name[nlen] = 0;
                    size_t span = (isDef && !wholeBody) ? 0 : ll + 1;   /* a variable, a macro, or a one-liner */
                    if (wholeBody) span = ll + 1;           /* one line: the whole definition */
                    if (isMacro) {
                        /* Only the `#define` line is the candidate. Taking the whole guard block
                         * would be wrong twice over: a shared `#if defined(__GNUC__)` block holds
                         * several macros at once (`EXTC_INLINE` and `EXTC_UNUSED` live in the same
                         * one), so removing it for an unused macro deleted a macro that was still
                         * in use - five C errors, `expected ';' before 'static'`. Leaving
                         * `#ifndef X`/`#endif` behind is harmless: an empty conditional is valid.
                         *
                         * The guard line mentions the name only to ask whether it is defined, which
                         * is not a use, so those lines are subtracted from the count. */
                        /* A macro can be defined more than once in the same conditional block -
                         * `EXTC_INLINE` is defined once for GNU compilers and once for the rest,
                         * in the `#if`/`#else` of one block - and every one of those lines is a
                         * piece of the same macro, so they all count as "its own mentions" and all
                         * of them go together. */
                        size_t guards = 0, defs = 0;
                        for (char *p = rp; p < rp + rl; ) {
                            char *el = memchr(p, '\n', (size_t)(rp + rl - p));
                            size_t l2 = el ? (size_t)(el - p) : (size_t)(rp + rl - p);
                            if (l2 > 8 && (strncmp(p, "#ifndef ", 8) == 0 || strncmp(p, "#ifdef ", 7) == 0))
                                guards += countMentionsIn(g, p, l2, name);
                            const char *mn = (l2 > 8) ? macroDefineName(p, l2) : NULL;
                            if (mn && strncmp(mn, name, strlen(name)) == 0 &&
                                !identByte(mn[strlen(name)]))
                                defs += countMentionsIn(g, p, l2, name);
                            if (!el) break;
                            p = el + 1;
                        }
                        totalOverride  = (long)countMentions(text, name) - (long)guards;
                        insideOverride = (long)defs;
                        if (!defs) continue;                 /* not this macro's own line */
                    }
                    if (isDef && !wholeBody) {              /* to the `}` at column zero */
                        char *p = ln;
                        while (p < rp + rl) {
                            if (p[0] == '}' && p[-1] == '\n') { span = (size_t)(p - ln) + 1; break; }
                            p++;
                        }
                    }
                    if (span) {
                        /* every mention inside the candidate itself? */
                        size_t total  = countMentions(text, name);
                        if (totalOverride >= 0) total = (size_t)totalOverride;
                        size_t inside = (insideOverride >= 0) ? (size_t)insideOverride
                                                             : countMentionsIn(g, ln, span, name);
                        if (dbgOn("EXTC_DBG_PRIM"))
                            fprintf(stderr, "[prim] %-22s total=%zu inside=%zu span=%zu %s\n",
                                    name, total, inside, span, total == inside ? "DROP" : "keep");
                        if (total == inside) {
                            memmove(ln, ln + span, len - (size_t)(ln - text) - span + 1);
                            len -= span;
                            rl  -= span;
                            out->len = len;
                            text = bufCstr(out);
                            cut = true;
                            break;
                        }
                    }
                }
            }
            if (!eol) break;
            ln = eol + 1;
        }
        if (!cut) break;      /* nothing left to drop: the block is settled */
    }
    *textp = text;
    *lenp = len;
}

/* Drop local declarations that nothing reads. The stage is the whole function body: a name
 * that occurs there exactly once is the declaration itself. */
/* A statement to remove, or to shorten to its right-hand side. */
typedef struct {
    size_t      start, end;   /* the span in the output text */
    const char *repl;         /* what takes its place, or NULL to drop it entirely */
} LocalCut;

/* Does this text call something? A statement whose right-hand side calls a function must not be
 * dropped: the call is the point of it (`uint64_t skip = pcg32_next(&r)` in `examples/rng.extc`
 * advances the generator, and the value it returns is what nobody reads). */
static bool textHasCall(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == '(') return true;
    return false;
}

/* How many times is `name` *read* here? Literals and comments are skipped, and a mention that is
 * the target of an assignment (`name =`, a single `=`) is a write, not a read. */
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

static void dropUnusedLocals(CG *g, Buf *out, char **textp, size_t *lenp) {
    char  *text = *textp;
    size_t len  = *lenp;
    for (;;) {
        bool cut = false;
        for (size_t i = 0; i < g->deadLocals.len && !cut; i++) {
            DeadLocal *d = *(DeadLocal **)vecAt(&g->deadLocals, i);
            if (!d->text || !d->funcName) continue;
            const char *body = NULL;
            for (size_t k = 0; k < g->deadFuncs.len; k++) {
                DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, k);
                if (df->body && strcmp(df->name, d->funcName) == 0) { body = df->body; break; }
            }
            if (!body && g->mainBody && strcmp(d->funcName, g->mainFuncName) == 0)
                body = g->mainBody;                    /* main has no DeadFunc entry */
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s func=%-12s mainFunc=%-12s body=%s\n", d->name,
                        d->funcName ? d->funcName : "(null)",
                        g->mainFuncName ? g->mainFuncName : "(null)", body ? "yes" : "no");
            if (!body) continue;                       /* its function is gone already */
            char  *bp = strstr(text, body);
            if (!bp) continue;
            size_t bl = strlen(body);
            if (dbgOn("EXTC_DBG_LOCAL"))
                fprintf(stderr, "[local] %-14s bp=%s cnt=%zu own=%zu\n", d->name,
                        bp ? "hit" : "miss", bp ? countMentionsIn(g, bp, bl, d->name) : 0, d->own);
            /* The declaration's own mention is not a read of the variable, so the body is
             * examined without that line. Any other read keeps everything. */
            Buf bm;
            bufInit(&bm, g->arena);
            size_t declAt = 0;
            bool   declInside = false;
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
            if (countReads(bufCstr(&bm), bm.len, d->name) != 0) continue;   /* it is read */
            (void)declInside;
            (void)declAt;
            /* Collect what goes: the declaration, and every assignment to it. A right-hand side
             * that calls something leaves the call behind; anything else disappears. */
            Vec cuts;
            vecInit(&cuts, g->arena, sizeof(LocalCut));
            char *dl = (d->own == 0) ? strstr(text, d->text) : strstr(bp, d->text);
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
                        Buf r;
                        bufInit(&r, g->arena);
                        bufPutn(&r, text + ls, ws);
                        bufPutn(&r, text + rhs, le - rhs);
                        c.repl = bufCstr(&r);
                    }
                }
                *(LocalCut *)vecPush(&cuts) = c;
            }
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
            d->text = NULL;
            cut = true;
        }
        if (!cut) break;
    }
    *textp = text;
    *lenp = len;
}

/* Count mentions of `name` in generated *code*: string and character literals and comments are
 * skipped. A one-letter local otherwise matches what is inside a string literal - `"v = "`
 * counted as a use of `v`, which kept declarations that nothing reads, and the same held for
 * `e`, `n` and `d` all over the corpus. */
static size_t countCodeMentions(const char *hay, size_t n, const char *name) {
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

/* The same count, over one piece of the body buffer, which is not terminated. */
static size_t countMentionsIn(CG *g, const char *hay, size_t n, const char *needle) {
    (void)g;
    return countCodeMentions(hay, n, needle);
}

/* How many times does `name` appear in this text *outside* the given spans?
 *
 * This is the question the descriptor phase asks, and it used to be answered by copying the whole
 * unit, cutting the pieces out of the copy and counting what was left: one full copy per candidate
 * (a hundred candidates against a hundred kilobytes) showed up as a quarter of the compile time in
 * a callgrind profile. Walking the text once and skipping the spans says the same thing.
 *
 * Literals and comments are skipped, as everywhere else: a name inside a string is not a use. */
static size_t countMentionsOutside(const char *hay, const char *name,
                                   const size_t *starts, const size_t *ends, size_t nspans) {
    size_t len = strlen(name), count = 0;
    /* `strstr` jumps from one occurrence to the next (it is vectorized); walking byte by byte and
     * calling `strncmp` at every position was 4x the whole program's instructions, which a profile
     * showed immediately. A mention inside a string literal or comment counts here - that only ever
     * keeps a definition, which is the safe direction. */
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
        len -= tl;
        out->len = len;
    }
    /* Local declarations that nothing reads: after the offset-based phase, before the
     * phases that remove whole functions (a local of a function that is gone needs no
     * decision). */
    dropUnusedLocals(g, out, &text, &len);
    /* Functions nobody calls. Both halves are located by their own text, so what is
     * removed is exactly what was captured - never a piece of a function. The
     * definition sits after the declaration, so it goes first and the declaration's
     * position stays valid. */
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;                                   /* no definition emitted */
        if (countMentions(text, df->name) != 2) continue;           /* someone calls it */
        char  *pt = strstr(text, df->proto);
        /* The definition is located by its signature line: the passes above rewrite the inside of
         * bodies, so the copy taken at generation time often no longer matches - and every function
         * that was touched that way survived this phase, which is where the remaining dead code
         * (`fs$outPut`, `pcg32_withStream`, `io$reader_fill`) came from. */
        size_t blen = 0;
        char  *bd = funcDefStart(text, df, &blen);
        if (!pt || !bd || bd < pt || !blen) continue;               /* not both, in order */
        size_t bl = blen, pl = strlen(df->proto);
        memmove(bd, bd + bl, len - (size_t)(bd - text) - bl + 1);    /* definition first */
        len -= bl;
        memmove(pt, pt + pl, len - (size_t)(pt - text) - pl + 1);    /* then declaration */
        len -= pl;
        if (g->bodyOff > (size_t)(pt - text)) g->bodyOff -= pl;
        out->len = len;
        df->body = NULL;
    }
    /* The runtime primitives come after the functions and before the globals: a
     * primitive can be called only from code that the function phase has just removed
     * (`extc_modU` is called by dead library functions), and its own calls disappear as
     * it goes, so this is the point where the counts mean what they should. */
    dropRuntimeDefs(g, out, &text, &len);
    /* Then the top-level definitions, to a fixed point: a definition can be the only
     * thing that names another one (`io$STDIN` is the sole mention of `io$STDIN_FD`,
     * which no program refers to either), so one pass leaves a chain behind. Each
     * round is safe for the same reason a single pass is: a name is dropped only when
     * the finished text mentions it exactly once. */
    /* Top-level definitions, one name at a time.
     *
     * The test is done on a scratch copy of the unit: every piece registered under the name is
     * cut out of the copy, and if the name is not mentioned anywhere else, the pieces are the
     * only places it appears - so nothing uses it and they can go. Counting pieces against
     * mentions (an earlier attempt) cannot tell a registration from a use: a descriptor with a
     * declaration and a definition was dropped while its call site stayed, and the C compiler
     * reported `undeclared`.
     *
     * A piece that is not found, or a name mentioned elsewhere, keeps everything: the answer is
     * conservative in the one direction that matters. */
    bool dropped = true;
    while (dropped) {
        dropped = false;
        for (size_t i = 0; i < g->deadDefs.len; i++) {
            DeadDef *d = *(DeadDef **)vecAt(&g->deadDefs, i);
            if (d->scoped || !d->text) continue;            /* inside a body, or already gone */
            size_t starts[8], ends[8], pieces = 0;
            bool   allThere = true;
            for (size_t k = 0; k < g->deadDefs.len; k++) {
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

/* Make sure the attribute macro the insertions below rely on is defined.
 *
 * The macro pruning pass runs before them and sees no use of `EXTC_UNUSED` at all, so it drops the
 * definition - and then the attribute inserted here names a macro that does not exist
 * (`expected ';' before 'static'`, five errors in one program). The definition is put back inside
 * the same `#if defined(__GNUC__)` block it came from; if that block is gone too, the insertion is
 * abandoned by the caller rather than emitting an attribute no compiler understands. */
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

/* Where does this function's definition start, and where does it end?
 *
 * Found by its *signature line*, not by the whole captured text: the passes that remove
 * unreferenced definitions and rewrite dead assignments change the inside of bodies, so comparing
 * against a copy taken before them fails exactly for the functions that were touched - which is
 * why a first attempt at this silently skipped `pcg32_withStream`. The body ends at the `}` that
 * sits in column zero, which is how every generated definition is closed. */
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
    for (char *p = at; *p; p++)
        if (p[0] == '}' && p[-1] == '\n') { e = p + 1; break; }
    *lenOut = (size_t)(e - at);
    return at;
}

/* Mark the functions nothing calls.
 *
 * Every emitted function carries `EXTC_UNUSED` on its declaration today, and clang says what that
 * means: the attribute is a statement about a function nobody calls, and putting it on one that
 * *is* called is wrong (`-Wused-but-marked-unused` reports exactly those, one per call site).
 *
 * The honest answer is only known once everything that could be dropped has been: a function
 * whose name appears nowhere but in its own declaration and definition is a survivor of the
 * pruning that nothing calls - either because the only callers were dropped, or because it was
 * kept for a reference the analysis could not resolve. Those, and only those, get the attribute
 * inserted into their declaration.
 */
static void markUncalledFunctions(CG *g, Buf *out) {
    char  *text = out->data;
    size_t len  = out->len;
    Vec    at;
    vecInit(&at, g->arena, sizeof(size_t));
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body) continue;
        size_t blen = 0;
        char *bd = funcDefStart(text, df, &blen);
        if (!bd) continue;
        char *pt = strstr(text, df->proto);
        size_t own = 1;                                  /* the definition always mentions it */
        if (pt && pt < bd) own++;
        if (bd) (void)blen;
        if (countMentions(text, df->name) != own) continue;   /* something calls it: leave it alone */
        /* The attribute goes at the start of the declaration. It used to be looked up by the
         * literal `static `, which missed every `@inline` function: those are declared through the
         * `EXTC_INLINE` macro, so no `static` is spelled out (`io$pairsReady` in
         * `tests/io/stream-file.extc` kept its warning that way). */
        char *decl = pt ? pt : bd;
        while (*decl == ' ') decl++;
        if (decl >= text + len) continue;
        *(size_t *)vecPush(&at) = (size_t)(decl - text);
    }
    if (!at.len) return;
    /* Whether the macro can be put back is checked here, but acted on at the end: inserting it
     * now would shift every offset collected below (that produced `EXEXTC_UNUSED TC_INLINE` - two
     * insertions fighting over one offset). */
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

/* Mark the parameters a body never reads.
 *
 * A parameter cannot be dropped - it is part of the signature - so the attribute is the only way
 * to say "this one is deliberately unused". `-Wunused-parameter` is in `-Wextra`, and taking an
 * argument a function does not need is legitimate: `examples/borrowing.extc` and
 * `examples/out-param.extc` both do it.
 *
 * The answer is only known once the body exists, so this runs on the finished text: the parameter
 * list is delimited by the first `(` after the definition and its matching `)`, split on top-level
 * commas, and each parameter's name is counted in the body that follows. Every insertion point is
 * collected first and applied from the end backwards, so no offset is invalidated on the way; the
 * result is assembled in a scratch buffer and handed back to the output.
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
    /* Whether the macro can be put back is checked here, but acted on at the end: inserting it
     * now would shift every offset collected below (that produced `EXEXTC_UNUSED TC_INLINE` - two
     * insertions fighting over one offset). */
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
static bool inTraitTable(Module *m, FuncDef *f) {
    if (!f->owner) return false;
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!im->trait || !im->target) continue;
        Type *bt = ttBase(im->target);
        if (!bt || bt->sdef != f->owner) continue;
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            if (mth == f) return true;
            if (mth->name && f->name && strcmp(mth->name, f->name) == 0) return true;
        }
    }
    return false;
}

bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out) {
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.tt = tt;

    vecInit(&g.structs, arena, sizeof(void *));
    bufInit(&g.prefix, arena);          /* statement prefix; uninitialized, it segfaults */
    /* Deduplicate by C name: a writable and a read-only view are the same C
     * struct - `slice<mut slice<T>>` and `slice<slice<T>>` are both
     * `slice_slice_T` - while `ttEquals` counts `mut` as part of the identity,
     * so the type table can hold two instances under one name. Deduplicating by
     * name keeps every later piece from being generated twice. */
    /* Coroutine handle prepass: one `kind` per coroutine function (the boxing site and the handle
     * dispatch must agree on it), and whether this program needs the handle type at all -- the
     * checker sets `coroBoxed` when it coerces a frame into a handle. */
    {
        int ck = 0;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
            if (!cf || !cf->isCoro || cf->tmpl) continue;
            cf->coroKind = ck++;
            if (cf->coroBoxed) g.needCoroHandle = true;
        }
        /* The event layer is needed if anything used calls one of its entry points. */
        for (size_t fi = 0; fi < m->funcs.len; fi++) {
            FuncDef *ef = *(FuncDef **)vecAt(&m->funcs, fi);
            if (ef && ef->used && ef->name &&
                (strncmp(ef->name, "extc_epoll_", 11) == 0 || strncmp(ef->name, "extc_sock_", 10) == 0))
                g.needEvent = true;
        }
    }
    vecInit(&g.insts, arena, sizeof(void *));
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->instances, i);
        bool dup = false;
        for (size_t k = 0; k < g.insts.len && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&g.insts, k))->name, it->name) == 0;
        if (!dup) *(Type **)vecPush(&g.insts) = it;
    }
    /* Does the program use the raw-terminal primitives? They arrive as an `extern!` the
     * library declares (`extc_raw_enter` in `std::sys::term`), which is the one thing
     * codegen can see from here. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExtern || !f->name) continue;
        if (strcmp(f->name, "extc_raw_enter") == 0) g.needRawTerm = true;
        /* The pool registry, on the same footing: the library declares these through
         * `extern!`, so their presence is what says the program uses pools at all. */
        if (f->name && strncmp(f->name, "extc_pool_", strlen("extc_pool_")) == 0) g.needPool = true;
        if (strcmp(f->name, "extc_cout_put") == 0)  g.needCout    = true;
        /* The byte-search runtime, on the same footing: `std::sys::mem` is the only module
         * that declares these, and its declarations are what says the program wants `memmem`
         * instead of a byte loop. The flag is read again before the includes (the `_GNU_SOURCE`
         * block below), so this scan has to run before the preamble -- it does. */
        if (f->externLib && strcmp(f->externLib, "extc-mem") == 0)  g.needMemFind = true;
    }
    /* 池还多一条来源：**检查器早就算好的 `makesPool`**。只看"这个模块里声明了 `extc_pool_*`
     * 的 extern"是不够的 —— 那些声明在**库模块**里，而 `m` 是入口模块，于是 main 的 prologue
     * 那一刻 `needPool` 还是假 ⇒ 帧的 zone（`__extc_zm1`）不发 ⇒ 提权到第 1 层也没法表达
     * （`zoneArgRef` 会静默退回动态兜底）。`makesPool` 是可传递最小不动点，任何函数（含方法）
     * 为真都说明这个程序用池 —— 这正是这一格要问的问题。 */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f && f->makesPool) g.needPool = true;
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (!f || f->coroProto) continue;
            if (f->makesPool) g.needPool = true;
        }
    }
    vecInit(&g.funcs, arena, sizeof(void *));
    vecInit(&g.helpers, arena, sizeof(SliceHelper));
    vecInit(&g.viewIdx, arena, sizeof(Type *));
    vecInit(&g.viewCpy, arena, sizeof(Type *));
    vecInit(&g.descs, arena, sizeof(void *));
    vecInit(&g.eqNeed, arena, sizeof(void *));
    vecInit(&g.deadDefs, arena, sizeof(DeadDef *));
    vecInit(&g.deadFuncs, arena, sizeof(DeadFunc *));
    vecInit(&g.deadLocals, arena, sizeof(DeadLocal *));
    bufInit(&g.desc, arena);
    bufInit(&g.rt, arena);              /* print/compare runtime: emitted only if needed */
    bufInit(&g.rtPrint, arena);
    bufInit(&g.rtEq, arena);
    bufInit(&g.rtDie, arena);
    bufPuts(&g.rtDie,
        "/* Every path that ends the process on purpose goes through here. A trap path is\n"
        " * the only code that still runs when a program dies, so this is where anything the\n"
        " * process has to give back is given back. Today that is the terminal: a program\n"
        " * that traps in raw mode would otherwise leave the user's shell unable to echo\n"
        " * what they type. The hook is installed by `extc_raw_enter` (emitted only when the\n"
        " * program uses the raw-terminal primitives) and cleared before it runs, so it\n"
        " * cannot run twice. */\n"
        "static int32_t (*__extc_dying)(void);\n"
        /* The three functions that end a program never come back, and saying so is not
         * decoration: it is what lets the C compiler see that the path below a check
         * cannot be reached, which removes both `-Wmissing-noreturn` and the
         * `-Wunreachable-code` warnings that follow every trap. */
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#  define EXTC_NORETURN __attribute__((noreturn))\n"
        "#else\n"
        "#  define EXTC_NORETURN _Noreturn\n"
        "#endif\n"
        "EXTC_NORETURN static inline void extc_die(int code) {\n"
        "    if (__extc_dying) { int32_t (*f)(void) = __extc_dying; __extc_dying = 0; (void)f(); }\n"
        "    exit(code);\n"
        "}\n"
);
    bufInit(&g.rtCout, arena);
    bufPuts(&g.rtCout,
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
        "    if (extc_cout_len + n > (int64_t)sizeof extc_cout_buf) extc_cout_flush();\n"
        "    memcpy(extc_cout_buf + extc_cout_len, p, (size_t)n);\n"
        "    extc_cout_len += n;\n"
        "}\n");
    /* The one formatter the library cannot write itself: `%g`. extC has no variadics,
     * so `snprintf` is unreachable from extC, and this is the fixed-arity door to it.
     * The format is **the same one the retired builtin uses** (`extc_print`'s `f64`
     * case above), so migrating a program off `println` cannot change a byte of its
     * output; `%g` also fits `char[32]` for every double, and `snprintf` truncates
     * rather than overruns if it ever did not.
     *
     * It lives in a buffer of its own because it is appended only when a call site
     * reaches it (see `needCoutF64`): the library's `<<(f64)` is its one user, and a
     * program that never prints a float should not carry the formatter. */
    bufInit(&g.rtCoutF64, arena);
    bufPuts(&g.rtCoutF64,
        "void extc_cout_f64(double v) {\n"
        "    char b[32];\n"
        "    int n = snprintf(b, sizeof b, \"%g\", v);\n"
        "    if (n > 0) extc_cout_put((uint8_t *)b, (int64_t)n);\n"
        "}\n");
    bufInit(&g.rtRaw, arena);
    bufPuts(&g.rtRaw,
        "/* ---- raw terminal: give it back even when the program dies ----\n"
        " * The library turns raw mode on through `extc_raw_enter`, handing over a copy of\n"
        " * the settings it found. The trap path then restores them through the dying hook\n"
        " * above -- measured with `strace`: a program that traps in raw mode still issues\n"
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
    bufInit(&g.body, arena);
    bufInit(&g.coroDefs, arena);    /* appended last: see `CG.coroDefs` */
    g.tmpSeq = 0;
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (sd->typeParams.len > 0) continue;   /* generic: generated per instance */
        /* A synthetic holder for `impl i64 { ... }` is not a C struct: it has no fields and no
         * layout, so there is nothing to declare. Its methods are emitted like any other
         * function -- the loop below still walks them -- which is the whole point of attaching
         * them to a holder instead of teaching five passes about builtins. */
        if (!sd->builtinHolder) *(StructDef **)vecPush(&g.structs) = sd;
        /* Methods are functions too and share the prototype and definition table. */
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&sd->methods, j);
            if (md->coroProto) continue;      /* emitted inline at the call site, not here */
            /* ⚡ 可达性剪枝（PLAN #69）：**没人调用的方法不发射** ✓
             * 检查器早就在每个调用点打 `used`（`e->func = f; f->used = true;` ✓
             * 实例方法那一处也一直在用 ✓）——这里只是把**同一套标记**接到发射表上 ✓
             * 为什么重要：一个小程序会带出 prelude + 库的**整片**代码 ✗
             * （实测 `tests/io/stream-file.extc` 曾生成 **2437 行 / 169 个静态函数** ✓）*/
            if ((!md->used) && !inTraitTable(m, md)) continue;
            *(FuncDef **)vecPush(&g.funcs) = md;
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* The generic template itself is not generated: `T` is still a
         * parameter, so the result would be wrong C that does not compile. Only
         * the instances are emitted, those with `f->tmpl != NULL`. Without this
         * `continue`, both the template and its instances were emitted and the
         * unsubstituted `T` in the template produced a false error. */
        if (f->typeParams.len > 0) continue;
        /* Nor is an instance whose *signature* still mentions a type parameter.
         *
         * A generic call inside a generic body creates one of those -- `firstTwice<T>`
         * calling `firstOr<T>` builds `firstOr_T` with `T` itself as the argument -- so
         * that the template body can be checked at all. It exists for the checker: at
         * instantiation every real call site is repointed to a concrete instance
         * (`firstOr_i64`), and nothing calls the provisional one.
         *
         * Emitting it produced a function whose parameter and return types have no C
         * name: `static option_T firstOr_T(slice_T xs)` referred to types that do not
         * exist, so the generated C did not compile. It stayed hidden because a lone `T`
         * degenerates to `int` (`static int idOf_T(int x)` compiled fine as dead code),
         * so only a signature with a *constructed* type around the parameter broke. */
        if (f->tmpl && funcSignatureMentionsParam(f)) continue;
        /* 同上：自由函数/库函数/预置也按可达性剪枝 ✓
         * `main` 永远留（它是入口 ✓）；`used` 由检查器在每个调用点打 ✓
         * `extern!` 没人调就只留声明也无妨 —— 没有引用就不会进生成的 C ✓ */
        if (!f->used && !cgIsMain(f) && !inTraitTable(m, f)) continue;
        /* The coroutine protocols (`next`/`value`) are emitted inline at their call sites and have no
         * body of their own -- emitting one here hit genBlockBody with a null body. */
        if (f->coroProto) continue;
        *(FuncDef **)vecPush(&g.funcs) = f;
    }

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
     * be repeated when it names the same type, so the runtime keeps its own copy. Gated on
     * `makesPool` so a program that cannot use dyn or a pool is byte-for-byte unchanged. */
    {
        if (m->usesDyn)
            bufPuts(out,
                "#define EXTC_DYN_HANDLE_DEFINED 1\n"
                "struct ExtcDynHandleS { int64_t pid, slot, gen, pgen; };\n"
                "typedef struct ExtcDynHandleS ExtcDynHandle;\n\n");
        /* The coroutine handle, for the same reason as the dyn one: prototypes and the instance
         * structs (`vector<coroutine<T>>`) mention it before the runtime could define it. */
        if (g.needCoroHandle)
            bufPuts(out,
                "#define EXTC_CORO_HANDLE_DEFINED 1\n"
                "struct ExtcCoroS { void *frame; int64_t kind; int64_t task; };\n"
                "typedef struct ExtcCoroS extc_coro;\n\n");
    }
    /* The dying hook goes between the includes and the trap paths that call it: `int32_t`
     * has to be known, and the definition has to precede every use. It is its own block
     * because the prologue around it is already at the 4095-byte limit C99 guarantees for
     * a string literal. */
    /* The dying hook is part of the same block as far as the unreferenced-definition pass is
     * concerned: when the traps that call it are dropped, it becomes dead itself, and a first
     * version of that pass left it behind because it was captured from a later point
     * (`unused function 'extc_die'`). */
    size_t primA = out->len;        /* the runtime primitive block, captured below */
    bufPuts(out, bufCstr(&g.rtDie));
    {
        /* The dying hook's storage is a definition like any other: once the traps that call it are
         * dropped, nothing mentions it. It is registered here, where its text is at hand, because
         * the primitive pass does not recognize a function-pointer declaration as a variable (`static
         * int32_t (*__extc_dying)(void);` has parentheses in it). */
        for (char *p = out->data; p < out->data + out->len; p++) {
            if (p != out->data && p[-1] != '\n') continue;
            if (strncmp(p, "static ", 7) != 0 || !strstr(p, "__extc_dying")) continue;
            char *le = memchr(p, '\n', (size_t)((out->data + out->len) - p));
            if (!le) break;
            Buf t;
            bufInit(&t, arena);
            bufPutn(&t, p, (size_t)(le - p) + 1);
            DeadDef *d = arenaAllocZero(arena, sizeof *d);
            d->name = "__extc_dying";
            d->text = bufCstr(&t);
            *(DeadDef **)vecPush(&g.deadDefs) = d;
            break;
        }
    }
    bufPuts(out,
        "/* Every primitive below is `static inline`, and that is not a style choice:\n"
        " * without inlining, gcc at -O1 cannot see the body of a check, so it can neither\n"
        " * eliminate the check nor turn `i % 7` into a multiply and shift. Measured: the\n"
        " * remainder operator was 4.6x slower and a matrix multiply 1.5x slower; with\n"
        " * inlining both match C. */\n"
        "/* Out-of-range trap; the position comes from the call site via `#line`. */\n"
        "EXTC_NORETURN static inline void extc_trap(const char *file, int line, int64_t i, int64_t n) {\n"
        "    fprintf(stderr, \"%s:%d: trap: index %lld out of range (length %lld)\\n\",\n"
        "            file, line, (long long)i, (long long)n);\n"
        "    extc_die(1);\n"
        "}\n"
        "/* Arithmetic failures must be loud: division by zero, the overflowing division, and\n"
        " * an over-wide shift are all undefined behaviour in C. Each one traps with a\n"
        " * source position instead of computing a wrong answer or leaving UB behind. */\n"
        "EXTC_NORETURN static inline void extc_trapMsg(const char *file, int line, const char *msg) {\n"
        "    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, msg);\n"
        "    extc_die(1);\n"
        "}\n"
        /* The recursion depth guard, used only by self-recursive functions.
         * Runaway recursion used to be hard to diagnose: gcc folded it into a
         * loop that printed nothing, or the stack really overflowed and the OS
         * reported a segmentation fault, which is neither an extC message nor
         * carries a position. A depth counter maintained on entry and exit of a
         * self-recursive function now traps with a position, and it triggers far
         * earlier than a real overflow: with the default 8 MB stack and frames
         * of a few hundred bytes, a hundred thousand levels are comfortable. */
         "#ifndef EXTC_REC_LIMIT\n"
         "#define EXTC_REC_LIMIT 100000\n"
         "#endif\n"
         "static int64_t __extc_rec_depth = 0;\n"
        /* Called by the prologue of a self-recursive function; it traps with
         * the position of the call site, which points at the offending line. */
        "static inline void extc_rec_enter(const char *f, int l) {\n"
        "    if (++__extc_rec_depth > EXTC_REC_LIMIT)\n"
        "        extc_trapMsg(f, l, \"recursion too deep (unbounded recursion?)\");\n"
        "}\n"
        "static inline int64_t extc_divI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a % b;\n"
        "}\n"
        "static inline uint64_t extc_divU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline uint64_t extc_modU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a % b;\n"
        "}\n"
        "/* Shifts: shifting by the width or more, or by a negative amount, is undefined\n"
        " * behaviour in C, so the count is checked and returned (evaluated once). */\n"
        "static inline int64_t extc_shiftCount(int64_t b, int64_t w, const char *f, int l) {\n"
        "    if (b < 0 || b >= w) extc_trapMsg(f, l, \"shift count out of range\");\n"
        "    return b;\n"
        "}\n"
        "/* Checked index: the index is returned, so the caller evaluates it only once. */\n"
        "static inline int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line) {\n"
        "    if (i < 0 || i >= n) extc_trap(file, line, i, n);\n"
        "    return i;\n"
        "}\n"
        "/* Checked slice: requires 0 <= lo <= hi <= n, and returns `lo`. */\n"
        "static inline int64_t extc_checkedRange(int64_t lo, int64_t hi, int64_t n,\n"
        "                          const char *file, int line) {\n"
        "    if (lo < 0 || hi < lo || hi > n) {\n"
        "        fprintf(stderr, \"%s:%d: trap: slice %lld..%lld is out of range (length %lld)\\n\",\n"
        "                file, line, (long long)lo, (long long)hi, (long long)n);\n"
        "        extc_die(1);\n"
        "    }\n"
        "    return lo;\n"
        "}\n\n");
    /* The @overwrite cell type is emitted only when it is really used: emitted
     * unconditionally it would add a line to the generated C of every program
     * and fill the golden files with noise. The type contains an
     * `extc_arena *home`, so it must come after `extc_arena`; therefore only
     * `needOw` is computed here and the typedef itself is emitted further down,
     * inside the arena section. */
    bool needOw = false;
    {
        for (size_t i = 0; i < m->funcs.len && !needOw; i++)
            if ((*(FuncDef **)vecAt(&m->funcs, i))->owSites > 0) needOw = true;
        for (size_t i = 0; i < m->structs.len && !needOw; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                if ((*(FuncDef **)vecAt(&sd->methods, j))->owSites > 0) { needOw = true; break; }
        }
        (void)needOw;   /* the typedef needs extc_arena, so it comes later */
    }
    bufPuts(out,
        /* ------------------------------------------------------------------
         * The arena: one per frame, and allocations go through a handle.
         *
         * An arena is one lexical scope, implemented as a linked list of blocks:
         * allocating pushes space in the current block, releasing hands the whole
         * chain back to the system.
         *
         * A handle, an `extc_arena *`, rather than "the current arena" is what
         * lets a container remember which arena it was born in and ask that one
         * for room when it grows. Memory a method allocates therefore outlives
         * the method call, which is the rule that a container allocates into the
         * pool where the container itself lives.
         *
         * There is no shared state in the process, since every frame has its own
         * object, so threading will not have to change this structure.
         * ------------------------------------------------------------------ */
        "typedef struct extc_ablock { struct extc_ablock *prev; int64_t cap, used; char data[1]; } extc_ablock;\n"
        /* `spare` is one released block kept for the next round.
         *
         * The cost of an arena is now one `malloc` per **block**, so a block that
         * is allocated and released once per loop iteration makes the arena do one
         * `malloc` plus one `free` per iteration -- it degenerates into
         * per-object allocation and loses the point of having an arena at all.
         * Measured on the churn shape: 32e6 allocations across 32e6 iterations
         * produced 32e6 `malloc` and 32e6 `free` calls, while the same program
         * with a block that survives the release ran about 50x faster.
         *
         * Keeping exactly one block per arena bounds the cost: the memory is
         * already in the cache when the next round asks for it, and the arena
         * cannot hold more than one block it does not need. A block larger than
         * `EXTC_ARENA_SPARE_MAX` is not kept, so an arena that once served a huge
         * array does not hold that memory for the rest of the frame. */
        "typedef struct extc_arena { extc_ablock *top; extc_ablock *spare; } extc_arena;\n"
        "#ifndef EXTC_ARENA_SPARE_MAX\n"
        "#define EXTC_ARENA_SPARE_MAX (1 << 20)\n"
        "#endif\n"

        "static inline void extc_arena_init(extc_arena *a) { a->top = NULL; a->spare = NULL; }\n"
        /* Release everything except the spare, which becomes the next block to be
         * handed out. The spare is zeroed lazily, by `extc_arena_alloc` only for
         * the bytes it actually hands out, so the "fresh allocations are always
         * zero" promise costs the same as before. */
        "static inline void extc_arena_release(extc_arena *a) {\n"
        "    while (a->top) {\n"
        "        extc_ablock *p = a->top->prev;\n"
        "        /* Always keep one block back. Guarding this with \"the arena had more\n"
        "         * than one block\" looked like a saving and was the opposite: the\n"
        "         * block that pays off is precisely the single-block arena of a tight\n"
        "         * loop, and skipping it put the churn shape back to its old time\n"
        "         * (measured: 139.9ms -> 389.9ms). */\n"
        "        if (!a->spare && a->top->cap <= EXTC_ARENA_SPARE_MAX) {\n"
        "            a->spare = a->top; a->spare->prev = NULL;\n"
        "        } else {\n"
        "            free(a->top);\n"
        "        }\n"
        "        a->top = p;\n"
        "    }\n"
        "}\n");


    /* The frame is over: the spare has to go too, or it outlives its purpose. */
    bufPuts(out,
        "static inline void extc_arena_destroy(extc_arena *a) {\n"
        "    extc_arena_release(a);\n"
        "    if (a->spare) { free(a->spare); a->spare = NULL; }\n"
        "}\n"
        "/* Explicit conversions: narrowing, sign change, or float-to-integer. A value that\n"
        " * does not fit traps, reporting the source position. */\n"
        "static inline int64_t extc_narrowI(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {\n"
        "    if (v < lo || v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline uint64_t extc_narrowU(uint64_t v, uint64_t hi, const char *f, int l) {\n"
        "    if (v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline int64_t extc_convFloat(double v, int64_t lo, int64_t hi,"
        " const char *f, int l) {\n"
        "    if (!(v >= (double)lo && v <= (double)hi))"
        " extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n"
        "    return (int64_t)v;   /* truncation toward zero, as in C */\n"
        "}\n"
        /* Getting this function inlined is worth a lot: it has one call site per program
         * and that site is usually inside a loop, so an out-of-line call costs a call and a
         * return per object. Measured on `bench/gc/src/rebuild.extc`: 100.0ms out of line
         * against 25.8ms inlined, a factor of 3.9, and the binary came out 80 bytes smaller
         * inlined. The error path below is what makes the compiler refuse on its own.
         *
         * Asking for it needs a compiler extension: ISO C has `inline`, but nothing that
         * *requires* inlining, so `always_inline` is a GNU/Clang attribute. The generated C
         * was otherwise strict ISO C99 (`cc -std=c99 -pedantic-errors` accepted it), so the
         * attribute is guarded and the code stays ISO C everywhere else. The guard names
         * `__GNUC__` rather than using `__has_attribute`, which is itself an extension. */
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#define EXTC_INLINE static inline __attribute__((always_inline))\n"
        /* 生成物里"定义了但没人调"是正常的：库/预置按需发射，而按需比实际需要更宽 ✓
         * 函数那一族由 DeadFunc 逐对剪枝（原型+定义一起删），但**剪不到的那些**（只被死代码提到）
         * 仍然需要这个属性，否则 gcc 会为每个这样的定义报一条 unused-function ✗ */
        "#define EXTC_UNUSED __attribute__((unused))\n"
        "#else\n"
        "#define EXTC_INLINE static inline\n"
        "#endif\n"
        "EXTC_INLINE void *extc_arena_alloc(extc_arena *a, int64_t n, const char *f, int l) {\n"
        "    if (n <= 0) n = 1;\n"
        "    n = (n + 7) & ~(int64_t)7;\n"
        "    /* Adopt the spare only when the arena is empty.\n"
        "     *\n"
        "     * This is one test on a pointer that is NULL in the common case, which\n"
        "     * matters: an allocation in a loop runs this code millions of times, and\n"
        "     * an earlier version that asked 'is the spare big enough for n' on every\n"
        "     * call cost about 20% more instructions over the whole program (measured\n"
        "     * with callgrind on the rebuild shape) for no benefit at all -- the spare\n"
        "     * is a whole block, so `cap >= n` is already true whenever the arena is\n"
        "     * empty enough to want it. */\n"
        "    if (!a->top && a->spare) { a->top = a->spare; a->spare = NULL; a->top->used = 0; }\n"
        "    if (!a->top || a->top->cap - a->top->used < n) {\n"
        "        /* Block size: the request, grown by doubling, with a small floor.\n"
        "         *\n"
        "         * It used to be a flat 4096 floor, and that is 4 KB per **task**: a coroutine's frame\n"
        "         * is ~100 bytes, so 10000 live tasks cost 41.8 MB instead of ~1.5 MB (measured,\n"
        "         * bench/coro/liveN). Doubling keeps the malloc count of an allocation loop the same\n"
        "         * after a few blocks while a one-shot allocation (the common case in a coroutine)\n"
        "         * pays only for what it asks. */\n"
        "        int64_t cap = n > 64 ? n : 64;\n"
        "        if (a->top && a->top->cap < (INT64_C(1) << 20) && cap < a->top->cap * 2)\n"
        "            cap = a->top->cap * 2;\n"
        "        extc_ablock *b = (extc_ablock *)malloc(sizeof(extc_ablock) + (size_t)cap);\n"
        /* Like every other trap, this one carries a source position. It used to
         * print a bare "out of arena memory" and exit, which breaks the rule
         * that a failure the compiler can locate must say where it happened. */
        "        if (!b) { fprintf(stderr, \"%s:%d: trap: out of arena memory\"\n"
        "                        \" (this allocation wanted %lld bytes)\\n\",\n"
        "                        f, l, (long long)n); extc_die(1); }\n"
        "        b->prev = a->top; b->cap = cap; b->used = 0;\n"
        "        a->top = b;\n"
        "    }\n"
        "    {\n"
        "        void *p = a->top->data + a->top->used;\n"
        "        a->top->used += n;\n"
        "        memset(p, 0, (size_t)n);   /* fresh allocations are always zeroed */\n"
        "        return p;\n"
        "    }\n"
        "}\n\n");
    {   /* Kept as text: dropRuntimeDefs scans it for definitions nothing names. */
        Buf pb;
        bufInit(&pb, arena);
        bufPutn(&pb, out->data + primA, out->len - primA);
        g.primText = bufCstr(&pb);
    }

    /* The @overwrite cell type, emitted on demand: emitted unconditionally it
     * would change every golden file. It must come after `extc_arena`, because
     * its `home` field says which arena the storage belongs to. */
    if (needOw)
        bufPuts(out, "typedef struct extc_owcell { void *p; int64_t cap; extc_arena *home; } extc_owcell;\n");

    /* ==================================================================
     * The descriptor table and the single generic printer.
     *
     * Before, one `_debug` print function was derived per type used: a synthetic
     * stress program of N=1000 had 2028 of them, 22.9% of the generated C, and
     * never printed a single struct. Now every type costs one compile-time
     * constant in .rodata, the printing logic exists once for the whole program,
     * and whether to emit it at all is decided on demand, by the reachability
     * closure of the descriptors.
     *
     * This is not runtime reflection: the descriptors are static data, and the
     * types, field offsets and variant names in them are compile-time constants
     * that gcc can see throughout, so nothing is weakened about a failure being
     * loud when it cannot be proven impossible.
     *
     * The same table can later feed structural `==`, serialization or hashing.
     * ================================================================== */
    bufPuts(&g.rt,
        "/* ---- Type descriptor table ----\n"
        " * One static entry per type; `extc_print` exists once per program.\n"
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
        size_t rowOff = g.rt.len;
        bufPuts(&g.rt, SCALAR_DESC[i].row);
        Buf row;
        bufInit(&row, g.arena);
        bufPutn(&row, g.rt.data + rowOff, g.rt.len - rowOff);
        DeadDef *dd = arenaAllocZero(g.arena, sizeof *dd);
        dd->name = SCALAR_DESC[i].name;
        dd->text = bufCstr(&row);
        *(DeadDef **)vecPush(&g.deadDefs) = dd;
    }
    bufPuts(&g.rt, "\n");
    /* Split into two calls: C99 only guarantees support for string literals of
     * 4095 characters, and one large literal would trigger -Woverlength-strings.
     * That is not an error, but there is no reason to keep the noise. */
    bufPuts(&g.rtPrint,
        "/* Generic recursive printer. The output format must stay byte-for-byte identical to\n"
        " * the `_debug` helpers it replaces; the truth table lives in\n"
        " * tools/print-formats.txt (floats use %g, [N]u8 prints numerically, and\n"
        " * slice<u8> prints as text). */\n"
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
     * So "one `_eq` function per array type" became "one unit of data per type":
     * 200 distinct array types used to cost 2450 lines of derived code.
     */
    bufPuts(&g.rtEq,
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

    /* Enums come first: C11 cannot forward-declare an enum tag, so a struct
     * field of enum type needs the definition to be there already. */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        if (!enumHasPayload(td)) {
            /* Without payloads: still a plain C enum, unchanged. */
            bufPuts(&b, "typedef enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&b, ", ");
                bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&b, " } %s;", td->name);
            cgLine(&g, "%s", bufCstr(&b));
            cgLine(&g, "");
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
            cgLine(&g, "%s", bufCstr(&e2));
            cgLine(&g, "");
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
    /* Deduplicated by C name: a writable and a read-only view are the same C
     * struct - `slice<mut slice<T>>` and `slice<slice<T>>` are both
     * `slice_slice_T` - while `ttEquals` counts `mut` as part of the identity,
     * so the type table can hold two instances under one name. Without
     * deduplication the generated C would hold two identical `struct`
     * definitions and gcc would report a redefinition. */
    for (size_t i = 0; i < g.structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g.structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *it = *(Type **)vecAt(&g.insts, i);
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
    /* Coroutine frames: synthesized structs, but real types. Their fields say what a coroutine's
     * storage is made of, and scanning them here is what makes the instances those fields mention
     * (`vector<i32>`, behind a container) get generated at all -- without this a step called
     * `vector$vector_i32_withCap` and nothing ever emitted it (docs/topics/CONCURRENCY.md 4.4). */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !cf->isCoro || cf->tmpl || !cf->coroFrameType || !cf->coroFrameType->sdef) continue;
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = cf->coroFrameType->sdef;
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
            /* The cap does not guard against a broken loop; it guards against
             * instances that nest without end, which every monomorphizing
             * language has to guard against - C++ has `-ftemplate-depth` and
             * rustc has `recursion_limit`. Those only report that the limit was
             * passed, while this one also names which instance expanded into
             * which, which makes the problem easy to locate. */
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
        cgLine(&g, "typedef struct %s %s;", unitName(u), unitName(u));
    }
    if (units.len) cgLine(&g, "");

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
            unitBody(&g, u);
            u->done = true;
            progressed = true;
        }
        if (allDone || !progressed) break;
    }
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (!u->done) { unitBody(&g, u); u->done = true; }
    }

    /* A `<Type>_name` function used to be emitted here for an enum with
     * payloads, reading `v.tag`, which is why it had to come after the
     * definitions. The variant names now live in the `table` of the descriptor,
     * so that section is gone. The tag constants of an enum are emitted where
     * the enum itself is emitted and are unaffected. */

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
        cgLine(&g, "%s", bufCstr(&tb));
    }

    /* ------------------------------------------------------------------
     * The descriptor table is not emitted here. It goes between the prototypes
     * and the function bodies, because which descriptors are needed is known
     * only from the bodies, through what `genPrint` prints; see emitDescRegion.
     * ------------------------------------------------------------------ */

    /* Globals and constants are plain C static objects; a global of fixed size
     * needs no arena. C zeroes a static object by itself, so a global without an
     * initializer needs no initializer expression at all.
     *
     * The `static` here is a performance decision, for the reason given above
     * cgIsMain: external linkage makes gcc reject outer-loop vectorization,
     * which made a matrix multiply at OI scale 3.2x slower. */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(&g, gd->ann);
        size_t before = g.out->len;                 /* recorded for dropUnreferenced */
        if (gd->init) {
            cgLine(&g, "static %s %s = %s;", ct, gd->name, genGlobalInit(&g, gd->init));
        } else {
            /* No initializer: C zeroes static storage by itself. */
            cgLine(&g, "static %s %s;", ct, gd->name);
        }
        /* A global the program never names costs a warning in every translation
         * unit that carries it - and a `use std::io::*` carries the library's
         * streams and constants into every program. The line just written is
         * remembered; dropUnreferenced takes it back out if nothing names it. */
        Buf line;
        bufInit(&line, g.arena);
        bufPutn(&line, g.out->data + before, g.out->len - before);
        DeadDef *d = arenaAllocZero(g.arena, sizeof *d);
        d->name = gd->name;
        d->text = bufCstr(&line);
        *(DeadDef **)vecPush(&g.deadDefs) = d;
    }
    if (m->globals.len) cgLine(&g, "");


    /* ------------------------------------------------------------------
     * The prototype pool: every function is declared before any body.
     *
     * Only prototypes may appear here, never a definition. The reason is a trap
     * that was hit for real: the comparison of an array is generated by the
     * compiler, and its loop calls the `fn ==` of the element type, `point_eq`
     * among others. When the definition of the array comparison came before the
     * prototype of `point_eq`, C took `point_eq` to be implicitly declared as
     * `int()`, and the real prototype that followed produced "conflicting types
     * for 'point_eq'". Example code caught that bug.
     *
     * The lesson is the same as for the ordering of struct definitions: never
     * rely on an ordering that happens to work, rule it out by construction.
     * ---------------------------------------------------------------- */
    /* The prototypes of `_debug`, `_writeText`, `_name` and the array `_eq` are
     * gone: printing and structural `==` go through the descriptor table, so no
     * derived function needs a forward declaration. */
    /* The prototypes of instance methods; an array has no sdef and no methods. */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            /* Only a method that is really called is emitted, which keeps the
             * generated C smaller. The flag is a conservative approximation: it
             * is set whenever the template body mentions a call, so a closure
             * such as `push` calling `grow` is included automatically, and
             * emitting too much is the safe direction. */
            if (md->coroProto) continue;   /* emitted inline at the call site */
            if (!md->used) continue;
            /* Instance methods are candidates too. They used to be left out, and that is
             * exactly where the remaining `unused function` warnings lived: a library
             * method of a generic instance (`slice<i32>::isEmpty`) is marked used by the
             * checker even when the only thing that mentions it is dead code, and without
             * a candidate here nothing ever took it back out. The pairing is by name, so
             * the trap of #64 - one node, many instances - does not apply. */
            size_t pb = g.out->len;
            genFuncProto(&g, md);
            {
                Buf pt;
                bufInit(&pt, g.arena);
                bufPutn(&pt, g.out->data + pb, g.out->len - pb);
                DeadFunc *df = arenaAllocZero(g.arena, sizeof *df);
                df->name  = cFuncName(&g, md);
                df->proto = bufCstr(&pt);
                *(DeadFunc **)vecPush(&g.deadFuncs) = df;
            }
        }
        substLeave(&g);
    }
    /* Methods of ordinary structs and free functions: their order does not
     * matter, and mutual calls are covered by the prototypes above. */
    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        Vec *svP, *svA;
        substEnterFunc(&g, f, &svP, &svA);
        const char *ret = cgIsMain(f) ? "int" : cType(&g, f->ret);
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
        if (f->isCoro) continue;   /* a coroutine emits `$frame` + `$step` last */
        bufPrintf(&sig, "%s%s %s(%s);",
                  (cgIsMain(f) || f->isExtern) ? ""
                                               : (f->isInline ? "EXTC_INLINE " : "static "),
                  ret, cFuncName(&g, f), cgParamList(&g, f));
        cgLine(&g, "%s", bufCstr(&sig));
        if (!cgIsMain(f) && !f->isExtern && !inTraitTable(m, f)) {
            DeadFunc *df = arenaAllocZero(g.arena, sizeof *df);
            df->name  = cFuncName(&g, f);
            df->proto = bufCstr(&sig);
            *(DeadFunc **)vecPush(&g.deadFuncs) = df;
        }
        substLeaveFunc(&g, svP, svA);
    }
    if (g.structs.len || g.insts.len || g.funcs.len) cgLine(&g, "");

    /* ================= the body pool: definitions from here on ========== */

    /* Bodies are written into a temporary buffer first, because a slice helper is
     * discovered to be needed only while generating, and C wants definitions
     * before uses. Everything is spliced together at the end as prototypes, then
     * helpers, then bodies. */
    g.out = &g.body;

    /* Coroutine frames and step prototypes, before any body that may spawn one or drive it. */
    genCoroDecls(&g, m);

    /* The index primitives of the views are not emitted here any more: they are
     * emitted on demand, at the first subscript that needs one (`viewIndexer`).
     * That is also what fixes a missing definition when an index inside a generic
     * body names an instance this list never held -- the generated C called a
     * function nobody emitted. Printing and comparison derive no function at all,
     * see the descriptor table. */

    /* method definitions of the instances */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (md->coroProto) continue;   /* emitted inline at the call site */
            if (!md->used) continue;      /* called methods only */
            size_t fb = g.out->len;
            genFunc(&g, md);
            deadFuncBody(&g, md, fb, g.out->len - fb);
            cgLine(&g, "");
        }
        substLeave(&g);
    }

    /* Uniform method tables, one **per trait** (DYN.md stage 3).
     *
     * The receiver is erased to `void *`, and that is what lets a stored `dyn` value call through a
     * table at all: its concrete type is not known at the call site. The per-type field types the
     * first version used (`__typeof__(impl) *`) only worked because the immediate form knows the
     * type statically. Each (trait, type, method) gets a thunk adapting the implementation to the
     * uniform signature; the instance keeps the stable key `extc_vt$Trait$Type` and the trait's
     * declaration order, so the phase-1 contract is unchanged.
     *
     * Only traits the checker saw in a `dyn` form (`usedDyn`) get a table: a statically used trait
     * needs none. A method that cannot be dispatched (no receiver, generic, or returning `Self`)
     * keeps its slot -- order is ABI -- but gets a `void *` type and a NULL entry, and the checker
     * refuses to dispatch through it, so the slot is unreachable rather than miscompiled.
     *
     * Declarations are emitted with the prototypes (a body that dispatches needs the struct in
     * scope); definitions are appended after every pass that rewrites the unit by offset. */
    Buf vtDefs;
    bufInit(&vtDefs, arena);
    bufInit(&g.vtDecls, arena);
    for (size_t ti = 0; ti < m->traits.len; ti++) {
        TraitDef *tr = *(TraitDef **)vecAt(&m->traits, ti);
        if (!tr->usedDyn) continue;
        bufPrintf(&g.vtDecls, "struct extc_vt$%s_t {", tr->name);
        for (size_t k = 0; k < tr->methods.len; k++) {
            FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
            bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
            bufPrintf(&g.vtDecls, " %s (*%s)(void *",
                      unsafe ? "void *" : (want->ret ? cType(&g, ttBase(want->ret)) : "void"),
                      want->name);
            for (size_t pi = 1; pi < want->params.len; pi++)
                bufPrintf(&g.vtDecls, ", %s",
                          cType(&g, ttBase((*(Param **)vecAt(&want->params, pi))->type)));
            bufPuts(&g.vtDecls, ");");
        }
        bufPuts(&g.vtDecls, " };\n");
        for (size_t i = 0; i < m->impls.len; i++) {
            ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
            if (im->trait != tr || !im->target) continue;
            Type *bt = ttBase(im->target);
            StructDef *tsd = bt ? bt->sdef : NULL;
            if (!tsd) continue;
            bufPrintf(&g.vtDecls, "static const struct extc_vt$%s_t extc_vt$%s$%s;\n",
                      tr->name, tr->name, tsd->name);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                FuncDef *have = NULL;
                for (size_t j = 0; j < tsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&tsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                if (!have) continue;                 /* completeness is the checker's business */
                if (!funcIsMethod(want) || (want->ret && mentionsParam(want->ret))) continue;
                bufPrintf(&vtDefs, "static %s extc_th$%s$%s$%s(void *self",
                          want->ret ? cType(&g, ttBase(want->ret)) : "void",
                          tr->name, tsd->name, want->name);
                for (size_t pi = 1; pi < want->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&want->params, pi);
                    bufPrintf(&vtDefs, ", %s %s", cType(&g, ttBase(pp->type)), pp->name);
                }
                bufPrintf(&vtDefs, ") { %s%s((%s *)self", want->ret ? "return " : "",
                          cFuncName(&g, have), cType(&g, bt));
                for (size_t pi = 1; pi < want->params.len; pi++)
                    bufPrintf(&vtDefs, ", %s", (*(Param **)vecAt(&want->params, pi))->name);
                bufPuts(&vtDefs, "); }\n");
            }
            bufPrintf(&vtDefs, "static const struct extc_vt$%s_t __attribute__((unused)) "
                               "extc_vt$%s$%s = {", tr->name, tr->name, tsd->name);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
                FuncDef *have = NULL;
                for (size_t j = 0; j < tsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&tsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                bufPrintf(&vtDefs, "%s %s", k ? "," : "",
                          (unsafe || !have) ? "NULL"
                                            : arenaPrintf(arena, "extc_th$%s$%s$%s",
                                                          tr->name, tsd->name, want->name));
            }
            bufPuts(&vtDefs, " };\n");
        }
    }
    bufPuts(out, bufCstr(&g.vtDecls));

    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        if (f->isExtern) continue;              /* an external declaration has no body */
        Vec *svP, *svA;
        substEnterFunc(&g, f, &svP, &svA);      /* instances need substitution */
        size_t fb = g.out->len;
        genFunc(&g, f);
        deadFuncBody(&g, f, fb, g.out->len - fb);
        substLeaveFunc(&g, svP, svA);
        cgLine(&g, "");
    }

    /* Final assembly: the pieces whose need is discovered during generation are
     * spliced in ahead of the bodies - the print runtime and the descriptor
     * table, emitted on demand according to what genPrint printed, and the slice
     * helpers. The order is prototypes, then descriptors, then bodies, which is
     * what the define-before-use rule of C requires. */
    g.out = out;
    emitDescRegion(&g);
    /* Descriptor types and shared scalar descriptors: needed by printing or by
     * comparison, whichever comes first. */
    if (g.needRuntime || g.eqNeed.len) bufPuts(out, bufCstr(&g.rt));
    if (g.needRuntime)                 bufPuts(out, bufCstr(&g.rtPrint));
    if (g.needCout) bufPuts(out, bufCstr(&g.rtCout));
    if (g.needCoutF64) bufPuts(out, bufCstr(&g.rtCoutF64));
    if (g.needPool) poolsEmitRuntime(arena, out);
    /* The byte-search runtime, for a program that declares `extern!("extc-mem")`. Its trigger
     * also decided the `_GNU_SOURCE` preamble above; here only the bodies are appended. */
    if (g.needMemFind) memfindEmitRuntime(arena, out);
    if (g.needEvent) eventEmitRuntime(arena, out);
    /* The dyn half is separate so a pool-only program keeps byte-identical generated C. */
    if (m->usesDyn) poolsEmitDynRuntime(arena, out);
    if (g.needRawTerm) {
        /* The raw-terminal block. `tcsetattr` is declared with the same prototype the
         * library declares for it, so the two declarations agree and the call reaches
         * libc; the layout of `struct termios` is never named. */
        bufPuts(out, bufCstr(&g.rtRaw));
    }
    if (g.eqNeed.len)                  bufPuts(out, bufCstr(&g.rtEq));
    bufPuts(out, bufCstr(&g.desc));
    for (size_t i = 0; i < g.helpers.len; i++) {
        /* A slice helper is emitted on demand, and the demand is conservative: a helper
         * whose only user is itself unused is still emitted. It is a definition like any
         * other, so it is registered here and dropUnreferenced takes it back out when the
         * finished unit does not name it (which is what gcc and clang report as an unused
         * function). The attribute that used to silence that warning is gone: with this
         * in place it was not only unnecessary, clang reports a warning *about* using it. */
        SliceHelper *h = (SliceHelper *)vecAt(&g.helpers, i);
        DeadDef *d = arenaAllocZero(arena, sizeof *d);
        d->name = h->name;
        d->text = h->text;
        *(DeadDef **)vecPush(&g.deadDefs) = d;
        bufPuts(out, h->text);
    }
    /* The view index primitives, in the order they were first needed. They go here,
     * before the bodies, for the same reason the slice helpers do: a definition has to
     * precede its uses, and the need for one is discovered while the bodies are
     * generated. Emitting them at the call site instead wrote a function definition into
     * whichever buffer was current -- the prototypes -- and produced C that did not
     * compile. */
    for (size_t i = 0; i < g.viewIdx.len; i++) {
        Type *inst = *(Type **)vecAt(&g.viewIdx, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g.out;
        g.out = &tmp;
        substEnter(&g, inst);
        genViewIndexer(&g, inst);
        substLeave(&g);
        g.out = save;
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
        *(DeadDef **)vecPush(&g.deadDefs) = idd;
    }
    for (size_t i = 0; i < g.viewCpy.len; i++) {
        Type *inst = *(Type **)vecAt(&g.viewCpy, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g.out;
        g.out = &tmp;
        substEnter(&g, inst);
        genViewCopier(&g, inst);
        substLeave(&g);
        g.out = save;
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *cdd = arenaAllocZero(arena, sizeof *cdd);
        cdd->name = arenaPrintf(arena, "%s_copy", inst->name);
        cdd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g.deadDefs) = cdd;
    }
    g.bodyOff = out->len;                    /* where the bodies start in the finished unit */
    bufPuts(out, bufCstr(&g.body));
    /* The body buffer is complete and contiguous now, so the texts the unreferenced-definition
     * passes work on come from it: a slice cut during generation would not survive the view-index
     * helpers, which write into a different buffer for a moment. */
    for (size_t i = 0; i < g.deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g.deadFuncs, i);
        if (!df->body && df->len) {
            Buf b;
            bufInit(&b, arena);
            bufPutn(&b, g.body.data + df->off, df->len);
            df->body = bufCstr(&b);
        }
    }
    if (g.mainLen) {
        Buf b;
        bufInit(&b, arena);
        bufPutn(&b, g.body.data + g.mainOff, g.mainLen);
        g.mainBody = bufCstr(&b);
    }

    /* The unit is complete: now the definitions that nobody names can go - repeatedly, until the
     * text stops shrinking. One pass is not enough: dropping `pcg32_withStream` is what makes
     * `pcg32_next` dead, and the pass that would have caught it has already gone by. The
     * offset-based phase inside simply finds its spans changed and skips them, which is safe. */
    for (int round = 0; round < 8; round++) {
        size_t before = out->len;
        dropUnreferenced(&g, out);
        if (out->len == before) break;
    }
    /* A function nothing calls says so; then the parameters a body never reads. */
    markUncalledFunctions(&g, out);
    markUnusedParams(&g, out);

    /* The method-table **definitions** built before the bodies (see there): they come last so
     * that no pass which rewrites the unit by byte offset ever sees them, and the declarations
     * emitted earlier are what lets a dispatch site in a body refer to them. */
    /* Coroutine frames and step functions: appended here, after every byte-offset pass, for the
     * same reason as `vtDefs` right below -- and before it, so the frame type a step function uses
     * is defined above it in the file. */
    bufPuts(out, bufCstr(&g.coroDefs));
    bufPuts(out, bufCstr(&vtDefs));

    return !ctx->hasError;
}
