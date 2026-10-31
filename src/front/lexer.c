/**
 * @file lexer.c
 * @brief Implementation of the scanner declared in lexer.h.
 *
 * lexAll walks ctx->src once and appends one Token per number, identifier, keyword,
 * builtin type name, string literal, newline, or punctuation mark. Every token text is
 * copied into the context arena, so no token outlives the run and the parser never
 * touches the source buffer directly. Nothing here resolves names or checks grammar:
 * that is the parser's job.
 */
#include "lexer.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ tables */

static const char *KEYWORDS[] = {
    "fn", "let", "var", "if", "else", "while", "for", "return", "match",
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
    "+=", "-=", "*=", "/=", "%=",
    "|", "&", "^", "~", "@",
    "+", "-", "*", "/", "%", "=", "<", ">", "!", "?",
    "(", ")", "{", "}", "[", "]", ",", ":", ";", ".",
    NULL
};

/**
 * @brief Tell whether the first `n` bytes of `s` equal a spelling in `list` byte for byte.
 *
 * The length comparison must come first: `s` is a slice of the source text, so a NUL at
 * `s[n]` is not guaranteed.
 *
 * @param list  NUL-terminated spelling table, e.g. KEYWORDS.
 * @param s     Bytes to test; need not be NUL-terminated.
 * @param n     Byte count to compare.
 * @return      `true` when an entry of exactly `n` bytes matches byte for byte.
 */
static bool inList(const char **list, const char *s, size_t n) {
    for (size_t i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && memcmp(list[i], s, n) == 0) return true;
    }
    return false;
}

/**
 * @brief Tell whether `s` is a reserved word (`fn`, `let`, `if`, ...).
 *
 * @param s  Spelling to test.
 * @return   `true` when it is a reserved word (reserved words are TK_KEYWORD).
 */
bool isKeyword(const char *s)     { return inList(KEYWORDS, s, strlen(s)); }

/**
 * @brief Tell whether `s` is a builtin scalar type name (`i32`, `f64`, `bool`, `void`,
 *        ...).
 *
 * @param s  Spelling to test.
 * @return   `true` when it is a builtin type name.
 */
bool isBuiltinType(const char *s) { return inList(BUILTIN_TYPES, s, strlen(s)); }

/* ================================================================ character classes */

/**
 * @brief Tell whether `c` is an ASCII digit.
 *
 * Only ASCII counts; no other digit does, because the source language is ASCII.
 *
 * @param c  Character to test.
 * @return   `true` when it is in 0..9.
 */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/**
 * @brief Tell whether `c` is a hexadecimal digit, in either case.
 *
 * @param c  Character to test.
 * @return   `true` when it is a hexadecimal digit.
 */
