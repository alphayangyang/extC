/**
 * @file types.h
 * @brief The type table: interning, equality, rendering, and the builtin types.
 *
 * Types are **interned**: one type has exactly one instance, so for every type other than
 * references, type parameters, and generic instances, "same type" is "same pointer" and type
 * checking is therefore mostly pointer comparison.
 *
 * Each layer owns one concern:
 *   ast.h        describes the shape of the data only
 *   types.h      the type table: interning, equality, rendering, builtin types
 *   check.h      type inference and checking (the only place that infers)
 *   codegen.h    only reads what the checker wrote back onto the AST, never infers
 */

#ifndef EXTC_TYPES_H
#define EXTC_TYPES_H

#include "ast.h"
#include "base.h"

typedef struct {
    Arena *arena;       /* arena that owns this table and every type in it */
    Vec    builtins;    /* Type* - the built-in types; interned */
    Vec    structs;     /* StructDef* - every registered struct */
    Vec    enums;       /* TypeDef* - every registered enum declaration */
    /* One entry per module declaration whose name had to be prefixed to stay unique:
     * the source name and the name it was given (`pair` -> `liba$pair`). The type
     * table consults them when a bare name has to be resolved, and two modules that
     * export the same name produce two entries, which is what lets the resolver count
     * the matches and refuse the ambiguity instead of picking one. */
    Vec    aliases;     /* Alias* */
    Vec    instances;   /* Type* - generic instances; interned */
    /* Instances of generic *enums* (`option<i64>`), kept in a table of their own:
     * their owner is `edef` (a TypeDef) and `sdef` is NULL, so mixing them into
     * `instances` breaks every loop that walks that table by `sdef`. */
    Vec    enumInstances; /* Type* */
    /* Shadow instances for writable views (`mut slice<T>`).
     * A shadow shares the C struct name of the read-only instance, so it must NOT
     * enter `instances`: it would make codegen emit two identical C structs. That
     * is what keeps one view type with one method set (Rust instead mints two
     * distinct slice types). */
    Vec    viewShadows; /* Type* */
    /* Function types (`fn(A) -> R`), interned like generic instances: a function type is a
     * constructor, so `fn(i64) -> i64` written twice is the same type. Codegen emits one C
     * `typedef` per entry here -- and this table **is** the demand set, which is why there is no
     * separate "which function types does the program use" scan: every function type the program
     * mentions was resolved through `ttFn` at some point, whether it was written in a struct
     * field, a parameter, a local, or produced by substituting a generic. */
    Vec    fnTypes;     /* Type* - function types; interned */
    Type  *tVoid;       /* the interned `void` type, which is the only one */
    Type  *tAny;        /* the interned `any` type, which is the only one: `any` has no
                         * parameters and no declaration site, so one pointer compares equal
                         * to every `any` written anywhere in the program */
    Type  *tError;      /* the error type; every failed resolution returns it, and
                         * widening to or from it succeeds, so one bad type does not
                         * produce a cascade of follow-up errors */
} TypeTable;

/**
 * @brief Build a type table and intern the builtin types, `void`, and the error type into
 * it.
 *
 * @param a  Arena: owns this table and every type in it.
 * @param m  Module whose declarations are registered immediately; NULL registers nothing.
 * @return   The new table, ready to hand to `ttRegister` or `ttResolve`.
 */
TypeTable *ttNew(Arena *a, Module *m);

/**
 * @brief Register one module's struct and enum declarations; names that already exist are
 * skipped.
 *
 * The prelude and the user files share one table: types are interned and compared by
 * pointer, and two tables would give the same `bool` two pointers, so every check would
 * report "bool is not bool". Repeated calls are expected and have no side effects.
 *
 * @param tt  The type table to register into.
 * @param m   Module to read; NULL is ignored.
 */
void ttRegister(TypeTable *tt, Module *m);

/**
 * @brief Tell whether `name` is a builtin type name.
 *
 * @param name  Type name.
 * @return      `true` when it is a builtin type name.
 */
bool  ttIsBuiltinName(const char *name);

/**
 * @brief Look up a registered type by name: builtins first, then structs, then enums.
 *
 * Only a **bare name** is looked up: when two modules export the same name both matches are
 * returned, and a caller that may not pick one has to count the matches itself (see
 * `ttResolve`).
 *
 * @param tt    Type table.
 * @param name  The name as written in the source, not the module-renamed name.
 * @return      That type; `NULL` when it was never registered. A struct's / enum's type node
 *              is built on first use and cached on its declaration.
 */
Type *ttFromName(TypeTable *tt, const char *name);

/**
 * @brief Get the one `void` type.
 *
 * @param tt  Type table.
 * @return    The table's `void` type.
 */
Type *ttVoid(TypeTable *tt);

/**
 * @brief Get the one `any` type.
 *
 * @param tt  Type table.
 * @return    The table's `any` type; every `any` in the program is this pointer.
 */
Type *ttAny(TypeTable *tt);

