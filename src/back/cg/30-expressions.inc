/* ---------------------------------------------------------------- expressions */

static const char *genExpr(CG *g, Expr *e);

/**
 * @brief Generate the **top-level value expression** of a statement and record "we are at statement
 *        top level" in `g->topExpr`.
 *
 * Only ownership delegation looks at this marker (`homeOwnArg`): when the call site is the
 * top-level value of a whole statement, the owner cannot still be holding a reference into this
 * region that lives only in a register -- and that is exactly what the callee's sweep, seeded from
 * its static root table, would miss. Subexpressions never delegate (they fall back to the owed
 * collection path of `rcEmitOwe`).
 *
 * @param g  The generator.
 * @param e  The statement's value expression.
 * @return   The C text of that expression.
 */
static const char *genTopExpr(CG *g, Expr *e) {
    const Expr *saved = g->topExpr;
    g->topExpr = e;
    const char *code = genExpr(g, e);
    g->topExpr = saved;
    return code;
}

static const char *selfOperandAsParam(CG *g, Expr *operand, const char *code,
                                      Param *p0, Type *want);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);
static bool cgIsEntry(CG *g, const FuncDef *f);       /* `main`, or a `@module` entry */
static const char *cgEntryCName(CG *g, const FuncDef *f);
static const char *zoneArgRef(CG *g, Expr *e);   /* the zone argument of a pool-creating callee */
static void cgReclaimDropArena(CG *g, int lvl);  /* release the region at frame destruction (defined after cgIsRefType) */
/* The collection trigger gate and the owed-collection accounting (defined further below):
 * `cgImplicitArgs` needs them at a call site to decide whether this frame is eligible, or has to,
 * collect on itself.
 *
 * The trigger route has three states: this frame owns the region => sweep now (`RC_OWN_YES`); it
 * does not own it => only record what is owed (`RC_OWN_NO`); thread-local arena (parTls) => at run
 * time `extc_rc_tlsDepth` decides whether this is a worker entry frame, and an entry frame sweeps
 * now while an inner frame records what is owed (`RC_OWN_TLS`, see `rcOwnKind`). */
typedef enum { RC_OWN_NO = 0, RC_OWN_YES = 1, RC_OWN_TLS = 2 } RcOwn;

static bool rcFrameOwnsArena(CG *g, int lvl);
static RcOwn rcOwnKind(CG *g, int lvl);
static void rcEmitOwedCheck(CG *g, const char *basePtr, int lvl, RcOwn own);


    

/**
 * @brief Map an extC symbol name onto a fragment usable inside a C identifier.
 *
 * An operator such as `==` is a legal method name in extC but cannot go into a C identifier, so it
 * is renamed here; the mapping table must cover every operator the language allows as a method
 * name, because one it misses is pasted into the generated C verbatim and fails to compile. A user
 * name can collide with a C keyword (`double` is not a keyword in extC); those get a `__c` suffix,
 * with the keyword set supplied by `cIdentIsKeyword`, shared with the checker.
 *
 * @param g     Generator state; the new name is allocated in g->arena.
 * @param name  extC symbol name or identifier.
 * @return      The fragment usable in a C identifier; `name` itself when there is no mapping and it
 *              is not a keyword, otherwise the new name in the arena.
 */
