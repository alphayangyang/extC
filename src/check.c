/* 类型检查 pass。
 *
 * 唯一做类型推理的地方。它把结果写回 AST，之后 codegen 只负责翻译。
 *
 * 检查原则（对应 DESIGN §3）：
 *   · 拓宽（无损失）自动允许
 *   · 收窄（有损失）一律禁止 —— 有损转换必须写出方法名
 *   · 字面量的类型可以按**值**适配（`let x: f32 = 3.14`、`let x: u8 = 200`）
 */

#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 状态 */

typedef struct {
    const char *name;
    Type       *type;
    bool        mut;
    int         line;
} Sym;

typedef struct { Vec syms; } Scope;

typedef struct {
    Ctx       *ctx;
    Arena     *arena;
    TypeTable *tt;
    Module    *m;
    Vec        scopes;      /* Scope* */
    FuncDef   *curFunc;
    Vec       *curParams;   /* 当前可见的泛型参数名（NULL = 不在泛型上下文）*/
    Vec        eqChecks;    /* EqCheck* —— 推迟到实例化复查的 `==` */

    Type *tI32, *tF64, *tBool, *tStr;
} Checker;

/* ---------------------------------------------------------------- 报错 */

static void ckError(Checker *c, int line, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(c->ctx, line, 1, note, "%s", tmp);
}

static const char *typeStr(Checker *c, Type *t) {
    Buf b;
    bufInit(&b, c->arena);
    ttRender(t, &b);
    return bufCstr(&b);
}

/* ---------------------------------------------------------------- 作用域 */

static void pushScope(Checker *c) {
    Scope *s = (Scope *)arenaAllocZero(c->arena, sizeof(Scope));
    vecInit(&s->syms, c->arena, sizeof(void *));
    *(Scope **)vecPush(&c->scopes) = s;
}

static void popScope(Checker *c) {
    if (c->scopes.len) c->scopes.len--;
}

static void declare(Checker *c, const char *name, Type *t, bool mut, int line) {
    Scope *top = *(Scope **)vecAt(&c->scopes, c->scopes.len - 1);

    for (size_t i = 0; i < top->syms.len; i++) {
        Sym *old = *(Sym **)vecAt(&top->syms, i);
        if (strcmp(old->name, name) == 0) {
            ckError(c, line, NULL, "`%s` is already declared in this scope", name);
            return;
        }
    }

    Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
    s->name = name;
    s->type = t;
    s->mut = mut;
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
}

static Sym *lookup(Checker *c, const char *name) {
    for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *s = *(Scope **)vecAt(&c->scopes, i);
        for (size_t j = 0; j < s->syms.len; j++) {
            Sym *sym = *(Sym **)vecAt(&s->syms, j);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 查找 */

static FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

static FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}

/* TY_STRUCT 和 TY_GENERIC 都指向一个 StructDef */
static StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/* 方法只住在 struct 体内（定案 9）*/
static FuncDef *findMethod(Type *st, const char *name) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

static Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* 能取引用的东西：变量和字段 */
static bool isLvalue(Expr *e) {
    return e->kind == EX_IDENT || e->kind == EX_FIELD;
}

/* C 原生就能比的类型：数值 / bool / 枚举。
 * `str` **不在**这里 —— 它的 `==` 会退化成指针比较（陷阱），必须有 eq 才行。 */
static bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    return ttBase(t)->kind == TY_ENUM;
}

/* 找类型上定义的运算符方法。
 * `!=` 是特例：没定义 `!=` 就退回用 `==` 取反（codegen 那边会自动取反）。*/
static FuncDef *findOp(Type *b, const char *sym, const char *fallback) {
    FuncDef *m = findMethod(b, sym);
    if (!m && fallback) m = findMethod(b, fallback);
    return m;
}

/* 运算符方法的签名必须是：(self: ref T, other: T 或 ref T) -> bool */
static bool eqSignatureOk(TypeTable *tt, FuncDef *m, Type *lt, Vec *sp, Vec *sa) {
    if (!m || !m->ret || !ttIs(m->ret, "bool")) return false;
    if (m->params.len != 2) return false;

    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    Type *t0 = ttSubstitute(tt, p0->type, sp, sa);
    Type *t1 = ttSubstitute(tt, p1->type, sp, sa);

    if (t0->kind != TY_REF) return false;
    return ttEquals(ttBase(t0), lt) && ttEquals(ttBase(t1), lt);
}