/**
 * @brief Get the error type: the result of a failed parse; it suppresses cascading
 * diagnostics.
 *
 * @param tt  Type table.
 * @return    The error type.
 */
Type *ttError(TypeTable *tt);

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
Type *ttRef(TypeTable *tt, Type *inner);

/**
 * @brief Resolve a type the parser built into an interned type.
 *
 * A `TY_UNRESOLVED` name is first compared against the type parameters in scope, then
 * against the registered declarations; `TY_REF` keeps its own flags; a generic name with type
 * arguments becomes the corresponding instance.
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
Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params);

/**
 * @brief Build (or reuse) a generic struct instance `sd<args...>`.
 *
 * Only a **fully concrete** instance is interned, and only such an instance needs C emitted:
 * the `Pair<A, B>` built while checking a template holds type parameters, that instance only
 * serves type comparison, and interning it would make codegen emit C for `Box_T_set`.
 *
 * @param tt    Type table.
 * @param sd    Generic struct declaration.
 * @param args  Resolved type arguments, one per declared parameter.
 * @return      The interned instance: the same declaration plus equal arguments always yields
 *              the same pointer.
 */
Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args);

/**
 * @brief Build (or reuse) a generic enum instance `td<args...>` (`option<i64>`).
 *
 * Same shape as `ttGeneric`, with two differences:
 *   1. The owner is a `TypeDef` (`edef`) rather than a `StructDef`, so instances are interned
 *      in their own `enumInstances` table.
 *   2. Payload types are **not** substituted here. Variant payloads are written on the
 *      `TypeDef` (`some(T)`), and at each use site `t->targs` is substituted against
 *      `edef->typeParams`, so one `TypeDef` serves every instance and an instance is only
 *      "a name plus arguments". Codegen turns the instance into a substitution context, and
 *      the payload's C type falls out by itself.
 *
 * @param tt    Type table.
 * @param td    Generic enum declaration.
 * @param args  Resolved type arguments, one per declared parameter.
 * @return      The interned instance; when the arguments mention type parameters, an instance
 *              used only inside templates (that one is not interned).
 */
Type *ttEnumGeneric(TypeTable *tt, TypeDef *td, Vec *args);

/**
 * @brief Build (or reuse) a function type `fn(params...) -> ret`.
 *
 * It is interned only when every part is concrete, by the same rule as `ttGeneric`: checking a
 * template builds `fn(T) -> T`, which only serves type comparison and must not be emitted as
 * C.
 *
 * A function type's C spelling is a **typedef name**, not a declarator: this compiler writes
 * types as declaration prefixes, while C function pointers wrap the name inside
 * (`int64_t (*)(int64_t)` is a type, `int64_t (*f)(int64_t)` is not "a type plus a name").
 * The name comes from `ttMangle` and codegen emits the typedef.
 *
 * @param tt      Type table.
 * @param params  Resolved parameter types, in order (`Type *` elements); the vector is copied.
 * @param ret     Resolved return type; NULL reads as `void`.
 * @return        The interned function type: the same signature always yields the same
 *                pointer.
 */
Type *ttFn(TypeTable *tt, Vec *params, Type *ret);

/**
 * @brief Tell whether a value of type `got` may be used where `want` is expected, allowing
 * `mut` to be dropped at any depth.
 *
 * `mut` can only be dropped, never added: it is a **permission** and not layout --
 * `slice<mut slice<i32>>` and `slice<slice<i32>>` are the same C struct (both called
 * `slice_slice_i32`), and losing one permission changes no byte; the other direction is
 * **asking for** a permission, so the source has to write the `mut` itself.
 *
 * This is not `ttEquals`: equality asks "is it the same type" (there `mut` is part of
 * identity), this asks "may it be used as that" (`mut` is only a permission).
 *
 * @param want  The type the context expects.
 * @param got   The value's type.
 * @return      `true` when `got` may be used as `want`.
 */
bool  ttViewDowngradable(Type *want, Type *got);
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
Type *ttViewMut(TypeTable *tt, Type *base, bool mut);
/**
 * @brief Drop `mut` from a view type: writable view -> read-only view.
 *
 * The recursion is the point: the element type loses `mut` as well, so
 * `mut slice<mut slice<T>>` can be used wherever a read-only `slice<slice<T>>` is expected.
 * The recursion is safe because `mut` is a permission and not layout: `slice<mut slice<i32>>`
 * and `slice<slice<i32>>` are the same C struct (both called `slice_slice_i32`, `mut` never
 * appears in a C type), and moving bytes between them makes no difference.
 *
 * The other direction (read-only -> writable) is never allowed: no code can conjure a
 * permission out of nothing.
 *
 * @param tt  Type table.
 * @param t   The type to strip.
 * @return    The read-only form of `t`; `t` itself when nothing changed.
 */
Type *ttViewReadonly(TypeTable *tt, Type *t);

/**
 * @brief Build (or reuse) a fixed-length array type `[n]elem`.
 *
 * Array types live in the instance table together with generic instances: both need a C
 * struct emitted, and codegen treats them the same way.
 *
 * @param tt    Type table.
 * @param n     Number of elements, a compile-time constant.
 * @param elem  Element type.
 * @return      The interned array type: equal length and element type yield the same pointer.
 */
