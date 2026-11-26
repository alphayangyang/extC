/* ---- the implicit trailing arguments: **one owner** ----
 *
 * Two kinds exist -- the home arena (`usesHome`, `extc_arena *__extc_home`) and the zone
 * (`makesPool`, `int64_t __extc_home_zone`) -- and **both sides have to agree**: a call site must
 * pass what the definition's signature declares, or the generated C does not even compile
 * (`too few arguments to function ...`). Splitting that agreement across the call emitters, the
 * assoc path, the coroutine path and the signature emitter is what lets a definition with
 * `__extc_home` and a call site that passes nothing drift apart.
 *
 * Every site asks these two functions instead. The compiler's own speed is irrelevant at this
 * scale; the emitted C is what runs, and decoupling the convention from its consumers is what keeps
 * this file maintainable.
 *
 * `nargs` is how many **source** arguments were already emitted, so the separating comma lands in
 * the right place when there are none. `forSignature` picks the declaration spelling; otherwise
 * `site` supplies the arena level (`homeArg` explains why the level matters, and `zoneArgRef` the
 * zone's). Returns nothing: `cgHasImplicitArgs` is the predicate the `@overwrite` cell pass needs.
 *
 */
/**
 * @brief Report whether a function takes the implicit trailing arguments.
 *
 * @param callee  The function definition; NULL takes none.
 * @return true when `cgImplicitArgs` would append a home arena or a zone argument.
 */
static bool cgHasImplicitArgs(const FuncDef *callee) {
    return callee && (funcTakesHomeArena(callee) || funcTakesHomeZone(callee));
}

/**
 * @brief Generate the C text of the "ownership delegation" argument: does the owner of this home
 *        arena have a live reference slot right now?
 *
 * The home arena is the same one handed down from the caller, and a kernel frame's static root table
 * (`rcFrameOwnsArena` false) cannot see the live references of the calling frame, so a kernel frame
 * does not sweep and only records what it owes (`rcEmitOwe`). But when the **owner itself** has just
 * handed the arena out and holds no live reference slot at this moment, the callee's root table
 * (parameters + locals + globals) is a complete seed: an object live in that region can only be
 * reachable from the callee's parameters, so the right to sweep can be delegated to the callee on
 * the spot instead of owing the collection until the owner's next allocation. Ownership of `parTls`
 * is decided by the runtime depth of `RcOwn` and does not take this route.
 *
 * Only one call-site shape allows delegation: **the call site is the top-level value expression of a
 * whole statement** (`g->topExpr`). A subexpression may still hold a reference into this region that
 * was created earlier in the same expression and lives only in a register
 * (`f(mk(1).v, deep(x))`); the callee sweeping with its own root table would then release a live
 * object as garbage.
 *
 * @param g     The generator.
 * @param site  The call-site expression (`NULL` => conservative).
 * @param hlvl  Which arena level this call hands out (`ARENA_HOME`, or 1 and above).
 * @return      `0` (do not delegate) / `1` (this frame is the owner) / `__extc_homeOwn` (pass on the
 *              value that was received).
 */
static const char *homeOwnArg(CG *g, const Expr *site, int hlvl) {
    if (!g->rc.reclaim || g->parTls || g->coroFunc) return "0";
    if (g->rc.rootsUnreliable) return "0";
    if (g->rcGlobalRoots.len + g->rcRoots.len > 0) return "0";
    if (!site || site != g->topExpr) return "0";
    if (g->inMain) return "1";                      /* main's arena belongs to main itself */
    if (hlvl == ARENA_HOME && g->rcFuncUsesHome) return "__extc_homeOwn";
    return "0";
}
/**
 * @brief Append the implicit trailing arguments, home arena and home zone, according to the callee's
 *        declaration.
 *
 * Both sides must use the same test: the declaration side (`forSignature`) writes the parameter
 * declarations and the call side writes the argument expressions, both appending exactly what
 * `cgHasImplicitArgs` decides; otherwise the generated C has the wrong argument count and does not
 * compile.
 *
 * @param g             The generator.
 * @param b             The call/declaration text being assembled.
 * @param callee        The callee; nothing is appended when it is NULL.
 * @param site          The call-site expression, which decides the arena level and the zone
 *                      argument; may be NULL on the declaration side.
 * @param nargs         The number of **source** arguments already emitted, which decides the
 *                      separating comma.
 * @param forSignature  true writes the parameter spellings (`extc_arena *__extc_home` /
 *                      `int64_t __extc_home_zone`), false writes call arguments.
 */
static void cgImplicitArgs(CG *g, Buf *b, const FuncDef *callee, const Expr *site,
                           size_t nargs, bool forSignature) {
    if (!callee) return;
    size_t n = nargs;
    if (funcTakesHomeArena(callee)) {
        int hlvl = site ? planArenaArg(site) : ARENA_HOME;
        if (n) bufPuts(b, ", ");
        if (forSignature) {
            bufPuts(b, "extc_arena *__extc_home");
        } else {
            const char *harg = homeArg(g, hlvl);
            bufPuts(b, harg);
            /* The owed-collection point of subset B: when an arena **this frame owns** is passed out as the
             * callee's home arena, the callee can only record "one collection owed" on that region
             * (its static root table is incomplete for it). This is the moment, "the previous batch
             * of callees has returned and the next has not been entered", to settle that debt: no
             * callee subtree of this frame is active, so this frame's static root table is a complete
             * seed (see the invariant of `rcEmitOwedCheck`). `main`'s home arena is
             * `&__extc_a[1]` (subset A), so `main` is in this state too. */
            RcOwn own = rcOwnKind(g, hlvl);
            if (own != RC_OWN_NO)   /* the TLS state is emitted; ownership is decided at run time */
                rcEmitOwedCheck(g, arenaPrintf(g->arena, "(void *)(%s)", harg), hlvl, own);
        }
        n++;
        /* The ownership-delegation flag: it follows the home arena argument and both sides must emit it
         * (see `homeOwnArg`). The callee uses it to decide whether its own static root table
         * qualifies to sweep on the spot, so it exists only in a translation unit with
         * `rc.reclaim`. */
        if (g->rc.reclaim) {
            if (n) bufPuts(b, ", ");
            bufPuts(b, forSignature ? "EXTC_UNUSED int32_t __extc_homeOwn"
                                    : homeOwnArg(g, site, hlvl));
            n++;
        }
    }
    if (funcTakesHomeZone(callee)) {
        if (n) bufPuts(b, ", ");
        bufPuts(b, forSignature ? "int64_t __extc_home_zone" : zoneArgRef(g, (Expr *)site));
        n++;
    }
}
static bool f_owLocal(CG *g, Stmt *s);
static const char *zoneArgRef(CG *g, Expr *e);

/**
 * @brief Shape the first operand of a call or operator the way the target's `self` parameter
 *        requires.
 *
 * `a.f(x)` is `f(a, x)` and `a << b` is `(<<)(a, b)`: both pass the object as the first
 * argument, as an address when the parameter is a reference and as a value when it is a value.
 *
 * A temporary has no address (`&(f())` is not legal C), so it is materialized first into a
 * one-element array compound literal: `(T[]){ f() }` has type `T *` (the array decays). That is
 * exactly what makes chained calls work: in `cout << a << b` the second `<<` receives the value
 * returned by the first.
 *
 * **Do not write `&((T){ f() })`**: `(T){ x }` is not a copy in C, it initializes the first
 * member with x, which yields a confusing error such as `_Bool has = <option_i64>`; an array
 * initializer is applied per element, so `{ f() }` really is "initialize element 0 with T".
 *
 * An operand that is a place must keep its address: taking the address of a copy turns a write
 * parameter into a write to a temporary.
 *
 * @param g        The generator.
 * @param operand  The expression passed out as the first argument.
 * @param code     Its generated C text.
 * @param p0       The parameter that receives it.
 * @param want     The operand's static type, used for the C type when a temporary is
 *                 materialized.
 * @return         The C text to pass out.
 */
static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want) {
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = want && want->kind == TY_REF;
    if (wantRef && !haveRef) {
        /* The question is whether the **generated C** is an lvalue, not whether the operand is a
         * place in extC: a slice expression is a place in extC while its C form is a call. That is
         * `printArgIsPlace`'s question, so `isPlaceExpr` must not be asked here -- it would emit
         * `&(slice_i32_slice(...))`, which C rejects. */
        if (printArgIsPlace(operand)) return arenaPrintf(g->arena, "&(%s)", code);
        return arenaPrintf(g->arena, "(%s[]){ %s }", cType(g, subst(g, want)), code);
    }
    if (!wantRef && haveRef) return arenaPrintf(g->arena, "*(%s)", code);
    return code;
}

