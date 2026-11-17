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
#include "typelayer.h"   /* the pool questions below are shared with codegen */
#include "plan.h"          /* plan/analysis products: written through setters, read through accessors */
/* Defined further down; the call-site publication needs it (INV-K). */
static void noteFieldSrc(Checker *c, Sym *root, const char *field, int d2, Expr *src);
#include "dataflow.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <stdlib.h>   /* getenv (the EXTC_DUMP_EFFECTS debug switch) */
#include <time.h>

/**
 * @brief Whether phase timing is enabled (environment variable `EXTC_DBG_TIME`).
 *
 * The environment is read only once.
 *
 * @return  1 when enabled; 0 otherwise.
 */
static int ctOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }

/**
 * @brief Process CPU time in seconds.
 *
 * @return  CPU seconds consumed since process start.
 */
static double ctNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/**
 * @brief Print one phase's elapsed time; does nothing while the switch is off.
 *
 * @param n   Phase name.
 * @param t0  CPU seconds at the start of the phase.
 */
static void ctPhase(const char *n, double t0) { if (ctOn()) fprintf(stderr, "[time] %-9s %.3f s\n", n, ctNow() - t0); }

/**
 * @brief Fetch the type parameter list visible to a function or method.
 *
 * A method sees its owning struct's type parameters, a free function its own; every place in a
 * function body that resolves a type asks only here.
 *
 * @param f  Function or method; NULL allowed.
 * @return   The parameter table to instantiate; NULL when not generic.
 */
Vec *funcTParams(FuncDef *f) {
    if (!f) return NULL;
    if (f->typeParams.len) return &f->typeParams;
    if (f->owner) return &f->owner->typeParams;
    return NULL;
}

/* Forward declarations: the effect-summary walk and the escape analysis call each
 * other, so one of them has to be declared ahead of its definition. */
/**
 * @brief Tell whether two declarations come from the same module.
 *
 * @param a  Module name; NULL means the root file (or the prelude).
 * @param b  Module name; NULL means the root file (or the prelude).
 * @return   True when both are NULL or the names are equal; false otherwise.
 */
static bool sameModule(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static int  paramIndexByName(FuncDef *f, const char *name);   /* only used when only a name is available; use `paramIndexOfExpr` when an expression is available */
static bool exprIsFresh(Expr *e);   /* used by the call-site publication (INV-K) */

/* ------------------------------------------------------------------- top level */

/**
 * @brief Resolve every type named in a signature, in place.
 *
 * Runs before any function body is checked, so later checking does not have to resolve type names.
 * The type names in a signature belong to the file that declares it, so they are resolved under
 * that file's context (`FuncDef.ctx`).
 *
 * @param c  Checker (type table and the module context used for resolution).
 * @param f  Function whose parameter and return types are resolved in place.
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

/**
 * @brief Do two `extern!` declarations name the same C function?
 *
 * Two modules may want the same runtime entry point (`extc_pool_new` is the first case: the
 * prelude grows its own array and `std::sys::pool` declares the primitive for library code).
 * The prelude cannot `use` a module, so the only way to say "the same C symbol" twice is to
 * allow a repeated declaration whose signature matches. A signature that *differs* is still
 * an error: the C side is one function, so the two views have to agree.
 *
 * @param a  First declaration.
 * @param b  Second declaration.
 * @return true when both are extern and every parameter and the result type agree.
 */
static bool sameExternSignature(FuncDef *a, FuncDef *b) {
    if (!a->isExtern || !b->isExtern) return false;
    if (a->params.len != b->params.len) return false;
    if (!a->ret || !b->ret || !ttEquals(a->ret, b->ret)) return false;
    for (size_t i = 0; i < a->params.len; i++) {
        Param *pa = *(Param **)vecAt(&a->params, i);
        Param *pb = *(Param **)vecAt(&b->params, i);
        if (!pa->type || !pb->type || !ttEquals(pa->type, pb->type)) return false;
    }
    return true;
}

/**
 * @brief Reject duplicate names among module-level declarations.
 *
 * Names are looked up in the scope table by string, so a duplicate silently shadows the other
 * definition instead of being reported; each declaration kind is compared separately so the
 * diagnostic can name the conflicting pair. The error is reported at the line of the later
 * declaration.
 *
 * @param c  Checker.
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
            if (strcmp(a->name, b->name) == 0) {
                if (sameExternSignature(a, b)) continue;   /* one C symbol, declared twice */
                ckError(c, b->line,
                        (a->reserved || b->reserved)
                            ? "It comes from stdlib/prelude.extc and is part of the language contract."
                            : NULL,
                        (a->reserved || b->reserved)
                            ? "`%s` is a reserved definition and cannot be redefined"
                            : "duplicate function `%s`", b->name);
            }
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
                                 " replace it: the first declaration is at %s:%d",
                          ma->ctx ? ma->ctx->path : "this file",
                          (ma->body && ma->body->line > 0) ? ma->body->line : ma->line);
                /* Reported at the file of the **second declaration**: after a type's method set is merged across
 * modules, using the entry file's line would point the reader at a line unrelated to this rule. */
                Ctx *savedCtx = c->ctx;
                if (mb->ctx) c->ctx = mb->ctx;
                ckError(c, mb->line, bufCstr(&note), "struct `%s` has duplicate method `%s`",
                        DN(sd), mb->name);
                c->ctx = savedCtx;
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
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        StructDef *sd = structOf(planImplTarget(im));
        if (!sd) continue;
        Vec selfParams, selfArgs;
        vecInit(&selfParams, c->arena, sizeof(const char *));
        vecInit(&selfArgs,   c->arena, sizeof(Type *));
        *(const char **)vecPush(&selfParams) = "Self";
        *(Type **)vecPush(&selfArgs) = planImplTarget(im);
        /* `impl Codec<i64> for box`: the trait's **own** parameters substitute through the same
         * pair of parallel lists, so a declared signature that mentions `T` is compared against
         * the implementation as `i64`. Without this the conformance check compared `T` with `i64`
         * and rejected every explicit-parameter trait. */
        for (size_t pi = 0; pi < im->traitArgs.len && pi < planImplTrait(im)->typeParams.len; pi++) {
            *(const char **)vecPush(&selfParams) =
                *(const char **)vecAt(&planImplTrait(im)->typeParams, pi);
            /* Resolved, not as written: the parser hands back an unresolved `i64`, and substituting
             * that in produces a look-alike node that `ttEquals` (interning identity) rejects --
             * the check then reported "is `i64`, but the trait declares `i64`". */
            *(Type **)vecPush(&selfArgs) =
                ttResolve(c->tt, c->ctx, *(Type **)vecAt(&im->traitArgs, pi), im->line, NULL);
        }
        for (size_t k = 0; k < planImplTrait(im)->methods.len; k++) {
            if (k + 8 < planImplTrait(im)->methods.len) __builtin_prefetch(*(FuncDef **)vecAt(&planImplTrait(im)->methods, k + 8), 0, 0);
            FuncDef *want = *(FuncDef **)vecAt(&planImplTrait(im)->methods, k);
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
                        planImplTrait(im)->name, want->name);
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
                            planImplTrait(im)->name, j + 1, want->name,
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
                        planImplTrait(im)->name, want->name, typeStr(c, haveR), typeStr(c, wantR));
        }
    }

}

/**
 * @brief Is this expression something a call site can copy verbatim into an argument slot?
 *
 * Default values travel to the call site, so they may not read the callee's locals or the
 * caller's state: only literals (and a conversion or a negation of one) qualify. The rule is
 * also what makes copying safe -- a constant has no plan facts that differ between two uses.
 *
 * @param e  Expression written after `=` in a parameter list.
 * @return   `true` when the value is a compile-time constant.
 */
static bool defaultIsConstant(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: return true;
    case EX_UN:   return defaultIsConstant(e->u.un.operand);
    case EX_CONV: return defaultIsConstant(e->u.conv.operand);
    default:      return false;
    }
}

/**
 * @brief Check the shape of a method: where it is declared and what `self` is.
 *
 * A function that operates on a value must be declared inside a struct; operators such as `==` are
 * such functions, so writing one outside a struct is an error rather than a free function that
 * happens to be named `==`.
 *
 * @param c  Checker.
 * @param f  Function to check; `f->owner` is NULL for a free function.
 */
static void checkMethodShape(Checker *c, FuncDef *f) {
    /* Default values (`t: bool = false`): a call may leave out the tail of the parameter list,
     * so the defaults have to form a suffix, and they have to be constants the call site can
     * copy. Both rules are checked here, before any body is looked at, so the diagnostics do not
     * depend on which arguments some call happens to pass. */
    bool sawDefault = false;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (p->dflt) {
            sawDefault = true;
            if (i == 0 && f->owner && strcmp(p->name, "self") == 0)
                ckError(c, p->line, NULL, "`self` cannot have a default value");
            else if (!defaultIsConstant(p->dflt))
                ckError(c, p->line,
                        "the value is copied into every call that leaves the argument out, so it "
                        "may not read anything: a literal (or a conversion or negation of one) is "
                        "the whole language here",
                        "a default value must be a compile-time constant");
        } else if (sawDefault) {
            ckError(c, p->line,
                    "a call leaves out the arguments at the end only, so every parameter after the "
                    "first one with a default value needs one too",
                    "parameter `%s` comes after a parameter with a default value, so it needs one "
                    "too", p->name);
        }
    }

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
        anSetIsAssoc(f, true);
    } else {
        /* Compare the struct definition rather than the type node: the `self` of a
         * method on a generic struct is written `ref Pair<A, B>`, so its type node
         * is not the same pointer as the owner's type. */

        Type *sb = ttBase(p0->type);
        /* A builtin scalar has no `sdef` to compare against, so its holder is matched by the
         * type it stands for (`impl i64` -> `self: ref i64`). Every other owner is a real
         * declaration and is matched by definition, which is also what lets a generic method
         * write `ref Pair<A, B>` and still belong to `Pair`. */
        bool selfOk = planBuiltinHolder(f->owner)
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
    /* Did the initializer name storage that outlives every frame? Recorded while the scopes were
     * live (`ST_VAR.initOutOfFrame`): by the time `coroCheckDeferred` runs, `lookup` cannot
     * find a function-local name, so asking `exprOutOfFrame` again would answer "no" for a view of
     * a `global` declaration. */
    bool        outOfFrame;
} CoroDeferred;

static bool hasNewInStmt(void *ctx, Stmt *s);
/** How the "does this contain an allocation" walk treats a generic allocation call.
 *
 * `alloc<T>(n)` and `new T[n]` both reserve fresh storage, but `poolSlice`-style calls only borrow
 * a slice of a plate that already exists: repeating one per round reuses the same memory, so a
 * question about retained storage must not count it as an allocation. */
typedef struct {
    bool skipPoolSlices;    /* after `poolSlice` / `poolResize`: borrows, does not allocate */
} NewScan;

static bool exprIsPoolBlock(Expr *e);

/**
 * @brief Tell whether an expression contains `new` or `alloc<T>(n)`.
 *
 * The walk goes through `astWalkExprChildren` / `astWalkStmtChildren`, so AST kinds are listed only
 * there.
 *
 * @param ctx  `NewScan` options, or NULL for "count every `EX_GENCALL` as an allocation".
 * @param e    Expression.
 * @return     Callback convention: false to stop the walk when found, true otherwise.
 */
static bool hasNewInExpr(void *ctx, Expr *e) {
    const NewScan *sc = ctx;
    if (e->kind == EX_NEW) return false;                            /* found */
    if (e->kind == EX_GENCALL &&
        !(sc && sc->skipPoolSlices && exprIsPoolBlock(e))) return false; /* found */
    AstVisit v = { hasNewInExpr, hasNewInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement-side walk callback for `hasNewInExpr`.
 *
 * @param ctx  Walk options, passed through to the expression side.
 * @param s    Statement.
 * @return     Callback convention: false to stop the walk when found, true otherwise.
 */
static bool hasNewInStmt(void *ctx, Stmt *s) {
    AstVisit v = { hasNewInExpr, hasNewInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Tell whether an expression contains `new` or `alloc<T>(n)`.
 *
 * @param e  Expression; NULL returns false.
 * @return   True when it does.
 */
static bool exprHasNew(Expr *e) {
    if (!e) return false;
    return !hasNewInExpr(NULL, e);
}

/**
 * @brief Tell whether an expression **allocates fresh storage**.
 *
 * Same walk as `exprHasNew`, with one difference: a `poolSlice`-family call is a borrow of a plate
 * that already exists, not an allocation, so repeating it each round does not pile anything up. The
 * "buffer written into a binding from an enclosing block" warning asks this narrower question --
 * counting borrows there would flag the ordinary "reset the pool, take the slice again" loop.
 *
 * @param e  Expression; NULL returns false.
 * @return   True when some node allocates (`new` or `alloc<T>(n)` that is not a plate borrow).
 */
bool exprContainsNew(Expr *e) {
    if (!e) return false;
    const NewScan sc = { true };
    return !hasNewInExpr((void *)&sc, e);
}

/**
 * @brief Tell whether a statement contains `new` or `alloc<T>(n)`.
 *
 * @param s  Statement; NULL returns false.
 * @return   True when it does.
 */
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
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which
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
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which the
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
 * storage lives -- bugs of exactly this shape are on record:
 *
 *   - `EX_STRUCTLIT` was missing from this predicate, so `var b: box = { r: mknode() }` looked
 *     like a body that reaches no callee with a home arena. The function was not given one, the
 *     callee allocated into the caller's *block* arena (released when the block ends) while the
 *     depth recorded for the value said 0, and the pointer dangled. Every shape a call can hide
 *     in therefore has to be listed -- which is now the job of `ast.c`, not of this file.
 * The same missing-kind mistake was also made once for `EX_DYN`'s payload and once for
 * `EX_SLICE`'s bounds; migrating this predicate ends that class
 * of bug for it.
 *
 * Notes:
 *   - Only valid after the calls have been resolved: the walk reads `planCallee(e)`, which the checker
 *     fills in when it resolves a call. A node without one (a generic primitive) is descended
 *     into like any other.
 *   - An associated call (`EX_ASSOC`) counts as well: an associated function such as
 *     `T::make` is declared without `self` but takes a home arena like any other. */
/* **One question, one name.** The question travels as a predicate with a name, never as a bare
 * `bool` switch that changes what the predicate below *meant* ("does the callee reach a home arena?"
 * versus "does it take one itself?"). A bare switch is precisely the shape that lets the two sides
 * of a convention drift apart: the definition asks one, the call site the other, and nothing in the
 * code says which is which. The two entry points below (`callsNeedsHome` / `callsUsesHome`) are the
 * only places that pick one. */
typedef struct { bool (*takesHomeArena)(const FuncDef *); } HomeQ;

/**
 * @brief Whether this function reaches, transitively, a callee that takes a home arena.
 *
 * The conservative question that decides whether this function body needs a home arena of its own.
 *
 * @param f  Function definition.
 * @return   True when needed; false when `f` is NULL.
 */
static bool reachesHomeArenaTaker(const FuncDef *f) { return f && anNeedsHome(f); }

/**
 * @brief Whether this node is a call shape that carries a callee.
 *
 * Three call shapes plus two operators: after checking passes, an operator has the same shape as a
 * method call (`planCallee` points at the selected operator method), so every walk that asks for
 * the callee must accept them.
 *
 * @param e  Expression.
 * @return   True when it is a call shape.
 */
bool exprCarriesCallee(const Expr *e) {
    if (!e) return false;
    /* `EX_NEW` carries a callee only when the context asked for a `@buffer` type: then the `new`
     * stands for that type's constructor (`withCap`), so the closures must see the edge -- without
     * it the enclosing function is not marked as pool-making and the generated C passes the wrong
     * zone. An ordinary `new` has no callee and this predicate stays false. */
    return e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC ||
           e->kind == EX_BIN || e->kind == EX_UN || e->kind == EX_NEW;
}

/**
 * @brief Whether this node can only be a call (no operators, no `new`).
 *
 * The narrower companion of `exprCarriesCallee`: for questions about a call whose callee is
 * **not resolved**, an operator with no callee is not "unknown" -- it is a builtin arithmetic
 * node with no hidden arguments at all. Treating it as an unresolved call marks the enclosing
 * function's effects incomplete, which turns into false rejections at every call of it.
 *
 * @param e  Expression; NULL allowed.
 * @return   True for `EX_CALL`, `EX_METHOD`, `EX_ASSOC`.
 */
bool exprIsCallKind(const Expr *e) {
    return e && (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC);
}

static bool needsHomeInStmt(void *ctx, Stmt *s);

/**
 * @brief Tell whether an expression calls a callee that satisfies the given predicate.
 *
 * The predicate comes from the caller: `reachesHomeArenaTaker` asks "can a callee that needs a home
 * arena be reached", `funcTakesHomeArena` asks "does the callee itself take a home arena".
 *
 * @param ctx  `HomeQ *`, carrying the callee predicate.
 * @param e    Expression.
 * @return     Callback convention: false to stop the walk when found, true otherwise.
 */
static bool needsHomeInExpr(void *ctx, Expr *e) {
    HomeQ *q = (HomeQ *)ctx;
    /* Operators ARE method calls (`planCallee` sits on `EX_BIN`/`EX_UN`), so this group must accept
 * them: otherwise a function that only reaches a home-arena-taking callee through an operator is not
 * marked `needsHome`, the definition does not emit `__extc_home`, while the call site still passes
 * the home arena ⇒ the generated C has an undeclared `__extc_a` / `__extc_home`. */
    if (exprCarriesCallee(e) && planCallee(e) && q->takesHomeArena(planCallee(e)))
        return false;                       /* found: stop the walk */
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement-side walk callback for `needsHomeInExpr`.
 *
 * @param ctx  `HomeQ *`, carrying the callee predicate.
 * @param s    Statement.
 * @return     Callback convention: false to stop the walk when found, true otherwise.
 */
static bool needsHomeInStmt(void *ctx, Stmt *s) {
    AstVisit v = { needsHomeInExpr, needsHomeInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Tell whether a statement has a call that satisfies the given `takesHomeArena` predicate.
 *
 * Only valid after calls have been resolved: the walk reads `planCallee(e)`, which the checker fills
 * in when it resolves a call.
 *
 * @param s               Statement; NULL returns false.
 * @param takesHomeArena  Callee predicate.
 * @return                True when found.
 */
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
 * bounds and `EX_DYN`'s payload went missing from all of them.
 *
 * The comparison is on the *generated* name, so a local that shadows the parameter (which gets
 * `__2` appended) cannot be mistaken for it.
 *
 * A callback returns false to stop the walk; "found" is carried by that early exit. */
typedef struct { const char *cname; } CnameCtx;

static bool cnameInStmt(void *ctx, Stmt *s);

/**
 * @brief Tell whether an expression references the binding whose generated name is `cname`.
 *
 * @param ctx  `CnameCtx *`, carrying the generated name to compare.
 * @param e    Expression.
 * @return     Callback convention: false to stop the walk on a hit, true otherwise.
 */
static bool cnameInExpr(void *ctx, Expr *e) {
    CnameCtx *c = (CnameCtx *)ctx;
    if (e->kind == EX_IDENT)
        return !(planCName(e) && strcmp(planCName(e), c->cname) == 0);
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement-side walk callback for `cnameInExpr`.
 *
 * @param ctx  `CnameCtx *`, carrying the generated name to compare.
 * @param s    Statement.
 * @return     Callback convention: false to stop the walk on a hit, true otherwise.
 */
static bool cnameInStmt(void *ctx, Stmt *s) {
    AstVisit v = { cnameInExpr, cnameInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Tell whether a statement reads the binding whose generated name is `cname`.
 *
 * Used for one question only: whether a parameter is read at all. The comparison is on the
 * **generated** name, so a shadowing local of the same source name (whose generated name carries the
 * `__2` suffix) is not mistaken for the parameter.
 *
 * @param s      Statement; NULL returns false.
 * @param cname  Generated C name.
 * @return       True when it is read.
 */
static bool stmtUsesCname(Stmt *s, const char *cname) {
    if (!s) return false;
    CnameCtx c = { cname };
    return !cnameInStmt(&c, s);
}

/**
 * @brief Whether a statement has a call that reaches, transitively, a callee taking a home arena.
 *
 * @param body  Statement.
 * @return      True when there is one.
 */
static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body, reachesHomeArenaTaker); }

/**
 * @brief The exact question: whether this statement **directly** calls a function that takes a home
 *        arena.
 *
 * Uses the shared accessor `funcTakesHomeArena`, the same spelling codegen asks.
 *
 * @param body  Statement.
 * @return      True when there is one.
 */
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
 * This rule lives here, with the per-node criterion in `bodyReaches`, so changing it is a
 * change in one place. What legitimately differs between callers is only **which**
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
 * criterion is `planCallee(e)`, which the checker filled in while the bodies were checked -- and the
 * pointer is here for one reason only: `EXTC_DBG_FX` counts the rounds this driver runs. */
typedef struct { Module *m; TypeTable *tt; Checker *c; } CloseCtx;

/**
 * @brief Read a function's flag for the given reach property.
 *
 * @param f  Function definition.
 * @param k  Property to read.
 * @return   Current value of that property.
 */
static bool getReach(const FuncDef *f, ReachKind k) {
    switch (k) {
    case REACH_HOME:  return anNeedsHome(f);
    case REACH_POOL:  return planMakesPool(f);
    case REACH_ALLOC: return anAllocState(f) == 1;
    case REACH_USES_HOME: return planUsesHome(f);
    }
    return false;
}

/**
 * @brief Write a function's flag for the given reach property.
 *
 * @param f  Function definition.
 * @param k  Property to write.
 * @param v  New value.
 */
static void setReach(FuncDef *f, ReachKind k, bool v) {
    switch (k) {
    case REACH_HOME:  anSetNeedsHome(f, v); break;
    case REACH_POOL:  planSetMakesPool(f, v); break;
    case REACH_ALLOC: anSetAllocState(f, v ? 1 : 2); break;
    case REACH_USES_HOME: planSetUsesHome(f, v); break;
    }
}

/**
 * @brief Tell whether a function body **directly** reaches a callee with the given property.
 *
 * The transitive part is left to the fixed point in `closeReach`.
 *
 * @param f  Function definition.
 * @param k  Property to ask about.
 * @return   True when it reaches one directly.
 */
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
         * in the home arena counts, as does handing this function's home on to a callee. A
         * function whose home arena is never read is not marked, so it gets no unused hidden
         * parameter. */
        if (callsUsesHome(f->body)) return true;
        Vec as_ = anArenaSites(f);
        for (size_t i = 0; i < as_.len; i++)
            if (planArenaLevel(*(Expr **)vecAt(&as_, i)) == ARENA_HOME) return true;
        return false;
    }
    return false;
}

/**
 * @brief Re-run the same closure round until one round changes nothing.
 *
 * The rule is monotone and the lattice is finite, so the result is the **least** fixed point, not
 * merely some consistent solution.
 *
 * @param cx     Closure context.
 * @param k      Property to close.
 * @param round  Function that runs one round; it sets `*changed` when the round changed anything.
 */
static void closeReach(CloseCtx *cx, ReachKind k,
                       void (*round)(CloseCtx *cx, ReachKind k, bool *changed)) {
    for (bool changed = true; changed; ) {
        changed = false;
        round(cx, k, &changed);
        if (cx->c && cx->c->fxOn) cx->c->fxReachRounds++;
    }
}

/**
 * @brief Run one round of reach propagation over a module's free functions and each struct's
 *        methods.
 *
 * @param cx          Closure context.
 * @param k           Property to propagate.
 * @param changed     Set to true when this round changed anything.
 * @param skipExtern  Skip `extern!`, which has no body to walk.
 */
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
    /* Methods **declared** in an `impl` block need a round too: they live in `m->impls`, not in
 * `m->funcs` and not in the extended type's `methods` (the block may sit in another module).
 * Missing them shows up as: a method body calls a callee that needs a home arena but `needsHome`
 * does not propagate in ⇒ the definition side does not emit the `__extc_home` parameter, while the
 * call site still passes `&__extc_a[0]` for the block level ⇒ the generated C has an undeclared
 * `__extc_a`. */
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&im->methods, j);
            if (cx->c && cx->c->fxOn) cx->c->fxReachVisits++;
            if (getReach(f, k) || !f->body || (skipExtern && f->isExtern)) continue;
            if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
        }
    }
}

/**
 * @brief One `needsHome` round: walk the module's free functions and methods, skipping extern.
 *
 * @param cx       Closure context.
 * @param k        Property (`REACH_HOME`).
 * @param changed  Set to true when this round changed anything.
 */
static void roundNeedsHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);      /* an extern has no body to walk */
}

/**
 * @brief One `usesHome` round: walk the module's free functions and methods, skipping extern.
 *
 * @param cx       Closure context.
 * @param k        Property (`REACH_USES_HOME`).
 * @param changed  Set to true when this round changed anything.
 */
static void roundUsesHome(CloseCtx *cx, ReachKind k, bool *changed) {
    roundOverModule(cx, k, changed, true);
}

static void roundMakesPool(CloseCtx *cx, ReachKind k, bool *changed);   /* defined below */

/* ---- Does this code create a pool? (`FuncDef.makesPool`) ----
 *
 * The runtime primitive that creates one is `extc_pool_new`, and it is the *only* one:
 * `drop`/`reset`/`generation`/`live`/`capacity`/`zoneDepth` neither create a record nor
 * need a zone to exist. So the whole predicate rests on finding calls to that one name,
 * directly or through a callee that reaches it.
 *
 * Two callers ask two different questions with it:
 *   - the fixed-point closure below asks the transitive one ("can this function create a
 *     pool at all?"), and walks nested blocks, which each have a zone of their own;
 */
/**
 * @brief Count the `@overwrite` sites in a function body.
 *
 * The order must match codegen's `collectOwSites`: both walk the same tree in source order, so site
 * n maps to slot n.
 *
 * @param s  Function body.
 * @return   Number of `@overwrite` bindings.
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

/**
 * @brief Tell whether a function can reach itself through its callees.
 *
 * A function that can re-enter itself needs one `@overwrite` slot per activation: with a shared slot
 * a nested call overwrites the storage of the activation that is still running. The conservative
 * direction is "yes": a callee recorded as NULL answers true too, at the cost of one more slot per
 * activation; the walk stops after 64 hops and answers true, so a cycle cannot overflow the stack.
 *
 * @param c      Unused; the walk only reads the call graph recorded on `FuncDef`.
 * @param f      Function to test.
 * @param depth  Call-graph hops already taken, for the cycle guard.
 * @return       True when it can reach itself or the answer cannot be decided.
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

/**
 * @brief Fetch the name of a place expression's root identifier.
 *
 * Needed to decide whether `*head = cell`, `l.head = cell` or `l.buf[i] = cell` lands on the
 * parameter side. A reference-typed binding (`?ref node`) cannot be lent out, so the out
 * parameter convention wraps it in a struct.
 *
 * @param e  Place expression; NULL allowed.
 * @return   The root identifier's name; NULL when it is not a place (a literal, a call, ...).
 */
const char *placeRootName(Expr *e) {
    /* Question 4: the root's **syntactic name**, for diagnostics and simple callers. The mask is
 * FIELD|INDEX|DEREF and nothing is resolved -- so it also works on unresolved nodes, which is the key
 * difference from `placeRoot`. */
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF, 0);
    return leaf ? leaf->u.ident.name : NULL;
}
/**
 * @brief Tell whether the name `n` is a parameter of `f`.
 *
 * @param f  Function to search.
 * @param n  Name to look for.
 * @return   True when a parameter has that name.
 */
static bool isParamName(FuncDef *f, const char *n) {
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, n) == 0) return true;
    return false;
}

/**
 * @brief Tell whether a value expression may carry a reference.
 *
 * Do not split this into two mutually recursive functions: both questions (whether a type can carry
 * a reference, and whether an expression's result can carry one) are answered by this one recursive
 * function, and each expression kind recurses only in its own branch.
 *
 * @param v  Value expression; NULL answers false.
 * @return   True when it may carry a live reference. The answer over-approximates: an opaque shape
 *           answers true, which may ask for one more home arena but never misses a reference that
 *           really exists.
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

/**
 * @brief Tell whether a function body writes into a place reachable through its parameters.
 *
 * Together with `valueMayCarryRef` this decides whether the function needs a home arena: a write
 * through an out parameter (`fn push(head: mut ref ?ref node) { *head = cell }`) must allocate in
 * the caller's arena.
 *
 * @param c  Checker.
 * @param s  Statement to walk; may be NULL.
 * @param f  Function whose parameter names delimit the write targets.
 * @return   True when an assignment hits a place rooted at a parameter and the stored value may
 *           carry a reference.
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

/**
 * @brief Close and cache a function's effect summary.
 *
 * Three disciplines: lazy + memoized (`effState`); a cycle guard (`EFF_IN_PROGRESS` counts as a
 * cycle and is marked incomplete); an unresolved call (`effUnknown`) is also marked incomplete. Only
 * when this function returns true may a caller relax a check based on the summary.
 *
 * @param c  Checker.
 * @param f  Function whose summary is closed and cached; NULL is not allowed.
 * @return   True when the summary is complete; false on a cycle, an unresolved call or an incomplete
 *           callee, in which case the callee must be assumed to be able to store anything.
 */
bool computeEffectsTransitive(Checker *c, FuncDef *f) {
    if (!f) return false;
    /* An external declaration's summary is the signature that was collected for it
     * (`collectEffects` has already filled it in), so it counts as complete: we choose
     * to trust the declaration. Never let the absence of a body turn into an empty
     * summary.
     */
    if (f->isExtern) return true;
    if (anEffState(f) == EFF_DONE) { if (c->fxOn) c->fxEffCached++; return anEffComplete(f); }
    if (anEffState(f) == EFF_IN_PROGRESS) { anSetEffComplete(f, false); return false; }   /* a cycle => incomplete */
    anSetEffState(f, EFF_IN_PROGRESS);
    if (c->fxOn) c->fxEffCalls++;      /* one body entered (a memo hit returned above) */
    bool complete = !anEffUnknown(f);
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (c->fxOn) c->fxEffEdges++;  /* one callee edge merged */
        if (g == f) { complete = false; continue; }                   /* self-call: conservative */
        if (computeEffectsTransitive(c, g)) {                         /* merge the callee summary */
            anSetAddrMask(f, anAddrMask(f) | anAddrMask(g));
            anSetContMask(f, anContMask(f) | anContMask(g));
            anSetHomeAddrMask(f, anHomeAddrMask(f) | anHomeAddrMask(g));
            anSetHomeContMask(f, anHomeContMask(f) | anHomeContMask(g));
            anSetOtherMask(f, anOtherMask(f) | anOtherMask(g));
            anSetAddrFromLocal(f, anAddrFromLocal(f) || anAddrFromLocal(g));
        } else {
            complete = false;
        }
    }
    anSetEffComplete(f, complete);
    anSetEffState(f, EFF_DONE);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects-closed] %-20s complete=%d toParam[Addr=0x%llx Cont=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] other=0x%llx\n",
                FN(f), (int)anEffComplete(f), (unsigned long long)anAddrMask(f), (unsigned long long)anContMask(f),
                (unsigned long long)anHomeAddrMask(f), (unsigned long long)anHomeContMask(f), (unsigned long long)anOtherMask(f));
    return complete;
}

/** @brief Expression-side walk callback for `callMayGrow`. */
static bool growsInExpr(void *ctx, Expr *e);
/** @brief Statement-side walk callback for `callMayGrow`. */
static bool growsInStmt(void *ctx, Stmt *s);

/**
 * @brief Statement-side walk callback for `callMayGrow`.
 *
 * @param ctx  Unused.
 * @param s    Statement whose children are visited.
 * @return     Callback convention: false to stop the walk when a growing call is found.
 */
