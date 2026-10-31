/**
 * @file prelude.h
 * @brief The built-in extC prelude.
 *
 * Its source lives in stdlib/prelude.extc; at build time tools/embed.c turns it into a
 * plain byte array in build/prelude_data.c, which is linked into the compiler. The
 * compiler therefore stays a single executable and needs no external path at runtime.
 */
#ifndef EXTC_PRELUDE_H
#define EXTC_PRELUDE_H

#include <stddef.h>

/**
 * @brief Return the prelude source text.
 *
 * The text is **not NUL-terminated**: the length is returned through `*len`, so it is
 * handed out as bytes rather than as a string. It lives in static storage for the whole
 * run, so the caller neither frees it nor relies on a terminator.
 *
 * @param len  Receives the byte length.
 * @return     The byte array linked into the compiler.
 */
const char *preludeSource(size_t *len);

/* The file name shown when a diagnostic points into the prelude. */
#define PRELUDE_PATH "<extc prelude>"

#endif /* EXTC_PRELUDE_H */
