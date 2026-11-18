/* ------------------------------------------------- criterion (d): does the unit grow memory?
 *
 * The reference criteria cannot see a replaced array: `b = new u8[n]` in a loop has no reference slot
 * anywhere, so (a)(b)(c) are vacuously true. Criterion (d) is the memory half of the same question --
 * "does this unit lose something a collection would get back?" -- and it stays purely syntactic: a
 * `new T[n]` that `exNew` puts in the **function-body arena** (level 1) and that sits inside a loop.
 * Level 1 is exactly the level that outlives the iteration but not the frame, so the previous
 * generation is unreachable and unreleased until the function returns.
 *
 * Deliberately narrow (the failure direction is "false => judged idle => memory grows", so a wrong
 * "true" would only emit a runtime block nobody needs):
 *   - a loop-body declaration (`while ... { var b = new u8[n] }`) is level 2: the arena releases it at
 *     the end of the iteration, so it is not growth;
 *   - the home arena / a value handed back to the caller is not level 1: the caller names the
 *     release point;
 *   - an allocation outside a loop happens once per call and the frame's release bounds it.
 * `tests/gc-torture/src/t12_overwrite_arena.extc` is the case that turns red if this is lost. */

/** @brief Walk context of criterion (d): the current loop-nesting depth. */
typedef struct { int depth; } RcMemProbeCtx;

static bool rcMemStmtProbe(void *ctx, Stmt *s);

/** @brief Expression callback: false when a level-1 `new T[n]` sits inside a loop.
 * @param ctx  The `RcMemProbeCtx` of the walk.
 * @param e    The expression to test.
 * @return     false to stop the walk (the site was found). */
static bool rcMemExprProbe(void *ctx, Expr *e) {
    RcMemProbeCtx *c = (RcMemProbeCtx *)ctx;
    if (!e) return true;
    if (e->kind == EX_NEW && e->u.new_.count && c->depth > 0 && planArenaLevel(e) == 1)
        return false;
    AstVisit v = { rcMemExprProbe, rcMemStmtProbe, c };
    return astWalkExprChildren(e, &v);
}

/** @brief Statement callback: counts loop depth, then forwards to the generic child walk.
 * @param ctx  The `RcMemProbeCtx` of the walk.
 * @param s    The statement whose children are walked.
 * @return     false to stop the walk (a site was found below). */
static bool rcMemStmtProbe(void *ctx, Stmt *s) {
    RcMemProbeCtx *c = (RcMemProbeCtx *)ctx;
    if (!s) return true;
    bool loop = (s->kind == ST_WHILE);   /* `for` and `par` are desugared into `while` */
    if (loop) c->depth++;
    AstVisit v = { rcMemExprProbe, rcMemStmtProbe, c };
    bool ok = astWalkStmtChildren(s, &v);
    if (loop) c->depth--;
    return ok;
}

/** @brief Does this body replace a function-level array inside a loop (criterion (d))?
 * @param body  The function body; NULL (an extern) is false.
 * @return      true when such a site is present. */
static bool rcBodyGrowsMemory(Stmt *body) {
    if (!body) return false;
    RcMemProbeCtx c = { 0 };
    AstVisit v = { rcMemExprProbe, rcMemStmtProbe, &c };
    return !astWalkStmtChildren(body, &v);
}

/**
 * @brief Will this instance method's body actually be emitted?
 *
 * The same gate `emitInstanceBodies` uses. The criterion loops run **before** emission and must count
 * against the same set: a template-level use of a method (`set::shrink` calling `map::shrink`, say)
 * flips `planUsed` for the template, but no instance body is emitted unless that instance uses it --
 * counting the unemitted body would carry a whole reclamation runtime into a program that never
 * allocates (the empty-rc capability ratchet, `gate-rc-idle-corpus`).
 */
static bool rcInstMethodEmitted(Type *inst, FuncDef *md) {
    if (!md || !md->body) return false;
    if (inst->sdef && lamEnvNeedsVariants(inst->sdef)) return false;
    if (planCoroProto(md)) return false;
    if (!planUsed(md)) return false;
    if (md->owner && md->owner->typeParams.len > 0 &&
        !planInstUsed(md, inst->name) && !planDynTable(md))
        return false;
    return true;
}

/**
 * @brief Can the whole unit **skip emitting the rc runtime entirely**: every used function judged
 *        "loses no live reference"?
 *
 * The criteria (a)(b)(c) are computed by the checker (`planFnReclaimIdle`; the per-site scan is
 * `idleWalkStmt` in `check_top.c`): (a) every heap `ref` field is written at most once per object;
 * (b) a local or parameter of a ref type is never reassigned; (c) there is no reference-dropping
 * operation (container removal/replacement, `@overwrite` reusing one cell, the free-like builtins, an
 * opaque callee taking a reference). All this function does is conjoin over the whole unit -- **one
 * unsatisfied site => the whole unit is judged false**, which is the conservative direction (false
 * means the rc runtime is emitted as usual). Transitivity is therefore covered automatically: a callee
 * that drops a reference is itself an emitted function, and the conjunction sees it (an opaque callee
 * has already been judged false locally, per parameter).
 *
 * A global's initializer sits in no function body and the criteria cannot reach it => as soon as it
 * has a reclamation-governed allocation, return 0 conservatively (the same stance as
 * `unitAllAcyclic`).
 *
 * Failure direction: a wrong judgment under-collects => **memory grows**, it does not dangle (rc only
 * frees objects the markers do not reach). Safety nets: peak RSS bounded with the switch on and off on
 * the same shape, plus N vs 4N < 1.5x (safety cell 7 of `tests/reclaim-shapes` and
 * `tests/reclaim-acyclic`), and a red proof with `tools/inject.py reclaim-idle-always` forcing the
 * judgment to true.
 */
static int unitReclaimIdle(CG *g, Module *m) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        /* A function without a body (an extern declaration): the same load-bearing assumption as
         * `unitAllAcyclic` -- an extern has no body, and the references it receives are constrained by
         * the escape/borrow rules at the call site (the caller's reference outlives the call), so it
         * cannot be the place where "the last root is dropped". */
        if (f && f->body && !planFnReclaimIdle(f)) return 0;
    }
    /* The method bodies of generic instances: the template itself does not enter `g->funcs`; instance
     * bodies are emitted by `emitInstanceBodies`. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && md->body && !planFnReclaimIdle(md)) return 0;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return 0;
    }
    return 1;
}

/**
 * @brief Does any emitted body grow memory (criterion (d))?
 *
 * Same traversal as `unitReclaimIdle`: the pruned function list, then the method bodies of generic
 * instances. Global initializers sit in no function body, so they have no loop and cannot grow.
 *
 * @param g  The generator; `funcs` and `insts` are already filled in.
 * @param m  The module (unused; kept symmetric with `unitReclaimIdle`).
 * @return   true when at least one emitted body replaces a function-level array inside a loop.
 */
static int unitGrowsMemory(CG *g, Module *m) {
    (void)m;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && f->body && rcBodyGrowsMemory(f->body)) return 1;
    }
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && md->body && rcBodyGrowsMemory(md->body)) return 1;
        }
    }
    return 0;
}

/**
 * @brief Can the whole unit take the "event tier": every used function judged acyclic and references
 *        living only in counted slots?
 *
 * The criteria are computed by the checker (`planFnAcyclic`, which covers both "acyclic" and "every
 * position that can drop a reference is counted"); all this function does is conjoin over the whole
 * unit. A global's initializer sits in no function body and the criteria cannot reach it => as soon as
 * it has a reclamation-governed allocation, return 0 conservatively.
 *
 * Failure direction: a wrong judgment means "should have swept but did not" => memory grows, it does
 * not dangle => the forced tiers and the criteria are in `tests/reclaim-acyclic/` (the sweep count must
 * be 0 **and** peak RSS bounded; forcing the judgment to true on a cyclic shape must turn red).
 */
static int unitAllAcyclic(CG *g, Module *m) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        /* A function without a body (an extern declaration): `collectEmitted` pushes `planUsed` externs into
         * `g->funcs` as well, and they never have an analysis verdict => they must not turn the whole
         * unit false, or **any program that calls a C extern could not enter the event tier** (`io`'s
         * printing path pulls in `extc_cout_*`; the criterion group's success shapes are exactly what
         * this would block).
         *
         * Load-bearing assumption: an extern has no body of its own => it never writes our reference
         * slots; the references it receives are constrained by the escape/borrow rules at the **call
         * site**, and the caller's reference outlives the call => it cannot make an object in the region
         * unreachable. The assumption is recorded here, and its failure direction is "should have swept
         * but did not => memory grows" (not dangling), watched by `gate-acyclic`'s RSS bound and red
         * proof. */
        if (f && f->body && !planFnAcyclic(f)) return 0;
    }
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && md->body && !planFnAcyclic(md)) return 0;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return 0;
    }
    return 1;
}

/**
 * @brief Narrow `needReclaim`: no reclamation-governed allocation point means no runtime is emitted.
 *
 * `EXTC_RECLAIM=1` (`rcForced`) does not force emission past this proof: the proof decides whether any
 * reclamation work exists at all, and the switch only means "do not force it off". What `rcForced`
 * bypasses is the last pruning step ("no governed allocation point => no runtime"), which the
 * switch-oriented gates rely on to get a "switch on" build of a shape that allocates.
 *
 * Must run after `collectEmitted` and before `emitPreamble`/`emitRuntimePrologue`: the criterion needs
 * `g->funcs` (the pruned function list), and the runtime text enters the preamble based on this flag.
 *
 * @param g  The generator; `funcs` and `insts` are already filled in.
 * @param m  The module; global initializers are inspected from it.
 */

static void rcPlanDecide(CG *g, Module *m) {
    if (!g->rc.reclaim) {
        /* A weak reference names an rc slot, so a unit that holds one cannot do without the runtime:
         * there would be no slot to name and nothing to invalidate the handle. */
        if (g->needWeak)
            ctxError(g->ctx, 1, 1, NULL,
                     "a weak reference names a reclaimed object, so it needs the reclamation runtime"
                     " (EXTC_RECLAIM=0 turns that off)");
        return;
    }

    /* (2) "skip the rc runtime entirely": the whole unit is proved to have no reclamation work => no
     * runtime is emitted **in any tier**.
     *
     * This is a **proof**, not an optimization switch: when the proof holds, emitting the runtime would
     * just spin. So `EXTC_RECLAIM=1` means "**allow** reclamation / do not force it off", not "force
     * emission". The criteria (a)(b)(c) and the "one unsatisfied site => whole unit false" conjunction
     * are in `unitReclaimIdle`.
     *
     * Failure direction: under-collection => memory grows, it does not dangle; the safety nets are in
     * the note on `unitReclaimIdle`.
     *
     * (This is a **compile-time** early exit: it is not in the emitted runtime text, so it does not
     * belong in the "fast path and skips" table of the reclamation notes -- that table is kept aligned
     * in both directions with the runtime text alone by `tools/check_skip_table.py`.) */
    /* A handle makes the idle proof inapplicable: it is meaningful only when the object it names is an
     * rc slot, so the unit keeps the runtime even when it allocates nothing the collector would reclaim. */
    if (!g->needWeak && unitReclaimIdle(g, m)) {
        /* (2b) Criterion (d): "it loses no live **memory** either". Criteria (a)(b)(c) speak about
         * references only, and a unit whose garbage is a replaced array holds no reference at all, so
         * they are vacuous on it. Criterion (d) says: a `new T[n]` that the frame arena owns (level 1),
         * created inside a loop, survives the iteration that replaced it -- the block is released only
         * when the enclosing frame returns, so the peak grows with the round count. Such a unit keeps
         * the runtime. */
        if (!unitGrowsMemory(g, m)) {
            g->rc.reclaim = false;
            return;
        }
        g->rc.memGrowth = true;
        if (dbgOn("EXTC_DBG_IDLEMEM"))
            fprintf(stderr, "[idlemem] idle by (a)(b)(c) but replaces a frame-level array in a loop:"
                            " runtime emitted (sweep tier)\n");
    }
    /* (3) "this unit has a possible cross-site cycle" => **sharding is refused** (`EXTC_RC_SHARDS > 1`
     * degrades to one shard in this unit). Reason: shard boundaries must align with structural
     * boundaries -- cutting a cycle apart turns the cross-shard registration bits into seeds for cyclic
     * garbage, and the bound grows with the round count (`digraph` with `SHARDS=4`: 4N/N = 3.84x, the
     * same order as naive RC). Pinning only "the sites of the same cycle group" is not enough: a site
     * outside the group (a container, say) still lives in another shard, and a cross-shard reference
     * still pins the whole cycle. A single-site self-cycle (one allocation point per chain in
     * `shard_bands`) is unaffected => the pause benefit of the shard knob survives. */
    for (size_t i = 0; i < g->funcs.len && g->rc.nShard > 1; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && f->body && planFnCrossSiteCycle(f)) {
            if (dbgOn("EXTC_DBG_ZONE"))
                fprintf(stderr,
                        "[shard] unit has a cycle across sites (fn=%s); refusing to shard\n",
                        f->name ? f->name : "?");
            g->rc.nShard = 1;
        }
    }
    for (size_t i = 0; i < g->insts.len && g->rc.nShard > 1; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && planUsed(md) && planFnCrossSiteCycle(md)) { g->rc.nShard = 1; break; }
        }
    }
    /* `EXTC_RECLAIM=1` (`rcForced`) bypasses this last pruning only: the gates that compare "switch on"
     * against "switch off" (`tests/reclaim-shapes/run-switch.sh`, `tests/reclaim-perf/`) need a runtime
     * in the on build even when the shape allocates nothing the collector could reclaim. */
    if (g->rc.forced) return;
    /* A unit that holds a handle keeps the runtime even when it allocates nothing the collector
     * would reclaim: the handle names an rc slot, and there would be no slot table without the
     * runtime. This is the same keep direction as the forced switch, one line above it. */
    if (g->needWeak) return;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f && rcBodyHasGovernedAlloc(f->body)) return;
    }
    /* The method bodies of generic instances: the template itself does not enter `g->funcs` (its type
     * parameters are still there), and instance bodies are emitted by `emitInstanceBodies`, so they are
     * covered here from each instance's `sdef`. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *it = *(Type **)vecAt(&g->insts, i);
        if (!it || it->kind != TY_GENERIC || !it->sdef) continue;
        for (size_t j = 0; j < it->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&it->sdef->methods, j);
            if (md && rcInstMethodEmitted(it, md) && rcBodyHasGovernedAlloc(md->body)) return;
        }
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (gd && gd->init && !rcAllocInExpr(NULL, gd->init)) return;
    }
    g->rc.reclaim = false;
}

/**
 * @brief Initialize CG's runtime buffers and write the fixed runtime text into them.
 *
 * The text is written once here; whether it enters the final product is decided by the individual
 * `need*` flags or demand tables, e.g. `rt` is spliced only when there is printing, structural equality
 * or a descriptor line. The console buffer, the `%g` formatter and the raw-terminal restore block each
 * carry static state and must be emitted as a whole.
 *
 * @param g     The code generation context, whose buffers are initialized and pre-filled here.
 * @param arena The allocator the buffer storage comes from.
 */
