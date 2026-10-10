/* The type table: interning, resolving, equality, rendering, and widening.
 *
 * Sits between the parser and the checker. The parser produces type syntax only
 * (names and `TY_UNRESOLVED` nodes); this file turns that into interned types that
 * the checker compares by pointer, and it is the only place that knows how a type
 * is spelled in C. No inference happens here.
 */

#include <stdio.h>
#include <stdlib.h>
#include "types.h"

#include <string.h>

/* ------------------------------------------------------------ built-in types */

/* Names of the built-in types, in the order they are interned; NULL-terminated. */
static const char *BUILTIN_NAMES[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool",
    NULL
};

/**
 * @brief 判断 `name` 是不是内建类型名。
 * @param name  类型名。
 * @return      是内建类型名则 `true`。
 */
bool ttIsBuiltinName(const char *name) {
    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        if (strcmp(BUILTIN_NAMES[i], name) == 0) return true;
    return false;
}

/* Render a type for a diagnostic; defined next to `ttRender` below, declared here because
 * `ttResolve` (which reports the shape errors of a `fn` type) needs it. */
static const char *ttShow(TypeTable *tt, Type *t);

/**
 * @brief 分配一个零初始化的类型节点，记下种类与名字。
 *
 * @param a     拥有新节点的 arena。
 * @param kind  类型种类。
 * @param name  节点的名字：内建类型是源码名，构造出来的类型是 mangle 名。
 * @return      新节点；除种类与名字外的字段全为零。
 */
static Type *mkType(Arena *a, TypeKind kind, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = kind;
    t->name = name;
    return t;
}

/**
 * @brief 建类型表，并把内建类型、`void` 与错误类型驻留进去。
 *
 * @param a  arena：拥有这张表与它里面每一个类型。
 * @param m  立刻注册其声明的模块；NULL 表示不注册。
 * @return   新表，可以直接交给 `ttRegister` 或 `ttResolve`。
 */
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
    vecInit(&tt->fnTypes, a, sizeof(void *));

    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        *(Type **)vecPush(&tt->builtins) = mkType(a, TY_BUILTIN, BUILTIN_NAMES[i]);

    tt->tVoid  = mkType(a, TY_VOID, "void");
    tt->tError = mkType(a, TY_ERROR, "<error>");

    if (m) ttRegister(tt, m);
    return tt;
}

/**
 * @brief 注册一个模块的结构体与枚举声明，已存在的名字跳过。
 *
 * The prelude's declarations and the user files must land in one table：类型靠驻留、相等靠
 * 指针，两张表会让同一个 `bool` 有两个指针，于是每次检查都报"bool 不是 bool"，
 * ttEquals's 指针比较正是靠这一点成立。重复调用是预期内的，且无副作用。
 *
 * @param tt  要注册进去的类型表。
 * @param m   要读的模块；NULL 忽略。
 */
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

/**
 * @brief 取那唯一的 `void` 类型。
 * @param tt  类型表。
 * @return    表里的 `void` 类型。
 */
Type *ttVoid(TypeTable *tt)  { return tt->tVoid; }

/**
 * @brief 取错误类型：解析失败后的结果，用来压住连锁诊断。
 * @param tt  类型表。
 * @return    错误类型。
 */
Type *ttError(TypeTable *tt) { return tt->tError; }

/**
 * @brief 造（或复用）定长数组类型 `[n]elem`。
 *
 * 数组类型与泛型实例一起存在实例表里：两者都需要发 C 结构体，代码生成一样对待。
 *
 * @param tt    类型表。
 * @param n     元素个数，编译期常量。
 * @param elem  元素类型。
 * @return      驻留的数组类型：长度与元素类型都相等就给同一个指针。
 */
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

/**
 * @brief 造一个 `ref T` 类型。
 *
 * 引用**故意不驻留**：它很少见，而且调用方随后会设 `mut` / `nullable`，所以 `ttEquals`
 * 对它们按字段比较。
 *
 * @param tt     拥有新类型的类型表。
 * @param inner  被引用的类型。
 * @return       一个新的、只读、不可空的引用。
 */
