/* ==================== Reclamation: static root tables and sweep triggers ====================
 *
 * The trigger point is an **allocation point** (`new T` / `new [N]T` / `alloc<T>(n)` /
 * `@overwrite`), where codegen statically emits two things:
 *
 *   1. A type descriptor `ExtcRcType`: the offsets at which the reference slots of this
 *      allocation point's type live. It is stored into the slot and travels with the object, and
 *      marking follows pointer fields through the **object's own** descriptor (obligation 4 of
 *      the sweep).
 *   2. A root table: the **values** of the reference slots that are live at this moment (which
 *      slots are live is something the compiler knows).
 *
 * Root tables come from **lexical scope**: declaring a storage that contains references
 * registers each of its leaf reference slots in `rcRoots` (`a[i].f` is expanded like that -- a
 * C-level array is a root too), and leaving the block truncates the registrations. So the root
 * table at any trigger point is: locals of the enclosing blocks that are still live + this
 * function's parameters + the globals.
 *
 * Two safety directions:
 *  - **Emit no trigger at all while the root table is incomplete** (`rc.rootsUnreliable`):
 *    skipping a sweep is better than missing a root and freeing a live object. Storage that
 *    cannot be expanded statically, such as enum payloads, generic enums and coroutine frames,
 *    counts as incomplete.
 *  - An offset entry count above 32 (EXTC_RC_MARK_PUSH_STEP territory) is treated as
 *    incomplete too: a wider descriptor would let one pop push an unbounded number of
 *    children inside one bounded step (see rcEmitTypeDesc).
 *
 * A third safety direction (the **C hole**: the root table is complete but a calling frame still
 * holds a live reference):
 *
 *  - **Sweep only a region this frame owns** (`rcFrameOwnsArena` / `frameOwned` in
 *    `rcAllocCore`). The static root table is **lexical**: it covers this frame's locals and the
 *    globals and **structurally cannot see the live references of the other frames on the call
 *    stack**. The home arena (`(*__extc_home)`) is handed down from the caller and the
 *    thread-local arena (`extc_tls_arena`) is shared by a whole worker call chain -- when an
 *    inner frame allocates into either, an object in that region may also be pointed at by an
 *    **outer frame's** locals; one trigger from the inner frame would then use a root table built
 *    only from its own lexical scope, an **incomplete** seed, and free a whole live subtree as
 *    garbage (observed: `nroots=0`, and **every one** of the 65537 freed slots had `hold>0`).
 *    Therefore a region this frame does not own gets no trigger at all. This frame's own block
 *    arena (`&__extc_a[lvl]`, lvl >= 1) still triggers as usual: an outer frame cannot obtain a
 *    reference into it (the checker blocks cross-frame storage, and a value that escapes is
 *    placed in the **home arena** when it is allocated), so this frame's root table is complete
 *    for it -- which is also why the sweeping capability is not disabled as a whole.
 *
 *    **Taking part of that capability back (subsets A+B)**:
 *    - **A -- `main` is the home-arena exception**: `main` has no caller and its home arena is
 *      its own `&__extc_a[1]` (the `__extc_home = &__extc_a[1]` line in `genFunc`) => the root
 *      table is complete for it, and `rcFrameOwnsArena` returns true for `ARENA_HOME` in `main`.
 *    - **B -- owed flag + owner settlement**: every other function's home arena belongs to the
 *      **frame that owns it**. An inner frame does not sweep that region and only records "one
 *      collection owed" (`rcEmitOwe` => the runtime `extc_rc_owe`, whose bookkeeping carries the
 *      `shouldSweep` pressure test); the owning frame settles it with its own static root table
 *      **before its next call** (`rcEmitOwedCheck` => `extc_rc_checkOwed`).
 *
 *    **The soundness invariant of B (this is its whole justification)**: when a collection
 *    happens the collector must have no active callee subtree -- at that moment its static root
 *    table (this frame's locals + parameters + globals) is a **complete** seed. So a settlement
 *    point can only sit before the call where "the previous batch of callees has returned and
 *    the next has not been entered"; for the same reason a frame-level "roots unreliable" marker
 *    is **not** needed (that is only required by a whole runtime root stack). An owed flag that is
 *    never settled (the owner never calls again) makes that region wait for frame destruction --
 *    a looser bound, not a dangling pointer.
 *
 *    The cost, stated as it is: shapes that only allocate into the home arena and whose owner
 *    never calls again afterwards (`hole_c`, the one-shot entry of `persist_nodes`) still wait for
 *    frame destruction; a shape where the owner repeatedly calls callees in a loop
 *    (`bench/stress/stress.extc`) still peaks at 10.86-10.92 MB.
 *
 * A view (`slice<T>` / `mut slice<T>`) is not a root: its `data` points into an arena, a plate or
 * C storage, and rc slots are only produced by `new T` / `new [N]T` / `alloc<T>(n)` /
 * `@overwrite`, none of which produces a view.
 */

/* How reference slots are distributed in a type. */
typedef enum {
    RC_NO_REFS,       /* no reference slots inside */
    RC_LEAF_REFS,     /* reference slots can be enumerated statically (structs and fixed-length arrays expand
     * recursively) */
    RC_CONSERVATIVE   /* not enumerable (enum payloads, generic enums, type parameters, ...) => do not trust the root
     * table */
} RcScan;

static RcScan rcScanType(CG *g, Type *t, int depth);

/**
 * @brief Scan a struct's fields: a conservative field makes the whole struct conservative, and a
 *        field with references makes the whole struct have references.
 *
 * @param g       The generator.
 * @param fields  The `FieldDef*` vector.
 * @param depth   Recursion depth, guarding against type cycles.
 * @return        The struct's reference-slot distribution.
 */
static RcScan rcScanFields(CG *g, Vec *fields, int depth) {
    bool any = false;
    for (size_t i = 0; i < fields->len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(fields, i);
        RcScan s = rcScanType(g, fd->type, depth + 1);
        if (s == RC_CONSERVATIVE) return RC_CONSERVATIVE;
        if (s == RC_LEAF_REFS) any = true;
    }
    return any ? RC_LEAF_REFS : RC_NO_REFS;
}

/**
 * @brief Determine whether a type contains reference slots and whether it can be enumerated
 *        statically.
 *
 * A view counts as having no reference slots (see the block comment above); a generic struct
 * instance enters the instance context before its fields are scanned; an enum's or a generic
 * enum's payload is not expanded => conservative.
 *
 * @param g      The generator (supplies the instantiation context).
 * @param t      The type to scan; NULL counts as conservative.
 * @param depth  Recursion depth, guarding against cycles.
 * @return       The reference-slot distribution.
 */
static RcScan rcScanType(CG *g, Type *t, int depth) {
    if (!t || depth > 16) return RC_CONSERVATIVE;
    t = subst(g, t);
    if (!t) return RC_CONSERVATIVE;
    switch (t->kind) {
    case TY_REF:
        return RC_LEAF_REFS;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return RC_NO_REFS;
    case TY_ANY:
        /* What a dynamic value holds is not visible here, so no offset table can describe it. */
        return RC_CONSERVATIVE;
    case TY_ENUM:
        return RC_CONSERVATIVE;      /* A variant payload may carry references and cannot be expanded statically => do not trust the
     * root table */
    case TY_ARRAY:
        return rcScanType(g, t->inner, depth + 1);
    case TY_STRUCT:
        return rcScanFields(g, &t->sdef->fields, depth);
    case TY_GENERIC: {
        if (isView(t)) return RC_NO_REFS;
        if (!t->sdef) return RC_CONSERVATIVE;
        /* A pool/plate container's elements live on the container's own plate and **not in the C
         * struct's fields** (`pool<ref node>` and `vector<ref node>` element references are exactly
         * this): the descriptor fields are three views and the elements are written as bare slots
         * on the plate. When the field scan finds **not one reference leaf**, this descriptor has
         * no reference slots of its own -- the root table owes nothing here and a trigger is
         * emitted as usual; references inside elements are the responsibility of the write-site
         * barrier (`storeRefSlot` accounts by slot when it can find an rc slot and otherwise pins
         * the value as a root). Only when references are **also** visible in the fields (inline
         * arrays, structs embedded by value) does the type still count as not enumerable, going
         * down the conservative path of `rcEnumRootable`. */
        bool targRefs = false;
        for (size_t i = 0; i < t->targs.len; i++) {
            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS) {
                targRefs = true;
                break;
            }
        }
        SubCtx sv = substEnter(g, t);
        RcScan s = rcScanFields(g, &t->sdef->fields, depth);
        substLeave(g, sv);
        if (targRefs && s != RC_NO_REFS) return RC_CONSERVATIVE;
        return s;
    }
    /* kind-default: only the kinds listed above take part in "has reference slots"; any new kind
     * may hide a reference, so falling through to here counts as not enumerable (do not trust the
     * root table), which is the safe direction. */
    default:
        return RC_CONSERVATIVE;
    }
}

