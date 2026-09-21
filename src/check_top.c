/* 顶层：函数 / 泛型实例化
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include "check_internal.h"

/* ---------------------------------------------------------------- 顶层 */

static void resolveSignature(Checker *c, FuncDef *f) {
    /* 方法里，它所属 struct 的泛型参数是可见的 */
    Vec *params = f->owner ? &f->owner->typeParams : NULL;

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line, params);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line, params);
    if (f->ret && ttIs(f->ret, "void")) f->ret = NULL;
}

static void checkDeclarations(Checker *c) {
    Module *m = c->m;

    /* 重名 */
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

    /* 字段重名 + 方法重名 + 方法撞字段名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fa = *(FieldDef **)vecAt(&sd->fields, j);
            for (size_t k = j + 1; k < sd->fields.len; k++) {
                FieldDef *fb = *(FieldDef **)vecAt(&sd->fields, k);
                if (strcmp(fa->name, fb->name) == 0)
                    ckError(c, fb->line, NULL, "struct `%s` has duplicate field `%s`",
                            sd->name, fb->name);
            }
        }
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *ma = *(FuncDef **)vecAt(&sd->methods, j);
            if (findField(sd, ma->name))
                ckError(c, ma->line, NULL, "`%s.%s`: a field and a method cannot share a name",
                        sd->name, ma->name);
            for (size_t k = j + 1; k < sd->methods.len; k++) {
                FuncDef *mb = *(FuncDef **)vecAt(&sd->methods, k);
                if (strcmp(ma->name, mb->name) == 0)
                    ckError(c, mb->line, NULL, "struct `%s` has duplicate method `%s`",
                            sd->name, mb->name);
            }
        }
    }

    /* type 重名 / 变体重名 / 空枚举 */
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

    /* struct 和 type 之间也不能重名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < m->types.len; j++) {
            TypeDef *td = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(sd->name, td->name) == 0)
                ckError(c, td->line, NULL, "`%s` is already a struct", td->name);
        }
    }
}

static void checkMethodShape(Checker *c, FuncDef *f) {
    if (!f->owner) {
        /* 自由函数不能有 `self` —— 方法必须写在 struct 体内（定案 9）*/
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
        /* 不带 `self` = **关联函数**：`option<i64>::some(x)` / `point::origin()`。
         * 它属于这个类型，但不作用于某个值 —— 容器要的构造器就靠它，
         * 而且它是**显式**的（调用点写全类型，不靠上下文猜，定案 27）。 */
        f->isAssoc = true;
    } else {
        /* 比 sdef 而不是比类型指针 —— 泛型 struct 的 self 是 `ref Pair<A, B>` */
        Type *sb = ttBase(p0->type);
        if (p0->type->kind != TY_REF || !sb || sb->sdef != f->owner)
            ckError(c, p0->line, NULL, "`self` of `%s.%s` must be `ref %s`",
                    f->owner->name, f->name, f->owner->name);
    }
    for (size_t i = 1; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0)
            ckError(c, p->line, NULL, "`self` must be the first parameter");
    }

    checkOperatorSig(c, f);
}

/* 函数体里有没有**分配**（`new`）？—— 决定这个函数要不要一只"家"arena（A3）
 * 纯语法扫（在类型检查之前就能回答），所以下面 `new` 的深度当场就能定 ✓ */
static bool stmtHasNew(Stmt *s);
static bool exprHasNew(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_NEW: return true;
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

/* 体里有没有调用"有家"的函数？（检查完之后 `e->func` 已经填好了 ✓）*/
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
    if ((e->kind == EX_CALL || e->kind == EX_METHOD) && e->func && e->func->needsHome)
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
    default: return false;
    }
}
static bool callsNeedsHome(Stmt *body) { return stmtCallsNeedsHome(body); }

/* 体里有没有"往 `*p` 里写"？（出参形状：`fn push(head: mut ref ?ref node) { *head = cell }`）
 * 有 ⇒ 这个函数要一只家 arena（分配得进"实参那边"的 arena）✓ */
/* 目标是"参数那边"的地方吗？`*head = cell` / `l.head = cell` / `l.buf[i] = cell` 都算 ✓
 * （`?ref node` 这种引用型变量没法再取 `ref`，所以出参惯用"包一层 struct"——
 *   而 `varArray<T>` 本来就是这个形状 ✓）*/
