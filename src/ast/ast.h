/**
 * @file ast.h
 * @brief The extC abstract syntax tree: the data shape the parser produces, which the type
 * checker annotates in place.
 *
 * The tree carries the results of name and type resolution, which the checker writes back
 * onto the nodes:
 *   Expr.type   the type of the expression
 *   Expr.func   the function a call resolved to
 *   Expr.field  the field a field access resolved to
 *   Stmt.type   the final type of a variable declaration
 * Code generation therefore does no type inference at all: it translates decisions that
 * have already been made.
 */
#ifndef EXTC_AST_H
#define EXTC_AST_H

#include "base.h"

/* ---------------------------------------------------------------- types */

typedef struct Type Type;
typedef struct TypeDef TypeDef;
typedef struct StructDef StructDef;
/* State of the effect summary: computed lazily and memoized; `EFF_IN_PROGRESS` is how a
 * cycle is detected. */
typedef enum {
    EFF_NONE = 0,        /* not computed yet (`arenaAllocZero` produces this value) */
    EFF_DONE,            /* computed; `effComplete` says whether it can be trusted */
    EFF_IN_PROGRESS      /* being computed -- hitting it again means a cycle, and the
                          * summary is treated as incomplete */
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
                     * type that crosses a C ABI boundary, not a closure:
                     * a lambda with captures is a different type (a capture struct + a `call`
                     * method), and only a capture-free lambda could ever be one of these. It has
                     * **no null value**, exactly like a non-nullable `ref` -- see
                     * `typeLacksZeroValue` -- and `option<fn …>` is how "may be absent" is
                     * written. */
    TY_DYN,         /* `dyn Trait`: a pool-backed `{pool, slot, gen}` name; `name` is the trait */
    TY_ANY,         /* `any`: a dynamic value. 16 bytes: a type descriptor pointer plus either
                     * the value itself (when it fits in 8 bytes and holds no references) or a
                     * pointer to a copy of it. The type it holds is known only at run time, and
                     * it is exactly one type at a time -- that invariant is what lets assignment,
                     * copying, and returning be ordinary value semantics. */
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
    /* TY_FN: `fn(A, B) -> R`, a bare code pointer.
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

/**
 * @brief Build an unresolved type name (TY_UNRESOLVED).
 *
 * @param a     Arena that owns the node.
 * @param name  The type name as written in the source; the pointer is kept as is, the text
 *              is not copied.
 * @return      A TY_UNRESOLVED type. Resolving it, together with the type arguments the
 *              parser attaches afterwards, into an interned type is the checker's job.
 */
Type *typeNamed(Arena *a, const char *name);

/**
 * @brief Build `ref inner`, the ordinary reference type.
 *
 * @param a      Arena that owns the node.
 * @param inner  The referenced type.
 * @return       A TY_REF that is neither writable nor nullable; the parser sets `mut` /
 *               `nullable` when the source writes `mut ref` / `?ref`.
 */
Type *typeRef(Arena *a, Type *inner);

/**
 * @brief Build a type parameter (TY_PARAM).
 *
 * @param a     Arena that owns the node.
 * @param name  Parameter name, such as `"T"`.
 * @param idx   Its position in the parameter list that declares it.
 * @return      A TY_PARAM type.
 */
Type *typeParam(Arena *a, const char *name, int idx);

/**
 * @brief Build a fixed-length array type `[n]elem` (TY_ARRAY).
 *
 * The length is a compile-time constant and part of the type, so `[3]i32` and `[4]i32` are
 * two different types.
 *
 * @param a     Arena that owns the node.
 * @param n     Number of elements.
 * @param elem  Element type.
 * @return      A TY_ARRAY type.
 */
Type *typeArray(Arena *a, int64_t n, Type *elem);

/**
 * @brief Build a function type `fn(params...) -> ret` (TY_FN) from the pieces the parser
 * has.
 *
 * The parameter and return types the parser hands over may still be TY_UNRESOLVED names,
 * so this only builds the shape; interning happens in `ttResolve` / `ttFn`. `ret` must not
 * be NULL.
 *
 * @param a       Arena that owns the node and the parameter vector.
 * @param params  Parameter types, in order (`Type *` elements); the vector is copied.
 * @param ret     Return type.
 * @return        A TY_FN type whose parts may still be unresolved names.
 */
Type *typeFn(Arena *a, Vec *params, Type *ret);

/* ------------------------------------------------------------ expressions */

/* Sentinel for "allocate into this function's home arena" -- the arena the caller
 * chose and passed down. It means the same thing as the -1 that `homeDepth` and
 * `arenaArg` carry. Those fields are all filled in by the checker and code
 * generation only translates them: it must never re-derive the answer from "does
 * this function have a home arena", because that would be a second source of truth
 * for a decision that is already made. */
#define ARENA_HOME (-1)

/**
 * @brief The deepest block level reachable inside one statement.
 *
 * A block counts as one level, and the loops, conditionals, and matches inside it each add
 * their own, so the answer is the number of arena slots the enclosing function needs. It is
 * the length of the stack array `__extc_a`, so setting up the block arenas needs no
 * allocation (the block is defined by code generation, which the checker uses to size one
 * block arena per level of a coroutine frame).
 *
 * @param s  Statement to inspect.
 * @return   The deepest level; 0 when there is no block.
 */
int blkMaxLevel(Stmt *s);

/**
 * @brief The deepest block level reachable inside one block statement.
 *
 * @param block  A block statement; for a non-block it degrades to the same question asked
 *               about that single statement.
 * @return       The deepest level inside the block, **not** counting the level the block
 *               itself adds; 0 when there is no block.
 */
int blkMaxOfBlock(Stmt *block);
/* Sentinel for "the home zone". Parallel to ARENA_HOME:
 *   0         = this call site builds no pool (the default value of `zoneLevel`)
 *   ZONE_HOME = hand it to wherever the **caller** chose (the hidden parameter
 *               `__extc_home_zone` is passed down)
 *   k >= 1    = build it in the place at level k */
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
    EX_EXT,         /* `ext f(x)`: start one concurrent task (the `ext` sugar, i.e. "`ext` and the scheduling domain"). It is an **expression**: it evaluates to a handle, so
                     * `let h = ext f(x)` is the normal spelling. Legal only inside a domain --
                     * that rule and the spawn itself are the checker's job; the parser only
                     * builds the node. */
    EX_LAMBDA,      /* `fn(x: i64) -> i64 { ... }`: a closure literal. The checker gives it a
                     * generated unique type (a capture struct + a `call` method), so a lambda is
                     * an ordinary value of an ordinary type. */
} ExprKind;

