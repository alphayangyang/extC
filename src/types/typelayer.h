/**
 * @file typelayer.h
 * @brief The type layer: **read-only questions** about a type, shared by the checker and
 * code generation.
 *
 * Only functions that "take a type and a node, answer one question, and change nothing" belong
 * here: no `Checker`, no side tables, no memo cache. Anything that needs checker state belongs
 * in the checker's own private header, and codegen should not want it.
 *
 * This boundary keeps dependencies one-way: codegen may depend on **what a type is**, never on
 * the checker's working state.
 */
#ifndef EXTC_TYPELAYER_H
#define EXTC_TYPELAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ast.h"
#include "types.h"

/* ---- Operators ----------------------------------------------------------------------- */

/**
 * @brief Tell whether `op` is a comparison operator: `==` `!=` `<` `<=` `>` `>=`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isCmpOp(const char *op);

/**
 * @brief Tell whether `op` is a logical operator: `&&` `||`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isLogicOp(const char *op);

/**
 * @brief Tell whether `op` is an equality operator: `==` `!=`.
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isEqualityOp(const char *op);

/**
 * @brief Tell whether a user function may declare this operator name (a name in the
 * overloadable set).
 *
 * @param name  Operator name.
 * @return      `true` when it may.
 */
bool isOverloadableOp(const char *name);

/**
 * @brief Tell whether a type parameter (`T`) occurs in this type, directly or nested.
 *
 * The checker defers work on such types until instantiation.
 *
 * @param t  Type.
 * @return   `true` when it mentions a type parameter.
 */
bool mentionsParam(Type *t);

/**
 * @brief Tell whether `t` can answer `op` with `rhs`, either natively or through a method.
 *
 * Read-only: it walks the type and, on hitting a method, walks that type's method table.
 *
 * @param tt   Type table.
 * @param t    Left operand type.
 * @param op   Operator spelling.
 * @param rhs  Right operand type.
 * @return     `true` when supported.
 */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs);

/* ---- Operators (continued) ----------------------------------------------------------- */

/**
 * @brief Tell whether `op` is one of the five arithmetic operators: `+` `-` `*` `/` `%`.
 *
 * `<<` / `>>` are **not** here -- the caller tests them separately as stream operators (the
 * two `strcmp` calls in `isOverloadableOp` do exactly that).
 *
 * @param op  Operator spelling.
 * @return    `true` when it is.
 */
bool isArithOp(const char *op);

/**
 * @brief Can C compare this type with a plain `==`?
 *
 * @param t  Type.
 * @return   `true` when C can compare it natively.
 */
bool cmpIsNative(Type *t);

/**
 * @brief Find a type's operator method.
 *
 * When `sym` does not exist, `fallback` is used instead (the rule that `!=` falls back to `==`).
 *
 * @param tt        Type table.
 * @param b         The type that carries the method (`mholder`).
 * @param sym       Operator name to find.
 * @param rhs       Right operand type, which picks among same-named methods.
 * @param fallback  Fallback operator name; may be `NULL`.
 * @return          The method found; `NULL` when there is none.
 */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);

/* ---- A type and its members ----------------------------------------------------------- */

/**
 * @brief Is `t` an instance of the prelude type `name` with exactly `nargs` arguments?
 *
 * For example `isProtoType(x, "coroutine", 1)`. This is the shape test the whole compiler
 * agrees on.
 *
 * @param t      Type.
 * @param name   Type name.
 * @param nargs  Number of type arguments.
 * @return       `true` when the shape matches.
 */
bool isProtoType(Type *t, const char *name, size_t nargs);

/**
 * @brief The struct definition behind `TY_STRUCT` / `TY_GENERIC`.
 *
 * @param t  Type.
 * @return   The struct definition; `NULL` when it is not one.
 */
StructDef *structOf(Type *t);

/**
 * @brief The method named `name` on a type.
 *
 * @param st    The type that carries the method.
 * @param name  Method name.
 * @return      The method; `NULL` when there is none.
 */
FuncDef *findMethod(Type *st, const char *name);

/**
 * @brief The operator method on `st` whose right operand is `rhs`.
 *
 * Passing `NULL` for `rhs` accepts the first one with the same name. Arguments of a generic
 * type are substituted before comparison, so `slice<u8>` finds `slice<T>::==`.
 *
 * @param tt    Type table.
 * @param st    Left operand type.
 * @param name  Operator name.
 * @param rhs   Right operand type; may be `NULL`.
 * @return      The method; `NULL` when there is none.
 */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs);

