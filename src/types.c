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

/* Report whether `name` is a built-in type name. */
bool ttIsBuiltinName(const char *name) {
    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        if (strcmp(BUILTIN_NAMES[i], name) == 0) return true;
    return false;
}

/* Allocate a zeroed type node of `kind` carrying `name`. */
static Type *mkType(Arena *a, TypeKind kind, const char *name) {
    Type *t = (Type *)arenaAllocZero(a, sizeof(Type));
    t->kind = kind;
    t->name = name;
    return t;
}

/* Create the type table and intern the built-in types, `void`, and the error type.
 *
 * Params:
 *   a - arena that owns the table and every type in it
 *   m - module whose declarations are registered immediately; NULL registers none
 *
 * Returns:
 *   The new table, ready for `ttRegister` or `ttResolve`.
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

    for (size_t i = 0; BUILTIN_NAMES[i]; i++)
        *(Type **)vecPush(&tt->builtins) = mkType(a, TY_BUILTIN, BUILTIN_NAMES[i]);

    tt->tVoid  = mkType(a, TY_VOID, "void");
    tt->tError = mkType(a, TY_ERROR, "<error>");

    if (m) ttRegister(tt, m);
    return tt;
}

/* Register a module's struct and type declarations, skipping names already present.
 *
 * Params:
 *   tt - type table to register into
 *   m  - module to read; NULL is ignored
 *
 * Notes:
 *   - The prelude and the user files share one table. Types are interned and equality
 *     is pointer comparison, so two tables would give the same `bool` two different
 *     pointers and every check would report "bool is not bool".
 *   - Repeated calls are expected and harmless by design.
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

/* Return the single `void` type. */
Type *ttVoid(TypeTable *tt)  { return tt->tVoid; }

/* Return the error type used to suppress cascading diagnostics after a bad type. */
Type *ttError(TypeTable *tt) { return tt->tError; }

/* Create (or reuse) the fixed-array type `[n]elem`.
 *
 * Params:
 *   tt   - type table
 *   n    - element count, a compile-time constant
 *   elem - element type
 *
 * Returns:
 *   The interned array type, so equal length and element type give one pointer.
 *
 * Notes:
 *   - Array types are stored in the instance table next to generic instances: both
 *     need a C struct emitted, and codegen treats them alike.
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

/* Create a `ref T` type.
 *
 * Params:
 *   tt    - type table that owns the new type
 *   inner - the referenced type
 *
 * Returns:
 *   A fresh, read-only, non-nullable reference.
 *
 * Notes:
 *   - References are deliberately not interned: they are rare and the caller sets
 *     `mut` / `nullable` afterwards, so `ttEquals` compares them structurally.
 */

Type *ttRef(TypeTable *tt, Type *inner) {
    Type *t = (Type *)arenaAllocZero(tt->arena, sizeof(Type));
    t->kind = TY_REF;
    t->inner = inner;
    return t;
}

/* Build a reference that copies the permissions of `src`.
 *
 * Params:
 *   tt    - type table
 *   src   - reference type whose flags are copied
 *   inner - the type the new reference points at
 *
 * Returns:
 *   A reference with `src`'s `mut` and `nullable` flags.
 *
 * Notes:
 *   - Both flags are part of the type, so resolving or substituting a reference must
 *     not lose them.
 */

static Type *refLike(TypeTable *tt, Type *src, Type *inner) {
    Type *t = ttRef(tt, inner);
    t->mut = src->mut;
    t->nullable = src->nullable;   /* nullability is part of the type too */
    return t;
}

/* Look up a registered type by name: built-in, then struct, then enum.
 *
 * Params:
 *   tt   - type table
 *   name - the source name, not a mangled module name
 *
 * Returns:
 *   The type, or NULL when nothing of that name is registered. A struct or enum type
 *   node is created on first use and cached on its declaration.
 *
 * Notes:
 *   - A bare-name lookup only. When two modules export the same name both match, and
 *     a caller that must not pick one silently has to count the matches itself
 *     (see `ttResolve`).
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

/* Resolve a parser-built type into an interned type.
 *
 * A `TY_UNRESOLVED` name is matched against the type parameters in scope, then
 * against the registered declarations; `TY_REF` keeps its flags; a generic name with
 * type arguments becomes the corresponding instance.
 *
 * Params:
 *   tt     - type table
 *   ctx    - context that receives diagnostics
 *   t      - the type node built by the parser
 *   line   - source line blamed by a diagnostic
 *   params - names of the type parameters in scope; NULL or empty means this is not
 *            inside a generic declaration
 *
 * Returns:
 *   The interned type. On failure it reports the error and returns the error type so
 *   the caller keeps checking without a cascade of follow-up errors.
 *
 * Notes:
 *   - A bare name exported by two imported modules is an error, never a pick: all
 *     matches are counted and the diagnostic asks for `module::Name`. Choosing the
 *     first match used to bind the wrong type silently - the program compiled and had
 *     the wrong type, which is worse than a rejection.
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
                /* Ambiguity must be an error, never a pick. When two modules export
                 * `pair`, a bare `var p: pair = alpha::make(5)` that "takes the first
                 * match" silently binds the type of whichever module registered
                 * first: the program compiles and the type is wrong, which is the
                 * worst outcome. So count the matches: none means unknown type, two
                 * or more means ambiguous and the user must write `module::Name`. */
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

            /* A generic struct or enum written without type arguments. */
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

