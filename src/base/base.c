/* Implementation of the arena, growable buffer, vector, and diagnostic context
 * declared in base.h. Every pass allocates from the arena and never releases:
 * the process exit reclaims everything at once.
 */

#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 判断某个 `EXTC_DBG_*` 开关是否打开；每个名字只查一次环境。
 *
 * 这些开关所在的循环按局部变量、候选与层级反复运行，`getenv` 在那里是实打实的开销。
 * 环境在编译期间不会变，所以答案缓存下来。缓存最多记 16 个名字，满了就不再记新的；
 * 没记下的名字每次仍照常查环境，结果不变。
 *
 * @param name  开关名（环境变量名，例如 `EXTC_DBG_ARENA`）。
 * @return      环境里设置了该变量则为 `true`（值是什么无所谓）。
 */
bool dbgOn(const char *name) {
    static struct { const char *name; int on; } cache[16];
    static size_t n = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(cache[i].name, name) == 0) return cache[i].on != 0;
    bool on = getenv(name) != NULL;
    if (n < 16) { cache[n].name = name; cache[n].on = on ? 1 : 0; n++; }
    return on;
}

/* ================================================================ Arena
 *
 * A block is a small header followed by inline storage; blocks form a singly
 * linked list, newest first, and allocation only ever advances `used` inside the
 * newest block.
 */

struct ArenaBlock {
    ArenaBlock *next;
    size_t      used;
    size_t      cap;
    char        data[];
};

#define ARENA_ALIGN 16

/**
 * @brief 把字节数向上取整到 arena 的对齐。
 *
 * 传 0 得 0：把 0 变成真实大小是调用方的事。
 *
 * @param n  请求的字节数。
 * @return   不小于 `n` 的最小 ARENA_ALIGN 倍数。
 */
static size_t alignUp(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

/**
 * @brief 初始化一只 arena；此刻它还不拥有任何块。
 *
 * 第一块由第一次分配惰性创建，所以从不使用的 arena 不花代价。
 *
 * @param a          要初始化的 arena。
 * @param blockSize  每次向系统要的块大小（字节）；传 0 选 64 KiB。
 */
void arenaInit(Arena *a, size_t blockSize) {
    a->head = NULL;
    a->blockSize = blockSize ? blockSize : 65536;
}

/**
 * @brief 向系统要一块新块，并把它压到 arena 顶上。
 *
 * 内存耗尽即致命：没有渠道报给用户，也没有能继续下去的状态，于是打印一行并 `exit(1)`。
 *
 * @param a     要扩展的 arena。
 * @param need  调用方马上要 bump 分配的字节数；块绝不小于它，否则这次请求装不下。
 * @return      新块，同时也是 arena 的头，下一次分配就从它里面出。
 */
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

/**
 * @brief 从 arena 分配 `n` 字节，**不清零**。
 *
 * 单块内存从不单独释放，所以 arena 只适合活到进程结束的数据。对齐来自块自身的对齐，
 * 由 malloc 保证。
 *
 * @param a  arena。
 * @param n  字节数；传 0 按 1 处理。
 * @return   指向 `n` 字节的指针，按 ARENA_ALIGN 对齐；要清零用 arenaAllocZero。
 */
void *arenaAlloc(Arena *a, size_t n) {
    n = alignUp(n ? n : 1);

    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) b = arenaNewBlock(a, n);

    void *p = b->data + b->used;
    b->used += n;
    return p;
}

/**
 * @brief 从 arena 分配 `n` 字节并清零。
 *
 * @param a  arena。
 * @param n  字节数；对齐后可能占更多，但只清 `n` 个字节。
 * @return   指向已清零的 `n` 字节的指针。
 */
void *arenaAllocZero(Arena *a, size_t n) {
    void *p = arenaAlloc(a, n);
    memset(p, 0, n);
    return p;
}

/**
 * @brief 把 `s` 的前 `n` 个字节拷进 arena，并补一个 NUL 结尾。
 *
 * @param a  arena。
 * @param s  源字节串（不必以 NUL 结尾）。
 * @param n  拷贝的字节数。
 * @return   以 NUL 结尾的副本。
 */
char *arenaStrndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arenaAlloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/**
 * @brief 按格式串在 arena 里拼一个字符串（与 `printf` 同族的格式）。
 *
 * 先量长度再写，缓冲区大小刚好。`va_list` 只能走一遍，所以量长度用 `ap`，写入用先前
 * `va_copy` 出来的副本；重复使用 `ap` 是未定义行为。
 *
 * @param a    arena。
 * @param fmt  格式串。
 * @return      拼接结果，以 NUL 结尾。
 */
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

/* ================================================================ Buf
 *
 * `len` is the number of bytes in use and `cap` the usable capacity, excluding the
 * one extra byte kept for the terminating NUL that bufCstr writes. The buffer
 * holds raw bytes in the meantime: nothing but bufCstr maintains a terminator.
 */

