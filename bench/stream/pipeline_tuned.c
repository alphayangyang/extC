/* bench/stream —— "如果代码生成这样发"的手写 C：restrict + 64B 对齐分配 + ivdep
 * (+ mode 5 软件预取)。同一语义，只改 C 层面的假设，用来量 extC 发射的 C 还有多少空间。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <immintrin.h>

static int64_t argOr(int argc, char **argv, int i, int64_t dflt) {
    if (argc <= i) return dflt;
    char *end = NULL;
    long v = strtol(argv[i], &end, 10);
    return end == argv[i] ? dflt : (int64_t)v;
}

static double *alloc64(size_t bytes) {
    size_t rounded = (bytes + 63u) & ~(size_t)63u;
    return (double *)aligned_alloc(64, rounded);
}

/* mode 6：非临时写（`_mm256_stream_pd`）—— out 写完立刻从寄存器求和、不再读回，
 * 所以省掉普通 store 的写分配读（RFO）。用 target 属性保证不依赖整份二进制开 AVX2。 */
__attribute__((target("avx2")))
static void fused_nt_avx2(const double *restrict src, double *restrict dst, int64_t n, double *cs) {
    __m256d k15 = _mm256_set1_pd(1.5), k025 = _mm256_set1_pd(0.25), k1 = _mm256_set1_pd(1.0);
    int64_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d x = _mm256_loadu_pd(&src[i]);
        __m256d y = _mm256_add_pd(_mm256_mul_pd(x, k15), k025);
        __m256d o = _mm256_add_pd(_mm256_mul_pd(y, y), k1);
        _mm256_stream_pd(&dst[i], o);
        double v[4];
        _mm256_storeu_pd(v, o);
        *cs += v[0]; *cs += v[1]; *cs += v[2]; *cs += v[3];
    }
    for (; i < n; i++) {
        double y = src[i] * 1.5 + 0.25;
        double o = y * y + 1.0;
        dst[i] = o;
        *cs += o;
    }
    _mm_sfence();
}

int main(int argc, char **argv) {
    int64_t n = argOr(argc, argv, 1, 1024);
    int64_t mode = argOr(argc, argv, 2, 2);
    int64_t reps = argOr(argc, argv, 3, 1);
    size_t bytes = (size_t)n * sizeof(double);
    double *in = alloc64(bytes), *out = alloc64(bytes);
    if (!in || !out) return 1;
    for (int64_t i = 0; i < n; i++) in[i] = (double)((i * 7 + 3) % 17) / 17.0;
    double cs = 0.0;
    double *restrict src = (double *)__builtin_assume_aligned(in, 64);
    double *restrict dst = (double *)__builtin_assume_aligned(out, 64);
    for (int64_t rep = 0; rep < reps; rep++) {
        if (mode == 3) {
            /* 直接累加进 cs：与参考实现严格同序（先攒局部量再加会改变 f64 结合顺序）。 */
#pragma GCC ivdep
            for (int64_t i = 0; i < n; i++) {
                double y = src[i] * 1.5 + 0.25;
                cs += y * y + 1.0;
            }
        } else if (mode == 6) {
            fused_nt_avx2(src, dst, n, &cs);
        } else if (mode == 5) {
#pragma GCC ivdep
            for (int64_t i = 0; i < n; i++) {
                __builtin_prefetch(&src[i + 16], 0, 0);
                double y = src[i] * 1.5 + 0.25;
                dst[i] = y * y + 1.0;
            }
            for (int64_t i = 0; i < n; i++) cs += dst[i];
        } else {
#pragma GCC ivdep
            for (int64_t i = 0; i < n; i++) {
                double y = src[i] * 1.5 + 0.25;
                dst[i] = y * y + 1.0;
            }
            for (int64_t i = 0; i < n; i++) cs += dst[i];
        }
    }
    printf("cs=%lld n=%lld\n", (long long)(cs * 1000000.0), (long long)n);
    free(in); free(out);
    return 0;
}
