/**
 * @file plate.c
 * @brief `extc_viewOf`: turns "a byte pointer + a length" into `mut slice<u8>`.
 *
 * The door exists for three reasons: a view cannot be built, a generic type has no
 * literal, and `ref void` -> `ref T` is not opened. Semantically it is exactly that
 * two-word struct, with **no check**: who supplied the pointer, where it points, and how
 * large the length is are the business of the calling layer (the plate: one range check in
 * `plate::holds`; the C boundary: the signature at the declaration).
 */
#include "plate.h"

/**
 * @brief Emit `extc_viewOf`, the door that turns "byte pointer + length" into
 * `mut slice<u8>`.
 *
 * Semantically it is that two-word struct, with **no check**: who supplied the pointer,
 * where it points, and how large the length is are the business of the calling layer (the
 * plate does one range check; the C boundary trusts the signature at the declaration).
 *
 * @param a    Unused: the runtime is plain text and has no state that needs an arena.
 * @param out  Output buffer the generated code is appended to.
 */
void plateEmitViewOf(Arena *a, Buf *out) {
    (void)a;
    /* The field names of `slice_u8` match `struct slice<T> { data, len }` in the prelude;
     * the type is defined in the struct section (this block is emitted after the prototype
     * pool and before the function bodies), so it can be used directly here. */
    bufPuts(out,
        "\n/* ---- plate (`std::sys::heap`) ----\n"
        " * A view over a pointer and a length. The language cannot build one -- a view's type is a\n"
        " * generic instance, so no struct literal spells it, and `ref void` -> `ref T` is refused on\n"
        " * purpose -- so the privileged layer owns this one three-line door.\n"
        " */\n"
        "EXTC_UNUSED static slice_u8 extc_viewOf(uint8_t *p, int64_t n) {\n"
        "    slice_u8 v = { p, n };\n"
        "    return v;\n"
        "}\n\n");
}

/**
 * @brief Emit the bulk-copy door (`extc_memCopy`): returns the number of bytes actually
 * copied, and `n <= 0` copies nothing.
 *
 * A `memcpy` declaration would clash with `<string.h>` (the generated file includes it),
 * so it too has to leave through this integer door. It lives in the **plate runtime
 * block**: that block is triggered only by the plate's own declaration, so it cannot pull
 * along the artifacts of programs that declare the byte-search door only.
 *
 * @param a    Unused: the runtime is plain text and has no state that needs an arena.
 * @param out  Output buffer the generated code is appended to.
 */
void plateEmitMemCopy(Arena *a, Buf *out) {
    (void)a;
    /* The second item behind the same door: the **bulk copy**. A `memcpy` declaration would
     * clash with `<string.h>` (the generated file includes it), so it too has to leave
     * through this door. It lives in the **plate runtime block** rather than the byte-search
     * block: that block's trigger is the declaration of `extern!("extc-mem")`, and every user
     * of `std::string` declares it, so adding anything there would change a large amount of
     * unrelated output; the plate's trigger is its own declaration, so anything added here
     * leaves other programs untouched. */
    bufPuts(out,
        "/* Bulk copy (`memcpy` behind a door extC can declare). Returns how many bytes it copied;\n"
        " * `n <= 0` copies nothing. The plate\'s `copyIn`/`copyOut` want a **bulk** copy: a byte loop\n"
        " * would pay one bounds check per byte, which is the thing this door exists to avoid. */\n"
        /* **Not static**: its `extern!` declaration emits a non-static prototype (`extc_memEq`
         * is the same), and gcc reports "static declaration follows non-static declaration"
         * when the two sides disagree. */
        "int64_t extc_memCopy(uint8_t *dst, uint8_t *src, int64_t n) {\n"
        "    if (n <= 0) return 0;\n"
        "    memcpy((void *)dst, (const void *)src, (size_t)n);\n"
        "    return n;\n"
        "}\n\n");
}