static const char *placeRootName(Expr *e) {
    while (e) {
        if (e->kind == EX_IDENT) return e->u.ident.name;
        if (e->kind == EX_FIELD) { e = e->u.field.obj; continue; }
        if (e->kind == EX_INDEX) { e = e->u.index.obj; continue; }
        if (e->kind == EX_DEREF) { e = e->u.deref.operand; continue; }
        return NULL;
    }
    return NULL;
}
static bool isParamName(FuncDef *f, const char *n) {
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, n) == 0) return true;
    return false;
}

static bool stmtStoresThroughDeref(Stmt *s, FuncDef *f) {
    if (!s) return false;
    switch (s->kind) {
    case ST_ASSIGN: {
        const char *rn = placeRootName(s->u.assign.target);
        return rn && isParamName(f, rn);      /* 写进参数指的地方/字段 ⇒ 出参形状 ✓ */
    }
    case ST_IF:     return stmtStoresThroughDeref(s->u.ifs.thenBody, f) ||
                           stmtStoresThroughDeref(s->u.ifs.elseBody, f);
    case ST_WHILE:  return stmtStoresThroughDeref(s->u.whiles.body, f);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtStoresThroughDeref(*(Stmt **)vecAt(&s->u.block.stmts, i), f)) return true;
        return false;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtStoresThroughDeref((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, f))
                return true;
        return false;
    default: return false;
    }
}

/* ⭐ 调用点该传哪只 arena？（A3 第二半）
 * 依据 = **最浅的那个 `mut ref` 实参**所指对象住哪儿（ARENA.md §1.2）：
 *   · 实参是我自己的局部/字段/元素 ⇒ `&__extc_a[它的块深度]` （**精确**：新东西跟着它走 ✓）
 *   · 实参是我自己的参数           ⇒ 我的家（祖先那只）✓
 *   · 没有 `mut ref` 实参           ⇒ 0（退回老规则：有家传家、没有传当前块）✓ */
/* ⭐ 规则 ④（A3 第三半，2026-09-20）：**实参指向的东西必须活得 ≥ 这一刀的家 arena**
 *
 * 为什么需要它：被调函数可以把它存进**自己的家 arena**（ARENA.md §7 的链式论证：
 * "分配的东西比所有可写目标都长寿 ⇒ 写进哪都安全"）。但那句话只对**活得够久的实参**
 * 成立 —— 调用者要是把"更深的局部"传进来，家 arena 就比它长寿 ⇒ 悬垂 ✗
 *
 *     fn bind(s: mut ref slot, target: mut ref i32) { s.r = target }
 *     var keeper: slot                       // 深度 1
 *     { var n: i32 = 1  bind(ref keeper, ref n) }   // n 深度 2 > h=1 ⇒ 调用点报错 ✓
 */
void checkCallRefArgs(Checker *c, Vec *args, Vec *params, int homeDepth, int line,
                             const char *fname) {
    if (params->len != args->len) return;
    int h;
    if (homeDepth != 0) h = (homeDepth < 0) ? 0 : homeDepth;
    else h = (c->curFunc && c->curFunc->needsHome) ? 0 : c->scopes.len;

    for (size_t i = 0; i < params->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* 泛型推迟 ✓ */
        int d = placeDepth(c, place);
        if (d == 0 || d <= h) continue;              /* 活得够久 ✓ */
        ckError(c, line,
                "The callee may store this reference into the arena it was given, so the"
                " argument must live at least that long. Move it to a shallower scope.",
                "argument %zu of `%s` points into a deeper scope (depth %d) than the arena"
                " this call may store it in (depth %d)", i + 1, fname, d, h);
    }
}

/* ⭐ PLAN #24：**调用点按"结果逃不逃出当前块"选 arena**（2026-09-20 压测实测出来的）
 *
 * 背景：被调用者（有家）的分配进"我传给它的那只 arena"。以前一律传**我的家**
 * （= 函数体那只）⇒ 在**循环里**调用 ⇒ 每一轮的东西都累积到函数结束 ✗
 * 压测量到：300 轮 × 2001 节点链表 ⇒ 峰值 15.3MB vs C 5.9MB（≈14.4MB = 算得出来）✗
 *
 * 修法：看我把它**存到多深的地方**：
 *   · 存进**当前块**（`var l = build(…)` 就在循环体里）⇒ 传**当前块**那只
 *     ⇒ 每轮出块就回收 ✓ **峰值常数级** ✓
 *   · 存进**更浅的地方 / 返回出去**（`return build(…)`、赋给外层局部）⇒ 传我的家 ✓
 * 参数位置（`g(build(…))`）保守地不标 ⇒ 退回老规则（传家）✓ */
