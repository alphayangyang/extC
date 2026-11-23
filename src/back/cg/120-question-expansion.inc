/* ------------------------------------------------------------- `?` expansion
 *
 * `?` works at statement level, because C has no statement expressions, so it
 * expands into
 *     <evaluate once>  ->  <return on failure>  ->  <use the payload>
 *
 * What the compiler knows here is the protocol of the two result types, in the
 * same spirit as the `data` and `len` of a view: the struct, its methods and its
 * constructors all live in the prelude, and only a few names are fixed -
 * `some` and `none` for `option`, `success` and `failure` for `result`, with the
 * payload at `u.<variant>._0`. main.c verifies that those names exist where they
 * are needed.
 *
 * `option` and `result` are ordinary enums here, so everything below goes
 * through a tag comparison and a payload path; there are no `has`/`ok` fields
 * to read and rebuild.
 */
typedef struct {
    const char *tmp;      /* holds the operand, evaluated exactly once */
    const char *inst;     /* C name of the instance: option_i64, result_unit_gameError */
    const char *okVar;    /* success variant: some / success */
    const char *failVar;  /* failure variant: none / failure */
    Type       *payload;  /* payload type T, carried by the success variant */
    bool        isOpt;    /* true for option, false for result */
} TryInfo;

/* Is this type an instance of a given generic type with a given arity?
 *
 * The definition name is compared, not the instantiated C name, so `option<i64>`
 * is still recognized as `option`.
 *
 * Params:
 *   name  - name of the generic definition, such as "option"
 *   nargs - required number of type arguments
 *
 * Returns:
 *   true for a generic struct (`slice<T>`, `vector<T>`) or a generic enum
 *   (`option<T>`, `result<T,E>`) with that name and arity.
 */
/**
 * @brief Give the C path of the success-side payload.
 *
 * It is `x.u.some._0` or `x.u.success._0`: `option` and `result` are both
 * `struct { tag; union }` in C and the payload is always at `u.<success variant>._0`.
 *
 * @param g   The generator state.
 * @param ti  The descriptor already filled in by genTryHead: `tmp` and `okVar` build the path.
 * @return    The C lvalue expression of that payload.
 */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/**
 * @brief Construct the value a `?` operand's failure has to return.
 *
 * The failure value is constructed according to the outer function's return type: for `option`
 * it is its zero value, that is, `none`; for `result` it is a failure variant carrying the inner
 * error.
 *
 * @param g   The generator state.
 * @param ti  The descriptor of the `?` operand; `isOpt` distinguishes `option` from `result`,
 *            and `tmp` / `failVar` move the inner error.
 * @return    The C expression of the return type; `"0"` when the outer function has no return
 *            type.
 */
static const char *genTryFail(CG *g, TryInfo *ti) {
    Type *rt = subst(g, g->retType);
    if (!rt) return "0";
    if (ti->isOpt) return zeroValue(g, rt);
    /* For a `result` the inner error is copied across as it is: the error type
     * E is the same, so the C member is read directly. */
    return arenaPrintf(g->arena,
                       "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s.u.%s._0 } }",
                       cType(g, rt), rt->name, ti->failVar, ti->failVar,
                       ti->tmp, ti->failVar);
}

/**
 * @brief Decrement the recursion depth counter on one exit path.
 *
 * The decrement must come after the return value is computed and never before it: decrementing
 * first turns `return spin(n)` into `--depth; return spin(n);`, so every tail-call frame resets
 * the counter and the guard never fires (at -O0 gcc folds that shape into a tail call and the
 * program exits at once with no output).
 *
 * A `return` is not the only exit: the implicit return at the end of the body is one too, so the
 * depth is also decremented where the body ends; an explicit return that already decremented
 * through cgReturn does not decrement a second time.
 *
 * @param g  The generator state; nothing is emitted when the function is not self-recursive.
 */
static void cgRecLeave(CG *g) {
    if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
}

/** @brief Run the `@drop` hooks of every local still in scope (see the definition). */
static void genReturnDrops(CG *g);

/** @brief Is a local within reach whose death hands storage back? Asked before choosing the shape of
 *         an arena-less return, which has no epilogue to jump to. */
static bool genReturnDropsPending(CG *g);

/** @brief Does one by-value parameter own storage, so that leaving the function has to hand it back? */
static bool paramNeedsDrop(CG *g, Param *p);

/** @brief Are there parameters of `f` whose death hands storage back? */
static bool funcHasParamDrops(CG *g, FuncDef *f);

/** @brief Emit the drop of every by-value parameter that owns storage (see the definition). */
static void genParamDrops(CG *g, FuncDef *f);

/**
 * @brief Return a value from the current function.
 *
 * Every exit funnels into one epilogue instead of repeating the arena release sequence at each
 * return: the value is stored into `__extc_ret_v`, the recursion depth is decremented, and control
 * jumps to `__extc_ret`, where that release sequence appears once.
 *
 * The value is computed before the arenas are released, slightly more conservative than
 * "release, then evaluate".
 *
 * @param g    The generator state.
 * @param val  The C expression of the value to return; NULL means a bare return (a `void` exit).
 *             A function with `noArena` returns directly: it does not allocate and has no arena
 *             array to release.
 */
static void cgReturn(CG *g, const char *val) {
    /* The buffered console tail leaves here (ruling 90): every `return` of `main` is a
     * place where the process is about to end and whatever is still buffered would be
     * lost. It is a no-op when the buffer is empty. */
    if (g->inMain && g->needCout) cgLine(g, "extc_cout_flush();");
    if (g->noArena) {
        /* There is no epilogue label to jump to, so the drops are emitted here. The value has to be
         * materialised into a temporary first: a hook must not read the parameter or the local the
         * returned value was taken from. */
        /* \`curFunc\` is read-only here: the generator never writes through it. */
        FuncDef *cur = (FuncDef *)g->curFunc;
        bool pending = genReturnDropsPending(g) || funcHasParamDrops(g, cur);
        if (val && pending) {
            cgLine(g, "{ %s __r = %s;", cType(g, g->curFunc->ret), val);
            g->indent++;
            genReturnDrops(g);
            genParamDrops(g, cur);
            cgTaskRootPopsForReturn(g);
            if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
            cgLine(g, "return __r;");
            g->indent--;
            cgLine(g, "}");
            return;
        }
        genReturnDrops(g);
        genParamDrops(g, cur);
        cgTaskRootPopsForReturn(g);
        if (g->isRecursive) {
            /* The return type need not be scalar: a self-recursive function may return `option<T>` or a
             * struct. The temporary is therefore declared with the real return type and **no cast**
             * is added (C does not allow a cast to a struct type, and a scalar assignment converts
             * implicitly anyway). */
            if (val) cgLine(g, "{ %s __r = %s; --__extc_rec_depth; return __r; }",
                            cType(g, g->curFunc->ret), val);
            else     cgLine(g, "{ --__extc_rec_depth; return; }");
        } else if (val) cgLine(g, "return %s;", val);
        else            cgLine(g, "return;");
        return;
    }
    if (val) cgLine(g, "__extc_ret_v = %s;", val);
    /* `@drop` hooks run after the value is stored and before the jump: a local that owns state
     * hands it back on the way out, innermost block first. */
    genReturnDrops(g);
    cgTaskRootPopsForReturn(g);
    cgRecLeave(g);
    cgLine(g, "goto __extc_ret;");
}

/**
 * @brief Emit the statement that "evaluates the `?` operand once and returns on failure", and
 *        describe the result to the caller.
 *
 * The failure branch's return is already written into the line emitted here, so "release every
 * enclosing arena level" must be settled before that line.
 *
 * @param g  The generator state of the `?`.
 * @param e  The `EX_TRY` node.
 * @return   The TryInfo describing the operand's temporary, its C type name and the variant name;
 *           the caller reads the payload through it.
 */
static TryInfo genTryHead(CG *g, Expr *e) {
    TryInfo ti;
    Type *ot = ttBase(subst(g, e->u.try_.operand->type));
    ti.isOpt = isProtoType(ot, "option", 1);
    ti.inst = cType(g, ot);
    ti.okVar = ti.isOpt ? "some" : "success";
    ti.failVar = ti.isOpt ? "none" : "failure";
    ti.payload = subst(g, *(Type **)vecAt(&ot->targs, 0));
    ti.tmp = arenaPrintf(g->arena, "__extc_try%d", g->tmpSeq++);

    const char *operand = genExpr(g, e->u.try_.operand);
    flushPrefix(g);
    cgLine(g, "%s %s = %s;", ti.inst, ti.tmp, operand);
    /* A failure returns, so every enclosing arena level is released, exactly as
     * for a `return` statement, through the shared epilogue. That decision has
     * to be taken before this line is written, because the line already
     * contains the return. */
    cgLine(g, "if (%s.tag != %s_%s) {", ti.tmp, ti.inst, ti.okVar);
    g->indent++;
    if (g->inMain) {
        /* `?` in `main`: the failure is fatal, so it is reported and the program stops. The payload is
         * printed through its descriptor, which is the same machinery `println(e)` uses -- so
         * `ioError.readFailed(5)` still says which fd failed rather than "something failed". A
         * `none` has no payload. */
        if (!ti.isOpt) {
            Type *et = subst(g, *(Type **)vecAt(&ot->targs, 1));
            g->needRuntime = true;               /* the descriptor table ... */
            g->needPrint   = true;               /* ... and `extc_print`, called just below */
            g->needCout    = true;               /* the descriptor printer writes the console buffer */
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: a step in `main` failed: \","
                      " \"%s\", %d);", g->path, e->line);
            cgLine(g, "extc_print(&(%s.u.%s._0), %s);", ti.tmp, ti.failVar, descRef(g, et));
            /* The payload is written through the **console buffer** and the process is about to call
             * `extc_die(1)`: it must be flushed here, or the trap message keeps only its first half
             * (`extc_die` does not go through the exit registered with `atexit`). */
            cgLine(g, "extc_cout_flush();");
            cgLine(g, "fprintf(stderr, \"\\n\");");
        } else {
            cgLine(g, "extc_trapMsg(\"%s\", %d, \"a step in `main` returned `none`\");",
                   g->path, e->line);
        }
        cgLine(g, "extc_die(1);");
    } else {
        cgReturn(g, genTryFail(g, &ti));
    }
    g->indent--;
    cgLine(g, "}");
    return ti;
}