/**
 * @brief Does this type contain reference slots at all? (`rcScanType != RC_NO_REFS`, so the
 *        "cannot be enumerated" answer also counts.)
 *
 * `genViewCopier` needs this question earlier in the file than `rcScanType` is defined; the
 * declaration there and this definition are one pair.
 *
 * @param g  The generator (supplies the instantiation context).
 * @param t  The type to ask about.
 * @return   True when a byte copy of this type carries reference words.
 */
static bool typeHasRefSlots(CG *g, Type *t) { return rcScanType(g, t, 0) != RC_NO_REFS; }

/**
 * @brief Collect a type's reference leaf slots as "relative offset" C constant expressions.
 *
 * The expressions are written with `offsetof` / `sizeof` (the layout is not computed here: a
 * wrong computation silently misses a mark), arrays are expanded per element and nested structs
 * add their offsets.
 *
 * @param g      The generator.
 * @param t      The type to expand.
 * @param base   The current sub-object's offset expression; the top level passes an empty string
 *               meaning 0.
 * @param out    A `const char *` vector that receives the offset expression of every reference
 *               slot.
 * @param depth  Recursion depth, guarding against cycles.
 */
static void rcCollectOffsets(CG *g, Type *t, const char *base, Vec *out, int depth) {
    if (!t || depth > 16 || out->len > 4096) return;
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(out) = (*base) ? base : "0u";
        return;
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        /* A fixed-length array is `struct { T data[N]; }` in C (see the array type's emission), so the
         * element offset starts at `offsetof(array type, data)`. */
        const char *d = (*base) ? arenaPrintf(g->arena, "(%s + offsetof(%s, data))",
                                              base, cType(g, t))
                                : arenaPrintf(g->arena, "offsetof(%s, data)", cType(g, t));
        for (int64_t i = 0; i < t->asize && out->len <= 4096; i++) {
            const char *off = arenaPrintf(g->arena, "(uint32_t)(%s + %lld * sizeof(%s))",
                                          d, (long long)i, cType(g, e));
            rcCollectOffsets(g, e, off, out, depth + 1);
        }
        return;
    }
    case TY_STRUCT: {
        const char *cn = cType(g, t);
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            const char *off = (*base)
                ? arenaPrintf(g->arena, "(%s + offsetof(%s, %s))", base, cn, fd->name)
                : arenaPrintf(g->arena, "offsetof(%s, %s)", cn, fd->name);
            rcCollectOffsets(g, fd->type, off, out, depth + 1);
        }
        return;
    }
    case TY_GENERIC:
        if (isView(t) || !t->sdef) return;
        SubCtx sv = substEnter(g, t);
        {
            const char *cn = cType(g, t);
            for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
                const char *off = (*base)
                    ? arenaPrintf(g->arena, "(%s + offsetof(%s, %s))", base, cn, fd->name)
                    : arenaPrintf(g->arena, "offsetof(%s, %s)", cn, fd->name);
                rcCollectOffsets(g, fd->type, off, out, depth + 1);
            }
        }
        substLeave(g, sv);
        return;
    /* kind-default: offsets only recurse through the REF / ARRAY / STRUCT / GENERIC shapes; the
     * other kinds have no reference slots to collect. A new kind that contains reference slots
     * must add a case here and must not slide through the default. */
    default:
        return;
    }
}

/**
 * @brief Emit the type descriptor for one allocation and return a C expression pointing at it.
 *
 * No reference slots => `(const ExtcRcType *)0` (marking stops at this object); not enumerable =>
 * `EXTC_RC_CONSERVATIVE` (marking scans every word of the slot).
 *
 * The descriptor is a **file-scope static const** written into `g->rcDesc` and spliced into the
 * unit before the function bodies like any other on-demand definition (the address has static
 * storage duration, so it fits in the slot). Deduplicated by the offset-table text: one type is
 * emitted once even when it appears at many allocation points -- one per point would pile
 * isomorphic descriptors into the same slab's type table and make the allocation path's `typeIdx`
 * scan them linearly (about 40 Ir per allocation across the eight node-allocation points of
 * `shard_bands`, a third of that shape's cost).
 *
 * @param g  The generator; returns an empty descriptor while `rc.reclaim` is off.
 * @param t  The allocated type.
 * @return   A C expression pointing at an `ExtcRcType`.
 */
static const char *rcEmitTypeDesc(CG *g, Type *t) {
    if (!g->rc.reclaim || g->prefixBlk == 0) return "(const ExtcRcType *)0";
    RcScan sc = rcScanType(g, t, 0);
    if (sc == RC_NO_REFS) return "(const ExtcRcType *)0";
    if (sc == RC_CONSERVATIVE) return "((const ExtcRcType *)-1)";
    Vec offs;
    vecInit(&offs, g->arena, sizeof(const char *));
    rcCollectOffsets(g, t, "", &offs, 0);
    if (offs.len == 0) return "(const ExtcRcType *)0";
    /* Fan-out limit: a descriptor with more entries falls back to the conservative scan.
     * The number is a **pause contract** constant, not a formatting choice. One bounded step
     * pops at most EXTC_RC_MARK_STEP_POP_MAX objects and the step push cap charges the
     * children it pushes once per batch, so the pushes a single pop can add must be a
     * compile-time constant: 32 makes the worst case of a bounded step about 8k pushes
     * (200 x 32 + EXTC_RC_MARK_PUSH_STEP) instead of 200 x 4096. A struct with more than 32
     * reference fields keeps working through the conservative scan, which walks every word
     * of the slot and is bounded by EXTC_RC_SCAN_CHUNK per step, at the cost of retaining
     * values that happen to look like pointers. */
    if (offs.len > 32) return "((const ExtcRcType *)-1)";
    Buf b;
    bufInit(&b, g->arena);
    for (size_t i = 0; i < offs.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, "%s", *(const char **)vecAt(&offs, i));
    }
    /* Dedup: the key is the offset-table text (it decides both the pointer count and every offset,
     * so equal keys are isomorphic). A descriptor is pure data, sharing one has no semantic
     * difference, and the per-point cost sits on the allocation path (see the function comment
     * above). */
    const char *key = bufCstr(&b);
    int id = -1;
    for (size_t i = 0; i < g->rcDescKeys.len; i++) {
        if (strcmp(*(const char **)vecAt(&g->rcDescKeys, i), key) == 0) { id = (int)i; break; }
    }
    if (id < 0) {
        id = (int)g->rcDescKeys.len;
        *(const char **)vecPush(&g->rcDescKeys) = key;
        bufPrintf(&g->rcDesc,
                  "__attribute__((unused)) static const uint32_t __extc_rco%d[] = { %s };\n"
                  "__attribute__((unused)) static const ExtcRcType __extc_rct%d = { %zuu, __extc_rco%d };\n",
                  id, key, id, offs.len, id);
    }
    return arenaPrintf(g->arena, "&__extc_rct%d", id);
}

/**
 * @brief Whether the region this allocation goes into is **owned by this frame** (its static
 *        root table is a complete seed for it).
 *
 * The test (see "third safety direction" above): this frame's own block arena
 * (`&__extc_a[lvl]`, `lvl >= 1`) counts; the home arena (`ARENA_HOME`: the `(*__extc_home)`
 * handed down from the caller) generally does not; the thread-local arena (`parTls`) does not
 * either -- it is likewise shared by a whole worker call chain, for the same reason as the home
 * arena. A coroutine step's block arena lives in the frame (`f->arena<lvl>`) and belongs to the
 * frame itself: `genFunc` registers the fields listed in `coroFrame` as roots, and the checker
 * forbids yielding a value that carries references (its `yield` rule for coroutines) => the
 * caller cannot hold a reference into that region, so the root table is complete for it.
 *
 * **`main` is the home-arena exception (subset A)**: `main` has no caller and its home arena is
 * its own `&__extc_a[1]` (`__extc_home = &__extc_a[1]` in `genFunc`) => the static root table is
 * complete for it, so it counts. Every other function's home arena belongs to **the frame that
 * owns it**: an inner frame does not sweep it and only records an owed collection (`rcEmitOwe`),
 * and the owner settles it with its own static root table while it has no active callee subtree
 * (`rcEmitOwedCheck`).
 *
 * @param g    The generator.
 * @param lvl  `ARENA_HOME` (-1) or a block level of 1 and above.
 * @return     true = owned by this frame, so a sweep may be triggered on this region.
 */
