/* Escape checking: the depth model, borrows, and the home arena.
 *
 * This is the pass that decides whether a borrow may outlive the storage it points
 * at, and which arena an allocation has to live in. It is driven by a single rule
 * (see the block below) and by the per-expression depth recorded for every value.
 */

#include "check_internal.h"
#include <stdlib.h>
#include <stdio.h>

/* --------------------------------------------------------------- escape checks
 *
 * The one reference rule: if a reference `r` points at a value `v`, then
 * depth(r) >= depth(v). In words: a reference may not outlive what it points at.
 *
 * Depth is purely lexical: a parameter is depth 0, a function body is depth 1, and
 * every nested block adds 1. Comparing two integers is therefore enough, and no
 * lifetime annotations are needed from the user.
 *
 * Three obligations are checked here; a fourth is deliberately left out:
 *   1. A returned reference or view must point at a parameter or at static data
 *     (depth 0).
 *   2. A local variable may not be initialized with a reference to something deeper
 *     than the variable itself.
 *   3. A field or element store (`o.f = v`) may not store a reference to something
 *     deeper than the target.
 *   4. Passing a reference into a function that stores it needs interprocedural
 *     analysis and is checked at the call site instead (see `checkCallRefArgs`).
 */

static int maxInt(int a, int b) { return a > b ? a : b; }

static bool isGlobalSym(Checker *c, Sym *s);   /* defined below; used by storeLayer */

/* The diagnostic baseline for `EXTC_DBG_RHO`: a depth that depends only on lexical block
 * depths and origin chains, never on the level solver or on a cache. Not used by the
 * checker itself, so it can be compared against the real answer without changing it. */
static int exprRefDepthPure(Checker *c, Expr *e, int hops, Expr **seen);


/* Depth of the storage a place expression denotes.
 *
 * A "place" is something that can be written: a binding, a field, an element, or a
 * dereference. The answer is how deep the *storage* is, not how deep a reference
 * stored there points; `storeLayer` answers the other question.
 *
 * Params:
 *    c - checker (scope stack and type table)
 *    e - the place expression
 *
 * Returns:
 *    Lexical depth of the storage: 0 for a parameter, global, or anything outside
 *    this frame; the block depth for a local; for a reference-typed binding, the
 *    depth of the storage it points at (the slot itself does not hold the value).
 */
int placeDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* Dereference does not create storage; it only names storage through a pointer,
     * so the lifetime in question is that of the reference. */
    if (e->kind == EX_DEREF) return exprRefDepth(c, e->u.deref.operand);
    /* A reference-typed binding denotes the storage it points at, not its own slot
     * (`var cur: ?ref node = head`: the slot is in this frame, the node is outside). */
    if (e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return sy->refDepth;
        return sy ? sy->depth : 0;
    }
    /* A field or element lives inside the object that contains it. */
    if (e->kind == EX_FIELD) return placeDepth(c, e->u.field.obj);
    if (e->kind == EX_INDEX) return placeDepth(c, e->u.index.obj);
    Sym *root = placeRoot(c, e);
    return root ? root->depth : 0;
}

/* Arena level at which the storage of a place lives.
 *
 * This answers a different question from `placeDepth`, and the distinction matters:
 *
 *   - how deep is the object a reference *points at*?  -> `placeDepth`
 *   - how deep is the storage *itself*?                -> this function
 *
 * `head = n` stores a pointer into the slot `head`. The slot lives in this frame,
 * so the value that `n` points at must live at least as long as that slot. Asking
 * `placeDepth(head)` on a reference-typed binding would instead report where the
 * *pointee* lives (for `= null`, depth 0), which would reject the ordinary pattern
 * of building a list whose head outlives the loop body.
 *
 * Params:
 *    c - checker
 *    e - the target place of a store
 *
 * Returns:
 *    Arena level of the storage. A local binding reports its block depth; a parameter
 *    reports 1, because the argument is a copy and the slot is in this frame; a global
 *    reports 0, because static storage outlives every frame. A place projected through
 *    a reference or parameter falls back to `placeDepth`, which reports the caller's
 *    depth (0), so storing into a caller's object is still treated conservatively.
 */
int storeLayer(Checker *c, Expr *e) {
    if (e && e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy) {
            /* A parameter's slot is in this frame: the argument is a copy, so writing
             * it cannot touch the caller's binding. Depth 1 is therefore correct, and
             * assigning between two parameters is allowed. A global is static storage
             * and reports 0, because it outlives every frame. */
            if (sy->depth == 0 && !isGlobalSym(c, sy)) return 1;
            return sy->depth;
        }
    }
    return placeDepth(c, e);
}

/* Depth of the references inside a value, computed from syntax rather than types.
 *
 * `exprRefDepth` returns early when `typeContainsRef` says the type carries no
 * reference, but a struct whose *field* type is `ref` reports false there. A value
 * such as `inner { v: ref local }` would then be treated as holding no pointer and
 * be given depth 0, which lets a live pointer escape unnoticed.
 *
 * This walk ignores types and looks only at the shape of the expression, so it can
 * only raise the depth estimate. Raising the estimate is the safe direction: it may
 * reject a program that is in fact safe, but it cannot accept one that is not.
 *
 * Params:
 *    c - checker
 *    e - expression to inspect
 *
 * Returns:
 *    An upper bound on the depth of the deepest reference the value can carry.
 */
int valDepthStructural(Checker *c, Expr *e) {
    if (!e) return 0;
    int d = 0;
    switch (e->kind) {
    case EX_IDENT: {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return sy->refDepth;
        return 0;
    }
    case EX_REF:    return placeDepth(c, e->u.ref.operand);
    case EX_DEREF:  return valDepthStructural(c, e->u.deref.operand);
    case EX_SIGN:   return valDepthStructural(c, e->u.sign.operand);
    case EX_COALESCE:
        return maxInt(valDepthStructural(c, e->u.coalesce.main),
                      valDepthStructural(c, e->u.coalesce.fallback));
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, valDepthStructural(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value));
        return d;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
        return d;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        return d;
    case EX_INDEX: case EX_FIELD: return placeDepth(c, e);
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.call.args, i)));
        return d;
    case EX_METHOD:
        d = valDepthStructural(c, e->u.method.recv);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.method.args, i)));
        return d;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        return d;
    case EX_NEW: case EX_GENCALL:
        return e->arenaLevel == ARENA_HOME ? 0 : (e->arenaLevel > 0 ? e->arenaLevel : 0);
    default: return 0;
    }
}

/* True when a value of this type provably cannot contain a live reference.
 *
 * This decides whether storing a value imposes a lifetime requirement on the source
 * object. Storing `v` into `dst` has two cases:
 *
 *   - `v` is a reference: a pointer is copied in, so the pointee must live at least
 *     as long as the storage it is placed in.
 *   - `v` is a plain value: the bytes are copied, so where the source object lives
 *     and how long it lives have no effect on the stored copy. Only the references
 *     *inside* the copy matter, and those are covered by `exprRefDepth`.
 *
 * When no reference can be inside the value, the source object's depth is not a
 * constraint of the store, and promoting it would only cause a false rejection.
 *
 * Params:
 *    c - checker (type table)
 *    e - the value being stored
 *
 * Returns:
 *    True when the type provably carries no reference, so no requirement is needed.
 *
 * Notes:
 *   - Do not reduce this to inspecting the type node the user wrote: for an aggregate
 *     whose field type is itself a reference, the shallow check answers false, so the
 *     whole type must be examined (it recurses into fields and payloads).
 *   - The answer is only ever used to *drop* a requirement, so it can turn a false
 *     rejection into an acceptance, never the other way round.
 */
static bool typeCannotCarryRef(Checker *c, Expr *e) {
    if (!e) return true;
    if (mentionsParam(e->type)) return false;    /* `T` may carry a reference -> defer */
    return !typeContainsRef(c->tt, tsub(c, e->type));
}

/* Depth to record whenever a value is stored anywhere.
 *
 * This is the single entry point for store-depth accounting: it takes the maximum of
 * the reference depth and the structural upper bound, so nothing that carries a live
 * pointer is under-reported.
 *
 * The structural bound is only consulted when the value can carry a reference at
 * all. Without that guard, a plain value copy inherits the lifetime of the source
 * object, which is wrong and expensive:
 *
 *     while round < N {
 *         var it = new item      // item holds three integers and no reference
 *         outer.push(*it)        // the container stores the bytes, not the pointer
 *     }
 *
 * Taking the structural bound here would require the `new` site to live as long as
 * the caller's frame, so every iteration would allocate into the caller's home arena
 * and nothing would be reclaimed until the frame ended (measured: a 200k-iteration
 * loop grows to 98 MB instead of 1 MB). The container holds a copy of the bytes, so
 * where `it` points and how long it lives are irrelevant.
 *
 * Params:
 *    c - checker
 *    e - the value being stored
 *
 * Returns:
 *    An upper bound on the depth of the references the stored bytes can carry.
 */
int valDepthForStore(Checker *c, Expr *e) {
    int a = exprRefDepth(c, e);
    /* A plain value copy: where the source object lives and how long it lives
     * are not constraints on the store, because the bytes are copied.
     */
    if (typeCannotCarryRef(c, e)) return a;
    int b = valDepthStructural(c, e);
    return a > b ? a : b;
}

