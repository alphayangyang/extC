#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

/* check.c 拆分的**内部头**：跨文件共享的状态 + 跨文件函数原型。
 * 公开接口只有 `check.h` 里的 `checkModule` 一个 ✓
 *
 * ⚠️ 只允许放**类型和原型**，不许放逻辑 ✓ */

#include "check.h"

/* ⭐ **诊断里显示声明名用 `DN(x)`** —— `StructDef*` / `TypeDef*` 都行 ✓
 * 为什么要有这一层：模块 mangle 之后 `name` 是内部的 `io$reader`，而 `srcName`
 * 才是用户写的 `io::reader` ✗ 直接印 `->name` 就是把编译器内部编码漏给用户
 * （真踩过：`struct \`alpha$pair\` has no field \`zzz\``）✓ */
#define DN(d) ttDispName((d) ? (d)->srcName : NULL, (d) ? (d)->name : NULL)

/* ⭐ **诊断/调试输出里显示函数名用 `FN(f)`** —— mangle 之后 `f->name` 是
 * `pair$make`，而用户写的是 `pair::make` ✗（`EXTC_DUMP_EFFECTS` 那几行踩过 ✓）
 * 根模块/单文件程序 `modName` 为空 ⇒ 直接 `name`（就是源码名 ✓）
 * 实现：把 mangle 前缀 `mod$` 换成 `mod::`（前缀一定等于 `modName` ✓）*/
const char *checkFnDisplay(const char *name, const char *modName);
#define FN(f) checkFnDisplay((f) ? (f)->name : NULL, (f) ? (f)->modName : NULL)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------ 共享状态（原 check.c 的「状态」一节）
 * 以及被多个文件用到的类型 —— 谁用到谁就得能看见 ⇒ 放这里 ✓ */

