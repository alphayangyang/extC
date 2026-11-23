/* ------------------------------------------------------- struct definition order
 *
 * Ordinary structs and generic instances contain each other - a `player` holds a
 * `slice<u8>`, whose fields are ordinary `i64`s - so emitting all ordinary
 * structs first and the instances afterwards does not work. Definitions are
 * emitted in dependency order instead, which also covers types such as
 * `array<point>` that do not exist yet.
 *
 * A cycle can only close through a `ref`, that is through a pointer, and a
 * pointer needs nothing but the typedef of the type it points at, so all
 * typedefs are emitted first.
 */

typedef struct {
    StructDef *sd;      /* an ordinary struct; NULL for an instance or an array */
    Type      *inst;    /* a generic instance or an array; NULL for a struct */
    TypeDef   *td;      /* an enum with payloads; NULL otherwise */
    Vec        deps;    /* int* - indices of the units this one depends on */
    bool       done;
} SUnit;

/**
 * @brief The C name of a definition unit (a struct, an instance, an array or an enum with payload).
 *
 * @param u  The definition unit; a generic struct instance and a generic enum instance both have
 *           `inst` and `sd`/`td` set.
 * @return   The instance name when `inst` is set (both arrays and generic enum instances take this
 *           path), otherwise the payload-carrying enum's name when `td` is set, otherwise the struct
 *           name.
 */
static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* instance or array, enum instances included */
    if (u->td) return u->td->name;          /* non-generic enum with payloads */
    return u->sd->name;
}

/**
 * @brief Find the index in `units` of the unit that defines a type.
 *
 * A generic instance is matched by C name rather than by pointer: `mut slice<T>` is a shadow type with
 * the same name and the same C struct as the read-only view, so a pointer comparison never finds it,
 * the dependency sort misses it, and `struct reader { chunk: mut slice<u8> }` then fails on an
 * incomplete type; besides, one C name can correspond to two instances, one writable view and one
 * read-only view.
 *
 * @param units  The unit list whose elements are `SUnit *`.
 * @param t      The type to find; returns -1 when NULL.
 * @return       The unit's index; -1 when `units` holds no matching unit.
 */
static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_ENUM && u->td && t->edef == u->td) return (int)i;
        if ((t->kind == TY_GENERIC || t->kind == TY_ARRAY) && u->inst &&
            strcmp(u->inst->name, t->name) == 0) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

/**
 * @brief Emit the C definition of a struct, an array or an enum.
 *
 * An enum with payload is emitted as `struct { tag; union }` and takes part in the dependency sort
 * like any other struct: a payload can contain other structs, and missing the sort would leave an
 * unknown type name in the generated C.
 *
 * @param g  The generator state; a generic instance and a generic enum instance enter the
 *           substitution context first and leave it after the output.
 * @param u  The definition unit to emit.
 */