/* One entry of a lambda's capture list, `[mut ref hits]`.
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
    /* Three builtin forms carry no callee by the time code generation runs, so the checker
     * marks the **call node** instead and codegen keys on the marker: the worker of
     * `parallel::run(worker, ...)` (for the trampoline), `sys::domain::single()` (an assoc
     * builtin), and `std::sys::heap::extc_viewOf(p, n)` (the plate layer's one primitive,
     * whose declaration has `@builtin` and therefore no body). The markers live in the plan
     * side table: `planParWorker`, `planDomNew`, `planViewOf` in plan.h. */
    /* Three more resolutions the checker records on the node and codegen reads: the domain an
     * `EX_EXT` task is handed to, the field an `EX_FIELD` resolved to, and the instance type an
     * `EX_ASSOC` resolved to (kept so the C name can be mangled). Storage is the plan side
     * table: `planExtDom`, `planField`, `planAssocOwner` in plan.h. */
    /* Those in the analysis side: the block level a borrow must outlive, whether the value
     * may be reused at the same site, whether it is borrowed, whether the source wrote a
     * qualified name, whether a narrowing conversion gets a runtime check, and whether a
     * comparison over a type parameter still has to be resolved at instantiation (that last
     * one is read by codegen: `planNeedOp`). See `planAnalysisFields` in plan.c. */
    /* `deref` and `boxedCoro` are two more facts code generation reads: "the expression sits
     * in value position while its type is `ref T`, so emit `*(...)`" (the checker treats a
     * `ref T` as the `T` itself and adds the dereference back once, where the value is
     * needed), and "this coroutine call is stored as a handle, so box it". Storage is the
     * plan side table: `planDeref` / `planBoxedCoro` in plan.h. */
    /* For the escape check: how deep does the storage live that the references in
     * this expression point at?
     *   0  = a parameter, static data, or unknown; a reference returned by a callee is
     *        covered by that callee's own return check
     *   >0 = the block depth of some local
     * Only meaningful when the type contains a reference. */
    /* The **compilation plan** of this node (arena/zone level, argument arena, whether a
     * temporary is needed) does not hang off this AST: its storage and its accessors live in
     * the plan side table, which is addressed by node.
     * That leaves the AST holding syntax facts and identity only, while "when a plan fact is
     * valid, who writes it, who reads it" is spelled out field by field in the plan header. */

    /* The lexical block level of this site, for long-running programs.
     * The final pass overwrites `arenaLevel` with ARENA_HOME for every allocation in
     * a function that has a home arena, which erases the level the site really needs.
     * Once the constraints have been solved, a site that nothing pulled out of the
     * frame goes back to its own block, which is the level recorded here. */
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
    /* The shallowest level at which this value has been observed being published (stored
     * into a place, returned, or handed to a call that may keep it). `-1` means the
     * question has not been asked yet. Recording is the only thing the checking pass
     * does about levels: deciding which arena a site goes to is a separate pass over
     * these records, so that no decision depends on the order the checker happens to
     * walk the tree in. */
    /* This `new` belongs to an `@overwrite` site, so there is only one block of
     * storage: it is allocated lazily in the function frame and cleared before every
     * reuse. The level therefore follows the function body rather than the block the
     * statement sits in. */
    /* Are the references inside this value borrowed from outside this call, that is,
     * do they come from a parameter or from a call that returned them? A borrowed
     * value may not be stored anywhere that outlives this call, because the compiler
     * does not know how long it really lives: it may point into a local of the
     * caller's frame that dies before the destination does. */
    /* Which arena should this call pass to a callee that has a home arena? The answer
     * follows the shallowest `mut ref` argument: newly allocated objects live as long
     * as the storage that argument points into.
     *
     * This field is for the checker itself and is deliberately conservative -- a site
     * at level 0 is looked up as depth 0, so it may reject a program that is in fact
     * safe. Code generation does not read it: it reads `arenaArg`, which is already
     * resolved. */
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
    /* Explicit conversion: is a runtime check needed? Integer narrowing, a sign
     * change, and float to integer may all lose the value, while a conversion whose
     * result provably fits is not checked. What the compiler can prove at compile
     * time leaves no trace at runtime. */

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
                 /* The binding itself, as resolved: a `Sym *` declared in
                  * check_internal.h, opaque because the AST does not know the checker's types.
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
                                             * through `IdentBinding` (check_internal.h):
                                             * the AST does not know the checker's types. */
        struct { const char *op; Expr *left, *right; } bin;
        struct { const char *op; Expr *operand; } un;
        struct { Expr *callee; Vec args; } call;          /* args: Expr* */
        struct { Expr *payload; const char *traitName; Type *payloadType; } dynv;  /* EX_DYN */
        /* `fn(params) -> ret [mut ref x, ...] { body }` (EX_LAMBDA). `captures` holds the
         * **written** entries only (the writable ones); the checker records what it actually
         * captured in `fields`, in declaration order, and builds `sdef` (the capture struct)
         * plus a `call` method on it. `tname` is that struct's generated name, which diagnostics
         * show as `fn(A) -> R`. */
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
         * `typeName` is NULL, because a function type is not a name. */
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
    /* `dyn Trait(x).m(...)`: construction and the call stay in one
     * expression, so the trait being dispatched through is a property of the call node rather
     * than a type of its own. The parser sets it and refuses to let it be stored; codegen reads
     * it to dispatch through the table instead of calling the implementation directly. */
    const char *dynTrait;
    /* `r.tag()` where `r: ref dyn Tag`: the checker unwraps the reference to find the trait, which
     * the node's type does not show. The C expression is then a **pointer** to
     * the handle while `extc_dyn_slot` takes the handle by value, so codegen has to dereference --
     * this flag is what tells it. */
    /* `f(x)` where `f` is a **value** of function type and not a declaration: the call goes through
     * the code pointer, so there is no `FuncDef` for codegen to name.
     *
     * The checker sets this once it has matched every argument against the signature the **type**
     * carries. Without the flag, codegen's call path would
     * look for `e->func`, find NULL, and drop the call on the floor -- `if (!e->func) return "0"`
     * turns a missing call into a valid-looking product, which is worse than invalid C. */
};

typedef struct { const char *name; Expr *value; } FieldInit;

/**
 * @brief Build an expression node.
 *
 * The node comes from the arena already zeroed; only `kind` and the line number are set, so
 * a field the parser does not fill is zero rather than indeterminate. The results of
 * resolution (`type`, `func`, `field`, and the escape-analysis depth) are filled in later by
 * the checker.
 *
 * @param a     Arena that owns the node.
 * @param kind  Expression kind; only the corresponding union member is usable.
 * @param line  Source line number (1-based), kept for diagnostics.
 * @return      The zeroed Expr.
 */