typedef struct {
    const char *name;
    const char *cname;   /* 生成 C 的名字 —— 同层遮蔽时 `a` → `a__2`，见 §名字 */
    Type       *type;
    bool        mut;
    /* ⭐ 档2（ARENA-FORMAL §7.4）：这个绑定的**地址被人拿走过**吗？
     * （`ref x` / `x[..]` / 当 `mut ref` 实参传出去……）
     *   取过 ⇒ 别人可能通过**别名**改它的内容 ⇒ 只能**弱更新**（保守）✗
     *   没取过 ⇒ 可以直接**强更新**（覆盖深度）⇒ 消掉"置空/换值救不回来"的误拒 ✓ */
    bool        addressed;
    /* ⭐ 档2.3（ARENA-FORMAL §7.4）：**字段级深度** ——
     * `h.p` 有自己的深度，不再跟 `h` 共用一个数 ⇒ `h.p = null` 能真正**覆盖**那一格 ✓
     * 最多 4 格；满了 / 归不到某一格的（元素写、对象字段）走 `otherDepth` 保守兜底 ✓
     * 根的有效深度 = max(otherDepth, 各格) ✓ */
    struct { const char *name; int depth;
             /* ⭐⭐ 层 2（附录 D.2）：**这一格是被哪个值写进去的**（NULL = 说不清）——
              * 只用来"顺着容器找到里面那个分配站点"，**不用来改任何记账** ✓
              * 为什么需要：`var h: box = { p: null, q: null }` 的**来路**里没有 `x`，
              * 而 `h.q = x` 是后来写的 ⇒ 沿来路走永远追不到那个站点 ✗ */
             Expr *src;
             /* ⚠️ **来源自己那次写入的深度** —— 比新旧必须用它，不能用 `depth` ✗
              * (`depth` 是弱更新取 max 的：`h.q = x; h.q = null` 之后它还是深的，
              *  拿它比会把 null 那次误判成"更深" ⇒ 来源被清掉 ⇒ 链断 ✓ 实测踩过) */
             int   srcDepth;
             /* ⭐ 这一格里的引用**被要求过活到的最浅那一层**（0 = 要活到帧外）——
              * `out = h` 是第 1 层，可 `return out` 要的是**帧外** ⇒ 站点得进家 arena ✓
              * （只看"这一次的目的地"会算得太浅 ⇒ 站点提不动 ⇒ 悬垂 ✗ 实测踩到）*/
             int   minReq; } fields[4];
    /* ⭐ 层 1（数据流）第一步：**这张字段表完整吗？**
     * 完整 = "这个绑定的**每一格**都在表里（或已知为 0）" ⇒ 允许"抹掉一格之后
     *        把根重算成剩下几格的最大值"（强更新）✓
     * 不完整 = 表是**拷贝/别处来的**，缺的格可能有值 ⇒ **不许下降**（否则那几格的
     *          深度会凭空消失 —— 我上次就是这样造出一条 `stack-use-after-scope` 的 ✗）
     * 默认 false（保守：不完整）✓ */
    bool        fieldsComplete;
    int         nfields;
    int         otherDepth;
    int         depth;   /* 词法深度：参数 = 0，函数体里的局部 = 1，每进一层块 +1。
                          * 逃逸检查就比这个数 —— 见 REFS.md §4 */
    /* **引用型绑定**：这个引用**指向的东西**有多深？
     *
     * 为什么它跟 `depth` 是两回事：`var cur: ?ref node = head` 里，装引用的那个
     * **槽位**在本帧（深度 1），但它指的节点在外面（深度 0）。
     * 「引用不能活得比被指对象长」这条规则问的是**被指对象** ⇒ 必须用这个数 ✓
     * （以前一律拿槽位深度 ⇒ `while cur != null { ... return cur }` 这种
     *   最正常的链表搜索被误报成"指向会死的局部" ✗ 见 PLAN.md §0.4 #8）
     *
     * 只在**类型是引用**时有意义；默认值 = 槽位深度（保守：跟老行为一样），
     * 声明和换指向时按初始值的真实深度**收紧** ✓ */
    int         refDepth;
    /* ⭐ 定案 63（PLAN #38）：这个绑定的**初始值表达式**（没有初始化式 ⇒ NULL）✓
     *
     * 为什么要有它：提升是**顺着值的来路往回走**的 ——
     *     var n = new node        // n 的"来路"就是这个 `new`
     *     head = n                // 存进更外层的地方 ⇒ 顺着 n 找回那个 `new` 提升它 ✓
     * ⚠️ 只跟**初始值**（不追后续赋值、不追别名）：提不动的照旧报错 ⇒ 安全方向 ✓ */
    Expr       *origin;
    int         line;
    /* ⭐ 定案 70：这个绑定属于哪个模块（全局才有；局部 = NULL）——
     * 用它在解析处挡"不带限定地引用别的模块的名字" ✓ */
    const char *modName;
} Sym;

/* ⭐⭐ 层 2（数据流）：**把"这个名字解析到了哪个绑定"钉在 AST 节点上** ✓
 *
 * 为什么需要它：收尾的解算 pass 跑在**所有函数体都查完之后**，那时作用域已经弹了 ⇒
 * `lookup(c, name)` 找不到局部绑定（`noteOrigin` 为同一个坑警告过一次 ✗）。
 * 实测症状（`varArray_i32` 的 `return v`）：`[at] … kind=4 dca=0 svd=0` ⇒ 复算被跳过 ⇒
 * 用了解算**之前**冻的旧深度 ⇒ 误拒 ✗
 *
 * 为什么存"绑定指针"而不是"名字"：同一个名字在不同作用域是**不同**的绑定 ⇒
 * 解析**那一刻**的答案才是权威（收尾时按名字再查可能撞上另一个同名的东西 ✗）。
 * 绑定的寿命不是问题：`Sym` 由 arena 分配（`scopeAdd` 走 `c->arena`）⇒
 * 弹作用域只是从可见性里摘掉，内存还活着 ✓
 *
 * ⚠️ 包装成一个小结构只是为了**类型安全**：`ast.h` 那边只能放不透明指针
 * （它不认识 `Sym`，跟 `Expr.cname` 同一个理由）⇒ 两边各转一次，中间不留 `void*` 裸奔 ✓ */