Type *ttArray(TypeTable *tt, int64_t n, Type *elem);

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
bool ttHasParam(Type *t);

/**
 * @brief Tell whether a value of this type is **one** C parameter when it crosses the C
 * boundary.
 *
 * One spelling for two askers: `extern!` signatures and function types (`fn(A) -> R`), the two
 * roads to a C function. `slice<T>` is two C parameters and so never "one"; a struct or an
 * enum with a payload has, by value, a layout this language never promised to match, and does
 * not pass either.
 *
 * @param t         The type of the argument, or of the return value.
 * @param isReturn  True for a return type, where `void` is legal.
 * @return          `true` when it is one C parameter: a scalar, or **a pointer to anything**
 *                  (`ref T` / `mut ref T` / `?ref T` / `ref void`), or a function type.
 */
bool ttCrossesC(Type *t, bool isReturn);

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
 *                no arguments, or `params` does not contain the name) `t` is returned
 *                unchanged.
 */
Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args);

/**
 * @brief Render a type recursively into a C identifier.
 *
 * @param tt  Type table, also used as the arena for the generated string.
 * @param t   The type to render; NULL renders as `void`.
 * @return    The C identifier: `Pair<i32, u8>` becomes `Pair_i32_u8`; references, arrays, and
 *            function types each get their own prefix, so different types cannot collide.
 */
const char *ttMangle(TypeTable *tt, Type *t);
/**
 * @brief Name a type the way the user writes it (`io::reader`), not by its renamed C name
 * (`io$reader`).
 *
 * @param srcName  The source qualified name the module declaration recorded; NULL or empty for
 *                 a root file declaration.
 * @param name     The name to show when no source name was recorded.
 * @return         `srcName` when set, else `name`, else `"?"` -- never NULL.
 */
const char *ttDispName(const char *srcName, const char *name);

/**
 * @brief Tell whether `t` is the type parameter named `name`.
 *
 * @param t     The type to inspect.
 * @param name  Type parameter name.
 * @return      `true` when it is.
 */
bool  ttIsParam(Type *t, const char *name);

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
bool  ttEquals(Type *a, Type *b);

/**
 * @brief Tell whether `t` is the builtin type `builtinName`, ignoring references.
 *
 * @param t            The type to inspect.
 * @param builtinName  Builtin type name.
 * @return             `true` when it is that builtin type after stripping references.
 */
bool  ttIs(Type *t, const char *builtinName);

/**
 * @brief Tell whether `t` is some builtin integer type.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is an integer after stripping references.
 */
bool  ttIsInteger(Type *t);

/**
 * @brief Tell whether `t` is `f32` or `f64`.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a float after stripping references.
 */
bool  ttIsFloat(Type *t);

/**
 * @brief Tell whether `t` is an integer or a float.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a numeric type after stripping references.
 */
bool  ttIsNumeric(Type *t);

/**
 * @brief The bit width of an integer type.
 *
 * @param t  The type to inspect.
 * @return   The bit width; 0 when it is not an integer after stripping references.
 */
int   ttIntBits(Type *t);

/**
 * @brief Tell whether an integer type is signed.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is signed; `false` for a non-integer type.
 */
bool  ttIntSigned(Type *t);

/**
 * @brief Tell whether `t` is the error type produced by a failed parse.
 *
 * @param t  The type to inspect.
 * @return   `true` when it is the error type.
 */
bool  ttIsError(Type *t);

/**
 * @brief Strip all references: `ref ref T` -> `T`.
 *
 * @param t  The type to strip.
 * @return   The innermost non-reference type; NULL when `t` is NULL.
 */
Type *ttBase(Type *t);

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
void  ttRender(Type *t, Buf *out);

/**
 * @brief Tell whether a value of type `from` may be used where `to` is expected (implicit
 * widening).
 *
 * Only lossless conversions may happen implicitly. References never widen: `ref T` and
 * `mut ref T` are two permissions that only downgrade in one direction, and that is handled at
 * the assignment check. The reference test must come before the integer helpers, because those
 * helpers strip references: without it, `ref i32` to `mut ref i32` would look like "i32 widens
 * to i32" and be accepted.
 *
 * The error type widens to and from everything, so one failed type does not produce cascading
 * diagnostics.
 *
 * @param from  The value's type.
 * @param to    The type it will be converted to.
 * @return      `true` when the conversion is lossless and may therefore happen implicitly.
 */
bool  ttCanWiden(Type *from, Type *to);

/**
 * @brief Tell whether `t` is a view type.
 *
 * A view is recognised by shape: its name is `slice` and it carries exactly one type argument
 * -- the same approach as the compiler recognising the `data` + `len` shape (where `s[i]` is
 * defined).
 *
 * @param t  The type to inspect.
 * @return   `true` when it is a view.
 */
bool ttIsViewType(Type *t);

#endif /* EXTC_TYPES_H */