/**
 * @brief Tell whether this enum has a variant with a payload.
 *
 * @param td  Enum definition.
 * @return    `true` when it does.
 */
bool enumHasPayload(TypeDef *td);

/**
 * @brief Take the last segment of a declaration name (module renaming adds a `$` prefix).
 *
 * `$` cannot occur in an extC identifier, so it can only come from the loader's module
 * renaming.
 *
 * @param name  Declaration name; may be `NULL`.
 * @return      The last segment; when `name` is `NULL`, `name` itself.
 */
static inline const char *plateDeclTail(const char *name) {
    const char *d = name ? strrchr(name, '$') : NULL;
    return d ? d + 1 : name;
}

/**
 * @brief Is this declaration the slab-layer primitive `extc_viewOf`?
 *
 * @param name  Declaration name.
 * @return      `true` when it is.
 */
static inline bool plateIsViewOfName(const char *name) {
    return name && strcmp(plateDeclTail(name), "extc_viewOf") == 0;
}

/**
 * @brief Is this declaration `extc_memCopy`?
 *
 * @param name  Declaration name.
 * @return      `true` when it is.
 */
static inline bool plateIsMemCopyName(const char *name) {
    return name && strcmp(plateDeclTail(name), "extc_memCopy") == 0;
}

/* ---- Pool questions ------------------------------------------------------------------- */

/**
 * @brief Can this statement reach `extc_pool_new`?
 *
 * `descendBlocks` is where the two askers part ways: the checker wants to walk into nested
 * blocks (each block has its own zone), while codegen asks only about the direct statements of
 * one block.
 *
 * @param s              Statement.
 * @param descendBlocks  Whether to walk into nested blocks.
 * @return               `true` when it can reach it.
 */
bool stmtMakesPool(Stmt *s, bool descendBlocks);

/**
 * @brief May this callee create a pool at run time?
 *
 * The read-only port of the checker's own answer, for the pass that decides which places need a
 * zone: a call whose pool is born in **this** place has to be wrapped in a zone enter/leave pair,
 * while one that names an outer place does not.
 *
 * @param f  Callee definition; NULL answers false.
 * @return   `true` when the callee can reach `extc_pool_new`.
 */
bool calleeCreatesPool(FuncDef *f);

/* ---- Pool names (one list, one spelling) ---------------------------------------------- */

/**
 * @brief Is this a pool constructor name (`extc_pool_new` / `extc_pool_new_at` /
 * `extc_pool_new_table`)?
 *
 * @param n  Declaration name.
 * @return   `true` when it is.
 */
static inline bool isPoolCtorName(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_at") == 0 ||
                 strcmp(n, "extc_pool_new_table") == 0);
}

/**
 * @brief Does this pool constructor take an explicit zone (`extc_pool_new` /
 * `extc_pool_new_table` do)?
 *
 * @param n  Declaration name.
 * @return   `true` when it does.
 */
static inline bool poolCtorNeedsZone(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_table") == 0);
}

/**
 * @brief Is this one of the six pool primitives (`poolSlice` / `poolSliceRaw` / `poolResize` /
 * `poolResizeRaw` / `poolGive` / `copyInto`)?
 *
 * The checker validates their argument counts, and codegen lowers them to C by hand. The names
 * are written only here, so the two sides' lists cannot drift apart.
 *
 * @param n  Declaration name.
 * @return   `true` when it is.
 */
static inline bool isPoolPrimitiveName(const char *n) {
    return n && (strcmp(n, "poolSlice") == 0 || strcmp(n, "poolSliceRaw") == 0 ||
                 strcmp(n, "poolResize") == 0 || strcmp(n, "poolResizeRaw") == 0 ||
                 strcmp(n, "poolGive") == 0 || strcmp(n, "copyInto") == 0);
}

/**
 * @brief Is this one of the two buffer primitives (`extc_sizeOf` / `extc_viewOfT`)?
 *
 * Both exist for the same reason `alloc` is a primitive: the generator has to name the
 * element type in C, and the standard library cannot (`extc_sizeOf<T>()` has no `sizeof`
 * spelling in extC, and `extc_viewOfT<T>()` would have to return `slice<T>`, which
 * `extern!` refuses -- two C parameters, not one machine word). The names live only here,
 * so the checker and codegen cannot drift apart.
 *
 * @param n  Declaration name.
 * @return   `true` when it is one of them.
 */
static inline bool isBufPrimitiveName(const char *n) {
    return n && (strcmp(n, "extc_sizeOf") == 0 || strcmp(n, "extc_viewOfT") == 0);
}

#endif