static bool growsInStmt(void *ctx, Stmt *s) {
    (void)ctx;
    AstVisit v = { growsInExpr, growsInStmt, NULL };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Expression-side walk callback for `callMayGrow`.
 *
 * @param ctx  Unused.
 * @param e    Expression to test and whose children are visited.
 * @return     Callback convention: false to stop the walk when found.
 */
static bool growsInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (e && e->kind == EX_METHOD && e->u.method.name && ckGrowMethodName(e->u.method.name))
        return false;
    AstVisit v = { growsInExpr, growsInStmt, NULL };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Whether a callee's body may move the storage of a container it was handed.
 *
 * A conservative scan: any call to a method whose name `ckGrowMethodName` lists says yes. The
 * callee's body is checked before its callers, so this sees resolved method targets.
 *
 * @param f  Callee; a body-less function cannot grow anything.
 * @return   true when the body contains a growing call at any depth.
 */
bool ckCalleeMayGrow(FuncDef *f) {
    if (!f || !f->body) return false;
    AstVisit v = { growsInExpr, growsInStmt, NULL };
    /* The walker returns false when a callback asked it to stop, which is what the
     * expression callback does on the first growing call it sees. */
    return !astWalkStmtChildren(f->body, &v);
}

/**
 * @brief Check that a call's reference arguments outlive the call, and set the arena level from it.
 *
 * Returns at once when the argument count does not match the parameter count. An extern callee is a
 * black box on the C side: the effects bits in its signature decide bit by bit which argument may be
 * stored, and a stored reference argument must live until level 0. A callee with a body is first
 * effect-closed, then the address flow and the content flow are judged separately, each criterion
 * deciding on its own whether to run; a promotable value is promoted first and only reported when it
 * cannot be promoted. A `mut ref` argument carries write permission, so its non-null proof is
 * invalidated here.
 *
 * @param c          Checker.
 * @param callee     Callee definition; NULL means the callee is unresolved and the conservative
 *                   treatment applies.
 * @param args       Argument expressions.
 * @param params     Callee parameter list, in the same order as `args`.
 * @param homeDepth  Arena level declared by the callee; less than or equal to 0 means no explicit
 *                   declaration.
 * @param line       Source line for diagnostics.
 * @param fname      Callee name, for diagnostics.
 */
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
        uint64_t stored = anAddrMask(callee) | anContMask(callee) | anOtherMask(callee);
        if (stored == 0) return;                 /* the signature says nothing is stored */
        /* A black box that may keep a pointer forever: the destination level is 0, so the
         * place-level report can compare this decision with the others. */
        LvlSite *lvlSite = lvlSiteNew(c, args, 0, line, fname);
        for (size_t j = 0; j < params->len && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || p->type->kind != TY_REF) continue;   /* a scalar has no lifetime, so
                                                                  * the witness records no opinion
                                                                  * for it */
            lvlSiteMark(lvlSite, j, LVL_CAND_SKIP);
            lvlSiteRole(lvlSite, j, LVL_ROLE_DEST);
            Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
            int d = placeRoot(c, place) ? slotDepth(c, place) : targetDepth(c, a);
            if (d == 0) { lvlSiteMark(lvlSite, j, LVL_CAND_ACCEPT); continue; } /* already outlives */
            lvlSiteMark(lvlSite, j, LVL_CAND_REJECT);
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
                (int)complete, callee ? (unsigned long long)anAddrMask(callee) : 0,
                callee ? (unsigned long long)anContMask(callee) : 0,
                callee ? (unsigned long long)anOtherMask(callee) : 0,
                callee ? (unsigned long long)anHomeAddrMask(callee) : 0,
                callee ? (unsigned long long)anHomeContMask(callee) : 0, homeDepth);
    /* The two checks decide independently whether to run; do not give them one shared
     * early return. One shared return makes an over-strict combination possible: a `Cont`
     * bit on `push` made the address-flow check run as well, and `examples/list-return`
     * was wrongly rejected. On the escape-analysis path, `homeDepth = -1 => h = 0` is too
     * strict for a local `mut ref` argument.
     */
    bool addrMaybe = !complete || !callee
                   || anAddrMask(callee) != 0 || anHomeAddrMask(callee) != 0
                   || anAddrFromLocal(callee) || anOtherMask(callee) != 0;
    bool contMaybe = !complete || !callee
                   || anContMask(callee) != 0 || anHomeContMask(callee) != 0
                   || anAddrMask(callee) != 0 || anHomeAddrMask(callee) != 0
                   || anOtherMask(callee) != 0;
    if (!addrMaybe && !contMaybe) return;
    /* The bound is the DESTINATION's own depth (below), falling back to the
     * caller's body depth. A `needsHome => 0` or `homeDepth < 0 => 0` guess collapses to
     * "must live in the home arena" for any argument living in any block, which rejects
     * safe programs: `homeDepth < 0 => 0` is too strict for a local `mut ref` argument. A
     * destination that is really home-promoted reports depth 0, so h = 0 still happens when
     * it must. */
    int h = (homeDepth > 0) ? homeDepth : (int)c->scopes.len;
    /* PRECISION: the destination is one of the `ref`-typed arguments. The callee may store a
     * value it received into any of them, so that value has to outlive every one of them:
     * take the shallowest (longest-lived) destination, never a deeper one than the caller's
     * own body. This replaces the old "the caller owns a home arena => h = 0" fallback,
     * which rejected every argument that lived in any block at all.
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
             * not one: counting it lets a depth-0 argument (a view of a global) collapse the
             * destination to "lives forever", which rejects the *callee's own table*.
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

    /* `h` is final: this is the place level every store made through this call has to fit
     * into. The verification pass (`check_lvl.c`) records the site here and the decision
     * at each point below, so `EXTC_DBG_LVL` can compare its own answer with this one. */
    LvlSite *lvlSite = lvlSiteNew(c, args, h, line, fname);

    for (size_t i = 0; addrMaybe && i < params->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* generic: defer */
        lvlSiteRole(lvlSite, i, LVL_ROLE_DEST);
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
        if (d == 0 || d <= h) { lvlSiteMark(lvlSite, i, LVL_CAND_ACCEPT); continue; } /* lives long enough */
        lvlSiteMark(lvlSite, i, LVL_CAND_REJECT);
        ckError(c, line,
                "The callee may store this reference into the arena it was given, so the"
                " argument must live at least that long. Move it to a shallower scope.",
                "argument %zu of `%s` points into a deeper scope (depth %d) than the arena"
                " this call may store it in (depth %d)", i + 1, fname, d, h);
    }

    /* A view of a container that can move its storage must not be handed to a function that
     * may keep it: the callee only sees a view, and nothing in its body can tell it that the
     * bytes will be freed by a later growth at the caller. Rejecting here is the call-site
     * half of the field-store rule; `tests/pin/view_arg_stored` is the shape. */
    {
        uint64_t stores = callee ? (anAddrMask(callee) | anContMask(callee) | anOtherMask(callee))
                                 : 0;
        for (size_t j = 0; stores && j < params->len && j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((stores >> j) & 1u)) continue;
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || !ttIsViewType(ttBase(tsub(c, p->type)))) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            const char *owner = NULL;
            if (!ckViewOfMovableStorage(c, a, &owner)) continue;
            /* A view of a container this function created is promotable: the fresh
             * allocation moves out to the destination's place, so passing it along is
             * the caller's normal escape, not a dangling view (`tests/ops/view_promote`).
             * Growth inside this function is already caught by the block-local rule. */
            {
                const char *vname = NULL;
                if (ckHoldsMovingViewOf(c, a, &vname) && vname) {
                    bool isParam = false;
                    if (c->curFunc)
                        for (size_t k = 0; k < c->curFunc->params.len; k++) {
                            Param *pp = *(Param **)vecAt(&c->curFunc->params, k);
                            if (pp->name && strcmp(pp->name, vname) == 0) { isParam = true; break; }
                        }
                    if (!isParam) continue;
                }
            }
            ckError(c, a->line,
                    "The callee may keep this view, and the container it points into can move its"
                    " storage when it grows. Keep the view local, or take a copy (`snapshot()`).",
                    "argument %zu of `%s` is a view of `%s`, whose storage moves when it grows",
                    j + 1, fname, owner);
        }
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
    if (contMaybe && (!complete || (callee && anOtherMask(callee) != 0))) {
        for (size_t j = 0; j < args->len; j++) {
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;                  /* generic: defer */
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue; /* no reference: always true */
            /* Record "it must outlive h" as a fact first, **regardless of the d computed right now**:
 * `d` is computed on the spot, while whether this value is a pool-constructing call
 * (`makesPool`) is only known after all function bodies have been checked -- here it is still 0,
 * so the depth query answers 0 and the promotion path would never be taken. The last-round replay
 * (`c->lvlFacts`) runs after the closure, when `makesPool` is already true, so `promoteInto` can
 * raise the pool-constructing site to level h. The replay ignores the return value, so recording
 * extra facts introduces no new errors (the values that cannot be promoted are still handled by the
 * depth criteria below). */
            recordLvlFact(c, a, h);
            lvlSiteRole(lvlSite, j, LVL_ROLE_VALUE);
            int d = targetDepth(c, a);
            /* A value assembled here can still carry another expression's borrowed bytes
             * (`holder { s = k.atDense(0) }`). Ask the whole value, not just its top: whichever
             * expression shape the borrow hides in, the answer to "may this be stored" has to be
             * the same. */
            {
                int bd = 0;
                if (borrowDepthAnywhere(c, a, &bd) && bd > d) d = bd;
            }
            if (d != 0 && d > h && promoteInto(c, a, h)) {
                /* Re-read the level after the promotion. For `dyn T(x)` the value's life is the
                 * **place** its handle lives in (`planZoneLevel`), which `promoteInto` just lowered
                 * to `h`; `targetDepth` answers from the payload's lexical site and would still say
                 * "the loop body", so the store would be rejected even though the slot really
                 * moved. Keep the smaller of the two: only the promotion path uses `planZoneLevel`
                 * here, so the ordinary arena answer is unchanged. */
                int zl = planZoneLevel(a);
                d = targetDepth(c, a);
                if (zl != 0 && zl < d) d = zl;
            }
            if (d == 0 || d <= h) { lvlSiteMark(lvlSite, j, LVL_CAND_ACCEPT); continue; }
            lvlSiteMark(lvlSite, j, LVL_CAND_REJECT);
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
         * Checking only the content-flow bits would accept `names.push(buf[..])` with
         * `buf` declared in a deeper block, which is a real dangling pointer. */
        uint64_t cont = anContMask(callee) | anHomeContMask(callee)
                      | anAddrMask(callee) | anHomeAddrMask(callee);
        for (size_t j = 0; j < args->len && j < EFF_MAX_PARAMS; j++) {
            if (!((cont >> j) & 1u)) continue;
            Param *pj = *(Param **)vecAt(params, j);
            if (pj->type && pj->type->kind == TY_REF) { lvlSiteMark(lvlSite, j, LVL_CAND_SKIP); continue; }   /* `ref` arg: checked above */
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) { lvlSiteMark(lvlSite, j, LVL_CAND_SKIP); continue; }
            /* A value whose type cannot carry a reference owns no bytes in any place, so the
             * store rule says nothing about it. Record no opinion here rather than `skip`: the
             * witness compares the two, and the level of an expression that merely computes a
             * scalar (`g.off.get(i) ?? 0`) is not a lifetime anyone can violate. */
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue;
            lvlSiteMark(lvlSite, j, LVL_CAND_SKIP);
            recordLvlFact(c, a, h);         /* same as above: record the fact first, promote on the last-round replay */
            lvlSiteRole(lvlSite, j, LVL_ROLE_VALUE);
            int d = targetDepth(c, a);
            /* A value that came out of a pool **by value** borrows that pool's bytes: the pool was
             * born where it was created, and a store cannot move it. `promoteInto` would lower the
             * call's own level and report success while the bytes stay behind (gap G1:
             * `k.atDense(0)` stored into an outer pool, then freed with the loop body). Read the
             * borrow fact of the initializer, keep the requirement, and skip promotion. */
            bool borrowedValue = false;
            {
                int bd = 0;
                if (borrowDepthAnywhere(c, a, &bd)) {
                    borrowedValue = true;
                    if (bd > d) d = bd;
                }
            }
            /* A view of storage that can move cannot be repaired by promoting the container
             * when the destination lives past this frame: the growth hands the old slab
             * back, so the view dies before the place that holds it, and no later function
             * can see the link. An implicit conversion is the same shape -- the argument is
             * the container, and the parameter only sees a view of it. Inside the frame the
             * growth stays visible to the block-local rule, so only `h == 0` is refused. */
            if (d != 0 && d > h && h == 0) {
                const char *mowner = NULL;
                if (ckViewOfMovableStorage(c, a, &mowner)) {
                    ckError(c, line,
                            "The callee stores this view into a container it was given, and the"
                            " container the view points into is one whose storage moves when it"
                            " grows. Keep the view local, or take a copy (`snapshot()`).",
                            "argument %zu of `%s` carries a reference into a deeper scope"
                            " (depth %d) than the place the callee may store it (depth %d);"
                            " promoting `%s` cannot keep the view alive across a growth",
                            j + 1, fname, d, h, mowner);
                    lvlSiteMark(lvlSite, j, LVL_CAND_REJECT);
                    continue;
                }
            }
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
             * pool born at the destination's place. */
            if (d != 0 && d > h && !borrowedValue && promoteInto(c, a, h)) {
                /* Re-read the level after the promotion: for `dyn T(x)` the value's life is the
                 * **place** the handle lives in (`planZoneLevel`), which the promotion just lowered
                 * to `h`. `targetDepth` answers from the payload's lexical site, so it would still
                 * report the loop body and the store would be rejected after the slot really
                 * moved. Only the promoted path consults `planZoneLevel`, so nothing else changes. */
                int zl = planZoneLevel(a);
                d = targetDepth(c, a);
                if (zl != 0 && zl < d) d = zl;
            }
            /* A by-value call result that borrows storage (`k.atDense(i)` handing back a
             * `string`) does not create data: the bytes stay in the pool the receiver names, and
             * a store cannot move that pool. Whatever level the paths above arrived at, the
             * borrow's own depth is the lifetime the destination must respect (gap G1). */
            {
                Expr *srcb = a;
                if (a->kind == EX_IDENT) {
                    Sym *syb = lookup(c, a->u.ident.name);
                    if (syb && syb->origin) srcb = syb->origin;
                }
                int bdb = 0;
                if (d <= h && borrowDepthOf(c, srcb, &bdb) && bdb > h) d = bdb;
            }
            if (d == 0 || d <= h) { lvlSiteMark(lvlSite, j, LVL_CAND_ACCEPT); continue; }
            lvlSiteMark(lvlSite, j, LVL_CAND_REJECT);
            ckError(c, line,
                    "The callee stores what this value points at into a container it was"
                    " given, so that data must live at least as long as that container.",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d)", j + 1, fname, d, h);
        }
    }

    /* ---- : a `mut ref` argument is write access, so the proofs about it are stale --------
     * The callee may rewrite what the place holds, so every non-null proof about that binding --
     * and about a path through it -- stops holding: `if s.top != null { s.pop() ... s.top.value }`
     * dereferenced a NULL `s.top`, because `pop` had set it and the proof was still in effect
     * (ASan SEGV). This is the write-point invalidation of the proof stack, the same
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
 * A store has two halves: the **destination** has to learn that something was written into it, or
 * the caller's binding goes on looking like a record with no live references.
 * `fn stash(out: mut ref box, v: ?ref node) { out.p = v }` called as `stash(ref b, ref n)` and then
 * `return b` would be accepted, and the generated program would read a local of the returning frame
 * after it died (ASan heap-use-after-free).
 *
 * The summary says *which argument* was stored, never into which field, so the write goes to the
 * binding's whole-object slot (`field == NULL`). The destination is normally **addressed** here --
 * `ref b` is what takes its address -- which is not a reason to skip it: the depth update is a weak
 * max, and `promoteFieldsAt` promotes for an addressed root while refusing to lower it. No
 * `recordStore` here: it constrains the level solver, which then rejects valid shapes. */
    if (callee && (addrMaybe || contMaybe)) {
        uint64_t stored = anAddrMask(callee) | anHomeAddrMask(callee)
                        | anContMask(callee) | anHomeContMask(callee);
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
                    /* No `otherSrc` for `dsym` here: only *which argument* was stored is known, not which field it
 * went into; the recorded source would have no matching destination requirement (`otherMinReq`
 * would stop at 0 = must outlive this frame), so `promoteInto` would fail when `at > 0` and
 * reject a normal shape. Only `callSrc` is published here (it carries its own depth), and
 * promotion follows it. */
                    Expr *saveCsv = c->curStoreVal;
                    c->curStoreVal = NULL;   /* update the depth only; do not record the source along the way */
                    noteFieldDepthWrite(c, dsym, NULL, dj);
                    c->curStoreVal = saveCsv;
                }
                /* The dedicated slot. No `recordLvlFact` here: constraining the level
                 * for *every* `mut ref` call was the first suspect for the three false rejections the
                 * full version produced (`tests/pool` rt_nest/rt_rett, `tests/stl` custom). */
                /* Publish the dedicated slot when the level is known, **or** when the source is a
                 * promotable fresh allocation -- those are the shapes whose site can really be moved,
                 * which is what `return b` needs (`var n: mut ref node = new node`).
                 * An unknown level whose source cannot be promoted gets no slot: publishing it added
                 * promotion pressure and nothing else, and that is what rejected `tests/pool`'s
                 * rt_nest/rt_rett and `tests/stl`'s custom when the publication was unconditional. */
                Sym *ss = placeRoot(c, splace);
                bool promotable = ss && ss->origin && exprIsFresh(ss->origin);
                /* `callSrc` also stores the **flattened root**: when a container element is taken out in another
 * scope and promoted, the intermediate binding (`p`) is long out of scope and `lookup` cannot find
 * it, so the site cannot be promoted. */
                Expr *csrc = splace;
                if (splace && splace->kind == EX_IDENT) {
                    Expr *r = originOf(c, splace, 0);
                    if (r) csrc = r;
                }
                if (dj != 0 || promotable) {
                    if (!dsym->callSrc) {
                        dsym->callSrc = csrc;
                        /* The threshold starts from the source's current depth, not 0 ("must outlive this frame"): the
 * source already lives at least until `dj`, and `promoteFieldsAt` only tightens from there.
 * Starting from 0 would make `prev = n` after `link(prev, n)` promote a site that never needs to
 * reach the home level, answer false, and reject a normal shape. */
                        dsym->callMinReq = dj;
                    }
                    if (dj > dsym->callSrcDepth) dsym->callSrcDepth = dj;
                }
            }
        }
    }

}

/**
 * @brief Pick the arena for a call from whether its result escapes the current block.
 *
 * A result stored in the current block uses the current block's arena, reclaimed every round with a
 * constant peak; a result stored further out, or returned, uses this function's home arena; the
 * argument position is deliberately left unmarked so it falls back to the home arena. `homeDepth`
 * and `arenaArg` must be updated together: codegen only reads `arenaArg`.
 *
 * @param c   Checker (scope stack).
 * @param v   Call expression whose result is stored; other shapes are ignored.
 * @param at  Scope depth of the place receiving the result; 0 means the result is returned and must
 *            outlive this frame.
 */
void markCallHomeIfEscaping(Checker *c, Expr *v, int at) {
    if (!v) return;
    /* The destination depth of a value reaches the call that builds it through every wrapper that
     * hands the value on unchanged: `a ?? b` stores whichever side it takes, `x?` stores `x`'s value,
     * `T(x)` is `x`, and a literal stores its parts in the value it becomes. Walking them matters
     * inside a loop -- `buf b = make() ?? dflt` otherwise keeps one allocation per round, because the
     * call is handed the function's home arena, which is only released when the function returns. */
    switch (v->kind) {
    case EX_COALESCE:
        markCallHomeIfEscaping(c, v->u.coalesce.main, at);
        markCallHomeIfEscaping(c, v->u.coalesce.fallback, at);
        return;
    case EX_TRY:
        markCallHomeIfEscaping(c, v->u.try_.operand, at);
        return;
    case EX_CONV:
        markCallHomeIfEscaping(c, v->u.conv.operand, at);
        return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < v->u.lit.inits.len; i++)
            markCallHomeIfEscaping(c, (*(FieldInit **)vecAt(&v->u.lit.inits, i))->value, at);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < v->u.enumval.args.len; i++)
            markCallHomeIfEscaping(c, *(Expr **)vecAt(&v->u.enumval.args, i), at);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < v->u.arraylit.elems.len; i++)
            markCallHomeIfEscaping(c, *(Expr **)vecAt(&v->u.arraylit.elems, i), at);
        return;
    default:
        break;
    }
    if (!exprCarriesCallee(v) || !planCallee(v)) return;

    /* -1: the value is handed out, so the call must use the caller's home arena.
     * `scopes.len`: the value is stored in the block being checked. */
    const int depth = (at < (int)c->scopes.len) ? -1 : (int)c->scopes.len;

    /* Only a plain call and a method call answer `needsHome` this early. An associated function
     * (`vec<u8>::withCap(4)`) waits for the final settlement: `needsHome` is a whole-program answer
     * that can already read "yes" while the body is being checked, and answering "block" here would
     * release the callee's blocks at block exit while the body still hands the value out through a
     * later `return` -- the returned storage would dangle. */
    if (exprCarriesCallee(v) && planCallee(v) && anNeedsHome(planCallee(v))) {
        anSetHomeDepth(v, depth);
        setCallArenaArg(c, v);      /* keep `homeDepth` and `arenaArg` in step */
        return;
    }
    if (depth == -1) {
        /* Handed out after all: a site recorded for a block has to go back to the home arena. */
        if (anHomeDepth(v) >= 2) anSetHomeDepth(v, -1);
        return;
    }
    /* Nothing to gain at body level: a value stored there lives as long as the home arena, and home
     * is the safe answer for a value a later statement hands out. A deeper block is released when it
     * exits -- which is the case worth fixing, since a loop body is exactly such a block. */
    if (depth < 2) return;
    const int cur = anHomeDepth(v);
    if (cur == 0 || depth < cur) anSetHomeDepth(v, depth);   /* lower only: never shorten a life */
    /* Stay pending: the final settlement asks the closed question `usesHome` and also sees an escape
     * (`anHomeDepth == -1`) recorded after this point, which `setCallArenaArg` would have frozen. */
    if (!v->arenaArgPending) {
        v->arenaArgPending = true;
        *(Expr **)vecPush(&c->curArenaSites) = v;
    }
}

/**
 * @brief Tell whether a callee can create a pool (every read site goes through here).
 *
 * A generic method's template and instance are two `FuncDef`s; the closure only reaches the
 * template, while at a call site `e->func` may be the template. A `@poolObject` declaration wins
 * over inference; a struct's `makesPoolAny` only relaxes for associated functions (the `new` /
 * `withCap` / `withParent` family), and other library methods do not join that family, so a value
 * that cannot be promoted is not misjudged as a site.
 *
 * @param f  Callee; NULL returns false.
 * @return   True when it may create a pool.
 */
bool calleeMakesPool(FuncDef *f) {
    if (!f) return false;
    /* **Declaration wins**: `@poolObject` marks a struct as owning a pool. It is more reliable than
 * inference: looking for a `pid` field is magic, and looking at whether methods create pools has to
 * wait for the closure. */
    if (f->owner && f->owner->poolObject) return true;
    if (planMakesPool(f)) return true;
    /* The runtime call **itself** is a pool site: `extc_pool_new` (extern, no body ⇒ the closure
 * cannot mark it, so it is recognized by name, the same name codegen uses when rewriting
 * `extc_pool_new` to `_at`).
 *
 * Without this case: when a user writes a pool-backed container **in their own file**, the
 * `extc_pool_new` in the constructor cannot be promoted ⇒ the pool goes into the constructor's own
 * frame ⇒ reclaimed on return ⇒ segfault. The library's containers lack this flaw only because
 * library functions are "transparent about place"; a user's file is not the library, so that hidden
 * difference shows. */
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return anIsAssoc(f) && f->owner && anMakesPoolAny(f->owner);
}

/* Substitution context for the instance-closure round. */
static TypeTable *instResTT = NULL;
static Vec *instResParams = NULL;
static Vec *instResTargs = NULL;
/**
 * @brief Re-resolve a method call site under the instance-closure substitution context.
 *
 * @param e  Call expression.
 * @return   The method definition found after substitution; NULL when there is no instance context
 *           or the expression is not a method call with a receiver.
 */
static FuncDef *resolveOnInstance(Expr *e) {
    if (!instResTT || !instResParams || !instResTargs) return NULL;
    if (!e || e->kind != EX_METHOD || !e->u.method.recv) return NULL;
    Type *rt = ttSubstitute(instResTT, e->u.method.recv->type, instResParams, instResTargs);
    return findMethod(ttBase(rt), e->u.method.name);
}


static bool exprMakesPool(Expr *e, bool descendBlocks);   /* defined with the pool questions at the end */

/**
 * @brief Fix the zone level of a pool-constructing call site.
 *
 * A library function (non-empty `modName`) is transparent about place: it has no place of its own,
 * so the choice goes to the caller (`ZONE_HOME`); otherwise the current block is the default. Only
 * lowers, never raises: the same node may be checked more than once, and promotion has already made
 * this level smaller; overwriting it from the current block would erase that promotion (smaller
 * means longer-lived).
 *
 * @param c  Checker (scope stack).
 * @param e  Call expression; NULL is ignored.
 */
void setCallZoneArg(Checker *c, Expr *e) {
    if (!e) return;
    int lvl = (c->curFunc && c->curFunc->modName && *c->curFunc->modName)
                  ? ZONE_HOME : (int)c->scopes.len;
    /* **Only lower, never raise**: the same node may be checked more than once (generic instances,
 * the last-round re-walk of that family), and promotion has already made this level smaller;
 * overwriting it from the current block would erase the promotion. Smaller means longer-lived, so
 * keep the smaller value. */
    if (planZoneLevel(e) == 0 || lvl < planZoneLevel(e)) planSetZoneLevel(e, lvl);
}

/**
 * @brief Resolve `homeDepth` into the arena actually passed at the call (`Expr.arenaArg`).
 *
 * The decision is made only here; codegen just prints it, and must not fall back to the
 * per-function home flag. When no `mut ref` argument gives a reason, the site is recorded with the
 * current block and `arenaArgPending` is set, to be settled by the last round.
 *
 * @param c  Checker (scope stack).
 * @param e  Call expression with `homeDepth` already set; NULL is ignored.
 */
void setCallArenaArg(Checker *c, Expr *e) {
    if (!e) return;
    if (anHomeDepth(e) == -1) {                 /* destination at the home level: my home arena */
        planSetArenaArg(e, ARENA_HOME);
        e->arenaArgPending = false;
    } else if (anHomeDepth(e) >= 1) {           /* an explicit block level: use that arena */
        planSetArenaArg(e, anHomeDepth(e));
        e->arenaArgPending = false;
    } else {
        /* No `mut ref` argument gave a reason, so the fallback applies: pass my home
         * arena if I have one, otherwise the current block.
         *
         * Whether I have a home arena is decided by the transitive closure of
         * `needsHome`, which is only known after every function body has been checked.
         * The site is therefore recorded as the current block with `arenaArgPending`
         * set, and the final pass settles it (see `curArenaSites`).
         *
         * The decision lives here so it is made in exactly one place: codegen reads
         * `arenaArg` and decides nothing on its own. */
        planSetArenaArg(e, (int)c->scopes.len);
        e->arenaArgPending = true;
        *(Expr **)vecPush(&c->curArenaSites) = e;   /* the final pass comes back to this site */
    }
}

/**
 * @brief Read the arena depth to pass at a call from its `mut ref` arguments.
 *
 * The callee's allocations must outlive anything it can store, so the shallowest `mut ref`
 * argument's object decides the arena. When the argument is a parameter of this function or a
 * global it already outlives this frame, so the caller's home arena is passed instead.
 *
 * @param c         Checker.
 * @param args      Argument expressions, in call order.
 * @param params    Callee's declared parameters, in the same order.
 * @param callNode  Call expression; recorded when the answer depends on escape analysis; may be
 *                  NULL (then it is not recorded).
 * @return          Arena depth to pass; 0 when there is no `mut ref` argument, -1 when the caller's
 *                  home arena must be passed.
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


/**
 * @brief Tell whether a vector of names contains a name.
 *
 * @param v  Vector of `const char *`.
 * @param n  Name to look for.
 * @return   True when any element is equal to it.
 */
static bool vecHasName(Vec *v, const char *n) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), n) == 0) return true;
    return false;
}
/**
 * @brief Tell whether an expression is a block carved out of a pool plate.
 *
 * `poolSlice` / `poolSliceRaw` take a new block, `poolResize` / `poolResizeRaw` adjust an existing
 * one; the memory belongs to the pool the container owns, and no reference of the caller is
 * published. The SSO `string`'s `grow` depends on this exemption, or the whole `push`/`append` chain
 * would carry `otherMask`.
 *
 * @param e  Expression; may be NULL.
 * @return   True when it is a pool plate block.
 */
static bool exprIsPoolBlock(Expr *e) {
    if (!e || e->kind != EX_GENCALL || !e->u.gencall.name) return false;
    const char *n = e->u.gencall.name;
    return strcmp(n, "poolSlice")     == 0 || strcmp(n, "poolSliceRaw") == 0 ||
           strcmp(n, "poolResize")    == 0 || strcmp(n, "poolResizeRaw") == 0;
}

/**
 * @brief Tell whether an initial value is freshly allocated in this frame.
 *
 * `new` counts, a pool plate block counts (see `exprIsPoolBlock`), and a struct literal whose every
 * field is itself fresh or scalar counts too.
 *
 * @param e  Initializer expression; may be NULL.
 * @return   True when it is a fresh allocation, or a literal built only from such values.
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
/**
 * @brief Record the innermost loop and the block level of each allocation line.
 *
 * The level must be the **loop body**'s level, not the block the `new` happens to be written in: an
 * allocation is reclaimed every iteration only when it lands in the loop body's arena or deeper; a
 * shallower one survives across iterations and grows with them. The loop body's level is the
 * smallest `lexicalLevel` of the sites in that loop, because nested blocks only report deeper
 * values.
 *
 * @param s         Statement to walk.
 * @param loopId    Current loop id; 0 means not inside a loop, and nothing is recorded.
 * @param nextLoop  Next free loop id.
 * @param sites     Output: `(line, loopId, lexicalLevel)` triples are appended.
 */
static void collectLoopSites(Stmt *s, int loopId, int *nextLoop, Vec *sites) {
    if (!s) return;
    switch (s->kind) {
    case ST_DROP:
        /* The target is a place, not an expression that can allocate. */
        return;
    case ST_TRAP:
        if (loopId && exprHasNew(s->u.trap_.msg)) {
            *(int *)vecPush(sites) = s->u.trap_.msg->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.trap_.msg);
        }
        return;
    case ST_YIELD:
        if (loopId && exprHasNew(s->u.yield_.value)) {
            *(int *)vecPush(sites) = s->u.yield_.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.yield_.value);
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
            *(int *)vecPush(sites) = anLexicalLevel(s->u.var.init);
        }
        return;
    case ST_ASSIGN:
        if (loopId && exprHasNew(s->u.assign.value)) {
            *(int *)vecPush(sites) = s->u.assign.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.assign.value);
        }
        return;
    case ST_EXPR:
        if (loopId && exprHasNew(s->u.expr.expr)) {
            *(int *)vecPush(sites) = s->u.expr.expr->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.expr.expr);
        }
        return;
    case ST_RETURN:
        if (loopId && exprHasNew(s->u.ret.value)) {
            *(int *)vecPush(sites) = s->u.ret.value->line;
            *(int *)vecPush(sites) = loopId;
            *(int *)vecPush(sites) = anLexicalLevel(s->u.ret.value);
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

/**
 * @brief Fetch the id of the loop containing a line.
 *
 * @param sites  Table of `(line, loopId, lexicalLevel)` triples.
 * @param line   Source line number.
 * @return       Id of the loop containing that line; 0 when there is none.
 */
static int loopIdOf(const Vec *sites, int line) {
    for (size_t i = 0; i + 2 < sites->len; i += 3)
        if (*(int *)vecAt((Vec *)sites, i) == line) return *(int *)vecAt((Vec *)sites, i + 1);
    return 0;
}

/**
 * @brief Print the `--explain-memory` report: one line per allocation site, with the numbers used
 *        for the decision.
 *
 * The levels are the same numbers the checker decided on; stating them is the point of the report.
 * Sites are listed by the level they landed in, the shallowest level they were ever published to,
 * and whether they sit in a loop. `minAt == 0` means the site was stored into something that
 * outlives this frame, the only conclusion the numbers alone support; the level a site lands in and
 * the block release points are decided by different rules, so the report does not conclude anything
 * about growth.
 *
 * @param c    Checker.
 * @param all  All allocation sites.
 */
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
        if (!anArenaSites(f).len) continue;
        Vec sites;
        vecInit(&sites, c->arena, sizeof(int));
        int nextLoop = 1;
        collectLoopSites(f->body, 0, &nextLoop, &sites);
        fprintf(stderr, "\n  %s\n", f->name ? f->name : "?");
        Vec as_ = anArenaSites(f);
        for (size_t j = 0; j < as_.len; j++) {
            Expr *site = *(Expr **)vecAt(&as_, j);
            if (!site) continue;
            nSite++;
            bool inLoop = loopIdOf(&sites, site->line) != 0;
            if (inLoop) nLoop++;
            fprintf(stderr, "    line %-5d level %-3d lexical %-3d  %s\n",
                    site->line, planArenaLevel(site), anLexicalLevel(site),
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
     * printed here by a predicate over them: such a predicate either misses a shape that
     * really grows or flags one that stays constant. The reliable form is to record, at code
     * generation time, which loop releases which arena; until then this report prints the
     * facts and claims nothing. */
    fprintf(stderr, "  (this report prints facts only: it does not yet say which sites grow)\n");
}

/**
 * @brief Drop one name from the fresh set.
 *
 * The set is a plain name vector and is always small (one function's locals), so compaction is
 * cheaper than any index.
 *
 * @param fresh  Name vector.
 * @param name   Name to drop.
 */
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

/**
 * @brief Collect this frame's local names whose initial value is a fresh allocation.
 *
 * Only the direct form `var x = new ...` counts; alias propagation is deliberately not done, and a
 * missing name only keeps a requirement that could be dropped, which is the conservative direction.
 *
 * @param a      Unused; only forwarded to recursive calls.
 * @param fresh  Output: collected names are appended here.
 * @param s      Statement to walk; blocks, if/else, while and match arms are followed.
 */
static void collectFreshLocals(Arena *a, Vec *fresh, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (exprIsFresh(s->u.var.init)) *(const char **)vecPush(fresh) = s->u.var.name;
        return;
    case ST_ASSIGN: {
        /* A name stops being fresh the moment an assignment gives it a value that is not itself
         * fresh: after `n = id(target)`, `n` points at whatever `id` returned, not at the `new
         * i32` its declaration made. Keeping
         * the name made `classifyStoredValue` answer "freshly allocated, so trivially safe" for a
         * value that now points into the caller's frame, so `stash`'s summary recorded nothing,
         * `effComplete` stayed true, and the whole call-site lifetime check was skipped -- a real
         * stack-use-after-scope. This is the write-point invalidation of the fresh
         * set; the fresh-value case keeps the name, so `p = new i32` does not lose it. */
        /* Only a binding that is **retargeted** stops being fresh. A field write such as
         * `n.value = v` writes *through* the binding: `n` still points at the object its
         * declaration made. Dropping it there removed the fresh answer for a value that really is
         * fresh, and the call-site check then rejected `stash(dest, ref deep, v)` -- the accepted
         * and ASan-clean program of -- which is how this narrowing was found. */
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


/**
 * @brief Tell whether a name is in the escape set E.
 *
 * The rule (over-approximating: one extra name costs memory, one missing name leaves a dangling
 * pointer): a name that appears in `return e` enters E; a name assigned to a binding already in E,
 * or stored into a container a parameter points at, enters E. E is closed by a fixed point.
 *
 * @param c  Checker; E lives in `c->escapees`.
 * @param n  Binding name.
 * @return   True when it is in E (its contents may leave this frame).
 */
bool isEscapeeName(Checker *c, const char *n) {
    if (!n) return false;
    for (size_t i = 0; i < c->escapees.len; i++)
        if (strcmp(*(const char **)vecAt(&c->escapees, i), n) == 0) return true;
    return false;
}
/**
 * @brief Add a name to the escape set.
 *
 * @param c  Checker.
 * @param n  Name to add; may be NULL.
 * @return   True when it was not in the set, which drives the fixed-point iteration.
 */
static bool addEscapee(Checker *c, const char *n) {
    if (!n || isEscapeeName(c, n)) return false;
    *(const char **)vecPush(&c->escapees) = n;
    return true;
}

/* Add every name appearing in the expression to E; true when a new name was added,
 * which is what the fixpoint loop uses. */
static bool markNamesInExpr(Checker *c, Expr *e);
/**
 * @brief Add the names appearing in a statement to the escape set.
 *
 * @param c  Checker.
 * @param f  Function whose escape set is being computed.
 * @param s  Statement; may be NULL.
 * @return   True when at least one new name was added.
 */
static bool markNamesInStmt(Checker *c, FuncDef *f, Stmt *s) {
    if (!s) return false;
    bool grew = false;
    switch (s->kind) {
    case ST_DROP:
        /* Naming the place in `drop x` does not make it escape. */
        return false;
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
        /* A local published through a call. An unconditional false here costs real
         * use-after-free bugs of this shape: a local published
         * through a call never enters E, so `callHomeDepth` takes the receiver for depth
         * 1, the caller's frame, and the arena argument becomes the current frame's arena,
         * the callee's own frame, instead of the home arena. Whatever the callee stored
         * into that container is then released when this call returns.
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
         * callee stored was released when the call returned -- , heap-use-after-free,
         * with a one-character difference from the accepted twin (`out.put(x)` without the `?`).
         * The `?` decides what happens *after* the call, not what the call can publish, so unwrap
         * it and ask the same question. */
        {
        Expr *se = s->u.expr.expr;
        while (se && se->kind == EX_TRY) se = se->u.try_.operand;
        if (!se) return false;
        switch (se->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: {
            FuncDef *cf = planCallee(se);
            if (!cf) return false;
            uint64_t pub = anAddrMask(cf) | anContMask(cf) | anOtherMask(cf)
                         | anHomeAddrMask(cf) | anHomeContMask(cf);
            if (anAddrFromLocal(cf)) pub |= 1u;
            /* An incomplete summary must never be read as proof that nothing is
             * stored: when the body contains a call that cannot be resolved,
             * `effComplete` stays false forever. So a callee with any `mut ref`
             * parameter, which is an entry point for a store, is treated as one that may
             * store. The direction is safe: marking more only costs memory. */
            if (!anEffComplete(cf)) {
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
/**
 * @brief Add the names appearing in an expression to the escape set.
 *
 * @param c  Checker.
 * @param e  Expression; may be NULL.
 * @return   True when at least one new name was added.
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
/**
 * @brief Compute a function body's escape set by fixed point.
 *
 * @param c          Checker; the set lives in `c->escapees`.
 * @param f          Function to analyze.
 * @param unionMode  Keep the names already collected (union) when true; clear first when false.
 */
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

/**
 * @brief Compute which bindings escape the function body (non-silent mode).
 *
 * @param c  Checker.
 * @param f  Function definition.
 */
static void computeEscapes(Checker *c, FuncDef *f) { computeEscapesMode(c, f, false); }

/**
 * @brief Grow the field table on demand (doubling, starting at 4) and return the grown table.
 *
 * The new array is allocated in the checker's arena: pointers inside `Sym` only ever grow, the old
 * array is left to the arena's bulk reclamation, and there is no free path to maintain. **No cap**:
 * a cap would change promotion decisions.
 *
 * @param c     Checker.
 * @param s     Binding to grow.
 * @param need  Minimum number of slots the table must hold.
 * @return      The grown table (callers usually ignore it; `s->fields` is already updated).
 */
static SymField *symFieldsGrow(Checker *c, Sym *s, int need) {
    if (s->fieldsCap >= need) return s->fields;
    int cap = s->fieldsCap ? s->fieldsCap : 4;
    while (cap < need) cap *= 2;
    /* **Must be zeroed**: in the new table, slots past `nfields` have no writer for
 * `.src`/`.srcSym`/`.minReq` (`fieldDepthEntry` only writes `.name`/`.depth`), while
 * `promoteFieldsAt` reads `.src` for every index < `nfields` and dereferences it as an `Expr *`.
 * With `arenaAlloc` that is an uninitialized heap pointer, and `promoteInto2` crashes when it
 * dereferences it. Zeroing turns a slot with no source into `src == NULL` (`promoteFieldsAt` just
 * continues) and `.minReq == 0` ("must outlive this frame", the conservative direction). */
    SymField *nf = (SymField *)arenaAllocZero(c->arena, (size_t)cap * sizeof *nf);
    for (int i = 0; i < s->nfields; i++) nf[i] = s->fields[i];
    s->fields = nf;
    s->fieldsCap = cap;
    return nf;
}


/**
 * @brief Find, or create on demand, the depth slot of one reference field.
 *
 * @param c      Checker.
 * @param s      Binding's reference field table.
 * @param field  Field name (interned in the AST, so it cannot dangle).
 * @param create Whether to create a slot when the field is not in the table yet.
 * @return       Pointer to the field's depth slot; NULL when `create` is false and there is no slot.
 *               The pointer is valid only until the next `symFieldsGrow` on this binding: growing
 *               the table copies it, so callers must use the slot before touching the table again.
 */
int *fieldDepthEntry(Checker *c, Sym *s, const char *field, bool create) {
    if (!s || !field) return NULL;
    for (int i = 0; i < s->nfields; i++)
        if (s->fields[i].name && strcmp(s->fields[i].name, field) == 0) return &s->fields[i].depth;
    if (!create) return NULL;
    symFieldsGrow(c, s, s->nfields + 1);
    s->fields[s->nfields].name  = field;          /* interned in the AST, so it cannot dangle */
    s->fields[s->nfields].depth = 0;
    return &s->fields[s->nfields++].depth;
}

/**
 * @brief Recompute a binding's effective depth after its field table changed.
 *
 * What is read is the deepest reference the binding may still carry, i.e. the maximum of the
 * fallback slot and all field slots; it is an upper bound and may only grow.
 *
 * @param s  Binding whose field table was just updated; NULL is allowed and ignored.
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
    if (anRefDepth(s) > m) m = anRefDepth(s);
    anSetRefDepth(s, m);
}

/**
 * @brief Remember the source expression last written into a field slot.
 *
 * Only shapes that can point at a site are recorded: `null` and literals are not, since recording
 * them would evict the real source. The comparison base is this write's depth `srcDepth`, not the
 * weakly updated `depth` (which takes the maximum, so a later `null` would look deeper and clear the
 * source).
 *
 * @param root   Binding whose field table is updated; may be NULL.
 * @param field  Field name; NULL for a whole-object or element write.
 * @param d2     Depth of this write.
 * @param src    Stored expression; may be NULL.
 */
static void noteFieldSrc(Checker *c, Sym *root, const char *field, int d2, Expr *src) {
    if (!root || !src) return;
    if (src->kind != EX_IDENT && src->kind != EX_FIELD && src->kind != EX_INDEX &&
        src->kind != EX_NEW   && src->kind != EX_GENCALL) return;
    /* Only a value that **can carry a reference** is worth recording. `n.val = m` (i64) reaches here
 * too (a root type's field may hold a reference); recording it as a source only makes promotion try
 * to promote an expression it cannot promote and reject a normal shape. */
    if (!src->type || !typeContainsRef(c->tt, tsub(c, src->type))) return;
    /* Store the source flattened to its root expression: an intermediate binding (`p`, `pt`, `q`) may
 * already be out of scope by promotion time, when `lookup` cannot find it and `promoteInto2` stops
 * at "no source" -- the site then stays at the lexical level while the returned chain already points
 * at it. A root expression (such as `new`) has no such problem.
 *
 * Do **not** record when the root is a zero value/literal or a binding without a source
 * (`p.next = saved`, whose root is `null`): such a source cannot be promoted, and recording it only
 * makes `promoteFieldsAt` answer false and reject a normal shape such as `saved = p`. The only use of
 * a source is promotion, so an unpromotable source has no value. */
    /* Store the source flattened to its root expression (the last-round replay has no scopes, so
 * `lookup` cannot find intermediate bindings), and keep that binding in the separate `srcSym` slot:
 * flattening loses its **field table** (`b.next = ca` is recorded on `b`), which cycle/chain
 * promotion must be able to reach. A `Sym` lives in the checker's arena and stays usable after its
 * scope ends. */
    Sym *srcSym = (src->kind == EX_IDENT) ? lookup(c, src->u.ident.name) : NULL;
    {
        Expr *r = originOf(c, src, 0);
        if (!r) return;
        if (r->kind == EX_NULL || r->kind == EX_INT || r->kind == EX_FLOAT ||
            r->kind == EX_BOOL || r->kind == EX_STR) return;
        if (r->kind == EX_IDENT) return;      /* cannot trace back to a site (a parameter, a binding with no source) */
        src = r;
    }
    if (!field) {
        /* A write with no single field -- whole-value copy, element write, a call through `mut ref` -- goes
 * into the `otherSrc` slot. The update rule is the same as for a field slot: compare this write's
 * `d2`, not the weakly updated `depth`, or a later `null` would look deeper and evict the real
 * source. */
        if (d2 >= root->otherSrcDepth) {
            /* On the first recorded source, set `otherMinReq` to this write's depth: 0 means "must outlive this
 * frame", a very strong assertion that, used as the default, makes every promotion try to promote a
 * site that does not need it and fail. Afterwards `promoteFieldsAt` only tightens it. */
            if (!root->otherSrc) root->otherMinReq = d2;
            root->otherSrc = src;
            root->otherSrcSym = srcSym;
            root->otherSrcDepth = d2;
        }
        return;
    }
    int n = -1;
    for (int i = 0; i < root->nfields; i++)
        if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) { n = i; break; }
    if (n < 0) {
        /* **Create** a slot when it does not exist. Writing only existing slots would leave two-level
 * projections such as `bx.mid.next = p` without a slot forever: declaring `{ mid: { next: null } }`
 * creates only `mid`, so nobody records the source and the return-value check cannot reach that
 * allocation site. */
        symFieldsGrow(c, root, root->nfields + 1);
        n = root->nfields++;
        root->fields[n].name     = field;   /* interned in the checker's arena, so it cannot dangle */
        root->fields[n].depth    = 0;
        root->fields[n].src      = NULL;
        root->fields[n].srcSym   = NULL;
        root->fields[n].srcDepth = 0;
        /* The threshold starts at this write's depth, not 0 (0 means "must outlive this frame", a very
 * strong assertion): a new slot's source now lives at least until `d2`, and afterwards
 * `promoteFieldsAt` only tightens it. Starting at 0 would make a normal promotion such as
 * `head = b` (whose `next` holds the previous head) promote a site that never needs to reach the
 * home level, answer false and reject it. */
        root->fields[n].minReq   = d2;
    } else if (!root->fields[n].src) {
        root->fields[n].minReq = d2;        /* first source recorded for this existing slot */
    }
    if (dbgOn("EXTC_DBG_FS"))
        fprintf(stderr, "[fs] %s.%s d2=%d oldSrcDepth=%d srckind=%d\n", root->name,
                field, d2, root->fields[n].srcDepth, (int)src->kind);
    if (d2 >= root->fields[n].srcDepth) {
        root->fields[n].src = src;
        root->fields[n].srcSym = srcSym;
        root->fields[n].srcDepth = d2;
    }
}

/**
 * @brief Record the depth of a write that reaches a binding.
 *
 * A reference-typed binding's `refDepth` (`n: mut ref node`) may only rise, never fall: lowering it
 * would overwrite "what I point at" with "what the pointee contains", and let a dangling pointer be
 * accepted. An ordinary struct binding may fall, which the strong field-slot update needs.
 *
 * @param c      Checker; `c->curStoreVal` is the stored expression.
 * @param root   Binding whose field table is updated; may be NULL.
 * @param field  Field name; NULL for a whole-object or element write.
 * @param d2     Depth of the stored value.
 */
void noteFieldDepthWrite(Checker *c, Sym *root, const char *field, int d2) {
    if (!root) return;
    noteFieldSrc(c, root, field, d2, c->curStoreVal);   /* the depth record is left untouched */
    /* A field write through a reference-typed binding (`n: mut ref node`) writes a
     * field of the pointee, and `refreshRootDepth` recomputes the root as the max over
     * the field slots, which replaces what I point at (depth 2) with what is stored
     * inside the object I point at (the field depth, 0). A later `keeper = n` was then
     * accepted and dangled (confirmed with ASan). The fix: for a reference-typed
     * binding, `refDepth` may only be moved toward the longer-lived value, never
     * lowered. A struct binding that is not a reference may still drop, and that is
     * exactly what the strong update is for. */
    bool isRefRoot = root->type && tsub(c, root->type)->kind == TY_REF;
    int  before    = anRefDepth(root);
    if (!field) {                                  /* whole-object assignment or element write */
        /* An element write invalidates no other element or field, so it must neither
         * clear the field table nor overwrite `otherDepth`. The old behavior cleared the
         * table and wrote `otherDepth = 0`, so `a[1] = x; a[0] = null` let a whole-value
         * escape through. */
        if (d2 > root->otherDepth) root->otherDepth = d2;
        refreshRootDepth(root);
        if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);   /* keep the pointee */
        return;
    }
    int *slot = fieldDepthEntry(c, root, field, true);
    if (!slot) {                                   /* full table: the fallback slot takes the max */
        if (d2 > root->otherDepth) root->otherDepth = d2;
    } else if (root->addressed) {                  /* address taken: aliases may write, take max */
        if (d2 > *slot) *slot = d2;
    } else if (root->fieldsComplete) {
        /* First step of the data-flow part: with a complete field table, the new value
         * replaces the old one in this slot, so the old value is not a property of
         * any slot of the container, and the root is recomputed skipping it. That is the
         * strong update. It is sound because the root record is an upper bound over all
         * slots: with a complete table, removing an edge that does not hold leaves an
         * upper bound over the rest. The depth is not shrunk out of thin air. */
        *slot = d2;
        int m = root->otherDepth;
        for (int i = 0; i < root->nfields; i++) {
            if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) continue;
            if (root->fields[i].depth > m) m = root->fields[i].depth;
        }
        anSetRefDepth(root, m);
        if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);   /* keep the pointee */
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
    if (isRefRoot && anRefDepth(root) < before) anSetRefDepth(root, before);
}

/**
 * @brief Record a whole-aggregate assignment while keeping per-field sources.
 *
 * Recording only `h.otherDepth = depth(t)` would lose `t`'s per-field allocation sites, and a later
 * safe `return h` would be rejected. Copied exactly when the source table is complete; falls back to
 * the conservative aggregate update when the destination is exposed or the source is incomplete.
 *
 * @param c      Checker.
 * @param dst    Assignment destination binding.
 * @param value  Assigned aggregate value.
 * @param d2     Depth of that value.
 */
void noteWholeValueDepthWrite(Checker *c, Sym *dst, Expr *value, int d2) {
    if (!dst) return;

    Sym *src = value && value->kind == EX_IDENT ? identBindOf(value) : NULL;
    if (!dst->addressed && src && src != dst && src->fieldsComplete) {
        symFieldsGrow(c, dst, src->nfields > 0 ? src->nfields : 1);
        dst->nfields = src->nfields;
        for (int i = 0; i < src->nfields; i++) dst->fields[i] = src->fields[i];
        dst->otherDepth = src->otherDepth;
        dst->fieldsComplete = true;

        int m = dst->otherDepth;
        for (int i = 0; i < dst->nfields; i++)
            if (dst->fields[i].depth > m) m = dst->fields[i].depth;
        anSetRefDepth(dst, m);
        return;
    }

    noteFieldDepthWrite(c, dst, NULL, d2);
}
/**
 * @brief Look up a parameter position by name.
 *
 * Parameter shadowing is a legal language feature, so matching by name can pick the wrong one; when
 * an expression is at hand, use `paramIndexOfExpr` (which compares symbols).
 *
 * @param f     Function to search; may be NULL.
 * @param name  Parameter name; may be NULL.
 * @return      Zero-based parameter index; -1 when it is not a parameter.
 */
static int paramIndexByName(FuncDef *f, const char *name) {
    if (!f || !name) return -1;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, name) == 0) return (int)i;
    return -1;
}

/**
 * @brief Fetch the binding an expression root resolves to, without a scope lookup.
 *
 * `placeRoot` answers the same question, but it goes through the checker's scopes, and scopes exist
 * only while a function body is being checked; effect summaries run after that. The resolver already
 * attaches a `Sym` to `EX_IDENT` nodes, so no scope is needed here.
 *
 * @param e  Expression.
 * @return   The root identifier's binding; NULL when it is not an identifier or is unresolved.
 */
static Sym *rootSymNoScope(Expr *e) {
    /* The **scope-free** version of question 3 (the summary pass runs after all function bodies are
 * checked, when no scope can be looked up -- the root cause). The mask includes `SIGN` and keeps the
 * 16-hop limit; that difference is now expressed in the mask. */
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_DEREF | EXTC_ROOT_SIGN, 16);
    return leaf ? identBindOf(leaf) : NULL;
}

/**
 * @brief Tell which parameter of `f` a value `e` comes from, by symbol rather than by spelling.
 *
 * The summary records "argument j was stored", and the call site checks only that argument, so the
 * answer has to survive shadowing; callers that have only a name use `paramIndexByName`.
 *
 * @param c  Checker.
 * @param f  Function.
 * @param e  Value expression.
 * @return   Parameter index; -1 when it does not come from a parameter.
 */
static int paramIndexOfExpr(Checker *c, FuncDef *f, Expr *e) {
    (void)c;
    if (!f || !e) return -1;
    Sym *root = rootSymNoScope(e);
    if (!root) return -1;
    EXTC_DBG_ASSERT(anParamSymCount(f) <= 64);         /* must equal the array the struct declares */
    for (int i = 0; i < anParamSymCount(f); i++)
        if ((Sym *)anParamSyms(f)[i] == root) return i;
    return -1;
}

/**
 * @brief Record the summary bits set by storing value `e` into an object `f` owns.
 *
 * An assignment through a `mut ref` parameter, or into an object freshly allocated in this frame,
 * may hand a pointer to the caller. The summary is recorded per parameter: whether the address came
 * from the argument (address flow) or a pointer read out of the argument's contents (content flow);
 * the call site only checks the bits that were set.
 *
 * @param c        Checker; the type judgement is skipped when NULL.
 * @param f        Function whose summary is being filled in.
 * @param e        Value being stored.
 * @param i        Index of the destination container parameter; -1 when the destination is not a
 *                 parameter of `f`.
 * @param intoHome True when the destination is an object freshly allocated in this frame, false
 *                 when it is a `mut ref` parameter.
 * @param fresh    Names allocated in this frame; values from them need no constraint at the call
 *                 site.
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
            if (intoHome) anSetHomeAddrMask(f, anHomeAddrMask(f) | (((uint64_t)1 << j)));
            else if (i >= 0) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << j)));
        } else {
            anSetAddrFromLocal(f, true);              /* local address: a different problem */
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
                    if (intoHome) anSetHomeAddrMask(f, anHomeAddrMask(f) | (((uint64_t)1 << j)));
                    else if (i >= 0) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << j)));
                } else {                          /* content flow: pointer read from a container */
                    if (intoHome) anSetHomeContMask(f, anHomeContMask(f) | (((uint64_t)1 << j)));
                    else if (i >= 0) anSetContMask(f, anContMask(f) | (((uint64_t)1 << j)));
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
     * of a caller may not live in any block at all. `failure(e)` and `some(v)` carry a value
     * and are classified as before. */
    if (e->kind == EX_ENUMVAL && e->u.enumval.args.len == 0) return;
    { const char *vr = placeRootName(e);          /* `l.head = n`: the source is a fresh local */
      if (vr && vecHasName(fresh, vr)) return; }
    if (c && e->type && !typeContainsRef(c->tt, e->type)) return;   /* scalar, trivially safe */
    if (i >= 0) anSetOtherMask(f, anOtherMask(f) | (((uint64_t)1 << i)));        /* not sure, so stay conservative */
}

