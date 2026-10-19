/* 表达式检查：每个表达式的类型，以及只有表达式的形状才能决定的那部分检查。
 *
 * 这里是检查器的递归核心：把名字解析成绑定、原地改写少数节点（变体访问变成枚举值、泛型调用变成
 * 普通调用），并决定每个分配站点的 arena 层级。借用与生命周期规则本身住在检查器的逃逸分析一侧。
 */

#include <stdlib.h>
#include "check_internal.h"
#include "plan.h"          /* 计划/分析产物：写入经 setter，读取经访问器 */
/* stdlib 目录，由加载器经 `hintIndexSetStdDir` 交给检查器；NULL 表示 INDEX 提示不可用。 */
static const char *stdDirSeen = NULL;

/**
 * @brief 把 stdlib 目录交给检查器，用于从 `INDEX` 读"没有这个方法"的提示。
 *
 * @param stdDir  stdlib 目录；提示在首次未命中、只走错误路径时读它。
 */
void hintIndexSetStdDir(const char *stdDir) { stdDirSeen = stdDir; }

/**
 * @brief 查 `stdlib/INDEX`，返回声明 `impl` 方法 `method` 的模块名。
 *
 * `impl` 块只有在模块被**语义导入**后才可见，所以"string 上没有 `parseJSON`"几乎总是"没有导入
 * 声明它的模块" —— 这是诊断最值得补的一句话。为这句话去加载候选模块会破坏导入的隔离性，于是
 * 答案由索引生成器预计算、随仓库提交为 `stdlib/INDEX`（`INDEX`'s columns are fixed by its
 * generator），这里**只在首次未命中、只走错误路径**时读一次。
 *
 * @param method  方法名（用户书写形式，未改写）；NULL 直接返回。
 * @return        声明该方法的模块名；INDEX 不可用或没有该方法时返回 NULL。
 */
const char *modulesMethodHint(const char *method) {
    static char names[256][96];
    static char mods[256][96];
    static int  n = -1;
    if (n < 0) {
        n = 0;
        if (stdDirSeen && method) {
            char path[1024];
            snprintf(path, sizeof path, "%s/INDEX", stdDirSeen);
            FILE *f = fopen(path, "rb");
            if (f) {
                char line[256];
                while (n < 256 && fgets(line, sizeof line, f)) {
                    if (line[0] == '#') continue;
                    char *tab1 = strchr(line, '\t'); if (!tab1) continue; *tab1++ = '\0';
                    char *tab2 = strchr(tab1, '\t'); if (!tab2) continue; *tab2++ = '\0';
                    /* Columns past the module name (visibility, kind) belong to the manual's
                     * coverage tooling, not to this diagnostic. */
                    char *tab3 = strchr(tab2, '\t'); if (tab3) *tab3 = '\0';
                    char *nl = strchr(tab2, '\n');   if (nl) *nl = '\0';
                    snprintf(names[n], sizeof names[n], "%.95s", line);
                    snprintf(mods[n],  sizeof mods[n],  "%.95s", tab2);
                    n++;
                }
                fclose(f);
            }
        }
    }
    if (!method) return NULL;
    for (int i = 0; i < n; i++) if (strcmp(names[i], method) == 0) return mods[i];
    return NULL;
}



/**
 * @brief 取一个**内建整数**类型按名字对应的闭区间 `[lo, hi]`。
 *
 * 两个整数字面量的折叠由 C 编译器完成（gcc's constant folding），而 C 的常量折叠正是 gcc 在
 * `-Werror` 下拒绝的地方（`int32_t n = (2000000000 + 2000000000);`），所以范围要在这里先算清楚。
 *
 * @param t   类型；NULL、非 `TY_BUILTIN` 或不是已知内建整数时返回 false。
 * @param lo  接收下界；返回 false 时不动。
 * @param hi  接收上界，用 `unsigned long long` 以装下 u64 的最大值；返回 false 时不动。
 * @return    认得该内建整数类型则返回 true。
 */
static bool intLitRange(Type *t, long long *lo, unsigned long long *hi) {
    if (!t || t->kind != TY_BUILTIN || !t->name) return false;
    if (strcmp(t->name, "i8")  == 0) { *lo = -128LL;   *hi = 127ULL;   return true; }
    if (strcmp(t->name, "i16") == 0) { *lo = -32768LL; *hi = 32767ULL; return true; }
    if (strcmp(t->name, "i32") == 0) { *lo = -2147483648LL; *hi = 2147483647ULL; return true; }
    if (strcmp(t->name, "i64") == 0) { *lo = (-9223372036854775807LL - 1); *hi = 9223372036854775807ULL; return true; }
    if (strcmp(t->name, "u8")  == 0) { *lo = 0; *hi = 255ULL;   return true; }
    if (strcmp(t->name, "u16") == 0) { *lo = 0; *hi = 65535ULL; return true; }
    if (strcmp(t->name, "u32") == 0) { *lo = 0; *hi = 4294967295ULL; return true; }
    if (strcmp(t->name, "u64") == 0) { *lo = 0; *hi = 18446744073709551615ULL; return true; }
    return false;
}

/* ---------------------------------------------------------------- expressions */


/* ---------------------------------------------------------------- parallel::run
 *
 * `parallel::run(worker, out, n, threads)` 是 stdlib/std/parallel.extc 里用 `@builtin` 声明的内建：
 * 语言没有一等函数值，所以"把 worker 放到线程上跑"必须由编译器生成一个 trampoline 去调它。
 *
 * 语义（与库文件头一致）：区间 [0,n) 分块，worker 拿到自己的 [lo,hi)；**`out` 按 [lo,hi) 切成
 * 互不重叠的一段**交给每个 worker ⇒ 读只共享、写必分区 ⇒ 不需要新的可发送性分析。
 *
 * worker 体内的语法白名单（保守但可靠）：只许算符、字面量、下标、它自己的参数与局部；任何调用
 * （含用户函数与 stdlib）、`new`、以及既非参数又非局部的名字（模块级 `var` 就在这一档）一律拒绝。
 * 传递性（worker 调用户辅助函数）与线程本地 arena 的路由由 `parBodyProblem` 一并判定。 */

/**
 * @brief 判断一个函数声明是不是 `parallel::run` 内建。
 *
 * 认的是改写后的内建名加形状，不看模块路径，所以用户自己的同名函数不受影响。
 *
 * @param f  函数定义；NULL 返回 false。
 * @return   改写成 `parallel$run` 的内建声明则返回 true。
 */
static bool isParRunDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "parallel$run") == 0;
}
/**
 * @brief 判断一个函数声明是不是 `sys::domain::single` 内建。
 *
 * 与 `parallel::run` 一样按改写后的内建名加形状识别，不看模块路径（the module's path is never
 * consulted），所以用户自己写的 `single` 函数（没有 `@builtin` 标记）不受影响。
 *
 * @param f  函数定义；NULL 返回 false。
 * @return   返回类型为 `domain` 结构体的 `domain$single` 内建声明则返回 true。
 */
static bool isDomSingleDecl(FuncDef *f) {
    return f && f->isBuiltin && f->name && strcmp(f->name, "domain$single") == 0 &&
           f->ret && f->ret->kind == TY_STRUCT && f->ret->name &&
           strcmp(f->ret->name, "domain") == 0;
}
/**
 * @brief 判断一个函数声明是不是 `extc_viewOf(p, n)` 这个内建：一个指针加长度的 view。
 *
 * 与另外两个内建一样按改写后的 `@builtin` 名加形状识别，绝不按模块路径。为什么非得有编译器：
 * view 在 extC 里**构造**不出来 —— 它的类型是泛型实例，没有结构体字面量能拼出来（`slice<u8> { ... }`
 * 在表达式位置会被读成类型而拒绝），指针又只能来自 `ref void`，那也在拒绝之列。构造它的三行 C
 * 由代码生成按需发出。
 *
 * @param f  函数定义；NULL 返回 false。
 * @return   是这一内建、恰有两个参数且返回类型为 view 时返回 true。
 */
static bool isViewOfDecl(FuncDef *f) {
    return f && f->isBuiltin && plateIsViewOfName(f->name) &&
           f->params.len == 2 && f->ret && ttIsViewType(f->ret);
}
/**
 * @brief 判断一个类型是不是内建 `i64`（by the builtin's name）。
 *
 * @param t  类型；NULL 返回 false。
 * @return   是内建 `i64` 则返回 true。
 */
static bool isI64Type(Type *t) {
    return t && t->kind == TY_BUILTIN && t->name && strcmp(t->name, "i64") == 0;
}
typedef struct { Vec names; const char *why; int depth; Checker *c; } ParQ;
static bool parQHas(ParQ *q, const char *n);
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth);
static bool parLocalStmt(void *ctx, Stmt *s);
static bool parBodyExpr(void *ctx, Expr *e);
static bool parBodyStmt(void *ctx, Stmt *s);

/**
 * @brief 收集 worker 函数体里声明的局部名（`let`/`var`）。
 *
 * @param ctx  拥有名字表的 `ParQ`。
 * @param s    要看的语句节点。
 * @return     继续遍历子节点返回 true；收集本身从不拒绝。
 */
static bool parLocalStmt(void *ctx, Stmt *s) {
    ParQ *q = (ParQ *)ctx;
    if (s->kind == ST_VAR && s->u.var.name) *(const char **)vecPush(&q->names) = s->u.var.name;
    AstVisit v = { NULL, parLocalStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
/**
 * @brief 递归遍历语句的子节点，把其中的表达式交给 `parBodyExpr`。
 *
 * @param ctx  拥有名字表的 `ParQ`。
 * @param s    要遍历的语句节点。
 * @return     某个子节点被拒绝时返回 false，遍历停止。
 */
static bool parBodyStmt(void *ctx, Stmt *s) {
    AstVisit v = { parBodyExpr, parBodyStmt, ctx };
    return astWalkStmtChildren(s, &v);
}
/**
 * @brief 检查 worker 体内一个表达式：只认自己的参数与局部，以及可接受的调用。
 *
 * 拒绝时把第一条理由（英文，直接进诊断）写进 `q->why`。
 *
 * @param ctx  拥有名字表与理由的 `ParQ`。
 * @param e    要检查的表达式节点。
 * @return     这个表达式可以出现在 worker 里返回 true；否则返回 false。
 */
static bool parBodyExpr(void *ctx, Expr *e) {
    ParQ *q = (ParQ *)ctx;
    if (e->kind == EX_IDENT && !parQHas(q, e->u.ident.name)) {
        if (!q->why) q->why = "it names something that is neither a parameter nor a local of the worker";
        return false;
    }
    /* `new` 放行：worker 链上每个函数都带 parTlsArena ⇒ arenaRefAt 给 `(*extc_tls_arena)`；worker
     * 体内分配会让它多一个隐藏的 home 尾参，trampoline 补传的是同一个线程本地 arena。安全性来自
     * 签名：只能写 out、只能返回 i64 ⇒ 分配逃不出去。 */
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        FuncDef *g = planCallee(e);
        if (!g) {
            Expr *ce = e->u.call.callee;
            if (ce && ce->kind == EX_IDENT) g = findFunc(q->c, ce->u.ident.name);
        }
        if (!g) { if (!q->why) q->why = "it makes a call this check cannot resolve"; return false; }
        if (g->isExtern) {
            if (g->extThreadMask != 0) {
                if (!q->why) q->why = "it calls into shared runtime state (that declaration is not signed `Thread=0`)";
                return false;
            }
        } else {
            const char *sub = parBodyProblem(q->c, g, q->depth + 1);
            if (sub) { if (!q->why) q->why = sub; return false; }
        }
    }
    AstVisit v = { parBodyExpr, parBodyStmt, ctx };
    return astWalkExprChildren(e, &v);
}

#define PAR_DEPTH_LIMIT 16      /* 递归深度上限：宁可拒绝，也不让检查自己爆栈 */
/**
 * @brief 判断名字是否已经在收集到的名字表里。
 *
 * @param q  名字表。
 * @param n  要查的名字。
 * @return   在表里返回 true。
 */
static bool parQHas(ParQ *q, const char *n) {
    for (size_t i = 0; i < q->names.len; i++)
        if (strcmp(*(const char **)vecAt(&q->names, i), n) == 0) return true;
    return false;
}
/**
 * @brief 判断一个函数能不能出现在 worker 里（含**传递**调用链）。
 *
 * 规则：函数体只碰自己的参数与局部（模块级 `var` 既非参数也非局部，自然落在拒绝档，不用另做全局
 * 分析）；调用只能是签过 `Thread=0` 的 extern，或另一个同样通过这一关的顶层函数。递归带深度上限，
 * 超限按"不安全"处理（保守方向）。通过时给链上每个函数打上线程本地 arena 标记。
 *
 * @param c      检查器，用于分配名字表。
 * @param f      被检查的函数；没有函数体（extern/内建）时直接通过。
 * @param depth  当前递归深度，从 0 起。
 * @return       通过返回 NULL；否则返回一句拒绝理由（英文，直接拼进诊断）。
 */
static const char *parBodyProblem(Checker *c, FuncDef *f, int depth) {
    ParQ q;
    if (depth > PAR_DEPTH_LIMIT) return "the call chain is too deep to check (conservative refusal)";
    if (!f->body) return NULL;                       /* extern/内建：由调用点的规则管 */
    vecInit(&q.names, c->arena, sizeof(const char *));
    q.why = NULL; q.depth = depth; q.c = c;
    {
        AstVisit v = { NULL, parLocalStmt, &q };
        astWalkStmtChildren(f->body, &v);
    }
    for (size_t i = 0; i < f->params.len; i++)
        *(const char **)vecPush(&q.names) = (*(Param **)vecAt(&f->params, i))->name;
    {
        AstVisit v = { parBodyExpr, parBodyStmt, &q };
        if (!astWalkStmtChildren(f->body, &v))
            return q.why ? q.why : "an unsupported construct";
    }
    planSetParTlsArena(f, true);        /* 这条链上的每个人：`new` 都走线程本地 arena */
    return NULL;
}

static bool exprHasCall(Checker *c, Expr *e);   /* defined at the end of this file */
static bool exprHasAnyCall(Expr *e);            /* the syntactic question; see its comment */

/**
 * @brief 记下"对类型参数使用的运算符要按实例重新检查"。
 *
 * 模板里的 `T` 是不透明的，`T` 支不支持某个运算符只有类型实参确定后才有答案；这个记录让消费者
 * （`runOpCheck`）在实例上重问具体类型路径用的同一个谓词。记录只此一处：写三份就会漂移，而漂移
 * 的表现是一个运算符接受另一个运算符拒绝的东西。
 *
 * @param c   检查器。
 * @param e   携带运算符的 `EX_BIN` 节点。
 * @param op  运算符，决定实例必须提供什么。
 *
 * 函数体之外没有可延迟的对象（`c->curFunc` 为空时直接返回）：全局初始化器必须是常量，那里的比较
 * 按自己的口径报错。
 */
static void deferOp(Checker *c, Expr *e, const char *op) {
    planSetNeedOp(e, true);
    if (!c->curFunc) return;
    OpCheck *oc = (OpCheck *)arenaAllocZero(c->arena, sizeof(OpCheck));
    oc->node  = e;
    oc->op    = op;
    oc->func  = c->curFunc;
    oc->owner = c->curFunc->owner;   /* NULL for a free function */
    *(OpCheck **)vecPush(&c->opChecks) = oc;
}

/**
 * @brief 为可重载运算符补上和 `EX_METHOD` 完全相同的两个隐藏实参。
 *
 * 运算符**就是**方法调用：左操作数是接收者、右操作数是唯一实参，被调用者可以分配（`usesHome`）
 * 或建池（`makesPool`），所以调用点欠下同样的隐藏实参，走的是方法路径那两个同样的函数。
 *
 * ZONE 那一半不能省：SSO 字符串上的 `push`/`append` 会在离开内联缓冲时建池，于是读字符串的
 * 运算符（`io >> string`、`ifstream >> string`）也是建池被调用者；少了这一步，声明里带着
 * `int64_t __extc_home_zone` 而调用点只传两个实参 —— 生成的 C 根本编不过。
 *
 * 层级由 `setCallZoneArg` 先定，`callHomeDepth` 再拿到两个操作数记录逃逸点；记录是无条件的
 * （`makesPool` 是闭包，此处仍为 false），最后一趟按标志过滤并把层级下调。
 *
 * @param c  检查器。
 * @param e  携带运算符的 `EX_BIN` 节点。
 * @param m  检查器为这个节点解析出的运算符方法；NULL 时只做 zone 那一半。
 */
static void setOpCallArgs(Checker *c, Expr *e, FuncDef *m) {
    setCallZoneArg(c, e);
    if (!m || !c->eSites.arena) return;
    /* 只有 `mut ref` 操作数才可能被写入，才存在要记录的逃逸点：`==` / `<` 两个操作数都按 `ref`
     * 传，什么都不记。 */
    bool anyMut = false;
    for (size_t i = 0; i < m->params.len && i < 2; i++) {
        Param *p = *(Param **)vecAt(&m->params, i);
        if (p->type && p->type->kind == TY_REF && p->type->mut) anyMut = true;
    }
    if (!anyMut) return;
    /* 运算符恰好两个参数、两个操作数且顺序一致（`checkOperatorSig` 保证），所以可以直接交给方法
     * 路径自己的记录器，不必再抄一份深度/逃逸规则。 */
    Vec args;
    vecInit(&args, c->arena, sizeof(Expr *));
    *(Expr **)vecPush(&args) = e->u.bin.left;
    *(Expr **)vecPush(&args) = e->u.bin.right;
    (void)callHomeDepth(c, &args, &m->params, e);
}

/**
 * @brief 算一个二元算术运算符的结果类型。
 *
 * 结果规则只住在这里：位运算符也用这一份，抄第二份就会漂移。
 *
 * @param c   检查器。
 * @param e   被检查的 `EX_BIN` 节点，取它的运算符与源码位置。
 * @param lt  左操作数类型。
 * @param rt  右操作数类型。
 * @return    两个操作数的公共类型；报过诊断后返回错误类型。
 *
 * 字面量操作数向另一侧适配，所以 `u32 + 1` 的类型是 `u32`；`%` 在这里限定为整数。引用类型的
 * 操作数不会进来，调用方先报它们。
 */
static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;

    /* 用户自定义运算符先于数值规则尝试：结构体操作数因此走到自己的方法，而不是"算术运算符只接受
     * 数值类型"。协议同 `==`：方法名就是运算符，签名在定义处已由 checkOperatorSig 校验。
     * 运算符按右操作数类型精确匹配，所以 `vec2 * f64` 可以在 `vec2` 上单独定义为一个方法
     * （the operand's exact type picks it）。 */
    if (isArithOp(op)) {
        StructDef *sd = structOf(ttBase(lt));
        if (sd) {
            FuncDef *m = findOp(c->tt, ttBase(lt), op, rt, NULL);
            if (!m) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note,
                          "define it inside `%s`:\n"
                          "      fn %s(self: ref %s, other: %s) -> %s { ... }",
                          DN(sd), op, DN(sd), DN(sd), DN(sd));
                ckError(c, e->line, bufCstr(&note), "`%s` does not define `%s`", DN(sd), op);
                return err;
            }
            planSetCallee(e, m);
            planSetUsed(m, true);
            setOpCallArgs(c, e, m);
            return lt;
        }
        /* 两个操作数里**任一**是类型参数都要延迟，不只是左边：`value()` 返回模板的 `T`，会从
         * **右边**到来。实例决定操作数真正是什么；结果类型仍然钉在参数上（the instance's answer
         * decides）。 */
        if (lt->kind == TY_PARAM || rt->kind == TY_PARAM) {
            deferOp(c, e, op);
            return lt->kind == TY_PARAM ? lt : rt;
        }
    }

    if (!ttIsNumeric(lt) || !ttIsNumeric(rt)) {
        ckError(c, e->line, "arithmetic operators only accept numeric types",
                "cannot apply `%s` to `%s` and `%s`", op, typeStr(c, lt), typeStr(c, rt));
        return err;
    }
    if (strcmp(op, "%") == 0 && (!ttIsInteger(lt) || !ttIsInteger(rt))) {
        ckError(c, e->line, "`%` is only meaningful for integers",
                "cannot apply `%%` to `%s` and `%s`", typeStr(c, lt), typeStr(c, rt));
        return err;
    }

    /* 两个整数字面量加一个算术运算符：折叠发生在 C 里，而 C 的常量折叠正是 gcc 拒绝它的地方
     * —— `int32_t n = (2000000000 + 2000000000);` 是 "integer overflow in expression"，在
     * `-Werror` 下直接报错（`-fwrapv` 只定义运行期行为）。所以在这里**当场报出来**，语言才能
     * 告诉用户该写什么（`i64(...)`）；放任回绕会把边界条件扩散到整个语言。 */
    if ((strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0) &&
        e->u.bin.left->kind == EX_INT && e->u.bin.right->kind == EX_INT &&
        ttEquals(lt, rt) && ttIsInteger(lt)) {
        long long lo; unsigned long long hi;
        if (intLitRange(lt, &lo, &hi)) {
            long long a = e->u.bin.left->u.ival;
            long long b = e->u.bin.right->u.ival;
            bool over = false;
            long long r = 0;
            /* 先判溢出再算：`a + b` 自己就可能溢出 `long long`，那是未定义行为。结果落在
             * `long long` 之外时，本来就落在这些类型中任何一个之外。 */
            if (op[0] == '+') {
                if (b > 0 && a > 9223372036854775807LL - b) over = true;
                else if (b < 0 && a < (-9223372036854775807LL - 1) - b) over = true;
                else r = a + b;
            } else if (op[0] == '-') {
                if (b < 0 && a > 9223372036854775807LL + b) over = true;
                else if (b > 0 && a < (-9223372036854775807LL - 1) + b) over = true;
                else r = a - b;
            } else {   /* `*` */
                if (a == 0 || b == 0) r = 0;
                else if (a > 0 && b > 0 && a > 9223372036854775807LL / b) over = true;
                else if (a > 0 && b < 0 && b < (-9223372036854775807LL - 1) / a) over = true;
                else if (a < 0 && b > 0 && a < (-9223372036854775807LL - 1) / b) over = true;
                else if (a < 0 && b < 0 && a < 9223372036854775807LL / b) over = true;
                else r = a * b;
            }
            /* 上界是无符号的（u64 装不进 `long long`），所以只对**非负**结果做转换：把 `-1`
             * 转成无符号会变成巨大的值，让每个负常量都被当成溢出。 */
            if (!over && (r < lo ||
                          (r >= 0 && hi <= 9223372036854775807ULL && (unsigned long long)r > hi)))
                over = true;
            if (over) {
                ckError(c, e->line,
                        "a constant that does not fit its type is reported here, not left to the C"
                        " compiler: it folds the expression and rejects the overflow",
                        "the constant `%lld %s %lld` overflows `%s`; write the type you mean,"
                        " e.g. `i64(...)`",
                        a, op, b, typeStr(c, lt));
                return err;
            }
        }
    }

    /* 字面量按值适配另一侧：`u32 + 1` 里的 `1` 是 u32。 */
    if (isNumericLit(e->u.bin.left) && ttIsNumeric(rt)) {
        if (!literalFits(e->u.bin.left, rt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, rt));
            return err;
        }
        return rt;
    }
    if (isNumericLit(e->u.bin.right) && ttIsNumeric(lt)) {
        if (!literalFits(e->u.bin.right, lt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, lt));
            return err;
        }
        return lt;
    }

    if (ttEquals(lt, rt)) return lt;
    if (ttCanWiden(lt, rt)) return rt;
    if (ttCanWiden(rt, lt)) return lt;

    ckError(c, e->line, "these two types have no lossless conversion; convert explicitly first",
            "`%s` and `%s` have no common type for `%s`",
            typeStr(c, lt), typeStr(c, rt), op);
    return err;
}
/**
 * @brief 解析复合赋值的运算符，返回 `target op value` 的类型。
 *
 * 语句保持书写原样，运算交给 `x + y` 会走的同一个算术检查，因此算术规则、字面量适配、
 * 引用拒绝与用户自定义运算符都只有一处答案。用户自定义运算符把接收者当引用，生成的
 * 调用要把目标再命名一次，所以目标里含调用时报错并提示拆成两行。
 *
 * @param c       检查器。
 * @param op      书写形式的复合运算符；首字符就是对应的二元运算符（五个都是单字符）。
 * @param target  已经检查过的左侧目标。
 * @param value   已经检查过的右侧值。
 * @param tgt     目标的类型。
 * @param val     值的类型。
 * @param out     接收解析出的 `EX_BIN` 节点；NULL 表示调用方不需要。
 * @return        `target op value` 的类型；运算符不适用或目标含调用时返回错误类型。
 */
