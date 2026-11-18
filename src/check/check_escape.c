/**
 * @file check_escape.c
 * @brief Escape and lifetime analysis: whether a borrow may outlive the storage it points at,
 *        and which arena an allocation has to live in.
 *
 * The pass is driven by the one reference rule and by the per-expression depth recorded for every
 * value. Depth 0 means the value lives outside this frame (a parameter, a global, static
 * storage); every nested block adds 1, so a smaller depth means a longer life.
 */

#include "dbg.h"
#include "check_internal.h"
#include "plan.h"          /* Plan and analysis products of the checking passes, read here through their accessors. */
#include <stdlib.h>
#include <stdio.h>

/* Defined further down, but the value-position rule that calls it comes first. */
static bool rejectNoCopy(Checker *c, Expr *e, Type *t);
void warnSharedCopy(Checker *c, Expr *e, Type *t);

/* Hop limit for one promotion walk. It is only the **backstop for cycles**: the real cycle
 * guard is `Checker.fsChain`, which stops a given binding precisely (one binding is walked once
 * per walk). The number therefore has to fit legitimate long chains -- one binding takes
 * `EX_IDENT` + `promoteFieldsAt` + the carrier steps of its origin expression, and 32 is not
 * enough for a program with dozens of bindings; hitting the limit goes through
 * `EXTC_DBG_FALLBACK`, and an assertion build reports red immediately. */
#define EXTC_PROMOTE_MAX_HOPS 512

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
 *     analysis and is checked at the call site instead (by the pass that runs there).
 */

/**
 * @brief Larger of two integers.
 */
static int maxInt(int a, int b) { return a > b ? a : b; }

static bool isGlobalSym(Checker *c, Sym *s);   /* defined below; used by storeLayer */

/* Diagnostic baseline for `EXTC_DBG_RHO` (defined at `targetDepthPure`): it depends only on
 * lexical block depth and the origin chain and reads neither the level solver nor the cache, so
 * it can be used as a reference without changing its own answer. */
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen);

/**
 * @brief Tell whether an expression's storage provably lives outside every frame.
 *
 * Two sources, both authored rather than guessed: a chain rooted at a **global** (static
 * storage outlives every frame), and a call whose declaration carries `effects Ret=0` -- "the
 * reference/view I return does not point into the caller's frames", recorded as
 * `Sym.outOfFrame` so that a view may then answer with its pointee's depth. Everything else
 * answers no, which keeps the rule conservative: `new`, an arena allocation, a stack array, or
 * a callee without that declaration may all die with the frame. String literals live in static
 * storage and last as long as a global => depth 0.
 *
 * @param c  Checker.
 * @param e  Binding initializer or right-hand side.
 * @return   `true` when the storage certainly outlives this frame, `false` otherwise.
 */
bool exprOutOfFrame(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* **String literals live in static storage**: they last as long as a global (C gives them
     * static storage duration) => depth 0. This must not fall into the default branch and answer
     * false: that would misjudge `let k: slice<u8> = "…"` across a `yield` as "pointing at
     * somebody else's stack". */
    case EX_STR:   return true;
    case EX_SIGN:  return exprOutOfFrame(c, e->u.sign.operand);      /* `p!` only drops nullability */
    case EX_TRY:   return exprOutOfFrame(c, e->u.try_.operand);
    case EX_CONV:  return exprOutOfFrame(c, e->u.conv.operand);
    case EX_COALESCE:
        return exprOutOfFrame(c, e->u.coalesce.main) &&
               exprOutOfFrame(c, e->u.coalesce.fallback);
    case EX_IDENT: {
        Sym *sy = lookup(c, e->u.ident.name);
        if (!sy) return false;
        if (isGlobalSym(c, sy)) return true;          /* static storage: depth 0, for real */
        return sy->outOfFrame;                        /* propagated from its own initializer */
    }
    case EX_SLICE:  return exprOutOfFrame(c, e->u.slice.obj);   /* `mem[..]`: the view's storage */
    case EX_FIELD: return exprOutOfFrame(c, e->u.field.obj);
    case EX_INDEX: return exprOutOfFrame(c, e->u.index.obj);
    case EX_DEREF: {
        /* `*p` lives where `p` points; a pointer that is out of frame says so itself. */
        Expr *p = e->u.deref.operand;
        return p && p->type && tsub(c, p->type)->kind == TY_REF &&
               exprOutOfFrame(c, p);
    }
    case EX_CALL: case EX_METHOD: case EX_ASSOC: {
        /* A return value whose declaration is signed (`effects Ret=0`): it does not point into
         * the caller's frames. A slot on a table field carries the same clause. */
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_FIELD) {
            FieldDef *fd = planField(e->u.call.callee);
            if (fd && fd->hasEffects && fd->effRetFresh) return true;
        }
        if (e->kind == EX_METHOD && e->u.method.recv && e->u.method.recv->kind == EX_FIELD) {
            FieldDef *fd = planField(e->u.method.recv);
            if (fd && fd->hasEffects && fd->effRetFresh) return true;
        }
        /* The substrate view constructor is `&p[0..n]` by construction, so the view it makes
         * inherits the lifetime of `p`: a view over a reservation -- or over any other storage
         * that outlives every frame -- outlives every frame as well. This is a fact of the
         * compiler-level primitive (`extc_viewOf`), not a guess about a library function: the
         * SSO `string` builds its big buffer this way, and without the propagation the binding
         * below answers "in frame", the store into `self.big` has no attributable source, and
         * every `push`/`append` caller inherits the strictest call-site rule. */
        if (exprIsCallKind(e)) {
            const char *nm = NULL;
            Vec *as = NULL;
            switch (e->kind) {   /* the argument vector lives in a different member per call shape */
                case EX_CALL:
                    nm = (e->u.call.callee && e->u.call.callee->kind == EX_IDENT)
                             ? e->u.call.callee->u.ident.name : NULL;
                    as = &e->u.call.args;
                    break;
                case EX_METHOD: nm = e->u.method.name; as = &e->u.method.args; break;
                case EX_ASSOC:  nm = e->u.assoc.name;  as = &e->u.assoc.args;  break;
                default: break;
            }
            const char *last = nm ? strrchr(nm, '$') : NULL;
            if (last ? strcmp(last + 1, "extc_viewOf") == 0 : (nm && strcmp(nm, "extc_viewOf") == 0)) {
                if (as && as->len >= 1 && exprOutOfFrame(c, *(Expr **)vecAt(as, 0))) return true;
            }
        }
        return planCallee(e) && planCallee(e)->extRetFresh;
    }
    default: return false;
    }
}

/* `slotDepth` asks "how deep is the storage", `targetDepth` asks "where does the reference
 * point": two different questions with different answers. */

/**
 * @brief Depth of the storage a place expression denotes.
 *
 * A local gets its block depth; a parameter or a global gets 0 (outside this frame); a
 * reference-typed binding gets the depth of the object it points at rather than the depth of its
 * own slot; `*p` does not create storage, so the lifetime in question is the reference's own. A
 * shape whose root cannot be named falls back to `targetDepth`, so the fallback lands on the
 * longer-lived side instead of recording depth 0.
 *
 * @param c  Checker, providing the scope stack and the type table.
 * @param e  Place expression.
 * @return   Lexical depth of the storage; 0 when `e` is NULL.
 */
int slotDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* Dereference does not create storage; it only names storage through a pointer,
     * so the lifetime in question is that of the reference. */
    if (e->kind == EX_DEREF) return targetDepth(c, e->u.deref.operand);
    /* A reference-typed binding denotes the storage it points at, not its own slot
     * (`var cur: ?ref node = head`: the slot is in this frame, the node is outside). */
    if (e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type) {
            Type *st = tsub(c, sy->type);
            if (st && st->kind == TY_REF) return anRefDepth(sy);
            /* A **view** whose storage is out of this frame denotes that storage, not the slot
             * it sits in: `var v = pl.view(off, n)!` is a two-word handle whose bytes point into
             * the plate, so `v.data` must not be read as "a pointer into this frame".
             * `outOfFrame` is set at binding time by a global or by a call with `effects
             * Ret=0`; a view on storage in this frame, a `new` block, or an arena allocation
             * keeps the slot depth, which is what the conservative rule wants. */
            if (st && ttIsViewType(st) && sy->outOfFrame) return anRefDepth(sy);
        }
        return sy ? sy->depth : 0;
    }
    /* A field or element lives inside the object that contains it. */
    if (e->kind == EX_FIELD) return slotDepth(c, e->u.field.obj);
    if (e->kind == EX_INDEX) return slotDepth(c, e->u.index.obj);
    Sym *root = placeRoot(c, e);
    /* A shape `placeRoot` cannot name is not "storage that lives forever" -- reading it as 0
     * points the conservative direction exactly backwards: that storage dies with this frame,
     * while 0 means "outlives this frame", so `G = [v, v][0]` would be accepted. What the
     * unnamed shape has to be asked is what the other checks ask of a value: how deep is the
     * thing it **points at** (`targetDepth`).
     *
     * This does not loop back here: `targetDepth` consults `slotDepth` only for shapes that are
     * themselves place expressions, and this branch is precisely the one where `placeRoot`
     * already answered NULL (INV-U: the fallback records the longer-lived requirement, never the
     * shorter one). */
    return root ? root->depth : targetDepth(c, e);
}

/**
 * @brief Tell whether a store target is reached through a **named reference** (`(*cur).f` /
 *        `*cur`), and return that binding if so.
 *
 * This is kept apart from `destDerefDepth`: the shape test (whether the decision has to be
 * deferred) and the depth value (`symMinPointee`, which may not be recorded yet) are two
 * different things -- the lexical-order problem is exactly the moment where "the shape is
 * already known but the depth is not yet recorded".
 *
 * @param c  Checker.
 * @param e  Place expression of the store target.
 * @return   The binding when the path ends in `*` followed by a named reference; NULL otherwise.
 */
Sym *destDerefSym(Checker *c, Expr *e) {
    for (; e; ) {
        if (e->kind == EX_FIELD) { e = e->u.field.obj; continue; }
        if (e->kind == EX_INDEX) { e = e->u.index.obj; continue; }
        if (e->kind == EX_SLICE) { e = e->u.slice.obj; continue; }
        if (e->kind == EX_DEREF) {
            Expr *o = e->u.deref.operand;
            if (!o || o->kind != EX_IDENT) return NULL;
            Sym *sy = lookup(c, o->u.ident.name);
            return (sy && sy->type && sy->type->kind == TY_REF) ? sy : NULL;
        }
        return NULL;
    }
    return NULL;
}

/**
 * @brief The shallowest depth a reference binding's pointee may have: the one recorded on the
 *        binding, plus the alias chain followed when the declaration is a copy.
 *
 * In `var ocur: ?mut ref node = cur`, `ocur` and `cur` **share the pointee**, but `ocur`'s depth
 * snapshot is the lexical value at the moment of the copy (`cur` is still `null` in the first
 * iteration => 0), while `cur`'s `minPointee` is only complete once the walk has finished. So
 * the alias chain is walked and the min is taken: it can only move towards shallower depths,
 * which is the safe direction.
 *
 * @param c     Checker.
 * @param sy    Reference binding.
 * @param hops  Alias-chain hops so far; bounded by `EXTC_PROMOTE_MAX_HOPS`.
 * @return      The shallowest depth; -1 when nothing was ever recorded. Running out of hops
 *              answers 0 ("must outlive this frame"), the shallowest possibility, because the
 *              minimum is the safe direction and stopping early would enlarge the answer.
 */
static int symMinPointee(Checker *c, Sym *sy, int hops) {
    if (!sy) return -1;
    if (hops > EXTC_PROMOTE_MAX_HOPS) { EXTC_DBG_FALLBACK("symMinPointee: hop budget exhausted"); return 0; }
    int m = sy->minPointee;
    if (sy->initSrc && sy->initSrc->kind == EX_IDENT) {
        Sym *src = lookup(c, sy->initSrc->u.ident.name);
        if (src && src != sy) {
            int m2 = symMinPointee(c, src, hops + 1);
            if (m2 >= 0 && (m < 0 || m2 < m)) m = m2;
        }
    }
    return m;
}

/**
 * @brief Shallowest depth the pointee of a write target may have when the target is reached
 *        through `*`; -1 when the shape does not match (the caller falls back to `slotDepth`).
 *
 * The destination of `(*cur).f = v` is not the slot `cur` but the object `cur` may point at.
 * `cur` may point at a parameter (0) or at a `new` in this frame (1), and the destination is
 * taken from the **longest-lived** possibility (min), because the stored value has to live as
 * long as the worst-case destination. The DFA answers with max, and using that as the
 * destination would let cases through: a value from this frame stored into the caller's object
 * dangles as soon as the callee returns.
 *
 * @param c  Checker.
 * @param e  Place expression of the store target.
 * @return   Shallowest depth; -1 when the path has no named reference step or nothing was
 *           recorded yet.
 */
static int destDerefDepth(Checker *c, Expr *e) {
    Sym *sy = destDerefSym(c, e);
    return sy ? symMinPointee(c, sy, 0) : -1;
}

/**
 * @brief Arena level of the storage a write target sits in.
 *
 * This is not the question `slotDepth` answers: here the question is how long the storage itself
 * lives, not how long the object a reference points at lives. `head = n` stores a pointer into
 * the slot `head`, the slot is in this frame, so what `n` points at must live at least as long
 * as the slot; asking `slotDepth(head)` instead would answer 0 for a `= null` binding and reject
 * the ordinary way of building a list.
 *
 * @param c  Checker.
 * @param e  Place expression of the store target.
 * @return   The level the storage sits in: a local binding is the depth of its block, a
 *           parameter copied into this frame is 1, a global is 0; a projection from a reference
 *           or a parameter falls back to `slotDepth`, which reports the caller's depth (0), so
 *           storing into a caller's object stays conservative.
 */
int storeLayer(Checker *c, Expr *e) {
    /* The write-through-a-reference path (`(*cur).f = v`) asks "how long may the pointee live"
     * and takes the shallowest possibility: `cur` may point at a parameter (0) or at a `new` in
     * this frame (1) => the destination is 0, and a value from this frame stored there dangles
     * -- the criterion's direction is **reject**, not promoting that value into the home. */
    int dd = destDerefDepth(c, e);
    if (dd >= 0) return dd;
    if (e && e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy) {
            /* A parameter's slot is in this frame (the argument is a copy), so writing it
             * cannot touch the caller's binding => depth 1; a global is static storage and
             * outlives every frame => 0. */
            if (sy->depth == 0 && !isGlobalSym(c, sy)) return 1;
            return sy->depth;
        }
    }
    return slotDepth(c, e);
}

/**
 * @brief Lifetime depth of a `dyn` value: the handle is an ordinary value, but the payload lives
 *        in the dyn pool of the **place the site sits in**.
 *
 * The payload is copied into the pool, and the pool lives as long as the place of the site =>
 * the value must live at least that long. `ZONE_HOME` (-1) means "the caller's place", which is
 * depth 0. When the site level is not settled yet (0) nothing is added: if the call site really
 * lets it escape, promotion writes the level and the depth pass will see it.
 *
 * @param e  The `EX_DYN` site.
 * @param d  Depth the payload itself reports (the upper bound of the references it holds).
 * @return   The larger of `d` and the site level; a larger depth means a shorter life, so the
 *           larger value is the conservative direction.
 */
static int dynSiteDepth(Expr *e, int d) {
    int lvl = planZoneLevel(e);
    if (lvl == 0) return d;
    return maxInt(d, (lvl == ZONE_HOME) ? 0 : lvl);
}
/**
 * @brief Upper bound on the depth of the references a value carries, by syntax shape, ignoring
 *        the type.
 *
 * `targetDepth` returns early when `typeContainsRef` says the type does not carry a reference,
 * but a struct whose **field** type is a reference answers false there, so `inner { v: ref local }`
 * would be treated as pointer-free and answered 0, letting a live pointer escape. This function
 * looks at the expression shape only, so it can only **raise** the estimate; raising is the safe
 * direction: it may reject, it cannot accept.
 *
 * @param c  Checker.
 * @param e  Expression to inspect.
 * @return   Upper bound on the depth of the references the value may carry.
 */
int valDepthStructural(Checker *c, Expr *e) {
    if (!e) return 0;
    int d = 0;
    switch (e->kind) {
    case EX_IDENT: {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return anRefDepth(sy);
        /* Mind the axis difference: this asks "does the **structure** hold a reference"
         * (`valDepthStructural`), not "how long does it live". A non-reference binding holds no
         * reference in its structure => 0 is **semantics**, not a fallback. */
        return 0;
    }
    case EX_REF:    return slotDepth(c, e->u.ref.operand);
    case EX_EXT: return slotDepth(c, e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:  return valDepthStructural(c, e->u.deref.operand);
    case EX_SIGN:   return valDepthStructural(c, e->u.sign.operand);
    case EX_COALESCE:
        return maxInt(valDepthStructural(c, e->u.coalesce.main),
                      valDepthStructural(c, e->u.coalesce.fallback));
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, valDepthStructural(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value));
        return d;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            d = maxInt(d, valDepthStructural(c,
                                             (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value));
        return d;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
        return d;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, valDepthStructural(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        return d;
    /* `?` and a conversion pass the operand's references through, a slice is a view over the
     * object it indexes, and an operator can only carry what its operands carry. Falling into
     * `default: return 0` would **underestimate** the depth, and the depth decides how long the
     * storage has to live => unsafe. */
    case EX_SLICE: return slotDepth(c, e->u.slice.obj);
    case EX_TRY:   return valDepthStructural(c, e->u.try_.operand);
    case EX_CONV:  return valDepthStructural(c, e->u.conv.operand);
    case EX_BIN:   return maxInt(valDepthStructural(c, e->u.bin.left),
                                 valDepthStructural(c, e->u.bin.right));
    case EX_UN:    return valDepthStructural(c, e->u.un.operand);
    case EX_INDEX: case EX_FIELD: return slotDepth(c, e);
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
        return planArenaLevel(e) == ARENA_HOME ? 0 : (planArenaLevel(e) > 0 ? planArenaLevel(e) : 0);
    /* The payload is **copied** into the pool, so any reference it carries is carried by this
     * value too -- take the payload's depth. The pool itself lives in the place the site names, so
     * the value cannot outlive that level either. */
    case EX_DYN: return dynSiteDepth(e, valDepthStructural(c, e->u.dynv.payload));
    default:
        /* Answering "the structure holds no reference" for an unrecognised shape is the **unsafe**
         * side (it may well carry one) => this is a fallback, not semantics: being reached means
         * the classification missed a shape. A scalar such as `EX_INT` answers 0 by semantics. An
         * assertion would turn "a signal worth checking" into a crash, and listing every kind one
         * by one would blow the budget of a hand-written walk, so only a debug note is recorded. */
        EXTC_DBG_NOTEF("valDepthStructural: kind=%d answered 0", (int)e->kind);
        return 0;
    }
}

/**
 * @brief Tell whether a type provably cannot hold a live reference.
 *
 * Storing `v` has two cases: `v` is a reference => a pointer is copied and the pointee must live
 * at least as long as that storage; `v` is a plain value => bytes are copied, the source object's
 * lifetime is irrelevant, and only references **inside** the copy matter, which `targetDepth`
 * covers. When this answers true, the source object's depth is not a constraint of the store and
 * promoting it would only cause a false rejection.
 *
 * The user-written type node alone is not enough: an aggregate whose field type is a reference
 * answers false under the shallow check, so integral types have to be recursed into. The answer
 * only **removes** one requirement, so it can only turn a false rejection into an acceptance,
 * never the other way round.
 *
 * @param c  Checker (type table).
 * @param e  Value being stored.
 * @return   `true` when the type provably carries no reference.
 */
static bool typeCannotCarryRef(Checker *c, Expr *e) {
    if (!e) return true;
    if (mentionsParam(e->type)) return false;    /* `T` may carry a reference -> defer */
    return !typeContainsRef(c->tt, tsub(c, e->type));
}

/**
 * @brief Reference depth to record for one stored value.
 *
 * Takes the larger of the reference depth and the structural bound, so a value carrying a live
 * pointer is never underestimated. The structural bound is consulted only when the type may carry
 * a reference, so a pure value copy is not constrained by the source object's lifetime and storing
 * bytes into a container does not drag the allocation site into the caller's arena.
 *
 * Using the structural bound unconditionally would require the `new` site to live as long as the
 * caller's frame, so every iteration would allocate into the caller's home arena and nothing would
 * be reclaimed until the frame ended.
 *
 * @param c  Checker.
 * @param e  Value being stored.
 * @return   Upper bound on the reference depth the stored bytes may carry.
 */
int valDepthForStore(Checker *c, Expr *e) {
    int a = targetDepth(c, e);
    if (typeCannotCarryRef(c, e)) return a;
    int b = valDepthStructural(c, e);
    return a > b ? a : b;
}

/**
 * @brief Depth of a "pool-building call" (promotion of the pool).
 *
 * The same rule as `new`: a fresh allocation lives until the level of its site. A container
 * constructor is such a site -- the place its pool is born in is the level the container lives to.
 * `ZONE_HOME` => 0 ("lives longer than this frame"): a constructor inside a library leaves the
 * place to the caller; `k >= 1` => k: born at level k, lives to level k. When the call does not
 * build a pool (`makesPool` false) `d` is returned unchanged: the depth of a call result is still
 * the deepest of its arguments.
 *
 * @param c  Checker; unused, kept so the signature matches its sibling functions.
 * @param e  Call expression.
 * @param d  Deepest depth among the arguments, also the value returned for a non-pool call.
 * @return   For a pool call the larger of `d` and the site level; `d` otherwise.
 */
static int poolCallDepth(Checker *c, Expr *e, int d) {
    (void)c;
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zone] call=%s makesPool=%d zoneLevel=%d d=%d\n",
                planCallee(e) && planCallee(e)->name ? planCallee(e)->name : "-",
                planCallee(e) ? (int)planMakesPool(planCallee(e)) : -1, planZoneLevel(e), d);
    if (!calleeMakesPool(planCallee(e)) || planZoneLevel(e) == 0) return d;
    int zd = (planZoneLevel(e) == ZONE_HOME) ? 0 : planZoneLevel(e);
    return maxInt(d, zd);
}

