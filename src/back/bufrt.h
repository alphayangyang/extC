/**
 * @file bufrt.h
 * @brief On-demand emission for the buffer substrate (`std::data::buf`).
 *
 * Same shape as `plate.h` and `memfind.h`: one flag in codegen (`CG.needBuf`) and one text
 * block appended to the generated translation unit. The trigger is the **declaration**:
 * `extern!("extc-buf")` appears only in `stdlib/std/sys/heap.extc`.
 *
 * Why two of the three operations live in the compiler and not in extC: a buffer keeps one
 * aligned reservation and commits inside it. Aligning the returned range up to 2 MiB and
 * advising it `MADV_HUGEPAGE` needs pointer arithmetic and integer-to-pointer conversion,
 * which the language deliberately refuses. Everything that *can* be written in extC (the
 * reservation policy, the commit frontier, lengths, moves) stays in `buf.extc`.
 */
#ifndef EXTC_BUFRT_H
#define EXTC_BUFRT_H

#include "base.h"

/**
 * @brief Append the buffer substrate (`extc_buf_reserve` / `extc_buf_release` /
 * `extc_buf_discard`) to `out`.
 *
 * The block declares `mmap` / `munmap` / `madvise` itself with the same types the stdlib
 * emits for plates, so a program that uses both compiles; the preprocessor guards keep the
 * `PROT_*` / `MAP_*` / `MADV_*` macros from colliding with the reclamation runtime's copy.
 *
 * @param a    Arena (unused: the block is plain text).
 * @param out  Generated C being written.
 * @param mt   Whether the unit can start threads (`par`). The arena is process-wide, so it
 *             needs a lock exactly then; a single-threaded unit gets a block with the lock
 *             compiled out, which is what makes a churn of small buffers competitive with a
 *             bump-pointer pool.
 */
void bufRtEmitRuntime(Arena *a, Buf *out, int mt, int rcTier);

#endif