/* ------------------------------------------------------------------- generics */

/* Report whether `t` is a type parameter named `name`. */
bool ttIsParam(Type *t, const char *name) {
    return t && t->kind == TY_PARAM && strcmp(t->param, name) == 0;
}

/* Name a type the way the user wrote it (`io::reader`) rather than by its mangled
 * C name (`io$reader`).
 *
 * Params:
 *   srcName - the source-qualified name recorded for a module declaration; NULL or
 *             empty for a declaration of the root file
 *   name    - the name to show when no source name was recorded
 *
 * Returns:
 *   `srcName` when set, otherwise `name`, otherwise `"?"` - never NULL.
 */

const char *ttDispName(const char *srcName, const char *name) {
    return (srcName && *srcName) ? srcName : (name ? name : "?");
}

/* Render a type as a C identifier, recursively.
 *
 * Params:
 *   tt - type table, used as the arena for the built strings
 *   t  - type to render; NULL renders as `void`
 *
 * Returns:
 *   The C identifier: `Pair<i32, u8>` becomes `Pair_i32_u8`, and references and
 *   arrays are prefixed so that distinct types cannot collide.
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
        default:       return t->name ? t->name : "?";
    }
}

/* Report whether a type mentions a type parameter, at any depth.
 *
 * Params:
 *   t - type to inspect
 *
 * Returns:
 *   True when `t` is a type parameter or contains one inside a reference or a
 *   generic argument.
 *
 * Notes:
 *   - A rule that depends on the parameter cannot be decided inside a generic body;
 *     the check is deferred until the body is instantiated. A missed `true` here
 *     would let a template be checked as if it were concrete.
 */

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