/* Depth of the references a value carries, as the checker computes it.
 *
 * This is the number the reference rule compares against the depth of the storage the
 * value is placed in, so the direction of every approximation in here matters: a larger
 * answer can only cause a false rejection, while a smaller one would accept a program
 * that lets a reference outlive its target.
 *
 * Params:
 *   c - checker
 *   e - expression to inspect
 *
 * Returns:
 *   Depth of the references in the value: 0 when it can hold none, and otherwise the
 *   depth of the storage they point at (0 = a parameter or global, so outside this frame).
 *
 * Notes:
 *   - The result is cached on the node, never for a node whose type mentions a type
 *     parameter: that answer is computed under the assumption that the parameter may
 *     carry a reference, and it holds only for the generic body, not for an instance. */
int exprRefDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* The only reason to answer without looking: this type cannot carry a reference.
     * A type that mentions a type parameter must not take this exit. Inside a generic
     * body the depth is computed under the assumption that the parameter may carry a
     * reference, and the instance check decides whether the rule applies. */
    if (!typeContainsRef(c->tt, tsub(c, e->type)) && !mentionsParam(e->type)) return 0;
    if (!c->substParams && !mentionsParam(e->type) && e->refDepth) return e->refDepth;

    int d = 0;
    switch (e->kind) {
    case EX_REF:
        d = placeDepth(c, e->u.ref.operand);
        break;
    case EX_DEREF:
        /* The value of `*p` lives where `p` points, so it has the depth of `p`. */
        d = exprRefDepth(c, e->u.deref.operand);
        break;
    case EX_SIGN:
        /* `p!` only drops nullability; it still refers to the same storage. */
        d = exprRefDepth(c, e->u.sign.operand);
        break;
    case EX_NEW:
    case EX_GENCALL:
        /* A freshly allocated object lives until the end of the enclosing block, so
         * its depth is the number the checker recorded for the site. */
        d = e->refDepth;
        break;
    case EX_COALESCE:
        /* Either side can become the result, so take the deeper one. The deeper
         * answer is the conservative one. */
        d = maxInt(exprRefDepth(c, e->u.coalesce.main),
                   exprRefDepth(c, e->u.coalesce.fallback));
        break;
    case EX_SLICE:
        d = placeDepth(c, e->u.slice.obj);
        break;
    case EX_IDENT: {
        /* A binding that holds an aggregate carrying references (a struct, array, or
         * generic instance holding a reference or a view) must be asked where those
         * references point, not how deep its own slot is:
         *
         *     var h: holder = { n: ref *p }   // p is a parameter, so depth 0
         *     return h                        // answering with the slot depth 1 used
         *                                     // to reject this valid program
         *
         * The slot depth is still the right answer for storing into the binding, which
         * is what `placeDepth` computes. The two questions are kept separate. */
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = sy->refDepth;
        else d = placeDepth(c, e);
        /* Do not stop at the `typeContainsRef` answer above: it says false for an aggregate
         * whose field types are themselves references. A binding of
         * `struct holder { p: ?ref i32 }` would then be reported as depth 0 without the field
         * table being consulted at all, so take the per-field upper bound as well, whatever
         * the type said.
         */
        if (sy) {
            for (int fi = 0; fi < sy->nfields; fi++)
                if (sy->fields[fi].depth > d) d = sy->fields[fi].depth;
            if (sy->otherDepth > d) d = sy->otherDepth;
        }
        break;
    }
    case EX_FIELD: case EX_INDEX:
        /* Read the depth recorded for the field itself. Reporting the depth of the enclosing
         * slot instead (where `h` lives) would falsely reject `h.p`, and a later `h.p = null`
         * cannot repair that, because the two answers are about different things. When there
         * is no entry for this field -- just declared, or the name does not match -- fall back
         * to the conservative place-depth answer.
         */
        if (e->kind == EX_FIELD) {
            Sym *rf = placeRoot(c, e);
            int *slot = rf ? fieldDepthEntry(c, rf, e->u.field.name, false) : NULL;
            if (slot) { d = *slot; break; }
        }
        d = placeDepth(c, e);
        break;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, exprRefDepth(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value));
        break;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
        break;
    case EX_CALL:
        /* The depth of a call result is the maximum depth of its arguments.
         *
         * Why that is a sound upper bound: the only references a callee can return come from
         * static storage (depth 0) or from its arguments (depth <= max(argument)). It cannot
         * return a reference to one of its own locals -- its own return check rejects that --
         * so there is no third source.
         *
         * Answering 0 here used to be wrong:
         *     fn identity(r: ref i32) -> ref i32 { return r }
         *     fn bad() -> ref i32 { var x: i32 = 5  return identity(ref x) }
         * which compiled and printed 0, a dangling reference. Now max(argument) = 1 > 0, so it
         * is a compile error.
         */
        for (size_t i = 0; i < e->u.call.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.call.args, i)));
        break;
    case EX_METHOD:
        d = maxInt(d, exprRefDepth(c, e->u.method.recv));   /* the receiver is an argument too */
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.method.args, i)));
        break;
    case EX_ENUMVAL:
        /* A payload construction such as `shape.holding(a[..])` puts the payload inside this
         * value, so the value's depth is the payload's depth, exactly as for an array literal.
         */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        break;
    default:
        d = 0;
        break;
    }
    /* Never cache a node whose type mentions a type parameter: that number was computed
     * under the assumption that the parameter carries a reference, and it holds only for
     * the generic body, not for an instance.
     */
    /* Diagnostic: compare the answer above with a computation that depends only on
     * lexical block depths and origin chains, never on a value the solver may still
     * change nor on a cache this function itself writes. A discrepancy where `pure` is
     * larger means the checker is under-reporting how deep the value lives, which is the
     * direction that produces dangling allocations. */
    if (getenv("EXTC_DBG_RHO")) {
        Expr *seen[40];
        int pure = exprRefDepthPure(c, e, 0, seen);
        if (pure != d)
            fprintf(stderr, "[rho] %s:%d kind=%d cached=%d pure=%d %s\n",
                    c->ctx && c->ctx->path ? c->ctx->path : "?", e->line, (int)e->kind, d, pure,
                    pure > d ? "UNDER" : "OVER");
    }
    if (!c->substParams && !mentionsParam(e->type)) e->refDepth = d;
    return d;
}

/* Defined below; every call goes through it. */

/* Whether a symbol is one of the module-level globals.
 *
 * A global and a parameter both have depth 0, but only the parameter is borrowed, so the
 * escape rules have to tell them apart.
 *
 * Params:
 *   c - checker; the global list is searched
 *   s - the symbol to look up
 *
 * Returns:
 *   True when `s` was declared at module level. */
static bool isGlobalSym(Checker *c, Sym *s) {
    for (size_t i = 0; i < c->globals.len; i++)
        if (*(Sym **)vecAt(&c->globals, i) == s) return true;
    return false;
}

/* Whether a place refers to storage owned by someone else.
 *
 * Unlike `exprBorrowed` this ignores the type of the value (the value of `*p` may
 * contain no reference at all) and asks only who owns the storage. It is used when a
 * reference is re-created, as in `ref *p`, where the question is whether the result
 * still points into the caller's frame.
 *
 * Params:
 *    c - checker
 *    e - a place expression
 *
 * Returns:
 *    True when the storage is reached through a dereference or belongs to a parameter
 *    rather than to this frame. */
static bool placeIsBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_DEREF) return placeIsBorrowed(c, e->u.deref.operand);
    if (e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_INDEX) {
        Sym *root = placeRoot(c, e);
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    return false;
}

/* Whether a value holds a reference borrowed from outside this frame.
 *
 * A reference traced back to a parameter or to a call result is borrowed: its real
 * lifetime is decided by the caller, so the callee may pass it on but must not store it
 * where it would outlive the call. A reference into a global is not borrowed, because
 * static storage outlives every frame.
 *
 * Params:
 *   c - checker
 *   e - expression to inspect
 *
 * Returns:
 *   True when the value is, or may be, borrowed.
 *
 * Notes:
 *   - The type-level exit below is the only early return allowed here. A node whose type
 *     mentions a type parameter must be walked, because the deferred check recorded for
 *     a generic body consumes this answer at instantiation. */
static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    /* The only reason to answer without looking: this type cannot carry a reference.
     * A type that mentions a type parameter must not take this exit -- the deferred check
     * recorded for a generic body needs this answer.
     */
    if (!typeContainsRef(c->tt, tsub(c, e->type)) && !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* A parameter is depth 0 and is not a global, so the value is borrowed. A global or
         * static is depth 0 as well, but anyone may store it.
         */
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    case EX_SIGN:
        /* The nullability suffix changes what the type promises, not where the value came
         * from, so the borrowed answer is carried through.
         */
        return exprBorrowed(c, e->u.sign.operand);
    case EX_COALESCE:
        /* Either side can become the result, so a borrowed value on either side makes the
         * whole expression borrowed.
         */
        return exprBorrowed(c, e->u.coalesce.main) ||
               exprBorrowed(c, e->u.coalesce.fallback);
    case EX_REF:
        /* `ref *p` re-creates a reference and would otherwise hide that it came from a
         * parameter: `b.r = ref *p` laundered the borrow in an attack test. `*p` denotes the
         * storage `p` points at, so ask whether that storage is borrowed. Note that this must
         * not recurse through `exprBorrowed`: the value of `*p` may be `i32`, and the early
         * exit above would drop the case.
         */
        if (e->u.ref.operand->kind == EX_DEREF)
            return placeIsBorrowed(c, e->u.ref.operand->u.deref.operand);
        return exprBorrowed(c, e->u.ref.operand);
    case EX_SLICE: return exprBorrowed(c, e->u.slice.obj);
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (exprBorrowed(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        /* A payload construction: if the payload is borrowed, so is the value built from it,
         * the same as for an array literal.
         */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    default: return false;      /* literals, globals, and fresh allocations belong to this frame */
    }
}

/* Whether the origin of a value can be traced back to one of `f`'s parameters.
 *
 * If it can, the lifetime obligation is handed to the call site, which knows how long
 * the corresponding argument lives. If it cannot (the value is a call result, a local
 * of this frame, or of unknown origin) the store is refused here, matching the
 * conservative treatment used for effect summaries.
 *
 * `placeRootName` resolves the root of a place: `v` stays `v`, `*p` becomes `p`, and
 * `l.head` becomes `l`.
 *
 * Params:
 *    c   - checker (currently unused, kept for symmetry with the other predicates)
 *    f   - function whose parameters are the tracing targets
 *    val - value to trace
 *
 * Returns:
 *    True when the root of the value is one of the parameters of `f`. */
bool valTracesToParam(Checker *c, FuncDef *f, Expr *val) {
    /* Only "function plus value" is needed, but the parameter is kept so that
     * this predicate has the same shape as the others. */
    (void)c;
    if (!val || !f) return false;
    const char *root = placeRootName(val);
    if (!root) return false;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, root) == 0) return true;
    return false;
}

