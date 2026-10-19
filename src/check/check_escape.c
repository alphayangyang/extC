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

/* 一次提权走查的跳数上限。它只是**环的后备**：真正的环由 `Checker.fsChain` 按绑定精确挡住
 * （同一个绑定在一次走查里只走一次）。所以这个数要容得下合法的长链 —— 一个绑定要走
 * `EX_IDENT` + `promoteFieldsAt` + 它来源表达式的载体几步，32 对几十个绑定的程序就不够了；
 * 撞上限会走 `EXTC_DBG_FALLBACK`，断言版直接判红。 */
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
 *     analysis and is checked at the call site instead（由调用点那趟检查判定）。
 */

/**
 * @brief 取两个整数中较大者。
 */
static int maxInt(int a, int b) { return a > b ? a : b; }

static bool isGlobalSym(Checker *c, Sym *s);   /* defined below; used by storeLayer */

/* `EXTC_DBG_RHO` 的诊断基线（定义见本文件的 `targetDepthPure`）：只依赖词法块深度与来源链，
 * 不读电平求解器、不读缓存，因此可拿来对照而不改变自身答案。 */
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen);


/* `slotDepth` 问"存储有多深"，`targetDepth` 问引用指向哪里：两个问题不同，答案也不同。 */
/**
 * @brief 判断一个表达式的存储是否可证明不在任何帧里。
 *
 * 只有两个来源：根是全局的名字链（静态存储活过每一帧），以及声明带 `effects Ret=0` 的调用
 * （“我返回的引用不指向调用者的帧”）。其余一律答否，保持保守规则：`new`、arena 分配、栈上数组
 * 或没有这样声明的被调方都可能随帧消失。字符串字面量住在静态存储里，与全局同寿 ⇒ 深度 0。
 *
 * @param c  检查器。
 * @param e  绑定的初始化式或右值。
 * @return   存储确定活过本帧时返回 true，否则返回 false。
 *
 * Two sources, both authored rather than guessed: - a chain rooted at a **global** (static
 * storage outlives every frame), and - a call whose declaration carries `effects Ret=0` -- "the
 * reference/view I return does not point into the caller's frames".
 * (`Sym.outOfFrame`) so that a view may then answer with its pointee's depth.
 */
bool exprOutOfFrame(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* **字符串字面量住在静态存储里**：与全局同寿（C 的字符串字面量就是静态存储期）⇒ 深度 0。
     * 不能落到默认分支答 false：那会把 `let k: slice<u8> = "…"` 跨 `yield` 误判成"指向别人的栈"。 */
    case EX_STR:   return true;
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
        /* 声明签过字（`effects Ret=0`）的返回值：不指向调用者的帧。表字段上的槽带同样条款。 */
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

/**
 * @brief 算出地方表达式所指存储的深度。
 *
 * 局部给它的块深度，参数与全局给 0（本帧之外），引用型绑定给的是它指向的对象的深度而不是自己
 * 槽位的深度；`*p` 不创造存储，取的是引用本身的深度。根部无法命名的形状退回 `targetDepth`，
 * 使兜底落在更长寿的一侧，而不是把深度记成 0。
 *
 * @param c  检查器，提供作用域栈与类型表。
 * @param e  地方表达式。
 * @return   存储的词法深度；`e` 为 NULL 时返回 0。
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
            /* **视图**的存储不在本帧时，它表示那份存储而不是自己的槽：`var v = pl.view(off, n)!`
             * 的两字句柄指向板块内部，`v.data` 不能读成“指向本帧的指针”。`outOfFrame` 在绑定时
             * 由全局或 `effects Ret=0` 的调用置上；帧内存储上的视图、`new` 块或 arena 分配保留
             * 槽深度，这是保守规则要的。
             *
             * A **view** whose storage is out of this frame denotes that storage, not the slot
             * it sits in: `var v = pl.view(off, n)!` is a two-word handle whose bytes point
             * into the plate, so `v.data` must not be read as "a pointer into this frame".
             */
            if (st && ttIsViewType(st) && sy->outOfFrame) return anRefDepth(sy);
        }
        return sy ? sy->depth : 0;
    }
    /* A field or element lives inside the object that contains it. */
    if (e->kind == EX_FIELD) return slotDepth(c, e->u.field.obj);
    if (e->kind == EX_INDEX) return slotDepth(c, e->u.index.obj);
    Sym *root = placeRoot(c, e);
    /* `placeRoot` 命不了名的形状不是“活得最久的存储”——把它读成 0 恰是保守方向的反面：这种形状
     * 的存储随本帧消失，而 0 表示活过本帧，于是 `G = [v, v][0]` 会被放行。对无名形状要问的是其余
     * 检查问值的问题：它**指向**的东西有多深（`targetDepth`）。
     *
     * 这不会绕回这里：`targetDepth` 只对本身就是地方表达式的形状查 `slotDepth`，而本分支正是
     * `placeRoot` 已经答 NULL 的分支（INV-U：兜底记更长寿的要求，绝不记更短的那个）。
     *
     * A shape `placeRoot` cannot name is not "storage that lives forever" -- that reading
     * pointed the conservative direction exactly backwards.
     */
    return root ? root->depth : targetDepth(c, e);
}

/**
 * @brief 写目标经由**引用**到达时，可能被指物的最浅深度（min）；没有这条信息时返回 -1。
 *
 * `(*cur).f = v` 的目的地不是 `cur` 这个槽，而是 `cur` 可能指向的对象。`cur` 可能指向参数
 * （0）也可能指向帧内 `new`（1）—— 目的地要按**可能活得最久**的那一个算（min），因为存进去
 * 的值必须活到最坏的那个目的地那么久。DFA 给的是 max，拿它当目的地会漏收：帧内值被存进了
 * 调用者的对象，被调方一返回就悬垂。
 *
 * @param c  检查器。
 * @param e  写目标表达式。
 * @return   可能被指物的最浅深度；链上没有命名的引用步进时返回 -1（调用方沿用旧答案）。
 */
/**
 * @brief 写目标是否**经具名引用**到达（`(*cur).f` / `*cur`），是则返回那个绑定。
 *
 * 与 `destDerefDepth` 分开：形状判据（要不要推迟判定）与深度值（`minPointee`，可能还没记）是
 * 两件事 —— 词序问题恰恰出在"形状已经在、深度还没记"的那一刻。
 *
 * @param c  检查器。
 * @param e  写目标的位置表达式。
 * @return    路径尽头 `*` 后面是具名引用绑定时返回它；否则返回 NULL。
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
 * @brief 一个引用绑定「可能被指物的最浅层」：本绑定记的，外加声明处是拷贝时的别名链。
 *
 * `var ocur: ?mut ref node = cur` 里 `ocur` 与 `cur` **共享所指对象**，但 `ocur` 的深度快照
 * 取的是拷贝那一刻的词法值（第一个迭代里 `cur` 还是 `null` ⇒ 0），而 `cur` 的 `minPointee`
 * 要等 walk 走完才收全。所以走别名链取 min：只往更浅取，方向安全。
 *
 * @param c     检查器。
 * @param sy    引用绑定。
 * @param hops  别名链的跳数（防环）。
 * @return      最浅深度；一次都没记过时返回 -1。
 */
static int symMinPointee(Checker *c, Sym *sy, int hops) {
    if (!sy || hops > 16) return -1;
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
 * @brief 写目标经 `*` 到达时「可能被指物的最浅层」；不是这种形状时返回 -1（调用方退回 `slotDepth`）。
 *
 * @param c  检查器。
 * @param e  写目标的位置表达式。
 * @return    最浅深度；路径不经具名引用或一次都没记过时返回 -1。
 */
static int destDerefDepth(Checker *c, Expr *e) {
    Sym *sy = destDerefSym(c, e);
    return sy ? symMinPointee(c, sy, 0) : -1;
}

/**
 * @brief 求一个写目标所在存储的 arena 层。
 *
 * 它与 `slotDepth` 回答的不是同一个问题：这里问的是存储本身活多久，而不是引用指向的对象活多久。
 * `head = n` 把指针存进 `head` 这个槽，槽在本帧，所以 `n` 指向的东西至少要活到槽那么久；若问
 * `slotDepth(head)`，`= null` 的绑定会答 0，构建链表的寻常写法反而被拒。
 *
 * @param c  检查器。
 * @param e  写目标的位置表达式。
 * @return   存储所在的层：局部绑定是它所在块的深度，复制进本帧的参数是 1，全局是 0；
 *           投影自引用或参数的位置退回 `slotDepth` 的答案。
 *
 * a reference or parameter falls back to `slotDepth`, which reports the caller's
 * depth (0), so storing into a caller's object is still treated conservatively.
 */
int storeLayer(Checker *c, Expr *e) {
    /* 经由引用写的那条路（`(*cur).f = v`）问的是"被指物可能活多久"，按可能最浅的算：
     * `cur` 可能指向参数（0）也可能指向帧内 `new`（1）⇒ 目的地是 0，帧内值存进去就是悬垂
     * —— 判据的方向是**拒绝**，不是把那个值提进 home。 */
    int dd = destDerefDepth(c, e);
    if (dd >= 0) return dd;
    if (e && e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy) {
            /* 参数的槽在本帧（实参是一份拷贝），写它碰不到调用者的绑定 ⇒ 深度 1；全局是静态
             * 存储，活过每一帧 ⇒ 0。
             *
             * A parameter's slot is in this frame: the argument is a copy, so writing it cannot
             * touch the caller's binding.
             */
            if (sy->depth == 0 && !isGlobalSym(c, sy)) return 1;
            return sy->depth;
        }
    }
    return slotDepth(c, e);
}

/**
 * @brief `dyn` 值的寿命深度：句柄是普通值，载荷却住在**站点那一层地方**的 dyn 池里。
 *
 * 载荷被拷进池，池按站点所在的 place 存活 ⇒ 值至少要活到那一层。`ZONE_HOME`（-1）表示
 * "调用者的地方"，对深度来说就是 0。站点层还没定（0）时不加东西：调用点若真让它逃逸，
 * 提权会把层写上，深度那一趟会看到。
 *
 * @param e   `EX_DYN` 站点。
 * @param d   载荷本身给出的深度（值里引用的上界）。
 * @return    `d` 与站点层的较大者；深度越大活得越短，所以取较大者是保守方向。
 */
static int dynSiteDepth(Expr *e, int d) {
    int lvl = planZoneLevel(e);
    if (lvl == 0) return d;
    return maxInt(d, (lvl == ZONE_HOME) ? 0 : lvl);
}
/**
 * @brief 按语法形状求一个值里引用深度的上界，不看类型。
 *
 * `targetDepth` 在 `typeContainsRef` 说类型不带引用时提前返回，但“字段类型是引用”的结构体在那里
 * 答 false，于是 `inner { v: ref local }` 会被当成不带指针、给 0，活指针就此外逃。这里只看表达式
 * 形状，所以只会**抬高**估计；抬高是安全方向：可能误拒，不会误收。
 *
 * @param c  检查器。
 * @param e  要检查的表达式。
 * @return   值可能携带的最深引用深度的上界。
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
    /* `?`、转换原样传递操作数的引用，slice 是它索引对象的视图，运算符只能带操作数带的东西。
     * 这些若落到 `default: return 0`，深度就被**低估**，而深度决定存储要活多久 ⇒ 不安全。
     *
     * `?` and a conversion pass the operand's references through, a slice is a view over the
     * object it indexes, and an operator can only carry what its operands carry.
     */
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
        /* 认不出的形状答“结构里没有引用”是**不安全的那一侧**（它可能真的带着引用）⇒ 这是兜底，
         * 不是语义：被走到就说明分类漏了形状。`EX_INT` 之类的标量本来就答 0，是语义。
         * 做成断言会把“该查的信号”变成崩溃，所以只记一条调试提示。
         *
         * 认不出的形状答"结构里没有引用"是**不安全的那一侧**（它可能真的带着引用）⇒ 这是兜底，不是语义：被走到就说明分类漏了形状。
         * `EX_INT`（整数字面量）本来就不带引用，答 0 是语义；除此之外的 kind 走到这里， 计数就会变 —— 那才是要查的信号。做成断言会把"该查的信号"变成崩溃，
         * 而逐个列 kind 会撞手写遍历的预算，"列全"在这里买不到正确性。
         */
        EXTC_DBG_NOTEF("valDepthStructural: kind=%d answered 0", (int)e->kind);
        return 0;
    }
}

/**
 * @brief 判断某个类型的值是否可证明装不进活引用。
 *
 * 存储 `v` 有两种情形：`v` 是引用 ⇒ 拷进去的是指针，被指向者必须至少活到那个存储；`v` 是普通值
 * ⇒ 拷的是字节，来源对象活多久与存储无关，只有拷贝**内部**的引用要紧，那些由 `targetDepth` 覆盖。
 * 答 true 时来源对象的深度不构成约束，提权只会误拒。
 *
 * 不能只看用户写的类型节点：字段类型本身是引用的聚合类型在浅检查下答 false，所以整型必须递归看
 * 进去。答案只会**去掉**一条要求，因此只能把误拒变成接受，不会反过来。
 *
 * @param c  检查器（类型表）。
 * @param e  被存储的值。
 * @return   类型可证明不带引用时返回 true。
 *
 * When no reference can be inside the value, the source object's depth is not a constraint of
 * the store, and promoting it would only cause a false rejection.
 */
