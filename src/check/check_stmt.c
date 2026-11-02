/**
 * @file check_stmt.c
 * @brief Statement checking: declarations, assignments, control flow, `match`, and the
 *        statement-level desugarings (`par`, element-chain fusion, `?` and `??` hoisting).
 *
 * Each statement is checked here for what it does to the depth bookkeeping and to the set of
 * narrowing facts; the expressions inside it are handed to the expression checker.
 */

#include "check_internal.h"
#include "plan.h"          /* plan: written through setters */
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------- statements */

/**
 * @brief Rewrite a bare constructor name in place into a qualified variant construction.
 *
 * Only `success` / `failure` / `some` / `none` are recognized, and the expected type must be the
 * matching `result` or `option` instance; a user-defined function with the same name keeps the name as
 * written. `none` is the only one whose parentheses may be omitted. The node is rewritten in place, so
 * later checking and code generation both take the ordinary variant-construction path.
 *
 * @param c     Checker.
 * @param e     Expression to rewrite; left untouched when it is not of that shape.
 * @param want  Expected type; decides whether the name is a known constructor.
 */
void desugarBareCtor(Checker *c, Expr *e, Type *want) {
    if (!e) return;

    const char *nm = NULL;
    const char *proto = NULL;
    size_t nargs = 0;
    bool noParens = false;

    if (e->kind == EX_CALL && e->u.call.callee->kind == EX_IDENT) {
        nm = e->u.call.callee->u.ident.name;
    } else if (e->kind == EX_IDENT) {
        /* A constructor without a payload may omit the parentheses, as enum variants already may. */
        nm = e->u.ident.name;
        noParens = true;
    } else return;

    if      (strcmp(nm, "success") == 0 || strcmp(nm, "failure") == 0) { proto = "result"; nargs = 2; }
    else if (strcmp(nm, "some")    == 0 || strcmp(nm, "none")    == 0) { proto = "option"; nargs = 1; }
    else return;
    if (noParens && strcmp(nm, "none") != 0) return;   /* `none` is the only variant without parentheses */

    /* A user-defined function of the same name wins: the short form is only used where unambiguous. */
    if (findFunc(c, nm)) return;
    /* The return type is not the matching container: the name is left as written and reported as
     * undefined. */
    if (!isProtoType(want, proto, nargs)) return;

    /* Union: take `args` out before changing the kind, or the new field would overwrite it. */
    Vec args;
    if (noParens) vecInit(&args, c->arena, sizeof(void *));
    else          args = e->u.call.args;
    /* `option` / `result` are ordinary enums, so a bare constructor is just a variant construction;
     * `typeName` holds the instance name, which code generation uses to build the C label constant
     * `option_i64_some`. */
    e->kind = EX_ENUMVAL;
    e->u.enumval.typeName = want->name;
    e->u.enumval.variant  = nm;
    e->u.enumval.args     = args;
    planSetAssocOwner(e, want);
}

/**
 * @brief Infer the type of SUBJ in `for x in SUBJ` without checking the expression.
 *
 * Only the shapes the parser allows are answered: a name, a field, or `*p`; all three can be read
 * directly from the symbol and field tables. The type must be available **without** checking:
 * `checkExpr` does not memoize, so checking here would report diagnostics twice. Other shapes return
 * NULL and the caller keeps the slice path.
 *
 * @param c  Checker.
 * @param e  Subject expression of the loop.
 * @return   The type of the subject; NULL when the shape is not recognized or cannot be resolved.
 */
static Type *forSubjectType(Checker *c, Expr *e) {
    switch (e ? e->kind : EX_NULL) {
    case EX_IDENT: {
        if (!e->u.ident.name) return NULL;
        Sym *sy = lookup(c, e->u.ident.name);
        return sy ? sy->type : NULL;
    }
    case EX_DEREF: {
        Type *inner = forSubjectType(c, e->u.deref.operand);
        if (!inner) return NULL;
        /* `ttBase` strips **all** `ref` layers, so the reference test looks at the substituted type
         * itself: `*p` takes the type of the pointee. */
        Type *it = tsub(c, inner);
        return (it && it->kind == TY_REF) ? it->inner : NULL;
    }
    case EX_FIELD: {
        Type *obj = forSubjectType(c, e->u.field.obj);
        if (!obj) return NULL;
        Type *ob = ttBase(tsub(c, obj));
        if (!ob || ob->kind != TY_STRUCT || !ob->sdef) return NULL;
        FieldDef *fd = findField(ob->sdef, e->u.field.name);
        return fd ? fd->type : NULL;
    }
    default: return NULL;
    }
}

/**
 * @brief Retarget a `for x in SUBJ` that was desugared into a slice loop to the iterator protocol.
 *
 * Only the fixed three-statement shape the parser builds for `for … in` is handled; the subject is left
 * alone when its type is sliceable. The statement is **retargeted in place, not rebuilt**: the loop
 * variable, the body, and the loop are still the parser's nodes, and the three protocol calls are
 * ordinary `EX_METHOD` nodes, so method resolution handles them as usual. A coroutine is its own
 * iterator (its frame holds the pc), so `next` / `value` are called directly on the subject with no
 * intermediate binding; the subject is a name, a field, or `*p`, so evaluating it twice has no effect.
 *
 * @param c      Checker.
 * @param block  Block statement desugared from `for … in`.
 * @return       True when retargeted to the iterator; false when the shape does not match or the
 *               subject is sliceable.
 */
static bool forRetargetToIterator(Checker *c, Stmt *block) {
    /* Shape: `[<hidden subject binding>, var __extc_s = SUBJ[..], var __extc_i, while …]`. The subject
     * binding appears only when the subject is not a place (`for d in split(s)`), so the length is
     * 3 or 4. */
    size_t bindIdx = (size_t)-1, wIdx = (size_t)-1;
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt *s = *(Stmt **)vecAt(&block->u.block.stmts, i);
        if (!s) continue;
        if (bindIdx == (size_t)-1 && s->kind == ST_VAR && s->u.var.init &&
            s->u.var.init->kind == EX_SLICE) { bindIdx = i; continue; }
        if (bindIdx != (size_t)-1 && s->kind == ST_WHILE) { wIdx = i; break; }
    }
    if (bindIdx == (size_t)-1 || wIdx != bindIdx + 2) return false;
    Stmt *bind = *(Stmt **)vecAt(&block->u.block.stmts, bindIdx);
    Stmt *w    = *(Stmt **)vecAt(&block->u.block.stmts, wIdx);
    if (!w || w->kind != ST_WHILE || !w->u.whiles.body ||
        w->u.whiles.body->kind != ST_BLOCK || w->u.whiles.body->u.block.stmts.len != 3)
        return false;
    Expr *subject = bind->u.var.init->u.slice.obj;
    Type *st = forSubjectType(c, subject);
    if (!st) return false;                       /* type unknown: keep the slice path */
    /* A coroutine is its own iterator: its frame holds the pc, so `next` / `value` are called on SUBJ
     * directly, with no `__extc_s` binding and no `__extc_it` alias to keep in sync. */
    {
        Type *sb0 = ttBase(tsub(c, st));
        if (sb0 && sb0->sdef && planCoroOf(sb0->sdef)) {
            Stmt *inner0 = w->u.whiles.body;
            Stmt *elemDecl0 = *(Stmt **)vecAt(&inner0->u.block.stmts, 0);
            if (!elemDecl0 || elemDecl0->kind != ST_VAR) return false;
            Expr *nx = exprNew(c->arena, EX_METHOD, w->line);
            nx->u.method.recv = subject;
            nx->u.method.name = "next";
            w->u.whiles.cond = nx;
            Expr *vl = exprNew(c->arena, EX_METHOD, elemDecl0->line);
            vl->u.method.recv = subject;
            vl->u.method.name = "value";
            elemDecl0->u.var.init = vl;
            inner0->u.block.stmts.len = 2;       /* `[ var x = ...value(); BODY ]`: the step statement is gone */
            *(Stmt **)vecAt(&block->u.block.stmts, bindIdx) = w;   /* drop the view binding */
            block->u.block.stmts.len = bindIdx + 1;
            return true;
        }
    }
    /* Container (`varArray<T>`, `vector<T>` and the like): it has neither a `.len` field nor `[..]`, so
     * it goes through its own `asSlice()`. Then `for p in parts` and `for p in parts.asSlice()` are one
     * path. */
    {
        Type *sbv = ttBase(tsub(c, st));
        /* `view()` is the convention; `asSlice()` is the older spelling and stays accepted. */
        const char *lend = NULL;
        if (sbv && sbv->kind != TY_ARRAY && !viewElemOf(sbv)) {
            if (findMethod(sbv, "view")) lend = "view";
            else if (findMethod(sbv, "asSlice")) lend = "asSlice";
        }
        if (lend) {
            bind->u.var.init = exprMethodNew(c->arena, subject, lend, bind->line);
            return true;
        }
    }
    Type *sb = ttBase(tsub(c, st));
    Type *elem = (sb && sb->kind == TY_ARRAY) ? sb->inner : viewElemOf(sb);
    if (elem) return false;                      /* sliceable: keep the slice path */
    if (!bind->u.var.name) return false;

    /* `var __extc_s = SUBJ`: binds the subject expression itself, still evaluated only once. */
    bind->u.var.init = subject;

    /* `var __extc_it = __extc_s.iter()` */
    Expr *iter = exprNew(c->arena, EX_METHOD, block->line);
    iter->u.method.recv = exprIdent(c->arena, bind->u.var.name, block->line);
    iter->u.method.name = "iter";
    Stmt *itDecl = stmtNew(c->arena, ST_VAR, block->line);
    itDecl->u.var.name = "__extc_it";
    itDecl->u.var.init = iter;
    itDecl->u.var.mut = true;                    /* `next` takes `mut ref Self` */
    *(Stmt **)vecAt(&block->u.block.stmts, bindIdx + 1) = itDecl;

    Stmt *inner = w->u.whiles.body;
    Stmt *elemDecl = *(Stmt **)vecAt(&inner->u.block.stmts, 0);
    Expr *next = exprNew(c->arena, EX_METHOD, w->line);
    next->u.method.recv = exprIdent(c->arena, "__extc_it", w->line);
    next->u.method.name = "next";
    w->u.whiles.cond = next;
    if (!elemDecl || elemDecl->kind != ST_VAR) return false;
    Expr *value = exprNew(c->arena, EX_METHOD, elemDecl->line);
    value->u.method.recv = exprIdent(c->arena, "__extc_it", elemDecl->line);
    value->u.method.name = "value";
    elemDecl->u.var.init = value;
    inner->u.block.stmts.len = 2;                /* `[ var x = ...value(); BODY ]`: the step statement is gone */
    /* The step statement was removed, so `continue` has no label to jump to: the advance happens in
     * the condition's `next()`. Without recording this, code generation would emit a `goto` to a label
     * that does not exist; only the fact is recorded, the tree is not changed. */
    planSetForStepDropped(inner);
    return true;
}

/**
 * @brief Widen the loop variable of a range loop to the type of its upper bound (`i` becomes `i64` in
 *        `for i in 0..n` when `n: i64`).
 *
 * The parser desugars `for i in lo..hi` into an outer block `[var i = lo, var __extc_hi = hi,
 * while i < __extc_hi { body; i += 1 }]`; `i` has no annotation and takes its type from `lo`, so
 * `for i in 0..n` gets `i32` even when `n` is `i64`. The comparison itself is done in the wider type
 * (both sides are widened), so the semantics are fine; but the **variable stays narrow**, so every
 * index arithmetic in the body narrows and then widens, the generated C is full of `(int64_t)(…)`, and
 * vectorization is lost.
 *
 * Widening only happens when `hi` can hold `lo` losslessly (`ttCanWiden`): the relative order of the
 * two sides of the comparison is unchanged, and the loop variable's arithmetic goes from "narrow then
 * wide" to "all wide". Same-type ends, a reversed range, a non-integer range, and shapes that are not
 * a range loop are left alone.
 *
 * Called from `checkBlockBody` after `stmts[1]` has been checked: both end types are settled, the
 * `while` has not been checked yet, and uses of `i` in the body resolve to the widened type.
 *
 * @param c      Checker.
 * @param block  Block statement; returns immediately when it is not the outer block of a range loop.
 */
static void widenRangeForVar(Checker *c, Stmt *block) {
    if (!block || block->kind != ST_BLOCK || block->u.block.stmts.len != 3) return;
    Stmt *decl = *(Stmt **)vecAt(&block->u.block.stmts, 0);
    Stmt *hiDecl = *(Stmt **)vecAt(&block->u.block.stmts, 1);
    Stmt *loop = *(Stmt **)vecAt(&block->u.block.stmts, 2);
    if (!decl || !hiDecl || !loop) return;
    if (decl->kind != ST_VAR || !decl->u.var.init || decl->u.var.ann) return;
    if (hiDecl->kind != ST_VAR || !hiDecl->u.var.name || hiDecl->u.var.ann) return;
    if (strcmp(hiDecl->u.var.name, "__extc_hi") != 0) return;
    if (loop->kind != ST_WHILE || !loop->u.whiles.cond) return;
    Expr *cond = loop->u.whiles.cond;
    if (cond->kind != EX_BIN || !cond->u.bin.op || strcmp(cond->u.bin.op, "<") != 0) return;
    if (!cond->u.bin.left || cond->u.bin.left->kind != EX_IDENT) return;
    if (!cond->u.bin.right || cond->u.bin.right->kind != EX_IDENT) return;
    if (!decl->u.var.name || !cond->u.bin.left->u.ident.name ||
        !cond->u.bin.right->u.ident.name) return;
    if (strcmp(cond->u.bin.left->u.ident.name, decl->u.var.name) != 0) return;
    if (strcmp(cond->u.bin.right->u.ident.name, "__extc_hi") != 0) return;

    Type *lo = decl->type, *hi = hiDecl->type;
    if (!lo || !hi || !ttIsInteger(lo) || !ttIsInteger(hi)) return;
    if (ttEquals(lo, hi) || !ttCanWiden(lo, hi)) return;
    Sym *sym = lookup(c, decl->u.var.name);
    if (!sym) return;
    decl->type = hi;
    sym->type = hi;
}

/* ----------------------------------------------------- `par` desugaring */

/**
 * @brief One entry on the `par` desugaring scan stack: a statement, an expression, or an "inner loop
 *        ended" marker.
 *
 * The scan uses an explicit stack rather than recursion: `tools/check_walkers.py` counts hand-written
 * recursive kind-walkers, and the `par` scan is only a **classifier** (incomplete; kinds it does not
 * enumerate are let through), so it should not add to that account.
 */
typedef struct {
    const void   *node;
    unsigned char tag;    /* 0 = Stmt*, 1 = Expr*, 2 = end of inner loop */
} ParItem;

/**
 * @brief Scan context of the `par` desugaring.
 *
 * The scan runs after the loop variable and `__extc_hi` have been checked, and before the `while` is:
 * the enclosing scope is still open, so a captured view name resolves through `lookup`; a `var`
 * inside the body is only recorded by name (its type is known only once the worker body is
 * re-checked).
 */
typedef struct {
    Checker    *c;
    const char *loopVar;    /* source name of the loop variable */
    const char *outName;    /* output view name; NULL while no write has been seen */
    int         outLine;    /* line of the first write */
    Vec         locals;     /* const char*: names of the `var`s declared in the body */
    Vec         captures;   /* const char*: captured read-only view names, in first-seen order */
    Vec         scalars;    /* const char*: by-value captured scalar names, in first-seen order */
    Vec         parVars;    /* const char*: `par` loop variable names (inner one too when folding 2-D) */
    const char *innerVar;   /* inner loop variable name when folding 2-D; NULL otherwise */
    Expr       *innerLo;    /* lower bound of the inner loop range */
    Expr       *innerHi;    /* upper bound of the inner loop range */
    int         foldDepth;  /* inner `par` levels already folded (v1 allows at most 1) */
    bool        fullOut;    /* inner `par` folded: the whole output view goes to the worker */
    Vec         items;      /* ParItem: explicit work stack */
    int         loopDepth;  /* inner loop depth: `break`/`continue` stay inside the kernel only at depth > 0 */
    bool        failed;     /* an error was already reported: stop scanning (avoid a flood) */
} ParLower;

/**
 * @brief Tell whether a name is in a string table.
 *
 * @param v     Table (elements of type `const char *`).
 * @param name  Name to find; NULL gives false.
 * @return      True when the name is in the table.
 */
static bool parHasName(const Vec *v, const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt((Vec *)v, i), name) == 0) return true;
    return false;
}

/**
 * @brief Push one entry onto the scan stack.
 *
 * @param w     Scan context.
 * @param node  Statement or expression node.
 * @param tag   0 = statement, 1 = expression, 2 = inner loop ended.
 */
static void parPushItem(ParLower *w, const void *node, unsigned char tag) {
    ParItem *it = (ParItem *)vecPush(&w->items);
    it->node = node;
    it->tag  = tag;
}

/**
 * @brief Record a local name declared inside the body (duplicates are recorded once).
 *
 * @param w     Scan context.
 * @param name  Local variable name.
 */
static void parAddLocal(ParLower *w, const char *name) {
    if (!name || parHasName(&w->locals, name)) return;
    *(const char **)vecPush(&w->locals) = name;
}

/**
 * @brief Record a captured read-only view name (duplicates are recorded once).
 *
 * @param w     Scan context.
 * @param name  View name.
 */
static void parAddCapture(ParLower *w, const char *name) {
    if (!name || parHasName(&w->captures, name)) return;
    *(const char **)vecPush(&w->captures) = name;
}

/**
 * @brief Record a scalar name captured by value (duplicates are recorded once).
 *
 * A scalar has no shared state: its value is evaluated once before the call and stays fixed, so
 * capturing it by value is sound; a worker gets its own copy, and writing it writes that copy only.
 *
 * @param w     Scan context.
 * @param name  Scalar name.
 */
static void parAddScalar(ParLower *w, const char *name) {
    if (!name || parHasName(&w->scalars, name)) return;
    *(const char **)vecPush(&w->scalars) = name;
}

/**
 * @brief Record a `par` loop variable name (a write target's index may consist only of these names
 *        and constants).
 *
 * @param w     Scan context.
 * @param name  Loop variable name.
 */
static void parAddParVar(ParLower *w, const char *name) {
    if (!name || parHasName(&w->parVars, name)) return;
    *(const char **)vecPush(&w->parVars) = name;
}

/**
 * @brief Tell whether a scalar type can be captured by value.
 *
 * Only builtin scalars are accepted: integers, `f32`/`f64`, `bool`. Views take the other path (shared
 * by reference); structs, references, and arrays -- types that carry interior pointers or mutability
 * -- are not accepted: this is the captured surface of v1.
 *
 * @param t  Type (already `ttBase`d).
 * @return   True when it can be captured by value.
 */
static bool parIsScalarCopyable(Type *t) {
    return t && (ttIsInteger(t) || ttIs(t, "f64") || ttIs(t, "f32") || ttIs(t, "bool"));
}

/**
 * @brief Report one `par` restriction-set diagnostic and stop the scan.
 *
 * @param w     Scan context.
 * @param line  Source line.
 * @param note  Supplementary note explaining "why" (may be NULL).
 * @param msg   Main diagnostic text.
 */
static void parFail(ParLower *w, int line, const char *note, const char *msg) {
    if (w->failed) return;
    w->failed = true;
    ckError(w->c, line, note, "%s", msg);
}

/**
 * @brief Tell whether a name resolves to a view in the current scope.
 *
 * @param w     Scan context.
 * @param name  Name.
 * @return      True when it resolves to a view type.
 */
static bool parNameIsView(ParLower *w, const char *name) {
    if (!name) return false;
    Sym *sym = lookup(w->c, name);
    return sym && sym->type && ttIsViewType(ttBase(sym->type));
}

/**
 * @brief Tell whether two expressions denote "the same value" (the criterion stops at names and
 *        integer literals).
 *
 * A 2-D flattened expansion constant has to be shown to be at least the inner iteration count, and
 * the usual way to write it uses the inner upper bound itself as the constant (`out[r * cols + c]`
 * with an inner range `0..cols`), so recognizing the same name / same number is all that is needed.
 *
 * @param a  Expression.
 * @param b  Expression.
 * @return   True for identical identifiers, equal integer literals, or structurally equal additive /
 *           multiplicative combinations.
 */
static bool parSameExpr(Expr *a, Expr *b) {
    if (!a || !b) return false;
    if (a->kind == EX_CONV) return parSameExpr(a->u.conv.operand, b);
    if (b->kind == EX_CONV) return parSameExpr(a, b->u.conv.operand);
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case EX_IDENT:
        return a->u.ident.name && b->u.ident.name &&
               strcmp(a->u.ident.name, b->u.ident.name) == 0;
    case EX_INT:
        return a->u.ival == b->u.ival;
    case EX_BIN:
        return a->u.bin.op && b->u.bin.op && strcmp(a->u.bin.op, b->u.bin.op) == 0 &&
               parSameExpr(a->u.bin.left, b->u.bin.left) &&
               parSameExpr(a->u.bin.right, b->u.bin.right);
    default:
        return false;
    }
}


/**
 * @brief Read an expression as an integer-coefficient affine form `A * hi + B * lo + C` in the inner
 *        bounds.
 *
 * Only `+ - *`, integer literals, and the two names of the inner loop variable / upper bound are
 * recognized; a multiplication needs a pure constant on at least one side. Every other shape (calls,
 * subscripts, outer-scope names, ...) fails: the proof accepts only arithmetic it understands.
 *
 * @param w      Scan context, providing the inner variable's and bounds' names.
 * @param e      Expression to read.
 * @param coeff  Output: three coefficients, `[0]` = hi, `[1]` = lo, `[2]` = constant term.
 * @return       True when the expression was read; false otherwise (`coeff` is then meaningless).
 */
static bool parAffineOf(ParLower *w, Expr *e, long long coeff[3]) {
    coeff[0] = coeff[1] = coeff[2] = 0;
    if (!e) return false;
    while (e->kind == EX_CONV) e = e->u.conv.operand;
    if (e->kind == EX_INT) { coeff[2] = (long long)e->u.ival; return true; }
    if (e->kind == EX_IDENT && e->u.ident.name) {
        if (w->innerHi && w->innerHi->kind == EX_IDENT && w->innerHi->u.ident.name &&
            strcmp(e->u.ident.name, w->innerHi->u.ident.name) == 0) { coeff[0] = 1; return true; }
        if (w->innerLo && w->innerLo->kind == EX_IDENT && w->innerLo->u.ident.name &&
            strcmp(e->u.ident.name, w->innerLo->u.ident.name) == 0) { coeff[1] = 1; return true; }
        return false;
    }
    if (e->kind != EX_BIN || !e->u.bin.op) return false;
    long long l[3], r[3];
    if (!parAffineOf(w, e->u.bin.left, l) || !parAffineOf(w, e->u.bin.right, r)) return false;
    if (strcmp(e->u.bin.op, "+") == 0) {
        for (int i = 0; i < 3; i++) coeff[i] = l[i] + r[i];
        return true;
    }
    if (strcmp(e->u.bin.op, "-") == 0) {
        for (int i = 0; i < 3; i++) coeff[i] = l[i] - r[i];
        return true;
    }
    if (strcmp(e->u.bin.op, "*") == 0) {
        bool lc = l[0] == 0 && l[1] == 0, rc = r[0] == 0 && r[1] == 0;
        if (lc && rc) { coeff[2] = l[2] * r[2]; return true; }
        if (lc) { for (int i = 0; i < 3; i++) coeff[i] = r[i] * l[2]; return true; }
        if (rc) { for (int i = 0; i < 3; i++) coeff[i] = l[i] * r[2]; return true; }
        return false;
    }
    return false;
}

/**
 * @brief Tell whether the "cells per row" `K` of a 2-D flattening is at least the inner iteration
 *        count.
 *
 * The required proof is `K >= hi - lo`, for every `hi >= lo`. Method: read `K` as the affine form
 * `A*hi + B*lo + C`; then `K - (hi - lo) = (A-1)*hi + (B+1)*lo + C` is non-negative for all inputs as
 * soon as `A >= 1`, `A + B >= 0`, and `C >= 0` (with `hi >= lo >= 0`; a negative `lo` is covered by
 * the inner loop's own check). This proves `cols`, `cols + 1`, `2 * cols`, `cols + rowPad`, and the
 * like. The more direct path -- all terms are integer literals, compare them -- is kept as well.
 *
 * @param w  Scan context (the inner loop variable and bounds supply the symbols).
 * @param k  Cells per row.
 * @return   True when `k` can be shown to cover the whole inner range.
 */
static bool parFlattenConstOk(ParLower *w, Expr *k) {
    if (!k || !w->innerHi) return false;
    Expr *lo = w->innerLo, *hi = w->innerHi;
    while (lo && lo->kind == EX_CONV) lo = lo->u.conv.operand;
    while (hi && hi->kind == EX_CONV) hi = hi->u.conv.operand;
    if (parSameExpr(k, hi) && lo && lo->kind == EX_INT && lo->u.ival == 0) return true;
    Expr *kk = k;
    while (kk && kk->kind == EX_CONV) kk = kk->u.conv.operand;
    if (kk && kk->kind == EX_INT && hi && hi->kind == EX_INT && lo && lo->kind == EX_INT)
        return kk->u.ival >= hi->u.ival - lo->u.ival;
    long long c[3];
    if (!parAffineOf(w, k, c)) return false;
    return c[0] >= 1 && c[0] + c[1] >= 0 && c[2] >= 0;
}

/**
 * @brief Tell whether a 2-D flattening's write index maps each `(r, c)` to its own cell: only
 *        `out[r * K + c]` is recognized.
 *
 * The 1-D slot rule (the index is the loop variable) is injective by construction; after flattening
 * two loops, "the index is made of loop variables" is not enough -- `out[c]` has no `r`, so every
 * outer worker rewrites the whole column. The only provable shapes are `outer * K + inner` (with
 * `K >=` the inner iteration count) and the equivalent form with the two loops swapped.
 *
 * @param w  Scan context.
 * @param e  Write index.
 * @return   True when one cell per slot is proven.
 */
static bool parIndexInjective(ParLower *w, Expr *e) {
    if (!e || e->kind != EX_BIN || !e->u.bin.op || strcmp(e->u.bin.op, "+") != 0) return false;
    Expr *l = e->u.bin.left, *r = e->u.bin.right;
    for (int swap = 0; swap < 2; swap++) {
        Expr *sum = swap ? r : l, *prod = swap ? l : r;
        if (!sum || sum->kind != EX_IDENT || !sum->u.ident.name) continue;
        if (!w->innerVar || strcmp(sum->u.ident.name, w->innerVar) != 0) continue;
        if (!prod || prod->kind != EX_BIN || !prod->u.bin.op ||
            strcmp(prod->u.bin.op, "*") != 0)
            continue;
        Expr *a = prod->u.bin.left, *b = prod->u.bin.right;
        if (a && a->kind == EX_IDENT && w->loopVar && a->u.ident.name &&
            strcmp(a->u.ident.name, w->loopVar) == 0 && parFlattenConstOk(w, b))
            return true;
        if (b && b->kind == EX_IDENT && w->loopVar && b->u.ident.name &&
            strcmp(b->u.ident.name, w->loopVar) == 0 && parFlattenConstOk(w, a))
            return true;
    }
    return false;
}

/**
 * @brief Tell whether a write target's index is built only from `par` loop variables and integer
 *        constants.
 *
 * A 2-D flattened worker writes by absolute slot (`out[r * cols + c]`), so the index is "an integer
 * expression in the loop variables": integer constants, `par` loop variables, `+ - *`, and unary `-`.
 * Other names (captured scalars/views, globals) and calls are not accepted.
 *
 * @param w  Scan context (`parVars` is filled in).
 * @param e  Index expression.
 * @return   True when the shape is allowed.
 */
