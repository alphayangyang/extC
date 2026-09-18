#include "parser.h"

#include <string.h>

#include "lexer.h"

typedef struct {
    Ctx   *ctx;
    Arena *arena;
    Vec   *toks;
    size_t pos;
} Parser;

/* ---------------------------------------------------------------- 前瞻 */

static Token *pk(Parser *p, size_t k) {
    size_t i = p->pos + k;
    if (i >= p->toks->len) i = p->toks->len - 1;    /* 停在 EOF */
    return (Token *)vecAt(p->toks, i);
}

static Token *cur(Parser *p) { return pk(p, 0); }

static bool at(Parser *p, const char *value) {
    return strcmp(cur(p)->text, value) == 0;
}

static bool atKind(Parser *p, TokenKind k) { return cur(p)->kind == k; }

static Token *take(Parser *p) {
    Token *t = cur(p);
    if (p->pos + 1 < p->toks->len) p->pos++;
    return t;
}

static bool accept(Parser *p, const char *value) {
    if (!at(p, value)) return false;
    take(p);
    return true;
}

static const char *shown(const Token *t) {
    return t->text[0] ? t->text : tokenKindName(t->kind);
}

static bool expect(Parser *p, const char *value, const char *note) {
    if (at(p, value)) { take(p); return true; }
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, note,
             "expected `%s`, found `%s`", value, shown(t));
    return false;
}

static Token *expectIdent(Parser *p, const char *what) {
    if (atKind(p, TK_IDENT)) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, NULL,
             "expected %s, found `%s`", what, shown(t));
    return NULL;
}

static void skipNl(Parser *p) {
    while (atKind(p, TK_NEWLINE)) take(p);
}

/* 换行和可选的 `;` 都是语句分隔符 */
static void skipJunk(Parser *p) {
    for (;;) {
        if (atKind(p, TK_NEWLINE) || at(p, ";")) { take(p); continue; }
        return;
    }
}

/* ---------------------------------------------------------------- 前向声明 */

static Type    *parseType(Parser *p);
static Stmt    *parseBlock(Parser *p);
static Stmt    *parseStmt(Parser *p);
static Stmt    *parseVarDecl(Parser *p);
static Stmt    *parseIf(Parser *p);
static Stmt    *parseWhile(Parser *p);
static StructDef *parseStruct(Parser *p);
static FuncDef   *parseFunc(Parser *p);

static Expr *parseExpr(Parser *p);
static Expr *parseOr(Parser *p);
static Expr *parseAnd(Parser *p);
static Expr *parseEquality(Parser *p);
static Expr *parseComparison(Parser *p);
static Expr *parseTerm(Parser *p);
static Expr *parseFactor(Parser *p);
static Expr *parseUnary(Parser *p);
static Expr *parsePostfix(Parser *p);
static Expr *parsePrimary(Parser *p);
static Expr *parseStructLit(Parser *p, const char *name);
static bool  parseArgs(Parser *p, Vec *out);

static Expr *mkBin(Parser *p, const char *op, Expr *l, Expr *r, int line) {
    Expr *e = exprNew(p->arena, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = l;
    e->u.bin.right = r;
    return e;
}

/* ================================================================ 顶层 */

bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out) {
    Parser p = { ctx, arena, toks, 0 };
    moduleInit(out, arena);
    skipJunk(&p);

    while (!atKind(&p, TK_EOF) && !ctx->hasError) {
        if (at(&p, "struct")) {
            StructDef *s = parseStruct(&p);
            if (!s) return false;
            *(StructDef **)vecPush(&out->structs) = s;
        } else if (at(&p, "fn")) {
            FuncDef *f = parseFunc(&p);
            if (!f) return false;
            *(FuncDef **)vecPush(&out->funcs) = f;
        } else {
            Token *t = cur(&p);
            ctxError(ctx, t->line, t->col,
                     "extC 的顶层只允许函数和结构体定义",
                     "expected `fn` or `struct` at the top level, found `%s`", shown(t));
            return false;
        }
        skipJunk(&p);
    }
    return !ctx->hasError;
}