static bool typeCannotCarryRef(Checker *c, Expr *e) {
    if (!e) return true;
    if (mentionsParam(e->type)) return false;    /* `T` may carry a reference -> defer */
    return !typeContainsRef(c->tt, tsub(c, e->type));
}

/**
 * @brief 算出一次存储要记录的值的引用深度。
 *
 * 取引用深度与结构上界的较大者，保证携带活指针的值不会被低估。只有类型可能携带引用时才看结构
 * 上界，因此纯值拷贝不受来源对象寿命的约束，容器存字节时不会把分配点拖进调用者的 arena。
 *
 * @param c  检查器。
 * @param e  被存储的值。
 * @return   被存储的字节可能携带的引用深度的上界。
 *
 * Taking the structural bound here would require the `new` site to live as long as the caller's
 * frame, so every iteration would allocate into the caller's home arena and nothing would be
 * reclaimed until the frame ended.
 */
int valDepthForStore(Checker *c, Expr *e) {
    int a = targetDepth(c, e);
    if (typeCannotCarryRef(c, e)) return a;
    int b = valDepthStructural(c, e);
    return a > b ? a : b;
}

/**
 * @brief 求“建池的调用”这个值的深度（池的提权）。
 *
 * 与 `new` 同一条规则：新分配的东西活到它的站点那一层。容器构造就是一个站点，它建出来的池生在
 * 哪一层，容器就活到哪一层。不是建池的调用时原样返回 `d`，调用结果的深度仍是实参里最深的那个。
 *
 * @param c  检查器；本函数不用，保留是为了与同类函数形状一致。
 * @param e  调用表达式。
 * @param d  实参里最深的深度，也是非建池调用时的返回值。
 * @return   建池调用时取 `d` 与站点层的较大者；否则返回 `d`。
 *
 * 与 `new` 同一条规则：新分配的东西活到它的站点那一层。容器构造就是一个站点 —— 它建出来的池生在哪一层地方，容器就活到哪一层。 - `ZONE_HOME` ⇒
 * 0（"活得比本帧还久"）：库函数里的构造交给调用者选的地方； - `k >= 1`    ⇒ k：生在第 k 层，就活到第 k 层。 不是建池的调用（`makesPool` 为假）⇒
 * 原样返回：调用结果的深度仍是"实参里最深的那个"。
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
 * @brief 写/读字段槽用的**路径键**：从根绑定到这次投影的整条字段链。
 *
 * 只取最后一段字段名（`next`）会让 `bx.mid.next` 与 `bx.next` 撞进同一格，而两层投影的 `next`
 * 根本找不到槽（声明只建了 `mid`）⇒ 来源记不下、`promoteFieldsAt` 提不动那个站点、返回值检查
 * 看不到它，分配于是留在函数自己的帧 arena，返回即悬垂。用整条路径当键，读与写才对得上同一格。
 *
 * 元素下标是编译期常量时写进名字（`[0]`），否则记 `[*]`：同一根上的不同常量下标各占一格，
 * 动态下标共享一格 —— 后者粒度粗，只影响提权精度，不漏来源。
 *
 * @param c  检查器；键字符串建在它的 arena 里，活得比字段表久。
 * @param e  投影表达式（`EX_FIELD` / `EX_INDEX` 链）。
 * @return   路径键；走不到根、或路径里有解引用/切片等命不了名的步进时返回 NULL。
 */
const char *fieldSlotKey(Checker *c, Expr *e) {
    if (!e) return NULL;
    if (e->kind == EX_IDENT) return "";
    /* `(*a).next = v` 的槽挂在**绑定 a** 上（根由 `destDerefSym` 给出），键只记解引用之后的
     * 路径：环/链的提权要从这里走到被存进去的那个对象自己的字段表。 */
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

/**
 * @brief 算出值所携带的引用指向的存储有多深。
 *
 * 数字越小活得越久，0 表示参数或全局，也就是本帧之外。这个数是引用规则拿来与目标存储深度比较
 * 的量，所以每个近似都必须偏大：偏大只会误拒，偏小就会放过悬垂引用。
 *
 * @param c  检查器。
 * @param e  要检查的表达式。
 * @return   值里引用的深度；类型不可能携带引用时返回 0。
 */
int targetDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* 提前返回的唯一理由：这个类型不可能携带引用。提及类型参数的类型不能走这个出口 —— 泛型体里
     * 深度按参数可能带引用算，由实例检查决定规则是否适用。`EX_DYN` 也不能走：`dyn T` 自身不带引用，
     * 但负载被**拷进池**，值会连带引用负载所指的东西（the payload's 引用也要算）。 */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return 0;
    /* 绑定的深度以绑定为准，不以表达式节点为准：节点上的副本在表达式首次检查时写入，而绑定深度
     * 由数据流分析在整个控制流上随后定下。这里读节点会拿到不动点之前的答案，从而把持有分配的结构
     * 体报成不持有活指针。
     *
     * A binding's depth is authoritative on the binding, not on the expression node: the node's
     * copy is written when the expression is first checked, and the data-flow analysis settles
     * binding depths only afterwards, from the whole control-flow structure.
     */
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
    /* `?`、转换原样传递操作数的引用，slice 是它索引对象的视图，运算符只能带操作数带的东西。
     * 这些若落到 `default: return 0`，深度就被**低估**，而深度决定存储要活多久 ⇒ 不安全。
     *
     * `?` and a conversion pass the operand's references through, a slice is a view over the
     * object it indexes, and an operator can only carry what its operands carry.
     */
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
        /* 持有引用的聚合绑定（结构体、数组、泛型实例）要问那些引用指向哪里，而不是自己的槽有多
         * 深：`return h`（`h` 里存着参数 `p` 的引用）按槽深度 1 回答会拒掉合法的程序。槽深度仍是
         * “存进这个绑定”的正确答案（`slotDepth` 算的那个），两个问题分开。 */
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && typeContainsRef(c->tt, tsub(c, sy->type))) d = anRefDepth(sy);
        else d = slotDepth(c, e);
        /* 不能止于上面的 `typeContainsRef`：字段类型本身是引用时它答 false，于是
         * `struct holder { p: ?ref i32 }` 的绑定会在没查字段表的情况下报 0，所以无论类型怎么说
         * 都要再取一遍逐字段上界。 */
        if (sy) {
            for (int fi = 0; fi < sy->nfields; fi++)
                if (sy->fields[fi].depth > d) d = sy->fields[fi].depth;
            if (sy->otherDepth > d) d = sy->otherDepth;
        }
        break;
    }
    case EX_FIELD: case EX_INDEX:
        /* 读字段自己记录的深度。改报外层槽的深度会误拒 `h.p`，而之后 `h.p = null` 也补不回来，
         * 因为两个答案说的是不同的东西。字段没有条目时（刚声明、或名字对不上）退回保守的地方深度。
         *
         * 元素（`arr[0]`）与字段同一格：`arr[0] = p` 记在 `arr` 的 `[0]` 槽里，`return arr[0]`
         * 要读的正是那一格（提权会把它降到要求的那层）。只在 `EX_FIELD` 上查会让元素退回
         * `slotDepth`（数组槽深度 1），帧内 `new` 的返回因此被误拒。 */
        {
            Sym *rf = placeRoot(c, e);
            /* `(*a).next` 命不了名（`placeRoot` 走不过解引用），但槽就挂在 `a` 上：环/链
             * （`(*a).next = b`）要靠它把要求传给 `b` 指向的对象。 */
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
        /* 调用结果的深度是实参深度的最大值。为什么这是可靠上界：被调方能返回的引用只来自静态存储
         * （0）或它的实参（<= max(实参)），返回自己局部的引用会被它自己的返回检查拒掉，没有第三个
         * 来源。取最大值才不会漏掉“原样返回某个引用实参”的被调方。 */
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
        /*
         * 枚举负载构造（如 `shape.holding(a[..])`）把负载放进这个值，与数组字面量同理，深度取负载深度。
         *
         * A payload construction such as `shape.holding(a[..])` puts the payload inside this
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
    /* `EXTC_DBG_RHO` 的对照：基线只依赖词法块深度与来源链，不读求解器、不读缓存。基线更大即检查
     * 器低估了值的寿命，正是产生悬垂分配的方向。 */
    if (dbgOn("EXTC_DBG_RHO")) {
        Expr *seen[40];
        int pure = targetDepthPure(c, e, 0, seen);
        if (pure != d)
            fprintf(stderr, "[rho] %s:%d kind=%d cached=%d pure=%d %s\n",
                    c->ctx && c->ctx->path ? c->ctx->path : "?", e->line, (int)e->kind, d, pure,
                    pure > d ? "UNDER" : "OVER");
    }
    /* 提及类型参数的节点不缓存：那个数只在泛型体成立，实例上可能不成立。 */
    if (!c->substParams && !mentionsParam(e->type)) anSetRefDepth(e, d);
    return d;
}


/**
 * @brief 判断一个符号是不是模块级全局。
 *
 * 全局与参数深度都是 0，但只有参数是借来的，逃逸规则必须把两者分开。
 *
 * @param c  检查器；在它的全局表里查。
 * @param s  要查的符号。
 * @return   `s` 声明在模块级时返回 true。
 */
static bool isGlobalSym(Checker *c, Sym *s) {
    for (size_t i = 0; i < c->globals.len; i++)
        if (*(Sym **)vecAt(&c->globals, i) == s) return true;
    return false;
}

/**
 * @brief 判断一个地方指的是不是别人拥有的存储。
 *
 * 与 `exprBorrowed` 不同，这里不看值的类型（`*p` 的值可能完全不带引用），只问存储归谁。重新造出
 * 引用时（如 `ref *p`）用它：要问的是结果是否仍指向调用者的帧。
 *
 * @param c  检查器。
 * @param e  地方表达式。
 * @return   存储经解引用到达，或属于参数而不是本帧时返回 true。
 *
 * It is used when a reference is re-created, as in `ref *p`, where the question is whether the
 * result still points into the caller's frame.
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
 * @brief 判断一个值是否持有从本帧之外借来的引用。
 *
 * 追到参数或调用结果的引用是借来的：真正的寿命由调用者定，所以被调方可以往下传，但不能存到活过
 * 本次调用的地方。指向全局的引用不算借：静态存储活过每一帧。
 *
 * 类型级出口是这里唯一允许的提前返回：类型提及类型参数的节点必须走下去，泛型体记下的延迟检查会
 * 在实例化时消费这个答案。
 *
 * @param c  检查器。
 * @param e  要检查的表达式。
 * @return   值就是、或可能是借来的时返回 true。
 */
static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    /* 不用看就能答的唯一理由：这个类型不可能携带引用。提及类型参数的节点不能走这个出口，泛型体的
     * 延迟检查需要这个答案；`EX_DYN` 也不能走，负载被拷进池 ⇒ 值借了负载借的东西。 */
    if (e->kind != EX_DYN && !typeContainsRef(c->tt, tsub(c, e->type)) &&
        !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_DYN: return exprBorrowed(c, e->u.dynv.payload);
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* 参数深度 0 且不是全局 ⇒ 借来的。全局/静态也是 0，但谁都能存。 */
        if (root && root->depth == 0 && !isGlobalSym(c, root)) return true;
        /* 从借来的值**初始化**出来的局部带着那份借用：'q' 有参数的寿命，于是把它写进全局时仍是
         * 在存一个借用（the caller's 那份寿命），尽管 q 本身是本帧的局部。只问绑定自己的词法深度
         * 会答 false 而放行。 */
        Sym *sy = (e->kind == EX_IDENT) ? identBindOf(e) : root;
        if (sy && sy->depth > 0 && sy->origin && sy->origin->kind == EX_IDENT)
            return exprBorrowed(c, sy->origin);
        /* 只追**普通绑定**，不追 `EX_FIELD`：追字段来源会让 `self.top = t.next`（从 `self` 指向的
         * 对象里读引用再写回同一对象）报“不能把借来的值存到活过本次调用的地方”，而两侧都在同一个
         * `mut ref` 后面，并没有延长寿命。
         *
         * Only a **plain binding** is followed, and the narrowness is load-bearing: following
         * `EX_FIELD` origins as well made `self.top = t.next` (read a reference out of the
         * object `self` points at, write it back into that same object) report "cannot store a
         * borrowed value into something that outlives this call".
         */
        return false;
    }
    case EX_SIGN:
        /* 可空后缀改变的是类型承诺，不是值的来处 ⇒ 借用与否照传。 */
        return exprBorrowed(c, e->u.sign.operand);
    case EX_COALESCE:
        /* 任一侧都可能成为结果，所以任一侧借来 ⇒ 整个表达式借来。 */
        return exprBorrowed(c, e->u.coalesce.main) ||
               exprBorrowed(c, e->u.coalesce.fallback);
    case EX_REF:
        /* `ref *p` 重新造出引用，会掩盖它来自参数：`*p` 表示 `p` 指向的存储，所以问那份存储是否
         * 借来的。这里不能递归 `exprBorrowed`：`*p` 的值可能是 `i32`，上面那个类型出口会把这种
         * 情况丢掉。 */
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
        /* 负载构造：负载借来则构造出的值也借来，与数组字面量同理。 */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    default: return false;      /* literals, globals, and fresh allocations belong to this frame */
    }
}