/* Create (or reuse) the generic struct instance `sd<args...>`.
 *
 * Params:
 *   tt   - type table
 *   sd   - the generic struct declaration
 *   args - resolved type arguments, one per declared type parameter
 *
 * Returns:
 *   The interned instance, so the same declaration and equal arguments always give
 *   the same pointer.
 *
 * Notes:
 *   - Only a fully concrete instance is interned and only it needs C emitted.
 *     Checking a template produces `Pair<A, B>`, whose arguments are themselves type
 *     parameters; that instance exists to compare types with, and interning it would
 *     make codegen emit C for `Box_T_set`.
 */

Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args) {
    bool concrete = true;
    for (size_t j = 0; j < args->len; j++) {
        if (ttHasParam(*(Type **)vecAt(args, j))) { concrete = false; break; }
    }

    if (concrete) {
        /* Interning: one instance per (declaration, argument list) pair. */
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

/* Create (or reuse) the generic enum instance `td<args...>` (`option<i64>`).
 *
 * Same shape as `ttGeneric`, with two differences:
 *   1. The owner is a `TypeDef` (`edef`) and not a `StructDef`, so the instance is
 *      interned in the `enumInstances` table of its own.
 *   2. Payload types are *not* substituted here. A variant payload is written on the
 *      `TypeDef` (`some(T)`) and substituted at each use site from `edef->typeParams`
 *      against `t->targs`, so one `TypeDef` serves every instance and an instance is
 *      only "a name plus arguments". Codegen pushes the instance as the substitution
 *      context, which makes the payload C type resolve by itself.
 *
 * Params:
 *   tt   - type table
 *   td   - the generic enum declaration
 *   args - resolved type arguments, one per declared type parameter
 *
 * Returns:
 *   The interned instance, or a template-only instance when an argument mentions a
 *   type parameter (that one is not interned).
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

/* Return the writable shadow of a view instance (`mut slice<T>`).
 *
 * A shadow exists instead of a second instance because a writable view and the
 * read-only view are the *same C struct* - same layout, same name, same method set -
 * and differ only in the permission the checker sees. The shadow therefore shares the
 * C name of `base` and does not enter the instance table, so codegen emits one struct
 * for both. This is how `mut` stays a qualifier rather than a second type.
 *
 * Params:
 *   tt   - type table
 *   base - the read-only view instance
 *   mut  - when false, `base` is returned unchanged
 *
 * Returns:
 *   The shadow instance for `base`, created on first use and reused afterwards.
 *   A type that is not a generic instance (so not a view) is returned unchanged.
 */

Type *ttViewMut(TypeTable *tt, Type *base, bool mut) {
    if (!mut || !base || base->kind != TY_GENERIC) return base;
    if (base->mut) return base;                 /* already writable */
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

/* Drop the `mut` qualifier from a view type: writable view -> read-only view.
 *
 * The recursion is the point: element types lose `mut` too, so
 * `mut slice<mut slice<T>>` is usable anywhere a read-only `slice<slice<T>>` is
 * expected. Recursion is safe because `mut` is a permission and not a layout:
 * `slice<mut slice<i32>>` and `slice<slice<i32>>` are the same C struct (both are
 * named `slice_slice_i32`, `mut` never appears in a C type), so passing one where the
 * other is expected moves no different bytes.
 *
 * The opposite direction (read-only -> writable) is never allowed: no code may invent
 * a permission.
 *
 * Params:
 *   tt - type table
 *   t  - the type to strip
 *
 * Returns:
 *   The read-only form of `t`, or `t` itself when nothing changed.
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
    if (argChanged) return ttGeneric(tt, t->sdef, &args); /* an argument changed: rebuild */
    return t;
}

/* Replace the type parameters of `t` with concrete types.
 *
 * Used for monomorphization: every `params[i]` found inside `t` becomes `args[i]`,
 * recursively through references, arrays, generic arguments, and enum instances.
 * A reference that is rebuilt this way keeps its `mut` and `nullable` flags, because
 * those are part of the type.
 *
 * Params:
 *   tt     - type table
 *   t      - the type to substitute into
 *   params - type parameter names, by index (`const char *` entries)
 *   args   - the concrete type for each name, at the same index (`Type *` entries)
 *
 * Returns:
 *   `t` with the parameters replaced, or `t` unchanged when there is nothing to do:
 *   no parameters, no arguments, or a parameter that `params` does not name.
 */

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
            /* Writability travels with the substitution: `mut` is part of the type. */
            return ttViewMut(tt, ttGeneric(tt, t->sdef, &na), t->mut);
        }
        /* Enum instances need substituting too, and missing this branch was a real
         * bug: once the enum declarations became generic, the enum instance has kind
         * `TY_ENUM` and not `TY_GENERIC`, so it fell into `default` and came back
         * unchanged. A method of a generic instance that returned `option<T>` then
         * produced `option_T` instead of `option_i32` (`v.get(0)` on a
         * `varArray<i32>`), and the user saw "expects option_i32, found option_T".
         * The same gap had already been fixed once in the enum branch of
         * `typeContainsRef`, so: when a type constructor is added, every function
         * that dispatches on the type kind has to be revisited - `ttSubstitute`,
         * `ttEquals`, `ttRender`, and `typeContainsRef` at least. */
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

/* ------------------------------------------------------------------ equality */

/* Report whether two types are the same type.
 *
 * Params:
 *   a - one type; NULL equals nothing
 *   b - the other type
 *
 * Returns:
 *   True when the two describe the same type.
 *
 * Notes:
 *   - Interned kinds are settled by the pointer comparison above, so only the kinds
 *     that are built on demand are compared field by field.
 *   - For a generic instance `mut` is part of the identity: a writable view is not
 *     the read-only view. "May be used as" is a different question, answered by
 *     `ttViewDowngradable`.
 */

bool ttEquals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    /* Every kind except references, type parameters, and generic instances is
     * interned, so differing pointers already mean differing types. */
    if (a->kind == TY_REF)
        return a->mut == b->mut && a->nullable == b->nullable &&
               ttEquals(a->inner, b->inner);

    if (a->kind == TY_ARRAY)
        return a->asize == b->asize && ttEquals(a->inner, b->inner);

    if (a->kind == TY_PARAM)
        return a->tpIndex == b->tpIndex && strcmp(a->param, b->param) == 0;

    if (a->kind == TY_GENERIC) {
        if (a->mut != b->mut) return false;   /* a writable view is not the read-only view */
        if (a->sdef != b->sdef || a->targs.len != b->targs.len) return false;
        for (size_t i = 0; i < a->targs.len; i++)
            if (!ttEquals(*(Type **)vecAt(&a->targs, i), *(Type **)vecAt(&b->targs, i)))
                return false;
        return true;
    }
    return false;
}

/* Report whether `t` is the built-in type `builtinName`, ignoring any references. */
bool ttIs(Type *t, const char *builtinName) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN && strcmp(b->name, builtinName) == 0;
}

/* Report whether `t` is the error type produced by a failed resolution. */
bool ttIsError(Type *t) {
    return t && t->kind == TY_ERROR;
}

/* Strip every reference from a type: `ref ref T` -> `T`. */
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

