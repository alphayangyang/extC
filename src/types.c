#include <stdio.h>
#include <stdlib.h>
#include "types.h"

#include <string.h>

/* ================================================================ 内建类型 */

static const char *BUILTIN_NAMES[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool",
    NULL
};

bool ttIsBuiltinName(const char *name) {
    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        if (strcmp(BUILTIN_NAMES[i], name) == 0) return true;
    return false;
}

static Type *mkType(Arena *a, TypeKind kind, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = kind;
    t->name = name;
    return t;
}

TypeTable *ttNew(Arena *a, Module *m) {
    TypeTable *tt = (TypeTable *)arenaAllocZero(a, sizeof(TypeTable));
    tt->arena = a;
    vecInit(&tt->builtins, a, sizeof(void *));
    vecInit(&tt->structs, a, sizeof(void *));
    vecInit(&tt->enums, a, sizeof(void *));
    vecInit(&tt->aliases, a, sizeof(Alias));
    vecInit(&tt->instances, a, sizeof(void *));
    vecInit(&tt->enumInstances, a, sizeof(void *));
    vecInit(&tt->viewShadows, a, sizeof(void *));

    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        *(Type **)vecPush(&tt->builtins) = mkType(a, TY_BUILTIN, BUILTIN_NAMES[i]);

    tt->tVoid  = mkType(a, TY_VOID, "void");
    tt->tError = mkType(a, TY_ERROR, "<error>");

    if (m) ttRegister(tt, m);
    return tt;
}

void ttRegister(TypeTable *tt, Module *m) {
    if (!m) return;

    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        bool seen = false;
        for (size_t j = 0; j < tt->structs.len && !seen; j++)
            seen = strcmp((*(StructDef **)vecAt(&tt->structs, j))->name, sd->name) == 0;
        if (!seen) *(StructDef **)vecPush(&tt->structs) = sd;
    }
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        bool seen = false;
        for (size_t j = 0; j < tt->enums.len && !seen; j++)
            seen = strcmp((*(TypeDef **)vecAt(&tt->enums, j))->name, td->name) == 0;
        if (!seen) *(TypeDef **)vecPush(&tt->enums) = td;
    }
}

Type *ttVoid(TypeTable *tt)  { return tt->tVoid; }
Type *ttError(TypeTable *tt) { return tt->tError; }

/* 固定数组：也驻留（`[15]i32` 全局只有一份），放在跟泛型实例同一张表里 —— 
 * 它们都要生成 C 结构体，codegen 一视同仁。 */
Type *ttArray(TypeTable *tt, int64_t n, Type *elem) {
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *c = *(Type **)vecAt(&tt->instances, i);
        if (c->kind == TY_ARRAY && c->asize == n && ttEquals(c->inner, elem)) return c;
    }
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_ARRAY;
    t->asize = n;
    t->inner = elem;
    t->name = ttMangle(tt, t);
    *(Type **)vecPush(&tt->instances) = t;
    return t;
}

