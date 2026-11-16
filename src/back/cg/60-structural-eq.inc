/* ------------------------------------------------------------- structural `==`
 *
 * Every type goes through the generic `extc_eq` plus its descriptor; no
 * `<T>_eq` function is derived per array type.
 *
 * `extc_eq(a, b, desc)` takes addresses, so each operand must be a place; an
 * operand that is not one is first stored in a temporary through the statement
 * prefix, for the same reason `println` does it.
 */
/**
 * @brief Return the address of an operand to pass to `extc_eq`.
 *
 * `extc_eq` takes addresses, so the operand must be a place. One that is not a place is copied
 * into a fresh temporary through the statement prefix, which is what makes the copy happen
 * before the enclosing statement runs.
 *
 * @param g  The generator; it supplies the temporary counter.
 * @param x  The operand expression.
 * @param t  The static type of `x`; it types the temporary.
 * @return A C expression of pointer-to-`t` type: `&(code)` for a place, otherwise the address
 *         of the temporary.
 */
static const char *eqOperand(CG *g, Expr *x, Type *t) {
    const char *code = genExpr(g, x);
    if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
    const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);
    return arenaPrintf(g->arena, "&%s", tmp);
}

/**
 * @brief Emit a structural equality test as one `extc_eq` call.
 *
 * Both sides are evaluated exactly once and in source order, temporaries included.
 *
 * @param g    The generator.
 * @param e    The binary `==` or `!=` expression.
 * @param arr  The static type of the operands, that is the type being compared.
 * @return The call expression; the caller negates it for `!=`.
 */
static const char *genEqCall(CG *g, Expr *e, Type *arr) {
    const char *l = eqOperand(g, e->u.bin.left, arr);
    const char *r = eqOperand(g, e->u.bin.right, arr);
    return arenaPrintf(g->arena, "extc_eq(%s, %s, %s)", l, r, descRef(g, arr));
}

/**
 * @brief Emit the equality adapter of a struct.
 *
 * The adapter connects the user-written `fn ==` to the `bool (*)(const void *, const void *)`
 * shape `extc_eq` expects. Only a struct needs one: its comparison is arbitrary user code that
 * can only be delegated to.
 *
 * Taking the address of an operand must follow the method signature exactly: each of the first
 * two parameters is passed by address when it is declared `ref`, and by value otherwise.
 *
 * @param g  The generator.
 * @param t  The struct type.
 * @param m  Its `==` method; its first two parameters are the operands.
 */
static void genEqAdapter(CG *g, Type *t, FuncDef *m) {
    const char *tn = cType(g, t);
    cgLine(g, "static bool %s_eqD(const void *a, const void *b) {", t->name);
    g->indent++;
    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    const char *a = p0->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)a", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)a", tn);
    const char *b = p1->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)b", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)b", tn);
    cgLine(g, "return %s(%s, %s);", cMethodName(g, t, m), a, b);
    g->indent--;
    cgLine(g, "}");
}

/* Array equality goes through genEqCall and the generic `extc_eq`, and struct
 * or slice equality is emitted where the user's method is called, in genBin
 * below.
 */

/* The single owner of the implicit trailing arguments (home arena / zone). Declared up here because
 * the operator path -- an operator is a method call too -- asks it from inside `genBin`, which sits
 * above the definition. */
static void cgImplicitArgs(CG *g, Buf *b, const FuncDef *callee, const Expr *site, size_t nargs,
                           bool forSignature);

/**
 * @brief Wrap an emitted integer expression back to its declared width when that width is
 *        narrower than C's `int`.
 *
 * The checker types `a + a` for `a: i8` as `i8`, but C promotes both operands to `int`, so the
 * arithmetic value and the stored value would disagree and `a + a == x` would be false. Narrow
 * arithmetic therefore wraps at its own width, the rule the 32/64-bit widths already get from
 * `-fwrapv`.
 *
 * Only 8- and 16-bit results are touched: wider types are handled by `-fwrapv`, and a
 * comparison's type is `bool`, so it never reaches here. The cast is free in C.
 *
 * @param g     The generator.
 * @param t     The declared result type; NULL and non-integer types pass through unchanged.
 * @param code  The C expression to wrap.
 * @return `code` cast to the declared type, or `code` unchanged when no wrap is needed.
 */
