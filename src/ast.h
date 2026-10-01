/* The extC abstract syntax tree: the data shapes the parser produces and the type
 * checker annotates in place.
 *
 * The tree carries the results of name and type resolution. The checker writes
 * them back onto the nodes:
 *   Expr.type   the type of the expression
 *   Expr.func   the function a call resolved to
 *   Expr.field  the field a field access resolved to
 *   Stmt.type   the final type of a variable declaration
 * Code generation can therefore perform no type inference at all: it translates
 * decisions that have already been made.
 */
#ifndef EXTC_AST_H
#define EXTC_AST_H

#include "base.h"

/* ---------------------------------------------------------------- types */

typedef struct Type Type;
typedef struct TypeDef TypeDef;
typedef struct StructDef StructDef;
/* 效果摘要的计算状态（V 批次：把 0/1/3 的魔数编码换成命名枚举 —— 原来的 2 空着，是历史留下的）。*/
typedef enum {
    EFF_NONE = 0,        /* 还没算（`arenaAllocZero` 出来就是这个值）*/
    EFF_DONE,            /* 算完了，`effComplete` 说它可不可信 */
    EFF_IN_PROGRESS      /* 正在算 —— 再遇到它就是环，摘要按"不完整"处理 */
} EffState;

typedef struct FuncDef FuncDef;
typedef struct FieldDef FieldDef;
typedef struct TraitDef  TraitDef;
typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef enum {
    TY_UNRESOLVED,  /* a bare type name as the parser writes it; the checker resolves it */
    TY_VOID,
    TY_BUILTIN,     /* i32, bool, str, ... */
    TY_STRUCT,
    TY_ENUM,        /* `type Status = | ok | warn` */
    TY_REF,         /* ref T */
    TY_PARAM,       /* the type parameter itself: the `T` written inside a template */
    TY_GENERIC,     /* an instance, such as `Pair<i32, u8>` */
    TY_ARRAY,       /* a fixed array `[15]i32`: the length is part of the type */
    TY_FN,          /* `fn(A, B) -> R`: a **bare code pointer** with no environment. This is the
                     * type that crosses a C ABI boundary (`docs/topics/C-ABI.md`), not a closure:
                     * a lambda with captures is a different type (a capture struct + a `call`
                     * method), and only a capture-free lambda could ever be one of these. It has
                     * **no null value**, exactly like a non-nullable `ref` -- see
                     * `typeLacksZeroValue` -- and `option<fn …>` is how "may be absent" is
                     * written. */
    TY_DYN,         /* `dyn Trait`: a pool-backed `{pool, slot, gen}` name; `name` is the trait */
    TY_ERROR        /* dummy type for a failed check, so errors do not cascade */
} TypeKind;

struct Type {
    TypeKind    kind;
    const char *name;    /* TY_UNRESOLVED / TY_BUILTIN / TY_STRUCT / TY_ENUM;
                          * for TY_GENERIC it is the mangled name (see ttMangle) */
    Type       *inner;   /* TY_REF: the referenced type */
    bool        nullable;/* TY_REF: `?ref T`, a reference that may be absent. Its zero
                          * value is `null`; a non-nullable reference has no zero value.
                          * A `?T` that is not a reference becomes `option<T>` in the
                          * parser and never reaches this flag. */
    bool        mut;     /* TY_REF: may the target be written through it? Read-only is
                          * the default, and `mut ref T` is the writable form, so
                          * writability has to be asked for explicitly. */
    StructDef  *sdef;    /* TY_STRUCT / TY_GENERIC */
    TypeDef    *edef;    /* TY_ENUM */
    Vec         targs;   /* TY_GENERIC: the type arguments (Type*) */
    const char *param;   /* TY_PARAM: the parameter name, such as "T" */
    int         tpIndex; /* TY_PARAM: which parameter it is, by position */
    int64_t     asize;   /* TY_ARRAY: the length, a compile-time constant */
    /* TY_FN: `fn(A, B) -> R`, a bare code pointer (`docs/topics/C-ABI.md`).
     *
     * `params` holds the parameter types (`Type *`) in order and `ret` is the return type, which
     * is never NULL: `fn() -> void` is a real type and `void` is a real type in extC.
     *
     * Two fields of its own rather than a share of `inner` / `targs`, because a function type is
     * a constructor like any other and every walker over the type tree has to visit both halves:
     * sharing a field with a different meaning is how a walker silently walks half a type. The
     * mangle of a function type is a **typedef name** (`extc_fn_i64__i64`), because a C function
     * pointer declarator wraps the name ("int64_t (*f)(int64_t)") and the rest of this compiler
     * spells a type as a prefix of the declaration. */
    Vec         params;  /* TY_FN: the parameter types (Type*), in order */
    Type       *ret;     /* TY_FN: the return type; `void` for none */
    /* TY_BUILTIN: where an `impl i64 { ... }` block put the methods it attached. A builtin
     * scalar has no declaration body, so methods written for it from outside need a holder of
     * their own; `NULL` when no impl ever targeted this builtin. Every other kind is covered by
     * `sdef` / `edef`: a declared type already has a body to hold its methods. */
    StructDef  *mholder;
};

Type *typeNamed(Arena *a, const char *name);   /* TY_UNRESOLVED; targs may be filled in after */
Type *typeRef(Arena *a, Type *inner);
Type *typeParam(Arena *a, const char *name, int idx);
Type *typeArray(Arena *a, int64_t n, Type *elem);   /* TY_ARRAY */
/* Build a function type from the parser's pieces: `fn(params...) -> ret` (TY_FN).
 *
 * The parser hands over parameter and return types that may still be `TY_UNRESOLVED` names, so
 * this only builds the shape; interning happens in `ttResolve`. `ret` must not be NULL. */
Type *typeFn(Arena *a, Vec *params, Type *ret);

/* ------------------------------------------------------------ expressions */

/* Sentinel for "allocate into this function's home arena" -- the arena the caller
 * chose and passed down. It means the same thing as the -1 that `homeDepth` and
 * `arenaArg` carry. Those fields are all filled in by the checker and code
 * generation only translates them: it must never re-derive the answer from "does
 * this function have a home arena", because that would be a second source of truth
 * for a decision that is already made. */
#define ARENA_HOME (-1)

/* Deepest block level inside a statement / block (defined in codegen.c, used by the checker to size
 * a coroutine frame's per-level block arenas). A block counts as one level. */
int blkMaxLevel(Stmt *s);
int blkMaxOfBlock(Stmt *block);
/* 「家 zone」的哨兵（池的提权，见 PLAN #87）。与 ARENA_HOME 平行：
 *   0        = 这个调用点不建池（`zoneLevel` 的默认值）
 *   ZONE_HOME= 把它交给**调用者**选的那个地方（隐藏参数 `__extc_home_zone` 往下传）
 *   k >= 1   = 生在第 k 层那个地方 */