static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e);

/**
 * @brief Record every "store" a statement may perform.
 *
 * The summary is per parameter: which parameter receives an address, which receives a pointer read
 * out of another parameter, and which one is unknown.
 *
 * @param c      Checker.
 * @param f      Function whose summary is being filled in.
 * @param s      Statement to walk; NULL allowed.
 * @param fresh  Names allocated in this frame; values from them are safe and not recorded.
 */
static void collectEffectsStmt(Checker *c, FuncDef *f, Stmt *s, Vec *fresh) {
    if (!s) return;
    switch (s->kind) {
    case ST_DROP:
        collectEffectsExpr(c, f, s->u.drop_.target);
        return;
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

/**
 * @brief Tell whether a function type can neither take nor hand out a reference.
 *
 * Only the signature is inspected: no parameter type contains a reference and the return type does
 * not either. When such a `fn` value is called indirectly, the caller has no reference that can flow
 * in through it and the callee has none that can flow out -- so that indirect call **cannot**
 * publish a caller's reference elsewhere, and the summary need not be marked incomplete for it (see
 * `collectEffectsExpr`).
 *
 * @param tt  Type table (interprets generic arguments).
 * @param t   Type; returns false when it is not `fn(...) -> ...` (conservative: an unknown shape does
 *            not get this exemption).
 * @return    True when the signature contains no reference at all.
 */
static bool fnTypeCarriesNoRef(TypeTable *tt, Type *t) {
    if (!t || t->kind != TY_FN) return false;
    for (size_t i = 0; i < t->params.len; i++)
        if (typeContainsRef(tt, *(Type **)vecAt(&t->params, i))) return false;
    if (t->ret && typeContainsRef(tt, t->ret)) return false;
    return true;
}

/**
 * @brief Is this call deferred because its subject is a type parameter?
 *
 * `k.hash()` inside `hashMap<K,V>` (`k: K`), `T == T`, or an indirect call through a `fn` value whose
 * signature mentions `T` cannot be resolved while the template is being checked: there is no struct
 * behind `T` yet. The instance round resolves the same call site once the type argument is known, so a
 * template summary must not be poisoned by it -- see the caller.
 *
 * @param e  Call expression.
 * @return   True when the subject (receiver, explicit type arguments or callee type) mentions a type
 *           parameter.
 */
static bool callSubjectMentionsParam(Expr *e) {
    if (!e) return false;
    if (e->kind == EX_METHOD && e->u.method.recv) return mentionsParam(e->u.method.recv->type);
    if (e->kind == EX_ASSOC) {
        for (size_t i = 0; i < e->u.assoc.targs.len; i++)
            if (mentionsParam(*(Type **)vecAt(&e->u.assoc.targs, i))) return true;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (mentionsParam(((Expr *)vecAt(&e->u.assoc.args, i))->type)) return true;
        return false;
    }
    if (e->kind == EX_CALL && e->u.call.callee) return mentionsParam(e->u.call.callee->type);
    return false;
}

/**
 * @brief Tell whether a call target is a builtin that provably retains nothing.
 *
 * These are dispatched by the checker or by the code generator and have no declaration to take a
 * summary from, so the effect walk has to recognize them by name. Every one of them is safe to
 * treat as storing nothing: the print family writes to the output buffer, trap does not return, and
 * the three view/size doors only re-type a pointer the caller already holds (the same promise
 * extc_viewOf makes in its "effects Addr=0 Cont=0" clause).
 *
 * The comparison is on the LAST component of a dollar-separated name, because a call reached
 * through a module is written mod$name. Matching the whole string missed every qualified call:
 * io$print inside io$println, heap$extc_viewOf inside buf::withCap. Marking those unresolved made
 * the enclosing summary incomplete, and an incomplete callee forces EVERY caller to prove that all
 * of its reference arguments outlive the callee place, so vector::from(a[..]) was rejected for a
 * local array that the base tier accepted.
 *
 * @param name  Callee name as written; may be module-qualified.
 * @return      True when the builtin provably retains nothing.
 */
static bool isRetainsNothingBuiltin(const char *name) {
    static const char *const BASE[] = {"print", "println", "flush", "trap",
                                       "extc_sizeOf", "extc_viewOf", "extc_viewOfT"};
    if (!name) return false;
    const char *last = strrchr(name, '$');
    last = last ? last + 1 : name;
    for (size_t i = 0; i < sizeof BASE / sizeof BASE[0]; i++)
        if (strcmp(last, BASE[i]) == 0) return true;
    return false;
}

/**
 * @brief Record the calls in an expression and the callees they reach.
 *
 * A call that cannot be resolved sets `anEffUnknown(f)`, and the summary stays incomplete forever;
 * no check may then be relaxed on its strength. The edges recorded are the function's call graph,
 * closed afterwards by `computeEffectsTransitive`.
 *
 * @param c  Checker.
 * @param f  Function whose callee table and summary are being filled in.
 * @param e  Expression to walk; NULL allowed.
 */
static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e) {
    if (!e) return;
    if (exprIsCallKind(e) && !planCallee(e)) {
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
         *     // `setSourceRgb` is signed `Addr=0 Cont=0`, yet `rgb`'s summary would come out
         *     // incomplete ⇒ its callers could not pass a frame-local pointer
         *     // ("points into a deeper scope (depth 1) than the arena this call may store it
         *     // it in").
         *
         * A `fn` value that did **not** come from a signed slot (a local copy, a parameter, an
         * unsigned slot) still has nothing to go on: the summary stays incomplete, conservatively.
         * Note this is the same trade the extern path makes: the clause is believed, and the whole
         * basis for believing it is that the author wrote it. */
        FieldDef *sfd = NULL;
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_FIELD)
            sfd = planField(e->u.call.callee);
        else if (e->kind == EX_METHOD && e->u.method.recv && e->u.method.recv->kind == EX_FIELD)
            sfd = planField(e->u.method.recv);
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
                    if ((addr >> j) & 1u) anSetAddrMask(f, anAddrMask(f) | (((uint64_t)1 << pi)));
                    if ((cont >> j) & 1u) anSetContMask(f, anContMask(f) | (((uint64_t)1 << pi)));
                } else {
                    /* It keeps something that does not trace back to a parameter of mine: say so
                     * in the one way the summary has (`otherMask` = "not attributable"), which the
                     * call site reads as the strictest case. */
                    anSetOtherMask(f, anOtherMask(f) | (1u));
                }
            }
        } else if (c->substParams && c->substArgs && c->substParams->len > 0
                   && e->kind == EX_METHOD && e->u.method.recv) {
            /* **Instance context**: the receiver's type is concrete now, so a method call that could
             * not be resolved on the template (`k.hash()` inside `hashMap<K,V>`, where `k: K`) resolves
             * here. Recording the edge is what lets this instance's summary come out complete; without
             * it the unknown set on the template survives and every caller of `remove`/`contains` --
             * which reach `find` -- pays the worst case, so it cannot pass a view argument at all.
             * Only the summary of the *current* collection is affected: the conservative template
             * answer (`tests/errors/generic_borrowed_store.extc` relies on it) stays as it is. */
            Type *rt = tsub(c, e->u.method.recv->type);
            FuncDef *md = rt ? findMethod(ttBase(rt), e->u.method.name) : NULL;
            if (md) {
                bool seen = false;
                for (size_t k = 0; k < f->callees.len; k++)
                    if (*(FuncDef **)vecAt(&f->callees, k) == md) { seen = true; break; }
                if (!seen) *(FuncDef **)vecPush(&f->callees) = md;
            } else {
                anSetEffUnknown(f, true);
            }
        } else if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT &&
                   e->u.call.callee->u.ident.name &&
                   isRetainsNothingBuiltin(e->u.call.callee->u.ident.name)) {
            /* These are builtins dispatched by name, so there is no declaration to take a summary
             * from; none of them retains a reference. Treating them as unknown made every function
             * that prints, or that builds a buf<T>, incomplete, and an incomplete summary forces the
             * caller to prove that *every* reference argument outlives the callee place: show(v: val)
             * (which matches and prints) could not be called with a value read out of a container
             * (tests/genmatrix/hetero_values), and vector::from(a[..]) was rejected for a local
             * array (tests/stl/from.extc) only because buf::withCap calls extc_viewOfT. */
        } else if (e->kind == EX_CALL && e->u.call.callee
                   && fnTypeCarriesNoRef(c->tt, tsub(c, e->u.call.callee->type))) {
            /* An indirect call through a `fn` value whose signature has neither reference parameters nor a
 * reference return: the caller cannot pass a reference in and the callee cannot hand one out, so it
 * cannot publish the caller's reference. The summary stays complete (otherwise every library
 * function taking a callback would drag its caller into the worst case: `apply(buf, inc)` would
 * report "effects could not be fully analyzed"). */
        } else {
            /* A call on a **type parameter** is deferred, not unknown: `k.hash()` inside
             * `hashMap<K,V>` cannot be resolved while the template is checked, but the instance round
             * below resolves the same call site with the type argument in place and records the real
             * edge. Poisoning the template summary here is permanent, and it is load-bearing in the
             * wrong direction: `hashMap::remove` reaches `find`, so every caller of `remove` (or
             * `contains`, or anything built on them) is told "effects could not be fully analyzed"
             * and cannot pass a view argument -- even though the instantiated call is perfectly
             * decidable. The template's own conservatism lives in the `mentionsParam` rules
             * (a `T` may carry a reference), which is where `tests/errors/generic_borrowed_store.extc`
             * gets its rejection from; it does not depend on this bit. */
            if (!callSubjectMentionsParam(e))
                anSetEffUnknown(f, true);   /* nothing to go on -> incomplete, conservatively */
        }
    }
    if (exprCarriesCallee(e) && planCallee(e)) {
        bool seen = false;
        for (size_t k = 0; k < f->callees.len; k++)
            if (*(FuncDef **)vecAt(&f->callees, k) == planCallee(e)) { seen = true; break; }
        if (!seen) *(FuncDef **)vecPush(&f->callees) = planCallee(e);
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
     * `point { x: f() }`. Letting them fall to `default` drops the call and makes the
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

/**
 * @brief Compute a function's effect summary: what it stores into the containers handed to it.
 *
 * Only the address flow `Addr(j)` and the content flow `Cont(j)` constrain arguments; `new`,
 * literals and scalars do not. An `extern!` declaration is summarized from its `effects` clause: with
 * no clause, every parameter below 32 is recorded as "stored" (the safe default).
 *
 * @param c  Checker.
 * @param f  Function to summarize.
 */
static void collectEffects(Checker *c, FuncDef *f) {
    /* An extern declaration has no body, so the summary comes from the signed clause or
    * is the worst case. Skipping this would leave the summary empty, which reads as
    * "stores nothing" and would accept a dangling pointer. */
    if (f->isExtern) {
        if (f->hasEffects) {
            anSetAddrMask(f, f->extAddrMask);
            anSetContMask(f, f->extContMask);
        } else {
            /* No clause means every parameter may be stored. That is nearly unusable, and it
            * is what a safe default should look like: sign the declaration to make it
            * usable. */
            uint64_t all = 0;
            for (size_t i = 0; i < f->params.len && i < EFF_MAX_PARAMS; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                if (!p->type) continue;
                /* Only a parameter that can carry a reference can be kept by C. A scalar
                 * (`extc_pool_new(parent: i64)`) has nothing to hand over, and marking it
                 * made every user of a container inherit "stores from parameter 0" through
                 * the closure -- the parameter numbers do not travel with the bits, so the
                 * bit landed on some other function's `self`. */
                if (p->type->kind == TY_REF || ttIsViewType(p->type)
                    || typeContainsRef(c->tt, p->type))
                    all |= ((uint64_t)1 << i);
            }
            anSetAddrMask(f, all);
            anSetContMask(f, all);
        }
        anSetEffComplete(f, true);      /* the declaration is authoritative, as a body would be */
        anSetOtherMask(f, 0);
        return;
    }
    /* `FuncDef` comes from `arenaAllocZero`, so `callees` has no arena yet; without an
     * explicit `vecInit`, `vecPush` would allocate through a NULL arena and crash. */
    if (!f->callees.arena) vecInit(&f->callees, c->arena, sizeof(FuncDef *));
    /* The escape set decides which arena the calls in this body pass, so it has to be
     * known before the body is walked. */
    computeEscapes(c, f);
    /* Do not write `c->escapeesFor = (int)(size_t)f;` here -- that truncates a pointer to `int` as the
 * "already computed" key, and the field would be write-only (a dead field plus pointer truncation).
 * If memoization is really needed later, use a `FuncDef *` or `Sym *` field, not an address packed
 * into an integer. */
    if (getenv("EXTC_DUMP_EFFECTS")) fprintf(stderr, "[escapes-for] %s\n", FN(f));
    Vec fresh; vecInit(&fresh, c->arena, sizeof(const char *));
    collectFreshLocals(c->arena, &fresh, f->body);
    anSetFreshCount(f, (unsigned)fresh.len);
    collectEffectsStmt(c, f, f->body, &fresh);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects] %-22s toParam[Addr=0x%llx Cont=0x%llx Other=0x%llx] toHome[Addr=0x%llx Cont=0x%llx] localAddr=%d fresh=%u callees=%zu\n",
                FN(f), (unsigned long long)anAddrMask(f), (unsigned long long)anContMask(f),
                (unsigned long long)anOtherMask(f),
                (unsigned long long)anHomeAddrMask(f), (unsigned long long)anHomeContMask(f),
                (int)anAddrFromLocal(f), (unsigned)anFreshCount(f), f->callees.len);
}

/**
 * @brief Tell whether a value's depth is decided by an allocation site.
 *
 * Only such shapes may have their depth recomputed: the template-time record is a snapshot, and the
 * level solver may afterwards move the site from the home arena back to a block level. Recomputing
 * may only lower `rc->depth` (relax a check), so bindings, call results and values coming from
 * parameters are all treated by their recorded value (leaning to rejection). `targetDepth` cannot
 * replace this: it calls `lookup` for a binding, and `runRefCheck` runs in another module's scope and
 * would find a different binding.
 *
 * @param c     Checker.
 * @param e     Expression to classify; may be NULL.
 * @param hops  Indirect levels already followed; the walk stops past 32.
 * @return      True when the value traces back to `new` or a builtin generic allocation.
 */
static bool depthComesFromAlloc2(Checker *c, Expr *e, int hops) {
    if (!e || hops > 32) return false;
    switch (e->kind) {
    /* `EX_GENCALL` is over-approximated: a generic instance may hand back memory it allocated
     * (`stl::make`, `vec::withCapacity`, ...) and its body is not visible here. The cost of the
     * guess being wrong is a **relaxed** (lower) depth bound, never a crash: the walk only ever
     * lowers `rc->depth`, and call results whose origin is a parameter keep their recorded value. */
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


/**
 * @brief Read the depth a value finally settled on.
 *
 * An allocation site reads `planArenaLevel` (do not fall back to `refDepth`, that number can be
 * stale and would then contradict the arena level); a binding, field or index reads `refDepth`; a
 * `dyn` payload and a container shape take the maximum depth of the values they hold.
 *
 * @param e  Expression; NULL returns 0.
 * @return   Depth of the value; 0 when there is no requirement.
 */
static int solvedDepth(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        /* Do not fall back to `refDepth`: that number can be stale and would then
        * contradict the arena level. */
        return arenaDepthOf(planArenaLevel(e));
    case EX_IDENT: case EX_FIELD: case EX_INDEX:
        return anRefDepth(e) > 0 ? anRefDepth(e) : 0;
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
        /* Only two shapes reach here: `EX_INT` and `EX_CONV`. Neither can **carry a reference**: a literal
 * has none; `EX_CONV`'s source type is deliberately narrowed (only `ref void` is a pointer source,
 * no general pointer conversion) ⇒ **answering 0 is semantics, not a fallback**.
 *
 * The NOTE stays as a **counting sentinel**: a new `ExprKind` reaching here would change the count,
 * and that change is the signal to investigate (related to the walker budget: the repository limits
 * the number of hand-written traversals, and listing every kind here would buy no correctness). */
        EXTC_DBG_NOTEF("solvedDepth: kind=%d answered 0 (INT/CONV only, both carry no reference)", (int)e->kind);
        return 0;
    }
}

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
 *     one is a false rejection rather than a missed promotion, because it
 *     pulls in the level of a value that has nothing to do with this one.
 *   - A binding is followed in both directions. Outwards, along what it holds, which is
 *     what reaches the allocation sites inside the value; inwards, to the binding's own
 *     level, which is where a demand that arrived from another publication enters. The
 *     descent uses the tighter of the two, because a binding that must outlive this frame
 *     cannot be holding storage that dies with it.
 *   - The answer is what travels along the edges between publications, which is why the
 *     pass that calls this runs to a fixed point over them. */
static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops);

/**
 * @brief One step of the level solver from a value down to its allocation sites, with cycle cutting
 *        and a step budget.
 *
 * Every recursive edge inside the body goes through this wrapper, so the path it maintains is
 * exactly the chain being walked, and the budget counts every pair it reaches (reached pairs
 * included -- that is the reason cycle cutting exists).
 *
 * @param c       Checker.
 * @param ls      Level-solver state (path and step budget).
 * @param val     Value to solve.
 * @param target  Level required by the destination.
 * @param hops    Hops already followed; past 32 returns `LEVEL_INF`.
 * @return        Level the value must live to; `LEVEL_INF` when there is no requirement.
 */
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

/**
 * @brief The main body of the level solver: follow one publication along carriers to the allocation
 *        sites it can reach.
 *
 * A publication `(value, at)` says "this value lands somewhere that lives `at` levels deep"; every
 * allocation site the value can reach is then also required to live to `at`. Followed relations: a
 * binding follows `Sym.origin`; `ref` / `*` / `!` follow the same storage; slice/deref/sign are part
 * of an object; fields and indexes live inside an object/array; an aggregate follows every element;
 * a call is not followed (what its callee publishes is the callee's business). This is a fold over
 * the data, independent of order.
 *
 * @param c       Checker.
 * @param ls      Level-solver state.
 * @param val     Published value.
 * @param target  Arena level of the destination; 0 = outside this frame.
 * @param hops    Recursion guard: bindings can form cycles (`a = b  b = a`).
 * @return        Level the value must live to.
 */
static int levelOfValue2(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    if (dbgOn("EXTC_DBG_LV"))
        fprintf(stderr, "      [lv] kind=%-3d line=%-4d target=%-2d hops=%d\n",
                (int)val->kind, val->line, target, hops);
    switch (val->kind) {
    case EX_NEW:
    case EX_GENCALL:
        /* The site this whole walk was looking for. */        if (target < LEVEL_INF && (anMinAt(val) < 0 || target < anMinAt(val))) {
            anSetMinAt(val, target);
        }
        return target;
    case EX_IDENT: {
        Sym *sy = identBindOf(val);
        /* Reaching an allocation through a *value* answers "where did this number come
         * from", and only the three forms above, this one, and the containers below
         * preserve that. The three forms deliberately absent from this switch -- `*e`,
         * `&e` and `e[a:b]` -- do not: `*e` is the thing `e` points at, and `&e` is the
         * address of `e`, so neither is the value the sub-expression holds.
         * Following one anyway is a false rejection, not a missed
         * promotion: in `examples/alloc-in-block`, `return total` is followed through
         * `total = *q` into `q`, and from there into the allocation, so the `alloc` inside
         * the block is told "must outlive this frame" although nothing outlives the
         * block; the function has no home arena to give it, and the generated C references
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
         * is at level 0 because it is returned, while the store `out = h` is recorded at
         * level 1, and entering `out` at 1 leaves everything `out` holds one level too deep:
         * the `alloc` behind `h` stays at the block level while the struct carrying it is
         * returned to the caller. */
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
            /* An identifier that never resolved has no binding to walk, and reaching
             * `sy->heldSrc` with a NULL `sy` is a SIGSEGV, found by tools/fuzz.py from a
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
     * Leaving this form out is the last thing keeping the whole-value if-join unsound.
     * `out = h` stores a join node, so a walk without this case returns "no requirement"
     * at the store itself and never reaches the allocation behind either arm -- on
     * `tests/arena-promoted/C3_if_join_wholevalue`, the site would stay at the block
     * level while the struct holding it was returned. */
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

/**
 * @brief Read the level a binding is currently known to need.
 *
 * A binding's level is not decided by a single statement: the value it holds now may come from
 * several statements, and where it is published may require a longer lifetime than the value itself.
 * Both are edges between publication records, and this table is where they meet.
 *
 * @param ls  Level-solver state.
 * @param sy  Binding.
 * @return    Level of that binding.
 */
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

/**
 * @brief Lower a binding's level, reporting whether it actually moved.
 *
 * There is only the lowering direction: a level is a requirement, and requirements only accumulate.
 *
 * @param ls  Level-solver state.
 * @param sy  Binding.
 * @param lv  New level.
 * @return    True when the level got smaller.
 */
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

/**
 * @brief Read the level a stored value brings from the binding it names.
 *
 * This is the other half of `levelOfValue`: that one walks a value down to the allocation sites
 * inside it, this one walks it up to the bindings that make it up; both are iterated together until
 * the numbers stop moving.
 *
 * @param c     Checker.
 * @param ls    Level-solver state.
 * @param val   Value expression.
 * @param hops  Recursion-guard hops.
 * @return      Level carried by the value; `LEVEL_INF` when there is no requirement.
 */
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
        return anMinAt(val) >= 0 ? anMinAt(val) : anLexicalLevel(val);
    default:
        return LEVEL_INF;
    }
}

/**
 * @brief Run the level pass: fold the publications recorded while checking function bodies.
 *
 * It does **not** take a data-flow fixed point: the level solver reads its own table (seeded by the
 * publications made during the body walk) and runs **before** the DFA's numbers are written back to
 * `Sym.refDepth`.
 *
 * @param c  Checker.
 * @param f  Function to run the level pass on.
 */
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
     * narrow: a store into a place that does not outlive the store itself says nothing
     * new about the value, and walking it anyway moves the `new` inside a
     * loop from the loop arena to the frame arena, where it is not reclaimed each
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
             * on `C3_if_join_wholevalue`, `h = g` is followed by
             * `h = zero`, so the origin names the empty box and the
             * allocation behind `g` is unreachable from every later store of `h`.
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
    bool        releases;  /* the type has an `@drop` hook: leaving the scope closes it */
    Vec         aliases;   /* const char*: `let g = f` shares one obligation */
} FdOblig;

/**
 * @brief Tell whether a type declares a `close` method (resource protocol).
 *
 * @param t  Type.
 * @return   True when its struct declares `close`.
 */
static bool typeIsResource(Type *t) {
    Type *b = t ? ttBase(t) : NULL;
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return false;
    for (size_t i = 0; i < b->sdef->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&b->sdef->methods, i);
        if (m && m->name && strcmp(m->name, "close") == 0) return true;
    }
    return false;
}

/**
 * @brief Find an obligation in the resource-obligation table by generated name (aliases included).
 *
 * @param obs    Obligation table (`FdOblig *`).
 * @param cname  Generated C name; NULL returns NULL.
 * @return       The obligation found; NULL when there is none.
 */
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

/**
 * @brief Mark every obligation "handed out".
 *
 * Used for shapes this walk does not recognize: a missed report is only one diagnostic fewer, while
 * a false report would reject a correct program, so an unknown direction stays silent.
 *
 * @param obs  Obligation table.
 */
static void obligEscapeAll(Vec *obs) {
    for (size_t i = 0; i < obs->len; i++) (*(FdOblig **)vecAt(obs, i))->escaped = true;
}

/**
 * @brief Track resource obligations in an expression: closed or handed out.
 *
 * `f.close()` fulfils the obligation; the receiver itself is not a hand-out (`f.put(x)` only writes
 * through the handle).
 *
 * @param c       Checker.
 * @param e       Expression; NULL is ignored.
 * @param obs     Obligation table.
 * @param escape  True when the value at this position leaves the function.
 */
