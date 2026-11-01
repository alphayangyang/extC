/* Type checking: infer the type of every expression and write it back into the AST.
 *
 * Three conversion rules drive every assignment, argument, and return check:
 *   - Widening (lossless) is implicit.
 *   - Narrowing (lossy) is always rejected; a lossy conversion has to be written out.
 *   - A literal takes the type its context expects, by value (`let x: f32 = 3.14`,
 *     `let x: u8 = 200`).
 *
 * After this pass codegen only translates; it never reasons about types again.
 */

#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort: sorting the table of module-level names */
#include <string.h>


#include "check_internal.h"
#include "dbg.h"            /* EXTC_DBG_ASSERT: contract violations are compiler bugs */
#include "plan.h"          /* Plan: read only through accessors */
/* ---------------------------------------------------------------- errors */

/**
 * @brief Report a fatal compile error on one source line.
 *
 * The message is rendered into a fixed-size buffer and truncated when too long rather than
 * overflowing it. Reporting follows "first error wins": once the error flag is set, later errors
 * are dropped so that a flood of cascading diagnostics does not reach the user.
 *
 * @param c     The checker collecting diagnostics.
 * @param line  The source line the diagnostic points at; 0 when no position is known.
 * @param note  One extra line of explanation printed under the message, or NULL.
 * @param fmt   The message's printf format.
 * @param ...   The format arguments for `fmt`.
 */
void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

/**
 * @brief Report a warning on one source line.
 *
 * The only difference from reporting an error is that a warning does not set the error flag:
 * compilation continues and the exit status is unchanged.
 *
 * @param c     The checker emitting the warning.
 * @param line  The source line the warning points at.
 * @param note  One extra line of explanation printed under the message, or NULL.
 * @param fmt   The message's printf format.
 * @param ...   The format arguments for `fmt`.
 */
void ckWarn(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxWarn(c->ctx, line, 1, note, "%s", tmp);
}

/**
 * @brief Render a type into the source text the user writes, for diagnostics.
 *
 * A lambda's type is a generated closure struct while the user wrote a signature, so diagnostics
 * show the signature.
 *
 * @param c  The checker; the text is allocated in its arena.
 * @param t  The type to render; NULL renders as `void`.
 * @return   A NUL-terminated string valid for the rest of this run.
 */