static const char *narrowWrap(CG *g, Type *t, const char *code) {
    t = t ? subst(g, t) : NULL;
    if (!t || t->kind != TY_BUILTIN || !t->name) return code;
    const char *nm = t->name;
    if (nm[0] != 'i' && nm[0] != 'u') return code;
    int bits = 0;
    for (const char *q = nm + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
    if (bits != 8 && bits != 16) return code;
    return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), code);
}

/**
 * @brief Emit a binary operation.
 *
 * An overloadable operator the checker already resolved carries the method to call in
 * `planCallee(e)`, which covers `==` `!=` and the ordering and arithmetic operators. The same
 * operators are emitted from here when the operand was a type parameter inside a generic:
 * `planNeedOp(e)` says the decision was deferred, and the instance context is what makes `T`
 * resolvable at that point. Array equality becomes one generic `extc_eq` call; integer division, modulo
 * and shifts go through the trapping helpers unless `@unchecked` is in effect.
 *
 * @param g  The generator.
 * @param e  The binary expression.
 * @return A C expression; the caller supplies the surrounding syntax.
 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;
    /* An array has no `sdef` and therefore no method: `==` and `!=` on arrays are emitted here
     * through `extc_eq`. The condition must not also require `!planNeedOp(e)`: a comparison
     * inside a generic is resolved at instantiation, and after substitution the element type
     * can well turn out to be an array - comparing two rows of a `slice<[6]i32>` is that case -
     * and adding it would report "`array_6_i32` needs to define `!=`" instead. */
    if (!planCallee(e)) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ANY && isEqualityOp(op)) {
            g->needAny  = true;
            g->needAnyEq = true;
            const char *l = eqOperand(g, e->u.bin.left, lt);
            const char *r = eqOperand(g, e->u.bin.right, lt);
            const char *call = arenaPrintf(g->arena, "extc_any_eq(%s, %s)", l, r);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
        if (lt && lt->kind == TY_ARRAY &&
            isEqualityOp(op)) {
            needEq(g, lt);                 /* register: needs to be comparable via extc_eq */
            const char *call = genEqCall(g, e, lt);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
    }

    if (planCallee(e) || planNeedOp(e)) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        FuncDef *m = planCallee(e)
                     ? planCallee(e)
                     : findOpMethod(g->tt, lt, op, ttBase(subst(g, e->u.bin.right->type)),
                                    strcmp(op, "!=") == 0 ? "==" : NULL);

        if (!m) {
            /* `typeSupportsOp` is the checker's own predicate for both halves, so the
             * operator rules are not decided again here. `m` is NULL and the lookup already
             * tried the `!=` -> `==` fallback, so a true answer here means "native". */
            if (typeSupportsOp(g->tt, lt, op, ttBase(subst(g, e->u.bin.right->type))))
                return arenaPrintf(g->arena, "(%s %s %s)",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right));

            ctxError(g->ctx, e->line, 1,
                     arenaPrintf(g->arena,
                        "`%s` inside a generic is checked at instantiation, not on the template -- the price of having no traits. "
                        "Add a `fn %s` to that type.", op, op),
                     "`%s` needs to define `%s`", cType(g, lt), op);
            return "0";
        }

        Param *p0 = *(Param **)vecAt(&m->params, 0);
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        /* Evaluation order: C leaves argument order unspecified (gcc goes right to left), and here one
         * operator is one call. So in `cout << "x" << a() << b()` b() would run first, and
         * `<< dl::close(h)` would release before use. **The left half is materialized into a
         * statement first**, so the side effects of every link happen in source order, left to right.
         *
         * The left half is not materialized when it is a place: its evaluation has no side effect,
         * and keeping its address is what makes the write parameters (such as `+=`) land on the
         * original object. A receiver that is a place still goes to selfOperandAsParam. */
        const char *l = genExpr(g, e->u.bin.left);
        Type *lT = subst(g, e->u.bin.left->type);
        const bool lIsPlace = printArgIsPlace(e->u.bin.left);
        const bool wantRef0 = p0->type && p0->type->kind == TY_REF;
        const bool haveRef0 = lT && lT->kind == TY_REF;
        if (!lIsPlace && g->prefixBlk > 0 && !(lT && lT->kind == TY_ARRAY)) {
            const char *tmp = arenaPrintf(g->arena, "__extc_op%d", g->tmpSeq++);
            pfLine(g, "%s %s = %s;", cType(g, lT), tmp, l);
            if (wantRef0 && !haveRef0)      l = arenaPrintf(g->arena, "&%s", tmp);
            else if (!wantRef0 && haveRef0) l = arenaPrintf(g->arena, "*(%s)", tmp);
            else                            l = tmp;
        } else {
            /* The left operand is the method receiver, so it obeys the receiver rule: a value
             * that is not a place, such as the result of a chained `<<`, has to be materialized. */
            l = selfOperandAsParam(g, e->u.bin.left, l, p0, e->u.bin.left->type);
        }
        const char *r = genExpr(g, e->u.bin.right);
        if (p1->type->kind == TY_REF) r = arenaPrintf(g->arena, "&(%s)", r);

        /* An operator is a method call, so it asks the same single owner for the hidden
         * arguments (`cgImplicitArgs`); the checker already decided the level for it
         * (`setOpCallArgs`). Both halves must be emitted, or the generated C calls the method
         * with too few arguments. */
        Buf cb;
        bufInit(&cb, g->arena);
        bufPrintf(&cb, "%s(%s, %s", cMethodName(g, e->u.bin.left->type, m), l, r);
        cgImplicitArgs(g, &cb, m, e, 2, false);   /* 2 = the two operands already emitted */
        bufPutc(&cb, ')');
        const char *call = bufCstr(&cb);
        return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
    }

    /* Division by zero and an over-wide shift are undefined behaviour in C, so
     * they trap with a source position instead of passing silently. Each
     * operand is evaluated exactly once, which is why they go through a helper
     * rather than a comma expression such as `(check(r), l op r)` that would
     * evaluate one operand twice.
     *
     * `@unchecked` signs the checks away: the raw C operator is emitted and the
     * helper is never named (so it is not even defined in the product). */
    if (!g->uncheckedBody) {
        Type *lt   = ttBase(subst(g, e->u.bin.left->type));
        bool  sint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'i';
        bool  uint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'u';

        if ((sint || uint) && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)) {
            const char *fn = strcmp(op, "/") == 0 ? (sint ? "extc_divI" : "extc_divU")
                                                  : (sint ? "extc_modI" : "extc_modU");
            const char *ity = sint ? "int64_t" : "uint64_t";
            return arenaPrintf(g->arena, "(%s)%s((%s)(%s), (%s)(%s), \"%s\", %d)",
                               cType(g, lt), fn, ity, genExpr(g, e->u.bin.left),
                               ity, genExpr(g, e->u.bin.right), g->path, e->line);
        }
        if ((sint || uint) && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)) {
            int bits = 0;
            for (const char *q = lt->name + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
            if (bits > 0)
                return narrowWrap(g, e->type,
                                  arenaPrintf(g->arena, "(%s %s extc_shiftCount((int64_t)(%s), %d, \"%s\", %d))",
                                              genExpr(g, e->u.bin.left), op,
                                              genExpr(g, e->u.bin.right), bits, g->path, e->line));
        }
    }

    return narrowWrap(g, e->type,
                      arenaPrintf(g->arena, "(%s %s %s)",
                                  genExpr(g, e->u.bin.left), op, genExpr(g, e->u.bin.right)));
}