static void obligExpr(Checker *c, Expr *e, Vec *obs, bool escape) {
    if (!e) return;
    switch (e->kind) {
    case EX_IDENT: {
        FdOblig *o = obligFind(obs, planCName(e));
        if (o && escape) o->escaped = true;
        return;
    }
    case EX_METHOD: {
        /* `f.close()` is the discharge. The receiver is not an escape: `f.put(x)` writes
         * through the handle and leaves the file exactly as open as it was. */
        Expr *recv = e->u.method.recv;
        if (recv && recv->kind == EX_IDENT && e->u.method.name &&
            strcmp(e->u.method.name, "close") == 0) {
            FdOblig *o = obligFind(obs, planCName(recv));
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

/**
 * @brief Track resource obligations in a statement by walking its expressions.
 *
 * @param c    Checker.
 * @param s    Statement; NULL is ignored.
 * @param obs  Obligation table.
 */
static void obligStmt(Checker *c, Stmt *s, Vec *obs) {
    if (!s) return;
    switch (s->kind) {
    case ST_DROP:
        obligExpr(c, s->u.drop_.target, obs, false);
        return;
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
            const char *cn = planCName(s) ? planCName(s) : s->u.var.name;
            /* A `@drop` hook is a release point the compiler emits by itself, so the value is
             * closed where its life ends and there is nothing to warn about. */
            Type *bt = ttBase(s->type);
            StructDef *bsd = bt ? structOf(bt) : NULL;
            bool releases = bsd && bsd->dropHook != NULL;
            Expr *init = s->u.var.init;
            FdOblig *same = (init && init->kind == EX_IDENT)
                          ? obligFind(obs, planCName(init)) : NULL;
            if (same) {
                *(const char **)vecPush(&same->aliases) = cn;
            } else {
                FdOblig *o = (FdOblig *)arenaAllocZero(c->arena, sizeof *o);
                o->cname = cn;
                o->name  = s->u.var.name;
                o->line  = s->line;
                o->releases = releases;
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
            FdOblig *o = obligFind(obs, planCName(s->u.assign.target));
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

/**
 * @brief Warn about every descriptor opened in a function that is never closed.
 *
 * Deliberately flow-insensitive: any `close` anywhere in the function counts. Proving that a
 * **path** closes requires path-sensitive analysis, and rejecting a program that closes the file on
 * one of two paths would be a false report. A second `close` is a no-op in the library, so accepted
 * shapes really are closed at run time.
 *
 * @param c  Checker.
 * @param f  Function to check.
 */
static void checkOpenHandles(Checker *c, FuncDef *f) {
    if (!f || !f->body || f->isExtern) return;
    Vec obs; vecInit(&obs, c->arena, sizeof(FdOblig *));
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        obligStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i), &obs);
    for (size_t i = 0; i < obs.len; i++) {
        FdOblig *o = *(FdOblig **)vecAt(&obs, i);
        if (o->closed || o->escaped || o->releases) continue;
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

/**
 * @brief Hash a C name with FNV-1a.
 *
 * @param p  NUL-terminated name.
 * @return   Hash value.
 */
static size_t sgHash(const char *p) {
    size_t h = 1469598103934665603u;
    for (const char *q = p; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211u; }
    return h;
}
/**
 * @brief Find a bucket in the symbol index, creating one when needed.
 *
 * @param name    Generated name to find.
 * @param create  Whether to create it when missing.
 * @return        Bucket; NULL when no slot is free within 16 linear probes, and the caller falls
 *                back to a linear search.
 */
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
/**
 * @brief Append symbols added since the last call to the symbol index.
 *
 * Append-only (amortized O(1) per symbol). As soon as the index is found to disagree with
 * `c->allSyms` (table window full, list shrank, different arena or first symbol), `g_sbBad` is set
 * and every later call falls back to the linear search for good.
 *
 * @param c  Checker.
 */
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

/**
 * @brief Set up coroutine support for a function: the handle protocol (once per program) plus the
 *        frame type its calls produce and that type's protocol methods.
 *
 * Must also run for **late-created instances**: calls inside a generic body are deferred, and an
 * instance is set up only after the body has been checked. Running it for that instance is what
 * defines `<name>$frame` and makes `value()` land on the substituted `T`.
 *
 * @param c  Checker.
 * @param f  Function definition.
 */
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
         * I yield T". */
        StructDef *cfd = arenaAllocZero(c->arena, sizeof *cfd);
        /* A C name of its own: the frame is emitted through the type channel now, so it must not
         * collide with the function's own name. */
        const char *frameOwner = (planInstName(f) && planInstName(f)[0]) ? planInstName(f)
                                : (f->name ? f->name : "coro");
        cfd->name = arenaPrintf(c->arena, "%s$frame", frameOwner);
        planSetCoroOf(cfd, f);
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
         * through the existing deferred-method machinery. Codegen emits the two inline. */
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

/**
 * @brief Check a function: signature, home-arena need, and body.
 *
 * A function that both allocates and hands back something useful (a reference, a view, a write
 * through an out parameter) needs a home arena; one returning only `i32` does not. The escape set
 * must be computed before the body is walked, because the walk uses it to pick the arena of each
 * call. `extern!` is shape-checked only: C functions take scalars and single pointers, `slice<T>`
 * becomes two C arguments, and structs have no frozen layout.
 *
 * @param c  Checker.
 * @param f  Function to check.
 */
static void checkFunc(Checker *c, FuncDef *f) {
    /* A `@builtin` declaration has a signature but no body (same shape as `extern!`), and this path
 * walks `f->body` shortly below ⇒ a bodyless declaration would segfault on entry. Builtins are
 * implemented by the compiler **at the call site**, so there is nothing to check here; calls to them
 * are rejected by the "not yet implemented" guard in check_expr. Only builtins are skipped, and
 * `extern` behavior is left unchanged. */
    if (f->isBuiltin && !f->body) return;
    /* A lambda's `call` method: its body was checked where the lambda was written, because that is
     * where the capture set comes from (check_expr.c, checkLambda). */
    if (anLamChecked(f)) return;

    /* The coroutine protocols (`next`/`value`/`send`) have no body: they are checked inline at
     * their call sites. Checking one here walked a null body. */
    if (planCoroProto(f)) return;
    /* More parameters than the effect summary has bits would silently lose the high ones. That is
     * exactly the failure mode seen before: the same store shape with 4 parameters was rejected while its
     * 40-parameter twin was accepted, because parameter 40's `addrMask` bit fell off the end of a
     * 32-bit mask. The mask is 64 bits now, and this is the contract on top of it: refusing loudly
     * beats ignoring a parameter. A function that needs more has to be split (an array-backed
     * summary is the deliberate later step if a real program ever asks for it). */
    if (f->params.len > EFF_MAX_PARAMS) {
        ckError(c, f->line,
                "The effect summary indexes parameters by position in a 64-bit mask, so the"
                " compiler refuses more than 64 of them instead of silently ignoring the rest."
                " Split the function.",
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
        anSetNeedsHome(f, false);
        /* Only scalars and single pointers may cross the boundary. A `slice<T>` would
        * become two C arguments (data and length), so the names would not match, and a
        * struct has no frozen layout; the stdlib wrappers pass `s.data` and `s.len`
        * explicitly.
        *
        * The predicate itself lives in `ttCrossesC`: one spelling, shared with the `fn` type
        * (`ttResolve`), which is the other way a C function is reached. Spelling it out here
        * as well would be a second copy that can fall out of step with `ttCrossesC`. */
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
            /* A missing return type is a different mistake from a type that cannot cross, so the
             * two have separate messages: a NULL return type is reported as missing rather than
             * printed as `void`. */
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
     *: one parameter, `slice<slice<u8>>`. C hands the entry
     * point `argc` and `argv`, and codegen wraps those two into the view; any other
     * parameter has no C-level meaning, because C's `main` takes integers and pointers and
     * nothing else. Saying so here keeps the diagnostic in extC instead of letting the C
     * compiler complain about the wrapper.
     *
     * The return type is C's `int`, so it may be absent or `i32`; `-> i64` would be
     * truncated silently by the C compiler, which is exactly the kind of thing that must not
     * be silent. */
    bool isEntryFn = !f->owner &&
                     (f->isModuleEntry || strcmp(f->name, "main") == 0);
    if (isEntryFn) {
        /* An `@module` entry goes through the same gate as `main`: the dispatcher hands it the command line
 * verbatim, so the requirements on its return value and parameter shapes are identical. The message
 * names it the way the user wrote it, not `main`. */
        const char *what = f->isModuleEntry ? "an `@module` entry" : "`main`";
        Type *r = f->ret ? ttBase(f->ret) : NULL;
        bool retOk = !r || r->kind == TY_VOID ||
                     (r->kind == TY_BUILTIN && r->name && strcmp(r->name, "i32") == 0);
        if (!retOk)
            ckError(c, f->line,
                    "C's entry point returns an `int`, so an entry point may return `i32`"
                    " (or nothing at all). Any other type would be converted by the C compiler"
                    " without a word.",
                    "%s cannot return `%s`", what, typeStr(c, f->ret));
        if (f->params.len > 1)
            ckError(c, f->line,
                    "The command line is one view of the arguments. Wrap several pieces of"
                    " information in a struct if the entry point needs more.",
                    "%s takes at most one parameter, `slice<slice<u8>>`; %zu were declared",
                    what, f->params.len);
        else if (f->params.len == 1) {
            Param *p = *(Param **)vecAt(&f->params, 0);
            /* The shape is judged by shape and not through `ttBase`: resolving a view
             * lands on the struct behind it, which does not say `slice`. */
            Type *eo = ttIsViewType(p->type) ? *(Type **)vecAt(&p->type->targs, 0) : NULL;
            Type *ei = eo && ttIsViewType(eo) ? ttBase(*(Type **)vecAt(&eo->targs, 0)) : NULL;
            bool ok = ei && ei->kind == TY_BUILTIN && ei->name && strcmp(ei->name, "u8") == 0;
            if (!ok)
                ckError(c, p->line,
                        "The arguments arrive as bytes, the way the operating system hands them"
                        " over, so the entry point takes `slice<slice<u8>>`: the outer view is"
                        " `args`, and `args[0]` is the program name, again as C defines it.",
                        "%s's parameter must be `slice<slice<u8>>`, found `%s`",
                        what, typeStr(c, p->type));
        }
    }
    /* Every diagnostic inside this body has to name the file the body came from. The
     * loader records that context on each declaration (`FuncDef.ctx`), and this pass runs
     * every unit's functions in one go, so the context is switched here: without it, a
     * mistake inside a module came back pointing at a line of the **entry** file -- the
     * line was the module's, the file was not. */
    Ctx *savedCtx = c->ctx;
    if (f->ctx) c->ctx = f->ctx;

    FuncDef *savedFunc = c->curFunc;
    /* A function that allocates and hands back something useful (a reference or a view,
    * or a store through an out-parameter) needs a home arena. One that returns `i32`
    * does not: its allocations stay in its own block. */
    if (!f->isExtern && stmtHasNew(f->body) &&
        ((f->ret && typeContainsRef(c->tt, f->ret)) || stmtStoresThroughDeref(c, f->body, f)))
        anSetNeedsHome(f, true);

    Vec     *savedParams = c->curParams;

    /* A coroutine: the declared return type is `coroutine<T>` (a prototype in the prelude). The
     * declaration is the marker -- `yield` is legal only here, and the frame the checker lays out
     * for this function is a value. */
    /* Both `coroutine<T>` (shorthand: request and reply share a type) and `coroutine<A, B>` count as
 * coroutines: one type parameter and two respectively. Which parameter each protocol method takes is
 * set in the synthesis below. */
    planSetIsCoro(f, f->ret && (isProtoType(f->ret, "coroutine", 1)
                           || isProtoType(f->ret, "coroutine", 2)));
    planSetCoroRetProto(f, planIsCoro(f) ? f->ret : NULL);
    /* `coroutine<A, B>`: `B` (the last parameter) is the yielded type. The shorthand `coroutine<T>` has a
 * single parameter, and that one is it. */
    planSetYieldType(f, planIsCoro(f)
        ? *(Type **)vecAt(&f->ret->targs, f->ret->targs.len - 1) : NULL);
    /* What a coroutine yields is a **value**. Its frame is released when the task ends, so a
     * reference handed out from it can outlive the storage it points into -- the consumer has no
     * way to know the referent is gone. A pointer type is also not a spelling the per-type
     * helper names can be built from. Refusing the type here, at the declaration, reports the
     * mistake against the protocol instead of inside the generated C. */
    if (planIsCoro(f) && planYieldType(f) && typeContainsRef(c->tt, planYieldType(f)))
        ckError(c, f->line,
                "A coroutine hands out values. Its frame dies with the task, so a reference"
                " yielded from it can point at storage the consumer cannot tell is gone. Yield the"
                " value itself, or an index the consumer resolves against something that outlives"
                " the task.",
                "`%s` yields `%s`, which carries a reference",
                f->name, typeStr(c, planYieldType(f)));
    if (planIsCoro(f)) coroSetup(c, f);
    /* A parameter typed `coroutine<T>` is legal: the unified handle, not a
     * marker, is what the type stands for, so no check rejects such a parameter here. */

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

    anSetParamSymCount(f, 0);                    /* a generic body may be walked more than once */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* A parameter is mutable: it is a local copy handed over by the caller, as in C,
         * so `ref real` is allowed inside `fn f(real: Board)`. */
        Sym *sym = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sym->cname;
        /* The storage a reference parameter points at is **the caller's** (a borrow): the destination of
 * `(*p).f = v` is computed from the shallowest depth of the possible referents, and for a parameter
 * that is 0 (it lives past this frame). */
        if (p->type && tsub(c, p->type)->kind == TY_REF) sym->minPointee = 0;
        /* Record the binding now, while the scope exists: the effect summary runs later, with no
         * scope at all, and needs the identity to attribute a store to the right parameter. */
        if (anParamSymCount(f) < EFF_MAX_PARAMS) {
            int at = anParamSymCount(f);
            anParamSyms(f)[at] = sym;
            anSetParamSymCount(f, at + 1);
        }
    }
    /* The body does not open a scope of its own: parameters and body locals share one.
     * Shadowing a parameter with `let a = ...` is therefore only a new name, which is
     * legal and becomes `a__2` in the generated C, while `var a = ...` declares a second
     * piece of storage and is rejected (see `declare`). */
    checkFuseFuncBody(c, f->body);
    LiveViews live;
    live.items = NULL;
    live.len = 0;
    live.cap = 0;
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++) {
        checkHoistStatementTrys(c, &f->body->u.block.stmts, &i);
        Stmt **slot = (Stmt **)vecAt(&f->body->u.block.stmts, i);
        Stmt *lowered = arrayAssignLower(c, *slot);
        if (lowered) *slot = lowered;
        liveViewsStmtPre(c, &live, &f->body->u.block.stmts, i);
        checkStmt(c, *slot);
        liveViewsStmtPost(c, &live, *slot);
    }

    /* The types of the declarations are final by now, so this is where a coroutine's frame can be
     * laid out -- and where rule 2 is enforced on it. */
    coroFrameLay(c, f);

    /* The types of the declarations are final by now, so this is the place to ask
     * whether every handle the function opened also gets closed. */
    checkOpenHandles(c, f);

    /* A `par` worker body is checked like an ordinary function, but a worker signature has a run-time
 * convention (its `id` is for the trampoline), so it is re-audited against the resolved shape once
 * the body has been checked. */
    if (planIsParWorker(f)) parWorkerAudit(c, f);

    /* A parameter the body never reads. extC lets the program keep it and the generated C marks it
     * `unused` so the C compiler stays quiet, but the reader deserves to hear about dead weight in
     * a signature they wrote. `self` is exempt: a method that ignores its receiver is ordinary. */
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (planIsParWorker(f) && p->name && strcmp(p->name, "id") == 0) continue;
        if (!p->name || !p->cname || strcmp(p->name, "self") == 0) continue;
        if (stmtUsesCname(f->body, p->cname)) continue;
        ckWarn(c, p->line,
               "a parameter belongs to the signature, so it is kept and marked `unused` in the"
               " generated C",
               "parameter `%s` is never used", p->name);
    }

    collectEffects(c, f);      /* compute the effect summary; it is only recorded here */
    anSetArenaSites(f, c->curArenaSites);   /* handed to the pass that runs after the analysis closes */

    /* Reference depths, recomputed as a monotone data-flow fixed point over the body.
     *
     * The checking walk above maintains a binding's depth by destructive assignment and
     * has no join at control-flow merges, so at an `if` the arm visited last simply wins.
     * That cannot produce the upper bound the escape check needs, and it is the reason a
     * struct holding an allocation could come back pointing into a block arena. The walk
     * below derives the same numbers from the whole control-flow structure instead:
     * stores raise a depth, merges take the maximum, and loops are iterated to a fixed
     * point. It reads the tree and writes only the depths, so every later reader sees a
     * value that does not depend on the order branches were visited.
     *
     *3, item 3-a, and `src/check_dataflow.c`. */
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
                    if (d > anRefDepth(sy)) anSetRefDepth(sy, d);
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
                        if (sy && sy->line >= 0 && d > anRefDepth(sy)) anSetRefDepth(sy, d);
                    }
                }
            }
        }
        /* The destination depth is fixed now: the DFA has written back, and `Sym.minPointee` has collected
 * every retarget during the walk. Re-judge the `(*cur).f = v` stores the walk deferred -- word order
 * does not affect the answer (the scope is still open, so `lookup` works). */
        recheckPendingRefStores(c, !dfr.overflow);
        /* Verification only (`EXTC_DBG_LVL=1`): derive a place level for every expression
         * from the whole body and print where that answer disagrees with the per-site plan
         * bits the check-time rules used. Reads the tree, prints to stderr, changes nothing;
         * returns before doing any work when the variable is unset. */
        lvlAnalyze(c, f);
    }
    popScope(c);
    c->curFunc = savedFunc;
    resultsEnterOwner(savedFunc);
    c->curParams = savedParams;
    c->ctx = savedCtx;
    c->storeBase = savedStoreBase;
}

/**
 * @brief Tell whether an initializer expression is a constant expression.
 *
 * There is no constant folder, so the shape is inspected instead: literals, negated literals, enum
 * constants, and arithmetic over them; every call is rejected.
 *
 * @param e  Initializer expression; may be NULL.
 * @return   True when it is a constant expression.
 */
bool isConstInit(Expr *e) {
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
     * C-ABI line's table: a table of `fn` fields cannot be zero-initialized
     * (a `fn` has no zero value, which is the whole point), so it must be initialized where it is
     * declared -- and a table belongs at the top level, because the host hands it to a module that
     * may keep it. Without this the only way to build a table was to assign the slots in a function,
     * which put the storage in a frame: exactly the lifetime a table must not have. */
    case EX_IDENT: return planCallee(e) != NULL;   /* the checker set it: this name is a function */
    /* `null` is C's `(void *)0`: nothing runs to produce it. It matters for the same reason the
     * line above does -- a table has a `ctx: ?ref void` field, and the slot beside it holds a
     * function address. */
    case EX_NULL:  return true;
    /* A **lossless** conversion of a constant is one: `i64(3)` is `(int64_t)3` in C, which folds,
     * and a table's `size: i64(3)` field is written exactly that way. A conversion the checker marked
     * as needing a runtime check is **not** a constant -- it comes out as a call to a helper
     * (`((uint64_t)extc_narrowU((uint64_t)(4096), ...))`, as for `let PAGE: u64 = u64(4096)`),
     * and gcc refuses that in a static initializer. Saying so here means the user gets "must be
     * initialized with a constant" instead of a gcc error in the product. */
    case EX_CONV: return !planConvCheck(e) && isConstInit(e->u.conv.operand);
    default: return false;
    }
}
/**
 * @brief Check top-level `let` and `var` declarations.
 *
 * A global lives at depth 0 and outlives every frame; two consequences then follow with no special
 * rule: the escape rule already rejects storing a local into a global, and a fixed-size global is a C
 * static object that needs no arena. The initializer must be constant, because a C static initializer
 * may contain constant expressions only.
 *
 * @param c  Checker.
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

        /* A global of a type with an `@drop` hook gets that hook run at process exit
         * (`atexit`, emitted by codegen). Mark it reachable here: the only other place that does
         * so is a `var` declaration, and a global is not checked through `ST_VAR` in a body. */
        if (!ttIsError(g->ann) && g->ann) {
            StructDef *sd = structOf(ttBase(ttIsError(g->ann) ? NULL : tsub(c, g->ann)));
            if (sd && sd->dropHook) planSetUsed(sd->dropHook, true);
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
 * `grow(box<T>)` -- and nothing stopped it: the compiler kept creating instances without
 * bound. This is a limit of the compiler, not of the language, so the message says so. */
#define FUNC_INST_LIMIT 4096

/* How deeply a **created** type may nest. The runaway in E3 does not just add instances: each
 * step instantiates with a type one level deeper (`box<T>` again), so by step N the type is N
 * levels deep and every copy of it costs O(N) -- the total is O(N^2) and the compiler was
 * OOM-killed after 54 seconds. Depth is the quantity that has to be bounded; the instance count
 * above is only a backstop. 64 is far above anything a source file can write by hand
 * (the attack suite's A4 uses 10) and far below anything that can exhaust memory. */
#define TYPE_DEPTH_LIMIT 64

/**
 * @brief Count the nesting depth of type-instantiation arguments (0 for an ordinary type).
 *
 * Recursion is truncated defensively: it runs on types the compiler itself built.
 *
 * @param t  Type.
 * @return   Nesting depth.
 */
static int typeDepth(Type *t) {
    if (!t) return 0;
    int best = 0;
    for (size_t i = 0; i < t->targs.len; i++) {
        int d = typeDepth(*(Type **)vecAt(&t->targs, i));
        if (d > best) best = d;
    }
    return best + (t->targs.len ? 1 : 0);
}

/**
 * @brief Create or find a concrete instance of a free generic function.
 *
 * A type instance (`varArray<i32>`) is decided by the type, while a free function's type arguments are
 * read off the call site, so an instance can only be created there. The same `targs` list always maps
 * to the same instance.
 *
 * @param c      Checker; the instance is appended to `c->funcInsts` and to the module.
 * @param tmpl   Generic template; NULL yields NULL.
 * @param targs  Concrete type arguments.
 * @param line   Source line of the call; unused, kept for the call shape.
 * @return       Stand-alone `FuncDef`: `tmpl` points back at the template, `targs` and `instName` are
 *               filled in, and parameter and return types are substituted; codegen emits it like an
 *               ordinary function.
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
    planSetUsed(in, false);
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
    if (planCoroRetProto(tmpl)) {
        in->ret = ttSubstitute(c->tt, planCoroRetProto(tmpl), &tmpl->typeParams, targs);
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

/**
 * @brief Infer and fill type parameters from argument types.
 *
 * For a call like `f<T>(x)`, inferring `T` from the argument type means matching `slice<T>` against
 * `slice<u8>`.
 *
 * @param tt     Type table.
 * @param tp     Type-parameter names, in the same order as `targs`.
 * @param targs  One cell per type parameter, filled in during inference; an already filled cell must
 *               agree with the new type.
 * @param want   Parameter type, may mention type parameters.
 * @param got    Type of the argument.
 * @return       False when it cannot be inferred (e.g. `T` appears only in the return type), in which
 *               case the call must spell it out: `f<i32>`.
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
    /* Generic **enum** instances (`option<T>`, `result<T,E>`) unify per argument exactly like generic
 * structs: without this arm, a signature such as `fn f<T>(o: option<T>)` could never infer `T` (the
 * library's `preludeIsNone` has precisely this shape). */
    if (want->kind == TY_ENUM && got->kind == TY_ENUM) {
        if (want->edef != got->edef || want->targs.len != got->targs.len) return false;
        for (size_t i = 0; i < want->targs.len; i++)
            if (!unifyTParams(tt, tp, targs, *(Type **)vecAt(&want->targs, i),
                              *(Type **)vecAt(&got->targs, i))) return false;
        return true;
    }
    /* `[N]T` against a concrete array: the `const` parameter's argument is the array type the
     * caller passed (that is where the length comes from), then the element types unify. */
    if (want->kind == TY_ARRAY && want->asize < 0 && want->param && got->kind == TY_ARRAY) {
        for (size_t i = 0; i < tp->len; i++) {
            if (strcmp(*(const char **)vecAt(tp, i), want->param) != 0) continue;
            Type *cur = *(Type **)vecAt(targs, i);
            /* Two different lengths: keep the first witness, so the instance is built and the
             * second argument is then checked against that length -- the diagnostic the author
             * needs ("expects `[3]f64`, found `[2]f64`") comes out of the ordinary argument check
             * instead of an "cannot infer" message here. */
            if (!cur) *(Type **)vecAt(targs, i) = got;
            break;
        }
        return unifyTParams(tt, tp, targs, want->inner, got->inner);
    }
    if (ttHasParam(want)) return false;   /* mentions T but the shapes disagree => cannot infer */
    return true;
}

/**
 * @brief Callback context for the instance-use closure: methods of the same owner reached from a
 *        method body are recorded as used by the same instance.
 */
typedef struct {
    Checker *c;     /* Checker: its type table is needed to substitute the receiver type */
    Vec *params;    /* Current instance's type-parameter names (struct or free-function template) */
    Vec *targs;     /* Corresponding type arguments */
} InstUseCtx;

/**
 * @brief `instUseClosure`'s expression callback: record a hit and keep walking the whole tree.
 *
 * @param raw  `InstUseCtx *`.
 * @param e    Current expression.
 * @return     Always true (the closure only grows, so there is no reason to stop early).
 */
static bool instUseOnExpr(void *raw, Expr *e) {
    InstUseCtx *ctx = raw;
    if (e) {
        FuncDef *callee = planCallee(e);
        if (callee && callee->owner && callee->owner->typeParams.len > 0 &&
            ctx->params && ctx->targs) {
            /* The receiver type may be an instance of another generic struct (`ndarray<T>::viewMut` calls
 * `ndview<T>::rowMajor`): substitute with the current instance's type arguments first, then take the
 * instance name. */
            Type *recvType = NULL;
            switch (e->kind) {
            case EX_METHOD: recvType = e->u.method.recv->type; break;
            case EX_ASSOC:  recvType = planAssocOwner(e); break;
            case EX_BIN:    recvType = e->u.bin.left->type; break;
            case EX_UN:     recvType = e->u.un.operand->type; break;
            default: break;
            }
            Type *t = recvType ? ttSubstitute(ctx->c->tt, recvType, ctx->params, ctx->targs) : NULL;
            Type *b = t ? ttBase(t) : NULL;
            if (b && b->kind == TY_GENERIC && b->sdef == callee->owner && b->name &&
                !ttHasParam(b))
                planSetInstUsed(callee, b->name);
        }
    }
    AstVisit v = { instUseOnExpr, NULL, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief `instUseClosure`'s expression callback: record a hit and keep walking.
 *
 * @param raw  `InstUseCtx *`.
 * @param e    Current expression.
 * @return     Always true (walk the whole tree; the closure only grows).
 */
static bool instUseOnExpr(void *raw, Expr *e);

/**
 * @brief Record the methods of the **same struct** reached from an instance method body as used by
 *        this instance.
 *
 * A method body is shared by all instances: `self.grow()` resolved on the template carries the method
 * template but no instance, so when deciding per instance whether to emit a body,
 * `vector<i64>::push` would be emitted while its `grow` would not. Following the resolution
 * (`planCallee`), methods of the same owner are recorded here too; the set only grows, so this
 * terminates.
 *
 * @param c       Checker (its arena walks the AST).
 * @param body    Method body to walk.
 * @param params  Instance's type-parameter names.
 * @param targs   Corresponding type arguments.
 */
static void instUseClosure(Checker *c, Stmt *body, Vec *params, Vec *targs) {
    InstUseCtx ctx;
    ctx.c = c;
    ctx.params = params;
    ctx.targs = targs;
    AstVisit v = { instUseOnExpr, NULL, &ctx };
    if (body) (void)astWalkStmtChildren(body, &v);
}

/**
 * @brief Re-check a deferred operator use for one concrete instance.
 *
 * The two batches deferred to instantiation (this operator batch and the reference-rule batch) take
 * the same path for type instances and free-function instances, so they share one helper instead of a
 * second copy for `fn f<T>`.
 *
 * @param c         Checker.
 * @param ec        Recorded check whose operand types still mention type parameters.
 * @param params    Instance's type parameters.
 * @param targs     Instance's type arguments, positionally matching `params`.
 * @param instName  Instance name used in diagnostics.
 *
 * The caller must first set `c.substParams` / `c.substArgs` to this instance (`runRefCheck` reads
 * them through `tsub`). The diagnostic names the recorded operator, not a hard-coded `==`.
 */

static void runOpCheck(Checker *c, OpCheck *ec, Vec *params, Vec *targs, const char *instName) {
    TypeTable *tt = c->tt;
    Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type, params, targs);
    Type *rt2 = ttSubstitute(tt, ec->node->u.bin.right->type, params, targs);
    /* A `T op T` in the template is an instance's `A op B` here. The equality of the two is
     * **not** this function's business: `typeSupportsOp` below is the authority on "can this
     * type be compared with that one" -- it accepts native numerics, judges an array by its
     * element type **and the same array type on the other side**, and finds an operator method
     * for structs. Asking "is the left operand a struct?" first skips all of that for
     * every other type, so a `[1]i32 == i32` slips through and the emitted descriptor comparison
     * reads 8 bytes out of a 4-byte operand (ASan: stack-buffer-overflow in `extc_eq`, found by
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
     * Not marking it would let `T == T` instantiated at a slice call the prelude's
     * `slice<T>::==` without anybody emitting it, so the generated C would hold a call to a
     * function that does not exist. The node itself is not touched on purpose: the template
     * has one node and many instances, so a `func` stored here would be the wrong method
     * for every other instance. */
    FuncDef *m = findOp(tt, ttBase(lt), ec->op, rt2,
                        strcmp(ec->op, "!=") == 0 ? "==" : NULL);
    if (m) planSetUsed(m, true);
    if (m) ckNoteInstUse(m, lt);
}

/**
 * @brief Re-judge a deferred call result's USE record.
 *
 * The template accepted it because the value's type was the error type at the time; here the receiver
 * type is concrete, the method's real return type is known, and the expected type recorded at the use
 * site can finally be enforced. `instName` is the instance name, so the diagnostic reads
 * "`f_i32` expects `i8`, found `i64`" -- doing this at instantiation rather than on the template is
 * the point.
 *
 * @param c         Checker.
 * @param du        Recorded use site.
 * @param params    Instance's type-parameter names.
 * @param targs     Instance's type arguments.
 * @param instName  Instance name, used in diagnostics.
 */
static void runDeferredUse(Checker *c, DeferredUse *du, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    /* Nodes that are not deferred method nodes carry their own type: compare the substituted expression
 * type. Values of type-parameter type come this way (`s = s + c.value()` inside `fn run<T>` records a
 * sum whose type on the template is `T`). Reading `u.method.recv` on such a node is wrong: that union
 * member is not `EX_METHOD`. */
    if (du->call->kind != EX_METHOD || planCallee(du->call)) {
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

/**
 * @brief Re-check a deferred method call for one concrete instance.
 *
 * On the template the receiver type is a type parameter and no method is found, so the call is
 * recorded; here the receiver type is concrete: when the method exists it is marked used so codegen
 * emits it, and when it does not exist that is exactly why the deferral exists -- report that this
 * instance does not support the call.
 *
 * The node deliberately **records no `func`**: a template body has several instances, and a recorded
 * `func` would be the wrong method for all the others; codegen re-resolves on the substituted receiver
 * type, just as it does for operators. The argument count is checked here because nothing else can see
 * it: the template has no signature to compare against, and letting it through would only blow up in
 * the generated C.
 *
 * @param c         Checker.
 * @param mc        Deferred call: node, method name, argument count.
 * @param params    Instance's type-parameter names.
 * @param targs     Instance's type arguments.
 * @param instName  Instance name, used in diagnostics.
 */
static void runMethodCheck(Checker *c, MethodCheck *mc, Vec *params, Vec *targs,
                           const char *instName) {
    TypeTable *tt = c->tt;
    Type *rt = ttSubstitute(tt, mc->node->u.method.recv->type, params, targs);
    Type *rb = ttBase(rt);
    /* The instance's type is a **bare `fn(...)` pointer**: `f.call(x)` on such a value is a call
     * through the pointer, not a method of the type. The template recorded the call as deferred
     * because `F` was opaque; the signature the call has to match is the pointer's own. */
    if (rb && rb->kind == TY_FN && mc->name && strcmp(mc->name, "call") == 0) {
        if (mc->nargs != rb->params.len) {
            ckError(c, mc->node->line, NULL,
                    "`%s` takes %zu argument%s, but %zu %s written",
                    mc->name, rb->params.len, rb->params.len == 1 ? "" : "s", mc->nargs,
                    mc->nargs == 1 ? "was" : "were");
        }
        return;
    }
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
    planSetUsed(f, true);
    ckNoteInstUse(f, rb);
}

/**
 * @brief Tell whether a deferred reference check belongs to this free-function instance.
 *
 * @param rc  A check deferred from a template body.
 * @param fi  Instance being replayed.
 * @return    True when the check came from this instance's own template and the template is generic;
 *            methods take the type-instance path and answer false.
 */
static bool refCheckApplies(RefCheck *rc, FuncDef *fi) {
    FuncDef *tmpl = planTemplate(fi);
    if (!fi || !tmpl) return false;
    if (rc->func != tmpl) return false;
    return tmpl->typeParams.len > 0;
}

/**
 * @brief Re-check one deferred reference rule for a concrete instance.
 *
 * A generic body is checked once on the template, where `T` is opaque, so every rule whose outcome
 * depends on `T` is replayed here per instance.
 *
 * @param c         Checker.
 * @param rc        Deferred check: the value, its depth, the depth it must fit into, and what the
 *                  reference rule applies to.
 * @param instName  Instance name, used in diagnostics.
 *
 * The depth recorded at template time is a snapshot of a world the level solver changes afterwards, so
 * shapes whose depth is **decided by an allocation site** are recomputed here, lowering only;
 * recomputation is not allowed for bindings -- their site is not fixed yet, and answering 0 would
 * accept a truly dangling pointer.
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
        if (rc->func && !planUsed(rc->func)) return;
        Type *vt = tsub(c, rc->val->type);
        /* Fourth kind: an owning value stored into a view element.
         *
         * The element storage is a plain array; the store copies the bytes and no hook runs for
         * that copy, so the value would be released twice -- once by this copy and once by the
         * original name. The concrete case is reported in `checkStoreEscape`; a template sees
         * only `T`, so the decision is made here with the instance's type. This is what makes
         * `vector<vector<i64>>` a compile error instead of the silent use-after-free it was.
         */
        /* Fifth kind: an owning value read out of storage the reader does not own. */
        if (rc->ownRead) {
            /* Only for the instance that actually calls this method: the template is used by
             * another instantiation, which says nothing about this one. */
            if (rc->func && !planInstUsed(rc->func, instName)) return;
            Type *ot = tsub(c, rc->declType);
            c->substParams = NULL;
            c->substArgs   = NULL;
            if (!ot || ot->kind == TY_REF || ttHasParam(ot)) return;
            StructDef *osd = structOf(ttBase(ot));
            if (!osd || !osd->noCopy || !rc->val) return;
            if (rc->val->kind != EX_INDEX && rc->val->kind != EX_DEREF && rc->val->kind != EX_FIELD)
                return;
            char omsg[EXTC_MAXERR];
            snprintf(omsg, sizeof omsg,
                     "in instance `%s`: `%s` is `@noCopy`: it may not be copied by value",
                     instName, DN(osd));
            ctxError(rc->ctx ? rc->ctx : c->ctx, rc->line, 1,
                     "An element, a field or a pointee hands back bytes that the aggregate still"
                     " owns, so two names would free one storage. Read it through a method call on"
                     " the view, or take it out with a move such as `pop`.", "%s", omsg);
            return;
        }
        if (rc->viewElem && !rc->viewMove && typeHasDrop(c, vt, 0)) {
            c->substParams = NULL;
            c->substArgs   = NULL;
            /* Report through the file the store was written in, not the caller's: the store
             * lives in the callee's body, so the entry file's path with the callee's line
             * number would point at an unrelated line of the wrong file. Naming the instance
             * keeps the call path visible from there. */
            char msg[EXTC_MAXERR];
            snprintf(msg, sizeof msg,
                     "in instance `%s`: a view element cannot hold `%s` -- it owns a reservation"
                     " and the copy would never be dropped", instName, typeStr(c, vt));
            ctxError(rc->ctx ? rc->ctx : c->ctx, rc->line, 1,
                     "A container's element storage is a flat array: the store copies the bytes and no"
                     " `@drop` hook runs for that copy, so the value would be released twice. A view"
                     " cannot hold an element that owns a reservation.", "%s", msg);
            return;
        }
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
        } else if (rc->target && rc->at == 0 && anBorrowed(rc)
                   && !valTracesToParam(c, rc->func, rc->val)) {   /* not parameter-backed */
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: cannot store a borrowed value into something"
                    " that outlives this call", instName);
        }
}

/**
 * @brief Intern the type instances that appear as **local declaration types** inside a body.
 *
 * A statement walk suffices: the failing shape is `var p: pair<T>` in a generic function, and
 * interning is a side effect of substitution itself.
 *
 * @param c  Checker.
 * @param f  Function definition.
 */
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
         * named an instance nobody emitted (`'slice_box_i64' undeclared`). */
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

/**
 * @brief Tell whether an instance is a **temporary placeholder** rather than a real instance.
 *
 * A generic call inside a generic body must create a placeholder so the enclosing template body can
 * keep type-checking (`T` is opaque there). At instantiation every real call site retargets to a
 * concrete instance; a placeholder is owned by the checker and codegen never emits it, and deferred
 * re-checks skip it too -- asking a placeholder "does this type have `<`" is exactly the question the
 * template cannot answer, and a concrete instance answers it.
 *
 * @param targs  Type arguments.
 * @return       True when any argument still mentions a type parameter.
 */
static bool provisionalInstance(Vec *targs) {
    for (size_t i = 0; i < targs->len; i++)
        if (ttHasParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/**
 * @brief Re-resolve a deferred call site into a concrete instance under the enclosing instance's
 *        substitution.
 *
 * Three steps: substitute the type arguments, intern (or create) that concrete instance, and point the
 * call expression's `func` at it (`cc->node->func`). Instances are interned (`funcInstance`
 * deduplicates), so one argument combination has exactly one instance.
 *
 * @param c              Checker.
 * @param cc             Recorded call site whose type arguments still mention type parameters.
 * @param enclosing      Enclosing instance's function definition.
 * @param enclosingInst  Enclosing instance's type; the fallback answer for a site that belongs to no
 *                       instance.
 * @param params         Enclosing instance's type-parameter names.
 * @param targs          Enclosing instance's type arguments, positionally matching `params`.
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
    planSetUsed(inst, true);
    /* The site may be shared by every instance of the enclosing body (`wrap<i32>` and
     * `wrap<meter>` share one `pick(x)`), and the answer differs per instance: record it
     * against the enclosing instance as well. The plain pointer stays as the fallback
     * answer for a site that belongs to no instance. */
    planSetCallee(cc->node, inst);
    planSetAltCallee(cc->node, enclosing, enclosingInst, inst);
}

/**
 * @brief One round of the `makesPool` closure.
 *
 * The **template** round of a generic struct is deliberately skipped: template and instance share one
 * `FuncDef`, `k.hash()` cannot be resolved while the template is being checked, and marking true there
 * would be frozen in and the instance round could never take it back. An `extern` has no body to walk
 * -- `stmtMakesPool` finds the callee at the call site, and `extc_pool_new` is exactly that case.
 *
 * @param cx       Closure context.
 * @param k        Property (`REACH_POOL`).
 * @param changed  Set true when this round changed something.
 */
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
        /* **Skip the template round of generic structs**: template and instance **share the same FuncDefs**
 * (`template find` and `hashMap$hashMap.find` are the same pointer), and `k.hash()` cannot be resolved
 * in the template round ⇒ it can only be marked true conservatively, and once marked, the instance
 * round's `if (getReach(f, k)) continue` skips it forever, so precise resolution **never gets a chance**.
 * A generic struct's method bodies only run on instances anyway, so they are left to the round below,
 * which resolves per instance; when it cannot resolve that round stays conservative too
 * (`resolveOnInstance` returns NULL ⇒ treated as making a pool), and multiple instances are **OR-ed**
 * (any instance making a pool ⇒ the whole chain is conservatively marked true), which keeps the
 * direction safe. Non-generic structs have no such problem and go through the ordinary path. */
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (getReach(f, k)) continue;
                if (bodyReaches(f, k)) { setReach(f, k, true); *changed = true; }
            }
        }
        /* The instance round: an instance of a generic method is a shallow copy of the template (sharing its
 * body), so it is not in `m->structs` but in the type table's `tt->instances`. This round resolves
 * protocol methods **per instance**, substituting exactly as `runMethodCheck` does. */
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


/**
 * @brief Collect the function list the arena analysis walks: free functions, each struct's methods,
 *        and methods attached by `impl` blocks.
 *
 * `impl` groups are included even though an `impl` block usually lives in a module **different** from
 * the type it extends (`impl ostream` is in `stl::stringio`, the type in `std::io`): the module that
 * carries the block is the entry module. The list is spelled out only here.
 *
 * @param arena  Arena to allocate from.
 * @param m      Module.
 * @param out    Output: appended `FuncDef *`s.
 */
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

/* ------------------------------------------------------------------
 * "This frame does not decrement": the **least fixed point** over the whole call graph
 * (`planRcNoDec`).
 *
 * Eliding the `inc` of a local reference binding (`var p = t`) is unsound in one case only: while `p`
 * is alive some `dec` drives the object's `hold` to 0 -- `reclaim.c`'s `freeSlot` frees on the spot and
 * `p` dangles. Every `dec` comes from a **store into a reference slot** (`storeRefOwned` / `storeRef` /
 * an overwriting assignment). Hence:
 *
 *     rcNoDec(F) = ¬writesRefSlot(F) ∧ ¬externOrUnknownCall(F) ∧ ¬concurrent(F)
 *                  ∧ ∀g∈callees(F): rcNoDec(g)
 *
 * This is a least fixed point (tightened down from "everything holds"): **a mutually recursive set of
 * pure-reading functions still holds** -- exactly the shape `bt`'s `check` needs (it only reads and
 * calls itself). A self-loop is no reason to be conservative here: as long as nobody in the cycle
 * writes a reference slot, the cycle contains no `dec`.
 *
 * Two things are never marked: generic templates/instances (one AST slot is shared by all instances, so
 * holding for one instance proves nothing for the next), and any function not marked at all (default
 * `false` = counted as usual, the safe direction).
 *
 * **One language rule it depends on (coroutines)**: this scan sees the AST only. Coroutine frame-field
 * initialization reruns on resume with **overwrite semantics** (`extc_rc_storeRef`, `dec` included), a
 * write the scan cannot see -- but it requires "a reference lives across a `yield`", which the compiler
 * **rejects** (`p lives across a yield and carries a reference`). So a coroutine frame field can hold
 * no reference ⇒ no invisible `dec` exists. No function in the repository corpus combines
 * `initRefNoHold` with an overwriting `storeRef`.
 * **If that language rule is ever relaxed, coroutine functions must be excluded here explicitly.**
 */
/**
 * @brief Context of the `rcNoDec` scan: the checker plus a "definitely disqualified" flag.
 *
 * Read this typedef together with the two forward declarations below: `rcScanStmt` and `rcScanExpr`
 * are mutually recursive (a statement contains expressions, an expression contains statements), so at
 * least one forward declaration is needed.
 */
typedef struct {
    Checker *c;      /**< Checker (reads the type table and call-site resolutions). */
    bool     bad;    /**< A "will decrement" source was already found ⇒ this function is not marked. */
} RcScan;

/**
 * @brief Statement callback: can this statement cause a `dec` (write a reference slot, a static
 *        reference slot, a concurrency domain)?
 *
 * @param ctx  Scan context (`RcScan *`).
 * @param s    Visited sub-statement.
 * @return     `false` stops the whole walk.
 */
static bool rcScanStmt(void *ctx, Stmt *s);

/**
 * @brief Expression callback: is there an unresolved callee, an `extern`, or a concurrency start here?
 *
 * @param ctx  Scan context (`RcScan *`).
 * @param e    Visited sub-expression.
 * @return     `false` stops the whole walk.
 */
static bool rcScanExpr(void *ctx, Expr *e);

/**
 * @brief Walk one level of children: statement children when given a statement, expression children
 *        when given an expression.
 *
 * @param s   Statement; when NULL, `e` is used instead.
 * @param e   Expression; ignored when `s` is non-NULL.
 * @param sc  Scan context (a hit sets `bad`).
 * @return    True when the walk finished; false when a callback stopped it early.
 */
