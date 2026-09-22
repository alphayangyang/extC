/* 表达式检查
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include "check_internal.h"

/* ---------------------------------------------------------------- 表达式 */

Type *checkExpr(Checker *c, Expr *e);

static bool exprHasCall(Checker *c, Expr *e);   /* ⭐ PLAN #22（定义在文件末尾 ✓）*/

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
            if (s && s->modName && !e->qualified)
                requireQualified(c, e->u.ident.name, s->modName, false, e->line);
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
                        /* ⚠️ 以前这里有个 `&& c->curFunc->owner` 的门 —— 那是"只有方法"的写法 ✗
                         *   泛型**自由函数**（PLAN #47）没有 owner ⇒ 那条 `T: ==` 就没人查了 ✗
                         * ⇒ 记下来：实例化时按实例问一遍"这个 T 有没有 `==`" ✓ */
                        if (c->curFunc) {
                            EqCheck *ec = (EqCheck *)arenaAllocZero(c->arena, sizeof(EqCheck));
                            ec->node = e;
                            ec->owner = c->curFunc->owner;   /* 自由函数 = NULL ✓ */
                            ec->op = op;
                            ec->func = c->curFunc;     /* ⭐ #42(c)：记下它属于谁 ✓ */
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
                    e->func = m;  m->used = true;   /* ⭐ #42(c)：这个方法被用到了 ✓ */
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
            Type *ftype = (bt->kind == TY_GENERIC)
                          ? ttSubstitute(tt, fd->type, &sd->typeParams, &bt->targs)
                          : fd->type;
            /* ⭐ 档3：路径收窄 —— `if h.p != null { … }` 里读 `h.p` 直接给**非空**版本 ✓
             * （`narrowTarget` 只在"根是没取过地址的局部"时才发这个事实 ⇒ 这里照用 ✓）
             * 这跟 `EX_IDENT` 那条规则（见本文件上方 `isNarrowed(c, s->cname)`）完全对称 ✓ */
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

            /* 固定数组的长度是**编译期常数** ⇒ 字面量下标越界当场报，
             * 不留到运行时（P′：能证明的必须看得见）。
             * 判据跟切片那三条一致，只是这里只有一个界、且是**半开**的：
             * 合法下标是 [0, n)。`b[-1]` 和 `b[4]`（`[4]T`）都是编译错误。 */
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
            /* ⭐ 档2：取地址 ⇒ 记下"这个绑定的地址出去过"（别名可能出现 ⇒ 禁止强更新）✓
             * ⭐ 档3：同时**作废它的路径收窄事实**（`h.p` 非空这件事不再可靠）✓ */
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
            e->func = f;  f->used = true;   /* ⭐ #42(c) ✓ */

            Vec *sp = NULL, *sa = NULL;
            if (t->kind == TY_GENERIC && sd) { sp = &sd->typeParams; sa = &t->targs; }

            /* A3：关联函数也要算"传哪只 arena"（它自己可能分配、也可能返回引用）✓
             * ⚠️ 以前这里漏了 ⇒ 会生成"少一个实参"的 C（真 bug：`Type::make()` 编不过）✗ */
            e->homeDepth = callHomeDepth(c, &e->u.assoc.args, &f->params);
            setCallArenaArg(c, e);      /* 定案 68：解析成最终要传的那只 arena ✓ */
            /* ⚠️ 检查挪到实参查完之后 ✓ 见本分支末尾 */

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
            /* ⭐ 定案 67：关联函数也一样（实参查完之后 ✓）*/
            checkCallRefArgs(c, f, &e->u.assoc.args, &f->params, e->homeDepth,
                             e->line, e->u.assoc.name);
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
             *    分配出来的东西活到**调用者选的那个作用域** ⇒ 记成 `ARENA_HOME`
             *    （对 `refDepth` 来说是 0 = "外面/参数那一级"，正是逃逸检查里
             *     "可以带出去"那一档 ✓）这样 `fn build() -> mut ref node` 里那句
             *    `return h` 才成立 ✓
             *
             * ⭐ 定案 63（PLAN #38）：**这一层是"起点"，不是"终点"** ——
             * 逃逸检查发现"这个新东西被存进了活得更久的地方" ⇒ 把 `arenaLevel`
             * **提升**到那一层（`check_escape.c` 的 `promoteInto`）✓
             * `refDepth` 跟着 `arenaLevel` 走（两件事必须永远是同一个数）✓
             * ⚠️ 同一个节点**可能被查两遍** ⇒ 只第一次定层、只往"更长寿"的方向调 ✓ */
            /* ⭐ 定案 68：**层号由检查器算定，codegen 只翻译** ——
             * 有家 ⇒ `ARENA_HOME`（家 arena）；否则按块（`@overwrite` 的存储要跨循环每轮
             * ⇒ 住本帧那层 = 1）✓
             * ⚠️ 这里用的是**当时的** `needsHome`（只有直接判据）；"因为调用而有家"的那种
             * 函数由 `checkModule` 在闭包之后用 `FuncDef.newSites` 再定一次 ✓ */
            if (e->arenaLevel == 0)
                e->arenaLevel = (c->curFunc && c->curFunc->needsHome) ? ARENA_HOME
                              : (e->reuse ? 1 : (int)c->scopes.len);
            /* 顺手记下站点（见 `FuncDef.arenaSites` 的注释：闭包之后要回头定它们）✓ */
            *(Expr **)vecPush(&c->curArenaSites) = e;
            /* `refDepth` 跟着 `arenaLevel` 走（两件事必须永远是同一个数）——
             * ⚠️ 只有 `ARENA_HOME` 例外：那个哨兵是 -1，而 `refDepth` 的语言是
             * "0 = 外面那一级"，所以**映射回 0**（家 = 调用者选的作用域 = 深度 0 ✓）*/
            {
                int depth = (e->arenaLevel == ARENA_HOME) ? 0 : e->arenaLevel;
                if (e->refDepth == 0 || e->refDepth > depth) e->refDepth = depth;
            }

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
            /* ⭐ PLAN #47：`maxOf<i32>(4, 3)` —— **显式类型实参**的自由函数调用 ✓
             * （`T` 只出现在返回类型时只能这么写 ✓）
             * 剩下的就是老规矩：只有内建原语能走泛型调用这条路（`alloc<T>(n)` ✓）*/
            if (strcmp(e->u.gencall.name, "alloc") != 0) {
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
                    /* ⭐ PLAN #50：实参里还带 `T`（泛型体里 `idOf<T>(…)`）。
                     * 这里**不用**记账：下面把节点改写成 `EX_CALL` 并**重新走一遍**，
                     * 那一条路自己会看到"实参带 `T`"并记下推迟项 ✓（少写一份，少一处会漂 ✗）*/
                    FuncDef *inst = funcInstance(c, tf, &targs, e->line);
                    inst->used = true;
                    tf->used = true;
                    /* 把节点改写成普通调用（`alloc` 那条路之外的形状 ✓）*/
                    Vec args = e->u.gencall.args;
                    Expr *id = exprNew(c->arena, EX_IDENT, e->line);
                    id->u.ident.name = tf->name;
                    e->kind = EX_CALL;
                    e->u.call.callee = id;
                    e->u.call.args   = args;
                    e->qualified     = true;      /* 别触发"要写限定名"那条 ✓ */
                    e->func = inst;
                    /* ⚠️ 改写成 EX_CALL 之后**重新走一遍**：实参检查、返回值类型、
                     * 家 arena 那些都在 EX_CALL 那条路上 ⇒ 千万别自己返回 void ✗（踩过）*/
                    return checkExpr(c, e);
                }
                ckError(c, e->line, "only the built-in primitives may be called this way",
                        "`%s` is not a built-in primitive or generic function",
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
            /* ⭐ 定案 68：`alloc` 也一样，**层号由检查器算定**（= 当前块）⇒ codegen 不再
             * 自己数块层（以前它走 `arenaRef(g)` = `g->blkLevel`，那是"第二个权威"里
             * 最隐蔽的一个：两个数必须永远相等，一旦不等就是内存问题）✓ */
            e->arenaLevel = (int)c->scopes.len;
            Type *r = ttRef(tt, elem);
            r->mut = true;                       /* 刚分配的地方当然可写 */
            return r;
        }

        case EX_COALESCE: {
            /* `a ?? b` —— **可能没有就兜底**。语义 = `match a { 有(v) => v  _ => b }`，
             * 但**只算一边**：有值时 b 不求值 ✓（跟 `&&` / `||` 的短路同一件事）*/
            /* ⚠️ 旗子必须在**主体还没查之前**取 ✗（主体自己是个调用 ⇒ 查完就把旗子举起来了，
             * 踩过：`var a = h() ?? 0` 这条**第一句**被误报 ✓）*/
            bool priorFx = c->stmtFx != 0;
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
            /* ⭐ PLAN #22（2026-09-22）：主体不纯 ⇒ 要**先算进临时变量**，而那个前缀是吐在
             * **整条语句之前**的 ⇒ 它会跳到同语句里更靠前的副作用前头去 ✗
             *     `g() + (h() ?? 0)`   源码是 g 先、h 后，实际是 **h 先**跑 ✗（实测过）
             * ⇒ 与其偷偷换顺序，不如**报错让他拆两行** ✓（主人：「显式是对的」）✓
             * ⚠️ 判据只看**调用**：`new` / 字面量 / 转换的顺序不可观测 ⇒ 不报 ✓ */
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
                e->needTemp = true;        /* codegen 照做：先算一次，再对临时变量做三元 */
                /* ⚠️ 主体的**全部**副作用都是被"提前算"的（而且跟别的临时变量按源码顺序排 ✓）
                 * ⇒ 它**不算**"留在原地的调用" ✗（踩过：同一个 `println` 里两个 `f() ?? -1`
                 * 被误报 ✓ —— 语料当场抓出来 ✓）
                 * 兜底那边**留在三元里**（只有主体没值才跑）⇒ 它算 ✓ */
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

            /* ⭐ 定案 73：`flush()` —— 把 `println` 那边的缓冲刷出去 ✓
             * 为什么需要它：`std::io` 的 `writeBytes` 走 **fd 直写**（无缓冲，`write(2)`），
             * 而 `println` 走 printf（**有缓冲**）⇒ 混用时**顺序会乱** ✗
             * （交互式程序最难忍：提示语还没出来，程序已经在等你输入了 ✗）*/
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
                    Type *at = checkPrintArg(c, a);   /* 打印的是**值** ⇒ 自动解引用 ✓ */
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, NULL,
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            FuncDef *f = findFunc(c, name);
            if (f && !f->reserved && f->modName && !e->qualified)
                requireQualified(c, name, f->modName, false, e->line);
            if (!f) {
                ckError(c, e->line, "built-ins available: `print(x)` / `println(x)`",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            e->func = f;  f->used = true;   /* ⭐ #42(c) ✓ */

            /* ⭐ PLAN #47：**泛型自由函数** —— `T` 只能从实参推出来（类型实例是类型决定的，
             * 而自由函数的实参只看调用点 ✓）⇒ 先查实参、再推导、再拿实例 ✓
             * 推不出来（`T` 只出现在返回类型）⇒ 教他写显式实参 `f<i32>(…)` ✓ */
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
                    /* `ref T` 形参：实参写 `ref x` 时 `at` 是引用 ⇒ 拿被指类型去合一 ✓ */
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
                /* ⭐ PLAN #50（2026-09-23）：泛型体里调用泛型函数 —— 类型实参里还带 `T`
                 * ⇒ **这个时刻造不出正确的实例**，但也不该报错（那是 v1 的限制，已解）✓
                 * 解法和 `RefCheck`/`EqCheck` 同族：模板期先造一个"以 `T` 为实参"的实例
                 * （`idOf_T`，签名自洽、`T` 当不透明类型用 ⇒ 模板体后面照样能查 ✓），
                 * 同时**记一笔**，等实例化复查时把 `e->func` 改指到具体实例（`idOf_i32`）✓ */
                FuncDef *inst = funcInstance(c, f, &targs, e->line);
                inst->used = true;
                e->func = inst;
                f = inst;                     /* 后面的检查都按实例来 ✓ */

                bool hasParamTarg = false;
                for (size_t i = 0; i < targs.len; i++) {
                    Type *a = *(Type **)vecAt(&targs, i);
                    if (a && ttHasParam(a)) { hasParamTarg = true; break; }
                }
                if (hasParamTarg && c->curFunc) {
                    CallCheck *cc = (CallCheck *)arenaAllocZero(c->arena, sizeof(CallCheck));
                    cc->node = e;
                    cc->tmpl = f->tmpl ? f->tmpl : f;   /* 记**模板**（`f` 已经是实例了 ✓）*/
                    cc->func = c->curFunc;              /* 这条属于哪个模板体 ✓ */
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
            /* A3 第二半：这只 arena 该取"最浅的那个 `mut ref` 实参"那边 ✓ */
            e->homeDepth = callHomeDepth(c, &e->u.call.args, &f->params);
            setCallArenaArg(c, e);      /* 定案 68 ✓ */
            /* ⚠️ 定案 67 的检查必须在**实参查完之后** ✗（不然 `a->type` 还没填 ⇒
             * `typeContainsRef` 一律答"否" ⇒ **静默漏放** ✗✗ —— 方法那条踩过同一个坑 ✓）
             * ⇒ 见本分支末尾 ✓ */
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
            /* ⭐ 定案 67：**每个**调用点都过（不只是"有家"的那些 —— 内容流跟家 arena
             * 无关，是被调者"存进参数所指容器"产生的 ✓）
             * ⚠️ 位置：**实参查完之后**（`exprRefDepth` 要靠实参的类型 ✓）*/
            checkCallRefArgs(c, f, &e->u.call.args, &f->params, e->homeDepth, e->line, name);
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
            e->func = f;  f->used = true;   /* ⭐ #42(c) ✓ */

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
                    /* ⭐ 1.2b：`self: mut ref` 这条分支**不经过** callHomeDepth ⇒ 这里也要看 E ✓
                     * （第一版漏了它 ⇒ ASan 抓到 use-after-free ✗）*/
                    const char *rrn = placeRootName(e->u.method.recv);
                    if (d != 0 && rrn && isEscapeeName(c, rrn)) d = -1;
                    e->homeDepth = (d == 0) ? -1 : d;
                } else {
                    e->homeDepth = callHomeDepth(c, &e->u.method.args, &f->params);
                }
                /* ⭐ 定案 68：解析成"最终传哪只 arena"（两个数都在检查器里定完）✓ */
                setCallArenaArg(c, e);
                /* 甲′(#31)：不管实参是本地的还是参数，都要记"往容器里塞的东西住哪" ✓
                 * ⚠️ 第一版只加在 else 分支里 ⇒ `self: mut ref` 那条（`v.push(…)` 正是它）
                 * 走的是 if 分支 ⇒ **一次都没执行** ✗ 调试才看出来 ✓ */
                /* ⭐ 定案 67：方法也一样 —— **每个**调用点都过（内容流跟家 arena 无关 ✓）
                 * ⚠️⚠️ **接收者要当成第 0 个实参** ✗ —— 被调者的 `params[0]` 是 `self`，
                 * 而 `e->u.method.args` **不含**接收者 ⇒ 两边长度不等 ⇒
                 * `checkCallRefArgs` 开头那句 `params->len != args->len` **静默早退** ✗✗
                 * ⇒ **规则 ④ 对方法调用从来就没生效过**（2026-09-22 抓到的潜缺陷 ✓）
                 * ⇒ 把接收者拼到最前面 ✓*/
                (void)0;   /* ⚠️ 检查挪到下面"实参查完之后" ✓ 见那段注释 */
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
            /* ⚠️⚠️ 规则的检查必须在**实参查完之后** ✗ —— 不然 `a->type` 还没填，
             * `typeContainsRef` 一律答"否" ⇒ 静默漏放 ✗✗（踩过：反例没被拒 ✓）
             * ⭐ 定案 67：接收者拼成第 0 个实参（被调者的 `params[0]` 是 `self` ✓）*/
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

/* ⭐ PLAN #22：这个函数（传递地）**会不会打印**？（走 AST，环保护 ✓）
 * ⚠️ **运行时**的坑：`FuncDef.callees` 是"查完体"之后才填的 ⇒ 查体期间不能靠它 ✗
 * ⇒ 直接走被调者的 AST，自带环保护 ✓ */
static bool funcMayPrint(Checker *c, FuncDef *f);

static bool stmtMayPrint(Checker *c, Stmt *s);

static bool exprMayPrint(Checker *c, Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CALL && e->u.call.callee && e->u.call.callee->kind == EX_IDENT) {
        const char *n = e->u.call.callee->u.ident.name;
        if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return true;
        if (strcmp(n, "flush") == 0) return false;      /* 不是"可观测副作用"里的打印 ✗ */
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

static bool funcMayPrint(Checker *c, FuncDef *f) {
    if (!f) return true;                       /* 拿不准 ⇒ 当"会"（保守 ✓）*/
    if (f->mayPrintState == 1) return true;
    if (f->mayPrintState == 2) return false;
    if (f->mayPrintState == 3) return true;    /* 环 ⇒ 当"会" ✓ */
    f->mayPrintState = 3;
    bool r = stmtMayPrint(c, f->body);
    f->mayPrintState = r ? 1 : 2;
    return r;
}

/* ⭐ PLAN #22：这个表达式里有没有**可观测的副作用**？
 * ⇒ 只认：① `print`/`println` ② **带 `mut ref`/`mut` 视图形参的函数**（它可能写实参）
 *         ③ （传递地）会打印的函数 ④ 拿不准的（解析不出来）✓
 * ⚠️ **纯 getter 不算** —— 顺序换了也看不出来 ✓
 *    （踩过：这一条一开始按"含调用"判 ⇒ 把编译时长基准的合成程序
 *      `t.get() + (v.get(0) ?? 0)` 也拒了 ✗ —— 那是个纯访问器 ✓）*/
static bool callIsEffectful(Checker *c, FuncDef *f) {
    if (!f) return true;
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (!p->type) continue;
        if (p->type->kind == TY_REF && p->type->mut) return true;      /* 可能写实参 ✓ */
        if (p->type->kind == TY_GENERIC && p->type->mut) return true;  /* mut 视图 ✓ */
    }
    return funcMayPrint(c, f);
}

static bool exprHasCall(Checker *c, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_CALL: case EX_METHOD: case EX_ASSOC:
        if (callIsEffectful(c, e->func)) return true;
        /* ⚠️⚠️ **这里以前写的是 `break`**（2026-09-22 修）—— 那会掉出 switch、
         * **落到函数末尾返回一个不确定的值** ✗（编译器自己的 UB！
         *  `-Wreturn-type` 早就报了，只是没人看那份 warning）
         * 后果：`pure(x)` 这种**无副作用**的调用会让本函数返回垃圾 ⇒
         * PLAN #22 那条"前面有没有留在原位的调用"的判据**时灵时不灵** ✗
         * ⇒ 老老实实 `return false` ✓ */
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
    default:          return false;      /* 字面量 / 绑定 / null ✓ */
    }
}

Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    /* ⭐ PLAN #22：记下"本语句里已经有**留在原位**的调用了" ⇒ 后面再遇到"要临时变量的 `??`"
     * 就报错（那个临时变量会跳到它前面去 ✗）✓
     * ⚠️ **`??` 自己的主体不算**：它的临时变量跟别的临时变量是**按源码顺序一起**提前算的
     *    ⇒ 它们之间的顺序**没变** ✓（踩过：`println(a, v.get(3) ?? -1, b, v.get(99) ?? -1)`
     *    被误报了 ✗ —— 语料里 5 个例子当场抓出来 ✓）*/
    if (exprHasCall(c, e) && !(e->kind == EX_COALESCE && e->needTemp)) c->stmtFx = 1;
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

