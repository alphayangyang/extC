/* 顶层：函数 / 泛型实例化
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include "check_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include <stdlib.h>   /* getenv（EXTC_DUMP_EFFECTS 这个调试开关）*/

/* ⭐ PLAN #47：一个函数**可见的类型参数**在哪？—— 方法在 `owner` 上，自由函数在自己身上 ✓ */
Vec *funcTParams(FuncDef *f) {
    if (!f) return NULL;
    if (f->typeParams.len) return &f->typeParams;
    if (f->owner) return &f->owner->typeParams;
    return NULL;
}

/* 前向声明：下面这几件在"效果摘要"和"E 分析"里互相引用 ✓ */
static int  paramIndex(FuncDef *f, const char *name);
bool isEscapeeName(Checker *c, const char *n);

/* ---------------------------------------------------------------- 顶层 */

static void resolveSignature(Checker *c, FuncDef *f) {
    /* 方法里，它所属 struct 的泛型参数可见；**自由函数**用自己那份（PLAN #47 ✓）*/
    Vec *params = funcTParams(f);

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
                ckError(c, td->line, NULL, "`%s` is already a struct", DN(td));
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
                    DN(f->owner), f->name, DN(f->owner));
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
    /* ⚠️ `alloc<T>(n)` 是**内建原语**（`EX_GENCALL`，检查器保证只有它走这条路），
     * 它跟 `new` 一样是**从当前块 arena 里要地方** ✓
     *
     * 这里以前漏了 ⇒ 真 bug（2026-09-22 撞到）：只在内层块里分配的
     *     fn inner(n: i32) { { var q = alloc<i32>(250000)  *q = n } }
     * 被判成"不用 arena"（不声明 `__extc_a`），而块里照样吐 `&__extc_a[2]`
     * ⇒ 生成的 C **编不过**（gcc: `__extc_a` undeclared）✗
     * 更糟的是 `tests/arena/control-flow.extc` 正是这个形状，而验收脚本只
     * grep "out of arena memory" ⇒ **那条用例一直是空转的** ✗（脚本已同步收紧 ✓）*/
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
    /* ⚠️ **`EX_ASSOC` 也要算**（2026-09-22 修）：
     * `T::assoc(...)`（`varArray<i32>::withCap(1)` / `bufT<i32>::make(4)`）
     * 走的也是 `e->func`（check_expr.c 里 `e->func = f` 那条对 assoc 同样生效 ✓），
     * 可这里以前**只认 EX_CALL / EX_METHOD** ⇒ 调用了"会分配的关联函数"的函数
     * 被判成"不用 arena"（`mayUseArena = false`，连 `__extc_a` 都不声明），
     * 而调用点照样吐 `&__extc_a[k]` ⇒ **生成的 C 编不过** ✗
     * （复现：`fn helper() { var b: bufT<i32> = bufT<i32>::make(4) }`，`make` 里有 `new`）
     *
     * 这个洞以前**只对 main** 被 `mayUseArena` 里那句 `|| 是 main` 遮住了 ——
     * 而那句兜底的代价是：**主函数永远白带一整套 arena 样板**，于是每个 `while` 体
     * 都吐 `extc_arena_release(...)` ⇒ 实测矩阵乘 **103 ms → 34 ms（3.0×）** ✗
     * （是在"OI 数量级多语言横评"上量出来的 ✓）
     * 现在把根因堵上，兜底那句就可以去掉了 ✓ */
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
    /* ⭐ B1（ARENA-SOUNDNESS §9 档 1）：**藏在字面量里的调用**以前一概看不见 ——
     * 这个谓词只认 `EX_CALL/EX_METHOD/EX_ASSOC` 的**直接**位置，
     * 而 `var b: box = { r: mknode() }` 里的调用被 `EX_STRUCTLIT` 挡住了
     * ⇒ 传递闭包判"这个函数没有有家被调者" ⇒ 它拿不到家 arena
     * ⇒ 被调者分配到**调用者的块** arena（出块就 release），而记账说"深度 0" ⇒ 悬垂 ✗
     * （反例 A2/A3/A4：枚举载荷 / 数组字面量 / 外层局部字段，加了调用 ⇒ 都是 UAF ✓）
     * ⇒ 补上四种漏掉的容器形状（`EX_GENCALL` 的**实参**里也可能藏调用）✓ */
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprCallsNeedsHome((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprCallsNeedsHome(*(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        /* 带载荷构造：`holder.holding(mknode())` —— 载荷里藏调用 ✓ */
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

/* ⭐ 定案 65：数一数这个体里有几个 `@overwrite` 站点（顺序要跟 codegen 的
 * `collectOwSites` **一致** —— 两边都是"按源码顺序的同一棵树" ✓）*/
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

/* 本函数能不能（传递地）调到自己？能 ⇒ 它的 `@overwrite` 格子必须**每激活一块** ✓
 * （否则子调用会踩父激活那块存储 —— 父激活回来后看到的是子调用的数据 ✗✗）
 * ⚠️ 保守方向：拿不准（callees 里有个 NULL）就当"能" ✓ */
static bool funcReachesItself(Checker *c, FuncDef *f, int depth) {
    if (depth > 64) return true;                      /* 环保护 ⇒ 保守 */
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (!g) return true;
        if (g == f) return true;
        if (funcReachesItself(c, g, depth + 1)) return true;
    }
    return false;
}

/* 体里有没有"往 `*p` 里写"？（出参形状：`fn push(head: mut ref ?ref node) { *head = cell }`）
 * 有 ⇒ 这个函数要一只家 arena（分配得进"实参那边"的 arena）✓ */
/* 目标是"参数那边"的地方吗？`*head = cell` / `l.head = cell` / `l.buf[i] = cell` 都算 ✓
 * （`?ref node` 这种引用型变量没法再取 `ref`，所以出参惯用"包一层 struct"——
 *   而 `varArray<T>` 本来就是这个形状 ✓）*/
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
static bool isParamName(FuncDef *f, const char *n) {
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, n) == 0) return true;
    return false;
}

/* ⭐ 层 1 第三步（**值拷贝不延长寿命**，跟 `valDepthForStore` 同一条原理）：
 *
 * "把值写进参数指的地方"**并不总是**"有东西要活到帧外" ✗ —— 要分两种：
 *   · 写进去的是**引用/视图**（`dst.p = ref x`、`dst.p = q`）⇒ 那份存储必须活得够久 ⇒ 是 ✓
 *   · 写进去的是**纯值**（`dst.v = *p`）⇒ 存的是**字节的副本** ⇒ 源对象活多久无关，
 *     副本里的东西也一个都活不下来 ⇒ **不是** ✓
 * 判据同 `check_escape.c` 的 `typeCannotCarryRef`（连 `mentionsParam` 那半也一样：
 * `T` 可能带引用 ⇒ 推迟到实例化）✓
 *
 * 为什么这条直接决定长运行程序的内存（主人给的 50/25/25 形状，实测 **98 MB ⇒ 1 MB**）：
 *   `varArray<T>::push` 的体是 `self.buf[self.len] = v` ⇒ 旧判据一律答"是" ⇒
 *   **每一处**调 `push` 的函数都被传染成"有家" ⇒ `main` 有家 ⇒ `main` 里
 *   **每一个** `new` 都落进家 arena（一只活到进程结束的 arena）⇒ 全都不回收 ✗✗
 *   而值拷贝那条路一个字节都不需要留下来 ✓ */
/* ⚠️ **别写成两个互相兜底的函数** —— 第一版就是
 *    `valueMayCarryRef` 兜底 `exprMayCarryRef`、`exprMayCarryRef` 又兜底回来
 * ⇒ 当场 **段错误**（栈溢出，gdb 里 26 万层 `valueMayCarryRef` ✗）。
 * 结构：**一个递归函数 + 两个问题**（"这个类型能不能装引用" / "这个表达式的结果能不能"），
 * 每种表达式**只在自己那一支里递归**，谁都不兜底谁 ✓ */
static bool valueMayCarryRef(Expr *v) {
    if (!v) return false;
    switch (v->kind) {
    /* ① 它本身就是一份引用/视图 ✓ */
    case EX_REF: case EX_SLICE:
    case EX_DEREF:      /* `*p` 指向的地方里可能装着引用 ✓（保守）*/
    case EX_INDEX: case EX_FIELD:   /* `a[i]` / `o.f` 读出来的可能是引用 ✓ */
        return true;
    /* ② 这些形状看它们**装着什么** ✓ */
    case EX_IDENT:      return true;      /* 绑定：不知道它是什么（且字节码里看不出）⇒ 保守 ✓ */
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
    default: return false;   /* 标量字面量 / `new`（下面自己算它）✓ */
    }
}

static bool stmtStoresThroughDeref(Checker *c, Stmt *s, FuncDef *f) {
    if (!s) return false;
    switch (s->kind) {
    case ST_ASSIGN: {
        const char *rn = placeRootName(s->u.assign.target);
        /* 写进参数指的地方 ⇒ **只有"值里可能装着引用"时才需要活到帧外** ✓ */
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

/* ⭐ 1.2a（`PLAN-REGION.md` §6）：**效果摘要的传递闭包**
 *
 * 为什么必须有它：`Addr` 全空才允许跳过规则 ④，可"callee 里再转一手"（调另一个
 * 会存地址的函数）时，**直接**扫描看不到 ⇒ "Addr 全空"是**假的** ⇒ 收窄就等于放行悬垂 ✗
 * （这条是 `tests/errors/ref_arg_too_deep` 从"必须报错"变成"通过"抓出来的 ✓）
 *
 * 三条纪律：
 *   · **惰性 + memo**（`effState`）：只在真需要时算，算过就缓存 ✓
 *   · **环保护**：正在算的（`effState == 3`）⇒ 环 ⇒ 标"不完整"（保守）✓
 *   · **拿不准也标不完整**：`effUnknown`（有解析不出来的调用）⇒ 永远不完整 ✓
 * ⇒ 只有 `computeEffectsTransitive` 返回 **true** 时，才允许拿摘要去"跳过"任何检查 ✓
 */
bool computeEffectsTransitive(Checker *c, FuncDef *f) {
    if (!f) return false;
    /* ⭐ 定案 72：外部声明的摘要**就是那张签字**（`collectEffects` 已经填好）⇒
     * 它"完整"（我们选择相信声明 ✓）—— 别拿"没有体"去算成空摘要 ✗✗ */
    if (f->isExtern) return true;
    if (f->effState == 1) return f->effComplete;
    if (f->effState == 3) { f->effComplete = false; return false; }   /* 环 ⇒ 不完整 */
    f->effState = 3;
    bool complete = !f->effUnknown;
    for (size_t i = 0; i < f->callees.len; i++) {
        FuncDef *g = *(FuncDef **)vecAt(&f->callees, i);
        if (g == f) { complete = false; continue; }                   /* 自递归 ⇒ 保守 */
        if (computeEffectsTransitive(c, g)) {                         /* 合并 callee 的摘要 */
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
    /* ⭐ 档1（引理 1 / ARENA-FORMAL §3.4）：**只有真的发生"地址流"时**，才需要
     * 要求实参活得 ≥ 家 arena ✓
     *   · 效果摘要 Addr 全空 ⇒ 被调者从没存过 `&实参` ⇒ 这条约束**不存在** ✓
     *     （这就是 `push` 那类"只往容器里塞新东西"的 callee —— 今天却按最坏情况处理 ✗）
     *   · 拿不准（摘要不完整 / `otherMask` 非空）⇒ 退回今天那条保守规则 ✓ */
    /* ⚠️⚠️ **这里曾经收窄过**（"Addr 全空就跳过规则 ④"）—— 被双向判据当场打回 ✗
     * 证据：tests/errors/ref_arg_too_deep 从"必须报错"变成**通过** ✗
     * 根因：效果摘要**还没有传递闭包**（callee 里再转一手就漏）⇒ 摘要不完整时
     * "Addr 全空"是**假的** ⇒ 收窄就等于放行悬垂 ✗
     * ⇒ 纪律：**摘要完整之前，规则 ④ 保持保守**（宁可误拒，不可漏 UB）✓
     * 下一步（PLAN-REGION 1.2b）：照 §8.5 做**惰性传递闭包**（memo + 环保护），
     * 并且只在"摘要可判为完整"时才跳过本规则 ✓ */
    /* ⭐ 1.2a：**只有摘要被证明完整**时，才允许按"没有地址流"跳过下面的保守检查 ✓ */
    /* ⭐ 定案 72：**外部声明**单独一条路 —— 它是 C 那边的黑盒，
     * 签名里写的效果就是**全部**依据，而"它可能存下来"意味着那份存储可能
     * 活到**帧外**（C 那边可以塞进全局/静态）⇒ 那一档的实参必须活到**深度 0** ✓
     * （没签字 ⇒ 每个参数都按"会存"算 ⇒ 传局部会被挡 —— **默认安全** ✓
     *   签了 `effects Addr=0 Cont=0` ⇒ 一个参数都不查 ⇒ 好用 ✓）*/
    if (callee && callee->isExtern) {
        unsigned stored = callee->addrMask | callee->contMask | callee->otherMask;
        if (stored == 0) return;                 /* 签字说不存 ⇒ 无约束 ✓ */
        for (size_t j = 0; j < params->len && j < args->len && j < 32; j++) {
            if (!((stored >> j) & 1u)) continue;
            Expr *a = *(Expr **)vecAt(args, j);
            Param *p = *(Param **)vecAt(params, j);
            if (!p->type || p->type->kind != TY_REF) continue;   /* 标量没有寿命 ✓ */
            Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
            int d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, a);
            if (d == 0) continue;                /* 本来就活到帧外 ✓ */
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
    /* ⚠️⚠️ **两个检查各自决定跳不跳，别共用一个 early return** ✗
     * （共用的代价踩过：`push` 有 `Cont` 位就会让**地址流**那条也跑起来 ⇒
     *   `examples/list-return` 被一条"本来被早退挡住的"过严组合误拒 ✗
     *   —— E 分析那条路 `homeDepth = -1 ⇒ h = 0` 对**本地 mut ref 实参**过严 ✗）*/
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
        if (mentionsParam(p->type) || mentionsParam(place->type)) continue;  /* 泛型推迟 ✓ */
        /* ⚠️ 实参**不是一个"地方"**（最典型：`stash(ref l, new node)` 直接传一个 `new`）⇒
         * `placeDepth` 对非地方返回 0，那个数会被当成"活到永远"⇒ 这条规矩等于没查 ✗
         * （2026-09-22 修：那种实参的寿命就是它那块 arena 的**层**）✓ */
        int d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, place);
        /* ⭐ 定案 63（PLAN #38）：够不着就先试着**提升**（跟赋值边同一条规则）——
         * 提不动 ⇒ 下面照旧报错；提得动 ⇒ 它确实活到了这一层 ✓ */
        if (d != 0 && d > h && promoteInto(c, place, h))
            d = placeRoot(c, place) ? placeDepth(c, place) : exprRefDepth(c, place);
        if (d == 0 || d <= h) continue;              /* 活得够久 ✓ */
        ckError(c, line,
                "The callee may store this reference into the arena it was given, so the"
                " argument must live at least that long. Move it to a shallower scope.",
                "argument %zu of `%s` points into a deeper scope (depth %d) than the arena"
                " this call may store it in (depth %d)", i + 1, fname, d, h);
    }

    /* ⭐ 定案 67 / `ARENA-FORMAL` §9.2：**内容流** —— 被调者把「从实参 j 读出的指针 /
     * 含引用的**值**」存进容器 ⇒ 那份**数据**也得活得 ≥ 目的地所在层 h ✓
     * （这正是"借用规则挪到调用点"的那一条：被调者一次编译、不知道调用者的区域，
     *   所以它只**发布**约束；代入求解在这里 ✓）
     *   · 摘要完整 ⇒ 只查摘要点名的那些 j ✓
     *   · 摘要**不完整**（环 / 有解析不出来的调用）或 `otherMask` 非空 ⇒ 对**每个
     *     含引用的实参**都按最坏情况查 ✓
     *     ⚠️ 这条是**不可分割的一半**：不放它，被调者侧一放宽就等于放行悬垂 ✗
     *     （`PLAN #31` 那次收窄规则 ④ 的教训 ✓）*/
    if (contMaybe && (!complete || (callee && callee->otherMask != 0))) {
        for (size_t j = 0; j < args->len; j++) {
            Expr *a = *(Expr **)vecAt(args, j);
            if (mentionsParam(a->type)) continue;                  /* 泛型推迟 ✓ */
            if (!typeContainsRef(c->tt, tsub(c, a->type))) continue; /* 不带引用 ⇒ 恒真 ✓ */
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
        /* ⚠️ 四种位**都要查** ✗：摘要对"按值的视图/含引用的参数"可能记成 `Addr` 也可能记成
         * `Cont`（收集器按"它本身是不是指针/视图"分 ✗），但对**按值参数**来说两件事
         * 归纳成同一句话：**它携带的那份数据必须活得 ≥ h** ✓
         * （踩过：只查 Cont ⇒ `names.push(buf[..])`（buf 在更深的块里）**漏放**了 ✗
         *   那是真悬垂 ⇒ 判据当场抓出来 ✓）*/
        unsigned cont = callee->contMask | callee->homeContMask
                      | callee->addrMask | callee->homeAddrMask;
        for (size_t j = 0; j < args->len && j < 32; j++) {
            if (!((cont >> j) & 1u)) continue;
            Param *pj = *(Param **)vecAt(params, j);
            if (pj->type && pj->type->kind == TY_REF) continue;   /* `ref` 形参归规则 ④ ✓ */
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
    setCallArenaArg(c, v);          /* ⭐ 定案 68：两个数一起更新，永远不脱节 ✓ */
}

/* ⭐ 定案 68：**把 `homeDepth` 解析成"最终要传的那只 arena"**（`Expr.arenaArg`）——
 * 由检查器算定，codegen 只翻译成 C 文本 ✓
 *
 * 为什么要两个字段：`homeDepth` 是**判据**（算规则 ④ 的 `h`；`0` 那一档故意按深度 0 查
 * ⇒ 宁可误拒 ✓），而这里是**实际执行的选择**（`0` 那一档 = 当前块 ⇒ 更紧、少占内存 ✓）。
 * 两个数都在检查器里算完 ⇒ codegen 不用再看 `g->hasHome` 兜底（那就是"两个权威"）✗ */
void setCallArenaArg(Checker *c, Expr *e) {
    if (!e) return;
    if (e->homeDepth == -1) {                 /* "传我的家"（实参就在家那一级）✓ */
        e->arenaArg = ARENA_HOME;
        e->arenaArgPending = false;
    } else if (e->homeDepth >= 1) {           /* 明确的块层 ✓ */
        e->arenaArg = e->homeDepth;
        e->arenaArgPending = false;
    } else {
        /* 没有 `mut ref` 实参给出依据 ⇒ 老规矩："**我有家就传家**，没有就传当前块" ✓
         * ⚠️ "我有没有家"要看 `needsHome` 的**传递闭包**（查完所有函数体才有）✗
         * ⇒ 这里先按"当前块"记下，并打上 pending ⇒ 收尾 pass 再定 ✓
         * （这就是以前 codegen 里那句 `if (g->hasHome) return "__extc_home";` 的职责，
         *   现在挪到检查器里 —— **只此一处**，不再两边各判一次 ✓）*/
        e->arenaArg = (int)c->scopes.len;
        e->arenaArgPending = true;
        *(Expr **)vecPush(&c->curArenaSites) = e;   /* 收尾 pass 要回头找它 ✓ */
    }
}

int callHomeDepth(Checker *c, Vec *args, Vec *params, Expr *callNode) {
    int best = 0;                       /* 0 = 没找到 */
    /* ⭐ B4′：E 敏感的那些实参要**记下来**，等摘封闭合之后重算（见 EArenaSite 的注释）✓ */
    EArenaSite *rec = NULL;
    for (size_t i = 0; i < params->len && i < args->len; i++) {
        Param *p = *(Param **)vecAt(params, i);
        if (!p->type || p->type->kind != TY_REF || !p->type->mut) continue;
        Expr *a = *(Expr **)vecAt(args, i);
        Expr *place = (a->kind == EX_REF) ? a->u.ref.operand : a;
        int d = placeDepth(c, place);   /* 参数 = 0；局部 = 它的块深度 ✓ */
        if (d == 0) d = -1;             /* 参数那边 ⇒ 传我的家 ✓ */
        else {
            /* ⭐ 1.2b（ARENA-FORMAL §9）：这个实参**会被搬出本函数**吗？会 ⇒ 家 arena
             * 必须活得比"它的内容可能去的任何地方"更久 ⇒ 传**我的家** ✓
             * 不会 ⇒ 保持实参所在的那只 arena（PLAN #24 的紧致性不丢 ✓）*/
            const char *rn = placeRootName(place);
            if (rn && isEscapeeName(c, rn)) d = -1;
            /* 这一格的结果**取决于 E**（现在 E 可能还不准）⇒ 记下来收尾重算 ✓ */
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
                    rec->overflow = true;   /* ⚠️ 记不全 ⇒ 收尾必须**保守**，不许静默截断 ✗ */
                }
            }
        }
        if (best == 0 || d < best) best = d;
    }
    return best;
}


/* ⭐ 档1（`ARENA-FORMAL.md` §3.4 / `PLAN-REGION.md` 步骤 1.1）：**效果摘要**
 *
 * 要回答的问题：**这个函数往它的 `mut ref` 实参所指的容器里，存了什么东西？**
 * 这正是原规则 ④ 缺的那一问 —— 它一律按"最坏情况（地址流）"处理 ✗（ARENA-FORMAL §2.3）✓
 *
 * 三种来源（ARENA-FORMAL §2.1）：
 *   · **地址流 Addr(j)**：存 `ref 形参_j` / `ref 形参_j.字段` ⇒ 需要 `R_slot(实参 j) ⊒ H`
 *   · **内容流 Cont(j)**：存「从形参 j 读出来的指针」（`l.head` 那种）⇒ 需要 `ρ_j ⊒ H`
 *   · **Fresh**：存 `new` 出来的、字面量、标量 ⇒ **恒真**（不用记）
 * 另外：存「本帧局部的地址」是另一回事（调用点本来就该挡）⇒ 记 `addrFromLocal` ✓
 *
 * ⚠️ 这一步**只算、不用**（PLAN-REGION 的规矩：先做到"行为零变化"，
 * 由 `tools/golden.sh` 逐字节判据背书）✓
 */
static bool vecHasName(Vec *v, const char *n) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), n) == 0) return true;
    return false;
}
/* 初始值是不是"本帧新分配的东西"？`new X` / 全是 fresh 或标量的结构体字面量 ✓ */
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
/* 收集"fresh 局部"（一步：直接 `var x = new …`）—— 保守起见不做别名传播 ✓ */
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


/* ⭐ 档1（`PLAN-REGION.md` 步骤 1.2 / `ARENA-FORMAL` §3.4、§9）：
 * **E 分析 —— 哪些局部会被"搬出本函数"？**
 *
 * 为什么要它：被调者分配出来的东西住在"家" arena 里，而**家 arena 必须活得比
 * "这个容器的内容可能去的任何地方"更久** ✗（§3.2 的 C3/C4）
 * ⇒ 调用点必须知道"这个实参会不会逃出我这个函数" ✓
 *
 * 规则（保守方向 = **宁可多算**，多算只会费内存，漏算才会悬垂 ✗）：
 *   · `return e`：`e` 里出现的**每个名字**都进 E（不管它是不是真被拷出去）✓
 *   · `x = e` / `var x = e` 且 `x ∈ E`：`e` 里的名字进 E（传递）✓
 *   · 存进"形参所指的容器"里的东西：名字进 E（那只容器活得更久）✓
 * 不动点：反复走，直到集合不再长大 ✓（函数体大小有限 ⇒ 一定停）
 */
bool isEscapeeName(Checker *c, const char *n) {
    if (!n) return false;
    for (size_t i = 0; i < c->escapees.len; i++)
        if (strcmp(*(const char **)vecAt(&c->escapees, i), n) == 0) return true;
    return false;
}
static bool addEscapee(Checker *c, const char *n) {
    if (!n || isEscapeeName(c, n)) return false;
    *(const char **)vecPush(&c->escapees) = n;
    return true;
}

/* 把表达式里出现的名字加进 E；"真的加了新名字"返回 true（给不动点用）*/
static bool markNamesInExpr(Checker *c, Expr *e);
static bool markNamesInStmt(Checker *c, FuncDef *f, Stmt *s) {
    if (!s) return false;
    bool grew = false;
    switch (s->kind) {
    case ST_RETURN:
        return markNamesInExpr(c, s->u.ret.value);          /* 交出去的都算逃逸 ✓ */
    case ST_VAR:
        /* ⚠️ 只在"声明的这个名字本身会逃逸"时才传递（第一版无条件传递 ⇒ E 变成
         * "所有出现过的名字" ⇒ 全都在逃逸 ⇒ 浪费内存 + golden 全变 ✗ 已修）*/
        if (isEscapeeName(c, s->u.var.name)) grew |= markNamesInExpr(c, s->u.var.init);
        return grew;
    case ST_ASSIGN: {
        /* 只有"写进 E 里的东西"或"写进形参所指的容器"才传递 ✓（第一版无条件 ⇒ E 过大 ✗）*/
        const char *rn = placeRootName(s->u.assign.target);
        if (rn && (isEscapeeName(c, rn) || paramIndex(f, rn) >= 0))
            grew |= markNamesInExpr(c, s->u.assign.value);
        return grew;
    }
    case ST_IF:
        /* 条件里的名字**不算**逃逸（第一版无条件加 ⇒ E 过大 ⇒ 家 arena 到处变、误拒 out-param ✗）*/
        grew |= markNamesInStmt(c, f, s->u.ifs.thenBody);
        grew |= markNamesInStmt(c, f, s->u.ifs.elseBody);
        return grew;
    case ST_WHILE:
        /* 同上：循环条件不算逃逸 ✓ */
        grew |= markNamesInStmt(c, f, s->u.whiles.body);
        return grew;
    case ST_EXPR:
        /* ⭐ B4（ARENA-SOUNDNESS §9 档 1，**精准版**）：以前这里**无条件 false** ✗
         * 代价（反例 B2/B3，两条真 UAF）：经**调用**发布出去的局部不进 E
         * ⇒ `callHomeDepth` 把接收者当成"深度 1 = 调用者的帧"
         * ⇒ `arenaArg = &本帧arena`（**被调者自己的帧**！）而不是家 arena
         * ⇒ 被调者往那个容器里塞的新东西，在**本次调用返回**时就被 release ✗
         *
         * ⚠️ **不能无条件标**（试过：`println(x)` 这种读也被标 ⇒ 6 条误拒：
         *   `borrowing` / `container-of-view` / `ref-field-write` / `borrowed-stash-sameframe` /
         *   `G_stale_origin` … ✗）⇒ 只标**真的可能发布实参**的那些被调者的实参 ✓
         * 判据用**现成的效果摘要**：摘要里只要有一丁点"往哪儿存"的位
         * （`Addr/Cont/Other` 或其 home 版），这个被调者的实参就可能被发布出去 ⇒ 标 ✓
         * 摘要干净（如 `println`）⇒ 不标 ⇒ 精度保住 ✓
         * ⚠️ 摘要是"会不会存"的**上界近似**，所以判"可能" ⇒ 方向安全 ✓ */
        switch (s->u.expr.expr->kind) {
        case EX_CALL: case EX_METHOD: case EX_ASSOC: {
            FuncDef *cf = s->u.expr.expr->func;
            if (!cf) return false;
            unsigned pub = cf->addrMask | cf->contMask | cf->otherMask
                         | cf->homeAddrMask | cf->homeContMask;
            if (cf->addrFromLocal) pub |= 1u;
            /* ⚠️ **摘要不完整**时不许下"它不会存东西"的结论 ✗
             * （`effUnknown`：体里有解析不出来的调用 ⇒ 摘要永远不完整）
             * ⇒ 只要这个被调者**有 `mut ref` 形参**（那就是"能存进去"的入口），
             *    就按"可能存"处理 ✓ 方向安全（多标只多费内存）*/
            if (!cf->effComplete) {
                for (size_t pi = 0; pi < cf->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&cf->params, pi);
                    if (pp->type && pp->type->kind == TY_REF && pp->type->mut) { pub |= 1u; break; }
                }
            }
            if (pub == 0) return false;        /* 这个被调者不存任何东西（如 println）⇒ 不标 ✓ */
            return markNamesInExpr(c, s->u.expr.expr);
        }
        default:
            return false;                      /* 别的表达式语句（纯读/纯写）⇒ 不算逃逸 ✓ */
        }
    case ST_BLOCK:
        for (size_t k = 0; k < s->u.block.stmts.len; k++)
            grew |= markNamesInStmt(c, f, *(Stmt **)vecAt(&s->u.block.stmts, k));
        return grew;
    case ST_MATCH:
        /* 同上：match 主体不算逃逸 ✓ */
        for (size_t k = 0; k < s->u.match.arms.len; k++)
            grew |= markNamesInStmt(c, f, (*(MatchArm **)vecAt(&s->u.match.arms, k))->body);
        return grew;
    default: return false;
    }
}
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

/* 不动点：谁被搬出去，谁的内容也就跟着搬出去 ✓ */
static void computeEscapes(Checker *c, FuncDef *f);
static void computeEscapesMode(Checker *c, FuncDef *f, bool unionMode) {
    if (!c->escapees.arena) vecInit(&c->escapees, c->arena, sizeof(const char *));
    if (!unionMode) c->escapees.len = 0;     /* 并集模式：**保留**已有的名字 ✓ */
    if (unionMode) {
        /* 并集模式：跑到不再长大（`isEscapeeName` 会把已有的名字当成"已经在了"，
         * 所以这里必须多跑几轮直到完全不动 ✓）*/
        for (int round = 0; round < 64; round++)
            if (!markNamesInStmt(c, f, f->body)) break;
    } else {
        for (int round = 0; round < 32; round++)
            if (!markNamesInStmt(c, f, f->body)) break;   /* 不长大了就停 ✓ */
    }
    if (getenv("EXTC_DUMP_EFFECTS")) {
        fprintf(stderr, "[escapes] %-22s { ", FN(f));
        for (size_t i = 0; i < c->escapees.len; i++)
            fprintf(stderr, "%s ", *(const char **)vecAt(&c->escapees, i));
        fprintf(stderr, "}\n");
    }
}

static void computeEscapes(Checker *c, FuncDef *f) { computeEscapesMode(c, f, false); }

/* ⭐ 档2.3（`ARENA-FORMAL` §7.4 / `PLAN-REGION` §7）：**字段级深度**
 *
 * 问题：`Sym.refDepth` 只有一个数（"这个绑定里面那些引用指哪"的上界）。
 * 于是 `h.p = null` 只能用**弱更新**（取 max）⇒ 旧的 1 留着 ⇒ `return h` 误拒 ✗
 *
 * 做法：给每个绑定记一张**小的字段表**（最多 4 格，超了就记进 `otherDepth` 保守兜底）：
 *   · 根的有效深度 = `max(otherDepth, 各字段深度)` ✓（读 `h` 时用它）
 *   · 读 `h.p` ⇒ 直接读那一格 ✓
 *   · 写 `h.p = v` ⇒ **没取过地址就强更新**（覆盖那一格）✓；取过地址 ⇒ 弱更新（保守）✗
 *   · 整块赋值（`h = …`）⇒ 清表重填（取过地址 ⇒ 保守兜底）✓
 * `otherDepth` 是"归不到某一格"的那部分（元素写、超过 4 个字段……）⇒ 只增不减 ✓ 保守 ✓
 */
int *fieldDepthEntry(Checker *c, Sym *s, const char *field, bool create) {
    (void)c;                                       /* 早就用不上了 ⇒ 顺手消掉那条老 warning ✓ */
    if (!s || !field) return NULL;
    for (int i = 0; i < s->nfields; i++)
        if (s->fields[i].name && strcmp(s->fields[i].name, field) == 0) return &s->fields[i].depth;
    if (!create) return NULL;
    if (s->nfields >= 4) return NULL;             /* 满了 ⇒ 交给 otherDepth 兜底 ✓ */
    s->fields[s->nfields].name  = field;          /* 名字是驻留的（AST 里的），不会悬垂 ✓ */
    s->fields[s->nfields].depth = 0;
    return &s->fields[s->nfields++].depth;
}

/* 写某一格之后，把"根的有效深度"重算成 max(otherDepth, 各格) ✓ */
static void refreshRootDepth(Sym *s) {
    if (!s) return;
    int m = s->otherDepth;
    for (int i = 0; i < s->nfields; i++) if (s->fields[i].depth > m) m = s->fields[i].depth;
    /* ⭐ A1（ARENA-SOUNDNESS §9 档 0）：**根的记账深度只许长大**（上界性）。
     * 允许它下降 ⇒ "先把深的存进 A 格、再往 B 格写个浅的"就把整根压小，
     * 而浅的那一格**没有**抹掉 A 格里还活着的指针 ⇒ 之后整值拷贝/返回被放行 ✗
     * （反例 A/B/C：ASan heap-use-after-free ✓）*/
    if (s->refDepth > m) m = s->refDepth;
    s->refDepth = m;
}

/* 记一次"往引用型地方写"的深度（`field == NULL` 表示**整块赋值**或归不到字段）*/
/* ⭐⭐ 层 2（附录 D.2）：**把"这一格是谁写的"记下来** —— 只用于"顺着容器找站点" ✓
 * 两条纪律（都是实测踩出来的）：
 *   ① **只记能指认站点的形状**：`null`/字面量指不出任何分配 ⇒ 记了会把前面那次
 *      真来源顶掉 ✗（`if c==1 { h.q = x } else { h.q = null }` 就是这么断的）；
 *   ② 跟**它自己那次写入的深度**（`srcDepth`）比，不能跟 `depth` 比 ✗
 *      （`depth` 是弱更新取 max ⇒ null 那次会被误判成"更深" ⇒ 来源被清）✓ */
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

void noteFieldDepthWrite(Checker *c, Sym *root, const char *field, int d2) {
    if (!root) return;
    noteFieldSrc(root, field, d2, c->curStoreVal);   /* ⭐ 记账**一个字不动** ✓ */
    /* ⭐ PLAN #39：**引用型绑定**（`n: mut ref node`）上的字段写，写的是
     * **被指对象**的字段 —— 而 `refreshRootDepth` 把根的有效深度重算成
     * "max(各字段)"，等于把"我指着谁"（深度 2）覆盖成了"我指的那个东西里
     * 装着什么"（字段 0）✗ ⇒ 之后 `keeper = n` 被放行 ⇒ 悬垂（ASan 实锤）✗
     * 修法：**引用型绑定的 `refDepth` 只许往"更长命"的方向调，不许降** ✓
     * （非引用型的结构体绑定照旧可以降 —— 那正是档2.3 强更新的用处 ✓）*/
    bool isRefRoot = root->type && tsub(c, root->type)->kind == TY_REF;
    int  before    = root->refDepth;
    if (!field) {                                  /* 整块赋值 / 元素写 */
        /* ⭐ A1：**不许清表**、不许覆盖 otherDepth —— 元素写不会让别的元素/字段失效 ✗
         * （旧行为：`a[1] = x; a[0] = null` ⇒ 清表 + otherDepth 覆盖成 0 ⇒ 整块逃逸被放行 ✗）*/
        if (d2 > root->otherDepth) root->otherDepth = d2;
        refreshRootDepth(root);
        if (isRefRoot && root->refDepth < before) root->refDepth = before;   /* #39 ✓ */
        return;
    }
    int *slot = fieldDepthEntry(c, root, field, true);
    if (!slot) {                                   /* 表满了 ⇒ 兜底那一格只能取 max ✓ */
        if (d2 > root->otherDepth) root->otherDepth = d2;
    } else if (root->addressed) {                  /* 取过地址 ⇒ 别名可能写别处 ⇒ 弱更新 ✗ */
        if (d2 > *slot) *slot = d2;
    } else if (root->fieldsComplete) {
        /* ⭐ 层 1（数据流）第一步：**表完整** ⇒ 这一格被新值替代 ⇒ 它的旧值不再是
         * 这个容器任何一格的性质 ⇒ 重算根时**跳过它**（= `ARENA-FORMAL` §7.4 的强更新）✓
         * 为什么 sound：根的记数是"**所有格**的上界"；表完整 ⇒ 去掉一条**已不成立**的边，
         * 剩下的仍然是上界 ✓（不是凭空调小）*/
        *slot = d2;
        int m = root->otherDepth;
        for (int i = 0; i < root->nfields; i++) {
            if (root->fields[i].name && strcmp(root->fields[i].name, field) == 0) continue;
            if (root->fields[i].depth > m) m = root->fields[i].depth;
        }
        root->refDepth = m;
        if (isRefRoot && root->refDepth < before) root->refDepth = before;   /* #39 ✓ */
        return;
    } else {
        /* ⚠️ **表不完整** ⇒ 绝对不许下降 ✗
         * 缺的那些格可能有值：我上次就是这样造出一条真的 `stack-use-after-scope`
         * （`var h3 = h` 把 `q` 那一格丢了 ⇒ `h3.p = null` 之后 `q` 的深度凭空消失 ⇒
         *  `return h3` 被放行 ✗ —— 哨兵 `H1_strongupdate_missed` 当场抓到）✓ */
        *slot = d2;
    }
    refreshRootDepth(root);
    /* ⭐ PLAN #39：不要忘记"我指着谁" ✓（字段表说的是"我指的那个东西里装着什么"）*/
    if (isRefRoot && root->refDepth < before) root->refDepth = before;
}
static int paramIndex(FuncDef *f, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < f->params.len; i++)
        if (strcmp((*(Param **)vecAt(&f->params, i))->name, name) == 0) return (int)i;
    return -1;
}