static bool rcScanChildren(Stmt *s, Expr *e, RcScan *sc) {
    AstVisit v = { rcScanExpr, rcScanStmt, sc };
    return s ? astWalkStmtChildren(s, &v) : astWalkExprChildren(e, &v);
}

/**
 * @brief Statement callback: can this statement cause a `dec` (write a reference slot, a static
 *        reference slot, a concurrency domain)?
 *
 * @param ctx  Scan context (`RcScan *`).
 * @param s    Visited sub-statement.
 * @return     `false` stops the whole walk (the function is already disqualified).
 */
static bool rcScanStmt(void *ctx, Stmt *s) {
    RcScan *sc = ctx;
    if (!s) return true;
    switch (s->kind) {
    case ST_ASSIGN:
        /* Writing a slot that may contain a reference = a possible dec (it overwrites the old value).
 * An unknown type counts as bad too. */
        if (!s->u.assign.target || !s->u.assign.target->type
            || typeContainsRef(sc->c->tt, s->u.assign.target->type))
            sc->bad = true;
        break;
    case ST_VAR:
        /* A reference slot in static storage: anything, callees included, can write it at any time
 * ⇒ conservatively bad. */
        if (s->u.var.isGlobal && (!s->type || typeContainsRef(sc->c->tt, s->type)))
            sc->bad = true;
        break;
    case ST_DOMAIN:
        sc->bad = true;                       /* scheduling domain: concurrency / interleaving */
        break;
    default:
        break;
    }
    if (sc->bad) return false;
    return rcScanChildren(s, NULL, sc);
}

/**
 * @brief Expression callback: is there an **unresolved** callee, an `extern`, or a concurrency start
 *        here?
 *
 * @param ctx  Scan context (`RcScan *`).
 * @param e    Visited sub-expression.
 * @return     `false` stops the whole walk.
 */
static bool rcScanExpr(void *ctx, Expr *e) {
    RcScan *sc = ctx;
    if (!e) return true;
    if (e->kind == EX_EXT) { sc->bad = true; return false; }      /* starts a concurrent task */
    if (e->kind == EX_DYN) { sc->bad = true; return false; }      /* vtable dispatch: callee unresolved */
    if (exprIsCallKind(e) || (exprCarriesCallee(e) && planCallee(e))) {
        FuncDef *g = planCallee(e);
        if (!g || g->isExtern) { sc->bad = true; return false; }  /* unknown / C black box */
    }
    return rcScanChildren(NULL, e, sc);
}

/**
 * @brief Compute the least fixed point of `rcNoDec` over the whole call graph and store the result on
 *        the plan side.
 *
 * Must be called **after all function bodies are checked**: `FuncDef.callees` is only filled in by
 * then. Generic templates/instances are never marked (one AST slot is shared by all instances, so
 * holding for one proves nothing for another); default `false` = counted as usual.
 *
 * @param c  Checker.
 */
static void computeRcNoDec(Checker *c) {
    Module *m = c->m;
    if (!m || m->funcs.len == 0) return;
        /* (1) Self-judgement: writes a reference slot / unknown call / concurrency / generic ⇒ not marked */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        bool ok = f && f->body && !planTemplate(f) && f->typeParams.len == 0 && !f->isExtern;
        if (ok) {
            RcScan sc = { c, false };
            rcScanChildren(f->body, NULL, &sc);
            ok = !sc.bad;
        }
        planSetRcNoDec(f, ok);
    }
        /* (2) A callee that does not hold makes the caller not hold either; iterate to the fixed point
     * (mutual recursion converges on its own) */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (!planRcNoDec(f)) continue;
            for (size_t k = 0; k < f->callees.len; k++) {
                if (!planRcNoDec(*(FuncDef **)vecAt(&f->callees, k))) {
                    planSetRcNoDec(f, false);
                    changed = true;
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------
 * Region acyclicity: `planFnAcyclic` / `planFnRefsCounted` (two fixed points over the whole call
 * graph).
 *
 * Purpose: give "the event tier (counting) can replace sweeping" a **static premise**. Criterion v1
 * (soundness first: prefer conservative over a miss):
 *
 *   (i)   every store into a ref-typed field has a value that is an object **freshly allocated in this
 *         frame**: directly `new T` / `alloc<T>(n)`, or returned from one call whose callee is
 *         **acyclic** and whose return value is provably **freshly allocated** (`retFresh`);
 *   (ii)  the value is `null`;
 *   (iii) the value's level is **strictly greater** than its host's level (the value lives shorter ⇒
 *         this edge cannot be a back edge). Only shapes whose level lower bound is reliable get this
 *         conclusion (decided in `noteAcyclicStore`): using an over-large upper bound to let it through
 *         would be the criterion's opposite direction;
 *   (iv)  every place that can **drop a reference** is a **counted rc slot**: the function makes no
 *         non-declaration-initializing assignment to a reference-typed local or parameter (references
 *         live only in heap objects' ref fields), and it has no opaque call (an opaque call involving
 *         references ⇒ not qualified). This one is necessary -- with (i)(ii)(iii) but not (iv) a
 *         reference lives in a local, the object's `hold` never reaches zero, and the event tier frees
 *         nothing.
 *
 * The per-site classification of (i)(ii)(iii) happens during body checking (`noteAcyclicStore`: the
 * level comparison needs a scope); here only a syntax-level scan (finding (iv) violations, collecting
 * call dependencies) and the two fixed points are done:
 *   · `retFresh(F)`: every `return` in F hands out a freshly allocated object;
 *   · `acyc` / `iv` are tightened down the call graph (a mutually recursive set of functions can hold
 *     at the same time).
 * Generic templates and instances, externs and bodyless functions are never marked (default false =
 * keep sweeping, the safe direction).
 */

/* One call dependency: the callee (NULL = unresolved) and its source line, for diagnostics. */
typedef struct { FuncDef *fn; int line; } AcDep;

/* What one function's scan produced in this pass. */
typedef struct {
    Arena      *a;
    Vec         acycDeps;   /* AcDep*: SR_CALL sites ((i') needs the callee acyclic and returning fresh) */
    Vec         ivDeps;     /* AcDep*: call sites whose body is visible ((iv) requires the callee to hold too) */
    Vec         retDeps;    /* AcDep*: callees of `return <call>` (the retFresh closure) */
    bool        acycBad;    /* A site that (i)(ii)(iii)(i') does not support was found */
    bool        ivBad;      /* The local part of (iv) does not hold */
    const char *acycWhy;    /* Reason for the first (i)(ii)(iii)(i') failure */
    const char *ivWhy;      /* Reason for the first (iv) failure */
    /* ---- (2) The other pass: "reclaim has nothing to do" (criteria (a)(b)(c)) --------------
 *
 * **Independent** of the two passes above: the acyclicity criterion asks "does the reference graph
 * have a back edge", this one asks "is a live reference dropped early". Neither implies the other
 * (see `idleNote`), so the facts are stored and judged separately. */
    bool        idleBad;    /* A site that (a)(b)(c) does not support was found */
    const char *idleWhy;    /* First reason (`EXTC_DBG_RCIDLE=1` prints it directly) */
    Vec         idleFresh;  /* IdleFresh*: C name of `var x = new T` plus the allocation point's loop depth / arena level */
    Vec         idleAsgn;   /* const char*: C name of an assigned identifier (the host must not be among them) */
    Vec         idleAsgnLoop; /* int: parallel to idleAsgn -- the **innermost loop-body scope** of that assignment (0 = no loop) */
    Vec         idleWrites; /* IdleWrite*: every write of a value into a reference slot -- the criterion's only source of facts */
    Vec         idleBindLvl;   /* IdleLvl*: binding -> arena level of the storage it points to (the same-scale fact for (b')) */
    Vec         idleFieldLvl;  /* IdleLvl*: field name -> arena level of the value written into that field (shallowest wins) */
    Vec         idleFieldStores; /* IdleFieldStore*: raw values of field writes (input to the fixed-point solve) */
    Vec         idleOrig;    /* IdleOrig*: every value of `name` is a projection of `origin` (flavor ii) */
    Vec         idleKeep;    /* const char*: a kept root (objects held in earlier iterations are still reachable) */
    Vec         idleFreshSet; /* const char*: bindings whose every value is freshly allocated in this frame (or null) */
    int         idleSeq;    /* Statement number in the walk (from 1): decides "the allocation point precedes the site" */
} AcInfo;

/* The kinds of reference slot. The criterion looks only at these: no other shape has the
 * "overwrite drops a root" problem. */
typedef enum {
    IDLE_DEST_COMPOUND,     /* Compound assignment (`x += ...`): not an overwrite, but it drops a reference just the same */
    IDLE_DEST_SLOT_NAMED,   /* A named ref slot on the stack (local in scope): deferred to the (b') conjunction */
    IDLE_DEST_SLOT_OTHER,   /* Any other stack location (a field of a local aggregate, etc.): rejected outright */
    IDLE_DEST_HEAP_FIELD,   /* A named field of a named object: the per-site criterion for (a) */
} IdleDestKind;

/* Normalized description of one "write a value into a reference slot" -- the criterion's **only**
 * input.
 *
 * The three slot kinds (named stack slot, other stack location, heap field) each had their own
 * structure and ledger; they are merged into one, so adding a rule changes the criterion in one place
 * instead of adding another ledger.
 *
 * The field name is `fname`, not `field`: `field` is an AST node member name, and the AST-freeze
 * check is a text scan that would mistake this descriptor member assignment for "writing the AST
 * outside the parser".
 *
 * `host` is dual-purpose: for `HEAP_FIELD` it is the host name, for `SLOT_NAMED` the slot name (both
 * are compared by name in the criterion). `isAssign` separates "assignment site" from "declaration
 * initializer": the (b') conjunction looks at both, but candidate sites take only the assignment
 * half. `allocDepth` is meaningful only on heap-field sites (the arena level of the host's `new`,
 * for diagnostics). */
typedef struct {
    IdleDestKind kind;
    Expr        *value;
    const char  *host;       /* HEAP_FIELD: host name; SLOT_NAMED: slot name */
    const char  *fname;      /* HEAP_FIELD: field name */
    int          line;
    int          depth;      /* Loop depth of the site */
    int          allocDepth; /* Arena level of the host's `new` (for diagnostics) */
    int          loopScope;  /* Innermost loop-body scope level (threshold for (b')) */
    int          seq;        /* Walk sequence number (decides "the allocation point precedes the site") */
    int          condDepth;  /* How many conditional statements enclose this write (0 = it always runs this iteration) */
    bool         seqElem;    /* The outermost level is an element slot, and the index is an identifier assigned in this loop body */
    bool         isAssign;   /* This is an assignment site (not a declaration initializer) */
} IdleWrite;

/* The criterion's verdict: `IDLE_DEFER` = cannot answer at this stage (the facts are not complete
 * yet), judge again in the next stage. */
typedef enum { IDLE_ACC, IDLE_REJ, IDLE_DEFER } IdleJudgment;

/* The criterion's two stages: the walk stage (only syntax and declarations seen so far) and the
 * post-processing stage (the ledgers and the fixed point are complete). */
enum { IDLE_STAGE_WALK = 0, IDLE_STAGE_POST = 1 };

/* Ledger of "a local newly created in this frame": the candidate host for (a), and the basis for
 * (a')'s "the host is new this iteration".
 *
 * `depth` is the allocation point's loop depth + 1 (from 1: 0 is reserved for "not in the ledger");
 * `arenaLevel` is the arena level the storage of that `new` **finally lands in**
 * (`arenaDepthOf(planArenaLevel)`, after escape promotion). Both must be >= the site's loop depth:
 * (a) asks "at most one write per object", so loop nesting (`depth`) is the primary criterion; the
 * arena level narrows it (a host promoted to a longer-lived level is more conservative). `seq`
 * ensures the allocation precedes the site. */
typedef struct { const char *name; int depth; int arenaLevel; int seq; } IdleFresh;

/* Ledger of "which arena level the storage a name points to falls in" (refines (b')'s "same-scale
 * fact").
 *
 * The level number uses the **same scale** as `arenaDepthOf(planArenaLevel(new))` on the (a') side
 * (smaller = lives longer; 0 = this frame's home region / undecidable). When one name has several
 * entries, the **shallowest** (longest-lived) one wins, which is the conservative direction. `null`
 * never enters the ledger: it points at no object, so overwriting it loses nothing (see
 * `IDLE_LVL_NULL`). */
typedef struct { const char *name; int lvl; } IdleLvl;

/* One "write a reference into a named object's field": field reads (`p.next`) take from it "which
 * level this field may hold". One field name takes the shallowest (longest-lived) level, the
 * conservative direction. `seq` is the walk sequence number: refinement P must ask whether this field
 * write happens **before** the slot overwrite and nobody touched that slot in between. */
typedef struct { const char *fname; const char *host; Expr *value; int seq; } IdleFieldStore;

/* Sentinel level for "what this write drops is null (no object)": it is "longer-lived" than any real
 * arena level, yet by semantics it drops no object, so `idleStoreQualified` treats it as qualified
 * outright. */
#define IDLE_LVL_NULL 0x3fffffff

/* Walk context of pass (2): it carries the **loop nesting depth**, the **scope depth** and the
 * **scope level of the innermost loop body**.
 *
 * `depth` is the lexical state for criteria (a)/(a'): when a site is in a loop of depth d, only a
 * host created in this iteration satisfies "at most one write per object". `scope` is block nesting
 * depth (function body 1, +1 per block entered, including the loop body's block) -- the same scale as
 * arena levels (both are the checker's scope-stack depth). `minLoopScope` is **the shallowest scope
 * level among the loop bodies currently enclosing the site** (b' uses it as the threshold for "dies
 * with which release this iteration"); 0 = not inside any loop. The statement callback only receives
 * the child node, so all three numbers are passed down by the callbacks themselves. */
typedef struct { Checker *c; AcInfo *ac; int depth; int scope; int minLoopScope; int condDepth; } IdleWalk;

/* Scan context: the checker (type table) and this function's record. */
typedef struct { Checker *c; AcInfo *ac; } AcScan;

static bool acScanStmt(void *ctx, Stmt *s);
static bool acScanExpr(void *ctx, Expr *e);
static bool idleWalkStmtCb(void *ctx, Stmt *s);
static bool idleWalkExprCb(void *ctx, Expr *e);
static bool idleWalkStmt(Checker *c, AcInfo *ac, Stmt *s, int depth, int scope, int minLoopScope, int condDepth);
static bool idleWalkExpr(Checker *c, AcInfo *ac, Expr *e, int depth, int minLoopScope);
static bool idleWalkExprFlag(Checker *c, AcInfo *ac, Expr *e, int depth, bool bound, int minLoopScope);


/**
 * @brief Can this function yield a region-acyclicity verdict (it has a body, is not a generic body or
 *        instance, is not an extern)?
 *
 * A generic body (including methods whose **owner struct has type parameters**, such as
 * `vector<T>::grow`) is never marked: one AST slot is shared by all instances, and the template's
 * types are opaque here. "Cannot decide" is not "rejected": the call site applies the
 * parameter-sensitive rule (an argument carrying no reference ⇒ it cannot get our reference).
 *
 * @param g  Function definition.
 * @return   True when a verdict can be computed.
 */
static bool acAnalyzable(const FuncDef *g) {
    if (!g || !g->body || g->isExtern || planTemplate(g)) return false;
    if (g->typeParams.len > 0) return false;
    if (g->owner && g->owner->typeParams.len > 0) return false;
    return true;
}

/**
 * @brief Is this function a method of some `impl Trait for T` (codegen emits it for the vtable)?
 *
 * The mirror of `inTraitTable` in codegen: a method may be emitted because it appears in an
 * `impl Trait` even when nothing calls it by name, so `unitAllAcyclic` requires it to be judged. A
 * diagnostic must explain "why it did not take effect", so "which functions get emitted" is computed
 * from the same table.
 *
 * @param m  Module.
 * @param f  Function definition.
 * @return   True when it is in the trait table.
 */
static bool acTraitTable(const Module *m, const FuncDef *f) {
    if (!f || !f->owner) return false;
    for (size_t i = 0; i < m->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt((Vec *)&m->impls, i);
        if (!planImplTrait(im) || !planImplTarget(im)) continue;
        for (size_t j = 0; j < im->methods.len; j++) {
            FuncDef *mth = *(FuncDef **)vecAt(&im->methods, j);
            if (mth == f) return true;
            if (mth->name && f->name && strcmp(mth->name, f->name) == 0) return true;
        }
    }
    return false;
}

/**
 * @brief Will the code generator emit this function (the `collectEmitted` rule, so diagnostics line
 *        up with `unitAllAcyclic`)?
 *
 * `unitAllAcyclic` asks that **every emitted function** satisfy `planFnAcyclic`, and "emitted" is
 * wider than "called": extern declarations (a declaration in `planUsed` is emitted) and `impl Trait`
 * methods both count. A generic template is not emitted (its `T` is still a parameter, the generated
 * C would not compile); a template method of a generic struct counts only when one of its
 * **instances** is emitted.
 *
 * @param m  Module.
 * @param f  Function definition.
 * @return   True when it gets emitted.
 */
static bool acRequired(const Module *m, const FuncDef *f) {
    if (!f) return false;
    bool isMain = f->name && strcmp(f->name, "main") == 0;
    if (!planUsed(f) && !isMain && !f->isExport && !acTraitTable(m, f)) return false;
    if (f->typeParams.len > 0) return false;                 /* template: not emitted */
    if (!planTemplate(f) && f->owner && f->owner->typeParams.len > 0) return false;  /* template method of a generic struct */
    return true;
}

/**
 * @brief Why an emitted function has no recorded acyclicity verdict (the `unproven` line).
 *
 * @param g  Function definition.
 * @return   Static reason string.
 */
static const char *acUnprovenWhy(const FuncDef *g) {
    if (planTemplate(g)) return "a generic instance (not analysed)";
    if (g->isExtern) return "extern: emitted, but it has no body to analyse";
    if (!g->body) return "no body (a declaration only)";
    return "not analysed";
}


/**
 * @brief Does a **stack slot** (local, parameter, field of a local aggregate) hold a counted rc
 *        reference?
 *
 * Any `ref` counts: `ref i64` may point at a `new i64` rc object, and a local that drops it leaves
 * the object uncounted. A view handle does not count (`typeHoldsCountedRef`).
 *
 * @param c  Checker (type table).
 * @param t  Slot type; NULL counts as not holding.
 * @return   True when it holds one.
 */
static bool acSlotHoldsCountedRef(Checker *c, Type *t) {
    if (!t) return false;
    if (mentionsParam(t)) return true;
    return typeHoldsCountedRef(c->tt, tsub(c, t));
}

/**
 * @brief Does an **argument or receiver** hand a reference to a black box (black-box test only, one
 *        notch wider than the slot test)?
 *
 * Only a reference that can "hold" an rc object counts: `ref X` where `X` itself can hold references
 * (the black box may store it into a heap slot, and that object may hold further objects). A
 * reference to a scalar such as `ref i64` / `ref bool` does **not** count -- the call-site escape rule
 * already forbids the black box from keeping it, and it holds no rc object; I/O helpers such as
 * `fill(buf)` / `flush()` have exactly these in their signatures (`mut slice<u8>` + `mut ref i64`),
 * and judging them as failure would pin the whole program as cyclic.
 *
 * @param c  Checker (type table).
 * @param t  Argument or receiver type; NULL counts as carrying no reference.
 * @return   True when it may hand an rc reference to a black box.
 */
static bool acTypeMaybeRef(Checker *c, Type *t) {
    if (!t) return false;
    if (mentionsParam(t)) return true;
    Type *b = tsub(c, t);
    if (b && b->kind == TY_REF) return typeHoldsCountedRef(c->tt, b->inner);
    return typeHoldsCountedRef(c->tt, b);
}

/**
 * @brief Does a projection's base expression name a **heap object**?
 *
 * Judged from syntax and the node's type only (the later pass has no scope and cannot `lookup`): a
 * reference-typed binding as base ⇒ the host is the referent; a dereference ⇒ the host is the
 * referent; a plain binding as base ⇒ the host is on the stack. Criterion (iv) asks exactly "is the
 * slot on the heap".
 *
 * @param c    Checker (type table).
 * @param obj  Base expression of the projection.
 * @return     True when the base is a heap object.
 */
static bool acBaseIsHeap(Checker *c, Expr *obj) {
    if (!obj) return false;
    switch (obj->kind) {
    case EX_DEREF: return true;
    case EX_IDENT: {
        Type *t = obj->type ? tsub(c, obj->type) : NULL;
        return t && t->kind == TY_REF;
    }
    case EX_FIELD: return acBaseIsHeap(c, obj->u.field.obj);
    case EX_INDEX: return acBaseIsHeap(c, obj->u.index.obj);
    case EX_SLICE: return acBaseIsHeap(c, obj->u.slice.obj);
    default:       return false;
    }
}

/**
 * @brief Does the reference slot named by an assignment's destination live in a heap object?
 *
 * `EX_IDENT` (retargeting a binding of this frame) and "a projection whose root is a plain binding"
 * are both on the stack: the column where criterion (iv) fails.
 *
 * @param c  Checker (type table).
 * @param t  Write target.
 * @return   True when the slot lives in a heap object.
 */
static bool acTargetSlotIsHeap(Checker *c, Expr *t) {
    if (!t) return false;
    switch (t->kind) {
    case EX_DEREF: return true;
    case EX_FIELD: return acBaseIsHeap(c, t->u.field.obj);
    case EX_INDEX: return acBaseIsHeap(c, t->u.index.obj);
    case EX_SLICE: return acBaseIsHeap(c, t->u.slice.obj);
    default:       return false;
    }
}

/**
 * @brief Can a call site send a reference into a black box (an unanalyzable callee)?
 *
 * The call counts as "touching a reference" when an argument or the receiver may carry one: the
 * black box may store it anywhere, so "references live only in counted heap slots" does not hold.
 *
 * @param c  Checker (type table).
 * @param e  Call expression.
 * @return   True when a reference is involved.
 */
static bool acCallTouchesRef(Checker *c, Expr *e) {
    switch (e->kind) {
    case EX_METHOD:
        if (e->u.method.recv && acTypeMaybeRef(c, e->u.method.recv->type)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.method.args, i))->type)) return true;
        return false;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.call.args, i))->type)) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (acTypeMaybeRef(c, (*(Expr **)vecAt(&e->u.assoc.args, i))->type)) return true;
        return false;
    default: return false;
    }
}

/* ---- (2) "Reclaim has nothing to do": per-site scan for criteria (a)(b)(c) --------------
 *
 * Purpose: a static premise for not emitting the rc runtime at all. A **region** (= one arena level)
 * needs no reclaim iff no object inside it can **lose its last root while it is still alive**. Three
 * statically decidable conditions:
 *
 *   (a) each ref field of each heap object is **written at most once per object** (an initialization,
 *       not an overwrite). Judged by shape: the site's host is a local from `var x = new T` in this
 *       frame, that binding is never reassigned anywhere in the body, the site is **not inside any
 *       loop**, and the same (host declaration, field) has only **one** site in this function. The
 *       first two pin the host object to that `new` (each recursive activation has its own `x`, hence
 *       its own dynamic object); the third guarantees that site runs once per object.
 *   (b) a **local or parameter of reference type is not reassigned**: an assignment writing a
 *       reference into a stack slot (an identifier, a field of a local aggregate) drops a reference.
 *       **Declaring** a ref local in a nested block does not count -- its objects are freed wholesale
 *       with the block's arena (the `let t = bottomUp(d)` in `bt` is this case).
 *   (c) no operation drops a reference: `@overwrite` reuses the same slot, `domain` blocks, `ext` /
 *       `dyn` (unanalyzable concurrency and dispatch), and an **unanalyzable callee** carrying a
 *       reference (a black box may drop it). An analyzable callee is not followed here -- the verdict
 *       is conjoined over the whole unit (codegen's `unitReclaimIdle`), and each callee is judged on
 *       its own.
 *
 * **Relation to the acyclicity criteria (i)(ii)(iii)(iv)**: neither implies the other. (2) allows back
 * edges (`a.next = b  b.next = a` -- each site satisfies (a) on its own), while acyclicity allows
 * reference drops other than "one write per object" (the counts absorb them). The two criteria serve
 * two different mechanisms: acyclic → the event tier (counts replace sweeping); (2) → rc is never
 * emitted at all. (2)'s safety net is **bounded peak RSS** (a wrong verdict reclaims less ⇒ memory
 * grows, it does not dangle).
 *
 * **Failure direction**: any one condition failing ⇒ the function is rejected ⇒ the unit is rejected
 * ⇒ rc is emitted as usual (conservative).
 */

/** @brief Record (2)'s first rejection reason (only the first, same convention as (iv)). */
static void idleNote(AcInfo *ac, const char *why) {
    if (ac->idleBad) return;
    ac->idleBad = true;
    ac->idleWhy = why;
}

/** @brief The preferred spelling of `planCName`: fall back to the source name when it has no answer. */
static const char *idleBindName(const void *node, const char *fallback) {
    const char *n = planCName(node);
    return n ? n : fallback;
}

/**
 * @brief Does a type contain a **reclaim-managed reference slot** (the "aggregate carrying references"
 *        question of (2))?
 *
 * Same rule as `rcScanType` in codegen: a **view** (`slice<T>`, including nested in struct fields) is
 * not an rc object -- it points into arena/slab memory, dropping it frees no rc object; an enum
 * payload cannot be expanded statically ⇒ conservative; a generic argument containing a reference
 * slot ⇒ cannot enumerate ⇒ conservative; an unknown kind may hide references ⇒ conservative.
 *
 * `typeHoldsCountedRef` is not used directly: at the **struct field recursion** level it does not
 * apply the view rule, so value types that hold only a view, such as `reader` / `writer`, would be
 * misjudged as carrying references (that rejected stdlib's `readerOf` / `writerOf` / `inSync`, and
 * then the whole unit). The rule must match the rc accounting side: only slots that get **recorded by
 * `storeRef*`** count.
 *
 * @param tt     Type table.
 * @param t      Type to ask about; NULL is conservatively "yes".
 * @param depth  Recursion depth, guards type cycles.
 * @return       True when it has one (or cannot be expanded).
 */
static bool idleCarriesRef(TypeTable *tt, Type *t, int depth) {
    if (!t || depth > 16) return true;                       /* conservative */
    if (ttIsViewType(t)) return false;                       /* a view is not an rc object */
    switch (t->kind) {
    case TY_REF: return true;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return false;
    /* An `any` owns a copy of its bytes in the arena, never a counted object: dropping the slot
     * releases nothing, so it does not make this function non-idle. (`rcScanType` answers the
     * *other* question -- can a root table be enumerated -- and says "conservative" for `any`.) */
    case TY_ANY: return false;
    case TY_ENUM:
                /* Look at variant payloads one by one: **the question here is "does dropping this value
         * release an rc object"**, so answer per payload (the `rcScanType` side needs "can root
         * tables be enumerated", which forces conservatism). */
        if (!t->edef) return true;
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (idleCarriesRef(tt, payloadType(tt, t, va, i), depth + 1)) return true;
        }
        return false;
    case TY_ARRAY: return idleCarriesRef(tt, t->inner, depth + 1);
    case TY_STRUCT: {
        StructDef *sd = structOf(t);
        if (!sd) return true;
        for (size_t i = 0; i < sd->fields.len; i++)
            if (idleCarriesRef(tt, (*(FieldDef **)vecAt(&sd->fields, i))->type, depth + 1))
                return true;
        return false;
    }
    case TY_GENERIC: {
        if (!t->sdef) return true;
        for (size_t i = 0; i < t->targs.len; i++)
            if (idleCarriesRef(tt, *(Type **)vecAt(&t->targs, i), depth + 1)) return true;
        StructDef *sd = t->sdef;
        Vec *sp = (t->targs.len == sd->typeParams.len) ? &sd->typeParams : NULL;
        Vec *sa = sp ? &t->targs : NULL;
        for (size_t i = 0; i < sd->fields.len; i++)
            if (idleCarriesRef(tt, ttSubstitute(tt, (*(FieldDef **)vecAt(&sd->fields, i))->type, sp, sa),
                               depth + 1))
                return true;
        return false;
    }
    default: return true;                                    /* an unknown kind may hide references */
    }
}

/** @brief Is the name in that vector (small vector, linear search)? */
static bool idleNameIn(Vec *v, const char *nm) {
    if (!nm) return false;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), nm) == 0) return true;
    return false;
}

/**
 * @brief Outermost host binding of a write target (`t.f` / `(*p).f` / `h.a.f` / `(*p).a.f` → `t` /
 *        `p` / `h` / `p`).
 *
 * Walk outward along the **named** path only: a field takes its base, a dereference takes its operand
 * (which must be an identifier). An element, a slice, or a call result as base all answer NULL ⇒ the
 * site is rejected (conservative). Criterion (a) asks exactly "which `new` created this object", so
 * the host must be a binding.
 *
 * @param t  Write-target expression.
 * @return   Binding expression (`EX_IDENT`), or NULL.
 */
static Expr *idleHostIdent(Expr *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case EX_FIELD: return idleHostIdent(t->u.field.obj);
    case EX_INDEX: return idleHostIdent(t->u.index.obj);
    case EX_DEREF: {
        Expr *o = t->u.deref.operand;
        return (o && o->kind == EX_IDENT) ? o : NULL;
    }
    case EX_IDENT: return t;
    default:       return NULL;
    }
}

/**
 * @brief Is the statement a **loop** (loop depth increments)?
 *
 * All four loop spellings of extC (`while`, `loop`, `for (init; cond; step)`, `for x in …`) become
 * `ST_WHILE` in the parser (see `parseFor` / `parseWhile`), and `StmtKind` has no `ST_FOR` ⇒ only
 * this one kind has to be recognized here. It is a function so that "which kinds are loops" has a
 * single source.
 *
 * @param s  Statement.
 * @return   True when it is a loop.
 */
static bool idleLoopStmt(Stmt *s) {
    return s && s->kind == ST_WHILE;
}

/**
 * @brief Does the statement **open a new lexical scope** (scope depth increments)?
 *
 * Blocks only. A loop body is an `ST_BLOCK` in extC, so this covers it; no separate case is needed.
 *
 * @param s  Statement.
 * @return   True when it is a block.
 */
static bool idleScopedStmt(Stmt *s) {
    return s && s->kind == ST_BLOCK;
}

/**
 * @brief Refinement E: is this name assigned **inside the current loop body**?
 *
 * Element-slot sequences (`(*root)[i] = …`) are allowed because each iteration writes a **different
 * leaf slot**. That only holds when the index **advances with the loop**, so the index binding must be
 * assigned **in this loop body** (`i = i + 1`); an index fixed once outside the loop (`j = 0` then
 * `arr[j] = x`) is rejected here -- that is the case where the same slot is rewritten every iteration.
 *
 * @param ac           (2)'s facts for the current function.
 * @param name         C name of the index binding.
 * @param minLoopScope Innermost loop-body scope of the site.
 * @return             True when the name is assigned in that loop body.
 */
static bool idleNameAssignedInLoop(AcInfo *ac, const char *name, int minLoopScope) {
    if (!name || minLoopScope == 0) return false;
    for (size_t j = 0; j < ac->idleAsgn.len && j < ac->idleAsgnLoop.len; j++) {
        const char *nm = *(const char **)vecAt(&ac->idleAsgn, j);
        int sc = *(int *)vecAt(&ac->idleAsgnLoop, j);
        if (nm && strcmp(nm, name) == 0 && sc >= minLoopScope) return true;
    }
    return false;
}

/**
 * @brief Refinement A: does the overwritten old value have **another root**?
 *
 * When the same (host, leaf path) is written at two sites, the base rule rejects outright (fearing the
 * old value becomes garbage). But if the old value is **a projection of a parameter** (`prev.l` /
 * `*p` / `q[i]`) and this frame **writes no field of that host** (which could change that root), the
 * old value **stays reachable through that parameter** after the overwrite ⇒ the overwrite drops no
 * root, allow it.
 *
 * Where it is narrow: only "a projection of a parameter + this frame does not write that host"; not
 * "the two writes are mutually exclusive" (a separate rule, none in the corpus), nor a general proof
 * that "the old value is still alive" -- that needs interprocedural/alias analysis, which this frame
 * cannot provide.
 *
 * @param ac  (2)'s facts for the current function.
 * @param v   Value expression written by the overwritten site.
 * @return    True = the old value has another root (allow); false = cannot be proven (still reject).
 */
static bool idleValueOtherRoot(AcInfo *ac, Expr *v) {
    int guard = 0;
    while (v && (v->kind == EX_FIELD || v->kind == EX_DEREF || v->kind == EX_INDEX) && guard++ < 16)
        v = v->kind == EX_FIELD ? v->u.field.obj
          : (v->kind == EX_DEREF ? v->u.deref.operand : v->u.index.obj);
    if (!v || v->kind != EX_IDENT) return false;
    Sym *b = identBindOf(v);
    if (!b || b->depth != 0) return false;      /* parameters only (depth 0) */
    for (size_t j = 0; j < ac->idleFieldStores.len; j++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, j);
        if (fs->host && strcmp(fs->host, v->u.ident.name) == 0) return false;
    }
    return true;
}

/**
 * @brief Refinement B: the **leaf path** of a write destination (`s.kids[0]` → `kids[0]`,
 *        `(*root)[i]` → `[i]`).
 *
 * The B rule recognizes `EX_FIELD` (a named field of a named object). Here the destination is
 * evaluated **once** and recorded leaf by leaf: the path is the site key, and an index is accepted
 * only as an **integer literal** (`kids[0]` and `kids[1]` are different leaf slots); **non-literal
 * indices are accepted too** (the key is recorded as `base[]`) because two writes to the same leaf
 * slot are still judged by A (including refinement A) -- accepting an index does not let "the same
 * slot written twice" through.
 *
 * **A field at the root uses the bare field name** (`(*p).f` → `f`): the read side of the field
 * ledger (the `EX_FIELD` arm of `idleValueLvl`) looks up `field.name`, so the key must match; a
 * leading dot would not match and would reject the already-idle `rebuild` family.
 *
 * @param a  Arena for the path string.
 * @param t  Write-target expression.
 * @return   The path; NULL when the leaf slot cannot be determined (a base with a call, a slice, …)
 *           (the caller rejects).
 */
static const char *idleDestPath(Arena *a, Expr *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case EX_IDENT: return "";
    case EX_FIELD: {
        const char *base = idleDestPath(a, t->u.field.obj);
        if (!base) return NULL;
        return *base ? arenaPrintf(a, "%s.%s", base, t->u.field.name) : t->u.field.name;
    }
    case EX_INDEX: {
        const char *base = idleDestPath(a, t->u.index.obj);
        Expr *ix = t->u.index.index;
        if (!base || !ix) return NULL;
        if (ix->kind != EX_INT) return arenaPrintf(a, "%s[]", base);
        return arenaPrintf(a, "%s[%lld]", base, (long long)ix->u.ival);
    }
    case EX_DEREF: {
        Expr *o = t->u.deref.operand;
        return (o && o->kind == EX_IDENT) ? "" : NULL;
    }
    default: return NULL;
    }
}

/**
 * @brief Statement callback: continue down through the children at the current depths.
 *
 * The `astWalkStmtChildren` callback sees the **child** nodes, and the two depths are passed down by
 * the callback itself: entering a loop body increments loop depth (`idleLoopStmt`), entering a block
 * increments scope depth (`idleScopedStmt`).
 *
 * @param ctx  `IdleWalk *`; its `depth` / `scope` are the depths of the **child** node.
 * @param s    Child statement being visited.
 * @return     Always true.
 */
static bool idleWalkStmtCb(void *ctx, Stmt *s) {
    IdleWalk *w = ctx;
    return idleWalkStmt(w->c, w->ac, s, w->depth, w->scope, w->minLoopScope, w->condDepth);
}

/** @brief Expression callback: expressions do not change loop depth, descend unchanged. */
static bool idleWalkExprCb(void *ctx, Expr *e) {
    IdleWalk *w = ctx;
    return idleWalkExpr(w->c, w->ac, e, w->depth, w->minLoopScope);
}

/**
 * @brief Find a name in the "new locals of this frame" ledger (the candidate host for (a)).
 *
 * The ledger is a function-level linear list, and one name may have several entries (same-name
 * bindings in different branches are distinguished by their C names). `seq` guarantees the allocation
 * is **before** the site (a write cannot precede the `new` of its host). For one name take the entry
 * with the **deepest loop** (the most favorable one), but it must still satisfy the ordering itself.
 * Returns NULL when absent.
 *
 * @param ac    This function's record.
 * @param name  C name of the host.
 * @param seq   Walk sequence number of the site.
 * @return      The most favorable ledger entry, or NULL.
 */
static IdleFresh *idleFreshAt(AcInfo *ac, const char *name, int seq) {
    if (!name) return NULL;
    IdleFresh *best = NULL;
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (strcmp(fr->name, name) != 0) continue;
        if (fr->seq >= seq) continue;                  /* the allocation must precede the site */
        if (!best || fr->depth > best->depth) best = fr;
    }
    return best;
}

/**
 * @brief Arena level recorded for a name (several entries for one name take the **shallowest** =
 *        longest-lived one, the conservative direction).
 *
 * @param v     Ledger (a vector of `IdleLvl *`).
 * @param name  Name.
 * @return      Level number; -1 when unrecorded or unknown.
 */
static int idleLvlName(Vec *v, const char *name) {
    if (!name) return -1;
    int best = -1;
    for (size_t i = 0; i < v->len; i++) {
        IdleLvl *e = *(IdleLvl **)vecAt(v, i);
        if (strcmp(e->name, name) != 0) continue;
        if (e->lvl < 0) return -1;                 /* unknown wins (conservative) */
        if (best < 0 || e->lvl < best) best = e->lvl;
    }
    return best;
}

/**
 * @brief Merge one "the storage this name points to is at that level" entry into the ledger.
 *        Several entries for one name take the shallowest; any unknown ⇒ unknown.
 *
 * `null` (`IDLE_LVL_NULL`) never enters the ledger: it points at no object.
 *
 * @param v     Ledger.
 * @param a     Arena for allocation.
 * @param name  Name.
 * @param lvl   Level (-1 = unknown).
 */
static void idleLvlMerge(Vec *v, Arena *a, const char *name, int lvl) {
    if (!name) return;
    if (lvl == IDLE_LVL_NULL) return;
    for (size_t i = 0; i < v->len; i++) {
        IdleLvl *e = *(IdleLvl **)vecAt(v, i);
        if (strcmp(e->name, name) != 0) continue;
        if (lvl < 0) { e->lvl = -1; return; }
        if (e->lvl < 0) return;                    /* unknown wins */
        if (lvl < e->lvl) e->lvl = lvl;
        return;
    }
    IdleLvl *e = arenaAllocZero(a, sizeof *e);
    e->name = name;
    e->lvl  = lvl;
    *(IdleLvl **)vecPush(v) = e;
}

