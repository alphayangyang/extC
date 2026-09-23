/* extC syntax, part one of the pipeline: the scanner.
 *
 * lexAll walks ctx->src once and appends one Token per number, identifier,
 * keyword, builtin type name, string literal, newline, or punctuation mark.
 * Every token text is copied into the context arena, so no token outlives the
 * run and the parser never touches the source buffer directly.  Nothing here
 * resolves names or checks grammar: that is the parser's job.
 */
#include "lexer.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================ tables */

static const char *KEYWORDS[] = {
    "fn", "let", "var", "if", "else", "while", "return", "match",
    "break", "continue", "struct", "type", "true", "false", "ref", "mut",
    "use",                            /* semantic import: `use std::io` */
    "extern",                         /* C interop: `extern!("libc")` + effect declaration */
    NULL
};

static const char *BUILTIN_TYPES[] = {
    "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64",
    "bool", "void",
    NULL
};

/* Longer symbols must come before the shorter ones they start with: lexPunct
 * takes the first table entry that matches, so `->` has to be reached before
 * `-`, or a function type would lex as a minus sign. */
static const char *PUNCTS[] = {
    "->", "==", "!=", "<=", ">=", "&&", "||", "...", "..", "::", "=>", "??",
    "+=", "-=", "*=", "/=",
    "|", "&", "^", "~", "@",
    "+", "-", "*", "/", "%", "=", "<", ">", "!", "?",
    "(", ")", "{", "}", "[", "]", ",", ":", ";", ".",
    NULL
};

/* Test whether the first `n` bytes of `s` spell one of the NUL-terminated
 * strings in `list`.
 *
 * Params:
 *   list - NULL-terminated table of spellings, e.g. KEYWORDS
 *   s    - bytes to test; need not be NUL-terminated
 *   n    - how many bytes of `s` are part of the candidate spelling
 *
 * Returns:
 *   true when some entry has length `n` and matches byte for byte.
 *
 * Notes:
 *   - The length test has to come first: the caller passes a slice of the
 *     source, so a NUL is not guaranteed at `s[n]`.
 */
static bool inList(const char **list, const char *s, size_t n) {
    for (size_t i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && memcmp(list[i], s, n) == 0) return true;
    }
    return false;
}

/* Report whether `s` is a reserved word. */
bool isKeyword(const char *s)     { return inList(KEYWORDS, s, strlen(s)); }

/* Report whether `s` is a builtin scalar type name. */
bool isBuiltinType(const char *s) { return inList(BUILTIN_TYPES, s, strlen(s)); }

/* ================================================================ character classes */

/* ASCII digit, and deliberately nothing else: the source language is ASCII. */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/* Hex digit, either case; used by the 0x integer path. */
static bool isHex(char c)   { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* Identifier start character: a letter or an underscore. */
static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* Identifier continuation character. */
static bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }

/* ================================================================ Lexer */

/* Scanning state: the source slice plus the cursor.
 *
 * `line` and `col` are 1-based, and lxAdvance is the only place that updates
 * them -- every token records the position it started at, for diagnostics.
 */
typedef struct {
    Ctx        *ctx;
    const char *src;
    size_t      len;
    size_t      pos;
    int         line, col;
} Lexer;

/* Return the character `k` positions ahead without consuming it.
 *
 * Params:
 *   k - lookahead distance in bytes; 0 is the current character
 *
 * Returns:
 *   The character, or '\0' past the end of the source.
 */
static char lxPeek(const Lexer *lx, size_t k) {
    size_t j = lx->pos + k;
    return j < lx->len ? lx->src[j] : '\0';
}

/* Consume one character, keeping the line and column counters in step.
 *
 * Returns:
 *   The character that was consumed.
 *
 * Notes:
 *   - Do not advance the cursor any other way.  `pos` and `line`/`col` have to
 *     move together or every later diagnostic points at the wrong place.
 */
static char lxAdvance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

/* Append a token built from `text[0..n)` to `out`.
 *
 * Params:
 *   out  - token vector to append to
 *   kind - token kind to record
 *   text - bytes of the token; they are copied, so the caller may reuse the buffer
 *   n    - token length; 0 is legal and yields an empty text
 *   line - 1-based line of the token's first character
 *   col  - 1-based column of the token's first character
 *
 * Returns:
 *   The freshly pushed token, so the caller can fill in ival or fval.
 *
 * Notes:
 *   - The copy is arena-owned and NUL-terminated; the length is kept separately
 *     because a string literal's contents may contain a NUL byte.
 */
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

/* Return the most recent token in `out`, or NULL when it is still empty. */
static Token *lxLast(Vec *out) {
    return out->len ? (Token *)vecAt(out, out->len - 1) : NULL;
}

/* ---------------------------------------------------------------- token kinds */