Type *ttRef(TypeTable *tt, Type *inner) {
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/* 造一个跟 `src` 同样**可写性**的引用 —— 解析和替换时 `mut` 不能丢。 */
static Type *refLike(TypeTable *tt, Type *src, Type *inner) {
    Type *t = ttRef(tt, inner);
    t->mut = src->mut;
    t->nullable = src->nullable;   /* `?ref T` 的可空性也是类型的一部分，解析/替换时不能丢 */
    return t;
}

Type *ttFromName(TypeTable *tt, const char *name) {
    if (strcmp(name, "void") == 0) return tt->tVoid;

    for (size_t i = 0; i < tt->builtins.len; i++) {
        Type *t = *(Type **)vecAt(&tt->builtins, i);
        if (strcmp(t->name, name) == 0) return t;
    }
    for (size_t i = 0; i < tt->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&tt->structs, i);
        if (strcmp(sd->name, name) == 0) {
            if (!sd->type) {
                sd->type = mkType(tt->arena, TY_STRUCT, sd->name);
                sd->type->sdef = sd;
            }
            return sd->type;
        }
    }
    for (size_t i = 0; i < tt->enums.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&tt->enums, i);
        if (strcmp(td->name, name) == 0) {
            if (!td->type) {
                td->type = mkType(tt->arena, TY_ENUM, td->name);
                td->type->edef = td;
            }
            return td->type;
        }
    }
    return NULL;
}

Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params) {
    if (!t) return NULL;

    switch (t->kind) {
        case TY_UNRESOLVED: {
            /* 1) 是不是泛型参数？*/
            if (params) {
                for (size_t i = 0; i < params->len; i++) {
                    if (strcmp(*(const char **)vecAt(params, i), t->name) == 0)
                        return typeParam(tt->arena, t->name, (int)i);
                }
            }

            /* ⭐ 模块 mangle：裸名先查别名表（`pair` → `liba$pair`）✓
             * 只在**源码名**查不到时才走它 ⇒ 本文件/内建/prelude 优先级不变 ✓ */
            const char *nm0 = t->name;
            if (!ttFromName(tt, nm0)) {
                for (size_t i = 0; i < tt->aliases.len; i++) {
                    Alias *al = (Alias *)vecAt(&tt->aliases, i);
                    if (strcmp(al->from, nm0) == 0) { nm0 = al->to; break; }
                }
            }
            Type *base = ttFromName(tt, nm0);
            if (!base) {
                ctxError(ctx, line, 1,
                         "built-in types: i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool str void",
                         "unknown type `%s`", t->name);
                return tt->tError;
            }

            /* 2) 带类型实参 → 泛型实例 */
            if (t->targs.len > 0) {
                /* **泛型枚举**：`option<i64>`（载荷类型在用到时按实参替换）*/
                if (base->kind == TY_ENUM && base->edef) {
                    if (base->edef->typeParams.len != t->targs.len) {
                        ctxError(ctx, line, 1, NULL,
                                 "`%s` expects %zu type argument(s), got %zu",
                                 t->name, base->edef->typeParams.len, t->targs.len);
                        return tt->tError;
                    }
                    Vec eargs;
                    vecInit(&eargs, tt->arena, sizeof(void *));
                    for (size_t i = 0; i < t->targs.len; i++)
                        *(Type **)vecPush(&eargs) =
                            ttResolve(tt, ctx, *(Type **)vecAt(&t->targs, i), line, params);
                    return ttEnumGeneric(tt, base->edef, &eargs);
                }
                if (base->kind != TY_STRUCT || !base->sdef) {
                    ctxError(ctx, line, 1, NULL,
                             "`%s` is not a generic type, so it takes no type arguments",
                             t->name);
                    return tt->tError;
                }
                if (base->sdef->typeParams.len != t->targs.len) {
                    ctxError(ctx, line, 1, NULL,
                             "`%s` expects %zu type argument(s), got %zu",
                             t->name, base->sdef->typeParams.len, t->targs.len);
                    return tt->tError;
                }
                Vec args;
                vecInit(&args, tt->arena, sizeof(void *));
                for (size_t i = 0; i < t->targs.len; i++)
                    *(Type **)vecPush(&args) =
                        ttResolve(tt, ctx, *(Type **)vecAt(&t->targs, i), line, params);
                Type *g = ttGeneric(tt, base->sdef, &args);
                if (t->mut) {
                    /* `mut slice<T>` —— 只有**视图**能有可写版。
                     * 别的泛型加 `mut` 是「深可变性」，那是更大的概念，明确拒绝。 */
                    if (!ttIsViewType(g)) {
                        ctxError(ctx, line, 1,
                                 "`mut` on a type means \"the references inside are writable\", "
                                 "which only makes sense for a view. Everything else is written "
                                 "through a `mut ref` or a `var` binding.",
                                 "`mut` cannot qualify `%s`", t->name);
                        return tt->tError;
                    }
                    return ttViewMut(tt, g, true);
                }
                return g;
            }

            /* 3) 泛型 struct / 泛型枚举不带实参 → 报错 */
            if (base->kind == TY_STRUCT && base->sdef && base->sdef->typeParams.len > 0) {
                ctxError(ctx, line, 1,
                         "a generic needs explicit type arguments, e.g. `%s<i32>`",
                         "`%s` is generic and needs type arguments", t->name);
                return tt->tError;
            }
            if (base->kind == TY_ENUM && base->edef && base->edef->typeParams.len > 0) {
                ctxError(ctx, line, 1,
                         "a generic needs explicit type arguments, e.g. `%s<i32>`",
                         "`%s` is generic and needs type arguments", t->name);
                return tt->tError;
            }
            return base;
        }
        case TY_REF:
            return refLike(tt, t, ttResolve(tt, ctx, t->inner, line, params));
        case TY_ARRAY: {
            Type *e = ttResolve(tt, ctx, t->inner, line, params);
            if (e->kind == TY_PARAM) {
                ctxError(ctx, line, 1,
                         "The element type of a fixed array must be concrete "
                         "(its size is part of the type).",
                         "cannot make a fixed array of the type parameter `%s`", e->param);
                return tt->tError;
            }
            return ttArray(tt, t->asize, e);
        }
        default:
            return t;
    }
}