/**
 * @brief Which arena level the written reference value lands in (the same-scale fact for refinement
 *        (b')).
 *
 * One scale only: `arenaDepthOf(planArenaLevel(...))` -- the same number as on the (a') side and as
 * the escape analysis' own placement (smaller = lives longer, 0 = this frame's home region /
 * undecidable).
 *   · `null`: `IDLE_LVL_NULL` (points at no object);
 *   · `new`/`alloc`: the level that allocation site finally lands in (after escape promotion);
 *   · identifier: look up the **binding ledger** (computed by `idleSolveLvls` from all writes in
 *     this function);
 *   · field read: look up the **field ledger** (the shallowest level among all values written into
 *     that field in this function).
 * Other shapes (call results, `*p`, indices, aggregate literals, …) answer -1 ⇒ the caller rejects.
 *
 * **Not used**: `anRefDepth` / `Sym.fields[].depth` / lexical nesting counts / `lookup()->depth` --
 * those scales disagree with arena levels (for `rebuild`'s `h = c` they would give 1 / 3 / 6 / 4
 * while the arena level is 3).
 *
 * @param bind   Binding ledger.
 * @param field  Field ledger.
 * @param v      Written value.
 * @return       Level number (`IDLE_LVL_NULL` for null); -1 when undecidable.
 */
static int idleValueLvl(Vec *bind, Vec *field, Expr *v) {
    if (!v) return -1;
    if (v->kind == EX_NULL) return IDLE_LVL_NULL;
    if (v->kind == EX_NEW || v->kind == EX_GENCALL)
        return arenaDepthOf(planArenaLevel(v));
    if (v->kind == EX_IDENT)
        return idleLvlName(bind, idleBindName(v, v->u.ident.name));
    if (v->kind == EX_FIELD)
        return idleLvlName(field, v->u.field.name);
    return -1;
}

/**
 * @brief Does one write's "dropped thing" **die with this iteration's arena** (the only threshold of
 *        refinement (b'))?
 *
 * The real condition: what is written is `null` (drops no object), or its storage level is not
 * longer-lived than **the innermost loop body containing this write** (`lvl >= loopScope`). The latter
 * says "this object's arena is freed in or at this iteration", so overwriting the slot later just
 * loses one root and accumulates no garbage whose bound grows with the iteration count. Outside any
 * loop (`loopScope == 0`) or an undecidable level (`lvl < 0`) ⇒ reject (conservative).
 *
 * @param lvl        Arena level of the written value.
 * @param loopScope  Innermost loop-body scope level of the write site.
 * @return           True when qualified.
 */
static bool idleStoreQualified(int lvl, int loopScope) {
    if (lvl == IDLE_LVL_NULL) return true;
    if (lvl < 0) return false;
    return loopScope > 0 && lvl >= loopScope;
}

/** @brief Are two ledgers identical entry by entry (name + level)? Used for fixed-point convergence. */
static bool idleLvlSame(Vec *x, Vec *y) {
    if (x->len != y->len) return false;
    for (size_t i = 0; i < x->len; i++) {
        IdleLvl *a = *(IdleLvl **)vecAt(x, i);
        IdleLvl *b = *(IdleLvl **)vecAt(y, i);
        if (strcmp(a->name, b->name) != 0 || a->lvl != b->lvl) return false;
    }
    return true;
}

/**
 * @brief Compute the **least fixed point** of "which arena level the storage a binding/field points
 *        to falls in" (the facts behind refinement (b')).
 *
 * Why iterate: a written value may depend on the levels of other bindings/fields, and the walk order
 * does not guarantee "defined before used" -- `rebuild`'s `c.next = h` comes **before** `h = c`, and
 * `p = p.next` comes after two loops. Recording once in walk order would yield "undecidable" and
 * reject an idle shape.
 *
 * How: each round recomputes from the **previous** round's ledgers (`idleValueLvl` reads only the
 * previous round), until the ledgers match entry by entry or the round limit is reached. The direction
 * is monotone: undecidable can only become known, and a known level only takes smaller values ⇒ only
 * more conservative. The limit is 8 rounds (an ordinary chain converges in two or three); still
 * undecidable at the limit ⇒ the reading side answers -1 ⇒ reject.
 *
 * @param ac  This function's record; results are written back to `idleBindLvl` / `idleFieldLvl`.
 */
static void idleSolveLvls(AcInfo *ac) {
    for (int round = 0; round < 8; round++) {
        Vec nb, nf;
        vecInit(&nb, ac->a, sizeof(void *));
        vecInit(&nf, ac->a, sizeof(void *));
        for (size_t i = 0; i < ac->idleWrites.len; i++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
            if (w->kind != IDLE_DEST_SLOT_NAMED) continue;
            idleLvlMerge(&nb, ac->a, w->host,
                         idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, w->value));
        }
        for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
            IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
            idleLvlMerge(&nf, ac->a, fs->fname,
                         idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, fs->value));
        }
        if (idleLvlSame(&nb, &ac->idleBindLvl) && idleLvlSame(&nf, &ac->idleFieldLvl)) break;
        ac->idleBindLvl  = nb;
        ac->idleFieldLvl = nf;
    }
}

/* Source fact of flavor (ii): every value of `name` is a projection of `origin` along **stable
 * fields**. Solved as a greatest fixed point (deletion only): all pairs start in the table, and only
 * those refuted by "one of the writes is not a projection" are removed. */
typedef struct { const char *name; const char *origin; } IdleOrig;

/** @brief Declaration of refinement P (flavor i) (defined below). */
static bool idleLinksOld(AcInfo *ac, const IdleWrite *w);

/**
 * @brief Refinement P (flavor ii): this slot overwrite **drops no root** -- every value the slot can
 *        hold is a projection of some "kept root".
 *
 * Semantics: cursors such as `cur = (*cur).next`. The old value is not dropped: it is still a node of
 * `head` (or another kept root) along the `next` chain, which is **only initialized, never
 * retargeted**. Three things must hold:
 *
 *   1. **every value in the slot is a projection of the same root `origin`** (table `ac->idleOrig`,
 *      computed by greatest fixed point over "every write of this name is a projection of `origin`");
 *   2. `origin` is a **kept root** (`ac->idleKeep`: the objects it has held over all iterations are
 *      still reachable -- every non-null write either gets chained by the new value (refinement P), or
 *      is its own projection, or is the single `new` initialization that is never assigned again);
 *   3. the fields along the projection are **stable fields** (`idleFieldStable`: every host writing
 *      one is a fresh allocation of this frame ⇒ it can only be created, never retargeted).
 *
 * The three together: the old value is still reachable from `origin` along a path that is never cut ⇒
 * overwriting this slot drops no root. Conservative direction: failing any one ⇒ does not hold
 * (reject).
 *
 * @param ac  This function's record.
 * @param w   This slot write.
 * @return    True when it holds.
 */
static bool idleProjRetained(AcInfo *ac, const IdleWrite *w) {
    if (!w->host) return false;
    for (size_t i = 0; i < ac->idleOrig.len; i++) {
        IdleOrig *o = *(IdleOrig **)vecAt(&ac->idleOrig, i);
        if (!o->name || strcmp(o->name, w->host) != 0) continue;
        for (size_t j = 0; j < ac->idleKeep.len; j++) {
            const char *k = *(const char **)vecAt(&ac->idleKeep, j);
            if (k && strcmp(k, o->origin) == 0) return true;
        }
    }
    return false;
}


/** @brief Does the table contain this (name, origin) projection pair? */
static bool idleOrigHas(Vec *orig, const char *name, const char *origin) {
    for (size_t i = 0; i < orig->len; i++) {
        IdleOrig *o = *(IdleOrig **)vecAt(orig, i);
        if (o->name && o->origin && strcmp(o->name, name) == 0 && strcmp(o->origin, origin) == 0)
            return true;
    }
    return false;
}

/** @brief Is the name in the kept-root set? */
static bool idleKeepHas(Vec *keep, const char *name) {
    for (size_t i = 0; i < keep->len; i++) {
        const char *k = *(const char **)vecAt(keep, i);
        if (k && strcmp(k, name) == 0) return true;
    }
    return false;
}


/** @brief The host was **advanced** in the loop of the site: one write happens at the same loop depth
 *  or deeper and stores an object newly allocated in this frame. Without it, "the host object is new"
 *  only means it was created once **outside** the loop -- the loop then keeps writing the **same**
 *  object (`fixed_slot_overwrite`: peak RSS grows with the iteration count). */
static bool idleAdvancesInLoop(AcInfo *ac, const IdleWrite *w) {
    if (!w->host) return false;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, w->host) != 0) continue;
        if (o->depth < w->depth) continue;                       /* must be in the same loop or deeper */
        if (!o->value) continue;
        if (o->value->kind == EX_NEW && !o->value->u.new_.count) return true;
        if (o->value->kind == EX_IDENT) {
            const char *vn = idleBindName(o->value, o->value->u.ident.name);
            if (vn && idleNameIn(&ac->idleFreshSet, vn)) return true;
        }
    }
    return false;
}

/** @brief This field name has only one write site in this function.
 *  The host facts of refinement P need it: two cursors with **different names** (`tail` / `prev2`) may
 *  be the same object, which "same (host, field) at two sites" (rule A) cannot see by name. */
static bool idleLoneFieldSite(AcInfo *ac, const char *fname) {
    if (!fname) return false;
    size_t n = 0;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (fs->fname && strcmp(fs->fname, fname) == 0) n++;
    }
    return n == 1;
}

/** @brief Is the field stable: every host object writing it is **freshly allocated in this frame**
 *  (it can only be created, never retargeted). "The host object is new" uses the **same fact** as the
 *  E/C/B host clauses (see `idleHostFresh`). */
static bool idleFieldStable(AcInfo *ac, const char *fname) {
    if (!fname) return false;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->fname || strcmp(fs->fname, fname) != 0) continue;
        if (idleFreshAt(ac, fs->host, fs->seq)) continue;
        if (idleNameIn(&ac->idleFreshSet, fs->host) && idleLoneFieldSite(ac, fs->fname)) continue;
        return false;
    }
    return true;
}

/** @brief L1 "linked into the structure": is the object `vname` points to **linked into `origin`'s
 *  structure** by some field write.
 *
 * The transitivity rule from the formal specification §3: for `h.f := v`, if `h` is a projection of
 * `origin` and field `f` is stable ⇒ the object `v` is reachable from `origin` ⇒ it becomes a
 * projection of `origin`. Without it the fixed point is smaller than `A*` (`rg_grow_tail`'s
 * `tail = p` is this rule). */
static bool idleLinkedInto(AcInfo *ac, Vec *orig, const char *vname, const char *origin,
                           int seq) {
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->value || fs->value->kind != EX_IDENT) continue;
        const char *vn = idleBindName(fs->value, fs->value->u.ident.name);
        if (!vn || strcmp(vn, vname) != 0) continue;
        if (seq > 0 && fs->seq >= seq) continue;                 /* the link happens before this write */
        if (!idleFieldStable(ac, fs->fname)) continue;
        if (fs->host && idleOrigHas(orig, fs->host, origin)) return true;
    }
    return false;
}


/** @brief Is `v` a projection of `origin` along stable fields (a self reference is assumed to hold,
 *  for the greatest fixed point)? `seq` = walk sequence number of this write (for L1's "the link
 *  happens first"; 0 = no ordering required). */
static bool idleProjOf(AcInfo *ac, Vec *orig, Vec *keep, Expr *v, const char *origin,
                       const char *self, int seq) {
    if (!v) return false;
    if (v->kind == EX_NULL) return true;                    /* points at no object ⇒ breaks no projection invariant */
    if (v->kind == EX_IDENT) {
        const char *n = idleBindName(v, v->u.ident.name);
        if (!n) return false;
        if (strcmp(n, origin) == 0) return idleKeepHas(keep, origin);
        if (self && strcmp(n, self) == 0) return true;      /* self reference: assume it holds */
        if (idleOrigHas(orig, n, origin)) return true;
        return idleLinkedInto(ac, orig, n, origin, seq);     /* L1: linked into origin's structure */
    }
    if (v->kind == EX_FIELD || v->kind == EX_DEREF) {
        Expr *base = (v->kind == EX_FIELD) ? v->u.field.obj : v->u.deref.operand;
        const char *fld = (v->kind == EX_FIELD) ? v->u.field.name : "[]";
        if (!idleFieldStable(ac, fld)) return false;
        return idleProjOf(ac, orig, keep, base, origin, self, seq);
    }
    return false;                                           /* `new`/call result/index: a projection of nothing */
}


/** @brief Is `r` a kept root (the objects it has held over all iterations are still reachable)? */
static bool idleRetains(AcInfo *ac, Vec *orig, Vec *keep, const char *r) {
    size_t nonzero = 0;
    bool loneNew = false;
    int declDepth = 0;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, r) != 0) continue;
        if (!o->value || o->value->kind == EX_NULL) continue;
        nonzero++;
        loneNew = (nonzero == 1) && (o->value->kind == EX_NEW) && !o->isAssign;
        declDepth = o->depth;
    }
    if (nonzero == 0) return false;                         /* never assigned: not a dependable root */
    /* Only one `new` declaration initializer ⇒ the object it holds always has a root. **But it must not
 * be in any loop**: a binding created per iteration leaves the old instance held by nobody after its
 * scope exits (its storage may even be promoted) ⇒ it accumulates. */
    if (nonzero == 1 && loneNew && declDepth == 0) return true;
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, r) != 0) continue;
        if (!o->value || o->value->kind == EX_NULL) continue;
        if (idleLinksOld(ac, o)) continue;                  /* chained by the new value ⇒ the old value is still reachable */
        if (idleProjOf(ac, orig, keep, o->value, r, r, o->seq)) continue;   /* still on its own structure */
        return false;
    }
    return true;
}

/**
 * @brief Compute the **greatest fixed point** of flavor (ii)'s two tables (`idleOrig` / `idleKeep`).
 *
 * Monotone tightening (deletion only): all candidate pairs and candidate roots start in place, then
 * each round removes pairs refuted by some write and roots that are not retained, until the
 * fixed point is reached. Round limit 8 (an ordinary chain converges in two or three). The tables are
 * built from `idleWrites` and `idleFieldStores` at function scale (names × names); nothing is
 * extrapolated across functions.
 *
 * @param ac  This function's record; results are written back to `idleOrig` / `idleKeep`.
 */
static void idleSolveProj(AcInfo *ac) {
    Vec names;
    vecInit(&names, ac->a, sizeof(const char *));
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
        if (!idleKeepHas(&names, w->host)) *(const char **)vecPush(&names) = w->host;
    }
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (fr->name && !idleKeepHas(&names, fr->name))
            *(const char **)vecPush(&names) = fr->name;
    }
    for (size_t i = 0; i < names.len; i++) {
        const char *n = *(const char **)vecAt(&names, i);
        for (size_t j = 0; j < names.len; j++) {
            const char *o = *(const char **)vecAt(&names, j);
            IdleOrig *p = arenaAllocZero(ac->a, sizeof *p);
            p->name = n;
            p->origin = o;
            *(IdleOrig **)vecPush(&ac->idleOrig) = p;
        }
        *(const char **)vecPush(&ac->idleKeep) = n;
    }
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        Vec next, keep;
        vecInit(&next, ac->a, sizeof(void *));
        vecInit(&keep, ac->a, sizeof(void *));
        for (size_t i = 0; i < ac->idleOrig.len; i++) {
            IdleOrig *p = *(IdleOrig **)vecAt(&ac->idleOrig, i);
            bool ok = true;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
                if (strcmp(w->host, p->name) != 0) continue;
                if (!idleProjOf(ac, &ac->idleOrig, &ac->idleKeep, w->value, p->origin, p->name, w->seq))
                    ok = false;
            }
            if (ok) *(IdleOrig **)vecPush(&next) = p;
            else changed = true;
        }
        for (size_t i = 0; i < ac->idleKeep.len; i++) {
            const char *k = *(const char **)vecAt(&ac->idleKeep, i);
            if (idleRetains(ac, &next, &ac->idleKeep, k)) *(const char **)vecPush(&keep) = k;
            else changed = true;
        }
        ac->idleOrig = next;
        ac->idleKeep = keep;
        if (!changed) break;
    }
}

/**
 * @brief Refinement P (flavor i): this slot overwrite **drops no root** -- the new value written in
 *        already chains the old one.
 *
 * Shape (head insert / tail insert / bucket insert): `v.f = <the value the slot holds right now>`
 * happens first, then `slot = v`. The old value is reachable via `v.f` and `v` is written into the
 * slot ⇒ the overwrite cuts no path. Independent of the slot kind (it holds for stack slots and field
 * writes) and an **independent** qualification path from (b')'s "the value dies with this iteration".
 *
 * **The B dimension is required** (omitting it makes the rule unsound): this store must **always run
 * in this iteration** (`condDepth == 0`). Otherwise there are shapes where "only some iterations
 * store the new value" -- `scale_growth`'s `if i == n-1 { saved = p }`: the instances not stored are
 * promoted to the home region and dropped when the scope exits, so peak RSS grows with the iteration
 * count. **The A dimension (the old value still has a root) cannot replace the B dimension.**
 *
 * Other conditions: the written thing is an **identifier** `v` (not a field read, a call result, or
 * another unclear source); the ledger has a field write whose host is `v` and whose value is **this
 * same slot name**; that field write precedes the overwrite; and the slot was not written in between
 * (otherwise what got chained is an earlier value, not the one this overwrite drops).
 *
 * @param ac  This function's record.
 * @param w   This slot write.
 * @return    True when it holds.
 */
static bool idleLinksOld(AcInfo *ac, const IdleWrite *w) {
    if (w->condDepth != 0) return false;
    if (!w->value || w->value->kind != EX_IDENT || !w->host) return false;
    const char *v = idleBindName(w->value, w->value->u.ident.name);
    if (!v) return false;
    for (size_t i = 0; i < ac->idleFieldStores.len; i++) {
        IdleFieldStore *fs = *(IdleFieldStore **)vecAt(&ac->idleFieldStores, i);
        if (!fs->host || strcmp(fs->host, v) != 0) continue;      /* the host is that new value */
        if (fs->seq >= w->seq) continue;                          /* the field write precedes the overwrite */
        if (!fs->value || fs->value->kind != EX_IDENT) continue;
        const char *held = idleBindName(fs->value, fs->value->u.ident.name);
        if (!held || strcmp(held, w->host) != 0) continue;        /* exactly this slot is chained */
        bool clean = true;                                        /* nobody wrote this slot in between */
        for (size_t j = 0; j < ac->idleWrites.len && clean; j++) {
            IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
            if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host) continue;
            if (strcmp(o->host, w->host) != 0) continue;
            if (o->seq > fs->seq && o->seq < w->seq) clean = false;
        }
        if (clean) return true;
    }
    return false;
}

/**
 * @brief Compute the greatest fixed point of "every value of which bindings is **freshly allocated in
 *        this frame** (or `null`)".
 *
 * This is refinement P's **host fact**: the E/C/B host clauses and "field stable" all ask the same
 * question -- "is the object this write stores one just created in this frame". A field that was
 * never written ⇒ writing it drops no root and leaves no old value either.
 *
 * The relation is transitive (`prev2 = tail`, and every value of `tail` is a fresh allocation) ⇒
 * iterate to the fixed point. Deletion only: all candidates start in place, and only names refuted by
 * some write are removed. Candidates = names ever written as a named slot ∪ names fresh in this
 * frame. Round limit 8 (same convention as the other ledgers).
 *
 * @param ac  This function's record; results are written back to `idleFreshSet`.
 */
static void idleSolveFreshSet(AcInfo *ac) {
    Vec cand;
    vecInit(&cand, ac->a, sizeof(const char *));
    for (size_t i = 0; i < ac->idleWrites.len; i++) {
        IdleWrite *w = *(IdleWrite **)vecAt(&ac->idleWrites, i);
        if (w->kind != IDLE_DEST_SLOT_NAMED || !w->host) continue;
        if (!idleNameIn(&cand, w->host)) *(const char **)vecPush(&cand) = w->host;
    }
    for (size_t i = 0; i < ac->idleFresh.len; i++) {
        IdleFresh *fr = *(IdleFresh **)vecAt(&ac->idleFresh, i);
        if (fr->name && !idleNameIn(&cand, fr->name)) *(const char **)vecPush(&cand) = fr->name;
    }
    ac->idleFreshSet = cand;
    for (int round = 0; round < 8; round++) {
        Vec next;
        vecInit(&next, ac->a, sizeof(void *));
        bool changed = false;
        for (size_t i = 0; i < ac->idleFreshSet.len; i++) {
            const char *n = *(const char **)vecAt(&ac->idleFreshSet, i);
            bool ok = true, any = false;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (o->kind != IDLE_DEST_SLOT_NAMED || !o->host || strcmp(o->host, n) != 0) continue;
                any = true;
                if (!o->value || o->value->kind == EX_NULL) continue;
                if (o->value->kind == EX_GENCALL) continue;
                if (o->value->kind == EX_NEW) {
                    if (o->value->u.new_.count) { ok = false; break; }   /* with a count = view/array */
                    continue;
                }
                if (o->value->kind == EX_IDENT) {
                    const char *vn = idleBindName(o->value, o->value->u.ident.name);
                    if (vn && (strcmp(vn, n) == 0 || idleNameIn(&ac->idleFreshSet, vn)
                               || idleFreshAt(ac, vn, o->seq)))
                        continue;
                }
                ok = false;
            }
            if (ok && any) *(const char **)vecPush(&next) = n;
            else changed = true;
        }
        ac->idleFreshSet = next;
        if (!changed) break;
    }
}


/**
 * @brief (2)'s **single criterion**: does writing a value into a reference slot here make garbage
 *        unbounded?
 *
 * **The criterion has two dimensions** (a negative result for flavor (i) pins this down: judging only
 * the first dimension rejects accumulation in shapes such as `if i == n-1 { slot = p }`):
 *
 *   · **dimension A (drops no root)** -- overwriting a slot drops no root iff the old value is still
 *     reachable from **another** live root;
 *   · **dimension B (does not accumulate)** -- the **storage** of the value written into the slot or
 *     of the overwritten value is not left ownerless in later iterations.
 *
 * The rules below are a sound sufficient approximation of the two dimensions, judged in order -- the
 * order is the "first matching reason", so the order is part of the behavior.
 *
 * Walk stage (`IDLE_STAGE_WALK`, only syntax and declarations seen so far):
 *   1. compound assignment ⇒ reject (**A**: the old value is rewritten in place);
 *   2. named stack slot ⇒ defer: (b') wants "**every** value written into this slot qualifies", which
 *      is only fully visible once the whole walk is done;
 *   3. another stack location ⇒ reject (**A**: a reference is written into an untracked stack
 *      location);
 *   4. heap field: the target is not "a named field of a named object" ⇒ reject;
 *   5. heap field: the site is in a loop and the host is not created in this iteration and is not an
 *      element slot assigned in this loop body ⇒ reject (refinement E; **B**: a host from an earlier
 *      iteration gets overwritten repeatedly);
 *   6. another heap field ⇒ defer to post-processing (the per-site part of (a)).
 * Post-processing stage (`IDLE_STAGE_POST`, the ledgers and fixed points are complete):
 *   7. heap-field site: the host is not a `new` of this frame ⇒ reject (**B**: writing into another
 *      object accumulates); the host is reassigned ⇒ reject (**B**: the host may be swapped for
 *      another object, defeating the "one write per object" argument); an **earlier** site for the
 *      same (host, field) exists and the old value has no other root ⇒ reject (refinement A; **A**:
 *      the old value was overwritten a second time);
 *   8. named-stack-slot site: the (b') conjunction -- every value written into this slot (including
 *      the declaration initializer) must qualify (**B**: the value dies with this iteration's arena ⇒
 *      nothing accumulates).
 *
 * Failure direction: any rejection ⇒ the function is rejected ⇒ the unit is rejected ⇒ the reclaim
 * runtime is emitted as usual (conservative).
 *
 * @param ac     This function's record (the only source of facts for the criterion).
 * @param w      Normalized description of this write.
 * @param idx    Index of `w` in `ac->idleWrites` (for "earlier site" in post-processing; 0 in the
 *               walk stage).
 * @param why    Rejection reason. The wording is fixed: the blocker-classification tool keys rules
 *               off it.
 * @param stage  `IDLE_STAGE_WALK` / `IDLE_STAGE_POST`.
 * @return       `IDLE_ACC` qualifies; `IDLE_REJ` rejected (`*why` filled); `IDLE_DEFER` judge in the
 *               other stage.
 */
static IdleJudgment idleWriteOk(AcInfo *ac, const IdleWrite *w, size_t idx,
                                const char **why, int stage) {
    if (stage == IDLE_STAGE_WALK) {
        if (w->kind == IDLE_DEST_COMPOUND) {
            *why = arenaPrintf(ac->a,
                "line %d: a compound assignment to a reference slot overwrites it", w->line);
            return IDLE_REJ;
        }
        if (w->kind == IDLE_DEST_SLOT_NAMED) return IDLE_DEFER;
        if (w->kind == IDLE_DEST_SLOT_OTHER) {
            *why = arenaPrintf(ac->a,
                "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one)",
                w->line);
            return IDLE_REJ;
        }
        if (!w->host || !w->fname || !*w->fname) {
            *why = arenaPrintf(ac->a,
                "line %d: the destination is not a named field of a named object", w->line);
            return IDLE_REJ;
        }
                /* Whether the host is "an object created in this frame" is only fully decidable after the
         * whole walk (`idleFreshSet` is a fixed point) ⇒ only syntax is filtered here; E (a field
         * write inside a loop) and C (the host is not created in this frame) are deferred to
         * post-processing and judged from the same fact. */
        return IDLE_DEFER;
    }
    if (w->kind == IDLE_DEST_SLOT_NAMED) {
                /* (b'): **every** value written into this slot must qualify -- one name may have several
         * writes, and one bad write rejects every overwrite of this slot (conservative). Two
         * **independent** qualification paths: the value dies with this iteration ((b'), dimension B),
         * or this overwrite is chained by the new value and always runs in this iteration
         * (refinement P / flavor i, dimensions A + B). */
        if (idleStoreQualified(idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, w->value),
                               w->loopScope)
            || idleLinksOld(ac, w)) {
            bool ok = true;
            for (size_t j = 0; ok && j < ac->idleWrites.len; j++) {
                IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
                if (o->kind != IDLE_DEST_SLOT_NAMED) continue;
                if (!o->host || !w->host || strcmp(o->host, w->host) != 0) continue;
                if (!idleStoreQualified(idleValueLvl(&ac->idleBindLvl, &ac->idleFieldLvl, o->value),
                                        o->loopScope)
                    && !idleLinksOld(ac, o)) ok = false;
            }
            if (ok) return IDLE_ACC;
        }
        if (idleProjRetained(ac, w)) return IDLE_ACC;   /* flavor (ii): every slot value is a projection of a kept root */
        *why = arenaPrintf(ac->a,
            "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one)",
            w->line);
        return IDLE_REJ;
    }
        /* Post-processing of a heap-field site: (E)/(C)/(B)/(A). All four questions rest on the **same
     * fact** -- "is the host object of this write one just created in this frame" (`idleHostFresh`):
     *   · E: (a') -- created in **this iteration** (allocation depth and arena level both); the host
     *     fact of refinement P adds another path (every value of the host binding is fresh in this
     *     frame + this field has a single site);
     *   · C: relaxed to "this frame" (the name is in the ledger, or the host fact of refinement P);
     *   · B: the host binding is reassigned ⇒ "one write per object" does not hold by name alone;
     *     but if the refinement-P path applies (the host object is new every time), the reassignment
     *     itself is not a problem.
     * Rule A only looks at **earlier** sites, so this is judged in record order. */
    IdleFresh *fr = idleFreshAt(ac, w->host, w->seq);
    bool hostThisIter = fr && fr->depth - 1 >= w->depth && fr->arenaLevel >= w->depth;
    bool hostCursor = idleNameIn(&ac->idleFreshSet, w->host) && idleLoneFieldSite(ac, w->fname)
                      && idleAdvancesInLoop(ac, w);
    if (w->depth > 0 && !hostThisIter && !hostCursor && !w->seqElem) {
        *why = arenaPrintf(ac->a,
            "line %d: a reference field is written inside a loop, so one object can be overwritten",
            w->line);
        return IDLE_REJ;
    }
    if (!idleFreshAt(ac, w->host, w->seq) && !hostCursor && !w->seqElem) {
        *why = arenaPrintf(ac->a,
            "line %d: the host object is not a local created by `new` in this frame", w->line);
        return IDLE_REJ;
    }
    if (idleNameIn(&ac->idleAsgn, w->host) && !hostCursor) {
        *why = arenaPrintf(ac->a,
            "line %d: the host binding is reassigned in this function", w->line);
        return IDLE_REJ;
    }
    for (size_t j = 0; j < idx; j++) {
        IdleWrite *o = *(IdleWrite **)vecAt(&ac->idleWrites, j);
        if (o->kind != IDLE_DEST_HEAP_FIELD) continue;
        if (o->host == w->host && o->fname && w->fname && strcmp(o->fname, w->fname) == 0) {
                        /* refinement A: the old value has another root ⇒ the overwrite drops no root ⇒ no conflict */
            if (!(o->value && idleValueOtherRoot(ac, o->value))) {
                *why = arenaPrintf(ac->a,
                    "line %d: the same field of the same host is written at two sites", w->line);
                return IDLE_REJ;
            }
            break;
        }
    }
    return IDLE_ACC;
}

/**
 * @brief (2)'s statement visit: judge the statement half of criteria (a)(b)(c) and collect the local
 *        facts (a) needs.
 *
 * There is deliberately **no** `switch (s->kind)` here: this is not a hand-written kind traversal (the
 * walker ratchet targets exactly those), descent always goes through `astWalkStmtChildren`, so there is
 * no blind spot for a new kind.
 *
 * @param c            Checker (type table).
 * @param ac           This function's record.
 * @param s            Current statement.
 * @param depth        Current loop nesting depth (0 = not in any loop).
 * @param scope        Current scope depth (function body 1, +1 per block entered).
 * @param minLoopScope The **shallowest scope level** among all loop bodies enclosing this point
 *                     (0 = not in any loop); refinement (b') uses it as the threshold for "what this
 *                     write drops dies with this iteration".
 * @param condDepth    How many conditional statements enclose this statement.
 * @return             Always true.
 */
static bool idleWalkStmt(Checker *c, AcInfo *ac, Stmt *s, int depth, int scope, int minLoopScope,
                         int condDepth) {
    if (!s) return true;
    ac->idleSeq++;
    if (s->kind == ST_DOMAIN)
        idleNote(ac, "a domain block: the interleaving of reference drops is unprovable");
    if (s->kind == ST_VAR && s->u.var.overwrite) {
        idleNote(ac, arenaPrintf(ac->a,
            "line %d: `@overwrite` reuses one piece of storage, so the previous object is dropped",
            s->line));
    }
    if (s->kind == ST_VAR && !s->u.var.overwrite) {
        Expr *ini = s->u.var.init;
                /* Candidate host for (a): a local initialized by a **single** `new T` (no count; a count
         * makes it a view). Record the allocation point's loop depth and arena level (for refinement
         * (a')). Whether it is "never reassigned" is only known after the whole walk (an assignment
         * may follow the declaration or sit in a branch). */
        if (ini && ini->kind == EX_NEW && !ini->u.new_.count) {
            const char *nm = idleBindName(s, NULL);
            if (nm) {
                IdleFresh *fr = arenaAllocZero(ac->a, sizeof *fr);
                fr->name = nm;
                fr->depth = depth + 1;   /* from 1: 0 is reserved for "not in the ledger" */
                fr->arenaLevel = arenaDepthOf(planArenaLevel(ini));
                fr->seq = ac->idleSeq;
                *(IdleFresh **)vecPush(&ac->idleFresh) = fr;
            }
        }
    }
    if (s->kind == ST_ASSIGN) {
        Expr *t = s->u.assign.target;
        if (t && t->type && acSlotHoldsCountedRef(c, t->type)) {
                        /* Normalize this write into a case and hand it to the **single criterion**. The facts
             * (which names are fresh, which are reassigned, each value's arena level) are collected
             * in place during the walk; the criterion only looks at the case. */
            IdleWrite w;
            memset(&w, 0, sizeof w);
            w.value     = s->u.assign.value;
            w.line      = s->line;
            w.depth     = depth;
            w.loopScope = minLoopScope;
            w.seq       = ac->idleSeq;
            w.isAssign  = true;
            w.condDepth = condDepth;
            if (s->u.assign.op) {
                w.kind = IDLE_DEST_COMPOUND;
            } else if (!acTargetSlotIsHeap(c, t)) {
                                /* (b): a reference is written into a stack slot (a local/parameter, or a field of
                 * one) -- that drops a reference.
                 * Refinement (b'): a **ref identifier slot** may be overwritten as long as **every**
                 * value written into it dies with this iteration's arena (`idleStoreQualified`);
                 * `null` also qualifies (it drops no object).
                 * Parameters go through the same criterion: a value stored into a parameter must
                 * outlive this call ⇒ escape analysis places it in the home region (level 0) ⇒ the
                 * level test rejects naturally. Fields of local aggregates and other destinations are
                 * not this case and are still rejected. */
                w.kind = (t->kind == EX_IDENT) ? IDLE_DEST_SLOT_NAMED : IDLE_DEST_SLOT_OTHER;
                if (t->kind == EX_IDENT) w.host = idleBindName(t, t->u.ident.name);
            } else {
                Expr *host = idleHostIdent(t);
                w.kind  = IDLE_DEST_HEAP_FIELD;
                w.fname = idleDestPath(ac->a, t);
                                /* refinement E: the outermost level is an **element slot** and its index is an identifier assigned in this loop body */
                if (t->kind == EX_INDEX && t->u.index.index
                    && t->u.index.index->kind == EX_IDENT)
                    w.seqElem = idleNameAssignedInLoop(ac, idleBindName(t->u.index.index,
                                                    t->u.index.index->u.ident.name), minLoopScope);
                if (host) w.host = idleBindName(host, host->u.ident.name);
            }
            const char *why = NULL;
            IdleJudgment v = idleWriteOk(ac, &w, 0, &why, IDLE_STAGE_WALK);
            if (v == IDLE_REJ) {
                idleNote(ac, why);
            } else if (v == IDLE_DEFER) {
                if (w.kind == IDLE_DEST_HEAP_FIELD) {
                                        /* Record field writes with their raw value: (b')'s "a field read takes along the
                     * root" is computed by `idleSolveLvls`'s fixed point -- the written value may
                     * depend on another binding (`c.next = h` comes before `h = c`). */
                    IdleFieldStore *fs = arenaAllocZero(ac->a, sizeof *fs);
                    fs->fname = w.fname;
                    fs->host  = w.host;
                    fs->value = w.value;
                    fs->seq   = w.seq;
                    *(IdleFieldStore **)vecPush(&ac->idleFieldStores) = fs;
                    w.allocDepth = idleFreshAt(ac, w.host, w.seq)
                                 ? idleFreshAt(ac, w.host, w.seq)->arenaLevel : -1;
                }
                IdleWrite *rec = arenaAllocZero(ac->a, sizeof *rec);
                *rec = w;
                *(IdleWrite **)vecPush(&ac->idleWrites) = rec;
            }
        }
        if (t && t->kind == EX_IDENT) {
                        /* the flip side of the host candidates: this name is assigned somewhere in this function */
            const char *nm = idleBindName(t, t->u.ident.name);
            if (nm) {
                *(const char **)vecPush(&ac->idleAsgn) = nm;
                *(int *)vecPush(&ac->idleAsgnLoop) = minLoopScope;
            }
        }
    }
        /* A declaration initializer is the **bounded** branch: the value lands in a named slot whose
     * arena level is given by its block depth; so the root itself is not judged as a temporary (see
     * `bound` in `idleWalkExprFlag`). Its subexpressions are judged as usual. `ST_VAR`'s only child is
     * the initializer, so this replaces the generic child traversal.
     *
     * It also records "the value written into this ref slot" (a declaration initializer counts as one
     * write) -- (b')'s conjunction needs it. */
    if (s->kind == ST_VAR) {
        const char *nm = idleBindName(s, NULL);
        if (!s->u.var.overwrite) {
            Type *dt = s->type ? s->type : (s->u.var.init ? s->u.var.init->type : NULL);
            if (dt && acSlotHoldsCountedRef(c, dt)) {
                IdleWrite w;
                memset(&w, 0, sizeof w);
                w.kind      = IDLE_DEST_SLOT_NAMED;
                w.host      = nm;
                w.line      = s->line;
                w.loopScope = minLoopScope;
                w.value     = s->u.var.init;
                w.seq       = ac->idleSeq;
                w.condDepth = condDepth;
                w.depth     = depth;
                const char *why = NULL;
                if (idleWriteOk(ac, &w, 0, &why, IDLE_STAGE_WALK) == IDLE_DEFER) {
                    IdleWrite *rec = arenaAllocZero(ac->a, sizeof *rec);
                    *rec = w;
                    *(IdleWrite **)vecPush(&ac->idleWrites) = rec;
                }
            }
        }
        return idleWalkExprFlag(c, ac, s->u.var.init, depth, true, minLoopScope);
    }
        /* Entering a loop body: that block's scope level is the "this iteration's arena" level; take the
     * shallowest among all loops in the context. `condDepth` **resets** inside a loop body: the
     * question is "does this store happen in this round of the iteration", and whether an enclosing
     * condition covers the loop has nothing to do with "stores every round". */
    IdleWalk inner = { c, ac, depth + (idleLoopStmt(s) ? 1 : 0),
                       scope + (idleScopedStmt(s) ? 1 : 0), minLoopScope,
                       idleLoopStmt(s) ? 0
                                       : condDepth + ((s->kind == ST_IF || s->kind == ST_MATCH) ? 1 : 0) };
    if (idleLoopStmt(s)) {
        int bodyScope = scope + 1;                  /* a loop body is always a block in extC */
        if (inner.minLoopScope == 0 || bodyScope < inner.minLoopScope)
            inner.minLoopScope = bodyScope;
    }
    AstVisit v = { idleWalkExprCb, idleWalkStmtCb, &inner };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief (2)'s expression visit: judge the (c) half (concurrency, dispatch, an unanalyzable callee
 *        carrying a reference).
 *
 * @param c            Checker (type table).
 * @param ac           This function's record.
 * @param e            Current expression.
 * @param depth        Current loop nesting depth (expressions do not change it; passed down).
 * @param bound        This expression is a declaration initializer (its value lands in a bounded slot).
 * @param minLoopScope Innermost loop-body scope of the site.
 * @return             Always true.
 */
