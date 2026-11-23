/* ---------------------------------------------------------------- type descriptors
 *
 * Printing derives no code any more: every type yields one `static const
 * ExtcDesc`, which is pure data, and the whole program shares a single
 * `extc_print`, the runtime part emitted at the top of the generated file.
 *
 * The abandoned scheme, kept here only as a warning, generated a whole printf
 * sequence per struct: a synthetic stress program grew 2028 such functions,
 * 22.9% of the generated C, and printed none of them.
 *
 * `descRef` maps an extC type to the address of the constant describing it:
 *   - scalar, ref, slice<u8>: one of the few descriptors shared by the whole
 *     program, which cost no extra space
 *   - struct, generic instance, array, enum: its own `<C name>_desc`
 *
 * Notes:
 *   - Descriptors reference one another; `struct s { xs: slice<s> }` is a cycle.
 *     That is why the pool opens with a `static const ExtcDesc X_desc;`
 *     tentative definition for every descriptor, which makes the definition
 *     order irrelevant.
 */
/**
 * @brief Register that a type needs a descriptor of its own.
 *
 * Deduplicated by C name: a mutable and a read-only view are one C struct and share one descriptor.
 * Scalars, `ref` and `slice<u8>` use the shared descriptor and are not registered.
 *
 * @param g  The generator; the registry is `g->descs` and new types are appended at the end.
 * @param t  The type; returns immediately when it is `NULL` or has no C name.
 */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/**
 * @brief Register that a type needs `extc_eq`.
 *
 * Deduplicated by C name. Comparing a container compares its elements, so the element type is
 * registered too; `emitDescRegion` closes this demand set into a closure.
 *
 * @param g  The generator; the registry is `g->eqNeed` and new types are appended at the end.
 * @param t  The type; returns immediately when it is `NULL` or has no C name.
 */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

/**
 * @brief Return the C expression of the descriptor of a type.
 *
 * Walking outwards registers every type the descriptor refers to, which is why the demand set
 * becomes a closure.
 *
 * @param g  The generator.
 * @param t  The type; `NULL` yields `&extc_desc_i32`.
 * @return   A string such as `&extc_desc_i32` or `&list_i32_desc`, allocated in the generator's
 *           arena.
 */
static const char *descRef(CG *g, Type *t) {
    if (!t) return "&extc_desc_i32";
    needDesc(g, t);
    if (t->kind == TY_BUILTIN) return arenaPrintf(g->arena, "&extc_desc_%s", t->name);
    if (t->kind == TY_REF)     return "&extc_desc_ref";
    if (isByteView(t))         return "&extc_desc_text";
    /* Everything else has one of its own; a generic instance uses its own C
     * name, as in list_i32_desc. */
    return arenaPrintf(g->arena, "&%s_desc", t->name);
}

/**
 * @brief Record a top-level definition just emitted into the current buffer, so that it can later
 *        be judged for references.
 *
 * As with globals, the finished text decides whether anything names the definition. The descriptor
 * pool depends on this: its definitions are produced on demand and not every one of them is
 * referenced. The declaration and the definition are two separate texts registered under the same
 * name.
 *
 * @param g       The generator; the record is appended to `g->deadDefs`.
 * @param name    The C symbol name the definition is registered under.
 * @param before  The offset of the definition in `g->out`.
 */
static void deadDefAdd(CG *g, const char *name, size_t before) {
    Buf l;
    bufInit(&l, g->arena);
    bufPutn(&l, g->out->data + before, g->out->len - before);
    DeadDef *d = arenaAllocZero(g->arena, sizeof *d);
    d->name = name;
    d->text = bufCstr(&l);
    *(DeadDef **)vecPush(&g->deadDefs) = d;
}

/**
 * @brief Emit the descriptor of a struct: field names plus `offsetof`.
 *
 * `offsetof` instead of computing the layout here: a missed field or a wrong layout fails at
 * compile time instead of printing garbage.
 *
 * @param g      The generator.
 * @param cname  The struct's C name, also the prefix of its descriptor and field table.
 * @param disp   The name to display; a generic instance uses the template name
 *               (`vector { ... }`).
 * @param sd     The struct definition; field types are substituted before their descriptors are
 *               taken when the field table is written.
 * @param eqFn   The C name of the equality adapter; `NULL` when the type has none.
 */
static void genStructDesc(CG *g, const char *cname, const char *disp, StructDef *sd,
                          const char *eqFn) {
    size_t n = sd->fields.len;
    if (n) {
        size_t ftb = g->out->len;              /* the whole table, up to its `};` */
        cgLine(g, "static const ExtcField %s_fields[] = {", cname);
        g->indent++;
        for (size_t i = 0; i < n; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            cgLine(g, "{ \"%s\", offsetof(%s, %s), %s },", fd->name, cname, fd->name,
                   descRef(g, subst(g, fd->type)));
        }
        g->indent--;
        cgLine(g, "};");
        /* Registered like a descriptor: the row that names it may itself be dropped. */
        deadDefAdd(g, arenaPrintf(g->arena, "%s_fields", cname), ftb);
    }
    size_t sdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_STRUCT, \"%s\", sizeof(%s), %zu, %s, NULL, %s };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_fields", cname) : "NULL",
           eqFn ? eqFn : "NULL");
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", cname), sdb);
}

/**
 * @brief Emit the descriptor of an array: element count plus element descriptor.
 *
 * An array has no field names, so it prints as `[1, 2, 3]`.
 *
 * @param g    The generator.
 * @param arr  The array type; `name` is its C name and `inner` the element type.
 */
static void genArrayDesc(CG *g, Type *arr) {
    size_t adb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s, NULL };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", arr->name), adb);
}

/**
 * @brief Emit the descriptor of a non-byte view; it prints as `[a, b]`.
 *
 * A byte view never reaches here: it shares `extc_desc_text`.
 *
 * @param g  The generator.
 * @param v  The view instance; the first element of `targs` is the element type, substituted before
 *           its descriptor is taken.
 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    size_t vdb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s, NULL };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", v->name), vdb);
}

/**
 * @brief Emit the descriptor of an enum: the variant-name table, with a value outside the table
 *        printed as `<type name>`.
 *
 * Only variant names are printed, never payloads.
 *
 * @param g      The generator.
 * @param cname  The enum's C name, also the prefix of its descriptor and variant table.
 * @param disp   The name to display.
 * @param td     The enum definition; with no variants the variant table is not emitted and that
 *               cell of the descriptor is `NULL`.
 */
static void genEnumDesc(CG *g, const char *cname, const char *disp, TypeDef *td) {
    size_t n = td->variants.len;
    if (n) {
        Buf b;
        bufInit(&b, g->arena);
        for (size_t j = 0; j < n; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&b, ", ");
            bufPrintf(&b, "\"%s\"", v->name);
        }
        size_t vtb = g->out->len;
        cgLine(g, "static const char *const %s_variants[] = { %s };", cname, bufCstr(&b));
        deadDefAdd(g, arenaPrintf(g->arena, "%s_variants", cname), vtb);
    }
    size_t edb = g->out->len;
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ENUM, \"%s\", sizeof(%s), %zu, %s, NULL, NULL };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_variants", cname) : "NULL");
    deadDefAdd(g, arenaPrintf(g->arena, "%s_desc", cname), edb);
}

