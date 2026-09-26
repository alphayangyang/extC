/* Top level: function bodies, generic instantiation, deferred checks.
 *
 * This is the last checking pass. It validates every declaration for duplicate
 * names, computes the effect summary of every function (does it reach an allocator,
 * can a reference it hands out escape), instantiates generic functions at each call
 * site, and checks each function body and global initializer for lifetimes and
 * arena levels.
 */

#include "check_internal.h"
#include "dataflow.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <stdlib.h>   /* getenv (the EXTC_DUMP_EFFECTS debug switch) */

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

static int  paramIndex(FuncDef *f, const char *name);

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
static void resolveSignature(Checker *c, FuncDef *f) {
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
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fa = *(FieldDef **)vecAt(&sd->fields, j);
            for (size_t k = j + 1; k < sd->fields.len; k++) {
                FieldDef *fb = *(FieldDef **)vecAt(&sd->fields, k);
                if (strcmp(fa->name, fb->name) == 0)
                    ckError(c, fb->line, NULL, "struct `%s` has duplicate field `%s`",
                            DN(sd), fb->name);
            }
        }
        for (size_t j = 0; j < sd->methods.len; j++) {
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
        TypeDef *a = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = i + 1; j < m->types.len; j++) {
            TypeDef *b = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate type `%s`", b->name);
        }
        if (a->variants.len == 0)
            ckError(c, a->line, NULL, "type `%s` has no variants", a->name);
        for (size_t j = 0; j < a->variants.len; j++) {
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
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < m->types.len; j++) {
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
        ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
        if (!im->trait || !im->target) continue;
        StructDef *sd = structOf(im->target);
        if (!sd) continue;
        Vec selfParams, selfArgs;
        vecInit(&selfParams, c->arena, sizeof(const char *));
        vecInit(&selfArgs,   c->arena, sizeof(Type *));
        *(const char **)vecPush(&selfParams) = "Self";
        *(Type **)vecPush(&selfArgs) = im->target;
        for (size_t k = 0; k < im->trait->methods.len; k++) {
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
static bool exprHasNew(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NEW: return true;
    /* `alloc<T>(n)` is a builtin primitive (it is the only call the checker lowers to
     * `EX_GENCALL`), and it takes its storage from the block arena exactly like `new`
     * does. Missing this case was a real bug: a function that allocated only inside a
     * nested block,
     *
     *     fn inner(n: i32) { { var q = alloc<i32>(250000)  *q = n } }
     *
     * was classified as needing no arena, so no `__extc_a` was declared, while the
     * block still emitted `&__extc_a[2]` and the generated C did not compile (gcc:
     * `__extc_a` undeclared). Worse, `tests/arena/control-flow.extc` has exactly that
     * shape while the acceptance script only greps for "out of arena memory", so the
     * case was a no-op until the script was tightened as well. */
    case EX_GENCALL: return true;
    case EX_BIN: return exprHasNew(e->u.bin.left) || exprHasNew(e->u.bin.right);
    case EX_UN:  return exprHasNew(e->u.un.operand);
    case EX_REF: return exprHasNew(e->u.ref.operand);
    case EX_DEREF: return exprHasNew(e->u.deref.operand);
    case EX_SIGN:  return exprHasNew(e->u.sign.operand);
    case EX_CONV:  return exprHasNew(e->u.conv.operand);
    case EX_TRY:   return exprHasNew(e->u.try_.operand);
    case EX_INDEX: return exprHasNew(e->u.index.obj) || exprHasNew(e->u.index.index);
    case EX_SLICE: return exprHasNew(e->u.slice.obj) || exprHasNew(e->u.slice.lo) ||
                          exprHasNew(e->u.slice.hi);
    case EX_FIELD: return exprHasNew(e->u.field.obj);
    case EX_METHOD:
        if (exprHasNew(e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprHasNew(*(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_COALESCE:
        return exprHasNew(e->u.coalesce.main) || exprHasNew(e->u.coalesce.fallback);
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprHasNew(*(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprHasNew(*(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasNew(*(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasNew((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasNew(*(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    default: return false;
    }
}
static bool stmtHasNew(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprHasNew(s->u.var.init);
    case ST_ASSIGN: return exprHasNew(s->u.assign.value) || exprHasNew(s->u.assign.target);
    case ST_IF:     return exprHasNew(s->u.ifs.cond) || stmtHasNew(s->u.ifs.thenBody) ||
                           stmtHasNew(s->u.ifs.elseBody);
    case ST_WHILE:  return exprHasNew(s->u.whiles.cond) || stmtHasNew(s->u.whiles.body);
    case ST_RETURN: return exprHasNew(s->u.ret.value);
    case ST_EXPR:   return exprHasNew(s->u.expr.expr);
    case ST_BLOCK: case ST_MATCH: {
        Vec *v = s->kind == ST_BLOCK ? &s->u.block.stmts : NULL;
        if (v) {
            for (size_t i = 0; i < v->len; i++)
                if (stmtHasNew(*(Stmt **)vecAt(v, i))) return true;
            return false;
        }
        if (exprHasNew(s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtHasNew((*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    }
    default: return false;
    }
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
static bool exprCallsNeedsHome(Expr *e, bool precise);
static bool stmtCallsNeedsHome(Stmt *s, bool precise) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprCallsNeedsHome(s->u.var.init, precise);
    case ST_ASSIGN: return exprCallsNeedsHome(s->u.assign.value, precise) ||
                           exprCallsNeedsHome(s->u.assign.target, precise);
    case ST_IF:     return exprCallsNeedsHome(s->u.ifs.cond, precise) ||
                           stmtCallsNeedsHome(s->u.ifs.thenBody, precise) ||
                           stmtCallsNeedsHome(s->u.ifs.elseBody, precise);
    case ST_WHILE:  return exprCallsNeedsHome(s->u.whiles.cond, precise) ||
                           stmtCallsNeedsHome(s->u.whiles.body, precise);
    case ST_RETURN: return exprCallsNeedsHome(s->u.ret.value, precise);
    case ST_EXPR:   return exprCallsNeedsHome(s->u.expr.expr, precise);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtCallsNeedsHome(*(Stmt **)vecAt(&s->u.block.stmts, i), precise)) return true;
        return false;
    case ST_MATCH:
        if (exprCallsNeedsHome(s->u.match.scrutinee, precise)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtCallsNeedsHome((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, precise)) return true;
        return false;
    default: return false;
    }
}
static bool exprCallsNeedsHome(Expr *e, bool precise) {
    if (!e) return false;
    /* An associated call (`EX_ASSOC`) counts as well: an associated function such as
     * `varArray<i32>::withCap(1)` or `bufT<i32>::make(4)` also records its callee in
     * `e->func`, but this walk used to accept only `EX_CALL` and `EX_METHOD`. A
     * function that called an allocating associated function was therefore classified
     * as needing no arena (`mayUseArena = false`, not even an `__extc_a`
     * declaration), while the call site still emitted `&__extc_a[k]`, and the
     * generated C did not compile. Repro:
     *
     *     fn helper() { var b: bufT<i32> = bufT<i32>::make(4) }   // `make` contains `new`
     *
     * The hole was hidden for `main` only, by an `|| is main` fallback in
     * `mayUseArena`, and that fallback had a measurable cost: every `main` carried the
     * whole arena prologue, so every `while` body emitted `extc_arena_release(...)`
     * and a matrix multiply ran 103 ms instead of 34 ms (3.0x slower). Once the root
     * cause is covered here, the fallback is gone. */
    /* `precise` asks the narrow question ("does the callee really take a home arena?"), which is
     * what a signature needs; the wide one is the escape question `needsHome` answers. */
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && e->func &&
        (precise ? e->func->usesHome : e->func->needsHome))
        return true;
    switch (e->kind) {
    case EX_BIN: return exprCallsNeedsHome(e->u.bin.left, precise) || exprCallsNeedsHome(e->u.bin.right, precise);
    case EX_UN:  return exprCallsNeedsHome(e->u.un.operand, precise);
    case EX_REF: return exprCallsNeedsHome(e->u.ref.operand, precise);
    case EX_DEREF: return exprCallsNeedsHome(e->u.deref.operand, precise);
    case EX_SIGN:  return exprCallsNeedsHome(e->u.sign.operand, precise);
    case EX_CONV:  return exprCallsNeedsHome(e->u.conv.operand, precise);
    case EX_TRY:   return exprCallsNeedsHome(e->u.try_.operand, precise);
    case EX_INDEX:
        return exprCallsNeedsHome(e->u.index.obj, precise) || exprCallsNeedsHome(e->u.index.index, precise);
    case EX_SLICE: return exprCallsNeedsHome(e->u.slice.obj, precise) ||
                          exprCallsNeedsHome(e->u.slice.lo, precise) ||
                          exprCallsNeedsHome(e->u.slice.hi, precise);
    case EX_FIELD: return exprCallsNeedsHome(e->u.field.obj, precise);
    case EX_COALESCE:
        return exprCallsNeedsHome(e->u.coalesce.main, precise) ||
               exprCallsNeedsHome(e->u.coalesce.fallback, precise);
    case EX_METHOD: {
        if (exprCallsNeedsHome(e->u.method.recv, precise)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.method.args, i), precise)) return true;
        return false;
    }
    case EX_CALL: {
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.call.args, i), precise)) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.assoc.args, i), precise)) return true;
        return false;
    }
    case EX_NEW: return exprCallsNeedsHome(e->u.new_.count, precise);
    /* A call hidden inside a literal used to be invisible here: this predicate looked
     * only at the direct positions (`EX_CALL`, `EX_METHOD`, `EX_ASSOC`), and the call
     * in `var b: box = { r: mknode() }` was swallowed by `EX_STRUCTLIT`. The
     * transitive closure then reported that the function reaches no callee with a
     * home arena, so the function was not given one, the callee allocated into the
     * caller's block arena (released when the block ends) while the depth recorded
     * for the value said 0, and the pointer dangled. Every shape a call can hide in
     * must therefore be listed: a field initializer, an array element, an enum
     * payload, and the arguments of a builtin generic call. */
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprCallsNeedsHome((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, precise)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.arraylit.elems, i), precise)) return true;
        return false;
    case EX_ENUMVAL:
        /* Payload construction: the call hides in the payload, as in
         * `holder.holding(mknode())`. */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.enumval.args, i), precise)) return true;
        return false;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.gencall.args, i), precise)) return true;
        return false;
    default: return false;
    }
}
/* Does this expression, or this statement, mention the binding whose generated-C name is `cname`?
 *
 * Used for one question only: is a parameter ever read? The list of shapes mirrors
 * `exprCallsNeedsHome`, and for the same reason its comment gives - an expression can hide
 * anywhere, and a case left out here would report a parameter as unused while the body uses it.
 *
 * The comparison is on the *generated* name, so a local that shadows the parameter (which gets
 * `__2` appended) cannot be mistaken for it.
 */
