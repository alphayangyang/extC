/* ------------------------------------------------------- lambda function pointers ----
 * `fn(...)` is a bare code pointer. A lambda that captures nothing has an empty environment, so its
 * value can be that pointer; the environment's `call` method cannot be used directly, because its
 * first parameter is `self`. Each such literal therefore gets a **thunk**: a static function with
 * the lambda's own signature that calls `call` with a static empty environment. `cc -O2` inlines the
 * thunk, so the only cost left is the indirect jump every function-pointer call already has.
 *
 * The environment is empty by construction (the checker only accepts the conversion when no capture
 * was made), so the dummy is never read; it exists so that the callee is not handed a null pointer.
 * --------------------------------------------------------------------------------------- */

/**
 * @brief The C name of the thunk that turns a lambda's `call` method into a bare function pointer.
 *
 * @param g   The generator state (names are arena-allocated).
 * @param sd  The lambda's environment struct; it must have exactly one method.
 * @return    The thunk's C identifier, owned by `g->arena`.
 */
static const char *lamThunkName(CG *g, StructDef *sd) {
    /* A layout made for one instance (`LamVar`) has its own `call`, and therefore its own thunk: the
     * signature is the instance's, not the template's. */
    LamVar *v = lamVarFor(g, sd);
    if (v) return arenaPrintf(g->arena, "%s$fn", cFuncName(g, v->vcall));
    FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
    return arenaPrintf(g->arena, "%s$fn", cFuncName(g, md));
}

/**
 * @brief Print a thunk's parameter list: the lambda's own parameters, without the receiver.
 *
 * @param g   The generator state.
 * @param md  The lambda's `call` method (its first parameter is the environment).
 * @param b   The buffer to append to.
 */
static void lamThunkParams(CG *g, FuncDef *md, Buf *b) {
    if (md->params.len <= 1) { bufPuts(b, "void"); return; }
    for (size_t i = 1; i < md->params.len; i++) {
        Param *p = *(Param **)vecAt(&md->params, i);
        bufPrintf(b, "%s%s %s", i > 1 ? ", " : "", cType(g, p->type), p->cname);
    }
}

/**
 * @brief Emit one thunk definition.
 *
 * @param g   The generator state; output goes to the current buffer.
 * @param sd  The lambda's environment struct.
 */
static void emitLamThunk(CG *g, StructDef *sd) {
    FuncDef *md = *(FuncDef **)vecAt(&sd->methods, 0);
    Buf ps;
    bufInit(&ps, g->arena);
    lamThunkParams(g, md, &ps);
    bool vret = !md->ret || md->ret->kind == TY_VOID;
    cgLine(g, "");
    cgLine(g, "/* A `fn(...)` value for a lambda that captures nothing: the empty environment is a");
    cgLine(g, " * static dummy, so a call site never has to invent one. */");
    cgLine(g, "static %s %s(%s) {", vret ? "void" : cType(g, md->ret), lamThunkName(g, sd),
           bufCstr(&ps));
    {
        Param *selfP = *(Param **)vecAt(&md->params, 0);
        cgLine(g, "    static %s __env;", cType(g, selfP->type->inner));
    }
    Buf args;
    bufInit(&args, g->arena);
    for (size_t i = 1; i < md->params.len; i++) {
        Param *p = *(Param **)vecAt(&md->params, i);
        bufPrintf(&args, "%s%s", i > 1 ? ", " : "", p->cname);
    }
    if (vret)
        cgLine(g, "    %s(&__env%s%s);", cFuncName(g, md), args.len ? ", " : "", bufCstr(&args));
    else
        cgLine(g, "    return %s(&__env%s%s);", cFuncName(g, md), args.len ? ", " : "",
               bufCstr(&args));
    cgLine(g, "}");
}

/**
 * @brief Dispatch on the expression node and generate the C expression text for it.
 *
 * Every `case` returns a piece of C that can be embedded in an enclosing expression; an uncovered
 * node falls through to the trailing `"0"`, so a new Expr kind must add an arm here.
 *
 * @param g  The generator state: the arena and buffer come from it and errors are reported to it.
 * @param e  The expression node to generate; the caller guarantees it is not NULL.
 * @return   The generated C expression text, owned by `g->arena`.
 */
