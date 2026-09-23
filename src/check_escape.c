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
 *      (depth 0).
 *   2. A local variable may not be initialized with a reference to something deeper
 *      than the variable itself.
 *   3. A field or element store (`o.f = v`) may not store a reference to something
 *      deeper than the target.
 *   4. Passing a reference into a function that stores it needs interprocedural
 *      analysis and is checked at the call site instead (see `checkCallRefArgs`).
 */

static int maxInt(int a, int b) { return a > b ? a : b; }

static bool isGlobalSym(Checker *c, Sym *s);   /* defined below; used by storeLayer */

int exprRefDepth(Checker *c, Expr *e);         /* defined below; used by placeDepth */

/* Depth of the storage a place expression denotes.
 *
 * A "place" is something that can be written: a binding, a field, an element, or a
 * dereference. The answer is how deep the *storage* is, not how deep a reference
 * stored there points; `storeLayer` answers the other question.
 *
 * Params:
 *   c - checker (scope stack and type table)
 *   e - the place expression
 *
 * Returns:
 *   Lexical depth of the storage: 0 for a parameter, global, or anything outside
 *   this frame; the block depth for a local; for a reference-typed binding, the
 *   depth of the storage it points at (the slot itself does not hold the value).
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
 *   c - checker
 *   e - the target place of a store
 *
 * Returns:
 *   Arena level of the storage. A local binding reports its block depth; a parameter
 *   reports 1, because the argument is a copy and the slot is in this frame; a global
 *   reports 0, because static storage outlives every frame. A place projected through
 *   a reference or parameter falls back to `placeDepth`, which reports the caller's
 *   depth (0), so storing into a caller's object is still treated conservatively.
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
 *   c - checker
 *   e - expression to inspect
 *
 * Returns:
 *   An upper bound on the depth of the deepest reference the value can carry.
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
 *   c - checker (type table)
 *   e - the value being stored
 *
 * Returns:
 *   True when the type provably carries no reference, so no requirement is needed.
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
    if (mentionsParam(e->type)) return false;    /* `T` 可能带引用 ⇒ 推迟 ✓ */
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
 *   c - checker
 *   e - the value being stored
 *
 * Returns:
 *   An upper bound on the depth of the references the stored bytes can carry.
 */
int valDepthForStore(Checker *c, Expr *e) {
    int a = exprRefDepth(c, e);
    if (typeCannotCarryRef(c, e)) return a;      /* 纯值拷贝 ⇒ 源对象的寿命不是约束 ✓ */
    int b = valDepthStructural(c, e);
    return a > b ? a : b;
}

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
        /* ⭐ A2：**别只看 `typeContainsRef`** —— 它对"字段类型本身是 `ref`"的聚合答 false
         * ⇒ `struct holder { p: ?ref i32 }` 的绑定会被判成 0，连字段表都不看 ✗
         * ⇒ 无论如何再取一次"各字段的上界" ✓ */
        if (sy) {
            for (int fi = 0; fi < sy->nfields; fi++)
                if (sy->fields[fi].depth > d) d = sy->fields[fi].depth;
            if (sy->otherDepth > d) d = sy->otherDepth;
        }
        break;
    }
    case EX_FIELD: case EX_INDEX:
        /* ⭐ 档2.3（ARENA-FORMAL §7.4）：**字段级深度** —— `h.p` 读的是"p 那一格记的数"，
         * 而不是"h 这个槽位在本帧"（后者会误拒；而且 `h.p = null` 之后也救不回来 ✗）✓
         * 没有那一格（比如刚声明 / 字段名对不上）⇒ 退回旧的保守算法 ✓ */
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
        /* **调用结果的深度 = 所有实参深度的最大值。**
         *
         * 为什么这是成立的上界：被调函数能返回的引用只有两种来源 ——
         * 全局/静态（深度 0），或者**实参**（深度 ≤ max(实参)）。
         * 它返回不了**自己的局部**（那条已被它自己的返回检查挡住 ✓）⇒ 没有第三种来源 ✓
         *
         * 以前的写法是「调用结果深度 = 0」，那是**错的**：
         *     fn identity(r: ref i32) -> ref i32 { return r }
         *     fn bad() -> ref i32 { var x: i32 = 5  return identity(ref x) }
         * 实测能编过、打印 0（悬垂）。现在：max(实参) = 1 > 0 ⇒ 编译错误 ✓
         */
        for (size_t i = 0; i < e->u.call.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.call.args, i)));
        break;
    case EX_METHOD:
        d = maxInt(d, exprRefDepth(c, e->u.method.recv));   /* 接收者也是实参 */
        for (size_t i = 0; i < e->u.method.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.method.args, i)));
        break;
    case EX_ENUMVAL:
        /* 带载荷构造 `shape.holding(a[..])` —— 载荷装进这个值里，
         * 所以它的深度就是载荷的深度（跟数组字面量同一个道理）✓ */
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
    /* 提到 `T` 的节点**不缓存**（那份深度是"假设 T 带引用"算出来的）✓ */
    if (!c->substParams && !mentionsParam(e->type)) e->refDepth = d;
    return d;
}

/* 把一个值放进「深度 at」的地方：它里面的引用活得够不够久？ */
/* 这个值里的引用是不是「**从外面借来的**」？—— 参数来的，或者函数调用回来的。
 *
 * 借来的东西**不能存进比这次调用活得更长的地方**（全局、或者别的参数指向的对象）：
 * 编译器**不知道它的真实寿命** —— 它可能指向调用者帧里比本函数更内层的局部。
 * 这就是 BOOTSTRAP §8 的 ④（参数洗白），跟「调用结果深度 = max(实参)」是同一件事的两半。 */
bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what);

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
 *   c - checker
 *   e - a place expression
 *
 * Returns:
 *   True when the storage is reached through a dereference or belongs to a parameter
 *   rather than to this frame. */