/**
 * @brief Emit one `#line` directive pointing back at the .extC source position of this statement.
 *
 * @param g  The generator state; nothing is emitted while line mapping is off.
 * @param s  The statement; nothing is emitted either when `s->line` is 0 (the statement carries
 *           no line number).
 */
static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

/* Return the arena argument for a call to a callee that needs a home arena.
 *
 * Which arena to pass has already been decided by the checker and recorded in
 * `Expr.arenaArg`: `ARENA_HOME` means this function's own home arena, and a
 * value of 1 or more means the block arena at that level. Code generation only
 * translates that decision and never makes one; a fallback that second-guessed
 * the checker here would be a second authority on the same question.
 *
 * Returns:
 *   A C expression denoting the arena, usable as a function argument.
 */
/**
 * @brief Give the zone argument this call site passes to the callee, that is, "which place the
 *        pool should be created in".
 *
 * Parallel to homeArg: the arena side passes "the arena the caller chose" and this side passes
 * "the zone index of the place the caller chose".
 *
 *   - When this function itself receives the parameter (`funcHasZoneParam`) it is passed straight
 *     down: a caller that picked a place for the `dyn` payload this call stores reaches the
 *     runtime with its own choice;
 *   - otherwise (`main` in the entry file, an ordinary function) it passes the current place's
 *     zone, that is, the most recently pushed `__extc_zm<lvl>` -- exactly "the value lands in the
 *     place of the block that stores it".
 *
 * The site records its own level (the checker's `zoneLevel`): `ZONE_HOME` = the home zone this
 * function received (passed down), k>=1 = `__extc_zm<k>` of the place at level k.
 *
 * The fallback is the runtime's own "the place I am in right now", `extc_placeTop`: it is exactly
 * equivalent to `(extc_place_depth() - 1)` (in the runtime `extc_place_depth() = extc_placeTop + 1`), but
 * its meaning is direct instead of drifting with the definition of `zoneDepth()`. On a single
 * stack it is the place the caller is in. Only a `noArena` function reaches this (it pushed no
 * zone); so does `stmtMakesPool` when it conservatively answers true for an unresolved callee that
 * does not actually create a pool, in which case there is neither a zone nor a runtime yet the
 * call site still has to produce the second argument. This arm references the runtime's zone
 * table, so the pool runtime is marked as needed here; the runtime text is only emitted after the
 * function bodies (`if (g->needPool) placesEmitRuntime(...)`), so there is time.
 *
 * @param g  The generator state.
 * @param e  The call-site expression; NULL or no zone level takes the fallback.
 * @return   A C expression usable directly as a function argument, naming a zone.
 */
static const char *zoneArgRef(CG *g, Expr *e) {
    /* When the site records its own level (the checker's `zoneLevel`), use it: `ZONE_HOME` = the
     * home zone this function received (passed down), k>=1 = `__extc_zm<k>` of the place at level
     * k. */
    if (getenv("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonearg] %s node=%p lvl=%d mark1=%d mark2=%d blk=%d\n",
                g->curFuncName ? g->curFuncName : "-", (void *)e, e ? planZoneLevel(e) : 0,
                (int)g->zoneMark[1], (int)g->zoneMark[2], g->blkLevel);
    /* Inside a coroutine's step the place is **read from the frame**: rule 1 of the design says a
     * resume must not re-derive it from the environment, and a step has nothing to re-derive it from
     * anyway -- no hidden parameters, no zone mark of its own. */
    if (g->coroFunc) return "f->zone";
    if (e && planZoneLevel(e) != 0) {
        if (planZoneLevel(e) == ZONE_HOME) {
            if (g->funcHasZoneParam) return "__extc_home_zone";
            /* `main`'s C signature is fixed and it receives no home-zone parameter => its "home" is **its
             * own body** (the same rule as `__extc_home = &__extc_a[1]` for the home arena).
             *
             * Falling through to "the most recently pushed zone" below is the **wrong** thing here:
             * a pool upgraded inside an inner block is registered on that block's zone and collected
             * when the block ends, while the receiver is still alive. SSO's `push` is the first
             * method that creates a pool for its receiver: a loop pushing past byte 17 upgrades =>
             * each iteration collects the pool at block end => the next `pStale` poisons the string
             * into an empty one, `len=3` instead of 20; when the string is declared outside the loop
             * it is worse -- the pool has been returned to malloc and the read sees reused memory.
             * The home zone lives longer and only costs memory (the same direction as escape
             * analysis preferring to over-retain). */
            if (g->zoneFrameMarked) return "__extc_zm1";
        }
        if (planZoneLevel(e) == 1 && g->zoneFrameMarked) return "__extc_zm1";
        if (planZoneLevel(e) > 1
            && planZoneLevel(e) < g->levelCap
            && g->zoneMark[planZoneLevel(e)])
            return arenaPrintf(g->arena, "__extc_zm%d", planZoneLevel(e));
    }
    if (g->funcHasZoneParam) return "__extc_home_zone";
    for (int lvl = g->blkLevel; lvl >= 1; lvl--) {
        if (lvl < g->levelCap && g->zoneMark[lvl])
            return arenaPrintf(g->arena, "__extc_zm%d", lvl);
    }
    /* The fallback references the runtime zone table, so the pool runtime is marked as "needed"
     * here -- the runtime text is only emitted after the function bodies
     * (`if (g.needPool) placesEmitRuntime(...)`), so there is time. The typical way to reach this
     * arm is `stmtMakesPool` conservatively answering true for an unresolved callee: that function
     * does not actually create a pool, so there is neither a zone nor a runtime, yet the call site
     * still has to produce the second argument. */
    g->needPool = true;
    return "extc_placeTop";
}

/**
 * @brief Write the arena level the checker chose as one arena argument.
 *
 * The result is the pointer itself, passed as an argument, and must not be confused with
 * arenaRefAt: the caller wraps that one in `&`, so it yields the dereferenced form
 * `(*__extc_home)`.
 *
 * @param g         The generator state; the subscript form is built in its arena.
 * @param arenaArg  The arena level: `ARENA_HOME` (the function's own home arena) or a block
 *                  level of 1 and above.
 * @return          `__extc_home` or `&__extc_a[arenaArg]`.
 */
static const char *homeArg(CG *g, int arenaArg) {
    if (arenaArg == ARENA_HOME) return "__extc_home";
    if (arenaArg >= 1) g->rcArenaUsed |= 1u << arenaArg;   /* the callee may allocate into this arena */
    return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);
}

/**
 * @brief Predeclare the "region pointer for this region base address" local and return its name
 *        (or just the name when it is already declared).
 *
 * The declaration must be in a scope that **covers every use site**: some `regionOf` call sites
 * sit inside `{ ... }` statement blocks (trigger / owed check) and others inside allocation
 * expressions, so every declaration is emitted at the **top of the function body**
 * (`rcPredeclare`, from the base-address set collected by a first scan over the body's `EX_NEW`
 * nodes). The local is recorded in `localDef` (`own = 1`): one that no site reads is removed by
 * the dead-local pass.
 *
 * `rid` is only stored into the struct by `regionNew` and has no reader (see
 * `src/back/reclaim.c`), so passing the level number of the first encounter does not change
 * behavior.
 *
 * @param g        The generator state.
 * @param basePtr  The region base-address expression (the caller has already applied the
 *                 `(void *)` / `&` form).
 * @param lvl      The block level (only used at the moment the region is created).
 * @return         The local's name; NULL when this base address was not predeclared (the caller
 *                 falls back to the inline call).
 */
static const char *rcRegionDeclare(CG *g, const char *basePtr, int lvl) {
    for (int i = 0; i < g->rcRegN; i++)
        if (strcmp(g->rcRegBase[i], basePtr) == 0) return g->rcRegName[i];
    if (g->rcRegN >= 40) return NULL;
    int i = g->rcRegN++;
    g->rcRegBase[i] = arenaPrintf(g->arena, "%s", basePtr);
    snprintf(g->rcRegName[i], sizeof g->rcRegName[i], "__extc_rcr%d", i);
    size_t before = g->out->len;
    cgLine(g, "ExtcRcRegion *%s = extc_rc_regionOf(%s, %d);",
           g->rcRegName[i], basePtr, lvl);
    localDef(g, before, g->rcRegName[i], 1);   /* declared inside the function body: removed when nothing reads it */
    return g->rcRegName[i];
}

/**
 * @brief Find the already predeclared region pointer local; NULL when it was not declared (the
 *        caller falls back to the inline call).
 *
 * @param g        The generator state.
 * @param basePtr  The region base-address expression.
 * @return         The local's name, or NULL.
 */
static const char *rcRegionRef(CG *g, const char *basePtr) {
    for (int i = 0; i < g->rcRegN; i++)
        if (strcmp(g->rcRegBase[i], basePtr) == 0) return g->rcRegName[i];
    return NULL;
}

typedef struct { CG *g; } RcPreCtx;
static bool rcPreStmt(void *ctx, Stmt *s);
static bool rcPreExpr(void *ctx, Expr *e);

/**
 * @brief First scan over the function body: convert the arena level of every `new` point into a
 *        "region pointer" local.
 *
 * Only `EX_NEW` is scanned: rc allocation happens only at those points (the trigger / owed check
 * and the allocation expression share the same base-address text). The base-address text is
 * **character for character** the text `exNew` generates -- both sides use
 * `arenaPrintf("(void *)&(%s)", arenaRefAt(...))`, because lookup compares by text.
 *
 * @param g     The generator state.
 * @param body  The function body statement.
 */
static void rcPredeclare(CG *g, Stmt *body) {
    if (!g->rc.reclaim || !body) return;
    /* A coroutine body is a `switch (pc)` + `goto` resume state machine: resuming can **jump into**
     * the middle of the body, so a local computed once at function entry is uninitialized on the
     * path that skips it (gcc reports `-Wmaybe-uninitialized`). Allocations inside a coroutine
     * frame stay inline at the call site and are not hoisted. */
    if (g->coroFunc) return;
    RcPreCtx c = { g };
    AstVisit v = { rcPreExpr, rcPreStmt, &c };
    astWalkStmtChildren(body, &v);
}

/**
 * @brief Statement callback: keep walking (`new` may be in any statement).
 *
 * @param ctx  The prescan context (`RcPreCtx *`).
 * @param s    The child statement being visited.
 * @return     Always true.
 */
