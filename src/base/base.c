/* Implementation of the arena, growable buffer, vector, and diagnostic context
 * declared in base.h. Every pass allocates from the arena and never releases;
 * process exit reclaims everything at once.
 */

#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Tell whether an `EXTC_DBG_*` switch is set; each name looks at the
 * environment at most once.
 *
 * These switches sit in loops that run repeatedly over locals, candidates, and
 * levels, where `getenv` is a real cost. The environment does not change during a
 * compilation, so the answer is cached. The cache holds at most 16 names; once it
 * is full, later names keep consulting the environment, with the same result.
 *
 * @param name  Switch name (the environment variable, e.g. `EXTC_DBG_ARENA`).
 * @return      `true` when the variable is set (its value does not matter).
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
 * @brief Round a byte count up to the arena alignment.
 *
 * Rounding 0 gives 0: turning 0 into a real size is the caller's job.
 *
 * @param n  Requested byte count.
 * @return   The smallest multiple of ARENA_ALIGN that is at least `n`.
 */
static size_t alignUp(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

/**
 * @brief Initialize an arena; it owns no block yet.
 *
 * The first block is created lazily by the first allocation, so an unused arena
 * costs nothing.
 *
 * @param a          Arena to initialize.
 * @param blockSize  Size of each block requested from the system, in bytes; 0
 *                   selects 64 KiB.
 */
void arenaInit(Arena *a, size_t blockSize) {
    a->head = NULL;
    a->blockSize = blockSize ? blockSize : 65536;
}

/**
 * @brief Request a new block from the system and push it onto the arena.
 *
 * Running out of memory is fatal: there is no channel to report it to the user and
 * no state worth continuing from, so print one line and `exit(1)`.
 *
 * @param a     Arena to extend.
 * @param need  Bytes the caller is about to bump-allocate; the block is never
 *              smaller than this, or the request would not fit.
 * @return      The new block, now the arena head and the source of the next
 *              allocation.
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
 * @brief Value of `EXTC_ARENA_FILL`, read from the environment once; 0 disables
 * filling (the default).
 *
 * @return Fill byte in 0..255.
 */
static int arenaFillByte(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("EXTC_ARENA_FILL");
        cached = 0;
        if (e && *e) {
            long v = strtol(e, NULL, 0);
            if (v > 0 && v <= 255) cached = (int)v;
        }
    }
    return cached;
}

/**
 * @brief Allocate `n` bytes from the arena, **without** zeroing.
 *
 * A single block is never freed on its own, so the arena suits data that lives to
 * the end of the process. Alignment comes from the block, which `malloc` aligns.
 *
 * @param a  Arena.
 * @param n  Byte count; 0 is treated as 1.
 * @return   Pointer to `n` bytes aligned to ARENA_ALIGN; use arenaAllocZero for
 *           zeroed memory.
 */
void *arenaAlloc(Arena *a, size_t n) {
    n = alignUp(n ? n : 1);

    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) b = arenaNewBlock(a, n);

    void *p = b->data + b->used;
    b->used += n;
    /* Debug switch `EXTC_ARENA_FILL=<0-255>`: fill every **unzeroed** arena
     * allocation with a fixed pattern, which turns "read arena memory that was never
     * written" from intermittent into deterministic -- our own `MALLOC_PERTURB_`.
     * Unset by default, so not a single byte changes and output is byte-identical;
     * arenaAllocZero clears after this call and is unaffected either way. */
    int fill = arenaFillByte();
    if (fill) memset(p, fill, n);
    return p;
}

/**
 * @brief Allocate `n` bytes from the arena and zero them.
 *
 * @param a  Arena.
 * @param n  Byte count; alignment may reserve more, but only `n` bytes are cleared.
 * @return   Pointer to `n` zeroed bytes.
 */
void *arenaAllocZero(Arena *a, size_t n) {
    void *p = arenaAlloc(a, n);
    memset(p, 0, n);
    return p;
}