static bool placeIsBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_DEREF) return placeIsBorrowed(c, e->u.deref.operand);
    if (e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_INDEX) {
        Sym *root = placeRoot(c, e);
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    return false;
}

static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    /* 早退的唯一理由：这个类型里不可能有引用。
     * ⚠️ 提到 `T` 的**不许早退** —— 泛型体的推迟检查要用这个答案 ✓ */
    if (!typeContainsRef(c->tt, tsub(c, e->type)) && !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* 参数：深度 0 且不是全局 ⇒ 借来的 ✓
         * 全局 / 静态：也深度 0，但**谁都存得下它** ✓ */
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    case EX_SIGN:
        /* 签字只是换个说法，来源没变 ⇒ "借来的"这条照样跟着走 ✓ */
        return exprBorrowed(c, e->u.sign.operand);
    case EX_COALESCE:
        /* 两边都可能是结果 ⇒ 任一边借来的就算借来的 ✓ */
        return exprBorrowed(c, e->u.coalesce.main) ||
               exprBorrowed(c, e->u.coalesce.fallback);
    case EX_REF:
        /* ⚠️ **`ref *p` 是洗白路径**（2026-09-20 攻击测试打出来）：
         * `b.r = ref *p` —— 重新取一次引用，就看不出它来自参数了 ✗
         * `*p` 是"p 指的那个地方" ⇒ 要看**那个地方**借没借来 ✓
         * （注意不能走 exprBorrowed：`*p` 的**值**可能是 `i32`，会被早退挡掉 ✗）*/
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
        /* 带载荷构造：载荷是借来的 ⇒ 这个值也是借来的（跟数组字面量同理）*/
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    default: return false;      /* 字面量 / 全局 / alloc 出来的是本帧的 ✓ */
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
 *   c   - checker (currently unused, kept for symmetry with the other predicates)
 *   f   - function whose parameters are the tracing targets
 *   val - value to trace
 *
 * Returns:
 *   True when the root of the value is one of the parameters of `f`. */
bool valTracesToParam(Checker *c, FuncDef *f, Expr *val) {
    (void)c;                                  /* 现在只用得到"函数 + 值"（留着参数是为了
                                               * 跟别的谓词一个形状 ✓）*/
    if (!val || !f) return false;
    const char *root = placeRootName(val);
    if (!root) return false;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, root) == 0) return true;
    return false;
}

/* 把一个值**存进**某个地方之前的全部检查（深度 + 借来的东西）。 */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what);   /* 定义在后面 */

/* ⭐ 定案 63（PLAN #38）：**块级逃逸提升** —— "`new` 出来的东西被存进活得
 * 更久的地方 ⇒ 把那只 arena 提升到那一层，**不拒绝**"（主人 2026-09-22 拍板）✓
 *
 * 起点：`new` 默认分配进**当前块**的 arena（A2 按块细化：出块就回收）。
 * 可"在循环里建链表"这种**最正常**的写法，节点是在**循环体那一层**分配的，
 * 而 `head` 活在循环外 ⇒ 下一轮出块就把它释放了 ⇒ 悬垂（PLAN #38，ASan 实锤）✗
 *
 * 规则（一句话）：`arenaLevel = min(当前块深度, 所有目的地的深度)`
 *   · 深度越小 = 活得越久 ⇒ 取 min = 跟着**最长寿的那个目的地**走 ✓
 *   · 只往更深处存（min 没变）⇒ 行为**一个字都不变**（零假阳性）✓
 *   · 往更外层存 ⇒ 提升到那一层。最坏情况 = 一个**函数级、自动清理的堆**
 *     （主人原话：「最多就一直退化成一个会自动清理的类似堆的东西，没必要拒绝」）✓
 *
 * 为什么提升是**安全方向**（ARENA-FORMAL §7 那只旋钮：**把 region 拉长**，
 * 而不是"把事实变细"）：拉长寿命不会让任何还活着的引用悬垂；反过来"变细"
 * 要别名分析、会带来误拒 ✗ 见 `DECISIONS.md` 定案 63 ✓
 *
 * ⚠️ 只沿**值的载体**往回找（绑定 / `!` / `??` / 结构体字面量 / 枚举载荷 /
 *    数组字面量），**不追别名、不做跨函数推理** —— 提不动的照旧走原来的深度检查
 *    ⇒ 这个函数**永远不会**把"本来该报错的东西"放过去 ✓
 *
 * 返回 true = 这个值里所有 `new` 都确实活得到 `at`（可以放行 ✓）
 *      false = 里面有提不动的东西（调用方照旧报错）✓
 * ⚠️ 两个方向都要对：**父节点缓存的深度只有在全部子节点都成功时才能改** ——
 *    否则会记下一个"比实际更长寿"的数 ⇒ 那是**洞**（不是误拒）✗ */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops);
static bool promoteFields(Checker *c, Sym *sy, int at, int hops);
bool promoteInto(Checker *c, Expr *val, int at) { return promoteInto2(c, val, at, 0); }

/* ⭐⭐ 层 2（附录 D.2）：**顺着字段表的"来源"找到容器里那些站点** ✓
 *
 * 为什么需要：`Sym.origin` 只记"声明时那个初始化式" —— 而
 *   `var h: box = { p: null, q: null }` 的来路就是两个 null，**`x` 是后来 `h.q = x` 写的** ✗
 * ⇒ 顺着"h 里装着什么"想给 `x` 降层号时永远碰不到它 ⇒ 站点停在浅层 ⇒ 容器一死就悬垂 ✓
 *
 * 纪律（这次只做"提升"这一半 —— 降记账试过，会把别的绿用例弄红，见附录 D.3）：
 *   · 只提 `src` 存在的那一格；
 *   · **要提到哪一层** = 目的地 `at`（这一次把整个容器存到第 `at` 层 ⇒
 *     里面的引用活到那一层就够）✓ 不用字段表那个数（它是"现在装着什么"）；
 *   · **提成功才允许把那一格的记账跟着降到 `at`**（提不动 = 说不清 ⇒ 保留旧账 ⇒
 *     宁可误拒 ✓）；没有 `src` 的格（比如从没被写过的）**一个字不动** ✓ */
