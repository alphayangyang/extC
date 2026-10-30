/* Statement checking: declarations, assignments, control flow and `match`.
 *
 * Each statement is checked here for what it does to the depth bookkeeping and to the
 * set of narrowing facts; the expressions inside it are handed to the expression
 * checker.
 */

#include "check_internal.h"
#include "plan.h"          /* 计划：写入经 setter */
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------- statements */

/**
 * @brief 把裸的构造器名就地改写成限定变体构造。
 *
 * 只认 `success` / `failure` / `some` / `none` 四个名字，期望类型必须是对应的 `result`
 * 或 `option` 实例；用户定义了同名函数时名字原样保留。`none` 是唯一可以省略括号的。
 * 节点就地进行改写，因此之后的检查与代码生成都走普通变体构造那条路。
 *
 * @param c     检查器。
 * @param e     要改写的表达式；不是上述形状时原样不动。
 * @param want  期望的类型，用来判断该名字是不是已知构造器。
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
        /* 无载荷的构造器可以省掉括号，枚举变体本来就是这个待遇。 */
        nm = e->u.ident.name;
        noParens = true;
    } else return;

    if      (strcmp(nm, "success") == 0 || strcmp(nm, "failure") == 0) { proto = "result"; nargs = 2; }
    else if (strcmp(nm, "some")    == 0 || strcmp(nm, "none")    == 0) { proto = "option"; nargs = 1; }
    else return;
    if (noParens && strcmp(nm, "none") != 0) return;   /* `none` 是唯一无参的 */

    /* 同名用户函数优先：短形式只用在无歧义处。 */
    if (findFunc(c, nm)) return;
    /* 返回类型不是对应的容器：名字原样留下，按未定义报出。 */
    if (!isProtoType(want, proto, nargs)) return;

    /* 联合体：先取出 `args` 再改 kind，否则新字段会覆盖它。 */
    Vec args;
    if (noParens) vecInit(&args, c->arena, sizeof(void *));
    else          args = e->u.call.args;
    /* `option` / `result` 是普通枚举，裸构造器就是变体构造；`typeName` 记实例名，代码生成
     * 用它拼 C 标签常量 `option_i64_some`。 */
    e->kind = EX_ENUMVAL;
    e->u.enumval.typeName = want->name;
    e->u.enumval.variant  = nm;
    e->u.enumval.args     = args;
    planSetAssocOwner(e, want);
}


/**
 * @brief 在不检查表达式的前提下推断 `for x in SUBJ` 里 SUBJ 的类型。
 *
 * 只对解析器允许的形状作答：名字、字段、`*p`；三者都能从符号表与字段表直接读出。类型必须
 * **不经过检查**就能拿到：`checkExpr` 没有记忆化，在这里检查会让诊断重复报一次。其余形状
 * 返回 NULL，调用方保留 slice 路径。
 *
 * @param c  检查器。
 * @param e  循环的主表达式。
 * @return   主表达式的类型；形状不认识或查不到时返回 NULL。
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
        /* `ttBase` 会剥掉**所有** `ref` 层，所以引用判定看替换后的类型本身：`*p` 取的是
         * 所指对象的类型。 */
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
 * @brief 把反糖成 slice 循环的 `for x in SUBJ` 改指到迭代器协议。
 *
 * 只处理解析器为 `for … in` 造出的固定三语句形状；SUBJ 的类型可切片时原样不动。语句是
 * **就地改指而不是重建**：循环变量、循环体与循环都还是解析器造的节点，三个协议调用是普通
 * `EX_METHOD` 节点，因此照常走方法解析。协程本身即迭代器（帧里存着 pc），直接在主体上
 * 调用 `next` / `value`，不引入中间绑定；SUBJ 是名字、字段或 `*p`，求值两次没有副作用。
 *
 * @param c      检查器。
 * @param block  `for … in` 反糖出的块语句。
 * @return       改指到迭代器则返回 true；形状不符或 SUBJ 可切片时返回 false。
 */
static bool forRetargetToIterator(Checker *c, Stmt *block) {
    if (block->u.block.stmts.len != 3) return false;
    Stmt *bind = *(Stmt **)vecAt(&block->u.block.stmts, 0);
    Stmt *w    = *(Stmt **)vecAt(&block->u.block.stmts, 2);
    if (!bind || bind->kind != ST_VAR || !bind->u.var.init ||
        bind->u.var.init->kind != EX_SLICE) return false;
    if (!w || w->kind != ST_WHILE || !w->u.whiles.body ||
        w->u.whiles.body->kind != ST_BLOCK || w->u.whiles.body->u.block.stmts.len != 3)
        return false;
    Expr *subject = bind->u.var.init->u.slice.obj;
    Type *st = forSubjectType(c, subject);
    if (!st) return false;                       /* 类型未知：保留 slice 路径 */
    /* 协程即自身的迭代器：帧里存着 pc，直接对 SUBJ 调 `next` / `value`，没有 `__extc_s`
     * 绑定也没有 `__extc_it` 别名要同步。 */
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
            inner0->u.block.stmts.len = 2;       /* `[ var x = …value(); BODY ]`：步进语句已删 */
            *(Stmt **)vecAt(&block->u.block.stmts, 0) = w;   /* 丢掉主语绑定 */
            block->u.block.stmts.len = 1;
            return true;
        }
    }
    Type *sb = ttBase(tsub(c, st));
    Type *elem = (sb && sb->kind == TY_ARRAY) ? sb->inner : viewElemOf(sb);
    if (elem) return false;                      /* 可切片：保留 slice 路径 */
    if (!bind->u.var.name) return false;

    /* `var __extc_s = SUBJ`：绑定主表达式本身，仍然只求值一次。 */
    bind->u.var.init = subject;

    /* `var __extc_it = __extc_s.iter()` */
    Expr *iter = exprNew(c->arena, EX_METHOD, block->line);
    iter->u.method.recv = exprIdent(c->arena, bind->u.var.name, block->line);
    iter->u.method.name = "iter";
    Stmt *itDecl = stmtNew(c->arena, ST_VAR, block->line);
    itDecl->u.var.name = "__extc_it";
    itDecl->u.var.init = iter;
    itDecl->u.var.mut = true;                    /* `next` 收 `mut ref Self` */
    *(Stmt **)vecAt(&block->u.block.stmts, 1) = itDecl;

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
    inner->u.block.stmts.len = 2;                /* `[ var x = …value(); BODY ]`：步进语句已删 */
    /* 步进语句已删，`continue` 没有可跳的标签：推进由条件里的 `next()` 完成。不记这一步就会
     * 生成指向不存在标签的 `goto`；只记结果，不改树。 */
    planSetForStepDropped(inner);
    return true;
}

/**
 * @brief 把区间循环的循环变量按上界放宽（`for i in 0..n` 里 `n: i64` 时 `i` 也变 `i64`）。
 *
 * 解析器把 `for i in lo..hi` 脱糖成外层块 `[var i = lo, var __extc_hi = hi,
 * while i < __extc_hi { 体; i += 1 }]`，`i` 没有注解、类型取自 `lo`，所以 `for i in 0..n`
 * 拿到 `i32` —— 即使 `n` 是 `i64`。比较本身在更宽的类型里做（两侧都加宽），语义没问题；
 * 但**变量停在窄类型**会让体里每次下标算术都先窄后宽，生成 C 里到处是 `(int64_t)(…)`，
 * 向量化因此失效（见 `bench/fortran` 的 -O3 档）。
 *
 * 只在 `hi` 能无损容纳 `lo`（`ttCanWiden`）时放宽：比较两侧的相对关系不变，循环变量的
 * 算术从“先窄后宽”变成“全宽”。两端同型、反向、非整数、或形状不是区间循环时都不动。
 *
 * 调用点在 `checkBlockBody` 检查完 `stmts[1]` 之后：两端类型都已落地、`while` 还没检查，
 * 体里对 `i` 的使用会解析到放宽后的类型。
 *
 * @param c      检查器。
 * @param block  块语句；不是区间循环的外层块就直接返回。
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

/* ----------------------------------------------------- `par` 降糖 */

/**
 * @brief `par` 降糖扫描栈里的一项：语句、表达式或"内层循环结束"标记。
 *
 * 用显式栈而不是递归：`tools/check_walkers.py` 那本棘轮数的是"手写递归 kind-walker"，
 * `par` 的扫描只是**分类器**（不完整、遇到没列的 kind 就放过），不该去动那本账。
 */
typedef struct {
    const void   *node;
    unsigned char tag;    /* 0 = Stmt*，1 = Expr*，2 = 内层循环结束 */
} ParItem;

/**
 * @brief `par` 降糖的扫描上下文。
 *
 * 扫描发生在循环变量与 `__extc_hi` 都检查完、`while` 还没检查的时候：外侧作用域还开着，
 * 所以捕获视图的名字能 `lookup` 到；体内自己的 `var` 只是先记下名字（它们的类型要等
 * worker 复查时才有）。
 */
typedef struct {
    Checker    *c;
    const char *loopVar;    /* 循环变量的源码名 */
    const char *outName;    /* 输出视图名；NULL 表示还没看到写 */
    int         outLine;    /* 第一次写的行 */
    Vec         locals;     /* const char*：体内 `var` 声明的名字 */
    Vec         captures;   /* const char*：只读捕获的视图名，按首次出现顺序 */
    Vec         scalars;    /* const char*：按值捕获的标量名，按首次出现顺序 */
    Vec         parVars;    /* const char*：par 循环变量名（二维折叠时含内层循环变量） */
    const char *innerVar;   /* 二维折叠时内层循环变量名；未折叠为 NULL */
    Expr       *innerLo;    /* 内层循环区间的下界 */
    Expr       *innerHi;    /* 内层循环区间的上界 */
    int         foldDepth;  /* 已折叠的内层 `par` 层数（v1 最多 1） */
    bool        fullOut;    /* 折叠过内层 `par`：输出视图整份交给 worker */
    Vec         items;      /* ParItem：显式工作栈 */
    int         loopDepth;  /* 内层循环层数：`break`/`continue` 只有在内层才不逃出内核 */
    bool        failed;     /* 已经报过错：停止继续扫描（避免刷屏） */
} ParLower;

/**
 * @brief 名字是否在字符串表里。
 *
 * @param v     表（`const char *` 元素）。
 * @param name  要找的名字；NULL 返回 false。
 * @return      在表里则 `true`。
 */
static bool parHasName(const Vec *v, const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt((Vec *)v, i), name) == 0) return true;
    return false;
}

/**
 * @brief 往扫描栈里压一项。
 *
 * @param w     扫描上下文。
 * @param node  语句或表达式节点。
 * @param tag   0 = 语句，1 = 表达式，2 = 内层循环结束。
 */
static void parPushItem(ParLower *w, const void *node, unsigned char tag) {
    ParItem *it = (ParItem *)vecPush(&w->items);
    it->node = node;
    it->tag  = tag;
}

/**
 * @brief 记一个体内声明的局部名（重复的只记一次）。
 *
 * @param w     扫描上下文。
 * @param name  局部变量名。
 */
static void parAddLocal(ParLower *w, const char *name) {
    if (!name || parHasName(&w->locals, name)) return;
    *(const char **)vecPush(&w->locals) = name;
}

/**
 * @brief 记一个只读捕获的视图名（重复的只记一次）。
 *
 * @param w     扫描上下文。
 * @param name  视图名。
 */
static void parAddCapture(ParLower *w, const char *name) {
    if (!name || parHasName(&w->captures, name)) return;
    *(const char **)vecPush(&w->captures) = name;
}

/**
 * @brief 记一个按值捕获的标量名（重复的只记一次）。
 *
 * 标量没有共享状态，值在调用前求一次就固定了，所以按值捕获是安全的；worker 拿到的是
 * 自己的副本，写它也只写副本。
 *
 * @param w     扫描上下文。
 * @param name  标量名。
 */
static void parAddScalar(ParLower *w, const char *name) {
    if (!name || parHasName(&w->scalars, name)) return;
    *(const char **)vecPush(&w->scalars) = name;
}

/**
 * @brief 记一个 par 循环变量名（写目标的下标只允许由这些名字与常量构成）。
 *
 * @param w     扫描上下文。
 * @param name  循环变量名。
 */
static void parAddParVar(ParLower *w, const char *name) {
    if (!name || parHasName(&w->parVars, name)) return;
    *(const char **)vecPush(&w->parVars) = name;
}

/**
 * @brief 标量类型能否按值捕获。
 *
 * 只收内建标量：整数、`f32`/`f64`、`bool`。视图走另的路径（按引用共享），结构体/引用/
 * 数组这些带内部指针或可变的类型都不收 —— v1 的捕获面就到这里。
 *
 * @param t  类型（已 `ttBase`）。
 * @return   可以按值捕获则 `true`。
 */
static bool parIsScalarCopyable(Type *t) {
    return t && (ttIsInteger(t) || ttIs(t, "f64") || ttIs(t, "f32") || ttIs(t, "bool"));
}

/**
 * @brief 报一条 `par` 限制集诊断并停止扫描。
 *
 * @param w     扫描上下文。
 * @param line  源码行。
 * @param note  解释"为什么"的补充说明（可为 NULL）。
 * @param msg   主诊断文案。
 */
static void parFail(ParLower *w, int line, const char *note, const char *msg) {
    if (w->failed) return;
    w->failed = true;
    ckError(w->c, line, note, "%s", msg);
}

/**
 * @brief 判断一个名字在当前作用域里是不是视图。
 *
 * @param w     扫描上下文。
 * @param name  名字。
 * @return      解析到视图类型则 `true`。
 */
