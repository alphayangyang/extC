/* The type table: interning, equality, rendering, and the built-in types.
 *
 * A type is interned, so a type has exactly one instance and "same type" is
 * "same pointer" for everything that is not a reference, a type parameter, or a
 * generic instance. Type checking is therefore mostly pointer comparison.
 *
 * Each layer keeps its own job:
 *   ast.[ch]     describes only the shape of the data
 *   types.[ch]   the type table: interning, equality, rendering, built-ins
 *   check.[ch]   type inference and checking (the only place that infers)
 *   codegen.[ch] only reads what check wrote back into the AST; it never infers
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
    Type  *tVoid;       /* the interned `void` type, which is the only one */
    Type  *tError;      /* the error type; every failed resolution returns it, and
                         * widening to or from it succeeds, so one bad type does not
                         * produce a cascade of follow-up errors */
} TypeTable;

/* Create the type table and intern the built-in types, `void`, and the error type.
 *
 * Params:
 *   a - arena that owns the table and every type in it
 *   m - module whose declarations are registered immediately; NULL registers none
 *
 * Returns:
 *   The new table, ready for `ttRegister` or `ttResolve`.
 */

TypeTable *ttNew(Arena *a, Module *m);

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

void ttRegister(TypeTable *tt, Module *m);

/* Report whether `name` is a built-in type name. */
bool  ttIsBuiltinName(const char *name);

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

Type *ttFromName(TypeTable *tt, const char *name);

/* Return the single `void` type. */
Type *ttVoid(TypeTable *tt);

/* Return the error type used to suppress cascading diagnostics after a bad type. */
Type *ttError(TypeTable *tt);

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

Type *ttRef(TypeTable *tt, Type *inner);

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

Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params);

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

Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args);

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

Type *ttEnumGeneric(TypeTable *tt, TypeDef *td, Vec *args);

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

bool  ttViewDowngradable(Type *want, Type *got);
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

Type *ttViewMut(TypeTable *tt, Type *base, bool mut);
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

Type *ttViewReadonly(TypeTable *tt, Type *t);

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

Type *ttArray(TypeTable *tt, int64_t n, Type *elem);

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

bool ttHasParam(Type *t);

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

Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args);

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

const char *ttMangle(TypeTable *tt, Type *t);
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

const char *ttDispName(const char *srcName, const char *name);

/* Report whether `t` is a type parameter named `name`. */
bool  ttIsParam(Type *t, const char *name);

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

bool  ttEquals(Type *a, Type *b);

/* Report whether `t` is the built-in type `builtinName`, ignoring any references. */
bool  ttIs(Type *t, const char *builtinName);

/* Report whether `t` is one of the built-in integer types. */
bool  ttIsInteger(Type *t);

/* Report whether `t` is `f32` or `f64`. */
bool  ttIsFloat(Type *t);

/* Report whether `t` is an integer or a float. */
bool  ttIsNumeric(Type *t);

/* Width in bits of an integer type; 0 for anything that is not an integer. */
int   ttIntBits(Type *t);

/* Report whether an integer type is signed; false for a non-integer type. */
bool  ttIntSigned(Type *t);

/* Report whether `t` is the error type produced by a failed resolution. */
bool  ttIsError(Type *t);

/* Strip every reference from a type: `ref ref T` -> `T`. */
Type *ttBase(Type *t);

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

void  ttRender(Type *t, Buf *out);

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
 *   - References never widen. `ref T` and `mut ref T` are different permissions that
 *     only downgrade one way, and that is handled where assignments are checked.
 *     The reference test has to come before the integer helpers, because those strip
 *     references: without it, `ref i32` to `mut ref i32` looks like "i32 widens to
 *     i32" and is accepted. This mistake has been made four times in this codebase,
 *     always by a helper that quietly drops references or fails to substitute generic
 *     arguments.
 *   - The error type widens to and from everything, so one failed type does not
 *     produce a cascade of follow-up diagnostics.
 */

bool  ttCanWiden(Type *from, Type *to);

/* Report whether `t` is a view type.
 *
 * A view is recognized by shape - a type named `slice` with exactly one type
 * argument - the same way the compiler recognizes the `data` + `len` shape that
 * `s[i]` is defined in terms of.
 */

bool ttIsViewType(Type *t);

#endif /* EXTC_TYPES_H */