#define ZONE_HOME (-1)

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STR, EX_IDENT,
    EX_BIN, EX_UN, EX_CALL, EX_METHOD, EX_FIELD, EX_STRUCTLIT,
    EX_INDEX,     /* `a[i]`: an index */
    EX_SLICE,     /* `a[i..j]`: a view over part of an array */
    EX_ARRAYLIT,  /* `[1, 2, 3]`: an array literal */
    EX_REF,       /* `ref x`: taking a reference, so `ref` is an expression and not
                   * only a type modifier */
    EX_ASSOC,     /* `option<i64>::some(x)`: an associated function, meaning a function
                   * declared without `self` */
    EX_GENCALL,   /* `alloc<i32>(n)`: a call to a generic primitive; only the built-in
                   * primitives use this form so far */
    EX_TRY,       /* `e?`: propagate a failure to the caller; legal in three statement
                   * positions only */
    EX_DEREF,     /* `*p`: explicit dereference. Reading gives the value p points at,
                   * writing updates the place p points at. */
    EX_ENUMVAL,   /* `Status.warn`: the checker rewrites the EX_FIELD it parsed into this */
    EX_CONV,      /* `i32(x)` / `f64(y)`: an explicit conversion. extC widens
                   * implicitly only when nothing is lost, so narrowing, a sign change,
                   * or an integer/float conversion has to be written out.
                   * The syntax is `T(x)` rather than C's `(T)x` because the extC parser
                   * does not consult the symbol table, and `(T)x` would be ambiguous
                   * with a parenthesised expression -- C can tell them apart only
                   * because it knows the types. */
    EX_NEW,       /* `new T` / `new T[n]` / `new [N]T`: take a zeroed place from the
                   * arena of the current block. What is produced:
                   *   `new T`     => `mut ref T`, a place holding one T
                   *   `new [N]T`  => `mut ref [N]T`
                   *   `new T[n]`  => `mut slice<T>`, n elements, which is how a buffer
                   *                   is built */
    EX_COALESCE,  /* `a ?? b`: use the fallback when there may be nothing.
                   *   `opt ?? other`   => the payload if there is one, else other
                   *   `r ?? other`     => the value on success, else other
                   *   `p ?? q` on ?ref => q when p is null
                   * The semantics are `match a { some(v) => v  _ => b }` except that
                   * only one side is ever evaluated. */
    EX_SIGN,      /* `e!`: the author's signature on the value, saying "I vouch for
                   * this". `opt!` and `r!` yield the payload without any compile-time
                   * check that it is there; `p!` on a `?ref T` yields a `ref T`, on the
                   * author's word that it is not null. Nothing happens at runtime --
                   * the whole meaning of the signature is that being wrong is the
                   * author's problem, not the compiler's. */
    EX_NULL,      /* `null`: the zero value of a nullable reference (`?ref T`). It may
                   * appear only where the context already says which `?ref T` is meant;
                   * the type is supplied by adoptContextType, the same way an empty
                   * array literal picks up its element type. */
    EX_DYN,         /* `dyn Trait(x)`: the value form (a pool-backed handle) */
    EX_EXT,         /* `ext f(x)`: start one concurrent task (the `ext` sugar, CONCURRENCY.md
                     * "`ext` 与调度域"). It is an **expression**: it evaluates to a handle, so
                     * `let h = ext f(x)` is the normal spelling. Legal only inside a domain --
                     * that rule and the spawn itself are the checker's job; the parser only
                     * builds the node. */
    EX_LAMBDA,      /* `fn(x: i64) -> i64 { ... }`: a closure literal. The checker gives it a
                     * generated unique type (a capture struct + a `call` method), so a lambda is
                     * an ordinary value of an ordinary type; see docs/topics/LAMBDA.md. */
} ExprKind;

/* One entry of a lambda's capture list, `[mut ref hits]` (LAMBDA.md section 2).
 *
 * The list only ever names the captures the body **writes**: a read is captured by value and is
 * not written in the source. That is the author's decision, and it has a second payoff -- every
 * writable alias is visible in the program text, which is what the escape and arena passes read. */
typedef struct {
    const char *name;
    bool        mutRef;   /* written as `mut ref x`: the closure may write through it */
    int         line;
} LamCap;

struct Expr {
    ExprKind kind;
    int      line;

    /* ---- filled in by the type checker ---- */
    Type     *type;
    FuncDef  *func;     /* the function an EX_CALL or EX_METHOD resolved to; for `==`
                         * it is the eq method.
                         * On an **EX_IDENT** it means something else: this identifier is a
                         * **function used as a value** (`var f: fn(i64) -> i64 = double_it`), so
                         * the expression is a code pointer and codegen prints its address. The
                         * checker's `lookup` finds bindings and never functions, which is why this
                         * node arrives at the "undefined name" path and is claimed there. */
    /* `parallel::run(worker, ...)`: 内建识别出来的那个 worker（codegen 据此生成 trampoline）。 */
    FuncDef  *parWorker;
    /* `sys::domain::single()`: the checker marks the **call node** (the way it marks `parWorker`
     * above) because the node carries no `func` by the time codegen runs -- measured: an assoc
     * builtin call reached codegen with `e->func == NULL`, so keying on it could never fire. */
    bool      domNew;
    /* `std::sys::heap::extc_viewOf(p, n)`: the plate layer's one primitive. A flag for the same
     * reason `domNew` is one -- the declaration carries `@builtin`, so the node would otherwise have
     * to name a `FuncDef` with no body (and letting it be treated as a callee would drag a
     * body-less function into the effect summaries). Codegen emits a call to the runtime helper the
     * declaration triggers (`src/plate.c`). */
    bool      viewOf;
    /* `EX_EXT`: the domain this task is handed to (the nearest `d.run { … }` receiver, resolved by
     * the checker). Codegen registers the task there. */
    Expr     *extDom;
    FieldDef *field;    /* the field an EX_FIELD resolved to */
    Type     *assocOwner; /* EX_ASSOC: the instance type it resolved to, kept so the
                           * C name can be mangled */
    bool      needOp;   /* a comparison whose operand mentions a type parameter, so the
                         * check waits until the generic is instantiated. Set for every
                         * overloadable operator (`==` `!=` `<` `<=` `>` `>=` and the
                         * arithmetic ones), not only `==`: the deferral is one mechanism
                         * and code generation re-resolves whichever operator this node
                         * carries. */
    bool      deref;    /* The expression sits in value position while its type is
                         * `ref T`, so code generation emits `*(...)`.
                         * The checker treats a `ref T` as the `T` itself -- what may
                         * be done to it is decided by `ref` versus `mut ref` -- and the
                         * dereference is added back here, once, where the value is
                         * actually needed. */
    /* For the escape check: how deep does the storage live that the references in
     * this expression point at?
     *   0  = a parameter, static data, or unknown; a reference returned by a callee is
     *        covered by that callee's own return check
     *   >0 = the block depth of some local
     * Only meaningful when the type contains a reference. */
    int       refDepth;
    /* Which arena does this `new` allocate into?
     *
     * This number is the single authority for the level. The checker settles it and
     * code generation only translates it (through `arenaRefAt`); codegen never
     * decides for itself whether the function has a home arena.
     *
     * The default is the block depth of the statement, so the memory is reclaimed
     * when that block ends. When the escape check finds that the value is stored
     * somewhere that lives longer, the level is promoted to that place.
     *
     *     ARENA_HOME (-1) = this function's home arena, `*__extc_home`, the arena the
     *                       caller picked
     *     > 0             = block k of this function, released when that block ends
     *     0               = not decided yet; it exists only while the checker runs and
     *                       code generation never sees it
     *
     * A function may turn out to need a home arena only after the bodies of the
     * functions it calls have been checked, because the property is transitive. While
     * a body is being checked, `c->curFunc->needsHome` reflects the direct test only,
     * so a `new` inside a function that needs a home arena only through its callees
     * was given the block level, while the generated C allocated it in the home
     * arena. That was still safe -- the home arena lives longer -- but the tree and
     * the generated code held two different answers. `checkModule` therefore rewrites
     * every such `new` site to ARENA_HOME once the closure is known; the sites are
     * collected in `FuncDef.arenaSites`, filled in by `check_top.c`.
     *
     * Only EX_NEW goes through this path. `alloc<T>(n)` allocates in the current
     * block; the checker decides that too, but it takes no part in promotion or in
     * the home arena. */
    int       arenaLevel;
    /* The lexical block level of this site, for long-running programs.
     * The final pass overwrites `arenaLevel` with ARENA_HOME for every allocation in
     * a function that has a home arena, which erases the level the site really needs.
     * Once the constraints have been solved, a site that nothing pulled out of the
     * frame goes back to its own block, which is the level recorded here. */
    int       lexicalLevel;
    /* 这个调用点建出来的池该生在哪一层地方（`ZONE_HOME` = 交给调用者）。
     * 只有"建池的调用"（`e->func->makesPool`）才用得上；0 = 不是这种站点。
     * 与 `arenaLevel` 平行，提权就是把这个数变小（越小越长寿）。 */
    int       zoneLevel;
    bool      boxedCoro;         /* a coroutine call whose value is stored as a handle (codegen boxes it) */
    /* The strongest requirement escape analysis placed on this site: the smallest
     * level that satisfies every constraint it takes part in.
     *     -1  = no constraint touched it, so it keeps its own level (`lexicalLevel`)
     *      0  = it must outlive the frame, so it belongs in the home arena; only a
     *           function with a home arena can satisfy that, which the checker has
     *           already verified
     *     k>=1 = it must live until block k, so it belongs in that block
     *
     * A boolean "escaped" would not do, because being touched by escape analysis is
     * not the same as being required to outlive the frame. In a loop,
     * `nb = new item[cap * 2]` only has to live until the end of the current frame,
     * but treating "touched" as "goes into the home arena" made `main` emit a
     * `__extc_home` it does not have, and the generated C did not compile. */
    int       minAt;
    /* The shallowest level at which this value has been observed being published (stored
     * into a place, returned, or handed to a call that may keep it). `-1` means the
     * question has not been asked yet. Recording is the only thing the checking pass
     * does about levels: deciding which arena a site goes to is a separate pass over
     * these records, so that no decision depends on the order the checker happens to
     * walk the tree in. */
    int       storedAt;
    /* This `new` belongs to an `@overwrite` site, so there is only one block of
     * storage: it is allocated lazily in the function frame and cleared before every
     * reuse. The level therefore follows the function body rather than the block the
     * statement sits in. */
    bool      reuse;
    /* Are the references inside this value borrowed from outside this call, that is,
     * do they come from a parameter or from a call that returned them? A borrowed
     * value may not be stored anywhere that outlives this call, because the compiler
     * does not know how long it really lives: it may point into a local of the
     * caller's frame that dies before the destination does. */
    bool      borrowed;
    /* The main side of `??` is not free of side effects, so code generation has to
     * emit a temporary before the statement and apply the conditional to that
     * temporary: the main side must be evaluated exactly once.
     * The checker decides this -- it has already computed `repeatablePure` -- and code
     * generation only obeys the flag. */
    bool      needTemp;
    /* Which arena should this call pass to a callee that has a home arena? The answer
     * follows the shallowest `mut ref` argument: newly allocated objects live as long
     * as the storage that argument points into.
     *
     * This field is for the checker itself and is deliberately conservative -- a site
     * at level 0 is looked up as depth 0, so it may reject a program that is in fact
     * safe. Code generation does not read it: it reads `arenaArg`, which is already
     * resolved. */
    int       homeDepth;
    /* Which arena the call site actually passes, settled by the checker; code
     * generation only translates it.
     *     ARENA_HOME (-1) => `__extc_home`, this function's own home arena
     *     >= 1            => `&__extc_a[that block level]`
     *
     * It may differ from `homeDepth` on purpose: `homeDepth` is the conservative test
     * that keeps undefined behaviour out (a site at level 0 is looked up as depth 0),
     * while this is the choice that is actually executed. Both are decided by the
     * checker, so code generation makes no decision of its own. */
    int       arenaArg;
    /* This call site is not settled yet. While the body is being checked the answer
     * can only be "the current block", because "pass my home arena if I have one"
     * depends on the transitive `needsHome` closure, which is known only after every
     * function body has been checked; `checkModule` resolves it in its final pass.
     * Until then `arenaArg` holds the current block level, which is what the site
     * falls back to when the closure says there is no home arena. */
    bool      arenaArgPending;
    /* Did this identifier come from rewriting a qualified name, `io::readLine` into
     * its flat form? The loader sets this flag on that path only, which is how the
     * checker tells "the user wrote a qualified name" apart from "the user forgot the
     * module prefix". */
    bool      qualified;
    /* Explicit conversion: is a runtime check needed? Integer narrowing, a sign
     * change, and float to integer may all lose the value, while a conversion whose
     * result provably fits is not checked. What the compiler can prove at compile
     * time leaves no trace at runtime. */
    bool      convCheck;