static bool rcPreStmt(void *ctx, Stmt *s) {
    RcPreCtx *c = ctx;
    AstVisit v = { rcPreExpr, rcPreStmt, c };
    return astWalkStmtChildren(s, &v);
}

/**
 * @brief Expression callback: on `EX_NEW`, register its arena base address as a "region pointer"
 *        local.
 *
 * @param ctx  The prescan context (`RcPreCtx *`).
 * @param e    The child expression being visited.
 * @return     Always true.
 */
static bool rcPreExpr(void *ctx, Expr *e) {
    RcPreCtx *c = ctx;
    CG *g = c->g;
    if (e && e->kind == EX_NEW) {
        int lvl = planArenaLevel(e);
        if (lvl == ARENA_HOME || lvl >= 1) {
            const char *base = arenaPrintf(g->arena, "(void *)&(%s)", arenaRefAt(g, lvl));
            rcRegionDeclare(g, base, lvl);
        }
    }
    AstVisit v = { rcPreExpr, rcPreStmt, c };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Give the arena an allocation point at some level refers to.
 *
 * The level is computed by the checker and recorded in `Expr.arenaLevel`: the starting point is
 * the block the statement is in, promoted when the value is stored somewhere longer-lived; in a
 * function with a home arena every site is already `ARENA_HOME`.
 *
 * There is not one fallback here that re-decides the level for the checker: returning
 * `(*__extc_home)` for every function that has a home arena would make a second authority -- the
 * checker computes a level and codegen picks another. Once the closure is built the checker
 * rewrites every site from `FuncDef.arenaSites`, so no decision is needed here.
 *
 * The caller wraps this result in `&`, so what is given here is in the dereferenced form of
 * `extc_arena *`.
 *
 * @param g      The generator state.
 * @param level  `ARENA_HOME`, or a block level of 1 and above.
 * @return       `(*__extc_home)` (the caller-chosen, longest-lived arena), or `__extc_a[level]`
 *               (freed when the block at level `level` ends); a parallel worker call chain uses
 *               the thread-local `(*extc_tls_arena)` and a coroutine step uses the frame's
 *               `arena<level>`.
 */
static const char *arenaRefAt(CG *g, int level) {
    /* On the parallel::run call chain (the worker and every function it calls) use the
     * **thread-local** arena: the frame arena (`__extc_a[]`) belongs to the main thread and two
     * workers using it at once is a data race; a worker's signature is fixed (it only writes out
     * and returns i64) => an allocation cannot escape the worker => thread-local is safe. */
    if (g->parTls) return "(*extc_tls_arena)";
    if (level == ARENA_HOME) return "(*__extc_home)";
    /* A step has no C-stack arena array: the block arenas live **in the frame** (one field per level,
     * `arena1`..`arenaN`), because a block in a coroutine can span a suspension -- the allocation has
     * to survive the resume, and the block's own release point still reclaims it. */
    if (g->coroFunc) {
        int lv = level > 0 ? level : 1;
        g->rcArenaUsed |= 1u << lv;
        return arenaPrintf(g->arena, "%s->arena%d", g->coroFrame, lv);
    }
    if (level >= 1) g->rcArenaUsed |= 1u << level;   /* this arena will hold rc objects */
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/**
 * @brief Report a consistency problem between an arena level and the block currently being
 *        generated.
 *
 * This is a drift detector for the level the checker computed: it must agree with the block
 * codegen is in at this moment, and a disagreement means the two sides are computing levels
 * separately again. It only runs with the `EXTC_DBG_ARENA` environment variable set and changes
 * no output.
 *
 * The two tests are one-sided and deliberately relaxed:
 *   - the level must not be 0: 0 means "not determined yet", so seeing it means the checker
 *     missed a site;
 *   - the level must not be deeper than the current block. Promotion only moves an allocation
 *     toward a longer lifetime, so being shallower than the current block is legal.
 *
 * @param g     The generator state, supplying the current block level and path.
 * @param e     The allocation-site expression being checked.
 * @param what  The site name, used only in the diagnostic.
 */
static void arenaDriftCheck(CG *g, Expr *e, const char *what) {
    if (dbgOn("EXTC_DBG_ARENA_VERBOSE"))
        fprintf(stderr, "[arena-ok?] %s: level=%d block=%d %s:%d\n",
                what, planArenaLevel(e), g->blkLevel, g->path, e->line);
    if (planArenaLevel(e) == 0)
        fprintf(stderr, "[arena!] %s has level 0 (undecided)  %s:%d\n", what, g->path, e->line);
    else if (planArenaLevel(e) != ARENA_HOME && planArenaLevel(e) > g->blkLevel)
        fprintf(stderr, "[arena!] %s has level %d, deeper than block %d: the two answers disagree  %s:%d\n",
                what, planArenaLevel(e), g->blkLevel, g->path, e->line);
}

/**
 * @brief The scan state for "would this expression create its pool in a given place?".
 */
typedef struct {
    int  lvl;   /**< The block level whose zone is being questioned. */
    bool hit;   /**< Some call inside the expression names that place (or names none at all). */
} ZoneUse;

static bool zoneUseExpr(ZoneUse *u, Expr *e);

/** @brief Child-expression callback that keeps walking until the answer is known. */
static bool zoneUseChild(void *ctx, Expr *e) {
    return !zoneUseExpr((ZoneUse *)ctx, e);
}

/**
 * @brief Would any call inside this expression create a pool in zone @p u->lvl ?
 *
 * `planZoneLevel` is the checker's answer to "which place this call's pool is born in": a level
 * of `k >= 1` names `__extc_zm<k>`, `ZONE_HOME` names the place this function received, and 0
 * means the checker recorded nothing -- `zoneArgRef` then passes the innermost zone that is
 * actually pushed, which is never the block that would skip its own push. Only `u->lvl` asks for
 * this block's zone.
 *
 * @param u  Scan state; `u->hit` is set when the answer is yes.
 * @param e  Expression to walk; NULL leaves the answer alone.
 * @return   `u->hit` after the walk.
 */
static bool zoneUseExpr(ZoneUse *u, Expr *e) {
    if (!e) return u->hit;
    if (e->kind == EX_DYN) {
        /* A `dyn Trait(x)` construction registers the payload pool on the place the checker
         * recorded for the site (`setCallZoneArg`), and the emission passes that same zone to
         * `extc_dyn_put`. When it is this block, the block has to push it. */
        if (planZoneLevel(e) == u->lvl) u->hit = true;
    }
    if (e->kind == EX_CALL || e->kind == EX_METHOD || e->kind == EX_ASSOC) {
        /* Only an explicit "this block" needs this block's zone. A site that recorded no level
         * resolves through `zoneArgRef`'s fallback to the innermost zone that **is** pushed -- the
         * function body or an enclosing block -- so its pool outlives us anyway.
         *
         * The level alone is not enough: every call in the block carries one (`recByte` does), and
         * counting those would keep the zone for exactly the loops this scan exists to skip. It
         * has to be a call that **creates** a pool, which is the same question `stmtMakesPool`
         * asks -- an unresolved callee stays conservative here. */
        FuncDef *f = planCallee(e);
        if (f && calleeCreatesPool(f) && (planZoneLevel(e) == u->lvl || planZoneLevel(e) == 0))
            u->hit = true;
    }
    AstVisit v = { zoneUseChild, NULL, u };
    astWalkExprChildren(e, &v);
    return u->hit;
}

/**
 * @brief May this block create a pool **in its own zone**?
 *
 * The block-level question, and the only one `genBlockBody` asks: a block pushes
 * `extc_place_enter()` so that pools created in it are released when it ends. But a call whose
 * pool is born elsewhere -- `ZONE_HOME`, that is, the place the caller received -- does not need
 * it: `string::push` upgrades its receiver's SSO buffer through `grow`, which asks for the
 * **home** zone, so in a loop body of `s.push(c)` the place would guard pools that never land in
 * it: one push and one pop per byte, both wasted.
 *
 * Only the statements' own expressions count: a nested block is a place of its own and decides
 * for itself when it is generated.
 *
 * @param b    Block statement; anything else answers false.
 * @param lvl  The block level (= the level of the zone the body would push).
 * @return     true when the block must be wrapped in a zone enter / leave pair.
 */
static bool blockMakesPoolAtLevel(Stmt *b, int lvl) {
    if (!b || b->kind != ST_BLOCK) return false;
    for (size_t i = 0; i < b->u.block.stmts.len; i++) {
        Stmt *st = *(Stmt **)vecAt(&b->u.block.stmts, i);
        /* The conservative "may reach a place" answer first: only a statement that can build a
         * `dyn` value at all is worth scanning for which place it builds it in. */
        if (!stmtMakesPool(st, false)) continue;
        ZoneUse u = { lvl, false };
        bool hit = false;
        switch (st->kind) {
        case ST_VAR:    hit = zoneUseExpr(&u, st->u.var.init); break;
        case ST_ASSIGN: hit = zoneUseExpr(&u, st->u.assign.value) || zoneUseExpr(&u, st->u.assign.target); break;
        case ST_EXPR:   hit = zoneUseExpr(&u, st->u.expr.expr); break;
        case ST_RETURN: hit = zoneUseExpr(&u, st->u.ret.value); break;
        case ST_YIELD:  hit = zoneUseExpr(&u, st->u.yield_.value); break;
        case ST_TRAP:   hit = zoneUseExpr(&u, st->u.trap_.msg); break;
        case ST_DROP:   hit = zoneUseExpr(&u, st->u.drop_.target); break;
        case ST_IF:     hit = zoneUseExpr(&u, st->u.ifs.cond); break;
        case ST_WHILE:  hit = zoneUseExpr(&u, st->u.whiles.cond); break;
        case ST_MATCH:  hit = zoneUseExpr(&u, st->u.match.scrutinee); break;
        default:        hit = false; break;
        }
        if (hit) return true;
    }
    return false;
}

static void cgZoneLeave(CG *g, int lvl);
static void cgArenaRelease(CG *g, int lvl);

/**
 * @brief Remember that this block's frame driver owns a task, so leaving the block can end it.
 *
 * @param g   The generator state.
 * @param sq  The sequence number of the spawn (the `__extc_czid<sq>` temporary).
 */
static void noteStackTask(CG *g, int sq) {
    if (!g->coroTaskAt || g->blkLevel < 0 || g->blkLevel >= g->levelCap) return;
    *(int *)vecPush(&g->coroTaskAt[g->blkLevel]) = sq;
}

/**
 * @brief End the tasks a leaving block's frames still own.
 *
 * A coroutine whose frame is a C local cannot outlive the block that declared it, but the driver
 * only calls `extc_task_end` when the step reports "done". A loop abandoned halfway (`break`) or a
 * `for` over a coroutine that is left early therefore leaks the task's arena and its place -- and
 * with them every pool the coroutine created (16 KB of pool blocks per abandoned task,
 * unbounded). The block's release point ends them; the call is idempotent, so a task that did run
 * to completion is unaffected.
 *
 * @param g    The generator state.
 * @param lvl  The block level being left.
 */
static void cgCoroTaskEndAt(CG *g, int lvl, bool clear) {
    if (!g->coroTaskAt || lvl <= 0 || lvl >= g->levelCap) return;
    Vec *v = &g->coroTaskAt[lvl];
    for (size_t i = 0; i < v->len; i++)
        cgLine(g, "extc_task_end(__extc_czid%d);", *(int *)vecAt(v, i));
    /* Only the ordinary block end clears: a break/continue release writes its end text here and the
     * block's own release text stays in the output for the fall-through path (dead on the break
     * path), exactly like a return. Clearing on the early exit would silence the normal path and
     * leak one task per completed iteration. */
    if (clear) v->len = 0;
}

/**
 * @brief Remember that this block has to end a handle's task when it is left.
 *
 * The checker only marks a handle whose every later use in the block is a drive call
 * (`noteOwnedCoroHandles`), so nothing outside can still be pointing at the task.
 *
 * @param g  The generator state.
 * @param s  The `var` statement; ignored unless the checker marked it.
 */
static void noteOwnedCoro(CG *g, Stmt *s) {
    if (!s || !planCoroOwned(s) || !g->ownedCoro) return;
    const char *cn = planCName(s);
    if (!cn || g->blkLevel < 0 || g->blkLevel >= g->levelCap) return;
    *(const char **)vecPush(&g->ownedCoro[g->blkLevel]) = cn;
}

/**
 * @brief End the tasks of the handles this block owned, if they are still alive.
 *
 * Running to completion already ended them (`extc_task_end` is idempotent); this covers the
 * handles abandoned halfway, and that is the shape whose zone and pools leak without it.
 *
 * @param g    The generator state.
 * @param lvl  The block level being left.
 */
static void cgCoroOwnedEndAt(CG *g, int lvl, bool clear) {
    if (!g->ownedCoro || lvl <= 0 || lvl >= g->levelCap) return;
    Vec *v = &g->ownedCoro[lvl];
    for (size_t i = 0; i < v->len; i++) {
        const char *cn = *(const char **)vecAt(v, i);
        cgLine(g, "if (extc_task_alive((%s).task)) extc_task_end((%s).task);", cn, cn);
    }
    if (clear) v->len = 0;            /* see cgCoroTaskEndAt: early exits do not clear */
}

/**
 * @brief Register one task id slot with the runtime's task table (`extc_task_rootPush`).
 *
 * The collector (`extc_task_collect`) frees a task-owned place once no registered slot can still
 * reach it, so registration is emitted for every slot the compiler can name and whose lifetime it
 * can balance statically: a `d.run` body's `ext f(x)` frame (the domain drives it inside that
 * block), a stack-frame spawn (the block release point ends the task), and every **local** boxed
 * handle `extc_coro h` -- the handle's `.task` field is the slot, so the collector reads the
 * slot's current value, a later assignment to the same variable needs no new registration, and a
 * handle copied into another local is registered at that local's own declaration.
 *
 * What is *not* enumerated is a handle stored into an aggregate, a container or a struct field:
 * the per-slot table cannot name those slots, so a task reachable only through such a container can
 * still be collected and driven afterwards. That direction is documented as the caller's contract
 * in `stdlib/std/sys/pool.extc`; registration only ever extends a task's life, never shortens it.
 *
 * The runtime table holds 64 slots and `extc_task_rootPush` returns 0 when it is full. The guard is
 * kept in a local, so `extc_task_rootPop` (a blind pop) runs exactly once per successful push and
 * never steals another frame's slot; see `cgTaskRootPopAt`.
 *
 * @param g        The generator state.
 * @param slotExpr C expression of the address of the task id slot (e.g. `&__extc_dtid3`).
 */
static void cgTaskRootPush(CG *g, const char *slotExpr) {
    if (!g->taskRoots || !g->taskRootAt || g->blkLevel < 0 || g->blkLevel >= g->levelCap) return;
    int trk = g->taskRootSeq++;
    cgLine(g, "int64_t __extc_trk%d = extc_task_rootPush(%s);", trk, slotExpr);
    *(int *)vecPush(&g->taskRootAt[g->blkLevel]) = trk;
}

/**
 * @brief Emit the guarded pops of the registrations one block level made, and forget them.
 *
 * The ordinary exit of the block is `atBlockEnd`; when that block is the body of a
 * `d.run { ... }` the pop has to wait for `extc_dom_run` (the tasks must stay registered while
 * they are driven, and the block's release text is written before the drive call -- see
 * ST_DOMAIN), so this call is a no-op there and the caller emits the pop afterwards.
 *
 * @param g          The generator state.
 * @param lvl        The block level whose registrations are being dropped.
 * @param atBlockEnd True for the ordinary exit of the block that made them; false for an early
 *                   exit (return / break / continue) and for the deferred pop after the drive.
 * @param clear      True only for the ordinary block end. A break/continue release emits the pops
 *                   but keeps the list, so the block's own release text (dead on the early-exit
 *                   path) still names them on the fall-through path.
 */
static void cgTaskRootPopAt(CG *g, int lvl, bool atBlockEnd, bool clear) {
    if (!g->taskRootAt || lvl <= 0 || lvl >= g->levelCap) return;
    if (atBlockEnd && g->domBodyLevel == lvl) return;
    Vec *v = &g->taskRootAt[lvl];
    for (size_t i = 0; i < v->len; i++)
        cgLine(g, "if (__extc_trk%d) extc_task_rootPop();", *(int *)vecAt(v, i));
    /* A break/continue release does not clear: the block's own release text still has to name
     * these registrations for the fall-through path (dead on the break path), exactly like
     * cgTaskRootPopsForReturn. Clearing here would leave every completed loop iteration without
     * a pop -- one stolen slot per iteration. */
    if (clear) v->len = 0;
}

/**
 * @brief Emit the pops of every level a `return` leaves, **without** clearing the lists.
 *
 * A `return` is one exit among several of the same block: the block's own release text stays in
 * the output (dead on the return path, live on the fall-through one) and every `return` of a block
 * needs the full list. Clearing here would silence the second `return` of a block, whose push
 * would then never be popped; leaving the list intact keeps every path statically balanced.
 *
 * @param g The generator state.
 */
static void cgTaskRootPopsForReturn(CG *g) {
    if (!g->taskRootAt) return;
    int top = g->blkLevel;
    if (top >= g->levelCap) top = g->levelCap - 1;
    for (int lvl = top; lvl >= 1; lvl--) {
        Vec *v = &g->taskRootAt[lvl];
        for (size_t i = 0; i < v->len; i++)
            cgLine(g, "if (__extc_trk%d) extc_task_rootPop();", *(int *)vecAt(v, i));
    }
}

/**
 * @brief Release the block arenas down to and including one level.
 *
 * Leaving a place pops back to the depth it started at (mark) instead of popping blindly: early
 * exits (return/break/continue) therefore cannot skew the zone stack.
 *
 * Emission and popping are paired through `zoneMark`, and `zoneHere` only says "this function is
 * a place": a block's zone is pushed on demand, so popping a level that pushed none pops the
 * **outer** one -- the value then lands in a place that outlives the block that pushed it. The table is written when the block is entered and read
 * here, so both sides see one fact.
 *
 * The region goes first and the arena afterwards: a pool's memory is its own malloc block and the
 * two are independent, but releasing the pools that belong to this block is exactly what this
 * block's release point is for. Only memory is closed; files are the program's own business, so
 * this single block release point has nothing to close.
 *
 * @param g    The generator state.
 * @param lvl  The innermost level to release down to; 0 or below releases nothing -- level 0 is
 *             not a block arena. A function with `noArena` (other than a coroutine step) emits
 *             nothing: it never allocates.
 */
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    cgCoroTaskEndAt(g, lvl, g->atBlockEnd);
    cgCoroOwnedEndAt(g, lvl, g->atBlockEnd);
    /* After the task ends, not before: a registration that disappears while the task is still
     * alive would let the collector free a live task-owned place. */
    cgTaskRootPopAt(g, lvl, g->atBlockEnd, g->atBlockEnd);
    if (g->noArena && !g->coroFunc) return;
    /* Memory only: files are the program's business (decision 79), so the one place a
     * block release is emitted has nothing to close. */
    /* Regions first, then the arena: a pool's memory is its own malloc block, so the two are
     * independent, but releasing the pools that belong to this block is what the block's
     * release point is for. */
    cgZoneLeave(g, lvl);
    cgArenaRelease(g, lvl);
}

/**
 * @brief Emit the zone pop of this level (only when this level pushed a zone; `zoneMark` is that
 *        table).
 *
 * Leaving a place pops back to the depth it started at (mark) instead of popping blindly: early
 * exits (return/break/continue) therefore cannot skew the zone stack.
 *
 * Emission and popping are paired through `zoneMark`, and `zoneHere` only says "this function is a
 * place": a block's zone is pushed on demand, so popping a level that pushed none pops the
 * **outer** one (the value then lands in a place that outlives the block that pushed it). The table is written when the block is entered and read
 * here, so both sides see one fact and cannot drift apart.
 *
 * @param g    The generator state.
 * @param lvl  The block level; out of range or a level that pushed no zone emits nothing.
 */
static void cgZoneLeave(CG *g, int lvl) {
    if (lvl <= 0 || lvl >= g->levelCap || !g->zoneMark[lvl]) return;
    cgLine(g, "extc_place_leaveTo(__extc_zm%d);", lvl);
}

/**
 * @brief Emit the release of this level's arena.
 *
 * On an empty arena it is nearly free, but **it is a call**: inside an inner loop body that alone
 * is enough to block vectorization. The call site therefore first asks whether this block touched
 * this level (`arenaLevelInText`) and emits nothing when it did not -- the same holds for clearing
 * at entry.
 *
 * A coroutine's block levels live in the frame (`<frame>->arenaN`): a coroutine body does not own
 * its place, so it must not release the **caller's** arena levels -- that would release, on every
 * resume, what the next resume still needs. Its block arenas die with the frame, and this block's
 * release point reclaims its own level as usual.
 *
 * **A block's release does not drop the reclamation region**: a region is claimed by the arena's
 * address, and sibling blocks at one level share a single arena while a loop body reuses it every
 * iteration -- dropping the region here would make the next iteration's `regionOf` create a new
 * **1 MiB slab**, paying allocation and zeroing on every iteration, and a long loop would stop
 * making progress. The region lives until the frame is destroyed (only `extc_arena_destroy` calls
 * `regionDrop`): every object in the block is unreachable, sweeping returns them to the free list
 * by pressure and the slots are reused across iterations -- memory stays bounded with no
 * per-iteration cost.
 *
 * @param g    The generator state.
 * @param lvl  The block level; 0 or below emits nothing -- level 0 is not a block arena. A
 *             function with `noArena` (other than a coroutine step) emits nothing: it never
 *             allocates.
 */
static void cgArenaRelease(CG *g, int lvl) {
    if (lvl <= 0) return;
    /* A step keeps `noArena` (its prologue has no `__extc_a` and its returns take the direct path),
     * but it still releases block levels -- out of the frame's per-level arenas. */
    if (g->noArena && !g->coroFunc) return;
    if (g->coroFunc)
        cgLine(g, "extc_arena_release(&%s->arena%d);", g->coroFrame, lvl);
    else
        cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/**
 * @brief Destroy the block-level arenas of a coroutine's frame where it finishes.
 *
 * A step function does `return true` on every `yield` and never takes the shared epilogue; the
 * coroutine body's own level lives until the task ends rather than until the step returns, so the
 * levels a finished coroutine still owns must be released here, and before the driver's
 * `extc_task_end` releases the frame itself. A coroutine discarded halfway and never finished does
 * not reach this, so the blocks its frame arenas still hold are not released here; the leak is
 * bounded by a delay bound.
 *
 * @param g  The generator state; nothing is emitted when `g->coroFunc` is null (not inside a
 *           coroutine body).
 */
static void cgCoroArenaDestroy(CG *g) {
    if (!g->coroFunc) return;
    int n = 1 + blkMaxOfBlock(((FuncDef *)g->coroFunc)->body);
    for (int lv = 1; lv <= n; lv++) {
        if (g->rc.reclaim)
            cgLine(g, "extc_rc_regionDrop((void *)&%s->arena%d);", g->coroFrame, lv);
        cgLine(g, "extc_arena_destroy(&%s->arena%d);", g->coroFrame, lv);
    }
}

/* Collect every `@overwrite` site of one function body, in source order.
 *
 * The order is what makes the numbering stable: the prologue emits one storage
 * cell per site in this same order, and owIndex looks a site up by position.
 */
/* Defined below; `collectOwSites` walks a domain block's receiver with it. */
static void collectOwCallsExpr(Expr *e, Vec *out);

/**
 * @brief Collect every `@overwrite` site in a function body in source order.
 *
 * **Order** is what makes the numbering stable: the prologue emits one storage cell per site in
 * the same order and `owIndex` looks the site up by position. A `d.run { ... }` domain block's
 * receiver is an expression and is collected by `collectOwCallsExpr`.
 *
 * @param s    The statement to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Stmt *` in order.
 */
static void collectOwSites(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.overwrite) *(Stmt **)vecPush(out) = s;
        return;
    /* `d.run { ... }`: the receiver is an expression and the block is a body -- collect both, the
     * same shape as `ST_IF`. */
    case ST_DOMAIN: collectOwCallsExpr(s->u.domain_.callee, out); collectOwSites(s->u.domain_.body, out); return;    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwSites(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_IF:
        collectOwSites(s->u.ifs.thenBody, out);
        collectOwSites(s->u.ifs.elseBody, out);
        return;
    case ST_WHILE: collectOwSites(s->u.whiles.body, out); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwSites((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;
    }
}

/**
 * @brief Return a site's index within the current function.
 *
 * The index is **looked up**, not stored in the AST: one generic body is walked again for every
 * instance, so a stored index would be overwritten by the next instance. The prologue emits cells
 * in the order of `g->owSitesOwn`, so both sides agree.
 *
 * @param g  The generator state; looked up in `g->owSitesOwn`.
 * @param s  The site statement.
 * @return   Its position in `g->owSitesOwn`; -1 when it is not registered.
 */
static int owIndex(CG *g, Stmt *s) {
    for (size_t i = 0; i < g->owSitesOwn.len; i++)
        if (*(Stmt **)vecAt(&g->owSitesOwn, i) == s) return (int)i;
    return -1;
}

/* Collect the call sites whose callee needs @overwrite cells.
 *
 * Every expression kind has to be covered. A missed one leaves that call site
 * without cells, so the callee receives NULL; it has a fallback for that, so the
 * program stays defined, but the storage is then allocated afresh on every call
 * instead of being reused.
 */
static void collectOwCallsExpr(Expr *e, Vec *out);
static void collectOwCallsStmt(Stmt *s, Vec *out);

/**
 * @brief Collect, from one expression, the call sites whose callee needs an `@overwrite` cell.
 *
 * **Every expression kind must be covered**: a missed kind leaves that call site with no cell and
 * the callee receives NULL -- it has a fallback, the program stays defined, but the storage is
 * reallocated on every call instead of reused. The payload of `dyn Trait(x)` is a subexpression
 * and must be entered; each branch ends with `default:`, so a missed kind does not trigger
 * `-Wswitch`.
 *
 * @param e    The expression to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Expr *` in order.
 */
static void collectOwCallsExpr(Expr *e, Vec *out) {
    if (!e) return;
    switch (e->kind) {

    case EX_DYN:
        collectOwCallsExpr(e->u.dynv.payload, out);
        return;
    case EX_CALL:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.call.args, i), out);
        return;
    case EX_METHOD:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        collectOwCallsExpr(e->u.method.recv, out);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.method.args, i), out);
        return;
    case EX_ASSOC:
        if (planCallee(e) && planOwSites(planCallee(e)) > 0 && !planOwLocal(planCallee(e)))
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.assoc.args, i), out);
        return;
    case EX_BIN:      collectOwCallsExpr(e->u.bin.left, out); collectOwCallsExpr(e->u.bin.right, out); return;
    case EX_UN:       collectOwCallsExpr(e->u.un.operand, out); return;
    case EX_FIELD:    collectOwCallsExpr(e->u.field.obj, out); return;
    case EX_INDEX:    collectOwCallsExpr(e->u.index.obj, out); collectOwCallsExpr(e->u.index.index, out); return;
    case EX_SLICE:    collectOwCallsExpr(e->u.slice.obj, out);
                      collectOwCallsExpr(e->u.slice.lo, out);
                      collectOwCallsExpr(e->u.slice.hi, out);
                      collectOwCallsExpr(e->u.slice.lo, out); collectOwCallsExpr(e->u.slice.hi, out); return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            collectOwCallsExpr((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, out);
        return;
    /* A lambda's value is its environment: the field values are the reads that happen here;
     * the body belongs to the generated `call` method and is analysed there. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            collectOwCallsExpr((*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value, out);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.arraylit.elems, i), out);
        return;
    case EX_REF:      collectOwCallsExpr(e->u.ref.operand, out); return;
    case EX_EXT: collectOwCallsExpr(e->u.ext_.call, out); return;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:    collectOwCallsExpr(e->u.deref.operand, out); return;
    case EX_SIGN:     collectOwCallsExpr(e->u.sign.operand, out); return;
    case EX_CONV:     collectOwCallsExpr(e->u.conv.operand, out); return;
    case EX_TRY:      collectOwCallsExpr(e->u.try_.operand, out); return;
    case EX_NEW:      collectOwCallsExpr(e->u.new_.count, out); return;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.gencall.args, i), out);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.enumval.args, i), out);
        return;
    case EX_COALESCE: collectOwCallsExpr(e->u.coalesce.main, out);
                      collectOwCallsExpr(e->u.coalesce.fallback, out); return;
    default: return;      /* literals, bindings, null */
    }
}