/* Every check performed before a value is stored into a place (depth and borrows).
 */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what);   /* defined below */

/* Walk the carriers of a value and promote every allocation site inside it to the level
 * the value has to reach, instead of rejecting the store.
 *
 * `new` allocates into the arena of the current block, which is released when the block
 * ends. The most ordinary code -- building a list inside a loop -- allocates its nodes
 * in the loop body while `head` lives outside the loop, so the next iteration's block
 * exit frees them and leaves a dangling pointer (confirmed with ASan). The rule is:
 *
 *     arenaLevel = min(depth of the current block, depth of every destination)
 *
 *   - A smaller depth means a longer lifetime, so the minimum follows the destination
 *     that lives longest.
 *   - A store only into deeper places leaves the minimum unchanged, so the behaviour
 *     is bit for bit the same and no false positives are introduced.
 *   - A store into an outer place promotes the arena to that level. The worst case is
 *     a function-level heap that is cleaned up automatically, which is preferable to
 *     rejecting a correct program.
 *
 * Promotion is the safe direction: it lengthens a region rather than making the
 * recorded facts more precise. A longer lifetime cannot leave a live reference
 * dangling, whereas making the facts finer would need alias analysis and would produce
 * false rejections.
 *
 * Params:
 *   c    - checker
 *   val  - the value being stored or returned
 *   at   - depth the value must reach; 0 = must outlive this frame (the caller's arena)
 *   hops - carrier steps already taken; the walk gives up past 32 steps
 *
 * Returns:
 *   True when every `new` inside the value does reach `at`, so the caller may accept
 *   the store. False when some carrier could not be promoted, and the caller must fall
 *   back to the depth check and report an error.
 *
 * Notes:
 *   - Only the carriers of the value are walked (bindings, `!`, `??`, struct
 *     literals, enum payloads, array literals). Aliases are not followed and no
 *     interprocedural reasoning is done, so a carrier this walk cannot promote keeps
 *     the previous depth check, and this function can never let through something
 *     that ought to be rejected.
 *   - Both directions of the cached depths have to be right: the depth cached on a
 *     parent may only be tightened when every child succeeded, otherwise it records a
 *     longer lifetime than the value really has, which is a hole rather than a false
 *     rejection.
 */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops);
static bool promoteFields(Checker *c, Sym *sy, int at, int hops);
/* Promote every allocation site inside a value to the level that value has to reach.
 *
 * This is the entry point used by the store and return checks, and it starts the carrier
 * walk with no steps taken. See `promoteInto2` for the rule and for what a false return
 * obliges the caller to do.
 *
 * Params:
 *   c   - checker
 *   val - the value being stored or returned
 *   at  - depth the value must reach; 0 = must outlive this frame
 *
 * Returns:
 *   True when every `new` inside the value does reach `at`, so the store may be accepted. */
bool promoteInto(Checker *c, Expr *val, int at) { return promoteInto2(c, val, at, 0); }

/* Promote the allocation sites reachable through a binding's field table.
 *
 * `Sym.origin` records only the initializer written at the declaration, so for
 * `var h: box = { p: null, q: null }` the origin is two nulls while `x` arrives later
 * through `h.q = x`. A walk that follows only the origin therefore never reaches that
 * site, the site stays at the shallow level, and the value dangles as soon as the
 * container dies.
 *
 * Discipline -- only the promotion half is done here; lowering the accounting as well
 * was tried and turned other passing cases red:
 *   - Only an entry that has a `src` is promoted.
 *   - The level to reach is the destination's `at`: storing the whole container at
 *     level `at` means the references inside it only have to live that long, so the
 *     field table's own number (which says what the field holds right now) is not used.
 *   - That entry's accounting may only drop to `at` after the promotion succeeded. A
 *     site that cannot be promoted cannot be described, so its old accounting is kept
 *     and the outcome is a false rejection rather than a hole. An entry with no `src`
 *     at all, such as one that was never written, is left completely untouched.
 *
 * Params:
 *   c    - checker
 *   sy   - binding whose field table is walked
 *   at   - depth the container is being stored at; 0 = must outlive this frame
 *   hops - carrier steps already taken, forwarded to `promoteInto2`
 *
 * Returns:
 *   True when every field that has a `src` was promoted. False leaves the caller to
 *   report the depth error.
 */
static bool promoteFields(Checker *c, Sym *sy, int at, int hops) {
    if (!sy || hops > 32) return true;
    /* An alias may write it, so the table cannot be trusted. */
    if (sy->addressed) return true;
    bool ok = true;
    for (int i = 0; i < sy->nfields; i++) {
        Expr *src = sy->fields[i].src;
        if (!src) continue;
        /* The level to reach is min(the depth recorded for this entry, the shallowest level it
         * has ever been required to reach); the destination of this one store is not enough.
         * `out = h` is level 1, but a later `return out` needs the value to outlive the frame
         * (0), so the site eventually has to go into the home arena. Promoting only to 1 looks
         * sufficient, but level 1 is released as soon as the function returns, and ASan still
         * reported a heap-use-after-free.
         */
        if (at < sy->fields[i].minReq) sy->fields[i].minReq = at;
        int want = sy->fields[i].depth;
        if (sy->fields[i].minReq < want) want = sy->fields[i].minReq;
        if (!promoteInto2(c, src, want, hops + 1)) { ok = false; continue; }
        if (sy->fields[i].depth > want) sy->fields[i].depth = want;   /* promoted -> lower it too */
    }
    return ok;
}

/* Record the fact that the value has to fit into level `at`.
 *
 * It is recorded at the entry of `promoteInto2` because every store outwards goes
 * through there -- both `checkStoreEscape` and `checkEscape` call it first -- and that
 * call is itself the walk along the carriers of a value. Reusing it means there is no
 * second traversal that could miss a store site, because there is no second traversal
 * at all.
 *
 * Params:
 *   c   - checker; the record is appended to `c->lvlFacts`
 *   val - the value being stored or returned
 *   at  - level the value must fit into; only finite levels (at >= 1) are recorded
 *
 * Notes:
 *   - `at == 0` means "must outlive this frame" and is represented by `ARENA_HOME`,
 *     which is not a level number and must not be pushed back into one. The two agree
 *     exactly when the solved level is the home arena.
 */
static void recordLvlFact(Checker *c, Expr *val, int at);
static void applyLvlFact(Checker *c, Expr *val, int at);

/* The root origin of a value: the expression the carrier chain ends at (defined below).
 */
Expr *originOf(Checker *c, Expr *val, int hops);

/* Record the origin of a binding: the initializer of `var n = new node`, or the
 * right-hand side of `mid = n`.
 *
 * The origin must be flattened to its root before it is recorded. An intermediate
 * binding in the chain may already have left scope by the time the origin is followed,
 * `lookup` then finds nothing and the chain is broken -- seen for real with `head = mid`
 * inside a doubly nested loop. Walking to the root while the scope is still alive
 * avoids that.
 *
 * Params:
 *   c   - checker
 *   sy  - the binding being assigned or initialized
 *   val - the expression whose origin is recorded
 */
void noteOrigin(Checker *c, Sym *sy, Expr *val) {
    if (!sy || sy->addressed) return;
    /* A literal must not overwrite an origin that can already be followed.
     *
     * The checks run in source order, so for
     *     var p: ?ref i32 = null
     *     if c == 1 { p = x } else { p = null }
     * the `p = x` arm records the allocation site first. Letting the later `p = null`
     * overwrite that origin leaves only `null` behind, so a later `out = p` cannot follow
     * the chain to the site, the site stays at the block level, and the value dangles --
     * seen for real, with ASan.
     *
     * The predicate is therefore: a literal points at no site, so it may only be recorded
     * while there is no followable origin yet. The other direction matters as well: a
     * binding initialized as `= null` must record that null, because otherwise `origin`
     * stays NULL and `promoteInto2` exits early on "no origin" instead of walking the
     * branches.
     */
    bool literal = val && (val->kind == EX_NULL || val->kind == EX_INT || val->kind == EX_FLOAT ||
                           val->kind == EX_BOOL || val->kind == EX_STR);
    bool haveReal = sy->origin && sy->origin->kind != EX_NULL && sy->origin->kind != EX_INT &&
                    sy->origin->kind != EX_FLOAT && sy->origin->kind != EX_BOOL &&
                    sy->origin->kind != EX_STR;
    if (literal && haveReal) return;
    sy->origin = originOf(c, val, 0);
}