Expr *exprNew(Arena *a, ExprKind kind, int line);

/**
 * @brief Build an `EX_IDENT`: the only place this node is constructed.
 *
 * `name` and `srcName` are set together: the identifier records what was written, and
 * renaming happens after the parser.
 *
 * @param a     Arena that owns the node.
 * @param name  The name as written.
 * @param line  Source line number (1-based).
 * @return      The zeroed identifier node with both names filled in.
 */
Expr *exprIdent(Arena *a, const char *name, int line);

/**
 * @brief Build a binary operation node with `op` and both operands filled in at once.
 *
 * @param a      Arena that owns the node.
 * @param op     Operator text (`"+"`, `"<"`, `"-"`, ...).
 * @param left   Left operand.
 * @param right  Right operand.
 * @param line   Source line number (1-based).
 * @return       The new `EX_BIN` node.
 */
Expr *exprBinNew(Arena *a, const char *op, Expr *left, Expr *right, int line);

/**
 * @brief Build an integer literal node.
 *
 * @param a      Arena that owns the node.
 * @param value  Literal value.
 * @param line   Source line number (1-based).
 * @return       The new `EX_INT` node; `type` is left for the caller to fill.
 */
Expr *exprIntNew(Arena *a, long long value, int line);

/**
 * @brief Build a boolean literal node (`true` / `false`).
 *
 * @param a      Arena that owns the node.
 * @param value  The literal's value.
 * @param line   Source line number (1-based).
 * @return       The new `EX_BOOL` node.
 */
Expr *exprBoolNew(Arena *a, bool value, int line);
/**
 * @brief Build a floating-point literal node.
 *
 * @param a      Arena that owns the node.
 * @param value  The literal's value (`f64`).
 * @param line   Line number.
 * @return       An `EX_FLOAT` node whose type the checker picks from context.
 */
Expr *exprFloatNew (Arena *a, double value, int line);

/**
 * @brief Build an `EX_CALL` node with the callee filled in and the argument list empty.
 *
 * @param a       Arena that owns the node.
 * @param callee  Callee expression.
 * @param line    Source line number (1-based).
 * @return        The new `EX_CALL` node.
 */
Expr *exprCallNew(Arena *a, Expr *callee, int line);

/**
 * @brief Replace the index of an index expression in place (used by lowering).
 *
 * @param e      Must be an `EX_INDEX` node.
 * @param index  The new index expression.
 */
void exprIndexRetarget(Expr *e, Expr *index);

/**
 * @brief Record the name of the module a `FuncDef` belongs to (for functions the checker
 * synthesizes).
 *
 * @param f        Function definition to write.
 * @param modName  Module name; may be NULL.
 */
void funcDefSetModName(FuncDef *f, const char *modName);

/**
 * @brief Set a statement's "parallel loop block" flag.
 *
 * @param s        Statement node to write.
 * @param parLoop  The new value.
 */
void stmtSetParLoop(Stmt *s, bool parLoop);
/**
 * @brief Build a method call node `recv.name(args...)` with an empty argument list, for the
 * caller to fill.
 *
 * @param a     Arena to allocate from.
 * @param recv  Receiver expression.
 * @param name  Method name (statically stored).
 * @param line  Source line number.
 * @return      The new node.
 */
Expr *exprMethodNew(Arena *a, Expr *recv, const char *name, int line);

/**
 * @brief Build a slice node `obj[lo..hi]`; `lo` / `hi` may be `NULL` (that end was not
 * written).
 *
 * @param a     Arena to allocate from.
 * @param obj   Expression being sliced.
 * @param lo    Lower bound, or `NULL`.
 * @param hi    Upper bound, or `NULL`.
 * @param line  Source line number.
 * @return      The new node.
 */
Expr *exprSliceNew(Arena *a, Expr *obj, Expr *lo, Expr *hi, int line);

/**
 * @brief Rewrite an expression in place into a module function call
 * `modPrefix::name(args...)`.
 *
 * Used when the checker redirects a reserved-name call the parser built to a concrete module
 * function according to the static type (the specifier slot of a value f-string becomes
 * `stl::stringio::padI64` / `padF64` / `padText`).
 *
 * @param a          Arena to allocate from.
 * @param e          Expression node to rewrite (in place).
 * @param typeName   The type-name segment.
 * @param modPrefix  Module prefix.
 * @param name       Function name (statically stored).
 * @param args       The new argument list; on failure the node is left as it was.
 */
void exprRetargetAssoc(Arena *a, Expr *e, const char *typeName, const char *modPrefix,
                       const char *name, Vec *args);

/**
 * @brief Deep-copy an expression subtree (used by fusion lowering: one right-hand side has
 * to appear in several places).
 *
 * Only the expression shape and the source location are copied; `type` and the plan side
 * tables are cleared in the copy and filled in again by the checker -- the copy happens
 * before checking, so the source nodes have not been checked either. The node shapes allowed
 * are pure expressions (literals, identifiers, binary/unary, conversions, fields, indexes,
 * slices); anything else (calls, `new`, closures, ...) is shallow-copied as is, and the
 * caller must not apply fusion to them.
 *
 * @param a    Arena to allocate from.
 * @param src  Source expression; NULL returns NULL.
 * @return     The new subtree.
 */
Expr *exprClone(Arena *a, Expr *src);


/**
 * @brief Replace the statement list of a block statement (used by fusion lowering).
 *
 * @param s      Must be an `ST_BLOCK` node.
 * @param stmts  The new statement list (a `Vec` of `Stmt*`).
 */
void stmtBlockSetStmts(Stmt *s, Vec *stmts);
/**
 * @brief Build an `obj[index]` index node (used by lowering of whole-array expressions).
 *
 * @param a      Arena to allocate from.
 * @param obj    Expression being indexed (usually a view name).
 * @param index  Index expression.
 * @param line   Source line attached to the node.
 * @return       The new `EX_INDEX` node.
 */
Expr *exprIndexNew(Arena *a, Expr *obj, Expr *index, int line);
/**
 * @brief Build an `obj.name` field access node (used by lowering to read `v.len`).
 *
 * @param a     Arena to allocate from.
 * @param obj   Expression being accessed.
 * @param name  Field name (the text as written in the source; not copied).
 * @param line  Source line attached to the node.
 * @return      The new `EX_FIELD` node.
 */
Expr *exprFieldNew(Arena *a, Expr *obj, const char *name, int line);
/**
 * @brief Build a string literal node, copying the text into the arena.
 *
 * A trap message built by lowering must outlive this module's arena: the token text in the
 * source may live in a temporary buffer, so the text is copied by value here.
 *
 * @param a     Arena to allocate from.
 * @param text  String contents (UTF-8, unescaped).
 * @param line  Source line attached to the node.
 * @return      The new `EX_STR` node.
 */