static bool idleWalkExprFlag(Checker *c, AcInfo *ac, Expr *e, int depth, bool bound, int minLoopScope) {
    if (!e) return true;
    if (e->kind == EX_EXT)
        idleNote(ac, "a spawned task (`ext`): the callee frame's drops are not visible");
    if (e->kind == EX_DYN)
        idleNote(ac, "a `dyn` call: the callee is not resolved");
        /* (c): a **value aggregate carrying references** (a struct, array, enum payload, or a by-value
     * call result) produced as a **temporary** loses that root the moment it dies -- and which arena
     * level the temporary's storage belongs to is decided by escape analysis; the possible shape is
     * "this frame's home region" (longer-lived than the block) ⇒ the bound grows with iteration count.
     *
     * `bound` true means this expression is **a declaration initializer**: the value lands in a named
     * slot whose arena level is given by its block depth (declared in a loop body ⇒ freed wholesale
     * every iteration) ⇒ bounded, not judged as a temporary. Its **sub**expressions are still judged
     * as usual (`mk(i)` inside `wrap { n: mk(i) }` is still a temporary that gets dropped).
     *
     * `ref T` is not itself an aggregate and is not this case (the value of
     * `t.left = bottomUp(...)` is `?ref tree`).
     *
     * **Refinement F**: allow when the site is **not inside any loop** (`minLoopScope == 0`). The
     * reason is the failure direction: (2)'s bad shape is "**every iteration** drops one copy that
     * accumulates in an arena longer-lived than the iteration", while a temporary outside a loop is
     * produced and dropped once per call, and its storage falls at worst in the **caller's current
     * block** (`arenaArg` defaults to the call site's block) -- the caller's block is reclaimed every
     * iteration when the caller is in a loop, and outside a loop it is dropped once. The path that
     * would really turn it into "home-region garbage" (storing the aggregate into a slot longer-lived
     * than the iteration) is blocked by the (a)/(b) criteria (`byval_temp`'s `take(mk(i))` is inside
     * a **loop** and is still rejected; `struct_copy_ret_home`'s site is inside `mk`, outside loops,
     * and goes idle with memory flat over 4N iterations). */
    if (!bound && e->type && e->kind != EX_IDENT && e->kind != EX_FIELD && e->kind != EX_INDEX
        && e->kind != EX_DEREF && e->kind != EX_SLICE && e->kind != EX_NEW
        && e->kind != EX_REF) {
        Type *et = tsub(c, e->type);
        if (et && et->kind != TY_REF && idleCarriesRef(c->tt, e->type, 0) && minLoopScope != 0)
            idleNote(ac, arenaPrintf(ac->a,
                "line %d: a reference-carrying aggregate is produced as a temporary value;"
                " its references lose this root when it dies", e->line));
    }
    if (exprIsCallKind(e)) {
        FuncDef *g = planCallee(e);
                /* An analyzable callee is not judged locally: the unit-wide conjunction judges it (see the
         * section note). An unanalyzable one is handled **parameter-sensitively** -- when neither the
         * receiver nor any argument carries an rc reference, it cannot receive our reference and is
         * not a counterexample. */
        if ((!acAnalyzable(g) || planCallViaFn(e)) && acCallTouchesRef(c, e)) {
            const char *nm = (g && g->name) ? g->name
                           : (e->kind == EX_METHOD ? e->u.method.name
                                                   : (e->u.call.callee && e->u.call.callee->kind == EX_IDENT
                                                          ? e->u.call.callee->u.ident.name : "?"));
            idleNote(ac, arenaPrintf(ac->a,
                "line %d: a call to '%s' (whose body is not analysed) carries a reference",
                e->line, nm ? nm : "?"));
        }
    }
        /* The expression walk does not change the three depths: the 0 passed here only keeps aggregate
     * initialization complete (expressions never reach the `idleWalkStmt` branches that use them;
     * descending further would only be more conservative). */
    IdleWalk inner = { c, ac, depth, 0, minLoopScope, 1 };
    AstVisit v = { idleWalkExprCb, idleWalkStmtCb, &inner };
    return astWalkExprChildren(e, &v);
}

/** @brief Expression visit in a general position (does not exempt the "value aggregate carrying references" rule). */
static bool idleWalkExpr(Checker *c, AcInfo *ac, Expr *e, int depth, int minLoopScope) {
    return idleWalkExprFlag(c, ac, e, depth, false, minLoopScope);
}

/**
 * @brief Record one call dependency into a vector (`AcDep *`, arena-allocated).
 *
 * @param dep   Dependency vector.
 * @param f     Callee; NULL is recorded too ("cannot resolve" is a dependency, and the closure treats
 *              it as not holding).
 * @param line  Call-site line.
 * @param a     Arena for the allocation.
 */
static void acPushDep(Vec *dep, FuncDef *f, int line, Arena *a) {
    AcDep *d = arenaAllocZero(a, sizeof *d);
    d->fn = f;
    d->line = line;
    *(AcDep **)vecPush(dep) = d;
}

/**
 * @brief Expression callback: collect (iv)'s call dependencies, black-box calls, and concurrency
 *        forms.
 *
 * @param ctx  Scan context (`AcScan *`).
 * @param e    Child expression being visited.
 * @return     Always true (walk the whole body; only the first reason is kept).
 */
static bool acScanExpr(void *ctx, Expr *e) {
    AcScan *sc = ctx;
    AcInfo *ac = sc->ac;
    if (!e) return true;
    switch (e->kind) {
    case EX_EXT:
        if (!ac->ivBad) { ac->ivBad = true; ac->ivWhy = "a spawned task (`ext`)"; }
        break;
    case EX_DYN:
        if (!ac->ivBad) { ac->ivBad = true; ac->ivWhy = "a `dyn` call: the callee is not resolved"; }
        break;
    case EX_GENCALL: {
        /* (iv) cannot see a **bulk byte move**. `copyInto<T>` is one `memmove` that duplicates
         * every reference leaf of the element into the destination, and `poolResize<T>` /
         * `poolResizeRaw<T>` move the block's bytes (a grow keeps the live prefix) without an
         * `inc` per reference. The premise of the event tier -- "every drop of a reference goes
         * through a counted slot" -- therefore does not hold for such a unit: a later overwrite of
         * the source slot decrements the only count and frees an object the copied block still
         * points at (shape G13, `tests/gc-p0/src/uaf_bulk_copy.extc`). Mark the local part of (iv)
         * false, which keeps the unit on the sweep tier (the conservative direction); the program
         * is not rejected, and a pure-data instantiation (`poolResize<i64>`) passes unchanged. */
        const char *nm = e->u.gencall.name;
        if (!ac->ivBad && nm &&
            (strcmp(nm, "copyInto") == 0 || strcmp(nm, "poolResize") == 0 ||
             strcmp(nm, "poolResizeRaw") == 0) &&
            e->u.gencall.targs.len > 0) {
            Type *et = tsub(sc->c, *(Type **)vecAt(&e->u.gencall.targs, 0));
            if (et && (mentionsParam(et) || typeHoldsCountedRef(sc->c->tt, et))) {
                ac->ivBad = true;
                ac->ivWhy = arenaPrintf(ac->a,
                    "line %d: `%s` moves a block of elements containing references; the acyclic"
                    " analysis cannot see the byte move", e->line, nm);
            }
        }
        break;
    }
    case EX_CALL: case EX_METHOD: case EX_ASSOC: {
        FuncDef *g = planCallee(e);
                /* An analyzable callee goes through the call-graph closure ((iv) must hold for it too); an
         * unanalyzable one (black box / extern / generic body) is handled **parameter-sensitively**:
         * when neither the arguments nor the receiver carry an rc object reference, it cannot receive
         * our reference and is no counterexample to (iv). A reference to a scalar such as `ref i64`
         * does not count either (see `acTypeMaybeRef`). */
        if (!acAnalyzable(g) || planCallViaFn(e)) {
            if (!ac->ivBad && acCallTouchesRef(sc->c, e)) {
                const char *nm = g && g->name ? g->name
                               : (e->kind == EX_METHOD ? e->u.method.name
                                                       : (e->kind == EX_CALL && e->u.call.callee
                                                              && e->u.call.callee->kind == EX_IDENT
                                                          ? e->u.call.callee->u.ident.name : "?"));
                ac->ivBad = true;
                ac->ivWhy = arenaPrintf(ac->a,
                    "a call to '%s' (whose body is not analysed) carries a reference",
                    nm ? nm : "?");
            }
        } else {
            acPushDep(&ac->ivDeps, g, e->line, ac->a);
        }
        break;
    }
    default: break;
    }
    AstVisit v = { acScanExpr, acScanStmt, sc };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement callback: find (iv) violations and (i') dependencies.
 *
 * @param ctx  Scan context (`AcScan *`).
 * @param s    Child statement being visited.
 * @return     Always true (walk the whole body; only the first reason is kept).
 */
static bool acScanStmt(void *ctx, Stmt *s) {
    AcScan *sc = ctx;
    AcInfo *ac = sc->ac;
    if (!s) return true;
    if (s->kind == ST_DOMAIN && !ac->ivBad) {
        ac->ivBad = true;
        ac->ivWhy = "a domain block: the interleaving of reference drops is unprovable";
    }
    if (s->kind == ST_VAR && anStoreRefKind(s) == SR_LOCAL && !ac->ivBad) {
                /* Marked by the checker while the scope is still alive: a reference-typed local in a nested
         * block drops its reference at block exit without settling the count. */
        ac->ivBad = true;
        ac->ivWhy = arenaPrintf(ac->a,
            "line %d: a reference local is declared in a nested block; its reference is dropped without a counted slot",
            s->line);
    }
    if (s->kind == ST_ASSIGN) {
        Expr *t = s->u.assign.target;
                /* Type-aware: it counts as a reference-slot store only when the slot holds a **counted**
         * reference. `i = i + 1` is not one, and neither is a view handle (`s = otherSlice`). */
        if (t && t->type && acSlotHoldsCountedRef(sc->c, t->type)) {
            if (!acTargetSlotIsHeap(sc->c, t)) {
                                /* (iv): a reference is written into a stack slot (a local/parameter, or a field of one), not a counted rc slot. */
                if (!ac->ivBad) {
                    ac->ivBad = true;
                    ac->ivWhy = arenaPrintf(ac->a,
                        "line %d: a reference is assigned to a stack slot (a local/parameter, or a field of one); references must live in counted heap slots",
                        s->line);
                }
            } else {
                int kind = anStoreRefKind(s);
                if (kind == SR_NONE || kind == SR_BAD) {
                    if (!ac->acycBad) {
                        ac->acycBad = true;
                        ac->acycWhy = arenaPrintf(ac->a,
                            "line %d: a reference field is written with a value that is neither null, nor a fresh allocation, nor a strictly younger layer",
                            s->line);
                    }
                } else if (kind == SR_CALL) {
                    acPushDep(&ac->acycDeps, planCallee(s->u.assign.value), s->line, ac->a);
                }
            }
        }
    }
    AstVisit v = { acScanExpr, acScanStmt, sc };
    return astWalkStmtChildren(s, &v);
}

/* ---- `retFresh`: does every `return` of this function deliver a newly allocated object? ----
 *
 * Criterion (i') allows "a value returned by one call". For that to be **sound**, the callee being
 * acyclic is not enough: it may return a parameter unchanged (`fn id(p: ref T) -> ref T { return p }`),
 * and then what gets stored is an object older than the host. So ask a second question: does every
 * `return` of F deliver a fresh allocation -- a direct `new`, a call to another `retFresh` function,
 * or a local binding that is **initialized exactly once and never reassigned, with its address never
 * taken** (`var n = new T; return n`). Names are matched as "declared exactly once in the whole body"
 * so shadowing cannot mix them up.
 */
typedef struct { const char *name; Expr *init; } AcDecl;

typedef struct {
    Checker *c;
    AcInfo  *ac;
    Vec      rets;      /* Expr*: non-void `return` values */
    Vec      decls;     /* AcDecl*: `ST_VAR` declarations */
    Vec      mutated;   /* const char*: names directly on the left of `=` */
    Vec      addressed; /* const char*: names appearing in `ref x` */
} FreshScan;

static bool freshStmt(void *ctx, Stmt *s);
static bool freshExpr(void *ctx, Expr *e);

/**
 * @brief Does an expression deliver a newly allocated object (the recursive test of `retFresh`)?
 *
 * @param fs  Scan result (declaration table, mutated/addressed name tables).
 * @param e   Delivered value.
 * @return    True when it provably is a fresh allocation.
 */
static bool freshValue(FreshScan *fs, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        return true;
    case EX_SIGN: return freshValue(fs, e->u.sign.operand);
    case EX_TRY:  return freshValue(fs, e->u.try_.operand);
    case EX_CONV: return freshValue(fs, e->u.conv.operand);
    case EX_CALL: case EX_METHOD: case EX_ASSOC: {
        FuncDef *g = planCallee(e);
        if (!g || g->isExtern || planCallViaFn(e)) return false;
        acPushDep(&fs->ac->retDeps, g, e->line, fs->ac->a);
        return true;                      /* the dependency is tightened by the fixed point */
    }
    case EX_IDENT: {
        const char *nm = e->u.ident.name;
        if (!nm) return false;
        AcDecl *d = NULL;
        int count = 0;
        for (size_t i = 0; i < fs->decls.len; i++) {
            AcDecl *cand = *(AcDecl **)vecAt(&fs->decls, i);
            if (cand->name && strcmp(cand->name, nm) == 0) { d = cand; count++; }
        }
        if (count != 1 || !d || !d->init) return false;
        for (size_t i = 0; i < fs->mutated.len; i++)
            if (strcmp(*(const char **)vecAt(&fs->mutated, i), nm) == 0) return false;
        for (size_t i = 0; i < fs->addressed.len; i++)
            if (strcmp(*(const char **)vecAt(&fs->addressed, i), nm) == 0) return false;
        return freshValue(fs, d->init);
    }
    default: return false;
    }
}

/**
 * @brief Statement callback: collect `return` values, declarations, and names that were mutated or
 *        had their address taken.
 *
 * @param ctx  Scan context (`FreshScan *`).
 * @param s    Child statement being visited.
 * @return     Always true.
 */
static bool freshStmt(void *ctx, Stmt *s) {
    FreshScan *fs = ctx;
    if (!s) return true;
    if (s->kind == ST_VAR && s->u.var.name) {
        AcDecl *d = arenaAllocZero(fs->ac->a, sizeof *d);
        d->name = s->u.var.name;
        d->init = s->u.var.init;
        *(AcDecl **)vecPush(&fs->decls) = d;
    }
    if (s->kind == ST_ASSIGN && s->u.assign.target
        && s->u.assign.target->kind == EX_IDENT && s->u.assign.target->u.ident.name)
        *(const char **)vecPush(&fs->mutated) = s->u.assign.target->u.ident.name;
    if (s->kind == ST_RETURN && s->u.ret.value)
        *(Expr **)vecPush(&fs->rets) = s->u.ret.value;
    AstVisit v = { freshExpr, freshStmt, fs };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Expression callback: record names that had their address taken (an alias through `ref x` may
 *        modify that binding).
 *
 * @param ctx  Scan context (`FreshScan *`).
 * @param e    Child expression being visited.
 * @return     Always true.
 */
static bool freshExpr(void *ctx, Expr *e) {
    FreshScan *fs = ctx;
    if (!e) return true;
    if (e->kind == EX_REF && e->u.ref.operand && e->u.ref.operand->kind == EX_IDENT
        && e->u.ref.operand->u.ident.name)
        *(const char **)vecPush(&fs->addressed) = e->u.ref.operand->u.ident.name;
    AstVisit v = { freshExpr, freshStmt, fs };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Static test (ignoring callees): "every `return` of F delivers a freshly allocated object".
 *
 * @param f   Function (with a body).
 * @param c   Checker (arena and type table).
 * @param ac  Record (`retDeps` is collected here).
 * @return    True when the static part holds; false when there is no `return` value (a void function).
 */
static bool retFreshLocal(FuncDef *f, Checker *c, AcInfo *ac) {
    FreshScan fs;
    memset(&fs, 0, sizeof fs);
    fs.c = c;
    fs.ac = ac;
    vecInit(&fs.rets, c->arena, sizeof(void *));
    vecInit(&fs.decls, c->arena, sizeof(void *));
    vecInit(&fs.mutated, c->arena, sizeof(void *));
    vecInit(&fs.addressed, c->arena, sizeof(void *));
    AstVisit v = { freshExpr, freshStmt, &fs };
    astWalkStmtChildren(f->body, &v);
    if (fs.rets.len == 0) return false;
    for (size_t i = 0; i < fs.rets.len; i++)
        if (!freshValue(&fs, *(Expr **)vecAt(&fs.rets, i))) return false;
    return true;
}

/**
 * @brief Index of a function in the list.
 *
 * @param all  Function list (`FuncDef *`).
 * @param g    Function to find.
 * @return     Index; -1 when absent (including NULL).
 */
static long acIndex(Vec *all, FuncDef *g) {
    if (!g) return -1;
    for (size_t i = 0; i < all->len; i++)
        if (*(FuncDef **)vecAt(all, i) == g) return (long)i;
    return -1;
}

/**
 * @brief Is the callee "judged and true"?
 *
 * @param all         Function list.
 * @param considered  Whether each index was judged.
 * @param flags       Conclusion array.
 * @param g           Callee.
 * @return            True when judged and true.
 */
static bool acFlagAt(Vec *all, const bool *considered, const bool *flags, FuncDef *g) {
    long i = acIndex(all, g);
    return i >= 0 && considered[i] && flags[i];
}

/**
 * @brief Compute `planFnAcyclic` / `planFnRefsCounted`: two fixed points over the whole call graph.
 *
 * Must run after every body is checked and every deferred call site is resolved (`planCallee` is only
 * complete then). Diagnostic `EXTC_DBG_ACYCLIC=1`: one line per used function plus one program-level
 * summary.
 *
 * @param c  Checker.
 */
void computeFnAcyclic(Checker *c) {
    Module *m = c->m;
    Arena *a = c->arena;
    if (!m) return;
    Vec all;
    allFunctions(a, m, &all);
    size_t n = all.len;
    if (n == 0) return;
    AcInfo *info = arenaAllocZero(a, n * sizeof *info);
    bool *considered = arenaAllocZero(a, n * sizeof(bool));
    bool *rf = arenaAllocZero(a, n * sizeof(bool));
    bool *acb = arenaAllocZero(a, n * sizeof(bool));
    bool *ivb = arenaAllocZero(a, n * sizeof(bool));
    const char **acWhy = arenaAllocZero(a, n * sizeof(const char *));
    const char **ivWhy = arenaAllocZero(a, n * sizeof(const char *));

    for (size_t i = 0; i < n; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        considered[i] = acAnalyzable(f);
        if (!considered[i]) continue;
        info[i].a = a;
        vecInit(&info[i].acycDeps, a, sizeof(void *));
        vecInit(&info[i].ivDeps, a, sizeof(void *));
        vecInit(&info[i].retDeps, a, sizeof(void *));
        AcScan sc = { c, &info[i] };
        AstVisit v = { acScanExpr, acScanStmt, &sc };
        astWalkStmtChildren(f->body, &v);
        rf[i] = retFreshLocal(f, c, &info[i]);
        acb[i] = !info[i].acycBad;
        ivb[i] = !info[i].ivBad;
        acWhy[i] = info[i].acycWhy;
        ivWhy[i] = info[i].ivWhy;

                /* ---- (2) "reclaim has nothing to do": the local half of criteria (a)(b)(c) (see `idleWalkStmt`) ---- */
        vecInit(&info[i].idleFresh, a, sizeof(void *));
        vecInit(&info[i].idleAsgn, a, sizeof(void *));
        vecInit(&info[i].idleAsgnLoop, a, sizeof(int));
        vecInit(&info[i].idleWrites, a, sizeof(void *));
        vecInit(&info[i].idleBindLvl, a, sizeof(void *));
        vecInit(&info[i].idleFieldLvl, a, sizeof(void *));
        vecInit(&info[i].idleFieldStores, a, sizeof(void *));
        vecInit(&info[i].idleOrig, a, sizeof(void *));
        vecInit(&info[i].idleKeep, a, sizeof(void *));
        vecInit(&info[i].idleFreshSet, a, sizeof(void *));
        IdleWalk iw = { c, &info[i], 0, 1, 0, 0 };
        AstVisit iv = { idleWalkExprCb, idleWalkStmtCb, &iw };
        astWalkStmtChildren(f->body, &iv);
        idleSolveLvls(&info[i]);
        idleSolveFreshSet(&info[i]);   /* refinement P, host facts: which bindings hold only fresh values */
        idleSolveProj(&info[i]);       /* the two tables of flavor (ii) (greatest fixed point) */
        /* The remaining halves of (a)/(a') plus refinement A form the **post-walk** stage of the
         * criteria: the walk only filtered out "a write to a host that is not this iteration's";
         * the full ledger decides C/B/A here. Site order is record order, and refinement A only
         * looks at earlier sites. */
        for (size_t k = 0; k < info[i].idleWrites.len && !info[i].idleBad; k++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&info[i].idleWrites, k);
            if (w->kind != IDLE_DEST_HEAP_FIELD) continue;
            const char *why = NULL;
            if (idleWriteOk(&info[i], w, k, &why, IDLE_STAGE_POST) == IDLE_REJ)
                idleNote(&info[i], why);
        }
        /* Refinement (b'): assignments into an on-stack ref slot are judged here. The condition
         * is "**every** value written into this slot dies with this iteration's arena"
         * (`idleStoreQualified`). One name can be written at several sites, so only the whole walk
         * sees them all; if any one site fails, every overwrite of that slot is rejected
         * (conservative). */
        for (size_t k = 0; k < info[i].idleWrites.len && !info[i].idleBad; k++) {
            IdleWrite *w = *(IdleWrite **)vecAt(&info[i].idleWrites, k);
            if (w->kind != IDLE_DEST_SLOT_NAMED || !w->isAssign) continue;
            const char *why = NULL;
            if (idleWriteOk(&info[i], w, k, &why, IDLE_STAGE_POST) == IDLE_REJ)
                idleNote(&info[i], why);
        }
    }

    /* retFresh closure: if the callee does not hold, neither does the caller. */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!rf[i]) continue;
            for (size_t k = 0; k < info[i].retDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].retDeps, k);
                if (!acFlagAt(&all, considered, rf, d->fn)) { rf[i] = false; changed = true; break; }
            }
        }
    }
    /* acyclic closure: every site of (i') requires the callee to be **both** acyclic and to
     * return a fresh allocation. */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!acb[i]) continue;
            for (size_t k = 0; k < info[i].acycDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].acycDeps, k);
                long gi = acIndex(&all, d->fn);
                const char *why = NULL;
                if (!d->fn) {
                    why = "the callee of a stored call result is not resolved";
                } else if (gi < 0 || !considered[gi] || !acb[gi]) {
                    why = arenaPrintf(a, "the callee '%s' is not proved acyclic",
                                      d->fn->name ? d->fn->name : "?");
                } else if (!rf[gi]) {
                    why = arenaPrintf(a, "the callee '%s' does not provably return a fresh allocation",
                                      d->fn->name ? d->fn->name : "?");
                }
                if (why) {
                    acb[i] = false;
                    acWhy[i] = arenaPrintf(a, "line %d: %s", d->line, why);
                    changed = true;
                    break;
                }
            }
        }
    }
    /* iv closure: every callee in sight must hold too (references live only in counted heap
     * slots). */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!ivb[i]) continue;
            for (size_t k = 0; k < info[i].ivDeps.len; k++) {
                AcDep *d = *(AcDep **)vecAt(&info[i].ivDeps, k);
                if (!acFlagAt(&all, considered, ivb, d->fn)) {
                    ivb[i] = false;
                    ivWhy[i] = arenaPrintf(a,
                        "line %d: it calls '%s', which may drop references outside counted heap slots",
                        d->line, (d->fn && d->fn->name) ? d->fn->name : "?");
                    changed = true;
                    break;
                }
            }
        }
    }

    /* Write the plan side back: `fnAcyclic` is the conjunction of (i)(ii)(iii)(i') and (iv),
     * `fnRefsCounted` carries (iv) alone, and `fnReclaimIdle` is the separate criteria (a)(b)(c)
     * (see `idleWalkStmt`: neither implies the other). */
    for (size_t i = 0; i < n; i++) {
        if (!considered[i]) continue;
        FuncDef *f = *(FuncDef **)vecAt(&all, i);
        bool ok = acb[i] && ivb[i];
        planSetFnAcyclic(f, ok);
        planSetFnRefsCounted(f, ivb[i]);
        planSetFnReclaimIdle(f, !info[i].idleBad);
        if (getenv("EXTC_DBG_RCIDLE"))
            fprintf(stderr, "[rcidle] %s idle=%d reason=%s\n", f->name ? f->name : "?",
                    (int)!info[i].idleBad,
                    info[i].idleBad ? (info[i].idleWhy ? info[i].idleWhy : "not computed") : "ok");
        if (ok) { anSetAcyclicWhy(f, NULL); continue; }
        if (!acb[i])
            anSetAcyclicWhy(f, arenaPrintf(a, "i-iii:%s",
                acWhy[i] ? acWhy[i] : "a reference store is not provably acyclic"));
        else
            anSetAcyclicWhy(f, arenaPrintf(a, "iv:%s",
                ivWhy[i] ? ivWhy[i] : "references do not live only in counted heap slots"));
    }

    /* `EXTC_DBG_ACYCLIC=1`: one `[acyclic] <name> fn=<0|1> reason=<...>` line per considered
     * function that is used; every function not considered gets one line, either `skipped` (an
     * extern or a declaration only, which the backend does not need) or `unproven` (a generic
     * body, which blocks the sweep while it is used); the last line is the program-level
     * summary. */
    if (getenv("EXTC_DBG_ACYCLIC")) {
        size_t nf = 0, na = 0, nskip = 0, nunproof = 0;
        bool hasMain = false;
        for (size_t i = 0; i < n; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f && f->name && strcmp(f->name, "main") == 0) hasMain = true;
        }
        for (size_t i = 0; i < n; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (!f) continue;
            if (!acRequired(c->m, f)) {
                /* Not emitted (a generic template, or a template method of a generic struct),
                 * so it does not block "sweep off"; it only counts. A program can hold dozens of
                 * template methods, and printing each one would bury the real reason. */
                if (planUsed(f) || (f->name && strcmp(f->name, "main") == 0)) nskip++;
                continue;
            }
            if (!considered[i]) {
                /* Emitted, but the checker has no verdict (an extern, or a generic instance),
                 * so `planFnAcyclic` is always false and `unitAllAcyclic` refuses. Listed so that
                 * "why did it not apply" is visible at a glance. */
                nunproof++;
                fprintf(stderr, "[acyclic] unproven %s (%s)\n",
                        f->name ? f->name : "?", acUnprovenWhy(f));
                continue;
            }
            nf++;
            if (planFnAcyclic(f)) {
                na++;
                fprintf(stderr, "[acyclic] %s fn=1 reason=ok\n", f->name ? f->name : "?");
                continue;
            }
            const char *why = anAcyclicWhy(f);
            fprintf(stderr, "[acyclic] %s fn=0 reason=%s\n",
                    f->name ? f->name : "?", why ? why : "not computed");
        }
        /* The program-level summary prints once: the module that defines `main` is the program
         * itself, and modules pulled in by `use` print their own function lines without repeating
         * the "program" summary.
         *
         * `all=1` means **usable for turning the sweep off**: every function `unitAllAcyclic`
         * emits has a true verdict (considered ones through `acyclic/funcs`, unconsidered ones
         * through `unproven`, which blocks outright). `skipped` (generic templates) does not
         * block, because a template is not emitted. Approximation: `unitAllAcyclic` also requires
         * the template methods of instantiated types, and instances are built lazily by codegen,
         * invisible to the checker, so `skipped` template methods are undercounted there. */
        if (hasMain) {
            if (nskip)
                fprintf(stderr, "[acyclic] skipped %zu generic template method(s) (not emitted)\n", nskip);
            fprintf(stderr, "[acyclic] program: funcs=%zu acyclic=%zu skipped=%zu unproven=%zu all=%d\n",
                    nf, na, nskip, nunproof, (int)(nf == na && nunproof == 0));
        }
    }
}

/**
 * @brief How shallow a call site's destination is; the arena and zone paths share this.
 *
 * The result is a **depth**, and a smaller number lives longer. A caller that sees `<= 0` uses
 * its own HOME. `rec->overflow` (arguments not all recorded) counts as longest-lived (-1); an
 * argument at depth 0 counts as 0; an argument in the escape set counts as -1.
 *
 * @param c    The checker.
 * @param rec  Recorded call-site information.
 * @return     The destination depth.
 */

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

/**
 * @brief The function whose body a deferred check was written in, when that body is a lambda's.
 *
 * A check recorded inside a lambda body carries the lambda's synthesized `call` method, because the
 * recording pass stores `c->curFunc`. That method is shared by every instance of the enclosing body,
 * so neither replay branch can find it: the type-instance branch keys on `owner` (the struct the
 * method belongs to, here the environment struct, which has no instances) and the free-function
 * branch keys on `planTemplate` (a `call` method is not an instance of anything). The enclosing body
 * is recorded on the environment struct, and *it* has instances.
 *
 * @param f  The `func` a deferred check was recorded against.
 * @return   The enclosing function; NULL when the check is not inside a lambda body.
 */
static FuncDef *lamHostOf(const FuncDef *f) {
    if (!f || !f->owner) return NULL;
    return planLamScope(f->owner);
}

/**
 * @brief One deferred check, replayed in one concrete instance.
 *
 * The four batches that need instance replay (operators, methods, deferred uses, references) all
 * walk the same two lists with the same guards; only the routine that re-checks the record differs.
 * They hand that routine to `replayInTypeInstances`/`replayInFuncInstances` through this type.
 *
 * @param c         The checker.
 * @param rec       The record, as the batch stored it.
 * @param params    The instance's type parameters (`owner->typeParams` or the template's).
 * @param targs     The instance's type arguments.
 * @param instName  The instance's C name, used in the diagnostic.
 */
typedef void (*ReplayFn)(Checker *c, void *rec, Vec *params, Vec *targs, const char *instName);

/**
 * @brief Replay a deferred check once per instance of a generic *type*.
 *
 * @param c      The checker.
 * @param tt     The type table that owns the instance list.
 * @param owner  The generic struct whose instances to visit.
 * @param gate   When non-NULL, only instances whose shared body actually used that method: a body
 *               shared by every element type must not force all of them to define the operator.
 * @param fn     The re-check to run.
 * @param rec    The record to pass to `fn`.
 */
static void replayInTypeInstances(Checker *c, TypeTable *tt, StructDef *owner, FuncDef *gate,
                                  ReplayFn fn, void *rec) {
    for (size_t j = 0; j < tt->instances.len; j++) {
        Type *inst = *(Type **)vecAt(&tt->instances, j);
        if (!inst || inst->sdef != owner) continue;
        if (provisionalInstance(&inst->targs)) continue;
        if (gate && !planInstUsed(gate, inst->name)) continue;
        fn(c, rec, &owner->typeParams, &inst->targs, inst->name);
    }
}

/**
 * @brief Replay a deferred check once per instance of a generic free function.
 *
 * The record stores the function it was written in, so only instances of that template are replayed
 * (a method is never an instance of a free function, which is why the caller can run this
 * unconditionally).
 *
 * @param c     The checker.
 * @param tmpl  The generic function template.
 * @param fn    The re-check to run.
 * @param rec   The record to pass to `fn`.
 */
static void replayInFuncInstances(Checker *c, FuncDef *tmpl, ReplayFn fn, void *rec) {
    for (size_t j = 0; j < c->funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c->funcInsts, j);
        if (!fi || planTemplate(fi) != tmpl) continue;
        if (provisionalInstance(&fi->targs)) continue;
        fn(c, rec, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
    }
}

/**
 * @brief Replay a deferred check for a body written inside a lambda, once per instance of the body
 *        that lambda is written in.
 *
 * A `call` method is neither an instance of the environment struct nor an instance of a free
 * function template, so both ordinary branches skip it. The environment records the enclosing
 * function, and *that* has instances. A lambda inside a lambda is refused earlier (the inner
 * environment would have to be laid out inside the outer one), so one hop is enough.
 *
 * @param c      The checker.
 * @param tt     The type table that owns the instance list.
 * @param recFunc The function a deferred check was recorded against.
 * @param fn     The re-check to run.
 * @param rec    The record to pass to `fn`.
 * @return       true when the record was inside a lambda body (whether or not any instance exists).
 */
static bool replayInLambdaInstances(Checker *c, TypeTable *tt, FuncDef *recFunc, ReplayFn fn,
                                    void *rec) {
    FuncDef *host = lamHostOf(recFunc);
    if (!host) return false;
    if (host->owner && host->owner->typeParams.len > 0) {
        replayInTypeInstances(c, tt, host->owner, NULL, fn, rec);
    } else if (host->typeParams.len > 0) {
        replayInFuncInstances(c, host, fn, rec);
    }
    return true;
}

/** @brief Replay one recorded operator use in one instance. */
static void replayOpRecord(Checker *c, void *rec, Vec *params, Vec *targs, const char *instName) {
    runOpCheck(c, (OpCheck *)rec, params, targs, instName);
}

/** @brief Replay one recorded method call in one instance. */
static void replayMethodRecord(Checker *c, void *rec, Vec *params, Vec *targs,
                               const char *instName) {
    runMethodCheck(c, (MethodCheck *)rec, params, targs, instName);
}

/** @brief Replay one recorded deferred use in one instance. */
static void replayDeferredRecord(Checker *c, void *rec, Vec *params, Vec *targs,
                                 const char *instName) {
    runDeferredUse(c, (DeferredUse *)rec, params, targs, instName);
}

/** @brief Replay one recorded reference rule in one instance. */
static void replayRefRecord(Checker *c, void *rec, Vec *params, Vec *targs, const char *instName) {
    /* `runRefCheck` reads the substitution context from the checker rather than taking it. */
    c->substParams = params;
    c->substArgs   = targs;
    runRefCheck(c, (RefCheck *)rec, instName);
}