/* Walk the carriers of a value and promote the allocation sites inside it.
 *
 * `new` allocates into the arena of the current block, which is released when the block
 * ends, so a value stored somewhere that outlives its block has to be moved to an arena
 * that lives at least as long. The rule is
 *
 *     arenaLevel = min(depth of the current block, depth of every destination)
 *
 * since a smaller depth means a longer lifetime. A store only into deeper places leaves
 * the minimum unchanged, so nothing about such a store changes and no false positives are
 * introduced. Promoting is always the safe direction: it lengthens a region, and a longer
 * lifetime cannot leave a live reference dangling.
 *
 * Params:
 *   c    - checker
 *   val  - the value being stored or returned
 *   at   - depth the value must reach; 0 = must outlive this frame (the caller's arena)
 *   hops - carrier steps already taken; the walk stops past 32 steps to break cycles
 *         between bindings, which would otherwise loop forever
 *
 * Returns:
 *   True when every `new` inside the value does reach `at`. False when some carrier could
 *   not be promoted, which leaves the caller to report the depth error.
 *
 * Notes:
 *   - The cached depth on a parent node may only be tightened when every child
 *     succeeded; tightening it otherwise records a longer lifetime than the value really
 *     has, which is a hole rather than a false rejection.
 *   - This function is also the only entry point for level facts, so a case that returns
 *     early must still have walked far enough to record them. */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops) {
    if (!val) return true;
    /* A level that cannot be described: touch nothing. */
    if (at < 0) return false;
    /* A step limit is required: bindings can form a cycle (`a = b  b = a`, because an
     * assignment updates the origin), which would loop forever without a cap. Hitting the
     * cap means the value cannot be promoted, so the caller falls back to reporting the
     * error -- the safe direction. */
    if (hops > 32) return false;
    /* Record one fact in passing. This does not change any behaviour.
     *
     * Only at the outermost level (`hops == 0`): a replay re-runs exactly this level, and the
     * inner ones are reached by this call itself, so recording them too would only repeat
     * the work. */
    if (hops == 0) {
        if (getenv("EXTC_DBG_FACT"))
            fprintf(stderr, "[fact] %-8s at=%d kind=%d minAt=%d lexi=%d line=%d\n",
                    c->curFunc?c->curFunc->name:"?", at, (int)val->kind,
                    val->minAt, val->lexicalLevel, val->line);
        recordLvlFact(c, val, at); applyLvlFact(c, val, at);
    }
    switch (val->kind) {

    /* `EX_GENCALL` is `alloc<T>(n)` / `allocSlice<T>(n)`. It is exactly symmetric with `new`:
     * the same level rules apply and it is registered in the same site table, so the
     * promotion rules have to be symmetric as well. It used to fall into the default case as
     * "cannot be promoted", which left the whole family of "return a block of `alloc`ed
     * memory" unpromotable and therefore falsely rejected. */
    case EX_NEW:
    case EX_GENCALL: {
        /* A destination depth of 0 means "the caller's level", the home arena. Only a function
         * that has a home can reach that level; without one the value cannot be promoted, so
         * answer false and let the caller report the error, which is the safe direction.
         *
         * That tier is represented by `ARENA_HOME` rather than by 0, because in the current
         * encoding 0 means "not yet decided", so the value has to be translated before it is
         * written back. */
        if (at == 0 && !(c->curFunc && c->curFunc->needsHome)) return false;
        /* Reaching this point means the escape analysis has met this site by walking the
         * carriers of a value, rather than the fallback of "the enclosing function has a home". A
         * site met this way really has been required to outlive the frame, so mark it.
         *
         * This has to happen before the early exit below. The preparse has already set every
         * `new` in a function with a home to `ARENA_HOME`, which is why that exit would trigger,
         * but that is the fallback and not evidence. */
        /* Record the strongest requirement the escape analysis has made on this value; a smaller
         * level is a stronger requirement, so keep the minimum.
         *
         * A single boolean is not enough here: having been seen is not the same as being required
         * to outlive the frame. `nb = new item[cap*2]` only has to live until the end of the
         * current frame, so treating it as "goes home" would make `main`, which has no home, emit
         * `__extc_home`. */
        if (val->minAt < 0 || at < val->minAt) val->minAt = at;
        /* Already in the home arena: the home lives longest, so there is
         * nothing to promote. */
        if (val->arenaLevel == ARENA_HOME) return true;
        int target = (at == 0) ? ARENA_HOME : at;
        if (val->arenaLevel > target) val->arenaLevel = target;
        /* The one place where a level is converted back into a depth is
         * here. */
        int depth = arenaDepthOf(val->arenaLevel);
        if (val->refDepth > depth || val->refDepth == 0)
            val->refDepth = depth;
        return true;
    }

    case EX_IDENT: {
        /* Walk back along the binding's origin: `var n = new node` or `mid = n`
         * leads to that `new`. */
        Sym *sy = lookup(c, val->u.ident.name);
        if (!sy || !sy->origin) return false;
        /* The slot already outlives `at`, which means it sits at a shallower level, so
         * whatever is inside it already lives at that level. The initializer is
         * evaluated in the same statement, so its level is the slot's depth.
         *
         * A call result is the exception. The arena a callee with a home allocates
         * into is chosen at the call site and need not be the slot's level, so that
         * depth is recorded into `Sym.refDepth` when the binding is initialized (see
         * the handling of a call as an initializer in `check_stmt.c`), and that
         * record is the authority here. */
        /* This case must no longer return early.
         *
         * The old predicate said "the slot lives long enough, so no promotion is needed". That is
         * true for promotion, but `promoteInto` is also the only entry point for level facts, so
         * an early return leaves the site with no constraint at all, and the final pass puts it
         * back at its lexical level as if nothing had ever touched it.
         *
         * Measured on `fn build` in `examples/escape-promotion`: the site of
         * `var head = new node` kept `minAt = -1`, was put back at level 1, and was released as
         * soon as `build` returned, so the caller held a freed list (the generated C changed from
         * `&(*__extc_home)` to `&__extc_a[1]`). Now the walk always descends, purely to record the
         * facts, while the return value still answers "nothing to change" under the old
         * predicate. */
        bool slotDeepEnough = (sy->depth <= at);
        /* The origin is already the flattened root (see `noteOrigin`), so the chain
         * here is at most one step long and no binding is looked up twice. */
        bool ok = promoteInto2(c, sy->origin, at, hops + 1);
        /* The origin only covers the fields written at the declaration, so read the field table's
         * sources as well. A later write such as `h.q = x` exists only in the field table; see the
         * comment on `promoteFields`. */
        if (!promoteFields(c, sy, at, hops + 1)) ok = false;
        if (!ok) return false;
        if (slotDeepEnough) return true;
        if (sy->type && typeContainsRef(c->tt, tsub(c, sy->type)) && sy->refDepth > at)
            sy->refDepth = at;
        if (val->refDepth > at || val->refDepth == 0) val->refDepth = at;
        return true;
    }

    case EX_SIGN:
        return promoteInto2(c, val->u.sign.operand, at, hops + 1);

    /* Three more shapes are walked here. `promoteInto` has to be able to follow the carriers
     * of a value down to the allocations inside it, otherwise the fact that this value
     * escapes is never discovered:
     *     var v: varArray<T> = { buf: new T[cap], ... }  return v
     *     o.f = cell
     *     a[i] = cell
     * which are the three most common shapes.
     *
     * The discipline is the same as for `EX_STRUCTLIT` and `EX_ARRAYLIT`: only the carriers of
     * the value are followed, aliases are not chased and no interprocedural reasoning is done,
     * so a carrier that cannot be promoted answers false and the caller reports the error. */
    case EX_SLICE:
        return promoteInto2(c, val->u.slice.obj, at, hops + 1);
    case EX_FIELD:
        return promoteInto2(c, val->u.field.obj, at, hops + 1);
    case EX_INDEX:
        return promoteInto2(c, val->u.index.obj, at, hops + 1);

    case EX_COALESCE: {
        /* Both sides have to be tried, with no short circuit: promoting only one side
         * would miss the `new` on the other. */
        bool a = promoteInto2(c, val->u.coalesce.main, at, hops + 1);
        bool b = promoteInto2(c, val->u.coalesce.fallback, at, hops + 1);
        return a && b;
    }

    case EX_STRUCTLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.lit.inits.len; i++)
            if (!promoteInto2(c, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value, at, hops + 1))
                ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;     /* tighten the cached depth too */
        return ok;
    }

    case EX_ARRAYLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.arraylit.elems, i), at, hops + 1)) ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;
        return ok;
    }

    case EX_ENUMVAL: {                       /* a payload construction: promote the payload too */
        bool ok = true;
        for (size_t i = 0; i < val->u.enumval.args.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.enumval.args, i), at, hops + 1)) ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;
        return ok;
    }

    default:
        /* Nothing else can be promoted (a `ref` to a local, the result of a call, a slice, and so
         * on). Do not guess: fall through to the original depth check and report the error. */
        return false;
    }
}

/* Record the fact that a value has to fit into a level.
 *
 * It is recorded here, at the entry of `promoteInto2`, because every store outwards
 * passes through this function. Reusing that walk means there is no second traversal
 * that could miss a site.
 *
 * Params:
 *   c   - checker; the record is appended to `c->lvlFacts`
 *   val - the value being stored or returned
 *   at  - level the value must fit into; only finite levels (at >= 1) are recorded, since
 *         "must outlive this frame" is represented by `ARENA_HOME` rather than by a level
 *         number and must not be pushed back into one */
