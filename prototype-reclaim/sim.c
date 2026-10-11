/* 手写的「生成物」—— 用来量提案的代价，形状与 extc 真的发射出来的 C 一致。
 *
 * 第 1 段是**逐字**取自 extc 对一个小程序输出的 arena 运行时（注释压掉了）。
 * 第 2 段是提案要加的东西：一块一对象、块转交、区域局部清扫、事件回收。
 * 第 3 段是按 codegen 的样子手写的工作负载：`extc_arena __extc_a[k]`、每轮
 * `extc_arena_release(&__extc_a[热区])`、分配带 `"sim.extc", 行号`、函数收 `__extc_home`。
 *
 * 模式（argv[1]）：
 *   base    今天的生成物（打包块 + 区域整片释放）
 *   solo    提案的块布局（一块一对象），不回收
 *   sweep   solo + 区域局部清扫
 *   ev      sweep + 事件回收（摘链当场释放）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* ==================== 第 1 段：真实生成的运行时（逐字） ==================== */
#define EXTC_MALLOC __attribute__((malloc))
#define EXTC_NOINLINE __attribute__((noinline))
#define EXTC_ARENA_SPARE_MAX (1 << 20)
typedef struct extc_ablock { struct extc_ablock *prev; int fresh; int64_t cap, used; char data[1]; } extc_ablock;
typedef struct extc_arena { extc_ablock *top; extc_ablock *spare; } extc_arena;

static inline void extc_arena_release(extc_arena *a) {
    while (a->top) {
        extc_ablock *p = a->top->prev;
        if (!a->spare && a->top->cap <= EXTC_ARENA_SPARE_MAX) {
            a->spare = a->top; a->spare->prev = NULL; a->spare->fresh = 0;
        } else {
            free(a->top);
        }
        a->top = p;
    }
}
static inline void extc_arena_destroy(extc_arena *a) {
    extc_arena_release(a);
    if (a->spare) { free(a->spare); a->spare = NULL; }
}
static EXTC_MALLOC EXTC_NOINLINE void *extc_arena_alloc(extc_arena *a, int64_t n, const char *f, int l) {
    if (n <= 0) n = 1;
    n = (n + 7) & ~(int64_t)7;
    if (!a->top && a->spare) { a->top = a->spare; a->spare = NULL; a->top->used = 0; }
    if (!a->top || a->top->cap - a->top->used < n) {
        int64_t cap = n > 64 ? n : 64;
        if (a->top && a->top->cap < (INT64_C(1) << 20) && cap < a->top->cap * 2) cap = a->top->cap * 2;
        extc_ablock *b = (extc_ablock *)calloc(1, sizeof(extc_ablock) + (size_t)cap);
        if (!b) { fprintf(stderr, "%s:%d: trap: out of arena memory (%lld bytes)\n", f, l, (long long)n); exit(1); }
        b->prev = a->top; b->cap = cap; b->used = 0; b->fresh = 1; a->top = b;
    }
    { void *p = a->top->data + a->top->used; a->top->used += n;
      if (!a->top->fresh) memset(p, 0, (size_t)n);
      return p; }
}

/* 标记栈：动态增长（固定大小会在大结构上溢出 —— 实测过） */
static void **MK; static size_t MKN, MKCAP;
static uint32_t g_epoch = 0;   /* 标记世代：跨区遍历时不必清别的区域的标记位 */
static inline void mk_push(void *o) {
    if (MKN == MKCAP) { MKCAP = MKCAP ? MKCAP * 2 : (1 << 16); MK = realloc(MK, MKCAP * sizeof(void *)); }
    MK[MKN++] = o;
}

/* ==================== 第 2 段：提案要加的东西 ==================== */
/* 块需要 O(1) 摘链 ⇒ 加一个 `self` 回指针（8 字节/块）。`mark` 供清扫用。 */
typedef struct { int nptr; size_t off[2]; } extc_typedesc;
typedef struct extc_pblock { struct extc_pblock *prev; struct extc_pblock **self;
                             int fresh; int64_t cap, used; uint32_t mark; uint32_t rid; uint32_t rsmark; uint32_t vmark; const extc_typedesc *td; char data[1]; } extc_pblock;
#define PB(p) ((extc_pblock *)((char *)(p) - offsetof(extc_pblock, data)))