/* 这个类型能不能用 `==`？*/
static bool typeSupportsEq(Checker *c, Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (cmpIsNative(t)) return true;

    Type *b = ttBase(t);
    StructDef *sd = structOf(b);
    if (!sd) return false;

    FuncDef *m = findOp(b, "==", NULL);
    Vec *sp = NULL, *sa = NULL;
    if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
    return eqSignatureOk(c->tt, m, b, sp, sa);
}

/* 泛型里的 `==` 推迟到实例化才检查 —— 这里记一笔 */
typedef struct { Expr *node; StructDef *owner; } EqCheck;

/* ---------------------------------------------------------------- 小工具 */

static bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

static bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

static bool isNumericLit(Expr *e) {
    return e->kind == EX_INT || e->kind == EX_FLOAT;
}

static bool isPrintable(Type *t) {
    Type *b = ttBase(t);
    if (!b) return false;
    if (b->kind == TY_BUILTIN) return true;
    /* 定案 11：无载荷枚举自动有名字文本 */
    if (b->kind == TY_ENUM) return true;
    /* struct 由编译器生成 <Type>_debug 递归打印；泛型实例同理 */
    return b->kind == TY_STRUCT || b->kind == TY_GENERIC;
}

/* 字面量的类型按**值**适配目标类型（DESIGN §5 的「字面量类型推导」）。 */
static bool literalFits(Expr *e, Type *want) {
    Type *w = ttBase(want);
    if (!w) return false;
    if (ttIsError(w)) return true;

    if (e->kind == EX_INT) {
        int bits = ttIntBits(w);
        if (bits > 0) {
            long long v = e->u.ival;
            if (bits == 64) return ttIntSigned(w) || v >= 0;
            if (ttIntSigned(w)) {
                long long lo = -(1LL << (bits - 1));
                long long hi = (1LL << (bits - 1)) - 1;
                return v >= lo && v <= hi;
            }
            if (v < 0) return false;
            return (unsigned long long)v <= ((1ULL << bits) - 1);
        }
        return ttIsFloat(w);                    /* 整数字面量给浮点变量 */
    }

    if (e->kind == EX_FLOAT) return ttIsFloat(w);
    if (e->kind == EX_STR)   return ttIs(w, "str");
    if (e->kind == EX_BOOL)  return ttIs(w, "bool");
    return false;
}

/* 裸 `{}` 从上下文拿类型。只在上下文能唯一确定类型的地方成立。 */
static void adoptContextType(Expr *e, Type *want) {
    if (!e || !want) return;
    Type *w = ttBase(want);
    if (!w) return;
    if (w->kind != TY_STRUCT && w->kind != TY_GENERIC) return;
    /* 把上下文类型**寄放**在 e->type 上（checkExpr 之后会被覆盖成同一个类型）。
     * 泛型实例没有名字可查，所以必须走这条路。 */
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) e->type = w;
}

static bool checkAssignable(Checker *c, Type *want, Type *got, Expr *node, const char *what) {
    if (ttIsError(want) || ttIsError(got)) return true;
    if (ttEquals(want, got)) return true;
    if (ttCanWiden(got, want)) return true;
    if (node && literalFits(node, want)) return true;

    /* 整数字面量放不下，跟「类型不匹配」是两回事，报错要分开。
     * （浮点字面量给整数变量属于有损转换，走下面的通用消息。） */
    if (node && node->kind == EX_INT && ttIsInteger(want)) {
        ckError(c, node->line, NULL,
                "%s: literal `%lld` does not fit in `%s`",
                what, node->u.ival, typeStr(c, want));
        return false;
    }

    ckError(c, node ? node->line : 0,
            "extC 只自动做**无损失**的拓宽；有损转换必须显式写出来",
            "%s expects `%s`, found `%s`", what, typeStr(c, want), typeStr(c, got));
    return false;
}

static void expectBool(Checker *c, Type *t, Expr *node) {
    if (ttIsError(t)) return;
    if (!ttIs(t, "bool")) {
        ckError(c, node ? node->line : 0, "条件必须是 `bool`（extC 没有隐式真值转换）",
                "expected `bool`, found `%s`", typeStr(c, t));
    }
}

/* ---------------------------------------------------------------- 表达式 */

static Type *checkExpr(Checker *c, Expr *e);

