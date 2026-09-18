/* extC -> C 代码生成。
 *
 * week-0 不做聪明的优化：生成的 C 是「无聊的 C」。
 * 「轻量」是靠 `#line` 把报错映射回 .extc 的行号实现的，不是靠不解析。
 */

#include "codegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 类型映射 */

typedef struct { const char *extc; const char *c; } TypeMap;

static const TypeMap C_TYPES[] = {
    { "i8",  "int8_t"  }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" },
    { "u8",  "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" },
    { "f32", "float"   }, { "f64", "double"  },
    { "bool", "bool"   }, { "str", "const char *" },
    { NULL, NULL }
};

typedef struct { const char *extc; const char *fmt; const char *cast; } PrintFmt;

/* 「无格式串」的实现方式：格式由编译器按**静态类型**选，用户永远不写 "%d"。 */
static const PrintFmt PRINT_FMT[] = {
    { "i8",  "%d",   "(int)"       }, { "i16", "%d",   "(int)"       },
    { "i32", "%d",   "(int)"       }, { "i64", "%lld", "(long long)" },
    { "u8",  "%u",   "(unsigned)"  }, { "u16", "%u",   "(unsigned)"  },
    { "u32", "%u",   "(unsigned)"  }, { "u64", "%llu", "(unsigned long long)" },
    { "f32", "%g",   "(double)"    }, { "f64", "%g",   "(double)"    },
    { NULL, NULL, NULL }
};

static bool isIntType(const char *n) {
    static const char *T[] = { "i8","i16","i32","i64","u8","u16","u32","u64", NULL };
    for (size_t i = 0; T[i]; i++) if (strcmp(T[i], n) == 0) return true;
    return false;
}

static bool isFloatType(const char *n) {
    return strcmp(n, "f32") == 0 || strcmp(n, "f64") == 0;
}

/* ---------------------------------------------------------------- 状态 */

typedef struct {
    const char *name;
    Type       *type;
    bool        mut;
} Sym;

typedef struct {
    Arena      *arena;
    Ctx        *ctx;
    Buf        *out;
    const char *path;
    bool        lineMap;

    Vec structs;        /* StructDef* */
    Vec funcs;          /* FuncDef*  */
    Vec scopes;         /* Vec* —— 每层作用域 */

    int  indent;
    int  curLine;
    Type *curRet;       /* 当前函数的返回类型：给裸 `{}` 定类型用 */
    Type *tI32;
    Type *tF64;
    Type *tBool;
} CG;

static void cgError(CG *g, const char *note, const char *fmt, ...) {
    char tmp[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ctxError(g->ctx, g->curLine, 1, note, "%s", tmp);
}

static void cgLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    for (int i = 0; i < g->indent; i++) bufPuts(g->out, "    ");
    bufPuts(g->out, tmp);
    bufPutc(g->out, '\n');
}

/* ---------------------------------------------------------------- 符号表 */

static void cgPushScope(CG *g) {
    Vec *s = (Vec *)arenaAlloc(g->arena, sizeof(Vec));
    vecInit(s, g->arena, sizeof(void *));
    *(Vec **)vecPush(&g->scopes) = s;
}

static void cgPopScope(CG *g) {
    if (g->scopes.len) g->scopes.len--;
}

static void cgDeclare(CG *g, const char *name, Type *t, bool mut) {
    Vec *top = *(Vec **)vecAt(&g->scopes, g->scopes.len - 1);
    for (size_t i = 0; i < top->len; i++) {
        Sym *old = *(Sym **)vecAt(top, i);
        if (strcmp(old->name, name) == 0) {
            cgError(g, NULL, "`%s` is already declared in this scope", name);
            return;
        }
    }
    Sym *sym = (Sym *)arenaAllocZero(g->arena, sizeof(Sym));
    sym->name = name;
    sym->type = t;
    sym->mut = mut;
    *(Sym **)vecPush(top) = sym;
}

