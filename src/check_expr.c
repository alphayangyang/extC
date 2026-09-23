/* Expression checking: the type of every expression, and the checks that only the
 * shape of an expression can decide.
 *
 * This is the recursive core of the checker. It resolves names to bindings, rewrites a
 * few nodes in place (a variant access becomes an enum value, a generic call becomes a
 * plain call), and decides the arena level of every allocation site. The lifetime and
 * borrow rules themselves live in check_escape.c.
 */

#include <stdlib.h>
#include "check_internal.h"

/* ---------------------------------------------------------------- expressions */

Type *checkExpr(Checker *c, Expr *e);

static bool exprHasCall(Checker *c, Expr *e);   /* defined at the end of this file */

/* Compute the result type of a binary arithmetic operator.
 *
 * Params:
 *   c  - checker
 *   e  - the EX_BIN node being checked, for its operator and source position
 *   lt - type of the left operand
 *   rt - type of the right operand
 *
 * Returns:
 *   The common type of the two operands, or the error type after a diagnostic.
 *
 * Notes:
 *   - A literal operand adapts to the other side, so `u32 + 1` has type `u32`.
 *   - `%` is restricted to integers here.
 *   - The result rule lives only in this function on purpose: the bitwise operators use
 *     it too, and a second copy would be free to drift away from it.
 *   - Reference-typed operands never reach this function; the caller reports them first.
 */
static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;
    if (!ttIsNumeric(lt) || !ttIsNumeric(rt)) {
        ckError(c, e->line, "arithmetic operators only accept numeric types",
                "cannot apply `%s` to `%s` and `%s`", op, typeStr(c, lt), typeStr(c, rt));
        return err;
    }
    if (strcmp(op, "%") == 0 && (!ttIsInteger(lt) || !ttIsInteger(rt))) {
        ckError(c, e->line, "`%` is only meaningful for integers",
                "cannot apply `%%` to `%s` and `%s`", typeStr(c, lt), typeStr(c, rt));
        return err;
    }

    /* A literal adapts to the other operand by value: in `u32 + 1` the `1` is a u32. */
    if (isNumericLit(e->u.bin.left) && ttIsNumeric(rt)) {
        if (!literalFits(e->u.bin.left, rt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, rt));
            return err;
        }
        return rt;
    }
    if (isNumericLit(e->u.bin.right) && ttIsNumeric(lt)) {
        if (!literalFits(e->u.bin.right, lt)) {
            ckError(c, e->line, NULL, "literal does not fit in `%s`", typeStr(c, lt));
            return err;
        }
        return lt;
    }

    if (ttEquals(lt, rt)) return lt;
    if (ttCanWiden(lt, rt)) return rt;
    if (ttCanWiden(rt, lt)) return lt;

    ckError(c, e->line, "these two types have no lossless conversion; convert explicitly first",
            "`%s` and `%s` have no common type for `%s`",
            typeStr(c, lt), typeStr(c, rt), op);
    return err;
}

/* Report whether `op` is one of the bitwise operators `&`, `|`, `^`, `<<`, `>>`.
 *
 * The result type of these operators comes from `checkArith`, so the rule is not copied
 * here and the two cannot drift apart.
 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

/* Report whether a type is a reference, nullable or not. */
static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/* Report an arithmetic or comparison operator applied to a reference.
 *
 * In C this would silently become pointer arithmetic, which is not what the user wrote.
 * Reporting an error is preferred over an answer that looks right and moves a pointer.
 *
 * Params:
 *   c  - checker
 *   e  - the EX_BIN node being checked, for its source position
 *   lt - type of the left operand
 *   rt - type of the right operand
 *   op - the operator spelling, for the message
 *
 * Returns:
 *   The error type, so the caller can return it directly.
 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

/* Check one expression and return its type.
 *
 * Params:
 *   c - checker
 *   e - the expression node
 *
 * Returns:
 *   The type of the expression, or the error type once a diagnostic was reported.
 *
 * Notes:
 *   - This is the recursive core; `checkExpr` wraps it with the per-statement
 *     bookkeeping that every subexpression needs.
 *   - Some nodes are rewritten in place while they are checked, so the caller must not
 *     rely on `e->kind` staying what it was on entry.
 */