static StructDef *parseStruct(Parser *p) {
    Token *kw = take(p);                    /* struct */
    Token *name = expectIdent(p, "a struct name");
    if (!name) return NULL;

    StructDef *sd = (StructDef *)arenaAllocZero(p->arena, sizeof(StructDef));
    sd->name = name->text;
    sd->line = kw->line;
    vecInit(&sd->fields, p->arena, sizeof(void *));

    if (!expect(p, "{", NULL)) return NULL;
    skipJunk(p);
    while (!at(p, "}")) {
        Token *fname = expectIdent(p, "a field name");
        if (!fname) return NULL;
        if (!expect(p, ":", NULL)) return NULL;
        Type *ft = parseType(p);
        if (!ft) return NULL;

        FieldDef *fd = (FieldDef *)arenaAllocZero(p->arena, sizeof(FieldDef));
        fd->name = fname->text;
        fd->type = ft;
        fd->line = fname->line;
        *(FieldDef **)vecPush(&sd->fields) = fd;
        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return sd;
}

static FuncDef *parseFunc(Parser *p) {
    Token *kw = take(p);                    /* fn */
    Token *name = expectIdent(p, "a function name");
    if (!name) return NULL;

    FuncDef *fd = (FuncDef *)arenaAllocZero(p->arena, sizeof(FuncDef));
    fd->name = name->text;
    fd->line = kw->line;
    fd->ret = NULL;
    vecInit(&fd->params, p->arena, sizeof(void *));

    if (!expect(p, "(", NULL)) return NULL;
    skipNl(p);
    while (!at(p, ")")) {
        Token *pn = expectIdent(p, "a parameter name");
        if (!pn) return NULL;
        if (!expect(p, ":", "参数必须写类型：`name: Type`")) return NULL;
        Type *pt = parseType(p);
        if (!pt) return NULL;

        Param *pm = (Param *)arenaAllocZero(p->arena, sizeof(Param));
        pm->name = pn->text;
        pm->type = pt;
        pm->line = pn->line;
        *(Param **)vecPush(&fd->params) = pm;

        if (accept(p, ",")) skipNl(p);
        else break;
    }
    if (!expect(p, ")", NULL)) return NULL;

    if (accept(p, "->")) {
        fd->ret = parseType(p);
        if (!fd->ret) return NULL;
    }

    fd->body = parseBlock(p);
    if (!fd->body) return NULL;
    return fd;
}

static Type *parseType(Parser *p) {
    if (at(p, "ref")) {
        take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        return typeRef(p->arena, inner);
    }
    Token *t = cur(p);
    if (t->kind == TK_TYPE || t->kind == TK_IDENT) {
        take(p);
        return typeNamed(p->arena, t->text);
    }
    ctxError(p->ctx, t->line, t->col, NULL, "expected a type, found `%s`", shown(t));
    return NULL;
}

/* ================================================================ 语句 */

static Stmt *parseBlock(Parser *p) {
    Token *open = cur(p);
    if (!expect(p, "{", NULL)) return NULL;

    Stmt *b = stmtNew(p->arena, ST_BLOCK, open->line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(void *));

    skipJunk(p);
    while (!at(p, "}")) {
        Stmt *s = parseStmt(p);
        if (!s) return NULL;
        *(Stmt **)vecPush(&b->u.block.stmts) = s;
        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return b;
}

static Stmt *parseStmt(Parser *p) {
    Token *t = cur(p);

    if (at(p, "let") || at(p, "var"))  return parseVarDecl(p);
    if (at(p, "if"))                   return parseIf(p);
    if (at(p, "while"))                return parseWhile(p);

    if (at(p, "return")) {
        take(p);
        Stmt *s = stmtNew(p->arena, ST_RETURN, t->line);
        if (atKind(p, TK_NEWLINE) || atKind(p, TK_EOF) || at(p, "}") || at(p, ";")) {
            s->u.ret.value = NULL;
        } else {
            s->u.ret.value = parseExpr(p);
            if (!s->u.ret.value) return NULL;
        }
        return s;
    }

    if (at(p, "break"))    { take(p); return stmtNew(p->arena, ST_BREAK, t->line); }
    if (at(p, "continue")) { take(p); return stmtNew(p->arena, ST_CONTINUE, t->line); }

    if (at(p, "struct") || at(p, "fn")) {
        ctxError(p->ctx, t->line, t->col, "extC 不支持嵌套定义",
                 "`%s` cannot appear inside a function body", t->text);
        return NULL;
    }

    if (at(p, "{")) return parseBlock(p);

    Expr *e = parseExpr(p);
    if (!e) return NULL;

    if (at(p, "=")) {
        take(p);
        skipNl(p);
        Expr *v = parseExpr(p);
        if (!v) return NULL;
        Stmt *s = stmtNew(p->arena, ST_ASSIGN, t->line);
        s->u.assign.target = e;
        s->u.assign.value = v;
        return s;
    }

    Stmt *s = stmtNew(p->arena, ST_EXPR, t->line);
    s->u.expr.expr = e;
    return s;
}

static Stmt *parseVarDecl(Parser *p) {
    Token *kw = take(p);
    Token *name = expectIdent(p, "a variable name");
    if (!name) return NULL;

    Type *ann = NULL;
    if (accept(p, ":")) {
        ann = parseType(p);
        if (!ann) return NULL;
    }

    if (!at(p, "=")) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "week-0 还不支持先声明后赋值（这样 `let` 的不可变语义才成立）",
                 "`%s %s` needs an initializer", kw->text, name->text);
        return NULL;
    }
    take(p);
    skipNl(p);

    Expr *init = parseExpr(p);
    if (!init) return NULL;

    Stmt *s = stmtNew(p->arena, ST_VAR, kw->line);
    s->u.var.name = name->text;
    s->u.var.ann = ann;
    s->u.var.init = init;
    s->u.var.mut = (strcmp(kw->text, "var") == 0);
    return s;
}

