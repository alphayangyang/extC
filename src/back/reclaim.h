/**
 * @file reclaim.h
 * @brief On-demand emission entry point for the reclamation runtime: bitmap slab
 * allocator + holder-count zeroing + region sweep.
 *
 * It has the same shape as the gates in pools.h and plate.h: one flag in codegen, and this
 * C block is appended to the same translation unit on demand. The trigger is "the program
 * has heap storage that needs reclaiming", so a program without reclamation carries not one
 * byte of it.
 *
 * The emitted text is **self-sufficient**: it depends only on `<stdint.h>` / `<stddef.h>` /
 * `<stdlib.h>` / `<stdio.h>`, which the generated file already includes at the top, and it
 * can be emitted **before** user code -- it references no user type: the pointer-field
 * layout of a type is handed to the region by the caller through the `ExtcRcType` table.
 * Every function and datum in the text is `static`, so it produces no external symbol and
 * cannot clash with user code (all names carry the `extc_rc_` prefix).
 *
 * Mechanism: a region is a run of slabs aligned to `EXTC_RC_SLAB_BITS`; a payload is
 * rounded up to 8 bytes to form a slot (not a power of two -- an 80-byte object occupies 80
 * bytes), the slab is found from the object pointer with one bitwise AND, and the slot
 * number with one multiply by a reciprocal and a shift. The holder count hangs only on
 * **pointer assignment**, and reaching zero returns the slot to the free list on the spot.
 * Cycles, and the parts whose counts are still held alive by the out-edges of dead objects,
 * are the region sweep's fallback: it marks starting from the roots plus the cross-region
 * registration bitmap, then scans 64-bit words in order and frees the unmarked slots. Marking
 * follows pointer fields by the **type descriptor carried by each slot**; the descriptor is
 * handed to the runtime with the slot by the allocation point (codegen), so there is no
 * region-level type table here.
 *
 * Pointers returned by the plate, a pool, the stack, or C are not slots of this runtime:
 * every slab is registered at creation in a table sorted by base address, and
 * `inc`/`dec`/`register`/marking **ask `owns` first**, skipping anything outside the table
 * or on the free list. Without that guard those pointers would be taken for slab addresses,
 * bitwise-ANDed, and read as unrelated memory (the plate's 4 GiB reservation is not even
 * committed) -> a segfault.
 */
#ifndef EXTC_RECLAIM_H
#define EXTC_RECLAIM_H

#include "base.h"

/**
 * @brief Append the reclamation runtime's C text to `out`.
 *
 * One append is enough, and it must come **before the code that calls it** (top of the
 * generated file, before user functions): the functions in the text are `static`, so a
 * call site has to see them first. The text contains no user type, so it is equally valid
 * before or after the user type definitions. Append it only once -- appending twice would
 * define the same batch of `static` functions twice.
 *
 * The appended text provides these entry points: for regions
 * `extc_rc_regionNew` / `extc_rc_regionFloor` / `extc_rc_regionFree`; for slot allocation
 * `extc_rc_alloc(region, bytes, type descriptor)`; for the bump region `extc_rc_bump` /
 * `extc_rc_bumpReset`; for counting `extc_rc_inc` / `extc_rc_dec` / `extc_rc_initRef` /
 * `extc_rc_storeRef`; for cross-region registration `extc_rc_storeRefOwned(slot, v, owner)`
 * (the source is provable) and `extc_rc_storeRefUnknown(slot, v)` (the source is not
 * provable => the target region leaves the sweep entirely); for sweeping
 * `extc_rc_mark` / `extc_rc_sweep` / `extc_rc_collect` /
 * `extc_rc_maybeCollect(region, root table, root count)`; plus read-only accounting
 * queries. The type descriptor `ExtcRcType` and the fallback descriptor
 * `EXTC_RC_CONSERVATIVE` (which marks the whole slot) are also defined here.
 *
 * @param a           Arena (unused here; kept to match the other emission entry points).
 * @param out         Generated C being written.
 * @param noCount     `EXTC_RC_NOCOUNT`: the counting entry points compile to no-ops.
 * @param timeOn      `EXTC_RC_TIME`: per-item timing in the runtime.
 * @param sweepFree   `EXTC_RC_SWEEP_FREE`: the event tier, where `shouldSweep` is always 0.
 * @param localStore  `EXTC_RC_LOCALSTORE`: the write-only store entry point for local slots
 *                    (meaningful only in the no-count tier).
 * @param frameRoots  Frame root chain: 1 = emit the chain push/lookup code. No emitter
 *                    pushes onto the chain yet, so callers pass 0.
 * @param weakOn      weak references: 1 = emit the [rt:weak] section and define EXTC_RC_WEAK;
 *                    otherwise the emitted text is unchanged.
 */
void reclaimEmitRuntime(Arena *a, Buf *out, int noCount, int timeOn, int sweepFree, int localStore,
                        int frameRoots, int weakOn);

#endif /* EXTC_RECLAIM_H */
