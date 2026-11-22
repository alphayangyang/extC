/* --------------------------------------- view-subscript loop-entry check
 *
 * `a[i]` calls a checking primitive on every access (null pointer + bounds, trapping out of range)
 * and then returns a pointer. The check itself is cheap, but it is a **branch** inside an inner loop
 * body, which alone blocks vectorization of the whole loop; and the most common subscript in a
 * `for k in lo..hi` body is exactly affine in k (`a[r*n + c]`), with the loop's whole trip count
 * known before entry (the parser desugars the upper bound into an `__extc_hi` temporary, evaluated
 * once).
 *
 * Once that is recognized, the body's view subscripts affine in k are collected and one range check
 * per (view, coef, base) is emitted before the `while`, with the body switching to bare pointer
 * access: the checked range is the two endpoint values of `coef*k + base` over k in [lo, hi), and
 * run time compares which is larger. Out of range still traps, with the same message
 * `trap: index %lld out of range (length %lld)` -- it is just reported at loop entry.
 *
 * Only this conservative tier is done; when it cannot be proved the whole loop falls back to
 * per-element checks:
 *   - the body contains a call / allocation / trap / early exit / conditionally evaluated
 *     expression -- don't touch it;
 *   - the subscript is not affine scalar arithmetic, or the index type is not i64 (i32 wraps, so
 *     recomputing is not equivalent) -- not accepted;
 *   - a quantity in coef/base is assigned in the body -- not accepted;
 *   - the view object is not a plain identifier, the view itself is rebound in the body, or the
 *     site is inside a conditional branch -- not accepted;
 *   - an `@unchecked` body (the author turned checks off) or a coroutine body (a state machine
 *     shape) -- don't touch it.
 *
 * Bare access is recorded by node identity: `g->hoistedIdx` holds the `EX_INDEX` nodes already
 * covered, and `exIndex` consults the table to decide between the checking primitive and a bare
 * pointer. Nested loops append their own entries and truncate the length back to the entry `mark`
 * when leaving the body.
 */

/** @brief One view-subscript site covered by a loop-entry check. */
typedef struct {
    const char *view;   /* the view object's C name */
    const char *coef;   /* the coef of the subscript = coef*k + base (a C expression) */
    const char *base;   /* as above, base (a C expression without k) */
    int         line;   /* the site's line number (for the trap) */
    Expr       *site;   /* the covered EX_INDEX node */
} HoistedIdx;

/** @brief The text form of `coef*k + base` (a coef of "0" means it does not depend on k). */
typedef struct {
    const char *coef;
    const char *base;
} Affine;

/**
 * @brief The C name of an identifier: when the checker renamed it (shadowing, or a suffix on
 *        `__extc_hi`), the renamed one is used.
 *
 * @param e  The expression; returns NULL when it is not `EX_IDENT`.
 */
static const char *identCName(const Expr *e) {
    if (!e || e->kind != EX_IDENT) return NULL;
    const char *cn = planCName(e);
    return cn ? cn : e->u.ident.name;
}

/**
 * @brief Is this expression one whose "evaluating once more at loop entry" does not change the
 *        program's behavior: no call, no allocation, no assignment?
 *
 * @param e  The expression to judge.
 */
static bool hoistPureExpr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_IDENT: return true;
    case EX_BIN: return hoistPureExpr(e->u.bin.left) && hoistPureExpr(e->u.bin.right);
    case EX_UN: return hoistPureExpr(e->u.un.operand);
    default: return false;
    }
}

/**
 * @brief Does `e` contain an identifier whose C name is `name`?
 *
 * @param e     The expression to search (only the node kinds a pure expression can have).
 * @param name  The C name to compare against.
 */
static bool exprHasIdent(const Expr *e, const char *name) {
    if (!e || !name) return false;
    switch (e->kind) {
    case EX_IDENT: {
        const char *cn = identCName(e);
        return cn && strcmp(cn, name) == 0;
    }
    case EX_BIN: return exprHasIdent(e->u.bin.left, name) || exprHasIdent(e->u.bin.right, name);
    case EX_UN: return exprHasIdent(e->u.un.operand, name);
    default: return false;
    }
}

/**
 * @brief Try to split the expression into `coef*k + base`; returns false when it cannot be split.
 *
 * `coef` and `base` are **text**: a subexpression independent of k is emitted as it is and evaluated
 * once at entry. Only scalar arithmetic (`+`, `-`, `*`, unary `-`) is recognized; no symbolic prover
 * is brought in.
 *
 * @param g    The generator (temporary text is allocated in its arena).
 * @param e    The subscript expression.
 * @param var  The loop variable's C name.
 * @param out  The split-out coefficient and constant term.
 */
static bool affineIn(CG *g, Expr *e, const char *var, Affine *out) {
    if (!e || !var) return false;
    /* a pure expression independent of k is used whole as the constant term: emitted as it is and
     * evaluated once at loop entry. */
    if (hoistPureExpr(e) && !exprHasIdent(e, var)) {
        out->coef = "0";
        out->base = genExpr(g, e);
        return true;
    }
    if (e->kind == EX_IDENT) {
        const char *cn = identCName(e);
        if (cn && strcmp(cn, var) == 0) { out->coef = "1"; out->base = "0"; return true; }
        return false;
    }
    if (e->kind == EX_BIN && e->u.bin.op) {
        const char *op = e->u.bin.op;
        if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
            Affine l, r;
            if (!affineIn(g, e->u.bin.left, var, &l)) return false;
            if (!affineIn(g, e->u.bin.right, var, &r)) return false;
            const bool sub = strcmp(op, "-") == 0;
            if (strcmp(l.coef, "0") == 0 && strcmp(r.coef, "0") == 0) return false;
            out->coef = arenaPrintf(g->arena, "(%s %s %s)", l.coef, sub ? "-" : "+", r.coef);
            out->base = arenaPrintf(g->arena, "(%s %s %s)", l.base, sub ? "-" : "+", r.base);
            return true;
        }
        if (strcmp(op, "*") == 0) {
            /* only one side may contain k: the other side is multiplied across entirely. Both sides containing
     * k is nonlinear and not accepted. */
            const bool lk = exprHasIdent(e->u.bin.left, var);
            const bool rk = exprHasIdent(e->u.bin.right, var);
            if (lk == rk) return false;
            Expr *kSide = lk ? e->u.bin.left : e->u.bin.right;
            Expr *cSide = lk ? e->u.bin.right : e->u.bin.left;
            Affine kk;
            if (!affineIn(g, kSide, var, &kk)) return false;
            const char *factor = genExpr(g, cSide);
            out->coef = arenaPrintf(g->arena, "(%s * %s)", kk.coef, factor);
            out->base = arenaPrintf(g->arena, "(%s * %s)", kk.base, factor);
            return true;
        }
        return false;
    }
    if (e->kind == EX_UN && e->u.un.op && strcmp(e->u.un.op, "-") == 0) {
        Affine kk;
        if (!affineIn(g, e->u.un.operand, var, &kk)) return false;
        out->coef = arenaPrintf(g->arena, "(-%s)", kk.coef);
        out->base = arenaPrintf(g->arena, "(-%s)", kk.base);
        return true;
    }
    return false;
}

/**
 * @brief Collect the C names of the identifiers in `e` other than the loop variable; returns false
 *        when one of them is not a built-in scalar type.
 *
 * Only built-in scalars are let through: struct fields, views, references and "something a function
 * returns" cannot be hoisted as a constant term.
 *
 * @param g      The generator (needs `subst`).
 * @param e      The subscript expression.
 * @param var    The loop variable's C name (its type need not be a built-in scalar).
 * @param names  The collected result (`const char *`).
 */
static bool hoistCollectIdents(CG *g, const Expr *e, const char *var, Vec *names) {
    if (!e) return true;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: return true;
    case EX_IDENT: {
        const char *cn = identCName(e);
        if (cn && var && strcmp(cn, var) == 0) return true;
        Type *t = e->type ? ttBase(subst(g, e->type)) : NULL;
        if (!t || t->kind != TY_BUILTIN) return false;
        if (!cn) return false;
        *(const char **)vecPush(names) = cn;
        return true;
    }
    case EX_BIN:
        return hoistCollectIdents(g, e->u.bin.left, var, names)
            && hoistCollectIdents(g, e->u.bin.right, var, names);
    case EX_UN: return hoistCollectIdents(g, e->u.un.operand, var, names);
    default: return false;
    }
}

/** @brief Is `name` already in `names` (`const char *`)? */
static bool nameInVec(const Vec *names, const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < names->len; i++)
        if (strcmp(*(const char **)vecAt((Vec *)names, i), name) == 0) return true;
    return false;
}

