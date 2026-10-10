/* Construction of AST nodes, plus the one structural predicate that needs nothing
 * but the tree itself.
 *
 * Every constructor takes a zeroed node from the arena and sets only the kind and
 * the line number, so a field the parser never fills in is zero rather than
 * indeterminate.
 */

#include "ast.h"

#include <string.h>

/**
 * @brief 造一个未解析的类型名（TY_UNRESOLVED）。
 *
 * @param a     拥有节点的 arena。
 * @param name  源码里写的类型名；指针原样保留，文本不拷贝。
 * @return      一个 TY_UNRESOLVED 类型。把它连同解析器随后挂上的类型实参一起解析成驻留类型
 *              是检查器的事。
 */
Type *typeNamed(Arena *a, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_UNRESOLVED;
    t->name = name;
    return t;
}

/**
 * @brief 造 `ref inner`，普通的引用类型。
 *
 * @param a      拥有节点的 arena。
 * @param inner  被引用的类型。
 * @return       既不 writable 也不 nullable 的 TY_REF；源码写 `mut ref` / `?ref` 时由解析器
 *               设置 `mut` / `nullable`。
 */
Type *typeRef(Arena *a, Type *inner) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/**
 * @brief 造定长数组类型 `[n]elem`（TY_ARRAY）。
 *
 * 长度是编译期常量，也是类型的一部分，所以 `[3]i32` 与 `[4]i32` 是两个不同的类型。
 *
 * @param a     拥有节点的 arena。
 * @param n     元素个数。
 * @param elem  元素类型。
 * @return      一个 TY_ARRAY 类型。
 */
Type *typeArray(Arena *a, int64_t n, Type *elem) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_ARRAY;
    t->asize = n;
    t->inner = elem;
    return t;
}

/**
 * @brief 按解析器给的碎片造函数类型 `fn(params...) -> ret`（TY_FN）。
 *
 * 参数与返回类型可能还是 `TY_UNRESOLVED` 名字，所以这里只搭形状：驻留发生在 `ttResolve` /
 * `ttFn` 里（the split that keeps `ast.c`'s constructors out of the type table）。
 *
 * @param a       拥有节点与参数向量的 arena。
 * @param params  参数类型，按顺序（`Type *` 元素）；向量会被复制。
 * @param ret     返回类型；不能是 NULL，`void` 也要传一个真正的类型。
 * @return        一个 TY_FN 类型，它的各部分可能仍是未解析的名字。
 */
Type *typeFn(Arena *a, Vec *params, Type *ret) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_FN;
    vecInit(&t->params, a, sizeof(void *));
    for (size_t i = 0; i < params->len; i++)
        *(Type **)vecPush(&t->params) = *(Type **)vecAt(params, i);
    t->ret = ret;
    return t;
}

/**
 * @brief 造一个类型参数（TY_PARAM）。
 *
 * `param` 与 `name` 都记这个名字；判类型相等、打印类型都按 `param` 认它。
 *
 * @param a     拥有节点的 arena。
 * @param name  参数名，例如 `"T"`。
 * @param idx   它在所在参数表里的位置。
 * @return      一个 TY_PARAM 类型。
 */
Type *typeParam(Arena *a, const char *name, int idx) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = TY_PARAM;
    t->param = name;
    t->name = name;
    t->tpIndex = idx;
    return t;
}

/**
 * @brief 造一个表达式节点。
 *
 * 节点取自 arena 且已清零，只设 `kind` 与行号，所以解析器没填的字段是零而不是未定值。解析的
 * 结果（`type`、`func`、`field`，以及逃逸分析的深度）随后由检查器填。
 *
 * @param a     拥有节点的 arena。
 * @param kind  表达式种类；联合体只有对应那个成员可用。
 * @param line  源码行号（1 起），留给诊断。
 * @return      清零的 Expr。
 */
