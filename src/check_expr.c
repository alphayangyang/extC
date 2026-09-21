/* 表达式检查
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include "check_internal.h"

/* ---------------------------------------------------------------- 表达式 */

Type *checkExpr(Checker *c, Expr *e);

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

    /* 字面量按值适配另一边的类型：`u32 + 1` 里 `1` 就是 u32 */
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

/* 位运算：只许整数（`&` `|` `^` `<<` `>>`）。
 * 结果类型跟算术走同一套（不拓宽就报错）——
 * 特意**不**在这里抄一份类型规则，免得两处规则漂开。 */
static bool isBitOp(const char *op) {
    return strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}

static bool isRef(Type *t) { return t && t->kind == TY_REF; }

/* `ref T` 上的算术/比较是**没有意义**的：C 里那是指针算术，语义完全不是用户想的。
 * extC 宁可报错，也不给一个「看起来对、其实在挪指针」的答案。 */
static Type *refNotANumber(Checker *c, Expr *e, Type *lt, Type *rt, const char *op) {
    Type *bad = isRef(lt) ? lt : rt;
    ckError(c, e->line,
            "a `ref` is not a number: in C this would silently become pointer arithmetic. "
            "Use `.field` / `[i]` to operate on what it points to.",
            "cannot apply `%s` to `%s` (a reference)", op, typeStr(c, bad));
    return ttError(c->tt);
}