static bool parNameIsView(ParLower *w, const char *name) {
    if (!name) return false;
    Sym *sym = lookup(w->c, name);
    return sym && sym->type && ttIsViewType(ttBase(sym->type));
}

/**
 * @brief 两个表达式在"同一个值"的意义上是否相同（判据只到名字与整数字面量）。
 *
 * 二维折叠的展开常数要证明"至少等于内层循环的迭代次数"，最常用的写法是把内层上界本身
 * 当常数用（`out[r * cols + c]` 内层是 `0..cols`），所以这里只需要认出同一个名字/同一个数。
 *
 * @param a 表达式。
 * @param b 表达式。
 * @return   同名标识符、同值整数字面量，或两者结构相同的加乘组合时返回 `true`。
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
 * @brief 判断展开常数 `k` 能不能保证 `outer * k + inner` 一格一槽。
 *
 * 需要 `k >= innerHi - innerLo`。可证的两种写法：`k` 与内层上界是同一个名字（内层从 0 起
 * 时成立），或两者都是整数字面量且 `k >= hi - lo`。证不出来就拒 —— 猜一个常数会让两个
 * 外层落进同一格，是静默错值。
 *
 * @param w 扫描上下文（`innerLo`/`innerHi` 已填）。
 * @param k 乘数表达式。
 * @return  能证明一格一槽则 `true`。
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
    return false;
}

/**
 * @brief 判断二维折叠的写下标是否一一对应每个 `(r, c)`：只认 `out[r * K + c]`。
 *
 * 一维的槽规则（下标就是循环变量）天生一一对应；折叠两层之后"下标由循环变量构成"不够
 * ——`out[c]` 里没有 `r`，每个外层 worker 都会重写整列。能证明的情况只有
 * `outer * K + inner`（`K >=` 内层迭代次数），或交换两层的等价写法。
 *
 * @param w 扫描上下文。
 * @param e 写下标。
 * @return  一格一槽则 `true`。
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
 * @brief 判断一个写目标的下标是否只由 par 循环变量与整数常量构成。
 *
 * 二维折叠的 worker 按绝对槽位写（`out[r * cols + c]`），所以下标是"以循环变量为自变量
 * 的整数表达式"：整数常量、par 循环变量、`+ - *` 与一元 `-`。别的名字（捕获的标量/视图、
 * 全局变量）与调用都不收。
 *
 * @param w  扫描上下文（`parVars` 已填）。
 * @param e  下标表达式。
 * @return   形状允许则 `true`。
 */
static bool parIndexOverLoops(ParLower *w, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT:
        return true;
    case EX_IDENT:
        /* 循环变量，或按值捕获的标量（worker 里的不可变副本，`out[r * cols + c]` 的 `cols`
         * 就是它）。视图出现在下标里是类型错误，交给复查去报。 */
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
 * @brief 把一个赋值目标按 `par` 的限制集分类，并把输出写下标改写成子视图下标。
 *
 * 允许的目标只有两种：体内声明的局部标量（私有的，随便写），以及**唯一**输出视图的
 * `out[i]`。后者要同时满足：`out` 是视图、`i` 恰好是循环变量本身。改写把 `out[i]` 变成
 * `out[i - __extc_plo]`：trampoline 交给 worker 的是 `[lo, hi)` 切过的零基子视图。
 *
 * @param w  扫描上下文。
 * @param t  赋值目标表达式。
 */
static void parScanTarget(ParLower *w, Expr *t) {
    if (!t || w->failed) return;
    /* 体内声明的私有标量：写它不共享，随便写。 */
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
    if (parHasName(&w->locals, name)) {      /* 私有标量 */
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
        /* 二维折叠：下标是"以循环变量为自变量的整数表达式"（`out[r * cols + c]`）。只认
         * 整数常量、par 循环变量、`+ - *` 与一元 `-`。 */
        if (!parIndexOverLoops(w, idx) || !parIndexInjective(w, idx)) {
            parFail(w, t->line,
                    "A folded two-dimensional loop writes `out[r * K + c]` with `K` at least the"
                    " inner loop's iteration count (the inner upper bound is the usual choice):"
                    " anything else lets two outer iterations land in the same slot.",
                    "this par loop may write only its own output slot");
            return;
        }
        parPushItem(w, idx, 1);
        return;                                 /* 绝对下标：不重写 */
    }
    if (!idx || idx->kind != EX_IDENT || !idx->u.ident.name ||
        strcmp(idx->u.ident.name, w->loopVar) != 0) {
        parFail(w, t->line,
                "The slot a worker owns is decided by the loop index, so the subscript has to"
                " be the loop variable itself.",
                "this par loop may write only its own output slot");
        return;
    }
    /* `out[i]` -> `out[i - lo]`：trampoline 给的是 [lo, hi) 的子视图。 */
    Expr *lo = exprIdent(w->c->arena, "__extc_plo", t->line);
    Expr *sub = exprBinNew(w->c->arena, "-", idx, lo, t->line);
    sub->type = w->c->tI64;
    exprIndexRetarget(t, sub);
}

/**
 * @brief 扫描一个表达式节点：分类名字、拒掉分配与调用、把子表达式压栈。
 *
 * @param w  扫描上下文。
 * @param e  表达式。
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
        /* 标量转换是纯的（可能带截断检查，失败也只是 `trap`）：`f64(i)` 正是逐元素循环
         * 最常见的写法。 */
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
        if (!sym) return;                       /* 更外层的名字：留给 worker 复查报错 */
        if (parNameIsView(w, name)) {
            parAddCapture(w, name);
            return;
        }
        if (parIsScalarCopyable(sym->type)) {   /* 标量按值捕获：副本不共享 */
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
 * @brief 把一个内层 `par` 块折叠进外层内核：清标记、记内层循环变量、切到整份输出视图。
 *
 * 内层 `par` 不再另开一批线程，它就是 worker 里的普通循环（`par r in .. { par c in .. }`
 * 的设计语义）。形状不对（不是区间循环块）时不动它，让 worker 复查去报错。
 *
 * @param w    扫描上下文。
 * @param blk  内层 `par` 块。
 * @return     折叠成功（或本来就不需要折叠）返回 `true`；嵌套过深返回 `false`。
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
            /* `[循环变量, 上界, while]`：上界声明里存着内层区间的上界表达式。 */
            if (blk->u.block.stmts.len >= 2) {
                Stmt *hdecl = *(Stmt **)vecAt(&blk->u.block.stmts, 1);
                if (hdecl && hdecl->kind == ST_VAR) w->innerHi = hdecl->u.var.init;
            }
        }
    }
    return true;
}

/**
 * @brief 扫描一条语句：收集声明、检查写目标、把子块与子表达式压栈。
 *
 * @param w  扫描上下文。
 * @param s  语句。
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
        parPushItem(w, NULL, 2);                 /* 出内层循环时把层数减回去 */
        parPushItem(w, s->u.whiles.cond, 1);
        parPushItem(w, s->u.whiles.body, 0);
        return;
    case ST_BLOCK:
        /* 脱糖 `for` 的步进语句挂在体块上（`forStep`）：它是循环机制，不是用户写的赋值，
         * 写的是循环变量自己。扫描跳过它，和 `widenRangeForVar` 的口径一致。 */
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
 * @brief 用显式栈把循环体扫一遍。
 *
 * @param w     扫描上下文。
 * @param body  循环体块。
 * @return      扫描成功（没有诊断）时返回 `true`。
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
 * @brief 造一个 worker 形参。
 *
 * @param c     检查器（提供 arena）。
 * @param name  源码里的名字（也就是 worker 作用域里要解析到的名字）。
 * @param type  形参类型。
 * @param line  源码行。
 * @return      新形参。
 */
static Param *parParam(Checker *c, const char *name, Type *type, int line) {
    Param *p = (Param *)arenaAllocZero(c->arena, sizeof(Param));
    p->name = name;
    p->type = type;
    p->line = line;
    return p;
}

/**
 * @brief 造一个 `i32` 整数字面量表达式。
 *
 * @param c     检查器。
 * @param value 值。
 * @param line  源码行。
 * @return      `EX_INT` 节点，类型已填。
 */
static Expr *parIntLit(Checker *c, long long value, int line) {
    Expr *e = exprIntNew(c->arena, value, line);
    e->type = c->tI32;
    return e;
}

/**
 * @brief 造一个已检查过、带 C 名的 `EX_IDENT` 引用（给合成的调用实参用）。
 *
 * 这些节点不会交给 `checkExpr`：它们引用的绑定在外层作用域里已经检查过，这里把
 * `planCName` 直接抄过去，代码生成就照普通标识符发。
 *
 * @param c     检查器。
 * @param name  源码名。
 * @param sym   它解析到的绑定。
 * @param line  源码行。
 * @return      填好类型与 C 名的标识符节点。
 */
static Expr *parCheckedIdent(Checker *c, const char *name, Sym *sym, int line) {
    Expr *e = exprIdent(c->arena, name, line);
    e->type = sym->type;
    planSetCName(e, sym->cname, RKIND_EXPR);
    return e;
}

/**
 * @brief 合成 worker 函数并追加进模块。
 *
 * worker 的形状与 `parallel::run` 手写的 worker 完全一致：`(id, lo, hi, <只读视图…>,
 * out: mut slice<T>) -> i32`。名字从 `__extc_parw<N>` 里挑一个没被占用的（codegen 直接拿
 * `wf->name` 拼 `__extc_par_<name>`，所以它必须是合法 C 标识符）。
 *
 * @param w     扫描上下文（捕获表、输出视图名都已填）。
 * @param decl  循环变量声明（拿 lo 初始化式）。
 * @param hiDecl `__extc_hi` 声明（拿 hi 初始化式）。
 * @param loop  原 `while` 节点。
 * @return      合成出来的 worker。
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

    /* 体：`[var <i> = lo, var __extc_hi = hi, while … , return 0]`。 */
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
    /* 二维折叠的 worker 写绝对槽位，所以 trampoline 传整份输出视图而不是 [lo,hi) 切段。 */
    if (w->fullOut) planSetParFullOut(wf, true);

    *(FuncDef **)vecPush(&c->m->funcs) = wf;   /* 主检查循环稍后会 checkFunc 它 */
    return wf;
}

/**
 * @brief 把一个 `par` 块降糖成对合成 worker 的调用。
 *
 * 时机与 `widenRangeForVar` 相同（`[循环变量, __extc_hi, while]` 的前两条刚检查完）：
 * 这时两端有类型、名字在外层作用域里可查，而 `while` 还没检查。成功后块里只剩一条
 * `ST_EXPR(EX_CALL)`，并盖好 `plan` 标记 —— 那就是 `parallel::run` 内建校验本来会盖的
 * 那几项。失败时块原样保留，让普通检查继续报错。
 *
 * @param c      检查器。
 * @param block  带 `parLoop` 标记的块。
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
    /* 下标是 `i64`：两端都得能无损进 `i64`（`0..n` 这种 `i32` 字面量允许 —— 它是精确的
     * 常量，比较与算术都加宽到 `i64`）。 */
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

    /* 合成调用：`__extc_parwN(0, 捕获视图…, out, hi - lo, -1)`。args[0] 是占位：codegen 对
     * worker 调用从不 genExpr 它。threads 用 `-1` 哨兵，codegen 会换成
     * `extc_par_default_threads()`。 */
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
    /* 也记成普通被调者：效应摘要按调用图闭合，这条边不在图上就会让外层函数变成
     * “effects could not be fully analyzed”（合成调用被 `checkStmt` 的守卫跳过，
     * 不再经过 `checkExpr` 的记账路径）。 */
    planSetCallee(call, wf);
    planSetIsParWorker(wf, true);
    planSetParTlsArena(wf, true);
    planSetUsed(wf, true);
    call->type = c->tI32;

    /* 块里只剩一条被丢弃的调用；`checkStmt` 的 `ST_EXPR` 分支看到 plan 标记就不再复查。 */
    block->u.block.stmts.len = 0;
    Stmt *s = stmtNew(c->arena, ST_EXPR, block->line);
    s->u.expr.expr = call;
    *(Stmt **)vecPush(&block->u.block.stmts) = s;
    stmtSetParLoop(block, false);
}

/**
 * @brief 复查 worker 体：拒掉检查之后才认得出的调用形状。
 *
 * 降糖时的扫描只能看**解析形状**：`io::cout << x` 是 `EX_BIN`，要到检查器才变成方法调用。
 * worker 体照普通函数检查完之后，这里按解析好的形状再扫一遍，把调用、分配与"体内又有
 * 一个 `par`"拒掉。显式栈，同样是为了不碰 walker 棘轮。
 *
 * @param c  检查器。
 * @param f  刚检查完的 worker。
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

/* ------------------------------------------------- S1 元素链融合（第 2 层） */

/* 融合总开关：`--no-fuse` 把它打开（关掉融合）。检查器只读它。 */
static bool gNoFuse = false;

/**
 * @brief 设置"关掉融合"开关（驱动层的 `--no-fuse` 调用）。
 *
 * @param off 为 `true` 时不融合。
 */
void checkSetNoFuse(bool off) {
    gNoFuse = off;
}

/**
 * @brief 融合是否被关掉。
 *
 * 调试与对拍开关：关掉之后同一份源码走未融合的路径，`tests/fuse` 用它逐字节比对两种形状的
 * 输出。
 *
 * @return 关掉时为 `true`。
 */
static bool fuseOff(void) {
    return gNoFuse;
}

