/* ------------------------------------------------------- generic instances
 *
 * The checker verifies a template once; code generation emits one copy of the C
 * per instance. The source of a container is written in extC, in the prelude,
 * and all the compiler does is substitute the type arguments for T.
 */

/* Everything generated is `static`, except `main`, whose linkage C fixes.
 *
 * This is not style but performance. extC emits a single .c file, one
 * translation unit, so external linkage buys nothing while forcing gcc to assume
 * that other code may call the function and that pointers may alias, which makes
 * it reject outer-loop vectorization outright. Internal linkage also brings
 * cross-function inlining and stronger constant propagation for free, and
 * `-Wl,--gc-sections` stops being the only safety net.
 *
 * Should extC ever support several translation units, which it does not today,
 * this has to become "export only what another unit uses".
 */

/**
 * @brief Is a function the program's entry point?
 *
 * The criterion is a free function (no owner) whose name is exactly `main`.
 *
 * @param f  The candidate function; may be NULL.
 * @return   true when it is the entry point, false otherwise.
 */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

/**
 * @brief Is a function an **entry point**: C's main, or a free function marked @module?
 *
 * Both are treated the same in the product: the return value is C's int, no hidden parameters are
 * taken, the home arena is the function body's own, and falling off the body returns 0. The only
 * difference is that with a dispatcher C's main is the dispatcher and the entry itself becomes a
 * static helper.
 *
 * @param g  The code generation state (the criterion does not depend on it; the parameter is kept so
 *           call sites read consistently).
 * @param f  The candidate function; may be NULL.
 * @return   true when it is an entry point.
 */
static bool cgIsEntry(CG *g, const FuncDef *f) {
    (void)g;
    return cgIsMain(f) || (f && f->isModuleEntry);
}

/**
 * @brief The C name of an entry function in the product.
 *
 * With a dispatcher every entry is renamed to __extc_entry_<C name>: C's main already belongs to the
 * dispatcher, and the entry still needs a name with internal linkage for the table to take its
 * address. Without a dispatcher it is exactly main (a normal program's output is unchanged).
 *
 * @param g  The code generation state; needModuleTramp decides whether the prefix is used.
 * @param f  The function definition; a non-entry returns its C name unchanged.
 * @return   The C identifier in the product.
 */
static const char *cgEntryCName(CG *g, const FuncDef *f) {
    if (!f) return "";
    const char *base = cFuncName(g, (FuncDef *)f);
    if (g->needModuleTramp && cgIsEntry(g, f))
        return arenaPrintf(g->arena, "__extc_entry_%s", base);
    return base;
}

/**
 * @brief Emit a string that has **already been lexed** (a name or a description) as a C string
 *        literal, including the quotes.
 *
 * A string token's text has its surrounding quotes stripped and **its escapes kept as written in the
 * source** (the escape spellings were validated by the lexer's `lexEscape`, and C accepts the same
 * set), so it is copied between quotes here without escaping again. Escaping again would turn `"\""`
 * in the source into `\"` in the output (one extra backslash), and a TAB in a description into two
 * characters.
 *
 * @param b  The buffer receiving the text.
 * @param s  The string token's text, NUL-terminated; NULL is treated as the empty string.
 */
static void emitStringToken(Buf *b, const char *s) {
    bufPutc(b, '"');
    bufPuts(b, s ? s : "");
    bufPutc(b, '"');
}

/**
 * @brief A module's key: the same key is the same module (the entry file's is the empty string).
 */
static const char *moduleEntryKey(const FuncDef *f) {
    return (f->modName && *f->modName) ? f->modName : "";
}

/**
 * @brief Collect the @module entries, decide the names shown by --help, and validate uniqueness.
 *
 * Two rules are checked here because they are **command-line semantics** rather than type errors: a
 * module has exactly one entry (which is what -m selects), and an -m name is globally unique
 * (otherwise the command line cannot choose). An entry that fails a check does not enter the table
 * and the rest of the program is generated as usual.
 *
 * @param g  The code generation state; moduleEntries/needModuleTramp/hasMainEntry are filled in.
 * @param m  The current module; funcs holds all candidates.
 */
