/* Escape checking: the depth model, borrows, and the home arena.
 *
 * This is the pass that decides whether a borrow may outlive the storage it points
 * at, and which arena an allocation has to live in. It is driven by a single rule
 * (see the block below) and by the per-expression depth recorded for every value.
 */

#include "dbg.h"
#include "check_internal.h"
#include "plan.h"          /* 计划/分析产物：写入经 setter，读取经访问器（X2）*/
#include <stdlib.h>
#include <stdio.h>

/* Defined further down, but the value-position rule that calls it comes first. */
static bool rejectNoCopy(Checker *c, Expr *e, Type *t);
void warnSharedCopy(Checker *c, Expr *e, Type *t);

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
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen);


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
/* Is the storage this expression denotes provably **not** in any frame?
 *
 * Two sources, both authored rather than guessed:
 *   - a chain rooted at a **global** (static storage outlives every frame), and
 *   - a call whose declaration carries `effects Ret=0` -- "the reference/view I return does not
 *     point into the caller's frames". The plate signs that once (`std::heap`), and `mmap`-style
 *     handles are the same shape.
 *
 * Everything else answers false, which keeps the conservative rule: a `new` block, an arena
 * allocation, a stack array, or a callee that did not claim it may all die with the frame.
 *
 * Params:
 *   c - checker
 *   e - the initializer (or right-hand side) of a binding
 *
 * Returns:
 *   True when the storage is out of this frame; the caller records it on the binding
 *   (`Sym.outOfFrame`) so that a view may then answer with its pointee's depth. */
bool exprOutOfFrame(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_SIGN:  return exprOutOfFrame(c, e->u.sign.operand);      /* `p!` 只去掉可空 */
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
        /* A return value the declaration vouched for. A slot on a table field carries the same
         * clause, so a table entry is covered by the same test. */
        if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_FIELD) {
            FieldDef *fd = planField(e->u.call.callee);
            if (fd && fd->hasEffects && fd->effRetFresh) return true;
        }
        if (e->kind == EX_METHOD && e->u.method.recv && e->u.method.recv->kind == EX_FIELD) {
            FieldDef *fd = planField(e->u.method.recv);
            if (fd && fd->hasEffects && fd->effRetFresh) return true;
        }
        return planCallee(e) && planCallee(e)->extRetFresh;
    }
    default: return false;
    }
}

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
            /* A **view** whose storage is out of this frame denotes that storage, not the slot it
             * sits in: `var v = pl.view(off, n)!` is a two-word handle whose bytes point into the
             * plate, so `v.data` must not be read as "a pointer into this frame". `outOfFrame` is
             * set where the binding is initialized, from a global or from a call that signed
             * `effects Ret=0` (the plate signed it once); a view over frame storage, a `new` block
             * or an arena allocation keeps the slot depth, which is what the conservative rule
             * needs. */
            if (st && ttIsViewType(st) && sy->outOfFrame) return anRefDepth(sy);
        }
        return sy ? sy->depth : 0;
    }
    /* A field or element lives inside the object that contains it. */
    if (e->kind == EX_FIELD) return slotDepth(c, e->u.field.obj);
    if (e->kind == EX_INDEX) return slotDepth(c, e->u.index.obj);
    Sym *root = placeRoot(c, e);
    /* A shape `placeRoot` cannot name is not "storage that lives forever" -- that reading pointed
     * the conservative direction exactly backwards. `placeRoot` knows `EX_IDENT` / `EX_FIELD` /
     * `EX_INDEX` / `EX_SLICE`, so anything built on the fly fell through to `0`, and a store of such
     * a value was accepted although the storage it names dies with this frame:
     *
     *     G = v              // rejected: a local of this frame
     *     G = [v, v][0]      // accepted, and read a dead array (audit P0-6c)
     *
     * The right question for an unnamed shape is the one the rest of the checker asks about values:
     * how deep is what it *points at* (`targetDepth`). For `[v, v][0]` that is the depth of `v`;
     * for a call result it is the level the callee allocated in. Note this cannot recurse back
     * here: `targetDepth` only consults `slotDepth` for expressions that *are* places, and this
     * branch is the one where `placeRoot` already answered NULL (INV-U: the fallback records the
     * longer-lived requirement, never the shorter one). */
    return root ? root->depth : targetDepth(c, e);
}

