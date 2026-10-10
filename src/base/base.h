/**
 * @file base.h
 * @brief 编译器的基础设施：arena、可增长缓冲、向量、以及每个 pass 都收的诊断上下文。
 *
 * 这是流水线的最底层，词法、解析、检查、代码生成都包含这个头文件。
 *
 * 这份 C 代码遵守的规矩，就是 extC 自己要强制的规矩：
 *   1. 没有手动释放——一切都来自 Arena，在进程退出时整体归还；
 *   2. 没有可变全局变量——所有状态都挂在显式传进来的 Ctx 上；
 *   3. 命名：函数 camelCase，类型 PascalCase。
 */
#ifndef EXTC_BASE_H
#define EXTC_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 判断某个 `EXTC_DBG_*` 开关是否打开。
 *
 * @param name  开关名（环境变量名，例如 `EXTC_DBG_ARENA`）。
 * @return      开关打开时为 `true`。
 */
bool dbgOn(const char *name);

/* ---------------------------------------------------------------- Arena
 * 只向前推进、从不回收的 bump 分配器。对"跑一次就退出"的编译器这是最优的。
 */

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t      blockSize;
} Arena;

/**
 * @brief 初始化一只 arena。
 * @param a          要初始化的 arena。
 * @param blockSize  每次向系统要的块大小（字节）。
 */
void  arenaInit(Arena *a, size_t blockSize);

/**
 * @brief 从 arena 分配 `n` 字节，**不清零**。
 * @param a  arena。
 * @param n  字节数。
 * @return   指向 `n` 字节的指针。
 */
void *arenaAlloc(Arena *a, size_t n);

/**
 * @brief 从 arena 分配 `n` 字节并清零。
 * @param a  arena。
 * @param n  字节数。
 * @return   指向已清零的 `n` 字节的指针。
 */
void *arenaAllocZero(Arena *a, size_t n);

/**
 * @brief 把 `s` 的前 `n` 个字节拷进 arena，并补一个 NUL 结尾。
 * @param a  arena。
 * @param s  源字节串（不必以 NUL 结尾）。
 * @param n  拷贝的字节数。
 * @return   以 NUL 结尾的副本。
 */
char *arenaStrndup(Arena *a, const char *s, size_t n);

/**
 * @brief 把一个文件整份读进 arena，并以 NUL 结尾。
 *
 * @param a      提供内存的 arena。
 * @param path    要读的文件。
 * @param outLen  接收读到的字节数（不含结尾的 NUL）；可以传 `NULL`。
 * @return        缓冲区；文件打不开、定位不了或量不出长度时返回 `NULL`。
 */
char *readWholeFile(Arena *a, const char *path, size_t *outLen);

/**
 * @brief 按格式串在 arena 里拼一个字符串（与 `printf` 同族的格式）。
 * @param a    arena。
 * @param fmt  格式串。
 * @return      拼接结果，以 NUL 结尾。
 */
char *arenaPrintf(Arena *a, const char *fmt, ...);

/* ---------------------------------------------------------------- Buf
 * 可增长的字节缓冲，用来累积输出文本（生成的 C）。
 * 增长时不归还旧块——旧块归 arena 所有。
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap;
} Buf;

/**
 * @brief 用一只 arena 初始化一个缓冲。
 * @param b  要初始化的缓冲。
 * @param a  提供内存的 arena。
 */
void  bufInit(Buf *b, Arena *a);

/**
 * @brief 追加一个字节。
 * @param b  缓冲。
 * @param c  要追加的字节。
 */
void  bufPutc(Buf *b, char c);

/**
 * @brief 追加一个以 NUL 结尾的字符串（不含结尾的 NUL）。
 * @param b  缓冲。
 * @param s  源字符串。
 */
void  bufPuts(Buf *b, const char *s);

/**
 * @brief 追加 `s` 的前 `n` 个字节。
 * @param b  缓冲。
 * @param s  源字节串。
 * @param n  字节数。
 */
void  bufPutn(Buf *b, const char *s, size_t n);

/**
 * @brief 按格式串把内容追加进缓冲。
 * @param b    缓冲。
 * @param fmt  格式串。
 */
void  bufPrintf(Buf *b, const char *fmt, ...);

/**
 * @brief 把缓冲内容当作 C 字符串：确保以 NUL 结尾，并返回首地址。
 * @param b  缓冲。
 * @return    以 NUL 结尾的内容首地址。
 */
char *bufCstr(Buf *b);

/* ---------------------------------------------------------------- Vec
 * 定长元素的动态数组（结构体的数组）。
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap, elemSize;
} Vec;

/**
 * @brief 初始化一个向量。
 * @param v         要初始化的向量。
 * @param a         提供内存的 arena。
 * @param elemSize  单个元素的字节数。
 */
void  vecInit(Vec *v, Arena *a, size_t elemSize);

/**
 * @brief 追加一个**未初始化**的元素并返回它的地址。
 * @param v  向量。
 * @return   新元素的地址；调用者负责写满 `elemSize` 个字节。
 */
void *vecPush(Vec *v);

/**
 * @brief 取第 `i` 个元素的地址。
 * @param v  向量。
 * @param i  下标（从 0 起，必须小于当前长度）。
 * @return   元素地址。
 */
void *vecAt(Vec *v, size_t i);

/**
 * @brief 当前元素个数。
 * @param v  向量。
 * @return   元素个数。
 */
size_t vecLen(const Vec *v);

/* ---------------------------------------------------------------- Ctx
 * 所有可变状态都挂在这里，并显式传递：编译器没有全局变量。
 * 诊断被**记录**下来而不是当场打印，于是发现问题的那个 pass 不决定文本最终去哪。
 */

#define EXTC_MAXERR 512

typedef struct {
    Arena      *arena;
    const char *path;
    const char *src;
    size_t      srcLen;

    bool hasError;
    /* 警告通道：不改变退出码的诊断。 */
    int  warnCount;
    bool noWarn;      /* `-w`：一条警告都不打印 */
    int  errLine, errCol;
    char errMsg[EXTC_MAXERR];
    char errNote[EXTC_MAXERR];
} Ctx;

/**
 * @brief 这个标识符是不是 C 关键字？
 *
 * extC 的名字要能当作 C 名用，所以必须避开它们。规则：extC 源码里的名字从不改写，
 * 只有生成的 C 里的名字会加后缀。
 *
 * @param name  要判断的标识符。
 * @return      是 C 关键字时为 `true`。
 */
bool cIdentIsKeyword(const char *name);

/**
 * @brief 初始化诊断上下文。
 * @param c       要初始化的上下文。
 * @param a       arena。
 * @param path    源文件路径（出现在诊断里）。
 * @param src     源文本。
 * @param srcLen  源文本字节数。
 */
void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen);

/**
 * @brief 记一条错误（会置 `hasError`，编译最终失败）。
 * @param c     上下文。
 * @param line  源行号。
 * @param col   源列号。
 * @param note  补充说明，可以传 `NULL`。
 * @param fmt   消息格式串。
 */
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...);

/**
 * @brief 记一条警告：**不置** `hasError`，编译继续。
 *
 * 与错误同样的格式，立刻打到 stderr。
 *
 * @param c     上下文。
 * @param line  源行号。
 * @param col   源列号。
 * @param note  补充说明，可以传 `NULL`。
 * @param fmt   消息格式串。
 */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...);

/**
 * @brief 把记录下来的诊断渲染成文本。
 * @param c    上下文。
 * @param out  接收文本的缓冲。
 */
void ctxRenderDiag(Ctx *c, Buf *out);

#endif /* EXTC_BASE_H */