/* ================================================================ 泛型 */

bool ttIsParam(Type *t, const char *name) {
    return t && t->kind == TY_PARAM && strcmp(t->param, name) == 0;
}

const char *ttMangle(TypeTable *tt, Type *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TY_REF:
            return arenaPrintf(tt->arena, "Ref_%s", ttMangle(tt, t->inner));
        case TY_ARRAY:
            return arenaPrintf(tt->arena, "array_%lld_%s",
                               (long long)t->asize, ttMangle(tt, t->inner));
        case TY_GENERIC: {
            Buf b;
            bufInit(&b, tt->arena);
            bufPuts(&b, t->sdef->name);
            for (size_t i = 0; i < t->targs.len; i++) {
                bufPutc(&b, '_');
                bufPuts(&b, ttMangle(tt, *(Type **)vecAt(&t->targs, i)));
            }
            return bufCstr(&b);
        }
        case TY_PARAM: return t->param;
        case TY_VOID:  return "void";
        case TY_ERROR: return "Error";
        default:       return t->name ? t->name : "?";
    }
}

bool ttHasParam(Type *t) {
    if (!t) return false;
    if (t->kind == TY_PARAM) return true;
    if (t->kind == TY_REF) return ttHasParam(t->inner);
    if (t->kind == TY_GENERIC) {
        for (size_t i = 0; i < t->targs.len; i++)
            if (ttHasParam(*(Type **)vecAt(&t->targs, i))) return true;
    }
    return false;
}

Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args) {
    /* 只有**完全具体**的实例才驻留、才需要生成 C。
     * 检查模板时会出现 `Pair<A, B>`（实参是类型参数）—— 那只是拿来比类型的，
     * 绝不能混进实例表，否则 codegen 会去生成 `Box_T_set` 这种东西。 */
    bool concrete = true;
    for (size_t j = 0; j < args->len; j++) {
        if (ttHasParam(*(Type **)vecAt(args, j))) { concrete = false; break; }
    }

    if (concrete) {
        /* 驻留：同一个实例全局只有一份 */
        for (size_t i = 0; i < tt->instances.len; i++) {
            Type *c = *(Type **)vecAt(&tt->instances, i);
            if (c->sdef != sd || c->targs.len != args->len) continue;
            bool same = true;
            for (size_t j = 0; j < args->len; j++) {
                if (!ttEquals(*(Type **)vecAt(&c->targs, j), *(Type **)vecAt(args, j))) {
                    same = false;
                    break;
                }
            }
            if (same) return c;
        }
    }

    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_GENERIC;
    t->sdef = sd;
    vecInit(&t->targs, tt->arena, sizeof(void *));
    for (size_t j = 0; j < args->len; j++)
        *(Type **)vecPush(&t->targs) = *(Type **)vecAt(args, j);
    t->name = ttMangle(tt, t);
    if (concrete) *(Type **)vecPush(&tt->instances) = t;
    return t;
}

