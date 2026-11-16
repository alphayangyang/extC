/* ---------------------------------------------------------------- top level */

/**
 * @brief Assemble a function's C parameter list.
 *
 * A function whose allocations may escape takes one extra hidden arena pointer at the end: its `new`
 * allocates into the arena the caller chose. An `@overwrite` cell is passed in the same way, as an
 * opaque `extc_owcell *`, so the caller need not know the type of the callee's site and a generic
 * instance needs no special case. `main`'s C signature is fixed and takes no hidden parameter; a
 * parameter written in the language (`main(args)`) is not a C parameter either.
 *
 * The implicit parameters are emitted **once**, in `cgImplicitArgs` (one each for `usesHome` and
 * `makesPool`); adding another here would give a function that only creates pools one parameter too
 * many.
 *
 * @param g  The code generation state.
 * @param f  The function definition whose parameter list is assembled.
 * @return   The parameter-list text, without the enclosing parentheses.
 */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* C fixes the signature of `main`, so it takes no hidden parameter; its
     * home arena is the local `extc_arena *__extc_home = &__extc_a[1]` in its
     * own body. A parameter the language wrote is not a C parameter either:
     * `main(args)` is built from `argc`/`argv` in the prologue, so C still sees
     * exactly the two arguments it insists on. */
    if (!f->owner && (strcmp(f->name, "main") == 0 || f->isModuleEntry)) {
        /* with a dispatcher, every entry point takes the same two C parameters: the table stores one kind
     * of function pointer */
        bufPuts(&sig, (g->needModuleTramp || f->params.len) ? "int argc, char **argv" : "void");
        return bufCstr(&sig);
    }
    if (f->params.len == 0 && !planUsesHome(f) && !planMakesPool(f) && planOwLocal(f)) {   /* no hidden parameters follow */
        bufPuts(&sig, "void"); return bufCstr(&sig);
    }
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (i) bufPuts(&sig, ", ");
        bufPrintf(&sig, "%s %s", cType(g, p->type), p->cname ? p->cname : p->name);
    }
    cgImplicitArgs(g, &sig, f, NULL, f->params.len, true);   /* declaration spelling, the same owner */
    /* A function that creates pools takes one extra hidden "home zone" -- the mirror of the hidden
     * arena parameter (`makesPool` is a transitive least fixed point, so every function along the chain
     * receives it). Where a container is "born" is therefore decided by the **caller**, not by the
     * lexical block inside the library. */
    /* Note: do **not** add `makesPool` again here -- the `cgImplicitArgs` call above already emitted
     * both kinds of implicit parameter (one each for `usesHome` and `makesPool`); one more here is "a
     * function that only creates pools gets one parameter too many" and the output becomes invalid
     * C. */
    /* @overwrite cells live in the frame of the call site and are passed in as
     * opaque `extc_owcell *`: opaque means a caller does not need to know the
     * types of the callee's sites, and a generic instance needs no special
     * case. */
    if (!planOwLocal(f)) {
        for (int i = 0; i < planOwSites(f); i++) {
            if (f->params.len || planUsesHome(f) || planMakesPool(f) || i) bufPuts(&sig, ", ");
            bufPrintf(&sig, "extc_owcell *__extc_owarg%d", i);
        }
    }
    return bufCstr(&sig);
}

/**
 * @brief Does a statement **certainly** return?
 *
 * Only the simplest shapes are recognized: `return` itself, or a block whose last statement has such
 * a shape.
 *
 * @param s  The statement; false when it is NULL.
 * @return   true when it certainly returns; anything uncertain is false. This direction is deliberate:
 *           a missed judgment only emits one extra `return` (dead code that still compiles), while the
 *           opposite would leave a missing return that gcc rejects.
 */
static bool stmtIsDefiniteReturn(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_RETURN) return true;
    if (s->kind == ST_BLOCK && s->u.block.stmts.len > 0)
        return stmtIsDefiniteReturn(*(Stmt **)vecAt(&s->u.block.stmts,
                                                    s->u.block.stmts.len - 1));
    return false;
}

/* Does this function reach itself, directly or through other calls?
 *
 * The answer decides whether a recursion guard is emitted. Unguarded runaway
 * recursion is silent: gcc either folds it into an endless loop that prints
 * nothing, or the stack overflows and the OS reports a segmentation fault,
 * which is neither an extC message nor a position in the source. Only a
 * self-recursive function pays for the guard, so an ordinary call costs one
 * increment and nothing else.
 *
 * The reachability uses the call graph the checker already recorded in
 * `planCallee(e)`, so no second AST walker is needed, and it is transitive: in the
 * mutual recursion `g -> h -> g`, `g` counts as self-recursive. That shape is the
 * one the guard is for: unguarded, it would run for 60 seconds without printing
 * anything.
 *
 * Returns:
 *   true when some path through the call graph leads back to f.
 *
 * Notes:
 *   - The graph is small and the answer is computed once per generated function,
 *     so the plainest fixed-point iteration is good enough.
 */
/* Is `f` reachable from itself? The walk itself is fine; the *dedup* is what a linear scan
 * would make cubic: scanning `seen` for every edge makes the walk from a function that reaches
 * k others cost O(k^2), paid once per emitted function. A pointer-keyed set answers the same
 * question with the same result -- only the lookup changes, never the verdict. */
typedef struct { FuncDef **slot; size_t cap, len; } FuncSet;

/**
 * @brief Initialize an empty function-pointer set, taking the slot array from an arena.
 *
 * The capacity is fixed at 64 and must be a power of two: both lookup and insertion locate a slot by
 * masking with `cap - 1`. The set itself is never freed; the old slot array replaced by a growth
 * stays in the same arena.
 *
 * @param s  The set to initialize; its previous contents are overwritten.
 * @param a  The arena the slot array is allocated from.
 */
static void funcSetInit(FuncSet *s, Arena *a) {
    s->cap = 64; s->len = 0;
    s->slot = arenaAllocZero(a, s->cap * sizeof *s->slot);
}
/**
 * @brief Hash a function pointer into a hash value for the set.
 *
 * Pointers are 16-byte aligned, so the low four bits that are always zero are shifted away first and
 * the result is multiplied by an odd number to spread it.
 *
 * @param p  The function definition pointer.
 * @return   The hash value; the caller masks it with `cap - 1` into a slot subscript.
 */
static size_t funcSetHash(FuncDef *p) { return ((size_t)(uintptr_t)p >> 4) * 2654435761u; }
/**
 * @brief Is a function pointer already in the set?
 *
 * Open addressing with linear probing: reaching an empty slot means it is absent. The premise is that
 * `s` was initialized by `funcSetInit` and that the table always keeps an empty slot -- `funcSetAdd`
 * grows it once the load factor reaches one half.
 *
 * @param s  The already initialized set.
 * @param p  The function pointer to find.
 * @return   true when it is in the set, false otherwise.
 */