Expr *exprNew(Arena *a, ExprKind kind, int line) {
    Expr *e = (Expr *)arenaAllocZero(a, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    return e;
}

/**
 * @brief 造一个语句节点。
 *
 * 节点取自 arena 且已清零，只设 `kind` 与行号。
 *
 * @param a     拥有节点的 arena。
 * @param kind  语句种类；联合体只有对应那个成员可用。
 * @param line  源码行号（1 起），留给诊断。
 * @return      清零的 Stmt。
 */
Stmt *stmtNew(Arena *a, StmtKind kind, int line) {
    Stmt *s = (Stmt *)arenaAllocZero(a, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

/**
 * @brief 准备一个空模块。
 *
 * 声明随着解析器与加载器发现它们而追加，所以一个 Module 最终可以按序装着 prelude、用户的
 * 文件、然后是该文件导入的每个模块。
 *
 * @param m  要初始化的模块；存储由调用方拥有。
 * @param a  向量分配所用的 arena。
 */
void moduleInit(Module *m, Arena *a) {
    vecInit(&m->structs, a, sizeof(void *));
    vecInit(&m->types, a, sizeof(void *));
    vecInit(&m->funcs, a, sizeof(void *));
    vecInit(&m->globals, a, sizeof(void *));
    vecInit(&m->traits, a, sizeof(void *));
    vecInit(&m->impls, a, sizeof(void *));
    vecInit(&m->uses, a, sizeof(void *));
    vecInit(&m->opens, a, sizeof(Open));
}

/**
 * @brief 造一个 `EX_IDENT`：标识符写出来是什么就记什么。
 *
 * `name` 与 `srcName` 一起设，此刻两者是同一个指针；改名发生在解析器之后。
 *
 * @param a     拥有节点的 arena。
 * @param name  写出来的名字。
 * @param line  源码行号（1 起）。
 * @return      清零并填好两个名字的标识符节点。
 */
Expr *exprIdent(Arena *a, const char *name, int line) {
    Expr *e = exprNew(a, EX_IDENT, line);
    e->u.ident.name = name;
    e->u.ident.srcName = name;
    return e;
}

/**
 * @brief 判断一个函数是不是方法。
 *
 * 规则就是名字：第一个参数名为 `self` 就是方法，声明里没有别的东西区分方法与自由函数。
 *
 * @param f  要看的函数。
 * @return   第一个参数名为 `self` 则 `true`；`f` 为 NULL 或没有参数时 `false`。
 */
bool funcIsMethod(const FuncDef *f) {
    if (!f || f->params.len == 0) return false;
    /* vecAt 取非 const 的 Vec；这里只读元素，不写它。 */
    Param *p0 = *(Param **)vecAt((Vec *)&f->params, 0);
    return strcmp(p0->name, "self") == 0;
}

/* ---------------------------------------------------------------- walking */

/**
 * @brief 对一个子表达式调用表达式回调；没有回调或子表达式为空时视为通过。
 *
 * @param v  回调与它的上下文。
 * @param e  要访问的子表达式；NULL 或没设表达式回调时直接返回 `true`。
 * @return   回调的结果；`false` 表示停下整趟走。
 */
static bool visitExpr(const AstVisit *v, Expr *e) {
    if (!e || !v->expr) return true;
    return v->expr(v->ctx, e);
}
/**
 * @brief 对一个子语句调用语句回调；没设语句回调时继续往下走它的子节点。
 *
 * @param v  回调与它的上下文。
 * @param s  要访问的子语句；NULL 时直接返回 `true`。
 * @return   回调的结果；`false` 表示停下整趟走。
 */
static bool visitStmt(const AstVisit *v, Stmt *s) {
    if (!s) return true;
    if (v->stmt) return v->stmt(v->ctx, s);
    return astWalkStmtChildren(s, v);
}
/**
 * @brief 按顺序对一个表达式向量里的每个元素调用表达式回调。
 *
 * @param v   回调与它的上下文。
 * @param xs  表达式向量（`Expr *` 元素）。
 * @return    全部走完返回 `true`；某个回调返回 `false` 时立刻返回 `false`。
 */
static bool visitExprList(const AstVisit *v, Vec *xs) {
    for (size_t i = 0; i < xs->len; i++)
        if (!visitExpr(v, *(Expr **)vecAt(xs, i))) return false;
    return true;
}

/**
 * @brief 访问表达式 `e` 的直接子节点。
 *
 * 回调访问的是**子**节点，从不访问被调用它的那个节点；回调返回 false 表示停下整趟走。没有
 * 设表达式回调时直接返回 `true`（跳过）。
 *
 * @param e  要走的表达式；NULL 直接返回 `true`。
 * @param v  回调与它的上下文。
 * @return   走完了返回 `true`；某个回调返回 false 时返回 `false`。
 */
/*@@all-kinds*/   bool astWalkExprChildren(Expr *e, const AstVisit *v) {
    if (!e) return true;
    switch (e->kind) {
    /* two children */
    case EX_BIN:      return visitExpr(v, e->u.bin.left) && visitExpr(v, e->u.bin.right);
    case EX_INDEX:    return visitExpr(v, e->u.index.obj) && visitExpr(v, e->u.index.index);
    case EX_COALESCE: return visitExpr(v, e->u.coalesce.main) &&
                             visitExpr(v, e->u.coalesce.fallback);
    /* one child */
    case EX_UN:    return visitExpr(v, e->u.un.operand);
    case EX_REF:   return visitExpr(v, e->u.ref.operand);
    case EX_DEREF: return visitExpr(v, e->u.deref.operand);
    case EX_SIGN:  return visitExpr(v, e->u.sign.operand);
    case EX_CONV:  return visitExpr(v, e->u.conv.operand);
    case EX_TRY:   return visitExpr(v, e->u.try_.operand);
    case EX_FIELD: return visitExpr(v, e->u.field.obj);
    case EX_NEW:   return visitExpr(v, e->u.new_.count);
    case EX_DYN:   return visitExpr(v, e->u.dynv.payload);
    case EX_EXT:   return visitExpr(v, e->u.ext_.call);
    case EX_LAMBDA:   /* 体，加上环境字段值：那些读发生在 lambda 写出来的地方 */
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (!visitExpr(v, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return false;
        return visitStmt(v, e->u.lambda.body);
    /* a receiver, or a list */
    case EX_METHOD:  return visitExpr(v, e->u.method.recv) && visitExprList(v, &e->u.method.args);
    /* 被调方只在不是**声明名**时才是子节点：直接调用 `g(x)` 走进去会把函数报成"当值用"；
     * 通过值调用 `f(x)`（`f` 持有一个 `fn`）读了这个绑定，必须像别的子节点一样走。 */
    case EX_CALL:
        return (!e->u.call.callee || e->u.call.callee->kind != EX_IDENT ||
                visitExpr(v, e->u.call.callee)) &&
                            visitExprList(v, &e->u.call.args);
    case EX_ASSOC:   return visitExprList(v, &e->u.assoc.args);
    case EX_GENCALL: return visitExprList(v, &e->u.gencall.args);
    case EX_ENUMVAL: return visitExprList(v, &e->u.enumval.args);
    case EX_ARRAYLIT:return visitExprList(v, &e->u.arraylit.elems);
    case EX_SLICE:   return visitExpr(v, e->u.slice.obj) && visitExpr(v, e->u.slice.lo) &&
                            visitExpr(v, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (!visitExpr(v, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return false;
        return true;
    /* no children */
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_IDENT: case EX_NULL:
        return true;
    }
    return true;
}

/**
 * @brief 访问语句 `s` 的直接子节点。
 *
 * 与 `astWalkExprChildren` 同一条约定：回调访问子节点，返回 false 停下整趟走。没有设语句
 * 回调时继续往下走它的子节点。
 *
 * @param s  要走的语句；NULL 直接返回 `true`。
 * @param v  回调与它的上下文。
 * @return   走完了返回 `true`；某个回调返回 false 时返回 `false`。
 */
/*@@all-kinds*/   bool astWalkStmtChildren(Stmt *s, const AstVisit *v) {
    if (!s) return true;
    switch (s->kind) {
    case ST_VAR:    return visitExpr(v, s->u.var.init);
    case ST_ASSIGN: return visitExpr(v, s->u.assign.target) && visitExpr(v, s->u.assign.value);
    case ST_RETURN: return visitExpr(v, s->u.ret.value);
    case ST_YIELD:  return visitExpr(v, s->u.yield_.value);
    case ST_TRAP:   return visitExpr(v, s->u.trap_.msg);
    /* `d.run { … }`: the block is the domain's body, so both children are walked. */
    case ST_DOMAIN: return visitExpr(v, s->u.domain_.callee) && visitStmt(v, s->u.domain_.body);
    case ST_EXPR:   return visitExpr(v, s->u.expr.expr);
    case ST_IF:     return visitExpr(v, s->u.ifs.cond) &&
                           visitStmt(v, s->u.ifs.thenBody) && visitStmt(v, s->u.ifs.elseBody);
    case ST_WHILE:  return visitExpr(v, s->u.whiles.cond) && visitStmt(v, s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (!visitStmt(v, *(Stmt **)vecAt(&s->u.block.stmts, i))) return false;
        return true;
    case ST_MATCH:
        if (!visitExpr(v, s->u.match.scrutinee)) return false;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (!visitStmt(v, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return false;
        return true;
    case ST_BREAK: case ST_CONTINUE: return true;   /* no children */
    }
    return true;
}
