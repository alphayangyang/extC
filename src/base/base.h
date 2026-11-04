/**
 * @file base.h
 * @brief Compiler infrastructure: arena, growable buffer, vector, and the diagnostic
 * context every pass receives.
 *
 * This is the bottom layer of the pipeline; the lexer, parser, checker, and code
 * generator all include this header.
 *
 * The C code here follows the rules extC itself enforces:
 *   1. no manual free -- everything comes from an arena and is returned at process exit;
 *   2. no mutable globals -- all state hangs off an explicitly passed Ctx;
 *   3. naming: functions are camelCase, types are PascalCase.
 */
#ifndef EXTC_BASE_H
#define EXTC_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Tell whether an `EXTC_DBG_*` switch is set in the environment.
 *
 * @param name  Switch name (the environment variable, e.g. `EXTC_DBG_ARENA`).
 * @return      `true` when the variable is set.
 */
bool dbgOn(const char *name);

/* ---------------------------------------------------------------- Arena
 * A forward-only bump allocator that never reclaims. For a compiler that runs
 * once and exits this is the cheapest possible policy.
 */

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t      blockSize;
} Arena;

/**
 * @brief Initialize an arena; it owns no block yet.
 * @param a          Arena to initialize.
 * @param blockSize  Size of each block requested from the system, in bytes.
 */
void  arenaInit(Arena *a, size_t blockSize);

/**
 * @brief Allocate `n` bytes from the arena, **without** zeroing.
 * @param a  Arena.
 * @param n  Byte count.
 * @return   Pointer to `n` usable bytes.
 */
void *arenaAlloc(Arena *a, size_t n);

/**
 * @brief Allocate `n` bytes from the arena and zero them.
 * @param a  Arena.
 * @param n  Byte count.
 * @return   Pointer to `n` zeroed bytes.
 */
void *arenaAllocZero(Arena *a, size_t n);

/**
 * @brief Copy the first `n` bytes of `s` into the arena and append a NUL.
 * @param a  Arena.
 * @param s  Source bytes (need not be NUL-terminated).
 * @param n  Number of bytes to copy.
 * @return   NUL-terminated copy.
 */
char *arenaStrndup(Arena *a, const char *s, size_t n);

/**
 * @brief Read a whole file into the arena and NUL-terminate it.
 *
 * @param a       Arena that provides the memory.
 * @param path    File to read.
 * @param outLen  Receives the byte count, excluding the final NUL; may be `NULL`.
 * @return        The buffer, or `NULL` when the file cannot be opened, sought, or sized.
 */
char *readWholeFile(Arena *a, const char *path, size_t *outLen);

/**
 * @brief Format a string into the arena (`printf`-family formats).
 * @param a    Arena.
 * @param fmt  Format string.
 * @return     The formatted text, NUL-terminated.
 */
char *arenaPrintf(Arena *a, const char *fmt, ...);

/* ---------------------------------------------------------------- Buf
 * A growable byte buffer that accumulates output text (the generated C).
 * Growth never returns the old block: the arena owns it.
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap;
} Buf;

/**
 * @brief Initialize a buffer backed by an arena.
 * @param b  Buffer to initialize.
 * @param a  Arena that provides the memory.
 */
void  bufInit(Buf *b, Arena *a);

/**
 * @brief Append one byte.
 * @param b  Buffer.
 * @param c  Byte to append.
 */
void  bufPutc(Buf *b, char c);

/**
 * @brief Append a NUL-terminated string (the terminator itself is not stored).
 * @param b  Buffer.
 * @param s  Source string.
 */
void  bufPuts(Buf *b, const char *s);

/**
 * @brief Append the first `n` bytes of `s`.
 * @param b  Buffer.
 * @param s  Source bytes.
 * @param n  Byte count.
 */
void  bufPutn(Buf *b, const char *s, size_t n);

/**
 * @brief Append formatted text to the buffer.
 * @param b    Buffer.
 * @param fmt  Format string.
 */
void  bufPrintf(Buf *b, const char *fmt, ...);

/**
 * @brief Treat the buffer as a C string: NUL-terminate it and return the first byte.
 * @param b  Buffer.
 * @return   Pointer to the NUL-terminated contents.
 */
char *bufCstr(Buf *b);

/* ---------------------------------------------------------------- Vec
 * A dynamic array of fixed-size elements (arrays of structs).
 */

typedef struct {
    Arena *arena;
    char  *data;
    size_t len, cap, elemSize;
} Vec;

/**
 * @brief Initialize a vector.
 * @param v         Vector to initialize.
 * @param a         Arena that provides the memory.
 * @param elemSize  Size of one element in bytes.
 */
void  vecInit(Vec *v, Arena *a, size_t elemSize);

/**
 * @brief Append one **uninitialized** element and return its address.
 * @param v  Vector.
 * @return   Address of the new element; the caller must write all `elemSize` bytes.
 */
void *vecPush(Vec *v);

/**
 * @brief Return the address of element `i`.
 * @param v  Vector.
 * @param i  Index (zero-based, must be below the current length).
 * @return   Element address.
 */
void *vecAt(Vec *v, size_t i);

/**
 * @brief Current element count.
 * @param v  Vector.
 * @return   Number of elements.
 */
size_t vecLen(const Vec *v);

/* ---------------------------------------------------------------- Ctx
 * All mutable state lives here and is passed explicitly: the compiler has no globals.
 * Diagnostics are **recorded** rather than printed on the spot, so the pass that
 * finds a problem does not decide where the text finally goes.
 */

#define EXTC_MAXERR 512

typedef struct {
    Arena      *arena;
    const char *path;
    const char *src;
    size_t      srcLen;

    bool hasError;
    /* Warning channel: diagnostics that do not change the exit code. */
    int  warnCount;
    bool noWarn;      /* `-w`: do not print any warning */
    bool warnOwnedView; /* `-Wowned-view`: warn when `new T[n]` is held as a bare view */
    int  errLine, errCol;
    char errMsg[EXTC_MAXERR];
    char errNote[EXTC_MAXERR];
} Ctx;

/**
 * @brief Is this identifier a C keyword, or a keyword-like name the generated C rejects?
 *
 * extC names must work as C names, so they have to avoid the C vocabulary. Rule: a
 * name in extC source is never rewritten; only names in the generated C get a suffix.
 *
 * @param name  Identifier to test.
 * @return      `true` when the generated C needs a renamed form.
 */
bool cIdentIsKeyword(const char *name);

/**
 * @brief Initialize the diagnostic context.
 * @param c       Context to initialize.
 * @param a       Arena.
 * @param path    Source path (appears in diagnostics).
 * @param src     Source text.
 * @param srcLen  Source length in bytes.
 */
void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen);

/**
 * @brief Record an error (sets `hasError`; the compilation ultimately fails).
 * @param c     Context.
 * @param line  Source line.
 * @param col   Source column.
 * @param note  Extra note, or `NULL`.
 * @param fmt   Message format.
 */
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...);

/**
 * @brief Record a warning: **does not** set `hasError`, compilation continues.
 *
 * Same shape as an error, but printed to stderr immediately.
 *
 * @param c     Context.
 * @param line  Source line.
 * @param col   Source column.
 * @param note  Extra note, or `NULL`.
 * @param fmt   Message format.
 */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...);

/**
 * @brief Render the recorded diagnostic to text.
 * @param c    Context.
 * @param out  Buffer that receives the text.
 */
void ctxRenderDiag(Ctx *c, Buf *out);

#endif /* EXTC_BASE_H */
