/* 查找 + `?ref T` 收窄
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include <string.h>
#include "check_internal.h"

/* --------------------------------------------------- `?ref T` 的非空收窄 */
Sym *lookup(Checker *c, const char *name);   /* 定义在后面 */
void recordNewSizeCheck(Checker *c, Type *t, int line);   /* 同上 */
bool typeContainsRef(TypeTable *tt, Type *t);   /* 同上 */
int callHomeDepth(Checker *c, Vec *args, Vec *params);   /* 定义在后面 */
bool isNarrowed(Checker *c, const char *cname) {
    if (!cname) return false;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) return true;
    return false;
}

void pushNarrow(Checker *c, const char *cname) {
    if (!cname || isNarrowed(c, cname)) return;
    *(const char **)vecPush(&c->narrow) = cname;
}

/* 这个绑定又被赋成 null / 换了指向 ⇒ 之前的证明**作废**（收窄是栈式的，砍到它为止）*/
void unNarrow(Checker *c, const char *cname) {
    if (!cname) return;
    for (size_t i = 0; i < c->narrow.len; i++) {
        const char *k = *(const char **)vecAt(&c->narrow, i);
        if (strcmp(k, cname) == 0) { c->narrow.len = i; return; }
        /* ⭐ 档3：路径事实（`h.p`）也要跟着作废 —— 前缀相同就砍掉 ✓
         * 对根 `h` 的任何写、或"取它的地址"都可能让 `h.p` 非空不再成立 ✓ */
        size_t l = strlen(cname);
        if (strncmp(k, cname, l) == 0 && k[l] == '.') { c->narrow.len = i; return; }
    }
}

/* 这个条件式证明了**谁**非空？认不出来返回 NULL，`*whenTrue` 说证明在哪个分支里。
 *    `p != null` ⇒ then 分支里 p 非空
 *    `p == null` ⇒ **else** 分支里 p 非空（也就是 `if p == null { return }` 那个护栏形态）
 * 只有**可空引用**才算 —— 非空引用跟 null 比本身就是错（下面 checkBin 会报）。*/
const char *narrowTarget(Checker *c, Expr *cond, bool *whenTrue) {
    if (!cond || cond->kind != EX_BIN) return NULL;
    const char *op = cond->u.bin.op;
    if (strcmp(op, "!=") != 0 && strcmp(op, "==") != 0) return NULL;
    Expr *var = NULL;
    if (cond->u.bin.right->kind == EX_NULL)      var = cond->u.bin.left;
    else if (cond->u.bin.left->kind == EX_NULL)  var = cond->u.bin.right;
    else return NULL;
    /* ⭐ 档3（PLAN #14 的 **sound 子集**）：路径收窄 `if h.p != null { … }` ✓
     * 只认**没取过地址的局部**（depth ≥ 1 且 `!addressed`）当根 —— 那时
     * **没人能通过别名改它的字段** ⇒ "h.p 非空"这个事实稳定 ✓
     * ⚠️ 参数当根**不行**（调用者可能持有别名，或另有 `mut ref` 指向同一对象）✗
     *    取过地址也不行 ✗ —— 这正是 ARENA-FORMAL §7.4 那条"别名硬门槛"在路径上的样子 ✓ */
    if (var && var->kind == EX_FIELD && var->u.field.obj->kind == EX_IDENT) {
        Sym *rs = lookup(c, var->u.field.obj->u.ident.name);
        if (!rs || !rs->type || rs->depth < 1 || rs->addressed) return NULL;
        if (!var->type || var->type->kind != TY_REF || !var->type->nullable) return NULL;
        size_t kn = strlen(rs->cname) + strlen(var->u.field.name) + 2;
        char *key = (char *)arenaAlloc(c->arena, kn);
        snprintf(key, kn, "%s.%s", rs->cname, var->u.field.name);
        *whenTrue = (strcmp(op, "!=") == 0);
        return key;
    }
    if (!var || var->kind != EX_IDENT) return NULL;
    Sym *sy = lookup(c, var->u.ident.name);
    if (!sy || !sy->type || sy->type->kind != TY_REF || !sy->type->nullable) return NULL;
    *whenTrue = (strcmp(op, "!=") == 0);
    return sy->cname;
}

/* 一个条件式能证明的**全部**非空事实。
 * `&&` 两边都要走 —— 能走到这个条件成立的分支，说明两边都成立过 ✓
 * （`if p != null && p.next != null { ... }` 就是这个形状；链越深越需要）*/
void narrowFactsOf(Checker *c, Expr *cond) {
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
bool blockExits(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_BLOCK) {
        if (s->u.block.stmts.len == 0) return false;
        return blockExits(*(Stmt **)vecAt(&s->u.block.stmts, s->u.block.stmts.len - 1));
    }
    return s->kind == ST_RETURN || s->kind == ST_BREAK || s->kind == ST_CONTINUE;
}