static bool rcFrameOwnsArena(CG *g, int lvl) {
    if (g->parTls) return false;
    if (lvl == ARENA_HOME) return g->inMain;
    return true;
}

/**
 * @brief Which trigger route an allocation point takes (three states, see `RcOwn`).
 *
 * The static test only reaches "owns / does not own"; ownership of the thread-local arena is only
 * known at **run time**: a whole worker call chain shares `extc_tls_arena`, the entry frame (the
 * one where `extc_rc_tlsDepth++ == 0`) has only the C runtime as its caller and its static root
 * table is a complete seed => it owns; an inner frame's root table cannot see the calling frame's
 * live references => it only records what is owed. A `noArena` frame emits no prologue (see
 * below), so the TLS state must be excluded -- otherwise a trigger point would reference an
 * `__extc_tlsOwner` that does not exist. A coroutine step does not take part in the TLS state
 * (same reason), but its block arena belongs to the frame itself (see `rcFrameOwnsArena`).
 *
 * @param g    The generator.
 * @param lvl  `ARENA_HOME` (-1) or a block level of 1 and above.
 * @return     `RC_OWN_NO` / `RC_OWN_YES` / `RC_OWN_TLS`.
 */
static RcOwn rcOwnKind(CG *g, int lvl) {
    /* The prologue is only emitted in a frame with `parTls && !noArena && !coroFunc` (see
     * `genFunc`) => the same test here, or a trigger point would reference an `__extc_tlsOwner`
     * that does not exist in this frame. */
    if (g->parTls) return (g->noArena || g->coroFunc) ? RC_OWN_NO : RC_OWN_TLS;
    return rcFrameOwnsArena(g, lvl) ? RC_OWN_YES : RC_OWN_NO;
}

/** @brief Record one rc site (diagnostic `EXTC_DBG_RCCOST=1`; statistics only, never changes
 *         what is emitted).
 *
 * @param g     The generator.
 * @param lvl   Block level (= region); a negative value is merged into 0.
 * @param kind  0 = reference store, 1 = allocation.
 */
static void dbgRcSite(CG *g, int lvl, int kind) {
    if (!g->dbgCost) return;
    int i = lvl < 0 ? 0 : (lvl > 71 ? 71 : lvl);
    if (kind == 0) { g->dbgRc[i].nRef++;   if (g->loopLen > 0) g->dbgRc[i].nRefLoop++; }
    else           { g->dbgRc[i].nAlloc++; if (g->loopLen > 0) g->dbgRc[i].nAllocLoop++; }
}

/** @brief Print the per-level rc site table to stderr (diagnostic `EXTC_DBG_RCCOST=1`; read
 *         only, never changes what is emitted).
 *
 * Purpose: statically price whether a region should be counted or swept. `nRefLoop/nAllocLoop`
 * are the per-iteration densities under the same loop nesting (execution counts cancel out), and
 * `rootSlotsMax` is the largest root-table slot count at a trigger point = the marking start-up
 * size.
 *
 * @param g  The generator.
 */
static void dbgRcCostReport(CG *g) {
    if (!g->dbgCost) return;
    fprintf(stderr, "[rccost] unit needReclaim=%d noCount=%d sweepFree=%d shards=%d\n",
            g->rc.reclaim, g->rc.noCount, g->rc.sweepFree, g->rc.nShard > 0 ? g->rc.nShard : 1);
    fprintf(stderr, "[rccost] %-4s %8s %10s %8s %12s %9s\n",
            "lvl", "ref", "refInLoop", "alloc", "allocInLoop", "rootMax");
    int64_t tr = 0, trl = 0, ta = 0, tal = 0, trm = 0;
    for (int i = 0; i < 72; i++) {
        if (!g->dbgRc[i].nRef && !g->dbgRc[i].nAlloc && !g->dbgRc[i].rootSlotsMax) continue;
        fprintf(stderr, "[rccost] %-4d %8lld %10lld %8lld %12lld %9lld\n", i,
                (long long)g->dbgRc[i].nRef, (long long)g->dbgRc[i].nRefLoop,
                (long long)g->dbgRc[i].nAlloc, (long long)g->dbgRc[i].nAllocLoop,
                (long long)g->dbgRc[i].rootSlotsMax);
        tr += g->dbgRc[i].nRef;   trl += g->dbgRc[i].nRefLoop;
        ta += g->dbgRc[i].nAlloc; tal += g->dbgRc[i].nAllocLoop;
        if (g->dbgRc[i].rootSlotsMax > trm) trm = g->dbgRc[i].rootSlotsMax;
    }
    fprintf(stderr, "[rccost] TOTAL ref=%lld refInLoop=%lld alloc=%lld allocInLoop=%lld rootMax=%lld\n",
            (long long)tr, (long long)trl, (long long)ta, (long long)tal, (long long)trm);
}

static size_t rcRootTableTextSkip(CG *g, Buf *b, size_t skipLocal);

/**
 * @brief Assemble the current static root table (global reference slots + this frame's local and
 *        parameter reference slots) into one compound literal.
 *
 * The table is just the **reference-slot expressions live at this moment**, materialized on the
 * spot by value: `(void *[]){ ... }`. With `n == 0` it is a null pointer constant -- the runtime's
 * seeding loop only looks at `nroots` and does not need an empty table.
 *
 * @param g  The generator.
 * @param b  The buffer the text is appended to.
 * @return   The root count (the number of slots in the table).
 */
static size_t rcRootTableText(CG *g, Buf *b) {
    return rcRootTableTextSkip(g, b, (size_t)-1);
}

/**
 * @brief Like `rcRootTableText`, but **skips slot `skipLocal` of this frame's root table**.
 *
 * The point release of an overwrite point wants the **other** roots: the skipped slot still holds
 * the old value at that moment, so counting it in the seed would prove the old structure live and
 * release nothing. Excluding that slot is what "overwriting it = judging it dead" means.
 *
 * @param g          The generator.
 * @param b          The buffer the text is appended to.
 * @param skipLocal  The slot of `g->rcRoots` to skip; `(size_t)-1` skips none.
 * @return           The number of slots actually written into the table.
 */
static size_t rcRootTableTextSkip(CG *g, Buf *b, size_t skipLocal) {
    size_t n = g->rcGlobalRoots.len + g->rcRoots.len;
    if (g->dbgCost) {
        int i = g->blkLevel < 0 ? 0 : (g->blkLevel > 71 ? 71 : g->blkLevel);
        if ((int64_t)n > g->dbgRc[i].rootSlotsMax) g->dbgRc[i].rootSlotsMax = (int64_t)n;
    }
    if (n == 0) {
        bufPuts(b, "(void *const *)0");
        return 0;
    }    bufPuts(b, "(void *[]){ ");
    size_t k = 0;
    for (size_t i = 0; i < g->rcGlobalRoots.len; i++)
        bufPrintf(b, "%s(void *)(%s)", k++ ? ", " : "",
                  *(const char **)vecAt(&g->rcGlobalRoots, i));
    for (size_t i = 0; i < g->rcRoots.len; i++) {
        if (i == skipLocal) continue;
        bufPrintf(b, "%s(void *)(%s)", k++ ? ", " : "",
                  *(const char **)vecAt(&g->rcRoots, i));
    }
    bufPuts(b, " }");
    return k;
}

/**
 * @brief The bookkeeping for when this frame **does not own** the region: record "one collection
 *        owed" on it (subset B).
 *
 * The reason (see `rcFrameOwnsArena` and "third safety direction" in the block comment above):
 * the home arena is the same one handed down from the caller, and this frame's static root table
 * cannot see the live references of the other frames on the call stack => sweeping such a region
 * as usual frees live objects as garbage (the C hole). So an inner frame does not sweep and only
 * records the debt; **the frame that owns the region** settles it with its own static root table
 * while it has no active callee subtree (`rcEmitOwedCheck`).
 *
 * The bookkeeping itself carries the pressure test (`extc_rc_shouldSweep`): an allocation below
 * the pressure line does not even record the debt, so the owner's check is "read one bit" and
 * does not materialize a root table at every call site.
 *
 * Ownership of the thread-local arena (`parTls`) is only known at run time: a whole worker call
 * chain shares `extc_tls_arena`, the worker entry frame (the one that reads 0 from
 * `extc_rc_tlsDepth`) owns it and an inner frame records an owed collection (`RC_OWN_TLS`). This
 * function is therefore the body of the bookkeeping **without a parTls guard**; the non-TLS entry
 * `rcEmitOwe` is the one that guards.
 *
 * @param g        The generator.
 * @param basePtr  The region's base address expression (in `(void *)` form).
 * @param lvl      The block level (`ARENA_HOME` or 1 and above); with sharding the region number
 *                 is `lvl * shard count + k`.
 */
