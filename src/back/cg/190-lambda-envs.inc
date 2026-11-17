/* ---- lambda environments laid out per instance ---------------------------------------------
 *
 * A lambda written inside a generic body can capture a value whose type is a type parameter, and its
 * own signature can mention one too. The environment's C layout is then only concrete per instance,
 * so it is emitted once per instance (`LamVar`) instead of once per literal: `$lam$calc$0` becomes
 * `$lam$calc$0$calc_key` (an `i64` field holding a `key`) and `$lam$calc$0$calc_item` (holding an
 * `item`), each with its own `call` method and, when the literal is used as a bare `fn(...)` value,
 * its own thunk.
 *
 * The field list is shared with the checker's struct -- substitution happens while a field's type is
 * printed -- so one `FieldDef` reads as `key` in one layout and as `item` in the other.
 */

/**
 * @brief Whether a lambda environment's C layout depends on a type parameter.
 *
 * The layout has to be repeated per instance exactly when a field or the lambda's own signature
 * mentions one. A lambda inside a generic body that only captures concrete values keeps one layout
 * for every instance, which is what almost every closure in practice is.
 *
 * @param sd  The lambda's synthesized environment struct.
 * @return    true when the environment has to be laid out once per instance.
 */
static bool lamEnvNeedsVariants(const StructDef *sd) {
    if (!sd || !planLamScope(sd)) return false;
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt((Vec *)&sd->fields, i);
        if (fd && fd->type && mentionsParam(fd->type)) return true;
    }
    if (sd->methods.len == 1) {
        FuncDef *md = *(FuncDef **)vecAt((Vec *)&sd->methods, 0);
        if (md && funcSignatureMentionsParam(md)) return true;
    }
    return false;
}

/**
 * @brief The layout of a lambda environment for the instance currently being generated.
 *
 * @param g   The generator state; `g->lamCtx` names the instance.
 * @param sd  The lambda's synthesized environment struct.
 * @return    The layout, or NULL when `sd` is not an environment that depends on a type parameter or
 *            the current instance has no layout for it.
 */
static LamVar *lamVarFor(CG *g, const StructDef *sd) {
    if (!g->lamCtx || !sd) return NULL;
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        if (v->sd == sd && strcmp(v->ctxName, g->lamCtx) == 0) return v;
    }
    return NULL;
}

/**
 * @brief The C name of a lambda environment inside the instance currently being generated.
 *
 * @param g   The generator state.
 * @param sd  The lambda's synthesized environment struct.
 * @return    The layout's name; NULL when the environment keeps one layout (use the struct name) or
 *            the current instance has no layout for it.
 */
static const char *lamVarName(CG *g, const StructDef *sd) {
    LamVar *v = lamVarFor(g, sd);
    return v ? v->vt->name : NULL;
}

/* Scan state for `collectLamVariants`: one enclosing instance plus the walk callbacks. */
typedef struct {
    CG      *g;
    Vec     *params;      /* the enclosing instance's type parameter names (borrowed) */
    Vec     *targs;       /* its type arguments (borrowed) */
    const char *ctxName;  /* its C name */
    FuncDef *ownerFn;     /* the enclosing function instance, or NULL */
    Type    *ownerInst;   /* the enclosing type instance, or NULL */
    AstVisit v;
} LamVarScan;

static bool lamVarScanExpr(void *ctx, Expr *e);
static bool lamVarScanStmt(void *ctx, Stmt *s);

/**
 * @brief Record the layout of one lambda environment for the instance being scanned.
 *
 * The environment struct, its fields and its `call` body are shared with the checker's products; what
 * is made here is the per-instance shell: a struct type named `<env>$<instance>`, a `call` method
 * whose `self` has that type, and -- when the literal is used as a bare pointer -- a thunk.
 *
 * @param sc  The scan state; the enclosing instance comes from it.
 * @param e   The `EX_LAMBDA` node.
 */
