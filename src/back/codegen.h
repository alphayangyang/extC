/**
 * @file codegen.h
 * @brief C code generation: turn a checked module into C source text.
 *
 * The pass infers no types of its own -- every decision it needs has already
 * been recorded on the tree by the checker.
 */
#ifndef EXTC_CODEGEN_H
#define EXTC_CODEGEN_H

#include "ast.h"
#include "base.h"
#include "types.h"

/**
 * @brief Translate a checked module into C source and append it to `out`.
 *
 * @param ctx       Diagnostic context; it also holds the path used by `#line`.
 * @param arena     Arena that backs everything the generator allocates.
 * @param tt        Type table; it also holds the generic instances whose
 *                  monomorphized code is emitted here.
 * @param m         The checked module.
 * @param lineMap   Emit `#line` directives so C compiler diagnostics point back
 *                  at the `.extc` line the code came from.
 * @param out       Buffer the C text is appended to.
 * @return          `false` when generation recorded an error in `ctx`.
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out);

#endif /* EXTC_CODEGEN_H */
