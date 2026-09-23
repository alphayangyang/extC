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
/* ---------------------------------------------------------------- errors */

/* Report a fatal error at a source position.
 *
 * Params:
 *   c    - checker whose Ctx collects the diagnostic
 *   line - source line the diagnostic points at; 0 when no position is known
 *   note - an extra explanatory sentence printed under the message, or NULL
 *   fmt  - printf format for the message, followed by its arguments
 *
 * Notes:
 *   - The message is rendered into a fixed-size buffer first, so a long message is
 *     truncated rather than overflowing.
 *   - Reporting is first-error-wins: once `Ctx.hasError` is set, later errors are
 *     dropped, so a cascade of follow-on diagnostics never reaches the user.
 */
void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

/* Report a warning: something worth telling the user about, but not worth stopping
 * the build for.
 *
 * Params:
 *   c    - checker whose Ctx carries the warning channel
 *   line - source line the warning points at
 *   note - an extra explanatory sentence printed under the message, or NULL
 *   fmt  - printf format for the message, followed by its arguments
 *
 * Notes:
 *   - The only difference from `ckError` is that a warning does not set `hasError`, so
 *     compilation continues and the exit status is unchanged.
 */
void ckWarn(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxWarn(c->ctx, line, 1, note, "%s", tmp);
}

/* Render a type the way the user writes it, for use inside diagnostics.
 *
 * Params:
 *   c - checker owning the arena the text is allocated in
 *   t - the type to render; NULL renders as "void"
 *
 * Returns:
 *   A NUL-terminated string owned by the checker's arena, valid for the rest of the run.
 */
