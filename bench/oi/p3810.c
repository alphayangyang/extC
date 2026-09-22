/* 三维偏序（CDQ 分治 + 树状数组）—— OI 数量级四语言横评的 **C 参照实现**
 *
 * 题目（就是那个模板题「陌上花开」）：给 n 个点 (a,b,c)，对每个点数出
 * 有多少 j 满足 a_j<=a_i 且 b_j<=b_i 且 c_j<=c_i（含自己，最后减 1）。
 *
 * 四个语言写**同一套算法**：
 *   ① pcg32 生成数据（种子固定 ⇒ 四语言必须逐位一致）
 *   ② 按 (a,b,c) 排序
 *   ③ 相邻去重 + 记 cnt
 *   ④ CDQ 分治：左半 → 右半 的贡献，用**权值树状数组**按 c 统计；顺带按 b 归并
 *   ⑤ 校验和：sum(cnt_i * ans_i)（u64 回绕）—— 去重和计数任何一步错了它都会变
 *
 * ⚠️ 排序这一步各语言的"std 写法"不一样（C 只有 qsort，C++/Rust 有 std 排序，
 *    extC 没有 std 排序）⇒ 主表用**四语言都手写的归并排序**（苹果对苹果），
 *    另有一组 `-DUSE_STD_SORT` 的对照看标准库的差距 ✓
 * ⚠️ `-DCHECKED` 是**「安全设计的代价」对照**：把数组访问包一层边界检查，
 *    形状跟 extC 生成的一样 ⇒ 能把"extC 慢的那点"拆成两半 ✓
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N
#define N 4000000
#endif
#ifndef V
#define V 1000000          /* a,b,c 的取值范围 [0,V) */
#endif

typedef struct { int32_t a, b, c, cnt, ans; } Pt;   /* 20 字节：ans 用 i32 够了（<= n）*/
_Static_assert(sizeof(Pt) == 20, "Pt 要保持 20 字节");

static Pt p[N], tmp[N];
static int32_t bit[V + 1];

#ifdef CHECKED
/* 边界检查版的下标：越界就 trap（形状跟 extC 生成的一模一样）✓ */
static inline int64_t ck(int64_t i, int64_t n) {
    if (i < 0 || i >= n) {
        fprintf(stderr, "trap: index %lld out of range (length %lld)\n",
                (long long)i, (long long)n);
        exit(1);
    }
    return i;
}
#define IDX(A, N_, I) (A)[ck((I), (N_))]
#else
#define IDX(A, N_, I) (A)[(I)]
#endif

/* ---------------- pcg32（跟 extC prelude 里那份逐位一致）---------------- */
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

/* ---------------- 权值树状数组 ----------------
 * ⚠️ 1-indexed：调用点一律 `c + 1`（值域是 [0,V)，而 c=0 会让 `i & -i` 恒为 0
 *    ⇒ 死循环 ✗ 这是写这份参照实现时自己踩的坑，四个语言都得注意）*/
static void bitAdd(int64_t i, int32_t v) {
    for (; i <= V; i += i & (-i)) IDX(bit, V + 1, i) += v;
}
static int32_t bitSum(int64_t i) {
    int32_t s = 0;
    for (; i > 0; i -= i & (-i)) s += IDX(bit, V + 1, i);
    return s;
}

/* ---------------- CDQ 分治 ---------------- */
static void cdq(int64_t l, int64_t r) {
    if (l >= r) return;
    int64_t mid = l + (r - l) / 2;
    cdq(l, mid);
    cdq(mid + 1, r);

    /* 左半 → 右半 的贡献：两边都已经按 b 有序 ⇒ 双指针，c 进树状数组 */
    int64_t i = l, j = mid + 1;
    while (j <= r) {
        while (i <= mid && IDX(p, N, i).b <= IDX(p, N, j).b) {
            bitAdd(IDX(p, N, i).c + 1, IDX(p, N, i).cnt);
            i++;
        }
        IDX(p, N, j).ans += bitSum(IDX(p, N, j).c + 1);
        j++;
    }
    for (int64_t t = l; t < i; t++) bitAdd(IDX(p, N, t).c + 1, -IDX(p, N, t).cnt);  /* 撤销 */

    /* 按 b 归并（这样上一层拿到的两半仍然有序）*/
    i = l; j = mid + 1;
    int64_t k = l;
    while (i <= mid && j <= r)
        IDX(tmp, N, k++) = (IDX(p, N, i).b <= IDX(p, N, j).b) ? IDX(p, N, i++) : IDX(p, N, j++);
    while (i <= mid) IDX(tmp, N, k++) = IDX(p, N, i++);
    while (j <= r)   IDX(tmp, N, k++) = IDX(p, N, j++);
    for (i = l; i <= r; i++) IDX(p, N, i) = IDX(tmp, N, i);
}

