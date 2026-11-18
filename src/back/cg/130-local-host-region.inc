/* ---- local objects whose host region can be given statically (see CG.rcFresh*) --------------- */

/** @brief Statement callback of the assignment scan: note the identifiers that are rebound and
 *         keep walking. */
static bool rcAsgnStmt(void *ctx, Stmt *s);
/** @brief Expression callback of the assignment scan: keep walking. */
static bool rcAsgnExpr(void *ctx, Expr *e);

/**
 * @brief Statement callback: note the identifiers that are **directly** assigned (rebound) and
 *        keep walking.
 *
 * @param ctx  The generator state.
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcAsgnStmt(void *ctx, Stmt *s) {
    CG *g = (CG *)ctx;
    /* The statement kind is tested first: in the union, the pointers of other statement kinds would
     * be misread as assignment targets. */
    Expr *tgt = (s && s->kind == ST_ASSIGN) ? s->u.assign.target : NULL;
    /* Only a **direct** assignment to an identifier counts as a rebinding: `t.f = v`, `a[i] = v`
     * and `*p = v` change a slot inside the object or something elsewhere, not which object this
     * local points to. */
    if (tgt && tgt->kind == EX_IDENT) {
        const char *nm = planCName(tgt) ? planCName(tgt) : tgt->u.ident.name;
        if (nm) *(const char **)vecPush(&g->rcAssigned) = nm;
    }
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/** @brief Expression callback: keep walking (an assignment target can also be nested inside
 *         an expression statement). */
static bool rcAsgnExpr(void *ctx, Expr *e) {
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Scan the function body once: which identifiers are assigned (the complement of "is never
 *        rebound").
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcScanAssigned(CG *g, Stmt *body) {
    vecInit(&g->rcAssigned, g->arena, sizeof(const char *));
    if (!body) return;
    AstVisit v = { rcAsgnExpr, rcAsgnStmt, g };
    astWalkStmtChildren(body, &v);
}

/** @brief Was this name assigned? */
static bool rcIsAssigned(CG *g, const char *nm) {
    for (size_t i = 0; i < g->rcAssigned.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcAssigned, i), nm) == 0) return true;
    return false;
}

/**
 * @brief Register a "freshly allocated and never rebound" local object (`stVar` calls this while
 *        emitting the declaration).
 *
 * The conditions are: the initializer is a **single** `EX_NEW`, reclamation is on (such an
 * allocation always goes through rc), the level is the home or a block level, and the name is
 * never assigned. A coroutine body registers nothing (region locals are not hoisted there).
 *
 * @param g  The generator state.
 * @param s  The `ST_VAR` statement.
 */
static void rcNoteFreshLocal(CG *g, Stmt *s) {
    /* When sharding, **do not emit**: the region key is "base + shard number" and objects at one
     * level land on different shards by allocation point -- the statically given region need not be
     * the one holding the host (`cross_region_elem` peaks at 2.77x with SHARDS=8, so the
     * registration missed it). A coroutine body is the same (region locals are not hoisted). */
    if (!g->rc.reclaim || g->coroFunc || g->rc.nShard > 1) return;
    if (g->rcFreshN >= 64) return;
    Expr *init = s->u.var.init;
    if (!init || init->kind != EX_NEW || init->u.new_.count) return;
    const char *nm = planCName(s);
    if (!nm || rcIsAssigned(g, nm)) return;
    int lvl = planArenaLevel(init);
    if (!(lvl == ARENA_HOME || lvl >= 1)) return;
    g->rcFreshName[g->rcFreshN] = nm;
    g->rcFreshLvl[g->rcFreshN] = lvl;
    g->rcFreshN++;
}

/**
 * @brief When the host expression is such a local object, return its region local's name,
 *        otherwise NULL.
 *
 * The region local was declared at the top of the function body by `rcPredeclare` (which collects
 * it from every `EX_NEW` point); here it is looked up by a **byte-identical** base-address text.
 *
 * @param g     The generator state.
 * @param base  The host object's C expression node.
 * @return      The region local's name, or NULL (fall back to `storeRefOwned`).
 */
static const char *rcFreshHostRegion(CG *g, Expr *base) {
    if (!base || base->kind != EX_IDENT) return NULL;
    const char *nm = planCName(base) ? planCName(base) : base->u.ident.name;
    if (!nm) return NULL;
    for (int i = 0; i < g->rcFreshN; i++) {
        if (strcmp(g->rcFreshName[i], nm) != 0) continue;
        const char *bt = arenaPrintf(g->arena, "(void *)&(%s)", arenaRefAt(g, g->rcFreshLvl[i]));
        const char *rr = rcRegionRef(g, bt);
        return rr;
    }
    return NULL;
}

/** @brief Which cell this C name occupies in this frame's root table; -1 when it is not in the
 *         table (or only in the global root table). */
static int rcRootIndexOf(CG *g, const char *nm) {
    if (!nm) return -1;
    for (size_t i = 0; i < g->rcRoots.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcRoots, i), nm) == 0) return (int)i;
    return -1;
}

/** @brief Has this identifier been written into the reference field of some heap object (see
 *         `g->rcLinked`)? */
static bool rcLinkedHas(CG *g, const char *nm) {
    if (!nm) return false;
    for (size_t i = 0; i < g->rcLinked.len; i++)
        if (strcmp(*(const char **)vecAt(&g->rcLinked, i), nm) == 0) return true;
    return false;
}

