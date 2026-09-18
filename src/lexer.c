#include "lexer.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================ 字表 */

static const char *KEYWORDS[] = {
    "fn", "let", "var", "if", "else", "while", "return",
    "break", "continue", "struct", "type", "true", "false", "ref",
    NULL
};

static const char *BUILTIN_TYPES[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool", "void",
    NULL
};

/* 长符号必须排在短符号前面 —— 靠最长匹配消歧义（`->` 不能先撞上 `-`） */
static const char *PUNCTS[] = {
    "->", "==", "!=", "<=", ">=", "&&", "||", "...", "..", "::",
    "+=", "-=", "*=", "/=",
    "|", "&", "^", "~",
    "+", "-", "*", "/", "%", "=", "<", ">", "!",
    "(", ")", "{", "}", "[", "]", ",", ":", ";", ".",
    NULL
};

static bool inList(const char **list, const char *s, size_t n) {
    for (size_t i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && memcmp(list[i], s, n) == 0) return true;
    }
    return false;
}

bool isKeyword(const char *s)     { return inList(KEYWORDS, s, strlen(s)); }
bool isBuiltinType(const char *s) { return inList(BUILTIN_TYPES, s, strlen(s)); }

/* ================================================================ 字符类 */

static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static bool isHex(char c)   { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }

/* ================================================================ Lexer */

typedef struct {
    Ctx        *ctx;
    const char *src;
    size_t      len;
    size_t      pos;
    int         line, col;
} Lexer;

static char lxPeek(const Lexer *lx, size_t k) {
    size_t j = lx->pos + k;
    return j < lx->len ? lx->src[j] : '\0';
}

static char lxAdvance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

static Token *lxPushAt(Lexer *lx, Vec *out, TokenKind kind,
                       const char *text, size_t n, int line, int col) {
    Token *t = (Token *)vecPush(out);
    t->kind = kind;
    t->text = arenaStrndup(lx->ctx->arena, text, n);
    t->len  = n;
    t->ival = 0;
    t->fval = 0.0;
    t->line = line;
    t->col  = col;
    return t;
}

static Token *lxLast(Vec *out) {
    return out->len ? (Token *)vecAt(out, out->len - 1) : NULL;
}

/* ---------------------------------------------------------------- 各类 token */

static void lexNumber(Lexer *lx, Vec *out) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    bool isFloat = false;

    if (lxPeek(lx, 0) == '0' && (lxPeek(lx, 1) == 'x' || lxPeek(lx, 1) == 'X')) {
        lxAdvance(lx);
        lxAdvance(lx);
        size_t hexStart = lx->pos;
        while (isHex(lxPeek(lx, 0))) lxAdvance(lx);

        long long v = 0;
        for (size_t i = hexStart; i < lx->pos; i++) {
            char h = lx->src[i];
            long long d = isDigit(h) ? (h - '0') : ((h | 0x20) - 'a' + 10);
            v = v * 16 + d;
        }
        Token *t = lxPushAt(lx, out, TK_INT, lx->src + start, lx->pos - start, line, col);
        t->ival = v;
        return;
    }

    while (isDigit(lxPeek(lx, 0))) lxAdvance(lx);

    if (lxPeek(lx, 0) == '.' && isDigit(lxPeek(lx, 1))) {
        isFloat = true;
        lxAdvance(lx);
        while (isDigit(lxPeek(lx, 0))) lxAdvance(lx);
    }

    if (lxPeek(lx, 0) == 'e' || lxPeek(lx, 0) == 'E') {
        size_t k = (lxPeek(lx, 1) == '+' || lxPeek(lx, 1) == '-') ? 2 : 1;
        if (isDigit(lxPeek(lx, k))) {
            isFloat = true;
            for (size_t i = 0; i < k; i++) lxAdvance(lx);
            while (isDigit(lxPeek(lx, 0))) lxAdvance(lx);
        }
    }

    Token *t = lxPushAt(lx, out, isFloat ? TK_FLOAT : TK_INT,
                        lx->src + start, lx->pos - start, line, col);
    if (isFloat) t->fval = strtod(t->text, NULL);
    else         t->ival = strtoll(t->text, NULL, 10);
}

static void lexIdent(Lexer *lx, Vec *out, int line, int col) {
    size_t start = lx->pos;
    while (isAlnum(lxPeek(lx, 0))) lxAdvance(lx);
    size_t n = lx->pos - start;
    const char *text = lx->src + start;

    TokenKind kind = TK_IDENT;
    if (inList(KEYWORDS, text, n))           kind = TK_KEYWORD;
    else if (inList(BUILTIN_TYPES, text, n)) kind = TK_TYPE;

    lxPushAt(lx, out, kind, text, n, line, col);
}

