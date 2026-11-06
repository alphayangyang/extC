/* 位图 slab 原型 —— 往极致性能做的一版。
 *
 * 与 sim.c 的区别（就一条，但是关键的一条）：**元数据与载荷分开**。
 *   · 载荷：按 2 的幂向上取整的槽，放在**对齐的 slab** 里连续排布
 *   · 元数据：每 slab 两张位图（占用 + 标记），与载荷分开
 *   · 由对象指针找 slab = 一次按位与（slab 边界对齐）；找槽号 = 一次右移
 *   · 清扫 = **顺序扫位图**（按 64 位字），不追指针、不逐个 free
 *   · 空闲槽用一条链（存在槽内），分配是 O(1)
 *
 * 于是：RSS ≈ 载荷 + 2 bit/槽；清扫是顺序访存。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec; }
static long rss_kib(void){ FILE*f=fopen("/proc/self/statm","r"); if(!f) return 0; long a=0,b=0;
    if(fscanf(f,"%ld %ld",&a,&b)!=2) b=0; fclose(f); return b*(long)(sysconf(_SC_PAGESIZE)/1024); }
static long sink, g_peak_rss;
static double g_pauses[1 << 20]; static int64_t g_plive[1 << 20]; static size_t g_np;
static int64_t g_t_alloc, g_t_mark, g_t_sweep_slots, g_t_push, g_t_free;
static int64_t g_t_seed, g_t_trav, g_t_scan;
static size_t g_nalloc, g_nfreed, g_marked, g_kills, g_evfired, g_sweepfreed;

/* ---------- 类型表（编译器按类型发射） ---------- */
typedef struct { int nptr; size_t off[2]; } TypeDesc;
typedef struct Obj { int64_t key, val; struct Obj *next, *prev, *parent, *child; } Obj;   /* 48 字节 */
typedef struct { int64_t next; } Inode;                          /* 占位，见下 */

/* ---------- 对齐 slab ---------- */
#define SLAB_BITS 20                       /* 1 MiB */
#define SLAB_SIZE ((size_t)1 << SLAB_BITS)
typedef struct Slab {
    struct Slab *next;                     /* 本区域的 slab 链 */
    uint64_t *occ, *mark, *rsbit;          /* 位图：占用 / 标记 / 已登记入边 */
    uint16_t *hold;                        /* 每槽：精确持有者计数（事件回收用；环由清扫兜底） */
    int64_t   freelist;                    /* 空闲槽链：存**槽号**，-1 = 空；槽头 8 字节存下一槽号 */
    uint32_t  nslots, nwords, shift, rid, used;
    /* 载荷从对齐边界的固定偏移开始 */
} Slab;
#define SLAB_HDR ((sizeof(Slab) + 63) & ~(size_t)63)
#define SLAB_DATA(s) ((char *)(s) + SLAB_HDR)

/* 由对象指针找 slab：slab 按 SLAB_SIZE 对齐 ⇒ 一次按位与 */
static inline Slab *slab_of(void *p) { return (Slab *)((uintptr_t)p & ~(uintptr_t)(SLAB_SIZE - 1)); }
static inline size_t slot_of(Slab *s, void *p) { return ((size_t)((char *)p - SLAB_DATA(s))) >> s->shift; }

typedef struct Region2 {
    Slab *slabs;
    char *bump, *bump_end;               /* bump 模式（解析区）：O(1) 重置 */
    int64_t bytes;                         /* 占用载荷字节（O(1) 计数） */
    void **rs; size_t rsn, rscap;          /* 入边集 */
    uint32_t rid;
} Region2;