static bool funcSetHas(FuncSet *s, FuncDef *p) {
    for (size_t i = funcSetHash(p) & (s->cap - 1); ; i = (i + 1) & (s->cap - 1)) {
        if (!s->slot[i]) return false;
        if (s->slot[i] == p) return true;
    }
}
/**
 * @brief Double the set's capacity and rehash every element into the new slot array.
 *
 * @param s  The already initialized set; its capacity and slot array are updated in place.
 * @param a  The arena the new slot array is allocated from; the old array is not reclaimed.
 */
static void funcSetGrow(FuncSet *s, Arena *a) {
    FuncSet bigger;
    funcSetInit(&bigger, a);
    bigger.cap = s->cap * 2;
    bigger.slot = arenaAllocZero(a, bigger.cap * sizeof *bigger.slot);
    for (size_t i = 0; i < s->cap; i++) {
        if (!s->slot[i]) continue;
        FuncDef *p = s->slot[i];
        for (size_t j = funcSetHash(p) & (bigger.cap - 1); ; j = (j + 1) & (bigger.cap - 1))
            if (!bigger.slot[j]) { bigger.slot[j] = p; bigger.len++; break; }
    }
    *s = bigger;
}
/**
 * @brief Add a function pointer to the set; when it is already there, do nothing.
 *
 * The capacity is doubled first once the set is half full, so probing always meets an empty slot.
 *
 * @param s  The already initialized set.
 * @param a  The arena used when growing.
 * @param p  The function pointer to add.
 */
static void funcSetAdd(FuncSet *s, Arena *a, FuncDef *p) {
    if (s->len * 2 >= s->cap) funcSetGrow(s, a);
    for (size_t i = funcSetHash(p) & (s->cap - 1); ; i = (i + 1) & (s->cap - 1))
        if (!s->slot[i]) { s->slot[i] = p; s->len++; return; }
        else if (s->slot[i] == p) return;
}

/* "Does f reach itself?" = "is f on a cycle of the call graph" -- a static property, but asking
 * it per emitted function over that function's whole reachable set is O(N^2). One DFS over the
 * graph marks every function that lies on a cycle -- a back edge to a still-grey node marks
 * exactly the stack slice from that node up -- and each query is then a lookup. Colours persist
 * across roots, which is sound for cycle detection: a cycle either was found from an earlier
 * root or is still all white. */
static FuncSet g_cycSeen, g_cycOn, g_cycGrey, g_cycBlack;
static int    g_cycReady;
static Vec    g_cycStack;

/**
 * @brief Depth-first walk of the call graph from `f`, marking the functions on a cycle in `g_cycOn`.
 *
 * Grey means still on the current recursion stack, black means finished. An edge to a grey node is a
 * back edge, and then the whole span of `g_cycStack` from that node up to the top is marked "on a
 * cycle". Colors are kept across root nodes, which is safe for cycle detection: a cycle is either
 * already found by an earlier root or all its nodes are still white. `g_cycStack` must already be
 * initialized by `cycleSaysRecursive`.
 *
 * @param g  The code generation context, which also provides the arena.
 * @param f  The walk's starting point; returns immediately when it is NULL or has no function body
 *           (an extern declaration).
 */
static void cycDfs(CG *g, FuncDef *f) {
    if (!f || !f->body) return;
    if (funcSetHas(&g_cycSeen, f)) return;
    funcSetAdd(&g_cycSeen, g->arena, f);
    funcSetAdd(&g_cycGrey, g->arena, f);
    *(FuncDef **)vecPush(&g_cycStack) = f;
    for (size_t j = 0; j < f->callees.len; j++) {
        FuncDef *nx = *(FuncDef **)vecAt(&f->callees, j);
        if (!nx) continue;
        if (funcSetHas(&g_cycBlack, nx)) {
            continue;                                        /* black: finished, never a back edge */
        } else if (funcSetHas(&g_cycGrey, nx)) {              /* grey ancestor: back edge */
            size_t at = 0;
            for (size_t k = 0; k < g_cycStack.len; k++)
                if (*(FuncDef **)vecAt(&g_cycStack, k) == nx) { at = k; break; }
            for (size_t k = at; k < g_cycStack.len; k++)
                funcSetAdd(&g_cycOn, g->arena, *(FuncDef **)vecAt(&g_cycStack, k));
        } else {
            cycDfs(g, nx);
        }
    }
    g_cycStack.len--;                                        /* pop */
    funcSetAdd(&g_cycBlack, g->arena, f);                     /* black: finished, never a back edge */
}

/**
 * @brief Is a function on some cycle of the call graph, that is, directly or indirectly recursive?
 *
 * The first call initializes the four sets and the walk stack; afterwards an unseen `f` gets one walk
 * from itself, and the query itself only reads `g_cycOn`. The state lives in file-level statics, all
 * allocated from `g->arena`.
 *
 * @param g  The code generation context, which also provides the arena.
 * @param f  The function to query.
 * @return   true when it is on a cycle, false otherwise.
 */
static bool cycleSaysRecursive(CG *g, FuncDef *f) {
    if (!g_cycReady) {
        g_cycReady = 1;
        funcSetInit(&g_cycSeen, g->arena);
        funcSetInit(&g_cycOn, g->arena);
        funcSetInit(&g_cycGrey, g->arena);
        funcSetInit(&g_cycBlack, g->arena);
        vecInit(&g_cycStack, g->arena, sizeof(FuncDef *));
    }
    if (!funcSetHas(&g_cycSeen, f)) cycDfs(g, f);
    return funcSetHas(&g_cycOn, f);
}

/**
 * @brief Does a function directly or indirectly call itself?
 *
 * The answer decides whether a recursion-depth guard is emitted. The decision only uses the call
 * graph already recorded by the checker (`f->callees`) instead of walking the AST again, and it is
 * transitive: in the mutual recursion `g -> h -> g`, both `g` and `h` count.
 *
 * @param g  The code generation context.
 * @param f  The function to judge.
 * @return   true when a path leads back to itself; false when `f` is NULL or has no function body.
 */
static bool funcCallsItself(CG *g, FuncDef *f) {
    if (!f || !f->body) return false;
    /* One DFS from `f` covers its whole reachable subgraph: a cycle through `f` needs no second walk. */
    return cycleSaysRecursive(g, f);
}

/* Emit one complete function definition.
 *
 * The prologue creates the block arena array, the `@overwrite` cells, the slot
 * the value is returned through and the recursion guard; then the body is
 * generated; and finally a single shared epilogue releases every arena level and
 * returns. Every piece of per-function state it changes is saved and restored,
 * because the instances of a generic generate several functions in a row.
 *
 * Params:
 *   f - the function to emit
 */
/* Every coroutine in the module declares its step **before** the bodies: a body may drive one, and
 * `main` is emitted before the other bodies. The frame itself comes from the type channel (it is a
 * unit), and the step's definition comes from `genFunc` like any other function -- which is what
 * registers the generic instances its body calls. */
/* The handle type and its two helpers. Emitted only when a handle exists somewhere; the helpers are
 * EXTC_UNUSED so that a `value` helper for an unused yield type stays quiet under -Werror. */