static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_EXT: return exExt(g, e);
        case EX_LAMBDA: {
            /* Used as a bare `fn(...)` value: the value is the code pointer of the thunk that wraps
             * the synthesised `call` method (the checker accepted this only for a lambda that
             * captures nothing, so the environment is empty). */
            if (planLamFnPtr(e) && e->u.lambda.sdef && e->u.lambda.sdef->methods.len == 1)
                return lamThunkName(g, e->u.lambda.sdef);
            /* The environment, built where the lambda is written: the checker puts the field values on the
             * node, so a struct literal is written by hand here. The by-name literal path is
             * unusable -- the lambda's type has no name in the source to look up. */
            StructDef *sd = e->type ? e->type->sdef : NULL;
            if (!sd) {
                ctxError(g->ctx, e->line, 1, NULL, "lambda: the checker left no environment type");
                return "0";
            }
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){", cType(g, e->type));
            for (size_t i = 0; i < e->u.lambda.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lambda.inits, i);
                bufPrintf(&b, "%s.%s = %s", i ? ", " : "", fi->name, genExpr(g, fi->value));
            }
            bufPuts(&b, "}");
            return bufCstr(&b);
        }
        case EX_INT:
            /* A literal above `i64`'s maximum is a `u64` by its own type (see the checker);
             * printing it as `%lld` would write a negative number that no longer has the
             * value. `%llu` is the same decimal the user wrote, and the C compiler gives an
             * unsuffixed literal that large the type `unsigned long long` by itself. */
            if ((unsigned long long)e->u.ival > 9223372036854775807ULL)
                return arenaPrintf(g->arena, "%lluULL", (unsigned long long)e->u.ival);
            return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT:
            /* An `f32` literal is written as the float it is: `%g` prints a `double`, and assigning that
             * to a `float` is an implicit narrowing that clang reports as
             * `-Wimplicit-float-conversion`. The value does not change; the cast only makes the
             * narrowing explicit. */
            {
                bool isF32 = e->type && e->type->kind == TY_BUILTIN && e->type->name &&
                             strcmp(e->type->name, "f32") == 0;
                /* A literal that overflows `double` (`1e400`) has no corresponding C literal, and `%g` would
                 * print it as `inf` -- in C that is an **undeclared identifier** (`-1e400` prints as
                 * `-inf` and gcc errors on it). `__builtin_inf()` / `__builtin_nan("")` say the same
                 * thing without pulling in `<math.h>`: that include would change the bytes of every
                 * output file. */
                const char *special = NULL;
                if (e->u.fval != e->u.fval)                       special = "__builtin_nan(\"\")";
                else if (e->u.fval != 0.0 && e->u.fval * 0.5 == e->u.fval)
                    special = e->u.fval < 0 ? "-__builtin_inf()" : "__builtin_inf()";
                if (special)
                    return isF32 ? arenaPrintf(g->arena, "(float)%s", special) : (char *)special;
                /* It must **round-trip**, so `%g` is not usable: `%g` keeps six significant digits and would
                 * silently change the value of any literal needing more (`4294967295.0` prints as
                 * `4.29497e+09`, so `x - 4294967296.0` yields 0 instead of -1, and the conversion of
                 * that literal then traps just outside the bound of the range it actually lies in).
                 * 17 digits round-trip a `double` and 9 digits round-trip a `float` (the cast below
                 * is what turns an f32 literal into a float, so its digit count only has to survive
                 * that step). `%g` still drops trailing zeros, so ordinary literals (`2.5`, `0.5`)
                 * are unchanged. */
                if (isF32) return arenaPrintf(g->arena, "(float)%.9g", e->u.fval);
                /* An integral-valued `f64` prints with no decimal point and no exponent (`1.0` -> `1`, `-0.0`
                 * -> `-0`), and in C that is an *integer* constant: the literal changes type, so
                 * `1.0 / 2.0` becomes integer division `(1 / 2)` = 0, `1e9 * 1e9` overflows `int` and
                 * `-0.0` loses its sign (`1.0 / z` prints `inf` instead of `-inf`), all with no
                 * diagnostic. Force a floating constant when the text itself is not one. The f32
                 * branch above is unaffected: its `(float)` cast already fixes the type. */
                {
                    char *txt = arenaPrintf(g->arena, "%.17g", e->u.fval);
                    if (!strpbrk(txt, ".eE"))
                        txt = arenaPrintf(g->arena, "%s.0", txt);
                    return txt;
                }
            }
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:
            /* `"abc"` becomes a byte view of the literal in read-only memory.
             * The length comes from `sizeof("...") - 1`, so C handles escapes
             * and nothing has to parse them here. */
            return arenaPrintf(g->arena,
                "(%s){ .data = (uint8_t *)\"%s\", .len = sizeof(\"%s\") - 1 }",
                cType(g, e->type), e->u.str.text, e->u.str.text);
        case EX_IDENT: return exIdent(g, e);

        /* `*p`: an explicit dereference is a C dereference; whether the target
         * may be written is the checker's business. */
        case EX_DEREF:
            return arenaPrintf(g->arena, "(*(%s))", genExpr(g, e->u.deref.operand));

        case EX_BIN: return genBin(g, e);

        case EX_UN:
            /* `-a` / `~a` on a narrow type promotes to `int` in C; the result must come back to the
             * operand's declared width, same rule as the binary operators (). `!` has
             * type `bool`, which `narrowWrap` leaves alone. */
            return narrowWrap(g, e->type,
                              arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand)));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: return exCall(g, e);

        case EX_INDEX: return exIndex(g, e);

        case EX_ARRAYLIT: {
            Type *t = e->type;
            if (!t || t->kind != TY_ARRAY) return "0";
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .data = {", cType(g, t));
            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
            }
            /* A trailing `...` needs nothing extra: a C initializer zero-fills
             * the remaining elements by itself. */
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }

        case EX_SLICE: return genSlice(g, e);

        case EX_TRY:
            /* `?` must not reach here: it expands at statement level, where
             * genStmt handles it directly. Arriving here means the checker's
             * position rule has a hole; report it rather than silently generate
             * wrong C. */
            ctxError(g->ctx, e->line, 1, NULL,
                     "internal: `?` reached expression codegen (position check missed it)");
            return "0";

        case EX_ASSOC: return exAssoc(g, e);

        /* Numeric conversion. Without `convCheck` the checker has proven the
         * value fits and a plain cast is enough; with it the value is checked
         * against a range and traps with a source position. */
        case EX_CONV: return exConv(g, e);

        /* `new T`, `new [N]T` and `new T[n]`: allocate into a block arena and
         * zero the storage. The runtime allocation zeroes on its own, so extC
         * has exactly one rule about fresh memory. */
        case EX_NEW: return exNew(g, e);

        case EX_GENCALL: return exGencall(g, e);

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_DYN:       return genDynValue(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);

        case EX_REF:
            return arenaPrintf(g->arena, "&(%s)", genExpr(g, e->u.ref.operand));

        /* `a ?? b`: the value if there is one, otherwise the fallback.
         *
         *   - A pure subject - a variable, field or index - becomes a C
         *     conditional directly: `x.tag == t_some ? x.u.some._0 : b`.
         *   - An impure subject, as in `f() ?? -1`, is marked `needTemp` by the
         *     checker and computed into a temporary first; flushPrefix emits
         *     those lines before the enclosing statement, which the
         *     `planNeedTemp(e)` branch below relies on.
         *
         * A C conditional evaluates only one side, so the fallback's side
         * effects do not run when the value is present. */
        case EX_COALESCE: return exCoalesce(g, e);

        /* `e!` asserts that the value is present -- the author signs for it, but
         * the signature itself is **checked at runtime**: the merit
         * of `!` is that it is short, not that it is silent. So:
         *   - `p!` on a `?ref T` calls `extc_refAssert` and yields the pointer;
         *   - `opt!` / `r!` call `extc_tagAssert` first and only then read the
         *     payload, so a `none` / failed arm traps with a position instead of
         *     handing back a zero value.
         * The helpers return what they checked, so the operand is evaluated once;
         * `opt!` / `r!` go one step further and become a call to a per-instance
         * **unwrap helper** (`unwrapHelper`), so the operand is a function argument
         * and is named exactly once. That is what lets `!` stand in a `while`
         * condition, where a statement-prefix temporary cannot live.
         * A function marked `@unchecked` gets the raw behaviour: it is the
         * one signed road to undefined behaviour (manual 0.5 / 12.5). */
        case EX_SIGN: {
            Type *ot = subst(g, e->u.sign.operand->type);
            bool isRef = ot && ot->kind == TY_REF;
            bool isOpt = !isRef && isProtoType(ot, "option", 1);
            const char *var = isOpt ? "some" : "success";
            if (g->uncheckedBody) {
                const char *v = genExpr(g, e->u.sign.operand);
                if (isRef) return v;
                return arenaPrintf(g->arena, "(%s).u.%s._0", v, var);
            }
            /* `o! = v` writes the payload, so the comma expression below cannot be
             * used: C11 6.5.17p3 says its result is not an lvalue. The assertion goes
             * into the statement prefix (the same mechanism `??` uses) and the payload
             * itself is returned as the lvalue.
             *
             * A call operand (`f()!.field = v` on a `?ref T`) is not a C lvalue, so the
             * expression may not appear twice -- once in the assert, once in the
             * assignment: evaluate it into a temporary of the same type and assert that.
             * A place operand (`p! = v`) stays exactly as it is, because the payload must
             * remain the lvalue the assignment writes. */
            if (g->inAssignTarget) {
                const char *v = genExpr(g, e->u.sign.operand);
                if (!printArgIsPlace(e->u.sign.operand)) {
                    const char *tmp = arenaPrintf(g->arena, "__extc_sig%d", g->tmpSeq++);
                    pfLine(g, "%s %s = %s;", cType(g, e->type), tmp, v);
                    v = tmp;
                }
                if (isRef) {
                    pfLine(g, "(void)extc_refAssert((void *)(%s), \"%s\", %d);",
                           v, g->path, e->line);
                    return v;
                }
                pfLine(g, "(void)extc_tagAssert((int)(%s).tag, %s_%s, \"%s\", %d);",
                       v, cType(g, ot), var, g->path, e->line);
                return arenaPrintf(g->arena, "(%s).u.%s._0", v, var);
            }
            if (isRef) {
                /* A nullable reference is a plain pointer: the assert itself returns what
                 * it checked, so a single mention of the operand is already evaluated once
                 * and no helper is needed. */
                return arenaPrintf(g->arena,
                    "((%s)extc_refAssert((void *)(%s), \"%s\", %d))",
                    cType(g, e->type), genExpr(g, e->u.sign.operand), g->path, e->line);
            }
            const char *fn = unwrapHelper(g, ot, isOpt, e->type);
            return arenaPrintf(g->arena, "%s(%s, \"%s\", %d)",
                               fn, genExpr(g, e->u.sign.operand), g->path, e->line);
        }

        /* `null`, the zero value of a nullable reference, is a null pointer in
         * C. The checker has already proven that a null test narrows the value
         * before any use, so no runtime check is generated: what the compiler
         * can prove leaves no trace at runtime. */
        case EX_NULL:
            return nullValue(g, e->type);

        case EX_ENUMVAL: return exEnumval(g, e);
    }
    return "0";
}

