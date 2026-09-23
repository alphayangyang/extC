/* extC syntax, part two of the pipeline: the recursive-descent parser.
 *
 * Turns the token vector from lexAll into ast.h nodes (see parser.h for the
 * accepted grammar).  It resolves no names: identifiers, module paths, and
 * generic arguments are kept exactly as written, so the loader and the type
 * checker can decide what they mean.  `{` after a name is a struct literal by
 * position, not by capitalization, and p->inCond records where a `{` must be
 * read as the start of a block instead.
 */
#include "parser.h"

#include <stdio.h>      /* fprintf: the EXTC_DBG_QN trace */
#include <stdlib.h>     /* getenv: the EXTC_DBG_QN switch */
#include <string.h>

#include "lexer.h"

typedef struct {
    Ctx   *ctx;
    Arena *arena;
    Vec   *toks;
    size_t pos;
    /* True while parsing the condition of `if` / `while` / `match`.  In a
     * condition a `{` starts the body block, so a struct literal has to be
     * parenthesized.  This positional rule replaced an earlier one that told
     * literals apart by capitalizing the type name. */
    bool   inCond;
    bool   noBody;      /* true while parsing an `extern!` signature (no body) */
} Parser;

/* ---------------------------------------------------------------- lookahead */

/* Return the token `k` positions ahead of the cursor without consuming it.
 *
 * Params:
 *   k - lookahead distance in tokens; 0 is the current token
 *
 * Returns:
 *   That token, or the trailing TK_EOF when the distance runs past the end.
 *
 * Notes:
 *   - Clamping to TK_EOF instead of returning NULL is what lets every caller
 *     read a fixed number of tokens ahead without a bounds check.
 */
static Token *pk(Parser *p, size_t k) {
    size_t i = p->pos + k;
    if (i >= p->toks->len) i = p->toks->len - 1;    /* clamp at EOF */
    return (Token *)vecAt(p->toks, i);
}

/* Return the token at the cursor, without consuming it. */
static Token *cur(Parser *p) { return pk(p, 0); }

/* Report whether the token at the cursor spells `value`.
 *
 * Params:
 *   value - the spelling to compare against, e.g. ")" or "match"
 *
 * Notes:
 *   - A spelling that is punctuation additionally requires the token to BE
 *     punctuation.  A string literal's text is stored without its quotes, so the
 *     source `")"` yields a token whose text is exactly `)`; comparing text
 *     alone made println(")") fail with "expected an expression, found `)`"
 *     while println("x)") was fine.  For keywords and identifiers the plain
 *     text comparison is right and stays.
 */
static bool at(Parser *p, const char *value) {
    Token *t = cur(p);
    if (t->kind == TK_STRING && lexIsPunct(value)) return false;
    return strcmp(t->text, value) == 0;
}

/* Report whether the token at the cursor has kind `k`, ignoring its text. */
static bool atKind(Parser *p, TokenKind k) { return cur(p)->kind == k; }

/* Consume the token at the cursor and return it.
 *
 * Notes:
 *   - The cursor never moves past the trailing TK_EOF, so a caller that keeps
 *     taking tokens on malformed input cannot walk off the vector.
 */
static Token *take(Parser *p) {
    Token *t = cur(p);
    if (p->pos + 1 < p->toks->len) p->pos++;
    return t;
}

/* Consume the token at the cursor when it spells `value`.
 *
 * Returns:
 *   true when it matched and was consumed; false, with the cursor unchanged,
 *   otherwise.
 */
static bool accept(Parser *p, const char *value) {
    if (!at(p, value)) return false;
    take(p);
    return true;
}

/* Return a printable spelling for `t` to drop into an error message.
 *
 * Notes:
 *   - A token with empty text (TK_EOF, TK_NEWLINE) shows its kind name instead,
 *     so a message never ends in "found ``".
 */
static const char *shown(const Token *t) {
    return t->text[0] ? t->text : tokenKindName(t->kind);
}

/* Consume the token at the cursor when it spells `value`, else report it.
 *
 * Params:
 *   value - required spelling, e.g. "{"
 *   note  - optional explanation stored with the error, usually the syntax rule
 *           the caller was following; NULL when there is nothing to add
 *
 * Returns:
 *   true when consumed; false after reporting through ctxError, in which case
 *   the caller must return immediately.
 */
static bool expect(Parser *p, const char *value, const char *note) {
    if (at(p, value)) { take(p); return true; }
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, note,
             "expected `%s`, found `%s`", value, shown(t));
    return false;
}

/* Consume an identifier at the cursor, else report what was needed.
 *
 * Params:
 *   what - description of the missing item, phrased to follow "expected",
 *          e.g. "a field name"
 *
 * Returns:
 *   The consumed token, or NULL after reporting through ctxError.
 */
static Token *expectIdent(Parser *p, const char *what) {
    if (atKind(p, TK_IDENT)) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, NULL,
             "expected %s, found `%s`", what, shown(t));
    return NULL;
}

/* Consume any run of newline tokens, and nothing else. */
static void skipNl(Parser *p) {
    while (atKind(p, TK_NEWLINE)) take(p);
}

/* Newlines and an optional `;` are both statement separators. */
static void skipJunk(Parser *p) {
    for (;;) {
        if (atKind(p, TK_NEWLINE) || at(p, ";")) { take(p); continue; }
        return;
    }
}

/* ---------------------------------------------------------------- forward declarations */

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

/* Report whether `n` is one of the ten scalar builtin type names.
 *
 * Notes:
 *   - `bool` and `void` are excluded: an explicit conversion needs a scalar
 *     numeric type on both sides, and extC has no implicit truth conversion.
 */
static bool isScalarTypeName(const char *n) {
    static const char *N[] = { "i8","i16","i32","i64","u8","u16","u32","u64","f32","f64", NULL };
    for (size_t i = 0; N[i]; i++) if (strcmp(N[i], n) == 0) return true;
    return false;
}
static Expr *parseStructLit(Parser *p, const char *name);
static int   looksLikeAssoc(Parser *p);
static Expr *parseAssoc(Parser *p, const char *name, int line, size_t startPos,
                        const char **modPrefixOut);
static bool  parseArgs(Parser *p, Vec *out);

