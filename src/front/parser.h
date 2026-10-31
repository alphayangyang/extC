/**
 * @file parser.h
 * @brief Second stage of the extC pipeline: recursive-descent parsing.
 *
 * Consumes the token vector from `lexAll` and builds the ast.h `Module`. It resolves
 * nothing: names, module paths, and generic arguments are passed through as written, for
 * the loader and the type checker to interpret. A statement ends at a newline; a semicolon
 * after a newline is optional and ignored.
 */
#ifndef EXTC_PARSER_H
#define EXTC_PARSER_H

#include "ast.h"
#include "base.h"

/**
 * @brief Parse one file's tokens into `out`.
 *
 * `out` is the Module that gets **appended to**: the caller initializes it once, so the
 * prelude and the user file end up in the same module. Accepted syntax:
 *
 *     fn name(p: T, q: T2) -> T { ... }   // no `->` means void
 *     struct Name { field: T <newline> field: T }
 *     let x: T = e / var x: T = e         // the type may be omitted
 *     if / else if / else / while / return / break / continue / match
 *     a.f(x)  is  f(a, x)                 // a method's first parameter is self
 *
 * The token vector must end with TK_EOF (`lexAll` always appends one).
 *
 * @param ctx    Reports the first syntax error; parsing stops once it is set.
 * @param arena  Owns every AST node and every copied name.
 * @param toks   Token vector produced by `lexAll`.
 * @param out    Module to append to.
 * @return       `true` when the whole token vector was consumed; `false` when
 *               `ctx->hasError` is set, in which case `out` holds the declarations parsed
 *               so far.
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out);

#endif /* EXTC_PARSER_H */