    union {
        long long ival;
        double    fval;
        bool      bval;
        struct { const char *text; } str;                 /* without the quotes; escapes
                                                           * are kept as written */
        struct { const char *name;
                 /* The name exactly as written in the source, before module mangling;
                  * diagnostics have to echo it. It must be stored separately because
                  * `name` is rewritten by the loader into the mangled form (`open`
                  * becomes `lib$open`), while an error has to point at the word the user
                  * typed. Without it, the message degenerates into "please write
                  * `lib::lib$open`", which is nonsense. */
                 const char *srcName;
                 /* The C name of the binding this resolved to; it differs from `name`
                  * when one declaration shadows another. Filled in by the checker,
                  * because resolving names is the checker's job and code generation only
                  * prints the answer. */
                 const char *cname;
                 /* The binding itself, as resolved: a `Sym *` declared in
                  * check_internal.h, opaque here for the same reason as `Expr.cname`.
                  *
                  * Why it is pinned onto the node instead of being looked up again in
                  * the final pass: that pass runs after every function body has been
                  * checked, by which time the scopes are long gone, so `lookup` cannot
                  * find a local binding any more. The symptom was a false rejection in
                  * the `varArray_i32` case: `return v` reported `kind=4 dca=0 svd=0`, the
                  * recomputation was skipped, and a depth frozen before the solver ran
                  * was used instead.
                  * The same name can denote a different binding in a different scope, so
                  * only the answer from the moment of resolution is authoritative:
                  * `lookup` matches on the name and may find a namesake later. */
                 void       *sym; } ident;   /* `sym` is a `Sym *` kept opaque and cast back
                                             * through `IdentBinding` (check_internal.h)
                                             * for the same reason as `cname`: the AST does
                                             * not know the checker's types. */
        struct { const char *op; Expr *left, *right; } bin;
        struct { const char *op; Expr *operand; } un;
        struct { Expr *callee; Vec args; } call;          /* args: Expr* */
        struct { Expr *payload; const char *traitName; Type *payloadType; } dynv;  /* EX_DYN */
        /* `fn(params) -> ret [mut ref x, ...] { body }` (EX_LAMBDA). `captures` holds the
         * **written** entries only (the writable ones); the checker records what it actually
         * captured in `fields`, in declaration order, and builds `sdef` (the capture struct)
         * plus a `call` method on it. `tname` is that struct's generated name, which diagnostics
         * show as `fn(A) -> R`. See docs/topics/LAMBDA.md sections 3 and 4. */
        struct {
            Vec         params;    /* Param* */
            Type       *ret;       /* NULL: inferred from the body, like a named function */
            Vec         captures;  /* LamCap*: the explicitly written `mut ref x` entries */
            Stmt       *body;
            StructDef  *sdef;      /* filled in by the checker */
            const char *tname;
            Vec         inits;     /* FieldInit*: the environment's field values, in field order */
        } lambda;
        /* `ext f(x)` (EX_EXT): `call` is the spawned expression, syntactically an ordinary call.
         * The checker fills in what it resolved to; see the kind's comment in ExprKind. */
        struct { Expr *call; } ext_;
        struct { Expr *recv; const char *name; Vec args; } method;
        struct { Expr *obj; const char *name; } field;
        struct { const char *name; Vec inits; } lit;      /* inits: FieldInit* */
        struct { Expr *obj; Expr *index; } index;
        struct { Expr *obj; Expr *lo; Expr *hi; } slice;   /* lo / hi may be NULL */
        struct { Vec elems; bool rest; } arraylit;         /* elems: Expr*; rest = a trailing
                                                            * `...` is present */
        struct { Expr *operand; } ref;
        struct { Expr *operand; } deref;
        struct { Expr *operand; } sign;   /* `e!`: the author's signature */
        struct { Expr *main, *fallback; } coalesce;   /* `a ?? b` */
        struct { const char *typeName; Type *type; Expr *count; } new_;  /* `new T[n]` */
        /* `i32(x)` / `f64(y)`: a conversion, and one case that is not about numbers at all --
         * `fn(i64) -> i64(p)`, which turns C's `void *` into a callable code pointer (or back). For
         * the numeric form `typeName` is the scalar's name and `type` is filled in by the checker;
         * for the function-type form `type` is the **parsed** type (resolved by the checker) and
         * `typeName` is NULL, because a function type is not a name. See `C-ABI.md` section 9.9. */
        struct { const char *typeName; Type *type; Expr *operand; } conv;
        /* An associated function call: `typeName<targs>::name(args)`.
         * Writing the whole type is deliberate: nothing is guessed from context. */
        struct { const char *typeName; Vec targs; const char *name; Vec args; bool isCall;
                 const char *modPrefix; } assoc;
        /*   `isCall`: a `(` after the last segment means a call, its absence means a
         *   value, such as the constant `std::sys::io::STDOUT`. The loader uses it to
         *   tell a function in a module from a constant in a module, which cannot be
         *   decided from the shape alone. */
        struct { Expr *operand; } try_;   /* `e?` */
        struct { const char *name; Vec targs; Vec args; } gencall;
        struct { const char *typeName; const char *variant; Vec args; } enumval;
        /*   Empty args = a variant without payload (`status.ok`); non-empty = a
         *   payload construction (`shape.circle(2.0)`). */
    } u;
    /* `dyn Trait(x).m(...)`: stage 1 of DYN.md keeps construction and the call in one
     * expression, so the trait being dispatched through is a property of the call node rather
     * than a type of its own. The parser sets it and refuses to let it be stored; codegen reads
     * it to dispatch through the table instead of calling the implementation directly. */
    const char *dynTrait;
    /* `r.tag()` where `r: ref dyn Tag`: the checker unwraps the reference to find the trait, so
     * codegen can no longer see it from the node's type. The C expression is then a **pointer** to
     * the handle while `extc_dyn_slot` takes the handle by value, so codegen has to dereference --
     * this flag is what tells it. (Without it the compiler accepted `ref dyn Tag` and emitted C
     * that did not build.) */
    bool dynRecvViaRef;
    /* `f(x)` where `f` is a **value** of function type and not a declaration: the call goes through
     * the code pointer, so there is no `FuncDef` for codegen to name.
     *
     * The checker sets this once it has matched every argument against the signature the **type**
     * carries (`docs/topics/C-ABI.md` section 9 step 1). Without the flag, codegen's call path would
     * look for `e->func`, find NULL, and drop the call on the floor -- `if (!e->func) return "0"`
     * turns a missing call into a valid-looking product, which is worse than invalid C. */
    bool callViaFn;
};