static Region2 *g_reg[128];
static size_t slot_size_of(size_t n);
static Slab *slab_new(uint32_t rid, size_t slot_size);
__attribute__((noinline)) static void *r2_bump_alloc(Region2 *r, size_t n) {
    size_t ss = slot_size_of(n);
    if (!r->bump || (size_t)(r->bump_end - r->bump) < ss) {
        Slab *s = slab_new(r->rid, ss); s->next = r->slabs; r->slabs = s;
        r->bump = SLAB_DATA(s); r->bump_end = SLAB_DATA(s) + ((size_t)s->nslots << s->shift);
    }
    void *p = r->bump; r->bump += ss; return p;
}
static void r2_bump_reset(Region2 *r) {          /* 整片回收 = O(1)：只把 bump 指针拨回去 */
    Slab *s = r->slabs; if (!s) return;
    r->bump = SLAB_DATA(s); r->bump_end = SLAB_DATA(s) + ((size_t)s->nslots << s->shift);
}
static size_t slot_size_of(size_t n) {     /* 向上取到 2 的幂，且至少放得下槽头 */
    size_t s = 16; while (s < n) s <<= 1; return s;
}
__attribute__((noinline)) static Slab *slab_new(uint32_t rid, size_t slot_size) {
    Slab *s = aligned_alloc(SLAB_SIZE, SLAB_SIZE);
    if (!s) { fprintf(stderr, "slab: 分配失败\n"); exit(1); }
    memset(s, 0, SLAB_HDR);
    s->shift = 0; while (((size_t)1 << s->shift) < slot_size) s->shift++;
    size_t nslots = (SLAB_SIZE - SLAB_HDR) >> s->shift;
    s->nslots = (uint32_t)nslots; s->nwords = (uint32_t)((nslots + 63) / 64);
    s->rid = rid; s->next = NULL;
    s->occ  = calloc(s->nwords, sizeof(uint64_t));
    s->mark = calloc(s->nwords, sizeof(uint64_t));
    s->rsbit = calloc(s->nwords, sizeof(uint64_t));
    s->hold = calloc(nslots, sizeof(uint16_t));
    char *d = SLAB_DATA(s);
    for (size_t i = 0; i < nslots; i++) { *(int64_t *)(d + (i << s->shift)) = (int64_t)(i + 1 < nslots ? i + 1 : -1); }
    s->freelist = nslots ? 0 : -1;         /* 整块串成空闲链（槽号 0 起） */
    return s;
}
__attribute__((noinline)) static void *r2_alloc(Region2 *r, size_t n) {
#ifdef PHASES
    int64_t t0 = now_ns();
#endif
    g_nalloc++;
    Slab *s = r->slabs;
    while (s && (s->freelist < 0 || ((size_t)1 << s->shift) < n)) s = s->next;   /* 找有合适槽的 slab */
    if (!s) { s = slab_new(r->rid, slot_size_of(n)); s->next = r->slabs; r->slabs = s; }
    size_t i = (size_t)s->freelist;
    char *p = SLAB_DATA(s) + (i << s->shift);
    s->freelist = *(int64_t *)p;                       /* 取下一槽号 */
    s->occ[i >> 6] |= (uint64_t)1 << (i & 63);
    s->used++;
    r->bytes += (int64_t)n;
#ifdef PHASES
    g_t_alloc += now_ns() - t0;
#endif
    return p;
}
static int RC_FREE = 1;
static size_t g_rcfreed;
static void r2_free_slot(Slab *s, size_t i) {
    if (g_reg[s->rid]) g_reg[s->rid]->bytes -= (int64_t)((size_t)1 << s->shift);
    char *p = SLAB_DATA(s) + (i << s->shift);
    *(int64_t *)p = s->freelist;                       /* 头插：存旧槽号 */
    s->freelist = (int64_t)i;
    s->occ[i >> 6] &= ~((uint64_t)1 << (i & 63));
    s->rsbit[i >> 6] &= ~((uint64_t)1 << (i & 63));
    s->hold[i] = 0;
    s->used--;
}
#define HOLD_SAT 65535u
static inline void upd_hold(Obj *o, int delta) {
    Slab *s = slab_of(o); size_t i = slot_of(s, o);
    if (delta > 0) { if (s->hold[i] != HOLD_SAT) s->hold[i]++; }        /* 到 65535 粘住 */
    else if (s->hold[i] && s->hold[i] != HOLD_SAT) {                     /* 饱和值不再减 ⇒ 永不误判归零 */
        s->hold[i]--;
        if (s->hold[i] == 0 && RC_FREE) { r2_free_slot(s, i); g_rcfreed++; }
    }
}
static inline void r2_mark(void *p) { Slab *s = slab_of(p); size_t i = slot_of(s, p); s->mark[i >> 6] |= (uint64_t)1 << (i & 63); }
static inline int  r2_marked(void *p) { Slab *s = slab_of(p); size_t i = slot_of(s, p); return (s->mark[i >> 6] >> (i & 63)) & 1; }