/**
 * @brief **Path key** that addresses a field slot on read and write: the whole field chain from
 *        the root binding down to this projection.
 *
 * Using only the last field name (`next`) would make `bx.mid.next` and `bx.next` collide in one
 * slot, and the `next` of a two-level projection would find no slot at all (the declaration only
 * created `mid`) => the origin cannot be recorded, `promoteFieldsAt` cannot move that site, the
 * return-value check cannot see it, and the allocation stays in the function's own frame arena,
 * dangling on return. The whole path is the key, so that a read and a write address the same slot.
 *
 * An element index is written into the name when it is a compile-time constant (`[0]`), and as
 * `[*]` otherwise: different constant indices on the same root get their own slot, dynamic indices
 * share one -- coarser, which only costs promotion precision, never an origin.
 *
 * @param c  Checker; the key string is built in its arena, which outlives the field table.
 * @param e  Projection expression (`EX_FIELD` / `EX_INDEX` chain).
 * @return   The path key; NULL when the root cannot be reached or a step (dereference, slice, ...)
 *           cannot be named.
 */
const char *fieldSlotKey(Checker *c, Expr *e) {
    if (!e) return NULL;
    if (e->kind == EX_IDENT) return "";
    /* The slot of `(*a).next = v` hangs on the **binding a** (the root is given by
     * `destDerefSym`); the key records only the path after the dereference: promotion of a
     * cycle/chain has to walk from here to the field table of the stored object itself. */
    if (e->kind == EX_DEREF) return "";
    if (e->kind == EX_FIELD) {
        const char *p = fieldSlotKey(c, e->u.field.obj);
        if (!p) return NULL;
        return arenaPrintf(c->arena, "%s%s%s", p, *p ? "." : "", e->u.field.name);
    }
    if (e->kind == EX_INDEX) {
        const char *p = fieldSlotKey(c, e->u.index.obj);
        if (!p) return NULL;
        long long k = 0;
        const char *idx = asIntLit(e->u.index.index, &k) ? arenaPrintf(c->arena, "%lld", k) : "*";
        return arenaPrintf(c->arena, "%s[%s]", p, idx);
    }
    return NULL;
}

/* ------------------------------------------------------- return-value borrow depth
 *
 * A call that hands back a view of storage which already exists is not an allocation site: the
 * view lives exactly as long as that storage. The function's borrow record names the parameter the
 * storage belongs to (a method receiver is parameter 0) and the field of
 * that parameter, or -1 for the parameter's own storage. The `return` statement writes the
 * record and the call site reads it.
 */

/**
 * @brief Tell whether a value of this type can name storage somebody else owns.
 *
 * @param c  Checker, providing the type table.
 * @param t  Type of the value; may be NULL.
 * @return   `true` for a view (`slice<T>`) and for a reference.
 */
static bool borrowResultType(Checker *c, Type *t) {
    if (!c || !t) return false;
    Type *b = tsub(c, t);
    return ttIsViewType(b) || (b && b->kind == TY_REF);
}

/**
 * @brief Position of a struct field by name.
 *
 * @param c     Checker, providing the type table.
 * @param t     Type of the struct, possibly behind a reference.
 * @param name  Field name as written in the source.
 * @return      Index into the struct's field list; -1 when the type is not a struct or the name
 *              is not one of its fields.
 */
static int structFieldIndex(Checker *c, Type *t, const char *name) {
    if (!c || !t || !name) return -1;
    Type *b = ttBase(tsub(c, t));
    if (!b || !b->sdef || (b->kind != TY_STRUCT && b->kind != TY_GENERIC)) return -1;
    for (size_t i = 0; i < b->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&b->sdef->fields, i);
        if (fd && fd->name && strcmp(fd->name, name) == 0) return (int)i;
    }
    return -1;
}

/**
 * @brief Index of a function's parameter that a binding is, by identity.
 *
 * @param f   Function whose parameters are looked up; a method receiver is parameter 0.
 * @param sy  Binding to find.
 * @return    Parameter index, or -1 when the binding is not a parameter of `f`.
 */
static int paramSymIndex(FuncDef *f, Sym *sy) {
    if (!f || !sy) return -1;
    int n = anParamSymCount(f);
    if (n > EFF_MAX_PARAMS) n = EFF_MAX_PARAMS;
    for (int i = 0; i < n; i++)
        if ((Sym *)anParamSyms(f)[i] == sy) return i;
    return -1;
}

/* Forward declarations: the borrow-source trace asks a call which argument its own borrow comes
 * from, so the callee lookup and the argument mapping have to be visible before their bodies. */
static FuncDef *borrowCallee(FuncDef *f);
static Expr *callArgForParam(Expr *e, int i);

/**
 * @brief Follow a returned view back to the parameter whose storage it names.
 *
 * A path of `field` projections is followed, with a view, a `?` unwrap, or a `try` in between, and
 * a call that borrows is followed through the argument it named. An index, a dereference, or a call
 * with no record on the way means the storage is not the parameter's own object, and the answer is
 * "no borrow". Returning "no borrow" is always the conservative answer, because the call site then
 * keeps the rule that reads every argument.
 *
 * @param c      Checker, providing the type table.
 * @param e      Returned value.
 * @param field  Out: index of the field the value is read from, -1 for the parameter itself.
 * @return       Index of the parameter, or -1 when the value does not borrow from one.
 */
static int borrowSourceParam(Checker *c, Expr *e, int *field) {
    *field = -1;
    for (int hops = 0; e && hops < 16; hops++) {
        switch (e->kind) {
        case EX_SLICE: e = e->u.slice.obj; continue;
        case EX_INDEX:
            /* `return self.vals[i]` hands out an element of a view that lives in the parameter:
             * the bytes read were never owned by this frame, so the result borrows from `self`.
             * Without this step a pool accessor such as `pool::atDense` looks like it mints a
             * fresh value, and a caller may promote that value into a younger place (gap G1). */
            e = e->u.index.obj;
            continue;
        case EX_SIGN:  e = e->u.sign.operand; continue;
        case EX_TRY:   e = e->u.try_.operand; continue;
        case EX_CALL:
        case EX_METHOD:
        case EX_ASSOC: {
            /* The value came from a call that borrows: the storage is wherever the argument that
             * call named lives in this frame. */
            FuncDef *callee = borrowResultType(c, e->type) ? borrowCallee(planCallee(e)) : NULL;
            Expr *inner = callee ? callArgForParam(e, planRetBorrowArg(callee)) : NULL;
            int innerField = -1;
            if (!inner) return -1;
            int p = borrowSourceParam(c, inner, &innerField);
            *field = innerField;
            return p;
        }
        case EX_FIELD:
            if (*field < 0) {
                *field = structFieldIndex(c, e->u.field.obj ? e->u.field.obj->type : NULL,
                                          e->u.field.name);
                if (*field < 0) return -1;
            }
            e = e->u.field.obj;
            continue;
        case EX_IDENT:
            return paramSymIndex(c ? c->curFunc : NULL, identBindOf(e));
        default:
            return -1;
        }
    }
    return -1;
}

/**
 * @brief Callee that reported a borrow source for its result.
 *
 * @param f  Resolved callee; may be NULL.
 * @return   `f` or its template when the record sits there; NULL when no source is recorded, or
 *           when the body reported more than one source and the record cannot be used.
 */
static FuncDef *borrowCallee(FuncDef *f) {
    for (int i = 0; f && i < 2; i++) {
        int arg = planRetBorrowArg(f);
        if (arg >= 0) return f;
        if (arg == RET_BORROW_CONFLICT) return NULL;
        f = planTemplate(f);
    }
    return NULL;
}

/**
 * @brief Argument expression that is passed as one parameter of a call.
 *
 * @param e  Call site (`EX_CALL`, `EX_METHOD`, `EX_ASSOC`).
 * @param i  Parameter index; 0 is the receiver of a method.
 * @return   The argument, or NULL when the call has no such argument.
 */
static Expr *callArgForParam(Expr *e, int i) {
    if (!e || i < 0) return NULL;
    if (e->kind == EX_METHOD) {
        if (i == 0) return e->u.method.recv;
        if ((size_t)(i - 1) >= e->u.method.args.len) return NULL;
        return *(Expr **)vecAt(&e->u.method.args, i - 1);
    }
    Vec *args = (e->kind == EX_CALL) ? &e->u.call.args
              : (e->kind == EX_ASSOC) ? &e->u.assoc.args : NULL;
    if (!args || (size_t)i >= args->len) return NULL;
    return *(Expr **)vecAt(args, i);
}

/**
 * @brief Name of the field a borrow record points at, in the callee's own types.
 *
 * @param c      Checker, providing the type table.
 * @param f      Callee carrying the record.
 * @param field  Field index of the recorded parameter; -1 means the parameter's own storage.
 * @return       Field name, or NULL when the record names the whole parameter or cannot be
 *               resolved.
 */
static const char *borrowFieldName(Checker *c, FuncDef *f, int field) {
    int arg = planRetBorrowArg(f);
    if (!c || !f || field < 0 || arg < 0) return NULL;
    if ((size_t)arg >= f->params.len) return NULL;
    Param *p = *(Param **)vecAt(&f->params, arg);
    if (!p || !p->type) return NULL;
    Type *b = ttBase(tsub(c, p->type));
    if (!b || !b->sdef || (size_t)field >= b->sdef->fields.len) return NULL;
    FieldDef *fd = *(FieldDef **)vecAt(&b->sdef->fields, field);
    return fd ? fd->name : NULL;
}

/**
 * @brief Depth of the storage a call result borrows from one of its arguments.
 *
 * The argument's own depth answers "where does the handle live"; the field slot answers "where
 * does the storage that handle names live", and the larger of the two is the conservative answer,
 * because a plate may be younger than the container object that holds it.
 *
 * @param c      Checker.
 * @param arg    Argument the callee named; may be NULL.
 * @param field  Field name on that argument, NULL for the argument itself.
 * @return       Depth of the storage; 0 when the argument cannot be named.
 */
static int borrowArgDepth(Checker *c, Expr *arg, const char *field) {
    if (!arg) return 0;
    int d = targetDepth(c, arg);
    if (field && c->scopes.len != 0) {
        /* A by-value element read borrows the storage the container owns, and the field table
         * records where that storage was last handed out. Ask for the larger of the handle's own
         * depth and the recorded slot: erring toward the longer life is the safe direction. */
        Expr *place = (arg->kind == EX_REF) ? arg->u.ref.operand : arg;
        Sym *root = place ? placeRoot(c, place) : NULL;
        int *slot = root ? fieldDepthEntry(c, root, field, false) : NULL;
        return slot ? maxInt(d, *slot) : d;
    }
    return d;
}

/**
 * @brief Depth a borrow result's storage is known to have reached.
 *
 * @param c  Checker.
 * @param e  Call site.
 * @return   The recorded depth, or -1 when the call has no record.
 */
static int settledBorrowDepth(Checker *c, Expr *e) {
    if (!c || !e) return -1;
    for (size_t i = 0; i < c->borrowSettled.len; i++) {
        BorrowSettled *r = *(BorrowSettled **)vecAt(&c->borrowSettled, i);
        if (r && r->site == e) return r->depth;
    }
    return -1;
}

/**
 * @brief Record that a borrow result's storage reached a depth.
 *
 * Called only after the promotion of that storage reported success. A later requirement may be
 * stricter, so the record keeps the smallest depth seen.
 *
 * @param c  Checker.
 * @param e  Call site whose storage moved.
 * @param d  Depth the storage reached.
 */
static void settleBorrowDepth(Checker *c, Expr *e, int d) {
    if (!c || !e || d < 0) return;
    for (size_t i = 0; i < c->borrowSettled.len; i++) {
        BorrowSettled *r = *(BorrowSettled **)vecAt(&c->borrowSettled, i);
        if (r && r->site == e) {
            if (d < r->depth) r->depth = d;
            return;
        }
    }
    BorrowSettled *r = arenaAllocZero(c->arena, sizeof *r);
    r->site = e;
    r->depth = d;
    *(BorrowSettled **)vecPush(&c->borrowSettled) = r;
}

/**
 * @brief Depth a call result has when the callee named a borrow source for it.
 *
 * @param c    Checker.
 * @param e    Call site (`EX_CALL`, `EX_METHOD`, `EX_ASSOC`).
 * @param out  Receives the depth when the call reports a source.
 * @return     `true` when `out` was written, `false` when the call site keeps its own rule.
 */


static bool borrowResultDepth(Checker *c, Expr *e, int *out) {
    if (!c || !e || !out) return false;
    /* Views and references always borrow, and so does a by-value result whose type owns bytes
     * (`pool::atDense` hands back a `string` that lives in the pool). The recorded source is what
     * decides; a result the callee built itself has no record and falls through to the old path. */
    if (!borrowResultType(c, e->type) && !typeContainsRef(c->tt, tsub(c, e->type))) return false;
    int settled = settledBorrowDepth(c, e);
    if (settled >= 0) {
        *out = settled;
        return true;
    }
    FuncDef *f = borrowCallee(planCallee(e));
    if (!f) return false;
    Expr *arg = callArgForParam(e, planRetBorrowArg(f));
    if (!arg) return false;
    *out = borrowArgDepth(c, arg, borrowFieldName(c, f, planRetBorrowField(f)));
    return true;
}

/**
 * @brief Public form of the borrow-depth query, for the store rule.
 *
 * A store of a by-value result (`k.atDense(i)` handing back a `string`) stores the pool's bytes,
 * not a new value; the call site needs the depth of that borrow to keep the destination rule
 * honest (gap G1).
 *
 * @param c    Checker, providing the plan table.
 * @param e    Expression whose storage depth is wanted.
 * @param out  Receives the depth when the result borrows.
 * @return     true when `e` is a call whose result borrows existing storage.
 */
bool borrowDepthOf(Checker *c, Expr *e, int *out) { return borrowResultDepth(c, e, out); }

/**
 * @brief Deepest borrow found anywhere inside a value, not just at its top.
 *
 * A store moves **bytes**, and a struct literal, an array literal, a `??` arm or an operator
 * result can carry another expression's borrowed bytes even though the node itself was built
 * here. Asking only the top of the tree let `pool_a.insert(holder { s = pool_b.atDense(0) })`
 * through while `pool_a.insert(pool_b.atDense(0))` was refused -- one question, two answers, and
 * the accepted one was a use-after-free.
 *
 * @param c    Checker, providing the plan table.
 * @param e    Expression to walk; nested expressions are visited without recursing into callees.
 * @param out  Receives the largest depth found (0 when nothing borrows).
 * @return     True when any part of the value borrows existing storage.
 */
static bool borrowDepthWalk(Checker *c, Expr *e, int *out, int hops);

/** @brief State of one `borrowDepthWalk` descent. */
typedef struct {
    Checker *c;
    int *out;   /* largest depth found so far */
    int hops;   /* recursion budget, so a cyclic tree cannot loop forever */
    bool any;   /* some part of the value borrows */
} BorrowWalk;

/**
 * @brief Visit one child of the value being walked, and keep descending through it.
 *
 * The descent is driven by `astWalkExprChildren`, so a new expression kind is covered by
 * construction -- that is the whole point: this walk replaced a hand-written switch, and the
 * hand-written one is what missed the shapes that made the struct-literal store a use-after-free.
 *
 * @param ctx  `BorrowWalk *` for the current descent.
 * @param e    Child expression to visit.
 * @return     True to continue walking.
 */
static bool borrowDepthVisit(void *ctx, Expr *e) {
    BorrowWalk *w = (BorrowWalk *)ctx;
    if (!e || w->hops > 24) return true;
    int d = 0;
    if (borrowDepthOf(w->c, e, &d)) {
        if (d > *w->out) *w->out = d;
        w->any = true;
    }
    if (e->kind == EX_IDENT) {
        /* A binding carries no storage of its own: ask the expression it came from. */
        Sym *sy = identBindOf(e);
        if (sy && sy->origin && sy->origin != e) {
            if (borrowDepthWalk(w->c, sy->origin, w->out, w->hops + 1)) w->any = true;
        }
    }
    AstVisit v = { borrowDepthVisit, NULL, w };
    BorrowWalk kids = { w->c, w->out, w->hops + 1, false };
    v.ctx = &kids;
    astWalkExprChildren(e, &v);
    w->any = kids.any || w->any;
    return true;
}

/** @brief Walk one value and every expression inside it, collecting the deepest borrow. */
static bool borrowDepthWalk(Checker *c, Expr *e, int *out, int hops) {
    if (!e || !out || hops > 24) return false;
    BorrowWalk w = { c, out, hops, false };
    int d = 0;
    if (borrowDepthOf(c, e, &d)) { if (d > *out) *out = d; w.any = true; }
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        if (sy && sy->origin && sy->origin != e && borrowDepthWalk(c, sy->origin, out, hops + 1)) w.any = true;
    }
    AstVisit v = { borrowDepthVisit, NULL, &w };
    astWalkExprChildren(e, &v);
    return w.any;
}

/**
 * @brief Does any part of this value borrow storage, and how deep is the deepest borrow.
 *
 * @param c    Checker.
 * @param e    Expression to inspect.
 * @param out  Receives the deepest borrowed depth.
 * @return     True when some part of the value borrows.
 */
bool borrowDepthAnywhere(Checker *c, Expr *e, int *out) {
    int d = 0;
    bool any = borrowDepthWalk(c, e, &d, 0);
    if (out) *out = d;
    return any;
}

/**
 * @brief Depth of the storage the references carried by a value point at.
 *
 * A smaller number means a longer life; 0 means a parameter or a global, that is, outside this
 * frame. This is the quantity the reference rule compares against the depth of the target storage,
 * so every approximation has to err large: too large only rejects, too small lets a dangling
 * reference through.
 *
 * @param c  Checker.
 * @param e  Expression to inspect.
 * @return   Depth of the references inside the value; 0 when the type cannot carry a reference.
 */
int targetDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* The only reason to return early: this type cannot carry a reference. A type that mentions a
     * type parameter must not take that exit -- inside a generic body the depth assumes the
     * parameter may carry a reference, and the instance check decides whether the rule applies.
     * `EX_DYN` must not take it either: `dyn T` itself carries no reference, but the payload is
     * **copied into the pool**, so the value carries the references of what the payload points at
     * (the payload's references count too). */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return 0;
    /* A binding's depth is authoritative on the binding, not on the expression node: the node's
     * copy is written when the expression is first checked, while the data-flow analysis settles
     * binding depths only afterwards, over the whole control flow. Reading the node here would
     * answer with a pre-fixpoint value and report a struct that holds an allocation as holding no
     * live pointer. */
    if (!c->substParams && !mentionsParam(e->type) && anRefDepth(e)) {
        if (e->kind != EX_IDENT) return anRefDepth(e);
        Sym *syc = identBindOf(e);
        if (!syc || !syc->type || tsub(c, syc->type)->kind != TY_REF ||
            anRefDepth(syc) >= anRefDepth(e)) return anRefDepth(e);
    }

    int d = 0;
    switch (e->kind) {
    case EX_REF:
        d = slotDepth(c, e->u.ref.operand);
        break;
    case EX_EXT: 
        d = slotDepth(c, e->u.ext_.call);
        break;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:
        /* The value of `*p` lives where `p` points, so it has the depth of `p`. */
        d = targetDepth(c, e->u.deref.operand);
        break;
    case EX_SIGN:
        /* `p!` only drops nullability; it still refers to the same storage. */
        d = targetDepth(c, e->u.sign.operand);
        break;
    case EX_DYN:
        /* The payload is copied into the pool: its references are this value's references, and the
         * pool lives in the place this site names -- both bound how long the value can live. */
        d = dynSiteDepth(e, targetDepth(c, e->u.dynv.payload));
        break;
    case EX_NEW:
    case EX_GENCALL:
        /* A freshly allocated object lives until the end of the enclosing block, so
         * its depth is the number the checker recorded for the site. */
        d = anRefDepth(e);
        break;
    case EX_COALESCE:
        /* Either side can become the result, so take the deeper one. The deeper
         * answer is the conservative one. */
        d = maxInt(targetDepth(c, e->u.coalesce.main),
                   targetDepth(c, e->u.coalesce.fallback));
        break;
    /* `?` and a conversion pass the operand's references through, a slice is a view over the
     * object it indexes, and an operator can only carry what its operands carry. Falling into
     * `default: return 0` would **underestimate** the depth, and the depth decides how long the
     * storage has to live => unsafe. */
    case EX_SLICE:
        d = slotDepth(c, e->u.slice.obj);
        break;
    case EX_TRY:
        d = targetDepth(c, e->u.try_.operand);
        break;
    case EX_CONV:
        d = targetDepth(c, e->u.conv.operand);
        break;
    case EX_BIN:
        d = maxInt(targetDepth(c, e->u.bin.left), targetDepth(c, e->u.bin.right));
        break;
    case EX_UN:
        d = targetDepth(c, e->u.un.operand);
        break;
    case EX_IDENT: {
        /* An aggregate binding that holds references has to be asked where those references
         * point, not how deep its own slot is: `return h` (with `h` holding a reference to
         * parameter `p`) answered by slot depth 1 would reject a legal program. Slot depth is
         * still the right answer for "storing into this binding" (`slotDepth` computes that); the
         * two questions are kept apart. */
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = anRefDepth(sy);
        else d = slotDepth(c, e);
        /* The `typeContainsRef` test above is not enough: it answers false when a **field** type
         * is a reference, so a binding of `struct holder { p: ?ref i32 }` would report 0 without
         * ever consulting the field table. So the per-field bound is taken again whatever the type
         * says. */
        if (sy) {
            for (int fi = 0; fi < sy->nfields; fi++)
                if (sy->fields[fi].depth > d) d = sy->fields[fi].depth;
            if (sy->otherDepth > d) d = sy->otherDepth;
        }
        break;
    }
    case EX_FIELD: case EX_INDEX:
        /* Read the depth recorded on the field itself. Reporting the enclosing slot's depth would
         * reject `h.p`, and a later `h.p = null` could not repair it, because the two answers say
         * different things. When the field has no entry (just declared, or the name does not
         * match), fall back to the conservative place depth.
         *
         * An element (`arr[0]`) is the same cell as a field: `arr[0] = p` is recorded in `arr`'s
         * `[0]` slot, and `return arr[0]` has to read exactly that cell (promotion lowers it to the
         * required level). Checking only `EX_FIELD` would send elements back to `slotDepth` (array
         * slot depth 1) and falsely reject a returned `new` from this frame. */
        {
            Sym *rf = placeRoot(c, e);
            /* `(*a).next` cannot be named (`placeRoot` does not walk through a dereference), but
             * the slot hangs on `a`: promotion of a cycle/chain (`(*a).next = b`) needs it to pass
             * the requirement on to the object `b` points at. */
            if (!rf) rf = destDerefSym(c, e);
            const char *key = fieldSlotKey(c, e);
            int *slot = (rf && key) ? fieldDepthEntry(c, rf, key, false) : NULL;
            if (slot) { d = *slot; break; }
        }
        d = slotDepth(c, e);
        break;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, targetDepth(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value));
        break;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            d = maxInt(d, targetDepth(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value));
        break;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
        break;
    case EX_CALL:
        /* The depth of a call result is the maximum depth of its arguments. Why that is a sound
         * upper bound: the references a callee can return come only from static storage (0) or from
         * its arguments (<= max(argument)); returning a reference to one of its own locals is
         * rejected by its own return check, so there is no third source. Taking the maximum is what
         * keeps a callee that returns one of its reference arguments unchanged.
         *
         * A callee that hands back a **view of storage that already exists** names the argument
         * the storage belongs to, and the result lives exactly as long as that storage. The call
         * then creates nothing, so its own block level takes no part. */
        for (size_t i = 0; i < e->u.call.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.call.args, i)));
        if (!borrowResultDepth(c, e, &d)) d = poolCallDepth(c, e, d);
        break;
    case EX_METHOD:
        d = maxInt(d, targetDepth(c, e->u.method.recv));   /* the receiver is an argument too */
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.method.args, i)));
        if (!borrowResultDepth(c, e, &d)) d = poolCallDepth(c, e, d);
        break;
    case EX_ENUMVAL:
        /* An enum payload construction such as `shape.holding(a[..])` puts the payload inside
         * this value, so, exactly as for an array literal, the value's depth is the payload's
         * depth. */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        if (!borrowResultDepth(c, e, &d)) d = poolCallDepth(c, e, d);
        break;
    default:
        d = 0;
        break;
    }
    /* Comparison against `EXTC_DBG_RHO`: the baseline depends only on lexical block depth and the
     * origin chain and reads neither the solver nor the cache. A larger baseline means the checker
     * underestimated the value's lifetime, which is exactly the direction that produces dangling
     * allocations. */
    if (dbgOn("EXTC_DBG_RHO")) {
        Expr *seen[40];
        int pure = targetDepthPure(c, e, 0, seen);
        if (pure != d)
            fprintf(stderr, "[rho] %s:%d kind=%d cached=%d pure=%d %s\n",
                    c->ctx && c->ctx->path ? c->ctx->path : "?", e->line, (int)e->kind, d, pure,
                    pure > d ? "UNDER" : "OVER");
    }
    /* A node that mentions a type parameter is not cached: that number holds only inside the
     * generic body and may not hold for an instance. */
    if (!c->substParams && !mentionsParam(e->type)) anSetRefDepth(e, d);
    return d;
}

/**
 * @brief Tell whether a symbol is a module-level global.
 *
 * A global and a parameter both have depth 0, but only a parameter is borrowed, so the escape rules
 * have to tell the two apart.
 *
 * @param c  Checker; the lookup runs against its global table.
 * @param s  Symbol to test.
 * @return   `true` when `s` is declared at module level.
 */
static bool isGlobalSym(Checker *c, Sym *s) {
    for (size_t i = 0; i < c->globals.len; i++)
        if (*(Sym **)vecAt(&c->globals, i) == s) return true;
    return false;
}

/**
 * @brief Tell whether a place denotes storage somebody else owns.
 *
 * Unlike `exprBorrowed` this does not look at the value's type (`*p` may carry no reference at
 * all); it asks only who owns the storage. It is used when a reference is re-created, as in
 * `ref *p`, where the question is whether the result still points into the caller's frame.
 *
 * @param c  Checker.
 * @param e  Place expression.
 * @return   `true` when the storage is reached through a dereference or belongs to a parameter
 *           rather than to this frame.
 */
static bool placeIsBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_DEREF) return placeIsBorrowed(c, e->u.deref.operand);
    if (e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_INDEX) {
        Sym *root = placeRoot(c, e);
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    return false;
}

/**
 * @brief Tell whether a value holds a reference borrowed from outside this frame.
 *
 * A reference that traces to a parameter or to a call result is borrowed: its real lifetime is
 * decided by the caller, so the callee may pass it on but may not store it anywhere that outlives
 * this call. A reference to a global does not count as borrowed: static storage outlives every
 * frame.
 *
 * The type-level exit is the only early return allowed here: a node that mentions a type parameter
 * has to be walked, because a deferred check recorded inside a generic body consumes this answer at
 * instantiation.
 *
 * @param c  Checker.
 * @param e  Expression to inspect.
 * @return   `true` when the value is, or may be, borrowed.
 */
static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    /* The only reason to answer without looking: this type cannot carry a reference. A node that
     * mentions a type parameter must not take this exit, since the generic body's deferred check
     * needs this answer; `EX_DYN` must not take it either, because the payload is copied into the
     * pool => the value borrows whatever the payload borrowed. */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_DYN: return exprBorrowed(c, e->u.dynv.payload);
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* Depth 0 and not a global => borrowed. A global/static is 0 too, but anybody may store
         * it. */
        if (root && root->depth == 0 && !isGlobalSym(c, root)) return true;
        /* A local **initialized from** a borrowed value carries that borrow: `q` has the
         * parameter's lifetime, so writing it into a global is still storing a borrow (the caller's
         * lifetime) even though `q` itself is a local of this frame. Asking only the binding's own
         * lexical depth would answer false and let it through. */
        Sym *sy = (e->kind == EX_IDENT) ? identBindOf(e) : root;
        if (sy && sy->depth > 0 && sy->origin && sy->origin->kind == EX_IDENT)
            return exprBorrowed(c, sy->origin);
        /* Only a **plain binding** is followed, and the narrowness is load-bearing: following
         * `EX_FIELD` origins as well made `self.top = t.next` (read a reference out of the object
         * `self` points at and write it back into that same object) report "cannot store a borrowed
         * value into something that outlives this call", while both sides sit behind the same
         * `mut ref` and no lifetime was extended. */
        return false;
    }
    case EX_SIGN:
        /* The nullable suffix changes the type's promise, not where the value came from => the
         * borrowed status is passed through unchanged. */
        return exprBorrowed(c, e->u.sign.operand);
    case EX_COALESCE:
        /* Either side can become the result, so if either side is borrowed the whole expression
         * is borrowed. */
        return exprBorrowed(c, e->u.coalesce.main) ||
               exprBorrowed(c, e->u.coalesce.fallback);
    case EX_REF:
        /* `ref *p` re-creates a reference and hides that it comes from a parameter: `*p` denotes
         * the storage `p` points at, so the question is whether that storage is borrowed.
         * `exprBorrowed` must not be recursed into here: the value of `*p` may be `i32`, and the
         * type exit above would drop that case. */
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
        /* Payload construction: a borrowed payload makes the constructed value borrowed, exactly as
         * for an array literal. */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    default: return false;      /* literals, globals, and fresh allocations belong to this frame */
    }
}

/**
 * @brief Lower the level a pool-building value is recorded at, so it can reach `at`.
 *
 * A pool is an allocation site, and a fresh allocation has no other owner: the level "where the
 * pool is born" can be lowered (smaller is longer-lived). That is what lets a container built in a
 * loop be stored into a slot declared outside it -- without this, the pool is remembered at the
 * block the expression sits in and the store is rejected as "a reference into a deeper scope".
 *
 * @param val  Value being stored or returned; its zone fact is read and lowered in place.
 * @param at   Level the value has to fit into; 0 means "the tier outside this frame".
 * @return     `true` when the site builds a pool and was handled here (the caller then treats it as
 *             stored), `false` when the site allocates nothing and keeps the old answer.
 */
static bool lowerPoolSite(Expr *val, int at) {
    if (!calleeMakesPool(planCallee(val)) || planZoneLevel(val) == 0) return false;
    int want = (at == 0) ? ZONE_HOME : at;
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[promoted] node=%p %d -> %d (at=%d)\n", (void *)val, planZoneLevel(val), want, at);
    if (planZoneLevel(val) > want) planSetZoneLevel(val, want);
    if (anRefDepth(val) > at) anSetRefDepth(val, at);
    return true;
}

/**
 * @brief Tell whether the root of a value comes from a parameter of some function.
 *
 * If it does, the lifetime obligation is handed to the call site, which knows how long the
 * corresponding argument lives. `placeRootName` resolves the root of the place: `v` stays `v`,
 * `*p` becomes `p`, and `l.head` becomes `l`.
 *
 * @param c    Checker; unused, kept so the predicate matches its siblings.
 * @param f    Function definition providing the parameter names.
 * @param val  Value to trace.
 * @return     `true` when the value's root is a parameter name of `f`; `false` when the value is
 *             NULL, the function is NULL, or the root cannot be named.
 */
bool valTracesToParam(Checker *c, FuncDef *f, Expr *val) {
    /* This predicate uses only "function and value"; the checker parameter is kept so the signature
     * matches its siblings. */
    (void)c;
    if (!val || !f) return false;
    const char *root = placeRootName(val);
    if (!root) return false;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, root) == 0) return true;
    return false;
}

/* Forward declaration: the deferred reference check before a value is stored into a place
 * (defined below). */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what);   /* defined below */

/*
 * Forward declaration of the carrier walk and promotion, defined below at `promoteInto2`.
 *
 * The most ordinary code -- building a list inside a loop -- allocates its nodes in the loop
 * body while `head` lives outside the loop, so the next iteration's block exit frees them and
 * leaves a dangling pointer (confirmed with ASan).
 * at   - depth the value must reach; 0 = must outlive this frame (the caller's arena)
 */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops);

/**
 * @brief Promote the storage a borrowed call result points into.
 *
 * A view handed back by a callee lives in storage that already exists, so the requirement goes to
 * that storage instead of to the call's own block level. With a field, the slot named by the
 * callee is the one that moves: a slot with no recorded source cannot be moved, and answering
 * `true` there would lower the call's depth over storage that stayed behind.
 *
 * @param c      Checker.
 * @param arg    Argument the callee named as the source; may be NULL.
 * @param field  Field of that argument, NULL for the argument itself.
 * @param at     Depth the result has to reach.
 * @param hops   Carrier steps already taken.
 * @return       `true` when the storage now reaches `at`.
 */
static bool promoteBorrowed(Checker *c, Expr *arg, const char *field, int at, int hops) {
    if (!c || !arg || at < 0) return false;
    Expr *place = (arg->kind == EX_REF) ? arg->u.ref.operand : arg;
    if (!place) return false;
    if (!field) return promoteInto2(c, place, at, hops + 1);
    if (c->scopes.len == 0) return false;
    Sym *root = placeRoot(c, place);
    if (!root) return false;
    for (int i = 0; i < root->nfields; i++) {
        if (!root->fields[i].name || strcmp(root->fields[i].name, field) != 0) continue;
        if (!root->fields[i].src) break;
        if (at < root->fields[i].minReq) root->fields[i].minReq = at;
        int want = root->fields[i].depth;
        if (root->fields[i].minReq < want) want = root->fields[i].minReq;
        Expr *src = root->fields[i].src;
        Sym *srcSym = root->fields[i].srcSym;
        bool lower = !root->addressed;
        if (!promoteInto2(c, src, want, hops + 1)) return false;
        if (srcSym && srcSym != root && !promoteFieldsAt(c, srcSym, want, hops + 1)) return false;
        if (lower && root->fields[i].depth > want) root->fields[i].depth = want;
        return true;
    }
    /* The caller keeps no record for that field (its fields are reached through the argument
     * itself), so the whole argument is what has to reach `at`; moving more than the callee named
     * only lengthens lifetimes. */
    return promoteInto2(c, place, at, hops + 1);
}
/* `dyn Trait(x)` carries its payload with it, so the payload's allocation sites have to reach the
 * level this value has to reach -- exactly what the rest of this function does for a struct. */
bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops);
/**
 * @brief Promote every allocation site inside a value to the level it has to reach.
 *
 * The walk follows only the value's carriers (bindings, `!`, `??`, struct literals, enum payloads,
 * array literals); it does not follow aliases and does no interprocedural reasoning, and it gives up
 * once its hop budget (`EXTC_PROMOTE_MAX_HOPS`) is spent. Promotion only lengthens lifetimes; it
 * never turns an unsafe store into an accepted one. This is the entry point the store and return
 * checks call, starting at zero steps.
 *
 * @param c    Checker.
 * @param val  Value being stored or returned.
 * @param at   Level the value has to reach; 0 means it must outlive this frame (the caller's
 *             arena).
 * @return     `true` when every allocation site in the value reached `at`; `false` when some
 *             carrier cannot be moved, in which case the caller falls back to reporting by the
 *             depth criterion.
 */
bool promoteInto(Checker *c, Expr *val, int at) { return promoteInto2(c, val, at, 0); }

/**
 * @brief Promote the allocation sites reachable through a binding's field table to the level they
 *        have to reach.
 *
 * `Sym.origin` records only the initializer at the declaration, so a site reached through `h.q = x`
 * after `var h: box = { p: null, q: null }` can never be reached through `origin`; it would stay at
 * a shallow level and dangle as soon as the container dies. This function does only the promotion
 * half, reading the field table's origins in addition to `origin`.
 *
 * Discipline: only slots that have a `src` are promoted; the level to reach is the destination's
 * `at` (the field table's own number says what the field holds right now and takes no part); after
 * a successful promotion that slot's accounting may drop to `at`, while a slot that cannot be
 * promoted keeps its old accounting, so the outcome is a false rejection rather than a hole; and a
 * slot without a `src` is left entirely alone.
 *
 * @param c     Checker.
 * @param sy    Binding whose field table is walked.
 * @param at    Depth the container is stored at; 0 means it must outlive this frame.
 * @param hops  Carrier steps already taken.
 * @return      `true` when every slot with an origin was promoted; `false` when some slot cannot be
 *              moved.
 */
bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops) {
    if (!sy) return true;
    if (hops > EXTC_PROMOTE_MAX_HOPS) { EXTC_DBG_FALLBACK("promoteFieldsAt: recursion budget exhausted"); return true; }
    /* Cycle guard: one binding is walked once per walk. The origin table can be cyclic (`a = b`,
     * `b = a`, or two field tables pointing at each other) and walking the origin chain would
     * recurse forever; the hop limit alone would only trip `EXTC_DBG_FALLBACK` (red in an assertion
     * build) after burning the budget on a cycle. The requirement is already being applied along
     * that path, so walking it again adds no information. */
    for (int i = 0; i < c->fsDepth; i++)
        if (c->fsChain[i] == sy) return true;
    if (c->fsDepth >= (int)(sizeof c->fsChain / sizeof c->fsChain[0])) {
        EXTC_DBG_FALLBACK("promoteFieldsAt: field-source chain too deep");
        return true;
    }
    c->fsChain[c->fsDepth++] = sy;
    /* A root whose address was taken has two opposite problems: lowering the accounting is unsafe
     * when an alias may write (an alias could store a deeper value), while promoting the sites
     * behind the origins is always safe (it only moves an allocation to a longer-lived arena). So
     * promotion is done as usual and only the accounting drop is skipped. */
    bool lower = !sy->addressed;
    bool ok = true;
    for (int i = 0; i < sy->nfields; i++) {
        Expr *src = sy->fields[i].src;
        if (!src) continue;
        /* What has to be reached is min(the depth recorded for the slot, the shallowest level it was
         * ever required to reach); this store's destination alone is not enough: `out = h` is level
         * 1, but a later `return out` requires the value to outlive this frame (0), so the site has
         * to end up in the home arena. Promoting to 1 looks sufficient, but level 1 is freed when
         * the function returns and would leave a use-after-free. */
        if (at < sy->fields[i].minReq) sy->fields[i].minReq = at;
        int want = sy->fields[i].depth;
        if (sy->fields[i].minReq < want) want = sy->fields[i].minReq;
        if (!promoteInto2(c, src, want, hops + 1)) { ok = false; continue; }
        /* The origin binding's own field table is walked too: a cycle/chain such as
         * `a.next = b  b.next = c` has to be promoted all the way to `want`. The flattened `src` is
         * the site expression and has no field table; `srcSym` is the binding of the same object and
         * still works after it leaves scope. */
        if (sy->fields[i].srcSym && sy->fields[i].srcSym != sy
            && !promoteFieldsAt(c, sy->fields[i].srcSym, want, hops + 1)) ok = false;
        if (lower && sy->fields[i].depth > want) sy->fields[i].depth = want;   /* promoted -> lower it too */
    }
    /* Same for a store that belongs to no single field: element writes, and stores a callee made
     * through an out-parameter. Both directions follow the rule above. */
    if (sy->otherSrc) {
        if (dbgOn("EXTC_DBG_FS"))
            fprintf(stderr, "[fs-promote] %s kind=%d depth=%d minReq=%d at=%d\n", sy->name,
                    (int)sy->otherSrc->kind, sy->otherDepth, sy->otherMinReq, at);
        if (at < sy->otherMinReq) sy->otherMinReq = at;
        int want = sy->otherDepth;
        if (sy->otherMinReq < want) want = sy->otherMinReq;
        if (!promoteInto2(c, sy->otherSrc, want, hops + 1)) ok = false;
        else if (lower && sy->otherDepth > want) sy->otherDepth = want;
        if (sy->otherSrcSym && sy->otherSrcSym != sy
            && !promoteFieldsAt(c, sy->otherSrcSym, want, hops + 1)) ok = false;
    }
    /* The declaration's **alias origin** (`var head = a`): head and a share the pointee, and when a
     * is later re-pointed (`a = p`) the requirement has to follow a's **current** origin. This cell
     * serves promotion only and takes no part in depth accounting. */
    if (sy->initSrc) {
        if (at < sy->initMinReq) sy->initMinReq = at;
        int want = sy->initSrcDepth;
        if (sy->initMinReq < want) want = sy->initMinReq;
        if (!promoteInto2(c, sy->initSrc, want, hops + 1)) ok = false;
    }
    /* Same for a write the callee made through a `mut ref` argument: no accounting is lowered
     * here; what has to move is the site behind the origin, because `return b` needs the home
     * arena. */
    if (sy->callSrc) {
        if (at < sy->callMinReq) sy->callMinReq = at;
        int want = sy->callSrcDepth;
        if (sy->callMinReq < want) want = sy->callMinReq;
        if (!promoteInto2(c, sy->callSrc, want, hops + 1)) ok = false;
    }
    c->fsDepth--;
    return ok;
}