/**
 * @brief Do a set of type arguments still mention a type parameter, that is, is the instance not yet
 *        concrete?
 *
 * The rule matches the checker's test for a provisional instance: as soon as one argument mentions a
 * type parameter, this instance (`gen_T`) is not emitted.
 *
 * @param targs  The type argument list; treated as containing no type parameter when NULL.
 * @return       true when some argument mentions a type parameter, false otherwise.
 */
static bool coroProvisional(Vec *targs) {
    if (!targs) return false;
    for (size_t i = 0; i < targs->len; i++)
        if (mentionsParam(*(Type **)vecAt(targs, i))) return true;
    return false;
}

/**
 * @brief Will a coroutine really be emitted as a C function?
 *
 * Only a concrete instance is emitted: a generic template that still has type parameters is not, and
 * neither is a provisional instance (`gen_T`) whose type arguments still mention a type parameter. An
 * instance must be emitted -- what `genFunc` generates is the instance, and the handle dispatch table
 * dispatches on the `$step`/`$next` it generates.
 *
 * @param f  The candidate function.
 * @return   true when it is a coroutine and has become concrete; false when `f` is NULL, is not a
 *           coroutine, or is still a template.
 */
static bool coroEmitted(FuncDef *f) {
    if (!f || !planIsCoro(f)) return false;
    if (!planTemplate(f)) return f->typeParams.len == 0;
    return planTemplate(f)->typeParams.len == 0 ? true : !coroProvisional(&f->targs);
}

/**
 * @brief Emit the forward declarations and the two families of helper functions that a coroutine
 *        handle type needs.
 *
 * Nothing is emitted when `g->needCoroHandle` is false. What is emitted: the prototype of the task
 * liveness query; the `$step` prototype of every emitted coroutine, plus a `$next` prototype when it
 * needs a zone; the shared `extc_trap_dead_handle` and the `extc_coro_next` dispatcher; and finally
 * one `value`/`send` helper per yield type (one only per distinct yield type). Driving a handle whose
 * task has finished goes through the trap instead of reading a freed frame.
 *
 * @param g  The code generation context.
 * @param m  The current module; the emitted coroutines are picked from `m->funcs`.
 */
static void genCoroHandleDecls(CG *g, Module *m) {
    if (!g->needCoroHandle) return;
    cgLine(g, "/* The coroutine **handle**: one struct for every `coroutine<T>`, which is what lets");
    cgLine(g, " * handles from different coroutines live in the same container. `task` is the safety");
    cgLine(g, " * token: driving a handle whose task has ended traps loudly instead of touching freed");
    cgLine(g, " * memory (the frame lives in that task's place). */");
    (void)0;   /* the typedef itself is emitted early, before every type that mentions it */
    /* The task table is spliced in during final assembly, so name what the helpers need here --
     * the same reason the pool primitives get a prototype in `coroutine.c`. */
    cgLine(g, "int64_t extc_task_alive(int64_t id);");
    /* C allows a declaration to be repeated, and these helpers may precede the definitions below. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf)) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);", cn, cn);
        if (planCoroNeedsZone(cf))
            cgLine(g, "EXTC_UNUSED static bool %s$next(struct %s$frame *f);", cn, cn);
        /* The domain's task table stores `bool (*)(void *)`, while `$step`/`$next` take a concrete
         * frame pointer -- calling one through the other's type is undefined behaviour, so a program
         * that starts tasks gets this typed adapter. Emitted **only** for coroutines that some `ext`
         * starts (`isExtTarget`, known at check time) => every other product is byte-identical. */
    }
    /* One trap for all three helpers: every trap in this project carries a source position, and a
     * handle outliving its task is the one thing a `coroutine<T>` can get wrong. */
    cgLine(g, "EXTC_UNUSED static void extc_trap_dead_handle(const char *f, int l) {");
    cgLine(g, "    fprintf(stderr, \"%%s:%%d: trap: driving a coroutine whose task has already\"");
    cgLine(g, "                    \" ended (its frame was released with the task)\\n\", f, l);");
    cgLine(g, "    exit(70);");
    cgLine(g, "}");
    cgLine(g, "EXTC_UNUSED static bool extc_coro_next(extc_coro *h, const char *f, int l) {");
    cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
    cgLine(g, "    switch (h->kind) {");
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf)) continue;
        const char *cn = cFuncName(g, cf);
        cgLine(g, "    case %d: return %s%s((struct %s$frame *)h->frame);",
               planCoroKind(cf), cn, planCoroNeedsZone(cf) ? "$next" : "$step", cn);
    }
    cgLine(g, "    }");
    cgLine(g, "    return false;");
    cgLine(g, "}");
    /* One `value` helper per yield type that a handle can carry. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
        if (!coroEmitted(cf) || !planYieldType(cf)) continue;
        const char *yt = cType(g, planYieldType(cf));
        bool done = false;
        for (size_t k = 0; k < i && !done; k++) {
            FuncDef *prev = *(FuncDef **)vecAt(&m->funcs, k);
            if (coroEmitted(prev) && planYieldType(prev) &&
                strcmp(cType(g, planYieldType(prev)), yt) == 0) done = true;
        }
        if (done) continue;
        cgLine(g, "EXTC_UNUSED static %s extc_coro_value_%s(extc_coro *h, const char *f, int l) {",
               yt, yt);
        /* `value()` reads the frame's `ret`, so it needs the same liveness check as `next`: reading a
         * released frame is a use-after-free, not a stale number. */
        cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
        cgLine(g, "    switch (h->kind) {");
        for (size_t k = 0; k < m->funcs.len; k++) {
            FuncDef *ck2 = *(FuncDef **)vecAt(&m->funcs, k);
            if (!coroEmitted(ck2) || !planYieldType(ck2)) continue;
            if (strcmp(cType(g, planYieldType(ck2)), yt) != 0) continue;
            const char *cn2 = cFuncName(g, ck2);
            cgLine(g, "    case %d: return ((struct %s$frame *)h->frame)->ret;", planCoroKind(ck2), cn2);
        }
        cgLine(g, "    }");
        /* Unreachable in type-safe code (a handle's kind always matches its `T`); written as the
         * first case's expression so it needs no zero value of `T`. */
        cgLine(g, "    return ((struct %s$frame *)h->frame)->ret;", cFuncName(g, cf));
        cgLine(g, "}");
        /* `send`: hand a value to the resume (the frame's `in` slot), then drive it like `next`. */
        cgLine(g, "EXTC_UNUSED static bool extc_coro_send_%s(extc_coro *h, %s v, const char *f, int l) {",
               yt, yt);
        cgLine(g, "    if (!extc_task_alive(h->task)) extc_trap_dead_handle(f, l);");
        cgLine(g, "    switch (h->kind) {");
        for (size_t k = 0; k < m->funcs.len; k++) {
            FuncDef *ck3 = *(FuncDef **)vecAt(&m->funcs, k);
            if (!coroEmitted(ck3) || !planYieldType(ck3)) continue;
            if (strcmp(cType(g, planYieldType(ck3)), yt) != 0) continue;
            const char *cn3 = cFuncName(g, ck3);
            cgLine(g, "    case %d: ((struct %s$frame *)h->frame)->in = v; return %s%s("
                      "(struct %s$frame *)h->frame);",
                   planCoroKind(ck3), cn3, cn3, planCoroNeedsZone(ck3) ? "$next" : "$step", cn3);
        }
        cgLine(g, "    }");
        cgLine(g, "    return false;");
        cgLine(g, "}");
    }
}