static Type *checkArith(Checker *c, Expr *e, Type *lt, Type *rt) {
    Type *err = ttError(c->tt);
    if (ttIsError(lt) || ttIsError(rt)) return err;

    const char *op = e->u.bin.op;
    if (!ttIsNumeric(lt) || !ttIsNumeric(rt)) {
        ckError(c, e->line, "算术运算符只接受数值类型",
                "cannot apply `%s` to `%s` and `%s`", op, typeStr(c, lt), typeStr(c, rt));
        return err;
    }
    if (strcmp(op, "%") == 0 && (!ttIsInteger(lt) || !ttIsInteger(rt))) {
        ckError(c, e->line, "`%` 只对整数有意义",
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

    ckError(c, e->line, "两种类型之间没有无损失的转换，先显式转一下再来运算",
            "`%s` and `%s` have no common type for `%s`",
            typeStr(c, lt), typeStr(c, rt), op);
    return err;
}

static Type *checkExprInner(Checker *c, Expr *e) {
    TypeTable *tt = c->tt;

    switch (e->kind) {
        case EX_INT:   return c->tI32;
        case EX_FLOAT: return c->tF64;
        case EX_BOOL:  return c->tBool;
        case EX_STR:   return c->tStr;

        case EX_IDENT: {
            Sym *s = lookup(c, e->u.ident.name);
            if (!s) {
                ckError(c, e->line, "所有名字都要先声明（week-1 还没有全局变量）",
                        "undefined name `%s`", e->u.ident.name);
                return ttError(tt);
            }
            return s->type;
        }

        case EX_BIN: {
            Type *lt = checkExpr(c, e->u.bin.left);
            Type *rt = checkExpr(c, e->u.bin.right);
            const char *op = e->u.bin.op;

            if (isLogicOp(op)) {
                expectBool(c, lt, e->u.bin.left);
                expectBool(c, rt, e->u.bin.right);
                return c->tBool;
            }
            if (isCmpOp(op)) {
                if (ttIsError(lt) || ttIsError(rt)) return c->tBool;

                bool isEqOp = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);

                /* ---- `==` / `!=`：内建原生比；struct 走 eq 方法 ---- */
                if (isEqOp && ttEquals(lt, rt)) {
                    if (cmpIsNative(lt)) return c->tBool;

                    /* 泛型参数 → 推迟到实例化再检查（规则 2） */
                    if (lt->kind == TY_PARAM) {
                        e->needEq = true;
                        if (c->curFunc && c->curFunc->owner) {
                            EqCheck *ec = (EqCheck *)arenaAllocZero(c->arena, sizeof(EqCheck));
                            ec->node = e;
                            ec->owner = c->curFunc->owner;
                            *(EqCheck **)vecPush(&c->eqChecks) = ec;
                        }
                        return c->tBool;
                    }

                    Type *b = ttBase(lt);
                    StructDef *sd = structOf(b);
                    if (!sd) {
                        ckError(c, e->line,
                                "`str` 的 `==` 会退化成 C 的指针比较 —— 那是个陷阱，所以现在不许比。"
                                "等 T4c 把字符串换成 `Slice<u8>` 之后，prelude 会给它一个按内容比较的 `eq`。",
                                "`%s` does not support `%s`", typeStr(c, lt), op);
                        return c->tBool;
                    }

                    FuncDef *m = findOp(b, op, strcmp(op, "!=") == 0 ? "==" : NULL);
                    if (!m) {
                        Buf note;
                        bufInit(&note, c->arena);
                        bufPrintf(&note,
                                  "在 `%s` 里定义它即可：\n"
                                  "      fn ==(self: ref %s, other: %s) -> bool { ... }",
                                  sd->name, sd->name, sd->name);
                        ckError(c, e->line, bufCstr(&note),
                                "`%s` does not define `==`, so it cannot be compared",
                                sd->name);
                        return c->tBool;
                    }

                    Vec *sp = NULL, *sa = NULL;
                    if (b->kind == TY_GENERIC) { sp = &sd->typeParams; sa = &b->targs; }
                    if (!eqSignatureOk(tt, m, b, sp, sa)) {
                        ckError(c, e->line,
                                "运算符方法的签名必须是 `fn ==(self: ref T, other: T) -> bool`",
                                "`%s.%s` has the wrong signature", sd->name, m->name);
                        return c->tBool;
                    }
                    e->func = m;        /* codegen 用它生成 `Type_eq(&a, &b)` */
                    return c->tBool;
                }

                /* ---- 其余比较：只对数值有意义 ---- */
                if (ttIsNumeric(lt) && ttIsNumeric(rt) &&
                    (ttCanWiden(lt, rt) || ttCanWiden(rt, lt))) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;

                ckError(c, e->line, isEqOp ? "只有数值、bool、枚举和带 `eq` 的 struct 能比" : NULL,
                        "cannot compare `%s` with `%s`", typeStr(c, lt), typeStr(c, rt));
                return c->tBool;
            }
            return checkArith(c, e, lt, rt);
        }

        case EX_UN: {
            Type *ot = checkExpr(c, e->u.un.operand);
            if (strcmp(e->u.un.op, "!") == 0) {
                expectBool(c, ot, e->u.un.operand);
                return c->tBool;
            }
            if (ttIsError(ot)) return ot;
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
                            bufPrintf(&note, "%s 的变体：", et->name);
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
                        return et;
                    }
                }
            }

            Type *bt = ttBase(checkExpr(c, e->u.field.obj));
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
                bufPrintf(&note, "%s 的字段：", sd->name);
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

        case EX_REF: {
            Expr *op = e->u.ref.operand;
            Type *ot = checkExpr(c, op);
            if (ttIsError(ot)) return ttError(tt);

            if (ot->kind == TY_REF) {
                ckError(c, e->line, "它本来就是引用，直接传就行",
                        "`ref` applied to a value that is already a reference");
                return ot;
            }
            if (!isLvalue(op)) {
                ckError(c, e->line, "只有变量和字段可以取引用",
                        "cannot take a reference to this expression");
                return ttError(tt);
            }
            /* `ref T` 是**可变**引用（方法靠它改调用者的数据），
             * 所以不能对 `let` 取引用 */
            if (op->kind == EX_IDENT) {
                Sym *s = lookup(c, op->u.ident.name);
                if (s && !s->mut) {
                    ckError(c, e->line, "`ref T` 是可变引用；只读的值直接按值传",
                            "cannot take a mutable reference to `%s`, which is a `let`",
                            s->name);
                    return ttError(tt);
                }
            }
            return ttRef(tt, ot);
        }

        case EX_ENUMVAL:
            return ttFromName(tt, e->u.enumval.typeName);

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) {
                ckError(c, e->line, "week-1 只支持直接调用函数名",
                        "only direct function calls are supported");
                return ttError(tt);
            }
            const char *name = e->u.call.callee->u.ident.name;

            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                for (size_t i = 0; i < e->u.call.args.len; i++) {
                    Expr *a = *(Expr **)vecAt(&e->u.call.args, i);
                    Type *at = checkExpr(c, a);
                    if (!isPrintable(at) || ttIs(at, "void")) {
                        ckError(c, a->line, "week-1 的 println 只支持内建类型",
                                "cannot print a value of type `%s`", typeStr(c, at));
                    }
                }
                return ttVoid(tt);
            }

            FuncDef *f = findFunc(c, name);
            if (!f) {
                ckError(c, e->line, "可用内建： print(x) / println(x)",
                        "call to undefined function `%s`", name);
                return ttError(tt);
            }
            e->func = f;

            if (e->u.call.args.len != f->params.len) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        name, f->params.len, e->u.call.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p  = *(Param **)vecAt(&f->params, i);
                Expr  *a  = *(Expr **)vecAt(&e->u.call.args, i);
                adoptContextType(a, p->type);
                Type *at = checkExpr(c, a);

                /* T3：形参是 `ref T` 时，值必须在调用点显式写 `ref` ——
                 * 「这里传的是引用不是拷贝」要让读代码的人一眼看见（P′）。
                 * 实参本身就是引用的话，直接传即可。*/
                if (p->type->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`. 表示在这个值上操作，而自由函数的引用参数要点明。"
                            "`ref` 是可变引用，所以被引用的东西必须是 `var`",
                            "argument expects `%s`; write `ref ...` here to pass a reference",
                            typeStr(c, p->type));
                    continue;
                }
                checkAssignable(c, p->type, at, a, "argument");
            }
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_METHOD: {
            /* 方法只住在 struct 体内 —— 按接收者的类型去找 */
            Type *recvT = checkExpr(c, e->u.method.recv);
            Type *rb = ttBase(recvT);

            FuncDef *f = findMethod(rb, e->u.method.name);
            if (!f) {
                StructDef *sd = structOf(rb);
                Buf note;
                bufInit(&note, c->arena);
                if (sd) {
                    bufPrintf(&note, "%s 的方法：", sd->name);
                    if (sd->methods.len == 0) bufPuts(&note, " （一个都没有）");
                    for (size_t i = 0; i < sd->methods.len; i++)
                        bufPrintf(&note, " %s",
                                  (*(FuncDef **)vecAt(&sd->methods, i))->name);
                    bufPuts(&note, "；方法必须写在 struct 体内（定案 9）");
                } else {
                    bufPuts(&note, "只有 struct 的值有方法");
                }
                ckError(c, e->line, bufCstr(&note), "no method `%s` on `%s`",
                        e->u.method.name, typeStr(c, rb ? rb : recvT));
                return ttError(tt);
            }
            e->func = f;

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
                Type *at = checkExpr(c, a);
                if (pt->kind == TY_REF && at->kind != TY_REF && !ttIsError(at)) {
                    ckError(c, a->line,
                            "`. 表示在这个值上操作，而自由函数的引用参数要点明。"
                            "`ref` 是可变引用，所以被引用的东西必须是 `var`",
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
                        "裸 `{}` 只在上下文能唯一确定类型的地方可用："
                        "`var x: T = {}` / `return {}` / `f({})`",
                        "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            if (st->kind == TY_STRUCT && sd->typeParams.len > 0) {
                ckError(c, e->line, "泛型要写出实参，例如 `Pair<i32, i32> { ... }`",
                        "`%s` is generic; type arguments cannot be inferred here", sd->name);
                return ttError(tt);
            }

            for (size_t i = 0; i < e->u.lit.inits.len; i++) {
                FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
                FieldDef *fd = findField(sd, fi->name);
                if (!fd) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPrintf(&note, "%s 的字段：", sd->name);
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
                Type *vt = checkExpr(c, fi->value);
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "field `%s`", fi->name);
                checkAssignable(c, want, vt, fi->value, bufCstr(&what));
            }
            return st;
        }
    }
    return ttError(tt);
}

