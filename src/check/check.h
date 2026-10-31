/**
 * @file check.h
 * @brief Type checker: resolves names, infers types, and enforces the borrow and escape
 * rules.
 *
 * Every result is written back into the AST, so code generation never has to infer a type.
 */
#ifndef EXTC_CHECK_H
#define EXTC_CHECK_H

#include "ast.h"
#include "base.h"
#include "types.h"

/**
 * @brief Check one module and annotate it in place.
 *
 * Results are written onto the nodes: `Expr.type`, `Expr.func`, `Expr.field`, and
 * `Stmt.type`. Code generation therefore needs no type inference at all, so each language
 * feature is implemented once, in the checker, instead of twice, once here and once in the
 * generator.
 *
 * @param ctx    Diagnostic context of the file being checked.
 * @param arena  Arena that owns everything this pass allocates.
 * @param tt     Type table shared by the prelude and every module.
 * @param m      Module to check; it is already loaded and registered in `tt`.
 * @return       `false` when an error was recorded in `ctx`.
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m);

/**
 * @brief Turn off S1 element-chain fusion (the `--no-fuse` driver switch).
 *
 * @param off  `true` disables fusion.
 */
void checkSetNoFuse(bool off);

/**
 * @brief Give the checker the stdlib directory, so the "no such method" hint can be read
 * from `INDEX`.
 *
 * The loader (a later layer) resolves that directory, but the hint lives on the checker
 * side next to the diagnostic that uses it, so this call goes **downward**: the loader
 * calls in through this public entry point, and the checker does not reach for the module
 * layer's header.
 *
 * @param stdDir  stdlib directory; the hint is read from it on the first miss, on the error
 *                path only.
 */
void hintIndexSetStdDir(const char *stdDir);

#endif /* EXTC_CHECK_H */