static bool promoteFields(Checker *c, Sym *sy, int at, int hops) {
    if (!sy || hops > 32) return true;
    if (sy->addressed) return true;                 /* 别名可能改它 ⇒ 表上的数说不清 ✓ */
    bool ok = true;
    for (int i = 0; i < sy->nfields; i++) {
        Expr *src = sy->fields[i].src;
        if (!src) continue;
        /* ⚠️ 要提到哪一层 = **min(它记的深度, 它被要求过的最浅层)** ✗
         * 只看"这一次的目的地"不够：`out = h` 是第 1 层，可 `return out` 要的是
         * **帧外**（0）⇒ 站点最终得进家 arena；只看 1 ⇒ 提到 1 就"够"了 ⇒ 而函数
         * 一返回第 1 层就 release ⇒ 悬垂（实测：ASan 仍报 heap-use-after-free）✗ */
        if (at < sy->fields[i].minReq) sy->fields[i].minReq = at;
        int want = sy->fields[i].depth;
        if (sy->fields[i].minReq < want) want = sy->fields[i].minReq;
        if (!promoteInto2(c, src, want, hops + 1)) { ok = false; continue; }
        if (sy->fields[i].depth > want) sy->fields[i].depth = want;   /* 提成功 ⇒ 跟着降 ✓ */
    }
    return ok;
}

/* ⭐⭐ 长运行内存：把 **"这个值得装得下第 `at` 层"** 记成一条事实 ✓
 *
 * 为什么记在 `promoteInto2` 的**入口**：**每一处"往外存"** 都要过这里
 * （`checkStoreEscape` / `checkEscape` 都先调它），而它自己就是那个"沿**值的载体链**
 * 往里走"的函数 ⇒ **不另写一个遍历去找存储点** ⇒ 就没有"漏掉某个存储点"
 * 的余地（因为根本没有第二个遍历可以漏 ✓）
 *
 * ⚠️ 只记 **有限层**（`at >= 1`）：`at == 0` = "要活到帧外"，那一档由 `ARENA_HOME`
 * 表示，不能再塞回"层号"里去 ✗（两者关系 = `解出来的 L == 0 ⇔ home`）*/
static void recordLvlFact(Checker *c, Expr *val, int at);
static void applyLvlFact(Checker *c, Expr *val, int at);

/* **这个值的"根来路"是哪个表达式？**（定义在下面 —— 记来路时用它**压平**链条）*/
Expr *originOf(Checker *c, Expr *val, int hops);

/* 记下"这个绑定的来路"（`var n = new node` 的初始化式 / `mid = n` 的右式）——
 * ⚠️ **压平成根来路**再记：链条里的中间变量可能**已经出了作用域**，
 * 那时 `lookup` 找不到 ⇒ 链就断了 ✗（真踩过：双层循环里的 `head = mid`）
 * ⇒ 记的时候（作用域还在）就一路走到根 ✓ */
void noteOrigin(Checker *c, Sym *sy, Expr *val) {
    if (!sy || sy->addressed) return;
    /* ⚠️⚠️ **字面量不许盖掉"已经追得到的来路"** ✗✗
     *
     * 为什么（gdb 实测，`tests/arena-promoted/C2_if_join_refbinding`）：
     *     var p: ?ref i32 = null
     *     if c == 1 { p = x } else { p = null }
     * 检查是**顺序**做的 ⇒ `p = x` 先把来路记成那个 `alloc` 站点 ✓，
     * 紧接着 `p = null` 又把它盖成 `EX_NULL` ⇒ 后面顺 `out = p` 追站点时
     * 只看到 `null`（`org=24`）⇒ 追不到 ⇒ 站点留在块层 ⇒ **悬垂**（ASan 实锤）✗
     *
     * 判据：`null` / 字面量**指不出任何站点** ⇒ 只有在"现在还没有可追的来路"时
     * 才让它记（否则保持已有那条）✓
     * ⚠️ 反过来也要对：一开始就是 `= null` 的绑定必须让 `null` 记进来 ——
     *    否则 `origin` 停在 NULL 上，`promoteInto2` 会**早退**（"没有来路"）✗ */
    bool literal = val && (val->kind == EX_NULL || val->kind == EX_INT || val->kind == EX_FLOAT ||
                           val->kind == EX_BOOL || val->kind == EX_STR);
    bool haveReal = sy->origin && sy->origin->kind != EX_NULL && sy->origin->kind != EX_INT &&
                    sy->origin->kind != EX_FLOAT && sy->origin->kind != EX_BOOL &&
                    sy->origin->kind != EX_STR;
    if (literal && haveReal) return;
    sy->origin = originOf(c, val, 0);
}