static void lamVarAdd(LamVarScan *sc, Expr *e) {
    CG *g = sc->g;
    StructDef *sd = e->u.lambda.sdef;
    if (!sd || sd->methods.len != 1 || !lamEnvNeedsVariants(sd)) return;
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *have = *(LamVar **)vecAt(&g->lamVars, i);
        if (have->sd == sd && strcmp(have->ctxName, sc->ctxName) == 0) {
            have->thunk = have->thunk || planLamFnPtr(e);
            return;
        }
    }
    FuncDef *tcall = *(FuncDef **)vecAt(&sd->methods, 0);
    const char *envName = arenaPrintf(g->arena, "%s$%s", sd->name, sc->ctxName);

    StructDef *vsd = structDefNamedCopy(g->arena, sd, envName);
    /* The plan facts are keyed by pointer, so the shell has to carry the one that marks it as a
     * lambda environment: `lamEnvNeedsVariants` reads it back. */
    planSetLamScope(vsd, planLamScope(sd));
    vecInit(&vsd->typeParams, g->arena, sizeof(const char *));
    if (sc->params)
        for (size_t i = 0; i < sc->params->len; i++)
            *(const char **)vecPush(&vsd->typeParams) = *(const char **)vecAt(sc->params, i);

    Type *vt = arenaAllocZero(g->arena, sizeof *vt);
    vt->kind = TY_GENERIC;                       /* printed as its name; the struct is emitted as a unit */
    vt->name = envName;
    vt->sdef = vsd;
    vecInit(&vt->targs, g->arena, sizeof(Type *));
    if (sc->targs)
        for (size_t i = 0; i < sc->targs->len; i++)
            *(Type **)vecPush(&vt->targs) = *(Type **)vecAt(sc->targs, i);

    /* The `call` method of this layout: the template's body and result type, with `self` typed as this
     * layout. The body is shared, so one statement tree serves every instance. */
    FuncDef *vcall = arenaAllocZero(g->arena, sizeof *vcall);
    *vcall = *tcall;
    vcall->owner = vsd;
    /* Signatures spell the layout through a `TY_STRUCT` of the same name. A `TY_GENERIC` would be
     * substituted by `cType` into a *new* instance named after its type arguments
     * (`<env>_key`), which is not the struct that was emitted. */
    Type *vtNamed = arenaAllocZero(g->arena, sizeof *vtNamed);
    *vtNamed = *vt;
    vtNamed->kind = TY_STRUCT;
    Type *selfT = arenaAllocZero(g->arena, sizeof *selfT);
    selfT->kind = TY_REF;
    selfT->inner = vtNamed;
    selfT->mut = false;                          /* `call` reads the environment */
    Param *self = paramWithType(g->arena, *(Param **)vecAt(&tcall->params, 0), selfT);
    vecInit(&vcall->params, g->arena, sizeof(Param *));
    *(Param **)vecPush(&vcall->params) = self;
    for (size_t i = 1; i < tcall->params.len; i++)
        *(Param **)vecPush(&vcall->params) = *(Param **)vecAt(&tcall->params, i);
    vecInit(&vsd->methods, g->arena, sizeof(FuncDef *));
    *(FuncDef **)vecPush(&vsd->methods) = vcall;
    /* Both loops that emit an instance's methods keep only what the program used; this layout exists
     * exactly because something used it. */
    planSetUsed(vcall, true);
    planSetInstUsed(vcall, envName);
    /* The hidden parameters of a `call` method (`home`, `zone`, `@overwrite` cells) are decided from
     * plan facts recorded on the *template* function: the lambda body may allocate, may create a pool
     * or may write an `@overwrite` cell, and the call site passes the arguments according to those
     * facts. The clone has to answer the same way or the signature and the call disagree. */
    planSetUsesHome(vcall, planUsesHome(tcall));
    planSetMakesPool(vcall, planMakesPool(tcall));
    planSetOwLocal(vcall, planOwLocal(tcall));
    planSetOwSites(vcall, planOwSites(tcall));

    LamVar *v = arenaAllocZero(g->arena, sizeof *v);
    v->sd = sd;
    v->vsd = vsd;
    v->vt = vt;
    v->vtNamed = vtNamed;
    v->vcall = vcall;
    v->ctxName = sc->ctxName;
    v->ownerFn = sc->ownerFn;
    v->ownerInst = sc->ownerInst;
    v->thunk = planLamFnPtr(e);
    *(LamVar **)vecPush(&g->lamVars) = v;
    /* The struct definition is emitted with the other units (`emitTypes`), which is also what gives it
     * its forward typedef and dependency-ordered position. */
    *(Type **)vecPush(&g->insts) = vt;
}

/** @brief Expression callback of the per-instance lambda scan. */
static bool lamVarScanExpr(void *ctx, Expr *e) {
    LamVarScan *sc = (LamVarScan *)ctx;
    if (e && e->kind == EX_LAMBDA) lamVarAdd(sc, e);
    return astWalkExprChildren(e, &sc->v);
}

/** @brief Statement callback of the per-instance lambda scan. */
static bool lamVarScanStmt(void *ctx, Stmt *s) {
    LamVarScan *sc = (LamVarScan *)ctx;
    return astWalkStmtChildren(s, &sc->v);
}

/**
 * @brief Find every lambda environment that needs one layout per instance, and make those layouts.
 *
 * Only bodies that are really emitted can contain such a lambda, so the scan walks the function
 * instances in `g->funcs` and the methods of the type instances in `g->insts` -- the same sets the
 * two body-emission loops walk. Must run after `collectEmitted` (which fills those sets) and before
 * the prototypes, because a layout is a type and a method that both have to be declared.
 *
 * @param g  The generator state; `lamVars` and the instance list grow.
 */