typedef struct { Sym *sym; } IdentBinding;

/* ⭐ 把 `e`（`EX_IDENT`）解析到的绑定取回来 —— 没解析过返回 NULL ✓ */
static inline Sym *identBindOf(Expr *e) {
    return (e && e->kind == EX_IDENT && e->u.ident.sym) ? ((IdentBinding *)e->u.ident.sym)->sym : NULL;
}

typedef struct { const char *name; int count; } NameUse;

typedef struct { Vec syms; } Scope;

/* ⭐ B4′：一个"E 敏感"的调用点 —— 记下足够的信息，收尾时**不依赖作用域**也能重算 ✓ */
typedef struct {
    Expr  *call;         /* EX_CALL / EX_METHOD / EX_ASSOC 节点（收尾时改它的 arenaArg）*/
    char  *argRoot[8];   /* 每个 `mut ref` 实参的根名（NULL = 那个位置不是 mut ref 实参）*/
    int    argDepth[8];  /* 那个实参求值时的深度（0 = 参数/全局 ⇒ 已经"活在帧外"）*/
    int    n;            /* 用到的槽数（上限 8）*/
    bool   overflow;     /* 超过上限 ⇒ **说不全** ⇒ 收尾按"传家 arena"处理（保守但 sound）✗ */
} EArenaSite;

