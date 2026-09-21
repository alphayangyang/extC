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
    Vec        narrowMarks; /* size_t —— 每个作用域进来时的 narrow.len（出作用域回退用）*/

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
    /* 收窄也是词法作用域的：进块时记下水位，出块时退回去 ✓ */
    *(size_t *)vecPush(&c->narrowMarks) = c->narrow.len;
}

static void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
    if (c->narrowMarks.len) {
        c->narrow.len = *(size_t *)vecAt(&c->narrowMarks, c->narrowMarks.len - 1);
        c->narrowMarks.len--;
    }
}

/* 实例化时把类型里的 TY_PARAM 换成实参 ✓（不在实例化里就是原样返回）*/
static Type *tsub(Checker *c, Type *t) {
    if (!c->substParams || !c->substArgs || !t) return t;
    return ttSubstitute(c->tt, t, c->substParams, c->substArgs);
}

/* 这个类型里"提到了类型参数"吗？提到了就不能现在下结论 ⇒ 推迟到实例化 ✓ */
static bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/* --------------------------------------------------- `?ref T` 的非空收窄 */
static Sym *lookup(Checker *c, const char *name);   /* 定义在后面 */

static bool isNarrowed(Checker *c, const char *cname) {
    if (!cname) return false;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) return true;
    return false;
}

static void pushNarrow(Checker *c, const char *cname) {
    if (!cname || isNarrowed(c, cname)) return;
    *(const char **)vecPush(&c->narrow) = cname;
}

/* 这个绑定又被赋成 null / 换了指向 ⇒ 之前的证明**作废**（收窄是栈式的，砍到它为止）*/
static void unNarrow(Checker *c, const char *cname) {
    if (!cname) return;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) { c->narrow.len = i; return; }
}

/* 这个条件式证明了**谁**非空？认不出来返回 NULL，`*whenTrue` 说证明在哪个分支里。
 *    `p != null` ⇒ then 分支里 p 非空
 *    `p == null` ⇒ **else** 分支里 p 非空（也就是 `if p == null { return }` 那个护栏形态）
 * 只有**可空引用**才算 —— 非空引用跟 null 比本身就是错（下面 checkBin 会报）。*/
static const char *narrowTarget(Checker *c, Expr *cond, bool *whenTrue) {
    if (!cond || cond->kind != EX_BIN) return NULL;
    const char *op = cond->u.bin.op;
    if (strcmp(op, "!=") != 0 && strcmp(op, "==") != 0) return NULL;
    Expr *var = NULL;
    if (cond->u.bin.right->kind == EX_NULL)      var = cond->u.bin.left;
    else if (cond->u.bin.left->kind == EX_NULL)  var = cond->u.bin.right;
    else return NULL;
    if (!var || var->kind != EX_IDENT) return NULL;
    Sym *sy = lookup(c, var->u.ident.name);
    if (!sy || !sy->type || sy->type->kind != TY_REF || !sy->type->nullable) return NULL;
    *whenTrue = (strcmp(op, "!=") == 0);
    return sy->cname;
}

/* 一个条件式能证明的**全部**非空事实。
 * `&&` 两边都要走 —— 能走到这个条件成立的分支，说明两边都成立过 ✓
 * （`if p != null && p.next != null { ... }` 就是这个形状；链越深越需要）*/
static void narrowFactsOf(Checker *c, Expr *cond) {
    if (!cond) return;
    if (cond->kind == EX_BIN && strcmp(cond->u.bin.op, "&&") == 0) {
        narrowFactsOf(c, cond->u.bin.left);
        narrowFactsOf(c, cond->u.bin.right);
        return;
    }
    bool whenTrue = false;
    const char *tg = narrowTarget(c, cond, &whenTrue);
    if (tg && whenTrue) pushNarrow(c, tg);
}

/* 这个块**一定会离开**（return / break / continue）？
 * 用来认护栏形态：`if p == null { return }` ⇒ 走到后面就说明 p 非空 ✓ */
static bool blockExits(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_BLOCK) {
        if (s->u.block.stmts.len == 0) return false;
        return blockExits(*(Stmt **)vecAt(&s->u.block.stmts, s->u.block.stmts.len - 1));
    }
    return s->kind == ST_RETURN || s->kind == ST_BREAK || s->kind == ST_CONTINUE;
}