void markCallHomeIfEscaping(Checker *c, Expr *v, int at) {
    if (!v) return;
    if ((v->kind != EX_CALL && v->kind != EX_METHOD) || !v->func) return;
    if (!v->func->needsHome) return;
    v->homeDepth = (at < (int)c->scopes.len) ? -1 : (int)c->scopes.len;
}

int callHomeDepth(Checker *c, Vec *args, Vec *params) {
    int best = 0;                       /* 0 = 没找到 */
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        int d = placeDepth(c, place);   /* 参数 = 0；局部 = 它的块深度 ✓ */
        if (d == 0) d = -1;             /* 参数那边 ⇒ 传我的家 ✓ */
        if (best == 0 || d < best) best = d;
    }
    return best;
}

static void checkFunc(Checker *c, FuncDef *f) {
    FuncDef *savedFunc = c->curFunc;
    /* A3：**有分配 + 返回有用的东西（含引用/视图）** ⇒ 这个函数要一只"家"arena ✓
     * （返回 `i32` 的函数不要 —— 它的分配留在自己块里，A2 的紧致性不丢 ✓）*/
    if (stmtHasNew(f->body) &&
        ((f->ret && typeContainsRef(c->tt, f->ret)) || stmtStoresThroughDeref(f->body, f)))
        f->needsHome = true;

    Vec     *savedParams = c->curParams;

    c->curFunc = f;
    c->curParams = f->owner ? &f->owner->typeParams : NULL;
    pushScope(c);
    /* 每个函数单独一套 C 名字 —— 不同函数里的 `a` 互不影响（生成 C 时它们本来就在
     * 不同的函数体里）。泛型实例化会**再检查一遍同一个函数体**，但遍历顺序一样 ⇒
     * 算出来的名字也一样，不会漂移 ✓ */
    c->nameUses.len = 0;

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* 参数是**可变的** —— 它是调用者给的局部副本（跟 C 一致），
         * 所以 `fn f(real: Board)` 里可以 `ref real` */
        Sym *sym = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sym->cname;
    }
    /* 函数体不另开作用域 —— 参数和函数体的局部变量同一层。
     * 所以 `let a = ...` 遮蔽参数 = **命名**（合法，生成 C 里改叫 `a__2`）；
     * 而 `var a = ...` 遮蔽参数 = 声明第二个存储（报错，见 declare）✓ */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

    popScope(c);
    c->curFunc = savedFunc;
    c->curParams = savedParams;
}

/* 全局变量 / 常量（顶层 `let` / `var`）。
 *
 * **全局 = 深度 0** —— 它活得比谁都长。所以：
 *   ① 逃逸规则自动禁止把局部的东西存进全局（`0 ≥ 1` 为假）—— 不需要为全局写特殊规则
 *   ② 定长全局**不需要 arena**：它就是 C 的静态对象
 *   ③ 初始化式必须是**字面量**（C 的全局初始化器只能是常量表达式）
 */
/* 全局的初始化式必须是**常量**（C 的静态初始化器只能含常量表达式）。
 * 今天「常量」= 字面量 / 负字面量 / 枚举常量。
 * ⚠️ 还没有常量求值器，所以 `let A = 1 + 2` 会被拒 —— 报错信息要说清怎么办。 */