static Sym *cgLookup(CG *g, const char *name) {
    for (size_t i = g->scopes.len; i-- > 0; ) {
        Vec *s = *(Vec **)vecAt(&g->scopes, i);
        for (size_t j = 0; j < s->len; j++) {
            Sym *sym = *(Sym **)vecAt(s, j);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

static StructDef *findStruct(CG *g, const char *name) {
    for (size_t i = 0; i < g->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&g->structs, i);
        if (strcmp(sd->name, name) == 0) return sd;
    }
    return NULL;
}

static FuncDef *findFunc(CG *g, const char *name) {
    for (size_t i = 0; i < g->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g->funcs, i);
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

/* ---------------------------------------------------------------- C 类型 */

static const char *cType(CG *g, Type *t) {
    if (!t) return "void";
    if (t->isRef) return arenaPrintf(g->arena, "%s *", cType(g, t->inner));
    if (strcmp(t->name, "void") == 0) return "void";
    for (size_t i = 0; C_TYPES[i].extc; i++)
        if (strcmp(C_TYPES[i].extc, t->name) == 0) return C_TYPES[i].c;
    if (findStruct(g, t->name)) return t->name;
    cgError(g, NULL, "unknown type `%s`", t->name);
    return "void";
}

/* ---------------------------------------------------------------- 表达式类型 */

static Type *typeOf(CG *g, Expr *e);
static const char *genExpr(CG *g, Expr *e);

/* 裸 `{}` 的类型从**上下文**推导。
 * 规则是统一的，不是特例：只在"上下文能唯一确定类型"的地方允许 ——
 *   `var x: T = {}`、`return {}`（函数返回 T）、`f({})`（形参类型 T）。
 * 推导不出来就报错，绝不猜。 */
static void fillLitName(Expr *e, Type *want) {
    if (!want || !e) return;
    if (e->kind == EX_STRUCTLIT && !e->u.lit.name) {
        Type *bt = typeBase(want);
        if (bt && !bt->isRef) e->u.lit.name = bt->name;
    }
}

static Type *typeOf(CG *g, Expr *e) {
    g->curLine = e->line;

    switch (e->kind) {
        case EX_INT:   return g->tI32;
        case EX_FLOAT: return g->tF64;
        case EX_BOOL:  return g->tBool;
        case EX_STR:   return typeName(g->arena, "str");

        case EX_IDENT: {
            Sym *s = cgLookup(g, e->u.ident.name);
            if (!s) {
                cgError(g, "week-0 没有全局变量；所有名字都要先声明（或用参数传进来）",
                        "undefined name `%s`", e->u.ident.name);
                return g->tI32;
            }
            return s->type;
        }

        case EX_BIN: {
            const char *op = e->u.bin.op;
            if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0 ||
                strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
                return g->tBool;
            }
            Type *lt = typeBase(typeOf(g, e->u.bin.left));
            Type *rt = typeBase(typeOf(g, e->u.bin.right));
            if (isFloatType(lt->name) || isFloatType(rt->name)) return g->tF64;
            /* week-0 的简化：整数运算取左边；真正的类型检查是下一步 */
            return isIntType(lt->name) ? lt : g->tI32;
        }

        case EX_UN:
            if (strcmp(e->u.un.op, "!") == 0) return g->tBool;
            return typeOf(g, e->u.un.operand);

        case EX_FIELD: {
            Type *bt = typeBase(typeOf(g, e->u.field.obj));
            StructDef *sd = findStruct(g, bt->name);
            if (!sd) {
                cgError(g, NULL, "`%s` is not a struct, so it has no field `%s`",
                        bt->name, e->u.field.name);
                return g->tI32;
            }
            FieldDef *fd = findField(sd, e->u.field.name);
            if (!fd) {
                cgError(g, NULL, "struct `%s` has no field `%s`", sd->name, e->u.field.name);
                return g->tI32;
            }
            return fd->type;
        }

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) {
                cgError(g, NULL, "only direct function calls are supported in week-0");
                return g->tI32;
            }
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0)
                return typeName(g->arena, "void");
            FuncDef *f = findFunc(g, name);
            if (!f) {
                cgError(g, "可用内建： print(x) / println(x)",
                        "call to undefined function `%s`", name);
                return g->tI32;
            }
            return f->ret ? f->ret : typeName(g->arena, "void");
        }

        case EX_METHOD: {
            FuncDef *f = findFunc(g, e->u.method.name);
            if (!f) {
                cgError(g, NULL, "call to undefined method `%s`", e->u.method.name);
                return g->tI32;
            }
            return f->ret ? f->ret : typeName(g->arena, "void");
        }

        case EX_STRUCTLIT: {
            if (!e->u.lit.name) {
                cgError(g, "只有 `var x: T = {}` 这种写法才能靠声明类型补全",
                        "cannot infer the type of a bare `{}` here");
                return g->tI32;
            }
            return typeName(g->arena, e->u.lit.name);
        }
    }
    return g->tI32;
}

