/**
 * @file modules.h
 * @brief The module loader: resolves `use`, loads files, and merges them into one unit.
 *
 * `use a::b` is a semantic import, not a textual include: a module is compiled once and
 * its declarations are merged in topological order, so a dependency is merged before the
 * declarations that need it.
 */
#ifndef EXTC_MODULES_H
#define EXTC_MODULES_H

#include "ast.h"
#include "types.h"

/**
 * @brief Load every module the root file imports and merge them into `out`.
 *
 * @param a           Arena that owns every loaded module.
 * @param out         The merged module; the prelude must already be in it.
 * @param rootm       The root file's own parsed module.
 * @param rootCtx     The root file's source context, which blames errors in that file.
 * @param rootPath    Path of the root file; its directory becomes the project root.
 * @param searchDirs  Directories from `-I`, or `NULL`.
 * @param outCtxs     Receives the Ctx of every loaded module so the caller can render their
 *                    diagnostics afterwards, or `NULL` when the caller does not need them.
 * @param tt          Type table; imported `const` values are registered into it so a fixed
 *                    array may take its length from another module. May be `NULL` (no
 *                    registration) for a caller that only wants the merge.
 * @return            `false` when any error was reported; the diagnostics are already
 *                    printed and compilation must stop.
 *
 * `rootCtx` is stored by pointer, never copied: a copy would swallow the diagnostics
 * reported through it. `outCtxs` is used by pointer as well, because its length has to
 * reach the caller.
 */
bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs, TypeTable *tt);

#endif