static Stmt *parseIf(Parser *p) {
    Token *kw = take(p);                    /* if */
    Expr *cond = parseExpr(p);
    if (!cond) return NULL;

    Stmt *thenBody = parseBlock(p);
    if (!thenBody) return NULL;

    skipJunk(p);
    Stmt *els = NULL;
    if (at(p, "else")) {
        take(p);
        skipJunk(p);
        els = at(p, "if") ? parseIf(p) : parseBlock(p);
        if (!els) return NULL;
    }

    Stmt *s = stmtNew(p->arena, ST_IF, kw->line);
    s->u.ifs.cond = cond;
    s->u.ifs.thenBody = thenBody;
    s->u.ifs.elseBody = els;
    return s;
}

static Stmt *parseWhile(Parser *p) {
    Token *kw = take(p);                    /* while */
    Expr *cond = parseExpr(p);
    if (!cond) return NULL;

    Stmt *body = parseBlock(p);
    if (!body) return NULL;

    Stmt *s = stmtNew(p->arena, ST_WHILE, kw->line);
    s->u.whiles.cond = cond;
    s->u.whiles.body = body;
    return s;
}

/* ================================================================ 表达式 */

static Expr *parseExpr(Parser *p) { return parseOr(p); }

static Expr *parseOr(Parser *p) {
    Expr *e = parseAnd(p);
    if (!e) return NULL;
    while (at(p, "||")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseAnd(p);
        if (!r) return NULL;
        e = mkBin(p, "||", e, r, op->line);
    }
    return e;
}

