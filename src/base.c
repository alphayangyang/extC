/* Implementation of the arena, growable buffer, vector, and diagnostic context
 * declared in base.h. Every pass allocates from the arena and never releases:
 * the process exit reclaims everything at once.
 */

#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Round a size up to the arena's alignment.
 *
 * Params:
 *   n - requested size in bytes
 *
 * Returns:
 *   The smallest multiple of ARENA_ALIGN that is >= n. A zero request stays zero,
 *   so the caller is responsible for having turned 0 into a real size already.
 */
static size_t alignUp(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

/* Prepare an arena that owns no blocks yet.
 *
 * Params:
 *   a         - arena to initialise
 *   blockSize - default block size in bytes; 0 selects 64 KiB
 *
 * Notes:
 *   - The first block is created lazily by the first allocation, so an arena that
 *     is never used costs nothing.
 */
void arenaInit(Arena *a, size_t blockSize) {
    a->head = NULL;
    a->blockSize = blockSize ? blockSize : 65536;
}

/* Take a fresh block from the system allocator and push it onto the arena.
 *
 * Params:
 *   a    - arena to extend
 *   need - number of bytes the caller is about to bump-allocate; the block is
 *          never made smaller than this, or the request would not fit in it
 *
 * Returns:
 *   The new block, which is also the arena's head and therefore the block the
 *   next allocation comes out of.
 *
 * Notes:
 *   - Running out of memory is fatal. There is no way to report it to the user and
 *     no sensible state to continue from, so this prints one line and exits 1.
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

/* Bump-allocate bytes from the arena.
 *
 * Params:
 *   a - arena to allocate from
 *   n - size in bytes; 0 is rounded up to 1
 *
 * Returns:
 *   n bytes aligned to ARENA_ALIGN, uninitialised: arenaAllocZero is the zeroing
 *   variant.
 *
 * Notes:
 *   - Nothing is ever freed individually, so the arena is only usable for data
 *     that lives until the end of the process.
 *   - The alignment comes from the block's own alignment, which malloc provides.
 */
void *arenaAlloc(Arena *a, size_t n) {
    n = alignUp(n ? n : 1);

    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) b = arenaNewBlock(a, n);

    void *p = b->data + b->used;
    b->used += n;
    return p;
}

/* Bump-allocate n bytes and clear them.
 *
 * Params:
 *   a - arena to allocate from
 *   n - size in bytes; the aligned size may be larger, but only n bytes are cleared
 *
 * Returns:
 *   A pointer to n zeroed bytes.
 */
void *arenaAllocZero(Arena *a, size_t n) {
    void *p = arenaAlloc(a, n);
    memset(p, 0, n);
    return p;
}

/* Copy a byte range into the arena and terminate the copy.
 *
 * Params:
 *   a - arena to allocate from
 *   s - source bytes; need not be NUL-terminated
 *   n - number of bytes to copy
 *
 * Returns:
 *   An arena-owned NUL-terminated string.
 */
char *arenaStrndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arenaAlloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Format text into the arena, measuring first so the buffer is exactly sized.
 *
 * Params:
 *   a   - arena to allocate from
 *   fmt - printf format
 *   ... - arguments for fmt
 *
 * Returns:
 *   An arena-owned NUL-terminated string holding the formatted text.
 *
 * Notes:
 *   - A va_list can be walked only once, so the measuring call runs on `ap` and
 *     the writing call on a copy made beforehand; reusing `ap` would be undefined.
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

/* Prepare an empty buffer.
 *
 * Params:
 *   b - buffer to initialise
 *   a - arena that owns the storage and receives the abandoned blocks on growth
 *
 * Notes:
 *   - No storage is allocated here: data stays NULL until the first append.
 */