/** @brief Does the body contain something that hoisting the check would change: a call, an
 *         allocation, a trap, an early exit, a lambda... */
static bool hoistForbiddenExpr(const Expr *e);

/**
 * @brief Does the statement tree contain a construct forbidden for hoisting the check (an early
 *        exit, a call, a trap, a conditional evaluation)?
 *
 * An early exit is hard-forbidden: moving the check of `a[k]` to loop entry means trapping before a
 * `break`/`return`. A conditionally evaluated expression (`??`, short-circuit evaluation, a call) is
 * the same: evaluating it once at entry changes how many times its side effects happen.
 *
 * @param s  The statement.
 */
static bool hoistForbiddenStmt(const Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR: return hoistForbiddenExpr(s->u.var.init);
    case ST_ASSIGN:
        return hoistForbiddenExpr(s->u.assign.target) || hoistForbiddenExpr(s->u.assign.value)
            || hoistForbiddenExpr(s->u.assign.opExpr);
    case ST_EXPR: return hoistForbiddenExpr(s->u.expr.expr);
    case ST_IF:
        return hoistForbiddenExpr(s->u.ifs.cond) || hoistForbiddenStmt(s->u.ifs.thenBody)
            || hoistForbiddenStmt(s->u.ifs.elseBody);
    case ST_WHILE:
        return hoistForbiddenExpr(s->u.whiles.cond) || hoistForbiddenStmt(s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (hoistForbiddenStmt(*(Stmt **)vecAt((Vec *)&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH: {
        if (hoistForbiddenExpr(s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt((Vec *)&s->u.match.arms, i);
            if (arm && hoistForbiddenStmt(arm->body)) return true;
        }
        return false;
    }
    /* Only the allowed kinds are listed: `break`/`continue`/`return`/`trap`/`yield`/`domain` and any
     * statement kind added later fall to default and are always forbidden (fall back to per-element
     * checks, semantics unchanged). */
    default: return true;
    }
}

/** @brief Does `e` contain a forbidden construct (only the node kinds that can appear in a pure
 *         expression are examined; anything else is forbidden)?
 *
 * A conversion is pure: it turns one value into another and cannot trap, call, or be skipped, so
 * the scan looks through it. Without that, `i64(v[i])` -- the shape nearly every byte loop has --
 * would read as a side effect and no loop containing one would ever hoist its bounds test. */
static bool hoistForbiddenExpr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_IDENT: case EX_NULL: return false;
    case EX_BIN:
        /* An operator may be resolved by the checker into a method call (`cout << x`, a user-defined
         * operator, array `==` going through `extc_eq`), or deferred until instantiation -- all
         * calls, so none of them is a pure expression. */
        if (planCallee(e) || planNeedOp(e)) return true;
        return hoistForbiddenExpr(e->u.bin.left) || hoistForbiddenExpr(e->u.bin.right);
    case EX_UN:
        if (planCallee(e) || planNeedOp(e)) return true;
        return hoistForbiddenExpr(e->u.un.operand);
    case EX_CONV: return hoistForbiddenExpr(e->u.conv.operand);
    case EX_FIELD: return hoistForbiddenExpr(e->u.field.obj);
    case EX_INDEX: return hoistForbiddenExpr(e->u.index.obj) || hoistForbiddenExpr(e->u.index.index);
    case EX_DEREF: return hoistForbiddenExpr(e->u.deref.operand);
    case EX_SIGN: return hoistForbiddenExpr(e->u.sign.operand);
    case EX_ARRAYLIT: {
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (hoistForbiddenExpr(*(Expr **)vecAt((Vec *)&e->u.arraylit.elems, i))) return true;
        return false;
    }
    default: return true;
    }
}

/** @brief Collect the view-subscript sites that certainly execute every round (does not cross
 *         conditional branches or inner loops). */
static void hoistSitesExpr(Expr *e, Vec *sites);

/**
 * @brief Collect the view-subscript sites that certainly execute every round along a statement
 *        chain.
 *
 * Only unconditional statements are crossed: the statement chain of `ST_BLOCK`, the initializer of
 * `ST_VAR`, both sides of an assignment, expression statements. Sites inside `if`/`match` branches
 * and inner `while` loops do not count as "executed every round" (not entered); a skipped site keeps
 * going through the per-element check, semantics unchanged.
 *
 * @param g      The generator.
 * @param s      The statement.
 * @param sites  The collected result (`Expr *`).
 */
static void hoistSitesStmt(CG *g, Stmt *s, Vec *sites) {
    (void)g;
    if (!s) return;
    switch (s->kind) {
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;   /* the step of a for runs only at the end of each round and is not collected here */
            hoistSitesStmt(g, one, sites);
        }
        return;
    }
    case ST_VAR: hoistSitesExpr(s->u.var.init, sites); return;
    case ST_ASSIGN:
        hoistSitesExpr(s->u.assign.target, sites);
        hoistSitesExpr(s->u.assign.value, sites);
        return;
    case ST_EXPR: hoistSitesExpr(s->u.expr.expr, sites); return;
    /* An `if` condition is evaluated every round, so its subscripts are covered too; the two
     * branches are not (a subscript there runs only on the rounds the branch is taken). */
    case ST_IF: hoistSitesExpr(s->u.ifs.cond, sites); return;
    default: return;
    }
}

/** @brief The view-subscript sites in an expression (only pure expressions are crossed;
 *         side-effecting nodes are already blocked as forbidden). */
static void hoistSitesExpr(Expr *e, Vec *sites) {
    if (!e) return;
    switch (e->kind) {
    case EX_INDEX:
        *(Expr **)vecPush(sites) = e;
        hoistSitesExpr(e->u.index.obj, sites);
        hoistSitesExpr(e->u.index.index, sites);
        return;
    case EX_BIN:
        hoistSitesExpr(e->u.bin.left, sites);
        hoistSitesExpr(e->u.bin.right, sites);
        return;
    case EX_UN: hoistSitesExpr(e->u.un.operand, sites); return;
    case EX_FIELD: hoistSitesExpr(e->u.field.obj, sites); return;
    case EX_CONV: hoistSitesExpr(e->u.conv.operand, sites); return;
    case EX_DEREF: hoistSitesExpr(e->u.deref.operand, sites); return;
    default: return;
    }
}

/** @brief Collect the C names of the identifiers assigned in the body (a for step does not count:
 *         it only advances the loop variable). */
static void hoistWrites(Stmt *s, Vec *names) {
    if (!s) return;
    switch (s->kind) {
    case ST_ASSIGN: {
        Expr *t = s->u.assign.target;
        if (t && t->kind == EX_IDENT) {
            const char *cn = identCName(t);
            if (cn) *(const char **)vecPush(names) = cn;
        }
        return;
    }
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;
            hoistWrites(one, names);
        }
        return;
    }
    case ST_IF:
        hoistWrites(s->u.ifs.thenBody, names);
        hoistWrites(s->u.ifs.elseBody, names);
        return;
    case ST_WHILE: hoistWrites(s->u.whiles.body, names); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
            if (arm) hoistWrites(arm->body, names);
        }
        return;
    default: return;
    }
}

/** @brief Collect the names declared in the body (`var`, match bindings; a for step declares no new
 *         name).
 *
 * The entry check is emitted **before** the loop, where names declared in the body are not in scope
 * yet: using them as the constant term of an affine form would make the generated C reference an
 * undeclared identifier (`off` in `a[off]` is exactly that). They are collected once and the sites
 * are rejected by name -- the check falls back to per-element, semantics unchanged, only without the
 * hoisting.
 */
static void hoistDecls(Stmt *s, Vec *names) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR: {
        const char *cn = planCName(s);
        if (cn) *(const char **)vecPush(names) = cn;
        return;
    }
    case ST_BLOCK: {
        Stmt *step = planForStep(s);
        for (size_t i = 0; i < s->u.block.stmts.len; i++) {
            Stmt *one = *(Stmt **)vecAt(&s->u.block.stmts, i);
            if (step && one == step) continue;
            hoistDecls(one, names);
        }
        return;
    }
    case ST_IF:
        hoistDecls(s->u.ifs.thenBody, names);
        hoistDecls(s->u.ifs.elseBody, names);
        return;
    case ST_WHILE: hoistDecls(s->u.whiles.body, names); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
            if (!arm) continue;
            for (size_t k = 0; k < arm->binds.len; k++) {
                const char *cn = *(const char **)vecAt(&arm->binds, k);
                if (cn) *(const char **)vecPush(names) = cn;
            }
            hoistDecls(arm->body, names);
        }
        return;
    default: return;
    }
}