static void collectLamVariants(CG *g) {
    LamVarScan sc;
    sc.g = g;
    sc.v.expr = lamVarScanExpr;
    sc.v.stmt = lamVarScanStmt;
    sc.v.ctx  = &sc;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (!f || !f->body || !planTemplate(f)) continue;
        sc.params = &planTemplate(f)->typeParams;
        sc.targs  = &f->targs;
        sc.ctxName = planInstName(f);
        sc.ownerFn = f;
        sc.ownerInst = NULL;
        if (!sc.ctxName) continue;
        astWalkStmtChildren(f->body, &sc.v);
    }
    /* `g->insts` grows as layouts are added, so the count is taken first: a layout's own fields are
     * already substituted, and no layout can contain a lambda. */
    size_t n = g->insts.len;
    for (size_t i = 0; i < n; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (!inst || inst->kind != TY_GENERIC || !inst->sdef) continue;
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (!md || !md->body || !planUsed(md)) continue;
            /* The same gate as `emitInstanceBodies`: a method body is emitted for the instances that
             * used it. */
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;
            sc.params = &inst->sdef->typeParams;
            sc.targs  = &inst->targs;
            sc.ctxName = inst->name;
            sc.ownerFn = NULL;
            sc.ownerInst = inst;
            astWalkStmtChildren(md->body, &sc.v);
        }
    }
}

/**
 * @brief Drop the one-layout forms of the environments that are now emitted per instance.
 *
 * An environment that needs one layout per instance must not also be emitted once, unsubstituted:
 * its field would say `int` where the instance writes a `key`, and its template `call` would be
 * generated with a `T` in a signature. Both are removed here, after `collectLamVariants` decided
 * which environments they are -- whether or not any layout was made (a generic body that is never
 * instantiated emits nothing either way).
 *
 * @param g  The generator state; `structs` and `funcs` are compacted in place.
 */
static void dropLamTemplates(CG *g) {
    size_t w = 0;
    for (size_t i = 0; i < g->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&g->structs, i);
        if (lamEnvNeedsVariants(sd)) continue;
        *(StructDef **)vecAt(&g->structs, w++) = sd;
    }
    g->structs.len = w;
    w = 0;
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        if (f->owner && lamEnvNeedsVariants(f->owner)) continue;
        *(FuncDef **)vecAt(&g->funcs, w++) = f;
    }
    g->funcs.len = w;
}

/**
 * @brief Emit the prototypes of the per-instance lambda layouts: their `call` methods and, where the
 *        layout is used as a bare `fn(...)` value, their thunks.
 *
 * A use site may precede any definition, and the signature is the instance's, so it is printed
 * inside that instance's substitution. The `call` prototype goes through `genFuncProto` like any other
 * body, which is also what gives `deadFuncBody` the entry it fills in later and what keeps a `call`
 * method that ignores `self` compilable under `-Werror=unused-parameter`.
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamVarProtos(CG *g) {
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        SubCtx sv = substEnter(g, v->vt);
        const char *savedCtx = g->lamCtx;
        g->lamCtx = v->ctxName;
        /* Register the prototype the way the instance loop does: `deadFuncBody` pairs by name, and
         * only a paired body reaches `markUnusedParams` -- without it a `call` that ignores its
         * `home` parameter fails `-Werror=unused-parameter`. */
        size_t pb = g->out->len;
        genFuncProto(g, v->vcall);
        {
            Buf pt;
            bufInit(&pt, g->arena);
            bufPutn(&pt, g->out->data + pb, g->out->len - pb);
            DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
            df->name  = cFuncName(g, v->vcall);
            df->proto = bufCstr(&pt);
            *(DeadFunc **)vecPush(&g->deadFuncs) = df;
        }
        g->lamCtx = savedCtx;
        if (v->thunk) {
            Buf ps;
            bufInit(&ps, g->arena);
            lamThunkParams(g, v->vcall, &ps);
            bool vret = !v->vcall->ret || v->vcall->ret->kind == TY_VOID;
            cgLine(g, "static %s %s(%s);", vret ? "void" : cType(g, v->vcall->ret),
                   arenaPrintf(g->arena, "%s$fn", cFuncName(g, v->vcall)), bufCstr(&ps));
        }
        substLeave(g, sv);
    }
}

/**
 * @brief Emit the definitions of the per-instance lambda layouts: each `call` method and, when the
 *        layout is used as a bare pointer, its thunk.
 *
 * The body is generated inside the instance's substitution context, with `lamCtx` naming the
 * instance: a lambda literal in that body becomes a literal of *this* layout, and a closure call
 * reaches *this* layout's `call`.
 *
 * @param g  The generator state; output goes to the current buffer.
 */