static void recordLvlFact(Checker *c, Expr *val, int at) {
    if (!c || !val || at < 1) return;
    /* Never record during the level solve: the solve re-enters this function, so
     * recording there would make the fact list grow without bound. */
    if (c->lvlSolving) return;
    LvlFact *f = (LvlFact *)arenaAllocZero(c->arena, sizeof(LvlFact));
    f->val = val;
    f->at  = at;
    *(LvlFact **)vecPush(&c->lvlFacts) = f;
}

/* Apply one level fact to a site, keeping the strongest requirement (the smallest
 * level).
 *
 * Recording a fact and changing a level are two different jobs, but they run in the
 * same traversal -- both at the entry of `promoteInto2` -- so no site can be missed.
 *
 * Params:
 *   c   - checker (unused; the requirement is stored on the expression)
 *   val - the value the constraint applies to
 *   at  - level the value must reach; negative values are ignored */
static void applyLvlFact(Checker *c, Expr *val, int at) {
    (void)c;
    if (!val || at < 0) return;
    if (val->minAt < 0 || at < val->minAt) val->minAt = at;
}

/* Find the root origin of a value.
 *
 * The carrier chain is followed until it reaches an expression that is not a binding with
 * a known origin, which is the site the value really came from. Flattening the chain here
 * is what keeps the origin usable later: an intermediate binding may have left scope by
 * then, and `lookup` would find nothing.
 *
 * Params:
 *   c    - checker
 *   val  - the expression whose origin is wanted
 *   hops - binding steps already taken; the walk stops past 32 steps
 *
 * Returns:
 *   The root expression, which is `val` itself when there is no followable origin. */
Expr *originOf(Checker *c, Expr *val, int hops) {
    if (!val || hops > 32) return val;
    /* A struct literal, `new`, or any other shape is its own origin. */
    if (val->kind != EX_IDENT) return val;
    Sym *sy = lookup(c, val->u.ident.name);
    /* No origin, so the answer is the value itself, which cannot be promoted. */
    if (!sy || !sy->origin) return val;
    return originOf(c, sy->origin, hops + 1);
}

/* Check that a value may be stored into a place.
 *
 * The store is refused when it would leave a dangling reference, and refused as well by
 * the borrowed-value rule below, which cannot be decided from depths alone.
 *
 * Params:
 *   c      - checker
 *   val    - the value being stored
 *   target - the place it is stored into
 *   line   - source line, for the diagnostic
 *
 * Returns:
 *   True when the store is an error.
 *
 * Notes:
 *   - Two different numbers are in play: the level the store is accounted at, which the
 *     container's own scope caps, and the depth the store is checked against, which is
 *     the depth of the storage the place denotes. Only the first is capped, because a
 *     projection such as `h.q` reports deeper than the container itself outlives, while
 *     the second is the question the reference rule actually asks. */
bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    /* The same rule as above: a value whose type mentions a type parameter defers the
     * whole check. Return here without letting the `checkEscape` below record the
     * deferred check a second time. */
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, target, placeDepth(c, target), line, "this assignment");
        return false;
    }
    /* The question here is which level the value is stored into, so this uses
     * `storeLayer` rather than `placeDepth`. For the trap with reference-typed bindings,
     * see the comment on `storeLayer`. */
    int at = storeLayer(c, target);
    /* The level this store needs is capped by the lifetime of the container.
     *
     * For a projection such as `h.q` or `a[i]`, `storeLayer` answers with the depth of the
     * path it projected, which is deeper than the container's own block level (`h.q`
     * answers 2 while `h` sits at 1). The container cannot outlive its own scope, so
     * anything stored inside it only has to live that long.
     *
     * Only this accounting number is lowered. The `at` passed to `checkEscape` below is
     * left exactly as it is. */
    int atStore = at;
    if ((int)c->scopes.len < atStore) atStore = (int)c->scopes.len;
    /* Try the promotion first. When it cannot promote anything, the call below still
     * reports the error, so this is a safety net and is idempotent. */
    promoteInto(c, val, atStore);
    bool bad = checkEscape(c, val, at, line, "this assignment");
    /* When the origin of the value can be traced to a parameter, accept the store and let
     * the call site judge the lifetime.
     *
     * The callee is compiled once and does not know the caller's region, so all it can do
     * is publish the constraint "the data behind argument j lives at least as long as the
     * container the callee stores it into". The call site is able to evaluate that
     * constraint (the `Cont(j)` case in `checkCallRefArgs`).
     *
     * A value whose origin cannot be traced (a call result, a local of this frame, an
     * unknown origin) is still rejected, which matches the `otherMask` accounting in the
     * effect summary: neither place lets it through. */
    /* Loosening the rule requires both preconditions; either one alone is not enough.
     *
     *   1. The origin of the value can be traced to a formal parameter, so the call site
     *     can compute its lifetime.
     *   2. The destination is a container reachable through a parameter, so the region of
     *     that storage is the region of the corresponding argument at the call site.
     *
     * Precondition 2 cannot be dropped: when the destination is a global (`g = s`), the
     * constraint is "the data must live forever (depth 0)", which the call site's knowledge
     * of "which container the callee will store into" cannot express, so the store stays
     * rejected. `tests/errors/escape_stash_borrowed.extc` caught exactly this hole.
     *
     * Conversely, a call at the same level is now legal. It was always safe -- the two die
     * together -- but a blanket rule used to reject it along with everything else. */
    bool destIsParam = false;
    if (target) {
        const char *droot = placeRootName(target);
        if (droot && c->curFunc)
            for (size_t pi = 0; pi < c->curFunc->params.len; pi++)
                if (strcmp((*(Param **)vecAt(&c->curFunc->params, pi))->name, droot) == 0) {
                    destIsParam = true;
                    break;
                }
    }
    if (storeLayer(c, target) == 0 && exprBorrowed(c, val)
        && !(destIsParam && valTracesToParam(c, c->curFunc, val))) {
        /* Accept the store inside a function that has a home arena. The call site has already
         * guaranteed that every `ref` / `mut ref` argument lives at least as long as this
         * call's home arena, and whatever the callee allocates goes into that same home arena,
         * so the stored value lives exactly as long as the destination.
         *
         * Without that guarantee, accepting the store outright would be a hole;
         * `ref_launder_field` caught one. */
        if (c->curFunc && c->curFunc->needsHome) return bad;
        ckError(c, line,
                "A borrowed value may not be stored where it outlives the call: its real "
                "lifetime is unknown here. Copy it, or store it into a local of this frame.",
                "cannot store a borrowed value into something that outlives this call");
        return true;
    }
    return bad;
}


/* Record a deferred rule. It is recorded only inside a generic body and only for a
 * value whose type mentions a type parameter; for a concrete type the check can be
 * decided now, and deferring would only report the error later. */
/* Record a deferred zero-value check, part of the same family as the reference rule
 * recorded by `recordRefCheck`. Like that one it applies only inside a generic body,
 * and the value is checked when the generic is instantiated. */
/* Record a deferred check on the size in `new T[n]`, a third kind alongside the
 * reference rule and the zero-value check.
 *
 * Like the others it is recorded only inside a generic body, and the size is checked
 * when the generic is instantiated.
 *
 * Params:
 *   c    - checker; the record is appended to `c->refChecks`
 *   t    - the declared element type
 *   line - source line, for the diagnostic */