/**
 * @brief Recognize the range-for shape and do entry range checks for the body's view subscripts
 *        (emit nothing when it cannot be proved).
 *
 * The shape is fixed by the checker's desugaring: `for k in lo..hi` is emitted as
 * `k = lo; while (k < __extc_hi) { body; k += 1 }`, with the upper bound evaluated once **outside**
 * the loop. The entry check therefore holds only for the trip range [lo, hi), and the out-of-range
 * decision for an array or view is exactly the per-element one (trapping early: only when the body
 * would really execute).
 *
 * @param g  The generator (uses `tmpSeq`, `arena`, `path`).
 * @param s  The `while` statement; the caller guarantees `!condTemps && !ownLevel` and that the
 *           function body has not turned checks off.
 */
static void loopHoistRangeFor(CG *g, Stmt *s) {
    if (!g || !s || g->uncheckedBody || g->coroFunc) return;
    Stmt *body = s->u.whiles.body;
    Expr *cond = s->u.whiles.cond;
    if (!body || body->kind != ST_BLOCK || !cond || cond->kind != EX_BIN) return;
    if (!cond->u.bin.op || strcmp(cond->u.bin.op, "<") != 0) return;
    Expr *lv = cond->u.bin.left, *rv = cond->u.bin.right;
    if (!lv || lv->kind != EX_IDENT || !rv || rv->kind != EX_IDENT) return;
    const char *var = identCName(lv);
    const char *hi = identCName(rv);
    if (!var || !hi || strncmp(hi, "__extc_hi", 9) != 0) return;
    if (!planForStep(body)) return;    /* a redirected step (iterator for) is not this trip range */
    if (hoistForbiddenStmt(body)) return;

    /* who is assigned in the body: the loop variable's trip, the upper bound, the constant terms and
     * the view object itself must not change. */
    Vec writes; vecInit(&writes, g->arena, sizeof(const char *));
    hoistWrites(body, &writes);
    if (nameInVec(&writes, var) || nameInVec(&writes, hi)) return;

    /* names declared in the body are not in scope at entry and cannot be the constant term of an
     * affine form. */
    Vec decls; vecInit(&decls, g->arena, sizeof(const char *));
    hoistDecls(body, &decls);

    Vec sites; vecInit(&sites, g->arena, sizeof(Expr *));
    hoistSitesStmt(g, body, &sites);

    Vec picks; vecInit(&picks, g->arena, sizeof(HoistedIdx));
    for (size_t i = 0; i < sites.len; i++) {
        Expr *site = *(Expr **)vecAt(&sites, i);
        Expr *obj = site->u.index.obj;
        Expr *ix = site->u.index.index;
        if (!obj || obj->kind != EX_IDENT || !ix || idxHoisted(g, site)) continue;
        Type *ot = obj->type;
        if (!ot || ot->kind == TY_REF) continue;
        if (!isView(ttBase(subst(g, ot)))) continue;
        Type *it = ix->type;
        if (!it) continue;
        it = ttBase(subst(g, it));
        if (!it || !ttIs(it, "i64")) continue;
        const char *view = identCName(obj);
        if (!view || nameInVec(&writes, view)) continue;
        Vec ids; vecInit(&ids, g->arena, sizeof(const char *));
        if (!hoistCollectIdents(g, ix, var, &ids)) continue;
        bool bad = false;
        for (size_t k = 0; k < ids.len; k++) {
            const char *one = *(const char **)vecAt(&ids, k);
            if (nameInVec(&writes, one) || nameInVec(&decls, one)) { bad = true; break; }
        }
        if (bad) continue;
        Affine af;
        if (!affineIn(g, ix, var, &af)) continue;
        HoistedIdx *h = vecPush(&picks);
        h->view = view;
        h->coef = af.coef;
        h->base = af.base;
        h->line = site->line;
        h->site = site;
    }
    if (picks.len == 0) return;

    /* First emit all the empty-view checks (with the same message as the checking primitive), then one
     * range check per deduplicated affine form. Wrapped in `k < hi`: when the trip range is empty the
     * loop body never executes and the check must not trap. */
    cgLine(g, "if ((int64_t)(%s) < (int64_t)(%s)) {", var, hi);
    g->indent++;
    for (size_t i = 0; i < picks.len; i++) {
        HoistedIdx *h = vecAt(&picks, i);
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++)
            if (strcmp(((HoistedIdx *)vecAt(&picks, k))->view, h->view) == 0) seen = true;
        if (!seen)
            cgLine(g, "if (!(%s).data) extc_trapMsg(\"%s\", %d, \"the view has no storage\");",
                   h->view, g->path, h->line);
    }
    for (size_t i = 0; i < picks.len; i++) {
        HoistedIdx *h = vecAt(&picks, i);
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++) {
            HoistedIdx *p = vecAt(&picks, k);
            if (strcmp(p->view, h->view) == 0 && strcmp(p->coef, h->coef) == 0
                && strcmp(p->base, h->base) == 0) seen = true;
        }
        if (seen) continue;
        const int id = g->tmpSeq++;
        cgLine(g, "int64_t __extc_rk%d_c = (int64_t)(%s);", id, h->coef);
        cgLine(g, "int64_t __extc_rk%d_b = (int64_t)(%s);", id, h->base);
        cgLine(g, "int64_t __extc_rk%d_i = __extc_rk%d_c * (int64_t)(%s) + __extc_rk%d_b;",
               id, id, var, id);
        cgLine(g, "int64_t __extc_rk%d_j = __extc_rk%d_c * ((int64_t)(%s) - 1) + __extc_rk%d_b;",
               id, id, hi, id);
        cgLine(g, "if (__extc_rk%d_i > __extc_rk%d_j) { int64_t __extc_rk%d_t = __extc_rk%d_i; "
                  "__extc_rk%d_i = __extc_rk%d_j; __extc_rk%d_j = __extc_rk%d_t; }",
               id, id, id, id, id, id, id, id);
        cgLine(g, "if (__extc_rk%d_i < 0 || __extc_rk%d_j >= (int64_t)(%s).len) "
                  "extc_trap(\"%s\", %d, __extc_rk%d_i < 0 ? __extc_rk%d_i : __extc_rk%d_j, "
                  "(int64_t)(%s).len);",
               id, id, h->view, g->path, h->line, id, id, id, h->view);
    }
    g->indent--;
    cgLine(g, "}");

    for (size_t i = 0; i < picks.len; i++)
        *(Expr **)vecPush(&g->hoistedIdx) = ((HoistedIdx *)vecAt(&picks, i))->site;
}

/** @brief Is this view-subscript site already covered by the entry check of some outer loop? */
static bool idxHoisted(CG *g, Expr *e) {
    if (!g || g->hoistedIdx.len == 0) return false;
    for (size_t i = 0; i < g->hoistedIdx.len; i++)
        if (*(Expr **)vecAt(&g->hoistedIdx, i) == e) return true;
    return false;
}


/**
 * @brief Emit `ST_WHILE`: an ordinary `while (...)` or a `while (1)` shape that takes a level of its
 *        own.
 *
 * Evaluating the condition can write temporaries into the statement prefix (the right operand of an
 * array `==`, a method receiver, or the subject of `??`). Flushing the prefix **before** the
 * `while (...)` would evaluate it once only, while the condition reads that temporary every round,
 * so `while a == mk()` would keep comparing the first value; when there is a prefix the condition is
 * therefore moved into the loop body, where the number of evaluations matches the source and the
 * body is unchanged. With no prefix (the vast majority) `while (cond) {` is kept.
 *
 * When the condition allocates space itself it takes a level of its own, released at the start of
 * every round; `g->blkLevel` is raised for the whole loop so that the condition and the body agree
 * with the level the checker assigned (`blkMaxLevel` counts one level more for this shape).
 *
 * A `for`'s step statement is recorded on the body block; while the step is still inside the body,
 * the loop gets a label id, `genBlockBody` emits the label and `ST_CONTINUE` jumps to it.
 *
 * @param g  The code generation state.
 * @param s  The `while` statement; its body must be a block.
 */