typedef struct {
    Ctx       *ctx;
    Arena     *arena;
    TypeTable *tt;
    Module    *m;
    Vec        scopes;      /* Scope* */
    FuncDef   *curFunc;
    Vec       *curParams;   /* 当前可见的泛型参数名（NULL = 不在泛型上下文）*/
    Vec        eqChecks;    /* EqCheck* —— 推迟到实例化复查的 `==` */
    /* ⭐ PLAN #47：**泛型自由函数的实例**（`fn f<T>` 每套实参一份）✓
     * 它们在检查器里"出生"（调用点推导出来），在 codegen 里当普通函数吐出来 ✓ */
    Vec        funcInsts;   /* FuncDef*（实例：tmpl/targs/instName 都填好）*/
    Vec        globals;     /* Sym* —— 全局变量（深度 0），不进 scopes 见 lookup 的注释 */
    /* ⭐ 层 2（附录 D.2）：**"正在往某个地方存的那个值"** ——
     * `noteFieldDepthWrite` 要顺手记"这一格是谁写的"，而它的调用点已经有这个值了 ⇒
     * 用一个字段传过去，**不改它的签名**（那会牵动所有调用点）✓ */
    Expr      *curStoreVal;
    Vec        nameUses;    /* NameUse* —— 当前函数里每个名字用过几次（生成 C 的改名用）*/
    Vec        moduleNames;     /* const char*（已排序）—— 编译时长优化见 check.c */
    size_t     moduleNamesSize;
    /* ---- `?ref T` 的**非空收窄**（narrowing）----
     * 可空引用不能直接解 —— 必须先**证明**它非空，唯一的证明方式是在 `if` 里跟
     * `null` 比过。这里记的就是"当前这个位置上，哪些绑定已经被证明非空"。
     * 收窄是**词法作用域的**（跟着 pushScope/popScope 自动回退），
     * 而且证明完之后**不生成任何运行时检查** —— 编译期能证明的，运行时不留痕迹（P）✓ */
    /* 这个位置上**吐不出"语句前缀"** ⇒ 需要提前求值的 `??` 一律报错。
     * 两个地方：`while` 的条件（吐出来就变成"进循环前算一次"，不是每轮算 ✗）
     * 和全局初始化式（没有语句可挂）✓ */
    /* ---- 泛型体的**推迟检查**（PLAN #17/#18）----
     * 泛型体是照**模板**查一次的，那时 `T` 不透明 ⇒ "含引用才查"的规矩全早退 ⇒
     * `T = slice<u8>` 的实例化**根本没人查** ✗
     * 修法：**记录时就把深度按"`T` 可能带引用"算好**（那时作用域还在、`lookup` 找得到），
     * 实例化时只回答一个问题："这个实例的 `T` 到底带不带引用" ✓
     * ⚠️ 走过的弯路：第一版把"算深度"也推到实例化再做 —— 那时**函数作用域已经没了**，
     *    `lookup` 找不到参数 ⇒ 深度算成 0 ⇒ 检查静默失灵 ✗（真踩了）*/
    Vec        refChecks;   /* RefCheck* —— 推迟到实例化复查的引用规矩 */
    Vec        callChecks;  /* CallCheck* —— 推迟到实例化再解析的调用点（PLAN #50）*/
    Vec       *substParams; /* 实例化复查时正在用的替换（NULL = 不在复查）*/
    Vec       *substArgs;
    int        noHoist;
    /* ⭐ PLAN #22（2026-09-22）：**本语句里已经出现过"带副作用的子表达式"了吗？**
     * `??` 的主体不是纯的时候要先算进临时变量，而那个前缀**吐在整条语句之前** ⇒
     * 它会**跳到**同语句里更靠前的副作用前面去 ✗
     * ⇒ 一旦发现"前面已经有副作用"且这个 `??` 要临时变量 ⇒ **报错让他拆行** ✓
     * 判据用**调用**（EX_CALL/EX_METHOD/EX_ASSOC）—— 分配/字面量/转换的顺序不可观测 ✓ */
    int        stmtFx;
    /* ⭐ 定案 68：**当前函数体里"等闭包之后再定一次"的节点**（`Expr*`，`new` 站点 +
     * 那些 arena 归属待定的调用点）—— 查完体之后交给 `FuncDef.arenaSites`，
     * 等 `needsHome` 闭包跑完再统一定案 ✓ */
    Vec        curArenaSites;
    /* ⭐ B4′（架构：**分析最后写、检查只读**）：E 敏感的"家 arena"决策要**推迟**
     * 到所有函数体的效果摘要封闭之后再定。
     * 为什么：`callHomeDepth` 里那条"实参会不会被搬出本函数 ⇒ 传我的家"依赖 **E**，
     * 而 E（`computeEscapes`）要用**被调者的效果摘要**（"它会不会把实参存起来"）
     * —— 那个摘要在**查体当中还没封闭**（闭包在所有体查完之后才跑）✗
     * ⇒ 那时 `pub == 0`（摘要还是空的）⇒ 判不出"实参会被发布"⇒ 家 arena 选成
     *    **调用者自己的帧** ⇒ 被调者往那个容器里塞的东西，本次调用返回即 release ✗
     * （反例 B2/B3：`fill` 里 `pushOne(l, 7)` 之后 `sink(out, l)` 把 l 发布出去 ✓）
     * ⇒ 查体时把这些调用点**记下来**，收尾 pass 按封闭后的摘要重算 ✓ */
    Vec        eSites;      /* EArenaSite* */
    /* ⭐⭐ 长运行内存：**"这个值被存到了第 k 层"** 的纪录表
     * —— 每一条都是**已有的逃逸检查顺手交出来的**（不另写遍历）✓
     * 收尾时把它们**反复重放到不动点** ⇒ 每个分配站点的最终层号
     * = **所有约束里的最小值**（越小活得越久）。
     * 为什么要不动点：一条约束只能推动"它碰到的那些站点"，而那些站点
     * 变长寿之后又会让**别的**约束推得动 ⇒ 得轮到没变化为止 ✓
     * （跟 `EArenaSite` 那个"跑四轮"、`needsHome` 的传递闭包是**同一件事** ✓）*/
    Vec        lvlFacts;    /* LvlFact* */
    /* ⭐ 解算中：重放事实时**不许再记新事实** ✗
     * （第一版漏了这个闸 ⇒ 重放一次又记一条 ⇒ 事实表无限长 + 32 轮
     *   ⇒ **编译器自己被 OOM kill** ✗✗ 真踩到了）*/
    bool       lvlSolving;
    Vec        narrow;      /* const char* —— 已被证明非空的绑定的 cname */
    /* ⭐ 档1（ARENA-FORMAL §9）：**E 分析** —— 本函数里会被"搬出本函数"的局部名 ✓
     * 只影响"家 arena 选哪只"（保守方向 = 多算只会费内存）✓ */
    Vec        escapees;    /* const char* */
    int        escapeesFor; /* 已经为哪个函数算过 E（-1 = 还没算）*/

    Vec        narrowMarks; /* size_t —— 每个作用域进来时的 narrow.len（出作用域回退用）*/

    Type *tI32, *tF64, *tBool;
    StructDef *sliceDef;/* prelude 里的 `slice<T>` 声明（视图协议）*/
    Type *tSliceU8;     /* 字符串字面量的类型：`slice<u8>` */
} Checker;