static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:   return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tSliceU8;

        case EX_NULL: {
            /* 类型是上下文寄放在 e->type 上的（adoptContextType）。
             * 上下文没说清楚 ⇒ 报错，**绝不猜**（显式优于推导）✓ */
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
            if (!s) {
                ckError(c, e->line, "every name must be declared first (extC has no globals yet)",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            /* 名字的**解析**在这里定格 ⇒ 代码生成直接印 `cname`。
             * 遮蔽过的名字（`a` vs `a__2`）就靠这一行分开 ✓ */
            e->u.ident.cname = s->cname;
            /* 已经被 `if p != null` 证明过 ⇒ 交出**非空**引用。
             * 于是 `p.field`、`p.method()`、传给 `ref T` 参数全部自动成立，
             * 而且**一行运行时检查都不用加** ✓ */
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

            /* `&&` / `||` 的**短路**也是收窄点：`p != null && p.value > 3`
             * 右边能直接解 —— 因为走到右边就说明左边成立过 ✓ */
            if (isLogicOp(op0)) {
                Type *lt = checkValue(c, e->u.bin.left);
                expectBool(c, lt, e->u.bin.left);

                bool whenTrue = false;
                const char *tg = narrowTarget(c, e->u.bin.left, &whenTrue);
                const size_t mark = c->narrow.len;
                if (strcmp(op0, "&&") == 0) {
                    /* 走到右边 ⟺ 左边**为真** ⇒ 左边的全部事实都成立 ✓ */
                    narrowFactsOf(c, e->u.bin.left);
                } else if (tg && !whenTrue) {
                    /* `a || b`：走到右边 ⟺ 左边为假。只有最简单的 `p == null` 形态
                     * 能反推出 `p` 非空（`||` 的完整反推要先会否定一个合取，先不做）*/
                    pushNarrow(c, tg);
                }

                Type *rt = checkValue(c, e->u.bin.right);
                expectBool(c, rt, e->u.bin.right);
                c->narrow.len = mark;
                return c->tBool;
            }

            /* `p == null` / `p != null` —— 可空引用**唯一**的用法就是跟 null 比。
             * 必须抢在下面 `checkValue` 之前：`null` 自己是不知道类型的 ✓ */
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

                /* ---- `==` / `!=`：内建原生比；struct 走 eq 方法 ---- */
                if (isEqOp && ttEquals(lt, rt)) {
                    if (cmpIsNative(lt)) return c->tBool;

                    /* 数组：`==` 由**编译器派生**（数组类型用户写不出来，没法用 fn == 实现）*/
                    if (lt->kind == TY_ARRAY) {
                        if (!typeSupportsEq(lt->inner, op))
                            ckError(c, e->line,
                                    "An array's `==` is derived by the compiler, "
                                    "so its elements have to be comparable.",
                                    "`%s` cannot be compared: its element type `%s` does not define `==`",
                                    typeStr(c, lt), typeStr(c, ttBase(lt)->inner));
                        return c->tBool;
                    }

                    /* 泛型参数 → 推迟到实例化再检查（规则 2） */
                    if (lt->kind == TY_PARAM) {
                        e->needEq = true;
                        if (c->curFunc && c->curFunc->owner) {
                            EqCheck *ec = (EqCheck *)arenaAllocZero(c->arena, sizeof(EqCheck));
                            ec->node = e;
                            ec->owner = c->curFunc->owner;
                            ec->op = op;
                            *(EqCheck **)vecPush(&c->eqChecks) = ec;
                        }
                        return c->tBool;
                    }

                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (!sd) {
                        /* 带载荷枚举是最容易撞上这个的类型：C 里它是 struct，
                         * 而 C 的 struct 不能用 `==` ⇒ 告诉用户改用 match ✓ */
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
                                  sd->name, sd->name, sd->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==`, so it cannot be compared",
                                sd->name);
                        return c->tBool;
                    }

                    Vec *sp = NULL, *sa = NULL;
                    if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                    (void)sp; (void)sa;
                    e->func = m;        /* codegen 用它生成 `Type_eq(&a, &b)` */
                    return c->tBool;
                }

                /* ---- 其余比较：只对数值有意义 ---- */
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
            /* 同样的坑：`ttIsNumeric` 会把 `ref T` 抹成 `T`，
             * 于是 `p + 1`（p 是 `ref i64`）被当成数字运算放过去，
             * 生成的 C 却是**指针算术** `p + 1` —— 静默地做完全不是那个意思的事。 */
            if (isRef(lt) || isRef(rt)) return refNotANumber(c, e, lt, rt, op);
            /* 结果类型交给算术那一套统一处理（含字面量适配）——不在这里抄第二份规则 */
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkValue(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
            /* `~` 按位取反只对整数有意义 */
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
            /* 先看是不是枚举变体：`Status.warn`
             * （`Status` 不是变量，而是一个 type 名字）*/
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
                        /* 改写成枚举值节点，codegen 直接用 */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->assocOwner = et;      /* 解析好的类型（泛型实例要用）*/
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
                bufPrintf(&note, "fields of %s:", sd->name);
                for (size_t i = 0; i < sd->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", sd->name, e->u.field.name);
                return ttError(tt);
            }
            e->field = fd;
            /* 字段的类型里可能有泛型参数 —— 用接收者的实参替换掉 */
            if (bt->kind == TY_GENERIC)
                return ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs);
            return fd->type;
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
            return elem;
        }

        case EX_SLICE: {
            /* `a[lo..hi]` —— 只读视图。lo / hi 可以为空（前端 / 后端省略）*/
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

            /* 切固定数组要取元素的地址 ⇒ 底必须是个「地方」。
             * 切 slice 只是对值做指针算术，不需要（所以字符串字面量可以切）。 */
            if (ob->kind == TY_ARRAY && !isPlace(e->u.slice.obj)) {
                ckError(c, e->line, "Slicing takes the address of an element, so the base must be a place.",
                        "cannot slice a temporary value; `%s` needs a variable, a field or an index",
                        typeStr(c, ot));
                return ttError(tt);
            }

            /* 底是固定数组 ⇒ 长度是**编译期常数**，省略的界直接补成字面量：
             *     a[2..] → a[2..15]      a[..] → a[0..15]
             * 于是 codegen 看到的是「两个界都是字面量」，能生成**没有检查**的代码（P）。 */
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

            /* 编译期能证明的错误**当场报**，一个都不留到运行时。
             * 每个界单独看：越界的字面量永远错，跟另一个界是什么无关。 */
            if (ob->kind == TY_ARRAY) {
                long long n = (long long)ob->asize;
                Expr *lp = e->u.slice.lo, *hp = e->u.slice.hi;
                long long lv = 0, hv = 0;
                bool lOk = asIntLit(lp, &lv);
                bool hOk = asIntLit(hp, &hv);

                /* 每个界单独看：越界的字面量永远错，跟另一个界是什么无关。 */
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
            /* 视图的**可写性从切出来的源头继承**：
             *   `var a` / `mut ref` 参数 ⇒ `mut slice<T>`
             *   `let a` / 字符串字面量 / 只读视图 ⇒ `slice<T>`
             * 这就是「一个视图类型」能同时表达两种权限的办法 ——
             * 不用像 Rust 那样给切片造两个类型。 */
            return ttViewMut(tt, sliceOf(c, elem),
                             isWritablePlace(c, e->u.slice.obj));
        }

        case EX_ARRAYLIT: {
            /* 类型从上下文来（`var a: [3]i32 = [...]`）或者从元素推 */
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
            /* **取哪种引用，看这个「地方」可不可写**：
             *   `var` 变量 / `mut ref` 参数 ⇒ `mut ref T`
             *   `let` 变量 / `ref` 参数     ⇒ `ref T`（只读）
             *
             * 只读借用**随时可以取** —— 那正是借用存在的理由。
             * （以前 `ref` 只有可变一种，所以对 `let` 取引用被一律拒绝，
             *   连「只是想读一下」都写不出来。） */
            Type *r = ttRef(tt, ot);
            r->mut = isWritablePlace(c, op);
            return r;
        }

        case EX_TRY:
            /* `?` 是**语句级**的转发：要展开成「求值一次 + 判断 + return」。
             * C 没有语句表达式，所以它只在这三处合法：
             *     let x = e?   /   x = e?   /   return e?
             * 别的地方（比如 `f(e? + 1)`）要报清楚，而不是生成编不过的 C。 */
            ckError(c, e->line,
                    "`?` has to expand into statements (evaluate once, test, return), "
                    "so it only fits where a whole statement can be rewritten.",
                    "`?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?`");
            return ttError(tt);

        case EX_ASSOC: {            /* `option<i64>::some(x)` —— 关联函数（struct 体内不带 `self` 的函数）。
             * 类型实参**写全**，不从参数或上下文倒推（定案 27：显式优于推导）。 */
            Type *raw = typeNamed(c->arena, e->u.assoc.typeName);
            raw->targs = e->u.assoc.targs;
            Type *t = ttResolve(tt, c->ctx, raw, e->line, c->curParams);
            if (ttIsError(t)) return ttError(tt);
            e->assocOwner = t;

            /* **枚举变体的构造**也走这条路：`option<i64>::some(3)` / `maybe<i64>::nothing`。
             * 也就是说 `::` 对枚举可以有两种读法，但**含义只有一种**（造一个值）——
             * 而且这样 prelude 和现有代码里的写法一个字都不用改 ✓ */
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
                Vec evargs = e->u.assoc.args;      /* union：先拿出来 */
                e->kind = EX_ENUMVAL;
                e->u.enumval.typeName = t->name;   /* 实例名，如 `maybe_i64` */
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
                    bufPrintf(&note, "associated functions of %s:", sd->name);
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
            e->func = f;

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* A3：关联函数也要算"传哪只 arena"（它自己可能分配、也可能返回引用）✓
             * ⚠️ 以前这里漏了 ⇒ 会生成"少一个实参"的 C（真 bug：`Type::make()` 编不过）✗ */
            e->homeDepth = callHomeDepth(c, &e->u.assoc.args, &f->params);
            raiseMutRefTargets(c, f, NULL, &e->u.assoc.args, &f->params, e->homeDepth);
            if (f->needsHome)
                checkCallRefArgs(c, &e->u.assoc.args, &f->params, e->homeDepth,
                                 e->line, e->u.assoc.name);

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
            return f->ret ? ttSubstitute(tt, f->ret, sp, sa) : ttVoid(tt);
        }

        case EX_CONV: {
            /* `i32(x)` —— 显式转换（PLAN #23）：
             *   · 拓宽（已经自动）⇒ 写出来也行，**不生成任何检查** ✓
             *   · **收窄 / 换符号** ⇒ 装不下就 **trap（带源码位置）** ✓
             *   · 整数 ↔ 浮点 ⇒ C 的规则（截断 / 就近舍入），浮点转整数要查范围 ✓
             * 能**证明**装得下的（比如 `i32(u8 值)`）⇒ 零检查 ✓（P）*/
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
            /* 浮点之间的转换按 C 走（就近舍入；超出范围变 ±inf）⇒ 不查 ✓
             * 整数之间能**拓宽**的（无损失、同符号方向）也不查 ✓
             * 其余（收窄 / 换符号 / 浮点转整数）查 ✓ */
            bool lossless = (si && ti && ttCanWiden(src, t)) || (sf && tf);
            e->convCheck = !lossless;
            return t;
        }

        case EX_NEW: {
            /* `new T` / `new [N]T` / `new T[n]`（PLAN A1）
             *   单个 T        ⇒ `mut ref T`
             *   固定数组 [N]T ⇒ `mut ref [N]T`
             *   `T[n]`        ⇒ `mut slice<T>`（**造 buffer 靠这个** ✓）
             * 分配进**当前块**的 arena、**清零**（extC 里分配出来的一定是零 ✓）*/
            Type *w = ttResolve(tt, c->ctx, e->u.new_.type, e->line, c->curParams);
            if (ttIsError(w)) return ttError(tt);
            e->u.new_.type = w;

            if (w->kind == TY_PARAM || ttHasParam(w)) {
                /* ⭐ 模板期 `T` 没有大小 ⇒ **推迟到实例化再查**（记录一笔）✓
                 * 这样 `varArray<T>` 里的 `new T[cap]` 才写得出来 ✓
                 * ⚠️ 实例化后还是不确定的（比如嵌套泛型没传到底）要在那里报错 ✓ */
                recordNewSizeCheck(c, w, e->line);
            } else if (ttIs(w, "void") || ttIsError(w)) {
                ckError(c, e->line,
                        "`new` needs a concrete type: its size is decided at compile time",
                        "cannot `new` `%s` -- its size is not known here", typeStr(c, w));
                return ttError(tt);
            }

            /* ⭐ 深度 = **当前块深度** —— 跟生成的 C 选 `__extc_a[k]` 用同一个数 ✓
             * ⚠️ 例外：这个函数有"家"arena（A3：它会把自己的分配交给调用者）⇒
             *    分配出来的东西活到**调用者选的那个作用域** ⇒ 深度按 0 算 ✓
             *    （0 = "外面/参数那一级"，正是逃逸检查里"可以带出去"那一档 ✓）
             *    这样 `fn build() -> mut ref node` 里那句 `return h` 才成立 ✓ */
            e->refDepth = c->curFunc && c->curFunc->needsHome ? 0 : c->scopes.len;

            if (!e->u.new_.count) {
                Type *r = ttRef(tt, w);
                r->mut = true;                  /* 刚分配的地方当然可写 ✓ */
                return r;
            }

            /* `T[n]` —— 个数必须是整数；个数不纯的话要引临时变量（别求值两次）*/
            Type *nt = checkValue(c, e->u.new_.count);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, e->u.new_.count->line, NULL,
                        "the element count must be an integer, found `%s`", typeStr(c, nt));
                return ttError(tt);
            }
            if (!repeatablePure(e->u.new_.count)) e->needTemp = true;
            /* 刚分配出来的内存当然**可写** ⇒ 给 `mut slice<T>` ✓
             * （跟 `a[..]` 在可写的地方切片得到 `mut slice<T>` 是同一条规则）*/
            return ttViewMut(tt, sliceOf(c, w), true);
        }

        case EX_GENCALL: {
            /* 泛型调用 —— 目前**只有内置原语**用它：`alloc<i32>(n)`。
             * 它向当前函数帧的 arena 要 `n` 个 T 的地方，返回可写引用。
             *
             * 为什么它是原语而不是库函数：**arena 本身必须由编译器生成**
             * （库要用它来分配自己 ⇒ 不能由库提供）。见 BOOTSTRAP 规则二。 */
            if (strcmp(e->u.gencall.name, "alloc") != 0) {
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive (only `alloc<T>(n)` is)",
                        e->u.gencall.name);
                return ttError(tt);
            }
            if (e->u.gencall.targs.len != 1) {
                ckError(c, e->line, NULL, "`alloc` needs exactly one type argument, e.g. `alloc<i32>(4)`");
                return ttError(tt);
            }
            Type *elem = ttResolve(tt, c->ctx, *(Type **)vecAt(&e->u.gencall.targs, 0),
                                   e->line, c->curParams);
            if (ttIsError(elem)) return ttError(tt);
            /* 把**解析后的**类型写回去 —— codegen 看的是 targs，不是局部变量 */
            *(Type **)vecAt(&e->u.gencall.targs, 0) = elem;
            if (e->u.gencall.args.len != 1) {
                ckError(c, e->line, NULL, "`alloc` takes one argument (how many elements)");
                return ttError(tt);
            }
            Expr *n = *(Expr **)vecAt(&e->u.gencall.args, 0);
            Type *nt = checkValue(c, n);
            if (!ttIsError(nt) && !ttIsInteger(nt)) {
                ckError(c, n->line, NULL, "the element count must be an integer, found `%s`",
                        typeStr(c, nt));
                return ttError(tt);
            }

            /* ⭐ 这块内存活到**当前块结束**（arena 按块细化，PLAN A2）⇒
             * 它的深度就是**当前块的深度** `c->scopes.len` ✓
             *
             * ⚠️ 这一行是"深度模型"和"arena 粒度"的**接缝** —— 两边必须是同一个数：
             *   检查器用块深度判断"引用能不能存进这里"，
             *   生成的 C 用块深度选 `__extc_a[k]`、出块就 release。
             *   写死 1（老行为）的话，`while { p = alloc<i32>(1) }` 里 p 会在
             *   下一次迭代时指向**已经释放**的内存 ⇒ 悬垂 ✗（A2 之后实测过）✓
             * 于是「返回一块刚 alloc 的内存」「把循环里分配的东西存到循环外」
             * 都会被逃逸检查拦住 ✓ 想把它交出去，就得让调用者提供 buffer/arena ✓ */
            e->refDepth = c->scopes.len;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* 刚分配的地方当然可写 */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b` —— **可能没有就兜底**。语义 = `match a { 有(v) => v  _ => b }`，
             * 但**只算一边**：有值时 b 不求值 ✓（跟 `&&` / `||` 的短路同一件事）*/
            Type *mt = checkExpr(c, e->u.coalesce.main);
            if (ttIsError(mt)) { checkExpr(c, e->u.coalesce.fallback); return ttError(tt); }

            bool isOpt = isProtoType(mt, "option", 1);
            bool isRes = isProtoType(mt, "result", 2);
            Type *want = NULL;
            if (isOpt || isRes) {
                want = *(Type **)vecAt(&mt->targs, 0);          /* 载荷类型 T */
            } else if (mt->kind == TY_REF && mt->nullable) {
                want = mt;                                       /* `?ref T` ⇒ 兜底也是引用 */
            } else {
                /* ⚠️ 位置规则：`??` 只对"可能没有"的东西有意义 —— 说得清楚、报得响 ✓ */
                checkExpr(c, e->u.coalesce.fallback);
                ckError(c, e->line,
                        "`??` means \"if there is nothing, use this instead\", so the left"
                        " side must be an `option`, a `result`, or a `?ref T`",
                        "left of `??` is `%s`, which always has a value", typeStr(c, mt));
                return ttError(tt);
            }

            /* 主体**不是**"没有副作用的东西"（比如 `f() ?? -1`）⇒ 不能直接生成三元
             * （主体在 C 里出现两次 ⇒ `f()` 会跑两遍 ✗）⇒ 必须**提前求值到一个临时变量**。
             * 那需要"往所在语句前面吐前缀"的能力：大多数位置有，**两个位置没有** ✓ */
            if (!repeatablePure(e->u.coalesce.main)) {
                checkExpr(c, e->u.coalesce.fallback);
                if (c->noHoist) {
                    ckError(c, e->line,
                            "`while` re-evaluates its condition every round, but a temporary"
                            " can only be computed once, before the loop. Bind it inside the"
                            " loop body: `while true { let r = f()  if ... { break } }`",
                            "`??` here cannot be evaluated ahead of time -- the temporary"
                            " would run once instead of every round");
                    return ttError(tt);
                }
                e->needTemp = true;        /* codegen 照做：先算一次，再对临时变量做三元 */
                return want;
            }

            adoptContextType(e->u.coalesce.fallback, want);
            Type *ft = checkInto(c, want, e->u.coalesce.fallback);
            checkAssignable(c, want, ft, e->u.coalesce.fallback, "the right side of `??`");
            e->type = want;
            return want;
        }

        case EX_SIGN: {
            /* `e!` —— **我签字**（定案 1.3）。
             * 编译器证不出来的事，用户签字负责 ⇒ **一行检查都不生成** ✓
             * 三个用途：
             *   `opt!`（`option<T>`）⇒ 直接给载荷 T
             *   `r!`（`result<T,E>`）⇒ 直接给载荷 T（失败时的行为 = 签字，UB 算他的）
             *   `p!`（`?ref T`）⇒ 我知道非空，给我 `ref T` ✓（定案 ㊻ 留的逃生舱）
             * 不是这三样 ⇒ 报错：签字也要签在对的地方 ✓ */
            Type *ot = checkExpr(c, e->u.sign.operand);
            if (ttIsError(ot)) return ttError(tt);
            if (isProtoType(ot, "option", 1) || isProtoType(ot, "result", 2)) {
                return *(Type **)vecAt(&ot->targs, 0);      /* 载荷类型 */
            }
            if (ot->kind == TY_REF) {
                if (!ot->nullable) {
                    ckError(c, e->line, "it is already a plain `ref T`, which can never be null",
                            "`%s` is not nullable, so `!` has nothing to assert",
                            typeStr(c, ot));
                    return ttError(tt);
                }
                Type *nn = ttRef(tt, ot->inner);            /* 非空版本 ✓ */
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
            /* `*p` = "p 指的那个值/那个地方"。可写性由 **p 的类型** 给（`mut ref`）✓ */
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
            /* 泛型枚举的**实例**（`maybe<i64>`）不在类型表的按名字表里 ——
             * 它是实例化出来的。所以走 EX_ASSOC 那条路构造时会把解析好的类型
             * 记在 `assocOwner` 上，这里优先用它 ✓ */
            Type *et = e->assocOwner ? e->assocOwner : ttFromName(tt, e->u.enumval.typeName);
            if (!et || et->kind != TY_ENUM || !et->edef) return ttError(tt);
            Variant *v = findVariant(et->edef, e->u.enumval.variant);
            if (!v) return ttError(tt);

            /* 无载荷变体：`status.ok` —— 不能带参数 */
            if (v->types.len == 0) {
                if (e->u.enumval.args.len > 0) {
                    ckError(c, e->line, NULL,
                            "`%s.%s` carries no payload, so it takes no arguments",
                            et->name, v->name);
                    return ttError(tt);
                }
                return et;
            }

            /* **带载荷变体的构造**：`shape.circle(2.0)` —— 载荷按位置对 */
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
                /* 载荷装进这个值里 ⇒ 它的引用不能活得比这个值短 */
                checkEscape(c, arg, c->scopes.len, e->line, "this payload value");
            }
            return et;
        }

        case EX_CALL: {
            /* **带载荷变体的构造**：`shape.circle(2.0)` —— 它长得像"字段访问 + 调用"，
             * 但 `shape` 是个类型名不是变量 ⇒ 认出它是枚举构造，改写成 EX_ENUMVAL ✓ */
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
                        Vec args = e->u.call.args;      /* union：先拿出来再改 kind */
                        e->kind = EX_ENUMVAL;
                        e->u.enumval.typeName = et->name;
                        e->u.enumval.variant  = v->name;
                        e->u.enumval.args     = args;
                        e->assocOwner = et;
                        return checkExprInner(c, e);    /* 剩下的交给 EX_ENUMVAL 那条 */
                    }
                }
            }

            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "only direct calls to a function name are supported for now",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkPrintArg(c, a);   /* 打印的是**值** ⇒ 自动解引用 ✓ */
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            FuncDef *f = findFunc(c, name);
            if (!f) {
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            e->func = f;

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            /* A3 第二半：这只 arena 该取"最浅的那个 `mut ref` 实参"那边 ✓ */
            e->homeDepth = callHomeDepth(c, &e->u.call.args, &f->params);
            raiseMutRefTargets(c, f, NULL, &e->u.call.args, &f->params, e->homeDepth);
            if (f->needsHome)
                checkCallRefArgs(c, &e->u.call.args, &f->params, e->homeDepth, e->line, name);
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkInto(c, p->type, a);

                /* T3：形参是 `ref T` 时，值必须在调用点显式写 `ref` ——
                 * 「这里传的是引用不是拷贝」要让读代码的人一眼看见（P′）。
                 * 实参本身就是引用的话，直接传即可。*/
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
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_METHOD: {
            /* **带载荷变体的构造**也可能长这样：`shape.circle(2.0)` 的语法形状
             * 跟"方法调用"一模一样（`接收者.名字(参数)`）—— 区别在于 `shape`
             * 是个**类型名**而不是变量。所以先在这里认一次 ✓ （跟 `status.ok`
             * 在 EX_FIELD 那条路上被认出来是同一件事。）*/
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

            /* 方法只住在 struct 体内 —— 按接收者的类型去找 */
            Type *recvT = checkExpr(c, e->u.method.recv);
            if (rejectNullableDeref(c, recvT, e->u.method.recv, "a method call")) return ttError(tt);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            if (!f) {
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "methods of %s:", sd->name);
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
            e->func = f;

            /* 方法要**可写借用**（`self: mut ref T`）⇒ 接收者必须可写。
             * 这是「签名不说实话」的另一半：光看调用点 `x.bump()` 看不出它会不会改 x，
             * 而 `self: mut ref` 让**签名说了**，这里就把它落实。 */
            {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type->kind == TY_REF && selfP->type->mut &&
                    requireMutable(c, e->u.method.recv, e->line, "call a method that writes"))
                    return ttError(tt);
            }

            /* A3 第二半：**接收者就是那个"最浅的 `mut ref` 实参"**（`self` 排第一）
             * ⇒ 新东西跟着接收者所在的 arena 走 ✓ */
            {
                Param *selfP = *(Param **)vecAt(&f->params, 0);
                if (selfP->type && selfP->type->kind == TY_REF && selfP->type->mut) {
                    int d = placeDepth(c, e->u.method.recv);
                    e->homeDepth = (d == 0) ? -1 : d;
                } else {
                    e->homeDepth = callHomeDepth(c, &e->u.method.args, &f->params);
                }
                /* 甲′(#31)：不管实参是本地的还是参数，都要记"往容器里塞的东西住哪" ✓
                 * ⚠️ 第一版只加在 else 分支里 ⇒ `self: mut ref` 那条（`v.push(…)` 正是它）
                 * 走的是 if 分支 ⇒ **一次都没执行** ✗ 调试才看出来 ✓ */
                raiseMutRefTargets(c, f, e->u.method.recv, &e->u.method.args, &f->params, e->homeDepth);
                if (f->needsHome)
                    checkCallRefArgs(c, &e->u.method.args, &f->params, e->homeDepth,
                                     e->line, e->u.method.name);
            }

            /* 接收者是泛型实例时，方法签名里的 T 要换成实参 */
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
            return rt;
        }

        case EX_STRUCTLIT: {
            /* 类型要么来自字面量里的名字，要么来自上下文（泛型实例只能靠上下文） */
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
                        "`%s` is generic; type arguments cannot be inferred here", sd->name);
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "fields of %s:", sd->name);
                    for (size_t k = 0; k < sd->fields.len; k++)
                        bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&sd->fields, k))->name);
                    ckError(c, fi->value->line, bufCstr(&note),
                            "struct `%s` has no field `%s`", sd->name, fi->name);
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

            /* 省略的字段靠「零值」补齐 —— 但 `ref` 没有零值。
             * 不管的话会生成 `.field = 0`，也就是一个**空引用**，
             * 而语言层明明说 `ref` 不可为空。（跟 str 那次是同一类洞。）*/
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

Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