const char *typeStr(Checker *c, Type *t) {
    /* A lambda's type is a generated closure struct; the user wrote a signature, so diagnostics
     * show the signature. */
    if (t && t->sdef && anLamSig(t->sdef)) return anLamSig(t->sdef);
    Buf b;
    bufInit(&b, c->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/* ---------------------------------------------------------------- generated C names (renaming) */

/**
 * @brief Compare two `const char *` names, for sorting the module-name table.
 *
 * @param a  Pointer to the first name, interpreted as a `const char **`.
 * @param b  Pointer to the second name, interpreted as a `const char **`.
 * @return   The `strcmp` result: negative, 0, or positive when the first is smaller, equal, or
 *           greater respectively.
 */
static int cmpNamePtr(const void *a, const void *b) {
    const char *x = *(const char **)a, *y = *(const char **)b;
    /* A name is NULL only for a declaration that was never given a source name (an anonymous
     * constructed type); sort those first so `strcmp` never sees one. */
    if (!x || !y) return (x != NULL) - (y != NULL);
    return strcmp(x, y);
}
/**
 * @brief Tell whether a module-level name is already taken.
 *
 * This is a compile-time optimisation: looking the name up in each module-level vector one by one
 * would make naming quadratic in their total length, and one sorted table plus a binary search
 * avoids that.
 *
 * The table is rebuilt only when the sum of the four vectors' lengths changes: module-level names
 * only grow and never change during checking, and generic instances are recorded in the type table
 * and on the code generation side instead of entering `m->*`.
 *
 * @param c     The checker holding the cached table.
 * @param name  The candidate name to look up.
 * @return      true when a function, struct, type, or global already has this name.
 */
static bool moduleNameTaken(Checker *c, const char *name) {
    Module *m = c->m;
    /* Keyed on the four lengths, not on their sum: a sum cannot see one vector shrink while
     * another grows, and a stale table would let a local take a module-level name. */
    if (c->moduleNamesFuncs != m->funcs.len || c->moduleNamesStructs != m->structs.len ||
        c->moduleNamesTypes != m->types.len || c->moduleNamesGlobals != m->globals.len) {
        if (!c->moduleNames.arena) vecInit(&c->moduleNames, c->arena, sizeof(const char *));
        c->moduleNames.len = 0;
        for (size_t i = 0; i < m->funcs.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(FuncDef **)vecAt(&m->funcs, i))->name;
        for (size_t i = 0; i < m->structs.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(StructDef **)vecAt(&m->structs, i))->name;
        for (size_t i = 0; i < m->types.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(TypeDef **)vecAt(&m->types, i))->name;
        for (size_t i = 0; i < m->globals.len; i++)
            *(const char **)vecPush(&c->moduleNames) = (*(GlobalDef **)vecAt(&m->globals, i))->name;
        qsort(c->moduleNames.data, c->moduleNames.len, sizeof(const char *), cmpNamePtr);
        c->moduleNamesFuncs = m->funcs.len;
        c->moduleNamesStructs = m->structs.len;
        c->moduleNamesTypes = m->types.len;
        c->moduleNamesGlobals = m->globals.len;
    }
    size_t lo = 0, hi = c->moduleNames.len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, *(const char **)vecAt(&c->moduleNames, mid));
        if (r == 0) return true;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return false;
}

/**
 * @brief Give a source name its name in the generated C.
 *
 * `let` only names a value and allocates no storage, so writing `let a = a + 1` in the same scope
 * after `let a = 1` is legal: the second `a` gives the name a new meaning while the first binding
 * and its storage stay where they are. C does not allow two declarations of the same name in one
 * block, so from the second use on the generated C calls it `a__2`. The name in the extC source
 * never changes -- renaming is a compiler-internal matter and must not leak into the user's view.
 *
 * Two constraints follow:
 *   - within one function the suffix increases monotonically and is never reused, so the generated
 *     C reads deterministically without relying on C's own block scoping rules.
 *   - module-level names (functions, structs, globals) must be avoided too, otherwise a local would
 *     shadow a function name in the generated C and `foo()` could not be called any more.
 *
 * @param c     The checker, holding the per-function name-use counts.
 * @param name  The name as written in the source.
 * @return      The name to emit; the original name on first use. The string lives in the checker's
 *              arena.
 */
const char *cNameFor(Checker *c, const char *name) {
    NameUse *u = NULL;
    for (size_t i = 0; i < c->nameUses.len; i++) {
        NameUse *it = *(NameUse **)vecAt(&c->nameUses, i);
        if (strcmp(it->name, name) == 0) { u = it; break; }
    }
    int n = u ? u->count : 0;

    const char *cand;
    do {
        n++;
        cand = (n == 1) ? name : arenaPrintf(c->arena, "%s__%d", name, n);
        /* C keywords must be avoided too: `let double = 3` is legal extC, but the generated C
         * cannot declare a variable named `double`. */
    } while (moduleNameTaken(c, cand) || cIdentIsKeyword(cand));

    if (u) u->count = n;
    else {
        NameUse *fresh = (NameUse *)arenaAllocZero(c->arena, sizeof(NameUse));
        fresh->name = name;
        fresh->count = n;
        *(NameUse **)vecPush(&c->nameUses) = fresh;
    }
    return cand;
}

/* ---------------------------------------------------------------- scopes */

/**
 * @brief Enter a new lexical scope.
 *
 * The current narrowing table length is recorded as a watermark, and the table is restored to it
 * when the block ends: nullability facts proven inside the block disappear together with the
 * block, consistent with the lexical scoping of non-null narrowing.
 *
 * @param c  The checker.
 */
void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
    *(size_t *)vecPush(&c->narrowMarks) = c->narrow.len;
}

/**
 * @brief Leave the innermost lexical scope, dropping its bindings and revoking the non-null facts
 * it established.
 *
 * The narrowing table is truncated back to the watermark recorded when the scope was entered.
 *
 * @param c  The checker.
 */
void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
    if (c->narrowMarks.len) {
        c->narrow.len = *(size_t *)vecAt(&c->narrowMarks, c->narrowMarks.len - 1);
        c->narrowMarks.len--;
    }
}

/**
 * @brief Substitute the current instance's type arguments into a type.
 *
 * @param c  The checker; the substitution context is the checker's `substParams` / `substArgs`.
 * @param t  The type to substitute into.
 * @return   The substituted type; `t` unchanged when no instantiation is in progress or `t` is
 *           NULL.
 */
