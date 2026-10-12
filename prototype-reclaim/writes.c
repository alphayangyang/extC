/* 写密集负载：预分配一批节点串成链，之后每轮做大量**改链**（约 6 次引用赋值），
   每 ALLOCEVERY 轮才分配 1 个新节点。用来量"每次引用赋值维护持有者计数"这个始终在跑的开销。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#define CHAINS 64
#ifdef NOCOUNT
typedef struct Nd { int64_t key, val; struct Nd *next, *prev; } Nd;          /* 32 字节 */
#else
typedef struct Nd { int64_t key, val; struct Nd *next, *prev; uint16_t hold; } Nd; /* 40 字节 */
#endif
static Nd *head[CHAINS], *tail[CHAINS];
static long sink;
#ifdef NOCOUNT
#  define REF(p)   ((void)0)
#  define UNREF(p) ((void)0)
#else
#  define REF(p)   do { if (p) (p)->hold++; } while (0)
#  define UNREF(p) do { if (p && (p)->hold) (p)->hold--; } while (0)
#endif
static Nd *alloc_one(void) { return calloc(1, sizeof(Nd)); }
int main(int argc, char **argv) {
    long POOL = argc > 1 ? atol(argv[1]) : 200000;
    long N = argc > 2 ? atol(argv[2]) : 20000000;
    long AE = argc > 3 ? atol(argv[3]) : 1024;
    long DATA = argc > 4 ? atol(argv[4]) : 0;   /* 每次改链前做多少次数据写 */
    for (int c = 0; c < CHAINS; c++) head[c] = tail[c] = NULL;
    for (long i = 0; i < POOL; i++) {
        Nd *n = alloc_one(); n->key = i;
        int c = (int)(i % CHAINS);
        if (head[c]) { REF(head[c]); n->next = head[c]; REF(n); head[c]->prev = n; }
        UNREF(head[c]); head[c] = n; REF(n);
        if (!tail[c]) tail[c] = n;
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < N; i++) {
        int c = (int)(i % CHAINS);
        Nd *n = head[c];
        if (!n || !n->next) continue;
        for (long d = 0; d < DATA; d++) {            /* 数据字段写：不改指向关系 ⇒ 不计数 */
            n->val = n->val * 1103515245 + 12345; sink += n->val;
        }
        Nd *nx = n->next, *tl = tail[c];
        UNREF(nx->prev); nx->prev = NULL;
        UNREF(head[c]); head[c] = nx; REF(nx);
        REF(n); UNREF(tl->next); tl->next = n;
        UNREF(n->prev); n->prev = tl;
        UNREF(n->next); n->next = NULL;
        UNREF(tail[c]); tail[c] = n; REF(n);
        sink += n->key;
        if (AE && (i % AE) == 0) {
            Nd *m = alloc_one(); m->key = -i;
            REF(head[c]); m->next = head[c]; UNREF(head[c]); head[c] = m; REF(m);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("每轮=%.2fns 总耗时=%.1fms (数据写/指针写=%ld)\n", ms*1e6/N, ms, DATA);
    return 0;
}