static void rcEmitOweNow(CG *g, const char *basePtr, int lvl) {
    if (g->rc.nShard <= 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (extc_rc_shouldSweep(%s)) extc_rc_owe(%s); }", rr, rr);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *__extc_rcw = extc_rc_regionOf(%s, %d);"
                  " if (extc_rc_shouldSweep(__extc_rcw)) extc_rc_owe(__extc_rcw); }",
               basePtr, lvl);
        return;
    }
    /* Sharding: the region key is "base address + k". This allocation can only land in one of the
     * shards, but every shard is asked here (marking several is harmless, the owner settles per
     * shard) -- no new sharding state is introduced. */
    pfLine(g, "{ for (int32_t __extc_rcwk = 0; __extc_rcwk < %d; __extc_rcwk++) {"
              " ExtcRcRegion *__extc_rcw = extc_rc_regionOf((void *)((char *)(%s) + __extc_rcwk),"
              " (int32_t)(%d + __extc_rcwk));"
              " if (extc_rc_shouldSweep(__extc_rcw)) extc_rc_owe(__extc_rcw); } }",
           g->rc.nShard, basePtr, lvl * g->rc.nShard);
}

/** @brief Entry point for the bookkeeping when the region is not owned; the TLS state does not
 *         take this path (see the three states of `rcEmitTrigger`). */
static void rcEmitOwe(CG *g, const char *basePtr, int lvl) {
    if (g->parTls) return;   /* TLS ownership is left to the run-time branch of `RC_OWN_TLS` */
    rcEmitOweNow(g, basePtr, lvl);
}

/**
 * @brief The owner frame's settlement point: collect the sweep owed on the outer region with its
 *        **own static root table at this moment** (subset B).
 *
 * The soundness invariant (the whole justification of B): **when a collection happens the
 * collector must have no active callee subtree** -- at that moment its static root table (this
 * frame's locals + parameters + globals) is a complete seed. The check is therefore only emitted
 * **before a call** where this frame owns the arena and really passes it out as the callee's home
 * arena: the previous batch of callees has returned, this batch has not been entered and no deep
 * frame is around. => A frame-level "roots unreliable" marker is **not** needed (a whole runtime
 * root stack would need one).
 *
 * At the vast majority of call sites where the owed flag is false this costs one `regionOf`
 * (a table hit) plus one bit read; the root table is an argument of `extc_rc_checkOwed` and is
 * only materialized when `extc_rc_owed` is true (the same rule as the entry guard of a trigger
 * point).
 *
 * @param g        The generator.
 * @param basePtr  The base address expression of the region this frame owns (in `(void *)`
 *                 form).
 * @param lvl      The block level (`main`'s home arena is `ARENA_HOME`, everything else is 1 and
 *                 above).
 * @param own      The trigger route (with `RC_OWN_TLS` a run-time ownership test
 *                 `if (__extc_tlsOwner && ...)` wraps the whole settlement point: an inner
 *                 frame's root table is incomplete and cannot settle).
 */
static void rcEmitOwedCheck(CG *g, const char *basePtr, int lvl, RcOwn own) {
    if (!g->rc.reclaim || g->rc.rootsUnreliable || g->prefixBlk == 0) return;
    if (g->rcGlobalRoots.len + g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; }
    Buf b;
    bufInit(&b, g->arena);
    size_t n = rcRootTableText(g, &b);
    const char *ownCond = own == RC_OWN_TLS ? "__extc_tlsOwner && " : "";
    if (g->rc.nShard <= 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (%sextc_rc_owed(%s)) extc_rc_checkOwed(%s, %s, %zu); }",
                   ownCond, rr, rr, bufCstr(&b), n);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *__extc_rcc = extc_rc_regionOf(%s, %d);"
                  " if (%sextc_rc_owed(__extc_rcc)) extc_rc_checkOwed(__extc_rcc, %s, %zu); }",
               basePtr, lvl, ownCond, bufCstr(&b), n);
        return;
    }
    pfLine(g, "{ for (int32_t __extc_rcck = 0; __extc_rcck < %d; __extc_rcck++) {"
              " ExtcRcRegion *__extc_rcc = extc_rc_regionOf((void *)((char *)(%s) + __extc_rcck),"
              " (int32_t)(%d + __extc_rcck));"
              " if (%sextc_rc_owed(__extc_rcc)) extc_rc_checkOwed(__extc_rcc, %s, %zu); } }",
           g->rc.nShard, basePtr, lvl * g->rc.nShard, ownCond, bufCstr(&b), n);
}

/**
 * @brief Emit a sweep trigger at an allocation point: `extc_rc_maybeCollect(region, roots, nroots)`.
 *
 * The pressure test and the self-learning exit live at run time (reclaim.c); this function only
 * **calls with the right arguments at the right moment**: the region is the one this allocation
 * lands in (the same claim key as `extc_rc_alloc`) and the root table is the reference slots live
 * at this moment. The trigger goes into the **statement prefix**, so it lands before the
 * statement containing the allocation -- the new object does not exist yet and needs no root.
 *
 * **Entry guard**: the root table is a call argument, and C semantics require materializing the
 * whole table (about 1412 entries in this shape) before the call; but the first statement of
 * `maybeCollect` is `if (!extc_rc_shouldSweep(r)) return -1;` -- the vast majority of allocation
 * points never reach the pressure line. So the test is lifted in front of the call site: the root
 * table is materialized and the call made **only when a sweep is due**. This changes when the
 * root table is materialized, not when collection happens: there is one copy of the test
 * (`extc_rc_shouldSweep`, the very function `maybeCollect` uses inside) and none is duplicated.
 *
 * **Only the "this frame owns it" side is taken here**: when that is false the path becomes the
 * owed bookkeeping (`rcEmitOwe`) -- the root table cannot see the calling frames' live
 * references, and sweeping such a region as usual frees live objects as garbage. The three-state
 * dispatch is in `rcEmitTrigger`.
 *
 * @param g          The generator.
 * @param basePtr    The **base address expression** of the region this allocation lands in
 *                   (already in `(void *)` form), such as `(void *)&(__extc_a[1])` or
 *                   `(void *)__owc0->home`; the claim key of shard k is
 *                   `(char *)basePtr + k` (compared only, never read -- see `rcAllocCore`).
 * @param lvl        The block level (1 = function body, -1 = home region); with sharding the
 *                   region number is `lvl * shard count + k`.
 */
static void rcEmitTriggerNow(CG *g, const char *basePtr, int lvl) {
    size_t n = g->rcGlobalRoots.len + g->rcRoots.len;
    if (n > 4096) { g->rc.rootsUnreliable = true; return; }
    Buf b;
    bufInit(&b, g->arena);
    n = rcRootTableText(g, &b);
    const char *rg = arenaPrintf(g->arena, "__extc_rcg%d", g->rcGuardSeq++);
    if (g->rc.nShard <= 1) {
        /* Single shard: byte-identical to the non-sharded wiring (the claim key is the region base
     * address). */
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) {
            pfLine(g, "{ if (extc_rc_shouldSweep(%s)) extc_rc_maybeCollect(%s, %s, %zu); }",
                   rr, rr, bufCstr(&b), n);
            return;
        }
        pfLine(g, "{ ExtcRcRegion *%s = extc_rc_regionOf(%s, %d);"
                  " if (extc_rc_shouldSweep(%s)) extc_rc_maybeCollect(%s, %s, %zu); }",
               rg, basePtr, lvl, rg, rg, bufCstr(&b), n);
        return;
    }
    /* Sharding: the region key is "base address + k" (compared only, never read). One trigger
     * collects **one shard**, the one with the **most garbage** (`bytes - live`, the same approach
     * as the prototype): once it is collected its `bytes` drops, so the next trigger picks another
     * shard -- no extra rotation state is needed and no shard can starve (always taking the first
     * would let shard 0 monopolize).
     *
     * The memory pressure floor is **divided per shard** (`EXTC_RC_FLOOR / N`, the same rule as the
     * prototype's `TRIGGER / NSHARD`): without the division N shards each guard the full floor and
     * the total bound becomes N times larger, which would let the pause knob change the memory
     * bound as a side effect; divided, the total stays `EXTC_RC_FLOOR` and more shards only mean
     * more frequent, smaller collections. The floor is set at run time by
     * `extc_rc_regionFloor` (the test stays at run time; only the same floor is divided by the
     * shard count here). The root table is still a call argument and still only materialized when
     * a sweep really happens -- the same rule as the single-shard entry guard. */
    const char *ck = "__extc_rck";
    const char *rb = arenaPrintf(g->arena, "__extc_rcb%d", g->rcGuardSeq);
    const char *rbg = arenaPrintf(g->arena, "__extc_rcbg%d", g->rcGuardSeq);
    /* Garbage bytes of this round's shard: it **must be kept separate** from "the best so far", or
     * `gd > rbg` degenerates into comparing a value with itself and `rbg = gd` into self-assignment
     * => one trigger always collects the first shard above the line, the later shards starve and
     * each grows without bound. */
    const char *gd = arenaPrintf(g->arena, "__extc_rcgd%d", g->rcGuardSeq);
    pfLine(g, "{ ExtcRcRegion *%s = (ExtcRcRegion *)0; int64_t %s = 0;"
              " for (int32_t %s = 0; %s < %d; %s++) {"
              " ExtcRcRegion *%s = extc_rc_regionOf((void *)((char *)(%s) + %s), (int32_t)(%d + %s));"
              " extc_rc_regionFloor(%s, (EXTC_RC_FLOOR) / %d);"
              " if (!extc_rc_shouldSweep(%s)) continue;"
              " int64_t %s = extc_rc_garbageHint(%s);"
              " if (!%s || %s > %s) { %s = %s; %s = %s; } }"
              " if (%s) extc_rc_maybeCollect(%s, %s, %zu); }",
           rb, rbg, ck, ck, g->rc.nShard, ck, rg, basePtr, ck, lvl * g->rc.nShard, ck,
           rg, g->rc.nShard, rg,
           gd, rg, rb, gd, rbg, rb, rg, rbg, gd,
           rb, rb, bufCstr(&b), n);
}