Type *checkCompoundOp(Checker *c, const char *op, Expr *target, Expr *value,
                      Type *tgt, Type *val, Expr **out) {
    Expr *bin = exprNew(c->arena, EX_BIN, target->line);
    bin->u.bin.op    = arenaStrndup(c->arena, op, 1);   /* `+=` asks about the `+` operator's meaning */
    bin->u.bin.left  = target;
    bin->u.bin.right = value;
    Type *rt = checkArith(c, bin, tgt, val);
    if (out) *out = bin;
    if (ttIsError(rt)) return rt;
    if (planCallee(bin) && exprHasAnyCall(target)) {
        ckError(c, target->line,
                "the statement is rewritten as `x = add(&x, y)`, so the target has to be a"
                " place that can be named twice without side effects",
                "the target has a call in it -- split it into two lines: `var t = ...`"
                " then `t %s ...`", op);
        return ttError(c->tt);
    }
    return rt;
}


/**
 * @brief 判断 `op` 是不是位运算符 `&`、`|`、`^`、`<<`、`>>` 之一。
 *
 * 这些运算符的结果类型来自 `checkArith`，规则不在这里另抄一份，两边不会漂移。
 *
 * @param op  运算符拼写。
 * @return    是这五个之一则返回 true。
 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

/**
 * @brief 判断一个类型是不是引用（可空与否都算）。
 *
 * @param t  类型；NULL 返回 false。
 * @return   是 `TY_REF` 则返回 true。
 */
static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/**
 * @brief 报告"算术或比较运算符用在了引用上"，返回错误类型。
 *
 * 在 C 里这会悄悄变成指针算术，不是用户写的东西；报错好过给出一个看着对、实际在移动指针的答案。
 *
 * @param c   检查器。
 * @param e   被检查的 `EX_BIN` 节点，取它的源码位置。
 * @param lt  左操作数类型。
 * @param rt  右操作数类型。
 * @param op  运算符拼写，用于消息。
 * @return    错误类型，调用方可直接返回。
 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

/**
 * @brief 检查池原语 `poolSlice` / `poolResize` / `poolGive` 及前两者的 `...Raw` 变体。
 *
 * 这些是通向**池自己的 plate** 的类型化入口。`...Raw` 拼写只少了一件事：不把交出去的字节清零。
 * 对于每个槽都先写后读的列（`map` 的四处存储、`vector`/`string` 的缓冲、`hashMap` 的
 * `keys`/`slot`/`bucketOfDense`），清零是纯多余的工作：写的字节马上被覆盖，还可能提交永不使用的
 * 页。必须保留清零的是 `hashMap` 的 `tag`（空桶就是零）与 `pool` 自己的 `ent`（`pFree` 把
 * `low == 0` 读作空闲）。回归检查用两个哨兵钉住这件事：投毒块不得改变程序输出，memcheck 的未
 * 初始化读不得多于记录基线。
 *
 * 为什么不能做成库里的 `extern!`：`extern!` 不许返回 `slice<T>`（会变成两个 C 参数），而 extC 没有
 * 指针转换，库无法把一块字节变成类型化视图；生成器可以，所以由生成器做 —— 与 `alloc` 是原语而非
 * 库函数同理。`poolResize` 是 `grow` 用的那个，原因是峰值：take + copy + give 会同时持有新旧两块，
 * 原地 resize 则贴着它走。
 *
 * 这里**故意**不记 arena 层级：这块内存不属于 arena，池拥有它，归还块（`poolGive`）或释放池时它才
 * 回去，逃逸规则在这里没有话说；"引用不得比地方活得久"对池内存由容器自己的纪律保证（只有句柄
 * 能离开）。
 *
 * @param c     检查器。
 * @param e     泛型调用节点，取它的名字与实参。
 * @param elem  调用点写明的元素类型。
 * @return      `poolSlice` / `poolResize` 的 `mut slice<T>`、`poolGive` 的 `bool`；名字或实参不对
 *              时返回错误类型。
 */
static Type *checkPoolPrim(Checker *c, Expr *e, Type *elem) {
    TypeTable *tt = c->tt;
    const char *nm = e->u.gencall.name;
    /* `copyInto<T>(dst: mut slice<T>, src: slice<T>, n) -> i64`：一次受检的 `memmove`。它和池原语
     * 共用这个入口，因为生成器同样要写出元素类型；语言里没有别的办法做批量搬移（循环按元素付边界
     * 检查，容器还会按元素付陈旧池守卫（the container's own guard）。 */
    if (strcmp(nm, "copyInto") == 0) {
        if (e->u.gencall.args.len != 3) {
            ckError(c, e->line, NULL, "`copyInto` takes 3 arguments (the destination, the source,"
                                      " and how many elements)");
            return ttError(tt);
        }
        Expr *dE = *(Expr **)vecAt(&e->u.gencall.args, 0);
        Expr *sE = *(Expr **)vecAt(&e->u.gencall.args, 1);
        Expr *nE = *(Expr **)vecAt(&e->u.gencall.args, 2);
        Type *dT = checkValue(c, dE);
        Type *sT = checkValue(c, sE);
        Type *nT = checkValue(c, nE);
        if (!ttIsError(dT)) {
            Type *de = viewElemOf(dT);
            if (!de || !ttEquals(de, elem) || !dT->mut) {
                ckError(c, dE->line, NULL,
                        "`copyInto` writes into its destination, so that must be `%s`, not `%s`",
                        typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, dT));
                return ttError(tt);
            }
        }
        if (!ttIsError(sT)) {
            Type *se = viewElemOf(sT);
            if (!se || !ttEquals(se, elem)) {
                ckError(c, sE->line, NULL, "`copyInto` copies from `%s`, not `%s`",
                        typeStr(c, sliceOf(c, elem)), typeStr(c, sT));
                return ttError(tt);
            }
        }
        if (!ttIsError(nT) && !ttIsInteger(nT)) {
            ckError(c, nE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, nT));
            return ttError(tt);
        }
        if (elem->kind == TY_PARAM || ttHasParam(elem)) recordNewSizeCheck(c, elem, e->line);
        return c->tI64;
    }
    bool isTake    = strcmp(nm, "poolSlice") == 0 || strcmp(nm, "poolSliceRaw") == 0;
    bool isResize  = strcmp(nm, "poolResize") == 0 || strcmp(nm, "poolResizeRaw") == 0;
    size_t want    = isResize ? 3 : 2;
    if (e->u.gencall.args.len != want) {
        ckError(c, e->line, NULL, "`%s` takes %d arguments (the pool, %s)", nm, (int)want,
                isTake   ? "and how many elements"
              : isResize ? "the block, and how many elements it should hold"
                         : "and the block to give back");
        return ttError(tt);
    }
    Expr *ridE = *(Expr **)vecAt(&e->u.gencall.args, 0);
    Type *ridT = checkValue(c, ridE);
    if (!ttIsError(ridT) && !ttIsInteger(ridT)) {
        ckError(c, ridE->line, NULL, "the pool is named by its integer id, found `%s`",
                typeStr(c, ridT));
        return ttError(tt);
    }
    Expr *argE = *(Expr **)vecAt(&e->u.gencall.args, 1);
    Type *argT = checkValue(c, argE);
    if (isTake) {
        if (!ttIsError(argT) && !ttIsInteger(argT)) {
            ckError(c, argE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, argT));
            return ttError(tt);
        }
    } else if (isResize) {
        if (!ttIsError(argT)) {
            Type *se = viewElemOf(argT);
            if (!se || !ttEquals(se, elem)) {
                ckError(c, argE->line, NULL, "`%s` resizes an existing block: `%s`, not `%s`",
                        nm, typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, argT));
                return ttError(tt);
            }
        }
    }
    if (isTake || isResize) {
        Expr *nE = isResize ? *(Expr **)vecAt(&e->u.gencall.args, 2) : argE;
        Type *nT = isResize ? checkValue(c, nE) : argT;
        if (!ttIsError(nT) && !ttIsInteger(nT)) {
            ckError(c, nE->line, NULL, "the element count must be an integer, found `%s`",
                    typeStr(c, nT));
            return ttError(tt);
        }
        /* `T` has no size while a template is checked, exactly as in `new T[n]`, so the size
         * check is deferred to instantiation and recorded there. */
        if (elem->kind == TY_PARAM || ttHasParam(elem)) recordNewSizeCheck(c, elem, e->line);
        /* The count appears twice in the emitted C (once for the bytes, once for `.len`), so
         * an impure count is computed into a temporary first -- the same rule as `new T[n]`. */
        if (!repeatablePure(nE)) planSetNeedTemp(e, true);
        return ttViewMut(tt, sliceOf(c, elem), true);
    }
    if (!ttIsError(argT)) {
        Type *se = viewElemOf(argT);
        if (!se || !ttEquals(se, elem)) {
            ckError(c, argE->line, NULL,
                    "`poolGive` needs the block itself: `%s`, not `%s`",
                    typeStr(c, ttViewMut(tt, sliceOf(c, elem), true)), typeStr(c, argT));
            return ttError(tt);
        }
    }
    return c->tBool;
}

/**
 * @brief 解析 `dyn` 形式点名的 trait，并要求载荷实现了它。
 *
 * `dyn Trait(x)`（值）与 `dyn Trait(x).m(...)`（立即形式）共用这一个助手，因为规则只有一条，而
 * `impl Trait for T` 的记录就是答案。
 *
 * @param c          检查器。
 * @param traitName  `dyn` 写出的 trait 名。
 * @param payT       载荷类型，用来查实现。
 * @param line       诊断指向的源码行。
 * @return           命中的 trait；trait 未知时报告诊断并返回 NULL；载荷没实现时报告诊断，但仍
 *                   返回该 trait。
 */
static TraitDef *dynTraitOf(Checker *c, const char *traitName, Type *payT, int line) {
    TraitDef *tr = NULL;
    for (size_t i = 0; i < c->m->traits.len && !tr; i++) {
        TraitDef *cand = *(TraitDef **)vecAt(&c->m->traits, i);
        if (cand->name && strcmp(cand->name, traitName) == 0) tr = cand;
    }
    if (!tr) {
        ckError(c, line, "A trait is declared with `trait Name { ... }`, and a `dyn` value must name"
                " one that exists.", "`dyn` on unknown trait `%s`", traitName);
        return NULL;
    }
    /* 统一表只为真正被派发过的 trait 发出 —— 这也是对象不安全的方法（没有接收者、泛型、返回
     * `Self`）不必出现在表里的原因。 */
    planSetUsedDyn(tr, true);
    planSetUsesDyn(c->m, true);
    Type *pt = ttBase(payT);
    bool impl = false;
    for (size_t i = 0; i < c->m->impls.len && !impl; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&c->m->impls, i);
        /* `impl<T> Tag for pair<T>` 覆盖 `pair` 的**每个**实例，所以算数的是声明相同：直接比类型
         * 永远匹配不上 `pair<i64>` 与 `pair<T>`。 */
        Type *it = planImplTarget(im) ? ttBase(planImplTarget(im)) : NULL;
        impl = planImplTrait(im) == tr && it && pt &&
               (ttEquals(it, pt) || (it->sdef && it->sdef == pt->sdef));
        /* **泛型** impl 的方法由每个实例共享，所以在这里标记为已用：这是按实例发出的循环写出
         * `pair_i64_tag` 的原因；少了它，`dyn` 表会指向一个没人发出的函数。 */
        if (impl && it && it->sdef && it->kind == TY_GENERIC)
            for (size_t k = 0; k < im->methods.len; k++) {
                FuncDef *mf = *(FuncDef **)vecAt(&im->methods, k);
                planSetUsed(mf, true);
                planSetDynTable(mf, true);   /* its body is named by the vt thunk, from outside */
            }
    }
    if (pt && pt->sdef && !impl)
        ckError(c, line, "A `dyn` value names a trait its payload implements, because the"
                " implementation is what the table points at.",
                "`%s` does not implement `%s`", pt->name, traitName);
    return tr;
}


/* ---------------------------------------------------------------- lambda literals --------------
 *
 * 语法：`fn(x: i64) -> i64 { ... }`；参数单态化，所以语言里不引入函数指针；**可写捕获**必须写明
 * `[mut ref x]`，因为读捕获按值捕获、不需要条目。
 *
 * 下面的顺序就是整个设计：
 *
 *   1. 函数体**原地**检查，作用域里放着 lambda 的参数，因此体内每个名字都按普通规则解析——包括
 *      遮蔽；只做名字的预扫描会把它搞错；
 *   2. 体内引用到该作用域**之外**的东西恰好就是捕获集：它从解析好的 `Sym` 上读出来，不从文本猜；
 *   3. 每个捕获变成生成结构体的一个字段——按值（一次拷贝，闭包看到创建时的值）或对 `mut ref x`
 *      取外层变量的引用；
 *   4. 函数体变成该结构体的 `call` 方法，每个捕获名发射成 `self.<field>`（可写的是
 *      `(*self.<field>)`）。最后一步不需要动 AST：codegen 通过 `cname` 打印标识符，所以改写就是
 *      赋一个 `cname`。
 *
 * `f(x)`（`f` 持有这样一个值）是经由值的调用；调用点把它原地改写成方法调用 `f.call(x)` —— 与
 * EX_CALL 的枚举构造分支所做的改写相同。
 */
typedef struct {
    Checker *c;
    size_t   nOuter;      /* scopes below this index belong to the enclosing functions */
    Vec      caps;        /* Sym*: the capture set, in declaration order */
    Vec      written;     /* Sym*: the captured variables the body writes */
    bool     usedNew;
    bool     sawReturn;
} LamWalk;

/**
 * @brief 把一个绑定加入向量，已在里面的不再重复加入。
 *
 * @param v  目标向量（`Sym *`）。
 * @param s  要加入的绑定；NULL 忽略。
 */
static void lamAddUnique(Vec *v, Sym *s) {
    if (!s) return;
    for (size_t i = 0; i < v->len; i++)
        if (*(Sym **)vecAt(v, i) == s) return;
    *(Sym **)vecPush(v) = s;
}

/**
 * @brief 判断一个绑定是不是声明在 lambda 自己作用域的下方（也就是来自外层函数）。
 *
 * 成员资格按指针在作用域栈里查：`Sym.depth` 是 **arena** 深度，回答的是另一个问题。
 *
 * @param w  遍历状态，`nOuter` 是 lambda 之外那部分作用域的层数。
 * @param s  要查的绑定。
 * @return   绑定住在外层作用域则返回 true。
 */
static bool lamIsOuter(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->nOuter && i < w->c->scopes.len; i++) {
        Scope *sc = *(Scope **)vecAt(&w->c->scopes, i);
        for (size_t j = 0; j < sc->syms.len; j++)
            if (*(Sym **)vecAt(&sc->syms, j) == s) return true;
    }
    return false;
}

/**
 * @brief 判断一个捕获是否在函数体里被写过。
 *
 * @param w  遍历状态。
 * @param s  要查的绑定。
 * @return   写集合里有它则返回 true。
 */
static bool lamWritten(LamWalk *w, Sym *s) {
    for (size_t i = 0; i < w->written.len; i++)
        if (*(Sym **)vecAt(&w->written, i) == s) return true;
    return false;
}

/**
 * @brief 取赋值目标的根部绑定：`x[3].f = ...` 写的是 `x`。
 *
 * @param t  赋值目标表达式。
 * @return   沿下标、字段、解引用走到的最内层节点；走不动时返回当前节点。
 */
static Expr *lamRootOf(Expr *t) {
    while (t) {
        if (t->kind == EX_INDEX)      t = t->u.index.obj;
        else if (t->kind == EX_FIELD) t = t->u.field.obj;
        else if (t->kind == EX_DEREF) t = t->u.deref.operand;
        else break;
    }
    return t;
}

static bool lamWalkExpr(void *ctx, Expr *e);
static bool lamWalkStmt(void *ctx, Stmt *s);

/**
 * @brief 捕获扫描的表达式访问器：记下引用到的外层绑定与用到的 `new`。
 *
 * @param ctx  遍历状态 `LamWalk`。
 * @param e    表达式；NULL 直接通过。
 * @return     始终返回 true，遍历继续。
 */
static bool lamWalkExpr(void *ctx, Expr *e) {
    LamWalk *w = (LamWalk *)ctx;
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        if (sy && lamIsOuter(w, sy)) lamAddUnique(&w->caps, sy);
    } else if (e->kind == EX_NEW) {
        w->usedNew = true;
    }
    AstVisit v = { lamWalkExpr, lamWalkStmt, w };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief 捕获扫描的语句访问器：记下 `return` 与对根部外层绑定的赋值。
 *
 * @param ctx  遍历状态 `LamWalk`。
 * @param s    语句；NULL 直接通过。
 * @return     始终返回 true，遍历继续。
 */
static bool lamWalkStmt(void *ctx, Stmt *s) {
    LamWalk *w = (LamWalk *)ctx;
    if (!s) return true;
    if (s->kind == ST_RETURN) w->sawReturn = true;
    if (s->kind == ST_ASSIGN) {
        Sym *sy = identBindOf(lamRootOf(s->u.assign.target));
        if (sy && lamIsOuter(w, sy)) lamAddUnique(&w->written, sy);
    }
    AstVisit v = { lamWalkExpr, lamWalkStmt, w };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief 判断名字是否已经写在 lambda 的捕获列表里。
 *
 * @param lam   lambda 表达式节点。
 * @param name  要查的名字。
 * @return      捕获列表里有这个名字则返回 true。
 */
static bool lamCapListed(Expr *lam, const char *name) {
    for (size_t i = 0; i < lam->u.lambda.captures.len; i++)
        if (strcmp((*(LamCap **)vecAt(&lam->u.lambda.captures, i))->name, name) == 0) return true;
    return false;
}

/* 第 4 步：把捕获名改写成环境的字段。 */
typedef struct { Checker *c; LamWalk *w; Vec *fields; } LamRw;   /* fields: const char*, per cap */
static bool lamRwExpr(void *ctx, Expr *e);
static bool lamRwStmt(void *ctx, Stmt *s);

/**
 * @brief 捕获改写的表达式访问器：把捕获名换成环境字段的 C 名。
 *
 * @param ctx  改写状态 `LamRw`。
 * @param e    表达式；NULL 直接通过。
 * @return     始终返回 true，遍历继续。
 */
static bool lamRwExpr(void *ctx, Expr *e) {
    LamRw *r = (LamRw *)ctx;
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        Sym *sy = identBindOf(e);
        for (size_t i = 0; i < r->w->caps.len; i++) {
            if (*(Sym **)vecAt(&r->w->caps, i) != sy) continue;
            const char *fn = *(const char **)vecAt(r->fields, i);
            /* 可写捕获存的是引用：`(*self->c0)` 是左值，读和写都因此正确，节点类型也始终不变。 */
            planSetCName(e, arenaPrintf(r->c->arena,
                                        lamWritten(r->w, sy) ? "(*self->%s)" : "self->%s", fn), RKIND_EXPR);
            break;
        }
    }
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkExprChildren(e, &v);
}
/**
 * @brief 捕获改写的语句访问器，递归遍历子节点。
 *
 * @param ctx  改写状态 `LamRw`。
 * @param s    语句；NULL 直接通过。
 * @return     始终返回 true，遍历继续。
 */
static bool lamRwStmt(void *ctx, Stmt *s) {
    LamRw *r = (LamRw *)ctx;
    if (!s) return true;
    AstVisit v = { lamRwExpr, lamRwStmt, r };
    return astWalkStmtChildren(s, &v);
}

static Type *checkExprInner(Checker *c, Expr *e);   /* the struct-literal path is re-entered below */

/**
 * @brief 检查一个字面量 lambda，必要时合成它的环境结构体与 `call` 方法。
 *
 * 顺序：先造环境类型（字段稍后填，因为捕获集要等函数体查完），再造带 `self` 的 `call` 方法；然后
 * 在参数作用域里原地检查函数体，从解析好的绑定读出捕获集，校验写捕获必须写在 `[mut ref x]` 里，
 * 最后为每个捕获生成一个字段（按值或按可变引用）并把体内名字改写成字段。
 *
 * @param c  检查器。
 * @param e  `EX_LAMBDA` 节点。
 * @return   合成出的环境结构体类型；嵌套 lambda、body 含 `new`、捕获集不自洽或缺结果类型时报告
 *           诊断并返回错误类型。
 */