/* ---------------- 排序 ---------------- */
static int cmpPt(const void *x, const void *y) {
    const Pt *u = (const Pt *)x, *v = (const Pt *)y;
    if (u->a != v->a) return u->a < v->a ? -1 : 1;
    if (u->b != v->b) return u->b < v->b ? -1 : 1;
    if (u->c != v->c) return u->c < v->c ? -1 : 1;
    return 0;
}

/* 四语言**同一套**的归并排序（苹果对苹果那一组）*/
static void mergeSort(Pt *src, Pt *buf, int64_t lo, int64_t hi) {
    if (hi - lo < 2) return;
    int64_t mid = lo + (hi - lo) / 2;
    mergeSort(src, buf, lo, mid);
    mergeSort(src, buf, mid, hi);
    int64_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        const Pt *x = &src[i], *y = &src[j];
        int less = (x->a != y->a) ? (x->a < y->a)
                 : (x->b != y->b) ? (x->b < y->b)
                 : (x->c < y->c);
        buf[k++] = less ? src[i++] : src[j++];
    }
    while (i < mid) buf[k++] = src[i++];
    while (j < hi)  buf[k++] = src[j++];
    for (i = lo; i < hi; i++) src[i] = buf[i];
}

int main(void) {
    pcg32 r = pcgSeed(20260922ULL, 54);

    for (int64_t i = 0; i < N; i++) {
        IDX(p, N, i).a = (int32_t)pcgBounded(&r, V);
        IDX(p, N, i).b = (int32_t)pcgBounded(&r, V);
        IDX(p, N, i).c = (int32_t)pcgBounded(&r, V);
        IDX(p, N, i).cnt = 1;
        IDX(p, N, i).ans = 0;
    }
    /* 输入本身的校验和 —— 四个语言必须一致，不然就是 PRNG 抄错了 */
    uint64_t inSum = 0;
    for (int64_t i = 0; i < N; i++)
        inSum = inSum * 1000003ULL + (uint64_t)IDX(p, N, i).a * 3
                                  + (uint64_t)IDX(p, N, i).b * 5
                                  + (uint64_t)IDX(p, N, i).c * 7;

    /* ② 排序 */
#ifdef USE_STD_SORT
    qsort(p, N, sizeof(Pt), cmpPt);
#else
    mergeSort(p, tmp, 0, N);
#endif

    /* ③ 相邻去重 */
    int64_t m = 0;
    for (int64_t i = 0; i < N; i++) {
        if (m > 0 && IDX(p, N, m - 1).a == IDX(p, N, i).a
                  && IDX(p, N, m - 1).b == IDX(p, N, i).b
                  && IDX(p, N, m - 1).c == IDX(p, N, i).c) {
            IDX(p, N, m - 1).cnt++;
        } else {
            IDX(p, N, m) = IDX(p, N, i);
            IDX(p, N, m).cnt = 1;
            IDX(p, N, m).ans = 0;
            m++;
        }
    }

    /* ④ 分治 */
    cdq(0, m - 1);

    /* ⑤ 校验和 */
    uint64_t sum = 0, maxAns = 0;
    for (int64_t i = 0; i < m; i++) {
        int32_t ans = IDX(p, N, i).ans + IDX(p, N, i).cnt - 1;   /* 含自己 ⇒ 减 1 */
        sum += (uint64_t)IDX(p, N, i).cnt * (uint64_t)ans;
        if ((uint64_t)ans > maxAns) maxAns = (uint64_t)ans;
    }

    printf("in   = %llu\n", (unsigned long long)inSum);
    printf("m    = %lld\n", (long long)m);
    printf("max  = %llu\n", (unsigned long long)maxAns);
    printf("sum  = %llu\n", (unsigned long long)sum);
    return 0;
}
