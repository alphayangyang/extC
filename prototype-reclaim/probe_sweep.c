/* 最终测量：触发量 = max(上次清扫后存活字节, 下限)；5 次取最小 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef struct Blk { struct Blk *prev; uint32_t mark; size_t cap; } Blk;
typedef struct { Blk *top; size_t since, live, peak, sweeps, live_after; } Region;
typedef struct Node { int64_t v; struct Node *next; } Node;

static void *ralloc(Region *r, size_t n) {
    Blk *b = malloc(sizeof(Blk) + n);
    b->prev = r->top; b->mark = 0; b->cap = n;
    r->top = b; r->since += n; r->live += n;
    if (r->live > r->peak) r->peak = r->live;
    return (void *)(b + 1);
}
static void rrelease(Region *r) { while (r->top) { Blk *b = r->top; r->top = b->prev; r->live -= b->cap; free(b); } r->since = 0; }

static void sweep(Region *r, Node **roots, int nr) {
    for (Blk *b = r->top; b; b = b->prev) b->mark = 0;
    static Node *st[1000000]; int sp = 0;
    for (int i = 0; i < nr; i++) if (roots[i]) st[sp++] = roots[i];
    while (sp) { Node *p = st[--sp]; Blk *b = ((Blk *)p) - 1; if (b->mark) continue; b->mark = 1; if (p->next) st[sp++] = p->next; }
    Blk **link = &r->top;
    while (*link) { Blk *b = *link; if (b->mark) link = &b->prev; else { *link = b->prev; r->live -= b->cap; free(b); } }
    r->since = 0; r->live_after = r->live; r->sweeps++;
}

static long workload(int n, size_t floor, int live_nodes, size_t *sw, int collect) {
    Region ll = {0}, loc = {0};
    Node *live_root = NULL;
    for (int i = 0; i < live_nodes; i++) { Node *p = ralloc(&ll, sizeof(Node)); p->v = i; p->next = live_root; live_root = p; }
    Node *saved = NULL; long sum = 0;
    for (int i = 0; i < n; i++) {
        Node *head = NULL;
        for (int k = 0; k < 4; k++) { Node *p = ralloc(&loc, sizeof(Node)); p->v = i+k; p->next = head; head = p; }
        sum += head->v;
        Node *esc = ralloc(&ll, sizeof(Node)); esc->v = i; esc->next = NULL; saved = esc;
        rrelease(&loc);
        if (collect) { size_t trig = ll.live_after > floor ? ll.live_after : floor;
                       if (ll.since >= trig) { Node *roots[2] = { live_root, saved }; sweep(&ll, roots, 2); } }
    }
    long peak = (long)ll.peak; *sw = ll.sweeps;
    rrelease(&ll); rrelease(&loc);
    return peak + sum*0;
}
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000000000LL + t.tv_nsec; }

int main(void) {
    const int N = 1000000;
    size_t FL[] = {1u<<14, 1u<<18, 1u<<22};
    int LIVES[] = {0, 1000, 10000, 100000};
    printf("== 触发量 = max(存活量, 下限)；n=%d，每轮 1 逃逸垃圾 + 4 本地 ==\n\n", N);
    printf("%-9s %-9s %10s %10s %10s %11s %8s\n", "存活集", "下限", "基线(ms)", "有清扫", "开销", "峰值(KiB)", "清扫数");
    for (int li = 0; li < 4; li++) {
        int64_t b0 = 0;
        for (int rep = 0; rep < 5; rep++) { size_t sw; int64_t a = now_ns(); workload(N, 0, LIVES[li], &sw, 0); int64_t b = now_ns(); if (!b0 || b-a < b0) b0 = b-a; }
        for (int ti = 0; ti < 3; ti++) {
            int64_t b1 = 0, pk = 0; size_t swbest = 0;
            for (int rep = 0; rep < 5; rep++) {
                size_t sw; int64_t a = now_ns(); long pk1 = workload(N, FL[ti], LIVES[li], &sw, 1); int64_t b = now_ns();
                if (!b1 || b-a < b1) { b1 = b-a; pk = pk1; swbest = sw; }
            }
            printf("%-9d %-9zu %10.1f %10.1f %9.2f%% %11ld %8zu\n",
                   LIVES[li], FL[ti], b0/1e6, b1/1e6, 100.0*b1/b0-100, pk/1024, swbest);
        }
        printf("\n");
    }
    return 0;
}
