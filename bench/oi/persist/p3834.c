/* 静态区间第 k 小（主席树 / 可持久化线段树，P3834）—— **C 参照实现**
 *
 * 跟 `p3834.cpp` / `p3834.rs` / `p3834.extc` **同一套算法**（逐行对应）：
 *   ① pcg32 生成 n 个数（种子固定 ⇒ 四语言必须逐位一致）
 *   ② 对每个前缀建一棵可持久化权值线段树（只在一条路径上开新节点）
 *      ⇒ 节点池 = **索引式**（int32 下标 + 一块连续内存）
 *   ③ q 次询问：在 `root[r] - root[l-1]` 这棵"差分树"上二分找第 k 小
 *   ④ 校验和：输入校验和 + 答案校验和（u64 回绕）
 *
 * ⭐ **C 侧用足 C 的分配设施**（主人要求）：默认就是 `malloc` + `free` ——
 *   一块 252MB 的池子（21M × 12 字节）从堆上拿，程序结束 `free` 掉 ✓
 *   三个编译期开关给的是**同一算法的三种"C 味"内存策略**（对照用）：
 *     · （默认）`malloc` 一整块池子 + 末尾 `free`
 *     · `-DSTATIC_POOL`：`static Node pool[…]`（BSS，靠 OS 懒清零，不 malloc）
 *     · `-DNODE_MALLOC`：**每节点一次 `malloc`** + 末尾逐个 `free`
 *       （C 里最"顺手"的写法，也是分配器的照妖镜：21M 次 malloc 的元数据开销 ✓）
 *     · `-DCHECKED`：把数组访问包一层边界检查，**形状跟 extC 生成的一样**
 *       ⇒ 用它把"extC 慢的那点"拆成【检查的代价】和【别的代价】✓
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N
#define N 1000000
#endif
#ifndef Q
#define Q 1000000
#endif
#ifndef V
#define V 1000000
#endif
#ifndef DEPTH
#define DEPTH 21               /* 每个数开的新节点数 ≈ ⌈log2 V⌉ + 1 */
#endif

#define POOL_SIZE ((int64_t)N * DEPTH + 2)

typedef struct { int32_t l, r, sum; } Node;

static int32_t A[N + 1];
static int32_t ROOT[N + 1];
static int32_t NP = 1;                 /* 0 号是空节点（l=r=sum=0）*/

#ifdef NODE_MALLOC
/* ---- 每节点一次 malloc（指针数组 + 逐个 free）---- */
static Node *POOLP[POOL_SIZE];
static int32_t newNode(void) {
    int32_t i = NP++;
    POOLP[i] = (Node *)calloc(1, sizeof(Node));
    if (!POOLP[i]) { fprintf(stderr, "calloc failed at node %d\n", i); exit(1); }
    return i;
}
static void poolInit(void) {
    /* 0 号空节点在"每节点 malloc"模式下也得真的存在（不然 `POOLP[0]->l` 就是空指针解引用 ✗）*/
    POOLP[0] = (Node *)calloc(1, sizeof(Node));
    if (!POOLP[0]) { fprintf(stderr, "calloc failed\n"); exit(1); }
}
static void dropPool(void) {
    for (int32_t i = 0; i < NP; i++) free(POOLP[i]);
}
#define ND(i) POOLP[i]
#else
# ifndef STATIC_POOL
/* ---- 一整块 malloc 的池子（默认）---- */
static Node *POOL;
static void poolInit(void) { }
static int32_t newNode(void) { return NP++; }
static void dropPool(void) { free(POOL); }
# else
/* ---- 静态数组池（对照：不 malloc，BSS 懒清零）---- */
static Node POOL[POOL_SIZE];
static void poolInit(void) { }
static int32_t newNode(void) { return NP++; }
static void dropPool(void) { }
# endif
#define ND(i) (POOL + (i))
#endif

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
#define AT(a, n, i) ((a)[ck((i), (n))])
#else
#define AT(a, n, i) ((a)[(i)])
#endif

#define A_(i)      AT(A, N + 1, i)
#define ROOT_(i)   AT(ROOT, N + 1, i)
#define LCH(i)     ND(AT_POOL(i))->l
#define RCH(i)     ND(AT_POOL(i))->r
#define SUM_(i)    ND(AT_POOL(i))->sum
#ifdef CHECKED
#define AT_POOL(i) ck(i, POOL_SIZE)
#else
#define AT_POOL(i) (i)
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

/* ---------------- 可持久化线段树 ---------------- */
static int32_t update(int32_t prev, int32_t lo, int32_t hi, int32_t pos) {
    int32_t rt = newNode();
    LCH(rt) = LCH(prev);
    RCH(rt) = RCH(prev);
    SUM_(rt) = SUM_(prev) + 1;
    if (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (pos <= mid) LCH(rt) = update(LCH(prev), lo, mid, pos);
        else            RCH(rt) = update(RCH(prev), mid + 1, hi, pos);
    }
    return rt;
}

static int32_t query(int32_t u, int32_t v, int32_t lo, int32_t hi, int32_t k) {
    if (lo == hi) return lo;
    int32_t mid = lo + (hi - lo) / 2;
    int32_t leftCount = SUM_(LCH(v)) - SUM_(LCH(u));
    if (k <= leftCount) return query(LCH(u), LCH(v), lo, mid, k);
    return query(RCH(u), RCH(v), mid + 1, hi, k - leftCount);
}

int main(void) {
    pcg32 rng = pcgSeed(20260922ULL, 54);

#ifndef STATIC_POOL
# ifndef NODE_MALLOC
    POOL = (Node *)calloc((size_t)POOL_SIZE, sizeof(Node));
    if (!POOL) { fprintf(stderr, "pool calloc failed\n"); return 1; }
# endif
#endif
    poolInit();

    for (int64_t i = 1; i <= N; i++) A_(i) = (int32_t)pcgBounded(&rng, V) + 1;

    uint64_t inSum = 0;
    for (int64_t i = 1; i <= N; i++) inSum = inSum * 1000003ULL + (uint64_t)A_(i);

    for (int64_t i = 1; i <= N; i++)
        ROOT_(i) = update(ROOT_(i - 1), 1, V, A_(i));

    uint64_t ansSum = 0;
    for (int64_t j = 0; j < Q; j++) {
        int64_t l = 1 + (int64_t)pcgBounded(&rng, N);
        int64_t r = l + (int64_t)pcgBounded(&rng, (uint64_t)(N - l + 1));
        int64_t k = 1 + (int64_t)pcgBounded(&rng, (uint64_t)(r - l + 1));
        int32_t ans = query(ROOT_(l - 1), ROOT_(r), 1, V, (int32_t)k);
        ansSum = ansSum * 1000009ULL + (uint64_t)ans;
    }

    printf("in    = %llu\n", (unsigned long long)inSum);
    printf("n     = %lld\n", (long long)N);
    printf("q     = %lld\n", (long long)Q);
    printf("nodes = %lld\n", (long long)NP);
    printf("ans   = %llu\n", (unsigned long long)ansSum);

    dropPool();
    return 0;
}