/**
 * @brief Emit the coroutine runtime and the task table on demand, and declare every coroutine's step
 *        function.
 *
 * The task table is considered first: having any coroutine that needs a zone, or having `extc_task_*`
 * external functions declared on the extC side, both count as needing it -- and then the pool zone
 * runtime is pulled in along with it and the coroutine runtime is emitted. `genCoroHandleDecls` is
 * called next, then a `$step` prototype is emitted for every concrete coroutine; one that needs a
 * zone also gets its `$next` defined right there: it installs the frame's place as the current one
 * before entering the step, restores it afterwards, and frees the whole thing when the step reports
 * completion.
 *
 * @param g  The code generation context.
 * @param m  The current module.
 */
static void genCoroDecls(CG *g, Module *m) {
    bool taskTable = false;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *c0 = *(FuncDef **)vecAt(&m->funcs, i);
        if (c0 && planIsCoro(c0) && !planTemplate(c0) && planCoroNeedsZone(c0)) { taskTable = true; break; }
        /* A scheduler written in extC reaches the table through `std::sys::coroutine`; those
         * declarations are ordinary externs, so their presence is what pulls the runtime in. */
        if (c0 && c0->isExtern && c0->name && strncmp(c0->name, "extc_task_", 10) == 0) {
            taskTable = true; break;
        }
    }
    /* The task table, defined before every body that may spawn or drive: the pool prototypes are
     * already in the prototype region above, so this only needs to precede its users. */
    g->taskTableDone = taskTable;
    if (taskTable) {
        /* The table sits on the pool zone runtime (enter/leave), so a program that only spawns
         * coroutines has to pull that in too -- the same rule the zone-argument fallback follows. */
        g->needPool = true;
        coroutineEmitRuntime(g->arena, g->out);
    }
    g->coroDeclsDone = g->needCoroHandle;
    genCoroHandleDecls(g, m);
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* Instances emit too: skipping `planTemplate(f)` here would leave a generic coroutine
         * with no `$next` at all, and the handle dispatcher would then call `$step` directly,
         * which neither makes the task's place current nor ends the task. A *template* still has
         * type parameters left, which is what the second test excludes. */
        if (!f || !planIsCoro(f) || f->typeParams.len > 0) continue;
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f);",
               cFuncName(g, f), cFuncName(g, f));
        /* The **task driver**, defined here (before every body that may drive it): the place is read
         * from the frame -- never re-derived at a resume (rule 1) -- made current for the step, and
         * restored afterwards so the caller's place is untouched. When the step reports "done", the
         * task's whole place goes in one release: the author's B decision for slice C. */
        if (planCoroNeedsZone(f)) {
            cgLine(g, "static bool %s$next(struct %s$frame *f) {", cFuncName(g, f), cFuncName(g, f));
            cgLine(g, "    int64_t __sv = extc_zoneTop;");
            cgLine(g, "    extc_zoneTop = f->zone;");
            cgLine(g, "    bool __r = %s$step(f);", cFuncName(g, f));
            cgLine(g, "    extc_zoneTop = __sv;");
            cgLine(g, "    if (!__r) extc_task_end(f->task);   /* ran to completion: same release as a drop */");
            cgLine(g, "    return __r;");
            cgLine(g, "}");
        }
    }
}

#include <time.h>

/* Sub-phase timers for codegen, off unless EXTC_DBG_TIME=1. The front-end timers say codegen
 * is 91% of a big build; these say which part of it, because the profile's leaf shares (vecAt
 * 60%) do not name the loop behind the calls. Accumulated per name and reported once. */
/**
 * @brief Is codegen sub-phase timing enabled?
 *
 * @return   true when `EXTC_DBG_TIME` is set; the environment variable is read once and cached.
 */
static int cgTimeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }
/**
 * @brief Read the current clock for timing.
 *
 * @return   Process CPU time in seconds.
 */
static double cgNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
typedef struct { const char *name; double sec; } CgAcc;
static CgAcc cgAccs[8];
/**
 * @brief Add one sub-phase duration to the timing table indexed by name.
 *
 * Returns immediately when `EXTC_DBG_TIME` is off. The table holds at most 8 entries; a new name takes
 * the first empty slot and is dropped when there is none. The table stores only the `name` pointer
 * without copying the string, so call sites pass literals.
 *
 * @param name  The phase name.
 * @param sec   This duration in seconds.
 */
static void cgAcc(const char *name, double sec) {
    if (!cgTimeOn()) return;
    for (size_t i = 0; i < 8; i++) {
        if (!cgAccs[i].name) { cgAccs[i].name = name; cgAccs[i].sec = sec; return; }
        if (strcmp(cgAccs[i].name, name) == 0) { cgAccs[i].sec += sec; return; }
    }
}
#define CGACC(name, call) do { double t_ = cgNow(); call; cgAcc((name), cgNow() - t_); } while (0)
static void strReport(void);   /* defined with the scan counters, below */
/* What dropUnusedLocals actually does per local, in bytes: the pass is still x4 per doubling and
 * the per-local work is all O(body), which should be linear -- so one of these four counters must
 * be growing with the program. */
static long g_locN, g_locCopy, g_locRead, g_locLine, g_cuts;
/**
 * @brief Print the local-variable scan counters to stderr.
 *
 * Does nothing when `EXTC_DBG_TIME` is off.
 */
static void locReport(void) {
    if (!cgTimeOn()) return;
    fprintf(stderr, "[locals] n=%ld copy=%lld reads=%lld lines=%lld cuts=%ld\n",
            g_locN, (long long)g_locCopy, (long long)g_locRead, (long long)g_locLine, g_cuts);
}
/**
 * @brief Print the code generation timing and scan counter report.
 *
 * Does nothing when `EXTC_DBG_TIME` is off. It prints the string scan counters, the local variable
 * counters, and the accumulated duration of every sub-phase in turn.
 */
static void cgReport(void) {
    if (!cgTimeOn()) return;
    strReport(); locReport();
    for (size_t i = 0; i < 8 && cgAccs[i].name; i++)
        fprintf(stderr, "[time] %-9s %.3f s\n", cgAccs[i].name, cgAccs[i].sec);
}

/**
 * @brief Emit one complete function definition: prologue, body and shared epilogue.
 *
 * The prologue prepares the block arena array, the `@overwrite` storage cells, the slot the return
 * value passes through and the recursion guard; the body is generated next, and finally the same
 * epilogue releases every arena level and returns. Every piece of per-function state it touches is
 * saved on entry and restored on exit, because one generic generates several functions in a row. The
 * entry has three shapes: `main` uses C's fixed signature (`argc`/`argv` wrapped into the
 * language-declared view in the prologue), a coroutine step is emitted as `$step`, and everything else
 * is an ordinary function.
 *
 * @param g  The code generation context.
 * @param f  The function to emit.
 */
static void genFuncInner(CG *g, FuncDef *f);