Type *ttRef(TypeTable *tt, Type *inner) {
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/**
 * @brief 造一个引用，复制 `src` 的许可标志。
 *
 * `mut` 与 `nullable` 都是类型的一部分，所以解析或替换一个引用时不能把它们丢掉。
 *
 * @param tt     类型表。
 * @param src    提供标志的引用类型。
 * @param inner  新引用指向的类型。
 * @return       带上 `src` 的 `mut` 与 `nullable` 的引用。
 */
static Type *refLike(TypeTable *tt, Type *src, Type *inner) {
    Type *t = ttRef(tt, inner);
    t->mut = src->mut;
    t->nullable = src->nullable;
    return t;
}

/**
 * @brief 按名字查已注册的类型：先内建，再结构体，最后枚举。
 *
 * 只做**裸名**查找：两个模块导出同一个名字时两个都命中，不能随便选一个的调用方得自己
 * 数命中数（见 `ttResolve`）。
 *
 * @param tt    类型表。
 * @param name  源码里的名字，不是模块改名后的名字。
 * @return      那个类型；没有注册过则 `NULL`。结构体 / 枚举的类型节点在首次用到时建好，
 *              并缓存在它的声明上。
 */
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

/**
 * @brief 把解析器造的类型解析成驻留类型。
 *
 * `TY_UNRESOLVED` 名字先与作用域里的类型参数比，再与已注册的声明比；`TY_REF` 保留自己的
 * 标志；带类型实参的泛型名变成对应的实例。
 *
 * 两个导入模块导出同一个裸名时，这是错误而绝不挑一个：所有命中都被数出来，诊断要求写
 * `module::Name`。挑第一个会悄悄绑错类型 —— 程序编过、类型是错的，比拒绝更糟。
 *
 * @param tt      类型表。
 * @param ctx     接收诊断的上下文。
 * @param t       解析器造的类型节点。
 * @param line    诊断归到哪一行。
 * @param params  作用域里的类型参数名；NULL 或空表示不在泛型声明里。
 * @return        驻留后的类型。失败时报告错误并返回错误类型，让调用方继续查下去而不产生
 *                连锁错误。
 */
Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params) {
    if (!t) return NULL;

    switch (t->kind) {
        case TY_UNRESOLVED: {
            /* A type parameter in scope wins over every declaration. */
            if (params) {
                for (size_t i = 0; i < params->len; i++) {
                    if (strcmp(*(const char **)vecAt(params, i), t->name) == 0)
                        return typeParam(tt->arena, t->name, (int)i);
                }
            }

            /* A name mangled for a module is reachable through the alias table
             * (`pair` -> `liba$pair`), and only after the plain lookup failed, so the
             * precedence of this file, the built-ins, and the prelude is unchanged. */
            const char *nm0 = t->name;
            if (!ttFromName(tt, nm0)) {
                /* A bare name may match several aliases: count them instead of "picking the first". */
                const char *hit = NULL;
                int nHit = 0;
                for (size_t i = 0; i < tt->aliases.len; i++) {
                    Alias *al = (Alias *)vecAt(&tt->aliases, i);
                    if (strcmp(al->from, nm0) == 0) { hit = al->to; nHit++; }
                }
                if (nHit >= 2) {
                    ctxError(ctx, line, 1,
                             "More than one imported module exports this name, so a bare name"
                             " cannot say which one you mean. Write `module::Name`.",
                             "ambiguous type `%s` -- %d modules export it, write `module::%s`",
                             t->name, nHit, t->name);
                    return tt->tError;
                }
                if (nHit == 1) nm0 = hit;
            }
            Type *base = ttFromName(tt, nm0);
            if (!base) {
                ctxError(ctx, line, 1,
                         "built-in types: i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool str void",
                         "unknown type `%s`", t->name);
                return tt->tError;
            }

            /* A name with type arguments denotes a generic instance. */
            if (t->targs.len > 0) {
                /* Generic enum (`option<i64>`): payload types are substituted at
                 * each use site, so the declaration itself stays generic. */
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
                /* `coroutine<T>` 是**同型两遍**的简写：请求类型与应答类型都是 T；
                 * `coroutine<A, B>` 才是一般形态，`A` 送给它、`B` 是它 yield 出来的。
                 * 这里**不**把参数补成两个：实例化的 C 名是从写出来的参数算的，补了就会
                 * 让产物改名。读参数的地方按"最后一个参数是应答类型"取值。 */
                bool coroMarker = base->sdef->name && strcmp(base->sdef->name, "coroutine") == 0;
                if (base->sdef->typeParams.len != t->targs.len
                    && !(coroMarker && base->sdef->typeParams.len == 2 && t->targs.len == 1)) {
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
                    /* `mut slice<T>`: only a view has a writable form. Elsewhere
                     * `mut` would mean deep mutability - a larger concept that is
                     * deliberately not implemented, so it is rejected outright. */
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
        /* `fn(A, B) -> R` as the parser wrote it: resolve both halves, then intern through
         * `ttFn`. A name in scope resolves to a type parameter exactly as it does anywhere else,
         * so a template's callback type is `fn(T) -> T` and stays uninterned until instantiation. */
        case TY_FN: {
            Vec np;
            vecInit(&np, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->params.len; i++)
                *(Type **)vecPush(&np) =
                    ttResolve(tt, ctx, *(Type **)vecAt(&t->params, i), line, params);
            Type *fr = ttFn(tt, &np, ttResolve(tt, ctx, t->ret, line, params));

            /* What may a function type carry? The same rule an `extern!` signature obeys, because
             * this is the other way a C function is reached. Without it `fn(slice<u8>) -> void`
             * would be a **lying type**: it declares one argument while the function's C
             * signature takes two, and every call site would compile and call it wrong.
             *
             * Skipped while a type parameter is still in the signature: the answer depends on what
             * `T` becomes, and a template is checked with `T` opaque. */
            /* `ttHasParam` and not the checker's `mentionsParam`: the latter is just this test plus
             * a bare-`TY_PARAM` case, and `fr` is a function type, never a bare parameter. Asking the
             * checker's copy here would make the type table depend on the layer above it. */
            if (!ttHasParam(fr)) {
                if (!ttCrossesC(fr->ret, true))
                    ctxError(ctx, line, 1,
                             "A C function can return `void`, a scalar, or a single pointer; anything"
                             " else has no C-level representation here.",
                             "`fn` return type `%s` cannot cross the C boundary",
                             ttShow(tt, fr->ret));
                for (size_t i = 0; i < fr->params.len; i++) {
                    Type *pt = *(Type **)vecAt(&fr->params, i);
                    if (ttCrossesC(pt, false)) continue;
                    ctxError(ctx, line, 1,
                             "A C argument is one machine word: a scalar, a pointer, or a function"
                             " pointer; a `slice<T>` becomes two C arguments (data + len), and a"
                             " struct crosses by value only when its layout is promised (`@frozen`)."
                             " Write the two arguments out (`s.data` / `s.len`) instead of one.",
                             "`fn` argument %zu has type `%s`, which cannot cross the C boundary",
                             i + 1, ttShow(tt, pt));
                }
            }
            return fr;
        }
                /* kind-default: a kind not listed above carries no unresolved name: the parser only ever builds UNRESOLVED / REF / ARRAY, and every other kind is already interned. A new *syntactic* shape must get a case here, not a default. */
default:
            return t;
    }
}

/* ------------------------------------------------------------------- generics */

/**
 * @brief 判断 `t` 是不是名为 `name` 的类型参数。
 * @param t     要看的类型。
 * @param name  类型参数名。
 * @return      是则 `true`。
 */
bool ttIsParam(Type *t, const char *name) {
    return t && t->kind == TY_PARAM && strcmp(t->param, name) == 0;
}

/**
 * @brief 按用户写的方式命名类型（`io::reader`），而不是按改名后的 C 名（`io$reader`）。
 *
 * @param srcName  模块声明记下的源码限定名；根文件的声明传 NULL 或空串。
 * @param name     没有记下源码名时显示的名字。
 * @return         `srcName` 有值就是它，否则 `name`，再否则 `"?"` —— 永不返回 NULL。
 */
const char *ttDispName(const char *srcName, const char *name) {
    return (srcName && *srcName) ? srcName : (name ? name : "?");
}

/**
 * @brief 把一个类型递归渲染成 C 标识符。
 *
 * @param tt  类型表，兼作生成字符串的 arena。
 * @param t   要渲染的类型；NULL 渲染成 `void`。
 * @return     C 标识符：`Pair<i32, u8>` 变 `Pair_i32_u8`；引用、数组与函数类型各有前缀，
 *             使不同的类型不会撞名。
 */
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
        case TY_FN: {
            /* The name of the C **typedef** that stands for this signature (`extc_fn_i64__i64`).
             * A function type cannot be spelled as a prefix of a declaration the way every other
             * type here can: C wraps the declared name *inside* the pointer declarator
             * (`int64_t (*f)(int64_t)`), so the name has to be introduced by a typedef first and
             * the type text is then just that name.
             *
             * `__` separates the parts because a mangle already joins its own pieces with a single
             * `_` (`pair_i32_u8`), so one `_` would read as part of a name. Two signatures could
             * still collide if a type name itself contained `__`; codegen refuses that case by name
             * instead of emitting two different typedefs under one name. */
            Buf b;
            bufInit(&b, tt->arena);
            bufPuts(&b, "extc_fn_");
            bufPuts(&b, ttMangle(tt, t->ret));
            for (size_t i = 0; i < t->params.len; i++) {
                bufPuts(&b, "__");
                bufPuts(&b, ttMangle(tt, *(Type **)vecAt(&t->params, i)));
            }
            return bufCstr(&b);
        }
                /* kind-default: a kind not listed above has no structure to mangle -- it is an interned name (builtin, struct, payload-free enum), so its own name is already unique. A constructed kind must never land here: `Pair<i32>` and `Pair<u8>` would mangle alike. */
default:       return t->name ? t->name : "?";
    }
}

/**
 * @brief 判断类型在任意深度上有没有提到类型参数。
 *
 * 依赖参数的规则在泛型体内无法判定，要等函数体实例化后再查；这里漏一个 `true` 会让模板被
 * 当成具体类型检查。
 *
 * @param t  要看的类型。
 * @return   `t` 是类型参数，或在引用、数组、泛型实参、函数签名、枚举实例里含有一个，
 *           则 `true`。
 */
bool ttHasParam(Type *t) {
    if (!t) return false;
    if (t->kind == TY_PARAM) return true;
    if (t->kind == TY_REF) return ttHasParam(t->inner);
    /* Arrays must be walked: an array of a type parameter (`[2]box<T>`) is interned
     * unconditionally, because arrays do not go through `ttGeneric`'s "concrete only" rule.
     * Missing this case lets a **provisional** type into `tt->instances`, and from there into
     * the unit list, whose struct definition names an undefined `box_T`. */
    if (t->kind == TY_ARRAY) return ttHasParam(t->inner);
    /* A function type can carry a parameter through its signature (`fn(T) -> T` inside a
     * template), so both halves are inspected. Missing this would let a rule that depends on the
     * parameter be decided as if the signature were concrete. */
    if (t->kind == TY_FN) {
        if (ttHasParam(t->ret)) return true;
        for (size_t i = 0; i < t->params.len; i++)
            if (ttHasParam(*(Type **)vecAt(&t->params, i))) return true;
        return false;
    }
    if (t->kind == TY_GENERIC || t->kind == TY_ENUM) {
        for (size_t i = 0; i < t->targs.len; i++)
            if (ttHasParam(*(Type **)vecAt(&t->targs, i))) return true;
    }
    return false;
}

/**
 * @brief 造（或复用）泛型结构体实例 `sd<args...>`。
 *
 * 只有**完全具体**的实例才驻留，也只有它需要发 C：检查模板时造出来的 `Pair<A, B>` 里是
 * 类型参数，那个实例只用来比类型，驻留它会让代码生成给 `Box_T_set` 发 C。
 *
 * @param tt    类型表。
 * @param sd    泛型结构体声明。
 * @param args  解析好的类型实参，每个声明参数一个。
 * @return      驻留的实例：同一个声明加相等的实参永远给同一个指针。
 */
Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args) {
    bool concrete = true;
    for (size_t j = 0; j < args->len; j++) {
        if (ttHasParam(*(Type **)vecAt(args, j))) { concrete = false; break; }
    }

    if (concrete) {
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

/**
 * @brief 造（或复用）函数类型 `fn(params...) -> ret`。
 *
 * 只有每一部分都具体时才驻留，规则与 `ttGeneric` 相同：检查模板会造出 `fn(T) -> T`，它只
 * 用来比类型，不能发成 C。
 *
 * 函数类型的 C 拼法是**typedef 名**而不是声明符：本编译器把类型写成声明的前缀，而 C 的
 * 函数指针把名字包在里面（`int64_t (*)(int64_t)` 是类型，`int64_t (*f)(int64_t)` 不是
 * “类型加名字”）。名字由 `ttMangle` 产生，typedef 由代码生成发射。
 *
 * @param tt      类型表。
 * @param params  解析好的参数类型，按顺序（`Type *` 元素）；向量会被复制。
 * @param ret     解析好的返回类型；NULL 读作 `void`。
 * @return        驻留的函数类型：同一个签名永远给同一个指针。
 */
Type *ttFn(TypeTable *tt, Vec *params, Type *ret) {
    if (!ret) ret = tt->tVoid;
    bool concrete = !ttHasParam(ret);
    for (size_t i = 0; i < params->len && concrete; i++)
        if (ttHasParam(*(Type **)vecAt(params, i))) concrete = false;

    if (concrete) {
        for (size_t i = 0; i < tt->fnTypes.len; i++) {
            Type *c = *(Type **)vecAt(&tt->fnTypes, i);
            if (c->params.len != params->len) continue;
            if (!ttEquals(c->ret, ret)) continue;
            bool same = true;
            for (size_t j = 0; j < params->len && same; j++)
                same = ttEquals(*(Type **)vecAt(&c->params, j), *(Type **)vecAt(params, j));
            if (same) return c;
        }
    }

    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_FN;
    t->ret  = ret;
    vecInit(&t->params, tt->arena, sizeof(void *));
    for (size_t i = 0; i < params->len; i++)
        *(Type **)vecPush(&t->params) = *(Type **)vecAt(params, i);
    t->name = ttMangle(tt, t);
    if (concrete) *(Type **)vecPush(&tt->fnTypes) = t;
    return t;
}

/**
 * @brief 造（或复用）泛型枚举实例 `td<args...>`（`option<i64>`）。
 *
 * 与 `ttGeneric` 同形，两处不同：
 *   1. 主人是 `TypeDef`（`edef`）而不是 `StructDef`，所以实例驻留在自己的
 *      `enumInstances` 表里。
 *   2. 载荷类型**不**在这里替换。变体载荷写在 `TypeDef` 上（`some(T)`），在每个使用处用
 *      `t->targs` 对着 `edef->typeParams` 替换，于是一个 `TypeDef` 服务每个实例，而实例
 *      只是"名字加实参"。代码生成把实例压成替换上下文，载荷的 C 类型自己就解析出来了。
 *
 * @param tt    类型表。
 * @param td    泛型枚举声明。
 * @param args  解析好的类型实参，每个声明参数一个。
 * @return      驻留的实例；实参里提到类型参数时返回只在模板里用的实例（那个不驻留）。
 */
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

/**
 * @brief 取视图实例的可写影子（`mut slice<T>`）。
 *
 * 影子代替第二个实例，是因为可写视图与只读视图是**同一个 C 结构体** —— 同布局、同名、同
 * 方法集，只差检查器看到的许可。影子因此共用 `base` 的 C 名，也不进实例表，代码生成对两者
 * 只发一个结构体。`mut` 就是这样留成一个限定符，而不是第二个类型。
 *
 * @param tt    类型表。
 * @param base  只读视图实例。
 * @param mut   false 时原样返回 `base`。
 * @return      `base` 的影子实例，首次用到时建、之后复用；不是泛型实例（因而不是视图）的
 *              类型原样返回。
 */
Type *ttViewMut(TypeTable *tt, Type *base, bool mut) {
    if (!mut || !base || base->kind != TY_GENERIC) return base;
    if (base->mut) return base;
    for (size_t i = 0; i < tt->viewShadows.len; i++) {
        Type *s = *(Type **)vecAt(&tt->viewShadows, i);
        if (s->inner == base) return s;          /* `inner` records which view this shadows */
    }
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind  = TY_GENERIC;
    t->sdef  = base->sdef;
    t->targs = base->targs;
    t->name  = base->name;                       /* deliberately the same C name */
    t->inner = base;
    t->mut   = true;
    *(Type **)vecPush(&tt->viewShadows) = t;
    return t;
}

/**
 * @brief 去掉视图类型上的 `mut`：可写视图 -> 只读视图。
 *
 * 递归是重点：元素类型也一起去掉 `mut`，所以 `mut slice<mut slice<T>>` 在任何期待只读
 * `slice<slice<T>>` 的地方都能用。递归是安全的，因为 `mut` 是许可而不是布局：
 * `slice<mut slice<i32>>` 与 `slice<slice<i32>>` 是同一个 C 结构体（都叫
 * `slice_slice_i32`，`mut` 从不出现在 C 类型里），把其中一个传去当另一个用，移动的字节
 * 没有不同。
 *
 * 反方向（只读 -> 可写）永远不允许：没有代码可以凭空造一个许可。
 *
 * @param tt  类型表。
 * @param t   要剥的类型。
 * @return    `t` 的只读形式；什么都没变时返回 `t` 自己。
 */
Type *ttViewReadonly(TypeTable *tt, Type *t) {
    if (!t || t->kind != TY_GENERIC || !t->sdef) return t;

    /* Recurse into the arguments first: even a read-only outer view may hold a
     * writable element (`slice<mut slice<T>>`). */
    bool argChanged = false;
    Vec args;
    vecInit(&args, tt->arena, sizeof(void *));
    for (size_t i = 0; i < t->targs.len; i++) {
        Type *a  = *(Type **)vecAt(&t->targs, i);
        Type *na = ttViewReadonly(tt, a);
        if (na != a) argChanged = true;
        *(Type **)vecPush(&args) = na;
    }

    if (t->mut) return ttGeneric(tt, t->sdef, &args);     /* shadow -> plain instance */
    if (argChanged) return ttGeneric(tt, t->sdef, &args);
    return t;
}

/**
 * @brief 把 `t` 里的类型参数换成具体类型。
 *
 * 用于单态化：`t` 里出现的每个 `params[i]` 都变 `args[i]`，递归穿过引用、数组、泛型实参、
 * 函数签名与枚举实例。这样重建出来的引用保留 `mut` 与 `nullable` 标志，因为它们是类型的一
 * 部分。
 *
 * @param tt      类型表。
 * @param t       要替换进去的类型。
 * @param params  类型参数名，按位置（`const char *` 元素）。
 * @param args    每个名字的具体类型，位置一一对应（`Type *` 元素）。
 * @return        `t` 换掉参数后的类型；无事可做时（没有参数、没有实参，或 `params` 里没有
 *                这个名字）原样返回 `t`。
 */
Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args) {
    if (!t || !params || params->len == 0 || !args) return t;

    switch (t->kind) {
        case TY_PARAM:
            for (size_t i = 0; i < params->len && i < args->len; i++) {
                if (strcmp(*(const char **)vecAt(params, i), t->param) == 0)
                    return *(Type **)vecAt(args, i);
            }
            /* `coroutine<T>` 是"请求与应答同型"的**简写**：标记声明了两个类型参数，实例只给了一个
             * （这是个数检查唯一放行的"少给参数"情形），于是应答类型 B 回落到 A。其余泛型的参数
             * 个数不匹配在类型解析处就被拒了，所以这一条不会替别的类型兜底。 */
            if (params->len == 2 && args->len == 1) return *(Type **)vecAt(args, 0);
            return t;
        case TY_REF:
            return refLike(tt, t, ttSubstitute(tt, t->inner, params, args));
        case TY_ARRAY:
            return ttArray(tt, t->asize, ttSubstitute(tt, t->inner, params, args));
        /* A function type is a constructor too, and it is the one whose parts are *both* walked:
         * a template holding a callback (`fn(T) -> T`) hands back a concrete signature only if the
         * parameter and the return type are substituted here. Falling into `default` would leave
         * `fn(T) -> T` unchanged, and the call site would then want `fn_T__T`. */
        case TY_FN: {
            Vec np;
            vecInit(&np, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->params.len; i++)
                *(Type **)vecPush(&np) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->params, i), params, args);
            return ttFn(tt, &np, ttSubstitute(tt, t->ret, params, args));
        }
        case TY_GENERIC: {
            Vec na;
            vecInit(&na, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->targs.len; i++)
                *(Type **)vecPush(&na) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->targs, i), params, args);
            /* Writability travels with the substitution: `mut` is part of the type. */
            return ttViewMut(tt, ttGeneric(tt, t->sdef, &na), t->mut);
        }
        /* An enum instance needs substituting too: it has kind `TY_ENUM` and not
         * `TY_GENERIC`, so falling into `default` returns it unchanged and a method that
         * returned `option<T>` produces `option_T` instead of `option_i32` ("expects
         * option_i32, found option_T").
         *
         * When a type constructor is added, every function that dispatches on the type kind
         * has to be revisited - `ttSubstitute`, `ttEquals` and `ttRender` at least. */
        case TY_ENUM: {
            if (!t->edef || t->targs.len == 0) return t;
            Vec na;
            vecInit(&na, tt->arena, sizeof(void *));
            for (size_t i = 0; i < t->targs.len; i++)
                *(Type **)vecPush(&na) =
                    ttSubstitute(tt, *(Type **)vecAt(&t->targs, i), params, args);
            return ttEnumGeneric(tt, t->edef, &na);
        }
                /* kind-default: only a kind that **cannot contain a type parameter** may fall here. Parameters are reachable through the targs of REF / ARRAY / GENERIC and through an enum instance -- TY_ENUM is handled above. A new constructor with targs needs its own case, not this default. */
