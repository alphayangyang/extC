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
    const char *text;      /* arena 里的 NUL 结尾副本；字符串 token 存的是不含引号的内容 */
    size_t      len;
    long long   ival;
    double      fval;
    int         line, col;
} Token;

/* ⭐ 这个字符串是不是**标点**（`(` `)` `,` `=>` …）？
 *
 * 为什么 parser 需要问这个：`at(p, ")")` 原来是 `strcmp(cur(p)->text, ")")`，
 * 而**字符串字面量** token 的 `text` 存的是**不含引号的内容** ⇒
 * 写一个内容恰好是 `")"` 的字符串，`at(p, ")")` 就**成立**了 ✗✗
 * 实测症状：`println(")")` 报 `expected an expression, found \`)\``
 * 而 `println("x)")` 没事 —— 因为前者 text 恰好等于 `")"` ✓（真踩过）
 * ⇒ 判据不能只看文本，得先问"这个 token 可能是标点吗" ✓ */
bool lexIsPunct(const char *value);

/* 语句以换行结束（Go 风格），所以 NEWLINE 是一个真 token，
 * 由 parser 决定它在哪些位置有意义。 */
void lexAll(Ctx *ctx, Vec *out);

const char *tokenKindName(TokenKind k);
void        tokenDescribe(const Token *t, Buf *out);

bool isKeyword(const char *s);
bool isBuiltinType(const char *s);

#endif /* EXTC_LEXER_H */