/**
 * @brief Three-state dispatch of a trigger point (see `RcOwn`): owns => sweep now; does not own
 *        => only record the owed collection; TLS => choose at run time between "this is the
 *        worker entry frame" and not.
 *
 * In the TLS state the root table is only materialized in the owner branch (the compound literal
 * is an argument of `rcEmitTriggerNow`, which sits entirely inside `if (__extc_tlsOwner)`) => an
 * inner frame does not pay for a second copy of the root table.
 *
 * @param g        The generator.
 * @param basePtr  The region's base address expression (as in `rcEmitTriggerNow`).
 * @param lvl      The block level (as in `rcEmitTriggerNow`).
 * @param own      The trigger route.
 */
static void rcEmitTrigger(CG *g, const char *basePtr, int lvl, RcOwn own) {
    if (!g->rc.reclaim || g->rc.rootsUnreliable || g->prefixBlk == 0) return;
    if (own == RC_OWN_YES) { rcEmitTriggerNow(g, basePtr, lvl); return; }
    if (own == RC_OWN_NO) {
        /* The other half of ownership delegation (`homeOwnArg`): this frame does not own the home arena,
         * but the caller's flag may say "the owner holds no live reference slot right now, your own
         * root table is a complete seed". The root table is only materialized in the owner branch --
         * the compound literal is an argument of `rcEmitTriggerNow` and stays inside the `if`. */
        if (lvl == ARENA_HOME && g->rcFuncUsesHome && !g->parTls && !g->coroFunc) {
            pfLine(g, "if (__extc_homeOwn) {");
            g->indent++;
            rcEmitTriggerNow(g, basePtr, lvl);
            g->indent--;
            pfLine(g, "} else {");
            g->indent++;
            rcEmitOwe(g, basePtr, lvl);
            g->indent--;
            pfLine(g, "}");
            return;
        }
        rcEmitOwe(g, basePtr, lvl);
        return;
    }
    /* RC_OWN_TLS: the whole worker call chain shares one region, so ownership is a run-time
     * matter. */
    pfLine(g, "if (__extc_tlsOwner) {");
    g->indent++;
    rcEmitTriggerNow(g, basePtr, lvl);
    g->indent--;
    pfLine(g, "} else {");
    g->indent++;
    rcEmitOweNow(g, basePtr, lvl);
    g->indent--;
    pfLine(g, "}");
}

/**
 * @brief Emit one rc allocation: the shard index rotates per **allocation point**, and the text
 *        of the `extc_rc_alloc(...)` call is returned.
 *
 * Shard index = `rcSite++ % rcNShard`: the same `new` always lands in the same shard, so chains
 * and cycles inside a shard stay complete; references between shards are handled by the existing
 * cross-region registration (`rsInc`/`rsDec` of `storeRefOwned`), whose bit sits on the `rsbit`
 * of the **target's** shard -- seeding and sweeping never cross a shard.
 *
 * Trigger and allocation use the **same** shard key: the pressure is asked once with this shard's
 * key (the trigger lands in the statement prefix) and the real allocation then takes its slot
 * from the same shard. With N == 1 the returned text is byte-identical to the non-sharded
 * wiring.
 *
 * @param g          The generator.
 * @param basePtr    The region's base address expression (as in `rcEmitTrigger`).
 * @param lvl        The block level (may be -1; only used for the region number).
 * @param own        The trigger route (as in `rcEmitTrigger`; `RC_OWN_NO` => no sweep, only the
 *                   owed bookkeeping).
 * @param bytesExpr  The C text of the byte count (spliced into the call as it is, with no extra
 *                   parentheses).
 * @param td         The C text of the type descriptor.
 * @return           The C text of the `extc_rc_alloc(...)` call.
 */
static const char *rcAllocCore(CG *g, const char *basePtr, int lvl, RcOwn own,
                               const char *bytesExpr, const char *td) {
    dbgRcSite(g, lvl, 1);
    int nsh = g->rc.nShard > 0 ? g->rc.nShard : 1;
    int k = g->rcSite++ % nsh;
    rcEmitTrigger(g, basePtr, lvl, own);
    if (nsh == 1) {
        const char *rr = rcRegionRef(g, basePtr);
        if (rr) return arenaPrintf(g->arena, "extc_rc_alloc(%s, %s, %s)", rr, bytesExpr, td);
        return arenaPrintf(g->arena,
            "extc_rc_alloc(extc_rc_regionOf(%s, %d), %s, %s)",
            basePtr, lvl, bytesExpr, td);
    }
    return arenaPrintf(g->arena,
        "extc_rc_alloc(extc_rc_regionOf((void *)((char *)(%s) + %d), %d), %s, %s)",
        basePtr, k, lvl * nsh + k, bytesExpr, td);
}

/**
 * @brief Register every reference leaf slot of one storage as a root.
 *
 * Arrays are expanded per element and nested structs are followed down (`a[i].f`). A view is not
 * registered (its `data` is not an rc slot). Being slow or deep is not a concern: this runs once,
 * at the **declaration**, and only while the switch is on.
 *
 * @param g      The generator; registrations are appended to `g->rcRoots`.
 * @param t      The storage's type.
 * @param base   The storage's lvalue expression in C (such as `saved` or `h.c`).
 * @param depth  Recursion depth, guarding against cycles.
 */
static void rcRootLeaves(CG *g, Type *t, const char *base, int depth) {
    if (!t || depth > 16) return;
    if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; }
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(&g->rcRoots) = base;
        return;
    case TY_ENUM: {
        /* An enum payload: the reference leaves of **all** variants are registered (no pick by run-time
         * tag). Reading `u.<v>._<k>` of an inactive variant yields garbage, and a root-table entry
         * goes through `extc_rc_owns` in `markBegin` => a wrong guess retains more, never less. So a
         * local whose leaf slots cannot be enumerated (`var m: maybe = ...`) can be a root too, and
         * the storage side does not have to take the target region out of sweeping (see
         * `extc_rc_storeStructRooted`). */
        if (!t->edef) return;
        for (size_t i = 0; i < t->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&t->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; }
                /* A generic enum instance must substitute its arguments into the payload types (the same
     * technique as the binding in `stMatch`). */
                if (t->edef->typeParams.len > 0 &&
                    t->targs.len == t->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &t->edef->typeParams, &t->targs);
                rcRootLeaves(g, pt,
                             arenaPrintf(g->arena, "%s.u.%s._%zu", base, v->name, k),
                             depth + 1);
            }
        }
        return;
    }
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        for (int64_t i = 0; i < t->asize; i++) {
            if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; }
            /* An array is a wrapping C struct: its elements are written as `arr.data[i]`. */
            rcRootLeaves(g, e, arenaPrintf(g->arena, "%s.data[%lld]", base, (long long)i), depth + 1);
        }
        return;
    }
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcRootLeaves(g, fd->type,
                         arenaPrintf(g->arena, "%s.%s", base, fd->name), depth + 1);
        }
        return;
    case TY_GENERIC:
        /* A view's `data` word points at the block that holds its elements. For `new T[n]` on the
         * reclaiming path that block is an rc slot, and this root is what keeps it alive; a pointer
         * into an arena, a plate or the stack fails `extc_rc_owns` in the marker, so a view whose
         * storage is not rc contributes nothing. */
        if (isView(t)) {
            *(const char **)vecPush(&g->rcRoots) = arenaPrintf(g->arena, "%s.data", base);
            return;
        }
        if (!t->sdef) return;
        SubCtx sv = substEnter(g, t);
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcRootLeaves(g, fd->type,
                         arenaPrintf(g->arena, "%s.%s", base, fd->name), depth + 1);
        }
        substLeave(g, sv);
        return;
    /* kind-default: roots only recurse through the REF / ARRAY / STRUCT / GENERIC shapes; the other
     * kinds have no reference slots to register. A new kind that contains reference slots must add
     * a case here and must not slide through the default. */
    default:
        return;
    }
}