static bool parIndexOverLoops(ParLower *w, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT:
        return true;
    case EX_IDENT:
        /* A loop variable, or a scalar captured by value (an immutable copy inside the worker -- the
         * `cols` of `out[r * cols + c]` is one). A view in an index is a type error, reported by the
         * worker re-check. */
        return parHasName(&w->parVars, e->u.ident.name) || !parNameIsView(w, e->u.ident.name);
    case EX_BIN:
        if (!e->u.bin.op) return false;
        if (strcmp(e->u.bin.op, "+") != 0 && strcmp(e->u.bin.op, "-") != 0 &&
            strcmp(e->u.bin.op, "*") != 0)
            return false;
        return parIndexOverLoops(w, e->u.bin.left) && parIndexOverLoops(w, e->u.bin.right);
    case EX_UN:
        if (!e->u.un.op || strcmp(e->u.un.op, "-") != 0) return false;
        return parIndexOverLoops(w, e->u.un.operand);
    case EX_CONV:
        return parIndexOverLoops(w, e->u.conv.operand);
    case EX_SIGN:
        return parIndexOverLoops(w, e->u.sign.operand);
    default:
        return false;
    }
}

/**
 * @brief Classify an assignment target against the `par` restriction set, and rewrite an output
 *        write's index into a sub-view index.
 *
 * Only two targets are allowed: a local scalar declared in the body (private, so freely writable),
 * and `out[i]` on the **single** output view. The latter additionally requires that `out` is a view
 * and that `i` is exactly the loop variable. The rewrite turns `out[i]` into `out[i - __extc_plo]`:
 * the trampoline hands the worker the zero-based sub-view cut to `[lo, hi)`.
 *
 * @param w  Scan context.
 * @param t  Assignment target expression.
 */
static void parScanTarget(ParLower *w, Expr *t) {
    if (!t || w->failed) return;
    /* A private scalar declared in the body: writing it shares nothing, so it is freely writable. */
    if (t->kind == EX_IDENT && t->u.ident.name && parHasName(&w->locals, t->u.ident.name))
        return;
    if (t->kind != EX_INDEX) {
        parFail(w, t->line,
                "Shared data is written only through the one output view the loop declared;"
                " a local `var` is private and can be written freely.",
                "this par loop may write only its own output slot");
        return;
    }
    Expr *obj = t->u.index.obj;
    Expr *idx = t->u.index.index;
    if (!obj || obj->kind != EX_IDENT || !obj->u.ident.name) {
        parFail(w, t->line, NULL, "this par loop may write only its own output slot");
        return;
    }
    const char *name = obj->u.ident.name;
    if (parHasName(&w->locals, name)) {      /* private scalar */
        parPushItem(w, idx, 1);
        return;
    }
    if (!parNameIsView(w, name)) {
        parFail(w, t->line,
                "A par loop shares only read-only views and writes only its output view's own"
                " slot; writing a captured scalar or another name is not one of those shapes.",
                "this par loop may write only its own output slot");
        return;
    }
    if (w->outName && strcmp(w->outName, name) != 0) {
        parFail(w, t->line,
                "One loop has exactly one output view; a separate write names a different"
                " view, which makes the slot each worker owns ambiguous.",
                "this par loop may write only its own output slot");
        return;
    }
    if (!w->outName) {
        w->outName = name;
        w->outLine = t->line;
    }
    if (w->fullOut) {
        /* 2-D flattening: the index is "an integer expression in the loop variables"
         * (`out[r * cols + c]`). Only integer constants, `par` loop variables, `+ - *`, and unary `-`
         * are accepted. */
        if (!parIndexOverLoops(w, idx) || !parIndexInjective(w, idx)) {
            parFail(w, t->line,
                    "A folded two-dimensional loop writes `out[r * K + c]` with `K` at least the"
                    " inner loop's iteration count (the inner upper bound is the usual choice):"
                    " anything else lets two outer iterations land in the same slot.",
                    "this par loop may write only its own output slot");
            return;
        }
        parPushItem(w, idx, 1);
        return;                                 /* absolute index: no rewrite */
    }
    if (!idx || idx->kind != EX_IDENT || !idx->u.ident.name ||
        strcmp(idx->u.ident.name, w->loopVar) != 0) {
        parFail(w, t->line,
                "The slot a worker owns is decided by the loop index, so the subscript has to"
                " be the loop variable itself.",
                "this par loop may write only its own output slot");
        return;
    }
    /* `out[i]` -> `out[i - lo]`: the trampoline supplies the `[lo, hi)` sub-view. */
    Expr *lo = exprIdent(w->c->arena, "__extc_plo", t->line);
    Expr *sub = exprBinNew(w->c->arena, "-", idx, lo, t->line);
    sub->type = w->c->tI64;
    exprIndexRetarget(t, sub);
}

/**
 * @brief Scan one expression node: classify names, reject allocations and calls, push subexpressions.
 *
 * @param w  Scan context.
 * @param e  Expression.
 */
static void parScanExpr(ParLower *w, Expr *e) {
    if (!e || w->failed) return;
    switch (e->kind) {
    case EX_CALL:
    case EX_METHOD:
    case EX_ASSOC:
    case EX_GENCALL:
    case EX_EXT:
        parFail(w, e->line,
                "A worker runs on another thread; the first version keeps the body to scalar"
                " arithmetic and view reads so no call can touch shared state.",
                "cannot call a function inside a par loop");
        return;
    case EX_NEW:
        parFail(w, e->line,
                "A worker's allocations would have to outlive the loop, and the worker's arena"
                " is private to the thread; the first version keeps allocation outside.",
                "cannot allocate inside a par loop");
        return;
    case EX_SLICE:
        parFail(w, e->line,
                "A sub-view of shared data would make the slot a worker owns impossible to"
                " decide; index the view directly instead.",
                "cannot take a slice inside a par loop");
        return;
    case EX_STRUCTLIT:
    case EX_ARRAYLIT:
    case EX_DYN:
    case EX_TRY:
    case EX_COALESCE:
    case EX_ENUMVAL:
    case EX_LAMBDA:
    case EX_NULL:
        parFail(w, e->line, NULL, "cannot build a value inside a par loop");
        return;
    case EX_CONV:
        /* A scalar conversion is pure (it may carry a truncation check, whose only failure is a
         * `trap`): `f64(i)` is exactly the most common element-wise loop body. */
        parPushItem(w, e->u.conv.operand, 1);
        return;
    case EX_IDENT: {
        const char *name = e->u.ident.name;
        if (!name || strcmp(name, w->loopVar) == 0) return;
        if (parHasName(&w->locals, name)) return;
        if (w->outName && strcmp(w->outName, name) == 0) {
            parFail(w, e->line,
                    "One iteration must not see another's half-written value, so the output"
                    " view is write-only inside the loop.",
                    "the output view of a par loop aliases a read-only view");
            return;
        }
        Sym *sym = lookup(w->c, name);
        if (!sym) return;                       /* name from an outer scope: left to the worker's own re-check */
        if (parNameIsView(w, name)) {
            parAddCapture(w, name);
            return;
        }
        if (parIsScalarCopyable(sym->type)) {   /* captured by value: the copy shares no state */
            parAddScalar(w, name);
            return;
        }
        parFail(w, e->line,
                "Only read-only views and builtin scalars have a capture story: views are"
                " shared by pointer, scalars are copied by value; other values would hide"
                " writes from the caller.",
                "only read-only views and scalar values can be captured by a par loop");
        return;
    }
    case EX_BIN:
        parPushItem(w, e->u.bin.left, 1);
        parPushItem(w, e->u.bin.right, 1);
        return;
    case EX_UN:
    case EX_SIGN:
    case EX_REF:
    case EX_DEREF:
        parPushItem(w, e->u.un.operand, 1);
        return;
    case EX_FIELD:
        parPushItem(w, e->u.field.obj, 1);
        return;
    case EX_INDEX:
        parPushItem(w, e->u.index.obj, 1);
        parPushItem(w, e->u.index.index, 1);
        return;
    case EX_INT:
    case EX_FLOAT:
    case EX_BOOL:
    case EX_STR:
    default:
        return;
    }
}

/**
 * @brief Fold one inner `par` block into the outer kernel: clear the marker, record the inner loop
 *        variable, and switch to the whole output view.
 *
 * An inner `par` does not start a second batch of threads: it is an ordinary loop inside the worker
 * (the designed semantics of `par r in .. { par c in .. }`). When the shape is wrong (not a range-loop
 * block) it is left alone, and the worker re-check reports the error.
 *
 * @param w    Scan context.
 * @param blk  Inner `par` block.
 * @return     True when the fold succeeded (or was not needed); false when the nesting is too deep.
 */
static bool parFoldInner(ParLower *w, Stmt *blk) {
    if (!blk || blk->kind != ST_BLOCK || !blk->parLoop) return true;
    if (w->foldDepth >= 1) {
        parFail(w, blk->line,
                "A worker is one range loop nesting: `par r in .. { par c in .. { … } }`.",
                "cannot nest `par` more than two levels");
        return false;
    }
    w->foldDepth++;
    w->fullOut = true;
    stmtSetParLoop(blk, false);
    if (blk->u.block.stmts.len >= 1) {
        Stmt *idecl = *(Stmt **)vecAt(&blk->u.block.stmts, 0);
        if (idecl && idecl->kind == ST_VAR && idecl->u.var.name) {
            w->innerVar = idecl->u.var.name;
            w->innerLo  = idecl->u.var.init;
            parAddParVar(w, idecl->u.var.name);
            /* `[loop variable, upper bound, while]`: the upper-bound declaration stores the
             * expression for the inner range's end. */
            if (blk->u.block.stmts.len >= 2) {
                Stmt *hdecl = *(Stmt **)vecAt(&blk->u.block.stmts, 1);
                if (hdecl && hdecl->kind == ST_VAR) w->innerHi = hdecl->u.var.init;
            }
        }
    }
    return true;
}

/**
 * @brief Scan one statement: collect declarations, check write targets, push sub-blocks and
 *        sub-expressions.
 *
 * @param w  Scan context.
 * @param s  Statement.
 */
static void parScanStmt(ParLower *w, Stmt *s) {
    if (!s || w->failed) return;
    switch (s->kind) {
    case ST_VAR:
        parAddLocal(w, s->u.var.name);
        if (s->u.var.init && s->u.var.init->kind == EX_IDENT &&
            parNameIsView(w, s->u.var.init->u.ident.name)) {
            parFail(w, s->line,
                    "Reading the view directly gives the loop its value; a local view would"
                    " need the same disjointness proof the slot rule avoids.",
                    "cannot bind a view to a local inside a par loop");
            return;
        }
        parPushItem(w, s->u.var.init, 1);
        return;
    case ST_ASSIGN:
        parScanTarget(w, s->u.assign.target);
        parPushItem(w, s->u.assign.value, 1);
        return;
    case ST_EXPR:
        parPushItem(w, s->u.expr.expr, 1);
        return;
    case ST_IF:
        parPushItem(w, s->u.ifs.cond, 1);
        parPushItem(w, s->u.ifs.thenBody, 0);
        parPushItem(w, s->u.ifs.elseBody, 0);
        return;
    case ST_WHILE:
        w->loopDepth++;
        parPushItem(w, NULL, 2);                 /* leaving the inner loop: put the depth back */
        parPushItem(w, s->u.whiles.cond, 1);
        parPushItem(w, s->u.whiles.body, 0);
        return;
    case ST_BLOCK:
        /* A desugared `for`'s step statement hangs off the body block (`forStep`): it is loop
         * machinery, not a user-written assignment -- it writes the loop variable itself. The scan
         * skips it, matching the view taken by `widenRangeForVar`. */
        for (size_t i = s->u.block.stmts.len; i > 0; i--) {
            Stmt *child = *(Stmt **)vecAt(&s->u.block.stmts, i - 1);
            if (child && child == s->forStep) continue;
            if (child && child->kind == ST_BLOCK && child->parLoop &&
                !parFoldInner(w, child)) return;
            parPushItem(w, child, 0);
        }
        return;
    case ST_MATCH: {
        Vec *arms = &s->u.match.arms;
        parPushItem(w, s->u.match.scrutinee, 1);
        for (size_t i = arms->len; i > 0; i--) {
            MatchArm *a = *(MatchArm **)vecAt(arms, i - 1);
            parPushItem(w, a->body, 0);
        }
        return;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        if (w->loopDepth > 0) return;
        parFail(w, s->line,
                "A worker takes exactly the slots it is handed; leaving the loop early would"
                " let the join see a different iteration set.",
                "cannot escape a par loop");
        return;
    case ST_RETURN:
    case ST_YIELD:
    case ST_DOMAIN:
        parFail(w, s->line,
                "A worker takes exactly the slots it is handed; leaving the loop early would"
                " let the join see a different iteration set.",
                "cannot escape a par loop");
        return;
    case ST_TRAP:
        parPushItem(w, s->u.trap_.msg, 1);
        return;
    default:
        return;
    }
}

/**
 * @brief Scan the loop body with the explicit stack.
 *
 * @param w     Scan context.
 * @param body  Loop body block.
 * @return      True when the scan finished without diagnostics.
 */
static bool parScanBody(ParLower *w, Stmt *body) {
    if (!parFoldInner(w, body)) return false;
    parPushItem(w, body, 0);
    while (w->items.len > 0) {
        ParItem *it = (ParItem *)vecAt(&w->items, w->items.len - 1);
        const void   *node = it->node;
        unsigned char tag  = it->tag;
        w->items.len--;
        if (w->failed) return false;
        if (tag == 0)      parScanStmt(w, (Stmt *)node);
        else if (tag == 1) parScanExpr(w, (Expr *)node);
        else if (w->loopDepth > 0) w->loopDepth--;
    }
    return !w->failed;
}

/**
 * @brief Build one worker parameter.
 *
 * @param c     Checker (supplies the arena).
 * @param name  Name as written in the source (that is, the name the worker scope must resolve).
 * @param type  Parameter type.
 * @param line  Source line.
 * @return      The new parameter.
 */
static Param *parParam(Checker *c, const char *name, Type *type, int line) {
    Param *p = (Param *)arenaAllocZero(c->arena, sizeof(Param));
    p->name = name;
    p->type = type;
    p->line = line;
    return p;
}

/**
 * @brief Build an `i32` integer-literal expression.
 *
 * @param c      Checker.
 * @param value  Value.
 * @param line   Source line.
 * @return       An `EX_INT` node with its type filled in.
 */
static Expr *parIntLit(Checker *c, long long value, int line) {
    Expr *e = exprIntNew(c->arena, value, line);
    e->type = c->tI32;
    return e;
}

/**
 * @brief Build an already-checked `EX_IDENT` reference carrying its C name (for synthesised call
 *        arguments).
 *
 * These nodes are never handed to `checkExpr`: the binding they refer to was already checked in the
 * enclosing scope, so copying `planCName` over is enough, and code generation emits them like
 * ordinary identifiers.
 *
 * @param c     Checker.
 * @param name  Source name.
 * @param sym   The binding it resolves to.
 * @param line  Source line.
 * @return      An identifier node with type and C name filled in.
 */
static Expr *parCheckedIdent(Checker *c, const char *name, Sym *sym, int line) {
    Expr *e = exprIdent(c->arena, name, line);
    e->type = sym->type;
    planSetCName(e, sym->cname, RKIND_EXPR);
    return e;
}

/**
 * @brief Synthesise the worker function and append it to the module.
 *
 * The worker has exactly the shape of a hand-written `parallel::run` worker:
 * `(id, lo, hi, <captured read-only views...>, out: mut slice<T>) -> i32`. The name is the first free
 * `__extc_parw<N>` (code generation builds `__extc_par_<name>` straight from `wf->name`, so it has to
 * be a valid C identifier).
 *
 * @param w       Scan context (capture table and output view name already filled in).
 * @param decl    Loop-variable declaration (supplies the lo initialiser).
 * @param hiDecl  The `__extc_hi` declaration (supplies the hi initialiser).
 * @param loop    Original `while` node.
 * @return        The synthesised worker.
 */
static FuncDef *parMakeWorker(ParLower *w, Stmt *decl, Stmt *hiDecl, Stmt *loop) {
    Checker *c = w->c;
    FuncDef *wf = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    Buf nm;
    bufInit(&nm, c->arena);
    size_t serial = c->m->funcs.len;
    for (;; serial++) {
        bufInit(&nm, c->arena);
        bufPrintf(&nm, "__extc_parw%zu", serial);
        if (!findFunc(c, bufCstr(&nm))) break;
    }
    wf->name      = bufCstr(&nm);
    wf->ctx       = c->ctx;
    funcDefSetModName(wf, c->curFunc ? c->curFunc->modName : NULL);
    wf->line      = decl->line;
    vecInit(&wf->typeParams, c->arena, sizeof(void *));
    vecInit(&wf->targs,      c->arena, sizeof(void *));
    vecInit(&wf->params,     c->arena, sizeof(void *));

    int line = decl->line;
    *(Param **)vecPush(&wf->params) = parParam(c, "id",              c->tI64, line);
    *(Param **)vecPush(&wf->params) = parParam(c, "__extc_plo",      c->tI64, line);
    *(Param **)vecPush(&wf->params) = parParam(c, "__extc_phi",      c->tI64, line);
    for (size_t i = 0; i < w->captures.len; i++) {
        const char *cn = *(const char **)vecAt(&w->captures, i);
        Sym *sym = lookup(c, cn);
        *(Param **)vecPush(&wf->params) =
            parParam(c, cn, ttViewReadonly(c->tt, sym->type), line);
    }
    for (size_t i = 0; i < w->scalars.len; i++) {
        const char *sn = *(const char **)vecAt(&w->scalars, i);
        Sym *sym = lookup(c, sn);
        *(Param **)vecPush(&wf->params) = parParam(c, sn, sym->type, line);
    }
    Sym *outSym = lookup(c, w->outName);
    *(Param **)vecPush(&wf->params) = parParam(c, w->outName, outSym->type, line);

    /* Body: `[var <i> = lo, var __extc_hi = hi, while ..., return 0]`. */
    decl->u.var.init   = exprIdent(c->arena, "__extc_plo", line);
    hiDecl->u.var.init = exprIdent(c->arena, "__extc_phi", line);
    Stmt *ret = stmtNew(c->arena, ST_RETURN, line);
    ret->u.ret.value = parIntLit(c, 0, line);
    Stmt *body = stmtNew(c->arena, ST_BLOCK, line);
    vecInit(&body->u.block.stmts, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&body->u.block.stmts) = decl;
    *(Stmt **)vecPush(&body->u.block.stmts) = hiDecl;
    *(Stmt **)vecPush(&body->u.block.stmts) = loop;
    *(Stmt **)vecPush(&body->u.block.stmts) = ret;
    wf->body = body;
    wf->ret  = c->tI32;
    /* A 2-D folded worker writes absolute slots, so the trampoline passes the whole output view
     * instead of the `[lo, hi)` cut. */
    if (w->fullOut) planSetParFullOut(wf, true);

    *(FuncDef **)vecPush(&c->m->funcs) = wf;   /* the main check loop calls checkFunc on it afterwards */
    return wf;
}

/**
 * @brief Desugar one `par` block into a call to a synthesised worker.
 *
 * Timing matches `widenRangeForVar` (right after the first two of
 * `[loop variable, __extc_hi, while]` have been checked): both ends have types, the names resolve in
 * the enclosing scope, and the `while` has not been checked yet. On success the block holds a single
 * `ST_EXPR(EX_CALL)` and carries the `plan` flags -- exactly those the `parallel::run` builtin
 * validation would set. On failure the block is left as it is, so the ordinary check still reports
 * the error.
 *
 * @param c      Checker.
 * @param block  Block carrying the `parLoop` marker.
 */
static void parLower(Checker *c, Stmt *block) {
    if (c->curParams && c->curParams->len > 0) {
        ckError(c, block->line,
                "A generic body is checked once per instantiation, and each instance would"
                " need its own worker; the first version keeps `par` out of generics.",
                "cannot use `par` inside a generic function");
        return;
    }
    if (c->curFunc && planIsCoro(c->curFunc)) {
        ckError(c, block->line,
                "A coroutine body is split at its yields, so a parallel loop has no single"
                " frame to borrow the output view from.",
                "cannot use `par` inside a coroutine");
        return;
    }
    if (block->u.block.stmts.len != 3) {
        ckError(c, block->line, NULL,
                "`par` needs a range loop: `par i in lo..hi { … }`");
        return;
    }
    Stmt *decl   = *(Stmt **)vecAt(&block->u.block.stmts, 0);
    Stmt *hiDecl = *(Stmt **)vecAt(&block->u.block.stmts, 1);
    Stmt *loop   = *(Stmt **)vecAt(&block->u.block.stmts, 2);
    if (!decl || !hiDecl || !loop || decl->kind != ST_VAR || hiDecl->kind != ST_VAR ||
        loop->kind != ST_WHILE) {
        ckError(c, block->line, NULL,
                "`par` needs a range loop: `par i in lo..hi { … }`");
        return;
    }
    /* The index is `i64`: both ends must convert into `i64` without loss (an `i32` literal like
     * `0..n` is allowed -- it is an exact constant, and comparisons and arithmetic widen to `i64`). */
    if (!decl->type || !hiDecl->type || !ttIsInteger(decl->type) || !ttIsInteger(hiDecl->type) ||
        !ttCanWiden(decl->type, c->tI64) || !ttCanWiden(hiDecl->type, c->tI64)) {
        ckError(c, decl->line,
                "The index a worker's slot is decided by is an `i64`, so both bounds have to"
                " widen into it without loss.",
                "par loop bounds must be integers that fit in i64");
        return;
    }
    ParLower w = {0};
    w.c       = c;
    w.loopVar = decl->u.var.name;
    vecInit(&w.locals,   c->arena, sizeof(void *));
    vecInit(&w.captures, c->arena, sizeof(void *));
    vecInit(&w.scalars,  c->arena, sizeof(void *));
    vecInit(&w.parVars,  c->arena, sizeof(void *));
    vecInit(&w.items,    c->arena, sizeof(ParItem));
    parAddParVar(&w, decl->u.var.name);
    Expr *loInit = decl->u.var.init;
    Expr *hiInit = hiDecl->u.var.init;
    if (!parScanBody(&w, loop->u.whiles.body)) return;
    if (!w.outName) {
        ckError(c, block->line,
                "The loop writes nothing, so there is no slot to give a worker; every"
                " iteration would be pure work with no result.",
                "a par loop needs exactly one output view it writes into");
        return;
    }
    Sym *outSym = lookup(c, w.outName);
    if (!outSym || !outSym->type || !outSym->type->mut) {
        ckError(c, w.outLine, NULL,
                "this par loop may write only its own output slot");
        return;
    }
    FuncDef *wf = parMakeWorker(&w, decl, hiDecl, loop);

    /* The synthesised call: `__extc_parwN(0, captured views..., out, hi - lo, -1)`. `args[0]` is a
     * placeholder: code generation never calls genExpr on it for a worker call. `-1` is the sentinel
     * for the thread count, which code generation replaces with `extc_par_default_threads()`. */
    Expr *call = exprCallNew(c->arena, exprIdent(c->arena, wf->name, block->line), block->line);
    *(Expr **)vecPush(&call->u.call.args) = parIntLit(c, 0, block->line);
    for (size_t i = 0; i < w.captures.len; i++) {
        const char *cn = *(const char **)vecAt(&w.captures, i);
        Sym *sym = lookup(c, cn);
        *(Expr **)vecPush(&call->u.call.args) = parCheckedIdent(c, cn, sym, block->line);
    }
    for (size_t i = 0; i < w.scalars.len; i++) {
        const char *sn = *(const char **)vecAt(&w.scalars, i);
        Sym *sym = lookup(c, sn);
        *(Expr **)vecPush(&call->u.call.args) = parCheckedIdent(c, sn, sym, block->line);
    }
    *(Expr **)vecPush(&call->u.call.args) = parCheckedIdent(c, w.outName, outSym, block->line);
    Expr *span = exprBinNew(c->arena, "-", hiInit, loInit, block->line);
    span->type = c->tI64;
    *(Expr **)vecPush(&call->u.call.args) = span;
    *(Expr **)vecPush(&call->u.call.args) = parIntLit(c, -1, block->line);

    planSetParWorker(call, wf);
    /* Record it as an ordinary callee too: effect summaries are closed over the call graph, and
     * without this edge the enclosing function would end up with "effects could not be fully
     * analyzed" (the synthesised call is skipped by `checkStmt`'s guard and does not go through
     * `checkExpr`'s bookkeeping). */
    planSetCallee(call, wf);
    planSetIsParWorker(wf, true);
    planSetParTlsArena(wf, true);
    planSetUsed(wf, true);
    call->type = c->tI32;

    /* The block is left holding one discarded call; `checkStmt`'s `ST_EXPR` branch sees the plan
     * marker and does not re-check it. */
    block->u.block.stmts.len = 0;
    Stmt *s = stmtNew(c->arena, ST_EXPR, block->line);
    s->u.expr.expr = call;
    *(Stmt **)vecPush(&block->u.block.stmts) = s;
    stmtSetParLoop(block, false);
}

/**
 * @brief Re-check the worker body: reject call shapes that are recognizable only after checking.
 *
 * The scan during desugaring sees **parsed shapes** only: `io::cout << x` is an `EX_BIN` and turns
 * into a method call only in the checker. Once the worker body has been checked like an ordinary
 * function, this walks the analysed shapes again and rejects calls, allocations, and a nested `par`
 * inside the body. It uses the explicit stack as well, to stay out of the walker account.
 *
 * @param c  Checker.
 * @param f  The worker that has just been checked.
 */
void parWorkerAudit(Checker *c, FuncDef *f) {
    if (!f->body) return;
    Vec stmts, exprs;
    vecInit(&stmts, c->arena, sizeof(Stmt *));
    vecInit(&exprs, c->arena, sizeof(Expr *));
    *(Stmt **)vecPush(&stmts) = f->body;
    while (stmts.len > 0) {
        Stmt *s = *(Stmt **)vecAt(&stmts, stmts.len - 1);
        stmts.len--;
        if (!s) continue;
        switch (s->kind) {
        case ST_VAR:    if (s->u.var.init) *(Expr **)vecPush(&exprs) = s->u.var.init; break;
        case ST_ASSIGN:
            *(Expr **)vecPush(&exprs) = s->u.assign.target;
            *(Expr **)vecPush(&exprs) = s->u.assign.value;
            break;
        case ST_EXPR: *(Expr **)vecPush(&exprs) = s->u.expr.expr; break;
        case ST_IF:
            *(Expr **)vecPush(&exprs) = s->u.ifs.cond;
            *(Stmt **)vecPush(&stmts) = s->u.ifs.thenBody;
            *(Stmt **)vecPush(&stmts) = s->u.ifs.elseBody;
            break;
        case ST_WHILE:
            *(Expr **)vecPush(&exprs) = s->u.whiles.cond;
            *(Stmt **)vecPush(&stmts) = s->u.whiles.body;
            break;
        case ST_BLOCK:
            for (size_t i = 0; i < s->u.block.stmts.len; i++)
                *(Stmt **)vecPush(&stmts) = *(Stmt **)vecAt(&s->u.block.stmts, i);
            break;
        case ST_MATCH:
            *(Expr **)vecPush(&exprs) = s->u.match.scrutinee;
            for (size_t i = 0; i < s->u.match.arms.len; i++)
                *(Stmt **)vecPush(&stmts) = ((MatchArm *)vecAt(&s->u.match.arms, i))->body;
            break;
        case ST_TRAP: *(Expr **)vecPush(&exprs) = s->u.trap_.msg; break;
        default: break;
        }
    }
    while (exprs.len > 0) {
        Expr *e = *(Expr **)vecAt(&exprs, exprs.len - 1);
        exprs.len--;
        if (!e) continue;
        switch (e->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: case EX_GENCALL: case EX_EXT:
            ckError(c, e->line, NULL, "cannot call a function inside a par loop");
            continue;
        case EX_NEW:
            ckError(c, e->line, NULL, "cannot allocate inside a par loop");
            continue;
        case EX_SLICE:
            ckError(c, e->line, NULL, "cannot take a slice inside a par loop");
            continue;
        case EX_BIN:
            if (planCallee(e) || planNeedOp(e)) {
                ckError(c, e->line, NULL, "cannot call a function inside a par loop");
                continue;
            }
            *(Expr **)vecPush(&exprs) = e->u.bin.left;
            *(Expr **)vecPush(&exprs) = e->u.bin.right;
            break;
        case EX_UN: case EX_SIGN: case EX_REF: case EX_DEREF:
            *(Expr **)vecPush(&exprs) = e->u.un.operand;
            break;
        case EX_FIELD:
            *(Expr **)vecPush(&exprs) = e->u.field.obj;
            break;
        case EX_INDEX:
            *(Expr **)vecPush(&exprs) = e->u.index.obj;
            *(Expr **)vecPush(&exprs) = e->u.index.index;
            break;
        default: break;
        }
    }
}

/* -------------------------------------------- S1 element-chain fusion (layer 2) */

/* Master fusion switch: `--no-fuse` turns it on (disabling fusion). The checker only reads it. */
static bool gNoFuse = false;

/**
 * @brief Set the "disable fusion" switch (called by the driver for `--no-fuse`).
 *
 * @param off  True disables fusion.
 */
void checkSetNoFuse(bool off) {
    gNoFuse = off;
}

/**
 * @brief Tell whether fusion is switched off.
 *
 * A debug and differential-testing switch: with it off the same source takes the unfused path, and the
 * fusion tests compare the two shapes byte for byte.
 *
 * @return  True when fusion is off.
 */
static bool fuseOff(void) {
    return gNoFuse;
}

/**
 * @brief Tell whether two expressions are structurally equal (the iteration-space criterion of fusion).
 *
 * Only shapes are compared: identical identifier names, integer literals of equal value, binary
 * operators that match with recursively equal operands, fields with the same name and a recursively
 * equal object. Richer shapes (calls and the like) are never equal, so fusion gives up on them.
 *
 * @param a  Left expression.
 * @param b  Right expression.
 * @return   True when structurally equal.
 */
static bool fuseExprSame(Expr *a, Expr *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case EX_IDENT: return strcmp(a->u.ident.name, b->u.ident.name) == 0;
    case EX_INT:   return a->u.ival == b->u.ival;
    case EX_FLOAT: return a->u.fval == b->u.fval;
    case EX_BOOL:  return a->u.bval == b->u.bval;
    case EX_BIN:
        return strcmp(a->u.bin.op, b->u.bin.op) == 0
            && fuseExprSame(a->u.bin.left, b->u.bin.left)
            && fuseExprSame(a->u.bin.right, b->u.bin.right);
    case EX_FIELD:
        return strcmp(a->u.field.name, b->u.field.name) == 0
            && fuseExprSame(a->u.field.obj, b->u.field.obj);
    default: return false;
    }
}