static Type *checkLambda(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    if (c->lamDepth > 0) {
        ckError(c, e->line,
                "A lambda inside a lambda would have to lay its environment out inside the outer"
                " one; version 1 refuses that instead of laying it out wrongly.",
                "a lambda inside a lambda is not supported yet");
        return ttError(tt);
    }
    c->lamDepth++;

    /* The environment's type exists before the body is checked, because the body is checked as the
     * `call` method of it: the method's `self` is a `ref` to this very type. Its fields are filled in
     * after the body (that is where the capture set comes from) -- layout is read later, by codegen,
     * so an empty field list now is not a problem. */
    const char *owner = (c->curFunc && c->curFunc->name) ? c->curFunc->name : "top";
    const char *lname = arenaPrintf(c->arena, "$lam$%s$%d", owner, c->lamSeq++);
    StructDef *sd = arenaAllocZero(c->arena, sizeof *sd);
    sd->name = lname;
    sd->srcName = lname;
    sd->ctx = c->ctx;
    sd->modName = c->curFunc ? c->curFunc->modName : NULL;
    sd->line = e->line;
    vecInit(&sd->typeParams, c->arena, sizeof(const char *));
    vecInit(&sd->fields, c->arena, sizeof(FieldDef *));
    vecInit(&sd->methods, c->arena, sizeof(FuncDef *));
    Type *lt = arenaAllocZero(c->arena, sizeof *lt);
    lt->kind = TY_STRUCT;
    lt->name = lname;
    lt->sdef = sd;
    vecInit(&lt->targs, c->arena, sizeof(Type *));

    /* `call` 方法在函数体检查**之前**就存在：lambda 里的 `return` 属于这个 lambda，而检查器从
     * `curFunc` 上读返回类型。没写 `->` 时，检查函数体的过程中 `ret` 一直是 NULL，之后由第一个
     * `return` 填上 —— 与具名函数在语法里享有的自由相同。 */
    FuncDef *cf = arenaAllocZero(c->arena, sizeof *cf);
    cf->name = "call";
    cf->modName = sd->modName;
    cf->ctx = c->ctx;
    cf->line = e->line;
    cf->ret = e->u.lambda.ret;
    vecInit(&cf->params, c->arena, sizeof(Param *));
    {
        Param *self = arenaAllocZero(c->arena, sizeof *self);
        self->name = self->cname = "self";
        self->line = e->line;
        Type *st = arenaAllocZero(c->arena, sizeof *st);
        st->kind = TY_REF;
        st->inner = lt;
        st->mut = false;                     /* `call` reads the environment */
        self->type = st;
        *(Param **)vecPush(&cf->params) = self;
    }
    for (size_t i = 0; i < e->u.lambda.params.len; i++)
        *(Param **)vecPush(&cf->params) = *(Param **)vecAt(&e->u.lambda.params, i);
    /* A declared function's signature is resolved before its body is checked; a synthesized one
     * goes through the same step, so its parameter types are resolved rather than left as the
     * parser's raw nodes. */
    resolveSignature(c, cf);
    {   /* the signature the user wrote, for diagnostics: `fn(i64) -> i64` */
        Buf sig;
        bufInit(&sig, c->arena);
        bufPuts(&sig, "fn(");
        for (size_t i = 0; i < e->u.lambda.params.len; i++) {
            Param *p = *(Param **)vecAt(&e->u.lambda.params, i);
            bufPrintf(&sig, "%s%s", i ? ", " : "", typeStr(c, p->type));
        }
        bufPrintf(&sig, ") -> %s", cf->ret ? typeStr(c, cf->ret) : "?");
        anSetLamSig(sd, bufCstr(&sig));
    }

    /* (1) the parameters in scope, so the body's names resolve by the ordinary rules -- shadowing
     * included, which a name-only pre-pass would get wrong. */
    const size_t nOuter = c->scopes.len;     /* below this: the enclosing functions' scopes */
    pushScope(c);
    for (size_t i = 0; i < e->u.lambda.params.len; i++) {
        Param *p = *(Param **)vecAt(&e->u.lambda.params, i);
        Sym *sy = declare(c, p->name, p->type, true, false, p->line, 0);
        p->cname = sy->cname;
    }
    FuncDef *saveCur = c->curFunc;
    c->curFunc = cf;
    resultsEnterOwner(cf);   /* a lambda body is its own scope */
    /* 未写 `->` 时，结果类型由函数体里第一个 `return <value>` 决定，由 ST_RETURN 分支填上。 */
    anSetLamInferRet(cf, (cf->ret == NULL));
    checkStmt(c, e->u.lambda.body);
    anSetLamInferRet(cf, false);
    c->curFunc = saveCur;
    resultsEnterOwner(saveCur);
    popScope(c);

    if (!cf->ret) {
        ckError(c, e->line,
                "A lambda's result type has to be inferable: either write `-> T`, or give the body a"
                " `return <value>`.",
                "cannot infer the result type of this lambda");
        cf->ret = ttError(tt);
    }

    /* (2) the capture set, read off the resolved bindings (not guessed from the text) */
    LamWalk w;
    w.c = c; w.nOuter = nOuter; w.usedNew = false; w.sawReturn = false;
    vecInit(&w.caps, c->arena, sizeof(Sym *));
    vecInit(&w.written, c->arena, sizeof(Sym *));
    {
        AstVisit v = { lamWalkExpr, lamWalkStmt, &w };
        astWalkStmtChildren(e->u.lambda.body, &v);
    }
    /* The `call` method is skipped by `checkFunc` (its body was already checked above), so the
     * "does a value-returning function return?" check that a named function gets from there has to
     * happen here -- otherwise the generated C falls off the end of a non-void function. */
    if (cf->ret && !ttIs(cf->ret, "void") && !w.sawReturn)
        ckError(c, e->line,
                "The lambda declares a result type, so its body has to return one.",
                "this lambda returns no value, but its type says `%s`", typeStr(c, cf->ret));

    if (w.usedNew) {
        ckError(c, e->line,
                "A lambda runs wherever the value is called from, so which arena a `new` inside it"
                " would belong to is not decided yet. Take the place as a parameter instead.",
                "a lambda body may not allocate yet");
    }

    /* 每个可写捕获都写在源码里 —— 逃逸与 arena 遍历也读这一点：每个可写别名都出现在程序文本中
     * （so the reader's pass can see every writable alias）。 */
    for (size_t i = 0; i < w.written.len; i++) {
        Sym *sy = *(Sym **)vecAt(&w.written, i);
        if (!lamCapListed(e, sy->name))
            ckError(c, e->line,
                    "A lambda that writes an enclosing variable must say so, so that every writable"
                    " alias stays visible in the source: write `[mut ref %s]` after the parameter"
                    " list.",
                    "the body writes `%s`, but the capture list does not name it", sy->name);
    }
    for (size_t i = 0; i < e->u.lambda.captures.len; i++) {
        LamCap *cap = *(LamCap **)vecAt(&e->u.lambda.captures, i);
        bool found = false;
        for (size_t j = 0; j < w.caps.len && !found; j++)
            found = strcmp((*(Sym **)vecAt(&w.caps, j))->name, cap->name) == 0;
        if (!found)
            ckError(c, cap->line, NULL,
                    "`%s` is not an enclosing variable this body uses", cap->name);
    }

    /* (3) the environment: one field per capture, in declaration order, and the value it is built
     * with at the place the lambda is written. */
    Vec fields;                              /* const char*: the generated field of caps[i] */
    vecInit(&fields, c->arena, sizeof(const char *));
    Vec inits;                               /* FieldInit* */
    vecInit(&inits, c->arena, sizeof(FieldInit *));
    for (size_t i = 0; i < w.caps.len; i++) {
        Sym *sy = *(Sym **)vecAt(&w.caps, i);
        bool wr = lamWritten(&w, sy);
        FieldDef *fd = arenaAllocZero(c->arena, sizeof *fd);
        fd->name = arenaPrintf(c->arena, "c%zu", i);   /* index-based: two captures may share a name */
        fd->line = e->line;
        if (wr) {
            Type *rt = arenaAllocZero(c->arena, sizeof *rt);
            rt->kind = TY_REF;
            rt->inner = sy->type;
            rt->mut = true;
            fd->type = rt;
        } else {
            fd->type = sy->type;             /* a copy: the closure sees the value it was made with */
        }
        *(FieldDef **)vecPush(&sd->fields) = fd;
        *(const char **)vecPush(&fields) = fd->name;

        Expr *id = exprNew(c->arena, EX_IDENT, e->line);
        id->u.ident.name = sy->name;
        planSetCName(id, sy->cname, RKIND_EXPR);
        id->type = sy->type;
        Expr *val = id;
        if (wr) {                            /* `mut ref x`: a reference to the variable itself */
            Expr *rf = exprNew(c->arena, EX_REF, e->line);
            rf->u.ref.operand = id;
            rf->type = fd->type;
            val = rf;
        }
        FieldInit *fi = arenaAllocZero(c->arena, sizeof *fi);
        fi->name = fd->name;
        fi->value = val;
        *(FieldInit **)vecPush(&inits) = fi;
    }

    {   /* (4) rewrite the captured names to the environment's fields */
        LamRw rw; rw.c = c; rw.w = &w; rw.fields = &fields;
        AstVisit v = { lamRwExpr, lamRwStmt, &rw };
        astWalkStmtChildren(e->u.lambda.body, &v);
    }

    cf->owner = sd;
    cf->body = e->u.lambda.body;
    anSetLamChecked(cf, true);                   /* checked above, in the scope it was written in */
    *(FuncDef **)vecPush(&sd->methods) = cf;
    /* 结构体必须从模块可达：codegen 从 `m->structs` 收集要发出的结构体类型与方法。 */
    *(StructDef **)vecPush(&c->m->structs) = sd;

    c->lamDepth--;

    /* The value **is** the environment. It stays an EX_LAMBDA node -- codegen writes the literal out
     * from the field values stored here -- because the generated type has no name in the source for
     * the ordinary struct-literal path to look up. */
    e->u.lambda.sdef = sd;
    e->u.lambda.tname = lname;
    e->u.lambda.inits = inits;
    e->type = lt;
    return lt;
}

/**
 * @brief 取**函数名当值用**时的类型：函数自己的签名，作成一个 `fn` 类型。
 *
 * 签名必须和 C 的完全一致，所以三种形状各给一个理由拒绝：**泛型**（每个实例一具函数体，裸名字不是
 * 单个代码指针）、**方法**（第一个参数是接收者，经由指针的调用不会写它；包一层的自由函数才是诚实
 * 的写法）、**协程**（它的值是帧句柄，不是代码指针）。第四种拒绝只能留给 codegen：最终带上隐藏
 * home arena 或 zone 参数的函数，其 C 签名多一个 `fn` 类型没有的参数，而这两个标志要等所有函数体
 * 检查完才定下来。
 *
 * 这里只覆盖最小的一种 `fn` 值：已存在函数的地址；无捕获 lambda 变成 `fn` 值不在这条路径上。
 *
 * @param c  检查器。
 * @param e  携带名字的 `EX_IDENT` 节点。
 * @param f  名字解析到的函数。
 * @return   函数类型，同时写回节点；报告诊断后返回错误类型。
 */
static Type *fnValueOf(Checker *c, Expr *e, FuncDef *f) {
    if (f->typeParams.len > 0) {
        ckError(c, e->line,
                "Which instance? A `fn` value is one code pointer, and a generic function has one"
                " body per instantiation. Call it, or write a free function with the signature you"
                " want.",
                "`%s` is generic, so its bare name is not a value", e->u.ident.name);
        return ttError(c->tt);
    }
    if (funcIsMethod(f)) {
        ckError(c, e->line,
                "A method's first parameter is its receiver, which a call through a pointer would"
                " have to write out. Write a free function that forwards to it.",
                "`%s` is a method, not a function, so its name is not a `fn` value", e->u.ident.name);
        return ttError(c->tt);
    }
    if (planIsCoro(f)) {
        ckError(c, e->line,
                "A coroutine's value is a frame handle that some task drives, not a code pointer.",
                "`%s` is a coroutine, so its name is not a `fn` value", e->u.ident.name);
        return ttError(c->tt);
    }

    Vec ps;
    vecInit(&ps, c->arena, sizeof(void *));
    for (size_t i = 0; i < f->params.len; i++)
        *(Type **)vecPush(&ps) = tsub(c, (*(Param **)vecAt(&f->params, i))->type);
    Type *ft = ttFn(c->tt, &ps, tsub(c, f->ret ? f->ret : ttVoid(c->tt)));

    planSetCallee(e, f);
    /* Taking the address is a use like any other: codegen emits only the functions something
     * calls or names, so without this the definition would be missing and the C compiler would
     * report an undeclared identifier. */
    planSetUsed(f, true);
    return ft;
}

/**
 * @brief 不做完整检查就算出一个**字段访问**应有的类型。
 *
 * 为什么要有它，而不是对 callee 做一次 `checkInto`：`o.f(x)` 被解析成方法调用，在方法路径认领它
 * 之前，"`f` 是不是函数类型的字段"必须能便宜地回答；为回答去检查 `o.f` 会把接收者的检查在普通
 * 路径上再跑一遍，而那条路径是每个程序的每次方法调用都要走的。
 *
 * @param c      检查器。
 * @param field  `EX_FIELD` 节点。
 * @return       字段类型，泛型接收者已替换；答案拿不到这么便宜时返回 NULL。NULL 只表示"不是我的
 *               情形"，绝不是错误：调用方会落到普通路径，由那里报告程序的问题。
 */
static Type *peekFieldType(Checker *c, Expr *field) {
    if (!field || field->kind != EX_FIELD) return NULL;
    Expr *obj = field->u.field.obj;
    if (!obj || obj->kind != EX_IDENT) return NULL;
    Sym *s = lookup(c, obj->u.ident.name);
    if (!s || !s->type) return NULL;
    Type *bt = ttBase(tsub(c, s->type));
    if (ttIsError(bt)) return NULL;
    StructDef *sd = structOf(bt);
    if (!sd) return NULL;
    FieldDef *fd = findField(sd, field->u.field.name);
    if (!fd || !fd->type) return NULL;
    /* The same substitution the field-access case does: a field type may mention the receiver's
     * type arguments. */
    return (bt->kind == TY_GENERIC)
               ? tsub(c, ttSubstitute(c->tt, fd->type, &sd->typeParams, &bt->targs))
               : tsub(c, fd->type);
}

/**
 * @brief 把一个 `fn` 类型的参数物化成 `Param` 节点。
 *
 * 实参规则是按声明的参数列表写的（逐参 `checkAssignable`，再对整表 `checkCallRefArgs`），而函数
 * 类型是一份没有声明的签名。为间接调用把那些规则再写一遍，就是让一条规则变成会漂移的两条，所以
 * 这里给它们同样的形状。名字故意留空：函数类型没有参数名，这条路径上也没有谁打印它。
 *
 * @param c   检查器，用于分配。
 * @param ft  函数类型。
 * @return    与 `ft->params` 一一对应的 `Param *` 向量。
 */
static Vec fnTypeParams(Checker *c, Type *ft) {
    Vec ps;
    vecInit(&ps, c->arena, sizeof(void *));
    for (size_t i = 0; i < ft->params.len; i++) {
        Param *p = (Param *)arenaAllocZero(c->arena, sizeof(Param));
        p->name = p->cname = "";
        p->type = *(Type **)vecAt(&ft->params, i);
        *(Param **)vecPush(&ps) = p;
    }
    return ps;
}

/**
 * @brief 检查 `f(x)`，其中 `f` **持有函数类型的值**，而不是一个声明。
 *
 * 签名就是值自己的，所以每个实参都对着这个类型匹配。
 *
 * @param c   检查器。
 * @param e   调用节点 `EX_CALL`。
 * @param ft  callee 的函数类型。
 * @return    签名携带的返回类型；实参个数或类型不符时报告诊断。
 *
 * callee 表达式是被检查的，不是被查找的：codegen 照原样打印它，它的绑定（`cname`）必须像其他
 * 标识符一样由检查器冻结。`checkCallRefArgs` 故意拿到 **NULL** 的被调者：代码指针不带 `effects`
 * 签名（签名是声明的属性，这个值没有声明），所以诚实的摘要就是保守的那一个 —— "它可能存下给它的
 * 任何东西"；这与不带子句的 `extern!` 拿到同一个默认值。
 */
static Type *checkCallThroughFn(Checker *c, Expr *e, Type *ft) {
    checkInto(c, NULL, e->u.call.callee);      /* freezes the callee's C name, so codegen's printer can use it */

    Vec ps = fnTypeParams(c, ft);
    if (e->u.call.args.len != ps.len) {
        ckError(c, e->line, NULL, "this `fn` value expects %zu argument(s), got %zu",
                ps.len, e->u.call.args.len);
        return ft->ret;
    }
    for (size_t i = 0; i < ps.len; i++) {
        Param *p = *(Param **)vecAt(&ps, i);
        Expr  *a = *(Expr **)vecAt(&e->u.call.args, i);
        adoptContextType(a, p->type);
        Type *at = checkInto(c, p->type, a);

        /* The same spelling rule as an ordinary call: passing a reference to a `ref T` parameter is
         * written `ref ...` at the site, so a reader sees a reference and not a copy. */
        if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
            ckError(c, a->line,
                    "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                    "`ref` is a mutable reference, so the target must be a `var`",
                    "argument expects `%s`; write `ref ...` here to pass a reference",
                    typeStr(c, p->type));
            continue;
        }
        checkAssignable(c, p->type, at, a, "argument");
    }
    /* 这次调用按谁的签名？`fn` 值的**类型**里不带 `effects` —— 子句写在声明上，而函数类型不是
     * 声明。能带它的是指针出来的那个**槽位**：函数类型的字段可以写 `effects Addr=0 Cont=0`，而
     * 这次调用的被调者正是那个字段。`fn` 被拷进局部就丢掉签名，这是丢它最诚实的方向。
     *
     * mask 交给签过名的 `extern!` 走的**同一个**规则：`checkCallRefArgs` 只要一个合成声明，两边
     * 因而不会漂移。
     *
     * 没有子句的槽位拿到最坏情况，与没签名的 `extern!` 一样：每个实参都可能被永久保留
     * （`collectEffects` 为这一档填同样的全 1 mask）。这才让"签名是承诺"在调用点有意义。 */
    FuncDef *sig = (FuncDef *)arenaAllocZero(c->arena, sizeof(FuncDef));
    sig->isExtern    = true;        /* "a black box whose signature is the whole basis" */
    anSetEffComplete(sig, true);
    {
        FieldDef *fd = (e->u.call.callee && e->u.call.callee->kind == EX_FIELD)
                           ? planField(e->u.call.callee) : NULL;
        if (fd && fd->hasEffects) {
            sig->name     = fd->name;
            sig->hasEffects = true;
            anSetAddrMask(sig, fd->effAddrMask);
            anSetContMask(sig, fd->effContMask);
        } else {
            /* Named after the slot when there is one: "argument 1 of `copy` may be kept by C
             * forever" points at the line to sign. The message wraps this in backticks, so it has to
             * be a bare word. */
            sig->name = fd ? fd->name : "fn";
            for (size_t i = 0; i < ps.len && i < 32; i++) {
                anSetAddrMask(sig, anAddrMask(sig) | (1u << i));
                anSetContMask(sig, anContMask(sig) | (1u << i));
            }
        }
    }
    checkCallRefArgs(c, sig, &e->u.call.args, &ps, 0, e->line, sig->name);

    planSetCallViaFn(e, true);
    return ft->ret;
}

/**
 * @brief 检查一个表达式并返回它的类型：检查器里的递归核心。
 *
 * `checkExpr` 在它外面包了每个子表达式都需要的记账。检查过程中有些节点会被**原地**改写，所以调用
 * 方不能指望 `e->kind` 保持进入时的样子。
 *
 * @param c  检查器。
 * @param e  表达式节点；不能为 NULL。
 * @return   表达式类型；报过诊断后返回错误类型。
 */