/**
 * @brief Copy the first `n` bytes of `s` into the arena and append a NUL.
 *
 * @param a  Arena.
 * @param s  Source bytes (need not be NUL-terminated).
 * @param n  Number of bytes to copy.
 * @return   NUL-terminated copy.
 */
char *arenaStrndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arenaAlloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/**
 * @brief Format a string into the arena (`printf`-family formats).
 *
 * The length is computed first, then the buffer is sized exactly. A `va_list` can
 * be walked once only: measure with `ap` and write with the `va_copy`-ed `ap2`;
 * reusing `ap` is undefined behavior.
 *
 * @param a    Arena.
 * @param fmt  Format string.
 * @return     The formatted text, NUL-terminated.
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
 * one extra byte kept for the terminating NUL that bufCstr writes. In between the
 * buffer holds raw bytes: nothing but bufCstr maintains a terminator.
 */

/**
 * @brief Initialize an empty buffer backed by an arena.
 *
 * No storage is allocated here: `data` stays NULL until the first append. Blocks
 * dropped by growth belong to the arena.
 *
 * @param b  Buffer to initialize.
 * @param a  Arena that provides the memory.
 */
void bufInit(Buf *b, Arena *a) {
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/**
 * @brief Make room for `extra` bytes beyond the current length.
 *
 * Capacity starts at 256 bytes and doubles until it fits, so the number of
 * allocations is logarithmic in the output size. One byte beyond the capacity is
 * always requested, so bufCstr can write its terminator without growing again.
 *
 * @param b      Buffer to grow.
 * @param extra  Bytes about to be appended.
 */
static void bufReserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;

    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;

    char *nd = (char *)arenaAlloc(b->arena, ncap + 1);
    if (b->len) memcpy(nd, b->data, b->len);
    b->data = nd;
    b->cap = ncap;
    /* The old block is not given back: the arena owns it. */
}

/**
 * @brief Append one byte.
 *
 * @param b  Buffer.
 * @param c  Byte to append; a NUL is stored as ordinary content.
 */
void bufPutc(Buf *b, char c) {
    bufReserve(b, 1);
    b->data[b->len++] = c;
}

/**
 * @brief Append the first `n` bytes of `s`.
 *
 * @param b  Buffer.
 * @param s  Source bytes; may contain NUL and need not be NUL-terminated.
 * @param n  Byte count; 0 leaves the buffer untouched.
 */