static void stWhile(CG *g, Stmt *s) {
const char *cnd = cgCond(g, genExpr(g, s->u.whiles.cond));
/* Generating the condition may have written temporaries to the statement prefix:
 * `eqOperand` materializes the right operand of an array `==` there, and a receiver or a
 * `??` subject can do the same. Flushing that prefix **in front of** `while (...)` runs it
 * once, while the condition reads the temporary on every round -- `while a == mk()` would
 * compare against the first call's value forever. When there is a prefix, put the condition
 * inside the loop instead: the same number of evaluations as the source, and the body is
 * untouched. No prefix (the overwhelmingly common case) keeps the plain `while (cond) {`
 * shape. */
const bool condTemps = g->prefix.len != 0;
/* A `for` loop records its step on the body block; give this loop a label id if that
 * step is still part of the body (the iterator retargeting clears it, see
 * `forRetargetToIterator`). `genBlockBody` emits the label, `ST_CONTINUE` jumps to it. */
Stmt *wbody = s->u.whiles.body;
int stepLabel = 0;
if (wbody && planForStep(wbody)) {
    for (size_t i = 0; i < wbody->u.block.stmts.len; i++)
        if (*(Stmt **)vecAt(&wbody->u.block.stmts, i) == planForStep(wbody)) { stepLabel = ++g->forStepSeq; break; }
}
/* A condition that **allocates** gets its own arena level, released at the top of every
 * round: the checker put its sites one scope deeper for exactly this. Without
 * it the condition's allocations lived until the enclosing block ended, so the loop grew
 * with the number of rounds. `g->blkLevel` is raised for the whole loop so the condition
 * and the body agree with what the checker assigned; the body block then sits one level
 * deeper than before, which is why `blkMaxLevel` counts an extra level for this shape. */
const bool ownLevel = planCondAllocs(s);
/* Hoist the view-subscript checks to loop entry. Only done in the shape where the condition and the
 * prefix are both "clean": the other path moves the condition into the body, where the check would
 * rerun every round and would also trap wrongly when the condition is false. */
const size_t hoistMark = g->hoistedIdx.len;
if (!condTemps && !ownLevel) loopHoistRangeFor(g, s);
if (condTemps || ownLevel) {
    cgLine(g, "while (1) {");
    g->indent++;
    if (ownLevel) g->blkLevel++;
    /* the condition's own level clears its arena every round; the reclamation region is left until
 * frame destruction (see `cgReleaseLevel`). */
    if (ownLevel) cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);
    flushPrefix(g);               /* the temporaries, now once per round */
    cgLine(g, "if (!(%s)) break;", cnd);
} else {
    cgLine(g, "while (%s) {", cnd);
    g->indent++;
}
g->loopLevel[g->loopLen] = g->blkLevel + 1;   /* the body is the next level */
g->loopStep[g->loopLen] = stepLabel;
g->loopStepUsed[g->loopLen] = false;
/* The reference slots live when the loop is entered: slots with a subscript smaller than this cell
 * are the long-lived slots "live before the loop starts" (criterion 2 of `rcEmitStoreRelease`). The
 * loop body's block has not been entered yet, so the root table is exactly the slots live outside
 * the loop. */
g->loopRootBase[g->loopLen] = (int)g->rcRoots.len;
g->loopLen++;
*(size_t *)vecPush(&g->dropLoopBases) = g->dropBlocks.len;
genBlockBody(g, s->u.whiles.body);
g->dropLoopBases.len--;
g->hoistedIdx.len = hoistMark;   /* restore the outer loop's site table: this loop's ranges hold only inside this loop */
g->loopLen--;
if (ownLevel) g->blkLevel--;
g->indent--;
cgLine(g, "}");
return;
}

/**
 * @brief Emit `ST_MATCH`: the arms become an if/else chain, binding payloads first when needed.
 *
 * The matched expression is evaluated once (with a payload-carrying enum it lands in a temporary
 * first), because the arm bodies read `.u.<variant>` and a second evaluation would repeat side
 * effects. Inside a generic instance the record's type is `subst`ed to the instance type first, or
 * the arms would compare a template type against a constant that does not exist.
 *
 * An if/else chain is used rather than a `switch` on the variant constant: a `break`/`continue` in
 * an arm body belongs to the enclosing loop, and inside a `switch` it would only leave the switch.
 * The trailing `else` appears only when the last arm is the `_` fallback arm; otherwise the checker
 * has already guaranteed the match is exhaustive.
 *
 * @param g  The code generation state.
 * @param s  The `match` statement.
 */
static void stMatch(CG *g, Stmt *s) {
/* The scrutinee is evaluated exactly once, into a temporary when
 * the enum has a payload, because an arm reads `.u.<variant>` and
 * evaluating the expression a second time - as in
 * `match f() { ... }` - would be wrong. */
/* `subst` matters here for the same reason it does on the construction side
 * (`EX_ENUMVAL`): inside the body of a generic instance the scrutinee's
 * recorded type is the *template's* type, so `et->name` would be `option_V` and the
 * arm would compare against `option_V_none`, a constant that does not exist.
 * Substituting first gives the instance's name (`option_i64`), and the payload types
 * below are then already concrete. */
Type *et = ttBase(subst(g, s->u.match.scrutinee->type));
bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);
/* A match on a constant unit variant (`match color.red`) compares the same
 * constant against itself in the arm that matches. The comparison is correct,
 * but tautological; emit `1` for that one arm so C compilers do not warn
 * about it (-Wtautological-compare) while the shape stays an if/else chain. */
Expr *scrut = s->u.match.scrutinee;
const char *scrutVar = (!payload && scrut->kind == EX_ENUMVAL &&
                        scrut->u.enumval.args.len == 0)
                           ? scrut->u.enumval.variant
                           : NULL;
const char *subj = genExpr(g, scrut);
if (payload) {
    const char *tmp = arenaPrintf(g->arena, "__extc_m%d", g->tmpSeq++);
    cgLine(g, "%s %s = %s;", cType(g, et), tmp, subj);
    subj = tmp;
}

/* The arms become an if/else chain, deliberately not a `switch`
 * over the variant constants: a `break` or `continue` in an arm
 * body belongs to the enclosing loop, but inside a `switch` it
 * would only leave the switch, so a `while true` with a `break`
 * would never terminate.
 *
 * A final `else` appears only when the last arm is the `_` catch-all;
 * otherwise the checker guarantees that the match is exhaustive, and a
 * missing variant would not compile at all. */
flushPrefix(g);
for (size_t i = 0; i < s->u.match.arms.len; i++) {
    MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
    if (arm->wildcard) {
        /* the `_` arm has no payload to bind: it is the trailing else of the if chain (when the first arm is
     * `_`, a block is emitted). */
        cgLine(g, "%s", i == 0 ? "{" : "} else {");
        g->indent++;
        genBlockBody(g, arm->body);
        g->indent--;
        continue;
    }
    if (scrutVar && strcmp(arm->variant, scrutVar) == 0) {
        cgLine(g, "%s (1) {", i == 0 ? "if" : "} else if");
    } else {
        cgLine(g, "%s (%s%s == %s_%s) {", i == 0 ? "if" : "} else if",
               subj, payload ? ".tag" : "", et ? et->name : "?", arm->variant);
    }
    g->indent++;
    /* Bind the payload: `circle(r) => ...` becomes
     * `double r = tmp.u.circle._0;`. */
    size_t firstBind = g->deadDefs.len;
    for (size_t k = 0; k < arm->binds.len; k++) {
        Variant *v = et && et->edef ? NULL : NULL;
        (void)v;
        Type *bt = NULL;
        if (et && et->edef) {
            for (size_t j = 0; j < et->edef->variants.len; j++) {
                Variant *vv = *(Variant **)vecAt(&et->edef->variants, j);
                if (strcmp(vv->name, arm->variant) != 0) continue;
                bt = *(Type **)vecAt(&vv->types, k);
                /* A generic enum instance substitutes its type
                 * arguments into the payload type, so `just(T)`
                 * with `T = slice<u8>` yields the instance type. */
                if (et->edef->typeParams.len > 0 &&
                    et->targs.len == et->edef->typeParams.len)
                    bt = ttSubstitute(g->tt, bt, &et->edef->typeParams, &et->targs);
                break;
            }
        }
        flushPrefix(g);              /* nothing pending may end up in the span */
        /* A payload binding is a C local **inside this arm**: its scope ends with the `if` block, and the
         * block-registered mechanism of the static root table cannot cover it. So when the bound type
         * contains a reference slot, this function is treated as having an incomplete root table and
         * emits no trigger at all (the safe direction). */
        if (g->rc.reclaim && bt && rcScanType(g, bt, 0) != RC_NO_REFS)
            g->rc.rootsUnreliable = true;
        size_t beforeBind = g->out->len;
        cgLine(g, "%s %s = %s.u.%s._%zu;", cType(g, bt), *(const char **)vecAt(&arm->binds, k),
               subj, arm->variant, k);
        /* A `_` in the payload list means "discard": the parser gives it a hidden `__extc_t<N>`
         * name so two discards in one arm do not collide. Nothing reads such a binding by
         * construction, and the generated unit is compiled with -Werror, so mark it read at the
         * declaration. The dead-binding pass removes the line only when it can place it in the
         * unit text; a discard binding taken out of a function that was itself assembled from a
         * scratch buffer cannot be located there, and this mark is what keeps that case clean. */
        const char *bn0 = *(const char **)vecAt(&arm->binds, k);
        if (bn0 && strncmp(bn0, "__extc_t", 8) == 0) cgLine(g, "(void)%s;", bn0);
        /* An arm that never reads its payload - `none => 0` written with a
         * binding it does not use, or an arm that only ignores the value it
         * matched - should not pay for the copy. The line is remembered, and
         * the arm body below says whether anything names it. */
        Buf bl;
        bufInit(&bl, g->arena);
        bufPutn(&bl, g->out->data + beforeBind, g->out->len - beforeBind);
        DeadDef *bd = arenaAllocZero(g->arena, sizeof *bd);
        bd->name = *(const char **)vecAt(&arm->binds, k);
        bd->text = bufCstr(&bl);
        bd->off  = beforeBind;
        bd->scoped = true;   /* counted in the arm body only */
        *(DeadDef **)vecPush(&g->deadDefs) = bd;
    }
    size_t bodyA = g->out->len;
    genBlockBody(g, arm->body);
    size_t bodyB = g->out->len;
    /* Only the bindings of *this* arm: a nested match's bindings already know their own
     * stage, and a larger scope here would count them as mentioned. */
    for (size_t k = firstBind; k < firstBind + arm->binds.len && k < g->deadDefs.len; k++) {
        DeadDef *bd = *(DeadDef **)vecAt(&g->deadDefs, k);
        bd->scopeA = bodyA;      /* what could read the binding: this arm body */
        bd->scopeB = bodyB;
    }
    g->indent--;
}
cgLine(g, "}");
return;
}