static void initRuntimeText(CG *g, Arena *arena) {
    vecInit(&g->funcs, arena, sizeof(void *));
    vecInit(&g->helpers, arena, sizeof(SliceHelper));
    vecInit(&g->unwraps, arena, sizeof(UnwrapHelper));
    vecInit(&g->viewIdx, arena, sizeof(Type *));
    vecInit(&g->viewCpy, arena, sizeof(Type *));
    vecInit(&g->viewCpyU, arena, sizeof(Type *));
    vecInit(&g->descs, arena, sizeof(void *));
    vecInit(&g->eqNeed, arena, sizeof(void *));
    vecInit(&g->deadDefs, arena, sizeof(DeadDef *));
    vecInit(&g->deadFuncs, arena, sizeof(DeadFunc *));
    vecInit(&g->deadLocals, arena, sizeof(DeadLocal *));
    vecInit(&g->primEnts, arena, sizeof(PrimEnt *));
    vecInit(&g->lamThunks, arena, sizeof(StructDef *));
    vecInit(&g->lamVars, arena, sizeof(LamVar *));
    vecInit(&g->dynCalls, arena, sizeof(Expr *));
    g->lamCtx = NULL;
    bufInit(&g->desc, arena);
    bufInit(&g->rcDesc, arena);
    vecInit(&g->rcDescKeys, arena, sizeof(const char *));
    bufInit(&g->rt, arena);              /* print/compare runtime: emitted only if needed */
    bufInit(&g->rtPrint, arena);
    bufInit(&g->rtEq, arena);
    bufInit(&g->rtCout, arena);
    bufPuts(&g->rtCout,
        "/* Buffered console output (ruling 90). The buffer lives in the runtime rather than\n"
        " * in the library so that the flush has a home the generator can call; the library\n"
        " * formats and calls `extc_cout_put`, which the C compiler inlines into it (same\n"
        " * translation unit, small body). */\n"
        "static uint8_t extc_cout_buf[1 << 18];\n"
        "static int64_t extc_cout_len = 0;\n"
        /* Prototypes ahead of the definitions: these two have external linkage on purpose (the
         * library reaches them through `extern!`, so they cannot be `static`), and clang asks for a
         * declaration in view before the definition (`-Wmissing-prototypes`). */
        "void extc_cout_flush(void);\n"
        "void extc_cout_put(uint8_t *p, int64_t n);\n"
        "void extc_cout_f64(double v);\n"
        "void extc_cout_flush(void) {\n"
        "    if (extc_cout_len <= 0) return;\n"
        "    /* Through stdio: `unistd.h` cannot be included here (the library declares\n"
        "     * `read`/`write` with its own prototypes, and the two clash). stdio also keeps\n"
        "     * the order with the deprecated print path and flushes at exit by itself. */\n"
        "    fwrite(extc_cout_buf, 1, (size_t)extc_cout_len, stdout);\n"
        "    /* `fwrite` alone only fills stdio's own buffer, which stays invisible while stdout is\n"
        "     * a file or a pipe (fully buffered) until the process exits: a program that prints,\n"
        "     * calls `io::flushCout` and then keeps running would show nothing to a reader. */\n"
        "    fflush(stdout);\n"
        "    extc_cout_len = 0;\n"
        "}\n"
        "void extc_cout_put(uint8_t *p, int64_t n) {\n"
        "    if (n <= 0) return;\n"
        "    /* A single write **larger than the whole buffer** goes straight out, after the buffer\n"
        "     * is drained so the order is kept. Without this the memcpy below runs past the end of\n"
        "     * `extc_cout_buf`: `io::cout << <300 KB slice>` aborts with glibc's\n"
        "     * \"buffer overflow detected\" (ASan reports a global-buffer-overflow WRITE of the\n"
        "     * whole length). Flushing only when the buffer is *nearly* full does nothing for a\n"
        "     * write bigger than the buffer itself; this branch takes a single write of a whole\n"
        "     * buffer (256 KiB) or more. */\n"
        "    if (n >= (int64_t)sizeof extc_cout_buf) {\n"
        "        extc_cout_flush();\n"
        "        fwrite(p, 1, (size_t)n, stdout);\n"
        "        fflush(stdout);\n"
        "        return;\n"
        "    }\n"
        "    if (extc_cout_len + n > (int64_t)sizeof extc_cout_buf) extc_cout_flush();\n"
        "    memcpy(extc_cout_buf + extc_cout_len, p, (size_t)n);\n"
        "    extc_cout_len += n;\n"
        "}\n"
        /* A `printf` stand-in: it **appends the formatted result to the same cout buffer**, so built-in
         * `print`/`println`, descriptor printing and `io::cout` have one output path and one buffer, and
         * mixed writes are ordered by construction (no `fflush` needed). extC itself has no variadics,
         * but the generated C can: this is purely an internal detail of the compiler. */
        "void extc_printf(const char *fmt, ...) {\n"
        "    char b[256];\n"
        "    va_list ap;\n"
        "    int n;\n"
        "    va_start(ap, fmt);\n"
        "    n = vsnprintf(b, sizeof b, fmt, ap);\n"
        "    va_end(ap);\n"
        "    if (n <= 0) return;\n"
        "    if ((size_t)n > sizeof b - 1) n = (int)(sizeof b - 1);\n"
        "    extc_cout_put((uint8_t *)b, (int64_t)n);\n"
        "}\n");
    /* The one formatter the library cannot write itself: extC has no variadics,
     * so `snprintf` is unreachable and this is the fixed-arity door to it. `%g`
     * fits `char[32]` for every double, and `snprintf` truncates rather than
     * overruns if it does not.
     *
     * It lives in a buffer of its own because it is appended only when a call
     * site reaches it (`needCoutF64`): a program that never prints a float
     * should not carry the formatter. */
    bufInit(&g->rtCoutF64, arena);
    bufPuts(&g->rtCoutF64,
        "void extc_cout_f64(double v) {\n"
        "    char b[32];\n"
        "    int n = snprintf(b, sizeof b, \"%g\", v);\n"
        "    if (n > 0) extc_cout_put((uint8_t *)b, (int64_t)n);\n"
        "}\n");
    bufInit(&g->rtRaw, arena);
    bufPuts(&g->rtRaw,
        "/* ---- raw terminal: give it back even when the program dies ----\n"
        " * The library turns raw mode on through `extc_raw_enter`, handing over a copy of\n"
        " * the settings it found. The trap path then restores them through the dying hook\n"
        " * above: a program that traps in raw mode still issues\n"
        " * the `TCSETS` that puts the terminal back before it exits.\n"
        " * The buffer is opaque bytes; the layout of `struct termios` is never named, and\n"
        " * `cfmakeraw` on the library side is what decides what raw means. */\n"
        "static int32_t __extc_raw_fd = -1;\n"
        "static uint8_t __extc_raw_saved[256];\n"
        "static int32_t __extc_raw_on;\n"
        "extern int32_t tcsetattr(int32_t fd, int32_t action, uint8_t *buf);\n"
        "int32_t extc_raw_leave(void) {\n"
        "    int32_t r;\n"
        "    if (!__extc_raw_on) return 0;\n"
        "    __extc_raw_on = 0;\n"
        "    if (__extc_raw_fd < 0) return 0;\n"
        "    r = tcsetattr(__extc_raw_fd, 0 /* TCSANOW */, __extc_raw_saved);\n"
        "    __extc_raw_fd = -1;\n"
        "    return r;\n"
        "}\n"
        "void extc_raw_enter(int32_t fd, uint8_t *buf, int64_t n) {\n"
        "    int64_t i;\n"
        "    if (n > (int64_t)sizeof __extc_raw_saved) n = (int64_t)sizeof __extc_raw_saved;\n"
        "    for (i = 0; i < n; i++) __extc_raw_saved[i] = buf[i];\n"
        "    __extc_raw_fd = fd;\n"
        "    __extc_raw_on = 1;\n"
        "    __extc_dying  = extc_raw_leave;   /* if we die before restore(), put it back */\n"
        "}\n");
    bufInit(&g->body, arena);
    bufInit(&g->coroDefs, arena);    /* appended last: see `CG.coroDefs` */
    g->tmpSeq = 0;
}

/** @brief Expression callback: will this program use the console buffer (built-in printing or
 * `io::cout`)? */
static bool coutScanExpr(void *ctx, Expr *e) {
    bool *hit = ctx;
    if (!e) return true;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (n && (builtinNameIs(n, "print") || builtinNameIs(n, "println") ||
                  builtinNameIs(n, "flush")))
            *hit = true;
    }
    FuncDef *callee = planCallee(e);
    if (callee && callee->name && strstr(callee->name, "ostream_shl")) *hit = true;
    return true;
}

/** @brief Statement callback: check the statement itself first, then let the generic walk descend
 * into its children. */
static bool coutScanStmt(void *ctx, Stmt *s) {
    bool *hit = ctx;
    if (!s || *hit) return !*hit;
    AstVisit v = { coutScanExpr, coutScanStmt, ctx };
    return astWalkStmtChildren(s, &v) || !*hit;
}

/**
 * @brief Decide in advance whether the program writes to the console buffer.
 *
 * main's prologue registers `atexit(extc_cout_flush)` only when `needCout` is true **at that moment**,
 * and `needCout` is normally set while statements are being emitted (`println(...)`, `io::cout <<`). A
 * program that `exit(7)`s halfway or hits a trap would therefore lose the bytes in the buffer. This
 * scans the function bodies that are about to be emitted before any body is generated, turning the
 * switch on early.
 *
 * @param g  The code generation state; the scan result only writes `g->needCout`.
 */
static void scanConsoleUse(CG *g) {
    bool hit = false;
    for (size_t i = 0; i < g->funcs.len && !hit; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (!f || !f->body) continue;
        AstVisit v = { coutScanExpr, coutScanStmt, &hit };
        astWalkStmtChildren(f->body, &v);
    }
    if (hit) g->needCout = true;
}

/**
 * @brief Filter out, by reachability pruning, the structs, methods and free functions that really get
 *        emitted.
 *
 * The criteria are the `used` bits the checker set at call sites, plus a few roots: `main`, `@export`
 * and the members of trait tables. Three kinds are not emitted: a generic template itself (with `T`
 * unsubstituted it generates C that does not compile), a provisional instance whose signature still
 * mentions a type parameter, and the `next`/`value` protocol functions of a coroutine (they are inlined
 * at the call site).
 *
 * @param g  The code generation context; the filtering result is pushed into `structs` and `funcs`.
 * @param m  The current module; `structs` and `funcs` are the candidates.
 */

static void collectEmitted(CG *g, Module *m) {
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (sd->typeParams.len > 0) continue;   /* generic: generated per instance */
        /* A synthetic holder for `impl i64 { ... }` has no fields and no layout,
         * so it is no C struct; its methods are emitted by the loop below like
         * any other function. */
        if (!planBuiltinHolder(sd)) *(StructDef **)vecPush(&g->structs) = sd;
        /* Methods are functions too and share the prototype and definition table. */
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&sd->methods, j);
            if (planCoroProto(md)) continue;      /* emitted inline at the call site, not here */
            /* Reachability pruning: a method nobody calls is not emitted. The
             * checker already marks every call site with `planUsed`, so a small
             * program does not drag in the whole prelude and library. */
            if ((!planUsed(md)) && !inTraitTable(m, md)) continue;
            *(FuncDef **)vecPush(&g->funcs) = md;
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* The generic template itself is not generated: `T` is still a
         * parameter, so the result would not compile. Only the instances of a
         * template (`planTemplate(f) != NULL`) are emitted. */
        if (f->typeParams.len > 0) continue;
        /* Nor is an instance whose signature still mentions a type parameter.
         * A generic call inside a generic body builds one (`firstOr_T` with `T`
         * as the argument) so the template body can be checked at all; every
         * real call site is repointed to a concrete instance at instantiation,
         * and the provisional one has parameter and return types with no C name. */
        if (planTemplate(f) && funcSignatureMentionsParam(f)) continue;
        /* Free functions are pruned the same way; `main` is always kept, and
         * `@export` is a root too because nothing on the extC side names it. */
        if (!planUsed(f) && !cgIsMain(f) && !f->isModuleEntry && !f->isExport && !inTraitTable(m, f)) continue;
        /* The coroutine protocols (`next`/`value`) are emitted inline at their call sites and have no
         * body of their own -- emitting one here hit genBlockBody with a null body. */
        if (planCoroProto(f)) continue;
        *(FuncDef **)vecPush(&g->funcs) = f;
    }
}