/**
 * @brief Whether a value of this type may contain a view, so boxing it must copy the bytes it names.
 *
 * A view is a pointer plus a length: copying the view copies an address. An `any` owns its payload,
 * so the payload copied into the box has to be the data, not somebody else's address -- otherwise a
 * box returned from a function would name a buffer that died with the frame.
 *
 * The answer is computed from the **static** type at the boxing site, so types without views pay
 * nothing: code generation passes a constant to `extc_any_box`, and the memcpy path stays.
 *
 * @param g  The generator (field types are substituted in an instance's context).
 * @param t  The type of the value being boxed.
 * @return   true when the payload has to be deepened.
 */
static bool typeNeedsAnyDup(CG *g, Type *t) {
    t = subst(g, t);
    if (!t) return false;
    /* A borrow is tested **before** `ttBase` strips it: `?ref node` inside `struct node` is how a
     * recursive type is spelled, and stripping the reference would make the walk descend into the
     * same struct forever (the field of a field of a field ...). The borrow itself is never
     * deepened -- a box holding one is rejected at the boxing site by the checker. */
    if (t->kind == TY_REF) return false;
    t = ttBase(t);
    switch (t->kind) {
    case TY_ARRAY:
        return true;                       /* an array is stored inline; its elements may be views */
    case TY_GENERIC:
        if (ttIsViewType(t)) return true;  /* `slice<T>` / the byte view: the data is elsewhere */
        /* fall through: an instance is laid out like a struct */
        /* FALLTHROUGH */
    case TY_STRUCT: {
        StructDef *sd = t->sdef;
        for (size_t i = 0; sd && i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (typeNeedsAnyDup(g, fd->type)) return true;
        }
        return false;
    }
    case TY_ENUM:
        return true;                       /* a variant's payload may hold a view; be conservative */
    case TY_REF:
        return false;                      /* borrows are rejected at the boxing site, not deepened */
    /* kind-default: only views, arrays, aggregates and enums can carry bytes that live somewhere
     * else. A scalar, a function value or a borrow is copied as it stands, so the memcpy path is
     * right for every kind that reaches here -- and a future kind has to be looked at, because
     * "the bytes are the value" is exactly what this predicate decides. */
    default:
        return false;
    }
}