static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:
            /* 字面量自己的类型是装得下它的最小整数：放得下就是 `i32`，否则 `i64`，所以泛型体里
             * 未标注的 `let n = 5000000000` 仍是 64 位。隐式的只有无损加宽，没有隐式截断；想要别的
             * 整数类型的上下文通过 `literalFits` / `adoptContextType` 自己给字面量定类型。 */
            if (e->u.ival < -2147483648LL || e->u.ival > 2147483647LL) return c->tI64;
            return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_NULL: {
            /* The type was parked on `e->type` by `adoptContextType`. If the context did
             * not say which `?ref T` this stands for, report it rather than guess: extC is
             * explicit instead of inferring, and the context's silence is not an answer. */
            Type *nt = e->type;
            if (nt && nt->kind == TY_REF && nt->nullable) return nt;
            ckError(c, e->line,
                    "`null` is the zero value of a nullable reference, so its type must be"
                    " clear from context -- e.g. `var p: ?ref node = null`, or a parameter"
                    " or return type of `?ref T`",
                    "`null` here does not know which `?ref T` it is");
            return ttError(tt);
        }

        case EX_IDENT: {
            Sym *s = lookup(c, e->u.ident.name);
            if (s && s->modName && !anQualified(e))
                requireQualified(c, e->u.ident.name, s->modName, false, e->line);
            if (!s) {
                /* A **function name used as a value**: `var f: fn(i64) -> i64 = double_it`.
                 *
                 * `lookup` finds bindings -- locals, parameters, globals -- and a function is not one,
                 * so this is where a function name arrives: at the point the language would otherwise
                 * say "undefined name". The expression's type is the function's **own signature**,
                 * built here; no context is consulted, exactly as every other expression has a type
                 * of its own (an annotation on the left of `=` then checks against it as usual).
                 *
                 * Three kinds of function cannot be a `fn` value, and each is refused with its own
                 * reason rather than being allowed to produce C that does not compile; see
                 * `fnValueOf`. */
                FuncDef *fv = findFunc(c, e->u.ident.name);
                if (fv) return fnValueOf(c, e, fv);
                /* A bare variant name, with no payload. After `type st = | ok | bad`,
                 * writing `let s: st = ok` would otherwise report `undefined name `ok``,
                 * which does not show that the qualified form `st.ok` is wanted. A variant
                 * is named by its type, so this gets the same guidance as a bare payload
                 * constructor does. */
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, e->u.ident.name)) owner = td;
                }
                if (owner) {
                    const char *note = arenaPrintf(c->arena,
                            "A variant is named by its type -- write `%s.%s`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), e->u.ident.name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a value -- did you mean `%s.%s`?",
                            e->u.ident.name, DN(owner), DN(owner), e->u.ident.name);
                    return ttError(tt);
                }
                ckError(c, e->line, "every name must be declared first: locals, parameters, globals,"
                                " and imported module members all count",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* Name resolution is frozen here: codegen prints `cname` directly. This is
             * what tells shadowed names apart (`a` from `a__2`). */
            planSetCName(e, s->cname, RKIND_EXPR);
            /* Pin the resolved binding itself onto the node. The solving pass at the end
             * of the run happens after every function body has been checked, when the
             * scopes are already popped, so looking the name up again would either fail or
             * find a different binding that happens to share the name. See `IdentBinding`. */
            {
                IdentBinding *ib = (IdentBinding *)arenaAllocZero(c->arena, sizeof(IdentBinding));
                ib->sym = s;
                e->u.ident.sym = ib;
            }
            /* An `if p != null` test already proved this, so hand out the non-null
             * reference. Then `p.field`, `p.method()`, and passing it to a `ref T`
             * parameter all follow, and not one runtime check has to be generated. */
            Type *sty = s->type;
            if (sty && sty->kind == TY_REF && sty->nullable && isNarrowed(c, s->cname)) {
                Type *nn = ttRef(tt, sty->inner);
                nn->mut = sty->mut;
                return nn;
            }
            return sty;
        }

        case EX_BIN: {
            const char *op0 = e->u.bin.op;

            /* The short circuit of `&&` / `||` is a narrowing point too:
             * `p != null && p.value > 3` may dereference on the right, because reaching
             * the right side means the left side held. */
            if (isLogicOp(op0)) {
                Type *lt = checkValue(c, e->u.bin.left);
                expectBool(c, lt, e->u.bin.left);

                bool whenTrue = false;
                const char *tg = narrowTarget(c, e->u.bin.left, &whenTrue);
                const size_t mark = c->narrow.len;
                if (strcmp(op0, "&&") == 0) {
                    /* Reaching the right side means the left side was true, so every
                     * fact the left side proves holds here. */
                    narrowFactsOf(c, e->u.bin.left);
                } else if (tg && !whenTrue) {
                    /* `a || b`: reaching the right side means the left side was false.
                     * Only the simplest shape, `p == null`, yields "p is not null" when
                     * negated; the full negation of `||` would have to negate a
                     * conjunction, which is not implemented. */
                    pushNarrow(c, tg);
                }

                Type *rt = checkValue(c, e->u.bin.right);
                expectBool(c, rt, e->u.bin.right);
                c->narrow.len = mark;
                return c->tBool;
            }

            /* `p == null` / `p != null`: comparing against null is the only thing a
             * nullable reference is for. This has to run before the `checkValue` calls
             * below, because `null` does not know its own type. */
            if ((strcmp(op0, "==") == 0 || strcmp(op0, "!=") == 0) &&
                (e->u.bin.left->kind == EX_NULL || e->u.bin.right->kind == EX_NULL)) {
                Expr *nv = (e->u.bin.left->kind == EX_NULL) ? e->u.bin.left : e->u.bin.right;
                Expr *ov = (nv == e->u.bin.left) ? e->u.bin.right : e->u.bin.left;
                Type *ot = checkExpr(c, ov);
                if (ttIsError(ot)) return c->tBool;
                if (ot->kind == TY_REF && ot->nullable) {
                    adoptContextType(nv, ot);
                    checkExpr(c, nv);
                    return c->tBool;
                }
                if (ot->kind == TY_REF) {
                    ckError(c, e->line,
                            "only a nullable reference (`?ref T`) can be null -- a plain"
                            " `ref T` is guaranteed non-null, that is what it means",
                            "`%s` is not nullable, so it can never be `null`", typeStr(c, ot));
                    return c->tBool;
                }
                ckError(c, e->line,
                        "`null` is the zero value of a nullable reference, so compare it"
                        " with one: `if p != null { ... }`",
                        "`null` cannot be compared with `%s`", typeStr(c, ot));
                return c->tBool;
            }

            /* `<<` 和 `>>` 把左操作数当**接收者**，不当值：`fin >> x >> line` 的第一个 `>>` 返回
             * `mut ref fin`，第二个从同一个对象继续，所以"引用不是值，要写 `*p`"这条值位置规则
             * 不能在这个操作数上触发。其余运算符照旧。 */
            bool shiftRecv = op0[0] == '<' || op0[0] == '>';
            Type *lt = shiftRecv ? checkExpr(c, e->u.bin.left) : checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

            if (isCmpOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return c->tBool;

                bool isEqOp = isEqualityOp(op);

                /* `==` / `!=`: builtins compare natively; a struct goes through its
                 * `==` method. The right operand does not have to be the same type as
                 * the left one: like every other overloadable operator, `==` is matched
                 * on the right operand's type exactly, so `vec2 == i64` is a method of
                 * its own and no conversion is invented for it. */
                if (isEqOp) {
                    bool same = ttEquals(lt, rt);
                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);

                    if (same && cmpIsNative(lt)) return c->tBool;

                    /* Arrays: `==` is derived by the compiler. The user cannot write an
                     * array type down, so it cannot be given an `==` method. */
                    if (same && lt->kind == TY_ARRAY) {
                        if (!typeSupportsOp(tt, lt->inner, op, lt->inner))
                            ckError(c, e->line,
                                    "An array's `==` is derived by the compiler, "
                                    "so its elements have to be comparable.",
                                    "`%s` cannot be compared: its element type `%s` does not define `==`",
                                    typeStr(c, lt), typeStr(c, ttBase(lt)->inner));
                        return c->tBool;
                    }

                    /* A type parameter: defer the check until the instance is known.
                     * Do not gate the record on `c->curFunc->owner`: that would cover
                     * methods only. A generic free function has no owner, so the
                     * `T: ==` requirement would never be checked. Record the use and
                     * ask once per instance whether this `T` has `==`. */
                    if (same && lt->kind == TY_PARAM) {
                        deferOp(c, e, op);
                        return c->tBool;
                    }

                    if (!sd) {
                        /* Different types with nothing to compare them: the rules below
                         * report it, and they are the ones that know how a numeric
                         * literal adapts to the other side. */
                        if (!same) goto notEq;
                        /* An enum with a payload is the type most likely to hit this: in
                         * C it is a struct, and C structs cannot be compared with `==`, so
                         * the message points the user at `match`. */
                        bool ep = b && b->kind == TY_ENUM && enumHasPayload(b->edef);
                        ckError(c, e->line,
                                ep ? "an enum with a payload is a tagged union -- "
                                     "compare it with `match`, or write a method that does"
                                   : NULL,
                                ep ? "`%s` is an enum with a payload, so it has no `%s`"
                                   : "`%s` does not support `%s`", typeStr(c, lt), op);
                        return c->tBool;
                    }

                    FuncDef *m = findOp(tt, b, op, rt, strcmp(op, "!=") == 0 ? "==" : NULL);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn ==(self: ref %s, other: %s) -> bool { ... }",
                                  DN(sd), DN(sd), typeStr(c, rt));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==` for `%s`, so it cannot be compared",
                                DN(sd), typeStr(c, rt));
                        return c->tBool;
                    }

                    planSetCallee(e, m);  planSetUsed(m, true);
                    setOpCallArgs(c, e, m);
                    return c->tBool;
                }
            notEq:

                /* Every other comparison is meaningful for numbers only. */
                if (ttIsNumeric(lt) && ttIsNumeric(rt) &&
                    (ttCanWiden(lt, rt) || ttCanWiden(rt, lt))) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;

                /* Ordering on a user type: the same protocol as `==` -- a method whose
                 * name is the operator. Nothing is derived from anything else here:
                 * `!=` is exactly the negation of `==` and so falls back, but `>` does
                 * not come from `<`. A derived answer would have to be explained in
                 * every diagnostic, and it would silently accept a type that defined
                 * only one of the pair. */
                StructDef *osd = structOf(ttBase(lt));
                if (osd) {
                    FuncDef *m = findOp(tt, ttBase(lt), op, rt, NULL);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn %s(self: ref %s, other: %s) -> bool { ... }",
                                  DN(osd), op, DN(osd), typeStr(c, rt));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `%s` for `%s`, so it cannot be ordered",
                                DN(osd), op, typeStr(c, rt));
                        return c->tBool;
                    }
                    planSetCallee(e, m);
                    planSetUsed(m, true);
                    setOpCallArgs(c, e, m);
                    return c->tBool;
                }

                /* A comparison whose operand is a type parameter: on the template `T` is
                 * opaque, so the answer depends on the instance. Record it and let the
                 * per-instance pass ask -- the same record and the same consumer that
                 * `==` uses. */
                if (lt->kind == TY_PARAM || rt->kind == TY_PARAM) {
                    deferOp(c, e, op);
                    return c->tBool;
                }
                ckError(c, e->line, isEqOp ? "only numbers, `bool`, enums, and structs that define `==` can be compared" : NULL,
                        "cannot compare `%s` with `%s`", typeStr(c, lt), typeStr(c, rt));
                return c->tBool;
            }
            if (isBitOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return ttError(tt);
                /* **引用接收者**只为流运算符放行，且仅当其基类型是定义了该运算符的结构体：这才让
                 * `fin >> x >> line` 成立 —— 第一个 `>>` 返回 `mut ref ifstream`，第二个从同一个
                 * 对象继续。其余情况仍报"数字的位置上出现了引用"，那才是有用的一句话。 */
                bool streamRecv = (op[0] == '<' || op[0] == '>') &&
                                  isRef(lt) && structOf(ttBase(lt)) != NULL;
                if (!streamRecv && (isRef(lt) || isRef(rt)))
                    return refNotANumber(c, e, lt, rt, op);
                /* `<<` and `>>` are shifts for integers and stream operators for a type
                 * that defines them. Which one applies is decided by the *static type of
                 * the left operand*, so this is a branch rather than overload resolution:
                 * a builtin integer shifts (rule three: builtins are never overridden),
                 * a struct with such a method calls it. */
                if (op[0] == '<' || op[0] == '>') {
                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (sd) {
                        FuncDef *m = findOp(c->tt, b, op, rt, NULL);
                        if (!m) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note,
                                      "define it inside `%s`:\n"
                                      "      fn %s(self: ref %s, other: %s) -> %s { ... }",
                                      DN(sd), op, DN(sd), typeStr(c, rt), DN(sd));
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` does not define `%s` for `%s`",
                                    DN(sd), op, typeStr(c, rt));
                            return ttError(tt);
                        }
                        Vec *sp = NULL, *sa = NULL;
                        if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                        planSetCallee(e, m);
                        planSetUsed(m, true);
                        setOpCallArgs(c, e, m);
                        return m->ret ? ttSubstitute(tt, m->ret, sp, sa) : ttVoid(tt);
                    }
                }
                if (!ttIsInteger(lt) || !ttIsInteger(rt)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `%s` to `%s` and `%s`",
                            op, typeStr(c, lt), typeStr(c, rt));
                    return ttError(tt);
                }
            }
            /* The same trap as in the bitwise case: `ttIsNumeric` strips `ref T` down to
             * `T`, so `p + 1` for `p: ref i64` would pass as arithmetic while the generated
             * C performs pointer arithmetic -- silently doing something else entirely. */
            if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
            /* The result type comes from the arithmetic rule, literal fitting included;
             * no second copy of that rule here. */
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkValue(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
            /* `~` is meaningful for integers only. */
            if (strcmp(e->u.un.op, "~") == 0) {
                if (!ttIsInteger(ot)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `~` to `%s`", typeStr(c, ot));
                    return ttError(tt);
                }
                return ot;
            }
            if (!ttIsNumeric(ot)) {
                ckError(c, e->line, NULL, "cannot negate `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            return ot;
        }

        case EX_FIELD: {
            /* First decide whether this names an enum variant, as in `Status.warn`, where
             * `Status` is a type name rather than a variable. */
            if (e->u.field.obj->kind == EX_IDENT) {
                const char *tn   = e->u.field.obj->u.ident.name;
                const char *vn   = e->u.field.name;
                if (!lookup(c, tn)) {
                    Type *et = ttFromName(tt, tn);
                    if (et && et->kind == TY_ENUM) {
                        Variant *v = findVariant(et->edef, vn);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, vn);
                            return ttError(tt);
                        }
                        /* Rewrite into an enum value node; codegen consumes it directly. */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        planSetAssocOwner(e, et);      /* the resolved type; instances need it */
                        return et;
                    }
                }
            }

            Type *rawT = checkExpr(c, e->u.field.obj);
            if (rejectNullableDeref(c, rawT, e->u.field.obj, "field access")) return ttError(tt);
            Type *bt = ttBase(rawT);
            StructDef *sd = structOf(bt);
            if (!sd) {
                ckError(c, e->line, NULL, "`%s` is not a struct, so it has no field `%s`",
                        typeStr(c, bt ? bt : e->type), e->u.field.name);
                return ttError(tt);
            }
            FieldDef *fd = findField(sd, e->u.field.name);
            if (!fd) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "fields of %s:", DN(sd));
                for (size_t i = 0; i < sd->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", DN(sd), e->u.field.name);
                return ttError(tt);
            }
            /* `@private` storage belongs to the module that declares it. This is the rule that
             * keeps a container's raw storage out of reach: `s.buf[0..n]` from another module
             * would be a view into pool storage, and a view that can be stored is the hole the
             * pool tier closes by never handing one out. */
            {
                const char *home = c->curFunc ? c->curFunc->modName : NULL;
                if (fd->isPrivate && sd->modName != home) {
                    ckError(c, e->line,
                            "Storage is private to the module that declares it; use the type's"
                            " own methods, which decide what may be lent out and for how long.",
                            "field `%s` of `%s` is private to module `%s`",
                            fd->name, DN(sd), sd->modName ? sd->modName : "this file");
                    return ttError(tt);
                }
            }
            planSetField(e, fd);
            /* A field type may mention type parameters; substitute the receiver's type
             * arguments for them. */
            Type *ftype = (bt->kind == TY_GENERIC)
                          ? ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs)
                          : fd->type;
            /* Path narrowing: inside `if h.p != null { ... }` a read of `h.p` yields the
             * non-null version. `narrowTarget` records the fact only when the root is a
             * local whose address was never taken, which is exactly the condition relied on
             * here. This mirrors the `EX_IDENT` rule (`isNarrowed(c, s->cname)`) above. */
            if (ftype && ftype->kind == TY_REF && ftype->nullable &&
                e->u.field.obj->kind == EX_IDENT && planCName(e->u.field.obj)) {
                const char *rc = planCName(e->u.field.obj);
                size_t kn = strlen(rc) + strlen(e->u.field.name) + 2;
                char *key = (char *)arenaAlloc(c->arena, kn);
                snprintf(key, kn, "%s.%s", rc, e->u.field.name);
                if (isNarrowed(c, key)) {
                    Type *nn = ttRef(tt, ftype->inner);
                    nn->mut = ftype->mut;
                    return nn;
                }
            }
            return ftype;
        }

        case EX_INDEX: {
            Type *ot = checkExpr(c, e->u.index.obj);

            /* 定义了 `[]` 的类型得到下标语法：`m[k]` 变成它本来就是的方法调用，整套方法机制 ——
             * 实参检查、返回类型、泛型实例的延迟检查 —— 原样适用，无需在这里抄第二份。下面内建的
             * 数组 / slice / view 路径留给其余所有类型。`[]` 与 `[]=` 是独立的名字：这里只有读侧，
             * 经由只有 `[]` 的类型赋值由赋值路径报告（写走 `[]=`）。 */
            {
                FuncDef *ix = ttIsError(ot) ? NULL : findMethod(ttBase(ot), "[]");
                if (ix) {
                    Vec args;
                    vecInit(&args, c->arena, sizeof(void *));
                    *(Expr **)vecPush(&args) = e->u.index.index;
                    Expr *recv = e->u.index.obj;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "[]";
                    e->u.method.args = args;
                    return checkExpr(c, e);
                }
            }

            Type *it = checkValue(c, e->u.index.index);
            if (ttIsError(ot)) return ttError(tt);

            /* 内建下标路径会解引用对象，所以可空引用必须先拿到非空证明 —— 与字段访问、`*`、方法接收
             * 者一样（它们都调这个）。少了它，`var p: ?ref [4]i32 = null` 后接 `p[1]` 会毫无诊断地
             * 编译出来，生成的 C 读 `(*p).data[1]`。上面的 `[]` 方法路径在接收者处有自己的检查，所以
             * 这一条故意排在它后面。 */
            if (rejectNullableDeref(c, ot, e->u.index.obj, "a subscript")) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, NULL,
                        "cannot index a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            /* 下标的错误恢复：它的失败已经在来处报告过，而 `self.tag[i]` 无论 `i` 出了什么事都仍是
             * u8。在这里传播错误会毒化元素的每一个后续使用，而被毒化的操作数会让之后的检查提前返回
             * —— 它们的延迟记录因此从未写下，代码生成于是发出错误的运算符。 */
            if (ttIsError(it)) return elem;
            if (!ttIsInteger(it)) {
                ckError(c, e->line, "An index must be an integer.",
                        "index must be an integer, found `%s`", typeStr(c, it));
                return ttError(tt);
            }

            /* A fixed array has a compile-time length, so a literal index that is out of
             * range is reported here instead of at runtime: what the compiler can prove, it
             * has to say. The rule matches the three slice bounds checked below, except that
             * a single half-open bound applies -- legal indices are [0, n). Both `b[-1]` and
             * `b[4]` for a `[4]T` are compile errors. */
            if (ob->kind == TY_ARRAY) {
                long long iv = 0;
                if (asIntLit(e->u.index.index, &iv)) {
                    long long n = (long long)ob->asize;
                    if (iv < 0 || iv >= n) {
                        ckError(c, e->line,
                                "The compiler can prove this index is out of range.",
                                "index %lld is not inside `%s` (length %lld)",
                                iv, typeStr(c, ot), n);
                        return ttError(tt);
                    }
                }
            }
            return elem;
        }

        case EX_SLICE: {
            /* `a[lo..hi]` is a view. Either bound may be absent: `lo` when the front is
             * omitted, `hi` when the back is. */
            Type *ot = checkExpr(c, e->u.slice.obj);
            if (ttIsError(ot)) return ttError(tt);

            /* Same rule as the subscript above: slicing a `?ref` view/array takes its address
             * without asking whether it is null (the second half). */
            if (rejectNullableDeref(c, ot, e->u.slice.obj, "a slice")) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, "Only a fixed array or a view can be sliced.",
                        "cannot slice a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }

            /* Slicing a fixed array takes the address of an element, so the base has to be
             * a place. Slicing a view is only pointer arithmetic on a value, which needs no
             * storage -- that is why a string literal can be sliced. */
            if (ob->kind == TY_ARRAY && !isPlace(e->u.slice.obj)) {
                ckError(c, e->line, "Slicing takes the address of an element, so the base must be a place.",
                        "cannot slice a temporary value; `%s` needs a variable, a field or an index",
                        typeStr(c, ot));
                return ttError(tt);
            }

            /* With a fixed-array base the length is a compile-time constant, so an omitted
             * bound is filled in as a literal:
             *     a[2..] -> a[2..15]      a[..] -> a[0..15]
             * Codegen then sees two literal bounds and can emit code with no checks. */
            if (ob->kind == TY_ARRAY) {
                if (!e->u.slice.lo) e->u.slice.lo = intLit(c, 0, e->line);
                if (!e->u.slice.hi) e->u.slice.hi = intLit(c, ob->asize, e->line);
            }

            for (int k = 0; k < 2; k++) {
                Expr *b = k == 0 ? e->u.slice.lo : e->u.slice.hi;
                if (!b) continue;
                Type *bt = checkValue(c, b);
                if (!ttIsError(bt) && !ttIsInteger(bt))
                    ckError(c, b->line, "A slice bound must be an integer.",
                            "slice bound must be an integer, found `%s`", typeStr(c, bt));
            }

            /* An error the compiler can prove is reported here; none of it is left to
             * runtime. Each bound is examined on its own: an out-of-range literal is wrong
             * whatever the other bound says. */
            if (ob->kind == TY_ARRAY) {
                long long n = (long long)ob->asize;
                Expr *lp = e->u.slice.lo, *hp = e->u.slice.hi;
                long long lv = 0, hv = 0;
                bool lOk = asIntLit(lp, &lv);
                bool hOk = asIntLit(hp, &hv);

                /* Each bound is examined on its own: an out-of-range literal is wrong
                 * whatever the other bound says. */
                if (hOk && (hv > n || hv < 0)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice end %lld is not inside `%s` (length %lld)",
                            hv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && (lv < 0 || lv > n)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice start %lld is not inside `%s` (length %lld)",
                            lv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && hOk && hv < lv) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice `%lld..%lld` ends before it starts", lv, hv);
                    return ttError(tt);
                }
            }
            /* A view inherits writability from what it was sliced out of:
             *   `var a` / a `mut ref` parameter          ->  `mut slice<T>`
             *   `let a` / a string literal / a read-only view  ->  `slice<T>`
             * That is how one view type expresses both permissions, without the two slice
             * types Rust needs. */
            return ttViewMut(tt, sliceOf(c, elem),
                             isWritablePlace(c, e->u.slice.obj));
        }

        case EX_ARRAYLIT: {
            /* The type comes from the context (`var a: [3]i32 = [...]`) or is taken from
             * the elements. */
            Type *want = (e->type && e->type->kind == TY_ARRAY) ? e->type : NULL;
            Type *elemT = NULL;

            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                Expr *el = *(Expr **)vecAt(&e->u.arraylit.elems, i);
                if (want) adoptContextType(el, want->inner);
                Type *t = checkValue(c, el);
                if (ttIsError(t)) continue;

                if (!elemT) {
                    elemT = want ? want->inner : t;
                }
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "array element %zu", i);
                checkAssignable(c, elemT, t, el, bufCstr(&what));
            }

            if (!elemT && want) elemT = want->inner;
            if (!elemT || ttIsError(elemT)) {
                ckError(c, e->line,
                        "An empty array literal needs the length and element type from context: "
                        "`var a: [3]i32 = [...]`.",
                        "cannot infer the element type of this array literal");
                return ttError(tt);
            }

            int64_t n = (int64_t)e->u.arraylit.elems.len;
            if (e->u.arraylit.rest) {
                if (!want) {
                    ckError(c, e->line,
                            "`...` fills the rest with zero values, so the length has to "
                            "come from the type.",
                            "`...` needs the array length from context");
                    return ttError(tt);
                }
                n = want->asize;
                if (n < (int64_t)e->u.arraylit.elems.len)
                    ckError(c, e->line, NULL,
                            "too many elements: `%s` holds %lld",
                            typeStr(c, want), (long long)want->asize);
            } else if (want && n != want->asize) {
                ckError(c, e->line,
                        "An array literal must give every element. Use a trailing `...` "
                        "to fill the rest with zero values.",
                        "`%s` needs %lld element(s), got %lld",
                        typeStr(c, want), (long long)want->asize, (long long)n);
                return ttError(tt);
            }

            Type *arr = ttArray(tt, n, elemT);
            if (want && !ttEquals(want, arr))
                ckError(c, e->line, NULL, "array literal does not match `%s`", typeStr(c, want));
            return arr;
        }

        case EX_REF: {
            Expr *op = e->u.ref.operand;
            Type *ot = checkExpr(c, op);
            /* Taking an address records that this binding's address has gone out. An alias
             * may exist, so a write through the binding is not necessarily the only write to
             * that storage.
             * The narrowing facts recorded for paths through it are dropped at the same
             * time: with an alias possible, `h.p` being non-null cannot be relied on. */
            { Sym *rs = placeRoot(c, op); if (rs) { unNarrow(c, rs->cname); rs->addressed = true; } }
            if (ttIsError(ot)) return ttError(tt);

            if (ot->kind == TY_REF) {
                ckError(c, e->line, "it is already a reference -- just pass it as is",
                        "`ref` applied to a value that is already a reference");
                return ot;
            }
            if (!isLvalue(op)) {
                ckError(c, e->line, "only variables and fields can be referenced",
                        "cannot take a reference to this expression");
                return ttError(tt);
            }
            /* Which reference comes out depends on whether the place is writable:
             *   a `var` variable / a `mut ref` parameter  ->  `mut ref T`
             *   a `let` variable / a `ref` parameter      ->  `ref T` (read-only)
             *
             * A read-only borrow may always be taken: that is the whole reason borrows
             * exist. While `ref` meant mutable only, taking a reference to a `let` was
             * rejected outright, so even a plain read could not be written. */
            Type *r = ttRef(tt, ot);
            r->mut = isWritablePlace(c, op);
            return r;
        }

        case EX_TRY:
            /* `?` forwards an error at statement level: it expands into "evaluate once,
             * test, return". C has no statement expressions, so it is legal in exactly three
             * positions:
             *     let x = e?   /   x = e?   /   return e?
             * Anywhere else, say `f(e? + 1)`, it is reported clearly instead of producing C
             * that does not compile. */
            ckError(c, e->line,
                    "`?` has to expand into statements (evaluate once, test, return), "
                    "so it only fits where a whole statement can be rewritten.",
                    "`?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?`");
            return ttError(tt);

        case EX_ASSOC: {            /* `option<i64>::some(x)`. An associated function is a
                                     * function written inside a `struct` without `self`.
                                     * The type arguments are written out in full rather than
                                     * inferred from the arguments or the context: extC is
                                     * explicit instead of inferring. */
            Type *raw = typeNamed(c->arena, e->u.assoc.typeName);
            raw->targs = e->u.assoc.targs;
            Type *t = ttResolve(tt, c->ctx, raw, e->line, c->curParams);
            if (ttIsError(t)) return ttError(tt);
            planSetAssocOwner(e, t);

            /* Constructing an enum variant goes through this path as well:
             * `option<i64>::some(3)` and `maybe<i64>::nothing`. So `::` has two readings for
             * an enum but one meaning only -- build a value -- and the prelude and existing
             * code keep writing it exactly as they do today. */
            StructDef *esd = structOf(t);
            if (!esd && t->kind == TY_ENUM && t->edef) {
                Variant *v = findVariant(t->edef, e->u.assoc.name);
                if (!v) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "variants of %s:", t->name);
                    for (size_t i = 0; i < t->edef->variants.len; i++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&t->edef->variants, i))->name);
                    ckError(c, e->line, bufCstr(&note),
                            "`%s` has no variant `%s`", t->name, e->u.assoc.name);
                    return ttError(tt);
                }
                Vec evargs = e->u.assoc.args;      /* union: copy it out first */
                e->kind = EX_ENUMVAL;
                e->u.enumval.typeName = t->name;   /* instance name, e.g. `maybe_i64` */
                e->u.enumval.variant  = v->name;
                e->u.enumval.args     = evargs;
                return checkExprInner(c, e);
            }

            StructDef *sd = structOf(t);
            FuncDef *f = NULL;
            if (sd) {
                for (size_t i = 0; i < sd->methods.len; i++) {
                    FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                    if (anIsAssoc(m) && strcmp(m->name, e->u.assoc.name) == 0) { f = m; break; }
                }
            }
            if (!f) {
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "associated functions of %s:", DN(sd));
                    bool any = false;
                    for (size_t i = 0; i < sd->methods.len; i++) {
                        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                        if (!anIsAssoc(m)) continue;
                        bufPrintf(&note, " %s", m->name);
                        any = true;
                    }
                    if (!any) bufPuts(&note, " (none)");
                    bufPuts(&note, "; a function written inside a `struct` without"
                                  " `self` is an associated function");
                } else {
                    bufPuts(&note, "only struct types have associated functions");
                }
                ckError(c, e->line, bufCstr(&note), "no associated function `%s` on `%s`",
                        e->u.assoc.name, e->u.assoc.typeName);
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区 ⇒ 不需要
                 * 新的可发送性分析）。 */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                /* 视图实参逐个对齐类型：`mut` 形参必须收到 `mut` 实参（它要被写）；只读形参两种都收
                 * （可变的东西只读地用是安全的）。 */
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);   /* codegen 据此生成 trampoline */
                planSetParTlsArena(wf, true);   /* worker 里的 new 走线程本地 arena */
                planSetUsed(wf, true);          /* trampoline 会点名它，所以必须发出 */
                {
                    /* 第一个实参不是表达式，是函数名：换成一个 i64 字面量，后面的实参照常走。 */
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* 关联函数也要算 arena 实参：它可能分配，也可能返回引用；少了这一步就是一个实参过少的
             * 调用，生成的 C 编不过。 */
            anSetHomeDepth(e, callHomeDepth(c, &e->u.assoc.args, &f->params, e));
            setCallArenaArg(c, e);      /* 定下这次调用最终传哪个 arena */
            setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
            /* 引用检查挪到本 case 末尾，等实参都检查完再做。 */

            if (e->u.assoc.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s::%s` expects %zu argument(s), got %zu",
                        e->u.assoc.typeName, f->name, f->params.len, e->u.assoc.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                Expr  *a = *(Expr **)vecAt(&e->u.assoc.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);
                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. ",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* `coroutine<T>` 是统一的句柄，一个普通的 24 字节值。 */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, pt, at, a, "argument");
                }
            }
            /* The reference check applies to an associated function as well, once its
             * arguments have been checked. */
            checkCallRefArgs(c, f, &e->u.assoc.args, &f->params, anHomeDepth(e),
                             e->line, e->u.assoc.name);
            return f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);
        }

        case EX_CONV: {
            /* `i32(x)`: an explicit conversion.
             *   - Widening, which is already implicit, may still be written out and generates
             *     no check at all.
             *   - Narrowing and sign changes trap when the value does not fit, with the
             *     source position in the message.
             *   - Integer to float and back follows C (truncation, round to nearest), with a
             *     range check on the float-to-integer direction.
             * A conversion the compiler can prove fits, such as `i32(u8 value)`, generates no
             * check either. */
            /* The one conversion that is not about numbers: a **code pointer**.
             *   `fn(i64) -> i64(p)`   C's `void *` becomes callable
             *   `ref void(f)`         a code pointer becomes an opaque address again
             * It is narrow on purpose. The only source type is `ref void` -- a pointer with **no
             * operations at all** (no `*`, no `[i]`, no `.field`), so the only thing it could ever
             * become is a call target, and the type written out *is* the author's signature, exactly
             * as `effects` is. A general pointer cast (`ref void` -> `ref point`) is deliberately not
             * here: that is the point where a type system stops being one.
             *
             * The null rule is the language's own: `fn` has no null value, so a **nullable** `?ref
             * void` (what `dlsym` returns when it fails) cannot become a `fn` without proving it is
             * not null first -- `p!` is how the author signs for that, and it is checked elsewhere. */
            /* `typeName` 是标记，不是 `type`：下面的数值路径会**写** `type`，而泛型体每个实例都要
             * 被检查一遍 —— 写回 `type` 会让第二次见到的数值转换被这个分支认领，报出"cannot
             * convert `i32` to `i64`"。解析器恰好只设两者之一：`i32(x)` 设名字，`fn(...) -> R (x)`
             * 设解析出的类型。 */
            if (!e->u.conv.typeName && e->u.conv.type) {
                Type *want = ttResolve(tt, c->ctx, e->u.conv.type, e->line, c->curParams);
                e->u.conv.type = want;
                /* `checkExpr`, not `checkValue`: in value position a reference is an error that
                 * tells the author to write `*p`, but **here the reference is the value** -- the
                 * pointer itself is what is being converted. `ref x` and `alloc` are the language's
                 * other two stated exceptions to that rule, for the same reason. */
                Type *src = checkExpr(c, e->u.conv.operand);
                if (ttIsError(src)) return ttError(tt);
                Type *sb = src->kind == TY_REF ? ttBase(src->inner) : NULL;
                bool srcVoid = sb && sb->kind == TY_VOID;
                if (want->kind == TY_FN && srcVoid) {
                    if (src->nullable) {
                        ckError(c, e->line,
                                "`dlsym` returns a nullable pointer, and a `fn` has no null value:"
                                " check it or sign for it first (`p!`), then convert.",
                                "cannot convert `%s` to `%s`: it may be null",
                                typeStr(c, src), typeStr(c, want));
                        return ttError(tt);
                    }
                    return want;
                }
                /* One direction only. Handing a code pointer back out as an opaque address has no
                 * user in this line (the table carries `fn` fields, `dlclose` takes the handle the
                 * loader gave back), and **not** having it is worth something: a code pointer stays
                 * a code pointer, and cannot be laundered into a data pointer through here. The
                 * spelling would also collide with `ref x` (`ref void(f)` reads as `ref (void(f))`),
                 * so a real need would come with its own syntax decision rather than a quiet one. */
                ckError(c, e->line,
                        "This conversion exists for one thing: turning a C code address into a"
                        " callable value -- `fn(i64) -> i64(p)` where `p` is a non-null `ref void`."
                        " Pointers to data change type where they are declared, not here.",
                        "cannot convert `%s` to `%s`", typeStr(c, src), typeStr(c, want));
                return ttError(tt);
            }
            Type *t = ttFromName(tt, e->u.conv.typeName);
            if (!t || t->kind != TY_BUILTIN) {
                ckError(c, e->line, NULL, "`%s` is not a scalar type", e->u.conv.typeName);
                return ttError(tt);
            }
            e->u.conv.type = t;
            /* `checkExpr`, not `checkValue`: this node owns one exception to the value-position rule.
             * `u64(p)` / `i64(p)` is **the address of `p`** -- the one thing the plate model needs
             * (`base <= p < base + size`, one comparison, then trust) and the one thing a reference
             * cannot be asked for any other way, because `*p` would read the pointee and there is no
             * pointer arithmetic to compare with.
             *
             * It is deliberately **one way**: nothing turns a number back into a reference, so a
             * pointer can be checked but never forged, and the only operations on the number are the
             * integer ones (compare, subtract, index). The other half of `checkValue`
             * (`rejectNoCopy`) has nothing to say here: a source that is numeric is not a `@noCopy`
             * value, and a source that is a reference is the case handled right here. */
            Type *src = checkExpr(c, e->u.conv.operand);
            if (ttIsError(src)) return ttError(tt);
            if (src->kind == TY_REF) {
                if (ttIs(t, "u64") || ttIs(t, "i64")) {
                    /* `convCheck = false`: there is nothing to check -- the bits are the same width,
                     * and a pointer's value is whatever the machine put there. */
                    planSetConvCheck(e, false);
                    return t;
                }
                ckError(c, e->line,
                        src->nullable
                          ? "it is a nullable reference (`?ref T`) -- check it first:"
                            " `if p != null { ... *p ... }`"
                          : "write `*p` where the value is needed (`*p + 1`, `let v = *p`, `f(*p)`)",
                        "`%s` is a **reference**, not a value -- dereference it first: `*p`",
                        typeStr(c, src));
                return ttError(tt);
            }
            bool si = ttIsInteger(src), sf = ttIsFloat(src);
            bool ti = ttIsInteger(t),   tf = ttIsFloat(t);
            if (!((si || sf) && (ti || tf))) {
                ckError(c, e->line,
                        "explicit conversions are between numbers: integers and floats",
                        "cannot convert `%s` to `%s`", typeStr(c, src), typeStr(c, t));
                return ttError(tt);
            }
            /* Float to float follows C (round to nearest; out of range becomes an infinity),
             * so it is not checked. An integer widening is lossless and keeps the signedness,
             * so it is not checked either. Integer to float is not checked either: every integer
             * is inside the target float's range, the only loss is precision (round to nearest,
             * as in C). Everything else -- narrowing, a sign change, float to integer -- is
             * checked. */
            bool lossless = (si && ti && ttCanWiden(src, t)) || (sf && tf) || (si && tf);
            planSetConvCheck(e, !lossless);
            return t;
        }

        case EX_NEW: {
            /* `new T`, `new [N]T`, and `new T[n]`:
             *   a single T      ->  `mut ref T`
             *   a fixed `[N]T`  ->  `mut ref [N]T`
             *   `T[n]`          ->  `mut slice<T>`, which is how a buffer is built
             * The memory comes from the arena of the current block and is zeroed: in extC a
             * fresh allocation always reads as zero. */
            Type *w = ttResolve(tt, c->ctx, e->u.new_.type, e->line, c->curParams);
            if (ttIsError(w)) return ttError(tt);
            e->u.new_.type = w;

            if (w->kind == TY_PARAM || ttHasParam(w)) {
                /* `T` has no size while a template is being checked, so the size check is
                 * deferred to instantiation and recorded. That is what makes `new T[cap]`
                 * inside `varArray<T>` writable at all. If the size is still unknown at
                 * instantiation -- a nested generic that never received its argument, say --
                 * the error is reported there. */
                recordNewSizeCheck(c, w, e->line);
            } else if (ttIs(w, "void") || ttIsError(w)) {
                ckError(c, e->line,
                        "`new` needs a concrete type: its size is decided at compile time",
                        "cannot `new` `%s` -- its size is not known here", typeStr(c, w));
                return ttError(tt);
            }

            /* 层级是当前块的深度：与生成的 C 选 `__extc_a[k]` 用的是同一个数。例外是带 home arena
             * 的函数（把自己的分配交给调用方）：站点记作 `ARENA_HOME`，在 `refDepth` 口径下读作 0，
             * 即"本帧之外那一层"，逃逸检查放行的正是这一层 —— `fn build() -> mut ref node` 里的
             * `return h` 因此合法。
             *
             * 这只是起点：逃逸检查发现新对象被存到活得更久的地方时，会把 `arenaLevel` 提升到那一层，
             * 而 `refDepth` 跟着 `arenaLevel` 走，两个数必须始终相同。同一个节点可能被检查两次，所以
             * 只在第一次赋值，且只朝活得更久的方向移动。
             *
             * 层级由检查器决定、codegen 只翻译。除 home arena 外按块走，但标了 `@overwrite` 的存储
             * 要活过循环的每一轮，所以它住在本帧、层级 1。这里读到的是当下的 `needsHome`，只覆盖直接
             * 证据；因为某个调用才获得 home arena 的函数，等闭包完整后再由检查模块那一趟用
             * `FuncDef.arenaSites` 重新决定。 */
            if (planArenaLevel(e) == 0)
                planSetArenaLevel(e, (c->curFunc && anNeedsHome(c->curFunc)) ? ARENA_HOME
                              : (anReuse(e) ? 1 : (int)c->scopes.len));
            c->allocSites++;
            /* Keep the lexical level in its own field: the branch above may have replaced
             * `arenaLevel` with the `ARENA_HOME` sentinel, while the solver still needs to
             * know which block the site started in. */
            if (anLexicalLevel(e) == 0)
                anSetLexicalLevel(e, anReuse(e) ? 1 : (int)c->scopes.len);
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame". */
            anSetMinAt(e, -1);
        *(Expr **)vecPush(&c->curArenaSites) = e;
            /* `refDepth` follows `arenaLevel`: the two must always be the same number. The
             * only exception is `ARENA_HOME`, whose sentinel is -1 while `refDepth` speaks of
             * 0 as "the level outside this frame", so it maps back to 0 -- the home arena is
             * the scope the caller chose, which is depth 0. */
            {
                int depth = arenaDepthOf(planArenaLevel(e));    /* the one conversion point */
                if (anRefDepth(e) == 0 || anRefDepth(e) > depth) anSetRefDepth(e, depth);
            }

            if (!e->u.new_.count) {
                Type *r = ttRef(tt, w);
                r->mut = true;                  /* freshly allocated storage is writable */
                return r;
            }

            /* `T[n]`: the count must be an integer. An impure count needs a temporary so
             * that it is not evaluated twice. */
            Type *nt = checkValue(c, e->u.new_.count);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, e->u.new_.count->line, NULL,
                        "the element count must be an integer, found `%s`", typeStr(c, nt));
                return ttError(tt);
            }
            if (!repeatablePure(e->u.new_.count)) planSetNeedTemp(e, true);
            /* Freshly allocated memory is writable, so the view is a `mut slice<T>`: the
             * same rule that gives `a[..]` a `mut slice<T>` when `a` is writable. */
            return ttViewMut(tt, sliceOf(c, w), true);
        }

        case EX_GENCALL: {
            /* A generic call. Only the built-in primitives use this shape today:
             * `alloc<i32>(n)` asks the arena of the current frame for room for n values of T
             * and returns a writable reference.
             *
             * It is a primitive rather than a library function because the arena itself has to
             * be generated by the compiler: the library needs it to allocate for itself, so the
             * library cannot provide it. */
            /* `maxOf<i32>(4, 3)`：带显式类型实参调用的自由函数，这是 `T` 只出现在返回类型里时
             * 唯一可用的拼写；否则只走内建原语（`alloc<T>(n)`）那条规则。 */
            /* `alloc<T>(n) -> mut ref T` is the one allocation primitive on this path: a
             * place for n values, reached through a reference.
             *
             * A buffer whose length is known only at run time -- reading a file of unknown
             * size, or the 64 KB the `reader` asks for -- is spelled `new T[n]`, which gives a
             * `mut slice<T>`. That is the only spelling: `allocSlice<T>(n)` is not a primitive,
             * and using it reports the migration below. */
            bool isAlloc  = strcmp(e->u.gencall.name, "alloc") == 0;
            /* The pool primitives share this path for the same reason `alloc` does: the
             * library needs memory the compiler has to name the type of. */
            bool isPoolPrim = isPoolPrimitiveName(e->u.gencall.name);
            if (!isAlloc && !isPoolPrim) {
                FuncDef *tf = findFunc(c, e->u.gencall.name);
                if (tf && tf->typeParams.len == e->u.gencall.targs.len && tf->typeParams.len > 0) {
                    Vec targs;
                    vecInit(&targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < e->u.gencall.targs.len; i++) {
                        Type *t = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, i),
                                            e->line, c->curParams);
                        if (ttIsError(t)) return ttError(tt);
                        *(Type **)vecPush(&targs) = t;
                    }
                    /* The type arguments still mention `T` here: a call to `idOf<T>(...)`
                     * inside a generic body. Nothing is recorded at this point, because the node
                     * is rewritten to EX_CALL and walked again below, and that path notices the
                     * `T` argument and records the deferred check itself. One place instead of
                     * two, so the two cannot drift apart. */
                    FuncDef *inst = funcInstance(c, tf, &targs, e->line);
                    /* `funcInstance` reports the limit it hit and returns NULL (type nesting past
                     * `TYPE_DEPTH_LIMIT`, or more than `FUNC_INST_LIMIT` instances). Dereferencing
                     * that NULL would crash the compiler *while reporting its own limit*
                     * (`z<box<...65 deep...<i64>>>()`). The error is already recorded, so
                     * stop here. */
                    if (!inst) return ttError(tt);
                    planSetUsed(inst, true);
                    planSetUsed(tf, true);
                    /* Rewrite the node into an ordinary call: the shape every path except
                     * `alloc` uses. */
                    Vec args = e->u.gencall.args;
                    Expr *id = exprNew(c->arena, EX_IDENT, e->line);
                    id->u.ident.name = tf->name;
                    e->kind = EX_CALL;
                    e->u.call.callee = id;
                    e->u.call.args   = args;
                    anSetQualified(e, true);      /* do not trigger the qualification check */
                    planSetCallee(e, inst);
                    /* 把写出来的类型实参经侧表交给下面的 EX_CALL 路径：没有它，那条路径会从头推断，
                     * `f<i64>(x)` 就只是装饰 —— 只在推断碰巧成功的地方可用，而在诊断推荐它的地方
                     * （`T` 只出现在返回类型里，或没有实参可供推断）失败。 */
                    ExplicitTargs *et = (ExplicitTargs *)arenaAllocZero(c->arena, sizeof *et);
                    et->node  = e;
                    et->targs = targs;
                    *(ExplicitTargs **)vecPush(&c->explicitTargs) = et;
                    /* 改写之后必须再走一遍这个节点：实参检查、返回类型与 home arena 都住在 EX_CALL
                     * 路径上，这里返回 void 是错的。 */
                    return checkExpr(c, e);
                }
                /* 被删掉的原语要给出迁移提示，而不是"不是内建"：这个拼写仍在示例、测试与用户代码里
                 * 出现。 */
                if (strcmp(e->u.gencall.name, "allocSlice") == 0) {
                    ckError(c, e->line,
                            "`new T[n]` says the same thing -- a zeroed `mut slice<T>` -- and is"
                            " the only spelling now. The arena memsets every allocation, which is"
                            " what the zeroing promise always rested on (SPEC section 0.6).",
                            "`allocSlice<T>(n)` was removed; write `new T[n]`");
                    return ttError(tt);
                }
                /* `varArray<i64>(200)`: the constructor of a **generic** type. The type
                 * arguments are written out just as `varArray<i64>::withCap(200)` writes
                 * them -- nothing is inferred -- and the node becomes the associated call
                 * for the rest of the pipeline. */
                if (!lookup(c, e->u.gencall.name)) {
                    Type *base = ttFromName(tt, e->u.gencall.name);
                    StructDef *gsd = structOf(base);
                    if (gsd && gsd->typeParams.len == e->u.gencall.targs.len) {
                        FuncDef *ctor = NULL;
                        for (size_t i = 0; i < gsd->methods.len && !ctor; i++) {
                            FuncDef *m = *(FuncDef **)vecAt(&gsd->methods, i);
                            if (anIsAssoc(m) && strcmp(m->name, "new") == 0) ctor = m;
                        }
                        if (ctor) {
                            /* 每个字段都在写 union 之前读出来：两种形状重叠，先写 `u.assoc.typeName`
                             * 会覆盖 `u.gencall.targs` 与 `u.gencall.args`，于是节点在一个该放列表的
                             * 地方放着数字。 */
                            const char *gname = e->u.gencall.name;
                            Vec gtargs = e->u.gencall.targs;
                            Vec gargs  = e->u.gencall.args;
                            e->kind = EX_ASSOC;
                            e->u.assoc.typeName  = gname;
                            e->u.assoc.targs     = gtargs;
                            e->u.assoc.name      = "new";
                            e->u.assoc.args      = gargs;
                            e->u.assoc.isCall    = true;
                            e->u.assoc.modPrefix = NULL;
                            return checkExpr(c, e);
                        }
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note, "declare one inside `%s`:"
                                         "\n      fn new(...) -> %s { ... }",
                                  DN(gsd), DN(gsd));
                        bufPrintf(&note, "\nor build it field by field: `%s { ... }`"
                                         " (with its type arguments written out)",
                                  DN(gsd));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` has no constructor `new`, so `%s<...>(...)` has nothing to call",
                                DN(gsd), e->u.gencall.name);
                        return ttError(tt);
                    }
                }
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive or generic function",
                        e->u.gencall.name);
                return ttError(tt);
            }
            if (e->u.gencall.targs.len != 1) {
                ckError(c, e->line, NULL, "`%s` needs exactly one type argument, e.g. `%s<i32>(4)`",
                        e->u.gencall.name, e->u.gencall.name);
                return ttError(tt);
            }
            Type *elem = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, 0),
                                   e->line, c->curParams);
            if (ttIsError(elem)) return ttError(tt);
            /* Write the resolved type back: codegen reads the type arguments of the node, not
             * this local variable. */
            *(Type **)vecAt(&e->u.gencall.targs, 0) = elem;
            if (isPoolPrim) return checkPoolPrim(c, e, elem);
            if (e->u.gencall.args.len != 1) {
                ckError(c, e->line, NULL, "`%s` takes one argument (how many elements)",
                        e->u.gencall.name);
                return ttError(tt);
            }
            Expr *n = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *nt = checkValue(c, n);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, n->line, NULL, "the element count must be an integer, found `%s`",
                        typeStr(c, nt));
                return ttError(tt);
            }

            /* 这块内存活到当前块结束（arena 按块释放），所以它的深度就是那个块的深度：
             * `c->scopes.len`。这里是深度模型与 arena 粒度的接缝，两侧必须用同一个数：检查器比较
             * 块深度来决定引用能否存在这里，生成的 C 用块深度选 `__extc_a[k]`，块一结束就释放。
             * 必须是 `c->scopes.len` 而不是硬编码的 1：否则 `while { p = alloc<i32>(1) }` 里的
             * `p` 会在下一轮指向已释放的内存。于是"返回刚分配的内存"和"把循环内的分配存到循环外"
             * 都会被逃逸检查拦下；要把内存交出去，调用方得提供缓冲或 arena。
             *
             * `alloc` 站点用与 `new` 完全相同的规则定层级：带 home arena 的函数给 `ARENA_HOME`
             * （深度 0，"本帧之外那一层"），否则就是块层级。 */
            if (c->curFunc && anNeedsHome(c->curFunc)) {
                planSetArenaLevel(e, ARENA_HOME);
                anSetRefDepth(e, 0);
            } else {
                anSetRefDepth(e, c->scopes.len);
                planSetArenaLevel(e, (int)c->scopes.len);
            }
            c->allocSites++;
            if (anLexicalLevel(e) == 0) anSetLexicalLevel(e, (int)c->scopes.len);   /* lexical level */
            /* 初值是"还没有约束碰过这个站点"。不能依赖 `arenaAllocZero` 给的 0，因为 0 的意思是
             * "必须活得比本帧久"；`alloc` 也是分配站点，和 `new` 一样需要这行初始化。 */
            anSetMinAt(e, -1);
            /* `alloc` 必须像 EX_NEW 里的 `new` 那样登记进 `c->curArenaSites`：不登记，闭包之后跑的
             * 重写遍历就看不到它，带 home arena 的 `alloc` 会停在块层级、随块释放，于是出现一族误
             * 拒 —— 用 `new` 写的同一个程序能过，用 `alloc` 写的被拒。 */
            *(Expr **)vecPush(&c->curArenaSites) = e;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* freshly allocated storage is writable */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b`：没有值时用后备值。含义是 `match a { some(v) => v  _ => b }`，但只有一侧
             * 会被求值：`a` 有值时 `b` 从不计算 —— 又是 `&&` / `||` 那样的短路。 */
            /* 标志必须在检查主语**之前**读：主语本身是调用时，检查它就会把标志抬起来；之后再读会
             * 把 `var a = h() ?? 0` 这种作为第一条语句的写法误报。 */
            bool priorFx = c->stmtFx != 0;
            Type *mt = checkExpr(c, e->u.coalesce.main);
            if (ttIsError(mt)) { checkExpr(c, e->u.coalesce.fallback); return ttError(tt); }

            bool isOpt = isProtoType(mt, "option", 1);
            bool isRes = isProtoType(mt, "result", 2);
            Type *want = NULL;
            if (isOpt || isRes) {
                want = *(Type **)vecAt(&mt->targs, 0);          /* the payload type T */
            } else if (mt->kind == TY_REF && mt->nullable) {
                want = mt;   /* `?ref T`: the fallback is a reference as well */
            } else {
                /* Position rule: `??` is meaningful only for something that may have no
                 * value. Saying so beats inventing a meaning for it. */
                checkExpr(c, e->u.coalesce.fallback);
                ckError(c, e->line,
                        "`??` means \"if there is nothing, use this instead\", so the left"
                        " side must be an `option`, a `result`, or a `?ref T`",
                        "left of `??` is `%s`, which always has a value", typeStr(c, mt));
                return ttError(tt);
            }

            /* 主语不纯时（如 `f() ?? -1`），普通条件表达式不行：主语会在 C 里出现两次，`f()` 会跑两
             * 次。必须先求值进临时量，而临时量的前缀只能发在整条语句之前 —— 于是它会抢在同一条语句里
             * 更早的副作用前面：
             *     `g() + (h() ?? 0)`   读起来先 g 后 h，但 h 会先跑
             * 悄悄重排比报错让人拆两行更糟：用户读到的顺序必须就是执行的顺序。只有调用算证据；`new`、
             * 字面量、转换的顺序观察不到，不报。 */
            if (!repeatablePure(e->u.coalesce.main)) {
                checkExpr(c, e->u.coalesce.fallback);
                if (priorFx && !c->noHoist) {
                    ckError(c, e->line,
                            "The subject of `??` is computed into a temporary **before the whole"
                            " statement** (that is what makes it run only once). So an earlier"
                            " side effect in the same statement would run *after* it -- the"
                            " opposite of what the line reads like. Split the statement in two"
                            " (`var t = f() ?? 0` then use `t`).",
                            "`??` here would run **before** the call that comes earlier in this"
                            " statement; split the line so the order you read is the order it runs");
                    return ttError(tt);
                }
                if (c->noHoist) {
                    ckError(c, e->line,
                            "`while` re-evaluates its condition every round, but a temporary"
                            " can only be computed once, before the loop. Bind it inside the"
                            " loop body: `while true { let r = f()  if ... { break } }`",
                            "`??` here cannot be evaluated ahead of time -- the temporary"
                            " would run once instead of every round");
                    return ttError(tt);
                }
                planSetNeedTemp(e, true);        /* codegen evaluates it once, then tests the temp */
                /* 主语的所有副作用按源码顺序与其它临时量一起上提到前缀里，所以主语不算"留在原地的
                 * 调用"；后备值仍留在条件表达式里，只在主语没有值时才跑，因此它算。 */
                c->stmtFx = priorFx ? 1 : 0;
                if (exprHasCall(c, e->u.coalesce.fallback)) c->stmtFx = 1;
                return want;
            }

            adoptContextType(e->u.coalesce.fallback, want);
            Type *ft = checkInto(c, want, e->u.coalesce.fallback);
            checkAssignable(c, want, ft, e->u.coalesce.fallback, "the right side of `??`");
            e->type = want;
            return want;
        }

        case EX_SIGN: {
            /* `e!` 是用户的签名：编译器证不出来的，用户背书。背书**在运行期受检**：签错会带源码
             * 位置 trap，而不是未定义行为；唯一签掉这项检查的办法是函数上的 `@unchecked`。三种用法：
             *   `opt!` (`option<T>`)  -> 载荷 T
             *   `r!` (`result<T,E>`) -> 载荷 T
             *   `p!` (`?ref T`)      -> `ref T`，"我知道它不是 null"
             * 其它类型一律报错：签名要写在有意义的地方。 */
            /* This case needs no `priorFx` bookkeeping: codegen emits a helper call around the
             * operand (`unwrapHelper` for `option`/`result`, `extc_refAssert` for `?ref T`), so
             * it is evaluated once, in place, in any expression position -- including a `while`
             * condition, where a statement-prefix temporary cannot live; the operand's own helper
             * call keeps the order. */
            Type *ot = checkExpr(c, e->u.sign.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (isProtoType(ot, "option", 1) || isProtoType(ot, "result", 2)) {
                return *(Type **)vecAt(&ot->targs, 0);      /* the payload type */
            }
            if (ot->kind == TY_REF) {
                if (!ot->nullable) {
                    ckError(c, e->line, "it is already a plain `ref T`, which can never be null",
                            "`%s` is not nullable, so `!` has nothing to assert",
                            typeStr(c, ot));
                    return ttError(tt);
                }
                Type *nn = ttRef(tt, ot->inner);            /* the non-null version */
                nn->mut = ot->mut;
                e->type = nn;
                return nn;
            }
            ckError(c, e->line,
                    "`!` means \"I sign for it\": it turns an `option` / `result` / `?ref T`"
                    " into the value / non-null reference without any check",
                    "`!` needs an `option`, a `result`, or a nullable reference, found `%s`",
                    typeStr(c, ot));
            return ttError(tt);
        }

        case EX_DEREF: {
            /* `*p` is the value, or the place, that `p` points at. Writability comes from the
             * type of `p` (`mut ref`). */
            Type *ot = checkExpr(c, e->u.deref.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (ot->kind != TY_REF) {
                ckError(c, e->line,
                        "`*` means \"the value this reference points at\"; a plain value has"
                        " nothing to dereference",
                        "cannot dereference `%s`, which is not a reference", typeStr(c, ot));
                return ttError(tt);
            }
            if (rejectNullableDeref(c, ot, e->u.deref.operand, "`*`")) return ttError(tt);
            /* `ref void` is C's `void *`: one pointer with no element type, so there is nothing to
             * read. C refuses the same thing, but it refuses it in the generated C, where the error
             * points at a line the author never wrote; the boundary admits `ref void` on purpose
             * (`mmap`, an opaque handle), and this is the other half of that deal. */
            if (ot->inner && ot->inner->kind == TY_VOID) {
                ckError(c, e->line,
                        "A `ref void` is C's `void *` -- a handle extC cannot look inside. Hand it"
                        " back to C, or declare the parameter with the pointee type it really has"
                        " (`ref i64`, `ref point`).",
                        "cannot dereference `%s`: `void` has no size", typeStr(c, ot));
                return ttError(tt);
            }
            return ot->inner;
        }

        case EX_ENUMVAL: {
            /* An instance of a generic enum (`maybe<i64>`) is not in the type table's by-name
             * map: it is produced by instantiation. Constructing it through the EX_ASSOC path
             * therefore records the resolved type on `assocOwner`, which is preferred here. */
            Type *et = planAssocOwner(e) ? planAssocOwner(e) : ttFromName(tt, e->u.enumval.typeName);
            if (!et || et->kind != TY_ENUM || !et->edef) return ttError(tt);
            Variant *v = findVariant(et->edef, e->u.enumval.variant);
            if (!v) return ttError(tt);

            /* A variant without a payload, `status.ok`: it takes no arguments. */
            if (v->types.len == 0) {
                if (e->u.enumval.args.len > 0) {
                    ckError(c, e->line, NULL,
                            "`%s.%s` carries no payload, so it takes no arguments",
                            et->name, v->name);
                    return ttError(tt);
                }
                return et;
            }

            /* Constructing a variant with a payload, `shape.circle(2.0)`: payload values are
             * matched by position. */
            if (e->u.enumval.args.len != v->types.len) {
                ckError(c, e->line, NULL,
                        "`%s.%s` carries %zu value(s), got %zu",
                        et->name, v->name, v->types.len, e->u.enumval.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < v->types.len; i++) {
                Type *pt  = payloadType(tt, et, v, i);
                Expr *arg = *(Expr **)vecAt(&e->u.enumval.args, i);
                Type *at  = checkInto(c, pt, arg);
                checkAssignable(c, pt, at, arg, "payload value");
                /* The payload is stored inside this value, so a reference it carries must not
                 * live for less than the value does. */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }

        case EX_CALL: {
            /* `f(x)` 里的 `f` 持有一个 lambda：节点**原地**改写成方法调用 `f.call(x)`，之后交给
             * 普通的方法路径 —— 与下面变体分支所做的原地改写相同。 */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                if (sy && sy->type && sy->type->kind == TY_STRUCT && sy->type->sdef
                    && anLamSig(sy->type->sdef)) {
                    Expr *recv = e->u.call.callee;      /* read before `u.method` overwrites them */
                    Vec   args = e->u.call.args;
                    e->kind = EX_METHOD;
                    e->u.method.recv = recv;
                    e->u.method.name = "call";
                    e->u.method.args = args;
                    return checkExprInner(c, e);
                }
            }
            /* `f(x)` 里的 `f` **持有函数类型的值**，而不是命名一个声明：调用经由代码指针，签名来自
             * 类型。这一支放在下面所有按名字的分支之前，因为它们说的都是一个这次调用没有的声明。 */
            if (e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
                Sym *sy = lookup(c, e->u.call.callee->u.ident.name);
                Type *ct = (sy && sy->type) ? tsub(c, sy->type) : NULL;
                if (ct && ct->kind == TY_FN) return checkCallThroughFn(c, e, ct);
            }
            /* `o.f(x)` where `f` is a **field** holding a `fn`, not a method. Without this the
             * method path answers "no method `f` on `op`" -- a sentence about the wrong thing, on a
             * program that has the field. The field's type is peeked at rather than checked, so the
             * ordinary method path (which checks the receiver itself) is untouched. */
            if (e->u.call.callee && e->u.call.callee->kind == EX_FIELD) {
                Type *ft2 = peekFieldType(c, e->u.call.callee);
                if (ft2 && ft2->kind == TY_FN) return checkCallThroughFn(c, e, ft2);
            }
            /* Constructing a variant with a payload, `shape.circle(2.0)`: it looks like a field
             * access followed by a call, but `shape` is a type name and not a variable, so it is
             * recognised as an enum construction and rewritten to EX_ENUMVAL. */
            if (e->u.call.callee->kind == EX_FIELD) {
                Expr *fld = e->u.call.callee;
                if (fld->u.field.obj->kind == EX_IDENT && !lookup(c, fld->u.field.obj->u.ident.name)) {
                    Type *et = ttFromName(tt, fld->u.field.obj->u.ident.name);
                    if (et && et->kind == TY_ENUM && et->edef) {
                        Variant *v = findVariant(et->edef, fld->u.field.name);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, fld->u.field.name);
                            return ttError(tt);
                        }
                        Vec args = e->u.call.args;      /* union: copy it out first */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->u.enumval.args     = args;
                        planSetAssocOwner(e, et);
                        return checkExprInner(c, e);    /* the rest is EX_ENUMVAL's job */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            /* `flush()` pushes out the buffer that `println` writes through.
             *
             * It is needed because `std::io`'s `writeBytes` writes straight to the file
             * descriptor with `write(2)` and no buffering, while `println` goes through printf,
             * which buffers. Mixing the two loses the order: the prompt has not been printed yet
             * and the program is already waiting for input. */
            if (strcmp(name, "flush") == 0) {
                if (e->u.call.args.len != 0) {
                    ckError(c, e->line, "`flush()` takes no arguments.",
                            "`flush` takes no arguments");
                }
                return ttVoid(tt);
            }

            /* 文件是程序自己的事，编译器不维护描述符表 —— `std::fs` 用声明为 `extern!` 的 `close(2)`
             * 关闭，句柄的 `open` 标志让第二次 close 成为空操作；编译器能做的是在能证明的时候*证明*
             * 泄漏。 */
            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                /* `print` / `println` 是按名字派发的内建，经由 C 的 stdio 写 —— 那是 `io::cout` 无法
                 * 与之排序的第二个缓冲。`io::cout` 是唯一的控制台路径：它是 `fd` 上的流，以后能长成
                 * 文件、管道或缓冲，内建名字则不能。这是警告而不是错误：存量调用点还很多，提示就是
                 * 迁移办法：`println(a, b)` 变成 `io::cout << a << b << "\n"`。 */
                ckWarn(c, e->line,
                       "`io::cout` is the one console path -- `use std::io`, then"
                       " `io::cout << x` (`println(x)` is `io::cout << x << \"\\n\"`)",
                       "`%s` is deprecated", name);
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkPrintArg(c, a); /* takes a value, so it dereferences */
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            /* A diagnostic has to echo the name as written in the source (`open`), not the
             * mangled `lib$open`. Without `srcName` the message would read "write
             * `lib::lib$open`", which is nonsense. */
            const char *shownName = e->u.call.callee->u.ident.srcName
                                    ? e->u.call.callee->u.ident.srcName : name;
            FuncDef *f = findFunc(c, name);
            /* 另一个模块的函数写成了不带限定的形式：`lib.extc` 里有 `open`，这里写的是 `open()`。
             *
             * 不含 `$` 的名字是源码级名字（加载器只把限定名改写成扁平名），这时该说的是"这里需要
             * 限定名"，而不是"没有这个函数"。判据就是名字里有没有 `$`：该字符在 extC 标识符里不
             * 合法，所以它出现就说明名字被加载器改写过，也只有那时函数才是真的缺失。 */
            if (!f && name && !strchr(name, '$') && !anQualified(e)) {
                for (size_t i = 0; i < c->m->funcs.len; i++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&c->m->funcs, i);
                    const char *d = cand->name ? strchr(cand->name, '$') : NULL;
                    if (d && cand->modName && !cand->isPrivate && strcmp(d + 1, shownName) == 0) {
                        requireQualified(c, shownName, cand->modName, false, e->line);
                        return ttError(tt);
                    }
                }
            }
            if (f && !f->reserved && f->modName && !anQualified(e))
                requireQualified(c, shownName, f->modName, false, e->line);
            if (!f) {
                /* 裸的枚举构造式。错误必须指路，而不是只说"没有这个函数"：写完
                 * `type shape = | circle(f64) | ...` 后写 `circle(2.0)`，要报 `circle` 是 `shape` 的
                 * 变体并指向 `shape.circle(...)`，而不是一句没给出正确拼写的"内建只有 print/println"。
                 * 这里不做类型推断：把类型完整写出来是刻意的，消息只是陈述正确拼写，不替用户填。
                 * 只有带载荷的变体才会被这样指向；无载荷的 `status.ok` 本身就是一个值，走另一条路。 */
                /* `ifstream("input.txt")`：名字是一个**类型**的调用就是该类型的构造函数，即关联函数
                 * `new`。它被改写成关联调用节点，于是实参检查、arena 实参与生成的 C 与
                 * `ifstream::new("input.txt")` 完全相同，没有第二份实现可以漂移（the constructor's
                 * one path）。
                 *
                 * 仅当该名字没有被局部绑定遮蔽时：否则被类型名遮蔽的变量会被报成缺少构造函数，那是对
                 * 一个没有的问题说错话。 */
                if (!lookup(c, name)) {
                    Type *ct = ttFromName(tt, name);
                    if (ct && !ttIsError(ct)) {
                        StructDef *csd = structOf(ct);
                        FuncDef *ctor = NULL;
                        if (csd) {
                            for (size_t i = 0; i < csd->methods.len && !ctor; i++) {
                                FuncDef *m = *(FuncDef **)vecAt(&csd->methods, i);
                                if (anIsAssoc(m) && strcmp(m->name, "new") == 0) ctor = m;
                            }
                        }
                        if (ctor) {
                            Vec cargs = e->u.call.args;      /* union: copy it out first */
                            e->kind = EX_ASSOC;
                            e->u.assoc.typeName  = name;
                            e->u.assoc.name      = "new";
                            e->u.assoc.args      = cargs;
                            e->u.assoc.isCall    = true;
                            e->u.assoc.modPrefix = NULL;
                            /* 旧 kind 用过的字段仍在 union 里，所以每个都要写：漏写 `targs` 会让节点
                             * 带着这次调用的遗留值，解析器于是报"`named` 期望 0 个类型实参、得到 1 个"
                             * —— 一个谁都没写过类型实参的类型（the node's leftovers）。 */
                            vecInit(&e->u.assoc.targs, c->arena, sizeof(void *));
                            return checkExprInner(c, e);
                        }
                        if (csd) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "declare one inside `%s`:", DN(csd));
                            bufPrintf(&note, "\n      fn new(...) -> %s { ... }", DN(csd));
                            bufPrintf(&note, "\nor build it field by field: `%s { ... }`",
                                      DN(csd));
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no constructor `new`, so `%s(...)` has nothing to call",
                                    DN(csd), name);
                            return ttError(tt);
                        }
                    }
                }
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, name)) owner = td;
                }
                if (owner) {
                    /* The `note` argument of `ckError` is passed through verbatim: unlike the
                     * format it takes no varargs, so a value has to be rendered with
                     * `arenaPrintf` first. Passing a raw `%s` printed the literal text. */
                    const char *note = arenaPrintf(c->arena,
                            "A payload variant is named by its type -- write `%s.%s(...)`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a function -- did you mean `%s.%s(...)`?",
                            name, DN(owner), DN(owner), name);
                    return ttError(tt);
                }
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
                    e->type = f->ret;
                    return f->ret;
                }
                if (isViewOfDecl(f)) {
                    /* `extc_viewOf(p, n)`: no runtime check of any kind -- whoever calls it owns the
                     * pointer (the plate checks it with `holds`, the boundary signs for it in a
                     * declaration). The shapes are still checked: a byte pointer and a length. */
                    if (e->u.call.args.len != 2) {
                        ckError(c, e->line, NULL, "`extc_viewOf` takes 2 arguments, got %zu",
                                e->u.call.args.len);
                        return ttError(tt);
                    }
                    Expr *ap = *(Expr **)vecAt(&e->u.call.args, 0);
                    Expr *an = *(Expr **)vecAt(&e->u.call.args, 1);
                    Type *tp = checkExpr(c, ap);   /* the reference **is** the value here */
                    Type *tn = checkExpr(c, an);
                    if (!ttIsError(tp) && !(tp->kind == TY_REF &&
                                            ttBase(tp->inner) && ttBase(tp->inner)->kind == TY_BUILTIN))
                        ckError(c, ap->line,
                                "It re-types a pointer without checking anything, so it insists on"
                                " the one shape it can mean: `ref u8`.",
                                "`extc_viewOf` wants a `ref u8`, found `%s`", typeStr(c, tp));
                    if (!ttIsError(tn) && !ttIsInteger(tn))
                        ckError(c, an->line, NULL,
                                "`extc_viewOf` wants an integer length, found `%s`", typeStr(c, tn));
                    planSetViewOf(e, true);              /* codegen 认这个标记（声明没有函数体，没 func 可认） */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                /* `vecAt` 不做边界检查：零实参的 `parallel::run()` 会解引用空向量的 NULL 基址，把
                 * 编译器带崩。这里先报形状；有一个实参时已经会走到诊断路径，零个不会。 */
                if (e->u.call.args.len == 0) {
                    ckError(c, e->line,
                            "`parallel::run(worker, views..., n)` takes the worker function's"
                            " **name** first (v1 has no first-class function values).",
                            "`parallel::run` needs at least the worker function's name");
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区）。 */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);
                planSetParTlsArena(wf, true);
                planSetUsed(wf, true);
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);

            /* A generic free function: `T` can be inferred from an argument only, because an
             * instance of a type is decided by the type while a call to a free function has
             * nothing but its arguments. So the arguments are checked first, then `T` is
             * inferred, then the instance is created. When inference fails because `T` appears
             * only in the return type, the message tells the user to write the arguments
             * explicitly: `f<i32>(...)`. */
            if (f->typeParams.len > 0) {
                if (e->u.call.args.len != f->params.len) {
                    ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                            name, f->params.len, e->u.call.args.len);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                Vec targs;
                vecInit(&targs, c->arena, sizeof(void *));
                Vec *written = NULL;
                for (size_t k = 0; k < c->explicitTargs.len && !written; k++) {
                    ExplicitTargs *cand = *(ExplicitTargs **)vecAt(&c->explicitTargs, k);
                    if (cand->node == e) written = &cand->targs;
                }
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    /* Seeded with the written `<...>`; the arguments still have to agree with it
                     * (unification reports a mismatch). `NULL` means "infer this one". */
                    Type *ex = (written && i < written->len)
                                   ? *(Type **)vecAt(written, i) : NULL;
                    *(Type **)vecPush(&targs) = ex;
                }
                for (size_t i = 0; i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i);
                    Expr  *a = *(Expr **)vecAt(&e->u.call.args, i);
                    adoptContextType(a, p->type);
                    Type *at = checkExpr(c, a);
                    if (ttIsError(at)) return ttError(tt);
                    /* A `ref T` parameter: when the argument is written `ref x`, `at` is a
                     * reference, so the pointee types are unified. */
                    Type *want = p->type;
                    if (want->kind == TY_REF && at->kind == TY_REF) { want = want->inner; at = at->inner; }
                    if (!unifyTParams(c->tt, &f->typeParams, &targs, want, at)) {
                        ckError(c, e->line,
                                "A generic function's type parameters are inferred from its arguments."
                                " Write them out at the call to supply one that no argument mentions:"
                                " `f<i32>(...)` seeds the inference.",
                                "cannot infer type parameter(s) of `%s` from the arguments", name);
                        return f->ret ? f->ret : ttVoid(tt);
                    }
                }
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    if (*(Type **)vecAt(&targs, i)) continue;
                    ckError(c, e->line,
                            "A generic function's type parameters are inferred from its arguments."
                            " Write them out at the call to supply one that no argument mentions:"
                            " `f<i32>(...)` seeds the inference.",
                            "cannot infer type parameter `%s` of `%s`",
                            *(const char **)vecAt(&f->typeParams, i), name);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                /* Calling a generic function from inside a generic body: the type arguments
                 * still mention `T`, so no correct instance can be built at this moment. That is
                 * not an error either.
                 *
                 * The technique matches `RefCheck` and `OpCheck`: while the template is checked,
                 * an instance whose arguments are the parameters themselves is created
                 * (`idOf_T`), which is self-consistent and treats `T` as opaque, so the rest of
                 * the template body still type-checks. The call is recorded, and when the
                 * instance is checked again, the callee is redirected to the concrete instance
                 * (`idOf_i32`). */
                FuncDef *inst = funcInstance(c, f, &targs, e->line);
                /* 与上面 EX_GENCALL 处的 NULL 契约相同：错误已记录，直接停。 */
                if (!inst) return ttError(tt);
                planSetUsed(inst, true);
                planSetCallee(e, inst);
                f = inst;                     /* every check below uses the instance */

                bool hasParamTarg = false;
                for (size_t i = 0; i < targs.len; i++) {
                    Type *a = *(Type **)vecAt(&targs, i);
                    if (a && ttHasParam(a)) { hasParamTarg = true; break; }
                }
                if (hasParamTarg && c->curFunc) {
                    CallCheck *cc = (CallCheck *)arenaAllocZero(c->arena, sizeof(CallCheck));
                    FuncDef *tmplOfF = planTemplate(f);
                    cc->node = e;
                    cc->tmpl = tmplOfF ? tmplOfF : f;   /* the template, not the instance */
                    cc->func = c->curFunc;   /* which template body this belongs to */
                    vecInit(&cc->targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < targs.len; i++)
                        *(Type **)vecPush(&cc->targs) = *(Type **)vecAt(&targs, i);
                    *(CallCheck **)vecPush(&c->callChecks) = cc;
                }
            }

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            /* The arena for this call comes from the shallowest `mut ref` argument. */
            anSetHomeDepth(e, callHomeDepth(c, &e->u.call.args, &f->params, e));
            setCallArenaArg(c, e);      /* decide which arena the call finally passes */
            setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
            /* The reference check has to run after the arguments are checked: until then
             * `a->type` is not filled in, `typeContainsRef` answers "no" for everything, and a
             * violation is silently accepted. The method path hit the same trap. The check itself
             * is at the end of this case. */
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkInto(c, p->type, a);

                /* When the parameter is a `ref T`, the call site has to spell `ref` out, so
                 * that a reader sees at a glance that a reference is passed and not a copy. An
                 * argument that already is a reference can be passed as it stands. */
                if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, p->type));
                    continue;
                }
                /* `coroutine<T>` 是统一的句柄，一个普通的 24 字节值。 */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, p->type, at, a, "argument");
                }
            }
            /* Every call site is checked, not only the ones with a home arena: what matters is
             * that the callee may store into a container the argument points at, which has
             * nothing to do with the home arena.
             * The position matters as well: after the arguments are checked, because
             * `targetDepth` reads their types. */
            checkCallRefArgs(c, f, &e->u.call.args, &f->params, anHomeDepth(e), e->line, name);
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_EXT: {
            /* `ext` 把一个任务交给一个**域**，而域是库给出的词法块（`sched::run { … }`、
             * `par::pool(n) { … }`）。域外没有接收者，所以这是编译错误 —— 设计要点就是它在这里
             * 失败，而不是在运行期失败。 */
            if (c->domainDepth == 0) {
                ckError(c, e->line,
                        "A domain is a lexical block that says where the concurrency runs and when"
                        " it must all be finished; `ext` hands the task to the nearest one.",
                        "there is no domain here: `ext` is only legal inside `sched::run { … }` or"
                        " `par::pool(n) { … }`");
                return ttError(tt);
            }
            /* Inside a domain: the operand must be a call to a function that **can suspend**, i.e. a
             * coroutine. A plain function has no frame to stop in, so "starting" it would silently
             * be an ordinary call -- exactly the kind of quiet difference this design refuses. */
            Expr *in = e->u.ext_.call;
            if (!in || in->kind != EX_CALL) {
                ckError(c, e->line,
                        "`ext` starts one task, and a task is a call: it needs arguments and a frame"
                        " to hold its progress.",
                        "`ext` takes a call, found something else");
                return ttError(tt);
            }
            Type *inner = checkExpr(c, in);
            (void)inner;
            if (!planCallee(in) || !planIsCoro(planCallee(in))) {
                ckError(c, in->line,
                        "A task must be able to **stop in the middle**: only a coroutine has a frame to"
                        " hold where it stopped and what its locals were. A plain function would just"
                        " run to completion right here, so `ext` on one would be a silent ordinary"
                        " call.",
                        "`ext` needs a coroutine function, and `%s` is a plain function",
                        planCallee(in) ? planCallee(in)->name : "this");
                return ttError(tt);
            }
            planSetExtDom(e, c->curDom);          /* codegen 把任务登记到这个域上 */
            planSetIsExtTarget(planCallee(in), true);   /* ⇒ 为它生成域用的适配器（只为它，别的产物不受影响） */
            /* 值**故意是 void**：`let h = ext f(x)` 由类型系统拒绝，而不是靠一条特殊规则；句柄随驱动
             * 循环一起交付。 */
            e->type = ttVoid(tt);
            return e->type;
        }
        case EX_LAMBDA: return checkLambda(c, e);
        case EX_DYN: {
            /* `dyn Trait(x)` as a **value**: its type is `dyn Trait`, and the payload must
             * implement the trait -- the implementation is what the table points at. The payload's
             * type is kept on the node: codegen needs its C spelling for the temporary and for
             * `sizeof`, and by then this node's own type is `dyn Trait`. */
            Type *payT = checkExprInner(c, e->u.dynv.payload);
            /* 载荷节点也需要类型：codegen 由它发出临时量，没有类型的节点会变成 `(int){0}`。 */
            if (payT) e->u.dynv.payload->type = payT;
            e->u.dynv.payloadType = payT;
            /* `dyn` 值由池支撑，所以所在函数要标记为建池 —— 与派发点用的是同一个已有标志，这里也
             * 必须置上：少了它池运行期不会被发出，生成的 C 会调用未声明的 `extc_dyn_put`。 */
            if (c->curFunc) planSetMakesPool(c->curFunc, true);
            dynTraitOf(c, e->u.dynv.traitName, payT, e->line);
            Type *dt = typeNamed(c->arena, e->u.dynv.traitName);
            dt->kind = TY_DYN;
            e->type = dt;
            return dt;
        }

        case EX_METHOD: {
            /* `o.f(x)` where `f` is a **field** holding a `fn`, not a method. The parser reads the
             * shape `receiver.name(args)` as a method call (only a bare `f(x)` becomes EX_CALL), so
             * the field case has to be recognised here -- otherwise the answer is "no method `f` on
             * `op`", a sentence about the wrong thing, on a program that has the field.
             *
             * The field's type is peeked at, never checked: if this turns out to be an ordinary
             * method call (the overwhelmingly common case) the receiver's own checks must run once,
             * and they run on the path below. The node is then rewritten in place into a call
             * through the value, which is what `o.f(x)` means. */
            if (e->u.method.recv->kind == EX_IDENT && lookup(c, e->u.method.recv->u.ident.name)) {
                /* callee 节点必须是 arena 上**真实**的字段访问，而不是栈上的临时量：它会成为改写后
                 * 调用的 callee，并被 codegen 打印成 `(o.f)(x)`；把调用指向*接收者*会打印出 `o`。 */
                Expr *fldE = exprNew(c->arena, EX_FIELD, e->line);
                fldE->u.field.obj  = e->u.method.recv;
                fldE->u.field.name = e->u.method.name;
                Type *ft2 = peekFieldType(c, fldE);
                if (ft2 && ft2->kind == TY_FN) {
                    Vec args = e->u.method.args;
                    e->kind = EX_CALL;
                    e->u.call.callee = fldE;
                    e->u.call.args   = args;
                    return checkCallThroughFn(c, e, ft2);
                }
            }

            /* Constructing a variant with a payload can look like this as well: the syntax of
             * `shape.circle(2.0)` is that of a method call, `receiver.name(args)`, and the only
             * difference is that `shape` is a type name rather than a variable. So it is
             * recognised here first, exactly as `status.ok` is recognised on the EX_FIELD path;
             * the receiver's name decides. */
            if (e->u.method.recv->kind == EX_IDENT && !lookup(c, e->u.method.recv->u.ident.name)) {
                Type *et = ttFromName(tt, e->u.method.recv->u.ident.name);
                if (et && et->kind == TY_ENUM && et->edef) {
                    Variant *v = findVariant(et->edef, e->u.method.name);
                    if (!v) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note, "variants of %s:", et->name);
                        for (size_t i = 0; i < et->edef->variants.len; i++)
                            bufPrintf(&note, " %s",
                                      (*(Variant **)vecAt(&et->edef->variants, i))->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` has no variant `%s`", et->name, e->u.method.name);
                        return ttError(tt);
                    }
                    Vec args = e->u.method.args;
                    e->kind = EX_ENUMVAL;
                    e->u.enumval.typeName = et->name;
                    e->u.enumval.variant  = v->name;
                    e->u.enumval.args     = args;
                    planSetAssocOwner(e, et);
                    return checkExprInner(c, e);
                }
            }

            /* Methods live inside a `struct` body only, so they are found by the type of the
             * receiver. */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            if (f && f->isPrivate) {
                const char *home = c->curFunc ? c->curFunc->modName : NULL;
                if (f->modName != home) {
                    ckError(c, e->line,
                            "`@private` members are reachable only from the module that declares"
                            " them; if this is meant to be public, drop the annotation.",
                            "`%s` is private to module `%s`", e->u.method.name,
                            f->modName ? f->modName : "this file");
                    return ttError(tt);
                }
            }
            if (!f) {
                /* `d.m(...)` 里的 `d` 是 `dyn Trait` 值：方法住在**trait 的**声明里，不在任何类型的
                 * 方法集里 —— 具体类型要到运行期才知道。放在这里（接收者已检查之后），所以每种接收者
                 * 形状都成立（`d`、`h.d`…），接收者节点也已经带着类型供 codegen 用；不检查接收者就
                 * 读它的类型会把这一趟的次序搞乱。 */
                if (ttBase(recvT) && ttBase(recvT)->kind == TY_DYN) {
                    /* The receiver may be reached through a reference (`r.tag()` with
                     * `r: ref dyn Tag`). `ttBase` unwraps it here, so record the fact for codegen:
                     * it has to dereference the C expression when it hands the handle to
                     * `extc_dyn_slot`. */
                    planSetDynRecvViaRef(e, (recvT && recvT->kind == TY_REF));
                    const char *traitName = ttBase(recvT)->name;
                    TraitDef *tr = NULL;
                    for (size_t i = 0; i < c->m->traits.len && !tr; i++) {
                        TraitDef *cand = *(TraitDef **)vecAt(&c->m->traits, i);
                        if (cand->name && strcmp(cand->name, traitName) == 0) tr = cand;
                    }
                    FuncDef *want = NULL;
                    for (size_t i = 0; tr && i < tr->methods.len && !want; i++) {
                        FuncDef *cand = *(FuncDef **)vecAt(&tr->methods, i);
                        if (strcmp(cand->name, e->u.method.name) == 0) want = cand;
                    }
                    /* 对象安全性：`dyn` 调用经由 trait 的统一表，表把接收者交给 thunk，所以**没有
                     * 接收者**的方法没有槽位可坐。`want->params.len == 0` 在这里就拒绝，与返回
                     * `Self` 的情形同一条规则；这样诊断能点名 trait 方法，而不是把调用留给围绕
                     * `params[0]` 接收者写成的代码生成（the table's slot decides it）。 */
                    if (want && want->params.len == 0) {
                        ckError(c, e->line,
                                "A `dyn` value dispatches through the trait's uniform table, and that"
                                " table hands a receiver to every entry. A method with no receiver has"
                                " no place in it -- make it a free function, or give it a receiver.",
                                "`%s` has no receiver, so it cannot be called through `dyn`",
                                e->u.method.name);
                        return ttError(tt);
                    }
                    if (!want) {
                        ckError(c, e->line,
                                "A `dyn` value dispatches through the trait it names, so the method"
                                " has to be one the trait declares.",
                                "trait `%s` has no method `%s`", traitName, e->u.method.name);
                        return ttError(tt);
                    }
                    /* Arguments come against the trait's declaration; a parameter mentioning `Self`
                     * is skipped -- the implementing type is deliberately unknown here, and object
                     * safety keeps such methods out of the table. */
                    size_t wantArgs = want->params.len > 0 ? want->params.len - 1 : 0;
                    if (e->u.method.args.len != wantArgs) {
                        ckError(c, e->line,
                                "The trait's declaration says how many arguments the method takes.",
                                "`%s` takes %zu argument(s), but %zu were given",
                                e->u.method.name, wantArgs, e->u.method.args.len);
                        return ttError(tt);
                    }
                    for (size_t i = 0; i < e->u.method.args.len; i++) {
                        Expr *a = *(Expr **)vecAt(&e->u.method.args, i);
                        Type *at = checkExprInner(c, a);
                        Param *wp = *(Param **)vecAt(&want->params, i + 1);
                        if (!mentionsParam(wp->type) && !ttEquals(ttBase(wp->type), ttBase(at)))
                            ckError(c, a->line,
                                    "Argument types must match the trait's declaration.",
                                    "argument %zu expects `%s`, found `%s`", i + 1,
                                    typeStr(c, ttBase(wp->type)), typeStr(c, ttBase(at)));
                    }
                    if (c->curFunc) planSetMakesPool(c->curFunc, true);
                    e->u.method.recv->type = ttBase(recvT);
                    planSetCallee(e, want);
                    e->dynTrait = traitName;
                    e->type = want->ret ? want->ret : ttVoid(tt);
                    return e->type;
                }
                /* 类型参数上的方法调用：模板里 `T` 是不透明的，这个方法到底指谁是逐实例才知道的。
                 * 记下来并交回错误类型（错误类型的存在就是为了不让错误级联，`checkAssignable` 会放
                 * 它过去），由实例那一趟决定并报告。没有它，泛型容器就完全没法用元素类型的值做任何
                 * 事 —— `hashMap<K, V>` 正是卡在这里：哈希一个 `K` 就是在类型参数上调用 `hash()`。 */
                if (c->curFunc && mentionsParam(recvT)) {
                    MethodCheck *mc = (MethodCheck *)arenaAllocZero(c->arena, sizeof(MethodCheck));
                    mc->node  = e;
                    mc->owner = c->curFunc->owner;
                    mc->name  = e->u.method.name;
                    mc->nargs = e->u.method.args.len;
                    mc->func  = c->curFunc;
                    *(MethodCheck **)vecPush(&c->methodChecks) = mc;
                    return ttError(tt);
                }
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                /* An `impl` block in a module that was not imported is the usual reason a
                 * method is "missing": say which module declares it (stdlib/INDEX). */
                const char *provider = modulesMethodHint(e->u.method.name);
                if (sd) {
                    bufPrintf(&note, "methods of %s:", DN(sd));
                    if (sd->methods.len == 0) bufPuts(&note, " (none)");
                    for (size_t i = 0; i < sd->methods.len; i++)
                        bufPrintf(&note, " %s",
                                  (*(FuncDef **)vecAt(&sd->methods, i))->name);
                    bufPuts(&note, "; declare it inside its `struct`, or attach it with an"
                                  " `impl` block");
                } else {
                    bufPuts(&note, "only struct values and builtin scalars have methods");
                }
                if (provider)
                    bufPrintf(&note, "; method `%s` is declared by module `%s` -- add `use %s`",
                              e->u.method.name, provider, provider);
                ckError(c, e->line, bufCstr(&note), "no method `%s` on `%s`",
                        e->u.method.name, typeStr(c, rb ? rb : recvT));
                return ttError(tt);
            }
            if (f->isBuiltin) {
                if (isDomSingleDecl(f)) {
                    planSetDomNew(e, true);          /* codegen 认这个标记，不认 e->func（那里是 NULL） */
                    e->type = f->ret;
                    return f->ret;
                }
                if (!isParRunDecl(f)) {
                    ckError(c, e->line,
                            "The declaration carries `@builtin`, so the code has to come from the"
                            " compiler, and there is no implementation for this one yet.",
                            "`%s` is a builtin that is not implemented yet", f->name);
                    return ttError(tt);
                }
                Expr *wa = *(Expr **)vecAt(&e->u.call.args, 0);
                FuncDef *wf = (wa->kind == EX_IDENT) ? findFunc(c, wa->u.ident.name) : NULL;
                if (!wf || !wf->body) {
                    ckError(c, e->line,
                            "v1 has no first-class function values, so the worker is written as a name, and"
                            " the compiler generates the trampoline that puts it on a thread.",
                            "the first argument of `parallel::run` must be the name of a function");
                    return ttError(tt);
                }
                /* 签名：`(id: i64, lo: i64, hi: i64, 视图们…)`，其中**恰好一个** `mut` 视图是输出
                 * （按 [lo,hi) 分区），其余是**只读**视图（每个 worker 拿到同一份，不分区）。 */
                {
                    bool sigOk = wf->params.len >= 4;
                    size_t mutCount = 0;
                    for (size_t i = 0; sigOk && i < wf->params.len; i++) {
                        Type *pt = (*(Param **)vecAt(&wf->params, i))->type;
                        if (i < 3) { if (!isI64Type(pt)) sigOk = false; }
                        else if (!ttIsViewType(pt)) sigOk = false;
                        else if (pt->mut) mutCount++;
                    }
                    if (!sigOk || mutCount != 1) {
                        ckError(c, e->line,
                                "A worker is handed the range [lo,hi), its own slice of the output, and any"
                                " number of read-only views shared by every worker -- so the signature is"
                                " `(id, lo, hi, <read-only views>..., out: mut slice<T>)`.",
                                "worker `%s` must take `(id: i64, lo: i64, hi: i64, [slice<T>...],"
                                " out: mut slice<T>)` with exactly one `mut` view", wf->name);
                        return ttError(tt);
                    }
                }
                size_t nViews = wf->params.len - 3;
                if (e->u.call.args.len != nViews + 3) {
                    ckError(c, e->line, NULL,
                            "`parallel::run` takes `(worker, <view>..., n, threads)`: worker `%s` has %d view"
                            " parameter(s), so %d arguments, got %d",
                            wf->name, (int)nViews, (int)(nViews + 3), (int)e->u.call.args.len);
                    return ttError(tt);
                }
                {
                    const char *why = parBodyProblem(c, wf, 0);
                    if (why) {
                        ckError(c, e->line,
                                "A worker runs on another thread while the caller keeps running, so step 3b"
                                " allows only operators, literals, indexing, its own parameters and locals.",
                                "worker `%s` cannot be used here: %s", wf->name, why);
                        return ttError(tt);
                    }
                }
                for (size_t i = 0; i < nViews; i++) {
                    Type *pt = (*(Param **)vecAt(&wf->params, 3 + i))->type;
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, 1 + i);
                    Type *at = checkExpr(c, a);
                    bool ok = ttEquals(pt, at)
                           || (!pt->mut && ttEquals(pt, ttViewReadonly(c->tt, at)));
                    if (!ok) {
                        ckError(c, a->line,
                                "Only the view a worker declares `mut` is partitioned; the others are shared"
                                " read-only, so their element types must match the parameters.",
                                "worker `%s` view parameter %d is `%s`, but this argument is `%s`",
                                wf->name, (int)(i + 1), typeStr(c, pt), typeStr(c, at));
                        return ttError(tt);
                    }
                }
                {
                    Type *tn = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 1 + nViews));
                    Type *tx = checkExpr(c, *(Expr **)vecAt(&e->u.call.args, 2 + nViews));
                    if (!isI64Type(tn) || !isI64Type(tx)) {
                        ckError(c, e->line, NULL,
                                "`parallel::run` takes `(worker, <view>..., n, threads)`: `n` and `threads`"
                                " are `i64`");
                        return ttError(tt);
                    }
                }
                planSetParWorker(e, wf);
                planSetIsParWorker(wf, true);
                planSetParTlsArena(wf, true);
                planSetUsed(wf, true);
                {
                    Expr *lit = exprNew(c->arena, EX_INT, wa->line);
                    lit->u.ival = 0;
                    *(Expr **)vecAt(&e->u.call.args, 0) = lit;
                }
                e->type = c->tI32;
                return c->tI32;
            }
            planSetCallee(e, f);  planSetUsed(f, true);
            /* `dyn Trait(x).m(...)`：**对象安全**。
             *
             * 不是每个方法都能经由表派发，规则在 `dyn` 的**使用点**执行，而不是在 trait 声明上：
             * 这样的方法作为静态调用完全没问题，它只是没有槽位坐。判断针对的是**trait 的**签名，
             * 所以 trait 按名字从模块声明里查（与 `impl` 归属用的是同一次查找）。 */
            if (e->dynTrait) {
                /* `dyn` 值由池支撑，所以所在函数要建池。用的是**已有的**标志：codegen 据此为函数体
                 * 进入一个地方并发出池运行期；决策必须在检查器里做出，因为"发出 `zoneEnter`"是在生成
                 * 函数体之前定的，函数体生成中途再置标志已经太晚。 */
                if (c->curFunc) planSetMakesPool(c->curFunc, true);
                TraitDef *tr = dynTraitOf(c, e->dynTrait, e->u.method.recv->type, e->line);
                FuncDef *want = NULL;
                for (size_t ti = 0; tr && ti < tr->methods.len && !want; ti++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&tr->methods, ti);
                    if (strcmp(cand->name, e->u.method.name) == 0) want = cand;
                }
                /* Object safety. These three are FATAL on purpose: a `dyn` call dispatches
                 * through a uniform table whose every entry receives a `self`, so a trait method
                 * with no receiver (an empty `f->params`) has no slot to sit in. */
                bool objSafeBad = false;
                if (want && !funcIsMethod(want)) {
                    ckError(c, e->line,
                            "A table holds method pointers, so the trait method it names must take"
                            " a receiver; an associated function is still callable statically.",
                            "`%s::%s` has no `self`, so it cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                } else if (want && want->typeParams.len > 1) {
                    ckError(c, e->line,
                            "A generic method has one implementation per instantiation, so there is"
                            " no single slot for it in a table.",
                            "`%s::%s` is generic and cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                } else if (want && want->ret && mentionsParam(want->ret)) {
                    ckError(c, e->line,
                            "A method returning `Self` would have to name the implementing type,"
                            " which a table slot cannot do.",
                            "`%s::%s` returns `Self` and cannot be dispatched through `dyn`",
                            e->dynTrait, e->u.method.name);
                    objSafeBad = true;
                }
                if (objSafeBad) return ttError(tt);
            }

            /* A method that takes `self: mut ref T` needs a writable receiver. This is the other
             * half of making signatures tell the truth: `x.bump()` alone does not show whether
             * `x` will be modified, `self: mut ref` in the signature does, and this is where that
             * promise is enforced. */
            if (f->params.len > 0) {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type->kind == TY_REF && selfP->type->mut &&
                    requireMutable(c, e->u.method.recv, e->line, "call a method that writes"))
                    return ttError(tt);
            }

            /* The receiver is the shallowest `mut ref` argument, since `self` comes first, so
             * what the callee allocates follows the arena the receiver lives in. */
            if (f->params.len > 0) {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type && selfP->type->kind == TY_REF && selfP->type->mut) {
                    int d = slotDepth(c, e->u.method.recv);
                    /* `self: mut ref` 这一支不走 `callHomeDepth`，所以逃逸判定也要在这里做。 */
                    const char *rrn = placeRootName(e->u.method.recv);
                    if (d != 0 && rrn && isEscapeeName(c, rrn)) d = -1;
                    anSetHomeDepth(e, (d == 0) ? -1 : d);
                    /* This receiver branch depends on the escape information as well, and that
                     * information is not final while the body is being checked: it is complete
                     * only once the closure of the call graph is closed. So the site is recorded
                     * and recomputed at the end.
                     * The shape that needs it:
                     *     fn fill(out) { var l: list  l.pushOne(7)  out.take(l) }
                     * Here `l` is published by a method call afterwards, so the receiver has to be
                     * treated as escaping. */
                    /* Recorded for a receiver at any depth, `d == 0` included: the arena is
                     * settled then, but the **zone** is not (it waits for the `makesPool`
                     * closure), and the final pass needs the site to promote it. */
                    if (rrn && c->eSites.arena) {
                        EArenaSite *rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                        rec->call = e;
                        rec->argRoot[0]  = (char *)rrn;      /* the receiver is argument 0 */
                        rec->argDepth[0] = (d == 0) ? 0 : d;
                        rec->n = 1;
                        *(EArenaSite **)vecPush(&c->eSites) = rec;
                    }
                } else {
                    anSetHomeDepth(e, callHomeDepth(c, &e->u.method.args, &f->params, e));
                }
                /* Resolve which arena the call finally passes; both numbers are decided here. */
                setCallArenaArg(c, e);
                setCallZoneArg(c, e);       /* 建池的调用：池生在哪一层地方 */
                /* 无论实参是局部还是参数，被推进容器的那个东西的深度都必须记下来。 */
                /* 方法调用与其它调用点一样要检查：内容流不取决于 home arena。
                 *
                 * 接收者要算作实参 0。被调者的 `params[0]` 是 `self`，而 `e->u.method.args` 里
                 * 没有接收者，两个长度因此不同，`checkCallRefArgs` 开头的 `params->len != args->len`
                 * 守卫会一言不发地提前返回 —— 引用规则于是对方法调用完全不生效。下面把接收者补在
                 * 前面即可。 */
                (void)0;   /* the check is moved below, after the arguments; see there */
            }

            /* 接收者是泛型实例时，方法签名里的 `T` 要替换成类型实参；而**方法自己**也是泛型时
             * （`fn cast<U>(self: ref box, u: U)`），它自己的参数就像自由泛型函数一样从实参推断
             * （`unifyTParams`，与 `EX_CALL` 路径同一次调用；the method's own parameters
             * included）。 */
            StructDef *msd = structOf(rb);
            Vec *sp = NULL, *sa = NULL;
            Vec  mp, ma;
            vecInit(&mp, c->arena, sizeof(const char *));
            vecInit(&ma, c->arena, sizeof(Type *));
            if (rb && rb->kind == TY_GENERIC && msd) {
                sp = &msd->typeParams;
                sa = &rb->targs;
            }
            if (f->typeParams.len) {
                Vec mt;
                vecInit(&mt, c->arena, sizeof(Type *));
                for (size_t i = 0; i < f->typeParams.len; i++)
                    *(Type **)vecPush(&mt) = NULL;
                for (size_t i = 0; i < e->u.method.args.len && i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i + 1);
                    Expr  *arg = *(Expr **)vecAt(&e->u.method.args, i);
                    Type  *want = ttSubstitute(tt, p->type, sp, sa);
                    Type  *got  = checkExprInner(c, arg);
                    (void)unifyTParams(tt, &f->typeParams, &mt, want, got);
                }
                bool missing = false;
                for (size_t i = 0; i < mt.len; i++) {
                    if (*(Type **)vecAt(&mt, i)) continue;
                    ckError(c, e->line,
                            "A generic method's type parameters are inferred from its arguments;"
                            " give an argument whose type mentions the parameter.",
                            "cannot infer type parameter `%s` of `%s`",
                            *(const char **)vecAt(&f->typeParams, i), e->u.method.name);
                    missing = true;
                    break;
                }
                if (missing) return ttError(tt);
                /* The method's own parameters come **after** the receiver's, so the parallel lists
                 * the substitutions below use cover both. */
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    *(const char **)vecPush(&mp) = *(const char **)vecAt(&f->typeParams, i);
                    *(Type **)vecPush(&ma) = *(Type **)vecAt(&mt, i);
                }
                if (sp) {
                    for (size_t i = 0; i < sp->len; i++) {
                        *(const char **)vecPush(&mp) = *(const char **)vecAt(sp, i);
                        *(Type **)vecPush(&ma) = *(Type **)vecAt(sa, i);
                    }
                }
                sp = &mp;
                sa = &ma;
            }

            size_t want = f->params.len - 1;
            Type *rt = f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);

            if (e->u.method.args.len != want) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        e->u.method.name, want, e->u.method.args.len);
                return rt;
            }
            for (size_t i = 0; i < want; i++) {
                Param *p = *(Param **)vecAt(&f->params, i + 1);
                Expr  *a = *(Expr **)vecAt(&e->u.method.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);

                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                /* `coroutine<T>` 是统一的句柄，一个普通的 24 字节值。 */
                if (0 && typeContainsProto(c->tt, at, "coroutine")) {
                    ckError(c, a->line,
                            "A coroutine value stays where it was spawned: today it cannot be passed"
                            " to a function, stored in a container or returned. Keep driving it in"
                            " the function that created it",
                            "a coroutine cannot be passed as an argument");
                } else {
                    checkAssignable(c, pt, at, a, "argument");
                }
            }
            /* 规则要在实参检查完之后再查：在那之前 `a->type` 还没填，`typeContainsRef` 对一切都说
             * "没有"，违规会悄悄通过。接收者作为实参 0 补在最前面，因为被调者的 `params[0]` 是
             * `self`。 */
            {
                Vec margs; vecInit(&margs, c->arena, sizeof(Expr *));
                *(Expr **)vecPush(&margs) = e->u.method.recv;
                for (size_t ai = 0; ai < e->u.method.args.len; ai++)
                    *(Expr **)vecPush(&margs) = *(Expr **)vecAt(&e->u.method.args, ai);
                checkCallRefArgs(c, f, &margs, &f->params, anHomeDepth(e),
                                 e->line, e->u.method.name);
            }
            return rt;
        }

        case EX_STRUCTLIT: {
            /* The type comes from the name written in the literal or from the context; a generic
             * instance can only come from the context.
             *
             * A written name goes through `ttResolve` with the current context, exactly the way a
             * type annotation does, so a name the module system mangles (`box` -> `box$box`)
             * resolves as written. The message for a name that really is unknown comes from
             * `ttResolve`, which also reports the ambiguous case (`pair` exported by two
             * modules). The literal's own fields are then looked up in the resolved struct. */
            Type *st = e->type;
            if (e->u.lit.name) {
                st = ttResolve(tt, c->ctx, typeNamed(c->arena, e->u.lit.name), e->line, c->curParams);
                if (!st || ttIsError(st)) return ttError(tt);
            }
            StructDef *sd = structOf(st);
            if (!sd) {
                if (e->u.lit.name)
                    ckError(c, e->line, NULL, "`%s` is not a struct", e->u.lit.name);
                else
                    ckError(c, e->line,
                            "a bare `{}` works only where the context pins the type down: "
                            "`var x: T = {}` / `return {}` / `f({})`",
                            "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            if (st->kind == TY_STRUCT && sd->typeParams.len > 0) {
                ckError(c, e->line, "a generic needs explicit type arguments, e.g. `Pair<i32, i32> { ... }`",
                        "`%s` is generic; type arguments cannot be inferred here", DN(sd));
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "fields of %s:", DN(sd));
                    for (size_t k = 0; k < sd->fields.len; k++)
                        bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, k))->name);
                    ckError(c, fi->value->line, bufCstr(&note),
                            "struct `%s` has no field `%s`", DN(sd), fi->name);
                    continue;
                }
                /* Writing a private field is as much a breach as reading one: a literal that can
                 * set a container's storage from outside lets anyone plant a fabricated pointer
                 * there. Assignment goes through the field path, which is checked; a literal does
                 * not, so it needs its own check. */
                {
                    const char *home = c->curFunc ? c->curFunc->modName : NULL;
                    if (fd->isPrivate && sd->modName != home) {
                        ckError(c, fi->value->line,
                                "Construct the value through the type's own constructor: storage"
                                " is the type's business.",
                                "field `%s` of `%s` is private to module `%s`", fd->name, DN(sd),
                                sd->modName ? sd->modName : "this file");
                        continue;
                    }
                }
                Type *want = fd->type;
                if (st->kind == TY_GENERIC)
                    want = ttSubstitute(tt, want, &sd->typeParams, &st->targs);

                adoptContextType(fi->value, want);
                Type *vt = checkInto(c, fd->type, fi->value);
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "field `%s`", fi->name);
                checkAssignable(c, want, vt, fi->value, bufCstr(&what));
            }

            /* 省略的字段用零值填，但 `ref` 没有零值：放任不管会生成 `.field = 0`，即空引用，而语言
             * 说 `ref` 永不为 null。 */
            for (size_t i = 0; i < sd->fields.len; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
                bool given = false;
                for (size_t k = 0; k < e->u.lit.inits.len && !given; k++)
                    given = strcmp((*(FieldInit **)vecAt(&e->u.lit.inits, k))->name, fd->name) == 0;
                if (given) continue;

                Type *ft = fd->type;
                if (st->kind == TY_GENERIC)
                    ft = ttSubstitute(tt, ft, &sd->typeParams, &st->targs);
                if (typeLacksZeroValue(tt, ft))
                    ckError(c, e->line,
                            typeContainsFn(tt, ft)
                                ? "A `fn` field is a code pointer, and this language has no null"
                                  " one: give it explicitly, or declare the field as"
                                  " `option<fn …>`."
                                : "`ref` has no default value (it is a non-nullable reference)",
                            "field `%s` must be given explicitly", fd->name);
            }
            return st;
        }
    }
    return ttError(tt);
}

