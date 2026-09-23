/* The type checker: it resolves names, infers types, and enforces the borrow and
 * escape rules. Every result is written back into the tree declared in ast.h, which
 * is what keeps code generation free of type inference.
 */
#ifndef EXTC_CHECK_H
#define EXTC_CHECK_H

#include "ast.h"
#include "base.h"
#include "types.h"

/* Type-check a module and annotate it in place.
 *
 * Params:
 *   ctx   - diagnostics context of the file being checked
 *   arena - arena for everything the pass allocates
 *   tt    - the type table shared by the prelude and every module
 *   m     - the module to check, already loaded and registered in tt
 *
 * Returns:
 *   False when an error was recorded in ctx.
 *
 * Notes:
 *   - The results are written onto the nodes: Expr.type, Expr.func, Expr.field, and
 *     Stmt.type. Code generation then needs no type inference at all, which is what
 *     stops every new language feature from having to be implemented twice, once in
 *     the checker and once in the generator.
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m);

#endif /* EXTC_CHECK_H */