default:
            return t;
    }
}

/* ------------------------------------------------------------------ equality */

/**
 * @brief 判断两个类型是不是同一个类型。
 *
 * 驻留的种类由指针比较结清，只有按需构建的种类才逐字段比较。泛型实例的 `mut` 是身份的一部
 * 分：可写视图不是只读视图。"能不能当那个用"是另一个问题，由 `ttViewDowngradable` 回答。
 *
 * @param a  一个类型；NULL 与任何类型都不相等。
 * @param b  另一个类型。
 * @return   两者描述同一个类型则 `true`。
 */
bool ttEquals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    /* `dyn Trait` has no declaration site -- the parser builds it wherever it is written -- so it
     * belongs with the kinds below that are compared structurally rather than by interning. */
    if (a->kind == TY_DYN)
        return a->name && b->name && strcmp(a->name, b->name) == 0;

    /* Every kind except references, type parameters, and generic instances is
     * interned, so differing pointers already mean differing types. */
    if (a->kind == TY_REF)
        return a->mut == b->mut && a->nullable == b->nullable &&
               ttEquals(a->inner, b->inner);

    if (a->kind == TY_ARRAY)
        return a->asize == b->asize && ttEquals(a->inner, b->inner);

    if (a->kind == TY_PARAM)
        return a->tpIndex == b->tpIndex && strcmp(a->param, b->param) == 0;

    /* A function type is interned when it is concrete and built fresh when it is not
     * (`ttFn`), so like a generic instance it needs the structural comparison: the parser's
     * `fn(T) -> T` inside a template and the one `ttSubstitute` builds are two pointers. */
    if (a->kind == TY_FN) {
        if (!ttEquals(a->ret, b->ret) || a->params.len != b->params.len) return false;
        for (size_t i = 0; i < a->params.len; i++)
            if (!ttEquals(*(Type **)vecAt(&a->params, i), *(Type **)vecAt(&b->params, i)))
                return false;
        return true;
    }

    if (a->kind == TY_GENERIC) {
        if (a->mut != b->mut) return false;   /* a writable view is not the read-only view */
        if (a->sdef != b->sdef || a->targs.len != b->targs.len) return false;
        for (size_t i = 0; i < a->targs.len; i++)
            if (!ttEquals(*(Type **)vecAt(&a->targs, i), *(Type **)vecAt(&b->targs, i)))
                return false;
        return true;
    }

    /* A generic enum instance (`option<i64>`, `result<T, E>`) is an enum kind carrying
     * type arguments, and unlike the kinds above it is *not* interned: the parser builds
     * one object and `ttSubstitute` builds another, so two structurally identical
     * instances can be two different pointers. Comparing the definition and the arguments
     * is what "the same type" means here; falling through to the pointer comparison below
     * would make them unequal and reject a `return` of `?T` that another generic instance
     * handed back ("expects `option<T>`, found `option<T>`"). */
    if (a->kind == TY_ENUM) {
        if (a->edef != b->edef || a->targs.len != b->targs.len) return false;
        for (size_t i = 0; i < a->targs.len; i++)
            if (!ttEquals(*(Type **)vecAt(&a->targs, i), *(Type **)vecAt(&b->targs, i)))
                return false;
        return true;
    }
    return false;
}