/* Declared here because the two walks below recurse through it. */
static bool funcMayPrint(Checker *c, FuncDef *f);

static bool stmtMayPrint(Checker *c, Stmt *s);

/**
 * @brief 判断求值一个表达式是否会打印（直接或经由调用）。
 *
 * @param c  检查器。
 * @param e  要看的表达式；NULL 算作"不打印"。
 * @return   从这里能到达 `print` / `println` 调用则返回 true。
 */
static bool exprMayPrint(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return true;
        if (strcmp(n, "flush") == 0) return false;      /* flushing is not printing */
    }
    switch (e->kind) {
    /* `dyn Trait(x)` 的载荷是子表达式（`dynv.payload`），每个遍历器都必须看进去 —— 遍历器都以
     * `default:` 结尾，`-Wswitch` 不会指出漏掉的那一支。 */
    case EX_DYN: return exprMayPrint(c, e->u.dynv.payload);
    case EX_CALL:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        if (exprMayPrint(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_ASSOC:
        if (planCallee(e) && funcMayPrint(c, planCallee(e))) return true;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    case EX_BIN:   return exprMayPrint(c, e->u.bin.left) || exprMayPrint(c, e->u.bin.right);
    case EX_UN:    return exprMayPrint(c, e->u.un.operand);
    case EX_FIELD: return exprMayPrint(c, e->u.field.obj);
    case EX_INDEX: return exprMayPrint(c, e->u.index.obj) || exprMayPrint(c, e->u.index.index);
    case EX_SLICE: return exprMayPrint(c, e->u.slice.obj) || exprMayPrint(c, e->u.slice.lo) ||
                          exprMayPrint(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprMayPrint(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprMayPrint(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF:   return exprMayPrint(c, e->u.ref.operand);
    case EX_EXT: return exprMayPrint(c, e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprMayPrint(c, e->u.deref.operand);
    case EX_SIGN:  return exprMayPrint(c, e->u.sign.operand);
    case EX_CONV:  return exprMayPrint(c, e->u.conv.operand);
    case EX_TRY:   return exprMayPrint(c, e->u.try_.operand);
    case EX_NEW:   return exprMayPrint(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE:
        return exprMayPrint(c, e->u.coalesce.main) || exprMayPrint(c, e->u.coalesce.fallback);
    default: return false;
    }
}

/**
 * @brief 判断执行一个语句是否会打印（直接或经由调用）。
 *
 * @param c  检查器。
 * @param s  要看的语句；NULL 算作"不打印"。
 * @return   从这里能到达 `print` / `println` 调用则返回 true。
 */
static bool stmtMayPrint(Checker *c, Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_YIELD:  return exprMayPrint(c, s->u.yield_.value);
    case ST_TRAP:   return true;      /* 它自己就往 stderr 写一行 */
    case ST_VAR:    return exprMayPrint(c, s->u.var.init);
    case ST_ASSIGN: return exprMayPrint(c, s->u.assign.target) || exprMayPrint(c, s->u.assign.value);
    case ST_EXPR:   return exprMayPrint(c, s->u.expr.expr);
    case ST_RETURN: return exprMayPrint(c, s->u.ret.value);
    case ST_IF:     return exprMayPrint(c, s->u.ifs.cond) ||
                           stmtMayPrint(c, s->u.ifs.thenBody) || stmtMayPrint(c, s->u.ifs.elseBody);
    /* `d.run { … }`: the receiver is an expression, the block is a body -- walk both. */
    case ST_DOMAIN: return exprMayPrint(c, s->u.domain_.callee) || stmtMayPrint(c, s->u.domain_.body);
    case ST_WHILE:  return exprMayPrint(c, s->u.whiles.cond) || stmtMayPrint(c, s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtMayPrint(c, *(Stmt **)vecAt(&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH:
        if (exprMayPrint(c, s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtMayPrint(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    default: return false;
    }
}

/**
 * @brief 判断一个函数是否会打印（直接或经由它发出的调用），带环路守卫。
 *
 * `FuncDef.callees` 要等函数体检查完才填好，所以检查函数体的过程中不能用它；这里改走被调者的
 * AST，因而需要自己的环路守卫。调用者无法解析或函数处于递归环里都算作会打印，这是给"能否重排"
 * 这个问题用的安全方向。
 *
 * @param c  检查器。
 * @param f  要看的函数；NULL 表示被调者没能解析出来。
 * @return   从这里能到达 `print` / `println` 调用则返回 true。
 */
static bool funcMayPrint(Checker *c, FuncDef *f) {
    if (!f) return true;                       /* unresolved: assume it prints */
    if (anMayPrintState(f) == 1) return true;
    if (anMayPrintState(f) == 2) return false;
    if (anMayPrintState(f) == 3) return true;    /* a cycle: assume it prints */
    anSetMayPrintState(f, 3);
    bool r = stmtMayPrint(c, f->body);
    anSetMayPrintState(f, r ? 1 : 2);
    return r;
}

/**
 * @brief 判断调用一个函数是否有可观察的副作用。
 *
 * 会打印、拿到 `mut ref` 或 `mut` 视图形参（因而可能写实参）、自己又会到达这样的调用、或者根本
 * 无法解析，都算有副作用。纯 getter 不算：交换两个 getter 观察不到；按"含调用"判断会拒绝由普通
 * 访问器写成的程序。
 *
 * @param c  检查器。
 * @param f  被调者；NULL 表示没能解析出来。
 * @return   重排这次调用会改变程序行为则返回 true。
 */
static bool callIsEffectful(Checker *c, FuncDef *f) {
    if (!f) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->type) continue;
        if (p->type->kind == TY_REF && p->type->mut) return true;      /* may write the argument */
        if (p->type->kind == TY_GENERIC && p->type->mut) return true;  /* a `mut` view */
    }
    return funcMayPrint(c, f);
}

/**
 * @brief 判断表达式里是否含**调用**，不管副作用摘要怎么说（语法问题）。
 *
 * `exprHasCall` 回答语义问题（"求值两次能被观察到吗"），那是 `??` 排序规则需要的。这一个回答语法
 * 问题，而且故意钝：它守的是编译器唯一要把表达式命名两次的地方 —— 运算符是用户自定义方法的复合
 * 赋值目标（`x += y` 变成 `x = add(&x, y)`）。那里"摘要说它是纯的"不足以成为复制一次调用的理由：
 * 一个读全局却从不写它的函数体按摘要就是纯的，复制它仍然错。规则只有一条、没有例外：目标里有调用
 * 就拒绝，消息让人拆成两行。
 *
 * @param e  要看的表达式；NULL 算作"没有调用"。
 * @return   里面任意深度出现调用则返回 true。
 */
static bool exprHasAnyCall(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* `dyn Trait(x)` 的载荷是子表达式（`dynv.payload`），每个遍历器都必须看进去 —— 遍历器都以
     * `default:` 结尾，`-Wswitch` 不会指出漏掉的那一支。 */
    case EX_DYN: return exprHasAnyCall(e->u.dynv.payload);
    case EX_CALL: case EX_METHOD: case EX_ASSOC: case EX_GENCALL: return true;
    case EX_BIN:   return exprHasAnyCall(e->u.bin.left) || exprHasAnyCall(e->u.bin.right);
    case EX_UN:    return exprHasAnyCall(e->u.un.operand);
    case EX_REF:   return exprHasAnyCall(e->u.ref.operand);
    case EX_EXT: return exprHasAnyCall(e->u.ext_.call);   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF: return exprHasAnyCall(e->u.deref.operand);
    case EX_SIGN:  return exprHasAnyCall(e->u.sign.operand);
    case EX_CONV:  return exprHasAnyCall(e->u.conv.operand);
    case EX_TRY:   return exprHasAnyCall(e->u.try_.operand);
    case EX_FIELD: return exprHasAnyCall(e->u.field.obj);
    case EX_INDEX: return exprHasAnyCall(e->u.index.obj) || exprHasAnyCall(e->u.index.index);
    case EX_SLICE: return exprHasAnyCall(e->u.slice.obj) || exprHasAnyCall(e->u.slice.lo) ||
                          exprHasAnyCall(e->u.slice.hi);
    case EX_NEW:   return exprHasAnyCall(e->u.new_.count);
    case EX_COALESCE:
        return exprHasAnyCall(e->u.coalesce.main) || exprHasAnyCall(e->u.coalesce.fallback);
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasAnyCall(*(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasAnyCall(*(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasAnyCall((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprHasAnyCall((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    default: return false;      /* literals, bindings, `null` */
    }
}

/**
 * @brief 判断表达式里是否含**重排就可观察**的调用（语义问题）。
 *
 * switch 每一支都返回值、`default:` 给 `false`，所以遍历总有确定答案：已知无副作用的调用
 * （`pure(x)`）被报成没有，于是"前面还有一个调用没被移走"由表达式决定，而不是由一个不确定的结果
 * 决定。
 *
 * @param c  检查器。
 * @param e  要看的表达式；NULL 算作"没有调用"。
 * @return   里面任意深度出现了有副作用的调用则返回 true。
 */
static bool exprHasCall(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    /* `dyn Trait(x)` 的载荷是子表达式（`dynv.payload`），每个遍历器都必须看进去 —— 遍历器都以
     * `default:` 结尾，`-Wswitch` 不会指出漏掉的那一支。 */
    case EX_DYN: return exprHasCall(c, e->u.dynv.payload);
    case EX_CALL: case EX_METHOD: case EX_ASSOC:
        if (callIsEffectful(c, planCallee(e))) return true;
        return false;
    case EX_BIN:      return exprHasCall(c, e->u.bin.left) || exprHasCall(c, e->u.bin.right);
    case EX_UN:       return exprHasCall(c, e->u.un.operand);
    case EX_FIELD:    return exprHasCall(c, e->u.field.obj);
    case EX_INDEX:    return exprHasCall(c, e->u.index.obj) || exprHasCall(c, e->u.index.index);
    case EX_SLICE:    return exprHasCall(c, e->u.slice.obj) || exprHasCall(c, e->u.slice.lo) ||
                             exprHasCall(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasCall(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            if (exprHasCall(c, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF: case EX_DEREF: case EX_SIGN: case EX_CONV: case EX_TRY:
        return exprHasCall(c, e->kind == EX_REF ? e->u.ref.operand :
                              e->kind == EX_DEREF ? e->u.deref.operand :
                              e->kind == EX_SIGN ? e->u.sign.operand :
                              e->kind == EX_CONV ? e->u.conv.operand : e->u.try_.operand);
    case EX_EXT:      return exprHasCall(c, e->u.ext_.call);   /* `ext f(x)`: the spawned call */
    case EX_NEW:      return exprHasCall(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE: return exprHasCall(c, e->u.coalesce.main) || exprHasCall(c, e->u.coalesce.fallback);
    default:          return false;      /* literals, bindings, and `null` */
    }
}

/**
 * @brief 检查一个表达式并把类型写回节点，引用不被解引用。
 *
 * 这是检查器其余部分使用的入口，结果同时留在 `e->type` 上；表达式含调用时置上语句效果标志，供
 * `??` 判断是否需要提前取临时量。置标志的是被求值的表达式，`??` 的主语不置：它的临时量与其它
 * 临时量按源码顺序一起上提，相对顺序不变。
 *
 * @param c  检查器。
 * @param e  表达式；NULL 直接得到错误类型。
 * @return   表达式的类型；无法确定时返回错误类型，调用者不会看到空类型。
 */
Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    /* `!` 故意不在这里：它的操作数是一个解包助手的实参，里面的调用就地执行，标志照常置上。 */
    if (exprHasCall(c, e) &&
        !(e->kind == EX_COALESCE && planNeedTemp(e))) c->stmtFx = 1;
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

