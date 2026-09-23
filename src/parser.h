/* extC syntax, part two of the pipeline: recursive-descent parsing.
 *
 * Consumes the token vector from lexAll and builds an ast.h Module.  It resolves
 * nothing: names, module paths, and generic arguments travel through as written,
 * for the loader and the type checker to interpret.  Statements end at a
 * newline, and the semicolon that may follow one is optional and ignored.
 */
#ifndef EXTC_PARSER_H
#define EXTC_PARSER_H

#include "ast.h"
#include "base.h"

/* Parse one file's tokens into `out`.
 *
 * Params:
 *   ctx   - reports the first syntax error; parsing stops once it is set
 *   arena - owns every AST node and every copied name
 *   toks  - token vector from lexAll, ending in TK_EOF
 *   out   - Module to APPEND to; the caller initializes it once, which is how
 *           the prelude and the user file end up in the same module
 *
 * Returns:
 *   true when the whole token vector was consumed; false when ctx->hasError is
 *   set, in which case `out` holds the declarations parsed so far.
 *
 * Notes:
 *   - The accepted grammar:
 *       fn name(p: T, q: T2) -> T { ... }   // no `->` means void
 *       struct Name { field: T <newline> field: T }
 *       let x: T = e / var x: T = e         // the type may be omitted
 *       if / else if / else / while / return / break / continue / match
 *       a.f(x)  is  f(a, x)                 // a method takes self as its first
 *                                           // argument
 *   - A token vector without its trailing TK_EOF is malformed; lexAll always
 *     appends one.
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out);

#endif /* EXTC_PARSER_H */
