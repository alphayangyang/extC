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
 * @brief 造一个二元运算节点：`op`、左右操作数一次填好。
 *
 * 解析器的 `mkBin` 与检查器的降糖都走这里，免得「构造 Bin 节点」这件事散成七八处手写。
 * `type` 留给检查器填（降糖自己知道类型时在调用点补）。
 *
 * @param a     拥有节点的 arena。
 * @param op    运算符文本（`"+"`、`"<"`、`"-"` 等），生命周期与 arena 一致。
 * @param left  左操作数。
 * @param right 右操作数。
 * @param line  源码行号（1 起）。
 * @return      新 `EX_BIN` 节点。
 */
Expr *exprBinNew(Arena *a, const char *op, Expr *left, Expr *right, int line) {
    Expr *e = exprNew(a, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = left;
    e->u.bin.right = right;
    return e;
}

/**
 * @brief 造一个浮点字面量节点。
 *
 * @param a     拥有节点的 arena。
 * @param value 字面量值（同时存进 `u.fval`，`f64`）。
 * @param line  源码行号（1 起）。
 * @return      新 `EX_FLOAT` 节点；`type` 留给调用方填。
 */
Expr *exprFloatNew(Arena *a, double value, int line) {
    Expr *e = exprNew(a, EX_FLOAT, line);
    e->u.fval = value;
    return e;
}

/**
 * @brief 造一个整数字面量节点。
 *
 * @param a     拥有节点的 arena。
 * @param value 字面量值（同时存进 `u.ival`）。
 * @param line  源码行号（1 起）。
 * @return      新 `EX_INT` 节点；`type` 留给调用方填。
 */
Expr *exprIntNew(Arena *a, long long value, int line) {
    Expr *e = exprNew(a, EX_INT, line);
    e->u.ival = value;
    return e;
}

/**
 * @brief 造一个 `EX_CALL` 节点：被调者填好、实参表初始化成空。
 *
 * @param a      拥有节点的 arena。
 * @param callee 被调者表达式。
 * @param line   源码行号（1 起）。
 * @return       新 `EX_CALL` 节点，`u.call.args` 可用 `vecPush` 追加。
 */
Expr *exprCallNew(Arena *a, Expr *callee, int line) {
    Expr *e = exprNew(a, EX_CALL, line);
    e->u.call.callee = callee;
    vecInit(&e->u.call.args, a, sizeof(Expr *));
    return e;
}

/**
 * @brief 把一个下标表达式节点里的下标换掉（降糖用：`out[i]` → `out[i - lo]`）。
 *
 * 只改下标槽，不动 `obj`、`type` 与其他字段；调用方负责新下标已经检查过。
 *
 * @param e     必须是 `EX_INDEX` 节点。
 * @param index 新下标表达式。
 */
void exprIndexRetarget(Expr *e, Expr *index) {
    e->u.index.index = index;
}

/**
 * @brief 给 `FuncDef` 记下它所属模块的名字。
 *
 * 检查器合成的函数（`par` 的 worker）没有解析期的模块上下文，靠这条把后续诊断与发射需要的
 * 名字补齐。
 *
 * @param f       要写的函数定义。
 * @param modName 模块名；生命周期与 arena 一致，可为 NULL。
 */
void funcDefSetModName(FuncDef *f, const char *modName) {
    f->modName = modName;
}

/**
 * @brief 把语句标记/取消「并行循环块」标记。
 *
 * 标记只由解析器造，检查器在降糖之后清掉；不新增 `StmtKind` 的代价就是这一个可由两侧设置的
 * 布尔值。
 *
 * @param s       要写的语句节点。
 * @param parLoop 新值。
 */
void stmtSetParLoop(Stmt *s, bool parLoop) {
    s->parLoop = parLoop;
}

/**
 * @brief 造一个方法调用节点 `recv.name(args…)`。
 *
 * 只填语法事实（接收者、名字、实参表）：方法解析、借用与 arena 记账都由检查器的 `EX_METHOD`
 * 路径完成，所以检查器可以把它当成用户写的一样重新检查一遍。
 *
 * @param a     分配用的 arena。
 * @param recv  接收者表达式。
 * @param name  方法名（静态存储）。
 * @param line  源码行号。
 * @return      新节点，`args` 初始化为空表。
 */
Expr *exprMethodNew(Arena *a, Expr *recv, const char *name, int line) {
    Expr *e = exprNew(a, EX_METHOD, line);
    e->u.method.recv = recv;
    e->u.method.name = name;
    vecInit(&e->u.method.args, a, sizeof(void *));
    return e;
}

/**
 * @brief 造一个切片节点 `obj[lo..hi]`。
 *
 * `lo`/`hi` 可以为 `NULL`，表示这一端没写；固定数组的省略端点由检查器补成字面量
 * （`a[..]` → `a[0..N]`），与解析器写出来的节点同形。
 *
 * @param a   分配用的 arena。
 * @param obj 被切的表达式。
 * @param lo  下界或 `NULL`。
 * @param hi  上界或 `NULL`。
 * @param line 源码行号。
 * @return    新节点。
 */
Expr *exprSliceNew(Arena *a, Expr *obj, Expr *lo, Expr *hi, int line) {
    Expr *e = exprNew(a, EX_SLICE, line);
    e->u.slice.obj = obj;
    e->u.slice.lo  = lo;
    e->u.slice.hi  = hi;
    return e;
}

/**
 * @brief 把一个表达式**原地**改成模块函数调用 `modPrefix::name(args…)`（`EX_ASSOC`）。
 *
 * 值 f-string 的说明符槽由解析器脱糖成保留名的调用，检查器按值的静态类型把它重定向到
 * `stl::stringio` 的 `padI64` / `padF64` / `padText`：那一层同时知道值的类型和写下来的
 * 说明符，也只有它能挑填充原语。重定向成 `EX_ASSOC` 之后，模块导入检查、mangling 与
 * 计划侧记账都走自由函数调用的既有路径。
 *
 * @param a         分配用的 arena（空 targs 表用）。
 * @param e         要改写的表达式节点（原地改）。
 * @param typeName  类型名段（如 `"stl"`）。
 * @param modPrefix 模块前缀（如 `"stl::stringio"`）。
 * @param name      函数名（静态存储）。
 * @param args      新的实参表（指针向量）。
 */
void exprRetargetAssoc(Arena *a, Expr *e, const char *typeName, const char *modPrefix,
                       const char *name, Vec *args) {
    e->kind = EX_ASSOC;
    e->u.assoc.typeName = typeName;
    e->u.assoc.modPrefix = modPrefix;
    e->u.assoc.name = name;
    e->u.assoc.args = *args;
    vecInit(&e->u.assoc.targs, a, sizeof(Type *));
    e->u.assoc.isCall = true;
    e->type = NULL;
}

/**
 * @brief 深拷贝一个表达式子树（融合降糖用）。
 *
 * 见 `ast.h` 的同名声明：只保证纯表达式形状的子树被完整复制，其它形状退化为浅拷贝。
 *
 * @param a   分配用的 arena。
 * @param src 源表达式；NULL 时返回 NULL。
 * @return    新子树。
 */
Expr *exprClone(Arena *a, Expr *src) {
    if (!src) return NULL;
    Expr *n = arenaAllocZero(a, sizeof(Expr));
    *n = *src;
    switch (src->kind) {
    case EX_BIN:
        n->u.bin.left  = exprClone(a, src->u.bin.left);
        n->u.bin.right = exprClone(a, src->u.bin.right);
        break;
    case EX_UN:
        n->u.un.operand = exprClone(a, src->u.un.operand);
        break;
    case EX_CONV:
        n->u.conv.operand = exprClone(a, src->u.conv.operand);
        break;
    case EX_FIELD:
        n->u.field.obj = exprClone(a, src->u.field.obj);
        break;
    case EX_INDEX:
        n->u.index.obj   = exprClone(a, src->u.index.obj);
        n->u.index.index = exprClone(a, src->u.index.index);
        break;
    case EX_SLICE:
        n->u.slice.obj = exprClone(a, src->u.slice.obj);
        n->u.slice.lo  = exprClone(a, src->u.slice.lo);
        n->u.slice.hi  = exprClone(a, src->u.slice.hi);
        break;
    case EX_SIGN:
        n->u.sign.operand = exprClone(a, src->u.sign.operand);
        break;
    case EX_COALESCE:
        n->u.coalesce.main     = exprClone(a, src->u.coalesce.main);
        n->u.coalesce.fallback = exprClone(a, src->u.coalesce.fallback);
        break;
    case EX_REF:
        n->u.ref.operand = exprClone(a, src->u.ref.operand);
        break;
    case EX_DEREF:
        n->u.deref.operand = exprClone(a, src->u.deref.operand);
        break;
    default:
        break;   /* 叶子与"不该融合"的形状：浅拷贝已经足够 */
    }
    return n;
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

/**
 * @brief 换掉一个块语句的语句表（融合降糖用）。
 *
 * @param s     必须是 `ST_BLOCK` 节点。
 * @param stmts 新语句表（`Stmt*` 的 `Vec`，按值拷进去）。
 */
void stmtBlockSetStmts(Stmt *s, Vec *stmts) {
    s->u.block.stmts = *stmts;
}

/**
 * @brief 造 `obj[index]` 下标节点（整数组表达式降糖用）。
 *
 * @param a     分配用的 arena。
 * @param obj   被下标的表达式。
 * @param index 下标表达式。
 * @param line  挂到节点上的源码行。
 * @return      新的 `EX_INDEX` 节点。
 */
Expr *exprIndexNew(Arena *a, Expr *obj, Expr *index, int line) {
    Expr *e = exprNew(a, EX_INDEX, line);
    e->u.index.obj   = obj;
    e->u.index.index = index;
    return e;
}

/**
 * @brief 造 `obj.name` 字段访问节点（降糖里读 `v.len` 用）。
 *
 * @param a    分配用的 arena。
 * @param obj  被访问的表达式。
 * @param name 字段名（不拷贝）。
 * @param line 挂到节点上的源码行。
 * @return     新的 `EX_FIELD` 节点。
 */
Expr *exprFieldNew(Arena *a, Expr *obj, const char *name, int line) {
    Expr *e = exprNew(a, EX_FIELD, line);
    e->u.field.obj  = obj;
    e->u.field.name = name;
    return e;
}

/**
 * @brief 造字符串字面量节点，把文本拷进 arena。
 *
 * @param a    分配用的 arena。
 * @param text 字符串内容。
 * @param line 挂到节点上的源码行。
 * @return     新的 `EX_STR` 节点。
 */
Expr *exprStrNew(Arena *a, const char *text, int line) {
    Expr *e = exprNew(a, EX_STR, line);
    size_t n = strlen(text);
    char *copy = arenaAlloc(a, n + 1);
    memcpy(copy, text, n + 1);
    e->u.str.text = copy;
    return e;
}

/**
 * @brief 造一元表达式节点。
 *
 * @param a       分配用的 arena。
 * @param op      运算符文本（静态存储）。
 * @param operand 操作数。
 * @param line    挂到节点上的源码行。
 * @return        新的 `EX_UN` 节点。
 */
Expr *exprUnNew(Arena *a, const char *op, Expr *operand, int line) {
    Expr *e = exprNew(a, EX_UN, line);
    e->u.un.op      = op;
    e->u.un.operand = operand;
    return e;
}

/**
 * @brief 造一条赋值语句（`target = value`，`op` 为空表示普通赋值）。
 *
 * @param a      分配用的 arena。
 * @param target 赋值目标。
 * @param value  右值。
 * @param op     复合赋值的运算符文本（`"+="` 等）；普通赋值传 `NULL`。
 * @param line   挂到节点上的源码行。
 * @return       新的 `ST_ASSIGN` 节点，`opExpr` 留给检查器。
 */
Stmt *stmtAssignNew(Arena *a, Expr *target, Expr *value, const char *op, int line) {
    Stmt *s = stmtNew(a, ST_ASSIGN, line);
    s->u.assign.target = target;
    s->u.assign.value  = value;
    s->u.assign.op     = op;
    s->u.assign.opExpr = NULL;
    return s;
}
/**
 * @brief 换掉一条赋值语句的右值与运算符（降糖重写用）。
 *
 * @param s     赋值语句（`ST_ASSIGN`）。
 * @param value 新的右值。
 * @param op    新的复合运算符；普通 `=` 传 NULL。
 */
void stmtAssignSetValue(Stmt *s, Expr *value, const char *op) {
    if (!s || s->kind != ST_ASSIGN) return;
    s->u.assign.value = value;
    s->u.assign.op = op;
}


/**
 * @brief 造 `trap(msg)` 语句。
 *
 * @param a    分配用的 arena。
 * @param msg  报文表达式。
 * @param line 挂到节点上的源码行。
 * @return     新的 `ST_TRAP` 节点。
 */
Stmt *stmtTrapNew(Arena *a, Expr *msg, int line) {
    Stmt *s = stmtNew(a, ST_TRAP, line);
    s->u.trap_.msg = msg;
    return s;
}

/**
 * @brief 造 `if cond { thenBody } else { elseBody }` 语句。
 *
 * @param a        分配用的 arena。
 * @param cond     条件表达式。
 * @param thenBody 条件为真时的语句。
 * @param elseBody 条件为假时的语句；NULL 表示没有 `else`。
 * @param line     挂到节点上的源码行。
 * @return         新的 `ST_IF` 节点。
 */
Stmt *stmtIfNew(Arena *a, Expr *cond, Stmt *thenBody, Stmt *elseBody, int line) {
    Stmt *s = stmtNew(a, ST_IF, line);
    s->u.ifs.cond     = cond;
    s->u.ifs.thenBody = thenBody;
    s->u.ifs.elseBody = elseBody;
    return s;
}

/**
 * @brief 造 `for var in 0..hi { body }` 的脱糖块（整数组表达式降糖用）。
 *
 * 形状与解析器的区间 `for` 一致：`[循环变量声明, 上界声明, while]`，`while` 的体是
 * `[body, 步进]` 并挂 `forStep`。检查器的区间变量放宽与 `par` 降糖都按这个形状认。
 *
 * @param a    分配用的 arena。
 * @param var  循环变量名。
 * @param hi   上界表达式（半开区间）。
 * @param body 循环体（一个块）。
 * @param line 挂到节点上的源码行。
 * @return     外层块语句。
 */
Stmt *stmtRangeForNew(Arena *a, const char *var, Expr *hi, Stmt *body, int line) {
    Stmt *decl = stmtNew(a, ST_VAR, line);
    decl->u.var.name      = var;
    decl->u.var.ann       = NULL;
    decl->u.var.init      = exprIntNew(a, 0, line);
    decl->u.var.mut       = true;      /* the loop itself writes it */
    decl->u.var.overwrite = false;

    Stmt *hiDecl = stmtNew(a, ST_VAR, line);
    hiDecl->u.var.name      = "__extc_hi";
    hiDecl->u.var.ann       = NULL;
    hiDecl->u.var.init      = hi;
    hiDecl->u.var.mut       = false;
    hiDecl->u.var.overwrite = false;

    Expr *cond = exprBinNew(a, "<", exprIdent(a, var, line), exprIdent(a, "__extc_hi", line), line);
    Stmt *step = stmtAssignNew(a, exprIdent(a, var, line), exprIntNew(a, 1, line), "+=", line);

    Stmt *loopBody = stmtNew(a, ST_BLOCK, line);
    vecInit(&loopBody->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&loopBody->u.block.stmts) = body;
    *(Stmt **)vecPush(&loopBody->u.block.stmts) = step;
    loopBody->forStep = step;          /* label target for `continue` */

    Stmt *w = stmtNew(a, ST_WHILE, line);
    w->u.whiles.cond = cond;
    w->u.whiles.body = loopBody;

    Stmt *outer = stmtNew(a, ST_BLOCK, line);
    vecInit(&outer->u.block.stmts, a, sizeof(Stmt *));
    *(Stmt **)vecPush(&outer->u.block.stmts) = decl;
    *(Stmt **)vecPush(&outer->u.block.stmts) = hiDecl;
    *(Stmt **)vecPush(&outer->u.block.stmts) = w;
    return outer;
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