Expr *exprStrNew(Arena *a, const char *text, int line);
/**
 * @brief Build a unary expression node.
 *
 * @param a        Arena to allocate from.
 * @param op       Operator text, such as `"-"`; must be a static string that needs no extra
 *                 storage.
 * @param operand  Operand.
 * @param line     Source line attached to the node.
 * @return         The new `EX_UN` node.
 */
Expr *exprUnNew(Arena *a, const char *op, Expr *operand, int line);

/**
 * @brief Build `e!`: the author's "I know it has a value" signature node (the payload's type
 * is set by the checker).
 *
 * The compiler itself uses it to take the value out when it expands `x ?? break`: the guard
 * has already proved a value is there, so this signature cannot trap.
 *
 * @param a        Arena.
 * @param operand  Expression being signed (`option<T>` / `result<T,E>` / `?ref T`).
 * @param line     Line number.
 * @return         The new signature expression.
 */
Expr *exprSignNew(Arena *a, Expr *operand, int line);
/**
 * @brief Build an assignment statement `target op= value` (used by lowering to build a loop
 * step and element-wise stores).
 *
 * @param a       Arena to allocate from.
 * @param target  Assignment target.
 * @param value   Right-hand side.
 * @param op      Compound assignment operator (such as `"+="`); NULL for a plain `=`.
 * @param line    Source line attached to the node.
 * @return        The new `ST_ASSIGN` node.
 */
Stmt *stmtAssignNew(Arena *a, Expr *target, Expr *value, const char *op, int line);
/**
 * @brief Build a local variable declaration (`let` / `var x: ann = init`) for temporaries the
 * compiler creates itself.
 *
 * @param a     Arena to allocate from.
 * @param name  Variable name; must live at least as long as this module.
 * @param ann   Type annotation; may be NULL (inferred from the initializer).
 * @param init  Initializer; may be NULL.
 * @param mut   true = `var`, false = `let`.
 * @param line  Source line.
 * @return      The new `ST_VAR` node.
 */
Stmt *stmtVarNew(Arena *a, const char *name, Type *ann, Expr *init, bool mut, int line);
/**
 * @brief Replace an assignment's right-hand side and operator (used by lowering rewrites).
 *
 * A compound assignment (`dst += rhs`) is first rewritten to `dst = dst <op> rhs` and then
 * handed to the same lowering, and the rewrite touches only these two places. The node
 * itself is kept, so its position and the fields the plan table already recorded need no
 * update.
 *
 * @param s      The assignment statement (`ST_ASSIGN`).
 * @param value  The new right-hand side.
 * @param op     The new compound operator; NULL for a plain `=`.
 */
void stmtAssignSetValue(Stmt *s, Expr *value, const char *op);
/**
 * @brief Build a `trap(msg)` statement.
 *
 * @param a     Arena to allocate from.
 * @param msg   Message expression (usually a string literal built by `exprStrNew`).
 * @param line  Source line attached to the node.
 * @return      The new `ST_TRAP` node.
 */
Stmt *stmtTrapNew(Arena *a, Expr *msg, int line);
/**
 * @brief Build an `if cond { thenBody } else { elseBody }` statement; `elseBody` may be NULL.
 *
 * @param a         Arena to allocate from.
 * @param cond      Condition expression.
 * @param thenBody  Statements run when the condition holds.
 * @param elseBody  Statements run when it does not; NULL means there is no `else`.
 * @param line      Source line attached to the node.
 * @return          The new `ST_IF` node.
 */
Stmt *stmtIfNew(Arena *a, Expr *cond, Stmt *thenBody, Stmt *elseBody, int line);
/**
 * @brief Build the desugared block of a `for var in 0..hi { body }` (used by lowering of
 * whole-array expressions).
 *
 * The shape is exactly the parser's range `for`: `[loop variable declaration, upper bound
 * declaration, while]`, where the `while` body is `[body, step]` with `forStep` attached --
 * the checker's range widening and lowering both recognise this shape.
 *
 * @param a     Arena to allocate from.
 * @param var   Loop variable name (the caller guarantees an internal name that cannot
 *              collide with a user name).
 * @param hi    Upper bound expression (half-open range).
 * @param body  Loop body (a block).
 * @param line  Source line attached to the node.
 * @return      The outer block statement.
 */
Stmt *stmtRangeForNew(Arena *a, const char *var, Expr *hi, Stmt *body, int line);



/* ------------------------------------------------------------ statements */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK, ST_MATCH,
    ST_YIELD,               /* `yield e`: only inside a `-> coroutine<T>` body */
    ST_TRAP,                /* `trap(msg)`: the program cannot continue. **Not an exception**: there
                             * is no handler and no unwinding -- the process writes
                             * `file:line: trap: msg` to stderr and stops with code 1, down the same
                             * road the compiler's own checks take (bounds, division by zero), dying
                             * hook included. Expected failures are `result`/`option` values. */
    ST_DROP,                /* `drop x`: run `x`'s `@drop` hook **now** and mark the place dead. A
                             * statement, not an expression: it has no value, and a later use of `x`
                             * is a compile error rather than a second drop. */
    ST_DOMAIN               /* `d.run { … }`: a **domain block**. A statement on purpose: the block
                             * is not a value and cannot be used as an expression, the expression
                             * walkers never have to see it, and the statement walkers handle its
                             * body the same way they handle any other block. */
} StmtKind;

/* One arm of a `match`: `circle => { ... }`.
 *
 * A variant that carries a payload also carries a list of names to bind it to. */
