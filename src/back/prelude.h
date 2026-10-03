/* The bundled extC prelude.
 *
 * Its source lives in stdlib/prelude.extc and is turned into build/prelude_data.c, a
 * plain byte array, by tools/embed.c at build time; that array is linked into the
 * compiler. The compiler therefore stays a single executable and needs no external
 * path at run time.
 */
#ifndef EXTC_PRELUDE_H
#define EXTC_PRELUDE_H

#include <stddef.h>

/* Return the prelude source text. It is not NUL-terminated: the length comes back
 * through *len, which is why the text is handed over as bytes rather than a string. */
const char *preludeSource(size_t *len);

/* The file name shown in diagnostics that point into the prelude. */
#define PRELUDE_PATH "<extc prelude>"

#endif /* EXTC_PRELUDE_H */