/* C expression for the zero value of a type.
 *
 * Zero initialization cannot always be `{0}`: `str` is non-nullable, and `{0}`
 * turns a `const char *` into NULL, where `printf("%s", NULL)` is undefined
 * behaviour (glibc happens to print "(null)" and hides the mistake). A struct
 * that contains a `str` therefore spells its zero value out field by field.
 *
 * Returns:
 *   The C initializer expression.
 */
static const char *zeroValue(CG *g, Type *t);

/**
 * @brief Report whether a type contains a `ref`, directly or nested inside a struct.
 *
 * Such a type must not be zero-initialized with `{0}`, which would build a null reference.
 *
 * @param t  The type to inspect; NULL contains no reference.
 * @return true when a `ref` appears anywhere inside the type.
 */
static bool needsExplicitZero(Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    /* An enum's zero is one of its variants, not the integer 0. Naming that variant would
     * remove clang's `-Wimplicit-int-enum-cast`, at the cost of nesting the initializer one
     * level deeper, so the warning is documented and accepted instead. */
    if (t->kind == TY_ENUM || t->kind == TY_ARRAY) return false;
    if (t->kind != TY_STRUCT || !t->sdef) return false;
    for (size_t i = 0; i < t->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
        if (needsExplicitZero(fd->type)) return true;
    }
    return false;
}