static bool isConstInit(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: return true;
    case EX_ENUMVAL: return true;                       /* `status.ok` 是常量 */
    case EX_BIN:                                         /* `15 * 15` —— C 会折叠 */
        return isConstInit(e->u.bin.left) && isConstInit(e->u.bin.right);
    case EX_UN:                                          /* `-1` */
        return e->u.un.operand && isConstInit(e->u.un.operand);
    default: return false;
    }
}
static void checkGlobals(Checker *c) {
    Module *m = c->m;
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&m->globals, i);

        /* 重名检查 */
        for (size_t j = 0; j < c->globals.len; j++)
            if (strcmp((*(Sym **)vecAt(&c->globals, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a global", g->name);
        for (size_t j = 0; j < m->funcs.len; j++)
            if (strcmp((*(FuncDef **)vecAt(&m->funcs, j))->name, g->name) == 0)
                ckError(c, g->line, NULL, "`%s` is already a function", g->name);

        if (g->ann) g->ann = ttResolve(c->tt, c->ctx, g->ann, g->line, NULL);
        if (ttIsError(g->ann)) g->ann = NULL;

        if (!g->init) {
            /* 零初始化：C 的静态存储期自动清零（跟定案 8 一致）。
             * 但含引用的类型没有零值 —— 全局又是深度 0，没有东西可借。 */
            if (g->ann && typeContainsRef(c->tt, g->ann))
                ckError(c, g->line,
                        "a global is depth 0, so a reference inside it has nothing to "
                        "borrow from without an initializer.",
                        "cannot zero-initialize the global `%s`: it contains a reference",
                        g->name);
            if (!g->ann) g->ann = ttError(c->tt);
        } else {
            /* ⚠️ 全局这里**故意不设 noHoist**：全局初始化式本来就有一条更根本的规则
             * —— "必须是常量"（C 的静态初始化；全局是 C 的静态对象）。
             * 让那条报出来，比让 `??` 报"没地方放临时变量"清楚得多 ✓
             * （`get() ?? -1` 里的 `get()` 本来就不是常量，跟 `??` 无关。）*/
            Type *it = checkValue(c, g->init);
            Type *declT = g->ann ? g->ann : it;
            if (g->ann) checkAssignable(c, g->ann, it, g->init, "initializer");

            /* 常量检查放在**类型检查之后** —— `color.empty` 要到那时才被改写成
             * 枚举常量节点（EX_ENUMVAL），在那之前它还是个 EX_FIELD。 */
            if (!isConstInit(g->init)) {
                ckError(c, g->line,
                        "A global exists before any function runs, so its initializer has to be "
                        "a constant (a literal, an enum constant, or arithmetic on those). "
                        "Anything computed belongs inside a function.",
                        "the global `%s` must be initialized with a constant", g->name);
                continue;
            }

            /* **深度 0** ⇒ 逃逸规则自动生效：把局部的东西存进全局会被拒 */
            checkEscape(c, g->init, 0, g->line, "this global");

            g->ann = ttIsError(declT) ? ttError(c->tt) : declT;
        }

        Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
        s->name  = g->name;
        s->type  = g->ann;
        s->mut   = g->mut;
        s->depth = 0;                     /* 全局 = 深度 0 */
        s->line  = g->line;
        *(Sym **)vecPush(&c->globals) = s;
    }
}

bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    Checker c;
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    vecInit(&c.scopes, arena, sizeof(void *));
    vecInit(&c.eqChecks, arena, sizeof(void *));
    vecInit(&c.globals, arena, sizeof(void *));
    vecInit(&c.nameUses, arena, sizeof(void *));
    vecInit(&c.narrow, arena, sizeof(void *));
    vecInit(&c.refChecks, arena, sizeof(void *));
    vecInit(&c.refChecks, arena, sizeof(void *));
    vecInit(&c.narrowMarks, arena, sizeof(size_t));

    c.tI32  = ttFromName(tt, "i32");
    c.tF64  = ttFromName(tt, "f64");
    c.tBool = ttFromName(tt, "bool");
    /* 字符串字面量的类型 = `slice<u8>`。
     * 它来自 prelude —— 也就是说**字符串类型是 extC 写的**，
     * 编译器只负责把字面量变成它的一个值。 */
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

    /* 先把所有 struct / type 的类型驻留出来（方法签名比较要用）*/
    for (size_t i = 0; i < m->structs.len; i++)
        ttFromName(tt, (*(StructDef **)vecAt(&m->structs, i))->name);
    for (size_t i = 0; i < m->types.len; i++)
        ttFromName(tt, (*(TypeDef **)vecAt(&m->types, i))->name);

    /* 第一遍：解析所有签名与字段里的类型名 */
    /* 枚举的**载荷**类型也是这里解析（`| circle(f64) | rect(f64, f64)`）*/
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
    for (size_t i = 0; i < m->funcs.len; i++)
        checkMethodShape(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* 第二遍：检查函数体 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        checkFunc(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* 推迟的 `==` 检查：对每个具体实例复查一遍。
     * 这是「不引入 trait」的代价 —— 错误晚到这里，但信息要说清是哪个实例。 */
    for (size_t i = 0; i < c.eqChecks.len; i++) {
        EqCheck *ec = *(EqCheck **)vecAt(&c.eqChecks, i);
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != ec->owner) continue;

            Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type,
                                    &ec->owner->typeParams, &inst->targs);
            if (!ttEquals(lt, ttSubstitute(tt, ec->node->u.bin.right->type,
                                          &ec->owner->typeParams, &inst->targs)))
                continue;

            if (!typeSupportsEq(lt, ec->op)) {
                ckError(&c, ec->node->line,
                        "`==` inside a generic is checked at instantiation, not on the template -- the price of having no traits. "
                        "Add a `fn ==` to that type.",
                        "`%s` needs `%s` to define `==`",
                        inst->name, typeStr(&c, lt));
            }
        }
    }

    /* ---------------------------------------------------------------- 泛型体的推迟复查
     *
     * `==` 那一批上面查过了；下面是**引用规矩**那一批（PLAN #17/#18）：
     * 模板期遇到"值里提到 `T`"就只记了一笔（那时 `T` 不透明），
     * 现在对每个具体实例带上替换重算一遍 ✓
     *
     * 为什么必须这么修：`typeContainsRef(T)` 对不透明的 `T` 只能回答 false，
     * 于是 `exprRefDepth` / `exprBorrowed` 全部早退 ——
     *     struct boxT<T> { v: T  fn stash(self: mut ref boxT<T>, value: T) { self.v = value } }
     * 用 `boxT<slice<u8>>` 实例化后能把这个视图洗进活得更久的对象 ⇒ **悬垂** ✗
     * 而"看到 `T` 就当它含引用"那种 2 行的保守修法，会把 `box<T>::set`
     * 这种教科书写法一起拒掉（对 `T = i64` 它完全安全）⇒ 只能按实例算 ✓ */
    for (size_t i = 0; i < c.refChecks.len; i++) {
        RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
        StructDef *owner = rc->func->owner;
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;

            c.substParams = &owner->typeParams;
            c.substArgs   = &inst->targs;

            /* 这个实例里那个 `T` 到底含不含引用？不含 ⇒ 这条规矩本来就不适用 ✓
             * （所以 `box<i64>::set` 照样合法，而 `boxT<slice<u8>>::stash` 会被挡住 ——
             *   这正是"不能简单地把 `T` 一律当成含引用"的原因）*/
            if (rc->isNewSize) {
                /* 实例化之后还带类型参数（或 void）⇒ 大小仍然不知道 ⇒ 报错点名实例 ✓ */
                Type *nt = tsub(&c, rc->declType);
                c.substParams = NULL;
                c.substArgs   = NULL;
                if (nt->kind == TY_PARAM || ttHasParam(nt) || ttIs(nt, "void"))
                    ckError(&c, rc->line,
                            "`new` needs a concrete type: its size is decided at compile time,"
                            " and this one is still not concrete after instantiation.",
                            "in instance `%s`: cannot `new` `%s` -- its size is not known"
                            " even here", inst->name, typeStr(&c, nt));
                continue;
            }

            if (rc->isZero) {
                /* 零值这一类：这个实例的 `T` 到底有没有零值？*/
                Type *zt = tsub(&c, rc->declType);
                c.substParams = NULL;
                c.substArgs   = NULL;
                if (typeLacksZeroValue(tt, zt))
                    ckError(&c, rc->line,
                            "A generic body is checked once on the template, where `T` is"
                            " opaque -- so this is re-checked for every concrete instance."
                            " Give the local an initializer.",
                            "in instance `%s`: `%s` has no zero value (it contains a"
                            " reference)", inst->name, rc->what);
                continue;
            }

            Type *vt = tsub(&c, rc->val->type);
            c.substParams = NULL;
            c.substArgs   = NULL;
            if (!typeContainsRef(tt, vt)) continue;

            if (rc->depth > rc->at) {
                ckError(&c, rc->line,
                        "A generic body is checked once on the template, where `T` is"
                        " opaque -- so the reference rules are re-checked for every"
                        " concrete instance.",
                        "in instance `%s`: %s would hold a reference to something that"
                        " dies first (depth %d, but this can only hold up to %d)",
                        inst->name, rc->target ? "this assignment" : rc->what,
                        rc->depth, rc->at);
            } else if (rc->target && rc->at == 0 && rc->borrowed) {
                ckError(&c, rc->line,
                        "A generic body is checked once on the template, where `T` is"
                        " opaque -- so the reference rules are re-checked for every"
                        " concrete instance.",
                        "in instance `%s`: cannot store a borrowed value into something"
                        " that outlives this call", inst->name);
            }
        }
    }

    /* ---- A3：`needsHome` 的**传递闭包** ----
     * 调用一个"有家"的函数时，调用者必须**有东西可传**（`__extc_home` 或
     * `&__extc_a[当前块]`）⇒ 调用者自己也得收一只家 arena ✓
     * 到不动点为止（函数不多，直接多跑几轮）✓ */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (f->needsHome) continue;
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

    return !ctx->hasError;
}