/* 可空引用**不能**直接解 —— 先查 null（P′：不能证明的，语法上必须看得见）*/
bool rejectNullableDeref(Checker *c, Type *t, Expr *node, const char *what) {
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
Sym *declare(Checker *c, const char *name, Type *t, bool mut,
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
    s->refDepth = (t && typeContainsRef(c->tt, t)) ? depth : 0;
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
    return s;
}

Sym *lookup(Checker *c, const char *name) {
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

/* ⭐ 调试/诊断里显示函数名：`pair$make` ⇒ `pair::make`（根模块原样 ✓）
 * 为什么要这一层：`EXTC_DUMP_EFFECTS` 那几行原来直接把 `f->name` 印出来，
 * 于是调试输出里全是内部编码 `pair$make` ✗（跟"诊断不许露 mangle 名"是同一条规矩）*/
const char *checkFnDisplay(const char *name, const char *modName) {
    if (!name) return "?";
    if (!modName || !*modName) return name;    /* 根模块/单文件 ⇒ 原名 ✓ */
    const char *d = strchr(name, '$');
    if (!d) return name;                       /* 没加前缀（`extern!`）⇒ 原样 ✓ */
    static char buf[512];
    size_t nl = (size_t)(d - name);
    if (nl + 2 + strlen(d + 1) + 1 > sizeof buf) return name;
    memcpy(buf, name, nl);
    buf[nl] = ':'; buf[nl + 1] = ':';
    strcpy(buf + nl + 2, d + 1);
    return buf;
}

FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (f->tmpl) continue;               /* ⭐ PLAN #47：实例不是"名字"（模板才是 ✓）*/
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/* ⭐ 定案 70：**别的模块的名字必须写限定名**（`mod::name`）——
 * 不加这一条，`use greet` 就只是装饰（平表里什么都够得着 ✗），
 * `@private` 也就挡不住（私有名同样够得着 ✗）。
 * 判据：被调者属于别的模块、且**这个引用不是限定名改写来的** ⇒ 报错 ✓
 * （限定名在**装载器**里就改写成平名字了，所以靠 `Expr.qualified` 那个位区分 ✓）
 * 豁免：prelude（`reserved` = 自动导入 ✓）与同一个文件里的名字 ✓ */
void requireQualified(Checker *c, const char *what, const char *whatMod, bool qualified, int line) {
    if (qualified) return;
    if (!whatMod) return;
    if (!c->curFunc || !c->curFunc->modName) {
        if (c->curFunc) {                        /* 根文件：别的模块的名字一律要限定 ✓ */
            ckError(c, line++, "Write `mod::name` (and `use mod` at the top of the file)."
                               " A module's `@private` names are not reachable here at all.",
                    "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
        }
        return;
    }
    if (strcmp(c->curFunc->modName, whatMod) == 0) return;   /* 自己模块的 ✓ */
    ckError(c, line, "Write `mod::name` (and `use mod` at the top of the file)."
                     " A module's `@private` names are not reachable here at all.",
            "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
}

FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}

/* TY_STRUCT 和 TY_GENERIC 都指向一个 StructDef */
StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/* 方法只住在 struct 体内（定案 9）*/
FuncDef *findMethod(Type *st, const char *name) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* 枚举实例的载荷类型：泛型实例要**按实参替换**
 * （`type maybe<T> = | nothing | just(T)` 的实例 `maybe<i64>` 里，`just` 的载荷是 `i64`）✓ */
Type *payloadType(TypeTable *tt, Type *et, Variant *v, size_t k) {
    Type *pt = *(Type **)vecAt(&v->types, k);
    if (et && et->kind == TY_ENUM && et->edef &&
        et->edef->typeParams.len > 0 && et->targs.len == et->edef->typeParams.len)
        pt = ttSubstitute(tt, pt, &et->edef->typeParams, &et->targs);
    return pt;
}

/* 这个枚举有**带载荷**的变体吗？（`| circle(f64)`）
 * 有的话：C 里它不再是 `enum` 而是 `struct { tag; union }` ⇒
 * ① 不能用 `==`（C 的 struct 不能比）② 打印只打变体名 */
bool enumHasPayload(TypeDef *td) {
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
bool typeContainsRef(TypeTable *tt, Type *t) {
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
bool typeLacksZeroValue(TypeTable *tt, Type *t) {
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
bool repeatablePure(Expr *e) {
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
bool isLvalue(Expr *e) {
    /* `*p` 也是**地方**（p 指的那个地方）✓ */
    return e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_DEREF;
}

/* 拼一个 `slice<elem>`（视图协议来自 prelude）*/
Type *sliceOf(Checker *c, Type *elem) {
    if (!c->sliceDef) return ttError(c->tt);
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Type **)vecPush(&args) = elem;
    return ttGeneric(c->tt, c->sliceDef, &args);
}

/* 造一个整数字面量节点。
 * 用途：`a[2..]` 省略的界由**编译器**补成字面量（见 EX_SLICE），
 * 这样 codegen 只要看到「两个界都是字面量」就知道范围能证明、无需运行时检查。 */
Expr *intLit(Checker *c, long long v, int line) {
    Expr *e = exprNew(c->arena, EX_INT, line);
    e->u.ival = v;
    e->type = c->tI32;
    return e;
}

/* 这个表达式是不是一个字面整数？`-1` 也算 —— 它是 `EX_UN` 套 `EX_INT`，
 * 而负数界正是最容易写错的地方（`a[-1..n]` 必须当场报错，不能等到运行时）。 */
bool asIntLit(Expr *e, long long *out) {
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
bool isPlace(Expr *e) {
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
Sym *placeRoot(Checker *c, Expr *e) {
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
bool pathHasReadonlyRef(Expr *e);

bool isWritablePlace(Checker *c, Expr *e) {
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