const char *typeStr(Checker *c, Type *t) {
    Buf b;
    bufInit(&b, c->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/* ---------------------------------------------------------------- generated C names (renaming) */

/* Compare two `const char *` names, for qsort over the module-name table. */
static int cmpNamePtr(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
/* Report whether a module-level name is already taken.
 *
 * Params:
 *   c    - checker holding the cached table
 *   name - candidate name to look up
 *
 * Returns:
 *   True when a function, struct, type, or global of the module already uses this name.
 *
 * Notes:
 *   - This is a compile-time optimisation. Asking the module-level vectors in turn made
 *     naming quadratic in their total length: a front end with N functions and N structs
 *     measured 35 ms at N = 500 and 1029 ms at N = 4000. One sorted table searched by
 *     binary search replaces it.
 *   - Module-level names are stable while checking (generic instances live in
 *     tt->enumInstances and in codegen's g.insts, never in m->*), so the table is rebuilt
 *     only when the combined length of those vectors changes.
 */
static bool moduleNameTaken(Checker *c, const char *name) {
    Module *m = c->m;
    size_t want = m->funcs.len + m->structs.len + m->types.len + m->globals.len;
    if (!c->moduleNames.arena) vecInit(&c->moduleNames, c->arena, sizeof(const char *));
    if (c->moduleNamesSize != want) {
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
        c->moduleNamesSize = want;
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

/* Give a source-level name the name it will carry in the generated C.
 *
 * `let` names a value; it does not store one. So `let a = 1` followed by
 * `let a = a + 1` in the same scope is legal: the second `a` gives the name a new
 * meaning and leaves the first binding, with its storage, where it was. C forbids two
 * declarations of one name in the same block, so from the second use on the generated C
 * calls it `a__2`. The extC source name never changes -- renaming is internal to the
 * compiler and must not leak into the user's view.
 *
 * Two constraints follow:
 *   - Within one function the suffixes are monotonic and never reused, so the generated C
 *     reads deterministically and does not depend on C's own block-scope rules.
 *   - Module-level names (functions, structs, globals) have to be avoided as well,
 *     otherwise a local would shadow a function name in the generated C and `foo()`
 *     would no longer be callable.
 *
 * Params:
 *   c    - checker holding the per-function name-use counters
 *   name - the name as written in the source
 *
 * Returns:
 *   The name to emit; the first use of a name returns it unchanged. The string lives in
 *   the checker's arena.
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
        /* C keywords have to be avoided too: `let double = 3` is legal extC, but the
         * generated C cannot declare a variable named `double`. */
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

/* Enter a lexical scope.
 *
 * Also records the current length of the narrowing list, because the facts proved about
 * nullable references inside the block are dropped when the block ends: narrowing is
 * lexically scoped like every other binding.
 */
void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
    /* Narrowing is lexically scoped too: remember the watermark on entry and restore
     * it when the block ends. */
    *(size_t *)vecPush(&c->narrowMarks) = c->narrow.len;
}

/* Leave the innermost lexical scope and drop the narrowing facts it introduced. */
void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
    if (c->narrowMarks.len) {
        c->narrow.len = *(size_t *)vecAt(&c->narrowMarks, c->narrowMarks.len - 1);
        c->narrowMarks.len--;
    }
}

/* Replace the type parameters in a type with the type arguments of the instance being
 * checked.
 *
 * Params:
 *   c - checker; the current substitution is c->substParams / c->substArgs
 *   t - the type to substitute into
 *
 * Returns:
 *   The substituted type, or `t` itself when no instantiation is in progress.
 */
Type *tsub(Checker *c, Type *t) {
    if (!c->substParams || !c->substArgs || !t) return t;
    return ttSubstitute(c->tt, t, c->substParams, c->substArgs);
}

/* Report whether a type mentions a type parameter, directly or inside a field.
 *
 * Params:
 *   t - the type to inspect
 *
 * Returns:
 *   True when the type cannot be judged yet, so the check has to be deferred until the
 *   instance is known.
 */
bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/* ---------------------------------------------------------------- helpers */

/* Report whether `op` is one of the six comparison operators. */
bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

/* Report whether `op` is `&&` or `||`. */
bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

/* Report whether an expression is an integer or floating-point literal.
 *
 * Params:
 *   e - the expression to test; must not be NULL
 *
 * Returns:
 *   True for EX_INT and EX_FLOAT, the two shapes whose type is chosen by the context
 *   rather than by the literal itself.
 */
bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

/* Report whether a value of this type can be handed to `print` / `println`.
 *
 * Params:
 *   t - type of the argument
 *
 * Returns:
 *   True when a printer exists for the type.
 */
bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    /* An enum without a payload always has a name to print. */
    if (b->kind == TY_ENUM) return true;
    /* Structs, generic instances, and arrays are printed by the `<Type>_debug` function
     * the compiler generates; it recurses into the members. */
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC || b->kind == TY_ARRAY;
}

/* Report whether a literal fits the target type by value.
 *
 * A literal has no type of its own until the context gives it one: `3.14` becomes an
 * `f32` in `let x: f32 = 3.14`, and `200` becomes a `u8` in `let x: u8 = 200`.
 *
 * Params:
 *   e    - the literal: EX_INT, EX_FLOAT, or EX_BOOL
 *   want - the type the surrounding context expects
 *
 * Returns:
 *   True when the literal's value is representable in `want`. An error type accepts
 *   anything, so that one diagnostic does not cascade into more.
 *
 * Notes:
 *   - An unsigned target rejects a negative integer literal instead of keeping its bit
 *     pattern.
 *   - The 64-bit case is decided without shifts: every value a 64-bit literal can hold
 *     fits a signed 64-bit target, and an unsigned one takes the non-negative values.
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
        return ttIsFloat(w);                    /* an integer literal for a float target */
    }

    if (e->kind == EX_FLOAT) return ttIsFloat(w);
    if (e->kind == EX_BOOL)  return ttIs(w, "bool");
    return false;
}

/* Give an expression that cannot spell its own type the type its context expects.
 *
 * Three shapes have no type of their own and are handled here: a bare `null`, an array
 * literal, and a bare `{}` struct literal. It only applies where the context pins the
 * type down uniquely.
 *
 * Params:
 *   e    - the expression to give a type to
 *   want - the type the context expects
 *
 * Notes:
 *   - `null` is the zero value of a nullable reference, so only a `?ref T` context is
 *     adopted: the literal cannot know what `T` is.
 */
