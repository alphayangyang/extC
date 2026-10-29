/* bench/stream —— 手写 C 的同一条管线（口径参照：不检查下标）。
 * mode 1 materialize / 2 fused / 3 stream，argv[3] = reps，语义与 pipeline.extc 逐位相同。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int64_t argOr(int argc, char **argv, int i, int64_t dflt) {
    if (argc <= i) return dflt;
    char *end = NULL;
    long v = strtol(argv[i], &end, 10);
    return end == argv[i] ? dflt : (int64_t)v;
}

int main(int argc, char **argv) {
    int64_t n = argOr(argc, argv, 1, 1024);
    int64_t mode = argOr(argc, argv, 2, 1);
    int64_t reps = argOr(argc, argv, 3, 1);
    double *in = (double *)malloc((size_t)n * sizeof(double));
    double *tmp = (double *)malloc((size_t)n * sizeof(double));
    double *out = (double *)malloc((size_t)n * sizeof(double));
    if (!in || !tmp || !out) return 1;
    for (int64_t i = 0; i < n; i++) in[i] = (double)((i * 7 + 3) % 17) / 17.0;
    double cs = 0.0;
    for (int64_t rep = 0; rep < reps; rep++) {
        if (mode == 1) {
            for (int64_t i = 0; i < n; i++) tmp[i] = in[i] * 1.5 + 0.25;
            for (int64_t i = 0; i < n; i++) out[i] = tmp[i] * tmp[i] + 1.0;
            for (int64_t i = 0; i < n; i++) cs += out[i];
        } else if (mode == 2) {
            for (int64_t i = 0; i < n; i++) {
                double y = in[i] * 1.5 + 0.25;
                out[i] = y * y + 1.0;
            }
            for (int64_t i = 0; i < n; i++) cs += out[i];
        } else {
            for (int64_t i = 0; i < n; i++) {
                double y = in[i] * 1.5 + 0.25;
                cs += y * y + 1.0;
            }
        }
    }
    printf("cs=%lld n=%lld\n", (long long)(cs * 1000000.0), (long long)n);
    free(in); free(tmp); free(out);
    return 0;
}