/* ---------------------------------------------------------------- 表达式生成 */

static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = typeBase(typeOf(g, a));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");

        if (strcmp(bt->name, "bool") == 0) {
            bufPrintf(&b, "printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
            continue;
        }
        if (strcmp(bt->name, "str") == 0) {
            bufPrintf(&b, "printf(\"%%s\", %s)", code);
            continue;
        }
        const PrintFmt *pf = NULL;
        for (size_t k = 0; PRINT_FMT[k].extc; k++)
            if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
        if (!pf) {
            cgError(g, "week-0 的 println 只支持整数 / 浮点 / bool / 字符串",
                    "cannot print a value of type `%s`", bt->name);
            bufPuts(&b, "0");
            continue;
        }
        bufPrintf(&b, "printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
    }

    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "printf(\"\\n\")");
    } else if (args->len == 0) {
        bufPuts(&b, "printf(\"\")");
    }

    bufPutc(&b, ')');
    return bufCstr(&b);
}

static void checkArity(CG *g, FuncDef *f, size_t got, Expr *node, size_t offset) {
    size_t want = f->params.len - offset;
    if (got != want) {
        g->curLine = node->line;
        cgError(g, NULL, "`%s` expects %zu argument(s), got %zu", f->name, want, got);
    }
}

static const char *genMethodCall(CG *g, Expr *e) {
    FuncDef *f = findFunc(g, e->u.method.name);
    if (!f) {
        cgError(g, NULL, "call to undefined method `%s`", e->u.method.name);
        return "0";
    }
    if (!funcIsMethod(f)) {
        cgError(g, "方法 = 首参数名为 `self` 的函数，例如 `fn f(self: ref T, ...)`",
                "`%s` is not a method", e->u.method.name);
        return "0";
    }
    checkArity(g, f, e->u.method.args.len, e, 1);

    for (size_t i = 0; i < e->u.method.args.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i + 1);
        fillLitName(*(Expr **)vecAt(&e->u.method.args, i), p->type);
    }

    Type *recvT = typeOf(g, e->u.method.recv);
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* 接收者自动取地址 / 解引用 —— 这就是 `a.f(x)` == `f(a, x)` 的全部机制 */
    if (p0->type->isRef && !recvT->isRef)      recvC = arenaPrintf(g->arena, "&(%s)", recvC);
    else if (!p0->type->isRef && recvT->isRef) recvC = arenaPrintf(g->arena, "*(%s)", recvC);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", f->name, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

static const char *genStructLit(CG *g, Expr *e) {
    if (!e->u.lit.name) {
        cgError(g, "只有 `var x: T = {}` 这种写法才能靠声明类型补全",
                "cannot infer the type of a bare `{}` here");
        return "0";
    }
    StructDef *sd = findStruct(g, e->u.lit.name);
    if (!sd) {
        cgError(g, NULL, "unknown struct `%s`", e->u.lit.name);
        return "0";
    }
    if (e->u.lit.inits.len == 0) return arenaPrintf(g->arena, "(%s){0}", sd->name);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){ ", sd->name);
    for (size_t i = 0; i < e->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
        if (!findField(sd, fi->name)) {
            cgError(g, NULL, "struct `%s` has no field `%s`", sd->name, fi->name);
            continue;
        }
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, ".%s = %s", fi->name, genExpr(g, fi->value));
    }
    bufPuts(&b, " }");
    return bufCstr(&b);
}

