/**
 * @file pools.h
 * @brief Pool registry: the runtime half of ECS-style pools.
 *
 * The subsystem has a file of its own: pools touch several passes, and spreading them
 * through `codegen.c` / `check_expr.c` would repeat the pattern of "a little in every
 * file, complete in none" (which is exactly how the escape pass is not written). Every
 * pool-shaped thing starts here, and each pass keeps only one or two call sites.
 *
 * The runtime is emitted on demand -- a program that never mentions a pool carries not one
 * byte of it -- through the same path as `extc_raw_enter`.
 */

#ifndef EXTC_POOLS_H
#define EXTC_POOLS_H

#include "base.h"

/**
 * @brief Append the registry's C text to `out`.
 *
 * It is called only when the program declared the pool extern, so a program without pools
 * does not pay for this text. Every name is `extc_pool_*`: the library reaches them
 * through `extern!`, so they cannot be `static`.
 *
 * @param a    Arena for temporary buffers.
 * @param out  Generated C being written.
 */
void poolsEmitRuntime(Arena *a, Buf *out);

/**
 * @brief Append the `dyn` half of the runtime.
 *
 * It is emitted separately so that a program using pools only keeps its artifact
 * unchanged.
 *
 * @param a    Arena for temporary buffers.
 * @param out  Generated C being written.
 */
void poolsEmitDynRuntime(Arena *a, Buf *out);

#endif /* EXTC_POOLS_H */