static bool isHex(char c)   { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/**
 * @brief Tell whether `c` may start an identifier: a letter or an underscore.
 *
 * @param c  Character to test.
 * @return   `true` when it may start an identifier.
 */
static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/**
 * @brief Tell whether `c` may continue an identifier.
 *
 * @param c  Character to test.
 * @return   `true` when it is a letter, a digit, or an underscore.
 */
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

/**
 * @brief Return the `k`-th character ahead of the cursor without consuming it.
 *
 * @param lx  Scanner state.
 * @param k   Number of bytes to look ahead; 0 is the current character.
 * @return    That character; '\0' past the end of the source.
 */
static char lxPeek(const Lexer *lx, size_t k) {
    size_t j = lx->pos + k;
    return j < lx->len ? lx->src[j] : '\0';
}

/**
 * @brief Consume one character and advance the line and column counters with it.
 *
 * The cursor may move only through here: `pos` and `line`/`col` have to move together,
 * or every later diagnostic points at the wrong place.
 *
 * @param lx  Scanner state.
 * @return    The consumed character.
 */
static char lxAdvance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

/**
 * @brief Append a token built from `text[0..n)` to `out`.
 *
 * The text is copied into the context arena and NUL-terminated. The token's length is
 * kept separately because a literal's contents may contain a NUL byte, so the caller's
 * buffer may be reused.
 *
 * @param lx    Scanner state, which supplies the arena the token text is copied into.
 * @param out   Token vector to append to.
 * @param kind  Token kind to record.
 * @param text  Token bytes; they are copied, so the caller may reuse the buffer.
 * @param n     Token length; 0 is legal and yields empty text.
 * @param line  1-based line number of the token's first character.
 * @param col   1-based column number of the token's first character.
 * @return      The token just pushed, for the caller to fill in `ival` or `fval`.
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

/**
 * @brief Return the most recently pushed token in `out`.
 *
 * @param out  Token vector.
 * @return     The last token; NULL while the vector is still empty.
 */
static Token *lxLast(Vec *out) {
    return out->len ? (Token *)vecAt(out, out->len - 1) : NULL;
}

/* ---------------------------------------------------------------- token kinds */

/**
 * @brief Lex one number into `out`: a decimal or 0x hexadecimal integer, or a float with
 *        a fractional part and/or an exponent.
 *
 * Every number produced here is non-negative: a leading `-` is a separate token consumed
 * by the parser's parseUnary. A `.` is part of the number only when a digit follows it
 * (`1..2` is a range, `1.foo` reads a field of 1). The value is accumulated rather than
 * read with strtoll, because the text is NUL-terminated only after being copied.
 *
 * @param lx   Cursor, sitting on the first digit.
 * @param out  Token vector to append to.
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

        if (lx->pos == hexStart) {
            ctxError(lx->ctx, line, col, NULL, "hexadecimal literal needs at least one digit");
            return;
        }

        long long v = 0;
        for (size_t i = hexStart; i < lx->pos; i++) {
            char h = lx->src[i];
            long long d = isDigit(h) ? (h - '0') : ((h | 0x20) - 'a' + 10);
            if (v > (LLONG_MAX - d) / 16) {
                ctxError(lx->ctx, line, col, NULL,
                         "integer literal exceeds the supported signed 64-bit range");
                return;
            }
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
    if (isFloat) {
        t->fval = strtod(t->text, NULL);
    } else {
        errno = 0;
        t->ival = strtoll(t->text, NULL, 10);
        if (errno == ERANGE)
            ctxError(lx->ctx, line, col, NULL,
                     "integer literal exceeds the supported signed 64-bit range");
    }
}

/**
 * @brief Lex one identifier, keyword, or builtin type name into `out`.
 *
 * Keywords and builtin type names get their own kinds (TK_KEYWORD, TK_TYPE); every other
 * identifier is TK_IDENT.
 *
 * @param lx    Cursor, sitting on the first identifier character.
 * @param out   Token vector to append to.
 * @param line  Line number of the first character, taken by the caller beforehand.
 * @param col   Column number of the first character, taken by the caller beforehand.
 */
static void lexIdent(Lexer *lx, Vec *out, int line, int col) {
    size_t start = lx->pos;
    while (isAlnum(lxPeek(lx, 0))) lxAdvance(lx);
    size_t n = lx->pos - start;
    const char *text = lx->src + start;

    /* `__` is reserved for compiler-generated names (C reserves it for the implementation
     * too), so a program may not use it; the token is still pushed after the error, so the
     * user sees this one message instead of a following string of unknown-name errors. */
    if (n >= 2 && text[0] == '_' && text[1] == '_')
        ctxError(lx->ctx, line, col,
                 "The compiler gives its own generated names the `__` prefix (C reserves it for the"
                 " implementation too), so a program may not use it.",
                 "identifiers starting with `__` are reserved for the compiler");

    TokenKind kind = TK_IDENT;
    if (inList(KEYWORDS, text, n))           kind = TK_KEYWORD;
    else if (inList(BUILTIN_TYPES, text, n)) kind = TK_TYPE;

    lxPushAt(lx, out, kind, text, n, line, col);
}

/* `lexEscape` is defined below, and the escape table is shared with character literals,
 * so `lexStringTagged` needs this forward declaration. */
static long lexEscape(Lexer *lx, int line, int col, const char *what);

/**
 * @brief Scan one string literal; for an f-string it also has to watch quotes inside a
 *        slot.
 *
 * A plain `"..."` runs to the next `"`. An f-string (`f"..."`) is different: the
 * expression in a `{ ... }` slot may contain **string literals**, and in `f"c={\"hi\"}"`
 * the `"` belongs to the expression inside the slot, not to the end of the f-string. So
 * when `fstr` is set the scan first follows `{{` / `}}` and the brace depth; inside a slot
 * a `"` or `'` skips the whole literal (escapes included), and the slot's braces do not
 * take part in pairing. The parser then runs the real lexer and parser over the fragment;
 * the slot text itself can be kept verbatim.
 *
 * @param lx    Cursor, sitting on the `f` (when `fstr`) or on the opening quote.
 * @param out   Token vector to append to.
 * @param line  Line the opening quote is on.
 * @param col   Column the opening quote is on.
 * @param fstr  Whether this is an f-string.
 */
static void lexStringTagged(Lexer *lx, Vec *out, int line, int col, bool fstr) {
    lxAdvance(lx);                              /* the opening quote (`f` was already consumed by the caller) */
    size_t start = lx->pos;
    int depth = 0;                              /* brace depth of the slot; always 0 for a non-f-string */

    while (lx->pos < lx->len) {
        char c = lxPeek(lx, 0);
        if (c == '\n') {
            ctxError(lx->ctx, line, col, NULL, fstr ? "unterminated f-string literal"
                                                   : "unterminated string literal");
            return;
        }
        if (c == '\\') {
            /* The escape has to be validated through `lexEscape` (the only judge of
             * "which escapes exist"): passed through to the C backend unchanged, it is
             * rejected outright, e.g. `"\u64"` reports "incomplete universal character
             * name \u64". `lexEscape` has already consumed the escape and its arguments,
             * so this code must not advance again, or it eats one literal character too
             * many. */
            lxAdvance(lx);
            if (lexEscape(lx, line, col, "a string literal") < 0) return;
            continue;
        }
        if (fstr) {
            if (c == '{' && lxPeek(lx, 1) == '{') { lxAdvance(lx); lxAdvance(lx); continue; }
            if (c == '}' && lxPeek(lx, 1) == '}') { lxAdvance(lx); lxAdvance(lx); continue; }
            if (c == '{') { depth += 1; lxAdvance(lx); continue; }
            if (c == '}') { if (depth > 0) depth -= 1; lxAdvance(lx); continue; }
            if (depth > 0 && (c == '"' || c == '\'')) {
                /* a literal inside a slot: skip the whole thing; its quotes do not end the f-string */
                char q = c;
                lxAdvance(lx);
                while (lx->pos < lx->len) {
                    char d = lxPeek(lx, 0);
                    if (d == '\n') {
                        ctxError(lx->ctx, line, col, NULL, "unterminated f-string literal");
                        return;
                    }
                    if (d == '\\') { lxAdvance(lx); lxAdvance(lx); continue; }
                    lxAdvance(lx);
                    if (d == q) break;
                }
                continue;
            }
            if (c == '"' && depth == 0) break;
            lxAdvance(lx);
            continue;
        }
        if (c == '"') break;
        lxAdvance(lx);
    }
    if (lx->pos >= lx->len || (fstr && lxPeek(lx, 0) != '"')) {
        ctxError(lx->ctx, line, col, NULL, fstr ? "unterminated f-string literal"
                                               : "unterminated string literal");
        return;
    }

    size_t n = lx->pos - start;
    lxPushAt(lx, out, TK_STRING, lx->src + start, n, line, col);
    lxAdvance(lx);                              /* consume the closing quote along with the body */
}

/**
 * @brief Decode one escape sequence to its byte value; on error, report it and return -1.
 *
 * It sits next to `lexChar` so that "which escapes exist" and the error wording live in
 * one place. `\0` is the NUL byte; `\xHH` accepts one or two hexadecimal digits.
 *
 * @param lx    Cursor, sitting on the character to decode after the backslash.
 * @param line  Line the escape is on, for diagnostics.
 * @param col   Column the escape is on, for diagnostics.
 * @param what  Context name used in the error wording (e.g. `string literal`).
 * @return      The byte value the escape stands for; -1 after reporting an unknown escape
 *              or a hex escape that has no digits.
 */
static long lexEscape(Lexer *lx, int line, int col, const char *what) {
    int c = lxPeek(lx, 0);
    lxAdvance(lx);
    switch (c) {
    case 'n': return 10;
    case 't': return 9;
    case 'r': return 13;
    case '0': return 0;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'x': {
        long v = 0;
        int n = 0;
        for (; n < 2; n++) {
            int h = lxPeek(lx, 0);
            int d;
            if (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else break;
            v = v * 16 + d;
            lxAdvance(lx);
        }
        if (n == 0) {
            ctxError(lx->ctx, line, col, NULL, "`\\x` needs at least one hex digit");
            return -1;
        }
        return v;
    }
    default:
        ctxError(lx->ctx, line, col, NULL,
                 "unknown escape `\\%c` in %s", c, what);
        return -1;
    }
}

/**
 * @brief Lex one character literal: `'a'`, `'\n'`, `'\x41'`, `'\''`.
 *
 * What it produces is an ordinary **integer literal** carrying the byte value, so neither
 * the parser nor the type checker needs a new branch: `'a'` is `u8(97)` with the noise
 * removed, and the rule "no `char`; a type name carries its width" stays true. The scan
 * first looks for the closing quote on this line (skipping `\\`), which tells
 * "unterminated" apart from "more than one byte", and only then decodes the contents. The
 * token text is the source spelling, so `--dump-tokens` shows what was written; only
 * `ival` is meaningful to the parser.
 *
 * @param lx    Cursor, sitting on the opening quote.
 * @param out   Token vector to append to.
 * @param line  Line number of the opening quote, taken by the caller beforehand.
 * @param col   Column number of the opening quote, taken by the caller beforehand.
 */
static void lexChar(Lexer *lx, Vec *out, int line, int col) {
    size_t start = lx->pos;
    size_t p = start + 1;
    size_t close = 0;
    bool closed = false;
    while (p < lx->len && lx->src[p] != '\n') {
        if (lx->src[p] == '\\') { p += 2; continue; }
        if (lx->src[p] == '\'') { closed = true; close = p; break; }
        p++;
    }
    if (!closed) {
        ctxError(lx->ctx, line, col, NULL, "unterminated character literal");
        return;
    }
    if (close == start + 1) {
        ctxError(lx->ctx, line, col, NULL,
                 "empty character literal (write `'\\''` for the quote itself)");
        return;
    }
    long v;
    if (lx->src[start + 1] == '\\') {
        lx->pos = start + 2;                       /* skip the backslash; lexEscape decodes from just after it */
        v = lexEscape(lx, line, col, "a character literal");
        if (v < 0) return;
        if (lx->pos != close) {
            ctxError(lx->ctx, line, col, NULL,
                     "a character literal holds exactly one byte; use a string literal for"
                     " multi-byte text");
            return;
        }
    } else if (close == start + 2) {
        v = (long)(unsigned char)lx->src[start + 1];
    } else {
        ctxError(lx->ctx, line, col, NULL,
                 "a character literal holds exactly one byte; use a string literal for"
                 " multi-byte text");
        return;
    }
    lx->pos = close + 1;                           /* consume the closing quote along with the literal */
    Token *t = lxPushAt(lx, out, TK_INT, lx->src + start, close + 1 - start, line, col);
    t->ival = v;
}

/**
 * @brief Tell whether a spelling is one of the language's punctuation tokens.
 *
 * A string token stores its contents without the quotes (a string literal's text), so a
 * string token whose contents are a closing parenthesis has the same text as the
 * punctuation `)`. The parser must ask this before comparing token text, or that closing
 * parenthesis in `println` is taken as the end of the arguments, while a string whose
 * contents are `x)` is fine -- the difference is one byte.
 *
 * @param value  Spelling to test, e.g. the symbol string `)` or `=>`; NULL and the empty
 *               string are not punctuation.
 * @return       `true` when `value` appears in PUNCTS.
 */
bool lexIsPunct(const char *value) {
    if (!value || !*value) return false;
    for (size_t i = 0; PUNCTS[i]; i++)
        if (strcmp(PUNCTS[i], value) == 0) return true;
    return false;
}

/**
 * @brief Lex one punctuation mark into `out`, taking the first match in the table.
 *
 * The table is tried in order, so a longer symbol must come before the shorter symbol it
 * starts with, or a multi-character entry could never match longest. `<<` and `>>` are
 * deliberately absent: the parser reads them as two adjacent `<` / `>` tokens, which is
 * what lets `box<box<i32>>` close its generic arguments.
 *
 * @param lx    Cursor, sitting on the symbol's first character.
 * @param out   Token vector to append to.
 * @param line  Line number of that character, taken by the caller beforehand.
 * @param col   Column number of that character, taken by the caller beforehand.
 * @return      `true` when a symbol matched and was consumed; otherwise `false` after
 *              reporting "unexpected character" through `ctxError`.
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

/**
 * @brief Scan one stretch of source, appending tokens to `out` and ending with TK_EOF.
 *
 * An f-string (an `f` immediately followed by a quote) uses it to scan the fragments
 * inside its braces: positions start at the given line and column, so a diagnostic in a
 * fragment points back into the file instead of at offset 0 of the fragment. A newline is
 * a real token rather than whitespace: statements end at a newline, and only the parser
 * can say where that matters; a run of newlines collapses into one TK_NEWLINE, and the end
 * of the stream always has one, so the parser needs no end-of-file special case. Line and
 * block comments are skipped here and never reach the parser. A lexical error is reported
 * through `ctxError` and the scan returns early, leaving in `out` the tokens scanned before
 * the error; TK_EOF may be missing.
 *
 * @param ctx    Compilation context: supplies the source text and the arena every token
 *               text is copied into; errors go through `ctxError`, which sets
 *               `ctx->hasError`.
 * @param src    First byte to scan.
 * @param len    Number of bytes to scan.
 * @param line0  Starting line number (1-based).
 * @param col0   Starting column number (1-based).
 * @param out    Vector to append to; the caller initializes it.
 */
void lexRange(Ctx *ctx, const char *src, size_t len, int line0, int col0, Vec *out) {
    Lexer lx = { ctx, src, len, 0, line0, col0 };

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
        /* `f"..."`: the identifier is exactly `f` and **immediately** precedes a quote,
         * which makes it an f-string. `f` is still a legal identifier on its own
         * (`f "x"`, with a space, is two tokens). */
        if (isAlpha(c)) {
            if (c == 'f' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '"') {
                lxAdvance(&lx);
                lexStringTagged(&lx, out, line, col, true);
                if (ctx->hasError) return;
                Token *ft = lxLast(out);
                if (ft && ft->kind == TK_STRING) ft->kind = TK_FSTRING;
                continue;
            }
            lexIdent(&lx, out, line, col);
            continue;
        }
        if (c == '"')                        { lexStringTagged(&lx, out, line, col, false); if (ctx->hasError) return; continue; }
        if (c == '\'')                      { lexChar(&lx, out, line, col); if (ctx->hasError) return; continue; }
        if (!lexPunct(&lx, out, line, col))  { return; }
    }

    /* A statement terminator is always appended at end of file, so the parser needs no
     * end-of-file special case. */
    Token *last = lxLast(out);
    if (!last || last->kind != TK_NEWLINE)
        lxPushAt(&lx, out, TK_NEWLINE, "\n", 1, lx.line, lx.col);
    lxPushAt(&lx, out, TK_EOF, "", 0, lx.line, lx.col);
}

/**
 * @brief Lex all of `ctx->src`, appending tokens to `out`.
 *
 * A thin shell over `lexRange`: `f"..."` needs to scan substrings, so the scanner that
 * does the work takes a range; the whole file is just the case that starts at line 1,
 * column 1.
 *
 * @param ctx  Supplies the source text and the arena every token text is copied into.
 * @param out  Vector to append to; the caller initializes it.
 */
void lexAll(Ctx *ctx, Vec *out) {
    lexRange(ctx, ctx->src, ctx->srcLen, 1, 1, out);
}

/* ================================================================ display */

/**
 * @brief Spelling of a token kind, for diagnostics and `--dump-tokens`, e.g.
 *        TK_IDENT -> `IDENT`.
 *
 * @param k  Token kind.
 * @return   Its name; `?` for an unknown kind.
 */
const char *tokenKindName(TokenKind k) {
    switch (k) {
        case TK_EOF:     return "EOF";
        case TK_NEWLINE: return "NEWLINE";
        case TK_IDENT:   return "IDENT";
        case TK_INT:     return "INT";
        case TK_FLOAT:   return "FLOAT";
        case TK_STRING:  return "STRING";        case TK_FSTRING: return "FSTRING";

        case TK_KEYWORD: return "KEYWORD";
        case TK_TYPE:    return "TYPE";
        case TK_PUNCT:   return "PUNCT";
    }
    return "?";
}

/**
 * @brief Append one readable line describing `t` to `out`, for `--dump-tokens`.
 *
 * Values are printed by token kind (TK_INT prints `ival` rather than the source
 * spelling), so the output is the token stream the parser sees.
 *
 * @param t    Token to describe.
 * @param out  Buffer that receives the text.
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