/**
 * @brief 判断一个值的根是不是来自某个函数的参数。
 *
 * 是的话，生命周期义务就交给调用点，由它知道对应实参活多久。`placeRootName` 解出地方的根：
 * `v` 仍是 `v`，`*p` 变成 `p`，`l.head` 变成 `l`。
 *
 * @param c    检查器；本判断用不到，保留是为了与同类谓词形状一致。
 * @param f    提供参数名单的函数定义。
 * @param val  要追溯的值。
 * @return     值的根是 `f` 的参数名时返回 true；值为 NULL、函数为 NULL 或根无法命名时返回 false。
 *
 * Whether the origin of a value can be traced back to one of `f`'s parameters.
 */
bool valTracesToParam(Checker *c, FuncDef *f, Expr *val) {
    /* 本判断只用得到"函数与值"两样；参数保留是为了与同类谓词形状一致。 */
    (void)c;
    if (!val || !f) return false;
    const char *root = placeRootName(val);
    if (!root) return false;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, root) == 0) return true;
    return false;
}

/* 前向声明：值存入某个地方前的延迟引用检查，定义在下方。 */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what);   /* defined below */

/*
 * 前向声明：载体走查与提权，定义在下方 `promoteInto2`。
 *
 * The most ordinary code -- building a list inside a loop -- allocates its nodes in the loop
 * body while `head` lives outside the loop, so the next iteration's block exit frees them and
 * leaves a dangling pointer (confirmed with ASan).
 * at   - depth the value must reach; 0 = must outlive this frame (the caller's arena)
 */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops);
/* `dyn Trait(x)` carries its payload with it, so the payload's allocation sites have to reach the
 * level this value has to reach -- exactly what the rest of this function does for a struct. */
bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops);
/**
 * @brief 把值内部的每个分配站点提升到它必须达到的层。
 *
 * 只沿值的载体走（绑定、`!`、`??`、结构体字面量、枚举负载、数组字面量），不追别名、不做跨函数
 * 推理，最多 32 步。提升只把生命周期拉长，不会让不安全的存储变成接受。这是存储与返回检查用的
 * 入口，从零步开始走。
 *
 * @param c    检查器。
 * @param val  被存储或返回的值。
 * @param at   值必须达到的层；0 表示要活过本帧（调用者的 arena）。
 * @return     值里每个分配站点都到达 `at` 时返回 true；有载体提不动时返回 false，调用方据此退回
 *             深度判据报错。
 */
bool promoteInto(Checker *c, Expr *val, int at) { return promoteInto2(c, val, at, 0); }

/**
 * @brief 把绑定字段表里可达的分配点提升到需要活到的层。
 *
 * `Sym.origin` 只记声明处的初始化式，所以 `var h: box = { p: null, q: null }` 之后经 `h.q = x`
 * 到达的站点靠 origin 永远走不到，会停在浅层，容器一死值就悬垂。这里只做提升那一半，`origin`
 * 之外还要读字段表的来源。
 *
 * 纪律：只有带 `src` 的槽被提权；要提到的是目的地的 `at`（字段表自己的数说不了它现在装什么，
 * 不参与）；该槽的记账只在提权成功后才允许降到 `at`，提不动的槽保留原记账，结果是误拒而不是漏洞；
 * 没有 `src` 的槽完全不动。
 *
 * @param c     检查器。
 * @param sy    要走的字段表所属绑定。
 * @param at    容器被存到的深度；0 表示必须活过本帧。
 * @param hops  已经走过的载体步数。
 * @return      每个带来源的槽都提升成功时返回 true；有提升不动的槽时返回 false。
 *
 * Promote the allocation sites reachable through a binding's field table.
 * - The level to reach is the destination's `at`: storing the whole container at level `at`
 * means the references inside it only have to live that long, so the field table's own number
 * (which says what the field holds right now) is not used.
 * - That entry's accounting may only drop to `at` after the promotion succeeded.
 */
