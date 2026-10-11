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

/* ==================== 第 2 段：提案要加的东西 ==================== */
/* 块需要 O(1) 摘链 ⇒ 加一个 `self` 回指针（8 字节/块）。`mark` 供清扫用。 */
typedef struct extc_pblock { struct extc_pblock *prev; struct extc_pblock **self;
                             int fresh; int64_t cap, used; uint32_t mark; char data[1]; } extc_pblock;
#define PB(p) ((extc_pblock *)((char *)(p) - offsetof(extc_pblock, data)))

static void *extc_alloc_solo(extc_arena *a, int64_t n, const char *f, int l) {
    if (n <= 0) n = 1;
    n = (n + 7) & ~(int64_t)7;
    extc_pblock *b = (extc_pblock *)calloc(1, sizeof(extc_pblock) + (size_t)n);
    if (!b) { fprintf(stderr, "%s:%d: trap: out of arena memory\n", f, l); exit(1); }
    b->prev = (extc_pblock *)a->top; b->self = (extc_pblock **)&a->top;
    if (a->top) ((extc_pblock *)a->top)->self = &b->prev;
    b->cap = n; b->used = n; b->fresh = 1; b->mark = 0;
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
typedef struct { int nptr; size_t off[2]; } extc_typedesc;
/* 区域局部清扫：从给的根出发标记；链上没标上的块释放 */
static size_t extc_sweep(extc_arena *a, void **roots, int nroots, const extc_typedesc *td, int64_t *out_bytes) {
    static uint32_t *st[1 << 20]; int sp = 0;
    for (extc_pblock *b = (extc_pblock *)a->top; b; b = b->prev) b->mark = 0;
    for (int i = 0; i < nroots; i++) if (roots[i]) st[sp++] = (uint32_t *)roots[i];
    size_t marked = 0;
    while (sp) { uint32_t *o = st[--sp]; extc_pblock *b = PB(o);
        if (b->mark == 1) continue;
        b->mark = 1; marked++;
        for (int i = 0; i < td->nptr; i++) { void *c = *(void **)((char *)o + td->off[i]); if (c) st[sp++] = (uint32_t *)c; } }
    size_t freed = 0;
    extc_pblock **l = (extc_pblock **)&a->top;
    while (*l) { extc_pblock *b = *l;
        if (b->mark == 1) { b->self = l; l = &b->prev; }
        else { *l = b->prev; if (b->prev) b->prev->self = l; free(b); freed++; } }
    if (out_bytes) { int64_t tot = 0; for (extc_pblock *b = (extc_pblock *)a->top; b; b = b->prev) tot += b->cap; *out_bytes = tot; }
    return marked;
}

typedef struct node_hdr { int64_t key, val; struct node_hdr *next; uint32_t nf; } node_hdr;
typedef node_hdr node;

/* ==================== 第 3 段：按 codegen 的样子手写的工作负载 ==================== */
#define BUCKETS 64
#define BUCKET_MAX 8
#define TRIGGER (262144)
static const extc_typedesc TD_NODE = { 1, { offsetof(node, next), 0 } };

typedef struct { int64_t max_ns, total_ns; size_t n; } pauses;
static void padd(pauses *p, int64_t d) { if (d > p->max_ns) p->max_ns = d; p->total_ns += d; p->n++; }
static int64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000000000LL + t.tv_nsec; }

static long sink;
static int g_mode, g_count;   /* 0=base 1=solo 2=sweep 3=sweepc 4=ev */
static size_t g_sweeps, g_kills, g_evfired, g_fall;
static int64_t g_peak;