/**
 * @brief Check one whole module: the entry point of this pass.
 *
 * Runs its stages in a fixed order: intern names -> resolve signatures and the types in fields
 * and enum payloads -> check for duplicate declarations -> check globals -> check each function
 * body -> replay the deferred checks per concrete instance -> close the analyses (`needsHome`
 * transitive closure, the escape set, effect summaries, the level solver, the arena passed at each
 * call) -> hand the decisions to codegen.
 *
 * The order is not free. Instances are created while function bodies are checked, so the closure
 * has to run after the bodies; `needsHome` is only final after the closure; and the escape set has
 * to be known before each call's arena is chosen, so the closing stage recomputes it instead of
 * reusing per-function results. `refDepth` and `arenaLevel` describe the same thing, and after the
 * solver runs every site's `refDepth` must be resynchronised from the final `arenaLevel`, or the two
 * fields contradict each other.
 *
 * @param ctx    Compile context; `hasError` is set when a diagnostic is emitted.
 * @param arena  Arena used for this pass's allocations.
 * @param tt     Type table.
 * @param m      Module to check.
 * @return       true when the module has no errors.
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    /* The phase is explicit state (the phase contract). */

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
    vecInit(&c.pendingRefStores, arena, sizeof(void *));   /* deferred `(*cur).f = v` verdicts */
    vecInit(&c.borrowSettled, arena, sizeof(void *));      /* borrow results whose storage moved */
    vecInit(&c.cycleMerges, arena, sizeof(void *));        /* regions merged as candidates for writes that may close a cycle */
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
        /* Same as the on-demand synthesis path: use **the type parameter names of the marker
         * itself**, because substitution matches by name (`param`). `A` comes in from `send` and
         * `B` goes out through `value`; a `coroutine<T>` that names only one parameter falls back
         * at the substitution. */
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
    /* Verification-only place-level data flow (`check_lvl.c`): the switch is read once, and
     * an unset variable makes every recording site and the analysis itself a no-op. */
    c.lvlOn = getenv("EXTC_DBG_LVL") != NULL;
    vecInit(&c.lvlSites, arena, sizeof(LvlSite *));
    /* Must be initialized here, before any pass.
     *
     * `checkGlobals` runs before the first `checkFunc`, so initializing `curArenaSites` inside
     * `checkFunc` alone is too late: a top-level initializer with a `new` (`var G = new i32`) or a
     * call to a function that needs a home arena (`let G = helper()`) pushes onto an uninitialized
     * Vec, so `vecGrow` reaches `arenaAlloc(NULL)` and the compiler dies with SIGSEGV instead of
     * reporting "a global can only be initialized with a constant". */
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
            planSetImplTrait(im, tr);
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
                planSetBuiltinHolder(sd, true);
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
                    planSetBuiltinHolder(ih, true);   /* no C struct of its own: the instance has one */
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
            planSetImplTarget(im, t);                 /* resolved here, read by the conformance pass and codegen */
            /* Orphan rule: the block must live where the trait is declared or where the type is
             * declared. Anywhere else it has no home -- and because a type has exactly one method
             * set, a competing implementation from a third module would surface as an unrelated
             * duplicate-method error far from its cause. */
            /* Ownership is written on the block: a type has exactly one method set, and a reader
             * has to see from one line who provides it. Extending a type from **another module**
             * therefore requires `@ext`; when the block already lives in the type's own module the
             * annotation is forbidden instead, since it would say nothing. A builtin scalar (the
             * holder of `i64`) has no declaring module, so both forms pass: its methods come from
             * whoever writes the block. */
            if (!planImplTrait(im) && sd && sd->modName) {
                bool own = sameModule(im->modName, sd->modName);
                /* The location is reported in **the block's own file**: diagnostics inside a
                 * module body go through that module's `Ctx` (the same one function bodies use),
                 * and the ownership rule has no reason to be the exception. */
                Ctx *savedCtx = c.ctx;
                if (im->ctx) c.ctx = im->ctx;
                if (own && im->isExt) {
                    ckError(&c, im->line,
                            "Drop `@ext`: the block already lives in the module that declares the"
                            " type, so the annotation says nothing.",
                            "`@ext` on an impl block of the type's own module");
                    c.ctx = savedCtx;
                    continue;
                }
                if (!own && !im->isExt) {
                    ckError(&c, im->line,
                            "An `impl` block for another module's type is an extension: mark it"
                            " `@ext` so the method set has a written-down provider.",
                            "`impl %s` extends a type of module `%s`: write `@ext`",
                            im->typeName, sd->modName);
                    c.ctx = savedCtx;
                    continue;
                }
                c.ctx = savedCtx;
            }
            if (planImplTrait(im) && !sameModule(im->modName, planImplTrait(im)->modName) &&
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
    c.phase = PHASE_BODIES;   /* from here on scopes exist (the phase contract) */
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

    /* Call-graph fixpoint: it runs **after every function body is checked** (`callees` is only
     * filled by then) and before the batches below rewrite call sites -- none of them adds a new
     * source of "writes a reference slot". */
    computeRcNoDec(&c);

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
    c.phase = PHASE_POST;    /* **no scopes** after this: only tables and the identity recorded on nodes and symbols */
    ctPhase("ckB8", tB8);   /* after the last of the eight: the rest of checkModule */
    /* **To a fixpoint**, not in a single pass.
     *
     * Resolving one call site can *create* the callee's instance (`funcInstance` interns on
     * demand), and that new instance has deferred call sites of its own that also need resolving.
     * One pass therefore left the inner ones dangling: `fn f<T>` / `fn g<T> { f(x) }` /
     * `fn m<T> { g(x) }` **declared in that order** generated a call to `f_T` that was never
     * emitted, so the C did not compile -- while the same program with the declarations reversed
     * compiled, which is what made this look like a declaration-order oddity instead of a missing
     * round. `resolveDeferredCall` is idempotent (interning the same argument
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

    /* Every instance of a type with a `@drop` hook needs that hook's body: the instance body calls
     * it by the **instance** name, so the template alone leaves the C with an undeclared function.
     * A `var` that declares such a value registers the instance through its own statement, but a
     * value that only ever appears as a temporary, such as `take(buf<i64>::withCap(4))`, has no such
     * site, so the registration is done here for every instance.
     *
     * This has to run **before** the closure below. The hook body is a root of that
     * closure, and the methods it calls -- a `@drop` that revokes reference edges goes
     * through a private helper -- are reached from no other site. Registered after the
     * closure, the helper is left out of the emitted C while the hook body still names
     * it: an implicit declaration at the call site, seen as
     * `implicit declaration of function ...; did you mean ...` when the C is compiled. */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (!inst || !inst->sdef || !inst->sdef->dropHook) continue;
        if (provisionalInstance(&inst->targs)) continue;
        planSetInstUsed(inst->sdef->dropHook, inst->name);
    }

    /* Per-instance method-use closure: a method body is emitted for one type instance only
     * when that instance really used it (see the gates in the replay loops above). A body that
     * is emitted calls other methods of the **same** generic struct -- `vector<T>::push` calls
     * `grow` -- and those calls were resolved on the template, so they carry no instance. Close
     * the set here: for every used (method, instance) pair, mark the callees of its body that
     * belong to the same struct, until it stops growing. Monotone, so it terminates. */
    for (;;) {
        size_t before = planInstUseCount();
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (!inst || inst->kind != TY_GENERIC || !inst->sdef) continue;
            if (provisionalInstance(&inst->targs)) continue;
            for (size_t k = 0; k < inst->sdef->methods.len; k++) {
                FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, k);
                if (!md || !md->body || !planUsed(md)) continue;
                if (!planInstUsed(md, inst->name)) continue;
                instUseClosure(&c, md->body, &inst->sdef->typeParams, &inst->targs);
            }
        }
        /* The instance body of a free generic function: it mentions associated calls such as
         * `ndview<T>::rowMajor(...)`, and the receiver type is substituted with the instance's type
         * arguments the same way. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            FuncDef *tmpl = fi ? planTemplate(fi) : NULL;
            if (!fi || !tmpl || !planUsed(tmpl) || !fi->body) continue;
            if (provisionalInstance(&fi->targs)) continue;
            instUseClosure(&c, fi->body, &tmpl->typeParams, &fi->targs);
        }
        if (planInstUseCount() == before) break;   /* fixpoint: the count only grows, so it stops */
    }

    /* Deferred operator checks: re-check each recorded use for every concrete instance.
     * This is the price of having no traits. The error surfaces late, here, so the
     * message has to name the instance it came from. */
    for (size_t i = 0; i < c.opChecks.len; i++) {
        OpCheck *ec = *(OpCheck **)vecAt(&c.opChecks, i);
        /* The function holding the operator is never called, so none of its instances can
         * run and the per-instance recheck is unnecessary. This also spares the user from
         * having to define the operator for a type that is never used with it. */
        if (ec->func && !planUsed(ec->func)) continue;
        /* (1) Type instances (an operator used on a method's `T`). A free function has no
         * owner, so it has no type instances; it is covered by (2).
         *
         * The two branches are separate on purpose: a `goto done` when `ec->owner` is NULL
         * would skip (2) as well, leaving every `T: ==` inside a free generic function to
         * code generation, whose message happens to be word for word the same -- and which
         * cannot see the `%`-on-float case at all. So the loop boundary is written out here. */
        if (ec->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != ec->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                /* Only instances that really used this method: a shared body must not
                 * force every element type of the container to define the operator. */
                if (ec->func && !planInstUsed(ec->func, inst->name)) continue;
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
        if (replayInLambdaInstances(&c, tt, ec->func, replayOpRecord, ec)) continue;
    }

    /* Deferred method calls on a type parameter: the same shape as the operator batch
     * above -- replayed once per concrete instance, with the instance named in the message.
     *
     * This is what makes a generic container able to do anything with its elements at all:
     * `hashMap<K, V>` hashes a `K`, which means calling `hash()` on a value whose type is a type
     * parameter. On the template there is no such method to find, so the call is recorded and
     * the instance decides. */
    for (size_t i = 0; i < c.methodChecks.len; i++) {
        MethodCheck *mc = *(MethodCheck **)vecAt(&c.methodChecks, i);
        /* A template that is never called has no instances, so nothing can be wrong yet. */
        if (mc->func && !planUsed(mc->func)) continue;
        /* (1) Type instances: a method call inside a generic type's method, where `T` comes
         * from the type arguments of the struct that owns it. */
        if (mc->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != mc->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                if (mc->func && !planInstUsed(mc->func, inst->name)) continue;
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
        if (replayInLambdaInstances(&c, tt, mc->func, replayMethodRecord, mc)) continue;
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
         * on a local inside `mk<i64>`. Substituting the annotation here interns
         * it (`ttGeneric`), early enough for the unit list to see it. It is the same kind of fix as
         * the `coroSetup` re-run above: the instance existed, it just appeared too late. */
        coroFrameLay(&c, fi);
    }

    /* Every generic function **instance**: intern the type instances its body declares as local
     * types. The loop above is coroutine-only (it skips instances that already have a frame), and the
     * substitution that would normally intern such a type happens during **body emission** -- after
     * code generation built its unit list and emitted the struct definitions, so the struct never
     * appeared (`unknown type name 'pair_i64'`). */
    for (size_t j = 0; j < c.funcInsts.len; j++) {
        FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
        if (fi && planTemplate(fi)) internLocalTypes(&c, fi);
    }

    /* Deferred uses of such a call's result: same two branches, but the question is now
     * whether the instance's real return type fits the type the use site expected. */
    for (size_t i = 0; i < c.deferredUses.len; i++) {
        DeferredUse *du = *(DeferredUse **)vecAt(&c.deferredUses, i);
        if (du->func && !planUsed(du->func)) continue;
        if (du->owner) {
            for (size_t j = 0; j < tt->instances.len; j++) {
                Type *inst = *(Type **)vecAt(&tt->instances, j);
                if (inst->sdef != du->owner) continue;
                if (provisionalInstance(&inst->targs)) continue;
                if (du->func && !planInstUsed(du->func, inst->name)) continue;
                runDeferredUse(&c, du, &du->owner->typeParams, &inst->targs, inst->name);
            }
        }
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (du->func != planTemplate(fi)) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runDeferredUse(&c, du, &planTemplate(fi)->typeParams, &fi->targs, planInstName(fi));
        }
        if (replayInLambdaInstances(&c, tt, du->func, replayDeferredRecord, du)) continue;
    }


    /* Materialise every concrete instance's field types.
     *
     * A generic struct that holds another generic instance (`vals: pool<V>`) declares that field
     * with a parameter, so checking the template interns nothing: `pool<V>` is not concrete, and
     * correctly so. The concrete `pool<i32>` only comes into being when the field type is
     * substituted with the instance's arguments. Leaving that to code generation is too late: it
     * snapshots the instance table first, so the mangled call is emitted while the callee's
     * functions are not, and the C compiler reports
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
        if (replayInLambdaInstances(&c, tt, rc->func, replayRefRecord, rc)) continue;
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
     * On the template `T` is opaque, so the `carrier` test needs `mentionsParam`:
     * "anything mentioning `T` counts as carrying a reference". That charges the `Cont`
     * bit to instances such as `varArray<i32>` that carry no reference at all -- a loss
     * of precision, not a wrong answer.
     *
     * Here the summary is recomputed inside each instance's substitution context
     * (`c.substParams` / `c.substArgs` set). The `carrier` line goes through
     * `tsub(c, e->type)`, so `carrier` is false when `T = i32` and that bit stays clear,
     * while `T = slice<u8>` still sets it. Safety rests on the latter case.
     *
     * The `mentionsParam` clause cannot be deleted in place of this recompute: without it,
     * `tests/errors/generic_borrowed_store.extc` goes from rejected to
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
        anSetAddrMask(fi, 0);
        anSetContMask(fi, 0);
        anSetOtherMask(fi, 0);
        anSetHomeAddrMask(fi, 0);
        anSetHomeContMask(fi, 0);
        anSetAddrFromLocal(fi, false);
        anSetFreshCount(fi, 0);
        anSetEffState(fi, EFF_NONE);  anSetEffComplete(fi, false);  anSetEffUnknown(fi, false);
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
            anSetAddrMask(f, 0);
            anSetContMask(f, 0);
            anSetOtherMask(f, 0);
            anSetHomeAddrMask(f, 0);
            anSetHomeContMask(f, 0);
            anSetAddrFromLocal(f, false);
            anSetFreshCount(f, 0);
            anSetEffState(f, EFF_NONE);  anSetEffComplete(f, false);  anSetEffUnknown(f, false);
            vecInit(&f->callees, c.arena, sizeof(FuncDef *));
            collectEffects(&c, f);
        }
    }
    c.substParams = NULL;
    c.substArgs   = NULL;

    /* The instance recompute just above makes summaries complete that a caller may already have
     * been closed against in an earlier, incomplete state: `hashMapI64::remove` is a method of a
     * *different* type instance than `hashMap::find`, and the instance lists are not in call order,
     * so the facade could have merged the template's incomplete summary and cached "incomplete".
     * Dropping the cached closure makes the next query recompute it from the summaries as they
     * stand now. Masks are only merged upward by a closure, so recomputing cannot lose a bit; it
     * can only keep one that is freshly attributable. */
    {
        Vec all; vecInit(&all, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            if (!sd) continue;
            for (size_t k = 0; k < sd->methods.len; k++)
                *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&sd->methods, k);
        }
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (!f || !f->body || f->isExtern) continue;
            anSetEffState(f, EFF_NONE);
            anSetEffComplete(f, false);
        }
    }

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
     * Once the closure has run, every `new` in a function that needs a home arena is
     * rewritten to `ARENA_HOME`, so codegen's `arenaRefAt` consults only the number the
     * checker produced, never a per-function home flag. */
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
            if (planTemplate(f)) anSetArenaSites(f, anArenaSites(planTemplate(f)));
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
         * substituted return type. Declaration order does not matter.
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
                anSetNeedsHome(f, true);
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
         * call sites to propagate them, so one round is not enough. A few rounds push it to the
         * fixed point; there are few functions, so the cost is negligible, and `computeEscapes`
         * itself is capped at 32 rounds. */
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
             * `nd == 0` must not double as "nothing seen yet" and as "this destination lives
             * outside the frame": depth 0 is the *shallowest* there is, so a loop keyed on
             * `nd` alone computes the **maximum** instead of the minimum -- `argDepth = {0, 1}`
             * comes out as 1, and the call hands over the caller's own block arena where it has to
             * hand over the home arena. The generated program then reads a block that has already
             * been released (heap-use-after-free). Two separate representations remove
             * the ambiguity -- no argument recorded leaves `nd` at 0, which still means "home
             * arena", the conservative answer. */
                        /* How shallow the destination is: one shared implementation with the zone channel.
             * `rec->overflow` was already handled by the `continue` in the branch above. */
            int nd = callSiteMinDestDepth(&c, rec);
            /* Write the new homeDepth onto `arenaArg`, by the same rules as `setCallArenaArg`. */
            int want = (nd <= 0) ? ARENA_HOME : nd;
                        /* Promotion (the call-result cell of `promoteInto2`) may already have moved the arena to
             * the **longer-lived** side: take the min of both requirements, do not raise it back.
             * `ARENA_HOME` (-1) is the longest-lived side. */
            {
                int cur = planArenaArg(rec->call);
                if (cur == ARENA_HOME || (cur >= 1 && cur < want)) want = cur;
            }
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

                /* Route C: merge the candidate regions of a "possible cycle" into the shallowest layer.
         *
         * Must run **after the solve and before placement**: the solve produces each site's final
         * `anMinAt` (merge input), and placement then writes `arenaLevel`/`refDepth` from it (merge
         * output). Scopes are all closed by then, so this only reads site pointers and never
         * resolves names. See `CycleMerge` / `applyCycleMerges`. */
        applyCycleMerges(&c);

        int fixed = 0, keptBlock = 0;
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind == EX_NEW || site->kind == EX_GENCALL) {
                    /* The solver has finished, so place the site as the single authority:
                     *   - `escaped` (some path that touched it demanded a lifetime beyond
                     *     this frame) => the home arena (the arena the caller chose);
                     *   - otherwise => the block level the solver produced, which inside a
                     *     loop means reclaimed every iteration.
                     *
                     * An unconditional "needs a home arena => home arena" fallback would pin
                     * per-iteration temporaries that never escape there until the frame
                     * ended. */
                    if (dbgOn("EXTC_DBG_MINAT"))
                        fprintf(stderr, "[minAt] %-8s line=%-4d minAt=%-3d arena=%d\n",
                                f->name ? f->name : "?", site->line, anMinAt(site), planArenaLevel(site));
                    if (dbgOn("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", anMinAt(site), anLexicalLevel(site),
                                planArenaLevel(site), anNeedsHome(f)?1:0, (int)site->kind);
                    if (dbgOn("EXTC_DBG_S3"))
                        fprintf(stderr, "[s3] %-8s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", anMinAt(site), anLexicalLevel(site),
                                planArenaLevel(site), anNeedsHome(f)?1:0, (int)site->kind);
                    int want;                            /* the arena it finally belongs to */
                    if (anMinAt(site) == 0) {
                        want = ARENA_HOME;              /* must outlive this frame => home arena */
                    } else if (anMinAt(site) >= 1) {
                        want = anMinAt(site);             /* must live to level k => block arena k */
                    } else {
                        /* No constraint touched it, so use its own level.
                         * `arenaLevel` cannot be consulted here: the provisional pass already
                         * rewrote every site in a function that needs a home arena to the
                         * not-yet-decided value. */
                        want = anLexicalLevel(site) >= 1 ? anLexicalLevel(site) : 1;
                    }
                    if (planArenaLevel(site) != want) {
                        planSetArenaLevel(site, want);
                        anSetRefDepth(site, arenaDepthOf(want));   /* the single conversion point */
                        if (want == ARENA_HOME) fixed++; else keptBlock++;
                    }
                } else if (site->arenaArgPending) {
                    /* A call site that hands an arena down is settled **after** the fixed
                     * point below, from the callee's `usesHome`, and deliberately not here:
                     * codegen appends the extra argument exactly when the callee's
                     * `usesHome` is set, so the value has to come from that same flag.
                     * Answering from the *owning* function's `needsHome` answers a
                     * different question (does this body have a home, not does the
                     * callee take one) and leaves the site with a block
                     * level: the caller then passes `&__extc_a[1]` while no such array is
                     * declared, and `tests/arena-promoted/R2a` stops compiling. */
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
        Vec as_ = anArenaSites(f);
        for (size_t j = 0; j < as_.len; j++) {
            Expr *site = *(Expr **)vecAt(&as_, j);
            /* A recorded destination decides on its own (`markCallHomeIfEscaping`); otherwise the
             * callee has to take a home parameter for the site to matter at all. */
            if (!site->arenaArgPending || !planCallee(site)) continue;
            if (anHomeDepth(site) == 0 && !planUsesHome(planCallee(site))) continue;
            /* Hand the block the result is stored in when `markCallHomeIfEscaping` recorded one: the
             * value only has to live as long as its destination, and the home arena would keep every
             * round of a loop alive (`buf b = make() ?? dflt` inside a loop leaked one per round).
             * A site that escaped afterwards carries `-1` and goes home. */
            planSetArenaArg(site, anHomeDepth(site) >= 2 ? anHomeDepth(site) : ARENA_HOME);
            site->arenaArgPending = false;
        }
    }

        /* Recompute every site's `refDepth` from its final level, whether or not the level
         * changed.
         *
         * Why: the
         * `anSetRefDepth(site, ...` in the placement loop above sits inside
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
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                anSetRefDepth(site, arenaDepthOf(planArenaLevel(site)));   /* keep the two in sync */
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
                        planInstName(f) ? planInstName(f) : f->name, (int)planUsesHome(f), (int)anNeedsHome(f),
                        ftName,
                        anArenaSites(f).len);
                Vec as_ = anArenaSites(f);
                for (size_t j = 0; j < as_.len; j++)
                    fprintf(stderr, " L%d", planArenaLevel(*(Expr **)vecAt(&as_, j)));
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
             * `new T[cap]` is still the not-yet-decided 0, and after solving it may be 1.
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
            planSetOwSites(f, countOwSites(f->body));
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
            planSetOwLocal(f, true);
        }
        if (getenv("EXTC_DUMP_OW"))
            for (size_t i = 0; i < all.len; i++) {
                FuncDef *f = *(FuncDef **)vecAt(&all, i);
                if (planOwSites(f)) fprintf(stderr, "[ow] %-18s sites=%d local=%d\n",
                                            f->name, planOwSites(f), (int)planOwLocal(f));
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
        /* Also record on the struct "one of its methods creates a pool" (see `StructDef.makesPoolAny`). */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (f && planMakesPool(f)) { anSetMakesPoolAny(sd, true); break; }
        }
    }
        /* ---- zone level at call sites: also decided only after the `makesPool` closure ----
     *
     * Same root cause as the second level solve above. `zoneLevel` decides which layer the callee's
     * pool allocations live to, and **only a pool-creating callee receives that argument**;
     * `makesPool` is a closure ⇒ before the closure it reads false everywhere, so the call site stays
     * at a temporary level and a chain two hops long passes its own place down:
     *
     *     main → wrap → put → newString
     *
     * `wrap` passes **its own** zone to `put`, so `newString`'s buffer is born in `wrap`'s place, is
     * freed as soon as `wrap` returns, and the next allocation reuses it ⇒ ASan
     * `heap-use-after-free` (`tests/arena-soundness/H2_home_zone_two_hops.extc`).
     *
     * The criterion is the same question and the same quantity as for the arena channel: `nd` = that
     * place must outlive every `mut ref` argument (receiver included) the callee can store into;
     * parameters and globals count as "outside this frame" (0), an escaping local counts as (-1).
     * `ZONE_HOME` = "pass the zone I received all the way down".
     * Only raise, never lower: from `ZONE_HOME` (longest-lived) upward, `zoneLevel` is the block
     * number of the layer. */
    for (size_t i = 0; i < c.eSites.len; i++) {
        EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
        if (!rec || !rec->call || !planCallee(rec->call)) continue;
        if (!planMakesPool(planCallee(rec->call)) || planZoneLevel(rec->call) == 0) continue;
                /* How shallow the destination is: one shared implementation with the arena channel;
         * `overflow` is handled inside it as "lives longest" -- the only difference between the two
         * channels, so it lives in the shared function instead of in two copies. */
        int nd = callSiteMinDestDepth(&c, rec);
        int zwant = (nd <= 0) ? ZONE_HOME : nd;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zfix] call=%s nd=%d lvl=%d -> %d\n",
                    planCallee(rec->call)->name ? planCallee(rec->call)->name : "-",
                    nd, planZoneLevel(rec->call), zwant);
        if (zwant < planZoneLevel(rec->call)) planSetZoneLevel(rec->call, zwant);
    }

        /* ---- the level solve runs a second time ----
     *
     * The first pass ran **before** the `makesPool` closure (see the solve above); at that point
     * `calleeMakesPool` still reads false for every call site, so **no pool-site promotion ever
     * landed**. Replaying the fact table after the closure makes the gate read the real flag;
     * `promoteInto` only lowers levels and the replay is monotone, so the extra pass adds nothing new
     * -- it just lands the promotions that should have landed. */
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
                if (anRefDepth(sy) < siteD) {
                    fprintf(stderr, "[selfcheck] binding `%s` has refDepth=%d, shallower than"
                            " its site (level %d => %d)\n", sy->name, anRefDepth(sy),
                            planArenaLevel(sy->origin), siteD);
                    bad++;
                }
            }
        }
        Vec allF; allFunctions(arena, m, &allF);
        for (size_t i = 0; i < allF.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&allF, i);
            Vec as_ = anArenaSites(f);
            for (size_t j = 0; j < as_.len; j++) {
                Expr *site = *(Expr **)vecAt(&as_, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                int want = arenaDepthOf(planArenaLevel(site));
                if (anRefDepth(site) != want) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: refDepth=%d but"
                            " arenaLevel=%d (should be %d)\n", site->line,
                            f->name ? f->name : "?", anRefDepth(site), planArenaLevel(site), want);
                    bad++;
                }
                if (anMinAt(site) == 0 && planArenaLevel(site) != ARENA_HOME) {
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

        /* Region-acyclicity decision: must run after all function bodies and deferred call sites have
     * settled, because `planCallee` is complete only then. Prints its own diagnostics under
     * `EXTC_DBG_ACYCLIC=1`. */
    computeFnAcyclic(&c);

    return !ctx->hasError;
}

static bool stmtCallsAllocator(Checker *c, Stmt *s);   /* defined below; mutually recursive */
static bool allocInStmt(void *ctx, Stmt *s);
/**
 * @brief Does a function allocate, directly or through its callees?
 *
 * This is the same question as the `needsHome` transitive closure, but it must be answered lazily:
 * the closure only runs after every function has been checked, while a caller checking its own body
 * already needs to know whether a call in front of it can put new storage into a container
 * (`varArray::push` does not allocate itself, its `grow` does). Results are cached in
 * `FuncDef.allocState`; a function currently being computed counts as allocating -- that is the
 * conservative direction and also how cycles are broken.
 *
 * @param c  Checker.
 * @param f  Function to classify; NULL means unknown.
 * @return   True when the body contains a `new` or a callee allocates.
 */
static bool funcAllocates(Checker *c, FuncDef *f) {
    if (!f) return true;                     /* unknown, so assume it allocates */
    if (f->isExtern) return false;           /* an extern function does not touch our arenas */
    if (anAllocState(f) == 1) return true;
    if (anAllocState(f) == 2) return false;
    if (anAllocState(f) == 3) return true;     /* on the cycle being computed => conservative */
    anSetAllocState(f, 3);
    /* The same per-node criterion the closures use (`bodyReaches`), plus the callee half that only
     * this lazy path can afford to ask: it runs while a body is being checked, before the closures
     * exist, and it counts a function already on the stack as allocating, which is the
     * conservative direction for a question asked mid-check. */
    bool r = bodyReaches(f, REACH_ALLOC) || stmtCallsAllocator(c, f->body);
    anSetAllocState(f, r ? 1 : 2);
    return r;
}

typedef struct { Checker *c; } AllocCtx;

/**
 * @brief Does evaluating this expression reach the allocator?
 *
 * `alloc<T>(n)` allocates like `new`; a call counts by whether its callee may allocate
 * (`funcAllocates`, memoized because it recurses through the call graph); everything else is left to
 * the child nodes. Traversal goes through `astWalkExprChildren`.
 *
 * @param ctx  `AllocCtx *`, carrying the checker.
 * @param e    Expression.
 * @return     Callback semantics: false stops the traversal (found), true continues.
 */
static bool allocInExpr(void *ctx, Expr *e) {
    AllocCtx *a = (AllocCtx *)ctx;
    if (exprIsCallKind(e) && (!planCallee(e) || funcAllocates(a->c, planCallee(e)))) return false;   /* found */
    if (e->kind == EX_NEW || e->kind == EX_GENCALL) return false;   /* found */
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement callback for the `allocInExpr` traversal.
 *
 * @param ctx  `AllocCtx *`, carrying the checker.
 * @param s    Statement.
 * @return     Callback semantics: false stops the traversal (found), true continues.
 */
static bool allocInStmt(void *ctx, Stmt *s) {
    AstVisit v = { allocInExpr, allocInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Does any call in this statement possibly reach the allocator?
 *
 * The two traversal functions recurse into each other and share the per-function cache, so the answer
 * is only meaningful after the callee has been checked.
 *
 * @param c  Checker.
 * @param s  Statement; NULL returns false.
 * @return   True when something allocates.
 */
static bool stmtCallsAllocator(Checker *c, Stmt *s) {
    if (!s) return false;
    AllocCtx a = { c };
    return !allocInStmt(&a, s);
}

/**
 * @brief Finish coroutine handling: decide which coroutines need their own place, and reject locals
 *        that outlive a `yield` while carrying external storage.
 *
 * `coroNeedsZone` must be computed for templates and instances alike: an instance shares the
 * template's body, so missing it makes a generic coroutine's allocations land in the **caller's** zone
 * and the task is never freed.
 *
 * @param c  Checker.
 * @param m  Module.
 */
static void coroCheckDeferred(Checker *c, Module *m) {
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!cf || !planIsCoro(cf) || planTemplate(cf)) continue;
        /* A boxed coroutine always has a task: its frame lives in that task's place. */
        planSetCoroNeedsZone(cf, planMakesPool(cf) || planCoroBoxed(cf));
    }
    /* **Instances** need the same answer, so the loop below repeats the computation for them:
     * skipping them leaves a generic coroutine without a place, so allocations made inside it land
     * in the *caller's* zone, and nothing ever releases the task (`live=20000` after 20000 finished
     * tasks; the same omission as `coroNeedsZone` not being computed for an instance).
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
        /* A third shape joins the two above: storage that **provably outlives every frame**.
         * A global, a `effects Ret=0` result, and a **string literal** (static storage) are all
         * outside the frame by construction -- `exprOutOfFrame` is the predicate that already
         * answers that question, so the rule generalizes instead of growing a case list.
         * (Measured cost of not having it: a literal slice bound with `let` could not cross a
         * `yield`, so the workaround was to inline the literal at every call site.) */
        if (d->init && (exprMakesPool(d->init, true) || d->init->kind == EX_NEW ||
                        d->outOfFrame || exprOutOfFrame(c, d->init))) continue;
        ckError(c, d->line,
                "A local that lives across a `yield` may not point at someone else's storage: the C"
                " stack of this call is gone when the coroutine yields. Two fixes: allocate it"
                " yourself in this body (`new` lands in the frame's own arena, a pooled container in"
                " the pool's place), or keep the offset and take the view again after the resume"
                "",
                "`%s` lives across a `yield` and carries a reference", d->cname);
    }
    c->coroDeferred.len = 0;
}

/**
 * @brief Will codegen wrap this statement in a place boundary
 *        (`zoneEnter`/`zoneLeaveTo` + `arena_release`)?
 *
 * The checker needs a **conservative superset**: answering "no" for a block that codegen does wrap
 * would let a `yield` land in an already-reclaimed block, so it only asks the two predicates it
 * already has; codegen's own decision is finer, and being finer here is safe.
 *
 * @param s       Statement; NULL returns false.
 * @param inCoro  True when the statement is inside a coroutine body, which always returns false.
 * @return        True when a place boundary is needed.
 */
bool stmtNeedsPlaceBoundary(Stmt *s, bool inCoro) {
    if (!s) return false;
    /* A coroutine body has no place boundary, for two reasons:
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

/* ---- the frame of a coroutine -------------------------------
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
    /* A `global` declaration's storage is not in any frame, so it must **not** go into the
     * frame: its C form is a block-scope `static` that already lives to the end of the process.
     * Putting it in the frame would make every resume read and write a frame field instead. */
    /* The statement itself, so the layout pass can **read** the syntax facts it needs
     * (`st->u.var.isGlobal`) and the plan results (`planVarInitOutOfFrame(st)`) instead of
     * copying them into this descriptor -- one source of truth, and no field name that the
     * AST-freeze scan has to tell apart from an AST node member. */
    const Stmt *st;
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

/**
 * @brief Push a loop id onto the current loop chain.
 *
 * When the chain gets too deep, set the sentinel: `coroSharesLoop` then answers "shared".
 *
 * @param s   Scan state.
 * @param id  Loop id.
 */
static void coroPushLoop(CoroScan *s, int id) {
    if (s->nchain < CORO_CHAIN_MAX) s->chain[s->nchain++] = id;
    else s->nchain = CORO_CHAIN_MAX + 1;      /* too deep: `coroSharesLoop` then says yes */
}
/**
 * @brief Pop the top of the current loop chain.
 *
 * @param s  Scan state.
 */
static void coroPopLoop(CoroScan *s) {
    if (s->nchain == CORO_CHAIN_MAX + 1) s->nchain = CORO_CHAIN_MAX;
    else if (s->nchain > 0) s->nchain--;
}
/**
 * @brief Copy the current loop chain.
 *
 * @param s      Scan state.
 * @param chain  Output buffer, at least `CORO_CHAIN_MAX` ints.
 * @param n      Output: how many entries were copied.
 */
static void coroCopyChain(CoroScan *s, int *chain, int *n) {
    *n = s->nchain > CORO_CHAIN_MAX ? CORO_CHAIN_MAX : s->nchain;
    for (int i = 0; i < *n; i++) chain[i] = s->chain[i];
}
/**
 * @brief Do two nodes share an enclosing loop?
 *
 * A chain longer than `CORO_CHAIN_MAX` means "assume shared".
 *
 * @param ac  First node's loop chain.
 * @param an  First chain length.
 * @param bc  Second node's loop chain.
 * @param bn  Second chain length.
 * @return    True when they share a loop id.
 */
static bool coroSharesLoop(const int *ac, int an, const int *bc, int bn) {
    for (int i = 0; i < an; i++)
        for (int j = 0; j < bn; j++)
            if (ac[i] == bc[j]) return true;
    return false;
}

static bool coroScanStmt(void *ctx, Stmt *s);
static bool coroScanExpr(void *ctx, Expr *e);

/**
 * @brief Scan callback: record reads of bindings in an expression along with their loop chains.
 *
 * @param ctx  `CoroScan *`.
 * @param e    Expression.
 * @return     True to continue the traversal.
 */
static bool coroScanExpr(void *ctx, Expr *e) {
    CoroScan *s = (CoroScan *)ctx;
    if (e->kind == EX_IDENT && planCName(e)) {
        CoroUse *u = (CoroUse *)vecPush(&s->uses);   /* by value: never a pointer into the vec */
        u->cname = planCName(e);
        u->index = s->index++;
        coroCopyChain(s, u->chain, &u->nchain);
    } else {
        s->index++;
    }
    AstVisit v = { coroScanExpr, coroScanStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Scan callback: record local declarations and `yield` sites, and maintain the loop chain.
 *
 * @param ctx  `CoroScan *`.
 * @param s    Statement.
 * @return     True to continue the traversal.
 */
static bool coroScanStmt(void *ctx, Stmt *s) {
    CoroScan *sc = (CoroScan *)ctx;
    if (s->kind == ST_VAR && planCName(s)) {
        CoroDecl *d = (CoroDecl *)vecPush(&sc->decls);   /* by value */
        d->cname = planCName(s);
        d->type = s->type;
        d->init = s->u.var.init;
        d->st = s;
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

/**
 * @brief Does declaration `d` live across yield `y`?
 *
 * @param s  Scan state.
 * @param d  Local declaration.
 * @param y  Yield site.
 * @return   True when used after `y` or sharing a loop with it.
 */
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

/**
 * @brief Lay out a coroutine's frame and reject what cannot go into it.
 *
 * Runs after the body is checked, when every declaration's type is final. Scan results are stored
 * **by value** in vectors: those vectors grow while being filled, so a pointer to an element would
 * dangle immediately.
 *
 * @param c  Checker.
 * @param f  Coroutine function; returns immediately when it is not a coroutine or has no body.
 */
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
     * are captured at spawn and read back on every step. */
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
        if (d->st && d->st->u.var.isGlobal) continue;   /* `global`: storage is already out of frame ✓ */
        if (typeContainsRef(c->tt, d->type)) {
            /* Safe or not depends on who owns the storage, which the pool fixpoint only settles
             * later; the decision is made in `coroCheckDeferred`. **No `continue`**: the local still
             * belongs in the frame (a resume jumps past its declaration). */
            if (!c->coroDeferred.arena)
                vecInit(&c->coroDeferred, c->arena, sizeof(CoroDeferred *));
            CoroDeferred *dl = arenaAllocZero(c->arena, sizeof *dl);
            dl->cname = d->cname; dl->init = d->init; dl->line = line;
            dl->outOfFrame = d->st ? planVarInitOutOfFrame(d->st) : false;
            *(CoroDeferred **)vecPush(&c->coroDeferred) = dl;
        }
        if (0 && typeContainsRef(c->tt, d->type)) {
            ckError(c, line,
                    "A local that lives across a `yield` may not carry a reference: its storage is"
                    " gone when the coroutine resumes (the C stack of this call is, and a block's"
                    " arena is). Two fixes: allocate it yourself with `new` so it lands in the"
                    " task's own place, or keep the offset and take the view again after the"
                    " resume",
                    "`%s` lives across a `yield` and carries a reference", d->cname);
            continue;
        }
        Param *p = (Param *)vecPush(&f->coroFrame);
        p->name = d->cname;
        p->cname = d->cname;
        /* The frame is laid out from the **body's** recorded declarations, and for an instance that
         * body is still the template's -- so a local declared `var b: box<T>` kept the parameter
         * spelling in the frame: the instance's frame had `box_i64 ret` next to `box_T b` and the C
         * said "incompatible types when assigning to type 'box_i64' from type 'box_T'"). Substitute with the instance's arguments here; the template keeps its
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

/* The pool questions, shared with code generation.
 *
 * `stmtMakesPool` / `exprMakesPool` walk a tree asking "can this reach `extc_pool_new`?" --
 * codegen asks the block-local form to decide whether a block needs a zone bracket, the
 * checker asks the transitive form for the `makesPool` closure. One spelling, two askers
 * (review F11).
 *
 * **The resolver hook is the checker's extension point, and it is the reason this is not in
 * `typelayer.c`**: a generic body's protocol method on a *type parameter* deliberately has
 * no callee recorded (one template body, many instances), so the pure AST answer is "no",
 * while the conservative answer is "yes". The checker installs a resolver that substitutes
 * the instance's type arguments; codegen gets the resolved callee from `planCallee`. With no
 * resolver installed the answer stays conservative. */
/* The resolver hook; there is none by default, which keeps the verdict conservative. */
static FuncDef *(*poolCalleeResolve)(Expr *e) = NULL;
/**
 * @brief Install a callee resolver so the pool-creation verdict can name the real callee of a
 *        template instance.
 *
 * The checker cannot answer "which concrete method is this instance's". One template body with
 * several instances leaves the pure AST answer at "no" while the conservative answer is "yes". With
 * a resolver installed the answer is substituted through the instance's type arguments; without one
 * it stays conservative.
 *
 * @param fn  Resolver; `NULL` restores the conservative behaviour.
 */
void setPoolCalleeResolver(FuncDef *(*fn)(Expr *e)) { poolCalleeResolve = fn; }

/**
 * @brief Can this call reach `extc_pool_new`?
 *
 * It is not the question `calleeMakesPool` asks: that one asks whether the result may live in a
 * pool (escape and promotion) and relaxes for `@poolObject` types, while the block-level zone hook
 * only needs "may this call create a pool". An unresolved callee counts as yes.
 *
 * @param f  Callee; NULL (unresolved) returns true.
 * @return   true when the call may create a pool.
 */
bool calleeCreatesPool(FuncDef *f) {
    if (!f) return true;                       /* unresolved => conservative */
    if (planMakesPool(f)) return true;
    if (f->body == NULL && isPoolCtorName(f->name)) return true;
    return anIsAssoc(f) && f->owner && anMakesPoolAny(f->owner);
}


static bool exprMakesPool(Expr *e, bool descendBlocks);

/**
 * @brief Does any expression in this statement possibly create a pool?
 *
 * @param s              Statement; NULL returns false.
 * @param descendBlocks  Whether to descend into nested blocks and domains: they are places of
 *                       their own with their own zone, so the block-level question stops there.
 * @return               true when a pool may be created.
 */
bool stmtMakesPool(Stmt *s, bool descendBlocks) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMakesPool(s->u.yield_.value, descendBlocks);
    case ST_TRAP:   return exprMakesPool(s->u.trap_.msg, descendBlocks);
    case ST_DROP:   return false;   /* the target is a place, never an allocation */
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

/**
 * @brief May this expression create a pool?
 *
 * All three resolved call shapes are checked, `EX_ASSOC` included. `flush` / `print` / `println`
 * are builtins dispatched by name, and the checker resolves no callee for them, so they have to be
 * named here; otherwise the conservative branch spreads `makesPool` over half the library.
 *
 * @param e              Expression; NULL returns false.
 * @param descendBlocks  Whether to descend into nested blocks and domains.
 * @return               true when a pool may be created.
 */
static bool exprMakesPool(Expr *e, bool descendBlocks) {
    if (!e) return false;
    /* The three shapes a resolved call takes. `EX_ASSOC` belongs here for the reason its
     * counterpart in `exprCallsNeedsHome` documents: an associated function records its
     * callee through `planCallee` too, and leaving it out is how a summary quietly becomes wrong. */
    if (exprIsCallKind(e)) {
        /* `flush()` / `print(x)` / `println(x)` are builtins dispatched by name, and the
         * checker leaves `e->func` null for them (it returns before resolving a callee).
         * They cannot create a pool; naming them here keeps the conservative branch below
         * from spreading `makesPool` over half the library. Measured before this case
         * existed: `io::flushOut`, `io::writeBytesRaw` and `io::errWrite` were all marked,
         * purely because each of them calls `flush()`. */
        bool builtinPrint = false;
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
            const char *bn = e->u.call.callee->u.ident.name;
            if (bn && (strcmp(bn, "flush") == 0 || strcmp(bn, "print") == 0 ||
                       strcmp(bn, "println") == 0))
                builtinPrint = true;
        }
        /* The builtin creates no pool itself, but its **arguments** may (`println(f"...")` runs a
         * value f-string, which calls `string::from` / `+`, and those need a zone). An earlier
         * version returned false here without looking at the arguments, so a frame holding such a
         * `println` was not marked makePool and emitted no `extc_pool_zoneEnter()`, while the call
         * site still passed the fallback `extc_zoneTop` for the zone parameter -- giving the runtime
         * error `the view has no storage`. */
        if (!builtinPrint) {
        /* The question here is "**may this call reach `extc_pool_new`**" (the zone decision, and
         * the `makesPool` closure itself), not "may the result live in a pool" -- that is
         * `calleeMakesPool`'s question (escape and promotion use it), and `calleeCreatesPool`
         * documents the difference.
         *
         * A protocol method on a **type parameter** inside a generic body (`k.hash()`) has
         * **deliberately no func** recorded on the node, so this side also asks the resolver hook,
         * which codegen and the closure's instance round install. */
            FuncDef *cf = planCallee(e);
            if (!cf && e->kind == EX_METHOD && poolCalleeResolve) cf = poolCalleeResolve(e);
            if (calleeCreatesPool(cf)) return true;
        }
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
    case EX_NEW:
        /* `new T[n]` in a `@buffer` context is a call of that type's constructor, which builds the
         * pool the buffer's storage lives on: the block that writes it needs a zone of its own, or
         * the storage is registered on the caller's zone and the value outlives it. */
        if (planNewAsBuffer(e) && planCallee(e)) return true;
        return exprMakesPool(e->u.new_.count, descendBlocks);
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
    /* `dyn Trait(x)` **is** a pool site: `extc_dyn_put` lazily builds an object table in the zone it
     * is handed, and that table holds the copied payload. So the block that constructs a `dyn` value
     * needs a zone of its own -- otherwise the table (and every payload in it) hangs off the enclosing
     * function's zone and cannot be dropped at the end of the block or of the iteration.
     *
     * The payload walk is subsumed by `true`: whatever the payload builds, the enclosing block needs a
     * zone for this value anyway. */
    case EX_DYN: return true;
    default: return false;
    }
}