static void emitLamVars(CG *g) {
    for (size_t i = 0; i < g->lamVars.len; i++) {
        LamVar *v = *(LamVar **)vecAt(&g->lamVars, i);
        SubCtx sv = substEnter(g, v->vt);
        const char *savedCtx = g->lamCtx;
        g->lamCtx = v->ctxName;
        if (v->ownerInst) planEnterInst(v->ownerInst);
        else if (v->ownerFn) planEnterFunc(v->ownerFn);
        size_t fb = g->out->len;
        CGACC("cg-emit", genFunc(g, v->vcall));
        deadFuncBody(g, v->vcall, fb, g->out->len - fb);
        if (v->ownerInst) planLeaveInst();
        else if (v->ownerFn) planLeaveFunc();
        if (v->thunk) {
            const char *callName = cFuncName(g, v->vcall);
            Buf ps;
            bufInit(&ps, g->arena);
            lamThunkParams(g, v->vcall, &ps);
            bool vret = !v->vcall->ret || v->vcall->ret->kind == TY_VOID;
            cgLine(g, "");
            cgLine(g, "/* A `fn(...)` value for a lambda that captures nothing, laid out for one");
            cgLine(g, " * instance: the empty environment is a static dummy. */");
            cgLine(g, "static %s %s$fn(%s) {", vret ? "void" : cType(g, v->vcall->ret), callName,
                   bufCstr(&ps));
            cgLine(g, "    static %s __env;", cType(g, v->vtNamed));
            Buf args;
            bufInit(&args, g->arena);
            for (size_t k = 1; k < v->vcall->params.len; k++) {
                Param *p = *(Param **)vecAt(&v->vcall->params, k);
                bufPrintf(&args, "%s%s", k > 1 ? ", " : "", p->cname);
            }
            if (vret)
                cgLine(g, "    %s(&__env%s%s);", callName, args.len ? ", " : "", bufCstr(&args));
            else
                cgLine(g, "    return %s(&__env%s%s);", callName, args.len ? ", " : "",
                       bufCstr(&args));
            cgLine(g, "}");
        }
        g->lamCtx = savedCtx;
        substLeave(g, sv);
        cgLine(g, "");
    }
}


/**
 * @brief Emit the function that runs the `@drop` hooks of module-level bindings at process exit.
 *
 * A global is a `static` object whose life is the whole process, so nothing in the block machinery
 * ever reaches it: without this, a global of a type with an `@drop` hook would abandon whatever the
 * hook is supposed to hand back. `main` registers it with `atexit`, which runs it before the console
 * flush (LIFO) so anything it prints still makes it out.
 *
 * @param g  Generator; emits nothing when the module has no such global.
 */
static void emitGlobalDrops(CG *g) {
    if (!g->needGlobalDrops) return;
    cgLine(g, "static void extc_drop_globals(void) {");
    for (size_t i = 0; i < g->globalDrops.len; i++) {
        GlobalDrop *d = (GlobalDrop *)vecAt(&g->globalDrops, i);
        genDropValue(g, d->name, d->declT, 0);
    }
    cgLine(g, "}");
    cgLine(g, "");
}

/**
 * @brief Emit the unit-wide prototype pool: before every function body, and prototypes only.
 *
 * No definition may appear here: a definition preceding the prototype of a function it calls makes C
 * treat the call as an implicitly declared `int()` and the real prototype then becomes a conflicting
 * type. The parameter list must be built by the same `cgParamList` as the definition: a function with
 * a home arena has one extra hidden parameter, and without it the C types do not match. Instance
 * methods are emitted only when called (`planUsed`), and the coroutine protocol is not emitted here --
 * it is inlined at the call site.
 *
 * @param g     The code generation context; `out` receives the prototypes and `deadFuncs` the
 *              prunable entries.
 * @param m     The current module.
 * @param arena The scratch allocator for assembling signature text.
 */