/**
 * @brief Collect the **C lvalue paths** of a type's reference leaf slots (projected along
 *        values: fields / inline array elements).
 *
 * Same recursive shape as `rcRootLeaves`; only the exit differs: there the leaf is registered as
 * a **root**, here the leaf's path is handed to the caller -- **whole-value storage**
 * (`s.w = wrap { n: keeper }`, `var b: wrap = ...`) has to run the reference-slot bookkeeping
 * (`initRef` / `storeRefOwned` / `storeRefSlot`) leaf by leaf. A bare block copy neither `inc`s
 * nor registers a cross-region edge: the slot is written with no bookkeeping, so the object is
 * freed when its count reaches zero or the region is swept while the slot still points at it.
 *
 * `base` must be an lvalue that is **safe to evaluate repeatedly** (the caller first evaluates
 * target and value once into a temporary / `(*p)`): the path splices it into every bookkeeping
 * call.
 *
 * @param g      The generator.
 * @param t      The type.
 * @param base   The value's lvalue expression in C (such as `(*__extc_svd0)` or `b`); an empty
 *               string produces **relative paths** (`.f`, `.kids.data[0]`) and lets the caller
 *               splice both bases itself -- one leaf set can then be used for target and value at
 *               once, without collecting either twice or having a leaf on one side only.
 * @param out    A `const char *` vector that receives the path of every reference leaf.
 * @param depth  Recursion depth, guarding against type cycles.
 */
static void rcLeafPaths(CG *g, Type *t, const char *base, Vec *out, int depth) {
    if (!t || depth > 16 || out->len > 4096) return;
    t = subst(g, t);
    if (!t) return;
    switch (t->kind) {
    case TY_REF:
        *(const char **)vecPush(out) = base;
        return;
    case TY_ARRAY: {
        Type *e = subst(g, t->inner);
        /* A fixed-length array is a wrapping struct in C: its elements are written as `arr.data[i]`
     * (the same as `rcRootLeaves`). */
        for (int64_t i = 0; i < t->asize && out->len <= 4096; i++)
            rcLeafPaths(g, e, arenaPrintf(g->arena, "%s.data[%lld]", base, (long long)i),
                        out, depth + 1);
        return;
    }
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcLeafPaths(g, fd->type, arenaPrintf(g->arena, "%s.%s", base, fd->name),
                        out, depth + 1);
        }
        return;
    case TY_GENERIC:
        if (isView(t) || !t->sdef) return;
        SubCtx sv = substEnter(g, t);
        for (size_t i = 0; i < t->sdef->fields.len && out->len <= 4096; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            rcLeafPaths(g, fd->type, arenaPrintf(g->arena, "%s.%s", base, fd->name),
                        out, depth + 1);
        }
        substLeave(g, sv);
        return;
    /* kind-default: only the kinds listed above take part in expanding reference leaves; the other
     * kinds are classified by `rcScanType` as "no references" or "not enumerable" and never reach
     * here. */
    default:
        return;
    }
}

/**
 * @brief The class of conservative types whose leaves are in fact enumerable: their references
 *        are hidden only in **enum payloads**.
 *
 * `rcScanType` returns `RC_CONSERVATIVE` for every TY_ENUM because it would have to pick one
 * branch by run-time tag; but "do not trust the root table" is not the only way out -- registering
 * the payload leaves of **all** variants as roots, with garbage from inactive variants filtered by
 * `extc_rc_owns`, errs on the retaining side (the same as the TY_ENUM branch of `rcRootLeaves`).
 * A local can then be a root, trigger points are emitted as usual, and the storage side does not
 * has to take the target region out of sweeping -- that is what closes the gap left by
 * "a `match` binding a reference payload".
 *
 * Only conservative types whose **shape really is enumerable** count: TY_ENUM alone is
 * "reading an inactive variant is harmless". A generic instance whose arguments carry references
 * (elements live on the container's own plate and the field scan cannot see them) and any kind
 * added later return false and still do not trust the root table.
 *
 * @param g        The generator.
 * @param t        The type to check.
 * @param depth    Recursion depth, guarding against cycles.
 * @param hasRefs  Output: whether this type has reference leaves at all. When every payload is an
 *                 enum without references (`var x: option<i64>`) there is no root to register and
 *                 returning true would be pointless -- the caller uses this output to leave that
 *                 class on the conservative path (no useless roots).
 * @return         true = every reference leaf can be statically expanded into a root, variant by
 *                 variant.
 */
static bool rcEnumRootable(CG *g, Type *t, int depth, bool *hasRefs) {
    if (!t || depth > 16) return false;
    t = subst(g, t);
    if (!t) return false;
    switch (t->kind) {
    case TY_REF:
        *hasRefs = true;
        return true;
    case TY_VOID: case TY_BUILTIN: case TY_FN: case TY_DYN:
    case TY_PARAM: case TY_ERROR: case TY_UNRESOLVED:
        return true;
    case TY_ENUM:
        if (!t->edef) return false;
        for (size_t i = 0; i < t->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&t->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (t->edef->typeParams.len > 0 &&
                    t->targs.len == t->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &t->edef->typeParams, &t->targs);
                if (!rcEnumRootable(g, pt, depth + 1, hasRefs)) return false;
            }
        }
        return true;
    case TY_ARRAY:
        return rcEnumRootable(g, t->inner, depth + 1, hasRefs);
    case TY_STRUCT:
        for (size_t i = 0; i < t->sdef->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            if (!rcEnumRootable(g, fd->type, depth + 1, hasRefs)) return false;
        }
        return true;
    case TY_GENERIC: {
        /* A view is a root word in its own right: its `data` pointer may be the only edge to a
         * `new T[n]` block, so the storage that holds the view must be registered even though the
         * view has no reference slots (`rcScanType` answers `RC_NO_REFS` for it). */
        if (isView(t)) {
            *hasRefs = true;
            return true;
        }
        if (!t->sdef) return false;
        for (size_t i = 0; i < t->targs.len; i++) {
            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS)
                return false;
        }
        SubCtx sv = substEnter(g, t);
        bool ok = true;
        for (size_t i = 0; i < t->sdef->fields.len && ok; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
            ok = rcEnumRootable(g, fd->type, depth + 1, hasRefs);
        }
        substLeave(g, sv);
        return ok;
    }
    /* kind-default: aligned with the default branch of `rcScanType` -- anything unrecognized counts
     * as not enumerable. */
    default:
        return false;
    }
}

/**
 * @brief Register a storage's reference roots when it is declared; when they cannot be
 *        enumerated, turn off every trigger in this function.
 *
 * @param g     The generator.
 * @param t     The declared type.
 * @param base  The storage's lvalue expression in C.
 */
static void rcRootDeclImpl(CG *g, Type *t, const char *base, bool isParam) {
    if (!g->rc.reclaim || !t) return;
    /* A view slot is a root in its own right: its `data` word may point at an rc block taken by
     * `new T[n]`, and `rcScanType` reports a view as having no reference slots (the elements live
     * behind the pointer, not in the C struct). */
    if (isView(t)) {
        /* A *parameter* view needs no root of its own: the caller's copy of the same view is what
         * keeps the block alive, and the caller's root table is the one a delegated sweep uses. Only
         * a local view can be the last word naming an rc block. */
        if (isParam) return;
        rcRootLeaves(g, t, base, 0);
        return;
    }
    RcScan sc = rcScanType(g, t, 0);
    if (sc == RC_CONSERVATIVE) {
        bool hasRefs = false;
        if (rcEnumRootable(g, t, 0, &hasRefs) && hasRefs) {
            rcRootLeaves(g, t, base, 0);
            return;
        }
        g->rc.rootsUnreliable = true;
        return;
    }
    if (sc == RC_NO_REFS) {
        /* No reference slots -- but the storage may still hold a view, whose `data` word is a root
         * (`new T[n]` blocks hang off it). Register those and nothing else. */
        bool hasRoots = false;
        if (rcEnumRootable(g, t, 0, &hasRoots) && hasRoots) rcRootLeaves(g, t, base, 0);
        return;
    }
    rcRootLeaves(g, t, base, 0);
}

