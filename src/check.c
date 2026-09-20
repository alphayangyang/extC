/* 类型检查 pass。
 *
 * 唯一做类型推理的地方。它把结果写回 AST，之后 codegen 只负责翻译。
 *
 * 检查原则（对应 DESIGN §3）：
 *   · 拓宽（无损失）自动允许
 *   · 收窄（有损失）一律禁止 —— 有损转换必须写出方法名
 *   · 字面量的类型可以按**值**适配（`let x: f32 = 3.14`、`let x: u8 = 200`）
 */

#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 状态 */

typedef struct {
    const char *name;
    const char *cname;   /* 生成 C 的名字 —— 同层遮蔽时 `a` → `a__2`，见 §名字 */
    Type       *type;
    bool        mut;
    int         depth;   /* 词法深度：参数 = 0，函数体里的局部 = 1，每进一层块 +1。
                          * 逃逸检查就比这个数 —— 见 REFS.md §4 */
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

    Type *tI32, *tF64, *tBool;
    StructDef *sliceDef;/* prelude 里的 `slice<T>` 声明（视图协议）*/
    Type *tSliceU8;     /* 字符串字面量的类型：`slice<u8>` */
} Checker;

/* ---------------------------------------------------------------- 报错 */

static void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

static const char *typeStr(Checker *c, Type *t) {
    Buf b;
    bufInit(&b, c->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/* ------------------------------------------------- 生成 C 的名字（改名） */

/* **`let` 是命名，不是存储。** 所以同一层里 `let a = 1` / `let a = a + 1` 是合法的 ——
 * 第二个 `a` 只是**重新给这个名字一个含义**，不是写进原来那个地方（老的那个 `a`
 * 连同它的存储一起留在原地，谁指着它谁还指着它）。
 *
 * 但 C 不允许同一个块里重名 ⇒ 第二个以后的名字在**生成的 C** 里改叫 `a__2`。
 * extC 源码里的名字**不变** —— 改名是编译器内部的事，不许漏给用户看。
 *
 * 两条约束：
 *   ① 同一个函数内**单调不重复**（不复用 `a__2`）—— 生成的 C 读起来才确定，
 *      也不用去推敲 C 自己的块作用域规则；
 *   ② 还要避开**模块级**的名字（函数 / 结构体 / 全局）—— 否则局部变量会在生成的 C 里
 *      盖住函数名，`foo()` 就调不到了。 */
static bool moduleNameTaken(Module *m, const char *name) {
    for (size_t i = 0; i < m->funcs.len; i++)
        if (strcmp((*(FuncDef **)vecAt(&m->funcs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->structs.len; i++)
        if (strcmp((*(StructDef **)vecAt(&m->structs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->types.len; i++)
        if (strcmp((*(TypeDef **)vecAt(&m->types, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->globals.len; i++)
        if (strcmp((*(GlobalDef **)vecAt(&m->globals, i))->name, name) == 0) return true;
    return false;
}

static const char *cNameFor(Checker *c, const char *name) {
    NameUse *u = NULL;
    for (size_t i = 0; i < c->nameUses.len; i++) {
        NameUse *it = *(NameUse **)vecAt(&c->nameUses, i);
        if (strcmp(it->name, name) == 0) { u = it; break; }
    }
    int n = u ? u->count : 0;

    const char *cand;
    do {
        n++;
        cand = (n == 1) ? name : arenaPrintf(c->arena, "%s__%d", name, n);
        /* 也要避开 C 关键字（`let double = 3` 合法，但生成的 C 里不能叫 double）*/
    } while (moduleNameTaken(c->m, cand) || cIdentIsKeyword(cand));

    if (u) u->count = n;
    else {
        NameUse *fresh = (NameUse *)arenaAllocZero(c->arena, sizeof(NameUse));
        fresh->name = name;
        fresh->count = n;
        *(NameUse **)vecPush(&c->nameUses) = fresh;
    }
    return cand;
}

/* ---------------------------------------------------------------- 作用域 */

static void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
}

static void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
}

/* 声明一个绑定。
 *
 * **`shadow`**：能不能遮蔽同层已有的同名绑定？
 *   `let` ⇒ 可以（命名，不是存储 —— 见 §名字）
 *   `var` ⇒ 不可以（声明**存储**；同一层写两个同名的 `var`，几乎肯定是想写 `a = ...`）
 * 参数按 `var` 算（它本来就是可写的局部副本）。
 *
 * 返回新建的 Sym（`cname` 由调用者写回 AST —— 参数写回 Param，局部写回 Stmt）。 */
static Sym *declare(Checker *c, const char *name, Type *t, bool mut,
                    bool shadow, int line, int depth) {
    Scope *top = *(Scope **)vecAt(&c->scopes, c->scopes.len - 1);

    if (!shadow) {
        for (size_t i = 0; i < top->syms.len; i++) {
            Sym *old = *(Sym **)vecAt(&top->syms, i);
            if (strcmp(old->name, name) == 0) {
                ckError(c, line,
                        "`var` declares storage, so two `var`s with the same name in one scope "
                        "is almost always a typo -- write `x = ...` to assign the existing one, "
                        "or `let x = ...` if you really mean a new name",
                        "`%s` is already declared in this scope", name);
                return old;
            }
        }
    }

    Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
    s->name = name;
    s->cname = cNameFor(c, name);
    s->type = t;
    s->mut = mut;
    s->depth = depth;
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
    return s;
}

static Sym *lookup(Checker *c, const char *name) {
    /* 局部作用域（从内往外）优先 —— 所以局部可以遮蔽全局 */
    for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *s = *(Scope **)vecAt(&c->scopes, i);
        /* **从后往前**扫：同一层里最新的 `let` 赢（遮蔽）。顺序反了的话
         * `let a = 1  let a = a + 1` 里的第二个 `a` 会解析回第一个 ✓ */
        for (size_t j = s->syms.len; j-- > 0; ) {
            Sym *sym = *(Sym **)vecAt(&s->syms, j);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    /* 全局（深度 0）。**不放 in scopes**：函数体要知道自己的深度是 1，
     * 而深度是按作用域层数算的 —— 给全局单开一层会把所有局部都推深一层。 */
    for (size_t i = 0; i < c->globals.len; i++) {
        Sym *sym = *(Sym **)vecAt(&c->globals, i);
        if (strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

/* ---------------------------------------------------------------- 查找 */

static FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

static FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}

/* TY_STRUCT 和 TY_GENERIC 都指向一个 StructDef */
static StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/* 方法只住在 struct 体内（定案 9）*/
static FuncDef *findMethod(Type *st, const char *name) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

static Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* 这个类型里（递归地）有没有 `ref`？
 * 有的话就不能零初始化 —— `ref` 不可为空，它没有「零值」。 */
/* 这个类型里有没有「进不去零值」的东西？`ref T` 没有零值，**含 ref 的聚合也没有**。
 *
 * ⚠️ 泛型实例必须**代入实参**再往下走：`option<i64>` 的 value 是 i64（有零值），
 * 但 `option<slice<u8>>` 的 value 是 slice（里面有 ref）⇒ 没有零值。
 * 不代入的话 `T` 是个类型参数、看着人畜无害，检查整条漏过去，
 * 最后在生成的 C 里露出 `__extc_reference_has_no_zero_value__`（真踩过）。 */
static bool typeContainsRef(TypeTable *tt, Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind == TY_ARRAY) return typeContainsRef(tt, t->inner);
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeContainsRef(tt, ttSubstitute(tt, ft, sp, sa))) return true;
    }
    return false;
}

/* 能取引用的东西：变量和字段 */
static bool isLvalue(Expr *e) {
    return e->kind == EX_IDENT || e->kind == EX_FIELD;
}

/* 拼一个 `slice<elem>`（视图协议来自 prelude）*/
static Type *sliceOf(Checker *c, Type *elem) {
    if (!c->sliceDef) return ttError(c->tt);
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Type **)vecPush(&args) = elem;
    return ttGeneric(c->tt, c->sliceDef, &args);
}

/* 造一个整数字面量节点。
 * 用途：`a[2..]` 省略的界由**编译器**补成字面量（见 EX_SLICE），
 * 这样 codegen 只要看到「两个界都是字面量」就知道范围能证明、无需运行时检查。 */
static Expr *intLit(Checker *c, long long v, int line) {
    Expr *e = exprNew(c->arena, EX_INT, line);
    e->u.ival = v;
    e->type = c->tI32;
    return e;
}

/* 这个表达式是不是一个字面整数？`-1` 也算 —— 它是 `EX_UN` 套 `EX_INT`，
 * 而负数界正是最容易写错的地方（`a[-1..n]` 必须当场报错，不能等到运行时）。 */
static bool asIntLit(Expr *e, long long *out) {
    if (!e) return false;
    if (e->kind == EX_INT) { *out = e->u.ival; return true; }
    if (e->kind == EX_UN && e->u.un.op && strcmp(e->u.un.op, "-") == 0 &&
        e->u.un.operand && e->u.un.operand->kind == EX_INT) {
        *out = -e->u.un.operand->u.ival;
        return true;
    }
    return false;
}

/* 这个表达式是不是一个「地方」（place）—— 变量 / 字段 / 索引组成的链？ * 只有切**固定数组**时才要求（因为要取元素的地址）。
 * 理由很实在：`f()[1..2]` 会切到函数返回值的临时存储上，那是**指向已死对象**的视图。
 * `"abcdef"[1..3]` 不受这条限制 —— 它切的是 slice 值（指针+长度），字节有静态生命期。
 * 完整的逃逸检查在 week-4，这条先挡住最脏的一种。 */
static bool isPlace(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlace(e->u.field.obj);
    case EX_INDEX: return isPlace(e->u.index.obj);
    case EX_SLICE: return isPlace(e->u.slice.obj);
    default:       return false;
    }
}

/* 一个「地方」的**根** —— 穿过字段和下标，找到最里面那个变量。
 *
 * `let` 管的是**根**：`p.x = 1` / `a[i] = 1` / `v[0] = 1` 的根都是那个变量。
 * 只查裸标识符（`v = ...`）是不够的 —— `let p: point; p.x = 1` 会整条漏过去，
 * 而那正是「忘了写 var」最常犯的形态。
 *
 * ⚠️ 这条规则是**浅的**：它管名字，不管数据。把 `let` 视图拷给一个 `var`
 * （或者传进函数）之后，那边照样能写同一块内存。要管到数据层得让可变性进类型
 * （Rust 的 `&` / `&mut`），那是 week-4 引用规则的范围。见 DECISIONS 定案 28。 */
static Sym *placeRoot(Checker *c, Expr *e) {
    while (e) {
        if (e->kind == EX_IDENT)  return lookup(c, e->u.ident.name);
        if (e->kind == EX_FIELD)  { e = e->u.field.obj; continue; }
        if (e->kind == EX_INDEX)  { e = e->u.index.obj; continue; }
        if (e->kind == EX_SLICE)  { e = e->u.slice.obj; continue; }
        return NULL;
    }
    return NULL;
}

/* 这个「地方」可不可写？（取引用时决定 `mut ref T` 还是 `ref T`）
 *
 * 三条来源，正好对应「一个值是从哪拿到的」：
 *   ① 绑定：`var` 可写 / `let` 只读
 *   ② 参数：`mut ref T` 可写 / `ref T` 只读（类型里写着）
 *   ③ 字段/元素：看它的根（跟赋值查的是同一个东西）
 * 见 DECISIONS「引用语义定案」。 */
static bool pathHasReadonlyRef(Expr *e);

static bool isWritablePlace(Checker *c, Expr *e) {
    if (!e) return false;
    /* 路上有只读引用 ⇒ 写不进去 */
    if (pathHasReadonlyRef(e)) return false;
    /* 表达式本身就是引用：`mut ref` 才可写 */
    if (e->type && e->type->kind == TY_REF) return e->type->mut;
    /* 表达式是视图：**视图自己的类型**必须可写（`mut slice<T>`）。
     * 字符串字面量也走这条 —— 它的类型是只读视图 ⇒ 不可写 ✓ */
    if (e->type && e->type->kind == TY_GENERIC && ttIsViewType(e->type) && !e->type->mut)
        return false;
    /* 绑定 + 字段/元素：走到根，根必须是 `var` */
    Sym *root = placeRoot(c, e);
    return root && root->mut;
}

/* 往一个「地方」里写之前，先看它的根是不是 `var`。
 * 返回 true = 已经报过错（调用点直接放弃）。 */
/* ---------------------------------------------------------------- 逃逸检查
 *
 * DESIGN §2 的「唯一引用规则」：
 *     若引用 `r` 指向值 `v`，则 depth(r) ≥ depth(v)。
 * 人话：**引用不能活得比被指对象长。**
 *
 * 深度是**纯词法属性**（参数 = 0，函数体 = 1，每进一层块 +1），比两个整数就行 ——
 * 不需要生命周期标注。**这正是「Rust 的安全 + 不写生命周期」的落点。**
 *
 * 今天查三条，第四条故意不查（要跨函数分析）：
 *   ① 返回的引用/视图：被指对象必须在**参数或静态数据**里（深度 0）
 *   ② 局部变量初始化：被指对象不能比它深
 *   ③ 给字段/元素赋值（`o.f = v`）：被指对象不能比它深
 *   ④ ⬜ 把引用传给函数、函数把它存起来 —— 需要「传染性」分析，见 REFS.md §6
 */

static int maxInt(int a, int b) { return a > b ? a : b; }

/* 这个「地方」根上的绑定有多深？（参数、非绑定 = 0） */
static int placeDepth(Checker *c, Expr *e) {
    Sym *root = placeRoot(c, e);
    return root ? root->depth : 0;
}

/* 表达式里的引用**指向的活物**有多深？
 * **类型里没有引用就直接 0** —— 纯值拷贝永远不会悬垂，
 * 不然后面 `let x: i32 = <深层局部>` 会被误报。 */
static int exprRefDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    if (!typeContainsRef(c->tt, e->type)) return 0;
    if (e->refDepth) return e->refDepth;              /* 算过就缓存 */

    int d = 0;
    switch (e->kind) {
    case EX_REF:
        d = placeDepth(c, e->u.ref.operand);
        break;
    case EX_SLICE:
        d = placeDepth(c, e->u.slice.obj);
        break;
    case EX_IDENT: case EX_FIELD: case EX_INDEX:
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
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        break;
    default:
        d = 0;
        break;
    }
    e->refDepth = d;
    return d;
}

/* 把一个值放进「深度 at」的地方：它里面的引用活得够不够久？ */
/* 这个值里的引用是不是「**从外面借来的**」？—— 参数来的，或者函数调用回来的。
 *
 * 借来的东西**不能存进比这次调用活得更长的地方**（全局、或者别的参数指向的对象）：
 * 编译器**不知道它的真实寿命** —— 它可能指向调用者帧里比本函数更内层的局部。
 * 这就是 BOOTSTRAP §8 的 ④（参数洗白），跟「调用结果深度 = max(实参)」是同一件事的两半。 */
static bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what);

static bool isGlobalSym(Checker *c, Sym *s) {
    for (size_t i = 0; i < c->globals.len; i++)
        if (*(Sym **)vecAt(&c->globals, i) == s) return true;
    return false;
}

static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    if (!typeContainsRef(c->tt, e->type)) return false;
    switch (e->kind) {
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* 参数：深度 0 且不是全局 ⇒ 借来的 ✓
         * 全局 / 静态：也深度 0，但**谁都存得下它** ✓ */
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    case EX_REF:   return exprBorrowed(c, e->u.ref.operand);
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
    default: return false;      /* 字面量 / 全局 / alloc 出来的是本帧的 ✓ */
    }
}

/* 把一个值**存进**某个地方之前的全部检查（深度 + 借来的东西）。 */
static bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    bool bad = checkEscape(c, val, placeDepth(c, target), line, "this assignment");
    if (placeDepth(c, target) == 0 && exprBorrowed(c, val)) {
        ckError(c, line,
                "A borrowed value may not be stored where it outlives the call: its real "
                "lifetime is unknown here. Copy it, or store it into a local of this frame.",
                "cannot store a borrowed value into something that outlives this call");
        return true;
    }
    return bad;
}

static bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    if (!val) return false;
    int d = exprRefDepth(c, val);
    if (d <= at) return false;
    ckError(c, line,
            "A reference may not outlive what it points to. Borrow from a parameter "
            "(depth 0) or copy the data instead.",
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
static bool pathHasReadonlyRef(Expr *e) {
    for (Expr *x = e; x; ) {
        if (x->type && x->type->kind == TY_REF && !x->type->mut) return true;
        if (x->kind == EX_FIELD) { x = x->u.field.obj; continue; }
        if (x->kind == EX_INDEX) { x = x->u.index.obj; continue; }
        if (x->kind == EX_SLICE) { x = x->u.slice.obj; continue; }
        break;
    }
    return false;
}

static bool requireMutable(Checker *c, Expr *e, int line, const char *what) {
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
static Type *viewElemOf(Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return NULL;
    if (strcmp(t->sdef->name, "slice") != 0) return NULL;
    if (t->targs.len != 1) return NULL;
    return *(Type **)vecAt(&t->targs, 0);
}

/* C 原生就能比的类型：数值 / bool / 枚举。
 * `str` **不在**这里 —— 它的 `==` 会退化成指针比较（陷阱），必须有 eq 才行。 */
static bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    return ttBase(t)->kind == TY_ENUM;
}

/* 找类型上定义的运算符方法。
 * `!=` 是特例：没定义 `!=` 就退回用 `==` 取反（codegen 那边会自动取反）。*/
static FuncDef *findOp(Type *b, const char *sym, const char *fallback) {
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
static void checkOperatorSig(Checker *c, FuncDef *f) {
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
static bool typeSupportsEq(Type *t, const char *op) {
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
static Type *checkExpr(Checker *c, Expr *e);
static Type *checkTryInner(Checker *c, Expr *e);

/* **值位置**：把 `ref T` 当 `T` 用（形状 3「值位置自动解引用」）。
 *
 * 跟 `checkExpr` 的分工：
 *   `checkValue` = 这里要的是**值** ⇒ `p` 就是 p 指向的东西，打 `deref` 标记
 *   `checkExpr`  = 这里要的是**地方/引用本身** ⇒ 赋值目标、`ref x` 的操作数、
 *                  字段/下标/切片的底、方法接收者（要取地址）
 *
 * 权限（能不能写）不在这里管 —— 那是 `ref` / `mut ref` 的事。 */
static Type *checkValue(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    /* `ref x` 是**显式**要一个引用 ⇒ 不再自动解引用 ——
     * 否则就等于「解掉自己刚取的那个引用」，纯属自相矛盾。
     * 所以 `let r = ref n` 得到的是引用；而 `let y = r` 得到的是 r 指向的**值**。 */
    /* `ref x` 和 `alloc<T>(n)` 都是**显式**要一个引用 ⇒ 不再自动解引用 ——
     * 否则就是「解掉自己刚取/刚要来那个引用」，纯属自相矛盾。 */
    if (t && t->kind == TY_REF && e->kind != EX_REF && e->kind != EX_GENCALL) {
        e->deref = true;
        return t->inner;
    }
    return t;
}

/* 实参 / 字段初值：**期望类型是引用时不自动解引用** ——
 * 那里是「放一个引用进去」（`{ data: ref n, ... }`、`f(ref c)`），不是取它的值。
 * 其余情况按值位置处理（形状 3）。 */
static Type *checkInto(Checker *c, Type *want, Expr *e) {
    Type *got = checkExpr(c, e);
    if (got && got->kind == TY_REF && (!want || want->kind != TY_REF)) {
        e->deref = true;
        return got->inner;
    }
    return got;
}

static Type *checkMaybeTry(Checker *c, Expr *e) {
    if (e && e->kind == EX_TRY) return checkTryInner(c, e);
    return checkValue(c, e);
}

/* 泛型里的 `==` 推迟到实例化才检查 —— 这里记一笔 */
typedef struct { Expr *node; StructDef *owner; const char *op; } EqCheck;

/* prelude 里两个「有真实语义」的容器，按名字 + 实参个数认（它们是**保留定义**，
 * 用户不能重定义，所以按名字认是安全的）。`?` 要用到它们的标签字段名 ——
 * 跟视图协议 `data` + `len` 是同一种分工：**语言认识协议，库提供结构**。 */
static bool isProtoType(Type *t, const char *name, size_t nargs) {
    return t && t->kind == TY_GENERIC && t->sdef &&
           strcmp(t->sdef->name, name) == 0 && t->targs.len == nargs;
}

/* `e?` —— 失败就顺着往上抛。
 * 只在三处语句位置上合法（C 没有语句表达式，得展开成语句），
 * 所以这个函数**只**从 checkStmt 的那三处调用；checkExprInner 遇到 EX_TRY 会报错。 */
static Type *checkTryInner(Checker *c, Expr *e) {
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

/* ---------------------------------------------------------------- 小工具 */

static bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

static bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

static bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

static bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    /* 定案 11：无载荷枚举自动有名字文本 */
    if (b->kind == TY_ENUM) return true;
    /* struct / 泛型实例 / 数组都由编译器生成 <Type>_debug 递归打印 */
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC || b->kind == TY_ARRAY;
}

/* 字面量的类型按**值**适配目标类型（DESIGN §5 的「字面量类型推导」）。 */
static bool literalFits(Expr *e, Type *want) {
    Type *w = ttBase(want);
    if (!w) return false;
    if (ttIsError(w)) return true;

    if (e->kind == EX_INT) {
        int bits = ttIntBits(w);
        if (bits > 0) {
            long long v = e->u.ival;
            if (bits == 64) return ttIntSigned(w) || v >= 0;
            if (ttIntSigned(w)) {
                long long lo = -(1LL << (bits - 1));
                long long hi = (1LL << (bits - 1)) - 1;
                return v >= lo && v <= hi;
            }
            if (v < 0) return false;
            return (unsigned long long)v <= ((1ULL << bits) - 1);
        }
        return ttIsFloat(w);                    /* 整数字面量给浮点变量 */
    }

    if (e->kind == EX_FLOAT) return ttIsFloat(w);
    if (e->kind == EX_BOOL)  return ttIs(w, "bool");
    return false;
}

/* 裸 `{}` 从上下文拿类型。只在上下文能唯一确定类型的地方成立。 */
static void adoptContextType(Expr *e, Type *want) {
    if (!e || !want) return;
    Type *w = ttBase(want);
    if (!w) return;
    /* 把上下文类型**寄放**在 e->type 上（checkExpr 之后会被覆盖成同一个类型）。
     * 泛型实例没有名字可查、数组字面量也不知道长度，所以必须走这条路。 */
    if (e->kind == EX_ARRAYLIT) { e->type = w; return; }
    if (w->kind != TY_STRUCT && w->kind != TY_GENERIC) return;
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) e->type = w;
}

static bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
    if (ttIsError(want) || ttIsError(got)) return true;

    /* ⚠️ `ref T` 两边都必须真是引用。
     *
     * 这个坑很隐蔽：字面量适配那条路会 `ttBase(want)` 把 ref 抹掉，
     * 于是 `p = 5`（p 是 `ref i64`）被判成「5 能放进 i64」而放过去 ——
     * 生成的 C 是 `int64_t * p; p = 5;`，只有 gcc 会抱怨一句
     * `makes pointer from integer without a cast`。**extC 必须在类型层挡住它。** */
    if (want->kind == TY_REF && got->kind != TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` can only be assigned another reference; "
                "to write through it, assign to what it points to (`p.field = ...`, `p[i] = ...`)",
                "%s expects `%s`, found `%s` -- a value is not a reference",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }
    if (want->kind != TY_REF && got->kind == TY_REF) {
        ckError(c, node ? node->line : 0,
                "a `ref` is not a value; extC has no implicit dereference",
                "%s expects `%s`, found `%s` -- dereference it first",
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (ttEquals(want, got)) return true;

    /* **降级**：可写的可以当只读的用（能写当然能读）—— 单向、永远安全，自动允许。
     * 反过来不行：那是在要写权限，必须显式写 `mut`。
     * 「安全是默认」的直接体现：往安全的方向收窄不用打招呼。
     * 两种东西都适用：引用（`mut ref T` → `ref T`）和视图（`mut slice<T>` → `slice<T>`）。 */
    if (got->mut && !want->mut && want->kind == got->kind) {
        if (want->kind == TY_REF && ttEquals(want->inner, got->inner)) return true;
        if (want->kind == TY_GENERIC &&
            ttEquals(want, ttViewReadonly(c->tt, got))) return true;
    }
    if (ttCanWiden(got, want)) return true;
    if (node && literalFits(node, want)) return true;

    /* 整数字面量放不下，跟「类型不匹配」是两回事，报错要分开。
     * （浮点字面量给整数变量属于有损转换，走下面的通用消息。） */
    if (node && node->kind == EX_INT && ttIsInteger(want)) {
        ckError(c, node->line, NULL,
                "%s: literal `%lld` does not fit in `%s`",
                what, node->u.ival, typeStr(c, want));
        return false;
    }

    ckError(c, node ? node->line : 0,
            "extC only widens implicitly (lossless); a lossy conversion must be written out",
            "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
    return false;
}

static void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "conditions must be `bool` -- extC has no implicit truthiness",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

/* ---------------------------------------------------------------- 表达式 */

static Type *checkExpr(Checker *c, Expr *e);

static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;
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

    /* 字面量按值适配另一边的类型：`u32 + 1` 里 `1` 就是 u32 */
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

/* 位运算：只许整数（`&` `|` `^` `<<` `>>`）。
 * 结果类型跟算术走同一套（不拓宽就报错）——
 * 特意**不**在这里抄一份类型规则，免得两处规则漂开。 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/* `ref T` 上的算术/比较是**没有意义**的：C 里那是指针算术，语义完全不是用户想的。
 * extC 宁可报错，也不给一个「看起来对、其实在挪指针」的答案。 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:   return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_IDENT: {
            Sym *s = lookup(c, e->u.ident.name);
            if (!s) {
                ckError(c, e->line, "every name must be declared first (extC has no globals yet)",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* 名字的**解析**在这里定格 ⇒ 代码生成直接印 `cname`。
             * 遮蔽过的名字（`a` vs `a__2`）就靠这一行分开 ✓ */
            e->u.ident.cname = s->cname;
            return s->type;
        }

        case EX_BIN: {
            Type *lt = checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

            if (isLogicOp(op)) {
                expectBool(c, lt, e->u.bin.left);
                expectBool(c, rt, e->u.bin.right);
                return c->tBool;
            }
            if (isCmpOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return c->tBool;

                bool isEqOp = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);

                /* ---- `==` / `!=`：内建原生比；struct 走 eq 方法 ---- */
                if (isEqOp && ttEquals(lt, rt)) {
                    if (cmpIsNative(lt)) return c->tBool;

                    /* 数组：`==` 由**编译器派生**（数组类型用户写不出来，没法用 fn == 实现）*/
                    if (lt->kind == TY_ARRAY) {
                        if (!typeSupportsEq(lt->inner, op))
                            ckError(c, e->line,
                                    "An array's `==` is derived by the compiler, "
                                    "so its elements have to be comparable.",
                                    "`%s` cannot be compared: its element type `%s` does not define `==`",
                                    typeStr(c, lt), typeStr(c, ttBase(lt)->inner));
                        return c->tBool;
                    }

                    /* 泛型参数 → 推迟到实例化再检查（规则 2） */
                    if (lt->kind == TY_PARAM) {
                        e->needEq = true;
                        if (c->curFunc && c->curFunc->owner) {
                            EqCheck *ec = (EqCheck *)arenaAllocZero(c->arena, sizeof(EqCheck));
                            ec->node = e;
                            ec->owner = c->curFunc->owner;
                            ec->op = op;
                            *(EqCheck **)vecPush(&c->eqChecks) = ec;
                        }
                        return c->tBool;
                    }

                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (!sd) {
                        ckError(c, e->line, NULL,
                                "`%s` does not support `%s`", typeStr(c, lt), op);
                        return c->tBool;
                    }

                    FuncDef *m = findOp(b, op, strcmp(op, "!=") == 0 ? "==" : NULL);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn ==(self: ref %s, other: %s) -> bool { ... }",
                                  sd->name, sd->name, sd->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==`, so it cannot be compared",
                                sd->name);
                        return c->tBool;
                    }

                    Vec *sp = NULL, *sa = NULL;
                    if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                    (void)sp; (void)sa;
                    e->func = m;        /* codegen 用它生成 `Type_eq(&a, &b)` */
                    return c->tBool;
                }

                /* ---- 其余比较：只对数值有意义 ---- */
                if (ttIsNumeric(lt) && ttIsNumeric(rt) &&
                    (ttCanWiden(lt, rt) || ttCanWiden(rt, lt))) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;

                ckError(c, e->line, isEqOp ? "only numbers, `bool`, enums, and structs that define `==` can be compared" : NULL,
                        "cannot compare `%s` with `%s`", typeStr(c, lt), typeStr(c, rt));
                return c->tBool;
            }
            if (isBitOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return ttError(tt);
                if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
                if (!ttIsInteger(lt) || !ttIsInteger(rt)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `%s` to `%s` and `%s`",
                            op, typeStr(c, lt), typeStr(c, rt));
                    return ttError(tt);
                }
            }
            /* 同样的坑：`ttIsNumeric` 会把 `ref T` 抹成 `T`，
             * 于是 `p + 1`（p 是 `ref i64`）被当成数字运算放过去，
             * 生成的 C 却是**指针算术** `p + 1` —— 静默地做完全不是那个意思的事。 */
            if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
            /* 结果类型交给算术那一套统一处理（含字面量适配）——不在这里抄第二份规则 */
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkValue(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
            /* `~` 按位取反只对整数有意义 */
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
            /* 先看是不是枚举变体：`Status.warn`
             * （`Status` 不是变量，而是一个 type 名字）*/
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
                        /* 改写成枚举值节点，codegen 直接用 */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        return et;
                    }
                }
            }

            Type *bt = ttBase(checkExpr(c, e->u.field.obj));
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
                bufPrintf(&note, "fields of %s:", sd->name);
                for (size_t i = 0; i < sd->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", sd->name, e->u.field.name);
                return ttError(tt);
            }
            e->field = fd;
            /* 字段的类型里可能有泛型参数 —— 用接收者的实参替换掉 */
            if (bt->kind == TY_GENERIC)
                return ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs);
            return fd->type;
        }

        case EX_INDEX: {
            Type *ot = checkExpr(c, e->u.index.obj);
            Type *it = checkValue(c, e->u.index.index);
            if (ttIsError(ot) || ttIsError(it)) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, NULL,
                        "cannot index a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            if (!ttIsInteger(it)) {
                ckError(c, e->line, "An index must be an integer.",
                        "index must be an integer, found `%s`", typeStr(c, it));
                return ttError(tt);
            }
            return elem;
        }

        case EX_SLICE: {
            /* `a[lo..hi]` —— 只读视图。lo / hi 可以为空（前端 / 后端省略）*/
            Type *ot = checkExpr(c, e->u.slice.obj);
            if (ttIsError(ot)) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, "Only a fixed array or a view can be sliced.",
                        "cannot slice a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }

            /* 切固定数组要取元素的地址 ⇒ 底必须是个「地方」。
             * 切 slice 只是对值做指针算术，不需要（所以字符串字面量可以切）。 */
            if (ob->kind == TY_ARRAY && !isPlace(e->u.slice.obj)) {
                ckError(c, e->line, "Slicing takes the address of an element, so the base must be a place.",
                        "cannot slice a temporary value; `%s` needs a variable, a field or an index",
                        typeStr(c, ot));
                return ttError(tt);
            }

            /* 底是固定数组 ⇒ 长度是**编译期常数**，省略的界直接补成字面量：
             *     a[2..] → a[2..15]      a[..] → a[0..15]
             * 于是 codegen 看到的是「两个界都是字面量」，能生成**没有检查**的代码（P）。 */
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

            /* 编译期能证明的错误**当场报**，一个都不留到运行时。
             * 每个界单独看：越界的字面量永远错，跟另一个界是什么无关。 */
            if (ob->kind == TY_ARRAY) {
                long long n = (long long)ob->asize;
                Expr *lp = e->u.slice.lo, *hp = e->u.slice.hi;
                long long lv = 0, hv = 0;
                bool lOk = asIntLit(lp, &lv);
                bool hOk = asIntLit(hp, &hv);

                /* 每个界单独看：越界的字面量永远错，跟另一个界是什么无关。 */
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
            /* 视图的**可写性从切出来的源头继承**：
             *   `var a` / `mut ref` 参数 ⇒ `mut slice<T>`
             *   `let a` / 字符串字面量 / 只读视图 ⇒ `slice<T>`
             * 这就是「一个视图类型」能同时表达两种权限的办法 ——
             * 不用像 Rust 那样给切片造两个类型。 */
            return ttViewMut(tt, sliceOf(c, elem),
                             isWritablePlace(c, e->u.slice.obj));
        }

        case EX_ARRAYLIT: {
            /* 类型从上下文来（`var a: [3]i32 = [...]`）或者从元素推 */
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
            /* **取哪种引用，看这个「地方」可不可写**：
             *   `var` 变量 / `mut ref` 参数 ⇒ `mut ref T`
             *   `let` 变量 / `ref` 参数     ⇒ `ref T`（只读）
             *
             * 只读借用**随时可以取** —— 那正是借用存在的理由。
             * （以前 `ref` 只有可变一种，所以对 `let` 取引用被一律拒绝，
             *   连「只是想读一下」都写不出来。） */
            Type *r = ttRef(tt, ot);
            r->mut = isWritablePlace(c, op);
            return r;
        }

        case EX_TRY:
            /* `?` 是**语句级**的转发：要展开成「求值一次 + 判断 + return」。
             * C 没有语句表达式，所以它只在这三处合法：
             *     let x = e?   /   x = e?   /   return e?
             * 别的地方（比如 `f(e? + 1)`）要报清楚，而不是生成编不过的 C。 */
            ckError(c, e->line,
                    "`?` has to expand into statements (evaluate once, test, return), "
                    "so it only fits where a whole statement can be rewritten.",
                    "`?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?`");
            return ttError(tt);

        case EX_ASSOC: {            /* `option<i64>::some(x)` —— 关联函数（struct 体内不带 `self` 的函数）。
             * 类型实参**写全**，不从参数或上下文倒推（定案 27：显式优于推导）。 */
            Type *raw = typeNamed(c->arena, e->u.assoc.typeName);
            raw->targs = e->u.assoc.targs;
            Type *t = ttResolve(tt, c->ctx, raw, e->line, c->curParams);
            if (ttIsError(t)) return ttError(tt);
            e->assocOwner = t;

            StructDef *sd = structOf(t);
            FuncDef *f = NULL;
            if (sd) {
                for (size_t i = 0; i < sd->methods.len; i++) {
                    FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                    if (m->isAssoc && strcmp(m->name, e->u.assoc.name) == 0) { f = m; break; }
                }
            }
            if (!f) {
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "associated functions of %s:", sd->name);
                    bool any = false;
                    for (size_t i = 0; i < sd->methods.len; i++) {
                        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                        if (!m->isAssoc) continue;
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
            e->func = f;

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

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
                checkAssignable(c, pt, at, a, "argument");
            }
            return f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);
        }

        case EX_GENCALL: {
            /* 泛型调用 —— 目前**只有内置原语**用它：`alloc<i32>(n)`。
             * 它向当前函数帧的 arena 要 `n` 个 T 的地方，返回可写引用。
             *
             * 为什么它是原语而不是库函数：**arena 本身必须由编译器生成**
             * （库要用它来分配自己 ⇒ 不能由库提供）。见 BOOTSTRAP 规则二。 */
            if (strcmp(e->u.gencall.name, "alloc") != 0) {
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive (only `alloc<T>(n)` is)",
                        e->u.gencall.name);
                return ttError(tt);
            }
            if (e->u.gencall.targs.len != 1) {
                ckError(c, e->line, NULL, "`alloc` needs exactly one type argument, e.g. `alloc<i32>(4)`");
                return ttError(tt);
            }
            Type *elem = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, 0),
                                   e->line, c->curParams);
            if (ttIsError(elem)) return ttError(tt);
            /* 把**解析后的**类型写回去 —— codegen 看的是 targs，不是局部变量 */
            *(Type **)vecAt(&e->u.gencall.targs, 0) = elem;
            if (e->u.gencall.args.len != 1) {
                ckError(c, e->line, NULL, "`alloc` takes one argument (how many elements)");
                return ttError(tt);
            }
            Expr *n = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *nt = checkValue(c, n);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, n->line, NULL, "the element count must be an integer, found `%s`",
                        typeStr(c, nt));
                return ttError(tt);
            }

            /* 这块内存活到**函数返回**（arena = 函数帧）⇒ 深度 1。
             * 于是「返回一块刚 alloc 的内存」会被逃逸检查拦住 ✓ 正确 ——
             * 想把它交出去，就得让调用者提供 buffer/arena。见 REFS.md §6。 */
            e->refDepth = 1;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* 刚分配的地方当然可写 */
            return r;
        }

        case EX_ENUMVAL:
            return ttFromName(tt, e->u.enumval.typeName);

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkValue(c, a);   /* 打印的是**值** ⇒ 解引用 ✓ */
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            FuncDef *f = findFunc(c, name);
            if (!f) {
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            e->func = f;

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkInto(c, p->type, a);

                /* T3：形参是 `ref T` 时，值必须在调用点显式写 `ref` ——
                 * 「这里传的是引用不是拷贝」要让读代码的人一眼看见（P′）。
                 * 实参本身就是引用的话，直接传即可。*/
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
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_METHOD: {
            /* 方法只住在 struct 体内 —— 按接收者的类型去找 */
            Type *recvT = checkExpr(c, e->u.method.recv);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            if (!f) {
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "methods of %s:", sd->name);
                    if (sd->methods.len == 0) bufPuts(&note, " (none)");
                    for (size_t i = 0; i < sd->methods.len; i++)
                        bufPrintf(&note, " %s",
                                  (*(FuncDef **)vecAt(&sd->methods, i))->name);
                    bufPuts(&note, "; methods must be declared inside their `struct`");
                } else {
                    bufPuts(&note, "only struct values have methods");
                }
                ckError(c, e->line, bufCstr(&note), "no method `%s` on `%s`",
                        e->u.method.name, typeStr(c, rb ? rb : recvT));
                return ttError(tt);
            }
            e->func = f;

            /* 方法要**可写借用**（`self: mut ref T`）⇒ 接收者必须可写。
             * 这是「签名不说实话」的另一半：光看调用点 `x.bump()` 看不出它会不会改 x，
             * 而 `self: mut ref` 让**签名说了**，这里就把它落实。 */
            {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type->kind == TY_REF && selfP->type->mut &&
                    requireMutable(c, e->u.method.recv, e->line, "call a method that writes"))
                    return ttError(tt);
            }

            /* 接收者是泛型实例时，方法签名里的 T 要换成实参 */
            StructDef *msd = structOf(rb);
            Vec *sp = NULL, *sa = NULL;
            if (rb && rb->kind == TY_GENERIC && msd) {
                sp = &msd->typeParams;
                sa = &rb->targs;
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
                checkAssignable(c, pt, at, a, "argument");
            }
            return rt;
        }

        case EX_STRUCTLIT: {
            /* 类型要么来自字面量里的名字，要么来自上下文（泛型实例只能靠上下文） */
            Type *st = e->u.lit.name ? ttFromName(tt, e->u.lit.name) : e->type;

            if (e->u.lit.name && (!st || ttIsError(st))) {
                ckError(c, e->line, NULL, "unknown struct `%s`", e->u.lit.name);
                return ttError(tt);
            }
            StructDef *sd = structOf(st);
            if (!sd) {
                ckError(c, e->line,
                        "a bare `{}` works only where the context pins the type down: "
                        "`var x: T = {}` / `return {}` / `f({})`",
                        "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            if (st->kind == TY_STRUCT && sd->typeParams.len > 0) {
                ckError(c, e->line, "a generic needs explicit type arguments, e.g. `Pair<i32, i32> { ... }`",
                        "`%s` is generic; type arguments cannot be inferred here", sd->name);
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "fields of %s:", sd->name);
                    for (size_t k = 0; k < sd->fields.len; k++)
                        bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, k))->name);
                    ckError(c, fi->value->line, bufCstr(&note),
                            "struct `%s` has no field `%s`", sd->name, fi->name);
                    continue;
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

            /* 省略的字段靠「零值」补齐 —— 但 `ref` 没有零值。
             * 不管的话会生成 `.field = 0`，也就是一个**空引用**，
             * 而语言层明明说 `ref` 不可为空。（跟 str 那次是同一类洞。）*/
            for (size_t i = 0; i < sd->fields.len; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
                bool given = false;
                for (size_t k = 0; k < e->u.lit.inits.len && !given; k++)
                    given = strcmp((*(FieldInit **)vecAt(&e->u.lit.inits, k))->name, fd->name) == 0;
                if (given) continue;

                Type *ft = fd->type;
                if (st->kind == TY_GENERIC)
                    ft = ttSubstitute(tt, ft, &sd->typeParams, &st->targs);
                if (typeContainsRef(tt, ft))
                    ckError(c, e->line,
                            "`ref` has no default value (it is a non-nullable reference)",
                            "field `%s` must be given explicitly", fd->name);
            }
            return st;
        }
    }
    return ttError(tt);
}

static Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

/* ---------------------------------------------------------------- 语句 */

static void checkStmt(Checker *c, Stmt *s);

static void checkBlockBody(Checker *c, Stmt *block) {
    pushScope(c);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i));
    popScope(c);
}

static void checkStmt(Checker *c, Stmt *s) {
    switch (s->kind) {
        case ST_VAR: {
            /* 局部变量声明的类型标注也要解析（parser 只造「类型名」） */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* 没有初始化式 ⇒ 零初始化（定案 8）。parser 保证此时必有类型标注。 */
            if (!s->u.var.init) {
                if (s->u.var.ann && typeContainsRef(c->tt, s->u.var.ann)) {
                    ckError(c, s->line,
                            "`ref` is a non-nullable reference, so it has no zero value -- "
                            "and neither does any struct that contains one",
                            "cannot zero-initialize `%s`: it contains a reference",
                            s->u.var.name);
                }
                s->type = s->u.var.ann ? s->u.var.ann : ttError(c->tt);
                Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                                   !s->u.var.mut, s->line, c->scopes.len);
                s->u.var.cname = sym->cname;
                return;
            }

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            /* `let x = e?` —— `?` 的合法位置之一。
             * 有类型标注时按**标注**决定要不要解引用（标的是引用 ⇒ 别解）；
             * 没标注时按值位置（形状 3）。 */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            else                               it = checkValue(c, s->u.var.init);
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            /* 逃逸②：初始化的引用不能指向比自己更深的局部 */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            s->u.var.cname = sym->cname;
            return;
        }

        case ST_ASSIGN: {
            Type *tt_ = checkExpr(c, s->u.assign.target);

            /* **目标是引用 ⇒ 写进去**（形状 3：`p = v` 写 p 指向的那个地方）。
             *
             * 「换指向」已经取消（见 DECISIONS 引用语义定案），所以给引用赋值
             * 必须对得上**被指的类型**；想改指向哪儿，只能重新声明一个绑定。 */
            if (tt_->kind == TY_REF) {
                if (requireMutable(c, s->u.assign.target, s->line, "write")) return;

                Expr *v = s->u.assign.value;

                /* **看右边是什么，就知道是哪件事 —— 歧义靠类型消掉，不靠禁用。**
                 *   右边是**值** `T`      ⇒ 写进它指向的地方   `*p = v`
                 *   右边是**引用** `ref T` ⇒ 换指向             `p = q`
                 *
                 * 这两件事类型不同、而且右边就在源码里看得见（P′）——
                 * 所以不需要像之前那样把换指向整个禁掉。 */
                adoptContextType(v, tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* 自然类型：引用保留 */

                if (vt0->kind == TY_REF) {
                    /* 换指向：类型要对得上（`mut ref` → `ref` 降级照旧允许），
                     * 而且新指向的东西不能活得比这个引用短。 */
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    checkEscape(c, v, placeDepth(c, s->u.assign.target),
                                s->line, "this reference");
                    return;
                }

                s->u.assign.target->deref = true;     /* 生成 `*(p) = v` */
                checkAssignable(c, tt_->inner, checkValue(c, v), v, "assignment");
                return;
            }

            adoptContextType(s->u.assign.value, tt_);
            Type *vt = checkMaybeTry(c, s->u.assign.value);

            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* 逃逸③：给字段/元素赋值时，被指对象不能比目标更深 */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (0) checkEscape(c, s->u.assign.value, placeDepth(c, s->u.assign.target),
                        s->line, "this assignment");
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            return;
        }

        case ST_IF:
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            if (s->u.ifs.elseBody) {
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
            }
            return;

        case ST_WHILE:
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            return;

        case ST_EXPR:
            /* `f()?` 单独成句 —— `?` 的第四个合法位置（最有用的那个：
             * 「这一步必须成功，否则整串失败」）*/
            checkMaybeTry(c, s->u.expr.expr);
            return;

        case ST_RETURN: {
            Type *want = c->curFunc ? c->curFunc->ret : NULL;
            if (!s->u.ret.value) {
                if (want && !ttIs(want, "void")) {
                    ckError(c, s->line, NULL, "`%s` must return a value of type `%s`",
                            c->curFunc->name, typeStr(c, want));
                }
                return;
            }
            if (!want) {
                checkExpr(c, s->u.ret.value);
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            /* `return e?` —— 表达式本身的值是**载荷**，而函数要交出的是外层类型，
             * 所以这里比的是「载荷能不能放进外层的载荷」（装回去由 codegen 做）。
             * 不能走下面那条 adoptContextType + checkAssignable：那会拿
             * option<i64> 去要求一个 i64。 */
            if (s->u.ret.value->kind == EX_TRY) {
                Type *vt = checkTryInner(c, s->u.ret.value);
                Type *wb = ttBase(want);
                if (wb && wb->kind == TY_GENERIC && wb->targs.len >= 1)
                    checkAssignable(c, *(Type **)vecAt(&wb->targs, 0), vt,
                                    s->u.ret.value, "return value");
                return;
            }

            adoptContextType(s->u.ret.value, want);
            Type *vt = checkInto(c, want, s->u.ret.value);   /* 期望是引用就别解 */
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            /* 逃逸①：返回的引用，被指对象必须在参数或静态数据里（深度 0）*/
            checkEscape(c, s->u.ret.value, 0, s->line, "this return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_BLOCK:            checkBlockBody(c, s);
            return;
    }
}

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

static void checkFunc(Checker *c, FuncDef *f) {
    FuncDef *savedFunc = c->curFunc;
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

    return !ctx->hasError;
}