/**
 * @brief Emit the boxing of a value into an `any`.
 *
 * The value is evaluated once, into a temporary of its own type, and that temporary is copied by
 * `extc_any_box` into the arena the `any` owns its bytes in. Copying (rather than pointing at the
 * operand) is what makes the `any` a **value**: `var x: any = 1`, then `x = "abc"`, keeps working
 * because each assignment replaces the bytes.
 *
 * @param g     The generator.
 * @param e     The source expression (the one the checker marked with `planAnyBox`).
 * @param code  Its generated C value.
 * @return      The name of the `ExtcAny` temporary holding the boxed value.
 */
static const char *emitAnyBox(CG *g, Expr *e, const char *code) {
    Type *t = ttBase(subst(g, e->type));
    g->needAny = true;
    g->needRuntime = true;
    const char *tmp = arenaPrintf(g->arena, "__extc_any%d", g->tmpSeq++);
    /* The arena is the **home** one: the value a boxed `any` owns may outlive this frame (it can be
     * returned or stored), so the copy must not sit in a block that dies at the end of the
     * statement. The checker registers the site so this function is handed a home arena. */
    const char *src = arenaPrintf(g->arena, "__extc_anyc%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), src, code);
    pfLine(g, "ExtcAny %s = extc_any_box(%s, %s, (const void *)&%s, (int64_t)sizeof(%s), %d);",
           tmp, homeArg(g, ARENA_HOME), descRef(g, t), src, cType(g, t),
           typeNeedsAnyDup(g, t) ? 1 : 0);
    return tmp;
}

/**
 * @brief Emit the unboxing of an `any` into a concrete type.
 *
 * The check happens at run time and traps with a source position when the `any` holds another
 * type -- that is the whole of the "implicit unboxing" rule: the compiler believes the source, the
 * run time verifies it, and a wrong belief stops the program at the line that had it.
 *
 * @param g     The generator.
 * @param e     The `any` expression the checker marked with `planAnyTo`.
 * @param want  The type the context expects.
 * @param code  The generated C value of the `any`.
 * @return      The name of the temporary holding the unboxed value.
 */
static const char *emitAnyUnbox(CG *g, Expr *e, Type *want, const char *code) {
    g->needAny = true;
    g->needRuntime = true;
    const char *addr;
    if (printArgIsPlace(e)) {
        addr = arenaPrintf(g->arena, "&(%s)", code);
    } else {
        const char *t = arenaPrintf(g->arena, "__extc_anyb%d", g->tmpSeq++);
        pfLine(g, "ExtcAny %s = %s;", t, code);
        addr = arenaPrintf(g->arena, "&%s", t);
    }
    const char *tmp = arenaPrintf(g->arena, "__extc_anyv%d", g->tmpSeq++);
    pfLine(g, "%s %s = *((const %s *)extc_any_unbox(%s, %s, \"%s\", %d));",
           cType(g, want), tmp, cType(g, want), addr, descRef(g, want), g->path, e->line);
    return tmp;
}

/**
 * @brief Generate an expression in value position, adding an automatic dereference when needed.
 *
 * The checker treats a `ref T` in value position as `T` and stamps the node with `deref`; that
 * dereference is added here. So `let y = p` copies the value and `p + 1` is numeric addition,
 * while whether the target is writable still depends on the `ref` and `mut ref` in the type.
 *
 * Wrapping it around genExprInner is what makes the rule complete: every path that generates an
 * expression value (recursive calls included) goes through here, so no path can miss the
 * dereference.
 *
 * @param g  The generator state.
 * @param e  The expression node; must not be NULL (a caller that violates this has a bug).
 * @return   The generated C expression text, owned by `g->arena`.
 */
static const char *genExpr(CG *g, Expr *e) {
    /* Every caller passes an expression node: this value position has no optional AST field, so
     * NULL is a caller bug. The check documents the invariant for both readers and static
     * analyzers. */
    assert(e != NULL);
    const char *s = genExprInner(g, e);
    /* The two conversions the checker accepted around this expression. Both are marked on the
     * operand, not on a new node: the value keeps its own type and only the emission site knows
     * that a wrapping or unwrapping happens here. */
    if (planAnyBox(e)) {
        s = emitAnyBox(g, e, s);
    } else if (planAnyTo(e)) {
        s = emitAnyUnbox(g, e, subst(g, (Type *)planAnyTo(e)), s);
    }
    if (planDeref(e)) return arenaPrintf(g->arena, "*(%s)", s);
    return s;
}