Type *tsub(Checker *c, Type *t) {
    if (!c->substParams || !c->substArgs || !t) return t;
    return ttSubstitute(c->tt, t, c->substParams, c->substArgs);
}



/* ---------------------------------------------------------------- helpers */











/**
 * @brief Tell whether an expression is an integer or a float literal.
 *
 * @param e  The expression; must be non-NULL.
 * @return   true for EX_INT or EX_FLOAT, whose types are determined by the context.
 */
bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

/**
 * @brief Tell whether a value of this type may be handed to `print` or `println`.
 *
 * Structs, generic instances, and arrays are printed recursively by the compiler-generated
 * `<Type>_debug` helper; an enum without payload always has a name to print.
 *
 * @param t  The argument type.
 * @return   true when some way to print it exists; false when `t` is NULL or the type is
 *           unsupported.
 */
bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    if (b->kind == TY_ENUM) return true;
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC || b->kind == TY_ARRAY;
}

/**
 * @brief Tell whether a literal's value fits into the target type.
 *
 * A literal has no type of its own until the context gives it one, so `3.14` becomes `f32` in an
 * `f32` context and `200` becomes `u8` in a `u8` context. An unsigned target rejects a negative
 * integer literal; the error type accepts any literal so that one diagnostic does not cascade into
 * more.
 *
 * The 64-bit case is not decided by shifting: every value a 64-bit literal can hold fits a signed
 * 64-bit target, and an unsigned target accepts the non-negative ones among them.
 *
 * @param e     The literal: EX_INT, EX_FLOAT, or EX_BOOL.
 * @param want  The type the context expects.
 * @return      true when the literal's value is representable in `want`.
 */
bool literalFits(Expr *e, Type *want) {
    Type *w = ttBase(want);
    if (!w) return false;
    if (ttIsError(w)) return true;

    if (e->kind == EX_INT) {
        int bits = ttIntBits(w);
        if (bits > 0) {
            long long v = e->u.ival;
            if (bits == 64) return ttIntSigned(w) || v >= 0;
            if (ttIntSigned(w)) {
                long long lo = -(1LL << (bits - 1));
                long long hi = (1LL << (bits - 1)) - 1;
                return v >= lo && v <= hi;
            }
            if (v < 0) return false;
            return (unsigned long long)v <= ((1ULL << bits) - 1);
        }
        return ttIsFloat(w);
    }

    if (e->kind == EX_FLOAT) return ttIsFloat(w);
    if (e->kind == EX_BOOL)  return ttIs(w, "bool");
    return false;
}

/**
 * @brief Tell whether the struct name written in a literal is the struct behind the context type.
 *
 * The module system rewrites a duplicated declaration as `lib$pair`, so the comparison looks at
 * both the source name and the last segment: what the source writes is always the source name, and
 * `srcName` is exactly that. When the names do not match the context is not trusted -- it could
 * really be another type, and letting name resolution report the error its own way (unknown name,
 * ambiguous name) is more honest.
 *
 * @param sd       The struct behind the context type (non-NULL).
 * @param written  The name written in the struct literal (non-NULL).
 * @return         true when the written name points at the same struct.
 */
bool structLitNameMatches(const StructDef *sd, const char *written) {
    if (!sd || !written) return false;
    if (sd->srcName && strcmp(sd->srcName, written) == 0) return true;
    if (strcmp(sd->name, written) == 0) return true;
    const char *last = sd->name;
    for (const char *q = sd->name; *q; q++)
        if (*q == '$' || *q == ':') last = q + 1;
    return strcmp(last, written) == 0;
}

/**
 * @brief Give an expression that cannot state its own type the type the context expects.
 *
 * Only bare `null`, array literals, and struct literals without a qualifier are handled, and only
 * when the context determines a unique type; `null` accepts only a nullable-reference context,
 * because it has no way to know the target type.
 *
 * @param e     The expression whose type is to be filled in.
 * @param want  The type the context expects.
 */