/**
 * @brief Emit the method bodies of generic instances and register each piece in the
 *        unreferenced-pruning table.
 *
 * Only called methods are emitted (`planUsed`); `planEnterInst` tells the plan layer which instance is
 * being emitted, because one call site inside a generic body is shared by all instances. A method body
 * is always registered: the prototype loop has already created an entry carrying `proto`, and
 * `deadFuncBody` only fills in `off`/`len`; without the body, `markUnusedParams` would skip it, and a
 * method that ignores `self` would then fail to compile under `-Werror=unused-parameter`.
 *
 * @param g  The code generation context; `insts` holds the instances to emit and `out` receives the
 *           bodies.
 */
static void emitInstanceBodies(CG *g) {
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (inst->kind != TY_GENERIC) continue;
        /* A lambda environment laid out per instance: `emitLamVars` emits its `call` under the
         * instance's own name (`lamCtx`), which this loop does not set up. */
        if (inst->sdef && lamEnvNeedsVariants(inst->sdef)) continue;
        SubCtx sv = substEnter(g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (planCoroProto(md)) continue;   /* emitted inline at the call site */
            if (!planUsed(md)) continue;      /* called methods only */
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;   /* no other instance has used it: its element type may not have the operator this method needs */

            planEnterInst(inst);          /* a method body is shared by every type instance */
            /* Name the instance for the lambda-environment layout code: a literal whose type depends
             * on a type parameter is this instance's layout (`LamVar`). */
            const char *savedLamCtx = g->lamCtx;
            g->lamCtx = inst->name;
            size_t fb = g->out->len;
            CGACC("cg-emit", genFunc(g, md));
            g->lamCtx = savedLamCtx;
            planLeaveInst();
            /* A method named by a `dyn` table's thunk is referenced from outside
             * its own scope, and the "definitions nothing names" pass cannot see
             * that reference, so it has to be registered. */
            /* Always register the body: the prototype loop above already created this entry (with
             * `proto`), and `deadFuncBody` only fills `off`/`len` on it. An entry left without a
             * body makes `markUnusedParams` skip it, so a method that ignores `self` makes the
             * generated C fail `-Werror=unused-parameter`. */
            deadFuncBody(g, md, fb, g->out->len - fb);
            cgLine(g, "");
        }
        substLeave(g, sv);
    }
}

/**
 * @brief Emit a uniform method table for every trait the checker has seen in `dyn` form.
 *
 * The receiver is erased to `void *`, which is what lets a stored `dyn` value be called through the
 * table: the call site does not know the concrete type. One thunk adapting the implementation is
 * generated per (trait, type, method), and the table name is built from the concrete type's `cType`, so
 * each instance of a generic target gets its own table. A slot that cannot be dispatched (no receiver,
 * generic, returns `Self`) keeps its place (the order is the ABI), with type `void *` and NULL, and the
 * checker never dispatches from it.
 * The declaration goes into `vtDecls` and is emitted with the prototypes (a dispatch point needs the
 * struct in scope); the definition stays in `vtDefs` and is appended by `finishUnit` after every
 * byte-offset-rewriting pass.
 *
 * @param g     The code generation context; `vtDecls`/`vtDefs` are the output buffers.
 * @param out   The unit text; this function appends `vtDecls` to its end.
 * @param m     The current module, which provides `traits` and `impls`.
 * @param arena The allocator for assembling thunk names and declaration text.
 */
static void emitVtTables(CG *g, Buf *out, Module *m, Arena *arena) {
    /* Uniform method tables, one per trait. The receiver is erased to `void *`,
     * which is what lets a stored `dyn` value call through a table at all: its
     * concrete type is not known at the call site. Each (trait, type, method)
     * gets a thunk adapting the implementation to the uniform signature, and the
     * instance keeps the stable key `extc_vt$Trait$Type`.
     *
     * Only traits the checker saw in a `dyn` form (`usedDyn`) get a table. A
     * method that cannot be dispatched (no receiver, generic, or returning
     * `Self`) keeps its slot -- order is ABI -- but gets a `void *` type and a
     * NULL entry, which the checker refuses to dispatch through. */
    bufInit(&g->vtDefs, arena);
    bufInit(&g->vtDecls, arena);
    for (size_t ti = 0; ti < m->traits.len; ti++) {
        TraitDef *tr = *(TraitDef **)vecAt(&m->traits, ti);
        if (!planUsedDyn(tr)) continue;
        bufPrintf(&g->vtDecls, "struct extc_vt$%s_t {", tr->name);
        for (size_t k = 0; k < tr->methods.len; k++) {
            FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
            bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
            bufPrintf(&g->vtDecls, " %s (*%s)(void *",
                      unsafe ? "void *" : (want->ret ? cType(g, ttBase(want->ret)) : "void"),
                      want->name);
            for (size_t pi = 1; pi < want->params.len; pi++)
                bufPrintf(&g->vtDecls, ", %s",
                          cType(g, ttBase((*(Param **)vecAt(&want->params, pi))->type)));
            bufPuts(&g->vtDecls, ");");
        }
        bufPuts(&g->vtDecls, " };\n");
        for (size_t i = 0; i < m->impls.len; i++) {
            ImplDef *im = *(ImplDef **)vecAt(&m->impls, i);
            if (planImplTrait(im) != tr || !planImplTarget(im)) continue;
            Type *bt = ttBase(planImplTarget(im));
            /* The methods of `bt` live in **its own** method set: `Type.mholder`
             * when the type has no body of its own -- a builtin scalar
             * (`impl Codec for i64`) or a generic instance (`impl Codec for
             * slice<u8>`) -- and `sdef` otherwise. */
            StructDef *tsd = bt ? (bt->mholder ? bt->mholder : bt->sdef) : NULL;
            if (!tsd) continue;
            /* The **same** derivation the construction and dispatch sites use
             * (`cType`): the table is named after the concrete type
             * (`extc_vt$Codec$slice_u8`), not after `sdef->name`. */
            /* A generic target is spelled once per instance, so
             * `impl<T> Tag for pair<T>` needs `extc_vt$Tag$pair_i64` for every
             * used instance of `pair`; emitting the thunk inside `substEnter` is
             * also what makes the body pool name `pair_i64_tag` and keeps that
             * body alive. */
            Vec spellings;
            vecInit(&spellings, arena, sizeof(Type *));
            if (bt->kind == TY_GENERIC && bt->sdef) {
                for (size_t ii = 0; ii < g->insts.len; ii++) {
                    Type *cand = *(Type **)vecAt(&g->insts, ii);
                    if (cand->kind == TY_GENERIC && cand->sdef == bt->sdef)
                        *(Type **)vecPush(&spellings) = cand;
                }
            } else {
                *(Type **)vecPush(&spellings) = bt;
            }
            for (size_t vi = 0; vi < spellings.len; vi++) {
            Type *vb = *(Type **)vecAt(&spellings, vi);
            StructDef *vsd = (vb == bt) ? tsd : (vb->mholder ? vb->mholder : vb->sdef);
            if (!vsd) continue;
            SubCtx vsv = {0};
            if (vb != bt) vsv = substEnter(g, vb);
            const char *vtKey = cType(g, vb);
            bufPrintf(&g->vtDecls, "static const struct extc_vt$%s_t extc_vt$%s$%s;\n",
                      tr->name, tr->name, vtKey);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                FuncDef *have = NULL;
                for (size_t j = 0; j < vsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&vsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                if (!have) continue;                 /* completeness is the checker's business */
                if (!funcIsMethod(want) || (want->ret && mentionsParam(want->ret))) continue;
                bufPrintf(&g->vtDefs, "static %s extc_th$%s$%s$%s(void *self",
                          want->ret ? cType(g, ttBase(want->ret)) : "void",
                          tr->name, vtKey, want->name);
                for (size_t pi = 1; pi < want->params.len; pi++) {
                    Param *pp = *(Param **)vecAt(&want->params, pi);
                    bufPrintf(&g->vtDefs, ", %s %s", cType(g, ttBase(pp->type)), pp->name);
                }
                bufPrintf(&g->vtDefs, ") { %s%s((%s *)self", want->ret ? "return " : "",
                          cFuncName(g, have), cType(g, vb));
                for (size_t pi = 1; pi < want->params.len; pi++)
                    bufPrintf(&g->vtDefs, ", %s", (*(Param **)vecAt(&want->params, pi))->name);
                bufPuts(&g->vtDefs, "); }\n");
            }
            bufPrintf(&g->vtDefs, "static const struct extc_vt$%s_t __attribute__((unused)) "
                               "extc_vt$%s$%s = {", tr->name, tr->name, vtKey);
            for (size_t k = 0; k < tr->methods.len; k++) {
                FuncDef *want = *(FuncDef **)vecAt(&tr->methods, k);
                bool unsafe = funcIsMethod(want) == false || (want->ret && mentionsParam(want->ret));
                FuncDef *have = NULL;
                for (size_t j = 0; j < vsd->methods.len && !have; j++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&vsd->methods, j);
                    if (strcmp(cand->name, want->name) == 0) have = cand;
                }
                bufPrintf(&g->vtDefs, "%s %s", k ? "," : "",
                          (unsafe || !have) ? "NULL"
                                            : arenaPrintf(arena, "extc_th$%s$%s$%s",
                                                          tr->name, vtKey, want->name));
            }
            bufPuts(&g->vtDefs, " };\n");
            if (vb != bt) substLeave(g, vsv);
            }   /* one table per concrete spelling */
        }
    }
    bufPuts(out, bufCstr(&g->vtDecls));
}

/**
 * @brief Emit the trampoline and prototype of a `parallel::run` worker, and set `needPar`.
 *
 * The position is the point of this section: the product's order is includes -> prototypes -> runtime
 * -> function bodies, and a trampoline must both precede the declaration at the call site and follow
 * the worker's prototype, so the start of the body loop is the only correct place. The worker's
 * prototype is emitted once more by the same `genFuncProto`, and a repeated declaration is legal and
 * textually identical.
 *
 * @param g  The code generation context; every function in `funcs` for which `planIsParWorker` is true
 *           gets one trampoline.
 */
static void emitParTramps(CG *g) {
    /* A `parallel::run` trampoline must land **before any function body**: the call site must see its
     * declaration and it must see the worker's prototype. The product's order is includes -> prototypes
     * -> runtime -> function bodies, so the start of the body loop is the only correct place; the worker
     * prototype is emitted once more by the same genFuncProto, and a repeated declaration is legal and
     * textually identical. */
    {
        bool parAny = false;
        for (size_t i = 0; i < g->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
            if (!planIsParWorker(f)) continue;
            if (!parAny) {
                cgLine(g, "/* ---- parallel::run trampolines (stdlib/std/parallel.extc) ---- */");
                parAny = true;
            }
            genFuncProto(g, f);
            genParTramp(g, f);
            g->needPar = true;               /* the two pthread prototypes only go out when extc_par_run does */
        }
        if (parAny) cgLine(g, "");
    }
}

/**
 * @brief Emit the body of every non-extern function in `funcs`.
 *
 * Before emitting, `substEnterFunc` (an instance needs substitution) and `planEnterFunc` (what a call
 * site inside a generic body resolves to is decided by the current instance) run; afterwards the byte
 * range is handed to `deadFuncBody` for registration.
 *
 * @param g  The code generation context; `funcs` has been filtered by `collectEmitted` and `out`
 *           receives the text.
 */
static void emitFuncBodies(CG *g) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f->isExtern) continue;              /* an external declaration has no body */
        SubCtx sv = substEnterFunc(g, f);      /* instances need substitution */
        /* Tell the plan which instance is being emitted: a call site inside a generic
         * body is shared by every instance, and what it calls depends on this. */
        planEnterFunc(f);
        /* A generic function instance may contain a lambda whose environment depends on the
         * instance's type arguments (`LamVar`), so the body is generated under its name. */
        const char *savedLamCtx = g->lamCtx;
        g->lamCtx = planTemplate(f) ? planInstName(f) : NULL;
        size_t fb = g->out->len;
        CGACC("cg-emit", genFunc(g, f));
        g->lamCtx = savedLamCtx;
        deadFuncBody(g, f, fb, g->out->len - fb);
        planLeaveFunc();
        substLeave(g, sv);
        cgLine(g, "");
    }
    /* the dispatcher is written at the end of g->body: finishUnit concatenates the body first and prunes
     * afterwards, so the table's references to the entries still count */
    if (g->needModuleTramp) emitModuleTramp(g);
}

