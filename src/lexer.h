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

/* 语句以换行结束（Go 风格），所以 NEWLINE 是一个真 token，
 * 由 parser 决定它在哪些位置有意义。 */
void lexAll(Ctx *ctx, Vec *out);

const char *tokenKindName(TokenKind k);
void        tokenDescribe(const Token *t, Buf *out);

bool isKeyword(const char *s);
bool isBuiltinType(const char *s);
bool isUpperCase(const char *s);

#endif /* EXTC_LEXER_H */