static bool promoteInto2(Checker *c, Expr *val, int at, int hops) {
    if (!val) return true;
    if (at < 0) return false;                      /* 说不清的层 ⇒ 别动 ✓ */
    /* ⚠️ **步数上限**：绑定之间可能成环（`a = b  b = a` —— 赋值会更新"来路"）⇒
     * 不设限就是死循环 ✗ 撞上限 = 提不动 = 退回老行为报错（安全方向）✓ */
    if (hops > 32) return false;
    /* ⭐ 长运行内存：顺手记一条事实（**不改变任何行为** ✓）
     * ⚠️ 只在**最外层**记（`hops == 0`）：重放时重跑的就是这一层，
     *   内层那些是它自己走出去的 ⇒ 记下来只会重复劳动 ✓ */
    if (hops == 0) {
        if (getenv("EXTC_DBG_FACT"))
            fprintf(stderr, "[fact] %-8s at=%d kind=%d minAt=%d lexi=%d line=%d\n",
                    c->curFunc?c->curFunc->name:"?", at, (int)val->kind,
                    val->minAt, val->lexicalLevel, val->line);
        recordLvlFact(c, val, at); applyLvlFact(c, val, at);
    }
    switch (val->kind) {

    /* ⭐ `EX_GENCALL` = `alloc<T>(n)` / `allocSlice<T>(n)` —— **B2 之后它和 `new` 完全对称**
     * （同一套层号规则、同样登记进 `arenaSites`）⇒ 提升规则也必须对称 ✗
     * 以前它落到 `default: return false`（"提不动"）⇒
     * 「返回一块 `alloc` 出来的内存」整族提不动 ⇒ 照旧误拒 ✗ */
    case EX_NEW:
    case EX_GENCALL: {
        /* 目的地深度 0 = "调用者那一级"（"家"arena）—— **只有"有家"的函数有** ✓
         * 没有家 ⇒ 提不到那一层 ⇒ 原样返回 false（照旧报错，安全方向）✓
         * ⚠️ 定案 68：那一档现在用 `ARENA_HOME` 表示（以前是拿 0 **兼职**的 ✗ ——
         *    0 在新编码里是"还没定"）⇒ 写回去之前必须翻译一下 ✓ */
        if (at == 0 && !(c->curFunc && c->curFunc->needsHome)) return false;
        /* ⭐⭐ 走到这里 = **逃逸分析已经沿载体链碰到了这个站点**（不是"因为所在函数
         * 有家"那种兜底）⇒ 打上"它要活到帧外"的标记 ✓
         * ⚠️ 必须在**早退之前**：预负已经把有家函数里每个 `new` 都置成了 `ARENA_HOME`
         * （所以下面那句早退会命中），可那是兜底、不是证据 ✗✗ */
        /* ⭐ 记下"逃逸分析对它提出的最强要求"（层号越小越强）✓
         * ⚠️ 不能只记一个布尔："碰过它"≠"要求它活到帧外" ✗
         *   （`nb = new item[cap*2]` 只需活到当前帧；当成"进家" ⇒ `main` 没有家却吐 `__extc_home` ✗）
         * ⚠️ 取**最小**：层号越小活得越久，所以"最强的要求"就是最小的那个 ✓ */
        if (val->minAt < 0 || at < val->minAt) val->minAt = at;
        if (val->arenaLevel == ARENA_HOME) return true;   /* 已经在家：家最长寿，不用再提 ✓ */
        int target = (at == 0) ? ARENA_HOME : at;
        if (val->arenaLevel > target) val->arenaLevel = target;
        int depth = arenaDepthOf(val->arenaLevel);          /* ⭐ 唯一换算处 ✓ */
        if (val->refDepth > depth || val->refDepth == 0)
            val->refDepth = depth;
        return true;
    }

    case EX_IDENT: {
        /* 顺着绑定的**来路**往回走：`var n = new node` / `mid = n` ⇒ 找到那个 `new` ✓ */
        Sym *sy = lookup(c, val->u.ident.name);
        if (!sy || !sy->origin) return false;
                /* 槽位本来就活到 `at` 之外（更浅）⇒ 里面的东西本来就住在那一层 ✓
         * （初始化式是在**同一条语句**里求值的 ⇒ 它的层 = 槽位的深度 ✓）
         * ⚠️ **但"调用结果"是例外** —— 有家被调者分配进的那只 arena 是**调用点**
         * 选的，不一定是槽位这一层 ⇒ 那份深度在**绑定时**就记进 `Sym.refDepth` 了
         * （见 `check_stmt.c` 里那段"初始化式是调用"的处理）⇒ 那才是这家事的权威 ✗ */
        /* ⚠️⚠️ **层 2：这一档不许再"提前返回"了** ✗✗
         * 老判据说"槽位活得够久 ⇒ 不用提升"—— 对**提升**是对的，可 `promoteInto`
         * 同时还是**层号事实的唯一入口** ⇒ 提前返回 = 这个站点**一条约束都拿不到**
         * ⇒ 收尾按"没人碰过"把它放回**词法层** ✗
         * 实测（`examples/escape-promotion` 的 `fn build`）：`var head = new node` 的站点
         * `minAt=-1` ⇒ 被放回第 1 层 ⇒ `build` 一返回就 release ⇒ 调用者手里是
         * 已释放的链表（生成的 C 从 `&(*__extc_home)` 变成 `&__extc_a[1]` ✗）
         * ⇒ 现在**照样往下走一趟**（只为记事实），返回值仍按老判据答"不用改" ✓ */
        bool slotDeepEnough = (sy->depth <= at);
        /* ⚠️ 来路是**压平的根**（见 `noteOrigin`）⇒ 这里链长最多一层，不会再查绑定 ✓ */
        bool ok = promoteInto2(c, sy->origin, at, hops + 1);
        /* ⭐⭐ 层 2：来路里只有"声明时写的那些字段" ⇒ 再读一遍**字段表的来源** ✓
         * （`h.q = x` 这种后来的写只在字段表里 —— 见 `promoteFields` 的注释 ✓）*/
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

    /* ⭐ 层 1：**新增这三种形状** —— `promoteInto` 要能沿**值的载体**走到
     * 里面的分配，否则"这一处逃出去了"这个事实根本没机会被发现 ✗
     *   `var v: varArray<T> = { buf: new T[cap], … }  return v`、`o.f = cell`、`a[i] = cell`
     * —— 这三种都是最常见的形状 ✓
     * 纪律跟 `EX_STRUCTLIT` / `EX_ARRAYLIT` 一样：**只沿值的载体往里走**，
     * 不追别名、不做跨函数推理 ⇒ 提不动照旧 false（调用方照旧报错）✓ */
    case EX_SLICE:
        return promoteInto2(c, val->u.slice.obj, at, hops + 1);
    case EX_FIELD:
        return promoteInto2(c, val->u.field.obj, at, hops + 1);
    case EX_INDEX:
        return promoteInto2(c, val->u.index.obj, at, hops + 1);

    case EX_COALESCE: {
        /* ⚠️ 两边都要试（不能短路）—— 只提一边就会漏掉另一边那个 `new` ✗ */
        bool a = promoteInto2(c, val->u.coalesce.main, at, hops + 1);
        bool b = promoteInto2(c, val->u.coalesce.fallback, at, hops + 1);
        return a && b;
    }

    case EX_STRUCTLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.lit.inits.len; i++)
            if (!promoteInto2(c, (*(FieldInit **)vecAt(&val->u.lit.inits, i))->value, at, hops + 1))
                ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;     /* 缓存跟着收紧 ✓ */
        return ok;
    }

    case EX_ARRAYLIT: {
        bool ok = true;
        for (size_t i = 0; i < val->u.arraylit.elems.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.arraylit.elems, i), at, hops + 1)) ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;
        return ok;
    }

    case EX_ENUMVAL: {                       /* 带载荷构造：载荷要一起提 ✓ */
        bool ok = true;
        for (size_t i = 0; i < val->u.enumval.args.len; i++)
            if (!promoteInto2(c, *(Expr **)vecAt(&val->u.enumval.args, i), at, hops + 1)) ok = false;
        if (ok && val->refDepth > at) val->refDepth = at;
        return ok;
    }

    default:
        /* 别的形状一概提不动（`ref 局部` / 函数调用的结果 / 切片…）——
         * **不猜**：照旧走原来的深度检查报错 ✓ */
        return false;
    }
}