typedef struct { const char *name; Expr *value; } FieldInit;

Expr *exprNew(Arena *a, ExprKind kind, int line);
Expr *exprIdent(Arena *a, const char *name, int line);   /* the one way an `EX_IDENT` is built */

/* ------------------------------------------------------------ statements */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK, ST_MATCH,
    ST_YIELD,               /* `yield e`: only inside a `-> coroutine<T>` body */
    ST_TRAP,                /* `trap(msg)`: the program cannot continue. **Not an exception**: there
                             * is no handler and no unwinding -- the process writes
                             * `file:line: trap: msg` to stderr and stops with code 1, down the same
                             * road the compiler's own checks take (bounds, division by zero), dying
                             * hook included (docs/DECISIONS.md 80). For what cannot be handled;
                             * expected failures are `result`/`option` values. */
    ST_DOMAIN               /* `d.run { … }`: a **domain block**. A statement on purpose: the block
                             * is not a value and cannot be used as an expression, the expression
                             * walkers never have to see it, and the statement walkers handle its
                             * body the same way they handle any other block. */
} StmtKind;

/* One arm of a `match`: `circle => { ... }`.
 *
 * A variant that carries a payload also carries a list of names to bind it to. */
typedef struct {
    const char *variant;   /* the variant name; `_` is the catch-all arm, which is not
                            * supported yet -- the slot is reserved for it */
    Vec         binds;     /* const char*: the names bound to the payload, that is, the
                            * `r` of `circle(r) => ...` */
    Stmt       *body;
    int         line;
} MatchArm;

struct Stmt {
    StmtKind kind;
    int      line;

    /* Set on the block that a `for x in SUBJ { … }` was desugared into (the parser cannot decide
     * whether SUBJ is sliceable -- it does not consult the symbol table -- so this flag is how the
     * checker finds the candidate for the iterator protocol). See `checkBlockBody`. */
    bool     forDesugar;

    Type    *type;      /* ST_VAR: the final type of the declaration, filled in by
                         * the checker */

    /* `for` desugars to a `while` whose body ends with the loop's step statement (`parseFor`).
     * This points at that step from the **body block**, and codegen emits a label right before
     * it so `continue` can jump there: C's `for` runs the step on the way out of a `continue`,
     * and without this the desugared form skipped it -- an infinite loop for any `for` that
     * contains a `continue` (audit P0-15). It is cleared when `forRetargetToIterator` drops the
     * step (the iterator's own `next()` in the condition advances instead, so a plain
     * `continue` is right again). */
    Stmt    *forStep;

    /* Set on a `while` whose **condition allocates**: `checkStmt` measures it by counting
     * allocation sites across the condition's own check. Codegen then gives the condition its own
     * arena level and releases it at the top of every round -- without that, the condition's
     * allocations live until the surrounding block ends, so a loop that allocates in its condition
     * grows with the number of rounds (measured: `while (new i64[1000000])[0] == 0`, 20000 rounds,
     * RSS 81.5 MB). 定案 101②. */
    bool     condAllocs;

    union {
        struct { const char *name; Type *ann; Expr *init; bool mut;
                 /* The name used in the generated C; a `let` that shadows another one
                  * in the same block becomes `a__2`. Filled in by the checker. */
                 const char *cname;
                 /* `@overwrite var n = new T` reuses a single block of storage: it is
                  * allocated once in the function frame, lazily on first use, and
                  * cleared before every later execution of this statement. Legal only
                  * for `new`, which the checker enforces. */
                 bool overwrite; } var;
        /* `op` is NULL for a plain `=` and the operator text (`"+="` ...) for a compound
         * assignment; `opExpr` is the `EX_BIN` the checker resolved for it, which codegen
         * needs when the operator is a user-defined method (`x += y` is then
         * `x = add(x, y)`, because C's `+=` knows nothing about the method). */
        struct { Expr *target; Expr *value; const char *op; Expr *opExpr; } assign;
        struct { Expr *value;                /* ST_YIELD: the value handed to the resumer */
                 const char *bind;           /* `var x = yield e`: where the resume's value lands */
                 const char *bindCName;      /* the frame field it becomes */
                 Type       *bindAnn;        /* the written annotation, if any */
                 int         bindLine; } yield_;
        struct { Expr *cond; Stmt *thenBody; Stmt *elseBody; } ifs;
        struct { Expr *cond; Stmt *body; } whiles;
        struct { Expr *value; } ret;
        struct { Expr *msg; } trap_;      /* ST_TRAP: the message (`slice<u8>` / `ref u8` / literal) */
        struct { Expr *expr; } expr;
        /* ST_DOMAIN: `callee` is the `d.run` that opened the block, `body` is the block itself.
         * Only a value of the language-level object `domain` may carry one (check_stmt.c). */
        struct { Expr *callee; Stmt *body; } domain_;
        struct { Vec stmts; } block;                      /* stmts: Stmt* */
        struct { Expr *scrutinee; Vec arms; } match;      /* arms: MatchArm* */
    } u;
};

Stmt *stmtNew(Arena *a, StmtKind kind, int line);

/* -------------------------------------------------------------- top level */

typedef struct {
    const char *name;
    const char *cname;   /* the name used in the generated C, as for Expr.cname */
    Type       *type;
    int         line;
} Param;

struct FieldDef {
    const char *name;
    Type       *type;
    int         line;
    /* `@private`: reachable only from the module that declares it. Storage is the reason this
     * exists as much as behaviour is -- a public `buf`/`vals` hands the reader a view into pool
     * storage, which is exactly what the pool tier forbids (POOLS.md 5.4, ruling #89). */
    bool        isPrivate;
    /* `effects Addr=… Cont=…` on a **field of function-pointer type**: the signature of the function
     * that slot will hold.
     *
     * It exists for the table (`HEAP.md` section 2): the callee is not a declaration -- the host fills
     * the slot at run time -- so "I do not keep the pointers you hand me" has nowhere to live except
     * the slot itself. Without it a call through the slot gets the conservative answer every
     * unsigned `extern!` gets ("it may keep everything"), which is safe and makes the table unusable.
     *
     * The masks mean exactly what they mean on a declaration (`FuncDef.extAddrMask` / `extContMask`),
     * and the call site hands them to the **same** rule (`checkCallRefArgs`), so a signed slot and a
     * signed `extern!` cannot drift apart. `hasEffects` says the author wrote the clause; a slot
     * without one keeps the conservative default. */
    bool        hasEffects;
    unsigned    effAddrMask;
    unsigned    effContMask;
    /* `effects Ret=0` on the slot: "the value I return does not point into the caller's frames".
     * Believed for a `fn` value exactly like `Addr`/`Cont` are, and it is the fact that lets a
     * caller keep a **view** returned by this slot in a local variable and still hand its `.data`
     * to C (see `Sym.outOfFrame`). Signed by whoever wrote the declaration -- for the standard
     * library that is once, in the privileged layer. */
    bool        effRetFresh;
};

typedef struct {
    const char *name;
    int         line;
    /* The payload: the types inside the parentheses of
     * `| circle(f64) | rect(f64, f64)` (types: Type*). Empty means a variant without
     * payload, such as `| outOfRange`. The types are positional and carry no field
     * names, and a `match` binds them positionally too: `circle(r) => ...`. */
    Vec         types;
} Variant;

struct TypeDef {                 /* type status = | ok | warn | error */
    const char  *name;
    Vec          typeParams;     /* const char*: the generic parameter names of
                                  * `type option<T>` */
    Vec          variants;       /* Variant* */
    Type        *type;           /* the interned type, filled in by the checker */
    bool         reserved;       /* came from the prelude, so the user may not redefine it */
    /* Which file it came from, which module, and whether it is `@private`. */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int          line;
    /* The name shown to the user (`io::reader`), while `name` is the internal mangled
     * form (`io$reader`). The two must stay apart: printing `io$reader` in a diagnostic
     * leaks the compiler's internal encoding for a word the user never wrote. It was
     * observed once as `struct \`alpha$pair\` has no field \`zzz\``. For a single-file
     * program the two are the same string. */
    const char *srcName;
};