/* **泛型枚举的实例**（`option<i64>`）。
 *
 * 跟 `ttGeneric` 是同一个套路，但有两点不同：
 *   ① owner 是 `TypeDef`（`edef`），不是 `StructDef` ⇒ C 名字在 `enumInstances` 里驻留
 *   ② 载荷类型**不在这里替换** —— 变体的载荷声明写在 `TypeDef` 上（`some(T)`），
 *      用到的时候按 `edef->typeParams` + `t->targs` 现场替换（`ttSubstitute`）。
 *      这样 `TypeDef` 本身只有一个，实例只是"名字 + 实参" ✓
 *      codegen 那边靠 `substEnter(inst)` 把上下文摆好，`cType` 自动替换 ✓ */
Type *ttEnumGeneric(TypeTable *tt, TypeDef *td, Vec *args) {
    bool concrete = true;
    for (size_t j = 0; j < args->len; j++)
        if (ttHasParam(*(Type **)vecAt(args, j))) { concrete = false; break; }

    if (concrete) {
        for (size_t i = 0; i < tt->enumInstances.len; i++) {
            Type *c = *(Type **)vecAt(&tt->enumInstances, i);
            if (c->edef != td || c->targs.len != args->len) continue;
            bool same = true;
            for (size_t j = 0; j < args->len; j++)
                if (!ttEquals(*(Type **)vecAt(&c->targs, j), *(Type **)vecAt(args, j))) { same = false; break; }
            if (same) return c;
        }
    }

    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_ENUM;
    t->edef = td;
    vecInit(&t->targs, tt->arena, sizeof(void *));
    for (size_t j = 0; j < args->len; j++)
        *(Type **)vecPush(&t->targs) = *(Type **)vecAt(args, j);

    Buf b;
    bufInit(&b, tt->arena);
    bufPuts(&b, td->name);
    for (size_t j = 0; j < t->targs.len; j++) {
        bufPutc(&b, '_');
        bufPuts(&b, ttMangle(tt, *(Type **)vecAt(&t->targs, j)));
    }
    t->name = bufCstr(&b);

    if (concrete) *(Type **)vecPush(&tt->enumInstances) = t;
    return t;
}

/* `mut slice<T>` —— 可写视图。
 *
 * 为什么要「影子」而不是新实例：可写视图和只读视图在 C 里是**同一个结构体**
 * （布局、名字、方法集都一样），只有 checker 眼里的**权限**不同。
 * 所以影子共用 `name`、**不进 instances** ⇒ codegen 只生成一份。
 * 这正是「`mut` 是限定词，不是第二个类型」的落地方式。 */
Type *ttViewMut(TypeTable *tt, Type *base, bool mut) {
    if (!mut || !base || base->kind != TY_GENERIC) return base;
    if (base->mut) return base;                 /* 已经是可写的了 */
    for (size_t i = 0; i < tt->viewShadows.len; i++) {
        Type *s = *(Type **)vecAt(&tt->viewShadows, i);
        if (s->inner == base) return s;          /* inner 拿来记「我是谁的可写版」 */
    }
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind  = TY_GENERIC;
    t->sdef  = base->sdef;
    t->targs = base->targs;
    t->name  = base->name;                       /* ★ 同一个 C 名字 */
    t->inner = base;
    t->mut   = true;
    *(Type **)vecPush(&tt->viewShadows) = t;
    return t;
}

