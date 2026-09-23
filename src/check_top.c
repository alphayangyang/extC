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

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line, params);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line, params);
    if (f->ret && ttIs(f->ret, "void")) f->ret = NULL;
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
                if (strcmp(ma->name, mb->name) == 0)
                    ckError(c, mb->line, NULL, "struct `%s` has duplicate method `%s`",
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
        if (p0->type->kind != TY_REF || !sb || sb->sdef != f->owner)
            ckError(c, p0->line, NULL, "`self` of `%s.%s` must be `ref %s`",
                    DN(f->owner), f->name, DN(f->owner));
    }
    for (size_t i = 1; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0)
            ckError(c, p->line, NULL, "`self` must be the first parameter");
    }

    checkOperatorSig(c, f);
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
    case EX_TRY:   return exprHasNew(e->u.try_.operand);
    case EX_INDEX: return exprHasNew(e->u.index.obj) || exprHasNew(e->u.index.index);
    case EX_SLICE: return exprHasNew(e->u.slice.obj);
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
static bool exprCallsNeedsHome(Expr *e);
static bool stmtCallsNeedsHome(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprCallsNeedsHome(s->u.var.init);
    case ST_ASSIGN: return exprCallsNeedsHome(s->u.assign.value) ||
                           exprCallsNeedsHome(s->u.assign.target);
    case ST_IF:     return exprCallsNeedsHome(s->u.ifs.cond) ||
                           stmtCallsNeedsHome(s->u.ifs.thenBody) ||
                           stmtCallsNeedsHome(s->u.ifs.elseBody);
    case ST_WHILE:  return exprCallsNeedsHome(s->u.whiles.cond) ||
                           stmtCallsNeedsHome(s->u.whiles.body);
    case ST_RETURN: return exprCallsNeedsHome(s->u.ret.value);
    case ST_EXPR:   return exprCallsNeedsHome(s->u.expr.expr);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtCallsNeedsHome(*(Stmt **)vecAt(&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH:
        if (exprCallsNeedsHome(s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtCallsNeedsHome((*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    default: return false;
    }
}
static bool exprCallsNeedsHome(Expr *e) {
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
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) &&
        e->func && e->func->needsHome)
        return true;
    switch (e->kind) {
    case EX_BIN: return exprCallsNeedsHome(e->u.bin.left) || exprCallsNeedsHome(e->u.bin.right);
    case EX_UN:  return exprCallsNeedsHome(e->u.un.operand);
    case EX_REF: return exprCallsNeedsHome(e->u.ref.operand);
    case EX_DEREF: return exprCallsNeedsHome(e->u.deref.operand);
    case EX_SIGN:  return exprCallsNeedsHome(e->u.sign.operand);
    case EX_TRY:   return exprCallsNeedsHome(e->u.try_.operand);
    case EX_INDEX:
        return exprCallsNeedsHome(e->u.index.obj) || exprCallsNeedsHome(e->u.index.index);
    case EX_SLICE: return exprCallsNeedsHome(e->u.slice.obj);
    case EX_FIELD: return exprCallsNeedsHome(e->u.field.obj);
    case EX_COALESCE:
        return exprCallsNeedsHome(e->u.coalesce.main) ||
               exprCallsNeedsHome(e->u.coalesce.fallback);
    case EX_METHOD: {
        if (exprCallsNeedsHome(e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    }
    case EX_CALL: {
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    }
    case EX_NEW: return exprCallsNeedsHome(e->u.new_.count);
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
            if (exprCallsNeedsHome((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        /* Payload construction: the call hides in the payload, as in
         * `holder.holding(mknode())`. */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    default: return false;
    }
}
static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body); }

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
     * does not know the caller's region, so it only publishes the constraint, and the
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
            int d = exprRefDepth(c, a);
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
    case EX_TRY:   return markNamesInExpr(c, e->u.try_.operand);
    case EX_SLICE: return markNamesInExpr(c, e->u.slice.obj);
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
        if (getenv("EXTC_DBG_FS"))
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
    case EX_TRY:   collectEffectsExpr(c, f, e->u.try_.operand); return;
    case EX_SLICE: collectEffectsExpr(c, f, e->u.slice.obj); return;
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

typedef struct {
    Vec tbl;          /* SymLevel */
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
static int levelOfValue(Checker *c, LvlState *ls, Expr *val, int target, int hops) {
    if (!val || hops > 32) return LEVEL_INF;
    if (getenv("EXTC_DBG_LV"))
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

        if (getenv("EXTC_DBG_LV"))
            fprintf(stderr, "      [lv] ident=%-5s sym=%s origin=%s target=%d\n",
                    val->u.ident.name, sy ? "yes" : "NULL",
                    (sy && sy->origin) ? "yes" : "NULL", target);
        if (getenv("EXTC_DBG_LV") && sy && sy->origin)
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
static void levelPass(Checker *c, const DfResult *dfr) {
    (void)dfr;
    LvlState ls;
    vecInit(&ls.tbl, c->arena, sizeof(SymLevel));

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
            bool ok = !r || r->kind == TY_BUILTIN ||
                      (r->kind == TY_REF && ttBase(r->inner) && ttBase(r->inner)->kind == TY_BUILTIN);
            if (!ok)
                ckError(c, f->line,
                        "A C function can return a scalar or a single pointer; anything else has"
                        " no C-level representation here.",
                        "`extern!` return type `%s` cannot cross the C boundary",
                        typeStr(c, f->ret));
        }
        collectEffects(c, f);            /* summary = the signed clause, or the worst case */
        return;
    }
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
        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, &dfr);
        if (getenv("EXTC_DBG_STORES")) {
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
        if (getenv("EXTC_DBG_DFA")) {
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

/* Re-check one deferred `==` for one concrete instance.
 *
 * Both batches of checks that are deferred to instantiation -- this `==` batch and the
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
 */
static void runEqCheck(Checker *c, EqCheck *ec, Vec *params, Vec *targs, const char *instName) {
    TypeTable *tt = c->tt;
    Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type, params, targs);
    Type *rt2 = ttSubstitute(tt, ec->node->u.bin.right->type, params, targs);
    if (!ttEquals(lt, rt2)) return;
    if (!typeSupportsEq(lt, ec->op))
        ckError(c, ec->node->line,
                "`==` inside a generic is checked at instantiation, not on the template --"
                " the price of having no traits. Add a `fn ==` to that type.",
                "`%s` needs `%s` to define `==`", instName, typeStr(c, lt));
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
        if (getenv("EXTC_DBG_AT"))
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
    vecInit(&c.eqChecks, arena, sizeof(void *));
    vecInit(&c.globals, arena, sizeof(void *));
    vecInit(&c.allSyms, arena, sizeof(void *));     /* kept for the EXTC_SELFCHECK invariant scan */
    vecInit(&c.nameUses, arena, sizeof(void *));
    vecInit(&c.narrow, arena, sizeof(void *));
    vecInit(&c.refChecks, arena, sizeof(void *));
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

    /* Deferred `==` checks: re-check each one for every concrete instance.
     * This is the price of having no traits. The error surfaces late, here, so the
     * message has to name the instance it came from. */
    for (size_t i = 0; i < c.eqChecks.len; i++) {
        EqCheck *ec = *(EqCheck **)vecAt(&c.eqChecks, i);
        /* The function holding this `==` is never called, so none of its instances can
         * run and the per-instance recheck is unnecessary. This also spares the user from
         * having to define `fn ==` for a type that is never compared. */
        if (ec->func && !ec->func->used) continue;
        /* (1) Type instances (a method's `T: ==`). A free function has no owner => skip. */
        if (!ec->owner) goto eq_done;
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != ec->owner) continue;
            runEqCheck(&c, ec, &ec->owner->typeParams, &inst->targs, inst->name);
        }
        /* (2) Free-function instances: a `T: ==` inside `fn f<T>`. */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (ec->func != fi->tmpl) continue;
            runEqCheck(&c, ec, &fi->tmpl->typeParams, &fi->targs, fi->instName);
        }
    eq_done: ;
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

            c.substParams = &owner->typeParams;
            c.substArgs   = &inst->targs;
            runRefCheck(&c, rc, inst->name);
        }
    }

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
                    if (getenv("EXTC_DBG_MINAT"))
                        fprintf(stderr, "[minAt] %-8s line=%-4d minAt=%-3d arena=%d\n",
                                f->name ? f->name : "?", site->line, site->minAt, site->arenaLevel);
                    if (getenv("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                site->arenaLevel, f->needsHome?1:0, (int)site->kind);
                    if (getenv("EXTC_DBG_S3"))
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
                    /* Call site with a pending `arenaArg`: the callee needs a home arena, so
                     * it gets ours.
                     *
                     * This case must not be narrowed the way the sites above are: `arenaArg`
                     * is the arena handed to the callee, and the callee may store into the
                     * caller's container, which outlives this frame, so "nothing escaped this
                     * frame" is no evidence at all here. */
                    if (f->needsHome) {
                        site->arenaArg = ARENA_HOME;
                        site->arenaArgPending = false;
                        fixed++;
                    }
                }
            }
        }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] decided: %d sites in the home arena, %d kept at block level\n", fixed, keptBlock);

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
    case EX_TRY:   return exprCallsAllocator(c, e->u.try_.operand);
    case EX_INDEX:
        return exprCallsAllocator(c, e->u.index.obj) || exprCallsAllocator(c, e->u.index.index);
    case EX_SLICE: return exprCallsAllocator(c, e->u.slice.obj);
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