static bool stmtUsesCname(Stmt *s, const char *cname);
static bool exprUsesCname(Expr *e, const char *cname) {
    if (!e) return false;
    if (e->kind == EX_IDENT)
        return e->u.ident.cname && strcmp(e->u.ident.cname, cname) == 0;
    switch (e->kind) {
    case EX_BIN: return exprUsesCname(e->u.bin.left, cname) || exprUsesCname(e->u.bin.right, cname);
    case EX_UN:  return exprUsesCname(e->u.un.operand, cname);
    case EX_REF: return exprUsesCname(e->u.ref.operand, cname);
    case EX_DEREF: return exprUsesCname(e->u.deref.operand, cname);
    case EX_SIGN:  return exprUsesCname(e->u.sign.operand, cname);
    case EX_CONV:  return exprUsesCname(e->u.conv.operand, cname);
    case EX_TRY:   return exprUsesCname(e->u.try_.operand, cname);
    case EX_INDEX:
        return exprUsesCname(e->u.index.obj, cname) || exprUsesCname(e->u.index.index, cname);
    /* `a[lo..hi]` has three expressions, not one, and the bounds were skipped by every
     * walker that asks "what appears in this expression" - including this one, which is
     * why `examples/gomoku-board.extc` reported `lo` and `hi` as never used while its body
     * reads both (`self.cell[y][lo..hi]`). The same hole sat in the neighbours
     * (`exprHasNew`, `exprCallsNeedsHome`, `markNamesInExpr`, `collectEffectsExpr`,
     * `exprCallsAllocator`, codegen's `collectOwCallsExpr`, and the loader's `rwExpr`), and
     * in each of them it is a real defect, not just a missing warning: a call in a bound
     * belongs in the function's effect summary, and a qualified name in a bound was never
     * rewritten. Walkers that ask what *place* or what *lifetime* an expression denotes
     * still look at the object alone - bounds are values, and `obligExpr` was the one that
     * had it right all along. */
    case EX_SLICE: return exprUsesCname(e->u.slice.obj, cname) ||
                          exprUsesCname(e->u.slice.lo, cname) ||
                          exprUsesCname(e->u.slice.hi, cname);
    case EX_FIELD: return exprUsesCname(e->u.field.obj, cname);
    case EX_COALESCE:
        return exprUsesCname(e->u.coalesce.main, cname) ||
               exprUsesCname(e->u.coalesce.fallback, cname);
    case EX_METHOD: {
        if (exprUsesCname(e->u.method.recv, cname)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.method.args, i), cname)) return true;
        return false;
    }
    case EX_CALL: {
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.call.args, i), cname)) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.assoc.args, i), cname)) return true;
        return false;
    }
    case EX_NEW: return exprUsesCname(e->u.new_.count, cname);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprUsesCname((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, cname)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.arraylit.elems, i), cname)) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.enumval.args, i), cname)) return true;
        return false;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprUsesCname(*(Expr **)vecAt(&e->u.gencall.args, i), cname)) return true;
        return false;
    default: return false;
    }
}

