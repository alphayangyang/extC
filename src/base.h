/* Compiler infrastructure: the arena, the growable buffer, the vector, and the
 * diagnostic context that every pass takes as its first argument. This is the
 * bottom of the pipeline -- lexer, parser, checker, and code generator all
 * include this header and rely on the invariants documented in it.
 *
 * The rules this C code follows are the rules extC itself will enforce, so it is
 * the first real use of extC's memory and global-state model rather than an
 * exercise in C:
 *
 *   1. No manual free: everything comes from an Arena and goes back at process
 *      exit, the same choice extC makes for its own allocations.
 *   2. No mutable static globals: all state hangs off a Ctx passed explicitly,
 *      mirroring extC's rule that a program carries no hidden state.
 *   3. Naming: camelCase for functions, PascalCase for types.
 *
 * Writing the compiler this way is what shows whether those rules hold up in a
 * real program.
 */
#ifndef EXTC_BASE_H
#define EXTC_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- Arena
 * A bump allocator: it only moves forward and never reclaims.
 * For a compiler that runs once and exits that is optimal -- and it is the model
 * extC applies to its own allocations.
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
 * A growable byte buffer, used to accumulate output text (the generated C).
 * Growing does not give the old block back -- the arena owns it. That is the real
 * cost of the arena model, recorded here on purpose.
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
 * A dynamic array of fixed-size elements (an array of structs).
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
 * All mutable state hangs off here and is passed explicitly: the compiler has no
 * global variables. Diagnostics are recorded rather than printed, so the pass
 * that notices a problem does not decide where the text ends up.
 */

#define EXTC_MAXERR 512

typedef struct {
    Arena      *arena;
    const char *path;
    const char *src;
    size_t      srcLen;

    bool hasError;
    /* Warning channel: diagnostics that do not change the exit code. Some things
     * have to be said without being blocked on -- refusing to build over them
     * would only be annoying. */
    int  warnCount;
    bool noWarn;      /* `-w`: print no warning at all */
    int  errLine, errCol;
    char errMsg[EXTC_MAXERR];
    char errNote[EXTC_MAXERR];
} Ctx;

/* Is this identifier a C keyword? An extC name has to avoid them to be usable as
 * a C name. The rule: the extC source name is never rewritten -- only the name in
 * the generated C gets a suffix (see the mangle step in codegen.c). */
bool cIdentIsKeyword(const char *name);

void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen);
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...);
/* Warn: does not set `hasError`, so compilation continues. Printed to stderr at
 * once, in the same format as an error. */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...);
void ctxRenderDiag(Ctx *c, Buf *out);

#endif /* EXTC_BASE_H */