void recordNewSizeCheck(Checker *c, Type *t, int line) {
    if (!c->curFunc || !c->curFunc->owner) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->isNewSize = true;
    rc->declType  = t;
    rc->line      = line;
    rc->func      = c->curFunc;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/* Record a deferred check on a zero-valued default.
 *
 * A generic body may not default a type parameter to its zero value, because whether that
 * is allowed depends on the type argument. The check is therefore recorded here and run
 * when the generic is instantiated, exactly like the reference rule.
 *
 * Params:
 *   c    - checker; the record is appended to `c->refChecks`
 *   t    - the declared type
 *   line - source line, for the diagnostic
 *   name - the name of the binding, for the diagnostic */
void recordZeroCheck(Checker *c, Type *t, int line, const char *name) {
    if (!c->curFunc || !c->curFunc->owner) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->isZero   = true;
    rc->declType = t;
    rc->line     = line;
    rc->what     = name;
    rc->func     = c->curFunc;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/* Record a deferred reference check for later instantiation.
 *
 * The check is recorded only inside a generic body whose value mentions a type
 * parameter, because a concrete value can be judged immediately and deferring it
 * would only report the error later.
 *
 * Params:
 *   c      - checker; the record is appended to `c->refChecks`
 *   val    - the value whose depth has to be checked
 *   target - the place it is stored into, or NULL for a return
 *   at     - depth the value must reach; 0 = must outlive this frame
 *   line   - source line, for the diagnostic
 *   what   - the operation being checked, named in the diagnostic
 *
 * Notes:
 *   - The depth and the borrowed flag are computed here rather than at instantiation
 *     time, because the scope is still open and `lookup` still finds the parameters. A
 *     deferred computation had no function scope left, answered 0, and silently stopped
 *     checking anything. */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what) {
    if (!c->curFunc || !c->curFunc->owner) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->val    = val;
    rc->target = target;
    rc->at     = at;
    rc->line   = line;
    rc->what   = what;
    rc->func   = c->curFunc;
    /* Compute the numbers now, while the scope is still open and `lookup` can still find
     * the parameters. The walk does not exit early for a node that mentions a type
     * parameter, so this depth is the one computed under the assumption that the parameter
     * may carry a reference, which is exactly the number the instance check needs.
     *
     * Computing it later instead was a dead end: by instantiation time the function scope
     * is gone, the depth comes out as 0, and the check silently stops firing. */
    rc->depth    = exprRefDepth(c, val);
    rc->borrowed = exprBorrowed(c, val);
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/* Whether a value is initialized with a reference borrowed from elsewhere.
 *
 * A reference obtained from a parameter, or returned by a call, cannot be stored where
 * it outlives this call -- a global, or an object reached through another parameter.
 * The compiler does not know its real lifetime, and it may point at a local of the
 * caller's frame that is nested more deeply than this function. This is one half of the
 * same rule as "the depth of a call result is the maximum depth of its arguments".
 *
 * Params:
 *   c    - checker
 *   val  - the value being initialized
 *   at   - depth of the storage it is placed in; 0 = must outlive this frame
 *   line - source line, for the diagnostic
 *   what - the operation being checked, named in the diagnostic ("this assignment")
 *
 * Returns:
 *   True when the value is (or may be) borrowed, so the caller must refuse the store
 *   unless the destination is reached through a parameter.
 */
bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    if (!val) return false;
    /* A value whose type mentions a type parameter cannot be decided now, because `T`
     * may be `i64` or `slice<u8>`. Record it and decide at instantiation, which is what
     * closes this hole. */
    if (mentionsParam(val->type)) {
        /* Promote once first, then defer.
         *
         * `promoteInto` is the only entry point for level facts, and this branch returns early,
         * so every place that returns or passes a value whose type mentions `T` would get no
         * constraint at all. Measured on `varArray<T>::withCap`: `return v` takes this branch,
         * so the `new T[cap]` inside it gets no fact (`minAt = -1`), the final pass puts it back
         * at the block level as if nothing had touched it, and the return value requires it to
         * live beyond the frame. The instance check then reports "depth 1, but this can only
         * hold up to 0", a whole family of false rejections.
         *
         * Promoting now is sound because the level does not depend on what `T` is: "this
         * storage has to live until level k" is the same statement for `T = i64` and for
         * `T = slice<u8>`.
         *
         * The return value is ignored: failing to promote is not an error, and whether to
         * reject is decided by the deferred rule recorded here. */
        promoteInto(c, val, at);
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
    /* The promotion has to run first here, the way `checkStoreEscape` has always ordered
     * it: promote, then compare depths. `promoteInto` is also the only entry point for
     * level facts, and the `d <= at` test below returns early, so as soon as the depth looks
     * shallow enough the site ends up with no constraint at all.
     *
     * Measured on `examples/escape-promotion`: on `return head`, `exprRefDepth(head)`
     * answers 0 first because the `refDepth` on that `EX_IDENT` node has not been filled
     * in yet, so the check returns early, the `new node` site keeps `minAt = -1`, the final
     * pass puts it back at its lexical level, and the caller receives a freed list.
     *
     * The order is therefore record the facts first and compare depths second; the
     * predicate itself is unchanged. */
    promoteInto(c, val, at);
    int d = exprRefDepth(c, val);
    if (d <= at) return false;
    ckError(c, line,
            "A reference may not outlive what it points to. Borrow from a parameter "
            "(depth 0) or copy the data instead. "
            "If this value is a container (varArray and friends), the storage inside it "
            "was allocated in this frame's arena: build the container in the caller's "
            "scope and fill it through a `mut ref` argument, or build it here with `new` "
            "(that allocates in this function's home arena, so it may escape).",
            "%s would hold a reference to a local variable that dies first "
            "(borrowed from depth %d, but this can only hold up to depth %d)",
            what, d, at);
    return true;
}

/* Whether walking into a place crosses a read-only reference.
 *
 * "The binding is a `let`" and "the path crosses a read-only reference" are two
 * different questions: a `var` can still hold a read-only reference, as in
 * `fn f(v: ref slice<i32>)`. A write has to satisfy both. */
/* Which references does writing this place cross? Every one of them must be a
 * `mut ref`.
 *
 * There are only two ways to cross one:
 *   - Explicitly, through `*p`.
 *   - Implicitly, when `p` in `p.f` or `v[i]` is a reference, so the storage lives
 *     behind `*p` (the automatic dereference).
 *
 * The type of the target itself is not part of this. `cur = v`, which repoints the
 * binding, writes the slot, and whether `cur` is a read-only reference has nothing to
 * do with it. An older implementation mixed the two questions together inside
 * `pathHasReadonlyRef`, which both falsely rejected `(*cell).next = v` (the field type
 * is the read-only reference `?ref node`) and missed real writes through a reference.
 *
 * Params:
 *   e       - the place expression
 *   crossed - out-parameter: set true when the storage is behind a crossed reference,
 *            in which case `placeRoot` answers NULL because the storage is inside the
 *            pointed-at object rather than in this frame
 *
 * Returns:
 *   True when every crossed reference is a `mut ref`.
 *
 * Notes:
 *   - A write through a read-only reference is not merely unsound here: the field type
 *     is checked separately by `pathHasReadonlyRef`, and the two answers are used for
 *     different diagnostics. */
bool pathRefsAllMut(Expr *e, bool *crossed) {
    if (crossed) *crossed = false;
    for (Expr *x = e; x; ) {
        if (x->kind == EX_DEREF) {                       /* an explicit crossing */
            Type *ot = x->u.deref.operand->type;
            if (!(ot && ot->kind == TY_REF && ot->mut)) return false;
            if (crossed) *crossed = true;
            x = x->u.deref.operand;
            /* Landed, so this is the storage `p` points at. */
            if (x->type && x->type->kind == TY_REF) return true;
            continue;
        }
        Expr *o = NULL;
        if (x->kind == EX_FIELD)      o = x->u.field.obj;
        else if (x->kind == EX_INDEX) o = x->u.index.obj;
        else if (x->kind == EX_SLICE) o = x->u.slice.obj;
        /* Reached a binding or another shape, so stop here. */
        if (!o) break;
        /* The field lives inside the object `o` points at. */
        if (o->type && o->type->kind == TY_REF) {
            if (!o->type->mut) return false;
            if (crossed) *crossed = true;
            return true;
        }
        x = o;
    }
    return true;
}

/* Whether a path crosses or lands on a read-only reference.
 *
 * This answers a different question from `pathRefsAllMut`: that one asks whether every
 * reference crossed is mutable, while this one also reports a read-only reference in the
 * type of the place itself, as in `c.bump()` where `c` is a `ref counter`. Both answers
 * are needed because the diagnostics differ.
 *
 * Params:
 *   e - the place expression
 *
 * Returns:
 *   True when the place, or any reference crossed on the way to it, is read-only. */
bool pathHasReadonlyRef(Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->type && x->type->kind == TY_REF && !x->type->mut) return true;
        /* An explicit crossing, as in `(*p).f` or `(*p).f.g`.
         *
         * The older implementation stopped at `EX_DEREF`, but a dereference has the type of the
         * pointed-at object rather than a reference type, so the mutability of that reference
         * was never tested and a write through a read-only reference was let through entirely.
         * Measured: `fn g(p: ref node) { (*p).val = 7 }` compiled. */
        if (x->kind == EX_DEREF) {
            Type *ot = x->u.deref.operand->type;
            if (ot && ot->kind == TY_REF && !ot->mut) return true;
            x = x->u.deref.operand;
            /* Landed on a binding, so nothing is crossed here. */
            if (x->type && x->type->kind == TY_REF) return false;
            continue;
        }
        if (x->kind == EX_FIELD) { x = x->u.field.obj; continue; }
        if (x->kind == EX_INDEX) { x = x->u.index.obj; continue; }
        if (x->kind == EX_SLICE) { x = x->u.slice.obj; continue; }
        break;
    }
    return false;
}

/* Check that a place may be written at all.
 *
 * Three separate reasons can make a write illegal, and each gets its own diagnostic: a
 * write through `*p` needs `mut ref`, a write through a read-only reference or view needs
 * the type to carry `mut`, and reassigning a `let` binding is forbidden outright.
 *
 * Params:
 *   c    - checker
 *   e    - the place being written
 *   line - source line, for the diagnostic
 *   what - the operation being checked, named in the diagnostic
 *
 * Returns:
 *   True when the write was refused and an error was reported. */