/**
 * @brief Splice the coroutine handle definitions, the pool runtime and the coroutine runtime into the
 *        unit text's coroutine section.
 *
 * It acts only when a handle is needed, the coroutine section exists and it is not fully spliced yet:
 * the text before `coroDefA` is taken, the handle structs, the pool runtime (when not emitted yet) and
 * the coroutine runtime are inserted in order, and the part after `coroDefA` is appended. The spliced
 * text is longer, so the whole `Buf` is reassembled; it cannot be written back in place into
 * `out->data`.
 *
 * @param g     The code generation context; `needCoroHandle`/`coroDefA`/`coroDefPrinted`/
 *              `taskTableDone` decide what is spliced, and `poolDone` and `needPool` are updated in
 *              place.
 * @param out   The unit text, replaced as a whole by the splice result.
 * @param arena The allocator the splice's temporary buffer is allocated from.
 */
static void assembleCoroutineRuntime(CG *g, Buf *out, Arena *arena) {
    if (g->needCoroHandle && g->coroDefA && (!g->coroDefPrinted || !g->taskTableDone)) {
        Buf nb;
        bufInit(&nb, arena);
        bufPutn(&nb, out->data, g->coroDefA);
        if (!g->coroDefPrinted)
            bufPuts(&nb,
                    "#define EXTC_CORO_HANDLE_DEFINED 1\n"
                    "struct ExtcCoroS { void *frame; int64_t kind; int64_t task; };\n"
                    "typedef struct ExtcCoroS extc_coro;\n\n");
        if (!g->taskTableDone) {
            if (!g->poolDone) {
                Buf pr;
                bufInit(&pr, arena);
                placesEmitRuntime(arena, &pr);
                bufPuts(&nb, bufCstr(&pr));
                g->poolDone = true;
            }
            Buf rt;
            bufInit(&rt, arena);
            coroutineEmitRuntime(arena, &rt);
            bufPuts(&nb, bufCstr(&rt));
            g->needPool = true;
        }
        bufPutn(&nb, out->data + g->coroDefA, out->len - g->coroDefA);
        /* Rebuild through `Buf` instead of `memcpy`-ing into `out->data`: the
         * spliced text makes the unit longer, so writing it in place runs past
         * the buffer's end. */
        *out = nb;
    }
}

/**
 * @brief Append the parallel runtime text of `par` to the end of the unit on demand.
 *
 * The language has no function pointers, so the callbacks come from generated static trampolines, and
 * this text only knows plain C function pointers. Ranges are distributed by an atomic chunk counter, so
 * they never overlap; each worker gets its own arena (`extc_tls_arena`), so a region has exactly one
 * owner. With `withRegion` set a region is opened, and the next run closes the previous one. The
 * workers are a resident pool: the first run starts them, and every later run hands a job to a slot
 * (a sequence number in, the same number back) instead of creating a thread, which turns the fixed
 * cost of a run from tens of microseconds into a store and a wake. A thread that cannot be started is
 * not an error: the range is finished by fewer workers, and a task that reaches `par` from inside the
 * pool runs the range inline rather than waiting on the pool that is running it. The pool is joined
 * at exit, so a leak checker sees no parked threads.
 *
 * @param g    The code generation context; `needParRegion` decides whether `EXTC_PAR_REGION` is
 *             emitted, and `needPar` (or the `EXTC_DBG_PAR` environment variable) decides whether the
 *             whole runtime is.
 * @param out  The unit text the runtime text is appended to.
 */
static void emitParallelRuntime(CG *g, Buf *out) {
    if (g->needParRegion) bufPuts(out, "#define EXTC_PAR_REGION 1\n");
    if (g->needPar || getenv("EXTC_DBG_PAR"))
    {
        bufPuts(out,
            "#include <pthread.h>\n"
            "\n"
            "typedef struct extc_par_job {\n"
            "    int64_t (*fn)(int64_t id, int64_t lo, int64_t hi, void *ctx);\n"
            "    void   *ctx;\n"
            "    int64_t n, chunk, next, first_err;\n"
            "    void   *region;   /* extc_par_region*, or NULL when no worker allocates */\n"
            "} extc_par_job;\n"
            "\n"
            "/* Where `new` inside a worker takes its memory from; NULL outside a worker, so the\n"
            " * checker can say so loudly. */\n"
            "_Thread_local extc_arena *extc_tls_arena = NULL;\n"
            "\n"
            "#ifdef EXTC_PAR_REGION\n"
            "/* Region-shared arena for one parallel run: one arena per worker, owned by the\n"
            " * region -- the parent reads worker allocations after the join. */\n"
            "typedef struct extc_par_region {\n"
            "    int64_t     workers;\n"
            "    extc_arena *arenas;\n"
            "} extc_par_region;\n"
            "/* The region the last parallel run in this thread produced into: the caller reads\n"
            " * worker allocations through it after the run. Closed by the next run (or never). */\n"
            "_Thread_local extc_par_region *extc_tls_region = NULL;\n"
            "\n"
            "EXTC_UNUSED static extc_par_region *extc_par_open(int64_t workers) {\n"
            "    if (workers <= 0) workers = 1;\n"
            "    extc_par_region *r = (extc_par_region *)calloc(1, sizeof *r\n"
            "        + (size_t)workers * sizeof(extc_arena));\n"
            "    if (!r) return NULL;\n"
            "    r->workers = workers;\n"
            "    r->arenas  = (extc_arena *)(r + 1);\n"
            "    for (int64_t i = 0; i < workers; i++) extc_arena_init(&r->arenas[i]);\n"
            "    return r;\n"
            "}\n"
            "\n"
            "/* Release: hand every worker arena back. */\n"
            "EXTC_UNUSED static void extc_par_close(extc_par_region *r) {\n"
            "    if (!r) return;\n"
            "    for (int64_t i = 0; i < r->workers; i++) {\n");
        if (g->rc.reclaim)
            bufPuts(out,
            "        extc_rc_regionDrop((void *)&r->arenas[i]);\n");
        bufPuts(out,
            "        extc_arena_destroy(&r->arenas[i]);\n"
            "    }\n"
            "    free(r);\n"
            "}\n"
            "#endif\n"
            "\n"
            "/* One job, run by whichever thread picks it up: the caller runs id 0 itself and the\n"
            " * pool threads take the other ids. The chunk counter is shared, so the split does not\n"
            " * depend on who starts first. */\n"
            "static void extc_par_body(extc_par_job *j, int64_t id) {\n"
            "    extc_arena local;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    void *r = j->region;\n"
            "    if (r)   /* the parent reads it after the join, so it is not destroyed here */\n"
            "        extc_tls_arena = &((extc_par_region *)r)->arenas[id];\n"
            "    else\n"
            "#endif\n"
            "    { extc_arena_init(&local); extc_tls_arena = &local; }\n"
            "    for (;;) {\n"
            "        int64_t k = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);\n"
            "        int64_t lo = k * j->chunk;\n"
            "        if (lo >= j->n) break;\n"
            "        int64_t hi = lo + j->chunk;\n"
            "        if (hi > j->n) hi = j->n;\n");
        if (g->rc.reclaim)
            bufPuts(out,
            "        /* Worker entry: the only caller is the C runtime. When the job runs inline on the\n"
            "         * same thread (threads <= 1, or a thread could not be started) the outer frame's\n"
            "         * depth is still in TLS; clearing it here makes the entry frame the owner, and every\n"
            "         * region it allocates is new in this round, so no outer frame can hold a reference\n"
            "         * into it. */\n"
            "        int32_t __extc_depth = extc_rc_tlsDepth;\n"
            "        extc_rc_tlsDepth = 0;\n");
        bufPuts(out,
            "        int64_t rc = j->fn(id, lo, hi, j->ctx);\n");
        if (g->rc.reclaim)
            bufPuts(out,
            "        extc_rc_tlsDepth = __extc_depth;\n");
        bufPuts(out,
            "        if (rc != 0 && __atomic_load_n(&j->first_err, __ATOMIC_RELAXED) == 0)\n"
            "            __atomic_store_n(&j->first_err, rc, __ATOMIC_RELAXED);\n"
            "    }\n"
            "    extc_tls_arena = NULL;\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    if (!r) {\n"
            "#else\n"
            "    {\n"
            "#endif\n");
        if (g->rc.reclaim)
            bufPuts(out,
            "        extc_rc_regionDrop((void *)&local);   /* the reclamation region lives as long as this arena */\n");
        bufPuts(out,
            "        extc_arena_destroy(&local);   /* a region's arenas are the parent's to free */\n"
            "    }\n"
            "}\n"
            "\n"
            "/* A resident pool: one thread per slot, reused by every `par` run. A slot belongs to one\n"
            " * thread, and the caller publishes a job there with a sequence number; the thread runs\n"
            " * it and publishes the same number back, so handing work over needs no lock. The lock\n"
            " * and the condition variable only park a thread that has been idle for a while. */\n"
            "#define EXTC_POOL_SPIN 20000\n"
            "#if defined(__x86_64__) || defined(__i386__)\n"
            "#  define EXTC_POOL_PAUSE() __builtin_ia32_pause()\n"
            "#elif defined(__aarch64__) || defined(__arm__)\n"
            "#  define EXTC_POOL_PAUSE() __asm__ __volatile__(\"yield\")\n"
            "#else\n"
            "#  define EXTC_POOL_PAUSE() ((void)0)\n"
            "#endif\n"
            "\n"
            "typedef struct extc_pool_slot {\n"
            "    pthread_t       th;\n"
            "    int64_t         id;\n"
            "    extc_par_job   *job;\n"
            "    _Atomic int64_t seq;    /* caller -> thread */\n"
            "    _Atomic int64_t done;   /* thread -> caller */\n"
            "    _Atomic int64_t stop;\n"
            "} extc_pool_slot;\n"
            "\n"
            "static pthread_mutex_t extc_pool_mu = PTHREAD_MUTEX_INITIALIZER;\n"
            "static pthread_cond_t  extc_pool_cv = PTHREAD_COND_INITIALIZER;\n"
            "static extc_pool_slot *extc_pool = NULL;\n"
            "static int64_t         extc_pool_n = 0;       /* threads actually created */\n"
            "static int64_t         extc_pool_seq = 0;     /* last job handed out */\n"
            "static int             extc_pool_started = 0;\n"
            "static _Atomic int     extc_pool_waiters = 0;\n"
            "static _Atomic int     extc_pool_parked = 0;    /* threads asleep on the condition variable */\n"
            "/* Nonzero on a pool thread: a task that itself reaches `par` must not wait on the pool\n"
            " * that is running it, so it runs the range inline instead. */\n"
            "_Thread_local int extc_pool_inside = 0;\n"
            "\n"
            "static void extc_pool_stop(void);\n"
            "\n"
            "EXTC_UNUSED static void *extc_pool_worker(void *p) {\n"
            "    extc_pool_slot *s = (extc_pool_slot *)p;\n"
            "    int64_t seen = 0, spin;\n"
            "    extc_pool_inside = 1;\n"
            "    for (;;) {\n"
            "        int64_t v = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);\n"
            "        if (v == seen) {\n"
            "            spin = 0;\n"
            "            for (;;) {\n"
            "                EXTC_POOL_PAUSE();\n"
            "                if (__atomic_load_n(&s->stop, __ATOMIC_RELAXED)) return NULL;\n");
        bufPuts(out,
            "                v = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);\n"
            "                if (v != seen) break;\n"
            "                if (++spin < EXTC_POOL_SPIN) continue;\n"
            "                /* Idle long enough: sleep until the caller publishes again. */\n"
            "                __atomic_fetch_add(&extc_pool_parked, 1, __ATOMIC_RELAXED);\n"
            "                pthread_mutex_lock(&extc_pool_mu);\n"
            "                while ((v = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE)) == seen &&\n"
            "                       !__atomic_load_n(&s->stop, __ATOMIC_RELAXED))\n"
            "                    pthread_cond_wait(&extc_pool_cv, &extc_pool_mu);\n"
            "                pthread_mutex_unlock(&extc_pool_mu);\n"
            "                __atomic_fetch_sub(&extc_pool_parked, 1, __ATOMIC_RELAXED);\n"
            "                if (__atomic_load_n(&s->stop, __ATOMIC_RELAXED)) return NULL;\n"
            "                break;\n"
            "            }\n"
            "        }\n"
            "        seen = v;\n"
            "        extc_par_body(s->job, s->id);\n"
            "        __atomic_store_n(&s->done, v, __ATOMIC_RELEASE);\n"
            "        if (__atomic_load_n(&extc_pool_waiters, __ATOMIC_RELAXED) > 0) {\n"
            "            pthread_mutex_lock(&extc_pool_mu);\n"
            "            pthread_cond_broadcast(&extc_pool_cv);\n"
            "            pthread_mutex_unlock(&extc_pool_mu);\n"
            "        }\n"
            "    }\n"
            "}\n"
            "\n"
            "/* Start `want - 1` threads the first time a program asks for them; a thread that\n"
            " * cannot be started leaves the pool smaller, which is not an error. */\n"
            "static void extc_pool_start(int64_t want) {\n"
            "    int64_t i;\n"
            "    if (extc_pool_started) return;\n"
            "    extc_pool_started = 1;\n"
            "    if (want > 257) want = 257;\n"
            "    if (want < 2) return;\n"
            "    extc_pool = (extc_pool_slot *)calloc((size_t)(want - 1), sizeof *extc_pool);\n"
            "    if (!extc_pool) return;\n"
            "    for (i = 0; i + 1 < want; i++) {\n"
            "        extc_pool[i].id = i + 1;\n"
            "        if (pthread_create(&extc_pool[i].th, NULL, extc_pool_worker,\n"
            "                           (void *)&extc_pool[i]) != 0)\n"
            "            break;\n"
            "        extc_pool_n = i + 1;\n"
            "    }\n"
            "    atexit(extc_pool_stop);\n"
            "}\n"
            "\n"
            "/* Give the threads back at exit, so a leak checker and a profiler see a clean\n"
            " * shutdown instead of threads that are still parked. */\n"
            "static void extc_pool_stop(void) {\n"
            "    int64_t i;\n"
            "    if (!extc_pool) return;\n"
            "    for (i = 0; i < extc_pool_n; i++)\n");
        bufPuts(out,
            "        __atomic_store_n(&extc_pool[i].stop, 1, __ATOMIC_RELAXED);\n"
            "    pthread_mutex_lock(&extc_pool_mu);\n"
            "    pthread_cond_broadcast(&extc_pool_cv);\n"
            "    pthread_mutex_unlock(&extc_pool_mu);\n"
            "    for (i = 0; i < extc_pool_n; i++) {\n"
            "        void *r = NULL;\n"
            "        pthread_join(extc_pool[i].th, &r);\n"
            "    }\n"
            "    extc_pool_n = 0;\n"
            "    free(extc_pool);\n"
            "    extc_pool = NULL;\n"
            "}\n"
            "\n"
            "/* Run [0,n) with `threads` workers in `chunk`-sized pieces; returns the first\n"
            " * non-zero worker result (0 = all fine). threads <= 1, or a pool that could not be\n"
            " * started, is not an error: the range is simply finished on fewer workers. */\n"
            "static int64_t extc_par_run(int64_t (*fn)(int64_t, int64_t, int64_t, void *),\n"
            "                            void *ctx, int64_t withRegion,\n"
            "                            int64_t n, int64_t threads, int64_t chunk) {\n"
            "    extc_par_job job;\n"
            "    int64_t nw = 0, i, seq = 0;\n"
            "    if (threads > 256) threads = 256;\n"
            "    if (threads < 1) threads = 1;\n"
            "    job.fn = fn; job.ctx = ctx; job.n = n;\n"
            "    job.chunk = chunk > 0 ? chunk : (n / (threads * 8) > 0 ? n / (threads * 8) : 1);\n"
            "    job.next = 0; job.first_err = 0; job.region = NULL;\n"
            "    (void)withRegion;\n"
            "    if (extc_pool_inside) threads = 1;   /* a task never waits on the pool that runs it */\n"
            "    if (threads > 1) {\n"
            "        extc_pool_start(threads);\n"
            "        if (threads > extc_pool_n + 1) threads = extc_pool_n + 1;\n"
            "    }\n"
            "#ifdef EXTC_PAR_REGION\n"
            "    if (withRegion) {\n"
            "        /* One region per thread, refreshed per run: the previous one is closed here,\n"
            "         * which is exactly the moment the caller stops being able to read it. */\n"
            "        extc_par_close(extc_tls_region);\n"
            "        extc_tls_region = extc_par_open(threads);\n"
            "        job.region = extc_tls_region;\n"
            "    }\n"
            "#endif\n"
            "    if (n <= 0) return 0;\n"
            "    nw = threads - 1;\n"
            "    if (nw > 0) {\n"
            "        seq = ++extc_pool_seq;\n"
            "        for (i = 0; i < nw; i++) {\n"
            "            extc_pool[i].job = &job;\n"
            "            __atomic_store_n(&extc_pool[i].seq, seq, __ATOMIC_RELEASE);\n"
            "        }\n"
            "        /* Only wake them when one is actually asleep: a thread inside its spin window\n"
            "         * sees the sequence number on its own, so the steady state takes no lock. */\n");
        bufPuts(out,
            "        if (__atomic_load_n(&extc_pool_parked, __ATOMIC_RELAXED) > 0) {\n"
            "            pthread_mutex_lock(&extc_pool_mu);\n"
            "            pthread_cond_broadcast(&extc_pool_cv);\n"
            "            pthread_mutex_unlock(&extc_pool_mu);\n"
            "        }\n"
            "    }\n"
            "    extc_par_body(&job, 0);\n"
            "    if (nw > 0) {\n"
            "        for (i = 0; i < nw; i++)\n"
            "            if (__atomic_load_n(&extc_pool[i].done, __ATOMIC_ACQUIRE) != seq) break;\n"
            "        if (i < nw) {\n"
            "            /* Spin first: in the steady state the workers finish inside the spin window,\n"
            "             * so the hand-back costs a few loads instead of a wake-up. */\n"
            "            int64_t spins = 0;\n"
            "            while (spins++ < EXTC_POOL_SPIN) {\n"
            "                EXTC_POOL_PAUSE();\n"
            "                for (i = 0; i < nw; i++)\n"
            "                    if (__atomic_load_n(&extc_pool[i].done, __ATOMIC_ACQUIRE) != seq) break;\n"
            "                if (i >= nw) break;\n"
            "            }\n"
            "        }\n"
            "        if (i < nw) {\n"
            "            /* A worker is genuinely slow: park until the last one publishes. */\n"
            "            __atomic_fetch_add(&extc_pool_waiters, 1, __ATOMIC_RELAXED);\n"
            "            pthread_mutex_lock(&extc_pool_mu);\n"
            "            for (;;) {\n"
            "                for (i = 0; i < nw; i++)\n"
            "                    if (__atomic_load_n(&extc_pool[i].done, __ATOMIC_ACQUIRE) != seq) break;\n"
            "                if (i >= nw) break;\n"
            "                pthread_cond_wait(&extc_pool_cv, &extc_pool_mu);\n"
            "            }\n"
            "            pthread_mutex_unlock(&extc_pool_mu);\n"
            "            __atomic_fetch_sub(&extc_pool_waiters, 1, __ATOMIC_RELAXED);\n"
            "        }\n"
            "    }\n"
            "    return job.first_err;\n"
            "}\n"
            "\n"
            "/* Default worker count: when `par` names no thread count, use the cores this machine\n"
            " * makes available. `sysconf` is declared here instead of including unistd.h, because that\n"
            " * header collides with the library's own read/write declarations. */\n"
            "extern long sysconf(int);\n"
            "EXTC_UNUSED static int64_t extc_par_default_threads(void) {\n"
            "    const char *e = getenv(\"EXTC_PAR_THREADS\");\n"
            "    long t = e ? atol(e) : -1;\n"
            "    if (t <= 0) t = sysconf(84 /* _SC_NPROCESSORS_ONLN */);\n"
            "    if (t <= 0) t = 4;\n"
            "    if (t > 256) t = 256;\n"
            "    return t;\n"
            "}\n");
    }
}