void adoptContextType(Expr *e, Type *want) {
    if (!e || !want) return;
    if (e->kind == EX_NULL) {
        if (want->kind == TY_REF && want->nullable) e->type = want;
        return;
    }
    Type *w = ttBase(want);
    if (!w) return;
    /* Park the context type on `e->type`; `checkExpr` later overwrites it with the very
     * same type. A generic instance has no name to look up and an array literal does not
     * know its length, so this is the only way they can get a type at all. */
    if (e->kind == EX_ARRAYLIT) { e->type = w; return; }
    if (w->kind != TY_STRUCT && w->kind != TY_GENERIC) return;
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) e->type = w;
}

/* Check that a value of type `got` may be assigned where `want` is expected.
 *
 * Only lossless conversions are implicit; everything else has to be written out by the
 * user. This is the single gate for assignments, arguments, field initializers, and
 * payload values.
 *
 * Params:
 *   c    - checker, for `typeStr` and error reporting
 *   want - the expected type
 *   got  - the type of the expression being assigned
 *   node - the expression, used for its source position and for literal fitting
 *   what - description of the target ("argument", "field `x`"), used in the message
 *
 * Returns:
 *   True when the assignment is legal. A false result means a diagnostic was already
 *   reported here.
 *
 * Notes:
 *   - Only the lossless direction of `mut` is implicit: a writable reference may be used
 *     read-only, the reverse has to be written as `mut`.
 *   - `?ref T` to `ref T` is rejected: it claims the reference is not null, and that has
 *     to be proved first.
 */
bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
    if (ttIsError(want) || ttIsError(got)) return true;

    /* `ref T` has to be a reference on both sides.
     *
     * The trap is subtle: the literal-fitting path calls `ttBase(want)`, which strips the
     * `ref`, so `p = 5` for `p: ref i64` looks like "5 fits in i64" and is let through.
     * The generated C would be `int64_t * p; p = 5;`, and only gcc would complain
     * (`makes pointer from integer without a cast`). The type checker has to stop it. */
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

    /* `?ref T` to `ref T` claims the reference is not null, and that claim has to be
     * proved. The other direction (non-null to nullable) is always safe and implicit. */
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

    /* Non-null to nullable moves in the safe direction, so it is implicit, like
     * dropping `mut`. */
    if (want->kind == TY_REF && got->kind == TY_REF && want->nullable && !got->nullable &&
        want->mut == got->mut && ttEquals(want->inner, got->inner)) return true;

    /* Dropping `mut` is implicit: what may be written may of course be read. It is
     * one-way and always safe. The reverse direction asks for write permission and has to
     * be written as `mut`. Both references (`mut ref T` to `ref T`) and views
     * (`mut slice<T>` to `slice<T>`) behave this way. */
    if (want->kind == got->kind) {
        if (want->kind == TY_REF && got->mut && !want->mut &&
            ttEquals(want->inner, got->inner)) return true;
        /* Views: `mut` may be dropped recursively (`mut slice<mut slice<T>>` is usable
         * as `slice<slice<T>>`, never the other way round); see `ttViewDowngradable`. */
        if (want->kind == TY_GENERIC && ttViewDowngradable(want, got) &&
            !ttEquals(want, got)) return true;
    }
    if (ttCanWiden(got, want)) return true;
    if (node && literalFits(node, want)) return true;

    /* An integer literal that does not fit is a different mistake from a type mismatch,
     * so it gets its own message. (A float literal assigned to an integer variable is a
     * lossy conversion and falls through to the generic message below.) */
    if (node && node->kind == EX_INT && ttIsInteger(want)) {
        ckError(c, node->line, NULL,
                "%s: literal `%lld` does not fit in `%s`",
                what, node->u.ival, typeStr(c, want));
        return false;
    }

    ckError(c, node ? node->line : 0,
            "extC only widens implicitly (lossless); a lossy conversion must be written out",
            "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
    return false;
}

/* Require a condition to be `bool`.
 *
 * Params:
 *   c    - checker
 *   t    - type of the condition
 *   node - the condition expression, for its source position
 *
 * Notes:
 *   - extC has no implicit truthiness: an integer or a reference used as a condition is
 *     reported, not converted.
 */
void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "conditions must be `bool` -- extC has no implicit truthiness",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

