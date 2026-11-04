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
 * @brief Tell whether `name` is a builtin type name.
 *
 * @param name  Type name.
 * @return      `true` when it is a builtin type name.
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
 * @brief Allocate a zero-initialized type node and record its kind and name.
 *
 * @param a     Arena that owns the new node.
 * @param kind  Type kind.
 * @param name  The node's name: the source name for a builtin type, the mangled name for a
 *              constructed type.
 * @return      The new node; every field other than the kind and the name is zero.
 */
static Type *mkType(Arena *a, TypeKind kind, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = kind;
    t->name = name;
    return t;
}

/**
 * @brief Build a type table and intern the builtin types, `void`, and the error type into it.
 *
 * @param a  Arena: owns this table and every type in it.
 * @param m  Module whose declarations are registered immediately; NULL registers nothing.
 * @return   The new table, ready to hand to `ttRegister` or `ttResolve`.
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
    tt->tAny   = mkType(a, TY_ANY, "any");
    tt->tError = mkType(a, TY_ERROR, "<error>");

    if (m) ttRegister(tt, m);
    return tt;
}

/**
 * @brief Register one module's struct and enum declarations; names that already exist are
 * skipped.
 *
 * The prelude's declarations and the user files must land in one table: types are interned and
 * compared by pointer, and two tables would give the same `bool` two pointers, so every check
 * would report "bool is not bool"; `ttEquals`'s pointer comparison rests on exactly this.
 * Repeated calls are expected and have no side effects.
 *
 * @param tt  The type table to register into.
 * @param m   Module to read; NULL is ignored.
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
 * @brief Get the one `void` type.
 *
 * @param tt  Type table.
 * @return    The table's `void` type.
 */
Type *ttVoid(TypeTable *tt)  { return tt->tVoid; }

Type *ttAny(TypeTable *tt)   { return tt->tAny; }

/**
 * @brief Get the error type: the result of a failed parse; it suppresses cascading
 * diagnostics.
 *
 * @param tt  Type table.
 * @return    The error type.
 */
Type *ttError(TypeTable *tt) { return tt->tError; }

/**
 * @brief Build (or reuse) a fixed-length array type `[n]elem`.
 *
 * Array types live in the instance table together with generic instances: both need a C struct
 * emitted, and codegen treats them the same way.
 *
 * @param tt    Type table.
 * @param n     Number of elements, a compile-time constant.
 * @param elem  Element type.
 * @return      The interned array type: equal length and element type yield the same pointer.
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
 * @brief Intern a fixed-array type whose length is a `const` parameter.
 *
 * Kept apart from `ttArray` so a symbolic length can never be mistaken for a real one: the
 * length field is `-1` and the parameter's name is the identity. Two signatures that both write
 * `[N]T` share this node, and instantiation replaces it with a concrete `ttArray`.
 *
 * @param tt        Type table that owns the type.
 * @param lenParam  Name of the `const` parameter.
 * @param elem      Element type.
 * @return          The interned symbolic array type.
 */
Type *ttArraySym(TypeTable *tt, const char *lenParam, Type *elem) {
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *c = *(Type **)vecAt(&tt->instances, i);
        if (c->kind != TY_ARRAY || c->asize >= 0 || !c->param) continue;
        if (strcmp(c->param, lenParam) == 0 && ttEquals(c->inner, elem)) return c;
    }
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_ARRAY;
    t->asize = -1;
    t->param = lenParam;
    t->inner = elem;
    t->name = ttMangle(tt, t);
    *(Type **)vecPush(&tt->instances) = t;
    return t;
}

/**
 * @brief Build a `ref T` type.
 *
 * A reference is **deliberately not interned**: it is rare, and the caller sets `mut` /
 * `nullable` on it afterwards, so `ttEquals` compares them field by field instead.
 *
 * @param tt     Type table that owns the new type.
 * @param inner  The referenced type.
 * @return       A new read-only, non-nullable reference.
 */