static Type *checkExpr(Checker *c, Expr *e) {
    if (!e) return ttError(c->tt);
    Type *t = checkExprInner(c, e);
    if (!t) t = ttError(c->tt);
    e->type = t;
    return t;
}

/* ---------------------------------------------------------------- 语句 */

static void checkStmt(Checker *c, Stmt *s);

static void checkBlockBody(Checker *c, Stmt *block) {
    pushScope(c);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i));
    popScope(c);
}

static void checkStmt(Checker *c, Stmt *s) {
    switch (s->kind) {
        case ST_VAR: {
            /* 局部变量声明的类型标注也要解析（parser 只造「类型名」） */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* 没有初始化式 ⇒ 零初始化（定案 8）。parser 保证此时必有类型标注。 */
            if (!s->u.var.init) {
                if (s->u.var.ann && s->u.var.ann->kind == TY_REF) {
                    ckError(c, s->line, "`ref T` 是不可为空的引用，所以它没有「零值」",
                            "cannot zero-initialize `%s`: a reference has no zero value",
                            s->u.var.name);
                }
                s->type = s->u.var.ann ? s->u.var.ann : ttError(c->tt);
                declare(c, s->u.var.name, s->type, s->u.var.mut, s->line);
                return;
            }

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            Type *it = checkExpr(c, s->u.var.init);
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            declare(c, s->u.var.name, s->type, s->u.var.mut, s->line);
            return;
        }

        case ST_ASSIGN: {
            Type *tt_ = checkExpr(c, s->u.assign.target);
            adoptContextType(s->u.assign.value, tt_);
            Type *vt = checkExpr(c, s->u.assign.value);

            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *sym = lookup(c, s->u.assign.target->u.ident.name);
                if (sym && !sym->mut) {
                    ckError(c, s->line, "改成 `var` 才能重新赋值（`let` 是不可变绑定）",
                            "cannot assign to `%s`, which is a `let`", sym->name);
                    return;
                }
            }
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            return;
        }

        case ST_IF:
            expectBool(c, checkExpr(c, s->u.ifs.cond), s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            if (s->u.ifs.elseBody) {
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
            }
            return;

        case ST_WHILE:
            expectBool(c, checkExpr(c, s->u.whiles.cond), s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            return;

        case ST_RETURN: {
            Type *want = c->curFunc ? c->curFunc->ret : NULL;
            if (!s->u.ret.value) {
                if (want && !ttIs(want, "void")) {
                    ckError(c, s->line, NULL, "`%s` must return a value of type `%s`",
                            c->curFunc->name, typeStr(c, want));
                }
                return;
            }
            if (!want) {
                checkExpr(c, s->u.ret.value);
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            adoptContextType(s->u.ret.value, want);
            Type *vt = checkExpr(c, s->u.ret.value);
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_EXPR:
            checkExpr(c, s->u.expr.expr);
            return;

        case ST_BLOCK:
            checkBlockBody(c, s);
            return;
    }
}

/* ---------------------------------------------------------------- 顶层 */

static void resolveSignature(Checker *c, FuncDef *f) {
    /* 方法里，它所属 struct 的泛型参数是可见的 */
    Vec *params = f->owner ? &f->owner->typeParams : NULL;

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line, params);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line, params);
    if (f->ret && ttIs(f->ret, "void")) f->ret = NULL;
}

static void checkDeclarations(Checker *c) {
    Module *m = c->m;

    /* 重名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *a = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = i + 1; j < m->structs.len; j++) {
            StructDef *b = *(StructDef **)vecAt(&m->structs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate struct `%s`", b->name);
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *a = *(FuncDef **)vecAt(&m->funcs, i);
        for (size_t j = i + 1; j < m->funcs.len; j++) {
            FuncDef *b = *(FuncDef **)vecAt(&m->funcs, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate function `%s`", b->name);
        }
    }

    /* 字段重名 + 方法重名 + 方法撞字段名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fa = *(FieldDef **)vecAt(&sd->fields, j);
            for (size_t k = j + 1; k < sd->fields.len; k++) {
                FieldDef *fb = *(FieldDef **)vecAt(&sd->fields, k);
                if (strcmp(fa->name, fb->name) == 0)
                    ckError(c, fb->line, NULL, "struct `%s` has duplicate field `%s`",
                            sd->name, fb->name);
            }
        }
        for (size_t j = 0; j < sd->methods.len; j++) {
            FuncDef *ma = *(FuncDef **)vecAt(&sd->methods, j);
            if (findField(sd, ma->name))
                ckError(c, ma->line, NULL, "`%s.%s`: a field and a method cannot share a name",
                        sd->name, ma->name);
            for (size_t k = j + 1; k < sd->methods.len; k++) {
                FuncDef *mb = *(FuncDef **)vecAt(&sd->methods, k);
                if (strcmp(ma->name, mb->name) == 0)
                    ckError(c, mb->line, NULL, "struct `%s` has duplicate method `%s`",
                            sd->name, mb->name);
            }
        }
    }

    /* type 重名 / 变体重名 / 空枚举 */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *a = *(TypeDef **)vecAt(&m->types, i);
        for (size_t j = i + 1; j < m->types.len; j++) {
            TypeDef *b = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(a->name, b->name) == 0)
                ckError(c, b->line, NULL, "duplicate type `%s`", b->name);
        }
        if (a->variants.len == 0)
            ckError(c, a->line, NULL, "type `%s` has no variants", a->name);
        for (size_t j = 0; j < a->variants.len; j++) {
            Variant *va = *(Variant **)vecAt(&a->variants, j);
            for (size_t k = j + 1; k < a->variants.len; k++) {
                Variant *vb = *(Variant **)vecAt(&a->variants, k);
                if (strcmp(va->name, vb->name) == 0)
                    ckError(c, vb->line, NULL, "type `%s` has duplicate variant `%s`",
                            a->name, vb->name);
            }
        }
    }

    /* struct 和 type 之间也不能重名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < m->types.len; j++) {
            TypeDef *td = *(TypeDef **)vecAt(&m->types, j);
            if (strcmp(sd->name, td->name) == 0)
                ckError(c, td->line, NULL, "`%s` is already a struct", td->name);
        }
    }
}

static void checkMethodShape(Checker *c, FuncDef *f) {
    if (!f->owner) {
        /* 自由函数不能有 `self` —— 方法必须写在 struct 体内（定案 9）*/
        if (strcmp(f->name, "==") == 0 || strcmp(f->name, "!=") == 0)
            ckError(c, f->line, "运算符要写在 struct 体内（它是一个方法）",
                    "operator `%s` must be defined inside a `struct`", f->name);
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (strcmp(p->name, "self") == 0)
                ckError(c, p->line, "方法要写在 struct 体内（定案 9）",
                        "`self` is only allowed in a method declared inside a `struct`");
        }
        return;
    }

    Param *p0 = f->params.len ? *(Param **)vecAt(&f->params, 0) : NULL;
    if (!p0 || strcmp(p0->name, "self") != 0) {
        ckError(c, f->line, NULL,
                "method `%s.%s` must take `self: ref %s` as its first parameter",
                f->owner->name, f->name, f->owner->name);
    } else {
        /* 比 sdef 而不是比类型指针 —— 泛型 struct 的 self 是 `ref Pair<A, B>` */
        Type *sb = ttBase(p0->type);
        if (p0->type->kind != TY_REF || !sb || sb->sdef != f->owner)
            ckError(c, p0->line, NULL, "`self` of `%s.%s` must be `ref %s`",
                    f->owner->name, f->name, f->owner->name);
    }
    for (size_t i = 1; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0)
            ckError(c, p->line, NULL, "`self` must be the first parameter");
    }
}