/* Find the integer descriptor of a type, ignoring references.
 *
 * Params:
 *   t - type to classify
 *
 * Returns:
 *   The descriptor of a built-in integer type, or NULL for everything else (a
 *   reference is stripped first, so `ref i32` classifies as `i32`).
 */

static const IntInfo *intInfo(Type *t) {
    Type *b = ttBase(t);
    if (!b || b->kind != TY_BUILTIN) return NULL;
    for (size_t i = 0; INTS[i].name; i++)
        if (strcmp(INTS[i].name, b->name) == 0) return &INTS[i];
    return NULL;
}

/* Report whether `t` is one of the built-in integer types. */
bool ttIsInteger(Type *t) { return intInfo(t) != NULL; }

/* Report whether `t` is `f32` or `f64`. */
bool ttIsFloat(Type *t) {
    Type *b = ttBase(t);
    return b && b->kind == TY_BUILTIN &&
           (strcmp(b->name, "f32") == 0 || strcmp(b->name, "f64") == 0);
}

/* Report whether `t` is an integer or a float. */
bool ttIsNumeric(Type *t) { return ttIsInteger(t) || ttIsFloat(t); }

/* Width in bits of an integer type; 0 for anything that is not an integer. */
int ttIntBits(Type *t) {
    const IntInfo *i = intInfo(t);
    return i ? i->bits : 0;
}

/* Report whether an integer type is signed; false for a non-integer type. */
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

/* Report whether a value of type `from` may be used where `to` is expected.
 *
 * Params:
 *   from - the type of the value
 *   to   - the type it would be converted to
 *
 * Returns:
 *   True when the conversion is lossless and therefore allowed implicitly.
 *
 * Notes:
 *   - References never widen, and the reference test below has to come before the
 *     integer helpers. `ref T` and `mut ref T` are different permissions that only
 *     downgrade one way, and in `checkAssignable`; `intInfo` strips references, so an
 *     unfiltered `ref i32` to `mut ref i32` looks like "i32 widens to i32" and would
 *     be accepted. This mistake has been made four times in this codebase, always by
 *     a helper that quietly drops references or fails to substitute generic
 *     arguments.
 *   - The error type widens to and from everything, so one failed type does not
 *     produce a cascade of follow-up diagnostics.
 */

bool ttCanWiden(Type *from, Type *to) {
    if (!from || !to) return false;
    if (ttIsError(from) || ttIsError(to)) return true;   /* suppress cascading errors */
    if (ttEquals(from, to)) return true;

    /* References take no part in widening. */
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

/* Append the user-facing spelling of `t` to `out`, recursing into its parts.
 *
 * Params:
 *   t   - type to render; NULL renders as `void`
 *   out - buffer that receives the text
 *
 * Notes:
 *   - A writable view has to print its leading `mut`, otherwise the message reads
 *     "expects `slice<i32>`, found `slice<i32>`" and tells the user nothing.
 *   - Declaration names go through `ttDispName`, so a module type prints as
 *     `io::reader` and never as the mangled `io$reader`; exposing the internal
 *     encoding names a word the user never wrote.
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
            /* Print `mut` for a writable view, or the message becomes
             * "expects `slice<i32>`, found `slice<i32>`". */
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
        default: {
            /* Show the source name first: a module declaration prints as
             * `io::reader`. Printing `io$reader` leaks the internal encoding and
             * names a word the user never wrote. */
            if (t->sdef)      { bufPuts(out, ttDispName(t->sdef->srcName, t->sdef->name)); return; }
            if (t->edef)      { bufPuts(out, ttDispName(t->edef->srcName, t->edef->name)); return; }
            bufPuts(out, t->name ? t->name : "?");
            return;
        }
    }
}

/* Report whether `t` is a view type.
 *
 * A view is recognized by shape - a type named `slice` with exactly one type
 * argument - the same way the compiler recognizes the `data` + `len` shape that
 * `s[i]` is defined in terms of.
 */

bool ttIsViewType(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef && t->targs.len == 1 &&
           strcmp(t->sdef->name, "slice") == 0;
}

/* Report whether a value of type `got` may be used where `want` is expected, by
 * dropping `mut` at any depth. `mut` may only be removed, never added.
 *
 * Removing it is always safe because `mut` is a permission and not a layout:
 * `slice<mut slice<i32>>` and `slice<slice<i32>>` are the same C struct (both are
 * named `slice_slice_i32`), so one permission less changes no byte. The opposite
 * direction is a request *for* a permission, so the source has to write `mut` out.
 *
 * Params:
 *   want - the type the context expects
 *   got  - the type of the value
 *
 * Returns:
 *   True when `got` is usable as `want`.
 *
 * Notes:
 *   - This is not `ttEquals`. Equality asks "is this the same type", where `mut` is
 *     part of the identity; this asks "may it be used as that", where `mut` is only a
 *     permission.
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
