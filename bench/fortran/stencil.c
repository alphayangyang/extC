/* bench/fortran/stencil.c —— 2D 5 点 Jacobi 模板（f64，flat 行主序）。
 *
 * 与 stencil.extc / stencil.f90 同形状、同参数、同 stdout 字节；口径见 SPEC.md。
 * 用法：stencil [n] [iters]，缺省 64 10。输出：cs=<整数> n=<n>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    int64_t iters = argc > 2 ? toInt(argv[2], 10) : 10;
    int64_t cells = n * n;

    double *a = calloc((size_t)cells, sizeof(double));
    double *b = calloc((size_t)cells, sizeof(double));
    if (!a || !b) return 2;

    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++)
            a[i * n + j] = (double)((i * 7 + j * 13) % 17) / 17.0;

    for (int64_t it = 0; it < iters; it++) {
        for (int64_t r = 1; r < n - 1; r++)
            for (int64_t c = 1; c < n - 1; c++)
                b[r * n + c] = (a[(r - 1) * n + c] + a[(r + 1) * n + c]
                                + a[r * n + c - 1] + a[r * n + c + 1]) * 0.25;
        double *t = a;
        a = b;
        b = t;
    }

    double cs = 0.0;
    for (int64_t i = 0; i < cells; i++) cs += a[i];

    printf("cs=%lld n=%lld\n", (long long)(int64_t)(cs * 1000000.0), (long long)n);
    return 0;
}