/* 顺序扫位图：死槽 = 占用 & ~标记。按 64 位字走，不追指针。 */
__attribute__((noinline)) static size_t r2_sweep(Region2 *r) {
    int64_t t0 = now_ns(); size_t nf = 0;
    for (Slab *s = r->slabs; s; s = s->next) {
        for (uint32_t w = 0; w < s->nwords; w++) {
            uint64_t dead = s->occ[w] & ~s->mark[w];
            while (dead) { int b = __builtin_ctzll(dead); dead &= dead - 1;
                size_t i = (size_t)w * 64 + (size_t)b;
                r2_free_slot(s, i); nf++; }
        }
        memset(s->mark, 0, s->nwords * sizeof(uint64_t));   /* 顺序清零，很快 */
    }
    /* 清扫后重算存活载荷字节（顺序扫位图，代价很小） */
    int64_t tot = 0;
    for (Slab *s = r->slabs; s; s = s->next)
        for (uint32_t w = 0; w < s->nwords; w++)
            tot += (int64_t)__builtin_popcountll(s->occ[w]) * (int64_t)((size_t)1 << s->shift);
    r->bytes = tot;
    g_nfreed += nf; g_t_sweep_slots += now_ns() - t0;
    return nf;
}
static size_t r2_destroy(Region2 *r) {
    size_t nb = 0; Slab *s = r->slabs;
    while (s) { Slab *n = s->next; free(s->occ); free(s->mark); free(s->rsbit); free(s->hold); free(s); nb++; s = n; }
    r->slabs = NULL; free(r->rs); r->rs = NULL; r->rsn = r->rscap = 0; return nb;
}

static void **g_rs[4]; static size_t g_rsn[4], g_rscap[4]; static uint32_t g_rsmark_epoch = 1;
static int NSHARD = 1, BALANCE = 0; static int64_t FACTOR_CAP = 1000000;
static int64_t live_shard[64];
static int64_t sweep_factor[64];   /* 自学习：一次清扫一无所获就把阈值乘 4 */
static Region2 g_shard[64];                                   /* 分片：每片一个区域 */

/* 入边只登记**一个 bit**（在目标对象所在 slab 的 rsbit 位图里）。
   释放槽时会清掉 ⇒ 没有陈旧入边；播种时顺序扫位图 ⇒ 没有指针数组的增长与入栈成本。 */
static inline int shard_of(void *o) { return (int)(slab_of(o)->rid - 1); }
static void rs_add_shard(int k, void *p) {
    (void)k;
    Slab *s = slab_of(p); size_t i = slot_of(s, p);
    if (!s->rsbit) return;
    if ((s->rsbit[i >> 6] >> (i & 63)) & 1) return;         /* 已登记 ⇒ 去重（入边集**持久**，不清空） */
    s->rsbit[i >> 6] |= (uint64_t)1 << (i & 63);
    (void)g_rsmark_epoch;
}

/* 引用赋值：目标 +1、旧值 −1（饱和三点式，饱和值当"不确定"，永不从饱和值减回去 ⇒ 健全） */
/* 新对象的槽：里面没有旧值（是空闲链的下一槽号），只能"写 + 目标 +1"，不能读旧值 */
static void init_ref(Obj **slot, Obj *v) {
    *slot = v;
#ifdef NOHOLD
    (void)slot; (void)v; return;
#endif
    if (v) { Slab *s = slab_of(v); size_t i = slot_of(s, v); s->hold[i]++; }
}
__attribute__((noinline)) static inline void store_hold(Obj **slot, Obj *v) {
    Obj *old = *slot; *slot = v;
    if (v) upd_hold(v, +1);
    if (old) upd_hold(old, -1);
}
static uint16_t hold_of(Obj *o) { Slab *s = slab_of(o); return s->hold[slot_of(s, o)]; }

/* ---------- 负载：有界符号表 + 缓存（第二持有者）+ 索引（跨区边） ---------- */
#define BUCKETS 64
#define BUCKET_MAX 8
#define TRIGGER (1 << 18)
#define MEM_FLOOR (1 << 20)   /* 每区域内存压力阈值下限（1MiB）*/
static Region2 g_shard[64];   /* 分片：每片一个区域，各自回收 */
static Region2 g_churn;   /* 解析区（每轮清空） */
static Region2 g_idx;     /* 索引区（跨区冷结构） */
static Obj *g_head[BUCKETS], *g_tail[BUCKETS], *g_nodes[8192], *g_dummy;
static int g_size[BUCKETS]; static long g_nnodes;
static Obj **g_cache; static int g_ncache, g_cache_every;
static void *g_ihead;