/**
 * @brief 判断 `t` 是不是内建类型 `builtinName`，忽略引用。
 * @param t            要看的类型。
 * @param builtinName  内建类型名。
 * @return             剥掉引用后是那个内建类型则 `true`。
 */
bool ttIs(Type *t, const char *builtinName) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN && strcmp(b->name, builtinName) == 0;
}

/**
 * @brief 判断 `t` 是不是解析失败产生的错误类型。
 * @param t  要看的类型。
 * @return   是错误类型则 `true`。
 */
bool ttIsError(Type *t) {
    return t && t->kind == TY_ERROR;
}

/**
 * @brief 剥掉所有引用：`ref ref T` -> `T`。
 * @param t  要剥的类型。
 * @return   最里层的非引用类型；`t` 为 NULL 时返回 NULL。
 */
Type *ttBase(Type *t) {
    while (t && t->kind == TY_REF) t = t->inner;
    return t;
}

/* ------------------------------------------------------- numeric categories */

/* Width and signedness of one built-in integer type. */
typedef struct { const char *name; int bits; bool sgn; } IntInfo;

static const IntInfo INTS[] = {
    { "i8",  8, true  }, { "i16", 16, true  }, { "i32", 32, true  }, { "i64", 64, true  },
    { "u8",  8, false }, { "u16", 16, false }, { "u32", 32, false }, { "u64", 64, false },
    { NULL, 0, false }
};