bool promoteFieldsAt(Checker *c, Sym *sy, int at, int hops) {
    if (!sy) return true;
    if (hops > EXTC_PROMOTE_MAX_HOPS) { EXTC_DBG_FALLBACK("promoteFieldsAt: recursion budget exhausted"); return true; }
    /* 环守卫：同一个绑定在一次走查里只走一次。来源表可以成环（`a = b  b = a`，或两张字段表
     * 互指），按来源链走会无限递归；原来只靠 32 步上限兜底，撞上限就触发
     * `EXTC_DBG_FALLBACK`（断言版判红），还把预算白白烧在一个环上。要求已经正沿那条路施加，
     * 重复走没有新信息。 */
    for (int i = 0; i < c->fsDepth; i++)
        if (c->fsChain[i] == sy) return true;
    if (c->fsDepth >= (int)(sizeof c->fsChain / sizeof c->fsChain[0])) {
        EXTC_DBG_FALLBACK("promoteFieldsAt: field-source chain too deep");
        return true;
    }
    c->fsChain[c->fsDepth++] = sy;
    /* 取过地址的根有两个方向相反的问题：降记账在别名可能写入时不安全（别名能放进更深的值），而提升
     * 站点背后的来源总是安全的（只把分配挪到更长命的 arena）。所以提升照做，只跳过记账下降。 */
    bool lower = !sy->addressed;
    bool ok = true;
    for (int i = 0; i < sy->nfields; i++) {
        Expr *src = sy->fields[i].src;
        if (!src) continue;
        /* 要提到的是 min(该槽记录的深度, 它曾被要求达到的最浅层)，单看这一次存储的目的地不够：
         * `out = h` 是层 1，但之后的 `return out` 要求值活过本帧（0），站点最终必须进 home arena。
         * 只提到 1 看似够，但层 1 随函数返回释放，会留下 use-after-free。 */
        if (at < sy->fields[i].minReq) sy->fields[i].minReq = at;
        int want = sy->fields[i].depth;
        if (sy->fields[i].minReq < want) want = sy->fields[i].minReq;
        if (!promoteInto2(c, src, want, hops + 1)) { ok = false; continue; }
        /* 来源那个绑定自己的字段表也要走：`a.next = b  b.next = ca` 的环/链要一路提到 `want`。
         * 展平后的 `src` 是站点表达式，没有字段表；`srcSym` 是同一个对象的绑定，出作用域也能用。 */
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
    /* 声明处的**别名来源**（`var head = a`）：head 与 a 共享所指对象，a 后来被改指
     * （`a = p`）时，要求要沿着 a **当前**的来源传下去。这一格只服务提权，不参与深度记账。 */
    if (sy->initSrc) {
        if (at < sy->initMinReq) sy->initMinReq = at;
        int want = sy->initSrcDepth;
        if (sy->initMinReq < want) want = sy->initMinReq;
        if (!promoteInto2(c, sy->initSrc, want, hops + 1)) ok = false;
    }
    /* 被调方经 `mut ref` 实参做的写同理：这里不降任何记账，要挪的是来源背后的站点，因为
     * `return b` 需要 home arena。 */
    if (sy->callSrc) {
        if (at < sy->callMinReq) sy->callMinReq = at;
        int want = sy->callSrcDepth;
        if (sy->callMinReq < want) want = sy->callMinReq;
        if (!promoteInto2(c, sy->callSrc, want, hops + 1)) ok = false;
    }
    c->fsDepth--;
    return ok;
}

/* `recordLvlFact` 记的是"这个值必须装进某一层"这条事实，`at == 0` 一档也要记。 */
/**
 * @brief 记录一次发布：值被放到层 `at` 上，本身不做判定。
 *
 * 检查这一趟只记录，之后由一趟折叠结合深度不动点统一求解层号，因此没有判定依赖遍历顺序。
 * 记录挂在 `c->stores` 上，并带上所在函数体，供按体折叠使用。
 *
 * @param c       检查器；记录追加到它的发布表。
 * @param val     被发布的值。
 * @param target  目的地；NULL 表示返回值发布。
 * @param at      目标层；0 表示活过本帧。
 * @param line    源码行号，用于诊断。
 *
 * This is the checking pass's whole involvement with arena levels.
 */
void recordStore(Checker *c, Expr *val, Expr *target, int at, int line) {
    if (!c || !val || at < 0) return;
    if (anStoredAt(val) >= 0 && anStoredAt(val) <= at) {
        /* 值已按不更深的层发布过，它自己再记一条不会增加寿命要求 —— 但只对同一个目的地成立。
         * 目的地是独立事实，值是由哪些绑定建出来这条要求挂在它身上：`if` 两个分支里 `h = g` 与
         * `h = zero` 是两个不同的目的地，决定绑定层号要覆盖哪个的是目的地而不是值，跳过第二条
         * 记录会丢掉那个分支的要求。
         *
         * The destination is a separate fact, and it is the one that carries a requirement back
         * to the bindings the value was built from: `h = g` in one arm of an `if` and `h =
         * zero` in the other are two destinations for two different values, and it is the
         * destination, not the value, that decides which of them the binding's level has to
         * cover.
         */
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
 * @brief 一次写的统一入口：发布、提权、逃逸标记与字段表按固定顺序完成。
 *
 * 顺序不能变：`recordStore` 要在 `promoteInto` 之前，求解器先看到要求；字段表要在提权之后，因为
 * 字段表的提升会读它。整值写走 `noteWholeValueDepthWrite`，字段或元素写走 `noteFieldDepthWrite`；
 * 不涉及引用的写直接返回。
 *
 * @param c       检查器。
 * @param value   被写入的值。
 * @param target  写入的目标位置；没有目标时只做发布与提权。
 * @param at      目标所在的 arena 层；0 表示要活过本帧。
 * @param line    源码行，用于诊断。
 */
void noteStore(Checker *c, Expr *value, Expr *target, int at, int line) {
    if (!c || !value) return;
    recordStore(c, value, target, at, line);          /* ① 电平记录 */
    promoteInto(c, value, at);                        /* ② 提权（提不动就保守） */
    markCallHomeIfEscaping(c, value, at);             /* ③ 逃逸集合那一半 */
    /* ④ 字段表 / 整值表 */
    Sym *vs = target ? placeRoot(c, target) : NULL;
    /* `(*a).next = b` 命不了名（`placeRoot` 走不过解引用），但槽挂在 `a` 上：不记的话，
     * `return a` 提权时就走不到 `b` 指向的对象，环/链留在块层 ⇒ 返回即悬垂。 */
    if (!vs && target) vs = destDerefSym(c, target);
    if (!vs) return;
    int d2 = valDepthForStore(c, value);
    if (!(d2 > 0 || (vs->type && typeContainsRef(c->tt, vs->type)))) return;
    if (target->kind == EX_IDENT) {
        /* 写点失效的**检查期**那一半：一个整值写发生时，关于这个绑定的"非空证明"必须已经"作废"。
         * 断言把它变成被看守的不变量 —— 一旦某个写路径漏了 `unNarrow`，调试构建当场响。 */
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
 * @brief 按层 0 发布一个返回值。
 *
 * 目标就是值本身：层 0" 表示活过本帧，交回给调用方的"东西"一律按此发布。
 *
 * @param c      检查器。
 * @param value  被返回的值。
 * @param line   源码行，用于诊断。
 */
void noteReturnPublish(Checker *c, Expr *value, int line) {
    if (!c || !value) return;
    recordStore(c, value, value, 0, line);
}

/**
 * @brief 记录一次与层号有关的拒绝，供随后复核。
 *
 * 这里不报错，只把这次判定用到的值、层号与看到的深度留下来。
 *
 * @param c      检查器；记录追加到它的拒绝表。
 * @param val    被拒绝的值。
 * @param at     判定时用的深度。
 * @param depth  检查当时看到的深度。
 * @param line   源码行号。
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
 * @brief 用各趟求解后的数字重新核对记录下来的拒绝。
 *
 * 只更新记录里的后算深度并可选打印一行，不改变已经报出的诊断：报告的落点与这里的复核分开，否则
 * 会丢掉拒绝，因为检查时的读者与这里的读者并不总看到同一个数。
 *
 * @param c  检查器。
 *
 * Keeping the two apart is what makes the question answerable: reporting from here alone would
 * lose rejections, because the check's reader and this one do not always see the same number.
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

/* 前向声明：值的根来源，定义在下方 `originOf`。 */
Expr *originOf(Checker *c, Expr *val, int hops);

/**
 * @brief 记下一个绑定值的来源，并把载体链展平到根。
 *
 * 来源必须展平：链中间的绑定可能在后续再被追溯时已经出了作用域，`lookup` 查不到，链就断了。
 * 已经取过地址的绑定不记。
 *
 * @param c   检查器。
 * @param sy  被赋值或初始化的绑定。
 * @param val 要记来源的表达式。
 */
void noteOrigin(Checker *c, Sym *sy, Expr *val) {
    if (!sy || sy->addressed) return;
    /* 字面量不得覆盖一条已经能追的来源。检查按源码顺序跑：`p = x` 这一支先记下分配站点，若后面
     * 的 `p = null` 覆盖掉它，之后的 `out = p` 就追不到站点，站点停在块层，值悬垂。
     * 反过来也要紧：`= null` 初始化的绑定必须记下这个 null，否则 `origin` 一直是 NULL，
     * `promoteInto2` 会在“没有来源”处提前退出而不走分支。
     *
     * The other direction matters as well: a binding initialized as `= null` must record that
     * null, because otherwise `origin` stays NULL and `promoteInto2` exits early on "no origin"
     * instead of walking the branches.
     */
    bool literal = val && (val->kind == EX_NULL || val->kind == EX_INT || val->kind == EX_FLOAT ||
                           val->kind == EX_BOOL || val->kind == EX_STR);
    bool haveReal = sy->origin && sy->origin->kind != EX_NULL && sy->origin->kind != EX_INT &&
                    sy->origin->kind != EX_FLOAT && sy->origin->kind != EX_BOOL &&
                    sy->origin->kind != EX_STR;
    if (literal && haveReal) return;
    sy->origin = originOf(c, val, 0);
}

/**
 * @brief 沿值的载体走查，把里面的分配站点提到值必须达到的层。
 *
 * `new` 分配进当前块的 arena，块结束就释放，所以存到活过本块的地方的值必须挪到至少一样长寿的
 * arena。规则是 `arenaLevel = min(当前块深度, 每个目的地的深度)`：越小的深度寿命越长，最小值跟随
 * 活得最久的目的地；只存进更深的地方不改变最小值，行为不变。提升总是安全方向 —— 它把池拉长，
 * 更长的寿命不会留下悬垂引用。
 *
 * 只沿值的载体走，不追别名、不做跨函数推理；提不动的载体退回深度检查，所以本函数不会放行本该
 * 拒绝的东西。父节点上缓存的深度只有每个子节点都成功时才允许收紧，否则会记下比实际更长的寿命，
 * 那是漏洞而不是误拒。本函数也是电平事实的唯一入口，提前返回的分支仍须走够远把事实记下。
 *
 * @param c    检查器。
 * @param val  被存储或返回的值。
 * @param at   值必须达到的层；0 表示要活过本帧（调用者的 arena）。
 * @param hops 已走过的载体步数；超过 32 步放弃（绑定可以成环）。
 * @return     值里每个 `new` 都到达 `at` 时返回 true；有载体提不动时返回 false。
 *
 * at   - depth the value must reach; 0 = must outlive this frame (the caller's arena)
 */
static bool promoteInto2(Checker *c, Expr *val, int at, int hops) {
    if (!val) return true;
    /* 无法描述的层：什么都不动。 */
    if (at < 0) return false;
    /* 步数上限是必需的：绑定可以成环（`a = b  b = a`，因为赋值会更新来源），没有上限会死循环。
     * 撞上限即值提不动，调用方退回报错 —— 安全方向。 */
    if (hops > EXTC_PROMOTE_MAX_HOPS) { EXTC_DBG_NOTE("promoteInto2: recursion budget exhausted"); return false; }
    /*
     * `dyn Trait(x)`：负载被拷进池、由句柄保活，所以负载里的分配站点要到达本值要到达的层。
     * 句柄的池也生在站点那一层：值要活到 `at`，池就必须生在活到 `at` 的那个地方，所以站点层
     * 也要往长寿一侧提（与建池的调用同一条规则：`want = at == 0 ? ZONE_HOME : at`）。池生在更
     * 深的一层而句柄存到外面，就是悬垂 —— 这一提正是让 `let d: dyn T = dyn T(x)` 出块仍然有效。
     *
     * `dyn Trait(x)`: the payload is copied into the pool and the handle keeps it alive, so the
     * payload's allocation sites have to reach the level this value has to reach. The pool itself is
     * born in the place the site names, so that level has to reach `at` as well -- the same rule as
     * the pool-building call below.
     */
    if (val->kind == EX_DYN) {
        /* 池的层先提，再递归负载：负载可能提到一半就答"提不动"（结构体字面量里一个转换运算
         * 就足以），而池的层必须独立于那个答案 —— 站点层若留在块层，深度判定会拿一个比真实
         * 寿命更短的层去比，`return dyn T(x)` 会被误拒。
         *
         * Lift the pool's level first, then recurse into the payload: the payload walk can answer
         * "cannot lift" halfway (one conversion inside a struct literal is enough), and the pool's
         * level must not depend on that answer. */
        if (planZoneLevel(val) != 0) {
            int want = (at == 0) ? ZONE_HOME : at;
            if (planZoneLevel(val) > want) planSetZoneLevel(val, want);
        }
        return promoteInto2(c, val->u.dynv.payload, at, hops + 1);
    }
    /* 顺带记一条事实，不改变任何行为。只在最外层（`hops == 0`）记：重放跑的正是这一层，内层由
     * 本次调用自己到达，重复记只是重复工作。 */
    if (hops == 0) {
        if (dbgOn("EXTC_DBG_FACT"))
            fprintf(stderr, "[fact] %-8s at=%d kind=%d minAt=%d lexi=%d line=%d\n",
                    c->curFunc?c->curFunc->name:"?", at, (int)val->kind,
                    anMinAt(val), anLexicalLevel(val), val->line);
        recordLvlFact(c, val, at); applyLvlFact(c, val, at);
    }
    switch (val->kind) {

    /* `EX_GENCALL` 是 `alloc<T>(n)`，与 `new` 完全对称：同一套层规则、同一张站点表，提权规则也
     * 对称，所以两种情况由这一格一起处理。 */
    case EX_NEW:
    case EX_GENCALL: {
        /* 目的地深度 0 表示“调用者那一层”，即 home arena。只有带 home 的函数能到达；没有就提不动，
         * 答 false 让调用方报错 —— 安全方向。该档用 `ARENA_HOME` 表示而不是 0，因为当前编码里 0 是
         * “还没定”，写回前要翻译。
         *
         * A destination depth of 0 means "the caller's level", the home arena.
         * That tier is represented by `ARENA_HOME` rather than by 0, because in the current
         * encoding 0 means "not yet decided", so the value has to be translated before it is
         * written back.
         */
        if (at == 0 && !(c->curFunc && anNeedsHome(c->curFunc))) return false;
        /* 走到这里说明逃逸分析是沿值的载体碰到这个站点的，而不是“所在函数有 home”这个兜底。这样
         * 碰到的站点是**真的**被要求活过本帧，所以标记它。必须在下面的提前返回之前做。
         *
         * Reaching this point means the escape analysis has met this site by walking the
         * carriers of a value, rather than the fallback of "the enclosing function has a home".
         */
        /* 记下逃逸分析对这个值提出的最强要求：层越小要求越强，所以取最小值。一个布尔不够：被看见
         * 不等于被要求活过本帧 —— `nb = new item[cap*2]` 只需活到本帧结束，当成“去 home”会让没有
         * home 的 `main` 也发出 `__extc_home`。
         *
         * `nb = new item[cap*2]` only has to live until the end of the current frame, so
         * treating it as "goes home" would make `main`, which has no home, emit `__extc_home`.
         */
        if (anMinAt(val) < 0 || at < anMinAt(val)) anSetMinAt(val, at);
        /* 已在 home arena：home 活得最久，没什么可提的。 */
        if (planArenaLevel(val) == ARENA_HOME) return true;
        int target = (at == 0) ? ARENA_HOME : at;
        if (planArenaLevel(val) > target) planSetArenaLevel(val, target);
        /* 全检查器里唯一一处把层号换算回深度的地方。 */
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
         * 沿绑定的来源往回走：`var n = new node` 与 `mid = n` 都通向那个 `new`。
         *
         * Walk back along the binding's origin: `var n = new node` or `mid = n` leads to that
         * `new`.
         */
        Sym *sy = lookup(c, val->u.ident.name);
        if (sy && !sy->origin && sy->poolSite) {
            /* 这个绑定由一次调用初始化（`var inner = vector<i32>::new()`），而 `origin` 对调用是空
             * 的（它只认分配站点的形状）。池不同：层级由调用点决定，所以直接顺着池站点往下走，
             * 下一跳是调用那一格，那里才知道“建池的调用可以提权”。
             *
             * 这个绑定由一次调用初始化（`var inner = vector<i32>::new()`），而 `origin`
             * 对调用是空的（它只认分配站点的形状）。池不一样：层级就是调用点决定的， 所以直接顺着那格池站点往下走 —— 下一跳是
             * `EX_CALL/EX_METHOD/EX_ASSOC` 那一格， 那里才知道"建池的调用可以提权"。
             */
            if (getenv("EXTC_DBG_ZONE"))
                fprintf(stderr, "[promote] EX_IDENT %s -> poolSite %s\n",
                        val->u.ident.name ? val->u.ident.name : "-",
                        sy->poolSite->kind == EX_CALL ? "EX_CALL" :
                        sy->poolSite->kind == EX_METHOD ? "EX_METHOD" : "EX_ASSOC");
            return promoteInto2(c, sy->poolSite, at, hops + 1);
        }
        if (!sy) return false;
        if (!sy->origin) {
            /* 没有初始化式的绑定（`var bx: outer`、`var arr: [2]?mut ref node`）：来源只可能在
             * 字段表里 —— `bx.mid.next = p` 这类写下的站点没有别的地方能到（`origin` 是 NULL）。
             * 在这里提前返回会把整张字段表跳过，站点于是停在词法层、随函数返回释放。所以仍然走
             * 一遍 `promoteFieldsAt`：它的任务就是把要求传给真正的分配站点。判定本身不改（返回
             * false，交由调用方的深度比较），只是事实已经记下了。
             *
             * 一个没有初始化式的绑定，其写入已经按字段记进表里；表里没有的来源（解引用等命不了名
             * 的写）本来就走不到这里，判据不会因此放宽。 */
            promoteFieldsAt(c, sy, at, hops + 1);
            return false;
        }
        /* 槽已经活过 `at`，说明它在更浅的层，里面的东西也已经活到那一层：初始化式在同一条语句里
         * 求值，层号就是槽的深度（the slot's 深度）。
         *
         * 调用结果是例外：带 home 的被调方分配进哪个 arena 由调用点定，不必是 the slot's 层，所以那个
         * 深度在绑定初始化时记进 `Sym.refDepth`，那份记录才作准。 */
        /* 本分支**不能**提前返回。"槽活得够久所以不用提权"对提权本身成立，但 `promoteInto` 也是
         * 电平事实的唯一入口，提前返回会让站点一条约束都没有，末轮会把它放回词法层，仿佛没人碰过它。
         * 所以走查总要往下走，只为记事实；槽够深时返回值仍答"没什么要改"（下面的 `slotDeepEnough`）。 */
        bool slotDeepEnough = (sy->depth <= at);
        /* 来源可以**自指**（`c = c.next` 的 `origin` 就是 `c.next`，它的对象又是 `c`）：再沿它
         * 走会无限打转。跳数上限只把转圈截断，而截断后 `promoteFieldsAt(hops + 1)` 会带着超过
         * 上限的跳数进来、触发 `EXTC_DBG_FALLBACK`（断言版判红）。按绑定去重：同一个绑定在一次
         * 走查里只走一次来源；要求正沿那条路施加，重复走没有新信息。 */
        bool onStack = false;
        for (int i = 0; i < c->fsDepth; i++)
            if (c->fsChain[i] == sy) { onStack = true; break; }
        bool ok = true;
        if (!onStack) {
            c->fsChain[c->fsDepth++] = sy;
            /* 来源已经在记录时展平（见 `noteOrigin`），所以这里至多一步，不会重复查同一个绑定。 */
            ok = promoteInto2(c, sy->origin, at, hops + 1);
            c->fsDepth--;
        }
        /*
         * 来源只覆盖声明处写下的字段，所以还要读字段表的来源：之后的 `h.q = x` 只存在于字段表。
         *
         * The origin only covers the fields written at the declaration, so read the field
         * table's sources as well.
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

    /* 这里多走三种形状。`promoteInto` 必须能沿值的载体一直走到里面的分配，否则“这个值逃逸了”这个
     * 事实永远发现不了，而 `o.f = cell`、`a[i] = cell` 是最常见的形状。纪律与结构体字面量、数组
     * 字面量一致：只走载体，不追别名、不做跨函数推理，提不动的载体答 false，由调用方报错。 */
    case EX_SLICE:
        return promoteInto2(c, val->u.slice.obj, at, hops + 1);
    case EX_FIELD:
        return promoteInto2(c, val->u.field.obj, at, hops + 1);
    case EX_INDEX:
        return promoteInto2(c, val->u.index.obj, at, hops + 1);

    case EX_COALESCE: {
        /* 两侧都要试、不短路：只提一侧会漏掉另一侧的 `new`。 */
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
        /* 建池的调用是**分配站点**，与 `new` 一样可以提权：新分配的东西没有别的所有者，所以“池生在
         * 哪一层”这个数可以改小（越小越长寿）。容器能被放进更长寿的容器的前提就是这一格：
         * `outer.push(inner)` 要求 `inner` 活到 `outer` 那一层。别的调用结果照旧不可提：`slice`
         * 视图来自别人的存储（借用），提不动就是提不动，返回 false 让调用方报错。
         *
         * 建池的调用是**分配站点**，与 `new` 一样可以提权：新分配的东西没有别的所有者， 所以"池生在哪一层地方"这个数可以改小（越小越长寿）。
         */
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
        /* 被调方**需要 home arena**（`needsHome`）时，把新分配放进的是**调用点传进去的**那个
         * arena：那个数由调用点选，所以可以像 `new` 一样提权（越小越长寿）。这条一直是
         * `return mk(1)` 能通过的原因（`markCallHomeIfEscaping(..., 0)` ⇒ `__extc_home`）；
         * `var p = mk(1)` 只把 arena 记成块层、提权要求回不到调用点，于是 `return p` 被误拒。
         * `arenaArg` 与 `homeDepth` 必须一起搬：前者是 codegen 读的，后者是 `checkCallRefArgs`
         * 读的目的地深度。只往更长寿的一侧挪（单调），已更长寿时不抬回来。 */
        /* 只对**引用**结果做这件事：它直接命名被调方新分配的那个对象，把调用点的 arena 层
         * 改掉就把它挪到了正确的地方。按值返回的聚合（`box { p: mut slice<i32> }`）里的视图
         * 另有既存的保守规则（`tests/errors/escape_call_result_to_outer.extc` 常设反例），
         * 不在这一步的范围内。 */
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
        /* 从容器里取出来的值（`v.get(k)`）可能**正是当初存进去的那个引用**：取一次容器元素
         * 并不产生新存储，它交回的寿命就是容器里那份的寿命。容器自己的字段表记着那些来源
         * （`callSrc` / `otherSrc` / 字段槽），这里也把它们提到 `at` —— 只把寿命拉长，是安全
         * 方向。不这样做时，`v.push(p)` 存进去的 `p` 只被要求活到块层，而 `return head` 交出
         * 去的链已经指向它，函数返回即悬垂。
         *
         * 判定不变（仍然返回 false，交给调用方的深度比较）：这里只是把事实记足。
         *
         * 接收者只认 `EX_IDENT`，并且用 `lookup` 而不是 `placeRoot`：这一段在**末轮重放**里也会
         * 跑到（那时作用域已经关了），`placeRoot` 有"必须有作用域"的断言；`lookup` 在没有作用域时
         * 查不到局部绑定 ⇒ 安静跳过（检查那一趟已经把该提的提过了）。 */
        if (val->kind == EX_METHOD && val->u.method.recv &&
            val->u.method.recv->kind == EX_IDENT) {
            Sym *rv = lookup(c, val->u.method.recv->u.ident.name);
            if (rv) promoteFieldsAt(c, rv, at, hops + 1);
        }
        return false;

    default:
        /* 别的都提不动（指向局部的 `ref`、借用等）。不猜：退回原深度检查并报错。 */
        return false;
    }
}

/**
 * @brief 记下“这个值必须装进某一层”这条电平事实。
 *
 * 记在站点而不是绑定上：重放跑在所有函数体检查完之后，那时局部绑定已查不到。值是标识符时换成它
 * 的来源站点或建池站点。电平求解过程中不记，免得事实表无界增长。
 *
 * @param c    检查器；记录追加到 `c->lvlFacts`。
 * @param val  被存储或返回的值。
 * @param at   值必须装进的层；负值忽略。
 *
 * "must outlive this frame" is represented by `ARENA_HOME` rather than by a level
 */
void recordLvlFact(Checker *c, Expr *val, int at) {
    /* `at == 0` 也要记：对 arena 那是"活到本帧之外（家 arena）"，对池那是 `ZONE_HOME`（交给调用者
     * 选的地方）。**返回**那一档正是 `at == 0`，不记它，闭包之后的重放就没有这条，函数返回的容器永远
     * 提不到调用者那一层。 */
    if (!c || !val || at < 0) return;
    /* 记在**站点**上，不记在绑定上：末轮重放跑在所有函数体检查完之后，那时 `lookup` 已经查不到当时
     * 的局部绑定（符号表按函数/作用域），记绑定等于记一条谁也走不通的链。池的情形是"绑定由一次调用
     * 初始化"（`origin` 对调用为空），那种绑定身上带着 `poolSite`，换出来即可。 */
    if (val->kind == EX_IDENT) {
        /* 先取名字再让 `val` 改指来源：改指之后 `val` 可能是任何 kind，从它读 `u.ident.name` 会读
         * 另一个 kind 的联合成员 —— 野指针，会让下面的 dump 自己段错误。
         *
         * Take the name **before** `val` is re-pointed at the origin: after the reassignment
         * `val` may be any kind, and reading `u.ident.name` off it reads another kind's union
         * member -- a wild pointer that made the `[fact-sy]` dump itself segfault.
         */
        const char *nm = val->u.ident.name ? val->u.ident.name : "-";
        Sym *sy = lookup(c, val->u.ident.name);
        if (sy && sy->origin) val = sy->origin;
        else if (sy && sy->poolSite) val = sy->poolSite;
        if (getenv("EXTC_DBG_ZONE"))
            fprintf(stderr, "[fact-sy] %s sy=%p origin=%p poolSite=%p\n",
                    nm, (void *)sy,
                    sy ? (void *)sy->origin : NULL, sy ? (void *)sy->poolSite : NULL);
    }
    /* 求解过程中绝不记录：求解会重进本函数，记下去会让事实表无界增长。 */
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
 * @brief 把一条电平事实施加到站点上，保留最强要求（最小的层）。
 *
 * 记录事实与改层号是两件事，但跑在同一次遍历里（都在 `promoteInto2` 入口），所以不会漏站点。
 *
 * @param c    检查器；要求存在表达式节点上，本参数不用。
 * @param val  约束作用的值。
 * @param at   值必须达到的层；负值忽略。
 */
static void applyLvlFact(Checker *c, Expr *val, int at) {
    (void)c;
    if (!val || at < 0) return;
    if (anMinAt(val) < 0 || at < anMinAt(val)) anSetMinAt(val, at);
}

/**
 * @brief 求一个值的根来源。
 *
 * 沿载体链一直走到“不是带已知来源的绑定”的表达式，那就是值真正的来处。在这里展平链才能让来源之后
 * 仍可用：中间绑定到那时可能已出作用域，`lookup` 会查不到。
 *
 * @param c    检查器。
 * @param val  要求来源的表达式。
 * @param hops 已走过的绑定步数；超过 32 步停止。
 * @return     根表达式；没有可追的来源时就是 `val` 本身。
 */
Expr *originOf(Checker *c, Expr *val, int hops) {
    if (!val) return val;
    if (hops > 32) { EXTC_DBG_NOTE("carrier walk: recursion budget (32) exhausted"); return val; }
    /* 结构体字面量、`new` 之类的形状就是它自己的来源。 */
    if (val->kind != EX_IDENT) return val;
    Sym *sy = lookup(c, val->u.ident.name);
    /* 没有来源 ⇒ 答案就是值本身，提不动。 */
    if (!sy || !sy->origin) return val;
    return originOf(c, sy->origin, hops + 1);
}

/**
 * @brief 检查一次存储所需的全部逃逸规则。
 *
 * 深度规则之外还有“借来的值不得存到活过本次调用的地方”。目标深度为 0 且值可追到参数、目的地又能
 * 追到参数时，改由调用点判断寿命。类型提及类型参数时记录延迟检查。
 *
 * 有两个不同的数在起作用：记账用的层（被容器自己的作用域封顶）与判定用的深度（地方所指存储的
 * 深度）。只有前者封顶，因为 `h.q` 这类投影报得比容器本身活得久，而后者才是引用规则问的问题。
 *
 * @param c       检查器。
 * @param val     被存储的值。
 * @param target  被存入的地方。
 * @param line    源码行号，用于诊断。
 * @return        拒绝这次存储时返回 true；允许时返回 false。
 *
 * Notes: - Two different numbers are in play: the level the store is accounted at, which the
 * container's own scope caps, and the depth the store is checked against, which is the depth of
 * the storage the place denotes.
 */
bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    /* 类型提及类型参数的值把整条检查推迟：在这里返回，免得下面的 `checkEscape` 重复记一次延迟检查。 */
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, target, slotDepth(c, target), line, "this assignment");
        return false;
    }
    /* 这里问的是值被存进哪一层，所以用 `storeLayer` 而不是 `slotDepth`（陷阱见 `storeLayer`）。 */
    int at = storeLayer(c, target);
    /* 这次存储需要的层被容器的寿命封顶。`h.q` 或 `a[i]` 这类投影下 `storeLayer` 会答路径的深度，
     * 比容器自己的块层更深，而容器活不过自己的作用域，存进去的东西只需活那么久。
     * 只降这个记账数；下面传给 `checkEscape` 的 `at` 原样不动。
     *
     * For a projection such as `h.q` or `a[i]`, `storeLayer` answers with the depth of the path
     * it projected, which is deeper than the container's own block level (`h.q` answers 2 while
     * `h` sits at 1).
     */
    int atStore = at;
    if ((int)c->scopes.len < atStore) atStore = (int)c->scopes.len;
    /* 先试提权。提不动时下面的调用仍会报错，所以这是安全网，且幂等。 */
    recordStore(c, val, target, atStore, line);
    promoteInto(c, val, atStore);
    bool bad = checkEscapeDefer(c, val, at, line, "this assignment", target);
    /* 值的来源能追到参数时接受这次存储，把寿命交给调用点判。
     *
     * 被调方只编译一次，不知道调用者的池，所以它只能发布约束“第 j 个实参背后的数据至少活到被调方把
     * 它存进去的那个容器那么久”；调用点能求值这条约束。
     *
     * 放宽需要两个前提，缺一不可：1) 值的来源能追到形参，调用点才算得出它的寿命；2) 目的地是能经参数
     * 到达的容器，那份存储的池才是调用点上对应实参的池。前提 2 不能去掉：目的地是全局（`g = s`）时
     * 约束是“必须永远活着（深度 0）”，调用点无法表达，于是存储仍被拒。反过来，同层的调用是合法的：
     * 调用与目的地一起死。追不到来源的值（调用结果、本帧局部、来源不明）照旧拒绝。
     *
     * The callee is compiled once and does not know the caller's pool, so all it can do is
     * publish the constraint "the data behind argument j lives at least as long as the
     * container the callee stores it into".
     * Precondition 2 cannot be dropped: when the destination is a global (`g = s`), the
     * constraint is "the data must live forever (depth 0)", which the call site's knowledge of
     * "which container the callee will store into" cannot express, so the store stays rejected.
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
        /* 有 home arena 的函数里接受这次存储：调用点已经保证每个 `ref` / `mut ref` 实参至少活到这次
         * 调用的 home arena，而被调方分配的东西也进同一个 home arena，所以存下的值与目的地同寿。
         * 没有那条保证就放行会是漏洞。
         *
         * The call site has already guaranteed that every `ref` / `mut ref` argument lives at
         * least as long as this call's home arena, and whatever the callee allocates goes into
         * that same home arena, so the stored value lives exactly as long as the destination.
         */
        /* 这个理由在目的地是**全局**时不成立：静态存储活过每个 arena，没有哪个 home arena 能让借用
         * 变安全。所以问目的地在哪里，而不是假设：全局目标落到错误分支。
         *
         * The excuse is "the destination and this call's home arena live exactly as long as
         * each other, so the call site has already guaranteed the value reaches that level".
         */
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
 * @brief 把 `new T[n]` 的尺寸检查推迟到实例化。
 *
 * 只在当前处于泛型体（可见类型参数非空）里记录；具体类型当场就能判定，推迟只会让错误来得更晚。
 *
 * @param c     检查器；记录追加到 `c->refChecks`。
 * @param t     声明的元素类型。
 * @param line  源码行，用于诊断。
 */
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

/**
 * @brief 把声明缺少初值时的零值检查推迟到实例化。
 *
 * 泛型体里 `T` 有没有零值要等类型实参才知道，所以记录在这里，实例化时再判。只在可见类型参数非空
 * 时记录。
 *
 * @param c     检查器；记录追加到 `c->refChecks`。
 * @param t     声明的类型。
 * @param line  源码行，用于诊断。
 * @param name  绑定名，用于诊断。
 */
void recordZeroCheck(Checker *c, Type *t, int line, const char *name) {
    /* 自由泛型函数与泛型方法一样需要这条记录：重放循环本来就会走 `c.funcInsts`，规则也相同 —— 没有
     * 零值的 `T` 不能被默认初始化。只看 `owner` 会漏掉自由泛型函数。 */
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

/**
 * @brief 记录一条延迟的引用检查，留到实例化时判。
 *
 * 只在泛型体里、且值提及类型参数时记录；具体类型当场就能判，推迟只会让错误来得更晚。深度与借用
 * 标志现在就算，因为作用域还开着、`lookup` 还找得到参数：留到实例化时算，函数作用域已经没了，
 * 深度会答 0，检查会静默失效。
 *
 * @param c       检查器；记录追加到 `c->refChecks`。
 * @param val     要检查深度的值。
 * @param target  被存入的地方；返回时为 NULL。
 * @param at      值必须达到的深度；0 表示必须活过本帧。
 * @param line    源码行，用于诊断。
 * @param what    被检查的操作，出现在诊断里。
 */
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
    /* 现在就算这些数，趁作用域还开着、`lookup` 还找得到参数。走查不会因节点提及类型参数而提前退出，
     * 所以这个深度是在“参数可能带引用”的假设下算的，正是实例检查需要的数。 */
    rc->depth    = targetDepth(c, val);
    anSetBorrowed(rc, exprBorrowed(c, val));
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

/* 一次深度拒绝的说明文字。当场报与推迟到 DFA 之后复判（`recheckPendingRefStores`）时共用，
 * 两条路的诊断必须一字不差。 */
#define ESCAPE_OUTLIVE_NOTE \
    "A reference may not outlive what it points to. Borrow from a parameter " \
    "(depth 0) or copy the data instead. " \
    "If this value is a container (varArray and friends), the storage inside it " \
    "was allocated in this frame's arena: build the container in the caller's " \
    "scope and fill it through a `mut ref` argument, or build it here with `new` " \
    "(that allocates in this function's home arena, so it may escape)."

/**
 * @brief `checkEscape` 的实现：`defer` 为真时把一次拒绝记进 `pendingRefStores`（不发诊断）。
 *
 * 推迟的只有"目的地经具名引用到达"（`(*cur).f` / `*cur`）那一类：它的 `at` 读的是
 * `anRefDepth(cur)`，而那个数在 body walk 里按词法顺序写、没有 join，DFA 要等 walk 之后才跑。
 * 记录留着当时的 `at`/`depth`，DFA 溢出时照原样报。
 *
 * @param c       检查器。
 * @param val     被存入/返回的值。
 * @param at      目的地深度。
 * @param line    源码行号。
 * @param what    诊断里的操作名。
 * @param target  被存入的地方；无目的地（返回、初始化）时为 NULL。
 * @param defer   为真且判定失败时，记录而不报。
 * @return        判定为拒绝时返回 true。
 */
static bool checkEscapeImpl(Checker *c, Expr *val, int at, int line, const char *what,
                            Expr *target, bool defer) {
    if (!val) return false;
    /* 类型提及类型参数的值现在判不了（`T` 可能是 `i64` 或 `slice<u8>`）。记下来，实例化时再判。 */
    if (mentionsParam(val->type)) {
        /* 先提权一次，再推迟。`promoteInto` 是电平事实的唯一入口，而本分支提前返回，所以每个返回或
         * 传入“提及 `T`”的值的地方都会一条约束都没有。不提权的话，泛型体里 `return v` 会让里面的
         * `new T[cap]` 没有事实，末轮把它放回块层，而返回值要求它活过本帧，实例检查于是报一整族误拒。
         *
         * 现在提权是可靠的：层号不取决于 `T` 是什么 —— “这份存储要活到第 k 层”对 `T = i64` 与
         * `T = slice<u8>` 是同一句话。返回值忽略：提不动不是错误，拒还是收由这里记下的延迟规则决定。
         *
         * Without the promote below, `varArray<T>::withCap`'s `return v` (which takes this
         * branch) would leave the `new T[cap]` inside it with no fact (`minAt = -1`); the final
         * pass would put it back at the block level as if nothing had touched it, while the
         * return value requires it to live beyond the frame.
         * The instance check then reports "depth 1, but this can only hold up to 0", a whole
         * family of false rejections.
         * Promoting now is sound because the level does not depend on what `T` is: "this
         * storage has to live until level k" is the same statement for `T = i64` and for `T =
         * slice<u8>`.
         */
        recordStore(c, val, NULL, at, line);
        promoteInto(c, val, at);
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
    /* 提权必须先跑，与 `checkStoreEscape` 的顺序一致：先提权，再比深度。`promoteInto` 也是电平事实
     * 的唯一入口，而下面的 `d <= at` 会提前返回，深度一旦看着够浅，站点就一条约束都没有。所以先记
     * 事实、后比深度；判据本身不变。 */
    promoteInto(c, val, at);
    int d = targetDepth(c, val);
    if (d <= at) return false;
    /* 下面照旧报错（或按 `defer` 记下来）；这里只是把问题留给定下之后的那一趟。 */
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
 * @brief 检查值里的引用不会活得比它被放入的存储更久。
 *
 * 先提权值里所有分配点，再用深度比较判定，因为浅到看不出问题时早退会让分配点失去约束。值类型提及
 * 类型参数时记下延迟检查并返回 false，留到实例化时判定。
 *
 * @param c     检查器。
 * @param val   被存储或返回的值。
 * @param at    目标深度；0 表示必须活过本帧。
 * @param line  源码行号，用于诊断。
 * @param what  被检查的操作，出现在诊断里。
 * @return      值会留下悬垂引用时已报错并返回 true；否则返回 false。
 *
 * The compiler does not know its real lifetime, and it may point at a local of the caller's
 * frame that is nested more deeply than this function.
 * This is one half of the same rule as "the depth of a call result is the maximum depth of its
 * arguments".
 * what - the operation being checked, named in the diagnostic ("this assignment")
 */
bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    return checkEscapeImpl(c, val, at, line, what, NULL, false);
}

/**
 * @brief 带目的地的判定：目的地经具名引用到达时把拒绝推迟到 DFA 之后复判。
 *
 * 只有那条路（`(*cur).f` / `*cur`）的 `at` 读的是按词法顺序写的 `anRefDepth`，词序会改变答案；
 * 其余形状当场报，行为不变。
 *
 * @param c       检查器。
 * @param val     被存入的值。
 * @param at      目的地深度（walk 里按当时的 `anRefDepth` 算出的）。
 * @param line    源码行号。
 * @param what    诊断里的操作名。
 * @param target  被存入的地方；NULL 等同 `checkEscape`。
 * @return        判定为拒绝时返回 true（推迟的那些不报诊断）。
 */
bool checkEscapeDefer(Checker *c, Expr *val, int at, int line, const char *what, Expr *target) {
    /* 只有"目的地经具名引用到达"才推迟：那条路的 `at` 读的是按词法顺序写的 `anRefDepth`，
     * 词序会改变答案。其余形状当场报，行为不变。 */
    bool defer = target && destDerefSym(c, target) != NULL;
    return checkEscapeImpl(c, val, at, line, what, target, defer);
}

/**
 * @brief DFA 落定后复判推迟的存储判定；仍然不够深才报诊断。
 *
 * 目的地深度这时才取到「可能被指物的**最浅**层」（`Sym.minPointee` 收全了全部改指），而不是
 * walk 里那个按词法顺序的值。这条 min 极性是必须的：`q2_dualpointee` 里 `cur` 可能指向参数（0）
 * 或帧内 `new`（1），取 min=0 才挡得住悬垂；取 DFA 的 max=1 会把洞放过去。
 *
 * `dfaSettled` 为假（DFA 溢出、结果整份被丢弃）时照记录里的旧数报，保持原来的保守行为。
 *
 * @param c           检查器；作用域必须还开着（`lookup` 要用）。
 * @param dfaSettled  DFA 是否给出了可用结果。
 */
void recheckPendingRefStores(Checker *c, bool dfaSettled) {
    if (c->pendingRefStores.len == 0) return;
    for (size_t i = 0; i < c->pendingRefStores.len; i++) {
        PendingRefStore *p = *(PendingRefStore **)vecAt(&c->pendingRefStores, i);
        if (!p || p->fn != c->curFunc) continue;
        int at = p->at, d = p->depth;
        if (dfaSettled) {
            /* 目的地绑定在 walk 里就解析好了：块作用域此时已经关掉，`lookup` 再查 `ocur` 这类
             * 块内绑定会查不到。深度取别名链上的 min（`ocur` 随 `cur` 走）。 */
            int dd = p->destSym ? symMinPointee(c, p->destSym, 0) : -1;
            at = (dd >= 0) ? dd : storeLayer(c, p->target);
            promoteInto(c, p->val, at);         /* 用定下来的目的地再提一次（幂等） */
            d = targetDepth(c, p->val);
            if (d <= at) continue;              /* 词序造成的误拒：撤销 */
        }
        ckError(c, p->line, ESCAPE_OUTLIVE_NOTE,
                "%s would hold a reference to a local variable that dies first "
                "(borrowed from depth %d, but this can only hold up to depth %d)",
                p->what, d, at);
    }
    /* 本函数的记录已经消费掉；记录表按模块存，别的函数下一趟自己会清。 */
    size_t w = 0;
    for (size_t i = 0; i < c->pendingRefStores.len; i++) {
        PendingRefStore *p = *(PendingRefStore **)vecAt(&c->pendingRefStores, i);
        if (p && p->fn == c->curFunc) continue;
        *(PendingRefStore **)vecAt(&c->pendingRefStores, w++) = p;
    }
    c->pendingRefStores.len = w;
}

/* ---- 路线 C：把「可能的环」的候选区域并起来 --------------------------------------
 *
 * 见 `check_internal.h` 的 `CycleMerge`。三段：记录（检查走查）→ 收集站点（DFA 之后，作用域
 * 还开着）→ 合并（全局电平求解之后，placement 之前）。
 *
 * 为什么合并的判据是「候选站点落在**不止一只**区域」：区域标记精确 ⟺ 划分不切开任何 SCC。
 * 一个可能闭环的写，如果宿主可能指向的对象分布在多只区域里，这条环就可能跨区；跨区种子只在
 * 持有它的槽被释放时撤销，于是环垃圾把两只区域互相钉住、谁都不扫。把这些候选站点并到**最浅**
 * 的那一层（长寿的那一只），这条可能的环就落进同一只区域。
 *
 * 方向：只把站点往**更长寿**的一侧挪（`m` 是候选里的最小值），永远不会造成悬垂；代价是可能
 * 多留一会儿。候选里有家区域（`anMinAt == 0`）时**不动** —— `needsHome` 在合并这一步之前
 * 已经关死，把别的站点也提进家区域会让 codegen 用到没有声明的 `__extc_home`。
 */

/**
 * @brief 把一个分配站点记进候选集（按指针去重，有上限）。
 *
 * @param out   候选集（`Expr*`）。
 * @param site  分配站点表达式。
 */
static void cycleSitePush(Vec *out, Expr *site) {
    if (!site || out->len > 256) return;
    for (size_t i = 0; i < out->len; i++)
        if (*(Expr **)vecAt(out, i) == site) return;
    *(Expr **)vecPush(out) = site;
}

/**
 * @brief 只读走查：收出一个表达式「可能指向的那些分配站点」。
 *
 * 只走「这个绑定**自己**可能指向谁」的三条来源：当前持有的值（`origin`）、声明处的别名来源
 * （`initSrc`）、以及直接赋值记下的 `heldSrc`。**不走字段表 / `otherSrc` / `callSrc`**：那三格
 * 记的是"写进这个绑定的**字段**里的值"（`s.next = keeper`），那是从宿主**可达**的东西，不是
 * 宿主本身可能指向的对象 —— 把它们当候选会把 `cross_region.extc` 那种「循环体的 `s` 指向长命
 * 的 `keeper`」也误并成一只区域，而那条跨区边正是 run-switch ①c 要考的登记路径。
 *
 * 停下来的方向是安全的那一侧：候选集小 ⇒ 少合并 ⇒ 保留原状，不会放掉什么东西。**不写任何
 * 记账** —— 这里只是把一个已知的遍历形状借来当"可能指向谁"的近似。
 *
 * @param c     检查器；作用域必须还开着（`lookup` 要用，所以本走查只在记录时跑）。
 * @param e     起点表达式。
 * @param out   候选集（`Expr*`）。
 * @param hops  跳数上限（防环）。
 */
static void cycleCollectSites(Checker *c, Expr *e, Vec *out, int hops) {
    if (!e || hops > 32 || out->len > 256) return;
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
    /* kind-default: 只有会**传递**引用的形状继续走；字面量、调用结果等到此为止。新 kind 若
     * 是引用载体，必须在这里加 case —— 漏掉只会让候选集更小（合并更少），方向安全。 */
    default:
        return;
    }
}

/**
 * @brief 记下一次「可能闭环」的堆引用写，供候选区域合并在电平求解后处理。
 *
 * 记录时**当场**收宿主可能指向的分配站点（作用域还开着，`lookup` 能用）；合并动作留到
 * `applyCycleMerges`。判据与字段说明见 `check_internal.h` 的 `CycleMerge`。
 *
 * @param c      检查器（作用域还开着）。
 * @param base   宿主绑定（`(*cur).f = v` 的 `cur`）。
 * @param value  被写入的值。
 * @param line   源码行号。
 */
void noteCycleMerge(Checker *c, Sym *base, Expr *value, int line) {
    if (!c || !base || !value) return;
    for (size_t i = 0; i < c->cycleMerges.len; i++) {
        CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
        if (r && r->base == base && r->value == value && r->fn == c->curFunc)
            return;   /* 同一个站点被驱动程序检查多趟：只记一次 */
    }
    CycleMerge *r = arenaAllocZero(c->arena, sizeof *r);
    r->base  = base;
    r->value = value;
    r->line  = line;
    r->fn    = c->curFunc;
    vecInit(&r->sites, c->arena, sizeof(Expr *));
    /* 站点**当场**收：这时作用域还开着（`lookup` 能用）。收在 DFA 之后不行 —— 那时块作用域
     * 已经关掉，`var head = new node` 这种块内绑定查不到（`recheckPendingRefStores` 的注释记着
     * 同一件事）。层号到全局求解之后才是定的，所以合并动作仍留在后段（见 `applyCycleMerges`）。 */
    cycleCollectSites(c, base->origin,  &r->sites, 0);
    cycleCollectSites(c, base->initSrc, &r->sites, 0);
    cycleCollectSites(c, base->heldSrc, &r->sites, 0);
    *(CycleMerge **)vecPush(&c->cycleMerges) = r;
}

/**
 * @brief 一个分配站点最终会落进哪一只区域；与 placement 用同一条规则。
 *
 * @param site  分配站点。
 * @return      `ARENA_HOME`（家区域）或块层号（>= 1）。
 */
static int cycleSiteWant(Expr *site) {
    int a = anMinAt(site);
    if (a == 0) return ARENA_HOME;
    if (a >= 1) return a;
    int lx = anLexicalLevel(site);
    return lx >= 1 ? lx : 1;
}

/**
 * @brief 全局电平求解之后：把「可能的环」的候选区域并到最浅的那一层。
 *
 * 在 placement 之前跑，只改 `anMinAt`（placement 自己会据此写 `arenaLevel`/`refDepth`）。
 * 候选落在家区域就**不动**（`needsHome` 已定）；`EXTC_RECLAIM=0` 直接返回（没有区域划分）。
 *
 * @param c  检查器。
 */
/**
 * @brief 「可能成环」的分配站点并查集：同组的站点不能被回收片切开。
 *
 * 组号经 `planSetSiteCycleGroup` 交给 codegen；只有分量含 ≥2 个分配站点时才成组。
 */
typedef struct { Expr **site; int *par; size_t n, cap; Arena *a; } SiteUF;

/**
 * @brief 取站点在并查集里的下标，没有就追加（新元素自成一棵）。
 *
 * @param u  并查集。
 * @param s  分配站点表达式。
 * @return   下标（0 起）。
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
 * @brief 找下标所在树的根（带路径压缩）。
 *
 * @param u  并查集。
 * @param i  下标。
 * @return   根下标。
 */
static int sufRoot(SiteUF *u, int i) {
    while (u->par[i] != i) { u->par[i] = u->par[u->par[i]]; i = u->par[i]; }
    return i;
}

/**
 * @brief 合并两棵（按根挂，不做秩优化 —— 站点数很小）。
 *
 * @param u  并查集。
 * @param a  下标。
 * @param b  下标。
 */
static void sufUnion(SiteUF *u, int a, int b) {
    a = sufRoot(u, a); b = sufRoot(u, b);
    if (a != b) u->par[b] = a;
}

/**
 * @brief 分配站点所属的「可能成环」组号；0 表示不属于跨站点的可能环。
 *
 * @param c  检查器。
 */

void applyCycleMerges(Checker *c) {
    if (!c || c->cycleMerges.len == 0) return;
    /* 这条修法只对**区域**有意义：`EXTC_RECLAIM=0` 根本不发回收运行期，也就没有区域划分，
     * 功能 arena 的层号保持不动 —— 关档是整套对照的基线，产物逐字不变（`main.c --help` 已经
     * 点名 `EXTC_RECLAIM` 会改产物）。 */
    const char *rec = getenv("EXTC_RECLAIM");
    if (rec && rec[0] == '0' && rec[1] == '\0') return;
    /* ── 站点并查集 → 「可能成环」组 ─────────────────────────────────────────────
     * 一条 `CycleMerge` 说：宿主可能指向的那些分配站点 + 被写入的那个分配站点，可能互相成环。
     * 同一连通分量里的站点**必须落在同一个回收片**：片边界切开一条环 ⇒ 跨片登记位成为环垃圾
     * 的种子 ⇒ 界随轮数涨（`digraph` + `EXTC_RC_SHARDS=4` 实测 4N/N = 3.84×，与 naive RC 同量级）。
     * 分量只有一个站点时**不用钉**：那个站点本来就在一片里 —— `shard_bands` 的 8 条独立链正是
     * 这种（每条链一个分配点），于是分片旋钮的停顿收益不会被一起钉死。 */
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
        /* 哪些函数坐在"跨站点"的环组里：它们的单元拒绝分片（`codegen.c` 的
         * `scanUnitReclaim`）。判据只认**分量含 ≥2 个站点**的组 —— 单站点自环不必钉。 */
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
            fprintf(stderr, "[cycle-group] 站点 %zu 个 -> %d 组\n", u.n, next);
    }
    for (size_t i = 0; i < c->cycleMerges.len; i++) {
        CycleMerge *r = *(CycleMerge **)vecAt(&c->cycleMerges, i);
        if (!r || r->sites.len == 0) continue;
        int m = -1;
        bool home = false;
        for (size_t k = 0; k < r->sites.len; k++) {
            int w = cycleSiteWant(*(Expr **)vecAt(&r->sites, k));
            if (w == ARENA_HOME) { home = true; break; }   /* 家区域的决定不能再改 */
            if (m < 0 || w < m) m = w;
        }
        if (home || m < 1) continue;
        bool multi = false;
        for (size_t k = 0; k < r->sites.len && !multi; k++)
            if (cycleSiteWant(*(Expr **)vecAt(&r->sites, k)) != m) multi = true;
        if (!multi) continue;                              /* 候选只落在一只区域：没被切开 */
        for (size_t k = 0; k < r->sites.len; k++) {
            Expr *site = *(Expr **)vecAt(&r->sites, k);
            int w = cycleSiteWant(site);
            if (w == ARENA_HOME || w == m) continue;
            if (anMinAt(site) < 0 || anMinAt(site) > m) anSetMinAt(site, m);
        }
        promoteInto(c, r->value, m);                       /* 值也要落进同一只区域 */
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[merge] fn=%s line=%d sites=%zu -> level %d\n",
                    r->fn && r->fn->name ? r->fn->name : "?", r->line, r->sites.len, m);
    }
}

/* "let 绑定"与"路径跨过只读引用"是两个问题：`var` 也能持有只读引用，如 `fn f(v: ref slice<i32>)`；
 * 写入要同时满足两者。 */
/**
 * @brief 判断通往这个地方的路径上跨过的引用是否都是 `mut ref`。
 *
 * 跨过只有两种方式：显式经过 `*p`，或 `p.f`、`v[i]` 里 `p` 是引用从而隐式落在它指向的存储上。
 * 目标自身的类型不算在内，所以重新指向一个绑定仍可写。把两个问题混在一起会既误拒
 * `(*cell).next = v`（字段类型是只读引用），又漏掉真正的穿引用写。
 *
 * @param e        地方表达式。
 * @param crossed  出参：存储位于跨过的引用之后时置 true。
 * @return         每个跨过的引用都是 `mut ref` 时返回 true，遇到只读引用时返回 false。
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
 * @brief 判断通往这个地方的路径是否跨过或落在只读引用上。
 *
 * 与 `pathRefsAllMut` 问的不是同一个问题：这里还把地方自身的类型算进来，例如 `c.bump()` 里 `c` 是
 * `ref counter`。两个答案都要，因为诊断措辞不同。
 *
 * @param e  地方表达式。
 * @return   地方自身的类型或路径上任一引用是只读引用时返回 true。
 */
bool pathHasReadonlyRef(Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->type && x->type->kind == TY_REF && !x->type->mut) return true;
        /* 显式跨过一次，如 `(*p).f`。解引用的类型是被指向的对象而不是引用类型，所以那份引用的可变性
         * 要在操作数上测：不测的话 `fn g(p: ref node) { (*p).val = 7 }` 会编过。 */
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
 * @brief 检查一个地方是否允许被写入。
 *
 * 三种原因各自有诊断：写 `*p` 需要 `mut ref`；路径跨过只读引用或只读视图需要类型带 `mut`；重新赋值
 * `let` 绑定直接被禁。
 *
 * @param c     检查器。
 * @param e     被写入的地方。
 * @param line  源码行号，用于诊断。
 * @param what  被检查的操作，出现在诊断里。
 * @return      写入被拒绝且已报错时返回 true；允许写入时返回 false。
 */
bool requireMutable(Checker *c, Expr *e, int line, const char *what) {
    /* `*p` 要单独一支，因为 `placeRoot` 不认它、会返回 NULL，那会被当成可写。可写性来自引用的
     * 类型：只有 `mut ref` 允许 `*p = v`。 */
    if (e && e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        if (ot && ot->kind == TY_REF && ot->mut) return false;
        ckError(c, line,
                "`*p = v` needs `p` to be `mut ref T`; a `ref T` is a read-only borrow",
                "cannot %s through a read-only reference", what);
        return true;
    }
    /* 这里问一次写能否跨过去，所以要紧的是每一步引用的可变性：这个地方自身的类型是只读引用
     * （`c.bump()` 里的 `c` 是 `ref counter`），或路径显式跨过只读引用（`(*p).f`）。
     * `pathHasReadonlyRef` 两样都答。 */
    if (pathHasReadonlyRef(e)) {
        ckError(c, line,
                "`ref T` is a **read-only** borrow; writing through it needs `mut ref T` "
                "in the declaration. Read-only is the default so that a signature says "
                "what it does.",
                "cannot %s through a read-only reference", what);
        return true;
    }
    Sym *root = placeRoot(c, e);
    /* 视图元素能不能写，取决于视图类型带不带 `mut`。这堵住按值传视图的洞：
     * `fn f(v: slice<i32>) { v[0] = 1 }` —— 形参是一份拷贝，但元素属于调用者。写需要签名里写
     * `mut slice<i32>`（或 `mut ref slice<i32>`）。
     *
     * Whether a view's elements can be written depends on whether the view's type carries
     * `mut`.
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
 * @brief 取视图类型的元素类型。
 *
 * 视图协议是 `data` 加 `len` 两个成员，这里按名字 `slice` 与恰好一个类型实参识别它。
 *
 * @param t  要检查的类型。
 * @return   元素类型；`t` 不是单实参的 slice 实例时返回 NULL。
 */
Type *viewElemOf(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return NULL;
    if (strcmp(t->sdef->name, "slice") != 0) return NULL;
    if (t->targs.len != 1) return NULL;
    return *(Type **)vecAt(&t->targs, 0);
}





/* 签名检查放在定义处：`fn !=`' 之类的错误不会拖到 C 编译器那一步才以
 * "invalid use of void expression" 的形式暴露。 */
/**
 * @brief 在定义处检查构造器的签名。
 *
 * 只检查所属类型里名为 `new` 的关联函数。返回类型必须是所属类型本身，或者是首个类型实参为所属类型
 * 的 `result<T, E>`（可失败的构造器）；否则在定义处报错，即使还没有人调用它。别的名字（如 `fn make`）
 * 是普通关联函数，可以返回任何东西。
 *
 * @param c  检查器。
 * @param f  被检查的函数定义。
 *
 * The one exception is a **fallible** constructor: `result<T, E>` is accepted, because opening
 * a file, parsing text and allocating are all things that can fail, and the caller then has to
 * write `?` -- which is how extC says "this can fail" everywhere else.
 * The error type is the constructor's own choice.
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
 * @brief 在定义处检查运算符方法的签名。
 *
 * 只检查可重载集合里的比较与算术运算符（含 `<<` 与 `>>`），其它名字一律放过。要求恰好两个参数、
 * 第一个参数是所属类型的引用；比较运算符必须返回 `bool`，算术运算符必须返回所属类型的值，只有
 * `<<` 与 `>>` 允许返回该类型的引用。
 *
 * 在定义处查使得每条使用路径都安全：签名错的 `fn !=` 即使没人调用也会报，泛型 `a != b` 在实例化前
 * 就定下，不会让 codegen 拿到返回 `void` 的 `!=`。
 *
 * @param c  检查器。
 * @param f  被检查的函数定义。
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
        /* 结果必须是所属类型。算术运算符按**值**返回：检查器把 `a + b` 定成操作数类型，结果站在
         * `a` 站的地方。
         *
         * 流运算符 `<<` 与 `>>` 还可以按**引用**返回所属类型：有状态的流需要它来串联
         * （`fin >> x >> line`），结果是同一个对象而不是拷贝，第一个 `>>` 推进的位置才是第二个接着
         * 读的位置；拷贝会带自己的位置，原对象永远不动。只有这两个名字有第二种形式。`a + b` 返回
         * 引用会把引用放到语言规定值站的地方，加法也不需要它。 */
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
    /* 右操作数可以是任意类型：这才让同一个类型上能定义多个运算符（`out << i64` 与 `out << f64`
     * 并存），每次使用按操作数的确切类型匹配。
     *
     * The right operand may be any type: that is what makes an operator definable more than
     * once on one type (`out << i64` next to `out << f64`), and it is matched on the operand's
     * exact type at every use.
     */
}




/**
 * @brief 在值位置检查表达式，引用必须显式写成 `*p`。
 *
 * 与 `checkExpr` 分工：这里要的是值，所以 `p` 指它指向的对象并标记该节点需要解引用；`checkExpr`
 * 要的是地方或引用本身（赋值目标、`ref x` 的操作数、字段/索引/slice 的基、方法接收者，这些都要
 * 取地址）。许可不在这里管，写能不能做由 `ref` 与 `mut ref` 决定。
 *
 * `ref x` 与 `alloc` 这类自身就要求引用的表达式保留引用类型；成员选择是导航而不是读值，因此仍然
 * 看穿引用。`@noCopy` 的值在这里被拒绝拷贝。
 *
 * @param c  检查器。
 * @param e  值位置的表达式。
 * @return   值的类型；引用出现在值位置时返回被指向的类型（此时已报错）。
 */
Type *checkValue(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    /* `ref x` 与 `alloc<T>(n)` 都显式要一个引用，所以都不自动解引用：解了就等于取消刚要求的引用，
     * 自相矛盾。`let r = ref n` 因此得到引用，而 `let y = r` 得到 `r` 指向的值。 */
    /* 没有隐式解引用：值位置给出引用是错误，诊断让用户写 `*p`。例外是本来就要引用的 `ref x` 与
     * `alloc`，以及成员选择（`p.field` / `p.method()`）—— 那是导航而不是读值，仍看穿引用。 */
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
 * @brief 按期望类型检查一个表达式。
 *
 * 期望类型是 option 或 result 时先补全裸构造器；期望的是值类型时，`@noCopy` 值的拷贝与流运算符的
 * 借用结果都会被拒绝。引用出现在非引用的期望处会报错。
 *
 * @param c     检查器。
 * @param want  上下文期望的类型；NULL 表示只要一个值。
 * @param e     被检查的表达式。
 * @return      表达式的类型；引用出现在值位置时返回被指向的类型（此时已报错）。
 */
Type *checkInto(Checker *c, Type *want, Expr *e) {
    /* 期望类型是 `option<...>` / `result<...>` 时也接受裸构造器，于是 `f(some(3))` 与
     * `var x: ?i32 = some(3)` 都能写：类型从上下文来，不必写全名。 */
    if (want) desugarBareCtor(c, e, want);
    Type *got = checkExpr(c, e);
    rejectStreamBorrow(c, e);
    /* 只在**要值**时拒绝拷贝：`f(ref s)` 与 `-> mut ref T` 传的是对象本身，那正是 `@noCopy` 的用途。 */
    if (!want || want->kind != TY_REF) rejectNoCopy(c, e, got);
    /* 与 `checkValue` 一样：在不要引用的地方给出引用是错误。 */
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
 * @brief 检查 `print` 或 `println` 的一个实参，引用在此自动解引用。
 *
 * 打印一个引用只可能意味着打印它指向的值，暴露地址本身既无意义也不该做。这里是仅存的一处保留自动
 * 解引用的值位置。
 *
 * @param c  检查器。
 * @param e  实参表达式。
 * @return   被打印值的类型；实参是引用时返回其指向的类型，并标记该节点需要解引用。
 */
Type *checkPrintArg(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    if (t && t->kind == TY_REF) {
        planSetDeref(e, true);
        return t->inner;
    }
    return t;
}

/* The value's "place" check: 只拒绝拷贝地方，`ref s` 仍是引用，不是拷贝。 */
/**
 * @brief 对按值拷贝 `@sharesStorage` 类型的绑定位置发出警告。
 *
 * 这类拷贝给同一块存储起了"第二个名字"：写一边会从另一边看见，`release` 一边会让另一边悬垂。
 * 只对读已有值的形状报告（`var b = a` / `s.f` / `v[i]`），新构造的值不报；库自身代码里的拷贝
 * 也不报 —— 判据是拷贝发生的位置，不是类型的来源。
 *
 * @param c  检查器，用于报诊断。
 * @param e  处于值位置的表达式。
 * @param t  它的类型。
 */
void warnSharedCopy(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    /* 库自己写的代码不报 —— 判据是**拷贝发生的位置**（`c->ctx->path` 是当前正在检查的文件），不是
     * 这个类型的来源：容器类型当然来自库，但会写错的是用户那份 `var b = a`。不挡的话用户每编译一次
     * 都看到一串指向 stdlib 的警告，那条真警告就被埋了。 */
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    /* 新构造的值不是拷贝：`vector<i32>::new()`、`f()`、`x.m()`、结构体字面量都产生一个全新的容器，
     * 与任何已有容器不共用存储。只有“读一个已经存在的东西”才是要指出的拷贝。
     *
     * 新构造的值不是拷贝：`var a = vector<i32>::new()` / `f()` / `x.m()` / 结构体字面量 ——
     * 它们产生一个**全新的**容器，与任何已有容器不共用存储。只有"读一个已经存在的东西" （`var b = a` / `s.f` / `v[i]`）才是那个需要被点出来的拷贝。
     */
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
 * @brief 对按值交出一个已有的 `@sharesStorage` 值发出警告。
 *
 * 交出已有值时调用者与本帧会共用同一块存储；刚在本函数体里新建的值属于移交，不报。判据是绑定声明的
 * 行号相对函数签名行号的位置：`sym->line <= curFunc->line` 就是参数（在外层），否则是本函数体里新绑
 * 的；`s.f` / `v[i]` 这类读已有东西的表达式一律算已有。库自身代码里的返回不报。
 *
 * @param c  检查器，用于报诊断。
 * @param e  被返回的表达式。
 * @param t  它的类型。
 *
 * 难点在"已有"与"刚建好"的分界 —— 两者写法上只差右边那一个词： return v                       // v 是参数或外层变量 ⇒
 * 交出去的是**拷贝**，两份共用存储 var v = vector<i32>::new()     // 新建的值 ⇒ 交出去就是移交，没有第二份，不报
 * 判据用声明的行号：`Sym.line` 是声明所在行，而 `c->curFunc->line` 是函数**签名**那一行 ⇒ `sym->line <= curFunc->line`
 * 就是参数（在外层），否则是这个函数体里新绑的。 另外 `s.f` / `v[i]` 这类读已有东西的表达式一律算"已有"。
 * 与"绑定那一处"的分工：那里管 `var b = a`（新名字指向同一块板块），这里管 `return b`
 * （值被交出去时也复制一份结构体）。两处合起来就是用户能遇到的"看起来是赋值、实际是共享" 的全部形状。
 */
void warnSharedReturn(Checker *c, Expr *e, Type *t) {
    StructDef *sd;
    if (!e || e->kind == EX_REF || !t) return;
    sd = structOf(ttBase(t));
    if (!sd || !sd->sharesStorage || !isPlace(e)) return;
    if (c->ctx && c->ctx->path && strstr(c->ctx->path, "stdlib")) return;
    if (e->kind == EX_IDENT && c->curFunc) {
        Sym *sym = lookup(c, e->u.ident.name);
        /* 函数体里刚绑的 ⇒ 新建的值，交出去没有第二份。 */
        if (sym && sym->line > c->curFunc->line) return;
    } else if (e->kind != EX_FIELD && e->kind != EX_INDEX) {
        /*
         * 调用结果、字面量、`alloc` 之类：都不是“已有的一份”。
         *
         * 调用结果、字面量、`alloc` 之类：都不是"已有的一份"
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
 * @brief 拒绝拷贝一个 `@noCopy` 值。
 *
 * 只拒绝**地方**：`var s: ifstream = f()` 绑定的是调用刚产出的值，不是任何东西的拷贝，而
 * `var s2 = s` 给同一个 reader 起第二个名字，位置会在另一个名字背后移动 —— 正是这个标注要防的失败。
 * `ref s` 是引用不是拷贝，仍然合法。
 *
 * @param c  检查器。
 * @param e  值位置的表达式。
 * @param t  它的类型。
 * @return   报了错返回 true。
 */
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

/**
 * @brief 判断表达式是不是返回引用的流运算符结果。
 *
 * `fin >> x` 交回的是同一个流对象而不是一个新值；移位的运算结果类型是整数，因此不会命中。
 *
 * @param e  被检查的表达式。
 * @return   表达式是 `<` 或 `>` 开头的二元运算且其类型为引用时返回 true。
 *
 * It is not a value, and unlike `pickFirst(ref a, ref b)` -- where the user wrote `ref` and
 * asked for a reference -- nothing was asked for here: the borrow is the operator's own doing.
 * True when this is a `<<` / `>>` whose result is a reference. A shift's result is an
 */
bool isStreamBorrow(Expr *e) {
    if (!e || e->kind != EX_BIN || !e->u.bin.op) return false;
    const char *op = e->u.bin.op;
    if (op[0] != '<' && op[0] != '>') return false;
    return e->type && e->type->kind == TY_REF;
}

/**
 * @brief 拒绝把流运算符的借用结果留存下来。
 *
 * 移交没有发生：借用是运算符自己做的，存下来会给同一个 reader 起第二个名字，而两个名字都看不到位置。
 *
 * @param c  检查器。
 * @param e  初始化式、实参或返回表达式。
 * @return   是流借用时已报错并返回 true；否则返回 false。
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
 * @brief 检查一个可能是 `e?` 的表达式。
 *
 * `?` 只在少数语句位置合法，那些位置走这条入口，其余表达式按值位置检查。
 *
 * @param c  检查器。
 * @param e  表达式，可能是 EX_TRY 节点。
 * @return   `?` 解包后的载荷类型，否则值位置的类型。
 */
Type *checkMaybeTry(Checker *c, Expr *e) {
    if (e && e->kind == EX_TRY) return checkTryInner(c, e);
    return checkValue(c, e);
}



/**
 * @brief 检查 `e?` 并把失败转发给外围函数。
 *
 * 操作数必须是 `option` 或 `result`；外围函数必须返回同类，`result` 时错误类型还要完全相同。在
 * `main` 里且操作数确实带载荷时直接解包，失败即 trap。
 *
 * @param c  检查器。
 * @param e  `?` 表达式。
 * @return   解包后的载荷类型；操作数或外围返回类型不合适时返回错误类型。
 */
Type *checkTryInner(Checker *c, Expr *e) {
    Type *ot = checkExpr(c, e->u.try_.operand);
    if (ttIsError(ot)) return ttError(c->tt);
    Type *ob = ttBase(ot);

    /* 在 `main` 里失败的一步**trap**：没有谁可以接这个失败（`main` 返回 `i32` 是 C 入口点规定的），
     * 而 main 的一步失败之后程序本来就结束了，说清哪里错然后停下才诚实。规则故意窄：只有 `main`。
     * 别的函数仍要声明 `result` 才能用 `?`，因为那里失败有地方可去。
     *
     * There is nothing to hand a failure to -- `main` returns `i32` because C's entry point
     * does -- and a program whose main step failed is over anyway, so the honest thing is to
     * say what went wrong and stop.
     */
    if (c->curFunc && !c->curFunc->owner && c->curFunc->name &&
        strcmp(c->curFunc->name, "main") == 0 &&
        /* …且只有操作数真的带载荷时。`main` 里的 `5?` 没有载荷，没有这个守卫 `vecAt(&ob->targs, 0)`
         * 会读空表并把编译器带进 SIGSEGV。守卫让下面的寻常路径报出“`?` 需要 `option<...>` 或
         * `result<...>`”。
         *
         * The guard lets the ordinary path below report the honest "`?` needs an `option<...>`
         * or `result<...>`" instead.
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
 * @brief 只按词法块深度与来源链算出的深度，供 `EXTC_DBG_RHO` 对照。
 *
 * 故意忽略 `targetDepth` 依赖的两样输入：分配站点的 arena 层（求解器还可能改它，所以改用它出生的块
 * 深度，稳定且求解器只会给站点更长的寿命），以及节点上缓存的深度（`targetDepth` 自己写它，绑定改指
 * 之后可能留着上一趟的残值）。
 *
 * 两者不一致且 `pure` 更大时，检查器认为值活得比实际更短，存储可能被跳过而不去提升分配站点。
 *
 * @param c    检查器（作用域栈与类型表）。
 * @param e    要量的表达式。
 * @param hops 递归守卫，用于环状来源链（`a = b  b = a` 是合法的）。
 * @param seen 这条链上已经访问过的节点。
 * @return     值可能携带的引用深度的上界。
 */
static int targetDepthPure(Checker *c, Expr *e, int hops, Expr **seen) {
    if (!e) return 0;                 /* nothing to ask about: 0 is the honest answer */
    /* 预算与环守卫都是**兜底**，两者都答 `DEPTH_UNKNOWN` 而不是 0：0 表示“活得最久”，会把保守方向
     * 弄反；`DEPTH_UNKNOWN` 表示“证不出活得够久”，比较那一侧按拒绝处理。
     *
     * The budget and the cycle guard are **fallbacks**, and both answer `DEPTH_UNKNOWN`, not
     * `0`: `0` means "lives the longest" and would point the conservative direction backwards,
     * while `DEPTH_UNKNOWN` means "cannot prove this lives long enough", which the comparison
     * side treats as a rejection.
     */
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
        /* 站点出生的块。`lexicalLevel` 在节点首次被检查时定下，之后不动。 */
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_IDENT: {
        /* 沿来源追到它出自的站点；绑定本身不携带独立于站点的深度。 */
        Sym *sy = identBindOf(e);
        /* `!sy` 是“这个 ident 没解析过” —— 不该在检查器里发生（走到这里说明有别的 bug），而“没有
         * origin”是**合法**的：参数的 origin 就是空的，它的深度本来就是 0。
         *
         * `!sy` 是"这个 ident 没解析过" —— 不该在检查器里发生（走到这里说明有别的 bug）， 而"没有 origin"是**合法**的：参数的 origin
         * 就是空的，它的深度本来就是 0。
         */
        if (!sy) { EXTC_DBG_FALLBACK("targetDepthPure: unresolved ident"); return 0; }
        if (!sy->origin) return 0;
        return targetDepthPure(c, sy->origin, hops + 1, seen);
    }
    case EX_FIELD: {
        /* 取该字段记录的深度，字段没有就退回对象。 */
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
        /* `*p` 与 `p!` 与其操作数指同一份存储。 */
        return targetDepthPure(c, e->kind == EX_DEREF ? e->u.deref.operand
                                                       : e->u.sign.operand, hops + 1, seen);
    case EX_REF:
        /* 在这里造出的引用指向当前块。 */
        return anLexicalLevel(e) > 0 ? anLexicalLevel(e) : 0;
    case EX_SLICE:
        /*
         * 从地方切出来的视图与那个地方同块活。
         *
         * A view carved out of a place lives as long as that place's block.
         */
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
     * 闭包的值就是它的环境：字段值是这里发生的读；函数体属于生成的 `call` 方法，在那里分析。
     *
     * A lambda's value is its environment: the field values are the reads that happen here; the
     * body belongs to the generated `call` method and is analysed there.
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
        /* 调用只能返回来自实参或静态存储的引用，所以最深的实参就是上界。 */
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
    /* `?` 与转换原样传递操作数的引用；运算符只能带操作数带的东西。这一格既是对照用的 oracle，也喂
     * 数据流事实，所以这些形状都要答。
     *
     * `?` and a conversion pass the operand's references through; an operator can only carry
     * what its operands carry.
     */
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