/**
 * @brief Overwriting a reference slot: is it worth a **point fix** release
 *        (`extc_rc_storeRefOwnedRelease`)?
 *
 * This is the shape "build one tree per round, the next round overwrites it through the same
 * long-lived slot" (`ring_per_round`: the ring head `head` is stored into `keep`, and the next
 * round overwrites `keep`). All four criteria must hold.
 *
 *   1. This assignment is inside some loop body (outside a loop there is no "drop a tree per
 *      round").
 *   2. The target slot is a reference slot of this frame (in `rcRoots`) and is declared outside
 *      **all** of the loops in the registry -- it is overwritten at the same slot every round. A
 *      cursor inside a loop (`cur = t`) fails 2 and is excluded.
 *   3. The value written is a local object this frame **freshly allocated and never rebound**
 *      (`rcFreshName`), and the region it lives in is **owned by this frame**: only there is this
 *      frame's root table a complete seed (the same criterion as the trigger at the allocation
 *      point).
 *   4. This object has already been written into the reference field of some heap object
 *      (`(*cur).next = head`) -- only a fresh local chained into a heap object can be the head of
 *      a tree or a ring.
 *
 * **The static criteria only decide whether it is worth it**; the real work happens at run time
 * -- marking starts from **the other roots plus the cross-region seeds** and frees only the slots
 * reachable **from the old value alone**. Being wrong on every criterion cannot dangle (see the
 * note on `extc_rc_releaseOld`).
 *
 * When it cannot be emitted (unreliable root table, sharded, no static region, target not in the
 * root table, ...) it returns false and **falls back to `storeRefOwned` plus the region sweep**;
 * with reclamation off (`EXTC_RECLAIM=0`) the output is byte-identical.
 *
 * @param g        The generator state.
 * @param tgt      The target slot's C lvalue text.
 * @param tgtName  The target slot's C name (the root table is looked up by name).
 * @param val      The C text of the value being written.
 * @param value    The expression of the value being written.
 * @return         true when a point-fix release was emitted.
 */
static bool rcEmitStoreRelease(CG *g, const char *tgt, const char *tgtName,
                               const char *val, Expr *value) {
    /* In the event tier (`EXTC_RC_SWEEPFREE`) the sweep never triggers, so this mechanism has
     * nothing to do: its value is "a ring does not wait for a sweep", and the event tier is only
     * used for units proved **acyclic**, where every unreachable object is released on the spot
     * when its count reaches zero. This mechanism also **ignores the count** (`releaseOld` marks
     * the whole live set from the other roots), so keeping it in the event tier costs without
     * paying: shapes such as tail insertion would pay a full marking on every assignment. The
     * criterion hangs on the event tier's memory gate (`gate-event-bounded`). */
    if (g->rc.sweepFree == 1) return false;
    if (!g->rc.reclaim || g->rc.rootsUnreliable || g->coroFunc) return false;
    /* Sharding (`EXTC_RC_SHARDS` > 1): marking starts only from the roots of **this one shard
     * region** plus the cross-region seeds, so an object that only live objects in another shard
     * point at cannot be marked => it would be taken for "reachable from the old value alone" and
     * released (dangling). Under sharding `rcNoteFreshLocal` registers nothing anyway => nothing
     * can be emitted here; blocking it explicitly keeps the **code and this comment from one
     * source**, instead of relying on a side effect elsewhere. */
    if (g->rc.nShard > 1) return false;
    if (g->loopLen <= 0) return false;                       /* 1 */
    if (!value || value->kind != EX_IDENT) return false;
    const char *vn = planCName(value) ? planCName(value) : value->u.ident.name;
    if (!vn || !rcLinkedHas(g, vn)) return false;             /* 4 */
    int idx = rcRootIndexOf(g, tgtName);                     /* 2 */
    if (idx < 0) return false;   /* the target is not a reference slot of this frame (neither globals nor the fields of a local
     * aggregate are emitted) */
    for (int i = 0; i < g->loopLen; i++)
        if (idx >= g->loopRootBase[i]) return false;         /* declared inside some enclosing loop */
    const char *vreg = NULL;
    for (int i = 0; i < g->rcFreshN; i++) {
        if (strcmp(g->rcFreshName[i], vn) != 0) continue;
        if (!rcFrameOwnsArena(g, g->rcFreshLvl[i])) return false;   /* 3 */
        vreg = rcFreshHostRegion(g, value);
        break;
    }
    if (!vreg) return false;
    if (g->rcGlobalRoots.len + g->rcRoots.len > 4096) return false;
    Buf b;
    bufInit(&b, g->arena);
    size_t n = rcRootTableTextSkip(g, &b, (size_t)idx);
    cgLine(g, "extc_rc_storeRefOwnedRelease((void **)&(%s), (void *)(%s), (void *)0, %s, %s, %zu);",
           tgt, val, vreg, bufCstr(&b), n);
    return true;
}

/**
 * @brief Walk the **value projections** to find the object hosting a reference slot.
 *
 * The cross-region registration bit is set on the **target** object, while the "source" is the
 * object holding this slot: `a->f` is `a`, `*p` is `p`, the inline array element `w.arr[i]` is `w`
 * (the element storage lives in the object's payload, independent of the index), and `p[i]` of a
 * `ref [N]T` is `p`. So the walk goes outwards only along **value fields** (the `EX_FIELD` levels
 * whose base type is not a reference), inline arrays and the elements of a `ref [N]T`, until it
 * reaches a `ref` base or a dereference.
 *
 * Why three states: when the host cannot be traced, **sweeping as usual is out of the question**
 * (an incomplete seed releases too early). The caller picks the path from the return value:
 * `RC_HOST_OBJ` registers, `RC_HOST_LOCAL` does nothing (the root table covers it), and
 * `RC_HOST_UNKNOWN` takes the conservative path.
 *
 * @param g    The generator state (needs `subst` for generic arguments).
 * @param t    The assignment's target expression.
 * @param obj  Out: the host object expression for `RC_HOST_OBJ`.
 * @return     See `RcHost`.
 */