static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:   return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_NULL: {
            /* The type was parked on `e->type` by `adoptContextType`. If the context did
             * not say which `?ref T` this stands for, report it rather than guess: extC is
             * explicit instead of inferring. */
            Type *nt = e->type;
            if (nt && nt->kind == TY_REF && nt->nullable) return nt;
            ckError(c, e->line,
                    "`null` is the zero value of a nullable reference, so its type must be"
                    " clear from context -- e.g. `var p: ?ref node = null`, or a parameter"
                    " or return type of `?ref T`",
                    "`null` here does not know which `?ref T` it is");
            return ttError(tt);
        }

        case EX_IDENT: {
            Sym *s = lookup(c, e->u.ident.name);
            if (s && s->modName && !e->qualified)
                requireQualified(c, e->u.ident.name, s->modName, false, e->line);
            if (!s) {
                /* A bare variant name, with no payload. After `type st = | ok | bad`,
                 * writing `let s: st = ok` would otherwise report `undefined name `ok``,
                 * which does not show that the qualified form `st.ok` is wanted. A variant
                 * is named by its type, so this gets the same guidance as a bare payload
                 * constructor does. */
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, e->u.ident.name)) owner = td;
                }
                if (owner) {
                    const char *note = arenaPrintf(c->arena,
                            "A variant is named by its type -- write `%s.%s`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), e->u.ident.name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a value -- did you mean `%s.%s`?",
                            e->u.ident.name, DN(owner), DN(owner), e->u.ident.name);
                    return ttError(tt);
                }
                ckError(c, e->line, "every name must be declared first (extC has no globals yet)",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* Name resolution is frozen here: codegen prints `cname` directly. This is
             * what tells shadowed names apart (`a` from `a__2`). */
            e->u.ident.cname = s->cname;
            /* Pin the resolved binding itself onto the node. The solving pass at the end
             * of the run happens after every function body has been checked, when the
             * scopes are already popped, so looking the name up again would either fail or
             * find a different binding that happens to share the name. See `IdentBinding`. */
            {
                IdentBinding *ib = (IdentBinding *)arenaAllocZero(c->arena, sizeof(IdentBinding));
                ib->sym = s;
                e->u.ident.sym = ib;
            }
            /* An `if p != null` test already proved this, so hand out the non-null
             * reference. Then `p.field`, `p.method()`, and passing it to a `ref T`
             * parameter all follow, and not one runtime check has to be generated. */
            Type *sty = s->type;
            if (sty && sty->kind == TY_REF && sty->nullable && isNarrowed(c, s->cname)) {
                Type *nn = ttRef(tt, sty->inner);
                nn->mut = sty->mut;
                return nn;
            }
            return sty;
        }

        case EX_BIN: {
            const char *op0 = e->u.bin.op;

            /* The short circuit of `&&` / `||` is a narrowing point too:
             * `p != null && p.value > 3` may dereference on the right, because reaching
             * the right side means the left side held. */
            if (isLogicOp(op0)) {
                Type *lt = checkValue(c, e->u.bin.left);
                expectBool(c, lt, e->u.bin.left);

                bool whenTrue = false;
                const char *tg = narrowTarget(c, e->u.bin.left, &whenTrue);
                const size_t mark = c->narrow.len;
                if (strcmp(op0, "&&") == 0) {
                    /* Reaching the right side means the left side was true, so every
                     * fact the left side proves holds here. */
                    narrowFactsOf(c, e->u.bin.left);
                } else if (tg && !whenTrue) {
                    /* `a || b`: reaching the right side means the left side was false.
                     * Only the simplest shape, `p == null`, yields "p is not null" when
                     * negated; the full negation of `||` would have to negate a
                     * conjunction, which is not implemented. */
                    pushNarrow(c, tg);
                }

                Type *rt = checkValue(c, e->u.bin.right);
                expectBool(c, rt, e->u.bin.right);
                c->narrow.len = mark;
                return c->tBool;
            }

            /* `p == null` / `p != null`: comparing against null is the only thing a
             * nullable reference is for. This has to run before the `checkValue` calls
             * below, because `null` does not know its own type. */
            if ((strcmp(op0, "==") == 0 || strcmp(op0, "!=") == 0) &&
                (e->u.bin.left->kind == EX_NULL || e->u.bin.right->kind == EX_NULL)) {
                Expr *nv = (e->u.bin.left->kind == EX_NULL) ? e->u.bin.left : e->u.bin.right;
                Expr *ov = (nv == e->u.bin.left) ? e->u.bin.right : e->u.bin.left;
                Type *ot = checkExpr(c, ov);
                if (ttIsError(ot)) return c->tBool;
                if (ot->kind == TY_REF && ot->nullable) {
                    adoptContextType(nv, ot);
                    checkExpr(c, nv);
                    return c->tBool;
                }
                if (ot->kind == TY_REF) {
                    ckError(c, e->line,
                            "only a nullable reference (`?ref T`) can be null -- a plain"
                            " `ref T` is guaranteed non-null, that is what it means",
                            "`%s` is not nullable, so it can never be `null`", typeStr(c, ot));
                    return c->tBool;
                }
                ckError(c, e->line,
                        "`null` is the zero value of a nullable reference, so compare it"
                        " with one: `if p != null { ... }`",
                        "`null` cannot be compared with `%s`", typeStr(c, ot));
                return c->tBool;
            }

            Type *lt = checkValue(c, e->u.bin.left);
            Type *rt = checkValue(c, e->u.bin.right);
            const char *op = e->u.bin.op;

            if (isCmpOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return c->tBool;

                bool isEqOp = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);

                /* `==` / `!=`: builtins compare natively; a struct goes through its
                 * `==` method. */
                if (isEqOp && ttEquals(lt, rt)) {
                    if (cmpIsNative(lt)) return c->tBool;

                    /* Arrays: `==` is derived by the compiler. The user cannot write an
                     * array type down, so it cannot be given an `==` method. */
                    if (lt->kind == TY_ARRAY) {
                        if (!typeSupportsEq(lt->inner, op))
                            ckError(c, e->line,
                                    "An array's `==` is derived by the compiler, "
                                    "so its elements have to be comparable.",
                                    "`%s` cannot be compared: its element type `%s` does not define `==`",
                                    typeStr(c, lt), typeStr(c, ttBase(lt)->inner));
                        return c->tBool;
                    }

                    /* A type parameter: defer the check until the instance is known. */
                    if (lt->kind == TY_PARAM) {
                        e->needEq = true;
                        /* Do not gate this on `c->curFunc->owner`: that would cover
                         * methods only. A generic free function has no owner, so the
                         * `T: ==` requirement would never be checked. Record the use and
                         * ask once per instance whether this `T` has `==`. */
                        if (c->curFunc) {
                            EqCheck *ec = (EqCheck *)arenaAllocZero(c->arena, sizeof(EqCheck));
                            ec->node = e;
                            ec->owner = c->curFunc->owner;   /* NULL for a free function */
                            ec->op = op;
                            ec->func = c->curFunc;     /* which function it belongs to */
                            *(EqCheck **)vecPush(&c->eqChecks) = ec;
                        }
                        return c->tBool;
                    }

                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (!sd) {
                        /* An enum with a payload is the type most likely to hit this: in
                         * C it is a struct, and C structs cannot be compared with `==`, so
                         * the message points the user at `match`. */
                        bool ep = b && b->kind == TY_ENUM && enumHasPayload(b->edef);
                        ckError(c, e->line,
                                ep ? "an enum with a payload is a tagged union -- "
                                     "compare it with `match`, or write a method that does"
                                   : NULL,
                                ep ? "`%s` is an enum with a payload, so it has no `%s`"
                                   : "`%s` does not support `%s`", typeStr(c, lt), op);
                        return c->tBool;
                    }

                    FuncDef *m = findOp(b, op, strcmp(op, "!=") == 0 ? "==" : NULL);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "define it inside `%s`:\n"
                                  "      fn ==(self: ref %s, other: %s) -> bool { ... }",
                                  DN(sd), DN(sd), DN(sd));
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==`, so it cannot be compared",
                                DN(sd));
                        return c->tBool;
                    }

                    Vec *sp = NULL, *sa = NULL;
                    if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                    (void)sp; (void)sa;
                    e->func = m;  m->used = true;   /* record that this method is used */
                    return c->tBool;
                }

                /* Every other comparison is meaningful for numbers only. */
                if (ttIsNumeric(lt) && ttIsNumeric(rt) &&
                    (ttCanWiden(lt, rt) || ttCanWiden(rt, lt))) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;

                ckError(c, e->line, isEqOp ? "only numbers, `bool`, enums, and structs that define `==` can be compared" : NULL,
                        "cannot compare `%s` with `%s`", typeStr(c, lt), typeStr(c, rt));
                return c->tBool;
            }
            if (isBitOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return ttError(tt);
                if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
                if (!ttIsInteger(lt) || !ttIsInteger(rt)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `%s` to `%s` and `%s`",
                            op, typeStr(c, lt), typeStr(c, rt));
                    return ttError(tt);
                }
            }
            /* The same trap as in the bitwise case: `ttIsNumeric` strips `ref T` down to
             * `T`, so `p + 1` for `p: ref i64` would pass as arithmetic while the generated
             * C performs pointer arithmetic -- silently doing something else entirely. */
            if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
            /* The result type comes from the arithmetic rule, literal fitting included;
             * no second copy of that rule here. */
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkValue(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
            /* `~` is meaningful for integers only. */
            if (strcmp(e->u.un.op, "~") == 0) {
                if (!ttIsInteger(ot)) {
                    ckError(c, e->line, "bitwise operators only accept integers",
                            "cannot apply `~` to `%s`", typeStr(c, ot));
                    return ttError(tt);
                }
                return ot;
            }
            if (!ttIsNumeric(ot)) {
                ckError(c, e->line, NULL, "cannot negate `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            return ot;
        }

        case EX_FIELD: {
            /* First decide whether this names an enum variant, as in `Status.warn`, where
             * `Status` is a type name rather than a variable. */
            if (e->u.field.obj->kind == EX_IDENT) {
                const char *tn   = e->u.field.obj->u.ident.name;
                const char *vn   = e->u.field.name;
                if (!lookup(c, tn)) {
                    Type *et = ttFromName(tt, tn);
                    if (et && et->kind == TY_ENUM) {
                        Variant *v = findVariant(et->edef, vn);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, vn);
                            return ttError(tt);
                        }
                        /* Rewrite into an enum value node; codegen consumes it directly. */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->assocOwner = et;      /* the resolved type; instances need it */
                        return et;
                    }
                }
            }

            Type *rawT = checkExpr(c, e->u.field.obj);
            if (rejectNullableDeref(c, rawT, e->u.field.obj, "field access")) return ttError(tt);
            Type *bt = ttBase(rawT);
            StructDef *sd = structOf(bt);
            if (!sd) {
                ckError(c, e->line, NULL, "`%s` is not a struct, so it has no field `%s`",
                        typeStr(c, bt ? bt : e->type), e->u.field.name);
                return ttError(tt);
            }
            FieldDef *fd = findField(sd, e->u.field.name);
            if (!fd) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "fields of %s:", DN(sd));
                for (size_t i = 0; i < sd->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", DN(sd), e->u.field.name);
                return ttError(tt);
            }
            e->field = fd;
            /* A field type may mention type parameters; substitute the receiver's type
             * arguments for them. */
            Type *ftype = (bt->kind == TY_GENERIC)
                          ? ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs)
                          : fd->type;
            /* Path narrowing: inside `if h.p != null { ... }` a read of `h.p` yields the
             * non-null version. `narrowTarget` records the fact only when the root is a
             * local whose address was never taken, which is exactly the condition relied on
             * here. This mirrors the `EX_IDENT` rule (`isNarrowed(c, s->cname)`) above. */
            if (ftype && ftype->kind == TY_REF && ftype->nullable &&
                e->u.field.obj->kind == EX_IDENT && e->u.field.obj->u.ident.cname) {
                const char *rc = e->u.field.obj->u.ident.cname;
                size_t kn = strlen(rc) + strlen(e->u.field.name) + 2;
                char *key = (char *)arenaAlloc(c->arena, kn);
                snprintf(key, kn, "%s.%s", rc, e->u.field.name);
                if (isNarrowed(c, key)) {
                    Type *nn = ttRef(tt, ftype->inner);
                    nn->mut = ftype->mut;
                    return nn;
                }
            }
            return ftype;
        }

        case EX_INDEX: {
            Type *ot = checkExpr(c, e->u.index.obj);
            Type *it = checkValue(c, e->u.index.index);
            if (ttIsError(ot) || ttIsError(it)) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, NULL,
                        "cannot index a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }
            if (!ttIsInteger(it)) {
                ckError(c, e->line, "An index must be an integer.",
                        "index must be an integer, found `%s`", typeStr(c, it));
                return ttError(tt);
            }

            /* A fixed array has a compile-time length, so a literal index that is out of
             * range is reported here instead of at runtime: what the compiler can prove, it
             * has to say. The rule matches the three slice bounds checked below, except that
             * a single half-open bound applies -- legal indices are [0, n). Both `b[-1]` and
             * `b[4]` for a `[4]T` are compile errors. */
            if (ob->kind == TY_ARRAY) {
                long long iv = 0;
                if (asIntLit(e->u.index.index, &iv)) {
                    long long n = (long long)ob->asize;
                    if (iv < 0 || iv >= n) {
                        ckError(c, e->line,
                                "The compiler can prove this index is out of range.",
                                "index %lld is not inside `%s` (length %lld)",
                                iv, typeStr(c, ot), n);
                        return ttError(tt);
                    }
                }
            }
            return elem;
        }

        case EX_SLICE: {
            /* `a[lo..hi]` is a view. Either bound may be absent: `lo` when the front is
             * omitted, `hi` when the back is. */
            Type *ot = checkExpr(c, e->u.slice.obj);
            if (ttIsError(ot)) return ttError(tt);

            Type *ob = ttBase(ot);
            Type *elem = NULL;
            if (ob && ob->kind == TY_ARRAY) elem = ob->inner;
            else                              elem = viewElemOf(ob);
            if (!elem) {
                ckError(c, e->line, "Only a fixed array or a view can be sliced.",
                        "cannot slice a value of type `%s`", typeStr(c, ot));
                return ttError(tt);
            }

            /* Slicing a fixed array takes the address of an element, so the base has to be
             * a place. Slicing a view is only pointer arithmetic on a value, which needs no
             * storage -- that is why a string literal can be sliced. */
            if (ob->kind == TY_ARRAY && !isPlace(e->u.slice.obj)) {
                ckError(c, e->line, "Slicing takes the address of an element, so the base must be a place.",
                        "cannot slice a temporary value; `%s` needs a variable, a field or an index",
                        typeStr(c, ot));
                return ttError(tt);
            }

            /* With a fixed-array base the length is a compile-time constant, so an omitted
             * bound is filled in as a literal:
             *     a[2..] -> a[2..15]      a[..] -> a[0..15]
             * Codegen then sees two literal bounds and can emit code with no checks. */
            if (ob->kind == TY_ARRAY) {
                if (!e->u.slice.lo) e->u.slice.lo = intLit(c, 0, e->line);
                if (!e->u.slice.hi) e->u.slice.hi = intLit(c, ob->asize, e->line);
            }

            for (int k = 0; k < 2; k++) {
                Expr *b = k == 0 ? e->u.slice.lo : e->u.slice.hi;
                if (!b) continue;
                Type *bt = checkValue(c, b);
                if (!ttIsError(bt) && !ttIsInteger(bt))
                    ckError(c, b->line, "A slice bound must be an integer.",
                            "slice bound must be an integer, found `%s`", typeStr(c, bt));
            }

            /* An error the compiler can prove is reported here; none of it is left to
             * runtime. Each bound is examined on its own: an out-of-range literal is wrong
             * whatever the other bound says. */
            if (ob->kind == TY_ARRAY) {
                long long n = (long long)ob->asize;
                Expr *lp = e->u.slice.lo, *hp = e->u.slice.hi;
                long long lv = 0, hv = 0;
                bool lOk = asIntLit(lp, &lv);
                bool hOk = asIntLit(hp, &hv);

                /* Each bound is examined on its own: an out-of-range literal is wrong
                 * whatever the other bound says. */
                if (hOk && (hv > n || hv < 0)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice end %lld is not inside `%s` (length %lld)",
                            hv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && (lv < 0 || lv > n)) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice start %lld is not inside `%s` (length %lld)",
                            lv, typeStr(c, ot), n);
                    return ttError(tt);
                }
                if (lOk && hOk && hv < lv) {
                    ckError(c, e->line, "The compiler can prove this slice is out of range.",
                            "slice `%lld..%lld` ends before it starts", lv, hv);
                    return ttError(tt);
                }
            }
            /* A view inherits writability from what it was sliced out of:
             *   `var a` / a `mut ref` parameter          ->  `mut slice<T>`
             *   `let a` / a string literal / a read-only view  ->  `slice<T>`
             * That is how one view type expresses both permissions, without the two slice
             * types Rust needs. */
            return ttViewMut(tt, sliceOf(c, elem),
                             isWritablePlace(c, e->u.slice.obj));
        }

        case EX_ARRAYLIT: {
            /* The type comes from the context (`var a: [3]i32 = [...]`) or is taken from
             * the elements. */
            Type *want = (e->type && e->type->kind == TY_ARRAY) ? e->type : NULL;
            Type *elemT = NULL;

            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                Expr *el = *(Expr **)vecAt(&e->u.arraylit.elems, i);
                if (want) adoptContextType(el, want->inner);
                Type *t = checkValue(c, el);
                if (ttIsError(t)) continue;

                if (!elemT) {
                    elemT = want ? want->inner : t;
                }
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "array element %zu", i);
                checkAssignable(c, elemT, t, el, bufCstr(&what));
            }

            if (!elemT && want) elemT = want->inner;
            if (!elemT || ttIsError(elemT)) {
                ckError(c, e->line,
                        "An empty array literal needs the length and element type from context: "
                        "`var a: [3]i32 = [...]`.",
                        "cannot infer the element type of this array literal");
                return ttError(tt);
            }

            int64_t n = (int64_t)e->u.arraylit.elems.len;
            if (e->u.arraylit.rest) {
                if (!want) {
                    ckError(c, e->line,
                            "`...` fills the rest with zero values, so the length has to "
                            "come from the type.",
                            "`...` needs the array length from context");
                    return ttError(tt);
                }
                n = want->asize;
                if (n < (int64_t)e->u.arraylit.elems.len)
                    ckError(c, e->line, NULL,
                            "too many elements: `%s` holds %lld",
                            typeStr(c, want), (long long)want->asize);
            } else if (want && n != want->asize) {
                ckError(c, e->line,
                        "An array literal must give every element. Use a trailing `...` "
                        "to fill the rest with zero values.",
                        "`%s` needs %lld element(s), got %lld",
                        typeStr(c, want), (long long)want->asize, (long long)n);
                return ttError(tt);
            }

            Type *arr = ttArray(tt, n, elemT);
            if (want && !ttEquals(want, arr))
                ckError(c, e->line, NULL, "array literal does not match `%s`", typeStr(c, want));
            return arr;
        }

        case EX_REF: {
            Expr *op = e->u.ref.operand;
            Type *ot = checkExpr(c, op);
            /* Taking an address records that this binding's address has gone out. An alias
             * may exist now, so a write through the binding can no longer be treated as the
             * only write to that storage.
             * The narrowing facts recorded for paths through it are dropped at the same
             * time: `h.p` being non-null is no longer reliable. */
            { Sym *rs = placeRoot(c, op); if (rs) { unNarrow(c, rs->cname); rs->addressed = true; } }
            if (ttIsError(ot)) return ttError(tt);

            if (ot->kind == TY_REF) {
                ckError(c, e->line, "it is already a reference -- just pass it as is",
                        "`ref` applied to a value that is already a reference");
                return ot;
            }
            if (!isLvalue(op)) {
                ckError(c, e->line, "only variables and fields can be referenced",
                        "cannot take a reference to this expression");
                return ttError(tt);
            }
            /* Which reference comes out depends on whether the place is writable:
             *   a `var` variable / a `mut ref` parameter  ->  `mut ref T`
             *   a `let` variable / a `ref` parameter      ->  `ref T` (read-only)
             *
             * A read-only borrow may always be taken: that is the whole reason borrows
             * exist. While `ref` meant mutable only, taking a reference to a `let` was
             * rejected outright, so even a plain read could not be written. */
            Type *r = ttRef(tt, ot);
            r->mut = isWritablePlace(c, op);
            return r;
        }

        case EX_TRY:
            /* `?` forwards an error at statement level: it expands into "evaluate once,
             * test, return". C has no statement expressions, so it is legal in exactly three
             * positions:
             *     let x = e?   /   x = e?   /   return e?
             * Anywhere else, say `f(e? + 1)`, it is reported clearly instead of producing C
             * that does not compile. */
            ckError(c, e->line,
                    "`?` has to expand into statements (evaluate once, test, return), "
                    "so it only fits where a whole statement can be rewritten.",
                    "`?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?`");
            return ttError(tt);

        case EX_ASSOC: {            /* `option<i64>::some(x)`. An associated function is a
                                     * function written inside a `struct` without `self`.
                                     * The type arguments are written out in full rather than
                                     * inferred from the arguments or the context: extC is
                                     * explicit instead of inferring. */
            Type *raw = typeNamed(c->arena, e->u.assoc.typeName);
            raw->targs = e->u.assoc.targs;
            Type *t = ttResolve(tt, c->ctx, raw, e->line, c->curParams);
            if (ttIsError(t)) return ttError(tt);
            e->assocOwner = t;

            /* Constructing an enum variant goes through this path as well:
             * `option<i64>::some(3)` and `maybe<i64>::nothing`. So `::` has two readings for
             * an enum but one meaning only -- build a value -- and the prelude and existing
             * code keep writing it exactly as they do today. */
            StructDef *esd = structOf(t);
            if (!esd && t->kind == TY_ENUM && t->edef) {
                Variant *v = findVariant(t->edef, e->u.assoc.name);
                if (!v) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "variants of %s:", t->name);
                    for (size_t i = 0; i < t->edef->variants.len; i++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&t->edef->variants, i))->name);
                    ckError(c, e->line, bufCstr(&note),
                            "`%s` has no variant `%s`", t->name, e->u.assoc.name);
                    return ttError(tt);
                }
                Vec evargs = e->u.assoc.args;      /* union: copy it out first */
                e->kind = EX_ENUMVAL;
                e->u.enumval.typeName = t->name;   /* instance name, e.g. `maybe_i64` */
                e->u.enumval.variant  = v->name;
                e->u.enumval.args     = evargs;
                return checkExprInner(c, e);
            }

            StructDef *sd = structOf(t);
            FuncDef *f = NULL;
            if (sd) {
                for (size_t i = 0; i < sd->methods.len; i++) {
                    FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                    if (m->isAssoc && strcmp(m->name, e->u.assoc.name) == 0) { f = m; break; }
                }
            }
            if (!f) {
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "associated functions of %s:", DN(sd));
                    bool any = false;
                    for (size_t i = 0; i < sd->methods.len; i++) {
                        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
                        if (!m->isAssoc) continue;
                        bufPrintf(&note, " %s", m->name);
                        any = true;
                    }
                    if (!any) bufPuts(&note, " (none)");
                    bufPuts(&note, "; a function written inside a `struct` without"
                                  " `self` is an associated function");
                } else {
                    bufPuts(&note, "only struct types have associated functions");
                }
                ckError(c, e->line, bufCstr(&note), "no associated function `%s` on `%s`",
                        e->u.assoc.name, e->u.assoc.typeName);
                return ttError(tt);
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* An associated function needs its arena argument computed too: it may
             * allocate, or it may return a reference. Omitting this generated a call with one
             * argument too few, so `Type::make()` did not compile. */
            e->homeDepth = callHomeDepth(c, &e->u.assoc.args, &f->params, e);
            setCallArenaArg(c, e);      /* resolve which arena the call finally passes */
            /* The reference checking is moved to the end of this case, after the arguments
             * have been checked. */

            if (e->u.assoc.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s::%s` expects %zu argument(s), got %zu",
                        e->u.assoc.typeName, f->name, f->params.len, e->u.assoc.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                Expr  *a = *(Expr **)vecAt(&e->u.assoc.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);
                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. ",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                checkAssignable(c, pt, at, a, "argument");
            }
            /* The reference check applies to an associated function as well, once its
             * arguments have been checked. */
            checkCallRefArgs(c, f, &e->u.assoc.args, &f->params, e->homeDepth,
                             e->line, e->u.assoc.name);
            return f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);
        }

        case EX_CONV: {
            /* `i32(x)`: an explicit conversion.
             *   - Widening, which is already implicit, may still be written out and generates
             *     no check at all.
             *   - Narrowing and sign changes trap when the value does not fit, with the
             *     source position in the message.
             *   - Integer to float and back follows C (truncation, round to nearest), with a
             *     range check on the float-to-integer direction.
             * A conversion the compiler can prove fits, such as `i32(u8 value)`, generates no
             * check either. */
            Type *t = ttFromName(tt, e->u.conv.typeName);
            if (!t || t->kind != TY_BUILTIN) {
                ckError(c, e->line, NULL, "`%s` is not a scalar type", e->u.conv.typeName);
                return ttError(tt);
            }
            e->u.conv.type = t;
            Type *src = checkValue(c, e->u.conv.operand);
            if (ttIsError(src)) return ttError(tt);
            bool si = ttIsInteger(src), sf = ttIsFloat(src);
            bool ti = ttIsInteger(t),   tf = ttIsFloat(t);
            if (!((si || sf) && (ti || tf))) {
                ckError(c, e->line,
                        "explicit conversions are between numbers: integers and floats",
                        "cannot convert `%s` to `%s`", typeStr(c, src), typeStr(c, t));
                return ttError(tt);
            }
            /* Float to float follows C (round to nearest; out of range becomes an infinity),
             * so it is not checked. An integer widening is lossless and keeps the signedness,
             * so it is not checked either. Everything else -- narrowing, a sign change, float
             * to integer -- is checked. */
            bool lossless = (si && ti && ttCanWiden(src, t)) || (sf && tf);
            e->convCheck = !lossless;
            return t;
        }

        case EX_NEW: {
            /* `new T`, `new [N]T`, and `new T[n]`:
             *   a single T      ->  `mut ref T`
             *   a fixed `[N]T`  ->  `mut ref [N]T`
             *   `T[n]`          ->  `mut slice<T>`, which is how a buffer is built
             * The memory comes from the arena of the current block and is zeroed: in extC a
             * fresh allocation always reads as zero. */
            Type *w = ttResolve(tt, c->ctx, e->u.new_.type, e->line, c->curParams);
            if (ttIsError(w)) return ttError(tt);
            e->u.new_.type = w;

            if (w->kind == TY_PARAM || ttHasParam(w)) {
                /* `T` has no size while a template is being checked, so the size check is
                 * deferred to instantiation and recorded. That is what makes `new T[cap]`
                 * inside `varArray<T>` writable at all. If the size is still unknown at
                 * instantiation -- a nested generic that never received its argument, say --
                 * the error is reported there. */
                recordNewSizeCheck(c, w, e->line);
            } else if (ttIs(w, "void") || ttIsError(w)) {
                ckError(c, e->line,
                        "`new` needs a concrete type: its size is decided at compile time",
                        "cannot `new` `%s` -- its size is not known here", typeStr(c, w));
                return ttError(tt);
            }

            /* Depth is the depth of the current block: the same number the generated C uses
             * to pick `__extc_a[k]`.
             *
             * The exception is a function with a home arena, one that hands its own
             * allocations to the caller. What it allocates then lives as long as the scope the
             * caller chose, so the site is recorded as `ARENA_HOME`. In `refDepth` terms that
             * reads as 0, the level outside this frame, which is the level the escape check
             * lets through; this is what makes `return h` in
             * `fn build() -> mut ref node` legal.
             *
             * This level is the starting point, not the final one. When the escape check finds
             * the new object stored somewhere longer-lived, it promotes `arenaLevel` to that
             * level (`promoteInto` in check_escape.c), and `refDepth` follows `arenaLevel`,
             * because the two must always be the same number.
             *
             * The same node may be checked twice, so the level is only assigned on the first
             * visit and only ever moved towards a longer life. */
            /* The checker decides the level and codegen only translates it. A function with
             * a home arena gives `ARENA_HOME`; everything else goes by block, except that
             * storage marked `@overwrite` has to survive every round of the loop, so it lives
             * in this frame, at level 1.
             *
             * `needsHome` is read as it stands at this moment, which covers the direct
             * evidence only. A function that gains a home arena because of a call it makes is
             * decided again by `checkModule`, using `FuncDef.newSites`, once the closure is
             * complete. */
            if (e->arenaLevel == 0)
                e->arenaLevel = (c->curFunc && c->curFunc->needsHome) ? ARENA_HOME
                              : (e->reuse ? 1 : (int)c->scopes.len);
            /* Keep the lexical level in its own field: the branch above may have replaced
             * `arenaLevel` with the `ARENA_HOME` sentinel, while the solver still needs to
             * know which block the site started in. */
            if (e->lexicalLevel == 0)
                e->lexicalLevel = e->reuse ? 1 : (int)c->scopes.len;
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame". */
            e->minAt = -1;
        *(Expr **)vecPush(&c->curArenaSites) = e;
            /* `refDepth` follows `arenaLevel`: the two must always be the same number. The
             * only exception is `ARENA_HOME`, whose sentinel is -1 while `refDepth` speaks of
             * 0 as "the level outside this frame", so it maps back to 0 -- the home arena is
             * the scope the caller chose, which is depth 0. */
            {
                int depth = arenaDepthOf(e->arenaLevel);    /* the one conversion point */
                if (e->refDepth == 0 || e->refDepth > depth) e->refDepth = depth;
            }

            if (!e->u.new_.count) {
                Type *r = ttRef(tt, w);
                r->mut = true;                  /* freshly allocated storage is writable */
                return r;
            }

            /* `T[n]`: the count must be an integer. An impure count needs a temporary so
             * that it is not evaluated twice. */
            Type *nt = checkValue(c, e->u.new_.count);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, e->u.new_.count->line, NULL,
                        "the element count must be an integer, found `%s`", typeStr(c, nt));
                return ttError(tt);
            }
            if (!repeatablePure(e->u.new_.count)) e->needTemp = true;
            /* Freshly allocated memory is writable, so the view is a `mut slice<T>`: the
             * same rule that gives `a[..]` a `mut slice<T>` when `a` is writable. */
            return ttViewMut(tt, sliceOf(c, w), true);
        }

        case EX_GENCALL: {
            /* A generic call. Only the built-in primitives use this shape today:
             * `alloc<i32>(n)` asks the arena of the current frame for room for n values of T
             * and returns a writable reference.
             *
             * It is a primitive rather than a library function because the arena itself has to
             * be generated by the compiler: the library needs it to allocate for itself, so the
             * library cannot provide it. */
            /* `maxOf<i32>(4, 3)`: a free function called with explicit type arguments, which
             * is the only spelling available when `T` appears in the return type alone.
             * Otherwise the old rule stands: only the built-in primitives arrive through this
             * path (`alloc<T>(n)`). */
            /* Two allocation primitives share this path and differ only in the return type:
             *   `alloc<T>(n)      -> mut ref T`     (points at one place)
             *   `allocSlice<T>(n) -> mut slice<T>`  (a `{data,len}` view, zeroed)
             * The second one exists because a buffer whose length is known only at runtime --
             * reading a file of unknown size, or the 4 KB the `reader` asks for -- cannot be
             * built with `alloc`: that returns a `mut ref T`, which can be neither indexed nor
             * sliced. Both allocate zeroed memory. */
            bool isAlloc  = strcmp(e->u.gencall.name, "alloc") == 0;
            bool isAllocS = strcmp(e->u.gencall.name, "allocSlice") == 0;
            if (!isAlloc && !isAllocS) {
                FuncDef *tf = findFunc(c, e->u.gencall.name);
                if (tf && tf->typeParams.len == e->u.gencall.targs.len && tf->typeParams.len > 0) {
                    Vec targs;
                    vecInit(&targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < e->u.gencall.targs.len; i++) {
                        Type *t = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, i),
                                            e->line, c->curParams);
                        if (ttIsError(t)) return ttError(tt);
                        *(Type **)vecPush(&targs) = t;
                    }
                    /* The type arguments still mention `T` here: a call to `idOf<T>(...)`
                     * inside a generic body. Nothing is recorded at this point, because the node
                     * is rewritten to EX_CALL and walked again below, and that path notices the
                     * `T` argument and records the deferred check itself. One place instead of
                     * two, so the two cannot drift apart. */
                    FuncDef *inst = funcInstance(c, tf, &targs, e->line);
                    inst->used = true;
                    tf->used = true;
                    /* Rewrite the node into an ordinary call: the shape every path except
                     * `alloc` uses. */
                    Vec args = e->u.gencall.args;
                    Expr *id = exprNew(c->arena, EX_IDENT, e->line);
                    id->u.ident.name = tf->name;
                    e->kind = EX_CALL;
                    e->u.call.callee = id;
                    e->u.call.args   = args;
                    e->qualified     = true;      /* do not trigger the qualification check */
                    e->func = inst;
                    /* After the rewrite, walk the node again: the argument checks, the
                     * return type, and the home arena all live on the EX_CALL path, so
                     * returning void here instead is wrong. That mistake was made once. */
                    return checkExpr(c, e);
                }
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive or generic function",
                        e->u.gencall.name);
                return ttError(tt);
            }
            if (e->u.gencall.targs.len != 1) {
                ckError(c, e->line, NULL, "`%s` needs exactly one type argument, e.g. `%s<i32>(4)`",
                        e->u.gencall.name, e->u.gencall.name);
                return ttError(tt);
            }
            Type *elem = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, 0),
                                   e->line, c->curParams);
            if (ttIsError(elem)) return ttError(tt);
            /* Write the resolved type back: codegen reads the type arguments of the node, not
             * this local variable. */
            *(Type **)vecAt(&e->u.gencall.targs, 0) = elem;
            if (e->u.gencall.args.len != 1) {
                ckError(c, e->line, NULL, "`%s` takes one argument (how many elements)",
                        e->u.gencall.name);
                return ttError(tt);
            }
            Expr *n = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *nt = checkValue(c, n);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, n->line, NULL, "the element count must be an integer, found `%s`",
                        typeStr(c, nt));
                return ttError(tt);
            }

            /* This memory lives until the end of the current block, because arenas are
             * released per block, so its depth is the depth of that block: `c->scopes.len`.
             *
             * This line is the seam between the depth model and the arena granularity, and both
             * sides have to use the same number: the checker compares block depths to decide
             * whether a reference may be stored here, and the generated C uses the block depth
             * to pick `__extc_a[k]`, which is released when the block ends.
             * Hard-coding 1, as an earlier version did, makes `p` in
             * `while { p = alloc<i32>(1) }` point at freed memory on the next iteration --
             * measured, and exactly the dangling case this has to prevent.
             * As a result, returning freshly allocated memory and storing loop-local allocations
             * outside the loop are both stopped by the escape check. To hand the memory out, the
             * caller has to supply the buffer or the arena. */
            /* The checker decides the level of an `alloc` site, and it has to use exactly the
             * rule `new` uses. The EX_NEW case above reads: a function with a home arena gives
             * `ARENA_HOME`, so the object goes into the arena the caller chose and outlives this
             * frame; otherwise the level is the block level. An earlier version always used the
             * block level, so an `alloc` in a function with a home arena counted as frame-local
             * and returning it was rejected as `1 > 0`, while the same program written with `new`
             * passed -- and the error text told the user to write `new`.
             * Being symmetric with `new` removes that whole family of false rejections: a home
             * arena gives `ARENA_HOME` and depth 0, "the level outside this frame". */
            if (c->curFunc && c->curFunc->needsHome) {
                e->arenaLevel = ARENA_HOME;
                e->refDepth   = 0;
            } else {
                e->refDepth   = c->scopes.len;
                e->arenaLevel = (int)c->scopes.len;
            }
            if (e->lexicalLevel == 0) e->lexicalLevel = (int)c->scopes.len;   /* lexical level */
            /* Initial value: no constraint has touched this site yet. Relying on the 0 from
             * `arenaAllocZero` would be wrong, because 0 means "must outlive the frame".
             * `alloc` is an allocation site too, so it needs this initialisation as much as
             * `new` does. Without the line, the `inner` of `examples/alloc-in-block` was taken
             * to outlive the frame and was emitted as `__extc_home` although it has no home
             * arena, so the generated C did not compile. */
            e->minAt = -1;
            /* `alloc` has to be registered as an allocation site, the same way `new` is
             * registered in the EX_NEW case above with `vecPush(&c->curArenaSites)`. Without
             * the registration, the rewrite pass that runs after the closure cannot see it, so
             * an `alloc` in a function with a home arena stayed at the block level and was
             * released when the block ended. That produced a family of false rejections:
             * `fn f() -> mut ref i32 { return alloc<i32>(1) }` was rejected while the same
             * program written with `new` passed, and the error text told the user to write
             * `new`. Once registered, a home arena rewrites the site to `ARENA_HOME`, fully
             * symmetric with `new`. */
            *(Expr **)vecPush(&c->curArenaSites) = e;
            if (isAllocS) {
                /* `allocSlice<T>(n) -> mut slice<T>`: the view itself is writable. The `mut`
                 * sits on the view, not on a reference that points at it. */
                return ttViewMut(tt, sliceOf(c, elem), true);
            }
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* freshly allocated storage is writable */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b`: fall back when there is nothing. The meaning is
             * `match a { some(v) => v  _ => b }`, but only one side is evaluated: when a has a
             * value, b is never computed. That is the short circuit of `&&` / `||` again. */
            /* The flag has to be read before the subject is checked, because a subject that is
             * itself a call raises it while it is checked. Reading it afterwards wrongly
             * reported `var a = h() ?? 0` when that is the first statement. */
            bool priorFx = c->stmtFx != 0;
            Type *mt = checkExpr(c, e->u.coalesce.main);
            if (ttIsError(mt)) { checkExpr(c, e->u.coalesce.fallback); return ttError(tt); }

            bool isOpt = isProtoType(mt, "option", 1);
            bool isRes = isProtoType(mt, "result", 2);
            Type *want = NULL;
            if (isOpt || isRes) {
                want = *(Type **)vecAt(&mt->targs, 0);          /* the payload type T */
            } else if (mt->kind == TY_REF && mt->nullable) {
                want = mt;   /* `?ref T`: the fallback is a reference as well */
            } else {
                /* Position rule: `??` is meaningful only for something that may have no
                 * value. Saying so beats inventing a meaning for it. */
                checkExpr(c, e->u.coalesce.fallback);
                ckError(c, e->line,
                        "`??` means \"if there is nothing, use this instead\", so the left"
                        " side must be an `option`, a `result`, or a `?ref T`",
                        "left of `??` is `%s`, which always has a value", typeStr(c, mt));
                return ttError(tt);
            }

            /* When the subject is not free of side effects, as in `f() ?? -1`, a plain
             * conditional expression will not do: the subject would appear twice in the C and
             * `f()` would run twice. It has to be evaluated into a temporary first, which needs
             * the ability to emit a prefix in front of the enclosing statement. Most positions
             * have it; two do not. */
            /* An impure subject is computed into a temporary, and that prefix is emitted in
             * front of the whole statement, so it would jump ahead of an earlier side effect in
             * the same statement:
             *     `g() + (h() ?? 0)`   reads g first and h second, but h would run first
             * Reordering silently is worse than reporting it and asking for two lines: the order
             * the user reads has to be the order that runs.
             * Only calls count as evidence here: the order of `new`, of literals, and of
             * conversions is unobservable, so those are not reported. */
            if (!repeatablePure(e->u.coalesce.main)) {
                checkExpr(c, e->u.coalesce.fallback);
                if (priorFx && !c->noHoist) {
                    ckError(c, e->line,
                            "The subject of `??` is computed into a temporary **before the whole"
                            " statement** (that is what makes it run only once). So an earlier"
                            " side effect in the same statement would run *after* it -- the"
                            " opposite of what the line reads like. Split the statement in two"
                            " (`var t = f() ?? 0` then use `t`).",
                            "`??` here would run **before** the call that comes earlier in this"
                            " statement; split the line so the order you read is the order it runs");
                    return ttError(tt);
                }
                if (c->noHoist) {
                    ckError(c, e->line,
                            "`while` re-evaluates its condition every round, but a temporary"
                            " can only be computed once, before the loop. Bind it inside the"
                            " loop body: `while true { let r = f()  if ... { break } }`",
                            "`??` here cannot be evaluated ahead of time -- the temporary"
                            " would run once instead of every round");
                    return ttError(tt);
                }
                e->needTemp = true;        /* codegen evaluates it once, then tests the temp */
                /* Every side effect of the subject is hoisted into the prefix, in source order
                 * relative to the other temporaries, so the subject does not count as a call
                 * left in place. Counting it reported two `f() ?? -1` in one `println` that are
                 * in fact fine, which the corpus caught immediately.
                 * The fallback stays inside the conditional expression and runs only when the
                 * subject has no value, so it does count. */
                c->stmtFx = priorFx ? 1 : 0;
                if (exprHasCall(c, e->u.coalesce.fallback)) c->stmtFx = 1;
                return want;
            }

            adoptContextType(e->u.coalesce.fallback, want);
            Type *ft = checkInto(c, want, e->u.coalesce.fallback);
            checkAssignable(c, want, ft, e->u.coalesce.fallback, "the right side of `??`");
            e->type = want;
            return want;
        }

        case EX_SIGN: {
            /* `e!` is the user's signature: what the compiler cannot prove, the user takes
             * responsibility for, and not one check is generated. It has three uses:
             *   `opt!` (`option<T>`)  -> the payload T
             *   `r!` (`result<T,E>`)  -> the payload T; on failure the behaviour is whatever the
             *                            user signed for, and the undefined behaviour is theirs
             *   `p!` (`?ref T`)       -> `ref T`, the escape hatch for "I know it is not null"
             * Anything else is an error: a signature has to be written where it means something. */
            Type *ot = checkExpr(c, e->u.sign.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (isProtoType(ot, "option", 1) || isProtoType(ot, "result", 2)) {
                return *(Type **)vecAt(&ot->targs, 0);      /* the payload type */
            }
            if (ot->kind == TY_REF) {
                if (!ot->nullable) {
                    ckError(c, e->line, "it is already a plain `ref T`, which can never be null",
                            "`%s` is not nullable, so `!` has nothing to assert",
                            typeStr(c, ot));
                    return ttError(tt);
                }
                Type *nn = ttRef(tt, ot->inner);            /* the non-null version */
                nn->mut = ot->mut;
                e->type = nn;
                return nn;
            }
            ckError(c, e->line,
                    "`!` means \"I sign for it\": it turns an `option` / `result` / `?ref T`"
                    " into the value / non-null reference without any check",
                    "`!` needs an `option`, a `result`, or a nullable reference, found `%s`",
                    typeStr(c, ot));
            return ttError(tt);
        }

        case EX_DEREF: {
            /* `*p` is the value, or the place, that `p` points at. Writability comes from the
             * type of `p` (`mut ref`). */
            Type *ot = checkExpr(c, e->u.deref.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (ot->kind != TY_REF) {
                ckError(c, e->line,
                        "`*` means \"the value this reference points at\"; a plain value has"
                        " nothing to dereference",
                        "cannot dereference `%s`, which is not a reference", typeStr(c, ot));
                return ttError(tt);
            }
            if (rejectNullableDeref(c, ot, e->u.deref.operand, "`*`")) return ttError(tt);
            return ot->inner;
        }

        case EX_ENUMVAL: {
            /* An instance of a generic enum (`maybe<i64>`) is not in the type table's by-name
             * map: it is produced by instantiation. Constructing it through the EX_ASSOC path
             * therefore records the resolved type on `assocOwner`, which is preferred here. */
            Type *et = e->assocOwner ? e->assocOwner : ttFromName(tt, e->u.enumval.typeName);
            if (!et || et->kind != TY_ENUM || !et->edef) return ttError(tt);
            Variant *v = findVariant(et->edef, e->u.enumval.variant);
            if (!v) return ttError(tt);

            /* A variant without a payload, `status.ok`: it takes no arguments. */
            if (v->types.len == 0) {
                if (e->u.enumval.args.len > 0) {
                    ckError(c, e->line, NULL,
                            "`%s.%s` carries no payload, so it takes no arguments",
                            et->name, v->name);
                    return ttError(tt);
                }
                return et;
            }

            /* Constructing a variant with a payload, `shape.circle(2.0)`: payload values are
             * matched by position. */
            if (e->u.enumval.args.len != v->types.len) {
                ckError(c, e->line, NULL,
                        "`%s.%s` carries %zu value(s), got %zu",
                        et->name, v->name, v->types.len, e->u.enumval.args.len);
                return ttError(tt);
            }
            for (size_t i = 0; i < v->types.len; i++) {
                Type *pt  = payloadType(tt, et, v, i);
                Expr *arg = *(Expr **)vecAt(&e->u.enumval.args, i);
                Type *at  = checkInto(c, pt, arg);
                checkAssignable(c, pt, at, arg, "payload value");
                /* The payload is stored inside this value, so a reference it carries must not
                 * live for less than the value does. */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }

        case EX_CALL: {
            /* Constructing a variant with a payload, `shape.circle(2.0)`: it looks like a field
             * access followed by a call, but `shape` is a type name and not a variable, so it is
             * recognised as an enum construction and rewritten to EX_ENUMVAL. */
            if (e->u.call.callee->kind == EX_FIELD) {
                Expr *fld = e->u.call.callee;
                if (fld->u.field.obj->kind == EX_IDENT && !lookup(c, fld->u.field.obj->u.ident.name)) {
                    Type *et = ttFromName(tt, fld->u.field.obj->u.ident.name);
                    if (et && et->kind == TY_ENUM && et->edef) {
                        Variant *v = findVariant(et->edef, fld->u.field.name);
                        if (!v) {
                            Buf note;
                            bufInit(&note, c->arena);
                            bufPrintf(&note, "variants of %s:", et->name);
                            for (size_t i = 0; i < et->edef->variants.len; i++)
                                bufPrintf(&note, " %s",
                                          (*(Variant **)vecAt(&et->edef->variants, i))->name);
                            ckError(c, e->line, bufCstr(&note),
                                    "`%s` has no variant `%s`", et->name, fld->u.field.name);
                            return ttError(tt);
                        }
                        Vec args = e->u.call.args;      /* union: copy it out first */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->u.enumval.args     = args;
                        e->assocOwner = et;
                        return checkExprInner(c, e);    /* the rest is EX_ENUMVAL's job */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            /* `flush()` pushes out the buffer that `println` writes through.
             *
             * It is needed because `std::io`'s `writeBytes` writes straight to the file
             * descriptor with `write(2)` and no buffering, while `println` goes through printf,
             * which buffers. Mixing the two loses the order: the prompt has not been printed yet
             * and the program is already waiting for input. */
            if (strcmp(name, "flush") == 0) {
                if (e->u.call.args.len != 0) {
                    ckError(c, e->line, "`flush()` takes no arguments.",
                            "`flush` takes no arguments");
                }
                return ttVoid(tt);
            }

            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkPrintArg(c, a); /* takes a value, so it dereferences */
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            /* A diagnostic has to echo the name as written in the source (`open`), not the
             * mangled `lib$open`. Without `srcName` the message would read "write
             * `lib::lib$open`", which is nonsense. */
            const char *shownName = e->u.call.callee->u.ident.srcName
                                    ? e->u.call.callee->u.ident.srcName : name;
            FuncDef *f = findFunc(c, name);
            /* A function of another module written without qualification: `lib.extc` holds
             * `open`, and the source here says `open()`.
             *
             * A name without `$` is a source-level name, because the loader only rewrites
             * qualified names into flat ones. For such a name the message to give is "this needs
             * a qualified name", not "no such function", which would point the user the wrong
             * way. The test is simply whether the name contains `$`: the character is not legal
             * in an extC identifier, so when it appears the name must be one the loader mangled,
             * and only then is the function really missing. */
            if (!f && name && !strchr(name, '$') && !e->qualified) {
                for (size_t i = 0; i < c->m->funcs.len; i++) {
                    FuncDef *cand = *(FuncDef **)vecAt(&c->m->funcs, i);
                    const char *d = cand->name ? strchr(cand->name, '$') : NULL;
                    if (d && cand->modName && !cand->isPrivate && strcmp(d + 1, shownName) == 0) {
                        requireQualified(c, shownName, cand->modName, false, e->line);
                        return ttError(tt);
                    }
                }
            }
            if (f && !f->reserved && f->modName && !e->qualified)
                requireQualified(c, shownName, f->modName, false, e->line);
            if (!f) {
                /* A bare enum constructor. The error has to point the way instead of saying
                 * only "no such function".
                 *
                 * After `type shape = | circle(f64) | ...`, writing `circle(2.0)` used to report
                 * `call to undefined function `circle`` plus "built-ins available:
                 * print/println", from which nobody could tell that this is a variant and has to
                 * be written with its type name.
                 *
                 * No type inference happens here: writing the type out in full for an associated
                 * call is deliberate, because extC does not guess a type from context. The
                 * message states the correct spelling; it does not fill it in.
                 * Only a variant with a payload is pointed at. A payload-free `status.ok` is a
                 * value in its own right and takes the other path. */
                TypeDef *owner = NULL;
                for (size_t i = 0; i < c->tt->enums.len && !owner; i++) {
                    TypeDef *td = *(TypeDef **)vecAt(&c->tt->enums, i);
                    if (findVariant(td, name)) owner = td;
                }
                if (owner) {
                    /* The `note` argument of `ckError` is passed through verbatim: unlike the
                     * format it takes no varargs, so a value has to be rendered with
                     * `arenaPrintf` first. Passing a raw `%s` printed the literal text. */
                    const char *note = arenaPrintf(c->arena,
                            "A payload variant is named by its type -- write `%s.%s(...)`. "
                            "(extC never guesses a type from context: DECISIONS ruling 27.)",
                            DN(owner), name);
                    ckError(c, e->line, note,
                            "`%s` is a variant of `%s`, not a function -- did you mean `%s.%s(...)`?",
                            name, DN(owner), DN(owner), name);
                    return ttError(tt);
                }
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */

            /* A generic free function: `T` can be inferred from an argument only, because an
             * instance of a type is decided by the type while a call to a free function has
             * nothing but its arguments. So the arguments are checked first, then `T` is
             * inferred, then the instance is created. When inference fails because `T` appears
             * only in the return type, the message tells the user to write the arguments
             * explicitly: `f<i32>(...)`. */
            if (f->typeParams.len > 0) {
                if (e->u.call.args.len != f->params.len) {
                    ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                            name, f->params.len, e->u.call.args.len);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                Vec targs;
                vecInit(&targs, c->arena, sizeof(void *));
                for (size_t i = 0; i < f->typeParams.len; i++)
                    *(Type **)vecPush(&targs) = NULL;
                for (size_t i = 0; i < f->params.len; i++) {
                    Param *p = *(Param **)vecAt(&f->params, i);
                    Expr  *a = *(Expr **)vecAt(&e->u.call.args, i);
                    adoptContextType(a, p->type);
                    Type *at = checkExpr(c, a);
                    if (ttIsError(at)) return ttError(tt);
                    /* A `ref T` parameter: when the argument is written `ref x`, `at` is a
                     * reference, so the pointee types are unified. */
                    Type *want = p->type;
                    if (want->kind == TY_REF && at->kind == TY_REF) { want = want->inner; at = at->inner; }
                    if (!unifyTParams(c->tt, &f->typeParams, &targs, want, at)) {
                        ckError(c, e->line,
                                "Type inference for a generic function's type parameters must see"
                                " them in an argument. If one only appears in the return type,"
                                " write it explicitly: `f<i32>(…)`.",
                                "cannot infer type parameter(s) of `%s` from the arguments", name);
                        return f->ret ? f->ret : ttVoid(tt);
                    }
                }
                for (size_t i = 0; i < f->typeParams.len; i++) {
                    if (*(Type **)vecAt(&targs, i)) continue;
                    ckError(c, e->line,
                            "Type inference for a generic function's type parameters must see"
                            " them in an argument. If one only appears in the return type,"
                            " write it explicitly: `f<i32>(…)`.",
                            "cannot infer type parameter `%s` of `%s`",
                            *(const char **)vecAt(&f->typeParams, i), name);
                    return f->ret ? f->ret : ttVoid(tt);
                }
                /* Calling a generic function from inside a generic body: the type arguments
                 * still mention `T`, so no correct instance can be built at this moment. That is
                 * not an error either.
                 *
                 * The technique matches `RefCheck` and `EqCheck`: while the template is checked,
                 * an instance whose arguments are the parameters themselves is created
                 * (`idOf_T`), which is self-consistent and treats `T` as opaque, so the rest of
                 * the template body still type-checks. The call is recorded, and when the
                 * instance is checked again, `e->func` is redirected to the concrete instance
                 * (`idOf_i32`). */
                FuncDef *inst = funcInstance(c, f, &targs, e->line);
                inst->used = true;
                e->func = inst;
                f = inst;                     /* every check below uses the instance */

                bool hasParamTarg = false;
                for (size_t i = 0; i < targs.len; i++) {
                    Type *a = *(Type **)vecAt(&targs, i);
                    if (a && ttHasParam(a)) { hasParamTarg = true; break; }
                }
                if (hasParamTarg && c->curFunc) {
                    CallCheck *cc = (CallCheck *)arenaAllocZero(c->arena, sizeof(CallCheck));
                    cc->node = e;
                    cc->tmpl = f->tmpl ? f->tmpl : f;   /* the template, not the instance */
                    cc->func = c->curFunc;   /* which template body this belongs to */
                    vecInit(&cc->targs, c->arena, sizeof(void *));
                    for (size_t i = 0; i < targs.len; i++)
                        *(Type **)vecPush(&cc->targs) = *(Type **)vecAt(&targs, i);
                    *(CallCheck **)vecPush(&c->callChecks) = cc;
                }
            }

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            /* The arena for this call comes from the shallowest `mut ref` argument. */
            e->homeDepth = callHomeDepth(c, &e->u.call.args, &f->params, e);
            setCallArenaArg(c, e);      /* decide which arena the call finally passes */
            /* The reference check has to run after the arguments are checked: until then
             * `a->type` is not filled in, `typeContainsRef` answers "no" for everything, and a
             * violation is silently accepted. The method path hit the same trap. The check itself
             * is at the end of this case. */
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkInto(c, p->type, a);

                /* When the parameter is a `ref T`, the call site has to spell `ref` out, so
                 * that a reader sees at a glance that a reference is passed and not a copy. An
                 * argument that already is a reference can be passed as it stands. */
                if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, p->type));
                    continue;
                }
                checkAssignable(c, p->type, at, a, "argument");
            }
            /* Every call site is checked, not only the ones with a home arena: what matters is
             * that the callee may store into a container the argument points at, which has
             * nothing to do with the home arena.
             * The position matters as well: after the arguments are checked, because
             * `exprRefDepth` reads their types. */
            checkCallRefArgs(c, f, &e->u.call.args, &f->params, e->homeDepth, e->line, name);
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_METHOD: {
            /* Constructing a variant with a payload can look like this as well: the syntax of
             * `shape.circle(2.0)` is that of a method call, `receiver.name(args)`, and the only
             * difference is that `shape` is a type name rather than a variable. So it is
             * recognised here first, exactly as `status.ok` is recognised on the EX_FIELD path. */
            if (e->u.method.recv->kind == EX_IDENT && !lookup(c, e->u.method.recv->u.ident.name)) {
                Type *et = ttFromName(tt, e->u.method.recv->u.ident.name);
                if (et && et->kind == TY_ENUM && et->edef) {
                    Variant *v = findVariant(et->edef, e->u.method.name);
                    if (!v) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note, "variants of %s:", et->name);
                        for (size_t i = 0; i < et->edef->variants.len; i++)
                            bufPrintf(&note, " %s",
                                      (*(Variant **)vecAt(&et->edef->variants, i))->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` has no variant `%s`", et->name, e->u.method.name);
                        return ttError(tt);
                    }
                    Vec args = e->u.method.args;
                    e->kind = EX_ENUMVAL;
                    e->u.enumval.typeName = et->name;
                    e->u.enumval.variant  = v->name;
                    e->u.enumval.args     = args;
                    e->assocOwner = et;
                    return checkExprInner(c, e);
                }
            }

            /* Methods live inside a `struct` body only, so they are found by the type of the
             * receiver. */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            if (!f) {
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "methods of %s:", DN(sd));
                    if (sd->methods.len == 0) bufPuts(&note, " (none)");
                    for (size_t i = 0; i < sd->methods.len; i++)
                        bufPrintf(&note, " %s",
                                  (*(FuncDef **)vecAt(&sd->methods, i))->name);
                    bufPuts(&note, "; methods must be declared inside their `struct`");
                } else {
                    bufPuts(&note, "only struct values have methods");
                }
                ckError(c, e->line, bufCstr(&note), "no method `%s` on `%s`",
                        e->u.method.name, typeStr(c, rb ? rb : recvT));
                return ttError(tt);
            }
            e->func = f;  f->used = true;   /* record the resolved function and its use */

            /* A method that takes `self: mut ref T` needs a writable receiver. This is the other
             * half of making signatures tell the truth: `x.bump()` alone does not show whether
             * `x` will be modified, `self: mut ref` in the signature does, and this is where that
             * promise is enforced. */
            {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type->kind == TY_REF && selfP->type->mut &&
                    requireMutable(c, e->u.method.recv, e->line, "call a method that writes"))
                    return ttError(tt);
            }

            /* The receiver is the shallowest `mut ref` argument, since `self` comes first, so
             * what the callee allocates follows the arena the receiver lives in. */
            {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type && selfP->type->kind == TY_REF && selfP->type->mut) {
                    int d = placeDepth(c, e->u.method.recv);
                    /* The `self: mut ref` branch does not go through `callHomeDepth`, so the
                     * escape decision has to be applied here as well. The first version missed it,
                     * and ASan caught the resulting use-after-free. */
                    const char *rrn = placeRootName(e->u.method.recv);
                    if (d != 0 && rrn && isEscapeeName(c, rrn)) d = -1;
                    e->homeDepth = (d == 0) ? -1 : d;
                    /* This receiver branch depends on the escape information as well, and that
                     * information is not final while the body is being checked: it is complete
                     * only once the closure of the call graph is closed. So the site is recorded
                     * and recomputed at the end.
                     * The shape that needs it:
                     *     fn fill(out) { var l: list  l.pushOne(7)  out.take(l) }
                     * Here `l` is published by a method call afterwards, so the receiver has to be
                     * treated as escaping. */
                    if (d != 0 && rrn && c->eSites.arena) {
                        EArenaSite *rec = (EArenaSite *)arenaAllocZero(c->arena, sizeof(EArenaSite));
                        rec->call = e;
                        rec->argRoot[0]  = (char *)rrn;      /* the receiver is argument 0 */
                        rec->argDepth[0] = d;
                        rec->n = 1;
                        *(EArenaSite **)vecPush(&c->eSites) = rec;
                    }
                } else {
                    e->homeDepth = callHomeDepth(c, &e->u.method.args, &f->params, e);
                }
                /* Resolve which arena the call finally passes; both numbers are decided here. */
                setCallArenaArg(c, e);
                /* Whether the argument is a local or a parameter, the depth of what is pushed
                 * into a container has to be recorded. The first version did this in the `else`
                 * branch only, so the `self: mut ref` path -- which is the one `v.push(...)`
                 * takes -- never ran it at all. Only debugging showed why. */
                /* A method call is checked like any other call site: content flow does not
                 * depend on the home arena.
                 *
                 * The receiver has to count as argument 0. The callee's `params[0]` is `self`,
                 * while `e->u.method.args` does not contain the receiver, so the two lengths
                 * differ and the `params->len != args->len` guard at the top of
                 * `checkCallRefArgs` returns early without a word. The reference rule therefore
                 * never applied to a method call at all -- a latent defect found by inspection.
                 * Prepending the receiver below fixes that. */
                (void)0;   /* the check is moved below, after the arguments; see there */
            }

            /* When the receiver is a generic instance, the `T` in the method signature has to
             * be replaced with the type arguments. */
            StructDef *msd = structOf(rb);
            Vec *sp = NULL, *sa = NULL;
            if (rb && rb->kind == TY_GENERIC && msd) {
                sp = &msd->typeParams;
                sa = &rb->targs;
            }

            size_t want = f->params.len - 1;
            Type *rt = f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);

            if (e->u.method.args.len != want) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        e->u.method.name, want, e->u.method.args.len);
                return rt;
            }
            for (size_t i = 0; i < want; i++) {
                Param *p = *(Param **)vecAt(&f->params, i + 1);
                Expr  *a = *(Expr **)vecAt(&e->u.method.args, i);
                Type *pt = ttSubstitute(tt, p->type, sp, sa);

                adoptContextType(a, pt);
                Type *at = checkInto(c, pt, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`.` means \"operate on this value\", so free functions need `ref` spelled out. "
                            "`ref` is a mutable reference, so the target must be a `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, pt));
                    continue;
                }
                checkAssignable(c, pt, at, a, "argument");
            }
            /* The rule has to be checked after the arguments are checked: until then `a->type`
             * is not filled in, `typeContainsRef` answers "no" for everything, and a violation
             * passes silently -- a counterexample that should have been rejected was not.
             * The receiver is prepended as argument 0, because the callee's `params[0]` is
             * `self`. */
            {
                Vec margs; vecInit(&margs, c->arena, sizeof(Expr *));
                *(Expr **)vecPush(&margs) = e->u.method.recv;
                for (size_t ai = 0; ai < e->u.method.args.len; ai++)
                    *(Expr **)vecPush(&margs) = *(Expr **)vecAt(&e->u.method.args, ai);
                checkCallRefArgs(c, f, &margs, &f->params, e->homeDepth,
                                 e->line, e->u.method.name);
            }
            return rt;
        }

        case EX_STRUCTLIT: {
            /* The type comes from the name written in the literal or from the context; a generic
             * instance can only come from the context. */
            Type *st = e->u.lit.name ? ttFromName(tt, e->u.lit.name) : e->type;

            if (e->u.lit.name && (!st || ttIsError(st))) {
                ckError(c, e->line, NULL, "unknown struct `%s`", e->u.lit.name);
                return ttError(tt);
            }
            StructDef *sd = structOf(st);
            if (!sd) {
                ckError(c, e->line,
                        "a bare `{}` works only where the context pins the type down: "
                        "`var x: T = {}` / `return {}` / `f({})`",
                        "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            if (st->kind == TY_STRUCT && sd->typeParams.len > 0) {
                ckError(c, e->line, "a generic needs explicit type arguments, e.g. `Pair<i32, i32> { ... }`",
                        "`%s` is generic; type arguments cannot be inferred here", DN(sd));
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "fields of %s:", DN(sd));
                    for (size_t k = 0; k < sd->fields.len; k++)
                        bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, k))->name);
                    ckError(c, fi->value->line, bufCstr(&note),
                            "struct `%s` has no field `%s`", DN(sd), fi->name);
                    continue;
                }
                Type *want = fd->type;
                if (st->kind == TY_GENERIC)
                    want = ttSubstitute(tt, want, &sd->typeParams, &st->targs);

                adoptContextType(fi->value, want);
                Type *vt = checkInto(c, fd->type, fi->value);
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "field `%s`", fi->name);
                checkAssignable(c, want, vt, fi->value, bufCstr(&what));
            }

            /* An omitted field is filled in with its zero value, but `ref` has no zero value.
             * Left alone this generates `.field = 0`, which is a null reference, while the
             * language says `ref` can never be null. The same kind of hole appeared for `str`
             * before. */
            for (size_t i = 0; i < sd->fields.len; i++) {
                FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
                bool given = false;
                for (size_t k = 0; k < e->u.lit.inits.len && !given; k++)
                    given = strcmp((*(FieldInit **)vecAt(&e->u.lit.inits, k))->name, fd->name) == 0;
                if (given) continue;

                Type *ft = fd->type;
                if (st->kind == TY_GENERIC)
                    ft = ttSubstitute(tt, ft, &sd->typeParams, &st->targs);
                if (typeLacksZeroValue(tt, ft))
                    ckError(c, e->line,
                            "`ref` has no default value (it is a non-nullable reference)",
                            "field `%s` must be given explicitly", fd->name);
            }
            return st;
        }
    }
    return ttError(tt);
}

