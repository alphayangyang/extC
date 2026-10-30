/* bench/fusion：S1 的手写融合参照臂（与 pipeline.extc 同一份工作，人肉把两个循环合成一个）。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int64_t n = argc > 1 ? atoll(argv[1]) : 1048576;
    int64_t iters = argc > 2 ? atoll(argv[2]) : 8;
    double *a = malloc(sizeof(double) * (size_t)n);
    double *res = malloc(sizeof(double) * (size_t)n);
    if (!a || !res) return 1;
    for (int64_t i = 0; i < n; i++) {
        a[i] = (double)((i * 7 + 3) % 17) / 17.0;
        res[i] = 0.0;
    }
    for (int64_t k = 0; k < iters; k++) {
        /* 融合后的唯一一趟：A 与 B 合一，临时视图不存在。 */
        for (int64_t i = 0; i < n; i++) res[i] = (a[i] * 2.0 + 1.0) * 0.5;
    }
    double cs = 0.0;
    for (int64_t i = 0; i < n; i++) cs = cs + res[i];
    printf("cs=%lld n=%lld\n", (long long)(cs * 1000.0), (long long)n);
    return 0;
}