/**
 * @brief Tell whether an expression is "pure": it can be moved into another loop body with unchanged
 *        semantics.
 *
 * Pure means only literals, identifiers, binary and unary operations, conversions, fields, and
 * subscripts (whose own operands must be pure too). Calls, `new`, slices, closures, `trap`, and `?`
 * propagation do not count: they either have side effects or depend on how often they run.
 *
 * @param e  Expression to look at.
 * @return   True when pure.
 */
static bool fusePureExpr(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_IDENT:
        return true;
    case EX_BIN: return fusePureExpr(e->u.bin.left) && fusePureExpr(e->u.bin.right);
    case EX_UN:  return fusePureExpr(e->u.un.operand);
    case EX_CONV: return fusePureExpr(e->u.conv.operand);
    case EX_FIELD: return fusePureExpr(e->u.field.obj);
    case EX_INDEX: return fusePureExpr(e->u.index.obj) && fusePureExpr(e->u.index.index);
    default: return false;
    }
}

/**
 * @brief Tell whether an identifier appears anywhere in an expression (name comparison).
 *
 * @param e     Expression to scan.
 * @param name  Identifier name to find.
 * @return      True when it appears.
 */
static bool fuseMentions(Expr *e, const char *name) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return strcmp(e->u.ident.name, name) == 0;
    case EX_BIN:   return fuseMentions(e->u.bin.left, name) || fuseMentions(e->u.bin.right, name);
    case EX_UN:    return fuseMentions(e->u.un.operand, name);
    case EX_CONV:  return fuseMentions(e->u.conv.operand, name);
    case EX_FIELD: return fuseMentions(e->u.field.obj, name);
    case EX_INDEX: return fuseMentions(e->u.index.obj, name) || fuseMentions(e->u.index.index, name);
    case EX_SLICE: return fuseMentions(e->u.slice.obj, name) || fuseMentions(e->u.slice.lo, name)
                       || fuseMentions(e->u.slice.hi, name);
    case EX_CALL: {
        if (fuseMentions(e->u.call.callee, name)) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (fuseMentions(*(Expr **)vecAt(&e->u.call.args, i), name)) return true;
        return false;
    }
    case EX_METHOD: {
        if (fuseMentions(e->u.method.recv, name)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (fuseMentions(*(Expr **)vecAt(&e->u.method.args, i), name)) return true;
        return false;
    }
    case EX_ASSOC: {
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (fuseMentions(*(Expr **)vecAt(&e->u.assoc.args, i), name)) return true;
        return false;
    }
    case EX_COALESCE: return fuseMentions(e->u.coalesce.main, name)
                           || fuseMentions(e->u.coalesce.fallback, name);
    case EX_REF:   return fuseMentions(e->u.ref.operand, name);
    case EX_DEREF: return fuseMentions(e->u.deref.operand, name);
    case EX_SIGN:  return fuseMentions(e->u.sign.operand, name);
    default: return false;
    }
}

/**
 * @brief Tell whether a block is a loop block and therefore must not be flattened.
 *
 * True for a block flagged `parLoop` or carrying a step statement, and for the block a range or
 * container `for` desugars into (`[loop variable, upper bound, while]`). Flattening one of those would
 * merge the body of an inner loop into the outer loop, which breaks the nesting check of
 * `par r { par c { … } }` and the loop semantics.
 *
 * @param s  Candidate block.
 * @return   True when `s` is a loop block or a desugared loop container.
 */
static bool fuseIsLoopBlock(Stmt *s) {
    if (!s || s->kind != ST_BLOCK) return false;
    if (s->parLoop || s->forStep) return true;
    /* The outer block a range/container `for` desugars into: `[loop variable, upper bound, while]`.
     * The same shape means no further flattening -- otherwise the body of an independent loop would be
     * merged into the outer loop, which is what breaks the nesting check of `par r { par c { … } }`. */
    if (s->u.block.stmts.len == 3) {
        Stmt *a = *(Stmt **)vecAt(&s->u.block.stmts, 0);
        Stmt *b = *(Stmt **)vecAt(&s->u.block.stmts, 1);
        Stmt *c = *(Stmt **)vecAt(&s->u.block.stmts, 2);
        if (a && b && c && a->kind == ST_VAR && b->kind == ST_VAR && c->kind == ST_WHILE)
            return true;
    }
    return false;
}

/**
 * @brief Flatten the wrapper block the parser puts around a loop body: `{ {user stmts…}, step }` ->
 *        `{user stmts…, step}`.
 *
 * Only the shape "this block is exactly `[block, step]` and that block is not a loop" is accepted; loop
 * blocks (the desugared `par` and `for` blocks) are left alone -- flattening one would merge the body
 * of an inner loop into the outer loop, breaking `par`'s nesting check and the loop semantics.
 *
 * @param c     Checker (for the arena).
 * @param body  Loop body block.
 */
static void fuseFlattenBody(Checker *c, Stmt *body) {
    if (!body || body->kind != ST_BLOCK || body->u.block.stmts.len != 2 || !body->forStep)
        return;
    Stmt *first = *(Stmt **)vecAt(&body->u.block.stmts, 0);
    if (first == body->forStep || first->kind != ST_BLOCK || fuseIsLoopBlock(first)) return;
    Vec nv;
    vecInit(&nv, c->arena, sizeof(Stmt *));
    for (size_t k = 0; k < first->u.block.stmts.len; k++)
        *(Stmt **)vecPush(&nv) = *(Stmt **)vecAt(&first->u.block.stmts, k);
    *(Stmt **)vecPush(&nv) = body->forStep;
    stmtBlockSetStmts(body, &nv);
}

/**
 * @brief Split a "range `for` desugaring block" into what fusion needs: the loop variable name, both
 *        ends, the `while`, and the body.
 *
 * The shape comes from `parser.c`'s `forRangeFrom`: the block holds exactly three statements
 * `[var i, var __extc_hi, while]`, the `while` condition is `i < __extc_hi`, and the body is
 * `[original body..., step]` with `body->forStep` pointing at that step statement.
 *
 * @param c         Checker (for the arena when an inner block is flattened).
 * @param blk       Candidate statement.
 * @param whileOut  Receives the `while` statement on a match.
 * @param bodyOut   Receives the loop body block on a match.
 * @param varOut    Receives the loop variable name on a match.
 * @param loOut     Receives the lower-bound expression on a match.
 * @param hiOut     Receives the upper-bound expression on a match.
 * @return          True when the shape matches.
 */
static bool fuseLoopFrom(Checker *c, Stmt *blk, Stmt **whileOut, Stmt **bodyOut, const char **varOut,
                         Expr **loOut, Expr **hiOut) {
    if (!blk || blk->kind != ST_BLOCK || blk->u.block.stmts.len != 3) return false;
    Stmt *decl = *(Stmt **)vecAt(&blk->u.block.stmts, 0);
    Stmt *hi   = *(Stmt **)vecAt(&blk->u.block.stmts, 1);
    Stmt *w    = *(Stmt **)vecAt(&blk->u.block.stmts, 2);
    if (decl->kind != ST_VAR || !decl->u.var.init || decl->u.var.ann) return false;
    if (hi->kind != ST_VAR || !hi->u.var.init || strcmp(hi->u.var.name, "__extc_hi") != 0) return false;
    if (w->kind != ST_WHILE || !w->u.whiles.cond || w->u.whiles.cond->kind != EX_BIN) return false;
    Expr *cond = w->u.whiles.cond;
    if (strcmp(cond->u.bin.op, "<") != 0) return false;
    if (cond->u.bin.left->kind != EX_IDENT || cond->u.bin.right->kind != EX_IDENT) return false;
    if (strcmp(cond->u.bin.left->u.ident.name, decl->u.var.name) != 0) return false;
    if (strcmp(cond->u.bin.right->u.ident.name, "__extc_hi") != 0) return false;
    Stmt *body = w->u.whiles.body;
    if (!body || body->kind != ST_BLOCK || !body->forStep) return false;
    fuseFlattenBody(c, body);
    *whileOut = w; *bodyOut = body; *varOut = decl->u.var.name;
    *loOut = decl->u.var.init; *hiOut = hi->u.var.init;
    return true;
}

/**
 * @brief Add a name to a list of names (duplicates skipped, first-appearance order kept).
 *
 * @param v     `Vec` of `const char *`.
 * @param name  Name to add.
 */
static void fuseAddName(Vec *v, const char *name) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), name) == 0) return;
    *(const char **)vecPush(v) = name;
}

/**
 * @brief All identifier names that appear in an expression.
 *
 * @param e  Expression to scan.
 * @param v  `Vec` of `const char *`; a repeated name is recorded once.
 */
static void fuseCollectIdents(Expr *e, Vec *v) {
    if (!e) return;
    switch (e->kind) {
    case EX_IDENT: fuseAddName(v, e->u.ident.name); break;
    case EX_BIN:   fuseCollectIdents(e->u.bin.left, v); fuseCollectIdents(e->u.bin.right, v); break;
    case EX_UN:    fuseCollectIdents(e->u.un.operand, v); break;
    case EX_CONV:  fuseCollectIdents(e->u.conv.operand, v); break;
    case EX_FIELD: fuseCollectIdents(e->u.field.obj, v); break;
    case EX_INDEX: fuseCollectIdents(e->u.index.obj, v); fuseCollectIdents(e->u.index.index, v); break;
    case EX_SLICE: fuseCollectIdents(e->u.slice.obj, v); fuseCollectIdents(e->u.slice.lo, v);
                   fuseCollectIdents(e->u.slice.hi, v); break;
    case EX_CALL:
        fuseCollectIdents(e->u.call.callee, v);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            fuseCollectIdents(*(Expr **)vecAt(&e->u.call.args, i), v);
        break;
    case EX_METHOD:
        fuseCollectIdents(e->u.method.recv, v);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            fuseCollectIdents(*(Expr **)vecAt(&e->u.method.args, i), v);
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            fuseCollectIdents(*(Expr **)vecAt(&e->u.assoc.args, i), v);
        break;
    case EX_COALESCE: fuseCollectIdents(e->u.coalesce.main, v);
                      fuseCollectIdents(e->u.coalesce.fallback, v); break;
    case EX_REF:   fuseCollectIdents(e->u.ref.operand, v); break;
    case EX_DEREF: fuseCollectIdents(e->u.deref.operand, v); break;
    case EX_SIGN:  fuseCollectIdents(e->u.sign.operand, v); break;
    default: break;
    }
}

/**
 * @brief Tell whether every occurrence of `tmp` in an expression is a read of the form "subscripted by
 *        the loop variable alone".
 *
 * Fusion replaces `tmp[i]` as a whole by A's right-hand side, so any other use (`tmp.len`, `tmp[j]`,
 * passing `tmp` itself somewhere, writing `tmp`) makes fusion give up.
 *
 * @param e    Expression to scan.
 * @param tmp  Temporary view name.
 * @param var  Loop variable name.
 * @return     True when all uses are replaceable reads.
 */
static bool fuseTmpReadsOk(Expr *e, const char *tmp, const char *var) {
    if (!e) return true;
    if (e->kind == EX_IDENT) return strcmp(e->u.ident.name, tmp) != 0;
    if (e->kind == EX_INDEX) {
        Expr *obj = e->u.index.obj, *idx = e->u.index.index;
        if (obj && obj->kind == EX_IDENT && strcmp(obj->u.ident.name, tmp) == 0)
            return idx && idx->kind == EX_IDENT && strcmp(idx->u.ident.name, var) == 0;
        return fuseTmpReadsOk(obj, tmp, var) && fuseTmpReadsOk(idx, tmp, var);
    }
    switch (e->kind) {
    case EX_BIN: return fuseTmpReadsOk(e->u.bin.left, tmp, var)
                     && fuseTmpReadsOk(e->u.bin.right, tmp, var);
    case EX_UN:  return fuseTmpReadsOk(e->u.un.operand, tmp, var);
    case EX_CONV: return fuseTmpReadsOk(e->u.conv.operand, tmp, var);
    case EX_FIELD: return fuseTmpReadsOk(e->u.field.obj, tmp, var);
    case EX_SLICE: return fuseTmpReadsOk(e->u.slice.obj, tmp, var)
                        && fuseTmpReadsOk(e->u.slice.lo, tmp, var)
                        && fuseTmpReadsOk(e->u.slice.hi, tmp, var);
    case EX_CALL:
        if (!fuseTmpReadsOk(e->u.call.callee, tmp, var)) return false;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (!fuseTmpReadsOk(*(Expr **)vecAt(&e->u.call.args, i), tmp, var)) return false;
        return true;
    case EX_METHOD:
        if (!fuseTmpReadsOk(e->u.method.recv, tmp, var)) return false;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (!fuseTmpReadsOk(*(Expr **)vecAt(&e->u.method.args, i), tmp, var)) return false;
        return true;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (!fuseTmpReadsOk(*(Expr **)vecAt(&e->u.assoc.args, i), tmp, var)) return false;
        return true;
    case EX_COALESCE: return fuseTmpReadsOk(e->u.coalesce.main, tmp, var)
                          && fuseTmpReadsOk(e->u.coalesce.fallback, tmp, var);
    case EX_REF:   return fuseTmpReadsOk(e->u.ref.operand, tmp, var);
    case EX_DEREF: return fuseTmpReadsOk(e->u.deref.operand, tmp, var);
    case EX_SIGN:  return fuseTmpReadsOk(e->u.sign.operand, tmp, var);
    default: return true;
    }
}

/**
 * @brief Replace `tmp[i]` in place inside an expression (the second step of fusion).
 *
 * On a hit the node's contents are replaced as a whole by a deep copy of the right-hand side (the whole
 * node is assigned rather than field by field, so the AST field-write guard is not affected).
 *
 * @param c    Checker (for the arena).
 * @param e    Expression to rewrite.
 * @param tmp  Temporary view name.
 * @param var  Loop variable name.
 * @param rhs  Right-hand side to substitute.
 */
static void fuseSubstExpr(Checker *c, Expr *e, const char *tmp, const char *var, Expr *rhs) {
    if (!e) return;
    if (e->kind == EX_INDEX && e->u.index.obj && e->u.index.obj->kind == EX_IDENT
        && strcmp(e->u.index.obj->u.ident.name, tmp) == 0
        && e->u.index.index && e->u.index.index->kind == EX_IDENT
        && strcmp(e->u.index.index->u.ident.name, var) == 0) {
        *e = *exprClone(c->arena, rhs);
        return;
    }
    switch (e->kind) {
    case EX_BIN: fuseSubstExpr(c, e->u.bin.left, tmp, var, rhs);
                 fuseSubstExpr(c, e->u.bin.right, tmp, var, rhs); break;
    case EX_UN:  fuseSubstExpr(c, e->u.un.operand, tmp, var, rhs); break;
    case EX_CONV: fuseSubstExpr(c, e->u.conv.operand, tmp, var, rhs); break;
    case EX_FIELD: fuseSubstExpr(c, e->u.field.obj, tmp, var, rhs); break;
    case EX_INDEX: fuseSubstExpr(c, e->u.index.obj, tmp, var, rhs);
                   fuseSubstExpr(c, e->u.index.index, tmp, var, rhs); break;
    case EX_SLICE: fuseSubstExpr(c, e->u.slice.obj, tmp, var, rhs);
                   fuseSubstExpr(c, e->u.slice.lo, tmp, var, rhs);
                   fuseSubstExpr(c, e->u.slice.hi, tmp, var, rhs); break;
    case EX_CALL:
        fuseSubstExpr(c, e->u.call.callee, tmp, var, rhs);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            fuseSubstExpr(c, *(Expr **)vecAt(&e->u.call.args, i), tmp, var, rhs);
        break;
    case EX_METHOD:
        fuseSubstExpr(c, e->u.method.recv, tmp, var, rhs);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            fuseSubstExpr(c, *(Expr **)vecAt(&e->u.method.args, i), tmp, var, rhs);
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            fuseSubstExpr(c, *(Expr **)vecAt(&e->u.assoc.args, i), tmp, var, rhs);
        break;
    case EX_COALESCE: fuseSubstExpr(c, e->u.coalesce.main, tmp, var, rhs);
                      fuseSubstExpr(c, e->u.coalesce.fallback, tmp, var, rhs); break;
    case EX_REF:   fuseSubstExpr(c, e->u.ref.operand, tmp, var, rhs); break;
    case EX_DEREF: fuseSubstExpr(c, e->u.deref.operand, tmp, var, rhs); break;
    case EX_SIGN:  fuseSubstExpr(c, e->u.sign.operand, tmp, var, rhs); break;
    default: break;
    }
}

/**
 * @brief Replace `tmp[i]` in place at every writable position of a statement.
 *
 * Only the statement shapes fusion accepts are handled (assignments and expression statements); other
 * shapes were already rejected by the eligibility check.
 *
 * @param c    Checker.
 * @param s    Statement to rewrite.
 * @param tmp  Temporary view name.
 * @param var  Loop variable name.
 * @param rhs  Right-hand side to substitute.
 */
static void fuseSubstStmt(Checker *c, Stmt *s, const char *tmp, const char *var, Expr *rhs) {
    if (!s) return;
    switch (s->kind) {
    case ST_ASSIGN:
        fuseSubstExpr(c, s->u.assign.target, tmp, var, rhs);
        fuseSubstExpr(c, s->u.assign.value, tmp, var, rhs);
        break;
    case ST_EXPR:
        fuseSubstExpr(c, s->u.expr.expr, tmp, var, rhs);
        break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *child = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (child == s->forStep) continue;
            fuseSubstStmt(c, child, tmp, var, rhs);
        }
        break;
    default: break;
    }
}

/**
 * @brief Tell whether B's body has the fusable shape "each element writes only its own slot", and
 *        collect the names it reads and writes.
 *
 * Rule (the conservative v1): each statement is either `X[i] = <expression>` or an expression
 * statement; a write target must be a view slot subscripted by the loop variable alone, and the name of
 * a write target must not appear at any read position (otherwise fusion changes the order of
 * cross-element dependences).
 *
 * @param body    B's loop body (`ST_BLOCK`, including the step statement).
 * @param tmp     Temporary view name.
 * @param var     Loop variable name.
 * @param writes  `Vec` of `const char *` collecting written view names.
 * @param reads   `Vec` of `const char *` collecting read names.
 * @return        True when the shape fits.
 */
static bool fuseCheckBody(Stmt *body, const char *tmp, const char *var, Vec *writes, Vec *reads) {
    for (size_t i = 0; i < body->u.block.stmts.len; i++) {
        Stmt *s = *(Stmt **)vecAt(&body->u.block.stmts, i);
        if (s == body->forStep) continue;
        if (s->kind == ST_ASSIGN && !s->u.assign.op) {
            Expr *t = s->u.assign.target;
            if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                || strcmp(t->u.index.index->u.ident.name, var) != 0)
                return false;
            const char *w = t->u.index.obj->u.ident.name;
            if (strcmp(w, tmp) == 0) return false;
            fuseAddName(writes, w);
            if (!fuseTmpReadsOk(s->u.assign.value, tmp, var)) return false;
            fuseCollectIdents(s->u.assign.value, reads);
        } else if (s->kind == ST_EXPR && s->u.expr.expr) {
            if (!fuseTmpReadsOk(s->u.expr.expr, tmp, var)) return false;
            fuseCollectIdents(s->u.expr.expr, reads);
        } else {
            return false;
        }
    }
    return true;
}

/**
 * @brief Generic walk callback: whether a name appears in a statement (the lower half of
 *        `fuseStmtMentions`).
 *
 * @param raw  Points at a `FuseMentionCtx`.
 * @param s    Statement to scan.
 * @return     Always false (keep walking, do not prune).
 */
static bool fuseMentionOnStmt(void *raw, Stmt *s);

/** Walk context of `fuseStmtMentions`: the name to find and whether it has been found. */
typedef struct { const char *name; bool hit; } FuseMentionCtx;

/**
 * @brief Expression callback of `fuseStmtMentions`: a name hit stops the whole walk.
 *
 * @param raw  Context (a `FuseMentionCtx *`).
 * @param e    Subexpression being visited.
 * @return     True when the name has not been found (keep walking).
 */
static bool fuseMentionOnExpr(void *raw, Expr *e) {
    FuseMentionCtx *m = raw;
    if (!e) return true;
    if (e->kind == EX_IDENT && strcmp(e->u.ident.name, m->name) == 0) {
        m->hit = true;
        return false;
    }
    AstVisit v = { fuseMentionOnExpr, fuseMentionOnStmt, m };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Statement callback of `fuseStmtMentions`: keep walking into child statements.
 *
 * @param raw  Context (a `FuseMentionCtx *`).
 * @param s    Child statement being visited.
 * @return     True when the name has not been found (keep walking).
 */
static bool fuseMentionOnStmt(void *raw, Stmt *s) {
    FuseMentionCtx *m = raw;
    if (!s) return true;
    AstVisit v = { fuseMentionOnExpr, fuseMentionOnStmt, m };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Tell whether a statement (and all its child statements and their expressions) mentions an
 *        identifier.
 *
 * Walking goes through the AST's generic traversal (`astWalkStmtChildren`), so statement and expression
 * kinds added later are covered automatically and no second switch is needed here.
 *
 * @param s     Statement to scan.
 * @param name  Identifier name to find.
 * @return      True when mentioned.
 */
static bool fuseStmtMentions(Stmt *s, const char *name) {
    if (!s) return false;
    FuseMentionCtx m = { name, false };
    AstVisit v = { fuseMentionOnExpr, fuseMentionOnStmt, &m };
    astWalkStmtChildren(s, &v);
    return m.hit;
}

/**
 * @brief Recognize the single statement `acc = acc + <pure expression>` in the S2 reduction body.
 *
 * Only "ascending, single accumulator, `+`, a pure right-hand side that really reads tmp" is accepted:
 * after fusion replaces `tmp[i]` by the writing loop's right-hand side, the accumulation order is still
 * the same increasing sequence of additions, bit for bit. `*` is not accepted (floating-point
 * multiplication is not bit-equivalent under this reordering, and integers would only add a second
 * flavour), and neither is a form whose right-hand side mentions `acc` itself.
 *
 * @param s    One statement of the reduction loop body.
 * @param tmp  Temporary view name.
 * @param var  Loop variable name.
 * @param rhs  Output: the pure expression right of `acc +`.
 * @return     True when the shape fits.
 */
static bool fuseReduceStmt(Stmt *s, const char *tmp, const char *var, Expr **rhs) {
    if (!s || s->kind != ST_ASSIGN || s->u.assign.op) return false;
    Expr *t = s->u.assign.target, *v = s->u.assign.value;
    if (!t || t->kind != EX_IDENT) return false;
    if (strcmp(t->u.ident.name, tmp) == 0 || strcmp(t->u.ident.name, var) == 0) return false;
    if (!v || v->kind != EX_BIN || !v->u.bin.op || strcmp(v->u.bin.op, "+") != 0) return false;
    Expr *l = v->u.bin.left, *r = v->u.bin.right;
    if (!l || l->kind != EX_IDENT || strcmp(l->u.ident.name, t->u.ident.name) != 0) return false;
    if (!r || !fusePureExpr(r) || !fuseTmpReadsOk(r, tmp, var)) return false;
    if (fuseMentions(r, t->u.ident.name)) return false;
    if (!fuseMentions(r, tmp)) return false;
    *rhs = r;
    return true;
}

/**
 * @brief Try the S2 reduction fusion: `var tmp = new …` + loop A writing it + `var acc = init` + an
 *        ascending reduction loop.
 *
 * Shape (all of it must match): `var tmp: mut slice<T> = new …` at `i`; A at `i+1`; `var acc = init` at
 * `i+2` (`init` is not `new`, so acc is a scalar); the reduction loop at `i+3`, with the same loop
 * variable name as A and structurally equal ends; A's body is exactly `tmp[i] = <pure rhs>`; the
 * reduction body is exactly one statement `acc = acc + <pure expression>` (`tmp` read only as `tmp[i]`);
 * `tmp` is not mentioned anywhere else in this block; A's right-hand side does not read `acc`
 * (otherwise fusion would interleave A with the accumulation). On a hit: the accumulation statement
 * moves into A's body, `tmp[i]` is replaced by A's right-hand side, the `tmp` declaration, the
 * reduction loop, and A are deleted in place, and the `acc` declaration moves before the fused loop.
 *
 * @param c      Checker.
 * @param block  Block being checked.
 * @param i      Index in the block of the temporary-view declaration.
 * @return       True when fused (the statement table is rewritten in place).
 */
static bool fuseTryReduce(Checker *c, Stmt *block, size_t i) {
    Vec *v = &block->u.block.stmts;
    Stmt *decl = *(Stmt **)vecAt(v, i);
    if (!decl || decl->kind != ST_VAR || !decl->u.var.ann || !decl->u.var.init) return false;
    if (decl->u.var.init->kind != EX_NEW) return false;
    const char *tmp = decl->u.var.name;
    size_t n = v->len;
    if (i + 3 >= n) return false;
    Stmt *a = *(Stmt **)vecAt(v, i + 1);
    Stmt *aW, *aBody, *rW, *rBody;
    const char *aVar, *rVar;
    Expr *aLo, *aHi, *rLo, *rHi;
    if (!fuseLoopFrom(c, a, &aW, &aBody, &aVar, &aLo, &aHi)) return false;
    if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) return false;
    Stmt *accDecl = *(Stmt **)vecAt(v, i + 2);
    if (!accDecl || accDecl->kind != ST_VAR || !accDecl->u.var.init) return false;
    if (accDecl->u.var.init->kind == EX_NEW) return false;
    const char *acc = accDecl->u.var.name;
    if (strcmp(acc, tmp) == 0 || strcmp(acc, aVar) == 0) return false;
    if (fuseStmtMentions(accDecl, tmp)) return false;
    if (!fuseLoopFrom(c, *(Stmt **)vecAt(v, i + 3), &rW, &rBody, &rVar, &rLo, &rHi)) return false;
    (void)rW;
    if (strcmp(aVar, rVar) != 0) return false;
    if (!fuseExprSame(aLo, rLo) || !fuseExprSame(aHi, rHi)) return false;
    if (fuseMentions(rLo, tmp) || fuseMentions(rHi, tmp)) return false;
    /* A's body: exactly [tmp[i] = rhs, step]. */
    Expr *rhs = NULL;
    {
        size_t seen = 0;
        for (size_t k = 0; k < aBody->u.block.stmts.len; k++) {
            Stmt *s = *(Stmt **)vecAt(&aBody->u.block.stmts, k);
            if (s == aBody->forStep) continue;
            if (++seen > 1) break;
            if (s->kind != ST_ASSIGN || s->u.assign.op) break;
            Expr *t = s->u.assign.target;
            if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                || strcmp(t->u.index.obj->u.ident.name, tmp) != 0
                || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                || strcmp(t->u.index.index->u.ident.name, aVar) != 0) break;
            if (!fusePureExpr(s->u.assign.value)) break;
            if (fuseMentions(s->u.assign.value, tmp)) break;
            rhs = s->u.assign.value;
        }
        if (!rhs || seen != 1) return false;
    }
    if (fuseMentions(rhs, acc)) return false;
    /* Reduction body: exactly one statement. */
    Stmt *red = NULL;
    {
        size_t seen = 0;
        for (size_t k = 0; k < rBody->u.block.stmts.len; k++) {
            Stmt *s = *(Stmt **)vecAt(&rBody->u.block.stmts, k);
            if (s == rBody->forStep) continue;
            if (++seen > 1) return false;
            red = s;
        }
        if (!red) return false;
    }
    Expr *redRhs = NULL;
    if (!fuseReduceStmt(red, tmp, aVar, &redRhs)) return false;
    /* `tmp` has no other use in the remaining statements of this block. */
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 3) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    /* Rewrite: the accumulation statement moves into A's body, tmp[i] becomes A's right-hand side,
     * and the acc declaration moves in front of the fused loop. */
    fuseSubstStmt(c, red, tmp, aVar, rhs);
    Vec fused;
    vecInit(&fused, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&fused) = red;
    *(Stmt **)vecPush(&fused) = aBody->forStep;
    stmtBlockSetStmts(aBody, &fused);
    size_t w = i;
    *(Stmt **)vecAt(v, w++) = accDecl;
    *(Stmt **)vecAt(v, w++) = a;
    for (size_t k = i + 4; k < n; k++)
        *(Stmt **)vecAt(v, w++) = *(Stmt **)vecAt(v, k);
    v->len = w;
    return true;
}