/**
 * @brief Record one publication: a value is put at level `at`. Nothing is decided here.
 *
 * The checking pass only records; a later fold solves the level numbers together with the depth
 * fixpoint, so no decision depends on traversal order. Records hang on `c->stores` and carry the
 * enclosing function body, for the per-body fold.
 *
 * @param c       Checker; records are appended to its store table.
 * @param val     Value being published.
 * @param target  Destination; NULL for a return-value publication.
 * @param at      Target level; 0 means it must outlive this frame.
 * @param line    Source line for the diagnostic.
 *
 * This is the checking pass's whole involvement with arena levels.
 */
void recordStore(Checker *c, Expr *val, Expr *target, int at, int line) {
    if (!c || !val || at < 0) return;
    if (anStoredAt(val) >= 0 && anStoredAt(val) <= at) {
        /* The value was already published at a level no deeper than this one, so recording it again
         * adds no lifetime requirement -- but that holds only for the same destination. The
         * destination is a separate fact, and it is the one that carries the requirement back to the
         * bindings the value was built from: `h = g` in one arm of an `if` and `h = zero` in the
         * other are two destinations for two different values, and it is the destination, not the
         * value, that decides which of them the binding's level has to cover. Skipping the second
         * record would drop that arm's requirement. */
        for (size_t i = c->stores.len; i > c->storeBase; i--) {
            StoreSite *prev = *(StoreSite **)vecAt(&c->stores, i - 1);
            if (c->fxOn) c->fxDedupSteps++;
            if (prev && prev->value == val && prev->target == target) return;
        }
        if (!target) return;
    }
    anSetStoredAt(val, at);
    StoreSite *st = (StoreSite *)arenaAllocZero(c->arena, sizeof(StoreSite));
    st->value  = val;
    st->target = target;
    st->at     = at;
    st->line   = line;
    st->fn     = c->curFunc;    /* which body's fold this record belongs to */
    if (c->fxOn && !st->fn) c->fxPreBodyStores++;      /* recorded outside every body */
    *(StoreSite **)vecPush(&c->stores) = st;
}

/**
 * @brief Single entry point for one write: publication, promotion, the escape mark, and the field
 *        table, in that fixed order.
 *
 * The order cannot change: `recordStore` has to run before `promoteInto` so the solver sees the
 * requirement first, and the field table has to run after promotion because promoting the field
 * table reads it. A whole-value write goes through `noteWholeValueDepthWrite`, a field or element
 * write through `noteFieldDepthWrite`; a write that involves no reference returns immediately.
 *
 * @param c       Checker.
 * @param value   Value being written.
 * @param target  Target place of the write; without a target only publication and promotion run.
 * @param at      Arena level of the target; 0 means it must outlive this frame.
 * @param line    Source line for the diagnostic.
 */
void noteStore(Checker *c, Expr *value, Expr *target, int at, int line) {
    if (!c || !value) return;
    recordStore(c, value, target, at, line);          /* step 1: level record */
    promoteInto(c, value, at);                        /* step 2: promotion, conservative when it cannot lift */
    markCallHomeIfEscaping(c, value, at);             /* step 3: the escape-set half */
    /* step 4: the field table / whole-value table */
    Sym *vs = target ? placeRoot(c, target) : NULL;
    /* `(*a).next = b` cannot be named (`placeRoot` does not walk through a dereference), but the
     * slot hangs on `a`: without recording it, promotion of `return a` cannot reach the object `b`
     * points at, the cycle/chain stays at block level => it dangles on return. */
    if (!vs && target) vs = destDerefSym(c, target);
    if (!vs) return;
    int d2 = valDepthForStore(c, value);
    if (!(d2 > 0 || (vs->type && typeContainsRef(c->tt, vs->type)))) return;
    if (target->kind == EX_IDENT) {
        /* The **check-time** half of write invalidation: when a whole-value write happens, the
         * "proven non-null" fact about this binding must already be void. The assertion turns that
         * into a checked invariant -- if some write path forgets `unNarrow`, a debug build fires. */
        EXTC_DBG_ASSERT_MSGF(!isNarrowed(c, vs->cname),
                             "write without invalidating the non-null proof of `%s`", vs->name);
        noteWholeValueDepthWrite(c, vs, value, d2);
    } else {
        const char *fn2 = (target->kind == EX_FIELD || target->kind == EX_INDEX)
                          ? fieldSlotKey(c, target) : NULL;
        c->curStoreVal = value;
        noteFieldDepthWrite(c, vs, fn2, d2);
        c->curStoreVal = NULL;
    }
}

/**
 * @brief Publish a return value at level 0.
 *
 * The destination is the value itself: level 0 means "outlives this frame", and everything handed
 * back to the caller is published that way.
 *
 * @param c      Checker.
 * @param value  Value being returned.
 * @param line   Source line for the diagnostic.
 */
void noteReturnPublish(Checker *c, Expr *value, int line) {
    if (!c || !value) return;
    recordStore(c, value, value, 0, line);
}

/**
 * @brief Record which parameter a returned view borrows its storage from.
 *
 * The record is written by the `return` statement and read at the call site. Two `return`
 * statements that name different sources leave the body with no single answer, and the record
 * then says so: the call site falls back to the conservative rule that reads every argument.
 * That covers the shape which cannot be followed at all, such as a view of a local container:
 * the storage there is the frame's own, so the result does not borrow from a parameter.
 *
 * @param c      Checker.
 * @param value  Returned value.
 */
void noteReturnBorrow(Checker *c, Expr *value) {
    if (!c || !c->curFunc || !value) return;
    FuncDef *f = c->curFunc;
    int field = -1;
    int arg = -1;
    bool viewy = borrowResultType(c, value->type);
    /* A result that is handed back by value can still borrow: `pool::atDense` returns `T`, and
     * for a `T` that owns bytes (`string`, `buf`, a struct holding a view) those bytes live in
     * the pool the receiver names. Record that source too, otherwise rule R gives the result a
     * fresh level and rule S lets it be promoted into a younger place (gap G1). */
    /* A type parameter may be instantiated with a type that owns bytes (`pool<T>::atDense`
     * returns `T`), so treat it as carrying storage here: the call-site side decides again with
     * the instantiated type, where a scalar `T` falls through to the old path. */
    Type *vt = tsub(c, value->type);
    bool carries = viewy || (vt && vt->kind == TY_PARAM)
                || typeContainsRef(c->tt, vt);
    if (carries) arg = borrowSourceParam(c, value, &field);
    if (arg < 0 && viewy) {
        /* A `return` that names no parameter source is a second answer for this body, and the
         * storage it hands out may be younger than any borrow record would say. */
        if (planRetBorrowArg(f) >= 0) planSetRetBorrow(f, RET_BORROW_CONFLICT, -1);
        return;
    }
    if (planRetBorrowArg(f) == RET_BORROW_CONFLICT) return;
    if (planRetBorrowArg(f) < 0) {
        planSetRetBorrow(f, arg, field);
        return;
    }
    if (planRetBorrowArg(f) != arg || planRetBorrowField(f) != field)
        planSetRetBorrow(f, RET_BORROW_CONFLICT, -1);
}

/**
 * @brief Record a rejection that involved a level number, for a later review.
 *
 * Nothing is reported here; the value, the level, and the depth seen at the time are kept.
 *
 * @param c      Checker; records are appended to its rejection table.
 * @param val    Value that was rejected.
 * @param at     Depth used for the decision.
 * @param depth  Depth seen at the time of the check.
 * @param line   Source line.
 */
void recordLvlRejection(Checker *c, Expr *val, int at, int depth, int line) {
    if (!c || !val) return;
    LvlRejection *lr = (LvlRejection *)arenaAllocZero(c->arena, sizeof(LvlRejection));
    lr->val   = val;
    lr->at    = at;
    lr->depth = depth;
    lr->line  = line;
    *(LvlRejection **)vecPush(&c->lvlRejects) = lr;
}

/**
 * @brief Re-examine the recorded rejections with the numbers the passes have settled on.
 *
 * Only the late depth is stored and one debug line may be printed; the diagnostic already reported
 * is not changed. The report's decision and this review are kept apart, because reporting from here
 * alone would lose rejections: the check's reader and this one do not always see the same number.
 *
 * @param c  Checker.
 */
void recheckLevelRejections(Checker *c) {
    for (size_t i = 0; i < c->lvlRejects.len; i++) {
        LvlRejection *lr = *(LvlRejection **)vecAt(&c->lvlRejects, i);
        if (!lr) continue;
        int d = targetDepth(c, lr->val);
        lr->late = d;
        if (dbgOn("EXTC_DBG_DEFER"))
            fprintf(stderr, "[defer] line=%-4d at=%-2d check=%d settled=%d %s\n",
                    lr->line, lr->at, lr->depth, d,
                    d <= lr->at ? "DISAGREE" : "agree");
    }
}

void recordLvlFact(Checker *c, Expr *val, int at);
static void applyLvlFact(Checker *c, Expr *val, int at);

/* Forward declaration of a value's root origin, defined below at `originOf`. */
Expr *originOf(Checker *c, Expr *val, int hops);

/**
 * @brief Record the origin of a binding's value and flatten the carrier chain to its root.
 *
 * The origin has to be flattened: an intermediate binding in the chain may have left its scope by
 * the time it is traced again, `lookup` would miss it, and the chain would break. A binding whose
 * address was taken is not recorded.
 *
 * @param c   Checker.
 * @param sy  Binding being assigned or initialized.
 * @param val Expression whose origin is recorded.
 */
void noteOrigin(Checker *c, Sym *sy, Expr *val) {
    if (!sy || sy->addressed) return;
    /* A literal must not overwrite an origin that can still be traced. The check runs in source
     * order: the `p = x` arm records the allocation site first, and if a later `p = null` overwrote
     * it, a following `out = p` could not trace the site, the site would stay at block level, and
     * the value would dangle.
     *
     * The other direction matters as well: a binding initialized as `= null` must record that null,
     * because otherwise `origin` stays NULL and `promoteInto2` exits early on "no origin" instead of
     * walking the branches. */
    bool literal = val && (val->kind == EX_NULL || val->kind == EX_INT || val->kind == EX_FLOAT ||
                           val->kind == EX_BOOL || val->kind == EX_STR);
    bool haveReal = sy->origin && sy->origin->kind != EX_NULL && sy->origin->kind != EX_INT &&
                    sy->origin->kind != EX_FLOAT && sy->origin->kind != EX_BOOL &&
                    sy->origin->kind != EX_STR;
    if (literal && haveReal) return;
    sy->origin = originOf(c, val, 0);
}

/**
 * @brief Carry a lifetime requirement from a value read out of a container back to what was put in.
 *
 * A value taken out of a container (`v.get(k)`) may be **the very reference stored on the way in**:
 * reading an element creates no new storage, so the life it hands back is the life of the copy
 * inside the container. The container's own field table holds those sources (`callSrc` / `otherSrc`
 * / field slots; `checkCallRefArgs` publishes them when the call site sees `v.push(p)`), so lift
 * them to `at` as well -- this only lengthens a life, the safe direction. Without it, a `p` stored
 * by `v.push(p)` was only required to live to block level, while the chain handed out by
 * `return head` already points at it, and the function's return leaves it dangling.
 *
 * This must also run on the place-building path: "the value lands in the place at level X" says
 * nothing about what the callee stored there, and a container's every method may reach a place.
 * Skipping the walk there left the pushed elements at block level while the storage went to the
 * home place -- the `P2-6` / `P2-7` use-after-free.
 *
 * Only an `EX_IDENT` receiver is accepted, and it is resolved with `lookup` rather than `placeRoot`:
 * this runs during the **final-round replay** too, when the scope has been closed, and `placeRoot`
 * asserts that a scope exists; `lookup` simply finds no local binding and the step is skipped
 * silently (the checking pass already lifted what had to be lifted).
 *
 * @param c     Checker.
 * @param val   Call expression (method call; anything else is ignored).
 * @param at    Level the value has to reach.
 * @param hops  Carrier steps already taken.
 */
static void promoteReceiverFields(Checker *c, Expr *val, int at, int hops) {
    if (!val || val->kind != EX_METHOD || !val->u.method.recv) return;
    Expr *recv = val->u.method.recv;
    if (recv->kind != EX_IDENT) return;
    Sym *rv = lookup(c, recv->u.ident.name);
    if (rv) promoteFieldsAt(c, rv, at, hops + 1);
}