/* 可空引用**不能**直接解 —— 先查 null（P′：不能证明的，语法上必须看得见）*/
static bool rejectNullableDeref(Checker *c, Type *t, Expr *node, const char *what) {
    if (!t || t->kind != TY_REF || !t->nullable) return false;
    ckError(c, node->line,
            "write `if p != null { ... }` (or `if p == null { return }`) first: inside"
            " that branch the compiler knows it is not null and generates no runtime check",
            "`%s` is a nullable reference (`?ref T`), so %s needs a non-null one",
            typeStr(c, t), what);
    return true;
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
    /* 引用型绑定的**默认**指向深度 = 槽位深度（保守 = 老行为）。
     * 声明/换指向的地方知道初始值是什么，会把这里**收紧**到真实深度 ✓
     * （比如 `var cur: ?ref node = head` ⇒ 0 —— 它指的是参数那边的东西）*/
    s->refDepth = (t && t->kind == TY_REF) ? depth : 0;
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

/* 枚举实例的载荷类型：泛型实例要**按实参替换**
 * （`type maybe<T> = | nothing | just(T)` 的实例 `maybe<i64>` 里，`just` 的载荷是 `i64`）✓ */
static Type *payloadType(TypeTable *tt, Type *et, Variant *v, size_t k) {
    Type *pt = *(Type **)vecAt(&v->types, k);
    if (et && et->kind == TY_ENUM && et->edef &&
        et->edef->typeParams.len > 0 && et->targs.len == et->edef->typeParams.len)
        pt = ttSubstitute(tt, pt, &et->edef->typeParams, &et->targs);
    return pt;
}

/* 这个枚举有**带载荷**的变体吗？（`| circle(f64)`）
 * 有的话：C 里它不再是 `enum` 而是 `struct { tag; union }` ⇒
 * ① 不能用 `==`（C 的 struct 不能比）② 打印只打变体名 */
static bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

/* 这个类型里（递归地）有没有 `ref`？
 * 有的话就不能零初始化 —— `ref` 不可为空，它没有「零值」。 */
/* 这个类型里有没有「进不去零值」的东西？`ref T` 没有零值，**含 ref 的聚合也没有**。
 *
 * ⚠️ 泛型实例必须**代入实参**再往下走：`option<i64>` 的 value 是 i64（有零值），
 * 但 `option<slice<u8>>` 的 value 是 slice（里面有 ref）⇒ 没有零值。
 * 不代入的话 `T` 是个类型参数、看着人畜无害，检查整条漏过去，
 * 最后在生成的 C 里露出 `__extc_reference_has_no_zero_value__`（真踩过）。 */
/* ⚠️ **两个不同的问题**（2026-09-20 修洞时拆开 —— 以前是同一个函数，于是漏了一个）：

 *   ① 「这个类型**能不能携带引用**」⇒ 看**所有**变体 / 所有字段  ⇒ `typeContainsRef`
 *   ② 「这个类型**有没有零值**」  ⇒ 带载荷枚举**只看 tag 0**（零值 = tag 0 + 载荷清零）
 *                                   ⇒ `typeLacksZeroValue`
 *
 * 混在一起的后果（真出过的洞）：`type box = | empty | holding(slice<u8>)` 问"有没有引用"时
 * 只看 tag 0 的 `empty` ⇒ 回答"没有" ⇒ `exprRefDepth` 早退返回 0 ⇒
 * **载荷的深度从来没算过** ⇒ `return box.holding(local[..])` 编过 ⇒ **悬垂 / UB** ✗
 */
static bool typeContainsRef(TypeTable *tt, Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind == TY_ARRAY) return typeContainsRef(tt, t->inner);
    /* 带载荷枚举：**任何一个**变体的载荷可能带 ref 就算 ⇒ 看全部 ✓ */
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsRef(tt, payloadType(tt, t, va, i))) return true;
        }
        return false;
    }
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

/* 「有没有零值」——`ref` 没有零值；含 ref 的聚合也没有。
 * 带载荷枚举：**零值 = tag 0 + 载荷清零** ⇒ **只看第一个变体** ✓
 * （所以 `| nothing | holding(slice<u8>)` 的 `var b: box` 合法，
 *   而 `| holding(slice<u8>) | nothing` 的不合法 —— **变体顺序有意义** ✓）*/
static bool typeLacksZeroValue(TypeTable *tt, Type *t) {
    if (!t) return false;
    /* `?ref T` 的零值就是 `null` —— 这正是它存在的理由：链表/树的 `next` 终于有零值了 ✓ */
    if (t->kind == TY_REF) return !t->nullable;
    if (t->kind == TY_ARRAY) return typeLacksZeroValue(tt, t->inner);
    if (t->kind == TY_ENUM && t->edef) {
        if (t->edef->variants.len == 0) return false;
        Variant *v0 = *(Variant **)vecAt(&t->edef->variants, 0);
        for (size_t i = 0; i < v0->types.len; i++)
            if (typeLacksZeroValue(tt, payloadType(tt, t, v0, i))) return true;
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeLacksZeroValue(tt, ttSubstitute(tt, ft, sp, sa))) return true;
    }
    return false;
}

/* `??` 生成的是 C 的三元运算符，**主体在 C 里出现两次** ⇒ 主体必须**没有副作用**，
 * 否则 `f() ?? -1` 会让 f() 跑两遍 ✗
 * 判据是保守的**语法**检查：树里只要出现调用（或 `?` / `??`）就不许 —— 不猜纯不纯 ✓
 * （宁可让他先 `let r = f()` 一行，也不偷偷改变调用次数。）*/
static bool repeatablePure(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL:
    case EX_IDENT:
    case EX_ENUMVAL:
        return true;
    case EX_FIELD: return repeatablePure(e->u.field.obj);
    case EX_SIGN:  return repeatablePure(e->u.sign.operand);
    case EX_DEREF: return repeatablePure(e->u.deref.operand);
    case EX_INDEX: return repeatablePure(e->u.index.obj) &&
                          repeatablePure(e->u.index.index);
    case EX_UN:    return repeatablePure(e->u.un.operand);
    case EX_BIN:   return repeatablePure(e->u.bin.left) &&
                          repeatablePure(e->u.bin.right);
    default:       return false;   /* 调用 / 方法 / `?` / `??` / 字面量构造 … 一律不行 */
    }
}

/* 能取引用的东西：变量和字段 */
static bool isLvalue(Expr *e) {
    /* `*p` 也是**地方**（p 指的那个地方）✓ */
    return e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_DEREF;
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
    /* `*p` 可写 ⇔ **引用本身**是 `mut ref`（可写性来自引用的类型）✓ */
    if (e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        return ot && ot->kind == TY_REF && ot->mut;
    }
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
static int exprRefDepth(Checker *c, Expr *e);

static int placeDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* `*p` 的"地方"不在本帧的哪个绑定里，而是 p 指的地方 ⇒
     * 它的寿命就是**那个引用的寿命** ✓（解引用不创造存储，只换了个入口）*/
    if (e->kind == EX_DEREF) return exprRefDepth(c, e->u.deref.operand);
    /* **引用型绑定**：这个"地方"在它指的那边 ⇒ 问的是被指对象的深度 ✓
     * （`var cur: ?ref node = head` 的槽位在本帧，但节点在外面）*/
    if (e->kind == EX_IDENT) {
        Sym *sy = lookup(c, e->u.ident.name);
        if (sy && sy->type && tsub(c, sy->type)->kind == TY_REF) return sy->refDepth;
        return sy ? sy->depth : 0;
    }
    /* 字段/元素住在**它所在的那个对象**里 ⇒ 跟着对象走 ✓ */
    if (e->kind == EX_FIELD) return placeDepth(c, e->u.field.obj);
    if (e->kind == EX_INDEX) return placeDepth(c, e->u.index.obj);
    Sym *root = placeRoot(c, e);
    return root ? root->depth : 0;
}

/* 表达式里的引用**指向的活物**有多深？
 * **类型里没有引用就直接 0** —— 纯值拷贝永远不会悬垂，
 * 不然后面 `let x: i32 = <深层局部>` 会被误报。 */