static void scanModuleEntries(CG *g, Module *m) {
    g->hasMainEntry = false;
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (cgIsMain(f)) g->hasMainEntry = true;
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f->isModuleEntry) continue;
        const char *key = moduleEntryKey(f);
        bool sameModule = false;
        for (size_t j = 0; j < i && !sameModule; j++) {
            FuncDef *o = *(FuncDef **)vecAt(&m->funcs, j);
            if (o->isModuleEntry && strcmp(moduleEntryKey(o), key) == 0) sameModule = true;
        }
        if (sameModule) {
            ctxError(g->ctx, f->line ? f->line : 1, 1,
                     "One module has one entry: -m names a module, not a function. Move this"
                     " one into another module, or drop @module from it.",
                     "module %s already has an @module entry", key[0] ? key : "<root>");
            continue;
        }
        const char *name = f->moduleEntryName;
        if (!name || !*name) name = key[0] ? key : "main";
        bool dup = false;
        for (size_t j = 0; j < g->moduleEntries.len && !dup; j++) {
            ModEntry *e = (ModEntry *)vecAt(&g->moduleEntries, j);
            if (strcmp(e->name, name) == 0) dup = true;
        }
        if (!dup && g->hasMainEntry && strcmp(name, "main") == 0) dup = true;
        if (dup) {
            ctxError(g->ctx, f->line ? f->line : 1, 1,
                     "-m names how the command line picks an entry, so two entries cannot"
                     " share one name. Give this one its own: @module(\"other\").",
                     "duplicate module entry name %s", name);
            continue;
        }
        ModEntry *e = (ModEntry *)vecPush(&g->moduleEntries);
        e->f = f;
        e->name = name;
        e->desc = f->moduleDesc ? f->moduleDesc : "";
    }
    g->needModuleTramp = g->moduleEntries.len > 0;
}

/**
 * @brief Emit C's main: a table dispatched by -m, plus --help.
 *
 * A program without @module emits none of it (needModuleTramp is false), so an ordinary program pays
 * nothing. Dispatch is a plain table lookup: no dlopen, no cross-module ABI, because all entries live
 * in one compilation unit. `-m x a b` collapses a b into a new argv in place (argv[0] is still the
 * program name), so the entry sees the same command line as when it is the main program itself.
 *
 * @param g  The code generation state; moduleEntries has been filled in by scanModuleEntries.
 */