/* 存活持有者计数（饱和三点式：只对 <=2 精确；饱和值当"不确定"，永不减回去 ⇒ 健全） */
static void storep(void **slot, void *v) {
    void *old = *slot; *slot = v;
    node_hdr *nv = (node_hdr *)v, *ov = (node_hdr *)old;
    if (nv && nv->nf < 3) nv->nf++;
    if (g_count && ov && ov->nf && ov->nf < 3) ov->nf--;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "base";
    long N = argc > 2 ? atol(argv[2]) : 400000;
    int cacheslots = argc > 3 ? atoi(argv[3]) : 0;      /* 缓存槽数：>0 就制造第二持有者 + 跨区边 */
    int cacheevery = argc > 4 ? atoi(argv[4]) : 64;
    g_mode = !strcmp(mode,"solo") ? 1 : !strcmp(mode,"sweep") ? 2
           : !strcmp(mode,"sweepc") ? 3 : !strcmp(mode,"ev") ? 4 : 0;
    g_count = (g_mode == 3 || g_mode == 4);
    const int solo = g_mode >= 1, dolive = (g_mode == 2 || g_mode == 3 || g_mode == 4);

    extc_arena __extc_a[4] = {0};
    node *head[BUCKETS]; memset(head, 0, sizeof head);
    node **cache = cacheslots ? calloc((size_t)cacheslots, sizeof(node *)) : NULL;
    int ncache = cacheslots;
    pauses P = {0};
    int64_t live1 = 0, arena1 = 0, trig = TRIGGER;

    int64_t t0 = now_ns();
    for (long i = 0; i < N; i++) {
        int b = (int)(i % BUCKETS);
        extc_arena_release(&__extc_a[2]);                     /* 解析区：每轮整片释放 */
        node *t1,*t2,*t3;
        if (solo) { t1=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",7);
                    t2=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",8);
                    t3=extc_alloc_solo(&__extc_a[2],sizeof(node),"sim.extc",9); }
        else      { t1=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",7);
                    t2=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",8);
                    t3=extc_arena_alloc(&__extc_a[2],sizeof(node),"sim.extc",9); }
        t1->next=t2; t2->next=t3; t3->next=NULL; t3->val=i;
        for (node *q=t1;q;q=q->next) sink += q->val;

        node *nd = solo ? extc_alloc_solo(&__extc_a[1],sizeof(node),"sim.extc",14)
                        : extc_arena_alloc(&__extc_a[1],sizeof(node),"sim.extc",14);
        nd->key=i; nd->val=i*3;
        storep((void**)&nd->next, head[b]); storep((void**)&head[b], nd);
        live1 += (int64_t)sizeof(node); arena1 += (int64_t)sizeof(node);

        /* 最恶心的一步：刚插入的节点**同时**被另一个区域里的缓存槽持有 */
        if (cache && cacheevery && (i % cacheevery) == 0) {
            int j = (int)((i / cacheevery) % ncache);
            storep((void**)&cache[j], nd);
        }
        for (node *q=head[b];q;q=q->next) if (q->key == i-7) { q->val++; break; }

        /* 淘汰尾节点（链表手术） */
        node *prev=NULL,*cur=head[b]; int cnt=0;
        while (cur && cur->next) { prev=cur; cur=cur->next; if (++cnt > BUCKET_MAX) break; }
        if (cur && cnt >= BUCKET_MAX) {
            uint32_t was = cur->nf;                       /* 摘掉持有者**之前**看一眼 */
            if (prev) storep((void**)&prev->next, NULL); else storep((void**)&head[b], NULL);
            g_kills++; live1 -= (int64_t)sizeof(node);
            if (g_mode == 4 && was == 1) { extc_free_solo(&__extc_a[1], cur); arena1 -= (int64_t)sizeof(node); g_evfired++; }
            else g_fall++;
        }
        if (dolive && arena1 >= trig) {
            int64_t a = now_ns(), after = 0;
            void *roots[BUCKETS + 4096]; int nr = 0;
            for (int j=0;j<BUCKETS;j++) roots[nr++]=head[j];
            for (int j=0;j<ncache;j++) roots[nr++]=cache[j];     /* 跨区根：不这么做就不健全 */
            extc_sweep(&__extc_a[1], roots, nr, &TD_NODE, &after);
            arena1 = after; trig = after > TRIGGER ? after : TRIGGER;
            g_sweeps++; padd(&P, now_ns()-a);
        }
        if (arena1 > g_peak) g_peak = arena1;
    }
    int64_t dt = now_ns() - t0;
    extc_arena_destroy(&__extc_a[0]); extc_arena_destroy(&__extc_a[1]);
    extc_arena_release(&__extc_a[2]); extc_arena_destroy(&__extc_a[2]);
    printf("%-7s N=%-8ld 缓存=%-6d 每%-5d 耗时=%8.1fms 峰值=%7zuKiB 清扫=%-5zu 删除=%-7zu 事件回收=%-7zu 落空=%-7zu 命中率=%5.1f%% 最大停顿=%8.1fus 停顿总=%7.2fms\n",
        mode, N, ncache, cacheevery, dt/1e6, (size_t)g_peak/1024, g_sweeps, g_kills,
        g_evfired, g_fall, g_kills? 100.0*g_evfired/g_kills : 0.0, P.max_ns/1000.0, P.total_ns/1e6);
    return 0;
}
