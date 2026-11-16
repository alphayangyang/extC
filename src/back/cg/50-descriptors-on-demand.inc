/* ------------------------------------------------------- descriptors on demand
 *
 * Only a type that is really printed gets a descriptor. In the synthetic stress
 * program with N=1000 no struct was printed at all, and the descriptor pool
 * came out completely empty.
 *
 * The roots are the argument types of the `extc_print(&x, &x_desc)` calls that
 * genPrint emits, registered through `descRef`; the closure follows the
 * references between descriptors - struct fields, array elements, slice
 * elements - until it reaches a fixed point.
 *
 * Two things this has to get right:
 *   1. Order. Which descriptors are needed is known only after the function
 *      bodies are generated, because `genPrint` runs inside them, while C wants
 *      definitions before uses. The pool is therefore written after the
 *      prototypes and before the bodies, and spliced back in at the end, the
 *      same way the slice helpers are.
 *   2. Cycles. `struct s { xs: slice<s> }` gives `s_desc -> slice_s_desc ->
 *      s_desc`. Every descriptor is therefore forward-declared first, with a C
 *      tentative definition, which makes the order of the definitions
 *      irrelevant. Collecting that set needs a dry pass first; it has no side
 *      effects, because the descriptor emitters write only to `g->out` and to
 *      the arena.
 */
static bool eqNeeded(CG *g, Type *t);
static void closeEqNeeds(CG *g);
static FuncDef *findOpMethod(TypeTable *tt, Type *t, const char *sym, Type *rhs, const char *fallback);
static void genEqAdapter(CG *g, Type *t, FuncDef *m);

/**
 * @brief Emit the definition of every registered descriptor.
 *
 * Re-reading `len` in the loop condition is deliberate: `descRef` appends new dependencies while
 * definitions are being written.
 *
 * @param g  The generator; reads `g->descs` and writes the definitions into `g->out`.
 */