static void recordLvlFact(Checker *c, Expr *val, int at) {
    if (!c || !val || at < 1) return;
    if (c->lvlSolving) return;    /* ⭐ 解算期不记账 ✗（否则无限长 —— 真踩到）*/
    LvlFact *f = (LvlFact *)arenaAllocZero(c->arena, sizeof(LvlFact));
    f->val = val;
    f->at  = at;
    *(LvlFact **)vecPush(&c->lvlFacts) = f;
}

/* ⭐ 把一条事实应用到站点上 —— **取最强的要求**（层号最小）✓
 * 它跟 `promoteInto` 是两件事（一个是"记事实"，一个是"改层号"），
 * 但**跑在同一次遍历里**（都在 `promoteInto2` 的入口）⇒ 不可能漏一处 ✓ */
static void applyLvlFact(Checker *c, Expr *val, int at) {
    (void)c;
    if (!val || at < 0) return;
    if (val->minAt < 0 || at < val->minAt) val->minAt = at;
}

Expr *originOf(Checker *c, Expr *val, int hops) {
    if (!val || hops > 32) return val;
    if (val->kind != EX_IDENT) return val;          /* 结构体字面量 / `new` / 别的形状 ⇒ 就是它自己 ✓ */
    Sym *sy = lookup(c, val->u.ident.name);
    if (!sy || !sy->origin) return val;             /* 没有来路 ⇒ 它自己（提不动就照旧报错）✓ */
    return originOf(c, sy->origin, hops + 1);
}

bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    /* 同上：提到 `T` 就整条推迟 ✓（这里**直接返回**，别让里面的 checkEscape 再记一遍）*/
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, target, placeDepth(c, target), line, "this assignment");
        return false;
    }
    /* ⚠️ 这里问的是「**往哪一层**存」⇒ 用 `storeLayer`（不是 `placeDepth`）：
     * 引用型绑定那个坑见 `storeLayer` 的注释 ✓ */
    int at = storeLayer(c, target);
    /* ⭐⭐ 层 2：**"存进去"这个动作需要的层号，由容器的寿命封顶** ✗✗
     * `storeLayer` 对 `h.q` / `a[i]` 这种**投影**会答"投影那条路的深度"，
     * 比容器**自己**的块层还深（`h.q` 答 2，而 `h` 在 1）✗ ——
     * 而容器活不过它自己那一层作用域 ⇒ 往它里面存的东西也只需要活到那一层 ✓
     * ⚠️ 只把**记账用的这个数**降下来；下面 `checkEscape` 的 `at` 一个字不动 ✓ */
    int atStore = at;
    if ((int)c->scopes.len < atStore) atStore = (int)c->scopes.len;
    /* ⭐ 定案 63：先试着**提升**（提不动的话下面那句照旧报错 —— 这里是安全网，幂等）✓ */
    promoteInto(c, val, atStore);
    bool bad = checkEscape(c, val, at, line, "this assignment");
    /* ⭐ 定案 67（`ARENA-FORMAL` §9.3）：**来路能追到参数** ⇒ 放行，交给**调用点**判寿命 ✓
     * 为什么能放：被调者一次编译、不知道调用者的区域 ⇒ 它只能**发布约束**；
     *   "实参 j 的数据活得 ≥ 被调者会存进去的那只容器"这条，**调用点**算得出来
     *   （`checkCallRefArgs` 里 `Cont(j)` 那一支 ✓）✓
     * ⚠️ 追不到的（调用结果 / 本帧局部 / 不明来路）⇒ **照旧拒** ✗
     *   —— 与摘要里 `otherMask` 的口径保持一致（两边都不放 ✓）*/
    /* ⭐ 定案 67：**放宽要同时满足两个前提**（缺一不可 ✗）
     *   ① 值的**来路追得到形参** ✓ ⇒ 调用点能算它的寿命
     *   ② 目的地**是形参可达的容器** ✓ ⇒ 那块存储的区域 = 调用点那只实参的区域 = h ✓
     * ⚠️ ② 不能省：目的地是**全局**时（`g = s`）约束是"数据得活到永久（深度 0）"✗
     *    而调用点的 `h` 说的是"被调者会存进哪只容器"—— 表达不了"永久" ⇒ 照旧拒 ✗
     *    （`tests/errors/escape_stash_borrowed.extc` 当场抓到这个洞 ✓）
     * ⚠️ 反过来，"同层"的那种调用现在**合法**了 ✓（它本来就安全：两个一起死 ✓
     *    —— 以前被 blanket 规则连带拒掉 ✗ 这就是主人说的"好多地方卡了一次" ✓）*/
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
        /* ⭐ A3 第三半：**有家函数里放行** —— 调用点已经保证了"每个 `ref`/`mut ref`
         * 实参都活得 ≥ 这一刀的家 arena"（规则 ④ ✓），而被调函数分配的东西**就进那只
         * 家 arena** ⇒ 写进去的东西跟它一样长寿 ✓
         * ⚠️ 没有 ④ 直接放行就是洞（`ref_launder_field` 当场抓过 ✗）*/
        if (c->curFunc && c->curFunc->needsHome) return bad;
        ckError(c, line,
                "A borrowed value may not be stored where it outlives the call: its real "
                "lifetime is unknown here. Copy it, or store it into a local of this frame.",
                "cannot store a borrowed value into something that outlives this call");
        return true;
    }
    return bad;
}