/* 值 `e` 被存进 `f` 的第 `i` 个 `mut ref` 参数所指的容器 ⇒ 记哪一位 */
static void classifyStoredValue(Checker *c, FuncDef *f, Expr *e, int i, bool intoHome, Vec *fresh) {
    if (!e || !f) return;
    /* 地址流：存「某个地方的地址」⇒ 需要 R_slot(那个地方) ⊒ 目的地所在区域 */
    if (e->kind == EX_REF) {
        int j = paramIndex(f, placeRootName(e->u.ref.operand));
        if (j >= 0) {
            if (intoHome) f->homeAddrMask |= (1u << j);
            else if (i >= 0) f->addrMask |= (1u << j);
        } else {
            f->addrFromLocal = true;              /* 本帧局部的地址（另一类问题）*/
        }
        return;
    }
    /* 形参来的值：**必须分清"指针本身"还是"从容器里读出来的指针"** ✓
     *   · `s.r = target`（`target` 是**指针形参**）⇒ 存进去的是**调用者的指针**
     *     ⇒ 这是**地址流**：调用点必须保证"它指的东西"活得 ≥ 目的地 ✓
     *   · `n.next = l.head`（从形参的**内容里**读出来）⇒ **内容流** ✓
     * ⚠️ 第一版把两者都当内容流 ⇒ tests/errors/ref_arg_too_deep 从"必须报错"变成**通过** ✗
     *    （双向判据抓的：这一格错了，收窄规则 ④ 就等于放行悬垂）✓ */
    {
        int j = paramIndex(f, placeRootName(e));
        if (j >= 0) {
            /* ⚠️ 标量不带引用 ⇒ 不构成寿命约束（不然 varArray<i32>::push 会假报）
             * ⭐ 定案 67：**提到类型参数就算"带引用"** ✓ —— 摘要是在**模板**上算的，
             * 那时 `T` 不透明 ✗ ⇒ 不这么记的话 `varArray<T>::push` 的 `Cont(v)` 一格
             * **一位都不记** ⇒ 调用点等于没查 ⇒ 真悬垂漏放 ✗✗（判据当场抓出来的 ✓）
             * 代价为零：调用点看的是**具体实参**的类型 ✓（`push(ref v, 5)` 里 `5` 是 i32
             * ⇒ 那一步直接被跳过 ✓）*/
            bool carrier = (c && e->type)
                         ? (typeContainsRef(c->tt, tsub(c, e->type)) || mentionsParam(tsub(c, e->type)))
                         : true;
            bool isPtr = (e->kind == EX_IDENT) && e->type &&
                         (e->type->kind == TY_REF || ttIsViewType(e->type));
            if (carrier) {
                if (isPtr) {                      /* ① 地址流：调用者的指针被存进去了 */
                    if (intoHome) f->homeAddrMask |= (1u << j);
                    else if (i >= 0) f->addrMask |= (1u << j);
                } else {                          /* ② 内容流：从容器里读出来的指针 */
                    if (intoHome) f->homeContMask |= (1u << j);
                    else if (i >= 0) f->contMask |= (1u << j);
                }
            }
            return;
        }
    }
    if (e->kind == EX_NEW) return;                /* Fresh ⇒ 恒真 ✓ */
    { const char *vr = placeRootName(e);          /* `l.head = n`：n 是 fresh 局部 ⇒ 也恒真 ✓ */
      if (vr && vecHasName(fresh, vr)) return; }
    if (c && e->type && !typeContainsRef(c->tt, e->type)) return;   /* 标量 ⇒ 恒真 ✓ */
    if (i >= 0) f->otherMask |= (1u << i);        /* 拿不准 ⇒ 保守 ✓ */
}