static bool stmtUsesCname(Stmt *s, const char *cname) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprUsesCname(s->u.var.init, cname);
    case ST_ASSIGN: return exprUsesCname(s->u.assign.target, cname) ||
                           exprUsesCname(s->u.assign.value, cname);
    case ST_IF:     return exprUsesCname(s->u.ifs.cond, cname) ||
                           stmtUsesCname(s->u.ifs.thenBody, cname) ||
                           stmtUsesCname(s->u.ifs.elseBody, cname);
    case ST_WHILE:  return exprUsesCname(s->u.whiles.cond, cname) ||
                           stmtUsesCname(s->u.whiles.body, cname);
    case ST_RETURN: return exprUsesCname(s->u.ret.value, cname);
    case ST_EXPR:   return exprUsesCname(s->u.expr.expr, cname);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtUsesCname(*(Stmt **)vecAt(&s->u.block.stmts, i), cname)) return true;
        return false;
    case ST_MATCH:
        if (exprUsesCname(s->u.match.scrutinee, cname)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtUsesCname((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, cname)) return true;
        return false;
    default: return false;
    }
}

static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body, false); }
static bool callsUsesHome(Stmt *body)  { return stmtCallsNeedsHome(body, true); }

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
    case ST_VAR:    return exprMakesPool(s->u.var.init, descendBlocks);
    case ST_ASSIGN: return exprMakesPool(s->u.assign.value, descendBlocks) ||
                           exprMakesPool(s->u.assign.target, descendBlocks);
    case ST_IF:     return exprMakesPool(s->u.ifs.cond, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.thenBody, descendBlocks) ||
                           stmtMakesPool(s->u.ifs.elseBody, descendBlocks);
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
    while (e) {
        if (e->kind == EX_IDENT) return e->u.ident.name;
        if (e->kind == EX_FIELD) { e = e->u.field.obj; continue; }
        if (e->kind == EX_INDEX) { e = e->u.index.obj; continue; }
        if (e->kind == EX_DEREF) { e = e->u.deref.operand; continue; }
        return NULL;
    }
    return NULL;
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
 *   - cycle guard: a function that is being computed right now (`effState == 3`) is in
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
    if (f->effState == 1) return f->effComplete;
    if (f->effState == 3) { f->effComplete = false; return false; }   /* a cycle => incomplete */
    f->effState = 3;
    bool complete = !f->effUnknown;
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
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
    f->effState = 1;
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects-closed] %-20s complete=%d toParam[Addr=0x%x Cont=0x%x] toHome[Addr=0x%x Cont=0x%x] other=0x%x\n",
                FN(f), (int)f->effComplete, f->addrMask, f->contMask,
                f->homeAddrMask, f->homeContMask, f->otherMask);
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
        unsigned stored = callee->addrMask | callee->contMask | callee->otherMask;
        if (stored == 0) return;                 /* the signature says nothing is stored */
        for (size_t j = 0; j < params->len && j < args->len && j < 32; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || p->type->kind != TY_REF) continue;   /* a scalar has no lifetime */
            Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
            int d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, a);
            if (d == 0) continue;                /* it already outlives this frame */
            ckError(c, line,
                    "A C function is a black box: unless the `extern!` declaration says it does"
                    " not keep the pointer (`effects Addr=0 Cont=0`), it may store it somewhere"
                    " that outlives this frame. Sign the declaration, or pass something that"
                    " lives longer.",
                    "argument %zu of `%s` may be kept by C forever, but it points into this"
                    " frame (depth %d)", j + 1, fname, d);
        }
        return;
    }

    bool complete = callee && computeEffectsTransitive(c, callee);
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
    int h;
    if (homeDepth != 0) h = (homeDepth < 0) ? 0 : homeDepth;
    else h = (c->curFunc && c->curFunc->needsHome) ? 0 : c->scopes.len;

    for (size_t i = 0; addrMaybe && i < params->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* generic: defer */
        /* An argument is not always a place (the typical case: `stash(ref l, new node)`
         * passes a `new` directly). `placeDepth` answers 0 for a non-place, and that
         * number is read as "lives forever", so the rule would check nothing. The
         * lifetime of such an argument is taken from the level of the arena block it was
         * allocated in. */
        int d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, place);
        /* When the value is out of reach, first try to promote it (the same rule as on
         * assignment edges): if it cannot be promoted, the error below stands as it is;
         * if it can, the value really does live to this level. */
        if (d != 0 && d > h && promoteInto(c, place, h))
            d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, place);
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
            int d = exprRefDepth(c, a);
            if (d != 0 && d > h && promoteInto(c, a, h)) d = exprRefDepth(c, a);
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
        unsigned cont = callee->contMask | callee->homeContMask
                      | callee->addrMask | callee->homeAddrMask;
        for (size_t j = 0; j < args->len && j < 32; j++) {
            if (!((cont >> j) & 1u)) continue;
            Param *pj = *(Param **)vecAt(params, j);
            if (pj->type && pj->type->kind == TY_REF) continue;   /* `ref` arg: checked above */
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue;
            recordLvlFact(c, a, h);         /* 同上：先记事实，末轮重放时再提（PLAN #87） */
            int d = exprRefDepth(c, a);
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
            if (d != 0 && d > h && promoteInto(c, a, h)) d = exprRefDepth(c, a);
            if (d == 0 || d <= h) continue;
            ckError(c, line,
                    "The callee stores what this value points at into a container it was"
                    " given, so that data must live at least as long as that container.",
                    "argument %zu of `%s` carries a reference into a deeper scope (depth %d)"
                    " than the place the callee may store it (depth %d)", j + 1, fname, d, h);
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
 * 为什么不能只看 `f->makesPool`：泛型方法的**模板**与**实例**是两份 FuncDef，闭包只走到
 * 模板那一份，而调用点上 `e->func` 可能是模板（实测 `owner=vector$vector makesPool=0`，
 * 同一处 codegen 读到的实例却是真）。结构体那一格 `makesPoolAny` 是闭包顺手算的，
 * 只对**关联函数**（构造函数那一族：`new` / `withCap` / `withParent`）放宽 ——
 * 别的库方法（`get` / `asSlice` 之类）不进这一族，免得把"不可提权"的值误判成站点。 */
bool calleeMakesPool(FuncDef *f) {
    if (!f) return false;
    /* **声明优先**（作者口径 2026-09-26）：`@poolObject` 标在 struct 上，说"这个类型拥有一个池"。
     * 它比推断可靠 —— 早先试过"看有没有 `pid` 字段"（魔数）与"看方法建不建池"（要等闭包）。 */
    if (f->owner && f->owner->poolObject) return true;
    if (f->makesPool) return true;
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
 *   ② 「这一次调用**会不会建**池」是**被调者**的性质（`f->makesPool` + 构造器那一族）。
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
    if (f->makesPool) return true;
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
    if (e->zoneLevel == 0 || lvl < e->zoneLevel) e->zoneLevel = lvl;
}

void setCallArenaArg(Checker *c, Expr *e) {
    if (!e) return;
    if (e->homeDepth == -1) {                 /* destination at the home level: my home arena */
        e->arenaArg = ARENA_HOME;
        e->arenaArgPending = false;
    } else if (e->homeDepth >= 1) {           /* an explicit block level: use that arena */
        e->arenaArg = e->homeDepth;
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
        e->arenaArg = (int)c->scopes.len;
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
        int d = placeDepth(c, place);   /* a parameter reports 0, a local its block depth */
        if (d == 0) d = -1;             /* the place is in a parameter: pass my home arena */
        else {
            /* Will this argument be moved out of the current function? If so, the home
             * arena (the arena the caller of this function passes) must live longer than
             * anywhere its contents can go, so pass my home arena. If not, keep the arena
             * the argument already lives in, which preserves the tightness gained from
             * choosing the arena by escape. */
            const char *rn = placeRootName(place);
            if (rn && isEscapeeName(c, rn)) d = -1;
            /* This answer depends on E, which may not be final yet, so record it and
             * recompute it in the final pass. */
            if (callNode && c->eSites.arena) {
                if (!rec) {
                    rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                    rec->call = callNode;
                    *(EArenaSite **)vecPush(&c->eSites) = rec;
                }
                if (rec->n < 8) {
                    rec->argRoot[rec->n]  = (char *)rn;
                    rec->argDepth[rec->n] = d;
                    rec->n++;
                } else {
                    rec->overflow = true;   /* conservative final pass; no silent truncation */
                }
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
/* Is the initial value something freshly allocated in this frame?
 *
 * A `new` counts, and so does a struct literal whose every field is itself fresh or a
 * scalar.
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
                    site->line, site->arenaLevel, site->lexicalLevel,
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

static void collectFreshLocals(Arena *a, Vec *fresh, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (exprIsFresh(s->u.var.init)) *(const char **)vecPush(fresh) = s->u.var.name;
        return;
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
        if (rn && (isEscapeeName(c, rn) || paramIndex(f, rn) >= 0))
            grew |= markNamesInExpr(c, s->u.assign.value);
        return grew;
    }
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
        switch (s->u.expr.expr->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: {
            FuncDef *cf = s->u.expr.expr->func;
            if (!cf) return false;
            unsigned pub = cf->addrMask | cf->contMask | cf->otherMask
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
            return markNamesInExpr(c, s->u.expr.expr);
        }
        default:
            return false;                      /* other expression statements do not escape */
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
    case EX_IDENT: return addEscapee(c, e->u.ident.name);
    case EX_BIN:   grew |= markNamesInExpr(c, e->u.bin.left);
                   grew |= markNamesInExpr(c, e->u.bin.right); return grew;
    case EX_UN:    return markNamesInExpr(c, e->u.un.operand);
    case EX_REF:   return markNamesInExpr(c, e->u.ref.operand);
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
/* Position of a named parameter, or -1.
 *
 * Params:
 *   f    - function to search; may be NULL
 *   name - parameter name; may be NULL
 *
 * Returns:
 *   Zero-based index of the parameter, or -1 when it is not a parameter.
 */
static int paramIndex(FuncDef *f, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, name) == 0) return (int)i;
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
    /* Address flow: the address of a place is stored, so that place has to live at least
     * as long as the container it is stored into. */
    if (e->kind == EX_REF) {
        int j = paramIndex(f, placeRootName(e->u.ref.operand));
        if (j >= 0) {
            if (intoHome) f->homeAddrMask |= (1u << j);
            else if (i >= 0) f->addrMask |= (1u << j);
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
        int j = paramIndex(f, placeRootName(e));
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
                    if (intoHome) f->homeAddrMask |= (1u << j);
                    else if (i >= 0) f->addrMask |= (1u << j);
                } else {                          /* content flow: pointer read from a container */
                    if (intoHome) f->homeContMask |= (1u << j);
                    else if (i >= 0) f->contMask |= (1u << j);
                }
            }
            return;
        }
    }
    if (e->kind == EX_NEW) return;                /* freshly allocated, so it is trivially safe */
    { const char *vr = placeRootName(e);          /* `l.head = n`: the source is a fresh local */
      if (vr && vecHasName(fresh, vr)) return; }
    if (c && e->type && !typeContainsRef(c->tt, e->type)) return;   /* scalar, trivially safe */
    if (i >= 0) f->otherMask |= (1u << i);        /* not sure, so stay conservative */
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
    case ST_ASSIGN: {
        /* Is the target a container named by a `mut ref` parameter?
        * `l.head = ...`, `*p = ...`, and `l.buf[i] = ...` all are. */
        const char *dstRoot = placeRootName(s->u.assign.target);
        int i = paramIndex(f, dstRoot);
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
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && !e->func)
        f->effUnknown = true;   /* unresolved -> the summary stays incomplete, conservatively */
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
    case EX_GENCALL:
        for (size_t k = 0; k < e->u.gencall.args.len; k++)
            collectEffectsExpr(c, f, *(Expr **)vecAt(&e->u.gencall.args, k));
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
            unsigned all = 0;
            for (size_t i = 0; i < f->params.len && i < 32; i++) all |= (1u << i);
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
    c->escapeesFor = (int)(size_t)f;   /* marks "already computed", keyed by address */
    if (getenv("EXTC_DUMP_EFFECTS")) fprintf(stderr, "[escapes-for] %s\n", FN(f));
    Vec fresh; vecInit(&fresh, c->arena, sizeof(const char *));
    collectFreshLocals(c->arena, &fresh, f->body);
    f->freshCount = (unsigned)fresh.len;
    collectEffectsStmt(c, f, f->body, &fresh);
    if (getenv("EXTC_DUMP_EFFECTS"))
        fprintf(stderr, "[effects] %-22s toParam[Addr=0x%x Cont=0x%x Other=0x%x] toHome[Addr=0x%x Cont=0x%x] localAddr=%d fresh=%u callees=%zu\n",
                FN(f), f->addrMask, f->contMask, f->otherMask,
                f->homeAddrMask, f->homeContMask,
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
* `exprRefDepth` cannot replace this: it calls `lookup` for a binding, and `runRefCheck`
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


static int solvedValDepth(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        /* Do not fall back to `refDepth`: that number can be stale and would then
        * contradict the arena level. */
        return arenaDepthOf(e->arenaLevel);
    case EX_IDENT: case EX_FIELD: case EX_INDEX:
        return e->refDepth > 0 ? e->refDepth : 0;
    case EX_SIGN:     return solvedValDepth(e->u.sign.operand);
    case EX_DEREF:    return solvedValDepth(e->u.deref.operand);
    case EX_SLICE:    return solvedValDepth(e->u.slice.obj);
    case EX_COALESCE: {
        int a = solvedValDepth(e->u.coalesce.main);
        int b = solvedValDepth(e->u.coalesce.fallback);
        return a > b ? a : b;
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++) {
            int x = solvedValDepth((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
            int x = solvedValDepth(*(Expr **)vecAt(&e->u.arraylit.elems, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++) {
            int x = solvedValDepth(*(Expr **)vecAt(&e->u.enumval.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    case EX_CALL: {
        int d = 0;
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            int x = solvedValDepth(*(Expr **)vecAt(&e->u.call.args, i));
            if (x > d) d = x;
        }
        return d;
    }
    default: return 0;
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
} LvlState;

static int  symLevel(LvlState *ls, Sym *sy);
static bool setSymLevel(LvlState *ls, Sym *sy, int lv);
static int  valueLevel(Checker *c, LvlState *ls, Expr *val, int hops);

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
        for (size_t i = 0; i < c->stores.len; i++) {
            StoreSite *alt = *(StoreSite **)vecAt(&c->stores, i);
            if (!alt || !alt->target || alt->target->kind != EX_IDENT) continue;
            if (identBindOf(alt->target) != sy) continue;
            viaRecords = true;
            int v = levelOfValue(c, ls, alt->value, inner, hops + 1);
            if (v < best) best = v;
        }
        /* A binding with no publication of its own -- only ever initialized, or written
         * through a projection -- is still described by the expression it was given. */
        if (!viaRecords) {
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
    for (size_t i = 0; i < ls->tbl.len; i++) {
        SymLevel *e = (SymLevel *)vecAt(&ls->tbl, i);
        if (e->sym == sy) return e->lv;
    }
    return LEVEL_INF;
}

/* Lower a binding's level, and report whether that moved it. Only the lowering direction
 * exists: a level is a requirement, and requirements accumulate. */
static bool setSymLevel(LvlState *ls, Sym *sy, int lv) {
    if (!ls || !sy || lv >= LEVEL_INF) return false;
    for (size_t i = 0; i < ls->tbl.len; i++) {
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
 *   dfr - the data-flow fixed point for the same body; its depths are final by now, so a
 *         reader of them sees a number that does not depend on traversal order
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
static void levelPass(Checker *c, FuncDef *f, const DfResult *dfr) {
    (void)dfr;
    LvlState ls;
    vecInit(&ls.tbl, c->arena, sizeof(SymLevel));
    vecInit(&ls.path, c->arena, sizeof(LvlVisit));
    ls.steps = 0;
    ls.loud = false;
    ls.fn = f;

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
        for (size_t i = 0; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (!st || !st->target || st->target->kind != EX_IDENT) continue;
            Sym *d = identBindOf(st->target);
            if (!d) continue;
            int at = st->at;
            int v  = valueLevel(c, &ls, st->value, 0);
            if (v < at) at = v;
            if (setSymLevel(&ls, d, at)) moved = true;
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
        for (size_t i = 0; i < c->stores.len; i++) {
            StoreSite *st = *(StoreSite **)vecAt(&c->stores, i);
            if (!st) continue;
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
                for (size_t k = 0; k < c->stores.len; k++) {
                    StoreSite *alt = *(StoreSite **)vecAt(&c->stores, k);
                    if (!alt || alt == st || !alt->target) continue;
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
            if (dst && dl < st->at && setSymLevel(&ls, dst, at)) moved = true;
        }
        if (!moved) break;
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
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            obligExpr(c, *(Expr **)vecAt(&e->u.enumval.args, i), obs, true);
        return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            obligExpr(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, obs, true);
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

static void checkFunc(Checker *c, FuncDef *f) {
    /* An extern declaration has no body, so only its signature is checked: the parameter
     * types were already resolved elsewhere, and no arena is involved, since the function
     * takes no arena parameter and needs no home arena (the arena the caller passes). Its
     * summary comes from the signed clause, or is the worst case (see `collectEffects`
     * below). */
    if (f->isExtern) {
        f->mayUseArena = false;
        f->needsHome   = false;
        /* Only scalars and single pointers may cross the boundary. A `slice<T>` would
        * become two C arguments (data and length), so the names would not match, and a
        * struct has no frozen layout; the stdlib wrappers pass `s.data` and `s.len`
        * explicitly. */
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            Type *t = ttBase(p->type);
            bool ok = t && (t->kind == TY_BUILTIN ||
                            (t->kind == TY_REF && ttBase(t->inner) &&
                             ttBase(t->inner)->kind == TY_BUILTIN));
            if (!ok)
                ckError(c, p->line,
                        "A C function's arguments are scalars and single pointers. A `slice<T>`"
                        " becomes two C arguments (data + len), and a struct's layout is not"
                        " frozen -- wrap those in an extC function that passes `s.data` / `s.len`"
                        " explicitly.",
                        "`extern!` argument %zu has type `%s`, which cannot cross the C boundary",
                        i + 1, typeStr(c, p->type));
        }
        {
            Type *r = f->ret ? ttBase(f->ret) : NULL;
            /* `void` is a C return type like any other -- `exit`, `free`, `srand` and
             * `cfmakeraw` all have it, and refusing it left the privileged layer unable to
             * declare them. What has no C-level representation is a struct, a slice (two
             * arguments), or a view. */
            bool ok = !r || r->kind == TY_VOID || r->kind == TY_BUILTIN ||
                      (r->kind == TY_REF && ttBase(r->inner) && ttBase(r->inner)->kind == TY_BUILTIN);
            if (!ok)
                ckError(c, f->line,
                        "A C function can return `void`, a scalar, or a single pointer; anything"
                        " else has no C-level representation here.",
                        "`extern!` return type `%s` cannot cross the C boundary",
                        typeStr(c, f->ret));
        }
        collectEffects(c, f);            /* summary = the signed clause, or the worst case */
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

    c->curFunc = f;
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

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* A parameter is mutable: it is a local copy handed over by the caller, as in C,
         * so `ref real` is allowed inside `fn f(real: Board)`. */
        Sym *sym = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sym->cname;
    }
    /* The body does not open a scope of its own: parameters and body locals share one.
     * Shadowing a parameter with `let a = ...` is therefore only a new name, which is
     * legal and becomes `a__2` in the generated C, while `var a = ...` declares a second
     * piece of storage and is rejected (see `declare`). */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

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
        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, f, &dfr);
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
        if (!dfr.overflow) {
            for (size_t i = 0; i < c->allSyms.len; i++) {
                Sym *sy = *(Sym **)vecAt(&c->allSyms, i);
                if (!sy || sy->line < 0) continue;
                int d = dfLookup(&dfr, sy->cname);
                if (d > sy->refDepth) sy->refDepth = d;
            }
        }
    }
    popScope(c);
    c->curFunc = savedFunc;
    c->curParams = savedParams;
    c->ctx = savedCtx;
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
    /* Already built? One template plus one set of type arguments is one instance, so a
     * match is returned instead of allocating a second one. */
    for (size_t i = 0; i < c->funcInsts.len; i++) {
        FuncDef *in = *(FuncDef **)vecAt(&c->funcInsts, i);
        if (in->tmpl != tmpl || in->targs.len != targs->len) continue;
        bool same = true;
        for (size_t j = 0; j < targs->len; j++)
            if (!ttEquals(*(Type **)vecAt(&in->targs, j), *(Type **)vecAt(targs, j))) { same = false; break; }
        if (same) return in;
    }
    FuncDef *in = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    *in = *tmpl;                        /* shallow copy: shares the body (as method instances do) */
    in->tmpl = tmpl;
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
    /* C name: `max_i32`, built by mangling the type arguments onto the name. */
    Buf b;
    bufInit(&b, c->arena);
    bufPuts(&b, tmpl->name);
    for (size_t j = 0; j < targs->len; j++) {
        bufPutc(&b, '_');
        bufPuts(&b, ttMangle(c->tt, *(Type **)vecAt(targs, j)));
    }
    in->instName = bufCstr(&b);
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
    /* A `T op T` in the template is an instance's `A op B` here, and an operator method is
     * matched on the right operand's type, so the two only have to be equal when the left
     * operand is not a struct. */
    if (!ttEquals(lt, rt2) && !structOf(ttBase(lt))) return;
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
    if (!fi || !fi->tmpl) return false;
    if (rc->func != fi->tmpl) return false;
    return fi->tmpl->typeParams.len > 0;
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
            int now = solvedValDepth(rc->val);
            if (now < rc->depth) rc->depth = now;
        }
        if (dbgOn("EXTC_DBG_AT"))
            fprintf(stderr, "[at] %s what=%s depth=%d at=%d kind=%d dca=%d svd=%d\n",
                    instName, rc->what, rc->depth, rc->at, (int)rc->val->kind,
                    depthComesFromAlloc2(c, rc->val, 0)?1:0, solvedValDepth(rc->val));
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
static void resolveDeferredCall(Checker *c, CallCheck *cc, Vec *params, Vec *targs) {
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
    cc->node->func = inst;
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
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    Checker c;    memset(&c, 0, sizeof c);
    vecInit(&c.funcInsts, arena, sizeof(void *));   /* free function instances */
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    vecInit(&c.scopes, arena, sizeof(void *));
    vecInit(&c.opChecks, arena, sizeof(void *));
    vecInit(&c.methodChecks, arena, sizeof(void *));   /* #57: method calls on a type parameter */
    vecInit(&c.deferredUses, arena, sizeof(void *));   /* #79: uses of a deferred call's result */
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
                if (cand->name && strcmp(cand->name, im->traitName) == 0) { tr = cand; break; }
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
        Type *t = ttResolve(tt, ctx, typeNamed(arena, im->typeName), im->line, NULL);
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
            if (sd->typeParams.len > 0) {
                ctxError(ctx, im->line, 1,
                         "A generic type's methods are declared inside its own body; extending it "
                         "from outside needs the block's own type parameters.",
                         "`impl` on generic type `%s` is not supported yet", im->typeName);
                continue;
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
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            for (size_t k = 0; k < v->types.len; k++)
                *(Type **)vecAt(&v->types, k) =
                    ttResolve(tt, ctx, *(Type **)vecAt(&v->types, k), v->line, &td->typeParams);
        }
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
            fd->type = ttResolve(tt, ctx, fd->type, fd->line, &sd->typeParams);
        }
        for (size_t j = 0; j < sd->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        resolveSignature(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* Trait method signatures are resolved here for the same reason struct methods are: the
     * conformance check compares resolved types on both sides. `Self` resolves because the
     * parser gave every signature that type parameter (see parseTrait) -- which is the whole
     * implementation of "`Self` is an implicit type parameter". */
    for (size_t i = 0; i < m->traits.len; i++) {
        TraitDef *td = *(TraitDef **)vecAt(&m->traits, i);
        for (size_t j = 0; j < td->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&td->methods, j));
    }


    checkGlobals(&c);
    checkDeclarations(&c);

    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkMethodShape(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (fx->tmpl) continue;              /* an instance is not checked separately */
        checkMethodShape(&c, fx);
    }

    /* Second pass: check function bodies. */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (fx->tmpl) continue;              /* covered by the per-instance recheck */
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
    for (size_t i = 0; i < c.callChecks.len; i++) {
        CallCheck *cc = *(CallCheck **)vecAt(&c.callChecks, i);
        if (!cc->node || !cc->tmpl) continue;
        /* (1) The enclosing body is a free-function instance. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (fi->tmpl != cc->func) continue;       /* not a call in this template body */
            resolveDeferredCall(&c, cc, &fi->tmpl->typeParams, &fi->targs);
        }
        /* (2) The enclosing body is a type instance (a generic call inside a method);
         * `owner` is the struct or enum definition. */
        StructDef *owner = cc->func ? cc->func->owner : NULL;
        if (!owner) continue;
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;
            resolveDeferredCall(&c, cc, &owner->typeParams, &inst->targs);
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
            if (ec->func != fi->tmpl) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runOpCheck(&c, ec, &fi->tmpl->typeParams, &fi->targs, fi->instName);
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
            if (mc->func != fi->tmpl) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runMethodCheck(&c, mc, &fi->tmpl->typeParams, &fi->targs, fi->instName);
        }
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
            if (du->func != fi->tmpl) continue;
            if (provisionalInstance(&fi->targs)) continue;
            runDeferredUse(&c, du, &fi->tmpl->typeParams, &fi->targs, fi->instName);
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
     * opaque `T`, so `exprRefDepth` / `exprBorrowed` return early for it --
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
            c.substParams = &fi->tmpl->typeParams;
            c.substArgs   = &fi->targs;
            runRefCheck(&c, rc, fi->instName);
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
        c.substParams = &fi->tmpl->typeParams;
        c.substArgs   = &fi->targs;
        fi->addrMask = fi->contMask = fi->otherMask = 0;
        fi->homeAddrMask = fi->homeContMask = 0;
        fi->addrFromLocal = false;
        fi->freshCount = 0;
        fi->effState = 0;  fi->effComplete = false;  fi->effUnknown = false;
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
            f->effState = 0;  f->effComplete = false;  f->effUnknown = false;
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

    /* ---------------------------------------- transitive closure of `needsHome`
     * Calling a function that needs a home arena (the arena the caller passes) means the
     * caller must have something to pass (`__extc_home`, or `&__extc_a[current block]`),
     * so the caller needs a home arena of its own.
     * Iterate to a fixed point; there are few functions, so extra rounds are cheap. */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (f->needsHome || f->isExtern || !f->body) continue;   /* an extern has no body */
            if (callsNeedsHome(f->body)) { f->needsHome = true; changed = true; }
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (f->needsHome) continue;
                if (callsNeedsHome(f->body)) { f->needsHome = true; changed = true; }
            }
        }
    }

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
        Vec all; vecInit(&all, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&sd->methods, j);
        }
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
            if (f->tmpl) f->arenaSites = f->tmpl->arenaSites;
        }

        /* Recompute the direct `needsHome` criterion for every function, uniformly.
         *
         * The criterion in `checkFunc` (the body allocates and the return type carries a
         * reference) is evaluated only while that body is being checked, and a generic
         * instance's body is never checked on its own (`checkFunc` returns early for
         * `fx->tmpl`). An instance's `needsHome` is therefore only the shallow copy made
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
                rec->call->arenaArg = ARENA_HOME;
                rec->call->arenaArgPending = false;
                continue;
            }
            int nd = 0;
            for (int k = 0; k < rec->n; k++) {
                int d = rec->argDepth[k];        /* 0 = parameter/global: outside this frame */
                /* Only the depths whose argument is local to this frame consult the escape
                 * set; an argument at depth 0 already forces the home arena. */
                if (d != 0 && rec->argRoot[k] && isEscapeeName(&c, rec->argRoot[k])) d = -1;
                if (nd == 0 || d < nd) nd = d;
            }
            if (nd == 0 && rec->n > 0) nd = rec->n ? rec->argDepth[0] : 0;
            /* Write the new homeDepth onto `arenaArg`, by the same rules as `setCallArenaArg`. */
            int want = (nd <= 0) ? ARENA_HOME : nd;
            if (rec->call->arenaArg != want) { rec->call->arenaArg = want; eFixed++; }
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
                                f->name ? f->name : "?", site->line, site->minAt, site->arenaLevel);
                    if (dbgOn("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                site->arenaLevel, f->needsHome?1:0, (int)site->kind);
                    if (dbgOn("EXTC_DBG_S3"))
                        fprintf(stderr, "[s3] %-8s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                site->arenaLevel, f->needsHome?1:0, (int)site->kind);
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
                    if (site->arenaLevel != want) {
                        site->arenaLevel = want;
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

    /* ---------------------------------------- the precise question a signature needs
     * `needsHome` answers an *escape* question and is deliberately wide: a `new` whose value is
     * copied into an out-parameter marks the function even though nothing escapes. The hidden
     * parameter is only needed when the body really reads it, and the placement pass above has
     * just decided that per site: a site at `ARENA_HOME`, or a call that hands this function's
     * home on to a callee that takes one. Signatures and call sites both use this flag, so a
     * function that only ever allocates inside its own blocks stops carrying a parameter nobody
     * reads - `examples/out-param.extc` carried one, and gcc reported it as unused. */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (f->usesHome || f->isExtern || !f->body) continue;
            bool u = callsUsesHome(f->body);
            for (size_t j = 0; !u && j < f->arenaSites.len; j++)
                if ((*(Expr **)vecAt(&f->arenaSites, j))->arenaLevel == ARENA_HOME) u = true;
            if (u) { f->usesHome = true; changed = true; }
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (f->usesHome || !f->body) continue;
                bool u = callsUsesHome(f->body);
                for (size_t k = 0; !u && k < f->arenaSites.len; k++)
                    if ((*(Expr **)vecAt(&f->arenaSites, k))->arenaLevel == ARENA_HOME) u = true;
                if (u) { f->usesHome = true; changed = true; }
            }
        }
    }

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
            if (!site->arenaArgPending || !site->func || !site->func->usesHome) continue;
            site->arenaArg = ARENA_HOME;
            site->arenaArgPending = false;
        }
    }

        /* Recompute every site's `refDepth` from its final level, whether or not the level
         * changed.
         *
         * Why (measured with gdb on `tests/arena-promoted/C2_if_join_refbinding`): the
         * `site->refDepth = ...` in the placement loop above sits inside
         * `if (site->arenaLevel != want)`. For a site that the provisional pass had already
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
                site->refDepth = arenaDepthOf(site->arenaLevel);   /* keep the two in sync */
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
                fprintf(stderr, "[home] %-24s uses=%d needs=%d tmpl=%-12s sites=%zu",
                        f->instName ? f->instName : f->name, (int)f->usesHome, (int)f->needsHome,
                        f->tmpl ? (f->tmpl->instName ? f->tmpl->instName : f->tmpl->name) : "-",
                        f->arenaSites.len);
                for (size_t j = 0; j < f->arenaSites.len; j++)
                    fprintf(stderr, " L%d", (*(Expr **)vecAt(&f->arenaSites, j))->arenaLevel);
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
            rc->depth = solvedValDepth(rc->val);
        }
        if (getenv("EXTC_DUMP_OW"))
            fprintf(stderr, "[arena] single source of truth: %d site(s) (`new` and call sites) placed in a home arena\n", fixed);
    }

    /* ---- Which storage an `@overwrite` cell gets, and how many sites it has ----
     * Must run after every function body has been checked: whether a function can reach
     * itself is decided from the call graph. */
    {
        Vec all; vecInit(&all, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++) *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            f->owSites = countOwSites(f->body);
            /* Set `owLocal` even when there are no sites: otherwise the signature of a
             * function with no parameters and no home arena loses its `void` (a golden test
             * caught this immediately). */
            /* An `@overwrite` cell always lives in the current frame. */
            f->owLocal = true;
            (void)funcReachesItself;
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
        if (f->isExtern || !f->body) { f->mayUseArena = false; continue; } /* no body, no arena */
        f->mayUseArena = stmtHasNew(f->body) || callsNeedsHome(f->body);
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            f->mayUseArena = stmtHasNew(f->body) || callsNeedsHome(f->body);
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
     * none of them can create a pool at all. */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (f->makesPool) continue;
            /* An extern has no body to walk; `stmtMakesPool` finds the callee at the call
             * site (`extc_pool_new` itself is declared extern and is exactly that case). */
            if (stmtMakesPool(f->body, true)) { f->makesPool = true; changed = true; }
        }
        /* **泛型 struct 的模板轮要跳过**：模板与实例**共用同一批 FuncDef**
         * （实测 `template find` 与 `hashMap$hashMap.find` 是同一个指针），而模板轮的
         * `k.hash()` 解析不出来 ⇒ 只能保守标真，一旦标上，实例轮 `if (f->makesPool) continue`
         * 就永远跳过它，精确解析**没机会生效**。泛型结构体的方法体本来就只在实例上跑，
         * 所以交给下面那一轮按实例解析：解析不出来时那一轮仍旧保守（`resolveOnInstance`
         * 返回 NULL ⇒ 视为会建池），多个实例之间是**取或**（任何一个实例要建池 ⇒ 整条链
         * 保守标真），方向仍然是安全的。非泛型结构体没有这个问题，照旧。 */
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                if (f->makesPool) continue;
                if (stmtMakesPool(f->body, true)) { f->makesPool = true; changed = true; }
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
                if (f->makesPool) continue;
                instResTT = tt; instResParams = &sd->typeParams; instResTargs = &inst->targs;
                setPoolCalleeResolver(resolveOnInstance);
                bool mp = stmtMakesPool(f->body, true);
                setPoolCalleeResolver(NULL);
                instResTT = NULL;
                if (mp) { f->makesPool = true; changed = true; }
            }
        }
    }
    /* 顺手把"这个结构体的某个方法建池"记到结构体上（见 StructDef.makesPoolAny）。 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            if (f && f->makesPool) { sd->makesPoolAny = true; break; }
        }
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
                    f->name ? f->name : "-", (int)f->makesPool,
                    f->modName && *f->modName ? f->modName : "-");
        }
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
                fprintf(stderr, "[pool] %s::%-22s makesPool=%d mod=%s\n", sd->name,
                        f->name ? f->name : "-", (int)f->makesPool,
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
                int siteD = arenaDepthOf(sy->origin->arenaLevel);
                if (sy->refDepth < siteD) {
                    fprintf(stderr, "[selfcheck] binding `%s` has refDepth=%d, shallower than"
                            " its site (level %d => %d)\n", sy->name, sy->refDepth,
                            sy->origin->arenaLevel, siteD);
                    bad++;
                }
            }
        }
        Vec allF; vecInit(&allF, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++) *(FuncDef **)vecPush(&allF) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&allF) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        for (size_t i = 0; i < allF.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&allF, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                int want = arenaDepthOf(site->arenaLevel);
                if (site->refDepth != want) {
                    fprintf(stderr, "[selfcheck] site at line %d of %s: refDepth=%d but"
                            " arenaLevel=%d (should be %d)\n", site->line,
                            f->name ? f->name : "?", site->refDepth, site->arenaLevel, want);
                    bad++;
                }
                if (site->minAt == 0 && site->arenaLevel != ARENA_HOME) {
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
        Vec allF; vecInit(&allF, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&allF) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&allF) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        reportMemory(&c, &allF);
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
static bool funcAllocates(Checker *c, FuncDef *f) {
    if (!f) return true;                     /* unknown, so assume it allocates */
    if (f->isExtern) return false;           /* an extern function does not touch our arenas */
    if (f->allocState == 1) return true;
    if (f->allocState == 2) return false;
    if (f->allocState == 3) return true;     /* on the cycle being computed => conservative */
    f->allocState = 3;
    bool r = stmtHasNew(f->body) || stmtCallsAllocator(c, f->body);
    f->allocState = r ? 1 : 2;
    return r;
}