/**
 * @brief Emit the dynamic-value runtime: the operations on `ExtcAny`.
 *
 * `any` is a value whose type is known only when the program runs. The invariant that makes it
 * sound is that a value has **exactly one** type at a time: rebinding replaces the old value, it
 * never stacks a second type on top. So the representation is a pair -- a descriptor and the bytes
 * -- and copy, print, compare and mark are each one walk over that descriptor.
 *
 * The bytes live inline when the value is eight bytes or less, and in a copy made in the caller's
 * arena otherwise. Which of the two is live travels in the **low bit of the descriptor pointer**
 * (`extc_any_type` clears it): descriptor objects are aligned, so the bit is free, and it keeps the
 * value at 16 bytes instead of 24. `desc == NULL` means the slot holds nothing.
 *
 * `desc->size` is *not* the size of the value -- for a byte view it is 1, the element size -- so the
 * emission site passes `sizeof(<T>)` to `extc_any_box` and that number decides the layout, while the
 * descriptor stays the thing printing and comparison walk.
 *
 * Emitted only when `g->needAny`, and after `rtPrint`/`rtEq`, because recursing into a field is
 * `extc_print`/`extc_eq` on the descriptor rather than new code here.
 *
 * @param arena     The allocator for the text.
 * @param out       The unit text, the append target.
 * @param withPrint Whether `extc_print` is in the unit; the print helper recurses through it.
 * @param withEq    Whether `==` on two `any` values is used; comparing recurses into `extc_eq`,
 *                  which is emitted with the equality runtime, so that helper is only defined when
 *                  that runtime is present.
 * @param withJson  Whether the unit carries the two integer formatters of `std::json`; the JSON
 *                  half walks a descriptor here but formats numbers through them, so it exists only
 *                  when that module does (the flag comes from the scan in the unit prologue).
 */