void adoptContextType(Expr *e, Type *want) {
    if (!e || !want) return;
    if (e->kind == EX_NULL) {
        if (want->kind == TY_REF && want->nullable) e->type = want;
        return;
    }
    Type *w = ttBase(want);
    if (!w) return;
    /* Stash the context type in `e->type`; the later type check overwrites it with that same
     * type. A generic instance has no name to look up and an array literal does not know its own
     * length, so this is their only way to get a type. */
    if (e->kind == EX_ARRAYLIT) { e->type = w; return; }
    if (w->kind != TY_STRUCT && w->kind != TY_GENERIC) return;
    /* A struct literal's type comes from the context (`var c: box<i64> = box { v: 2 }`): the name
     * must point at the same struct, and the context supplies the full generic instance. A bare
     * `{ ... }` is always accepted. */
    if (e->kind == EX_STRUCTLIT &&
        (!e->u.lit.name || structLitNameMatches(w->sdef, e->u.lit.name)))
        e->type = w;
}

/**
 * @brief Accept a lambda that captures nothing where a `fn(...)` value is expected.
 *
 * A lambda whose capture set is empty has an empty environment: the value is a code pointer and
 * nothing else, which is exactly what a `fn(...)` type is. The conversion is recorded on the node
 * (`planSetLamFnPtr`) rather than rewriting its type, so a literal that is *also* kept as a closure
 * value elsewhere keeps the environment; code generation emits the pointer form only here.
 *
 * The signature has to match exactly: same parameter count and types, same result. A lambda that
 * captures state is left alone -- its environment has to travel with the code, and the diagnostic
 * for that case names the reason.
 *
 * @param c     The checker.
 * @param want  The expected type.
 * @param node  The expected expression (an initializer, an argument, a return value).
 * @return      `true` when the conversion was accepted and recorded.
 */
static bool lambdaBecomesFnPtr(Checker *c, Type *want, Expr *node) {
    if (!want || want->kind != TY_FN || !node || node->kind != EX_LAMBDA) return false;
    StructDef *sd = node->u.lambda.sdef;
    if (!sd || sd->fields.len != 0 || sd->methods.len != 1) return false;
    FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
    if (md->params.len == 0) return false;                    /* the receiver is the first one */
    if (md->params.len - 1 != want->params.len) return false;
    if (!md->ret || !want->ret || !ttEquals(tsub(c, md->ret), want->ret)) return false;
    for (size_t i = 0; i < want->params.len; i++) {
        Param *p = *(Param **)vecAt(&md->params, i + 1);
        Type *pt = p ? tsub(c, p->type) : NULL;
        Type *wt = *(Type **)vecAt(&want->params, i);
        if (!pt || !wt || !ttEquals(pt, wt)) return false;
    }
    /* The thunk calls the synthesised `call` method, so that method has to be emitted: the
     * reachability pruning would otherwise drop it (nothing on the extC side names it). */
    planSetUsed(md, true);
    planSetLamFnPtr(node, true);
    return true;
}

/**
 * @brief Check whether a value of type `got` may be assigned where `want` is expected.
 *
 * Only lossless conversions are implicit: widening, non-null reference to nullable reference,
 * dropping `mut`, and so on; every other conversion has to be written out by the user. This is the
 * single gate for assignment, arguments, field initialisation, and payload values. An assignment
 * whose types contain type parameters is recorded and re-checked against the real types at
 * instantiation.
 *
 * `mut` is implicit only in the lossless direction: a writable reference may be used as read-only,
 * the reverse has to be written as `mut`. `?ref T` to `ref T` is rejected: it claims the reference
 * is non-null, and that must first be proven.
 *
 * @param c     The checker, used for rendering types and reporting errors.
 * @param want  The expected type.
 * @param got   The type of the assigned expression.
 * @param node  That expression, for the line number and literal adaptation.
 * @param what  A description of the target (such as "argument", "field `x`") used in the
 *              diagnostic.
 * @return      true when the assignment is legal; on an illegal one the error has been reported
 *              and false is returned.
 */
bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
    /* Contract: both types are real. `ttIsError` below treats NULL as "not an error", so a missing
     * type would be dereferenced instead of reported -- a bug in the caller, not in a program. */
    EXTC_DBG_ASSERT(want != NULL && got != NULL);
    /* Two kinds of value are deferred: method calls whose resolution was deferred to
     * instantiation (there `got` is the error type), and **any** expression whose type mentions a
     * type parameter -- e.g. `s = s + c.value()` inside `fn run<T>`, where the sum's type is `T`
     * while the assignment expects `i64`. Letting it through on a template is right only if the
     * pair of expected and actual types is recorded and re-checked by every instance; otherwise
     * `fn f<T>(x: T) -> i8 { return x.hash() }` would silently truncate when the instance's
     * `hash` returns `i64`. */
    if ((ttIsError(got) && node && node->kind == EX_METHOD && !planCallee(node) &&
         mentionsParam(node->u.method.recv->type)) ||
        (node && got && (got->kind == TY_PARAM || mentionsParam(got)))) {
        DeferredUse *du = (DeferredUse *)arenaAllocZero(c->arena, sizeof(DeferredUse));
        du->call  = node;
        du->want  = want;
        du->func  = c->curFunc;
        du->owner = c->curFunc ? c->curFunc->owner : NULL;
        *(DeferredUse **)vecPush(&c->deferredUses) = du;
    }
    if (ttIsError(want) || ttIsError(got)) return true;
    /* A type parameter is equally unknown at this point; at instantiation the recorded pair pins
     * down the real type, and a mismatch is reported there, naming the instance. */
    if (got && (got->kind == TY_PARAM || mentionsParam(got))) return true;

    /* ---- Boxing: putting a coroutine's frame value where a handle type is expected ----
     * The surface type is only `coroutine<T>`; the representation is the compiler's business.
     * Here the value is a **frame** (the type the checker synthesises for that coroutine) and the
     * expected type is the handle, so the frame moves into the task's own slot and code generation
     * emits the handle. The resulting types must agree. */
    if (want && got && isProtoType(want, "coroutine", 1) && got->sdef && planCoroOf(got->sdef) &&
        node && node->kind == EX_CALL) {
        Type *yt = want->targs.len ? *(Type **)vecAt(&want->targs, 0) : NULL;
        Type *ft = planYieldType(planCoroOf(got->sdef));
        if (yt && ft && strcmp(typeStr(c, yt), typeStr(c, ft)) == 0) {
            planSetBoxedCoro(node, true);
            node->type = want;
            planSetCoroBoxed(planCoroOf(got->sdef), true);
            return true;
        }
    }

    /* A lambda that captures nothing has no environment, so its value is a code pointer -- what a
     * `fn(...)` type is. Accepting the conversion here keeps the syntax plain (`take(fn(x) ...)`)
     * while leaving the closure path (environment struct, `.call`) untouched. */
    if (lambdaBecomesFnPtr(c, want, node)) return true;

    /* The same shape with a capture set: the state has to travel with the code, so the value is a
     * closure and not a single pointer. Saying that beats "expects `fn(i64) -> i64`, found
     * `fn(i64) -> i64`", which names the same type twice. */
    if (want && want->kind == TY_FN && node && node->kind == EX_LAMBDA && node->u.lambda.sdef &&
        node->u.lambda.sdef->fields.len > 0) {
        ckError(c, node->line,
                "the state a lambda captures travels with its code, so its value is a closure and"
                " not one pointer; pass that state as a parameter, or give the closure a name to"
                " keep it as a value",
                "%s expects `%s`, but this lambda captures %zu variable(s)",
                what, typeStr(c, want), node->u.lambda.sdef->fields.len);
        return false;
    }

    /* Both sides of `ref T` must be references.
     *
     * The trap is subtle: the literal-adaptation path calls `ttBase(want)`, which strips `ref`, so
     * `p = 5` on `p: ref i64` looks like "5 fits in i64" and is let through. The generated C would
     * be `int64_t * p; p = 5;`, which only gcc complains about (`makes pointer from integer
     * without a cast`). The type check has to stop it here. */
    if (want->kind == TY_REF && got->kind != TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` can only be assigned another reference; "
                "to write through it, assign to what it points to (`p.field = ...`, `p[i] = ...`)",
                "%s expects `%s`, found `%s` -- a value is not a reference",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }
    if (want->kind != TY_REF && got->kind == TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` is not a value; extC has no implicit dereference",
                "%s expects `%s`, found `%s` -- dereference it first",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (want->kind == TY_REF && got->kind == TY_REF && got->nullable && !want->nullable &&
        ttEquals(want->inner, got->inner)) {
        ckError(c, node ? node->line : 0,
                "check it first: `if p != null { ... }` -- inside that branch the compiler"
                " knows it is not null and generates no runtime check; there is no other"
                " way to turn a `?ref T` into a `ref T`",
                "%s expects `%s`, found `%s` -- a nullable reference", 
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (ttEquals(want, got)) return true;

    /* Non-null to nullable is the safe direction, hence implicit, the same as dropping `mut`. */
    if (want->kind == TY_REF && got->kind == TY_REF && want->nullable && !got->nullable &&
        want->mut == got->mut && ttEquals(want->inner, got->inner)) return true;

    /* **Pointer erasure**: `ref T` may be handed to `ref void` (`mut ref T` to `mut ref void`), the
     * equivalent of C's `T *` -> `void *`. Boundary APIs rely on it to say "what it points to is
     * not my business" with `void *`.
     *
     * It is written as erasure rather than a cast, held in check by the three conditions below:
     *   - `want`'s pointee must be `void`, so the reverse (`ref void` -> `ref T`) cannot reach
     *     here; only a declaration that knows the real type (and the `fn(...)` symbol case) is the
     *     way back;
     *   - `!got->nullable || want->nullable` stops `?ref T` -> `ref void`: that would claim it is
     *     non-null;
     *   - `!want->mut || got->mut` stops `ref T` -> `mut ref void`: that would demand write
     *     permission.
     *
     * `ref void` can do strictly less: no `*`, no `[i]`, no field access -- the checker rejects all
     * three. */
    if (want->kind == TY_REF && got->kind == TY_REF &&
        want->inner && ttBase(want->inner) && ttBase(want->inner)->kind == TY_VOID &&
        (!got->nullable || want->nullable) && (!want->mut || got->mut))
        return true;

    /* Dropping `mut` is implicit: what is writable can of course be read. It is one-way and always
     * safe; the other direction demands write permission and must be written `mut`. The same
     * reasoning covers references (`mut ref T` -> `ref T`) and views (`mut slice<T>` ->
     * `slice<T>`). */
    if (want->kind == got->kind) {
        if (want->kind == TY_REF && got->mut && !want->mut &&
            ttEquals(want->inner, got->inner)) return true;
        /* View: `mut` can be dropped recursively (`mut slice<mut slice<T>>` may be used as
         * `slice<slice<T>>`, the reverse may not); the test is `ttViewDowngradable`. */
        if (want->kind == TY_GENERIC && ttViewDowngradable(want, got) &&
            !ttEquals(want, got)) return true;
    }
    if (ttCanWiden(got, want)) return true;
    if (node && literalFits(node, want)) return true;

    /* An integer literal that does not fit and a type mismatch are two kinds of error, so this one
     * gets its own message. (A float literal assigned to an integer variable is a lossy conversion
     * and falls through to the generic message below.) */
    if (node && node->kind == EX_INT && ttIsInteger(want)) {
        ckError(c, node->line, NULL,
                "%s: literal `%lld` does not fit in `%s`",
                what, node->u.ival, typeStr(c, want));
        return false;
    }

    /* Container to view is a **borrow**, not a conversion: `varArray<T>` has its own `asSlice()`
     * and a fixed-length array is sliced with `[..]`. It is not done implicitly because the
     * borrowed view carries a lifetime (the value returned by `asSlice()` can be borrow-checked),
     * and conjuring a view for the user would bypass that check. At least point out what to
     * write. */
    if (want && got && !ttIsError(want) && !ttIsError(got) && ttIsViewType(ttBase(want))) {
        if (!want->mut && isProtoType(got, "varArray", 1)) {
            ckError(c, node ? node->line : 0,
                    "lend a read-only view explicitly: `v.asSlice()`",
                    "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
            return false;
        }
        if (!want->mut && got->kind == TY_ARRAY) {
            ckError(c, node ? node->line : 0,
                    "slice the array explicitly: `a[..]` is a view over the whole array",
                    "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
            return false;
        }
    }

    ckError(c, node ? node->line : 0,
            "extC only widens implicitly (lossless); a lossy conversion must be written out",
            "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
    return false;
}

/**
 * @brief Require a condition expression to have type `bool`, reporting an error otherwise.
 *
 * extC has no implicit truthiness: an integer or a reference used as a condition is reported
 * rather than converted.
 *
 * @param c     The checker.
 * @param t     The condition's type.
 * @param node  The condition expression, for the source position.
 */
void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "conditions must be `bool` -- extC has no implicit truthiness",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