struct StructDef {
    const char *name;
    Vec         typeParams;      /* const char*: the generic parameter names, such as
                                  * "T"; empty for a non-generic struct */
    Vec         fields;          /* FieldDef* */
    Vec         methods;         /* FuncDef*: methods are declared inside the struct body */
    Type       *type;            /* the interned type of a non-generic struct; a generic
                                  * one is interned by ttGeneric instead */
    bool        reserved;        /* came from the prelude: the user may neither redefine it
                                  * nor add methods to it */
    /* Non-NULL when this struct is a **coroutine frame** the compiler synthesized: a call to that
     * coroutine evaluates to this type, and codegen derives `$frame` / `$step` from `coroOf`
     * (docs/topics/CONCURRENCY.md 4.4). */
    FuncDef    *coroOf;
    /* Synthetic holder for a **builtin scalar** that an `impl` block attached methods to.
     * It is pushed into the module's struct list so that every "struct x method" pass
     * (signature resolution, body checks, escape/borrow rules, operator collection, code
     * generation) reaches those methods with no pass changed at all, but it is NOT interned
     * under its name (so `i64` still resolves to the builtin, never to a struct) and code
     * generation never emits a C struct for it. */
    bool        builtinHolder;
    /* `@noCopy`: this type may not be copied by value -- it may only be passed as
     * `ref` / `mut ref`. A type whose state has an **identity** is the reason: copying a
     * `reader` (or an `ifstream`) gives two objects over one buffer with two independent
     * positions, and reads then interleave in a way neither name shows. Every language
     * that has such types says so one way or another -- C++ deletes the copy constructor,
     * Rust moves and borrows, Go passes a pointer, Python has no implicit object copy;
     * extC writes it down as an annotation. */
    bool        noCopy;
    /* `@frozen`：作者签字 —— **这个类型的布局就是 C 的布局**：字段按声明顺序、按 C 的对齐与填充，
     * 没有隐藏字段。它是"按值过 C 边界"的**门票**（`ttCrossesC` 只认带这个标记的结构体）。
     *
     * 它是签字，与 `effects` 同一类：编译器**不**去审"里面放了什么"。往里放 `slice<T>`、放枚举、
     * 放泛型参数都是作者的自由 —— 代价是 C 那边得**照抄我们发射的形状**，对不上是作者的账
     * （同一条规矩：特殊的权力不给太多，但也不替作者判他傻不傻）。
     *
     * 编译器欠这笔签字的正是另一半，而且是硬的：
     *   · **永不重排字段、永不加隐藏字段** —— 将来想要"塞一个判别位/世代号进结构体"的功能，
     *     只能放到别处去；
     *   · 在产物里用两条**平台无关**的静态断言把这件事钉住（相邻字段 `offsetof` 递增；
     *     `sizeof` 不超过"最后一个字段 + 它的对齐"），于是任何一次静默漂移都变成产物**编不过**，
     *     而不是某个调用点读出错的值。见 `C-ABI.md` §9.8。 */
    bool        frozen;
    /* `@sharesStorage`：这个类型按值拷贝时**两份共用同一块存储**（容器就是这一族）。
     *
     * 作者口径（2026-09-26）：不禁止，**报警告**。理由是"万一用户就是神人"——
     * 有些用途（把句柄交出去、短暂共享）是正当的，禁掉就把路堵死了；但用户必须
     * **知道**这一行在做什么，因为它与"别的值复制都是各一份"这条直觉相反。
     *   实测的坑（tests/stl/clone.extc 的起因）：`var b = a` 之后 `b.push(..)` 会改到 `a`，
     *   而 `a.release()` 会让 `b` 悬垂（守卫能把它变成带位置的 trap，但那已经是事故了）。
     * 与 `@noCopy` 的分工：`noCopy` 是"有身份的状态，复制一定错"（读者/流）；
     * `sharesStorage` 是"复制的语义与直觉不同，但你可能是有意的"。 */
    bool        sharesStorage;
    /* `@poolObject`：这个类型**拥有一个池**（存储住在池自己的板块上，寿命随它所在的地方）。
     *
     * 作者口径（2026-09-26）：声明比推断好 —— 编译器不必去猜（早先试过"看有没有 `pid` 字段"
     * 和"看它的方法建不建池"，前者是魔数、后者要等闭包），一个修饰符就把这件事说清楚，
     * 而且**用户自己写的容器用同一个修饰符**声明，走完全一样的路。
     *
     * 有了它，编译器能确定地做三件：① 该类型的构造调用是**池站点**（可提权）；
     * ② 建池的函数收隐藏的「家 zone」参数；③ 将来若要按类型判"元素不许带引用"，
     * 判据也是它，而不是任何命名约定。 */
    bool        poolObject;
    /* 这个结构体的**某个方法**会建池（闭包里顺手算出来的，见 `FuncDef.makesPool`）。
     * 为什么需要它：泛型方法的**模板**与**实例**是两份 FuncDef，闭包只标到模板那一份，
     * 而调用点上 `e->func` 可能是模板 ⇒ 读取处光看 `f->makesPool` 会看到假
     *（实测：`[promote] call new owner=vector$vector makesPool=0`，而 codegen 那一侧为真）。
     * 只给**关联函数**（构造函数那一族）放宽，免得波及其它方法。 */
    bool        makesPoolAny;
    /* Which file it came from, which module, and whether it is `@private`. */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
    /* The name shown to the user (`io::reader`), while `name` is the internal mangled
     * form (`io$reader`). The two must stay apart: printing `io$reader` in a diagnostic
     * leaks the compiler's internal encoding for a word the user never wrote. It was
     * observed once as `struct \`alpha$pair\` has no field \`zzz\``. For a single-file
     * program the two are the same string. */
    const char *srcName;
    /* A lambda's environment struct: the signature it was written as, `fn(i64) -> i64`, so a
     * diagnostic shows what the user wrote instead of the generated `$lam$f$3`. NULL otherwise
     * (docs/topics/LAMBDA.md). */
    const char *lamSig;
};