/**
 * @brief Collect, from one statement tree, the call sites whose callee needs a cell.
 *
 * Expressions (including a `d.run { ... }` receiver, a condition and a matched expression) go to
 * `collectOwCallsExpr` and statement bodies keep recursing; which expression kinds must be covered
 * is `collectOwCallsExpr`'s responsibility.
 *
 * @param s    The statement to walk; returns immediately when it is NULL.
 * @param out  Appends each site's `Expr *` in order.
 */
static void collectOwCallsStmt(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_YIELD:  collectOwCallsExpr(s->u.yield_.value, out); return;
    case ST_TRAP:   collectOwCallsExpr(s->u.trap_.msg, out); return;
    case ST_DROP:   collectOwCallsExpr(s->u.drop_.target, out); return;
    case ST_VAR:    collectOwCallsExpr(s->u.var.init, out); return;
    case ST_ASSIGN: collectOwCallsExpr(s->u.assign.target, out); collectOwCallsExpr(s->u.assign.value, out); return;
    case ST_EXPR:   collectOwCallsExpr(s->u.expr.expr, out); return;
    case ST_RETURN: collectOwCallsExpr(s->u.ret.value, out); return;
    case ST_DOMAIN: collectOwCallsExpr(s->u.domain_.callee, out);
                    collectOwCallsStmt(s->u.domain_.body, out); return;
    case ST_IF:     collectOwCallsExpr(s->u.ifs.cond, out);
                    collectOwCallsStmt(s->u.ifs.thenBody, out); collectOwCallsStmt(s->u.ifs.elseBody, out); return;
    case ST_WHILE:  collectOwCallsExpr(s->u.whiles.cond, out); collectOwCallsStmt(s->u.whiles.body, out); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwCallsStmt(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_MATCH:
        collectOwCallsExpr(s->u.match.scrutinee, out);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwCallsStmt((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;      /* break, continue */
    }
}

/**
 * @brief Return a call site's index within the current function.
 *
 * The prologue emits cells in the order of `g->owCalls`, so both sides agree; a site that is not
 * registered yields -1 and the call site passes no cell, leaving the callee on its fallback.
 *
 * @param g  The generator state; looked up in `g->owCalls`.
 * @param e  The call-site expression.
 * @return   Its position in `g->owCalls`; -1 when it is not registered.
 */
static int owCallIndex(CG *g, Expr *e) {
    for (size_t i = 0; i < g->owCalls.len; i++)
        if (*(Expr **)vecAt(&g->owCalls, i) == e) return (int)i;
    return -1;
}

/**
 * @brief Decide whether the current function's `@overwrite` cell stays in its own stack frame.
 *
 * True for `main` and for recursive functions: their frames live on, and the answer is recorded
 * in FuncDef.owLocal.
 *
 * @param g  The generator state; reads `g->owLocalOwn`.
 * @param s  Unused; the answer belongs to the whole function and does not depend on a statement.
 * @return   true when the cell is in this frame, false otherwise.
 */
static bool f_owLocal(CG *g, Stmt *s) { (void)s; return g->owLocalOwn; }

/**
 * @brief Append one call site's `@overwrite` cell argument at the end of the argument list.
 *
 * Nothing is appended when the site is not registered: the callee has a fallback for that and the
 * program stays defined.
 *
 * @param g        The generator state, for the site-index lookup.
 * @param b        The argument-list buffer being assembled, without the closing parenthesis.
 * @param e        The call-site expression.
 * @param nargs    The number of arguments already in the list, deciding whether to prepend a
 *                 comma.
 * @param hasHome  Whether a home arena argument was already appended; it also affects the comma.
 */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome) {
    if (!planCallee(e) || planOwSites(planCallee(e)) == 0 || planOwLocal(planCallee(e))) return;
    int j = owCallIndex(g, e);
    if (j < 0) return;                       /* not registered; the callee has a fallback */
    for (int k = 0; k < planOwSites(planCallee(e)); k++)
        bufPrintf(b, "%s&__extc_owc%d_%d", (nargs || hasHome || k) ? ", " : "", j, k);
}

/* How many block levels one function can use at the same time.
 *
 * The answer sizes the `__extc_a` array, which is a fixed-length array on the
 * stack, so setting up the block arenas needs no allocation.
 */
int blkMaxLevel(Stmt *s);

/**
 * @brief Return the deepest block level inside a block.
 *
 * @param block  The block statement; when it is `NULL` or not `ST_BLOCK` the result equals
 *               `blkMaxLevel(block)`, that is, 0.
 * @return       The deepest level reached inside the block, not counting the level the block adds
 *               itself; 0 when there is no block.
 */
int blkMaxOfBlock(Stmt *block) {
    int m = 0;
    if (!block || block->kind != ST_BLOCK) return blkMaxLevel(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        /* One call per statement, into a local: calling `blkMaxLevel` twice (once to compare,
         * once to assign) doubles the work at **every** level, so n nested blocks cost 2^n. */
        int d = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));
        if (d > m) m = d;
    }
    return m;
}
/**
 * @brief Return the deepest block level reached inside one statement.
 *
 * A block counts as one level, and the loops, conditionals and `match` inside it add their own, so
 * the result is the number of arena slots the enclosing function needs (it decides the length of
 * `__extc_a`).
 *
 * @param s  The statement; 0 when it is NULL.
 * @return   `1 + blkMaxOfBlock(s)` for a block; the maximum of the branches for `if`/`match`; a
 *           `while` whose condition allocates space one level more than its body block.
 */
