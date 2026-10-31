/**
 * @file memfind.h
 * @brief On-demand emission entry point for the byte-search runtime (`extc_memFind` /
 * `extc_memEq`).
 *
 * It has the same shape as the two gates in pools.h and coroutine.h: one flag in codegen
 * (`CG.needMemFind`), and after every function body has been emitted this C block is
 * appended to the same translation unit on demand.
 *
 * The trigger is the **declaration**, not a call: `extern!("extc-mem")` appears only in
 * the privileged layer `stdlib/std/sys/mem.extc`, so a program that `use`s it is asking
 * for this runtime; a program without the declaration carries not one extra byte (the
 * same discipline as the other runtime blocks).
 *
 * The text declares `memmem` itself: it is not ISO C, and `<string.h>` has already been
 * read by the time this block appears, so `_GNU_SOURCE` is not enabled (changing the
 * compilation mode of the whole translation unit for one function is the wrong price).
 */
#ifndef EXTC_MEMFIND_H
#define EXTC_MEMFIND_H

#include "base.h"

/**
 * @brief Append the byte-search runtime (`extc_memFind`, `extc_memEq`) to `out`.
 *
 * @param a    Arena the emitted text is copied from (unused now; kept to match the other
 *             emission entry points).
 * @param out  Generated C, written after every function body.
 */
void memfindEmitRuntime(Arena *a, Buf *out);

#endif
