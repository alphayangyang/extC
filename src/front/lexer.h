/**
 * @file lexer.h
 * @brief First stage of the extC pipeline: lexical analysis.
 *
 * `lexAll` turns source text into a flat token vector: numbers, identifiers, keywords,
 * builtin type names, string literals, and punctuation, with the text of every token
 * copied into the context arena. A newline is a token of its own, because a statement
 * ends at a newline and only the parser can say where that counts.
 */
#ifndef EXTC_LEXER_H
#define EXTC_LEXER_H

#include "base.h"

typedef enum {
    TK_EOF = 0,
    TK_NEWLINE,
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_STRING,
    /* `f"..."`: **compile-time format interpolation**. Lexically it is a string like
     * any other; the difference is that the parser expands it into a `<<` chain, so it
     * costs nothing at run time. */
    TK_FSTRING,
    TK_KEYWORD,
    TK_TYPE,
    TK_PUNCT
} TokenKind;

typedef struct {
    TokenKind   kind;
    const char *text;      /* arena-owned NUL-terminated copy; a string token keeps
                            * the contents without the surrounding quotes */
    size_t      len;
    long long   ival;
    double      fval;
    int         line, col;
} Token;

/**
 * @brief Tell whether a spelling is one of the language's punctuation tokens.
 *
 * The parser's `at()` must ask this before comparing token text: a string token stores
 * its contents without the quotes, so a `")"` in the source produces a token whose text
 * equals the punctuation `)`. Without this test, `println(")")` reports "expected an
 * expression, found `)`" while `println("x)")` is fine -- the difference is one byte.
 *
 * @param value  Spelling to test, e.g. `")"` or `"=>"`; NULL and `""` are not punctuation.
 * @return       `true` when `value` appears in PUNCTS.
 */
bool lexIsPunct(const char *value);

/**
 * @brief Scan `ctx->src` into tokens appended to `out` (elements are `Token`), ending
 *        with TK_EOF.
 *
 * A newline is a real token rather than whitespace: statements end at a newline, and only
 * the parser can say where that matters. A run of newlines collapses into one TK_NEWLINE,
 * and the stream always ends with one. Line and block comments are skipped here and never
 * reach the parser. A lexical error is reported through `ctxError` and the scan returns,
 * leaving in `out` the tokens scanned before the error.
 *
 * @param ctx  Supplies the source text and the arena every token text is copied into.
 * @param out  Vector to append to; the caller initializes it.
 */
void lexAll(Ctx *ctx, Vec *out);

/**
 * @brief The same scanner over a **sub-range**, starting at the given line and column.
 *
 * `f"..."` needs it: the expression inside a brace slot is parsed as a fragment with the
 * real parser, and its positions must line up with the file (a diagnostic in the slot has
 * to point into the file, not at offset 0 of the fragment).
 *
 * @param ctx    Supplies the arena and the error channel.
 * @param src    Start of the sub-range.
 * @param len    Byte count of the sub-range.
 * @param line   Starting line number (1-based).
 * @param col    Starting column number (1-based).
 * @param out    Vector to append to.
 */
void lexRange(Ctx *ctx, const char *src, size_t len, int line, int col, Vec *out);

/**
 * @brief Spelling of a token kind, for diagnostics and `--dump-tokens`, e.g.
 *        TK_IDENT -> `"IDENT"`.
 *
 * @param k  Token kind.
 * @return   Its name; `"?"` for an unknown kind.
 */
const char *tokenKindName(TokenKind k);

/**
 * @brief Append one readable line describing `t` to `out`, for `--dump-tokens`.
 *
 * Values are printed by token kind (TK_INT prints `ival` rather than the source
 * spelling), so the output is the token stream the parser sees.
 *
 * @param t    Token to describe.
 * @param out  Buffer that receives the text.
 */
void tokenDescribe(const Token *t, Buf *out);

/**
 * @brief Tell whether `s` is a reserved word (`fn`, `let`, `if`, ...).
 *
 * @param s  Spelling to test.
 * @return   `true` when it is a reserved word (reserved words are TK_KEYWORD).
 */
bool isKeyword(const char *s);

/**
 * @brief Tell whether `s` is a builtin scalar type name (`i32`, `f64`, `bool`, `void`,
 *        ...).
 *
 * @param s  Spelling to test.
 * @return   `true` when it is a builtin type name.
 */
bool isBuiltinType(const char *s);

#endif /* EXTC_LEXER_H */