static void emitModuleTramp(CG *g) {
    cgLine(g, "/* ---- module dispatcher: ./a -m <name> args... ---- */");
    cgLine(g, "typedef struct ExtcModuleEntry { const char *name; const char *desc; int (*fn)(int, char **); } ExtcModuleEntry;");
    cgLine(g, "static const ExtcModuleEntry extc_module_entries[] = {");
    for (size_t i = 0; i < g->moduleEntries.len; i++) {
        ModEntry *e = (ModEntry *)vecAt(&g->moduleEntries, i);
        Buf row;
        bufInit(&row, g->arena);
        bufPuts(&row, "    { ");
        emitStringToken(&row, e->name);
        bufPuts(&row, ", ");
        emitStringToken(&row, e->desc);
        bufPrintf(&row, ", %s },", cgEntryCName(g, e->f));
        cgLine(g, "%s", bufCstr(&row));
    }
    cgLine(g, "};");
    cgLine(g, "static const size_t extc_module_count = sizeof(extc_module_entries) / sizeof(extc_module_entries[0]);");
    cgLine(g, "");
    cgLine(g, "static void extc_module_usage(const char *prog) {");
    cgLine(g, "    printf(\"usage: %%s [-m <module>] [args...]\\n\", prog);");
    cgLine(g, "    printf(\"       %%s --help\\n\", prog);");
    if (g->hasMainEntry)
        cgLine(g, "    printf(\"\\nWithout -m it runs the program's own entry.\\n\");");
    else
        cgLine(g, "    printf(\"\\nPass -m to pick one of the compiled-in modules.\\n\");");
    cgLine(g, "    printf(\"\\nmodules:\\n\");");
    cgLine(g, "    for (size_t i = 0; i < extc_module_count; i++)");
    cgLine(g, "        printf(\"  -m %%-16s %%s\\n\", extc_module_entries[i].name, extc_module_entries[i].desc);");
    cgLine(g, "}");
    cgLine(g, "");
    cgLine(g, "int main(int argc, char **argv) {");
    cgLine(g, "    if (argc > 1 && (strcmp(argv[1], \"--help\") == 0 || strcmp(argv[1], \"-h\") == 0)) {");
    cgLine(g, "        extc_module_usage(argv[0]);");
    cgLine(g, "        return 0;");
    cgLine(g, "    }");
    cgLine(g, "    if (argc > 1 && strcmp(argv[1], \"-m\") == 0) {");
    cgLine(g, "        if (argc < 3) {");
    cgLine(g, "            fprintf(stderr, \"%%s: -m needs a module name\\n\", argv[0]);");
    cgLine(g, "            extc_module_usage(argv[0]);");
    cgLine(g, "            return 2;");
    cgLine(g, "        }");
    cgLine(g, "        for (size_t i = 0; i < extc_module_count; i++)");
    cgLine(g, "            if (strcmp(argv[2], extc_module_entries[i].name) == 0) {");
    cgLine(g, "                for (int k = 3; k < argc; k++) argv[k - 2] = argv[k];");
    cgLine(g, "                return extc_module_entries[i].fn(argc - 2, argv);");
    cgLine(g, "            }");
    cgLine(g, "        fprintf(stderr, \"%%s: unknown module '%%s'\\n\", argv[0], argv[2]);");
    cgLine(g, "        extc_module_usage(argv[0]);");
    cgLine(g, "        return 2;");
    cgLine(g, "    }");
    if (g->hasMainEntry)
        cgLine(g, "    return __extc_entry_main(argc, argv);");
    else {
        cgLine(g, "    extc_module_usage(argv[0]);");
        cgLine(g, "    return 2;");
    }
    cgLine(g, "}");
    cgLine(g, "");
}

/* Emit the forward declaration of one function, so that definitions may appear
 * in any order.
 */
/**
 * @brief Emit the trampoline of a `parallel::run` worker and its ctx type.
 *
 * A worker's parameters are `(id, lo, hi, views...)`, exactly one `mut slice<T>` of which is the
 * output: the trampoline slices it into a `sub` that belongs to this worker alone by `[lo, hi)`, and
 * the other views are shared as they are -- nobody writes them, so they need no partitioning. The
 * ctx is a struct with one field per view, passed by value.
 *
 * When the worker needs a home arena (its body allocates), one thread-local arena is appended as the
 * last parameter: its allocations cannot escape this call, and neither worker contends for the main
 * thread's frame arena.
 *
 * @param g  The code generation context.
 * @param f  The worker function; the first three parameters are `id`/`lo`/`hi` and all others are
 *           views.
 */