int blkMaxLevel(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_BLOCK: return 1 + blkMaxOfBlock(s);
    case ST_IF: {
        int a = 1 + blkMaxOfBlock(s->u.ifs.thenBody);
        int b = s->u.ifs.elseBody
                  ? 1 + (s->u.ifs.elseBody->kind == ST_BLOCK
                           ? blkMaxOfBlock(s->u.ifs.elseBody)
                           : blkMaxLevel(s->u.ifs.elseBody))
                  : 0;
        return a > b ? a : b;
    }
    case ST_WHILE:
        /* A condition that allocates gets a level of its own (see the emitter), so that shape
         * needs one more than the body's own block. */
        return (planCondAllocs(s) ? 2 : 1) + blkMaxOfBlock(s->u.whiles.body);
    /* `d.run { ... }` opens a block and `genStmtInner` emits its body through `genBlockBody`,
     * which takes one arena level; the count has to include that level, or `__extc_a[maxLv + 1]`
     * is too small for the `&__extc_a[k+1]` the body uses. */
    case ST_DOMAIN: return 1 + blkMaxOfBlock(s->u.domain_.body);
    case ST_MATCH: {
        int m = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            int d = 1 + blkMaxOfBlock(a->body);
            if (d > m) m = d;
        }
        return m;
    }
    default: return 0;
    }
}