typedef struct {
    const char *variant;   /* the variant name; for the catch-all arm it is "_" */
    bool        wildcard;  /* `_ => ...`: matches every variant the arms above it did not.
                            * It must be the last arm and binds no payload -- one name cannot
                            * have the type of every variant's payload. `if let` desugars to
                            * a match whose second arm is exactly this one. */
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

    /* Set on the block that a `par i in lo..hi { … }` was desugared into: the parser builds the
     * same shape as a range `for` and marks the outer block; the checker then lowers it to a call
     * to a synthesized worker (see `parLower` in check_stmt.c). The flag is what keeps the parser
     * from needing types and the language from needing a new statement kind. */
    bool     parLoop;

    Type    *type;      /* ST_VAR: the final type of the declaration, filled in by
                         * the checker */

    /* `for` desugars to a `while` whose body ends with the loop's step statement (`parseFor`).
     * This points at that step from the **body block**, and codegen emits a label right before
     * it so `continue` can jump there: C's `for` runs the step on the way out of a `continue`,
     * and without this the desugared form skipped it -- an infinite loop for any `for` that
     * contains a `continue`. It is cleared when `forRetargetToIterator` drops the
     * step (the iterator's own `next()` in the condition advances instead, so a plain
     * `continue` is right again). */
    Stmt    *forStep;


    union {
        struct { const char *name; Type *ann; Expr *init; bool mut;
                 /* The name used in the generated C; a `let` that shadows another one
                  * in the same block becomes `a__2`. Filled in by the checker. */
                 /* `@overwrite var n = new T` reuses a single block of storage: it is
                  * allocated once in the function frame, lazily on first use, and
                  * cleared before every later execution of this statement. Legal only
                  * for `new`, which the checker enforces. */
                 bool overwrite;
                 /* `global var x: T` / `global let x: T = c`: the **storage** lives outside every
                  * frame (a C block-scope `static`), while the **name** stays in this block.
                  * Depth 0 ⇒ a view of it may cross a `yield`; */
                 bool isGlobal; } var;
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
        struct { Expr *target; } drop_;    /* ST_DROP: the place whose hook runs now */
        /* ST_DOMAIN: `callee` is the `d.run` that opened the block, `body` is the block itself.
         * Only a value of the language-level object `domain` may carry one (check_stmt.c). */
        struct { Expr *callee; Stmt *body; } domain_;
        struct { Vec stmts; } block;                      /* stmts: Stmt* */
        struct { Expr *scrutinee; Vec arms; } match;      /* arms: MatchArm* */
    } u;
};

/**
 * @brief Build a statement node.
 *
 * The node comes from the arena already zeroed; only `kind` and the line number are set.
 *
 * @param a     Arena that owns the node.
 * @param kind  Statement kind; only the corresponding union member is usable.
 * @param line  Source line number (1-based), kept for diagnostics.
 * @return      The zeroed Stmt.
 */
Stmt *stmtNew(Arena *a, StmtKind kind, int line);

/* -------------------------------------------------------------- top level */

typedef struct {
    const char *name;
    /* The name this parameter is emitted under. **A plain field on purpose**: a `Param`
     * is stored by value (the coroutine frame copies them), and the plan side table is
     * addressed by node pointer -- a copy would have no slot, so a name kept there is
     * lost for every coroutine entry function. */
    const char *cname;
    Type       *type;
    /* The value a call uses when it leaves this argument out (`t: bool = false`), or NULL.
     * **Only a compile-time constant is allowed**, and the checker copies it into every call
     * site that omits the argument: the value cannot read the callee's locals, so copying is
     * the only shape that works. A parameter without a default may not follow one that has
     * one, which keeps "the arguments left out" a suffix of the parameter list. */
    Expr       *dflt;
    int         line;
} Param;

/**
 * @brief Build a second struct definition that shares `src`'s fields and methods under another name.
 *
 * A lambda environment written inside a generic body is laid out once per instance of that body:
 * the field types are printed through the instance's substitution, so the fields -- and the `call`
 * method the checker synthesized over them -- are shared, while the C name has to differ. Only the
 * names are written here; every other member is the template's.
 *
 * @param a     Arena to allocate from.
 * @param src   The struct to copy.
 * @param name  The C name, also used as the source name, of the copy.
 * @return      The copy.
 */
StructDef *structDefNamedCopy(Arena *a, const StructDef *src, const char *name);

/**
 * @brief Build a parameter that carries `src`'s identity but another type.
 *
 * A per-instance layout of a lambda's `call` method needs its `self` parameter typed as that
 * layout's own type (the checker typed it as the template environment). Name, emitted name and line
 * are the template's, so diagnostics still point at the lambda.
 *
 * @param a     Arena to allocate from.
 * @param src   The parameter to copy.
 * @param type  The type of the copy.
 * @return      The copy.
 */
Param *paramWithType(Arena *a, const Param *src, Type *type);

