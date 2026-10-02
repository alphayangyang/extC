/* Top level: function bodies, generic instantiation, deferred checks.
 *
 * This is the last checking pass. It validates every declaration for duplicate
 * names, computes the effect summary of every function (does it reach an allocator,
 * can a reference it hands out escape), instantiates generic functions at each call
 * site, and checks each function body and global initializer for lifetimes and
 * arena levels.
 */

#include "dbg.h"
#include "check_internal.h"
#include "plan.h"          /* 计划/分析产物：写入经 setter，读取经访问器（X2）*/
/* Defined further down; the call-site publication needs it (INV-K / audit P0-16). */
static void noteFieldSrc(Sym *root, const char *field, int d2, Expr *src);
#include "dataflow.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <stdlib.h>   /* getenv (the EXTC_DUMP_EFFECTS debug switch) */
#include <time.h>

static int ctOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }
static double ctNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
static void ctPhase(const char *n, double t0) { if (ctOn()) fprintf(stderr, "[time] %-9s %.3f s\n", n, ctNow() - t0); }

/* Type parameters visible inside a function.
 *
 * A method declared inside a struct sees the struct's type parameters; a free
 * function sees its own. Both cases are returned from here, so that every site that
 * resolves a type inside a function body asks exactly one place.
 *
 * Params:
 *   f - function or method to inspect; NULL is allowed
 *
 * Returns:
 *   The parameter list to instantiate, or NULL when the function is not generic.
 */
Vec *funcTParams(FuncDef *f) {
    if (!f) return NULL;
    if (f->typeParams.len) return &f->typeParams;
    if (f->owner) return &f->owner->typeParams;
    return NULL;
}

/* Forward declarations: the effect-summary walk and the escape analysis call each
 * other, so one of them has to be declared ahead of its definition. */
/* Do two declarations come from the same module? NULL means the root file (or the prelude),
 * and "the same" for two NULLs: that is this program, where an implementation may always live. */
static bool sameModule(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static int  paramIndexByName(FuncDef *f, const char *name);   /* 只在只有名字时用；有表达式用 paramIndexOfExpr */
static bool exprIsFresh(Expr *e);   /* used by the call-site publication (INV-K) */

/* ------------------------------------------------------------------- top level */

/* Resolve every type written in a signature.
 *
 * Runs before any body is checked, so the rest of the pass never has to resolve a
 * type name again.
 *
 * Params:
 *   c - checker (type table and module context used for resolution)
 *   f - function whose parameter and return types are resolved in place
 */
void resolveSignature(Checker *c, FuncDef *f) {
    /* A method sees the type parameters of the struct it belongs to; a free function
     * sees its own (`funcTParams` returns whichever applies). */
    Vec *params = funcTParams(f);
    /* The type names in a signature belong to the file that declares it, so they are
     * resolved under that file's context (`FuncDef.ctx`), not the entry file's. */
    Ctx *savedCtx = c->ctx;
    if (f->ctx) c->ctx = f->ctx;

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line, params);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line, params);
    if (f->ret && ttIs(f->ret, "void")) f->ret = NULL;
    c->ctx = savedCtx;
}

/* Reject duplicate names among the module's declarations.
 *
 * Names are looked up by string in the scope tables, so a duplicate would silently
 * shadow the other definition instead of being reported. Each declaration kind is
 * compared on its own, which is what makes the diagnostic name the colliding pair.
 *
 * Params:
 *   c - checker; every error is reported at the line of the later declaration
 */
static void checkDeclarations(Checker *c) {
    Module *m = c->m;

    /* Duplicate struct and function names. */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *a = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = i + 1; j < m->structs.len; j++) {
            StructDef *b = *(StructDef **)vecAt(&m->structs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line,
                        (a->reserved || b->reserved)
                            ? "It comes from stdlib/prelude.extc and is part of the language contract."
                            : NULL,
                        (a->reserved || b->reserved)
                            ? "`%s` is a reserved definition and cannot be redefined"
                            : "duplicate struct `%s`", b->name);
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        if (i + 8 < m->funcs.len) __builtin_prefetch(*(FuncDef **)vecAt(&m->funcs, i + 8), 0, 0);
        FuncDef *a = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t j = i + 1; j < m->funcs.len; j++) {
            FuncDef *b = *(FuncDef **)vecAt(&m->funcs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line,
                        (a->reserved || b->reserved)
                            ? "It comes from stdlib/prelude.extc and is part of the language contract."
                            : NULL,
                        (a->reserved || b->reserved)
                            ? "`%s` is a reserved definition and cannot be redefined"
                            : "duplicate function `%s`", b->name);
        }
    }

    /* Duplicate field names, duplicate method names, and a method that collides with
     * a field of the same struct (the field would become unreachable). */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            if (j + 8 < sd->fields.len) __builtin_prefetch(*(FieldDef **)vecAt(&sd->fields, j + 8), 0, 0);
            FieldDef *fa = *(FieldDef **)vecAt(&sd->fields, j);
            for (size_t k = j + 1; k < sd->fields.len; k++) {
                FieldDef *fb = *(FieldDef **)vecAt(&sd->fields, k);
                if (strcmp(fa->name, fb->name) == 0)
                    ckError(c, fb->line, NULL, "struct `%s` has duplicate field `%s`",
                            DN(sd), fb->name);
            }
        }
        for (size_t j = 0; j < sd->methods.len; j++) {
            if (j + 8 < sd->methods.len) __builtin_prefetch(*(FuncDef **)vecAt(&sd->methods, j + 8), 0, 0);
            FuncDef *ma = *(FuncDef **)vecAt(&sd->methods, j);
            if (findField(sd, ma->name))
                ckError(c, ma->line, NULL, "`%s.%s`: a field and a method cannot share a name",
                        DN(sd), ma->name);
            for (size_t k = j + 1; k < sd->methods.len; k++) {
                FuncDef *mb = *(FuncDef **)vecAt(&sd->methods, k);
                if (strcmp(ma->name, mb->name) != 0) continue;
                /* An operator name may be defined once per right-operand type: its shape
                 * is fixed (`self: ref T` plus one operand), so the operand type is the
                 * only thing left to tell two definitions apart, and matching it exactly
                 * is what makes the set decidable. The same operand type twice is still a
                 * duplicate -- there would be no rule to pick between them. */
                if (isOverloadableOp(ma->name) && ma->params.len == mb->params.len &&
                    ma->params.len >= 2) {
                    Param *qa = *(Param **)vecAt(&ma->params, 1);
                    Param *qb = *(Param **)vecAt(&mb->params, 1);
                    if (!ttEquals(ttBase(qa->type), ttBase(qb->type))) continue;
                    ckError(c, mb->line,
                            "Operators are matched on the type of the right operand, so two "
                            "definitions that take the same one cannot be told apart.",
                            "struct `%s` has two `%s` for `%s`", DN(sd), mb->name,
                            typeStr(c, ttBase(qb->type)));
                    continue;
                }
                /* Name the earlier declaration too: with one method set per type, "which of the
                 * two is the duplicate" is the first question a reader asks. This is the single
                 * place that answers it, for body methods and `impl` methods alike. */
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "a type has one method set and `impl` adds to it, it does not"
                                 " replace it: the first declaration is at line %d%s",
                          (ma->body && ma->body->line > 0) ? ma->body->line : 0,
                          ma->modName ? " of the module that declared it" : "");
                ckError(c, mb->line, bufCstr(&note), "struct `%s` has duplicate method `%s`",
                        DN(sd), mb->name);
            }
        }
    }

    /* Duplicate type names, duplicate variant names, and a type with no variants
     * (there would be no value of it to construct). */
    for (size_t i = 0; i < m->types.len; i++) {
        if (i + 8 < m->types.len) __builtin_prefetch(*(TypeDef **)vecAt(&m->types, i + 8), 0, 0);
        TypeDef *a = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = i + 1; j < m->types.len; j++) {
            TypeDef *b = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate type `%s`", b->name);
        }
        if (a->variants.len == 0)
            ckError(c, a->line, NULL, "type `%s` has no variants", a->name);
        for (size_t j = 0; j < a->variants.len; j++) {
            if (j + 8 < a->variants.len) __builtin_prefetch(*(Variant **)vecAt(&a->variants, j + 8), 0, 0);
            Variant *va = *(Variant **)vecAt(&a->variants, j);
            for (size_t k = j + 1; k < a->variants.len; k++) {
                Variant *vb = *(Variant **)vecAt(&a->variants, k);
                if (strcmp(va->name, vb->name) == 0)
                    ckError(c, vb->line, NULL, "type `%s` has duplicate variant `%s`",
                            a->name, vb->name);
            }
        }
    }

    /* A struct and a type share one namespace, so they may not collide either. */
    for (size_t i = 0; i < m->structs.len; i++) {
        if (i + 8 < m->structs.len) __builtin_prefetch(*(StructDef **)vecAt(&m->structs, i + 8), 0, 0);
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < m->types.len; j++) {
            if (j + 8 < m->types.len) __builtin_prefetch(*(TypeDef **)vecAt(&m->types, j + 8), 0, 0);
            TypeDef *td = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(sd->name, td->name) == 0)
                ckError(c, td->line, NULL, "`%s` is already a struct", DN(td));
        }
    }
    /* `impl Trait for Type`: do the signatures agree?
     *
     * This runs here rather than at attachment because comparing types requires them to be
     * **resolved** -- `Self` in a trait body is one name away from the implementing type, and
     * `ttSubstitute` is the existing machinery for exactly that substitution. The pair itself
     * (which trait, which type) was recorded at attachment, so nothing is looked up twice. */
    for (size_t i = 0; i < m->impls.len; i++) {
        if (i + 8 < m->impls.len) __builtin_prefetch(*(ImplDef **)vecAt(&m->impls, i + 8), 0, 0);
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!im->trait || !im->target) continue;
        StructDef *sd = structOf(im->target);
        if (!sd) continue;
        Vec selfParams, selfArgs;
        vecInit(&selfParams, c->arena, sizeof(const char *));
        vecInit(&selfArgs,   c->arena, sizeof(Type *));
        *(const char **)vecPush(&selfParams) = "Self";
        *(Type **)vecPush(&selfArgs) = im->target;
        /* `impl Codec<i64> for box`: the trait's **own** parameters substitute through the same
         * pair of parallel lists, so a declared signature that mentions `T` is compared against
         * the implementation as `i64`. Without this the conformance check compared `T` with `i64`
         * and rejected every explicit-parameter trait. */
        for (size_t pi = 0; pi < im->traitArgs.len && pi < im->trait->typeParams.len; pi++) {
            *(const char **)vecPush(&selfParams) =
                *(const char **)vecAt(&im->trait->typeParams, pi);
            /* Resolved, not as written: the parser hands back an unresolved `i64`, and substituting
             * that in produces a look-alike node that `ttEquals` (interning identity) rejects --
             * the check then reported "is `i64`, but the trait declares `i64`". */
            *(Type **)vecPush(&selfArgs) =
                ttResolve(c->tt, c->ctx, *(Type **)vecAt(&im->traitArgs, pi), im->line, NULL);
        }
        for (size_t k = 0; k < im->trait->methods.len; k++) {
            if (k + 8 < im->trait->methods.len) __builtin_prefetch(*(FuncDef **)vecAt(&im->trait->methods, k + 8), 0, 0);
            FuncDef *want = *(FuncDef **)vecAt(&im->trait->methods, k);
            FuncDef *have = NULL;
            for (size_t j = 0; j < sd->methods.len && !have; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (strcmp(cand->name, want->name) == 0) have = cand;
            }
            if (!have) continue;            /* absence was reported at attachment */
            if (funcIsMethod(want) != funcIsMethod(have) ||
                want->params.len != have->params.len) {
                ckError(c, im->line,
                        "The implementation repeats the trait's signature exactly, including"
                        " whether the method takes a receiver.",
                        "`%s`: `%s` does not match the signature the trait declares",
                        im->trait->name, want->name);
                continue;
            }
            for (size_t j = 1; j < want->params.len; j++) {
                Param *wp = *(Param **)vecAt(&want->params, j);
                Param *hp = *(Param **)vecAt(&have->params, j);
                /* Substitute only when the type actually mentions a parameter: `ttEquals` is
                 * interning identity, and rebuilding `i64` through `ttSubstitute` produces a
                 * look-alike node that would never compare equal (found by a false positive on
                 * the positive case). */
                Type  *wantT = mentionsParam(wp->type)
                             ? ttBase(ttSubstitute(c->tt, wp->type, &selfParams, &selfArgs))
                             : ttBase(wp->type);
                if (!ttEquals(wantT, ttBase(hp->type)))
                    ckError(c, have->line,
                            "A trait method's parameter types are part of the contract and must"
                            " match exactly (`Self` stands for the implementing type).",
                            "`%s`: parameter %zu of `%s` is `%s`, but the trait declares `%s`",
                            im->trait->name, j + 1, want->name,
                            typeStr(c, ttBase(hp->type)), typeStr(c, wantT));
            }
            Type *wantR = want->ret && mentionsParam(want->ret)
                        ? ttBase(ttSubstitute(c->tt, want->ret, &selfParams, &selfArgs))
                        : (want->ret ? ttBase(want->ret) : NULL);
            Type *haveR = have->ret ? ttBase(have->ret) : NULL;
            if (wantR && haveR && !ttEquals(wantR, haveR))
                ckError(c, have->line,
                        "A trait method's return type is part of the contract and must match"
                        " exactly (`Self` stands for the implementing type).",
                        "`%s`: `%s` returns `%s`, but the trait declares `%s`",
                        im->trait->name, want->name, typeStr(c, haveR), typeStr(c, wantR));
        }
    }

}

/* Check the shape of a method: where it is declared, and what `self` is.
 *
 * Params:
 *   c - checker
 *   f - function to check; `f->owner` is NULL for a free function
 *
 * Notes:
 *   - A function that acts on a value must be declared inside a struct. An operator
 *     such as `==` is such a function, so declaring it outside a struct is an error
 *     rather than a free function that happens to be named `==`.
 */
static void checkMethodShape(Checker *c, FuncDef *f) {
    if (!f->owner) {
        /* A free function has no receiver, so it may not take `self`. */
        if (strcmp(f->name, "==") == 0 || strcmp(f->name, "!=") == 0)
            ckError(c, f->line, "an operator is a method, so it must be declared inside a `struct`",
                    "operator `%s` must be defined inside a `struct`", f->name);
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (strcmp(p->name, "self") == 0)
                ckError(c, p->line, "methods must be declared inside their `struct`",
                        "`self` is only allowed in a method declared inside a `struct`");
        }
        return;
    }

    Param *p0 = f->params.len ? *(Param **)vecAt(&f->params, 0) : NULL;
    if (!p0 || strcmp(p0->name, "self") != 0) {
        /* No `self` means an associated function: `option<i64>::some(x)` or
         * `point::origin()`. It belongs to the type but acts on no value, which is
         * how a container exposes its constructors. Such a call is explicit: the
         * call site writes the full type name (`option<i64>::some`), so nothing is
         * inferred from the expected type. */
        f->isAssoc = true;
    } else {
        /* Compare the struct definition rather than the type node: the `self` of a
         * method on a generic struct is written `ref Pair<A, B>`, so its type node
         * is not the same pointer as the owner's type. */

        Type *sb = ttBase(p0->type);
        /* A builtin scalar has no `sdef` to compare against, so its holder is matched by the
         * type it stands for (`impl i64` -> `self: ref i64`). Every other owner is a real
         * declaration and is matched by definition, which is also what lets a generic method
         * write `ref Pair<A, B>` and still belong to `Pair`. */
        bool selfOk = f->owner->builtinHolder
                        ? (p0->type->kind == TY_REF && p0->type->inner == f->owner->type)
                        : (p0->type->kind == TY_REF && sb && sb->sdef == f->owner);
        if (!selfOk)
            ckError(c, p0->line, NULL, "`self` of `%s.%s` must be `ref %s`",
                    DN(f->owner), f->name, DN(f->owner));
    }
    for (size_t i = 1; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0)
            ckError(c, p->line, NULL, "`self` must be the first parameter");
    }

    checkOperatorSig(c, f);
    checkCtorSig(c, f);
}

/* Does the body allocate (`new`)?
 *
 * The answer decides whether the function needs a home arena of its own. It is a
 * pure syntax walk, so it can run before any type is known, and the arena level of
 * each `new` inside the body is therefore decided on the spot.
 *
 * Returns:
 *   True when the statement contains an allocation or an unknown-length allocation.
 */
/* Does the statement contain an allocation (`new`)?
 *
 * This decides whether a function needs a home arena of its own, and it is a pure syntax
 * walk, so it can answer before any type is known.
 *
 * Params:
 *   s - statement to walk; may be NULL
 *
 * Returns:
 *   True when the statement contains an allocation or an unknown-length allocation.
 */
static bool stmtHasNew(Stmt *s);
static void coroFrameLay(Checker *c, FuncDef *f);
static void coroCheckDeferred(Checker *c, Module *m);

typedef struct {
    const char *cname;
    Expr       *init;
    int         line;
} CoroDeferred;

static bool hasNewInStmt(void *ctx, Stmt *s);
/* Does this expression contain `new`, or `alloc<T>(n)` (which is the same thing)?
 *
 * The walk lives in `astWalkExprChildren` / `astWalkStmtChildren` (`ast.c`): the AST kinds are
 * listed **once** there, so this question cannot fall behind the AST. `new` and a generic
 * allocation are the answer; every other node is answered by its children.
 *
 * A callback returns false to stop the walk, so "found" travels as that early exit. */
static bool hasNewInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (e->kind == EX_NEW || e->kind == EX_GENCALL) return false;   /* found */
    AstVisit v = { hasNewInExpr, hasNewInStmt, NULL };
    return astWalkExprChildren(e, &v);
}

static bool hasNewInStmt(void *ctx, Stmt *s) {
    (void)ctx;                              /* this question needs no context */
    AstVisit v = { hasNewInExpr, hasNewInStmt, NULL };
    return astWalkStmtChildren(s, &v);
}

static bool exprHasNew(Expr *e) {
    if (!e) return false;
    return !hasNewInExpr(NULL, e);
}

static bool stmtHasNew(Stmt *s) {
    if (!s) return false;
    return !hasNewInStmt(NULL, s);
}

/* Does the body call a function that needs a home arena?
 *
 * Params:
 *   s - statement to walk
 *
 * Returns:
 *   True when some call in the statement reaches a callee with `needsHome` set.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `e->func`, which
 *     the checker fills in when it resolves a call.
 */
/* Does the statement call a function that needs a home arena?
 *
 * Params:
 *   s - statement to walk; may be NULL
 *
 * Returns:
 *   True when some call in the statement reaches a callee whose `needsHome` is set.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `e->func`, which the
 *     checker fills in when it resolves a call.
 */
/* Does this expression, or this statement, reach a callee that needs a home arena?
 *
 * Two questions, one walk:
 *   precise = false  "does the callee reach a function with a home arena?"  -- the escape
 *                    question, which is what decides whether *this* function gets one;
 *   precise = true   "does the callee itself take a home arena?"             -- the narrow
 *                    question a **signature** needs.
 * `callsNeedsHome` / `callsUsesHome` below name the two, so no call site carries a bare flag.
 *
 * The walk is `astWalkExprChildren` / `astWalkStmtChildren` (`ast.c`), the one place that lists
 * the AST kinds. That matters here more than anywhere else, because the answer decides where
 * storage lives -- two bugs of exactly this shape are on record:
 *
 *   - `EX_STRUCTLIT` was missing from this predicate, so `var b: box = { r: mknode() }` looked
 *     like a body that reaches no callee with a home arena. The function was not given one, the
 *     callee allocated into the caller's *block* arena (released when the block ends) while the
 *     depth recorded for the value said 0, and the pointer dangled. Every shape a call can hide
 *     in therefore has to be listed -- which is now the job of `ast.c`, not of this file.
 *   - The hole used to be papered over for `main` alone by an `|| is main` fallback in
 *     `mayUseArena`, and that fallback had a measurable cost: every `main` carried the whole
 *     arena prologue, so every `while` body emitted `extc_arena_release(...)`, and a matrix
 *     multiply ran 103 ms instead of 34 ms (3.0x slower). Covering the root cause here let the
 *     fallback go.
 * The same two mistakes were also made once for `EX_DYN`'s payload and once for `EX_SLICE`'s
 * bounds (docs/topics/AST-WALKERS.md); migrating this predicate ends that class of bug for it.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `e->func`, which the checker
 *     fills in when it resolves a call. A node without one (a generic primitive) is descended
 *     into like any other.
 *   - An associated call (`EX_ASSOC`) counts as well: an associated function such as
 *     `T::make` is declared without `self` but takes a home arena like any other. */
/* **One question, one name.** This used to be `bool precise` -- a switch that changed what the
 * predicate below *meant* ("does the callee reach a home arena?" versus "does it take one itself?"),
 * which is precisely the shape that lets the two sides of a convention drift apart: the definition
 * asks one, the call site the other, and nothing in the code says which is which. The question now
 * travels as a predicate with a name, and the two entry points below (`callsNeedsHome` /
 * `callsUsesHome`) are the only places that pick one. */
typedef struct { bool (*takesHomeArena)(const FuncDef *); } HomeQ;

/* "does the callee **reach** a function that takes one?" -- the conservative, transitive question,
 * which decides whether *this* body needs a home arena of its own (`needsHome`). */
static bool reachesHomeArenaTaker(const FuncDef *f) { return f && f->needsHome; }

static bool needsHomeInStmt(void *ctx, Stmt *s);

static bool needsHomeInExpr(void *ctx, Expr *e) {
    HomeQ *q = (HomeQ *)ctx;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && e->func &&
        q->takesHomeArena(e->func))
        return false;                       /* found: stop the walk */
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

static bool needsHomeInStmt(void *ctx, Stmt *s) {
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

static bool stmtCallsNeedsHome(Stmt *s, bool (*takesHomeArena)(const FuncDef *)) {
    if (!s) return false;
    HomeQ q = { takesHomeArena };
    return !needsHomeInStmt(&q, s);
}


/* Does this expression, or this statement, mention the binding whose generated-C name is
 * `cname`?
 *
 * Used for one question only: is a parameter ever read?
 *
 * The walk itself lives in `astWalkExprChildren` / `astWalkStmtChildren` (`ast.c`) since
 * 2026-09-26: the AST kinds are listed **once** there, so a new kind cannot be forgotten here.
 * That is not hypothetical -- this question and seven siblings each carried their own copy of
 * the kind list, each ending in `default:`, so `-Wswitch` stayed silent while `EX_SLICE`'s two
 * bounds and `EX_DYN`'s payload went missing from all of them (docs/topics/AST-WALKERS.md).
 *
 * The comparison is on the *generated* name, so a local that shadows the parameter (which gets
 * `__2` appended) cannot be mistaken for it.
 *
 * A callback returns false to stop the walk; "found" is carried by that early exit. */
typedef struct { const char *cname; } CnameCtx;

static bool cnameInStmt(void *ctx, Stmt *s);

static bool cnameInExpr(void *ctx, Expr *e) {
    CnameCtx *c = (CnameCtx *)ctx;
    if (e->kind == EX_IDENT)
        return !(e->u.ident.cname && strcmp(e->u.ident.cname, c->cname) == 0);
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

static bool cnameInStmt(void *ctx, Stmt *s) {
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

static bool stmtUsesCname(Stmt *s, const char *cname) {
    if (!s) return false;
    CnameCtx c = { cname };
    return !cnameInStmt(&c, s);
}

static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body, reachesHomeArenaTaker); }
/* 精确那一问直接用共享 accessor（`ast.h`）—— 与 codegen 的主人问的是同一处拼写。 */
static bool callsUsesHome(Stmt *body)  { return stmtCallsNeedsHome(body, funcTakesHomeArena); }

/* ---- how a "reaches X through its calls" property is closed over the call graph ----
 *
 * The rule is monotone and the lattice is finite, so iterating until a round changes nothing
 * yields the **least** fixed point -- which is the answer wanted, not merely a consistent one: a
 * larger fixed point would mark functions that cannot have the property and give back the
 * optimisations these passes exist for. Recursion and mutual recursion need no special case: a
 * cycle stays false until one of its members is reached from a body that really does have it, and
 * if none of them ever is, none of them can have it at all.
 *
 * This rule used to be written twice in this file (the `needsHome` closure, and "Same shape as the
 * `needsHome` closure above" for `makesPool`), and a third time as the lazy walk behind
 * `funcAllocates`. It lives here now, with the per-node criterion in `bodyReaches`, so changing
 * either is a change in one place. What legitimately differs between callers is only **which**
 * functions a round visits -- `makesPool` has an extra round over the generic instances -- and
 * that stays at the call site, where its reason is visible.
 *
 * The lazy path (`funcAllocates`) keeps its own recursion and memo: it has to answer *before*
 * these closures run, and it treats a function already on the stack as allocating, which is the
 * conservative direction for a question that is asked mid-check. Same criterion, same field; the
 * evaluation strategy is the only difference, and that difference is deliberate. */
typedef enum {
    REACH_HOME,     /* `FuncDef.needsHome`: the body reaches a callee that needs a home arena */
    REACH_POOL,     /* `FuncDef.makesPool`: the body may create a pool                       */
    REACH_ALLOC,    /* `FuncDef.allocState`: the body may allocate (also asked lazily)        */
    REACH_USES_HOME /* `FuncDef.usesHome`: the body really reads the home arena it is given  */
} ReachKind;

/* What a round needs: the module being closed, and the type table (`makesPool` resolves protocol
 * methods on generic instances through it). The closures never needed a `Checker *` -- their
 * criterion is `e->func`, which the checker filled in while the bodies were checked -- and the
 * pointer is here for one reason only: `EXTC_DBG_FX` counts the rounds this driver runs. */
typedef struct { Module *m; TypeTable *tt; Checker *c; } CloseCtx;

static bool getReach(const FuncDef *f, ReachKind k) {
    switch (k) {
    case REACH_HOME:  return f->needsHome;
    case REACH_POOL:  return planMakesPool(f);
    case REACH_ALLOC: return f->allocState == 1;
    case REACH_USES_HOME: return planUsesHome(f);
    }
    return false;
}

static void setReach(FuncDef *f, ReachKind k, bool v) {
    switch (k) {
    case REACH_HOME:  f->needsHome = v; break;
    case REACH_POOL:  planSetMakesPool(f, v); break;
    case REACH_ALLOC: f->allocState = v ? 1 : 2; break;
    case REACH_USES_HOME: planSetUsesHome(f, v); break;
    }
}

/* Does this body **directly** reach a callee of that kind? The transitive part is `closeReach`. */
static bool bodyReaches(FuncDef *f, ReachKind k) {
    switch (k) {
    case REACH_HOME:  return callsNeedsHome(f->body);
    case REACH_POOL:  return stmtMakesPool(f->body, true);
    case REACH_ALLOC: return stmtHasNew(f->body);   /* `funcAllocates` adds the callee half */
    case REACH_USES_HOME:
        /* Narrower than `needsHome` on purpose: `needsHome` is the escape question and is
         * deliberately wide (a `new` whose value is copied into an out-parameter marks the
         * function even though nothing escapes), while the hidden parameter is only needed when
         * the body really reads it. The placement pass decides that per site, so a site sitting
         * in the home arena counts, as does handing this function's home on to a callee. Measured:
         * `examples/out-param.extc` used to carry a parameter nobody read, and gcc said so. */
        if (callsUsesHome(f->body)) return true;
        for (size_t i = 0; i < f->arenaSites.len; i++)
            if (planArenaLevel(*(Expr **)vecAt(&f->arenaSites, i)) == ARENA_HOME) return true;
        return false;
    }
    return false;
}

/* A round raises the flag on the functions its caller considers; the driver repeats rounds until
 * one changes nothing. */
static void closeReach(CloseCtx *cx, ReachKind k,
                       void (*round)(CloseCtx *cx, ReachKind k, bool *changed)) {
    for (bool changed = true; changed; ) {
        changed = false;
        round(cx, k, &changed);
        if (cx->c && cx->c->fxOn) cx->c->fxReachRounds++;
    }
}

/* The two rounds most closures share: the module's functions, then the methods of its structs.
 * `skipExtern` is for the properties an extern cannot have -- it has no body to walk. */
static void roundOverModule(CloseCtx *cx, ReachKind k, bool *changed, bool skipExtern) {
    Module *m = cx->m;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (cx->c && cx->c->fxOn) cx->c->fxReachVisits++;
        if (getReach(f, k) || !f->body || (skipExtern && f->isExtern)) continue;
        if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (cx->c && cx->c->fxOn) cx->c->fxReachVisits++;
            if (getReach(f, k) || !f->body) continue;
            if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
        }
    }
}

static void roundNeedsHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);      /* an extern has no body to walk */
}

static void roundUsesHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);
}

static void roundMakesPool(CloseCtx *cx, ReachKind k, bool *changed);   /* defined below */

/* ---- Does this code create a pool? (`FuncDef.makesPool`; see ast.h for why) ----
 *
 * The runtime primitive that creates one is `extc_pool_new`, and it is the *only* one:
 * `drop`/`reset`/`generation`/`live`/`capacity`/`zoneDepth` neither create a record nor
 * need a zone to exist. So the whole predicate rests on finding calls to that one name,
 * directly or through a callee that reaches it.
 *
 * Two callers ask two different questions with it:
 *   - the fixed-point closure below asks the transitive one ("can this function create a
 *     pool at all?"), and walks nested blocks, which each have a zone of their own;
 *   - codegen asks the block-local one ("can *this* block's direct statements create one
 *     here?"), and stops at a nested block, because that block's own hook covers it.
 * `descendBlocks` picks between them.
 *
 * Unknown callees count as creating a pool, so the answer errs towards emitting a hook.
 */
static bool exprMakesPool(Expr *e, bool descendBlocks);

/* 解析钩子：checker 这一侧问不到"某个实例的具体方法是谁"。
 *
 * `#57` 让泛型体里对**类型参数**的协议方法调用在模板上**故意不写 `e->func`**（一份模板体
 * 有多个实例，写上去就是给别的实例写错方法）。不解析时 `exprMakesPool` 只能保守算真 ——
 * 而 `hashMap::find` 里正好有 `k.hash()`，于是整条链（find/get/put/remove/contains）全被
 * 标成"会建池"，循环体每轮压/弹一次 zone。
 *
 * codegen 生成某个实例时知道该解析成谁（`genMethodCall` 走的就是这一句）；闭包的实例那一轮
 * 也知道（拿这个实例的实参代入，`runMethodCheck` 是同一套）。解析不出来 ⇒ 仍旧保守。 */
static FuncDef *(*poolCalleeResolve)(Expr *e) = NULL;
void setPoolCalleeResolver(FuncDef *(*fn)(Expr *e)) { poolCalleeResolve = fn; }

/* 闭包实例那一轮的代入上下文。 */
static TypeTable *instResTT = NULL;
static Vec *instResParams = NULL;
static Vec *instResTargs = NULL;
static FuncDef *resolveOnInstance(Expr *e) {
    if (!instResTT || !instResParams || !instResTargs) return NULL;
    if (!e || e->kind != EX_METHOD || !e->u.method.recv) return NULL;
    Type *rt = ttSubstitute(instResTT, e->u.method.recv->type, instResParams, instResTargs);
    return findMethod(ttBase(rt), e->u.method.name);
}
bool stmtMakesPool(Stmt *s, bool descendBlocks) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMakesPool(s->u.yield_.value, descendBlocks);
    case ST_TRAP:   return exprMakesPool(s->u.trap_.msg, descendBlocks);
    case ST_VAR:    return exprMakesPool(s->u.var.init, descendBlocks);
    case ST_ASSIGN: return exprMakesPool(s->u.assign.value, descendBlocks) ||
                           exprMakesPool(s->u.assign.target, descendBlocks);
    case ST_IF:     return exprMakesPool(s->u.ifs.cond, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.thenBody, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.elseBody, descendBlocks);
    case ST_DOMAIN:
        /* A domain block is a place of its own, exactly like a block: what happens inside is that
         * block's business, and the transitive question walks in. */
        if (!descendBlocks) return false;
        return exprMakesPool(s->u.domain_.callee, descendBlocks) ||
               stmtMakesPool(s->u.domain_.body, descendBlocks);
    case ST_WHILE:  return exprMakesPool(s->u.whiles.cond, descendBlocks) ||
                           stmtMakesPool(s->u.whiles.body, descendBlocks);
    case ST_RETURN: return exprMakesPool(s->u.ret.value, descendBlocks);
    case ST_EXPR:   return exprMakesPool(s->u.expr.expr, descendBlocks);
    case ST_BLOCK:
        /* A block is a `place` of its own with its own zone, so the block-local question
         * stops here: whatever happens inside is that block's business. The transitive
         * question walks in. */
        if (!descendBlocks) return false;
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtMakesPool(*(Stmt **)vecAt(&s->u.block.stmts, i), descendBlocks)) return true;
        return false;
    case ST_MATCH:
        if (exprMakesPool(s->u.match.scrutinee, descendBlocks)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtMakesPool((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, descendBlocks)) return true;
        return false;
    default: return false;
    }
}
static bool exprMakesPool(Expr *e, bool descendBlocks) {
    if (!e) return false;
    /* The three shapes a resolved call takes. `EX_ASSOC` belongs here for the reason its
     * counterpart in `exprCallsNeedsHome` documents: an associated function records its
     * callee in `e->func` too, and leaving it out is how a summary quietly becomes wrong. */
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        /* `flush()` / `print(x)` / `println(x)` are builtins dispatched by name, and the
         * checker leaves `e->func` null for them (it returns before resolving a callee).
         * They cannot create a pool; naming them here keeps the conservative branch below
         * from spreading `makesPool` over half the library. Measured before this case
         * existed: `io::flushOut`, `io::writeBytesRaw` and `io::errWrite` were all marked,
         * purely because each of them calls `flush()`. */
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
            const char *bn = e->u.call.callee->u.ident.name;
            if (bn && (strcmp(bn, "flush") == 0 || strcmp(bn, "print") == 0 ||
                       strcmp(bn, "println") == 0))
                return false;
        }
        /* 这里问的是"**这一次调用能不能走到 `extc_pool_new`**"（zone 的决定，以及
         * `makesPool` 闭包本身），不是"结果会不会住在池上" —— 后者是 `calleeMakesPool`
         * 的问题（逃逸与提权那两处用它）。两者的差别与实测代价见 `calleeCreatesPool`。
         *
         * `#57`：泛型体里对**类型参数**的协议方法（`k.hash()`）在节点上**故意没有 func**，
         * 所以这一侧还要问解析钩子（由 codegen / 闭包的实例那一轮装上）。 */
        FuncDef *cf = e->func;
        if (!cf && e->kind == EX_METHOD && poolCalleeResolve) cf = poolCalleeResolve(e);
        if (calleeCreatesPool(cf)) return true;
    }
    switch (e->kind) {
    case EX_BIN: return exprMakesPool(e->u.bin.left, descendBlocks) ||
                       exprMakesPool(e->u.bin.right, descendBlocks);
    case EX_UN:  return exprMakesPool(e->u.un.operand, descendBlocks);
    case EX_REF: return exprMakesPool(e->u.ref.operand, descendBlocks);
    case EX_EXT: return exprMakesPool(e->u.ext_.call, descendBlocks);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprMakesPool(e->u.deref.operand, descendBlocks);
    case EX_SIGN:  return exprMakesPool(e->u.sign.operand, descendBlocks);
    case EX_CONV:  return exprMakesPool(e->u.conv.operand, descendBlocks);
    case EX_TRY:   return exprMakesPool(e->u.try_.operand, descendBlocks);
    case EX_INDEX: return exprMakesPool(e->u.index.obj, descendBlocks) ||
                          exprMakesPool(e->u.index.index, descendBlocks);
    /* The bounds are expressions too, and every walker in this file that forgot them had a
     * real defect (see the note on `exprUsesCname`); a call in a bound can create a pool. */
    case EX_SLICE: return exprMakesPool(e->u.slice.obj, descendBlocks) ||
                          exprMakesPool(e->u.slice.lo, descendBlocks) ||
                          exprMakesPool(e->u.slice.hi, descendBlocks);
    case EX_FIELD: return exprMakesPool(e->u.field.obj, descendBlocks);
    case EX_COALESCE:
        return exprMakesPool(e->u.coalesce.main, descendBlocks) ||
               exprMakesPool(e->u.coalesce.fallback, descendBlocks);
    case EX_METHOD: {
        if (exprMakesPool(e->u.method.recv, descendBlocks)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.method.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_CALL: {
        /* The callee as well: `f()()` is not legal today, but the walk costs nothing and a
         * shape left out is exactly how a summary goes wrong. */
        if (exprMakesPool(e->u.call.callee, descendBlocks)) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.call.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.assoc.args, i), descendBlocks)) return true;
        return false;
    }
    case EX_NEW: return exprMakesPool(e->u.new_.count, descendBlocks);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprMakesPool((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, descendBlocks)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprMakesPool((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value,
                              descendBlocks)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.arraylit.elems, i), descendBlocks)) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.enumval.args, i), descendBlocks)) return true;
        return false;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprMakesPool(*(Expr **)vecAt(&e->u.gencall.args, i), descendBlocks)) return true;
        return false;
    /* The payload may build a pool of its own, and then the enclosing call needs a zone. */
    case EX_DYN: return exprMakesPool(e->u.dynv.payload, descendBlocks);
    default: return false;
    }
}

/* Count the `@overwrite` sites in a body.
 *
 * Params:
 *   s - function body to walk
 *
 * Returns:
 *   Number of `@overwrite` bindings in the body.
 *
 * Notes:
 *   - The order must match codegen's `collectOwSites`: both walk the same tree in
 *     source order, so the n-th site here is the n-th cell there.
 */
static int countOwSites(Stmt *s) {

    if (!s) return 0;
    switch (s->kind) {
    case ST_VAR:  return s->u.var.overwrite ? 1 : 0;
    case ST_BLOCK: {
        int n = 0;
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            n += countOwSites(*(Stmt **)vecAt(&s->u.block.stmts, i));
        return n;
    }
    case ST_IF:    return countOwSites(s->u.ifs.thenBody) + countOwSites(s->u.ifs.elseBody);
    case ST_WHILE: return countOwSites(s->u.whiles.body);
    case ST_MATCH: {
        int n = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            n += countOwSites((*(MatchArm **)vecAt(&s->u.match.arms, i))->body);
        return n;
    }
    default: return 0;
    }
}

/* Report whether the function can reach itself through its callees.
 *
 * A function that can re-enter itself needs one `@overwrite` cell per activation:
 * with a single shared cell a nested call overwrites the storage of the activation
 * that is still running, and the outer activation then reads the inner call's data
 * after it returns.
 *
 * Params:
 *   c     - unused; the walk reads only the call graph recorded in `FuncDef`
 *   f     - function to test
 *   depth - call-graph hops this walk has already taken, used by the cycle guard
 *
 * Returns:
 *   True when `f` can reach itself, or when the walk cannot decide.
 *
 * Notes:
 *   - The conservative direction is "yes": a callee entry that is NULL answers true,
 *     which costs at most one extra cell per activation.
 *   - The walk stops at `depth > 64` and answers true, so a cyclic call graph cannot
 *     overflow the stack.
 */
static bool funcReachesItself(Checker *c, FuncDef *f, int depth) {
    if (depth > 64) return true;                      /* guard against a cycle: assume re-entrant */
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (!g) return true;
        if (g == f) return true;
        if (funcReachesItself(c, g, depth + 1)) return true;
    }
    return false;
}

/* Does the body write through a `*p`? The out-parameter shape is
 * `fn push(head: mut ref ?ref node) { *head = cell }`. If it does, the function needs
 * a home arena (the arena the caller passes in), so that what it allocates lands on
 * the argument side.
 */
/* Return the name of the identifier a place expression is rooted at.
 *
 * The checker asks whether a store target is a place on the parameter side, and
 * `*head = cell`, `l.head = cell`, and `l.buf[i] = cell` all count. Walking down to the
 * root identifier is what makes that decidable.
 *
 * Params:
 *   e - place expression; NULL is allowed
 *
 * Returns:
 *   The root identifier's name, or NULL when the expression is not a place (a literal,
 *   a call, and so on).
 *
 * Notes:
 *   - A reference-typed binding such as `?ref node` cannot be borrowed again, so an
 *     out-parameter is conventionally wrapped in a struct; `varArray<T>` already has
 *     that shape, which is why `l.buf[i] = cell` is the usual way to write into a
 *     parameter.
 */
const char *placeRootName(Expr *e) {
    /* 问题④：根的**语法名字**（诊断与老接口）。掩码 = FIELD|INDEX|DEREF，不解析 ——
     * 所以它对未解析的节点也能用，这正是它与 `placeRoot` 的关键差别。*/
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF, 0);
    return leaf ? leaf->u.ident.name : NULL;
}
/* True when `n` names a parameter of `f`.
 *
 * Params:
 *   f - function whose parameter list is searched
 *   n - name to look for
 *
 * Returns:
 *   True when some parameter is called `n`.
 */
static bool isParamName(FuncDef *f, const char *n) {
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, n) == 0) return true;
    return false;
}

/* A value copy does not extend the lifetime of what it copies.
 *
 * Storing a value into the place a parameter points at does not always mean that
 * something has to outlive the frame; there are two cases:
 *
 *   - a reference or a view is stored (`dst.p = ref x`, `dst.p = q`), so that storage
 *     has to live long enough => yes;
 *   - a plain value is stored (`dst.v = *p`), so the bytes are copied: how long the
 *     source object lives is irrelevant, and nothing inside the copy can outlive it
 *     => no.
 *
 * The criterion is the one `check_escape.c` uses in `typeCannotCarryRef`, including its
 * `mentionsParam` half: a type that mentions a type parameter may carry a reference, so
 * the decision is deferred to the instantiation.
 *
 * This is what decides the memory of a long-running program. The body of
 * `varArray<T>::push` is `self.buf[self.len] = v`, so the old criterion answered yes for
 * every call: every function that called `push` was marked as needing a home arena,
 * `main` needed one too, and every `new` in `main` landed in the home arena (the arena
 * the caller passes in, which lives until the process ends), so nothing was ever
 * reclaimed. The value-copy path needs not a single byte to stay behind. Measured on
 * the mixed 50/25/25 workload (half the iterations do nothing, a quarter append, a
 * quarter overwrite): 98 MB => 1 MB.
 */
/* Report whether a value expression may carry a reference.
 *
 * Do not write this as two functions that fall back to each other: the first version
 * had `valueMayCarryRef` fall back to `exprMayCarryRef` and `exprMayCarryRef` fall back
 * again, and it segfaulted on the spot (stack overflow; gdb showed 260,000 frames of
 * `valueMayCarryRef`). The structure is one recursive function answering two questions
 * ("can this type hold a reference" / "can the result of this expression"), where every
 * expression kind recurses only in its own branch, and neither question falls back to
 * the other.
 *
 * Params:
 *   v - value expression; NULL answers false
 *
 * Returns:
 *   True when the value may carry a live reference. The answer over-approximates: an
 *   opaque shape such as a binding answers true, which can cost an extra home arena but
 *   can never miss a reference that is really there.
 */
static bool valueMayCarryRef(Expr *v) {
    if (!v) return false;
    switch (v->kind) {
    /* It is itself a reference or a view. */
    case EX_REF: case EX_SLICE:
    case EX_DEREF:      /* the storage `*p` points at may hold a reference (conservative) */
    case EX_INDEX: case EX_FIELD:   /* `a[i]` / `o.f` may read out a reference */
        return true;
    /* These shapes are decided by what they hold. */
    case EX_IDENT:      return true;      /* a binding: unknown content => conservative */
    /* `dyn Trait(x)`: the payload is copied into the pool, so the references it carries are
     * carried by this value (family E). */
    case EX_DYN:        return valueMayCarryRef(v->u.dynv.payload);
    case EX_SIGN:       return valueMayCarryRef(v->u.sign.operand);
    case EX_COALESCE:   return valueMayCarryRef(v->u.coalesce.main)
                            || valueMayCarryRef(v->u.coalesce.fallback);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < v->u.lit.inits.len; i++)
            if (valueMayCarryRef((*(FieldInit **)vecAt(&v->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < v->u.arraylit.elems.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < v->u.enumval.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.enumval.args, i))) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < v->u.assoc.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.assoc.args, i))) return true;
        return false;
    case EX_CALL:
        for (size_t i = 0; i < v->u.call.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (valueMayCarryRef(v->u.method.recv)) return true;
        for (size_t i = 0; i < v->u.method.args.len; i++)
            if (valueMayCarryRef(*(Expr **)vecAt(&v->u.method.args, i))) return true;
        return false;
    default: return false;   /* scalar literal / `new` (its depth has its own case) */
    }
}

/* Does the body store into a place reached through a parameter?
 *
 * Together with `valueMayCarryRef` this decides whether the function needs a home arena:
 * a store through an out-parameter such as
 * `fn push(head: mut ref ?ref node) { *head = cell }` has to place its allocation in the
 * caller's arena, so the callee must be handed one.
 *
 * Params:
 *   c - checker
 *   s - statement to walk; may be NULL
 *   f - function whose parameters name the targets a store may go through
 *
 * Returns:
 *   True when an assignment targets a place rooted at a parameter and stores a value that
 *   may carry a reference.
 */
static bool stmtStoresThroughDeref(Checker *c, Stmt *s, FuncDef *f) {
    if (!s) return false;
    switch (s->kind) {
    case ST_ASSIGN: {
        const char *rn = placeRootName(s->u.assign.target);
        /* Writing into the place a parameter points at requires the stored value to
         * outlive this frame only when the value may carry a reference. */
        return rn && isParamName(f, rn) && valueMayCarryRef(s->u.assign.value);
    }
    case ST_IF:     return stmtStoresThroughDeref(c, s->u.ifs.thenBody, f) ||
                           stmtStoresThroughDeref(c, s->u.ifs.elseBody, f);
    case ST_WHILE:  return stmtStoresThroughDeref(c, s->u.whiles.body, f);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtStoresThroughDeref(c, *(Stmt **)vecAt(&s->u.block.stmts, i), f)) return true;
        return false;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtStoresThroughDeref(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body, f))
                return true;
        return false;
    default: return false;
    }
}

/* Which arena should a call site pass in?
 *
 * The basis is where the object the shallowest `mut ref` argument points at lives:
 *
 *   - the argument is one of my own locals, fields, or elements => pass
 *     `&__extc_a[its block depth]`, which is exact: the new object follows it;
 *   - the argument is one of my own parameters => pass my home arena (the arena my
 *     caller chose for this frame);
 *   - there is no `mut ref` argument => pass 0, which falls back to the old rule: pass
 *     the home arena when there is one, otherwise the current block.
 */
/* The argument-lifetime rule: whatever an argument points at must live at least as long
 * as the home arena of the call (the arena the caller passes in).
 *
 * Why it is needed: the callee may store the argument into its own home arena, and the
 * chained argument behind that ("an allocation that outlives every writable target can
 * be stored anywhere safely") holds only for arguments that live long enough. If the
 * caller passes a deeper local, the home arena outlives it and the stored reference
 * dangles.
 *
 *     fn bind(s: mut ref slot, target: mut ref i32) { s.r = target }
 *     var keeper: slot                              // depth 1
 *     { var n: i32 = 1  bind(ref keeper, ref n) }   // n has depth 2 > h = 1, so the
 *                                                   // call site reports an error
 */

/* The transitive closure of the effect summary.
 *
 * Why it has to exist: skipping the argument-lifetime rule is allowed only when every
 * `Addr` bit is clear, but when the callee passes the value on to another function that
 * also stores addresses, a direct scan cannot see it. "Every `Addr` bit clear" is then
 * false, so narrowing the rule would be the same as allowing a dangling reference. This
 * was caught by `tests/errors/ref_arg_too_deep` going from "must report an error" to
 * passing.
 *
 * Three disciplines:
 *   - lazy plus memoized (`effState`): computed only when it is really needed, and
 *     cached once computed;
 *   - cycle guard: a function that is being computed right now (`effState == EFF_IN_PROGRESS`) is in
 *     a cycle, so it is marked "incomplete" (conservative);
 *   - uncertainty marks it incomplete as well: `effUnknown` (a call that cannot be
 *     resolved) means never complete.
 * => A summary may be used to skip any check only when `computeEffectsTransitive`
 *    returns true.
 *
 * Params:
 *   c - checker
 *   f - function whose summary is closed and cached; NULL is not usable
 *
 * Returns:
 *   True when the summary is complete: every callee was analyzed and its summary merged
 *   in. False when a cycle, an unresolved call, or an incomplete callee leaves the
 *   summary unknown, in which case a callee has to be treated as possibly storing
 *   everything.
 */
bool computeEffectsTransitive(Checker *c, FuncDef *f) {
    if (!f) return false;
    /* An external declaration's summary is the signature that was collected for it
     * (`collectEffects` has already filled it in), so it counts as complete: we choose
     * to trust the declaration. Never let the absence of a body turn into an empty
     * summary.
     */
    if (f->isExtern) return true;
    if (f->effState == EFF_DONE) { if (c->fxOn) c->fxEffCached++; return f->effComplete; }
    if (f->effState == EFF_IN_PROGRESS) { f->effComplete = false; return false; }   /* a cycle => incomplete */
    f->effState = EFF_IN_PROGRESS;
    if (c->fxOn) c->fxEffCalls++;      /* one body entered (a memo hit returned above) */
    bool complete = !f->effUnknown;
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (c->fxOn) c->fxEffEdges++;  /* one callee edge merged */
        if (g == f) { complete = false; continue; }                   /* self-call: conservative */
        if (computeEffectsTransitive(c, g)) {                         /* merge the callee summary */
            f->addrMask     |= g->addrMask;
            f->contMask     |= g->contMask;
            f->homeAddrMask |= g->homeAddrMask;
            f->homeContMask |= g->homeContMask;
            f->otherMask    |= g->otherMask;
            f->addrFromLocal |= g->addrFromLocal;
        } else {
            complete = false;
        }
    }
    f->effComplete = complete;
    f->effState = EFF_DONE;
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects-closed] %-20s complete=%d toParam[Addr=0x%llx Cont=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] other=0x%llx\n",
                FN(f), (int)f->effComplete, (unsigned long long)f->addrMask, (unsigned long long)f->contMask,
                (unsigned long long)f->homeAddrMask, (unsigned long long)f->homeContMask, (unsigned long long)f->otherMask);
    return complete;
}

void checkCallRefArgs(Checker *c, FuncDef *callee, Vec *args, Vec *params, int homeDepth,
                       int line, const char *fname) {
    if (params->len != args->len) return;
    /* An argument only has to outlive the home arena when an address really flows.
     *
     *   - every `Addr` bit is clear in the effect summary => the callee never stored
     *     `&argument`, so the constraint does not exist. This is the `push`-like callee
     *     that only puts new things into a container, and it is still handled as the
     *     worst case today.
     *   - the answer is uncertain (an incomplete summary, or a non-empty `otherMask`)
     *     => fall back to the conservative rule.
     */
    /* This rule was narrowed once ("every `Addr` bit clear => skip the rule") and the
     * two-way criterion rejected it on the spot:
     *
     *   - evidence: `tests/errors/ref_arg_too_deep` went from "must report an error" to
     *     passing;
     *   - root cause: the effect summary had no transitive closure, so a store made one
     *     call deeper was invisible => while the summary is incomplete, "every `Addr`
     *     bit clear" is false => narrowing the rule is the same as allowing a dangling
     *     reference;
     *   - discipline: until the summary is provably complete, the argument-lifetime rule
     *     stays conservative (rejecting a safe program is better than missing undefined
     *     behavior).
     *
     * The narrowing becomes legal only with the lazy transitive closure (memo plus cycle
     * guard), and only for a summary that can be proven complete.
     */
    /* Only a summary that is proven complete may skip the conservative checks below on
     * the grounds that no address flows. */
    /* An external declaration takes a separate path: it is a black box on the C side,
     * and the effects written in its signature are the whole basis for it. "It may
     * store the argument" means that storage may live beyond this frame (C can put it
     * in a global or a static), so an argument at that position must live to depth 0.
     *
     * Without a signature every parameter counts as "stores it", so passing a local is
     * rejected: safe by default. With `effects Addr=0 Cont=0` no parameter is checked
     * at all, which is what makes such a declaration usable.
     */
    if (callee && callee->isExtern) {
        uint64_t stored = callee->addrMask | callee->contMask | callee->otherMask;
        if (stored == 0) return;                 /* the signature says nothing is stored */
        for (size_t j = 0; j < params->len && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || p->type->kind != TY_REF) continue;   /* a scalar has no lifetime */
            Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
            int d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, a);
            if (d == 0) continue;                /* it already outlives this frame */
            ckError(c, line,
                    "A C function is a black box: unless its declaration says it does not keep the"
                    " pointer, it may store it somewhere that outlives this frame. Sign it --"
                    " `effects Addr=0 Cont=0` on the `extern!`, or on the table slot that holds the"
                    " function pointer -- or pass something that lives longer.",
                    "argument %zu of `%s` may be kept by C forever, but it points into this"
                    " frame (depth %d)", j + 1, fname, d);
        }
        return;
    }

    bool complete = callee && computeEffectsTransitive(c, callee);
    /* The summary this check is about to use -- the switch that answers "why was this call
     * site rejected / why did it pass". Same shape as `EXTC_DBG_ZONE` / `EXTC_DUMP_EFFECTS`. */
    if (getenv("EXTC_DBG_REFARGS"))
        fprintf(stderr, "[refargs] %-16s mod=%-8s line=%-5d complete=%d"
                        " addr=0x%llx cont=0x%llx other=0x%llx homeAddr=0x%llx homeCont=0x%llx homeDepth=%d\n",
                fname, callee && callee->modName ? callee->modName : "-", callee ? callee->line : -1,
                (int)complete, callee ? (unsigned long long)callee->addrMask : 0,
                callee ? (unsigned long long)callee->contMask : 0,
                callee ? (unsigned long long)callee->otherMask : 0,
                callee ? (unsigned long long)callee->homeAddrMask : 0,
                callee ? (unsigned long long)callee->homeContMask : 0, homeDepth);
    /* The two checks decide independently whether to run; do not give them one shared
     * early return. Sharing that return already cost a bug: a `Cont` bit on `push` made
     * the address-flow check run as well, and `examples/list-return` was wrongly rejected
     * by an over-strict combination that the early return used to block. On the
     * escape-analysis path, `homeDepth = -1 => h = 0` is too strict for a local `mut ref`
     * argument.
     */
    bool addrMaybe = !complete || !callee
                   || callee->addrMask != 0 || callee->homeAddrMask != 0
                   || callee->addrFromLocal || callee->otherMask != 0;
    bool contMaybe = !complete || !callee
                   || callee->contMask != 0 || callee->homeContMask != 0
                   || callee->addrMask != 0 || callee->homeAddrMask != 0
                   || callee->otherMask != 0;
    if (!addrMaybe && !contMaybe) return;
    /* The bound is the DESTINATION's own measured depth (below), falling back to the
     * caller's body depth. The old `needsHome => 0` and `homeDepth < 0 => 0` guesses are
     * gone on purpose: both collapsed to "must live in the home arena" for any argument
     * living in any block, which rejected safe programs (this function's own comment used
     * to call the second one "too strict for a local `mut ref` argument"). A destination
     * that is really home-promoted reports depth 0, so h = 0 still happens when it must. */
    int h = (homeDepth > 0) ? homeDepth : (int)c->scopes.len;
    /* PRECISION: the destination is one of the `ref`-typed arguments. The callee may store a
     * value it received into any of them, so that value has to outlive every one of them:
     * take the shallowest (longest-lived) destination, never a deeper one than the caller's
     * own body. This replaces the old "the caller owns a home arena => h = 0" fallback,
     * which rejected every argument that lived in any block at all (see
     * docs/topics/CONCURRENCY.md, debt 15).
     *
     * min() can only make the requirement STRICTER than the destination really demands, so
     * nothing that was rejected for a real lifetime reason becomes accepted; the guards stay
     * `tests/errors/ref_arg_too_deep` and `tests/errors/borrowed_into_param_place`, and a
     * destination that lives forever (depth 0) still forces depth 0 on the source. */
    /* Only when the callee did NOT declare its arena (`homeDepth > 0` is an explicit
     * declaration and stays authoritative -- narrowing it further rejected four examples:
     * examples/borrowing, examples/container-of-view, examples/stream-sum,
     * tests/traps/main_question). */
    if ((addrMaybe || contMaybe) && homeDepth <= 0) {
        bool haveDest = false;
        for (size_t i2 = 0; i2 < params->len && i2 < args->len; i2++) {
            Param *p2 = *(Param **)vecAt(params, i2);
            if (!p2->type || p2->type->kind != TY_REF) continue;
            /* Only a reference to something that can **hold** references can be the destination a
             * callee stores into (a container, a plate, a record with ref fields). A `ref u8` is
             * not one: counting it let a depth-0 argument (a view of a global) collapse the
             * destination to "lives forever", which then rejected the *callee's own table*
             * (measured: `pl.viewAt(globalView.data, 4)` reported "argument 1 of `viewAt` points
             * into a deeper scope (depth 1) than the arena this call may store it in (depth 0)").
             * The predicate is the framework's own "this type can carry a reference". */
            if (!typeContainsRef(c->tt, tsub(c, p2->type->inner))) continue;
            Expr *a2 = *(Expr **)vecAt(args, i2);
            Expr *place2 = (a2->kind == EX_REF) ? a2->u.ref.operand : a2;
            /* NOTE: a generic parameter (`mut ref table<T>`) is NOT skipped here, unlike in
             * the bit-mask loops: this bound only ever LOWERS h (stricter), and the place's
             * depth does not depend on T. Skipping it left "no measurable destination" and
             * fell back to the strict 0, which is what kept rejecting the safe call. */
            /* EXACTLY the expression the store rules use for the same argument below
             * (`placeRoot ? slotDepth : targetDepth`). Mixing the two queries is what made
             * earlier attempts collapse h to 0: for a container `slotDepth` answers about
             * the 40-byte handle (1) while `targetDepth` answers about its storage, the pool
             * plate, which is born in the home arena and therefore reports 0. */
            int d2 = placeRoot(c, place2) ? slotDepth(c, place2) : targetDepth(c, place2);
            if (d2 == 0) { h = 0; haveDest = true; break; }   /* lives forever: the strictest */
            if (!haveDest || d2 < h) h = d2;
            haveDest = true;
        }
        /* Nothing measurable to store into, or a summary that is not proven complete: stay
         * strict, exactly as before this change. */
        if (!haveDest && (homeDepth < 0 || !complete)) h = 0;
    }

    for (size_t i = 0; addrMaybe && i < params->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* generic: defer */
        /* An argument is not always a place (the typical case: `stash(ref l, new node)`
         * passes a `new` directly). `slotDepth` answers 0 for a non-place, and that
         * number is read as "lives forever", so the rule would check nothing. The
         * lifetime of such an argument is taken from the level of the arena block it was
         * allocated in. */
        int d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, place);
        /* When the value is out of reach, first try to promote it (the same rule as on
         * assignment edges): if it cannot be promoted, the error below stands as it is;
         * if it can, the value really does live to this level. */
        if (d != 0 && d > h && promoteInto(c, place, h))
            d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, place);
        if (d == 0 || d <= h) continue;              /* lives long enough */
        ckError(c, line,
                "The callee may store this reference into the arena it was given, so the"
                " argument must live at least that long. Move it to a shallower scope.",
                "argument %zu of `%s` points into a deeper scope (depth %d) than the arena"
                " this call may store it in (depth %d)", i + 1, fname, d, h);
    }

    /* Content flow: the callee stores into a container either a pointer read out of
     * argument j or a value that carries references, so that data must live at least to
     * the level h of the destination as well.
     *
     * This is the borrow rule moved to the call site: the callee is compiled once and
     * does not know the caller's pool, so it only publishes the constraint, and the
     * substitution that solves it happens here.
     *
     *   - a complete summary => check only the j the summary names;
     *   - an incomplete summary (a cycle, or a call that cannot be resolved) or a
     *     non-empty `otherMask` => check every argument that carries a reference, as the
     *     worst case.
     *
     * This half is inseparable: without it, relaxing anything on the callee side would
     * be the same as allowing a dangling reference. That is the lesson from the earlier
     * attempt to narrow the argument-lifetime rule.
     */
    if (contMaybe && (!complete || (callee && callee->otherMask != 0))) {
        for (size_t j = 0; j < args->len; j++) {
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;                  /* generic: defer */
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue; /* no reference: always true */
            /* 先把"它得活到 h"这件事记成事实，**不管当下算出来的 d 是多少**：
             * `d` 是现场算的，而"这个值是不是建池的调用"（`makesPool`）要到所有函数体检查完
             * 才算得出来 —— 现场它还是 0，于是深度查询给出 0，提权那条路根本不会被走到。
             * 末轮重放（`c->lvlFacts`）在闭包之后跑，那时 `makesPool` 已经为真，
             * `promoteInto` 就能把建池的站点提到 h 那一层。重放忽略返回值，所以多记事实
             * 不会引入新报错（"提不动"的那些照旧由下面的深度判据负责）。 */
            recordLvlFact(c, a, h);
            int d = targetDepth(c, a);
            if (d != 0 && d > h && promoteInto(c, a, h)) d = targetDepth(c, a);
            if (d == 0 || d <= h) continue;
            ckError(c, line,
                    "Nothing was stored here that could be checked at this call, so the"
                    " compiler has to assume the worst: this value may end up in the place"
                    " the callee stores into. Make it live at least that long (or give the"
                    " callee a decidable body).",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d); the callee's"
                    " effects could not be fully analyzed", j + 1, fname, d, h);
        }
    } else if (contMaybe && callee) {
        /* All four bit classes must be checked here: the summary may record a by-value
         * view or a by-value argument that carries a reference as either an address-flow
         * bit or a content-flow bit, because the collector classifies by whether the
         * expression itself is a pointer or a view. For a by-value argument the two cases
         * reduce to one statement: the data it carries must live at least h.
         *
         * Checking only the content-flow bits used to accept `names.push(buf[..])` with
         * `buf` declared in a deeper block. That is a real dangling pointer, and the
         * tests caught it at once. */
        uint64_t cont = callee->contMask | callee->homeContMask
                      | callee->addrMask | callee->homeAddrMask;
        for (size_t j = 0; j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((cont >> j) & 1u)) continue;
            Param *pj = *(Param **)vecAt(params, j);
            if (pj->type && pj->type->kind == TY_REF) continue;   /* `ref` arg: checked above */
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue;
            recordLvlFact(c, a, h);         /* 同上：先记事实，末轮重放时再提（PLAN #87） */
            int d = targetDepth(c, a);
            /* PROMOTE BEFORE REJECTING -- the same half the incomplete-summary branch above
             * has always had, and the reason a container can be handed to something that
             * outlives it.
             *
             * A **fresh allocation** has no other owner, so its site can be moved to the
             * level the destination needs: `stash(ref b, p)` with `p = new i32[4]` in a
             * deeper block is safe once that `new` is promoted to `b`'s level. A **borrow**
             * cannot be moved (`names.push(buf[..])` where `buf` is a deeper block's array),
             * and `promoteInto` says so by returning false -- then the error below stands,
             * which is exactly the case this branch was added for.
             *
             * Sharing this half matters for pools as much as for arenas: a container built
             * inside a loop body and pushed into a container that outlives the loop needs its
             * pool born at the destination's place (POOLS.md 3.1, PLAN #87). */
            if (d != 0 && d > h && promoteInto(c, a, h)) d = targetDepth(c, a);
            if (d == 0 || d <= h) continue;
            ckError(c, line,
                    "The callee stores what this value points at into a container it was"
                    " given, so that data must live at least as long as that container.",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d)", j + 1, fname, d, h);
        }
    }

    /* ---- P0-17: a `mut ref` argument is write access, so the proofs about it are stale --------
     * The callee may rewrite what the place holds, so every non-null proof about that binding --
     * and about a path through it -- stops holding: `if s.top != null { s.pop() ... s.top.value }`
     * dereferenced a NULL `s.top`, because `pop` had set it and the proof was still in effect
     * (audit P0-17, ASan SEGV). This is the write-point invalidation of the proof stack, the same
     * one an assignment performs (`unNarrow`); the stack drops a path fact when its root is
     * dropped, so un-narrowing the root is enough.
     *
     * The condition is `mut ref`, not "the summary says it stores": a `mut ref` parameter is write
     * access by definition, and the summary is an upper bound on stores into the caller's
     * containers, not on writes through the reference. */
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        Sym *rs = placeRoot(c, place);
        if (rs) unNarrow(c, rs->cname);
    }

    /* ---- INV-K: the other half of a store, on the caller's side ---------------------------
     * A store has two halves and the second one was missing: the **destination** has to learn that
     * something was written into it, or the caller's binding goes on looking like a record with no
     * live references. `fn stash(out: mut ref box, v: ?ref node) { out.p = v }` called as
     * `stash(ref b, ref n)` and then `return b` was accepted, and the generated program read a
     * local of the returning frame after it died (audit P0-16, ASan heap-use-after-free).
     *
     * The summary says *which argument* was stored, never into which field, so the write goes to
     * the binding's whole-object slot (`field == NULL`). The destination is normally **addressed**
     * here -- `ref b` is what takes its address -- which is not a reason to skip it: the depth
     * update is a weak max, and `promoteFieldsAt` now promotes for an addressed root while refusing
     * to lower it. No `recordStore` here: it constrains the level solver and was the suspected
     * source of two false rejections (R1R3-LOG 补8/补9). */
    if (callee && (addrMaybe || contMaybe)) {
        uint64_t stored = callee->addrMask | callee->homeAddrMask
                        | callee->contMask | callee->homeContMask;
        for (size_t j = 0; stored && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *src = *(Expr **)vecAt(args, j);
            if (mentionsParam(src->type)) continue;
            Expr *splace = (src->kind == EX_REF) ? src->u.ref.operand : src;
            int dj = targetDepth(c, splace);
            if (dj == 0 && placeRoot(c, splace)) dj = slotDepth(c, splace);
            /* not skipped: for a reference-typed argument the level is not filled in yet */
            for (size_t i = 0; i < params->len && i < args->len; i++) {
                if (i == j) continue;
                Param *pp = *(Param **)vecAt(params, i);
                if (!pp->type || pp->type->kind != TY_REF || !pp->type->mut) continue;
                Expr *a = *(Expr **)vecAt(args, i);
                Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
                Sym *dsym = placeRoot(c, place);
                if (!dsym) continue;
                if (dj != 0) {
                    noteFieldSrc(dsym, NULL, dj, splace);
                    noteFieldDepthWrite(c, dsym, NULL, dj);
                }
                /* The dedicated slot (audit P0-16d). No `recordLvlFact` here: constraining the level
                 * for *every* `mut ref` call was the first suspect for the three false rejections the
                 * full version produced (`tests/pool` rt_nest/rt_rett, `tests/stl` custom). */
                /* Publish the dedicated slot when the level is known, **or** when the source is a
                 * promotable fresh allocation -- those are the shapes whose site can really be moved,
                 * which is what `return b` needs (audit P0-16d: `var n: mut ref node = new node`).
                 * An unknown level whose source cannot be promoted gets no slot: publishing it added
                 * promotion pressure and nothing else, and that is what rejected `tests/pool`'s
                 * rt_nest/rt_rett and `tests/stl`'s custom when the publication was unconditional. */
                Sym *ss = placeRoot(c, splace);
                bool promotable = ss && ss->origin && exprIsFresh(ss->origin);
                if (dj != 0 || promotable) {
                    if (!dsym->callSrc) dsym->callSrc = splace;
                    if (dj > dsym->callSrcDepth) dsym->callSrcDepth = dj;
                }
            }
        }
    }

}

/* Choose the arena for a call result by asking whether the result escapes this block.
 *
 * The callee allocates into the arena it is handed. Passing my home arena (the arena
 * the caller of this function passes) for every call meant that a call inside a loop
 * accumulated one round of data per iteration, with nothing reclaimed until the frame
 * ended. A stress run on 2026-09-20 measured a 300-round loop building a 2001-node
 * list: peak 15.3 MB against 5.9 MB for the same program in C (about 14.4 MB, which
 * the round count accounts for).
 *
 * The fix looks at how deep the result is stored:
 *   - stored in the current block (`var l = build(...)` in the loop body): pass the
 *     current block's arena, so each round is reclaimed when the block ends and the
 *     peak stays constant;
 *   - stored in a shallower place or handed back (`return build(...)`, assignment to an
 *     outer local): pass my home arena;
 *   - an argument position (`g(build(...))`) is deliberately left unmarked and falls
 *     back to the old rule, which passes the home arena.
 *
 * Params:
 *   c  - checker (scope stack)
 *   v  - the call expression whose result is being stored; anything else is ignored
 *   at - scope depth of the place that receives the result; 0 = the result is returned,
 *        so it must outlive this frame
 *
 * Notes:
 *   - A destination shallower than the current scope count means the result outlives
 *     this block, and `homeDepth` is set to -1, which makes `setCallArenaArg` pass
 *     ARENA_HOME. Otherwise `homeDepth` is the depth of the block the result stays in.
 *   - `homeDepth` and `arenaArg` must be updated together; codegen reads `arenaArg`
 *     only, so run `setCallArenaArg` right after setting `homeDepth`.
 */
void markCallHomeIfEscaping(Checker *c, Expr *v, int at) {
    if (!v) return;
    if ((v->kind != EX_CALL && v->kind != EX_METHOD) || !v->func) return;
    if (!v->func->needsHome) return;
    v->homeDepth = (at < (int)c->scopes.len) ? -1 : (int)c->scopes.len;
    setCallArenaArg(c, v);          /* always keep `homeDepth` and `arenaArg` in step */
}

/* Resolve `homeDepth` into the arena the call actually passes (`Expr.arenaArg`).
 *
 * The checker decides this value here; codegen only has to print it.
 *
 * Why two fields: `homeDepth` is the criterion used by the reference-argument check,
 * and there the out-of-this-frame case is deliberately looked up as depth 0, which
 * prefers a false rejection. `arenaArg` is the real choice: -1 passes my home arena
 * (the arena the caller of this function passes), a positive value passes that block's
 * arena, and 0 passes the current block, which is tighter and uses less memory.
 *
 * Params:
 *   c - checker (scope stack)
 *   e - call expression whose `homeDepth` has already been set
 *
 * Notes:
 *   - Both fields are written here so they cannot drift apart. Codegen must not fall
 *     back to `g->hasHome`: that would be a second authority for one decision, which is
 *     why the choice used to be made on both sides.
 *   - The 0 case has no `mut ref` argument to reason from, so the site is recorded with
 *     `arenaArgPending` set and resolved by the final pass.
 */
/* 建池的调用点：池该生在**哪一层地方**（与 `setCallArenaArg` 平行的一格）。
 *
 * 规则两条：
 *   - 我在**库函数**里（`modName` 非空）⇒ 库函数对"地方"是透明的（它自己不压 zone），
 *     所以这里没有"我的地方"可言：把选择权交给调用者（`ZONE_HOME`，隐藏参数往下传）。
 *     `vector<i32>::new` → `withCap` → 运行期那一句 `extc_pool_new_at` 因此拿到的是
 *     **用户**选的那个地方。
 *   - 我在入口文件的函数里 ⇒ 默认是当前这个块（今天的语义：池生在建它的那个块里）；
 *     提权（`promoteInto`）之后这个数会被改小，池就生到更长寿的地方去。
 */
/* 这个被调者会不会建池 —— 读取处统一走这里。
 *
 * 为什么不能只看 `planMakesPool(f)`：泛型方法的**模板**与**实例**是两份 FuncDef，闭包只走到
 * 模板那一份，而调用点上 `e->func` 可能是模板（实测 `owner=vector$vector makesPool=0`，
 * 同一处 codegen 读到的实例却是真）。结构体那一格 `makesPoolAny` 是闭包顺手算的，
 * 只对**关联函数**（构造函数那一族：`new` / `withCap` / `withParent`）放宽 ——
 * 别的库方法（`get` / `asSlice` 之类）不进这一族，免得把"不可提权"的值误判成站点。 */
bool calleeMakesPool(FuncDef *f) {
    if (!f) return false;
    /* **声明优先**（作者口径 2026-09-26）：`@poolObject` 标在 struct 上，说"这个类型拥有一个池"。
     * 它比推断可靠 —— 早先试过"看有没有 `pid` 字段"（魔数）与"看方法建不建池"（要等闭包）。 */
    if (f->owner && f->owner->poolObject) return true;
    if (planMakesPool(f)) return true;
    /* 运行期那一句**本身**就是池站点：`extc_pool_new`（extern，没有函数体 ⇒ 闭包标不到它，
     * 所以它按名字认，与 codegen 里把 `extc_pool_new` 改写成 `_at` 的那处同一个名字）。
     *
     * 少了这一条的后果（实测）：用户**在自己文件里**写一个池底容器时，构造器里的这一次
     * `extc_pool_new` 提不了权 ⇒ 池生进构造器自己的帧 ⇒ 返回时被回收 ⇒ 段错误。
     * 库里的容器没这个毛病，只因为库函数"对地方透明"；用户的文件不是库，
     * 于是那条隐藏的差别就露出来了。 */
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return f->isAssoc && f->owner && f->owner->makesPoolAny;
}

/* 「**这一次调用能不能走到 `extc_pool_new`**」—— 与 `calleeMakesPool` 问的**不是同一个问题**。
 *
 * 两个问题一直挤在同一只谓词里：
 *   ① 「这个类型**拥有**池」是**类型**的性质（`@poolObject` 声明）；
 *   ② 「这一次调用**会不会建**池」是**被调者**的性质（`planMakesPool(f)` + 构造器那一族）。
 * `calleeMakesPool` 要回答①（"结果会不会住在池上" ⇒ 逃逸深度与提权站点），所以它对
 * `@poolObject` 类型**一律**放宽 —— 在那里这是对的（保守方向）。
 * 但**块级的 zone 钩子**只需要②：一个池只在建它的那个块的 zone 上登记。拿①回答②的代价
 * 实测过（`bench/app/session.extc`，active=1M）：`hashMap::get` 被当成"会建池" ⇒ 循环体
 * **每轮压/弹一次 zone**。只把生成物里那两行删掉的消融实验值是 **25.3%**（42.34 → 31.63
 * ns/op）；**编译器这一刀端到端是 14.5%**（48.08 → 41.10 ns/op）—— 后者还顺带把那几个
 * 方法的 zone 形参去掉了。修完 extC 在该场景**与 Go 持平**（0.99×，之前 1.15×）。
 *
 * 剩下三条覆盖"建池"的全部入口：闭包（`makesPool`）覆盖一切能走到 `extc_pool_new` 的函数体；
 * `extc_pool_new*` 自己按名字认（extern 没有函数体）；构造器那一族按 `makesPoolAny` 认
 * （模板/实例两份 FuncDef 的口子）。未解析的被调者仍然**保守为真**。 */
bool calleeCreatesPool(FuncDef *f) {
    if (!f) return true;                       /* 未解析 ⇒ 保守 */
    if (planMakesPool(f)) return true;
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return f->isAssoc && f->owner && f->owner->makesPoolAny;
}

void setCallZoneArg(Checker *c, Expr *e) {
    if (!e) return;
    int lvl = (c->curFunc && c->curFunc->modName && *c->curFunc->modName)
                  ? ZONE_HOME : (int)c->scopes.len;
    /* **只往下调，不往上抬**：同一个节点可能被检查不止一次（泛型实例、末轮那一类重走），
     * 而提权已经把这一格改小了；再按"当前块"覆盖一遍就等于把提权抹掉。
     * 越小越长寿，所以取更小的那个。 */
    if (planZoneLevel(e) == 0 || lvl < planZoneLevel(e)) planSetZoneLevel(e, lvl);
}

void setCallArenaArg(Checker *c, Expr *e) {
    if (!e) return;
    if (e->homeDepth == -1) {                 /* destination at the home level: my home arena */
        planSetArenaArg(e, ARENA_HOME);
        e->arenaArgPending = false;
    } else if (e->homeDepth >= 1) {           /* an explicit block level: use that arena */
        planSetArenaArg(e, e->homeDepth);
        e->arenaArgPending = false;
    } else {
        /* No `mut ref` argument gave a reason, so the old rule applies: pass my home
         * arena if I have one, otherwise the current block.
         *
         * Whether I have a home arena is decided by the transitive closure of
         * `needsHome`, which is only known after every function body has been checked.
         * The site is therefore recorded as the current block with `arenaArgPending`
         * set, and the final pass settles it (see `curArenaSites`).
         *
         * This is the job the `if (g->hasHome) return "__extc_home";` line used to do in
         * codegen. It moved into the checker so the decision is made in exactly one
         * place instead of once on each side. */
        planSetArenaArg(e, (int)c->scopes.len);
        e->arenaArgPending = true;
        *(Expr **)vecPush(&c->curArenaSites) = e;   /* the final pass comes back to this site */
    }
}

/* Depth of the arena a call must pass, read off its `mut ref` arguments.
 *
 * The callee's allocations must live at least as long as anything it can store them into,
 * so the shallowest object a `mut ref` argument points at decides the arena. An argument
 * that is one of my own parameters, or a global, already lives beyond this frame, so the
 * caller's home arena is passed instead.
 *
 * Params:
 *   c        - checker
 *   args     - argument expressions in call order
 *   params   - declared parameters of the callee, same order
 *   callNode - the call expression, recorded when the answer depends on the escape
 *              analysis; may be NULL, and then nothing is recorded
 *
 * Returns:
 *   Depth of the arena to pass; 0 when no `mut ref` argument was found, -1 when the
 *   caller's home arena has to be passed.
 */
int callHomeDepth(Checker *c, Vec *args, Vec *params, Expr *callNode) {
    int best = 0;                       /* 0 = no `mut ref` argument was found */
    /* Arguments whose answer depends on the escape set E must be recorded and
     * recomputed once the effect summaries are closed (see the `EArenaSite` comment). */
    EArenaSite *rec = NULL;
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        int d = slotDepth(c, place);   /* a parameter reports 0, a local its block depth */
        /* What the record keeps: 0 = a parameter or a global (outside this frame), -1 = an
         * escaping local (also outside, for good), k >= 1 = a block of this body. */
        int recD = d;
        const char *rn = placeRootName(place);
        if (d == 0) d = -1;             /* the place is in a parameter: pass my home arena */
        else if (rn && isEscapeeName(c, rn)) { d = -1; recD = -1; }
        /* Record the site in **both** cases, including the `d == 0` one.
         *
         * The arena answer for a parameter is already final -- the caller's home arena is
         * the longest-lived one there is -- but the **zone** answer is not: the zone is only
         * passed on when the callee allocates (`makesPool`), and `makesPool` is a closure,
         * so at this moment a callee that allocates only through its own callees still reads
         * as "no". Without a record here the final pass never sees the site, the zone keeps
         * its provisional level, and a call chain of two hops passes its own place down --
         * that is H2 (`tests/arena-soundness/H2_home_zone_two_hops.extc`). */
        if (callNode && c->eSites.arena) {
            if (!rec) {
                rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                rec->call = callNode;
                *(EArenaSite **)vecPush(&c->eSites) = rec;
            }
            EXTC_DBG_ASSERT(rec->n <= 8);   /* the slot count the struct declares */
            if (rec->n < 8) {
                rec->argRoot[rec->n]  = (char *)rn;
                rec->argDepth[rec->n] = recD;
                rec->n++;
            } else {
                rec->overflow = true;   /* conservative final pass; no silent truncation */
            }
        }
        if (best == 0 || d < best) best = d;
    }
    return best;
}


/* True when the vector holds the given name.
 *
 * Params:
 *   v - vector of `const char *`
 *   n - name to look for
 *
 * Returns:
 *   True when some element compares equal to `n`.
 */
static bool vecHasName(Vec *v, const char *n) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), n) == 0) return true;
    return false;
}
/* Is this expression a block out of a **pool plate**?
 *
 * `poolSlice<T>(rid, n)` / `poolSliceRaw<T>(rid, n)` take a fresh block; `poolResize<T>` /
 * `poolResizeRaw<T>` grow or shrink the block already held. Either way the memory belongs to
 * the pool the *container* owns, not to anything the caller passed: storing such a block
 * publishes no caller reference, so it constrains no argument's lifetime -- the same
 * exemption `new` gets. SSO `string` is what needs it: `grow` stores the block into
 * `self.big`, and without this the whole `push`/`append` chain acquires `otherMask`
 * ("stores something unattributable"), after which **every** call site of a function that
 * appends to a string is required to pass arguments that live outside the frame
 * (measured: `tests/coro/statemachine.extc` was rejected at `step(ref f, ref st, …)`).
 *
 * Params:
 *   e - expression; may be NULL
 *
 * Returns:
 *   True when the value is a pool plate block.
 */
static bool exprIsPoolBlock(Expr *e) {
    if (!e || e->kind != EX_GENCALL || !e->u.gencall.name) return false;
    const char *n = e->u.gencall.name;
    return strcmp(n, "poolSlice")     == 0 || strcmp(n, "poolSliceRaw") == 0 ||
           strcmp(n, "poolResize")    == 0 || strcmp(n, "poolResizeRaw") == 0;
}

/* Is the initial value something freshly allocated in this frame?
 *
 * A `new` counts, a block out of a pool plate counts (see `exprIsPoolBlock`), and so does a
 * struct literal whose every field is itself fresh or a scalar.
 *
 * Params:
 *   e - initializer expression; may be NULL
 *
 * Returns:
 *   True when the value is a fresh allocation, or a literal built only from such values.
 */
static bool exprIsFresh(Expr *e) {
    if (!e) return false;
    if (e->kind == EX_NEW) return true;
    if (exprIsPoolBlock(e)) return true;
    if (e->kind == EX_STRUCTLIT) {
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (!exprIsFresh((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return false;
        return true;
    }
    return false;
}
/* Collect the names of locals whose initializer is a fresh allocation.
 *
 * Params:
 *   a     - unused; only forwarded to the recursive calls
 *   fresh - output: collected names are appended here
 *   s     - statement to walk; blocks, if/else, while, and match arms are followed
 *
 * Notes:
 *   - Only the direct form `var x = new ...` counts. Alias propagation is deliberately
 *     left out, so a name that is fresh only through an alias is not recognized, which
 *     is the conservative direction: a missing name only keeps a requirement that could
 *     have been dropped.
 */
/* The innermost loop each allocation line sits in, and the block level of that loop body.
 *
 * `--explain-memory` needs both, and the level has to be the level of the **loop body**,
 * not of the block the `new` happens to be written in. An allocation is released once per
 * round exactly when it lands in the loop body's arena or deeper; anything shallower is
 * still live when the next round starts, so its memory grows with the iteration count.
 *
 * Measuring against the worst level read as growing although its peak stays at 13 MB,
 * because its `new` sits in a nested block one level below the loop body and that block
 * still ends within the round. The loop body's level is taken as the smallest
 * `lexicalLevel` among the sites in that loop, since a nested block only ever reports a
 * deeper one.
 *
 * Records are (line, loopId) per site and (loopId, level) per loop. */
static void collectLoopSites(Stmt *s, int loopId, int *nextLoop, Vec *sites) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        if (loopId && exprHasNew(s->u.trap_.msg)) {
            *(int *)vecPush(sites) = s->u.trap_.msg->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.trap_.msg->lexicalLevel;
        }
        return;
    case ST_YIELD:
        if (loopId && exprHasNew(s->u.yield_.value)) {
            *(int *)vecPush(sites) = s->u.yield_.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.yield_.value->lexicalLevel;
        }
        return;
    case ST_DOMAIN:
        /* A domain block is not a loop: it introduces no new loop id, so the enclosing one is
         * passed straight through and only the body is walked. */
        collectLoopSites(s->u.domain_.body, loopId, nextLoop, sites);
        return;
    case ST_WHILE: {
        Stmt *b = s->u.whiles.body;
        if (!b) return;
        int id = (*nextLoop)++;
        collectLoopSites(b, id, nextLoop, sites);
        return;
    }
    case ST_VAR:
        if (loopId && exprHasNew(s->u.var.init)) {
            *(int *)vecPush(sites) = s->u.var.init->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.var.init->lexicalLevel;
        }
        return;
    case ST_ASSIGN:
        if (loopId && exprHasNew(s->u.assign.value)) {
            *(int *)vecPush(sites) = s->u.assign.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.assign.value->lexicalLevel;
        }
        return;
    case ST_EXPR:
        if (loopId && exprHasNew(s->u.expr.expr)) {
            *(int *)vecPush(sites) = s->u.expr.expr->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.expr.expr->lexicalLevel;
        }
        return;
    case ST_RETURN:
        if (loopId && exprHasNew(s->u.ret.value)) {
            *(int *)vecPush(sites) = s->u.ret.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = s->u.ret.value->lexicalLevel;
        }
        return;
    case ST_IF:
        collectLoopSites(s->u.ifs.thenBody, loopId, nextLoop, sites);
        collectLoopSites(s->u.ifs.elseBody, loopId, nextLoop, sites);
        return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectLoopSites(*(Stmt **)vecAt(&s->u.block.stmts, k), loopId, nextLoop, sites);
        return;
    case ST_MATCH:
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectLoopSites((*(MatchArm **)vecAt(&s->u.match.arms, k))->body, loopId, nextLoop, sites);
        return;
    default: return;
    }
}

/* Where an allocation site ends up, in words a reader can use.
 *
 * The levels are the same numbers the checker decided with; naming them is the point of
 * the report, because "level 0" alone does not tell a reader whether that is good news. */

/* `--explain-memory`: one line per allocation site, with the numbers behind the decision.
 *
 * The report exists because the checker knows all of this and none of it is visible: a
 * program that holds memory does so because of a specific allocation site, and nothing in
 * the source says which. Every site the checker decided on is listed with the level it
 * landed at, the shallowest level it was ever published to, and whether it sits in a loop.
 *
 * `minAt == 0` means the site was stored into something that outlives the frame. That is
 * the one verdict the numbers support on their own, and it is what `@overwrite` is for, so
 * those lines are called out; the rest are printed as data. An earlier version of the
 * report also claimed "memory grows with the iteration count" for any site in a loop whose
 * level was shallower than the loop body, which was wrong: `bench/gc/src/rebuild.extc` was
 * reported as growing while measuring it at 2000, 20000 and 200000 rounds gives 1.7 MB
 * every time. The level a site lands at and the point where its block is released are
 * decided by different rules, so the report no longer guesses at the second one. */
/* The loop that holds this line, or 0 when no loop does.
 *
 * The site table is (line, loopId, lexicalLevel) triples. */
static int loopIdOf(const Vec *sites, int line) {
    for (size_t i = 0; i + 2 < sites->len; i += 3)
        if (*(int *)vecAt((Vec *)sites, i) == line) return *(int *)vecAt((Vec *)sites, i + 1);
    return 0;
}

static void reportMemory(Checker *c, Vec *all) {
    int nSite = 0, nLoop = 0;
    fprintf(stderr, "extC memory report\n");
    fprintf(stderr, "------------------\n");
    fprintf(stderr, "  One line per allocation site, with the numbers the checker decided on.\n");
    fprintf(stderr, "  `level` is the arena the site was placed in (0 = this frame, -1 = the\n");
    fprintf(stderr, "  caller's); `lexical` is the block it is written in. A site whose level\n");
    fprintf(stderr, "  is shallower than its own block is not released by that block.\n");
    fprintf(stderr, "  (Lines under `prelude:` are in stdlib/prelude.extc, not in %s.)\n",
            (c->ctx && c->ctx->path) ? c->ctx->path : "?");
    for (size_t i = 0; i < all->len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(all, i);
        if (!f->arenaSites.len) continue;
        Vec sites;
        vecInit(&sites, c->arena, sizeof(int));
        int nextLoop = 1;
        collectLoopSites(f->body, 0, &nextLoop, &sites);
        fprintf(stderr, "\n  %s\n", f->name ? f->name : "?");
        for (size_t j = 0; j < f->arenaSites.len; j++) {
            Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
            if (!site) continue;
            nSite++;
            bool inLoop = loopIdOf(&sites, site->line) != 0;
            if (inLoop) nLoop++;
            fprintf(stderr, "    line %-5d level %-3d lexical %-3d  %s\n",
                    site->line, planArenaLevel(site), site->lexicalLevel,
                    inLoop ? "allocated once per round of a loop" : "allocated outside any loop");
        }
    }
    if (!nSite) {
        fprintf(stderr, "\n  no allocation site in this module\n");
        return;
    }
    fprintf(stderr, "\n  %d allocation site(s), %d of them inside a loop\n", nSite, nLoop);
    /* What this report does not do, yet.
     *
     * Whether a site's memory actually grows with the iteration count needs the level of
     * the block that releases the site's arena, and that is not derivable from the numbers
     * printed here by a predicate over them: three attempts were measured against programs
     * whose peak is known (four /tmp/mem_*.extc shapes, and bench/gc/rebuild measured at
     * 2000 through 2000000 rounds) and every one of them either missed the shape that grows
     * to 159 MB or flagged one that stays at 1.7 MB. The reliable form is to record, at code
     * generation time, which loop releases which arena; until then this report prints the
     * facts and claims nothing. */
    fprintf(stderr, "  (this report prints facts only: it does not yet say which sites grow)\n");
}

/* Drop a name from the fresh set. The set is a plain name vector and stays small (locals of one
 * function), so a compaction is cheaper than any index. */
static void freshDrop(Vec *fresh, const char *name) {
    if (!fresh || !name) return;
    size_t w = 0;
    for (size_t r = 0; r < fresh->len; r++) {
        const char *n = *(const char **)vecAt(fresh, r);
        if (strcmp(n, name) == 0) continue;
        *(const char **)vecAt(fresh, w++) = n;
    }
    fresh->len = w;
}

static void collectFreshLocals(Arena *a, Vec *fresh, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (exprIsFresh(s->u.var.init)) *(const char **)vecPush(fresh) = s->u.var.name;
        return;
    case ST_ASSIGN: {
        /* A name stops being fresh the moment an assignment gives it a value that is not itself
         * fresh: `n = id(target)` no longer points at the `new i32` its declaration made. Keeping
         * the name made `classifyStoredValue` answer "freshly allocated, so trivially safe" for a
         * value that now points into the caller's frame, so `stash`'s summary recorded nothing,
         * `effComplete` stayed true, and the whole call-site lifetime check was skipped -- a real
         * stack-use-after-scope (audit P1-5). This is the write-point invalidation of the fresh
         * set; the fresh-value case keeps the name, so `p = new i32` does not lose it. */
        /* Only a binding that is **retargeted** stops being fresh. A field write such as
         * `n.value = v` writes *through* the binding: `n` still points at the object its
         * declaration made. Dropping it there removed the fresh answer for a value that really is
         * fresh, and the call-site check then rejected `stash(dest, ref deep, v)` -- the accepted
         * and ASan-clean program of audit P0-2 -- which is how this narrowing was found. */
        if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
            const char *rn = placeRootName(s->u.assign.target);
            if (rn && !exprIsFresh(s->u.assign.value)) freshDrop(fresh, rn);
        }
        return;
    }
    case ST_IF:    collectFreshLocals(a, fresh, s->u.ifs.thenBody);
                   collectFreshLocals(a, fresh, s->u.ifs.elseBody); return;
    case ST_WHILE: collectFreshLocals(a, fresh, s->u.whiles.body); return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectFreshLocals(a, fresh, *(Stmt **)vecAt(&s->u.block.stmts, k));
        return;
    case ST_MATCH:
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectFreshLocals(a, fresh, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body);
        return;
    default: return;
    }
}


/* Escape analysis: which locals can be moved out of this function?
 *
 * Why it exists: what a callee allocates lives in its home arena (the arena the caller
 * passes), and that arena has to live longer than anywhere the contents of a container
 * can go. The call site therefore has to know whether an argument can leave this
 * function.
 *
 * Rules (the conservative direction is to over-approximate: an extra name only costs
 * memory, a missing name leaves a dangling pointer):
 *   - `return e`: every name appearing in `e` enters E, whether or not it is really
 *     copied out;
 *   - `x = e` or `var x = e` where `x` is already in E: the names in `e` enter E;
 *   - anything stored into a container a parameter points at: those names enter E,
 *     because that container lives longer.
 * E is closed as a fixpoint: the body is walked until the set stops growing, which
 * terminates because the body is finite.
 *
 * Params:
 *   c - checker; the escape set is `c->escapees`
 *   n - name of a binding
 *
 * Returns:
 *   True when the name is in E, so what it holds may leave this frame.
 */
bool isEscapeeName(Checker *c, const char *n) {
    if (!n) return false;
    for (size_t i = 0; i < c->escapees.len; i++)
        if (strcmp(*(const char **)vecAt(&c->escapees, i), n) == 0) return true;
    return false;
}
/* Add a name to the escape set.
 *
 * Params:
 *   c - checker
 *   n - name to add; may be NULL
 *
 * Returns:
 *   True when the name was not present yet, which is what drives the fixpoint iteration.
 */
static bool addEscapee(Checker *c, const char *n) {
    if (!n || isEscapeeName(c, n)) return false;
    *(const char **)vecPush(&c->escapees) = n;
    return true;
}

/* Add every name appearing in the expression to E; true when a new name was added,
 * which is what the fixpoint loop uses. */
static bool markNamesInExpr(Checker *c, Expr *e);
static bool markNamesInStmt(Checker *c, FuncDef *f, Stmt *s) {
    if (!s) return false;
    bool grew = false;
    switch (s->kind) {
    case ST_TRAP:
        /* The message is read in this frame; nothing in it leaves. */
        return markNamesInExpr(c, s->u.trap_.msg);
    case ST_YIELD:
        /* The value handed to the resumer leaves this frame just like a `return` does. */
        return markNamesInExpr(c, s->u.yield_.value);
    case ST_RETURN:
        return markNamesInExpr(c, s->u.ret.value);          /* everything handed back escapes */
    case ST_VAR:
        /* Propagate only when the declared name itself escapes. Propagating
         * unconditionally made E hold every name that ever appears, so everything
         * escaped, which wasted memory and changed every golden output; fixed. */
        if (isEscapeeName(c, s->u.var.name)) grew |= markNamesInExpr(c, s->u.var.init);
        return grew;
    case ST_ASSIGN: {
        /* Propagate only when the target is in E or is a container a parameter points
         * at. Propagating on every assignment made E too large. */
        const char *rn = placeRootName(s->u.assign.target);
        if (rn && (isEscapeeName(c, rn) || paramIndexByName(f, rn) >= 0))
            grew |= markNamesInExpr(c, s->u.assign.value);
        return grew;
    }
    case ST_DOMAIN:
        /* The receiver (`d.run`) is a domain object, not a name that escapes; the block's
         * body is ordinary code and gets exactly the treatment a block gets. */
        grew |= markNamesInStmt(c, f, s->u.domain_.body);
        return grew;
    case ST_IF:
        /* Names in the condition do not count as escaping. Adding them unconditionally
         * made E too large, so the home arena changed all over the place and
         * out-parameters were falsely rejected. */
        grew |= markNamesInStmt(c, f, s->u.ifs.thenBody);
        grew |= markNamesInStmt(c, f, s->u.ifs.elseBody);
        return grew;
    case ST_WHILE:
        /* As above: the loop condition does not count as escaping. */
        grew |= markNamesInStmt(c, f, s->u.whiles.body);
        return grew;
    case ST_EXPR:
        /* A local published through a call. This used to return an unconditional false,
         * which cost two real use-after-free bugs of this shape: a local published
         * through a call never entered E, so `callHomeDepth` took the receiver for depth
         * 1, the caller's frame, and the arena argument became the current frame's arena,
         * the callee's own frame, instead of the home arena. Whatever the callee stored
         * into that container was then released when this call returned.
         *
         * Marking every call is not an option either: a plain read such as `println(x)`
         * was marked too, which produced six false rejections (including `borrowing`,
         * `container-of-view`, `ref-field-write`, `borrowed-stash-sameframe`, and
         * `G_stale_origin`). So only the arguments of callees that can really publish
         * them are marked.
         *
         * The criterion is the existing effect summary: any bit that says it stores
         * somewhere (`addrMask`, `contMask`, `otherMask`, or their home variants) means
         * this callee's arguments may be published, so they are marked. A clean summary
         * (as for `println`) leaves them unmarked, which keeps the precision.
         *
         * The summary is an upper bound on whether a store happens at all, so answering
         * that it may store is the safe direction. */
        /* `out.put(x)?` is an **`EX_TRY` around the call**: the statement's top-level kind is
         * therefore neither `EX_CALL` nor `EX_METHOD`, so it fell into the `default` below and the
         * call's arguments never entered E. `callHomeDepth` then took the receiver for depth 1,
         * the arena argument became this frame's arena instead of the home arena, and whatever the
         * callee stored was released when the call returned -- audit P0-1, heap-use-after-free,
         * with a one-character difference from the accepted twin (`out.put(x)` without the `?`).
         * The `?` decides what happens *after* the call, not what the call can publish, so unwrap
         * it and ask the same question. */
        {
        Expr *se = s->u.expr.expr;
        while (se && se->kind == EX_TRY) se = se->u.try_.operand;
        if (!se) return false;
        switch (se->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: {
            FuncDef *cf = se->func;
            if (!cf) return false;
            uint64_t pub = cf->addrMask | cf->contMask | cf->otherMask
                         | cf->homeAddrMask | cf->homeContMask;
            if (cf->addrFromLocal) pub |= 1u;
            /* An incomplete summary must never be read as proof that nothing is
             * stored: when the body contains a call that cannot be resolved,
             * `effComplete` stays false forever. So a callee with any `mut ref`
             * parameter, which is an entry point for a store, is treated as one that may
             * store. The direction is safe: marking more only costs memory. */
            if (!cf->effComplete) {
                for (size_t pi = 0; pi < cf->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&cf->params, pi);
                    if (pp->type && pp->type->kind == TY_REF && pp->type->mut) { pub |= 1u; break; }
                }
            }
            if (pub == 0) return false;        /* the callee stores nothing (e.g. `println`) */
            return markNamesInExpr(c, se);
        }
        default:
            return false;                      /* other expression statements do not escape */
        }
        }
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            grew |= markNamesInStmt(c, f, *(Stmt **)vecAt(&s->u.block.stmts, k));
        return grew;
    case ST_MATCH:
        /* As above: the match subject does not count as escaping. */
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            grew |= markNamesInStmt(c, f, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body);
        return grew;
    default: return false;
    }
}
/* Add every name occurring in an expression to the escape set.
 *
 * Params:
 *   c - checker
 *   e - expression to walk; may be NULL
 *
 * Returns:
 *   True when at least one new name was added.
 */
static bool markNamesInExpr(Checker *c, Expr *e) {
    if (!e) return false;
    bool grew = false;
    switch (e->kind) {
/* `dyn Trait(x)`: the payload is a child expression (ast.h: `dynv.payload`), so every
     * walker has to look inside it -- the walkers end in `default:`, which is why `-Wswitch`
     * never pointed at the omission. */
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.gencall.args, i));
        return grew;
    case EX_DYN: return markNamesInExpr(c, e->u.dynv.payload);
    case EX_IDENT: return addEscapee(c, e->u.ident.name);
    case EX_BIN:   grew |= markNamesInExpr(c, e->u.bin.left);
                   grew |= markNamesInExpr(c, e->u.bin.right); return grew;
    case EX_UN:    return markNamesInExpr(c, e->u.un.operand);
    case EX_REF:   return markNamesInExpr(c, e->u.ref.operand);
    case EX_EXT: return markNamesInExpr(c, e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return markNamesInExpr(c, e->u.deref.operand);
    case EX_SIGN:  return markNamesInExpr(c, e->u.sign.operand);
    case EX_CONV:  return markNamesInExpr(c, e->u.conv.operand);
    case EX_TRY:   return markNamesInExpr(c, e->u.try_.operand);
    case EX_SLICE: grew |= markNamesInExpr(c, e->u.slice.obj);
                   grew |= markNamesInExpr(c, e->u.slice.lo);
                   grew |= markNamesInExpr(c, e->u.slice.hi); return grew;
    case EX_FIELD: return markNamesInExpr(c, e->u.field.obj);
    case EX_INDEX: grew |= markNamesInExpr(c, e->u.index.obj);
                   grew |= markNamesInExpr(c, e->u.index.index); return grew;
    case EX_COALESCE:
        grew |= markNamesInExpr(c, e->u.coalesce.main);
        grew |= markNamesInExpr(c, e->u.coalesce.fallback); return grew;
    case EX_NEW:   return markNamesInExpr(c, e->u.new_.count);
    case EX_CALL:
        for (size_t k = 0; k < e->u.call.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.call.args, k));
        return grew;
    case EX_METHOD:
        grew |= markNamesInExpr(c, e->u.method.recv);
        for (size_t k = 0; k < e->u.method.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.method.args, k));
        return grew;
    case EX_ASSOC:
        for (size_t k = 0; k < e->u.assoc.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.assoc.args, k));
        return grew;
    case EX_ENUMVAL:
        for (size_t k = 0; k < e->u.enumval.args.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.enumval.args, k));
        return grew;
    case EX_STRUCTLIT:
        for (size_t k = 0; k < e->u.lit.inits.len; k++)
            grew |= markNamesInExpr(c, (*(FieldInit **)vecAt(&e->u.lit.inits, k))->value);
        return grew;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t k = 0; k < e->u.lambda.inits.len; k++)
            grew |= markNamesInExpr(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, k))->value);
        return grew;
    case EX_ARRAYLIT:
        for (size_t k = 0; k < e->u.arraylit.elems.len; k++)
            grew |= markNamesInExpr(c, *(Expr **)vecAt(&e->u.arraylit.elems, k));
        return grew;
    default: return false;
    }
}

/* Fixpoint closure: whatever is moved out takes its contents with it. */
static void computeEscapes(Checker *c, FuncDef *f);
static void computeEscapesMode(Checker *c, FuncDef *f, bool unionMode) {
    if (!c->escapees.arena) vecInit(&c->escapees, c->arena, sizeof(const char *));
    if (!unionMode) c->escapees.len = 0;     /* union mode: keep the names already collected */
    if (unionMode) {
        /* Union mode: iterate until the set stops growing. `isEscapeeName` reports a name
         * that is already in E as present, so the loop has to run round after round until
         * nothing changes. */
        for (int round = 0; round < 64; round++)
            if (!markNamesInStmt(c, f, f->body)) break;
    } else {
        for (int round = 0; round < 32; round++)
            if (!markNamesInStmt(c, f, f->body)) break;   /* stop when the set stops growing */
    }
    if (getenv("EXTC_DUMP_EFFECTS")) {
        fprintf(stderr, "[escapes] %-22s { ", FN(f));
        for (size_t i = 0; i < c->escapees.len; i++)
            fprintf(stderr, "%s ", *(const char **)vecAt(&c->escapees, i));
        fprintf(stderr, "}\n");
    }
}

static void computeEscapes(Checker *c, FuncDef *f) { computeEscapesMode(c, f, false); }

/* Field-level depth: the small per-field table that tracks the references inside a
 * binding.
 *
 * The problem: `Sym.refDepth` is a single number, an upper bound on where the
 * references inside the binding point. With one number, `h.p = null` can only be a weak
 * update (take the maximum), so a stale depth 1 stayed recorded and `return h` was
 * falsely rejected.
 *
 * The fix keeps a small field table per binding, at most 4 slots, with whatever does not
 * fit going into `otherDepth` as a conservative fallback:
 *   - the effective depth of the root is max(otherDepth, depth of each field), which is
 *     what reading `h` uses;
 *   - reading `h.p` reads that slot directly;
 *   - writing `h.p = v` is a strong update (the slot is overwritten) when the address of
 *     the binding was never taken, and a weak update (maximum) when it was;
 *   - a whole-object assignment (`h = ...`) clears the table and refills it, falling
 *     back to the conservative path when the address was taken.
 * `otherDepth` holds what cannot be attributed to one slot (element writes, more than 4
 * fields) and only ever grows.
 *
 * Params:
 *   c      - unused; kept to match the checker argument convention
 *   s      - the binding whose field table is consulted
 *   field  - field name to look up or create
 *   create - true to add a slot when the field is not in the table yet
 *
 * Returns:
 *   Pointer to the depth slot for that field, or NULL when there is no slot and `create`
 *   is false or the table is full.
 *
 * Notes:
 *   - The name stored in a slot comes from the AST, which outlives the binding record,
 *     so the pointer cannot dangle.
 */
int *fieldDepthEntry(Checker *c, Sym *s, const char *field, bool create) {
    (void)c;                                       /* long unused; this silences the old warning */
    if (!s || !field) return NULL;
    for (int i = 0; i < s->nfields; i++)
        if (s->fields[i].name && strcmp(s->fields[i].name, field) == 0) return &s->fields[i].depth;
    if (!create) return NULL;
    EXTC_DBG_ASSERT(s->nfields <= 4);             /* the table's declared capacity */
    if (s->nfields >= 4) return NULL;             /* table full: `otherDepth` is the fallback */
    s->fields[s->nfields].name  = field;          /* interned in the AST, so it cannot dangle */
    s->fields[s->nfields].depth = 0;
    return &s->fields[s->nfields++].depth;
}

/* Recompute a binding's effective depth after one of its field slots changed.
 *
 * Why:
 *   The field table splits the depth of a binding into one slot per field, and a write
 *   updates a single slot. Reading the binding has to report the deepest reference it
 *   can still carry, which is the max of the fallback slot and every field slot.
 *
 * Params:
 *   s - binding whose field table was just updated; NULL is allowed and ignored
 *
 * Notes:
 *   - The result is an upper bound, so it may only grow. See the note inside the
 *     function for the failure that a drop causes.
 */
static void refreshRootDepth(Sym *s) {
    if (!s) return;
    int m = s->otherDepth;
    for (int i = 0; i < s->nfields; i++) if (s->fields[i].depth > m) m = s->fields[i].depth;
    /* The recorded depth of the root may only grow: it is an upper bound over the live
     * pointers in the binding, so a value may be reported too deep but never too
     * shallow. Letting it drop is unsound: storing a deep value into one field and then
     * a shallow value into another shrinks the root, while the shallow store does not
     * erase the pointer still live in the first field, so a later whole-value copy or
     * return is accepted. Three counterexamples of that shape reproduced as an ASan
     * heap-use-after-free. */
    if (s->refDepth > m) m = s->refDepth;
    s->refDepth = m;
}

/* Record the depth of a store into a place reached through a binding.
 *
 * Params:
 *   c     - checker; `c->curStoreVal` is the expression being stored
 *   root  - binding whose field table is updated; may be NULL
 *   field - field name, or NULL for a whole-value or element write
 *   d2    - depth of the stored value
 *
 * Notes:
 *   - A field write on a reference-typed binding (`n: mut ref node`) writes into the
 *     fields of the object it points at, while `refreshRootDepth` recomputes the effective
 *     depth as the maximum over the fields. That overwrote "who I point at" (depth 2) with
 *     "what the object I point at contains" (field depth 0), so `keeper = n` was accepted
 *     afterwards although the pointer dangled, as ASan confirmed. A reference-typed
 *     binding's `refDepth` may therefore only be raised, never lowered; an ordinary struct
 *     binding may still be lowered, which is what the field-slot update needs.
 */
/* Keep the source expression of a field slot, so a later pass can walk from a container
 * back to the allocation site that filled it.
 *
 * Params:
 *   root  - binding whose field table is updated; may be NULL
 *   field - field name, or NULL for a whole-object or element write; may be NULL
 *   d2    - depth of the write
 *   src   - the stored expression; may be NULL
 *
 * Notes:
 *   - Two rules, both learned by measurement:
 *   - Record a source only for a shape that names a site. `null` and a literal name no
 *     allocation, and recording one would displace the real source recorded earlier:
 *     `if c == 1 { h.q = x } else { h.q = null }` lost its source exactly that way.
 *   - Compare against the depth of this write (`srcDepth`), never against `depth`.
 *     `depth` is a weak update that takes the max, so a later `null` store looks deeper
 *     than the real one and would clear the source.
 */
static void noteFieldSrc(Sym *root, const char *field, int d2, Expr *src) {
    if (!root || !field || !src) return;
    if (src->kind != EX_IDENT && src->kind != EX_FIELD && src->kind != EX_INDEX &&
        src->kind != EX_NEW   && src->kind != EX_GENCALL) return;
    for (int i = 0; i < root->nfields; i++) {
        if (!root->fields[i].name || strcmp(root->fields[i].name, field) != 0) continue;
        if (dbgOn("EXTC_DBG_FS"))
            fprintf(stderr, "[fs] %s.%s d2=%d oldSrcDepth=%d srckind=%d\n", root->name,
                    field, d2, root->fields[i].srcDepth, (int)src->kind);
        if (d2 >= root->fields[i].srcDepth) {
            root->fields[i].src = src;
            root->fields[i].srcDepth = d2;
        }
        return;
    }
}

/* Record the depth of a store into a place reached through a binding.
 *
 * Params:
 *   c     - checker; `c->curStoreVal` is the expression being stored
 *   root  - binding whose field table is updated; may be NULL
 *   field - field name, or NULL for a whole-value or element write
 *   d2    - depth of the stored value
 *
 * Notes:
 *   - A field write on a reference-typed binding (`n: mut ref node`) writes into the
 *     fields of the object it points at, while `refreshRootDepth` recomputes the effective
 *     depth as the maximum over the fields. That overwrote "who I point at" (depth 2) with
 *     "what the object I point at contains" (field depth 0), so `keeper = n` was accepted
 *     afterwards although the pointer dangled, which ASan confirmed. A reference-typed
 *     binding's `refDepth` may therefore only be raised, never lowered; an ordinary struct
 *     binding may still be lowered, which is what the field-slot update needs.
 */
void noteFieldDepthWrite(Checker *c, Sym *root, const char *field, int d2) {
    if (!root) return;
    noteFieldSrc(root, field, d2, c->curStoreVal);   /* the depth record is left untouched */
    /* A field write through a reference-typed binding (`n: mut ref node`) writes a
     * field of the pointee, and `refreshRootDepth` recomputes the root as the max over
     * the field slots, which replaces what I point at (depth 2) with what is stored
     * inside the object I point at (the field depth, 0). A later `keeper = n` was then
     * accepted and dangled (confirmed with ASan). The fix: for a reference-typed
     * binding, `refDepth` may only be moved toward the longer-lived value, never
     * lowered. A struct binding that is not a reference may still drop, and that is
     * exactly what the strong update is for. */
    bool isRefRoot = root->type && tsub(c, root->type)->kind == TY_REF;
    int  before    = root->refDepth;
    if (!field) {                                  /* whole-object assignment or element write */
        /* An element write invalidates no other element or field, so it must neither
         * clear the field table nor overwrite `otherDepth`. The old behavior cleared the
         * table and wrote `otherDepth = 0`, so `a[1] = x; a[0] = null` let a whole-value
         * escape through. */
        if (d2 > root->otherDepth) root->otherDepth = d2;
        refreshRootDepth(root);
        if (isRefRoot && root->refDepth < before) root->refDepth = before;   /* keep the pointee */
        return;
    }
    int *slot = fieldDepthEntry(c, root, field, true);
    if (!slot) {                                   /* full table: the fallback slot takes the max */
        if (d2 > root->otherDepth) root->otherDepth = d2;
    } else if (root->addressed) {                  /* address taken: aliases may write, take max */
        if (d2 > *slot) *slot = d2;
    } else if (root->fieldsComplete) {
        /* First step of the data-flow part: with a complete field table, the new value
         * replaces the old one in this slot, so the old value is no longer a property of
         * any slot of the container, and the root is recomputed skipping it. That is the
         * strong update. It is sound because the root record is an upper bound over all
         * slots: with a complete table, removing an edge that no longer holds leaves an
         * upper bound over the rest. The depth is not shrunk out of thin air. */
        *slot = d2;
        int m = root->otherDepth;
        for (int i = 0; i < root->nfields; i++) {
            if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) continue;
            if (root->fields[i].depth > m) m = root->fields[i].depth;
        }
        root->refDepth = m;
        if (isRefRoot && root->refDepth < before) root->refDepth = before;   /* keep the pointee */
        return;
    } else {
        /* An incomplete field table forbids any drop: the missing slots may hold values.
         * This is how a real `stack-use-after-scope` was built once. `var h3 = h` dropped
         * the `q` slot, so after `h3.p = null` the depth of `q` vanished and `return h3`
         * was accepted; the regression test
         * `tests/arena-soundness/H1_strongupdate_missed.extc` caught it. */
        *slot = d2;
    }
    refreshRootDepth(root);
    /* Do not forget what a reference-typed binding points at: the field table only
     * describes what is stored inside the pointee, so `refDepth` is restored here. */
    if (isRefRoot && root->refDepth < before) root->refDepth = before;
}

/* Record a whole aggregate assignment without throwing away its per-field provenance.
 *
 * A plain `h = t` copies the fields by value. Recording only `h.otherDepth = depth(t)` loses
 * the allocation site stored in `t.fields[i].src`; a later `return h` can still promote the
 * site through `t`, but cannot lower the stale aggregate depth left on `h`, so a safe value is
 * rejected. A complete source table can be copied exactly. An exposed destination or an
 * incomplete source keeps the existing conservative aggregate update.
 */
void noteWholeValueDepthWrite(Checker *c, Sym *dst, Expr *value, int d2) {
    if (!dst) return;

    Sym *src = value && value->kind == EX_IDENT ? identBindOf(value) : NULL;
    if (!dst->addressed && src && src != dst && src->fieldsComplete) {
        dst->nfields = src->nfields;
        for (int i = 0; i < src->nfields; i++) dst->fields[i] = src->fields[i];
        dst->otherDepth = src->otherDepth;
        dst->fieldsComplete = true;

        int m = dst->otherDepth;
        for (int i = 0; i < dst->nfields; i++)
            if (dst->fields[i].depth > m) m = dst->fields[i].depth;
        dst->refDepth = m;
        return;
    }

    noteFieldDepthWrite(c, dst, NULL, d2);
}
/* Position of a named parameter, or -1.
 *
 * Params:
 *   f    - function to search; may be NULL
 *   name - parameter name; may be NULL
 *
 * Returns:
 *   Zero-based index of the parameter, or -1 when it is not a parameter.
 */
/* **按名字**找参数：只在调用点手上只有名字时才用（`paramIndexOfExpr` 才是身份版）。
 * 参数遮蔽是合法语言特性，按名字匹配会认错人（P0-6d）—— 所以有了表达式就不要再走这个。*/
static int paramIndexByName(FuncDef *f, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, name) == 0) return (int)i;
    return -1;
}

/* The binding an expression's root resolved to, **without a scope lookup**.
 *
 * `placeRoot` answers the same question by walking the checker's scopes, and those only exist while
 * a body is being checked; the effect summary runs afterwards. The `Sym` is already on the
 * `EX_IDENT` node, put there by resolution, so this needs no scope. */
static Sym *rootSymNoScope(Expr *e) {
    /* 问题③的**无作用域**版本（摘要 pass 在所有函数体检查完之后才跑，那时没有作用域可查，
     * 这是 P0-6d 的根因）。掩码含 `SIGN`、保留 16 跳上限 —— 差异来自历史，现在写在掩码里。*/
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF | EXTC_ROOT_SIGN, 16);
    return leaf ? identBindOf(leaf) : NULL;
}

/* Which parameter of `f` does the value `e` come from? **By symbol, not by spelling.**
 *
 * The summary records "argument j was stored" and the call site checks argument j, so this answer
 * has to survive shadowing. `paramIndex` by name stays for callers that only have a name. */
static int paramIndexOfExpr(Checker *c, FuncDef *f, Expr *e) {
    (void)c;
    if (!f || !e) return -1;
    Sym *root = rootSymNoScope(e);
    if (!root) return -1;
    EXTC_DBG_ASSERT(f->nParamSyms <= 64);         /* must equal the array the struct declares */
    for (int i = 0; i < f->nParamSyms; i++)
        if ((Sym *)f->paramSyms[i] == root) return i;
    return -1;
}

/* Record which effect-summary bit the stored value `e` sets for `f`.
 *
 * Why:
 *   An assignment through a `mut ref` parameter, or into an object allocated in this
 *   frame, can hand a pointer to the caller. The summary of `f` records, per parameter,
 *   whether an address coming from the argument is stored (address flow) or a pointer
 *   read out of the argument's contents is stored (content flow); the call site then
 *   checks only the bits that are set.
 *
 * Params:
 *   c        - checker; NULL is allowed, and then the type test is skipped
 *   f        - function whose effect summary is being filled in
 *   e        - the value being stored
 *   i        - index of the destination container parameter, or -1 when the destination
 *              is not a parameter of `f`
 *   intoHome - true when the destination is an object freshly allocated in this frame
 *              (the home arena, which the caller chooses), false for a `mut ref`
 *              parameter
 *   fresh    - names allocated in this frame; a value coming from one of them needs no
 *              constraint at the call site
 */
static void classifyStoredValue(Checker *c, FuncDef *f, Expr *e, int i, bool intoHome, Vec *fresh) {
    if (!e || !f) return;
    /* A one-payload variant construction (`some(v)`, `success(v)`) holds exactly what `v`
     * holds -- one tag and one copy -- so classify the payload, not the wrapper: the wrapper
     * is not a second source of references. SSO `string` keeps its pool block in an `option`
     * field (`self.big = some(blk)`), and without this the store reads as unattributable. */
    if (e->kind == EX_ENUMVAL && e->u.enumval.args.len == 1)
        e = *(Expr **)vecAt(&e->u.enumval.args, 0);
    /* Address flow: the address of a place is stored, so that place has to live at least
     * as long as the container it is stored into. */
    if (e->kind == EX_REF) {
        int j = paramIndexOfExpr(c, f, e->u.ref.operand);
        if (j >= 0) {
            if (intoHome) f->homeAddrMask |= ((uint64_t)1 << j);
            else if (i >= 0) f->addrMask |= ((uint64_t)1 << j);
        } else {
            f->addrFromLocal = true;              /* local address: a different problem */
        }
        return;
    }
    /* A value that comes from a parameter must be classified: is it the pointer itself,
     * or a pointer read out of a container?
     *   - `s.r = target`, where `target` is a pointer parameter, stores the caller's own
     *     pointer. That is address flow: the call site has to guarantee that what it
     *     points at lives at least as long as the destination.
     *   - `n.next = l.head` reads a pointer out of the contents of a parameter. That is
     *     content flow.
     * The first version treated both as content flow, and tests/errors/ref_arg_too_deep
     * went from being rejected to passing. With this slot wrong, the call-site
     * rule that requires whatever an argument points at to outlive the destination
     * accepts a dangling pointer instead. */
    {
        int j = paramIndexOfExpr(c, f, e);
        if (j >= 0) {
            /* A scalar carries no reference and so imposes no lifetime constraint;
             * saying otherwise would make `varArray<i32>::push` a false positive. A type
             * that mentions a type parameter, however, counts as carrying a reference:
             * the summary is computed on the template, where `T` is still opaque. Without
             * that, the `Cont(v)` slot of `varArray<T>::push` would record no parameter at
             * all, the call site would check nothing, and a real dangling pointer would be
             * accepted. The cost is zero, because the call site looks at the concrete
             * argument types: in `push(ref v, 5)` the `5` is i32, so that step is
             * skipped. */
            bool carrier = (c && e->type)
                         ? (typeContainsRef(c->tt, tsub(c, e->type)) || mentionsParam(tsub(c, e->type)))
                         : true;
            bool isPtr = (e->kind == EX_IDENT) && e->type &&
                         (e->type->kind == TY_REF || ttIsViewType(e->type));
            if (carrier) {
                if (isPtr) {                      /* address flow: the caller's pointer is stored */
                    if (intoHome) f->homeAddrMask |= ((uint64_t)1 << j);
                    else if (i >= 0) f->addrMask |= ((uint64_t)1 << j);
                } else {                          /* content flow: pointer read from a container */
                    if (intoHome) f->homeContMask |= ((uint64_t)1 << j);
                    else if (i >= 0) f->contMask |= ((uint64_t)1 << j);
                }
            }
            return;
        }
    }
    if (e->kind == EX_NEW) return;                /* freshly allocated, so it is trivially safe */
    if (exprIsPoolBlock(e)) return;               /* a pool plate block: fresh, and the container owns it */
    /* A variant construction with **no payload** (`none`, and any payload-free enum variant)
     * holds no reference, whatever the union's type says: the payload is simply not there.
     * Without this, `self.big = none` -- how an SSO string clears its pool view -- marks the
     * whole container as "stores something unknown", every caller of `push` inherits
     * `otherMask`, and the call-site rule turns into the strictest one there is: an argument
     * of a caller may not live in any block at all (measured: `tests/coro/statemachine.extc`
     * was rejected -- "argument 1 of `step` points into a deeper scope (depth 1) than the
     * arena this call may store it in (depth 0)"). `failure(e)` and `some(v)` carry a value
     * and are classified as before. */
    if (e->kind == EX_ENUMVAL && e->u.enumval.args.len == 0) return;
    { const char *vr = placeRootName(e);          /* `l.head = n`: the source is a fresh local */
      if (vr && vecHasName(fresh, vr)) return; }
    if (c && e->type && !typeContainsRef(c->tt, e->type)) return;   /* scalar, trivially safe */
    if (i >= 0) f->otherMask |= ((uint64_t)1 << i);        /* not sure, so stay conservative */
}

static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e);

/* Record every store into a place that this statement can perform.
 *
 * Why:
 *   The effect summary of a function is the union of what its body and its callees may
 *   store: which parameter receives an address, which receives a pointer read out of
 *   another parameter, and which one is unknown. The call site then constrains only the
 *   arguments whose bits are set.
 *
 * Params:
 *   c     - checker
 *   f     - function whose summary is being filled in
 *   s     - statement to walk; NULL is allowed
 *   fresh - names allocated in this frame; a value coming from one of them is safe, so
 *           nothing is recorded for it
 */
static void collectEffectsStmt(Checker *c, FuncDef *f, Stmt *s, Vec *fresh) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        collectEffectsExpr(c, f, s->u.trap_.msg);
        return;
    case ST_YIELD:
        collectEffectsExpr(c, f, s->u.yield_.value);
        return;
    case ST_ASSIGN: {
        /* Is the target a container named by a `mut ref` parameter?
        * `l.head = ...`, `*p = ...`, and `l.buf[i] = ...` all are. */
        const char *dstRoot = placeRootName(s->u.assign.target);
        int i = paramIndexByName(f, dstRoot);
        if (i >= 0) {
            /* Destination: the container parameter `i` names. */
            Param *p = *(Param **)vecAt(&f->params, i);
            if (p->type && p->type->kind == TY_REF && p->type->mut)
                classifyStoredValue(c, f, s->u.assign.value, i, false, fresh);
        } else if (dstRoot && vecHasName(fresh, dstRoot)) {
            /* Destination: an object freshly allocated in this frame
            * (`n.next = l.head`, `n.owner = ref l`). This is the store into the home
            * arena, which is where the content flow of `push` comes from. */
            classifyStoredValue(c, f, s->u.assign.value, -1, true, fresh);
        }
        collectEffectsExpr(c, f, s->u.assign.target);
        collectEffectsExpr(c, f, s->u.assign.value);
        return;
    }
    case ST_VAR:    collectEffectsExpr(c, f, s->u.var.init); return;
    case ST_DOMAIN:
        /* The receiver is a domain object; the body is ordinary code. */
        collectEffectsStmt(c, f, s->u.domain_.body, fresh);
        return;
    case ST_IF:
        collectEffectsExpr(c, f, s->u.ifs.cond);
        collectEffectsStmt(c, f, s->u.ifs.thenBody, fresh);
        collectEffectsStmt(c, f, s->u.ifs.elseBody, fresh);
        return;
    case ST_WHILE:
        collectEffectsExpr(c, f, s->u.whiles.cond);
        collectEffectsStmt(c, f, s->u.whiles.body, fresh);
        return;
    case ST_RETURN: collectEffectsExpr(c, f, s->u.ret.value); return;
    case ST_EXPR:   collectEffectsExpr(c, f, s->u.expr.expr); return;
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            collectEffectsStmt(c, f, *(Stmt **)vecAt(&s->u.block.stmts, k), fresh);
        return;
    case ST_MATCH:
        collectEffectsExpr(c, f, s->u.match.scrutinee);
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            collectEffectsStmt(c, f, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body, fresh);
        return;
    default: return;
    }
}

/* Record the calls inside an expression and the callees they reach.
*
* Params:
*   c - checker
*   f - function whose callee list and summary are being filled in
*   e - expression to walk; NULL is allowed
*
* Notes:
*   - A call that could not be resolved sets `f->effUnknown`, which keeps the summary
*     incomplete forever, so no check may be relaxed on the strength of it.
*   - The recorded edges are the call graph of this function: they are what
*     `computeEffectsTransitive` later closes the summary over.
*/
static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e) {
    if (!e) return;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && !e->func) {
        /* A call through a `fn` **value** (no `FuncDef` on the node).
         *
         * When the value came from a **table slot that carries an `effects` clause**, that clause
         * is exactly as authoritative as an `extern!` declaration -- it is the same parser, the
         * same three keys, and the checker already builds the call-site signature out of it. So
         * merge it into this function's summary and keep the summary **complete**; marking it
         * unknown instead made every caller of a wrapper pay the worst case, even when the slot
         * said "I store nothing":
         *
         *     fn rgb(api: ref cairoApi, cr: ref void, …) { api.setSourceRgb(cr, …) }
         *     // `setSourceRgb` is signed `Addr=0 Cont=0`, yet `rgb`'s summary came out
         *     // incomplete ⇒ its callers could no longer pass a frame-local pointer
         *     // ("points into a deeper scope (depth 1) than the arena this call may store it
         *     // it in"), measured while writing the cairo bindings (C-ABI.md §9.11 ④).
         *
         * A `fn` value that did **not** come from a signed slot (a local copy, a parameter, an
         * unsigned slot) still has nothing to go on: the summary stays incomplete, conservatively.
         * Note this is the same trade the extern path makes: the clause is believed, and the whole
         * basis for believing it is that the author wrote it. */
        FieldDef *sfd = NULL;
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_FIELD)
            sfd = e->u.call.callee->field;
        else if (e->kind == EX_METHOD && e->u.method.recv && e->u.method.recv->kind == EX_FIELD)
            sfd = e->u.method.recv->field;
        if (sfd && sfd->hasEffects) {
            Vec *args = (e->kind == EX_CALL)  ? &e->u.call.args :
                        (e->kind == EX_METHOD) ? &e->u.method.args : &e->u.assoc.args;
            uint64_t addr = sfd->effAddrMask, cont = sfd->effContMask;
            for (size_t j = 0; j < args->len && j < EFF_MAX_PARAMS; j++) {
                if (!(((addr | cont) >> j) & 1u)) continue;      /* this argument is not kept */
                Expr *a = *(Expr **)vecAt(args, j);
                if (a && a->kind == EX_REF) a = a->u.ref.operand; /* `f(ref x)` stores `&x` */
                int pi = paramIndexByName(f, placeRootName(a));
                if (pi >= 0) {
                    if ((addr >> j) & 1u) f->addrMask |= ((uint64_t)1 << pi);
                    if ((cont >> j) & 1u) f->contMask |= ((uint64_t)1 << pi);
                } else {
                    /* It keeps something that does not trace back to a parameter of mine: say so
                     * in the one way the summary has (`otherMask` = "not attributable"), which the
                     * call site reads as the strictest case. */
                    f->otherMask |= 1u;
                }
            }
        } else {
            f->effUnknown = true;   /* nothing to go on -> incomplete, conservatively */
        }
    }
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && e->func) {
        bool seen = false;
        for (size_t k = 0; k < f->callees.len; k++)
            if (*(FuncDef **)vecAt(&f->callees, k) == e->func) { seen = true; break; }
        if (!seen) *(FuncDef **)vecPush(&f->callees) = e->func;
    }
    switch (e->kind) {
    case EX_BIN:   collectEffectsExpr(c, f, e->u.bin.left);
                   collectEffectsExpr(c, f, e->u.bin.right); return;
    case EX_UN:    collectEffectsExpr(c, f, e->u.un.operand); return;
    case EX_REF:   collectEffectsExpr(c, f, e->u.ref.operand); return;
    case EX_EXT: collectEffectsExpr(c, f, e->u.ext_.call); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: collectEffectsExpr(c, f, e->u.deref.operand); return;
    case EX_SIGN:  collectEffectsExpr(c, f, e->u.sign.operand); return;
    case EX_CONV:  collectEffectsExpr(c, f, e->u.conv.operand); return;
    case EX_TRY:   collectEffectsExpr(c, f, e->u.try_.operand); return;
    case EX_SLICE: collectEffectsExpr(c, f, e->u.slice.obj);
                   collectEffectsExpr(c, f, e->u.slice.lo);
                   collectEffectsExpr(c, f, e->u.slice.hi); return;
    case EX_FIELD: collectEffectsExpr(c, f, e->u.field.obj); return;
    case EX_INDEX: collectEffectsExpr(c, f, e->u.index.obj);
                   collectEffectsExpr(c, f, e->u.index.index); return;
    case EX_COALESCE:
        collectEffectsExpr(c, f, e->u.coalesce.main);
        collectEffectsExpr(c, f, e->u.coalesce.fallback); return;
    case EX_NEW:   collectEffectsExpr(c, f, e->u.new_.count); return;
    case EX_CALL:
        for (size_t k = 0; k < e->u.call.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.call.args, k));
        return;
    case EX_METHOD:
        collectEffectsExpr(c, f, e->u.method.recv);
        for (size_t k = 0; k < e->u.method.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.method.args, k));
        return;
    case EX_ASSOC:
        for (size_t k = 0; k < e->u.assoc.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.assoc.args, k));
        return;
    /* The aggregate shapes hold expressions too, and a call inside one belongs in the
     * summary exactly like a call anywhere else: `[f(), g()]`, `some(f())`,
     * `point { x: f() }`. They used to fall to `default` and be dropped, which made the
     * summary of the enclosing function incomplete - the one direction the doc above
     * says must not happen (`effUnknown` exists so incompleteness is conservative). */
    case EX_ARRAYLIT:
        for (size_t k = 0; k < e->u.arraylit.elems.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.arraylit.elems, k));
        return;
    case EX_ENUMVAL:
        for (size_t k = 0; k < e->u.enumval.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.enumval.args, k));
        return;
    case EX_STRUCTLIT:
        for (size_t k = 0; k < e->u.lit.inits.len; k++)
            collectEffectsExpr(c, f, (*(FieldInit **)vecAt(&e->u.lit.inits, k))->value);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t k = 0; k < e->u.lambda.inits.len; k++)
            collectEffectsExpr(c, f, (*(FieldInit **)vecAt(&e->u.lambda.inits, k))->value);
        return;
    case EX_GENCALL:
        for (size_t k = 0; k < e->u.gencall.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.gencall.args, k));
        return;
    case EX_DYN:
        collectEffectsExpr(c, f, e->u.dynv.payload);   /* a call in the payload is a call */
        return;
    default: return;
    }
}

/* Effect summary of a function: what does it store into the containers it was handed?
 *
 * That question decides how long the `mut ref` arguments of a call have to live. The
 * earlier rule treated every callee as if it might store an address, which rejected
 * ordinary code that only pushes fresh values into a container. The summary is built from
 * three sources, and only the first two constrain an argument:
 *   - address flow `Addr(j)`: the body stores `ref param_j` or `ref param_j.field`, so
 *     the storage of argument `j` must outlive the destination;
 *   - content flow `Cont(j)`: the body stores a pointer read *out of* parameter `j`
 *     (such as `l.head`), so the data argument `j` points at must outlive the destination;
 *   - fresh data: a `new` allocation, a literal, or a scalar imposes nothing.
 *
 * Storing the address of a local of this frame is recorded separately (`addrFromLocal`),
 * because the call site rejects that on its own.
 *
 * Params:
 *   c - checker
 *   f - function to summarize
 *
 * Notes:
 *   - The summary is computed here but not yet used, so this step changes no behaviour;
 *     `checkCallRefArgs` is what reads it.
 *   - An `extern!` declaration is summarized from its `effects` clause instead of from a
 *     body. Without a clause every parameter below 32 is marked as stored, which is nearly
 *     unusable and is exactly what a safe default should look like; signing
 *     `effects Addr=0 Cont=0` is how a declaration becomes usable.
 */
static void collectEffects(Checker *c, FuncDef *f) {
    /* An extern declaration has no body, so the summary comes from the signed clause or
    * is the worst case. Skipping this would leave the summary empty, which reads as
    * "stores nothing" and would accept a dangling pointer. */
    if (f->isExtern) {
        if (f->hasEffects) {
            f->addrMask = f->extAddrMask;
            f->contMask = f->extContMask;
        } else {
            /* No clause means every parameter may be stored. That is nearly unusable, and it
            * is what a safe default should look like: sign the declaration to make it
            * usable. */
            uint64_t all = 0;
            for (size_t i = 0; i < f->params.len && i < EFF_MAX_PARAMS; i++) all |= ((uint64_t)1 << i);
            f->addrMask = all;
            f->contMask = all;
        }
        f->effComplete = true;      /* the declaration is authoritative, as a body would be */
        f->otherMask   = 0;
        return;
    }
    /* `FuncDef` comes from `arenaAllocZero`, so `callees` has no arena yet; without an
     * explicit `vecInit`, `vecPush` would allocate through a NULL arena and crash. */
    if (!f->callees.arena) vecInit(&f->callees, c->arena, sizeof(FuncDef *));
    /* The escape set decides which arena the calls in this body pass, so it has to be
     * known before the body is walked. */
    computeEscapes(c, f);
    /* 这里曾经写 `c->escapeesFor = (int)(size_t)f;` —— 把指针截断成 `int` 当"已算过"的键，
     * 而那个字段**只写不读**（V 批次：死字段 + 指针截断，一并删掉）。将来若真需要记忆化，
     * 用 `FuncDef *` 或 `Sym *` 字段，而不是把地址塞进整数。*/
    if (getenv("EXTC_DUMP_EFFECTS")) fprintf(stderr, "[escapes-for] %s\n", FN(f));
    Vec fresh; vecInit(&fresh, c->arena, sizeof(const char *));
    collectFreshLocals(c->arena, &fresh, f->body);
    f->freshCount = (unsigned)fresh.len;
    collectEffectsStmt(c, f, f->body, &fresh);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects] %-22s toParam[Addr=0x%llx Cont=0x%llx Other=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] localAddr=%d fresh=%u callees=%zu\n",
                FN(f), (unsigned long long)f->addrMask, (unsigned long long)f->contMask,
                (unsigned long long)f->otherMask,
                (unsigned long long)f->homeAddrMask, (unsigned long long)f->homeContMask,
                (int)f->addrFromLocal, f->freshCount, f->callees.len);
}

/* Is the depth of this value decided by an allocation site?
*
* The depth recorded while the template body was checked is a snapshot of a world the
* level solver later changes: some sites move from the home arena back to a block level,
* so the recorded number can be too deep. Recomputing is therefore the right answer for a
* value whose depth a site decides. Measured: `varArray<T>::withCap` records `depth = 1`
* for `return v` while the `new T[cap]` inside it has been placed in the home arena, and
* the instance check then reported "depth 1, but this can only hold up to 0" although the
* real answer is 0.
*
* Recomputing is only allowed for the shapes whose depth the solver decides. A binding
* gets its depth from `checkStmt` in whatever scope was current at the time, which has
* nothing to do with the solver: recomputing answered 0 for the local in
* `tests/errors/generic_return_local.extc`, whose site was still undecided (and an
* undecided site has `refDepth` 0), so the instance check accepted a real dangling
* pointer. Everything that is not a site (a binding, a call result, a value derived from a
* parameter) keeps its recorded depth, which errs towards rejection.
*
* `targetDepth` cannot replace this: it calls `lookup` for a binding, and `runRefCheck`
* runs in the scope of a different module, so the binding it finds is not the original
* one. This walk reads the levels already fixed on the nodes, which do not depend on the
* scope.
*
* Params:
*   c    - checker
*   e    - expression to classify; may be NULL
*   hops - number of indirections followed so far; the walk stops at 32
*
* Returns:
*   True when the value traces back to a `new` or a builtin generic allocation.
*/
/* Recomputing is only allowed for the shapes whose depth the solver decides.
*
* Recomputing can only lower `rc->depth`, that is, only relax the check, while a binding
* gets its depth from `checkStmt` in the scope that was current, which has nothing to do
* with the solver. A real case: `tests/errors/generic_return_local.extc` returns a local
* from a generic body with `T = slice<u8>`; the template recorded `depth = 1`, which is
* correct, and recomputation answered 0 for that binding because its site was still
* undecided and an undecided site has `refDepth` 0. The instance check then accepted a
* real dangling pointer.
*
* The correct test is whether the depth of the value really is decided by an allocation
* site, because that is the case the recomputation exists for: the solver can move a site
* from the home arena back to a block level. Every other shape (a binding, a call result,
* a value derived from a parameter) keeps its depth, which errs towards rejection.
*
* Params:
*   c    - checker
*   e    - expression to classify; may be NULL
*   hops - number of indirections followed so far; the walk stops at 32
*
* Returns:
*   True when the value traces back to a `new` or a builtin generic allocation.
*/
static bool depthComesFromAlloc2(Checker *c, Expr *e, int hops) {
    if (!e || hops > 32) return false;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL: return true;
    /* A `dyn` value's depth comes from its payload, which is copied into the pool. */
    case EX_DYN: return depthComesFromAlloc2(c, e->u.dynv.payload, hops + 1);
    /* Follow the origin: `var v = { buf: new T[cap], ... }  return v` reports `v`, a
    * binding, at the moment the error is raised, and its depth is decided by the `new`
    * it came from. Measured: the diagnostic said `kind=4 dca=0 svd=0` while the depth
    * was 1, which was a false rejection. */
    case EX_IDENT: {
        /* `lookup` cannot be used here: by the closing pass the scope has been popped, so
        * it finds nothing (the first fix still reported `dca=0`). Use the binding that
        * resolution pinned onto the node instead. */
        Sym *sy = identBindOf(e);
        Expr *org = sy ? sy->origin : NULL;
        if (!org) return false;
        return depthComesFromAlloc2(c, org, hops + 1);
    }
    case EX_DEREF:
        return depthComesFromAlloc2(c, e->u.deref.operand, hops + 1);
    case EX_FIELD:
        return depthComesFromAlloc2(c, e->u.field.obj, hops + 1);
    case EX_SIGN:     return depthComesFromAlloc2(c, e->u.sign.operand, hops+1);
    case EX_SLICE:    return depthComesFromAlloc2(c, e->u.slice.obj, hops+1);
    case EX_COALESCE: return depthComesFromAlloc2(c, e->u.coalesce.main, hops+1)
                          || depthComesFromAlloc2(c, e->u.coalesce.fallback, hops+1);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (depthComesFromAlloc2(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, hops+1)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (depthComesFromAlloc2(c, *(Expr **)vecAt(&e->u.arraylit.elems, i), hops+1)) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (depthComesFromAlloc2(c, *(Expr **)vecAt(&e->u.enumval.args, i), hops+1)) return true;
        return false;
    default: return false;   /* a binding, a field, a call, or a dereference decides nothing */
    }
}


static int solvedDepth(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        /* Do not fall back to `refDepth`: that number can be stale and would then
        * contradict the arena level. */
        return arenaDepthOf(planArenaLevel(e));
    case EX_IDENT: case EX_FIELD: case EX_INDEX:
        return e->refDepth > 0 ? e->refDepth : 0;
    /* The payload is copied into the pool: its depth is this value's depth (family E). */
    case EX_DYN:      return solvedDepth(e->u.dynv.payload);
    case EX_SIGN:     return solvedDepth(e->u.sign.operand);
    case EX_DEREF:    return solvedDepth(e->u.deref.operand);
    case EX_SLICE:    return solvedDepth(e->u.slice.obj);
    case EX_COALESCE: {
        int a = solvedDepth(e->u.coalesce.main);
        int b = solvedDepth(e->u.coalesce.fallback);
        return a > b ? a : b;
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            int x = solvedDepth((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.arraylit.elems, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.enumval.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_CALL: {
        int d = 0;
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            int x = solvedDepth(*(Expr **)vecAt(&e->u.call.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    default:
        /* **实测（U5，`EXTC_DBG_NOTEF` 打出 kind 数值）：走到这里的只有两种形状** ——
         *   `EX_INT`（151 次）与 `EX_CONV`（18 次）。
         * 两种都**不可能携带引用**：字面量没有引用；`EX_CONV` 的源类型被刻意收窄
         * （只有 `ref void` 这一种指针来源，不做一般指针转换，见 `check_expr.c` 里 `EX_CONV`
         * 那一段的注释）⇒ **答 0 是语义，不是兜底**。
         *
         * 那为什么还留着这个 NOTE：它是**计数哨兵** —— 将来若有新的 `ExprKind` 走到这里，
         * 计数会变，那才是要查的信号（与 walker 预算的关系：本仓库限制 23 个手写遍历
         * `tools/check_walkers.py`，把这些 kind 逐个列出来会让它变成第 24 个，
         * 而"列全"在这里买不到任何正确性）。*/
        EXTC_DBG_NOTEF("solvedDepth: kind=%d answered 0 (INT/CONV only, both carry no reference)", (int)e->kind);
        return 0;
    }
}

/* Check one function: its signature, its home-arena requirement, and its body.
 *
 * Params:
 *   c - checker
 *   f - function to check
 *
 * Notes:
 *   - A function that allocates and also hands back something useful (a reference or a
 *     view, or a store through an out-parameter) needs a home arena. A function that only
 *     returns `i32` does not: its allocations stay in its own block.
 *   - The escape set has to be computed before the body is walked, because the walk uses
 *     it to choose the arena of each call; computing it afterwards is the same as not
 *     computing it.
 *   - An `extern!` declaration is checked for shape only: a C function takes scalars and
 *     single pointers. A `slice<T>` would become two C arguments (pointer and length), so
 *     the names would not match, and a struct has no frozen layout. The stdlib wrappers
 *     pass `s.data` and `s.len` explicitly instead.
 */
/* Level pass: settle every allocation site's requirement from the recorded publications.
 *
 * A publication `(value, at)` says "this value ends up somewhere that lives `at` levels
 * deep". Following the value to the allocation sites it can reach, each of those sites
 * is then required to live to `at` as well. Following a value means:
 *
 *   binding      through `Sym.origin`, the expression its storage came from
 *   reference,   through the carrier: `ref x`/`*p`/`p!` name the same storage, a slice
 *   deref, sign  names part of its object, and a field or element lives inside its
 *   slice, field, object
 *   index
 *   aggregate    every element of a struct literal, array literal, or enum payload
 *   call         nothing: what a callee publishes is the callee's own business, and a
 *                call's result reaches a site only through a parameter it was handed
 *
 * This is a fold over data, so it is order-independent and can be run again: every step
 * only ever lowers `minAt`, that is, only strengthens a requirement, which is the
 * direction the solver needs. It replaces the previous scheme, where the same traversal
 * mutated site levels while the checker was still running, so a decision could depend on
 * which branch had been visited first.
 *
 * Params:
 *   c      - checker
 *   val    - the value being published
 *   target - arena level the destination lives at; 0 = beyond this frame
 *   hops   - recursion guard, since bindings may form cycles (`a = b  b = a`)
 */
/* The level a requirement carries when there is none: a literal, a call result, or the
 * value of a binding this analysis has no level for. It must be a number rather than a
 * sentinel, because requirements are combined with `min`, and it must be large enough
 * that it can never win that comparison, because "no requirement" is not a requirement --
 * reading it as 0 would make every such value look as if it had to outlive the frame. */
#define LEVEL_INF 1000000

/* The level of every binding of the function being decided, and the only state the level
 * pass keeps between rounds.
 *
 * It is a table rather than a field of `Sym` because a level and the depth a `Sym` already
 * carries are different questions with opposite monotonicity -- a depth only grows, a
 * level only shrinks -- and the repository's iron rule is that one fact has one place.
 * `LEVEL_INF` means nothing has demanded anything of the binding yet. */
typedef struct {
    Sym *sym;
    int  lv;
} SymLevel;

/* One entry of the cycle cut: a (value, level) pair that is on the walk right now.
 *
 * The walk follows a binding's publications, and a binding assigned from itself --
 * `s = s + x` inside a loop, which is what every fold looks like -- makes that graph
 * cyclic. The `hops` cap bounds the *depth* of the recursion but not the number of
 * *paths*: with a cycle and two edges per level there are 2^32 of them, and the compiler
 * simply did not return (no message, no position, flat memory). Measured on
 *
 *     fn acc<T>(a: T, b: T) -> T { var s: T = a  var i: i64 = 0
 *         while i < 3 { s = s + b  i = i + 1 }  return s }
 *
 * which is the shape of every generic fold (`sum<T>`, `max<T>`, `reduce<T>`).
 *
 * Cutting at a repeated pair is sound for what this walk is for: a level only ever gets
 * smaller, the pair carries the level being demanded, and the pass runs to a fixed point
 * over the publications -- so a demand this cut drops is applied by the next round,
 * starting from the state this one settled. */
typedef struct {
    Expr *val;
    int   target;
} LvlVisit;

/* Work budget for one level pass, in visited pairs.
 *
 * A backstop, not the fix: the cut above is what bounds the walk on a cycle, but a value
 * graph of diamonds can still branch. Hitting the budget is reported loudly and once --
 * a compiler that hangs has no message and no position, which is strictly worse than an
 * error saying so. Measured cost of the whole corpus is far below this: the pass visits
 * tens of pairs per ordinary function. */
#define LVL_STEP_BUDGET 4000000L

typedef struct {
    Vec      tbl;     /* SymLevel */
    Vec      path;    /* LvlVisit: the (value, level) pairs on the walk right now */
    long     steps;   /* pairs visited in this pass, against LVL_STEP_BUDGET */
    bool     loud;    /* the budget was already reported */
    FuncDef *fn;      /* the function being decided, for the diagnostic */
    Checker *c;       /* the checker, for the `EXTC_DBG_FX` counters */
    bool     fx;      /* `EXTC_DBG_FX`: count, or do not */
} LvlState;

static int  symLevel(LvlState *ls, Sym *sy);
static bool setSymLevel(LvlState *ls, Sym *sy, int lv);
static int  valueLevel(Checker *c, LvlState *ls, Expr *val, int hops);
/* The carrier chain of a `dyn Trait(x)` continues into its payload: the payload is copied into
 * the pool, so it must live as long as this value must. */

/* Walk a value's carrier chain and return the level the value itself has to live at.
 *
 * A level is a lower bound on lifetime: the smaller the number, the longer the storage
 * has to live, with 0 meaning "beyond this frame". The walk visits every allocation site
 * reachable through the value and lowers it to that level, and returns the smallest level
 * any part of the value carries.
 *
 * Params:
 *   c      - checker
 *   ls     - the levels of this function's bindings, as the pass currently knows them
 *   val    - the value to walk; may be NULL
 *   target - the level the value is being published at
 *   hops   - recursion guard (bindings may form cycles, as in `a = b  b = a`)
 *
 * Returns:
 *   The level the value has to live at, or `LEVEL_INF` when nothing about it demands a
 *   lifetime -- a literal, a call result, a binding with no level yet.
 *
 * Notes:
 *   - Only the forms that carry a value are followed. `*e` (the thing `e` points at),
 *     `&e` (the address of `e`) and `e[a:b]` (a view) are deliberately absent: following
 *     one was measured to be a false rejection rather than a missed promotion, because it
 *     pulls in the level of a value that has nothing to do with this one.
 *   - A binding is followed in both directions. Outwards, along what it holds, which is
 *     what reaches the allocation sites inside the value; inwards, to the binding's own
 *     level, which is where a demand that arrived from another publication enters. The
 *     descent uses the tighter of the two, because a binding that must outlive this frame
 *     cannot be holding storage that dies with it.
 *   - The answer is what travels along the edges between publications, which is why the
 *     pass that calls this runs to a fixed point over them. */
static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops);

/* The walk, with the cycle cut and the work budget around it.
 *
 * Every recursive edge in the body goes through this wrapper, so the path it keeps is the
 * chain being walked and the budget counts every pair the walk reaches -- including the
 * ones reached twice, which is the case the cut exists for. */
static int levelOfValue(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    for (size_t i = 0; i < ls->path.len; i++) {
        LvlVisit *v = (LvlVisit *)vecAt(&ls->path, i);
        /* Already on this path, at this level: everything reachable from here was reached
         * when it was entered, and whatever this visit could add is picked up by the next
         * round of the fixed point. */
        if (v->val == val && v->target == target) return LEVEL_INF;
    }
    if (++ls->steps > LVL_STEP_BUDGET && !ls->loud) {
        ls->loud = true;
        Ctx *cx = (ls->fn && ls->fn->ctx) ? ls->fn->ctx : c->ctx;
        ctxError(cx, val->line, 1,
                 "This is a limit of the compiler, not something wrong with the program;"
                 " please report it together with the program that triggered it.",
                 "internal: the level solver walked more than %ld steps and was stopped",
                 (long)LVL_STEP_BUDGET);
    }
    LvlVisit *slot = (LvlVisit *)vecPush(&ls->path);
    slot->val = val;
    slot->target = target;
    int r = levelOfValue2(c, ls, val, target, hops);
    ls->path.len--;                       /* pop: the path is the current chain, not a set */
    return r;
}

static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    if (dbgOn("EXTC_DBG_LV"))
        fprintf(stderr, "      [lv] kind=%-3d line=%-4d target=%-2d hops=%d\n",
                (int)val->kind, val->line, target, hops);
    switch (val->kind) {
    case EX_NEW:
    case EX_GENCALL:
        /* The site this whole walk was looking for. */        if (target < LEVEL_INF && (val->minAt < 0 || target < val->minAt)) {
            val->minAt = target;
        }
        return target;
    case EX_IDENT: {
        Sym *sy = identBindOf(val);
        /* Reaching an allocation through a *value* answers "where did this number come
         * from", and only the three forms above, this one, and the containers below
         * preserve that. The three forms deliberately absent from this switch -- `*e`,
         * `&e` and `e[a:b]` -- do not: `*e` is the thing `e` points at, and `&e` is the
         * address of `e`, so neither is the value the sub-expression holds.
         *
         * Following one anyway was measured to be a false rejection, not a missed
         * promotion: in `examples/alloc-in-block`, `return total` was followed through
         * `total = *q` into `q`, and from there into the allocation, so the `alloc` inside
         * the block was told "must outlive this frame" although nothing outlives the
         * block; the function has no home arena to give it, and the generated C referenced
         * `__extc_home`, which does not exist there. */

        if (dbgOn("EXTC_DBG_LV"))
            fprintf(stderr, "      [lv] ident=%-5s sym=%s origin=%s target=%d\n",
                    val->u.ident.name, sy ? "yes" : "NULL",
                    (sy && sy->origin) ? "yes" : "NULL", target);
        if (dbgOn("EXTC_DBG_LV") && sy && sy->origin)
            fprintf(stderr, "      [lv]   -> origin kind=%d line=%d (sym %s)\n",
                    (int)sy->origin->kind, sy->origin->line, sy->name ? sy->name : "?");
        /* A binding's level is its own: what it holds now may have arrived from several
         * statements, and `origin` remembers only one of them. It is computed by
         * `levelPass` and read here, which is how the requirement crosses from one
         * publication record to another. */
        /* Two edges leave a binding, and both are followed: outwards along what the
         * binding holds, which is what reaches the allocation sites inside the value, and
         * inwards to the binding's own level, which is where a requirement that arrived
         * from another publication enters. A binding's level is its own because what it
         * holds may have arrived from several statements, and `origin` remembers only one
         * of them. */
        int fromSym = sy ? symLevel(ls, sy) : LEVEL_INF;
        /* The level the value is being handed to also reaches the binding on the way, and
         * this is the edge that carries a demand *through* a binding.
         *
         * `out = h` hands `h` to `out`, which is level 0, so whatever `h` holds has to
         * reach level 0 as well -- otherwise the demand stops at the store and never
         * arrives at the allocation behind `h`. Lowering a binding's own level is the safe
         * direction: a binding that lives longer than it had to costs memory and cannot
         * leave a reference dangling. */
        if (sy && target < LEVEL_INF) setSymLevel(ls, sy, target);
        /* And the other direction of the same edge: a binding that is already known to
         * live longer than this walk demands tightens the walk.
         *
         * Without it the walk keeps carrying the level of the store it started from. `out`
         * is at level 0 because it is returned, while the store `out = h` was recorded at
         * level 1, and entering `out` at 1 left everything `out` holds one level too deep:
         * measured on `C3_if_join_wholevalue`, the `alloc` behind `h` stayed at the block
         * level while the struct carrying it was returned to the caller. */
        if (sy) {
            int cur = symLevel(ls, sy);
            if (cur < target) target = cur;
        }
        /* A binding is *not* descended into here.
         *
         * What a binding holds may have arrived from several assignments, and one
         * expression field cannot describe a join over them: whatever it remembers is the
         * last one written, which is one arm of an `if` chosen by source order. Measured on
         * `tests/arena-promoted/C3_if_join_wholevalue`, where `h = g` is followed by
         * `h = zero` and every walk of `h` therefore ended at the empty box in the other
         * arm, never reaching the allocation inside `g`.
         *
         * The records already hold every assigned expression, so the walk descends into
         * those instead -- `levelPass` walks the values recorded for a binding, which is
         * the join done properly rather than a race between statements. All that is read
         * here is the binding's own level.
         */
        /* Descend into the expression the binding was actually given, never into the
         * flattened root of its origin chain: the root belongs to whichever binding the
         * chain ended at, and `out = h` following `h = zero` therefore pointed at the
         * literal that initializes `zero`.
         *
         * This is the chain of assignments, and the level asked of the value is the
         * smaller of the binding's own level and the level being demanded here. */
        /* The walk descends at the tighter of the binding's level and the level demanded
         * of it: if the binding has to outlive the frame, so does the storage inside the
         * value it holds. */
        int inner = fromSym < target ? fromSym : target;
        int best = fromSym;
        /* Every expression ever assigned to the binding, not just the last one.
         *
         * What a binding holds at a control-flow merge is one of the values written to it,
         * and a single field cannot describe that: `Sym.heldSrc` remembers the assignment
         * that came last in source order, so `h = g` followed by `h = zero` made every walk
         * of `h` end at the empty box and never reach the allocation inside `g`. The
         * publications recorded while the body was checked are the authority -- they hold
         * each assigned expression, and walking all of them is the merge done properly
         * rather than a race between statements. */
        bool viaRecords = false;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *alt = *(StoreSite **)vecAt(&c->stores, i);
            if (!alt || alt->fn != ls->fn) continue;
            if (!alt->target || alt->target->kind != EX_IDENT) continue;
            if (identBindOf(alt->target) != sy) continue;
            viaRecords = true;
            int v = levelOfValue(c, ls, alt->value, inner, hops + 1);
            if (v < best) best = v;
        }
        /* A binding with no publication of its own -- only ever initialized, or written
         * through a projection -- is still described by the expression it was given. */
        if (!viaRecords) {
            /* An identifier that never resolved has no binding to walk, and this used to reach
             * `sy->heldSrc` with a NULL `sy` -- a SIGSEGV, found by tools/fuzz.py from a
             * three-line program (`fn make<T>(x: T) -> pair<T> { return { a: make, b: x } impl }`).
             * `best` is the answer for a binding with no publication to follow, and this cannot
             * hide an escape: an unresolved name is reported as an error, so no accepted program
             * takes this path. */
            if (!sy) return best;
            Expr *held = sy->heldSrc ? sy->heldSrc : sy->origin;
            if (!held) return fromSym;
            int v = levelOfValue(c, ls, held, inner, hops + 1);
            if (v < best) best = v;
        }
        return best;
    }
    /* The join node of an `if`: one value with two operands, either of which can be what
     * the destination ends up holding. Both are walked at the same level, and the answer is
     * the smaller of the two.
     *
     * Leaving this form out was the last thing keeping the whole-value if-join unsound.
     * `out = h` stores a join node, so a walk without this case returned "no requirement"
     * at the store itself and never reached the allocation behind either arm -- measured on
     * `tests/arena-promoted/C3_if_join_wholevalue`, where the site stayed at the block
     * level and the struct holding it was returned. */
    case EX_BIN: {
        int a = levelOfValue(c, ls, val->u.bin.left, target, hops + 1);
        int b = levelOfValue(c, ls, val->u.bin.right, target, hops + 1);
        /* The join takes the smaller of its arms, and that number belongs to the operands
         * as well as to the join.
         *
         * Each arm is a value the destination can end up holding, so an arm's bindings are
         * held to what the *other* arm needs: `h = g` in one arm and `h = zero` in the other
         * means either one is what `h` holds. When both arms are reached through the join
         * and neither is walked on its own, the binding inside the arm carrying the
         * allocation never learns what the join learned, and the demand stops at the arm
         * carrying nothing. Measured on `tests/arena-promoted/C3_if_join_wholevalue`. */
        int r = a < b ? a : b;
        /* Both arms are always revisited at the joined level. The first pass through them
         * is what produced `a` and `b`, but a walk can stop at a binding before reaching the
         * sites inside the value it holds, and then the level the join just settled on has
         * not reached those sites yet. The revisit is a no-op once they agree, so the
         * iteration still terminates. */
        levelOfValue(c, ls, val->u.bin.left, r, hops + 1);
        levelOfValue(c, ls, val->u.bin.right, r, hops + 1);
        return r;
    }
    case EX_SIGN:  return levelOfValue(c, ls, val->u.sign.operand, target, hops + 1);
    case EX_FIELD: return levelOfValue(c, ls, val->u.field.obj, target, hops + 1);
    case EX_INDEX: return levelOfValue(c, ls, val->u.index.obj, target, hops + 1);
    case EX_COALESCE: {
        /* Both sides can become the result, so both are published, and either may be the
         * one that carries the requirement. */
        int a = levelOfValue(c, ls, val->u.coalesce.main, target, hops + 1);
        int b = levelOfValue(c, ls, val->u.coalesce.fallback, target, hops + 1);
        return a < b ? a : b;
    }
    case EX_STRUCTLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.lit.inits.len; i++) {
            int x = levelOfValue(c, ls, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value,
                                 target, hops + 1);
            if (x < r) r = x;
        }
        /* The literal is how a binding gets its fields, and the field table stores the
         * expression that wrote each one. Reading it here covers the same fact from the
         * other side: `{ p: null, q: x }` names `x` in the literal, while a later read of
         * `g.q` is answered from the table, and the walk has to reach the allocation either
         * way. */
        for (size_t i = 0; i < c->allSyms.len; i++) {
            Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
            if (sy && sy->origin == val) promoteFieldsAt(c, sy, target, hops + 1);
        }
        return r;
    }
    case EX_ARRAYLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++) {
            int x = levelOfValue(c, ls, *(Expr **)vecAt(&val->u.arraylit.elems, i), target, hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ENUMVAL: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.enumval.args.len; i++) {
            int x = levelOfValue(c, ls, *(Expr **)vecAt(&val->u.enumval.args, i), target, hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    default:
        /* A scalar, a literal, a call result: nothing that reaches an allocation site
         * through this value. A callee's own sites are settled by the callee's
         * publications, and what it stores into a parameter is settled at the call site
         * from the arguments. */
        return LEVEL_INF;
    }
}

/* The level a binding has to live at, as the pass currently knows it.
 *
 * A binding's level is not something a single statement decides: what it holds now may
 * have arrived from several statements, and where it is published may demand more
 * lifetime than the value alone would. Both are edges between publication records, and
 * this table is where they meet. */

static int symLevel(LvlState *ls, Sym *sy) {
    if (!ls || !sy) return LEVEL_INF;
    if (ls->fx) ls->c->fxSymCalls++;
    for (size_t i = 0; i < ls->tbl.len; i++) {
        if (ls->fx) ls->c->fxSymCmp++;
        SymLevel *e = (SymLevel *)vecAt(&ls->tbl, i);
        if (e->sym == sy) return e->lv;
    }
    return LEVEL_INF;
}

/* Lower a binding's level, and report whether that moved it. Only the lowering direction
 * exists: a level is a requirement, and requirements accumulate. */
static bool setSymLevel(LvlState *ls, Sym *sy, int lv) {
    if (!ls || !sy || lv >= LEVEL_INF) return false;
    if (ls->fx) ls->c->fxSymCalls++;
    for (size_t i = 0; i < ls->tbl.len; i++) {
        if (ls->fx) ls->c->fxSymCmp++;
        SymLevel *e = (SymLevel *)vecAt(&ls->tbl, i);
        if (e->sym != sy) continue;
        if (lv < e->lv) {
            e->lv = lv; return true;
        }
        return false;
    }
    SymLevel *e = (SymLevel *)vecPush(&ls->tbl);
    e->sym = sy;
    e->lv  = lv;
    return true;
}

/* The level a stored value carries, read from the bindings it names.
 *
 * This is the other half of `levelOfValue`: that one walks a value down to the allocation
 * sites inside it, this one walks it up to the bindings it is made of. The two are run
 * together until the numbers stop moving. */
static int valueLevel(Checker *c, LvlState *ls, Expr *val, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    /* `dest` is the level of the place this value is being handed to, and it takes part in
     * every minimum below.
     *
     * Where two arms of an `if` write the same binding, the binding holds one value or the
     * other and must live long enough for both. That value is a join node, and one arm of
     * it can be a literal -- `h = g` in one arm and `h = zero` in the other leaves one arm
     * with no level of its own at all. The destination is the number that covers the case
     * the arm has nothing to say about, so leaving it out makes the join look like it
     * demands nothing, and the site behind the other arm is never told it has to outlive
     * the block. */
    switch (val->kind) {
    case EX_IDENT:  return symLevel(ls, identBindOf(val));
    /* `dyn Trait(x)`: the carrier chain continues into the payload (it is copied into the pool,
     * so it has to live as long as this value does). */
    case EX_DYN:    return valueLevel(c, ls, val->u.dynv.payload, hops + 1);
    /* The join node of an `if`: `h = g` in one arm and `h = zero` in the other is one
     * value with two operands. Since either operand can be what the binding ends up
     * holding, the requirement is the smaller of the two, compared with `dest` as well. */
    case EX_BIN: {
        int a = valueLevel(c, ls, val->u.bin.left, hops + 1);
        int b = valueLevel(c, ls, val->u.bin.right, hops + 1);
        return a < b ? a : b;
    }
    case EX_SIGN:   return valueLevel(c, ls, val->u.sign.operand, hops + 1);
    case EX_FIELD:  return valueLevel(c, ls, val->u.field.obj, hops + 1);
    case EX_INDEX:  return valueLevel(c, ls, val->u.index.obj, hops + 1);
    case EX_COALESCE: {
        int a = valueLevel(c, ls, val->u.coalesce.main, hops + 1);
        int b = valueLevel(c, ls, val->u.coalesce.fallback, hops + 1);
        return a < b ? a : b;
    }
    case EX_STRUCTLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.lit.inits.len; i++) {
            int x = valueLevel(c, ls, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value,
                               hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ARRAYLIT: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++) {
            int x = valueLevel(c, ls, *(Expr **)vecAt(&val->u.arraylit.elems, i), hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_ENUMVAL: {
        int r = LEVEL_INF;
        for (size_t i = 0; i < val->u.enumval.args.len; i++) {
            int x = valueLevel(c, ls, *(Expr **)vecAt(&val->u.enumval.args, i), hops + 1);
            if (x < r) r = x;
        }
        return r;
    }
    case EX_NEW:
    case EX_GENCALL:
        return val->minAt >= 0 ? val->minAt : val->lexicalLevel;
    default:
        return LEVEL_INF;
    }
}

/* Run the level pass: fold the publications recorded while the body was checked.
 *
 * Params:
 *   c   - checker
 *
 * It does **not** take the data-flow fixed point: the level solve reads its own table, seeded from
 * the publications recorded while the body was walked, and it runs *before* the DFA's numbers are
 * copied into `Sym.refDepth`. The parameter used to be passed in and ignored (`(void)dfr;`) while
 * this comment claimed the opposite -- that is the "decision sees the final depths" claim the
 * review called out. Making it *true* means moving the write-back above this call, which changes
 * arena placement and is a change of its own, not a comment fix.
 *
 * Returns:
 *   Nothing. The result is written to each allocation site's `minAt`.
 *
 * The pass answers one question -- how long must each allocation site live -- and it
 * answers it by replaying two kinds of edge until nothing moves:
 *
 *   - a value's carrier chain, from a publication down to the sites inside it, so the
 *     sites are lowered to the level the publication demands;
 *   - the bindings, whose level is the smallest level among the values stored into them
 *     and the stores that publish them onwards.
 *
 * The second kind is what a walk along one carrier chain cannot express. Where two arms
 * of an `if` write the same binding, the binding can hold either value, and only the join
 * over both records sees the one that demands the longer lifetime. Measured on
 * `tests/arena-promoted/C3_if_join_wholevalue`: the walk followed the arm holding the
 * empty box, the `alloc` in the other arm was never told it had to outlive the frame, and
 * the struct carrying it was returned after the block arena had been released.
 *
 * Every step only lowers a number, so the iteration is monotone and the answer does not
 * depend on the order the records are visited in. The round cap is the same shape as the
 * other fixed points in this file. */
static void levelPass(Checker *c, FuncDef *f) {
    LvlState ls;
    vecInit(&ls.tbl, c->arena, sizeof(SymLevel));
    vecInit(&ls.path, c->arena, sizeof(LvlVisit));
    ls.steps = 0;
    ls.loud = false;
    ls.fn = f;
    ls.c  = c;
    ls.fx = c->fxOn;
    if (ls.fx) c->fxLvlPasses++;

    /* Step one: how long does each binding have to live?
     *
     * A binding's level is the smallest level among the values stored into it and the
     * stores that publish it onwards, and a value read from a binding depends on that level
     * in turn, so this is a fixed point of its own. It converges before anything is
     * decided, which is what keeps the decision independent of the order the records are
     * visited in.
     *
     * Only the lowering direction exists, and a store only lowers the binding it names: a
     * value that arrives from somewhere shallower makes the binding live longer, never
     * shorter. */
    for (int round = 0; round < 64; round++) {
        bool moved = false;
        if (ls.fx) c->fxLvlRounds1++;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (ls.fx) { c->fxStoresSeen++; if (st && st->fn == f) c->fxStoresOwn++; }
            if (!st || st->fn != f) continue;   /* another body's publication */
            if (!st->target || st->target->kind != EX_IDENT) continue;
            Sym *d = identBindOf(st->target);
            if (!d) continue;
            int at = st->at;
            int v  = valueLevel(c, &ls, st->value, 0);
            if (v < at) at = v;
            if (setSymLevel(&ls, d, at)) { moved = true; if (ls.fx) c->fxLvlMoved++; }
        }
        if (!moved) break;
    }

    /* Step two: how long does each allocation site have to live?
     *
     * For every publication, the level to reach is the tighter of the level the store was
     * accounted at and the level of the destination -- the destination may be a binding
     * found in step one to live longer than the block the store happened to sit in, and
     * `out = h` recorded at level 1 with `out` returned at level 0 is exactly that case.
     *
     * When the destination is tighter, the requirement reaches everything the value was
     * built from, which is what the walk below does. That extra step is deliberately
     * narrow: a store into a place that lives no longer than the store itself says nothing
     * new about the value, and walking it anyway was measured to move the `new` inside a
     * loop from the loop arena to the frame arena, where it is no longer reclaimed each
     * round -- the per-block refinement the arena tests exist to protect. */
    for (int round = 0; round < 64; round++) {
        bool moved = false;
        if (ls.fx) c->fxLvlRounds2++;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (ls.fx) { c->fxStoresSeen++; if (st && st->fn == f) c->fxStoresOwn++; }
            if (!st || st->fn != f) continue;   /* another body's publication */
            int at = st->at;
            Sym *dst = (st->target && st->target->kind == EX_IDENT)
                       ? identBindOf(st->target) : NULL;
            int dl = dst ? symLevel(&ls, dst) : LEVEL_INF;
            if (dl < at) at = dl;
            int v = valueLevel(c, &ls, st->value, 0);
            if (v < at) at = v;
            bool carrying = typeContainsRef(c->tt, tsub(c, st->value->type))
                            || mentionsParam(st->value->type);
            /* Numbers inside a value still name a place (`cell.value = n` reaches `cell`),
             * so a walk of a value that carries no reference would drag bindings along with
             * it for no reason. */            if (!carrying) continue;
            levelOfValue(c, &ls, st->value, at, 0);
            /* A binding can be assigned more than once, and it then holds one of those
             * values. `Sy`->origin` remembers only the last assignment, so a walk that
             * followed it would stop at whichever arm happened to be written last --
             * measured on `C3_if_join_wholevalue`, where `h = g` is followed by
             * `h = zero` and the origin ended up naming the empty box, leaving the
             * allocation behind `g` unreachable from every later store of `h`.
             *
             * Every assigned value is in the record table, so walking them all is what
             * covers "one of these". Each is walked at the destination's level, which is
             * the level anything `h` holds has to reach. */            if (dst && dl < LEVEL_INF) {
                for (size_t k = c->storeBase; k < c->stores.len; k++) {
                    StoreSite *alt = *(StoreSite **)vecAt(&c->stores, k);
                    if (ls.fx) { c->fxAliasWalks++; if (alt && alt->fn == f) c->fxStoresOwn++; }
                    if (!alt || alt->fn != f) continue;
                    if (alt == st || !alt->target) continue;
                    if (alt->target->kind != EX_IDENT) continue;
                    if (identBindOf(alt->target) != dst) continue;
                    if (!typeContainsRef(c->tt, tsub(c, alt->value->type))
                        && !mentionsParam(alt->value->type)) continue;
                    /* The recorded value, not its origin. `originOf` flattens a chain of
                     * bindings down to the expression at the root of the chain, and that
                     * expression belongs to whichever binding the chain happened to end
                     * at: `out = h` following `h = zero` flattened to the literal that
                     * initializes `zero`, so descending through origins landed in an
                     * unrelated binding's empty box. The record holds the expression that
                     * was actually assigned, which is the arm itself. */
                    levelOfValue(c, &ls, alt->value, dl, 0);
                }
            }
            if (dst && dl < st->at && setSymLevel(&ls, dst, at)) { moved = true; if (ls.fx) c->fxLvlMoved++; }
        }
        if (!moved) break;
    }
    if (ls.fx) {
        c->fxTblSum += (long)ls.tbl.len;
        if ((long)ls.tbl.len > c->fxTblMax) c->fxTblMax = (long)ls.tbl.len;
        for (size_t i = c->storeBase; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (st && st->fn == f) c->fxTailRecords++;
        }
        if (getenv("EXTC_DBG_FX_VERBOSE"))
            fprintf(stderr, "[fx] %-24s tbl=%-5zu seen=%-9ld own=%-9ld symCmp=%-12ld"
                            " rounds=%ld/%ld moved=%ld\n",
                    f->name ? f->name : "?", ls.tbl.len, c->fxStoresSeen, c->fxStoresOwn,
                    c->fxSymCmp, c->fxLvlRounds1, c->fxLvlRounds2, c->fxLvlMoved);
    }
}



/* ------------------------------------------------ opened and never closed (warn)
 *
 * A file descriptor is an operating-system resource, not memory: nothing closes it on
 * the program's behalf, and the process holds it until it ends. The compiler cannot see
 * the kernel's table, but there is one shape of leak it *can* prove -- a handle that
 * never leaves the function and is never closed -- and that shape is a **warning** here
 * (decision 79). Everything else stays quiet on purpose: a handle handed to another
 * function, returned, or stored may well be closed there, and guessing would reject
 * correct programs.
 *
 * Why a warning and not an error: keeping a handle open until the process ends is a
 * legal choice (a log file, a pipe the program writes to and lets the OS reap), and the
 * compiler has no way to tell that intention from a mistake -- what it has is proof that
 * *nobody* closes it. So the proof is reported, loudly and with a position, and the
 * decision stays with the program. `-w` turns it off like any other warning.
 *
 * What makes a type a resource is the library's own protocol: **a struct that declares a
 * `close` method**. No library name appears in the compiler, the same way `slice` is a
 * protocol (`data` + `len`) and not a name codegen recognizes. `std::io`'s `writer` has
 * only `flush` and its `reader` has no `close`, so neither is a resource -- opting in is
 * a one-word decision the library makes.
 */
typedef struct {
    const char *cname;     /* the generated-C name: unique inside one function */
    const char *name;      /* the name the user wrote, for the diagnostic */
    int         line;
    bool        closed;    /* some `close` call can reach it */
    bool        escaped;   /* handed on, returned or stored: not our business */
    Vec         aliases;   /* const char*: `let g = f` shares one obligation */
} FdOblig;

/* True when the type declares a `close` method: the resource protocol. */
static bool typeIsResource(Type *t) {
    Type *b = t ? ttBase(t) : NULL;
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return false;
    for (size_t i = 0; i < b->sdef->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&b->sdef->methods, i);
        if (m && m->name && strcmp(m->name, "close") == 0) return true;
    }
    return false;
}

static FdOblig *obligFind(Vec *obs, const char *cname) {
    if (!cname) return NULL;
    for (size_t i = 0; i < obs->len; i++) {
        FdOblig *o = *(FdOblig **)vecAt(obs, i);
        if (o->cname && strcmp(o->cname, cname) == 0) return o;
        for (size_t j = 0; j < o->aliases.len; j++)
            if (strcmp(*(const char **)vecAt(&o->aliases, j), cname) == 0) return o;
    }
    return NULL;
}

/* Every obligation is out of our hands. Used for a shape this walk does not know: a
 * missed leak is a missing diagnostic, a wrong one rejects a correct program, so the
 * unknown direction is silence. */
static void obligEscapeAll(Vec *obs) {
    for (size_t i = 0; i < obs->len; i++) (*(FdOblig **)vecAt(obs, i))->escaped = true;
}

/* `escape` says whether the value in this position leaves the function's hands. */
static void obligExpr(Checker *c, Expr *e, Vec *obs, bool escape) {
    if (!e) return;
    switch (e->kind) {
    case EX_IDENT: {
        FdOblig *o = obligFind(obs, e->u.ident.cname);
        if (o && escape) o->escaped = true;
        return;
    }
    case EX_METHOD: {
        /* `f.close()` is the discharge. The receiver is not an escape: `f.put(x)` writes
         * through the handle and leaves the file exactly as open as it was. */
        Expr *recv = e->u.method.recv;
        if (recv && recv->kind == EX_IDENT && e->u.method.name &&
            strcmp(e->u.method.name, "close") == 0) {
            FdOblig *o = obligFind(obs, recv->u.ident.cname);
            if (o) o->closed = true;
        }
        obligExpr(c, recv, obs, false);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.method.args, i), obs, true);
        return;
    }
    case EX_FIELD:
        /* `f.fd`: reading a field of the handle does not hand the handle on. */
        obligExpr(c, e->u.field.obj, obs, false);
        return;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.call.args, i), obs, true);
        if (e->u.call.callee) obligExpr(c, e->u.call.callee, obs, false);
        return;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.assoc.args, i), obs, true);
        return;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.gencall.args, i), obs, true);
        return;
    case EX_DYN:
        obligExpr(c, e->u.dynv.payload, obs, true);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.enumval.args, i), obs, true);
        return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            obligExpr(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, obs, true);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            obligExpr(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value, obs, true);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.arraylit.elems, i), obs, true);
        return;
    case EX_INDEX:
        obligExpr(c, e->u.index.obj, obs, true);
        obligExpr(c, e->u.index.index, obs, true);
        return;
    case EX_SLICE:
        obligExpr(c, e->u.slice.obj, obs, true);
        obligExpr(c, e->u.slice.lo, obs, true);
        obligExpr(c, e->u.slice.hi, obs, true);
        return;
    /* The value passes straight through these, keeping whatever the position means. */
    case EX_BIN:      obligExpr(c, e->u.bin.left, obs, escape);
                      obligExpr(c, e->u.bin.right, obs, escape); return;
    case EX_UN:       obligExpr(c, e->u.un.operand, obs, escape); return;
    case EX_REF:      obligExpr(c, e->u.ref.operand, obs, escape); return;
    case EX_EXT: obligExpr(c, e->u.ext_.call, obs, escape); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:    obligExpr(c, e->u.deref.operand, obs, escape); return;
    case EX_SIGN:     obligExpr(c, e->u.sign.operand, obs, escape); return;
    case EX_TRY:      obligExpr(c, e->u.try_.operand, obs, escape); return;
    case EX_CONV:     obligExpr(c, e->u.conv.operand, obs, escape); return;
    case EX_COALESCE: obligExpr(c, e->u.coalesce.main, obs, escape);
                      obligExpr(c, e->u.coalesce.fallback, obs, escape); return;
    case EX_NEW:      obligExpr(c, e->u.new_.count, obs, true); return;
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL: return;
    default:          obligEscapeAll(obs); return;   /* an unknown shape: stay quiet */
    }
}

static void obligStmt(Checker *c, Stmt *s, Vec *obs) {
    if (!s) return;
    switch (s->kind) {
    case ST_TRAP:
        obligExpr(c, s->u.trap_.msg, obs, false);
        return;
    case ST_YIELD:
        obligExpr(c, s->u.yield_.value, obs, true);
        return;
    case ST_VAR: {
        /* A local of a resource type is where the obligation is born. An initializer that
         * is another handle makes this one an alias: one `close` serves both, because
         * both name the same descriptor. */
        if (typeIsResource(s->type)) {
            const char *cn = s->u.var.cname ? s->u.var.cname : s->u.var.name;
            Expr *init = s->u.var.init;
            FdOblig *same = (init && init->kind == EX_IDENT)
                          ? obligFind(obs, init->u.ident.cname) : NULL;
            if (same) {
                *(const char **)vecPush(&same->aliases) = cn;
            } else {
                FdOblig *o = (FdOblig *)arenaAllocZero(c->arena, sizeof *o);
                o->cname = cn;
                o->name  = s->u.var.name;
                o->line  = s->line;
                vecInit(&o->aliases, c->arena, sizeof(const char *));
                *(FdOblig **)vecPush(obs) = o;
            }
        }
        obligExpr(c, s->u.var.init, obs, false);
        return;
    }
    case ST_ASSIGN: {
        /* Overwriting a handle loses the old descriptor, which is a leak as well -- but
         * the old value may have been closed by whoever handed the new one over, so the
         * safe direction here is silence. The value being stored is out of our hands. */
        if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
            FdOblig *o = obligFind(obs, s->u.assign.target->u.ident.cname);
            if (o) o->escaped = true;
        }
        obligExpr(c, s->u.assign.value, obs, true);
        return;
    }
    case ST_DOMAIN: obligExpr(c, s->u.domain_.callee, obs, false);
                    obligStmt(c, s->u.domain_.body, obs); return;
    case ST_IF:    obligExpr(c, s->u.ifs.cond, obs, false);
                   obligStmt(c, s->u.ifs.thenBody, obs);
                   obligStmt(c, s->u.ifs.elseBody, obs); return;
    case ST_WHILE: obligExpr(c, s->u.whiles.cond, obs, false);
                   obligStmt(c, s->u.whiles.body, obs); return;
    case ST_RETURN: obligExpr(c, s->u.ret.value, obs, true); return;   /* handed to the caller */
    case ST_EXPR:  obligExpr(c, s->u.expr.expr, obs, false); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            obligStmt(c, *(Stmt **)vecAt(&s->u.block.stmts, i), obs);
        return;
    case ST_MATCH:
        obligExpr(c, s->u.match.scrutinee, obs, true);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            obligStmt(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body, obs);
        return;
    case ST_BREAK: case ST_CONTINUE:
    default: return;
    }
}

/* Warn about every descriptor this function opens and never closes.
 *
 * The walk is flow-insensitive on purpose: a `close` anywhere in the function counts.
 * Proving that a particular *path* closes it needs a path-sensitive analysis, and
 * rejecting a program that closes its file on one of two paths would be a false alarm.
 * The accepted shapes are still really closed at run time, because a second `close` is a
 * no-op in the library (the handle's `open` flag is already false).
 */
static void checkOpenHandles(Checker *c, FuncDef *f) {
    if (!f || !f->body || f->isExtern) return;
    Vec obs; vecInit(&obs, c->arena, sizeof(FdOblig *));
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        obligStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i), &obs);
    for (size_t i = 0; i < obs.len; i++) {
        FdOblig *o = *(FdOblig **)vecAt(&obs, i);
        if (o->closed || o->escaped) continue;
        ckWarn(c, o->line,
               "This is a resource the operating system hands out, not memory: nothing gives it"
               " back for you, and the process holds it until it ends -- a descriptor, a"
               " terminal left in raw mode. Close it where its life ends (`close()` on the"
               " value), or hand it to a function that takes over. (`-w` turns this warning"
               " off; keeping it until the process ends is a legal choice, it just has to be a"
               " visible one.)",
               "`%s` is opened here and nothing in this function closes it", o->name);
    }
}

/* name -> symbols, built incrementally, for the sweep at the end of checkFunc.
 *
 * That sweep walks *every* symbol of the module for *every* function (the vecAt read histogram:
 * 21,102,050 reads, ~6,400 per function at N=3200 -- the checker's x4.3). Its effect is "each
 * symbol's refDepth = max(refDepth, dataflow depth of its name)", which can be produced the other
 * way round if the symbols of a name can be found quickly.
 *
 * The first attempt rebuilt a flat index per module and was 2.6x SLOWER: `allSyms` grows while
 * checking, so a length-guarded full rebuild turned the O(N) build into O(N^2). This one appends
 * only the symbols added since the last call (amortised O(1) each, arena-backed Vec), and falls
 * back to the original loop -- permanently, for the rest of the run -- the moment anything is not
 * exactly the list it indexed (table window full, list shrank, different arena/first symbol). */
typedef struct { const char *name; Vec syms; int used; } SymBucket;
static SymBucket *g_sb;
static size_t g_sbCap, g_sbBuilt;
static Arena *g_sbArena;
static const Sym *g_sbFirst;
static int g_sbBad;

static size_t sgHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (const char *q = p; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211u; }
    return h;
}
static SymBucket *symBucket(const char *name, int create) {
    size_t h = sgHash(name) & (g_sbCap - 1);
    for (int step = 0; step < 16; step++, h = (h + 1) & (g_sbCap - 1)) {
        SymBucket *b = &g_sb[h];
        if (b->used) {
            if (strcmp(b->name, name) == 0) return b;
            continue;
        }
        if (!create) return NULL;
        b->used = 1; b->name = name;
        vecInit(&b->syms, g_sbArena, sizeof(Sym *));
        return b;
    }
    return NULL;                                   /* window full: caller falls back */
}
static void symIdxSync(Checker *c) {
    const Sym *first = c->allSyms.len ? *(Sym **)vecAt(&c->allSyms, 0) : NULL;
    if (g_sbArena != c->arena || g_sbFirst != first || c->allSyms.len < g_sbBuilt) {
        size_t cap = 4096;
        g_sb = arenaAllocZero(c->arena, cap * sizeof *g_sb);
        g_sbCap = cap; g_sbBuilt = 0; g_sbBad = 0;
        g_sbArena = c->arena; g_sbFirst = first;
    }
    if (g_sbBad) return;
    for (size_t i = g_sbBuilt; i < c->allSyms.len; i++) {
        Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
        if (!sy || sy->line < 0 || !sy->cname) continue;
        SymBucket *b = symBucket(sy->cname, 1);
        if (!b) { g_sbBad = 1; return; }           /* incomplete: never use it again */
        *(Sym **)vecPush(&b->syms) = sy;
    }
    g_sbBuilt = c->allSyms.len;
}

/* The coroutine setup for one function: the **handle** protocol (emitted once per program) and,
 * for this function, the frame type a call produces plus the protocol methods on it.
 *
 * Extracted from `checkFunc` so it can also be run for an **instance created late**: a call inside a
 * generic body is deferred (`fn run<T>() { gen(v) }`), so `funcInstance` for `gen<i64>` runs after
 * the body-checking pass, which used to be the only place this setup happened. The instance then
 * drove `<name>$frame` while nothing defined it, and `value()` kept the unsubstituted `T`
 * (tools/attack.py F4 + B7). */
static void coroSetup(Checker *c, FuncDef *f) {

        /* ---- The **handle** protocol, once per program ----
         * `coroutine<T>` is a storage type too (one surface type; the representation is chosen by
         * escape). Driving a handle is an ordinary method call on the marker type, with `T`
         * substituted by the existing instantiation machinery -- so `value()` returns each instance's
         * own `T`. Same shape as the frame's methods below; the difference is only that `self` is the
         * handle, which is why the two get different `coroProto` values and codegen dispatches on
         * `kind`. */
        StructDef *hsd = structOf(f->ret);
        if (hsd && hsd->methods.len == 0) {
            /* The protocol methods name the marker's **own** type parameters: substitution matches on
             * the parameter *name* (`ttSubstitute` compares `param`), so a synthetic `T` would stop
             * matching the moment the declaration is spelled `coroutine<A, B>`. `A` is what `send`
             * takes in, `B` is what `value` hands back; a one-argument `coroutine<T>` leaves `B`
             * unbound, and substitution falls back to `A` (types.c). */
            const char *an = hsd->typeParams.len > 0
                ? *(const char **)vecAt(&hsd->typeParams, 0) : "A";
            const char *bn = hsd->typeParams.len > 1
                ? *(const char **)vecAt(&hsd->typeParams, 1) : an;
            Type *tpA = arenaAllocZero(c->arena, sizeof *tpA);
            tpA->kind = TY_PARAM; tpA->name = an; tpA->param = an;
            vecInit(&tpA->targs, c->arena, sizeof(Type *));
            Type *tpB = arenaAllocZero(c->arena, sizeof *tpB);
            tpB->kind = TY_PARAM; tpB->name = bn; tpB->param = bn;
            vecInit(&tpB->targs, c->arena, sizeof(Type *));
            Type *hinst = arenaAllocZero(c->arena, sizeof *hinst);
            hinst->kind = TY_GENERIC;      /* an *instance* with targs: that is what `coroutine<A,B>` is */
            hinst->name = hsd->name;
            hinst->sdef = hsd;
            vecInit(&hinst->targs, c->arena, sizeof(Type *));
            *(Type **)vecPush(&hinst->targs) = tpA;
            *(Type **)vecPush(&hinst->targs) = tpB;
            const int hw[3] = { 3, 4, 6 };    /* next / value / send on the handle */
            for (int hi = 0; hi < 3; hi++) {
                const int which = hw[hi];
                FuncDef *pm = arenaAllocZero(c->arena, sizeof *pm);
                pm->name      = which == 3 ? "next" : which == 4 ? "value" : "send";
                pm->owner     = hsd;
                pm->modName   = f->modName;
                pm->line      = f->line;
                planSetCoroProto(pm, which);
                pm->ret       = which == 4 ? tpB : ttFromName(c->tt, "bool");
                vecInit(&pm->params, c->arena, sizeof(Param *));
                Param *self = arenaAllocZero(c->arena, sizeof *self);
                self->name = self->cname = "self";
                self->line = f->line;
                Type *rt = arenaAllocZero(c->arena, sizeof *rt);
                rt->kind = TY_REF;
                rt->inner = hinst;
                rt->mut = which == 3;      /* `next` advances the state, `value` only reads it */
                self->type = rt;
                *(Param **)vecPush(&pm->params) = self;
                if (which == 6) {
                    Param *vp = arenaAllocZero(c->arena, sizeof *vp);
                    vp->name = vp->cname = "v";
                    vp->type = tpA;            /* `send(v: A)`: what goes **into** the coroutine */
                    vp->line = f->line;
                    *(Param **)vecPush(&pm->params) = vp;
                }
                *(FuncDef **)vecPush(&hsd->methods) = pm;
            }
        }
        /* A call to a coroutine produces **that coroutine's own frame**, a concrete value type the
         * compiler synthesizes here -- before the body is checked, so call sites and the callee
         * agree. `coroutine<T>` stays what it always was: how a body declares "I am a coroutine and
         * I yield T". See docs/topics/CONCURRENCY.md 4.4. */
        StructDef *cfd = arenaAllocZero(c->arena, sizeof *cfd);
        /* A C name of its own: the frame is emitted through the type channel now, so it must not
         * collide with the function's own name. */
        const char *frameOwner = (planInstName(f) && planInstName(f)[0]) ? planInstName(f)
                                : (f->name ? f->name : "coro");
        cfd->name = arenaPrintf(c->arena, "%s$frame", frameOwner);
        cfd->coroOf = f;
        vecInit(&cfd->typeParams, c->arena, sizeof(const char *));
        vecInit(&cfd->fields, c->arena, sizeof(FieldDef *));
        vecInit(&cfd->methods, c->arena, sizeof(FuncDef *));
        Type *cft = arenaAllocZero(c->arena, sizeof *cft);
        cft->kind = TY_STRUCT;
        cft->name = cfd->name;
        cft->sdef = cfd;
        vecInit(&cft->targs, c->arena, sizeof(Type *));
        /* The protocol, as **real methods** on that type: `next(self: mut ref F) -> bool` and
         * `value(self: ref F) -> T`. Driving a coroutine is then ordinary method resolution -- no
         * special case in the checker -- and `fn drive<C>(c: mut ref C)` works at instantiation
         * through the existing deferred-method machinery (#57). Codegen emits the two inline. */
        for (int which = 1; which <= 3; which++) {
            FuncDef *pm = arenaAllocZero(c->arena, sizeof *pm);
            pm->name     = which == 1 ? "next" : which == 2 ? "value" : "send";
            pm->owner    = cfd;
            pm->modName  = f->modName;
            pm->line     = f->line;
            planSetCoroProto(pm, which == 3 ? 5 : which);   /* 5 = `send` on the frame (the handle's is 6) */
            pm->ret      = which == 2 ? planYieldType(f) : ttFromName(c->tt, "bool");
            vecInit(&pm->params, c->arena, sizeof(Param *));
            Param *self = arenaAllocZero(c->arena, sizeof *self);
            self->name = self->cname = "self";
            self->line = f->line;
            Type *rt = arenaAllocZero(c->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = cft;
            rt->mut = which != 2;              /* `next`/`send` advance the state, `value` reads */
            self->type = rt;
            *(Param **)vecPush(&pm->params) = self;
            if (which == 3) {
                /* `send(self: mut ref F, v: T) -> bool`: hand a value to the next resume, which the
                 * coroutine reads through a `var x = yield e` binding (the frame's `in` slot). */
                Param *vp = arenaAllocZero(c->arena, sizeof *vp);
                vp->name = vp->cname = "v";
                /* `send(v: A)`: A is the marker's **first** type argument (`coroutine<A, B>`); the
                 * one-argument shorthand leaves A = B = the yield type, which is why a corpus file
                 * that only ever yielded keeps type-checking unchanged. */
                vp->type = f->ret->targs.len > 0
                    ? *(Type **)vecAt(&f->ret->targs, 0) : planYieldType(f);
                vp->line = f->line;
                *(Param **)vecPush(&pm->params) = vp;
            }
            *(FuncDef **)vecPush(&cfd->methods) = pm;
        }
        planSetCoroFrameType(f, cft);
        f->ret = cft;
    
}

static void checkFunc(Checker *c, FuncDef *f) {
    /* `@builtin` 声明只有签名、没有函数体（与 `extern!` 同款），而这条路后面会直接遍历
     * `f->body` ⇒ 无体声明一进来就段错误（2026-09-28 实测：exit 139，bt 落在下面那行 for）。
     * 内建的实现由编译器在**调用点**给出，所以这里什么都不用查；对它的调用由 check_expr 里的
     * "尚未实现" 守卫负责拒绝。只挡内建，不动 extern 的既有行为。 */
    if (f->isBuiltin && !f->body) return;
    /* A lambda's `call` method: its body was checked where the lambda was written, because that is
     * where the capture set comes from (check_expr.c, checkLambda). */
    if (f->lamChecked) return;

    /* The coroutine protocols (`next`/`value`/`send`) have no body: they are checked inline at
     * their call sites. Checking one here walked a null body. */
    if (planCoroProto(f)) return;
    /* More parameters than the effect summary has bits would silently lose the high ones. That is
     * exactly the audit's P1-4: the same store shape with 4 parameters was rejected while its
     * 40-parameter twin was accepted, because parameter 40's `addrMask` bit fell off the end of a
     * 32-bit mask. The mask is 64 bits now, and this is the contract on top of it: refusing loudly
     * beats ignoring a parameter. A function that needs more has to be split (an array-backed
     * summary is the deliberate later step if a real program ever asks for it). */
    if (f->params.len > EFF_MAX_PARAMS) {
        ckError(c, f->line,
                "The effect summary indexes parameters by position in a 64-bit mask, so the"
                " compiler refuses more than 64 of them instead of silently ignoring the rest"
                " (audit P1-4). Split the function.",
                "`%s` has %zu parameters; the compiler's limit is %d",
                f->name ? f->name : "this function", f->params.len, (int)EFF_MAX_PARAMS);
        return;
    }
    /* An extern declaration has no body, so only its signature is checked: the parameter
     * types were already resolved elsewhere, and no arena is involved, since the function
     * takes no arena parameter and needs no home arena (the arena the caller passes). Its
     * summary comes from the signed clause, or is the worst case (see `collectEffects`
     * below). */
    if (f->isExtern) {
        planSetMayUseArena(f, false);
        f->needsHome   = false;
        /* Only scalars and single pointers may cross the boundary. A `slice<T>` would
        * become two C arguments (data and length), so the names would not match, and a
        * struct has no frozen layout; the stdlib wrappers pass `s.data` and `s.len`
        * explicitly.
        *
        * The predicate itself lives in `ttCrossesC`: one spelling, shared with the `fn` type
        * (`ttResolve`), which is the other way a C function is reached. It used to be spelled out
        * here twice, with a branch that could never be taken -- the test asked
        * `t->kind == TY_REF` about a type `ttBase` had already stripped every reference from. */
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (!ttCrossesC(p->type, false))
                ckError(c, p->line,
                        "A C argument is one machine word: a scalar, a pointer, or a function"
                        " pointer; a `slice<T>` becomes two C arguments (data + len), and a struct"
                        " crosses **by value** only when its layout is promised (`@frozen` on its"
                        " declaration). Otherwise wrap it in an extC function that passes `s.data` /"
                        " `s.len` explicitly.",
                        "`extern!` argument %zu has type `%s`, which cannot cross the C boundary",
                        i + 1, typeStr(c, p->type));
        }
        {
            /* `void` is a C return type like any other -- `exit`, `free`, `srand` and
             * `cfmakeraw` all have it, and refusing it left the privileged layer unable to
             * declare them. What has no C-level representation is a struct, a slice (two
             * arguments), or a view. */
            /* A missing return type is a different mistake from a type that cannot cross, and the
             * message used to be the same for both -- it printed `void` for a NULL return type, so
             * `fn f(x: i32)` looked like "`void` cannot cross the C boundary" (measured while
             * writing the cairo bindings: 16 declarations in a row said exactly that). */
            if (!f->ret)
                ckError(c, f->line,
                        "A declaration has to say what it returns; `-> void` if it returns nothing.",
                        "`extern!` declaration `%s` has no return type", f->name);
            else if (!ttCrossesC(f->ret, true))
                ckError(c, f->line,
                        "A C return value is one machine word -- a scalar, a pointer, or a function"
                        " pointer -- or a `@frozen` struct, whose layout the declaration promises.",
                        "`extern!` return type `%s` cannot cross the C boundary",
                        typeStr(c, f->ret));
        }
        double tE = ctNow();
        collectEffects(c, f);
        ctPhase("cf-effects", tE);            /* summary = the signed clause, or the worst case */
        return;
    }
    /* `main(args)`: the entry point may take the command line, and the shape is fixed
     * (docs/topics/IO.md section 7): one parameter, `slice<slice<u8>>`. C hands the entry
     * point `argc` and `argv`, and codegen wraps those two into the view; any other
     * parameter has no C-level meaning, because C's `main` takes integers and pointers and
     * nothing else. Saying so here keeps the diagnostic in extC instead of letting the C
     * compiler complain about the wrapper.
     *
     * The return type is C's `int`, so it may be absent or `i32`; `-> i64` would be
     * truncated silently by the C compiler, which is exactly the kind of thing that must not
     * be silent. */
    if (!f->owner && strcmp(f->name, "main") == 0) {
        Type *r = f->ret ? ttBase(f->ret) : NULL;
        bool retOk = !r || r->kind == TY_VOID ||
                     (r->kind == TY_BUILTIN && r->name && strcmp(r->name, "i32") == 0);
        if (!retOk)
            ckError(c, f->line,
                    "C's entry point returns an `int`, so `main` may return `i32` (or nothing at"
                    " all). Any other type would be converted by the C compiler without a word.",
                    "`main` cannot return `%s`", typeStr(c, f->ret));
        if (f->params.len > 1)
            ckError(c, f->line,
                    "The command line is one view of the arguments. Wrap several pieces of"
                    " information in a struct if the entry point needs more.",
                    "`main` takes at most one parameter, `slice<slice<u8>>`; %zu were declared",
                    f->params.len);
        else if (f->params.len == 1) {
            Param *p = *(Param **)vecAt(&f->params, 0);
            /* The shape is judged by shape and not through `ttBase`: resolving a view
             * lands on the struct behind it, which no longer says `slice`. */
            Type *eo = ttIsViewType(p->type) ? *(Type **)vecAt(&p->type->targs, 0) : NULL;
            Type *ei = eo && ttIsViewType(eo) ? ttBase(*(Type **)vecAt(&eo->targs, 0)) : NULL;
            bool ok = ei && ei->kind == TY_BUILTIN && ei->name && strcmp(ei->name, "u8") == 0;
            if (!ok)
                ckError(c, p->line,
                        "The arguments arrive as bytes, the way the operating system hands them"
                        " over, so the entry point takes `slice<slice<u8>>`: the outer view is"
                        " `args`, and `args[0]` is the program name, again as C defines it.",
                        "`main`'s parameter must be `slice<slice<u8>>`, found `%s`",
                        typeStr(c, p->type));
        }
    }
    /* Every diagnostic inside this body has to name the file the body came from. The
     * loader records that context on each declaration (`FuncDef.ctx`), and this pass runs
     * every unit's functions in one go, so the context is switched here: without it, a
     * mistake inside a module came back pointing at a line of the **entry** file -- the
     * line was the module's, the file was not (PLAN #54). */
    Ctx *savedCtx = c->ctx;
    if (f->ctx) c->ctx = f->ctx;

    FuncDef *savedFunc = c->curFunc;
    /* A function that allocates and hands back something useful (a reference or a view,
    * or a store through an out-parameter) needs a home arena. One that returns `i32`
    * does not: its allocations stay in its own block. */
    if (!f->isExtern && stmtHasNew(f->body) &&
        ((f->ret && typeContainsRef(c->tt, f->ret)) || stmtStoresThroughDeref(c, f->body, f)))
        f->needsHome = true;

    Vec     *savedParams = c->curParams;

    /* A coroutine: the declared return type is `coroutine<T>` (a prototype in the prelude). The
     * declaration is the marker -- `yield` is legal only here, and the frame the checker lays out
     * for this function is a value (docs/topics/CONCURRENCY.md 4.4). */
    /* `coroutine<T>`（简写：请求与应答同型）与 `coroutine<A, B>` 都算协程：前者一个类型参数，
     * 后者两个。协议方法各自取哪个参数见下面合成那段。 */
    planSetIsCoro(f, f->ret && (isProtoType(f->ret, "coroutine", 1)
                           || isProtoType(f->ret, "coroutine", 2)));
    f->coroRetProto = planIsCoro(f) ? f->ret : NULL;
    /* `coroutine<A, B>`：`B`（最后一个参数）是 yield 出来的类型。简写 `coroutine<T>` 只有一个
     * 参数，那个就是它。 */
    planSetYieldType(f, planIsCoro(f)
        ? *(Type **)vecAt(&f->ret->targs, f->ret->targs.len - 1) : NULL);
    if (planIsCoro(f)) coroSetup(c, f);
    /* **Retired**: this used to reject a parameter typed `coroutine<T>`, on the grounds that the
     * representation was a marker rather than a value. The unified handle (CONCURRENCY.md §4.4) made
     * those parameters legal, the check was switched off (`if (1 || …) continue;` -- which also made
     * everything below it unreachable, error message and all), and the wrapper outlived the rule by
     * long enough to be worth deleting rather than explaining. Git has the history if the old
     * diagnostic is ever needed again. */

    c->curFunc = f;
    /* Instances share their template's body, so the scope of a body's results is the
     * template, not the instance: `wrap<i32>` and `wrap<i64>` write the same nodes. */
    resultsEnterOwner(planTemplate(f) ? planTemplate(f) : f);
    /* The publications of this body start here, and the level pass below folds exactly
     * this slice of `c->stores` -- see `Checker.storeBase`. Nothing recorded before this
     * point belongs to this body, so the pass does not even look at it; the owner field
     * (`StoreSite.fn`) is the second half of the same filter, for the records a nested
     * body appends inside this slice. */
    size_t savedStoreBase = c->storeBase;
    c->storeBase = c->stores.len;
    /* The arena of each call is chosen with the escape set, so it has to be computed
     * before the body is checked. */
    computeEscapes(c, f);
    c->curParams = funcTParams(f);
    /* While walking the body, record every site whose arena is decided only after the
     * analysis closes. */
    vecInit(&c->curArenaSites, c->arena, sizeof(Expr *));
    pushScope(c);
    /* Each function gets its own set of C names, so an `a` in one function cannot affect
     * an `a` in another; they end up in different C function bodies anyway. Checking a
     * generic instance walks the same body a second time, but in the same order, so the
     * generated names are identical and do not drift. */
    c->nameUses.len = 0;

    f->nParamSyms = 0;                    /* a generic body may be walked more than once */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* A parameter is mutable: it is a local copy handed over by the caller, as in C,
         * so `ref real` is allowed inside `fn f(real: Board)`. */
        Sym *sym = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sym->cname;
        /* Record the binding now, while the scope exists: the effect summary runs later, with no
         * scope at all, and needs the identity to attribute a store to the right parameter. */
        if (f->nParamSyms < EFF_MAX_PARAMS) f->paramSyms[f->nParamSyms++] = sym;
    }
    /* The body does not open a scope of its own: parameters and body locals share one.
     * Shadowing a parameter with `let a = ...` is therefore only a new name, which is
     * legal and becomes `a__2` in the generated C, while `var a = ...` declares a second
     * piece of storage and is rejected (see `declare`). */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

    /* The types of the declarations are final by now, so this is where a coroutine's frame can be
     * laid out -- and where rule 2 is enforced on it (docs/topics/CONCURRENCY.md 4.4). */
    coroFrameLay(c, f);

    /* The types of the declarations are final by now, so this is the place to ask
     * whether every handle the function opened also gets closed. */
    checkOpenHandles(c, f);

    /* A parameter the body never reads. extC lets the program keep it and the generated C marks it
     * `unused` so the C compiler stays quiet, but the reader deserves to hear about dead weight in
     * a signature they wrote. `self` is exempt: a method that ignores its receiver is ordinary. */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->name || !p->cname || strcmp(p->name, "self") == 0) continue;
        if (stmtUsesCname(f->body, p->cname)) continue;
        ckWarn(c, p->line,
               "a parameter belongs to the signature, so it is kept and marked `unused` in the"
               " generated C",
               "parameter `%s` is never used", p->name);
    }

    collectEffects(c, f);      /* compute the effect summary; it is only recorded here */
    f->arenaSites = c->curArenaSites;   /* handed to the pass that runs after the analysis closes */

    /* Reference depths, recomputed as a monotone data-flow fixed point over the body.
     *
     * The checking walk above maintains a binding's depth by destructive assignment and
     * has no join at control-flow merges, so at an `if` the arm visited last simply wins.
     * That cannot produce the upper bound the escape check needs, and it is the reason a
     * struct holding an allocation could come back pointing into a block arena. The walk
     * below derives the same numbers from the whole control-flow structure instead:
     * stores raise a depth, merges take the maximum, and loops are iterated to a fixed
     * point. It reads the tree and writes only the depths, so every later reader sees a
     * value that no longer depends on the order branches were visited.
     *
     * See `docs/topics/ARENA-SOUNDNESS.md` section 9.3, item 3-a, and `src/dataflow.c`. */
    {
        DfResult dfr;
        dfAnalyze(c, f, &dfr);
        /* Publications recorded during this body settle the sites they reach. Running
         * here, after the depth fixed point, is the point of the whole split: the
         * decision sees the final depths instead of the numbers that happened to be
         * true while the body was being walked. */
        /* No `dfr`: the level solve reads its own table and runs before the write-back below (see
         * the note on `levelPass`). `EXTC_NO_LEVELPASS=1` skips it -- and, as `--help` now says,
         * that **changes the output**. */
        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, f);
        /* Ask the same question again with the numbers the passes settled on. Reporting
         * stays with the check; this is what makes the two answers comparable. */
        if (dbgOn("EXTC_DBG_DEFER")) recheckLevelRejections(c);
        if (dbgOn("EXTC_DBG_STORES")) {
            int n0 = 0;
            for (size_t i = 0; i < c->stores.len; i++) {
                StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
                fprintf(stderr, "[store] %s: line=%d at=%d kind=%d dst=%s\n",
                        f->name ? f->name : "?", st->line, st->at, (int)st->value->kind,
                        (st->target && st->target->kind == EX_IDENT) ? st->target->u.ident.name
                                                                     : (st->target ? "?" : "none"));
                n0++;
            }
            (void)n0;
        }
        if (dbgOn("EXTC_DBG_DFA")) {
            fprintf(stderr, "[dfa] %s: %d vars%s\n", f->name ? f->name : "?",
                    dfr.nvars, dfr.overflow ? " (OVERFLOW: result unused)" : "");
            for (int i = 0; i < dfr.nvars; i++) {
                fprintf(stderr, "       %-6s depth=%d", dfr.vars[i].cname, dfr.vars[i].depth);
                for (int k = 0; k < dfr.vars[i].nfields; k++)
                    fprintf(stderr, "  .%s=%d", dfr.vars[i].fields[k].name,
                            dfr.vars[i].fields[k].depth);
                fprintf(stderr, "\n");
            }
        }
        /* When the DFA gives up (`dfr.overflow`: a fifth ref-carrying field of one binding, or its
         * round cap) its **whole result is dropped** and the depths stay whatever the checking walk
         * left behind -- the walk this file calls unsound at control-flow merges. So the fallback is
         * real and it is weaker; what it is *not* is rare: `stdlib/stl/string.extc`'s own
         * `string::create` trips it, so a per-function warning here would be ~100% noise and would
         * break the "no false positives on the positive corpus" rule that `tests/warnings` enforces.
         * It is therefore documented rather than announced; `EXTC_DBG_DFA=1` still prints
         * "(OVERFLOW: result unused)" next to the body it happened in. */
        if (!dfr.overflow) {
            symIdxSync(c);
            if (g_sbBad) {
                for (size_t i = 0; i < c->allSyms.len; i++) {      /* exactly the old loop */
                    Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
                    if (!sy || sy->line < 0) continue;
                    int d = dfLookup(&dfr, sy->cname);
                    if (d > sy->refDepth) sy->refDepth = d;
                }
            } else {
                for (int i = 0; i < dfr.nvars; i++) {
                    const char *nm = dfr.vars[i].cname;
                    int d = dfr.vars[i].depth;
                    if (!nm || d < 0) continue;
                    SymBucket *b = symBucket(nm, 0);
                    if (!b) continue;
                    for (size_t k = 0; k < b->syms.len; k++) {
                        Sym *sy = *(Sym **)vecAt(&b->syms, k);
                        if (sy && sy->line >= 0 && d > sy->refDepth) sy->refDepth = d;
                    }
                }
            }
        }
    }
    popScope(c);
    c->curFunc = savedFunc;
    resultsEnterOwner(savedFunc);
    c->curParams = savedParams;
    c->ctx = savedCtx;
    c->storeBase = savedStoreBase;
}

/* Is this initializer a constant expression?
 *
 * Params:
 *   e - initializer expression; may be NULL
 *
 * Returns:
 *   True for a literal, a negated literal, an enum constant, or arithmetic over those.
 *
 * Notes:
 *   - There is no constant folder, so the shape of the expression is what counts:
 *     `let A = 1 + 2` is accepted because both operands are literals, while a call is
 *     rejected however simple it looks.
 */
static bool isConstInit(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: return true;
    case EX_ENUMVAL: return true;                       /* `status.ok` is a constant */
    case EX_BIN:                                         /* `15 * 15`: C folds this */
        return isConstInit(e->u.bin.left) && isConstInit(e->u.bin.right);
    case EX_UN:                                          /* `-1` */
        return e->u.un.operand && isConstInit(e->u.un.operand);
    /* A struct literal whose every field is a constant is one too: nothing in it has
     * to run, and C has always spelled exactly this as a static initializer
     * (`static pt ORIGIN = { .x = 0, .y = 0 };`). An omitted field is the zero value,
     * which is a constant as well.
     *
     * Leaving it out made the one thing a library cannot do otherwise impossible:
     * `let cout: out = out { fd: 1 }` -- the stream object at the top of `std::io`.
     * The rule it was caught by exists for a real reason (a global is a C static
     * object, so its initializer has to be one C can fold), and a literal of
     * constants satisfies that reason; a call does not, and is still rejected. */
    case EX_STRUCTLIT: {
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
            if (fi && fi->value && !isConstInit(fi->value)) return false;
        }
        return true;
    }
    /* A function's **address** is a constant: the linker knows it before the program runs, and C
     * spells it `&f` in a static initializer without any code running. It has to be here for the
     * C-ABI line's table (`C-ABI.md` section 9.9): a table of `fn` fields cannot be zero-initialized
     * (a `fn` has no zero value, which is the whole point), so it must be initialized where it is
     * declared -- and a table belongs at the top level, because the host hands it to a module that
     * may keep it. Without this the only way to build a table was to assign the slots in a function,
     * which put the storage in a frame: exactly the lifetime a table must not have. */
    case EX_IDENT: return e->func != NULL;   /* the checker set it: this name is a function */
    /* `null` is C's `(void *)0`: nothing runs to produce it. It matters for the same reason the
     * line above does -- a table has a `ctx: ?ref void` field, and the slot beside it holds a
     * function address. */
    case EX_NULL:  return true;
    /* A **lossless** conversion of a constant is one: `i64(3)` is `(int64_t)3` in C, which folds,
     * and a table's `size: i64(3)` field is written exactly that way. A conversion the checker marked
     * as needing a runtime check is **not** a constant -- it comes out as a call to a helper
     * (`((uint64_t)extc_narrowU((uint64_t)(4096), ...))`, measured with `let PAGE: u64 = u64(4096)`),
     * and gcc refuses that in a static initializer. Saying so here means the user gets "must be
     * initialized with a constant" instead of a gcc error in the product. */
    case EX_CONV: return !e->convCheck && isConstInit(e->u.conv.operand);
    default: return false;
    }
}
/* Check the top-level `let` and `var` declarations.
 *
 * A global is depth 0: it outlives every frame. Two consequences follow from that one fact
 * without any special rule for globals: the escape rule already rejects storing something
 * local into a global, and a global of fixed size needs no arena, because it is a C static
 * object.
 *
 * Params:
 *   c - checker
 *
 * Notes:
 *   - An initializer has to be a constant, because C's static initializer may only contain
 *     constant expressions.
 */
static void checkGlobals(Checker *c) {
    Module *m = c->m;
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&m->globals, i);

        /* A global may not share a name with another global or with a function. */
        for (size_t j = 0; j < c->globals.len; j++)
            if (strcmp((*(Sym **)vecAt(&c->globals, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a global", g->name);
        for (size_t j = 0; j < m->funcs.len; j++)
            if (strcmp((*(FuncDef **)vecAt(&m->funcs, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a function", g->name);

        if (g->ann) g->ann = ttResolve(c->tt, c->ctx, g->ann, g->line, NULL);
        if (ttIsError(g->ann)) g->ann = NULL;

        if (!g->init) {
            /* Zero initialization: C clears static storage duration on its own, which is
             * what an uninitialized global means. A type containing a reference has no zero
             * value, though, and a global is depth 0, so there is nothing to borrow. */
            if (g->ann && typeContainsRef(c->tt, g->ann))
                ckError(c, g->line,
                        "a global is depth 0, so a reference inside it has nothing to "
                        "borrow from without an initializer.",
                        "cannot zero-initialize the global `%s`: it contains a reference",
                        g->name);
            if (!g->ann) g->ann = ttError(c->tt);
        } else {
            /* `noHoist` is deliberately not set for a global: its initializer is already
             * governed by the more fundamental rule that it must be a constant (C static
             * initialization; a global is a C static object). Reporting that rule is much
             * clearer than letting `??` report that there is nowhere to put a temporary.
             * The `get()` in `get() ?? -1` is not a constant anyway, so `??` is
             * irrelevant. */
            Type *it = checkValue(c, g->init);
            Type *declT = g->ann ? g->ann : it;
            if (g->ann) checkAssignable(c, g->ann, it, g->init, "initializer");

            /* Must run after the type check: `color.empty` is only rewritten into an enum
             * constant node (EX_ENUMVAL) by then, and before that it is still an
             * EX_FIELD. */
            if (!isConstInit(g->init)) {
                ckError(c, g->line,
                        "A global exists before any function runs, so its initializer has to be "
                        "a constant (a literal, an enum constant, or arithmetic on those). "
                        "Anything computed belongs inside a function.",
                        "the global `%s` must be initialized with a constant", g->name);
                continue;
            }

            /* Depth 0, so the escape rule applies on its own: storing something local into
             * a global is rejected. */
            checkEscape(c, g->init, 0, g->line, "this global");

            g->ann = ttIsError(declT) ? ttError(c->tt) : declT;
        }

        Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
        s->name  = g->name;
        s->type  = g->ann;
        s->mut   = g->mut;
        s->depth = 0;                     /* a global is depth 0 */
        s->line  = g->line;
        s->modName = g->modName;          /* the module that declared it, for qualified names */
        *(Sym **)vecPush(&c->globals) = s;
    }
}

/* Create, or find, the concrete instance of a generic function.
 *
 * Why:
 *   An instance has to be born at the call site: a type instance such as
 *   `varArray<i32>` is decided by the type, while the type arguments of a free function
 *   are known only where it is called.
 *
 * Params:
 *   c     - checker, used for the arena and for substitution
 *   tmpl  - the template function
 *   targs - one type argument per template parameter
 *   line  - source line of the instantiation; currently unused
 *
 * Returns:
 *   One instance per template and set of type arguments: a match is reused, otherwise a
 *   new instance is allocated. NULL when `tmpl` is NULL.
 *
 * Notes:
 *   - The instance is a FuncDef of its own: `tmpl` points back at the template, `targs`
 *     and `instName` are filled in, and the parameter and return types are substituted,
 *     so codegen emits it like any other function. Entering it still has to open the
 *     substitution of the template's type parameters.
 *   - The instance is appended to `funcInsts` for reuse, and to the module's function
 *     list for codegen.
 */
#define FUNC_INST_PREFIX "__extc_fi_"

/* How many concrete instances of free generic functions one compilation may build.
 *
 * A template can be instantiated inside its own body with a bigger type -- `grow<T>(x)` calling
 * `grow(box<T>)` -- and nothing stopped it: the compiler kept creating instances until it was
 * killed (tools/attack.py E3: extC did not return within 25 seconds on a three-line program).
 * This is a limit of the compiler, not of the language, so the message says so. */
#define FUNC_INST_LIMIT 4096

/* How deeply a **created** type may nest. The runaway in E3 does not just add instances: each
 * step instantiates with a type one level deeper (`box<T>` again), so by step N the type is N
 * levels deep and every copy of it costs O(N) -- the total is O(N^2) and the compiler was
 * OOM-killed after 54 seconds. Depth is the quantity that has to be bounded; the instance count
 * above is only a backstop. 64 is far above anything a source file can write by hand
 * (the attack suite's A4 uses 10) and far below anything that can exhaust memory. */
#define TYPE_DEPTH_LIMIT 64

/* Nesting depth of a type's instantiation arguments (a plain type is 0). The recursion is cut
 * defensively: this runs on types the compiler built itself. */
static int typeDepth(Type *t) {
    if (!t) return 0;
    int best = 0;
    for (size_t i = 0; i < t->targs.len; i++) {
        int d = typeDepth(*(Type **)vecAt(&t->targs, i));
        if (d > best) best = d;
    }
    return best + (t->targs.len ? 1 : 0);
}

/* Create or find the concrete instance of a free generic function.
 *
 * A type instance (`varArray<i32>`) is decided by the type, but the type arguments of a
 * free function are read off the call, so an instance can only be created there.
 *
 * Params:
 *   c     - checker; instances are appended to `c->funcInsts` and to the module
 *   tmpl  - the generic template; NULL yields NULL
 *   targs - concrete type arguments; the same list always maps to the same instance
 *   line  - source line of the call; unused, kept for the call shape
 *
 * Returns:
 *   An independent `FuncDef` whose `tmpl` points back at the template, whose `targs` and
 *   `instName` are filled in, and whose parameter and return types are substituted.
 *   Codegen emits it like an ordinary function.
 */
FuncDef *funcInstance(Checker *c, FuncDef *tmpl, Vec *targs, int line) {
    if (!tmpl) return NULL;
    for (size_t j = 0; targs && j < targs->len; j++) {
        int d = typeDepth(*(Type **)vecAt(targs, j));
        if (d > TYPE_DEPTH_LIMIT) {
            ctxError(c->ctx, line, 1,
                     "This is a limit of the compiler, not something wrong with the program;"
                     " please report it together with the program that triggered it.",
                     "internal: a generic instance was requested with a type nested %d levels"
                     " deep (a template instantiated inside its own body with a growing type?)", d);
            return NULL;
        }
    }
    if (c->funcInsts.len >= FUNC_INST_LIMIT) {
        ctxError(c->ctx, line, 1,
                 "This is a limit of the compiler, not something wrong with the program;"
                 " please report it together with the program that triggered it.",
                 "internal: more than %d generic function instances were created"
                 " (a template instantiated inside its own body with a growing type?)",
                 FUNC_INST_LIMIT);
        return NULL;
    }
    /* Already built? One template plus one set of type arguments is one instance, so a
     * match is returned instead of allocating a second one. */
    for (size_t i = 0; i < c->funcInsts.len; i++) {
        FuncDef *in = *(FuncDef **)vecAt(&c->funcInsts, i);
        if (planTemplate(in) != tmpl || in->targs.len != targs->len) continue;
        bool same = true;
        for (size_t j = 0; j < targs->len; j++)
            if (!ttEquals(*(Type **)vecAt(&in->targs, j), *(Type **)vecAt(targs, j))) { same = false; break; }
        if (same) return in;
    }
    FuncDef *in = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    *in = *tmpl;                        /* shallow copy: shares the body (as method instances do) */
    planSetTemplate(in, tmpl);          /* the back pointer lives in the plan, not on the node */
    in->used = false;
    vecInit(&in->targs, c->arena, sizeof(void *));
    for (size_t j = 0; j < targs->len; j++)
        *(Type **)vecPush(&in->targs) = *(Type **)vecAt(targs, j);
    /* Substitute the parameter and return types.
     *
     * Each `Param` must be copied first: the pointers were shared with the template, so
     * substituting in place changed the template too, and the second inference saw a
     * template that already held concrete types. That silent corruption showed up as a
     * segfault in testing. */
    {
        Vec newParams;
        vecInit(&newParams, c->arena, sizeof(void *));
        for (size_t j = 0; j < tmpl->params.len; j++) {
            Param *src = *(Param **)vecAt(&tmpl->params, j);
            Param *copy = (Param *)arenaAllocZero(c->arena, sizeof(Param));
            *copy = *src;
            copy->type = ttSubstitute(c->tt, src->type, &tmpl->typeParams, targs);
            *(Param **)vecPush(&newParams) = copy;
        }
        in->params = newParams;
    }
    if (in->ret) in->ret = ttSubstitute(c->tt, in->ret, &tmpl->typeParams, targs);
    if (tmpl->coroRetProto) {
        in->ret = ttSubstitute(c->tt, tmpl->coroRetProto, &tmpl->typeParams, targs);
        planSetCoroFrameType(in, NULL);
        planSetIsCoro(in, false);
    }
    /* C name: `max_i32`, built by mangling the type arguments onto the name. */
    Buf b;
    bufInit(&b, c->arena);
    bufPuts(&b, tmpl->name);
    for (size_t j = 0; j < targs->len; j++) {
        bufPutc(&b, '_');
        bufPuts(&b, ttMangle(c->tt, *(Type **)vecAt(targs, j)));
    }
    planSetInstName(in, bufCstr(&b));
    in->typeParams.len = 0;             /* an instance has no type parameters left */
    *(FuncDef **)vecPush(&c->funcInsts) = in;
    *(FuncDef **)vecPush(&c->m->funcs) = in;   /* codegen emits the definition from this list */
    (void)line;
    return in;
}

/* Fill `targs` in with the type parameters found in a parameter type.
 *
 * Why:
 *   A call to `f<T>(x)` has to infer `T` from the argument types, which is what matching
 *   `slice<T>` against `slice<u8>` does.
 *
 * Params:
 *   tt    - type table
 *   tp    - names of the type parameters, in the order `targs` uses
 *   targs - one slot per type parameter, filled in as unification proceeds; a slot that
 *           is already filled has to agree with the new type
 *   want  - the parameter type, which may mention type parameters
 *   got   - the type of the argument
 *
 * Returns:
 *   false when the parameters cannot be inferred, for example when `T` appears only in
 *   the return type; the call then has to spell them out, as in `f<i32>(...)`.
 */
bool unifyTParams(TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got) {
    if (!want || !got) return true;
    if (want->kind == TY_PARAM) {
        for (size_t i = 0; i < tp->len; i++) {
            if (strcmp(*(const char **)vecAt(tp, i), want->param) != 0) continue;
            Type *cur = *(Type **)vecAt(targs, i);
            if (cur) return ttEquals(cur, got);
            *(Type **)vecAt(targs, i) = got;
            return true;
        }
        return true;
    }
    if (want->kind == TY_REF && got->kind == TY_REF)
        return unifyTParams(tt, tp, targs, want->inner, got->inner);
    if (want->kind == TY_GENERIC && got->kind == TY_GENERIC) {
        if (want->sdef != got->sdef || want->targs.len != got->targs.len) return false;
        for (size_t i = 0; i < want->targs.len; i++)
            if (!unifyTParams(tt, tp, targs, *(Type **)vecAt(&want->targs, i),
                              *(Type **)vecAt(&got->targs, i))) return false;
        return true;
    }
    if (ttHasParam(want)) return false;   /* mentions T but the shapes disagree => cannot infer */
    return true;
}

/* Re-check one deferred operator use for one concrete instance.
 *
 * Both batches of checks that are deferred to instantiation -- this operator batch and the
 * reference-rule batch below -- are driven the same way for type instances and for
 * free-function instances, so they share helpers instead of a second copy for `fn f<T>`.
 *
 * Params:
 *   c - checker
 *   ec - the recorded check, whose operand types still mention type parameters
 *   params - the instance's type parameters
 *   targs - the instance's type arguments, positionally matching `params`
 *   instName - instance name to report in the diagnostic
 *
 * Notes:
 *   - The caller must have set `c.substParams` / `c.substArgs` to this instance before
 *     the call: `runRefCheck` reads them through `tsub`.
 *   - The diagnostic names the operator that was recorded, never a fixed `==`: this one
 *     function serves `==`, the ordering operators and the arithmetic ones.
 */
static void runOpCheck(Checker *c, OpCheck *ec, Vec *params, Vec *targs, const char *instName) {
    TypeTable *tt = c->tt;
    Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type, params, targs);
    Type *rt2 = ttSubstitute(tt, ec->node->u.bin.right->type, params, targs);
    /* A `T op T` in the template is an instance's `A op B` here. The equality of the two is
     * **not** this function's business: `typeSupportsOp` below is the authority on "can this
     * type be compared with that one" -- it accepts native numerics, judges an array by its
     * element type **and the same array type on the other side**, and finds an operator method
     * for structs. An early `return` here used to skip all of that whenever the left operand was
     * not a struct, so a `[1]i32 == i32` slipped through and the emitted descriptor comparison
     * read 8 bytes out of a 4-byte operand (ASan: stack-buffer-overflow in `extc_eq`, found by
     * tools/fuzz.py in a mutation of examples/generic-free-fn.extc). Let the authority decide. */
    if (!typeSupportsOp(tt, lt, ec->op, rt2)) {
        const char *op = ec->op;
        Type *b = ttBase(lt);
        /* An instance the concrete-type path rejects for a better reason than "define a
         * method", so this does not tell the reader to go and write one:
         *   - `%` on a float instance: `%` is for integers;
         *   - an ordering operator on `bool` or a payload-free enum: ordering is for
         *     numbers (the concrete path reports the same thing).
         * Both messages mirror check_expr.c, which is where the concrete rules live. */
        if ((strcmp(op, "%") == 0 && ttIsNumeric(lt)) || (!isEqualityOp(op) && cmpIsNative(lt))) {
            ckError(c, ec->node->line, NULL,
                    "cannot apply `%s` to `%s`", op, typeStr(c, lt));
            return;
        }
        /* An enum with a payload has no `==` in C, and no method could fix that; the
         * concrete path points at `match` and so does this. */
        if (b && b->kind == TY_ENUM && enumHasPayload(b->edef) && isEqualityOp(op)) {
            ckError(c, ec->node->line,
                    "an enum with a payload is a tagged union -- compare it with `match`,"
                    " or write a method that does",
                    "`%s` is an enum with a payload, so it has no `%s`", typeStr(c, lt), op);
            return;
        }
        Buf note;
        bufInit(&note, c->arena);
        bufPrintf(&note,
                  "`%s` inside a generic is checked at instantiation, not on the template --"
                  " the price of having no traits. Add a `fn %s` to that type.", op, op);
        ckError(c, ec->node->line, bufCstr(&note),
                "`%s` needs `%s` to define `%s`", instName, typeStr(c, lt), op);
        return;
    }

    /* The instance answers with a method, and a method of an instance is emitted by code
     * generation only when it is marked used. This is the one place that knows it: the
     * template could not, `T` was opaque there, and the call site cannot, because code
     * generation resolves the operator again on the substituted type and finds the same
     * method without recording anything.
     *
     * Not marking it is what `#64` was: `T == T` instantiated at a slice called the
     * prelude's `slice<T>::==` and nobody emitted it, so the generated C held a call to a
     * function that did not exist. The node itself is not touched on purpose: the template
     * has one node and many instances, so a `func` stored here would be the wrong method
     * for every other instance. */
    FuncDef *m = findOp(tt, ttBase(lt), ec->op, rt2,
                        strcmp(ec->op, "!=") == 0 ? "==" : NULL);
    if (m) m->used = true;
}

/* Re-check one recorded USE of a deferred call's result (`#79`).
 *
 * The template accepted it because the value's type was the error type; here the receiver's type is
 * concrete, so the method's real return type is known and the expected type recorded at the use
 * site can finally be enforced. `what` is the instance name, so the diagnostic reads
 * "`f_i32` expects `i8`, found `i64`" -- naming the instance is the whole point of doing this at
 * instantiation instead of on the template. */
static void runDeferredUse(Checker *c, DeferredUse *du, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    /* Anything that is not a late method carries its own type: substitute the expression's type and
     * compare. That is the path a **type-parameter-typed value** takes -- `s = s + c.value()` inside
     * `fn run<T>` records the sum, whose type is `T` on the template (tools/attack.py B8). Reading
     * `u.method.recv` here for such a node is what crashed the compiler when this generalization was
     * first attempted (the union member is not an `EX_METHOD`). */
    if (du->call->kind != EX_METHOD || du->call->func) {
        Type *vt = ttSubstitute(tt, du->call->type, params, targs);
        /* **Both** sides: `want` is very often the template's own parameter too (an argument checked
         * against `gen<T>`'s parameter, say), and substituting only the value reported a bogus
         * `` `gen_i64` expects `T`, found `i64` `` for every generic call in the corpus. */
        Type *wt = ttSubstitute(tt, du->want, params, targs);
        if (ttIsError(vt) || ttIsError(wt)) return;
        checkAssignable(c, wt, vt, du->call, arenaPrintf(c->arena, "`%s`", instName));
        return;
    }
    Type *rt = ttSubstitute(tt, du->call->u.method.recv->type, params, targs);
    FuncDef *f = findMethod(ttBase(rt), du->call->u.method.name);
    /* A missing method is reported by runMethodCheck; nothing to add here. */
    if (!f || !f->ret) return;
    Type *ret = f->ret;
    if (rt && rt->kind == TY_GENERIC && f->owner == rt->sdef)
        ret = ttSubstitute(tt, f->ret, &f->owner->typeParams, &rt->targs);
    if (ttIsError(ret)) return;
    checkAssignable(c, du->want, ret, du->call, arenaPrintf(c->arena, "`%s`", instName));
}

/* Resolve one deferred method call for a concrete instance (`#57`).
 *
 * On the template the receiver's type was a type parameter, so no method could be found and the
 * call was recorded instead (see `MethodCheck`). Here the receiver type is concrete: the method
 * either exists -- and then it is marked used so code generation emits it -- or the instance
 * cannot support the call, which is the error this whole deferral exists to report.
 *
 * Params:
 *   c        - checker
 *   mc       - the deferred call: the node, the method name, its arity
 *   params   - type parameters of the instance
 *   targs    - type arguments of the instance
 *   instName - instance name, reported in the diagnostic
 *
 * Notes:
 *   - The node is deliberately NOT given a `func`: one template body has many instances, and a
 *     `func` stored here would be the wrong method for every other one. Code generation
 *     re-resolves the method on the substituted receiver type, the way it resolves operators.
 *   - Arity is checked here because nothing else can see it: the template had no signature to
 *     compare against, and letting it through would surface as a C error in generated code. */
static void runMethodCheck(Checker *c, MethodCheck *mc, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    Type *rt = ttSubstitute(tt, mc->node->u.method.recv->type, params, targs);
    Type *rb = ttBase(rt);
    FuncDef *f = findMethod(rb, mc->name);
    if (!f) {
        Buf note;
        bufInit(&note, c->arena);
        bufPrintf(&note,
                  "`%s` inside a generic is checked at instantiation, not on the template --"
                  " the price of having no traits. Add a `fn %s` to that type.",
                  mc->name, mc->name);
        ckError(c, mc->node->line, bufCstr(&note),
                "`%s` needs `%s` to define `%s`", instName, typeStr(c, rb), mc->name);
        return;
    }
    size_t want = f->params.len > 0 ? f->params.len - 1 : 0;     /* minus the receiver */
    if (mc->nargs != want) {
        ckError(c, mc->node->line, NULL,
                "`%s` takes %zu argument%s, but %zu %s written",
                mc->name, want, want == 1 ? "" : "s", mc->nargs, mc->nargs == 1 ? "was" : "were");
        return;
    }
    f->used = true;
}

/* Does this deferred reference check belong to this free-function instance?
 *
 * Params:
 *   rc - a check deferred from a template body
 *   fi - the instance being replayed
 *
 * Returns:
 *   True when the check came from this instance's own template and that template is
 *   generic. A method takes the type-instance path instead, so it answers false here.
 */
static bool refCheckApplies(RefCheck *rc, FuncDef *fi) {
    FuncDef *tmpl = planTemplate(fi);
    if (!fi || !tmpl) return false;
    if (rc->func != tmpl) return false;
    return tmpl->typeParams.len > 0;
}

/* Re-check one deferred reference rule for a concrete instance.
 *
 * A generic body is checked once on the template, where `T` is opaque, so every rule whose
 * outcome depends on `T` is deferred and replayed here per instance.
 *
 * Params:
 *   c        - checker
 *   rc       - the deferred check: the value, its depth, the depth it has to fit into, and
 *              what the reference rule applies to
 *   instName - name of the instance, used in the diagnostics
 *
 * Notes:
 *   - The depth recorded at template time is a snapshot of a world the level solver later
 *     changes, so it is recomputed here for the shapes whose depth an allocation site
 *     decides, and only downwards. Recomputing a binding is not allowed: that answers 0
 *     for a local whose site is still undecided and accepts a real dangling pointer.
 *   - A check whose value must trace back to a parameter is skipped when it does not, since
 *     the rule is about parameters.
 */
static void runRefCheck(Checker *c, RefCheck *rc, const char *instName) {
    TypeTable *tt = c->tt;
        /* Does that `T` carry a reference in this instance? If not, this rule does not
         * apply here at all: `box<i64>::set` stays legal while `boxT<slice<u8>>::stash`
         * is rejected. That is exactly why `T` cannot simply be treated as
         * reference-carrying in every instance. */
        if (rc->isNewSize) {
            /* Still a type parameter (or `void`) after instantiation => the size is
             * still unknown => reject, naming the instance. */
            Type *nt = tsub(c, rc->declType);
            c->substParams = NULL;
            c->substArgs   = NULL;
            if (nt->kind == TY_PARAM || ttHasParam(nt) || ttIs(nt, "void"))
                ckError(c, rc->line,
                        "`new` needs a concrete type: its size is decided at compile time,"
                        " and this one is still not concrete after instantiation.",
                        "in instance `%s`: cannot `new` `%s` -- its size is not known"
                        " even here", instName, typeStr(c, nt));
            return;
        }

        if (rc->isZero) {
            /* Zero-value kind: does this instance's `T` have a zero value? */
            Type *zt = tsub(c, rc->declType);
            c->substParams = NULL;
            c->substArgs   = NULL;
            if (typeLacksZeroValue(tt, zt))
                ckError(c, rc->line,
                        "A generic body is checked once on the template, where `T` is"
                        " opaque -- so this is re-checked for every concrete instance."
                        " Give the local an initializer.",
                        "in instance `%s`: `%s` has no zero value (it contains a"
                        " reference)", instName, rc->what);
            return;
        }

        /* Same reasoning: a function that is never called needs no per-instance recheck. */
        if (rc->func && !rc->func->used) return;
        Type *vt = tsub(c, rc->val->type);
        c->substParams = NULL;
        c->substArgs   = NULL;
        if (!typeContainsRef(tt, vt)) return;

        /* Recompute with the level the solver settled on: take the smaller value,
         * it can only get tighter. */
        if (depthComesFromAlloc2(c, rc->val, 0)) {
            int now = solvedDepth(rc->val);
            if (now < rc->depth) rc->depth = now;
        }
        if (dbgOn("EXTC_DBG_AT"))
            fprintf(stderr, "[at] %s what=%s depth=%d at=%d kind=%d dca=%d svd=%d\n",
                    instName, rc->what, rc->depth, rc->at, (int)rc->val->kind,
                    depthComesFromAlloc2(c, rc->val, 0)?1:0, solvedDepth(rc->val));
        if (rc->depth > rc->at) {
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: %s would hold a reference to something that"
                    " dies first (depth %d, but this can only hold up to %d)",
                    instName, rc->target ? "this assignment" : rc->what,
                    rc->depth, rc->at);
        } else if (rc->target && rc->at == 0 && rc->borrowed
                   && !valTracesToParam(c, rc->func, rc->val)) {   /* not parameter-backed */
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: cannot store a borrowed value into something"
                    " that outlives this call", instName);
        }
}

/* Is this instance **provisional** (a placeholder) rather than real?
 *
 * A generic call inside a generic body has to build one -- `less<T>` calling `less` with `T`
 * itself produces the instance `less_T` -- so that the enclosing template body can still be
 * type-checked (`T` stays opaque there). At instantiation every real call site is repointed to
 * a concrete instance (`less_i64`), so the placeholder is a thing the checker owns and code
 * generation never emits (`funcSignatureMentionsParam` skips it for exactly this reason).
 *
 * The deferred re-checks must skip it too. Asking a placeholder "does this type have `<`?" is
 * asking the one question that cannot be answered on a template -- it is the price of having
 * no traits, and the concrete instance answers it instead.
 */
/* Intern the type instances that appear as **local declaration types** in a body (`s->u.var.ann`).
 *
 * Only the statement walk is needed: the shape that broke was `var p: pair<T>` inside a generic
 * function (tools/attack.py W9), and interning is a side effect of the substitution itself. */
static void internLocalTypes(Checker *c, FuncDef *f) {
    if (!f || !f->body) return;
    Vec *params = f->typeParams.len ? &f->typeParams
                                   : (planTemplate(f) ? &planTemplate(f)->typeParams : NULL);
    Vec *targs  = f->targs.len ? &f->targs : NULL;
    if (!params || !targs) return;
    Vec stack;
    vecInit(&stack, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&stack) = f->body;
    while (stack.len) {
        Stmt *st = *(Stmt **)vecAt(&stack, stack.len - 1);
        stack.len--;
        if (!st) continue;
        /* The checker writes its results back into the AST (`Stmt.type`, `Expr.type`, ...), so a
         * statement's own type is often where an instance hides -- `let sl = arr[..]` has type
         * `slice<box<T>>` and nothing else in the body mentions that slice. Substituting it here
         * interns it (`ttGeneric`), early enough for the unit list: without this the generated C
         * named an instance nobody emitted (`'slice_box_i64' undeclared`, tools/attack.py X9; the
         * container case X3 has the same shape). */
        if (st->type) (void)ttSubstitute(c->tt, st->type, params, targs);
        switch (st->kind) {
        case ST_VAR:
            if (st->u.var.ann) (void)ttSubstitute(c->tt, st->u.var.ann, params, targs);
            break;
        case ST_BLOCK:
            for (size_t i = 0; i < st->u.block.stmts.len; i++)
                *(Stmt **)vecPush(&stack) = *(Stmt **)vecAt(&st->u.block.stmts, i);
            break;
        case ST_IF:
            *(Stmt **)vecPush(&stack) = st->u.ifs.thenBody;
            *(Stmt **)vecPush(&stack) = st->u.ifs.elseBody;
            break;
        case ST_WHILE:
            *(Stmt **)vecPush(&stack) = st->u.whiles.body;
            break;
        case ST_MATCH:
            for (size_t i = 0; i < st->u.match.arms.len; i++)
                *(Stmt **)vecPush(&stack) = (*(MatchArm **)vecAt(&st->u.match.arms, i))->body;
            break;
        default:
            break;
        }
    }
}

static bool provisionalInstance(Vec *targs) {
    for (size_t i = 0; i < targs->len; i++)
        if (ttHasParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/* Re-resolve one deferred call site into a concrete instance under the substitution
 * of the enclosing instance.
 *
 * Params:
 *   c - checker
 *   cc - the recorded call site, whose type arguments still mention type parameters
 *   params - the enclosing instance's type parameters
 *   targs - the enclosing instance's type arguments, positionally matching `params`
 *
 * Notes:
 *   - Three steps: substitute the type arguments, intern (or create) that concrete
 *     instance, then point the call expression's `func` at it (`cc->node->func`).
 *   - Instances are interned (`funcInstance` de-duplicates), so one argument
 *     combination has exactly one instance.
 */
static void resolveDeferredCall(Checker *c, CallCheck *cc, FuncDef *enclosing,
                                const Type *enclosingInst, Vec *params, Vec *targs) {
    if (!targs || !params) return;
    Vec concrete;
    vecInit(&concrete, c->arena, sizeof(void *));
    for (size_t i = 0; i < cc->targs.len; i++) {
        Type *a = *(Type **)vecAt(&cc->targs, i);
        /* `a` can be NULL (a slot the inference could not fill), and `ttSubstitute` is
         * not guaranteed to be NULL-safe, so bail out first: a failed inference should
         * have been reported on the template already, so here it only means this
         * instance is not ready yet and is skipped. */
        if (!a) return;
        Type *sub = ttSubstitute(c->tt, a, params, targs);
        if (!sub || ttHasParam(sub)) return;  /* still carries `T` => wait for an outer instance */
        *(Type **)vecPush(&concrete) = sub;
    }
    FuncDef *inst = funcInstance(c, cc->tmpl, &concrete, cc->node->line);
    if (!inst) return;
    inst->used = true;
    /* The site may be shared by every instance of the enclosing body (`wrap<i32>` and
     * `wrap<meter>` share one `pick(x)`), and the answer differs per instance: record it
     * against the enclosing instance as well. The plain pointer stays as the fallback
     * answer for a site that belongs to no instance. */
    planSetCallee(cc->node, inst);
    planSetAltCallee(cc->node, enclosing, enclosingInst, inst);
}

/* Check a whole module.
 *
 * This is the entry point of the pass. It runs in stages, in this order:
 *   1. intern the names of every struct and type, so that signatures can be compared;
 *   2. resolve the types written in signatures, fields, and enum payloads;
 *   3. check the declarations for duplicate names;
 *   4. check the global variables and constants;
 *   5. check every function body;
 *   6. replay the checks that were deferred to instantiation, once per concrete instance;
 *   7. close the analyses: the transitive closure of `needsHome`, the escape sets, the
 *      effect summaries, the level solver, and the arena every call passes;
 *   8. hand codegen the finished decisions.
 *
 * Params:
 *   ctx   - compilation context; `hasError` is set when any diagnostic was reported
 *   arena - arena for everything this pass allocates
 *   tt    - type table
 *   m     - module to check
 *
 * Returns:
 *   True when the module is free of errors.
 *
 * Notes:
 *   - The order is not free. Instances are created while the bodies are checked, so the
 *     pass that closes over every function can only run after stage 5, and `needsHome` is
 *     only final after that closure. The escape sets have to be known before the arena of
 *     each call is chosen, which is why stage 7 recomputes them instead of reusing the
 *     per-function result.
 *   - `refDepth` and `arenaLevel` describe the same fact, so every site's `refDepth` is
 *     resynchronised from its final `arenaLevel` after the solver has run. A site whose
 *     level does not change would otherwise keep the value the provisional pass left
 *     behind, and the two fields would contradict each other.
 */
/* One round of the `makesPool` closure. A generic struct's *template* round is skipped on
 * purpose: a template and its instances share the same `FuncDef`, and while the template is being
 * checked `k.hash()` cannot be resolved, so marking it there would freeze a conservative `true`
 * that the per-instance round could then never refine. Externs have no body to walk --
 * `stmtMakesPool` finds the callee at the call site, and `extc_pool_new` is exactly that case. */
static void roundMakesPool(CloseCtx *cx, ReachKind k, bool *changed) {
    Module *m = cx->m;
    TypeTable *tt = cx->tt;

    {
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (getReach(f, k)) continue;
            /* An extern has no body to walk; `stmtMakesPool` finds the callee at the call
             * site (`extc_pool_new` itself is declared extern and is exactly that case). */
            if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
        }
        /* **泛型 struct 的模板轮要跳过**：模板与实例**共用同一批 FuncDef**
         * （实测 `template find` 与 `hashMap$hashMap.find` 是同一个指针），而模板轮的
         * `k.hash()` 解析不出来 ⇒ 只能保守标真，一旦标上，实例轮 `if (getReach(f, k)) continue`
         * 就永远跳过它，精确解析**没机会生效**。泛型结构体的方法体本来就只在实例上跑，
         * 所以交给下面那一轮按实例解析：解析不出来时那一轮仍旧保守（`resolveOnInstance`
         * 返回 NULL ⇒ 视为会建池），多个实例之间是**取或**（任何一个实例要建池 ⇒ 整条链
         * 保守标真），方向仍然是安全的。非泛型结构体没有这个问题，照旧。 */
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (getReach(f, k)) continue;
                if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
            }
        }
        /* 实例那一轮：泛型方法的实例是模板的浅拷贝（共享 body），不在 `m->structs` 里，
         * 而在类型表的 `tt->instances` 上。这一轮**按实例解析**协议方法（`#57`），
         * 代入方式与 `runMethodCheck` 完全一致。 */
        for (size_t i = 0; i < tt->instances.len; i++) {
            Type *inst = *(Type **)vecAt(&tt->instances, i);
            StructDef *sd = inst ? inst->sdef : NULL;
            if (!sd) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (getReach(f, k)) continue;
                instResTT = tt; instResParams = &sd->typeParams; instResTargs = &inst->targs;
                setPoolCalleeResolver(resolveOnInstance);
                bool mp = bodyReaches(f, k);
                setPoolCalleeResolver(NULL);
                instResTT = NULL;
                if (mp) { setReach(f, k, true); *changed = true; }
            }
        }
    }
}


/* ---- the function list every arena analysis walks: **one owner** ----
 *
 * The home/zone passes (the transitive closure, the settle pass) all need "every function of this
 * module": its free functions, every struct's methods, and the methods attached by `impl` blocks.
 * The last group used to be missing: an `impl` block normally lives in a *different* module from
 * the type it extends (`impl ostream` in `stl::stringio`, the type in `std::io`), and the module
 * carrying the block is the entry module (measured: `impls=1 structs=12 funcs=51`).
 *
 * Spelling it out in one place is the point: it was written out four times (twice as `all`, twice
 * as `allF`), which is how one copy quietly falls out of step with the others. Sites are moved over
 * one at a time, each verified against the byte-identical baseline. */
static void allFunctions(Arena *arena, Module *m, Vec *out) {
    vecInit(out, arena, sizeof(FuncDef *));
    for (size_t i = 0; i < m->funcs.len; i++)
        *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&m->funcs, i);
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&sd->methods, j);
    }
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        for (size_t j = 0; j < im->methods.len; j++)
            *(FuncDef **)vecPush(out) = *(FuncDef **)vecAt(&im->methods, j);
    }
}

/* 调用点的"**目的地有多浅**"决策 —— arena 与 zone 两条通道共用这一份（U4）。
 *
 * 语义与极性（见 check_internal.h 的极性契约）：返回的是**深度**，数越小活得越久；
 * 调用方看到 `<= 0` 就取自己的 HOME（`ARENA_HOME` / `ZONE_HOME`）。
 *   · `rec->overflow`（实参没记全）⇒ 按"活得最久"算（-1），这是安全方向；
 *   · 深度 0 的实参（参数/全局）本身就让结果是 0；
 *   · 逃逸集合命中的实参按 -1 算（它活过本帧）。
 *
 * 为什么要共享：这两条通道曾经**各写一遍**，而 P0-2 的哨兵 bug（`nd == 0` 既表示"还没见过"
 * 又表示"活得最久"⇒ 循环求成了最大值）**在两条里各有一份** —— 修一条不会修另一条。
 * `seen` 把两义分开，`EXTC_DBG_ASSERT` 让这个分离在调试构建里被看守（U1）。*/
static int callSiteMinDestDepth(Checker *c, EArenaSite *rec) {
    if (rec->overflow) return -1;
    bool seen = false;
    int nd = 0;
    for (int k = 0; k < rec->n; k++) {
        int d = rec->argDepth[k];        /* 0 = parameter/global: outside this frame */
        /* Only the depths whose argument is local to this frame consult the escape set; an
         * argument at depth 0 already forces the home arena. */
        if (d != 0 && rec->argRoot[k] && isEscapeeName(c, rec->argRoot[k])) d = -1;
        if (!seen || d < nd) { nd = d; seen = true; }
    }
    EXTC_DBG_ASSERT(seen || rec->n == 0);
    return nd;
}

bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    /* 阶段 = 显式状态（见 check_internal.h 的阶段契约）。*/

    double tB0=0,tB1=0,tB2=0,tB3=0,tB4=0,tB5=0,tB6=0,tB7=0,tB8=0;
    Checker c;    memset(&c, 0, sizeof c);
    vecInit(&c.funcInsts, arena, sizeof(void *));   /* free function instances */
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    /* `EXTC_DBG_FX=1`: the growth counters, read once here so the loops below never call
     * `dbgOn` per element. `EXTC_DBG_FX_VERBOSE=1` adds one line per body (`levelPass`),
     * which is what tells "a few huge passes" from "many small ones"; the summary at the
     * end of this function is what a scaling table is read from. */
    c.fxOn = dbgOn("EXTC_DBG_FX");
    vecInit(&c.scopes, arena, sizeof(void *));
    vecInit(&c.opChecks, arena, sizeof(void *));
    vecInit(&c.methodChecks, arena, sizeof(void *));   /* #57: method calls on a type parameter */
    vecInit(&c.deferredUses, arena, sizeof(void *));
    vecInit(&c.explicitTargs, arena, sizeof(void *));  /* `f<i32>(...)`: handed to inference */   /* #79: uses of a deferred call's result */
    vecInit(&c.globals, arena, sizeof(void *));
    vecInit(&c.allSyms, arena, sizeof(void *));     /* kept for the EXTC_SELFCHECK invariant scan */
    vecInit(&c.nameUses, arena, sizeof(void *));
    vecInit(&c.narrow, arena, sizeof(void *));
    vecInit(&c.refChecks, arena, sizeof(void *));
    vecInit(&c.lvlRejects, arena, sizeof(void *));
    vecInit(&c.callChecks, arena, sizeof(void *));   /* deferred call sites */
    vecInit(&c.narrowMarks, arena, sizeof(size_t));
    vecInit(&c.eSites, arena, sizeof(EArenaSite *));   /* arena decisions that depend on escapes */
    vecInit(&c.lvlFacts, arena, sizeof(LvlFact *));

    /* ---- The **handle** protocol, before any body is checked ----
     * `coroutine<T>` is a storage type, so `next`/`value`/`send` on it must exist whatever order the
     * checker walks functions in: a library may drive handles without declaring a coroutine itself,
     * and the method would otherwise be missing when its body is checked. */
    for (size_t hi = 0; hi < m->structs.len; hi++) {
        StructDef *hsd = *(StructDef **)vecAt(&m->structs, hi);
        Checker   *cc  = &c;
        if (!hsd || !hsd->name || strcmp(hsd->name, "coroutine") != 0) continue;
        if (hsd->methods.len) continue;                 /* synthesized (pre-pass or on demand) */
        /* 与按需合成那条一致：用**标记自己的类型参数名**，因为替换是按名字（`param`）匹配的。
         * `A` 由 `send` 送进去，`B` 由 `value` 交出来；只写一个参数的 `coroutine<T>` 在替换处回落。 */
        const char *an = hsd->typeParams.len > 0
            ? *(const char **)vecAt(&hsd->typeParams, 0) : "A";
        const char *bn = hsd->typeParams.len > 1
            ? *(const char **)vecAt(&hsd->typeParams, 1) : an;
        Type *tpA = arenaAllocZero(cc->arena, sizeof *tpA);
        tpA->kind = TY_PARAM; tpA->name = an; tpA->param = an;
        vecInit(&tpA->targs, cc->arena, sizeof(Type *));
        Type *tpB = arenaAllocZero(cc->arena, sizeof *tpB);
        tpB->kind = TY_PARAM; tpB->name = bn; tpB->param = bn;
        vecInit(&tpB->targs, cc->arena, sizeof(Type *));
        Type *hinst = arenaAllocZero(cc->arena, sizeof *hinst);
        hinst->kind = TY_GENERIC;                       /* an instance with targs: `coroutine<T>` */
        hinst->name = hsd->name;
        hinst->sdef = hsd;
        vecInit(&hinst->targs, cc->arena, sizeof(Type *));
        *(Type **)vecPush(&hinst->targs) = tpA;
        *(Type **)vecPush(&hinst->targs) = tpB;
        const int hw[3] = { 3, 4, 6 };                  /* next / value / send on the handle */
        for (int k = 0; k < 3; k++) {
            const int which = hw[k];
            FuncDef *pm = arenaAllocZero(cc->arena, sizeof *pm);
            pm->name      = which == 3 ? "next" : which == 4 ? "value" : "send";
            pm->owner     = hsd;
            pm->line      = hsd->line;
            planSetCoroProto(pm, which);
            pm->ret       = which == 4 ? tpB : ttFromName(cc->tt, "bool");
            vecInit(&pm->params, cc->arena, sizeof(Param *));
            Param *self = arenaAllocZero(cc->arena, sizeof *self);
            self->name = self->cname = "self";
            self->line = hsd->line;
            Type *rt = arenaAllocZero(cc->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = hinst;
            rt->mut = which != 4;                       /* `next`/`send` advance, `value` reads */
            self->type = rt;
            *(Param **)vecPush(&pm->params) = self;
            if (which == 6) {
                Param *vp = arenaAllocZero(cc->arena, sizeof *vp);
                vp->name = vp->cname = "v";
                vp->type = tpA;   /* `send(v: A)` */
                vp->line = hsd->line;
                *(Param **)vecPush(&pm->params) = vp;
            }
            *(FuncDef **)vecPush(&hsd->methods) = pm;
        }
    }
    vecInit(&c.stores, arena, sizeof(StoreSite *));    /* long-running: "stored at level k" facts */
    /* Must be initialized here, before any pass.
     *
     * `curArenaSites` used to be `vecInit`ed inside `checkFunc` only, while
     * `checkGlobals` runs before the first `checkFunc`. A top-level initializer with a
     * `new` (`var G = new i32`) or a call to a function that needs a home arena
     * (`let G = helper()`) then pushed onto an uninitialized Vec, so `vecGrow` reached
     * `arenaAlloc(NULL)` and the compiler died with SIGSEGV instead of reporting "a
     * global can only be initialized with a constant". */
    vecInit(&c.curArenaSites, arena, sizeof(Expr *));

    c.tI32  = ttFromName(tt, "i32");
    c.tI64  = ttFromName(tt, "i64");
    c.tF64  = ttFromName(tt, "f64");
    c.tBool = ttFromName(tt, "bool");
    /* A string literal has type `slice<u8>`.
     * It comes from the prelude, so the string type itself is written in extC and the
     * compiler only has to turn the literal into one of its values. */
    {
        Type *base = ttFromName(tt, "slice");
        if (base && base->kind == TY_STRUCT && base->sdef) {
            c.sliceDef = base->sdef;
            Vec args;
            vecInit(&args, arena, sizeof(void *));
            *(Type **)vecPush(&args) = ttFromName(tt, "u8");
            c.tSliceU8 = ttGeneric(tt, base->sdef, &args);
        } else {
            c.tSliceU8 = ttError(tt);
            ctxError(ctx, 1, 1,
                     "the prelude (stdlib/prelude.extc) must define `slice<T>`",
                     "the prelude does not define `slice`, so string literals cannot work");
        }
    }

    /* Intern every struct / type first: method signature comparison needs them. */
    for (size_t i = 0; i < m->structs.len; i++)
        ttFromName(tt, (*(StructDef **)vecAt(&m->structs, i))->name);
    for (size_t i = 0; i < m->types.len; i++)
        ttFromName(tt, (*(TypeDef **)vecAt(&m->types, i))->name);

    /* Attach every `impl` block to its target type.
     *
     * This has to happen before signatures are resolved, because the attachment makes the
     * methods **indistinguishable** from methods written inside the type's body: from here on
     * every pass that walks "struct x method" (signature resolution, body checks, the escape and
     * borrow rules, operator collection, code generation) reaches them with no pass changed at
     * all. That is what makes an attachment point this cheap.
     *
     * A builtin scalar has no declaration body to attach to, so a synthetic holder is created
     * for it: the holder goes into this module's struct list (so those passes see the methods)
     * but is NOT interned under its name in the type table (so `i64` keeps resolving to the
     * builtin, never to a struct) and code generation emits no C struct for it. `Type.mholder`
     * points back at it, which is where method lookup ends up.
     *
     * The holder is keyed off the **type**, not off this module: attaching the same method twice
     * -- from two impl blocks, from two modules, or from the prelude and the program -- hits the
     * duplicate check below. A type has one method set, and coherence is enforced by that check
     * rather than by "the last one silently wins". */
    if (dbgOn("EXTC_DBG_IMPL"))
        fprintf(stderr, "[impl] %zu block(s), %zu struct(s), %zu type(s)\n",
                m->impls.len, m->structs.len, m->types.len);
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (dbgOn("EXTC_DBG_IMPL"))
            fprintf(stderr, "[impl]   target `%s` with %zu method(s)\n", im->typeName, im->methods.len);
        /* `impl Trait for Type` asks three questions, and all three are about the **pair**, which
         * is why they are answered here -- at the attachment point, where the type's one method
         * set is decided -- rather than in a pass of their own. */
        if (im->traitName) {
            TraitDef *tr = NULL;
            for (size_t k = 0; k < m->traits.len; k++) {
                TraitDef *cand = *(TraitDef **)vecAt(&m->traits, k);
                if (cand->name && strcmp(cand->name, im->traitName) == 0) {
                    /* `impl Codec<i64> for X`: an explicit-parameter trait must be implemented
                     * with that many arguments. The *matching* is still by name -- the arguments
                     * are not part of the conformance check yet -- but a wrong arity is caught
                     * here instead of silently binding `T` to nothing. */
                    if (im->traitArgs.len != cand->typeParams.len) {
                        ctxError(ctx, im->line, 1,
                                 "Name the trait's type arguments here: `impl Codec<i64> for X`.",
                                 "`impl %s for %s`: the trait has %zu type parameter(s), %zu given",
                                 im->traitName, im->typeName, cand->typeParams.len, im->traitArgs.len);
                        break;
                    }
                    tr = cand;
                    break;
                }
            }
            if (!tr) {
                ctxError(ctx, im->line, 1,
                         "A trait is declared with `trait Name { ... }`, and an `impl` block must"
                         " name one that exists.",
                         "`impl` on unknown trait `%s`", im->traitName);
                continue;
            }
            /* One implementation per (trait, type): the method set of a type is flat, so a second
             * implementation could only be a silent redefinition. */
            bool dup = false;
            for (size_t k = 0; k < i && !dup; k++) {
                ImplDef *prev = *(ImplDef **)vecAt(&m->impls, k);
                dup = prev->traitName && strcmp(prev->traitName, im->traitName) == 0 &&
                      strcmp(prev->typeName, im->typeName) == 0;
                if (dup)
                    ctxError(ctx, im->line, 1,
                             "A type has one implementation of a trait. Two behaviours means two"
                             " traits.",
                             "`%s` is already implemented for `%s` (first at line %d)",
                             im->traitName, im->typeName, prev->line);
            }
            im->trait = tr;
            /* Completeness. Whether the signatures agree is a question for the types, and is
             * answered once those are resolved; this half asks whether the names are there. */
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                bool found = false;
                for (size_t j = 0; j < im->methods.len && !found; j++)
                    found = strcmp((*(FuncDef **)vecAt(&im->methods, j))->name, want->name) == 0;
                if (!found)
                    ctxError(ctx, im->line, 1,
                             "An implementation supplies every method the trait declares.",
                             "`impl %s for %s` is missing `%s`",
                             im->traitName, im->typeName, want->name);
            }
        }
        /* Resolve the target the way a type annotation is resolved: `ttResolve` consults the
         * module's rename table and the bare-name aliases (`string` -> `stl$string`), counts
         * matches, and refuses an ambiguous bare name with the message every other type
         * position gets. `ttFromName` alone only knows the builtins and the declarations of
         * this module -- which silently made `impl string { ... }` from an importing module an
         * "unknown type" (found by writing `stl/stringio.extc`, the streaming pilot). */
        Type *base = typeNamed(arena, im->typeName);
        /* `impl slice<u8>`: the arguments belong to the target, and `ttResolve` resolves them the
         * way it resolves an annotation's (`targs` is recursed into). */
        base->targs = im->typeArgs;
        Type *t = ttResolve(tt, ctx, base, im->line,
                            im->typeParams.len ? &im->typeParams : NULL);
        StructDef *sd = NULL;
        if (ttIsError(t)) continue;
        if (!t) {
            ctxError(ctx, im->line, 1,
                     "An `impl` block extends a type that exists: a struct declaration, or a "
                     "builtin scalar such as `i64`.",
                     "`impl` on unknown type `%s`", im->typeName);
            continue;
        }
        if (t->kind == TY_BUILTIN) {
            sd = t->mholder;
            if (!sd) {
                sd = (StructDef *)arenaAllocZero(arena, sizeof(StructDef));
                sd->name = im->typeName;
                sd->line = im->line;
                sd->builtinHolder = true;
                sd->type = t;
                vecInit(&sd->typeParams, arena, sizeof(void *));
                vecInit(&sd->fields, arena, sizeof(void *));
                vecInit(&sd->methods, arena, sizeof(void *));
                t->mholder = sd;
                *(StructDef **)vecPush(&m->structs) = sd;
            }
        } else {
            sd = structOf(t);
            if (!sd) {
                ctxError(ctx, im->line, 1,
                         "A type that can own methods is a `struct` or a builtin scalar. An enum is "
                         "read with `match`, and a view or a reference is not a declaration.",
                         "`impl` on `%s`, which cannot own methods", im->typeName);
                continue;
            }
            /* `impl<T> pair<T> { ... }`: the methods belong to the generic declaration's body --
             * `structOf(t)` already is that body, and the machinery that instantiates
             * body-declared methods (`resolveSignature` + `ttSubstitute` over `tt->instances`)
             * then gives every instance its own copy. The only thing to check is the names: a
             * method's signature resolves against `f->owner->typeParams`, so a block that renames
             * `T` to `U` would leave `U` unknown. Flat on purpose -- a nested version of this
             * check is what broke the build the first time. */
            if (im->typeParams.len > 0) {
                bool bad = im->typeParams.len != sd->typeParams.len;
                for (size_t pi = 0; !bad && pi < im->typeParams.len; pi++)
                    bad = strcmp(*(const char **)vecAt(&im->typeParams, pi),
                                 *(const char **)vecAt(&sd->typeParams, pi)) != 0;
                if (bad) {
                    ctxError(ctx, im->line, 1,
                             "Name the block's type parameters exactly as the type declares them:"
                             " the methods are resolved against the declaration's body.",
                             "`impl` on `%s`: the type parameters must match the declaration's",
                             im->typeName);
                    continue;
                }
            }
            if (sd->typeParams.len > 0 && im->typeParams.len == 0) {
                if (t->targs.len == 0) {
                    ctxError(ctx, im->line, 1,
                             "A generic type's methods are declared inside its own body; extending "
                             "every instance at once needs the block's own type parameters.",
                             "`impl` on generic type `%s` is not supported yet", im->typeName);
                    continue;
                }
                /* The block names an **instance** (`slice<u8>`), and an instance's method set is
                 * its own: attaching to the generic body would hand `slice<i32>` the same methods
                 * and substitute `Self` to the wrong type. The holder hangs off the `Type`, which
                 * is exactly how a builtin scalar's `impl i64 { ... }` block is kept. */
                StructDef *ih = t->mholder;
                if (!ih) {
                    ih = (StructDef *)arenaAllocZero(arena, sizeof(StructDef));
                    /* The holder is a **distinct declaration** from the generic one, so it needs a
                     * distinct name: reusing `im->typeName` collided with the generic's own
                     * declaration and tripped the reserved-definition check (`slice` comes from
                     * the prelude). The mangled name of the instance is unique by construction. */
                    ih->name = (t->name && t->name[0]) ? t->name : "$inst";
                    ih->line = im->line;
                    ih->builtinHolder = true;   /* no C struct of its own: the instance has one */
                    ih->type = t;
                    vecInit(&ih->typeParams, arena, sizeof(void *));
                    vecInit(&ih->fields, arena, sizeof(void *));
                    vecInit(&ih->methods, arena, sizeof(void *));
                    t->mholder = ih;
                    /* The holder has to be in `m->structs`, exactly like a builtin scalar's: the
                     * pass that resolves method signatures walks `StructDef.methods`, so a holder
                     * nobody walks keeps `TY_UNRESOLVED` signatures -- which showed up as
                     * "initializer expects `i64`, found `i64`" (two names, one unresolved type). */
                    *(StructDef **)vecPush(&m->structs) = ih;
                }
                sd = ih;
            }
        }
        /* No duplicate check here **on purpose**. Attaching a method must make it
         * indistinguishable from one written in the body, and the rules for a method set then
         * apply to it exactly as they apply to the body -- including the one that matters here:
         * an operator name may be defined once per **right-operand type** (`isOverloadableOp`),
         * which is how `io::istream` carries `>>` for `i64`, `slice<u8>`, `u8` and now `string`.
         * A second copy of those rules in this pass rejected the legitimate overload with
         * "already has a method named `>>`" (found by attaching `>>` to `io::istream`) -- two
         * copies of a rule drift, and the stricter copy wins silently. The single authority is
         * `checkDeclarations` below, which runs after types are resolved (it needs `ttEquals` on
         * the operand types, which is only meaningful then). */
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            im->target = t;                 /* resolved here, read by the conformance pass and codegen */
            /* Orphan rule: the block must live where the trait is declared or where the type is
             * declared. Anywhere else it has no home -- and because a type has exactly one method
             * set, a competing implementation from a third module would surface as an unrelated
             * duplicate-method error far from its cause. */
            if (im->trait && !sameModule(im->modName, im->trait->modName) &&
                              !sameModule(im->modName, sd ? sd->modName : NULL)) {
                ctxError(ctx, im->line, 1,
                         "Implement a trait in the module that declares the trait, or in the one"
                         " that declares the type.",
                         "`impl %s for %s` is an orphan: module `%s` declares neither",
                         im->traitName, im->typeName,
                         im->modName ? im->modName : "this file");
                continue;
            }
            mth->owner = sd;
            *(FuncDef **)vecPush(&sd->methods) = mth;
        }
    }

    /* First pass: resolve the type names in every signature and field. */
    /* Enum payload types are resolved here as well (`| circle(f64) | rect(f64, f64)`). */
    ctPhase("ckB0", tB0); tB1 = ctNow();
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            for (size_t k = 0; k < v->types.len; k++)
                *(Type **)vecAt(&v->types, k) =
                    ttResolve(tt, ctx, *(Type **)vecAt(&v->types, k), v->line, &td->typeParams);
        }
    }
    ctPhase("ckB1", tB1); tB2 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
            fd->type = ttResolve(tt, ctx, fd->type, fd->line, &sd->typeParams);
        }
        for (size_t j = 0; j < sd->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB2", tB2); tB3 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++)
        resolveSignature(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* `@export`: the function is called **from C**, so C has to be able to write its signature down.
     * The predicate is the one `extern!` obeys (`ttCrossesC`), asked in the other direction -- one
     * spelling and two callers, which is what keeps the two halves of the boundary from drifting.
     * One more thing can break the promise (a hidden arena/zone parameter) and is refused in codegen,
     * where that transitive closure is final. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExport) continue;
        for (size_t j = 0; j < f->params.len; j++) {
            Param *p = *(Param **)vecAt(&f->params, j);
            if (ttCrossesC(p->type, false)) continue;
            ctxError(ctx, p->line ? p->line : f->line, 1,
                     "A function C calls must take what C can pass: one machine word per parameter"
                     " -- a scalar, a pointer, a function pointer, or a `@frozen` struct. A"
                     " `slice<T>` is two C parameters, so it cannot be one.",
                     "`@export` parameter %zu has type `%s`, which C cannot pass",
                     j + 1, typeStr(&c, p->type));
        }
        Type *rt = f->ret ? f->ret : ttVoid(tt);
        if (!ttCrossesC(rt, true))
            ctxError(ctx, f->line, 1,
                     "A function C calls must return what C can return: `void`, one machine word, or"
                     " a `@frozen` struct.",
                     "`@export` returns `%s`, which C cannot return", typeStr(&c, rt));
    }

    /* Trait method signatures are resolved here for the same reason struct methods are: the
     * conformance check compares resolved types on both sides. `Self` resolves because the
     * parser gave every signature that type parameter (see parseTrait) -- which is the whole
     * implementation of "`Self` is an implicit type parameter". */
    ctPhase("ckB3", tB3); tB4 = ctNow();
    for (size_t i = 0; i < m->traits.len; i++) {
        TraitDef *td = *(TraitDef **)vecAt(&m->traits, i);
        for (size_t j = 0; j < td->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&td->methods, j));
    }


    checkGlobals(&c);
    checkDeclarations(&c);

    ctPhase("ckB4", tB4); tB5 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkMethodShape(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB5", tB5); tB6 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (planTemplate(fx)) continue;      /* an instance is not checked separately */
        checkMethodShape(&c, fx);
    }

    /* Second pass: check function bodies. */
    c.phase = PHASE_BODIES;   /* 从这一刻起有作用域（见 check_internal.h 的阶段契约）*/
    ctPhase("ckB6", tB6); tB7 = ctNow();
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    ctPhase("ckB7", tB7); tB8 = ctNow();
    for (size_t i = 0; i < m->funcs.len; i++) {
        /* `m->funcs` grows during this loop (instance materialisation pushes to it), so the base
         * pointer is still taken through vecAt every time -- only the next function is prefetched. */
        if (i + 4 < m->funcs.len) __builtin_prefetch(*(FuncDef **)vecAt(&m->funcs, i + 4), 0, 0);
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (planTemplate(fx)) continue;      /* covered by the per-instance recheck */
        checkFunc(&c, fx);
    }

    /* This batch has to run **before** the requirement batches below.
     *
     * It repoints the call sites of a provisional instance at a concrete one (`less_T` ->
     * `less_i64`), while the batches below (operators / protocol methods / result types /
     * reference rules) only ever look at concrete instances. The other order let the concrete
     * instance this batch had just created miss its own re-check: it was built, but nobody
     * asked whether `T` really has `hash()`, so the missing check travelled all the way to code
     * generation, which can only emit a `0` for a call it cannot resolve.
     *
     * Three steps per site: substitute the type arguments, intern (or create) that instance,
     * and point the call expression's `func` at it. Instances are interned (`funcInstance`), so
     * one argument combination has exactly one instance. */
    /* Re-resolve each deferred call site into a concrete instance under the
     * substitution of the enclosing instance.
     *
     * `params` / `targs` are the enclosing instance's type parameters and type arguments
     * (and `c.substParams` / `c.substArgs` are set to that same pair). Three steps per
     * site: substitute the type arguments, intern (or create) that concrete instance,
     * then point the call expression's `func` at it. Static instances are interned
     * (`funcInstance` de-duplicates), so one argument combination has exactly one
     * instance. */
    c.phase = PHASE_POST;    /* 之后**没有作用域**：只能读表与记在节点/符号上的身份 */
    ctPhase("ckB8", tB8);   /* after the last of the eight: the rest of checkModule */
    /* **To a fixpoint**, not in a single pass.
     *
     * Resolving one call site can *create* the callee's instance (`funcInstance` interns on
     * demand), and that new instance has deferred call sites of its own that also need resolving.
     * One pass therefore left the inner ones dangling: `fn f<T>` / `fn g<T> { f(x) }` /
     * `fn m<T> { g(x) }` **declared in that order** generated a call to `f_T` that was never
     * emitted, so the C did not compile -- while the same program with the declarations reversed
     * compiled, which is what made this look like a declaration-order oddity instead of a missing
     * round (audit P0-5). `resolveDeferredCall` is idempotent (interning the same argument
     * combination gives the same instance), so repeating the pass is free; the loop stops as soon
     * as a round creates no new instance. The bound mirrors the other fixpoints here and is high
     * enough that a real program never reaches it (each round strictly grows one of the two
     * tables). */
    size_t instsSeen = 0, tinstsSeen = 0;
    for (int round = 0; round < 64; round++) {
        if (round > 0 && c.funcInsts.len == instsSeen && tt->instances.len == tinstsSeen) break;
        instsSeen  = c.funcInsts.len;
        tinstsSeen = tt->instances.len;
        for (size_t i = 0; i < c.callChecks.len; i++) {
            CallCheck *cc = *(CallCheck **)vecAt(&c.callChecks, i);
            if (!cc->node || !cc->tmpl) continue;
            /* (1) The enclosing body is a free-function instance. */
            for (size_t j = 0; j < c.funcInsts.len; j++) {
                FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
                if (planTemplate(fi) != cc->func) continue;   /* not a call in this template body */
                resolveDeferredCall(&c, cc, fi, NULL, &planTemplate(fi)->typeParams, &fi->targs);
            }
            /* (2) The enclosing body is a type instance (a generic call inside a method);
             * `owner` is the struct or enum definition. */
            StructDef *owner = cc->func ? cc->func->owner : NULL;
            if (!owner) continue;
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != owner) continue;
                /* The enclosing body is a **type** instance: the same method body is shared
                 * by every instance of that struct, so the answer is keyed on the type
                 * instance (`planEnterInst` on the code generation side). */
                resolveDeferredCall(&c, cc, NULL, inst, &owner->typeParams, &inst->targs);
            }
        }
    }


    /* Deferred operator checks: re-check each recorded use for every concrete instance.
     * This is the price of having no traits. The error surfaces late, here, so the
     * message has to name the instance it came from. */
    for (size_t i = 0; i < c.opChecks.len; i++) {
        OpCheck *ec = *(OpCheck **)vecAt(&c.opChecks, i);
        /* The function holding the operator is never called, so none of its instances can
         * run and the per-instance recheck is unnecessary. This also spares the user from
         * having to define the operator for a type that is never used with it. */
        if (ec->func && !ec->func->used) continue;
        /* (1) Type instances (an operator used on a method's `T`). A free function has no
         * owner, so it has no type instances; it is covered by (2).
         *
         * This used to be `if (!ec->owner) goto done;`, which skipped (2) as well: every
         * `T: ==` inside a free generic function was left to code generation, whose
         * message happens to be word for word the same, so the half never ran and nobody
         * noticed. Code generation is the wrong place for it - it cannot see the `%`-on-
         * float case at all - so the loop boundary is written out here. */
        if (ec->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != ec->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runOpCheck(&c, ec, &ec->owner->typeParams, &inst->targs, inst->name);
            }
        }
        /* (2) Free-function instances: an operator used on `T` inside `fn f<T>`. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (ec->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runOpCheck(&c, ec, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    /* Deferred method calls on a type parameter (`#57`): the same shape as the operator batch
     * above -- replayed once per concrete instance, with the instance named in the message.
     *
     * This is what makes a generic container able to do anything with its elements at all:
     * `hashMap<K, V>` hashes a `K`, which means calling `hash()` on a value whose type is a type
     * parameter. On the template there is no such method to find, so the call is recorded and
     * the instance decides. */
    for (size_t i = 0; i < c.methodChecks.len; i++) {
        MethodCheck *mc = *(MethodCheck **)vecAt(&c.methodChecks, i);
        /* A template that is never called has no instances, so nothing can be wrong yet. */
        if (mc->func && !mc->func->used) continue;
        /* (1) Type instances: a method call inside a generic type's method, where `T` comes
         * from the type arguments of the struct that owns it. */
        if (mc->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != mc->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runMethodCheck(&c, mc, &mc->owner->typeParams, &inst->targs, inst->name);
            }
        }
        /* (2) Free-function instances: a method call on `T` inside `fn f<T>`. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (mc->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runMethodCheck(&c, mc, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    for (size_t j = 0; j < c.funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
        FuncDef *tmpl = planTemplate(fi);
        if (!fi || !tmpl || !planIsCoro(tmpl) || planCoroFrameType(fi)) continue;
        if (provisionalInstance(&fi->targs)) continue;
        planSetIsCoro(fi, true);
        if (planYieldType(tmpl))
            planSetYieldType(fi, ttSubstitute(c.tt, planYieldType(tmpl),
                                              &tmpl->typeParams, &fi->targs));
        coroSetup(&c, fi);
        /* A local declaration's type inside a generic body is substituted only while the body is
         * **emitted** -- which happens after code generation has built its unit list and emitted the
         * struct definitions. A type instance that appears **only** as a local's type was therefore
         * never interned in time and its C struct was never emitted: `unknown type name 'pair_i64'`
         * on a local inside `mk<i64>` (tools/attack.py W9). Substituting the annotation here interns
         * it (`ttGeneric`), early enough for the unit list to see it. It is the same kind of fix as
         * the `coroSetup` re-run above: the instance existed, it just appeared too late. */
        coroFrameLay(&c, fi);
    }

    /* Every generic function **instance**: intern the type instances its body declares as local
     * types. The loop above is coroutine-only (it skips instances that already have a frame), and the
     * substitution that would normally intern such a type happens during **body emission** -- after
     * code generation built its unit list and emitted the struct definitions, so the struct never
     * appeared (`unknown type name 'pair_i64'`, tools/attack.py W9; measured: the hook inside the
     * coroutine loop never ran for `mk<i64>`). */
    for (size_t j = 0; j < c.funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
        if (fi && planTemplate(fi)) internLocalTypes(&c, fi);
    }

    /* Deferred uses of such a call's result (`#79`): same two branches, but the question is now
     * whether the instance's real return type fits the type the use site expected. */
    for (size_t i = 0; i < c.deferredUses.len; i++) {
        DeferredUse *du = *(DeferredUse **)vecAt(&c.deferredUses, i);
        if (du->func && !du->func->used) continue;
        if (du->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != du->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                runDeferredUse(&c, du, &du->owner->typeParams, &inst->targs, inst->name);
            }
        }
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (du->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runDeferredUse(&c, du, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
    }

    /* Materialise every concrete instance's field types (`#80`).
     *
     * A generic struct that holds another generic instance (`vals: pool<V>`) declares that field
     * with a parameter, so checking the template interns nothing: `pool<V>` is not concrete, and
     * correctly so. The concrete `pool<i32>` only comes into being when the field type is
     * substituted with the instance's arguments -- which used to happen during code generation,
     * long after code generation had snapshotted the instance table. The mangled call was
     * emitted, the callee's functions never were, and the C compiler reported
     * `implicit declaration of function 'pool$pool_i32_withCap'`.
     *
     * Substituting here is what interns it in time. It is also required for plain correctness:
     * the C struct emitted for `hashMap_i64Key_i32` has a `pool_i32` field, so that typedef has to
     * exist whether or not some other path noticed. The loop walks the table as it grows, so a
     * nested instance's own fields are materialised in turn. */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (!inst->sdef) continue;
        for (size_t j = 0; j < inst->sdef->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&inst->sdef->fields, j);
            (void)ttSubstitute(tt, fd->type, &inst->sdef->typeParams, &inst->targs);
        }
    }

    /* ------------------------------------------------- deferred generic rechecks
     *
     * The `==` batch was handled above; this is the reference-rule batch. On the
     * template, a value that mentions `T` was only recorded (`T` is opaque there), and
     * every concrete instance is now re-run with its substitution in place.
     *
     * Why it has to be done this way: `typeContainsRef(T)` can only answer false for an
     * opaque `T`, so `targetDepth` / `exprBorrowed` return early for it --
     *     struct boxT<T> {
     *         v: T
     *         fn stash(self: mut ref boxT<T>, value: T) { self.v = value }
     *     }
     * instantiated as `boxT<slice<u8>>`, that launders a view into a longer-lived object
     * => a dangling reference. The two-line conservative fix ("any `T` may carry a
     * reference") would also reject the textbook `box<T>::set`, which is entirely safe
     * for `T = i64`, so the check has to be done per instance. */
    for (size_t i = 0; i < c.refChecks.len; i++) {
        RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
        StructDef *owner = rc->func->owner;
        /* Free-function instances need the same recheck: same rule, with `T` coming
         * from the function's own parameter list. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (!refCheckApplies(rc, fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            c.substParams = &planTemplate(fi)->typeParams;
            c.substArgs   = &fi->targs;
            runRefCheck(&c, rc, planInstName(fi));
        }
        /* (2) Type instances, the original path: a method's `T` is fixed by the type
         * arguments of the struct that owns it. */
        if (!owner) continue;                    /* free function: handled by the loop above */
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;
            if (provisionalInstance(&inst->targs)) continue;

            c.substParams = &owner->typeParams;
            c.substArgs   = &inst->targs;
            runRefCheck(&c, rc, inst->name);
        }
    }

    /* ----------------------------- effect summaries, recomputed per instance
     *
     * The summary used to be computed once, on the template (the call in `checkFunc`),
     * where `T` is opaque, so the `carrier` test had to be patched with `mentionsParam`:
     * "anything mentioning `T` counts as carrying a reference". That charged the `Cont`
     * bit to instances such as `varArray<i32>` that carry no reference at all -- a loss
     * of precision, not a wrong answer.
     *
     * Here the summary is recomputed inside each instance's substitution context
     * (`c.substParams` / `c.substArgs` set). The `carrier` line now goes through
     * `tsub(c, e->type)`, so `carrier` is false when `T = i32` and that bit stays clear,
     * while `T = slice<u8>` still sets it. Safety rests on the latter case.
     *
     * The `mentionsParam` clause cannot be deleted in place of this recompute: measured
     * without it, `tests/errors/generic_borrowed_store.extc` went from rejected to
     * accepted, with a real dangling reference. That clause plugs a hole; it is not
     * redundant conservatism.
     *
     * The summary must be cleared before recomputing. `*in = *tmpl` is a shallow copy, so
     * an instance starts out holding the template's summary, and `callees` is copied from
     * the template as well (`collectEffects` does not reset a non-NULL `callees`).
     * Without the clear, the leaf count doubles and `effState` still claims to be
     * finished, so the transitive closure reads the template's stale bookkeeping. */
    for (size_t i = 0; i < c.funcInsts.len; i++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, i);
        if (!fi || !fi->body || fi->isExtern) continue;
        c.substParams = &planTemplate(fi)->typeParams;
        c.substArgs   = &fi->targs;
        fi->addrMask = fi->contMask = fi->otherMask = 0;
        fi->homeAddrMask = fi->homeContMask = 0;
        fi->addrFromLocal = false;
        fi->freshCount = 0;
        fi->effState = EFF_NONE;  fi->effComplete = false;  fi->effUnknown = false;
        vecInit(&fi->callees, c.arena, sizeof(FuncDef *));
        collectEffects(&c, fi);
    }
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        StructDef *sd = inst->sdef;
        if (!sd || sd->typeParams.len == 0) continue;
        for (size_t k = 0; k < sd->methods.len; k++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, k);
            if (!f || !f->body || f->isExtern) continue;
            c.substParams = &sd->typeParams;
            c.substArgs   = &inst->targs;
            f->addrMask = f->contMask = f->otherMask = 0;
            f->homeAddrMask = f->homeContMask = 0;
            f->addrFromLocal = false;
            f->freshCount = 0;
            f->effState = EFF_NONE;  f->effComplete = false;  f->effUnknown = false;
            vecInit(&f->callees, c.arena, sizeof(FuncDef *));
            collectEffects(&c, f);
        }
    }
    c.substParams = NULL;
    c.substArgs   = NULL;

    /* `@inline` is a requirement, not a hint, so a request that cannot be honoured has to
     * be refused here -- before code generation, and at the line the request was written on.
     *
     * The generated C asks for inlining with `always_inline`; a function that reaches
     * itself cannot be inlined, and the C compiler then fails with "inlining failed in call
     * to 'always_inline'" pointing into the generated file, which is no place to explain
     * anything. The call graph is complete by now: `collectEffects` has just filled in
     * `callees` for every function and instance.
     *
     * `funcReachesItself` already exists for the `@overwrite` cells and answers
     * conservatively -- a NULL callee, or a walk deeper than 64, answers yes -- which is the
     * direction wanted: refusing a request that might have worked costs a rewrite, while
     * accepting one that does not costs a compile error the user cannot act on. */
    {
        Vec inl; vecInit(&inl, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&inl) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&inl) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        for (size_t i = 0; i < inl.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&inl, i);
            if (!f || !f->isInline || !f->body) continue;
            if (!funcReachesItself(&c, f, 0)) continue;
            ckError(&c, f->line,
                    "Inlining is a requirement here, not a hint: the generated C asks the C"
                    " compiler for it, and a function that calls itself cannot be inlined."
                    " Make it a loop, or drop `@inline`.",
                    "`@inline` cannot be honoured: `%s` reaches itself, and a recursive"
                    " function cannot be inlined", f->name ? f->name : "?");
        }
    }

    /* ------------------------------- transitive closure of `needsHome`
     * Calling a function that needs a home arena (the arena the caller passes) means the caller
     * must have something to pass (`__extc_home`, or `&__extc_a[current block]`), so the caller
     * needs a home arena of its own. The rule and the iteration are `closeReach`; what is spelled
     * here is only which functions a round visits. */
    CloseCtx closeCtx = { m, tt, &c };
    closeReach(&closeCtx, REACH_HOME, roundNeedsHome);

    /* Single authority for arena levels: the checker hands codegen every decision it
     * needs, and codegen validates nothing on its own.
     *
     * While a body is being checked, `needsHome` has only the direct criterion (the body
     * contains a `new` and the return type carries a reference); the other half -- "the
     * function needs a home arena because it calls one that does" -- waits for the
     * transitive closure above. The `arenaLevel` computed at that point is therefore the
     * block level, while the generated C allocates as if the arena were a home arena. A
     * longer-lived arena is only safer, so this was not a hole, but it left two
     * authorities: an out-parameter allocation fell from the home arena back to a block
     * arena, which is what a golden test caught.
     *
     * Now, once the closure has run, every `new` in a function that needs a home arena is
     * rewritten to `ARENA_HOME`, so codegen's `arenaRefAt` no longer consults `g->hasHome`
     * and only translates the number the checker produced. */
    {
        Vec all; allFunctions(arena, m, &all);
        /* An instance's arena sites are the template's arena sites.
         *
         * `funcInstance` copies the template shallowly (`*in = *tmpl`), which copies the
         * `arenaSites` *struct*: same backing array, but the length as it stood at that
         * moment. An instance is created at a call site, and the calling function may be
         * declared before the template, so the template's own body was often checked
         * **after** the copy - and `vecPush` on the template then moved a length the
         * instance never saw. The instance was left with an empty list, so the pass below
         * never gave it a home arena, while its body (the template's, shared) had already
         * been rewritten to read `(*__extc_home)`: `f_i32` in
         * `tests/arena-promoted/R2a_generic_instance_first.extc` took no `__extc_home`
         * parameter and gcc rejected the generated C. Re-point both at one list here, once
         * every body has been checked, and the whole final phase agrees by construction.
         *
         * Placing the same `Expr*` twice is harmless: every pass below is idempotent for a
         * site (the level it computes does not depend on how often it runs). */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (planTemplate(f)) f->arenaSites = planTemplate(f)->arenaSites;
        }

        /* Recompute the direct `needsHome` criterion for every function, uniformly.
         *
         * The criterion in `checkFunc` (the body allocates and the return type carries a
         * reference) is evaluated only while that body is being checked, and a generic
         * instance's body is never checked on its own (`checkFunc` returns early for an
         * instance, where `planTemplate(fx)` is non-NULL). An instance's `needsHome` is
         * therefore only the shallow copy made
         * when it was created (`*in = *tmpl`, see `funcInstance`), so it inherits from
         * whichever function was created first and the same program behaves differently
         * under a different declaration order:
         *   - caller first: the instance is created before its template is checked, so
         *     `needsHome` is false. Codegen then emits no `__extc_home` parameter while
         *     the template's `new` has already been rewritten to `(*__extc_home)`, and
         *     the generated C does not compile (gcc: `__extc_home` undeclared).
         *   - template first: everything works.
         *
         * So, after the closure above (all bodies checked, `arenaSites` final), recompute
         * it once for every function -- instances and methods included -- using the
         * substituted return type. Declaration order no longer matters.
         *
         * Only the direct criterion is recomputed here; the "needs a home arena because it
         * calls one that does" half stays with the closure above. The recompute is
         * idempotent: it only sets the flag (`|=`), never clears it, so a result the
         * closure produced survives. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f->isExtern || !f->body) continue;
            Type *rt = f->ret;
            if (rt && f->typeParams.len > 0 && f->targs.len == f->typeParams.len)
                rt = ttSubstitute(c.tt, rt, &f->typeParams, &f->targs);
            if (stmtHasNew(f->body) &&
                ((rt && typeContainsRef(c.tt, rt)) || stmtStoresThroughDeref(&c, f->body, f)))
                f->needsHome = true;
        }

        /* Recompute the escape-sensitive home-arena decision: the analysis writes last and
         * the checks only read.
         *
         * While a body was being checked, `computeEscapes` could not see the callee effect
         * summaries (the call-graph closure had not run yet), so `l` in `pushOne(l, 7)` was
         * not marked as escaping even though the later `sink(out, l)` publishes it. The home
         * arena then came out as the caller's own frame, so whatever the callee put into the
         * container was released as soon as this call returned.
         *
         * The closure has run now, so recompute the escape set (`computeEscapes` reads
         * `callsNeedsHome` and the summaries) and update `arenaArg` at those sites. */
        /* Run a few rounds (at most four). `computeEscapes` decides its own fixed point against the
         * `isEscapeeName` set as it stands at that moment, and the names it marks need other
         * call sites to propagate them, so one round is not enough (measured: after the first
         * round the escape set was still empty). A few rounds push it to the fixed point;
         * there are few functions, so the cost is negligible, and `computeEscapes` itself is
         * capped at 32 rounds. */
        for (int round = 0; round < 4; round++) {
            bool changed = false;
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (f->isExtern || !f->body) continue;
                size_t before = c.escapees.len;
                computeEscapes(&c, f);
                if (c.escapees.len != before) changed = true;
            }
            if (!changed) break;
        }
        /* Decide on the union of the escape sets of all functions. `c.escapees` is the escape
         * set of the function currently being checked (it is rebuilt per function), while the
         * question asked here is whether a name can be moved out of the function it belongs
         * to, which is a different question.
         *
         * So take the union, conservatively: a name marked as escaping in any function counts
         * as possibly escaping. The direction is safe, since over-counting only makes the
         * home arena longer-lived: it costs memory but never misses an escape. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f->isExtern || !f->body) continue;
            computeEscapesMode(&c, f, true);   /* union mode: keeps what earlier functions added */
        }
        int eFixed = 0;
        for (size_t i = 0; i < c.eSites.len; i++) {
            EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
            if (!rec || !rec->call) continue;
            if (rec->overflow) {        /* args not all recorded => home arena (always sound) */
                planSetArenaArg(rec->call, ARENA_HOME);
                rec->call->arenaArgPending = false;
                continue;
            }
            /* The minimum depth over the recorded destinations, with `seen` to say whether a
             * value has been seen at all (INV-S).
             *
             * `nd == 0` used to double as "nothing seen yet" and as "this destination lives
             * outside the frame" -- and depth 0 is the *shallowest* there is, so the loop
             * computed the **maximum** instead of the minimum: `argDepth = {0, 1}` came out as
             * 1, and the call handed over the caller's own block arena where it had to hand over
             * the home arena. The generated program then read a block that had already been
             * released (audit P0-2: heap-use-after-free). Two separate representations remove
             * the ambiguity -- no argument recorded leaves `nd` at 0, which still means "home
             * arena", the conservative answer. */
            /* 目的地有多浅：与 zone 通道**共用一份**实现（U4；P0-2 的哨兵 bug 曾在这里
             * 与 zone 通道各有一份）。`overflow` 已在上面的分支里 `continue` 处理掉了。*/
            int nd = callSiteMinDestDepth(&c, rec);
            /* Write the new homeDepth onto `arenaArg`, by the same rules as `setCallArenaArg`. */
            int want = (nd <= 0) ? ARENA_HOME : nd;
            if (planArenaArg(rec->call) != want) { planSetArenaArg(rec->call, want); eFixed++; }
            rec->call->arenaArgPending = false;
        }

        /* Long-running memory: replay the "stored at level k" fact table to a fixed point.
         *
         * Why repeat: one fact can only promote the sites it touches, and once those sites
         * are longer-lived, other facts can promote as well, so keep going until a round
         * changes nothing. It is the same iteration as the four rounds over `EArenaSite` and
         * the transitive closure of `needsHome`.
         *
         * Why that is correct: a site's final level is the smallest level among all the
         * constraints that reach it, and `promoteInto` only moves a level down, that is, it
         * only extends a lifetime, so replay is monotone and must converge on that minimum.
         * The 32-round cap is the same number as the cycle guard inside `promoteInto`. */
        int solveRounds = 0;
        c.lvlSolving = true;            /* no recording during replay: this really happened */
        size_t nFacts = c.lvlFacts.len;          /* snapshot: frozen while solving */
        for (int round = 0; round < 32; round++) {
            bool changed = false;
            for (size_t i = 0; i < nFacts; i++) {
                LvlFact *f = *(LvlFact **)vecAt(&c.lvlFacts, i);
                if (!f || !f->val) continue;
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[replay] kind=%d at=%d\n", (int)f->val->kind, f->at);
                if (promoteInto(&c, f->val, f->at)) changed = true;
            }
            solveRounds++;
            if (!changed) break;
        }
        c.lvlSolving = false;
        if (c.fxOn) { c.fxReplayRounds += solveRounds; c.fxReplayFacts += (long)nFacts; }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] %zu level facts, converged after %d round(s)\n",
                    c.lvlFacts.len, solveRounds);

        int fixed = 0, keptBlock = 0;
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind == EX_NEW || site->kind == EX_GENCALL) {
                    /* The solver has finished, so place the site as the single authority:
                     *   - `escaped` (some path that touched it demanded a lifetime beyond
                     *     this frame) => the home arena (the arena the caller chose);
                     *   - otherwise => the block level the solver produced, which inside a
                     *     loop means reclaimed every iteration.
                     *
                     * This used to be an unconditional fallback: every `new` in a function
                     * that needs a home arena went to the home arena, so per-iteration
                     * temporaries that never escaped were pinned there until the frame ended.
                     * Measured on the 50/25/25 shape over 2e6 iterations: 98 MB, while the
                     * same shape with an inlined container needed only 50 MB. */
                    if (dbgOn("EXTC_DBG_MINAT"))
                        fprintf(stderr, "[minAt] %-8s line=%-4d minAt=%-3d arena=%d\n",
                                f->name ? f->name : "?", site->line, site->minAt, planArenaLevel(site));
                    if (dbgOn("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                planArenaLevel(site), f->needsHome?1:0, (int)site->kind);
                    if (dbgOn("EXTC_DBG_S3"))
                        fprintf(stderr, "[s3] %-8s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                planArenaLevel(site), f->needsHome?1:0, (int)site->kind);
                    int want;                            /* the arena it finally belongs to */
                    if (site->minAt == 0) {
                        want = ARENA_HOME;              /* must outlive this frame => home arena */
                    } else if (site->minAt >= 1) {
                        want = site->minAt;             /* must live to level k => block arena k */
                    } else {
                        /* No constraint touched it, so use its own level.
                         * `arenaLevel` cannot be consulted here: the provisional pass already
                         * rewrote every site in a function that needs a home arena to the
                         * not-yet-decided value. */
                        want = site->lexicalLevel >= 1 ? site->lexicalLevel : 1;
                    }
                    if (planArenaLevel(site) != want) {
                        planSetArenaLevel(site, want);
                        site->refDepth   = arenaDepthOf(want);   /* the single conversion point */
                        if (want == ARENA_HOME) fixed++; else keptBlock++;
                    }
                } else if (site->arenaArgPending) {
                    /* A call site that hands an arena down is settled **after** the fixed
                     * point below, from the callee's `usesHome`, and deliberately not here:
                     * codegen appends the extra argument exactly when the callee's
                     * `usesHome` is set, so the value has to come from that same flag. This
                     * branch used to answer from the *owning* function's `needsHome`, which
                     * is a different question (does this body have a home, not does the
                     * callee take one) and left the sites it did not settle with a block
                     * level - the caller then passed `&__extc_a[1]` while no such array was
                     * declared, and `tests/arena-promoted/R2a` stopped compiling. */
                    (void)0;
                }
            }
        }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] decided: %d sites in the home arena, %d kept at block level\n", fixed, keptBlock);

    /* ------------------------- the precise question a signature needs
     * `usesHome` is the narrow counterpart of `needsHome`: the per-node rule (a site that really
     * sits in the home arena, or handing this function's home on to a callee) is in `bodyReaches`
     * under `REACH_USES_HOME`, and the closure is the same one every other property uses. */
    closeReach(&closeCtx, REACH_USES_HOME, roundUsesHome);


    /* Settle every call site that hands an arena down, now that `usesHome` is final.
     *
     * This is the one place the value is decided, and it is decided by the flag codegen
     * reads to decide *whether* to pass anything at all: a callee that takes a home
     * parameter gets the caller's own, `__extc_home`, which its caller handed to it. The
     * fixed point above guarantees the caller has one - a body that calls a function with
     * `usesHome` is marked itself - so no arena array has to be materialized here, and the
     * `__extc_a` a function declares stays tied to the sites that really allocate.
     *
     * Anything left pending is a callee that takes no home parameter, so its site passes
     * nothing and the field is not read. */
    for (size_t i = 0; i < all.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        for (size_t j = 0; j < f->arenaSites.len; j++) {
            Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
            if (!site->arenaArgPending || !site->func || !planUsesHome(site->func)) continue;
            planSetArenaArg(site, ARENA_HOME);
            site->arenaArgPending = false;
        }
    }

        /* Recompute every site's `refDepth` from its final level, whether or not the level
         * changed.
         *
         * Why (measured with gdb on `tests/arena-promoted/C2_if_join_refbinding`): the
         * `site->refDepth = ...` in the placement loop above sits inside
         * `if (planArenaLevel(site) != want)`. For a site that the provisional pass had already
         * set to `ARENA_HOME` and the solver also placed in `ARENA_HOME`, the level did not
         * change, so that assignment never ran and `refDepth` kept a value that had been
         * damaged midway. Measured: the site of `alloc<i32>(1)` had `refDepth` 0, meaning
         * "must outlive this frame", while its level says 1, so the two contradicted each
         * other and the user saw "borrowed from depth 0", which makes no sense for an
         * allocation that lives in a block arena.
         *
         * The rule, and it was always the design rule: `refDepth` and `arenaLevel` must be
         * two ways of stating the same number, so sync them unconditionally after placement. */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                site->refDepth = arenaDepthOf(planArenaLevel(site));   /* keep the two in sync */
            }
        }
        /* `EXTC_DBG_HOME=1` prints, for every function, the two home flags and the arena
         * levels of its sites. Kept because the pair (`usesHome`, the levels of the very
         * sites it was computed from) is what has to agree for the generated C to compile,
         * and a mismatch is invisible in the source: the instance above took its parameter
         * from one answer and its body from the other. */
        if (dbgOn("EXTC_DBG_HOME"))
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (!f->body && !f->isExtern) continue;
                FuncDef *ft = planTemplate(f);
                const char *ftName = !ft ? "-" : (planInstName(ft) ? planInstName(ft) : ft->name);
                fprintf(stderr, "[home] %-24s uses=%d needs=%d tmpl=%-12s sites=%zu",
                        planInstName(f) ? planInstName(f) : f->name, (int)planUsesHome(f), (int)f->needsHome,
                        ftName,
                        f->arenaSites.len);
                for (size_t j = 0; j < f->arenaSites.len; j++)
                    fprintf(stderr, " L%d", planArenaLevel(*(Expr **)vecAt(&f->arenaSites, j)));
                fprintf(stderr, "\n");
            }

        /* Recompute the depths that were deferred until instantiation: they froze the value
         * from before the solver ran, and placement may have moved a site from the home arena
         * back down to a block level.
         *
         * Measured: the `return v` of `varArray<T>::withCap` froze `depth=1` in the template
         * round, while the `new T[cap]` inside it had correctly been placed in the home arena
         * (depth 0), so the instantiation re-check reported "depth 1, but this can only hold
         * up to 0" although the truth is 0.
         *
         * Recompute only shapes where `depthComesFromAlloc` holds, and only lower the value:
         * recomputing bindings would accept something like `generic_return_local`, which has
         * to be rejected. This was hit for real. */
        for (size_t i = 0; i < c.refChecks.len; i++) {
            RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
            if (!rc || !rc->val) continue;
            if (!depthComesFromAlloc2(&c, rc->val, 0)) continue;
            /* Recompute unconditionally, not only downwards: the value frozen before the
             * solver ran can be too low as well. In the template round the `refDepth` of
             * `new T[cap]` is still the not-yet-decided 0, and after solving it may be 1
             * (measured on `container-of-view`: `[post] ... frozen=0 ... now=1`).
             * Only shapes where `depthComesFromAlloc` holds are touched, so bindings (derived
             * from a parameter or from a call result) are never relaxed. */
            rc->depth = solvedDepth(rc->val);
        }
        if (getenv("EXTC_DUMP_OW"))
            fprintf(stderr, "[arena] single source of truth: %d site(s) (`new` and call sites) placed in a home arena\n", fixed);
    }

    /* ---- Which storage an `@overwrite` cell gets, and how many sites it has ----
     * Must run after every function body has been checked: whether a function can reach
     * itself is decided from the call graph. */
    {
        Vec all; allFunctions(arena, m, &all);
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            f->owSites = countOwSites(f->body);
            /* Set `owLocal` even when there are no sites: otherwise the signature of a
             * function with no parameters and no home arena loses its `void` (a golden test
             * caught this immediately). */
            /* An `@overwrite` cell always lives in the current frame -- **for the functions this
             * pass sees**. It is not always true at code generation time: a generic instance is
             * created later, during codegen, and keeps the `false` it was born with. That is the
             * case the comment above is about (a no-parameter instance whose signature then carries
             * parameters instead of `void`), which is why the flag is still read in four places in
             * `codegen.c` -- and why `funcReachesItself` is not called here any more: the decision
             * it was meant to feed is a constant for this pass's functions. */
            f->owLocal = true;
        }
        if (getenv("EXTC_DUMP_OW"))
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (f->owSites) fprintf(stderr, "[ow] %-18s sites=%d local=%d\n",
                                        f->name, f->owSites, (int)f->owLocal);
            }
    }

    /* ---- Compile-time optimization: does this function put anything into its own block
     * arena (`mayUseArena`)? Must be computed after the closure above has run. The test is
     * "the body has a `new`, or it calls a function that needs a home arena".
     *
     * When the answer is no, codegen drops the arena array and the release calls, which is
     * about 20% fewer lines of generated C (see the benchmark). `main` is always true: it is
     * the root home arena, and `__extc_home = &__extc_a[1]` needs that array. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f->isExtern || !f->body) { planSetMayUseArena(f, false); continue; } /* no body, no arena */
        planSetMayUseArena(f, stmtHasNew(f->body) || callsNeedsHome(f->body));
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            planSetMayUseArena(f, stmtHasNew(f->body) || callsNeedsHome(f->body));
        }
    }

    /* ---- Which functions can create a pool (`makesPool`)? ----
     *
     * Same shape as the `needsHome` closure above, and it has to be a closure for the same
     * reason: `vector<i32>::withCap` contains `extc_pool_new` and `withCap` calls `new`,
     * which calls `withCap`, so no single pass settles the answer. The rule
     *
     *     makesPool(f)  <=  f's body calls `extc_pool_new`
     *                    or f's body calls some g with makesPool(g)
     *
     * is monotone (a flag only ever goes from false to true) and the lattice is finite, so
     * iterating until a round changes nothing yields the *least* fixed point - which is the
     * answer wanted, not merely a consistent one: a larger fixed point would mark functions
     * that cannot create a pool and give back the hooks this pass exists to remove.
     *
     * Recursion and mutual recursion need no special case here, unlike the lazy
     * `funcAllocates` walk: a cycle simply stays false until one of its members is reached
     * from a body that really does call `extc_pool_new`, and if none of them ever is, then
     * none of them can create a pool at all.
     *
     * Three rounds: the plain functions, the methods of the non-generic structs, and -- because a
     * generic method is checked once per instance, with the protocol resolved for that instance --
     * the instances themselves. The rule and the iteration are `closeReach`; only the list of
     * functions a round visits is spelled here. */
    closeReach(&closeCtx, REACH_POOL, roundMakesPool);
    coroCheckDeferred(&c, m);
    /* 顺手把"这个结构体的某个方法建池"记到结构体上（见 StructDef.makesPoolAny）。 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (f && planMakesPool(f)) { sd->makesPoolAny = true; break; }
        }
    }
    /* ---- 调用点的 zone 层级：也要在 `makesPool` 闭包之后才能定 ----
     *
     * 与上面"电平求解再跑一遍"同一个病根、同一剂药：`zoneLevel` 决定"被调者的池分配活到
     * 第几层"，而**只有建池的被调者才会收到这个实参**；`makesPool` 是闭包 ⇒ 闭包之前它一律
     * 读作假，于是调用点停在临时层级上，跨两跳的链就会把自己的 place 传下去：
     *
     *     main → wrap → put → newString
     *
     * `wrap` 把**自己的** zone 传给了 `put`，`newString` 的缓冲因此生在 `wrap` 的 place 里，
     * `wrap` 一返回就被释放、下一次分配复用它 ⇒ ASan `heap-use-after-free`
     * （`tests/arena-soundness/H2_home_zone_two_hops.extc`）。
     *
     * 判据与 arena 完全同一个问题、同一个量：`nd` = 那个地方必须比被调者能存进去的每个
     * `mut ref` 实参（含接收者）活得更久；参数/全局算"本帧之外"（0），逃逸的局部量也算
     * （-1）。`ZONE_HOME` = "把我收到的 zone 一路传下去"。
     * 只往长里改（`<`），不往短里改：`zoneLevel` 从 `ZONE_HOME`（最长）向上是各层块号。 */
    for (size_t i = 0; i < c.eSites.len; i++) {
        EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
        if (!rec || !rec->call || !rec->call->func) continue;
        if (!planMakesPool(rec->call->func) || planZoneLevel(rec->call) == 0) continue;
        /* 目的地有多浅：与 arena 通道**共用一份**实现（U4）；`overflow` 由它在内部按
         * "活得最久"处理（两条通道的差别只剩这一点，现在写在共享函数里而不是两处代码里）。*/
        int nd = callSiteMinDestDepth(&c, rec);
        int zwant = (nd <= 0) ? ZONE_HOME : nd;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zfix] call=%s nd=%d lvl=%d -> %d\n",
                    rec->call->func->name ? rec->call->func->name : "-",
                    nd, planZoneLevel(rec->call), zwant);
        if (zwant < planZoneLevel(rec->call)) planSetZoneLevel(rec->call, zwant);
    }

    /* ---- 电平求解要再跑一遍 ----
     *
     * 第一遍跑在 `makesPool` 闭包**之前**（见上面那段求解），那时 `calleeMakesPool` 对所有
     * 调用点都还是假 ⇒ **池站点的提权一次都没落地**（实测 `[gate] call=new mP=0 cmp=0`，
     * 而同一个指针在 `EXTC_DUMP_POOL` 里是 `makesPool=1`）。
     * 闭包之后重放一次事实表，这次闸门读到的标志是真的；`promoteInto` 只把层级往小改，
     * 重放是单调的，所以多跑一遍不会引入新东西，只是把该落的提权落下去。 */
    c.lvlSolving = true;
    for (int round = 0; round < 32; round++) {
        bool changed2 = false;
        for (size_t i = 0; i < c.lvlFacts.len; i++) {
            LvlFact *f = *(LvlFact **)vecAt(&c.lvlFacts, i);
            if (!f || !f->val) continue;
            if (promoteInto(&c, f->val, f->at)) changed2 = true;
        }
        if (!changed2) break;
    }
    c.lvlSolving = false;

    if (getenv("EXTC_DUMP_POOL")) {
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            fprintf(stderr, "[pool] %-28s makesPool=%d mod=%s\n",
                    f->name ? f->name : "-", (int)planMakesPool(f),
                    f->modName && *f->modName ? f->modName : "-");
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                fprintf(stderr, "[pool] %s::%-22s makesPool=%d mod=%s\n", sd->name,
                        f->name ? f->name : "-", (int)planMakesPool(f),
                        f->modName && *f->modName ? f->modName : "-");
            }
        }
    }


    if (getenv("EXTC_SELFCHECK")) {
        int bad = 0;
        for (size_t i = 0; i < c.allSyms.len; i++) {
            Sym *sy = *(Sym **)vecAt(&c.allSyms, i);
            if (!sy) continue;
            /* Inequality, not equality: a reference may point at something that
             * outlives the site (`var cur: ?ref node = head`, where `head` lives
             * further out). Only claiming to be *shallower* than the site is wrong,
             * because that would mean pointing at storage that dies first.
             * The equality version was wrong and the escape-promotion example caught
             * it immediately, so the predicate itself needs checking too. */
            if (sy->origin && (sy->origin->kind == EX_NEW || sy->origin->kind == EX_GENCALL)) {
                int siteD = arenaDepthOf(planArenaLevel(sy->origin));
                if (sy->refDepth < siteD) {
                    fprintf(stderr, "[selfcheck] binding `%s` has refDepth=%d, shallower than"
                            " its site (level %d => %d)\n", sy->name, sy->refDepth,
                            planArenaLevel(sy->origin), siteD);
                    bad++;
                }
            }
        }
        Vec allF; allFunctions(arena, m, &allF);
        for (size_t i = 0; i < allF.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&allF, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                int want = arenaDepthOf(planArenaLevel(site));
                if (site->refDepth != want) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: refDepth=%d but"
                            " arenaLevel=%d (should be %d)\n", site->line,
                            f->name ? f->name : "?", site->refDepth, planArenaLevel(site), want);
                    bad++;
                }
                if (site->minAt == 0 && planArenaLevel(site) != ARENA_HOME) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: minAt=0 (must outlive"
                            " this frame) but it was not placed in the home arena\n",
                            site->line, f->name ? f->name : "?");
                    bad++;
                }
            }
        }
        if (bad)
            fprintf(stderr, "[selfcheck] %d consistency breaches (refDepth and arenaLevel"
                    " must describe the same fact)\n", bad);
        else if (getenv("EXTC_SELFCHECK_VERBOSE"))
            fprintf(stderr, "[selfcheck] all invariants hold\n");
        if (bad) ctx->hasError = true;      /* so scripts see the failure */
    }

    /* The full report, on request.
     *
     * Every level is final by now, so this is the one place that can print what the
     * checker knows about memory. The flag is read from the environment so that the driver
     * stays unaware of the report, the same arrangement `--dump-effects` uses. */
    if (getenv("EXTC_EXPLAIN_MEMORY") && getenv("EXTC_EXPLAIN_ROOT")
        && strcmp(getenv("EXTC_EXPLAIN_ROOT"), "1") == 0) {
        Vec allF; allFunctions(arena, m, &allF);
        reportMemory(&c, &allF);
    }

    /* `EXTC_DBG_FX=1`: what the analysis cost, in the quantities that can grow.
     *
     * The point of the split is that a scaling table can be read off it: `stores`/`lvlFacts`
     * are the **sizes** of the two fact tables, `storesSeen`/`symCmp`/`reachVisits` are what
     * the passes **walked** to fold them. A pass that is linear shows walked ~= size; the
     * front end being superlinear shows up as walked growing faster than size -- and which
     * of the three walked quantities it is tells which pass to look at. `own` is the share
     * of the walked records that belongs to the body being solved, so `seen/own` is the
     * ratio a per-body slice would remove. */
    if (c.fxOn) {
        size_t nBodies = 0, nEdges = 0;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (!f || !f->body) continue;
            nBodies++;
            nEdges += f->callees.len;
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (!f || !f->body) continue;
                nBodies++;
                nEdges += f->callees.len;
            }
        }
        fprintf(stderr, "[fx] sizes: bodies=%zu stores=%zu ownRecords=%ld lvlFacts=%zu"
                        " calleeEdges=%zu\n",
                nBodies, c.stores.len, c.fxTailRecords, c.lvlFacts.len, nEdges);
        fprintf(stderr, "[fx] levelpass: passes=%ld seen=%ld own=%ld altScan=%ld tblSum=%ld"
                        " tblMax=%ld\n",
                c.fxLvlPasses, c.fxStoresSeen, c.fxStoresOwn, c.fxAliasWalks,
                c.fxTblSum, c.fxTblMax);
        fprintf(stderr, "[fx] levelpass: rounds=%ld/%ld moved=%ld symCalls=%ld symCmp=%ld\n",
                c.fxLvlRounds1, c.fxLvlRounds2, c.fxLvlMoved, c.fxSymCalls, c.fxSymCmp);
        fprintf(stderr, "[fx] effects: calls=%ld cached=%ld edges=%ld\n",
                c.fxEffCalls, c.fxEffCached, c.fxEffEdges);
        fprintf(stderr, "[fx] reach: rounds=%ld visits=%ld replay=%ld rounds/%ld facts\n",
                c.fxReachRounds, c.fxReachVisits, c.fxReplayRounds, c.fxReplayFacts);
        fprintf(stderr, "[fx] stores: dedupSteps=%ld preBody=%ld\n",
                c.fxDedupSteps, c.fxPreBodyStores);
    }

    return !ctx->hasError;
}

/* Does this function allocate, directly or through the functions it calls?
 *
 * This is the same question the transitive closure of `needsHome` answers, but it has to be
 * answered lazily: that closure only runs after every function has been checked, while a
 * caller checking its own body already needs to know whether the call it is looking at puts
 * new storage into its container. `varArray::push` does not allocate itself, the `grow` it
 * calls does, so looking at `f->needsHome` alone misses a callee one hop away.
 *
 * Params:
 *   c - checker
 *   f - function to classify; null means "unknown"
 *
 * Returns:
 *   true when the function may allocate: its body has a `new`, or a callee allocates.
 *
 * Notes:
 *   - A function that is already being computed counts as allocating. That is the
 *     conservative direction, and it is what breaks a cycle.
 *   - The result is cached in `FuncDef.allocState`, so a repeated query is free.
 */
static bool stmtCallsAllocator(Checker *c, Stmt *s);   /* defined below; mutually recursive */
static bool allocInStmt(void *ctx, Stmt *s);
static bool funcAllocates(Checker *c, FuncDef *f) {
    if (!f) return true;                     /* unknown, so assume it allocates */
    if (f->isExtern) return false;           /* an extern function does not touch our arenas */
    if (f->allocState == 1) return true;
    if (f->allocState == 2) return false;
    if (f->allocState == 3) return true;     /* on the cycle being computed => conservative */
    f->allocState = 3;
    /* The same per-node criterion the closures use (`bodyReaches`), plus the callee half that only
     * this lazy path can afford to ask: it runs while a body is being checked, before the closures
     * exist, and it counts a function already on the stack as allocating, which is the
     * conservative direction for a question asked mid-check. */
    bool r = bodyReaches(f, REACH_ALLOC) || stmtCallsAllocator(c, f->body);
    f->allocState = r ? 1 : 2;
    return r;
}

/* Does this statement or expression call a function that needs a home arena?
 *
 * The two walkers are mutually recursive and share the per-function cache, so this is only
 * meaningful after the callee has been checked: `e->func` is filled in by then.
 */
/* Can evaluating this expression reach an allocator?
 *
 * `alloc<T>(n)` **is** an allocation, the same as `new`; a call counts when the callee may
 * allocate (`funcAllocates`, memoised because it recurses through the call graph); everything
 * else is answered by the children. The walk is `astWalkExprChildren`, the one place that lists
 * the kinds. */
typedef struct { Checker *c; } AllocCtx;

static bool allocInExpr(void *ctx, Expr *e) {
    AllocCtx *a = (AllocCtx *)ctx;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) &&
        (!e->func || funcAllocates(a->c, e->func))) return false;   /* found */
    if (e->kind == EX_NEW || e->kind == EX_GENCALL) return false;   /* found */
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

static bool allocInStmt(void *ctx, Stmt *s) {
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

static bool stmtCallsAllocator(Checker *c, Stmt *s) {
    if (!s) return false;
    AllocCtx a = { c };
    return !allocInStmt(&a, s);
}

/* Will codegen bracket this block with a place (`zoneEnter`/`zoneLeaveTo` + `arena_release`)?
 *
 * The checker needs a **conservative superset**: saying "no" for a block that codegen does bracket
 * would let a `yield` sit inside a reclaimed block (rule 3, docs/topics/CONCURRENCY.md 12), so it
 * asks the two predicates it already has. Codegen's own decision is finer, and finer is safe here.
 */
static void coroCheckDeferred(Checker *c, Module *m) {
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || planTemplate(cf)) continue;
        /* A boxed coroutine always has a task: its frame lives in that task's place. */
        planSetCoroNeedsZone(cf, planMakesPool(cf) || planCoroBoxed(cf));
    }
    /* **Instances** need the same answer, and this loop used to skip them, so a
     * generic coroutine never got its place: allocations made inside it landed in the *caller's*
     * zone, and nothing ever released the task (audit P1-13: `live=20000` after 20000 finished
     * tasks; the same omission is what the audit's "`coroNeedsZone` 未对实例计算" names).
     * An instance shares its template's body, so the template's freshly computed answer is the
     * right one -- and a coercion records `coroBoxed` on whichever function owns the frame. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || !planTemplate(cf)) continue;
        planSetCoroNeedsZone(cf, planCoroNeedsZone(planTemplate(cf)) || planCoroBoxed(cf));
    }
    for (size_t i = 0; i < c->coroDeferred.len; i++) {
        CoroDeferred *d = *(CoroDeferred **)vecAt(&c->coroDeferred, i);
        /* Two shapes a reference may legally outlive a suspension in:
         *   - a pooled container: the pool has a place of its own (the older exception);
         *   - a `new` written **in this coroutine's own body**: its storage is the frame's block arena
         *     (`arenaRefAt` puts every block level in the frame), which lives as long as the task does.
         * Everything else -- a parameter, a `ref` into the caller, a view handed in -- still gets the
         * error below, because that storage really is gone when the step returns. */
        if (d->init && (exprMakesPool(d->init, true) || d->init->kind == EX_NEW)) continue;
        ckError(c, d->line,
                "A local that lives across a `yield` may not point at someone else's storage: the C"
                " stack of this call is gone when the coroutine yields. Two fixes: allocate it"
                " yourself in this body (`new` lands in the frame's own arena, a pooled container in"
                " the pool's place), or keep the offset and take the view again after the resume"
                " (docs/topics/CONCURRENCY.md 4.4)",
                "`%s` lives across a `yield` and carries a reference", d->cname);
    }
    c->coroDeferred.len = 0;
}

bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro) {
    if (!s) return false;
    /* A coroutine body has no place boundary any more, and that is measured, not assumed:
     *   - block arenas live **in the frame** (`arenaRefAt`), so an allocating block survives a
     *     suspension and its own exit still reclaims it;
     *   - a pool's storage is the **task's place**, not the block's -- `tests/coro/coro_pool.extc`
     *     has been creating a vector at body level, growing it across three suspensions and staying
     *     ASan-clean since before this rule existed; the zone leave in a step goes through the
     *     "current top" fallback, so the stack-local mark is never read after a resume.
     * So `placeBoundaryDepth` stays 0 inside a coroutine and the check in the `ST_YIELD` case cannot
     * fire there. It stays in the code because it is the invariant that would have to come back the
     * day a suspension can sit somewhere its storage does not survive. */
    if (inCoro) return false;
    return stmtHasNew(s) || stmtMakesPool(s, true);
}

/* ---- the frame of a coroutine (docs/topics/CONCURRENCY.md 4.4) -------------------------------
 *
 * Which locals live in the frame? The ones **live across a `yield`**: declared before it and used
 * after it. One walk answers both questions the design needs:
 *
 *   1. rule 2 -- such a local may not carry a reference (a view, a `ref`, a container): its storage
 *      is either the C stack of this very call, which is gone the moment the coroutine yields, or an
 *      arena that dies before the task does. The rejection message carries the two fixes.
 *   2. the frame layout codegen needs (slice B): those locals by value, plus `pc`, the return slot,
 *      and the zone id when the body can create a pool.
 *
 * The walk is the authoritative one (`astWalkStmtChildren` / `astWalkExprChildren`), so a new kind
 * cannot be forgotten here, and its pre-order visit order is what "before" and "after" mean.
 *
 * **Loop-back edges** make "after" more than source order: a use that sits inside a loop which also
 * encloses the `yield` can run again *after* the resume, even when the textual use comes first --
 * `while c { use(a)  yield x }` uses `a` again on the next iteration. So a local counts as live
 * across when the use and the yield **share an enclosing loop** (the loop chain is recorded per node;
 * deeper than the chain array means "assume shared", which is the conservative direction). Getting
 * this wrong the other way would let a dangling view through silently, which is exactly what these
 * rounds exist to prevent.
 */
#define CORO_CHAIN_MAX 8
typedef struct {
    const char *cname;
    int         index;
    int         chain[CORO_CHAIN_MAX];      /* the enclosing loop ids, outermost first */
    int         nchain;
} CoroUse;
typedef struct {
    const char *cname;
    Type       *type;
    Expr       *init;
    int         index;
} CoroDecl;
typedef struct {
    int index;
    int line;
    int chain[CORO_CHAIN_MAX];
    int nchain;
} CoroYield;
typedef struct {
    AstVisit v;                 /* so the callbacks can share one context */
    int      index;
    int      chain[CORO_CHAIN_MAX];
    int      nchain;
    int      nextLoop;
    Vec      decls;             /* CoroDecl*  */
    Vec      uses;              /* CoroUse*   */
    Vec      yields;            /* CoroYield* */
} CoroScan;

static void coroPushLoop(CoroScan *s, int id) {
    if (s->nchain < CORO_CHAIN_MAX) s->chain[s->nchain++] = id;
    else s->nchain = CORO_CHAIN_MAX + 1;      /* too deep: `coroSharesLoop` then says yes */
}
static void coroPopLoop(CoroScan *s) {
    if (s->nchain == CORO_CHAIN_MAX + 1) s->nchain = CORO_CHAIN_MAX;
    else if (s->nchain > 0) s->nchain--;
}
static void coroCopyChain(CoroScan *s, int *chain, int *n) {
    *n = s->nchain > CORO_CHAIN_MAX ? CORO_CHAIN_MAX : s->nchain;
    for (int i = 0; i < *n; i++) chain[i] = s->chain[i];
}
/* Do these two nodes share an enclosing loop? (`a.nchain` too deep means "assume yes".) */
static bool coroSharesLoop(const int *ac, int an, const int *bc, int bn) {
    for (int i = 0; i < an; i++)
        for (int j = 0; j < bn; j++)
            if (ac[i] == bc[j]) return true;
    return false;
}

static bool coroScanStmt(void *ctx, Stmt *s);
static bool coroScanExpr(void *ctx, Expr *e);

static bool coroScanExpr(void *ctx, Expr *e) {
    CoroScan *s = (CoroScan *)ctx;
    if (e->kind == EX_IDENT && e->u.ident.cname) {
        CoroUse *u = (CoroUse *)vecPush(&s->uses);   /* by value: never a pointer into the vec */
        u->cname = e->u.ident.cname;
        u->index = s->index++;
        coroCopyChain(s, u->chain, &u->nchain);
    } else {
        s->index++;
    }
    AstVisit v = { coroScanExpr, coroScanStmt, ctx };
    return astWalkExprChildren(e, &v);
}

static bool coroScanStmt(void *ctx, Stmt *s) {
    CoroScan *sc = (CoroScan *)ctx;
    if (s->kind == ST_VAR && s->u.var.cname) {
        CoroDecl *d = (CoroDecl *)vecPush(&sc->decls);   /* by value */
        d->cname = s->u.var.cname;
        d->type = s->type;
        d->init = s->u.var.init;
        d->index = sc->index++;
    } else if (s->kind == ST_YIELD) {
        CoroYield *y = (CoroYield *)vecPush(&sc->yields);
        y->index = sc->index++;
        y->line = s->line;
        coroCopyChain(sc, y->chain, &y->nchain);
    } else {
        sc->index++;
    }
    const bool loop = s->kind == ST_WHILE;
    if (loop) coroPushLoop(sc, sc->nextLoop++);
    AstVisit v = { coroScanExpr, coroScanStmt, ctx };
    bool ok = astWalkStmtChildren(s, &v);
    if (loop) coroPopLoop(sc);
    return ok;
}

/* Is `d` live across the yield `y`? Either used after it, or used in a loop they share. */
static bool coroLiveAcross(const CoroScan *s, const CoroDecl *d, const CoroYield *y) {
    for (size_t i = 0; i < s->uses.len; i++) {
        const CoroUse *u = (const CoroUse *)vecAt((Vec *)&s->uses, i);
        if (!u->cname || strcmp(u->cname, d->cname) != 0) continue;
        if (u->index > y->index) return true;
        if (u->index > d->index && coroSharesLoop(u->chain, u->nchain, y->chain, y->nchain))
            return true;
    }
    return false;
}

/* Lay out the frame of a coroutine, and refuse what may not go in it. Runs after the body has been
 * checked, so every declaration's type is final. */
static void coroFrameLay(Checker *c, FuncDef *f) {
    if (!f || !planIsCoro(f) || !f->body) return;
    CoroScan s;
    memset(&s, 0, sizeof s);
    s.index = 0;
    /* Stored **by value**: these vectors grow while they are being filled, so holding pointers to
     * their elements would leave dangling pointers the moment one of them reallocates. */
    vecInit(&s.decls, c->arena, sizeof(CoroDecl));
    vecInit(&s.uses, c->arena, sizeof(CoroUse));
    vecInit(&s.yields, c->arena, sizeof(CoroYield));
    CoroScan *sp = &s;
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        coroScanStmt(sp, *(Stmt **)vecAt(&f->body->u.block.stmts, i));
    if (s.yields.len == 0) return;                  /* nothing to lay out */

    /* The frame holds the **parameters** too: a resume has no arguments to pass them again, so they
     * are captured at spawn and read back on every step (docs/topics/CONCURRENCY.md 4.4). */
    vecInit(&f->coroFrame, c->arena, sizeof(Param));   /* by value, like the scan above */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *src = *(Param **)vecAt(&f->params, i);
        Param *p = (Param *)vecPush(&f->coroFrame);
        *p = *src;
    }
    for (size_t i = 0; i < s.decls.len; i++) {
        const CoroDecl *d = (const CoroDecl *)vecAt(&s.decls, i);
        bool live = false;
        int line = f->body->line;
        for (size_t k = 0; k < s.yields.len && !live; k++) {
            const CoroYield *y = (const CoroYield *)vecAt(&s.yields, k);
            if (d->index < y->index && coroLiveAcross(&s, d, y)) { live = true; line = y->line; }
        }
        if (!live) continue;                        /* stays an ordinary C local ✓ */
        if (typeContainsRef(c->tt, d->type)) {
            /* Safe or not depends on who owns the storage, which the pool fixpoint only settles
             * later; the decision is made in `coroCheckDeferred`. **No `continue`**: the local still
             * belongs in the frame (a resume jumps past its declaration). */
            if (!c->coroDeferred.arena)
                vecInit(&c->coroDeferred, c->arena, sizeof(CoroDeferred *));
            CoroDeferred *dl = arenaAllocZero(c->arena, sizeof *dl);
            dl->cname = d->cname; dl->init = d->init; dl->line = line;
            *(CoroDeferred **)vecPush(&c->coroDeferred) = dl;
        }
        if (0 && typeContainsRef(c->tt, d->type)) {
            ckError(c, line,
                    "A local that lives across a `yield` may not carry a reference: its storage is"
                    " gone when the coroutine resumes (the C stack of this call is, and a block's"
                    " arena is). Two fixes: allocate it yourself with `new` so it lands in the"
                    " task's own place, or keep the offset and take the view again after the"
                    " resume (docs/topics/CONCURRENCY.md 4.4)",
                    "`%s` lives across a `yield` and carries a reference", d->cname);
            continue;
        }
        Param *p = (Param *)vecPush(&f->coroFrame);
        p->name = d->cname;
        p->cname = d->cname;
        /* The frame is laid out from the **body's** recorded declarations, and for an instance that
         * body is still the template's -- so a local declared `var b: box<T>` kept the parameter
         * spelling in the frame: the instance's frame had `box_i64 ret` next to `box_T b` and the C
         * said "incompatible types when assigning to type 'box_i64' from type 'box_T'"
         * (tools/attack.py X8). Substitute with the instance's arguments here; the template keeps its
         * own spelling, which is what `ttSubstitute` returns unchanged when there is no template. */
        p->type = (planTemplate(f) && f->targs.len)
                    ? ttSubstitute(c->tt, d->type, &planTemplate(f)->typeParams, &f->targs)
                    : d->type;
        p->line = f->body->line;
    }
    /* The frame is a **real type**, with real fields: the units pass scans a struct's fields to
     * find the instances its storage needs, and a `vector<i32>` living in the frame is exactly how a
     * coroutine pulls in that container's helpers. `zone` is always present -- eight bytes, and it
     * keeps the layout independent of the pool fixpoint, which only runs later. */
    {
        StructDef *fsd = planCoroFrameType(f) ? planCoroFrameType(f)->sdef : NULL;
        if (fsd) {
            if (!fsd->fields.arena) vecInit(&fsd->fields, c->arena, sizeof(FieldDef *));
            Type *i64t = ttFromName(c->tt, "i64");
            FieldDef *fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "pc"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "ret"; fd->type = planYieldType(f); fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "zone"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The task id: the table owns the place, and the step reads the zone beside it rather
             * than re-deriving it (rule 1). See src/coroutine.c. */
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "task"; fd->type = i64t; fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The **incoming slot**: what a resume hands the coroutine, read by a `yield` binding. */
            fd = arenaAllocZero(c->arena, sizeof *fd);
            fd->name = "in"; fd->type = planYieldType(f); fd->line = f->line;
            *(FieldDef **)vecPush(&fsd->fields) = fd;
            /* The block arenas, one per block level. A step has no C-stack arena array, and a block in
             * a coroutine can span a suspension (`new` ... `yield` ... use), so the arena lives here
             * and the block's release point still reclaims it. `cType` renders the synthesized field
             * type verbatim, so the generated struct says `extc_arena arena1;` and friends. */
            {
                int maxLv = 1 + blkMaxOfBlock(f->body);
                for (int lv = 1; lv <= maxLv; lv++) {
                    Type *at = arenaAllocZero(c->arena, sizeof *at);
                    at->kind = TY_STRUCT;
                    at->name = "extc_arena";
                    FieldDef *af = arenaAllocZero(c->arena, sizeof *af);
                    af->name = arenaPrintf(c->arena, "arena%d", lv);
                    af->type = at;
                    af->line = f->line;
                    *(FieldDef **)vecPush(&fsd->fields) = af;
                }
            }
            /* `yield` bindings recorded while the body was checked: frame locals like any other. */
            for (size_t i = 0; i < c->coroBinds.len; i++) {
                CoroBind *cb = *(CoroBind **)vecAt(&c->coroBinds, i);
                if (cb->fn != f) continue;
                bool dup = false;
                for (size_t k = 0; k < f->coroFrame.len && !dup; k++) {
                    const Param *pp = (const Param *)vecAt(&f->coroFrame, k);
                    dup = pp->name && strcmp(pp->name, cb->name) == 0;
                }
                if (dup) continue;
                Param bp;
                bp.name = bp.cname = cb->name;
                bp.type = cb->type;
                bp.line = cb->line;
                *(Param *)vecPush(&f->coroFrame) = bp;
            }
            for (size_t k = 0; k < f->coroFrame.len; k++) {
                const Param *p = (const Param *)vecAt(&f->coroFrame, k);
                fd = arenaAllocZero(c->arena, sizeof *fd);
                fd->name = p->name; fd->type = p->type; fd->line = f->line;
                *(FieldDef **)vecPush(&fsd->fields) = fd;
            }
            if (!fsd->type) fsd->type = planCoroFrameType(f);
        }
    }

    if (getenv("EXTC_DBG_CORO")) {
        fprintf(stderr, "[coro] %s: frame = pc, ret%s", f->name,
                planMakesPool(f) ? ", zone" : "");
        for (size_t i = 0; i < f->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt(&f->coroFrame, i);
            fprintf(stderr, ", %s: %s", p->name, typeStr(c, p->type));
        }
        fprintf(stderr, "   (%zu fields: %zu param%s + %zu live-across-yield local%s)\n",
                f->coroFrame.len, f->params.len, f->params.len == 1 ? "" : "s",
                f->coroFrame.len - f->params.len,
                f->coroFrame.len - f->params.len == 1 ? "" : "s");
    }
}