/**
 * @brief Whether the generated block-body text mentions some arena level (`__extc_a[<lvl>]`).
 *
 * Clearing at block entry and releasing at block exit both reclaim only what this block itself
 * allocated in **this level**. Every allocation in the text
 * (`extc_arena_alloc(&__extc_a[k], ...)`), every hand-down of this level as a callee's home
 * (`&__extc_a[k]`) and every region pointer base carries this text; so "not mentioned" is "this
 * block never touched this level" and both can be skipped. Recognizing one too many (say the
 * string inside a literal) only emits too much, which is the safe direction; recognizing too few is
 * impossible -- every emission point that uses this level draws its spelling from `arenaRefAt`.
 *
 * @param b    The block-body text.
 * @param lvl  The block level.
 * @return     true when this level appears in the text.
 */
static bool arenaLevelInText(const Buf *b, int lvl) {
    char needle[32];
    int n = snprintf(needle, sizeof needle, "__extc_a[%d]", lvl);
    if (n <= 0 || (size_t)n >= sizeof needle) return true;   /* when the text cannot be assembled, treat the level as touched */
    size_t nl = (size_t)n;
    if (!b->data || b->len < nl) return false;
    for (size_t i = 0; i + nl <= b->len; i++)
        if (b->data[i] == '_' && memcmp(b->data + i, needle, nl) == 0) return true;
    return false;
}


/**
 * @brief Run the `@drop` hooks of a block's own locals, in reverse declaration order.
 *
 * Emitted at the tail of the block and again before a `return`, so both the fall-through and the
 * early-exit path release once. `break`/`continue` are not covered yet: they leak rather than run a
 * hook twice.
 *
 * @param g      The generator state.
 * @param block  Block whose locals are leaving scope.
 */
static void genBlockTailDrops(CG *g, Stmt *block, size_t limit);

/**
 * @brief Run the `@drop` hooks of every local still in scope, before a `return`.
 *
 * Innermost block first, so a local drops before the locals of the blocks enclosing it. Emitted
 * into the current output buffer; a `return` inside a nested statement is part of that statement's
 * text, so this is the same place the return itself goes.
 *
 * @param g  The generator state.
 */
static void genReturnDrops(CG *g) {
    for (size_t i = g->dropBlocks.len; i-- > 0;)
        genBlockTailDrops(g, *(Stmt **)vecAt(&g->dropBlocks, i),
                          *(size_t *)vecAt(&g->dropPos, i));
}

/** @brief Run the hooks of the blocks above `base` (innermost first); used by `break`/`continue`. */
static void genDropBlocksDownTo(CG *g, size_t base) {
    for (size_t i = g->dropBlocks.len; i-- > base;)
        genBlockTailDrops(g, *(Stmt **)vecAt(&g->dropBlocks, i),
                          *(size_t *)vecAt(&g->dropPos, i));
}

/**
 * @brief Does dropping a value of this type do anything?
 *
 * True when the type has its own `@drop` hook, or when one of its fields does. A field of a `@drop`
 * type dies with the value that contains it -- the same rule as C++ members and Rust's `Drop` on
 * fields -- so the walk is recursive. References and views are not followed: they do not own the
 * value they point at, and following them would drop something that is still alive.
 *
 * @param g      The generator state (for the instantiation context).
 * @param t      The type; may be NULL.
 * @param depth  Nesting already walked, capped so a pathological type cannot loop.
 * @return       True when a drop has an effect.
 */
static bool typeNeedsDrop(CG *g, Type *t, int depth) {
    if (!t || depth > 8) return false;
    Type *st = subst(g, t);
    /* A reference or a view names someone else's storage, so it owns nothing to hand back. Without
     * this test `ttBase` unwraps it and the field is dropped as if it were the pointee, which makes a
     * guard struct holding `mut ref buf<T>` emit a call with the wrong pointer type. */
    if (st && (st->kind == TY_REF || ttIsViewType(st))) return false;
    Type *b = ttBase(st);
    /* An enum payload dies with the value that carries it, exactly like a struct field: `?buf<T>`
     * is an `option<T>` whose `some` payload owns storage, and a `pop` hands one back. The tag
     * picks the live variant at run time, so every variant is reported and the emitter switches. */
    if (b && b->kind == TY_ENUM && b->edef) {
        for (size_t i = 0; i < b->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&b->edef->variants, i);
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (b->edef->typeParams.len > 0 && b->targs.len == b->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &b->edef->typeParams, &b->targs);
                if (typeNeedsDrop(g, pt, depth + 1)) return true;
            }
        }
        return false;
    }
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd) return false;
    if (sd->dropHook) return true;
    /* A field of a **generic instance** still names the struct's type parameters: the StructDef is
     * shared by every instance, so `buf<T>` stays `buf<T>` in `fd->type`. Substituting it with the
     * instance's arguments is what names the right hook (without this, dropping a `vector<i32>`
     * emitted `buf$buf_T_release`, a function that does not exist). */
    Vec *tps = NULL, *tas = NULL;
    if (b->kind == TY_GENERIC && b->sdef == sd && b->targs.len == sd->typeParams.len) {
        tps = &sd->typeParams;
        tas = &b->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (!fd) continue;
        if (fd->type && (fd->type->kind == TY_REF || ttIsViewType(fd->type))) continue;
        Type *ft = (tps && tas) ? ttSubstitute(g->tt, fd->type, tps, tas) : fd->type;
        if (typeNeedsDrop(g, ft, depth + 1)) return true;
    }
    return false;
}