struct FieldDef {
    const char *name;
    Type       *type;
    int         line;
    /* `@private`: reachable only from the module that declares it. Storage is the reason this
     * exists as much as behaviour is -- a public `buf`/`vals` hands the reader a view into pool
     * storage, which is exactly what the pool tier forbids. */
    bool        isPrivate;
    /* `effects Addr=… Cont=…` on a **field of function-pointer type**: the signature of the function
     * that slot will hold.
     *
     * It exists for the table: the callee is not a declaration -- the host fills
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
     * leaks the compiler's internal encoding for a word the user never wrote. For a
     * single-file program the two are the same string. */
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
     * coroutine evaluates to this type, and codegen derives `$frame` / `$step` from the
 * coroutine function it belongs to (`planCoroOf` in plan.h)
     *. */
    /* Synthetic holder for a **builtin scalar** that an `impl` block attached methods to.
     * It is pushed into the module's struct list so that every "struct x method" pass
     * (signature resolution, body checks, escape/borrow rules, operator collection, code
     * generation) reaches those methods with no pass changed at all, but it is NOT interned
     * under its name (so `i64` still resolves to the builtin, never to a struct) and code
     * generation never emits a C struct for it. */
    /* `@noCopy`: this type may not be copied by value -- it may only be passed as
     * `ref` / `mut ref`. A type whose state has an **identity** is the reason: copying a
     * `reader` (or an `ifstream`) gives two objects over one buffer with two independent
     * positions, and reads then interleave in a way neither name shows. Every language
     * that has such types says so one way or another -- C++ deletes the copy constructor,
     * Rust moves and borrows, Go passes a pointer, Python has no implicit object copy;
     * extC writes it down as an annotation. */
    bool        noCopy;
    /* The `@drop` method, or NULL. It runs when a binding of this type leaves its block, in the
     * same spirit as a destructor: `@noCopy` promises a single owner, so "the owner is going away"
     * is exactly the moment the state can be handed back (a pool slot returned, a buffer
     * released). Only `@noCopy` types may carry one -- with two owners there is no single moment
     * to run it. */
    FuncDef    *dropHook;
    /* `@value`: a value type that owns storage, so a copy must go through its `clone` method
     * instead of copying the fields. The difference from `@noCopy` is that a copy is allowed at
     * all: `@value` promises `clone` exists and produces an independent value. A temporary is
     * still moved, never cloned (`@value` only changes what a copy from a *place* does). */
    FuncDef    *cloneHook;
    bool        valueCopy;
    /* `@frozen`: the author signs -- **this type's layout is C's layout**: fields in
     * declaration order, with C's alignment and padding, and no hidden fields. It is the
     * **ticket** for crossing a C boundary by value (`ttCrossesC` accepts only structs
     * carrying this marker).
     *
     * It is a signature, the same kind as `effects`: the compiler does **not** inspect what
     * is inside. Putting a `slice<T>`, an enum, or a generic parameter in it is the author's
     * call -- the price is that the C side has to **copy the shape we emit**, and a mismatch
     * is the author's debt (same rule: special powers are not handed out freely, but the
     * author is not second-guessed either).
     *
     * The other half is what the compiler owes under this signature, and it is hard:
     *   - **never reorder fields, never add hidden fields** -- a future feature that wants to
     *     put a discriminant or a generation number into a struct has to go elsewhere;
     *   - pin this in the product with two **platform-independent** static assertions
     *     (adjacent fields' `offsetof` increases; `sizeof` does not exceed "last field + its
     *     alignment"), so any silent drift makes the product **fail to compile** instead of
     *     making some call site read a wrong value. */
    bool        frozen;
    /* `@sharesStorage`: copying this type by value makes **both copies share one block of
     * storage** (the container family is the example).
     *
     * Not forbidden, but **warned about**: some uses (handing a handle out, brief sharing)
     * are legitimate, and forbidding them would close off the road. But the user has to
     * **know** what that line does, because it contradicts the intuition that "every copy of
     * a value is its own": after `var b = a`, `b.push(..)` writes into `a`, and `a.release()`
     * leaves `b` dangling (the guard turns that into a trap with a location, but by then it
     * is already an incident).
     * Division of labour with `@noCopy`: `noCopy` means "state with an identity, copying is
     * always wrong" (readers, streams); `sharesStorage` means "the semantics of copying
     * differ from intuition, but you may mean it". */
    bool        sharesStorage;
    /* `@poolObject`: this type **owns a pool** (its storage lives on the pool's own slabs
     * and lives as long as the place the pool object resides in).
     *
     * A declaration beats inference: the compiler does not have to guess ("does it have a
     * `pid` field" is a magic number, "do its methods build a pool" waits for the closure),
     * one modifier states the fact, and **user-written containers declare with the same
     * modifier** and take exactly the same path.
     *
     * With it the compiler can do three things deterministically: (1) a constructor call of
     * this type is a **pool site** (promotable); (2) a function that builds a pool takes the
     * hidden "home zone" parameter; (3) if a future rule wants to decide "elements may not
     * carry references" per type, this is the criterion too, not any naming convention. */
    bool        poolObject;
    /* `@buffer`: this type is the **owning form** of a `new` buffer for its single type parameter.
     * `let b: buf<T> = new T[n]` then builds the type through its one-argument constructor instead
     * of producing a `slice<T>` view, which is what makes "a buffer that owns its storage" spellable
     * without the compiler knowing any type by name. The element type must agree with the type
     * argument, and `new T[n]` is only a view when the context asks for one. */
    bool        buffer;
    /* **Some method** of this struct builds a pool (computed along with the closure; see
     * `FuncDef.makesPool`). Why it is needed: a generic method's **template** and
     * **instance** are two FuncDefs, the closure marks only the template, and at a call site
     * `e->func` may be the template, so a reader looking at `f->makesPool` alone sees a
     * false: the instance copy was not marked while the product side is true. Relaxed only
     * for **associated functions** (the constructor family) so other methods are unaffected. */
    /* Which file it came from, which module, and whether it is `@private`. */
    Ctx        *ctx;
    const char *modName;
    bool        isPrivate;
    int         line;
    /* The name shown to the user (`io::reader`), while `name` is the internal mangled
     * form (`io$reader`). The two must stay apart: printing `io$reader` in a diagnostic
     * leaks the compiler's internal encoding for a word the user never wrote. For a
     * single-file program the two are the same string. */
    const char *srcName;
    /* A lambda's environment struct: the signature it was written as, `fn(i64) -> i64`, so a
     * diagnostic shows what the user wrote instead of the generated `$lam$f$3`. NULL otherwise
     *. */
};