static void emitDescDefs(CG *g) {
    for (size_t i = 0; i < g->descs.len; i++) {
        Type *t = *(Type **)vecAt(&g->descs, i);
        /* Only a struct that `extc_eq` really recurses into fills this slot; others stay NULL. */
        const char *eqFn = eqNeeded(g, t) ? arenaPrintf(g->arena, "%s_eqD", t->name)
                                          : NULL;
        if (t->kind == TY_STRUCT && t->sdef) {
            genStructDesc(g, t->name, t->name, t->sdef, eqFn);
        } else if (t->kind == TY_ENUM && t->edef) {
            genEnumDesc(g, t->name, t->name, t->edef);
        } else if (t->kind == TY_ARRAY) {
            genArrayDesc(g, t);
        } else if (t->kind == TY_GENERIC && t->sdef) {
            SubCtx sv = substEnter(g, t);  /* T in the fields stands for the type arguments */
            if (isView(t)) genViewDesc(g, t);
            else           genStructDesc(g, t->name, t->sdef->name, t->sdef, eqFn);
            substLeave(g, sv);
        } else {
            /* A missing case must fail loudly: silently emitting no descriptor
             * would leave the generated C referring to a symbol that does not
             * exist. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: no descriptor generator for this type");
        }
    }
}

/**
 * @brief Test whether a type needs an `eq` generated or filled in.
 *
 * Looked up by C name.
 *
 * @param g  The generator; the lookup table is `g->eqNeed`.
 * @param t  The type; `false` when it is `NULL` or has no C name.
 * @return   `true` when it has been registered.
 */
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/**
 * @brief Close the `eq` set inwards: a container that needs equality needs it for its elements too.
 *
 * A struct stops this walk, because its equality goes through `d->eq` to the user's `fn ==`.
 *
 * @param g  The generator; walks `g->eqNeed`, to which the loop appends elements.
 */
static void closeEqNeeds(CG *g) {
    for (size_t i = 0; i < g->eqNeed.len; i++) {   /* reread: entries are appended */
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind == TY_ARRAY) { needEq(g, t->inner); continue; }
        if (t->kind == TY_GENERIC && isView(t) && !isByteView(t))
            needEq(g, *(Type **)vecAt(&t->targs, 0));
    }
}

/**
 * @brief Write the descriptor pool into `g->desc`.
 *
 * The pool holds the equality adapters and the descriptor definitions and is inserted between the
 * prototypes and the function bodies. When both `g->descs` and `g->eqNeed` are empty the whole
 * block is unnecessary.
 *
 * @param g  The generator; temporarily points `g->out` at `g->desc` and restores it afterwards.
 */
static void emitDescRegion(CG *g) {
    /* Nothing structured was printed and no array was compared, so the whole
     * pool - descriptors, printing and comparison - is unnecessary. */
    if (g->descs.len == 0 && g->eqNeed.len == 0) return;
    closeEqNeeds(g);
    Buf *saved = g->out;
    g->out = &g->desc;
    emitDescDefs(g);                       /* dry pass: collect dependencies, drop text */
    bufInit(&g->desc, g->arena);

    /* ---- Equality adapters for structural `==` ----
     * They must precede the descriptors, which take their address. Only a
     * struct that `extc_eq` really recurses into needs one: that struct's own
     * `fn ==` is arbitrary user code, so the runtime can only delegate to it.
     * Every other kind is recursed into by `extc_eq` itself. */
    for (size_t i = 0; i < g->eqNeed.len; i++) {
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind != TY_STRUCT && t->kind != TY_GENERIC) continue;
        if (t->kind == TY_GENERIC && isView(t)) continue;   /* extc_eq recurses into views */
        FuncDef *m = findOpMethod(g->tt, t, "==", t, NULL);
        if (!m) {
            /* Unreachable: the checker permits a comparison only when the
             * element type is comparable. If it is reached anyway, fail loudly
             * rather than emit C that does not compile. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: structural equality needs a `fn ==` on this type");
            continue;
        }
        genEqAdapter(g, t, m);
    }
    if (g->eqNeed.len) cgLine(g, "");

    cgLine(g, "/* ---- Type descriptor table, emitted on demand: only types that are used. ---- */");
    for (size_t i = 0; i < g->descs.len; i++) {  /* all forward-declared: order does not matter */
        const char *tn = (*(Type **)vecAt(&g->descs, i))->name;
        size_t pb = g->out->len;
        cgLine(g, "static const ExtcDesc %s_desc;", tn);
        deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", tn), pb);
    }
    cgLine(g, "");
    emitDescDefs(g);                       /* real pass: emit the definitions */
    cgLine(g, "");
    g->out = saved;
}

/* Per-type print functions are gone: printing is descriptor data plus one
 * `extc_print`, shared by the whole program. No `_debug`, `_writeText` or
 * `_name` is derived per type.
 */

/* The operator rule comes from `typeSupportsOp`, the same predicate the checker applies to
 * concrete types, so there is one rule instead of two that can disagree. Answering "can C
 * compare this type natively with `==`?" by kind alone says yes for every builtin and every
 * enum, so an instance of a generic that applied `%` to an `f64` - which the checker rejects,
 * because `%` is for integers - would be emitted as C `%` and fail inside gcc, in a file the
 * user cannot read.
 */

/**
 * @brief Find a user-defined operator method on a type.
 *
 * It goes through the checker's own lookup, `findOperator`, instead of keeping a second copy here: a
 * hand-written scan does not substitute a generic receiver's type arguments and would miss methods
 * on `slice<u8> == slice<u8>`, leaving the caller to compare two structs with bare C `==`.
 *
 * @param tt        The type table the method lookup runs in.
 * @param t         The operator's receiver type; `NULL` when `ttBase(t)` is neither a struct nor a
 *                  generic instance.
 * @param sym       The symbol to find, for example `"=="`.
 * @param rhs       The right operand's type, which takes part in overload resolution.
 * @param fallback  The symbol to try when `sym` does not exist, for example `"=="` while looking
 *                  for `"!="` (the caller negates the result); `NULL` means `sym` only.
 * @return          The method found; `NULL` when neither symbol is defined.
 */
static FuncDef *findOpMethod(TypeTable *tt, Type *t, const char *sym, Type *rhs, const char *fallback) {
    Type *b = ttBase(t);
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return NULL;
    /* Use the checker's own lookup rather than scanning the method list here: a hand-rolled scan
     * that compares the operand type without substituting a generic receiver's arguments would
     * miss `slice<u8> == slice<u8>` and let the caller fall back to a bare C `==` on two structs. */
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}

/* `typeHasEq`, which recursively decided whether an element type could be
 * compared, is gone: code generation derives no `_eq` for arrays, so
 * nothing on this side asks the question. Comparability is decided by the
 * checker, which reports ``[2]p` cannot be compared: its element type `p` does
 * not define `==` `` when it is violated. One copy of the rule cannot drift out
 * of sync with itself, so that class of bug is gone by construction.
 */