static void anyEmitRuntime(Arena *arena, Buf *out, bool withPrint, bool withEq, bool withJson) {
    (void)arena;
    bufPuts(out,
        "/* ---- `any`: one value whose type is known only at run time ----\n"
        " *\n"
        " * 16 bytes, the size of a `slice`: a descriptor and the bytes. Which half of `u` is live is\n"
        " * kept in the low bit of `desc` (`extc_any_type` clears it): descriptor objects are aligned,\n"
        " * so the bit is free, and it saves growing the value to 24 bytes. `desc == NULL` means the\n"
        " * slot holds nothing. The number that decides inline or boxed is the size of the value, which\n"
        " * the emission site knows and the descriptor does not (`desc->size` is the *element* size for\n"
        " * a view).\n"
        " */\n"
        "static const ExtcDesc *extc_any_type(const ExtcAny *a) {\n"
        "    return (const ExtcDesc *)((uintptr_t)a->desc & ~(uintptr_t)1);\n"
        "}\n"
        "\n"
        "static bool extc_any_boxed(const ExtcAny *a) {\n"
        "    return ((uintptr_t)a->desc & (uintptr_t)1) != 0;\n"
        "}\n"
        "\n"
        "EXTC_UNUSED static const void *extc_any_payload(const ExtcAny *a) {\n"
        "    return extc_any_boxed(a) ? (const void *)(uintptr_t)a->u : (const void *)&a->u;\n"
        "}\n"
        "\n"
        "/* Boxing copies the value in: that is what makes an `any` a value rather than a view, and\n"
        " * what lets one slot hold different types over its life. */\n"
        "/* Deepening a boxed value: a view is a pointer plus a length, so copying the view copies an\n"
        " * address, not the bytes it names. A payload that contains one is therefore copied field by\n"
        " * field with `extc_any_fixup`, which allocates fresh backing storage in the same arena. That\n"
        " * is what makes an `any` *own* its value: the box can outlive the frame the view pointed\n"
        " * into. Types without views take the memcpy path and pay nothing for this. */\n"
        "EXTC_UNUSED static void extc_any_fixup(extc_arena *arena, const ExtcDesc *d, void *value,\n"
        "                                      int depth);\n"
        "\n"
        "EXTC_UNUSED static void extc_any_fixup_elems(extc_arena *arena, const ExtcDesc *elem,\n"
        "                                            void *base, int64_t n, int depth) {\n"
        "    for (int64_t i = 0; i < n; i++)\n"
        "        extc_any_fixup(arena, elem, (char *)base + (size_t)i * elem->size, depth + 1);\n"
        "}\n"
        "\n"
        "EXTC_UNUSED static void extc_any_fixup(extc_arena *arena, const ExtcDesc *d, void *value,\n"
        "                                      int depth) {\n"
        "    if (!d || !value || depth > 32) return;   /* a cycle deeper than that is left as it is */\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *fs = (const ExtcField *)d->table;\n"
        "        for (size_t i = 0; fs && i < d->count; i++)\n"
        "            extc_any_fixup(arena, fs[i].desc, (char *)value + fs[i].off, depth + 1);\n"
        "        break;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "        extc_any_fixup_elems(arena, d->elem, value, (int64_t)d->count, depth);\n"
        "        break;\n"
        "    case EXTC_D_SLICE:\n"
        "    case EXTC_D_TEXT: {\n"
        "        /* Every view is two machine words: a data pointer then an element count. Reading\n"
        "         * them through `void **` / `int64_t *` avoids naming the view layout here. */\n"
        "        void **vw = (void **)value;\n"
        "        int64_t len = ((int64_t *)value)[1];\n"
        "        if (!vw[0] || len <= 0) break;\n"
        "        size_t stride = d->kind == EXTC_D_TEXT ? 1 : (d->elem ? d->elem->size : d->size);\n"
        "        size_t bytes = (size_t)len * stride;\n"
        "        void *copy = extc_arena_alloc(arena, (int64_t)bytes, \"any\", 0);\n"
        "        memcpy(copy, vw[0], bytes);\n"
        "        vw[0] = copy;\n"
        "        if (d->elem) extc_any_fixup_elems(arena, d->elem, copy, len, depth);\n"
        "        break;\n"
        "    }\n"
        "    case EXTC_D_REF: {\n"
        "        void **p = (void **)value;\n"
        "        if (!*p || !d->elem) break;\n"
        "        void *copy = extc_arena_alloc(arena, (int64_t)d->elem->size, \"any\", 0);\n"
        "        memcpy(copy, *p, d->elem->size);\n"
        "        *p = copy;\n"
        "        extc_any_fixup(arena, d->elem, copy, depth + 1);\n"
        "        break;\n"
        "    }\n"
        "    default:\n"
        "        break;   /* scalars, enums and the like hold no views of their own */\n"
        "    }\n"
        "}\n"
        "\n");
        bufPuts(out,
        "/* `dup` is a compile-time constant: the boxing site knows whether the static type can contain\n"
        " * a view, so the memcpy-only programs keep the fast path. */\n"
        "EXTC_UNUSED static ExtcAny extc_any_box(extc_arena *arena, const ExtcDesc *d,\n"
        "                                       const void *src, int64_t size, int64_t dup) {\n"
        "    ExtcAny v;\n"
        "    v.desc = d;\n"
        "    /* The payload word is always written, even when nothing is copied into it: readers take\n"
        "     * its address and read a whole struct through it, which the uninitialized-value analysis\n"
        "     * cannot follow. One store, and the diagnostic stays quiet. */\n"
        "    v.u = 0;\n"
        "    if (size <= 8 && !dup) {\n"
        "        memcpy(&v.u, src, (size_t)size);\n"
        "        return v;\n"
        "    }\n"
        "    void *p = size <= 8 ? (void *)&v.u : extc_arena_alloc(arena, size, \"any\", 0);\n"
        "    memcpy(p, src, (size_t)size);\n"
        "    if (dup) extc_any_fixup(arena, d, p, 0);\n"
        "    if (size <= 8) return v;\n"
        "    v.u = (uint64_t)(uintptr_t)p;\n"
        "    v.desc = (const ExtcDesc *)((uintptr_t)d | (uintptr_t)1);\n"
        "    return v;\n"
        "}\n"
        "\n"
        "/* Unboxing: the one implicit conversion in extC that can fail, so the check is the feature.\n"
        " * A mismatch traps at the line that asked for the other type; reinterpreting the bits\n"
        " * silently would be worse than stopping. */\n"
        "EXTC_UNUSED static const void *extc_any_unbox(const ExtcAny *a, const ExtcDesc *want,\n"
        "                                              const char *file, int line) {\n"
        "    if (!a->desc)\n"
        "        extc_trapMsg(file, line, \"this `any` holds no value\");\n"
        "    if (extc_any_type(a) != want) {\n"
        "        const ExtcDesc *got = extc_any_type(a);\n"
        "        fprintf(stderr, \"%s:%d: trap: this `any` holds a `%s`, not a `%s`\\n\",\n"
        "                file, line, got->name ? got->name : \"?\",\n"
        "                want->name ? want->name : \"?\");\n"
        "        extc_die(1);\n"
        "    }\n"
        "    return extc_any_payload(a);\n"
        "}\n");
    /* Printing an `any` is the descriptor printer with a run-time descriptor, so it is emitted only
     * when the program prints one: an unused `static` here would draw `-Wunused-function`, and
     * printing alone cannot be the reason `extc_print` exists. */
    if (withPrint)
        bufPuts(out,
            "\n"
            "EXTC_UNUSED static void extc_any_print(const ExtcAny *a) {\n"
            "    if (!a->desc) { extc_cout_put((uint8_t *)\"<none>\", 6); return; }\n"
            "    extc_print(extc_any_payload(a), extc_any_type(a));\n"
            "}\n");
    /* `==` compares through the same descriptor walk as printing, which is `extc_eq` -- so this half
     * waits for `rtEq` to be in the unit (the caller gates both on the same flag). */
    if (withEq)
        bufPuts(out,
            "\n"
            "EXTC_UNUSED static bool extc_any_eq(const ExtcAny *a, const ExtcAny *b) {\n"
            "    if (extc_any_type(a) != extc_any_type(b)) return false;\n"
            "    if (!a->desc) return true;\n"
            "    return extc_eq(extc_any_payload(a), extc_any_payload(b), extc_any_type(a));\n"
            "}\n");
    /* JSON from the same descriptor: an `any` is the only thing that can carry a run-time type, so
     * this half is part of the `any` runtime rather than of the printer. Everything below is that
     * half, so the gate is a return: it is the tail of the function, and the two `bufPuts` chunks
     * that follow open and close the brace of `extc_any_type_name` between them. */
    if (!withJson) return;
    bufPuts(out,
            "\n"
            "/* ---- JSON from a descriptor ----\n"
            " * The same walk the printer does, writing into a caller-supplied buffer instead of the\n"
            " * console. No allocation, no recursion through user code: a field whose type is a\n"
            " * struct is a nested descriptor, so the walk is depth-limited by the type graph. */\n"
            "EXTC_UNUSED static int64_t extc_json_raw(const char *s, int64_t n, uint8_t *out,\n"
            "                                        int64_t cap, int64_t o) {\n"
            "    for (int64_t i = 0; i < n; i++) { if (o < cap) out[o] = (uint8_t)s[i]; o++; }\n"
            "    return o;\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_text(const uint8_t *s, int64_t n, uint8_t *out,\n"
            "                                          int64_t cap, int64_t o) {\n"
            "    o = extc_json_raw(\"\\\"\", 1, out, cap, o);\n"
            "    for (int64_t i = 0; i < n; i++) {\n"
            "        uint8_t c = s[i];\n"
            "        if (c == '\"' || c == '\\\\') { o = extc_json_raw(\"\\\\\", 1, out, cap, o);\n"
            "            o = extc_json_raw((const char *)&c, 1, out, cap, o); }\n"
            "        else if (c == '\\n') o = extc_json_raw(\"\\\\n\", 2, out, cap, o);\n"
            "        else if (c == '\\t') o = extc_json_raw(\"\\\\t\", 2, out, cap, o);\n"
            "        else if (c < 0x20) {\n"
            "            char esc[7]; int k = snprintf(esc, sizeof esc, \"\\\\u%04x\", (unsigned)c);\n"
            "            o = extc_json_raw(esc, k, out, cap, o);\n"
            "        } else o = extc_json_raw((const char *)&c, 1, out, cap, o);\n"
            "    }\n"
            "    return extc_json_raw(\"\\\"\", 1, out, cap, o);\n"
            "}\n"
            "/* The integer formatters are extC functions in the `std::json` module, not text here:\n"
            " * they are ordinary digit arithmetic, and the descriptor walk only has to hand over the\n"
            " * tail of the buffer. The returned count is what the number **needs**, so a clipped\n"
            " * write still reports the size the caller has to ask for. */\n"
            "EXTC_UNUSED static int64_t extc_json_i64(uint8_t *out, int64_t cap, int64_t o, int64_t v) {\n"
            "    slice_u8 s;\n"
            "    s.data = cap > o ? out + o : out;\n"
            "    s.len = cap > o ? cap - o : 0;\n"
            "    return o + json$fmtI64(s, v);\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_u64(uint8_t *out, int64_t cap, int64_t o, uint64_t v) {\n"
            "    slice_u8 s;\n"
            "    s.data = cap > o ? out + o : out;\n"
            "    s.len = cap > o ? cap - o : 0;\n"
            "    return o + json$fmtU64(s, v);\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_num(const ExtcDesc *d, const void *p, uint8_t *out,\n"
            "                                         int64_t cap, int64_t o) {\n"
            "    char b[64]; int n = 0;   /* only the two floating kinds still go through snprintf */\n"
            "    switch (d->kind) {\n"
            "    case EXTC_D_I8:  return extc_json_i64(out, cap, o, (int64_t)*(const int8_t *)p);\n"
            "    case EXTC_D_I16: return extc_json_i64(out, cap, o, (int64_t)*(const int16_t *)p);\n"
            "    case EXTC_D_I32: return extc_json_i64(out, cap, o, (int64_t)*(const int32_t *)p);\n"
            "    case EXTC_D_I64: return extc_json_i64(out, cap, o, *(const int64_t *)p);\n"
            "    case EXTC_D_U8:  return extc_json_u64(out, cap, o, (uint64_t)*(const uint8_t *)p);\n"
            "    case EXTC_D_U16: return extc_json_u64(out, cap, o, (uint64_t)*(const uint16_t *)p);\n"
            "    case EXTC_D_U32: return extc_json_u64(out, cap, o, (uint64_t)*(const uint32_t *)p);\n"
            "    case EXTC_D_U64: return extc_json_u64(out, cap, o, *(const uint64_t *)p);\n"
            "    case EXTC_D_F32: n = snprintf(b, sizeof b, \"%.9g\", (double)*(const float *)p); break;\n"
            "    case EXTC_D_F64: n = snprintf(b, sizeof b, \"%.17g\", *(const double *)p); break;\n"
            "    case EXTC_D_BOOL: return extc_json_raw(*(const bool *)p ? \"true\" : \"false\",\n"
            "                                          *(const bool *)p ? 4 : 5, out, cap, o);\n"
            "    default: return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    }\n"
            "    return extc_json_raw(b, n, out, cap, o);\n"
            "}\n"
            "EXTC_UNUSED static int64_t extc_json_val(const ExtcDesc *d, const void *p, uint8_t *out,\n"
            "                                         int64_t cap, int64_t o, int depth) {\n"
            "    if (!d || depth > 32) return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    switch (d->kind) {\n"
            "    case EXTC_D_STRUCT: {\n"
            );

    bufPuts(out,
            "        const ExtcField *fs = (const ExtcField *)d->table;\n"
            "        o = extc_json_raw(\"{\", 1, out, cap, o);\n"
            "        for (size_t i = 0; fs && i < d->count; i++) {\n"
            "            if (i) o = extc_json_raw(\",\", 1, out, cap, o);\n"
            "            o = extc_json_text((const uint8_t *)fs[i].name,\n"
            "                               (int64_t)strlen(fs[i].name), out, cap, o);\n"
            "            o = extc_json_raw(\":\", 1, out, cap, o);\n"
            "            o = extc_json_val(fs[i].desc, (const char *)p + fs[i].off, out, cap, o, depth + 1);\n"
            "        }\n"
            "        return extc_json_raw(\"}\", 1, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_ENUM: {\n"
            "        const char *const *vs = (const char *const *)d->table;\n"
            "        int32_t tag = *(const int32_t *)p;\n"
            "        if (vs && tag >= 0 && (size_t)tag < d->count)\n"
            "            return extc_json_text((const uint8_t *)vs[tag], (int64_t)strlen(vs[tag]),\n"
            "                                  out, cap, o);\n"
            "        return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_ARRAY:\n"
            "    case EXTC_D_SLICE:\n"
            "    case EXTC_D_TEXT: {\n"
            "        const uint8_t *data;\n"
            "        int64_t n;\n"
            "        if (d->kind == EXTC_D_ARRAY) { data = (const uint8_t *)p; n = (int64_t)d->count; }\n"
            "        else { data = *(const uint8_t *const *)p; n = ((const int64_t *)p)[1]; }\n"
            "        if (d->kind == EXTC_D_TEXT) return extc_json_text(data, n, out, cap, o);\n"
            "        size_t stride = d->kind == EXTC_D_ARRAY ? d->size : (d->elem ? d->elem->size : d->size);\n"
            "        o = extc_json_raw(\"[\", 1, out, cap, o);\n"
            "        for (int64_t i = 0; i < n; i++) {\n"
            "            if (i) o = extc_json_raw(\",\", 1, out, cap, o);\n"
            "            o = extc_json_val(d->elem, data + (size_t)i * stride, out, cap, o, depth + 1);\n"
            "        }\n"
            "        return extc_json_raw(\"]\", 1, out, cap, o);\n"
            "    }\n"
            "    case EXTC_D_REF:\n"
            "        return extc_json_raw(\"null\", 4, out, cap, o);\n"
            "    default:\n"
            "        return extc_json_num(d, p, out, cap, o);\n"
            "    }\n"
            "}\n"
            "int64_t extc_any_json(ExtcAny *a, uint8_t *out, int64_t cap) {\n"
            "    if (!a->desc) return extc_json_raw(\"null\", 4, out, cap, 0);\n"
            "    return extc_json_val(extc_any_type(a), extc_any_payload(a), out, cap, 0, 0);\n"
            "}\n"
            "\n"
            "/* The type's name, copied out: a `ref u8` cannot be walked from extC source. */\n"
            "int64_t extc_any_type_name(ExtcAny *a, uint8_t *out, int64_t cap) {\n"
            "    const ExtcDesc *d = a->desc ? extc_any_type(a) : NULL;\n"
            "    const char *n = d && d->name ? d->name : (a->desc ? \"?\" : \"<none>\");\n"
            "    return extc_json_raw(n, (int64_t)strlen(n), out, cap, 0);\n"
            /* The closing brace belongs to **this** chunk: the text above is emitted in two
             * `bufPuts` calls because a single literal of that size is over the C99 minimum
             * (4095 bytes), and cutting between the `return` and the `}` left
             * `extc_any_type_name` open, so the descriptor table that follows landed inside its
             * body (`redeclaration of tri_desc with no linkage`). Keep the two halves in step:
             * every chunk that opens a brace has to close it. */
            "}\n"
            );
}

/**
 * @brief Ask for the descriptors a dynamic call needs, **before** the descriptor region is emitted.
 *
 * `descRef` registers a type's descriptor row for emission, and the rows are written out before the
 * bodies; a dispatcher emitted afterwards can therefore not add one. This pass walks the recorded
 * call sites while that is still possible and registers every candidate type, plus the type each
 * candidate returns (a return value is boxed, which needs its descriptor too).
 *
 * @param g  The generator; `g->dynCalls` holds the sites.
 * @param m  The module, the source of the candidate types.
 */
static void collectDynCallDescs(CG *g, Module *m) {
    for (size_t i = 0; i < g->dynCalls.len; i++) {
        Expr *e = *(Expr **)vecAt(&g->dynCalls, i);
        const char *name = e->u.method.name;
        for (size_t k = 0; k < m->structs.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, k);
            if (sd->typeParams.len > 0) continue;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *md = *(FuncDef **)vecAt(&sd->methods, j);
                if (md->params.len != 1 || strcmp(md->name, name) != 0 || !md->body) continue;
                (void)descRef(g, sd->type);
                if (md->ret && md->ret->kind != TY_VOID) (void)descRef(g, md->ret);
                break;
            }
        }
    }
}