/* Declared here because the two walks below recurse through it. */
static bool funcMayPrint(Checker *c, FuncDef *f);

static bool stmtMayPrint(Checker *c, Stmt *s);

/* Report whether evaluating an expression may print, directly or through a call.
 *
 * Params:
 *   c - checker
 *   e - the expression to inspect; NULL counts as "does not print"
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 */
static bool exprMayPrint(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return true;
        if (strcmp(n, "flush") == 0) return false;      /* flushing is not printing */
    }
    switch (e->kind) {
    case EX_CALL:
        if (e->func && funcMayPrint(c, e->func)) return true;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.call.args, i))) return true;
        return false;
    case EX_METHOD:
        if (e->func && funcMayPrint(c, e->func)) return true;
        if (exprMayPrint(c, e->u.method.recv)) return true;
        for (size_t i = 0; i < e->u.method.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.method.args, i))) return true;
        return false;
    case EX_ASSOC:
        if (e->func && funcMayPrint(c, e->func)) return true;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.assoc.args, i))) return true;
        return false;
    case EX_BIN:   return exprMayPrint(c, e->u.bin.left) || exprMayPrint(c, e->u.bin.right);
    case EX_UN:    return exprMayPrint(c, e->u.un.operand);
    case EX_FIELD: return exprMayPrint(c, e->u.field.obj);
    case EX_INDEX: return exprMayPrint(c, e->u.index.obj) || exprMayPrint(c, e->u.index.index);
    case EX_SLICE: return exprMayPrint(c, e->u.slice.obj) || exprMayPrint(c, e->u.slice.lo) ||
                          exprMayPrint(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprMayPrint(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF:   return exprMayPrint(c, e->u.ref.operand);
    case EX_DEREF: return exprMayPrint(c, e->u.deref.operand);
    case EX_SIGN:  return exprMayPrint(c, e->u.sign.operand);
    case EX_CONV:  return exprMayPrint(c, e->u.conv.operand);
    case EX_TRY:   return exprMayPrint(c, e->u.try_.operand);
    case EX_NEW:   return exprMayPrint(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprMayPrint(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE:
        return exprMayPrint(c, e->u.coalesce.main) || exprMayPrint(c, e->u.coalesce.fallback);
    default: return false;
    }
}

/* Report whether executing a statement may print, directly or through a call.
 *
 * Params:
 *   c - checker
 *   s - the statement to inspect; NULL counts as "does not print"
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 */
static bool stmtMayPrint(Checker *c, Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ST_VAR:    return exprMayPrint(c, s->u.var.init);
    case ST_ASSIGN: return exprMayPrint(c, s->u.assign.target) || exprMayPrint(c, s->u.assign.value);
    case ST_EXPR:   return exprMayPrint(c, s->u.expr.expr);
    case ST_RETURN: return exprMayPrint(c, s->u.ret.value);
    case ST_IF:     return exprMayPrint(c, s->u.ifs.cond) ||
                           stmtMayPrint(c, s->u.ifs.thenBody) || stmtMayPrint(c, s->u.ifs.elseBody);
    case ST_WHILE:  return exprMayPrint(c, s->u.whiles.cond) || stmtMayPrint(c, s->u.whiles.body);
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            if (stmtMayPrint(c, *(Stmt **)vecAt(&s->u.block.stmts, i))) return true;
        return false;
    case ST_MATCH:
        if (exprMayPrint(c, s->u.match.scrutinee)) return true;
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            if (stmtMayPrint(c, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body)) return true;
        return false;
    default: return false;
    }
}

/* Report whether a function may print, directly or through the calls it makes.
 *
 * Params:
 *   c - checker
 *   f - the function to inspect; NULL means the callee could not be resolved
 *
 * Returns:
 *   True when a `print` / `println` call is reachable from here.
 *
 * Notes:
 *   - `FuncDef.callees` is filled in only after a body has been checked, so it cannot be used
 *     while bodies are still being checked. This walks the AST of the callee instead, which
 *     needs a cycle guard of its own.
 *   - Both an unresolved callee and a recursive one count as printing, which is the safe
 *     direction for an answer that gates reordering.
 */
static bool funcMayPrint(Checker *c, FuncDef *f) {
    if (!f) return true;                       /* unresolved: assume it prints */
    if (f->mayPrintState == 1) return true;
    if (f->mayPrintState == 2) return false;
    if (f->mayPrintState == 3) return true;    /* a cycle: assume it prints */
    f->mayPrintState = 3;
    bool r = stmtMayPrint(c, f->body);
    f->mayPrintState = r ? 1 : 2;
    return r;
}

/* Report whether calling this function has an observable side effect.
 *
 * A call counts when it prints, when it takes a `mut ref` or a `mut` view parameter and may
 * therefore write the argument, when it reaches such a call itself, or when it cannot be
 * resolved at all.
 *
 * Params:
 *   c - checker
 *   f - the callee; NULL means it could not be resolved
 *
 * Returns:
 *   True when reordering this call would change what the program does.
 *
 * Notes:
 *   - A pure getter does not count: swapping two of them cannot be observed. Judging by
 *     "contains a call" instead rejected a benchmark program built from plain accessors, whose
 *     expression `t.get() + (v.get(0) ?? 0)` is entirely side-effect free.
 */
static bool callIsEffectful(Checker *c, FuncDef *f) {
    if (!f) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->type) continue;
        if (p->type->kind == TY_REF && p->type->mut) return true;      /* may write the argument */
        if (p->type->kind == TY_GENERIC && p->type->mut) return true;  /* a `mut` view */
    }
    return funcMayPrint(c, f);
}