static void emitProtos(CG *g, Module *m, Arena *arena) {
    /* The thunks that turn a lambda that captures nothing into a bare function pointer. They are
     * declared with everything else: a use site may precede any definition. */
    for (size_t i = 0; i < g->lamThunks.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&g->lamThunks, i);
        FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
        Buf ps;
        bufInit(&ps, g->arena);
        lamThunkParams(g, md, &ps);
        bool vret = !md->ret || md->ret->kind == TY_VOID;
        cgLine(g, "static %s %s(%s);", vret ? "void" : cType(g, md->ret), lamThunkName(g, sd),
               bufCstr(&ps));
    }
    emitLamVarProtos(g);
    /* The prototype pool: every function is declared before any body, and only
     * a prototype may appear here. A definition emitted before the prototype of
     * a function it calls makes C take that function as implicitly declared
     * `int()`, and the later real prototype is then a conflicting type.
     *
     * Printing and structural `==` go through the descriptor table, so none of
     * the derived functions needs a forward declaration here. */
    /* The prototypes of instance methods; an array has no sdef and no methods. */
    for (size_t i = 0; i < g->insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g->insts, i);
        if (inst->kind != TY_GENERIC) continue;
        if (inst->sdef && lamEnvNeedsVariants(inst->sdef)) continue;   /* see emitLamVarProtos */
        SubCtx sv = substEnter(g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *md = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            /* Only a method that is really called is emitted, which keeps the
             * generated C smaller. The flag is a conservative approximation: it
             * is set whenever the template body mentions a call, so a closure
             * such as `push` calling `grow` is included automatically, and
             * emitting too much is the safe direction. */
            if (planCoroProto(md)) continue;   /* emitted inline at the call site */
            if (!planUsed(md)) continue;
            if (md->owner && md->owner->typeParams.len > 0 &&
                !planInstUsed(md, inst->name) && !planDynTable(md))
                continue;   /* no other instance has used it: its element type may not have the operator this method needs */

            /* Instance methods are candidates too: a library method of a generic
             * instance is marked used even when only dead code mentions it, and
             * without an entry here nothing takes it back out. The pairing is by
             * name, so one node backing several instances is not a problem. */
            size_t pb = g->out->len;
            genFuncProto(g, md);
            {
                Buf pt;
                bufInit(&pt, g->arena);
                bufPutn(&pt, g->out->data + pb, g->out->len - pb);
                DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
                df->name  = cFuncName(g, md);
                df->proto = bufCstr(&pt);
                *(DeadFunc **)vecPush(&g->deadFuncs) = df;
            }
        }
        substLeave(g, sv);
    }
    /* Methods of ordinary structs and free functions: their order does not
     * matter, and mutual calls are covered by the prototypes above. */
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
        SubCtx sv = substEnterFunc(g, f);
        bool entry = cgIsEntry(g, f);
        const char *ret = entry ? "int" : cType(g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        /* The parameter list must come from the same cgParamList the definition
         * uses: a function with a home arena has one hidden parameter more, and
         * leaving it out of the prototype produced a C type mismatch, which was
         * hit for real. */
        /* An external declaration is not `static`, so that the linker can see
         * it, and only its prototype is emitted. */
        /* `@inline` has to be on the prototype too, or the attribute on the definition
         * alone does not bind: C takes the first declaration as the function's type. */
        if (planIsCoro(f)) continue;   /* a coroutine emits `$frame` + `$step` last */
        bufPrintf(&sig, "%s%s %s(%s);",
                  entry ? (g->needModuleTramp ? "static " : "")
                        : ((f->isExtern || f->isExport) ? ""
                                                        : (f->isInline ? "EXTC_INLINE " : "static ")),
                  ret, cgEntryCName(g, f), cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
        /* An exported function is registered like `main`: nowhere. The pruners drop a definition
         * only when its name occurs nowhere but its own declaration and definition, and a symbol
         * C calls is mentioned by nobody in this file -- so registering it would delete it. */
        if (!entry && !f->isExtern && !f->isExport) {
            DeadFunc *df = arenaAllocZero(g->arena, sizeof *df);
            df->name  = cFuncName(g, f);
            df->proto = bufCstr(&sig);
            /* A method a `dyn` table names is still registered: the table is spliced in after the
             * "definitions nothing names" pass, so that pass cannot see the reference, and
             * `namedByDynTable` is how it is told to keep the body. Registering it also matters for
             * the parameter pass -- an entry with no body is skipped there, and then a method that
             * ignores `self` (the ordinary shape of a trait implementation) fails the generated C
             * with `-Werror=unused-parameter`. */
            df->namedByDynTable = planDynTable(f) || inTraitTable(m, f);
            *(DeadFunc **)vecPush(&g->deadFuncs) = df;
        }
        substLeave(g, sv);
    }
    if (g->structs.len || g->insts.len || g->funcs.len) cgLine(g, "");
    if (g->needGlobalDrops) cgLine(g, "static void extc_drop_globals(void);");
}

/**
 * @brief Initialize the code generation state: instance tables, the coroutine-kind pre-scan and the
 *        event layer flags.
 *
 * Every emitted coroutine function gets an increasing `kind` (the boxing points and the handle dispatch
 * must agree), and `needCoroHandle` is set as soon as one coroutine's frame is promoted to a handle.
 * The event layer comes in only when the program uses one of `extc_epoll_*`, `extc_sock_*` or
 * `extc_tcp_*`.
 *
 * @param g     The code generation context, whose vectors and buffers are initialized here.
 * @param m     The current module; `funcs` is the source of the coroutine pre-scan.
 * @param tt    The type table; an instance that still mentions a type parameter is not emitted as C.
 * @param arena The allocator every vector and buffer in CG is allocated from.
 */