static const char *cSymName(CG *g, const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq"  }, { "!=", "ne"  },
        { "<",  "lt"  }, { "<=", "le"  }, { ">",  "gt" }, { ">=", "ge" },
        { "+",  "add" }, { "-",  "sub" }, { "*",  "mul" },
        { "/",  "div" }, { "%",  "rem" },
        { "<<", "shl" }, { ">>", "shr" },
        { "[]", "idx" }, { "[]=", "idxset" },
        /* The compound assignment hooks are separate names from their value-building cousins, so
         * they get their own C fragments (`+=` is not `add`). */
        { "+=", "iadd" }, { "-=", "isub" }, { "*=", "imul" },
        { "/=", "idiv" }, { "%=", "irem" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    if (cIdentIsKeyword(name)) return arenaPrintf(g->arena, "%s__c", name);
    return name;
}

/**
 * @brief Take the external C symbol name of an `@export` function, that is, the name the author
 *        wrote.
 *
 * An extC identifier may not contain `$`, so a `$` in `f->name` can only be the loader's module
 * prefix (`plugin$init`) and the symbol name is the part after it. An exported symbol is a contract
 * with the C side: names this compiler would have to rename (such as `fn double`) are rejected
 * where they are written instead of quietly becoming `double__c`.
 *
 * @param f  Function definition; `?` when it is NULL or its name is empty.
 * @return   The symbol name after the module prefix; the whole name when there is no `$`.
 */
static const char *exportName(const FuncDef *f) {
    const char *n = (f && f->name) ? f->name : "?";
    const char *d = strrchr(n, '$');
    return d ? d + 1 : n;
}

/**
 * @brief Give the type suffix an operator's C name needs when its owner defines that operator more
 *        than once.
 *
 * The same operator may be defined once per right-operand type, so the operand type must go into
 * the C name or several definitions collapse onto one symbol. The definition side (`cFuncName`) and
 * every call site (`cMethodName`) must use the same computation, so it lives in one function.
 *
 * The suffix must be computed in the **callee's** monomorphization context: `subst` reads the
 * ambient context, and a call site without one leaves `T` as `T`, so the names would disagree with
 * the definition side. A call site therefore passes the callee's own type arguments explicitly.
 *
 * @param g       Generator state; the result is allocated in g->arena.
 * @param f       The operator method definition.
 * @param params  Type parameters to substitute; `NULL` means use the ambient context.
 * @param args    Type arguments as long as params; also `NULL` when params is.
 * @return        `_<mangled operand type>` (a writable view adds the `mut_` prefix); the empty
 *                string when the owner defines the operator once only, when there are fewer than
 *                two parameters, or when the name is not an overloadable operator.
 */
static const char *opOverloadSuffixIn(CG *g, FuncDef *f, Vec *params, Vec *args) {
    if (!f->owner || f->params.len < 2 || !isOverloadableOp(f->name)) return "";
    size_t same = 0;
    for (size_t i = 0; i < f->owner->methods.len; i++)
        if (strcmp((*(FuncDef **)vecAt(&f->owner->methods, i))->name, f->name) == 0) same++;
    if (same <= 1) return "";
    Param *p1 = *(Param **)vecAt(&f->params, 1);
    Type *rt = (params && args) ? ttSubstitute(g->tt, p1->type, params, args) : subst(g, p1->type);
    /* A writable view and the read-only one are **different types** to the operator
     * rule (`ttEquals` compares `mut`) but the **same C struct**, so `ttMangle` gives
     * both the name `slice_u8`; the suffix has to carry the mutability itself, or the
     * exact-type dispatch that makes overloading decidable is not expressible in C. */
    const char *mangled = ttMangle(g->tt, rt);
    if (rt && rt->mut) mangled = arenaPrintf(g->arena, "mut_%s", mangled);
    return arenaPrintf(g->arena, "_%s", mangled);
}

/**
 * @brief Definition-side operator type suffix: here the ambient context is the instance's own.
 *
 * @param g  Generator state.
 * @param f  The operator method definition.
 * @return   Same as opOverloadSuffixIn.
 */
static const char *opOverloadSuffix(CG *g, FuncDef *f) {
    return opOverloadSuffixIn(g, f, NULL, NULL);
}

/**
 * @brief Give a function's name in the generated C.
 *
 * The tests run in a fixed order: an exported function returns the name the author wrote first (a
 * call site must match the symbol the C side looks up), a generic free function's instance returns
 * its own C name, and only then come the operator suffix and the owner prefix. A method name
 * carries its owner prefix so that `Point_eq` and `Board_eq` do not collide.
 *
 * @param g  Generator state; the result is allocated in g->arena.
 * @param f  Function definition.
 * @return   The C name, allocated in the generator's arena.
 */
static const char *cFuncName(CG *g, FuncDef *f) {
    /* The export name must be decided **first**: a call site has to match the symbol the C side
     * looks up, and no module prefix, instance prefix or keyword rename may get in front of it. */
    if (f->isExport) return exportName(f);
    /* An instance of a generic free function carries its own C name (eq2_i32). */
    if (planInstName(f)) return planInstName(f);
    const char *suffix = opOverloadSuffix(g, f);
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s%s", g->ownerPrefix, cSymName(g, f->name), suffix);
    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return arenaPrintf(g->arena, "%s%s", cSymName(g, f->name), suffix);
}

/**
 * @brief Give a method's name in the generated C at a call site.
 *
 * The prefix comes from the receiver: a generic instance uses its own name (such as
 * `Pair_i32_u8_getFirst`), everything else uses the method's owner. `cFuncName` cannot be used
 * here: it adds the prefix of the instance currently being generated, while the callee may belong
 * to another type -- calling `Point.==` from inside `Wrapper<Point>` is exactly that case.
 *
 * @param g         Generator state; the result is allocated in g->arena.
 * @param recvType  The receiver's static type.
 * @param f         The method definition.
 * @return          The C name, allocated in the generator's arena.
 */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    /* The suffix must come from the **callee's** context, not from the ambient one here: a method
     * instance has its own typeArgs and otherwise the receiver instance's are used; this is the
     * place that knows the receiver. */
    Vec *sp = NULL, *sa = NULL;
    if (planTemplate(f)) {
        sp = &planTemplate(f)->typeParams;
        sa = &f->targs;
    } else if (rb && rb->kind == TY_GENERIC && rb->sdef) {
        sp = &rb->sdef->typeParams;
        sa = &rb->targs;
    }
    const char *suffix = opOverloadSuffixIn(g, f, sp, sa);
    /* A lambda environment's `call`: an environment laid out per instance has one `call` per
     * instance, named after that layout (`LamVar`). */
    if (f->owner) {
        LamVar *v = lamVarFor(g, f->owner);
        if (v) return arenaPrintf(g->arena, "%s_%s%s", v->vt->name, cSymName(g, f->name), suffix);
    }
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s%s", rb->name, cSymName(g, f->name), suffix);

    if (f->owner)
        return arenaPrintf(g->arena, "%s_%s%s", f->owner->name, cSymName(g, f->name), suffix);
    return cSymName(g, f->name);
}