Type *ttRef(TypeTable *tt, Type *inner) {
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/**
 * @brief Build a reference that copies `src`'s permission flags.
 *
 * Both `mut` and `nullable` are part of the type, so they must not be lost when a reference is
 * resolved or substituted.
 *
 * @param tt     Type table.
 * @param src    The reference type that provides the flags.
 * @param inner  The type the new reference points to.
 * @return       A reference carrying `src`'s `mut` and `nullable`.
 */
static Type *refLike(TypeTable *tt, Type *src, Type *inner) {
    Type *t = ttRef(tt, inner);
    t->mut = src->mut;
    t->nullable = src->nullable;
    return t;
}

/**
 * @brief Look up a registered type by name: builtins first, then structs, then enums.
 *
 * Only a **bare name** is looked up: when two modules export the same name both matches are
 * returned, and a caller that may not pick one has to count the matches itself (see
 * `ttResolve`).
 *
 * @param tt    Type table.
 * @param name  The name as written in the source, not the module-renamed name.
 * @return      That type; `NULL` when it was never registered. A struct's / enum's type node is
 *              built on first use and cached on its declaration.
 */
Type *ttFromName(TypeTable *tt, const char *name) {
    if (strcmp(name, "void") == 0) return tt->tVoid;
    if (strcmp(name, "any") == 0) return tt->tAny;

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
 * @brief Resolve a type the parser built into an interned type.
 *
 * A `TY_UNRESOLVED` name is first compared against the type parameters in scope, then against
 * the registered declarations; `TY_REF` keeps its own flags; a generic name with type arguments
 * becomes the corresponding instance.
 *
 * When two imported modules export the same bare name this is an error and never a pick: all
 * matches are counted and the diagnostic asks for `module::Name`. Picking the first would
 * silently bind the wrong type -- a program that compiles with the wrong type is worse than a
 * rejection.
 *
 * @param tt      Type table.
 * @param ctx     Context that receives diagnostics.
 * @param t       The type node the parser built.
 * @param line    Which line the diagnostic is attributed to.
 * @param params  Type parameter names in scope; NULL or empty means "not in a generic
 *                declaration".
 * @return        The interned type. On failure an error is reported and the error type is
 *                returned, so the caller keeps checking without producing cascading errors.
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
                /* `coroutine<T>` is shorthand for "request and response are the same type";
                 * `coroutine<A, B>` is the general form, where `A` is sent to it and `B` is
                 * what it yields. The parameter is **not** padded to two here: an instance's C
                 * name is computed from the arguments as written, and padding would rename the
                 * product. The parameter reader takes "the last argument is the response
                 * type". */
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
                         "a generic needs explicit type arguments, e.g. `Pair<i32, i32>`",
                         "`%s` is generic and needs type arguments", t->name);
                return tt->tError;
            }
            if (base->kind == TY_ENUM && base->edef && base->edef->typeParams.len > 0) {
                ctxError(ctx, line, 1,
                         "a generic needs explicit type arguments, e.g. `Pair<i32, i32>`",
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
            if (t->asize < 0 && t->param) return ttArraySym(tt, t->param, e);
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
 * @brief Tell whether `t` is the type parameter named `name`.
 *
 * @param t     The type to inspect.
 * @param name  Type parameter name.
 * @return      `true` when it is.
 */
bool ttIsParam(Type *t, const char *name) {
    return t && t->kind == TY_PARAM && t->param && name && strcmp(t->param, name) == 0;
}

/**
 * @brief Name a type the way the user writes it (`io::reader`), not by its renamed C name
 * (`io$reader`).
 *
 * @param srcName  The source qualified name the module declaration recorded; NULL or empty for a
 *                 root file declaration.
 * @param name     The name to show when no source name was recorded.
 * @return         `srcName` when set, else `name`, else `"?"` -- never NULL.
 */
const char *ttDispName(const char *srcName, const char *name) {
    return (srcName && *srcName) ? srcName : (name ? name : "?");
}

/**
 * @brief Render a type recursively into a C identifier.
 *
 * @param tt  Type table, also used as the arena for the generated string.
 * @param t   The type to render; NULL renders as `void`.
 * @return    The C identifier: `Pair<i32, u8>` becomes `Pair_i32_u8`; references, arrays, and
 *            function types each get their own prefix, so different types cannot collide.
 */
const char *ttMangle(TypeTable *tt, Type *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TY_REF:
            return arenaPrintf(tt->arena, "Ref_%s", ttMangle(tt, t->inner));
        case TY_ARRAY:
            if (t->asize < 0 && t->param)
                return arenaPrintf(tt->arena, "array_P%s_%s", t->param,
                                   ttMangle(tt, t->inner));
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
        case TY_ANY: return "any";
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
 * @brief Tell whether a type mentions a type parameter at any depth.
 *
 * Rules that depend on a parameter cannot be decided inside a generic body and have to wait
 * until the function body is instantiated; missing one `true` here makes a template be checked
 * as a concrete type.
 *
 * @param t  The type to inspect.
 * @return   `true` when `t` is a type parameter, or contains one inside a reference, array,
 *           generic argument, function signature, or enum instance.
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
 * @brief Build (or reuse) a generic struct instance `sd<args...>`.
 *
 * Only a **fully concrete** instance is interned, and only such an instance needs C emitted: the
 * `Pair<A, B>` built while checking a template holds type parameters, that instance only serves
 * type comparison, and interning it would make codegen emit C for `Box_T_set`.
 *
 * @param tt    Type table.
 * @param sd    Generic struct declaration.
 * @param args  Resolved type arguments, one per declared parameter.
 * @return      The interned instance: the same declaration plus equal arguments always yields
 *              the same pointer.
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
 * @brief Build (or reuse) a function type `fn(params...) -> ret`.
 *
 * It is interned only when every part is concrete, by the same rule as `ttGeneric`: checking a
 * template builds `fn(T) -> T`, which only serves type comparison and must not be emitted as C.
 *
 * A function type's C spelling is a **typedef name**, not a declarator: this compiler writes
 * types as declaration prefixes, while C function pointers wrap the name inside
 * (`int64_t (*)(int64_t)` is a type, `int64_t (*f)(int64_t)` is not "a type plus a name"). The
 * name comes from `ttMangle` and codegen emits the typedef.
 *
 * @param tt      Type table.
 * @param params  Resolved parameter types, in order (`Type *` elements); the vector is copied.
 * @param ret     Resolved return type; NULL reads as `void`.
 * @return        The interned function type: the same signature always yields the same pointer.
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
 * @brief Build (or reuse) a generic enum instance `td<args...>` (`option<i64>`).
 *
 * Same shape as `ttGeneric`, with two differences:
 *   1. The owner is a `TypeDef` (`edef`) rather than a `StructDef`, so instances are interned in
 *      their own `enumInstances` table.
 *   2. Payload types are **not** substituted here. Variant payloads are written on the
 *      `TypeDef` (`some(T)`), and at each use site `t->targs` is substituted against
 *      `edef->typeParams`, so one `TypeDef` serves every instance and an instance is only "a
 *      name plus arguments". Codegen turns the instance into a substitution context, and the
 *      payload's C type falls out by itself.
 *
 * @param tt    Type table.
 * @param td    Generic enum declaration.
 * @param args  Resolved type arguments, one per declared parameter.
 * @return      The interned instance; when the arguments mention type parameters, an instance
 *              used only inside templates (that one is not interned).
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
 * @brief Get a view instance's writable shadow (`mut slice<T>`).
 *
 * A shadow replaces a second instance because the writable view and the read-only view are
 * **the same C struct** -- same layout, same name, same method set, differing only in the
 * permission the checker sees. The shadow therefore shares `base`'s C name and does not enter
 * the instance table, and codegen emits one struct for both. That is how `mut` stays a
 * qualifier instead of becoming a second type.
 *
 * @param tt    Type table.
 * @param base  The read-only view instance.
 * @param mut   When false, `base` is returned unchanged.
 * @return      `base`'s shadow instance, built on first use and reused afterwards; a type that
 *              is not a generic instance (and therefore not a view) is returned unchanged.
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
 * @brief Drop `mut` from a view type: writable view -> read-only view.
 *
 * The recursion is the point: the element type loses `mut` as well, so
 * `mut slice<mut slice<T>>` can be used wherever a read-only `slice<slice<T>>` is expected. The
 * recursion is safe because `mut` is a permission and not layout: `slice<mut slice<i32>>` and
 * `slice<slice<i32>>` are the same C struct (both called `slice_slice_i32`, `mut` never appears
 * in a C type), and moving bytes between them makes no difference.
 *
 * The other direction (read-only -> writable) is never allowed: no code can conjure a permission
 * out of nothing.
 *
 * @param tt  Type table.
 * @param t   The type to strip.
 * @return    The read-only form of `t`; `t` itself when nothing changed.
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
 * @brief Substitute concrete types for the type parameters in `t`.
 *
 * Used by monomorphization: every `params[i]` occurring in `t` becomes `args[i]`, recursing
 * through references, arrays, generic arguments, function signatures, and enum instances. The
 * references rebuilt this way keep their `mut` and `nullable` flags, because they are part of
 * the type.
 *
 * @param tt      Type table.
 * @param t       The type to substitute into.
 * @param params  Type parameter names, by position (`const char *` elements).
 * @param args    The concrete type for each name, positionally corresponding to `params`
 *                (`Type *` elements).
 * @return        `t` with the parameters replaced; when there is nothing to do (no parameters,
 *                no arguments, or `params` does not contain the name) `t` is returned unchanged.
 */
Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args) {
    if (!t || !params || params->len == 0 || !args) return t;

    switch (t->kind) {
        case TY_PARAM:
            for (size_t i = 0; i < params->len && i < args->len; i++) {
                if (strcmp(*(const char **)vecAt(params, i), t->param) == 0)
                    return *(Type **)vecAt(args, i);
            }
            /* `coroutine<T>` is **shorthand** for "request and response are the same type": the
             * marker declares two type parameters and the instance supplies only one (the only
             * "too few arguments" case the count check lets through), so the response type B
             * falls back to A. Every other generic's argument count mismatch is rejected during
             * type resolution, so this does not paper over any other type. */
            if (params->len == 2 && args->len == 1) return *(Type **)vecAt(args, 0);
            return t;
        case TY_REF:
            return refLike(tt, t, ttSubstitute(tt, t->inner, params, args));
        case TY_ARRAY: {
            Type *inner = ttSubstitute(tt, t->inner, params, args);
            if (t->asize >= 0 || !t->param) return ttArray(tt, t->asize, inner);
            /* The length is a `const` parameter: its argument is the array type the caller passed
             * (`[3]f64`), so the concrete length comes from there while the element type comes from
             * this type. A missing or non-array binding leaves the type symbolic, and the checker
             * reports that the length could not be inferred. */
            for (size_t i = 0; i < params->len; i++) {
                if (strcmp(*(const char **)vecAt(params, i), t->param) != 0) continue;
                Type *b = (i < args->len) ? *(Type **)vecAt(args, i) : NULL;
                if (b && b->kind == TY_ARRAY && b->asize >= 0) return ttArray(tt, b->asize, inner);
                break;
            }
            return ttArraySym(tt, t->param, inner);
        }
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
 * @brief Tell whether two types are the same type.
 *
 * Interned kinds settle by pointer comparison; only the kinds built on demand compare field by
 * field. A generic instance's `mut` is part of its identity: a writable view is not the
 * read-only view. "May it be used as that" is a different question, answered by
 * `ttViewDowngradable`.
 *
 * @param a  One type; NULL is equal to no type.
 * @param b  The other type.
 * @return   `true` when the two describe the same type.
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

    if (a->kind == TY_ARRAY) {
        if (a->asize != b->asize) return false;
        if (a->asize < 0) {
            if (!a->param || !b->param || strcmp(a->param, b->param) != 0) return false;
        }
        return ttEquals(a->inner, b->inner);
    }

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
 * @brief Tell whether `t` is the builtin type `builtinName`, ignoring references.
 *
 * @param t            The type to inspect.
 * @param builtinName  Builtin type name.
 * @return             `true` when it is that builtin type after stripping references.
 */
bool ttIs(Type *t, const char *builtinName) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN && strcmp(b->name, builtinName) == 0;
}

/**
 * @brief Tell whether `t` is the error type produced by a failed parse.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is the error type.
 */
bool ttIsError(Type *t) {
    return t && t->kind == TY_ERROR;
}

/**
 * @brief Strip all references: `ref ref T` -> `T`.
 *
 * @param t  The type to strip.
 * @return   The innermost non-reference type; NULL when `t` is NULL.
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
 * @brief Look up a type's integer descriptor, stripping references first.
 *
 * @param t  The type to classify.
 * @return   The builtin integer type's descriptor; NULL in every other case (references are
 *           stripped first, so `ref i32` classifies as `i32`).
 */
static const IntInfo *intInfo(Type *t) {
    Type *b = ttBase(t);
    if (!b || b->kind != TY_BUILTIN) return NULL;
    for (size_t i = 0; INTS[i].name; i++)
        if (strcmp(INTS[i].name, b->name) == 0) return &INTS[i];
    return NULL;
}

/**
 * @brief Tell whether `t` is some builtin integer type.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is an integer after stripping references.
 */
bool ttIsInteger(Type *t) { return intInfo(t) != NULL; }

/**
 * @brief Tell whether `t` is `f32` or `f64`.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a float after stripping references.
 */
bool ttIsFloat(Type *t) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN &&
           (strcmp(b->name, "f32") == 0 || strcmp(b->name, "f64") == 0);
}

/**
 * @brief Tell whether `t` is an integer or a float.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a numeric type after stripping references.
 */
bool ttIsNumeric(Type *t) { return ttIsInteger(t) || ttIsFloat(t); }

/**
 * @brief The bit width of an integer type.
 *
 * @param t  The type to inspect.
 * @return   The bit width; 0 when it is not an integer after stripping references.
 */
int ttIntBits(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->bits : 0;
}

/**
 * @brief Tell whether an integer type is signed.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is signed; `false` for a non-integer type.
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
 * @brief Tell whether a value of type `from` may be used where `to` is expected (implicit
 * widening).
 *
 * Only lossless conversions may happen implicitly. References never widen: `ref T` and
 * `mut ref T` are two permissions that only downgrade in one direction, and that is handled at
 * the assignment check. The reference test must come before the integer helpers, because those
 * helpers strip references: without it, `ref i32` to `mut ref i32` would look like "i32 widens to
 * i32" and be accepted.
 *
 * The error type widens to and from everything, so one failed type does not produce cascading
 * diagnostics.
 *
 * @param from  The value's type.
 * @param to    The type it will be converted to.
 * @return      `true` when the conversion is lossless and may therefore happen implicitly.
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
 * @brief Append the user-facing spelling of `t` to `out`, recursing into its parts.
 *
 * A writable view must print its leading `mut`, otherwise the message reads "expects
 * `slice<i32>`, found `slice<i32>`" and tells the user nothing. Declaration names go through
 * `ttDispName`, so a module type prints as `io::reader`, never as the renamed `io$reader`;
 * exposing the internal encoding means naming a word the user never wrote.
 *
 * @param t    The type to render; NULL renders as `void`.
 * @param out  Buffer that receives the text.
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
        case TY_ANY: bufPuts(out, "any"); return;
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
 * @brief Render a type into an arena string for diagnostics.
 *
 * @param tt  Type table, also used as the arena.
 * @param t   The type to render.
 * @return    The rendered result, stored in `tt`'s arena.
 */
static const char *ttShow(TypeTable *tt, Type *t) {
    Buf b;
    bufInit(&b, tt->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/**
 * @brief Tell whether a value of this type is **one** C parameter when it crosses the C
 * boundary.
 *
 * One spelling for two askers: `extern!` signatures and function types (`fn(A) -> R`), the two
 * roads to a C function. Writing the test once per road makes the boundary drift, and the drift
 * looks like this: `fn(slice<u8>) -> void` declares one argument while the C function takes two,
 * so every call site compiles and calls it wrong.
 *
 * **A pointer to anything** is one parameter: the platform ABI passes all object pointers the
 * same way, so what it points to only affects how readable the declaration is, not whether the
 * call is right -- `ref point`, `ref Pair<i64, u8>`, and `ref void` are one machine word each. A
 * by-value aggregate passes only when marked `@frozen`: that is the author's promise that this
 * struct's bytes are laid out the way C lays out this field list, not something the compiler
 * guarantees on its own. `slice<T>` is two C parameters (data + len, with the caller writing
 * `s.data` and `s.len` explicitly); `void` by value is not a value at all, and is a return type
 * only.
 *
 * @param t         The type of the argument, or of the return value.
 * @param isReturn  True for a return type, where `void` is legal.
 * @return          `true` when it is one C parameter: a scalar, or **a pointer to anything**
 *                  (`ref T` / `mut ref T` / `?ref T` / `ref void`), or a function type.
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
 * @brief Tell whether `t` is a view type.
 *
 * A view is recognised by shape: its name is `slice` and it carries exactly one type argument --
 * the same approach as the compiler recognising the `data` + `len` shape (where `s[i]` is
 * defined).
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a view.
 */
bool ttIsViewType(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef && t->targs.len == 1 &&
           strcmp(t->sdef->name, "slice") == 0;
}

/**
 * @brief Tell whether a value of type `got` may be used where `want` is expected, allowing `mut`
 * to be dropped at any depth.
 *
 * `mut` can only be dropped, never added: it is a **permission** and not layout --
 * `slice<mut slice<i32>>` and `slice<slice<i32>>` are the same C struct (both called
 * `slice_slice_i32`), and losing one permission changes no byte; the other direction is **asking
 * for** a permission, so the source has to write the `mut` itself.
 *
 * This is not `ttEquals`: equality asks "is it the same type" (there `mut` is part of identity),
 * this asks "may it be used as that" (`mut` is only a permission).
 *
 * @param want  The type the context expects.
 * @param got   The value's type.
 * @return      `true` when `got` may be used as `want`.
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