bool requireMutable(Checker *c, Expr *e, int line, const char *what) {
    /* `*p` needs its own branch, because `placeRoot` does not recognise it and returns
     * NULL, which would then be treated as writable.
     *
     * Writability comes from the type of the reference: only `mut ref` permits `*p = v`. */
    if (e && e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        if (ot && ot->kind == TY_REF && ot->mut) return false;
        ckError(c, line,
                "`*p = v` needs `p` to be `mut ref T`; a `ref T` is a read-only borrow",
                "cannot %s through a read-only reference", what);
        return true;
    }
    /* The question here is whether a write can cross, so what matters is the mutability of
     * the reference at every step:
     *   - The type of this place itself is a read-only reference (`c` in `c.bump()` is a
     *     `ref counter`).
     *   - The path crosses a read-only reference explicitly (`(*p).f`).
     *
     * `pathHasReadonlyRef` answers both, and the `EX_DEREF` case above is exactly what it
     * was missing. */
    if (pathHasReadonlyRef(e)) {
        ckError(c, line,
                "`ref T` is a **read-only** borrow; writing through it needs `mut ref T` "
                "in the declaration. Read-only is the default so that a signature says "
                "what it does.",
                "cannot %s through a read-only reference", what);
        return true;
    }
    Sym *root = placeRoot(c, e);
    /* Whether a view's elements can be written depends on whether the view's type carries
     * `mut`. This closes the hole of a view passed by value:
     *     fn f(v: slice<i32>) { v[0] = 1 }   // the parameter is a copy, but the
     *                                         // elements belong to the caller!
     * Writing requires `mut slice<i32>` (or `mut ref slice<i32>`) in the signature. */
    if (root && root->type && root->type->kind == TY_GENERIC &&
        ttIsViewType(root->type) && !root->type->mut &&
        /* The view's writability governs writes to its elements; assigning the
         * whole view (rebinding it) is not governed by it. */
        !(e->type && ttIsViewType(e->type))) {
        ckError(c, line,
                "a view is read-only unless its type carries `mut`. Writing through a "
                "by-value view would change the caller's data without the signature "
                "saying so.",
                "cannot %s through `%s`: it is a read-only view `%s`",
                what, root->name, typeStr(c, root->type));
        return true;
    }
    if (!root || root->mut) return false;
    if (e->kind == EX_IDENT) {
        ckError(c, line, "use `var` to allow reassignment (`let` is an immutable binding)",
                "cannot assign to `%s`, which is a `let`", root->name);
    } else {
        ckError(c, line,
                "`let` means the value is read-only: no reassignment, no field or element writes. "
                "Use `var` for a value you intend to write through.",
                "cannot %s through `%s`, which is a `let`", what, root->name);
    }
    return true;
}

/* Whether a type is a view, and if so its element type.
 *
 * The view protocol is a `data` member plus a `len` member. The compiler recognises
 * that protocol, while the structure itself and its methods live in
 * `stdlib/prelude.extc`: the language knows the protocol, and the library provides the
 * methods.
 *
 * Returns:
 *   The element type when `t` is a view, NULL otherwise. */
Type *viewElemOf(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return NULL;
    if (strcmp(t->sdef->name, "slice") != 0) return NULL;
    if (t->targs.len != 1) return NULL;
    return *(Type **)vecAt(&t->targs, 0);
}

/* Whether C itself can compare this type: numbers, `bool`, and enums.
 *
 * `str` is deliberately not in this set, because its `==` would degrade into a pointer
 * comparison, and an `eq` method is required instead.
 *
 * Returns:
 *   True when a C `==` on this type compares values rather than addresses. */
bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    /* An enum with no payload is an integer in C, so it can be compared directly.
     *
     * One with a payload cannot: in C it is a `struct { tag; union }`, and C structs do not
     * support `==`. Use `match` instead. A derived `_eq` would have to compare the payloads
     * recursively and is not done for now. */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
}

/* Find an operator method defined on a type.
 *
 * `!=` is the special case: when there is no `!=` method, the lookup falls back to `==`
 * and negates it, which codegen does.
 *
 * Params:
 *   b        - the type, with any wrapper already stripped
 *   sym      - the method name to look for (`==` or `!=`)
 *   fallback - method name to try when `sym` is absent, or NULL for none
 *
 * Returns:
 *   The method definition, or NULL when the type defines neither name. */
FuncDef *findOp(Type *b, const char *sym, const char *fallback) {
    FuncDef *m = findMethod(b, sym);
    if (!m && fallback) m = findMethod(b, fallback);
    return m;
}

/* Check the signature of an operator method at its definition site.
 *
 * The signature used to be checked only at the use site, and that caused two problems:
 * a `fn !=` with a wrong signature that happened to be unused was never checked at all,
 * and a generic `a != b` deferred to instantiation let codegen find a `void`-returning
 * `!=` and hand the user C's own "invalid use of void expression". Checking at the
 * definition makes every path of use safe.
 *
 * Params:
 *   c - checker
 *   f - the function definition being declared
 *
 * Notes:
 *   - Only `==` and `!=` are checked; any other name is left alone, so a method such as
 *     `compare` may return whatever it likes. */
void checkOperatorSig(Checker *c, FuncDef *f) {
    bool isOp = (strcmp(f->name, "==") == 0 || strcmp(f->name, "!=") == 0);
    if (!isOp || !f->owner) return;

    const char *want = "signature must be `fn ==(self: ref T, other: T) -> bool`";

    /* Why the result must be `bool`, which follows from three premises rather than from
     * taste:
     *   1. `a == b` is naturally used inside `if`, `while`, `&&` and `||`.
     *   2. extC has no implicit truthiness; `if 1` is an error.
     *   3. `!=` is implemented by negating `==`.
     * So `==` can only return `bool`; anything else could not be used in a condition. To
     * return something else, use a different method name such as `compare` or `diff`, which
     * carry no restriction. */
    const char *why =
        "`a == b` gets used in `if` / `&&` / `||`, and extC has no implicit truthiness; "
        "`!=` is also derived by negating `==`. "
        "To return something else, use a different method name (`compare` / `diff` etc.) -- those are unrestricted.";

    if (f->params.len != 2) {
        ckError(c, f->line, want, "operator `%s` must take exactly 2 parameters", f->name);
        return;
    }
    if (!f->ret || !ttIs(f->ret, "bool")) {
        ckError(c, f->line, why, "operator `%s` must return `bool`", f->name);
        return;
    }
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    Param *p1 = *(Param **)vecAt(&f->params, 1);

    if (p0->type->kind != TY_REF) {
        ckError(c, p0->line, want, "`self` of operator `%s` must be a reference", f->name);
        return;
    }
    Type *b0 = ttBase(p0->type);
    Type *b1 = ttBase(p1->type);
    if (!b0 || b0->sdef != f->owner || !b1 || b1->sdef != f->owner) {
        ckError(c, f->line, want,
                "both operands of operator `%s` must be `%s`", f->name, f->owner->name);
    }
}

/* Whether this type can be compared with `op`.
 *
 * The signature was already validated at the definition site, so finding the method is
 * enough here.
 *
 * Params:
 *   t  - the type being compared
 *   op - the operator name, `==` or `!=`
 *
 * Returns:
 *   True when a comparison is available for this type. */
bool typeSupportsEq(Type *t, const char *op) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (cmpIsNative(t)) return true;
    /* the compiler derives `==` for arrays, provided the elements can be compared */
    if (t->kind == TY_ARRAY) return typeSupportsEq(t->inner, op);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(b, op, strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
}

/* `?` is legal in only three places, so those three go through this entry point;
 * anywhere else an EX_TRY node is an error. */

/* Check an expression in value position, where `ref T` is used as `T`.
 *
 * The division of labour with `checkExpr`:
 *   `checkValue` - the value is what is wanted here, so `p` means the object `p`
 *                  points at, and the node is marked as dereferenced
 *   `checkExpr`  - the place, or the reference itself, is what is wanted here: an
 *                  assignment target, the operand of `ref x`, the base of a field,
 *                  index or slice, and a method receiver, all of which take an address
 *
 * Permissions are not part of this; whether a write is allowed is decided by `ref`
 * versus `mut ref`.
 *
 * Params:
 *   c - checker
 *   e - the expression in value position
 *
 * Returns:
 *   The type of the value, with a reference type replaced by its target. */
Type *checkValue(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    /* `ref x` asks for a reference explicitly, so it is not automatically dereferenced:
     * doing so would cancel the reference that was just taken, which is self-contradictory.
     * `let r = ref n` therefore yields a reference, while `let y = r` yields the value `r`
     * points at. */
    /* `ref x` and `alloc<T>(n)` both ask for a reference explicitly, so neither is
     * automatically dereferenced: doing so would cancel the reference that was just
     * requested, which is self-contradictory. */
    /* There is no implicit dereference any more: giving a reference in value position is
     * an error, and the diagnostic tells the user to write `*p`.
     *
     * The exceptions are `ref x` and `alloc`, which ask for a reference in the first place,
     * and member selection (`p.field` / `p.method()`), which navigates rather than reading
     * a value and therefore still sees through the reference.
     *
     * Params:
     *   c - checker
     *   e - the expression in value position
     *
     * Returns:
     *   The value type, with a reference type replaced by its target. */
    if (t && t->kind == TY_REF && e->kind != EX_REF && e->kind != EX_GENCALL) {
        ckError(c, e->line,
                t->nullable
                  ? "it is a nullable reference (`?ref T`) -- check it first:"
                    " `if p != null { ... *p ... }`"
                  : "write `*p` where the value is needed (`*p + 1`, `let v = *p`, `f(*p)`)",
                "`%s` is a **reference**, not a value -- dereference it first: `*p`",
                typeStr(c, t));
        return t->inner;
    }
    return t;
}

/* Whether an argument or field initializer may be automatically dereferenced.
 *
 * Where a reference is expected, an expression is not dereferenced automatically,
 * because the reference itself is what gets placed there (`{ data: ref n, ... }`,
 * `f(ref c)`) rather than the value it points at. Everywhere else the expression is
 * treated as a value position. */

Type *checkInto(Checker *c, Type *want, Expr *e) {
    /* An expected type of `option<...>` or `result<...>` also accepts a bare constructor,
     * so `f(some(3))` and `var x: ?i32 = some(3)` work: the type comes from the context and
     * the full name need not be written out. */
    if (want) desugarBareCtor(c, e, want);
    Type *got = checkExpr(c, e);
    /* As in `checkValue`: a reference supplied where no reference is expected is an
     * error. */
    if (got && got->kind == TY_REF && (!want || want->kind != TY_REF)) {
        ckError(c, e->line,
                got->nullable ? "it is a nullable reference (`?ref T`) -- check it first:"
                                " `if p != null { ... *p ... }`"
                              : "write `*p` here",
                "`%s` is a **reference**, not a value -- dereference it first: `*p`",
                typeStr(c, got));
        return got->inner;
    }
    return got;
}