/**
 * @brief Test whether a type is a `slice` instance of exactly one type argument.
 *
 * This is the only thing the compiler knows about views, and it is an output convention rather than
 * knowledge of the container: printing a byte view as text belongs to basic output, while the
 * container's definition, fields and methods come from the standard library.
 *
 * @param t  The type to test.
 * @return   true when it is `slice<T>` with exactly one type argument.
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

/**
 * @brief Test whether a type is a byte view, that is, `slice<u8>`.
 *
 * @param t  The type to test.
 * @return   true when the element type is `u8`; `println` then prints the view as text rather than
 *           as a list of numbers.
 */
static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/**
 * @brief Emit the subscript primitive `<C name>_index(v, i, file, line)` of a view type.
 *
 * A view is passed by value, so the operand of a subscript is evaluated once and the value and
 * reference forms have no second code path. The primitive does its own bounds check and traps with
 * the extC source position when the index is out of range. The prelude's `get`, `==` and `find` all
 * reach it through `self[i]`, which keeps pointer arithmetic and hand-written checks out of the
 * library.
 *
 * The emitted function returns a pointer to the element and the call site dereferences it.
 * Returning the element value would break two things: a `ref` parameter needs an lvalue, and
 * `slice<struct>::==` (written `self[i] != other[i]`, with `self: ref T`) would not compile at all;
 * assignments such as `s[i] = x` and `s[i].field = x` need an lvalue just as much. A view already
 * has reference semantics, namely the `ref` in `data: ref T`.
 *
 * @param g     The generator.
 * @param inst  The view instance; `targs` has at least one element, the element type.
 */
static void genViewIndexer(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    SubCtx sv = substEnter(g, inst);
    cgLine(g, "static inline %s *%s_index(%s v, int64_t i, const char *file, int line) {",
           cType(g, elem), inst->name, inst->name);
    g->indent++;
    /* A view whose storage is gone has no `i` that could be in range: the `!v.data` test must
     * come before the index test, since `data == NULL` with a non-zero `len` would otherwise pass
     * the bounds check and store straight through the null pointer. The message names what
     * happened instead of blaming the index. */
    cgLine(g, "if (!v.data) extc_trapMsg(file, line, \"the view has no storage\");");
    cgLine(g, "if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);");
    cgLine(g, "return &v.data[i];");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g, sv);
}

/** @brief Does this type contain reference slots (`rcScanType != RC_NO_REFS`), including the
 *         conservative "cannot be enumerated" case? Declared here so `genViewCopier` can ask
 *         before `rcScanType` itself is defined; defined next to `rcScanType`. */
static bool typeHasRefSlots(CG *g, Type *t);
/**
 * @brief Emit on demand the whole-block copy primitive of a view type (used by `copyInto`).
 *
 * One call checks the count once and then moves bytes with `memmove`, so the check is paid per
 * **call** instead of per element: a byte loop would pay a range check per byte, plus a stale-pool
 * guard for containers. `memmove` rather than `memcpy`, because the common caller moves a buffer to
 * a position that overlaps it (`string::removeFront`).
 *
 * The count check traps with a source position like any other out-of-range access. `unchecked` is
 * the `@unchecked` variant: it `memmove`s exactly the written count with no length check and no
 * storage check, and its function name carries `..._copyU`.
 *
 * @param g          The generator.
 * @param inst       The view instance; `targs` has at least one element, the element type.
 * @param unchecked  When `true`, emit the unchecked variant named `<C name>_copyU`.
 */

