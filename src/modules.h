/* The module loader: resolve `use`, load files, merge them into one unit.
 *
 * `use a::b` is a semantic import - a module is compiled once and its declarations
 * are merged in topological order - not a textual include. The rules and the limits
 * of the current implementation are described at the top of modules.c.
 */

#ifndef EXTC_MODULES_H
#define EXTC_MODULES_H

#include "ast.h"

/* Load every module the root file imports and merge them into `out`.
 *
 * Params:
 *   a          - arena that owns every loaded module
 *   out        - the merged module; the prelude must already be in it
 *   rootm      - the root file's own parsed module
 *   rootCtx    - the root file's source context, which blames errors in that file
 *   rootPath   - path of the root file; its directory becomes the project root
 *   searchDirs - directories from `-I`, or NULL
 *   outCtxs    - receives the Ctx of every loaded module so the caller can render
 *                their diagnostics afterwards, or NULL when the caller does not need
 *                them
 *
 * Returns:
 *   False when any error was reported; the diagnostics are already printed and
 *   compilation must stop.
 *
 * Notes:
 *   - `rootCtx` is stored by pointer, never copied. A copy would swallow the
 *     diagnostics reported through it.
 *   - `outCtxs` is used by pointer as well, because its length has to reach the
 *     caller.
 */

bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs);

#endif