static int exprRefDepth(Checker *c, Expr *e) {
    if (!e) return 0;
    /* 早退的唯一理由：**这个类型里不可能有引用**。
     * ⚠️ 提到 `T` 的类型**不许早退** —— 泛型体的推迟检查要靠这份深度
     * （按"`T` 可能带引用"算，实例化时再决定这条规矩适不适用）✓
     * ⚠️ 也不许缓存（见函数末尾）*/
    if (!typeContainsRef(c->tt, tsub(c, e->type)) && !mentionsParam(e->type)) return 0;
    if (!c->substParams && !mentionsParam(e->type) && e->refDepth) return e->refDepth;

    int d = 0;
    switch (e->kind) {
    case EX_REF:
        d = placeDepth(c, e->u.ref.operand);
        break;
    case EX_DEREF:
        /* `*p` 的值住在 **p 指的地方** ⇒ 深度跟 p 一样 ✓ */
        d = exprRefDepth(c, e->u.deref.operand);
        break;
    case EX_SIGN:
        /* `p!` 只是"把可空的说法去掉"，指的还是同一块地方 ⇒ 深度跟着主体 ✓ */
        d = exprRefDepth(c, e->u.sign.operand);
        break;
    case EX_COALESCE:
        /* 两边都可能成为结果 ⇒ 取**最深**的那个（保守 = 安全方向）✓ */
        d = maxInt(exprRefDepth(c, e->u.coalesce.main),
                   exprRefDepth(c, e->u.coalesce.fallback));
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
    case EX_ENUMVAL:
        /* 带载荷构造 `shape.holding(a[..])` —— 载荷装进这个值里，
         * 所以它的深度就是载荷的深度（跟数组字面量同一个道理）✓ */
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.enumval.args, i)));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            d = maxInt(d, exprRefDepth(c, *(Expr **)vecAt(&e->u.assoc.args, i)));
        break;
    default:
        d = 0;
        break;
    }
    /* 提到 `T` 的节点**不缓存**（那份深度是"假设 T 带引用"算出来的）✓ */
    if (!c->substParams && !mentionsParam(e->type)) e->refDepth = d;
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

/* 「这个**地方**是借来的吗」—— 跟 `exprBorrowed` 的区别：
 * 它**不看值类型**（`*p` 的值可能完全没有引用），只问"这块存储是谁的" ✓
 * 用在 `ref *p` 这种"重新取一次引用"的洗白路径上 ✓ */
static bool placeIsBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_DEREF) return placeIsBorrowed(c, e->u.deref.operand);
    if (e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_INDEX) {
        Sym *root = placeRoot(c, e);
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    return false;
}

static bool exprBorrowed(Checker *c, Expr *e) {
    if (!e) return false;
    /* 早退的唯一理由：这个类型里不可能有引用。
     * ⚠️ 提到 `T` 的**不许早退** —— 泛型体的推迟检查要用这个答案 ✓ */
    if (!typeContainsRef(c->tt, tsub(c, e->type)) && !mentionsParam(e->type)) return false;
    switch (e->kind) {
    case EX_IDENT: case EX_FIELD: case EX_INDEX: {
        Sym *root = placeRoot(c, e);
        /* 参数：深度 0 且不是全局 ⇒ 借来的 ✓
         * 全局 / 静态：也深度 0，但**谁都存得下它** ✓ */
        return root && root->depth == 0 && !isGlobalSym(c, root);
    }
    case EX_SIGN:
        /* 签字只是换个说法，来源没变 ⇒ "借来的"这条照样跟着走 ✓ */
        return exprBorrowed(c, e->u.sign.operand);
    case EX_COALESCE:
        /* 两边都可能是结果 ⇒ 任一边借来的就算借来的 ✓ */
        return exprBorrowed(c, e->u.coalesce.main) ||
               exprBorrowed(c, e->u.coalesce.fallback);
    case EX_REF:
        /* ⚠️ **`ref *p` 是洗白路径**（2026-09-20 攻击测试打出来）：
         * `b.r = ref *p` —— 重新取一次引用，就看不出它来自参数了 ✗
         * `*p` 是"p 指的那个地方" ⇒ 要看**那个地方**借没借来 ✓
         * （注意不能走 exprBorrowed：`*p` 的**值**可能是 `i32`，会被早退挡掉 ✗）*/
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
        /* 带载荷构造：载荷是借来的 ⇒ 这个值也是借来的（跟数组字面量同理）*/
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprBorrowed(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    default: return false;      /* 字面量 / 全局 / alloc 出来的是本帧的 ✓ */
    }
}

/* 把一个值**存进**某个地方之前的全部检查（深度 + 借来的东西）。 */
static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what);   /* 定义在后面 */

static bool checkStoreEscape(Checker *c, Expr *val, Expr *target, int line) {
    /* 同上：提到 `T` 就整条推迟 ✓（这里**直接返回**，别让里面的 checkEscape 再记一遍）*/
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, target, placeDepth(c, target), line, "this assignment");
        return false;
    }
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
    Type       *declType;
    int         line;
    const char *what;
    FuncDef    *func;    /* 这条规矩属于哪个泛型函数 */
} RefCheck;

/* 记一条推迟的规矩。**只在泛型体里、且值提到了类型参数时**才记 ✓
 * （具体类型的值现在就查得清楚，不用推迟 —— 推迟只会让报错变晚）*/
