#include "parser.h"

#include <string.h>

#include "lexer.h"

typedef struct {
    Ctx   *ctx;
    Arena *arena;
    Vec   *toks;
    size_t pos;
    /* 是否正在解析 `if` / `while` 的条件。
     * 在条件位置里，`{` 属于**块**而不是结构体字面量 —— 这就是「位置规则」，
     * 它取代了以前那套「首字母大写才算结构体字面量」的隐藏魔法。 */
    bool   inCond;
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
static GlobalDef *parseGlobalDecl(Parser *p);
static Stmt    *parseIf(Parser *p);
static Stmt    *parseWhile(Parser *p);
static StructDef *parseStruct(Parser *p);
static FuncDef   *parseFunc(Parser *p);
static TypeDef   *parseTypeDecl(Parser *p);

static bool   startsUpper(const char *s);
static Token *expectTypeName(Parser *p, const char *what);

static Expr *parseExpr(Parser *p);
static Expr *parseOr(Parser *p);
static Expr *parseAnd(Parser *p);
static Expr *parseBitOr(Parser *p);
static Expr *parseBitXor(Parser *p);
static Expr *parseBitAnd(Parser *p);
static Expr *parseEquality(Parser *p);
static Expr *parseComparison(Parser *p);
static Expr *parseShift(Parser *p);
static Expr *parseTerm(Parser *p);
static Expr *parseFactor(Parser *p);
static Expr *parseUnary(Parser *p);
static Expr *parsePostfix(Parser *p);
static Expr *parsePrimary(Parser *p);
static Expr *parseStructLit(Parser *p, const char *name);
static bool  looksLikeAssoc(Parser *p);
static Expr *parseAssoc(Parser *p, const char *name, int line);
static bool  parseArgs(Parser *p, Vec *out);

static Expr *mkBin(Parser *p, const char *op, Expr *l, Expr *r, int line) {
    Expr *e = exprNew(p->arena, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = l;
    e->u.bin.right = r;
    return e;
}

/* ================================================================ 顶层 */

/* 把 toks 里的声明**追加**到 out（不重置 —— out 由调用方初始化一次）。
 * prelude 和用户文件就是靠这个进同一个 Module 的，将来多文件编译也一样。 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out) {
    Parser p = { ctx, arena, toks, 0, false };
    skipJunk(&p);

    while (!atKind(&p, TK_EOF) && !ctx->hasError) {
        if (at(&p, "struct")) {
            StructDef *s = parseStruct(&p);
            if (!s) return false;
            *(StructDef **)vecPush(&out->structs) = s;
        } else if (at(&p, "type")) {
            TypeDef *td = parseTypeDecl(&p);
            if (!td) return false;
            *(TypeDef **)vecPush(&out->types) = td;
        } else if (at(&p, "let") || at(&p, "var")) {
            GlobalDef *g = parseGlobalDecl(&p);
            if (!g) return false;
            *(GlobalDef **)vecPush(&out->globals) = g;
        } else if (at(&p, "fn")) {
            FuncDef *f = parseFunc(&p);
            if (!f) return false;
            *(FuncDef **)vecPush(&out->funcs) = f;
        } else {
            Token *t = cur(&p);
            ctxError(ctx, t->line, t->col,
                     "the top level allows `fn`, `struct`, `type`, and `let`/`var` (globals)",
                     "expected `fn`, `struct`, `type`, `let` or `var`, found `%s`", shown(t));
            return false;
        }
        skipJunk(&p);
    }
    return !ctx->hasError;
}

static StructDef *parseStruct(Parser *p) {
    Token *kw = take(p);                    /* struct */
    Token *name = expectTypeName(p, "a struct name");
    if (!name) return NULL;

    StructDef *sd = (StructDef *)arenaAllocZero(p->arena, sizeof(StructDef));
    sd->name = name->text;
    sd->line = kw->line;
    vecInit(&sd->typeParams, p->arena, sizeof(void *));
    vecInit(&sd->fields, p->arena, sizeof(void *));
    vecInit(&sd->methods, p->arena, sizeof(void *));

    /* 泛型参数表：`struct Pair<A, B> { ... }` */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            Token *tp = expectIdent(p, "a type parameter name");
            if (!tp) return NULL;
            if (!startsUpper(tp->text)) {
                ctxError(p->ctx, tp->line, tp->col,
                         "Type parameters start with an uppercase letter, so they can never "
                         "collide with a type name (which is camelCase).",
                         "type parameter `%s` must start with an uppercase letter", tp->text);
                return NULL;
            }
            *(const char **)vecPush(&sd->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

    if (!expect(p, "{", NULL)) return NULL;
    skipJunk(p);
    while (!at(p, "}")) {
        /* 方法写在 struct 体内（定案 9）*/
        if (at(p, "fn")) {
            FuncDef *m = parseFunc(p);
            if (!m) return NULL;
            m->owner = sd;
            *(FuncDef **)vecPush(&sd->methods) = m;
            skipJunk(p);
            continue;
        }

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

/* type Status = | ok | warn | error
 * 前导 `|` 可写可不写。 */
static TypeDef *parseTypeDecl(Parser *p) {
    Token *kw = take(p);                    /* type */
    Token *name = expectTypeName(p, "a type name");
    if (!name) return NULL;

    TypeDef *td = (TypeDef *)arenaAllocZero(p->arena, sizeof(TypeDef));
    td->name = name->text;
    td->line = kw->line;
    vecInit(&td->variants, p->arena, sizeof(void *));

    if (!expect(p, "=", NULL)) return NULL;
    skipNl(p);
    accept(p, "|");
    skipNl(p);

    for (;;) {
        Token *v = expectIdent(p, "a variant name");
        if (!v) return NULL;

        Variant *va = (Variant *)arenaAllocZero(p->arena, sizeof(Variant));
        va->name = v->text;
        va->line = v->line;
        *(Variant **)vecPush(&td->variants) = va;

        skipNl(p);
        if (accept(p, "|")) { skipNl(p); continue; }
        break;
    }
    return td;
}

static bool startsUpper(const char *s) { return s[0] >= 'A' && s[0] <= 'Z'; }

/* 类型名必须首字母小写、泛型参数必须首字母大写 —— **两者集合不相交**，
 * 所以「泛型参数和用户类型重名」在语法上不可能发生。
 * （之前只靠约定，而 `struct T` + `struct box<T>` 是能编译过的：里面的 T 会遮蔽外面的。
 *   能编译但读者看不懂，就是坏设计。）*/
static Token *expectTypeName(Parser *p, const char *what) {
    Token *t = expectIdent(p, what);
    if (!t) return NULL;
    if (startsUpper(t->text)) {
        ctxError(p->ctx, t->line, t->col,
                 "Type names are camelCase (lowercase first letter). "
                 "A leading uppercase letter is reserved for type parameters, "
                 "so the two can never collide.",
                 "type name `%s` must start with a lowercase letter", t->text);
        return NULL;
    }
    return t;
}

/* 函数/方法的名字：普通标识符，或者可重载的运算符（`fn ==(...)`）。
 * 让「定义 ==」这件事在源码里**看得见** —— 不用去查一个隐式的约定名。 */
static Token *expectFuncName(Parser *p) {
    if (atKind(p, TK_IDENT)) return take(p);
    if (at(p, "==") || at(p, "!=")) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col,
             "only `==` and `!=` can be overloaded for now",
             "expected a function name, found `%s`", shown(t));
    return NULL;
}

static FuncDef *parseFunc(Parser *p) {
    Token *kw = take(p);                    /* fn */
    Token *name = expectFuncName(p);
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
        if (!expect(p, ":", "parameters must be typed: `name: Type`")) return NULL;
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
    /* `mut` = 「透过这个值能写」，只对**含引用的东西**有意义：
     *     `mut ref T`      可写的引用
     *     `mut slice<T>`   元素可写的**视图**
     * 裸值不需要（它在 `var` 里本来就写得到）。是不是视图 parser 不知道
     * （名字还没解析），所以这里只标上，合法性由 check 判。 */
    if (at(p, "mut")) {
        Token *m = take(p);
        Type *ty = parseType(p);
        if (!ty) return NULL;
        if (ty->kind != TY_REF && ty->kind != TY_UNRESOLVED) {
            ctxError(p->ctx, m->line, m->col,
                     "`mut` qualifies a reference (`mut ref T`) or a view (`mut slice<T>`); "
                     "a plain value does not need it -- put it in a `var`.",
                     "`mut` cannot qualify this type");
            return NULL;
        }
        ty->mut = true;
        return ty;
    }
    if (at(p, "ref")) {
        take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        return typeRef(p->arena, inner);
    }
    /* 固定数组 `[N]T` —— 多维天然递归（`[15][15]i32` 就是 15 个 `[15]i32`）*/
    if (at(p, "[")) {
        Token *br = take(p);
        if (!atKind(p, TK_INT)) {
            Token *bad = cur(p);
            ctxError(p->ctx, bad->line, bad->col,
                     "For now a fixed array's length must be an integer literal. "
                     "(Compile-time constants will come with `const`.)",
                     "array length must be an integer literal, found `%s`", shown(bad));
            return NULL;
        }
        Token *n = take(p);
        if (!expect(p, "]", NULL)) return NULL;
        Type *elem = parseType(p);
        if (!elem) return NULL;
        if (n->ival <= 0) {
            ctxError(p->ctx, br->line, br->col, NULL,
                     "array length must be positive, got %lld", (long long)n->ival);
            return NULL;
        }
        return typeArray(p->arena, n->ival, elem);
    }

    Token *t = cur(p);
    if (t->kind == TK_TYPE || t->kind == TK_IDENT) {
        take(p);
        Type *ty = typeNamed(p->arena, t->text);

        /* 泛型实参：`Pair<i32, u8>` */
        if (at(p, "<")) {
            take(p);
            skipNl(p);
            vecInit(&ty->targs, p->arena, sizeof(void *));
            for (;;) {
                Type *a = parseType(p);
                if (!a) return NULL;
                *(Type **)vecPush(&ty->targs) = a;
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            skipNl(p);
            if (!expect(p, ">", NULL)) return NULL;
        }
        return ty;
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

/* `match e { 变体 => 语句 ... }`
 *
 * **是语句，不是表达式** —— 跟 `?` 同一个理由：C 没有语句表达式，
 * 所以"match 能返回一个值"这件事在 extC 里做不到（除非引入新机制，那是另一条路）。
 * 每条分支的 body 就是一个语句（通常是个块）。
 *
 * 分支名必须是**枚举的变体名**（编译器对着类型检查，见 check.c）。
 * 暂时不做 `_ =>` 兜底：**穷尽列出所有变体**才是 match 的价值所在 ✓ */
static Stmt *parseMatch(Parser *p) {
    Token *kw = take(p);                    /* match */
    /* ⚠️ 跟 `if` / `while` 的条件一样要**关掉结构体字面量** ——
     * 否则 `match e { ... }` 里的 `{` 会被当成 `e { 字段: 值 }` ✓ */
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *scrut = parseExpr(p);
    p->inCond = saved;
    if (!scrut) return NULL;

    if (!expect(p, "{", "a `match` arm is written `variant => { ... }`")) return NULL;

    Stmt *s = stmtNew(p->arena, ST_MATCH, kw->line);
    s->u.match.scrutinee = scrut;
    vecInit(&s->u.match.arms, p->arena, sizeof(void *));

    skipJunk(p);
    while (!at(p, "}")) {
        Token *name = cur(p);
        if (name->kind != TK_IDENT && name->kind != TK_KEYWORD) {
            ctxError(p->ctx, name->line, name->col,
                     "a `match` arm names one of the enum's variants, e.g. `occupied => { ... }`",
                     "expected a variant name, found `%s`", name->text);
            return NULL;
        }
        take(p);

        if (!expect(p, "=>", NULL)) return NULL;

        MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
        arm->variant = name->text;
        arm->line = name->line;

        /* 分支体：一个块，或者单个语句（`occupied => return 1` 也要能写）*/
        skipNl(p);
        if (at(p, "{")) {
            arm->body = parseBlock(p);
        } else {
            Stmt *inner = parseStmt(p);
            if (!inner) return NULL;
            arm->body = stmtNew(p->arena, ST_BLOCK, inner->line);
            vecInit(&arm->body->u.block.stmts, p->arena, sizeof(void *));
            *(Stmt **)vecPush(&arm->body->u.block.stmts) = inner;
        }
        *(MatchArm **)vecPush(&s->u.match.arms) = arm;

        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return s;
}

static Stmt *parseStmt(Parser *p) {
    Token *t = cur(p);

    if (at(p, "let") || at(p, "var"))  return parseVarDecl(p);
    if (at(p, "if"))                   return parseIf(p);
    if (at(p, "while"))                return parseWhile(p);
    if (at(p, "match"))                return parseMatch(p);

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
        ctxError(p->ctx, t->line, t->col, "extC does not allow nested declarations",
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

/* 顶层的 `let` / `var` —— 全局变量 / 常量。
 * 跟局部声明同一套语法（类型可省、省略初始化式即零初始化），
 * 但**初始化式必须是字面量**：C 的全局初始化器只能是常量表达式。 */
static GlobalDef *parseGlobalDecl(Parser *p) {
    Token *kw = take(p);
    Token *name = expectIdent(p, "a variable name");
    if (!name) return NULL;

    Type *ann = NULL;
    if (accept(p, ":")) {
        ann = parseType(p);
        if (!ann) return NULL;
    }

    Expr *init = NULL;
    if (accept(p, "=")) {
        skipNl(p);
        init = parseExpr(p);
        if (!init) return NULL;
    } else if (ann == NULL) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "without an initializer you must write the type: `var x: T`",
                 "`%s %s` needs a type or an initializer", kw->text, name->text);
        return NULL;
    }

    GlobalDef *g = (GlobalDef *)arenaAllocZero(p->arena, sizeof(GlobalDef));
    g->name = name->text;
    g->ann  = ann;
    g->init = init;
    g->mut  = (strcmp(kw->text, "var") == 0);
    g->line = kw->line;
    return g;
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

    /* 初始化式可省 —— 省略即**零初始化**（定案 8）。
     * C 里最大的 UB 来源之一就是「读到未初始化内存」，它只能在运行时发现；
     * 默认清零把它变成编译期就能保证的东西。 */
    Expr *init = NULL;
    if (accept(p, "=")) {
        skipNl(p);
        init = parseExpr(p);
        if (!init) return NULL;
    } else if (ann == NULL) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "without an initializer you must write the type: `var x: T`",
                 "`%s %s` needs a type or an initializer", kw->text, name->text);
        return NULL;
    }

    Stmt *s = stmtNew(p->arena, ST_VAR, kw->line);
    s->u.var.name = name->text;
    s->u.var.ann = ann;
    s->u.var.init = init;
    s->u.var.mut = (strcmp(kw->text, "var") == 0);
    return s;
}

static Stmt *parseIf(Parser *p) {
    Token *kw = take(p);                    /* if */
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *cond = parseExpr(p);
    p->inCond = saved;
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
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *cond = parseExpr(p);
    p->inCond = saved;
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
    Expr *e = parseBitOr(p);
    if (!e) return NULL;
    while (at(p, "&&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitOr(p);
        if (!r) return NULL;
        e = mkBin(p, "&&", e, r, op->line);
    }
    return e;
}

/* 位运算三层，优先级跟 C 一致（也跟 Go 一致）：
 *     `|`  <  `^`  <  `&`  <  `== !=`  <  `< <=` …  <  `<< >>`  <  `+ -`
 * 跟 C 一致是有意的 —— 写算法题的人手上有 C 的肌肉记忆，
 * 这里要是「设计得更好」反而天天出错。*/
static Expr *parseBitOr(Parser *p) {
    Expr *e = parseBitXor(p);
    if (!e) return NULL;
    while (at(p, "|")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitXor(p);
        if (!r) return NULL;
        e = mkBin(p, "|", e, r, op->line);
    }
    return e;
}

static Expr *parseBitXor(Parser *p) {
    Expr *e = parseBitAnd(p);
    if (!e) return NULL;
    while (at(p, "^")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitAnd(p);
        if (!r) return NULL;
        e = mkBin(p, "^", e, r, op->line);
    }
    return e;
}

static Expr *parseBitAnd(Parser *p) {
    Expr *e = parseEquality(p);
    if (!e) return NULL;
    /* `&` 必须跟 `&&` 分开看：`at(p,"&")` 在 `&&` 上是假的（整词比较）*/
    while (at(p, "&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseEquality(p);
        if (!r) return NULL;
        e = mkBin(p, "&", e, r, op->line);
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
    Expr *e = parseShift(p);
    if (!e) return NULL;
    while (at(p, "<") || at(p, "<=") || at(p, ">") || at(p, ">=")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseShift(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

/* `<<` 和 `>>` —— **故意不放进词法表**。
 *
 * 因为 `box<box<i32>>` 里的 `>>` 必须是两个独立的 `>`（类型实参靠它配对，
 * 见 parseType / looksLikeAssoc）。要是词法层就把 `>>` 合成一个 token，
 * 泛型嵌套类型当场解析不了 —— C++ 当年正是这么踩的坑。
 * 所以在**表达式**这一层用「两个相邻的 `<` / `>`」来认，类型那一层不受影响。 */
static bool atShift(Parser *p, const char *ch, const char **op) {
    if (strcmp(pk(p, 0)->text, ch) != 0 || strcmp(pk(p, 1)->text, ch) != 0) return false;
    if (pk(p, 1)->kind != TK_PUNCT) return false;
    *op = strcmp(ch, "<") == 0 ? "<<" : ">>";
    return true;
}

static Expr *parseShift(Parser *p) {
    Expr *e = parseTerm(p);
    if (!e) return NULL;
    for (;;) {
        const char *op = NULL;
        if (!atShift(p, "<", &op) && !atShift(p, ">", &op)) break;
        int line = cur(p)->line;
        take(p);
        take(p);
        skipNl(p);
        Expr *r = parseTerm(p);
        if (!r) return NULL;
        e = mkBin(p, op, e, r, line);
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
    if (at(p, "!") || at(p, "-") || at(p, "~")) {
        Token *op = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_UN, op->line);
        e->u.un.op = op->text;
        e->u.un.operand = operand;
        return e;
    }
    /* T3：`ref` 在表达式位置是「取引用」(`f(ref x)`)，在类型位置是「引用类型」 */
    if (at(p, "ref")) {
        Token *kw = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_REF, kw->line);
        e->u.ref.operand = operand;
        return e;
    }
    return parsePostfix(p);
}

static Expr *parsePostfix(Parser *p) {
    Expr *e = parsePrimary(p);
    if (!e) return NULL;

    for (;;) {
        /* `e?` —— 失败就顺着往上抛。位置限制在 check 里查（只在三处语句位置上合法） */
        if (at(p, "?")) {
            Token *q = take(p);
            Expr *t = exprNew(p->arena, EX_TRY, q->line);
            t->u.try_.operand = e;
            e = t;
            continue;
        }
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
        } else if (at(p, "[")) {
            /* `a[i]` 索引、`a[lo..hi]` 切片。括号里是普通表达式。 */
            Token *br = take(p);
            skipNl(p);
            const bool savedC = p->inCond;
            p->inCond = false;

            Expr *lo = NULL, *hi = NULL;
            bool isRange = false;
            if (!at(p, "..")) {
                lo = parseExpr(p);
                if (!lo) { p->inCond = savedC; return NULL; }
                skipNl(p);
            }
            if (at(p, "..")) {
                isRange = true;
                take(p);
                skipNl(p);
                if (!at(p, "]")) {
                    hi = parseExpr(p);
                    if (!hi) { p->inCond = savedC; return NULL; }
                }
                skipNl(p);
            }
            p->inCond = savedC;
            if (!expect(p, "]", NULL)) return NULL;

            if (isRange) {
                Expr *sl = exprNew(p->arena, EX_SLICE, br->line);
                sl->u.slice.obj = e;
                sl->u.slice.lo  = lo;
                sl->u.slice.hi  = hi;
                e = sl;
            } else {
                Expr *ix = exprNew(p->arena, EX_INDEX, br->line);
                ix->u.index.obj = e;
                ix->u.index.index = lo;
                e = ix;
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
    const bool savedCond = p->inCond;
    p->inCond = false;
    while (!at(p, ")")) {
        Expr *a = parseExpr(p);
        if (!a) return false;
        *(Expr **)vecPush(out) = a;
        if (accept(p, ",")) skipNl(p);
        else break;
    }
    skipNl(p);
    p->inCond = savedCond;
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
        const bool saved = p->inCond;
        p->inCond = false;          /* 括号里是普通表达式，字面量不再受限 */
        Expr *e = parseExpr(p);
        p->inCond = saved;
        if (!e) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        return e;
    }
    if (at(p, "{")) return parseStructLit(p, NULL);

    /* 数组字面量 `[1, 2, 3]`；末尾的 `...` 表示「剩下的是零值」 */
    if (at(p, "[")) {
        Token *br = take(p);
        Expr *e = exprNew(p->arena, EX_ARRAYLIT, br->line);
        vecInit(&e->u.arraylit.elems, p->arena, sizeof(void *));
        e->u.arraylit.rest = false;

        skipNl(p);
        while (!at(p, "]")) {
            if (at(p, "...")) {
                take(p);
                e->u.arraylit.rest = true;
                skipNl(p);
                break;
            }
            const bool savedC = p->inCond;
            p->inCond = false;
            Expr *el = parseExpr(p);
            p->inCond = savedC;
            if (!el) return NULL;
            *(Expr **)vecPush(&e->u.arraylit.elems) = el;

            skipNl(p);
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, "]", NULL)) return NULL;
        return e;
    }

    if (t->kind == TK_IDENT) {
        take(p);
        /* `name { ... }` 是结构体字面量。
         *
         * 位置规则（跟 Go 一样）：在 `if` / `while` 的**条件位置**里 `{` 属于块，
         * 所以那里不加括号就写不出字面量：`if x == (point { a: 1 }) { }`。
         * 这样类型名不必靠大小写来消歧义 —— 规则写在语法里，不藏在命名里。 */
        if (at(p, "{") && !p->inCond) return parseStructLit(p, t->text);

        /* 关联函数调用 `option<i64>::some(x)` / `point::origin()`。
         *
         * `IDENT <` 跟小于号撞车，所以先**只看不动**地判断题实参到哪里结束、
         * 后面跟的是不是 `::`；是才真的解析（这样错误信息不会在试探里乱喷）。
         * 判据 `::` 本身不是合法运算符，所以两种解释互斥，不会误判。 */
        if (looksLikeAssoc(p)) return parseAssoc(p, t->text, t->line);

        /* 条件位置里的 `{` 属于块 —— 但如果括号里明显是字面量（`{ ident :`），
         * 那就是忘了加括号，给一条能直接照抄的提示 */
        if (at(p, "{") && p->inCond &&
            pk(p, 1)->kind == TK_IDENT && strcmp(pk(p, 2)->text, ":") == 0) {
            Token *bt = cur(p);
            ctxError(p->ctx, bt->line, bt->col,
                     "Inside an `if` / `while` condition a `{` starts the body block. "
                     "To write a struct literal there, wrap it in parentheses.",
                     "struct literal in a condition needs parentheses: `(%s { ... })`",
                     t->text);
            return NULL;
        }

        Expr *e = exprNew(p->arena, EX_IDENT, t->line);
        e->u.ident.name = t->text;
        return e;
    }

    ctxError(p->ctx, t->line, t->col, NULL,
             "expected an expression, found `%s`", shown(t));
    return NULL;
}

/* `Name` 后面是不是接 `类型实参? :: 名字 (`？
 *
 * **只看，不动 pos** —— 因为 `IDENT <` 跟小于号撞车，必须在真的解析之前拿定主意，
 * 否则试探会喷出一堆假错误。判据是 `::`：它不是合法的二元运算符，
 * 所以「关联调用」和「a < b」这两种解释互斥，不会误判。 */
static bool looksLikeAssoc(Parser *p) {
    size_t i = 0;

    bool sawTargs = false;
    if (strcmp(pk(p, i)->text, "<") == 0) {
        sawTargs = true;
        int depth = 0;
        for (;; i++) {
            Token *t = pk(p, i);
            if (t->kind == TK_EOF) return false;
            if (strcmp(t->text, "<") == 0) {
                depth++;
            } else if (strcmp(t->text, ">") == 0) {
                if (--depth == 0) { i++; break; }
            } else if (t->kind == TK_IDENT || t->kind == TK_TYPE ||
                       t->kind == TK_INT) {
                /* 类型名 / 内建类型 / 数组长度 */
            } else if (t->kind == TK_KEYWORD) {
                /* `ref` / `mut` */
            } else if (strcmp(t->text, ",") == 0 || strcmp(t->text, "[") == 0 ||
                       strcmp(t->text, "]") == 0) {
            } else {
                return false;       /* 出现不可能属于类型的东西 ⇒ 不是类型实参 */
            }
        }
    }
    /* `Name<T>::fn(...)` = **关联调用**
     * `name<T>(...)`     = **泛型调用**（目前只有内置原语用它，比如 `alloc<i32>(n)`）
     * 两者共用同一套「先看不动」的类型实参扫描，判据分别是 `::` 和 `(`。 */
    /* 只有**见过类型实参**才可能是泛型调用 —— 否则 `f(x)` 会被误认。 */
    return strcmp(pk(p, i)->text, "::") == 0 ||
           (sawTargs && strcmp(pk(p, i)->text, "(") == 0);
}

/* 真的解析：`Name<targs>::name(args)` 或 `Name::name(args)`（类型名已吃掉） */
static Expr *parseAssoc(Parser *p, const char *name, int line) {
    Vec targs;
    vecInit(&targs, p->arena, sizeof(void *));
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            Type *a = parseType(p);
            if (!a) return NULL;
            *(Type **)vecPush(&targs) = a;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }
    /* 没有 `::` ⇒ 是**泛型调用** `name<T>(args)`（`alloc<i32>(n)`）*/
    if (!at(p, "::")) {
        Vec gargs;
        if (!parseArgs(p, &gargs)) return NULL;
        Expr *g = exprNew(p->arena, EX_GENCALL, line);
        g->u.gencall.name  = name;
        g->u.gencall.targs = targs;
        g->u.gencall.args  = gargs;
        return g;
    }
    take(p);   /* `::` */

    Token *fn = expectIdent(p, "an associated function name");
    if (!fn) return NULL;
    Vec args;
    if (!parseArgs(p, &args)) return NULL;

    Expr *e = exprNew(p->arena, EX_ASSOC, line);
    e->u.assoc.typeName = name;
    e->u.assoc.targs = targs;
    e->u.assoc.name = fn->text;
    e->u.assoc.args = args;
    return e;
}

static Expr *parseStructLit(Parser *p, const char *name) {    Token *open = cur(p);
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