/* 拿掉 `mut` 限定词（可写视图 → 只读视图）。降级是单向安全的，所以到处要用。
 *
 * **递归**：元素的 `mut` 也要降 —— `mut slice<mut slice<T>>` 应该能用在任何
 * 只读的地方（`slice<slice<T>>` 参数）。
 *
 * 为什么递归是安全的：**`mut` 是权限，不是布局**。C 里
 * `slice<mut slice<i32>>` 和 `slice<slice<i32>>` 是**同一个结构体**
 * （名字都叫 `slice_slice_i32`，`mut` 不出现在 C 类型里）⇒ 传参零风险 ✓
 *
 * 反方向（只读 → 可写）**永远不允许**：不能凭空加权限 ✓ */
Type *ttViewReadonly(TypeTable *tt, Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return t;

    /* 先递归降元素的（哪怕外层本来就不带 mut：`slice<mut slice<T>>` 也要降）*/
    bool argChanged = false;
    Vec args;
    vecInit(&args, tt->arena, sizeof(void *));
    for (size_t i = 0; i < t->targs.len; i++) {
        Type *a  = *(Type **)vecAt(&t->targs, i);
        Type *na = ttViewReadonly(tt, a);
        if (na != a) argChanged = true;
        *(Type **)vecPush(&args) = na;
    }

    if (t->mut) return ttGeneric(tt, t->sdef, &args);     /* 顶层降：影子 → 实例 */
    if (argChanged) return ttGeneric(tt, t->sdef, &args); /* 元素降了，重建一次 */
    return t;
}

Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args) {
    if (!t || !params || params->len == 0 || !args) return t;

    switch (t->kind) {
        case TY_PARAM:
            for (size_t i = 0; i < params->len && i < args->len; i++) {
                if (strcmp(*(const char **)vecAt(params, i), t->param) == 0)
                    return *(Type **)vecAt(args, i);
            }
            return t;
        case TY_REF:
            return refLike(tt, t, ttSubstitute(tt, t->inner, params, args));
        case TY_ARRAY:
            return ttArray(tt, t->asize, ttSubstitute(tt, t->inner, params, args));
        case TY_GENERIC: {
            Vec na;
            vecInit(&na, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->targs.len; i++)
                *(Type **)vecPush(&na) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->targs, i), params, args);
            /* 可写性要跟着走（`mut` 是类型的一部分，替换时不能丢） */
            return ttViewMut(tt, ttGeneric(tt, t->sdef, &na), t->mut);
        }
        /* ⚠️ **泛型枚举实例**也要替换（2026-09-20 抓到的真 bug）：
         * 第三刀把 `option` / `result` 变成泛型**枚举**之后，这里没跟上 ——
         * 枚举实例的 kind 是 `TY_ENUM`（不是 `TY_GENERIC`）⇒ 掉进 default 原样返回 ✗
         * 后果：泛型实例的方法返回 `option<T>` 时**没代入** ⇒
         *   `var v: varArray<i32>  v.get(0)` 得到 `option_T` 而不是 `option_i32` ✗
         *   （用户看到 "expects option_i32, found option_T" 这种没法理解的报错）
         * 同一个坑的另一半早前修过（`typeContainsRef` 的枚举分支）✓ ——
         * **教训：加一种类型构造时，`ttSubstitute` / `ttEquals` / `typeContainsRef` /
         * `ttRender` 这一族"按 kind 分派"的函数全都要扫一遍** ✓ */
        case TY_ENUM: {
            if (!t->edef || t->targs.len == 0) return t;
            Vec na;
            vecInit(&na, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->targs.len; i++)
                *(Type **)vecPush(&na) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->targs, i), params, args);
            return ttEnumGeneric(tt, t->edef, &na);
        }
        default:
            return t;
    }
}

/* ================================================================ 相等 */

