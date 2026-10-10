/* extC syntax, part one of the pipeline: the scanner.
 *
 * lexAll walks ctx->src once and appends one Token per number, identifier,
 * keyword, builtin type name, string literal, newline, or punctuation mark.
 * Every token text is copied into the context arena, so no token outlives the
 * run and the parser never touches the source buffer directly.  Nothing here
 * resolves names or checks grammar: that is the parser's job.
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
 * @brief 判断 `s` 的前 `n` 个字节是否逐字节等于 `list` 中的某个拼法。
 *
 * 长度比较必须在前：`s` 是源文本的一段切片，`s[n]` 处不保证有 NUL。
 *
 * @param list  NUL 结尾的拼法表，例如 KEYWORDS。
 * @param s     要测的字节；不要求 NUL 结尾。
 * @param n     参与比较的字节数。
 * @return      存在某个长度恰为 `n` 且逐字节相同的条目时返回 true。
 */
static bool inList(const char **list, const char *s, size_t n) {
    for (size_t i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && memcmp(list[i], s, n) == 0) return true;
    }
    return false;
}

/**
 * @brief 判断 `s` 是不是保留字（`fn`、`let`、`if` …）。
 *
 * @param s  要测的拼法。
 * @return   是保留字则返回 true（保留字记作 TK_KEYWORD）。
 */
bool isKeyword(const char *s)     { return inList(KEYWORDS, s, strlen(s)); }

/**
 * @brief 判断 `s` 是不是内建标量类型名（`i32`、`f64`、`bool`、`void` …）。
 *
 * @param s  要测的拼法。
 * @return   是内建类型名则返回 true。
 */
bool isBuiltinType(const char *s) { return inList(BUILTIN_TYPES, s, strlen(s)); }

/* ================================================================ character classes */

/**
 * @brief 判断 `c` 是不是 ASCII 数字。
 *
 * 只认 ASCII，别的数字一律不算：源语言就是 ASCII。
 *
 * @param c  要测的字符。
 * @return   处于 0 到 9 则返回 true。
 */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/**
 * @brief 判断 `c` 是不是十六进制数字，大小写均可。
 *
 * @param c  要测的字符。
 * @return   是十六进制数字则返回 true。
 */