/**
 * @brief Enter the substitution context of the fields of a generic instance.
 *
 * The fields must be substituted with that instance's own type arguments, never with whatever
 * context the caller happens to leave behind: `subst` would then return the type unchanged, and
 * zero-value generation would recurse into itself until the stack overflows.
 *
 * @param g      The generator; the previous context is saved into the out parameters.
 * @param sd     The generic struct definition.
 * @param targs  Its type arguments in this instance.
 * @return       The previous context, for `substLeave`.
 */
static SubCtx substEnterInst(CG *g, StructDef *sd, Vec *targs) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };
    g->substParams = &sd->typeParams;
    g->substArgs   = targs;
    return sv;
}

/**
 * @brief Enter the substitution context of a generic free function instance.
 *
 * Without it a `T` in the body of the instance - the `T` of `a == b`, say - stays `TY_PARAM`,
 * which either reports a false error or generates wrong C.
 *
 * @param g      The generator; the previous context is saved into the returned value.
 * @param f      The instance; without a template this changes nothing.
 * @return       The previous context, for `substLeave`.
 */
static SubCtx substEnterFunc(CG *g, FuncDef *f) {
    SubCtx sv = { g->substParams, g->substArgs, g->ownerPrefix };
    if (f && planTemplate(f)) {
        g->substParams = &planTemplate(f)->typeParams;
        g->substArgs   = &f->targs;
    }
    return sv;
}

/* Emit the zero value of a type.
 *
 * This is the recursive implementation; the declaration above records why a
 * `{0}` is not always good enough.
 */
/**
 * @brief Emit a null pointer with the type the context expects.
 *
 * `((void *)0)` is a fine null in C, but assigning it to a typed pointer is an implicit `void *`
 * conversion that clang's `-Weverything` reports; `(node *)0` says the same thing in the type
 * the reader expects.
 *
 * @param g  The generator.
 * @param t  The reference type; a non-reference type yields the untyped null.
 * @return `((inner *)0)` for a reference to `inner`, `((void *)0)` otherwise.
 */
static const char *nullValue(CG *g, Type *t) {
    if (t && t->kind == TY_REF && t->inner) return arenaPrintf(g->arena, "((%s *)0)", cType(g, t->inner));
    return "((void *)0)";
}

/**
 * @brief Return the braces a zero value of type `t` needs when it sits inside another
 *        initializer.
 *
 * An array type is a struct wrapping `T data[N]`, so it brings two levels here (its own braces
 * and the member array's); a struct brings one. The outermost level costs nothing: it is the
 * compound literal's own brace, which is why `zeroValue` writes `(T){ { ... } }` for an array.
 *
 * The counts answer gcc's `-Wmissing-braces`, which `-Wall` enables and which asks for a brace
 * per aggregate; clang accepts any of them.
 *
 * @param g  The generator.
 * @param t  The element type; NULL yields the scalar `0`.
 * @return A C initializer fragment for one element of type `t`.
 */
static const char *elemBraces(CG *g, Type *t) {
    if (!t) return "0";
    if (t->kind == TY_ARRAY) return arenaPrintf(g->arena, "{ { %s } }", elemBraces(g, t->inner));
    /* An enum with payload is a `struct { tag; union }` in the generated C, so it brings a brace
     * like any other struct: `option<i64>` as an element needs `{ { { 0 } } }` around it. */
    if (t->kind == TY_ENUM && enumHasPayload(t->edef)) return "{ 0 }";
    if ((t->kind == TY_STRUCT || t->kind == TY_GENERIC) && t->sdef && t->sdef->fields.len > 0) {
        FieldDef *f0 = *(FieldDef **)vecAt(&t->sdef->fields, 0);
        if (f0->type && (f0->type->kind == TY_ARRAY ||
                         ((f0->type->kind == TY_STRUCT || f0->type->kind == TY_GENERIC) &&
                          f0->type->sdef && f0->type->sdef->fields.len > 0)))
            return arenaPrintf(g->arena, "{ %s }", elemBraces(g, f0->type));
        return "{ 0 }";        /* a struct that starts with a scalar: one brace, then the zero */
    }
    return "0";                /* scalar, enum, bool, a reference: the element itself */
}

