/* 暴力 O(n²) 对拍器 —— 只用来在小规模上验证 CDQ 实现是对的 ✓
 * （正式负载跑 n=10^6，暴力跑不动 ⇒ 这是**一次性正确性锚点**，不是基准）
 *
 * 口径：对**每个原始点** i 数出 j（含重复）满足三坐标都 <= 的个数，再减 1（自己）。
 * 即 sum_i (k_i - 1) —— 跟 CDQ 版 `sum(cnt * ans)` 必须相等（重复点的答案相同）✓
 */
#include <stdint.h>
#include <stdio.h>

#ifndef N
#define N 2000
#endif
#ifndef V
#define V 60
#endif

typedef struct { int32_t a, b, c; } P;
static P p[N];

typedef struct { uint64_t state, inc; } pcg32;
static uint64_t pcgNext(pcg32 *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint64_t x = (((old >> 18) ^ old) >> 27) & 4294967295ULL;
    uint64_t rot = old >> 59;
    return ((x >> rot) | (x << ((32 - rot) & 31))) & 4294967295ULL;
}
static pcg32 pcgSeed(uint64_t seed, uint64_t stream) {
    pcg32 r; r.state = 0; r.inc = (stream << 1) | 1;
    (void)pcgNext(&r); r.state += seed; (void)pcgNext(&r);
    return r;
}
static uint64_t pcgBounded(pcg32 *r, uint64_t bound) {
    uint64_t t = (0 - bound) % bound, x = pcgNext(r);
    while (x < t) x = pcgNext(r);
    return x % bound;
}

int main(void) {
    pcg32 r = pcgSeed(20260922ULL, 54);
    for (int i = 0; i < N; i++) {
        p[i].a = (int32_t)pcgBounded(&r, V);
        p[i].b = (int32_t)pcgBounded(&r, V);
        p[i].c = (int32_t)pcgBounded(&r, V);
    }
    uint64_t sum = 0;
    for (int i = 0; i < N; i++) {
        int64_t k = 0;
        for (int j = 0; j < N; j++)
            if (p[j].a <= p[i].a && p[j].b <= p[i].b && p[j].c <= p[i].c) k++;
        sum += (uint64_t)(k - 1);
    }
    printf("brute sum = %llu\n", (unsigned long long)sum);
    return 0;
}