/* Report whether an expression contains a call whose reordering would be observable.
 *
 * Params:
 *   c - checker
 *   e - the expression to inspect; NULL counts as "no call"
 *
 * Returns:
 *   True when an effectful call appears anywhere inside, at any depth.
 *
 * Notes:
 *   - This used to `break` out of the switch instead of returning, which dropped control to the
 *     end of the function and returned an indeterminate value: undefined behaviour in the
 *     compiler itself, and `-Wreturn-type` had been reporting it. The consequence was that a
 *     call known to be free of side effects, `pure(x)`, made the answer garbage, so the "an
 *     earlier call is still in place" test fired only sometimes.
 */
static bool exprHasCall(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_CALL: case EX_METHOD: case EX_ASSOC:
        if (callIsEffectful(c, e->func)) return true;
        return false;
    case EX_BIN:      return exprHasCall(c, e->u.bin.left) || exprHasCall(c, e->u.bin.right);
    case EX_UN:       return exprHasCall(c, e->u.un.operand);
    case EX_FIELD:    return exprHasCall(c, e->u.field.obj);
    case EX_INDEX:    return exprHasCall(c, e->u.index.obj) || exprHasCall(c, e->u.index.index);
    case EX_SLICE:    return exprHasCall(c, e->u.slice.obj) || exprHasCall(c, e->u.slice.lo) ||
                             exprHasCall(c, e->u.slice.hi);
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            if (exprHasCall(c, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value)) return true;
        return false;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.arraylit.elems, i))) return true;
        return false;
    case EX_REF: case EX_DEREF: case EX_SIGN: case EX_CONV: case EX_TRY:
        return exprHasCall(c, e->kind == EX_REF ? e->u.ref.operand :
                              e->kind == EX_DEREF ? e->u.deref.operand :
                              e->kind == EX_SIGN ? e->u.sign.operand :
                              e->kind == EX_CONV ? e->u.conv.operand : e->u.try_.operand);
    case EX_NEW:      return exprHasCall(c, e->u.new_.count);
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.gencall.args, i))) return true;
        return false;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            if (exprHasCall(c, *(Expr **)vecAt(&e->u.enumval.args, i))) return true;
        return false;
    case EX_COALESCE: return exprHasCall(c, e->u.coalesce.main) || exprHasCall(c, e->u.coalesce.fallback);
    default:          return false;      /* literals, bindings, and `null` */
    }
}

/* Check an expression, do the per-statement bookkeeping, and store the resulting type.
 *
 * This is the entry point the rest of the checker uses, so the bookkeeping below sees every
 * subexpression of a statement.
 *
 * Params:
 *   c - checker
 *   e - the expression; NULL yields the error type
 *
 * Returns:
 *   The type of the expression, which is also left on `e->type`. A missing type is turned into
 *   the error type, so no caller can observe one.
 *
 * Notes:
 *   - The effect flag records that a call left in place has already been seen in this statement.
 *     A later `??` that needs a temporary would hoist that temporary in front of the call and
 *     reorder the two, so it is reported instead.
 *   - The subject of `??` does not set the flag: its temporary is hoisted together with the
 *     other temporaries in source order, so their relative order is unchanged. Counting it
 *     wrongly reported `println(a, v.get(3) ?? -1, b, v.get(99) ?? -1)`.
 */
Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    if (exprHasCall(c, e) && !(e->kind == EX_COALESCE && e->needTemp)) c->stmtFx = 1;
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

