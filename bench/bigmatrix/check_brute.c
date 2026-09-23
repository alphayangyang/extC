/* cdq brute force: O(n^2), used to prove the CDQ reference on the small input.
 * Same definition as src/cdq.c: per point, count the points that dominate it in
 * all three coordinates, itself excluded; sum over all points, and the maximum. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/fastio.h"

int main(void) {
    int64_t n = bm_i64();
    int32_t *a = malloc(sizeof(int32_t) * (size_t)n);
    int32_t *b = malloc(sizeof(int32_t) * (size_t)n);
    int32_t *c = malloc(sizeof(int32_t) * (size_t)n);
    for (int64_t i = 0; i < n; i++) { a[i] = (int32_t)bm_u64(); b[i] = (int32_t)bm_u64(); c[i] = (int32_t)bm_u64(); }
    uint64_t sum = 0;
    int64_t mx = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t cnt = 0;
        for (int64_t j = 0; j < n; j++)
            if (a[j] <= a[i] && b[j] <= b[i] && c[j] <= c[i]) cnt++;
        cnt -= 1;
        sum += (uint64_t)cnt;
        if (cnt > mx) mx = cnt;
    }
    printf("sum=%llu max=%lld\n", (unsigned long long)sum, (long long)mx);
    return 0;
}