/**
 * @brief Walk a value's carriers and promote the allocation sites inside it to the level the value
 *        has to reach.
 *
 * `new` allocates into the current block's arena and the block end frees it, so a value stored
 * somewhere that outlives the block has to move to an arena at least as long-lived. The rule is
 * `arenaLevel = min(current block depth, depth of every destination)`: a smaller depth means a
 * longer life, so the minimum follows the longest-lived destination; storing only into deeper
 * places does not change the minimum and leaves the behaviour unchanged. Promotion is always the
 * safe direction -- it lengthens the pool, and a longer life cannot leave a dangling reference.
 *
 * Only the value's carriers are followed; no aliases and no interprocedural reasoning. A carrier
 * that cannot be moved falls back to the depth check, so this function never lets through something
 * that should be rejected. The depth cached on a parent may only be tightened when every child
 * succeeded, since otherwise it would record a longer life than the truth, which is a hole rather
 * than a false rejection. This function is also the only entry point for level facts, so a path
 * that returns early still has to walk far enough to record them.
 *
 * @param c     Checker.
 * @param val   Value being stored or returned.
 * @param at    Level the value has to reach; 0 means it must outlive this frame (the caller's
 *              arena).
 * @param hops  Carrier steps already taken, bounded by `EXTC_PROMOTE_MAX_HOPS` (bindings can be
 *              cyclic).
 * @return      `true` when every `new` inside the value reached `at`; `false` when some carrier
 *              cannot be moved.
 */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops) {
    if (!val) return true;
    /* A level that cannot be described: nothing is moved. */
    if (at < 0) return false;
    /* A hop limit is required: bindings can be cyclic (`a = b  b = a`, because assignment updates
     * the origin), and without a limit the walk would loop forever. Hitting the limit means the value
     * cannot be moved, and the caller falls back to reporting -- the safe direction. */
    if (hops > EXTC_PROMOTE_MAX_HOPS) { EXTC_DBG_NOTE("promoteInto2: recursion budget exhausted"); return false; }
    /*
     * `dyn Trait(x)`: the payload is copied into the pool and the handle keeps it alive, so the
     * payload's allocation sites have to reach the level this value has to reach. The pool is born at
     * the level the site names, so that level has to reach `at` as well -- the same rule as the
     * pool-building call below: `want = at == 0 ? ZONE_HOME : at`. A pool born at a deeper level while
     * the handle is stored outside is a dangling reference; this lift is what keeps
     * `let d: dyn T = dyn T(x)` valid once the block ends.
     */
    if (val->kind == EX_DYN) {
        /* Lift the pool's level first, then recurse into the payload: the payload walk can answer
         * "cannot lift" halfway (one conversion inside a struct literal is enough), and the pool's
         * level must not depend on that answer. Left at block level, the depth check would compare
         * against a shorter life than the truth and reject `return dyn T(x)`. */
        if (planZoneLevel(val) != 0) {
            int want = (at == 0) ? ZONE_HOME : at;
            if (planZoneLevel(val) > want) planSetZoneLevel(val, want);
        }
        return promoteInto2(c, val->u.dynv.payload, at, hops + 1);
    }
    /* Record a fact along the way; it changes no behaviour. Only the outermost call (`hops == 0`)
     * records it: the replay runs this level, the inner levels are reached by this call itself, and a
     * second record would only repeat the work. */
    if (hops == 0) {
        if (dbgOn("EXTC_DBG_FACT"))
            fprintf(stderr, "[fact] %-8s at=%d kind=%d minAt=%d lexi=%d line=%d\n",
                    c->curFunc?c->curFunc->name:"?", at, (int)val->kind,
                    anMinAt(val), anLexicalLevel(val), val->line);
        recordLvlFact(c, val, at); applyLvlFact(c, val, at);
    }
    switch (val->kind) {

    /* `EX_GENCALL` is `alloc<T>(n)`, exactly symmetric to `new`: the same level rules, the same site
     * table, and the same promotion rule, so both cases are handled by the same arm. */
    case EX_NEW:
    case EX_GENCALL: {
        /* A destination depth of 0 means "the caller's level", the home arena. Only a function with a
         * home can reach it; without one nothing can be lifted, and `false` lets the caller report --
         * the safe direction. That tier is represented by `ARENA_HOME` rather than by 0, because in
         * the current encoding 0 means "not yet decided", so the value has to be translated before it
         * is written back. */
        if (at == 0 && !(c->curFunc && anNeedsHome(c->curFunc))) return false;
        /* Reaching this point means the escape analysis met this site by walking the carriers of a
         * value, not through the "the enclosing function has a home" fallback. A site found this way is
         * really required to outlive this frame, so mark it. This must happen before the early returns
         * below. */
        /* Record the strongest requirement the escape analysis made on this value: a smaller level is
         * a stronger requirement, so keep the minimum. A boolean would not do: being seen is not the
         * same as being required to outlive the frame -- `nb = new item[cap*2]` only has to live until
         * the end of the current frame, and treating it as "goes home" would make `main`, which has no
         * home, emit `__extc_home`. */
        if (anMinAt(val) < 0 || at < anMinAt(val)) anSetMinAt(val, at);
        /* Already in the home arena: home lives longest, so nothing can be lifted. */
        if (planArenaLevel(val) == ARENA_HOME) return true;
        int target = (at == 0) ? ARENA_HOME : at;
        if (planArenaLevel(val) > target) planSetArenaLevel(val, target);
        /* The only place in the checker that converts a level number back to a depth. */
        int depth = arenaDepthOf(planArenaLevel(val));
        if (anRefDepth(val) > depth || anRefDepth(val) == 0)
            anSetRefDepth(val, depth);
        return true;
    }

    case EX_IDENT: {
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[promote] EX_IDENT %s src=%s typeRefs=%d at=%d\n",
                    val->u.ident.name ? val->u.ident.name : "-",
                    (lookup(c, val->u.ident.name) && lookup(c, val->u.ident.name)->origin) ? "yes" : "no",
                    (int)typeContainsRef(c->tt, tsub(c, val->type)), at);
        /*
         * Walk back along the binding's origin: `var n = new node` or `mid = n` leads to that `new`.
         */
        Sym *sy = lookup(c, val->u.ident.name);
        if (sy && !sy->origin && sy->poolSite) {
            /* The binding was initialized by a call (`var inner = vector<i32>::new()`), and `origin`
             * is empty for calls (it only recognises the shape of an allocation site). The pool is
             * different: its level is decided by the call site, so follow the pool site instead; the
             * next hop is the call arm, which knows that a pool-building call can be lifted. */
            if (getenv("EXTC_DBG_ZONE"))
                fprintf(stderr, "[promote] EX_IDENT %s -> poolSite %s\n",
                        val->u.ident.name ? val->u.ident.name : "-",
                        sy->poolSite->kind == EX_CALL ? "EX_CALL" :
                        sy->poolSite->kind == EX_METHOD ? "EX_METHOD" : "EX_ASSOC");
            int nHops = hops + 1;
            bool moved = promoteInto2(c, sy->poolSite, at, nHops);
            /* Claiming the value moved is only honest if its **storage** moved. A pool read
             * (`k.atDense(i)`) is a borrow: the bytes stay in `k`'s pool, and the pool is born at
             * the place where it was created. `promoteInto2` on the call lowers the call's own
             * level and returns true while that pool keeps its birth place, which is how a value
             * could be accepted into an outer container and then freed with the loop body (gap G1).
             * Report failure here so the caller keeps the error. */
            if (moved && calleeMakesPool(planCallee(sy->poolSite))) {
                int zl = planZoneLevel(sy->poolSite);
                if (zl != 0 && zl > at) moved = false;
            }
            return moved;
        }
        if (!sy) return false;
        if (!sy->origin) {
            /* A binding without an initializer (`var bx: outer`, `var arr: [2]?mut ref node`): its
             * sources can only be in the field table -- a site written as `bx.mid.next = p` has nowhere
             * else to be reached from (`origin` is NULL). Returning early here would skip the whole
             * field table, and the site would stay at the lexical level and die with the frame. So
             * still run `promoteFieldsAt`: its job is to pass the requirement on to the real allocation
             * sites. The decision itself is unchanged (`false` hands it to the caller's depth
             * comparison); the facts have been recorded all the same.
             *
             * A binding without an initializer has its writes recorded per field; sources that cannot
             * be named here (a write through a dereference) never reach this point, so the rule is not
             * weakened. */
            promoteFieldsAt(c, sy, at, hops + 1);
            return false;
        }
        /* The slot already outlives `at`, so it sits at a shallower level and its contents already
         * live that long: the initializer is evaluated in the same statement, and the level number is
         * the slot's depth.
         *
         * A call result is the exception: which arena a callee with a home allocates into is decided
         * by the call site and need not be the slot's level, so that depth is recorded into
         * `Sym.refDepth` when the binding is initialized, and that record is what counts. */
        /* This branch must **not** return early. "The slot lives long enough, so nothing has to be
         * lifted" holds for the lifting itself, but `promoteInto` is also the only entry point for
         * level facts; returning early would leave the site with no constraint at all, and the final
         * round would put it back at the lexical level as if nothing had touched it. So the walk
         * always goes on, if only to record facts; when the slot is deep enough the return value still
         * answers "nothing to change" (`slotDeepEnough` below). */
        bool slotDeepEnough = (sy->depth <= at);
        /* The origin can be **self-referential** (`c = c.next` has `origin` `c.next`, whose object is
         * `c` again): following it would spin forever. The hop limit only truncates the spin, and
         * after truncation `promoteFieldsAt(hops + 1)` would enter with a hops count above the limit
         * and trip `EXTC_DBG_FALLBACK` (red in the assertion build). Deduplicate by binding instead:
         * within one walk each binding's origin is followed once; the requirement is being pushed
         * along that path, and going round again adds no information. */
        bool onStack = false;
        for (int i = 0; i < c->fsDepth; i++)
            if (c->fsChain[i] == sy) { onStack = true; break; }
        bool ok = true;
        if (!onStack) {
            c->fsChain[c->fsDepth++] = sy;
            /* The origin was flattened when it was recorded (see `noteOrigin`), so this is at most
            * one step and never looks up the same binding twice. */
            ok = promoteInto2(c, sy->origin, at, hops + 1);
            c->fsDepth--;
        }
        /*
         * The origin only covers the fields written at the declaration, so read the field table's
         * sources as well: a later `h.q = x` exists only in the field table.
         */
        if (!promoteFieldsAt(c, sy, at, hops + 1)) ok = false;
        if (!ok) return false;
        if (slotDeepEnough) return true;
        if (sy->type && typeContainsRef(c->tt, tsub(c, sy->type)) && anRefDepth(sy) > at)
            anSetRefDepth(sy, at);
        if (anRefDepth(val) > at || anRefDepth(val) == 0) anSetRefDepth(val, at);
        return true;
    }

    case EX_SIGN:
        return promoteInto2(c, val->u.sign.operand, at, hops + 1);

    /* Three more shapes are walked here. `promoteInto` has to be able to walk a value's carriers all
     * the way to the allocation inside, otherwise the fact "this value escaped" would never be found,
     * and `o.f = cell` / `a[i] = cell` are the most common shapes. The discipline matches struct
     * literals and array literals: carriers only, no aliases, no interprocedural reasoning; a carrier
     * that cannot be lifted answers `false` and the caller reports. */
    case EX_SLICE:
        return promoteInto2(c, val->u.slice.obj, at, hops + 1);
    case EX_FIELD:
        return promoteInto2(c, val->u.field.obj, at, hops + 1);
    case EX_INDEX:
        return promoteInto2(c, val->u.index.obj, at, hops + 1);

    case EX_COALESCE: {
        /* Try both sides, without short-circuiting: lifting one side only would miss the `new` on the
        * other. */
        bool a = promoteInto2(c, val->u.coalesce.main, at, hops + 1);
        bool b = promoteInto2(c, val->u.coalesce.fallback, at, hops + 1);
        return a && b;
    }

    case EX_STRUCTLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.lit.inits.len; i++)
            if (!promoteInto2(c, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value, at, hops + 1))
                ok = false;
        if (ok && anRefDepth(val) > at) anSetRefDepth(val, at);     /* tighten the cached depth too */
        return ok;
    }

    case EX_ARRAYLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.arraylit.elems, i), at, hops + 1)) ok = false;
        if (ok && anRefDepth(val) > at) anSetRefDepth(val, at);
        return ok;
    }

    case EX_ENUMVAL: {                       /* a payload construction: promote the payload too */
        bool ok = true;
        for (size_t i = 0; i < val->u.enumval.args.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.enumval.args, i), at, hops + 1)) ok = false;
        if (ok && anRefDepth(val) > at) anSetRefDepth(val, at);
        return ok;
    }

    case EX_BIN:
    case EX_UN:
        /* An operator that resolved to a method is a call: `a + b` reaches `string::+`, whose body
         * calls `concat`, which builds the pool. A builtin operator allocates nothing, so the
         * no-callee case keeps the old answer `false` and the caller still reports. The pool branch
         * below is what a call site needs and it reads only the site's own facts, so it is repeated
         * here rather than reached through the shared `u.method` fields (an `EX_BIN` has no
         * receiver there). */
        if (!planCallee(val)) return false;
        return lowerPoolSite(val, at);

    case EX_CALL:
    case EX_METHOD:
    case EX_ASSOC:
        /* A pool-building call is an **allocation site** and can be lifted like `new`: a fresh
         * allocation has no other owner, so the "which level is the pool born at" number can be
         * lowered (smaller is longer-lived). That is what lets a container go into a longer-lived
         * container: `outer.push(inner)` requires `inner` to live as long as `outer`. Other call
         * results stay unliftable: a `slice` view borrows someone else's storage, so it cannot be
         * lifted and the caller reports. */
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[gate] call=%s ptr=%p owner=%p mP=%d cmp=%d lvl=%d at=%d\n",
                    planCallee(val) && planCallee(val)->name ? planCallee(val)->name : "-",
                    (void *)planCallee(val), (void *)(planCallee(val) ? planCallee(val)->owner : NULL),
                    planCallee(val) ? (int)planMakesPool(planCallee(val)) : -1,
                    (int)calleeMakesPool(planCallee(val)), planZoneLevel(val), at);
        /* A callee that hands back a **view of storage that already exists** names the argument the
         * storage belongs to, and that storage is what has to reach `at`; the call creates nothing,
         * so its own level takes no part. When the named storage cannot be moved the branches
         * below still answer, and they only keep the answer conservative. */
        {
            FuncDef *bc = borrowResultType(c, val->type) ? borrowCallee(planCallee(val)) : NULL;
            if (bc) {
                Expr *arg = callArgForParam(val, planRetBorrowArg(bc));
                if (promoteBorrowed(c, arg, borrowFieldName(c, bc, planRetBorrowField(bc)), at, hops)) {
                    settleBorrowDepth(c, val, at);
                    if (anRefDepth(val) == 0 || anRefDepth(val) > at) anSetRefDepth(val, at);
                    return true;
                }
            }
        }
        if (lowerPoolSite(val, at)) {
            /* Lowering the level of the **pool** says where the plate lives; it says nothing about
             * what the callee stored *into* that plate. The two obligations are separate: the pool's
             * birth level is decided here, the elements' life is decided by the field table. */
            promoteReceiverFields(c, val, at, hops);
            return true;
        }
        /* When the callee **needs a home arena** (`needsHome`), the new allocation goes into the
         * arena **the call site passed in**: that number is chosen by the call site, so it can be
         * lifted like `new` (smaller is longer-lived). That is why `return mk(1)` passes
         * (`markCallHomeIfEscaping(..., 0)` gives `__extc_home`); `var p = mk(1)` records the arena as
         * block level only, the lift requirement cannot get back to the call site, and `return p` is
         * wrongly rejected. `arenaArg` and `homeDepth` must move together: the first is read by
         * codegen, the second by `checkCallRefArgs` as the destination depth. Only ever move towards
         * the longer-lived side (monotone); a level that is already longer-lived is not raised again.
         *
         * This is done only for a **reference** result: it names the object the callee just allocated,
         * so changing the call site's arena level moves it where it belongs. A by-value aggregate
         * (`box { p: mut slice<i32> }`) has its own conservative rule for the view inside it (the
         * standing counterexample in `tests/errors/escape_call_result_to_outer.extc`), which is
         * outside this step. */
        if (planCallee(val) && anNeedsHome(planCallee(val)) && val->type &&
            tsub(c, val->type)->kind == TY_REF) {
            int want = (at == 0) ? ARENA_HOME : at;
            int cur = planArenaArg(val);
            bool longEnough = (cur == ARENA_HOME) || (cur >= 1 && cur <= want);
            if (!longEnough) {
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[promoted-call] %s arenaArg %d -> %d (at=%d)\n",
                            planCallee(val)->name ? planCallee(val)->name : "-", cur, want, at);
                planSetArenaArg(val, want);
                anSetHomeDepth(val, want);
                planSetArenaArgPending(val, false);
            }
            if (anRefDepth(val) > at) anSetRefDepth(val, at);
            return true;
        }
        /* The decision is unchanged (still `false`, left to the caller's depth comparison): only the
         * facts are completed here. See `promoteReceiverFields` for why the walk is needed, and why
         * the pool-building branch above must run it too. */
        promoteReceiverFields(c, val, at, hops);
        return false;

    default:
        /* Everything else is unliftable (a `ref` to a local, a borrow, ...). No guessing: fall back
        * to the original depth check and report. */
        return false;
    }
}

/**
 * @brief Record the level fact "this value has to fit into some level".
 *
 * It is recorded on the site rather than on the binding: the replay runs after every function
 * body has been checked, and local bindings cannot be found by then. An identifier is
 * replaced by its origin site or its pool site. Nothing is recorded while the level solve is
 * running, so the fact table cannot grow without bound.
 *
 * @param c    Checker; records are appended to `c->lvlFacts`.
 * @param val  Value being stored or returned.
 * @param at   Level the value has to fit into; negative values are ignored.
 *
 * "must outlive this frame" is represented by `ARENA_HOME` rather than by a level
 */
void recordLvlFact(Checker *c, Expr *val, int at) {
    /* `at == 0` is recorded too: for an arena it means "outlive this frame (the home arena)",
     * for a pool it means `ZONE_HOME` (the place the caller chooses). **Return** is exactly
     * that `at == 0` case; without it, the replay after the closure has no such record and a
     * container returned by the function can never reach the caller's level. */
    if (!c || !val || at < 0) return;
    /* Recorded on the **site**, not on the binding: the final replay runs after every body
     * has been checked, and by then `lookup` cannot find the local bindings of that time
     * (the symbol table is per function / per scope), so a record on a binding would be a chain
     * nobody can walk. The pool case is "the binding was initialised by a call" (`origin` is
     * empty for calls); such a binding carries `poolSite`, which is substituted. */
    if (val->kind == EX_IDENT) {
        /* Take the name **before** `val` is re-pointed at the origin: afterwards `val` may be
         * any kind, and reading `u.ident.name` off it reads another kind's union member -- a
         * wild pointer that would make the `[fact-sy]` dump below segfault. */
        const char *nm = val->u.ident.name ? val->u.ident.name : "-";
        Sym *sy = lookup(c, val->u.ident.name);
        if (sy && sy->origin) val = sy->origin;
        else if (sy && sy->poolSite) val = sy->poolSite;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[fact-sy] %s sy=%p origin=%p poolSite=%p\n",
                    nm, (void *)sy,
                    sy ? (void *)sy->origin : NULL, sy ? (void *)sy->poolSite : NULL);
    }
    /* Never record while the solver is running: the solve re-enters this function, and
     * recording there would grow the fact table without bound. */
    if (c->lvlSolving) return;
    LvlFact *f = (LvlFact *)arenaAllocZero(c->arena, sizeof(LvlFact));
    f->val = val;
    f->at  = at;
    *(LvlFact **)vecPush(&c->lvlFacts) = f;
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[fact] kind=%d name=%s -> kind=%d at=%d\n", (int)f->val->kind,
                f->val->kind == EX_IDENT && f->val->u.ident.name ? f->val->u.ident.name : "-",
                (int)val->kind, at);
}

/**
 * @brief Apply one level fact to its site, keeping the strongest requirement (the smallest
 *        level).
 *
 * Recording the fact and changing the level are two things, but they run in the same traversal
 * (both at the entry of `promoteInto2`), so no site is missed.
 *
 * @param c    Checker; the requirement lives on the expression node, so this parameter is
 *             unused.
 * @param val  Value the constraint applies to.
 * @param at   Level the value has to reach; negative values are ignored.
 */
static void applyLvlFact(Checker *c, Expr *val, int at) {
    (void)c;
    if (!val || at < 0) return;
    if (anMinAt(val) < 0 || at < anMinAt(val)) anSetMinAt(val, at);
}

/**
 * @brief Find the root origin of a value.
 *
 * Walks the carrier chain until it reaches an expression that is not a binding with a known
 * origin; that is where the value really came from. Flattening the chain here is what keeps the
 * origin usable later: an intermediate binding may have left its scope by then and `lookup`
 * would miss it.
 *
 * @param c     Checker.
 * @param val   Expression to find the origin of.
 * @param hops  Binding steps already taken; stops after 32.
 * @return      The root expression; `val` itself when there is no origin to follow.
 */
Expr *originOf(Checker *c, Expr *val, int hops) {
    if (!val) return val;
    if (hops > 32) { EXTC_DBG_NOTE("carrier walk: recursion budget (32) exhausted"); return val; }
    /* A struct literal, `new` and the like are their own origin. */
    if (val->kind != EX_IDENT) return val;
    Sym *sy = lookup(c, val->u.ident.name);
    /* No origin => the answer is the value itself, and it cannot be promoted. */
    if (!sy || !sy->origin) return val;
    return originOf(c, sy->origin, hops + 1);
}

/**
 * @brief Tell whether a store writes an element through a view.
 *
 * The storage behind a view is a flat array of values the container owns: an element store writes
 * the bytes and nothing else. For a plain value that is exactly right; for a value that owns a
 * reservation (a `@drop` type) it produces a second owner that no hook ever releases for the copy,
 * and the original name releases it a second time. `buf<T>::push` does exactly this
 * `self.vals[self.n] = v`, so the rule in `checkStoreEscape` and its deferred twin in
 * `runRefCheck` are what keep a container from holding an owning element.
 *
 * @param target  Left side of an assignment; may be NULL.
 * @return        True when it is `view[i]` (any projection whose immediate object is a view).
 */
static bool storesIntoView(const Expr *target) {
    if (!target || target->kind != EX_INDEX) return false;
    const Expr *obj = target->u.index.obj;
    if (!obj || !obj->type) return false;
    return ttIsViewType(obj->type);
}

/**
 * @brief Every escape rule a single store has to satisfy.
 *
 * Beyond the depth rule, a borrowed value may not be stored where it outlives the call. When
 * the target depth is 0, the value traces to a parameter, and the destination traces to a
 * parameter, the call site decides the lifetime instead. A type that mentions a type parameter
 * records a deferred check.
 *
 * Two different numbers are in play: the level the store is accounted at, which the container's
 * own scope caps, and the depth the store is checked against, which is the depth of the storage
 * the place denotes. Only the first is capped, because a projection such as `h.q` reports a
 * longer life than the container itself and the second number is what the reference rule asks
 * about.
 *
 * @param c       Checker.
 * @param val     Value being stored.
 * @param target  Place it is stored into.
 * @param line    Source line for the diagnostic.
 * @return        `true` when the store is rejected; `false` when it is allowed.
 */
bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    /* A value whose type mentions a type parameter defers the whole check: return here so that
     * `checkEscape` below does not record a second deferred check. */
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, target, slotDepth(c, target), line, "this assignment");
        return false;
    }
    /* A value that owns a reservation may live in a view element only by being **moved** in: the
     * store copies the bytes and no hook runs for that copy, so the destination becomes the one
     * owner and the source has to give its ownership up here. A plain name does that -- the
     * generator stops running its hook when the block exits -- and a temporary has no owner to
     * drop it. A place that is not a name -- a field, another element -- cannot give ownership up
     * without leaving its aggregate half-owned, so that stays rejected.
     *
     * The move is recorded by the generator, not here: the same name may be stored on several
     * control-flow paths (`linMap::put` stores on the found-key path and again after `grow`), and
     * a name is only dead once every path that stores it has been taken. Marking it here would
     * reject the second store as a read of a moved value. Named here for the concrete case, and
     * again in `runRefCheck` for the case a template sees only as `T`. */
    if (storesIntoView(target) && typeHasDrop(c, val->type, 0)) {
        if (val->kind == EX_IDENT || !isPlace(val)) return false;
        ckError(c, line,
                "A view is a flat array of values: storing writes the bytes and the container never"
                " runs a `@drop` hook for that copy, so the value would be released twice -- once"
                " here and once when the original name dies. Keep the element type plain, or own"
                " the value outside the container.",
                "cannot store `%s` into a view element: it owns a reservation and the copy would"
                " never be dropped", typeStr(c, val->type));
        return true;
    }
    /* The question here is which level the value is stored into, so `storeLayer` is used
     * rather than `slotDepth` (the trap is described at `storeLayer`). */
    int at = storeLayer(c, target);
    /* The level this store needs is capped by the container's lifetime. For a projection such
     * as `h.q` or `a[i]`, `storeLayer` answers with the depth of the projected path, which is
     * deeper than the container's own block level (`h.q` answers 2 while `h` sits at 1), and
     * the container cannot outlive its scope, so what is stored into it only has to live that
     * long. Only this accounting number is lowered; the `at` passed to `checkEscape` below is
     * left untouched. */
    int atStore = at;
    if ((int)c->scopes.len < atStore) atStore = (int)c->scopes.len;
    /* Try the promotion first. When it cannot move anything, the call below still reports, so
     * this is a safety net, and it is idempotent. */
    recordStore(c, val, target, atStore, line);
    /* Promotion moves a **site** down a level; it cannot move bytes that belong to someone else.
     * A value that borrows storage (`k.atDense(0)` handing back the pool's own bytes) is therefore
     * left alone: promoting it would report success while the pool stays where it was born, and the
     * field would hold a handle to freed bytes (the same misuse as `borrowDepthAnywhere` fixes in
     * `checkCallRefArgs`). Fresh allocations still promote as before. */
    {
        int bd = 0;
        if (!borrowDepthAnywhere(c, val, &bd)) promoteInto(c, val, atStore);
    }
    bool bad = checkEscapeDefer(c, val, at, line, "this assignment", target);
    /* When the value's origin traces to a parameter, accept the store and leave the lifetime to
     * the call site.
     *
     * The callee is compiled once and does not know the caller's pool, so all it can do is
     * publish the constraint "the data behind argument j lives at least as long as the
     * container the callee stores it into"; the call site can evaluate that constraint.
     *
     * The relaxation needs two preconditions, both required: 1) the value's origin traces to a
     * formal parameter, so the call site can compute its lifetime; 2) the destination is a
     * container reachable through a parameter, so its storage is the pool of the corresponding
     * argument at the call site. Precondition 2 cannot be dropped: when the destination is a
     * global (`g = s`), the constraint is "must live forever (depth 0)", which the call site
     * cannot express, so the store stays rejected. A same-level call, by contrast, is legal:
     * the call and the destination die together. Values whose origin cannot be traced (call
     * results, frame locals, unknown origins) are still rejected.
     */
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
        /* Inside a function that has a home arena the store is accepted: the call site has
         * already guaranteed that every `ref` / `mut ref` argument lives at least as long as
         * this call's home arena, and whatever the callee allocates goes into that same home
         * arena, so the stored value lives exactly as long as the destination. Accepting
         * without that guarantee would be a hole. */
        /* The reason does not hold when the destination is a **global**: static storage
         * outlives every arena, and no home arena can make a borrow safe. So the destination is
         * asked where it is instead of being assumed: a global target falls into the error
         * branch. */
        Sym *destRoot = target ? placeRoot(c, target) : NULL;
        if (c->curFunc && anNeedsHome(c->curFunc) && !(destRoot && isGlobalSym(c, destRoot)))
            return bad;
        ckError(c, line,
                "A borrowed value may not be stored where it outlives the call: its real "
                "lifetime is unknown here. Copy it, or store it into a local of this frame.",
                "cannot store a borrowed value into something that outlives this call");
        return true;
    }
    return bad;
}

/**
 * @brief Defer the size check of `new T[n]` to instantiation.
 *
 * It is recorded only inside a generic body (the visible type parameters are non-empty); a
 * concrete type can be decided on the spot, and deferring would only make the error arrive
 * later.
 *
 * @param c     Checker; records are appended to `c->refChecks`.
 * @param t     Declared element type.
 * @param line  Source line for the diagnostic.
 */