static RcHost cgSlotHost(CG *g, Expr *t, Expr **obj) {
    *obj = NULL;
    for (;;) {
        if (!t) return RC_HOST_UNKNOWN;
        switch (t->kind) {
        case EX_DEREF:
            /* `*p = v`: the slot is the payload of the object p points to. */
            *obj = t->u.deref.operand;
            return RC_HOST_OBJ;
        case EX_FIELD: {
            Expr *b = t->u.field.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            /* `p.f = v` (p is a reference, field access dereferences automatically): the slot is in *p's
     * payload. */
            if (bt && bt->kind == TY_REF) { *obj = b; return RC_HOST_OBJ; }
            /* value field: the slot's storage still belongs to b's host, keep walking outwards */
            t = b;
            continue;
        }
        case EX_INDEX: {
            Expr *b = t->u.index.obj;
            Type *bt = b ? subst(g, b->type) : NULL;
            if (bt && bt->kind == TY_REF) {
                Type *bi = bt->inner ? ttBase(subst(g, bt->inner)) : NULL;
                /* `p[i] = v` where p is `ref [N]T`: the element is inline in *p. */
                if (bi && bi->kind == TY_ARRAY) { *obj = b; return RC_HOST_OBJ; }
                /* a ref to a view: the element is in the storage the view's data points to, untraceable. */
                return RC_HOST_UNKNOWN;
            }
            /* inline array (a value): the element storage belongs to the array's host. */
            if (bt && bt->kind == TY_ARRAY) { t = b; continue; }
            /* view element: data may point into a plate, a pool, the stack or C storage -- the host cannot
     * be traced. */
            return RC_HOST_UNKNOWN;
        }
        case EX_IDENT:
            /* a variable's own storage: the static root table registers it (together with its value fields
     * and inline arrays) as a root. */
            return RC_HOST_LOCAL;
        /* kind-default: a projection whose host cannot be traced is always UNKNOWN (the safe
         * direction). A new kind that can host a reference slot must add a case here instead of
         * slipping through default. */
        default:
            return RC_HOST_UNKNOWN;
        }
    }
}

static bool rcLinkStmt(void *ctx, Stmt *s);
static bool rcLinkExpr(void *ctx, Expr *e);

/**
 * @brief Statement callback: record the identifiers "written into a heap object's reference
 *        field" in `g->rcLinked`.
 *
 * Only a **heap host** (`RC_HOST_OBJ`: `(*cur).next = head`, `p.f = v`) counts as "chained into a
 * tree"; a local's slot (a local array in `a[i] = v`, a local struct field) is the tier the root
 * table covers and does not count.
 *
 * @param ctx  The generator state.
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcLinkStmt(void *ctx, Stmt *s) {
    CG *g = (CG *)ctx;
    Expr *tgt = (s && s->kind == ST_ASSIGN) ? s->u.assign.target : NULL;
    Expr *ve  = (s && s->kind == ST_ASSIGN) ? s->u.assign.value : NULL;
    if (tgt && ve && !s->u.assign.op && ve->kind == EX_IDENT && g->rc.reclaim &&
        tgt->type && cgIsRefType(g, tgt->type)) {
        Expr *base = NULL;
        if (cgSlotHost(g, tgt, &base) == RC_HOST_OBJ) {
            const char *nm = planCName(ve) ? planCName(ve) : ve->u.ident.name;
            if (nm) *(const char **)vecPush(&g->rcLinked) = nm;
        }
    }
    AstVisit v = { rcLinkExpr, rcLinkStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/** @brief Expression callback: keep walking (an assignment can also be nested inside an
 *         expression statement or a condition). */