/**
 * @brief Emit one resolver per `any` dynamic-call site, keyed by the receiver's descriptor.
 *
 * A chain of descriptor comparisons grows with the number of candidate types (2.7 ns at 16, 25 ns at
 * 256 in the prototype), so above `chainMax` candidates the resolver searches a sorted table instead
 * and jumps on the index. The candidates are fixed at compile time but their *addresses* are not, so
 * the table is sorted once, on the first call, and every later call searches it.
 *
 * @param g    The generator; `g->dynCalls` holds the sites, in emission order.
 * @param m    The module whose struct methods are the candidates.
 * @param out  The unit text to append to.
 */
static void emitDynCalls(CG *g, Module *m, Buf *out) {
    const size_t chainMax = 8;
    for (size_t i = 0; i < g->dynCalls.len; i++) {
        Expr *e = *(Expr **)vecAt(&g->dynCalls, i);
        const char *name = e->u.method.name;
        /* Collect the candidates first: the emission shape depends on how many there are. */
        Vec cands;
        vecInit(&cands, g->arena, sizeof(StructDef *));
        for (size_t k = 0; k < m->structs.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, k);
            if (sd->typeParams.len > 0) continue;      /* an instance needs its own type argument */
            FuncDef *md = NULL;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (cand->params.len == 1 && strcmp(cand->name, name) == 0) { md = cand; break; }
            }
            if (!md || !md->body) continue;
            *(StructDef **)vecPush(&cands) = sd;
        }
        bufPrintf(out, "\n/* `v.%s()` on an `any`, resolved by the receiver's descriptor. */\n", name);
        const bool wide = cands.len > chainMax;
        if (wide) {
            bufPrintf(out, "static struct { const ExtcDesc *key; int idx; } extc_dyn_%zu_map[] = {\n", i);
            for (size_t k = 0; k < cands.len; k++) {
                StructDef *sd = *(StructDef **)vecAt(&cands, k);
                bufPrintf(out, "    { %s, %zu },\n", descRef(g, sd->type), k);
            }
            bufPuts(out, "};\n");
            bufPrintf(out, "static int extc_dyn_%zu_sorted = 0;\n", i);
            bufPrintf(out, "static void extc_dyn_%zu_sort(void) {\n", i);
            bufPrintf(out, "    size_t __n = sizeof extc_dyn_%zu_map / sizeof extc_dyn_%zu_map[0];\n", i, i);
            bufPrintf(out, "    for (size_t __i = 1; __i < __n; __i++) {\n");
            bufPrintf(out, "        const ExtcDesc *__k = extc_dyn_%zu_map[__i].key; int __v = extc_dyn_%zu_map[__i].idx;\n", i, i);
            bufPrintf(out, "        size_t __j = __i;\n");
            bufPrintf(out, "        while (__j > 0 && (uintptr_t)extc_dyn_%zu_map[__j - 1].key > (uintptr_t)__k) {\n", i);
            bufPrintf(out, "            extc_dyn_%zu_map[__j] = extc_dyn_%zu_map[__j - 1]; __j--; }\n", i, i);
            bufPrintf(out, "        extc_dyn_%zu_map[__j].key = __k; extc_dyn_%zu_map[__j].idx = __v;\n", i, i);
            bufPuts(out, "    }\n");
            bufPrintf(out, "    extc_dyn_%zu_sorted = 1;\n", i);
            bufPuts(out, "}\n");
        }
        bufPrintf(out, "static ExtcAny extc_dyn_%zu(extc_arena *__extc_home,"
                       " const ExtcAny *__recv) {\n", i);
        bufPuts(out, "    const ExtcDesc *__t = extc_any_type(__recv);\n");
        if (wide) {
            bufPrintf(out, "    if (!extc_dyn_%zu_sorted) extc_dyn_%zu_sort();\n", i, i);
            bufPrintf(out, "    size_t __n = sizeof extc_dyn_%zu_map / sizeof extc_dyn_%zu_map[0];\n", i, i);
            bufPuts(out, "    size_t __lo = 0, __hi = __n;\n");
            bufPuts(out, "    while (__lo < __hi) {\n");
            bufPuts(out, "        size_t __mid = __lo + (__hi - __lo) / 2;\n");
            bufPrintf(out, "        if ((uintptr_t)extc_dyn_%zu_map[__mid].key < (uintptr_t)__t) __lo = __mid + 1;\n", i);
            bufPuts(out, "        else __hi = __mid;\n");
            bufPuts(out, "    }\n");
            bufPrintf(out, "    if (__lo < __n && extc_dyn_%zu_map[__lo].key == __t) {\n", i);
            bufPrintf(out, "        switch (extc_dyn_%zu_map[__lo].idx) {\n", i);
        }
        for (size_t k = 0; k < cands.len; k++) {
            StructDef *sd = *(StructDef **)vecAt(&cands, k);
            FuncDef *md = NULL;
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *cand = *(FuncDef **)vecAt(&sd->methods, j);
                if (cand->params.len == 1 && strcmp(cand->name, name) == 0) { md = cand; break; }
            }
            const char *ct = cType(g, sd->type);
            /* The branch is picked by the *receiver's* descriptor: its type has to be described
             * (it is a struct, and printing or comparison may never ask for it otherwise). */
            if (wide) bufPrintf(out, "        case %zu: {\n", k);
            else      bufPrintf(out, "    if (__t == %s) {\n", descRef(g, sd->type));
            /* The payload is copied into a properly typed local instead of being cast: the box
             * stores either bytes or a pointer, and reading it back through a cast is the kind of
             * type punning that makes an optimizer report `-Wmaybe-uninitialized` on the callers.
             * One copy of at most a few words, and the dispatch stays in the same nanosecond. */
            bufPrintf(out, "            %s __self;\n", ct);
            bufPrintf(out, "            memcpy(&__self, extc_any_payload(__recv), sizeof __self);\n");
            if (md->ret && md->ret->kind != TY_VOID) {
                const char *rct = cType(g, md->ret);
                bufPrintf(out, "            %s __r = %s(&__self);\n", rct, cFuncName(g, md));
                bufPrintf(out, "            return extc_any_box(__extc_home, %s,"
                               " (const void *)&__r, (int64_t)sizeof(__r), %d);\n",
                          descRef(g, md->ret), typeNeedsAnyDup(g, md->ret) ? 1 : 0);
            } else {
                bufPrintf(out, "            %s(&__self);\n", cFuncName(g, md));
                bufPuts(out, "            ExtcAny __empty;\n");
                bufPuts(out, "            __empty.desc = NULL;\n");
                bufPuts(out, "            __empty.u = 0;\n");
                bufPuts(out, "            return __empty;\n");
            }
            bufPuts(out, wide ? "        }\n" : "    }\n");
        }
        if (wide) bufPuts(out, "        default: break;\n        }\n    }\n");
        bufPrintf(out, "    fprintf(stderr, \"trap: no `%s` for a `%%s`\\n\","
                       " __t ? __t->name : \"?\");\n", name);
        bufPuts(out, "    extc_die(1);\n");
        bufPuts(out, "    ExtcAny __none;\n");
        bufPuts(out, "    __none.desc = NULL;\n");
        bufPuts(out, "    __none.u = 0;\n");
        bufPuts(out, "    return __none;\n");
        bufPuts(out, "}\n");
    }
}

/**
 * @brief Append the on-demand runtime blocks, the helpers and the view primitives to the unit text, and
 *        register the prunable entries.
 *
 * The splice order is C's definition-before-use: descriptor table, printing/equality runtime, console,
 * pools, file layer, DNS, memfind, plate view/copy, time, events, domains, dyn runtime, raw terminal,
 * then the individual helpers and the view indexers/copiers. The demand for helpers and view primitives
 * is conservative, so they are registered in `deadDefs` like the descriptors and `dropUnreferenced`
 * reclaims the parts nobody names.
 *
 * @param g     The code generation context; the `need*` flags and `helpers`/`unwraps`/`viewIdx`/
 *              `viewCpy`/`viewCpyU`/`descs` decide what is emitted.
 * @param out   The unit text, the append target.
 * @param m     The current module; domain adapters are generated for the `ext` targets in `funcs`.
 * @param arena The allocator the temporary buffers and registration texts for the view primitives come
 *              from.
 */
static void emitLateRuntime(CG *g, Buf *out, Module *m, Arena *arena) {
    /* The table is what the descriptors `emitDescRegion` may have written are made of, so it
     * goes out whenever anything could name it: a print, a structural `==`, or a descriptor row
     * that exists at all (`descs`). `rtPrint` goes out only where `extc_print` is actually
     * called -- that separation keeps the larger block out of a product that never prints. */
    if (g->needRuntime || g->eqNeed.len || g->descs.len) bufPuts(out, bufCstr(&g->rt));
    /* `rtCout` goes first: `extc_print` inside `rtPrint` writes the same buffer through `extc_printf`, so
     * it must see the declarations of `extc_cout_put` / `extc_printf` first. */
    if (g->needCout || g->needPrint) bufPuts(out, bufCstr(&g->rtCout));
    if (g->needPrint)                                  bufPuts(out, bufCstr(&g->rtPrint));
    if (g->needCoutF64) bufPuts(out, bufCstr(&g->rtCoutF64));
    if (g->needPool && !g->poolDone) placesEmitRuntime(arena, out);
    if (g->needHttpFile) fileLayerEmitRuntime(arena, out);
    if (g->needDns) dnsEmitRuntime(arena, out);
    /* The byte-search runtime, for a program that declares `extern!("extc-mem")`. Its trigger
     * also decided the `_GNU_SOURCE` preamble above; here only the bodies are appended. */
    if (g->needMemFind) memfindEmitRuntime(arena, out);
    if (g->needPlateView) plateEmitViewOf(arena, out);
    if (g->needPlateCopy) plateEmitMemCopy(arena, out);
    /* `mt` is whether the unit can start threads: `par` is the only way it can, and this point in
     * the emission order is after every function body, so the flag is settled. A single-threaded
     * unit gets the arena with no lock (see bufrt.c). */
    if (g->needBuf) bufRtEmitRuntime(arena, out, g->needPar ? 1 : 0, g->rc.reclaim ? 1 : 0);
    if (g->needTime) timeEmitRuntime(arena, out);
    if (g->needEvent) eventEmitRuntime(arena, out);
    if (g->needDomain) {
        /* The typed adapters the task table needs. They have to be declared **here**, in the
         * preamble: a call site sits inside a function body, while the coroutine section (where
         * `$step`/`$next` are defined) is appended last. `EXTC_UNUSED` because only the coroutines
         * some `ext` actually starts get one. */
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
            if (!planIsExtTarget(cf) || !planIsCoro(cf)) continue;
            const char *cn = cFuncName(g, cf);
            bufPrintf(out, "typedef struct %s$frame %s$frame;\n", cn, cn);
            bufPrintf(out, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);\n", cn, cn);
            if (planCoroNeedsZone(cf))
                bufPrintf(out, "EXTC_UNUSED static bool %s$next(struct %s$frame *f);\n", cn, cn);
            bufPrintf(out, "EXTC_UNUSED static bool %s$domstep(void *p)"
                           " { return %s$%s((struct %s$frame *)p); }\n",
                      cn, cn, planCoroNeedsZone(cf) ? "next" : "step", cn);
        }
        domainEmitRuntime(arena, out);
    }
    /* The dyn half is separate so a pool-only program keeps byte-identical generated C. */
    if (planUsesDyn(m)) placesEmitDynRuntime(arena, out);
    if (g->needRawTerm) {
        /* The raw-terminal block. `tcsetattr` is declared with the same prototype the
         * library declares for it, so the two declarations agree and the call reaches
         * libc; the layout of `struct termios` is never named. */
        bufPuts(out, bufCstr(&g->rtRaw));
    }
    if (g->eqNeed.len || g->needAnyEq)  bufPuts(out, bufCstr(&g->rtEq));
    if (g->needAny)                     anyEmitRuntime(arena, out, g->needPrint, g->needAnyEq,
                                                         g->needAnyJson);
    bufPuts(out, bufCstr(&g->desc));
    bufPuts(out, bufCstr(&g->rcDesc));   /* the rc type descriptors: they must come before the function bodies too */
    for (size_t i = 0; i < g->helpers.len; i++) {
        /* A slice helper is emitted on demand, and the demand is conservative: a
         * helper whose only user is unused is still emitted. It is registered
         * here, and dropUnreferenced takes it back out when the finished unit
         * does not name it. */
        SliceHelper *h = (SliceHelper *)vecAt(&g->helpers, i);
        DeadDef *d = arenaAllocZero(arena, sizeof *d);
        d->name = h->name;
        d->text = h->text;
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        bufPuts(out, h->text);
    }
    /* The `!` unwrap helpers, registered in the order they were first needed. Same
     * placement rule as the slice helpers: a definition has to precede its uses, and the
     * need is discovered while the bodies are generated. */
    for (size_t i = 0; i < g->unwraps.len; i++) {
        UnwrapHelper *h = (UnwrapHelper *)vecAt(&g->unwraps, i);
        DeadDef *d = arenaAllocZero(arena, sizeof *d);
        d->name = h->name;
        d->text = h->text;
        *(DeadDef **)vecPush(&g->deadDefs) = d;
        bufPuts(out, h->text);
    }
    /* The view index primitives, in the order they were first needed. They go
     * here, before the bodies, for the same reason the slice helpers do: a
     * definition has to precede its uses, and the need for one is discovered
     * while the bodies are generated. */
    for (size_t i = 0; i < g->viewIdx.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewIdx, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewIndexer(g, inst);
        substLeave(g, sv);
        g->out = save;
        /* Registered like a descriptor: an index primitive is emitted on demand, and the
         * demand is conservative - a view whose subscripts all sit in dead code still gets one.
         * dropUnreferenced takes it back out when nothing names it. */
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *idd = arenaAllocZero(arena, sizeof *idd);
        idd->name = arenaPrintf(arena, "%s_index", inst->name);
        idd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = idd;
    }
    for (size_t i = 0; i < g->viewCpy.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewCpy, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewCopier(g, inst, false);
        substLeave(g, sv);
        g->out = save;
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *cdd = arenaAllocZero(arena, sizeof *cdd);
        cdd->name = arenaPrintf(arena, "%s_copy", inst->name);
        cdd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = cdd;
    }
    /* The `@unchecked` copiers: same shape, no count/storage check. They get their
     * own names so a checked call site of the same view type keeps the checked one. */
    for (size_t i = 0; i < g->viewCpyU.len; i++) {
        Type *inst = *(Type **)vecAt(&g->viewCpyU, i);
        Buf tmp;
        bufInit(&tmp, arena);
        Buf *save = g->out;
        g->out = &tmp;
        SubCtx sv = substEnter(g, inst);
        genViewCopier(g, inst, true);
        substLeave(g, sv);
        g->out = save;
        size_t bio = out->len;
        bufPuts(out, bufCstr(&tmp));
        Buf it;
        bufInit(&it, arena);
        bufPutn(&it, out->data + bio, out->len - bio);
        DeadDef *cdd = arenaAllocZero(arena, sizeof *cdd);
        cdd->name = arenaPrintf(arena, "%s_copyU", inst->name);
        cdd->text = bufCstr(&it);
        *(DeadDef **)vecPush(&g->deadDefs) = cdd;
    }
}

