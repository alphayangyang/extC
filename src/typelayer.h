/* The type layer: the read-only questions about a type, shared by the checker and by code
 * generation.
 *
 * Why this file exists (`[layering]` R3): `codegen.c` used to include `check_internal.h` --
 * the checker's private header -- to reach eleven predicates. That edge is the wrong shape:
 * code generation may depend on **what a type is**, never on the checker's working state.
 * A header of pure questions makes the dependency explicit and keeps it one-way, and the
 * checker keeps its own header for its own state.
 *
 * The rule for what may live here: a function takes types and nodes, answers a question,
 * and changes nothing -- no `Checker`, no side table, no memo cache. Anything that needs
 * the checker's state belongs in `check_internal.h`, and code generation must not want it.
 */
#ifndef EXTC_TYPELAYER_H
#define EXTC_TYPELAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ast.h"
#include "types.h"

/* ---- operators ---------------------------------------------------------------------- */
bool isCmpOp(const char *op);            /* `==` `!=` `<` `<=` `>` `>=` */
bool isLogicOp(const char *op);          /* `&&` `||` */
bool isEqualityOp(const char *op);       /* `==` `!=` (one rule, one `!=` -> `==` fallback) */
bool isOverloadableOp(const char *name); /* can a user function declare this operator name? */

/* Does this type mention a type parameter (`T`), directly or nested? The checker defers work
 * on such a type until instantiation. */
bool mentionsParam(Type *t);

/* Does `t` answer `op` against `rhs` natively, or through a method? Read-only: it walks the
 * type and, for a method, the type's method list. */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs);

/* ---- operators, continued ----------------------------------------------------------- */
bool isArithOp(const char *op);          /* `+` `-` `*` `/` `%` `<<` `>>` `&` `|` `^` */
bool cmpIsNative(Type *t);               /* does C compare this type with `==` directly? */
/* The operator method, with `fallback` tried when `sym` is absent (the `!=` -> `==` rule). */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);

/* ---- types and their members -------------------------------------------------------- */
/* Is `t` the instantiation of the prelude type `name` with exactly `nargs` arguments?
 * `isProtoType(x, "coroutine", 1)` -- the shape test the whole compiler agrees on. */
bool isProtoType(Type *t, const char *name, size_t nargs);

/* The method `name` of a type, or NULL. `mholder` is consulted first (methods that hang off
 * a type with no body of its own: a builtin scalar, or a generic instance). */
StructDef *structOf(Type *t);            /* the struct behind `TY_STRUCT` / `TY_GENERIC`, or NULL */
FuncDef *findMethod(Type *st, const char *name);

/* The operator method of `st` whose right operand is `rhs` (NULL accepts the first one with
 * that name). Substitutes a generic type's parameters before comparing, so `slice<T>::==` is
 * found for `slice<u8>`. */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs);

/* Does this enum have a variant with a payload? */
bool enumHasPayload(TypeDef *td);

/* ---- the pool questions -------------------------------------------------------------
 *
 * "Can this statement reach `extc_pool_new`?" `descendBlocks` is the difference between the
 * two askers: the checker's closure walks into nested blocks (each has a zone of its own),
 * codegen asks about one block's direct statements. Defined in `check_top.c` next to the
 * resolver hook it depends on -- see the note there for why it is not a pure function. */
bool stmtMakesPool(Stmt *s, bool descendBlocks);

/* ---- pool names (one list, one spelling: review F11) -------------------------------- */
static inline bool isPoolCtorName(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_at") == 0 ||
                 strcmp(n, "extc_pool_new_table") == 0);
}
static inline bool poolCtorNeedsZone(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_table") == 0);
}
/* The six pool primitives: `check_expr.c` validates their arity, `codegen.c` lowers them by
 * hand. The two files used to write the six names out separately, so a name added on one
 * side left the other reading arguments it never checked. */
static inline bool isPoolPrimitiveName(const char *n) {
    return n && (strcmp(n, "poolSlice") == 0 || strcmp(n, "poolSliceRaw") == 0 ||
                 strcmp(n, "poolResize") == 0 || strcmp(n, "poolResizeRaw") == 0 ||
                 strcmp(n, "poolGive") == 0 || strcmp(n, "copyInto") == 0);
}

#endif