/**
 * @brief Rewrite a container-form reduction loop into index form, so that `fuseTryReduce` can fold
 *        it.
 *
 * `for v in tmp { acc = acc + v }` visits `tmp.len` elements, while the preceding element-chain `A`
 * visits `[0, hi)`. The two describe the same iteration space only when `A`'s upper bound and the
 * length used at `tmp`'s allocation are the same expression, so `fuseExprSame(aHi, the length of
 * new T[len])` is required first; once it holds, the container loop is replaced with
 * `for <aVar> in <aHi> { acc = acc + tmp[<aVar>] }`, and the next scan pass lets `fuseTryReduce`
 * fold that into `A`'s body.
 *
 * Only the simplest shape is accepted: the element value takes part in the accumulation as `v` itself
 * (`acc = acc + v`), the body has no other statements, and `tmp` does not occur in any other
 * statement of the block.
 *
 * @param c      Checker.
 * @param block  Statement block.
 * @param i      Position of `var tmp = new T[n]` in the block.
 * @return       True when one rewrite happened; false when the shape does not match (the AST is left
 *               untouched).
 */
static bool fuseTryReduceContainer(Checker *c, Stmt *block, size_t i) {
    Vec *v = &block->u.block.stmts;
    Stmt *decl = *(Stmt **)vecAt(v, i);
    if (!decl || decl->kind != ST_VAR || !decl->u.var.ann || !decl->u.var.init) return false;
    if (decl->u.var.init->kind != EX_NEW) return false;
    Expr *lenExpr = decl->u.var.init->u.new_.count;
    if (!lenExpr) return false;
    const char *tmp = decl->u.var.name;
    size_t n = v->len;
    if (i + 3 >= n) return false;
    Stmt *a = *(Stmt **)vecAt(v, i + 1);
    Stmt *aW, *aBody;
    const char *aVar;
    Expr *aLo, *aHi;
    if (!fuseLoopFrom(c, a, &aW, &aBody, &aVar, &aLo, &aHi)) return false;
    (void)aW;
    if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) return false;
    if (!fuseExprSame(aHi, lenExpr)) return false;
    Stmt *accDecl = *(Stmt **)vecAt(v, i + 2);
    if (!accDecl || accDecl->kind != ST_VAR || !accDecl->u.var.init) return false;
    if (accDecl->u.var.init->kind == EX_NEW) return false;
    const char *acc = accDecl->u.var.name;
    if (strcmp(acc, tmp) == 0 || strcmp(acc, aVar) == 0) return false;
    if (fuseStmtMentions(accDecl, tmp)) return false;
    /* A's body: exactly `tmp[aVar] = <pure rhs>`. */
    Expr *rhs = NULL;
    {
        size_t seen = 0;
        for (size_t k = 0; k < aBody->u.block.stmts.len; k++) {
            Stmt *s = *(Stmt **)vecAt(&aBody->u.block.stmts, k);
            if (s == aBody->forStep) continue;
            if (++seen > 1) break;
            if (s->kind != ST_ASSIGN || s->u.assign.op) break;
            Expr *t = s->u.assign.target;
            if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                || strcmp(t->u.index.obj->u.ident.name, tmp) != 0
                || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                || strcmp(t->u.index.index->u.ident.name, aVar) != 0) break;
            if (!fusePureExpr(s->u.assign.value)) break;
            if (fuseMentions(s->u.assign.value, tmp)) break;
            rhs = s->u.assign.value;
        }
        if (!rhs || seen != 1) return false;
    }
    if (fuseMentions(rhs, acc)) return false;
    /* The container-form reduction loop: `for v in tmp { acc = acc + v }`. */
    Stmt *cf = *(Stmt **)vecAt(v, i + 3);
    if (!cf || cf->kind != ST_BLOCK || !cf->forDesugar) return false;
    Vec *cs = &cf->u.block.stmts;
    if (cs->len != 3) return false;
    Stmt *viewDecl = *(Stmt **)vecAt(cs, 0);
    Stmt *idxDecl  = *(Stmt **)vecAt(cs, 1);
    Stmt *cw       = *(Stmt **)vecAt(cs, 2);
    if (!viewDecl || viewDecl->kind != ST_VAR || strcmp(viewDecl->u.var.name, "__extc_s") != 0) return false;
    Expr *viewInit = viewDecl->u.var.init;
    if (!viewInit || viewInit->kind != EX_SLICE || viewInit->u.slice.lo || viewInit->u.slice.hi) return false;
    if (!viewInit->u.slice.obj || viewInit->u.slice.obj->kind != EX_IDENT
        || strcmp(viewInit->u.slice.obj->u.ident.name, tmp) != 0) return false;
    if (!idxDecl || idxDecl->kind != ST_VAR || strcmp(idxDecl->u.var.name, "__extc_i") != 0) return false;
    if (!cw || cw->kind != ST_WHILE || !cw->u.whiles.body || cw->u.whiles.body->kind != ST_BLOCK) return false;
    Stmt *loopBody = cw->u.whiles.body;
    if (loopBody->u.block.stmts.len != 3) return false;
    Stmt *elemDecl = *(Stmt **)vecAt(&loopBody->u.block.stmts, 0);
    Stmt *userBody = *(Stmt **)vecAt(&loopBody->u.block.stmts, 1);
    if (!elemDecl || elemDecl->kind != ST_VAR) return false;
    Expr *elemInit = elemDecl->u.var.init;
    if (!elemInit || elemInit->kind != EX_INDEX) return false;
    if (!elemInit->u.index.obj || elemInit->u.index.obj->kind != EX_IDENT
        || strcmp(elemInit->u.index.obj->u.ident.name, "__extc_s") != 0) return false;
    if (!elemInit->u.index.index || elemInit->u.index.index->kind != EX_IDENT
        || strcmp(elemInit->u.index.index->u.ident.name, "__extc_i") != 0) return false;
    const char *elem = elemDecl->u.var.name;
    if (!userBody || userBody->kind != ST_BLOCK || userBody->u.block.stmts.len != 1) return false;
    Stmt *red = *(Stmt **)vecAt(&userBody->u.block.stmts, 0);
    if (!red || red->kind != ST_ASSIGN || red->u.assign.op) return false;
    Expr *target = red->u.assign.target;
    Expr *value  = red->u.assign.value;
    if (!target || target->kind != EX_IDENT || strcmp(target->u.ident.name, acc) != 0) return false;
    if (!value || value->kind != EX_BIN || !value->u.bin.op || strcmp(value->u.bin.op, "+") != 0) return false;
    if (!value->u.bin.left || value->u.bin.left->kind != EX_IDENT
        || strcmp(value->u.bin.left->u.ident.name, acc) != 0) return false;
    if (!value->u.bin.right || value->u.bin.right->kind != EX_IDENT
        || strcmp(value->u.bin.right->u.ident.name, elem) != 0) return false;
    /* `tmp` has no other use in the block's other statements. */
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 3) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    /* Replace it with the index-form reduction loop, which has the same iteration space as A. */
    int line = cf->line;
    Expr *sum = exprBinNew(c->arena, "+", exprIdent(c->arena, acc, line),
                           exprIndexNew(c->arena, exprIdent(c->arena, tmp, line),
                                        exprIdent(c->arena, aVar, line), line), line);
    Vec bodyStmts;
    vecInit(&bodyStmts, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&bodyStmts) =
        stmtAssignNew(c->arena, exprIdent(c->arena, acc, line), sum, NULL, line);
    Stmt *innerBlock = stmtNew(c->arena, ST_BLOCK, line);
    stmtBlockSetStmts(innerBlock, &bodyStmts);
    *(Stmt **)vecAt(v, i + 3) =
        stmtRangeForNew(c->arena, aVar, exprClone(c->arena, aHi), innerBlock, line);
    return true;
}

/**
 * @brief Fold `let m = seq::max(tmp) ?? fb` / `seq::min` into the loop that writes the temporary
 *        view.
 *
 * Shape: `var tmp = new T[len]` + write loop A + `let m[: T] = seq::max/min(tmp) ?? <fallback>`. After
 * folding there is no temporary view and no second reading pass: A's body gains one flag and one
 * comparison, and an empty input still yields the user-written fallback (not `T`'s extremum), so the
 * semantics are bit for bit those of the library function.
 *
 * The only difference from the `seq::sum` version is that the fallback may be any pure expression --
 * an empty input uses the fallback, a non-empty one uses the extremum, and the two paths do not
 * interact; `sum` folds "zero + ...", so its fallback has to be zero as well.
 *
 * @param c      Checker.
 * @param block  Current block.
 * @param i      Index of the candidate `var tmp = new T[len]` statement.
 * @return       True when something was folded (the caller rescans the block).
 */
static bool fuseTryReduceExtremumCall(Checker *c, Stmt *block, size_t i) {
    Vec *v = &block->u.block.stmts;
    Stmt *decl = *(Stmt **)vecAt(v, i);
    if (!decl || decl->kind != ST_VAR || !decl->u.var.ann || !decl->u.var.init) return false;
    if (decl->u.var.init->kind != EX_NEW) return false;
    Expr *lenExpr = decl->u.var.init->u.new_.count;
    Type *elemNode = decl->u.var.init->u.new_.type;
    if (!lenExpr || !elemNode || !elemNode->name) return false;
    Type *elem = elemNode->kind == TY_BUILTIN ? elemNode : ttFromName(c->tt, elemNode->name);
    if (!elem || !elem->name) return false;
    if (elem->name[0] != 'i' && elem->name[0] != 'u' && elem->name[0] != 'f') return false;
    const char *tmp = decl->u.var.name;
    size_t n = v->len;
    if (i + 2 >= n) return false;
    Stmt *aW, *aBody;
    const char *aVar;
    Expr *aLo, *aHi;
    if (!fuseLoopFrom(c, *(Stmt **)vecAt(v, i + 1), &aW, &aBody, &aVar, &aLo, &aHi)) return false;
    (void)aW;
    if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) return false;
    if (!fuseExprSame(aHi, lenExpr)) return false;
    Expr *rhs = NULL;
    {
        size_t seen = 0;
        for (size_t k = 0; k < aBody->u.block.stmts.len; k++) {
            Stmt *st = *(Stmt **)vecAt(&aBody->u.block.stmts, k);
            if (st == aBody->forStep) continue;
            if (++seen > 1) break;
            if (st->kind != ST_ASSIGN || st->u.assign.op) break;
            Expr *t = st->u.assign.target;
            if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                || strcmp(t->u.index.obj->u.ident.name, tmp) != 0
                || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                || strcmp(t->u.index.index->u.ident.name, aVar) != 0) break;
            if (!fusePureExpr(st->u.assign.value)) break;
            if (fuseMentions(st->u.assign.value, tmp)) break;
            rhs = st->u.assign.value;
        }
        if (!rhs || seen != 1) return false;
    }
    Stmt *bind = *(Stmt **)vecAt(v, i + 2);
    if (!bind || bind->kind != ST_VAR || !bind->u.var.init) return false;
    if (bind->u.var.ann) {
        Type *ann = bind->u.var.ann;
        if (!ann->name || strcmp(ann->name, elem->name) != 0) return false;
    }
    Expr *co = bind->u.var.init;
    if (co->kind != EX_COALESCE || !co->u.coalesce.main || !co->u.coalesce.fallback) return false;
    Expr *opt = co->u.coalesce.main, *fb = co->u.coalesce.fallback;
    const char *fn = NULL;
    Expr *arg = NULL;
    if (opt->kind == EX_ASSOC) {
        const char *p = opt->u.assoc.modPrefix, *nm = opt->u.assoc.name;
        if (!p || !nm || opt->u.assoc.args.len != 1) return false;
        size_t pl = strlen(p);
        if (pl < 3 || strcmp(p + pl - 3, "seq") != 0) return false;
        fn = nm;
        arg = *(Expr **)vecAt(&opt->u.assoc.args, 0);
    } else if (opt->kind == EX_CALL && opt->u.call.callee && opt->u.call.callee->kind == EX_IDENT
               && opt->u.call.args.len == 1) {
        const char *nm = opt->u.call.callee->u.ident.name;
        const char *dollar = nm ? strrchr(nm, '$') : NULL;
        if (!dollar) return false;
        size_t ml = (size_t)(dollar - nm);
        if (ml < 3 || strncmp(nm + ml - 3, "seq", 3) != 0) return false;
        fn = dollar + 1;
        arg = *(Expr **)vecAt(&opt->u.call.args, 0);
    } else {
        return false;
    }
    bool isMax;
    if (strcmp(fn, "max") == 0) isMax = true;
    else if (strcmp(fn, "min") == 0) isMax = false;
    else return false;
    if (!arg || arg->kind != EX_IDENT || strcmp(arg->u.ident.name, tmp) != 0) return false;
    if (!fusePureExpr(fb) || fuseMentions(fb, tmp)) return false;
    const char *mname = bind->u.var.name;
    if (!mname || strcmp(mname, tmp) == 0 || strcmp(mname, aVar) == 0) return false;
    if (fuseMentions(rhs, mname)) return false;
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 2) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    int line = bind->line;
    char buf[64];
    snprintf(buf, sizeof buf, "__extc_found%zu", c->m->funcs.len + n);
    const char *found = arenaStrndup(c->arena, buf, strlen(buf));
    snprintf(buf, sizeof buf, "__extc_acc%zu", c->m->funcs.len + n);
    const char *acc = arenaStrndup(c->arena, buf, strlen(buf));
    snprintf(buf, sizeof buf, "__extc_val%zu", c->m->funcs.len + n);
    const char *val = arenaStrndup(c->arena, buf, strlen(buf));
    Stmt *foundDecl = stmtNew(c->arena, ST_VAR, line);
    foundDecl->u.var.name = found;
    foundDecl->u.var.ann  = c->tBool;
    foundDecl->u.var.init = exprBoolNew(c->arena, false, line);
    foundDecl->u.var.mut  = true;
    Stmt *accDecl = stmtNew(c->arena, ST_VAR, line);
    accDecl->u.var.name = acc;
    accDecl->u.var.ann  = elem;
    accDecl->u.var.init = elem->name[0] == 'f' ? exprFloatNew(c->arena, 0.0, line)
                                                : exprIntNew(c->arena, 0, line);
    accDecl->u.var.mut  = true;
    Expr *isFirst = exprBinNew(c->arena, "==", exprIdent(c->arena, found, line),
                               exprBoolNew(c->arena, false, line), line);
    Expr *better = exprBinNew(c->arena, isMax ? ">" : "<", rhs,
                              exprIdent(c->arena, acc, line), line);
    Expr *cond = exprBinNew(c->arena, "||", isFirst, better, line);
    Vec thenStmts;
    vecInit(&thenStmts, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&thenStmts) =
        stmtAssignNew(c->arena, exprIdent(c->arena, acc, line), exprClone(c->arena, rhs), NULL, line);
    *(Stmt **)vecPush(&thenStmts) =
        stmtAssignNew(c->arena, exprIdent(c->arena, found, line), exprBoolNew(c->arena, true, line),
                      NULL, line);
    Stmt *thenBlk = stmtNew(c->arena, ST_BLOCK, line);
    stmtBlockSetStmts(thenBlk, &thenStmts);
    Stmt *takeStmt = stmtIfNew(c->arena, cond, thenBlk, NULL, line);
    *(Stmt **)vecAt(&aBody->u.block.stmts, 0) = takeStmt;
    /* `var __extc_val: T = <fallback>` -- the user's `let` must not be rewritten into a writable
     * binding either, so the result lands in a temporary scalar of our own and the binding statement
     * then reads it. */
    Stmt *valDecl = stmtNew(c->arena, ST_VAR, line);
    valDecl->u.var.name = val;
    valDecl->u.var.ann  = elem;
    valDecl->u.var.init = exprClone(c->arena, fb);
    valDecl->u.var.mut  = true;
    bind->u.var.ann  = elem;
    bind->u.var.init = exprIdent(c->arena, val, line);
    Vec setStmts;
    vecInit(&setStmts, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&setStmts) =
        stmtAssignNew(c->arena, exprIdent(c->arena, val, line),
                      exprIdent(c->arena, acc, line), NULL, line);
    Stmt *setBlk = stmtNew(c->arena, ST_BLOCK, line);
    stmtBlockSetStmts(setBlk, &setStmts);
    Stmt *setM = stmtIfNew(c->arena, exprIdent(c->arena, found, line), setBlk, NULL, line);
    Vec nv;
    vecInit(&nv, c->arena, sizeof(Stmt *));
    for (size_t k = 0; k < i; k++) *(Stmt **)vecPush(&nv) = *(Stmt **)vecAt(v, k);
    *(Stmt **)vecPush(&nv) = foundDecl;
    *(Stmt **)vecPush(&nv) = accDecl;
    *(Stmt **)vecPush(&nv) = valDecl;
    *(Stmt **)vecPush(&nv) = *(Stmt **)vecAt(v, i + 1);
    *(Stmt **)vecPush(&nv) = setM;
    *(Stmt **)vecPush(&nv) = bind;
    for (size_t k = i + 3; k < n; k++) *(Stmt **)vecPush(&nv) = *(Stmt **)vecAt(v, k);
    stmtBlockSetStmts(block, &nv);
    return true;
}

/**
 * @brief Recognize `var s = seq::sum(tmp) ?? 0` (`tmp` being the output of the write loop just
 *        above).
 *
 * Shape: `tmp`'s declaration + write loop A + binding `s` (no annotation, initialiser
 * `seq::sum(tmp) ?? <zero literal>`). On a hit the binding is rewritten into the three statements S2
 * understands -- `var acc: T = T(0)`, the index-form reduction loop, `var s = acc` -- and the next
 * fixed-point scan folds the reduction into A (`tmp` then disappears).
 *
 * Why `?? <zero literal>` is required: after folding, an empty input gives "zero", which matches
 * `seq::sum`'s `none ?? fallback` on empty input bit for bit only when the fallback is zero too. A
 * non-zero fallback (`?? -1`) is therefore not folded.
 *
 * @param c      Checker.
 * @param block  The block statement.
 * @param i      Position of the temporary view's declaration.
 * @return       True when the rewrite succeeded.
 */
static bool fuseTryReduceSumCall(Checker *c, Stmt *block, size_t i) {
    Vec *v = &block->u.block.stmts;
    Stmt *decl = *(Stmt **)vecAt(v, i);
    if (!decl || decl->kind != ST_VAR || !decl->u.var.ann || !decl->u.var.init) return false;
    if (decl->u.var.init->kind != EX_NEW) return false;
    Expr *lenExpr = decl->u.var.init->u.new_.count;
    /* The parser writes `new f64[n]`'s type as a bare name (`TY_UNRESOLVED`), so builtin scalars are
     * resolved by name here; when that fails (a user struct) nothing is folded -- the shape of the
     * zero value cannot be guessed. */
    Type *elemNode = decl->u.var.init->u.new_.type;
    if (!lenExpr || !elemNode || !elemNode->name) return false;
    Type *elem = elemNode->kind == TY_BUILTIN ? elemNode : ttFromName(c->tt, elemNode->name);
    if (!elem || !elem->name) return false;
    if (elem->name[0] != 'i' && elem->name[0] != 'u' && elem->name[0] != 'f') return false;
    const char *tmp = decl->u.var.name;
    size_t n = v->len;
    if (i + 2 >= n) return false;
    /* A: the write loop, whose iteration space equals `new T[len]`. */
    Stmt *aW, *aBody;
    const char *aVar;
    Expr *aLo, *aHi;
    if (!fuseLoopFrom(c, *(Stmt **)vecAt(v, i + 1), &aW, &aBody, &aVar, &aLo, &aHi)) return false;
    (void)aW;
    if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) return false;
    if (!fuseExprSame(aHi, lenExpr)) return false;
    /* A's body: exactly `tmp[aVar] = <pure rhs>`. */
    Expr *rhs = NULL;
    {
        size_t seen = 0;
        for (size_t k = 0; k < aBody->u.block.stmts.len; k++) {
            Stmt *st = *(Stmt **)vecAt(&aBody->u.block.stmts, k);
            if (st == aBody->forStep) continue;
            if (++seen > 1) break;
            if (st->kind != ST_ASSIGN || st->u.assign.op) break;
            Expr *t = st->u.assign.target;
            if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                || strcmp(t->u.index.obj->u.ident.name, tmp) != 0
                || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                || strcmp(t->u.index.index->u.ident.name, aVar) != 0) break;
            if (!fusePureExpr(st->u.assign.value)) break;
            if (fuseMentions(st->u.assign.value, tmp)) break;
            rhs = st->u.assign.value;
        }
        if (!rhs || seen != 1) return false;
    }
    /* The binding: `var s = seq::sum(tmp) ?? <zero literal>`, unannotated (its type comes from the
     * element type). */
    Stmt *bind = *(Stmt **)vecAt(v, i + 2);
    if (!bind || bind->kind != ST_VAR || !bind->u.var.init) return false;
    /* The binding may be annotated, but the name must then agree with the element type (`f64`
     * elements with `let s: f64`): the accumulator keeps the element type and that is also
     * `seq::sum`'s result type, so a wider annotation would be handled by implicit widening, but
     * only the "same name" case is accepted here so as not to guess the meaning of a user's
     * conversion. */
    if (bind->u.var.ann) {
        Type *ann = bind->u.var.ann;
        if (!ann->name || strcmp(ann->name, elem->name) != 0) return false;
    }
    Expr *co = bind->u.var.init;
    if (co->kind != EX_COALESCE || !co->u.coalesce.main || !co->u.coalesce.fallback) return false;
    Expr *opt = co->u.coalesce.main, *fb = co->u.coalesce.fallback;
    /* `seq::sum(tmp)` has already been rewritten by the qualified-name pass into a flat call: the
     * callee of an `EX_CALL` is `EX_IDENT "seq$sum"` (the `EX_ASSOC` form is accepted too; both
     * entry points occur). Only "module name ending in `seq`, function name `sum`, exactly one
     * argument" is accepted. */
    Expr *sumArg = NULL;
    if (opt->kind == EX_ASSOC) {
        const char *p = opt->u.assoc.modPrefix, *nm = opt->u.assoc.name;
        if (!p || !nm || strcmp(nm, "sum") != 0 || opt->u.assoc.args.len != 1) return false;
        size_t pl = strlen(p);
        if (pl < 3 || strcmp(p + pl - 3, "seq") != 0) return false;
        sumArg = *(Expr **)vecAt(&opt->u.assoc.args, 0);
    } else if (opt->kind == EX_CALL && opt->u.call.callee && opt->u.call.callee->kind == EX_IDENT
               && opt->u.call.args.len == 1) {
        const char *nm = opt->u.call.callee->u.ident.name;
        const char *dollar = nm ? strrchr(nm, '$') : NULL;
        if (!dollar || strcmp(dollar + 1, "sum") != 0) return false;
        size_t ml = (size_t)(dollar - nm);
        if (ml < 3 || strncmp(nm + ml - 3, "seq", 3) != 0) return false;
        sumArg = *(Expr **)vecAt(&opt->u.call.args, 0);
    } else {
        return false;
    }
    if (!sumArg || sumArg->kind != EX_IDENT || strcmp(sumArg->u.ident.name, tmp) != 0) return false;
    bool zero = false;
    if (fb->kind == EX_INT && fb->u.ival == 0) zero = true;
    if (fb->kind == EX_FLOAT && fb->u.fval == 0.0) zero = true;
    if (!zero) return false;
    const char *sname = bind->u.var.name;
    if (strcmp(sname, tmp) == 0 || strcmp(sname, aVar) == 0) return false;
    if (fuseMentions(rhs, sname)) return false;
    /* `tmp` has no other use in the block's other statements. */
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 2) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    int line = bind->line;
    char accName[32];
    snprintf(accName, sizeof accName, "__extc_facc%zu", c->m->funcs.len + n);
    const char *acc = arenaStrndup(c->arena, accName, strlen(accName));
    /* `var acc: T = T(0)` */
    Stmt *accDecl = stmtNew(c->arena, ST_VAR, line);
    accDecl->u.var.name = acc;
    accDecl->u.var.ann  = elem;
    accDecl->u.var.init = elem->name[0] == 'f' ? exprFloatNew(c->arena, 0.0, line)
                                                : exprIntNew(c->arena, 0, line);
    accDecl->u.var.mut  = true;
    /* `for aVar in aHi { acc = acc + tmp[aVar] }` (handed to S2 to fold into A). */
    Expr *sumOp = exprBinNew(c->arena, "+", exprIdent(c->arena, acc, line),
                             exprIndexNew(c->arena, exprIdent(c->arena, tmp, line),
                                          exprIdent(c->arena, aVar, line), line), line);
    Vec redStmts;
    vecInit(&redStmts, c->arena, sizeof(Stmt *));
    *(Stmt **)vecPush(&redStmts) =
        stmtAssignNew(c->arena, exprIdent(c->arena, acc, line), sumOp, NULL, line);
    Stmt *redBody = stmtNew(c->arena, ST_BLOCK, line);
    stmtBlockSetStmts(redBody, &redStmts);
    Stmt *redLoop = stmtRangeForNew(c->arena, aVar, exprClone(c->arena, aHi), redBody, line);
    /* `var s = acc` (the original binding keeps its name and line). */
    bind->u.var.init = exprIdent(c->arena, acc, line);
    /* Replace one statement with three: accDecl, redLoop, bind. */
    vecPush(v);
    vecPush(v);
    Stmt **base = (Stmt **)vecAt(v, 0);
    size_t oldLen = v->len - 2;
    memmove(&base[i + 4], &base[i + 2], (oldLen - (i + 2)) * sizeof(Stmt *));
    base[i + 2] = accDecl;
    base[i + 3] = redLoop;
    base[i + 4] = bind;
    return true;
}

/**
 * @brief Find **one** fusable element chain in the block and rewrite it (return on a hit; one site
 *        per call).
 *
 * Walk the "temporary view declarations" front to back; try S1 first (the declaration is followed by
 * the loop A writing it, then by the loop B reading it), and, failing that, S2 (the declaration is
 * followed by A, then `var acc = init`, then the ascending reduction loop B).
 *
 * @param c      Checker.
 * @param block  Block statement (its statement table is shortened in place).
 * @return       True when one site was rewritten.
 */