/**
 * @brief Emit the drop of one value: its own hook first, then its fields in declaration order.
 *
 * The hook runs before the fields so it can still read them (a wrapper delegating to a field keeps
 * working). Fields are visited in declaration order, which is what makes a value's death the mirror
 * image of its construction.
 *
 * @param g      The generator state.
 * @param path   C expression naming the value, e.g. `a` or `a.inner`.
 * @param t      The value's type.
 * @param depth  Nesting already walked, capped like `typeNeedsDrop`.
 */
static void genDropValue(CG *g, const char *path, Type *t, int depth) {
    if (!t || depth > 8) return;
    Type *b = ttBase(subst(g, t));
    /* An enum carries its payloads in a tagged union, so the drop follows the tag: each variant
     * that holds something owning runs the hook on `u.<variant>._<k>`. The `switch` runs for a
     * value that is about to die, so it can read the tag in place. */
    if (b && b->kind == TY_ENUM && b->edef && enumHasPayload(b->edef)) {
        bool any = false;
        for (size_t i = 0; i < b->edef->variants.len && !any; i++) {
            Variant *v = *(Variant **)vecAt(&b->edef->variants, i);
            for (size_t k = 0; k < v->types.len && !any; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (b->edef->typeParams.len > 0 && b->targs.len == b->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &b->edef->typeParams, &b->targs);
                any = typeNeedsDrop(g, pt, depth + 1);
            }
        }
        if (!any) return;
        cgLine(g, "switch ((%s).tag) {", path);
        for (size_t i = 0; i < b->edef->variants.len; i++) {
            Variant *v = *(Variant **)vecAt(&b->edef->variants, i);
            if (v->types.len == 0) continue;
            cgLine(g, "case %s_%s:", cType(g, b), v->name);
            g->indent++;
            for (size_t k = 0; k < v->types.len; k++) {
                Type *pt = *(Type **)vecAt(&v->types, k);
                if (b->edef->typeParams.len > 0 && b->targs.len == b->edef->typeParams.len)
                    pt = ttSubstitute(g->tt, pt, &b->edef->typeParams, &b->targs);
                if (!typeNeedsDrop(g, pt, depth + 1)) continue;
                genDropValue(g, arenaPrintf(g->arena, "%s.u.%s._%zu", path, v->name, k),
                             pt, depth + 1);
            }
            cgLine(g, "break;");
            g->indent--;
        }
        cgLine(g, "default: break;");
        cgLine(g, "}");
        return;
    }
    StructDef *sd = b ? structOf(b) : NULL;
    if (!sd) return;
    if (sd->dropHook) cgLine(g, "%s(&(%s));", cMethodName(g, b, sd->dropHook), path);
    /* See `typeNeedsDrop`: a field of a generic instance still names the struct's type parameters,
     * so it has to be substituted with this instance's arguments before its own drop is named. */
    Vec *tps = NULL, *tas = NULL;
    if (b->kind == TY_GENERIC && b->sdef == sd && b->targs.len == sd->typeParams.len) {
        tps = &sd->typeParams;
        tas = &b->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (!fd) continue;
        if (fd->type && (fd->type->kind == TY_REF || ttIsViewType(fd->type))) continue;
        Type *ft = (tps && tas) ? ttSubstitute(g->tt, fd->type, tps, tas) : fd->type;
        if (!typeNeedsDrop(g, ft, depth + 1)) continue;
        const char *sub = arenaPrintf(g->arena, "%s.%s", path, fd->name);
        genDropValue(g, sub, ft, depth + 1);
    }
}

/**
 * @brief Name a local for a drop: a coroutine's live-across-yield locals are frame fields.
 *
 * Inside a coroutine every mention of a frame-resident local is emitted as a field access (see
 * \`exIdent\`), and a drop is a mention like any other. Without this the block-tail drop of a value
 * with an \`@drop\` hook emitted \`&(v)\` in a body where \`v\` only exists as \`f->v\`, which the C
 * compiler rejects.
 *
 * @param g      The generator state.
 * @param cname  The C name of the local, as written in the body.
 * @return       The frame field access, or \`cname\` unchanged outside a coroutine.
 */
static const char *dropValueRef(CG *g, const char *cname) {
    if (!g->coroFunc || !cname) return cname;
    for (size_t i = 0; i < g->coroFunc->coroFrame.len; i++) {
        const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
        if (p->cname && strcmp(p->cname, cname) == 0)
            return arenaPrintf(g->arena, "%s->%s", g->coroFrame, cname);
    }
    return cname;
}

/**
 * @brief Emit the drop of every by-value parameter that owns storage, at the function's exit.
 *
 * A parameter passed by value is the callee's copy: for a type with `@drop` (or with fields that
 * have one) the callee owns it, so leaving the function has to hand the storage back -- the same
 * rule that makes a local's death the mirror image of its construction. A `ref` or a view parameter
 * names the caller's storage and drops nothing.
 *
 * The drops run right after the shared epilogue label and **before** the block arenas are destroyed:
 * a hook may still read through the value (its storage is a plate, not an arena, but the reads go
 * through the same struct).
 *
 * @param g  The generator state.
 * @param f  The function being emitted.
 */
static bool paramNeedsDrop(CG *g, Param *p) {
    if (!p || !p->type || !p->cname) return false;
    Type *pt = subst(g, p->type);
    if (!pt || pt->kind == TY_REF || ttIsViewType(pt)) return false;
    return typeNeedsDrop(g, pt, 0);
}

static bool funcHasParamDrops(CG *g, FuncDef *f) {
    if (!f) return false;
    for (size_t i = 0; i < f->params.len; i++)
        if (paramNeedsDrop(g, *(Param **)vecAt(&f->params, i))) return true;
    return false;
}

static void genParamDrops(CG *g, FuncDef *f) {
    if (!f) return;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!paramNeedsDrop(g, p)) continue;
        /* A by-value parameter whose value was moved on to another receiver is not ours to drop:
         * the receiver owns it, and the parameter slot still holds the same state. */
        if (isMovedName(g, p->cname)) continue;
        genDropValue(g, dropValueRef(g, p->cname), subst(g, p->type), 0);
    }
}

/**
 * @brief Does one local of a block hand storage back when it dies?
 *
 * The one place that answers the question, so the sweep and the "is there anything to hand back"
 * test used to shape an arena-less return cannot drift apart.
 *
 * @param g   The generator state.
 * @param st  The statement; anything but a `var` answers false.
 * @return    True when its drop has an effect and has not been taken over by `return` or an
 *            explicit `drop`.
 *
 * A `var x: T` with no initializer still **is** a value -- the zero value -- and it can own storage
 * before the block ends (`var s: string` followed by `s.append(...)` moves it off the inline
 * buffer), so it is dropped like any other. The price is a rule on hooks: a zero value has to be
 * droppable, which is why `release` in the library is written as `if (reserved > 0) { ... }` rather
 * than assuming there is something to hand back.
 */
static bool localNeedsDrop(CG *g, Stmt *st) {
    if (!st || st->kind != ST_VAR) return false;
    if (!typeNeedsDrop(g, st->type, 0)) return false;
    const char *nm = planCName(st) ? planCName(st) : st->u.var.name;
    if (!nm) return false;
    /* `return x` moves `x` out; compare both spellings, because inside a generic instance the
     * generated name carries the instance suffix while the source name does not. */
    if (g->dropSkipName &&
        (strcmp(nm, g->dropSkipName) == 0 ||
         (st->u.var.name && strcmp(st->u.var.name, g->dropSkipName) == 0)))
        return false;
    /* The value was moved out of this local by a transfer: bound to another name or handed to a
     * by-value parameter. The receiver owns the state now, so the hook here would release it a
     * second time (two `buf` values would hand the same reservation back). */
    if (isMovedName(g, nm) || (st->u.var.name && isMovedName(g, st->u.var.name))) return false;
    /* An explicit `drop x` already ran the hook for this name in this block. */
    for (size_t k = 0; k < g->dropExplicit.len; k++) {
        const char *n = *(const char **)vecAt(&g->dropExplicit, k);
        if (n && (strcmp(n, nm) == 0 || (st->u.var.name && strcmp(n, st->u.var.name) == 0)))
            return false;
    }
    return true;
}

/** @brief Emit the hooks for one block's locals; see the declaration for the timing rules. */
static void genBlockTailDrops(CG *g, Stmt *block, size_t limit) {
    /* A return in the middle of a block stops at the statement it is in: locals declared later
     * never came to life. The block's own tail passes the full count. */
    size_t n = limit < block->u.block.stmts.len ? limit : block->u.block.stmts.len;
    for (size_t i = n; i-- > 0;) {
        Stmt *st = *(Stmt **)vecAt(&block->u.block.stmts, i);
        if (!localNeedsDrop(g, st)) continue;
        const char *nm = planCName(st) ? planCName(st) : st->u.var.name;
        genDropValue(g, dropValueRef(g, nm), st->type, 0);
    }
}

/**
 * @brief Does this statement end the block on every path that reaches it?
 *
 * Only a return qualifies: a break/continue leaves through the loop's own release point,
 * which already emitted this block's exit, while a block whose last statement is an if
 * may fall through. The answer is used to skip the block-tail drop hooks. Emitting them
 * after a return is not merely unreachable code: the hook may belong to a method that
 * nothing else in the program calls, so the emitter never defines it, and the generated C
 * then calls it without a declaration -- a compile error, and one that appears only when
 * the dead call happens to be the sole use.
 *
 * @param st  The statement, or NULL for an empty block.
 * @return    True when the statement cannot fall through to the block's tail.
 */
static bool stmtEndsBlock(Stmt *st) {
    return st != NULL && st->kind == ST_RETURN;
}

static bool genReturnDropsPending(CG *g) {
    for (size_t i = g->dropBlocks.len; i-- > 0;) {
        Stmt *block = *(Stmt **)vecAt(&g->dropBlocks, i);
        size_t limit = *(size_t *)vecAt(&g->dropPos, i);
        size_t n = limit < block->u.block.stmts.len ? limit : block->u.block.stmts.len;
        for (size_t k = n; k-- > 0;)
            if (localNeedsDrop(g, *(Stmt **)vecAt(&block->u.block.stmts, k))) return true;
    }
    return false;
}

