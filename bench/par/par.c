/* bench/par：手写 C 的顺序参照臂（与 seq.extc 同算法、同输出）。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t toInt(const char *s, int64_t dflt) {
    if (!s || !*s) return dflt;
    int64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return dflt;
        v = v * 10 + (*p - '0');
    }
    return v;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 ? toInt(argv[1], 64) : 64;
    int64_t iters = argc > 2 ? toInt(argv[2], 48) : 48;
    double *in = calloc((size_t)n, sizeof(double));
    double *out = calloc((size_t)n, sizeof(double));
    if (!in || !out) return 1;
    for (int64_t i = 0; i < n; i++)
        in[i] = (double)((i * 7 + 3) % 17) / 17.0;
    for (int64_t i = 0; i < n; i++) {
        double x = in[i], y = x;
        for (int64_t k = 0; k < iters; k++) {
            y = y * 1.0000001 + x * 0.5 + 1.0;
            x = x * 1.0000003;
        }
        out[i] = y;
    }
    double cs = 0.0;
    for (int64_t i = 0; i < n; i++) cs += out[i];
    printf("cs=%lld n=%lld\n", (long long)(cs * 1000000.0), (long long)n);
    free(in);
    free(out);
    return 0;
}
