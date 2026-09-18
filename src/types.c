#include "types.h"

#include <string.h>

/* ================================================================ 内建类型 */

static const char *BUILTIN_NAMES[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool", "str",
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
    vecInit(&tt->instances, a, sizeof(void *));

    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        *(Type **)vecPush(&tt->builtins) = mkType(a, TY_BUILTIN, BUILTIN_NAMES[i]);

    tt->tVoid  = mkType(a, TY_VOID, "void");
    tt->tError = mkType(a, TY_ERROR, "<error>");

    if (m) {
        for (size_t i = 0; i < m->structs.len; i++)
            *(StructDef **)vecPush(&tt->structs) = *(StructDef **)vecAt(&m->structs, i);
        for (size_t i = 0; i < m->types.len; i++)
            *(TypeDef **)vecPush(&tt->enums) = *(TypeDef **)vecAt(&m->types, i);
    }
    return tt;
}

Type *ttVoid(TypeTable *tt)  { return tt->tVoid; }
Type *ttError(TypeTable *tt) { return tt->tError; }

Type *ttRef(TypeTable *tt, Type *inner) {
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
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

            Type *base = ttFromName(tt, t->name);
            if (!base) {
                ctxError(ctx, line, 1,
                         "内建类型是 i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/bool/str/void",
                         "unknown type `%s`", t->name);
                return tt->tError;
            }

            /* 2) 带类型实参 → 泛型实例 */
            if (t->targs.len > 0) {
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
                return ttGeneric(tt, base->sdef, &args);
            }

            /* 3) 泛型 struct 不带实参 → 报错 */
            if (base->kind == TY_STRUCT && base->sdef && base->sdef->typeParams.len > 0) {
                ctxError(ctx, line, 1,
                         "泛型要写出实参，例如 `%s<i32>`",
                         "`%s` is generic and needs type arguments", t->name);
                return tt->tError;
            }
            return base;
        }
        case TY_REF:
            return ttRef(tt, ttResolve(tt, ctx, t->inner, line, params));
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

static bool typeHasParam(Type *t) {
    if (!t) return false;
    if (t->kind == TY_PARAM) return true;
    if (t->kind == TY_REF) return typeHasParam(t->inner);
    if (t->kind == TY_GENERIC) {
        for (size_t i = 0; i < t->targs.len; i++)
            if (typeHasParam(*(Type **)vecAt(&t->targs, i))) return true;
    }
    return false;
}

Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args) {
    /* 只有**完全具体**的实例才驻留、才需要生成 C。
     * 检查模板时会出现 `Pair<A, B>`（实参是类型参数）—— 那只是拿来比类型的，
     * 绝不能混进实例表，否则 codegen 会去生成 `Box_T_set` 这种东西。 */
    bool concrete = true;
    for (size_t j = 0; j < args->len; j++) {
        if (typeHasParam(*(Type **)vecAt(args, j))) { concrete = false; break; }
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
            return ttRef(tt, ttSubstitute(tt, t->inner, params, args));
        case TY_GENERIC: {
            Vec na;
            vecInit(&na, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->targs.len; i++)
                *(Type **)vecPush(&na) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->targs, i), params, args);
            return ttGeneric(tt, t->sdef, &na);
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
    if (a->kind == TY_REF) return ttEquals(a->inner, b->inner);

    if (a->kind == TY_PARAM)
        return a->tpIndex == b->tpIndex && strcmp(a->param, b->param) == 0;

    if (a->kind == TY_GENERIC) {
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
            bufPuts(out, "ref ");
            ttRender(t->inner, out);
            return;
        case TY_GENERIC:
            bufPuts(out, t->sdef->name);
            bufPutc(out, '<');
            for (size_t i = 0; i < t->targs.len; i++) {
                if (i) bufPuts(out, ", ");
                ttRender(*(Type **)vecAt(&t->targs, i), out);
            }
            bufPutc(out, '>');
            return;
        case TY_PARAM: bufPuts(out, t->param); return;
        case TY_VOID:  bufPuts(out, "void"); return;
        case TY_ERROR: bufPuts(out, "<error>"); return;
        default:       bufPuts(out, t->name ? t->name : "?"); return;
    }
}