struct FuncDef {
    const char *name;
    /* The type parameters of a generic free function, `fn f<T, U>(...)`. A method
     * keeps its type parameters in `owner->typeParams`, so this field is used by free
     * functions only. An instance points back at its template through `tmpl`, holds the
     * instance's type arguments in `targs`, and takes its C name from `instName`. */
    Vec         typeParams;      /* const char* */
    Vec         targs;           /* Type*: only an instance has these */
    FuncDef    *tmpl;            /* non-NULL when this is an instance, not the template */
    const char *instName;        /* the C name of an instance, such as `max_i32` */
    Vec         params;          /* Param* */
    /* The binding each parameter resolved to, in declaration order. `void *` because `Sym` is
     * defined in the checker's own header, which ast.h does not see; only the checker reads this.
     *
     * The effect summary is computed after every body has been checked, when **no scope exists**,
     * so "which parameter does this value come from" cannot be answered by a lookup and was
     * answered by comparing names instead. Matching by name is wrong the moment a local shadows a
     * parameter (legal, documented): audit P0-6(d) declared `var s: slice<u8> = q`, the store was
     * attributed to parameter `s`, and the call site checked the wrong argument. Identity has to be
     * recorded while the scope is still there (INV-I). */
    void       *paramSyms[64];   /* must equal EFF_MAX_PARAMS, the summary mask width */
    int         nParamSyms;
    Type       *ret;             /* NULL when the function returns nothing */
    /* A coroutine: the declared return type is `coroutine<T>`, and the body may `yield`.
     * `yieldType` is that `T`; the frame the checker lays out for it is a **value** (see
     * docs/topics/CONCURRENCY.md 4.4). The declaration is a marker: the call's real type and the
     * protocol methods are synthesized per coroutine (slice B). */
    bool        isCoro;
    /* Some `ext f(x)` starts this coroutine as a task. The stable adapter that the domain's task
     * table needs -- `bool (*)(void *)` while `$next` takes a concrete frame pointer, and calling one
     * through the other's type is undefined -- is emitted **only** for these, so a program that never
     * starts a task carries nothing extra. */
    bool        isExtTarget;
    /* A lambda's `call` method: its body was already checked **in place**, in the scope the lambda
     * was written in (that is where the capture set comes from), so the pass that checks struct
     * methods must not check it a second time. */
    bool        lamChecked;
    /* A lambda whose result type was left out: the first `return <value>` decides it, and this flag
     * is what lets that return through before the type is known (check_stmt.c, ST_RETURN). */
    bool        lamInferRet;
    Type       *yieldType;
    /* The synthesized per-coroutine value type: what a **call** to this coroutine evaluates to.
     * `ret` is set to it as well, so `let c = f(x)` binds a coroutine of its own concrete type. */
    Type       *coroFrameType;
    Type       *coroRetProto;
    /* Named from **outside** its instance: a `dyn` table's thunk calls it, so its body must survive
     * the "definitions nothing names" pass (which cannot see that reference). */
    bool        dynTable;
    /* Non-zero on the two protocol methods the checker synthesizes on a coroutine frame:
     * 1 = `next` (advance the state machine), 2 = `value` (read the return slot). Codegen emits
     * those two inline instead of calling a function (docs/topics/CONCURRENCY.md 4.4). */
    int         coroProto;
    int         coroKind;           /* codegen prepass: this coroutine's index, for handle dispatch */
    bool        coroBoxed;          /* a handle of it was made somewhere ⇒ it always gets a task */
    /* Does this coroutine need a **task place** (its own zone)? Decided after the pool fixpoint and
     * read by codegen (docs/topics/CONCURRENCY.md 4.4, slice C). */
    bool        coroNeedsZone;
    /* The locals that live across a `yield`, laid out by the checker for codegen (slice B):
     * `pc`, the return slot and (when `makesPool`) the zone id come first, then these by value.
     * See docs/topics/CONCURRENCY.md 4.4. */
    Vec         coroFrame;       /* Param* */
    Stmt       *body;            /* ST_BLOCK */
    StructDef  *owner;           /* the struct a method belongs to; NULL for a free function */
    bool        isAssoc;         /* declared inside a struct body but without `self` */
    /* An external declaration, `extern!("libc") fn ...`. The C side is a black box, so
     * whoever calls it has to vouch for it:
     *   hasEffects - the declaration carries `effects Addr=... Cont=...`, which is the
     *                author's promise that it does nothing else
     *   not given  - the worst case is assumed: every argument may be stored somewhere,
     *                which makes the function nearly unusable but never unsound
     *   owned      - returning memory that the caller owns is rejected outright for now
     *                and waits for the frame-owns-resources work */
    bool        isExtern;
    /* `@inline fn f(...)`: the call must be inlined into its callers.
     *
     * This is a request the language cannot express with a keyword of its own -- ISO C has
     * `inline`, but nothing that *requires* inlining -- so the generated C asks for it with
     * a compiler attribute, behind a `__GNUC__` guard. The attribute reports an error when
     * it cannot be honoured, which is the behaviour wanted here: a request that cannot be
     * met must not quietly do nothing.
     *
     * Measured on the reader's byte-at-a-time path: the call and return per byte cost 32%
     * even with the attribute, and rewriting the loop to work on the buffer instead cost
     * 48%; the attribute is what a library reaches for first, and it is what makes the
     * difference between 33.7ms and 22.9ms over 3e6 integers. */
    bool        isInline;
    /* `@unchecked fn f(...)`: **the bounds checks on the index expressions inside this body
     * are not emitted.**
     *
     * The promise the language makes everywhere else -- out-of-range means a located `trap:`,
     * never undefined behaviour -- is deliberately given up here, and only here, by writing
     * it out. That puts the annotation in the same category as `extern!` and `!`: the
     * programmer signs for a fact the compiler cannot check, and the manual says what the
     * signature costs (`docs/manual/18-modules.md` 12.5, `docs/manual/01-safety.md`).
     *
     * The granularity is the whole function, on purpose: an `unchecked { ... }` block would
     * make the guarantee depend on a statement's position, and a reader has to see, before
     * reading the body, that this one body is on its own. What the annotation covers is the
     * **element index** `x[i]` on `[N]T` and on `slice<T>` / `mut slice<T>`; a **range**
     * `x[a..b]` keeps its check (`genSlice` does not consult this flag).
     *
     * Nothing else changes: an out-of-range index was never the only thing a subscript did,
     * and the checker's own static rejections (a constant index outside a known length at a
     * site it can see) still happen. Codegen only stops emitting the trap. */
    bool        isUnchecked;
    /* `@builtin`: 这个声明由**编译器**实现（没有函数体），库只提供可发现性与文档。
     * 理由与 `alloc<T>`/`poolSlice<T>` 相同：它要生成的代码（并行 worker 的 trampoline）
     * 必须由编译器命名那个函数，库给不出。目前只有 `std::parallel::run`。 */
    bool        isBuiltin;
    /* `@export`：**这个函数的 C 符号是给外面用的** —— 产物里它不是 `static`，而且它的 C 名
     * 就是源码里写的那个（`$` 之前那段，见 codegen 的 `cFuncName`），于是 C 那边能
     * `dlsym(h, "init")` 找到它。这是 `extern!` 的另一面：那个是"我调别人"，这个是"别人调我"。
     *
     * 三条配套的硬规矩，都在实现处各自报错：
     *   · **签名必须能过 C 边界**（复用 `ttCrossesC` —— 与 `extern!` 同一把尺子）；
     *   · **不能有隐藏参数**（收 home arena / zone 的函数会多一个 C 参数，调用方看不见）；
     *   · **不能被可达性剪枝剪掉**（extC 侧没人调它，剪枝只看"有没有人提这个名字"）。
     * 泛型、方法、协程、`extern!` 都不能导出：它们没有"那一个 C 符号"。 */
    bool        isExport;
    /* 被 `parallel::run` 用过一次的 worker：codegen 要给它生成一个 static trampoline
     * （语言层没有函数指针，所以这份"取地址"只能由编译器写出来）。 */
    bool        isParWorker;
    /* 这个函数（worker 或它调用链上的辅助函数）里的 `new` 要用**线程本地** arena：
     * worker 的签名固定（只写 out、只返回 i64）⇒ 分配逃不出去 ⇒ 线程本地是安全的；
     * 而帧 arena 是主线程的，两个 worker 同时用就是数据竞争 ⇒ 必须换。 */
    bool        parTlsArena;
    const char *externLib;       /* the name in `extern!("libc")`, used in diagnostics */
    bool        hasEffects;
    unsigned    extAddrMask, extContMask;
    /* `effects Ret=0` on the declaration: "the reference/view I return does not point into the
     * caller's frames" (the return side of `Addr`). Believed for an `extern!` (a black box) and
     * for an extC function as well, because a library author writes it where the memory's
     * lifetime is known -- `std::heap`'s `alloc`/`view`/`allocPtr` are the first users: the plate
     * region is `mmap`ed and lives to `close()`, so its views are `Ret=0` by construction. */
    bool        extRetFresh;
    /* `Thread=N`: N = 0 表示这次调用**不碰跨线程共享的状态**（模块级 `var`、运行期缓冲、输出流），
     * 因此它可以出现在 worker 里；N = 1（缺省）表示它碰，保守处理。
     *
     * 与 `Addr`/`Cont` 同一性质：**作者签字**，不是编译器算出来的。区别在于这两个已经有人读
     * （寿命分析），而 `Thread` 至今只被存下来 —— 它服务的是并行那一步：worker 体内只许调用
     * `Thread=0` 的东西，诊断才有可能说人话（"`fs` 用了模块级缓冲"），而不是笼统地拒绝一切。 */
    unsigned    extThreadMask;
    bool        reserved;        /* came from the prelude */
    /* Does this function need a home arena?
     *
     * The rule: the body allocates and the return type contains a reference or a view.
     * Such a function takes one extra hidden parameter, `extc_arena *__extc_home`, and
     * every `new` in it allocates in the arena the caller picked. A call site therefore
     * passes its own `__extc_home`, or `&__extc_a[its block level]`.
     *
     * A caller needs something to pass, so the property is transitive: it is computed
     * as a fixed point over the call graph. */
    bool        needsHome;
    /* Does this function really place something in its home arena?
     *
     * `needsHome` is the conservative answer of an *escape* question ("could this function
     * hand out storage that outlives the call?"), and it is deliberately wide: `new i32`
     * whose value is copied into an out-parameter marks the function even though nothing
     * escapes. The hidden parameter is only needed for the precise question, which the
     * placement pass answers per allocation site: if no site of this body ends up at
     * `ARENA_HOME`, and no call passes this function's home on, then the parameter is
     * written by nobody and read by nobody - `examples/out-param.extc` had exactly that, and
     * gcc reported the unused parameter. The signature and the call sites both use this
     * flag, so they stay in step. */
    bool        usesHome;
    /* Does this function ever put anything into its own block arenas?
     *
     * When it does not, the generated C omits both `extc_arena __extc_a[N]` and the
     * string of `extc_arena_release` calls. About half of the functions in real programs
     * are pure computation of this kind, while the arena boilerplate accounted for
     * roughly 22% of the generated lines.
     *
     * Test: the body contains a `new`, or it calls a function that has a home arena,
     * because such a call writes into this function's block arena.
     *
     * Must be computed after the transitive `needsHome` closure has run; computing it
     * earlier misses a callee that turns out to have a home arena. */
    bool        mayUseArena;
    /* Can this function create a pool, directly or through what it calls?
     *
     * A created pool is registered in the zone (the `place`) current at the call, and
     * `extc_pool_new` returns -1 when no zone is current. So the zones are emitted where
     * they can be needed - and only there: a block whose direct statements cannot reach
     * `extc_pool_new` gets no `zoneEnter`/`zoneLeaveTo` pair. A loop body that only calls
     * `v.push` used to pay one pair per iteration, and on `bench/stl/vector.extc` those
     * hooks were 80% of the profile.
     *
     * Test: the body calls `extc_pool_new`, or it calls a function that does. The answer is
     * the *least* fixed point of that rule over the call graph, taken by `checkModule` once
     * every body has been checked - the walk reads `e->func`, which checking fills in.
     *
     * Conservative in the one direction that matters: an unresolved callee counts as
     * creating a pool. An unneeded hook costs time; a missing one costs a pool with no zone,
     * which makes `extc_pool_new` return -1 and the registry lose the record. */
    bool        makesPool;
    /* Nodes in this body whose arena answer has to wait for the transitive `needsHome`
     * closure, in the order they were checked (`Expr*`):
     *   - an `EX_NEW` site: with a home arena, `arenaLevel` becomes ARENA_HOME, so every
     *     allocation goes there
     *   - a call site with `arenaArgPending`: with a home arena, `arenaArg` becomes
     *     ARENA_HOME
     * `checkModule` resolves them in its final pass, once the closure is known.
     *
     * The list exists because the closure is computed only after every function body has
     * been checked, and walking the tree again at that point would mean writing another
     * expression visitor: a switch over the shape of the AST is exactly the code that is
     * forgotten when a node kind is added, and this compiler already has several.
     * Recording the sites while the body is checked costs no extra traversal. */
    Vec         arenaSites;
    /* `@overwrite`: how many reuse sites this body has, and where their storage lives.
     * The storage has to outlive the whole period over which a site is reused:
     *   - the site is in `main`, or this function can reach itself, that is, it recurses:
     *     the storage goes into this function's own frame, so every activation gets its
     *     own block -- a single shared block would let a recursive call clobber storage
     *     its caller is still using
     *   - otherwise the storage is held by the frame of the call site and passed in as
     *     an opaque hidden `void **` parameter. That is the only way a loop in the caller
     *     can really reuse a `new` that happens inside the callee, which is the leak
     *     shape this feature exists to remove */
    int         owSites;
    bool        owLocal;
    /* Does this function allocate, directly or through the functions it calls?
     *
     * The answer is needed while a caller's own body is being checked, because the call
     * may put fresh storage into the caller's container, and that is earlier than the
     * `needsHome` closure can be computed.
     *
     * Computed lazily from the declaration and its body, then cached:
     *   0 = not computed, 1 = allocates, 2 = does not, 3 = being computed.
     * State 3 breaks cycles, and a function on a cycle is conservatively treated as
     * allocating. See `funcAllocates` in check_top.c. */
    int         allocState;
    /* Does this function print, directly or through the functions it calls?
     *
     * Used to decide whether an earlier call in the same statement has an observable
     * effect that the temporary variable for `??` would end up jumping ahead of.
     *   0 = not computed, 1 = prints, 2 = does not, 3 = being computed (a cycle is
     *   treated as "prints"). */
    int         mayPrintState;
    /* Has this method or function ever been called?
     *
     * Generic instantiation rechecks and emits only the methods that are actually
     * called. Instantiation used to recheck every method body of the type, so an unused
     * `slice<T>::==` demanded `T: ==`, and every struct used in a container had to
     * declare `fn ==` whether or not anything ever compared two of them.
     *
     * This is a conservative approximation: a call inside a template body counts as a
     * use, so a function may be checked and emitted more often than necessary, never
     * less. */
    bool        used;
    /* Effect summary: what does this function store into its `mut ref` parameters?
     * Bit i stands for parameter i.
     *   addrMask  : the address of argument j, or the address of one of its fields, was
     *               stored. Storage reachable as a slot from argument j must therefore
     *               live at least as long as the destination.
     *   contMask  : a pointer read out of argument j was stored, so what that pointer
     *               points at must live at least as long as the destination.
     *   otherMask : something that cannot hold a reference but can still flow out;
     *               anything uncertain is counted here, which keeps the summary
     *               conservative.
     * addrFromLocal records that the address of a local of this frame was stored, which
     * is a call a caller must never make. */
    /* 64 bits, and that is a **contract**, not a convenience (INV-P, 定案见 R1/R3 批次):
     * the mask is indexed by parameter position, so a function with more parameters than bits
     * would silently lose the high ones -- `P1-4` in the audit: the same shape with 4 parameters
     * was rejected while its 40-parameter twin was accepted, because parameter 40's `addrMask`
     * bit fell off the end. `EFF_MAX_PARAMS` is checked when a function is declared, so the
     * silent-loss case cannot arise. Widening to an array is the escape hatch if a real program
     * ever needs more. */
#define EFF_MAX_PARAMS 64
    uint64_t    addrMask, contMask, otherMask;      /* destination: a parameter's container */
    uint64_t    homeAddrMask, homeContMask;         /* destination: memory allocated here */
    unsigned    freshCount;                         /* count only: fresh locals seen this round */
    /* State of the transitive closure of the effect summary:
     *   0 = not computed, 1 = computed (`effComplete` says whether it can be trusted),
     *   3 = being computed, which breaks cycles.
     * `effUnknown` records whether any call could not be resolved; such a summary is
     * never complete, so it is treated conservatively. */
    EffState    effState;
    bool        effComplete;
    bool        effUnknown;