/**
 * @brief 用一只 arena 初始化一个空的缓冲。
 *
 * 这里不分配存储：第一次追加之前 `data` 一直是 NULL。增长时丢下的旧块归 arena 所有。
 *
 * @param b  要初始化的缓冲。
 * @param a  提供内存的 arena。
 */
void bufInit(Buf *b, Arena *a) {
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/**
 * @brief 为当前长度之后再多 `extra` 个字节腾出空间。
 *
 * 容量从 256 字节起翻倍到装得下为止，所以分配次数是输出大小的对数。容量之外还多要
 * 一个字节，让 bufCstr 总能写下结尾 NUL，不必再增长一次。
 *
 * @param b      要增长的缓冲。
 * @param extra  马上要追加的字节数。
 */
static void bufReserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;

    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;

    char *nd = (char *)arenaAlloc(b->arena, ncap + 1);
    if (b->len) memcpy(nd, b->data, b->len);
    b->data = nd;
    b->cap = ncap;
    /* The old block is not given back -- the arena owns it. */
}

/**
 * @brief 追加一个字节。
 *
 * @param b  缓冲。
 * @param c  要追加的字节；NUL 也按普通内容存下。
 */
void bufPutc(Buf *b, char c) {
    bufReserve(b, 1);
    b->data[b->len++] = c;
}

/**
 * @brief 追加 `s` 的前 `n` 个字节。
 *
 * @param b  缓冲。
 * @param s  源字节串；可以含 NUL，不必以 NUL 结尾。
 * @param n  字节数；传 0 不动缓冲。
 */
