/* Shape 4: mandel -- C reference.
 * Read `w h maxiter`; same loop as bench/heavy/mb.c (double, escape at |z|>2). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fastio.h"

int main(void) {
    int64_t w = bm_i64(), h = bm_i64(), maxiter = bm_i64();
    uint8_t *img = (uint8_t *)malloc((size_t)(w * h));
    int64_t acc = 0;
    for (int64_t y = 0; y < h; y++)
        for (int64_t x = 0; x < w; x++) {
            double cr = (double)x / (double)w * 3.5 - 2.5;
            double ci = (double)y / (double)h * 2.0 - 1.0;
            double zr = 0, zi = 0;
            int64_t it = 0;
            while (it < maxiter) {
                double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                if (zr * zr + zi * zi > 4.0) break;
                it++;
            }
            img[y * w + x] = (uint8_t)(it & 255);
            acc += it;
        }
    printf("mandel=%lld img0=%d\n", (long long)acc, img[0]);
    return 0;
}