/**
 * @brief Emit the zero value of a type.
 *
 * `{0}` is not always a valid initializer: a type that carries a non-nullable `ref` would get a
 * null reference, so it is spelled out field by field instead. A generic instance is always
 * spelled out, because its field types have to be substituted with that instance's own type
 * arguments.
 *
 * @param g  The generator.
 * @param t  The type to zero-initialize; it is substituted before it is inspected.
 * @return A C initializer expression allocated in the generator's arena.
 */
static const char *zeroValue(CG *g, Type *t) {
    /* Substitute first, exactly as `cType` does: `t` can still carry the template type of a
     * generic function (`box<T>`, whose `targs` is `[T]`), and the instance context below would
     * substitute `T` with `T`, leaving the initializer named after an undeclared type while the
     * declaration beside it names the instance. */
    t = subst(g, t);
    if (!t) return "0";
    /* The zero value of an enum with payloads is tag 0 with a cleared payload,
     * which is exactly what `(shape){0}` means: C clears the tag and the whole
     * union, and the tag decides which member is the meaningful one. A `ref`
     * inside the payload of tag 0 is reported by the checker as a type that
     * cannot be zero-initialized. */
    if (t->kind == TY_ENUM)
        return enumHasPayload(t->edef) ? arenaPrintf(g->arena, "(%s){0}", t->name) : "0";
    /* An array is a struct wrapping `data[N]`, so its braces have to follow the nesting or gcc
     * asks for them (`-Wmissing-braces`; clang takes any of these). The element's zero value is
     * the ordinary one, already braced for structs by the cases below. */
    if (t->kind == TY_ARRAY)
        return arenaPrintf(g->arena, "(%s){ { %s } }", t->name, elemBraces(g, t->inner));

    if (t->kind == TY_PARAM) {
        Type *a = subst(g, t);
        if (a == t) return "0";     /* no context: cannot happen, but never loop forever */
        return zeroValue(g, a);
    }

    /* A generic instance substitutes its own type arguments into the field
     * types and then spells the zero value out field by field. */
    if (t->kind == TY_GENERIC && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", cType(g, t));

        SubCtx sv = substEnterInst(g, sd, &t->targs);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", cType(g, t));
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");

        substLeave(g, sv);
        return bufCstr(&b);
    }

    if (t->kind == TY_STRUCT && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0 || !needsExplicitZero(t))
            return arenaPrintf(g->arena, "(%s){0}", sd->name);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", sd->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");
        return bufCstr(&b);
    }

    if (ttIs(t, "bool")) return "false";

    /* Defensive: a non-nullable `ref` has no zero value, so an identifier that does not exist
     * is emitted and the C compiler reports it instead of a null reference being inserted
     * quietly. The checker refuses zero-initialization of a struct that contains such a field,
     * so this is unreachable unless another path slips through. A `?ref T` does have one: `null`. */
    if (t->kind == TY_REF) return t->nullable ? nullValue(g, t)
                                              : "__extc_reference_has_no_zero_value__";

    /* Same defensive shape as the `ref` case, for a stronger reason: the `0` that would fall out
     * below is a null function pointer, and calling one jumps to address zero. The checker
     * refuses to zero-initialize a `fn`, so this is unreachable; the C compiler names the
     * missing identifier instead. */
    if (t->kind == TY_FN) return "__extc_fn_has_no_zero_value__";

    return "0";
}

/**
 * @brief Return the zero value of a type as an initializer.
 *
 * @param g  The generator.
 * @param t  The type to zero-initialize.
 * @return The initializer expression produced by `zeroValue`.
 */
static const char *zeroInit(CG *g, Type *t) {
    return zeroValue(g, t);
}

/**
 * @brief Report whether the generated C expression of a `println` argument is an lvalue.
 *
 * This is the one owner of that question, and it answers for the C form, not the extC form: the
 * slice expression `s[0..5]` is a place in extC, but its C form is a function call, and `&` on
 * a call is illegal; an enum variant such as `color.red` has already been replaced by a
 * constructor. `selfOperandAsParam` asks the same question.
 *
 * @param e  The argument expression.
 * @return false whenever the answer is uncertain: a wrong false merely costs one extra copy,
 *         while a wrong true produces C that does not compile.
 */
static bool printArgIsPlace(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;                                  /* root of `x` / `p.f` */
    case EX_FIELD: return printArgIsPlace(e->u.field.obj);
    case EX_INDEX: return printArgIsPlace(e->u.index.obj);
    default:       return false;
    }
}