/**
 * @brief 查一个类型的整数描述符，先剥掉引用。
 *
 * @param t  要分类的类型。
 * @return   内建整数类型的描述符；其余情况返回 NULL（先剥引用，所以 `ref i32` 按 `i32`
 *           分类）。
 */
static const IntInfo *intInfo(Type *t) {
    Type *b = ttBase(t);
    if (!b || b->kind != TY_BUILTIN) return NULL;
    for (size_t i = 0; INTS[i].name; i++)
        if (strcmp(INTS[i].name, b->name) == 0) return &INTS[i];
    return NULL;
}

/**
 * @brief 判断 `t` 是不是某个内建整数类型。
 * @param t  要看的类型。
 * @return   剥掉引用后是整数则 `true`。
 */
bool ttIsInteger(Type *t) { return intInfo(t) != NULL; }

/**
 * @brief 判断 `t` 是不是 `f32` 或 `f64`。
 * @param t  要看的类型。
 * @return   剥掉引用后是浮点则 `true`。
 */
bool ttIsFloat(Type *t) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN &&
           (strcmp(b->name, "f32") == 0 || strcmp(b->name, "f64") == 0);
}

/**
 * @brief 判断 `t` 是不是整数或浮点。
 * @param t  要看的类型。
 * @return   剥掉引用后是数值类型则 `true`。
 */