/**
 * @brief Is this assignment target the C slot of a **parameter** of the function being emitted?
 *
 * The reference-slot store path asks this before it decrements an old value (BUG-A / shape G12).
 * A parameter slot is registered in the root table at function entry
 * (`rcRootDeclImpl(..., isParam = true)`) but is never `initRef`'d, so the value it holds carries
 * **no holder count of its own**. The ordinary store's `-1` on the old value would then free an
 * object the caller still aliases -- `fn clobber(p: mut ref node) { p = new node }` followed by a
 * write through the caller's old alias lands in the reused slot (uaf_param_overwrite).
 *
 * Matching the generated C name is exact: the checker mangles a local that shadows a parameter
 * (`a` becomes `a__2`), so a target whose C name equals a parameter's C name can only be that
 * parameter. Coroutine frames are not parameters: their fields live in the frame struct and their
 * C spelling (`f->x`) cannot equal the bare parameter name.
 *
 * @param g  The generator (supplies `curFunc`).
 * @param t  The assignment target expression.
 * @return   True when `t` names a parameter slot of the current function.
 */
static bool cgTargetIsParamSlot(CG *g, Expr *t) {
    if (!g || !t || t->kind != EX_IDENT || !g->curFunc) return false;
    const char *nm = planCName(t);
    if (!nm) nm = t->u.ident.name;
    if (!nm) return false;
    const FuncDef *f = g->curFunc;
    for (size_t i = 0; i < f->params.len; i++) {
        const Param *p = *(Param **)vecAt((Vec *)&f->params, i);
        const char *pn = (p && p->cname) ? p->cname : (p ? p->name : NULL);
        if (pn && strcmp(pn, nm) == 0) return true;
    }
    return false;
}

/** @brief State of one "does this expression read that binding" walk. */
typedef struct { const char *name; bool found; } MentionScan;

/**
 * @brief The child callback of `mentionWalk`, forward-declared ahead of its first use.
 *
 * @param ctx  The `MentionScan *` the walk is running for.
 * @param e    The child expression being visited.
 * @return     `false` when this child holds the name, which stops the whole walk.
 */
static bool mentionChild(void *ctx, Expr *e);

/**
 * @brief One step of the "does this expression read that binding" walk.
 *
 * A `@drop` value that is overwritten may have its right side read the very place being
 * overwritten (`s = s.concat(t)`, `p = f(ref p)`): finding that read is what decides whether the
 * old value can be handed back before the right side runs. Returning false stops the walk at the
 * first hit, the convention `astWalkExprChildren` uses for every early exit.
 *
 * @param ms  The scan: the name looked for and whether it was found.
 * @param e   The expression to inspect, or NULL.
 * @return    `true` to keep walking; `false` once the name has been found.
 */