/**
 * @brief Emit one function definition, with `g->path` set to the file the body was written in.
 *
 * Every run-time check the generated code can raise prints a `file:line` pair. The line travels
 * with the checked node, so the file has to travel with the function too: a check emitted inside a
 * library helper must name the library file, not whichever entry file happened to include it.
 * Restoring the entry path afterwards keeps the rest of the emission (whole-program thunks and
 * tables) addressing the program the user compiled.
 *
 * @param g  Code generator.
 * @param f  Function to emit; its `srcPath` may be NULL for compiler-synthesised functions.
 */
static void genFunc(CG *g, FuncDef *f) {
    const char *saved = g->path;
    if (f->srcPath) g->path = f->srcPath;
    genFuncInner(g, f);
    g->path = saved;
}

/**
 * @brief Body of `genFunc`: the emitter proper.
 *
 * @param g  Code generator.
 * @param f  Function to emit.
 */
static void genFuncInner(CG *g, FuncDef *f) {
    /* The coroutine protocols (`next`/`value`) have no body: they are emitted inline at their call
     * sites (see genMethodCall). Never emit one as a function, whichever path got here. */
    if (planCoroProto(f)) return;
    /* A coroutine's step is generated **through this flow**:
     * emitting a body is what registers the generic instances it calls, and a definition produced
     * outside this flow called helpers nothing ever emitted. Only two places differ -- the signature
     * below and the ending at the epilogue. */
    if (planIsCoro(f)) {
        g->coroFunc     = f;
        g->coroFrame    = "f";
        g->coroYieldSeq = 0;
    }
    bool isMain = cgIsMain(f);
    /* An entry point: `main`, or a function marked `@module`. C (or the dispatcher, when there is one)
     * hands the command line to both, and both return `int`; with a dispatcher the real `main` is also
     * renamed to a static helper and C's `main` is the dispatcher. */
    bool isEntry = isMain || (f && f->isModuleEntry);
    /* `main(args)`: the one parameter the language may declare is not a C
     * parameter (C fixes the entry signature), so `argc`/`argv` come in and the
     * view is built in the prologue. */
    bool mainArgs = isEntry && f->params.len > 0;
    if (isEntry) {
        /* C fixes the signature of `main`, so it takes no hidden parameter; its
         * home arena is one of its own block arenas. */
        cgLine(g, "%sint %s(%s) {", g->needModuleTramp ? "static " : "",
               cgEntryCName(g, f), cgParamList(g, f));
        if (g->needModuleTramp && !mainArgs) {
            cgLine(g, "    (void)argc;");   /* the dispatcher always passes two C parameters; whether they are used is this function's business */
            cgLine(g, "    (void)argv;");
        }
    } else if (g->coroFunc) {
        /* The step's signature: it takes **the frame**, not the coroutine's source parameters -- a
         * resume has no arguments to pass them again. `switch (f->pc)` is Duff's device: every `yield`
         * stores the pc and returns, and the `case` label right after it is where a resume jumps in. */
        cgLine(g, "EXTC_UNUSED static bool %s$step(struct %s$frame *f) {",
               cFuncName(g, (FuncDef *)g->coroFunc), cFuncName(g, (FuncDef *)g->coroFunc));
        cgLine(g, "    switch (f->pc) {");
        cgLine(g, "    case 0: ;");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        /* `@export`: external linkage, exactly its own name -- the same decision the prototype
         * makes (see `genFuncProto`), and the two have to agree or C sees two types. */
        bufPrintf(&sig, "%s%s %s(%s) {",
                  f->isExport ? "" : (f->isInline ? "EXTC_INLINE " : "static "),
                  cType(g, f->ret), cFuncName(g, f), cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    /* `?` needs the return type to build the value to return on failure. The
     * temporary counter restarts in every function, so each function has its own
     * `__extc_try0` and they cannot collide. */
    Type *savedRet = g->retType;
    int   savedSeq = g->tmpSeq;
    g->retType = subst(g, f->ret);
    g->inMain  = isEntry;
    /* The reclamation trigger only operates on a region this frame owns; which arena an `@overwrite` cell
     * lives in depends on whether this function receives the hidden "home arena" parameter (see the
     * note on the `rcFuncUsesHome` field). */
    g->rcFuncUsesHome = planUsesHome(f);
    g->tmpSeq = 0;
    /* Task-root registration state is per body too: the guard names restart (like the temporaries)
     * and no block of this body is a `d.run` body yet. */
    g->taskRootSeq = 0;
    g->domBodyLevel = 0;
    g->atBlockEnd = false;
    /* `@unchecked` is a property of **this body**, so it is set on entry and restored on the way
     * out. A coroutine's step function is generated from its own `FuncDef` (the one the
     * annotation sits on), so the flag covers `yield`-split bodies as well. */
    bool savedUnchecked = g->uncheckedBody;
    g->uncheckedBody = f->isUnchecked;
    /* ---- One arena per block level ----
     * The number of levels is known at compile time, being the maximum nesting
     * depth of the blocks, so the array has a fixed length and lives on the
     * stack: setting up the arenas needs no allocation. `{0}` is enough, since
     * an `extc_arena` holds only a `top` pointer and NULL means empty. The
     * function body itself is level 1; genBlockBody increments on entry. */
    int maxLv = 1 + blkMaxOfBlock(f->body);
    /* One table slot per block level, exactly like `__extc_a[maxLv + 1]` below. Allocated per
     * function because the level count is a property of this body. */
    g->levelCap = maxLv + 2;
    g->zoneMark  = (bool *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(bool));
    g->loopLevel = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    g->loopStep  = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    g->loopStepUsed = (bool *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(bool));
    g->loopRootBase = (int *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(int));
    g->coroTaskAt = (Vec *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(Vec));
    for (int lv = 0; lv < g->levelCap; lv++) vecInit(&g->coroTaskAt[lv], g->arena, sizeof(int));
    g->ownedCoro = (Vec *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(Vec));
    for (int lv = 0; lv < g->levelCap; lv++) vecInit(&g->ownedCoro[lv], g->arena, sizeof(const char *));
    g->taskRootAt = (Vec *)arenaAllocZero(g->arena, (size_t)g->levelCap * sizeof(Vec));
    for (int lv = 0; lv < g->levelCap; lv++) vecInit(&g->taskRootAt[lv], g->arena, sizeof(int));
    /* A function that puts nothing into its own block arenas does not even emit
     * the array. The checker decides that (`planMayUseArena(f)`: the body contains a
     * `new`, or calls a function that has a home arena), and about half of the
     * functions in real programs are pure computation. cgReleaseLevel skips the
     * release for them as well. */
    bool savedNoArena = g->noArena;
    bool savedOwLocal = g->owLocalOwn;
    g->owLocalOwn = planOwLocal(f);
    /* The @overwrite storage cells, one per site, declared in the prologue.
     * The order matters: a cell is initialized with `&__extc_a[1]` (see `home`
     * below), so the list of sites has to be collected before the decision
     * whether to emit the arena array at all can be taken. */
    Vec savedOw = g->owSitesOwn;              /* by value: no arena exists for the first function */
    Vec owNow; vecInit(&owNow, g->arena, sizeof(Stmt *));
    collectOwSites(f->body, &owNow);
    g->owSitesOwn = owNow;
    Vec savedOwCalls = g->owCalls;
    Vec owcNow; vecInit(&owcNow, g->arena, sizeof(Expr *));
    collectOwCallsStmt(f->body, &owcNow);
    g->owCalls = owcNow;
    /* A cell is initialized with `&__extc_a[1]`, so the arena array must be emitted whenever
     * there is any cell, even in a function that allocates nothing. */
    g->noArena = !planMayUseArena(f) && !(owNow.len > 0 || owcNow.len > 0);
    /* A step owns no arena array: it is not a place, and what it allocates belongs to the task's own
     * place. Emitting one also made it unused, which
     * `-Wall -Werror` rejects. */
    if (g->coroFunc) g->noArena = true;
    /* `main(args)` builds the view of `argv` in `&__extc_a[1]`, so the arena
     * array is needed even when the body allocates nothing. A program that only
     * prints its arguments is exactly that case. */
    if (mainArgs) g->noArena = false;
    /* `main`'s home arena **is** `&__extc_a[1]` (emitted below): it has no caller to hand
     * one in. When `main` only passes that arena down, its body holds no `new` of its own and
     * `mayUseArena` is false while the prologue still points at the array -- so force it here.
     * Every other function receives `__extc_home` as a parameter and needs no array of its own. */
    if (isEntry && planUsesHome(f)) g->noArena = false;
    /* Same reason, for pools: the frame mark and the per-block release hooks hang off the
     * arena's lifetime, so a `main` that only *uses* a pool (it allocates nothing itself)
     * still needs the frame. Phase 1 keys pools to `main`'s frame; when `new (r) T[n]`
     * lands, every function that can create one gets the same treatment. */
    if (isEntry && g->needPool) g->noArena = false;
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    /* The frame mark: pools created while this frame runs are linked to it, so leaving
     * the frame drops them whatever their block did. Emitted where
     * the arena exists, because hanging pools off an arena level is what this frame does. */
    /* Is this function "a place": only functions in the entry file are, and library functions are
     * transparent with respect to place (`pool<T>::withCap` creates a pool that belongs to its caller's
     * place). The criterion is that neither name contains `$` -- the loader turns a module's functions
     * into `mod$name`, and a generic instance also keeps a `$`-carrying name in `instName`. */
    {
        bool lib = (f->name && strchr(f->name, '$')) ||
                   (planInstName(f) && strchr(planInstName(f), '$'));
        if (dbgOn("EXTC_DBG_ZONE"))
            fprintf(stderr, "[zone] name=%s mod=%s inst=%s owner=%s line=%d\n",
                    f->name ? f->name : "-", f->modName ? f->modName : "-",
                    planInstName(f) ? planInstName(f) : "-", f->owner ? "yes" : "-", f->line);
        /* "A place" = a function in the entry file. `modName` is the module name the loader gives: library
         * methods have it, the entry file's is empty, so library functions are transparent with respect
         * to place -- a pool created in `pool<T>::withCap` belongs to its caller's place. */
        (void)lib;
        g->zoneHere = g->needPool && (isEntry || !f->modName || !*f->modName);
        /* main's C signature is fixed and cannot take a hidden parameter => it uses the current place's
         * zone. The entry helper in dispatcher mode also has a fixed C signature and follows the same
         * rule. */
        g->funcHasZoneParam = planMakesPool(f) && !isEntry;
    }
    /* The function body's zone (`__extc_zm1`) is emitted on demand for the same reason: without this
     * function, no pool would register on its frame, and pushing would push an empty zone anyway.
     * `zoneMark[1]` records that decision, and the closing `zoneLeaveTo(__extc_zm1)` consults it --
     * popping when nothing was pushed would pop **the caller's** zone. */
    g->zoneMark[1] = false;
    g->zoneFrameMarked = false;
    if (dbgOn("EXTC_DBG_ZONE"))
        fprintf(stderr, "[zonehook] %s zoneHere=%d noArena=%d makesPool=%d needPool=%d isMain=%d\n",
                f->name ? f->name : "-", (int)g->zoneHere, (int)g->noArena,
                (int)planMakesPool(f), (int)g->needPool, (int)isMain);
    /* A step is not a place of its own: the task's place (slice C) is what its storage hangs off, so
     * it must not open a zone of its own here -- that mark also went unused in the generated C. */
    if (g->zoneHere && !g->noArena && planMakesPool(f) && !g->coroFunc) {
        cgLine(g, "int64_t __extc_zm1 = extc_pool_zoneEnter();   /* the function body is one place */");
        g->zoneMark[1] = true;
        g->zoneFrameMarked = true;
    }
    /* ---- `main(args)`: wrap argc/argv into the view the language declared ----
     * The user wrote `fn main(args: slice<slice<u8>>) -> i32`; C hands in
     * `argc`/`argv`, so this is where the two meet. Three properties matter:
     *
     *   - the bytes are **not copied**. An `argv` string lives in the argument
     *     block of the process, which outlives every arena, so a view pointing
     *     at it is sound for as long as the program runs -- the same reason a
     *     string literal in read-only memory can be viewed.
     *   - the array of views does live in main's own home arena
     *     (`&__extc_a[1]`, the frame level), because it is main that owns it.
     *   - `args[0]` is the program name, exactly as C defines it, and the
     *     length is `argc`: no `+ 1` and no sentinel, so `args.len` is the
     *     truth. */
    if (mainArgs) {
        Param *p  = *(Param **)vecAt(&f->params, 0);
        Type  *pt = subst(g, p->type);                     /* slice<slice<u8>> */
        Type  *et = pt && pt->targs.len ? subst(g, *(Type **)vecAt(&pt->targs, 0)) : NULL;
        const char *at = cType(g, pt);
        const char *en = cType(g, et);                     /* slice<u8> */
        const char *nm = p->cname ? p->cname : p->name;
        cgLine(g, "%s %s;   /* built from argc/argv below */", at, nm);
        cgLine(g, "{");
        g->indent++;
        /* `self` often goes unused => mark it `(void)` explicitly: the receiver is part of the
         * **signature**, whether the body uses it is another matter, and the output should not
         * grow a warning over it. */
        if (f->params.len > 0 && strcmp((*(Param **)vecAt(&f->params, 0))->name, "self") == 0)
            cgLine(g, "(void)self;");
        cgLine(g, "int64_t __extc_argn = (int64_t)argc;");
        cgLine(g, "%s *__extc_argp = (%s *)extc_arena_alloc(&__extc_a[1],"
                   " (int64_t)(__extc_argn > 0 ? __extc_argn : 1) * (int64_t)sizeof(%s),"
                   " \"%s\", %d);", en, en, en, g->path, f->line);
        cgLine(g, "for (int64_t __extc_i = 0; __extc_i < __extc_argn; __extc_i++) {");
        g->indent++;
        cgLine(g, "const char *__extc_s = argv[__extc_i];");
        cgLine(g, "int64_t __extc_l = 0;");
        cgLine(g, "while (__extc_s[__extc_l]) __extc_l++;");
        cgLine(g, "__extc_argp[__extc_i].data = (uint8_t *)__extc_s;");
        cgLine(g, "__extc_argp[__extc_i].len = __extc_l;");
        g->indent--;
        cgLine(g, "}");
        cgLine(g, "%s.data = __extc_argp;", nm);
        cgLine(g, "%s.len = __extc_argn;", nm);
        g->indent--;
        cgLine(g, "}");
    }
    if (planOwLocal(f))
        for (size_t i = 0; i < owNow.len; i++)
            /* `home` says which arena the storage belongs to: with a home, the
             * home arena, which outlives this call and therefore really is
             * reused; otherwise level 1 of this frame, which certainly outlives
             * the statement. It is never NULL. */
            cgLine(g, "extc_owcell __extc_ow%zu = { 0, 0, %s };", i,
                   planUsesHome(f) ? "__extc_home" : "&__extc_a[1]");
    (void)owcNow;   /* no cells are prepared for a callee: they live in its frame */
    /* `owSites` must not be restored here: the body has not been generated yet,
     * and restoring it early made every lookup answer -1 and silently fall back
     * to allocating on every iteration. The restore happens at the end of
     * genFunc, together with `noArena`. */
    /* The slot the shared epilogue returns through; it is needed only when this
     * function really releases an arena on the way out. */
    bool retVoid = !g->retType || g->retType->kind == TY_VOID;
    /* `main` returns through it as well, and that is the point: its value is the process
     * exit code, so it has to survive the release sequence like any other return. The slot is
     * skipped only for `noArena`, where `cgReturn` returns the value directly and the
     * declaration would be unused. */
    bool retSlot = !g->noArena && !retVoid;
    if (retSlot)
        cgLine(g, "%s __extc_ret_v;", cType(g, g->retType));
    g->blkLevel = 0;
    g->loopLen  = 0;
    g->forStepSeq = 0;
    const char *savedFuncName = g->curFuncName;
    const FuncDef *savedFuncDef = g->curFunc;
    g->curFuncName = cFuncName(g, f);
    g->curFunc = f;
    g->rcArenaUsed = 0;
    g->rcRegN = 0;
    g->rcFreshN = 0;
    bool savedParTls = g->parTls;
    g->parTls = planParTlsArena(f);
    if (isEntry && planUsesHome(f)) {
        size_t hb = g->out->len;
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* the entry function's home arena is its own body */");
        g->rcArenaUsed |= 1u;   /* main's home arena is level 1: it can carry rc objects */
        localDef(g, hb, "__extc_home", 0);   /* the declaration sits above the body */
    }
    /* The buffered console tail has one guaranteed exit: `main` flushes on every explicit
     * `return` and in the arena epilogue, but a body that runs off its end passes through
     * neither. A trap exits from wherever it stands, and so does `sys::proc::exit`; one
     * `atexit` covers every way out, and the explicit flushes stay for the ordering they give
     * inside the program. Registered only when the program uses that console (`needCout`). */
    /* with a dispatcher, the dispatcher registers it: the entry helper is just a function that gets
     * called, and registering twice prints twice */
    if (isMain && g->needCout && !g->needModuleTramp) cgLine(g, "atexit(extc_cout_flush);");
    /* Registered after the flush, so atexit's LIFO order runs the hooks first and the flush still
     * gets the last word on anything they printed. */
    if (isMain && g->needGlobalDrops && !g->needModuleTramp) cgLine(g, "atexit(extc_drop_globals);");
    /* Only a self-recursive function needs the depth guard; see
     * funcCallsItself. Entering increments, every exit decrements, and
     * `extc_rec_enter` traps with a position once the limit is passed.
     *
     * The position reported is the line of the function declaration. The line of
     * the call site is what a user would rather see, but recursion comes back
     * around, so pointing at the declaration is the most readable choice. */
    g->isRecursive = funcCallsItself(g, f);
    if (g->isRecursive)
        cgLine(g, "extc_rec_enter(\"%s\", %d);", g->path, f->line);
    /* The owning depth of the thread-local arena (see `rcOwnKind`): a whole worker call chain
     * shares `extc_tls_arena`, and the entry frame reads 0 => it owns that region (its caller is
     * only the C runtime, so the static root table is the complete seed), while an inner frame only
     * records what it owes. The depth is maintained by each worker chain's prologue and epilogue;
     * trap/exit end the process outright and need no compensation. Frames with `noArena` take no
     * part (their epilogue emits no decrement and their prologue no increment). */
    bool tlsOwnerFrame = g->rc.reclaim && g->parTls && !g->noArena && !g->coroFunc;
    if (tlsOwnerFrame)
        cgLine(g, "EXTC_UNUSED int __extc_tlsOwner = (extc_rc_tlsDepth++ == 0);");
    /* The reclamation root table starts over per function: a parameter slot that contains a
     * reference is a root from the first line of the body. A coroutine step's only C parameter is
     * the frame (`f`): the parameters and locals that live across a `yield` live in frame fields
     * (`f->name`, see `exIdent`), so the reference leaves of those fields are registered as roots
     * using the `coroFrame` table the checker prepared. The frame is built by the caller as a
     * compound literal and zero-initialized at its starting point => an unassigned field reads 0, and
     * the root table entries pass `extc_rc_owns` in `markBegin` => a misjudgment only keeps more
     * alive. The checker guarantees that a yielded value carries no reference (the coroutine yield
     * type check in `src/check/check_top.c`) => the frame's references cannot escape past the yield
     * exit point into the caller's root table, and the seed is complete when the step sweeps only its
     * own block arena. */
    g->rcRoots.len = 0;
    g->rc.rootsUnreliable = g->rc.reclaim && g->rcGlobalsUnreliable;
    if (g->rc.reclaim && !g->coroFunc) {
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            rcRootDeclImpl(g, p->type, p->cname ? p->cname : p->name, true);
        }
    } else if (g->rc.reclaim) {
        const FuncDef *cf = g->coroFunc;
        for (size_t i = 0; i < cf->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&cf->coroFrame, i);
            if (!p->cname) continue;
            rcRootDeclImpl(g, p->type,
                       arenaPrintf(g->arena, "%s->%s", g->coroFrame, p->cname), true);
        }
    }
    size_t mb = g->out->len;
    rcScanAssigned(g, f->body);   /* which locals were assigned to (the opposite side of the "never reassigned" judgment) */
    rcScanLinked(g, f->body);     /* which locals were linked into a heap object (the evidence set for criterion 4 of point-fix
     * release) */
    rcPredeclare(g, f->body);   /* region pointer locals: declared at the top of the function, filled at the use point */
    genBlockBody(g, f->body);
    if (g->coroFunc) {
        /* The step's ending, **after** the body: falling off it finishes the coroutine. The pc gets a
         * value no `case` matches, so a further resume falls through to the closing `return false;`.
         * The switch and the function close here; the epilogue below belongs to a normal function. */
        cgCoroArenaDestroy(g);          /* falling off the body finishes the coroutine */
        cgLine(g, "f->pc = %d;", g->coroYieldSeq + 1);
        cgLine(g, "return false;");
        cgLine(g, "    }");
        cgLine(g, "return false;");
        g->curFuncName = savedFuncName;
    g->curFunc = savedFuncDef;
    g->parTls = savedParTls;
        g->parTls = savedParTls;
        g->retType = savedRet;
        g->inMain  = false;
        g->rcFuncUsesHome = false;
        g->tmpSeq = savedSeq;
        g->noArena = savedNoArena;
        g->owSitesOwn = savedOw;
        g->owCalls = savedOwCalls;
        g->owLocalOwn = savedOwLocal;
        g->coroFunc  = NULL;
        g->coroFrame = NULL;
        g->indent--;
        cgLine(g, "}");
        return;
    }
    if (isMain) {
        g->mainFuncName = g->curFuncName;      /* the text is taken once the buffer is complete */
        g->mainOff = mb;
        g->mainLen = g->out->len - mb;
    }
    g->curFuncName = savedFuncName;
    g->curFunc = savedFuncDef;
    /* Falling off the end of the body is an exit too, so the depth is
     * decremented here as well; otherwise the depth would only grow and a
     * legitimate program would be reported as a stack overflow. A function with
     * `noArena` has no epilogue below, so it must return on its own here, or the
     * trailing return would never be reached and the counter would never come
     * down. */
    bool fallsOff = !(f->body && f->body->kind == ST_BLOCK && f->body->u.block.stmts.len > 0
                      && stmtIsDefiniteReturn(*(Stmt **)vecAt(&f->body->u.block.stmts,
                                                              f->body->u.block.stmts.len - 1)));
    if (g->isRecursive) {
        cgRecLeave(g);
        /* This return is emitted only when the body can fall off its end. When
         * the last statement already returns, it is unreachable, and in a
         * function with `noArena` it would name `__extc_ret_v`, which is not
         * declared there, so gcc reported an error; in every other case it is
         * dead code and is left out. */
        if (fallsOff && g->noArena && retVoid) cgLine(g, "return;");
        /* When a non-void `noArena` function falls off the end, `__extc_ret_v` does not exist there: the
         * `extc_trapMsg` below (noreturn) closes the function instead -- do not invent a
         * `return __extc_ret_v;`. */
    }
    /* A body that runs off its end must not fall through: a non-void one would return whatever
     * the register happened to hold, or -- with an arena -- the return slot that no return
     * statement ever wrote. clang reports that as `-Wreturn-type`, and one shape it cannot see
     * through is a `match` whose arms all return: the arms become an `if / else if` chain,
     * which says nothing about being exhaustive. `extc_trapMsg` is `noreturn`, so the C
     * compiler stops asking for a value there. */
    if (fallsOff && !retVoid) {
        /* `main` is the one function C gives an implicit `return 0` for, and the checker's rule
         * for it says as much ("the return type may be absent or `i32`"): trapping there would
         * kill a program every C programmer expects to exit 0. Still a trap everywhere else:
         * the value there is genuinely undefined. */
        if (isEntry) {
            /* Falling off the end of `main` is a normal exit too: the console buffer has to be flushed
             * here. This step matters especially in an **arena-less** main -- that shape emits no
             * epilogue (the `!g->noArena` branch), and without it "the program prints nothing". */
            if (g->needCout) cgLine(g, "extc_cout_flush();");
            cgLine(g, "return 0;");
        }
        else
            cgLine(g, "extc_trapMsg(\"%s\", %d, \"a non-void function reached its end without returning\");",
                   g->path, f->line);
    }
    /* The shared epilogue: the release sequence appears once, and falling off
     * the end of the body goes through it as well. The `goto` guarantees that
     * the label has a user, so there is no -Wunused-label warning.
     *
     * Every level is released, because a return may jump out of a deeper block;
     * releasing an already empty arena is a no-op. */
    /* A `noArena` main emits no epilogue, but the "falls off the end" exit must flush the console
     * buffer as well; emitted only when the function really can fall off (`fallsOff`), otherwise the
     * line sits after a `return` as dead code. */
    if (g->noArena && isEntry && fallsOff && g->needCout)
        cgLine(g, "extc_cout_flush();");
    if (!g->noArena) {
        /* The fall-through entry. A body that cannot run off its end reaches the label through
         * its own returns, so this line would be dead code - clang says `code will never be
         * executed` for it and for the release that follows. `fallsOff` is the same answer the
         * epilogue trap uses, and the label keeps its users either way: a body that cannot fall
         * off ends with a return, and a return jumps to this label. */
        if (fallsOff) cgLine(g, "goto __extc_ret;");
        g->indent--;
        cgLine(g, "__extc_ret:");
        g->indent++;
        genParamDrops(g, f);   /* a by-value parameter is the callee's copy: hand its storage back */
        if (tlsOwnerFrame) cgLine(g, "extc_rc_tlsDepth--;");   /* paired with the prologue's increment */
        /* `destroy`, not `release`: this is the frame's last exit, so the block the
         * arena kept for the next round has to go too. Emitting `release` here would
         * leak one block per arena at every return -- bounded, but a leak. */
        for (int lv = 1; lv <= maxLv; lv++) {
            cgReclaimDropArena(g, lv);   /* the reclamation region lives as long as the arena; early exits leave through here too */
            cgLine(g, "extc_arena_destroy(&__extc_a[%d]);", lv);
        }
        /* Pop back to the outermost zone that was pushed: early exits (`goto __extc_ret`) and reaching
         * the end of the function both leave through here, and a jump out may leave several block
         * zones on the stack, so each level is popped from the deepest one. Popping too much is
         * idempotent (`zoneLeaveTo` pops until a mark), popping too little leaves a pool on a level
         * that has already been popped. `zoneMark[1]` (the frame's own zone) is decided by the
         * prologue: nothing pushed, nothing popped. */
        for (int lv = maxLv; lv >= 1; lv--)
            if (lv < g->levelCap && g->zoneMark[lv])
                cgLine(g, "extc_pool_zoneLeaveTo(__extc_zm%d);", lv);
        if (isEntry) {
            if (g->needCout) cgLine(g, "extc_cout_flush();");
            /* The C entry point returns an `int`, so the value is cast - and it **is** the
             * value: `fn main() -> i32` sets the exit code. A `main` that returns nothing
             * keeps the plain `return 0;`, which is also what a C programmer expects. */
            cgLine(g, retVoid ? "return 0;" : "return (int)__extc_ret_v;");
        }
        else if (retVoid) cgLine(g, "return;");
        else              cgLine(g, "return __extc_ret_v;");
    }
    g->retType = savedRet;
    g->inMain  = false;
    g->rcFuncUsesHome = false;
    g->tmpSeq = savedSeq;
    g->uncheckedBody = savedUnchecked;
    g->noArena = savedNoArena;
    g->owSitesOwn = savedOw;          /* restored last, see the note above */
    g->owCalls = savedOwCalls;
    g->owLocalOwn = savedOwLocal;
    g->indent--;
    cgLine(g, "}");
}