static void unitBody(CG *g, SUnit *u) {
    if (u->td) {
        /* A generic enum instance such as `option<i64>` enters the
         * substitution context, so that `cType` substitutes the payload
         * types. */
        SubCtx sv = {0};
        if (u->inst) sv = substEnter(g, u->inst);
        cgLine(g, "struct %s {", unitName(u));
        g->indent++;
        cgLine(g, "int tag;");
        cgLine(g, "union {");
        g->indent++;
        for (size_t j = 0; j < u->td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&u->td->variants, j);
            if (v->types.len == 0) continue;
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "struct { ");
            for (size_t k = 0; k < v->types.len; k++) {
                if (k) bufPuts(&b, " ");
                bufPrintf(&b, "%s _%zu;", cType(g, *(Type **)vecAt(&v->types, k)), k);
            }
            bufPrintf(&b, " } %s;", v->name);
            cgLine(g, "%s", bufCstr(&b));
        }
        g->indent--;
        cgLine(g, "} u;");
        g->indent--;
        cgLine(g, "};");
        cgLine(g, "");
        if (u->inst) substLeave(g, sv);
        return;
    }

    bool generic = u->inst && u->inst->kind == TY_GENERIC;
    SubCtx gsv = {0};
    if (generic) gsv = substEnter(g, u->inst);

    cgLine(g, "struct %s {", unitName(u));
    g->indent++;
    if (u->inst && u->inst->kind == TY_ARRAY && u->inst->asize < 0) {
        /* A type whose length is still a `const` parameter is a template, not a value: nothing can
         * be laid out for it, and instantiation replaces it before any declaration mentions it. */
        g->indent--;
        cgLine(g, "};");
        return;
    }
    if (u->inst && u->inst->kind == TY_ARRAY) {
        /* An array is a struct holding a bare C array; that is where its value
         * semantics come from. */
        cgLine(g, "%s data[%lld];", cType(g, u->inst->inner),
               (long long)u->inst->asize);
    } else {
        for (size_t i = 0; i < u->sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "%s %s;", cType(g, fd->type), fd->name);
        }
        /* C does not allow an empty struct: `struct unit { };` is a GNU
         * extension, and `(unit){0}` then warns about excess elements. A struct
         * with no fields therefore gets one placeholder byte. A user never sees
         * it and `(T){0}` stays legal; `unit`, the payload type of
         * `result<unit, E>`, exists thanks to this. */
        if (u->sd->fields.len == 0) cgLine(g, "char __extc_empty;");
    }
    g->indent--;
    cgLine(g, "};");

    /* `@frozen`: the struct's bytes are laid out exactly as C lays out this field list --
     * declaration order, C's alignment and padding, nothing else inside. The two assertions below
     * make a reordered or slipped-in field fail in the **generated** C instead of at a call site
     * reading the wrong bytes: neighbours come out in increasing offset order (declaration order,
     * never sorted by size), and `sizeof` stays within the end of the last field plus the struct's
     * own alignment, which is the trailing padding C may add -- so an added field shows up as a
     * larger size unless it fits in that padding. Every number is derived from C (`offsetof`,
     * `sizeof`, `_Alignof`); a hard-coded `== 8` would break a legitimate build on a platform with
     * different alignment rules. */
    if (u->sd && u->sd->frozen && u->sd->fields.len > 0) {
        const char *tn = unitName(u);
        for (size_t i = 1; i < u->sd->fields.len; i++) {
            FieldDef *a = *(FieldDef **)vecAt(&u->sd->fields, i - 1);
            FieldDef *b = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "_Static_assert(offsetof(%s, %s) < offsetof(%s, %s),", tn, a->name, tn, b->name);
            cgLine(g, "               \"extC @frozen: the fields stay in declaration order\");");
        }
        FieldDef *last = *(FieldDef **)vecAt(&u->sd->fields, u->sd->fields.len - 1);
        cgLine(g, "_Static_assert(sizeof(%s) <= offsetof(%s, %s) + sizeof(((%s *)0)->%s)"
                   " + _Alignof(%s),",
               tn, tn, last->name, tn, last->name, tn);
        cgLine(g, "               \"extC @frozen: nothing may be added to a frozen struct\");");
    }
    cgLine(g, "");
    if (generic) substLeave(g, gsv);
}

/* The derived `_eq` for arrays is gone: array equality now goes through
 * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
 * implementation. The old shape emitted a loop per array type, which cost 2450
 * lines, 32% of the generated C, for 200 types in a stress program.
 */

/**
 * @brief Register the unwrapping helper of an `option` / `result` instance and return its name.
 *
 * Deduplicated by the option type's C name: every `!` on one instance points at the same helper and
 * only one definition is emitted. An `@unchecked` body does not come here (it takes the payload
 * directly), so the helper can only come from a checked `!`.
 *
 * @param g      The generator state.
 * @param ot     The operand type, an `option<T>` or `result<T, E>` instance.
 * @param isOpt  true for `option` (tag `some`), false for `result` (tag `success`).
 * @param pay    The payload type `T`, which is also the helper's return type.
 * @return       The helper's name; the call site uses it to pass the operand, the extC source path
 *               and the line number.
 */