static bool fuseOnePass(Checker *c, Stmt *block) {
    Vec *v = &block->u.block.stmts;

    for (size_t i = 0; i + 2 < v->len; i++) {
        Stmt *decl = *(Stmt **)vecAt(v, i);
        if (decl->kind != ST_VAR || !decl->u.var.ann || !decl->u.var.init) continue;
        if (decl->u.var.init->kind != EX_NEW) continue;
        const char *tmp = decl->u.var.name;
        Stmt *a = *(Stmt **)vecAt(v, i + 1);
        Stmt *b = *(Stmt **)vecAt(v, i + 2);
        Stmt *aW, *aBody, *bW, *bBody;
        const char *aVar, *bVar;
        Expr *aLo, *aHi, *bLo, *bHi;
        if (!fuseLoopFrom(c, a, &aW, &aBody, &aVar, &aLo, &aHi)) continue;
        if (!fuseLoopFrom(c, b, &bW, &bBody, &bVar, &bLo, &bHi)) {
            if (fuseTryReduce(c, block, i) || fuseTryReduceContainer(c, block, i)
                || fuseTryReduceSumCall(c, block, i)
                || fuseTryReduceExtremumCall(c, block, i)) return true;
            continue;
        }
        (void)aW;
        (void)bW;
        if (strcmp(aVar, bVar) != 0) continue;
        if (!fuseExprSame(aLo, bLo) || !fuseExprSame(aHi, bHi)) continue;
        if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) continue;
        /* A's body: exactly [tmp[i] = rhs, step]. */
        Expr *rhs = NULL;
        {
            size_t seen = 0;
            for (size_t k = 0; k < aBody->u.block.stmts.len; k++) {
                Stmt *s = *(Stmt **)vecAt(&aBody->u.block.stmts, k);
                if (s == aBody->forStep) continue;
                if (++seen > 1) break;
                if (s->kind != ST_ASSIGN || s->u.assign.op) break;
                Expr *t = s->u.assign.target;
                if (!t || t->kind != EX_INDEX || !t->u.index.obj || t->u.index.obj->kind != EX_IDENT
                    || strcmp(t->u.index.obj->u.ident.name, tmp) != 0
                    || !t->u.index.index || t->u.index.index->kind != EX_IDENT
                    || strcmp(t->u.index.index->u.ident.name, aVar) != 0) break;
                if (!fusePureExpr(s->u.assign.value)) break;
                if (fuseMentions(s->u.assign.value, tmp)) break;
                rhs = s->u.assign.value;
            }
            if (!rhs || seen != 1) continue;
        }
        /* B's body and its read/write names: a written name and a read name must not overlap (the
         * order of cross-element dependences would change). */
        Vec writes, reads;
        vecInit(&writes, c->arena, sizeof(const char *));
        vecInit(&reads, c->arena, sizeof(const char *));
        if (!fuseCheckBody(bBody, tmp, bVar, &writes, &reads)) continue;
        fuseCollectIdents(rhs, &reads);
        bool clash = false;
        for (size_t k = 0; k < writes.len && !clash; k++) {
            const char *w = *(const char **)vecAt(&writes, k);
            for (size_t m = 0; m < reads.len; m++)
                if (strcmp(w, *(const char **)vecAt(&reads, m)) == 0) { clash = true; break; }
        }
        if (clash) continue;
        /* `tmp` has no other use in the statements of this block (including sub-blocks, branches, and
         * match arms). */
        for (size_t k = 0; k < v->len && !clash; k++) {
            if (k == i || k == i + 1 || k == i + 2) continue;
            if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) clash = true;
        }
        if (clash) continue;
        /* Rewrite: first replace tmp[i] in B's body, then move B's statements into A's body. */
        for (size_t k = 0; k < bBody->u.block.stmts.len; k++) {
            Stmt *s = *(Stmt **)vecAt(&bBody->u.block.stmts, k);
            if (s == bBody->forStep) continue;
            fuseSubstStmt(c, s, tmp, aVar, rhs);
        }
        Vec fused;
        vecInit(&fused, c->arena, sizeof(Stmt *));
        for (size_t k = 0; k < bBody->u.block.stmts.len; k++) {
            Stmt *s = *(Stmt **)vecAt(&bBody->u.block.stmts, k);
            if (s == bBody->forStep) continue;
            *(Stmt **)vecPush(&fused) = s;
        }
        *(Stmt **)vecPush(&fused) = aBody->forStep;
        stmtBlockSetStmts(aBody, &fused);
        /* Top level: drop the decl and B (two statements); A (the fused loop) stays where the decl
         * was. */
        size_t w = i;
        *(Stmt **)vecAt(v, w++) = a;
        for (size_t k = i + 3; k < v->len; k++)
            *(Stmt **)vecAt(v, w++) = *(Stmt **)vecAt(v, k);
        v->len = w;
        return true;
    }
    return false;
}

/**
 * @brief Rewrite every fusable element chain in the block, to a fixed point (each fusion shortens the
 *        statement table, so this always stops).
 *
 * Eligibility is described at `fuseOnePass` and `fuseTryReduce`: S1 is "a loop writing a temporary view
 * plus an adjacent loop reading it", S2 is "a loop writing a temporary view + `var acc = init` + an
 * ascending reduction loop"; both require the loop variable names to match, the two ends to be
 * structurally equal, and the temporary view to be unreferenced elsewhere in the block.
 *
 * @param c      Checker.
 * @param block  Block being checked (its statement table is shortened in place).
 */
static void fuseElementChains(Checker *c, Stmt *block) {
    if (!block || block->kind != ST_BLOCK || block->u.block.stmts.len < 4) return;
    if (block->parLoop) return;
    if (c->curFunc && (planIsCoro(c->curFunc) || planIsParWorker(c->curFunc))) return;
    while (fuseOnePass(c, block)) { }
}

/**
 * @brief Run one fusion rewrite pass over a function body (a function body does not go through
 *        `checkBlockBody`, so fusion is hooked in separately).
 *
 * @param c     Checker.
 * @param body  Function body block.
 */
void checkFuseFuncBody(Checker *c, Stmt *body) {
    if (!fuseOff()) fuseElementChains(c, body);
}

/* ---------------------------------------------- integer-array expression desugaring */

/**
 * @brief Tell whether a name is a writable rank-1 view, and return its element type.
 *
 * @param c     Checker.
 * @param name  Variable name.
 * @return      The element type (the type must also be `mut`); NULL when it is not a writable view.
 */
static Type *arrayViewElem(Checker *c, const char *name) {
    Sym *sym = lookup(c, name);
    if (!sym || !sym->type) return NULL;
    Type *t = ttBase(sym->type);
    Type *elem = viewElemOf(t);
    if (!elem || !t->mut) return NULL;
    return elem;
}

/**
 * @brief Check the right-hand-side shape of an integer-array expression and record the view names it
 *        references.
 *
 * The accepted shapes are deliberately small: read-only view names (of the same element type), scalar
 * names, number and boolean literals, binary `+ - * /`, and unary `+ -`. Calls, `new`, subscripts,
 * slices, and fields are out of scope -- they are either not element-wise operations or allocate
 * themselves, so the desugaring would change their semantics.
 *
 * @param c      Checker (for name lookup).
 * @param e      Right-hand-side expression.
 * @param elem   Element type of the target view.
 * @param views  Output: the view names found (`const char *`, first-appearance order, duplicates
 *               removed).
 * @return       True when the shape is usable; false otherwise (the caller then leaves the statement
 *               alone).
 */
static bool arrayExprShapes(Checker *c, Expr *e, Type *elem, Vec *views) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: {
        if (!e->u.ident.name) return false;
        Sym *sym = lookup(c, e->u.ident.name);
        if (!sym || !sym->type) return false;
        Type *t = ttBase(sym->type);
        Type *ve = viewElemOf(t);
        if (ve) {
            if (!ttEquals(ve, elem)) return false;
            for (size_t i = 0; i < views->len; i++)
                if (strcmp(*(const char **)vecAt(views, i), e->u.ident.name) == 0) return true;
            *(const char **)vecPush(views) = e->u.ident.name;
            return true;
        }
        /* Scalar name: its type is judged by the ordinary check (literal adaptation is not guessed
         * here). */
        return true;
    }
    case EX_INT: case EX_FLOAT: case EX_BOOL:
        return true;
    case EX_BIN: {
        const char *op = e->u.bin.op;
        if (!op) return false;
        if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 &&
            strcmp(op, "*") != 0 && strcmp(op, "/") != 0) return false;
        return arrayExprShapes(c, e->u.bin.left, elem, views) &&
               arrayExprShapes(c, e->u.bin.right, elem, views);
    }
    case EX_UN: {
        const char *op = e->u.un.op;
        if (!op || (strcmp(op, "-") != 0 && strcmp(op, "+") != 0)) return false;
        return arrayExprShapes(c, e->u.un.operand, elem, views);
    }
    default:
        return false;
    }
}

/**
 * @brief Rewrite the right-hand side of an integer-array expression element-wise: every view name `v`
 *        becomes `v[loop]`.
 *
 * @param c     Checker (for the arena).
 * @param e     Right-hand side that already passed `arrayExprShapes`.
 * @param loop  Loop variable name.
 * @return      The new expression subtree.
 */
static Expr *arrayExprIndexed(Checker *c, Expr *e, const char *loop) {
    Arena *a = c->arena;
    switch (e->kind) {
    case EX_IDENT: {
        Sym *sym = lookup(c, e->u.ident.name);
        if (sym && sym->type && viewElemOf(ttBase(sym->type))) {
            return exprIndexNew(a, exprIdent(a, e->u.ident.name, e->line),
                                exprIdent(a, loop, e->line), e->line);
        }
        return exprClone(a, e);
    }
    case EX_BIN:
        return exprBinNew(a, e->u.bin.op, arrayExprIndexed(c, e->u.bin.left, loop),
                          arrayExprIndexed(c, e->u.bin.right, loop), e->line);
    case EX_UN:
        return exprUnNew(a, e->u.un.op, arrayExprIndexed(c, e->u.un.operand, loop), e->line);
    default:
        return exprClone(a, e);
    }
}

/**
 * @brief Binary operator name of a compound assignment (`+=` gives `"+"`).
 *
 * The shape table of integer-array expressions only knows `+ - * /`, so `%=` and anything else give
 * `NULL` (the caller then gives up on the desugaring).
 *
 * @param aop  Text of the compound assignment operator as the parser stored it (`"+="` and the like;
 *             `NULL` means a plain `=`).
 * @return     A statically stored operator name; `NULL` for forms that are not recognized.
 */
static const char *arrayCompoundOp(const char *aop) {
    if (!aop) return NULL;
    if (strcmp(aop, "+=") == 0) return "+";
    if (strcmp(aop, "-=") == 0) return "-";
    if (strcmp(aop, "*=") == 0) return "*";
    if (strcmp(aop, "/=") == 0) return "/";
    return NULL; /* `%=` is not one of the whole-array shapes */
}

/**
 * @brief Tell whether a type is an instance of a generic struct declared in a module (`ndview<T>`).
 *
 * A module struct's `sdef->name` carries the module prefix (`nd$ndview`), and `isProtoType` compares
 * full names, so only the last segment is compared here.
 *
 * @param t     Type.
 * @param want  Struct name (without the module prefix).
 * @return      True when it is an instance of `want<T>`.
 */
static bool arrayNdProto(const Type *t, const char *want) {
    if (!t || !t->sdef) return false;
    const char *n = t->sdef->name;
    const char *d = strrchr(n, '$');
    return strcmp(d ? d + 1 : n, want) == 0;
}

/**
 * @brief Element type of a name of type `ndview<T>`.
 *
 * The rank-2 shape of an integer-array expression lowers directly to an element-wise kernel call of
 * `std::nd` (such as `addInto`), so only "the binding is an `ndview<T>`" is accepted and its element
 * type is returned.
 *
 * @param c     Checker.
 * @param name  Name.
 * @return      The element type; NULL when it is not an `ndview<T>`.
 */
static Type *arrayNdElem(Checker *c, const char *name) {
    Sym *sym = lookup(c, name);
    if (!sym) return NULL;
    Type *bt = ttBase(sym->type);
    if (!arrayNdProto(bt, "ndview") || bt->targs.len < 1) return NULL;
    return *(Type **)vecAt(&bt->targs, 0);
}

/**
 * @brief Tell whether an expression is the name of an `ndview<T>` with a matching element type.
 *
 * @param c     Checker.
 * @param e     Expression.
 * @param elem  Expected element type.
 * @return      True when it is a matching `ndview<T>` name.
 */
static bool arrayNdName(Checker *c, Expr *e, Type *elem) {
    if (!e || e->kind != EX_IDENT || !e->u.ident.name) return false;
    Type *got = arrayNdElem(c, e->u.ident.name);
    return got && ttEquals(got, elem);
}

/**
 * @brief Tell whether an expression is a scalar operand of matching element type (the `k` of
 *        `scaleInto`).
 *
 * @param c     Checker.
 * @param e     Expression.
 * @param elem  Expected type.
 * @return      True for a literal, or for a name of matching type that is not an ndview.
 */
static bool arrayNdScalar(Checker *c, Expr *e, Type *elem) {
    if (!e) return false;
    if (e->kind == EX_INT || e->kind == EX_FLOAT || e->kind == EX_BOOL) return true;
        /* An explicit conversion like `f64(2.0)`: the checker has not reached here yet, but the parser
         * has already written the converted type. */
    if (e->kind == EX_CONV) return true;
    if (e->type && ttEquals(ttBase(e->type), elem)) return true;
    if (e->kind != EX_IDENT || !e->u.ident.name) return false;
    Sym *sym = lookup(c, e->u.ident.name);
    if (!sym) return false;
    Type *bt = ttBase(sym->type);
    return !arrayNdProto(bt, "ndview") && ttEquals(bt, elem);
}

/**
 * @brief Build a statement "`if !nd::<fn>(dst, …) { trap }`" and hand it to the checker as an ordinary
 *        call.
 *
 * The kernel name uses the module's flat name (`nd$addInto`) and `anSetQualified` is set: the loader's
 * qualified-name rewrite runs before checking, and a module call built inside the checker has to carry
 * the same "already qualified" mark so that it does not trip `requireQualified`.
 *
 * @param c     Checker.
 * @param fn    Kernel name (without the module prefix).
 * @param dst   Destination view name.
 * @param args  Remaining arguments (expressions), in kernel signature order.
 * @param line  Source location.
 * @return      The wrapped statement.
 */
static Stmt *arrayNdKernelStmt(Checker *c, const char *fn, const char *dst,
                               Expr **args, size_t nargs, int line) {
    Arena *a = c->arena;
    /* The name has to outlive this function: `exprIdent` stores only the pointer, so a stack buffer
     * would be overwritten by later calls after returning. */
    char name[64];
    snprintf(name, sizeof name, "nd$%s", fn);
    Expr *call = exprCallNew(a, exprIdent(a, arenaStrndup(a, name, strlen(name)), line), line);
    *(Expr **)vecPush(&call->u.call.args) = exprIdent(a, dst, line);
    for (size_t i = 0; i < nargs; i++) *(Expr **)vecPush(&call->u.call.args) = args[i];
    anSetQualified(call, true);
    Expr *ok = exprUnNew(a, "!", call, line);
    Stmt *die = stmtTrapNew(a, exprStrNew(a,
        "array expression operands must have the same shape as the destination", line), line);
    Stmt *dieBlock = stmtNew(a, ST_BLOCK, line);
    vecInit(&dieBlock->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&dieBlock->u.block.stmts) = die;
    /* Return that `if` directly: wrapping it in another `ST_BLOCK` would open one more block-level
     * arena, so the generated C would differ from the hand-written call (one extra pair of
     * `extc_arena_destroy`). */
    return stmtIfNew(a, ok, dieBlock, NULL, line);
}

/**
 * @brief Lower an integer-array expression on an `ndview<T>` target to an element-wise `std::nd`
 *        kernel call.
 *
 * Accepted shapes (first rank-2 version): `dst = a + b`, `dst = a - b`, `dst = a * b`, `dst = a * k`
 * (`k` on either side), `dst = a + b * k`, and their compound-assignment forms (`dst += a` etc.,
 * first turned into `dst = dst <op> rhs`). The kernel checks the shape itself and returns `bool`;
 * failure traps -- the same criterion as the rank-1 desugaring's "length mismatch traps", except that
 * here the shape is known only at run time.
 *
 * @param c  Checker.
 * @param s  Candidate assignment statement.
 * @return   The lowered statement; NULL when the shape is not recognized.
 */
static Stmt *arrayNdLower(Checker *c, Stmt *s) {
    if (!s || s->kind != ST_ASSIGN || !s->u.assign.target || !s->u.assign.value) return NULL;
    if (s->u.assign.op) return NULL;            /* the compound form was built above, see the call site */
    Expr *tgt = s->u.assign.target;
    if (tgt->kind != EX_IDENT || !tgt->u.ident.name) return NULL;
    const char *dst = tgt->u.ident.name;
    Type *elem = arrayNdElem(c, dst);
    if (!elem) return NULL;
    Expr *v = s->u.assign.value;
    if (v->kind != EX_BIN || !v->u.bin.op) return NULL;
    const char *op = v->u.bin.op;
    Expr *l = v->u.bin.left, *r = v->u.bin.right;
    const int line = s->line;

    if (strcmp(op, "+") == 0) {
        if (arrayNdName(c, l, elem) && arrayNdName(c, r, elem)) {
            Expr *args[2] = { l, r };
            return arrayNdKernelStmt(c, "addInto", dst, args, 2, line);
        }
        if (arrayNdName(c, l, elem) && r->kind == EX_BIN && r->u.bin.op &&
            strcmp(r->u.bin.op, "*") == 0 && arrayNdName(c, r->u.bin.left, elem) &&
            arrayNdScalar(c, r->u.bin.right, elem)) {
            Expr *args[3] = { l, r->u.bin.left, r->u.bin.right };
            return arrayNdKernelStmt(c, "axpyInto", dst, args, 3, line);
        }
        return NULL;
    }
    if (strcmp(op, "-") == 0) {
        if (arrayNdName(c, l, elem) && arrayNdName(c, r, elem)) {
            Expr *args[2] = { l, r };
            return arrayNdKernelStmt(c, "subInto", dst, args, 2, line);
        }
        return NULL;
    }
    if (strcmp(op, "*") == 0) {
        if (arrayNdName(c, l, elem) && arrayNdName(c, r, elem)) {
            Expr *args[2] = { l, r };
            return arrayNdKernelStmt(c, "mulInto", dst, args, 2, line);
        }
        if (arrayNdName(c, l, elem) && arrayNdScalar(c, r, elem)) {
            Expr *args[2] = { l, r };
            return arrayNdKernelStmt(c, "scaleInto", dst, args, 2, line);
        }
        if (arrayNdScalar(c, l, elem) && arrayNdName(c, r, elem)) {
            Expr *args[2] = { r, l };
            return arrayNdKernelStmt(c, "scaleInto", dst, args, 2, line);
        }
        return NULL;
    }
    return NULL;
}

/**
 * @brief Desugar an integer-array expression statement:
 *        `out = a + b` becomes `for i in 0..out.len { out[i] = a[i] + b[i] }`.
 *
 * Only one assignment shape is accepted: a writable rank-1 view as the target and an element-wise
 * expression as the right-hand side. On a hit the statement is replaced by a block: first a length
 * guard (trap unless every participating view has the target's length), then the write loop. The
 * product has exactly the shape of a hand-written loop, so bounds-check elimination, vectorization,
 * and fusion all apply to it as usual.
 *
 * @param c  Checker (the target and the right-hand side have not been checked yet -- only name
 *           lookups happen here, and no scope is changed).
 * @param s  Candidate statement.
 * @return   The replacement statement; NULL when it should not be desugared.
 */
Stmt *arrayAssignLower(Checker *c, Stmt *s) {
    if (!s || s->kind != ST_ASSIGN) return NULL;
    if (!s->u.assign.target || !s->u.assign.value) return NULL;
    Expr *tgt = s->u.assign.target;
    if (tgt->kind != EX_IDENT || !tgt->u.ident.name) return NULL;
    /* A bare view name is not an integer-array expression: `buf = t` is a **view-value** assignment
     * (it swaps pointer and length and keeps the aliasing relation), not an element-wise copy. The
     * desugaring accepts only shapes that carry an operation. */
    if (!s->u.assign.op && s->u.assign.value->kind == EX_IDENT) return NULL;
    const char *outName = tgt->u.ident.name;
    /* Try the rank-2 shape (an `ndview<T>` target) first: it goes through `std::nd`'s element-wise
     * kernel and does not unroll a loop. */
    if (!arrayViewElem(c, outName)) {
        Stmt *nd = arrayNdLower(c, s);
        if (nd) return nd;
        if (s->u.assign.op) {
            const char *cop = arrayCompoundOp(s->u.assign.op);
            if (!cop) return NULL;
            Expr *rewritten = exprBinNew(c->arena, cop, exprIdent(c->arena, outName, s->line),
                                         s->u.assign.value, s->line);
            Stmt tmp = *s;
            stmtAssignSetValue(&tmp, rewritten, NULL);
            return arrayNdLower(c, &tmp);
        }
        return NULL;
    }
    Type *elem = arrayViewElem(c, outName);
    if (!elem) return NULL;

    Arena *a = c->arena;
    const int line = s->line;
    /* The compound form `out += rhs` is `out = out <op> rhs`: each cell of the target is read once
     * and written once, so aliasing is safe. */
    Expr *value = s->u.assign.value;
    if (s->u.assign.op) {
        const char *op = arrayCompoundOp(s->u.assign.op);
        if (!op) return NULL;
        value = exprBinNew(a, op, exprIdent(a, outName, line), value, line);
    }

    Vec views;
    vecInit(&views, c->arena, sizeof(const char *));
    if (!arrayExprShapes(c, value, elem, &views) || views.len == 0) return NULL;
    const char *loop = "__extc_ae";
    Vec out;
    vecInit(&out, a, sizeof(Stmt *));

    /* Length guard: `if out.len != v.len { trap }`. One of the view lengths is the target's own, so
     * it is skipped. */
    for (size_t i = 0; i < views.len; i++) {
        const char *vn = *(const char **)vecAt(&views, i);
        if (strcmp(vn, outName) == 0) continue;
        Expr *lhsLen = exprFieldNew(a, exprIdent(a, outName, line), "len", line);
        Expr *rhsLen = exprFieldNew(a, exprIdent(a, vn, line), "len", line);
        Expr *cond = exprBinNew(a, "!=", lhsLen, rhsLen, line);
        Stmt *die = stmtTrapNew(a, exprStrNew(a,
            "array expression operands must have the same length as the destination", line), line);
        /* Both branches of an `if` are blocks in the parser (`parseBlock` builds a `ST_BLOCK`), so
         * the same shape is built here. */
        Stmt *dieBlock = stmtNew(a, ST_BLOCK, line);
        vecInit(&dieBlock->u.block.stmts, a, sizeof(Stmt *));
        *(Stmt **)vecPush(&dieBlock->u.block.stmts) = die;
        *(Stmt **)vecPush(&out) = stmtIfNew(a, cond, dieBlock, NULL, line);
    }

    Expr *slot = exprIndexNew(a, exprIdent(a, outName, line), exprIdent(a, loop, line), line);
    Stmt *write = stmtAssignNew(a, slot, arrayExprIndexed(c, value, loop), NULL, line);
    Stmt *body = stmtNew(a, ST_BLOCK, line);
    vecInit(&body->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&body->u.block.stmts) = write;
    *(Stmt **)vecPush(&out) = stmtRangeForNew(a, loop,
        exprFieldNew(a, exprIdent(a, outName, line), "len", line), body, line);

    Stmt *wrap = stmtNew(a, ST_BLOCK, line);
    vecInit(&wrap->u.block.stmts, a, sizeof(Stmt *));
    for (size_t i = 0; i < out.len; i++)
        *(Stmt **)vecPush(&wrap->u.block.stmts) = *(Stmt **)vecAt(&out, i);
    return wrap;
}

/**
 * @brief Tell whether a block is a range `for`'s desugared block
 *        (`[loop variable, upper bound, while]`).
 *
 * Once the first two statements of a range block have been checked, `checkBlockBody` widens the loop
 * variable, and for a `par` block it desugars; the outer block of a desugaring product ("length guard
 * + write loop") goes through the same check path, so a shape mismatch has to be skipped rather than
 * treating the guard statement as a loop-variable declaration to retype.
 *
 * @param block  Candidate block.
 * @return       True when the shape is exactly `[ST_VAR, ST_VAR, ST_WHILE]`.
 */
static bool rangeForBlockShape(Stmt *block) {
    if (!block || block->kind != ST_BLOCK || block->u.block.stmts.len != 3) return false;
    Stmt *a = *(Stmt **)vecAt(&block->u.block.stmts, 0);
    Stmt *b = *(Stmt **)vecAt(&block->u.block.stmts, 1);
    Stmt *d = *(Stmt **)vecAt(&block->u.block.stmts, 2);
    return a && b && d && a->kind == ST_VAR && b->kind == ST_VAR && d->kind == ST_WHILE;
}

/* Interior helpers of the hoisting pass (defined below); declared first because
 * `checkHoistStatementTrys` uses them. */
typedef struct { Checker *c; Vec *pre; Vec *guards; int *seq; int line; } TryHoist;
static void tryHoistExpr(TryHoist *h, Expr **slot);
static size_t tryHoistStmt(Checker *c, Stmt *s, Vec *pre, Vec *guards);

/**
 * @brief Hoist the nested `e?`s of `stmts[*pi]` into declarations inserted in front of it.
 *
 * Both the temporary declarations and the guards of `x ?? break` / `x ?? continue` are inserted, in
 * that order. After the insertion `*pi` points at the original statement (now moved back). The outer
 * block of a range loop / `par` block has the fixed shape `[declaration, declaration, while]` and the
 * checker recognizes it by position, so those two kinds of block are skipped as a whole.
 *
 * @param c      Checker.
 * @param stmts  The statement table being walked.
 * @param pi     Current index (changed to the original statement's new position after insertion).
 */
void checkHoistStatementTrys(Checker *c, Vec *stmts, size_t *pi) {
    Stmt *s = *(Stmt **)vecAt(stmts, *pi);
    if (!s) return;
    Vec pre;
    vecInit(&pre, c->arena, sizeof(Stmt *));
    Vec guards;
    vecInit(&guards, c->arena, sizeof(Stmt *));
    if (tryHoistStmt(c, s, &pre, &guards) == 0) return;
    size_t n = pre.len + guards.len;
    for (size_t k = 0; k < n; k++) (void)vecPush(stmts);
    size_t len = stmts->len;
    memmove((Stmt **)vecAt(stmts, *pi + n), (Stmt **)vecAt(stmts, *pi),
            (len - *pi - n) * sizeof(Stmt *));
    for (size_t k = 0; k < pre.len; k++)
        *(Stmt **)vecAt(stmts, *pi + k) = *(Stmt **)vecAt(&pre, k);
    for (size_t k = 0; k < guards.len; k++)
        *(Stmt **)vecAt(stmts, *pi + pre.len + k) = *(Stmt **)vecAt(&guards, k);
    /* The front-inserted declarations must be **checked first**: the loop's next iteration does not
     * visit them again (`*pi` jumps straight to the original statement). A declaration is an
     * ordinary `var x = e?`, so it takes the existing expansion path. */
    size_t first = *pi;
    *pi += n;
    for (size_t k = first; k < *pi; k++) checkStmt(c, *(Stmt **)vecAt(stmts, k));
}