void recordNewSizeCheck(Checker *c, Type *t, int line) {
    /* A free generic function needs this record just as a generic method does: the replay loop
     * walks `c.funcInsts` and the rule is the same. Gating on `owner` would drop the `new T[n]`
     * size check in `fn f<T>(n: i64) -> mut slice<T>`. */
    if (!c->curFunc) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->isNewSize = true;
    rc->declType  = t;
    rc->line      = line;
    rc->func      = c->curFunc;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/**
 * @brief Defer the zero-value check of a declaration without an initializer to instantiation.
 *
 * Inside a generic body, whether `T` has a zero value is only known once the type argument is,
 * so the check is recorded here and decided at instantiation. It is recorded only when the
 * visible type parameters are non-empty.
 *
 * @param c     Checker; records are appended to `c->refChecks`.
 * @param t     Declared type.
 * @param line  Source line for the diagnostic.
 * @param name  Binding name, for the diagnostic.
 */
void recordZeroCheck(Checker *c, Type *t, int line, const char *name) {
    /* A free generic function needs this record just as a generic method does: the replay loop
     * walks `c.funcInsts` anyway and the rule is the same -- a `T` without a zero value cannot
     * be default-initialized. Looking only at `owner` would miss free generic functions. */
    if (!c->curFunc) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->isZero   = true;
    rc->declType = t;
    rc->line     = line;
    rc->what     = name;
    rc->func     = c->curFunc;
    rc->ctx      = c->ctx;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/**
 * @brief Record a deferred reference check, to be decided at instantiation.
 *
 * It is recorded only inside a generic body and only when the value mentions a type parameter;
 * a concrete type can be decided on the spot, and deferring would only make the error arrive
 * later. The depth and the borrowed flag are computed now, while the scope is open and
 * `lookup` still finds the parameters: at instantiation the function scope is gone, the depth
 * would answer 0, and the check would silently do nothing.
 *
 * @param c       Checker; records are appended to `c->refChecks`.
 * @param val     Value whose depth is checked.
 * @param target  Place it is stored into; NULL for a return.
 * @param at      Depth the value has to reach; 0 means it must outlive this frame.
 * @param line    Source line for the diagnostic.
 * @param what    Operation being checked, named in the diagnostic.
 */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what) {
    /* Same rule as `recordZeroCheck`: a free generic function stores references too, and at
     * instantiation the scope is gone, so the check has to be recorded now or never. */
    if (!c->curFunc) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->val    = val;
    rc->target = target;
    rc->at     = at;
    rc->line   = line;
    rc->what   = what;
    rc->func   = c->curFunc;
    /* Fourth kind (see `viewElem`): an owning value stored into a view element. The element type
     * is opaque while the template is checked, so the decision waits for the instance. */
    rc->ctx      = c->ctx;
    rc->viewElem = storesIntoView(target);
    /* Whether that store is a **move** into the slot, decided exactly as the concrete case in
     * `checkStoreEscape` decides it: a plain name hands its ownership over, a temporary has no
     * owner to drop it. Neither is marked moved in the checker (see `checkStoreEscape`). */
    if (rc->viewElem && (val->kind == EX_IDENT || !isPlace(val))) rc->viewMove = true;
    /* Compute these numbers now, while the scope is open and `lookup` still finds the
     * parameters. The walk does not stop early for a node that mentions a type parameter, so
     * this depth is computed under the assumption "the parameter may carry a reference", which
     * is exactly the number the instance check needs. */
    rc->depth    = targetDepth(c, val);
    anSetBorrowed(rc, exprBorrowed(c, val));
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/* The explanation text for a depth rejection. Reported on the spot and reused when the
 * decision is reconsidered after the DFA (`recheckPendingRefStores`); the two paths must agree
 * word for word. */
#define ESCAPE_OUTLIVE_NOTE \
    "A reference may not outlive what it points to. Borrow from a parameter " \
    "(depth 0) or copy the data instead. " \
    "If this value is a container (varArray and friends), the storage inside it " \
    "was allocated in this frame's arena: build the container in the caller's " \
    "scope and fill it through a `mut ref` argument, or build it here with `new` " \
    "(that allocates in this function's home arena, so it may escape)."

/**
 * @brief Implementation of `checkEscape`: with `defer` set, a rejection is recorded in
 *        `pendingRefStores` instead of being reported.
 *
 * Only "the destination is reached through a named reference" (`(*cur).f` / `*cur`) is
 * deferred: its `at` reads `anRefDepth(cur)`, a number the body walk writes in lexical order
 * with no join, and the DFA only runs after the walk. The record keeps the `at` / `depth` of
 * that moment, and reports them unchanged when the DFA overflows.
 *
 * @param c       Checker.
 * @param val     Value being stored or returned.
 * @param at      Destination depth.
 * @param line    Source line.
 * @param what    Operation name for the diagnostic.
 * @param target  Place being written; NULL when there is no destination (return,
 *                initialization).
 * @param defer   When true and the decision is a rejection, record instead of reporting.
 * @return        `true` when the decision is a rejection.
 */
static bool checkEscapeImpl(Checker *c, Expr *val, int at, int line, const char *what,
                            Expr *target, bool defer) {
    if (!val) return false;
    /* A value whose type mentions a type parameter cannot be decided now (`T` may be `i64` or
     * `slice<u8>`). Record it and decide at instantiation. */
    if (mentionsParam(val->type)) {
        /* Promote once, then defer. `promoteInto` is the only entry point for level facts, and
         * this branch returns early, so every place that returns or passes a value mentioning
         * `T` would otherwise leave that value with no constraint at all. Without the
         * promotion, `return v` inside a generic body leaves the `new T[cap]` it contains with
         * no fact (`minAt = -1`); the final pass would put it back at the block level as if
         * nothing had touched it, while the return value requires it to live beyond the frame,
         * and the instance check would then report a whole family of false rejections.
         *
         * Promoting now is sound because the level does not depend on what `T` is: "this
         * storage has to live until level k" is the same statement for `T = i64` and for
         * `T = slice<u8>`. The result is ignored: failing to promote is not an error, and the
         * deferred rule recorded here decides whether the store is rejected.
         */
        recordStore(c, val, NULL, at, line);
        promoteInto(c, val, at);
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
    /* Promotion has to run first, matching the order in `checkStoreEscape`: promote, then
     * compare depths. `promoteInto` is also the only entry point for level facts, and the
     * `d <= at` test below returns early, so once the depth looks shallow enough the site would
     * have no constraint at all. Facts first, comparison second; the criterion itself is
     * unchanged. */
    {
        /* Same rule as `checkStoreEscape`: promotion moves a **site**, so it can never make borrowed
         * bytes live longer than their pool. A value that borrows (a pool read handing back the
         * pool's own bytes) is compared as it is; only fresh allocations promote. */
        int bd = 0;
        if (!borrowDepthAnywhere(c, val, &bd)) promoteInto(c, val, at);
    }
    int d = targetDepth(c, val);
    if (d <= at) return false;
    /* The error below is still reported (or recorded when `defer`); this only leaves the
     * question to the pass that runs after the numbers are settled. */
    recordLvlRejection(c, val, at, d, line);
    if (defer) {
        PendingRefStore *p = arenaAllocZero(c->arena, sizeof *p);
        p->val = val;
        p->target = target;
        p->destSym = destDerefSym(c, target);
        p->line = line;
        p->at = at;
        p->depth = d;
        p->what = what;
        p->fn = c->curFunc;
        *(PendingRefStore **)vecPush(&c->pendingRefStores) = p;
        return true;
    }
    ckError(c, line, ESCAPE_OUTLIVE_NOTE,
            "%s would hold a reference to a local variable that dies first "
            "(borrowed from depth %d, but this can only hold up to depth %d)",
            what, d, at);
    return true;
}

/**
 * @brief Check that the references inside a value do not outlive the storage it is put into.
 *
 * Every allocation site in the value is promoted first and the depth comparison decides
 * afterwards, because returning early when the depth looks shallow enough would leave the
 * allocation sites unconstrained. When the value's type mentions a type parameter, a deferred
 * check is recorded and `false` returned, to be decided at instantiation.
 *
 * The compiler does not know the reference's real lifetime, and it may point at a local of the
 * caller's frame nested more deeply than this function.
 * This is one half of the same rule as "the depth of a call result is the maximum depth of its
 * arguments".
 * what - the operation being checked, named in the diagnostic ("this assignment")
 *
 * @param c     Checker.
 * @param val   Value being stored or returned.
 * @param at    Target depth; 0 means it must outlive this frame.
 * @param line  Source line for the diagnostic.
 * @param what  Operation being checked, named in the diagnostic.
 * @return      `true` when the value would leave a dangling reference and the error was
 *              reported; `false` otherwise.
 */
bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    return checkEscapeImpl(c, val, at, line, what, NULL, false);
}

/**
 * @brief Check with a destination: defer a rejection until after the DFA when the destination
 *        is reached through a named reference.
 *
 * Only that path (`(*cur).f` / `*cur`) reads an `anRefDepth` written in lexical order, so the
 * order changes the answer; every other shape is reported on the spot, unchanged.
 *
 * @param c       Checker.
 * @param val     Value being stored.
 * @param at      Destination depth (computed in the walk from that moment's `anRefDepth`).
 * @param line    Source line.
 * @param what    Operation name for the diagnostic.
 * @param target  Place being written; NULL is the same as `checkEscape`.
 * @return        `true` when the decision is a rejection (the deferred ones report nothing).
 */
bool checkEscapeDefer(Checker *c, Expr *val, int at, int line, const char *what, Expr *target) {
    /* Only "the destination is reached through a named reference" is deferred: that path's
     * `at` reads an `anRefDepth` written in lexical order, so the order changes the answer.
     * Every other shape is reported on the spot, unchanged. */
    bool defer = target && destDerefSym(c, target) != NULL;
    return checkEscapeImpl(c, val, at, line, what, target, defer);
}

/**
 * @brief Re-check deferred store decisions once the DFA has settled; report only if still too
 *        shallow.
 *
 * Only now is the destination depth the **shallowest** level the pointee may have
 * (`Sym.minPointee` has collected every re-pointing), instead of the lexical-order value seen
 * during the walk. The min polarity is required: in `q2_dualpointee`, `cur` may point at a
 * parameter (0) or at a frame-local `new` (1), and only min = 0 keeps the dangling case out;
 * taking the DFA's max = 1 would let the hole through.
 *
 * When `dfaSettled` is false (the DFA overflowed and its whole result was dropped), the numbers
 * recorded at check time are reported, which keeps the conservative behaviour.
 *
 * @param c           Checker; scopes must still be open (`lookup` is used).
 * @param dfaSettled  Whether the DFA produced a usable result.
 */
void recheckPendingRefStores(Checker *c, bool dfaSettled) {
    if (c->pendingRefStores.len == 0) return;
    for (size_t i = 0; i < c->pendingRefStores.len; i++) {
        PendingRefStore *p = *(PendingRefStore **)vecAt(&c->pendingRefStores, i);
        if (!p || p->fn != c->curFunc) continue;
        int at = p->at, d = p->depth;
        if (dfaSettled) {
            /* The destination binding was resolved during the walk: block scopes are closed
             * now, so `lookup` on a block-local binding such as `ocur` would miss. The depth
             * is the min over the alias chain (`ocur` follows `cur`). */
            int dd = p->destSym ? symMinPointee(c, p->destSym, 0) : -1;
            at = (dd >= 0) ? dd : storeLayer(c, p->target);
            promoteInto(c, p->val, at);         /* lift again with the destination now settled (idempotent) */
            d = targetDepth(c, p->val);
            if (d <= at) continue;              /* a false rejection caused by word order: undo it */
        }
        ckError(c, p->line, ESCAPE_OUTLIVE_NOTE,
                "%s would hold a reference to a local variable that dies first "
                "(borrowed from depth %d, but this can only hold up to depth %d)",
                p->what, d, at);
    }
    /* This function's records are consumed; the table is per module, so other functions
     * clear their own entries on their own pass. */
    size_t w = 0;
    for (size_t i = 0; i < c->pendingRefStores.len; i++) {
        PendingRefStore *p = *(PendingRefStore **)vecAt(&c->pendingRefStores, i);
        if (p && p->fn == c->curFunc) continue;
        *(PendingRefStore **)vecAt(&c->pendingRefStores, w++) = p;
    }
    c->pendingRefStores.len = w;
}

/* ---- Cycle merging: union the candidate regions of a "possible cycle" -------------
 *
 * See `CycleMerge` in check_internal.h. Three stages: record (during the checking walk),
 * collect sites (after the DFA, while scopes are still open), merge (after the global level
 * solve, before placement).
 *
 * Why the merge criterion is "candidate sites land in **more than one** region": a region
 * marking is exact if and only if the partition cuts no SCC. If a write that may close a cycle
 * has hosts that may point into several regions, the cycle may cross a region boundary; a
 * cross-region seed is only dropped when the slot holding it is released, so the cycle garbage
 * pins the two regions to each other and neither is ever swept. Merging those candidate sites
 * into the **shallowest** level (the longest-lived one) puts that possible cycle into a single
 * region.
 *
 * Direction: sites are only moved towards the **longer-lived** side (`m` is the minimum of the
 * candidates), which can never create a dangling reference; the price is that storage may be
 * kept a little longer. When the candidates include the home region (`anMinAt == 0`) nothing
 * moves: `needsHome` was already settled before this step, and promoting other sites into the
 * home region would make codegen use an `__extc_home` that was never declared.
 */

/**
 * @brief Record an allocation site into a candidate set (deduplicated by pointer, bounded).
 *
 * @param out   Candidate set (`Expr*`).
 * @param site  Allocation site expression.
 */
static void cycleSitePush(Vec *out, Expr *site) {
    if (!site || out->len >= 256) return;
    for (size_t i = 0; i < out->len; i++)
        if (*(Expr **)vecAt(out, i) == site) return;
    *(Expr **)vecPush(out) = site;
}

/**
 * @brief Read-only walk: collect the allocation sites an expression "may point at".
 *
 * Only the three origins of "who this binding **itself** may point at" are followed: the value
 * it currently holds (`origin`), the alias source at the declaration (`initSrc`), and
 * `heldSrc`, recorded by a direct assignment. The field table, `otherSrc` and `callSrc` are
 * **not** followed: those record values written into the binding's **fields**
 * (`s.next = keeper`), which are reachable from the host but are not objects the host itself
 * may point at; counting them would merge a case such as "the loop body's `s` points at the
 * long-lived `keeper`" into one region, and that cross-region edge is exactly the registration
 * path the run switch has to exercise.
 *
 * Stopping here is the safe direction: a smaller candidate set means fewer merges, which leaves
 * the existing placement in place and lets nothing through. **No accounting is written** -- a
 * known traversal shape is only borrowed as an approximation of "may point at".
 *
 * @param c     Checker; scopes must still be open (this runs only while recording, because it
 *              uses `lookup`).
 * @param e     Starting expression.
 * @param out   Candidate set (`Expr*`).
 * @param hops  Hop limit (guards against cycles).
 */
static void cycleCollectSites(Checker *c, Expr *e, Vec *out, int hops) {
    if (!e || hops > 32 || out->len >= 256) return;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        cycleSitePush(out, e);
        return;
    case EX_IDENT: {
        Sym *sy = lookup(c, e->u.ident.name);
        if (!sy) return;
        cycleCollectSites(c, sy->origin,  out, hops + 1);
        cycleCollectSites(c, sy->initSrc, out, hops + 1);
        cycleCollectSites(c, sy->heldSrc, out, hops + 1);
        return;
    }
    case EX_FIELD: cycleCollectSites(c, e->u.field.obj, out, hops + 1); return;
    case EX_INDEX: cycleCollectSites(c, e->u.index.obj, out, hops + 1); return;
    case EX_SLICE: cycleCollectSites(c, e->u.slice.obj, out, hops + 1); return;
    case EX_DEREF: cycleCollectSites(c, e->u.deref.operand, out, hops + 1); return;
    case EX_SIGN:  cycleCollectSites(c, e->u.sign.operand, out, hops + 1); return;
    case EX_TRY:   cycleCollectSites(c, e->u.try_.operand, out, hops + 1); return;
    case EX_CONV:  cycleCollectSites(c, e->u.conv.operand, out, hops + 1); return;
    case EX_COALESCE:
        cycleCollectSites(c, e->u.coalesce.main, out, hops + 1);
        cycleCollectSites(c, e->u.coalesce.fallback, out, hops + 1);
        return;
    /* kind-default: only shapes that **pass a reference through** keep walking; literals, call
     * results and the like stop here. A new kind that carries references must add a case here
     * -- missing one only makes the candidate set smaller (fewer merges), the safe direction. */
    default:
        return;
    }
}

/**
 * @brief Record a heap-reference write that may close a cycle, for the region merge after the
 *        level solve.
 *
 * The allocation sites the host may point at are collected **on the spot** (scopes are open and
 * `lookup` works); the merge itself is left to `applyCycleMerges`. The criterion and the field
 * meanings are in `CycleMerge`.
 *
 * @param c      Checker (scopes still open).
 * @param base   Host binding (the `cur` in `(*cur).f = v`).
 * @param value  Value being written.
 * @param line   Source line.
 */
void noteCycleMerge(Checker *c, Sym *base, Expr *value, int line) {
    if (!c || !base || !value) return;
    for (size_t i = 0; i < c->cycleMerges.len; i++) {
        CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
        if (r && r->base == base && r->value == value && r->fn == c->curFunc)
            return;   /* the driver checks the same site over several passes: record it once */
    }
    CycleMerge *r = arenaAllocZero(c->arena, sizeof *r);
    r->base  = base;
    r->value = value;
    r->line  = line;
    r->fn    = c->curFunc;
    vecInit(&r->sites, c->arena, sizeof(Expr *));
    /* The sites are collected **now**: scopes are still open (`lookup` works). Collecting
     * after the DFA would not do -- block scopes are closed by then and a block-local binding
     * such as `var head = new node` could not be found (`recheckPendingRefStores` notes the
     * same thing). Levels are only fixed after the global solve, so the merge itself stays in
     * the later stage (see `applyCycleMerges`). */
    cycleCollectSites(c, base->origin,  &r->sites, 0);
    cycleCollectSites(c, base->initSrc, &r->sites, 0);
    cycleCollectSites(c, base->heldSrc, &r->sites, 0);
    *(CycleMerge **)vecPush(&c->cycleMerges) = r;
}

/**
 * @brief The region an allocation site will end up in; the same rule placement uses.
 *
 * @param site  Allocation site.
 * @return      `ARENA_HOME` (the home region), or a block level (>= 1).
 */
static int cycleSiteWant(Expr *site) {
    int a = anMinAt(site);
    if (a == 0) return ARENA_HOME;
    if (a >= 1) return a;
    int lx = anLexicalLevel(site);
    return lx >= 1 ? lx : 1;
}

/**
 * @brief After the global level solve: merge the candidate regions of a "possible cycle" into
 *        the shallowest level.
 *
 * Runs before placement and only changes `anMinAt` (placement writes `arenaLevel` / `refDepth`
 * from it). Candidates that include the home region do not move (`needsHome` is already
 * settled); `EXTC_RECLAIM=0` returns immediately (there is no region partition).
 *
 * @param c  Checker.
 */
/**
 * @brief Union-find over the allocation sites of a "possible cycle": sites in one group may not
 *        be cut apart by a reclaim slab.
 *
 * Group numbers are handed to codegen through `planSetSiteCycleGroup`; a component only forms a
 * group when it holds at least two allocation sites.
 */
typedef struct { Expr **site; int *par; size_t n, cap; Arena *a; } SiteUF;

/**
 * @brief Return a site's union-find index, appending it when absent (a new element is its own
 *        tree).
 *
 * @param u  Union-find.
 * @param s  Allocation site expression.
 * @return   Index (0-based).
 */
static int sufAdd(SiteUF *u, Expr *s) {
    for (size_t i = 0; i < u->n; i++) if (u->site[i] == s) return (int)i;
    if (u->n == u->cap) {
        size_t c = u->cap ? u->cap * 2 : 64;
        Expr **ns = (Expr **)arenaAlloc(u->a, c * sizeof *ns);
        int *np = (int *)arenaAlloc(u->a, c * sizeof *np);
        for (size_t i = 0; i < u->n; i++) { ns[i] = u->site[i]; np[i] = u->par[i]; }
        u->site = ns; u->par = np; u->cap = c;
    }
    u->site[u->n] = s; u->par[u->n] = (int)u->n;
    return (int)u->n++;
}

/**
 * @brief Find the root of the tree containing an index (with path compression).
 *
 * @param u  Union-find.
 * @param i  Index.
 * @return   Root index.
 */
static int sufRoot(SiteUF *u, int i) {
    while (u->par[i] != i) { u->par[i] = u->par[u->par[i]]; i = u->par[i]; }
    return i;
}

/**
 * @brief Union two trees by attaching the second root (no rank -- there are very few sites).
 *
 * @param u  Union-find.
 * @param a  Index.
 * @param b  Index.
 */
static void sufUnion(SiteUF *u, int a, int b) {
    a = sufRoot(u, a); b = sufRoot(u, b);
    if (a != b) u->par[b] = a;
}

/**
 * @brief The "possible cycle" group an allocation site belongs to; 0 means it is in no
 *        cross-site cycle.
 *
 * @param c  Checker.
 */
void applyCycleMerges(Checker *c) {
    if (!c || c->cycleMerges.len == 0) return;
    /* The fix only means anything for **regions**: `EXTC_RECLAIM=0` emits no reclaim runtime
     * at all, so there is no region partition and functional-arena levels stay as they are --
     * the switch is the baseline of the whole comparison and the product has to stay
     * byte-identical (`main.c --help` names `EXTC_RECLAIM` as changing the product). */
    const char *rec = getenv("EXTC_RECLAIM");
    if (rec && rec[0] == '0' && rec[1] == '\0') return;
    /* ── Site union-find -> "possible cycle" groups ───────────────────────────────
     * One `CycleMerge` says: the allocation sites the host may point at, plus the site that
     * was written, may form a cycle. Sites in the same connected component **must land in one
     * reclaim slab**: a slab boundary cutting a cycle turns the cross-slab registration slot
     * into a seed of cycle garbage, so the bound grows with the round count (`digraph` with
     * `EXTC_RC_SHARDS=4` lands in the same order of magnitude as naive reference counting).
     * A component with a single site needs no pinning: it sits in one slab anyway -- the 8
     * independent chains of `shard_bands` are exactly this shape (one allocation point per
     * chain), so the shard knob's pause benefit is not pinned down with them. */
    {
        SiteUF u;
        u.a = c->arena; u.n = 0; u.cap = 64;
        u.site = (Expr **)arenaAlloc(c->arena, u.cap * sizeof *u.site);
        u.par = (int *)arenaAlloc(c->arena, u.cap * sizeof *u.par);
        for (size_t i = 0; i < c->cycleMerges.len; i++) {
            CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
            if (!r) continue;
            int first = -1;
            for (size_t k = 0; k < r->sites.len; k++) {
                int id = sufAdd(&u, *(Expr **)vecAt(&r->sites, k));
                if (first < 0) first = id; else sufUnion(&u, first, id);
            }
            if (r->value && (r->value->kind == EX_NEW || r->value->kind == EX_GENCALL)) {
                int id = sufAdd(&u, r->value);
                if (first < 0) first = id; else sufUnion(&u, first, id);
            }
        }
        int *size = (int *)arenaAllocZero(c->arena, (u.n ? u.n : 1) * sizeof *size);
        int *gid = (int *)arenaAllocZero(c->arena, (u.n ? u.n : 1) * sizeof *gid);
        for (size_t i = 0; i < u.n; i++) size[sufRoot(&u, (int)i)]++;
        int next = 0;
        for (size_t i = 0; i < u.n; i++) {
            int rt = sufRoot(&u, (int)i);
            if (size[rt] >= 2 && gid[rt] == 0) gid[rt] = ++next;
        }
        for (size_t i = 0; i < u.n; i++) {
            int rt = sufRoot(&u, (int)i);
            if (gid[rt]) planSetSiteCycleGroup(u.site[i], gid[rt]);
        }
        /* Which functions sit in a "cross-site" cycle group: their units refuse sharding
         * (`scanUnitReclaim` in codegen.c). The criterion only accepts groups whose component
         * holds >= 2 sites -- a single-site self-cycle needs no pinning. */
        if (next > 0) {
            for (size_t i = 0; i < c->cycleMerges.len; i++) {
                CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
                if (!r || !r->fn) continue;
                for (size_t k = 0; k < r->sites.len && r->fn; k++) {
                    Expr *st = *(Expr **)vecAt(&r->sites, k);
                    if (planSiteCycleGroup(st) > 0) planSetFnCrossSiteCycle(r->fn, true);
                }
            }
        }
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[cycle-group] %zu sites -> %d groups\n", u.n, next);
    }
    for (size_t i = 0; i < c->cycleMerges.len; i++) {
        CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
        if (!r || r->sites.len == 0) continue;
        int m = -1;
        bool home = false;
        for (size_t k = 0; k < r->sites.len; k++) {
            int w = cycleSiteWant(*(Expr **)vecAt(&r->sites, k));
            if (w == ARENA_HOME) { home = true; break; }   /* a home decision cannot be changed afterwards */
            if (m < 0 || w < m) m = w;
        }
        if (home || m < 1) continue;
        bool multi = false;
        for (size_t k = 0; k < r->sites.len && !multi; k++)
            if (cycleSiteWant(*(Expr **)vecAt(&r->sites, k)) != m) multi = true;
        if (!multi) continue;                              /* all candidates land in one region: nothing was cut apart */
        for (size_t k = 0; k < r->sites.len; k++) {
            Expr *site = *(Expr **)vecAt(&r->sites, k);
            int w = cycleSiteWant(site);
            if (w == ARENA_HOME || w == m) continue;
            if (anMinAt(site) < 0 || anMinAt(site) > m) anSetMinAt(site, m);
        }
        promoteInto(c, r->value, m);                       /* the value has to land in the same region too */
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[merge] fn=%s line=%d sites=%zu -> level %d\n",
                    r->fn && r->fn->name ? r->fn->name : "?", r->line, r->sites.len, m);
    }
}