void bufInit(Buf *b, Arena *a) {
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* Make room for `extra` more bytes after the current length.
 *
 * Params:
 *   b     - buffer to grow
 *   extra - number of bytes about to be appended
 *
 * Notes:
 *   - Capacity starts at 256 bytes and doubles until it fits, so the number of
 *     allocations is logarithmic in the output size.
 *   - One byte past the capacity is allocated as well, so bufCstr can always write
 *     its terminator without a second growth step.
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

/* Append one byte.
 *
 * Params:
 *   b - buffer
 *   c - byte to append; a NUL byte is stored as ordinary content
 */
void bufPutc(Buf *b, char c) {
    bufReserve(b, 1);
    b->data[b->len++] = c;
}

/* Append a byte range.
 *
 * Params:
 *   b - buffer
 *   s - bytes to append; may contain NUL and need not be terminated
 *   n - number of bytes; 0 leaves the buffer untouched
 */
void bufPutn(Buf *b, const char *s, size_t n) {
    if (!n) return;
    bufReserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

/* Append a NUL-terminated string without its terminator.
 *
 * Params:
 *   b - buffer
 *   s - string to append
 */
void bufPuts(Buf *b, const char *s) {
    bufPutn(b, s, strlen(s));
}

/* Format text and append it.
 *
 * Params:
 *   b   - buffer
 *   fmt - printf format
 *   ... - arguments for fmt
 *
 * Notes:
 *   - The length is measured first and the text written by a second vsnprintf from
 *     a va_copy, because a va_list cannot be walked twice.
 *   - vsnprintf is handed n + 1 bytes: it writes the terminator into the spare byte
 *     that bufReserve keeps past the capacity. That byte is outside `len`, so the
 *     next append simply overwrites it.
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

/* Return the contents as a NUL-terminated string.
 *
 * Returns:
 *   The buffer's own storage with a NUL written at data[len]; no copy is made.
 *
 * Notes:
 *   - Any later append invalidates the pointer: it overwrites the terminator and
 *     may move the data to a new block. Call this once, at the point the text is
 *     complete.
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

/* Prepare an empty vector.
 *
 * Params:
 *   v        - vector to initialise
 *   a        - arena that owns the storage
 *   elemSize - size of one element in bytes; 0 would make every element alias the
 *              same address, so callers always pass sizeof of a real type
 */
void vecInit(Vec *v, Arena *a, size_t elemSize) {
    v->arena = a;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elemSize = elemSize;
}

/* Double the capacity and move the live elements into the new block.
 *
 * Notes:
 *   - Capacity starts at 8 elements when the vector is empty.
 *   - Every element pointer handed out earlier becomes dangling here. Nothing may
 *     hold a vecPush or vecAt result across a push on the same vector.
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

/* Append one element.
 *
 * Returns:
 *   A pointer to the new element, aligned for the element type and left
 *   uninitialised: the caller writes the element through it.
 *
 * Notes:
 *   - The pointer dies at the next push on this vector, because growth moves the
 *     elements. Read the element out before pushing again.
 */
void *vecPush(Vec *v) {
    if (v->len == v->cap) vecGrow(v);
    void *p = v->data + v->len * v->elemSize;
    v->len++;
    return p;
}

/* Return a pointer to element i.
 *
 * Params:
 *   v - vector
 *   i - index; the caller guarantees i < vecLen(v)
 *
 * Notes:
 *   - Out-of-range indices are not checked: they read whatever follows the
 *     elements.
 */
void *vecAt(Vec *v, size_t i) {
    return v->data + i * v->elemSize;
}

/* Number of elements currently stored. */
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

/* Prepare the diagnostic context for one source file.
 *
 * Params:
 *   c      - context to initialise
 *   a      - arena the context belongs to
 *   path   - file name shown in diagnostics; PRELUDE_PATH for the bundled prelude
 *   src    - the whole source text, kept so a diagnostic can quote the line
 *   srcLen - length of src in bytes, without any terminator
 *
 * Notes:
 *   - warnCount and noWarn are not set here. The driver assigns noWarn just after
 *     this call; a caller that also cares about the count has to zero the context
 *     itself first.
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

/* Record the first error of this compilation and ignore every later one.
 *
 * Params:
 *   c    - context
 *   line - 1-based source line; <= 0 means the error has no position
 *   col  - 1-based column; <= 0 is rendered as column 1
 *   note - optional one-line explanation printed under the message; NULL for none
 *   fmt  - printf format for the message
 *   ...  - arguments for fmt
 *
 * Notes:
 *   - Only the first error survives, so one mistake does not bury the user under a
 *     cascade of follow-up diagnostics.
 *   - Nothing is printed here; the text is rendered later by ctxRenderDiag. That
 *     keeps the error independent of whatever else is being written to the streams.
 *   - Message and note are truncated at EXTC_MAXERR bytes.
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

/* Print a warning to stderr immediately, in the same shape as an error.
 *
 * Params:
 *   c    - context
 *   line - 1-based source line; a line past the end of the file prints only the
 *          header line
 *   col  - 1-based column the caret is placed under; <= 0 is treated as 1
 *   note - optional explanation printed after the source line; NULL for none
 *   fmt  - printf format for the message
 *   ...  - arguments for fmt
 *
 * Notes:
 *   - A warning never sets hasError: compilation continues and the exit code is
 *     unchanged. That is the whole point of the channel.
 *   - `-w` (Ctx.noWarn) returns before the count is bumped, so suppressed warnings
 *     are not counted either.
 *   - The source line is found by scanning from the beginning of the file, so each
 *     warning costs a pass over the text up to its own line.
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

/* Render the recorded error into a buffer: header, source line, caret.
 *
 * Params:
 *   c   - context holding the error
 *   out - buffer the text is appended to; the caller picks the stream
 *
 * Notes:
 *   - An error raised before parsing has no line number, so it renders as
 *     `path: error: message` with no source excerpt.
 *   - The source line is reached by scanning from the start of the file. That is
 *     linear in the file size, which is acceptable because only one error is ever
 *     reported per run.
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

/* True when this extC name cannot be copied into the generated C unchanged.
 *
 * Most names are copied verbatim, since the generated C is meant to stay
 * readable. Three groups collide and have to be renamed:
 *   1. C keywords: `fn double(...)` is perfectly legal extC (extC's float type is
 *      `f64`), but the generated C would contain `int32_t double(int32_t);`, which
 *      does not compile -- and the error would point into generated code the user
 *      never wrote.
 *   2. `bool`, `true`, and `false` from <stdbool.h>: they are macros in the
 *      generated C, so a collision would damage the code that follows.
 *   3. GNU C's `asm` and `typeof`, which gcc accepts by default under gnu11.
 *
 * Params:
 *   name - the extC name to test
 *
 * Returns:
 *   True when the generated C needs a modified name (codegen appends a suffix).
 *   The name in the extC source is never rewritten.
 *
 * Notes:
 *   - The table is NULL-terminated and scanned linearly; it is small enough that a
 *     lookup structure would only add code.
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
