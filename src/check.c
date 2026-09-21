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


#include "check_internal.h"
/* ---------------------------------------------------------------- 报错 */

void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

const char *typeStr(Checker *c, Type *t) {
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

const char *cNameFor(Checker *c, const char *name) {
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

void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
    /* 收窄也是词法作用域的：进块时记下水位，出块时退回去 ✓ */
    *(size_t *)vecPush(&c->narrowMarks) = c->narrow.len;
}

void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
    if (c->narrowMarks.len) {
        c->narrow.len = *(size_t *)vecAt(&c->narrowMarks, c->narrowMarks.len - 1);
        c->narrowMarks.len--;
    }
}

/* 实例化时把类型里的 TY_PARAM 换成实参 ✓（不在实例化里就是原样返回）*/
Type *tsub(Checker *c, Type *t) {
    if (!c->substParams || !c->substArgs || !t) return t;
    return ttSubstitute(c->tt, t, c->substParams, c->substArgs);
}

/* 这个类型里"提到了类型参数"吗？提到了就不能现在下结论 ⇒ 推迟到实例化 ✓ */
bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/* ---------------------------------------------------------------- 小工具 */

bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    /* 定案 11：无载荷枚举自动有名字文本 */
    if (b->kind == TY_ENUM) return true;
    /* struct / 泛型实例 / 数组都由编译器生成 <Type>_debug 递归打印 */
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC || b->kind == TY_ARRAY;
}

/* 字面量的类型按**值**适配目标类型（DESIGN §5 的「字面量类型推导」）。 */
bool literalFits(Expr *e, Type *want) {
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
void adoptContextType(Expr *e, Type *want) {
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

bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
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

void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "conditions must be `bool` -- extC has no implicit truthiness",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