static void checkFunc(Checker *c, FuncDef *f) {
    FuncDef *savedFunc = c->curFunc;
    Vec     *savedParams = c->curParams;

    c->curFunc = f;
    c->curParams = f->owner ? &f->owner->typeParams : NULL;
    pushScope(c);

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        /* 参数是**可变的** —— 它是调用者给的局部副本（跟 C 一致），
         * 所以 `fn f(real: Board)` 里可以 `ref real` */
        declare(c, p->name, p->type, true, p->line);
    }
    /* 函数体不另开作用域 —— 参数和函数体的局部变量同一层，
     * 这样「局部变量遮蔽参数」会直接报错 */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

    popScope(c);
    c->curFunc = savedFunc;
    c->curParams = savedParams;
}

bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    Checker c;
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    vecInit(&c.scopes, arena, sizeof(void *));
    vecInit(&c.eqChecks, arena, sizeof(void *));

    c.tI32  = ttFromName(tt, "i32");
    c.tF64  = ttFromName(tt, "f64");
    c.tBool = ttFromName(tt, "bool");
    c.tStr  = ttFromName(tt, "str");

    /* 先把所有 struct / type 的类型驻留出来（方法签名比较要用）*/
    for (size_t i = 0; i < m->structs.len; i++)
        ttFromName(tt, (*(StructDef **)vecAt(&m->structs, i))->name);
    for (size_t i = 0; i < m->types.len; i++)
        ttFromName(tt, (*(TypeDef **)vecAt(&m->types, i))->name);

    /* 第一遍：解析所有签名与字段里的类型名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
            fd->type = ttResolve(tt, ctx, fd->type, fd->line, &sd->typeParams);
        }
        for (size_t j = 0; j < sd->methods.len; j++)
            resolveSignature(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        resolveSignature(&c, *(FuncDef **)vecAt(&m->funcs, i));

    checkDeclarations(&c);

    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkMethodShape(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        checkMethodShape(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* 第二遍：检查函数体 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->methods.len; j++)
            checkFunc(&c, *(FuncDef **)vecAt(&sd->methods, j));
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        checkFunc(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* 推迟的 `==` 检查：对每个具体实例复查一遍。
     * 这是「不引入 trait」的代价 —— 错误晚到这里，但信息要说清是哪个实例。 */
    for (size_t i = 0; i < c.eqChecks.len; i++) {
        EqCheck *ec = *(EqCheck **)vecAt(&c.eqChecks, i);
        for (size_t j = 0; j < tt->instances.len; j++) {
            Type *inst = *(Type **)vecAt(&tt->instances, j);
            if (inst->sdef != ec->owner) continue;

            Type *lt = ttSubstitute(tt, ec->node->u.bin.left->type,
                                    &ec->owner->typeParams, &inst->targs);
            if (!ttEquals(lt, ttSubstitute(tt, ec->node->u.bin.right->type,
                                          &ec->owner->typeParams, &inst->targs)))
                continue;

            if (!typeSupportsEq(&c, lt)) {
                ckError(&c, ec->node->line,
                        "泛型里的 `==` 推迟到实例化才检查 —— 这是「不引入 trait」换来的代价。"
                        "给那个类型加一个 `eq` 方法就行。",
                        "`%s` needs `%s` to define `==`",
                        inst->name, typeStr(&c, lt));
            }
        }
    }

    return !ctx->hasError;
}
