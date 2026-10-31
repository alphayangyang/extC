/**
 * @file dbg.h
 * @brief Internal debug assertions: a runtime switch instead of conditional
 * compilation.
 *
 * The point is to expose the places that "should never be reached by design, but a
 * reader of that line cannot tell the polarity". Two rules:
 *   1. **Invariants** use `EXTC_DBG_ASSERT`: a violation is a bug in the compiler
 *      itself;
 *   2. **Fallback paths** use `EXTC_DBG_FALLBACK`: budget exhaustion, a source
 *      cycle, a full table, and the like.
 *
 * The switch is a **runtime** one (`EXTC_DBG=1`), so one binary serves both as a
 * release build and as an assertion build, and no conditional compilation appears in
 * this header. When it is off, every site costs one branch (`extcDbgHit` returns at
 * once) and neither prints nor aborts.
 */
#ifndef EXTC_DBG_H

#define EXTC_DBG_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Whether the debug switch is on (`EXTC_DBG=1`).
 * @return `true` when it is on.
 */
bool extcDbgOn(void);

/**
 * @brief Report one assertion or fallback hit; when the switch is off return at
 * once, and when it is on print and abort.
 *
 * @param kind  Either `"assert"` or `"fallback"`.
 * @param what  The asserted expression, or a description of which fallback was
 *              reached.
 * @param file  Source file of the call site.
 * @param line  Line of the call site.
 */
void extcDbgHit(const char *kind, const char *what, const char *file, int line);

/**
 * @brief Like `extcDbgHit`, but with formatted detail (which binding, which
 * value).
 *
 * @param kind   Either `"assert"` or `"fallback"`.
 * @param file   Source file of the call site.
 * @param line   Line of the call site.
 * @param fmt    Format string for the detail.
 */
void extcDbgHitF(const char *kind, const char *file, int line, const char *fmt, ...);

/**
 * @brief Report a conservative path that is **known to be reached**; report only,
 * never abort.
 *
 * @param what  Description.
 * @param file  Source file of the call site.
 * @param line  Line of the call site.
 */
void extcDbgNote(const char *what, const char *file, int line);

/**
 * @brief Like `extcDbgNote`, but with formatted detail.
 *
 * @param file   Source file of the call site.
 * @param line   Line of the call site.
 * @param fmt    Format string for the detail.
 */
void extcDbgNoteF(const char *file, int line, const char *fmt, ...);

/* Invariant: a violation is a bug in the compiler itself. */
#define EXTC_DBG_ASSERT(cond)          ((cond) ? (void)0 : extcDbgHit("assert", #cond, __FILE__, __LINE__))
#define EXTC_DBG_ASSERT_MSG(cond, msg) ((cond) ? (void)0 : extcDbgHit("assert", (msg), __FILE__, __LINE__))

/* Same, with formatted detail. */
#define EXTC_DBG_ASSERT_MSGF(cond, ...) \
    ((cond) ? (void)0 : extcDbgHitF("assert", __FILE__, __LINE__, __VA_ARGS__))

/* Fallback: should not be reached by design, and is exposed at once (with the
 * switch on) when it is. */
#define EXTC_DBG_FALLBACK(what)        extcDbgHit("fallback", (what), __FILE__, __LINE__)

/* A conservative path that **is known to be reached** (for example a recursion
 * budget that runs out of lines): report only, never abort. Its value is that it is
 * **countable**: a change in the count means the shape of the analysis changed. */
#define EXTC_DBG_NOTE(what)            extcDbgNote((what), __FILE__, __LINE__)

/* Same, with formatted detail (for example which form reached the fallback). */
#define EXTC_DBG_NOTEF(...)            extcDbgNoteF(__FILE__, __LINE__, __VA_ARGS__)

#endif /* EXTC_DBG_H */