static void genParTramp(CG *g, FuncDef *f) {
    size_t nCaps = f->params.len - 4, outIdx = 0;
    for (size_t i = 0; i <= nCaps; i++)
        if ((*(Param **)vecAt(&f->params, 3 + i))->type->mut) outIdx = i;
    {   /* the ctx type: fields v0..vN whose types are the C names of the parameters (views and scalars
     * follow the same naming rule as in the cType notes) */
        Buf ct;
        bufInit(&ct, g->arena);
        bufPrintf(&ct, "typedef struct {");
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&ct, " %s v%zu;",
                      cType(g, (*(Param **)vecAt(&f->params, 3 + i))->type), i);
        bufPrintf(&ct, " } __extc_parctx_%s;", f->name);
        cgLine(g, "%s", bufCstr(&ct));
    }
    Type *vt = (*(Param **)vecAt(&f->params, 3 + outIdx))->type;
    const char *vn = cType(g, vt);
    const char *ctn = arenaPrintf(g->arena, "__extc_parctx_%s", f->name);
    /* A worker of a two-dimensional fold writes `out[r * cols + c]` in absolute row numbers, and
     * slicing the range would shift them, so that shape passes the whole output view
     * (`planParFullOut`). */
    bool full = planParFullOut(f);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp);", f->name);
    cgLine(g, "static int64_t __extc_par_%s(int64_t id, int64_t lo, int64_t hi, void *ctxp) {", f->name);
    cgLine(g, "    %s *c = (%s *)ctxp;", ctn, ctn);
    if (!full)
        cgLine(g, "    %s sub = { .data = c->v%zu.data + lo, .len = hi - lo };", vn, outIdx);
    {   /* arguments: the captures (views/scalars) as they are, the output one replaced by the sliced
     * sub (as it is in whole-view mode) */
        Buf args;
        bufInit(&args, g->arena);
        for (size_t i = 0; i <= nCaps; i++)
            bufPrintf(&args, "%s%s", i ? ", " : "",
                      (i == outIdx && !full) ? "sub" : arenaPrintf(g->arena, "c->v%zu", i));
        /* As soon as the worker body has a `new`, it takes one extra hidden home parameter (the
         * needsHome scheme, last parameter), so **the thread-local arena** is passed here: the worker
         * writes only its own piece of the output and returns only an i64 => its allocations cannot
         * escape this worker => the thread-local arena is safe, and neither worker contends for the
         * main thread's frame arena. */
        if (planUsesHome(f)) {
            bufPrintf(&args, "%sextc_tls_arena", ", ");
            /* The ownership-delegation flag is always 0: a thread-local arena's ownership is decided by the
             * run-time depth (`RC_OWN_TLS`), and delegation means "the owner hands the right to sweep to
             * the callee on the spot", which does not apply to the worker chain. */
            if (g->rc.reclaim) bufPuts(&args, ", 0");
        }
        cgLine(g, "    return (int64_t)%s(id, lo, hi, %s);", f->name, bufCstr(&args));
    }
    cgLine(g, "}");
}

/**
 * @brief Emit a function's forward declaration so that definition order does not matter.
 *
 * A coroutine gets no prototype under its own name: its product is `name$frame` and `name$step`,
 * emitted together, and the frame necessarily precedes every use; an undefined `name` prototype would
 * also break the later byte-offset scans -- `name` would match inside `name$step` and erase the text
 * in between. `@inline` must be written on both the prototype and the definition.
 *
 * @param g  The generator state; output and allocation come from it.
 * @param f  The function to declare; `main` and exported functions get no `static`.
 */
static void genFuncProto(CG *g, FuncDef *f) {
    if (planIsCoro(f)) return;
    Buf sig;
    bufInit(&sig, g->arena);
    bool entry = cgIsEntry(g, f);
    bufPrintf(&sig, "%s%s %s(%s);",
              entry ? (g->needModuleTramp ? "static " : "")
                    : (f->isExport ? "" : (f->isInline ? "EXTC_INLINE " : "static ")),
              entry ? "int" : cType(g, f->ret), cgEntryCName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
}

/**
 * @brief Emit a forward declaration for every function mentioned in a global variable's initializer.
 *
 * A function address is a constant, so `var t: api = api { alloc: hostAlloc }` is a legal global
 * initializer; but C requires the declaration to be seen first. Globals are emitted before the
 * prototype pool, so without this declaration `static api t = { .alloc = &hostAlloc };` reports
 * `hostAlloc` undeclared. The prototype pool emits it again later, and C allows repeated
 * declarations.
 *
 * @param ctx  The generator state (`CG *`); passed as `void *` to fit the access callback's
 *             signature.
 * @param e    The expression reached; only `EX_IDENT` nodes that resolve to a called function are
 *             handled.
 * @return     Always true, so the child traversal continues.
 */
static bool globalFnProto(void *ctx, Expr *e) {
    if (e->kind == EX_IDENT && planCallee(e)) genFuncProto((CG *)ctx, planCallee(e));
    return true;
}

/**
 * @brief Walk a statement's children and hand the function references found there to globalFnProto.
 *
 * @param ctx  The generator state (`CG *`), passed through to the expression callback.
 * @param s    The statement whose children are walked.
 * @return     astWalkStmtChildren's return value: whether the child traversal was not interrupted.
 */
static bool globalFnProtoStmt(void *ctx, Stmt *s) {
    AstVisit v = { globalFnProto, globalFnProtoStmt, ctx };
    return astWalkStmtChildren(s, &v);
}