/**
 * @brief Register the reference roots that live in one declaration.
 *
 * @param g        The generator.
 * @param t        Declared type; NULL is a no-op.
 * @param base     C spelling of the declared object.
 * @param isParam  True for a parameter declaration, whose view needs no root of its own.
 */
static void rcRootDecl(CG *g, Type *t, const char *base) {
    rcRootDeclImpl(g, t, base, false);
}

/**
 * @brief Generate `EX_NEW`: allocate at the level the checker computed.
 *
 * The level may have been promoted because the value is stored somewhere longer-lived; this is
 * where memory with automatic reclamation is really taken. With a count a `slice<T>` view is
 * generated and the count is evaluated once: an impure count is marked `needTemp` by the checker
 * and stored into a temporary first.
 *
 * The `EX_NEW` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_NEW` node.
 * @return   The C text of the allocation expression or of the view literal.
 */
static const char *exNew(CG *g, Expr *e) {
    /* The context asked for the **owning** form of this buffer (`@buffer`), so the initializer is
     * that type's constructor with the element count: `buf<T>::withCap(n)`, named through the same
     * instance decoration methods use. */
    if (planNewAsBuffer(e) && planCallee(e)) {
        Buf nb;
        bufInit(&nb, g->arena);
        bufPuts(&nb, cMethodName(g, planAssocOwner(e), planCallee(e)));
        bufPutc(&nb, '(');
        bufPuts(&nb, genExpr(g, e->u.new_.count));
        cgImplicitArgs(g, &nb, planCallee(e), e, 1, false);
        bufPutc(&nb, ')');
        return bufCstr(&nb);
    }
    Type *w = subst(g, e->u.new_.type);
    /* Allocate at the level the checker computed, which may have been promoted because the value
     * is stored into a place that lives further out. This is where memory with automatic cleanup
     * is taken. */
    if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, e, "new");
    const char *ar = arenaRefAt(g, planArenaLevel(e));
    if (!e->u.new_.count) {
        /* The place of one T (or one [N]T) is simply its address. */
        /* With reclamation on, this storage comes from an **rc region**: `ref T` / `ref [N]T` are
         * reference slots that the counting functions (`extc_rc_initRef` / `storeRef` /
         * `register`) can reach, and counting only knows slab slots -- a pointer that lands in an
         * arena makes `extc_rc_slabOf` read garbage. A region is claimed by the arena's address
         * (see the table in `emitRuntimePrologue`), so its lifetime matches the arena's exactly.
         *
         * `new T[n]` (the next case) produces a `slice<T>` view, which is **not** a reference
         * slot: counting cannot reach its data pointer. The block is an rc slot all the same; the
         * view's `data` word is what keeps it alive, because `rcRootLeaves` registers `<base>.data`
         * for a view slot and the marker filters that word through `extc_rc_owns`. A large block
         * does not run into a slab size limit: `extc_rc_alloc` serves it from a slab of its own
         * size class. */
        if (g->rc.reclaim) {
            int lvl = planArenaLevel(e);
            /* Order: the type descriptor first (it must be visible in this statement), then the trigger
             * (it lands before the statement), and the allocation expression last -- all three use
             * the **same shard index** (see `rcAllocCore`).
             * Trigger route: ownership of the home arena / thread-local arena is decided by
 * `rcOwnKind` (the root table cannot see the calling frame's live references => the side that
 * does not own only records the owed collection). */
            const char *td = rcEmitTypeDesc(g, w);
            const char *base = arenaPrintf(g->arena, "(void *)&(%s)", ar);
            const char *core = rcAllocCore(g, base, lvl, rcOwnKind(g, lvl),
                arenaPrintf(g->arena, "(int64_t)sizeof(%s)", cType(g, w)), td);
            return arenaPrintf(g->arena, "((%s *)%s)", cType(g, w), core);
        }
        return arenaPrintf(g->arena,
            "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), \"%s\", %d))",
            cType(g, w), ar, cType(g, w), g->path, e->line);
    }
    /* `T[n]` becomes a view `{ data, len }`, with the count evaluated
     * exactly once: an impure count is marked `needTemp` by the checker
     * and stored in a temporary first. */
    const char *n;
    if (planNeedTemp(e)) {
        const char *tmp = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
        pfLine(g, "int64_t %s = (int64_t)(%s);", tmp, genExpr(g, e->u.new_.count));
        n = tmp;
    } else {
        n = genExpr(g, e->u.new_.count);
    }
    Type *st = subst(g, e->type);      /* the `slice<T>` the checker produced */
    /* Only a block at the **function-body level** goes through the rc region. That level is the one
     * where a replaced buffer has nowhere to go: an inner `{}` releases its arena when the block exits,
     * and `ARENA_HOME` means the *caller* named the release point, so both are bounded without the
     * collector. Routing those would pay for the runtime and reclaim nothing -- and it would pull units
     * that only hand arrays back to their caller out of the idle tier. */
    if (g->rc.reclaim && planArenaLevel(e) == 1) {
        /* The block is an rc slot in the same region the arena would have used; the view's `data`
         * word is registered as a root (see `rcRootLeaves`), which is how the marker reaches it.
         * The reference-field offsets of elements 1..n-1 vary with the run-time count, so a static
         * descriptor cannot express them: use the conservative marker (scan every word) when the
         * element type contains references, and no descriptor otherwise -- the same rule as
         * `alloc<T>(n)` in `exGencall`. */
        int lvl = planArenaLevel(e);
        const char *td = "(const ExtcRcType *)0";
        if (rcScanType(g, w, 0) != RC_NO_REFS) td = "((const ExtcRcType *)-1)";
        const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)&(%s)", ar), lvl,
            rcOwnKind(g, lvl),
            arenaPrintf(g->arena, "(int64_t)(%s) * (int64_t)sizeof(%s)", n, cType(g, w)), td);
        return arenaPrintf(g->arena, "(%s){ .data = (%s *)%s, .len = (int64_t)(%s) }",
            cType(g, st), cType(g, w), core, n);
    }
    return arenaPrintf(g->arena,
        "(%s){ .data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s),"
        " \"%s\", %d), .len = (int64_t)(%s) }",
        cType(g, st), cType(g, w), ar, n, cType(g, w), g->path, e->line, n);
}

/**
 * @brief Generate `EX_GENCALL`: `alloc<T>(n)` and the pool primitive calls.
 *
 * `alloc<T>(n)` asks the arena level the checker chose for room for n Ts and returns a pointer.
 * Zeroing is not this path's job: `extc_arena_alloc` zeroes every allocation, and the language's
 * promise rests on it -- bytes read after a lifetime ended are always initialized bytes.
 * `poolSlice` / `poolResize` / `poolGive` / `copyInto` and their `Raw` variants have the same
 * shape; only the memory belongs to the pool's own plate.
 *
 * The `EX_GENCALL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_GENCALL` node.
 * @return   The C text of the call expression.
 */