static bool mentionWalk(MentionScan *ms, Expr *e) {
    if (!e) return true;
    if (e->kind == EX_IDENT) {
        if (e->u.ident.name && strcmp(e->u.ident.name, ms->name) == 0) {
            ms->found = true;
            return false;
        }
        return true;
    }
    const AstVisit v = { mentionChild, NULL, ms };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief The child callback of `mentionWalk`; see that function for the question it answers.
 *
 * @param ctx  The `MentionScan *` the walk is running for.
 * @param e    The child expression being visited.
 * @return     `false` when this child holds the name, which stops the whole walk.
 */
static bool mentionChild(void *ctx, Expr *e) {
    return mentionWalk((MentionScan *)ctx, e);
}

/**
 * @brief Does `e` read a binding by this source name anywhere in it?
 *
 * Used by the assignment path: overwriting a binding hands the old value back with its `@drop`
 * hook, but the right side may read the very place that is being overwritten (`s = s.concat(t)`,
 * `p = f(ref p)`). In that case the old value is handed back only after the right side has been
 * evaluated and materialised, so the hook cannot free storage the expression still reads.
 *
 * The walk goes through `astWalkExprChildren`, the one place that lists an expression's children,
 * rather than a private `switch`: a kind added later is then followed here automatically instead
 * of being silently skipped.
 *
 * @param e     The expression to scan, or NULL.
 * @param name  The source name of the binding being overwritten.
 * @return      True when a read of that binding may appear inside `e`.
 */
static bool exprMentionsName(const Expr *e, const char *name) {
    if (!e || !name) return false;
    MentionScan ms = { name, false };
    mentionWalk(&ms, (Expr *)e);
    return ms.found;
}
/**
 * @brief Emit the statement itself; this is the real dispatch table, while prefix bookkeeping lives
 *        in `genStmt`.
 *
 * `ST_VAR`, `ST_WHILE` and `ST_MATCH` are emitted by `stVar`, `stWhile` and `stMatch` respectively,
 * which is what makes this table read like a table.
 *
 * @param g  The code generation state.
 * @param s  The statement to emit.
 */
static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_TRAP: {
            /* `trap(msg)`: **the same exit** -- the traps the compiler inserts itself go through it too.
             * `extc_trapMsg` prints `file:line: trap: message` to stderr and then calls `extc_die(1)`
             * (the dying hooks still run => buffered output is not lost). So "the program is broken"
             * has exactly one spelling in this language, whether the compiler says it or the author
             * does. */
            Expr *m = s->u.trap_.msg;
            /* Nothing from `rt`/`rtPrint` here: `extc_trapMsg` and `extc_die` are primitives of
             * the runtime block, and `trap` prints no structured payload. */
            if (m->kind == EX_STR) {
                /* literal: take the short path, with the message as an **argument** rather than a format string
     * (so a `%` in a user's message is safe) */
                cgLine(g, "extc_trapMsg(\"%s\", %d, \"%s\");", g->path, s->line, m->u.str.text);
                return;
            }
            Type *mt = m->type;
            if (mt && mt->kind == TY_REF) {
                cgLine(g, "extc_trapMsg(\"%s\", %d, (const char *)(%s));",
                       g->path, s->line, genExpr(g, m));
                return;
            }
            /* run-time slice: evaluate once (it may have side effects), then print a slice as long as its
     * length */
            const char *t = arenaPrintf(g->arena, "__extc_trap%d", g->tmpSeq++);
            cgLine(g, "%s %s = %s;", cType(g, mt), t, genExpr(g, m));
            cgLine(g, "fprintf(stderr, \"%%s:%%d: trap: %%.*s\\n\", \"%s\", %d,"
                      " (int)(%s).len, (const char *)(%s).data);",
                   g->path, s->line, t, t);
            cgLine(g, "extc_die(1);");
            return;
        }
        case ST_DROP: {
            /* `drop x`: run the hook **here**, and remember the name so the block tail does not run it
             * a second time. The checker has already rejected a second drop and any later use. */
            Expr *target = s->u.drop_.target;
            const char *nm = planCName(target);
            if (!nm && target->kind == EX_IDENT) nm = target->u.ident.name;
            if (nm && target->type) genDropValue(g, dropValueRef(g, nm), target->type, 0);
            if (nm) {
                *(const char **)vecPush(&g->dropExplicit) = nm;
                if (target->kind == EX_IDENT)
                    *(const char **)vecPush(&g->dropExplicit) = target->u.ident.name;
            }
            return;
        }
        case ST_DOMAIN: {
            /* `d.run { ... }`: the block ends by **running its tasks to completion** -- that is what
             * makes the block structured (nothing it started is still unfinished when it exits).
             * The domain itself is not freed here: the caller owns it (`single()` handed it over) and
             * may run another block on it; its lifetime is its own step. */
            Expr *recv = (s->u.domain_.callee && s->u.domain_.callee->kind == EX_FIELD)
                             ? s->u.domain_.callee->u.field.obj : NULL;
            if (!recv) {
                ctxError(g->ctx, s->line, 1, NULL, "domain block: no receiver to drive");
                return;
            }
            /* The block's own statements come **first**: they are what registers the tasks
             * (`ext f(x)` => `extc_dom_add`) and what the caller's locals hold. Only then does the
             * domain drive them to completion -- "the block cannot end while a task it started is
             * still unfinished" is exactly this order, and it is also why the drive call belongs
             * **inside** the block: each `ext` declares its frame as a local of this block
             * (`EX_EXT`), so running after the closing brace would hand the domain pointers to
             * stack slots that are already out of scope. */
            Stmt *body = s->u.domain_.body;
            /* The surface syntax is always a braced block (`d.run { ... }`), and the checker keeps
             * that shape, so `body` is a block here. */
            EXTC_DBG_ASSERT(body != NULL && body->kind == ST_BLOCK);
            if (body && body->kind == ST_BLOCK) {
                cgLine(g, "{");
                g->indent++;
                /* The `ext f(x)` frames of this block registered their task ids with the
                 * collector. The registration must stay until the drive call returns, but the
                 * block's release text is emitted with the body (genBlockBody) *before*
                 * `extc_dom_run`: `domBodyLevel` tells that release to defer the pop, which is
                 * emitted at the lines right after the drive. */
                int bodyLevel = g->blkLevel + 1;
                int savedDomBody = g->domBodyLevel;
                g->domBodyLevel = bodyLevel;
                genBlockBody(g, body);
                g->domBodyLevel = savedDomBody;
                const char *dv = genExpr(g, recv);
                g->needDomain = true;
                flushPrefix(g);
                cgLine(g, "extc_dom_run(%s);", dv);
                cgTaskRootPopAt(g, bodyLevel, false, true);
                g->indent--;
                cgLine(g, "}");
                return;
            }
            genStmt(g, body);
            const char *dv = genExpr(g, recv);
            g->needDomain = true;
            flushPrefix(g);
            cgLine(g, "extc_dom_run(%s);", dv);
            return;
        }
        case ST_VAR: stVar(g, s); noteOwnedCoro(g, s); return;

        case ST_ASSIGN:
            /* An assignment whose value is a `?` reads the payload; the failure
             * path has already returned by then. */
            if (s->u.assign.value && s->u.assign.value->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.assign.value);
                g->inAssignTarget = true;
                const char *at = genExpr(g, s->u.assign.target);
                g->inAssignTarget = false;
                flushPrefix(g);
                cgLine(g, "%s = %s;", at, tryPayloadPath(g, &ti));
                return;
            }
            /* Overwriting a binding that owns its state runs its `@drop` hook first: the old value
             * is going away, and this is the last moment it can hand that state back. Skipped for
             * `a = a`, where dropping first would release the very storage being read. */
            /* The old value of an overwritten binding is handed back with its `@drop` hook, but
             * not before the right side has been read: `s = s.concat(t)` reads the very place being
             * overwritten, so the hook has to run **after** the value exists (see below). These three
             * flags carry that decision across the value evaluation. */
            bool dropOld = false;
            bool oldReadByValue = false;
            const char *dropCName = NULL;
            Type *dropType = NULL;
            if (s->u.assign.target && s->u.assign.target->kind == EX_IDENT) {
                struct { bool same; } guard = { false };
                if (s->u.assign.value && s->u.assign.value->kind == EX_IDENT)
                    guard.same = strcmp(s->u.assign.value->u.ident.name,
                                        s->u.assign.target->u.ident.name) == 0;
                const char *hn = planCName(s->u.assign.target);
                const char *src0 = s->u.assign.target->u.ident.name;
                /* A place that an explicit `drop` already emptied holds nothing to hand back, and the
                 * assignment makes it a live place again -- so the name leaves the "already dropped"
                 * list here. */
                bool wasDropped = false;
                if (src0) {
                    size_t keep = g->dropExplicit.len;
                    for (size_t k = 0; k < g->dropExplicit.len; k++) {
                        const char *n = *(const char **)vecAt(&g->dropExplicit, k);
                        if (n && strcmp(n, src0) == 0) { wasDropped = true; continue; }
                        *(const char **)vecAt(&g->dropExplicit, keep++) = n;
                    }
                    g->dropExplicit.len = keep;
                }
                if (!guard.same && !wasDropped && hn &&
                    typeNeedsDrop(g, s->u.assign.target->type, 0)) {
                    dropOld = true;
                    dropCName = hn;
                    dropType = s->u.assign.target->type;
                    oldReadByValue = exprMentionsName(s->u.assign.value, src0);
                    if (!oldReadByValue)
                        genDropValue(g, dropValueRef(g, hn), s->u.assign.target->type, 0);
                }
            }
            g->inAssignTarget = true;
            const char *tgt = genExpr(g, s->u.assign.target);
            g->inAssignTarget = false;
            /* Storing into a **view element** whose element type owns storage: the slot may hold a
             * live element, and the store overwrites its bytes without running any hook, so the old
             * element is handed back first -- the same rule as overwriting a binding above, at a
             * place that is an index instead of a name. The address is taken once so an index
             * expression with a side effect is still evaluated once. Slots at and beyond the
             * container's length are zero (the container zeroes what `pop`/`clear`/`shrink`
             * take out of it), and a hook is a no-op on a zero value, so writing the next element
             * of a growing buffer releases nothing. A value that is **moved** in gives its
             * ownership up here, so its own block exit must not run the hook again. */
            if (!s->u.assign.op && s->u.assign.target &&
                s->u.assign.target->kind == EX_INDEX && s->u.assign.target->type &&
                typeNeedsDrop(g, s->u.assign.target->type, 0)) {
                g->pfMode = 1;
                pfLine(g, "{ __typeof__(%s) *__extc_ov = &(%s);", tgt, tgt);
                genDropValue(g, "(*__extc_ov)", s->u.assign.target->type, 0);
                pfLine(g, "}");
                g->pfMode = 0;
                const char *mn = plainIdentCName(g, s->u.assign.value);
                if (mn && !planArgClone(s->u.assign.value)) noteMoved(g, mn);
            }
            /* A `@value` target stores an **independent** value: the right side is cloned when it is a
             * place, exactly the way a by-value argument is (the checker marks the site with
             * `planArgClone`, the same bit the argument path uses -- one semantic, one bit). A
             * temporary is a move and is stored as it is. */
            const char *cl = (planArgClone(s->u.assign.value) && s->u.assign.target)
                             ? cloneValueText(g, s->u.assign.target->type, s->u.assign.value) : NULL;
            const char *val = cl ? cl : genTopExpr(g, s->u.assign.value);
            /* The right side reads the binding being overwritten, so the old value is handed back
             * only now, after the new value exists -- and it is materialised first, because the hook
             * may release the storage that the freshly computed value still points into. The
             * temporary is a plain C variable: no hook runs on it, and the store below gives its
             * ownership to the target, so nothing is released twice. */
            if (dropOld && oldReadByValue) {
                const char *tmp = arenaPrintf(g->arena, "__extc_ov%d", g->tmpSeq++);
                flushPrefix(g);
                cgLine(g, "__typeof__(%s) %s = %s;", dropCName, tmp, val);
                genDropValue(g, dropValueRef(g, dropCName), dropType, 0);
                val = tmp;
            }
            /* Assigning a reference slot has **overwrite** semantics (the slot already holds an old value), so
             * it goes through storeRef: decrement the old value, increment the new one. A data field
             * (`p.val = x`) is not a reference type => not a word is emitted.
             *
             * A cross-region edge (`a->f = v` where `a` and `v` are not in the same region) **must be
             * registered**: the target region's sweep seeds from roots plus registration bits, and a
             * heap-to-heap cross-region in-edge has nowhere else to come from. Clearing the
             * registration bit is the source side's job (`rscnt` counting plus withdrawal when the
             * source object or source region dies), so it is **not sticky** -- setting without
             * clearing would make the object created each round of a loop a seed forever (memory
             * grows linearly with the round count).
             *
             * The third argument is "the object holding this slot": `a->f` passes `a`, `*p` passes
             * `p`, and a local or global slot passes nothing (those are roots covered by the trigger
             * point's root table). The object expression must be a **pure place**, or it is evaluated
             * twice at the same spot (`f()->f = v` would call `f()` twice). */
            const bool refStore = g->rc.reclaim && !s->u.assign.op
                                  && cgIsRefType(g, s->u.assign.target->type);
            /* A **whole-value** store (a struct or fixed-size array copied by value): the target slot is not a
             * reference type itself, but its payload can hold reference leaves
             * (`s.w = wrap { n: keeper }`). Such an assignment cannot take the bare C whole-block copy
             * -- that writes the slot without `inc` and without cross-region registration (the same
             * class of "the slot was written but not accounted", reached through another mechanism).
             * The three states match a reference slot: enumerable => one
             * `storeRefOwned`/`storeRefSlot` per leaf; not enumerable (enum payloads, a generic
             * argument carrying a reference) => the conservative per-word path (increment only, plus
             * registration or dropping out of sweeping). */
            Type *rawTT = s->u.assign.target ? s->u.assign.target->type : NULL;
            const RcScan valScan = (g->rc.reclaim && !s->u.assign.op && !refStore && rawTT)
                                       ? rcScanType(g, rawTT, 0) : RC_NO_REFS;
            const bool valueStore = valScan != RC_NO_REFS;
            const char *owner = "0";
            bool hostUnknown = false;
            bool hostProven = false;   /* the host is an object the **region was traced to** (not a local or static slot) */
            Expr *hostBase = NULL;     /* the host object expression: the store point uses it to decide "the region can be given
     * statically" */
            if ((refStore || valueStore) && s->u.assign.target) {
                Expr *t = s->u.assign.target;
                /* The three outcomes for the host (see `cgSlotHost`): a pure place base => register the
                 * cross-region edge; statically declared storage => the root table covers it; not
                 * traceable => the conservative path. In `c.a = v` (`c` is a struct **value**) the slot
                 * belongs to a local or the arena, not to a slot of this run time; if the container of
                 * `a[i]` is an inline array (`w.arr[i]`) the host is `w`, while in a view it cannot be
                 * traced. */
                Expr *base = NULL;
                RcHost h = cgSlotHost(g, t, &base);
                hostBase = base;
                if (h == RC_HOST_OBJ && base && cgPurePlace(g, base)) {
                    owner = genExpr(g, base);
                    hostProven = true;
                } else if (h != RC_HOST_LOCAL) {
                    hostUnknown = true;
                }
            }
            flushPrefix(g);
            /* `x += y` is emitted as `x += y`: C evaluates the target **once**, which is
             * exactly the meaning the statement has, and it is what a reader expects to
             * find. A user-defined operator is a method, so that one case becomes the plain
             * assignment of its call - the checker has already required a target with no
             * call in it, because the call names the target a second time. */
            if (s->u.assign.op) {
                if (planCompoundCall(s) && s->u.assign.opExpr)
                    cgLine(g, "%s;", genBin(g, s->u.assign.opExpr));
                else if (s->u.assign.opExpr && planCallee(s->u.assign.opExpr))
                    cgLine(g, "%s = %s;", tgt, genBin(g, s->u.assign.opExpr));
                else
                    cgLine(g, "%s %s %s;", tgt, s->u.assign.op, val);
                return;
            }
            if (refStore) {
                dbgRcSite(g, g->blkLevel, 0);
                if (hostUnknown) {
                    /* The host expression cannot be proved (`(*id(s)).kids[0] = v`, a write to a view element): this
                     * slot must still be a seed, but the static root table does not hold it and the source
                     * side has nothing to withdraw either. The **slot address** itself usually still
                     * carries the answer -- a slot falling inside an rc slot tells which slab (hence
                     * which region) holds it, so the accounting proceeds as if the host were known
                     * (cross-region registration and old-value withdrawal both happen). Only when the
                     * slot is not in an rc slab either (view data, C or pool buffers) does the
                     * conservative direction apply: mark **the region the target lives in** as not
                     * swept, and from then on the region is reclaimed only by the count reaching
                     * zero. */
                    cgLine(g, "extc_rc_storeRefSlot((void **)&(%s), (void *)(%s));", tgt, val);
                    return;
                }
                /* BUG-A (G12): a **parameter slot** overwritten in the counting tier. The slot is
                 * a root at function entry (`rcRootDeclImpl(..., isParam = true)`), so it was never
                 * `initRef`'d -- the old value has no holder count of its own, and the ordinary
                 * store's `-1` on the old value would free an object the caller still aliases.
                 * Take the insert-only direction instead: `initRef` writes the slot and increments
                 * the **new** value, while the old value keeps its count. At worst one reference is
                 * reclaimed later (the conservative side of soundness); doing nothing would free
                 * early. That leak is bounded per *forced event tier* run only: one reference per
                 * overwritten parameter slot (~20 B/iteration in the G12 loop shape), while
                 * the default sweep tiers (TIER 2/3) reclaim the dead object by sweeping, so memory
                 * stays bounded there. The only unbounded shape is a forced event-tier run that
                 * overwrites a parameter slot with a fresh escaping object; the default tiers are bounded.
                 * The no-count tier does not take this path: there the ordinary store is
                 * `storeRefLocal` (a plain write, no `-1`), so the shape is already safe and its
                 * output must stay byte-identical. */
                if (!g->rc.noCount && cgTargetIsParamSlot(g, s->u.assign.target)) {
                    cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", tgt, val);
                    return;
                }
                const char *hreg = rcFreshHostRegion(g, hostBase);
                if (hreg) {
                    /* The host is a **fresh local of this frame** (its arena level is known while this line is
                     * generated). Check whether the value is the same: if so both are in one region =>
                     * this edge cannot be cross-region => registration, the region comparison and both
                     * `ownSlab` lookups can all be skipped (`EXTC_RC_DBG_LEVEL=1` prints the decision).
                     *
                     * Only done in the no-count tier: with counting on, `storeRefOwnedReg` still needs
                     * those two lookups for the holder count. A wrong decision means "missed
                     * registration => dangling"; the guardrail is the cross-region registration count in
                     * the shape ratchet (`gate-reclaim-switch`'s cross-region shape turns red the moment
                     * a registration is missed). */
                    int hlvl = 0, vlvl = -2;          /* -2 = the value is not a fresh local */
                    if (hostBase && hostBase->kind == EX_IDENT) {
                        const char *hn = planCName(hostBase) ? planCName(hostBase)
                                                             : hostBase->u.ident.name;
                        for (int i = 0; i < g->rcFreshN; i++)
                            if (hn && strcmp(g->rcFreshName[i], hn) == 0) hlvl = g->rcFreshLvl[i];
                    }
                    Expr *ve = s->u.assign.value;
                    if (ve && ve->kind == EX_IDENT) {
                        const char *vn = planCName(ve) ? planCName(ve) : ve->u.ident.name;
                        for (int i = 0; i < g->rcFreshN; i++)
                            if (vn && strcmp(g->rcFreshName[i], vn) == 0) vlvl = g->rcFreshLvl[i];
                    }
                    if (getenv("EXTC_RC_DBG_LEVEL")) {
                        int plvl = planArenaLevel(ve);
                        fprintf(stderr, "[rclevel] host=%s hlvl=%d planLvl=%d eqPlan=%d freshLvl=%d eqFresh=%d\n",
                                hreg, hlvl, plvl, hlvl == plvl, vlvl, vlvl == hlvl);
                    }
                    if (g->rc.noCount && vlvl != -2 && vlvl == hlvl)
                        cgLine(g, "extc_rc_storeRefSameRegion((void **)&(%s), (void *)(%s));", tgt, val);
                    else
                        cgLine(g, "extc_rc_storeRefOwnedReg((void **)&(%s), (void *)(%s), %s);",
                               tgt, val, hreg);
                    return;
                }
                /* Overwriting a reference slot of this frame (`hostBase == NULL`: the slot of a local or
                 * parameter, not the field of a heap object): whether it is worth a point-fix release is
                 * left to the four criteria of `rcEmitStoreRelease`; if none applies, the usual path is
                 * taken. */
                if (hostBase == NULL && s->u.assign.target &&
                    s->u.assign.target->kind == EX_IDENT) {
                    const char *atn = planCName(s->u.assign.target)
                                          ? planCName(s->u.assign.target)
                                          : s->u.assign.target->u.ident.name;
                    if (rcEmitStoreRelease(g, tgt, atn, val, s->u.assign.value)) return;
                }
                /* A local or parameter slot (`hostBase == NULL` => `owner` is `"0"`): in the no-count tier the
                 * gated block of `storeRefOwned` does nothing for `so == 0` => emit the write-only entry
                 * point. Placed after `rcEmitStoreRelease`: the point-fix release path goes first,
                 * semantics unchanged. */
                if (g->rcLocalStore && g->rc.noCount && hostBase == NULL) {
                    cgLine(g, "extc_rc_storeRefLocal((void **)&(%s), (void *)(%s));", tgt, val);
                    return;
                }
                cgLine(g, "extc_rc_storeRefOwned((void **)&(%s), (void *)(%s), (void *)(%s));",
                       tgt, val, owner);
                return;
            }
            if (valueStore) {
                Type *tt = subst(g, rawTT);
                const char *ct = cType(g, tt);
                const int id = g->tmpSeq++;
                const char *dv = arenaPrintf(g->arena, "__extc_svd%d", id);
                const char *sv = arenaPrintf(g->arena, "__extc_svv%d", id);
                if (valScan == RC_CONSERVATIVE) {
                    /* The leaves cannot be enumerated: copy the whole block first (semantics), then count each word
                     * that is a live slot once, and register it on the target region or make that region
                     * drop out of sweeping (see `extc_rc_storeStructCons`). Only a host whose region was
                     * **proved** goes through registration; the local or static slot tier has no source
                     * to withdraw from, and a local of a conservative type cannot even be enumerated by
                     * the root table => take the conservative side (the target region drops out of
                     * sweeping). */
                    cgLine(g, "{ %s *%s = &(%s); %s %s = %s; *%s = %s;"
                              " extc_rc_storeStructCons((void *)%s, sizeof *%s, (void *)(%s), %d); }",
                           ct, dv, tgt, ct, sv, val, dv, sv, dv, dv, owner, hostProven ? 1 : 0);
                    return;
                }
                /* Enumerable: evaluate the target and the value once each (`genExpr` runs only once even with side
                 * effects), then apply the overwrite semantics of a reference slot leaf by leaf, and
                 * finally copy the whole block to fill in the non-reference fields. */
                cgLine(g, "{ %s *%s = &(%s); %s %s = %s;", ct, dv, tgt, ct, sv, val);
                cgValueRefStore(g, tt, arenaPrintf(g->arena, "(*%s)", dv), sv,
                                false, owner, hostUnknown);
                cgLine(g, "*%s = %s; }", dv, sv);
                return;
            }
            cgLine(g, "%s = %s;", tgt, asF32(g, s->u.assign.target ? s->u.assign.target->type : NULL,
                                              s->u.assign.value, val));
            return;

        case ST_IF: {
            const char *cnd = cgCond(g, genExpr(g, s->u.ifs.cond));
            flushPrefix(g);
            cgLine(g, "if (%s) {", cnd);
            g->indent++;
            genBlockBody(g, s->u.ifs.thenBody);
            g->indent--;
            if (!s->u.ifs.elseBody) {
                cgLine(g, "}");
                return;
            }
            cgLine(g, "} else {");
            g->indent++;
            if (s->u.ifs.elseBody->kind == ST_BLOCK) genBlockBody(g, s->u.ifs.elseBody);
            else                                     genStmt(g, s->u.ifs.elseBody);
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_WHILE: stWhile(g, s); return;

        case ST_YIELD: {
            /* `yield e`: store the return slot and the pc, then return. The `case` label right after
             * the `return` is where a resume jumps back in -- a label inside whatever block the
             * `yield` sits in, which is legal C11 (the shape is Duff's device). */
            const int k = ++g->coroYieldSeq;
            const char *v = s->u.yield_.value ? genExpr(g, s->u.yield_.value) : "0";
            flushPrefix(g);
            cgLine(g, "%s->ret = %s;", g->coroFrame, v);
            cgLine(g, "%s->pc = %d;", g->coroFrame, k);
            cgLine(g, "return true;");
            cgLine(g, "case %d: ;", k);
            if (s->u.yield_.bindCName)      /* `var x = yield e`: the resume's value lands here */
                cgLine(g, "%s->%s = %s->in;", g->coroFrame, s->u.yield_.bindCName, g->coroFrame);
            return;
        }
        case ST_RETURN: {
            /* Every return goes through the shared epilogue: only
             * `__extc_ret_v = ...; goto __extc_ret;` is emitted here, and the
             * release sequence appears once, at the end of the function. */
            if (!s->u.ret.value) {
                if (g->coroFunc) {
                    /* Finishing early: a pc no `case` matches, so the next resume falls through to
                     * the end of the step and reports "done". */
                    flushPrefix(g);
                    cgCoroArenaDestroy(g);      /* finished here: give the frame arenas back */
                    cgLine(g, "%s->pc = -1;", g->coroFrame);
                    cgLine(g, "return false;");
                    return;
                }
                cgReturn(g, NULL);           /* shared epilogue */
                return;
            }
            if (s->u.ret.value->kind == EX_TRY) {
                /* `return e?`: on success the payload is wrapped in the
                 * enclosing return type. */
                TryInfo ti = genTryHead(g, s->u.ret.value);
                Type *rt = subst(g, g->retType);
                Buf rb;
                bufInit(&rb, g->arena);
                bufPrintf(&rb, "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s } }",
                          cType(g, rt), rt->name, ti.okVar, ti.okVar,
                          tryPayloadPath(g, &ti));
                cgReturn(g, bufCstr(&rb));
                return;
            }
            /* A returned place of an `@value` type is a **copy the callee builds**: the caller
             * owns what comes back, and the local the place names is dropped on the way out, so a
             * bit copy would leave the caller holding released storage. The checker marked it. */
            const char *cl = planRetClone(s) ? cloneValueText(g, g->retType, s->u.ret.value) : NULL;
            const char *v = cl ? cl : genTopExpr(g, s->u.ret.value);
            flushPrefix(g);              /* must precede the return */
            /* A plain local handed back is **moved out**: it is dead on the way out, so marking it
             * here keeps `cgReturn`'s drop sweep from releasing the state the caller now owns.
             * The name recorded is the source spelling; the sweep compares that too, because
             * inside a generic instance the generated name carries the instance suffix. */
            g->dropSkipName = (!cl && s->u.ret.value->kind == EX_IDENT) ? s->u.ret.value->u.ident.name : NULL;
            cgReturn(g, v);
            g->dropSkipName = NULL;
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            /* `break` and `continue` leave these block levels behind, so the
             * arenas are released first, down to the level of the loop body.
             * The body's own arena is cleared when the next iteration enters
             * it. */
            /* Locals of the blocks being left run their hooks first: the hook may still need the
             * arena the slot lives in, which is released two lines down. */
            if (g->dropLoopBases.len > 0) genDropBlocksDownTo(g, *(size_t *)vecAt(&g->dropLoopBases,
                                                                                g->dropLoopBases.len - 1));
            int to = g->loopLen ? g->loopLevel[g->loopLen - 1] : 1;
            for (int lv = g->blkLevel; lv >= to; lv--) cgReleaseLevel(g, lv);
            if (s->kind == ST_BREAK) { cgLine(g, "break;"); return; }
            /* `continue` in a `for` must still run the step (C's `for` does); the desugared
             * `while` has it at the end of the body, behind a label. */
            int step = g->loopLen ? g->loopStep[g->loopLen - 1] : 0;
            if (step) {
                g->loopStepUsed[g->loopLen - 1] = true;
                cgLine(g, "goto __extc_forstep_%d;", step);
            }
            else      cgLine(g, "continue;");
            return;
        }

        case ST_EXPR:
            /* `f()?` as a statement of its own needs only the two lines that
             * evaluate it once and return on failure; nothing reads the
             * payload. */
            if (s->u.expr.expr->kind == EX_TRY) {
                (void)genTryHead(g, s->u.expr.expr);
                return;
            }
            const char *ex = genTopExpr(g, s->u.expr.expr);
            flushPrefix(g);
            /* A discarded **unwrap** (`f()!` as a statement) is the one shape that needs `(void)`:
             * it lowers to a union field read (`(g()).u.success._0;`), which gcc reports as
             * `unused-value` -- an error under `-Werror`. The parser builds `x!` as an `EX_SIGN`
             * node, the same node the prefix `!x` uses, distinguished only by position; here the
             * **type** tells them apart: a negation always yields `bool`, an unwrap never does.
             * Wrapping every discarded value instead would rewrite most generated files for
             * nothing. */
            Type *xt = s->u.expr.expr->type;
            bool isBool = xt && xt->kind == TY_BUILTIN && xt->name && strcmp(xt->name, "bool") == 0;
            bool unwrap = s->u.expr.expr->kind == EX_SIGN && xt && xt->kind != TY_VOID && !isBool;
            cgLine(g, unwrap ? "(void)(%s);" : "%s;", ex);
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_MATCH: stMatch(g, s); return;
    }
}