bool ttIsNumeric(Type *t) { return ttIsInteger(t) || ttIsFloat(t); }

/**
 * @brief 整数类型的位宽。
 * @param t  要看的类型。
 * @return   位宽；剥掉引用后不是整数则返回 0。
 */
int ttIntBits(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->bits : 0;
}

/**
 * @brief 判断整数类型有没有符号。
 * @param t  要看的类型。
 * @return   有符号则 `true`；非整数类型返回 `false`。
 */
bool ttIntSigned(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->sgn : false;
}

/* ------------------------------------------------------------- widening rules
 *
 * Only lossless implicit conversions are allowed; narrowing never is. This is what
 * "every conversion must state its intent" means in practice: a lossy conversion has
 * to name a method (`.truncate()`, `.round()`, ...), so the loss is visible in the
 * source.
 *
 * `i32 -> f32` counts as lossy too - an `f32` mantissa holds only 24 bits - so it is
 * not allowed implicitly either. Literals are exempt from this rule: a literal's type
 * adapts to its value, which the checker handles at the literal itself.
 */

/**
 * @brief 判断 `from` 类型的值能不能用在期待 `to` 的地方（隐式加宽）。
 *
 * 只有无损转换才允许隐式发生。引用从不加宽：`ref T` 与 `mut ref T` 是两种许可，只能单向
 * 降级，那在赋值检查处处理。引用测试必须排在整数助手之前，因为那些助手会剥引用：少了它，
 * `ref i32` 到 `mut ref i32` 看起来像"i32 加宽到 i32"，于是被接受。
 *
 * 错误类型与一切互相加宽，于是一次失败的类型不会产生连锁诊断。
 *
 * @param from  值的类型。
 * @param to    会被转换成的类型。
 * @return      转换无损、因而允许隐式发生则 `true`。
 */