static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e);

/* 语句里所有"往地方里存东西"的形状 */
static void collectEffectsStmt(Checker *c, FuncDef *f, Stmt *s, Vec *fresh) {
    if (!s) return;
    switch (s->kind) {
    case ST_ASSIGN: {
        /* 目标是"某个 mut ref 参数所指的容器"吗？`l.head = …` / `*p = …` / `l.buf[i] = …` ✓ */
        const char *dstRoot = placeRootName(s->u.assign.target);
        int i = paramIndex(f, dstRoot);
        if (i >= 0) {
            /* 目的地 = 形参 i 所指的容器 */
            Param *p = *(Param **)vecAt(&f->params, i);
            if (p->type && p->type->kind == TY_REF && p->type->mut)
                classifyStoredValue(c, f, s->u.assign.value, i, false, fresh);
        } else if (dstRoot && vecHasName(fresh, dstRoot)) {
            /* ⭐ 目的地 = **本帧新分配的对象**（`n.next = l.head`、`n.owner = ref l`）
             * —— 这才是 §3.4 里"往家内存里存"的那一类（`push` 的内容流就在这）✓ */
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

/* 表达式里"藏着"的调用：callee 的效果靠摘要传递（§8.5 的调用图 / SCC）✓ */
static void collectEffectsExpr(Checker *c, FuncDef *f, Expr *e) {
    if (!e) return;
    if ((e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) && !e->func)
        f->effUnknown = true;   /* 解析不出来 ⇒ 摘要永远不完整 ⇒ 保守 ✓ */
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

static void collectEffects(Checker *c, FuncDef *f) {
    /* ⭐ 定案 72：**外部声明没有体** ⇒ 摘要只能来自签字（或者最坏情况）✓
     * ⚠️ 少了这一条就是"摘要全空 = 它什么都不存"✗✗ —— 那是**放行悬垂**的方向 ✗ */
    if (f->isExtern) {
        if (f->hasEffects) {
            f->addrMask = f->extAddrMask;
            f->contMask = f->extContMask;
        } else {
            /* 没签字 ⇒ 每个参数都可能被存下来（保守到"几乎不能用"，但安全 ✓
             * —— 这正是"默认安全"该有的样子：想用得舒服就得签 ✓）*/
            unsigned all = 0;
            for (size_t i = 0; i < f->params.len && i < 32; i++) all |= (1u << i);
            f->addrMask = all;
            f->contMask = all;
        }
        f->effComplete = true;      /* 声明就是权威（跟"体算出来的"等价 ✓）*/
        f->otherMask   = 0;
        return;
    }
    /* ⚠️ Vec 自带 arena 指针（base.h）；FuncDef 是 arenaAllocZero 出来的 ⇒
     *   这里必须显式 vecInit，不然 vecPush 会拿 NULL arena 去分配 ⇒ 段错误 ✗（踩过）*/
    if (!f->callees.arena) vecInit(&f->callees, c->arena, sizeof(FuncDef *));
    computeEscapes(c, f);      /* ⭐ 档1：先算 E（"谁会被搬出本函数"），再用它选家 arena ✓ */
    c->escapeesFor = (int)(size_t)f;   /* 只是标记"算过了"（用地址当 id）*/
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

/* ⭐ 长运行内存：**用"解算完之后"的层号回答《这个值里的引用指多深》** ✓
 *
 * 为什么需要它：`recordRefCheck` 冻的是"查模板体那一刻"的深度，而**解算会把一些
 * 站点从家 arena 放回块层** ⇒ 冻下的那份是**旧世界的数** ✗
 * （实测：`varArray<T>::withCap` 的 `return v` 在模板那轮冻下 `depth=1`，
 *   而它里面那个 `new T[cap]` 已被解算正确地放在家 arena（深度 0）
 *   ⇒ 实例复查报 "depth 1, but this can only hold up to 0"，而真相是 0 ✗✗）
 *
 * 为什么不能直接用 `exprRefDepth`：它对绑定（`EX_IDENT`）要 `lookup()`，而
 * `runRefCheck` 跑在**别的模块的作用域**里 ⇒ 查到的绑定不是当初那个 ✗（试过，数据漂了）
 * 而这里读的是**节点上那些已经定死的层号** ⇒ 跟作用域无关 ✓
 * 取上界：只可能**更松** ⇒ 只可能把误拒变成通过 ✓ */
/* ⭐ 重算只允许用在**"深度由解算器决定"的形状**上 ✗✗
 *
 * 为什么：重算能把 `rc->depth` 调小（只能变宽松），而**绑定**（`EX_IDENT`）的深度
 * 是 `check_stmt` 在当时的作用域里算好的，跟解算无关 ✗
 * 真踩到：`tests/errors/generic_return_local.extc`（泛型体 `return local`，`T = slice<u8>`）
 *   模板期冻下 `depth=1`（对的），而重算对那个绑定答了 **0**
 *   （它的站点当时还是"未定"的哨兵态 —— 而那个哨兵态的 `refDepth` 就是 0 ✗）
 *   ⇒ 实例复查把它放过了 ⇒ **真的悬垂** ✗✗
 *
 * 正确判据：只有当这个值的深度**真的由某个分配站点决定**时，
 * 重算才是权威（那正是当初需要它的理由：解算会把站点从家放回块层 ✓）。
 * 其余形状（绑定 / 调用结果 / 参数派生）一律**不下调** —— 保守方向 = 宁可误拒 ✓ */
static bool depthComesFromAlloc2(Checker *c, Expr *e, int hops) {
    if (!e || hops > 32) return false;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL: return true;
    /* ⭐ **跟着来路走** —— `var v = { buf: new T[cap], … }  return v`：
     * 报错那一刻 `rc->val` 是 `v`（一个**绑定**，`kind=4`），
     * 而它的深度正是由它来路里那个 `new` 决定的 ✗
     * （实测：`[at] … return value kind=4 dca=0 svd=0` 而 `depth=1` ⇒ 误拒 ✗）*/
    case EX_IDENT: {
        /* ⚠️ **不能用 `lookup`**：收尾 pass 里作用域已经弹了 ⇒ 找不到（真踩到：
         * 改完还是 `dca=0`）⇒ 用**解析那一刻钉在节点上的绑定**（`IdentBinding`）✓ */
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
    default: return false;   /* 绑定 / 字段 / 调用结果 / 解引用 ⇒ **不算** ✓ */
    }
}

static int solvedValDepth(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_NEW: case EX_GENCALL:
        /* ⚠️ 不许再“取 refDepth”当启发 —— 那个数可能被污染，会跟层号矛盾 ✗（§F.2 #2）*/
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

static void checkFunc(Checker *c, FuncDef *f) {
    /* ⭐ 定案 72：外部声明**没有体** ⇒ 只查签名（参数类型在别处已经解析过 ✓）*/
    if (f->isExtern) {
        f->mayUseArena = false;
        f->needsHome   = false;
        /* ⚠️ 形状限制（`LIBS.md` §5 的映射表）：跨边界只用**标量或单个指针** ✓
         * 为什么：`slice<T>` 在 C 那边是**两个**参数（ptr + len）⇒ 名字对不上 ✗
         * （`memcpy` 那种 `(dst, src, n)` 就写三个参数；slice 版的 wrapper 用 extC 写在
         *   stdlib 里，把 `s.data` / `s.len` 分别传下去 ✓）*/
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
        collectEffects(c, f);            /* 摘要 = 签字（或最坏情况）✓ */
        return;
    }
    FuncDef *savedFunc = c->curFunc;
    /* A3：**有分配 + 返回有用的东西（含引用/视图）** ⇒ 这个函数要一只"家"arena ✓
     * （返回 `i32` 的函数不要 —— 它的分配留在自己块里，A2 的紧致性不丢 ✓）*/
    if (!f->isExtern && stmtHasNew(f->body) &&
        ((f->ret && typeContainsRef(c->tt, f->ret)) || stmtStoresThroughDeref(c, f->body, f)))
        f->needsHome = true;

    Vec     *savedParams = c->curParams;

    c->curFunc = f;
    computeEscapes(c, f);   /* ⭐ 档1：**必须**在检查函数体**之前**算 E ——
                             * 选家 arena 时要靠它（放晚了就等于没算 ✗ 踩过）*/
    c->curParams = funcTParams(f);
    /* ⭐ 定案 68：这一遍查体时顺手记下所有"等闭包之后再定"的节点 ✓ */
    vecInit(&c->curArenaSites, c->arena, sizeof(Expr *));
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

    collectEffects(c, f);      /* 档1 步骤 1.1：算效果摘要（**只算不用**，行为零变化）✓ */
    f->arenaSites = c->curArenaSites;   /* ⭐ 定案 68：交给闭包之后的那个 pass ✓ */
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
        s->modName = g->modName;          /* ⭐ 定案 70 ✓ */
        *(Sym **)vecPush(&c->globals) = s;
    }
}

/* ⭐ PLAN #47：**自由函数实例**（`fn f<T>` + 实参类型 ⇒ 一份具体函数）✓
 *
 * 为什么实例要"出生"在调用点：类型实例（`varArray<i32>`）是**类型**决定的，
 * 而自由函数的实参只看调用点 ⇒ 只有那里知道 `T` 到底是什么 ✓
 *
 * 实例是**独立的 FuncDef**：`tmpl` 指回模板、`targs`/`instName` 填好、
 * 参数与返回类型**代入过** ⇒ codegen 当普通函数吐（只是进实例时要开替换 ✓）*/
#define FUNC_INST_PREFIX "__extc_fi_"

FuncDef *funcInstance(Checker *c, FuncDef *tmpl, Vec *targs, int line) {
    if (!tmpl) return NULL;
    /* 已经造过？（同一个模板 + 同一套实参 ⇒ 一份 ✓）*/
    for (size_t i = 0; i < c->funcInsts.len; i++) {
        FuncDef *in = *(FuncDef **)vecAt(&c->funcInsts, i);
        if (in->tmpl != tmpl || in->targs.len != targs->len) continue;
        bool same = true;
        for (size_t j = 0; j < targs->len; j++)
            if (!ttEquals(*(Type **)vecAt(&in->targs, j), *(Type **)vecAt(targs, j))) { same = false; break; }
        if (same) return in;
    }
    FuncDef *in = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    *in = *tmpl;                        /* 浅拷贝：**函数体共用** ✓（跟方法实例一个道理 ✓）*/
    in->tmpl = tmpl;
    in->used = false;
    vecInit(&in->targs, c->arena, sizeof(void *));
    for (size_t j = 0; j < targs->len; j++)
        *(Type **)vecPush(&in->targs) = *(Type **)vecAt(targs, j);
    /* 参数 / 返回类型代入 ✓
     * ⚠️⚠️ 参数必须**复制一份**（`Param*` 原来跟模板共享 ⇒ 改类型会把模板也改掉 ✗✗
     *   结果就是第二次推导时模板已经是具体类型了 —— 静默错乱，实测段错误 ✓）*/
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
    /* C 名字：`max_i32`（拿实参 mangle 拼 ✓）*/
    Buf b;
    bufInit(&b, c->arena);
    bufPuts(&b, tmpl->name);
    for (size_t j = 0; j < targs->len; j++) {
        bufPutc(&b, '_');
        bufPuts(&b, ttMangle(c->tt, *(Type **)vecAt(targs, j)));
    }
    in->instName = bufCstr(&b);
    in->typeParams.len = 0;             /* 实例没有类型参数了 ✓ */
    *(FuncDef **)vecPush(&c->funcInsts) = in;
    *(FuncDef **)vecPush(&c->m->funcs) = in;   /* codegen 从这里吐定义 ✓ */
    (void)line;
    return in;
}

/* 类型合一：把形参类型里的类型参数**填**进 `targs`（`slice<T>` vs `slice<u8>` ✓）
 * 返回 false = 推不出来（比如 `T` 只出现在返回类型 ⇒ 得写显式实参 `f<i32>(…)`）✓ */
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
    if (ttHasParam(want)) return false;   /* 形参里还有 T 但两边形状对不上 ⇒ 推不出来 ✓ */
    return true;
}

/* ⭐ PLAN #47：这两批"推迟到实例化"的复查，现在**类型实例与自由函数实例共用** ✓
 * 抽成函数就是为了让 `fn f<T>` 的实例也走同一条路，而不是复制一份 ✗
 * 调用者负责先把 `c.substParams/substArgs` 设成这个实例的 ✓ */
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

/* 这条 RefCheck 属于**这个自由函数实例**吗？（方法走类型实例那条 ✓）*/
static bool refCheckApplies(RefCheck *rc, FuncDef *fi) {
    if (!fi || !fi->tmpl) return false;
    if (rc->func != fi->tmpl) return false;
    return fi->tmpl->typeParams.len > 0;
}

static void runRefCheck(Checker *c, RefCheck *rc, const char *instName) {
    TypeTable *tt = c->tt;
        /* 这个实例里那个 `T` 到底含不含引用？不含 ⇒ 这条规矩本来就不适用 ✓
         * （所以 `box<i64>::set` 照样合法，而 `boxT<slice<u8>>::stash` 会被挡住 ——
         *   这正是"不能简单地把 `T` 一律当成含引用"的原因）*/
        if (rc->isNewSize) {
            /* 实例化之后还带类型参数（或 void）⇒ 大小仍然不知道 ⇒ 报错点名实例 ✓ */
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
            /* 零值这一类：这个实例的 `T` 到底有没有零值？*/
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

        /* ⭐ PLAN #42(c)：同一条道理 —— 从没被调用过的函数不用按实例复查 ✓ */
        if (rc->func && !rc->func->used) return;
        Type *vt = tsub(c, rc->val->type);
        c->substParams = NULL;
        c->substArgs   = NULL;
        if (!typeContainsRef(tt, vt)) return;

        /* ⭐ 用"解算完之后"的层号重算一次（取 ≤：只可能更紧 ✓）*/
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
                   && !valTracesToParam(c, rc->func, rc->val)) {   /* ⭐ 定案 67 ✓ */
            ckError(c, rc->line,
                    "A generic body is checked once on the template, where `T` is"
                    " opaque -- so the reference rules are re-checked for every"
                    " concrete instance.",
                    "in instance `%s`: cannot store a borrowed value into something"
                    " that outlives this call", instName);
        }
}

/* ⭐ PLAN #50：把一条推迟的调用点按**当前实例的代入**重解成具体实例 ✓
 *
 * `params`/`targs` = 外层实例的类型参数与实参 ✓
 * 做三件事：代入类型实参 → 造/取那个具体实例 → 把 `e->func` 改指过去 ✓
 * 实例是驻留的（`funcInstance` 自己查重）⇒ 同一个实参组合只会有一份 ✓ */
static void resolveDeferredCall(Checker *c, CallCheck *cc, Vec *params, Vec *targs) {
    if (!targs || !params) return;
    Vec concrete;
    vecInit(&concrete, c->arena, sizeof(void *));
    for (size_t i = 0; i < cc->targs.len; i++) {
        Type *a = *(Type **)vecAt(&cc->targs, i);
        /* ⚠️ `a` 可能是 NULL（推导没填上的位）；`ttSubstitute` 对 NULL 不保证安全 ⇒
         * 先挡掉：推不出来在模板期就该报过了，这里当"这个实例没准备好"跳过 ✓ */
        if (!a) return;
        Type *sub = ttSubstitute(c->tt, a, params, targs);
        if (!sub || ttHasParam(sub)) return;  /* 还带 `T` ⇒ 外层自己也是模板体，等更外层 ✓ */
        *(Type **)vecPush(&concrete) = sub;
    }
    FuncDef *inst = funcInstance(c, cc->tmpl, &concrete, cc->node->line);
    if (!inst) return;
    inst->used = true;
    cc->node->func = inst;
}

bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    Checker c;    memset(&c, 0, sizeof c);
    vecInit(&c.funcInsts, arena, sizeof(void *));   /* PLAN #47 ✓ */
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
    vecInit(&c.callChecks, arena, sizeof(void *));   /* ⭐ PLAN #50：推迟的调用点 ✓ */
    vecInit(&c.narrowMarks, arena, sizeof(size_t));
    vecInit(&c.eSites, arena, sizeof(EArenaSite *));   /* ⭐ B4′：E 敏感的 arena 决策记录 ✓ */
    vecInit(&c.lvlFacts, arena, sizeof(LvlFact *));    /* ⭐ 长运行内存："被存到第 k 层"的事实表 ✓ */
    /* ⚠️ R1（反例库 R1a/R1b）：`curArenaSites` 以前**只在 `checkFunc` 里** `vecInit`，
     * 而 `checkGlobals` 跑在第一个 `checkFunc` **之前** ⇒ 顶层初始化式里只要有
     * `new`（`var G = new i32`）或有家调用（`let G = helper()`），
     * `vecPush(&c->curArenaSites, …)` 就落到一个**没初始化**的 Vec 上
     * ⇒ `vecGrow` → `arenaAlloc(NULL)` ⇒ **SIGSEGV**（要的是"全局只能用常量初始化"那句报错）✗
     * ⇒ 在这里（任何 pass 之前）就初始化好 ✓ */
    vecInit(&c.curArenaSites, arena, sizeof(Expr *));

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
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (fx->tmpl) continue;              /* 实例不单独查 ✓ */
        checkMethodShape(&c, fx);
    }

    /* 第二遍：检查函数体 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *fx = *(FuncDef **)vecAt(&m->funcs, i);
        if (fx->tmpl) continue;              /* ⭐ PLAN #47：实例不单独查（模板 + 实例复查 ✓）*/
        checkFunc(&c, fx);
    }

    /* 推迟的 `==` 检查：对每个具体实例复查一遍。
     * 这是「不引入 trait」的代价 —— 错误晚到这里，但信息要说清是哪个实例。 */
    for (size_t i = 0; i < c.eqChecks.len; i++) {
        EqCheck *ec = *(EqCheck **)vecAt(&c.eqChecks, i);
        /* ⭐ PLAN #42(c)：这条 `==` 所在的函数**从没被调用过** ⇒ 它的实例不可能执行
         * ⇒ 不用按实例复查 ✓（也就不用逼用户给无关键写 `fn ==` ✗）*/
        if (ec->func && !ec->func->used) continue;
        /* ① 类型实例（方法上的 `T: ==`）—— 自由函数没有 owner ⇒ 跳过 ✓ */
        if (!ec->owner) goto eq_done;
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != ec->owner) continue;
            runEqCheck(&c, ec, &ec->owner->typeParams, &inst->targs, inst->name);
        }
        /* ② ⭐ PLAN #47：**自由函数实例**（`fn f<T>` 里的 `T: ==`）*/
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (ec->func != fi->tmpl) continue;
            runEqCheck(&c, ec, &fi->tmpl->typeParams, &fi->targs, fi->instName);
        }
    eq_done: ;
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
        /* ⭐ PLAN #47：自由函数实例也要复查（同样的规矩，只是 `T` 来自函数自己的形参表 ✓）*/
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (!refCheckApplies(rc, fi)) continue;
            c.substParams = &fi->tmpl->typeParams;
            c.substArgs   = &fi->targs;
            runRefCheck(&c, rc, fi->instName);
        }
        /* ② 类型实例（**原有那条路** ✓）：方法上的 `T` 由所属 struct 的实参决定 ✓ */
        if (!owner) continue;                    /* 自由函数模板没有 owner ⇒ 上面那条管 ✓ */
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;

            c.substParams = &owner->typeParams;
            c.substArgs   = &inst->targs;
            runRefCheck(&c, rc, inst->name);
        }
    }

    /* ⭐ PLAN #50：把一条推迟的调用点按**当前实例的代入**重解成具体实例 ✓
     *
     * `params`/`targs` = 外层实例的类型参数与实参（`c.substParams/substArgs` 也设成它们）✓
     * 做三件事：代入类型实参 → 造/取那个具体实例 → 把 `e->func` 改指过去 ✓
     * 静态实例是驻留的（`funcInstance` 自己查重）⇒ 同一个实参组合只会有一份 ✓ */
    for (size_t i = 0; i < c.callChecks.len; i++) {
        CallCheck *cc = *(CallCheck **)vecAt(&c.callChecks, i);
        if (!cc->node || !cc->tmpl) continue;
        /* ① 外层是**自由函数**实例 */
        for (size_t j = 0; j < c.funcInsts.len; j++) {
            FuncDef *fi = *(FuncDef **)vecAt(&c.funcInsts, j);
            if (fi->tmpl != cc->func) continue;       /* 不是这个模板体里的调用 ✗ */
            resolveDeferredCall(&c, cc, &fi->tmpl->typeParams, &fi->targs);
        }
        /* ② 外层是**类型**实例（方法里的泛型调用）—— `owner` 是 struct/枚举定义 ✓ */
        StructDef *owner = cc->func ? cc->func->owner : NULL;
        if (!owner) continue;
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != owner) continue;
            resolveDeferredCall(&c, cc, &owner->typeParams, &inst->targs);
        }
    }

    /* ---- ⭐ PLAN #44：**效果摘要按实例重算** ----
     *
     * 摘要原来只在模板上算一份（`checkFunc` 里那次），那时 `T` 不透明 ⇒ `carrier` 判据
     * 只能补一句 `mentionsParam`：「提到 `T` 就当它带引用」✗ ⇒ `varArray<i32>`
     * 这种**完全不带引用**的实例也背着 `Cont` 位 ✓（精度损失，不是错）
     *
     * 这里在**每个实例的代入上下文里**（`c.substParams/substArgs` 设好）重算一遍 ——
     * `carrier` 那行现在走 `tsub(c, e->type)`，所以 `T = i32` 时 `carrier` 为假
     * ⇒ 那一位**不置位** ✓；`T = slice<u8>` 时照旧置位 ✓（**安全性靠后者**）
     *
     * ⚠️⚠️ **绝不能**用"把 `mentionsParam` 那一句删掉"来代替这里 ——
     * `PLAN.md` 里实测过：删掉之后 `tests/errors/generic_borrowed_store.extc`
     * **从"被挡"变成"通过"**（真悬垂）✗ 那句是**堵洞的**，不是多余的保守 ✓
     *
     * ⚠️ 重算前必须清空：`*in = *tmpl` 是浅拷贝 ⇒ 实例一开始**继承**了模板的摘要，
     * 而且 `callees` 也是从模板带过来的（`collectEffects` 对非 NULL 的 `callees` 不重置 ✗）
     * ⇒ 不清就会叶子翻倍、`effState` 还是"算完了" ⇒ 传递闭包拿到的是模板的旧账 ✗ */
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

    /* ---- A3：`needsHome` 的**传递闭包** ----
     * 调用一个"有家"的函数时，调用者必须**有东西可传**（`__extc_home` 或
     * `&__extc_a[当前块]`）⇒ 调用者自己也得收一只家 arena ✓
     * 到不动点为止（函数不多，直接多跑几轮）✓ */
    for (bool changed = true; changed; ) {
        changed = false;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
            if (f->needsHome || f->isExtern || !f->body) continue;   /* extern 没有体 ✓ */
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

    /* ⭐ 定案 68（2026-09-22 主人拍板：「checker 操作完之后应该把全部生成信息给 codegen，
     * codegen 就不用再做校验」）—— **层号的唯一权威**：
     *
     * 查体的时候 `needsHome` 只有**直接判据**（体里有 `new` + 返回含引用），而"因为调用了
     * 有家函数才有家"这一半要等上面那个传递闭包 ✗ ⇒ 那时算出来的 `arenaLevel` 是**块层**，
     * 而生成的 C 按"有家"分配（家更长命 ⇒ 只会更安全 ⇒ 以前没有洞，但**有两个权威**）✗
     * （PLAN #38 的 golden 就是这么撞出来的：`out-param` 的分配从家 arena 掉回块 arena ✗）
     *
     * 现在：闭包跑完之后，把"有家"函数里**每一处 `new`** 都改写成 `ARENA_HOME` ✓
     * ⇒ codegen 的 `arenaRefAt` 不再看 `g->hasHome`（它只翻译检查器给的数）✓✓ */
    {
        Vec all; vecInit(&all, arena, sizeof(FuncDef *));
        for (size_t i = 0; i < m->funcs.len; i++)
            *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t i = 0; i < m->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                *(FuncDef **)vecPush(&all) = *(FuncDef **)vecAt(&sd->methods, j);
        }
        /* ⭐ B5（ARENA-SOUNDNESS §9 档 1）：**统一重算 `needsHome` 的"直接判据"** ——
         * `checkFunc` 里那条判据（体里有分配 + 返回含引用的类型）只在**查那个函数体时**算，
         * 而**泛型实例的体是不单独查的**（`checkFunc` 对 `fx->tmpl` 直接 `continue`）✗
         * ⇒ 实例的 `needsHome` 只是创建时的浅拷贝（`*in = *tmpl`，见 `funcInstance`）
         * ⇒ **谁先被创建就继承谁的值** ⇒ 同一个程序换个声明顺序，结果不一样：
         *     · 调用者先出现 ⇒ 实例先创建（那时模板还没查）⇒ `needsHome=false`
         *       ⇒ codegen 不吐 `__extc_home` 参数，而模板的 `new` 已被改成
         *         `(*__extc_home)` ⇒ **生成的 C 编不过**（gcc: `__extc_home` undeclared）✗
         *     · 模板先出现 ⇒ 正常 ✓
         * ⇒ 在闭包**之后**（那时所有体都查完了、`arenaSites` 也定了）对**每个**函数
         *   （含实例、方法）用**代入后的返回类型**重算一次 ⇒ 顺序不再影响结果 ✓
         * ⚠️ 只重算"直接判据"；"因为调用了有家函数"那一半由上面的闭包负责 ✓
         * ⚠️ 幂等：`|=` 语义（只置真、不置假）⇒ 不会把闭包算出来的结果抹掉 ✓ */
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

        /* ⭐ B4′（架构：**分析最后写、检查只读**）：E 敏感的"家 arena"决策**重算一遍** ——
         * 查体时 `computeEscapes` 看不到被调者的效果摘要（那时还没封闭）
         * ⇒ `pushOne(l, 7)` 里的 `l` 没被标成逃逸（尽管后面 `sink(out, l)` 会把它发布出去）
         * ⇒ 家 arena = 调用者自己的帧 ⇒ 被调者塞进容器的东西，本次调用返回即 release ✗
         * ⇒ 现在摘封闭合了，重新算 E（`computeEscapes` 会读 `callsNeedsHome`/摘要），
         *    再把那些站点的 `arenaArg` 按新 E 更新一次 ✓ */
        /* ⚠️ 跑**三轮**：`computeEscapes` 自己的不动点是"以**当时**的 `isEscapeeName`
         * 为真值"判的，而它标记出来的新名字又要靠**别的**调用点才能传播
         * ⇒ 一轮不够（实测：第一轮的 E 还是空的 ✗）。跑几轮把它推到不动点 ✓
         * （函数不多，成本可忽略；`computeEscapes` 内部也有 32 轮的上限 ✓）*/
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
        /* ⚠️ 用**所有函数 E 的并集**判 —— `c.escapees` 是"当前函数"的 E（每函数重建），
         * 而收尾时我们要问的是"这个名字在**它所属的那个函数**里会不会被搬出去" ✗
         * ⇒ 保守地取并集：任何函数里被判为逃逸的名字，都当成可能逃逸 ✓
         * （方向安全：多算只会让家 arena 选得更长寿 ⇒ 多费内存，不放过 ✓）*/
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            if (f->isExtern || !f->body) continue;
            computeEscapesMode(&c, f, true);   /* ⭐ 并集模式（不清空）✓ */
        }
        int eFixed = 0;
        for (size_t i = 0; i < c.eSites.len; i++) {
            EArenaSite *rec = *(EArenaSite **)vecAt(&c.eSites, i);
            if (!rec || !rec->call) continue;
            if (rec->overflow) {                       /* 记不全 ⇒ 传家 arena（永远 sound）✓ */
                rec->call->arenaArg = ARENA_HOME;
                rec->call->arenaArgPending = false;
                continue;
            }
            int nd = 0;
            for (int k = 0; k < rec->n; k++) {
                int d = rec->argDepth[k];          /* 0 = 参数/全局（本来就活在帧外）✓ */
                /* 只对"实参是本帧局部"的那几档看 E（0 那一档已经是 -1 了）✓ */
                if (d != 0 && rec->argRoot[k] && isEscapeeName(&c, rec->argRoot[k])) d = -1;
                if (nd == 0 || d < nd) nd = d;
            }
            if (nd == 0 && rec->n > 0) nd = rec->n ? rec->argDepth[0] : 0;
            /* 把新的 homeDepth 落到 arenaArg 上（与 `setCallArenaArg` 同一套规则 ✓）*/
            int want = (nd <= 0) ? ARENA_HOME : nd;
            if (rec->call->arenaArg != want) { rec->call->arenaArg = want; eFixed++; }
            rec->call->arenaArgPending = false;
        }

        /* ⭐⭐ 长运行内存：**把"被存到第 k 层"的事实表重放到不动点** ✓
         *
         * 为什么要反复：一条事实只能推动**它碰到的那些站点**，而那些站点
         * 变长寿之后又会让**别的**事实推得动 ⇒ 得轮到没变化为止 ✓
         * （跟 `EArenaSite` 那个"跑四轮"、`needsHome` 的传递闭包是**同一件事** ✓）
         *
         * 为什么这样就对：层号 = **越小活得越久**，而一个站点的最终层号
         * = **它碰到的所有约束里最小的那个**。`promoteInto` 只会往小改
         * （只会拉长寿命）⇒ 重放是**单调**的 ⇒ 一定收敛，而且收敛到的就是
         * 那个最小值 ✓（上限 32 轮 = 与它自己的环保护同一个数 ✓）*/
        int solveRounds = 0;
        c.lvlSolving = true;                     /* ⭐ 重放期不许再记账 ✗（真踩到过）*/
        size_t nFacts = c.lvlFacts.len;          /* 快照：事实表在解算期**不变** ✓ */
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
            fprintf(stderr, "[lvl] 事实 %zu 条，重放 %d 轮收敛\n",
                    c.lvlFacts.len, solveRounds);

        int fixed = 0, keptBlock = 0;
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind == EX_NEW || site->kind == EX_GENCALL) {
                    /* ⭐ 解算完了 ⇒ 现在**唯一权威**地定它的去处 ✓
                     *   · `escaped`（= 无论哪条路径碰到过它，那条路径都说"要活到帧外"）
                     *     ⇒ 进家 arena（家 = 调用者选的那只）✓
                     *   · 否则 ⇒ **留在解出来的那层块口袋	extbf{（循环里 = 每轮回收 ✓）}
                     * ⚠️ 以前这里是**无条件**兜底："有家函数里每个 `new` 都进家"
                     *   ⇒ 那些**根本没逃出去**的每轮临时对象也被钉在家，到帧结束才回收 ✗✗
                     *   实测：主人的 50/25/25 形状 2e6 轮 **98 MB**；同形状容器内联写只要 50 MB ✗ */
                    if (getenv("EXTC_DBG_SITE2"))
                        fprintf(stderr, "[site2] %-10s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                site->arenaLevel, f->needsHome?1:0, (int)site->kind);
                    if (getenv("EXTC_DBG_S3"))
                        fprintf(stderr, "[s3] %-8s minAt=%d lexi=%d arena=%d home=%d kind=%d\n",
                                f->name?f->name:"?", site->minAt, site->lexicalLevel,
                                site->arenaLevel, f->needsHome?1:0, (int)site->kind);
                    int want;                            /* ⭐ 它最终该住哪只口袋 ✓ */
                    if (site->minAt == 0) {
                        want = ARENA_HOME;               /* "要活到帧外" ⇒ 家 arena ✓ */
                    } else if (site->minAt >= 1) {
                        want = site->minAt;              /* "活到第 k 层" ⇒ 第 k 层块口袋 ✓ */
                    } else {
                        /* 没被任何约束碰过 ⇒ 用它自己那层 ✓
                         * ⚠️ 不能看 `arenaLevel`：预负已经把有家函数里的都改成了哨兵 ✗ */
                        want = site->lexicalLevel >= 1 ? site->lexicalLevel : 1;
                    }
                    if (site->arenaLevel != want) {
                        site->arenaLevel = want;
                        site->refDepth   = arenaDepthOf(want);   /* ⭐ 同上，唯一换算处 ✓ */
                        if (want == ARENA_HOME) fixed++; else keptBlock++;
                    }
                } else if (site->arenaArgPending) {
                    /* 调用点：pending 的那一档 ⇒ "有家传家" ✓
                     * ⚠️ 这一档**不能**跟着上面改窄 —— `arenaArg` 是"给被调者的 arena"，
                     *   被调者可能把东西存进**调用者的容器**（那容器活得比本帧久）
                     *   ⇒ "本帧没逃出去"完全不能作为依据 ✗ */
                    if (f->needsHome) {
                        site->arenaArg = ARENA_HOME;
                        site->arenaArgPending = false;
                        fixed++;
                    }
                }
            }
        }
        if (getenv("EXTC_DUMP_LVL"))
            fprintf(stderr, "[lvl] 定案：%d 处进家 / %d 处留块层\n", fixed, keptBlock);

        /* ⭐⭐ 层 2：**每个站点的 `refDepth` 都要按最终层号重算一遍**（不管层号有没有变）✗✗
         *
         * 为什么（gdb 实测，`tests/arena-promoted/C2_if_join_refbinding`）：
         * 上面那个定案循环里那句 `site->refDepth = …` 是**包在 `if (site->arenaLevel != want)`
         * 里面**的 —— 而"预负已经把它置成 `ARENA_HOME`、解算又定成 `ARENA_HOME`"的站点
         * **层号根本没变** ⇒ 那句话**一次都不执行** ⇒ `refDepth` 停在**中途被改坏的值**上 ✗
         *   实测：`alloc<i32>(1)` 的站点 `refDepth` 是 **0**（= "要活到帧外"），
         *   而同一个站点按层号算出来是 **1** ⇒ 两件事**自己跟自己矛盾** ⇒
         *   用户拿到的报错是 "borrowed from depth 0"（说不通：明明在块层）✗
         *
         * 铁律（本来就是设计口径）：**`refDepth` 与 `arenaLevel` 必须是同一个数的两种说法** ✓
         * ⇒ 定案之后无条件同步一次 ✓ */
        for (size_t i = 0; i < all.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&all, i);
            for (size_t j = 0; j < f->arenaSites.len; j++) {
                Expr *site = *(Expr **)vecAt(&f->arenaSites, j);
                if (site->kind != EX_NEW && site->kind != EX_GENCALL) continue;
                site->refDepth = arenaDepthOf(site->arenaLevel);   /* ⭐ 同上 ✓ */
            }
        }

        /* ⭐ 解算完了 ⇒ **把"推迟到实例化再查"的那些深度重算一遍** ✓
         * （它们冻的是**解算之前**的数 —— 而定案可能把站点从家放回了块层 ✗）
         * 实测：`varArray<T>::withCap` 的 `return v` 在模板那一轮冻下 `depth=1`，
         *   而它里面那个 `new T[cap]` 已经被正确地放在家 arena（深度 0）
         *   ⇒ 实例复查报 "depth 1, but this can only hold up to 0"，而真相是 0 ✗✗
         * ⚠️ 只对 `depthComesFromAlloc` 的形状重算，而且**只下调** ✓
         *   （对绑定重算会把 `generic_return_local` 那种该拒的放过 ✗ —— 真踩到过）*/
        for (size_t i = 0; i < c.refChecks.len; i++) {
            RefCheck *rc = *(RefCheck **)vecAt(&c.refChecks, i);
            if (!rc || !rc->val) continue;
            if (!depthComesFromAlloc2(&c, rc->val, 0)) continue;
            /* ⭐ **无条件重算**（不是只下调）—— 因为"解算前冻的那个数"可能**偏低**：
             * 模板那一轮 `new T[cap]` 的 `refDepth` 还是哨兵态的 0，而解算后它可能是 1 ✗
             * （实测：`container-of-view` 的 `[post] … frozen=0 … now=1`）
             * 只对 `depthComesFromAlloc` 的形状做，所以绑定（参数派生/调用结果）不会被放松 ✓ */
            rc->depth = solvedValDepth(rc->val);
        }
        if (getenv("EXTC_DUMP_OW"))
            fprintf(stderr, "[arena] 唯一权威：%d 处（`new` / 调用点）落到了家 arena ✓\n", fixed);
    }

    /* ---- ⭐ 定案 65：`@overwrite` 的站点数 + 格子该放哪 ----
     * 必须在**所有函数体都查完**之后（"能不能调到自己"要看调用图 ✓）*/
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
            /* ⚠️ 没有站点也要把 `owLocal` 定成 true ✗ —— 否则"无参数无家"的函数签名会
             * 少掉那个 `void`（golden 当场抓出来的 ✓）*/
            /* ⭐ F 族（ARENA-SOUNDNESS §10）：格子**永远住本帧** ✓ */
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

    /* ---- ⭐ 编译时长优化：算"会不会往**自己的块 arena** 里放东西"（`mayUseArena`）
     * 必须在上面那个闭包**跑完之后**算 ✓ 判据：体里有 `new`，或调用了"有家"的函数 ✓
     * 不会 ⇒ codegen 连 arena 数组和 release 都省掉（生成 C 约 −20% 行，见压测量）✓
     * ⚠️ `main` 恒为真（它是根"家"，`__extc_home = &__extc_a[1]` 需要那只数组）✓ */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f->isExtern || !f->body) { f->mayUseArena = false; continue; }   /* 定案 72 ✓ */
        f->mayUseArena = stmtHasNew(f->body) || callsNeedsHome(f->body);
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *f = *(FuncDef **)vecAt(&sd->methods, j);
            f->mayUseArena = stmtHasNew(f->body) || callsNeedsHome(f->body);
        }
    }

    return !ctx->hasError;
}