/* 记一条"零值"的推迟检查（跟引用规矩同一族，见 RefCheck 的注释）*/
static void recordZeroCheck(Checker *c, Type *t, int line, const char *name) {
    if (!c->curFunc || !c->curFunc->owner) return;
    if (c->curFunc->owner->typeParams.len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->isZero   = true;
    rc->declType = t;
    rc->line     = line;
    rc->what     = name;
    rc->func     = c->curFunc;
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

static void recordRefCheck(Checker *c, Expr *val, Expr *target, int at,
                           int line, const char *what) {
    if (!c->curFunc || !c->curFunc->owner) return;
    if (c->curFunc->owner->typeParams.len == 0) return;
    RefCheck *rc = (RefCheck *)arenaAllocZero(c->arena, sizeof(RefCheck));
    rc->val    = val;
    rc->target = target;
    rc->at     = at;
    rc->line   = line;
    rc->what   = what;
    rc->func   = c->curFunc;
    /* ⚠️ **数字现在就算好**（那时作用域还在、`lookup` 找得到参数）：
     * 走路函数对"提到 `T`"的节点不会早退，所以这份深度是按"`T` 可能带引用"算的 ——
     * 正是实例化时要用的那一个 ✓
     * （踩过的弯路：推到实例化再算 ⇒ 那时函数作用域已经没了，深度算成 0，检查静默失灵 ✗）*/
    rc->depth    = exprRefDepth(c, val);
    rc->borrowed = exprBorrowed(c, val);
    *(RefCheck **)vecPush(&c->refChecks) = rc;
}

static bool checkEscape(Checker *c, Expr *val, int at, int line, const char *what) {
    if (!val) return false;
    /* ⚠️ 值里提到 `T` ⇒ 现在下不了结论（`T` 可能是 `i64` 也可能是 `slice<u8>`）
     * ⇒ **记下来，等实例化再算** ✓ （这就是 #17 那个洞的封口）*/
    if (mentionsParam(val->type)) {
        recordRefCheck(c, val, NULL, at, line, what);
        return false;
    }
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
    /* ⚠️ `*p` 要走**自己那条**：`placeRoot` 认不出它（会返回 NULL ⇒ 被当成可写）✗
     * 可写性由**引用的类型**给：只有 `mut ref` 才能 `*p = v` ✓ */
    if (e && e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        if (ot && ot->kind == TY_REF && ot->mut) return false;
        ckError(c, line,
                "`*p = v` needs `p` to be `mut ref T`; a `ref T` is a read-only borrow",
                "cannot %s through a read-only reference", what);
        return true;
    }
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
    /* 无载荷枚举在 C 里就是整数 ⇒ 直接比 ✓
     * **带载荷**的不行：C 里它是 `struct { tag; union }`，而 C 的 struct 不能用 `==`。
     * 用 `match` 比（以后可以派生出 `_eq` —— 那要递归比载荷，先不做）。 */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
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
    /* ⚠️ **不再有隐式解引用**（2026-09-20 主人拍板：`(ref i32) + 1` 必须非法）：
     * 值位置给了引用 ⇒ **报错**，教他写 `*p` ✓
     * 例外：`ref x` / `alloc`（本来就是"要一个引用"）、
     *       以及**成员选择**（`p.field` / `p.method()` —— 那是"导航"不是"取值"，仍然自动穿透 ✓）*/
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

/* 实参 / 字段初值：**期望类型是引用时不自动解引用** ——
 * 那里是「放一个引用进去」（`{ data: ref n, ... }`、`f(ref c)`），不是取它的值。
 * 其余情况按值位置处理（形状 3）。 */
static void desugarBareCtor(Checker *c, Expr *e, Type *want);   /* 定义在后面 */

static Type *checkInto(Checker *c, Type *want, Expr *e) {
    /* 期望类型是 `option<...>` / `result<...>` ⇒ 裸写构造器也认 ✓
     * （`f(some(3))`、`var x: ?i32 = some(3)` —— 类型从上下文来，不用写全名）*/
    if (want) desugarBareCtor(c, e, want);
    Type *got = checkExpr(c, e);
    /* 同样：**显式解引用** —— 期望的不是引用却给了引用 ⇒ 报错 ✓ */
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

/* **输出位置**：`println(r)` / `print(r)` 自动解引用 ✓
 * 理由（主人 2026-09-20）：「但你总不能真暴露地址了」——
 * 打印一个引用**只可能**是想看它指的值，而地址本身对用户毫无意义也不该暴露 ✓
 * ⇒ 这是唯一保留"自动解引用"的**值位置** ✓ */
static Type *checkPrintArg(Checker *c, Expr *e) {
    Type *t = checkExpr(c, e);
    if (t && t->kind == TY_REF) {
        e->deref = true;
        return t->inner;
    }
    return t;
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
    if (!t || t->targs.len != nargs) return false;
    /* 泛型 struct：`slice<T>`、`varArray<T>` */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* 泛型**枚举**：`option<T>` / `result<T,E>`（第三刀之后它们就是枚举，
     * 实例的类型是 TY_ENUM + 具体 targs ✓）*/
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
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
    /* `null` —— 它的类型**只能**从上下文来（`?ref T` 里的 T 是什么，字面量自己是不知道的）*/
    if (e->kind == EX_NULL) {
        if (want->kind == TY_REF && want->nullable) e->type = want;
        return;
    }
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

    /* `?ref T` → `ref T`：**这是"我保证它非空"，必须证明过** ✗
     * 反过来（非空 → 可空）永远安全，自动允许 ✓ */
    if (want->kind == TY_REF && got->kind == TY_REF && got->nullable && !want->nullable &&
        ttEquals(want->inner, got->inner)) {
        ckError(c, node ? node->line : 0,
                "check it first: `if p != null { ... }` -- inside that branch the compiler"
                " knows it is not null and generates no runtime check; there is no other"
                " way to turn a `?ref T` into a `ref T`",
                "%s expects `%s`, found `%s` -- a nullable reference", 
                what, typeStr(c, want), typeStr(c, got));
        return false;
    }

    if (ttEquals(want, got)) return true;

    /* 非空 → 可空：往安全的方向走，自动允许（跟 `mut ref` → `ref` 同一种降级）✓ */
    if (want->kind == TY_REF && got->kind == TY_REF && want->nullable && !got->nullable &&
        want->mut == got->mut && ttEquals(want->inner, got->inner)) return true;

    /* **降级**：可写的可以当只读的用（能写当然能读）—— 单向、永远安全，自动允许。
     * 反过来不行：那是在要写权限，必须显式写 `mut`。
     * 「安全是默认」的直接体现：往安全的方向收窄不用打招呼。
     * 两种东西都适用：引用（`mut ref T` → `ref T`）和视图（`mut slice<T>` → `slice<T>`）。 */
    if (want->kind == got->kind) {
        if (want->kind == TY_REF && got->mut && !want->mut &&
            ttEquals(want->inner, got->inner)) return true;
        /* 视图：**递归**只许去掉 `mut`（`mut slice<mut slice<T>>` 能当
         * `slice<slice<T>>` 用；反过来不行）—— 见 ttViewDowngradable 的注释 */
        if (want->kind == TY_GENERIC && ttViewDowngradable(want, got) &&
            !ttEquals(want, got)) return true;
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

        case EX_NULL: {
            /* 类型是上下文寄放在 e->type 上的（adoptContextType）。
             * 上下文没说清楚 ⇒ 报错，**绝不猜**（显式优于推导）✓ */
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
            if (!s) {
                ckError(c, e->line, "every name must be declared first (extC has no globals yet)",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* 名字的**解析**在这里定格 ⇒ 代码生成直接印 `cname`。
             * 遮蔽过的名字（`a` vs `a__2`）就靠这一行分开 ✓ */
            e->u.ident.cname = s->cname;
            /* 已经被 `if p != null` 证明过 ⇒ 交出**非空**引用。
             * 于是 `p.field`、`p.method()`、传给 `ref T` 参数全部自动成立，
             * 而且**一行运行时检查都不用加** ✓ */
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

            /* `&&` / `||` 的**短路**也是收窄点：`p != null && p.value > 3`
             * 右边能直接解 —— 因为走到右边就说明左边成立过 ✓ */
            if (isLogicOp(op0)) {
                Type *lt = checkValue(c, e->u.bin.left);
                expectBool(c, lt, e->u.bin.left);

                bool whenTrue = false;
                const char *tg = narrowTarget(c, e->u.bin.left, &whenTrue);
                const size_t mark = c->narrow.len;
                if (strcmp(op0, "&&") == 0) {
                    /* 走到右边 ⟺ 左边**为真** ⇒ 左边的全部事实都成立 ✓ */
                    narrowFactsOf(c, e->u.bin.left);
                } else if (tg && !whenTrue) {
                    /* `a || b`：走到右边 ⟺ 左边为假。只有最简单的 `p == null` 形态
                     * 能反推出 `p` 非空（`||` 的完整反推要先会否定一个合取，先不做）*/
                    pushNarrow(c, tg);
                }

                Type *rt = checkValue(c, e->u.bin.right);
                expectBool(c, rt, e->u.bin.right);
                c->narrow.len = mark;
                return c->tBool;
            }

            /* `p == null` / `p != null` —— 可空引用**唯一**的用法就是跟 null 比。
             * 必须抢在下面 `checkValue` 之前：`null` 自己是不知道类型的 ✓ */
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

            Type *lt = checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

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
                        /* 带载荷枚举是最容易撞上这个的类型：C 里它是 struct，
                         * 而 C 的 struct 不能用 `==` ⇒ 告诉用户改用 match ✓ */
                        bool ep = b && b->kind == TY_ENUM && enumHasPayload(b->edef);
                        ckError(c, e->line,
                                ep ? "an enum with a payload is a tagged union -- "
                                     "compare it with `match`, or write a method that does"
                                   : NULL,
                                ep ? "`%s` is an enum with a payload, so it has no `%s`"
                                   : "`%s` does not support `%s`", typeStr(c, lt), op);
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
                        e->assocOwner = et;      /* 解析好的类型（泛型实例要用）*/
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

            /* **枚举变体的构造**也走这条路：`option<i64>::some(3)` / `maybe<i64>::nothing`。
             * 也就是说 `::` 对枚举可以有两种读法，但**含义只有一种**（造一个值）——
             * 而且这样 prelude 和现有代码里的写法一个字都不用改 ✓ */
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
                Vec evargs = e->u.assoc.args;      /* union：先拿出来 */
                e->kind = EX_ENUMVAL;
                e->u.enumval.typeName = t->name;   /* 实例名，如 `maybe_i64` */
                e->u.enumval.variant  = v->name;
                e->u.enumval.args     = evargs;
                return checkExprInner(c, e);
            }

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

            /* ⭐ 这块内存活到**当前块结束**（arena 按块细化，PLAN A2）⇒
             * 它的深度就是**当前块的深度** `c->scopes.len` ✓
             *
             * ⚠️ 这一行是"深度模型"和"arena 粒度"的**接缝** —— 两边必须是同一个数：
             *   检查器用块深度判断"引用能不能存进这里"，
             *   生成的 C 用块深度选 `__extc_a[k]`、出块就 release。
             *   写死 1（老行为）的话，`while { p = alloc<i32>(1) }` 里 p 会在
             *   下一次迭代时指向**已经释放**的内存 ⇒ 悬垂 ✗（A2 之后实测过）✓
             * 于是「返回一块刚 alloc 的内存」「把循环里分配的东西存到循环外」
             * 都会被逃逸检查拦住 ✓ 想把它交出去，就得让调用者提供 buffer/arena ✓ */
            e->refDepth = c->scopes.len;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* 刚分配的地方当然可写 */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b` —— **可能没有就兜底**。语义 = `match a { 有(v) => v  _ => b }`，
             * 但**只算一边**：有值时 b 不求值 ✓（跟 `&&` / `||` 的短路同一件事）*/
            Type *mt = checkExpr(c, e->u.coalesce.main);
            if (ttIsError(mt)) { checkExpr(c, e->u.coalesce.fallback); return ttError(tt); }

            bool isOpt = isProtoType(mt, "option", 1);
            bool isRes = isProtoType(mt, "result", 2);
            Type *want = NULL;
            if (isOpt || isRes) {
                want = *(Type **)vecAt(&mt->targs, 0);          /* 载荷类型 T */
            } else if (mt->kind == TY_REF && mt->nullable) {
                want = mt;                                       /* `?ref T` ⇒ 兜底也是引用 */
            } else {
                /* ⚠️ 位置规则：`??` 只对"可能没有"的东西有意义 —— 说得清楚、报得响 ✓ */
                checkExpr(c, e->u.coalesce.fallback);
                ckError(c, e->line,
                        "`??` means \"if there is nothing, use this instead\", so the left"
                        " side must be an `option`, a `result`, or a `?ref T`",
                        "left of `??` is `%s`, which always has a value", typeStr(c, mt));
                return ttError(tt);
            }

            /* 主体**不是**"没有副作用的东西"（比如 `f() ?? -1`）⇒ 不能直接生成三元
             * （主体在 C 里出现两次 ⇒ `f()` 会跑两遍 ✗）⇒ 必须**提前求值到一个临时变量**。
             * 那需要"往所在语句前面吐前缀"的能力：大多数位置有，**两个位置没有** ✓ */
            if (!repeatablePure(e->u.coalesce.main)) {
                checkExpr(c, e->u.coalesce.fallback);
                if (c->noHoist) {
                    ckError(c, e->line,
                            "`while` re-evaluates its condition every round, but a temporary"
                            " can only be computed once, before the loop. Bind it inside the"
                            " loop body: `while true { let r = f()  if ... { break } }`",
                            "`??` here cannot be evaluated ahead of time -- the temporary"
                            " would run once instead of every round");
                    return ttError(tt);
                }
                e->needTemp = true;        /* codegen 照做：先算一次，再对临时变量做三元 */
                return want;
            }

            adoptContextType(e->u.coalesce.fallback, want);
            Type *ft = checkInto(c, want, e->u.coalesce.fallback);
            checkAssignable(c, want, ft, e->u.coalesce.fallback, "the right side of `??`");
            e->type = want;
            return want;
        }

        case EX_SIGN: {
            /* `e!` —— **我签字**（定案 1.3）。
             * 编译器证不出来的事，用户签字负责 ⇒ **一行检查都不生成** ✓
             * 三个用途：
             *   `opt!`（`option<T>`）⇒ 直接给载荷 T
             *   `r!`（`result<T,E>`）⇒ 直接给载荷 T（失败时的行为 = 签字，UB 算他的）
             *   `p!`（`?ref T`）⇒ 我知道非空，给我 `ref T` ✓（定案 ㊻ 留的逃生舱）
             * 不是这三样 ⇒ 报错：签字也要签在对的地方 ✓ */
            Type *ot = checkExpr(c, e->u.sign.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (isProtoType(ot, "option", 1) || isProtoType(ot, "result", 2)) {
                return *(Type **)vecAt(&ot->targs, 0);      /* 载荷类型 */
            }
            if (ot->kind == TY_REF) {
                if (!ot->nullable) {
                    ckError(c, e->line, "it is already a plain `ref T`, which can never be null",
                            "`%s` is not nullable, so `!` has nothing to assert",
                            typeStr(c, ot));
                    return ttError(tt);
                }
                Type *nn = ttRef(tt, ot->inner);            /* 非空版本 ✓ */
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
            /* `*p` = "p 指的那个值/那个地方"。可写性由 **p 的类型** 给（`mut ref`）✓ */
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
            return ot->inner;
        }

        case EX_ENUMVAL: {
            /* 泛型枚举的**实例**（`maybe<i64>`）不在类型表的按名字表里 ——
             * 它是实例化出来的。所以走 EX_ASSOC 那条路构造时会把解析好的类型
             * 记在 `assocOwner` 上，这里优先用它 ✓ */
            Type *et = e->assocOwner ? e->assocOwner : ttFromName(tt, e->u.enumval.typeName);
            if (!et || et->kind != TY_ENUM || !et->edef) return ttError(tt);
            Variant *v = findVariant(et->edef, e->u.enumval.variant);
            if (!v) return ttError(tt);

            /* 无载荷变体：`status.ok` —— 不能带参数 */
            if (v->types.len == 0) {
                if (e->u.enumval.args.len > 0) {
                    ckError(c, e->line, NULL,
                            "`%s.%s` carries no payload, so it takes no arguments",
                            et->name, v->name);
                    return ttError(tt);
                }
                return et;
            }

            /* **带载荷变体的构造**：`shape.circle(2.0)` —— 载荷按位置对 */
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
                /* 载荷装进这个值里 ⇒ 它的引用不能活得比这个值短 */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }

        case EX_CALL: {
            /* **带载荷变体的构造**：`shape.circle(2.0)` —— 它长得像"字段访问 + 调用"，
             * 但 `shape` 是个类型名不是变量 ⇒ 认出它是枚举构造，改写成 EX_ENUMVAL ✓ */
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
                        Vec args = e->u.call.args;      /* union：先拿出来再改 kind */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->u.enumval.args     = args;
                        e->assocOwner = et;
                        return checkExprInner(c, e);    /* 剩下的交给 EX_ENUMVAL 那条 */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkPrintArg(c, a);   /* 打印的是**值** ⇒ 自动解引用 ✓ */
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
            /* **带载荷变体的构造**也可能长这样：`shape.circle(2.0)` 的语法形状
             * 跟"方法调用"一模一样（`接收者.名字(参数)`）—— 区别在于 `shape`
             * 是个**类型名**而不是变量。所以先在这里认一次 ✓ （跟 `status.ok`
             * 在 EX_FIELD 那条路上被认出来是同一件事。）*/
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
                    e->assocOwner = et;
                    return checkExprInner(c, e);
                }
            }

            /* 方法只住在 struct 体内 —— 按接收者的类型去找 */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
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
                if (typeLacksZeroValue(tt, ft))
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

/* **裸写构造器**：`return success(v)` / `failure(e)` / `some(v)` / `none()`。
 *
 * 类型从**函数签名里已经写好的返回类型**来 —— 这不是"推导"，是**没让你重复一遍**：
 * 编译器没有猜任何东西，它读的是你自己写的那一行 `-> result<i64, gameError>`。
 *
 * 为什么值得：`result<i64, gameError>::failure(...)` 那串类型长到把重点盖住了，
 * 而重点从来是「失败」本身。`?` 负责收，`failure` 负责发，两头都不该啰嗦 ✓
 *
 * 落点是**原地改写成 EX_ASSOC** —— 后面检查/生成的路一条都不变（零新机制）。 */
static void desugarBareCtor(Checker *c, Expr *e, Type *want) {
    if (!e) return;

    const char *nm = NULL;
    const char *proto = NULL;
    size_t nargs = 0;
    bool noParens = false;

    if (e->kind == EX_CALL && e->u.call.callee->kind == EX_IDENT) {
        nm = e->u.call.callee->u.ident.name;
    } else if (e->kind == EX_IDENT) {
        /* 没载荷的构造器连括号都能省 —— `return none`（跟枚举变体一个待遇）*/
        nm = e->u.ident.name;
        noParens = true;
    } else return;

    if      (strcmp(nm, "success") == 0 || strcmp(nm, "failure") == 0) { proto = "result"; nargs = 2; }
    else if (strcmp(nm, "some")    == 0 || strcmp(nm, "none")    == 0) { proto = "option"; nargs = 1; }
    else return;
    if (noParens && strcmp(nm, "none") != 0) return;   /* 只有 `none` 是零参数的 */

    /* 用户自己写的同名函数优先 —— 裸写只是**在没歧义时**的省事 */
    if (findFunc(c, nm)) return;
    /* 返回类型不是对应容器 ⇒ 不认，照原样走（报「未定义的名字」，那是实话）*/
    if (!isProtoType(want, proto, nargs)) return;

    /* ⚠️ union：先把 args 拿**出来**再改 kind，否则会被自己的新字段覆盖 */
    Vec args;
    if (noParens) vecInit(&args, c->arena, sizeof(void *));
    else          args = e->u.call.args;
    /* `option` / `result` 现在是**普通枚举** ⇒ 裸构造器就是一个变体构造 ✓
     * （`typeName` 要写**实例名**：codegen 拿它拼 C 的 tag 常量 `option_i64_some`）*/
    e->kind = EX_ENUMVAL;
    e->u.enumval.typeName = want->name;
    e->u.enumval.variant  = nm;
    e->u.enumval.args     = args;
    e->assocOwner = want;
}

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
                /* 标注里提到 `T` ⇒ 现在看不出有没有零值，推到实例化再查 ✓ */
                if (s->u.var.ann && mentionsParam(s->u.var.ann))
                    recordZeroCheck(c, s->u.var.ann, s->line, s->u.var.name);
                else if (s->u.var.ann && typeLacksZeroValue(c->tt, s->u.var.ann)) {
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

            /* ⚠️ **`let`（将来叫 `const`）是只读绑定** ⇒ 类型里不许出现 `mut` ✗
             * （"又 let 又 mut"很诡异 —— 主人 2026-09-20 拍板）*/
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

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            /* `let x = e?` —— `?` 的合法位置之一。
             * 有类型标注时按**标注**决定要不要解引用（标的是引用 ⇒ 别解）；
             * 没标注时按值位置（形状 3）。 */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            /* **无标注 ⇒ 自然类型**（引用就还是引用）✓
             * 想要值的拷贝就写 `let v = *p` —— 显式 ✓
             * （否则 `let r = pickFirst(ref a, ref b)` 这种"绑定一个引用"写不出来 ✗）*/
            else                               it = checkExpr(c, s->u.var.init);

            /* ⭐ **`let` 推断出来的东西自动降级成只读**（主人 2026-09-20）：
             * `let p = ref x` ⇒ `p: ref T`（不是 `mut ref T`）✓
             * `let s = a[..]` ⇒ `s: slice<T>` ✓（跟视图那边原本的行为**统一**了 ✓）
             * ⇒ 于是"看见 let ⇒ 整条链只读"成立 ✓
             * ⇒ 权限**写在类型里** ⇒ 拷到哪儿都跟着（`var q = p` 也洗不掉 ✓✓）*/
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

            /* 逃逸②：初始化的引用不能指向比自己更深的局部 */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            s->u.var.cname = sym->cname;
            /* 引用型绑定：它指的东西有多深，**从初始值数出来** ✓
             * （`var cur: ?ref node = head` ⇒ head 是参数 ⇒ 0 ⇒ 这个游标可以返回 ✓）*/
            if (s->type && s->type->kind == TY_REF)
                sym->refDepth = exprRefDepth(c, s->u.var.init);
            return;
        }

        case ST_ASSIGN: {
            Type *tt_ = checkExpr(c, s->u.assign.target);

            /* 目标是**已经收窄过**的绑定 ⇒ 这里比的是槽位的**声明类型**。
             * 收窄只影响"读出来的是什么"，不影响"这个槽位能装什么" ✓
             * （所以 `while cur != null { cur = cur.next }` 合法 —— 循环体里
             *   收窄被这次赋值作废，下一轮循环条件重新证明 ✓）*/
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *slot = lookup(c, s->u.assign.target->u.ident.name);
                if (slot && slot->type && slot->type->kind == TY_REF && slot->type->nullable &&
                    tt_->kind == TY_REF && !tt_->nullable) tt_ = slot->type;
            }

            /* **目标是引用 ⇒ 写进去**（形状 3：`p = v` 写 p 指向的那个地方）。
             *
             * 「换指向」已经取消（见 DECISIONS 引用语义定案），所以给引用赋值
             * 必须对得上**被指的类型**；想改指向哪儿，只能重新声明一个绑定。 */
            if (tt_->kind == TY_REF) {
                /* ⚠️ 这里**不能**用 `requireMutable` —— 它看的是"引用类型是不是 mut" ✗
                 * 而"换指向"写的是**那个槽位**（变量/字段本身）⇒
                 * 只要求**绑定/字段可写**（`var`）✓
                 * ⇒ 于是 `var p: ref T = ref x;  p = ref y` 合法 ✓（主人 2026-09-20）*/
                Sym *slotRoot = placeRoot(c, s->u.assign.target);
                if (!slotRoot || !slotRoot->mut) {
                    ckError(c, s->line,
                            "rebinding a reference writes the binding itself, so the binding"
                            " must be `var` (the reference's own `mut` is about writing through"
                            " it, not about rebinding)",
                            "cannot rebind `%s`: the binding is read-only",
                            slotRoot ? slotRoot->name : "this");
                    return;
                }

                Expr *v = s->u.assign.value;

                /* **看右边是什么，就知道是哪件事 —— 歧义靠类型消掉，不靠禁用。**
                 *   右边是**值** `T`      ⇒ 写进它指向的地方   `*p = v`
                 *   右边是**引用** `ref T` ⇒ 换指向             `p = q`
                 *
                 * 这两件事类型不同、而且右边就在源码里看得见（P′）——
                 * 所以不需要像之前那样把换指向整个禁掉。 */
                /* `null` 要的上下文是**引用本身**（`?ref T`），不是它指的东西 ✓
                 * （`p.next = null` 会走到这条"换指向"的路上 —— 右边是引用）*/
                adoptContextType(v, v->kind == EX_NULL ? tt_ : tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* 自然类型：引用保留 */

                /* ⚠️ 作废非空证明要**等右边查完**：`cur = cur.next` 里右边的 `cur`
                 * 用的还是循环条件证明过的那份非空信息 ✓（踩过：放在前面会误报）*/
                if (s->u.assign.target->kind == EX_IDENT)
                    unNarrow(c, s->u.assign.target->u.ident.cname);

                if (vt0->kind == TY_REF) {
                    /* 换指向：类型要对得上（`mut ref` → `ref` 降级照旧允许），
                     * 而且新指向的东西不能活得比这个引用短。
                     * ⚠️ 深度比较用的是**旧的** refDepth（这个槽位原来指多深）——
                     * 所以更新被指深度必须等这些都查完，否则等于拿新值跟新值比，
                     * `p = ref <更深的局部>` 会整条漏过去 ✗（真踩过，攻击测试 h1 打出来）*/
                    int at0 = placeDepth(c, s->u.assign.target);
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    checkEscape(c, v, at0, s->line, "this reference");
                    /* 换指向 ⇒ 被指对象的深度跟着换 ✓（`cur = cur.next`）*/
                    if (s->u.assign.target->kind == EX_IDENT) {
                        Sym *slot0 = lookup(c, s->u.assign.target->u.ident.name);
                        if (slot0 && slot0->type && slot0->type->kind == TY_REF)
                            slot0->refDepth = exprRefDepth(c, v);
                    }
                    /* ⚠️ **换指向也要查"借来的值"**（2026-09-20 攻击测试打出来）：
                     *     struct slot { r: mut ref i32 }
                     *     fn stash(b: mut ref slot, p: mut ref i32) { b.r = ref *p }
                     * 权限够、深度也够（p 是参数 = 0），只有"它其实是调用者的东西"这条能拦 ✗
                     * ⇒ 少了这一句就是一个**真悬垂**（实测打印出过期数据）✓ */
                    checkStoreEscape(c, v, s->u.assign.target, s->line);
                    return;
                }

                /* ⚠️ **引用上不再有隐式写穿**（2026-09-20 加了 `*p` 之后）：
                 * `p = v` 只有"换指向"一个含义；想写穿就写 `*p = v` ✓
                 * ⇒ **每个写法只有一个含义，不用看右边分辨**（显式优于推导）✓ */
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

            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* 逃逸③：给字段/元素赋值时，被指对象不能比目标更深 */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (0) checkEscape(c, s->u.assign.value, placeDepth(c, s->u.assign.target),
                        s->line, "this assignment");
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            return;
        }

        case ST_IF: {
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);

            /* **`?ref T` 的收窄**：`if p != null` / `if p == null ... else` /
             * 护栏形态 `if p == null { return }` —— 三种写法都能让编译器知道
             * "这里是那个分支，p 一定不是 null"，于是里面**一行运行时检查都不用加** ✓ */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.ifs.cond, &whenTrue);
            const size_t mark = c->narrow.len;

            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* 出了 then 分支，证明就不算数了 ✓ */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {
                /* 护栏：条件成立就离开函数/循环 ⇒ 走到后面 ⟺ p 非空。
                 * 证明**留在当前作用域**（到本层块结束为止）✓ */
                pushNarrow(c, tg);
            }
            return;
        }

        case ST_WHILE: {
            /* ⚠️ `while` 的条件**不许**提前求值（吐出来只算一次，不是每轮）*/
            c->noHoist++;
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            c->noHoist--;

            /* `while cur != null { cur = cur.next }` —— 链表**整个语言的存在理由**，
             * 所以循环条件也是收窄点：循环体里那个绑定一定非空 ✓
             * （循环体里给它赋了新值 ⇒ unNarrow 会作废，所以下一轮要重新比 —— 
             *   这跟 C 里 `while (p) { p = p->next; }` 的写法完全一致 ✓）*/
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.whiles.cond, &whenTrue);
            const size_t mark = c->narrow.len;
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            return;
        }

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
            /* 裸写构造器（`return failure(e)`）—— 类型从上面这个 `want` 来 ✓ */
            desugarBareCtor(c, s->u.ret.value, want);

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

        case ST_MATCH: {
            /* `match e { 变体 => ... }`
             *
             * **这个特性的全部价值就在穷尽检查这一件事上** ——
             * 今天写 `if e == gameError.outOfRange { } else { }` 得手写 else，
             * 而且**以后加了新变体，编译器不会提醒你漏了**。match 会 ✓ */
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

            /* 每条分支：名字要是这个枚举的变体，而且不能重复 */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
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
                    if (strcmp(prev->variant, arm->variant) == 0) {
                        ckError(c, arm->line, NULL,
                                "`%s` is matched twice", arm->variant);
                        return;
                    }
                }
            }

            /* **穷尽**：一个都不能漏 */
            for (size_t j = 0; j < td->variants.len; j++) {
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

            /* 分支体各自开一层作用域；**绑定的载荷**（`circle(r) => ...`）
             * 就住在这里 —— 第 i 个名字拿第 i 个载荷字段 ✓ */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
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
                    /* 绑定的载荷是**这个值的副本**（值语义），名字只读 ——
                     * 想改就自己 `var` 一份 */
                    Sym *bs = declare(c, bn, bt, false, false, arm->line, c->scopes.len);
                    /* ⚠️ 把**解析后的 C 名字**写回绑定表（跟 `Param.cname` / `Stmt.u.var.cname`
                     * 同一个套路）。不写回的话，同一层里两个 `match` 绑同名时，
                     * 声明用原名、而分支体里查到的却是 `s__2` ⇒ 生成的 C 编不过
                     * （`'s__2' undeclared`）✗ —— 这就是 PLAN §0.4 #2 ✓ */
                    *(const char **)vecAt(&arm->binds, k) = bs->cname;
                }
                for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                    checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                popScope(c);
            }
            return;
        }

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

    return !ctx->hasError;
}
