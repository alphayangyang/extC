/* extC -> C 代码生成。
 *
 * T1 之后，这里**不做任何类型推理** —— 类型检查 pass 已经把结果写回 AST
 * （Expr.type / Expr.func / Expr.field / Stmt.type），这里只负责翻译。
 *
 * 生成的 C 是「无聊的 C」：没有宏魔法，没有优化花招。
 * `#line` 把 gcc 的报错映射回 .extc 的行号。
 */

#include "codegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "types.h"

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

/* 「无格式串」的实现方式：格式由**编译器按静态类型选**，用户永远不写 "%d"。 */
static const PrintFmt PRINT_FMT[] = {
    { "i8",  "%d",   "(int)"       }, { "i16", "%d",   "(int)"       },
    { "i32", "%d",   "(int)"       }, { "i64", "%lld", "(long long)" },
    { "u8",  "%u",   "(unsigned)"  }, { "u16", "%u",   "(unsigned)"  },
    { "u32", "%u",   "(unsigned)"  }, { "u64", "%llu", "(unsigned long long)" },
    { "f32", "%g",   "(double)"    }, { "f64", "%g",   "(double)"    },
    { NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- 状态 */

typedef struct {
    Arena      *arena;
    Ctx        *ctx;
    Buf        *out;
    const char *path;
    bool        lineMap;

    Vec structs;        /* StructDef* */
    Vec funcs;          /* FuncDef*  */
    int indent;
} CG;

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

static const char *cType(CG *g, Type *t) {
    if (!t) return "void";

    switch (t->kind) {
        case TY_REF:   return arenaPrintf(g->arena, "%s *", cType(g, t->inner));
        case TY_VOID:  return "void";
        case TY_STRUCT: return t->name;
        case TY_ENUM:  return "int";
        case TY_ERROR: return "int";
        case TY_BUILTIN:
            for (size_t i = 0; C_TYPES[i].extc; i++)
                if (strcmp(C_TYPES[i].extc, t->name) == 0) return C_TYPES[i].c;
            return "int";
        case TY_UNRESOLVED:
            return "int";       /* check 跑完之后不该出现 */
    }
    return "int";
}

/* ---------------------------------------------------------------- 表达式 */

static const char *genExpr(CG *g, Expr *e);

static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(a->type);
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt || bt->kind != TY_BUILTIN) { bufPuts(&b, "0"); continue; }

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
        if (!pf) { bufPuts(&b, "0"); continue; }
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

static const char *genMethodCall(CG *g, Expr *e) {
    FuncDef *f = e->func;
    if (!f) return "0";

    Type *recvT = e->u.method.recv->type;
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* 接收者按需取地址 / 解引用 —— 这就是 `a.f(x)` == `f(a, x)` 的全部机制 */
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = recvT && recvT->kind == TY_REF;
    if (wantRef && !haveRef)      recvC = arenaPrintf(g->arena, "&(%s)", recvC);
    else if (!wantRef && haveRef) recvC = arenaPrintf(g->arena, "*(%s)", recvC);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", f->name, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
    bufPutc(&b, ')');
    return bufCstr(&b);
}

static const char *genStructLit(CG *g, Expr *e) {
    const char *sname = e->u.lit.name;
    if ((!sname) && e->type && e->type->kind == TY_STRUCT) sname = e->type->name;
    if (!sname) return "(int){0}";

    if (e->u.lit.inits.len == 0) return arenaPrintf(g->arena, "(%s){0}", sname);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){ ", sname);
    for (size_t i = 0; i < e->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&e->u.lit.inits, i);
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, ".%s = %s", fi->name, genExpr(g, fi->value));
    }
    bufPuts(&b, " }");
    return bufCstr(&b);
}

static const char *genExpr(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT: return arenaPrintf(g->arena, "%g", e->u.fval);
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:   return arenaPrintf(g->arena, "\"%s\"", e->u.str.text);
        case EX_IDENT: return e->u.ident.name;

        case EX_BIN:
            return arenaPrintf(g->arena, "(%s %s %s)",
                               genExpr(g, e->u.bin.left), e->u.bin.op,
                               genExpr(g, e->u.bin.right));

        case EX_UN:
            return arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) return "0";
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
            if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);
            if (!e->func) return "0";

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

/* ---------------------------------------------------------------- 语句 */

static void genStmt(CG *g, Stmt *s);

static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

static void genBlockBody(CG *g, Stmt *block) {
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
}

static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);

    switch (s->kind) {
        case ST_VAR:
            cgLine(g, "%s %s = %s;", cType(g, s->type), s->u.var.name,
                   genExpr(g, s->u.var.init));
            return;

        case ST_ASSIGN:
            cgLine(g, "%s = %s;", genExpr(g, s->u.assign.target),
                   genExpr(g, s->u.assign.value));
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
            if (!s->u.ret.value) cgLine(g, "return;");
            else                 cgLine(g, "return %s;", genExpr(g, s->u.ret.value));
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
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "%s %s(", cType(g, f->ret), f->name);
        if (f->params.len == 0) {
            bufPuts(&sig, "void");
        } else {
            for (size_t i = 0; i < f->params.len; i++) {
                Param *p = *(Param **)vecAt(&f->params, i);
                if (i) bufPuts(&sig, ", ");
                bufPrintf(&sig, "%s %s", cType(g, p->type), p->name);
            }
        }
        bufPuts(&sig, ") {");
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    genBlockBody(g, f->body);
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

    vecInit(&g.structs, arena, sizeof(void *));
    vecInit(&g.funcs, arena, sizeof(void *));
    for (size_t i = 0; i < m->structs.len; i++)
        *(StructDef **)vecPush(&g.structs) = *(StructDef **)vecAt(&m->structs, i);
    for (size_t i = 0; i < m->funcs.len; i++)
        *(FuncDef **)vecPush(&g.funcs) = *(FuncDef **)vecAt(&m->funcs, i);

    bufPuts(out,
        "/* 由 extC 编译器自动生成 —— 请勿手工编辑。\n"
        " * 生成的 C 是「无聊的 C」：没有宏魔法，没有优化花招。\n"
        " * `#line` 指令把 gcc 的报错映射回 .extc 的行号。\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stdio.h>\n\n");

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