/* ---------------------------------------------------------- `?` in value position
 *
 * `e?` expands to "evaluate once, test for none, `return` on failure"; C has no statement
 * expressions, so a value-position `e?` is accepted only where the whole statement can be rewritten
 * (`let x = e?` / `x = e?` / `return e?` / a statement-level `f()?`). A nested use such as
 * `f(g()? + 1)` is rejected with "only the four positions".
 *
 * This pass hoists a **nested** `e?` into a temporary declaration inserted before the statement: the
 * temporary's initialiser is the `e?` itself, so it takes the existing expansion path (neither type
 * checking nor code generation changes), and the statement is then checked as an ordinary value
 * position that sees a plain temporary. The semantics are preserved as long as the hoist does not
 * change when or how often the expression is evaluated:
 *   - the right side of `&&` / `||` is excluded (not evaluated when short-circuiting);
 *   - the right side of `??` is excluded (not evaluated when the left side has a value);
 *   - lambda bodies are excluded (both lifetime and evaluation point are not at the statement level);
 *   - a `while` condition is excluded (hoisting would evaluate it once, while a condition is
 *     evaluated every iteration).
 * An `if` condition is fine: the temporary declaration is inserted before the `if` and is evaluated
 * once per execution of the block, as before.
 */

/**
 * @brief Context of value-position `?` hoisting.
 *
 * `pre` collects the temporary declarations to insert before the statement; `guards` collects the
 * `if preludeIsNone(...) { break }` / `{ continue }` statements that implement `x ?? break` and
 * `x ?? continue`; `seq` numbers the temporary names (increasing left to right within one statement,
 * keeping evaluation order equal to the source); `line` supplies the line for the generated
 * declarations and identifiers.
 */

/**
 * @brief Recursively hoist the `e?`s found in sub-positions of an expression into temporary
 *        declarations.
 *
 * Walks left to right in evaluation order; on `e?` it builds `var __extc_tq<N> = e?`, collects it in
 * `h->pre`, and replaces the position with a reference to the temporary.
 *
 * @param h     Hoisting context.
 * @param slot  Expression slot (rewritten in place).
 */
static void tryHoistExpr(TryHoist *h, Expr **slot) {
    Expr *e = *slot;
    if (!e) return;
    if (e->kind == EX_COALESCE && e->u.coalesce.fallback &&
        e->u.coalesce.fallback->kind == EX_IDENT &&
        e->u.coalesce.fallback->u.ident.name &&
        (strcmp(e->u.coalesce.fallback->u.ident.name, "__extc_break") == 0 ||
         strcmp(e->u.coalesce.fallback->u.ident.name, "__extc_continue") == 0)) {
        /* `x ?? break`: evaluate once, test for none, jump. The guard uses `preludeIsNone` (the
         * prelude's generic function, which only asks "is there a value"); reading the value uses
         * `!` (the guard proved there is one, so it does not trap). */
        const bool isBreak =
            strcmp(e->u.coalesce.fallback->u.ident.name, "__extc_break") == 0;
        char buf[64];
        snprintf(buf, sizeof buf, "__extc_tg%d", (*h->seq)++);
        const char *name = arenaStrndup(h->c->arena, buf, strlen(buf));
        *(Stmt **)vecPush(h->pre) =
            stmtVarNew(h->c->arena, name, NULL, e->u.coalesce.main, false, h->line);
        Expr *cond = exprCallNew(h->c->arena,
                                 exprIdent(h->c->arena, "preludeIsNone", h->line), h->line);
        *(Expr **)vecPush(&cond->u.call.args) = exprIdent(h->c->arena, name, h->line);
        Stmt *jump = stmtNew(h->c->arena, isBreak ? ST_BREAK : ST_CONTINUE, h->line);
        Vec body;
        vecInit(&body, h->c->arena, sizeof(Stmt *));
        *(Stmt **)vecPush(&body) = jump;
        Stmt *blk = stmtNew(h->c->arena, ST_BLOCK, h->line);
        stmtBlockSetStmts(blk, &body);
        *(Stmt **)vecPush(h->guards) = stmtIfNew(h->c->arena, cond, blk, NULL, h->line);
        *slot = exprSignNew(h->c->arena, exprIdent(h->c->arena, name, h->line), h->line);
        return;
    }
    if (e->kind == EX_TRY) {
        char buf[64];
        snprintf(buf, sizeof buf, "__extc_tq%d", (*h->seq)++);
        const char *name = arenaStrndup(h->c->arena, buf, strlen(buf));
        *(Stmt **)vecPush(h->pre) = stmtVarNew(h->c->arena, name, NULL, e, true, h->line);
        *slot = exprIdent(h->c->arena, name, h->line);
        return;
    }
    switch (e->kind) {
    case EX_BIN:
        tryHoistExpr(h, &e->u.bin.left);
        /* The right side of a short-circuit operator may not be evaluated at all, so hoisting it
         * would change how often it runs. */
        if (strcmp(e->u.bin.op, "&&") != 0 && strcmp(e->u.bin.op, "||") != 0)
            tryHoistExpr(h, &e->u.bin.right);
        return;
    case EX_COALESCE:
        /* The fallback is evaluated only when the left side is `none`. */
        tryHoistExpr(h, &e->u.coalesce.main);
        return;
    case EX_UN: case EX_SIGN: case EX_REF: case EX_DEREF:
        tryHoistExpr(h, &e->u.un.operand);
        return;
    case EX_CONV:
        tryHoistExpr(h, &e->u.conv.operand);
        return;
    case EX_FIELD:
        tryHoistExpr(h, &e->u.field.obj);
        return;
    case EX_INDEX:
        tryHoistExpr(h, &e->u.index.obj);
        tryHoistExpr(h, &e->u.index.index);
        return;
    case EX_SLICE:
        tryHoistExpr(h, &e->u.slice.obj);
        tryHoistExpr(h, &e->u.slice.lo);
        tryHoistExpr(h, &e->u.slice.hi);
        return;
    case EX_CALL: {
        Vec *args = &e->u.call.args;
        for (size_t i = 0; i < args->len; i++) tryHoistExpr(h, (Expr **)vecAt(args, i));
        return;
    }
    case EX_METHOD: {
        tryHoistExpr(h, &e->u.method.recv);
        Vec *args = &e->u.method.args;
        for (size_t i = 0; i < args->len; i++) tryHoistExpr(h, (Expr **)vecAt(args, i));
        return;
    }
    case EX_ASSOC: {
        Vec *args = &e->u.assoc.args;
        for (size_t i = 0; i < args->len; i++) tryHoistExpr(h, (Expr **)vecAt(args, i));
        return;
    }
    default:
        /* Other shapes (lambdas, struct literals, whatever is nested inside a `match`, ...) are not
         * hoisted: either their evaluation point differs, or they cannot contain a value-position
         * `?` in the first place. */
        return;
    }
}

/**
 * @brief Hoist the nested `e?`s in a statement's value positions, collecting them as front-inserted
 *        declarations.
 *
 * Exactly the four positions whose statement root is `e?` are **not** hoisted (`let x = e?` /
 * `x = e?` / `return e?` / a statement-level `f()?` have their own expansion paths). A `while`
 * condition is not hoisted either (hoisting would evaluate it once).
 *
 * @param c       Checker.
 * @param s       Statement.
 * @param pre     Table collecting the front-inserted declarations.
 * @param guards  Table collecting the `x ?? break` / `x ?? continue` guards.
 * @return        Number of declarations collected.
 */
static size_t tryHoistStmt(Checker *c, Stmt *s, Vec *pre, Vec *guards) {
    int seq = 0;
    TryHoist h = { c, pre, guards, &seq, s->line };
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.init && s->u.var.init->kind != EX_TRY) tryHoistExpr(&h, &s->u.var.init);
        break;
    case ST_ASSIGN:
        if (s->u.assign.value && s->u.assign.value->kind != EX_TRY) tryHoistExpr(&h, &s->u.assign.value);
        break;
    case ST_EXPR:
        if (s->u.expr.expr && s->u.expr.expr->kind != EX_TRY) tryHoistExpr(&h, &s->u.expr.expr);
        break;
    case ST_RETURN:
        if (s->u.ret.value && s->u.ret.value->kind != EX_TRY) tryHoistExpr(&h, &s->u.ret.value);
        break;
    case ST_TRAP:
        tryHoistExpr(&h, &s->u.trap_.msg);
        break;
    case ST_IF:
        tryHoistExpr(&h, &s->u.ifs.cond);
        break;
    default:
        break;
    }
    return pre->len;
}

/**
 * @brief Find the index of the "subject binding" in a container `for`'s outer block (for
 *        `forRetargetToIterator` to use).
 *
 * When the subject is not a place (`for d in split(s)`) the parser inserts a hidden subject
 * declaration before the view binding; the retarget has to wait until that declaration has been
 * checked (only then is the name in the symbol table), so its position is computed here first.
 *
 * @param block  The outer block of `forDesugar`.
 * @return       The subject binding's index; 0 when there is no hidden subject, `(size_t)-1` when
 *               the shape is not recognized.
 */
static size_t forDesugarBindIndex(Stmt *block) {
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt *s = *(Stmt **)vecAt(&block->u.block.stmts, i);
        if (!s) continue;
        if (s->kind == ST_VAR && s->u.var.init && s->u.var.init->kind == EX_SLICE)
            return i == 0 ? 0 : i;   /* before the view binding there can only be the hidden subject */
    }
    return (size_t)-1;
}

/**
 * @brief Check a block body in a fresh scope; the block's bindings leave scope when it ends.
 *
 * A block that reclaims its own storage is a place boundary: suspending inside it is forbidden (rule
 * 3), and `c->placeBoundaryDepth` makes `ST_YIELD` see that.
 *
 * @param c      Checker.
 * @param block  Block statement to check.
 */
static void checkBlockBody(Checker *c, Stmt *block) {
    /* The `forDesugar` block's retarget is deferred until "the subject declaration has been
     * checked" (see the hook in the loop): when the subject is not a place the parser inserts a
     * hidden binding, and `forSubjectType` has to read its type from the symbol table. */
    const size_t forBind = block->forDesugar ? forDesugarBindIndex(block) : (size_t)-1;
    if (!fuseOff()) fuseElementChains(c, block);
    const bool boundary = stmtNeedsPlaceBoundary(block, c->curFunc && planIsCoro(c->curFunc));
    if (boundary) c->placeBoundaryDepth++;

    pushScope(c);
    const bool fixedShape = block->parLoop || rangeForBlockShape(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        /* A fixed-shape block (range loop / `par` outer block) is recognized by position, so no
         * statement may be inserted into it. */
        if (!fixedShape) checkHoistStatementTrys(c, &block->u.block.stmts, &i);
        if (forBind != (size_t)-1 && i == forBind) forRetargetToIterator(c, block);
        Stmt **slot = (Stmt **)vecAt(&block->u.block.stmts, i);
        Stmt *lowered = arrayAssignLower(c, *slot);
        if (lowered) *slot = lowered;
        checkStmt(c, *slot);
        /* A range loop's outer block is `[loop variable, __extc_hi, while]`: once both of them have
         * been checked and the `while` has not, widen the loop variable so that uses in the body get
         * the widened type (see widenRangeForVar). A `par` block is desugared at the same point:
         * both ends have types and the names resolve (see parLower). */
        if (i == 1) {
            if (block->parLoop) parLower(c, block);
            else if (rangeForBlockShape(block)) widenRangeForVar(c, block);
        }
    }
    popScope(c);

    if (boundary) c->placeBoundaryDepth--;
}

/**
 * @brief Register a `global` declaration as out-of-frame storage.
 *
 * The storage lives in no frame (code generation emits a block-scope `static`): the depth is
 * recorded as 0 and `outOfFrame` is set, so questions like "may this view cross a `yield`" get the
 * right answer. Both the initialised and the uninitialised declaration paths have to call it: the
 * uninitialised one returns early, and missing the call would leave `global var buf: [8]u8` unmarked,
 * so a view of it would be rejected as "someone else's storage".
 *
 * @param c    Checker; this function does not read it, only for a uniform signature.
 * @param s    Declaration statement; read for `isGlobal`.
 * @param sym  The symbol that has just been registered.
 */
static void pMarkGlobal(Checker *c, Stmt *s, Sym *sym) {
    if (!sym || !s->u.var.isGlobal) return;
    sym->outOfFrame = true;   /* depth 0: the storage outlives every frame */
    sym->depth = 0;
    anSetRefDepth(sym, 0);
    /* **A function-scope `global var` has to be registered in the global table too.** `isGlobalSym`
     * consults that table only; without the entry `storeLayer` treats its depth-0 slot as a
     * "parameter slot" (`check_escape.c` has `sy->depth == 0 && !isGlobalSym(...) => return 1`), so
     * the **stored object is allocated in this frame's arena** and dangles once the function
     * returns: in `global var g: ?ref node` where `g = new node`, a later call reading `g` is a
     * plain segfault or an ASan heap-use-after-free (`tests/asan/global-frame-holds-new.extc`).
     *
     * `checkGlobals` runs before the function bodies are checked, so appending a function-scope
     * global to this table cannot affect its module-level duplicate check (that one compares names,
     * while `isGlobalSym` compares symbol pointers). */
    *(Sym **)vecPush(&c->globals) = sym;
}

/**
 * @brief Decide whether an expression has storage that an assignment can write.
 *
 * Inspects the whole **base chain**, not just the outermost shape: in `f().x` the field access is
 * the outermost node, but the field belongs to a temporary and a write into it disappears at once; a
 * `!` is a place only when its operand is a place.
 *
 *   EX_IDENT                        -> place (a binding)
 *   EX_FIELD / EX_INDEX / EX_SLICE  -> a place when the selected object is one
 *   EX_DEREF                        -> place (`*p` names an object)
 *   EX_SIGN                         -> a place when the unpacked operand is one
 *   anything else                   -> no storage
 *
 * The exception is a `!` that hands back a **reference** while sitting in the base chain:
 * `f()!.x = v` where `f()` returns `?ref T` -- the `!` produces a pointer, the assignment writes the
 * object that pointer designates, and the assertion itself does not have to be a place. Such a `!`
 * may sit in the base chain only; as the assignment target itself (`f()! = v`) the payload temporary
 * is written, so the rule above stands.
 *
 * One shape cannot be decided from syntax: `s[i]` on a view (`slice<T>`), whose element really lives
 * at `s.data + i` even when the view value itself is a temporary. The caller (`ST_ASSIGN`) resolves
 * the base type once more when this function says no, and separates there a fixed array (`f()[i]`)
 * from a view.
 *
 * @param e       Assignment target expression.
 * @param asBase  True when `e` is a link in the base chain rather than the assignment target itself.
 * @return        True when the target is a writable place.
 */
static bool assignTargetWritableAt(const Expr *e, bool asBase) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_DEREF: return true;
    case EX_FIELD: return assignTargetWritableAt(e->u.field.obj, true);
    case EX_INDEX: return assignTargetWritableAt(e->u.index.obj, true);
    case EX_SLICE: return assignTargetWritableAt(e->u.slice.obj, true);
    /* A `!` that hands back a **reference** may sit in the base chain: the assignment writes
     * through the pointer, not into the asserted temporary. As the target itself the payload is
     * written, so the old rule stands. */
    case EX_SIGN:
        if (asBase && e->type && e->type->kind == TY_REF) return true;
        return assignTargetWritableAt(e->u.sign.operand, false);
    default:       return false;
    }
}

/**
 * @brief Decide whether an expression is a writable place (the base-chain rules above).
 *
 * @param e  Assignment target expression.
 * @return   True when the target is a writable place.
 */
static bool assignTargetWritable(const Expr *e) {
    return assignTargetWritableAt(e, false);
}

/* ------------------------------------------------- acyclicity: classifying reference-slot stores
 *
 * `computeFnAcyclic` (`check_top.c`) has to decide whether the heap-reference graph a function builds
 * has a back edge, and whether references live only in counted heap slots. The decision needs two
 * facts that are available **only while the scope is still alive**:
 *   - an upper bound on the layer of the host object (the host of `(*cur).f` is the pointee, so its
 *     `Sym.refDepth` is needed);
 *   - a lower bound on the layer of the value (`ref <local>` takes that binding's block depth; through
 *     a reference binding it takes `Sym.minPointee`).
 * The classification therefore happens here and is recorded in the statement's result slot
 * (`anSetStoreRefKind`); the later pass only closes the graph over calls. Criteria version 1 (its four
 * rules) and the `SR_*` encoding live in `check_internal.h`.
 */

/**
 * @brief Project the layer **upper bound** of the host object the base expression names (deeper =
 *        younger).
 *
 * Only the "host is a heap object" branch is handled: the base is either a reference binding (the
 * object it points to is the host) or one dereference. A return of -1 means the base is not a
 * reference (the host is a stack local, a parameter, or a global, which criterion (iv) covers and the
 * later syntactic scan decides) or the layer is unknown (conservative).
 *
 * `anRefDepth` (`Sym.refDepth` is an upper bound on "the pointee") is used as the **upper** bound: the
 * layer criterion asks "how young may the host be", and a younger upper bound is the conservative
 * direction (the value has to be strictly younger to pass).
 *
 * @param c    Checker (scope still alive).
 * @param obj  Base expression of the projection.
 * @return     Upper bound of the host layer; -1 when it is not a heap host or is unknown.
 */
static int acyclicHostUpper(Checker *c, Expr *obj) {
    if (!obj) return -1;
    switch (obj->kind) {
    case EX_DEREF: {
        /* The host of `(*p).f` is the object `p` points to: the layer upper bound is `p`'s refDepth.
         * Whether `p` is a parameter or a local does not matter on this branch -- a store through a
         * reference always lands in the pointee. */
        Expr *o = obj->u.deref.operand;
        if (o && o->kind == EX_IDENT) {
            Sym *sy = lookup(c, o->u.ident.name);
            if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return anRefDepth(sy);
        }
        return -1;
    }
    case EX_IDENT: {
        Sym *sy = lookup(c, obj->u.ident.name);
        if (!sy || !sy->type) return -1;
        /* Reference binding as the base (`p.f = v`): the host is the object it points to. */
        if (tsub(c, sy->type)->kind == TY_REF) return anRefDepth(sy);
        return -1;                       /* ordinary binding as base: its home is on the stack */
    }
    case EX_FIELD: return acyclicHostUpper(c, obj->u.field.obj);
    case EX_INDEX: return acyclicHostUpper(c, obj->u.index.obj);
    case EX_SLICE: return acyclicHostUpper(c, obj->u.slice.obj);
    default:       return -1;
    }
}

/**
 * @brief Layer upper bound of the host object of the reference slot inside a store target.
 *
 * The host of `EX_DEREF` is the pointee itself; a field, element, or slice is resolved through its
 * base.
 *
 * @param c  Checker (scope still alive).
 * @param t  Store target expression.
 * @return   Upper bound of the host layer; -1 when the host is not a heap object or is unknown.
 */
static int acyclicStoreHostUpper(Checker *c, Expr *t) {
    if (!t) return -1;
    switch (t->kind) {
    case EX_DEREF: return acyclicHostUpper(c, t);
    case EX_FIELD: return acyclicHostUpper(c, t->u.field.obj);
    case EX_INDEX: return acyclicHostUpper(c, t->u.index.obj);
    case EX_SLICE: return acyclicHostUpper(c, t->u.slice.obj);
    default:       return -1;    /* EX_IDENT: retargets a slot of this frame, not a heap slot */
    }
}

/**
 * @brief The **host binding** of a store target (the same base chain as `acyclicHostUpper`).
 *
 * For `(*cur).f = v` the host is the object `cur` points to, and `cur` is the binding wanted here:
 * the allocation sites it may point to form the "candidate region" set (see `CycleMerge`). The same
 * holds for `p.f = v` where `p` is a reference binding.
 *
 * @param c    Checker (scope still alive).
 * @param obj  Base expression of the projection.
 * @return     The reference binding; NULL when the host is not a heap object or is unknown.
 */
static Sym *acyclicHostSym(Checker *c, Expr *obj) {
    if (!obj) return NULL;
    switch (obj->kind) {
    case EX_DEREF: {
        Expr *o = obj->u.deref.operand;
        if (o && o->kind == EX_IDENT) {
            Sym *sy = lookup(c, o->u.ident.name);
            if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return sy;
        }
        return NULL;
    }
    case EX_IDENT: {
        Sym *sy = lookup(c, obj->u.ident.name);
        if (!sy || !sy->type) return NULL;
        return (tsub(c, sy->type)->kind == TY_REF) ? sy : NULL;
    }
    case EX_FIELD: return acyclicHostSym(c, obj->u.field.obj);
    case EX_INDEX: return acyclicHostSym(c, obj->u.index.obj);
    case EX_SLICE: return acyclicHostSym(c, obj->u.slice.obj);
    default:       return NULL;
    }
}

/**
 * @brief Host binding of a store target (`(*cur).next` -> `cur`; `p.f` -> `p`).
 *
 * @param c  Checker.
 * @param t  Store target expression.
 * @return   The host binding; NULL when the host is not a heap object reached through a named
 *           reference.
 */
static Sym *acyclicStoreHostSym(Checker *c, Expr *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case EX_DEREF: return acyclicHostSym(c, t);
    case EX_FIELD: return acyclicHostSym(c, t->u.field.obj);
    case EX_INDEX: return acyclicHostSym(c, t->u.index.obj);
    case EX_SLICE: return acyclicHostSym(c, t->u.slice.obj);
    default:       return NULL;
    }
}

/**
 * @brief **Lower bound** on the layer of the value written into a reference slot (smaller = lives at
 *        least that long).
 *
 * Layer criterion (iii) decides "the value is younger than the host", so the value side needs a
 * **lower** bound: using a too-large upper bound there would pass exactly the unsound direction. Only
 * shapes the later layer solver cannot raise answer with a number -- a direct allocation site (already
 * admitted by (i)) and `ref <local>` (that binding's block depth; the solver never moves a local slot).
 * A value read through a reference binding or a field always answers -1, because `Sym.minPointee` may
 * be raised by the later solver and cannot serve as a lower bound.
 *
 * @param c  Checker (scope still alive).
 * @param v  The value being stored.
 * @return   Lower bound of the layer; -1 when unknown.
 */
static int acyclicValueLower(Checker *c, Expr *v) {
    if (!v) return -1;
    switch (v->kind) {
    case EX_NEW: case EX_GENCALL: {
        int a = planArenaLevel(v);
        return a == ARENA_HOME ? 0 : (a > 0 ? a : -1);
    }
    case EX_REF: {
        /* `ref x` points at the storage of x itself: when x is a local its depth *is* its block depth
         * (exact and stable). */
        Expr *o = v->u.ref.operand;
        if (!o || o->kind != EX_IDENT) return -1;
        Sym *sy = lookup(c, o->u.ident.name);
        if (!sy || !sy->type) return -1;
        if (tsub(c, sy->type)->kind == TY_REF) return -1;   /* `ref *p`: reached through a reference, answer -1 */
        return slotDepth(c, o);
    }
    case EX_SIGN: return acyclicValueLower(c, v->u.sign.operand);
    case EX_TRY:  return acyclicValueLower(c, v->u.try_.operand);
    case EX_CONV: return acyclicValueLower(c, v->u.conv.operand);
    case EX_COALESCE: {
        int a = acyclicValueLower(c, v->u.coalesce.main);
        int b = acyclicValueLower(c, v->u.coalesce.fallback);
        if (a < 0 || b < 0) return -1;
        return a < b ? a : b;       /* both branches may be the result => take the longer-lived one */
    }
    default: return -1;
    }
}

/**
 * @brief Classify one store into a reference slot; of `old` and `now` the **more conservative** one
 *        wins.
 *
 * The same site can be checked several times (the driver runs multiple rounds), so a classification
 * may be written repeatedly: any round saying "does not hold" makes the result "does not hold", and
 * when both are call sources the "inspect the callee" one is kept (it demands more).
 *
 * @param old  Classification recorded so far (`SR_NONE` = not recorded yet).
 * @param now  This round's classification.
 * @return     The merged classification.
 */
static int acyclicMergeKind(int old, int now) {
    if (old == SR_NONE) return now;
    if (old == SR_BAD || now == SR_BAD) return SR_BAD;
    if (old == now) return old;
    if (old == SR_LOCAL || now == SR_LOCAL) return SR_LOCAL;
    if (old == SR_CALL || now == SR_CALL) return SR_CALL;
    return SR_NULL;   /* both static reasons hold: leave a "may pass" code */
}

/**
 * @brief Record the classification of one "write into a reference slot" (criteria (i)(ii)(iii)(i')).
 *
 * Only writes whose **host is a heap object** are handled: a reference slot on the stack (retargeting
 * a reference local or parameter, a field of a local aggregate) is not this pass's business -- the
 * later syntactic scan rejects those under criterion (iv). **Typing is required**: when the target
 * type holds no reference the function returns at once, so an integer assignment such as
 * `i = i + 1` is never counted.
 *
 * @param c       Checker (scope still alive).
 * @param s       Assignment statement (the classification is recorded in its result slot).
 * @param target  Store target.
 * @param value   The value written.
 */
static void noteAcyclicStore(Checker *c, Stmt *s, Expr *target, Expr *value) {
    if (!s || !target || !value) return;
    Type *tt = target->type ? tsub(c, target->type) : NULL;
    if (!tt) return;
    /* Does the slot hold a **counted** reference? A type mentioning a type parameter counts as
     * "possibly holds a reference" too (a generic body is not marked either way); a view handle does
     * not count (see `typeHoldsCountedRef`). */
    if (!typeHoldsCountedRef(c->tt, tt) && !mentionsParam(tt)) return;
    int host = acyclicStoreHostUpper(c, target);
    if (host < 0) return;                  /* slot on the stack: the column of criterion (iv) */
    int kind;
    if (value->kind == EX_NULL) {
        kind = SR_NULL;
    } else if (value->kind == EX_NEW || value->kind == EX_GENCALL) {
        kind = SR_FRESH;
    } else if (value->kind == EX_CALL || value->kind == EX_METHOD
               || value->kind == EX_ASSOC) {
            /* (i'): the later pass decides whether the callee is acyclic and whether its result is
             * provably fresh. An unrecognized callee lands in this branch too; the later pass treats
             * "cannot resolve" as "does not hold". */
        kind = SR_CALL;
    } else {
        int vl = acyclicValueLower(c, value);
        if (vl >= 0 && vl > host) kind = SR_LAYER;
        else                      kind = SR_BAD;
    }
    anSetStoreRefKind(s, acyclicMergeKind(anStoreRefKind(s), kind));
    /* A loop is possible: this write is neither null, nor a fresh allocation, nor a strictly younger
     * layer => the cycle may span regions. Record it (the allocation sites the host may point to are
     * collected on the spot, while the scope is still open) and, after the global level solve, merge
     * these candidate regions into the shallowest one (see `CycleMerge` / `applyCycleMerges`). */
    if (kind == SR_BAD) {
        Sym *base = acyclicStoreHostSym(c, target);
        if (base) noteCycleMerge(c, base, value, s->line);
    }
}

/**
 * @brief Record a **reference local inside a nested block**: the criterion (iv) rejection.
 *
 * The event path only counts edges from counted rc slots to objects. A reference binding at the top
 * level of a function body disappears with the frame, so its object lives at most one call longer
 * (bounded); a binding in a **nested block** (loop body, branch, bare block) is rebuilt each iteration
 * and emits no `dec` on block exit, so an object it solely owns becomes garbage nobody accounts for
 * and the peak grows linearly with the number of iterations (the `blind_localhold` shape).
 *
 * The rejection applies only when the initializer **produces a holder by itself**: `new`, a call
 * result, a struct or array literal, or another binding. Reading an existing slot
 * (`let cur = head.next`) does not count -- that object still has a counted holder.
 *
 * @param c  Checker (scope still alive; `scopes.len` is the block depth).
 * @param s  Declaration statement.
 */
static void noteAcyclicNestedLocal(Checker *c, Stmt *s) {
    if (!s || !s->type || s->u.var.isGlobal) return;
    if (!typeHoldsCountedRef(c->tt, tsub(c, s->type)) && !mentionsParam(s->type)) return;
    if (c->scopes.len <= 1) return;           /* top level of the function body: bounded */
    Expr *ini = s->u.var.init;
    if (!ini) return;                         /* declared as an empty reference: no reference to drop */
    switch (ini->kind) {
    case EX_NEW: case EX_GENCALL: case EX_CALL: case EX_METHOD: case EX_ASSOC:
    case EX_STRUCTLIT: case EX_ARRAYLIT: case EX_ENUMVAL: case EX_IDENT:
    case EX_LAMBDA: case EX_DYN:
        anSetStoreRefKind(s, SR_LOCAL);
        break;
    case EX_REF: {
        /* `ref *p` borrows the pointee (which may be the only holder of this frame's reference
         * binding); `ref x` is merely the address of a local and produces no heap object. */
        Expr *o = ini->u.ref.operand;
        if (o && o->kind == EX_DEREF) anSetStoreRefKind(s, SR_LOCAL);
        break;
    }
    default:
        break;                                /* reading an existing slot: the object still has a counted holder */
    }
}

