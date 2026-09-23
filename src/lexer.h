/* extC syntax, part one of the pipeline: lexing.
 *
 * lexAll turns the source text into a flat token vector: numbers, identifiers,
 * keywords, builtin type names, string literals, and punctuation, with every
 * token's text copied into the context arena.  A newline is a token of its own,
 * because statements end at one; only the parser can say where it matters.
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

/* Report whether a spelling is one of the language's punctuation tokens.
 *
 * Params:
 *   value - the spelling to test, e.g. ")" or "=>"; NULL and "" are not punctuation
 *
 * Returns:
 *   true when `value` appears in PUNCTS.
 *
 * Notes:
 *   - The parser's at() has to ask this before comparing token text.  A string
 *     token stores its contents without the quotes, so the source text `")"`
 *     produces a token whose text compares equal to the punctuation `)`.  That
 *     made println(")") report "expected an expression, found `)`", while
 *     println("x)") was fine -- the only difference was that one byte.
 */
bool lexIsPunct(const char *value);

/* Tokenize ctx->src into `out` (a Vec of Token), ending with TK_EOF.
 *
 * Params:
 *   ctx - supplies the source text and the arena every token text is copied into
 *   out - vector to append to; the caller owns its initialization
 *
 * Notes:
 *   - A newline is a real token rather than whitespace: statements end at one,
 *     and only the parser can say where it is significant.  Runs of newlines
 *     collapse into a single TK_NEWLINE, and a trailing one is always appended.
 *   - On a lexical error the function reports through ctxError and returns;
 *     `out` then holds whatever was tokenized up to that point.
 */
void lexAll(Ctx *ctx, Vec *out);

/* Name a token kind for diagnostics and `--dump-tokens`, e.g. TK_IDENT -> "IDENT". */
const char *tokenKindName(TokenKind k);

/* Append one human-readable line describing `t` to `out`, used by --dump-tokens. */
void        tokenDescribe(const Token *t, Buf *out);

/* Is `s` a reserved word (fn, let, if, ...)?  Reserved words are TK_KEYWORD. */
bool isKeyword(const char *s);

/* Is `s` a builtin scalar type name (i32, f64, bool, void, ...)? */
bool isBuiltinType(const char *s);

#endif /* EXTC_LEXER_H */