/* 一条"推迟到实例化再算"的引用规矩（PLAN #17/#18）*/
typedef struct {
    Expr       *val;     /* 被检查的值 */
    Expr       *target;  /* 非 NULL = "把它存进这个目标"（原来是 checkStoreEscape）*/
    int         at;      /* 目标深度（记录时算好：那时作用域还在 ✓）*/
    int         depth;   /* 值里引用的深度（同上，**按"T 可能带引用"算**）*/
    bool        borrowed;/* 值是不是借来的（同上）*/
    /* ---- 第二类：**零值**（同一族的另一半）----
     * `var local: T`（没有初始化式）在模板期看不出问题（`T` 不透明），
     * 但 `T = slice<u8>` 的实例化会得到 `(slice_u8){0}` = **一个 null 引用** ✗
     * （extC 承诺"`ref` 永不为空"）⇒ 也推迟到实例化再查 ✓ */
    bool        isZero;
    /* ---- 第三类：**`new T[n]` 的大小**（2026-09-20，为了让 `varArray<T>` 写得出来）----
     * 模板期 `T` 没有大小 ⇒ 不能报错、也不能放行 ⇒ 记录一笔，实例化时对每个具体实例查 ✓ */
    bool        isNewSize;
    Type       *declType;
    int         line;
    const char *what;
    FuncDef    *func;    /* 这条规矩属于哪个泛型函数 */
} RefCheck;

/* ⭐⭐ 长运行内存：**一条"这个值得装得下第 `at` 层"** 的约束 ✓
 *
 * 为什么是一张"事实表"而不是一个"决定"：
 *   层号越小 = 活得越久，而一个分配站点的最终层号 = **它碰到的所有约束里最小的那个** ✓
 *   而"它碰到哪些约束"取决于**它自己当时在哪层** ⇒ 得反复重放到不动点 ✓
 *   （以前是"边查边拍板"：`new` 一出现就先定一个层 ⇒ 而后只能往一个方向改
 *     ⇒ 收尾时再也分不清"这个层是拍的还是算的" ✗ —— 就是卡住的根因）*/
typedef struct { Expr *val; int at; } LvlFact;

/* 推迟到实例化复查的 `==` 的记账条目（expr 那边产生，top 那边消费）*/
/* ⭐ PLAN #42(c)：`func` = 这条 `==` 属于哪个函数（用来问"它被调用过吗"✓）*/
typedef struct { Expr *node; StructDef *owner; const char *op; FuncDef *func; } EqCheck;