static const char *exGencall(CG *g, Expr *e) {
    /* `alloc<T>(n)`: ask the arena of the level the checker chose for the place of n values of T,
     * and hand back a pointer to it.
     *
     * Zeroing is not this path's job: `extc_arena_alloc` zeroes every allocation, and that is what
     * makes the language's promise hold -- a byte read after its lifetime ends is always an
     * initialized byte. It does so for the cheapest sufficient set of bytes: a block obtained from
     * `calloc` is zero throughout, so only a reused block (one the spare handed back) needs the
     * `memset`. */
    /* `poolSlice<T>(rid, n)` / `poolGive<T>(rid, s)`: the same shape, but the memory belongs to
     * the pool's own plate rather than to an arena. `poolGive` is how `grow` hands the replaced
     * buffer back, so a doubling container stops leaving every old generation behind.
     * `poolSliceRaw` / `poolResizeRaw` are the same two calls with the zeroing left out, for a
     * column whose every slot is written before it is read. */
    const char *gcName = e->u.gencall.name;
    if (isPoolPrimitiveName(gcName)) {
        const char *rid = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
        if (strcmp(gcName, "poolGive") == 0) {
            return arenaPrintf(g->arena,
                "extc_pool_give((int64_t)(%s), (void *)(%s).data)",
                rid, genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)));
        }
        const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
        Type *st = subst(g, e->type);      /* the `mut slice<T>` the checker produced */
        /* The count appears twice below (bytes and `.len`), so an impure one was
         * computed into a temporary by the checker. */
        size_t nIdx = (strcmp(gcName, "poolResize") == 0
                       || strcmp(gcName, "poolResizeRaw") == 0) ? 2 : 1;
        const char *n;
        if (planNeedTemp(e)) {
            const char *tmp = arenaPrintf(g->arena, "__extc_pn%d", g->tmpSeq++);
            pfLine(g, "int64_t %s = (int64_t)(%s);", tmp,
                   genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, nIdx)));
            n = tmp;
        } else {
            n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, nIdx));
        }
        if (strcmp(gcName, "copyInto") == 0) {
            /* `copyInto<T>(dst, src, n)`: one checked `memmove`. The view type comes
             * from the DESTINATION argument, not from `e->type` -- this call's type is
             * `i64` (the count it returns), so the slice has to be read off the argument
             * the checker already typed. `@unchecked` names the `..._copyU` variant,
             * which checks neither the count nor the storage. */
            Expr *dArg = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *vt = subst(g, dArg->type);
            return arenaPrintf(g->arena,
                "%s((%s)(%s), (%s)(%s), (int64_t)(%s), \"%s\", %d)",
                viewCopier(g, vt, g->uncheckedBody), cType(g, vt),
                genExpr(g, dArg),
                cType(g, vt),
                genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)),
                genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 2)),
                g->path, e->line);
        }
        if (strcmp(gcName, "poolResize") == 0 || strcmp(gcName, "poolResizeRaw") == 0) {
            return arenaPrintf(g->arena,
                "(%s){ .data = (%s *)extc_pool_%s((int64_t)(%s), (void *)(%s).data,"
                " (int64_t)(%s) * (int64_t)sizeof(%s)), .len = (int64_t)(%s) }",
                cType(g, st), tn,
                strcmp(gcName, "poolResizeRaw") == 0 ? "resize_raw" : "resize",
                rid, genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 1)), n, tn, n);
        }
        return arenaPrintf(g->arena,
            "(%s){ .data = (%s *)extc_pool_%s((int64_t)(%s),"
            " (int64_t)(%s) * (int64_t)sizeof(%s)), .len = (int64_t)(%s) }",
            cType(g, st), tn,
            strcmp(gcName, "poolSliceRaw") == 0 ? "take_raw" : "take",
            rid, n, tn, n);
    }
    const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
    const char *n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
    /* The level comes from the checker as well, with `alloc` meaning
     * the current block, so `g->blkLevel` is not counted here. */
    const char *ar = arenaRefAt(g, planArenaLevel(e));
    /* The `T *` returned by `alloc<T>(n)` is a reference type and is caught by reference slots =>
     * with reclamation on it must be a slab slot too, for exactly the reason given at `exNew` (a
     * region is claimed by the arena's address). The pool/plate cases want the pool's own plate
     * and are not this path's concern. */
    if (g->rc.reclaim) {
        int lvl = planArenaLevel(e);
        Type *et = subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0));
        /* A block of `n` elements: the reference-field offsets of elements 1..n-1 vary with the
         * run-time `n`, which a static descriptor cannot express => use the conservative marker
         * (scan every word of the block) when the element type contains references, and no
         * descriptor otherwise. */
        const char *td = "(const ExtcRcType *)0";
        if (rcScanType(g, et, 0) != RC_NO_REFS) td = "((const ExtcRcType *)-1)";
        const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)&(%s)", ar), lvl,
            rcOwnKind(g, lvl),
            arenaPrintf(g->arena, "(int64_t)(%s) * (int64_t)sizeof(%s)", n, tn), td);
        return arenaPrintf(g->arena, "(%s *)%s", tn, core);
    }
    return arenaPrintf(g->arena,
        "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d)",
        tn, ar, n, tn, g->path, e->line);
}

/**
 * @brief Generate `EX_COALESCE`: the `??` null test and fallback value.
 *
 * When the subject is impure (`f() ?? -1`), it is evaluated into a temporary before the test so a
 * side effect cannot happen twice. The fallback side is explicitly cast to the result type because
 * C's `?:` applies the usual arithmetic conversions to both arms (see the function body).
 *
 * The `EX_COALESCE` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_COALESCE` node.
 * @return   The C text of the conditional expression.
 */
static const char *exCoalesce(CG *g, Expr *e) {
    Type *mt = e->u.coalesce.main->type;
    const char *m;
    if (planNeedTemp(e)) {
        /* An impure subject (`f() ?? -1`) is computed once into a temporary, and the conditional
         * then reads that variable, so nothing is evaluated twice. `flushPrefix` emits these lines
         * before the enclosing statement. */
        const char *tmp = arenaPrintf(g->arena, "__extc_c%d", g->tmpSeq++);
        pfLine(g, "%s %s = %s;", cType(g, mt), tmp, genExpr(g, e->u.coalesce.main));
        m = tmp;
    } else {
        m = genExpr(g, e->u.coalesce.main);
    }
    const char *fb = genExpr(g, e->u.coalesce.fallback);

    /* The fallback side is cast to the result type explicitly, because C's `?:` applies the usual
     * arithmetic conversions to its two arms: an `int64_t` and an `int` together can silently widen
     * the result, an f32 payload next to a double literal being the typical case. The checker
     * already guarantees that the fallback fits the result type, so this only makes the intended
     * type visible in the generated C.
     *
     * Only a builtin scalar is cast this way: C cannot cast to an array type, so
     * `option<[3]i32> ?? arr` would stop compiling.
     *
     * The cast goes to the result type, the payload T, not to `option<T>` itself. */
    Type *rt = mt;
    if (mt && mt->kind != TY_REF && mt->targs.len > 0)
        rt = *(Type **)vecAt(&mt->targs, 0);
    bool scalar = rt && (rt->kind == TY_BUILTIN || rt->kind == TY_REF);
    const char *rhs = scalar
        ? arenaPrintf(g->arena, "((%s)(%s))", cType(g, rt), fb) : fb;

    if (mt && mt->kind == TY_REF) {
        /* A `?ref T` is a plain pointer in C, so a null test is enough. */
        return arenaPrintf(g->arena, "((%s) != ((void *)0) ? (%s) : %s)", m, m, rhs);
    }
    bool isOpt = isProtoType(mt, "option", 1);
    const char *tag = isOpt ? "some" : "success";
    return arenaPrintf(g->arena, "((%s).tag == %s_%s ? (%s).u.%s._0 : %s)",
                       m, cType(g, mt), tag, m, tag, rhs);
}

/**
 * @brief Generate `EX_ENUMVAL`: constructing an enum variant value.
 *
 * An enum without payloads is a C enum and the variant itself is a constant; an enum with
 * payloads is `struct { tag; union }` in C, so even a payloadless variant is constructed, and a
 * payload variant fills `._0`, `._1`, ... in the union.
 *
 * The type name must account for a generic instance: the C name of an instantiated enum
 * (`maybe_i64`) is not in the type table, so the type the checker resolved, recorded in
 * `assocOwner`, is preferred.
 *
 * The `EX_ENUMVAL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_ENUMVAL` node.
 * @return   The C text of the enum constant or of the construction expression.
 */
static const char *exEnumval(CG *g, Expr *e) {
    const char *tn = e->u.enumval.typeName;
    const char *vn = e->u.enumval.variant;
    /* The instantiated name of a generic enum (`maybe_i64`) is not in
     * the type table, so the type the checker resolved, recorded in
     * `assocOwner`, is preferred. */
    Type *et = planAssocOwner(e);
    if (!et && g->tt) et = ttFromName(g->tt, tn);
    /* Inside a generic instance `tn` is the template's name (`option_T`), while the instance's C
     * name is `option_i32`. With an `assocOwner` present, `subst` plus `cType` gives the real name;
     * without it the emitted name does not exist. */
    if (et) {
        Type *rt = subst(g, et);
        if (rt && rt->name) tn = rt->name;
    }
    bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);

    /* An enum without payloads is a plain C enum, so the variant is
     * itself a constant. */
    if (!payload)
        return arenaPrintf(g->arena, "%s_%s", tn, vn);

    /* An enum with payloads is a `struct { tag; union }` in C, so even
     * a payloadless variant has to be constructed:
     * `(shape){ .tag = shape_dot }`. */
    if (e->u.enumval.args.len == 0)
        return arenaPrintf(g->arena, "(%s){ .tag = %s_%s }", tn, tn, vn);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){ .tag = %s_%s, .u.%s = {", tn, tn, vn, vn);
    for (size_t i = 0; i < e->u.enumval.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, "._%zu = %s", i, genExpr(g, *(Expr **)vecAt(&e->u.enumval.args, i)));
    }
    bufPuts(&b, "} }");
    return bufCstr(&b);
}