static const char *genExpr(CG *g, Expr *e) {
    g->curLine = e->line;

    switch (e->kind) {
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT: return arenaPrintf(g->arena, "%g", e->u.fval);
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:   return arenaPrintf(g->arena, "\"%s\"", e->u.str.text);

        case EX_IDENT:
            if (!cgLookup(g, e->u.ident.name)) {
                cgError(g, "week-0 没有全局变量；所有名字都要先声明（或用参数传进来）",
                        "undefined name `%s`", e->u.ident.name);
                return "0";
            }
            return e->u.ident.name;

        case EX_BIN:
            return arenaPrintf(g->arena, "(%s %s %s)",
                               genExpr(g, e->u.bin.left), e->u.bin.op,
                               genExpr(g, e->u.bin.right));

        case EX_UN:
            return arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            const char *arrow = typeOf(g, e->u.field.obj)->isRef ? "->" : ".";
            typeOf(g, e);            /* 顺便校验字段存在 */
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) {
                cgError(g, NULL, "only direct function calls are supported in week-0");
                return "0";
            }
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
            if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);

            FuncDef *f = findFunc(g, name);
            if (!f) {
                cgError(g, "可用内建： print(x) / println(x)",
                        "call to undefined function `%s`", name);
                return "0";
            }
            checkArity(g, f, e->u.call.args.len, e, 0);

            for (size_t i = 0; i < e->u.call.args.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                fillLitName(*(Expr **)vecAt(&e->u.call.args, i), p->type);
            }

            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, name);
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
            }
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);
    }
    return "0";
}

/* ---------------------------------------------------------------- 语句生成 */

static void genStmt(CG *g, Stmt *s);

static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

static void genBlockBody(CG *g, Stmt *block) {
    cgPushScope(g);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
    cgPopScope(g);
}

static void genVarDecl(CG *g, Stmt *s) {
    /* `var x: T = {}` —— 用声明类型补全裸 `{}` */
    if (s->u.var.init->kind == EX_STRUCTLIT && !s->u.var.init->u.lit.name) {
        if (!s->u.var.ann) {
            cgError(g, "初始化式是裸 `{}`，所以必须写类型：`var x: T = {}`",
                    "cannot infer the type of `%s`", s->u.var.name);
        } else {
            s->u.var.init->u.lit.name = typeBase(s->u.var.ann)->name;
        }
    }

    Type *declT = s->u.var.ann ? s->u.var.ann : typeOf(g, s->u.var.init);
    const char *init = genExpr(g, s->u.var.init);

    cgLine(g, "%s %s = %s;", cType(g, declT), s->u.var.name, init);
    cgDeclare(g, s->u.var.name, declT, s->u.var.mut);
}

static void genAssign(CG *g, Stmt *s) {
    if (s->u.assign.target->kind == EX_IDENT) {
        Sym *sym = cgLookup(g, s->u.assign.target->u.ident.name);
        if (sym && !sym->mut) {
            cgError(g, "改成 `var` 才能重新赋值（`let` 是不可变绑定）",
                    "cannot assign to `%s`, which is a `let`", sym->name);
        }
    }
    cgLine(g, "%s = %s;", genExpr(g, s->u.assign.target), genExpr(g, s->u.assign.value));
}

static void genStmt(CG *g, Stmt *s) {
    g->curLine = s->line;
    lineMark(g, s);

    switch (s->kind) {
        case ST_VAR:
            genVarDecl(g, s);
            return;

        case ST_ASSIGN:
            genAssign(g, s);
            return;

        case ST_IF:
            cgLine(g, "if (%s) {", genExpr(g, s->u.ifs.cond));
            g->indent++;
            genBlockBody(g, s->u.ifs.thenBody);
            g->indent--;
            if (!s->u.ifs.elseBody) {
                cgLine(g, "}");
                return;
            }
            cgLine(g, "} else {");
            g->indent++;
            if (s->u.ifs.elseBody->kind == ST_BLOCK) genBlockBody(g, s->u.ifs.elseBody);
            else                                     genStmt(g, s->u.ifs.elseBody);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_WHILE:
            cgLine(g, "while (%s) {", genExpr(g, s->u.whiles.cond));
            g->indent++;
            genBlockBody(g, s->u.whiles.body);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_RETURN:
            if (!s->u.ret.value) {
                cgLine(g, "return;");
            } else {
                fillLitName(s->u.ret.value, g->curRet);
                cgLine(g, "return %s;", genExpr(g, s->u.ret.value));
            }
            return;

        case ST_BREAK:    cgLine(g, "break;");    return;
        case ST_CONTINUE: cgLine(g, "continue;"); return;

        case ST_EXPR:
            cgLine(g, "%s;", genExpr(g, s->u.expr.expr));
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;
    }
}