void bufPutn(Buf *b, const char *s, size_t n) {
    if (!n) return;
    bufReserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

/**
 * @brief 追加一个以 NUL 结尾的字符串（不含结尾的 NUL）。
 *
 * @param b  缓冲。
 * @param s  源字符串。
 */
void bufPuts(Buf *b, const char *s) {
    bufPutn(b, s, strlen(s));
}

/**
 * @brief 按格式串把内容追加进缓冲。
 *
 * 先算长度、再用 `va_copy` 的副本写第二遍，因为 `va_list` 不能走两遍。
 * vsnprintf 拿到 `n + 1` 字节：结尾 NUL 落在 bufReserve 预留的那个空字节里；它不计入
 * `len`，下一次追加直接覆盖。
 *
 * @param b    缓冲。
 * @param fmt  格式串。
 */
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

/**
 * @brief 把缓冲内容当作 C 字符串：确保以 NUL 结尾，并返回首地址。
 *
 * 返回的是缓冲自己的存储，不复制。之后的任何追加都让这个指针失效（覆盖结尾 NUL，
 * 还可能把数据搬到新块），所以在文本拼完的那一刻调一次。
 *
 * @param b  缓冲。
 * @return    以 NUL 结尾的内容首地址。
 */
char *bufCstr(Buf *b) {
    bufReserve(b, 1);
    b->data[b->len] = '\0';
    return b->data;
}

/* ================================================================ Vec
 *
 * Elements are stored back to back with no header per element, so this code does
 * not know the element type: callers cast the pointers vecPush and vecAt return.
 */

/**
 * @brief 初始化一个空向量。
 *
 * @param v         要初始化的向量。
 * @param a         提供内存的 arena。
 * @param elemSize  单个元素的字节数；传 0 会让每个元素都落在同一地址，调用方总是传 sizeof。
 */
void vecInit(Vec *v, Arena *a, size_t elemSize) {
    v->arena = a;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elemSize = elemSize;
}

/**
 * @brief 容量翻倍，并把活元素搬进新块。
 *
 * 空向量从 8 个元素起。这里交出去的每个元素指针都会悬空：任何 vecPush / vecAt 的结果
 * 都不能跨同一向量上的下一次 push 使用。
 *
 * @param v  要增长的向量。
 */
static void vecGrow(Vec *v) {
    size_t ncap = v->cap ? v->cap * 2 : 8;

    char *nd = (char *)arenaAlloc(v->arena, ncap * v->elemSize);
    if (v->len) memcpy(nd, v->data, v->len * v->elemSize);
    v->data = nd;
    v->cap = ncap;
    /* The old block is not given back: the arena owns it. A dynamic array that
     * lives in an arena therefore leaves garbage behind on every growth until the
     * arena ends. Irrelevant for a compiler that runs once and exits, but a cost
     * worth remembering for a long-running program. */
}

/**
 * @brief 追加一个**未初始化**的元素并返回它的地址。
 *
 * 指针在下一次 push 时失效（增长会搬走元素）：要读就趁早读出来。
 *
 * @param v  向量。
 * @return   新元素的地址，按元素类型对齐；调用者负责写满 `elemSize` 个字节。
 */
void *vecPush(Vec *v) {
    if (v->len == v->cap) vecGrow(v);
    void *p = v->data + v->len * v->elemSize;
    v->len++;
    return p;
}

/**
 * @brief 取第 `i` 个元素的地址。
 *
 * 不检查越界：越界读到的是元素之后的内存。
 *
 * @param v  向量。
 * @param i  下标（从 0 起，必须小于当前长度）。
 * @return   元素地址。
 */
void *vecAt(Vec *v, size_t i) {
    return v->data + i * v->elemSize;
}

/**
 * @brief 当前元素个数。
 *
 * @param v  向量。
 * @return   元素个数。
 */
size_t vecLen(const Vec *v) {
    return v->len;
}

/* ================================================================ Ctx
 *
 * An error is recorded, not printed: the driver decides when and where the
 * diagnostic text appears. Warnings are the exception and go to stderr at once,
 * because a warning does not stop the run and its position in the output is of no
 * consequence.
 */

/**
 * @brief 初始化诊断上下文。
 *
 * 这里不设 `warnCount` 与 `noWarn`：driver 紧接着的那句才赋 `noWarn`，还想关心警告
 * 计数的调用方得自己先把上下文清零。
 *
 * @param c       要初始化的上下文。
 * @param a       arena。
 * @param path    源文件路径（出现在诊断里；自带 prelude 用 PRELUDE_PATH）。
 * @param src     源文本；留着让诊断能摘出那一行。
 * @param srcLen  源文本字节数（不含结尾 NUL）。
 */
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

/**
 * @brief 记一条错误：只留本次编译的第一条，后面的全部忽略。
 *
 * 只留第一条，一个错误才不会把用户埋在后续诊断的雪崩里。这里不打印：文本由
 * ctxRenderDiag 稍后渲染，错误因此与当时正往流里写什么无关。消息与说明都截断到
 * EXTC_MAXERR 字节。
 *
 * @param c     上下文。
 * @param line  源行号（从 1 起）；<= 0 表示这条错误没有位置。
 * @param col   源列号（从 1 起）；<= 0 渲染成第 1 列。
 * @param note  打印在消息下面的一行说明；没有就传 `NULL`。
 * @param fmt   消息格式串。
 */
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...) {
    if (c->hasError) return;            /* keep only the first error */
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

/**
 * @brief 记一条警告：**不置** `hasError`，编译继续；立刻打到 stderr。
 *
 * 与错误同样的格式。`-w`（`Ctx.noWarn`）在计数之前就返回，被压掉的警告也不计入。
 * 源行靠从文件开头扫描找到，所以每条警告的代价是到它自己那一行为止的一遍文本。
 *
 * @param c     上下文。
 * @param line  源行号；超出文件末尾时只打印头部一行。
 * @param col   插入符所在的列（从 1 起）；<= 0 当 1 处理。
 * @param note  源行之后的一行说明；没有就传 `NULL`。
 * @param fmt   消息格式串。
 */
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

/**
 * @brief 把记录下来的诊断渲染成文本：头部、源行、插入符。
 *
 * 解析之前产生的错误没有行号，渲染成 `path: error: message`，不给源码摘录。
 * 源行靠从文件开头扫描找到；每次运行只报一条错误，所以这个线性代价可以接受。
 *
 * @param c    上下文。
 * @param out  接收文本的缓冲；由调用方决定它最终去哪个流。
 */
void ctxRenderDiag(Ctx *c, Buf *out) {
    /* An error raised before parsing has no line number. */
    if (c->errLine > 0)
        bufPrintf(out, "%s:%d:%d: error: %s\n", c->path, c->errLine, c->errCol, c->errMsg);
    else
        bufPrintf(out, "%s: error: %s\n", c->path, c->errMsg);

    /* Walk to the offending source line by counting newlines. */
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

/* ------------------------------------------------------- C identifier names */

/**
 * @brief 这个标识符是不是 C 关键字，或者 `bool`/`true`/`false`，或者 GNU 的 `asm`/`typeof`？
 *
 * extC 的名字要能当作 C 名用，所以必须避开它们。三组会撞：C 关键字（extC 允许写
 * `fn double(...)`，生成的 C 却编不过，而报错会指进用户没写过的生成代码）、
 * `<stdbool.h>` 的 `bool`/`true`/`false`（在生成的 C 里是宏）、以及 gnu11 默认放行的
 * GNU `asm`/`typeof`。规则：extC 源码里的名字从不改写，只有生成的 C 里的名字会加后缀。
 *
 * 表以 NULL 结尾、线性扫描；它足够小，加查找结构只会多出代码。
 *
 * @param name  要判断的标识符。
 * @return      生成的 C 需要改名（codegen 加后缀）时为 `true`。
 */
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

/**
 * @brief 把一个文件整份读进 arena，并以 NUL 结尾。
 *
 * 按 `ftell` 报的长度要 `n + 1` 字节；短读时按实际读到的字节数收尾，`outLen` 报的也是
 * 实际值。
 *
 * @param a       提供内存的 arena。
 * @param path    要读的文件。
 * @param outLen  接收读到的字节数（不含结尾的 NUL）；可以传 `NULL`。
 * @return        缓冲区；文件打不开、定位不了或量不出长度时返回 `NULL`。
 */
char *readWholeFile(Arena *a, const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)arenaAlloc(a, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (outLen) *outLen = got;
    return buf;
}