/**
 * @brief 两个表达式是否结构相等（融合的迭代空间判据）。
 *
 * 只比形状：标识符同名、整数字面量同值、二元同运算符且子项递归相等、字段同名且对象递归相等。
 * 允许的更复杂形状（调用等）一律判不等，于是融合自动放弃。
 *
 * @param a 左表达式。
 * @param b 右表达式。
 * @return 结构相等时为 `true`。
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
 * @brief 表达式是否"纯"：可以搬进另一个循环体而语义不变。
 *
 * 纯 = 只有字面量、标识符、二元/一元运算、转换、字段与下标（下标本身也要纯）。调用、`new`、
 * 切片、闭包、`trap`、`?` 传播都不算：它们要么有副作用，要么依赖执行次数。
 *
 * @param e 要看的表达式。
 * @return 纯时为 `true`。
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
 * @brief 表达式里是否出现了某个标识符（名字比较）。
 *
 * @param e    要扫描的表达式。
 * @param name 要找的标识符名字。
 * @return 出现时为 `true`。
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
 * @brief 把循环体里"唯一的内层块"摊平（解析器给 `{ … }` 体包了一层块）。
 *
 * 摊平只发生在形状合格时：`for i in 0..n { stmt }` 的体解析成 `ST_BLOCK{ ST_BLOCK{stmt}, step }`，
 * 融合要按语句逐条看，所以先把这层壳去掉。内层块自己没有步进语句，搬上来不丢东西。
 *
 * @param c    检查器（取 arena）。
 * @param body 循环体块（含步进语句）。
 */
static bool fuseIsLoopBlock(Stmt *s) {
    if (!s || s->kind != ST_BLOCK) return false;
    if (s->parLoop || s->forStep) return true;
    /* 区间/容器 `for` 脱糖出来的外层块：`[循环变量, 上界, while]`。形状相同就不再往里摊，
     * 否则会把一个独立循环的体并进外层循环（`par r { par c { … } }` 的嵌套检查就是被这么
     * 弄坏的）。 */
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
 * @brief 把解析器给循环体套的那一层包装块摊平：`{ {用户语句…}, step }` → `{用户语句…, step}`。
 *
 * 只认"这个块正好是 `[块, 步进]` 两件、且那个块不是循环"这一种形状；循环块（`par`/`for` 的
 * 脱糖块）不动 —— 摊平它会把内层循环的体并进外层循环，破坏 `par` 的嵌套检查与循环语义。
 *
 * @param c    检查器（取 arena）。
 * @param body 循环体块。
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
 * @brief 把"区间 for 脱糖块"拆成融合需要的那几件：循环变量名、两端、`while` 与体。
 *
 * 形状来自 `parser.c` 的 `forRangeFrom`：块里正好三条语句 `[var i, var __extc_hi, while]`，
 * `while` 的条件是 `i < __extc_hi`，体是 `[原体..., step]` 且 `body->forStep` 指着那条步进。
 *
 * @param c       检查器（摊平内层块时取 arena）。
 * @param blk     候选语句。
 * @param whileOut 命中时填 `while` 语句。
 * @param bodyOut  命中时填循环体块。
 * @param varOut   命中时填循环变量名。
 * @param loOut    命中时填下界表达式。
 * @param hiOut    命中时填上界表达式。
 * @return 形状对上时为 `true`。
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
 * @brief 往一列表里加名字（去重，按出现顺序）。
 *
 * @param v    `const char*` 的 `Vec`。
 * @param name 要加的名字。
 */
static void fuseAddName(Vec *v, const char *name) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(*(const char **)vecAt(v, i), name) == 0) return;
    *(const char **)vecPush(v) = name;
}

/**
 * @brief 表达式里出现的所有标识符名字。
 *
 * @param e 要扫描的表达式。
 * @param v `const char*` 的 `Vec`；重复的名字只记一次。
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
 * @brief 表达式里每一处 `tmp` 是否都恰好是"以循环变量为唯一下标"的读形状。
 *
 * 融合把 `tmp[i]` 整个替换成 A 的右值，所以任何别的用法（`tmp.len`、`tmp[j]`、把 `tmp`
 * 整体传给谁、写 `tmp`）都让融合放弃。
 *
 * @param e   要扫描的表达式。
 * @param tmp 临时视图名字。
 * @param var 循环变量名字。
 * @return 全是可替换的读形状时为 `true`。
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
 * @brief 就地替换表达式里的 `tmp[i]`（融合的第二步）。
 *
 * 命中节点的内容整体换成右值深拷贝（整节点赋值，不逐字段写，于是 AST 的字段写守卫不受影响）。
 *
 * @param c   检查器（取 arena）。
 * @param e   要改写的表达式。
 * @param tmp 临时视图名字。
 * @param var 循环变量名字。
 * @param rhs 要替进去的右值。
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
 * @brief 就地替换语句里所有可写位置上的 `tmp[i]`。
 *
 * 只处理融合接受的那几种语句形状（赋值与表达式语句）；别的形状在资格判定那一步已经被拒。
 *
 * @param c   检查器。
 * @param s   要改写的语句。
 * @param tmp 临时视图名字。
 * @param var 循环变量名字。
 * @param rhs 要替进去的右值。
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
 * @brief 判断 B 的体是不是"每个元素只写自己的槽"的可融合形状，并收集读写名字。
 *
 * 规则（v1 的保守版）：每句只能是 `X[i] = <表达式>` 或表达式语句；写目标必须是"以循环变量
 * 为唯一下标"的视图槽，且写目标名不能出现在任何读位置（否则融合前后跨元素依赖的顺序会变）。
 *
 * @param body   B 的循环体（`ST_BLOCK`，其中含步进语句）。
 * @param tmp    临时视图名字。
 * @param var    循环变量名字。
 * @param writes `const char*` 的 `Vec`，收集写目标视图名。
 * @param reads  `const char*` 的 `Vec`，收集读到的名字。
 * @return 形状合格时为 `true`。
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

static bool fuseMentionOnStmt(void *raw, Stmt *s);

/** `fuseStmtMentions` 的走树上下文：要找的名字与是否已经找到。 */
typedef struct { const char *name; bool hit; } FuseMentionCtx;

/**
 * @brief `fuseStmtMentions` 的表达式回调：命中名字就停下整趟走。
 *
 * @param raw 上下文（`FuseMentionCtx *`）。
 * @param e   被访问的子表达式。
 * @return 没命中时为 `true`（继续走）。
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
 * @brief `fuseStmtMentions` 的语句回调：继续往下走子语句。
 *
 * @param raw 上下文（`FuseMentionCtx *`）。
 * @param s   被访问的子语句。
 * @return 没命中时为 `true`（继续走）。
 */
static bool fuseMentionOnStmt(void *raw, Stmt *s) {
    FuseMentionCtx *m = raw;
    if (!s) return true;
    AstVisit v = { fuseMentionOnExpr, fuseMentionOnStmt, m };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 语句（含所有子语句与其中的表达式）里是否提到某个标识符。
 *
 * 走的是 AST 的通用遍历（`astWalkStmtChildren`），于是新增的语句/表达式种类自动覆盖，
 * 不需要在这里再写一份 switch。
 *
 * @param s    要扫描的语句。
 * @param name 要找的标识符名字。
 * @return 提到时为 `true`。
 */
static bool fuseStmtMentions(Stmt *s, const char *name) {
    if (!s) return false;
    FuseMentionCtx m = { name, false };
    AstVisit v = { fuseMentionOnExpr, fuseMentionOnStmt, &m };
    astWalkStmtChildren(s, &v);
    return m.hit;
}

/**
 * @brief 认 S2 归约循环体里那一句 `acc = acc + <纯表达式>`。
 *
 * 只认"升序、单个累加器、`+`、右值纯且真的读到 tmp"这一种形状：融合把 `tmp[i]` 换成写循环
 * 的右值以后，累加的顺序仍然是 i 递增的同一串加法，逐位不变。`*` 不认（浮点乘法不满足这种
 * 重排的逐位等价，整数虽可但先不加第二种口径），也不认右值里出现 `acc` 自己的写法。
 *
 * @param s    归约循环体里的一句。
 * @param tmp  临时视图名字。
 * @param var  循环变量名字。
 * @param rhs 输出：`acc +` 右边那个纯表达式。
 * @return 形状合格时为 `true`。
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
 * @brief 尝试 S2 归约融合：`var tmp = new …` + 写它的循环 A + `var acc = init` + 升序归约循环。
 *
 * 形状（全中才动手）：`var tmp: mut slice<T> = new …` 在 `i`；A 在 `i+1`；`var acc = init` 在
 * `i+2`（`init` 不是 `new`，即 acc 是标量）；归约循环在 `i+3`，与 A 循环变量同名、两端结构相等；
 * A 体恰好 `tmp[i] = <纯 rhs>`；归约体恰好一句 `acc = acc + <纯表达式>`（`tmp` 只以 `tmp[i]` 读）；
 * `tmp` 在本块其它语句里没有再被提到；A 的右值不读 `acc`（否则融合会把 A 与累加交错）。
 * 命中后：累加语句搬进 A 的体、`tmp[i]` 换成 A 的右值，删掉 `tmp` 声明、归约循环与 A 原位，
 * 把 `acc` 声明挪到融合循环之前。
 *
 * @param c     检查器。
 * @param block 正在被检查的块。
 * @param i     临时视图声明在块里的下标。
 * @return 融合成功时为 `true`（语句表已原地改写）。
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
    /* A 的体：恰好 [tmp[i] = rhs, step]。 */
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
    /* 归约体：恰好一句。 */
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
    /* `tmp` 在这一块的其它语句里没有别的用处。 */
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 3) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    /* 改写：累加语句搬进 A 的体，tmp[i] 换成 A 的右值；acc 声明挪到融合循环之前。 */
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
 * @brief 把容器形式的归约循环改写成索引形式，交给 `fuseTryReduce` 折叠。
 *
 * `for v in tmp { acc = acc + v }` 遍历的是 `tmp.len` 个元素，而上一层元素链 `A` 遍历
 * `[0, hi)`。只有在 `A` 的上界与 `tmp` 的分配长度是同一个表达式时两者才是同一个迭代空间，
 * 所以这里先要求 `fuseExprSame(aHi, new T[len] 的长度)`；成立之后把容器循环换成
 * `for <aVar> in <aHi> { acc = acc + tmp[<aVar>] }`，下一遍扫描就由 `fuseTryReduce` 把它
 * 折进 `A` 的体里。
 *
 * 只认最简单的形状：元素值恰好以 `v` 本身参与累加（`acc = acc + v`），体里也不能有别的
 * 语句；`tmp` 在本块的其它语句里不能再出现。
 *
 * @param c     检查器。
 * @param block 语句块。
 * @param i     `var tmp = new T[n]` 在块里的位置。
 * @return      改写了一处返回 `true`；形状不匹配返回 `false`（不动 AST）。
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
    /* A 的体：恰好 `tmp[aVar] = <纯 rhs>`。 */
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
    /* 容器形式的归约循环：`for v in tmp { acc = acc + v }`。 */
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
    /* `tmp` 在这一块的其它语句里没有别的用处。 */
    for (size_t k = 0; k < n; k++) {
        if (k == i || k == i + 1 || k == i + 3) continue;
        if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) return false;
    }
    /* 换成索引形式的归约循环，迭代空间与 A 相同。 */
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
 * @brief 在块里找**一处**可融合的元素链并改写它（找到就返回，一次只动一处）。
 *
 * 从前往后扫"临时视图声明"，先试 S1（声明后是写它的循环 A，再紧跟读它的循环 B），不成立
 * 再试 S2（声明后是 A、然后是 `var acc = init`、再是升序归约循环 B）。
 *
 * @param c     检查器。
 * @param block 块语句（语句表会原地缩短）。
 * @return 改写了一处时为 `true`。
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
            if (fuseTryReduce(c, block, i) || fuseTryReduceContainer(c, block, i)) return true;
            continue;
        }
        (void)aW;
        (void)bW;
        if (strcmp(aVar, bVar) != 0) continue;
        if (!fuseExprSame(aLo, bLo) || !fuseExprSame(aHi, bHi)) continue;
        if (fuseMentions(aLo, tmp) || fuseMentions(aHi, tmp)) continue;
        /* A 的体：恰好 [tmp[i] = rhs, step]。 */
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
        /* B 的体与读写名字：写目标与读到的名字不许相交（跨元素依赖的顺序会变）。 */
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
        /* `tmp` 在这一块的其它语句里（含子块、分支、match 臂）没有别的用处。 */
        for (size_t k = 0; k < v->len && !clash; k++) {
            if (k == i || k == i + 1 || k == i + 2) continue;
            if (fuseStmtMentions(*(Stmt **)vecAt(v, k), tmp)) clash = true;
        }
        if (clash) continue;
        /* 改写：先替换 B 体里的 tmp[i]，再把 B 的语句搬进 A 的体。 */
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
        /* 顶层删掉 decl 与 B 两句，A（融合后那个循环）留在 decl 的位置上。 */
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
 * @brief 把块里所有可融合的元素链改写到不动点（每处融合都会让语句表变短，必然停下）。
 *
 * 资格见 `fuseOnePass` / `fuseTryReduce` 的注释：S1 是"写临时视图的循环 + 紧邻读它的
 * 循环"，S2 是"写临时视图的循环 + `var acc = init` + 升序归约循环"；两者都要求循环变量同名、
 * 两端结构相等、临时视图在本块其它地方没有引用。
 *
 * @param c     检查器。
 * @param block 正在被检查的块（语句表会原地缩短）。
 */