static Expr *parseAnd(Parser *p) {
    Expr *e = parseEquality(p);
    if (!e) return NULL;
    while (at(p, "&&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseEquality(p);
        if (!r) return NULL;
        e = mkBin(p, "&&", e, r, op->line);
    }
    return e;
}

static Expr *parseEquality(Parser *p) {
    Expr *e = parseComparison(p);
    if (!e) return NULL;
    while (at(p, "==") || at(p, "!=")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseComparison(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

static Expr *parseComparison(Parser *p) {
    Expr *e = parseTerm(p);
    if (!e) return NULL;
    while (at(p, "<") || at(p, "<=") || at(p, ">") || at(p, ">=")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseTerm(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

static Expr *parseTerm(Parser *p) {
    Expr *e = parseFactor(p);
    if (!e) return NULL;
    while (at(p, "+") || at(p, "-")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseFactor(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

static Expr *parseFactor(Parser *p) {
    Expr *e = parseUnary(p);
    if (!e) return NULL;
    while (at(p, "*") || at(p, "/") || at(p, "%")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseUnary(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

static Expr *parseUnary(Parser *p) {
    if (at(p, "!") || at(p, "-")) {
        Token *op = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_UN, op->line);
        e->u.un.op = op->text;
        e->u.un.operand = operand;
        return e;
    }
    return parsePostfix(p);
}

static Expr *parsePostfix(Parser *p) {
    Expr *e = parsePrimary(p);
    if (!e) return NULL;

    for (;;) {
        if (at(p, ".")) {
            Token *dot = take(p);
            Token *name = expectIdent(p, "a field or method name");
            if (!name) return NULL;

            if (at(p, "(")) {
                Vec args;
                if (!parseArgs(p, &args)) return NULL;
                Expr *m = exprNew(p->arena, EX_METHOD, dot->line);
                m->u.method.recv = e;
                m->u.method.name = name->text;
                m->u.method.args = args;
                e = m;
            } else {
                Expr *f = exprNew(p->arena, EX_FIELD, dot->line);
                f->u.field.obj = e;
                f->u.field.name = name->text;
                e = f;
            }
        } else if (at(p, "(")) {
            Vec args;
            if (!parseArgs(p, &args)) return NULL;
            Expr *c = exprNew(p->arena, EX_CALL, e->line);
            c->u.call.callee = e;
            c->u.call.args = args;
            e = c;
        } else {
            return e;
        }
    }
}

static bool parseArgs(Parser *p, Vec *out) {
    vecInit(out, p->arena, sizeof(void *));
    if (!expect(p, "(", NULL)) return false;
    skipNl(p);
    while (!at(p, ")")) {
        Expr *a = parseExpr(p);
        if (!a) return false;
        *(Expr **)vecPush(out) = a;
        if (accept(p, ",")) skipNl(p);
        else break;
    }
    skipNl(p);
    return expect(p, ")", NULL);
}

static Expr *parsePrimary(Parser *p) {
    Token *t = cur(p);

    if (t->kind == TK_INT) {
        take(p);
        Expr *e = exprNew(p->arena, EX_INT, t->line);
        e->u.ival = t->ival;
        return e;
    }
    if (t->kind == TK_FLOAT) {
        take(p);
        Expr *e = exprNew(p->arena, EX_FLOAT, t->line);
        e->u.fval = t->fval;
        return e;
    }
    if (t->kind == TK_STRING) {
        take(p);
        Expr *e = exprNew(p->arena, EX_STR, t->line);
        e->u.str.text = t->text;
        return e;
    }
    if (at(p, "true") || at(p, "false")) {
        take(p);
        Expr *e = exprNew(p->arena, EX_BOOL, t->line);
        e->u.bval = (t->text[0] == 't');
        return e;
    }
    if (at(p, "(")) {
        take(p);
        skipNl(p);
        Expr *e = parseExpr(p);
        if (!e) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        return e;
    }
    if (at(p, "{")) return parseStructLit(p, NULL);

    if (t->kind == TK_IDENT) {
        take(p);
        /* `Name { ... }` 只在首字母大写时当结构体字面量 ——
         * 用命名规范（类型 PascalCase）消歧义，省掉一个关键字。 */
        if (at(p, "{") && isUpperCase(t->text)) return parseStructLit(p, t->text);

        Expr *e = exprNew(p->arena, EX_IDENT, t->line);
        e->u.ident.name = t->text;
        return e;
    }

    ctxError(p->ctx, t->line, t->col, NULL,
             "expected an expression, found `%s`", shown(t));
    return NULL;
}

static Expr *parseStructLit(Parser *p, const char *name) {
    Token *open = cur(p);
    if (!expect(p, "{", NULL)) return NULL;

    Expr *e = exprNew(p->arena, EX_STRUCTLIT, open->line);
    e->u.lit.name = name;
    vecInit(&e->u.lit.inits, p->arena, sizeof(void *));

    skipNl(p);
    while (!at(p, "}")) {
        Token *fn = expectIdent(p, "a field name");
        if (!fn) return NULL;
        if (!expect(p, ":", NULL)) return NULL;
        skipNl(p);
        Expr *v = parseExpr(p);
        if (!v) return NULL;

        FieldInit *fi = (FieldInit *)arenaAllocZero(p->arena, sizeof(FieldInit));
        fi->name = fn->text;
        fi->value = v;
        *(FieldInit **)vecPush(&e->u.lit.inits) = fi;

        skipNl(p);
        accept(p, ",");
        skipNl(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return e;
}