/* ---------------------------------------------------------------- 顶层 */

static void genFunc(CG *g, FuncDef *f) {
    if (strcmp(f->name, "main") == 0) {
        if (f->params.len > 0) {
            g->curLine = f->line;
            cgError(g, NULL, "`main` cannot take parameters in week-0");
        }
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "%s %s(", cType(g, f->ret), f->name);
        for (size_t i = 0; i < f->params.len; i++) {
            Param *p = *(Param **)vecAt(&f->params, i);
            if (i) bufPuts(&sig, ", ");
            bufPrintf(&sig, "%s %s", cType(g, p->type), p->name);
        }
        if (f->params.len == 0) bufPuts(&sig, "void");
        bufPuts(&sig, ") {");
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    cgPushScope(g);
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        cgDeclare(g, p->name, p->type, false);
    }
    g->curRet = f->ret;
    for (size_t i = 0; i < f->body->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&f->body->u.block.stmts, i));
    g->curRet = NULL;
    cgPopScope(g);
    g->indent--;
    cgLine(g, "}");
}

bool generateC(Ctx *ctx, Arena *arena, Module *m, bool lineMap, Buf *out) {
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.curLine = 0;
    g.tI32  = typeName(arena, "i32");
    g.tF64  = typeName(arena, "f64");
    g.tBool = typeName(arena, "bool");

    vecInit(&g.structs, arena, sizeof(void *));
    vecInit(&g.funcs, arena, sizeof(void *));
    vecInit(&g.scopes, arena, sizeof(void *));

    for (size_t i = 0; i < m->structs.len; i++) *(StructDef **)vecPush(&g.structs) = *(StructDef **)vecAt(&m->structs, i);
    for (size_t i = 0; i < m->funcs.len; i++)   *(FuncDef **)vecPush(&g.funcs)     = *(FuncDef **)vecAt(&m->funcs, i);

    bufPuts(out,
        "/* 由 extC 编译器自动生成 —— 请勿手工编辑。\n"
        " * 生成的 C 是「无聊的 C」：没有宏魔法，没有优化花招。\n"
        " * `#line` 指令把 gcc 的报错映射回 .extc 的行号。\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stdio.h>\n\n");

    /* 结构体前置声明 + 定义 */
    if (g.structs.len) {
        for (size_t i = 0; i < g.structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&g.structs, i);
            cgLine(&g, "typedef struct %s %s;", sd->name, sd->name);
        }
        cgLine(&g, "");
        for (size_t i = 0; i < g.structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&g.structs, i);
            cgLine(&g, "struct %s {", sd->name);
            g.indent++;
            for (size_t j = 0; j < sd->fields.len; j++) {
                FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, j);
                cgLine(&g, "%s %s;", cType(&g, fd->type), fd->name);
            }
            g.indent--;
            cgLine(&g, "};");
            cgLine(&g, "");
        }
    }

    /* 原型：顺序无关，顺带支持互相调用 */
    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        const char *ret = strcmp(f->name, "main") == 0 ? "int" : cType(&g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        bufPrintf(&sig, "%s %s(", ret, f->name);
        if (f->params.len == 0) {
            bufPuts(&sig, "void");
        } else {
            for (size_t j = 0; j < f->params.len; j++) {
                Param *p = *(Param **)vecAt(&f->params, j);
                if (j) bufPuts(&sig, ", ");
                bufPuts(&sig, cType(&g, p->type));
            }
        }
        bufPuts(&sig, ");");
        cgLine(&g, "%s", bufCstr(&sig));
    }
    if (g.funcs.len) cgLine(&g, "");

    for (size_t i = 0; i < g.funcs.len; i++) {
        genFunc(&g, *(FuncDef **)vecAt(&g.funcs, i));
        cgLine(&g, "");
    }

    return !ctx->hasError;
}