/* Arena level at which the storage of a place lives.
 *
 * This answers a different question from `slotDepth`, and the distinction matters:
 *
 *   - how deep is the object a reference *points at*?  -> `slotDepth`
 *   - how deep is the storage *itself*?                -> this function
 *
 * `head = n` stores a pointer into the slot `head`. The slot lives in this frame,
 * so the value that `n` points at must live at least as long as that slot. Asking
 * `slotDepth(head)` on a reference-typed binding would instead report where the
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
 *    a reference or parameter falls back to `slotDepth`, which reports the caller's
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
    return slotDepth(c, e);
}

/* Depth of the references inside a value, computed from syntax rather than types.
 *
 * `targetDepth` returns early when `typeContainsRef` says the type carries no
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
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return anRefDepth(sy);
        /* 注意轴不同：这里是"**结构里有没有引用**"（`valDepthStructural`），不是"活多久"。
         * 非引用绑定结构上没有引用 ⇒ 0 是**语义**，不是兜底。*/
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
     * object it indexes, and an operator can only carry what its operands carry. Leaving these
     * to `default: return 0` **under-reported** the depth, which is the unsafe direction: the
     * depth decides how long the storage has to live. */
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
     * value too -- take the payload's depth. */
    case EX_DYN: return valDepthStructural(c, e->u.dynv.payload);
    default:
        /* 认不出的形状答"结构里没有引用"是**不安全的那一侧**（它可能真的带着引用）⇒
         * 这是兜底，不是语义。实测它会不会被走到：见 .audit/U-LOG.md 的 U5 一节。*/
        /* **实测（U5）：整个套件零命中** ⇒ 这条兜底按设计走不到，因此它可以响亮：
         * 认不出的形状答"结构里没有引用"是**不安全的那一侧**，被走到就是分类漏了。
         * （对照：`solvedDepth` 的同类兜底**是活路径**，所以那边只能是 NOTE —— 见 V-LOG。）*/
        /* **实测（V2）**：这条兜底**是活路径**（我先前"整套零命中"的读数不准，已纠正）——
         * `tests/asan/promote-field-lit.extc`（正例）就会走到它，而打出来的 kind = `0` = **`EX_INT`**：
         * 整数字面量，**不带引用 ⇒ 答 0 是语义，不是兜底**（与 `solvedDepth` 的 INT/CONV 同结论）。
         *
         * 为什么留 NOTE 而不是断言：将来若有**别的** kind 走到这里，计数会变 —— 那才是要查的信号
         * （能不能带引用、要不要改成"取自操作数/载荷"）。逐个列 kind 会撞 walker 预算
         * （`tools/check_walkers.py` 限 23 个手写遍历），而"列全"在这里买不到正确性。*/
        EXTC_DBG_NOTEF("valDepthStructural: kind=%d answered 0", (int)e->kind);
        return 0;
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
 *     *inside* the copy matter, and those are covered by `targetDepth`.
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
    int a = targetDepth(c, e);
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
/* 「建池的调用」这个值活到多深（池的提权，PLAN #87）。
 *
 * 与 `new` 同一条规则：新分配的东西活到它的站点那一层。容器构造就是一个站点 ——
 * 它建出来的池生在哪一层地方，容器就活到哪一层。
 *   - `ZONE_HOME` ⇒ 0（"活得比本帧还久"）：库函数里的构造交给调用者选的地方；
 *   - `k >= 1`    ⇒ k：生在第 k 层，就活到第 k 层。
 * 不是建池的调用（`makesPool` 为假）⇒ 原样返回：调用结果的深度仍是"实参里最深的那个"。
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

int targetDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* The only reason to answer without looking: this type cannot carry a reference.
     * A type that mentions a type parameter must not take this exit: inside a generic body the
     * depth is computed on the assumption that the parameter may carry one, and the instance
     * check decides whether the rule applies.
     *
     * `EX_DYN` must not take it either, even though `dyn T` carries no reference of its own: the
     * payload is **copied into the pool**, so whatever the payload refers to is referred to by
     * this value too. Missing that is how a payload's view of a stack local slipped past the
     * scope rule (family E, `tests/errors/dyn_payload_view_escapes_block.extc`). */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return 0;
    /* A binding's depth is authoritative on the binding, not on the expression node: the
     * node's copy is written when the expression is first checked, and the data-flow
     * analysis settles binding depths only afterwards, from the whole control-flow
     * structure. Consulting the node here would return the pre-fixed-point answer, which
     * is exactly how a struct holding an allocation was reported as holding nothing live
     * and the allocation stayed in a block arena. */
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
        /* The payload is copied into the pool: its references are this value's references. */
        d = targetDepth(c, e->u.dynv.payload);
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
     * object it indexes, and an operator can only carry what its operands carry. Leaving these
     * to `default: return 0` **under-reported** the depth, which is the unsafe direction: the
     * depth decides how long the storage has to live. */
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
        /* A binding that holds an aggregate carrying references (a struct, array, or
         * generic instance holding a reference or a view) must be asked where those
         * references point, not how deep its own slot is:
         *
         *     var h: holder = { n: ref *p }   // p is a parameter, so depth 0
         *     return h                        // answering with the slot depth 1 used
         *                                     // to reject this valid program
         *
         * The slot depth is still the right answer for storing into the binding, which
         * is what `slotDepth` computes. The two questions are kept separate. */
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = anRefDepth(sy);
        else d = slotDepth(c, e);
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
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.call.args, i)));
        d = poolCallDepth(c, e, d);
        break;
    case EX_METHOD:
        d = maxInt(d, targetDepth(c, e->u.method.recv));   /* the receiver is an argument too */
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.method.args, i)));
        d = poolCallDepth(c, e, d);
        break;
    case EX_ENUMVAL:
        /* A payload construction such as `shape.holding(a[..])` puts the payload inside this
         * value, so the value's depth is the payload's depth, exactly as for an array literal.
         */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, targetDepth(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        d = poolCallDepth(c, e, d);
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
    if (dbgOn("EXTC_DBG_RHO")) {
        Expr *seen[40];
        int pure = targetDepthPure(c, e, 0, seen);
        if (pure != d)
            fprintf(stderr, "[rho] %s:%d kind=%d cached=%d pure=%d %s\n",
                    c->ctx && c->ctx->path ? c->ctx->path : "?", e->line, (int)e->kind, d, pure,
                    pure > d ? "UNDER" : "OVER");
    }
    if (!c->substParams && !mentionsParam(e->type)) anSetRefDepth(e, d);
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
     * recorded for a generic body needs this answer. `EX_DYN` must not take it either: the
     * payload is copied into the pool, so this value borrows whatever the payload borrows. */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_DYN: return exprBorrowed(c, e->u.dynv.payload);
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* A parameter is depth 0 and is not a global, so the value is borrowed. A global or
         * static is depth 0 as well, but anyone may store it.
         */
        if (root && root->depth == 0 && !isGlobalSym(c, root)) return true;
        /* A local that was **initialized from** a borrowed value carries that borrow with it:
         * `var q: slice<u8> = p` gives `q` the parameter's lifetime, so `G = q` stores a borrow
         * into a global although `q` itself is a local of this frame. Asking only about the
         * binding's own lexical depth answered `false` and let the write through (audit P0-6a:
         * `G` then read a sibling block's dead stack slot).
         *
         * Only a **name-like** origin is followed. A call result is deliberately excluded: `var x =
         * f(p)` may well be a fresh allocation that merely took `p` as an argument, and treating
         * that as borrowed would reject storing it -- the over-approximation that broke whole
         * corpora in the earlier attempt at this rule. */
        Sym *sy = (e->kind == EX_IDENT) ? identBindOf(e) : root;
        if (sy && sy->depth > 0 && sy->origin && sy->origin->kind == EX_IDENT)
            return exprBorrowed(c, sy->origin);
        /* Only a **plain binding** is followed, and the narrowness is load-bearing: following
         * `EX_FIELD` origins as well made `self.top = t.next` (read a reference out of the object
         * `self` points at, write it back into that same object) report "cannot store a borrowed
         * value into something that outlives this call". Both sides live behind the same `mut ref`,
         * so nothing is extended -- a false rejection, and it made the *other* corpus entry
         * (P0-17) look fixed while its real hole was still open. A rule that turns one hole into a
         * rejection of ordinary code is worse than the hole. */
        return false;
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
 * Promotion is the safe direction: it lengthens a pool rather than making the
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
/* `dyn Trait(x)` carries its payload with it, so the payload's allocation sites have to reach the
 * level this value has to reach -- exactly what the rest of this function does for a struct. */

bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops);
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
bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops) {
    if (!sy) return true;
    if (hops > 32) { EXTC_DBG_FALLBACK("promoteFieldsAt: recursion budget (32) exhausted"); return true; }
    /* An addressed root gets two questions, and they point in opposite directions: lowering a
     * recorded depth is unsound when an alias may write (it can put a deeper value in), while
     * promoting the site behind a recorded source is always safe -- it only moves the allocation
     * to a longer-lived arena. Refusing the promotion left `stash(ref b, n)` with `n = new node`
     * accepted while reading a freed block (audit P0-16): `ref b` marks `b` addressed, this
     * function bailed out, and the site kept its block level. */
    bool lower = !sy->addressed;
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
    }
    /* The same for a store a callee made through a `mut ref` argument (audit P0-16d). Nothing is
     * lowered here; the site behind the source is what has to move, because `return b` needs the
     * home arena. */
    if (sy->callSrc) {
        if (at < sy->callMinReq) sy->callMinReq = at;
        int want = sy->callSrcDepth;
        if (sy->callMinReq < want) want = sy->callMinReq;
        if (!promoteInto2(c, sy->callSrc, want, hops + 1)) ok = false;
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
/* Record one publication and change nothing else.
 *
 * This is the checking pass's whole involvement with arena levels. Deciding where an
 * allocation site goes is a separate pass that folds over these records together with
 * the depth fixed point, so no decision depends on the order the checker walked the
 * tree in. The same value may be recorded more than once at different levels; the fold
 * takes the minimum, which is the strongest requirement.
 *
 * Params:
 *   c    - checker
 *   val  - the value being published
 *   at   - arena level of the destination; 0 = beyond this frame
 *   line - for diagnostics
 */
void recordStore(Checker *c, Expr *val, Expr *target, int at, int line) {
    if (!c || !val || at < 0) return;
    if (anStoredAt(val) >= 0 && anStoredAt(val) <= at) {
        /* The value was already published at a level at least this shallow, so a record of
         * its own would add no lifetime requirement -- but only for the same destination.
         *
         * The destination is a separate fact, and it is the one that carries a requirement
         * back to the bindings the value was built from: `h = g` in one arm of an `if` and
         * `h = zero` in the other are two destinations for two different values, and it is
         * the destination, not the value, that decides which of them the binding's level
         * has to cover. Skipping the second record lost that arm, and with it the demand
         * that made the allocation inside the first arm outlive the block. */
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

/* 一次写的**唯一入口**（U3）：四件事按固定顺序做完，站点不再各写一遍。
 * 顺序是有意义的 —— `recordStore` 必须在 `promoteInto` 之前（求解器先看到要求），
 * 字段表必须在提权之后（`promoteFieldsAt` 会读它，先写会让它在同一次调用里看到新条目）。 */
void noteStore(Checker *c, Expr *value, Expr *target, int at, int line) {
    if (!c || !value) return;
    recordStore(c, value, target, at, line);          /* ① 电平记录 */
    promoteInto(c, value, at);                        /* ② 提权（提不动就保守） */
    markCallHomeIfEscaping(c, value, at);             /* ③ 逃逸集合那一半 */
    /* ④ 字段表 / 整值表 */
    Sym *vs = target ? placeRoot(c, target) : NULL;
    if (!vs) return;
    int d2 = valDepthForStore(c, value);
    if (!(d2 > 0 || (vs->type && typeContainsRef(c->tt, vs->type)))) return;
    if (target->kind == EX_IDENT) {
        /* 写点失效的**检查期**那一半（见 check_internal.h 的"写点失效"一节）：一个整值写发生时，
         * 关于这个绑定的非空证明必须已经作废。断言把它变成被看守的不变量 —— 一旦某个写路径漏了
         * `unNarrow`，调试构建当场响（P0-17 就是这一类，只不过那次漏的是"被调方改写"）。*/
        EXTC_DBG_ASSERT_MSGF(!isNarrowed(c, vs->cname),
                             "write without invalidating the non-null proof of `%s`", vs->name);
        noteWholeValueDepthWrite(c, vs, value, d2);
    } else {
        const char *fn2 = (target->kind == EX_FIELD) ? target->u.field.name : NULL;
        c->curStoreVal = value;
        noteFieldDepthWrite(c, vs, fn2, d2);
        c->curStoreVal = NULL;
    }
}

/* 返回值发布：目标就是值本身（没有"目的地"这个"), 层 0 = 活过本帧。见头文件的形状清单。*/
void noteReturnPublish(Checker *c, Expr *value, int line) {
    if (!c || !value) return;
    recordStore(c, value, value, 0, line);
}

/* Record a level-dependent rejection so it can be re-judged later. Reports nothing.
 *
 * Params:
 *   c     - checker; the record is appended to `c->lvlRejects`
 *   val   - the value that was rejected
 *   at    - the depth it was judged against
 *   depth - the depth the check saw
 *   line  - source line
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

/* Re-judge the recorded rejections with the numbers the passes settled on.
 *
 * The report stays where the check put it -- this only measures whether the settled
 * numbers agree. Keeping the two apart is what makes the question answerable: reporting
 * from here alone was measured to lose rejections, because the check's reader and this
 * one do not always see the same number.
 *
 * Params:
 *   c - checker
 *
 * Notes:
 *   - `EXTC_DBG_DEFER=1` prints one line per record. A record that the settled numbers no
 *     longer support is what a deferral would gain; one they still support is what a
 *     deferral must not lose, and is the reason the report has not been moved yet.
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
 * introduced. Promoting is always the safe direction: it lengthens a pool, and a longer
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
    if (hops > 32) { EXTC_DBG_NOTE("promoteInto2: recursion budget (32) exhausted"); return false; }
    /* `dyn Trait(x)`: the payload is copied into the pool and the handle keeps it alive, so the
     * payload's allocation sites have to reach the level this value has to reach. */
    if (val->kind == EX_DYN) return promoteInto2(c, val->u.dynv.payload, at, hops + 1);
    /* Record one fact in passing. This does not change any behaviour.
     *
     * Only at the outermost level (`hops == 0`): a replay re-runs exactly this level, and the
     * inner ones are reached by this call itself, so recording them too would only repeat
     * the work. */
    if (hops == 0) {
        if (dbgOn("EXTC_DBG_FACT"))
            fprintf(stderr, "[fact] %-8s at=%d kind=%d minAt=%d lexi=%d line=%d\n",
                    c->curFunc?c->curFunc->name:"?", at, (int)val->kind,
                    val->minAt, anLexicalLevel(val), val->line);
        recordLvlFact(c, val, at); applyLvlFact(c, val, at);
    }
    switch (val->kind) {

    /* `EX_GENCALL` is `alloc<T>(n)`. It is exactly symmetric with `new`:
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
        if (planArenaLevel(val) == ARENA_HOME) return true;
        int target = (at == 0) ? ARENA_HOME : at;
        if (planArenaLevel(val) > target) planSetArenaLevel(val, target);
        /* The one place where a level is converted back into a depth is
         * here. */
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
        /* Walk back along the binding's origin: `var n = new node` or `mid = n`
         * leads to that `new`. */
        Sym *sy = lookup(c, val->u.ident.name);
        if (sy && !sy->origin && sy->poolSite) {
            /* 这个绑定由一次调用初始化（`var inner = vector<i32>::new()`），而 `origin`
             * 对调用是空的（它只认分配站点的形状）。池不一样：层级就是调用点决定的，
             * 所以直接顺着那格池站点往下走 —— 下一跳是 `EX_CALL/EX_METHOD/EX_ASSOC` 那一格，
             * 那里才知道"建池的调用可以提权"。 */
            if (getenv("EXTC_DBG_ZONE"))
                fprintf(stderr, "[promote] EX_IDENT %s -> poolSite %s\n",
                        val->u.ident.name ? val->u.ident.name : "-",
                        sy->poolSite->kind == EX_CALL ? "EX_CALL" :
                        sy->poolSite->kind == EX_METHOD ? "EX_METHOD" : "EX_ASSOC");
            return promoteInto2(c, sy->poolSite, at, hops + 1);
        }
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

    case EX_CALL:
    case EX_METHOD:
    case EX_ASSOC:
        /* 建池的调用是**分配站点**，与 `new` 一样可以提权：新分配的东西没有别的所有者，
         * 所以"池生在哪一层地方"这个数可以改小（越小越长寿）。
         *
         * 这一格是容器能被放进更长寿的容器的前提：
         *     while … { var inner = vector<i32>::new()  outer.push(inner) }
         * `outer.push` 要求 `inner` 活到 `outer` 那一层 ⇒ 这里把 `inner` 的构造站点从
         * 循环体（第 2 层）提到外层（第 1 层），池就生在外层那个地方，出循环依然有效。
         *
         * 别的调用结果照旧不可提：`slice` 的视图来自别人的存储（借用），
         * 提不动就是提不动，返回 false 让调用方报错 —— 那是这一格的纪律。 */
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[gate] call=%s ptr=%p owner=%p mP=%d cmp=%d lvl=%d at=%d\n",
                    planCallee(val) && planCallee(val)->name ? planCallee(val)->name : "-",
                    (void *)planCallee(val), (void *)(planCallee(val) ? planCallee(val)->owner : NULL),
                    planCallee(val) ? (int)planMakesPool(planCallee(val)) : -1,
                    (int)calleeMakesPool(planCallee(val)), planZoneLevel(val), at);
        if (calleeMakesPool(planCallee(val)) && planZoneLevel(val) != 0) {
            int want = (at == 0) ? ZONE_HOME : at;
            if (planZoneLevel(val) > want) {
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[promoted] node=%p %d -> %d (at=%d)\n",
                            (void *)val, planZoneLevel(val), want, at);
                planSetZoneLevel(val, want);
            }
            if (anRefDepth(val) > at) anSetRefDepth(val, at);
            return true;
        }
        return false;

    default:
        /* Nothing else can be promoted (a `ref` to a local, a slice, and so
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
void recordLvlFact(Checker *c, Expr *val, int at) {
    /* `at == 0` 也要记：对 arena 那是"活到本帧之外（家 arena）"，对池那是 `ZONE_HOME`
     *（交给调用者选的地方）。**返回**那一档正是 `at == 0`（见 `check_stmt.c` 的 `return` 分支），
     * 而它过去不记事实 ⇒ 闭包之后的重放没有这一条 ⇒ `fn make() -> vector<i32>` 里的容器
     * 永远提不到调用者那一层（实测 ASan 仍在 `push` 上报 use-after-free）。 */
    if (!c || !val || at < 0) return;
    /* 记在**站点**上，不记在绑定上：末轮重放跑在所有函数体检查完之后，那时
     * `lookup` 已经查不到当时的局部绑定了（符号表是按函数/作用域的），
     * 记绑定等于记一条谁也走不通的链 —— 实测就是这么静默失手的。
     * 池的情形是"绑定由一次调用初始化"（`origin` 对调用为空），那种绑定身上带着
     * `poolSite`，把它换出来。（`origin` 那一族本来就在记录时被展平，同一个理由。） */
    if (val->kind == EX_IDENT) {
        /* Take the name **before** `val` is re-pointed at the origin: after the reassignment
         * `val` may be any kind, and reading `u.ident.name` off it reads another kind's union
         * member -- a wild pointer that made the `[fact-sy]` dump itself segfault. */
        const char *nm = val->u.ident.name ? val->u.ident.name : "-";
        Sym *sy = lookup(c, val->u.ident.name);
        /* 记**站点**，不记绑定：重放跑在所有函数体检查完之后，那时 `lookup` 对当时的局部
         * 绑定一律返回 NULL（符号表是按函数/作用域的）⇒ 记绑定等于记一条谁也走不通的链。
         * 绑定身上有 origin（`new`/`alloc` 那一族）或 poolSite（调用那一族）时，换成那个节点。 */
        if (sy && sy->origin) val = sy->origin;
        else if (sy && sy->poolSite) val = sy->poolSite;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[fact-sy] %s sy=%p origin=%p poolSite=%p\n",
                    nm, (void *)sy,
                    sy ? (void *)sy->origin : NULL, sy ? (void *)sy->poolSite : NULL);
    }
    /* Never record during the level solve: the solve re-enters this function, so
     * recording there would make the fact list grow without bound. */
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
    if (!val) return val;
    if (hops > 32) { EXTC_DBG_NOTE("carrier walk: recursion budget (32) exhausted"); return val; }
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
        recordRefCheck(c, val, target, slotDepth(c, target), line, "this assignment");
        return false;
    }
    /* The question here is which level the value is stored into, so this uses
     * `storeLayer` rather than `slotDepth`. For the trap with reference-typed bindings,
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
    recordStore(c, val, target, atStore, line);
    promoteInto(c, val, atStore);
    bool bad = checkEscape(c, val, at, line, "this assignment");
    /* When the origin of the value can be traced to a parameter, accept the store and let
     * the call site judge the lifetime.
     *
     * The callee is compiled once and does not know the caller's pool, so all it can do
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
     *   2. The destination is a container reachable through a parameter, so the pool of
     *     that storage is the pool of the corresponding argument at the call site.
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
        /* The excuse is "the destination and this call's home arena live exactly as long as each
         * other, so the call site has already guaranteed the value reaches that level". It does not
         * hold when the destination is a **global**: static storage outlives every arena, so no
         * home arena makes the borrow safe. `fn stash(p: slice<u8>) -> hbox { G = p  var b = new
         * u8[4]  return {p: b} }` was accepted and read `G = AAAAA...`, while the same function
         * returning `i32` (no home arena at all) was correctly rejected (audit P0-6b). Ask where the
         * destination lives instead of assuming: a global target falls through to the error. */
        Sym *destRoot = target ? placeRoot(c, target) : NULL;
        if (c->curFunc && c->curFunc->needsHome && !(destRoot && isGlobalSym(c, destRoot)))
            return bad;
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
    /* Free generic functions need this record as much as methods do: the replay loop already
     * walks `c.funcInsts` (see `refCheckApplies`), and the rule is the same one -- a `T` with no
     * zero value may not be defaulted. The `!owner` test here was written when only type
     * instances were replayed, so `fn mk<T>() -> T { var local: T  return local }` never recorded
     * anything and `mk<slice<u8>>()` produced C that did not even compile
     * (`__extc_reference_has_no_zero_value__`), while the method twin was correctly rejected
     * (audit P0-3). */
    if (!c->curFunc) return;
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
    rc->depth    = targetDepth(c, val);
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
        recordStore(c, val, NULL, at, line);
        promoteInto(c, val, at);
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
    /* The promotion has to run first here, the way `checkStoreEscape` has always ordered
     * it: promote, then compare depths. `promoteInto` is also the only entry point for
     * level facts, and the `d <= at` test below returns early, so as soon as the depth looks
     * shallow enough the site ends up with no constraint at all.
     *
     * Measured on `examples/escape-promotion`: on `return head`, `targetDepth(head)`
     * answers 0 first because the `refDepth` on that `EX_IDENT` node has not been filled
     * in yet, so the check returns early, the `new node` site keeps `minAt = -1`, the final
     * pass puts it back at its lexical level, and the caller receives a freed list.
     *
     * The order is therefore record the facts first and compare depths second; the
     * predicate itself is unchanged. */
    promoteInto(c, val, at);
    int d = targetDepth(c, val);
    if (d <= at) return false;
    /* The report below stays; this only keeps the question for the pass that runs after
     * the numbers are settled. */
    recordLvlRejection(c, val, at, d, line);
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
 *   tt       - the type table (a generic receiver's arguments are substituted)
 *   b        - the type, with any wrapper already stripped
 *   sym      - the operator name to look for (`==`, `<`, `<<`, ...)
 *   rhs      - the type of the right operand; operators are matched on it exactly, and
 *              this is what makes a name being defined more than once decidable
 *   fallback - operator name to try when `sym` is absent, or NULL for none
 *
 * Returns:
 *   The method definition, or NULL when the type defines neither name for that operand. */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback) {
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
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
 *   - The overloadable set is checked here and nothing else: any other name is left alone,
 *     so a method such as `compare` may return whatever it likes.
 *   - The result decides the answer for the whole family. A comparison feeds `if` / `&&`
 *     / `||` and extC has no implicit truthiness, so it can only return `bool`; an
 *     arithmetic operator returns its own type, and only its own type, because the result
 *     of `a + b` is used wherever `a` was. */
/* Check the signature of a constructor at its definition site.
 *
 * A constructor is the associated function named `new` (a function written inside a
 * `struct` without `self`), and `T(...)` is the spelling for calling it. That spelling
 * promises a `T`, so the body has to produce one; letting `new` return anything would
 * make `T(...)` mean whatever that function happens to return, and the reader of
 * `var f: ifstream = ifstream(path)` would have to go and look.
 *
 * The one exception is a **fallible** constructor: `result<T, E>` is accepted, because
 * opening a file, parsing text and allocating are all things that can fail, and the
 * caller then has to write `?` -- which is how extC says "this can fail" everywhere
 * else. The error type is the constructor's own choice.
 *
 * Params:
 *   c - checker
 *   f - the function definition being declared
 *
 * Notes:
 *   - Checked at the definition, not at each call site, so a wrong constructor is
 *     reported even when nothing calls it yet.
 *   - Only the name `new` is treated this way: `fn make(...)` is an ordinary
 *     associated function and may return whatever it likes.
 */
void checkCtorSig(Checker *c, FuncDef *f) {
    if (!f->isAssoc || !f->owner || strcmp(f->name, "new") != 0) return;
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
        /* The result has to be the owner type. By **value** is the rule for arithmetic:
         * the checker types `a + b` as the operand type, and the result stands where `a`
         * stood.
         *
         * The stream operators `<<` and `>>` may also return the owner **by reference**.
         * That is what a stateful stream needs to chain (`fin >> x >> line`): the result
         * is the same object rather than a copy of it, so the position that the first
         * `>>` advanced is the one the second one continues from. A copy would carry its
         * own position and the original would never move (docs/DECISIONS.md 88, the
         * `istream&` shape that C++ settled on).
         *
         * Only these two names get the second form. `a + b` returning a reference would
         * put a reference where the language says a value stands, and nothing about
         * addition needs it. */
        bool isStream = strcmp(f->name, "<<") == 0 || strcmp(f->name, ">>") == 0;
        Type *rb = f->ret ? ttBase(f->ret) : NULL;
        if (!rb || rb->sdef != f->owner || (!isStream && f->ret->kind == TY_REF)) {
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
    if (!b0 || b0->sdef != f->owner) {
        ckError(c, f->line, wantS,
                "the left operand of operator `%s` must be `%s`", f->name, f->owner->name);
        return;
    }
    /* The right operand may be any type: that is what makes an operator definable more
     * than once on one type (`out << i64` next to `out << f64`), and it is matched on the
     * operand's exact type at every use. */
}

/* Whether this type supports the overloadable operator `op`, natively or through a
 * method whose name is the operator.
 *
 * This is the deferred per-instance rule, and it is deliberately the *same* predicate the
 * concrete-type path in check_expr.c applies: two copies of one rule drift, and the drift
 * would show up as a generic accepting what its own instance rejects. Each family answers
 * "native" differently, exactly as the concrete path does:
 *   - `==` / `!=` are native for numbers, `bool` and payload-free enums;
 *   - the ordering operators are native for numbers only;
 *   - the arithmetic operators are native for numbers, and `%` for integers.
 * A user method is accepted for any of them, and an array's `==` is derived by the
 * compiler as long as its elements have one.
 *
 * The signature of a user method was already validated at its definition site
 * (checkOperatorSig), so finding it by name is enough here.
 *
 * Params:
 *   t  - the type the operator is applied to
 *   op - the operator name, one of `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%`
 *
 * Returns:
 *   True when the operator is available for this type. */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    bool isEq = isEqualityOp(op);

    if (isEq ? cmpIsNative(t) : ttIsNumeric(t)) {
        if (strcmp(op, "%") != 0 || ttIsInteger(t)) return true;
    }
    /* The compiler derives `==` for arrays, provided the elements can be compared -- and only
     * against the **same** array type. This used to recurse into the element and ask whether the
     * element compares with `rhs`, so `[1]i64 == i32` was approved at instantiation time (the
     * deferred re-check runs once `T` is known and the element `i64` does compare with `i32`);
     * the emitted descriptor comparison then read 8 bytes out of a 4-byte operand.
     * ASan: stack-buffer-overflow in `extc_eq`, found by tools/fuzz.py in a mutation of
     * examples/generic-free-fn.extc (docs/topics/HARDENING.md 三点七). */
    if (isEq && t->kind == TY_ARRAY)
        return rhs && ttEquals(ttBase(t), ttBase(rhs)) &&
               typeSupportsOp(tt, t->inner, op, t->inner);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(tt, b, op, rhs, isEq && strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
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
    rejectStreamBorrow(c, e);
    /* Only when a **value** is wanted: `f(ref s)` and `-> mut ref T` pass the object
     * itself, which is exactly what a `@noCopy` type is for -- refusing those would make
     * the annotation unusable. */
    if (!want || want->kind != TY_REF) rejectNoCopy(c, e, got);
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
/* Refuse to copy a `@noCopy` value.
 *
 * Params:
 *   c - checker
 *   e - the expression in a value position
 *   t - its type
 *
 * Returns:
 *   True after reporting.
 *
 * Notes:
 *   - Only a **place** is refused: `var s: ifstream = f()` binds a value the call just
 *     produced, which is not a copy of anything, while `var s2 = s` gives a second name
 *     to one reader whose position then moves behind the other name's back -- the exact
 *     failure the annotation exists to prevent.
 *   - `ref s` is a reference, not a copy, and stays legal.
 *   - The two callers are the two places the language decides "a value is needed here":
 *     `checkValue` (conditions, operands, discarded reads) and `checkInto` (arguments,
 *     returns, annotated bindings). One predicate, two doors.
 */
/* 绑定一个新名字时，右边是个 `@sharesStorage` 类型的 place ⇒ **警告**（不是错误）。
 *
 * 作者口径（2026-09-26）：「万一用户就是神人」—— 有些用途是正当的（把句柄交出去、
 * 短暂共享一份只读数据），禁掉就把路堵死了。但用户必须**知道**这一行在做什么，
 * 因为它与「别的值复制都是各一份」这条直觉相反，而且后果是静默的：
 *     var b = a            // 两份共用同一块板块
 *     b.push(i32(9))       // ← 改到了 a 的内容
 *     a.release()          // ← b 变成悬垂（守卫能 trap，但那已经是事故了）
 *
 * **只在这个形状上报**：`var b = a` / `let b = a` —— 无类型标注的绑定，右边是个名字。
 * 试过把它挂在"需要值的那两扇门"（`checkValue` / `checkInto`）上，结果是噪音：
 *   · `outer.push(inner)` —— 按值传参**本来就是**移交一份值，那正是用户要的；
 *   · `return v`（库内部）—— 把刚构造好的值交出去，更不是拷贝；
 *   · `let c = a.concat(b)` —— 右边是调用结果，新容器。
 * 一扇门报一片，那条真警告就被埋了（实测：这三个形状都会误报）。
 *
 * Params:
 *   c - checker
 *   e - the expression in a value position
 *   t - its type */
void warnSharedCopy(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    /* 库自己写的代码不报 —— 判断依据是**拷贝发生的位置**（`c->ctx->path` 是当前正在检查的
     * 那个文件），不是这个类型的来源：容器类型当然来自库（`sd->modName` 非空），
     * 但会写错的是用户那份 `var b = a`。
     * 不挡的话，用户每编译一次都会看到一串指向 stdlib 行号的警告，那条真警告就被埋了。 */
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    /* 新构造的值不是拷贝：`var a = vector<i32>::new()` / `f()` / `x.m()` / 结构体字面量 ——
     * 它们产生一个**全新的**容器，与任何已有容器不共用存储。只有"读一个已经存在的东西"
     * （`var b = a` / `s.f` / `v[i]`）才是那个需要被点出来的拷贝。 */
    if (e->kind != EX_IDENT && e->kind != EX_FIELD && e->kind != EX_INDEX) return;
    ckWarn(c, e->line,
           "Use `a.clone()` for an independent copy (explicitly O(n): one copy plus one new"
           " plate), or `ref a` / `mut ref a` to share it on purpose. Until then the two names"
           " are one storage: writing through one is visible through the other, and"
           " `release` on one leaves the other dangling.",
           "`%s` copies by value but **shares storage**: `var b = a` gives two names to one"
           " plate, not two containers", DN(sd));
}

/* `return e` 把一个 `@sharesStorage` 类型的**已有**值交出去 ⇒ 警告（作者口径 2026-09-26：
 * 「我写 var a = foo()，foo 里面 return b，那也很危险」）。
 *
 * 难点在"已有"与"刚建好"的分界 —— 两者写法上只差右边那一个词：
 *     return v                       // v 是参数或外层变量 ⇒ 交出去的是**拷贝**，两份共用存储
 *     var v = vector<i32>::new()     // 新建的值 ⇒ 交出去就是移交，没有第二份，不报
 * 判据用声明的行号：`Sym.line` 是声明所在行，而 `c->curFunc->line` 是函数**签名**那一行
 * ⇒ `sym->line <= curFunc->line` 就是参数（在外层），否则是这个函数体里新绑的。
 * 另外 `s.f` / `v[i]` 这类读已有东西的表达式一律算"已有"。
 *
 * 与"绑定那一处"的分工：那里管 `var b = a`（新名字指向同一块板块），这里管 `return b`
 * （值被交出去时也复制一份结构体）。两处合起来就是用户能遇到的"看起来是赋值、实际是共享"
 * 的全部形状。

 * Params:
 *   c - checker
 *   e - the returned expression
 *   t - its type */
void warnSharedReturn(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    if (e->kind == EX_IDENT && c->curFunc) {
        Sym *sym = lookup(c, e->u.ident.name);
        /* 函数体里刚绑的 ⇒ 是新建的值，交出去没有第二份 */
        if (sym && sym->line > c->curFunc->line) return;
    } else if (e->kind != EX_FIELD && e->kind != EX_INDEX) {
        /* 调用结果、字面量、`alloc` 之类：都不是"已有的一份" */
        return;
    }
    ckWarn(c, e->line,
           "Return it as `ref T` / `mut ref T` (a borrow, no copy), or `return v.clone()` when"
           " the caller really needs its own container. Returning by value copies the value:"
           " the caller and this frame then name one storage.",
           "returning `%s` by value copies a `@sharesStorage` type: the caller gets the same"
           " plate, not its own", DN(sd));
}

static bool rejectNoCopy(Checker *c, Expr *e, Type *t) {
    if (!e || e->kind == EX_REF || !t) return false;
    StructDef *sd = structOf(ttBase(t));
    if (!sd || !sd->noCopy || !isPlace(e)) return false;
    ckError(c, e->line,
            "Pass it as `ref` / `mut ref` instead: `f(ref s)`, or declare the parameter"
            " `mut ref T`. The state it carries has an identity, so a copy would give two"
            " names to one position.",
            "`%s` is `@noCopy`: it may not be copied by value", DN(sd));
    return true;
}

/* Is this expression the **borrow** that a stream operator hands back?
 *
 * `fin >> x` returns `mut ref ifstream`: the same object, so that the next `>>`
 * continues from the position this one advanced to. It is not a value, and unlike
 * `pickFirst(ref a, ref b)` -- where the user wrote `ref` and asked for a reference --
 * nothing was asked for here: the borrow is the operator's own doing. Storing it would
 * give a second name to one reader whose position is visible in neither name, so it is
 * refused with a message that says what to do instead.
 *
 * Params:
 *   e - the expression
 *
 * Returns:
 *   True when this is a `<<` / `>>` whose result is a reference. A shift's result is an
 *   integer, so it can never answer true.
 */
bool isStreamBorrow(Expr *e) {
    if (!e || e->kind != EX_BIN || !e->u.bin.op) return false;
    const char *op = e->u.bin.op;
    if (op[0] != '<' && op[0] != '>') return false;
    return e->type && e->type->kind == TY_REF;
}

/* Report a stored stream borrow.
 *
 * Params:
 *   c - checker
 *   e - the initializer, argument or returned expression
 *
 * Returns:
 *   True after reporting, so the caller can stop treating the value as usable.
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

    /* Inside `main` a failed step **traps**. There is nothing to hand a failure to --
     * `main` returns `i32` because C's entry point does -- and a program whose main step
     * failed is over anyway, so the honest thing is to say what went wrong and stop.
     * Opening a file in `main` is the commonest thing a program does, and without this
     * `?` was unusable exactly there (docs/DECISIONS.md 89).
     *
     * The rule is narrow on purpose: `main` only. Every other function still has to
     * declare a `result` to use `?`, because there the failure has somewhere to go. */
    if (c->curFunc && !c->curFunc->owner && c->curFunc->name &&
        strcmp(c->curFunc->name, "main") == 0 &&
        /* ...and only when the operand really carries a payload. `5?` inside `main` used to
         * reach `vecAt(&ob->targs, 0)` with an empty list and take the compiler down with a
         * SIGSEGV (found by tools/fuzz.py in five lines: `fn main() -> i32 { let x = 5?`).
         * Without the guard the ordinary path below reports the honest
         * "`?` needs an `option<...>` or `result<...>`" instead. */
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

/* A depth computed only from lexical block depths and origin chains.
 *
 * This is the diagnostic baseline for `EXTC_DBG_RHO`. It deliberately ignores two inputs
 * that the real `targetDepth` depends on:
 *
 *   - the arena level of an allocation site, which the level solver may still change and
 *     which holds a provisional marker while the checker runs. The block depth the site
 *     was born at (`lexicalLevel`) is used instead, since it is stable and the solver can
 *     only give a site a *longer* lifetime than the shallowest block it could occupy;
 *   - the depth cached on a node, which `targetDepth` itself writes and which can be
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
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen) {
    if (!e) return 0;                 /* nothing to ask about: 0 is the honest answer */
    /* The budget and the cycle guard are **fallbacks**, and both used to answer `0` -- the value
     * whose meaning is "lives the longest" -- so exhausting either of them pointed the
     * conservative direction backwards (audit P0-6c is the same family). They are supposed never
     * to be reached in a real program; a debug build must find out if that is true. */
    if (hops >= 40) { EXTC_DBG_FALLBACK("targetDepthPure: recursion budget (40) exhausted"); return DEPTH_UNKNOWN; }
    for (int i = 0; i < hops; i++)
        if (seen[i] == e) { EXTC_DBG_FALLBACK("targetDepthPure: origin cycle"); return DEPTH_UNKNOWN; }
    seen[hops] = e;

    switch (e->kind) {
    case EX_DYN:
        return targetDepthPure(c, e->u.dynv.payload, hops + 1, seen);   /* payload is copied into the pool */
    case EX_NEW:
    case EX_GENCALL:
        /* The block the site was born in. `lexicalLevel` is set the first time the node
         * is checked and does not move afterwards. */
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_IDENT: {
        /* Follow the origin to the site it came from; a binding on its own carries no
         * depth that is independent of where that site ends up. */
        Sym *sy = identBindOf(e);
        /* `!sy` 是"这个 ident 没解析过" —— 不该在检查器里发生（走到这里说明有别的 bug），
         * 而"没有 origin"是**合法**的：参数的 origin 就是空的，它的深度本来就是 0。*/
        if (!sy) { EXTC_DBG_FALLBACK("targetDepthPure: unresolved ident"); return 0; }
        if (!sy->origin) return 0;
        return targetDepthPure(c, sy->origin, hops + 1, seen);
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
        return targetDepthPure(c, e->u.field.obj, hops + 1, seen);
    }
    case EX_INDEX:
        return targetDepthPure(c, e->u.index.obj, hops + 1, seen);
    case EX_DEREF:
    case EX_SIGN:
        /* `*p` and `p!` name the same storage as their operand. */
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
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
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
        /* A call can only return a reference that came from its arguments or from static
         * storage, so the deepest argument is the bound. */
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
    /* `?` and a conversion pass the operand's references through; an operator can only carry
     * what its operands carry. This is the oracle the `EXTC_DBG_RHO` diagnostic compares
     * against, and `dfExprDepth` feeds the flow facts, so both have to answer for these. */
    case EX_TRY:   return targetDepthPure(c, e->u.try_.operand, hops + 1, seen);
    case EX_CONV:  return targetDepthPure(c, e->u.conv.operand, hops + 1, seen);
    case EX_UN:    return targetDepthPure(c, e->u.un.operand, hops + 1, seen);
    case EX_EXT: return targetDepthPure(c, e->u.ext_.call, hops + 1, seen);   /* `ext f(x)`: spawned call (cloned from EX_UN) */
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