static const char *unwrapHelper(CG *g, Type *ot, bool isOpt, Type *pay) {
    const char *otName = cType(g, ot);
    const char *name = arenaPrintf(g->arena, "extc_unwrap$%s", otName);
    for (size_t i = 0; i < g->unwraps.len; i++)
        if (strcmp(((UnwrapHelper *)vecAt(&g->unwraps, i))->name, name) == 0)
            return name;

    const char *var = isOpt ? "some" : "success";
    Buf b;
    bufInit(&b, g->arena);
    /* The payload type is rendered **here**, inside the instance's substitution context:
     * at assembly time the context is long gone and a `T` would print as `int`. */
    bufPrintf(&b, "static %s %s(%s o, const char *f, int l) {\n",
              cType(g, pay), name, otName);
    bufPrintf(&b, "    (void)extc_tagAssert((int)o.tag, %s_%s, f, l);\n", otName, var);
    bufPrintf(&b, "    return o.u.%s._0;\n}\n", var);

    UnwrapHelper *h = (UnwrapHelper *)vecPush(&g->unwraps);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/**
 * @brief Register the slicing helper of a base type and return its name.
 *
 * Registering again with the same base type and the same kind returns the same name and only one
 * function is emitted. The `unchecked` helper has a name of its own, and a checked caller keeps using
 * the checked one: the two cannot share a definition.
 *
 * @param g          The generator state.
 * @param ob         The sliced type: an array or a view.
 * @param st         The resulting slice type.
 * @param tail       true when the slice runs to the end (`s[lo..]`), where the omitted upper bound
 *                   takes the base type's length.
 * @param unchecked  true when the caller's body is `@unchecked`: the helper skips
 *                   `extc_checkedRange` and uses the written form of `lo` directly.
 * @return           The helper's name, built from the base type and the kind; one base type is cut
 *                   into only one kind of slice, so the names cannot collide.
 */
static const char *sliceHelper(CG *g, Type *ob, Type *st, bool tail, bool unchecked) {
    const char *name = arenaPrintf(g->arena, "extc_slice%s%s_%s",
                                   tail ? "To" : "", unchecked ? "U" : "", ob->name);
    for (size_t i = 0; i < g->helpers.len; i++)
        if (strcmp(((SliceHelper *)vecAt(&g->helpers, i))->name, name) == 0)
            return name;

    const char *ret = cType(g, st);
    Buf b;
    bufInit(&b, g->arena);
    if (ob->kind == TY_ARRAY) {
        /* A fixed-size array: the length is a compile-time constant. */
        bufPrintf(&b, "static %s %s(%s *a, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, %lld, f, ln);\n",
                      (long long)ob->asize);
        bufPrintf(&b, "    return (%s){ .data = &a->data[s], .len = hi - lo };\n}\n", ret);
    } else if (tail) {
        /* Slicing to the end: hi is v.len. It is taken as a parameter instead
         * of being expanded in place, so the view is evaluated once. */
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, const char *f, int ln) {\n",
                  ret, name, ob->name);
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, v.len, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = v.len - lo };\n}\n", ret);
    } else {
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        if (unchecked)
            bufPrintf(&b, "    int64_t s = lo;\n    (void)f; (void)ln;\n");
        else
            bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = hi - lo };\n}\n", ret);
    }

    SliceHelper *h = (SliceHelper *)vecPush(&g->helpers);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/**
 * @brief Generate the view expression produced by `a[lo..hi]`.
 *
 * A slice the compiler can prove leaves no run-time trace: with a fixed-size array as the base and
 * both bounds as literals (an omitted bound is filled in by the checker), the range is known during
 * checking and an out-of-range one has already been reported, so the generated C is directly
 * `&a.data[2]` plus `.len = 3`; every other case goes to the helper, which checks the range at run
 * time and reports the error with the extC source position.
 *
 * @param g  The generator state.
 * @param e  The slice expression; `e->type` is the resulting view type, and when the base is
 *           `ref [N]T` it is a pointer in C, so taking `.data` needs one extra dereference.
 * @return   The view expression; when the type cannot be generated, reports the error first and
 *           returns the placeholder expression "0".
 */