/**
 * @brief Finish the whole unit: attach the function bodies, repeatedly prune unnamed definitions, then
 *        add the coroutine and method-table definitions.
 *
 * Unreferenced pruning runs until the text length stops changing: dropping one definition can leave the
 * next one unnamed as well. The runtime primitive blocks (`primSplice`) are spliced in only after
 * pruning, because that question can only be asked of the final text. `coroDefs` and `vtDefs` are
 * appended last: they must come after every byte-offset-rewriting pass, and a frame type must precede
 * the `$step` that uses it. `g->bodyOff` records where the function bodies start.
 *
 * @param g     The code generation context; `body`/`deadFuncs`/`mainOff`/`mainLen` are consumed here.
 * @param out   The unit text the bodies and the tail definitions are appended to.
 * @param arena The allocator the temporary buffers for slicing definition text come from.
 */
static void finishUnit(CG *g, Buf *out, Arena *arena) {
    g->bodyOff = out->len;                    /* where the bodies start in the finished unit */
    bufPuts(out, bufCstr(&g->body));
    /* The body buffer is complete and contiguous now, so the texts the unreferenced-definition
     * passes work on come from it: a slice cut during generation would not survive the view-index
     * helpers, which write into a different buffer for a moment. */
    for (size_t i = 0; i < g->deadFuncs.len; i++) {
        DeadFunc *df = *(DeadFunc **)vecAt(&g->deadFuncs, i);
        if (!df->body && df->len) {
            Buf b;
            bufInit(&b, arena);
            bufPutn(&b, g->body.data + df->off, df->len);
            df->body = bufCstr(&b);
        }
    }
    if (g->mainLen) {
        Buf b;
        bufInit(&b, arena);
        bufPutn(&b, g->body.data + g->mainOff, g->mainLen);
        g->mainBody = bufCstr(&b);
    }

    /* The unit is complete: the definitions nobody names can go, repeatedly
     * until the text stops shrinking. One pass is not enough, because dropping
     * one definition makes the next one unnamed. */
    for (int round = 0; round < 8; round++) {
        size_t before = out->len;
        CGACC("cg-prune", dropUnreferenced(g, out));
        cgReport();
        if (out->len == before) break;
    }
    /* The runtime primitive block is decided here from the registry, and only
     * then becomes part of the unit: this question is asked once, of the settled
     * text. Nothing before `primA` is ever removed, so that offset is still
     * exact. */
    primSplice(g, out);
    /* A function nothing calls says so; then the parameters a body never reads. */
    markUncalledFunctions(g, out);
    markUnusedParams(g, out);

    /* The method-table definitions come last so no pass that rewrites the unit
     * by byte offset sees them; the declarations emitted earlier are what lets a
     * dispatch site in a body refer to them. Coroutine frames and step functions
     * go before them, so a frame type is defined above the step function using
     * it. */
    bufPuts(out, bufCstr(&g->coroDefs));
    bufPuts(out, bufCstr(&g->vtDefs));
}

/**
 * @brief Generate one complete C translation unit for a checked module.
 *
 * The order is fixed: initialize the state -> scan which runtime is needed -> type definitions ->
 * globals -> prototypes -> function bodies (into a temporary buffer) ->
 * coroutines/descriptors/runtime spliced on demand -> unreferenced pruning -> tail definitions. Two
 * naming checks run on entry: a generic instance name must not collide with a user-declared struct, and
 * an `@export` name must carry no hidden parameter, must not be a C keyword, and no two exports may
 * share a name. Any failure reports an error and returns false.
 *
 * @param ctx     The compilation context; errors are reported through it and `hasError` decides the
 *                return value.
 * @param arena   The allocator both the CG state and the product buffer are allocated from.
 * @param tt      The type table, which provides instances and function types.
 * @param m       The current module.
 * @param lineMap When true, the line-number mapping is kept in the product.
 * @param out     The product buffer; the complete C text is written here.
 * @return        true when nothing was reported; false after reporting an error for a naming collision
 *                or a broken export contract.
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out) {
    /* A generated instance's C name is `<template>_<argument>...` (`pair` + `i64`
     * = `pair_i64`). A program may define that very name, and the unit would then
     * hold two definitions of `struct pair_i64`. The compiler cannot rename one
     * side, so it says which name it needs. Checked here rather than in the
     * checker: the last instantiation wave happens after the checker's passes. */
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *inst = *(Type **)vecAt(&tt->instances, i);
        if (!inst || inst->kind != TY_GENERIC || !inst->sdef || !inst->name) continue;
        for (size_t j = 0; j < m->structs.len; j++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, j);
            /* Skip the holders the compiler builds for generic instances itself:
             * they carry the very name being checked (`pair<i64>`'s method holder
             * is `pair_i64`) and are not user declarations. */
            if (sd == inst->sdef || !sd->name) continue;
            /* A compiler-built holder points at the **instance** as its type; a
             * user declaration points at itself. */
            if (sd->type && sd->type->sdef != sd) continue;
            if (strcmp(sd->name, inst->name) != 0) continue;
            ctxError(ctx, sd->line ? sd->line : 1, 1,
                     "The compiler names an instance of a generic type `<type>_<argument>`;"
                     " give this declaration another name.",
                     "`%s` is the name the compiler needs for an instance of `%s`, and the"
                     " program already defines it", inst->name, inst->sdef->name);
            return false;
        }
    }
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.tt = tt;

    /* `@export` promises the C side a symbol. Three things can break that promise, and all three are
     * decided here because this is where the answers are final: the hidden parameters (a transitive
     * closure the checker settles in its last pass), the name (this file's business), and uniqueness
     * among exports. A fourth -- that the signature is one C can write -- is the checker's, next to
     * `extern!`'s, so that the two halves of the boundary keep using one predicate (`ttCrossesC`). */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExport) continue;
        const char *en = exportName(f);
        if (funcTakesHomeArena(f) || funcTakesHomeZone(f))
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "A function that takes a home arena or zone is compiled with one hidden"
                     " parameter more than its source signature, so C's call would not match. Let it"
                     " allocate in its own block arenas instead, or export a wrapper that does.",
                     "`@export` on `%s`, which takes a hidden arena parameter", en);
        if (cSymName(&g, en) != en)
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "An exported symbol is a contract with the C side: the name has to be the one C"
                     " will look up, so this compiler will not rename it. Pick another name.",
                     "`@export` on `%s`: that name is a C keyword, so the symbol would have to be"
                     " renamed", en);
        for (size_t j = 0; j < i; j++) {
            FuncDef *o = *(FuncDef **)vecAt(&m->funcs, j);
            if (!o || !o->isExport) continue;
            if (strcmp(exportName(o), en) != 0) continue;
            ctxError(ctx, f->line ? f->line : 1, 1,
                     "Two exported functions with one C name would be one symbol with two bodies."
                     " The module prefix is stripped from an exported name, so the two declarations"
                     " have to differ in the name they write.",
                     "`@export` name `%s` is already taken by another exported function", en);
        }
    }

    initCGState(&g, m, tt, arena);
    /* the entry table must be in place before emitProtos: cgParamList decides an entry's C parameters
     * from needModuleTramp */
    scanModuleEntries(&g, m);
    scanUnitNeeds(&g, m);
    initRuntimeText(&g, arena);
    collectEmitted(&g, m);
    /* Lambda environments whose layout depends on a type parameter get one layout per instance of
     * the body they are written in; the one-layout forms must then not be emitted at all. */
    collectLamVariants(&g);
    dropLamTemplates(&g);
    collectLamThunks(&g);
    /* Settle "this program writes the console" first: main's prologue registers
     * `atexit(extc_cout_flush)` from it, and without it a program that exits or traps halfway loses the
     * bytes in the buffer. */
    scanConsoleUse(&g);
    rcPlanDecide(&g, m);
    /* The event tier (the automatic half of `EXTC_RC_SWEEPFREE`): the whole unit is proved "acyclic and
     * references live only in counted slots" => every unreachable object is eventually freed on the spot
     * when its count reaches zero => sweeping has nothing to do. `EXTC_RC_SWEEPFREE=1` still overrides
     * it (the criteria's red proof uses that). */
    /* Criterion (d) units stay in the sweep tier: the event tier frees an object when its holder count
     * reaches zero, and the array that made the unit non-idle has no holders to count. */
    if (g.rc.sweepFree != -1 && g.rc.reclaim && !g.rc.memGrowth && unitAllAcyclic(&g, m)) {
        g.rc.sweepFree = 1;
        g.rc.noCount = 0;
    }
    emitPreamble(&g, out, m);
    emitRuntimePrologue(&g, out, m);
    buildRuntimeText(&g);
    emitTypes(&g, m, tt, arena);
    /* ------------------------------------------------------------------
     * The descriptor table is not emitted here. It goes between the prototypes
     * and the function bodies, because which descriptors are needed is known
     * only from the bodies, through what `genPrint` prints; see emitDescRegion.
     * ------------------------------------------------------------------ */

    emitGlobals(&g, m);
    emitProtos(&g, m, arena);
    /* ================= the body pool: definitions from here on ========== */

    /* Bodies are written into a temporary buffer first, because a slice helper is
     * discovered to be needed only while generating, and C wants definitions
     * before uses. Everything is spliced together at the end as prototypes, then
     * helpers, then bodies. */
    g.out = &g.body;

    /* Coroutine frames and step prototypes, before any body that may spawn one or drive it. */
    genCoroDecls(&g, m);

    /* The index primitives of the views are emitted on demand, at the first
     * subscript that needs one (`viewIndexer`). Printing and comparison derive no
     * function at all, see the descriptor table. */

    emitInstanceBodies(&g);
    emitVtTables(&g, out, m, arena);
    emitParTramps(&g);
    emitFuncBodies(&g);
    emitLamThunks(&g);
    emitLamVars(&g);
    emitGlobalDrops(&g);
    dbgRcCostReport(&g);
    /* Final assembly: the pieces whose need is discovered during generation are
     * spliced in ahead of the bodies - the print runtime and the descriptor
     * table, emitted on demand according to what genPrint printed, and the slice
     * helpers. The order is prototypes, then descriptors, then bodies, which is
     * what the define-before-use rule of C requires. */
    g.out = out;
    assembleCoroutineRuntime(&g, out, arena);
    /* Descriptors a dispatcher names have to be registered while rows are still being collected. */
    collectDynCallDescs(&g, m);
    emitDescRegion(&g);
    if (g.needCoroHandle && !g.coroDeclsDone) genCoroHandleDecls(&g, m);
    /* Descriptor types and shared scalar descriptors: needed by printing or by
     * comparison, whichever comes first. */
    /* Parallel execution over a range: the runtime half of `par::run` / `par::map`.
     *
     * The language has no function pointers, so the callback comes from a generated
     * static trampoline; this block only knows a plain C function pointer. It is
     * emitted on demand (`needPar`, or `EXTC_DBG_PAR=1` while it is being built).
     *
     * What the caller and this runtime keep between them:
     *   - ranges are disjoint by construction: one atomic chunk counter hands out
     *     [lo,hi) and a worker only writes what it was handed;
     *   - every worker gets its own arena (`extc_tls_arena`), released whole when the
     *     worker ends, so two threads never touch the same block;
     *   - nothing here is shared mutable state, so the block is re-entrant.
     *
     * POSIX only, and deliberately without `pthread.h`: generated C carries only the
     * few headers it wants, and these two prototypes match the glibc ABI
     * (`pthread_t` is `unsigned long` there, and the attribute may be NULL). */
    /* One source text, two configurations: without the define the region half is not
     * compiled at all. */
    emitParallelRuntime(&g, out);
    emitLateRuntime(&g, out, m, arena);
    /* Last, because a dispatcher boxes its result: the `any` runtime has to be in the unit already. */
    emitDynCalls(&g, m, out);
    finishUnit(&g, out, arena);
    return !ctx->hasError;
}