    bool        addrFromLocal;
    Vec         callees;      /* FuncDef*: the functions it calls, used to build the
                               * call graph and find its strongly connected components */
    /* Which file does this declaration come from, and which module?
     *   ctx       - the Ctx of that file; a diagnostic has to go through it so that it
     *               names the right file and quotes the right source line
     *   modName   - the short module name, the `foo` of `use foo`; NULL for the root file
     *               and for the prelude
     *   isPrivate - `@private`: a reference from another module is a compile error.
     *               Declarations are public by default. */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
};

/* A global variable or constant: a top-level `let` or `var`.
 *
 * A global has depth 0, because it outlives every frame. The escape rule therefore
 * already forbids storing anything local into a global (0 >= 1 is false), so globals
 * need no special case of their own. A global of fixed size needs no arena either:
 * it becomes a static object in the C output. */
typedef struct {
    const char *name;
    Type       *ann;         /* the type annotation; optional, inferred from the initialiser */
    Expr       *init;        /* the initialiser; NULL means zero-initialised */
    bool        mut;         /* true for `var`, false for `let` */
    bool        reserved;
    /* Which file does this declaration come from, and which module?
     *   ctx       - the Ctx of that file; a diagnostic has to go through it so that it
     *               names the right file and quotes the right source line
     *   modName   - the short module name, the `foo` of `use foo`; NULL for the root file
     *               and for the prelude
     *   isPrivate - `@private`: a reference from another module is a compile error.
     *               Declarations are public by default. */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
} GlobalDef;

/* A semantic import, not a textual include the way C does it:
 *     use std::io   =>  the loader finds std/io.extc, parses it, and rewrites every
 *                       `io::name` in this file into the flat name, before the checker
 *                       runs
 * The short name is the last segment of the path; `as` aliases are not supported yet,
 * and an import cycle is a compile error. */
typedef struct { const char *from; const char *to; } Alias;

/* One name that an import put in scope: the module that wrote it (NULL for the root
 * file), the module it came from, and the name -- NULL meaning **every** public name of
 * that module (`use mod::*`, as opposed to `use mod::{a, b}`). */
typedef struct { const char *importer; const char *opened; const char *name; } Open;