/* "a `let` binding" and "the path crosses a read-only reference" are two separate
 * questions: a `var` can hold a read-only reference too, as `fn f(v: ref slice<i32>)` does;
 * a write has to satisfy both. */
/**
 * @brief Tell whether every reference crossed on the way to this place is a `mut ref`.
 *
 * There are only two ways to cross: explicitly through `*p`, or implicitly when `p` in `p.f`
 * / `v[i]` is a reference and the place therefore lands in the storage it points at. The
 * target's own type does not count, so re-pointing a binding stays writable. Mixing the two
 * questions up both rejects `(*cell).next = v` (the field type is a read-only reference) and
 * misses real writes through a reference.
 *
 * @param e        Place expression.
 * @param crossed  Out-parameter: set to true when the storage sits behind a crossed reference.
 * @return         `true` when every crossed reference is a `mut ref`, `false` on a read-only
 *                 one.
 */
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

/**
 * @brief Tell whether the path to this place crosses or lands on a read-only reference.
 *
 * This is not the same question as `pathRefsAllMut`: the place's own type counts here as well,
 * as in `c.bump()` where `c` is `ref counter`. Both answers are needed because the
 * diagnostics differ in wording.
 *
 * @param e  Place expression.
 * @return   `true` when the place's own type, or any reference on the path, is read-only.
 */
bool pathHasReadonlyRef(Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->type && x->type->kind == TY_REF && !x->type->mut) return true;
        /* An explicit crossing such as `(*p).f`. A dereference's type is the pointee, not a
         * reference type, so the reference's mutability has to be tested on the operand;
         * otherwise `fn g(p: ref node) { (*p).val = 7 }` would compile. */
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

/**
 * @brief Check that a place may be written.
 *
 * Three causes, each with its own diagnostic: writing `*p` needs a `mut ref`; a path that
 * crosses a read-only reference or a read-only view needs `mut` on the type; reassigning a
 * `let` binding is forbidden outright.
 *
 * @param c     Checker.
 * @param e     Place being written.
 * @param line  Source line for the diagnostic.
 * @param what  Operation being checked, named in the diagnostic.
 * @return      `true` when the write was rejected and reported, `false` when it is allowed.
 */