static void genViewCopier(CG *g, Type *inst, bool unchecked) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    SubCtx sv = substEnter(g, inst);
    cgLine(g, "static inline int64_t %s_copy%s(%s d, %s s, int64_t n, const char *file, int line) {",
           inst->name, unchecked ? "U" : "", inst->name, inst->name);
    g->indent++;
    if (!unchecked) {
        cgLine(g, "if (n < 0 || n > d.len || n > s.len)");
        cgLine(g, "    extc_trapMsg(file, line, \"copyInto: the count is beyond a slice length\");");
        /* A view whose storage is gone has no byte to copy, so it traps. Conditional on `n > 0`:
         * copying zero elements from an empty view is legal and must stay legal. */
        cgLine(g, "if (n > 0 && (!d.data || !s.data))");
        cgLine(g, "    extc_trapMsg(file, line, \"the view has no storage\");");
    } else {
        cgLine(g, "(void)file; (void)line;");
    }
    cgLine(g, "if (n > 0) memmove((void *)d.data, (const void *)s.data, (size_t)n * sizeof(%s));",
           cType(g, elem));
    /* BUG-B (G13): the byte move duplicates every reference leaf of the element into the
     * destination, but a count lives **per object, not per slot** -- the destination would be a
     * second pointer without a second count (counting tier) or without a seed / sweep guard (sweep
     * tier). A later overwrite of the source slot then decrements the only count and frees the
     * object the destination still points at (uaf_bulk_copy prints the reused slot's value).
     * Order matters: the `memmove` stays first so its overlap semantics are unchanged
     * (`string::removeFront` moves a buffer onto itself); the accounting pass then walks the
     * **destination** slice and hands each copied element to `extc_rc_storeStructCopy`. When the
     * base pointer `d.data` is inside rc storage, the primitive can prove the host region from the
     * base: it records the cross-region edge (rsbit) for every owned reference word and does NOT
     * evict the target region from sweeping, so repeated `copyInto` stays bounded. Only a base
     * outside rc storage (stack / pool view) falls back to the conservative direction
     * (`extc_rc_storeStructCons` with no host: the target region drops out of sweeping). One count
     * short would free early, one count too many only reclaims late -- the conservative side.
     * Element types with no reference slots keep the plain `memmove` (the common `u8` copy).
     * The acyclicity criterion (iv) is told about this separately: the scan cannot see a byte move
     * (`acScanExpr` in `src/check/check_top.c` marks such a unit as not acyclic). */
    if (g->rc.reclaim && typeHasRefSlots(g, elem))
        cgLine(g, "for (int64_t __extc_bk = 0; __extc_bk < n; __extc_bk++)"
                  " extc_rc_storeStructCopy((void *)&d.data[__extc_bk], sizeof(%s), (void *)d.data);",
               cType(g, elem));
    cgLine(g, "return n;");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g, sv);
}

/**
 * @brief Register a view type's subscript primitive on demand and return its name.
 *
 * It can only be emitted on demand: the type-instance table alone is not enough, because a
 * subscript inside a generic body uses the name of the **instance** (`slice_slice_u8_index`), and
 * the table need not hold that instance -- a view whose element type came from a type parameter is
 * not in it, and the generated C would call a function nobody emitted and fail to compile. Whoever
 * needs the primitive emits it (`sliceHelper` works the same way), the dependency is centralized in
 * one place, and after name dedup one view type produces exactly one function.
 *
 * @param g     The generator; the dedup table is `g->viewIdx` and new instances are appended at the
 *              end.
 * @param inst  The view instance, already substituted by the caller.
 * @return      The primitive's name, `<view instance name>_index`.
 */
static const char *viewIndexer(CG *g, Type *inst) {
    const char *name = arenaPrintf(g->arena, "%s_index", inst->name);
    for (size_t i = 0; i < g->viewIdx.len; i++)
        if (strcmp((*(Type **)vecAt(&g->viewIdx, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(&g->viewIdx) = inst;
    return name;
}

/**
 * @brief Register a view type's whole-block copy primitive on demand and return its name.
 *
 * Registered on demand like `viewIndexer`. The `@unchecked` form has its own name (`..._copyU`) and
 * its own table `g->viewCpyU`: checked call sites of the same view type must keep getting the
 * checked definition.
 *
 * @param g          The generator.
 * @param inst       The view instance, already substituted by the caller.
 * @param unchecked  When `true`, register the `..._copyU` variant (table `g->viewCpyU`), otherwise
 *                   the checked variant (table `g->viewCpy`).
 * @return           The primitive's name, `<view instance name>_copy`; `<view instance name>_copyU`
 *                   when `unchecked`.
 */
static const char *viewCopier(CG *g, Type *inst, bool unchecked) {
    const char *name = arenaPrintf(g->arena, "%s_copy%s", inst->name, unchecked ? "U" : "");
    Vec *list = unchecked ? &g->viewCpyU : &g->viewCpy;
    for (size_t i = 0; i < list->len; i++)
        if (strcmp((*(Type **)vecAt(list, i))->name, inst->name) == 0) return name;
    *(Type **)vecPush(list) = inst;
    return name;
}