static bool rcLinkExpr(void *ctx, Expr *e) {
    AstVisit v = { rcLinkExpr, rcLinkStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Scan the function body once: which local names have been written into a heap object's
 *        reference field (the evidence set behind criterion 4).
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcScanLinked(CG *g, Stmt *body) {
    vecInit(&g->rcLinked, g->arena, sizeof(const char *));
    if (!body || !g->rc.reclaim) return;
    AstVisit v = { rcLinkExpr, rcLinkStmt, g };
    astWalkStmtChildren(body, &v);
}

/**
 * @brief Account for a **new** reference slot whose value is already in place (a declaration).
 *
 * The counted tiers need one increment per stored pointer; the sweep tier also needs the shade,
 * because a white object stored into a slot while a marking is in flight would otherwise be missed.
 * The **event tier** has neither: EXTC_RC_SWEEP_FREE 1 makes extc_rc_shouldSweep a
 * compile-time 0, so extc_rc_begin is never called and extc_rc_wb -- the participant count that is
 * the first test in extc_rc_shade / extc_rc_shadeOld -- stays 0 for the whole run: every shade in
 * this unit is dead code. What remains of initRef is its self-store (the C initializer already
 * wrote the slot, so the store reads and writes the same storage) plus the increment; the increment
 * alone is extc_rc_inc, so the event tier emits that and drops the dead half.
 *
 * Kept per-site (not "the event tier never emits initRef" everywhere): using this in the
 * sweep/mixed tier would remove a live write barrier.
 */
static void cgInitRefStore(CG *g, const char *slotExpr) {
    if (g->rc.sweepFree == 1 && !g->rc.noCount) {
        cgLine(g, "extc_rc_inc((void *)(%s));", slotExpr);
        return;
    }
    cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", slotExpr, slotExpr);
}

/**
 * @brief When a whole-value store carries reference leaves, emit the reference-slot accounting
 *        **leaf by leaf**.
 *
 * A **by-value** whole-block copy (struct or fixed-size array) such as `s.w = wrap { n: keeper }`
 * or `var b: wrap = wrap { n: keeper }` can hide reference leaves, and C's whole-block assignment
 * is a bare compound-literal copy (`s->w = (wrap){.n = keeper};`): it neither `inc`s nor registers
 * a cross-region edge. The slot is therefore written without any accounting -- and then either
 * the count never learns of this holder (the only visible holder lets go, the count reaches zero
 * and the slot dangles), or the sweep's seed is incomplete (no cross-region in-edge was
 * registered => the target region's sweep releases objects that are still pointed at, the other
 * half of the hole that `cross_region_elem` shows). So the whole block is split back into **one
 * reference-slot assignment per leaf**: a new slot goes through `initRef` (write only and
 * increment, never read the old value), an overwritten slot through `storeRefOwned` /
 * `storeRefSlot` (decrement the old value, increment the new one; when the host cannot be proved,
 * first reverse-look-up by **slot address** which region the slot falls in, and only when that
 * fails make the target region drop out of sweeping) -- the same path as a direct reference-slot
 * assignment.
 *
 * The caller is responsible for evaluating the target and the value **once each** (into
 * temporaries), because every accounting entry assembles the path once more.
 *
 * @param g           The generator state.
 * @param t           The type of the whole-value store (taken in the caller's substituted
 *                    context).
 * @param dstBase     The C lvalue expression of the target storage (such as `(*__extc_svd3)`,
 *                    `b`).
 * @param srcBase     The C lvalue expression of the value (such as `__extc_svv3`); not read when
 *                    `init` is true.
 * @param init        true = new slot (the initialization of a `var ...`): increment only; false =
 *                    overwrite: read the old value and decrement.
 * @param owner       "the object holding this slot" (the third argument of `storeRefOwned`); pass
 *                    `0` for a local or static slot.
 * @param hostUnknown The host cannot be proved => every leaf takes `storeRefSlot` (reverse lookup
 *                    of the region by slot address), and only when the slot is not in an rc slab
 *                    either does the conservative path apply (the target region drops out of
 *                    sweeping).
 */
static void cgValueRefStore(CG *g, Type *t, const char *dstBase, const char *srcBase,
                            bool init, const char *owner, bool hostUnknown) {
    /* Collect the **relative** path only once (`.n`, `.kids.data[0]`, empty string for a top-level
     * reference) and assemble it against each of the two bases -- the target's and the value's leaf
     * sets are necessarily isomorphic by construction, so no path exists on one side only. */
    Vec rel;
    vecInit(&rel, g->arena, sizeof(const char *));
    rcLeafPaths(g, t, "", &rel, 0);
    for (size_t i = 0; i < rel.len; i++) {
        const char *p = *(const char **)vecAt(&rel, i);
        const char *d = arenaPrintf(g->arena, "%s%s", dstBase, p);
        if (init) {
            /* the value is already in the target (the declaration is the initialization): one
             * increment per leaf, through the entry that knows which tier can drop the dead half */
            dbgRcSite(g, g->blkLevel, 0);
            cgInitRefStore(g, d);
        } else if (hostUnknown) {
            /* When the host expression cannot be proved, the **slot address** itself may carry the answer
             * (it falls inside an rc slot => that is the memory of some region): try the reverse lookup
             * first, and fall back to the conservative path only when the slot is not in an rc slab
             * either (view data, C or pool buffers). */
            cgLine(g, "extc_rc_storeRefSlot((void **)&(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p));
        } else {
            cgLine(g, "extc_rc_storeRefOwned((void **)&(%s), (void *)(%s), (void *)(%s));",
                   d, arenaPrintf(g->arena, "%s%s", srcBase, p), owner);
        }
    }
}

/**
 * @brief Emit the region release at **frame destruction** (next to `extc_arena_destroy`).
 *
 * The claim key is the arena's address (see the table in `emitRuntimePrologue`), so only the
 * address is emitted here and the region itself need not be known. A coroutine step uses the
 * frame's `arena<k>` and spells it exactly as the arena release does. Under sharding one logical
 * region claims N run-time regions (key = address + k), so every shard must be **dropped
 * individually** -- missing one leaves that shard's slab and registration bit unreleased forever.
 *
 * Called only at frame destruction: a block release (`cgReleaseLevel`) does not drop the region,
 * or a loop body would create a new 1 MiB slab on every iteration. A region outliving its block is
 * safe -- every object in the block is unreachable and the sweep reclaims them by pressure.
 *
 * @param g    The generator state; nothing is emitted while `needReclaim` is off.
 * @param lvl  The block level (1 = the function body).
 */
static void cgReclaimDropArena(CG *g, int lvl) {
    if (!g->rc.reclaim) return;
    /* This function never referenced this level (it neither allocated nor handed it out as a home)
     * => this arena cannot have a region and the `regionDrop` would only scan the registry for
     * nothing. Skip it. */
    if (lvl >= 1 && !(g->rcArenaUsed & (1u << lvl))) return;
    if (g->rc.nShard <= 1) {
        /* single shard: one region, one drop -- the same text either way */
        if (g->coroFunc)
            cgLine(g, "extc_rc_regionDrop((void *)&%s->arena%d);", g->coroFrame, lvl);
        else
            cgLine(g, "extc_rc_regionDrop((void *)&__extc_a[%d]);", lvl);
        return;
    }
    const char *ar = g->coroFunc ? arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, lvl)
                                 : arenaPrintf(g->arena, "__extc_a[%d]", lvl);
    for (int k = 0; k < g->rc.nShard; k++)
        cgLine(g, "extc_rc_regionDrop((void *)((char *)&%s + %d));", ar, k);
}

/**
 * @brief Account for a declaration that **takes a value over** from an existing name.
 *
 * Two things are recorded. A local declared under this name is a new object, so a previous
 * "moved out" marking for the name is forgotten. And when the initializer is a plain name of a
 * type that owns state (no `@value` clone: a clone builds an independent value, so the source
 * keeps its own), the new local owns the state from here on and the old name's `@drop` hook must
 * not run at the end of the block -- that is the difference between a move and a leak or a
 * double release.
 *
 * @param g   Generator state; the moved-name set is edited.
 * @param s   The `var` statement.
 * @param nm  The C name of the new local.
 */
static void noteVarDeclMove(CG *g, Stmt *s, const char *nm) {
    forgetMoved(g, nm);
    if (!s || !s->u.var.init || planVarClone(s)) return;
    if (!typeNeedsDrop(g, subst(g, s->type), 0)) return;
    noteMoved(g, plainIdentCName(g, s->u.var.init));
}


/**
 * @brief Emit `ST_VAR`: an ordinary declaration, a coroutine spawn, an `@overwrite` unit, and a
 *        declaration initialized by `f()?`.
 *
 * The coroutine spawn branch treats the frame as an ordinary local value and initializes the
 * parameters in place (a resume has no arguments to pass again); with a `var h: coroutine<T>`
 * annotation the frame goes into the task's own place, while the plain form leaves it on the
 * caller's stack with no allocation. The `@overwrite` branch keeps a single storage cell: allocated
 * lazily on first execution, then only zeroed and reused. A variable inside a coroutine body is not
 * a C local but a field of the frame, so its initialization is emitted as an assignment.
 *
 * @param g  The code generation state.
 * @param s  The variable statement.
 */
static void stVar(CG *g, Stmt *s) {
    rcNoteFreshLocal(g, s);
/* `let c = counter(args)`: **spawn**. The frame is a plain value living here, and the
 * arguments initialize the parameters -- a resume has none to pass them again. */
if (planCName(s) && s->u.var.init && s->u.var.init->kind == EX_CALL &&
    planCallee(s->u.var.init) && planIsCoro(planCallee(s->u.var.init))) {
    FuncDef *cf = planCallee(s->u.var.init);
    const char *fr = cType(g, s->type);      /* the synthesized frame type */
    /* The **task's own place**: entered here (lazily, at the spawn), stored in the frame,
     * and left again so the caller keeps its own place current. Everything the coroutine
     * creates goes in there and outlives every suspension; the task's end releases it in
     * one go (see the `$next` driver in `genCoroDecls`). */
    /* `var h: coroutine<T> = f(...)`: the annotation *is* the handle type, so the frame
     * goes into the task's own place and the variable holds a 24-byte handle. This is the
     * escape-driven choice in its simplest form -- the same source shape without the
     * annotation keeps the frame on the caller's stack (zero allocation). */
    if (s->type && isProtoType(s->type, "coroutine", 1)) {
        const char *cn = cFuncName(g, cf);
        int sq = g->coroSeq++;
        flushPrefix(g);
        g->needCoroHandle = true;
        cgLine(g, "int64_t __extc_czh%d = extc_task_begin();", sq);
        cgLine(g, "int64_t __extc_czz%d = extc_task_zone(__extc_czh%d);", sq, sq);
        cgLine(g, "struct %s$frame *__extc_czf%d = (struct %s$frame *)extc_task_alloc("
                  "__extc_czh%d, (int64_t)sizeof(struct %s$frame), \"%s\", %d);",
               cn, sq, cn, sq, cn, g->path, s->line);
        Buf bx;
        bufInit(&bx, g->arena);
        bufPrintf(&bx, "*__extc_czf%d = (struct %s$frame){ .pc = 0, .zone = __extc_czz%d,"
                      " .task = __extc_czh%d", sq, cn, sq, sq);
        for (size_t i = 0; i < cf->params.len; i++) {
            Param *p = *(Param **)vecAt(&cf->params, i);
            Expr *a = i < s->u.var.init->u.call.args.len
                          ? *(Expr **)vecAt(&s->u.var.init->u.call.args, i) : NULL;
            bufPrintf(&bx, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
        }
        bufPrintf(&bx, " };");
        cgLine(g, "%s", bufCstr(&bx));
        cgLine(g, "extc_coro %s = (extc_coro){ .frame = __extc_czf%d, .kind = %d,"
                  " .task = __extc_czh%d };", planCName(s), sq, planCoroKind(cf), sq);
        /* A **local** boxed handle is a task root while its slot lives: `extc_task_collect` keeps a
         * task alive when any registered slot still names it. The **slot address** (not the id) is
         * registered, so a later assignment to the same variable needs no new registration; a copy
         * into another local variable gets its own registration at its own declaration (the generic
         * `ST_VAR` tail below). Not enumerated, and deliberately so: a handle stored into an
         * aggregate, a container or a struct field -- the static root table is per-slot. Registering
         * less only lets a task that no slot of the program reaches be freed; it never
         * frees a task a registered slot still names. A step function is skipped: a suspension
         * returns before any release point could pop the registration. */
        if (g->taskRoots && !g->coroFunc && planCName(s))
            cgTaskRootPush(g, arenaPrintf(g->arena, "&%s.task", planCName(s)));
        g->needCoroHandle = true;
        g->needPool = true;      /* the task table brings the zone runtime with it */
        /* A coroutine frame can hold reference slots, and the static root table does not enumerate the
     * frame => no trigger is emitted for this function (the safe direction). */
        if (g->rc.reclaim) g->rc.rootsUnreliable = true;
        return;
    }
    const bool hasZone = planCoroNeedsZone(cf);
    const int  sq = g->coroSeq++;
    if (hasZone) {
        flushPrefix(g);
        cgLine(g, "int64_t __extc_czsv%d = extc_placeTop;", sq);
        cgLine(g, "int64_t __extc_czid%d = extc_task_begin();", sq);
        cgLine(g, "int64_t __extc_czm%d = extc_task_zone(__extc_czid%d);", sq, sq);
        noteStackTask(g, sq);
        /* A step function has no release point on every exit (a suspension returns, and an
         * abandoned step never reaches its end), so a nested task is registered only in an
         * ordinary function, where the block release point and every return pop it. */
        if (!g->coroFunc) cgTaskRootPush(g, arenaPrintf(g->arena, "&__extc_czid%d", sq));
    }
    /* `var d = inner(args)` **inside a coroutine**: the nested frame is a *field* of this
     * frame, not a C local -- a resume jumps past the declaration, so the initializer has
     * to be an assignment to the field (same rule as every other frame-field local below). */
    bool framed = false;
    if (g->coroFunc) {
        for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
            framed = p->cname && strcmp(p->cname, planCName(s)) == 0;
        }
    }
    Buf init;
    bufInit(&init, g->arena);
    if (framed)
        bufPrintf(&init, "%s->%s = (%s){ .pc = 0", g->coroFrame, planCName(s), fr);
    else
        bufPrintf(&init, "%s %s = (%s){ .pc = 0", fr, planCName(s), fr);
    if (hasZone) bufPrintf(&init, ", .zone = __extc_czm%d, .task = __extc_czid%d", sq, sq);
    for (size_t i = 0; i < cf->params.len; i++) {
        Param *p = *(Param **)vecAt(&cf->params, i);
        Expr *a = i < s->u.var.init->u.call.args.len
                      ? *(Expr **)vecAt(&s->u.var.init->u.call.args, i) : NULL;
        bufPrintf(&init, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
    }
    bufPrintf(&init, " };");
    flushPrefix(g);
    cgLine(g, "%s", bufCstr(&init));
    if (hasZone) cgLine(g, "extc_placeTop = __extc_czsv%d;", sq);
    /* The frame is a value local and can hold reference slots; the static root table does not
     * enumerate the synthesized frame => this function turns triggers off (the safe direction). */
    if (g->rc.reclaim) g->rc.rootsUnreliable = true;
    return;
}
/* A frame field is not declared here: the frame struct holds it, and the assignment is
 * what a resume re-runs -- the initializer runs on every execution, as a C local's
 * would. */
if (g->coroFunc && planCName(s)) {
    bool framed = false;
    for (size_t i = 0; i < g->coroFunc->coroFrame.len && !framed; i++) {
        const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
        framed = p->cname && strcmp(p->cname, planCName(s)) == 0;
    }
    if (framed) {
        if (s->u.var.init) {
            const char *v = genTopExpr(g, s->u.var.init);
            flushPrefix(g);
            /* A frame field is an **existing** slot (the frame is a compound literal, so pointer fields
             * read as 0) and a resume re-runs this initialization => overwrite semantics, through
             * storeRef: decrement the old value, increment the new one. */
            if (g->rc.reclaim && cgIsRefType(g, s->type))
                cgLine(g, "extc_rc_storeRef((void **)&%s->%s, (void *)(%s));",
                       g->coroFrame, planCName(s), v);
            else
                cgLine(g, "%s->%s = %s;", g->coroFrame, planCName(s), v);
        }
        return;
    }
}
/* `cname` is the name the checker decided on; a shadowed one
 * carries a `__2` suffix. */
const char *nm = planCName(s) ? planCName(s) : s->u.var.name;
/* `global var x: T` / `global let x: T = c`: emit **C block-scope `static`** -- static storage
 * duration with the name visible only in this block, which is exactly the semantics needed.
 * Two further benefits: it is fine for the declaration to be **executed repeatedly** (a loop or a
 * coroutine resume both jump into the middle of the statement), because C initializes a static
 * object once, at program start. The initializer must be constant, and the checker already
 * enforces that. */
const char *stg = s->u.var.isGlobal ? "static " : "";
/* `@overwrite` keeps exactly one block of storage. It is allocated
 * the first time this statement runs and only cleared and reused
 * afterwards. What comes out is a few lines of inline C,
 * `extc_arena_alloc` plus `memset`, so no new runtime support is
 * needed. */
if (s->u.var.overwrite) {
    /* One block of storage is reused; `extc_owcell`, a block plus
     * its capacity, covers all three shapes - a single value, a
     * fixed-size buffer and a runtime length. The first execution
     * allocates and lazily at that, every later one clears and
     * reuses, and a reused cell reads as the zero value `new` promises.
     *
     * Where the cell comes from: a site of this function uses a cell
     * in its own frame, a site of a callee uses the cell passed in
     * by the call site. */
    int k = owIndex(g, s);
    Expr *nx = s->u.var.init;
    if (dbgOn("EXTC_DBG_ARENA")) arenaDriftCheck(g, nx, "@overwrite new");
    Type *st_t = subst(g, nx->u.new_.type);
    bool loc = f_owLocal(g, s);
    if (k >= 0) {
        const char *cell = loc ? arenaPrintf(g->arena, "&__extc_ow%d", k)
                               : arenaPrintf(g->arena, "__extc_owarg%d", k);
        /* The storage lives where the cell's own `home` says:
         * with a home it belongs to `__extc_home`, otherwise to
         * level 1 of this frame. `arenaRefAt(planArenaLevel(nx))` must
         * not be used here: that number is the callee's own level,
         * while the cell lives elsewhere, and the two do not share a
         * lifetime. */
        const char *owcv = arenaPrintf(g->arena, "__owc%d", k);
        const char *ar = arenaPrintf(g->arena, "(*%s->home)", owcv);
        (void)planArenaLevel(nx);
        const char *ct = cType(g, st_t);
        flushPrefix(g);
        /* The site index has to be part of the name: two
         * `@overwrite` sites in one function would otherwise be a
         * redefinition of the same variable. */
        cgLine(g, "extc_owcell *%s = %s;", owcv, cell);
        if (!nx->u.new_.count) {
            /* A single value, or a fixed-size `new [N]T`, has a
             * constant size. The binding has to be declared first,
             * because a declaration inside the if or the else branch
             * would go out of scope at its closing brace. */
            cgLine(g, "%s %s;", cType(g, s->type), nm);
            /* The temporary must not be named `p`: with
             * `@overwrite var p = ...` a `void *p` would shadow the
             * outer `array_T *p`, turning `p = (array_T *)p` into a
             * self-assignment and leaving the binding null. */
            const char *owtp = arenaPrintf(g->arena, "__owp%d", k);
            /* With reclamation on, this storage also comes from an rc region: an `@overwrite` binding can
             * have a reference type too, and a pointer into the arena reaching the counting functions
             * would read garbage. The region is claimed by the cell's home arena and lives as long as
             * that arena. */
            if (g->rc.reclaim) {
                /* Type descriptor + sweep trigger, placed before the allocating statement: this storage also comes
                 * from an rc region, the descriptor is stored with the slot, and the region claim key the
                 * trigger uses matches the `extc_rc_alloc` below (the same shard number, see
                 * `rcAllocCore`). */
                const char *td = rcEmitTypeDesc(g, st_t);
                /* Is this region (the cell's `home`) **owned by this frame**: the cell is in this frame
                 * (`f_owLocal`) and this function does not receive a home, so `home` is this frame's
                 * `&__extc_a[1]`. When this function does receive a home (`planUsesHome`, so the cell's
                 * `home` is the caller's arena), or `owLocal` is false (a generic instance generated
                 * during codegen), both land on an **outer frame** -- this frame's static root table
                 * cannot see the outer frame's live references, so this tier only records what is owed
                 * (see `rcEmitOwe`). **`main` is the exception (subset A)**: the home `main` receives
                 * is its own `&__extc_a[1]`, so it counts (the same criterion as
                 * `rcFrameOwnsArena`). */
                bool own = loc && (!g->rcFuncUsesHome || g->inMain);
                /* The thread-local arena tier: the cell's `home` is `extc_tls_arena` or this frame's arena, and
                 * ownership is decided at run time (`RC_OWN_TLS`) => an inner frame only records what is
                 * owed. */
                RcOwn owOwn = g->parTls ? RC_OWN_TLS : (own ? RC_OWN_YES : RC_OWN_NO);
                const char *core = rcAllocCore(g, arenaPrintf(g->arena, "(void *)%s->home", owcv),
                                               planArenaLevel(nx), owOwn,
                                               arenaPrintf(g->arena, "(int64_t)sizeof(%s)", ct), td);
                flushPrefix(g);
                cgLine(g, "if (!%s || !%s->p) { void *%s = %s;  if (%s) %s->p = %s;"
                          "  %s = (%s)%s; }",     /* the C type is already a pointer */
                        owcv, owcv, owtp, core,
                        owcv, owcv, owtp, nm, cType(g, s->type), owtp);
            } else
                cgLine(g, "if (!%s || !%s->p) { void *%s = extc_arena_alloc(&%s,"
                          " (int64_t)sizeof(%s), \"%s\", %d);  if (%s) %s->p = %s;"
                          "  %s = (%s)%s; }",     /* the C type is already a pointer */
                        owcv, owcv, owtp, ar, ct, g->path, nx->line,
                        owcv, owcv, owtp, nm, cType(g, s->type), owtp);
            cgLine(g, "else { memset(%s->p, 0, (size_t)sizeof(%s));"
                      "  %s = (%s)%s->p; }",
                    owcv, ct, nm, cType(g, s->type), owcv);
        } else {
            /* A runtime length uses `{ptr, cap}` and doubles the
             * capacity when it grows, so the memory stays within
             * twice the largest length seen and does not depend on
             * the number of iterations. */
            const char *cnt = genExpr(g, nx->u.new_.count);
            if (planNeedTemp(nx)) {
                const char *t = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
                pfLine(g, "int64_t %s = (int64_t)(%s);", t, cnt);
                cnt = t;
            }
            cgLine(g, "int64_t __owk = (int64_t)(%s);", cnt);
            cgLine(g, "if (__owk < 0) { extc_trapMsg(\"%s\", %d,"
                      " \"negative length\"); }", g->path, nx->line);
            cgLine(g, "%s %s;", cType(g, s->type), nm);
            cgLine(g, "if (!%s) { %s = (%s){ .data = (%s *)extc_arena_alloc(&%s,"
                      " __owk * (int64_t)sizeof(%s), \"%s\", %d), .len = __owk }; }",
                    owcv, nm, cType(g, s->type), ct, ar, ct, g->path, nx->line);
            cgLine(g, "else { if (__owk > %s->cap) { int64_t c = %s->cap * 2;"
                      " if (c < __owk) c = __owk;"
                      "  %s->p = extc_arena_alloc(&%s, c * (int64_t)sizeof(%s),"
                      " \"%s\", %d);  %s->cap = c; }"
                      "  %s = (%s){ .data = (%s *)%s->p, .len = __owk }; }",
                    owcv, owcv, owcv, ar, ct, g->path, nx->line, owcv,
                    nm, cType(g, s->type), ct, owcv);
            cgLine(g, "memset(%s.data, 0, (size_t)(__owk * (int64_t)sizeof(%s)));",
                    nm, ct);
        }
        /* An `@overwrite` binding is a reference slot too: it stays live past this statement, until the
     * block ends. */
        rcRootDecl(g, s->type, nm);
        return;
    }
}
/* `let q = f()?` and `var q = f()?` are one of the legal positions
 * of `?`. The shape matches the assignment case, because the generated
 * C is a declaration followed by an initialization. */
if (s->u.var.init && s->u.var.init->kind == EX_TRY) {
    TryInfo ti = genTryHead(g, s->u.var.init);
    flushPrefix(g);
    size_t lb = g->out->len;
          /* `@value`: copying from a place would share storage, so the value is built through the
       * type's `clone` (a temporary was moved and never reaches this branch). */
      if (planVarClone(s) && s->u.var.init) {
          StructDef *vsd = structOf(ttBase(subst(g, s->type)));
          const char *fn = vsd && vsd->cloneHook ? cMethodName(g, subst(g, s->type), vsd->cloneHook)
                                                 : NULL;
          const char *src = genExpr(g, s->u.var.init);
          if (fn && src)
              cgLine(g, "%s%s %s = %s(&(%s));", stg, cType(g, s->type), nm, fn, src);
          else
              cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, tryPayloadPath(g, &ti));
      } else
          cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, tryPayloadPath(g, &ti));
    localDef(g, lb, nm, 1);
    noteVarDeclMove(g, s, nm);
    rcRootDecl(g, s->type, nm);
    return;
}
const char *init = s->u.var.init ? genTopExpr(g, s->u.var.init)
                                 : zeroInit(g, s->type);
flushPrefix(g);
size_t lb = g->out->len;
/* `@value`: a copy from a place goes through the type's `clone` (a temporary was moved and is
 * not marked, so this branch is skipped). */
{
    StructDef *vsd = s->type ? structOf(ttBase(subst(g, s->type))) : NULL;
    const char *fn = (planVarClone(s) && vsd && vsd->cloneHook)
                         ? cMethodName(g, subst(g, s->type), vsd->cloneHook) : NULL;
    if (fn && s->u.var.init && init) {
        /* A method that returns an aggregate takes the same implicit arguments as any other call
         * (`int64_t __extc_home_zone`), so the call site has to append them. */
        Buf cl;
        bufInit(&cl, g->arena);
        bufPrintf(&cl, "%s(&(%s)", fn, asF32(g, s->type, s->u.var.init, init));
        cgImplicitArgs(g, &cl, vsd->cloneHook, s->u.var.init, 1, false);
        bufPuts(&cl, ")");
        cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, bufCstr(&cl));
    }
    else
        cgLine(g, "%s%s %s = %s;", stg, cType(g, s->type), nm, asF32(g, s->type, s->u.var.init, init));
}
localDef(g, lb, nm, 1);
    noteVarDeclMove(g, s, nm);
/* Counting for reference slots. A **new slot** goes through initRef: it writes and increments the
 * target, and **never reads the old value** -- the "old value" lying in a freshly declared slot is a
 * free-list slot number (the slab allocator keeps its free list in the slot head), and dereferencing
 * it as a pointer crashes. The value is read again from the variable just written, so evaluation
 * happens only once.
 *
 * Global declarations are skipped: they are `static`, so the statement **runs again** every time
 * control reaches it and the count would be incremented repeatedly; and a global reference is a
 * process-level root that belongs with the root table (left for the next step).
 *
 * A cross-region store (the object is in an outer region, the slot in this block) registers one more
 * bit: a bit on the target object's position, and the sweep walks the bitmap in order when seeding.
 * Registering too much is the safe direction (it only keeps an object alive longer); registering too
 * little is a missed root. */
if (g->rc.reclaim && !s->u.var.isGlobal && cgIsRefType(g, s->type)) {
    dbgRcSite(g, g->blkLevel, 0);
    /* The no-hold entry is a **no-count-tier** optimisation only: the count tier's cascade
     * decrements a slot's target when the slot's object dies, so every pointer stored into a slot
     * has to be counted exactly once -- NoHold would leave that decrement without an increment and
     * free the target early. In the count tier the plain `initRef` is emitted even where the
     * checker proved the old value dead. */
    if (planRcNoDec(g->curFunc) && g->rc.noCount)
        cgLine(g, "extc_rc_initRefNoHold((void **)&%s, (void *)%s);", nm, nm);
    else
        cgInitRefStore(g, nm);
} else if (g->rc.reclaim && !s->u.var.isGlobal && s->u.var.init) {
    /* A **whole-value** declaration's reference leaves (`var b: wrap = wrap { n: keeper }`) need
     * accounting too: a bare C initialization emits no `inc` at all, so `b.n` is another way for "the
     * slot was written without accounting" (the same hole as the assignment side). With no
     * initializer the value is **zero** (`zeroInit`) => every reference leaf is null, `inc` has
     * nothing to do, and the whole `initRef` would only place a few hundred lines into the output for
     * nothing (a local such as `world` with 64+1024 slots). */
    RcScan vsc = rcScanType(g, s->type, 0);
    if (vsc == RC_LEAF_REFS) {
        cgValueRefStore(g, s->type, nm, nm, true, "0", false);
    } else if (vsc == RC_CONSERVATIVE) {
        bool enumRefs = false;
        if (rcEnumRootable(g, s->type, 0, &enumRefs) && enumRefs) {
            /* References hidden only in enum payloads: `rcRootDecl` then registers the payload leaves of
             * **every** variant as roots (garbage words read from an inactive variant are filtered by
             * `extc_rc_owns`), so the store side only has to "count each word once as a candidate
             * reference". Together the two claim this value completely, so the target region is **not**
             * let out of sweeping -- this is exactly the plug for the permanent noSweep region in the
             * "match binding of a reference payload" hole. */
            cgLine(g, "extc_rc_storeStructRooted((void *)&(%s), sizeof(%s));",
                   nm, cType(g, s->type));
        } else {
            /* The leaves cannot be enumerated (a generic argument carrying a reference, an unrecognized kind):
             * count each word as a candidate reference, increment only and never decrement. There is no
             * source to undo from in the local tier and the root table cannot enumerate these leaves
             * either => `hostKnown = 0`, the target region drops out of sweeping (safe direction: better
             * to reclaim too little). */
            cgLine(g, "extc_rc_storeStructCons((void *)&(%s), sizeof(%s), (void *)0, 0);",
                   nm, cType(g, s->type));
        }
    }
}
/* This declaration is a **root** from this point on: the trigger point's root table has to list it
 * (a struct or fixed-size array is expanded as leaf reference slots). A global declaration is
 * registered too: it is block-scope `static`, visible in this block, and keeping it a bit longer is
 * the safe direction. */
rcRootDecl(g, s->type, nm);
/* A coroutine handle declared as a **local variable** (`var h = mk(5)`, `let h = other`) is a task
 * root for as long as the slot lives, exactly like the annotated spawn form above: the collector
 * keeps the task alive while the slot still names it. Registration is per slot, at its
 * declaration; an assignment later rewrites the same slot and needs none. A handle reaching a
 * struct field or a container is not enumerated (the same limitation as the spawn site): that
 * direction leaks a task, never frees a live one. Globals are skipped (static storage, no scope
 * exit to pop at) and so is a step function (a suspension returns before the pop). */
if (g->taskRoots && !g->coroFunc && !s->u.var.isGlobal && s->type &&
    isProtoType(s->type, "coroutine", 1))
    cgTaskRootPush(g, arenaPrintf(g->arena, "&%s.task", nm));
return;
}