bool ttEquals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    /* 除了 ref / 泛型参数 / 泛型实例，其余类型都是驻留的 —— 指针不等就是不相等 */
    if (a->kind == TY_REF)
        return a->mut == b->mut && a->nullable == b->nullable &&
               ttEquals(a->inner, b->inner);

    if (a->kind == TY_ARRAY)
        return a->asize == b->asize && ttEquals(a->inner, b->inner);

    if (a->kind == TY_PARAM)
        return a->tpIndex == b->tpIndex && strcmp(a->param, b->param) == 0;

    if (a->kind == TY_GENERIC) {
        if (a->mut != b->mut) return false;   /* 可写视图 ≠ 只读视图 */
        if (a->sdef != b->sdef || a->targs.len != b->targs.len) return false;
        for (size_t i = 0; i < a->targs.len; i++)
            if (!ttEquals(*(Type **)vecAt(&a->targs, i), *(Type **)vecAt(&b->targs, i)))
                return false;
        return true;
    }
    return false;
}

bool ttIs(Type *t, const char *builtinName) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN && strcmp(b->name, builtinName) == 0;
}

bool ttIsError(Type *t) {
    return t && t->kind == TY_ERROR;
}

Type *ttBase(Type *t) {
    while (t && t->kind == TY_REF) t = t->inner;
    return t;
}

/* ================================================================ 数值分类 */

typedef struct { const char *name; int bits; bool sgn; } IntInfo;

static const IntInfo INTS[] = {
    { "i8",  8, true  }, { "i16", 16, true  }, { "i32", 32, true  }, { "i64", 64, true  },
    { "u8",  8, false }, { "u16", 16, false }, { "u32", 32, false }, { "u64", 64, false },
    { NULL, 0, false }
};

static const IntInfo *intInfo(Type *t) {
    Type *b = ttBase(t);
    if (!b || b->kind != TY_BUILTIN) return NULL;
    for (size_t i = 0; INTS[i].name; i++)
        if (strcmp(INTS[i].name, b->name) == 0) return &INTS[i];
    return NULL;
}

bool ttIsInteger(Type *t) { return intInfo(t) != NULL; }

bool ttIsFloat(Type *t) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN &&
           (strcmp(b->name, "f32") == 0 || strcmp(b->name, "f64") == 0);
}

bool ttIsNumeric(Type *t) { return ttIsInteger(t) || ttIsFloat(t); }

int ttIntBits(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->bits : 0;
}

bool ttIntSigned(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->sgn : false;
}

/* ================================================================ 拓宽规则
 *
 * 只允许**无损失**的隐式转换。收窄一律禁止 —— 这是 DESIGN §3「每次转换的意图
 * 都必须清楚」的落地。有损转换必须写出方法名（.truncate() / .round() / …）。
 *
 * 注意：i32 → f32 也算有损（f32 尾数只有 24 位），所以**不**自动允许。
 * 字面量不受这条限制 —— 字面量的类型可以按值适配（见 check.c）。
 */

bool ttCanWiden(Type *from, Type *to) {
    if (!from || !to) return false;
    if (ttIsError(from) || ttIsError(to)) return true;   /* 抑制级联报错 */
    if (ttEquals(from, to)) return true;

    /* ⚠️ **引用不参与「拓宽」。**
     *
     * `ref T` 和 `mut ref T` 是**不同的权限**，只能单向降级（`checkAssignable`
     * 里专门处理），绝不能在这里被抹平 —— `intInfo()` 会把 ref 剥掉，
     * 于是 `ref i32` 到 `mut ref i32` 就变成了「i32 拓宽到 i32」而放过去。
     *
     * 这是**第四次**踩这个坑了（`ttBase` / `intInfo` 这类 helper 隐式抹掉 ref
     * 或者不代入泛型实参，把信息悄悄丢掉）。见 DEVLOG。 */
    if (from->kind == TY_REF || to->kind == TY_REF) return false;

    const IntInfo *fi = intInfo(from);
    const IntInfo *ti = intInfo(to);

    /* 整数 → 整数 */
    if (fi && ti) {
        if (fi->sgn == ti->sgn) return fi->bits <= ti->bits;
        if (!fi->sgn && ti->sgn) return fi->bits < ti->bits;   /* u32 → i64 可以；u64 → i64 不行 */
        return false;                                          /* i* → u* 一律不行 */
    }

    /* 整数 → 浮点：只有能精确表示才行 */
    if (fi && ttIsFloat(to)) {
        int mantissa = ttIs(to, "f32") ? 24 : 53;
        return fi->bits <= mantissa;
    }

    /* 浮点 → 浮点 */
    if (ttIsFloat(from) && ttIsFloat(to))
        return ttIs(from, "f32") && ttIs(to, "f64");

    return false;
}

