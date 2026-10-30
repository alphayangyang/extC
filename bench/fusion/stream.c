/* bench/fusion：S3 的手写融合参照臂（元素链 + 归约合成一个流式循环）。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int64_t n = argc > 1 ? atoll(argv[1]) : 1048576;
    int64_t iters = argc > 2 ? atoll(argv[2]) : 8;
    double *a = malloc(sizeof(double) * (size_t)n);
    if (!a) return 1;
    for (int64_t i = 0; i < n; i++) a[i] = (double)((i * 7 + 3) % 17) / 17.0;
    double cs = 0.0;
    for (int64_t k = 0; k < iters; k++) {
        /* tmp 与 tv2 都仅以"同下标"被下一段使用 ⇒ 一个循环里连着算、顺手累加。 */
        for (int64_t i = 0; i < n; i++) {
            double tmp = a[i] * 2.0 + 1.0;
            double tv2 = tmp * 0.5;
            cs = cs + tv2;
        }
    }
    printf("cs=%lld n=%lld\n", (long long)(cs * 1000.0), (long long)n);
    return 0;
}
