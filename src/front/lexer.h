/**
 * @file lexer.h
 * @brief extC 语法流水线的第一段：词法。
 *
 * `lexAll` 把源文本变成扁平的 token 向量：数字、标识符、关键字、内建类型名、字符串字面量
 * 与标点，每个 token 的文本都拷进上下文 arena。换行自成一种 token，因为语句在换行处结束，
 * 只有解析器能说它在哪里算数。
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
    /* `f"…"`：**编译期格式拼接**（见 docs/topics/…）。词法上它就是一个字符串，
     * 区别只在解析器会把它展开成 `<<` 链 —— 所以没有运行时成本。 */
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
 * @brief 判断一个拼法是不是语言的标点 token 之一。
 *
 * 解析器的 `at()` 在比较 token 文本之前必须先问它：字符串 token 存的是去掉引号后的内容，
 * 所以源码里的 `")"` 产出的 token，其文本与标点 `)` 相等。缺了这一问，`println(")")` 会报
 * "expected an expression, found `)`"，而 `println("x)")` 没事 —— 差别只是一个字节。
 *
 * @param value  要测的拼法，例如 `")"` 或 `"=>"`；NULL 与 `""` 不是标点。
 * @return       `value` 出现在 PUNCTS 里则 `true`。
 */
bool lexIsPunct(const char *value);

/**
 * @brief 把 `ctx->src` 切成 token 追加到 `out`（元素是 `Token`），末尾是 TK_EOF。
 *
 * 换行是真的 token 而不是空白：语句在换行处结束，只有解析器能说它在哪里有意义。连续的换行
 * 折成一个 TK_NEWLINE，末尾总有一个。行注释与块注释在这里跳过，从不进解析器。词法出错时经
 * `ctxError` 报告并返回，`out` 里留的是出错前切出的 token。
 *
 * @param ctx  提供源文本，以及每个 token 文本都拷进去的 arena。
 * @param out  追加到的向量；初始化由调用方负责。
 */
void lexAll(Ctx *ctx, Vec *out);

/**
 * @brief 同一个扫描器，扫**子区间**，从给定的行/列开始。
 *
 * `f"…"` 需要它：花括号里的表达式是拿真解析器跑片段解析的，位置必须与文件对得上（槽位里的
 * 诊断要指向文件，而不是片段的偏移 0）。
 *
 * @param ctx   提供 arena 与错误报告。
 * @param src   子区间的起点。
 * @param len   子区间的字节数。
 * @param line  起点行号（1 起）。
 * @param col   起点列号（1 起）。
 * @param out   追加到的向量。
 */
void lexRange(Ctx *ctx, const char *src, size_t len, int line, int col, Vec *out);

/**
 * @brief 给出 token 种类的拼法，供诊断与 `--dump-tokens` 用，例如 TK_IDENT -> `"IDENT"`。
 *
 * @param k  token 种类。
 * @return   它的名字；未知种类返回 `"?"`。
 */
const char *tokenKindName(TokenKind k);

/**
 * @brief 追加一行描述 `t` 的可读文本到 `out`，供 `--dump-tokens` 用。
 *
 * 数值按 token 种类打印（TK_INT 打 `ival` 而不是源码拼法），打出来的就是解析器看到的 token 流。
 *
 * @param t    要描述的 token。
 * @param out  接收文本的缓冲区。
 */
void tokenDescribe(const Token *t, Buf *out);

/**
 * @brief 判断 `s` 是不是保留字（`fn`、`let`、`if` …）。
 *
 * @param s  要测的拼法。
 * @return   是保留字则 `true`（保留字是 TK_KEYWORD）。
 */
bool isKeyword(const char *s);

/**
 * @brief 判断 `s` 是不是内建标量类型名（`i32`、`f64`、`bool`、`void` …）。
 *
 * @param s  要测的拼法。
 * @return   是内建类型名则 `true`。
 */
bool isBuiltinType(const char *s);

#endif /* EXTC_LEXER_H */