/* Build a binary-operator Expr node.
 *
 * Params:
 *   op   - operator spelling, e.g. "||"; stored as written for the code generator
 *   l, r - operand expressions, already parsed
 *   line - source line to attach to the node, for diagnostics
 *
 * Returns:
 *   The new EX_BIN node, allocated in the parser arena.
 */
static Expr *mkBin(Parser *p, const char *op, Expr *l, Expr *r, int line) {
    Expr *e = exprNew(p->arena, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = l;
    e->u.bin.right = r;
    return e;
}

/* ================================================================ top level */

/* Parse a `use a::b::c` import declaration.
 *
 * Notes:
 *   - A file is a module, so `use std::io` names the file std/io.extc; the
 *     loader owns the search path and the cycle check.
 *   - The short name is the last path segment and is what the file may call the
 *     module by.
 *   - A `use` declaration only records the path.  Nothing is resolved here.
 */
static UseDecl *parseUse(Parser *p) {
    Token *kw = take(p);                       /* use */
    Token *first = expectTypeName(p, "a module path (e.g. `use std::io`)");
    if (!first) return NULL;
    Buf  path;
    bufInit(&path, p->arena);
    bufPuts(&path, first->text);
    const char *shortName = first->text;
    while (at(p, "::")) {
        take(p);
        Token *seg = expectTypeName(p, "a module path segment");
        if (!seg) return NULL;
        bufPuts(&path, "::");
        bufPuts(&path, seg->text);
        shortName = seg->text;                 /* short name = last segment */
    }
    /* `use std::sys::io as sysio` -- an alias is not a convenience here but a
     * necessity: std::io and std::sys::io have the same short name `io`, and
     * reading stdin while opening a file needs both modules in reach at once.
     *
     * `path` keeps the full name as written, because the loader matches a deep
     * qualified name full-name to full-name (see importedAsPath in modules.c);
     * only `shortName` becomes the alias. */
    const char *alias = NULL;
    if (at(p, "as")) {
        take(p);
        Token *a = expectIdent(p, "an alias name (e.g. `use std::sys::io as sysio`)");
        if (!a) return NULL;
        alias = a->text;
    }
    UseDecl *u = (UseDecl *)arenaAllocZero(p->arena, sizeof(UseDecl));
    u->path      = bufCstr(&path);
    u->shortName = alias ? alias : shortName;
    u->line      = kw->line;
    return u;
}

/* Parse an `extern!("libc") fn name(...)` declaration of a C function.
 *
 * The shape is:
 *   extern!("libc") fn read(fd: i32, buf: ref u8, n: i64) -> i64
 *                   effects Addr=0 Cont=0    // the call stores nothing
 *
 * Notes:
 *   - A parameter or return type may only be a scalar or a single pointer
 *     (`ref T`, `?ref T`), because that is what C has.  A `slice<T>` is two C
 *     parameters (pointer plus length), and the mapping cannot be expressed
 *     across the boundary, so a stdlib wrapper has to spell it out instead.
 *   - The effect declaration is what the author asserts about the call.  Writing
 *     none means the worst case is assumed -- every argument may be stored --
 *     which is safe but costs the caller freedom at the call site.
 *   - The returned C function is declared, not defined: p->noBody makes parseFunc
 *     stop after the signature, so no body is parsed and the C library is the
 *     implementation.
 */
static FuncDef *parseExtern(Parser *p) {
    Token *kw = take(p);                       /* extern */
    if (!expect(p, "!", NULL)) return NULL;
    if (!expect(p, "(", NULL)) return NULL;
    Token *lib = cur(p);
    if (lib->kind != TK_STRING) {
        ctxError(p->ctx, lib->line, lib->col, NULL,
                 "`extern!(\"lib\")` needs the C library's name as a string (for diagnostics)");
        return NULL;
    }
    take(p);
    if (!expect(p, ")", NULL)) return NULL;
    if (!at(p, "fn")) {
        ctxError(p->ctx, kw->line, kw->col, NULL, "`extern!` must be followed by a `fn` declaration");
        return NULL;
    }
    p->noBody = true;                          /* signature only, no body */
    FuncDef *f = parseFunc(p);
    p->noBody = false;
    if (!f) return NULL;
    f->isExtern  = true;
    f->externLib = lib->text;
    f->body = NULL;                            /* an external declaration has no body */

    /* The caller's assertion about effects, spread over as many lines as it
     * needs.  Each round skips newlines first, or at() would see a TK_NEWLINE
     * and miss the `effects` keyword. */
    while (true) {
        skipJunk(p);
        if (at(p, "effects")) {
            take(p);
            f->hasEffects = true;
            for (;;) {
                Token *nm = expectIdent(p, "`Addr` or `Cont`");
                if (!nm) return NULL;
                if (!expect(p, "=", NULL)) return NULL;
                Token *val = cur(p);
                if (val->kind != TK_INT) {
                    ctxError(p->ctx, val->line, val->col, NULL,
                             "`effects` takes small integers, e.g. `effects Addr=0 Cont=0`");
                    return NULL;
                }
                take(p);
                unsigned bits = (unsigned)val->ival;
                if (strcmp(nm->text, "Addr") == 0)      f->extAddrMask = bits;
                else if (strcmp(nm->text, "Cont") == 0) f->extContMask = bits;
                else {
                    ctxError(p->ctx, nm->line, nm->col,
                             "`Addr` = it stores `&argument`; `Cont` = it stores a pointer it"
                             " read out of an argument. Bit i = the i-th argument.",
                             "unknown effect `%s` (only `Addr` and `Cont` exist)", nm->text);
                    return NULL;
                }
                if (at(p, "Addr") || at(p, "Cont")) continue;
                break;
            }
            continue;
        }
        if (at(p, "owned")) {
            Token *ow = take(p);
            ctxError(p->ctx, ow->line, ow->col,
                     "Memory returned by C has to be released, and extC has no `free`."
                     " The planned answer is a resource value owned by the frame that created"
                     " it, which is not implemented yet. Until then, declare only C functions"
                     " that write into memory the caller already owns.",
                     "`owned` is not implemented yet");
            return NULL;
        }
        break;
    }
    return f;
}

/* Parse the whole token vector and append its declarations to `out`.
 *
 * Params:
 *   ctx   - reports the first syntax error through ctxError
 *   arena - owns every AST node and copied name
 *   toks  - tokens from lexAll, ending in TK_EOF
 *   out   - Module to append to; the caller initializes it once, which is how
 *           the prelude and the user file land in the same module
 *
 * Returns:
 *   true when parsing finished without an error; false when ctx->hasError is
 *   set, in which case `out` holds what was parsed up to that point.
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out) {
    Parser p = { ctx, arena, toks, 0, false, false };
    skipJunk(&p);

    while (!atKind(&p, TK_EOF) && !ctx->hasError) {
        /* `use a::b` is a semantic import: the loader finds the file, checks for
         * import cycles, and resolves the qualified names it introduces. */
        if (at(&p, "use")) {
            UseDecl *u = parseUse(&p);
            if (!u) return false;
            *(UseDecl **)vecPush(&out->uses) = u;
            skipJunk(&p);
            continue;
        }
        /* The top-level annotation `@private`.  Declarations are public by
         * default, so hiding one has to be written out. */
        bool isPrivate = false;
        if (at(&p, "@")) {
            Token *a = take(&p);
            Token *nm = expectIdent(&p, "an annotation name (only `private` at the top level)");
            if (!nm) return false;
            if (strcmp(nm->text, "private") != 0) {
                ctxError(ctx, a->line, a->col,
                         "The only top-level annotation today is `@private` (hide a"
                         " declaration from other modules). `@overwrite` is for locals.",
                         "unknown top-level annotation `@%s`", nm->text);
                return false;
            }
            isPrivate = true;
            skipJunk(&p);
        }
        if (at(&p, "struct")) {
            StructDef *s = parseStruct(&p);
            if (!s) return false;
            s->isPrivate = isPrivate;
            *(StructDef **)vecPush(&out->structs) = s;
        } else if (at(&p, "type")) {
            TypeDef *td = parseTypeDecl(&p);
            if (!td) return false;
            td->isPrivate = isPrivate;
            *(TypeDef **)vecPush(&out->types) = td;
        } else if (at(&p, "let") || at(&p, "var")) {
            GlobalDef *g = parseGlobalDecl(&p);
            if (!g) return false;
            g->isPrivate = isPrivate;
            *(GlobalDef **)vecPush(&out->globals) = g;
        } else if (at(&p, "extern")) {
            FuncDef *f = parseExtern(&p);
            if (!f) return false;
            f->isPrivate = isPrivate;
            *(FuncDef **)vecPush(&out->funcs) = f;
        } else if (at(&p, "fn")) {
            FuncDef *f = parseFunc(&p);
            if (!f) return false;
            f->isPrivate = isPrivate;
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

/* Parse a `struct` declaration with its type parameters, fields, and methods.
 *
 * Returns:
 *   The new StructDef, or NULL after reporting an error.
 */
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

    /* Type parameter list: `struct Pair<A, B> { ... }` */
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
        /* A method is declared inside the struct body, like a field. */
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

/* Parse a `type Name = | v1 | v2(T)` declaration of a variant type.
 *
 * The leading `|` before the first variant is optional.
 *
 * Returns:
 *   The new TypeDef, or NULL after reporting an error.
 */
static TypeDef *parseTypeDecl(Parser *p) {
    Token *kw = take(p);                    /* type */
    Token *name = expectTypeName(p, "a type name");
    if (!name) return NULL;

    TypeDef *td = (TypeDef *)arenaAllocZero(p->arena, sizeof(TypeDef));
    td->name = name->text;
    td->line = kw->line;
    vecInit(&td->typeParams, p->arena, sizeof(void *));
    vecInit(&td->variants, p->arena, sizeof(void *));

    /* Type parameter list: `type option<T> = | none | some(T)`, the same shape
     * as on a struct. */
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
            *(const char **)vecPush(&td->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

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
        vecInit(&va->types, p->arena, sizeof(void *));

        /* Payload: `| circle(f64) | rect(f64, f64)`.  The parentheses hold
         * types, positionally and without field names, which is also how a match
         * arm binds them. */
        skipNl(p);
        if (at(p, "(")) {
            take(p);
            skipNl(p);
            while (!at(p, ")")) {
                Type *pt = parseType(p);
                if (!pt) return NULL;
                *(Type **)vecPush(&va->types) = pt;
                skipNl(p);
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            if (!expect(p, ")", NULL)) return NULL;
        }
        *(Variant **)vecPush(&td->variants) = va;

        skipNl(p);
        if (accept(p, "|")) { skipNl(p); continue; }
        break;
    }
    return td;
}

/* Report whether `s` begins with an uppercase ASCII letter. */
static bool startsUpper(const char *s) { return s[0] >= 'A' && s[0] <= 'Z'; }

/* Consume a type name, which must start with a lowercase letter.
 *
 * Notes:
 *   - Type names are camelCase and type parameters start uppercase, so the two
 *     sets are disjoint and a type parameter can never collide with a type name.
 *     That used to be a convention only, and `struct T` beside `struct box<T>`
 *     compiled happily -- the inner T shadowed the outer one.  Code that
 *     compiles but cannot be read is a design bug, so the rule is enforced here.
 */
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

/* Consume a function or method name: an identifier, or an overloadable
 * operator such as `fn ==(self, other)`.
 *
 * Notes:
 *   - Writing the operator itself keeps the definition visible at the
 *     declaration.  The alternative, an implicitly agreed name, would have to be
 *     looked up somewhere else.
 */
static Token *expectFuncName(Parser *p) {
    if (atKind(p, TK_IDENT)) return take(p);
    if (at(p, "==") || at(p, "!=")) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col,
             "only `==` and `!=` can be overloaded for now",
             "expected a function name, found `%s`", shown(t));
    return NULL;
}

/* Parse a `fn` declaration: name, type parameters, parameters, return type,
 * and body.
 *
 * Returns:
 *   The new FuncDef, or NULL after reporting an error.
 *
 * Notes:
 *   - When p->noBody is set (an `extern!` declaration) parsing stops after the
 *     return type and `fd->body` stays NULL.  Reusing this function rather than
 *     copying it is deliberate: the parameters, return type, and generic list
 *     would drift apart between the two copies.
 */
static FuncDef *parseFunc(Parser *p) {
    Token *kw = take(p);                    /* fn */
    Token *name = expectFuncName(p);
    if (!name) return NULL;

    FuncDef *fd = (FuncDef *)arenaAllocZero(p->arena, sizeof(FuncDef));
    fd->name = name->text;
    fd->line = kw->line;
    fd->ret = NULL;
    fd->ctx  = p->ctx;                         /* so later passes report in this file */
    vecInit(&fd->params, p->arena, sizeof(void *));
    vecInit(&fd->typeParams, p->arena, sizeof(void *));
    vecInit(&fd->targs, p->arena, sizeof(void *));
    /* `fn f<T, U>(...)`: type parameters on a free function, in the same shape
     * as the list a struct carries. */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            /* A type parameter starts uppercase (`T`), so this has to be
             * expectIdent.  expectTypeName rejects exactly that shape -- it is
             * there to reserve uppercase for type parameters. */
            Token *tp = expectIdent(p, "a type parameter name (uppercase, e.g. `T`)");
            if (!tp) return NULL;
            *(const char **)vecPush(&fd->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

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

    /* An `extern!` declaration has no body; p->noBody told parseFunc to stop
     * after the return type. */
    if (p->noBody) return fd;
    fd->body = parseBlock(p);
    if (!fd->body) return NULL;
    return fd;
}

/* Parse a type expression: `mut`, `?`, `ref`, `[N]T`, a name, and generic
 * arguments.
 *
 * Returns:
 *   The new Type node, or NULL after reporting an error.
 *
 * Notes:
 *   - A module-qualified name such as `io::File` is collected into one string.
 *     This function does not consult a symbol table, so the loader is the only
 *     place that decides whether the path names a module or a type.
 */
static Type *parseType(Parser *p) {
    /* `mut` means "writing through this value is allowed".  It only means
     * something for something that contains a reference:
     *     `mut ref T`     a writable reference
     *     `mut slice<T>`  a view whose elements may be written
     * A plain value does not need it -- a `var` binding is already writable.
     * Whether `slice<T>` really is a view cannot be decided here, because the
     * name is not resolved yet, so this only records the modifier and the type
     * checker rules on it. */
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
    /* `?T` means "may be absent", the same idea as the postfix `?`:
     *   `?ref T` / `mut ?ref T`  -> a nullable reference; null is its zero value
     *   `?T` for anything else   -> sugar for `option<T>`
     * One is a type and the other an expression suffix, so they never clash. */
    if (at(p, "?")) {
        Token *q = take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        if (inner->kind == TY_REF) {          /* `?ref T` */
            inner->nullable = true;
            return inner;
        }
        /* Everything else: build an `option<inner>` and let the checker
         * resolve that name. */
        Type *o = typeNamed(p->arena, "option");
        (void)q;
        vecInit(&o->targs, p->arena, sizeof(void *));
        *(Type **)vecPush(&o->targs) = inner;
        return o;
    }
    if (at(p, "ref")) {
        take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        return typeRef(p->arena, inner);
    }
    /* A fixed array `[N]T`.  Dimensions nest by recursion: `[15][15]i32` is
     * fifteen elements of type `[15]i32`. */
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
        /* A type name may be module-qualified, as in `io::File`.  The parser
         * keeps no symbol table, so the path is only collected into one string
         * here; the loader resolves it. */
        const char *nm = t->text;
        if (at(p, "::")) {
            Buf b;
            bufInit(&b, p->arena);
            bufPuts(&b, t->text);
            while (at(p, "::")) {
                take(p);
                Token *seg = expectTypeName(p, "a type name");
                if (!seg) return NULL;
                bufPuts(&b, "::");
                bufPuts(&b, seg->text);
            }
            nm = bufCstr(&b);
        }
        Type *ty = typeNamed(p->arena, nm);

        /* Generic arguments: `Pair<i32, u8>` */
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

/* ================================================================ statements */

/* Parse a `{ ... }` block into one ST_BLOCK statement.
 *
 * Returns:
 *   The block statement, or NULL after reporting an error.
 */
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

/* Parse `match e { variant => statement ... }`.
 *
 * Notes:
 *   - This is a statement, not an expression, for the same reason `?` is: C has
 *     no statement expressions, so "match yields a value" is not expressible
 *     here without a new mechanism.  Each arm's body is a single statement,
 *     usually a block.
 *   - An arm names one of the enum's variants; that the variant exists and that
 *     the payload binds match is checked against the type in check.c.
 *   - There is no `_ =>` fallback on purpose.  Listing every variant is the
 *     value match provides.
 */
static Stmt *parseMatch(Parser *p) {
    Token *kw = take(p);                    /* match */
    /* Struct literals have to be off here, exactly as in an `if` / `while`
     * condition: otherwise the `{` of `match e { ... }` would be read as the
     * start of `e { field: value }`. */
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

        MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
        arm->variant = name->text;
        arm->line = name->line;
        vecInit(&arm->binds, p->arena, sizeof(void *));

        /* Payload binding: `circle(r) => ...` / `rect(w, h) => ...`.  It has
         * to be parsed before the `=>`, because it directly follows the
         * variant name. */
        skipNl(p);
        if (at(p, "(")) {
            take(p);
            skipNl(p);
            while (!at(p, ")")) {
                Token *b = expectIdent(p, "a name to bind the payload to");
                if (!b) return NULL;
                *(const char **)vecPush(&arm->binds) = b->text;
                skipNl(p);
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            if (!expect(p, ")", NULL)) return NULL;
            skipNl(p);
        }

        if (!expect(p, "=>", NULL)) return NULL;

        /* Arm body: a block, or a single statement -- `occupied => return 1`
         * has to stay writable. */
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

/* Parse one statement: a declaration, a control statement, a block, an
 * assignment, or an expression statement.
 *
 * Returns:
 *   The new Stmt, or NULL after reporting an error.
 */
static Stmt *parseStmt(Parser *p) {
    Token *t = cur(p);

    /* An annotation `@xxx` may only introduce a local declaration, and
     * `@overwrite` is the only one that exists.  A function annotation like
     * `@main` is not implemented, and the error says so rather than leaving the
     * reader to guess. */
    if (at(p, "@")) {
        Token *a = take(p);
        Token *nm = expectIdent(p, "an annotation name (only `overwrite` for now)");
        if (!nm) return NULL;
        if (strcmp(nm->text, "overwrite") != 0) {
            ctxError(p->ctx, a->line, a->col,
                     "Annotations are compile-time instructions written in the source, so a"
                     " typo must not be silently ignored. Right now the only one is"
                     " `@overwrite` (reuse one piece of storage).",
                     "unknown annotation `@%s` -- only `@overwrite` exists today", nm->text);
            return NULL;
        }
        if (!(at(p, "let") || at(p, "var"))) {
            ctxError(p->ctx, a->line, a->col,
                     "`@overwrite` says \"this allocation is reused\", so it must sit on a"
                     " local declaration: `@overwrite var n = new node`.",
                     "`@overwrite` must be followed by a `var` declaration");
            return NULL;
        }
        Stmt *v = parseVarDecl(p);
        if (v) v->u.var.overwrite = true;
        return v;
    }
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

/* Parse a top-level `let` / `var`, i.e. a global variable or constant.
 *
 * Notes:
 *   - The syntax matches a local declaration: the type may be omitted and an
 *     omitted initializer means zero initialization.
 *   - The initializer must be a literal, because a C global initializer can only
 *     be a constant expression.
 */
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
/* Parse a local `let` / `var` declaration.
 *
 * Returns:
 *   The ST_VAR statement, or NULL after reporting an error.
 *
 * Notes:
 *   - Without an initializer the type annotation becomes mandatory; with
 *     neither, the error names the binding the reader forgot to type.
 */
static Stmt *parseVarDecl(Parser *p) {
    Token *kw = take(p);
    Token *name = expectIdent(p, "a variable name");
    if (!name) return NULL;

    Type *ann = NULL;
    if (accept(p, ":")) {
        ann = parseType(p);
        if (!ann) return NULL;
    }

    /* The initializer may be omitted, and omitting it means zero
     * initialization.  Reading uninitialized memory is one of the largest
     * sources of undefined behaviour in C, detectable only at run time;
     * defaulting to zero turns it into something the compiler guarantees. */
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

/* Parse `if <cond> <block>` with an optional `else` block or `else if`.
 *
 * Notes:
 *   - p->inCond is set while the condition is parsed, so a `{` there starts the
 *     body block rather than a struct literal.
 */
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

/* Parse `while <cond> <block>`.
 *
 * Notes:
 *   - The condition is parsed with p->inCond set, as in parseIf, so a `{` there
 *     starts the loop body.
 */
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

/* ================================================================ expressions */

/* Parse `a ?? b`, the lowest-precedence binary operator.
 *
 * Notes:
 *   - It is right-associative: `a ?? b ?? c` means `a ?? (b ?? c)`.
 *   - Where it may appear is a separate rule: the left side has to be an
 *     option, a result, or a `?ref T`, and check.c enforces that.
 */
static Expr *parseCoalesce(Parser *p) {
    Expr *l = parseOr(p);
    if (!l) return NULL;
    while (at(p, "??")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseCoalesce(p);        /* right-associative */
        if (!r) return NULL;
        Expr *e = exprNew(p->arena, EX_COALESCE, op->line);
        e->u.coalesce.main = l;
        e->u.coalesce.fallback = r;
        l = e;
    }
    return l;
}

/* Parse a full expression, the entry point for every caller that wants one. */
static Expr *parseExpr(Parser *p) { return parseCoalesce(p); }

/* Parse `||`, the logical-or level. */
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

/* Parse `&&`, the logical-and level. */
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

/* The three bitwise levels, with the precedence C and Go use:
 *     `|`  <  `^`  <  `&`  <  `== !=`  <  `< <=` ...  <  `<< >>`  <  `+ -`
 * Matching C is deliberate.  Anyone writing an algorithm here has C muscle
 * memory, and "designing it better" would only produce daily mistakes. */
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

/* Parse `^`. */
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

/* Parse `&` at the bitwise level. */
static Expr *parseBitAnd(Parser *p) {
    Expr *e = parseEquality(p);
    if (!e) return NULL;
    /* `&` stays distinct from `&&`: at(p, "&") is false on a `&&` token,
     * because the comparison covers the whole token text. */
    while (at(p, "&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseEquality(p);
        if (!r) return NULL;
        e = mkBin(p, "&", e, r, op->line);
    }
    return e;
}

/* Parse `==` and `!=`. */
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

/* Parse `<`, `<=`, `>`, and `>=`. */
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

/* Recognize a shift operator at the cursor.
 *
 * Params:
 *   ch  - the character to look for twice in a row, "<" or ">"
 *   op  - receives the operator spelling, "<<" or ">>"
 *
 * Returns:
 *   true when two adjacent tokens both spell `ch` (the second must be
 *   punctuation, so a string literal's text cannot fake it).
 *
 * Notes:
 *   - `<<` and `>>` are deliberately absent from the lexer's table.  The `>>`
 *     in `box<box<i32>>` has to stay two separate `>` tokens, because that is
 *     how generic arguments pair up (see parseType and looksLikeAssoc).  Fusing
 *     them in the lexer would make a nested generic type unparsable, which is
 *     the trap C++ fell into.  Only expressions see a shift operator; the type
 *     grammar never does.
 */
static bool atShift(Parser *p, const char *ch, const char **op) {
    if (strcmp(pk(p, 0)->text, ch) != 0 || strcmp(pk(p, 1)->text, ch) != 0) return false;
    if (pk(p, 1)->kind != TK_PUNCT) return false;
    *op = strcmp(ch, "<") == 0 ? "<<" : ">>";
    return true;
}

/* Parse `<<` and `>>`, which are recognized as two adjacent `<` / `>` tokens. */
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

/* Parse `+` and `-`, the additive level. */
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

/* Parse `*`, `/`, and `%`, the multiplicative level. */
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

/* Parse a prefix expression: `!`, `-`, `~`, the dereference `*p`, and the
 * reference `ref x`.
 *
 * Returns:
 *   The operand expression, or NULL after reporting an error.
 *
 * Notes:
 *   - `*` is unambiguous in prefix position, because multiplication always has
 *     a left operand.
 */
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
    /* `*p` is the explicit dereference, the dual of `ref x`.  Read it as "the
     * value p points at", write it as "the place p points at" (`*p = v`).  The
     * prefix position of `*` was free, since multiplication is binary. */
    if (at(p, "*")) {
        Token *op = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_DEREF, op->line);
        e->u.deref.operand = operand;
        return e;
    }
    /* In expression position `ref` takes a reference (`f(ref x)`); in type
     * position it is a reference type. */
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

/* Parse the postfix chain: `e!`, `e?`, `.field`, `.method(...)`, `[i]`,
 * `[lo..hi]`, and `(...)` calls.
 *
 * Returns:
 *   The outermost expression built so far, or NULL after reporting an error.
 */
static Expr *parsePostfix(Parser *p) {
    Expr *e = parsePrimary(p);
    if (!e) return NULL;

    for (;;) {
        /* `e!` is the programmer's assertion that the value is present: it
         * unwraps an option or a result, or drops the nullability of a `?ref T`,
         * and the checker emits no test for it.  Being wrong is undefined
         * behaviour rather than a checked error.  Position alone separates it
         * from the prefix `!` (`!x` negates, `x!` asserts), and `!=` is a single
         * token, so neither collides with it. */
        if (at(p, "!")) {
            Token *b = take(p);
            Expr *sg = exprNew(p->arena, EX_SIGN, b->line);
            sg->u.sign.operand = e;
            e = sg;
            continue;
        }
        /* `e?` propagates a failure to the caller.  Where it may appear is a
         * separate rule checked in check.c, not a syntactic one. */
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
            /* `a[i]` indexes and `a[lo..hi]` slices.  The brackets hold an
             * ordinary expression, so struct literals are allowed inside. */
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

/* Parse a parenthesized, comma-separated argument list into `out`.
 *
 * Params:
 *   out - receives the Expr pointers; initialized here, so the caller passes it
 *         uninitialized
 *
 * Returns:
 *   true when the closing `)` was consumed; false after reporting an error.
 *
 * Notes:
 *   - The cursor must be on the opening `(`, which this consumes.
 *   - p->inCond is cleared inside the parentheses, because there a `{` is
 *     unambiguously a struct literal again.
 */
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

/* Parse a primary expression: a literal, `null`, `new`, a parenthesized
 * expression, an array or struct literal, a conversion, or a name.
 *
 * Returns:
 *   The new Expr, or NULL after reporting an error.
 */
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
    /* `null` is a contextual keyword, recognized in expression position only,
     * like `ref` and `mut`.  Its type comes entirely from the context:
     * `var p: ?ref node = null`, `if p != null`, or `return null` when the
     * return type is `?ref T`. */
    if (at(p, "null")) {
        take(p);
        return exprNew(p->arena, EX_NULL, t->line);
    }
    if (at(p, "true") || at(p, "false")) {
        take(p);
        Expr *e = exprNew(p->arena, EX_BOOL, t->line);
        e->u.bval = (t->text[0] == 't');
        return e;
    }
    /* `new T` / `new T[n]` / `new [N]T` takes a zeroed block from the arena of
     * the current block.  `new` is a contextual keyword, recognized in
     * expression position only. */
    if (at(p, "new")) {
        Token *kw = take(p);
        skipNl(p);
        Expr *e = exprNew(p->arena, EX_NEW, kw->line);
        Type *ty = parseType(p);
        if (!ty) return NULL;
        e->u.new_.type = ty;
        /* No skipNl here.  Statements end at a newline, so consuming one would
         * swallow the next line into this expression: with `new [3]i32` on its
         * own line, the following line disappeared and the error was "expected
         * an expression, found `=`".  `new T[n]` therefore has to be written on
         * one line, as `a[..]` already is. */
        if (at(p, "[")) {
            take(p);
            skipNl(p);
            const bool savedC = p->inCond;
            p->inCond = false;
            Expr *n = parseExpr(p);
            p->inCond = savedC;
            if (!n) return NULL;
            e->u.new_.count = n;
            skipNl(p);
            if (!expect(p, "]", NULL)) return NULL;
        }
        return e;
    }
    if (at(p, "(")) {
        take(p);
        skipNl(p);
        const bool saved = p->inCond;
        p->inCond = false;          /* inside parentheses a `{` is a literal again */
        Expr *e = parseExpr(p);
        p->inCond = saved;
        if (!e) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        return e;
    }
    if (at(p, "{")) return parseStructLit(p, NULL);

    /* Array literal `[1, 2, 3]`.  A trailing `...` means the remaining
     * elements are zero. */
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

    /* The ten scalar type names lex as TK_TYPE, not as TK_IDENT or TK_KEYWORD,
     * so this case has to be tested before the identifier branch below. */
    if (t->kind == TK_TYPE && isScalarTypeName(t->text)) {
        take(p);
        if (!at(p, "(")) {
            ctxError(p->ctx, t->line, t->col,
                     "a type name only appears in expression position as an explicit"
                     " conversion, e.g. `i32(x)`",
                     "`%s` is a type, not a value -- did you mean `%s(x)`?", t->text, t->text);
            return NULL;
        }
        take(p);                                   /* ( */
        skipNl(p);
        Expr *in = parseExpr(p);
        if (!in) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        Expr *cv2 = exprNew(p->arena, EX_CONV, t->line);
        cv2->u.conv.typeName = t->text;
        cv2->u.conv.operand  = in;
        return cv2;
    }
    if (t->kind == TK_IDENT) {
        take(p);
        /* Debug switch `EXTC_DBG_QN=1`: print where each identifier in
         * expression position starts and which four tokens follow it, so a
         * qualified name can be watched segment by segment.
         *
         * The cursor position and the token texts are both printed on purpose.
         * An earlier probe printed a bare string, calls from the prelude got
         * mixed into the trace, and the output was misread as belonging to the
         * call under investigation.  Every line has to identify exactly one
         * call.
         *
         * This writes to stderr only; token output such as --dump-tokens is
         * unaffected. */
        if (getenv("EXTC_DBG_QN")) {
            fprintf(stderr, "[qn ENT] pos=%d cur=`%s` n1=`%s` n2=`%s` n3=`%s` n4=`%s`\n",
                    (int)p->pos, t->text, pk(p,0)->text, pk(p,1)->text,
                    pk(p,2)->text, pk(p,3)->text);
        }
        /* `i32(x)` / `f64(y)`: an explicit conversion, narrowing, changing
         * signedness, or crossing between integer and float.  C spells it
         * `(T)x`, but that is ambiguous with a parenthesized expression here,
         * because the parser keeps no symbol table.  Moving the parentheses to
         * `T(x)` says the same thing unambiguously. */
        if (isScalarTypeName(t->text) && at(p, "(")) {
            take(p);                                  /* ( */
            skipNl(p);
            Expr *in = parseExpr(p);
            if (!in) return NULL;
            skipNl(p);
            if (!expect(p, ")", NULL)) return NULL;
            Expr *cv = exprNew(p->arena, EX_CONV, t->line);
            cv->u.conv.typeName = t->text;
            cv->u.conv.operand  = in;
            return cv;
        }
        /* `name { ... }` is a struct literal.
         *
         * The rule is positional, as in Go: in the condition of an `if` or a
         * `while` a `{` starts the body block, so a literal there needs
         * parentheses, `if x == (point { a: 1 }) { }`.  The type name therefore
         * does not have to be told apart by capitalization -- the rule lives in
         * the grammar instead of hiding in the spelling. */
        if (at(p, "{") && !p->inCond) return parseStructLit(p, t->text);

        /* A qualified type name: `mod::Type { ... }` and `mod::Type.variant`.
         *
         * This has to exist because two modules may each define a type of the
         * same name, and a bare name cannot say which one is meant: the loader
         * deliberately registers no return path for an ambiguous bare name, so
         * it never resolves.  Without this syntax the literal of such a type
         * could not be written at all.
         *
         * The path is only concatenated into one string and no symbol table is
         * consulted, exactly as in type position; the loader decides whether it
         * names a module or a type.  `mod::fn(...)` is untouched: when neither
         * `.` nor `{` follows, the code falls through to looksLikeAssoc. */
        if (at(p, "::")) {
            /* Look, do not consume.  `mod::fn(args)` is by far the common
             * case, and taking its tokens would break the looksLikeAssoc path
             * that would otherwise handle it: once `pos` moves, the prelude's
             * `pcg32::withStream(...)` came apart into `pcg32` plus garbage.
             * Take over only when a `{` or a `.` really does follow. */
            int k = 0;
            bool takeIt = false;
            for (;;) {
                if (strcmp(pk(p, k)->text, "::") != 0) break;
                Token *seg = pk(p, k + 1);
                if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
                k += 2;
                if ((strcmp(pk(p, k)->text, "{") == 0 && !p->inCond) ||
                    strcmp(pk(p, k)->text, ".") == 0) { takeIt = true; break; }
            }
            if (takeIt) {
                Buf b;
                bufInit(&b, p->arena);
                bufPuts(&b, t->text);
                for (int j = 0; j < k; j += 2) {
                    take(p);                                   /* `::` */
                    bufPuts(&b, "::");
                    bufPuts(&b, take(p)->text);                /* segment name */
                }
                if (at(p, "{")) return parseStructLit(p, bufCstr(&b));
                /* `mod::Type.variant` becomes a field access, and the loader
                 * rewrites the name; the checker's existing enum-variant path
                 * then handles it unchanged. */
                take(p);                                       /* `.` */
                Token *vn = expectIdent(p, "a variant name");
                if (!vn) return NULL;
                Expr *id = exprNew(p->arena, EX_IDENT, t->line);
                id->u.ident.name = bufCstr(&b);
                Expr *fa = exprNew(p->arena, EX_FIELD, t->line);
                fa->u.field.obj  = id;
                fa->u.field.name = vn->text;
                return fa;
            }
        }

        /* An associated call `option<i64>::some(x)` / `point::origin()`, or a
         * qualified name at any depth `std::sys::io::write(...)` /
         * `std::sys::io::STDOUT`.
         *
         * `IDENT <` is ambiguous with the less-than operator, so the code first
         * looks without consuming to see where the type arguments would end and
         * whether a `::` follows, and only then parses.  Deciding first keeps
         * speculative attempts from spraying bogus errors.  The test is `::`
         * itself: it is not a valid binary operator, so the two readings are
         * mutually exclusive and the guess cannot be wrong. */
        {
            const size_t assocPos = p->pos;      /* cursor is on the first `::` */
            if (looksLikeAssoc(p)) {
                const char *modPrefix = NULL;
                Expr *ae = parseAssoc(p, t->text, t->line, assocPos, &modPrefix);
                if (ae) ae->u.assoc.modPrefix = modPrefix;
                return ae;
            }
        }

        /* In a condition a `{` starts the block -- but `{ ident :` is plainly a
         * literal missing its parentheses, so the error hands the reader the
         * line to write instead. */
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

/* Decide whether a name is followed by an optional type argument list and
 * then `::name`, without moving the cursor.
 *
 * Returns:
 *   The number of segments after the first `::`, or 0 when this is not a
 *   qualified name.
 *
 * Notes:
 *   - This only looks; the cursor is left where it was.  `IDENT <` is ambiguous
 *     with the less-than operator, so the decision has to be made before
 *     parsing starts, or speculative attempts emit bogus errors.  The test is
 *     `::`, which is not a valid binary operator: an associated call and an
 *     `a < b` comparison cannot both fit, so the guess is never wrong.
 *   - The count is why this returns an int rather than a bool, and the caller
 *     needs it.  In `std::sys::io::STDOUT` the cursor sits on the FIRST `::`,
 *     so a caller that only asked "is this qualified?" would consume `::` plus
 *     one identifier (`sys`) and leave the cursor stranded on `::io::STDOUT`.
 *     How many segments follow is information only this scan has.
 *   - A two-segment name (`mod::fn`, `Type::assoc`) behaves exactly as before;
 *     that path was always correct and is unchanged.
 */
static int looksLikeAssoc(Parser *p) {
    size_t i = 0;

    if (strcmp(pk(p, i)->text, "<") == 0) {
        int depth = 0;
        for (;; i++) {
            Token *t = pk(p, i);
            if (t->kind == TK_EOF) return 0;
            if (strcmp(t->text, "<") == 0) {
                depth++;
            } else if (strcmp(t->text, ">") == 0) {
                if (--depth == 0) { i++; break; }
            } else if (t->kind == TK_IDENT || t->kind == TK_TYPE ||
                       t->kind == TK_INT) {
                /* a type name, a builtin type, or an array length */
            } else if (t->kind == TK_KEYWORD) {
                /* the contextual keywords `ref` and `mut` */
            } else if (strcmp(t->text, ",") == 0 || strcmp(t->text, "[") == 0 ||
                       strcmp(t->text, "]") == 0) {
            } else {
                return 0;           /* something a type cannot contain: not type args */
            }
        }
    }
    /* `Name<targs>::fn(...)` is an associated call; `name<T>(...)` is a generic
     * call (only the builtin primitives use it today, e.g. `alloc<i32>(n)`).
     * Both share the scanning above and are told apart by `::` versus `(`.
     * A generic call is only possible once type arguments have actually been
     * seen; otherwise an ordinary `f(x)` would be misread as one. */
    int segs = 0;
    while (strcmp(pk(p, i)->text, "::") == 0) {
        Token *seg = pk(p, i + 1);
        if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
        i += 2;
        segs++;
    }
    if (segs > 0) return segs;
    return (strcmp(pk(p, i)->text, "(") == 0 && i > 0) ? 1 : 0;
}

/* Count how many complete `::name` segments follow the cursor.
 *
 * Returns:
 *   The number of segments; 0 when the cursor is not on a `::`.
 *
 * Notes:
 *   - looksLikeAssoc cannot answer this: it stops at the question "is a `(`,
 *     `{`, or `.` next?", while parsing a path needs "how long is the path".
 *   - The cursor must be on a `::`.
 */
static int countFollowingSegs(Parser *p) {
    int k = 0;
    while (strcmp(pk(p, k)->text, "::") == 0) {
        Token *seg = pk(p, k + 1);
        if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
        k += 2;
    }
    return k / 2;
}

/* Parse a qualified name: `Name<targs>::name(args)`, `Type::assoc(args)`, or
 * the arbitrarily deep `mod::sub::name(args)` / `mod::sub::CONST`.
 *
 * Params:
 *   name         - the leading name, already consumed by the caller
 *   line         - line of that name, attached to the node
 *   startPos     - cursor position where the scan began; currently unused
 *   modPrefixOut - receives the module prefix, or NULL when the caller does not
 *                  need it
 *
 * Returns:
 *   An EX_ASSOC node, an EX_GENCALL node for `name<T>(...)`, or NULL after
 *   reporting an error.
 *
 * Notes:
 *   - Two segments and deep paths share this one path, decided by what follows
 *     the last segment: a `(` makes it a call (isCall = true); anything else is
 *     a value, such as a constant or a payload-free variant (isCall = false).
 *   - A value is also an EX_ASSOC and not an EX_IDENT.  The loader walks a known
 *     set of node kinds when resolving qualified names, so an EX_IDENT whose
 *     name contains `::` would be invisible to it and the checker would look up
 *     the bare name and report an undefined name.
 *   - `modPrefixOut` gets a provisional split, `std::sys::io` plus `STDOUT`.  The
 *     loader makes the final split, because only it knows which modules were
 *     imported; the value handed over is just the path cut at the last `::`.
 */
static Expr *parseAssoc(Parser *p, const char *name, int line, size_t startPos,
                        const char **modPrefixOut) {
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
    /* No `::`, so this is a generic call `name<T>(args)`, as in
     * `alloc<i32>(n)`. */
    if (!at(p, "::")) {
        Vec gargs;
        if (!parseArgs(p, &gargs)) return NULL;
        Expr *g = exprNew(p->arena, EX_GENCALL, line);
        g->u.gencall.name  = name;
        g->u.gencall.targs = targs;
        g->u.gencall.args  = gargs;
        return g;
    }

    /* Path: consume `::name` segments but leave the last one, which is the
     * symbol name.
     *
     * The last segment stays because what follows it decides the meaning: `(`
     * is a call, `{` a literal, `.` a variant, anything else a value.  That
     * token has to remain under the cursor for the caller to see.  With two
     * segments the loop below never runs at all (`fn` in `mod::fn` is not
     * followed by `::`), so the two-segment path is untouched.
     *
     * The test is `> 1`, not `>= 2`, because the last segment must survive.
     * `std::sys::io::STDOUT` splits into the prefix `std::sys::io` and the
     * symbol `STDOUT`: `::sys` and `::io` are consumed, `::STDOUT` is left.
     * With `>= 2` one segment too many is consumed, so the prefix becomes
     * `std::sys` and the symbol `io`, and the loader then reports
     * "`sys` is not imported" -- two steps away from the real mistake.
     *
     * The prefix has to be built while those tokens are consumed; pulling text
     * out of the token range afterwards cannot work, because the range starts
     * after the first `::` and the leading name (`std`) is not in it.  Doing it
     * that way yielded `sys::io` instead of `std::sys::io`. */
    const char *prefix = NULL;
    if (countFollowingSegs(p) > 1) {
        Buf pb;
        bufInit(&pb, p->arena);
        bufPuts(&pb, name);                          /* first segment, taken by caller */
        while (countFollowingSegs(p) > 1) {
            take(p);                                 /* `::` */
            bufPuts(&pb, "::");
            bufPuts(&pb, take(p)->text);             /* a middle segment */
        }
        prefix = bufCstr(&pb);
    }
    take(p);                                         /* `::` */
    Token *sym = expectIdent(p, "a function, constant or variant name");
    if (!sym) return NULL;

    /* One test decides it: a `(` after the LAST segment makes this a call.
     * Without one it is a value, such as a module constant -- C has no function
     * pointers and extC has no function values, so a qualified name without
     * parentheses can only be a constant.  The split goes to the loader. */
    const bool isCall = !targs.len && at(p, "(");
    if (modPrefixOut) *modPrefixOut = prefix;

    Vec args;
    vecInit(&args, p->arena, sizeof(void *));
    /* The argument list may be omitted: `maybe<i64>::nothing` is how a variant
     * without a payload is written.  One with a payload needs its parentheses:
     * `option<i64>::some(3)`. */
    if (at(p, "(") && !parseArgs(p, &args)) return NULL;

    Expr *e = exprNew(p->arena, EX_ASSOC, line);
    e->u.assoc.typeName = name;
    e->u.assoc.targs = targs;
    e->u.assoc.name = sym->text;
    e->u.assoc.args = args;
    e->u.assoc.isCall = isCall;
    (void)startPos;
    return e;
}

/* Parse a struct literal `{ field: value, ... }`.
 *
 * Params:
 *   name - the struct's name, or NULL for an anonymous `{ ... }` literal
 *
 * Returns:
 *   The new EX_STRUCTLIT node, or NULL after reporting an error.
 */
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