static void **g_stk; static size_t g_sp, g_stkcap;
static void mpush(void *o) {
    if (g_sp == g_stkcap) { g_stkcap = g_stkcap ? g_stkcap * 2 : (1 << 16); g_stk = realloc(g_stk, g_stkcap * sizeof(void *)); }
    g_stk[g_sp++] = o;
}
static size_t g_mark_nstop;
/* 从根 + 入边集出发标记；只在 rid 区域内继续走（区域外都当活的） */
__attribute__((noinline)) static void mark_all(Region2 *reg, uint32_t rid, void **roots, int nr) {
    g_sp = 0;
#ifdef PHASES
    int64_t s0 = now_ns();
#endif
    for (int i = 0; i < nr; i++) if (roots[i]) mpush(roots[i]);
    /* 跨区入边：顺序扫本区域每个 slab 的登记位图，把仍占用的槽作为种子 */
    for (Slab *sl = reg->slabs; sl; sl = sl->next)
        for (uint32_t w = 0; w < sl->nwords; w++) {
            uint64_t bits = sl->rsbit[w] & sl->occ[w];
            while (bits) { int b = __builtin_ctzll(bits); bits &= bits - 1;
                mpush(SLAB_DATA(sl) + (((size_t)w * 64 + (size_t)b) << sl->shift)); }
        }
#ifdef PHASES
    int64_t s1 = now_ns();
    g_t_seed += s1 - s0;
#endif
    g_mark_nstop = 0;
    while (g_sp) { void *o = g_stk[--g_sp];
        if (r2_marked(o)) continue;
        r2_mark(o); g_marked++;
        if (rid && slab_of(o)->rid != rid) { g_mark_nstop++; continue; }
        Obj *ob = (Obj *)o; if (ob->next) mpush(ob->next); }
#ifdef PHASES
    g_t_trav += now_ns() - s1;
#endif
}

