/* Region registry - the runtime half of the ECS-style regions (docs/topics/REGIONS.md).
 *
 * This subsystem lives in its own file on purpose (REGIONS.md section 11): regions are
 * new, they touch several passes, and scattering them over `codegen.c` / `check_expr.c`
 * would repeat a mistake this project has already paid for - "a little bit in every file,
 * complete in none" (the escape pass was exactly that). Everything region-shaped starts
 * here; the passes keep a call site or two.
 *
 * Phase 1 is the runtime only: the registry, the parent chain, and the release hook. The
 * text is emitted on demand - a program that never mentions a region carries none of it -
 * through the same path `extc_raw_enter` uses.
 */

#ifndef EXTC_REGIONS_H
#define EXTC_REGIONS_H

#include "base.h"

/* Append the registry's C text to `out`.
 *
 * Params:
 *   a   - arena for the temporary buffer
 *   out - buffer the generated C is being written into
 *
 * Notes:
 *   - Called only when the program declares the region externs, so the text is free for
 *     every program that does not use regions.
 *   - The names are all `extc_region_*`: the library reaches them through `extern!`, which
 *     is why they cannot be `static`.
 */
void regionsEmitRuntime(Arena *a, Buf *out);

#endif /* EXTC_REGIONS_H */
