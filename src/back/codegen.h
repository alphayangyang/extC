/* The C code generator: it turns a checked module into C source text. It infers no
 * types of its own -- every decision it needs has already been written onto the tree
 * by the checker.
 */
#ifndef EXTC_CODEGEN_H
#define EXTC_CODEGEN_H

#include "ast.h"
#include "base.h"
#include "types.h"

/* Translate a checked module into C source and append it to out.
 *
 * Params:
 *   ctx     - diagnostics context, which also holds the path used in `#line`
 *   arena   - arena for everything the generator allocates
 *   tt      - the type table; it also holds the generic instances, and those are what
 *             the monomorphised code is generated from
 *   m       - the checked module
 *   lineMap - emit `#line` directives, so that the C compiler's diagnostics point
 *             back at the .extc line the code came from
 *   out     - buffer the C text is appended to
 *
 * Returns:
 *   False when an error was recorded in ctx while generating.
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out);

#endif /* EXTC_CODEGEN_H */
