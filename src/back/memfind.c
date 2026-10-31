/**
 * @file memfind.c
 * @brief The byte-search and whole-block-compare runtime: `extc_memFind` and
 * `extc_memEq`.
 *
 * Why this layer exists: a pure extC `string::find` is a Two-Way search that walks
 * `hay[i]` byte by byte, and every indexing operation goes through a bounds check -- the
 * algorithm is fine, it loses on "one check per byte"; libc's `memchr`/`memcmp` are
 * vectorized and have no per-byte check.
 *
 * `memmem` is libc's Two-Way (glibc goes through `__memmem`: a short pattern first uses
 * `memchr` to find the first byte, a long one uses a period shift), with the same semantics
 * as the pure extC implementation: it returns the **leftmost** match.
 *
 * ## Two disciplines
 *
 * 1. The criteria do not change -- offset, `-1`, and empty pattern `0`, none missing; a
 *    self-overlapping pattern (`aa` in `aaaa`) must be 0.
 * 2. Keep it portable -- `memmem` is a GNU/BSD extension, so non-GNU platforms take the
 *    naive `memchr` + `memcmp` fallback under `#else` below (slower, same semantics).
 *
 * ## Why the compare is behind the same door
 *
 * `extc_memEq` is the second exit of the same door: extC can write neither the `const void
 * *` nor the `size_t` of C's `memcmp` signature, and **a hand-written declaration would
 * collide with `<string.h>`** (the generated file includes it and gcc reports
 * `conflicting types for 'memcmp'`). So it is sealed behind this integer exit.
 */

#include "memfind.h"

/**
 * @brief Emit the byte-search and block-compare runtime (`extc_memFind` /
 * `extc_memEq`).
 *
 * Goes through libc's `memmem`/`memcmp` instead of a byte-by-byte extC implementation: the
 * semantics are unchanged (leftmost match, empty pattern returns 0, offset returns -1), but
 * each byte saves a bounds check.
 *
 * @param a    Unused: the runtime is plain text and has no state that needs an arena.
 * @param out  Output buffer the generated code is appended to.
 */
void memfindEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    /* First literal: the header comment plus `extc_memFind`. Kept separate because C99
     * only guarantees 4095-character literals (`-Woverlength-strings`). */
    bufPuts(out,
        "\n/* ---- byte search (`std::sys::mem`) ----\n"
        " * `memmem` under a door the language can declare: extC has no `const` and no `size_t`,\n"
        " * and the C library's own prototypes use both, so a hand-written declaration of\n"
        " * `memmem`/`memcmp` cannot be made to agree with <string.h>. Only integers and one\n"
        " * pointer cross this boundary.\n"
        " *\n"
        " * The parameter type is `uint8_t *`, not `const uint8_t *`: the prototype the\n"
        " * `extern!(\"extc-mem\")` declaration produces is `uint8_t *`, and a definition that\n"
        " * disagreed with it would be a conflicting-declaration error in the generated C. The\n"
        " * body reads both buffers and writes neither.\n"
        " *\n"
        " * `memmem` returns the LEFTMOST match, which is the promise `string::find` makes; the\n"
        " * fallback below keeps that too (scan with `memchr`, verify with `memcmp`, advance one\n"
        " * byte). */\n"
        "#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)\n"
        "#define EXTC_HAVE_MEMMEM 1\n"
        "/* `memmem` is a GNU/BSD extension: under `-std=c11` (strict ANSI) even glibc's <string.h>\n"
        " * leaves it undeclared. <string.h> has been read long before this block, so the\n"
        " * declaration is written out here instead of turning on `_GNU_SOURCE` -- a feature macro\n"
        " * is read by every other header in the translation unit, and changing how the whole file\n"
        " * is compiled to get one function is the wrong price. The signature is the one glibc,\n"
        " * musl and the BSDs agree on, so a translation unit that defines `_GNU_SOURCE` itself\n"
        " * sees the same declaration twice and stays valid. */\n"
        "extern void *memmem(const void *haystack, size_t haystacklen,\n"
        "                    const void *needle, size_t needlelen);\n"
        "#endif\n"
        "int64_t extc_memFind(uint8_t *hay, int64_t hayLen, uint8_t *needle, int64_t needleLen) {\n"
        "    /* The empty pattern matches at offset 0 -- and this is asked BEFORE either buffer is\n"
        "     * touched, so a zero-length slice with a null payload is a legal argument. */\n"
        "    if (needleLen <= 0) return 0;\n"
        "    if (hayLen < needleLen) return -1;   /* covers the empty haystack as well */\n"
        "#ifdef EXTC_HAVE_MEMMEM\n"
        "    {\n"
        "        const uint8_t *p = (const uint8_t *)memmem((const void *)hay, (size_t)hayLen,\n"
        "                                                  (const void *)needle, (size_t)needleLen);\n"
        "        return p ? (int64_t)(p - hay) : -1;\n"
        "    }\n"
        "#else\n"
        "    {\n"
        "        const uint8_t *end = hay + (hayLen - needleLen);\n"
        "        const uint8_t *p = hay;\n"
        "        for (;;) {\n"
        "            const uint8_t *q = (const uint8_t *)memchr(p, (int)needle[0],\n"
        "                                                       (size_t)(end - p) + 1);\n"
        "            if (!q) return -1;\n"
        "            if (memcmp(q, needle, (size_t)needleLen) == 0) return (int64_t)(q - hay);\n"
        "            p = q + 1;\n"
        "        }\n"
        "    }\n"
        "#endif\n"
        "}\n");
    /* Second literal: `extc_memEq`. */
    bufPuts(out,
        "/* `memcmp` behind the same door: 1 = the `n` bytes are equal, 0 = they differ. `n <= 0`\n"
        " * is equal by definition, so a caller may pass a zero-length slice's payload. */\n"
        "int64_t extc_memEq(uint8_t *a, uint8_t *b, int64_t n) {\n"
        "    if (n <= 0) return 1;\n"
        "    return (int64_t)(memcmp((const void *)a, (const void *)b, (size_t)n) == 0);\n"
        "}\n");
}