void bufPutn(Buf *b, const char *s, size_t n) {
    if (!n) return;
    bufReserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

/**
 * @brief Append a NUL-terminated string (the terminator itself is not stored).
 *
 * @param b  Buffer.
 * @param s  Source string.
 */
void bufPuts(Buf *b, const char *s) {
    bufPutn(b, s, strlen(s));
}

/**
 * @brief Append formatted text to the buffer.
 *
 * The length is computed first and the text written with the `va_copy`-ed copy,
 * because a `va_list` cannot be walked twice. `vsnprintf` receives `n + 1` bytes:
 * its terminator lands in the spare byte bufReserve reserved, is not counted in
 * `len`, and is overwritten by the next append.
 *
 * @param b    Buffer.
 * @param fmt  Format string.
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
 * @brief Treat the buffer as a C string: NUL-terminate it and return the first byte.
 *
 * The returned pointer is the buffer's own storage, not a copy. Any later append
 * invalidates it (it overwrites the terminator and may move the data to a new
 * block), so call this once the text is complete.
 *
 * @param b  Buffer.
 * @return   Pointer to the NUL-terminated contents.
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
 * @brief Initialize an empty vector.
 *
 * @param v         Vector to initialize.
 * @param a         Arena that provides the memory.
 * @param elemSize  Size of one element in bytes; passing 0 makes every element land
 *                  at the same address, so callers always pass sizeof.
 */
void vecInit(Vec *v, Arena *a, size_t elemSize) {
    v->arena = a;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elemSize = elemSize;
}

/**
 * @brief Double the capacity and move the live elements into a new block.
 *
 * An empty vector starts at 8 elements. Every element pointer handed out before
 * this call dangles: the result of vecPush or vecAt must not be used across the
 * next push on the same vector.
 *
 * @param v  Vector to grow.
 */
static void vecGrow(Vec *v) {
    size_t ncap = v->cap ? v->cap * 2 : 8;

    char *nd = (char *)arenaAlloc(v->arena, ncap * v->elemSize);
    if (v->len) memcpy(nd, v->data, v->len * v->elemSize);
    v->data = nd;
    v->cap = ncap;
    /* The old block is not given back: the arena owns it. A dynamic array living in
     * an arena therefore leaves garbage behind on every growth until the arena ends.
     * Irrelevant for a compiler that runs once and exits, but a cost to remember for
     * a long-running program. */
}

/**
 * @brief Append one **uninitialized** element and return its address.
 *
 * The pointer dangles at the next push (growth moves the elements), so read what
 * you need early.
 *
 * @param v  Vector.
 * @return   Address of the new element, aligned for the element type; the caller
 *           must write all `elemSize` bytes.
 */
void *vecPush(Vec *v) {
    if (v->len == v->cap) vecGrow(v);
    void *p = v->data + v->len * v->elemSize;
    v->len++;
    return p;
}

/**
 * @brief Return the address of element `i`.
 *
 * Bounds are not checked: an out-of-range index reads the memory after the
 * elements.
 *
 * @param v  Vector.
 * @param i  Index (zero-based, must be below the current length).
 * @return   Element address.
 */
void *vecAt(Vec *v, size_t i) {
    return v->data + i * v->elemSize;
}

/**
 * @brief Current element count.
 *
 * @param v  Vector.
 * @return   Number of elements.
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
 * @brief Initialize the diagnostic context.
 *
 * `warnCount` and `noWarn` are not set here: the driver assigns `noWarn`
 * immediately afterwards, and a caller that also cares about the warning count must
 * zero the context first.
 *
 * @param c       Context to initialize.
 * @param a       Arena.
 * @param path    Source path (appears in diagnostics; the built-in prelude uses
 *                PRELUDE_PATH).
 * @param src     Source text, kept so diagnostics can quote the line.
 * @param srcLen  Source length in bytes, excluding the final NUL.
 */
void ctxInit(Ctx *c, Arena *a, const char *path, const char *src, size_t srcLen) {
    /* Every flag starts false and every counter at zero, including the ones this function does
     * not name: a caller that only sets the options it was given would otherwise read an
     * indeterminate `warnCount` or `warnOwnedView` later. */
    memset(c, 0, sizeof *c);
    c->arena = a;
    c->path = path;
    c->src = src;
    c->srcLen = srcLen;
    c->hasError = false;
    c->diagCount = 0;
    c->diagOverflow = false;
}

/**
 * @brief Record an error: every error of a compilation is kept, up to EXTC_MAX_DIAGS.
 *
 * Reporting the next error instead of only the first one is what lets a reader fix
 * three independent mistakes in one pass. The cap exists because a file that is broken
 * everywhere produces one error per statement; past the cap the rest are dropped and
 * `diagOverflow` makes the renderer say so. Identical diagnostics at the same position
 * are recorded once: the same construct is often checked twice (an expression, then the
 * statement that holds it), and that repeat carries no new information.
 *
 * Nothing is printed here: ctxRenderDiag renders the text later, so the error is
 * independent of what is currently being written to which stream. The message and the
 * note are truncated to EXTC_MAXERR bytes.
 *
 * @param c     Context.
 * @param line  Source line (one-based); <= 0 means the error has no position.
 * @param col   Source column (one-based); <= 0 renders as column 1.
 * @param note  One explanation line printed under the message, or `NULL`.
 * @param fmt   Message format.
 */
void ctxError(Ctx *c, int line, int col, const char *note, const char *fmt, ...) {
    char msg[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    c->hasError = true;
    /* The same construct can be checked more than once; say it once. */
    for (int i = c->diagCount - 1; i >= 0 && i >= c->diagCount - 2; i--) {
        if (c->diags[i].line == line && c->diags[i].col == col &&
            strcmp(c->diags[i].msg, msg) == 0)
            return;
    }
    if (c->diagCount >= EXTC_MAX_DIAGS) {
        c->diagOverflow = true;
        return;
    }
    Diag *d = &c->diags[c->diagCount++];
    d->line = line;
    d->col = col;
    snprintf(d->msg, sizeof d->msg, "%s", msg);
    if (note) snprintf(d->note, sizeof d->note, "%s", note);
    else d->note[0] = '\0';
}

/* Set by `--diagnostics-json`; read by both diagnostic sinks. */
static bool g_diagJson = false;

void ctxSetDiagJson(bool on) { g_diagJson = on; }

/**
 * @brief Append `s` as a JSON string body (quotes and backslashes escaped).
 *
 * Only the characters that can appear in a diagnostic need escaping; a raw `\n` would break
 * the one-object-per-line framing, so it becomes `\n` like every other control character.
 *
 * @param out  Buffer receiving the escaped text (no surrounding quotes).
 * @param s    Text to escape; NULL writes nothing.
 */
static void diagJsonEscape(Buf *out, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  bufPuts(out, "\\\""); break;
        case '\\': bufPuts(out, "\\\\"); break;
        case '\n': bufPuts(out, "\\n"); break;
        case '\r': bufPuts(out, "\\r"); break;
        case '\t': bufPuts(out, "\\t"); break;
        default:
            if (*p < 0x20) bufPrintf(out, "\\u%04x", (unsigned)*p);
            else bufPutc(out, (char)*p);
        }
    }
}