/**
 * @brief Generate `EX_DYN`: turn `dyn Trait(x)` into a value -- copy the payload into the dyn
 *        pool and hand out the handle.
 *
 * The payload goes through a temporary first, so both rvalues and lvalues work; `pfLine` is the
 * mechanism that puts a statement into the current prefix. The payload's C type comes from the
 * checker's `payloadType`: the node's own type is `dyn Trait` by then, while the temporary must
 * have the payload's type.
 *
 * The fourth argument is the zone the pool is registered in: a `dyn` payload pool lives at a
 * **place**, and the place is fixed by the checker's `zoneLevel` (promoted to a longer-lived
 * level when it escapes). The runtime cannot substitute "the current top of stack": after a
 * promotion the construction point is in an inner block while the pool has to live in the outer
 * one, and those are two different things.
 *
 * @param g  The generator.
 * @param e  The `EX_DYN` node.
 * @return   The call expression of `extc_dyn_put` (the handle in the pool).
 */
static const char *genDynValue(CG *g, Expr *e) {
    const char *payC = genExpr(g, e->u.dynv.payload);
    Type *pt = subst(g, e->u.dynv.payloadType);
    const char *ct = cType(g, ttBase(pt));
    const char *tmp = arenaPrintf(g->arena, "__extc_dyp%d", g->tmpSeq++);
    const char *zi  = zoneArgRef(g, e);
    pfLine(g, "%s %s = %s;", ct, tmp, payC);
    return arenaPrintf(g->arena,
                       "extc_dyn_put((const void *)&%s, (int64_t)sizeof(%s), &extc_vt$%s$%s, %s)",
                       tmp, ct, e->u.dynv.traitName, ct, zi);
}

/**
 * @brief Emit the format specifier of an f-string slot: `padText` / `padInt` / `padF` according
 *        to the padding primitive the checker recorded.
 *
 * Arguments are matched by **parameter name** (`padF` has `prec` before `fill` / `align`, so
 * passing by position would misplace them). Width, fill, align and precision are literals by the
 * time this runs; when align is absent (-1) the default follows the value's kind: text to the
 * left, numbers to the right. The type code only exists for the checker's consistency check and
 * is not emitted.
 *
 * @param g  The generator.
 * @param e  The reserved-name EX_METHOD; arguments are the slot value, width, fill byte, align,
 *           precision and type code, in that order.
 * @return   A C call expression; `"0"` when nothing is recorded on the plan side or the method
 *           does not match.
 */
static const char *genPadCall(CG *g, Expr *e) {
    static const char *const padNames[] = {"", "padText", "padInt", "padF", "padUInt"};
    PlanPadKind kind = planPadKind(e);
    if (kind <= PLAN_PAD_NONE || kind > PLAN_PAD_UINT) return "0";
    Type *recvT = subst(g, e->u.method.recv->type);
    FuncDef *f = findMethod(ttBase(recvT), padNames[kind]);
    if (!f || f->params.len < 2) return "0";
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);
    recvC = selfOperandAsParam(g, e->u.method.recv, recvC, p0, recvT);
    const char *fname = cMethodName(g, recvT, f);
    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 1; i < f->params.len; i++) {
        Param *pm = *(Param **)vecAt(&f->params, i);
        int slot = -1;
        if (pm->name && strcmp(pm->name, "v") == 0) slot = 0;
        else if (pm->name && strcmp(pm->name, "width") == 0) slot = 1;
        else if (pm->name && strcmp(pm->name, "fill") == 0) slot = 2;
        else if (pm->name && strcmp(pm->name, "align") == 0) slot = 3;
        else if (pm->name && strcmp(pm->name, "prec") == 0) slot = 4;
        if (slot < 0 || slot >= (int)e->u.method.args.len) return "0";
        Expr *ae = *(Expr **)vecAt(&e->u.method.args, slot);
        if (slot == 3 && ae->kind == EX_INT && ae->u.ival < 0) {
            /* align absent: text to the left, numbers to the right */
            bufPrintf(&b, ", %s", kind == PLAN_PAD_TEXT ? "0" : "1");
            continue;
        }
        const char *at = genExpr(g, ae);
        if (slot == 0 && kind == PLAN_PAD_INT) bufPrintf(&b, ", (int64_t)(%s)", at);
        else if (slot == 0 && kind == PLAN_PAD_UINT) bufPrintf(&b, ", (uint64_t)(%s)", at);
        else if (slot == 0 && kind == PLAN_PAD_FLOAT) bufPrintf(&b, ", (double)(%s)", at);
        else bufPrintf(&b, ", %s", at);
    }
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Emit a call to a dynamically dispatched method: `v.m()` where `v` is an `any`.
 *
 * Nothing here knows which method runs: the helper named by this site switches on the receiver's
 * descriptor and calls that type's method directly (no table of function pointers). The result is
 * boxed into an `any`, because the checker typed the call as `any` -- the caller unboxes it, and a
 * wrong belief traps there.
 *
 * The site is appended to `g->dynCalls` and its dispatcher is generated by `emitDynCalls` once the
 * `any` runtime exists in the unit. One dispatcher per emission: a generic body containing such a
 * call is emitted once per instance, and each instance has its own candidate list.
 *
 * @param g  The generator.
 * @param e  The `EX_METHOD` call; its receiver is an `any`.
 * @return   The C expression that yields the boxed result.
 */
static const char *genDynCall(CG *g, Expr *e) {
    const char *recvC = genExpr(g, e->u.method.recv);
    const char *rtmp = arenaPrintf(g->arena, "__extc_dr%zu", g->tmpSeq++);
    pfLine(g, "ExtcAny %s = %s;", rtmp, recvC);
    size_t idx = g->dynCalls.len;
    *(Expr **)vecPush(&g->dynCalls) = e;
    g->needAny = true;
    return arenaPrintf(g->arena, "extc_dyn_%zu(%s, &%s)", idx, homeArg(g, ARENA_HOME), rtmp);
}

/* Defined below, next to the struct-literal path that also uses it: a by-value copy of an
 * `@value` place is a **built** copy, never a byte copy. */
static const char *cloneValueText(CG *g, Type *dst, Expr *site);
/* Defined in the drop section (120-question-expansion.inc), which comes after this file: the
 * argument loops here ask both whether a parameter owns storage and whether the argument is a
 * move. */
static bool typeNeedsDrop(CG *g, Type *t, int depth);
static void noteArgMove(CG *g, Expr *a, Type *pt);
static bool argCopyNeedsClone(CG *g, Expr *a, Type *pt);
static Type *methodParamType(CG *g, Type *recvT, FuncDef *f, size_t i);

/**
 * @brief Generate a method call as an ordinary function call on the receiver.
 *
 * `a.f(x)` is `f(a, x)`: the receiver is shaped to what the first parameter requires (an
 * address for a reference, the value itself for a value), the remaining arguments are expanded
 * as they are, and the callee's implicit trailing arguments and `@overwrite` unit are appended
 * last.
 *
 * Inside a generic body the written receiver type may still contain `T`, and a call node the
 * checker deferred carries no `func` at all: the method is resolved here against the
 * **substituted** type. One template body has many instances, so a `func` recorded on the node
 * would be the wrong method for the others.
 *
 * A coroutine handle receiver (`coroutine<T>`) goes through the `next`/`value`/`send` handle
 * protocol; a callee that belongs to a coroutine goes through the driver protocol; a `dyn Trait`
 * receiver takes the function pointer from the trait's uniform table.
 *
 * @param g  The generator.
 * @param e  The `EX_METHOD` node.
 * @return   The C call expression; `"0"` when the checker resolved no method or the method has
 *           no parameters.
 */