/* ================================================================ 渲染 */

void ttRender(Type *t, Buf *out) {
    if (!t) { bufPuts(out, "void"); return; }
    switch (t->kind) {
        case TY_REF:
            if (t->nullable) bufPuts(out, "?");
            bufPuts(out, t->mut ? "mut ref " : "ref ");
            ttRender(t->inner, out);
            return;
        case TY_GENERIC:
            /* 可写视图要打出 `mut` —— 否则报错信息会成为
             * 「expects `slice<i32>`, found `slice<i32>`」，谁也看不懂 */
            if (t->mut) bufPuts(out, "mut ");
            bufPuts(out, t->sdef->name);
            bufPutc(out, '<');
            for (size_t i = 0; i < t->targs.len; i++) {
                if (i) bufPuts(out, ", ");
                ttRender(*(Type **)vecAt(&t->targs, i), out);
            }
            bufPutc(out, '>');
            return;
        case TY_ARRAY:
            bufPutc(out, '[');
            bufPrintf(out, "%lld", (long long)t->asize);
            bufPutc(out, ']');
            ttRender(t->inner, out);
            return;
        case TY_PARAM: bufPuts(out, t->param); return;
        case TY_VOID:  bufPuts(out, "void"); return;
        case TY_ERROR: bufPuts(out, "<error>"); return;
        default:       bufPuts(out, t->name ? t->name : "?"); return;
    }
}

/* 是不是视图？（见 types.h 的说明） */
bool ttIsViewType(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef && t->targs.len == 1 &&
           strcmp(t->sdef->name, "slice") == 0;
}

/* `got` 能不能当 `want` 用？—— **只许去掉 `mut`（任何一层），不许加。**
 *
 * 为什么去掉永远安全：**`mut` 是权限，不是布局**。C 里
 * `slice<mut slice<i32>>` 和 `slice<slice<i32>>` 是**同一个结构体**
 * （名字都叫 `slice_slice_i32`）⇒ 少一份权限不改变任何字节 ✓
 *
 * 反方向（只读 → 可写）是在**要**权限 ⇒ 必须显式写 `mut` ✓
 *
 * 注意这跟 `ttEquals` 是**两件事**：`ttEquals` 判"是不是同一个类型"（`mut` 算身份），
 * 这里判"能不能当它用"（`mut` 只是权限）✓ */
bool ttViewDowngradable(Type *want, Type *got) {
    if (!want || !got) return false;
    /* 一模一样 ⇒ 通过（元素多半走到这一支：`i32` 不是视图，没法“降”）*/
    if (ttEquals(want, got)) return true;
    if (want->kind != got->kind) return false;
    if (want->kind != TY_GENERIC) return false;
    if (want->sdef != got->sdef || want->targs.len != got->targs.len) return false;
    /* 顶层：要去掉 mut 可以，要加不行 */
    if (got->mut != want->mut && want->mut) return false;
    for (size_t i = 0; i < want->targs.len; i++)
        if (!ttViewDowngradable(*(Type **)vecAt(&want->targs, i),
                                *(Type **)vecAt(&got->targs, i))) return false;
    return true;
}