static void fuseElementChains(Checker *c, Stmt *block) {
    if (!block || block->kind != ST_BLOCK || block->u.block.stmts.len < 4) return;
    if (block->parLoop) return;
    if (c->curFunc && (planIsCoro(c->curFunc) || planIsParWorker(c->curFunc))) return;
    while (fuseOnePass(c, block)) { }
}

/**
 * @brief 对函数体执行一次融合改写（函数体不经 `checkBlockBody`，要单独挂）。
 *
 * @param c     检查器。
 * @param body  函数体块。
 */
void checkFuseFuncBody(Checker *c, Stmt *body) {
    if (!fuseOff()) fuseElementChains(c, body);
}

/* ---------------------------------------------------- 整数组表达式降糖 */

/**
 * @brief 判断一个名字是不是可写的 rank-1 视图，返回它的元素类型。
 *
 * @param c    检查器。
 * @param name 变量名。
 * @return     元素类型（类型同时要求 `mut`）；不是可写视图时返回 NULL。
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
 * @brief 检查整数组表达式的右值形状，并登记里面引用的视图名。
 *
 * 允许的形状刻意小：只读视图名（元素类型相同）、标量名、数字/布尔字面量、`+ - * /` 的
 * 二元组合与一元 `+ -`。任何调用、`new`、下标、切片、字段、`?` 都不在范围内 —— 它们要么
 * 不是逐元素运算，要么自己会分配，降糖后语义会变。
 *
 * @param c     检查器（查名字）。
 * @param e     右值表达式。
 * @param elem  目标视图的元素类型。
 * @param views 输出：找到的视图名（`const char*`，按首次出现顺序，去重）。
 * @return      形状可用返回 true；否则 false（调用方原样放过这条语句）。
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
        /* 标量名：类型等到正常检查时判（不在这里猜字面量的适配）。 */
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
 * @brief 把整数组表达式的右值改写成逐元素形式：每个视图名 `v` 换成 `v[loop]`。
 *
 * @param c    检查器（取 arena）。
 * @param e    已通过 arrayExprShapes 的右值。
 * @param loop 循环变量名。
 * @return     新的表达式子树。
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
 * @brief 复合赋值对应的二元运算符名（`+=` 给 `"+"`）。
 *
 * 整数组表达式的形状表只认 `+ - * /`，所以 `%=` 与其它给 `NULL`（调用方据此放弃降糖）。
 *
 * @param aop 解析器存下的复合赋值运算符文本（`"+="` 等；`NULL` 表示普通 `=`）。
 * @return 静态存储的运算符名；不认的形式给 `NULL`。
 */
static const char *arrayCompoundOp(const char *aop) {
    if (!aop) return NULL;
    if (strcmp(aop, "+=") == 0) return "+";
    if (strcmp(aop, "-=") == 0) return "-";
    if (strcmp(aop, "*=") == 0) return "*";
    if (strcmp(aop, "/=") == 0) return "/";
    return NULL; /* `%=` 不在整数组表达式的形状里 */
}


/**
 * @brief 判断一个类型是不是模块里声明的泛型结构体实例（`ndview<T>`）。
 *
 * 模块结构体的 `sdef->name` 带模块前缀（`nd$ndview`），`isProtoType` 只比较完整名字，
 * 所以这里只比最后一段。
 *
 * @param t    类型。
 * @param want 结构体名（不带模块前缀）。
 * @return     是 `want<T>` 的实例时返回真。
 */
static bool arrayNdProto(const Type *t, const char *want) {
    if (!t || !t->sdef) return false;
    const char *n = t->sdef->name;
    const char *d = strrchr(n, '$');
    return strcmp(d ? d + 1 : n, want) == 0;
}

/**
 * @brief 取一个名字的 `ndview<T>` 元素类型。
 *
 * 整数组表达式的 rank-2 形状直接落成 `stl::nd` 的逐元素内核调用（`addInto` 一类），
 * 所以这里只认"绑定是 `ndview<T>`"这一种，返回它的元素类型。
 *
 * @param c    checker。
 * @param name 名字。
 * @return     元素类型；不是 `ndview<T>` 时返回 NULL。
 */
static Type *arrayNdElem(Checker *c, const char *name) {
    Sym *sym = lookup(c, name);
    if (!sym) return NULL;
    Type *bt = ttBase(sym->type);
    if (!arrayNdProto(bt, "ndview") || bt->targs.len < 1) return NULL;
    return *(Type **)vecAt(&bt->targs, 0);
}

/**
 * @brief 判断一个表达式是不是元素类型匹配的 `ndview<T>` 名字。
 *
 * @param c    checker。
 * @param e    表达式。
 * @param elem 期望元素类型。
 * @return     是匹配的 `ndview<T>` 名字时返回真。
 */
static bool arrayNdName(Checker *c, Expr *e, Type *elem) {
    if (!e || e->kind != EX_IDENT || !e->u.ident.name) return false;
    Type *got = arrayNdElem(c, e->u.ident.name);
    return got && ttEquals(got, elem);
}

/**
 * @brief 判断一个表达式是不是元素类型匹配的标量操作数（`scaleInto` 的 `k`）。
 *
 * @param c    checker。
 * @param e    表达式。
 * @param elem 期望类型。
 * @return     字面量，或类型匹配且不是 ndview 的名字时返回真。
 */
static bool arrayNdScalar(Checker *c, Expr *e, Type *elem) {
    if (!e) return false;
    if (e->kind == EX_INT || e->kind == EX_FLOAT || e->kind == EX_BOOL) return true;
    /* `f64(2.0)` 这类显式转换：检查器还没走到这里，但解析器已经写好转换后的类型。 */
    if (e->kind == EX_CONV) return true;
    if (e->type && ttEquals(ttBase(e->type), elem)) return true;
    if (e->kind != EX_IDENT || !e->u.ident.name) return false;
    Sym *sym = lookup(c, e->u.ident.name);
    if (!sym) return false;
    Type *bt = ttBase(sym->type);
    return !arrayNdProto(bt, "ndview") && ttEquals(bt, elem);
}

/**
 * @brief 造一条 "`if !nd::<fn>(dst, …) { trap }`" 语句，交给检查器按普通调用检查。
 *
 * 内核名用模块扁平名（`nd$addInto`），并打上 `anSetQualified`：加载器的限定名改写发生在
 * 检查之前，检查器里新造的模块调用要走同一条"已限定"标记，才不会撞 `requireQualified`。
 *
 * @param c     checker。
 * @param fn    内核名（不含模块前缀）。
 * @param dst   目标视图名。
 * @param args  其余实参（表达式），按内核签名顺序。
 * @param line  位置。
 * @return      包好的语句。
 */
static Stmt *arrayNdKernelStmt(Checker *c, const char *fn, const char *dst,
                               Expr **args, size_t nargs, int line) {
    Arena *a = c->arena;
    /* 名字要活过本函数：`exprIdent` 只存指针，栈上的缓冲区返回后会被后续调用覆盖。 */
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
    /* 直接返回这一条 `if`：再包一层 `ST_BLOCK` 会多开一个块级 arena，生成的 C 就与手写
     * 调用不再逐字节相同（多一对 `extc_arena_destroy`）。 */
    return stmtIfNew(a, ok, dieBlock, NULL, line);
}

/**
 * @brief 把 `ndview<T>` 目标上的整数组表达式落成 `stl::nd` 的逐元素内核调用。
 *
 * 收的形状（rank-2 第一版）：`dst = a + b`、`dst = a - b`、`dst = a * b`、`dst = a * k`
 * （`k` 在前在后都行）、`dst = a + b * k`，以及它们的复合赋值形式（`dst += a` 等，先造成
 * `dst = dst <op> rhs`）。内核自己按形状检查并返回 `bool`，失败就 trap —— 与 rank-1 降糖的
 * "长度不一致就 trap"同一套判据，只是这里形状是运行期才知道的。
 *
 * @param c checker。
 * @param s 候选赋值语句。
 * @return   降糖后的语句；形状不认识时返回 NULL。
 */
