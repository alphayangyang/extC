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
    if (b->kind != TY_BUILTIN) return false;
    return true;    /* 内建类型全部可打印（void 会在这里被挡掉，见下） */
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
    if (!w || w->kind != TY_STRUCT) return;
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) e->u.lit.name = w->name;
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
                if (ttEquals(lt, rt) || ttCanWiden(lt, rt) || ttCanWiden(rt, lt)) return c->tBool;
                if (isNumericLit(e->u.bin.left)  && literalFits(e->u.bin.left, rt))  return c->tBool;
                if (isNumericLit(e->u.bin.right) && literalFits(e->u.bin.right, lt)) return c->tBool;
                ckError(c, e->line, NULL, "cannot compare `%s` with `%s`",
                        typeStr(c, lt), typeStr(c, rt));
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
            Type *bt = ttBase(checkExpr(c, e->u.field.obj));
            if (!bt || ttIsError(bt)) return ttError(tt);
            if (bt->kind != TY_STRUCT) {
                ckError(c, e->line, NULL, "`%s` is not a struct, so it has no field `%s`",
                        typeStr(c, bt), e->u.field.name);
                return ttError(tt);
            }
            FieldDef *fd = findField(bt->sdef, e->u.field.name);
            if (!fd) {
                Buf note;
                bufInit(&note, c->arena);
                bufPrintf(&note, "%s 的字段：", bt->name);
                for (size_t i = 0; i < bt->sdef->fields.len; i++)
                    bufPrintf(&note, " %s", (*(FieldDef **)vecAt(&bt->sdef->fields, i))->name);
                ckError(c, e->line, bufCstr(&note),
                        "struct `%s` has no field `%s`", bt->name, e->u.field.name);
                return ttError(tt);
            }
            e->field = fd;
            return fd->type;
        }

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
                checkAssignable(c, p->type, at, a, "argument");
            }
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_METHOD: {
            FuncDef *f = findFunc(c, e->u.method.name);
            if (!f) {
                ckError(c, e->line, NULL, "call to undefined method `%s`", e->u.method.name);
                return ttError(tt);
            }
            if (!funcIsMethod(f)) {
                ckError(c, e->line,
                        "方法 = 首参数名为 `self` 的函数，例如 `fn f(self: ref T, ...)`",
                        "`%s` is not a method", e->u.method.name);
                return ttError(tt);
            }
            e->func = f;

            Type *recvT = checkExpr(c, e->u.method.recv);
            Param *p0 = *(Param **)vecAt(&f->params, 0);

            /* 接收者可以是值也可以是 ref —— 编译器按需取地址 / 解引用 */
            if (!ttEquals(ttBase(p0->type), ttBase(recvT)))
                ckError(c, e->line, NULL, "`%s` expects `%s`, found `%s`",
                        e->u.method.name, typeStr(c, p0->type), typeStr(c, recvT));

            size_t want = f->params.len - 1;
            if (e->u.method.args.len != want) {
                ckError(c, e->line, NULL, "`%s` expects %zu argument(s), got %zu",
                        e->u.method.name, want, e->u.method.args.len);
                return f->ret ? f->ret : ttVoid(tt);
            }
            for (size_t i = 0; i < want; i++) {
                Param *p = *(Param **)vecAt(&f->params, i + 1);
                Expr  *a = *(Expr **)vecAt(&e->u.method.args, i);
                adoptContextType(a, p->type);
                Type *at = checkExpr(c, a);
                checkAssignable(c, p->type, at, a, "argument");
            }
            return f->ret ? f->ret : ttVoid(tt);
        }

        case EX_STRUCTLIT: {
            if (!e->u.lit.name) {
                ckError(c, e->line,
                        "裸 `{}` 只在上下文能唯一确定类型的地方可用："
                        "`var x: T = {}` / `return {}` / `f({})`",
                        "cannot infer the type of a bare `{}` here");
                return ttError(tt);
            }
            Type *st = ttFromName(tt, e->u.lit.name);
            if (!st || st->kind == TY_ERROR) {
                ckError(c, e->line, NULL, "unknown struct `%s`", e->u.lit.name);
                return ttError(tt);
            }
            if (st->kind != TY_STRUCT) {
                ckError(c, e->line, NULL, "`%s` is not a struct", e->u.lit.name);
                return ttError(tt);
            }
            StructDef *sd = st->sdef;
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
                adoptContextType(fi->value, fd->type);
                Type *vt = checkExpr(c, fi->value);
                Buf what;
                bufInit(&what, c->arena);
                bufPrintf(&what, "field `%s`", fi->name);
                checkAssignable(c, fd->type, vt, fi->value, bufCstr(&what));
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
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line);
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;
            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            Type *it = checkExpr(c, s->u.var.init);
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            if (ttIsError(declT)) {
                s->type = ttError(c->tt);
            } else {
                s->type = declT;
            }
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
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        p->type = ttResolve(c->tt, c->ctx, p->type, p->line);
    }
    if (f->ret) f->ret = ttResolve(c->tt, c->ctx, f->ret, f->line);
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

    /* 字段重名 */
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
    }
}

static void checkMethodShape(Checker *c, FuncDef *f) {
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (strcmp(p->name, "self") == 0 && i != 0) {
            ckError(c, p->line, "方法 = 首参数名为 `self` 的函数",
                    "`self` must be the first parameter of `%s`", f->name);
        }
    }
    if (funcIsMethod(f)) {
        Param *p0 = *(Param **)vecAt(&f->params, 0);
        if (p0->type->kind != TY_REF)
            ckError(c, p0->line, "`self` 应该是 `ref T`，否则方法改不了调用者的数据",
                    "`self` of `%s` must be a reference", f->name);
    }
}

static void checkFunc(Checker *c, FuncDef *f) {
    c->curFunc = f;
    pushScope(c);

    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        declare(c, p->name, p->type, false, p->line);
    }
    /* 函数体不另开作用域 —— 参数和函数体的局部变量同一层，
     * 这样「局部变量遮蔽参数」会直接报错 */
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&f->body->u.block.stmts, i));

    popScope(c);
    c->curFunc = NULL;
}

bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m) {
    Checker c;
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.arena = arena;
    c.tt = tt;
    c.m = m;
    vecInit(&c.scopes, arena, sizeof(void *));

    c.tI32  = ttFromName(tt, "i32");
    c.tF64  = ttFromName(tt, "f64");
    c.tBool = ttFromName(tt, "bool");
    c.tStr  = ttFromName(tt, "str");

    /* 第一遍：解析所有签名与字段里的类型名 */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        for (size_t j = 0; j < sd->fields.len; j++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
            fd->type = ttResolve(tt, ctx, fd->type, fd->line);
        }
    }
    for (size_t i = 0; i < m->funcs.len; i++)
        resolveSignature(&c, *(FuncDef **)vecAt(&m->funcs, i));

    checkDeclarations(&c);
    for (size_t i = 0; i < m->funcs.len; i++)
        checkMethodShape(&c, *(FuncDef **)vecAt(&m->funcs, i));

    /* 第二遍：检查函数体 */
    for (size_t i = 0; i < m->funcs.len; i++)
        checkFunc(&c, *(FuncDef **)vecAt(&m->funcs, i));

    return !ctx->hasError;
}
