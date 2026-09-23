/* Fast stdin reader shared by the C references (and a template for the other
 * languages: every one of them reads the same text with its own fast parser).
 *
 * NOT scanf: the shapes feed 10^7 numbers and scanf would dominate the run,
 * turning every shape into a measurement of the C library instead. */
#ifndef BM_FASTIO_H
#define BM_FASTIO_H
#include <stdint.h>
#include <stdio.h>

static unsigned char bm_buf[1 << 20];
static size_t bm_len = 0, bm_pos = 0;

static inline int bm_get(void) {
    if (bm_pos >= bm_len) {
        bm_len = fread(bm_buf, 1, sizeof bm_buf, stdin);
        bm_pos = 0;
        if (bm_len == 0) return -1;
    }
    return bm_buf[bm_pos++];
}

static inline int64_t bm_i64(void) {
    int c = bm_get(), neg = 0;
    int64_t v = 0;
    while (c == ' ' || c == '\n' || c == '\t' || c == '\r') c = bm_get();
    if (c == '-') { neg = 1; c = bm_get(); }
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = bm_get(); }
    return neg ? -v : v;
}

static inline uint64_t bm_u64(void) {
    int c = bm_get();
    uint64_t v = 0;
    while (c < '0' || c > '9') { if (c < 0) return 0; c = bm_get(); }
    while (c >= '0' && c <= '9') { v = v * 10 + (uint64_t)(c - '0'); c = bm_get(); }
    return v;
}
#endif