static Stmt *arrayNdLower(Checker *c, Stmt *s) {
    if (!s || s->kind != ST_ASSIGN || !s->u.assign.target || !s->u.assign.value) return NULL;
    if (s->u.assign.op) return NULL;            /* 复合形式先在上面造好，见调用点 */
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
 * @brief 整数组表达式语句的降糖：`out = a + b` → `for i in 0..out.len { out[i] = a[i] + b[i] }`。
 *
 * 只认"目标是可写 rank-1 视图、右值是逐元素表达式"这一种赋值；命中就把语句换成一个块：
 * 先是长度守卫（每个参与视图与目标等长，否则 trap），再是写循环。降糖产物与手写循环完全
 * 同形，所以边界检查消除、向量化与融合都照常作用在它上面。
 *
 * @param c 检查器（此时目标与右值都还没检查过 —— 只做名字查表，不改作用域）。
 * @param s 候选语句。
 * @return  降糖后的替换语句；不该降糖时返回 NULL。
 */
Stmt *arrayAssignLower(Checker *c, Stmt *s) {
    if (!s || s->kind != ST_ASSIGN) return NULL;
    if (!s->u.assign.target || !s->u.assign.value) return NULL;
    Expr *tgt = s->u.assign.target;
    if (tgt->kind != EX_IDENT || !tgt->u.ident.name) return NULL;
    /* 裸视图名不是整数组表达式：`buf = t` 是**视图值**赋值（换掉指针与长度、别名关系照旧），
     * 不是逐元素拷贝。降糖只接带运算的形状。 */
    if (!s->u.assign.op && s->u.assign.value->kind == EX_IDENT) return NULL;
    const char *outName = tgt->u.ident.name;
    /* rank-2 形状（`ndview<T>` 目标）先试：它走 stl::nd 的逐元素内核，不展开循环。 */
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
    /* 复合形式 `out += rhs` 就是 `out = out <op> rhs`：目标按格读一次、写一次，别名安全。 */
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

    /* 长度守卫：`if out.len != v.len { trap }`。视图长度之一是目标自己的长度，跳过。 */
    for (size_t i = 0; i < views.len; i++) {
        const char *vn = *(const char **)vecAt(&views, i);
        if (strcmp(vn, outName) == 0) continue;
        Expr *lhsLen = exprFieldNew(a, exprIdent(a, outName, line), "len", line);
        Expr *rhsLen = exprFieldNew(a, exprIdent(a, vn, line), "len", line);
        Expr *cond = exprBinNew(a, "!=", lhsLen, rhsLen, line);
        Stmt *die = stmtTrapNew(a, exprStrNew(a,
            "array expression operands must have the same length as the destination", line), line);
        /* `if` 的两个分支在解析器里都是块（`parseBlock` 造 ST_BLOCK），照同一个形状造。 */
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
 * @brief 判断一个块是不是区间 `for` 的脱糖块（`[循环变量, 上界, while]`）。
 *
 * `checkBlockBody` 在检查完前两条语句时对区间块做循环变量放宽、对 `par` 块做降糖；降糖
 * 产物的外层块（"长度守卫 + 写循环"）也走同一条检查路径，形状不对时必须跳过，不能把守卫
 * 语句当循环变量声明去改类型。
 *
 * @param block 候选块。
 * @return      形状正好是 `[ST_VAR, ST_VAR, ST_WHILE]` 时返回 true。
 */
static bool rangeForBlockShape(Stmt *block) {
    if (!block || block->kind != ST_BLOCK || block->u.block.stmts.len != 3) return false;
    Stmt *a = *(Stmt **)vecAt(&block->u.block.stmts, 0);
    Stmt *b = *(Stmt **)vecAt(&block->u.block.stmts, 1);
    Stmt *d = *(Stmt **)vecAt(&block->u.block.stmts, 2);
    return a && b && d && a->kind == ST_VAR && b->kind == ST_VAR && d->kind == ST_WHILE;
}

/**
 * @brief 在新作用域里检查一个块体，块结束时块内绑定随之出作用域。
 *
 * 会自行回收存储的块是 place 边界：其中不许挂起（规则 3），通过 `c->placeBoundaryDepth`
 * 让 `ST_YIELD` 看到这一点。
 *
 * @param c      检查器。
 * @param block  要检查的块语句。
 */
static void checkBlockBody(Checker *c, Stmt *block) {
    if (block->forDesugar) forRetargetToIterator(c, block);
    if (!fuseOff()) fuseElementChains(c, block);
    const bool boundary = stmtNeedsPlaceBoundary(block, c->curFunc && planIsCoro(c->curFunc));
    if (boundary) c->placeBoundaryDepth++;

    pushScope(c);
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt **slot = (Stmt **)vecAt(&block->u.block.stmts, i);
        Stmt *lowered = arrayAssignLower(c, *slot);
        if (lowered) *slot = lowered;
        checkStmt(c, *slot);
        /* 区间循环的外层块是 `[循环变量, __extc_hi, while]`：等两者都检查完、`while` 还没
         * 检查时把循环变量放宽，体里的使用才会拿到放宽后的类型（见 widenRangeForVar）。
         * `par` 块在同一个时机降糖：两端类型已定、名字可查（见 parLower）。 */
        if (i == 1) {
            if (block->parLoop) parLower(c, block);
            else if (rangeForBlockShape(block)) widenRangeForVar(c, block);
        }
    }
    popScope(c);

    if (boundary) c->placeBoundaryDepth--;
}

/**
 * @brief 把 `global` 声明登记为帧外存储。
 *
 * 存储不在任何帧里（代码生成发块作用域 `static`）：深度记 0、`outOfFrame` 置位，于是
 * “视图能不能跨 `yield`”这类问题都能得到正确答案。带初始化式与不带初始化式的声明路径都要
 * 调用它：无初始化式那条会提前返回，漏掉它 `global var buf: [8]u8` 就不会被标记，它的视图
 * 会被当成“别人的存储”拒掉。
 *
 * @param c    检查器；本函数不读它，仅为签名统一。
 * @param s    声明语句；用来读 `isGlobal`。
 * @param sym  刚登记的符号。
 */
static void pMarkGlobal(Checker *c, Stmt *s, Sym *sym) {
    if (!sym || !s->u.var.isGlobal) return;
    sym->outOfFrame = true;   /* 深度 0：存储活过每一帧 */
    sym->depth = 0;
    anSetRefDepth(sym, 0);
    /* **函数内的 `global var` 也要登记进全局表。** `isGlobalSym` 只查这张表；漏登记的话
     * `storeLayer` 会把它的 depth-0 槽当成"参数槽"（check_escape.c 里
     * `sy->depth == 0 && !isGlobalSym(...) ⇒ return 1`），于是**存进去的对象被分配在本帧
     * arena**，函数返回即悬垂。实测：`global var g: ?ref node` 里 `g = new node`，第二次
     * 调用读 `g` ⇒ plain 段错误 / ASan heap-use-after-free
     * （用例 `tests/asan/global-frame-holds-new.extc`）。
     *
     * `checkGlobals` 早于函数检查运行，所以这张表里追加函数内的全局不会影响那里的
     * 模块级重名检查（它比的是名字，而 `isGlobalSym` 比的是符号指针）。 */
    *(Sym **)vecPush(&c->globals) = sym;
}

/**
 * @brief 判断一个表达式有没有可被赋值写入的存储。
 *
 * 看的是整条**基链**，不是最外层形状：`f().x` 外面是字段访问，但字段属于临时值，写进去
 * 随即消失；`!` 也只有在操作数是 place 时才构成 place。
 *
 *   EX_IDENT                        -> place（绑定）
 *   EX_FIELD / EX_INDEX / EX_SLICE  -> 所选对象是 place 才是
 *   EX_DEREF                        -> place（`*p` 命名一个对象）
 *   EX_SIGN                         -> 解包后的操作数是 place 才是
 *   其余                            -> 没有存储
 *
 * 一个例外是**引用载荷的 `!` 站在基链上**：`f()!.x = v` 里 `f()` 返回 `?ref T`，`!` 断言出来
 * 的是指针，写的是指针指向的对象，所以断言本身不是 place 也无所谓。它只能站在基链上：作为
 * 赋值目标本身（`f()! = v`）时，写进去的仍是载荷临时值。
 *
 * 有一种形状靠语法走不出来：视图（`slice<T>`）上的 `s[i]`，元素真实存在于 `s.data + i`，
 * 即使视图值本身是临时值。调用方（`ST_ASSIGN`）在本函数说否时再按基类型解析一次，固定数组
 * （`f()[i]`）与视图也在那里区分。
 *
 * @param e  赋值目标表达式。
 * @return   目标是可写 place 则返回 true；否则返回 false。
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
 * @brief 判断表达式是不是可写 place（见上一条的基链规则）。
 *
 * @param e  赋值目标表达式。
 * @return   目标是可写 place 则返回 true；否则返回 false。
 */
static bool assignTargetWritable(const Expr *e) {
    return assignTargetWritableAt(e, false);
}

/* ------------------------------------------------- 无环判定: 引用槽存储的分类
 *
 * `computeFnAcyclic`（check_top.c）要判「这个函数建立的堆引用图有没有回边」以及「引用是不是只
 * 住在被计数的堆槽里」。判定需要两类事实，只有在**作用域还活着**时拿得到：
 *   · 宿主对象的层上界（`(*cur).f` 的宿主是被指物，要 `Sym.refDepth`）；
 *   · 值的层下界（`ref <局部量>` 取该绑定的块深度，经引用绑定的取 `Sym.minPointee`）。
 * 所以分类在这里做，结果记在语句的结果槽里（`anSetStoreRefKind`），后一趟只做调用图闭包。
 * 判据 v1 的四条与 `SR_*` 编码见 `check_internal.h`。
 */

/**
 * @brief 投影基表达式所命名的**宿主对象**的层上界（越深 = 越年轻）。
 *
 * 只处理「宿主是堆对象」的那一支：基是一个引用型绑定（它指向的对象才是宿主）或者一次
 * 解引用。返回 -1 表示这个基不是引用（宿主是栈上的局部量/参数/全局 ⇒ 归判据 (iv) 那一栏，
 * 由后一趟的语法扫描判），或层不可知（保守）。
 *
 * 用 `anRefDepth`（`Sym.refDepth` 是"可能被指物"的上界）当**上界**：层判据要的是"宿主可能
 * 有多年轻"，上界越年轻越保守（值必须比它更年轻才放行）。
 *
 * @param c    检查器（作用域还活着）。
 * @param obj  投影的基表达式。
 * @return     宿主层上界；不是堆宿主或不可知时 -1。
 */
static int acyclicHostUpper(Checker *c, Expr *obj) {
    if (!obj) return -1;
    switch (obj->kind) {
    case EX_DEREF: {
        /* `(*p).f` 的宿主是 `p` 指向的对象：层上界就是 `p` 的 refDepth。`p` 是参数还是局部量
         * 不影响这一支 —— 经引用写永远打在被指物上。 */
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
        /* 引用型绑定作基（`p.f = v`）：宿主是它指向的对象。 */
        if (tsub(c, sy->type)->kind == TY_REF) return anRefDepth(sy);
        return -1;                       /* 普通绑定作基：宿主在栈上 */
    }
    case EX_FIELD: return acyclicHostUpper(c, obj->u.field.obj);
    case EX_INDEX: return acyclicHostUpper(c, obj->u.index.obj);
    case EX_SLICE: return acyclicHostUpper(c, obj->u.slice.obj);
    default:       return -1;
    }
}

/**
 * @brief 写目标里引用槽的宿主对象的层上界。
 *
 * `EX_DEREF` 的宿主就是被指物本身；字段/元素/切片看它们的基。
 *
 * @param c  检查器（作用域还活着）。
 * @param t  写目标表达式。
 * @return   宿主层上界；宿主不是堆对象或不可知时 -1。
 */
static int acyclicStoreHostUpper(Checker *c, Expr *t) {
    if (!t) return -1;
    switch (t->kind) {
    case EX_DEREF: return acyclicHostUpper(c, t);
    case EX_FIELD: return acyclicHostUpper(c, t->u.field.obj);
    case EX_INDEX: return acyclicHostUpper(c, t->u.index.obj);
    case EX_SLICE: return acyclicHostUpper(c, t->u.slice.obj);
    default:       return -1;    /* EX_IDENT：改指本帧的槽，不是堆槽 */
    }
}

/**
 * @brief 写目标里那个「宿主绑定」本身（与 `acyclicHostUpper` 走同一条基链）。
 *
 * `(*cur).f = v` 的宿主是 `cur` 指向的对象，而 `cur` 就是这里要的绑定 —— 它可能指向的分配站点
 * 构成「候选区域」集合（见 `CycleMerge`）。`p.f = v`（`p` 是引用型绑定）同理。
 *
 * @param c  检查器（作用域还活着）。
 * @param obj 投影的基表达式。
 * @return    引用型绑定；宿主不是堆对象或不可知时 NULL。
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
 * @brief 写目标对应的宿主绑定（`(*cur).next` → `cur`；`p.f` → `p`）。
 *
 * @param c  检查器。
 * @param t  写目标表达式。
 * @return    宿主绑定；不是经具名引用的堆宿主时 NULL。
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
 * @brief 写进引用槽的那个值「至少活到哪一层」的**下界**（越小 = 至少活得更久）。
 *
 * 层判据 (iii) 要判定"值比宿主年轻"，所以值这边必须是**下界**（不能拿偏大的上界去放行，
 * 那正是判据健全的反方向）。只有本来就不会被后续层求解器抬高的形状才答数：
 * 直接分配站点（(i) 已另行放行）、`ref <局部量>`（该绑定的块深度，求解器不移动局部槽）。
 * 经引用绑定/字段读的值一律答 -1（`Sym.minPointee` 会被后续求解器抬高，不能当下界）。
 *
 * @param c  检查器（作用域还活着）。
 * @param v  被存的值。
 * @return   层下界；不可知时 -1。
 */
static int acyclicValueLower(Checker *c, Expr *v) {
    if (!v) return -1;
    switch (v->kind) {
    case EX_NEW: case EX_GENCALL: {
        int a = planArenaLevel(v);
        return a == ARENA_HOME ? 0 : (a > 0 ? a : -1);
    }
    case EX_REF: {
        /* `ref x` 指向 x 的存储本身：x 是局部量时深度就是它的块深度（精确、稳定）。 */
        Expr *o = v->u.ref.operand;
        if (!o || o->kind != EX_IDENT) return -1;
        Sym *sy = lookup(c, o->u.ident.name);
        if (!sy || !sy->type) return -1;
        if (tsub(c, sy->type)->kind == TY_REF) return -1;   /* `ref *p`：经引用，答 -1 */
        return slotDepth(c, o);
    }
    case EX_SIGN: return acyclicValueLower(c, v->u.sign.operand);
    case EX_TRY:  return acyclicValueLower(c, v->u.try_.operand);
    case EX_CONV: return acyclicValueLower(c, v->u.conv.operand);
    case EX_COALESCE: {
        int a = acyclicValueLower(c, v->u.coalesce.main);
        int b = acyclicValueLower(c, v->u.coalesce.fallback);
        if (a < 0 || b < 0) return -1;
        return a < b ? a : b;       /* 两支都可能成为结果 ⇒ 取更长寿的那支 */
    }
    default: return -1;
    }
}

/**
 * @brief 一次存储的引用槽分类；`old` 与 `now` 取**更保守**的那一个。
 *
 * 同一个站点可能被检查多趟（驱动器的多轮），分类因此可能重复写入：任何一趟说"不成立"，
 * 结论就是不成立；两趟都是调用来源时保留"要查被调者"（要求更多）。
 *
 * @param old  已经记下的分类（`SR_NONE` = 还没记）。
 * @param now  这一趟的分类。
 * @return     合并后的分类。
 */
static int acyclicMergeKind(int old, int now) {
    if (old == SR_NONE) return now;
    if (old == SR_BAD || now == SR_BAD) return SR_BAD;
    if (old == now) return old;
    if (old == SR_LOCAL || now == SR_LOCAL) return SR_LOCAL;
    if (old == SR_CALL || now == SR_CALL) return SR_CALL;
    return SR_NULL;   /* 两条静态理由都成立：留一个"可以放行"的码 */
}

/**
 * @brief 记下一次「往引用槽里写」的分类（判据 (i)(ii)(iii)(i')）。
 *
 * 只处理**宿主是堆对象**的写：栈上的引用槽（引用型局部量/参数的改指、局部聚合的字段）不是
 * 这里的事，它们由后一趟的语法扫描按判据 (iv) 判否。**必须带类型**：目标类型不含引用时直接
 * 返回，`i = i + 1` 这种整型赋值不会被算进来。
 *
 * @param c       检查器（作用域还活着）。
 * @param s       赋值语句（分类记在它的结果槽上）。
 * @param target  写目标。
 * @param value   被写入的值。
 */
static void noteAcyclicStore(Checker *c, Stmt *s, Expr *target, Expr *value) {
    if (!s || !target || !value) return;
    Type *tt = target->type ? tsub(c, target->type) : NULL;
    if (!tt) return;
    /* 槽装**被计数的**引用吗？提及类型参数的类型也按"可能装引用"算（泛型体本来就不置位）；
     * 视图句柄不算（见 `typeHoldsCountedRef`）。 */
    if (!typeHoldsCountedRef(c->tt, tt) && !mentionsParam(tt)) return;
    int host = acyclicStoreHostUpper(c, target);
    if (host < 0) return;                  /* 栈上的槽：判据 (iv) 那一栏 */
    int kind;
    if (value->kind == EX_NULL) {
        kind = SR_NULL;
    } else if (value->kind == EX_NEW || value->kind == EX_GENCALL) {
        kind = SR_FRESH;
    } else if (value->kind == EX_CALL || value->kind == EX_METHOD
               || value->kind == EX_ASSOC) {
        /* (i')：等后一趟看被调者是不是无环、且返回值可证是新分配的。认不出被调者也在这一栏，
         * 后一趟会按"解析不到 ⇒ 判否"处理。 */
        kind = SR_CALL;
    } else {
        int vl = acyclicValueLower(c, value);
        if (vl >= 0 && vl > host) kind = SR_LAYER;
        else                      kind = SR_BAD;
    }
    anSetStoreRefKind(s, acyclicMergeKind(anStoreRefKind(s), kind));
    /* 可能闭环：这条写既不是空、也不是新分配、也不是严格更年轻的层 ⇒ 环可能横跨区域。记下来
     * （宿主可能指向的分配站点在这一刻当场收，作用域还开着），等全局电平求解之后把这些候选
     * 区域并到最浅的那一只（见 `CycleMerge` / `applyCycleMerges`）。 */
    if (kind == SR_BAD) {
        Sym *base = acyclicStoreHostSym(c, target);
        if (base) noteCycleMerge(c, base, value, s->line);
    }
}

/**
 * @brief 记下一个**嵌套块里的引用型局部量**：判据 (iv) 判否的那一栏。
 *
 * 事件路径只数"被计数的 rc 槽 → 对象"这条边。函数体顶层的引用绑定随帧一起消失，对象最多多活
 * 一次调用（有界）；**嵌套块**（循环体、分支、裸块）里的绑定每轮新建一个、块退出时不产生
 * `dec` ⇒ 它**唯一**持有的对象成了没人销账的垃圾，峰值随轮数线性涨（`blind_localhold` 形状）。
 *
 * 只在初始化式**自己产生一个持有者**时判否：`new` / 调用结果 / 结构体与数组字面量 / 另一个
 * 绑定。读一个已有槽（`let cur = head.next`）不算 —— 那个对象还有被计数的持有者。
 *
 * @param c  检查器（作用域还活着，`scopes.len` 就是块深度）。
 * @param s  声明语句。
 */
static void noteAcyclicNestedLocal(Checker *c, Stmt *s) {
    if (!s || !s->type || s->u.var.isGlobal) return;
    if (!typeHoldsCountedRef(c->tt, tsub(c, s->type)) && !mentionsParam(s->type)) return;
    if (c->scopes.len <= 1) return;           /* 函数体顶层：有界 */
    Expr *ini = s->u.var.init;
    if (!ini) return;                         /* 声明成空引用：没有引用要丢 */
    switch (ini->kind) {
    case EX_NEW: case EX_GENCALL: case EX_CALL: case EX_METHOD: case EX_ASSOC:
    case EX_STRUCTLIT: case EX_ARRAYLIT: case EX_ENUMVAL: case EX_IDENT:
    case EX_LAMBDA: case EX_DYN:
        anSetStoreRefKind(s, SR_LOCAL);
        break;
    case EX_REF: {
        /* `ref *p` 借的是被指物（可能是本帧引用绑定唯一的持有者）；`ref x` 只是局部量的地址，
         * 不产生堆对象。 */
        Expr *o = ini->u.ref.operand;
        if (o && o->kind == EX_DEREF) anSetStoreRefKind(s, SR_LOCAL);
        break;
    }
    default:
        break;                                /* 读已有槽：对象还有被计数的持有者 */
    }
}

/**
 * @brief 检查一条语句并落实它带来的深度与收窄后果。
 *
 * 进入时清零本语句的副作用计数，然后按语句种类分派：声明解析注解并登记绑定，赋值走写点
 * 入口，`if` / `while` 记录条件证明的事实并检查分支，`match` 逐臂开作用域，块与 domain 块
 * 各走自己的作用域路径。
 *
 * 副作用计数按语句清零：`??` 只有在同一语句中它之前没有副作用时才能把主题提升进临时值，
 * 因为那个临时值在语句顶部求值。
 *
 * @param c  检查器。
 * @param s  要检查的语句。
 */
void checkStmt(Checker *c, Stmt *s) {
    c->stmtFx = 0;      /* 每条语句从头计副作用 */
    switch (s->kind) {
        case ST_VAR: {
            /* 注解在这里解析：解析器只造未解析的类型名。 */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            /* `coroutine<T>` 是返回类型而不是存储；`let c = counter(n)` 是协程值取名（类型即
             * 帧）的写法。 */
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* 没有初始化式就是零初始化，解析器保证此时有注解。 */
            /* 句柄没有有意义的零值（空帧、坏任务）：未初始化的 `coroutine<T>` 是错误。 */
            if (!s->u.var.init && s->u.var.ann && isProtoType(s->u.var.ann, "coroutine", 1)) {
                ckError(c, s->line,
                        "a `coroutine<T>` value has no zero: it is a handle to a frame living in some"
                        " task's place. Initialize it (`var c: coroutine<i64> = worker(...)`)",
                        "`%s` is declared `coroutine<T>` with no initializer",
                        planCName(s) ? planCName(s) : "?");
                s->u.var.ann = ttError(c->tt);
            }
            if (!s->u.var.init) {
                /* 注解里有 `T`，零值有无此刻定不下来：把检查记给实例化。 */
                if (s->u.var.ann && mentionsParam(s->u.var.ann))
                    recordZeroCheck(c, s->u.var.ann, s->line, s->u.var.name);
                else if (s->u.var.ann && typeLacksZeroValue(c->tt, s->u.var.ann)) {  /* 无类型参数：可判定 */
                    /* 没有零值有两种原因，报错必须点名是哪一种：空的 `ref` 是个看着会悬垂的值，
                     * 而空的 `fn` 是被调用时跳到地址 0 的代码指针。 */
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
                /* 零初始化、又能装引用的值只可能装空引用（非空引用没有零值，上面已拒），所以
                 * 其中的引用都指向深度 0，而不是槽的深度：用槽深度会误拒
                 * `var a: [2]?ref node  a[0] = ref *p  return a`。 */
                if (s->type && typeContainsRef(c->tt, s->type)) anSetRefDepth(sym, 0);
                planSetCName(s, sym->cname, RKIND_STMT);
                return;
            }

            /* `let` 是只读绑定，类型不能带 `mut`：同一处同时要只读和可写自相矛盾。 */
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

            /* `@overwrite` 针对一次分配，两查一标记：
             *   - 绑定必须是 `var`：每轮执行都把名字重新指向同一块存储，通常还要写它
             *   - 初始化式必须是 `new`：值、局部量或调用结果没有可复用的存储
             *   - 标记要在检查初始化式之前落下，级别按函数体而不是外层块计算 */
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

            /* `let x = new T` 的存储会一直是零：这份分配只有这个名字一个引用，而 `let` 交出来
             * 的是只读引用，只读引用既不能写也不能变回可写（`ref *n`、`mut ref` 参数、
             * `x.f = v` 都被拒），没有路径能写它。
             *
             * 报警告而不是报错，因为有一个正当的边角：把全零缓冲交给只读消费者，如
             * `let buf = new [64]u8  hash(buf[..])`。`var w = new T  let r = w` 不警告：对象有
             * 可写来源，把已有的可写别名变成只读视图正是 `let` 的用途。 */
            /* 只有**分配原语**才算“写穿这个名字”。此刻初始化式还没做类型检查（在后面），所以
             * 判据是语法形状：`new T` / `new T[n]`，外加生成器按名字认的分配原语。返回标量的
             * 泛型调用不分配任何东西，比如 `let n = copyInto<T>(...)`。 */
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

            /* `let x = e?` 是允许 `?` 的位置之一：有注解时由注解决定是否解引用，没有注解时
             * 初始化式是值位置。 */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            /* 没有注解就保留自然类型，引用仍是引用；要拷贝值写 `let v = *p`。在这里解引用会让
             * 调用返回的引用无法绑定，如 `let r = pickFirst(ref a, ref b)`。 */
            else {
                it = checkExpr(c, s->u.var.init);
                /* 无标注的绑定（`var b = a`）不走 `checkInto` 而走 `checkExpr`，所以“按值
                 * 拷贝一个共用存储的容器”要在这里单独检查，否则 `var b = a` 一条警告都不报。 */
                warnSharedCopy(c, s->u.var.init, it);
            }

            /* 流借用是唯一不许绑定的引用：它是运算符自己对流的借用，不是用户要的引用。 */
            rejectStreamBorrow(c, s->u.var.init);

            /* 要在初始化式检查之后决定：`checkExpr` 会按实参先定 home 深度，这次调用再用值
             * 真正被存放的深度覆盖它。 */
            markCallHomeIfEscaping(c, s->u.var.init, (int)c->scopes.len);

            /* 为 `let` 推断出的类型降级成只读：`let p = ref x` 得到 `p: ref T` 而不是
             * `mut ref T`，`let s = a[..]` 得到 `s: slice<T>`。权限在类型里、随每次拷贝传播，
             * 所以 `var q = p` 也洗不回可写引用。 */
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

            /* 初始化式不许指向先于本绑定死去的局部量。 */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            planSetCName(s, sym->cname, RKIND_STMT);
            noteAcyclicNestedLocal(c, s);   /* 判据 (iv)：嵌套块里引用型局部量 */
            /* 记下这个绑定的值从哪来（初始化式）。提权沿链回溯：`var n = new node` 之后的
             * `head = n` 会顺 `n` 找到 `new` 站点并把它提到 `head` 的级别；没有初始化式的声明
             * 记 NULL。 */
            noteOrigin(c, sym, s->u.var.init);
            if (sym && !sym->addressed) sym->heldSrc = s->u.var.init;
            /* 调用做初始化式 ⇒ 记一格“池站点”（`poolSite`），末轮重放提权时用它。这里不问
             * `makesPool`：那个闭包要等所有函数体检查完才算得出来。 */
            /* 不按形状认：`vector<i32>::new()` 在解析期是 `EX_GENCALL`，检查器**就地**改写成
             * `EX_ASSOC`/`EX_CALL`，按 kind 认会漏。记原表达式即可，末轮重放时才需要它，那时
             * 形状已经定下来。 */
            if (sym && !sym->addressed && s->u.var.init && s->u.var.init->kind != EX_NULL)
            {
                sym->poolSite = s->u.var.init;
                if (getenv("EXTC_DBG_ZONE"))
                    fprintf(stderr, "[decl] %s kind=%d poolSite=%p\n", sym->name ? sym->name : "-",
                            (int)s->u.var.init->kind, (void *)sym->poolSite);
            }
            /* 持有引用的绑定，指向物的深度由初始化式算出：`var cur: ?ref node = head` 里 head
             * 是参数，所以游标深度 0，可以被返回。 */
            /* 调用做初始化式（`let t = make()`）要单独记账：结果里的引用落在哪一层只有调用点
             * 知道，因为 `arenaArg` 由调用点选定，而下面填的字段表只覆盖结构体字面量。漏掉它，
             * 字段 `t.p` 就没有条目，`targetDepth(t)` 回落到 `anRefDepth(sym)` 得 0，之后把 `t`
             * 存进更浅的 `b` 看着安全，直到块退出释放节点。
             *
             * 规则：被调方有 home arena 时，它分配到本次调用传入的 arena，也就是上面
             * `markCallHomeIfEscaping` 按 `at` 定下的级别，所以结果深度是 `at`。这里 `at` 是
             * `c->scopes.len`，绑定所在的级别。 */
            /* `global` 的存储在帧外（代码生成发块作用域 `static`）：深度 0、`outOfFrame` 置位，
             * 于是“视图能不能跨 `yield`”自动得到正确答案。初始化式必须是常量：静态存储期对象
             * 在程序启动时初始化一次，C 的静态初始化式只允许常量表达式。 */
            if (sym && s->u.var.isGlobal) {
                /* 不查 `@overwrite` 与 `global` 的冲突：注解与 `global` 都必须在 `var` 之前，
                 * `@overwrite global var` 在语法上就写不出来。 */
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
                    d = (int)c->scopes.len;      /* 被调方有 home arena 时用我的 */
                /* 初始化式签为帧外（`effects Ret=0`，或根源是 `global` 的链）的存储根本不在
                 * 帧里，上面的 home arena 启发式不适用，深度**确定**是 0，而不是“还没定”。视图
                 * 绑定的 `.data` 交给无签名的 C 函数后还能继续用，靠的就是这一点。 */
                /* 结论写进 plan 侧表：此刻作用域还活着，而 `coroCheckDeferred` 在函数体检查完
                 * 之后才跑。 */
                bool oo = exprOutOfFrame(c, ini);
                planSetVarInitOutOfFrame(s, oo);
                if (oo) {
                    sym->outOfFrame = true;
                    anSetRefDepth(sym, 0);
                } else {
                    anSetRefDepth(sym, d);
                }
                /* 引用绑定：**可能指向的最浅处** + 别名来源。
                 *
                 * 目的地 `(*cur).f` 的深度要的是"可能活得最久的被指物"（参数给 0、帧内 `new`
                 * 给 1，取 min = 0），不是 DFA 的 max。`var head = a` 这种**拷贝**还要留下 a
                 * 这个别名（字段表里的来源）：head 与 a 共享所指对象，而 a 之后会被改指
                 * （`a = p`），展平进 `origin` 的那份是拷贝当时的来源，追不到后来的站点。
                 * `promoteFieldsAt` 在 `return head` 时沿这条来源问 a 当前指向什么，于是循环里
                 * 那个 `new` 也被提到 home。
                 *
                 * 零值/字面量与**新分配**不算：`var cur = null` 没有指向调用者的存储，拿它把
                 * min 拉到 0 会让 `(*cur).f = v` 的目的地整个冤枉地要求到 home；`var p = new node`
                 * 的来源已经由 `origin` 覆盖，再记一条只会给 `cur = p` 这类改指添一条无谓要求。 */
                if (sym && tsub(c, s->type)->kind == TY_REF) {
                    Expr *ini = s->u.var.init;
                    ExprKind ik = ini->kind;
                    bool isLit = (ik == EX_NULL || ik == EX_INT || ik == EX_FLOAT ||
                                  ik == EX_BOOL || ik == EX_STR);
                    if (!isLit) {
                        /* 用 482-499 刚刚定下的 `refDepth`，不要重算 `targetDepth(ini)`：调用结果
                         * 是例外（带 home 的被调方分配进哪个 arena 由调用点定，见 484-486），
                         * `targetDepth(EX_CALL)` 会答 0，而那份记录（`refDepth`）才作准。 */
                        int dd = anRefDepth(sym);
                        /* 初始化式是**另一个引用绑定**时这是一次别名拷贝（`var ocur = cur`）：
                         * 二者共享所指对象，深度要随来源走（见 `symMinPointee` 的别名链），
                         * 不在这里快照 —— 快照取的是拷贝那一刻的词法值，第一个迭代里
                         * `cur = null` 会让它记成 0，把一个健全形状误拒。 */
                        bool alias = false;
                        if (ini->kind == EX_IDENT) {
                            Sym *src = lookup(c, ini->u.ident.name);
                            alias = src && src->type && src->type->kind == TY_REF;
                        }
                        if (!alias) sym->minPointee = dd;
                        /* 只记**来源**，不碰 `otherDepth`/深度记账：那两格是深度上界，
                         * `targetDepth` 会读走；把声明也当成一次字段写，普通形状的深度就会
                         * 多算一层（`head = p` 于是被误拒）。来源**不展平**：`var head = a`
                         * 要留 a 这个别名，a 之后被改指（`a = p`）时提权才追得到新站点。 */
                        sym->initSrc = ini;
                        sym->initSrcDepth = dd;
                        if (sym->initMinReq == 0 && dd > 0) sym->initMinReq = dd;
                    }
                }
            }
            /* 结构体字面量为每个写到的字段记一个深度。省略的字段零初始化，只可能装空引用、
             * 深度 0：不给它建条目，读时回落到保守答案——代价是可能误拒，不会漏。 */
            if (s->u.var.init && s->u.var.init->kind == EX_STRUCTLIT && sym) {
                for (size_t fi = 0; fi < s->u.var.init->u.lit.inits.len; fi++) {
                    FieldInit *fip = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, fi);
                    c->curStoreVal = fip->value;          /* 是哪个值写了这个字段 */
                    noteFieldDepthWrite(c, sym, fip->name, targetDepth(c, fip->value));
                }
                c->curStoreVal = NULL;
                /* 结构体字面量的字段表是完整的：写到的字段都有条目；省略的字段要么是零值为空的
                 * 可空引用（深度 0），要么没有零值而在填零时已被拒。没有字段能持有未记录的值。 */
                sym->fieldsComplete = true;
            }
            return;
        }

        case ST_ASSIGN: {
            /* 左侧必须是一个 **place**：绑定、place 的字段/下标/切片、`*` 解引用，或 place 的
             * 载荷 `o!`。判据是上面的 `assignTargetWritable`：它走整条**基链**，因此
             * `f().x = v` 与 `p.get(h)!.x = v`（载荷是值）都会被拒；载荷是**引用**的
             * `f()!.x = v` 例外——写的是指针指向的对象，基链上真的有一个 place。 */
            Expr *tgt0 = s->u.assign.target;
            /* 被标为“存储在本帧之外”的绑定一旦再次被写就不再可信：否则
             * `if c { v = mem[..] } else { v = buf[..] }` 会继承某一分支的信任，让另一分支的帧内
             * 视图流到 C。被重新赋值的绑定回到槽深度，是保守而刻意的。 */
            if (tgt0 && tgt0->kind == EX_IDENT) {
                Sym *tsy = lookup(c, tgt0->u.ident.name);
                if (tsy) tsy->outOfFrame = false;
            }
            Type *idxBaseT = NULL;    /* 下标目标的基类型，解析后填上 */
            if (!assignTargetWritable(tgt0)) {
                /* 严格遍历失败了。有一种形状离开基类型判不了：视图上的 `s[i]`。只在这条路上解析
                 * 基类型（合法代码不会被重复检查），结果留给下面的 `[]=` 改写复用。 */
                bool viewElem = false;
                if (tgt0 && tgt0->kind == EX_INDEX) {
                    idxBaseT = checkExpr(c, tgt0->u.index.obj);
                    viewElem = idxBaseT && !ttIsError(idxBaseT) && ttIsViewType(idxBaseT);
                }
                /* 另一种形态同理：基链的根是 `!`，而断言出来的是引用（`?ref T`）时，写的是
                 * 指针指向的对象。这一步靠语法判不了——目标还没被检查，`!` 的类型还没写上
                 * ——所以只在这条失败路径上解析根的 `!` 操作数。 */
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
            /* `x += y` 就是 `x = x + y`：是哪个运算符、对这两个类型适不适用，都交给书写 `x + y`
             * 走的同一个函数判定，规则不会分叉；这里只剩赋值那一半——目标可写、结果放得下。 */
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

            /* 定义了 `[]=` 的类型上，`x[i] = v` 就是对该方法的调用 `x.[]=(i, v)`。语句就地改写，
             * 于是实参检查、`mut ref` 接收者规则、返回类型与代码生成走的都是普通调用。`[]` 与
             * `[]=` 是两个独立名字，只定义 `[]` 时下面的诊断说明为什么不能赋值。 */
            if (s->u.assign.target && s->u.assign.target->kind == EX_INDEX) {
                Expr *tgt = s->u.assign.target;
                /* `idxBaseT` 只在上面可写性遍历已经解析过基类型时非空；复用它避免重复检查。 */
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
                    s->kind = ST_EXPR;          /* 读取都完成后才覆盖联合体 */
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

            /* 目标是收窄过的绑定时，比较用槽的声明类型：收窄改的是读出来的结果，不是槽能装
             * 什么。这让 `while cur != null { cur = cur.next }` 合法——赋值在循环体里注销证明，
             * 循环条件下一轮再证一次。 */
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *slot = lookup(c, s->u.assign.target->u.ident.name);
                if (slot && slot->type && slot->type->kind == TY_REF && slot->type->nullable &&
                    tt_->kind == TY_REF && !tt_->nullable) tt_ = slot->type;
            }

            /* 目标是引用类型：这次赋值要么写它所指的对象，要么改指该引用，由右边的类型在下面
             * 决定；把值赋给引用必须与所指类型相符。 */
            if (tt_->kind == TY_REF) {
                /* 这里不适用 `requireMutable`：它问的是引用类型带不带 `mut`，而改指写的是槽本身
                 * （变量或字段）。只有绑定需要可写，所以 `var p: ref T = ref x  p = ref y` 合法。 */
                /* 改指写的是槽本身，所以只有槽需要可写：
                 *   - 变量、字段或元素要求可写的根（`var`）
                 *   - `*p` 的可写性来自 `p` 的类型，即 `mut ref T` */
                /* 先走路径，再看落到的槽：
                 *   - 路上跨过的每个引用都必须是 `mut ref`（`*p`、`cur.f`、`(*p).f`）
                 *   - 跨过引用意味着存储在被指物内部，本帧没有根，遍历到此为止
                 *   - 没有跨过引用时，存储是本帧某个绑定的槽，该绑定必须可写 */
                bool crossed  = false;
                bool refsOk   = pathRefsAllMut(s->u.assign.target, &crossed);
                bool slotOk   = refsOk;
                if (slotOk && !crossed) {
                    Sym *slotRoot = placeRoot(c, s->u.assign.target);
                    slotOk = slotRoot && slotRoot->mut;
                }
                if (!slotOk) {
                    /* 两种原因给两条不同消息，否则用户会在没问题的绑定里找错。 */
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

                /* 右边是什么决定这是哪种操作，靠类型消歧而不是禁掉一种：
                 *   右边是值 `T`        => 写入所指对象
                 *   右边是引用 `ref T`  => 改指该引用 */
                /* `null` 需要引用本身（`?ref T`）作上下文而不是所指物：`p.next = null` 的右边
                 * 是引用，所以走这条改指路径。 */
                adoptContextType(v, v->kind == EX_NULL ? tt_ : tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* 自然类型：引用保留 */

                /* 丢弃非空证明要等右边检查完：`cur = cur.next` 右边的 `cur` 还依赖循环条件给的
                 * 证明。提前丢弃会造成误拒。 */
                if (s->u.assign.target->kind == EX_IDENT)
                    unNarrow(c, planCName(s->u.assign.target));

                if (vt0->kind == TY_REF) {
                    /* 改指：类型要对齐（`mut ref` 降为 `ref` 允许），新目标不能先于引用死去。
                     *
                     * 深度比较用的是旧的 `refDepth`，即槽原先所指物的深度。要等这些检查做完才能
                     * 更新它，否则新值是拿自己跟自己比，`p = ref <更深的局部量>` 会整个滑过去。 */
                    int at0 = storeLayer(c, s->u.assign.target);
                    /* 这个引用要达到的生命周期被容器本身封顶：`storeLayer(h.q)` 走 `slotDepth`
                     * 会答出投影的深度（2），而 `h` 这个局部量活在 1 层，把站点定在 2 会在块结束
                     * 时释放它，尽管 `h.q` 里的指针已经随 `out = h` 逃出帧。
                     *
                     * 封顶值是 `c->scopes.len`：本帧里没有东西活得过当前作用域，这是这次存放能
                     * 要求的最浅层。只有记账用的数被封顶，交给下面 `checkEscape` 的 `at0` 不动。 */
                    /* 先试提权：这个值里的每个 `new` 都能活到 `at0` 层吗？不能就交给下面的深度
                     * 检查。必须在 `checkEscape` 之前跑，因为它要读这个值的深度。 */
                    promoteInto(c, v, at0);
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    /* 目的地经具名引用到达（`(*cur).next = v`）时，这个 `at0` 读的是 body walk
                     * 里按词法顺序写的 `anRefDepth` —— 后面的 `cur = p` 还没走到，词序会改变
                     * 答案。判定因此推迟到 DFA 之后按「可能被指物的最浅层」复判（见
                     * `recheckPendingRefStores`）；其余形状照旧当场报。 */
                    checkEscapeDefer(c, v, at0, s->line, "this reference", s->u.assign.target);
                    /* 改指时所指深度一起搬（`cur = cur.next`）。 */
                    if (s->u.assign.target->kind == EX_IDENT) {
                        Sym *slot0 = lookup(c, s->u.assign.target->u.ident.name);
                        if (slot0 && slot0->type && slot0->type->kind == TY_REF) {
                            int nd = targetDepth(c, v);
                            /* 目的地 `(*cur).f` 要的可能被指物深度取 **min**：参数（0）与帧内
                             * `new`（1）都赋过 ⇒ 0。`null` 不算来源（它没有存储，值本身不是
                             * 调用者的东西），拿它把 min 拉到 0 只会白白多要一层。 */
                            if (v->kind != EX_NULL && (slot0->minPointee < 0 || nd < slot0->minPointee))
                                slot0->minPointee = nd;
                            anSetRefDepth(slot0, nd);
                        }
                        /* 来源一起搬：`mid = n` 之后 `mid` 的来源是 `n`，提权遍历中的中间绑定
                         * 才不会断链。
                         *
                         * 只在这个分支（引用类型的目标接收引用）记录；取过地址的槽跳过，因为别名
                         * 可能改它。`a = b  b = a` 这类环由 `promoteInto` 的跳数上限拦住。 */
                        if (slot0) noteOrigin(c, slot0, v);
                        if (slot0 && !slot0->addressed && v && v->kind != EX_NULL) slot0->poolSite = v;
                    }
                    /* 元素或字段上的改指（`a[0] = ref local`）也要记录：否则
                     * `var a: [2]?ref node` 零初始化后上界是 0，在深度 1 改指后仍是 0，
                     * `return a` 会被接受，而数组指向已死的局部量。取最大值是因为 `refDepth`
                     * 是内部引用落点的上界。 */
                    {
                        Sym *rootA = placeRoot(c, s->u.assign.target);
                        /* `(*a).next = b`：`placeRoot` 走不过解引用，槽要挂在 `a` 上，否则
                         * `return a` 的提权走不到 `b` 指向的对象（环留在块层 ⇒ 返回即悬垂）。 */
                        if (!rootA) rootA = destDerefSym(c, s->u.assign.target);
                        if (rootA && rootA->type && typeContainsRef(c->tt, rootA->type)) {
                            int dA = targetDepth(c, v);
                            /* 按字段记录。没取过地址时条目是覆盖而不是合并，所以 `h.p = null`
                             * 之后根的有效深度确实会降下来；一律取最大值会造成误拒。
                             * 键是**整条路径**（`bx.mid.next` 记成 `mid.next`，元素记 `[0]`）：
                             * 只取最后一段会让两层投影的来源对不上槽，`return bx.mid.next`
                             * 的那个 `new` 就没人提权。 */
                            const char *fn_ =
                                (s->u.assign.target->kind == EX_FIELD ||
                                 s->u.assign.target->kind == EX_INDEX)
                                  ? fieldSlotKey(c, s->u.assign.target) : NULL;
                            c->curStoreVal = v;
                            noteFieldDepthWrite(c, rootA, fn_, dA);
                            c->curStoreVal = NULL;
                        }
                    }
                    /* 改指也要跑借用值检查：在
                     *     struct slot { r: mut ref i32 }
                     *     fn stash(b: mut ref slot, p: mut ref i32) { b.r = ref *p }
                     * 里权限够、深度也够（`p` 是参数，深度 0），只剩“这个值真是调用方的”能拦。
                     * 漏掉这次调用就会产生悬垂引用。 */
                    checkStoreEscape(c, v, s->u.assign.target, s->line);
                    noteAcyclicStore(c, s, s->u.assign.target, v);
                    return;
                }

                /* 引用没有隐式写穿：`p = v` 只改指，写穿写作 `*p = v`。两种形式各只有一个
                 * 含义。 */
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

            /* 先把目标深度喂给值里的 `new` 站点。顺序要紧：下面两块把 `targetDepth(value)` 记进
             * 目标的深度表，提权晚一步就会记下旧数字、之后误拒。 */
            int atDst = storeLayer(c, s->u.assign.target);
            /* 这个绑定现在持有这个值，来源跟着值走：`originOf` 把绑定链压平到根的表达式，
             * `h = g` 于是让 `h` 的来源是 `g` 的根（把分配放进 `g` 的结构体字面量），提权遍历
             * 必须走到那里才能找到它。不更新的话，先声明后填充的绑定会一直不可达：遍历停在空
             * 声明上，分配留在块 arena 里，在持有它的结构体被返回前就释放了。
             *
             * 只有直接赋值给绑定本身才更新；`*q = v` 与 `s.f = v` 写的是绑定已指的东西，取过
             * 地址的绑定跳过（别名可能改动它）。 */
            {
                Sym *dst = (s->u.assign.target->kind == EX_IDENT)
                           ? identBindOf(s->u.assign.target) : NULL;
                if (dst && !dst->addressed) {
                    noteOrigin(c, dst, s->u.assign.value);
                    /* 再记值本身，不压平到链根：遍历读的是它，用来到达绑定现在持有的存储。把根
                     * 的表达式记到链上每个绑定，会让 `out = h` 沿 `h = zero` 回到初始化 `zero`
                     * 的字面量，即另一个无关绑定的空盒子。 */
                    dst->heldSrc = s->u.assign.value;
                }
            }
            /* 一次写：发布、提权、逃逸集合、字段表，四件事都走 `noteStore`。 */
            noteStore(c, s->u.assign.value, s->u.assign.target, atDst, s->line);
            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* 存进字段或元素的值不能比目标更深。 */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            noteAcyclicStore(c, s, s->u.assign.target, s->u.assign.value);
            return;
        }

        case ST_IF: {
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);

            /* 收窄 `?ref T`：`if p != null`、`if p == null ... else` 以及守卫
             * `if p == null { return }` 都告诉编译器该分支上 `p` 非空，分支内不必生成运行时
             * 检查。 */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.ifs.cond, &whenTrue);
            const size_t mark = c->narrow.len;

            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* 证明随 then 分支结束 */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {
                /* 守卫形式：条件离开函数或循环，所以能走到它后面的语句就意味着 `p` 非空。证明
                 * 留在当前作用域，直到本块结束。 */
                pushNarrow(c, tg);
            }
            return;
        }

        case ST_WHILE: {
            /* `while` 的条件不许提升：提升出去的前缀在循环前求值一次，而不是每轮一次。 */
            c->noHoist++;
            /* 条件是自己的 **place**：压一层作用域让它里面的分配站点深一层，并记下条件是否
             * 新增了分配站点，代码生成据此决定要不要给它自己的 arena 并每轮释放。否则
             * `while (new i64[N])[0] == 0` 会在外层块的级别分配，随轮数增长。 */
            pushScope(c);
            const int sitesBefore = c->allocSites;
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            popScope(c);
            planSetCondAllocs(s, (c->allocSites != sitesBefore));
            c->noHoist--;

            /* 循环条件同样收窄：体内该绑定已知非空。给它赋值会注销证明，下一轮重新比较，就像
             * `while (p) { p = p->next; }`。 */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.whiles.cond, &whenTrue);
            const size_t mark = c->narrow.len;
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            return;
        }

        case ST_EXPR:
            /* `par` 降糖时合成的那条 worker 调用已经检查过（实参与 plan 标记都由
             * `parLower` 填好）；再走普通调用路径会按 worker 的形参做 arity 检查。 */
            if (s->u.expr.expr && planParWorker(s->u.expr.expr)) return;
            /* 单独成句的 `f()?`：意思是“这一步必须成功，否则整条链失败”。 */
            if (s->u.expr.expr && s->u.expr.expr->kind == EX_TRY) {
                checkMaybeTry(c, s->u.expr.expr);
                return;
            }
            /* 被丢弃的表达式**不是值位置**：不从里面读走任何东西，因此引用结果在这里可以，
             * 而且必须可以，`fin >> x >> line;` 产生的就是它。真正需要值的地方仍按 `checkValue`
             * 的规则。 */
            checkExpr(c, s->u.expr.expr);
            return;

        case ST_YIELD: {
            /* 带绑定的 `yield`：名字成为所让出类型的协程帧局部量，恢复时从帧的 `in` 槽填入。
             * 在本 case 其余部分之前登记，帧布局那一趟才看得到。 */
            if (s->u.yield_.bind && c->curFunc && planIsCoro(c->curFunc) && planYieldType(c->curFunc)) {
                Type *bt = planYieldType(c->curFunc);
                if (s->u.yield_.bindAnn)
                    /* 与 `var` 注解同样处理：解析器只交来书写的类型，由 `ttResolve` 变成可用
                     * 的。 */
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
                /* 帧的 Vec 还没建（由函数体之后的 `coroFrameLay` 建），所以这里只**记录**绑定，
                 * 由那一趟消费。现在就要声明成局部量，体内后面的语句才能点名它；帧字段本身由
                 * `coroFrameLay` 按记录的绑定建。
                 *
                 * `got = yield e`：`got` 是程序自己的变量、已经声明过，必须复用那个符号（再声明
                 * 一次会得到 `__2` 后缀，帧字段对不上）；`var got = yield e` 则还没有这个名字。 */
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
            /* `yield e` 只在协程体内合法，且 `e` 必须是该协程的 `T`；两者都由声明的返回类型
             * `coroutine<T>` 决定。 */
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
            /* 规则 3：挂起不许落在自行回收存储的块里，那块 arena 在本协程恢复之前就释放了。 */
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
            /* 消息是程序停下时读者唯一看到的东西，所以必须是文本：字面量、`slice<u8>` 或 C
             * 字符串（`ref u8`）；其余都值得编译错误。
             *
             * 判据按**形状**（`ttIs` 只认内建类型，`ttIs(t, "slice<u8>")` 恒假）：`u8` 的视图或
             * `u8` 的引用。`mut` 是权限，不改变能打印什么。 */
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
                /* 协程以跑完或裸 `return` 结束，两者都表示“没有更多值”，即状态机的完成态（代码
                 * 生成存哨兵值）。`return e` 在协程里仍是错误：交出值要靠 `yield`。 */
                if (c->curFunc && planIsCoro(c->curFunc)) return;
                /* 到检查函数体时 `void` 有两种写法：内建的规范 `void`（自由函数，
                 * `resolveSignature` 也会把它置空），以及方法 `-> void` 那种尚未解析的**具名**
                 * 类型。两者都接受：裸 `return` 表示“没有更多值”，正是 void 函数承诺的东西。 */
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
                /* lambda 可以省掉结果类型：第一个 `return <值>` 决定它。在这里填上，体内其余部分
                 * 与之后的每个 return 才能对着真实类型检查。 */
                if (c->curFunc && anLamInferRet(c->curFunc) && got && !ttIsError(got)) {
                    c->curFunc->ret = got;
                    anSetLamInferRet(c->curFunc, false);
                    return;
                }
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            /* 裸构造器如 `return failure(e)`：类型来自上面的 `want`。 */
            desugarBareCtor(c, s->u.ret.value, want);

            /* `return e?`：表达式的值是载荷，而函数交回外层类型，所以比较的是载荷能否放进外层
             * 类型的载荷；代码生成再包回去。不能用下面的 `adoptContextType` 加 `checkAssignable`
             * 那条路，那会让 `i64` 去当 `option<i64>`。 */
            if (s->u.ret.value->kind == EX_TRY) {
                Type *vt = checkTryInner(c, s->u.ret.value);
                Type *wb = ttBase(want);
                /* 这个分支跳过了下面的 `checkEscape`，所以 `promoteInto` 不会跑，`e?` 交出的
                 * 分配就不会被记为逃出帧，函数体之后那一趟会把站点放回块级别。载荷的引用最终归
                 * 调用方，所以提到 0 层。 */
                noteReturnPublish(c, s->u.ret.value, s->line);
                promoteInto(c, s->u.ret.value, 0);
                /* 再记一条事实：此刻 `makesPool` 闭包还没算出来，标志为假，池站点的提权落不
                 * 下去；闭包之后的重放会拿这条事实再提一次。 */
                recordLvlFact(c, s->u.ret.value, 0);
                /* `option<T>` 与 `result<T, E>` 是**枚举实例**（带类型实参的 `TY_ENUM`），
                 * `TY_GENERIC` 对它们恒假，所以下面要一并接受 `TY_ENUM`，否则根本不比较载荷：
                 * 从 `option<i64>` 到 `option<u8>` 的 `return o?` 会把 1000 悄悄变成 232，而直接
                 * 写 `option<u8>::some(1000)` 会被拒。对枚举实例的载荷比较，`targs[0]` 是两个
                 * 家族的成功类型。 */
                if (wb && wb->targs.len >= 1 && (wb->kind == TY_GENERIC || wb->kind == TY_ENUM))
                    checkAssignable(c, *(Type **)vecAt(&wb->targs, 0), vt,
                                    s->u.ret.value, "return value");
                return;
            }

            adoptContextType(s->u.ret.value, want);
            Type *vt = checkInto(c, want, s->u.ret.value);   /* 要的是引用，不解引用 */
            warnSharedReturn(c, s->u.ret.value, vt);
            markCallHomeIfEscaping(c, s->u.ret.value, 0);    /* 要交出去，用 home arena */
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            /* 返回的引用必须指向参数或静态数据，即深度 0。 */
            if (dbgOn("EXTC_DBG_RET3"))
                fprintf(stderr, "[ret3] %-8s line=%d kind=%d d=%d mentionsParam=%d\n",
                        c->curFunc?c->curFunc->name:"?", s->line,
                        (int)s->u.ret.value->kind, targetDepth(c, s->u.ret.value),
                        mentionsParam(s->u.ret.value->type)?1:0);
            /* 返回的值交给调用方，所以在 0 层发布：返回值就该活过本帧。 */
            noteReturnPublish(c, s->u.ret.value, s->line);
            checkEscape(c, s->u.ret.value, 0, s->line, "this return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_MATCH: {
            /* `match e { variant => ... }` 的价值在穷尽性检查：用 `if`/`else` 写时 else 分支要
             * 手工兜底，日后新增的变体会悄悄掉进去；`match` 会报出缺失的臂。 */
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

            /* `_` 臂：匹配上面所有臂没覆盖的变体。它必须最后（解析器已挡），且没有载荷可绑
             * —— 每个可能落进来的变体载荷类型都不同，一个名字没有一种类型可给。它让
             * `if let` / `while let` 的脱糖变成普通 match 的第二条臂，也让手写 match 可以
             * 选择「剩下的都不重要」。 */
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

            /* 每臂：名字必须是本枚举的变体，且同一变体不许匹配两次。 */
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

            /* 穷尽性：`_` 臂兜住剩下的；没有它就必须列全每个变体。 */
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

            /* 每臂的体各自开作用域，臂的载荷绑定（`circle(r) => ...`）活在其中：第 i 个名字取
             * 第 i 个载荷字段。 */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);

                /* 兜底臂没有载荷绑定，体照常在自己的作用域里检查。 */
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
                    /* 载荷绑定是值的拷贝（值语义），名字只读；要改就拷进 `var`。 */
                    Sym *bs = declare(c, bn, bt, false, false, arm->line, c->scopes.len);
                    /* `declare` 给的是保守答案（所指物活在我这个槽的深度），知道初始化式的一方
                     * 要把它收窄。这里的初始化式是 **scrutinee**：载荷是拷贝，它的引用指向哪里就是
                     * scrutinee 的引用指向哪里（scrutinee 自身的绑定已带这个答案，所以
                     * `targetDepth` 查绑定而不是表达式节点）。不收窄的话
                     * `match dl::sym(h, n) { some(p) => { return p } ... }` 这种普通拷贝会被当成
                     * 借用深度 2 的局部量而拒绝。 */
                    if (bt && typeContainsRef(c->tt, bt))
                        anSetRefDepth(bs, targetDepth(c, s->u.match.scrutinee));
                    /* 把解析出的 C 名写回臂，与 `Param.cname`、声明的 `cname` 同样处理。否则同一
                     * 作用域里两个 `match` 绑同名时，声明用的是原名而体里查 `s__2`，生成的 C 编
                     * 不过。 */
                    *(const char **)vecAt(&arm->binds, k) = bs->cname;
                }
                for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                    checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                popScope(c);
            }
            return;
        }

        case ST_DOMAIN: {
            /* `d.run { … }` 的尾块是 **domain 块**：“这里的并发在哪运行，并且必须在本块结束前
             * 全部完成”。谁能带尾块由**类型**决定（语言级对象 `domain`），不看名字也不看注解：
             * 注解无法验证，按名字特判改个名就静默失效。体在 domain **内部**检查，`ext` 因此
             * 在那里合法，别处仍是编译错误。 */
            Expr *cal = s->u.domain_.callee;
            Type *rt = NULL;
            if (cal && cal->kind == EX_FIELD && cal->u.field.obj) {
                Expr *recv = cal->u.field.obj;
                checkExpr(c, recv);
                rt = recv->type;
                while (rt && rt->kind == TY_REF) rt = rt->inner;   /* `d: ref domain` */
            }
            /* domain 自己的方法是 `run`。接收者的**类型**才是 domain 的判据，方法名是对象协议的
             * 一部分，所以 `d.rnu { … }` 这样的拼写错误是编译错误，而不是被默默接受。 */
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
            /* 作用域规则：本块里的 `ext` 去**这个** domain。保存/恢复就是整叠——嵌套 domain 遮住
             * 外层，出来时还回去。 */
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
