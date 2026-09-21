#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

/* check.c 拆分的**内部头**：跨文件共享的状态 + 跨文件函数原型。
 * 公开接口只有 `check.h` 里的 `checkModule` 一个 ✓
 *
 * ⚠️ 只允许放**类型和原型**，不许放逻辑 ✓ */

#include "check.h"

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
    int         line;
} Sym;

typedef struct { const char *name; int count; } NameUse;

typedef struct { Vec syms; } Scope;

typedef struct {
    Ctx       *ctx;
    Arena     *arena;
    TypeTable *tt;
    Module    *m;
    Vec        scopes;      /* Scope* */
    FuncDef   *curFunc;
    Vec       *curParams;   /* 当前可见的泛型参数名（NULL = 不在泛型上下文）*/
    Vec        eqChecks;    /* EqCheck* —— 推迟到实例化复查的 `==` */
    Vec        globals;     /* Sym* —— 全局变量（深度 0），不进 scopes 见 lookup 的注释 */
    Vec        nameUses;    /* NameUse* —— 当前函数里每个名字用过几次（生成 C 的改名用）*/
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
    Vec       *substParams; /* 实例化复查时正在用的替换（NULL = 不在复查）*/
    Vec       *substArgs;
    int        noHoist;
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

/* 推迟到实例化复查的 `==` 的记账条目（expr 那边产生，top 那边消费）*/
typedef struct { Expr *node; StructDef *owner; const char *op; } EqCheck;

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
 const char *placeRootName (Expr *);
 int  callHomeDepth (Checker *, Vec *, Vec *);
 int callHomeDepth (Checker *c, Vec *args, Vec *params);
 int exprRefDepth (Checker *, Expr *);
 int exprRefDepth (Checker *c, Expr *e);
 int placeDepth (Checker *c, Expr *e);
 void adoptContextType (Expr *e, Type *want);
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
 void checkOperatorSig (Checker *c, FuncDef *f);
 void checkStmt (Checker *, Stmt *);
 void checkStmt (Checker *c, Stmt *s);
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
 void desugarBareCtor (Checker *, Expr *, Type *);
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
 void expectBool (Checker *c, Type *t, Expr *node);
 void markCallHomeIfEscaping (Checker *, Expr *, int);
 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
 void narrowFactsOf (Checker *c, Expr *cond);
 void popScope (Checker *c);
 void pushNarrow (Checker *c, const char *cname);
 void pushScope (Checker *c);
 void recordNewSizeCheck (Checker *, Type *, int);
 void recordNewSizeCheck (Checker *c, Type *t, int line);
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
 void unNarrow (Checker *c, const char *cname);

#endif /* EXTC_CHECK_INTERNAL_H */