struct FuncDef {
    const char *name;
    /* The type parameters of a generic free function, `fn f<T, U>(...)`. A method
     * keeps its type parameters in `owner->typeParams`, so this field is used by free
     * functions only. An instance holds the instance's type arguments in `targs`, takes
     * its C name from `instName`, and points back at its template through the plan (see
     * `planTemplate` in plan.h) rather than through a field here. */
    Vec         typeParams;      /* const char* */
    Vec         targs;           /* Type*: only an instance has these */
    /* The file this body was written in. Every diagnostic the generated code can raise at run
     * time (`trap`, a bounds check) prints a `file:line` pair; the line comes from the checked
     * node, so the file has to travel with the function -- otherwise a check emitted inside a
     * library helper names the entry file of the program that happened to include it. */
    const char *srcPath;
    Vec         params;          /* Param* */
    /* The binding each parameter resolved to, in declaration order. `void *` because `Sym` is
     * defined in the checker's own header, which ast.h does not see; only the checker reads this.
     *
     * The effect summary is computed after every body has been checked, when **no scope exists**,
     * so "which parameter does this value come from" cannot be answered by a lookup. Matching by
     * name is wrong the moment a local shadows a parameter (legal, documented): the store is
     * attributed to the parameter of that name, and the call site then checks the wrong argument.
     * Identity has to be recorded while the scope is still there (INV-I). */
    Type       *ret;             /* NULL when the function returns nothing */
    /* Some `ext f(x)` starts this coroutine as a task. The stable adapter that the domain's task
     * table needs -- `bool (*)(void *)` while `$next` takes a concrete frame pointer, and calling one
     * through the other's type is undefined -- is emitted **only** for these, so a program that never
     * starts a task carries nothing extra. */
    /* A lambda's `call` method: its body was already checked **in place**, in the scope the lambda
     * was written in (that is where the capture set comes from), so the pass that checks struct
     * methods must not check it a second time. */
    /* A lambda whose result type was left out: the first `return <value>` decides it, and this flag
     * is what lets that return through before the type is known (check_stmt.c, ST_RETURN). */
    Type       *coroRetProto;
    /* Named from **outside** its instance: a `dyn` table's thunk calls it, so its body must survive
     * the "definitions nothing names" pass (which cannot see that reference). */
    bool        dynTable;
    /* "a handle of it was made somewhere" (which makes it get a task even when it is never
     * spawned) is a plan fact: see `planCoroBoxed` in plan.h. */
    /* The locals that live across a `yield`, laid out by the checker for codegen (slice B):
     * `pc`, the return slot and (when `makesPool`) the zone id come first, then these by value.
     *4. */
    Vec         coroFrame;
    Vec         callees;      /* FuncDef*: callees named by the body (split planned) */
    Stmt       *body;            /* ST_BLOCK */
    StructDef  *owner;           /* the struct a method belongs to; NULL for a free function */
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
     * met must not quietly do nothing. */
    bool        isInline;
    /* `@unchecked fn f(...)`: **the bounds checks on the index expressions inside this body
     * are not emitted.**
     *
     * The promise the language makes everywhere else -- out-of-range means a located `trap:`,
     * never undefined behaviour -- is deliberately given up here, and only here, by writing
     * it out. That puts the annotation in the same category as `extern!` and `!`: the
     * programmer signs for a fact the compiler cannot check, and the manual says what the
     * signature costs.
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
    /* `@builtin`: this declaration is implemented by the **compiler** (it has no body); the
     * library provides only discoverability and documentation.
     * The reason is the same as for `alloc<T>` / `poolSlice<T>`: the code it generates (the
     * parallel worker's trampoline) must have the function named by the compiler, which the
     * library cannot provide. So far only `std::parallel::run`. */
    bool        isBuiltin;
    /* `@export`: **this function's C symbol is for outside use** -- in the product it is not
     * `static`, and its C name is the one written in the source (the part before `$`, see
     * codegen's `cFuncName`), so the C side can find it with `dlsym(h, "init")`. This is the
     * other side of `extern!`: that one says "I call others", this one says "others call me".
     *
     * Three accompanying hard rules, each reported where it is implemented:
     *   - **the signature must cross the C boundary** (reusing `ttCrossesC` -- the same
     *     measure as for `extern!`);
     *   - **no hidden parameters** (a function taking a home arena / zone has an extra C
     *     parameter the caller cannot see);
     *   - **it must not be pruned by reachability** (nothing on the extC side calls it, and
     *     pruning only looks at whether a name is mentioned).
     * Generics, methods, coroutines, and `extern!` cannot be exported: they have no "that one
     * C symbol". */
    bool        isExport;
    /* `@module`: **this free function is an entry point of this module** -- the C `main` in
     * the product is a dispatcher: `./a -m <module name> args...` selects it and
     * `./a --help` lists the names and descriptions.
     *
     * One binary, one runtime, no dlopen and no cross-module ABI: a module entry is another
     * `int (int, char **)` function in the same translation unit. The rules match `main`
     * (return `i32` or nothing, at most one parameter of type `slice<slice<u8>>`), because
     * the dispatcher passes the command line through unchanged.
     *
     * `moduleEntryName` is the name shown by `--help` (the module name when not written;
     * `main` for an `@module` in the entry file); `moduleDesc` is the optional one-line
     * description. All three affect only the dispatcher and the help text. */
    bool        isModuleEntry;
    const char *moduleEntryName;
    const char *moduleDesc;
    /* A worker that `parallel::run` used once: codegen has to emit a static trampoline for
     * it (the language layer has no function pointers, so this "take the address" can only be
     * written by the compiler). */
    /* The `new` in this function (a worker, or a helper on its call chain) must use a
     * **thread-local** arena: the worker's signature is fixed (it writes only `out` and
     * returns only i64), so its allocations cannot escape, which makes thread-local safe; the
     * frame arena belongs to the main thread, and two workers using it at once is a data race,
     * so it must be swapped. */
    const char *externLib;       /* the name in `extern!("libc")`, used in diagnostics */
    bool        hasEffects;
    unsigned    extAddrMask, extContMask;
    /* `effects Ret=0` on the declaration: "the reference/view I return does not point into the
     * caller's frames" (the return side of `Addr`). Believed for an `extern!` (a black box) and
     * for an extC function as well, because a library author writes it where the memory's
     * lifetime is known -- `std::heap`'s `alloc`/`view`/`allocPtr` are the first users: the plate
     * region is `mmap`ed and lives to `close()`, so its views are `Ret=0` by construction. */
    bool        extRetFresh;
    /* `Thread=N`: N = 0 means this call **does not touch state shared across threads**
     * (module-level `var`, runtime buffers, output streams), so it may appear inside a
     * worker; N = 1 (the default) means it does, and is handled conservatively.
     *
     * Of the same nature as `Addr` / `Cont`: **an author signature**, not something the
     * compiler computes. The difference is that those two already have readers (lifetime
     * analysis) while `Thread` is only stored so far -- it serves the parallel step: only
     * `Thread=0` things may be called inside a worker body, which is what lets a diagnostic
     * say something useful ("`fs` uses a module-level buffer") instead of rejecting
     * everything wholesale. */
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
    /* Does this function print, directly or through the functions it calls?
     *
     * Used to decide whether an earlier call in the same statement has an observable
     * effect that the temporary variable for `??` would end up jumping ahead of.
     *   0 = not computed, 1 = prints, 2 = does not, 3 = being computed (a cycle is
     *   treated as "prints"). */
    /* Has this method or function ever been called?
     *
     * Generic instantiation rechecks and emits only the methods that are actually
     * called, so an unused `slice<T>::==` demands nothing of `T`, and a struct used in a
     * container need not declare `fn ==` unless something compares two of them.
     *
     * This is a conservative approximation: a call inside a template body counts as a
     * use, so a function may be checked and emitted more often than necessary, never
     * less. The flag itself is a plan fact, not syntax: it lives in the plan side table
     * (`planUsed` in plan.h). */
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
    /* 64 bits, and that is a **contract**, not a convenience (INV-P):
     * the mask is indexed by parameter position, so a function with more parameters than bits
     * would silently lose the high ones (parameter 40's `addrMask` bit would fall off the end).
     * `EFF_MAX_PARAMS` is checked when a function is declared, so the silent-loss case cannot
     * arise. Widening to an array is the escape hatch if a real program ever needs more. */
#define EFF_MAX_PARAMS 64
    /* State of the transitive closure of the effect summary:
     *   `EFF_NONE` = not computed, `EFF_DONE` = computed (`effComplete` says whether it can
     *   be trusted), `EFF_IN_PROGRESS` = being computed, which breaks cycles.
     * `effUnknown` records whether any call could not be resolved; such a summary is
     * never complete, so it is treated conservatively. */