/* ⭐ PLAN #50（2026-09-23）：**推迟到实例化再解析的调用点**。
 *
 * 为什么需要它：`fn twice<T>(x: T) -> T { return idOf(x) }` 里，模板期 `T` 不透明 ⇒
 * `idOf` 那个类型实参是 `T`（`ttHasParam`）⇒ **这个时刻造不出正确的实例**（造出来会是
 * `idOf_T`，一个 `T` 还是参数的实例 ✗）。而这跟 `RefCheck`/`EqCheck` 是**同一族**
 * 问题、也用**同一个模式**解：模板期记一笔，实例化时按实例重解 ✓
 *
 * 为什么不能"就地跳过后面的检查"：模板体后面还要用 `e->func` 的类型判实参、
 * 算返回类型 ⇒ 必须有**某个**签名可用。所以模板期仍然造一个"以 `T` 为实参"的实例
 * （`idOf_T`，签名自洽、`T` 当不透明类型用 ✓），实例化时再把 `e->func`
 * **改指**到那个具体实例（`idOf_i32`）✓
 *
 * ⚠️ 与 `RefCheck` 的关键差别：`RefCheck` 只在**签名**上复查（不碰函数体），
 *    而这个要**改写 AST 上的 `e->func`** ⇒ 必须在"该实例正在复查"的上下文里做，
 *    而这正是复查 pass 已经具备的（`c.substParams/substArgs` 已设好 ✓）*/
typedef struct {
    Expr    *node;      /* 那个 `EX_CALL` / `EX_METHOD` / `EX_ASSOC` 节点 ✓ */
    FuncDef *tmpl;      /* 被调的**模板**（自由函数 / 泛型类型的方法）✓ */
    Vec      targs;     /* 模板期推出来的实参（**可能含 T** ⇒ 要用 tsub 代入 ✓）*/
    FuncDef *func;      /* 这条属于哪个（模板）函数 —— 决定它对哪些实例适用 ✓ */
} CallCheck;

/* ------------------------------ 跨文件原型
 * 自动生成：拿拆分**前**的 check.c 过一遍 `gcc -aux-info`，
 * 取每个函数的原文签名（去掉 static）—— 手写 60 多个签名只会引入笔误 ✓ */

 Expr *intLit (Checker *c, long long int v, int line);
 FieldDef *findField (StructDef *sd, const char *name);
 FuncDef *findFunc (Checker *c, const char *name);
 FuncDef *findMethod (Type *st, const char *name);
 FuncDef *findOp (Type *b, const char *sym, const char *fallback);
 StructDef *structOf (Type *t);
 Sym *declare (Checker *c, const char *name, Type *t, _Bool mut, _Bool shadow, int line, int depth);
 Sym *lookup (Checker *, const char *);
 Sym *lookup (Checker *c, const char *name);
 Sym *placeRoot (Checker *c, Expr *e);
 Type *checkExpr (Checker *, Expr *);
 Type *checkExpr (Checker *c, Expr *e);
 Type *checkInto (Checker *c, Type *want, Expr *e);
 Type *checkMaybeTry (Checker *c, Expr *e);
 Type *checkPrintArg (Checker *c, Expr *e);
 Type *checkTryInner (Checker *, Expr *);
 Type *checkTryInner (Checker *c, Expr *e);
 Type *checkValue (Checker *c, Expr *e);
 Type *payloadType (TypeTable *tt, Type *et, Variant *v, size_t k);
 Type *sliceOf (Checker *c, Type *elem);
 Type *tsub (Checker *c, Type *t);
 Type *viewElemOf (Type *t);
 Variant *findVariant (TypeDef *td, const char *name);
 _Bool asIntLit (Expr *e, long long int *out);
 _Bool blockExits (Stmt *s);
 _Bool checkAssignable (Checker *c, Type *want, Type *got, Expr *node, const char *what);
 _Bool checkEscape (Checker *, Expr *, int, int, const char *);
 _Bool checkEscape (Checker *c, Expr *val, int at, int line, const char *what);
 _Bool checkStoreEscape (Checker *c, Expr *val, Expr *target, int line);
 _Bool cmpIsNative (Type *t);
 _Bool enumHasPayload (TypeDef *td);
 _Bool isCmpOp (const char *op);
 _Bool isLogicOp (const char *op);
 _Bool isLvalue (Expr *e);
 _Bool isNarrowed (Checker *c, const char *cname);
 _Bool isNumericLit (Expr *e);
 _Bool isPlace (Expr *e);
 _Bool isPrintable (Type *t);
 _Bool isProtoType (Type *t, const char *name, size_t nargs);
 _Bool isWritablePlace (Checker *c, Expr *e);
 _Bool literalFits (Expr *e, Type *want);
 _Bool mentionsParam (Type *t);
 _Bool pathHasReadonlyRef (Expr *);
 _Bool pathRefsAllMut (Expr *e, _Bool *crossed);   /* ⭐ #40/#41：写要穿过的引用都得是 mut ✓ */
 _Bool pathHasReadonlyRef (Expr *e);
 _Bool rejectNullableDeref (Checker *c, Type *t, Expr *node, const char *what);
 _Bool repeatablePure (Expr *e);
 _Bool requireMutable (Checker *c, Expr *e, int line, const char *what);
 _Bool typeContainsRef (TypeTable *, Type *);
 _Bool typeContainsRef (TypeTable *tt, Type *t);
 _Bool typeLacksZeroValue (TypeTable *tt, Type *t);
 _Bool typeSupportsEq (Type *t, const char *op);
 const char *cNameFor (Checker *c, const char *name);
 const char *narrowTarget (Checker *c, Expr *cond, _Bool *whenTrue);
 const char *typeStr (Checker *c, Type *t);
 extern _Bool checkModule (Ctx *ctx, Arena *arena, TypeTable *tt, Module *m);
 bool isEscapeeName (Checker *, const char *);
 bool computeEffectsTransitive (Checker *, FuncDef *);
 int *fieldDepthEntry (Checker *, Sym *, const char *, bool);
 void noteFieldDepthWrite (Checker *, Sym *, const char *, int);
 const char *placeRootName (Expr *);
  int callHomeDepth (Checker *c, Vec *args, Vec *params, Expr *callNode);
 int exprRefDepth (Checker *, Expr *);