bool ttCanWiden(Type *from, Type *to) {
    if (!from || !to) return false;
    if (ttIsError(from) || ttIsError(to)) return true;
    if (ttEquals(from, to)) return true;

    if (from->kind == TY_REF || to->kind == TY_REF) return false;

    const IntInfo *fi = intInfo(from);
    const IntInfo *ti = intInfo(to);

    /* Integer to integer: keep the sign and do not grow past the target width. */
    if (fi && ti) {
        if (fi->sgn == ti->sgn) return fi->bits <= ti->bits;
        /* Unsigned fits a wider signed target: u32 -> i64 is fine, u64 -> i64 is not. */
        if (!fi->sgn && ti->sgn) return fi->bits < ti->bits;
        return false;                                   /* signed -> unsigned is never safe */
    }

    /* Integer to float: only when every bit of the integer fits the mantissa. */
    if (fi && ttIsFloat(to)) {
        int mantissa = ttIs(to, "f32") ? 24 : 53;
        return fi->bits <= mantissa;
    }

    /* Float to float: only the wider target can hold every `f32` exactly. */
    if (ttIsFloat(from) && ttIsFloat(to))
        return ttIs(from, "f32") && ttIs(to, "f64");

    return false;
}

/* ----------------------------------------------------------------- rendering */

/**
 * @brief 把 `t` 面向用户的拼法追加到 `out`，递归进入它的各部分。
 *
 * 可写视图必须打出开头的 `mut`，否则消息读起来是"expects `slice<i32>`, found
 * `slice<i32>`"，什么也没告诉用户。声明名走 `ttDispName`，所以模块类型打印成
 * `io::reader`，绝不是改名后的 `io$reader`；暴露内部编码等于说出用户没写过的词。
 *
 * @param t    要渲染的类型；NULL 渲染成 `void`。
 * @param out  接收文本的缓冲区。
 */
void ttRender(Type *t, Buf *out) {
    if (!t) { bufPuts(out, "void"); return; }
    switch (t->kind) {
        case TY_REF:
            if (t->nullable) bufPuts(out, "?");
            bufPuts(out, t->mut ? "mut ref " : "ref ");
            ttRender(t->inner, out);
            return;
        case TY_GENERIC:
            /* Print `mut` for a writable view, or the message reads "expects `slice<i32>`,
             * found `slice<i32>`" and tells the user nothing. */
            if (t->mut) bufPuts(out, "mut ");
            bufPuts(out, ttDispName(t->sdef->srcName, t->sdef->name));
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
        case TY_FN:
            /* The user-facing spelling is the one they wrote: `fn(i64) -> i64`. Printing the C
             * typedef name here would show a word the author never wrote (`extc_fn_i64__i64`), which
             * is the same mistake `ttDispName` exists to prevent for module types. */
            bufPuts(out, "fn(");
            for (size_t i = 0; i < t->params.len; i++) {
                if (i) bufPuts(out, ", ");
                ttRender(*(Type **)vecAt(&t->params, i), out);
            }
            bufPuts(out, ") -> ");
            ttRender(t->ret, out);
            return;
        case TY_ENUM:
            /* A generic enum instance carries type arguments (`option<i64>`), so the
             * arguments have to be printed too: otherwise two *different* instances render
             * as "expects `option`, found `option`", which says nothing about which two
             * types are meant. */
            if (t->edef) bufPuts(out, ttDispName(t->edef->srcName, t->edef->name));
            else         bufPuts(out, t->name ? t->name : "?");
            if (t->targs.len > 0) {
                bufPutc(out, '<');
                for (size_t i = 0; i < t->targs.len; i++) {
                    if (i) bufPuts(out, ", ");
                    ttRender(*(Type **)vecAt(&t->targs, i), out);
                }
                bufPutc(out, '>');
            }
            return;
                /* kind-default: every kind not special-cased above prints by its own name, which is what the reader wrote or what mangling produced; a new kind is expected to be name-printable unless it is given a case here. */
default: {
            if (t->sdef)      { bufPuts(out, ttDispName(t->sdef->srcName, t->sdef->name)); return; }
            if (t->edef)      { bufPuts(out, ttDispName(t->edef->srcName, t->edef->name)); return; }
            bufPuts(out, t->name ? t->name : "?");
            return;
        }
    }
}