static int EVENT = 0;
int main(int argc, char **argv) {
    EVENT = getenv("EV") != NULL;
    long N = argc > 1 ? atol(argv[1]) : 1000000;
    g_ncache = argc > 2 ? atoi(argv[2]) : 0;
    g_cache_every = argc > 3 ? atoi(argv[3]) : 64;
    long indexmax = argc > 4 ? atol(argv[4]) : 0;
    NSHARD = argc > 6 ? atoi(argv[6]) : 1;
    BALANCE = getenv("BALANCE") != NULL; if (getenv("CAP")) FACTOR_CAP = atol(getenv("CAP")); if (NSHARD < 1) NSHARD = 1; if (NSHARD > 64) NSHARD = 64;
    size_t objsz = argc > 5 ? (size_t)atol(argv[5]) : sizeof(Obj);

    g_churn.rid = (uint32_t)(NSHARD + 1); g_idx.rid = (uint32_t)(NSHARD + 2);
    memset(g_head, 0, sizeof g_head);
    if (g_ncache) g_cache = calloc((size_t)g_ncache, sizeof(Obj *));
    int64_t peak_buck = 0, peak_rss = 0, max_pause = 0, total_pause = 0, live_buck = 0;
    size_t sweeps = 0;

    for (int j = 0; j < NSHARD; j++) { g_shard[j].rid = (uint32_t)(j + 1); g_reg[j + 1] = &g_shard[j]; }
    g_reg[NSHARD + 1] = &g_churn; g_reg[NSHARD + 2] = &g_idx;
    static void **roots = NULL; size_t roots_cap = 0;
    int64_t t0 = now_ns();
    for (long i = 0; i < N; i++) {
        int b = (int)(i % BUCKETS);
        r2_bump_reset(&g_churn);                       /* 解析区：O(1) 整片回收 */
        Obj *t1 = r2_bump_alloc(&g_churn, objsz), *t2 = r2_bump_alloc(&g_churn, objsz), *t3 = r2_bump_alloc(&g_churn, objsz);
        t1->next = t2; t2->next = t3; t3->next = NULL; t3->val = i;
        for (Obj *q = t1; q; q = q->next) sink += q->val;

        int sh_i_alloc = b % NSHARD;
        if (BALANCE) {                       /* A：挑存活量最小的分片 */
            int64_t best = g_shard[0].bytes; sh_i_alloc = 0;
            for (int j = 1; j < NSHARD; j++) if (g_shard[j].bytes < best) { best = g_shard[j].bytes; sh_i_alloc = j; }
        }
        Region2 *sh = &g_shard[sh_i_alloc];
        Obj *nd = r2_alloc(sh, objsz);
        nd->key = i; nd->val = i * 3;

        /* ① 双向链表（LRU 形状）：next/prev 互指 ⇒ 天然两元环 */
        init_ref(&nd->next, g_head[b]);
        if (g_head[b]) init_ref(&g_head[b]->prev, nd);
        if (g_head[b] && shard_of(g_head[b]) != shard_of(nd)) {   /* 链的跨分片引用也要登记 */
            rs_add_shard(shard_of(g_head[b]), g_head[b]);
            rs_add_shard(shard_of(nd), nd);
        }
        store_hold(&g_head[b], nd);
        if (g_tail[b] == NULL) g_tail[b] = nd;
        g_size[b]++;

        /* ② 缓存（第二持有者）*/
        if (g_cache && g_cache_every && (i % g_cache_every) == 0)
            store_hold(&g_cache[(i / g_cache_every) % g_ncache], nd);

        /* ③ 索引（跨区边）*/
        if ((long)(g_idx.bytes / (int64_t)sizeof(void *)) < indexmax * 2) {
            void **ix = r2_bump_alloc(&g_idx, sizeof(void *) * 2);
            ix[0] = g_ihead; init_ref((Obj **)&ix[1], nd); g_ihead = ix;
            g_idx.bytes += (int64_t)sizeof(void *) * 2;
            rs_add_shard(sh_i_alloc, nd);
        }

        /* ④ 带父指针的树：child/parent 互指 ⇒ 天然环；节点长命 */
        if ((i & 15) == 0) {
            g_nodes[g_nnodes & 8191] = nd;
            if (g_nnodes > 0) {
                Obj *par = g_nodes[(g_nnodes - 1) & 8191];
                if (par) {
                    init_ref(&par->child, nd); init_ref(&nd->parent, par);
                    if (shard_of(par) != shard_of(nd)) {           /* 跨分片引用：两侧都要登记 */
                        rs_add_shard(shard_of(nd), nd);
                        rs_add_shard(shard_of(par), par);
                    }
                }
            }
            g_nnodes++;
        }

        /* 查询 */
        for (Obj *q = g_head[b]; q; q = q->next) if (q->key == i - 7) { q->val++; break; }

        /* ⑤ 淘汰链尾（但 next/prev 互指 ⇒ 环留下，计数永远不归零）*/
        if (g_size[b] > BUCKET_MAX && g_tail[b]) {
            Obj *tl = g_tail[b];
            Obj *pr = tl->prev;
            if (pr) { store_hold(&pr->next, NULL); store_hold(&g_tail[b], pr); }
            else    { store_hold(&g_head[b], NULL); store_hold(&g_tail[b], NULL); }
            g_size[b]--;
        }

        /* 触发：**内存压力**驱动。rc 已收掉一切非环垃圾 ⇒ 区域占用 ≈ 存活，
           于是"占用 > max(2×上次清扫后存活, 阈值)"只在**环真的攒起来**时才成立。
           无环程序永不进入清扫 ⇒ 停顿 0。 */
        {
            Region2 *best = NULL; int64_t bestg = 0;
            for (int j = 0; j < NSHARD; j++) {
                Region2 *r = &g_shard[j];
                int64_t thr = live_shard[j] * 2;
                if (thr < (int64_t)MEM_FLOOR / NSHARD) thr = (int64_t)MEM_FLOOR / NSHARD;
                thr *= (sweep_factor[j] ? sweep_factor[j] : 1);
                if (r->bytes > thr && r->bytes - live_shard[j] > bestg) { best = r; bestg = r->bytes - live_shard[j]; }
            }
            if (best) {
                int64_t p0 = now_ns();
                int sh_i = (int)(best - g_shard), nr = 0;
                size_t need = (size_t)BUCKETS + (size_t)g_ncache + 8192 + 8;
                if (need > roots_cap) { roots_cap = need; roots = realloc(roots, roots_cap * sizeof(void *)); }
                for (int j = 0; j < BUCKETS; j++)
                    if ((j % NSHARD) == sh_i && g_head[j]) roots[nr++] = g_head[j];
                for (int j = 0; j < g_ncache; j++) if (g_cache[j]) roots[nr++] = g_cache[j];
                for (int j = 0; j < 8192; j++) if (g_nodes[j]) roots[nr++] = g_nodes[j];   /* C 层数组也是根 */
                mark_all(best, best->rid, roots, nr);
                size_t swf_before = g_sweepfreed;
                { size_t b0 = g_nfreed; r2_sweep(best); g_sweepfreed += g_nfreed - b0; }
                live_shard[sh_i] = best->bytes;
                sweep_factor[sh_i] = (g_sweepfreed == swf_before) ? (sweep_factor[sh_i] ? sweep_factor[sh_i] * 2 : 2) : 1;
                if (sweep_factor[sh_i] > FACTOR_CAP) sweep_factor[sh_i] = FACTOR_CAP;
                int64_t p1 = now_ns();
                if (p1 - p0 > max_pause) max_pause = p1 - p0;
                total_pause += p1 - p0; sweeps++;
                if (g_np < (1 << 20)) { g_pauses[g_np] = (double)(p1 - p0) / 1000.0; g_plive[g_np] = best->bytes; g_np++; }
            }
        }
        if ((i & 8191) == 0) { long r_ = rss_kib(); if (r_ > peak_rss) peak_rss = r_; }
        for (int j = 0; j < NSHARD; j++) if (g_shard[j].bytes > peak_buck) peak_buck = g_shard[j].bytes;
    }
    int64_t dt = now_ns() - t0;
    size_t nslab = 0; for (int j = 0; j < NSHARD; j++) for (Slab *s = g_shard[j].slabs; s; s = s->next) nslab++;
    size_t nslab_i = 0; for (Slab *s = g_idx.slabs; s; s = s->next) nslab_i++;
    if (g_np) { int cmp(const void *a, const void *b){ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:(x>y?1:0); }
        qsort(g_pauses, g_np, sizeof(double), cmp);
        printf("  停顿分布us[p50=%.1f p90=%.1f p99=%.1f max=%.1f n=%zu]\n", g_pauses[g_np/2], g_pauses[g_np*9/10], g_pauses[g_np*99/100], g_pauses[g_np-1], g_np); }
    if (g_np) {
        int64_t lo = g_plive[0], hi = g_plive[0]; size_t ilo = 0, ihi = 0;
        for (size_t q = 0; q < g_np; q++) { if (g_plive[q] < lo) { lo = g_plive[q]; ilo = q; } if (g_plive[q] > hi) { hi = g_plive[q]; ihi = q; } }
        printf("  存活字节[p10=%.0fKiB p50=%.0fKiB p90=%.0fKiB max=%.0fKiB]  最小存活那次的停顿=%.1fus  最大存活那次的停顿=%.1fus\n",
               g_plive[0]/1024.0, g_plive[g_np/2]/1024.0, g_plive[g_np*9/10]/1024.0, hi/1024.0, g_pauses[ilo], g_pauses[ihi]);
    }
    printf("%-6s 分片=%-3d N=%-7ld 缓存=%-5d 索引=%-7ld 对象=%-4zu 耗时=%8.1fms 峰值载荷=%7zuKiB 峰值RSS=%8ldKiB 清扫=%-4zu 标记=%zu 最大停顿=%8.1fus 停顿总=%7.2fms 阶段ms[分配%.0f 入边入栈%.2f 走可达%.2f 扫位图%.2f] slab=%zu/%zu RC释放=%zu 清扫释放=%zu 命中=%.1f%%\n",
        "stress", NSHARD, N, g_ncache, indexmax, objsz, dt/1e6, (size_t)peak_buck/1024, peak_rss,
        sweeps, g_marked, max_pause/1000.0, total_pause/1e6, g_t_alloc/1e6, g_t_seed/1e6, g_t_trav/1e6, g_t_scan/1e6,
        nslab, nslab_i, g_rcfreed, g_sweepfreed, g_kills ? 100.0*g_evfired/g_kills : 0.0);
    return 0;
}