static void initCGState(CG *g, Module *m, TypeTable *tt, Arena *arena) {
    vecInit(&g->structs, arena, sizeof(void *));
    vecInit(&g->hoistedIdx, arena, sizeof(Expr *));
    bufInit(&g->prefix, arena);          /* statement prefix; uninitialized, it segfaults */
    /* The instance table is deduplicated by C name: a writable and a read-only
     * view are one C struct, so the type table can hold two instances under one
     * name and neither may be generated twice. */
    /* Coroutine handle prepass: one `kind` per coroutine function (the boxing site and the handle
     * dispatch must agree on it), and whether this program needs the handle type at all -- the
     * checker records the "a frame was coerced into a handle" fact through `planSetCoroBoxed`. */
    {
        int ck = 0;
        for (size_t i = 0; i < m->funcs.len; i++) {
            FuncDef *cf = *(FuncDef **)vecAt(&m->funcs, i);
            if (!coroEmitted(cf)) continue;
            /* Derived here rather than written back into the shared analysis
             * state: the instance's own flag counts, and so does the template's. */
            bool boxed = planCoroBoxed(cf)
                      || (planTemplate(cf) && planCoroBoxed(planTemplate(cf)));
            planSetCoroKind(cf, ck++);
            if (boxed) g->needCoroHandle = true;
        }
        /* The event layer is needed if anything used calls one of its entry points. */
        for (size_t fi = 0; fi < m->funcs.len; fi++) {
            FuncDef *ef = *(FuncDef **)vecAt(&m->funcs, fi);
            if (ef && planUsed(ef) && ef->name &&
                (strncmp(ef->name, "extc_epoll_", 11) == 0 || strncmp(ef->name, "extc_sock_", 10) == 0 ||
                 strncmp(ef->name, "extc_tcp_", 9) == 0))
                g->needEvent = true;
        }
    }
    vecInit(&g->insts, arena, sizeof(void *));
    vecInit(&g->moduleEntries, arena, sizeof(ModEntry));
    /* Reclamation's compile-time state: all three vectors start empty and are set up by `vecInit`
     * rather than by CG being zeroed. */
    vecInit(&g->rcRoots, arena, sizeof(const char *));
    vecInit(&g->rcRootMarks, arena, sizeof(size_t));
    vecInit(&g->dropBlocks, arena, sizeof(Stmt *));
    vecInit(&g->dropPos, arena, sizeof(size_t));
    vecInit(&g->dropExplicit, arena, sizeof(const char *));
    vecInit(&g->dropLoopBases, arena, sizeof(size_t));
    vecInit(&g->movedNames, arena, sizeof(const char *));
    vecInit(&g->globalDrops, arena, sizeof(GlobalDrop));
    vecInit(&g->rcGlobalRoots, arena, sizeof(const char *));
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->instances, i);
        /* A type that still mentions a type parameter gets no C: `[2]box<T>`
         * inside a generic body is interned unconditionally, and emitting its
         * struct would name the undefined `box_T`. */
        if (!it || mentionsParam(it)) continue;
        bool dup = false;
        for (size_t k = 0; k < g->insts.len && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&g->insts, k))->name, it->name) == 0;
        if (!dup) *(Type **)vecPush(&g->insts) = it;
    }
}

/**
 * @brief Scan the module's declarations and decide which optional runtime blocks have to be emitted.
 *
 * The trigger is a declaration rather than a call: an `extern!` or `@builtin` primitive appearing in
 * the module means the program uses the corresponding runtime. Pools have one more source: the
 * `makesPool` the checker computed, because the `extc_pool_*` declarations live in the library module
 * and the entry module's function table cannot see them. This scan must run before the preamble,
 * because `_GNU_SOURCE` is honored only before the first include.
 *
 * @param g  The code generation context; sets `needRawTerm`, `needPool`, `needCout`, `needMemFind`,
 *           `needPlateCopy`, `needPlateView`, `needTime`, `needHttpFile` and `needDns`.
 * @param m  The current module; the declarations in `funcs` and `structs` are the only sources.
 */
/** @brief Expression callback for the weak-reference scan: stops the walk once one is found. */
static bool weakUseInExpr(void *ctx, Expr *e);

/** @brief Statement callback for the weak-reference scan (declared here for the expression one). */
static bool weakUseInStmt(void *ctx, Stmt *s);

/** @brief Expression callback: sets `needWeak` and stops the walk at the first weak primitive. */
static bool weakUseInExpr(void *ctx, Expr *e) {
    CG *g = (CG *)ctx;
    if (!e) return true;
    /* The weak primitives take the primitive path of a generic call, which the checker recognizes
     * by name and never resolves to a declaration -- so unlike the plate and buffer triggers there
     * is no declaration to notice, and the bodies have to be walked. */
    if (e->kind == EX_GENCALL && e->u.gencall.name && isWeakPrimitiveName(e->u.gencall.name)) {
        g->needWeak = true;
        return false;
    }
    AstVisit v = { weakUseInExpr, weakUseInStmt, ctx };
    return astWalkExprChildren(e, &v);
}

/** @brief Statement callback: walks every child of the statement. */
static bool weakUseInStmt(void *ctx, Stmt *s) {
    AstVisit v = { weakUseInExpr, weakUseInStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

/** @brief Does the module take any weak reference? Sets `needWeak` on the first one found.
 *
 * Walks function bodies (generic templates included: the library's `weak::of` is one) and the
 * methods of every struct, because a `weak` handle can be taken inside a method and nowhere else.
 *
 * @param g  The code generation context; sets `needWeak`.
 * @param m  The module being generated.
 */
static void scanWeakUse(CG *g, Module *m) {
    AstVisit v = { weakUseInExpr, weakUseInStmt, g };
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (f && f->body) astWalkStmtChildren(f->body, &v);
    }
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (!sd) continue;
        for (size_t k = 0; k < sd->methods.len; k++) {
            FuncDef *mf = *(FuncDef **)vecAt(&sd->methods, k);
            if (mf && mf->body) astWalkStmtChildren(mf->body, &v);
        }
    }
}