static bool isHex(char c)   { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/**
 * @brief 判断 `c` 是不是标识符起始字符：字母或下划线。
 *
 * @param c  要测的字符。
 * @return   是标识符起始字符则返回 true。
 */
static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/**
 * @brief 判断 `c` 是不是标识符后续字符。
 *
 * @param c  要测的字符。
 * @return   是字母、数字或下划线则返回 true。
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
 * @brief 返回游标前方第 `k` 个字符，不消费它。
 *
 * @param lx  词法状态。
 * @param k   向前看的字节数；0 是当前字符。
 * @return    该字符；越过源末尾时返回 '\0'。
 */
static char lxPeek(const Lexer *lx, size_t k) {
    size_t j = lx->pos + k;
    return j < lx->len ? lx->src[j] : '\0';
}

/**
 * @brief 消费一个字符，并让行、列计数同步前进。
 *
 * 游标只能经这里移动：`pos` 与 `line`/`col` 必须一起走，否则之后每条诊断都指向错的位置。
 *
 * @param lx  词法状态。
 * @return    被消费的字符。
 */
static char lxAdvance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

/**
 * @brief 追加一个由 `text[0..n)` 构成的 token 到 `out`。
 *
 * 文本拷进上下文 arena 并补 NUL。The token's length is kept separately because a literal's
 * contents may contain a NUL byte, so the caller's buffer may be reused.
 *
 * @param lx    词法状态，提供 token 文本拷进去的 arena。
 * @param out   追加到的 token 向量。
 * @param kind  要记录的 token 种类。
 * @param text  token 的字节；会被拷贝，调用方可以复用缓冲区。
 * @param n     token 长度；0 合法，得到空文本。
 * @param line  token 首字符的 1 起行号。
 * @param col   token 首字符的 1 起列号。
 * @return      刚压入的 token，供调用方填 ival 或 fval。
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
 * @brief 返回 `out` 中最近压入的 token。
 *
 * @param out  token 向量。
 * @return     最后一个 token；向量仍为空时返回 NULL。
 */
static Token *lxLast(Vec *out) {
    return out->len ? (Token *)vecAt(out, out->len - 1) : NULL;
}

/* ---------------------------------------------------------------- token kinds */

/**
 * @brief 把一个数字词法化进 `out`：十进制或 0x 十六进制整数，或带小数部分与/或指数部分的浮点数。
 *
 * 这里产出的数一律非负：前导 `-` 是独立 token，由解析器的 parseUnary 消费。
 * `.` 只有后面跟着数字时才是数字的一部分（`1..2` 是区间，`1.foo` 是对 1 取字段）。
 * 值用累加求，而不是 strtoll：文本要拷过之后才 NUL 结尾。
 *
 * @param lx   游标，停在第一个数字上。
 * @param out  追加到的 token 向量。
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
 * @brief 把一个标识符、关键字或内建类型名词法化进 `out`。
 *
 * 关键字与内建类型名记成各自的种类（TK_KEYWORD、TK_TYPE），其余标识符记成 TK_IDENT。
 *
 * @param lx    游标，停在第一个标识符字符上。
 * @param out   追加到的 token 向量。
 * @param line  首字符的行号，由调用方先取好。
 * @param col   首字符的列号，由调用方先取好。
 */
static void lexIdent(Lexer *lx, Vec *out, int line, int col) {
    size_t start = lx->pos;
    while (isAlnum(lxPeek(lx, 0))) lxAdvance(lx);
    size_t n = lx->pos - start;
    const char *text = lx->src + start;

    /* `__` 留给编译器生成的名字（C 也把它留给实现），所以程序不得使用；报错后仍压入 token，
     * 让用户只看到这一条，而不是随后一串找不到名字的连带错误。 */
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

/* `lexEscape` 定义在下面，而转义表与字符字面量共用，所以 `lexString` 需要这个前置原型。 */
static long lexEscape(Lexer *lx, int line, int col, const char *what);

/**
 * @brief 把一个字符串字面量词法化进 `out`，存下的内容不含两侧引号。
 *
 * 字面量里出现换行即报错，而不是并入字符串：缺右引号不会吞掉文件余下部分。
 * 转义序列由 `lexEscape` 校验后原样拷给 C 后端，解析器与类型检查器不看其内容。
 * 存下的文本去掉了引号，所以解析器拿 token 文本比较标点拼法之前必须先问 `lexIsPunct`。
 *
 * @param lx    游标，停在起始引号上。
 * @param out   追加到的 token 向量。
 * @param line  起始引号的行号，由调用方先取好。
 * @param col   起始引号的列号，由调用方先取好。
 */
static void lexString(Lexer *lx, Vec *out, int line, int col) {
    lxAdvance(lx);
    size_t start = lx->pos;

    while (lx->pos < lx->len && lxPeek(lx, 0) != '"') {
        if (lxPeek(lx, 0) == '\n') {
            ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
            return;
        }
        if (lxPeek(lx, 0) == '\\') {
            /* 转义必须经 `lexEscape` 校验（"which escapes exist" 的唯一判据）：原样漏给 C 后端
             * 会被它整个拒绝，例如 `"\u64"` 报 "incomplete universal character name \u64"。
             * `lexEscape` 已消费掉转义及其参数，这里不能再前进一次，否则会多吃一个字面量字符。 */
            lxAdvance(lx);
            if (lexEscape(lx, line, col, "a string literal") < 0) return;
            continue;
        }
        lxAdvance(lx);
    }
    if (lx->pos >= lx->len) {
        ctxError(lx->ctx, line, col, NULL, "unterminated string literal");
        return;
    }

    size_t n = lx->pos - start;
    lxPushAt(lx, out, TK_STRING, lx->src + start, n, line, col);
    lxAdvance(lx);                              /* 连同右引号一起消费 */
}

/**
 * @brief 解出一个转义序列的字节值；出错时报错并返回 -1。
 *
 * 与 `lexChar` 相邻，好让「有哪些转义」与错误措辞只留一处。`\0` 是 NUL 字节；
 * `\xHH` 接受一或两个十六进制数字。
 *
 * @param lx    游标，停在反斜杠之后待解码的那个字符上。
 * @param line  转义所在行号，用于诊断。
 * @param col   转义所在列号，用于诊断。
 * @param what  错误措辞里的上下文名（如 `string literal`）。
 * @return      该转义代表的字节值；未知转义或十六进制转义缺数字时报错并返回 -1。
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
 * @brief 词法化一个字符字面量：`'a'`、`'\n'`、`'\x41'`、`'\''`。
 *
 * 它产出的是普通**整数字面量**，携带字节值，于是解析器与类型检查器都不用新增分支：
 * `'a'` 就是 `u8(97)` 去掉了噪音，the manual's rule ("no `char`; a type name carries its width")
 * 也因此保持成立。扫描先在本行内找右引号（跳过 `\\`），以此区分 "unterminated" 与
 * "more than one byte"，然后才解码内容。
 * token 文本是源码拼法，`--dump-tokens` 显示的就是写下的样子；其中只有 `ival` 对解析器有意义。
 *
 * @param lx    游标，停在起始引号上。
 * @param out   追加到的 token 向量。
 * @param line  起始引号的行号，由调用方先取好。
 * @param col   起始引号的列号，由调用方先取好。
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
        lx->pos = start + 2;                       /* 跳过反斜杠，lexEscape 从它后面解码 */
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
    lx->pos = close + 1;                           /* 连同右引号一起消费 */
    Token *t = lxPushAt(lx, out, TK_INT, lx->src + start, close + 1 - start, line, col);
    t->ival = v;
}

/**
 * @brief 判断一个拼法是不是语言的标点 token 之一。
 *
 * 字符串 token 存的是去掉引号后的内容（a string literal's text），于是源码里一个内容为右括号
 * 的字符串 token，其文本与标点 `)` 相等；解析器比较 token 文本前必须先问这一句，否则
 * `println` 里那个右括号会被当成参数结束，而内容为 `x)` 的字符串没事 —— 差别只是一个字节。
 *
 * @param value  要测的拼法，例如 `)` 或 `=>` 这样的符号串；NULL 与空串不是标点。
 * @return       `value` 出现在 PUNCTS 里则返回 true。
 */
bool lexIsPunct(const char *value) {
    if (!value || !*value) return false;
    for (size_t i = 0; PUNCTS[i]; i++)
        if (strcmp(PUNCTS[i], value) == 0) return true;
    return false;
}

/**
 * @brief 按表中首个匹配把一段标点词法化进 `out`。
 *
 * 表按顺序试，所以长符号必须排在被它开头的短符号之前，多字符条目才能最长匹配。
 * `<<` 与 `>>` 有意不在表里：解析器把它们认作相邻的两个 `<` / `>` token，
 * `box<box<i32>>` 才能合上泛型参数。
 *
 * @param lx    游标，停在符号首字符上。
 * @param out   追加到的 token 向量。
 * @param line  该字符的行号，由调用方先取好。
 * @param col   该字符的列号，由调用方先取好。
 * @return      匹配到并消费了一个符号则返回 true；否则经 ctxError 报 "unexpected character" 后返回 false。
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
 * @brief 扫描一段源，把 token 追加到 `out`，末尾是 TK_EOF。
 *
 * 格式串（`f` 后紧跟引号）用它扫花括号里的片段：位置从给定的行/列起算，片段里的诊断才能指回文件，
 * 而不是片段的偏移 0。换行是真的 token 而不是空白：语句在换行处结束，只有解析器能说它在哪里
 * 有意义；连续的换行折成一个 TK_NEWLINE，末尾总有一个，解析器就不必特判文件结尾。
 * 行注释与块注释在这里跳过，从不进解析器。词法出错时经 `ctxError` 报错并提前返回，
 * `out` 里留的是出错前切出的 token，TK_EOF 可能缺席。
 *
 * @param ctx    编译上下文：提供源文本，以及每个 token 文本都拷进去的 arena；错误经 ctxError 置 ctx->hasError。
 * @param src    要扫描的字节起点。
 * @param len    要扫描的字节数。
 * @param line0  起点行号（1 起）。
 * @param col0   起点列号（1 起）。
 * @param out    追加到的向量；初始化由调用方负责。
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
        /* `f"…"`：标识符恰好是 `f` 且**紧挨着**引号 ⇒ 格式串。`f` 本身仍是合法标识符
         * （`f "x"` 带空格就是两个 token）。 */
        if (isAlpha(c)) {
            if (c == 'f' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '"') {
                lxAdvance(&lx);
                lexString(&lx, out, line, col);
                if (ctx->hasError) return;
                Token *ft = lxLast(out);
                if (ft && ft->kind == TK_STRING) ft->kind = TK_FSTRING;
                continue;
            }
            lexIdent(&lx, out, line, col);
            continue;
        }
        if (c == '"')                        { lexString(&lx, out, line, col); if (ctx->hasError) return; continue; }
        if (c == '\'')                      { lexChar(&lx, out, line, col); if (ctx->hasError) return; continue; }
        if (!lexPunct(&lx, out, line, col))  { return; }
    }

    /* 文件末尾总补一个语句终止符，解析器不必特判结尾。 */
    Token *last = lxLast(out);
    if (!last || last->kind != TK_NEWLINE)
        lxPushAt(&lx, out, TK_NEWLINE, "\n", 1, lx.line, lx.col);
    lxPushAt(&lx, out, TK_EOF, "", 0, lx.line, lx.col);
}

/**
 * @brief 把整个 `ctx->src` 词法化，token 追加到 `out`。
 *
 * `lexRange` 的薄壳：`f"…"` 需要扫子串，真正干活的扫描器收的是区间；整个文件只是
 * 从第 1 行第 1 列开始的那个特例。
 *
 * @param ctx  提供源文本，以及每个 token 文本都拷进去的 arena。
 * @param out  追加到的向量；初始化由调用方负责。
 */
void lexAll(Ctx *ctx, Vec *out) {
    lexRange(ctx, ctx->src, ctx->srcLen, 1, 1, out);
}

/* ================================================================ display */

/**
 * @brief 给出 token 种类的拼法，供诊断与 `--dump-tokens` 用，例如 TK_IDENT -> `IDENT`。
 *
 * @param k  token 种类。
 * @return   它的名字；未知种类返回 `?`。
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
 * @brief 追加一行描述 `t` 的可读文本到 `out`，供 `--dump-tokens` 用。
 *
 * 数值按 token 种类打印（TK_INT 打 ival 而不是源码拼法），打出来的就是解析器看到的 token 流。
 *
 * @param t    要描述的 token。
 * @param out  接收文本的缓冲区。
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