int valDepthForStore (Checker *, Expr *);
 int exprRefDepth (Checker *c, Expr *e);
 int placeDepth (Checker *c, Expr *e);
 int storeLayer (Checker *c, Expr *e);   /* ⭐ 「这块存储住在哪一层」（≠ placeDepth）✓ */
 _Bool valTracesToParam (Checker *c, FuncDef *f, Expr *val);   /* ⭐ 定案 67 ✓ */
 _Bool promoteInto (Checker *c, Expr *val, int at);   /* ⭐ 定案 63：块级逃逸提升 ✓ */
 void noteOrigin (Checker *c, Sym *sy, Expr *val);   /* ⭐ 记「这个绑定的来路」（压平）✓ */
 void adoptContextType (Expr *e, Type *want);
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
 void checkOperatorSig (Checker *c, FuncDef *f);
 void checkStmt (Checker *, Stmt *);
 void checkStmt (Checker *c, Stmt *s);
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
 void ckWarn  (Checker *c, int line, const char *note, const char *fmt, ...);   /* ⭐ 警告：不拦 ✓ */
 void desugarBareCtor (Checker *, Expr *, Type *);
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
 void expectBool (Checker *c, Type *t, Expr *node);
 void markCallHomeIfEscaping (Checker *, Expr *, int);

 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
 void setCallArenaArg (Checker *c, Expr *e);
 void requireQualified (Checker *c, const char *what, const char *whatMod, bool qualified, int line);
 Vec *funcTParams (FuncDef *f);   /* PLAN #47：可见的类型参数在哪 ✓ */
 FuncDef *funcInstance (Checker *c, FuncDef *tmpl, Vec *targs, int line);
 _Bool unifyTParams (TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got);   /* 定案 68：解析出"最终传哪只 arena" ✓ */
 void narrowFactsOf (Checker *c, Expr *cond);
 void popScope (Checker *c);
 void pushNarrow (Checker *c, const char *cname);
 void pushScope (Checker *c);
 void recordNewSizeCheck (Checker *, Type *, int);
 void recordNewSizeCheck (Checker *c, Type *t, int line);
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
 void unNarrow (Checker *c, const char *cname);

#endif /* EXTC_CHECK_INTERNAL_H */