static void scanUnitNeeds(CG *g, Module *m) {
    /* The reclamation switch: on by default, `EXTC_RECLAIM=0` turns it off, and a nonzero value forces it
     * **on** (skipping the allocation-point criterion below). The criterion itself is not here -- this
     * pass only sets the intent from the environment, and once `collectEmitted` has picked the functions
     * that really get emitted, `rcPlanDecide` narrows it to "is there a reclamation-governed
     * allocation point". */
    const char *rcEnv = getenv("EXTC_RECLAIM");
    g->rc.forced = rcEnv && *rcEnv && strcmp(rcEnv, "0") != 0;
    g->rc.reclaim = !(rcEnv && *rcEnv && strcmp(rcEnv, "0") == 0);
    /* `EXTC_RC_SHARDS=N`: the reclamation **shard count** (the pause knob), 1 by default. Only a value
     * above 1 cuts each logical region into N run-time regions; 1, unset, empty, non-numeric and
     * out-of-range all mean 1 (single-shard behavior). The ceiling is 64: beyond that the baseline of
     * at least one 1 MiB slab per shard outweighs the pause benefit. */
    g->rc.nShard = 1;
    /* The default is the **no-count** fast path; `EXTC_RC_NOCOUNT=0` restores counting. */
    g->rc.noCount = 1;
    {
        const char *sh = getenv("EXTC_RC_SHARDS");
        if (sh && *sh) {
            long v = strtol(sh, NULL, 10);
            if (v > 1) g->rc.nShard = (int)(v > 64 ? 64 : v);
        }
    }
    /* `EXTC_RC_LOCALSTORE`: a dedicated entry point for local and parameter slots. **On by default**;
     * `=0` is the kill switch, and with it off the corpus output is byte-identical (the two paths
     * produce the same text on every shape). It only affects the no-count tier (see the emission
     * point). */
    g->rcLocalStore = 1;
    {
        const char *ls = getenv("EXTC_RC_LOCALSTORE");
        if (ls && *ls && strcmp(ls, "0") == 0) g->rcLocalStore = 0;
    }
    /* `EXTC_TASK_ROOTS`: register the task id slots the collector may consult. **On by default**;
     * `=0` is the kill switch and restores the previous product byte for byte (no registration
     * line is emitted at all). See `cgTaskRootPush`. */
    g->taskRoots = 1;
    {
        const char *tr = getenv("EXTC_TASK_ROOTS");
        if (tr && *tr && strcmp(tr, "0") == 0) g->taskRoots = 0;
    }
    /* `EXTC_RC_NOCOUNT`: the holder-counting switch. **Default 1 = no counting** (reclaimed by sweeping
     * alone): on the same 31 shape ratchets, ASan corpora, gaps sets, pause probe and absolute peak of a
     * large-live-set churn, the no-count tier is no worse and uses strictly fewer instructions.
     * `=0` restores "count plus early reclamation" (the count-narrowing analysis is meaningful only in
     * that mode too). */
    {
        const char *nc = getenv("EXTC_RC_NOCOUNT");
        if (nc && *nc && strcmp(nc, "0") == 0) g->rc.noCount = 0;
    }
    /* `EXTC_RC_SWEEPFREE`: the **event tier** -- a program without cycles has nothing to sweep: every
     * unreachable object is freed on the spot when its last holder disappears (the count-reaches-zero
     * fast path). So counting is turned on and `shouldSweep` is forced to 0 (see
     * `EXTC_RC_SWEEP_FREE` in `src/back/reclaim.c`).
     *
     * It also serves as a **diagnostic switch** (off by default, byte-identical product); the acyclicity
     * decision itself belongs to the analysis side, and a wrong decision errs towards "should have
     * swept but did not => memory grows", not towards dangling -- so the criteria come first: the sweep
     * count must be 0, peak RSS must be bounded, and forcing the decision to true on cyclic shapes must
     * blow up. */
    /* Three states: unset = decided by analysis (`unitAllAcyclic`); `=1` = forced on (the criteria's red
     * proof uses it); `=0` = forced off (for comparison and A/B -- otherwise, once the analysis decides
     * it holds, there is no way to compare against the other behavior). */
    {
        const char *sf = getenv("EXTC_RC_SWEEPFREE");
        g->rc.sweepFree = 0;
        if (sf && *sf) g->rc.sweepFree = (strcmp(sf, "0") == 0) ? -1 : 1;
        if (g->rc.sweepFree == 1) g->rc.noCount = 0;
    }
    /* `EXTC_DBG_RCCOST=1`: count rc sites per block level (diagnostic, byte-identical product). */
    if (getenv("EXTC_DBG_RCCOST")) g->dbgCost = 1;
    /* Does the program use the raw-terminal primitives? They arrive as an `extern!` the
     * library declares (`extc_raw_enter` in `std::sys::term`), which is the one thing
     * codegen can see from here. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->isExtern || !f->name) continue;
        if (strcmp(f->name, "extc_raw_enter") == 0) g->needRawTerm = true;
        /* The pool registry, on the same footing: the library declares these through
         * `extern!`, so their presence is what says the program uses pools at all. */
        if (strcmp(f->name, "extc_cout_put") == 0)  g->needCout    = true;
        /* The byte-search runtime, on the same footing: `std::sys::mem` is the only module
         * that declares these, and its declarations are what says the program wants `memmem`
         * instead of a byte loop. The flag is read again before the includes (the `_GNU_SOURCE`
         * block below), so this scan has to run before the preamble -- it does. */
        if (f->externLib && strcmp(f->externLib, "extc-mem") == 0)  g->needMemFind = true;
        /* The plate runtime is shaped like memfind: the trigger is a **declaration** (only the privileged
     * layer std::sys::heap declares it). */
        if (f->externLib && strcmp(f->externLib, "extc-heap") == 0) {
            if (plateIsMemCopyName(f->name)) g->needPlateCopy = true;
            else                             g->needPlateView = true;
        }
        /* The buffer substrate, same trigger shape: only the privileged layer declares
         * `extern!("extc-buf")`, so a program with no buffer never carries the block -- and
         * never carries the reclamation runtime either (a buffer's storage is foreign memory,
         * not an rc slot, so T0 stays T0). */
        if (f->externLib && strcmp(f->externLib, "extc-buf") == 0)  g->needBuf = true;
        if (f->externLib && strcmp(f->externLib, "extc-time") == 0)  g->needTime = true;
        /* The five file-level shims: they share the `extc-runtime` library tag with net => they can only be
         * recognized **by name**. This scan must run before the preamble (`_GNU_SOURCE` is honored only
         * before the first include), so the flags are set here rather than in `genCoroDecls`. */
        if (f->name && (strncmp(f->name, "extc_file_", 10) == 0 ||
                        strncmp(f->name, "extc_sendfile", 13) == 0 ||
                        strncmp(f->name, "extc_http_", 10) == 0))  g->needHttpFile = true;
        if (f->name && strcmp(f->name, "extc_dns_lookup") == 0)  g->needDns = true;
    }
    /* The JSON half of the `any` runtime is the one place the generated C calls **extC** code: the
     * descriptor walk hands each integer to `json$fmtI64`/`json$fmtU64`, which are ordinary digit
     * arithmetic in the `std::json` module (see `extc_json_i64`). A definition has no call site to
     * set its `planUsed` bit, so the scan sets it here -- before the prototypes and the bodies are
     * emitted -- and the same match is what tells the runtime the half is reachable: that module is the
     * only caller of `extc_any_json`, so no formatter means no caller means no JSON half. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || !f->body || !f->name) continue;
        if (strcmp(f->name, "json$fmtI64") == 0 || strcmp(f->name, "json$fmtU64") == 0) {
            planSetUsed(f, true);
            g->needAnyJson = true;
        }
    }
    /* Pools have one more source: **`makesPool`, which the checker has already computed**. Looking only
     * at "this module declares an extern of `extc_pool_*`" is not enough -- those declarations live in
     * the **library module** while `m` is the entry module, so at main's prologue `needPool` would still
     * be false => the frame's zone (`__extc_zm1`) is not emitted => raising to level 1 cannot be
     * expressed (`zoneArgRef` silently falls back to the dynamic fallback). `makesPool` is a transitive
     * least fixed point: any function (methods included) being true means the program uses pools --
     * which is exactly the question this cell has to answer. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* Only a function that is actually emitted can put a pool in the program: the prelude
         * declares `varArray`, whose methods make pools, in every program; counting those would
         * charge the pool runtime to programs that never touch it. */
        /* The same holds for the one `@builtin` primitive: it is declared in the privileged layer
         * (`std::sys::heap`) and the compiler supplies the body. It is not `extern!`, so the
         * `externLib`-based scan above cannot see it. */
        if (f && f->isBuiltin && plateIsViewOfName(f->name)) g->needPlateView = true;
    }
    /* `makesPool` over the functions the program actually reaches is the honest signal: the
     * prelude declares `varArray`, whose methods make pools, in every program, so their mere
     * presence must not pull the runtime in. */
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* `main`/`@export`/a module entry are roots the pruning keeps without a `planUsed` bit. */
        if (f && !f->isExtern && (planUsed(f) || cgIsMain(f) || f->isExport || f->isModuleEntry) &&
            planMakesPool(f))
            g->needPool = true;
    }
    /* Weak references: the runtime block and the tier decision both hang off this flag, and both are
     * emitted before the bodies, so the answer has to come from a scan, not from a call site. */
    scanWeakUse(g, m);
}