typedef struct {
    const char *path;      /* "std::io", exactly as written; used in error messages */
    const char *shortName; /* "io": members are then referred to as `io::name` */
    const char *file;      /* filled in by the loader: the file it resolved to */
    int         line;
    /* `use std::io::*`: the module's **public names are in scope unqualified** as
     * well. Qualified names keep working (they always do), and a `@private` name is
     * still out of reach: opening a module is not a way around privacy.
     *
     * The default is what 定案 70 settled on -- a name from another module is written
     * `mod::name` -- and this is the opt-in that a user asks for by writing the star,
     * which is why the form is explicit rather than implied. */
    bool        wildcard;
    /* `use std::io::{cin, cout}`: **these** names come into scope, and nothing else.
     * The braces make the meaning unambiguous -- a name list is never a module path,
     * so no file appearing later can change what the line means. Entries are
     * `const char *`. */
    Vec         names;
    void       *unit;      /* ModUnit* of the resolved module; filled in by the loader, so
                            * that the `::*` / `::{...}` rewrite can reach that unit's name
                            * table without threading the loader through every pass */
} UseDecl;

/* Does this function **take** a home arena -- i.e. does a call to it hand one down?
 *
 * One spelling, two askers: the checker's precise question (which decides whether a call site hands
 * an arena down) and codegen's implicit-argument owner (`cgImplicitArgs`, which both emits the
 * parameter in a signature and passes the argument at a call site). They must never disagree: a
 * definition declaring `extc_arena *__extc_home` while its call site passes nothing is exactly the
 * bug this closes (gcc: `too few arguments to function ...`).
 *
 * `needsHome` is the **other** question -- "does it reach one?" -- and deliberately not this one: it
 * decides whether a body needs an arena of its own, not whether a call passes one. */
static inline bool funcTakesHomeArena(const FuncDef *f) { return f && f->usesHome; }

/* Does this function **take** the hidden home zone -- i.e. does a call to it hand one down?
 *
 * One spelling, two askers, same shape as `funcTakesHomeArena`: the checker's promotion decision
 * and codegen's implicit-argument owner (`cgImplicitArgs`, which emits both the parameter in a
 * signature and the argument at a call site).
 *
 * **Known divergence, not yet fixed** (review F9): the checker promotes on the richer
 * `calleeMakesPool` (`check_top.c`), which is also true for `@poolObject` methods, extern
 * constructor names and `isAssoc && owner->makesPoolAny`; this accessor reads the raw flag, which
 * those three do not set. So a site can be promoted to the zone level and still have no argument
 * emitted -- the same shape as the home-arena bug that was fixed. Closing it means teaching both
 * sides the same question, which needs the pool-constructor name list to be shared instead of
 * spelled twice (`check_top.c` / `codegen.c`, review F11): do the two together. */
static inline bool funcTakesHomeZone(const FuncDef *f) { return f && f->makesPool; }

/* An `impl Type { fn ... }` block: methods attached to a type that is declared elsewhere
 * (or is a compiler builtin, which has no body to write them in).
 *
 * Kept as a declaration of its own until the checker has interned every type name, so that an
 * impl block may appear before the type it extends, and so that the attachment happens in one
 * place (the effect on the type is what every later pass sees -- see `checkModule`).
 *
 * Two forms, and the difference is only the `for` clause:
 *   - `impl Type { ... }`        -- inherent methods; `traitName == NULL`
 *   - `impl Trait for Type { ... }` -- the methods still join the type's **one method set**
 *     (that is what makes `x.method()` work with no extra machinery), and `traitName` records
 *     the contract the checker verifies conformance against.
 *
 * Traits are a separate namespace from types: a trait name starts with a capital letter while a
 * type name is camelCase, so the two can never collide. */
typedef struct {
    const char *typeName;   /* the type the methods attach to, e.g. `i64` or `point` */
    const char *traitName;  /* `impl Trait for Type`: the trait, or NULL for an inherent impl */
    const char *modName;    /* the module this block came from; NULL for the root file. The
                             * orphan rule compares it with the trait's and the type's. */
    Type       *target;     /* the resolved implementing type (`Self` substitutes to this) */
    TraitDef   *trait;      /* the resolved trait; NULL for an inherent impl */
    int         line;
    Vec         typeParams; /* const char*: the block's own type parameters (`impl<T> pair<T>`).
                             * The methods attach to the generic declaration's body, whose
                             * `typeParams` is what `resolveSignature` resolves against, so the
                             * names must agree with the declaration's; the checker says so where
                             * it attaches the block, which beats a puzzling "unknown type `U`". */
    Vec         traitArgs;  /* Type*: `impl Codec<i64> for X` -- the trait's arguments, parsed
                             * before `for`, next to the trait's name. */
    Vec         typeArgs;   /* Type*: the target's type arguments (`impl slice<u8>`). A generic
                             * type's method set is per instance, so the block names the instance;
                             * an unbound parameter (`impl pair<T>`) is rejected by the checker,
                             * which is where the type table can tell the two apart. */
    Vec         methods;    /* FuncDef*: `owner` is filled in when the block is attached */
} ImplDef;

/* A `trait` declaration: method **signatures** only -- no bodies, no storage, no runtime
 * existence of its own.
 *
 * It is read by two things: the conformance check (`impl Trait for T` must supply every method
 * with a matching signature), and codegen, which emits one static method table per
 * (trait, type) pair in **declaration order** -- that order is the slot order, and it must
 * never depend on insertion or hashing, or a later dynamic-link step would break silently.
 *
 * `Self` inside a trait body is an **implicit type parameter**: the trait is a template over
 * the implementing type, which is why `Self` is legal there and unknown anywhere else. */
struct TraitDef {
    const char *name;
    const char *modName;    /* the module that declares it; NULL for the root file */
    bool        usedDyn;    /* set by the checker when a `dyn` form names it: only then are the
                             * uniform method tables emitted (a trait used only statically needs no
                             * table at all, and an object-unsafe method would make one invalid C) */
    int         line;
    Vec         typeParams; /* const char*: `trait Codec<T>`. `Self` is the trait's **implicit**
                             * type parameter (TRAITS.md decision 2); these are the explicit ones,
                             * and every method's own list carries `Self` followed by these. */
    Vec         methods;    /* FuncDef*: signatures only (`body == NULL`) */
};

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*: the `type` enums */
    Vec funcs;                   /* FuncDef* */
    Vec globals;                 /* GlobalDef*: top-level let / var */
    Vec traits;                  /* TraitDef*: `trait Name { ... }` declarations */
    bool usesDyn;                /* a `dyn` form was seen: the dyn runtime and the handle type are
                                  * emitted only then, so a pool-only program stays unchanged */
    Vec impls;                   /* ImplDef*: `impl Type { ... }` blocks, attached by the
                                  * checker (each module attaches its own; the *effect* on a
                                  * type is global, which is what enforces coherence) */
    Vec uses;                    /* UseDecl* */
    /* Entries mapping a bare name back to its mangled module name (`pair` to
     * `liba$pair`); the loader fills them in and `ttResolve` looks them up. */
    Vec aliases;                 /* Alias* */
    /* `use mod::*` seen anywhere in the program, as (importer, opened) module name
     * pairs; the importer is NULL for the root file. The loader fills this in, and
     * `requireQualified` reads it: a name from an **opened** module may be written
     * bare, from anywhere in the module that opened it and nowhere else.
     *
     * A list, not a single module: two modules may be open at once, and the rule is
     * per importer. Keeping the importer in the entry is what stops an `open` in one
     * file from leaking into another -- a leak would be invisible until two files
     * happened to use the same name. */
    Vec opens;                   /* Open* */
} Module;

void moduleInit(Module *m, Arena *a);

/* A function whose first parameter is named `self` is a method. */
bool funcIsMethod(const FuncDef *f);

/* ------------------------------------------------------- walking the tree
 *
 * The one place that lists an AST node's children.
 *
 * Every question of the form "what is inside this expression" walks the same tree, and a
 * hand-written copy of that walk has to be kept in step with the AST. The copies end in
 * `default:`, so `-Wswitch` says nothing when a new kind gains children -- which is how
 * `EX_SLICE`'s two bounds were skipped by eight walkers at once, and `EX_DYN`'s payload by
 * sixteen (docs/topics/AST-WALKERS.md). Questions call the functions below instead of
 * writing their own switch; `tools/check_walkers.py` enforces that this switch covers every
 * kind, and it is the only switch it has to.
 *
 * A callback visits one **child** (never the node it was called for) and returns false to
 * stop the walk, which is how the callers keep their "exit on the first hit" behaviour. */
typedef struct {
    bool (*expr)(void *ctx, Expr *e);   /* visits a child expression; NULL to skip */
    bool (*stmt)(void *ctx, Stmt *s);   /* visits a child statement;  NULL to skip */
    void *ctx;
} AstVisit;

bool astWalkExprChildren(Expr *e, const AstVisit *v);
bool astWalkStmtChildren(Stmt *s, const AstVisit *v);

#endif /* EXTC_AST_H */