/**
 * @brief 把类型渲染进诊断用的 arena 字符串。
 *
 * @param tt  类型表，兼作 arena。
 * @param t   要渲染的类型。
 * @return    渲染结果，存放在 `tt` 的 arena 里。
 */
static const char *ttShow(TypeTable *tt, Type *t) {
    Buf b;
    bufInit(&b, tt->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/**
 * @brief 判断一个类型的值过 C 边界时是不是**一个** C 参数。
 *
 * 一个拼写给两个问的人：`extern!` 签名与函数类型（`fn(A) -> R`），这是够到 C 函数的两条
 * 路。两条路各写一份判断，边界就会漂移，漂移的形状是：`fn(slice<u8>) -> void` 声明一个
 * 实参，而 C 函数收两个，于是每个调用点都编得过、都调错。
 *
 * **指向任何东西的指针**都过一个参数：平台 ABI 传所有对象指针的方式一样，所以指向什么只
 * 影响声明读起来清不清楚，不影响调用对不对 —— `ref point`、`ref Pair<i64, u8>` 与
 * `ref void` 各是一个机器字。按值的聚合体只有标了 `@frozen` 才过：that is the author's
 * promise that this struct's bytes are laid out the way C lays out this field list，不是
 * 编译器自己保证的。`slice<T>` 是两个 C 参数（data + len，调用方自己写出 `s.data` 与
 * `s.len`）；`void` 按值不是一个值，只做返回类型。
 *
 * @param t         实参的类型，或返回值的类型。
 * @param isReturn  返回类型时为 true，那里 `void` 合法。
 * @return          是一个 C 参数则 `true`：标量，或**指向任何东西的指针**（`ref T` /
 *                  `mut ref T` / `?ref T` / `ref void`），或函数类型。
 */
bool ttCrossesC(Type *t, bool isReturn) {
    if (!t) return false;
    /* `ref T` for any T, including `void`: one pointer. `?ref T` is the same word with a null
     * allowed, which is C's own convention for a pointer it may hand back as null. */
    if (t->kind == TY_REF) return true;
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    /* A function type crosses as one pointer, and it is a pointer whatever it points at -- C passes
     * those freely (`qsort`'s comparator is exactly this shape). */
    if (b->kind == TY_FN) return true;
    /* An aggregate **by value**: only with `@frozen`, which is the author's promise that this
     * struct's bytes are laid out the way C lays out this field list. The promise is what makes it
     * safe, not the compiler's own layout rules: a reordering would be a **silent** wrong answer at
     * the call. Without the marker the answer stays no -- and the diagnostic says which marker to
     * write. */
    if ((b->kind == TY_STRUCT || b->kind == TY_GENERIC) && b->sdef && b->sdef->frozen) return true;
    return isReturn && b->kind == TY_VOID;
}

/**
 * @brief 判断 `t` 是不是视图类型。
 *
 * 视图按形状认：名字是 `slice`、正好带一个类型实参 —— 与编译器认 `data` + `len` 那个形状
 * （`s[i]` 的定义所在）是同一种做法。
 *
 * @param t  要看的类型。
 * @return   是视图则 `true`。
 */
bool ttIsViewType(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef && t->targs.len == 1 &&
           strcmp(t->sdef->name, "slice") == 0;
}

/**
 * @brief 判断 `got` 类型的值能不能用在期待 `want` 的地方，允许在任意深度去掉 `mut`。
 *
 * `mut` 只能去掉、不能加上：它是**许可**而不是布局 —— `slice<mut slice<i32>>` 与
 * `slice<slice<i32>>` 是同一个 C 结构体（都叫 `slice_slice_i32`），少一层许可不改变任何
 * 字节；反方向是在**要**一个许可，所以源码必须自己写出 `mut`。
 *
 * 这不是 `ttEquals`：相等问"是不是同一个类型"（那里 `mut` 是身份的一部分），这里问
 * "能不能当那个用"（`mut` 只是许可）。
 *
 * @param want  上下文期待的类型。
 * @param got   值的类型。
 * @return      `got` 可以当 `want` 用则 `true`。
 */
bool ttViewDowngradable(Type *want, Type *got) {
    if (!want || !got) return false;
    /* Identical types pass. Element types usually land here: `i32` is not a view
     * and has no `mut` to drop. */
    if (ttEquals(want, got)) return true;
    if (want->kind != got->kind) return false;
    if (want->kind != TY_GENERIC) return false;
    if (want->sdef != got->sdef || want->targs.len != got->targs.len) return false;
    /* At the top level `mut` may be dropped but not added. */
    if (got->mut != want->mut && want->mut) return false;
    for (size_t i = 0; i < want->targs.len; i++)
        if (!ttViewDowngradable(*(Type **)vecAt(&want->targs, i),
                                *(Type **)vecAt(&got->targs, i))) return false;
    return true;
}
