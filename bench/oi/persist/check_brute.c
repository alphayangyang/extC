/* 主席树的**暴力对拍**：同一个 pcg32 序列，每个询问把区间抄出来排序取第 k 个。
 *
 * 为什么需要它：四个语言之间对拍只能证明"**它们一致**"，证明不了"**它们对**" ✗
 *   —— 四份实现抄的是同一个算法，算法错了会一起错 ✓
 * ⇒ 小规模（N=Q=2000）拿 O(n log n · q) 的暴力当**真值**，输出必须逐字节相同 ✓
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 2000
#endif
#ifndef Q
#define Q 2000
#endif
#ifndef V
#define V 2000
#endif

static int32_t A[N + 1];
static int32_t B[N + 1];

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
    (void)pcgNext(&r);
    r.state += seed;
    (void)pcgNext(&r);
    return r;
}
static uint64_t pcgBounded(pcg32 *r, uint64_t bound) {
    if (bound == 0) return 0;
    uint64_t threshold = (0 - bound) % bound;
    uint64_t x = pcgNext(r);
    while (x < threshold) x = pcgNext(r);
    return x % bound;
}

static int cmpI32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    pcg32 rng = pcgSeed(20260922ULL, 54);
    for (int64_t i = 1; i <= N; i++) A[i] = (int32_t)pcgBounded(&rng, V) + 1;

    uint64_t inSum = 0;
    for (int64_t i = 1; i <= N; i++) inSum = inSum * 1000003ULL + (uint64_t)A[i];

    uint64_t ansSum = 0;
    for (int64_t j = 0; j < Q; j++) {
        int64_t l = 1 + (int64_t)pcgBounded(&rng, N);
        int64_t r = l + (int64_t)pcgBounded(&rng, (uint64_t)(N - l + 1));
        int64_t k = 1 + (int64_t)pcgBounded(&rng, (uint64_t)(r - l + 1));
        int64_t len = r - l + 1;
        for (int64_t t = 0; t < len; t++) B[t] = A[l + t];
        qsort(B, (size_t)len, sizeof(int32_t), cmpI32);
        int32_t ans = B[k - 1];
        ansSum = ansSum * 1000009ULL + (uint64_t)ans;
    }

    printf("in    = %llu\n", (unsigned long long)inSum);
    printf("n     = %lld\n", (long long)N);
    printf("q     = %lld\n", (long long)Q);
    printf("nodes = -\n");          /* 暴力没有节点池（对拍时这一行不比）*/
    printf("ans   = %llu\n", (unsigned long long)ansSum);
    return 0;
}