/**
 * @brief Record a warning: **does not** set `hasError`, compilation continues and
 * the text goes to stderr at once.
 *
 * Same shape as an error. `-w` (`Ctx.noWarn`) returns before counting, so
 * suppressed warnings are not counted either. The source line is found by scanning
 * from the start of the file, so each warning costs one pass of text up to its own
 * line.
 *
 * @param c     Context.
 * @param line  Source line; past the end of the file only the header line is
 *              printed.
 * @param col   Column the caret points at (one-based); <= 0 is treated as 1.
 * @param note  One explanation line after the source line, or `NULL`.
 * @param fmt   Message format.
 */
void ctxWarn(Ctx *c, int line, int col, const char *note, const char *fmt, ...) {
    if (c->noWarn) return;
    c->warnCount++;

    char msg[EXTC_MAXERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (g_diagJson) {
        Buf j;
        bufInit(&j, c->arena);
        bufPrintf(&j, "{\"severity\":\"warning\",\"file\":\"");
        diagJsonEscape(&j, c->path);
        bufPrintf(&j, "\",\"line\":%d,\"col\":%d,\"message\":\"", line > 0 ? line : 1,
                  col > 0 ? col : 1);
        diagJsonEscape(&j, msg);
        bufPrintf(&j, "\",\"note\":\"");
        diagJsonEscape(&j, note);
        bufPuts(&j, "\"}\n");
        fputs(bufCstr(&j), stdout);
        return;
    }

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
 * @brief Render one recorded error: header, source line, caret.
 *
 * An error raised before parsing has no line number and renders as
 * `path: error: message`, with no source excerpt. The source line is found by
 * scanning from the start of the file; there are at most EXTC_MAX_DIAGS errors, so
 * the linear cost is bounded.
 *
 * @param c    Context.
 * @param out  Buffer that receives the text.
 * @param d    The diagnostic to render.
 */
static void renderOneDiag(Ctx *c, Buf *out, const Diag *d) {
    if (g_diagJson) {
        Buf j;
        bufInit(&j, c->arena);
        bufPrintf(&j, "{\"severity\":\"error\",\"file\":\"");
        diagJsonEscape(&j, c->path);
        bufPrintf(&j, "\",\"line\":%d,\"col\":%d,\"message\":\"", d->line > 0 ? d->line : 1,
                  d->col > 0 ? d->col : 1);
        diagJsonEscape(&j, d->msg);
        bufPrintf(&j, "\",\"note\":\"");
        diagJsonEscape(&j, d->note[0] ? d->note : NULL);
        bufPuts(&j, "\"}\n");
        fputs(bufCstr(&j), stdout);
        return;
    }
    if (d->line > 0)
        bufPrintf(out, "%s:%d:%d: error: %s\n", c->path, d->line, d->col > 0 ? d->col : 1, d->msg);
    else
        bufPrintf(out, "%s: error: %s\n", c->path, d->msg);

    /* Walk to the offending source line by counting newlines. */
    int line = 1;
    size_t i = 0;
    while (i < c->srcLen && line < d->line) {
        if (c->src[i] == '\n') line++;
        i++;
    }

    if (line == d->line) {
        size_t start = i;
        size_t end = start;
        while (end < c->srcLen && c->src[end] != '\n') end++;

        bufPuts(out, "  ");
        bufPutn(out, c->src + start, end - start);
        bufPutc(out, '\n');

        bufPuts(out, "  ");
        int col = d->col > 0 ? d->col : 1;
        for (int k = 1; k < col; k++) bufPutc(out, ' ');
        bufPuts(out, "^\n");
    }

    if (d->note[0]) {
        bufPrintf(out, "  note: %s\n", d->note);
    }
}

/**
 * @brief Render every recorded diagnostic to text, in the order they were found.
 *
 * A run that hit EXTC_MAX_DIAGS ends with one extra line saying so: leaving that out
 * would make a truncated list look like the complete one.
 *
 * @param c    Context.
 * @param out  Buffer that receives the text; the caller decides which stream it
 *             finally reaches.
 */
void ctxRenderDiag(Ctx *c, Buf *out) {
    for (int i = 0; i < c->diagCount; i++) renderOneDiag(c, out, &c->diags[i]);
    if (c->diagOverflow) {
        if (g_diagJson)
            fprintf(stdout,
                    "{\"severity\":\"error\",\"file\":\"%s\",\"line\":1,\"col\":1,"
                    "\"message\":\"too many errors; stopped after %d\",\"note\":\"\"}\n",
                    c->path, EXTC_MAX_DIAGS);
        else
            bufPrintf(out, "%s: error: too many errors; stopped after %d\n", c->path,
                      EXTC_MAX_DIAGS);
    }
}

/* ------------------------------------------------------- C identifier names */

/**
 * @brief Is this identifier a C keyword, one of `bool`/`true`/`false`, or the GNU
 * `asm`/`typeof` that gnu11 accepts by default?
 *
 * extC names must work as C names, so they have to avoid this vocabulary. Three
 * groups collide: C keywords (extC accepts `fn double(...)`, but the generated C
 * would not compile and the error would point into code the user never wrote),
 * `bool`/`true`/`false` from `<stdbool.h>` (macros in the generated C), and the GNU
 * `asm`/`typeof` that gnu11 lets through by default. Rule: a name in extC source is
 * never rewritten; only names in the generated C get a suffix.
 *
 * The table is NULL-terminated and scanned linearly; it is small enough that a
 * lookup structure would only add code.
 *
 * @param name  Identifier to test.
 * @return      `true` when the generated C must rename it (codegen adds a suffix).
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
 * @brief Read a whole file into the arena and NUL-terminate it.
 *
 * `n + 1` bytes are requested for the length reported by `ftell`; a short read ends
 * the buffer at the bytes actually read, and `outLen` reports that number.
 *
 * @param a       Arena that provides the memory.
 * @param path    File to read.
 * @param outLen  Receives the byte count, excluding the final NUL; may be `NULL`.
 * @return        The buffer, or `NULL` when the file cannot be opened, sought, or sized.
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
