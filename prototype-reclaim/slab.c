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
static int64_t g_t_alloc, g_t_mark, g_t_sweep_slots, g_t_push, g_t_free;
static size_t g_nalloc, g_nfreed, g_marked, g_kills, g_evfired;

/* ---------- 类型表（编译器按类型发射） ---------- */
typedef struct { int nptr; size_t off[2]; } TypeDesc;
typedef struct Obj { int64_t key, val; struct Obj *next; } Obj;   /* 24 字节 */
typedef struct { int64_t next; } Inode;                          /* 占位，见下 */

/* ---------- 对齐 slab ---------- */
#define SLAB_BITS 20                       /* 1 MiB */
#define SLAB_SIZE ((size_t)1 << SLAB_BITS)
typedef struct Slab {
    struct Slab *next;                     /* 本区域的 slab 链 */
    uint64_t *occ, *mark, *rsbit;          /* 位图：占用 / 标记 / 已登记入边 */
    uint8_t  *hold;                        /* 每槽：饱和持有者计数（0..3） */
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

static Region2 *g_reg[4];
static size_t slot_size_of(size_t n);
static Slab *slab_new(uint32_t rid, size_t slot_size);
static void *r2_bump_alloc(Region2 *r, size_t n) {
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
static Slab *slab_new(uint32_t rid, size_t slot_size) {
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
    s->hold = calloc(nslots, 1);
    char *d = SLAB_DATA(s);
    for (size_t i = 0; i < nslots; i++) { *(int64_t *)(d + (i << s->shift)) = (int64_t)(i + 1 < nslots ? i + 1 : -1); }
    s->freelist = nslots ? 0 : -1;         /* 整块串成空闲链（槽号 0 起） */
    return s;
}
static void *r2_alloc(Region2 *r, size_t n) {
    int64_t t0 = now_ns();
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
    g_t_alloc += now_ns() - t0;
    return p;
}
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
static inline void r2_mark(void *p) { Slab *s = slab_of(p); size_t i = slot_of(s, p); s->mark[i >> 6] |= (uint64_t)1 << (i & 63); }
static inline int  r2_marked(void *p) { Slab *s = slab_of(p); size_t i = slot_of(s, p); return (s->mark[i >> 6] >> (i & 63)) & 1; }

/* 顺序扫位图：死槽 = 占用 & ~标记。按 64 位字走，不追指针。 */
static size_t r2_sweep(Region2 *r) {
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
static inline void rs_add(uint32_t rid, void *p) {
    Slab *s = slab_of(p); size_t i = slot_of(s, p);
    if (!s->rsbit) return;
    if ((s->rsbit[i >> 6] >> (i & 63)) & 1) return;         /* 已登记 ⇒ 去重（入边集**持久**，不清空） */
    s->rsbit[i >> 6] |= (uint64_t)1 << (i & 63);
    if (g_rsn[rid] == g_rscap[rid]) { g_rscap[rid] = g_rscap[rid] ? g_rscap[rid]*2 : 256;
        g_rs[rid] = realloc(g_rs[rid], g_rscap[rid]*sizeof(void*)); }
    g_rs[rid][g_rsn[rid]++] = p; (void)g_rsmark_epoch;
}

/* 引用赋值：目标 +1、旧值 −1（饱和三点式，饱和值当"不确定"，永不从饱和值减回去 ⇒ 健全） */
/* 新对象的槽：里面没有旧值（是空闲链的下一槽号），只能"写 + 目标 +1"，不能读旧值 */
static inline void init_ref(Obj **slot, Obj *v) {
    *slot = v;
    if (v) { Slab *s = slab_of(v); size_t i = slot_of(s, v); if (s->hold[i] < 3) s->hold[i]++; }
}
static inline void store_hold(Obj **slot, Obj *v) {
    Obj *old = *slot; *slot = v;
    if (v) { Slab *s = slab_of(v); size_t i = slot_of(s, v); if (s->hold[i] < 3) s->hold[i]++; }
    if (old) { Slab *s = slab_of(old); size_t i = slot_of(s, old); if (s->hold[i] && s->hold[i] < 3) s->hold[i]--; }
}
static inline uint8_t hold_of(Obj *o) { Slab *s = slab_of(o); return s->hold[slot_of(s, o)]; }

/* ---------- 负载：有界符号表 + 缓存（第二持有者）+ 索引（跨区边） ---------- */
#define BUCKETS 64
#define BUCKET_MAX 8
#define TRIGGER (1 << 18)
static Region2 g_buck;    /* 桶区 */
static Region2 g_churn;   /* 解析区（每轮清空） */
static Region2 g_idx;     /* 索引区（跨区冷结构） */
static Obj *g_head[BUCKETS], *g_dummy;
static Obj **g_cache; static int g_ncache, g_cache_every;
static void *g_ihead;

static void **g_stk; static size_t g_sp, g_stkcap;
static inline void mpush(void *o) {
    if (g_sp == g_stkcap) { g_stkcap = g_stkcap ? g_stkcap * 2 : (1 << 16); g_stk = realloc(g_stk, g_stkcap * sizeof(void *)); }
    g_stk[g_sp++] = o;
}
static size_t g_mark_nstop;
/* 从根 + 入边集出发标记；只在 rid 区域内继续走（区域外都当活的） */
static void mark_all(uint32_t rid, void **roots, int nr, void **rsv, size_t nrs) {
    g_sp = 0;
    for (int i = 0; i < nr; i++) if (roots[i]) mpush(roots[i]);
    for (size_t i = 0; i < nrs; i++) if (rsv[i]) mpush(rsv[i]);
    g_mark_nstop = 0;
    while (g_sp) { void *o = g_stk[--g_sp];
        if (r2_marked(o)) continue;
        r2_mark(o); g_marked++;
        if (rid && slab_of(o)->rid != rid) { g_mark_nstop++; continue; }
        Obj *ob = (Obj *)o; if (ob->next) mpush(ob->next); }
}

static int EVENT = 0;
int main(int argc, char **argv) {
    EVENT = getenv("EV") != NULL;
    long N = argc > 1 ? atol(argv[1]) : 1000000;
    g_ncache = argc > 2 ? atoi(argv[2]) : 0;
    g_cache_every = argc > 3 ? atoi(argv[3]) : 64;
    long indexmax = argc > 4 ? atol(argv[4]) : 0;
    size_t objsz = argc > 5 ? (size_t)atol(argv[5]) : sizeof(Obj);

    g_buck.rid = 1; g_churn.rid = 2; g_idx.rid = 3;
    memset(g_head, 0, sizeof g_head);
    if (g_ncache) g_cache = calloc((size_t)g_ncache, sizeof(Obj *));
    int64_t peak_buck = 0, peak_rss = 0, max_pause = 0, total_pause = 0, live_buck = 0;
    size_t sweeps = 0;

    g_reg[1] = &g_buck; g_reg[2] = &g_churn; g_reg[3] = &g_idx;
    static void *roots[8192];
    int64_t t0 = now_ns();
    for (long i = 0; i < N; i++) {
        int b = (int)(i % BUCKETS);
        r2_bump_reset(&g_churn);                       /* 解析区：O(1) 整片回收 */
        Obj *t1 = r2_bump_alloc(&g_churn, objsz), *t2 = r2_bump_alloc(&g_churn, objsz), *t3 = r2_bump_alloc(&g_churn, objsz);
        t1->next = t2; t2->next = t3; t3->next = NULL; t3->val = i;
        for (Obj *q = t1; q; q = q->next) sink += q->val;
        Obj *nd = r2_alloc(&g_buck, objsz);
        nd->key = i; nd->val = i * 3; init_ref(&nd->next, g_head[b]); store_hold(&g_head[b], nd);
        if (g_cache && g_cache_every && (i % g_cache_every) == 0) store_hold(&g_cache[(i / g_cache_every) % g_ncache], nd);
        if ((long)(g_idx.bytes / (int64_t)sizeof(void *)) < indexmax * 2) {   /* 索引项：{next, target} */
            void **ix = r2_bump_alloc(&g_idx, sizeof(void *) * 2);
            ix[0] = g_ihead; ix[1] = nd; g_ihead = ix;
            g_idx.bytes += (int64_t)sizeof(void *) * 2;
            rs_add(1, nd);                             /* 跨区边 3→1：登记到桶区入边集 */
        }
        for (Obj *q = g_head[b]; q; q = q->next) if (q->key == i - 7) { q->val++; break; }
        /* 淘汰尾节点 */
        Obj *prev = NULL, *cur = g_head[b]; int cnt = 0;
        while (cur && cur->next) { prev = cur; cur = cur->next; if (++cnt > BUCKET_MAX) break; }
        if (cur && cnt >= BUCKET_MAX) {
            uint8_t was = hold_of(cur);
            if (prev) store_hold(&prev->next, NULL); else store_hold(&g_head[b], NULL);
            g_kills++;
            if (EVENT && was == 1) { g_evfired++;                                /* 事件回收：唯一持有者就当场还给空闲链 */
                Slab *s = slab_of(cur); size_t idx = slot_of(s, cur);
                r2_free_slot(s, idx); g_nfreed++;
            }
        }
        /* 触发：垃圾 = 当前占用 − 上次清扫后的存活；比的是"要走的可达集" */
        int64_t garbage = g_buck.bytes - live_buck;
        int64_t reach = live_buck + g_idx.bytes;
        int64_t thr = reach > TRIGGER ? reach : TRIGGER;
        if (garbage >= thr) {
            int64_t p0 = now_ns();
            g_marked = 0;
            int nr = 0;
            for (int j = 0; j < BUCKETS; j++) if (g_head[j]) roots[nr++] = g_head[j];
            for (int j = 0; j < g_ncache; j++) if (g_cache[j]) roots[nr++] = g_cache[j];
            int64_t tm = now_ns();
            mark_all(1, roots, nr, g_rs[1], g_rsn[1]);
            g_t_mark += now_ns() - tm;
            r2_sweep(&g_buck);                          /* 顺序扫位图 */
            live_buck = g_buck.bytes;
            int64_t p1 = now_ns();
            if (p1 - p0 > max_pause) max_pause = p1 - p0;
            total_pause += p1 - p0; sweeps++;
            g_t_push += tm - p0;
        }
        if ((i & 8191) == 0) { long r_ = rss_kib(); if (r_ > peak_rss) peak_rss = r_; }
        if (g_buck.bytes > peak_buck) peak_buck = g_buck.bytes;
    }
    int64_t dt = now_ns() - t0;
    size_t nslab = 0; for (Slab *s = g_buck.slabs; s; s = s->next) nslab++;
    size_t nslab_i = 0; for (Slab *s = g_idx.slabs; s; s = s->next) nslab_i++;
    printf("%-6s N=%-8ld 缓存=%-5d 索引=%-7ld 对象=%-4zu 耗时=%8.1fms 峰值载荷=%7zuKiB 峰值RSS=%8ldKiB 清扫=%-4zu 标记=%zu 最大停顿=%8.1fus 停顿总=%7.2fms 阶段ms[分配%.0f 标记%.1f 扫位图%.1f] slab=%zu/%zu 命中=%.1f%%\n",
        EVENT ? "bmev" : "bm", N, g_ncache, indexmax, objsz, dt/1e6, (size_t)peak_buck/1024, peak_rss,
        sweeps, g_marked, max_pause/1000.0, total_pause/1e6, g_t_alloc/1e6, g_t_mark/1e6, g_t_sweep_slots/1e6,
        nslab, nslab_i, g_kills ? 100.0*g_evfired/g_kills : 0.0);
    return 0;
}