static uint32_t g_alloc_rid;
static void *extc_alloc_solo(extc_arena *a, int64_t n, const char *f, int l, const extc_typedesc *td) {
    if (n <= 0) n = 1;
    n = (n + 7) & ~(int64_t)7;
    extc_pblock *b = (extc_pblock *)calloc(1, sizeof(extc_pblock) + (size_t)n);
    if (!b) { fprintf(stderr, "%s:%d: trap: out of arena memory\n", f, l); exit(1); }
    b->prev = (extc_pblock *)a->top; b->self = (extc_pblock **)&a->top;
    if (a->top) ((extc_pblock *)a->top)->self = &b->prev;
    b->cap = n; b->used = n; b->fresh = 1; b->mark = 0; b->td = td; b->rid = g_alloc_rid; b->rsmark = 0; b->vmark = 0;
    a->top = (extc_ablock *)b;
    return (void *)((char *)b + offsetof(extc_pblock, data));
}
static void extc_free_solo(extc_arena *a, void *p) {          /* O(1) 摘链 */
    extc_pblock *b = PB(p);
    if (b->self) *b->self = b->prev;
    if (b->prev) b->prev->self = b->self;
    if (a->top == (extc_ablock *)b) a->top = (extc_ablock *)b->prev;
    free(b);
}
/* 块转交：把 p 所在的整块从 from 挂到 to（对象地址不变） */
__attribute__((unused)) static void extc_promote(extc_arena *from, extc_arena *to, void *p) {
    extc_pblock *b = PB(p);
    if (b->self) *b->self = b->prev;
    if (b->prev) b->prev->self = b->self;
    if (from->top == (extc_ablock *)b) from->top = (extc_ablock *)b->prev;
    b->prev = (extc_pblock *)to->top; b->self = (extc_pblock **)&to->top;
    if (to->top) ((extc_pblock *)to->top)->self = &b->prev;
    to->top = (extc_ablock *)b;
}
/* 类型表：哪些偏移是指针（编译器按类型发射） */
/* 区域局部清扫：从给的根出发标记；链上没标上的块释放 */
static size_t extc_sweep(extc_arena *a, void **roots, int nroots, const extc_typedesc *td, int64_t *out_bytes, size_t *out_marked) {
    (void)td;
    uint32_t e = ++g_epoch;               /* 本世代；不清理任何标记位 */
    MKN = 0;
    for (int i = 0; i < nroots; i++) if (roots[i]) mk_push(roots[i]);
    size_t marked = 0;
    while (MKN) { void *o = MK[--MKN]; extc_pblock *b = PB(o);
        if (b->mark == e) continue;
        b->mark = e; marked++;
        int np = b->td ? b->td->nptr : 0;
        for (int i = 0; i < np; i++) { void *c = *(void **)((char *)o + b->td->off[i]); if (c) mk_push(c); } }
    extc_pblock **l = (extc_pblock **)&a->top;
    while (*l) { extc_pblock *b = *l;
        if (b->mark == e) { b->self = l; l = &b->prev; }
        else { *l = b->prev; if (b->prev) b->prev->self = l; free(b); } }
    if (out_bytes) { int64_t tot = 0; for (extc_pblock *b = (extc_pblock *)a->top; b; b = b->prev) tot += b->cap; *out_bytes = tot; }
    if (out_marked) *out_marked = marked;
    return marked;
}

/* 全局 GC 用：清两个区域的标记、标记一次、两个区域一起扫 */
static void extc_sweep_global(extc_arena *a, extc_arena *b, void **roots, int nroots,
                              const extc_typedesc *td, int64_t *out_bytes, size_t *out_marked) {
    (void)td;
    uint32_t e = ++g_epoch;
    MKN = 0;
    for (int i = 0; i < nroots; i++) if (roots[i]) mk_push(roots[i]);
    size_t marked = 0;
    while (MKN) { void *o = MK[--MKN]; extc_pblock *pb = PB(o);
        if (pb->mark == e) continue;
        pb->mark = e; marked++;
        int np = pb->td ? pb->td->nptr : 0;
        for (int i = 0; i < np; i++) { void *c = *(void **)((char *)o + pb->td->off[i]); if (c) mk_push(c); } }
    int64_t tot = 0;
    for (int which = 0; which < 2; which++) {
        extc_arena *ar = which ? b : a;
        extc_pblock **l = (extc_pblock **)&ar->top;
        while (*l) { extc_pblock *pb = *l; if (pb->mark == e) { pb->self = l; l = &pb->prev; }
            else { *l = pb->prev; if (pb->prev) pb->prev->self = l; free(pb); } }
        for (extc_pblock *pb = (extc_pblock *)ar->top; pb; pb = pb->prev) tot += pb->cap;
    }
    if (out_bytes) *out_bytes = tot;
    if (out_marked) *out_marked = marked;
}