/* ⭐ 甲′（PLAN #31/#33）：**这个函数（传递地）会不会分配？**
 *
 * 跟 `needsHome` 的传递闭包是同一个问题，但要**惰性**算：
 * 那个闭包是所有函数查完之后才跑的，而调用者在查**自己**的函数体时就得知道
 * "这一刀会不会往我的容器里塞新存储" ✗（`varArray::push` 自己不 `new`，
 * 是它调的 `grow` 才有 ⇒ 只看 `f->needsHome` 会漏掉"转发一层"的 callee）✓
 *
 * 环保护：**正在算**的当"会"（保守方向）✓ 结果缓存在 `FuncDef.allocState` ✓
 */
static bool stmtCallsAllocator(Checker *c, Stmt *s);   /* 定义在下面（互相递归）*/
static bool funcAllocates(Checker *c, FuncDef *f) {
    if (!f) return true;                     /* 拿不准 ⇒ 当"会" ✓ */
    if (f->isExtern) return false;           /* 外部函数不碰我们的 arena ✓ */
    if (f->allocState == 1) return true;
    if (f->allocState == 2) return false;
    if (f->allocState == 3) return true;     /* 环上 ⇒ 保守 */
    f->allocState = 3;
    bool r = stmtHasNew(f->body) || stmtCallsAllocator(c, f->body);
    f->allocState = r ? 1 : 2;
    return r;
}

/* 体里有没有调用"有家"的函数？（检查完之后 `e->func` 已经填好了 ✓）*/
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