/* 记一条推迟的规矩。**只在泛型体里、且值提到了类型参数时**才记 ✓
 * （具体类型的值现在就查得清楚，不用推迟 —— 推迟只会让报错变晚）*/
/* 记一条"零值"的推迟检查（跟引用规矩同一族，见 RefCheck 的注释）*/
/* 记一条"`new T[n]` 的大小"的推迟检查（第三类，见 RefCheck 的注释）*/
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
    /* ⚠️ **数字现在就算好**（那时作用域还在、`lookup` 找得到参数）：
     * 走路函数对"提到 `T`"的节点不会早退，所以这份深度是按"`T` 可能带引用"算的 ——
     * 正是实例化时要用的那一个 ✓
     * （踩过的弯路：推到实例化再算 ⇒ 那时函数作用域已经没了，深度算成 0，检查静默失灵 ✗）*/
    rc->depth    = exprRefDepth(c, val);
    rc->borrowed = exprBorrowed(c, val);
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    if (!val) return false;
    /* ⚠️ 值里提到 `T` ⇒ 现在下不了结论（`T` 可能是 `i64` 也可能是 `slice<u8>`）
     * ⇒ **记下来，等实例化再算** ✓ （这就是 #17 那个洞的封口）*/
    if (mentionsParam(val->type)) {
        /* ⭐⭐ 层 2：**先提一次，再推迟** ✗✗
         * `promoteInto` 是**层号事实的唯一入口**，而这一支**提前返回** ⇒
         * 「返回值/实参里装着 `T`」的每一处**都拿不到约束** ✗
         * 实测（`varArray<T>::withCap`）：`return v` 走这一支 ⇒ 里面那个 `new T[cap]`
         * 一条事实都没有（`minAt=-1`）⇒ 收尾按"没人碰过"放回**块层** ⇒ 而返回值
         * 要求它活在**帧外** ⇒ 实例复查报 "depth 1, but this can only hold up to 0" ✗
         * （一整族误拒，13 条）
         * 为什么可以现在就提：**层号跟 `T` 是什么无关**（`T = i64` 与 `T = slice<u8>`
         * 的"这块存储要活到第 k 层"是同一句话 ✓）
         * ⚠️ 返回值**忽略**：提不动不是错误 —— 该不该拒由下面那条推迟的规矩判 ✓ */
        promoteInto(c, val, at);
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
    /* ⭐⭐ 层 2：**这里必须先提一次**（`checkStoreEscape` 早就是这么排的：
     * 先 `promoteInto` 再判深度）—— 因为 `promoteInto` 同时是**层号事实的唯一入口**，
     * 而下面那句 `d <= at` 会**提前返回** ⇒ 一旦深度看着"够浅"，站点就一条约束都没有 ✗
     * 实测（`examples/escape-promotion`）：`return head` 时 `exprRefDepth(head)` 先答 0
     * （那个 `EX_IDENT` 节点上的 `refDepth` 还没填）⇒ 早退 ⇒ `new node` 的站点
     * `minAt=-1` ⇒ 收尾把它放回**词法层** ⇒ 调用者拿的是已释放的链表 ✗✗
     * ⇒ 顺序改成"先记事实、再判深度"，判据一个字没变 ✓ */
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

/* 沿一个「地方」往内走，路上有没有**只读引用**？
 *
 * 「绑定是不是 `let`」和「路上有没有只读引用」是**两件事**：
 * `var` 的东西里也可能装着一个只读引用（比如 `fn f(v: ref slice<i32>)` 里的 v）。
 * 写进去要**两样都满足**。 */
/* ⭐ PLAN #40 / #41：**写这个「地方」要穿过哪些引用？每一只都必须是 `mut ref`** ✓
 *
 * "穿过"只有两种：
 *   · **显式**：`*p` ✓
 *   · **隐式**：`p.f` / `v[i]` 里 `p` 是引用 ⇒ 那块存储住在 **`*p`** 里（自动解引用）✓
 * ⚠️⚠️ **目标自己的类型不算**：`cur = v`（换指向）写的是**槽位**，
 *    "cur 是不是只读引用"跟这件事**无关** ✗
 *    （老实现把这两件事混在 `pathHasReadonlyRef` 里 ⇒ 一边误拒 `(*cell).next = v`
 *      （字段类型是只读引用 `?ref node`）、一边漏掉真正的写穿 ✗ 见 PLAN #40/#41）✓
 *
 * `*crossed` 回填"存储到底在不在本帧"：穿过引用 ⇒ 存储在被指对象里（`placeRoot` 会是 NULL）✓ */
bool pathRefsAllMut(Expr *e, bool *crossed) {
    if (crossed) *crossed = false;
    for (Expr *x = e; x; ) {
        if (x->kind == EX_DEREF) {                       /* 显式穿过 ✓ */
            Type *ot = x->u.deref.operand->type;
            if (!(ot && ot->kind == TY_REF && ot->mut)) return false;
            if (crossed) *crossed = true;
            x = x->u.deref.operand;
            if (x->type && x->type->kind == TY_REF) return true;   /* 落地 = p 指的地方 ✓ */
            continue;
        }
        Expr *o = NULL;
        if (x->kind == EX_FIELD)      o = x->u.field.obj;
        else if (x->kind == EX_INDEX) o = x->u.index.obj;
        else if (x->kind == EX_SLICE) o = x->u.slice.obj;
        if (!o) break;                                   /* 落到绑定/别的形状 ⇒ 到此为止 ✓ */
        if (o->type && o->type->kind == TY_REF) {        /* 字段住在 o **指的对象**里 ✓ */
            if (!o->type->mut) return false;
            if (crossed) *crossed = true;
            return true;
        }
        x = o;
    }
    return true;
}

bool pathHasReadonlyRef(Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->type && x->type->kind == TY_REF && !x->type->mut) return true;
        /* ⭐ PLAN #41：**显式穿过**（`(*p).f` / `(*p).f.g`）——
         * 老实现走到 `EX_DEREF` 就 `break`，而 DEREF 自己的类型是**被指对象**
         * （不是引用）⇒ 那只引用的 mut 从来没查过 ⇒ 写穿只读引用整条放行 ✗
         * （实测：`fn g(p: ref node) { (*p).val = 7 }` 编译通过 ✗）✓ */
        if (x->kind == EX_DEREF) {
            Type *ot = x->u.deref.operand->type;
            if (ot && ot->kind == TY_REF && !ot->mut) return true;
            x = x->u.deref.operand;
            if (x->type && x->type->kind == TY_REF) return false;   /* 落地 = p 指的地方 ✓ */
            continue;
        }
        if (x->kind == EX_FIELD) { x = x->u.field.obj; continue; }
        if (x->kind == EX_INDEX) { x = x->u.index.obj; continue; }
        if (x->kind == EX_SLICE) { x = x->u.slice.obj; continue; }
        break;
    }
    return false;
}