    /* FuncDef*: the call graph the effect closure walks. **Deliberately not in the plan side
     * table**: it is a vector grown in place, and `vecPush`'s contract (base.c) forbids holding
     * a `Vec *` across a push -- an accessor that hands one back invites exactly that. A mutable
     * builder output is not "a result attached to a node". */
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
    /* `pub use ...`: **re-export** this name to whoever imports this module. Facade modules
     * use it to split the implementation without changing user code -- `std::io` may
     * `pub use std::io::fmt::{padText, ...}`, and callers still write `io::padText(...)`.
     * Only an explicit name list (or a whole module path) can be re-exported;
     * `pub use x::*` is rejected, because "re-export every name" would let the public
     * surface drift with implementation files. */
    bool        isPub;
    /* `use std::io::*`: the module's **public names are in scope unqualified** as
     * well. Qualified names keep working (they always do), and a `@private` name is
     * still out of reach: opening a module is not a way around privacy.
     *
     * The default is that a name from another module is written `mod::name`; this is the
     * opt-in a user asks for by writing the star, which is why the form is explicit
     * rather than implied. */
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

/* The two questions below are **plan** questions ("does one call to this function hand the
 * hidden home arena / zone down?"), and their answers, like the rest of the plan, live in
 * `plan.c` / `plan.h`. ast.h cannot include plan.h -- plan.h depends on ast.h -- so the two
 * accessors are declared here, for the two askers. */

/**
 * @brief Tell whether this function **takes** the home arena -- that is, whether a call to
 * it has to hand one down.
 *
 * One spelling, two askers: the checker's precise question (which decides whether the call
 * site hands an arena down) and code generation's implicit-argument owner
 * (`cgImplicitArgs`, which emits the parameter in a signature and passes it at a call site).
 * The two must never disagree: a definition that declares `extc_arena *__extc_home` while
 * the call site passes nothing is exactly the shape this accessor exists to rule out.
 *
 * @param f  Function to query.
 * @return   `true` when a call to it has to hand the home arena down.
 */
bool planUsesHome(const FuncDef *f);

/**
 * @brief Tell whether this function **takes** the hidden home zone -- that is, whether a
 * call to it has to hand one down.
 *
 * Same shape as `funcTakesHomeArena`: one spelling, used by the checker's promotion decision
 * and by code generation's implicit-argument owner (`cgImplicitArgs`). It is a
 * **different** question: it decides whether a call hands one down, not whether the body
 * itself needs an arena.
 *
 * @param f  Function to query.
 * @return   `true` when a call to it has to hand the home zone down.
 */
bool planMakesPool(const FuncDef *f);

static inline bool funcTakesHomeArena(const FuncDef *f) { return f && planUsesHome(f); }

/* Does this function **take** the hidden home zone -- i.e. does a call to it hand one down?
 *
 * One spelling, two askers, same shape as `funcTakesHomeArena`: the checker's promotion decision
 * and codegen's implicit-argument owner (`cgImplicitArgs`, which emits both the parameter in a
 * signature and the argument at a call site).
 *
 * **Known divergence, not yet fixed**: the checker promotes on the richer `calleeMakesPool`,
 * which is also true for `@poolObject` methods, extern constructor names and `isAssoc` on an
 * owner whose body builds a pool anywhere (`anMakesPoolAny`); this accessor reads the raw
 * flag, which those three do not set. So a site can be promoted to the zone level and still
 * have no argument emitted. Closing it means teaching both sides the same question, which
 * needs the pool-constructor name list to be shared instead of spelled twice: do the two
 * together. */
static inline bool funcTakesHomeZone(const FuncDef *f) { return f && planMakesPool(f); }

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
    Ctx        *ctx;       /* the file the block was written in, so a diagnostic about the
                            * block blames that file (module bodies are checked under their
                            * own Ctx; a location rule is no different). */
    bool        isExt;      /* `@ext`: the block extends a type declared by another module.
                             * The checker requires it exactly when the modules differ, so the
                             * provider of a method set is written down where it is added. */
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
    int         line;
    Vec         typeParams; /* const char*: `trait Codec<T>`. `Self` is the trait's **implicit**
                             * type parameter; these are the explicit ones,
                             * and every method's own list carries `Self` followed by these. */
    Vec         methods;    /* FuncDef*: signatures only (`body == NULL`) */
};

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*: the `type` enums */
    Vec funcs;                   /* FuncDef* */
    Vec globals;                 /* GlobalDef*: top-level let / var */
    Vec traits;                  /* TraitDef*: `trait Name { ... }` declarations */
    Vec impls;                   /* ImplDef*: `impl Type { ... }` blocks, attached by the
                                  * checker (each module attaches its own; the *effect* on a
                                  * type is global, which is what enforces coherence) */
    Vec uses;                    /* UseDecl* */
    /* When `module a::b` is written at the top of a file, it decides what the module is
     * **called**, and the file path falls back to the physical layer:
     * `stdlib/std/io/fmt.extc` can declare `module std::io::fmt`, so splitting the library or
     * moving files is not tied to the "file name is the module name" rule. NULL when
     * there is no declaration, keeping "path = module name". */
    const char *declName;
    int         declLine;     /* the line of the `module` declaration (diagnostic position). */
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

/**
 * @brief Prepare an empty module.
 *
 * Declarations are appended as the parser and loader discover them, so a Module can end up
 * holding, in order, the prelude, the user's file, and then every module that file imports.
 *
 * @param m  Module to initialize; the caller owns the storage.
 * @param a  Arena the vectors allocate from.
 */
void moduleInit(Module *m, Arena *a);

/**
 * @brief Tell whether a function is a method.
 *
 * The rule is the name: a first parameter named `self` makes it a method; nothing else in a
 * declaration distinguishes a method from a free function.
 *
 * @param f  Function to inspect.
 * @return   `true` when the first parameter is named `self`; `false` when `f` is NULL or has
 *           no parameters.
 */
bool funcIsMethod(const FuncDef *f);

/* ------------------------------------------------------- walking the tree
 *
 * The one place that lists an AST node's children.
 *
 * Every question of the form "what is inside this expression" walks the same tree, and a
 * hand-written copy of that walk has to be kept in step with the AST. The copies end in
 * `default:`, so `-Wswitch` says nothing when a new kind gains children -- which is how
 * `EX_SLICE`'s two bounds were skipped by eight walkers at once, and `EX_DYN`'s payload by
 * sixteen. Questions call the functions below instead of
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

/**
 * @brief Visit the direct children of expression `e`.
 *
 * The callback visits a **child** and never the node it was called for; returning false from
 * the callback stops the whole walk. With no expression callback set it returns `true`
 * immediately (nothing to visit).
 *
 * @param e  Expression to walk; NULL returns `true` directly.
 * @param v  The callbacks and their context.
 * @return   `true` when the walk completed; `false` when a callback returned false.
 */
bool astWalkExprChildren(Expr *e, const AstVisit *v);

/**
 * @brief Visit the direct children of statement `s`.
 *
 * Same convention as `astWalkExprChildren`: the callback visits a child, and returning false
 * stops the whole walk. With no statement callback set the walk keeps going down (recursing
 * into the children).
 *
 * @param s  Statement to walk; NULL returns `true` directly.
 * @param v  The callbacks and their context.
 * @return   `true` when the walk completed; `false` when a callback returned false.
 */
bool astWalkStmtChildren(Stmt *s, const AstVisit *v);

#endif /* EXTC_AST_H */