static const char *genSlice(CG *g, Expr *e) {
    Type *ob = ttBase(subst(g, e->u.slice.obj->type));
    Type *st = subst(g, e->type);
    if (!ob || !st || !ob->name) {
        ctxError(g->ctx, e->line, 1, NULL, "cannot generate a slice of this type");
        return "0";
    }

    const char *obj = genExpr(g, e->u.slice.obj);
    Expr *lo = e->u.slice.lo, *hi = e->u.slice.hi;
    /* When the base is a `ref [N]T`, which is a pointer in C, reading `.data`
     * needs one dereference, as in `(*p).data[..]`, but the argument passed to
     * the slice helper is that pointer itself, since the helper takes the
     * `array_*` type. Without the dereference the result is `p.data[..]`,
     * and gcc answers "'p' is a pointer". */
    bool objIsRef = e->u.slice.obj->type &&
                    subst(g, e->u.slice.obj->type)->kind == TY_REF;

    if (ob->kind == TY_ARRAY && lo && hi &&
        lo->kind == EX_INT && hi->kind == EX_INT) {
        const char *arr = objIsRef ? arenaPrintf(g->arena, "(*%s)", obj) : obj;
        return arenaPrintf(g->arena, "(%s){ .data = &(%s.data[%lld]), .len = %lld }",
                           cType(g, st), arr, lo->u.ival, hi->u.ival - lo->u.ival);
    }

    const char *loS = lo ? genExpr(g, lo) : "0";
    /* `ref [N]T` is a pointer the helper already takes by pointer; `ref slice<T>` is a
     * pointer to the view struct, and the helper takes the struct **by value**, so it
     * needs one dereference. Without it `v[1..]` through a `ref slice` parameter emitted
     * `helper(v, ...)` with `v` a `slice_*`, which gcc rejects. */
    const char *arg = ob->kind != TY_ARRAY
                      ? (objIsRef ? arenaPrintf(g->arena, "(*%s)", obj) : obj)
                      : (objIsRef ? obj : arenaPrintf(g->arena, "&(%s)", obj));
    if (!hi) {
        /* Only a view base can reach here: for an array base the checker has
         * already filled the omitted bound in as a literal. */
        const char *fn = sliceHelper(g, ob, st, true, g->uncheckedBody);
        return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), \"%s\", %d)",
                           fn, arg, loS, g->path, e->line);
    }
    const char *fn = sliceHelper(g, ob, st, false, g->uncheckedBody);
    const char *hiS = genExpr(g, hi);
    return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), (int64_t)(%s), \"%s\", %d)",
                       fn, arg, loS, hiS, g->path, e->line);
}


/* Close the set of generated instances transitively.
 *
 * An instance mentioned only in a method signature never reached `units` unless
 * the program used it directly, so `vector<i64>::get()` returning `option<i64>`
 * produced C that named an unknown type. The user had written nothing wrong; the
 * compiler had simply not finished its own accounting.
 *
 * The set is closed by walking the fields, the method signatures and the enum
 * payloads of every unit, substituting the type arguments of a generic instance,
 * and adding every instance met on the way until nothing new appears. The
 * iteration count is capped, and passing the cap is reported loudly rather than
 * quietly generating too little.
 */
/**
 * @brief Add the generic instances a type mentions to the unit list; existing ones are skipped by C
 *        name.
 *
 * @param arena  The arena for allocating new units.
 * @param units  The unit list, appended to in place.
 * @param t      The type to check; returns immediately when it is not a generic struct or enum
 *               instance.
 */
static void addInstanceUnit(Arena *arena, Vec *units, Type *t) {
    if (!t) return;
    bool isStructInst = (t->kind == TY_GENERIC && t->sdef);          /* vector<i32> */
    bool isEnumInst   = (t->kind == TY_ENUM && t->edef && t->edef->typeParams.len > 0);
    if (!isStructInst && !isEnumInst) return;
    /* Deduplication must be by C name and cannot go through unitFind, which
     * compares a generic enum instance by its template: `option_i64` would match
     * an existing `option_i32`, since the two share the template, be considered
     * already present, and never be generated, leaving the generated C with an
     * unknown type name. */
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (u->inst && t->name && strcmp(u->inst->name, t->name) == 0) return;
        if (u->td && t->kind == TY_ENUM && u->inst == NULL && u->td == t->edef) return;
    }
    SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
    if (isStructInst) u->sd = t->sdef; else u->td = t->edef;
    u->inst = t;
    vecInit(&u->deps, arena, sizeof(int));
    *(SUnit **)vecPush(units) = u;
}

