/* 三维偏序（CDQ 分治 + 树状数组）—— C++ 实现
 *
 * 跟 `p3810.c` / `p3810.extc` / `p3810.rs` **同一套算法**。
 *
 * ⚠️ 排序这一格是本横评里**唯一有意分组的**地方：
 *   · 主组（`USE_STD_SORT` 未定义）⇒ 四个语言**都手写同一套归并排序**（苹果对苹果）
 *   · 对照组（`-DUSE_STD_SORT`）⇒ 各语言用它自己的"std 写法"：
 *       C `qsort` · C++ `std::sort` · Rust `sort_unstable_by`
 *       （extC **没有 std 排序** ⇒ 它只在主组里 ✓）
 *
 * 数组用全局（80MB×2，栈上放不下）—— 跟 C/extC 版一致 ✓
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef N
#define N 4000000
#endif
#ifndef V
#define V 1000000
#endif

struct Pt { int32_t a, b, c, cnt, ans; };
static_assert(sizeof(Pt) == 20, "Pt 要保持 20 字节");

static Pt p[N], tmp[N];
static int32_t bit[V + 1];

/* ---------------- pcg32（跟 extC prelude 里那份逐位一致）---------------- */
struct Pcg32 { uint64_t state, inc; };

static inline uint64_t pcgNext(Pcg32 *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint64_t x = (((old >> 18) ^ old) >> 27) & 4294967295ULL;
    uint64_t rot = old >> 59;
    return ((x >> rot) | (x << ((32 - rot) & 31))) & 4294967295ULL;
}
static Pcg32 pcgSeed(uint64_t seed, uint64_t stream) {
    Pcg32 r{0, (stream << 1) | 1};
    (void)pcgNext(&r);
    r.state += seed;
    (void)pcgNext(&r);
    return r;
}
static uint64_t pcgBounded(Pcg32 *r, uint64_t bound) {
    if (bound == 0) return 0;
    uint64_t threshold = (0 - bound) % bound;
    uint64_t x = pcgNext(r);
    while (x < threshold) x = pcgNext(r);
    return x % bound;
}

/* ---------------- 权值树状数组（1-indexed：调用点一律 c+1）---------------- */
static inline void bitAdd(int32_t i, int32_t v) {
    for (; i <= V; i += i & (-i)) bit[i] += v;
}
static inline int32_t bitSum(int32_t i) {
    int32_t s = 0;
    for (; i > 0; i -= i & (-i)) s += bit[i];
    return s;
}

/* ---------------- CDQ ---------------- */
static void cdq(int32_t l, int32_t r) {
    if (l >= r) return;
    int32_t mid = l + (r - l) / 2;
    cdq(l, mid);
    cdq(mid + 1, r);

    int32_t i = l, j = mid + 1;
    while (j <= r) {
        while (i <= mid && p[i].b <= p[j].b) { bitAdd(p[i].c + 1, p[i].cnt); i++; }
        p[j].ans += bitSum(p[j].c + 1);
        j++;
    }
    for (int32_t t = l; t < i; t++) bitAdd(p[t].c + 1, -p[t].cnt);

    i = l; j = mid + 1;
    int32_t k = l;
    while (i <= mid && j <= r) tmp[k++] = (p[i].b <= p[j].b) ? p[i++] : p[j++];
    while (i <= mid) tmp[k++] = p[i++];
    while (j <= r)   tmp[k++] = p[j++];
    for (i = l; i <= r; i++) p[i] = tmp[i];
}

/* ---------------- 排序 ---------------- */
static inline bool lessPt(const Pt &x, const Pt &y) {
    if (x.a != y.a) return x.a < y.a;
    if (x.b != y.b) return x.b < y.b;
    return x.c < y.c;
}

static void mergeSort(Pt *src, Pt *buf, int32_t lo, int32_t hi) {
    if (hi - lo < 2) return;
    int32_t mid = lo + (hi - lo) / 2;
    mergeSort(src, buf, lo, mid);
    mergeSort(src, buf, mid, hi);
    int32_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) buf[k++] = lessPt(src[i], src[j]) ? src[i++] : src[j++];
    while (i < mid) buf[k++] = src[i++];
    while (j < hi)  buf[k++] = src[j++];
    for (i = lo; i < hi; i++) src[i] = buf[i];
}

#if defined(USE_STD_SORT)
static int cmpPt(const void *x, const void *y) {
    const Pt *u = (const Pt *)x, *v = (const Pt *)y;
    if (u->a != v->a) return u->a < v->a ? -1 : 1;
    if (u->b != v->b) return u->b < v->b ? -1 : 1;
    if (u->c != v->c) return u->c < v->c ? -1 : 1;
    return 0;
}
#endif

int main() {
    Pcg32 r = pcgSeed(20260922ULL, 54);

    for (int32_t i = 0; i < N; i++) {
        p[i].a = (int32_t)pcgBounded(&r, V);
        p[i].b = (int32_t)pcgBounded(&r, V);
        p[i].c = (int32_t)pcgBounded(&r, V);
        p[i].cnt = 1;
        p[i].ans = 0;
    }
    uint64_t inSum = 0;
    for (int32_t i = 0; i < N; i++)
        inSum = inSum * 1000003ULL + (uint64_t)p[i].a * 3 + (uint64_t)p[i].b * 5 + (uint64_t)p[i].c * 7;

    /* ② 排序 */
#if defined(USE_STD_SORT)
    std::sort(p, p + N, lessPt);
#else
    mergeSort(p, tmp, 0, N);
#endif

    /* ③ 相邻去重 */
    int32_t m = 0;
    for (int32_t i = 0; i < N; i++) {
        if (m > 0 && p[m-1].a == p[i].a && p[m-1].b == p[i].b && p[m-1].c == p[i].c) {
            p[m-1].cnt++;
        } else {
            p[m] = p[i];
            p[m].cnt = 1;
            p[m].ans = 0;
            m++;
        }
    }

    /* ④ 分治 */
    cdq(0, m - 1);

    /* ⑤ 校验和 */
    uint64_t sum = 0, maxAns = 0;
    for (int32_t i = 0; i < m; i++) {
        int32_t ans = p[i].ans + p[i].cnt - 1;
        sum += (uint64_t)p[i].cnt * (uint64_t)ans;
        if ((uint64_t)ans > maxAns) maxAns = (uint64_t)ans;
    }

    printf("in   = %llu\n", (unsigned long long)inSum);
    printf("m    = %d\n", m);
    printf("max  = %llu\n", (unsigned long long)maxAns);
    printf("sum  = %llu\n", (unsigned long long)sum);
    return 0;
}