/* ---- 记忆集（G1 的做法）：每个区域一张"谁指进我"的集 ---- */
#define NREG 4
static void **RS[NREG]; static size_t RSN[NREG], RSCAP[NREG];
static uint32_t g_rs_epoch = 1;   /* 初值必须非 0：rsmark 初值是 0，否则第一次就去重掉 */
static void rs_add(uint32_t rid, void *v) {
    extc_pblock *b = PB(v);
    if (b->rsmark == g_rs_epoch) return;              /* 本轮已记过 ⇒ 天然去重 */
    b->rsmark = g_rs_epoch;
    if (RSN[rid] == RSCAP[rid]) { RSCAP[rid] = RSCAP[rid] ? RSCAP[rid]*2 : 256;
        RS[rid] = realloc(RS[rid], RSCAP[rid]*sizeof(void*)); }
    RS[rid][RSN[rid]++] = v;
}
/* 跨区写：目标区域 != 槽所在区域 ⇒ 登记到目标区域的入边集 */
static void store_rs(void **slot, void *v, uint32_t slot_rid) {
    *slot = v;
    if (v && PB(v)->rid != slot_rid) rs_add(PB(v)->rid, v);
}
/* 扫一个区域：只从"本区域自己的根 + 本区域的入边集"标记；碰到区域外的对象就停。 */
static size_t g_rs_bad;   /* 自检异常：可达、属本区域、却没被本区域标记覆盖 */
static size_t sweep_rs(uint32_t rid, extc_arena *a, void **own_roots, int nroot,
                       void **all_roots, int nall, int64_t *out_bytes, size_t *out_marked, int verify) {
    uint32_t e = ++g_epoch;
    MKN = 0;
    for (int i = 0; i < nroot; i++) if (own_roots[i]) mk_push(own_roots[i]);
    for (size_t i = 0; i < RSN[rid]; i++) mk_push(RS[rid][i]);
    size_t marked = 0;
    while (MKN) { void *o = MK[--MKN]; extc_pblock *b = PB(o);
        if (b->mark == e) continue;
        b->mark = e; marked++;
        if (b->rid != rid) continue;                  /* 区域外的对象都当活的：不往下走 */
        int np = b->td ? b->td->nptr : 0;
        for (int i = 0; i < np; i++) { void *c = *(void **)((char *)o + b->td->off[i]); if (c) mk_push(c); } }
    if (verify) {                                   /* 全根独立走一遍：本区域可达却没被标记 ⇒ 入边集不完备 */
        uint32_t ve = ++g_epoch; MKN = 0;
        for (int i = 0; i < nall; i++) if (all_roots[i]) mk_push(all_roots[i]);
        while (MKN) { void *o = MK[--MKN]; extc_pblock *b = PB(o);
            if (b->vmark == ve) continue;
            b->vmark = ve;
            if (b->rid == rid && b->mark != e) g_rs_bad++;
            int np = b->td ? b->td->nptr : 0;
            for (int i = 0; i < np; i++) { void *c = *(void **)((char *)o + b->td->off[i]); if (c) mk_push(c); } } }
    extc_pblock **l = (extc_pblock **)&a->top;
    while (*l) { extc_pblock *b = *l; if (b->mark == e) { b->self = l; l = &b->prev; }
        else { *l = b->prev; if (b->prev) b->prev->self = l; free(b); } }
    if (out_bytes) { int64_t tot = 0; for (extc_pblock *b = (extc_pblock *)a->top; b; b = b->prev) tot += b->cap; *out_bytes = tot; }
    if (out_marked) *out_marked = marked;
    return marked;
}

/* ==================== 第 3 段：按 codegen 的样子手写的工作负载 ==================== */
#define BUCKETS 64
#define BUCKET_MAX 8
#define TRIGGER (262144)
static int RS_VERIFY = 0;   /* 自检开关：从全根独立复核一遍，看入边集是否完备 */
typedef struct node_hdr { int64_t key, val; struct node_hdr *next; uint32_t nf; } node_hdr;
typedef node_hdr node;
/* 索引项住在**另一个区域**，指向被扫区域里的节点 ⇒ 真正的跨区边：
 * 扫 a1 时必须穿过 a3 的整条链，否则不健全。 */
