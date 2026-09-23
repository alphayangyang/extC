/* Shape 1: radix -- C reference.
 * Read n and n u32 values from stdin, LSD radix sort (4 passes x 8 bits),
 * print sorted/first/last.  Same shape as bench/heavy/rs.c, data from stdin. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fastio.h"

static uint32_t *a, *b;
static int64_t cnt[256];

int main(void) {
    int64_t n = bm_i64();
    a = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
    b = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
    for (int64_t i = 0; i < n; i++) a[i] = (uint32_t)bm_u64();

    for (int pass = 0; pass < 4; pass++) {
        uint64_t sh = (uint64_t)(pass * 8);
        for (int k = 0; k < 256; k++) cnt[k] = 0;
        for (int64_t i = 0; i < n; i++) cnt[(a[i] >> sh) & 255]++;
        int64_t sum = 0;
        for (int k = 0; k < 256; k++) { int64_t c = cnt[k]; cnt[k] = sum; sum += c; }
        for (int64_t i = 0; i < n; i++) b[cnt[(a[i] >> sh) & 255]++] = a[i];
        for (int64_t i = 0; i < n; i++) a[i] = b[i];
    }

    int ok = 1;
    for (int64_t i = 1; i < n; i++) if (a[i - 1] > a[i]) { ok = 0; break; }
    printf("sorted=%d first=%u last=%u\n", ok, a[0], a[n - 1]);
    return 0;
}
