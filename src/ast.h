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
typedef struct FuncDef FuncDef;
typedef struct FieldDef FieldDef;
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
};

Type *typeNamed(Arena *a, const char *name);   /* TY_UNRESOLVED; targs may be filled in after */
Type *typeRef(Arena *a, Type *inner);
Type *typeParam(Arena *a, const char *name, int idx);
Type *typeArray(Arena *a, int64_t n, Type *elem);   /* TY_ARRAY */

/* ------------------------------------------------------------ expressions */

/* Sentinel for "allocate into this function's home arena" -- the arena the caller
 * chose and passed down. It means the same thing as the -1 that `homeDepth` and
 * `arenaArg` carry. Those fields are all filled in by the checker and code
 * generation only translates them: it must never re-derive the answer from "does
 * this function have a home arena", because that would be a second source of truth
 * for a decision that is already made. */
#define ARENA_HOME (-1)

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
    EX_NULL       /* `null`: the zero value of a nullable reference (`?ref T`). It may
                   * appear only where the context already says which `?ref T` is meant;
                   * the type is supplied by adoptContextType, the same way an empty
                   * array literal picks up its element type. */
} ExprKind;

struct Expr {
    ExprKind kind;
    int      line;

    /* ---- filled in by the type checker ---- */
    Type     *type;
    FuncDef  *func;     /* the function an EX_CALL or EX_METHOD resolved to; for `==`
                         * it is the eq method */
    FieldDef *field;    /* the field an EX_FIELD resolved to */
    Type     *assocOwner; /* EX_ASSOC: the instance type it resolved to, kept so the
                           * C name can be mangled */
    bool      needEq;   /* an operand of `==` mentions a type parameter, so the check
                         * waits until the generic is instantiated */
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
        struct { const char *typeName; Type *type; Expr *operand; } conv; /* `i32(x)` */
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
};

typedef struct { const char *name; Expr *value; } FieldInit;

Expr *exprNew(Arena *a, ExprKind kind, int line);

/* ------------------------------------------------------------ statements */

typedef enum {
    ST_VAR, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN,
    ST_BREAK, ST_CONTINUE, ST_EXPR, ST_BLOCK, ST_MATCH
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

    Type    *type;      /* ST_VAR: the final type of the declaration, filled in by
                         * the checker */

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
        struct { Expr *target; Expr *value; } assign;
        struct { Expr *cond; Stmt *thenBody; Stmt *elseBody; } ifs;
        struct { Expr *cond; Stmt *body; } whiles;
        struct { Expr *value; } ret;
        struct { Expr *expr; } expr;
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
    Type       *ret;             /* NULL when the function returns nothing */
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
    const char *externLib;       /* the name in `extern!("libc")`, used in diagnostics */
    bool        hasEffects;
    unsigned    extAddrMask, extContMask;
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
    unsigned    addrMask, contMask, otherMask;      /* destination: a parameter's container */
    unsigned    homeAddrMask, homeContMask;         /* destination: memory allocated here */
    unsigned    freshCount;                         /* count only: fresh locals seen this round */
    /* State of the transitive closure of the effect summary:
     *   0 = not computed, 1 = computed (`effComplete` says whether it can be trusted),
     *   3 = being computed, which breaks cycles.
     * `effUnknown` records whether any call could not be resolved; such a summary is
     * never complete, so it is treated conservatively. */
    int         effState;
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

typedef struct {
    const char *path;      /* "std::io", exactly as written; used in error messages */
    const char *shortName; /* "io": members are then referred to as `io::name` */
    const char *file;      /* filled in by the loader: the file it resolved to */
    int         line;
} UseDecl;

typedef struct {
    Vec structs;                 /* StructDef* */
    Vec types;                   /* TypeDef*: the `type` enums */
    Vec funcs;                   /* FuncDef* */
    Vec globals;                 /* GlobalDef*: top-level let / var */
    Vec uses;                    /* UseDecl* */
    /* Entries mapping a bare name back to its mangled module name (`pair` to
     * `liba$pair`); the loader fills them in and `ttResolve` looks them up. */
    Vec aliases;                 /* Alias* */
} Module;

void moduleInit(Module *m, Arena *a);

/* A function whose first parameter is named `self` is a method. */
bool funcIsMethod(const FuncDef *f);

#endif /* EXTC_AST_H */
