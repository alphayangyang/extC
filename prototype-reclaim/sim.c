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

/* ==================== 第 3 段：按 codegen 的样子手写的工作负载 ==================== */
/* 符号表：BUCKETS 个桶，每桶最多 BUCKET_MAX 条，满了淘汰尾节点。 */
#define BUCKETS 64
#define BUCKET_MAX 8
typedef struct node { int64_t key, val; struct node *next; } node;
static const extc_typedesc TD_NODE = { 1, { offsetof(node, next), 0 } };

typedef struct { int64_t max_ns, total_ns; size_t n; } pauses;
static void padd(pauses *p, int64_t d) { if (d > p->max_ns) p->max_ns = d; p->total_ns += d; p->n++; }
static int64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000000000LL + t.tv_nsec; }

static long sink;
static int g_mode;                      /* 0=base 1=solo 2=sweep 3=ev */
static size_t g_peak_block, g_live_bytes, g_sweeps, g_evicted, g_kills, g_evfired;
#define TRIGGER (262144)

static void note_alloc(void) { g_live_bytes += sizeof(node); if (g_live_bytes > g_peak_block) g_peak_block = g_live_bytes; }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "base";
    long N = argc > 2 ? atol(argv[2]) : 400000;
    g_mode = !strcmp(mode, "sweep") ? 2 : !strcmp(mode, "ev") ? 3 : !strcmp(mode, "solo") ? 1 : 0;
    const int solo = g_mode >= 1;

    extc_arena __extc_a[4] = {0};                 /* 生成物里就是这个形状 */
    node *head[BUCKETS]; memset(head, 0, sizeof head);
    pauses P = {0};

    int64_t live1 = 0, g_peak_long = 0, arena1 = 0, trig = TRIGGER;
    int64_t t0 = now_ns();
    for (long i = 0; i < N; i++) {
        int b = (int)(i % BUCKETS);
        /* 解析阶段：3 个临时节点，活在本轮的作用域里 */
        extc_arena_release(&__extc_a[2]);
        node *t1, *t2, *t3;
        if (solo) { t1 = extc_alloc_solo(&__extc_a[2], sizeof(node), "sim.extc", 7);
                    t2 = extc_alloc_solo(&__extc_a[2], sizeof(node), "sim.extc", 8);
                    t3 = extc_alloc_solo(&__extc_a[2], sizeof(node), "sim.extc", 9); }
        else      { t1 = extc_arena_alloc(&__extc_a[2], sizeof(node), "sim.extc", 7);
                    t2 = extc_arena_alloc(&__extc_a[2], sizeof(node), "sim.extc", 8);
                    t3 = extc_arena_alloc(&__extc_a[2], sizeof(node), "sim.extc", 9); }
        t1->next = t2; t2->next = t3; t3->next = NULL; t3->val = i; note_alloc(); note_alloc(); note_alloc();
        for (node *q = t1; q; q = q->next) sink += q->val;      /* 指针追逐 */
        /* 插入：分配在桶自己的区域里（自包含） */
        node *nd = solo ? extc_alloc_solo(&__extc_a[1], sizeof(node), "sim.extc", 14)
                        : extc_arena_alloc(&__extc_a[1], sizeof(node), "sim.extc", 14);
        nd->key = i; nd->val = i * 3; nd->next = head[b]; head[b] = nd;
        live1 += (int64_t)sizeof(node); arena1 += (int64_t)sizeof(node); note_alloc();
        /* 查询：沿链找 key，命中改值 */
        for (node *q = head[b]; q; q = q->next) if (q->key == i - 7) { q->val++; break; }
        /* 淘汰尾节点（链表手术：前驱的 next 跳过它） */
        node *prev = NULL, *cur = head[b]; int cnt = 0;
        while (cur && cur->next) { prev = cur; cur = cur->next; if (++cnt > BUCKET_MAX) break; }
        if (cur && cnt >= BUCKET_MAX) {
            if (prev) prev->next = NULL; else head[b] = NULL;
            g_kills++; g_evicted++;
            if (g_mode == 3) { extc_free_solo(&__extc_a[1], cur); live1 -= (int64_t)sizeof(node); arena1 -= (int64_t)sizeof(node); g_evfired++; }
            else { live1 -= (int64_t)sizeof(node); }   /* 留作垃圾：占用不减 */
        }
        /* 回收：触发量是 O(1) 读的计数器，不是走链 */
        if (g_mode >= 2) {
            if (arena1 >= trig) {                    /* 触发量 = max(上次清扫后的存活, 下限) */
                int64_t a = now_ns(), after = 0;
                void *roots[BUCKETS]; for (int j = 0; j < BUCKETS; j++) roots[j] = head[j];
                extc_sweep(&__extc_a[1], roots, BUCKETS, &TD_NODE, &after);
                arena1 = after; live1 = after > 0 ? live1 : live1;
                trig = after > TRIGGER ? after : TRIGGER;
                g_sweeps++; padd(&P, now_ns() - a);
            }
        }
        if (arena1 > g_peak_long) g_peak_long = arena1;
    }
    int64_t dt = now_ns() - t0;
    extc_arena_destroy(&__extc_a[0]); extc_arena_destroy(&__extc_a[1]);
    extc_arena_release(&__extc_a[2]); extc_arena_destroy(&__extc_a[2]);

    printf("%-6s N=%-8ld 耗时=%7.1fms 峰值=%6zuKiB 清扫=%-5zu 淘汰=%-7zu 事件回收=%-7zu 最大停顿=%8.1fus 停顿总=%7.2fms\n",
           mode, N, dt / 1e6, (size_t)g_peak_long / 1024, g_sweeps, g_evicted, g_evfired,
           P.max_ns / 1000.0, P.total_ns / 1e6);
    return 0;
}