/**
 * @brief Tell whether a call's callee name is one of the print builtins.
 *
 * The builtin spellings are bare (`print` / `println`); the module-qualified spellings
 * (`io::print`, or the mangled `io$print` a qualified name can arrive as) mean the same thing.
 * Comparing the last component keeps the two spellings one construct.
 *
 * @param name  Callee name as written; may carry a module part.
 * @param want  Builtin name to compare against (`print` / `println` / `flush`).
 * @return      `true` when the last component of `name` is `want`.
 */
static bool builtinNameIs(const char *name, const char *want) {
    if (!name) return false;
    const char *tail = name;
    const char *sep = strstr(name, "::");
    if (sep && sep[2]) {
        tail = sep + 2;
    } else {
        const char *dollar = strrchr(name, '$');
        if (dollar && dollar[1]) tail = dollar + 1;
    }
    return strcmp(tail, want) == 0;
}

/**
 * @brief Emit the expression that implements one `print` call.
 *
 * A structured argument - enum, byte view, struct, generic instance or array - is printed
 * through its descriptor as `extc_print(&x, &x_desc)`, so per-type print code disappears and
 * one descriptor per type is left. That call needs an address, so an argument that is not a
 * place is first stored in a temporary through the statement prefix. A C compound literal
 * cannot be used instead: C refuses to initialize an aggregate from an expression of the same
 * type.
 *
 * @param g        The generator.
 * @param args     The argument expressions of the call.
 * @param newline  Append a trailing newline as well.
 * @return The generated expression, allocated in the generator's arena.
 *
 * Sets `needRuntime` and `needPrint`, which pull the descriptor table and `extc_print` into the
 * output.
 */
static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    /* Built-in `print`/`println` and `io::cout` go through the **same buffer** (`extc_printf` /
     * `extc_cout_put`), so mixed writes keep source order by construction and need no `fflush`.
     * Only structured types (enum/struct/array/non-byte view) still go to the descriptor printer,
     * which writes the same buffer. */
    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(subst(g, a->type));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt) { bufPuts(&b, "0"); continue; }

        if (bt->kind == TY_BUILTIN) {
            g->needCout = true;
            if (strcmp(bt->name, "bool") == 0) {
                bufPrintf(&b, "extc_printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
                continue;
            }
            const PrintFmt *pf = NULL;
            for (size_t k = 0; PRINT_FMT[k].extc; k++)
                if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
            if (!pf) { bufPuts(&b, "0"); continue; }
            bufPrintf(&b, "extc_printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
            continue;
        }

        /* byte views (`slice<u8>` and string literals): write these bytes directly instead of printing element by element */
        if (isByteView(bt)) {
            g->needCout = true;
            const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
            pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
            bufPrintf(&b, "extc_cout_put((uint8_t *)(%s).data, (%s).len)", tmp, tmp);
            continue;
        }

        /* An `any` prints through its own descriptor, which is exactly the type information the
         * program carries at that point; the value may be an empty slot, and that reads as
         * `<none>` rather than nothing at all. */
        if (bt->kind == TY_ANY) {
            g->needAny     = true;
            g->needRuntime = true;
            g->needPrint   = true;
            g->needCout    = true;
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_any_print(&(%s))", code);
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "ExtcAny %s = %s;", tmp, code);
                bufPrintf(&b, "extc_any_print(&%s)", tmp);
            }
            continue;
        }

        if (bt->kind == TY_ENUM || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;
            g->needPrint   = true;
            /* The descriptor printer writes the cout buffer too (`extc_print` is all `extc_printf`) =>
             * that part of the runtime must be emitted with it, or `extc_printf` has no definition. */
            g->needCout    = true;
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_print(&(%s), %s)", code, descRef(g, bt));
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
                bufPrintf(&b, "extc_print(&%s, %s)", tmp, descRef(g, bt));
            }
            continue;
        }
        bufPuts(&b, "0");
    }

    g->needCout = true;
    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "extc_cout_put((uint8_t *)\"\\n\", 1)");
    } else if (args->len == 0) {
        bufPuts(&b, "extc_cout_put((uint8_t *)\"\", 0)");
    }

    bufPutc(&b, ')');
    return bufCstr(&b);
}

static const char *homeArg(CG *g, int marked);   /* defined below */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome);
