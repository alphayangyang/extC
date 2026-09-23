/* Shape 2: cdq -- C reference (3D dominance, CDQ + weight BIT).
 *
 * Read n, then n triples (a b c) with values in [1,10^6].
 *   1. sort by (a,b,c)   2. collapse duplicates, keeping cnt
 *   3. CDQ over b, BIT over c: ans[j] += cnt[i] for every i<j that dominates j
 *      (the BIT is indexed by c itself: the generator emits c >= 1, so no +1
 *       shift is needed, and the array stays V+1 wide.  The earlier +1 form put
 *       c = 10^6 at index 1000001, one past the end -- a silent out-of-bounds
 *       write in C that extC's bounds check caught at full scale.)
 *   4. f = ans + cnt (itself), reported answer = f - 1
 *   print sum = SUM cnt_i * ans_i (u64 wrap) and max = max ans_i
 * Verified against O(n^2) brute force by check_brute.c on the small input. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fastio.h"

#define V 1000000

typedef struct { int32_t a, b, c, cnt, ans; } Pt;

static Pt *p, *tmp;
static int32_t *bit;
static int64_t m;

static void bitAdd(int64_t i, int32_t v) {
    for (; i <= V; i += i & (-i)) bit[i] += v;
}
static int32_t bitSum(int64_t i) {
    int32_t s = 0;
    for (; i > 0; i -= i & (-i)) s += bit[i];
    return s;
}

static void cdq(int64_t l, int64_t r) {
    if (l >= r) return;
    int64_t mid = l + (r - l) / 2;
    cdq(l, mid);
    cdq(mid + 1, r);

    int64_t i = l, j = mid + 1;
    while (j <= r) {
        while (i <= mid && p[i].b <= p[j].b) { bitAdd(p[i].c, p[i].cnt);              /* c >= 1, so the BIT index is c itself */ i++; }
        p[j].ans += bitSum(p[j].c);
        j++;
    }
    for (int64_t t = l; t < i; t++) bitAdd(p[t].c, -p[t].cnt);       /* undo */

    i = l; j = mid + 1;
    int64_t k = l;
    while (i <= mid && j <= r) tmp[k++] = (p[i].b <= p[j].b) ? p[i++] : p[j++];
    while (i <= mid) tmp[k++] = p[i++];
    while (j <= r) tmp[k++] = p[j++];
    for (i = l; i <= r; i++) p[i] = tmp[i];
}

static int cmpPt(const void *x, const void *y) {
    const Pt *u = (const Pt *)x, *v = (const Pt *)y;
    if (u->a != v->a) return u->a < v->a ? -1 : 1;
    if (u->b != v->b) return u->b < v->b ? -1 : 1;
    if (u->c != v->c) return u->c < v->c ? -1 : 1;
    return 0;
}

int main(void) {
    int64_t n = bm_i64();
    Pt *raw = (Pt *)malloc(sizeof(Pt) * (size_t)n);
    for (int64_t i = 0; i < n; i++) {
        raw[i].a = (int32_t)bm_u64();
        raw[i].b = (int32_t)bm_u64();
        raw[i].c = (int32_t)bm_u64();
        raw[i].cnt = 1;
        raw[i].ans = 0;
    }
    qsort(raw, n, sizeof(Pt), cmpPt);

    p = (Pt *)malloc(sizeof(Pt) * (size_t)n);
    tmp = (Pt *)malloc(sizeof(Pt) * (size_t)n);
    m = 0;
    for (int64_t i = 0; i < n;) {
        int64_t j = i + 1;
        while (j < n && raw[j].a == raw[i].a && raw[j].b == raw[i].b && raw[j].c == raw[i].c) j++;
        p[m] = raw[i];
        p[m].cnt = (int32_t)(j - i);
        p[m].ans = 0;
        m++;
        i = j;
    }

    bit = (int32_t *)calloc((size_t)V + 1, sizeof(int32_t));
    cdq(0, m - 1);

    uint64_t sum = 0;
    int64_t mx = 0;
    for (int64_t i = 0; i < m; i++) {
        int64_t ans = (int64_t)p[i].ans + (int64_t)p[i].cnt - 1;
        sum += (uint64_t)p[i].cnt * (uint64_t)ans;
        if (ans > mx) mx = ans;
    }
    printf("sum=%llu max=%lld\n", (unsigned long long)sum, (long long)mx);
    return 0;
}
