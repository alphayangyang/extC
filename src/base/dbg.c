/* Implementation of the debug assertions.
 *
 * The switch is the environment variable `EXTC_DBG`: any non-empty value other than
 * `0` turns it on. It is read once on first use and only a static variable is
 * consulted afterwards, so when it is off every assertion site costs one branch and
 * a release build needs no recompilation. */
#include "dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int g_on = -1;    /* -1 = not decided yet; 0 = off; 1 = on */

/**
 * @brief Whether the debug switch is on; the first call reads the environment and
 * later calls only consult the static variable.
 *
 * The switch is the environment variable `EXTC_DBG`: non-empty with a first
 * character other than `0` turns it on. When it is off every assertion site costs
 * one branch.
 *
 * @return  1 when on, 0 when off.
 */
static int dbgEnabled(void) {
    if (g_on < 0) {
        const char *e = getenv("EXTC_DBG");
        g_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_on;
}

/**
 * @brief Like `extcDbgNote`, but with formatted detail (which binding, which
 * value).
 *
 * @param file  Source file of the call site; `NULL` prints `?`.
 * @param line  Line of the call site.
 * @param fmt   Format string for the detail.
 */
void extcDbgNoteF(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: ", file ? file : "?", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/**
 * @brief Report a conservative path that is **known to be reached**; report only,
 * never abort.
 *
 * @param what  Description; `NULL` prints `?`.
 * @param file  Source file of the call site; `NULL` prints `?`.
 * @param line  Line of the call site.
 */
void extcDbgNote(const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: %s\n", file ? file : "?", line, what ? what : "?");
}

/**
 * @brief Like `extcDbgHit`, but with formatted detail; when the switch is off
 * return at once, and when it is on print and abort.
 *
 * @param kind  Either `"assert"` or `"fallback"`; `NULL` prints `dbg`.
 * @param file  Source file of the call site; `NULL` prints `?`.
 * @param line  Line of the call site.
 * @param fmt   Format string for the detail.
 */
void extcDbgHitF(const char *kind, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!dbgEnabled()) return;
    fprintf(stderr, "[%s] %s:%d: ", kind ? kind : "dbg", file ? file : "?", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

/**
 * @brief Whether the debug switch is on (`EXTC_DBG=1`).
 *
 * @return  `true` when it is on.
 */
bool extcDbgOn(void) { return dbgEnabled(); }

/**
 * @brief Report one assertion or fallback hit; when the switch is off return at
 * once, and when it is on print and abort.
 *
 * @param kind  Either `"assert"` or `"fallback"`; `NULL` prints `dbg`.
 * @param what  The asserted expression, or a description of which fallback was
 *              reached; `NULL` prints `?`.
 * @param file  Source file of the call site; `NULL` prints `?`.
 * @param line  Line of the call site.
 */
void extcDbgHit(const char *kind, const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    /* The `[assert]` / `[fallback]` prefix is what the gates and `check.sh` look
     * for: a red section comes from this compiler **aborting here**, not from some
     * string appearing in the output. Reaching a fallback is itself the signal. */
    fprintf(stderr, "[%s] %s:%d: %s\n", kind ? kind : "dbg", file ? file : "?", line, what ? what : "?");
    fflush(stderr);
    abort();
}