/**
 * @brief Check one statement and apply the depth and narrowing consequences it brings.
 *
 * The statement's effect counter is cleared on entry, then the statement is dispatched by kind: a
 * declaration resolves its annotation and registers the binding, an assignment goes through the write
 * entry point, `if` / `while` record the facts their conditions prove and check their branches,
 * `match` opens one scope per arm, and blocks and domain blocks each take their own scope path.
 *
 * The effect counter is per statement: `??` may promote its subject into a temporary only when
 * nothing before it in the same statement has an effect, because that temporary is evaluated at the
 * top of the statement.
 *
 * @param c  Checker.
 * @param s  Statement to check.
 */
void checkStmt(Checker *c, Stmt *s) {
    c->stmtFx = 0;      /* per-statement effect counter starts at zero */
    switch (s->kind) {
        case ST_VAR: {
            /* Annotations are resolved here: the parser builds only unresolved type names. */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            /* `coroutine<T>` is a return type rather than storage; `let c = counter(n)` is the
             * "coroutine values have names" form (its type is the frame). */
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* A handle has no meaningful zero value (an empty frame, a broken task): an
             * uninitialized `coroutine<T>` is an error. */
            /* No initializer means zero initialization, and the parser guarantees an annotation is
             * present in that case. */
            if (!s->u.var.init && s->u.var.ann && isProtoType(s->u.var.ann, "coroutine", 1)) {
                ckError(c, s->line,
                        "a `coroutine<T>` value has no zero: it is a handle to a frame living in some"
                        " task's place. Initialize it (`var c: coroutine<i64> = worker(...)`)",
                        "`%s` is declared `coroutine<T>` with no initializer",
                        planCName(s) ? planCName(s) : "?");
                s->u.var.ann = ttError(c->tt);
            }
            if (!s->u.var.init) {
                /* The annotation mentions `T`, so whether a zero value exists cannot be decided now:
                 * the check is deferred to instantiation. */
                if (s->u.var.ann && mentionsParam(s->u.var.ann))
                    recordZeroCheck(c, s->u.var.ann, s->line, s->u.var.name);
                else if (s->u.var.ann && typeLacksZeroValue(c->tt, s->u.var.ann)) {  /* no type parameters: decidable here */
                    /* A missing zero value has two causes, and the error has to say which: an empty
                     * `ref` is a value that looks like it could dangle, while an empty `fn` is a code
                     * pointer that jumps to address 0 when called. */
                    if (typeContainsFn(c->tt, s->u.var.ann))
                        ckError(c, s->line,
                                "A `fn` value is a code pointer with no null in this language:"
                                " initialize it, or write `option<fn …>` when it may be absent.",
                                "cannot zero-initialize `%s`: it contains a `fn`", s->u.var.name);
                    else
                        ckError(c, s->line,
                                "`ref` is a non-nullable reference, so it has no zero value -- "
                                "and neither does any struct that contains one",
                                "cannot zero-initialize `%s`: it contains a reference",
                                s->u.var.name);
                }
                s->type = s->u.var.ann ? s->u.var.ann : ttError(c->tt);
                Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                                   !s->u.var.mut, s->line, c->scopes.len);
                pMarkGlobal(c, s, sym);
                /* A zero-initialized value that can hold references can only hold empty references (a
                 * non-null reference has no zero value, rejected above), so its references all point to
                 * depth 0 rather than to the slot's depth: using the slot depth would wrongly reject
                 * `var a: [2]?ref node  a[0] = ref *p  return a`. */
                if (s->type && typeContainsRef(c->tt, s->type)) anSetRefDepth(sym, 0);
                planSetCName(s, sym->cname, RKIND_STMT);
                return;
            }

            /* `let` is a read-only binding, so its type may not carry `mut`: asking for read-only and
             * writable in the same place is self-contradictory. */
            if (!s->u.var.mut && s->u.var.ann) {
                Type *at = s->u.var.ann;
                if (at && ((at->kind == TY_REF && at->mut) ||
                           (at->kind == TY_GENERIC && at->mut))) {
                    ckError(c, s->line,
                            "a read-only binding cannot carry a writable reference or view; "
                            "write `var` if you need to modify through it",
                            "`let %s` cannot be declared with a `mut` type", s->u.var.name);
                }
            }

            /* `@overwrite` targets one allocation; three checks and one marking:
             *   - the binding must be `var`: each execution points the name at the same storage again,
             *     and usually writes it
             *   - the initializer must be `new`: a value, a local, or a call result has no reusable
             *     storage
             *   - the marking has to be recorded before the initializer is checked, and its level is
             *     computed against the function body rather than the enclosing block */
            if (s->u.var.overwrite) {
                if (!s->u.var.mut) {
                    ckError(c, s->line,
                            "Reusing one piece of storage means the binding is re-pointed at it"
                            " on every execution, so it must be writable. Write `var`.",
                            "`@overwrite` needs `var`: it re-binds the name to the same storage"
                            " on every execution, and you will usually write into it too");
                    return;
                }
                if (!s->u.var.init || s->u.var.init->kind != EX_NEW) {
                    ckError(c, s->line,
                            "`@overwrite` is about an **allocation**: it says \"this piece of"
                            " storage is reused; the previous round's contents are gone\". A value,"
                            " a local, or a call result has no such piece of storage to reuse."
                            " Write `@overwrite var n = new T` (or drop the annotation).",
                            "`@overwrite` only applies to `new`: the initializer must be an"
                            " allocation");
                    return;
                }
                anSetReuse(s->u.var.init, true);
            }

            /* `let x = new T` leaves that storage at zero: this allocation has the name as its only
             * reference, and `let` hands out a read-only reference that can neither be written nor
             * turned back into a writable one (`ref *n`, a `mut ref` parameter, and `x.f = v` are all
             * rejected), so no path can write it.
             *
             * A warning rather than an error, because one legitimate corner exists: handing an
             * all-zero buffer to a read-only consumer, e.g. `let buf = new [64]u8  hash(buf[..])`.
             * `var w = new T  let r = w` does not warn: the object has a writable source, and turning
             * an existing writable alias into a read-only view is exactly what `let` is for. */
            /* Only an **allocation primitive** counts as "writing through this name". At this point the
             * initializer has not been type-checked yet (that happens later), so the criterion is the
             * syntactic shape: `new T` / `new T[n]`, plus the allocation primitives the generator
             * recognizes by name. A generic call returning a scalar allocates nothing, e.g.
             * `let n = copyInto<T>(...)`. */
            bool allocInit = false;
            if (s->u.var.init) {
                if (s->u.var.init->kind == EX_NEW) {
                    allocInit = true;
                } else if (s->u.var.init->kind == EX_GENCALL) {
                    const char *gn = s->u.var.init->u.gencall.name;
                    allocInit = strcmp(gn, "alloc") == 0 || strcmp(gn, "poolSlice") == 0
                             || strcmp(gn, "poolSliceRaw") == 0;
                }
            }
            if (!s->u.var.mut && allocInit) {
                ckWarn(c, s->line,
                       "`let` is about the **name**, not the object: it promises you will not"
                       " write through this name. A fresh allocation has no other name, so"
                       " nothing can ever write into it and the storage stays zero. Use `var`"
                       " to write into it -- or, for a read-only view, bind one of something"
                       " writable: `var w = new T  let r = w`.",
                       "this allocation can never be written: `let` bound a fresh allocation,"
                       " and a read-only reference cannot be turned back into `mut ref`");
            }

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            /* `let x = e?` is one of the positions that allows `?`: with an annotation the annotation
             * decides whether to dereference, and without one the initializer is a value position. */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            /* Without an annotation the natural type is kept, so a reference stays a reference; write
             * `let v = *p` to copy the value. Dereferencing here would make a reference returned by a
             * call unbindable, e.g. `let r = pickFirst(ref a, ref b)`. */
            else {
                it = checkExpr(c, s->u.var.init);
                /* An unannotated binding (`var b = a`) goes through `checkExpr` rather than
                 * `checkInto`, so "copying a container that shares storage" has to be checked
                 * separately here; otherwise `var b = a` would produce no warning at all. */
                warnSharedCopy(c, s->u.var.init, it);
            }

            /* A stream borrow is the only reference that may not be bound: it is the operator's own
             * borrow of the stream, not a reference the user asked for. */
            rejectStreamBorrow(c, s->u.var.init);

            /* Decided after the initializer is checked: `checkExpr` first fixes the home depth from the
             * arguments, and this call then overrides it with the depth at which the value is really
             * stored. */
            markCallHomeIfEscaping(c, s->u.var.init, (int)c->scopes.len);

            /* Downgrade the type inferred for `let` to read-only: `let p = ref x` yields `p: ref T`
             * rather than `mut ref T`, and `let s = a[..]` yields `s: slice<T>`. The permission lives in
             * the type and propagates with every copy, so `var q = p` cannot wash a writable reference
             * back in. */
            if (!s->u.var.mut && !s->u.var.ann) {
                if (it && it->kind == TY_REF && it->mut) {
                    Type *ro = ttRef(c->tt, it->inner);
                    it = ro;
                } else if (it && it->kind == TY_GENERIC && it->mut && ttIsViewType(it)) {
                    it = ttViewReadonly(c->tt, it);
                }
            }
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            /* The initializer may not point at a local that dies before this binding. */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            planSetCName(s, sym->cname, RKIND_STMT);
            /* Record where a view's storage lives: only storage this frame owns may be indexed
             * for a reference, because a container may grow and move the plate behind a view.
             * A plain `let`/`var` of a view type keeps the flag false. */
            sym->frameStore = s->type && ttIsViewType(s->type) &&
                              exprIsFrameStorage(c, s->u.var.init);
            noteAcyclicNestedLocal(c, s);   /* criterion (iv): reference-typed local in a nested block */
            /* Record where this binding's value comes from (its initializer). Promotion walks the
             * chain back: after `var n = new node`, `head = n` follows `n` to the `new` site and raises
             * it to `head`'s level. A declaration without an initializer records NULL. */
            noteOrigin(c, sym, s->u.var.init);
            if (sym && !sym->addressed) sym->heldSrc = s->u.var.init;
                        /* A call as the initializer records a "pool site" (`poolSite`); the final
                         * promotion replay uses it. `makesPool` is not asked here: that closure can be
                         * computed only after all function bodies are checked. */
                        /* Not recognized by shape: `vector<i32>::new()` is `EX_GENCALL` at parse time
                         * and the checker rewrites it **in place** to `EX_ASSOC`/`EX_CALL`, so
                         * recognizing by kind would miss it. Recording the original expression suffices;
                         * it is needed only by the final replay, when the shape is settled. */
            if (sym && !sym->addressed && s->u.var.init && s->u.var.init->kind != EX_NULL)
            {
                sym->poolSite = s->u.var.init;
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[decl] %s kind=%d poolSite=%p\n", sym->name ? sym->name : "-",
                            (int)s->u.var.init->kind, (void *)sym->poolSite);
            }
                /* A binding that holds a reference: the depth of its pointee is computed from the
                 * initializer -- in `var cur: ?ref node = head`, `head` is a parameter, so the cursor has
                 * depth 0 and may be returned. */
                /* A call as the initializer (`let t = make()`) needs separate bookkeeping: the layer the
                 * references in its result land at is known only at the call site, because `arenaArg` is
                 * chosen there, while the field table filled in below covers only struct literals.
                 * Without this, field `t.p` would have no entry, `targetDepth(t)` would fall back to
                 * `anRefDepth(sym)` and answer 0, and storing `t` into a shallower `b` would look safe
                 * until the block exit frees the node.
                 *
                 * Rule: when the callee has a home arena it allocates in the arena passed by this call,
                 * which is the level `markCallHomeIfEscaping` fixed from `at` above, so the result depth
                 * is `at`. Here `at` is `c->scopes.len`, the level the binding sits at. */
            /* `global` storage lives outside any frame (code generation emits a block-scope `static`):
             * depth 0 with `outOfFrame` set, so "may a view cross a `yield`" answers itself. The
             * initializer must be a constant: a static-storage object is initialized once at program
             * start, and C allows only constant expressions in a static initializer. */
            if (sym && s->u.var.isGlobal) {
                /* The conflict between `@overwrite` and `global` is not checked: both annotations
                 * precede `var`, so `@overwrite global var` cannot be written syntactically. */
                if (s->u.var.init && !isConstInit(s->u.var.init)) {
                    ckError(c, s->line,
                            "a `global` is initialized once, when the program starts, so its"
                            " initializer has to be a **constant**: a literal, a constant"
                            " expression, or a struct literal whose fields are all constants",
                            "`%s` is `global`, but its initializer is not a constant", s->u.var.name);
                }
                pMarkGlobal(c, s, sym);
            }
            if (s->type && typeContainsRef(c->tt, s->type)) {
                int d = targetDepth(c, s->u.var.init);
                Expr *ini = s->u.var.init;
                if ((ini->kind == EX_CALL || ini->kind == EX_METHOD)
                    && planCallee(ini) && anNeedsHome(planCallee(ini)) && (int)c->scopes.len > d)
                    d = (int)c->scopes.len;      /* the callee has a home arena: use mine */
                /* Storage whose initializer is signed out-of-frame (`effects Ret=0`, or a chain rooted
                 * at a `global`) is not in the frame at all, so the home-arena heuristic above does not
                 * apply: its depth is **certainly** 0, not "not decided yet". This is what lets the
                 * `.data` of a view binding keep being used after being handed to an unsigned C
                 * function. */
                /* The conclusion goes into the plan-side table: the scope is still alive right now,
                 * while `coroCheckDeferred` runs only after the function body has been checked. */
                bool oo = exprOutOfFrame(c, ini);
                planSetVarInitOutOfFrame(s, oo);
                if (oo) {
                    sym->outOfFrame = true;
                    anSetRefDepth(sym, 0);
                } else {
                    anSetRefDepth(sym, d);
                }
                /* Reference binding: **the shallowest place it may point to** plus the alias source.
                 *
                 * The depth wanted for a destination `(*cur).f` is "the pointee that may live longest"
                 * (a parameter contributes 0, a `new` in the frame contributes 1, min = 0), not the DFA's
                 * max. A **copy** such as `var head = a` must also keep `a` as an alias (the source in
                 * the field table): `head` and `a` share the pointee while `a` is retargeted later
                 * (`a = p`), and the source flattened into `origin` is the one at copy time, which
                 * cannot reach the later site. At `return head`, `promoteFieldsAt` asks along that
                 * source what `a` currently points to, so the `new` in the loop is raised to home too.
                 *
                 * Zero values and literals, and **fresh allocations**, do not count: `var cur = null`
                 * points at no caller storage, and letting it pull the min down to 0 would make the
                 * destination of `(*cur).f = v` demand home for nothing; the source of
                 * `var p = new node` is already covered by `origin`, and recording one more would only
                 * add a needless requirement to retargets such as `cur = p`. */
                if (sym && tsub(c, s->type)->kind == TY_REF) {
                    Expr *ini = s->u.var.init;
                    ExprKind ik = ini->kind;
                    bool isLit = (ik == EX_NULL || ik == EX_INT || ik == EX_FLOAT ||
                                  ik == EX_BOOL || ik == EX_STR);
                    if (!isLit) {
                        /* Use the `refDepth` fixed just above; do not recompute `targetDepth(ini)`. A
                         * call result is the exception (which arena a callee with a home allocates in is
                         * chosen by the call site), `targetDepth(EX_CALL)` answers 0, and the recorded
                         * `refDepth` is what holds. */
                        int dd = anRefDepth(sym);
                        /* When the initializer is **another reference binding** this is an alias copy
                         * (`var ocur = cur`): both share the pointee, so the depth has to follow the
                         * source (see the alias chain of `symMinPointee`) and is not snapshotted here --
                         * a snapshot would take the lexical value at copy time, and `cur = null` in the
                         * first iteration would record 0 and wrongly reject a sound shape. */
                        bool alias = false;
                        if (ini->kind == EX_IDENT) {
                            Sym *src = lookup(c, ini->u.ident.name);
                            alias = src && src->type && src->type->kind == TY_REF;
                        }
                        if (!alias) sym->minPointee = dd;
                        /* Record the **source** only; leave `otherDepth` and the depth accounting
                         * alone, since those two are depth upper bounds read by `targetDepth`, and
                         * treating a declaration as a field write would add one layer to ordinary shapes
                         * (`head = p` would then be wrongly rejected). The source is **not flattened**:
                         * `var head = a` has to keep `a` as an alias so that promotion reaches the new
                         * site after `a` is retargeted (`a = p`). */
                        sym->initSrc = ini;
                        sym->initSrcDepth = dd;
                        if (sym->initMinReq == 0 && dd > 0) sym->initMinReq = dd;
                    }
                }
            }
            /* A struct literal records a depth for every field it writes. An omitted field is
             * zero-initialized and can only hold an empty reference at depth 0: no entry is created for
             * it and a read falls back to the conservative answer -- the cost is a possible wrong
             * rejection, never a missed one. */
            if (s->u.var.init && s->u.var.init->kind == EX_STRUCTLIT && sym) {
                for (size_t fi = 0; fi < s->u.var.init->u.lit.inits.len; fi++) {
                    FieldInit *fip = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, fi);
                    c->curStoreVal = fip->value;          /* which value wrote this field */
                    noteFieldDepthWrite(c, sym, fip->name, targetDepth(c, fip->value));
                }
                c->curStoreVal = NULL;
                /* The struct literal's field table is complete: every written field has an entry, and
                 * an omitted field is either a nullable reference whose zero value is empty (depth 0) or
                 * has no zero value and was already rejected when zero-filling. No field can hold an
                 * unrecorded value. */
                sym->fieldsComplete = true;
            }
            return;
        }

        case ST_ASSIGN: {
            /* The left side must be a **place**: a binding, a field / index / slice of a place, a `*`
             * dereference, or the payload `o!` of a place. The criterion is `assignTargetWritable`
             * above: it walks the whole **base chain**, so `f().x = v` and `p.get(h)!.x = v` (payload is
             * a value) are both rejected; a payload that is a **reference**, `f()!.x = v`, is the
             * exception -- the pointer's target is written, and the base chain really does contain a
             * place. */
            Expr *tgt0 = s->u.assign.target;
            /* A binding marked "storage is outside this frame" is not trusted once it is written
             * again: otherwise `if c { v = mem[..] } else { v = buf[..] }` would inherit the trust of one
             * branch and let the in-frame view of the other branch flow into C. A reassigned binding
             * falls back to the slot depth, which is conservative on purpose. */
            if (tgt0 && tgt0->kind == EX_IDENT) {
                Sym *tsy = lookup(c, tgt0->u.ident.name);
                if (tsy) {
                    tsy->outOfFrame = false;
                    /* A reassigned view loses its storage provenance: the new value may name a
                     * container's plate, and deciding from the shape of one branch would let the
                     * other branch's plate flow through. Conservative on purpose -- declare a
                     * fresh binding when a slice of frame storage is wanted. */
                    tsy->frameStore = false;
                }
            }
            Type *idxBaseT = NULL;    /* base type of an index target, filled in once resolved */
            if (!assignTargetWritable(tgt0)) {
                /* The strict walk failed. One shape cannot be decided without the base type: `s[i]` on
                 * a view. The base type is resolved only on this path (valid code is not checked twice),
                 * and the result is reused by the `[]=` rewrite below. */
                bool viewElem = false;
                if (tgt0 && tgt0->kind == EX_INDEX) {
                    idxBaseT = checkExpr(c, tgt0->u.index.obj);
                    viewElem = idxBaseT && !ttIsError(idxBaseT) && ttIsViewType(idxBaseT);
                }
                /* Same for another shape: when the root of the base chain is a `!` that asserts a
                 * reference (`?ref T`), the pointer's target is written. This cannot be decided
                 * syntactically -- the target has not been checked, so the `!` has no type yet -- so the
                 * root's `!` operand is resolved only on this failure path. */
                bool throughRef = false;
                if (!viewElem && tgt0) {
                    Expr *base = tgt0;
                    while (base) {
                        if (base->kind == EX_FIELD)      base = base->u.field.obj;
                        else if (base->kind == EX_INDEX) base = base->u.index.obj;
                        else if (base->kind == EX_SLICE) base = base->u.slice.obj;
                        else break;
                    }
                    if (base && base->kind == EX_SIGN) {
                        Type *ot = checkExpr(c, base->u.sign.operand);
                        throughRef = ot && !ttIsError(ot) && ot->kind == TY_REF;
                    }
                }
                if (!viewElem && !throughRef) {
                    ckError(c, s->line,
                            "The left side of an assignment must be a place with storage: a variable,"
                            " a field/index/slice **of a place**, a `*` dereference, or the payload"
                            " `o!` of a place. A call, literal, or arithmetic expression has no storage"
                            " to write.",
                            "cannot assign to this expression: it is not a place");
                    return;
                }
            }
            /* A fresh buffer written into a binding declared in an enclosing block outlives the
             * iteration. With reclamation on the collector reaches it (the block is an rc slot and
             * the view's data word is a root), so the bound is the collection floor rather than the
             * round count; with reclamation off it piles up until that block exits. Either way the
             * round adds an allocation that is still live when the next one starts, which is what
             * this warns about: moving the declaration into the loop or using a container that owns
             * its storage keeps the peak at the live set instead of the floor. The storage answer is
             * the binding's lexical depth, not the shape of the statement: a `var` written inside the
             * loop body is released every round and stays silent. */
            if (c->loopBodyDepth > 0 && tgt0 && tgt0->kind == EX_IDENT &&
                s->u.assign.value && exprContainsNew(s->u.assign.value)) {
                Sym *slot = lookup(c, tgt0->u.ident.name);
                if (slot && slot->depth < c->loopBodyDepth)
                    ckWarn(c, s->line,
                           "Declare the buffer inside the loop so that each round releases the one"
                           " before it, or keep the storage in a container that owns it (`vector`,"
                           " `string`, `hashMap`), which grows in place. (`-w` turns this warning"
                           " off.)",
                           "the buffer written into `%s` outlives the loop: every round adds an"
                           " allocation that is still live when the next round starts",
                           slot->name);
            }
            /* `x += y` is `x = x + y`: which operator it is and whether it applies to these two types
             * is decided by the same function that checks a written `x + y`, so the rules cannot
             * diverge; only the assignment half is left here -- the target is writable and the result
             * fits. */
            if (s->u.assign.op) {
                Type *lt = checkExpr(c, s->u.assign.target);
                if (ttIsError(lt)) return;
                if (lt->kind == TY_REF) {
                    ckError(c, s->line,
                            "a compound assignment writes the object, so write through the"
                            " reference: `*p += x`. On the reference itself it would be pointer"
                            " arithmetic, which extC does not have.",
                            "`%s` is a reference, not a value -- write `*p %s ...`",
                            typeStr(c, lt), s->u.assign.op);
                    return;
                }
                Type *vt = checkValue(c, s->u.assign.value);
                if (ttIsError(vt)) return;
                Type *rt = checkCompoundOp(c, s->u.assign.op, s->u.assign.target,
                                           s->u.assign.value, lt, vt, &s->u.assign.opExpr);
                if (ttIsError(rt)) return;
                if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
                checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
                checkAssignable(c, lt, rt, s->u.assign.value, "compound assignment");
                return;
            }

            /* On a type that defines `[]=`, `x[i] = v` is a call of that method, `x.[]=(i, v)`. The
             * statement is rewritten in place, so argument checking, the `mut ref` receiver rule, the
             * result type, and code generation all take the ordinary call path. `[]` and `[]=` are two
             * independent names; when only `[]` is defined, the diagnostic below explains why the
             * assignment is rejected. */
            if (s->u.assign.target && s->u.assign.target->kind == EX_INDEX) {
                Expr *tgt = s->u.assign.target;
                /* `idxBaseT` is non-null only when the writability walk above already resolved the
                 * base type; reusing it avoids checking twice. */
                Type *ot = idxBaseT ? idxBaseT : checkExpr(c, tgt->u.index.obj);
                if (!ttIsError(ot) && findMethod(ttBase(ot), "[]=")) {
                    Vec args;
                    vecInit(&args, c->arena, sizeof(void *));
                    *(Expr **)vecPush(&args) = tgt->u.index.index;
                    *(Expr **)vecPush(&args) = s->u.assign.value;
                    Expr *call = exprNew(c->arena, EX_METHOD, s->line);
                    call->u.method.recv = tgt->u.index.obj;
                    call->u.method.name = "[]=";
                    call->u.method.args = args;
                    s->kind = ST_EXPR;          /* overwrite the union only after every read is done */
                    s->u.expr.expr = call;
                    checkExpr(c, call);
                    return;
                }
                if (!ttIsError(ot) && findMethod(ttBase(ot), "[]")) {
                    ckError(c, s->line,
                            "`[]` and `[]=` are independent: this type defines reading only. Keep"
                            " writing explicit (a container usually has `put`), or add `fn []=`.",
                            "`%s` defines `[]` but not `[]=`, so `x[i] = v` is not available",
                            typeStr(c, ot));
                    return;
                }
            }

            Type *tt_ = checkExpr(c, s->u.assign.target);

            /* When the target is a narrowed binding, compare against the slot's declared type:
             * narrowing changes what a read yields, not what the slot may hold. This makes
             * `while cur != null { cur = cur.next }` legal -- the assignment cancels the proof inside
             * the loop body and the loop condition proves it again next round. */
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *slot = lookup(c, s->u.assign.target->u.ident.name);
                if (slot && slot->type && slot->type->kind == TY_REF && slot->type->nullable &&
                    tt_->kind == TY_REF && !tt_->nullable) tt_ = slot->type;
            }

            /* The target has reference type: this assignment either writes the object it points to or
             * retargets the reference, which the type of the right side decides below; assigning a value
             * to a reference must match the pointee type. */
            if (tt_->kind == TY_REF) {
                /* `requireMutable` does not apply here: it asks whether the reference type carries
                 * `mut`, whereas a retarget writes the slot itself (a variable or a field). Only the
                 * binding has to be writable, so `var p: ref T = ref x  p = ref y` is legal. */
                /* Retargeting writes the slot itself, so only the slot has to be writable:
                 *   - a variable, field, or element needs a writable root (`var`)
                 *   - the writability of `*p` comes from `p`'s type, i.e. `mut ref T` */
                /* First walk the path, then look at the slot it lands in:
                 *   - every reference crossed on the path must be `mut ref` (`*p`, `cur.f`, `(*p).f`)
                 *   - crossing a reference means the storage is inside the pointee, which has no root
                 *     in this frame, so the walk stops there
                 *   - when no reference was crossed the storage is the slot of some binding in this
                 *     frame, and that binding has to be writable */
                bool crossed  = false;
                bool refsOk   = pathRefsAllMut(s->u.assign.target, &crossed);
                bool slotOk   = refsOk;
                if (slotOk && !crossed) {
                    Sym *slotRoot = placeRoot(c, s->u.assign.target);
                    slotOk = slotRoot && slotRoot->mut;
                }
                if (!slotOk) {
                    /* Two causes get two different messages, otherwise the user looks for the mistake
                     * among bindings that are fine. */
                    if (!refsOk)
                        ckError(c, s->line,
                                "`ref T` is a **read-only** borrow; writing the object it "
                                "points to needs `mut ref T` in the declaration. Read-only "
                                "is the default so that a signature says what it does.",
                                "cannot write through a read-only reference");
                    else
                        ckError(c, s->line,
                                "rebinding a reference writes the binding itself, so the binding"
                                " must be writable (`var`, or a `mut ref`)",
                                "cannot rebind `%s`: the binding is read-only",
                                s->u.assign.target->kind == EX_DEREF
                                  ? typeStr(c, s->u.assign.target->u.deref.operand->type)
                                  : "this");
                    return;
                }

                Expr *v = s->u.assign.value;

                /* What the right side is decides which operation this is, disambiguated by type rather
                 * than by forbidding one:
                 *   right side is a value `T`         => write the pointee
                 *   right side is a reference `ref T` => retarget the reference */
                /* `null` needs the reference itself (`?ref T`) as context rather than the pointee: the
                 * right side of `p.next = null` is a reference, so it takes this retarget path. */
                adoptContextType(v, v->kind == EX_NULL ? tt_ : tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* natural type: references are kept */

                /* Dropping the non-null proof has to wait until the right side is checked: the `cur` in
                 * the right side of `cur = cur.next` still depends on the proof the loop condition gave.
                 * Dropping it early would cause a wrong rejection. */
                if (s->u.assign.target->kind == EX_IDENT)
                    unNarrow(c, planCName(s->u.assign.target));

                if (vt0->kind == TY_REF) {
                    /* Retarget: the types have to align (`mut ref` may downgrade to `ref`) and the new
                     * target may not die before the reference.
                     *
                     * The depth comparison uses the old `refDepth`, i.e. the depth of what the slot
                     * pointed at before. It must not be updated until these checks are done, otherwise
                     * the new value would be compared against itself and `p = ref <deeper local>` would
                     * slip through entirely. */
                    int at0 = storeLayer(c, s->u.assign.target);
                    /* The lifetime this reference has to reach is capped by the container itself:
                     * `storeLayer(h.q)` goes through `slotDepth` and answers the depth of the projection
                     * (2), while the local `h` lives at layer 1, so fixing the site at 2 would free it at
                     * the end of the block even though the pointer in `h.q` has already escaped the frame
                     * with `out = h`.
                     *
                     * The cap is `c->scopes.len`: nothing in this frame lives past the current scope,
                     * which is the shallowest layer this store can demand. Only the number used for
                     * bookkeeping is capped; the `at0` passed to `checkEscape` below stays as it is. */
                    /* Try promotion first: can every `new` in this value live to layer `at0`? If not,
                     * the depth check below handles it. This must run before `checkEscape`, which reads
                     * the value's depth. */
                    promoteInto(c, v, at0);
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    /* When the destination is reached through a named reference (`(*cur).next = v`),
                     * this `at0` reads an `anRefDepth` written in lexical order by the body walk: a later
                     * `cur = p` has not been walked yet, so lexical order would change the answer. The
                     * decision is therefore deferred until after the DFA and re-judged by "the
                     * shallowest pointee" (see `recheckPendingRefStores`); other shapes still report on
                     * the spot. */
                    checkEscapeDefer(c, v, at0, s->line, "this reference", s->u.assign.target);
                    /* Retargeting carries the pointee depth along (`cur = cur.next`). */
                    if (s->u.assign.target->kind == EX_IDENT) {
                        Sym *slot0 = lookup(c, s->u.assign.target->u.ident.name);
                        if (slot0 && slot0->type && slot0->type->kind == TY_REF) {
                            int nd = targetDepth(c, v);
                            /* The "possible pointee" depth wanted for a destination `(*cur).f` takes the
                             * **min**: a parameter (0) and a `new` in the frame (1) are both assigned, so
                             * the answer is 0. `null` is not a source (it has no storage, and the value
                             * itself is not the caller's), so letting it pull the min down to 0 would only
                             * demand one extra layer for nothing. */
                            if (v->kind != EX_NULL && (slot0->minPointee < 0 || nd < slot0->minPointee))
                                slot0->minPointee = nd;
                            anSetRefDepth(slot0, nd);
                        }
                        /* Carry the source along too: after `mid = n` the source of `mid` is `n`, so the
                         * intermediate bindings in the promotion walk do not break the chain.
                         *
                         * Recorded only on this branch (a reference-typed target receiving a reference);
                         * a slot whose address was taken is skipped because an alias may modify it.
                         * Cycles such as `a = b  b = a` are stopped by the step limit of
                         * `promoteInto`. */
                        if (slot0) noteOrigin(c, slot0, v);
                        if (slot0 && !slot0->addressed && v && v->kind != EX_NULL) slot0->poolSite = v;
                    }
                    /* A retarget on an element or a field (`a[0] = ref local`) has to be recorded too:
                     * otherwise `var a: [2]?ref node` has upper bound 0 after zero initialization, still 0
                     * after a retarget at depth 1, and `return a` would be accepted while the array points
                     * at a dead local. The maximum is taken because `refDepth` is the upper bound of
                     * where inner references land. */
                    {
                        Sym *rootA = placeRoot(c, s->u.assign.target);
                        /* `(*a).next = b`: `placeRoot` cannot walk past a dereference, so the slot has
                         * to hang on `a`; otherwise the promotion for `return a` cannot reach the object
                         * `b` points to (the cycle stays at block level => dangling on return). */
                        if (!rootA) rootA = destDerefSym(c, s->u.assign.target);
                        if (rootA && rootA->type && typeContainsRef(c->tt, rootA->type)) {
                            int dA = targetDepth(c, v);
                            /* Recorded per field. When no address was taken an entry is overwritten
                             * rather than merged, so the root's effective depth really does drop after
                             * `h.p = null`; always taking the maximum would cause a wrong rejection. The
                             * key is the **whole path** (`bx.mid.next` is recorded as `mid.next`, an
                             * element as `[0]`): taking only the last segment would leave the source of a
                             * two-level projection unmatched to the slot, and the `new` at
                             * `return bx.mid.next` would never be promoted. */
                            const char *fn_ =
                                (s->u.assign.target->kind == EX_FIELD ||
                                 s->u.assign.target->kind == EX_INDEX)
                                  ? fieldSlotKey(c, s->u.assign.target) : NULL;
                            c->curStoreVal = v;
                            noteFieldDepthWrite(c, rootA, fn_, dA);
                            c->curStoreVal = NULL;
                        }
                    }
                    /* Retargeting runs the borrow-value check too: in
                     *     struct slot { r: mut ref i32 }
                     *     fn stash(b: mut ref slot, p: mut ref i32) { b.r = ref *p }
                     * the permission and the depth are both fine (`p` is a parameter, depth 0), and only
                     * "this value really is the caller's" can stop it. Missing this call would produce a
                     * dangling reference. */
                    checkStoreEscape(c, v, s->u.assign.target, s->line);
                    noteAcyclicStore(c, s, s->u.assign.target, v);
                    return;
                }

                /* A reference has no implicit write-through: `p = v` only retargets, and write-through
                 * is written `*p = v`. Each form has exactly one meaning. */
                ckError(c, s->line,
                        "write through the reference instead: `*p = v` -- `=` on a reference"
                        " only ever retargets it",
                        "cannot assign a value to `%s`: it is a reference, not a place",
                        typeStr(c, tt_));
                return;
            }

            desugarBareCtor(c, s->u.assign.value, tt_);
            adoptContextType(s->u.assign.value, tt_);
            Type *vt = checkMaybeTry(c, s->u.assign.value);

            /* Feed the destination depth to the `new` sites in the value first. Order matters: the
             * two blocks below record `targetDepth(value)` into the target's depth table, and a late
             * promotion would record the old number and cause a wrong rejection afterwards. */
            int atDst = storeLayer(c, s->u.assign.target);
            /* This binding now holds this value, so the source follows the value: `originOf` flattens
             * the binding chain to its root expression, so `h = g` makes the source of `h` the root of
             * `g` (the struct literal that produces the allocation), which the promotion walk has to
             * reach. Without the update a declare-then-fill binding would stay unreachable: the walk
             * stops at the empty declaration, the allocation stays in the block arena, and it is freed
             * before the struct holding it is returned.
             *
             * Updated only for a direct assignment to the binding itself; `*q = v` and `s.f = v` write
             * what the binding already points at, and a binding whose address was taken is skipped (an
             * alias may modify it). */
            {
                Sym *dst = (s->u.assign.target->kind == EX_IDENT)
                           ? identBindOf(s->u.assign.target) : NULL;
                if (dst && !dst->addressed) {
                    noteOrigin(c, dst, s->u.assign.value);
                    /* Then record the value itself, not flattened to the chain root: the walk reads it
                     * to reach the storage the binding now holds. Recording the root expression on every
                     * binding of the chain would make `out = h` walk back through `h = zero` to the
                     * literal initialized into `zero`, i.e. an unrelated binding's empty box. */
                    dst->heldSrc = s->u.assign.value;
                }
            }
            /* One store: publication, promotion, the escape set, and the field table all go through
             * `noteStore`. */
            noteStore(c, s->u.assign.value, s->u.assign.target, atDst, s->line);
            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* A value stored into a field or an element may not be deeper than the target. */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            noteAcyclicStore(c, s, s->u.assign.target, s->u.assign.value);
            return;
        }

        case ST_IF: {
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);

            /* Narrowing `?ref T`: `if p != null`, `if p == null ... else`, and the guard form
             * `if p == null { return }` all tell the compiler that `p` is non-null on that branch, so no
             * run-time check has to be generated inside it. */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.ifs.cond, &whenTrue);
            const size_t mark = c->narrow.len;

            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* the proof ends with the then branch */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {
                /* Guard form: the condition leaves the function or the loop, so reaching the statements
                 * after it already means `p` is non-null. The proof stays in the current scope until the
                 * block ends. */
                pushNarrow(c, tg);
            }
            return;
        }

        case ST_WHILE: {
            /* A `while` condition may not be hoisted: a hoisted prefix is evaluated once before the
             * loop instead of once per iteration. */
            c->noHoist++;
            /* The condition is its own **place**: pushing a scope makes allocations inside it one layer
             * deeper, and it records whether the condition brought new allocation sites, which code
             * generation uses to decide whether it needs its own arena freed each iteration. Otherwise
             * `while (new i64[N])[0] == 0` would allocate at the enclosing block's level and grow with
             * the number of iterations. */
            pushScope(c);
            const int sitesBefore = c->allocSites;
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            popScope(c);
            planSetCondAllocs(s, (c->allocSites != sitesBefore));
            c->noHoist--;

            /* A loop condition narrows the same way: the binding is known non-null inside the body.
             * Assigning to it cancels the proof and the next round compares again, as in
             * `while (p) { p = p->next; }`. */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.whiles.cond, &whenTrue);
            const size_t mark = c->narrow.len;
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            /* `checkBlockBody` pushes exactly one scope for the body, so declarations written
             * directly in it land at `scopes.len + 1`. Bindings at a smaller depth belong to an
             * enclosing block and outlive the loop; the assignment case reads this number to tell
             * "released every round" from "kept across rounds". */
            const int enclosingLoop = c->loopBodyDepth;
            c->loopBodyDepth = (int)c->scopes.len + 1;
            checkBlockBody(c, s->u.whiles.body);
            c->loopBodyDepth = enclosingLoop;
            c->narrow.len = mark;
            return;
        }

        case ST_EXPR:
            /* The worker call synthesized by `par` desugaring has already been checked (both the
             * arguments and the plan flags are filled in by `parLower`); taking the ordinary call path
             * again would run an arity check against the worker's parameters. */
            if (s->u.expr.expr && planParWorker(s->u.expr.expr)) return;
            /* A statement-level `f()?`: "this step has to succeed, otherwise the whole chain fails". */
            if (s->u.expr.expr && s->u.expr.expr->kind == EX_TRY) {
                checkMaybeTry(c, s->u.expr.expr);
                return;
            }
            /* A discarded expression is **not a value position**: nothing is read out of it, so a
             * reference result is fine here -- and has to be, since `fin >> x >> line;` produces one.
             * Places that really need a value still follow the `checkValue` rules. */
            checkExpr(c, s->u.expr.expr);
            return;

        case ST_YIELD: {
            /* `yield` with a binding: the name becomes a coroutine-frame local of the yielded type,
             * filled from the frame's `in` slot on resume. It is registered before the rest of this case
             * so that the frame-layout pass can see it. */
            if (s->u.yield_.bind && c->curFunc && planIsCoro(c->curFunc) && planYieldType(c->curFunc)) {
                Type *bt = planYieldType(c->curFunc);
                if (s->u.yield_.bindAnn)
                    /* Handled like a `var` annotation: the parser hands over only the written type, and
                     * `ttResolve` turns it into a usable one. */
                    s->u.yield_.bindAnn = ttResolve(c->tt, c->ctx, s->u.yield_.bindAnn,
                                                    s->u.yield_.bindLine, c->curParams);
                if (s->u.yield_.bindAnn && !ttIsError(s->u.yield_.bindAnn)) {
                    if (strcmp(typeStr(c, s->u.yield_.bindAnn), typeStr(c, bt)) != 0)
                        ckError(c, s->u.yield_.bindLine,
                                "the binding of a `yield` takes the coroutine's yielded type",
                                "`%s` is `%s`, but this coroutine yields `%s`",
                                s->u.yield_.bind, typeStr(c, s->u.yield_.bindAnn), typeStr(c, bt));
                    else
                        bt = s->u.yield_.bindAnn;
                }
                /* The frame's Vec does not exist yet (`coroFrameLay` builds it after the function body),
                 * so the binding is only **recorded** here and consumed by that pass. It has to be
                 * declared as a local now, though, so that later statements in the body can name it; the
                 * frame field itself is built by `coroFrameLay` from the recorded bindings.
                 *
                 * `got = yield e`: `got` is the program's own variable and already declared, so that
                 * symbol must be reused (declaring it again would produce a `__2` suffix and the frame
                 * field would not match); `var got = yield e` does not have the name yet. */
                Sym *sym = lookup(c, s->u.yield_.bind);
                if (!sym)
                    sym = declare(c, s->u.yield_.bind, bt, false, true,
                                  s->u.yield_.bindLine, c->scopes.len);
                s->u.yield_.bindCName = sym ? sym->cname : s->u.yield_.bind;
                if (!c->coroBinds.arena)
                    vecInit(&c->coroBinds, c->arena, sizeof(CoroBind *));
                CoroBind *cb = arenaAllocZero(c->arena, sizeof *cb);
                cb->fn   = c->curFunc;
                cb->name = s->u.yield_.bind;
                cb->type = bt;
                cb->line = s->u.yield_.bindLine;
                *(CoroBind **)vecPush(&c->coroBinds) = cb;
            }
            /* `yield e` is legal only inside a coroutine body, and `e` must be that coroutine's `T`;
             * both follow from the declared return type `coroutine<T>`. */
            FuncDef *cf = c->curFunc;
            if (!cf || !planIsCoro(cf)) {
                ckError(c, s->line,
                        "`yield` only works inside a coroutine: declare the function as"
                        " `fn %s(...) -> coroutine<T>` (that return type is what makes its body a"
                        " coroutine)",
                        "`yield` outside a coroutine", cf ? cf->name : "f");
                checkExpr(c, s->u.yield_.value);
                return;
            }
            /* Rule 3: a suspension may not fall inside a block that reclaims its own storage, because
             * that block's arena is freed before this coroutine resumes. */
            if (c->placeBoundaryDepth > 0) {
                ckError(c, s->line,
                        "A suspension may not sit inside a block that creates a pool: the pool's"
                        " place is released when the block exits, before the coroutine resumes."
                        " Move the pool out of that block, or move the `yield` out of it"
                        "",
                        "`yield` inside a reclaimed block");
            }
            Type *got = checkExpr(c, s->u.yield_.value);
            if (planYieldType(cf) && got && !ttIsError(got))
                checkAssignable(c, planYieldType(cf), got, s->u.yield_.value, "the yielded value");
            return;
        }
        case ST_TRAP: {
            /* The message is the only thing a reader sees when the program stops, so it has to be text:
             * a literal, `slice<u8>`, or a C string (`ref u8`); anything else deserves a compile error.
             *
             * The criterion is by **shape** (`ttIs` knows builtin types only, and
             * `ttIs(t, "slice<u8>")` is always false): a view of `u8` or a reference to `u8`. `mut` is a
             * permission and does not change what can be printed. */
            Type *t = checkExpr(c, s->u.trap_.msg);
            bool isText = true;
            if (t && !ttIsError(t)) {
                if (t->kind == TY_REF) {
                    isText = ttIs(t->inner, "u8");
                } else if (ttIsViewType(t) && t->targs.len == 1) {
                    isText = ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
                } else {
                    isText = false;
                }
            }
            if (!isText) {
                ckError(c, s->line,
                        "`trap` stops the program and prints its message, so the message has to be"
                        " text: a string literal, a `slice<u8>`, or a `ref u8`.",
                        "`trap` needs a message of text, found `%s`", typeStr(c, t));
            }
            return;
        }
        case ST_RETURN: {
            Type *want = c->curFunc ? c->curFunc->ret : NULL;
            if (!s->u.ret.value) {
                /* A coroutine ends by running out or by a bare `return`, both meaning "no more values",
                 * i.e. the state machine's completed state (code generation stores a sentinel).
                 * `return e` is still an error in a coroutine: handing out a value goes through
                 * `yield`. */
                if (c->curFunc && planIsCoro(c->curFunc)) return;
                /* By the time a function body is checked, `void` has two spellings: the builtin
                 * canonical `void` (free functions; `resolveSignature` nulls it out as well), and the
                 * not-yet-resolved **named** type of a method's `-> void`. Both are accepted: a bare
                 * `return` means "no more values", which is exactly what a void function promises. */
                Type *wbase = ttBase(want);
                bool isVoid = want && (ttIs(want, "void") ||
                                       (wbase && wbase->name && strcmp(wbase->name, "void") == 0));
                if (want && !isVoid) {
                    ckError(c, s->line, NULL, "`%s` must return a value of type `%s`",
                            c->curFunc->name, typeStr(c, want));
                }
                return;
            }
            if (c->curFunc && planIsCoro(c->curFunc)) {
                checkExpr(c, s->u.ret.value);
                ckError(c, s->line,
                        "A coroutine hands values out with `yield`; to finish early use a bare"
                        " `return`",
                        "`return e` is not how a coroutine ends");
                return;
            }
            if (!want) {
                Type *got = checkExpr(c, s->u.ret.value);
                /* A lambda may omit its result type: the first `return <value>` decides it. Filling it
                 * in here lets the rest of the body and every later `return` be checked against the real
                 * type. */
                if (c->curFunc && anLamInferRet(c->curFunc) && got && !ttIsError(got)) {
                    c->curFunc->ret = got;
                    anSetLamInferRet(c->curFunc, false);
                    return;
                }
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            /* A bare constructor such as `return failure(e)`: its type comes from `want` above. */
            desugarBareCtor(c, s->u.ret.value, want);

            /* `return e?`: the expression's value is the payload while the function hands back the
             * outer type, so what is compared is whether the payload fits the outer type's payload; code
             * generation wraps it back. The `adoptContextType` plus `checkAssignable` route below cannot
             * be used, since it would make an `i64` stand in for an `option<i64>`. */
            if (s->u.ret.value->kind == EX_TRY) {
                Type *vt = checkTryInner(c, s->u.ret.value);
                Type *wb = ttBase(want);
                /* This branch skips the `checkEscape` below, so `promoteInto` does not run and the
                 * allocation handed out by `e?` would not be recorded as escaping the frame; the pass
                 * after the body would drop the site back to block level. The payload's references end up
                 * owned by the caller, so it is published at layer 0. */
                noteReturnPublish(c, s->u.ret.value, s->line);
                promoteInto(c, s->u.ret.value, 0);
                /* One more fact: the `makesPool` closure has not been computed yet and its flag is
                 * false, so promotion of pool sites cannot land; the replay after the closure raises it
                 * once more from this fact. */
                recordLvlFact(c, s->u.ret.value, 0);
                /* `option<T>` and `result<T, E>` are **enum instances** (a `TY_ENUM` with type
                 * arguments) and `TY_GENERIC` is always false for them, so `TY_ENUM` has to be accepted
                 * here as well; otherwise the payload would not be compared at all and `return o?` from
                 * `option<i64>` to `option<u8>` would quietly turn 1000 into 232, while writing
                 * `option<u8>::some(1000)` directly is rejected. For an enum instance's payload,
                 * `targs[0]` is the success type of both families. */
                if (wb && wb->targs.len >= 1 && (wb->kind == TY_GENERIC || wb->kind == TY_ENUM))
                    checkAssignable(c, *(Type **)vecAt(&wb->targs, 0), vt,
                                    s->u.ret.value, "return value");
                return;
            }

            adoptContextType(s->u.ret.value, want);
            Type *vt = checkInto(c, want, s->u.ret.value);   /* a reference is wanted: do not dereference */
            warnSharedReturn(c, s->u.ret.value, vt);
            markCallHomeIfEscaping(c, s->u.ret.value, 0);    /* it is handed out: use the home arena */
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            /* A returned reference must point at a parameter or at static data, i.e. depth 0. */
            if (dbgOn("EXTC_DBG_RET3"))
                fprintf(stderr, "[ret3] %-8s line=%d kind=%d d=%d mentionsParam=%d\n",
                        c->curFunc?c->curFunc->name:"?", s->line,
                        (int)s->u.ret.value->kind, targetDepth(c, s->u.ret.value),
                        mentionsParam(s->u.ret.value->type)?1:0);
            /* The returned value is handed to the caller, so it is published at layer 0: a return value
             * is supposed to outlive this frame. */
            noteReturnPublish(c, s->u.ret.value, s->line);
            checkEscape(c, s->u.ret.value, 0, s->line, "this return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_MATCH: {
            /* The value of `match e { variant => ... }` is the exhaustiveness check: with `if`/`else`
             * the else branch has to be handled by hand and a variant added later silently falls into
             * it, while `match` reports the missing arms. */
            Type *st = checkValue(c, s->u.match.scrutinee);
            Type *sb = ttBase(st);
            if (ttIsError(st)) return;

            if (!sb || sb->kind != TY_ENUM || !sb->edef) {
                ckError(c, s->line,
                        "`match` works on enums (`type color = | red | green`); "
                        "everything else is compared with `==`",
                        "cannot `match` on a value of type `%s`", typeStr(c, st));
                return;
            }
            TypeDef *td = sb->edef;

            /* The `_` arm matches the variants no arm above covers. It must come last (the parser
             * enforces that) and it binds no payload -- every variant that can land there has a
             * different payload type, so one name has no single type to be given. It makes the
             * `if let` / `while let` desugaring a second arm of an ordinary match, and lets a
             * hand-written match choose "the rest does not matter". */
            MatchArm *wild = NULL;
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                if (!arm->wildcard) continue;
                if (wild) {
                    ckError(c, arm->line, NULL, "`_` may appear only once");
                    return;
                }
                wild = arm;
                if (i + 1 != s->u.match.arms.len) {
                    ckError(c, arm->line,
                            "`_` matches whatever the arms above it did not, so an arm after it"
                            " can never match.",
                            "`_` must be the last arm");
                    return;
                }
            }

            /* Per arm: the name must be a variant of this enum, and the same variant may not be
             * matched twice. */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                if (arm->wildcard) continue;
                if (!findVariant(td, arm->variant)) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPuts(&note, "variants of ");
                    bufPuts(&note, sb->name);
                    bufPuts(&note, ":");
                    for (size_t j = 0; j < td->variants.len; j++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&td->variants, j))->name);
                    ckError(c, arm->line, bufCstr(&note),
                            "`%s` is not a variant of `%s`", arm->variant, sb->name);
                    return;
                }
                for (size_t j = 0; j < i; j++) {
                    MatchArm *prev = *(MatchArm **)vecAt(&s->u.match.arms, j);
                    if (prev->wildcard) continue;
                    if (strcmp(prev->variant, arm->variant) == 0) {
                        ckError(c, arm->line, NULL,
                                "`%s` is matched twice", arm->variant);
                        return;
                    }
                }
            }

            /* Exhaustiveness: a `_` arm covers the rest; without one every variant has to be listed. */
            if (!wild) for (size_t j = 0; j < td->variants.len; j++) {
                const char *vn = (*(Variant **)vecAt(&td->variants, j))->name;
                bool covered = false;
                for (size_t i = 0; i < s->u.match.arms.len && !covered; i++)
                    covered = strcmp((*(MatchArm **)vecAt(&s->u.match.arms, i))->variant, vn) == 0;
                if (covered) continue;

                Buf note;
                bufInit(&note, c->arena);
                bufPuts(&note, "add a `");
                bufPuts(&note, vn);
                bufPuts(&note, " => { ... }` arm");
                ckError(c, s->line, bufCstr(&note),
                        "`match` does not handle `%s` -- every variant must be listed",
                        vn);
                return;
            }

            /* Each arm's body opens its own scope, where the arm's payload bindings
             * (`circle(r) => ...`) live: the i-th name takes the i-th payload field. */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);

                /* The fallback arm has no payload bindings; its body is checked in its own scope as
                 * usual. */
                if (arm->wildcard) {
                    pushScope(c);
                    for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                        checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                    popScope(c);
                    continue;
                }

                Variant  *v   = findVariant(td, arm->variant);

                if (arm->binds.len != v->types.len) {
                    ckError(c, arm->line, NULL,
                            "`%s` carries %zu value(s), so this arm binds %zu name(s), not %zu",
                            v->name, v->types.len, v->types.len, arm->binds.len);
                    return;
                }
                pushScope(c);
                for (size_t k = 0; k < arm->binds.len; k++) {
                    const char *bn = *(const char **)vecAt(&arm->binds, k);
                    Type *bt = payloadType(c->tt, sb, v, k);
                    /* A payload binding is a copy of the value (value semantics) and the name is
                     * read-only; copy into a `var` to modify it. */
                    Sym *bs = declare(c, bn, bt, false, false, arm->line, c->scopes.len);
                /* `declare` gives the conservative answer (the pointee lives at the depth of my slot),
                 * and the side that knows the initializer has to narrow it. Here the initializer is the
                 * **scrutinee**: the payload is a copy, so its references point where the scrutinee's do
                 * (the scrutinee's own binding already carries that answer, which is why `targetDepth`
                 * looks up the binding rather than the expression node). Without the narrowing an
                 * ordinary copy such as `match dl::sym(h, n) { some(p) => { return p } ... }` would be
                 * treated as a local of borrow depth 2 and rejected. */
                    if (bt && typeContainsRef(c->tt, bt))
                        anSetRefDepth(bs, targetDepth(c, s->u.match.scrutinee));
                    /* Write the resolved C name back into the arm, as is done for `Param.cname` and the
                     * declared `cname`. Otherwise, when two `match`es in the same scope bind the same
                     * name, the declaration uses the plain name while the body looks up `s__2`, and the
                     * generated C does not compile. */
                    *(const char **)vecAt(&arm->binds, k) = bs->cname;
                }
                for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                    checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                popScope(c);
            }
            return;
        }

        case ST_DOMAIN: {
            /* The tail block of `d.run { ... }` is a **domain block**: "where the concurrency in here
             * runs, and it all has to finish before this block ends". Which callees may take a tail block
             * is decided by **type** (the language-level object `domain`), not by a name or an
             * annotation: an annotation cannot be verified, and a name special case silently breaks on a
             * rename. The body is checked **inside** the domain, which is why `ext` is legal there and
             * remains a compile error elsewhere. */
            Expr *cal = s->u.domain_.callee;
            Type *rt = NULL;
            if (cal && cal->kind == EX_FIELD && cal->u.field.obj) {
                Expr *recv = cal->u.field.obj;
                checkExpr(c, recv);
                rt = recv->type;
                while (rt && rt->kind == TY_REF) rt = rt->inner;   /* `d: ref domain` */
            }
            /* The domain's own method is `run`. The receiver's **type** is the criterion for a domain,
             * and the method name is part of the object protocol, so a typo such as `d.rnu { ... }` is a
             * compile error rather than being silently accepted. */
            const char *mname = (cal && cal->kind == EX_FIELD) ? cal->u.field.name : NULL;
            if (!mname || strcmp(mname, "run") != 0 ||
                !rt || rt->kind != TY_STRUCT || !rt->name || strcmp(rt->name, "domain") != 0) {
                ckError(c, s->line,
                        "A trailing block is the body of a **domain**: it says where the concurrency in it"
                        " runs and that all of it must be finished before the block ends.",
                        "a trailing block needs `run` on a domain object, found `%s`",
                        rt ? typeStr(c, rt) : "something that is not an object");
                return;
            }
            /* Scope rule: `ext` inside this block targets **this** domain. Save/restore covers the whole
             * stack -- a nested domain shadows the outer one, and the outer one comes back on exit. */
            Expr *savedDom = c->curDom;
            c->curDom = (cal && cal->kind == EX_FIELD) ? cal->u.field.obj : NULL;
            c->domainDepth++;
            checkStmt(c, s->u.domain_.body);
            c->domainDepth--;
            c->curDom = savedDom;
            return;
        }
        case ST_BLOCK:            checkBlockBody(c, s);
            return;
    }
}