/* Check an argument of `println` / `print`, where a reference is dereferenced
 * automatically.
 *
 * Printing a reference can only mean printing the value it points at, and exposing the
 * address itself would be both meaningless and undesirable. This is the only remaining
 * value position that keeps automatic dereferencing.
 *
 * Params:
 *   c - checker
 *   e - the argument expression
 *
 * Returns:
 *   The type of the printed value, with a reference type replaced by its target. */
Type *checkPrintArg(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    if (t && t->kind == TY_REF) {
        e->deref = true;
        return t->inner;
    }
    return t;
}

/* Check an expression that may be a `?` operator.
 *
 * `?` is only legal in three statement positions, so those three go through here and
 * everything else is checked as an ordinary value position.
 *
 * Params:
 *   c - checker
 *   e - the expression, possibly an `EX_TRY` node
 *
 * Returns:
 *   The type of the unwrapped payload for `?`, otherwise the value type. */
Type *checkMaybeTry(Checker *c, Expr *e) {
    if (e && e->kind == EX_TRY) return checkTryInner(c, e);
    return checkValue(c, e);
}

/* `==` inside a generic body is checked when the generic is instantiated rather than
 * here. */

/* Whether a type is one of the two prelude containers that carry real semantics,
 * recognised by name and argument count.
 *
 * They are reserved definitions that a user may not redefine, so recognising them by
 * name is safe. `?` needs their tag field names, which is the same division of labour
 * as the view protocol of `data` plus `len`: the language knows the protocol, and the
 * library provides the structure.
 *
 * Params:
 *   t     - the type to test
 *   name  - the reserved container name (`option` or `result`)
 *   nargs - the number of type arguments it must carry
 *
 * Returns:
 *   True when `t` is that container with exactly that many type arguments. */
bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* a generic struct: `slice<T>`, `varArray<T>` */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* A generic enum: `option<T>` and `result<T,E>` are enums, and an instance has the
     * type `TY_ENUM` carrying concrete type arguments. */
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/* Check `e?`, which forwards a failure to the caller.
 *
 * It is legal in only three statement positions, because C has no statement expressions
 * and the operator has to be expanded into statements. Those three are therefore the
 * only callers of this function; `checkExprInner` reports an error for an `EX_TRY` node
 * anywhere else.
 *
 * Params:
 *   c - checker
 *   e - the `?` expression
 *
 * Returns:
 *   The payload type of the `option` or `result` being unwrapped, or the error type when
 *   the operand or the enclosing return type is unsuitable. */
Type *checkTryInner(Checker *c, Expr *e) {
    Type *ot = checkExpr(c, e->u.try_.operand);
    if (ttIsError(ot)) return ttError(c->tt);
    Type *ob = ttBase(ot);

    Type *rt = (c->curFunc && c->curFunc->ret) ? ttBase(c->curFunc->ret) : NULL;
    /* The spelling of the enclosing return type, used in the diagnostic. */
    const char *fw = NULL;

    if (isProtoType(ob, "option", 1)) {
        if (!isProtoType(rt, "option", 1)) {
            fw = "option<...>";
            goto mismatch;
        }
    } else if (isProtoType(ob, "result", 2)) {
        if (!isProtoType(rt, "result", 2)) {
            fw = "result<..., E>";
            goto mismatch;
        }
        /* The error types have to be identical; otherwise forwarding the failure would
         * encode a conversion that does not exist. */
        Type *oe = *(Type **)vecAt(&ob->targs, 1);
        Type *re = *(Type **)vecAt(&rt->targs, 1);
        if (!ttEquals(oe, re)) {
            ckError(c, e->line,
                    "`?` forwards the failure as-is, so it cannot change the error type. "
                    "Use the same `E` in the return type.",
                    "`?` here would change the error type from `%s` to `%s`",
                    typeStr(c, oe), typeStr(c, re));
            return ttError(c->tt);
        }
    } else {
        ckError(c, e->line, "`?` works on the `option` / `result` from the prelude.",
                "`?` needs an `option<...>` or `result<...>`, found `%s`",
                typeStr(c, ot));
        return ttError(c->tt);
    }

    Type *payload = *(Type **)vecAt(&ob->targs, 0);
    e->type = payload;
    return payload;

mismatch:
    ckError(c, e->line,
            "`?` returns the failure from the enclosing function, so the two must be "
            "the same kind.",
            "`?` on `%s` needs the enclosing function to return `%s`, but it returns `%s`",
            typeStr(c, ot), fw,
            c->curFunc && c->curFunc->ret ? typeStr(c, c->curFunc->ret) : "void");
    return ttError(c->tt);
}

/* A depth computed only from lexical block depths and origin chains.
 *
 * This is the diagnostic baseline for `EXTC_DBG_RHO`. It deliberately ignores two inputs
 * that the real `exprRefDepth` depends on:
 *
 *   - the arena level of an allocation site, which the level solver may still change and
 *     which holds a provisional marker while the checker runs. The block depth the site
 *     was born at (`lexicalLevel`) is used instead, since it is stable and the solver can
 *     only give a site a *longer* lifetime than the shallowest block it could occupy;
 *   - the depth cached on a node, which `exprRefDepth` itself writes and which can be
 *     left over from an earlier traversal after a binding was retargeted.
 *
 * Where the two disagree with `pure` larger, the checker believes a value lives closer to
 * the end of the frame than it really does, and a store can then be skipped instead of
 * promoting the allocation site.
 *
 * Params:
 *   c    - checker (scope stack and type table)
 *   e    - expression to measure
 *   hops - recursion guard for cyclic origin chains (`a = b  b = a` is legal)
 *   seen - nodes already visited on this chain
 *
 * Returns:
 *   An upper bound on the depth of the references the value can carry.
 */
static int exprRefDepthPure(Checker *c, Expr *e, int hops, Expr **seen) {
    if (!e || hops >= 40) return 0;
    for (int i = 0; i < hops; i++) if (seen[i] == e) return 0;
    seen[hops] = e;

    switch (e->kind) {
    case EX_NEW:
    case EX_GENCALL:
        /* The block the site was born in. `lexicalLevel` is set the first time the node
         * is checked and does not move afterwards. */
        return e->lexicalLevel > 0 ? e->lexicalLevel : 0;
    case EX_IDENT: {
        /* Follow the origin to the site it came from; a binding on its own carries no
         * depth that is independent of where that site ends up. */
        Sym *sy = identBindOf(e);
        if (!sy || !sy->origin) return 0;
        return exprRefDepthPure(c, sy->origin, hops + 1, seen);
    }
    case EX_FIELD: {
        /* The depth recorded for that field, falling back to the object. */
        Sym *root = placeRoot(c, e);
        if (root) {
            for (int i = 0; i < root->nfields; i++)
                if (root->fields[i].name &&
                    strcmp(root->fields[i].name, e->u.field.name) == 0)
                    return root->fields[i].depth;
        }
        return exprRefDepthPure(c, e->u.field.obj, hops + 1, seen);
    }
    case EX_INDEX:
        return exprRefDepthPure(c, e->u.index.obj, hops + 1, seen);
    case EX_DEREF:
    case EX_SIGN:
        /* `*p` and `p!` name the same storage as their operand. */
        return exprRefDepthPure(c, e->kind == EX_DEREF ? e->u.deref.operand
                                                       : e->u.sign.operand, hops + 1, seen);
    case EX_REF:
        /* A reference created here points into the current block. */
        return e->lexicalLevel > 0 ? e->lexicalLevel : 0;
    case EX_SLICE:
        /* A view carved out of a place lives as long as that place's block. */
        return exprRefDepthPure(c, e->u.slice.obj, hops + 1, seen);
    case EX_COALESCE: {
        int a = exprRefDepthPure(c, e->u.coalesce.main, hops + 1, seen);
        int b = exprRefDepthPure(c, e->u.coalesce.fallback, hops + 1, seen);
        return maxInt(a, b);
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, exprRefDepthPure(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value,
                                           hops + 1, seen));
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, exprRefDepthPure(c, *(Expr **)vecAt(&e->u.arraylit.elems, i),
                                           hops + 1, seen));
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, exprRefDepthPure(c, *(Expr **)vecAt(&e->u.enumval.args, i),
                                           hops + 1, seen));
        return d;
    }
    case EX_CALL:
    case EX_ASSOC: {
        /* A call can only return a reference that came from its arguments or from static
         * storage, so the deepest argument is the bound. */
        Vec *args = e->kind == EX_CALL ? &e->u.call.args : &e->u.assoc.args;
        int d = 0;
        for (size_t i = 0; i < args->len; i++)
            d = maxInt(d, exprRefDepthPure(c, *(Expr **)vecAt(args, i), hops + 1, seen));
        return d;
    }
    case EX_METHOD: {
        int d = exprRefDepthPure(c, e->u.method.recv, hops + 1, seen);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, exprRefDepthPure(c, *(Expr **)vecAt(&e->u.method.args, i),
                                           hops + 1, seen));
        return d;
    }
    default:
        /* Scalars, string literals, `null`, `true`/`false`: nothing to point at. */
        return 0;
    }
}