/**
 * @brief Scan a type and add every generic instance it mentions to the unit list.
 *
 * It follows `inner` and the generic arguments, so `ref`, slices and array elements are all reached.
 * A function type keeps its parts on `params` / `ret` rather than on `inner` / `targs`, so it gets a
 * second walk: `fn(pair<i32, u8>) -> void` written inside a struct is a mention of the
 * `pair<i32, u8>` instance, and missing it leaves that struct unemitted.
 *
 * @param arena  The arena for allocating new units.
 * @param units  The unit list, appended to in place.
 * @param t      The type to scan; returns immediately when NULL.
 * @param depth  The recursion depth, starting at 0; the walk stops above 12, which bounds
 *               self-referential types.
 */
static void scanTypeForUnits(Arena *arena, Vec *units, Type *t, int depth) {
    if (!t || depth > 12) return;                                    /* bounds self-reference */
    addInstanceUnit(arena, units, t);
    if (t->inner) scanTypeForUnits(arena, units, t->inner, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        scanTypeForUnits(arena, units, *(Type **)vecAt(&t->targs, i), depth + 1);
    /* A function type mentions its parts through `params` / `ret`, not through `inner` /
     * `targs`, so it needs its own walk: `f: fn(pair<i32, u8>) -> void` inside a struct is a
     * mention of the instance `pair_i32_u8`, and without this walk that struct is never
     * emitted. */
    if (t->kind == TY_FN) {
        scanTypeForUnits(arena, units, t->ret, depth + 1);
        for (size_t i = 0; i < t->params.len; i++)
            scanTypeForUnits(arena, units, *(Type **)vecAt(&t->params, i), depth + 1);
    }
}

/**
 * @brief Scan every type a unit mentions and add the generic instances it depends on to the unit
 *        list.
 *
 * Three places are walked: the fields; the method signatures (both parameters and return type count,
 * and an instance hides there); and the payloads of the enum variants. When the unit is itself a
 * generic instance, its own type arguments are substituted in first.
 *
 * @param arena  The arena for allocating new units.
 * @param tt     The type table used for generic parameter substitution.
 * @param units  The unit list, appended to in place.
 * @param u      The unit to scan.
 */
static void scanUnitForUnits(Arena *arena, TypeTable *tt, Vec *units, SUnit *u) {
    Vec *tps = NULL; Vec *tas = NULL;
    StructDef *sd = u->sd ? u->sd : (u->inst ? u->inst->sdef : NULL);
    if (u->inst && sd) { tps = &sd->typeParams; tas = &u->inst->targs; }
    /* 1. fields */
    if (sd) for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (tps && ft) ft = ttSubstitute(tt, ft, tps, tas);
        scanTypeForUnits(arena, units, ft, 0);
    }
    /* 2. method signatures: parameters and return type */
    if (sd) for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&sd->methods, i);
        /* The coroutine protocols (`next`/`value`, on the frame and on the handle) are emitted
         * inline at their call sites, never as real functions. */
        if (planCoroProto(f)) continue;
        for (size_t j = 0; j < f->params.len; j++) {
            Type *pt = (*(Param **)vecAt(&f->params, j))->type;
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
        Type *rt = f->ret;
        if (tps && rt) rt = ttSubstitute(tt, rt, tps, tas);
        scanTypeForUnits(arena, units, rt, 0);
    }
    /* 3. enum payloads */
    if (u->td) for (size_t i = 0; i < u->td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&u->td->variants, i);
        for (size_t k = 0; k < v->types.len; k++) {
            Type *pt = *(Type **)vecAt(&v->types, k);
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
    }
}

/* Generate the whole C translation unit for a module.
 *
 * Params:
 *   lineMap - whether to emit `#line` directives
 *   out     - receives the generated text
 *
 * Returns:
 *   false when the checker recorded an error, true otherwise. Reporting a
 *   consistency problem never stops generation, so that the caller sees the
 *   error count and not a damaged output buffer.
 */
/**
 * @brief Does an instance's signature still mention a type parameter?
 *
 * Such a FuncDef is the checker's product: a generic call inside a generic body creates an instance
 * whose argument is `T` itself so that the body can be checked; it is never a C function and the
 * emission loop skips it.
 *
 * @param f  The function instance to check.
 * @return   true when a parameter type or the return type still mentions a type parameter.
 */
static bool funcSignatureMentionsParam(FuncDef *f) {
    if (f->ret && mentionsParam(f->ret)) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (p->type && mentionsParam(p->type)) return true;
    }
    return false;
}