/**
 * @brief Emit one block body: clear its arena level on entry and release it on exit.
 *
 * A loop body is a block too, so each iteration clears once -- the memory a loop uses is therefore
 * bounded by one iteration, not by the iteration count.
 *
 * Both only reclaim what this block allocated in this level. The body is emitted into a temporary
 * buffer first and the decision is made after a scan: when this level was not touched both are
 * skipped. Releasing an empty arena is nearly free, but **it is a call** and inside an inner loop
 * body that alone blocks vectorization -- and a loop body that allocated nothing is exactly the
 * one to vectorize. The emission order is unchanged: clear -> zoneEnter -> body -> zoneLeave ->
 * release.
 *
 * @param g      The generator state.
 * @param block  The block statement, whose statements are emitted in order; the caller guarantees
 *               it is `ST_BLOCK` (asserted).
 */
static void genBlockBody(CG *g, Stmt *block) {
    /* Only functions with a body reach here (`genFunc` returns early for an
     * external declaration, which has none), and every other caller passes a
     * block statement it just inspected. */
    assert(block != NULL && block->kind == ST_BLOCK);
    g->blkLevel++;
    /* `genFunc` sizes `zoneMark`, `loopLevel` and `loopStep` by `levelCap`; state the bound
     * here rather than walk off the array. */
    assert(g->blkLevel < g->levelCap);
    /* Entering a block overwrites this level's mark, so the one that was there is saved
     * and put back on the way out. Level 1 is the function body, whose mark the prologue
     * set (it pushed `__extc_zm1`, and the epilogue reads the mark to decide whether to
     * pop it): a plain `= false` here would erase that decision. Two sibling blocks at the
     * same level both write it, and nothing looks at a level after its block has been
     * left, but the save costs a byte and removes the question. */
    bool savedZoneMark = g->zoneMark[g->blkLevel];
    /* Names dropped explicitly inside this block: the record is cut back when the block ends, so a
     * sibling block may reuse the name and still get its own hook. */
    size_t savedDropExplicit = g->dropExplicit.len;
    /* Block boundary of the static root table: the current registration depth is recorded on entry
     * and truncated on exit -- a local in the block stops being a root once the block ends (which
     * slots are live at a trigger point is something the compiler supplies). */
    *(size_t *)vecPush(&g->rcRootMarks) = g->rcRoots.len;
    *(Stmt **)vecPush(&g->dropBlocks) = block;
    *(size_t *)vecPush(&g->dropPos) = 0;
    /* A coroutine's block level lives in the frame and its early-exit path winds differently; a
     * `noArena` function skips both anyway. Both cases are emitted as they are, and only "one block
     * arena level in an ordinary function" is worth asking about first. */
    const bool probe = !g->coroFunc && !g->noArena;
    Buf  scratch;
    Buf *savedOut = g->out;
    bool bodyTouches = true;
    bool zoneHere = false;
    if (!g->noArena) {
        /* Entering a place: push a zone, and the mark is named by level number (blocks at one level do
         * not nest, so the names cannot collide). It is only pushed when this block **may create a
         * pool in this place** (`blockMakesPoolAtLevel`): a loop body whose pools are born in the
         * caller's place (the SSO upgrade in `string::push` is one) needs no zone of its own,
         * saving two calls per iteration. Whether a
         * pop is emitted is recorded in `zoneMark`, which `cgReleaseLevel` reads from the same
         * table. */
        g->zoneMark[g->blkLevel] = false;
        if (g->zoneHere && g->blkLevel > 1 && blockMakesPoolAtLevel(block, g->blkLevel)) {
            g->zoneMark[g->blkLevel] = true;
            zoneHere = true;
        }
    }
    if (probe) {   /* The body is emitted into a temporary buffer first and moved back to the output after the
     * scan: all three enter/exit lines sit outside the body. `g->out` points at the **local**
     * `scratch` only inside this window and is restored to `savedOut` before the function returns,
     * so the pointer never outlives the object it names. */
        bufInit(&scratch, g->arena);
        g->out = &scratch;
    }
    for (size_t i = 0; i < block->u.block.stmts.len; i++) {
        Stmt *st = *(Stmt **)vecAt(&block->u.block.stmts, i);
        /* The `for` step: `continue` jumps here, so the label has to be immediately in front of
         * it. Emitted from the same walk that emits the statement, which is what keeps the label
         * and the `goto` in agreement (the `goto` is only emitted when ST_WHILE found this exact
         * statement in the block). */
        if (planForStep(block) && st == planForStep(block) && g->loopLen > 0 && g->loopStep[g->loopLen - 1]
            && g->loopStepUsed[g->loopLen - 1])
            cgLine(g, "__extc_forstep_%d: ;", g->loopStep[g->loopLen - 1]);
        if (g->dropPos.len > 0)
            *(size_t *)vecAt(&g->dropPos, g->dropPos.len - 1) = i + 1;
        genStmt(g, st);
    }
    /* The block-tail drop hooks run while the block's own storage is still alive -- unless the
     * block ends in a return: that path already ran its hooks (cgReturn -> genReturnDrops) and
     * left the block, so a hook emitted here could never execute (see stmtEndsBlock). */
    if (block->u.block.stmts.len == 0 ||
        !stmtEndsBlock(*(Stmt **)vecAt(&block->u.block.stmts, block->u.block.stmts.len - 1)))
        genBlockTailDrops(g, block, block->u.block.stmts.len);
    if (probe) {
        g->out = savedOut;
        bodyTouches = arenaLevelInText(&scratch, g->blkLevel);
        /* Entering a block only clears the arena (its block is reused for the next iteration); the
         * reclamation region is not dropped here, for the reason given in `cgReleaseLevel`: dropping
         * it every iteration would mean creating a new 1 MiB slab every iteration. */
        if (bodyTouches) cgArenaRelease(g, g->blkLevel);   /* clear */
        if (zoneHere) cgLine(g, "int64_t __extc_zm%d = extc_place_enter();", g->blkLevel);
        bufPutn(g->out, scratch.data, scratch.len);        /* the body itself */
        /* A coroutine frame declared in this block is a C local: the driver only ends its task
         * when the step reports "done", so a `break` out of the loop (or a driver left early)
         * would leave the task's arena and place -- and every pool the coroutine created --
         * alive forever. The block's release point ends whatever is left; the call is
         * idempotent, so a task that did finish is unaffected. Emitted here rather than in
         * cgReleaseLevel because this path (the probe that measures whether the block touches
         * its arena) writes the release point itself. */
        cgCoroTaskEndAt(g, g->blkLevel, true);
        cgCoroOwnedEndAt(g, g->blkLevel, true);
        cgTaskRootPopAt(g, g->blkLevel, true, true);
        if (zoneHere) cgZoneLeave(g, g->blkLevel);
        if (bodyTouches) cgArenaRelease(g, g->blkLevel);
    } else {
        if (!g->noArena && !g->coroFunc)   /* a coroutine's storage is the task's, not this block's */
            cgArenaRelease(g, g->blkLevel);   /* clear */
        if (zoneHere && !g->coroFunc)      /* see the note on the release below: the task owns the place */
            cgLine(g, "int64_t __extc_zm%d = extc_place_enter();", g->blkLevel);
        /* This is the normal exit of the block (a break/continue release is not): only it may be
         * deferred by a `d.run` body (see `cgTaskRootPopAt`). */
        bool savedAtBlockEnd = g->atBlockEnd;
        g->atBlockEnd = true;
        cgReleaseLevel(g, g->blkLevel);
        g->atBlockEnd = savedAtBlockEnd;
    }
    g->zoneMark[g->blkLevel] = savedZoneMark;
    /* Leaving the block: the reference slots declared in it stop being roots. */
    assert(g->rcRootMarks.len > 0);
    g->rcRoots.len = *(size_t *)vecAt(&g->rcRootMarks, g->rcRootMarks.len - 1);
    g->rcRootMarks.len--;
    g->dropBlocks.len--;
    if (g->dropPos.len > 0) g->dropPos.len--;
    g->dropExplicit.len = savedDropExplicit;
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/**
 * @brief Emit one statement, with an optional statement prefix before and after it.
 *
 * What the statement needs evaluated early is written into the prefix; a non-zero `prefixBlk`
 * means a statement is being generated, and the prefix mechanism then permits emitting there. The
 * statement itself is emitted by `genStmtInner`.
 *
 * @param g  The generator state.
 * @param s  The statement to emit.
 */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

/**
 * @brief Is this slot a reference type (`ref T` / `?ref T`)?
 *
 * This is the only counting test: counting hangs **only on a change of pointed-to relation**. An
 * assignment to a data field (`p.val = x`) never touches a count, so the type is asked before
 * emission is discussed. A `T` in an instantiation context is substituted first, or a generic
 * instance would compare a parameter type against `TY_REF`.
 *
 * @param g  The generator state, supplying the instantiation context.
 * @param t  The slot's declared type; may be NULL.
 * @return   true when it is a reference type.
 */
static bool cgIsRefType(CG *g, Type *t) {
    Type *s = subst(g, t);
    return s && s->kind == TY_REF;
}

/**
 * @brief A **pure place** expression: safe to evaluate repeatedly (no call, no subscript
 *        evaluation).
 *
 * Cross-region registration has to evaluate "the object holding this slot" once more at the same
 * place (`a->f = v` passes an extra `a`), so only bindings, field chains and dereferences count:
 * evaluating them has no side effect and re-reads the same value. A subscript projection (`a[i]`)
 * would have to evaluate the index again to be safe, which costs more than it saves, so it is not
 * registered -- those positions are covered by the trigger point's root table.
 *
 * @param g  The generator state.
 * @param e  The candidate expression.
 * @return   true when it is a pure place.
 */
static bool cgPurePlace(CG *g, Expr *e) {
    (void)g;
    for (; e; ) {
        if (e->kind == EX_IDENT) return true;
        if (e->kind == EX_FIELD) { e = e->u.field.obj; continue; }
        if (e->kind == EX_DEREF) { e = e->u.deref.operand; continue; }
        return false;
    }
    return false;
}

/* The host of one reference slot: the **run-time object** that holds it. */
typedef enum {
    RC_HOST_OBJ,      /* the host object expression was found (it still has to pass cgPurePlace to be evaluated
     * repeatedly) */
    RC_HOST_LOCAL,    /* the slot lives in statically declared storage (local/global/parameter): the trigger point's
     * root table already covers it */
    RC_HOST_UNKNOWN   /* the host cannot be traced (view element, call result, ...): the conservative path is the only
     * option */
} RcHost;