bool requireMutable(Checker *c, Expr *e, int line, const char *what) {
    /* `*p` needs its own arm because `placeRoot` does not recognise it and returns NULL,
     * which would be taken as writable. Writability comes from the reference's type: only
     * `mut ref` allows `*p = v`. */
    if (e && e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        if (ot && ot->kind == TY_REF && ot->mut) return false;
        ckError(c, line,
                "`*p = v` needs `p` to be `mut ref T`; a `ref T` is a read-only borrow",
                "cannot %s through a read-only reference", what);
        return true;
    }
    /* The question here is whether a write may cross, so what matters is the mutability of
     * every reference on the way: the place's own type may be a read-only reference (`c` in
     * `c.bump()` is `ref counter`), or the path may cross one explicitly (`(*p).f`).
     * `pathHasReadonlyRef` answers both. */
    if (pathHasReadonlyRef(e)) {
        ckError(c, line,
                "`ref T` is a **read-only** borrow; writing through it needs `mut ref T` "
                "in the declaration. Read-only is the default so that a signature says "
                "what it does.",
                "cannot %s through a read-only reference", what);
        return true;
    }
    /* A view is a pair the compiler builds: `data` and `len` for a slice, shape and stride
     * words for a rank-N view. Letting user code write either half would let a view overstate
     * or outlive the storage it names -- exactly the class of bug the depth rules exist to
     * prevent -- so the halves are read-only even through a `mut slice<T>`. `mut` governs the
     * *elements*, not the coordinates. Callers build the view they want instead. */
    if (e && e->kind == EX_FIELD) {
        Type *bt = e->u.field.obj->type;
        while (bt && bt->kind == TY_REF) bt = bt->inner;
        if (bt && ttIsViewType(bt)) {
            ckError(c, line,
                    "a view's coordinates are built by the compiler (`data` and `len` for a "
                    "slice, shape and strides for a rank-N view); writing one would let the "
                    "view name memory it does not own. Build the view you want instead: "
                    "`v[lo..hi]` shrinks a slice, `v.sub(...)` picks a sub-view.",
                    "cannot assign `%s` on a view of type `%s`; take a different view instead",
                    e->u.field.name, typeStr(c, bt));
            return true;
        }
    }
    Sym *root = placeRoot(c, e);
    /* Whether a view's elements can be written depends on whether the view's type carries
     * `mut`. This closes the hole of passing a view by value: in
     * `fn f(v: slice<i32>) { v[0] = 1 }` the parameter is a copy, but the elements belong to
     * the caller. Writing needs `mut slice<i32>` (or `mut ref slice<i32>`) in the signature.
     */
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

/**
 * @brief Return a view type's element type.
 *
 * The view protocol is the two members `data` and `len`; a type is recognised as a view by
 * the name `slice` and exactly one type argument.
 *
 * @param t  Type to check.
 * @return   The element type; NULL when `t` is not a single-argument `slice` instance.
 */
Type *viewElemOf(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return NULL;
    if (strcmp(t->sdef->name, "slice") != 0) return NULL;
    if (t->targs.len != 1) return NULL;
    return *(Type **)vecAt(&t->targs, 0);
}

/* Signature checks run at the definition site, so an error such as `fn !=` cannot surface
 * later in the C compiler as "invalid use of void expression". */
/**
 * @brief Check a constructor's signature at the definition site.
 *
 * Only the associated function named `new` of its type is checked. The return type must be
 * the owner itself, or `result<T, E>` whose first type argument is the owner (a fallible
 * constructor); otherwise the error is reported at the definition, even if nothing calls it
 * yet. Any other name (say `fn make`) is an ordinary associated function and may return
 * anything.
 *
 * A **fallible** constructor is the one exception: `result<T, E>` is accepted, because
 * opening a file, parsing text and allocating are all things that can fail, and the caller
 * then has to write `?` -- which is how extC says "this can fail" everywhere else. The error
 * type is the constructor's own choice.
 *
 * @param c  Checker.
 * @param f  Function definition being checked.
 */
void checkCtorSig(Checker *c, FuncDef *f) {
    if (!anIsAssoc(f) || !f->owner || strcmp(f->name, "new") != 0) return;
    Type *ret = f->ret;
    if (ret && (ret->kind == TY_STRUCT || ret->kind == TY_GENERIC) && ret->sdef == f->owner) return;
    /* `result<T, E>`: an enum whose first type argument is the owner. */
    if (ret && ret->kind == TY_ENUM && ret->edef && ret->targs.len >= 1) {
        Type *ok = *(Type **)vecAt(&ret->targs, 0);
        ok = ttBase(ok);
        if (ok && (ok->kind == TY_STRUCT || ok->kind == TY_GENERIC) && ok->sdef == f->owner) return;
    }
    Buf want;
    bufInit(&want, c->arena);
    bufPrintf(&want, "signature must be `fn new(...) -> %s` (or `-> result<%s, E>` for a"
                     " constructor that can fail)", DN(f->owner), DN(f->owner));
    ckError(c, f->line, bufCstr(&want),
            "the constructor `%s::new` must return `%s`", DN(f->owner), DN(f->owner));
}

/**
 * @brief Check an operator method's signature at the definition site.
 *
 * Only the comparison and arithmetic operators of the overloadable set (including `<<` and
 * `>>`) are checked; every other name passes. Exactly two parameters are required, and the
 * first must be a reference to the owner; a comparison must return `bool`, an arithmetic
 * operator must return a value of the owner type, and only `<<` and `>>` may return a
 * reference to it.
 *
 * Checking at the definition makes every use path safe: a `fn !=` with a wrong signature is
 * reported even when nothing calls it, and a generic `a != b` is settled before
 * instantiation, so codegen never receives a `!=` returning `void`.
 *
 * @param c  Checker.
 * @param f  Function definition being checked.
 */
void checkOperatorSig(Checker *c, FuncDef *f) {
    bool cmp   = isCmpOp(f->name);
    bool arith = isArithOp(f->name) || strcmp(f->name, "<<") == 0 || strcmp(f->name, ">>") == 0;
    if ((!cmp && !arith) || !f->owner) return;

    const char *ret = cmp ? "bool" : f->owner->name;
    Buf want;
    bufInit(&want, c->arena);
    bufPrintf(&want, "signature must be `fn %s(self: ref T, other: A) -> %s`"
              " (A is any type)", f->name, ret);
    const char *wantS = bufCstr(&want);

    const char *why =
        "`a == b` gets used in `if` / `&&` / `||`, and extC has no implicit truthiness; "
        "`!=` is also derived by negating `==`. "
        "To return something else, use a different method name (`compare` / `diff` etc.) -- those are unrestricted.";
    const char *whyArith =
        "the result of `a + b` stands where `a` stood, so it has to have the same type; "
        "for a mixed operand type (`vec * f64`) write a named method, since extC does not convert implicitly.";

    if (f->params.len != 2) {
        ckError(c, f->line, wantS, "operator `%s` must take exactly 2 parameters", f->name);
        return;
    }
    if (cmp) {
        if (!f->ret || !ttIs(f->ret, "bool")) {
            ckError(c, f->line, why, "operator `%s` must return `bool`", f->name);
            return;
        }
    } else {
        /* The result must be the owner type. An arithmetic operator returns by **value**: the
         * checker fixes `a + b` to the operand type, and the result stands where `a` stood.
         *
         * The stream operators `<<` and `>>` may also return the owner type by **reference**:
         * a stateful stream needs that to chain (`fin >> x >> line`), because the result is
         * then the same object rather than a copy, and the position the first `>>` advanced
         * is where the second one continues reading; a copy would carry its own position and
         * leave the original untouched. Only these two names have the second form. Returning
         * a reference from `a + b` would put a reference where the language says a value
         * stands, and addition does not need it. */
        bool isStream = strcmp(f->name, "<<") == 0 || strcmp(f->name, ">>") == 0;
        Type *rb = f->ret ? ttBase(f->ret) : NULL;
        /* Same holder tolerance as the left operand below: an operator attached to a generic
         * **instance** (`impl slice<u8>`) returns that instance's type, whose `sdef` is the
         * generic declaration and not the holder. */
        bool retOk = rb && !ttIsError(rb) && rb->sdef == f->owner;
        if (!retOk && rb && planBuiltinHolder(f->owner) && f->owner->type)
            retOk = ttEquals(ttViewReadonly(c->tt, rb), ttViewReadonly(c->tt, f->owner->type));
        if (!retOk || (!isStream && f->ret->kind == TY_REF)) {
            ckError(c, f->line,
                    isStream ? "a stream operator may return the stream by value or by"
                               " reference (`-> T` or `-> mut ref T`)"
                             : whyArith,
                    "operator `%s` must return `%s`", f->name, f->owner->name);
            return;
        }
    }
    Param *p0 = *(Param **)vecAt(&f->params, 0);

    if (p0->type->kind != TY_REF) {
        ckError(c, p0->line, wantS, "`self` of operator `%s` must be a reference", f->name);
        return;
    }
    Type *b0 = ttBase(p0->type);
    /* An operator attached with `impl slice<u8> { fn == ... }` lives in the instance's **holder**
     * declaration, which is not the body of `slice`; the receiver must therefore be compared with
     * the holder's target type instead of with `sdef`. `mut` is a permission and not identity
     * (see `ttViewReadonly`), so a writable view may use the read-only operator. */
    bool recvOk = b0 && !ttIsError(b0) && b0->sdef == f->owner;
    if (!recvOk && b0 && planBuiltinHolder(f->owner) && f->owner->type) {
        Type *want0 = ttViewReadonly(c->tt, f->owner->type);
        Type *got0 = ttViewReadonly(c->tt, b0);
        recvOk = ttEquals(got0, want0);
    }
    if (!recvOk) {
        ckError(c, f->line, wantS,
                "the left operand of operator `%s` must be `%s`", f->name, f->owner->name);
        return;
    }
    /* The right operand may be any type: that is what makes an operator definable more than
     * once on one type (`out << i64` next to `out << f64`), and it is matched on the
     * operand's exact type at every use.
     */
}

/**
 * @brief Check an expression in value position, where a reference must be written as `*p`.
 *
 * Division of labour with `checkExpr`: this entry wants a value, so `p` means the object it
 * points at and the node is marked as needing a dereference; `checkExpr` wants a place or a
 * reference itself (assignment targets, the operand of `ref x`, the base of a field / index /
 * slice, a method receiver -- all of which take an address). Permission is not decided here:
 * whether a write may happen is decided by `ref` and `mut ref`.
 *
 * Expressions that ask for a reference themselves, `ref x` and `alloc`, keep the reference
 * type; member selection navigates rather than reading a value, so it still looks through the
 * reference. A `@noCopy` value is rejected here when it is copied.
 *
 * @param c  Checker.
 * @param e  Expression in value position.
 * @return   The value's type; when a reference appears in value position, the pointee type
 *           (the error has been reported by then).
 */
Type *checkValue(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    /* There is no implicit dereference: handing back a reference where a value is wanted is
     * an error, and the diagnostic asks for `*p`. The exceptions are expressions that want a
     * reference in the first place, `ref x` and `alloc<T>(n)` -- dereferencing those would
     * cancel the reference just asked for -- and member selection (`p.field` / `p.method()`),
     * which navigates rather than reading a value. */
    rejectNoCopy(c, e, t);
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

/**
 * @brief Check an expression against an expected type.
 *
 * When the expected type is an option or a result, a bare constructor is completed first;
 * when a value type is expected, copies of `@noCopy` values and borrowed stream-operator
 * results are rejected. A reference in a non-reference expectation is an error.
 *
 * @param c     Checker.
 * @param want  Type the context expects; NULL means any value will do.
 * @param e     Expression to check.
 * @return      The expression's type; when a reference appears in value position, the pointee
 *              type (the error has been reported by then).
 */
Type *checkInto(Checker *c, Type *want, Expr *e) {
    /* An expected type of `option<...>` / `result<...>` also accepts a bare constructor, so
     * `f(some(3))` and `var x: ?i32 = some(3)` both work: the type comes from the context and
     * the full name does not have to be written. */
    if (want) desugarBareCtor(c, e, want);
    /* A call may need the context to infer its type arguments (`A` in `dup(3, u8(4))`
     * appears only in the return type). The expected type is installed here and the call takes
     * it before checking its arguments, so the sub-expressions' own checks do not see it. */
    Type *savedExpect = c->expectRet;
    c->expectRet = want;
    Type *got = checkExpr(c, e);
    c->expectRet = savedExpect;
    rejectStreamBorrow(c, e);
    /* Reject a copy only where a **value** is wanted: `f(ref s)` and `-> mut ref T` hand over
     * the object itself, which is exactly what `@noCopy` is for. */
    if (!want || want->kind != TY_REF) rejectNoCopy(c, e, got);
    /* As in `checkValue`: a reference where none is wanted is an error. */
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

/**
 * @brief Check one argument of `print` or `println`; a reference is dereferenced here.
 *
 * Printing a reference can only mean printing the value it points at: exposing the address
 * itself is never what the program means. This is the only remaining value position that keeps
 * automatic dereferencing.
 *
 * @param c  Checker.
 * @param e  Argument expression.
 * @return   The printed value's type; for a reference argument, its pointee type, with the
 *           node marked as needing a dereference.
 */
Type *checkPrintArg(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    if (t && t->kind == TY_REF) {
        planSetDeref(e, true);
        return t->inner;
    }
    return t;
}

/**
 * @brief Warn when a `@sharesStorage` binding is copied by value.
 *
 * Such a copy gives one storage a second name: a write through one is visible through the
 * other, and `release` on one leaves the other dangling. Only shapes that read an existing
 * value are reported (`var b = a` / `s.f` / `v[i]`); a freshly constructed value is not, and
 * neither are copies inside the library's own code -- the criterion is where the copy happens,
 * not where the type came from.
 *
 * @param c  Checker, for reporting.
 * @param e  Expression in value position.
 * @param t  Its type.
 */
void warnSharedCopy(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    /* The library's own code is not reported -- the criterion is **where the copy happens**
     * (`c->ctx->path` is the file currently being checked), not where the type came from: a
     * container type naturally comes from the library, but the `var b = a` that goes wrong is
     * in the user's code. Without this, every compilation shows a run of warnings pointing
     * into stdlib and the real one is buried. */
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    /* A freshly constructed value is not a copy: `vector<i32>::new()`, `f()`, `x.m()` and a
     * struct literal all produce a brand-new container that shares no storage with an
     * existing one. Only "read something that already exists" (`var b = a` / `s.f` / `v[i]`)
     * is the copy worth pointing out. */
    if (e->kind != EX_IDENT && e->kind != EX_FIELD && e->kind != EX_INDEX) return;
    ckWarn(c, e->line,
           "Use `a.clone()` for an independent copy (explicitly O(n): one copy plus one new"
           " plate), or `ref a` / `mut ref a` to share it on purpose. Until then the two names"
           " are one storage: writing through one is visible through the other, and"
           " `release` on one leaves the other dangling.",
           "`%s` copies by value but **shares storage**: `var b = a` gives two names to one"
           " plate, not two containers", DN(sd));
}

/**
 * @brief Warn when an existing `@sharesStorage` value is handed over by value.
 *
 * Handing over an existing value makes the caller and this frame share one storage; a value
 * built inside this function body is a move and is not reported. The criterion is where the
 * binding was declared relative to the function's signature line: `sym->line <= curFunc->line`
 * means a parameter (outside this body), anything later was bound inside it; expressions that
 * read something existing (`s.f` / `v[i]`) always count as existing. Returns inside the
 * library's own code are not reported.
 *
 * The hard part is the line between "already existed" and "just built", which differs by one
 * word on the right-hand side:
 *   return v                    // v is a parameter or an outer variable => a **copy** is
 *                               // handed over, and both names share one storage
 *   var v = vector<i32>::new()  // a new value => handing it over is a move; there is no
 *                               // second name, so nothing is reported
 * Division of labour with the binding site: that one covers `var b = a` (a new name for one
 * plate), this one covers `return b` (the struct is copied as the value is handed over).
 * Together they cover every shape where what looks like an assignment is really sharing.
 *
 * @param c  Checker, for reporting.
 * @param e  Returned expression.
 * @param t  Its type.
 */
void warnSharedReturn(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    if (e->kind == EX_IDENT && c->curFunc) {
        Sym *sym = lookup(c, e->u.ident.name);
        /* Bound inside the function body => a new value; handing it over leaves no second
         * name. */
        if (sym && sym->line > c->curFunc->line) return;
    } else if (e->kind != EX_FIELD && e->kind != EX_INDEX) {
        /*
         * A call result, a literal, `alloc` and the like: none of them is "an existing one".
         */
        return;
    }
    ckWarn(c, e->line,
           "Return it as `ref T` / `mut ref T` (a borrow, no copy), or `return v.clone()` when"
           " the caller really needs its own container. Returning by value copies the value:"
           " the caller and this frame then name one storage.",
           "returning `%s` by value copies a `@sharesStorage` type: the caller gets the same"
           " plate, not its own", DN(sd));
}

/**
 * @brief Record that a `@noCopy` binding is moved into a new name.
 *
 * The annotated form goes through `rejectNoCopy`, which sees `Checker.moveOkHere`; the unannotated
 * form takes `checkExpr`, which never looks at copies, so the transfer is recorded here. Only a
 * plain name moves: a field or an element keeps being rejected, because taking it out would leave
 * the aggregate half-owned.
 *
 * @param c     Checker.
 * @param e     Initializer of the new binding.
 * @param dest  Name of the destination binding, for the diagnostic.
 * @return      `true` when a transfer was recorded.
 */
bool ckMoveNoCopyInto(Checker *c, Expr *e, const char *dest) {
    if (!e || e->kind != EX_IDENT) return false;
    Sym *src = lookup(c, e->u.ident.name);
    if (!src || src->movedLine) return false;
    StructDef *sd = structOf(ttBase(src->type ? src->type : e->type));
    if (!sd || !sd->noCopy) return false;
    src->movedLine = e->line;
    src->movedInto = dest;
    return true;
}

/**
 * @brief Report that an `@noCopy` value would be copied by value.
 *
 * One wording for both paths that can reach it: a copy of a place (a binding, a field or an
 * element) and a read of an owned value out of storage the reader does not own.
 *
 * @param c  Checker, for reporting.
 * @param e  Expression that would be copied.
 * @param sd Its structure definition, which carries the annotation.
 */
static void reportNoCopyCopy(Checker *c, Expr *e, StructDef *sd) {
    ckError(c, e->line,
            "Pass it as `ref` / `mut ref` instead: `f(ref s)`, or declare the parameter"
            " `mut ref T`. The state it carries has an identity, so a copy would give two"
            " names to one position.",
            "`%s` is `@noCopy`: it may not be copied by value", DN(sd));
}
/**
 * @brief Reject a copy of a `@noCopy` value.
 *
 * Only a **place** is rejected: `var s: ifstream = f()` binds the value the call just
 * produced, not a copy of anything, whereas `var s2 = s` gives one reader a second name and
 * the position advances behind the other name -- exactly the failure this annotation exists
 * to prevent. `ref s` is a reference, not a copy, and stays legal.
 *
 * @param c  Checker.
 * @param e  Expression in value position.
 * @param t  Its type.
 * @return   `true` when an error was reported.
 */
static bool rejectNoCopy(Checker *c, Expr *e, Type *t) {
    if (!e || e->kind == EX_REF || !t) return false;
    StructDef *sd = structOf(ttBase(t));
    if (!sd || !sd->noCopy || !isPlace(e)) return false;
    /* A `var`/`let` binding of a plain name **moves** the value instead of copying it: the
     * language transfers `@noCopy` values (they carry an identity, so two names for one state is
     * what the annotation forbids -- transferring gives the state a single name, which is fine).
     * Fields, array elements and call arguments stay copied, so those keep the diagnostic. */
    if (c->moveOkHere && e->kind == EX_IDENT) {
        Sym *src = lookup(c, e->u.ident.name);
        if (src && src->movedLine == 0) {
            src->movedLine = e->line;
            src->movedInto = c->moveIntoName;
            /* Handing a local to the caller moves it only where that `return` runs, so the mark is
             * scoped to the block that holds the `return`; see `checkBlockBody`. */
            if (c->moveIsReturn) {
                if (!c->moveLog.arena) vecInit(&c->moveLog, c->arena, sizeof(MoveMark));
                MoveMark *mk = (MoveMark *)vecPush(&c->moveLog);
                mk->sym = src;
                mk->line = e->line;
            }
            return false;
        }
    }
    reportNoCopyCopy(c, e, sd);
    return true;
}

/**
 * @brief Exported entry to the copy rejection, for the unannotated binding path.
 *
 * That path checks its initializer with `checkExpr`, which is about values and never about
 * copies, so it asks here once the cheap transfer case has been ruled out. Keeping the
 * diagnostic in one place is the point: `var d = s[0]` and `var d: T = s[0]` must read the same.
 *
 * @param c  Checker, for reporting.
 * @param e  Expression in value position.
 * @param t  Its type.
 * @return   `true` when an error was reported.
 */
bool ckRejectNoCopyCopy(Checker *c, Expr *e, Type *t) { return rejectNoCopy(c, e, t); }

/**
 * @brief Reject a read of an owned value out of storage the reader does not own.
 *
 * `outer.view()[0]`, `box.c` and `*p` hand back a value whose bytes belong to an
 * aggregate that still frees them, so two names end up owning one storage.
 *
 * @param c  Checker, for reporting.
 * @param e  Expression in value position.
 * @param t  Its type.
 * @return   `true` when an error was reported.
 */
bool ckRejectOwningRead(Checker *c, Expr *e, Type *t) {
    if (!e || !t) return false;
    StructDef *sd = structOf(ttBase(t));
    if (!sd || !sd->noCopy) return false;
    reportNoCopyCopy(c, e, sd);
    return true;
}
/**
 * @brief Defer an owning-read check until the template it sits in is instantiated.
 *
 * Inside a template the destination type is still a parameter, so `structOf` has nothing to
 * test. The read is recorded instead and re-examined per instance by `runRefCheck`, which also
 * skips instances that never call the method.
 *
 * @param c     Checker.
 * @param dst   Declared destination type (still parameterised).
 * @param a     The expression read out of storage.
 * @param what  Short name for the destination, used in the diagnostic.
 */
void ckDeferOwningRead(Checker *c, Type *dst, Expr *a, const char *what) {
    if (!c || !dst || !a || !c->curFunc) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->val      = a;
    rc->declType = dst;
    rc->line     = a->line;
    rc->what     = what;
    rc->func     = c->curFunc;
    rc->ctx      = c->ctx;
    rc->ownRead  = true;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/**
 * @brief Defer the decision that a by-value argument has to be cloned until instantiation.
 *
 * A by-value parameter of an owning `@value` type is the callee's copy and the callee drops it, so
 * a caller that passes a place must pass a clone. Inside a template the parameter type is still a
 * type parameter and `structOf` has nothing to test, so the rule is recorded here and replayed per
 * instance by `runRefCheck`; the replay also decides whether the argument is a place, which a
 * temporary does not need. The code generator makes the matching decision per instance with the
 * substituted type (`argCopyNeedsClone`), and the two must agree.
 *
 * @param c    Checker; the record is appended to `c->refChecks`.
 * @param dst  Parameter type, still parameterised.
 * @param a    The argument expression.
 */
void ckDeferArgClone(Checker *c, Type *dst, Expr *a) {
    if (!c || !dst || !a || !c->curFunc) return;
    if (!funcTParams(c->curFunc) || funcTParams(c->curFunc)->len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->val      = a;
    rc->declType = dst;
    rc->line     = a->line;
    rc->func     = c->curFunc;
    rc->ctx      = c->ctx;
    rc->argClone = true;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}


/**
 * @brief Tell whether an expression is a stream-operator result that returns a reference.
 *
 * `fin >> x` hands back the same stream object, not a new value; a shift's result type is an
 * integer, so it never matches. It is not a value, and unlike `pickFirst(ref a, ref b)` --
 * where the user wrote `ref` and asked for a reference -- nothing was asked for here: the
 * borrow is the operator's own doing.
 *
 * @param e  Expression to check.
 * @return   `true` when this is a `<<` / `>>` binary operation whose type is a reference.
 */
bool isStreamBorrow(Expr *e) {
    if (!e || e->kind != EX_BIN || !e->u.bin.op) return false;
    const char *op = e->u.bin.op;
    if (op[0] != '<' && op[0] != '>') return false;
    return e->type && e->type->kind == TY_REF;
}

/**
 * @brief Reject keeping the borrowed result of a stream operator.
 *
 * No move took place: the borrow is the operator's own doing, and keeping it would give one
 * reader a second name while neither name sees the position.
 *
 * @param c  Checker.
 * @param e  Initializer, argument or return expression.
 * @return   `true` when it is a stream borrow and an error was reported.
 */
bool rejectStreamBorrow(Checker *c, Expr *e) {
    if (!isStreamBorrow(e)) return false;
    ckError(c, e->line,
            "The stream is not copied: `fin >> x` hands back `fin` itself, so there is"
            " nothing new to keep -- carry on with `fin`.",
            "a stream operator's result is a borrow of the stream, not a value -- it can"
            " only be chained (`fin >> x >> y`)");
    return true;
}

/**
 * @brief Check an expression that may be `e?`.
 *
 * `?` is legal in a few statement positions only; those positions come through this entry,
 * and every other expression is checked in value position.
 *
 * @param c  Checker.
 * @param e  Expression, possibly an EX_TRY node.
 * @return   The payload type when `?` unwrapped one, otherwise the value-position type.
 */
Type *checkMaybeTry(Checker *c, Expr *e) {
    if (e && e->kind == EX_TRY) return checkTryInner(c, e);
    return checkValue(c, e);
}

/**
 * @brief Check `e?` and forward the failure to the enclosing function.
 *
 * The operand must be an `option` or a `result`, and the enclosing function must return the
 * same kind -- for a `result`, with the identical error type. Inside `main`, and only when the
 * operand really carries a payload, the payload is unwrapped directly and a failure traps.
 *
 * @param c  Checker.
 * @param e  The `?` expression.
 * @return   The unwrapped payload type; the error type when the operand or the enclosing
 *           return type does not fit.
 */
Type *checkTryInner(Checker *c, Expr *e) {
    Type *ot = checkExpr(c, e->u.try_.operand);
    if (ttIsError(ot)) return ttError(c->tt);
    Type *ob = ttBase(ot);

    /* A failure inside `main` **traps**: there is nothing to hand a failure to -- `main`
     * returns `i32` because C's entry point does -- and a program whose main step failed is
     * over anyway, so the honest thing is to say what went wrong and stop. The rule is
     * deliberately narrow: only `main`. Other functions still have to declare `result` to use
     * `?`, because there a failure has somewhere to go. */
    if (c->curFunc && !c->curFunc->owner && c->curFunc->name &&
        strcmp(c->curFunc->name, "main") == 0 &&
        /* ... and only when the operand really carries a payload: `5?` in `main` has none,
         * and without this guard `vecAt(&ob->targs, 0)` would read an empty vector and take
         * the compiler into a SIGSEGV. The guard lets the ordinary path below report the
         * honest "`?` needs an `option<...>` or `result<...>`" instead.
         */
        ob && ob->targs.len > 0) {
        Type *payloadM = *(Type **)vecAt(&ob->targs, 0);
        e->type = payloadM;
        return payloadM;
    }

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

/**
 * @brief Depth computed from lexical block depth and the origin chain only, for the
 *        `EXTC_DBG_RHO` cross-check.
 *
 * Two of `targetDepth`'s inputs are deliberately ignored: the arena level of an allocation
 * site (the solver may still change it, so the block depth the site was born in is used
 * instead -- that number is stable and the solver only ever gives a site a longer life) and
 * the depth cached on the node (`targetDepth` writes that one itself, and it can keep a
 * stale value after a binding has been re-pointed).
 *
 * When the two disagree and `pure` is larger, the checker believes the value lives for less
 * time than it really does, so a store can be skipped and its allocation site never promoted.
 *
 * @param c     Checker (scope stack and type table).
 * @param e     Expression to measure.
 * @param hops  Recursion guard for cyclic origin chains (`a = b  b = a` is legal).
 * @param seen  Nodes already visited on this chain.
 * @return      Upper bound on the depth of the references the value may carry.
 */
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen) {
    if (!e) return 0;                 /* nothing to ask about: 0 is the honest answer */
    /* The budget and the cycle guard are fallbacks, and both answer `DEPTH_UNKNOWN` rather
     * than `0`: `0` means "lives the longest" and would point the conservative direction
     * backwards, whereas `DEPTH_UNKNOWN` means "cannot prove this lives long enough", which
     * the comparison side treats as a rejection. */
    if (hops >= 40) { EXTC_DBG_FALLBACK("targetDepthPure: recursion budget (40) exhausted"); return DEPTH_UNKNOWN; }
    for (int i = 0; i < hops; i++)
        if (seen[i] == e) { EXTC_DBG_FALLBACK("targetDepthPure: origin cycle"); return DEPTH_UNKNOWN; }
    seen[hops] = e;

    switch (e->kind) {
    case EX_DYN:
        /* payload is copied into the pool, and the pool lives in the place the site names */
        return dynSiteDepth(e, targetDepthPure(c, e->u.dynv.payload, hops + 1, seen));
    case EX_NEW:
    case EX_GENCALL:
        /* The block the site was born in. `lexicalLevel` is fixed when the node is first
         * checked and does not move afterwards. */
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_IDENT: {
        /* Follow the origin to the site it came from: a binding carries no depth of its
         * own, independent of that site. */
        Sym *sy = identBindOf(e);
        /* `!sy` means "this ident was never resolved", which should not happen inside the
         * checker (reaching here means another bug), while "no origin" is **legal**: a
         * parameter's origin is empty and its depth is 0 by construction. */
        if (!sy) {
            /* Optimistic here would claim the value lives as long as a parameter, which is the
             * direction that hides a dangling reference; every other unprovable shape answers
             * `DEPTH_UNKNOWN`, and this one must too. */
            EXTC_DBG_FALLBACK("targetDepthPure: unresolved ident");
            return DEPTH_UNKNOWN;
        }
        if (!sy->origin) return 0;
        return targetDepthPure(c, sy->origin, hops + 1, seen);
    }
    case EX_FIELD: {
        /* Take the depth recorded for the field; fall back to the object when the field
         * has no entry. */
        Sym *root = placeRoot(c, e);
        if (root) {
            for (int i = 0; i < root->nfields; i++)
                if (root->fields[i].name &&
                    strcmp(root->fields[i].name, e->u.field.name) == 0)
                    return root->fields[i].depth;
        }
        return targetDepthPure(c, e->u.field.obj, hops + 1, seen);
    }
    case EX_INDEX:
        return targetDepthPure(c, e->u.index.obj, hops + 1, seen);
    case EX_DEREF:
    case EX_SIGN:
        /* `*p` and `p!` denote the same storage as their operand. */
        return targetDepthPure(c, e->kind == EX_DEREF ? e->u.deref.operand
                                                       : e->u.sign.operand, hops + 1, seen);
    case EX_REF:
        /* A reference created here points into the current block. */
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_SLICE:
        /* A view carved out of a place lives as long as that place's block. */
        return targetDepthPure(c, e->u.slice.obj, hops + 1, seen);
    case EX_COALESCE: {
        int a = targetDepthPure(c, e->u.coalesce.main, hops + 1, seen);
        int b = targetDepthPure(c, e->u.coalesce.fallback, hops + 1, seen);
        return maxInt(a, b);
    }
    case EX_STRUCTLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            d = maxInt(d, targetDepthPure(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value,
                                           hops + 1, seen));
        return d;
    }
    /*
     * A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there.
     */
    case EX_LAMBDA: {
        int d = 0;
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            d = maxInt(d, targetDepthPure(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value,
                                           hops + 1, seen));
        return d;
    }
    case EX_ARRAYLIT: {
        int d = 0;
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            d = maxInt(d, targetDepthPure(c, *(Expr **)vecAt(&e->u.arraylit.elems, i),
                                           hops + 1, seen));
        return d;
    }
    case EX_ENUMVAL: {
        int d = 0;
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, targetDepthPure(c, *(Expr **)vecAt(&e->u.enumval.args, i),
                                           hops + 1, seen));
        return d;
    }
    case EX_CALL:
    case EX_ASSOC: {
        /* A call can only return a reference that came from an argument or from static
         * storage, so the deepest argument bounds the result. */
        Vec *args = e->kind == EX_CALL ? &e->u.call.args : &e->u.assoc.args;
        int d = 0;
        for (size_t i = 0; i < args->len; i++)
            d = maxInt(d, targetDepthPure(c, *(Expr **)vecAt(args, i), hops + 1, seen));
        return d;
    }
    case EX_METHOD: {
        int d = targetDepthPure(c, e->u.method.recv, hops + 1, seen);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, targetDepthPure(c, *(Expr **)vecAt(&e->u.method.args, i),
                                           hops + 1, seen));
        return d;
    }
    /* `?` and a conversion pass the operand's references through; an operator can only
     * carry what its operands carry. This arm feeds the cross-check oracle and the
     * data-flow facts, so all of these shapes have to answer.
     */
    case EX_TRY:   return targetDepthPure(c, e->u.try_.operand, hops + 1, seen);
    case EX_CONV:  return targetDepthPure(c, e->u.conv.operand, hops + 1, seen);
    case EX_UN:    return targetDepthPure(c, e->u.un.operand, hops + 1, seen);
    case EX_EXT: return targetDepthPure(c, e->u.ext_.call, hops + 1, seen);   /* `ext f(x)`: a spawned call */
    case EX_BIN: {
        int a = targetDepthPure(c, e->u.bin.left, hops + 1, seen);
        int b = targetDepthPure(c, e->u.bin.right, hops + 1, seen);
        return a > b ? a : b;
    }
    default:
        /* Scalars, string literals, `null`, `true`/`false`: nothing to point at. */
        return 0;
    }
}