static void lexString(Lexer *lx, Vec *out, int line, int col) {
    lxAdvance(lx);                              /* 开引号 */
    size_t start = lx->pos;

    while (lx->pos < lx->len && lxPeek(lx, 0) != '"') {
        if (lxPeek(lx, 0) == '\n') {
            ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
            return;
        }
        if (lxPeek(lx, 0) == '\\') lxAdvance(lx);   /* 转义：原样留着给 C */
        lxAdvance(lx);
    }
    if (lx->pos >= lx->len) {
        ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
        return;
    }

    size_t n = lx->pos - start;
    lxPushAt(lx, out, TK_STRING, lx->src + start, n, line, col);
    lxAdvance(lx);                              /* 闭引号 */
}

static bool lexPunct(Lexer *lx, Vec *out, int line, int col) {
    for (size_t i = 0; PUNCTS[i]; i++) {
        size_t n = strlen(PUNCTS[i]);
        if (lx->pos + n <= lx->len && memcmp(lx->src + lx->pos, PUNCTS[i], n) == 0) {
            for (size_t k = 0; k < n; k++) lxAdvance(lx);
            lxPushAt(lx, out, TK_PUNCT, PUNCTS[i], n, line, col);
            return true;
        }
    }
    ctxError(lx->ctx, line, col, NULL, "unexpected character `%c`", lxPeek(lx, 0));
    return false;
}

/* ---------------------------------------------------------------- 主循环 */

void lexAll(Ctx *ctx, Vec *out) {
    Lexer lx = { ctx, ctx->src, ctx->srcLen, 0, 1, 1 };

    while (lx.pos < lx.len) {
        char c = lxPeek(&lx, 0);
        int line = lx.line, col = lx.col;

        if (c == ' ' || c == '\t' || c == '\r') {
            lxAdvance(&lx);
            continue;
        }

        if (c == '\n') {
            lxAdvance(&lx);
            Token *last = lxLast(out);
            if (!last || last->kind != TK_NEWLINE)
                lxPushAt(&lx, out, TK_NEWLINE, "\n", 1, line, col);
            continue;
        }

        if (c == '/' && lxPeek(&lx, 1) == '/') {
            while (lx.pos < lx.len && lxPeek(&lx, 0) != '\n') lxAdvance(&lx);
            continue;
        }

        if (c == '/' && lxPeek(&lx, 1) == '*') {
            lxAdvance(&lx);
            lxAdvance(&lx);
            while (lx.pos < lx.len && !(lxPeek(&lx, 0) == '*' && lxPeek(&lx, 1) == '/'))
                lxAdvance(&lx);
            if (lx.pos >= lx.len) {
                ctxError(ctx, line, col, NULL, "unterminated block comment");
                return;
            }
            lxAdvance(&lx);
            lxAdvance(&lx);
            continue;
        }

        if (isDigit(c))                      { lexNumber(&lx, out); if (ctx->hasError) return; continue; }
        if (isAlpha(c))                      { lexIdent(&lx, out, line, col); continue; }
        if (c == '"')                        { lexString(&lx, out, line, col); if (ctx->hasError) return; continue; }
        if (!lexPunct(&lx, out, line, col))  { return; }
    }

    /* 保证文件末尾一定有一个语句终止符 */
    Token *last = lxLast(out);
    if (!last || last->kind != TK_NEWLINE)
        lxPushAt(&lx, out, TK_NEWLINE, "\n", 1, lx.line, lx.col);
    lxPushAt(&lx, out, TK_EOF, "", 0, lx.line, lx.col);
}

/* ================================================================ 展示 */

const char *tokenKindName(TokenKind k) {
    switch (k) {
        case TK_EOF:     return "EOF";
        case TK_NEWLINE: return "NEWLINE";
        case TK_IDENT:   return "IDENT";
        case TK_INT:     return "INT";
        case TK_FLOAT:   return "FLOAT";
        case TK_STRING:  return "STRING";
        case TK_KEYWORD: return "KEYWORD";
        case TK_TYPE:    return "TYPE";
        case TK_PUNCT:   return "PUNCT";
    }
    return "?";
}

void tokenDescribe(const Token *t, Buf *out) {
    bufPrintf(out, "%4d:%-3d %-8s ", t->line, t->col, tokenKindName(t->kind));
    switch (t->kind) {
        case TK_INT:     bufPrintf(out, "%lld", t->ival); break;
        case TK_FLOAT:   bufPrintf(out, "%g", t->fval); break;
        case TK_NEWLINE: bufPuts(out, "\\n"); break;
        case TK_EOF:     bufPuts(out, "<eof>"); break;
        case TK_STRING:  bufPrintf(out, "\"%s\"", t->text); break;
        default:         bufPuts(out, t->text); break;
    }
}