static const char *genMethodCall(CG *g, Expr *e) {
    /* The receiver's type as this instance sees it. Inside a generic body the written type may
     * still mention `T`, and for a deferred call (a method on a type parameter) the node carries
     * no `func` at all: the method is resolved here, on the substituted type, the same way the
     * operators are resolved. The checker deliberately leaves `planCallee(e)` alone -- one
     * template body has many instances, so a `func` stored there would be the wrong method for
     * all but the last one. */
    Type *recvT = subst(g, e->u.method.recv->type);
    /* An f-string slot's format specifier is a reserved-name call the parser desugars: the padding
     * primitive is recorded on the plan side and emitted from that choice directly, not through
     * ordinary method resolution. */
    if (e->u.method.name && strcmp(e->u.method.name, "__extc_pad") == 0)
        return genPadCall(g, e);
    /* `v.m()` where `v` is an `any`: the descriptor picks the method at run time. */
    if (planDynCall(e)) return genDynCall(g, e);
    /* `f.call(x)` where `f` holds a `fn(...)` pointer: a deferred call whose instance turned out to
     * be a bare code pointer. There is no method to name -- C calls through the pointer. The
     * parentheses keep a compound receiver from binding to the argument list. */
    if (e->u.method.name && strcmp(e->u.method.name, "call") == 0 && ttBase(recvT) &&
        ttBase(recvT)->kind == TY_FN) {
        Buf fb;
        bufInit(&fb, g->arena);
        bufPrintf(&fb, "(%s)(", genExpr(g, e->u.method.recv));
        for (size_t i = 0; i < e->u.method.args.len; i++)
            bufPrintf(&fb, "%s%s", i ? ", " : "",
                      genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
        bufPuts(&fb, ")");
        return bufCstr(&fb);
    }
    FuncDef *f = planCallee(e);
    if (!f) f = findMethod(ttBase(recvT), e->u.method.name);
    /* The **handle** protocol: the receiver is a `coroutine<T>` (the marker type, whatever the
     * resolved method's owner looks like -- instantiated copies carry a decorated name) and the
     * emitted call dispatches on `kind`. The frame protocol below is unaffected: a frame's type is
     * not a coroutine marker. This test comes before the other early returns so nothing else can
     * claim the call. */
    if (e->u.method.name && f &&
        (strcmp(e->u.method.name, "next") == 0 || strcmp(e->u.method.name, "value") == 0 ||
         strcmp(e->u.method.name, "send") == 0) &&
        isProtoType(ttBase(recvT), "coroutine", 1)) {
        const char *rr = genExpr(g, e->u.method.recv);
        const bool viaRef = e->u.method.recv->type && e->u.method.recv->type->kind == TY_REF;
        g->needCoroHandle = true;
        if (strcmp(e->u.method.name, "next") == 0)
            return arenaPrintf(g->arena, "extc_coro_next(%s%s, \"%s\", %d)",
                               viaRef ? "" : "&", rr, g->path, e->line);
        if (strcmp(e->u.method.name, "send") == 0) {
            Type *rb2 = ttBase(recvT);
            Type *yt2 = (rb2 && rb2->targs.len) ? subst(g, *(Type **)vecAt(&rb2->targs, 0)) : f->ret;
            const char *vv = e->u.method.args.len
                                 ? genExpr(g, *(Expr **)vecAt(&e->u.method.args, 0)) : "0";
            return arenaPrintf(g->arena, "extc_coro_send_%s(%s%s, %s, \"%s\", %d)",
                               cType(g, yt2), viaRef ? "" : "&", rr, vv, g->path, e->line);
        }
        /* `T` comes from the receiver's own type: the resolved instance method's `ret` may still be
         * the error type, and the helper is named after the coroutine's yield type. */
        Type *rb = ttBase(recvT);
        Type *yt = (rb && rb->targs.len) ? subst(g, *(Type **)vecAt(&rb->targs, 0)) : f->ret;
        return arenaPrintf(g->arena, "extc_coro_value_%s(%s%s, \"%s\", %d)",
                           cType(g, yt), viaRef ? "" : "&", rr, g->path, e->line);
    }
    if (f && planCoroProto(f) && f->owner && planCoroOf(f->owner)) {
        /* Driving a coroutine: the two protocol methods are the state machine's calling
         * convention, emitted inline. Whether the receiver is the frame itself or a reference to
         * it decides `&x`/`x.ret` versus `x`/`x->ret`, which is what lets a coroutine be driven
         * through a `mut ref` parameter. */
        Type *rt0 = ttBase(subst(g, e->u.method.recv->type));
        bool viaRef = rt0 && rt0->kind == TY_REF;
        const char *rc = genExpr(g, e->u.method.recv);
        if (planCoroProto(f) == 5) {
            /* `send(v)`: put the value in the frame's `in` slot, then resume exactly like `next`. */
            const char *vv = e->u.method.args.len
                                 ? genExpr(g, *(Expr **)vecAt(&e->u.method.args, 0)) : "0";
            pfLine(g, "%s%s in = %s;", rc, viaRef ? "->" : ".", vv);
        }
        if (planCoroProto(f) == 1 || planCoroProto(f) == 5) {
            /* With a task place the step is driven through the driver (it owns the place); without
             * one there is nothing to set up, so the step itself is the driver. */
            FuncDef *cf = planCoroOf(f->owner);
            return planCoroNeedsZone(cf)
                       ? arenaPrintf(g->arena, "%s$next(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc)
                       : arenaPrintf(g->arena, "%s$step(%s%s)", cFuncName(g, cf), viaRef ? "" : "&", rc);
        }
        return arenaPrintf(g->arena, "%s%sret", rc, viaRef ? "->" : ".");
    }
    if (!f) return "0";

    /* The checker refuses a receiver-less method through `dyn`, so this only keeps the generator
     * from indexing an empty parameter list. */
    if (f->params.len == 0) return "0";
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* A stored `dyn` value is a handle, not a payload: it is not re-materialised, and its callee
     * is named by the trait's uniform table instead of a per-type C name. Which source it is is
     * decided by the receiver's **type**, not its node kind: the parser rewrites the immediate
     * form's receiver to the payload, so both forms arrive as ordinary expressions and only the
     * type tells them apart (`dyn Tag` is the stored value). */
    bool dynStored = e->dynTrait && ttBase(recvT) && ttBase(recvT)->kind == TY_DYN;
    if (!dynStored)
        recvC = selfOperandAsParam(g, e->u.method.recv, recvC, p0, recvT);

    const char *fname = dynStored ? NULL : cMethodName(g, recvT, f);
    /* `dyn Trait(x).m(...)`: the callee comes from the trait's uniform table instead of a per-type
     * C name. The field **is** the implementation, so the call keeps the same signature --
     * including hidden parameters, which the table's field types picked up when it was emitted.
     * Same receiver, same arguments, same extra parameters, a different callee expression.
     *
     * Two sources: `dyn Trait(x).m(...)` copies the payload into the pool here; `d.m(...)` on a
     * stored value **is** a handle already, so only the checked slot lookup is emitted. Either way
     * the table and the receiver both come from the **checked slot**. */
    if (e->dynTrait) {
        Buf dynSym;
        bufInit(&dynSym, g->arena);
        if (dynStored) {
            /* The receiver may be reached **through a reference** (`fn f(r: ref dyn Tag)`), in
             * which case the C expression is a pointer to the handle while `extc_dyn_slot` takes
             * the handle **by value**: the pointer has to be dereferenced here. */
            const char *handleC = recvC;
            if (planDynRecvViaRef(e))
                handleC = arenaPrintf(g->arena, "(*%s)", recvC);
            const char *dynS = arenaPrintf(g->arena, "__extc_ds%d", g->tmpSeq++);
            pfLine(g, "ExtcDynSlot *%s = extc_dyn_slot(%s, \"%s\", %d);",
                   dynS, handleC, g->path, e->line);
            bufPrintf(&dynSym, "((const struct extc_vt$%s_t *)%s->vt)->%s",
                      e->dynTrait, dynS, e->u.method.name);
            recvC = arenaPrintf(g->arena, "%s->addr", dynS);
        } else {
            const char *dynT = cType(g, ttBase(recvT));
            bufPrintf(&dynSym, "((const struct extc_vt$%s_t *)&extc_vt$%s$%s)->%s",
                      e->dynTrait, e->dynTrait, dynT, e->u.method.name);
            /* The payload is copied into a stack temporary before the call even though nothing
             * owns it: a method that mutates its receiver (`mut ref Self`) would otherwise write
             * through to the **source object**, while the stored form mutates the pool's copy, so
             * the same expression `dyn Tag(b)` would mean two different things. `recvC` is already
             * a pointer to the payload, so this is one copy of `sizeof(T)`. */
            const char *slot = arenaPrintf(g->arena, "__extc_dynp%d", g->tmpSeq++);
            pfLine(g, "%s %s = *(%s);", dynT, slot, recvC);
            recvC = arenaPrintf(g->arena, "(void *)&%s", slot);
        }
        fname = bufCstr(&dynSym);
    }

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++) {
        /* The written argument that lands in parameter `i + 1` (0 is the receiver): a by-value
         * `@value` parameter is built here for the same reason an ordinary argument is. */
        Expr *a = *(Expr **)vecAt(&e->u.method.args, i);
        const char *av = NULL;
        if (i + 1 < f->params.len) {
            Type *pt = methodParamType(g, recvT, f, i + 1);
            if (argCopyNeedsClone(g, a, pt)) av = cloneValueText(g, pt, a);
            else                             noteArgMove(g, a, pt);
        }
        bufPrintf(&b, ", %s", av ? av : genExpr(g, a));
    }
    /* The receiver counts as the shallowest mutable reference argument, so it is the first
     * argument for both the implicit tail parameters and the `@overwrite` cells. */
    cgImplicitArgs(g, &b, f, e, e->u.method.args.len + 1, false);
    owPassCells(g, &b, e, e->u.method.args.len + 1, cgHasImplicitArgs(f));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Emit the `clone` call that builds a by-value copy of an `@value` place.
 *
 * An `@value` type owns its storage, so every by-value copy has to be built instead of copied
 * byte for byte: the checker marks the three copy points besides a declaration (a by-value
 * argument, a struct-literal field initializer and a `return`) and this turns the mark into
 * `Type_clone(&(place))`.
 *
 * The hook takes only `self`, but any call to a method that returns an aggregate still passes the
 * implicit arguments the declaration asks for, so they are appended here through the same helper
 * every other call site uses.
 *
 * @param g     The generator.
 * @param dst   The type of the place the copy is written into.
 * @param site  The place being copied; a temporary is moved and never reaches here.
 * @return      C text of the clone call, or `NULL` when the hook cannot be named.
 */
/**
 * @brief Note that a by-value argument is **moved** into the callee's parameter.
 *
 * A parameter of a type that owns storage is the callee's copy only in the sense that the bytes
 * are copied: ownership moves with them, and the callee runs the hook on the way out
 * (`genParamDrops`). The caller's name therefore owns nothing after the call, and its own block
 * exit must not run the hook a second time. A place the checker marked for cloning is not a move:
 * the clone is a new value and the source keeps its own.
 *
 * @param g   The generator.
 * @param a   The written argument.
 * @param pt  The parameter's declared type (a type parameter is substituted here).
 */
/** * @brief The declared type of parameter `i` as **this call site** sees it. * * Inside a generic body the declaration still reads `T`, and a call on a concrete receiver * (`outer.push(inner)` on a `buf<vector<i64>>`) calls an instance whose parameter types were * substituted at instantiation. Whether an argument is a move depends on that substituted type: * `T` itself owns nothing, `vector<i64>` owns a reservation. The receiver carries the arguments, * so the substitution is taken from it; otherwise the ambient context applies. * * @param g      The generator. * @param recvT  The receiver type as written at this site (already substituted). * @param f      The method definition. * @param i      Parameter index. * @return       The parameter type to reason about. */static Type *methodParamType(CG *g, Type *recvT, FuncDef *f, size_t i) {
    if (i >= f->params.len) return NULL;
    Type *pt = (*(Param **)vecAt(&f->params, i))->type;
    Type *bt = ttBase(recvT);
    if (bt && bt->kind == TY_GENERIC && bt->sdef && bt->targs.len && bt->sdef->typeParams.len)
        return ttSubstitute(g->tt, pt, &bt->sdef->typeParams, &bt->targs);
    return subst(g, pt);
}
/**
 * @brief Tell whether an argument names a place whose bytes the caller keeps.
 *
 * A binding, a field, an element and `*p` (the pointee of a reference) all have storage of their
 * own, so a by-value copy of one has to be built. A temporary has nothing to copy from.
 *
 * @param a  The written argument.
 * @return   True for a place; false for anything else.
 */
static bool argSiteIsPlace(Expr *a) {
    if (!a) return false;
    if (a->kind == EX_IDENT || a->kind == EX_INDEX || a->kind == EX_FIELD) return true;
    return a->kind == EX_DEREF && a->u.deref.operand && a->u.deref.operand->type &&
           a->u.deref.operand->type->kind == TY_REF;
}

/**
 * @brief Does this by-value argument have to be built as a clone?
 *
 * The checker decides this for a concrete call site and records it with `planArgClone`. Inside a
 * template body the parameter type is still `T`, so it can only record the question and replay it
 * per instance (`runRefCheck`); the decision at the emitted call is therefore made here with the
 * substituted type. The rule is the checker's: the callee drops its by-value parameter, so passing
 * a place hands over storage; a temporary is moved instead and needs nothing.
 *
 * @param g   The generator, whose substitution context answers for `T`.
 * @param a   The written argument.
 * @param pt  The parameter's declared type.
 * @return    True when the argument must be cloned.
 */
static bool argCopyNeedsClone(CG *g, Expr *a, Type *pt) {
    if (!a) return false;
    if (planArgClone(a)) return true;
    if (!pt) return false;
    Type *t = subst(g, pt);
    if (!t || t->kind == TY_REF) return false;
    if (!typeNeedsDrop(g, t, 0)) return false;
    StructDef *sd = structOf(ttBase(t));
    if (!sd || !sd->valueCopy || !sd->cloneHook || sd->cloneHook->params.len != 1) return false;
    return argSiteIsPlace(a);
}

static void noteArgMove(CG *g, Expr *a, Type *pt) {
    if (!a || !pt || argCopyNeedsClone(g, a, pt)) return;
    /* Inside a generic body the parameter still reads `T`: instantiate it before asking whether
     * the callee owns its parameter, or a `vector<i64>` element would look like a type with no
     * storage. */
    if (!typeNeedsDrop(g, subst(g, pt), 0)) return;
    const char *nm = plainIdentCName(g, a);
    if (nm) noteMoved(g, nm);
}

static const char *cloneValueText(CG *g, Type *dst, Expr *site) {
    Type *t = subst(g, dst);
    StructDef *sd = structOf(ttBase(t));
    if (!sd || !sd->cloneHook) return NULL;
    const char *fn = cMethodName(g, t, sd->cloneHook);
    const char *src = genExpr(g, site);
    if (!fn || !src) return NULL;
    Buf cl;
    bufInit(&cl, g->arena);
    bufPrintf(&cl, "%s(&(%s)", fn, src);
    cgImplicitArgs(g, &cl, sd->cloneHook, site, 1, false);
    bufPuts(&cl, ")");
    return bufCstr(&cl);
}

/**
 * @brief Find the initializer expression written for a field in a struct literal.
 *
 * @param lit    The struct literal node.
 * @param fname  The field name, compared verbatim.
 * @return       The field's initializer; `NULL` when the literal omits the field, in which case
 *               the caller uses the field's zero value.
 */
static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

/**
 * @brief Generate the C text of a struct literal: a compound literal or a brace initializer
 *        list.
 *
 * Every field is written out and an omitted one becomes its zero value: leaving the omission to
 * C's implicit zeroing would turn an omitted `str` field into NULL.
 *
 * The type is substituted as a whole first: a literal inside a generic instance still records
 * the template type (`result<T,E>`) with parameters as its type arguments, and using it
 * directly makes the zero-value path see a bare `T`, degrade to `0` and write `.value = 0`.
 * Field types are substituted with the instance's own type arguments for the same reason.
 *
 * @param g           The generator.
 * @param e           The `EX_STRUCTLIT` node.
 * @param staticInit  true means this initializes a C static object (a global): `(T)` compound
 *                    literals cannot be used there because C requires a constant initializer and
 *                    a compound literal is not one; the brace form has the same content and
 *                    differs only in that prefix.
 * @return            C text allocated in the generator's arena; `(int){0}` when the type is not
 *                    a struct.
 */
static const char *genStructLitAs(CG *g, Expr *e, bool staticInit) {
    /* Substitute over the whole type first: inside a generic instance the literal still records
     * the template type (`result<T,E>`), whose type arguments are parameters. Taking that type as
     * it is sends the zero-value path a bare `T`, which degrades to `0` and ends up as
     * `.value = 0` in the generated C. */
    Type *t = subst(g, e->type);
    StructDef *sd = (t && (t->kind == TY_STRUCT || t->kind == TY_GENERIC)) ? t->sdef : NULL;
    if (!sd) return "(int){0}";

    const char *cname = cType(g, t);       /* an instance gets its decorated name */
    if (sd->fields.len == 0)
        return staticInit ? "{0}" : arenaPrintf(g->arena, "(%s){0}", cname);

    /* A literal of a generic instance substitutes its own type arguments into
     * the field types as well. */
    Vec *sp = g->substParams, *sa = g->substArgs;
    const char *sn = g->ownerPrefix;
    if (t->kind == TY_GENERIC) {
        g->substParams = &sd->typeParams;
        g->substArgs   = &t->targs;
    }

    Buf b;
    bufInit(&b, g->arena);
    if (staticInit) bufPuts(&b, "{");
    else            bufPrintf(&b, "(%s){", cname);
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        Type *ft = subst(g, fd->type);
        Expr *v = litValueFor(e, fd->name);
        if (i) bufPuts(&b, ", ");
        /* A field of an `@value` type owns its storage: copying a place into the literal would
         * leave two owners, so the checker marked the initializer and the copy is built here. */
        const char *vt = NULL;
        if (v && argCopyNeedsClone(g, v, ft)) vt = cloneValueText(g, ft, v);
        /* A field of an owning type takes the storage of a plain name over: the name is dead
         * after the literal, so its block exit must not run the hook a second time. */
        if (!vt) noteArgMove(g, v, ft);
        if (!vt) vt = v ? genExpr(g, v) : zeroValue(g, ft);
        bufPrintf(&b, ".%s = %s", fd->name, vt);
    }
    bufPuts(&b, "}");

    g->substParams = sp;
    g->substArgs   = sa;
    g->ownerPrefix = sn;
    return bufCstr(&b);
}

/**
 * @brief Generate a struct literal as a C compound literal expression.
 *
 * Shares `genStructLitAs` with global initialization and differs only in `staticInit` being
 * false: unwritten fields are filled with `zeroValue`.
 *
 * @param g  The generator.
 * @param e  The struct literal expression.
 * @return   C text of the form `(T){ .field = v, ... }`, allocated in the arena.
 */
static const char *genStructLit(CG *g, Expr *e) { return genStructLitAs(g, e, false); }

/**
 * @brief Generate the initializer of a global variable (a C static object).
 *
 * The initializer must be a constant C can fold at compile time. A struct literal is therefore
 * written as a brace initializer list (see `genStructLitAs`); every other expression reaching
 * here has already been restricted to constants by the checker.
 *
 * @param g  The generator.
 * @param e  The initializer expression.
 * @return   The C text of the initializer.
 */
static const char *genGlobalInit(CG *g, Expr *e) {
    return e->kind == EX_STRUCTLIT ? genStructLitAs(g, e, true) : genExpr(g, e);
}

static const char *arenaRefAt(CG *g, int level);
static const char *rcRegionRef(CG *g, const char *basePtr);
static void arenaDriftCheck(CG *g, Expr *e, const char *what);   /* the arena is picked per level */
/* Task-root registration (the task-table half of the emitter's collector contract); defined with
 * the other coroutine release helpers. */
static void cgTaskRootPush(CG *g, const char *slotExpr);
static void cgTaskRootPopAt(CG *g, int lvl, bool atBlockEnd, bool clear);
static void cgTaskRootPopsForReturn(CG *g);

/* Emit the C expression for one expression node.
 *
 * Every caller goes through genExpr, which wraps this function to add the
 * automatic dereference; this one only dispatches on the node kind.
 *
 * Returns:
 *   A C expression, or "0" for a node that must not reach a value position.
 */
/**
 * @brief Generate `EX_EXT`: hand a coroutine call to the scheduling domain as one task.
 *
 * The frame is built along the ordinary frame path as a local of this function -- the domain
 * drives it to completion before the enclosing block ends, so it lives exactly long enough --
 * and is then handed to the domain as `(frame, step)`. The value of the whole expression is
 * void.
 *
 * The `EX_EXT` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_EXT` node.
 * @return   Always `"(void)0"`; a compile error is reported when the preconditions do not hold.
 */
static const char *exExt(CG *g, Expr *e) {
    /* `ext f(x)`: start one task -- build the frame as a plain local (the domain drives it
     * before the enclosing block ends, so it stays alive exactly as long as it is needed), then
     * hand it to the domain as `(frame, step)`. The expression's value is void. */
    Expr *in = e->u.ext_.call;
    FuncDef *cf = (in && in->kind == EX_CALL) ? planCallee(in) : NULL;
    if (!cf || !planIsCoro(cf) || !planExtDom(e) || g->coroFunc) {
        ctxError(g->ctx, e->line, 1, NULL,
                 "domain task: only a plain-function context with a coroutine callee is"
                 " implemented so far (nesting a task inside a coroutine is the next step)");
        return "0";
    }
    const char *cn = cFuncName(g, cf);
    const char *tmp = arenaPrintf(g->arena, "__extc_dt%d", g->tmpSeq++);
    const char *sv = arenaPrintf(g->arena, "__extc_dtsv%d", g->tmpSeq++);
    const char *id = arenaPrintf(g->arena, "__extc_dtid%d", g->tmpSeq++);
    const char *zn = arenaPrintf(g->arena, "__extc_dtzn%d", g->tmpSeq++);
    Buf init;
    bufInit(&init, g->arena);
    if (planCoroNeedsZone(cf)) {
        flushPrefix(g);
        cgLine(g, "int64_t %s = extc_placeTop;", sv);
        cgLine(g, "int64_t %s = extc_task_begin();", id);
        cgLine(g, "int64_t %s = extc_task_zone(%s);", zn, id);
        /* The task must stay visible to the collector while the domain drives it; `exExt` never
         * ends it (the domain runtime only steps frames), so the block's release point pops the
         * registration after `extc_dom_run` (see ST_DOMAIN). */
        cgTaskRootPush(g, arenaPrintf(g->arena, "&%s", id));
        g->needCoroHandle = true;
        g->needPool = true;
    }
    bufPrintf(&init, "struct %s$frame %s = (struct %s$frame){ .pc = 0", cn, tmp, cn);
    if (planCoroNeedsZone(cf)) bufPrintf(&init, ", .zone = %s, .task = %s", zn, id);
    for (size_t i = 0; i < cf->params.len; i++) {
        Param *pp = *(Param **)vecAt(&cf->params, i);
        Expr *arg = (in->u.call.args.len > i) ? *(Expr **)vecAt(&in->u.call.args, i) : NULL;
        bufPrintf(&init, ", .%s = %s", pp->cname, arg ? genExpr(g, arg) : "0");
    }
    bufPrintf(&init, " };");
    flushPrefix(g);
    cgLine(g, "%s", bufCstr(&init));
    if (planCoroNeedsZone(cf)) cgLine(g, "extc_placeTop = %s;", sv);
    g->needDomain = true;
    flushPrefix(g);
    cgLine(g, "extc_dom_add(%s, (void *)&%s, %s$domstep);", genExpr(g, planExtDom(e)), tmp, cn);
    return "(void)0";
}

/**
 * @brief Generate `EX_IDENT`: emit the identifier, or a frame field access inside a coroutine
 *        body.
 *
 * A function used as a value contributes its **address**: C would decay a function designator to
 * a pointer anyway, but only writing `&` makes the expression's C type exactly the pointer the
 * `fn` type promises.
 *
 * A function with a hidden home arena/zone is rejected here instead of being emitted: it has one
 * parameter more than the type declares, and those two markers (the call-graph closure) are only
 * final once every function body has been checked -- this is the first point where the answer is
 * final.
 *
 * Parameters and locals that stay live across a `yield` in a coroutine live in the frame (an
 * ordinary struct), and every mention of them becomes a field access; `FuncDef.coroFrame` is the
 * table the checker prepared.
 *
 * The `EX_IDENT` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_IDENT` node.
 * @return   The C expression; `"0"` for a function with hidden arguments.
 */
static const char *exIdent(CG *g, Expr *e) {
    /* A function used as a value: the expression is its **address**. C would also convert a
     * function designator to a pointer by itself, but writing the `&` out is what makes the
     * expression's C type the pointer the `fn` type promises.
     *
     * A function whose C signature cannot be the type's is refused here rather than emitted: a
     * function that takes a hidden home arena or zone has one parameter more than the type says,
     * and those two flags are settled only after every body has been checked (they are transitive
     * closures), so this is the first place the answer is final. A differing pointer type would
     * otherwise be reported against generated code instead of against the line the author wrote. */
    if (planCallee(e)) {
        if (funcTakesHomeArena(planCallee(e)) || funcTakesHomeZone(planCallee(e))) {
            ctxError(g->ctx, e->line, 1,
                     "A function that takes a home arena or zone is compiled with one hidden"
                     " parameter more than its source signature, so its address does not have"
                     " the type this `fn` says. Call it, or write a free function that wraps"
                     " it without the hidden argument.",
                     "`%s` takes a hidden arena parameter, so its address is not a `fn` value",
                     planCallee(e)->name ? planCallee(e)->name : "?");
            return "0";
        }
        return arenaPrintf(g->arena, "&%s", cFuncName(g, planCallee(e)));
    }
    /* `for i in 0..N`: `N` is a `const` parameter, so the number is the instance's argument -- the
     * array type the caller passed. The body is shared, the emission is not. */
    {
        int lp = planLenParam(e);
        if (lp >= 0 && g->curFunc && (size_t)lp < g->curFunc->targs.len) {
            /* `vecAt` takes a mutable Vec, and `curFunc` is const here: the cast is the read-only
             * use of an accessor that has no const overload. Nothing below writes through it. */
            Type *b = *(Type *const *)vecAt((Vec *)&g->curFunc->targs, (size_t)lp);
            if (b && b->kind == TY_ARRAY && b->asize >= 0)
                return arenaPrintf(g->arena, "%lld", (long long)b->asize);
            ctxError(g->ctx, e->line, 1, NULL,
                     "the length `%s` could not be inferred from the call: pass an array whose"
                     " length is known", e->u.ident.name);
            return "0";
        }
    }
    const char *cn = planCName(e) ? planCName(e) : e->u.ident.name;
    /* A coroutine's parameters and its live-across-`yield` locals live in the frame, which is a
     * plain struct: every mention of one becomes a field access (`FuncDef.coroFrame` is the list
     * the checker laid out). */
    if (g->coroFunc && cn) {
        for (size_t i = 0; i < g->coroFunc->coroFrame.len; i++) {
            const Param *p = (const Param *)vecAt((Vec *)&g->coroFunc->coroFrame, i);
            if (p->cname && strcmp(p->cname, cn) == 0)
                return arenaPrintf(g->arena, "%s->%s", g->coroFrame, cn);
        }
    }
    return cn;
}

/**
 * @brief Generate `EX_CALL`: dispatch to the emission path for the callee's and arguments'
 *        shapes.
 *
 * Handled in order: `domain::single()` domain construction, the plate `viewOf` gate,
 * `parallel::run`, the coroutine handle protocol, a boxed coroutine call, a function-value call,
 * and finally an ordinary function (including the callee's implicit trailing arguments and
 * `@overwrite` unit).
 *
 * The `EX_CALL` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_CALL` node.
 * @return   The call expression; `"0"` when no callee was resolved or the shape is unsupported.
 */
static const char *exCall(CG *g, Expr *e) {
    /* `domain::single()`: build a domain (an opaque pointer). Recognized by the **node marker** the
     * checker stamps on it. */
    if (planDomNew(e)) { g->needDomain = true; return "extc_dom_new()"; }
    /* The plate layer's one primitive: a call to the helper the declaration triggers. The callee
     * is a `@builtin` with no body, so there is no `FuncDef` to name -- hence the flag, exactly as
     * `domNew` above. */
    if (planViewOf(e)) {
        Buf vb;
        bufInit(&vb, g->arena);
        bufPuts(&vb, "extc_viewOf(");
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            if (i) bufPuts(&vb, ", ");
            bufPuts(&vb, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
        }
        bufPutc(&vb, ')');
        return bufCstr(&vb);
    }
    if (planParWorker(e)) {
        /* `parallel::run(worker, views..., n, threads)`: ctx is the trampoline's "one field per
         * view" struct, read-only views are passed through as they are and the output one is split
         * into [lo,hi) chunks inside the trampoline; every argument is evaluated **exactly once**.
         * Passing 0 for the chunk lets the runtime pick (about 8 chunks per thread, taken
         * dynamically). */
        FuncDef *wf = planParWorker(e);
        /* Parameter layout: `(id, lo, hi, captures..., out)`. A capture is either a read-only view or
         * a scalar captured by value (only `par` desugaring produces scalars; hand-written workers
         * are all views); the output is the **only** `mut` view among them and is not necessarily
         * last (a hand-written worker may put it between captures). */
        /* Invariant: only the desugared `(id, lo, hi, captures..., out)` shape reaches this
         * branch, so there are at least four parameters. An underflow would walk the parameter
         * vector off its end, so the shape is asserted instead of assumed. */
        EXTC_DBG_ASSERT(wf->params.len >= 4);
        size_t nCaps = wf->params.len - 4, outIdx = 0;
        for (size_t i = 0; i <= nCaps; i++)
            if ((*(Param **)vecAt(&wf->params, 3 + i))->type->mut) outIdx = i;
        const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", wf->name);
        const char *tmp = arenaPrintf(g->arena, "__extc_pc%d", g->tmpSeq++);
        const char *ntmp = arenaPrintf(g->arena, "__extc_pn%d", g->tmpSeq++);
        Buf init;
        bufInit(&init, g->arena);
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&init, "%s.v%zu = (%s)", i ? ", " : "", i,
                      genExpr(g, *(Expr **)vecAt(&e->u.call.args, 1 + i)));
        pfLine(g, "%s %s = { %s };", ctn, tmp, bufCstr(&init));
        pfLine(g, "int64_t %s = (int64_t)(%s);", ntmp,
               genExpr(g, *(Expr **)vecAt(&e->u.call.args, 2 + nCaps)));
        /* Chunks are cut by [lo,hi) => an output shorter than n would be cut out of bounds. It is
         * rejected here (the checks inside the worker are a second line of defence; an
         * `@unchecked` worker has none). */
        pfLine(g, "if (%s > 0 && %s.v%zu.len < %s)"
                  " extc_trapMsg(\"%s\", %d, \"parallel::run: the output view is shorter than n\");",
               ntmp, tmp, outIdx, ntmp, g->path, e->line);
        g->needPar = true;
        /* A worker that allocates takes its memory from the run's region, so the region
         * half of the runtime has to be compiled in for this program. */
        if (planUsesHome(wf)) g->needParRegion = true;
        /* `par` desugaring writes the threads count as the `-1` sentinel: no thread count means "as
         * many cores as this machine can use". At run time a value <= 1 would silently run
         * sequentially, so it is replaced here by `extc_par_default_threads()`. */
        if (e->u.call.args.len > 3 + nCaps) {
            Expr *th = *(Expr **)vecAt(&e->u.call.args, 3 + nCaps);
            if (th && th->kind == EX_INT && th->u.ival == -1)
                return arenaPrintf(g->arena,
                    "extc_par_run(__extc_par_%s, &%s, %s, %s,"
                    " extc_par_default_threads(), 0)",
                    wf->name, tmp, planUsesHome(wf) ? "1" : "0", ntmp);
        }
        return arenaPrintf(g->arena,
            "extc_par_run(__extc_par_%s, &%s, %s, %s, (int64_t)(%s), 0)",
            wf->name, tmp, planUsesHome(wf) ? "1" : "0", ntmp,
            genExpr(g, *(Expr **)vecAt(&e->u.call.args, 3 + nCaps)));
    }
    /* A handle-protocol call can arrive here too (a `ref coroutine<T>` receiver resolves as
     * an ordinary call, not as EX_METHOD), so both paths share the dispatch. */
    if (planCallee(e) && !planIsCoro(planCallee(e)) && e->u.call.callee->kind == EX_FIELD) {
        /* `self` is implicit, so a `ref` receiver arrives as a call with **no** arguments and
         * the receiver sits in the callee's field expression. The receiver's type decides
         * whether this is the handle protocol. */
        Expr *rf = e->u.call.args.len > 0
                       ? *(Expr **)vecAt(&e->u.call.args, 0)
                       : (e->u.call.callee->kind == EX_FIELD ? e->u.call.callee->u.field.obj
                                                             : NULL);
        int hp = 0;
        if (rf && isProtoType(ttBase(subst(g, rf->type)), "coroutine", 1))
            hp = (planCallee(e)->name && strcmp(planCallee(e)->name, "value") == 0) ? 4
               : (planCallee(e)->name && strcmp(planCallee(e)->name, "send") == 0)  ? 5 : 3;
        if (hp) {
            const char *rc = genExpr(g, rf);
            const bool viaRef = rf->type && rf->type->kind == TY_REF;
            g->needCoroHandle = true;
            if (hp == 3)
                return arenaPrintf(g->arena, "extc_coro_next(%s%s, \"%s\", %d)",
                                   viaRef ? "" : "&", rc, g->path, e->line);
            if (hp == 5) {
                /* `self` implicit: the value sits in `args[0]`; explicit: after the receiver. */
                size_t vi = (e->u.call.callee->kind == EX_FIELD) ? 0 : 1;
                const char *vv = (vi < e->u.call.args.len)
                                     ? genExpr(g, *(Expr **)vecAt(&e->u.call.args, vi)) : "0";
                Type *rb2 = ttBase(subst(g, rf->type));
                Type *yt2 = (rb2 && rb2->targs.len)
                                ? subst(g, *(Type **)vecAt(&rb2->targs, 0)) : NULL;
                return arenaPrintf(g->arena, "extc_coro_send_%s(%s%s, %s, \"%s\", %d)",
                                   yt2 ? cType(g, yt2) : "int64_t",
                                   viaRef ? "" : "&", rc, vv, g->path, e->line);
            }
            return arenaPrintf(g->arena, "extc_coro_value_%s(%s%s, \"%s\", %d)",
                               cType(g, subst(g, planCallee(e)->ret)), viaRef ? "" : "&", rc,
                               g->path, e->line);
        }
    }
    if (planCallee(e) && planIsCoro(planCallee(e)) && planBoxedCoro(e)) {
        /* **Boxing**: this coroutine value is stored somewhere that outlives the current C scope,
         * so its frame goes into the task's own place and the value is a `{frame, kind, task}`
         * handle -- a plain value, safe to copy and to store in a container. The setup statements
         * must precede the statement that reads the handle, so they go to the prefix (C11 has no
         * statement expressions). */
        FuncDef *cf = planCallee(e);
        const char *cn = cFuncName(g, cf);
        int sq = g->coroSeq++;
        g->needCoroHandle = true;
        pfLine(g, "int64_t __extc_czh%d = extc_task_begin();", sq);
        pfLine(g, "int64_t __extc_czz%d = extc_task_zone(__extc_czh%d);", sq, sq);
        pfLine(g, "struct %s$frame *__extc_czf%d = (struct %s$frame *)extc_task_alloc("
                   "__extc_czh%d, (int64_t)sizeof(struct %s$frame), \"%s\", %d);",
               cn, sq, cn, sq, cn, g->path, e->line);
        Buf init;
        bufInit(&init, g->arena);
        bufPrintf(&init, "*__extc_czf%d = (struct %s$frame){ .pc = 0, .zone = __extc_czz%d,"
                         " .task = __extc_czh%d", sq, cn, sq, sq);
        for (size_t i = 0; i < cf->params.len; i++) {
            Param *p = *(Param **)vecAt(&cf->params, i);
            Expr *a = i < e->u.call.args.len ? *(Expr **)vecAt(&e->u.call.args, i) : NULL;
            bufPrintf(&init, ", .%s = %s", p->cname, a ? genExpr(g, a) : "0");
        }
        bufPrintf(&init, " };");
        pfLine(g, "%s", bufCstr(&init));
        g->needCoroHandle = true;
        return arenaPrintf(g->arena,
                           "((extc_coro){ .frame = __extc_czf%d, .kind = %d,"
                           " .task = __extc_czh%d })", sq, planCoroKind(cf), sq);
    }
    if (planCallee(e) && planIsCoro(planCallee(e))) {
        /* A coroutine call that is neither boxed nor driven is a loud failure at the C compiler
         * rather than a silent miscompile -- except under `EXTC_CORO_B1_HARNESS`, which a small C
         * harness defines so it can drive the generated `$step` itself. */
        cgLine(g, "#ifndef EXTC_CORO_B1_HARNESS");
        cgLine(g, "#error \"calling a coroutine lands in slice B2"
                   "\"");
        cgLine(g, "#endif");
        return "0";
    }
    /* A call **through a value** of function type: the callee is an expression holding a code
     * pointer, so there is no C name to print -- C calls through the pointer directly. The
     * checker has already matched every argument against the signature the type carries; this only
     * prints the expression. The parentheses are not decoration: a callee that is a compound
     * expression or a dereference must not bind to the argument list. */
    if (planCallViaFn(e)) {
        Buf fb;
        bufInit(&fb, g->arena);
        bufPutc(&fb, '(');
        bufPuts(&fb, genExpr(g, e->u.call.callee));
        bufPuts(&fb, ")(");
        for (size_t i = 0; i < e->u.call.args.len; i++) {
            if (i) bufPuts(&fb, ", ");
            bufPuts(&fb, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
        }
        bufPutc(&fb, ')');
        return bufCstr(&fb);
    }
    if (e->u.call.callee->kind != EX_IDENT) return "0";
    const char *name = e->u.call.callee->u.ident.name;
    /* Both spellings reach here -- the bare builtin and the module-qualified `io::print` the
     * checker also accepts -- so the name comparison looks at the last component. */
    if (builtinNameIs(name, "print"))   return genPrint(g, &e->u.call.args, false);
    if (builtinNameIs(name, "println")) return genPrint(g, &e->u.call.args, true);
    /* `flush()` becomes `fflush(NULL)`; <stdio.h> is already included
     * by the runtime. */
    /* `flush()` only has to drain the cout buffer (the built-in printing writes to that buffer
     * too); `fflush` stays for external code that writes bytes through stdio. */
    if (strcmp(name, "flush") == 0) {
        g->needCout = true;
        return "(extc_cout_flush(), fflush((void *)0))";
    }
    if (!planCallee(e)) return "0";

    /* Only `cSymName` is used, without the owner prefix: a call site names the callee itself,
     * whereas `ownerPrefix` is the instance currently being generated. This also renames a name
     * that collides with a C keyword (`fn double`).
     *
     * `@export` has to be asked first: the definition names the C symbol after the author's
     * spelling with the module prefix dropped (`cFuncName`'s export branch), while a cross-module
     * call site spells the identifier `lib$exported`. Printing that identifier verbatim calls a
     * symbol nobody defines. */
    FuncDef *ce = planCallee(e);
    if (ce && ce->isExport)
        name = exportName(ce);
    else
        name = (ce && planInstName(ce)) ? planInstName(ce) : cSymName(g, name);
    if (strcmp(name, "extc_cout_f64") == 0) g->needCoutF64 = true;
    /* The buffer substrate rides on the **call**, not on the declaration: `std::sys::bufrt` declares
     * its five entries and every container module imports it, so a declaration-based trigger would
     * put the whole block into every program that merely imports `string`. Only a call site can say
     * the storage is used. */
    if (ce && ce->isExtern && ce->externLib && strcmp(ce->externLib, "extc-buf") == 0)
        g->needBuf = true;
    Buf b;
    bufInit(&b, g->arena);
    bufPuts(&b, name);
    bufPutc(&b, '(');
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        /* A by-value argument of an `@value` type is a **copy that has to be built**: the callee`s
         * parameter owns its storage and drops it on the way out, so handing over the place itself
         * would hand over the caller`s storage too. The checker marked the argument. */
        Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
        FuncDef *cf = planCallee(e);
        /* The destination is the **parameter's** type, not the argument's: inside a template body
         * the argument may be a `T` place while the parameter is already a concrete instance type,
         * and the clone hook is named after the destination. */
        Type *pt = (cf && i < cf->params.len) ? (*(Param **)vecAt(&cf->params, i))->type : a->type;
        const char *av = argCopyNeedsClone(g, a, pt) ? cloneValueText(g, pt, a) : NULL;
        if (!av && cf && i < cf->params.len) noteArgMove(g, a, pt);
        bufPuts(&b, av ? av : genExpr(g, a));
    }
    /* Asking the owner **once** is enough: it hands out both home and zone. Asking twice would
     * emit both into every function that needs both. */
    cgImplicitArgs(g, &b, planCallee(e), e, e->u.call.args.len, false);
    owPassCells(g, &b, e, e->u.call.args.len, cgHasImplicitArgs(planCallee(e)));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Test whether this `EX_INDEX` site is already covered by the entry range check of an
 *        enclosing range-for.
 *
 * Looks up the table the generator keeps by node identity (`loopHoistRangeFor` pushes on it
 * before entering the loop body and truncates it on the way out). On a hit `exIndex` emits a bare
 * pointer access instead of checking the bound per element.
 *
 * @param g  The generator.
 * @param e  The `EX_INDEX` node.
 * @return   true when covered.
 */
static bool idxHoisted(CG *g, Expr *e);

/**
 * @brief Generate `EX_INDEX`: array subscript and view subscript.
 *
 * An array's length is a compile-time constant, so the emitted check carries the length
 * directly. A view goes through the checking primitive that `viewIndexer` selects; it returns a
 * pointer whose dereference is an lvalue -- reads, taking the address to pass to a `ref`
 * parameter and assignments all work, and asking for it here is also what guarantees the
 * primitive has a definition.
 *
 * Inside an `@unchecked` function both sides drop the check: an array becomes
 * `obj.data[idx]` and a view is written in the lvalue form `*((v).data + i)`.
 *
 * The `EX_INDEX` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_INDEX` node.
 * @return   The C expression; `"0"` when the base type is neither an array nor a view.
 */
static const char *exIndex(CG *g, Expr *e) {
    Type *ot = e->u.index.obj->type;
    Type *ob = ttBase(subst(g, ot));
    const char *obj = genExpr(g, e->u.index.obj);
    const char *idx = genExpr(g, e->u.index.index);

    /* Arrays: the length is a compile-time constant, so obj appears
     * exactly once and cannot be evaluated twice. */
    if (ob && ob->kind == TY_ARRAY) {
        /* With a `ref [N]T` base, `obj` is a pointer in C and must be dereferenced one level
         * first, or `p.data[..]` would be a subscript on a pointer. */
        if (ot && ot->kind == TY_REF)
            obj = arenaPrintf(g->arena, "(*%s)", obj);
        /* `@unchecked`: the length is still known here, but the programmer has signed for the
         * range, so the index is used as written. No `extc_checkedIndex` call means no trap
         * position -- that is the cost of the annotation. */
        if (g->uncheckedBody)
            return arenaPrintf(g->arena, "%s.data[%s]", obj, idx);
        return arenaPrintf(g->arena,
            "%s.data[extc_checkedIndex((int64_t)(%s), %lld, \"%s\", %d)]",
            obj, idx, (long long)ob->asize, g->path, e->line);
    }
    if (!isView(ob)) return "0";
    /* The primitive takes the view by value, so a reference is
     * dereferenced. */
    if (ot && ot->kind == TY_REF) obj = arenaPrintf(g->arena, "*(%s)", obj);

    /* `@unchecked`: the view's element is reached through its data pointer directly. The shape is
     * the checked primitive's, `&v.data[i]` written out as an lvalue (`*(p + i)`), so a read, an
     * assignment and a `ref` argument all still work, and the `file`/`line` arguments have nowhere
     * to go because there is no check. The checked primitive is *not* emitted for this body:
     * nothing in it names that one. */
    if (g->uncheckedBody)
        return arenaPrintf(g->arena, "(*((%s).data + (int64_t)(%s)))", obj, idx);

    /* This subscript is already covered by the entry range check of an enclosing `for` (see
     * `loopHoistRangeFor`): the index is necessarily in bounds, so go through the bare pointer and
     * save the per-iteration null/bounds branch. The shape is the same as an `@unchecked` view
     * subscript, so reads, assignments and taking a `ref` still work. */
    if (idxHoisted(g, e))
        return arenaPrintf(g->arena, "(*((%s).data + (int64_t)(%s)))", obj, idx);

    /* The primitive returns a pointer and the dereference is an lvalue: it can be read, addressed
     * for a `ref` parameter, and assigned to. Asking for the primitive here is also what guarantees
     * its definition exists: an index inside a generic body names the instance, and the list of
     * instances is not necessarily complete otherwise. */
    const char *ix = viewIndexer(g, ob);
    return arenaPrintf(g->arena, "(*%s(%s, (int64_t)(%s), \"%s\", %d))",
                       ix, obj, idx, g->path, e->line);
}

/**
 * @brief Generate `EX_ASSOC`: a module-qualified associated function call.
 *
 * An associated function's C name is decorated with the instance name (`option_i64_some`) under
 * the same rule as a method's, so `cMethodName` is reused directly; the implicit trailing
 * arguments and the `@overwrite` unit are appended afterwards.
 *
 * A module-qualified call goes through an assoc node, so domain construction has to recognize the
 * node marker here as well (the same as in `exCall`).
 *
 * The `EX_ASSOC` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_ASSOC` node.
 * @return   The C call expression.
 */
static const char *exAssoc(CG *g, Expr *e) {
    /* A module-qualified call goes through an assoc node => domain construction is handled here
     * too (as in `exCall`, by node marker). */
    if (planDomNew(e)) { g->needDomain = true; return "extc_dom_new()"; }
    /* An associated function decorates its C name with the instance
     * name (`option_i64_some`), which is the same decoration rule
     * methods follow, so cMethodName is reused directly. */
    Buf b;
    bufInit(&b, g->arena);
    bufPuts(&b, cMethodName(g, planAssocOwner(e), planCallee(e)));
    bufPutc(&b, '(');
    for (size_t i = 0; i < e->u.assoc.args.len; i++) {
        if (i) bufPuts(&b, ", ");
        /* Same rule as an ordinary call: a by-value `@value` argument is built, not copied. */
        Expr *a = *(Expr **)vecAt(&e->u.assoc.args, i);
        FuncDef *cf = planCallee(e);
        Type *pt = (cf && i < cf->params.len) ? (*(Param **)vecAt(&cf->params, i))->type : a->type;
        const char *av = argCopyNeedsClone(g, a, pt) ? cloneValueText(g, pt, a) : NULL;
        if (!av && cf && i < cf->params.len) noteArgMove(g, a, pt);
        bufPuts(&b, av ? av : genExpr(g, a));
    }
    /* An associated function such as `Type::make()` allocates, so the
     * home arena argument is appended. */
    cgImplicitArgs(g, &b, planCallee(e), e, e->u.assoc.args.len, false);
    owPassCells(g, &b, e, e->u.assoc.args.len, cgHasImplicitArgs(planCallee(e)));   /* @overwrite cells */
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/**
 * @brief Generate `EX_CONV`: a checked or unchecked numeric conversion.
 *
 * Under `@unchecked`, or when the checker did not mark this conversion checked, it is a C cast.
 * Otherwise the target type's range is checked through the `static inline` primitives
 * `extc_narrowI` / `extc_narrowU` / `extc_convFloat`, whose out-of-range case traps at run time.
 * The range comes from the same table the checker uses (`ttIntBits` / `ttIntSigned`). When the
 * target is floating point the primitive's result is used as it is, without an extra cast (see
 * the function body).
 *
 * The `EX_CONV` arm of `genExprInner` lands here.
 *
 * @param g  The generator.
 * @param e  The `EX_CONV` node.
 * @return   The C text of the conversion expression.
 */
static const char *exConv(CG *g, Expr *e) {
    Type *t = subst(g, e->u.conv.type);          /* target type, from the checker */
    const char *x = genExpr(g, e->u.conv.operand);
    /* `@unchecked` signs the conversion check away as well: the value is cast the
     * way C casts it, and `extc_narrowI` / `extc_convFloat` are not named. */
    if (!planConvCheck(e) || g->uncheckedBody)
        return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

    /* The target is floating point (int->float, f32->f64): one C cast is the whole semantics --
     * round to nearest, and an integer is always inside the target float's range. This arm must
     * come **before** the integer range checks below: those read the operand as unsigned
     * (`(uint64_t)`), which turns a negative value into a huge one near 2^64 (guard:
     * tests/ops/conv_int_float.extc). */
    if (ttIsFloat(t))
        return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

    /* A checked conversion: look the range up in a table and go
     * through one of the `static inline` helpers. */
    const char *tn = t->name;
    bool isF = ttIsFloat(subst(g, e->u.conv.operand->type));
    if (isF) {
        /* The range comes from the same table the checker uses (`ttIntBits` / `ttIntSigned`)
         * instead of a hand-written chain per type name. The two 64-bit ends cannot be written as
         * `%lldLL` (`INT64_MIN` / `INT64_MAX` / `UINT64_MAX` are macros), so they take the macro
         * spelling. */
        int bits = ttIntBits(t);
        if (bits == 0)                          /* not an integer: the plain cast is the whole conversion */
            return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);
        if (bits == 64) {
            if (!ttIntSigned(t))        /* u64: the helper's bounds are int64_t, so it gets its own */
                return arenaPrintf(g->arena,
                    "((%s)extc_convFloatU((double)(%s), \"%s\", %d))",
                    cType(g, t), x, g->path, e->line);
            return arenaPrintf(g->arena,
                "((%s)extc_convFloat((double)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                cType(g, t), x, g->path, e->line);
        }
        int64_t lo = ttIntSigned(t) ? -((int64_t)1 << (bits - 1)) : 0;
        int64_t hi = ttIntSigned(t) ? (((int64_t)1 << (bits - 1)) - 1)
                                    : (((int64_t)1 << bits) - 1);
        return arenaPrintf(g->arena,
            "((%s)extc_convFloat((double)(%s), %lldLL, %lldLL, \"%s\", %d))",
            cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
    }
    bool sign = (tn[0] == 'i');
    if (sign) {
        int64_t lo = 0, hi = 0;
        if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
        else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
        else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
        else
            return arenaPrintf(g->arena,
                "((%s)extc_narrowI((int64_t)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                cType(g, t), x, g->path, e->line);
        return arenaPrintf(g->arena,
            "((%s)extc_narrowI((int64_t)(%s), %lldLL, %lldLL, \"%s\", %d))",
            cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
    }
    /* Target is not floating point => only the integer range check remains (unsigned targets,
     * including a sign change). A floating-point target returned the C cast above. */
    unsigned long long hi = 0;
    if (strcmp(tn,"u8")==0)  hi = 255ULL;
    else if (strcmp(tn,"u16")==0) hi = 65535ULL;
    else if (strcmp(tn,"u32")==0) hi = 4294967295ULL;
    else
        return arenaPrintf(g->arena,
            "((%s)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))",
            cType(g, t), x, g->path, e->line);
    return arenaPrintf(g->arena,
        "((%s)extc_narrowU((uint64_t)(%s), %lluULL, \"%s\", %d))",
        cType(g, t), x, hi, g->path, e->line);
}