typedef struct inode { struct inode *next; node *target; } inode;
static const extc_typedesc TD_NODE  = { 1, { offsetof(node, next), 0 } };
static const extc_typedesc TD_INODE = { 2, { offsetof(inode, next), offsetof(inode, target) } };

typedef struct { int64_t max_ns, total_ns; size_t n; } pauses;
static void padd(pauses *p, int64_t d) { if (d > p->max_ns) p->max_ns = d; p->total_ns += d; p->n++; }
static int64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000000000LL + t.tv_nsec; }

static long sink;
static int g_mode, g_count;
static size_t g_sweeps, g_kills, g_evfired, g_fall, g_marked;

static void storep(void **slot, void *v) {
    void *old = *slot; *slot = v;
    node_hdr *nv = (node_hdr *)v, *ov = (node_hdr *)old;
    if (nv && nv->nf < 3) nv->nf++;
    if (g_count && ov && ov->nf && ov->nf < 3) ov->nf--;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "base";
    long N = argc > 2 ? atol(argv[2]) : 400000;
    int cacheslots = argc > 3 ? atoi(argv[3]) : 0;
    int cacheevery = argc > 4 ? atoi(argv[4]) : 64;
    long indexmax  = argc > 5 ? atol(argv[5]) : 0;      /* 索引区规模（跨区冷结构） */
    int objsz      = argc > 6 ? atoi(argv[6]) : (int)sizeof(node);   /* 对象放大 */

    g_mode = !strcmp(mode,"solo") ? 1 : !strcmp(mode,"sweep") ? 2
           : !strcmp(mode,"sweepc") ? 3 : !strcmp(mode,"ev") ? 4
           : !strcmp(mode,"gc") ? 5 : !strcmp(mode,"rs") ? 6 : !strcmp(mode,"rsev") ? 7 : 0;
    const int gl = (g_mode == 5);            /* 真全局 GC：一个堆，全部东西都在里面 */
    g_count = (g_mode == 3 || g_mode == 4 || g_mode == 7);
    RS_VERIFY = getenv("VERIFY") != NULL;
    const int solo = g_mode >= 1, dolive = (g_mode >= 2);

    extc_arena __extc_a[4] = {0};
    node *head[BUCKETS]; memset(head, 0, sizeof head);
    node **cache = cacheslots ? calloc((size_t)cacheslots, sizeof(node *)) : NULL;
    int ncache = cacheslots;
    inode *ihead = NULL;                                 /* 索引链的头（a3 的根） */
    size_t ni = 0;
    pauses P = {0};
    int64_t arena1 = 0, arena3 = 0, live1 = 0, peak1 = 0, peak3 = 0;

    int64_t t0 = now_ns();
    for (long i = 0; i < N; i++) {
        int b = (int)(i % BUCKETS);
        g_alloc_rid = 2;                                  /* 解析区 */
        if (!gl) extc_arena_release(&__extc_a[2]);   /* 全局 GC：没有每轮整片释放这回事 */
        node *t1,*t2,*t3;
        if (gl) { t1=extc_alloc_solo(&__extc_a[1],sizeof(node),"sim.extc",7,&TD_NODE);
                  t2=extc_alloc_solo(&__extc_a[1],sizeof(node),"sim.extc",8,&TD_NODE);
                  t3=extc_alloc_solo(&__extc_a[1],sizeof(node),"sim.extc",9,&TD_NODE);
                  arena1 += 3*(int64_t)sizeof(node); }
        else if (solo) { t1=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",7,&TD_NODE);
                    t2=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",8,&TD_NODE);
                    t3=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",9,&TD_NODE); }
        else      { t1=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",7);
                    t2=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",8);
                    t3=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",9); }
        t1->next=t2; t2->next=t3; t3->next=NULL; t3->val=i;
        for (node *q=t1;q;q=q->next) sink += q->val;

        g_alloc_rid = 1;                                  /* 桶区 */
        node *nd = solo ? extc_alloc_solo(&__extc_a[1], objsz, "sim.extc", 14, &TD_NODE)
                        : extc_arena_alloc(&__extc_a[1], objsz, "sim.extc", 14);
        nd->key=i; nd->val=i*3;
        storep((void**)&nd->next, head[b]); storep((void**)&head[b], nd);
        arena1 += objsz;

        if (cache && cacheevery && (i % cacheevery) == 0)
            storep((void**)&cache[(i / cacheevery) % ncache], nd);

        /* 索引：慢慢长起来，长满 indexmax 为止；每一项指向本轮的节点 ⇒ 跨区边 */
        if ((long)ni < indexmax) {
            g_alloc_rid = 3;                              /* 索引区 */
            inode *ix = solo ? extc_alloc_solo(&__extc_a[3], sizeof(inode), "sim.extc", 20, &TD_INODE)
                             : extc_arena_alloc(&__extc_a[3], sizeof(inode), "sim.extc", 20);
            ix->next = ihead; ihead = ix;
            store_rs((void**)&ix->target, nd, 3);   /* 跨区写 3→1：登记到桶区的入边集 */
            ni++;
            arena3 += (int64_t)sizeof(inode);
        }
        for (node *q=head[b];q;q=q->next) if (q->key == i-7) { q->val++; break; }

        node *prev=NULL,*cur=head[b]; int cnt=0;
        while (cur && cur->next) { prev=cur; cur=cur->next; if (++cnt > BUCKET_MAX) break; }
        if (cur && cnt >= BUCKET_MAX) {
            uint32_t was = cur->nf;
            if (prev) storep((void**)&prev->next, NULL); else storep((void**)&head[b], NULL);
            g_kills++;
            if ((g_mode == 4 || g_mode == 7) && was == 1) { extc_free_solo(&__extc_a[1], cur); arena1 -= objsz; g_evfired++; }
            else g_fall++;
        }
        /* 触发量 = **垃圾量** vs **要走的可达集**：标记成本 ∝ 可达集，所以垃圾攒到与可达集
         * 同量级才值得扫。拿占用去比就会每轮都触发。 */
        int64_t occ = arena1 + (gl ? arena3 : 0), garbage = occ - live1;
        int64_t reach = gl ? live1 : (live1 + arena3);
        int64_t thr = reach > TRIGGER ? reach : TRIGGER;
        if (dolive && garbage >= thr) {
            int64_t a = now_ns(), after = 0; size_t mk = 0;
            void *roots[BUCKETS + 8192]; int nr = 0;
            for (int j=0;j<BUCKETS;j++) roots[nr++]=head[j];
            for (int j=0;j<(ncache<8000?ncache:8000);j++) roots[nr++]=cache[j];
            roots[nr++] = ihead;                        /* 索引链的根：不加它就不健全 */
            if (g_mode == 6 || g_mode == 7) {
                void *own[BUCKETS + 8192]; int no = 0;
                for (int j=0;j<BUCKETS;j++) own[no++]=head[j];
                for (int j=0;j<(ncache<8000?ncache:8000);j++) own[no++]=cache[j];
                sweep_rs(1, &__extc_a[1], own, no, roots, nr, &after, &mk, RS_VERIFY);
                        } else if (gl) { extc_sweep_global(&__extc_a[1], &__extc_a[3], roots, nr, &TD_NODE, &after, &mk);
                int64_t t3 = 0; for (extc_pblock *pb=(extc_pblock*)__extc_a[3].top; pb; pb=pb->prev) t3 += pb->cap;
                arena3 = t3; }
            else extc_sweep(&__extc_a[1], roots, nr, &TD_NODE, &after, &mk);
            g_marked += mk;
            if (gl) { arena1 = after - arena3; live1 = after; }
            else    { arena1 = after; live1 = after; }
            g_sweeps++; padd(&P, now_ns()-a);
        }
        if (arena1 > peak1) peak1 = arena1;   /* gc 模式下 a1 就是整个堆 */
        if (arena3 > peak3) peak3 = arena3;
    }
    int64_t dt = now_ns() - t0;
    extc_arena_destroy(&__extc_a[0]); extc_arena_destroy(&__extc_a[1]);
    extc_arena_release(&__extc_a[2]); extc_arena_destroy(&__extc_a[2]);
    extc_arena_destroy(&__extc_a[3]);
    printf("%-6s N=%-7ld 共享=%-5d 索引=%-7ld 对象=%-5d 耗时=%8.1fms 峰值总计=%8zuKiB 清扫=%-4zu 标记总量=%-10zu 入边集异常=%-4zu 最大停顿=%9.1fus 停顿总=%7.2fms 命中=%5.1f%%\n",
        mode, N, ncache, indexmax, objsz, dt/1e6, (size_t)(peak1+peak3)/1024,
        g_sweeps, g_marked, g_rs_bad, P.max_ns/1000.0, P.total_ns/1e6, g_kills?100.0*g_evfired/g_kills:0.0);
    return 0;
}