/* Lex one number into `out`: decimal or 0x hexadecimal integer, or a float with
 * a fractional part and/or an exponent.
 *
 * Params:
 *   lx  - cursor, positioned on the first digit
 *   out - token vector to append to
 *
 * Notes:
 *   - Numbers are always non-negative here.  A leading `-` is a separate token,
 *     consumed by parseUnary in the parser.
 *   - A `.` is part of the number only when a digit follows it (`1..2` is a
 *     range, and `1.foo` is a field access on 1).
 *   - The value is evaluated with accumulation rather than strtoll, because the
 *     text is not NUL-terminated until it has been copied.
 */
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

/* Lex one identifier, keyword, or builtin type name into `out`.
 *
 * Params:
 *   lx   - cursor, positioned on the first identifier character
 *   out  - token vector to append to
 *   line - line of the first character, already captured by the caller
 *   col  - column of the first character, already captured by the caller
 *
 * Notes:
 *   - Keyword and builtin type names are recorded with their own kinds
 *     (TK_KEYWORD, TK_TYPE); identifiers get TK_IDENT.
 */
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

/* Lex one string literal into `out`, storing the contents without the quotes.
 *
 * Params:
 *   lx   - cursor, positioned on the opening quote
 *   out  - token vector to append to
 *   line - line of the opening quote, already captured by the caller
 *   col  - column of the opening quote, already captured by the caller
 *
 * Notes:
 *   - A newline inside the literal is an error rather than part of the string,
 *     so a missing closing quote does not swallow the rest of the file.
 *   - Escape sequences are copied through verbatim for the C backend to emit;
 *     the parser and the type checker never look inside them.
 *   - The stored text is unquoted, which is why the parser must ask lexIsPunct
 *     before comparing token text with a punctuation spelling.
 */
static void lexString(Lexer *lx, Vec *out, int line, int col) {
    lxAdvance(lx);                              /* opening quote */
    size_t start = lx->pos;

    while (lx->pos < lx->len && lxPeek(lx, 0) != '"') {
        if (lxPeek(lx, 0) == '\n') {
            ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
            return;
        }
        if (lxPeek(lx, 0) == '\\') lxAdvance(lx);   /* escape: keep it for C */
        lxAdvance(lx);
    }
    if (lx->pos >= lx->len) {
        ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
        return;
    }

    size_t n = lx->pos - start;
    lxPushAt(lx, out, TK_STRING, lx->src + start, n, line, col);
    lxAdvance(lx);                              /* closing quote */
}

/* See lexer.h for the contract: a string literal's text can equal a punctuation
 * spelling, so the parser uses this to reject tokens that are not punctuation. */
bool lexIsPunct(const char *value) {
    if (!value || !*value) return false;
    for (size_t i = 0; PUNCTS[i]; i++)
        if (strcmp(PUNCTS[i], value) == 0) return true;
    return false;
}

/* Lex one punctuation token into `out` by first match in the table.
 *
 * Params:
 *   lx   - cursor, positioned on the first character of the symbol
 *   out  - token vector to append to
 *   line - line of that character, already captured by the caller
 *   col  - column of that character, already captured by the caller
 *
 * Returns:
 *   true when a symbol was matched and consumed; false after reporting an
 *   "unexpected character" error through ctxError.
 *
 * Notes:
 *   - The table is tried in order, so it has to list longer symbols first; the
 *     multi-character entries rely on that for longest-match behaviour.
 *   - `<<` and `>>` are deliberately absent from the table.  The parser
 *     recognizes them as two adjacent `<` / `>` tokens, which is what lets
 *     `box<box<i32>>` close its generic arguments.
 */
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

/* ---------------------------------------------------------------- main loop */

/* Scan the whole source and append its tokens to `out`.
 *
 * Params:
 *   ctx - compilation context: supplies ctx->src, the arena for token text, and
 *         the error slot (ctxError sets ctx->hasError, which the loop rechecks
 *         after the steps that can fail)
 *   out - token vector to append to, already initialized by the caller
 *
 * Notes:
 *   - A newline is significant, so it becomes a TK_NEWLINE token; consecutive
 *     newlines collapse into one, and a trailing one is always appended so that
 *     the parser never has to special-case the end of the file.
 *   - Line comments and block comments are skipped here and never reach the
 *     parser.
 *   - After a lexical error the scan stops early and the error is left on
 *     ctx->hasError; the caller renders it and does not parse the tokens.  The
 *     tokens produced so far stay in `out` and TK_EOF may be missing.
 */
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

    /* Guarantee a statement terminator at the end of the file. */
    Token *last = lxLast(out);
    if (!last || last->kind != TK_NEWLINE)
        lxPushAt(&lx, out, TK_NEWLINE, "\n", 1, lx.line, lx.col);
    lxPushAt(&lx, out, TK_EOF, "", 0, lx.line, lx.col);
}

/* ================================================================ display */

/* Return the spelling of a token kind as used by diagnostics and --dump-tokens. */
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

/* Append one --dump-tokens line for `t` to `out`.
 *
 * Notes:
 *   - The numeric value is printed according to the token kind, so a TK_INT
 *     shows ival rather than the source spelling; this is the token stream the
 *     parser actually sees.
 */
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
