#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ Arena */

struct ArenaBlock {
    ArenaBlock *next;
    size_t      used;
    size_t      cap;
    char        data[];
};

#define ARENA_ALIGN 16

static size_t alignUp(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

void arenaInit(Arena *a, size_t blockSize) {
    a->head = NULL;
    a->blockSize = blockSize ? blockSize : 65536;
}

static ArenaBlock *arenaNewBlock(Arena *a, size_t need) {
    size_t cap = a->blockSize;
    if (cap < need) cap = need;

    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!b) {
        fprintf(stderr, "extc: out of memory (arena block of %zu bytes)\n", cap);
        exit(1);
    }
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    return b;
}

void *arenaAlloc(Arena *a, size_t n) {
    n = alignUp(n ? n : 1);

    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) b = arenaNewBlock(a, n);

    void *p = b->data + b->used;
    b->used += n;
    return p;
}

void *arenaAllocZero(Arena *a, size_t n) {
    void *p = arenaAlloc(a, n);
    memset(p, 0, n);
    return p;
}

char *arenaStrndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arenaAlloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arenaPrintf(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    char *p = (char *)arenaAlloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

/* ================================================================ Buf */

void bufInit(Buf *b, Arena *a) {
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bufReserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;

    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;

    char *nd = (char *)arenaAlloc(b->arena, ncap + 1);
    if (b->len) memcpy(nd, b->data, b->len);
    b->data = nd;
    b->cap = ncap;
    /* 旧块不还 —— arena 拥有它 */
}

void bufPutc(Buf *b, char c) {
    bufReserve(b, 1);
    b->data[b->len++] = c;
}

void bufPutn(Buf *b, const char *s, size_t n) {
    if (!n) return;
    bufReserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

void bufPuts(Buf *b, const char *s) {
    bufPutn(b, s, strlen(s));
}

void bufPrintf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    bufReserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *bufCstr(Buf *b) {
    bufReserve(b, 1);
    b->data[b->len] = '\0';
    return b->data;
}

/* ================================================================ Vec */

void vecInit(Vec *v, Arena *a, size_t elemSize) {
    v->arena = a;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elemSize = elemSize;
}

static void vecGrow(Vec *v) {
    size_t ncap = v->cap ? v->cap * 2 : 8;

    char *nd = (char *)arenaAlloc(v->arena, ncap * v->elemSize);
    if (v->len) memcpy(nd, v->data, v->len * v->elemSize);
    v->data = nd;
    v->cap = ncap;
    /* 旧块不还 —— arena 拥有它。
     * 发现 #1：arena 里的动态数组在增长时会留下垃圾，直到 arena 结束。
     * 对编译器无所谓（跑一次就退出），但对长跑程序要记住这件事。 */
}

void *vecPush(Vec *v) {
    if (v->len == v->cap) vecGrow(v);
    void *p = v->data + v->len * v->elemSize;
    v->len++;
    return p;
}

void *vecAt(Vec *v, size_t i) {
    return v->data + i * v->elemSize;
}

size_t vecLen(const Vec *v) {
    return v->len;
}

/* ================================================================ Ctx */

void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen) {
    c->arena = a;
    c->path = path;
    c->src = src;
    c->srcLen = srcLen;
    c->hasError = false;
    c->errLine = 1;
    c->errCol = 1;
    c->errMsg[0] = '\0';
    c->errNote[0] = '\0';
}

void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...) {
    if (c->hasError) return;            /* week-0：只报第一个错误 */
    c->hasError = true;
    c->errLine = line;
    c->errCol = col;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->errMsg, sizeof c->errMsg, fmt, ap);
    va_end(ap);

    if (note) snprintf(c->errNote, sizeof c->errNote, "%s", note);
    else c->errNote[0] = '\0';
}

/* ⭐ 警告通道：格式跟错误一样（file:line:col: warning: msg + 那一行 + 插入符），
 * 但**不设 hasError** ⇒ 编译继续 ✓ 不改变退出码 ✓ */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...) {
    if (c->noWarn) return;
    c->warnCount++;

    char msg[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s:%d:%d: warning: %s\n", c->path, line, col, msg);

    int ln = 1;
    size_t i = 0;
    while (i < c->srcLen && ln < line) {
        if (c->src[i] == '\n') ln++;
        i++;
    }
    if (ln == line) {
        size_t start = i, end = start;
        while (end < c->srcLen && c->src[end] != '\n') end++;
        fprintf(stderr, "  ");
        fwrite(c->src + start, 1, end - start, stderr);
        fprintf(stderr, "\n  ");
        for (int k = 1; k < (col > 0 ? col : 1); k++) fputc(' ', stderr);
        fprintf(stderr, "^\n");
    }
    if (note) fprintf(stderr, "  note: %s\n", note);
}

void ctxRenderDiag(Ctx *c, Buf *out) {
    if (c->errLine > 0)
        bufPrintf(out, "%s:%d:%d: error: %s\n", c->path, c->errLine, c->errCol, c->errMsg);
    else
        bufPrintf(out, "%s: error: %s\n", c->path, c->errMsg);   /* 装载阶段的错误没有行号 ✓ */

    /* 找到出错的源码行 */
    int line = 1;
    size_t i = 0;
    while (i < c->srcLen && line < c->errLine) {
        if (c->src[i] == '\n') line++;
        i++;
    }

    if (line == c->errLine) {
        size_t start = i;
        size_t end = start;
        while (end < c->srcLen && c->src[end] != '\n') end++;

        bufPuts(out, "  ");
        bufPutn(out, c->src + start, end - start);
        bufPutc(out, '\n');

        bufPuts(out, "  ");
        int col = c->errCol > 0 ? c->errCol : 1;
        for (int k = 1; k < col; k++) bufPutc(out, ' ');
        bufPuts(out, "^\n");
    }

    if (c->errNote[0]) {
        bufPrintf(out, "  note: %s\n", c->errNote);
    }
}

/* ---------------------------------------------------------------- C 关键字 */

/* extC 的名字要当 C 名字用。**绝大多数名字直接照抄**（生成的 C 才读得懂），
 * 但有几类会撞车，撞了就得改名 —— 见 DECISIONS 定案 48：
 *   ① C 关键字：`fn double(...)` 在 extC 里完全合法（extC 的浮点是 `f64`），
 *      但生成的 C 里 `int32_t double(int32_t);` 编不过，而且报错落在生成的 C 上，
 *      用户看不见自己的源码
 *   ② `<stdbool.h>` 的 `bool` / `true` / `false`（生成的 C 里是宏，撞了连累后面的代码）
 *   ③ GNU C 的 `asm` / `typeof`（gcc 默认 gnu11 认它们）*/
bool cIdentIsKeyword(const char *name) {
    static const char *KW[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while",
        "_Bool", "_Complex", "_Imaginary",
        "bool", "true", "false",
        "asm", "typeof",
        NULL
    };
    for (size_t i = 0; KW[i]; i++)
        if (strcmp(KW[i], name) == 0) return true;
    return false;
}