/* Does this statement or expression call a function that needs a home arena?
 *
 * The two walkers are mutually recursive and share the per-function cache, so this is only
 * meaningful after the callee has been checked: `e->func` is filled in by then.
 */
static bool exprCallsAllocator(Checker *c, Expr *e);
static bool stmtCallsAllocator(Checker *c, Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprCallsAllocator(c, s->u.var.init);
    case ST_ASSIGN: return exprCallsAllocator(c, s->u.assign.value) ||
                           exprCallsAllocator(c, s->u.assign.target);
    case ST_IF:     return exprCallsAllocator(c, s->u.ifs.cond) ||
                           stmtCallsAllocator(c, s->u.ifs.thenBody) ||
                           stmtCallsAllocator(c, s->u.ifs.elseBody);
    case ST_WHILE:  return exprCallsAllocator(c, s->u.whiles.cond) ||
                           stmtCallsAllocator(c, s->u.whiles.body);
    case ST_RETURN: return exprCallsAllocator(c, s->u.ret.value);
    case ST_EXPR:   return exprCallsAllocator(c, s->u.expr.expr);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtCallsAllocator(c, *(Stmt **)vecAt(&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH:
        if (exprCallsAllocator(c, s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtCallsAllocator(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    default: return false;
    }
}
/* Can this expression reach an allocator?
 *
 * Params:
 *   c - checker
 *   e - expression to walk; may be NULL
 *
 * Returns:
 *   True when evaluation of the expression can reach a `new` or another allocator.
 */
static bool exprCallsAllocator(Checker *c, Expr *e) {
    if (!e) return false;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && (!e->func || funcAllocates(c, e->func)))
        return true;
    switch (e->kind) {
    case EX_BIN: return exprCallsAllocator(c, e->u.bin.left) || exprCallsAllocator(c, e->u.bin.right);
    case EX_UN:  return exprCallsAllocator(c, e->u.un.operand);
    case EX_REF: return exprCallsAllocator(c, e->u.ref.operand);
    case EX_DEREF: return exprCallsAllocator(c, e->u.deref.operand);
    case EX_SIGN:  return exprCallsAllocator(c, e->u.sign.operand);
    case EX_CONV:  return exprCallsAllocator(c, e->u.conv.operand);
    case EX_TRY:   return exprCallsAllocator(c, e->u.try_.operand);
    case EX_INDEX:
        return exprCallsAllocator(c, e->u.index.obj) || exprCallsAllocator(c, e->u.index.index);
    case EX_SLICE: return exprCallsAllocator(c, e->u.slice.obj) ||
                          exprCallsAllocator(c, e->u.slice.lo) ||
                          exprCallsAllocator(c, e->u.slice.hi);
    /* `alloc<T>(n)` **is** an allocation, the same as `new` - the loop above only caught
     * the call shapes, so a body whose only allocation was `alloc` answered "no". */
    case EX_GENCALL:  return true;
    case EX_ARRAYLIT:
        for (size_t k = 0; k < e->u.arraylit.elems.len; k++)
            if (exprCallsAllocator(c, *(Expr **)vecAt(&e->u.arraylit.elems, k))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t k = 0; k < e->u.enumval.args.len; k++)
            if (exprCallsAllocator(c, *(Expr **)vecAt(&e->u.enumval.args, k))) return true;
        return false;
    case EX_STRUCTLIT:
        for (size_t k = 0; k < e->u.lit.inits.len; k++)
            if (exprCallsAllocator(c, (*(FieldInit **)vecAt(&e->u.lit.inits, k))->value))
                return true;
        return false;
    case EX_FIELD: return exprCallsAllocator(c, e->u.field.obj);
    case EX_COALESCE:
        return exprCallsAllocator(c, e->u.coalesce.main) ||
               exprCallsAllocator(c, e->u.coalesce.fallback);
    case EX_METHOD: {
        if (exprCallsAllocator(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprCallsAllocator(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    }
    case EX_CALL: {
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprCallsAllocator(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprCallsAllocator(c, *(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    }
    case EX_NEW: return exprCallsAllocator(c, e->u.new_.count);
    default: return false;
    }
}

