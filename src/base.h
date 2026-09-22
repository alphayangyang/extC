/* extC 编译器 —— 基础设施。
 *
 * 写这个 C 编译器的规矩，就是 extC 将来要强制的规矩 ——
 * 所以它不是"练 C"，它是 extC 内存/全局状态模型的**第一次真实使用**：
 *
 *   1. 没有手动 free —— 一切从 Arena 走，进程结束一起还（对应 extC 的 arena）
 *   2. 没有 static 可变全局 —— 所有状态挂在 Ctx 上显式传递（对应 extC 的"无 static"）
 *   3. 命名：函数 camelCase，类型 PascalCase（对应主人的命名规范）
 *
 * 跑完这一遍就知道：这些规矩在真实程序里到底好不好用。
 */
#ifndef EXTC_BASE_H
#define EXTC_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- Arena
 * bump 分配器，只往后走，从不回收。
 * 对一个"跑一次就退出"的编译器，这就是最优解 —— 也是 extC 的模型。
 */

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t      blockSize;
} Arena;

void  arenaInit(Arena *a, size_t blockSize);
void *arenaAlloc(Arena *a, size_t n);
void *arenaAllocZero(Arena *a, size_t n);
char *arenaStrndup(Arena *a, const char *s, size_t n);
char *arenaPrintf(Arena *a, const char *fmt, ...);

/* ---------------------------------------------------------------- Buf
 * 可增长的字节缓冲。用来攒输出文本（生成的 C 代码）。
 * 增长时旧块不还 —— arena 拥有它。这是 arena 模型的真实代价，先记下来。
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap;
} Buf;

void  bufInit(Buf *b, Arena *a);
void  bufPutc(Buf *b, char c);
void  bufPuts(Buf *b, const char *s);
void  bufPutn(Buf *b, const char *s, size_t n);
void  bufPrintf(Buf *b, const char *fmt, ...);
char *bufCstr(Buf *b);

/* ---------------------------------------------------------------- Vec
 * 定长元素的动态数组（array of struct）。
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap, elemSize;
} Vec;

void  vecInit(Vec *v, Arena *a, size_t elemSize);
void *vecPush(Vec *v);
void *vecAt(Vec *v, size_t i);
size_t vecLen(const Vec *v);

/* ---------------------------------------------------------------- Ctx
 * 一切可变状态都挂在这里显式传递 —— 没有全局变量。
 * （跟五子棋引擎里的 SearchCtx 是同一个套路）
 */

#define EXTC_MAXERR 512

typedef struct {
    Arena      *arena;
    const char *path;
    const char *src;
    size_t      srcLen;

    bool hasError;
    /* ⭐ 警告通道（2026-09-22 加）：**不改变退出码**的诊断（P′ 的另一半 ——
     * 有些事"该说但不该拦"：拦下来只是烦人 ✗）*/
    int  warnCount;
    bool noWarn;      /* `-w`：一条警告都不吐 ✓ */
    int  errLine, errCol;
    char errMsg[EXTC_MAXERR];
    char errNote[EXTC_MAXERR];
} Ctx;

/* 这个标识符是不是 C 的关键字？extC 的名字要避开它们才能当 C 名字用。
 * 规则：**不改 extC 源码里的名字**，只在生成 C 时加后缀（见 DECISIONS 定案 48）。 */
bool cIdentIsKeyword(const char *name);

void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen);
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...);
/* 警告：**不设 hasError**（编译照常继续）✓ 立即打到 stderr ✓ 同 errors 的格式 ✓ */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...);
void ctxRenderDiag(Ctx *c, Buf *out);

#endif /* EXTC_BASE_H */
