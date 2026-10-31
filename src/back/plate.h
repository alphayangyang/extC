/**
 * @file plate.h
 * @brief On-demand emission for the plate (heap) runtime: `extc_viewOf` and
 * `extc_memCopy`.
 *
 * It has the same shape as the gates in pools.h and memfind.h: one flag in codegen
 * (`CG.needPlate`), and this C block is appended to the same translation unit on demand.
 * The trigger is the **declaration**: `extern!("extc-heap")` appears only in the
 * privileged layer `stdlib/std/sys/heap.extc`.
 *
 * The view has to come from the compiler because **a view cannot be built in extC**: a
 * view type is a generic instance (`slice<u8>`), and a struct literal cannot express it
 * (`slice<u8> { … }` is rejected when a type appears in expression position); and it has
 * to be built from a `ref u8`, while the `ref void` -> `ref T` loop is deliberately
 * closed. So "reinterpret a pointer plus a length as a view" lives in the privileged
 * layer, once.
 */
#ifndef EXTC_PLATE_H
#define EXTC_PLATE_H

#include "base.h"
#include "typelayer.h"   /* the name predicates below live there (they are pure name questions) */
#include <string.h>

/**
 * @brief Append `extc_viewOf` to `out`.
 *
 * **One declaration, one flag**: the trigger is "the program declared this name", so a
 * program that only declares the copy does not carry the view (and the converse holds
 * too). The same discipline as `memfindEmitRuntime`, one notch finer.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void plateEmitViewOf(Arena *a, Buf *out);

/**
 * @brief Append the whole-block copy `extc_memCopy` to `out`.
 *
 * It is the second item behind the same gate: the declaration of `memcpy` would clash
 * with `<string.h>` (the generated file includes it), so this is the only way out for it
 * as well. It hangs off the **plate runtime block**, not the byte-search block: that
 * block's trigger is the declaration of `extern!("extc-mem")`, and every user of
 * `std::string` declares it.
 *
 * @param a    Arena (unused here; kept to match the other emission entry points).
 * @param out  Generated C being written.
 */
void plateEmitMemCopy(Arena *a, Buf *out);

#endif
