/* ==================== Region reclamation: whether this pass emits at all ====================
 *
 * Reclamation governs heap storage at exactly three creation points, all inside codegen:
 *
 *   - `new T` (single value or fixed-size `new [N]T`) -- the branch of `exNew` without a length;
 *   - `alloc<T>(n)` -- the arena branch of `exGencall`;
 *   - a **single-value** `@overwrite` cell -- that branch also goes through `new T`, so its
 *     initializer is the first case above.
 *
 * A `new T[n]` without a length stays in the arena (a view is not a reference slot), and so does a
 * variable-length `@overwrite` cell, so neither counts. `@overwrite` needs no separate criterion: its
 * initializer is `new T`, and walking the expression sees it.
 *
 * This pass scans the function bodies that are **about to be emitted** (`g->funcs`, already pruned by
 * `collectEmitted`) plus the method bodies of generic instances plus global initializers, so "appears
 * in the program" means **appears in the product**: a program whose `new` exists only in dead functions
 * carries no runtime. Erring the other way is harmless too (it only carries a runtime block nobody
 * calls) -- the criterion prefers to be wide.
 */

/** @brief Statement callback: forwards to the generic expression/statement walk (declared first
 * because `rcAllocInExpr`'s visitor references it). */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s);

/** @brief Expression callback: returns false and stops the whole walk when a "reclamation-governed
 * allocation point" is hit. */
static bool rcAllocInExpr(void *ctx, Expr *e) {
    (void)ctx;
    if (!e) return true;
    if (e->kind == EX_NEW) {
        if (!e->u.new_.count) return false;   /* a single object is an rc slot */
        /* A block of them is an rc slot only where `exNew` puts it there: the function-body level.
         * Elsewhere the arena's release point (an inner block, or the home arena the caller picked)
         * already bounds it, so it is not a reclamation-governed allocation. */
        return planArenaLevel(e) != 1;
    }
    if (e->kind == EX_GENCALL)
        return !(e->u.gencall.name && strcmp(e->u.gencall.name, "alloc") == 0);
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkExprChildren(e, &v);
}

/**
 * @brief Does a function body contain a reclamation-governed allocation point?
 *
 * @param body  The function body; false when it is NULL (an extern, for instance).
 * @return      true when one is present.
 */
static bool rcBodyHasGovernedAlloc(Stmt *body) {
    if (!body) return false;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return !astWalkStmtChildren(body, &v);
}

/** @brief Statement callback: walks every child of the statement
 * (`rcBodyHasGovernedAlloc`'s visitor). */
static bool rcAllocInStmtProbe(void *ctx, Stmt *s) {
    (void)ctx;
    AstVisit v = { rcAllocInExpr, rcAllocInStmtProbe, NULL };
    return astWalkStmtChildren(s, &v);
}