bool requireMutable(Checker *c, Expr *e, int line, const char *what) {
    /* ⚠️ `*p` 要走**自己那条**：`placeRoot` 认不出它（会返回 NULL ⇒ 被当成可写）✗
     * 可写性由**引用的类型**给：只有 `mut ref` 才能 `*p = v` ✓ */
    if (e && e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        if (ot && ot->kind == TY_REF && ot->mut) return false;
        ckError(c, line,
                "`*p = v` needs `p` to be `mut ref T`; a `ref T` is a read-only borrow",
                "cannot %s through a read-only reference", what);
        return true;
    }
    /* ⚠️ 这里问的是「**能不能写穿**」—— 看的是**每个路口那只引用的 mut**：
     *   · 这个"地方"自己的类型是只读引用（`c.bump()` 里 c 是 `ref counter`）✗
     *   · 路上**显式穿过**了只读引用（`(*p).f`，PLAN #41 的洞）✗
     * 两件都由 `pathHasReadonlyRef` 答（#41 就是给它补上 DEREF 那一支 ✓）*/
    if (pathHasReadonlyRef(e)) {
        ckError(c, line,
                "`ref T` is a **read-only** borrow; writing through it needs `mut ref T` "
                "in the declaration. Read-only is the default so that a signature says "
                "what it does.",
                "cannot %s through a read-only reference", what);
        return true;
    }
    Sym *root = placeRoot(c, e);
    /* **视图的元素可不可写，看视图的类型带不带 `mut`。**
     * 这条关掉的是「按值传进来的视图」那个洞：
     *     fn f(v: slice<i32>) { v[0] = 1 }   // ✗ 参数是副本，但元素是调用者的！
     * 要写就得在签名上写 `mut slice<i32>`（或者 `mut ref slice<i32>`）。 */
    if (root && root->type && root->type->kind == TY_GENERIC &&
        ttIsViewType(root->type) && !root->type->mut &&
        /* 写**元素**才受视图可写性管；给视图**整体赋值**（换绑）不受它管 */
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

/* 一个类型是不是「视图」？视图的协议是 `data` + `len`（编译器认这条协议，
 * 但它的结构和方法都在 stdlib/prelude.extc 里）。返回元素类型，不是视图就返回 NULL。
 * 见 ARRAYS.md：语言认识「协议」，库提供「方法」。 */
Type *viewElemOf(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return NULL;
    if (strcmp(t->sdef->name, "slice") != 0) return NULL;
    if (t->targs.len != 1) return NULL;
    return *(Type **)vecAt(&t->targs, 0);
}

/* C 原生就能比的类型：数值 / bool / 枚举。
 * `str` **不在**这里 —— 它的 `==` 会退化成指针比较（陷阱），必须有 eq 才行。 */
bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    /* 无载荷枚举在 C 里就是整数 ⇒ 直接比 ✓
     * **带载荷**的不行：C 里它是 `struct { tag; union }`，而 C 的 struct 不能用 `==`。
     * 用 `match` 比（以后可以派生出 `_eq` —— 那要递归比载荷，先不做）。 */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
}

/* 找类型上定义的运算符方法。
 * `!=` 是特例：没定义 `!=` 就退回用 `==` 取反（codegen 那边会自动取反）。*/
FuncDef *findOp(Type *b, const char *sym, const char *fallback) {
    FuncDef *m = findMethod(b, sym);
    if (!m && fallback) m = findMethod(b, fallback);
    return m;
}

/* 运算符方法的签名，在**定义处**就检查。
 *
 * ⚠️ 这里踩过一个坑：最初只在**使用处**检查签名，于是
 *   ① 定义了签名错误的 `fn !=` 但没直接用到 → 一直没人查
 *   ② 泛型里 `a != b` 推迟到实例化，codegen 找到了那个 void 的 `!=`
 *      → 直接把 C 的 "invalid use of void expression" 漏给用户
 * 挪到定义处之后，无论从哪条路径使用它都是安全的。
 */
void checkOperatorSig(Checker *c, FuncDef *f) {
    bool isOp = (strcmp(f->name, "==") == 0 || strcmp(f->name, "!=") == 0);
    if (!isOp || !f->owner) return;

    const char *want = "signature must be `fn ==(self: ref T, other: T) -> bool`";

    /* 为什么必须是 bool —— 三个前提推出来的，不是拍的：
     *   ① `a == b` 天然会被用在 `if` / `while` / `&&` / `||` 里
     *   ② extC 没有隐式真值转换（`if 1` 是错的）
     *   ③ `!=` 是靠 `==` 取反实现的
     * 所以 `==` 只能是 `bool`，否则它就没法用在条件里。
     * 想要别的结果？换个方法名（`compare` / `diff` 之类），那些没有任何限制。 */
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

/* 这个类型能不能用 `op` 比较？
 * 签名合法性已经在**定义处**查过了，所以这里只要「找得到」就行。 */
bool typeSupportsEq(Type *t, const char *op) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (cmpIsNative(t)) return true;
    /* 数组的 `==` 由编译器派生 —— 条件是元素能比 */
    if (t->kind == TY_ARRAY) return typeSupportsEq(t->inner, op);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(b, op, strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
}

/* `?` 只在这三处合法，所以这三处走这个入口；别处遇到 EX_TRY 一律报错 */
Type *checkExpr(Checker *c, Expr *e);
Type *checkTryInner(Checker *c, Expr *e);

/* **值位置**：把 `ref T` 当 `T` 用（形状 3「值位置自动解引用」）。
 *
 * 跟 `checkExpr` 的分工：
 *   `checkValue` = 这里要的是**值** ⇒ `p` 就是 p 指向的东西，打 `deref` 标记
 *   `checkExpr`  = 这里要的是**地方/引用本身** ⇒ 赋值目标、`ref x` 的操作数、
 *                  字段/下标/切片的底、方法接收者（要取地址）
 *
 * 权限（能不能写）不在这里管 —— 那是 `ref` / `mut ref` 的事。 */
Type *checkValue(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    /* `ref x` 是**显式**要一个引用 ⇒ 不再自动解引用 ——
     * 否则就等于「解掉自己刚取的那个引用」，纯属自相矛盾。
     * 所以 `let r = ref n` 得到的是引用；而 `let y = r` 得到的是 r 指向的**值**。 */
    /* `ref x` 和 `alloc<T>(n)` 都是**显式**要一个引用 ⇒ 不再自动解引用 ——
     * 否则就是「解掉自己刚取/刚要来那个引用」，纯属自相矛盾。 */
    /* ⚠️ **不再有隐式解引用**（2026-09-20 主人拍板：`(ref i32) + 1` 必须非法）：
     * 值位置给了引用 ⇒ **报错**，教他写 `*p` ✓
     * 例外：`ref x` / `alloc`（本来就是"要一个引用"）、
     *       以及**成员选择**（`p.field` / `p.method()` —— 那是"导航"不是"取值"，仍然自动穿透 ✓）*/
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

/* 实参 / 字段初值：**期望类型是引用时不自动解引用** ——
 * 那里是「放一个引用进去」（`{ data: ref n, ... }`、`f(ref c)`），不是取它的值。
 * 其余情况按值位置处理（形状 3）。 */
void desugarBareCtor(Checker *c, Expr *e, Type *want);   /* 定义在后面 */

Type *checkInto(Checker *c, Type *want, Expr *e) {
    /* 期望类型是 `option<...>` / `result<...>` ⇒ 裸写构造器也认 ✓
     * （`f(some(3))`、`var x: ?i32 = some(3)` —— 类型从上下文来，不用写全名）*/
    if (want) desugarBareCtor(c, e, want);
    Type *got = checkExpr(c, e);
    /* 同样：**显式解引用** —— 期望的不是引用却给了引用 ⇒ 报错 ✓ */
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

/* **输出位置**：`println(r)` / `print(r)` 自动解引用 ✓
 * 理由（主人 2026-09-20）：「但你总不能真暴露地址了」——
 * 打印一个引用**只可能**是想看它指的值，而地址本身对用户毫无意义也不该暴露 ✓
 * ⇒ 这是唯一保留"自动解引用"的**值位置** ✓ */
Type *checkPrintArg(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    if (t && t->kind == TY_REF) {
        e->deref = true;
        return t->inner;
    }
    return t;
}

Type *checkMaybeTry(Checker *c, Expr *e) {
    if (e && e->kind == EX_TRY) return checkTryInner(c, e);
    return checkValue(c, e);
}

/* 泛型里的 `==` 推迟到实例化才检查 —— 这里记一笔 */

/* prelude 里两个「有真实语义」的容器，按名字 + 实参个数认（它们是**保留定义**，
 * 用户不能重定义，所以按名字认是安全的）。`?` 要用到它们的标签字段名 ——
 * 跟视图协议 `data` + `len` 是同一种分工：**语言认识协议，库提供结构**。 */
bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* 泛型 struct：`slice<T>`、`varArray<T>` */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* 泛型**枚举**：`option<T>` / `result<T,E>`（第三刀之后它们就是枚举，
     * 实例的类型是 TY_ENUM + 具体 targs ✓）*/
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/* `e?` —— 失败就顺着往上抛。
 * 只在三处语句位置上合法（C 没有语句表达式，得展开成语句），
 * 所以这个函数**只**从 checkStmt 的那三处调用；checkExprInner 遇到 EX_TRY 会报错。 */
Type *checkTryInner(Checker *c, Expr *e) {
    Type *ot = checkExpr(c, e->u.try_.operand);
    if (ttIsError(ot)) return ttError(c->tt);
    Type *ob = ttBase(ot);

    Type *rt = (c->curFunc && c->curFunc->ret) ? ttBase(c->curFunc->ret) : NULL;
    const char *fw = NULL;   /* 外层返回类型的写法，用来拼错误信息 */

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
        /* 错误类型必须一模一样 —— 否则「搬过去」就是在编一个不存在的转换 */
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

