#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <stdint.h>

/* Chunk-granularity prototype. Three numbers a language change must move:
 *   growth wall clock, peak RSS, and the access tax of a two-level index.
 *   ./chunkproto flat            pre-sized contiguous array (baseline)
 *   ./chunkproto copy  <factor>  grow by new+memcpy (1.1 / 1.5 / 2.0)
 *   ./chunkproto chunk <elems>   grow by appending 512- or 4096-element chunks
 * N = 8M i64 (64 MiB). Access columns:
 *   seq      s += v[i], the straightforward lowering of an indexed loop
 *   seqMaj   chunk-major scan, what a consumer that knows the layout can do
 *   randMix  2M uniformly scattered reads (splitmix64 indices, DRAM-latency bound) */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static long rssPeakKb(void) {
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; long kb = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (strncmp(line, "VmHWM:", 6) == 0) { kb = atol(line + 6); break; }
    fclose(f); return kb;
}
static int64_t *xmap(size_t bytes) {
    void *q = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) { perror("mmap"); exit(1); }
    return (int64_t *)q;
}
static inline uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
#define N   (8u << 20)
#define NR  2000000
static volatile int64_t sink;
typedef struct { int64_t *p; size_t cap, n; double factor; } Flat;
typedef struct { int64_t **chunk; size_t nchunk, tcap, n, ce; int sh; size_t mask; } Big;
static void flatPush(Flat *v, int64_t x) {
    if (v->n == v->cap) {
        size_t ncap = (size_t)((double)v->cap * v->factor);
        if (ncap <= v->cap) ncap = v->cap + 1;
        int64_t *np = xmap(ncap * sizeof(int64_t));
        if (v->n) memcpy(np, v->p, v->n * sizeof(int64_t));
        if (v->p) munmap(v->p, v->cap * sizeof(int64_t));
        v->p = np; v->cap = ncap;
    }
    v->p[v->n++] = x;
}
static inline int64_t flatGet(const Flat *v, size_t i) { return v->p[i]; }
static void bigPush(Big *v, int64_t x) {
    if ((v->n >> v->sh) == v->nchunk) {
        if (v->nchunk == v->tcap) {
            size_t nt = v->tcap ? v->tcap * 2 : 1024;
            int64_t **t = (int64_t **)xmap(nt * sizeof(int64_t *));
            if (v->nchunk) memcpy(t, v->chunk, v->nchunk * sizeof(int64_t *));
            if (v->chunk) munmap(v->chunk, v->tcap * sizeof(int64_t *));
            v->chunk = t; v->tcap = nt;
        }
        v->chunk[v->nchunk++] = xmap(v->ce * sizeof(int64_t));
    }
    v->chunk[v->n >> v->sh][v->n & v->mask] = x;
    v->n++;
}
static inline int64_t bigGet(const Big *v, size_t i) { return v->chunk[i >> v->sh][i & v->mask]; }
static void report(const char *tag, double grow, long rss, double seq, double seqM, double rnd, const char *extra) {
    printf("%-9s | grow %7.2f ms | rss %7ld kB | seq %7.3f ms | seqMaj %7.3f ms | rand %7.3f ms | %s\n",
           tag, grow, rss, seq, seqM, rnd, extra);
}
int main(int argc, char **argv) {
    double t0, t1, t2, t3, t4; long rss; int64_t s; uint64_t k;
    if (argc < 2) { fprintf(stderr, "usage: %s flat|copy <f>|chunk <elems>\n", argv[0]); return 2; }
    if (strcmp(argv[1], "flat") == 0) {
        int64_t *arr = xmap((size_t)N * sizeof(int64_t));
        t0 = now(); for (size_t i = 0; i < N; i++) arr[i] = (int64_t)i * 3 + 1;
        t1 = now(); rss = rssPeakKb(); s = 0;
        for (size_t i = 0; i < N; i++) s += arr[i];
        t2 = now();
        for (size_t i = 0; i < N; i++) s += arr[i];
        t3 = now();
        for (k = 0; k < NR; k++) s += arr[mix(k) % N];
        t4 = now(); sink = s;
        report("flat", (t1-t0)/1e6, rss, (t2-t1)/1e6, (t3-t2)/1e6, (t4-t3)/1e6, "");
        return 0;
    }
    if (strcmp(argv[1], "copy") == 0) {
        Flat v = {0, 0, 0, argc > 2 ? atof(argv[2]) : 1.1}; char ex[64];
        t0 = now(); for (size_t i = 0; i < N; i++) flatPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = rssPeakKb(); s = 0;
        for (size_t i = 0; i < N; i++) s += flatGet(&v, i);
        t2 = now();
        for (size_t i = 0; i < N; i++) s += flatGet(&v, i);
        t3 = now();
        for (k = 0; k < NR; k++) s += flatGet(&v, mix(k) % N);
        t4 = now(); sink = s;
        snprintf(ex, sizeof ex, "cap=%zu", v.cap);
        report("copy", (t1-t0)/1e6, rss, (t2-t1)/1e6, (t3-t2)/1e6, (t4-t3)/1e6, ex);
        return 0;
    }
    if (strcmp(argv[1], "chunk") == 0) {
        Big v = {0, 0, 0, 0, argc > 2 ? (size_t)atol(argv[2]) : 512, 0, 0}; char ex[64];
        if (v.ce != 512 && v.ce != 4096) { fprintf(stderr, "chunk size must be 512 or 4096\n"); return 2; }
        v.sh = (v.ce == 512) ? 9 : 12; v.mask = v.ce - 1;
        t0 = now(); for (size_t i = 0; i < N; i++) bigPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = rssPeakKb(); s = 0;
        for (size_t i = 0; i < N; i++) s += bigGet(&v, i);
        t2 = now();
        for (size_t c = 0; c < v.nchunk; c++) {
            size_t lim = (c + 1 == v.nchunk) ? (v.n - (c << v.sh)) : v.ce;
            for (size_t j = 0; j < lim; j++) s += v.chunk[c][j];
        }
        t3 = now();
        for (k = 0; k < NR; k++) s += bigGet(&v, mix(k) % N);
        t4 = now(); sink = s;
        snprintf(ex, sizeof ex, "chunks=%zu", v.nchunk);
        report("chunk", (t1-t0)/1e6, rss, (t2-t1)/1e6, (t3-t2)/1e6, (t4-t3)/1e6, ex);
        return 0;
    }
    return 2;
}
