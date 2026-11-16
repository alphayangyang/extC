#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Reserve-and-commit prototype vs copy-on-grow, for the growable-array question.
 * Workload: push 8M i64 (64 MiB) with 1.1x capacity growth.
 *   copy    mmap + memcpy per generation            (what the heap does today)
 *   arena   bump-carve + memcpy per generation      (what the extC frame arena does)
 *   plate   PROT_NONE reserve + mprotect commit     (plate as it exists today)
 *   lazy    RW NORESERVE reserve, grow = bump       (zero syscalls after reserve)
 * Then seq/rand access on the final 64 MiB, and a many-small-arrays test.
 * Build: cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -o /tmp/rp/reserveproto tools/probe/reserveproto.c
 */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static long statusKb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; long kb = -1; size_t kl = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (strncmp(line, key, kl) == 0) { kb = atol(line + kl); break; }
    fclose(f); return kb;
}
static long vmaCount(void) {
    FILE *f = fopen("/proc/self/maps", "r"); char line[512]; long n = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f); return n;
}
static long n_mmap, n_mprotect, n_munmap, n_madvise;
static void *xreserve(size_t bytes, int prot) {
    void *p = mmap(NULL, bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    n_mmap++; return p;
}
static inline uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

#define N        (8u << 20)
#define NR       2000000
#define RESERVE  (1ull << 30)
#define COMMIT   (1u << 20)
#define ARENA    (8ull << 30)
static volatile int64_t sink;

typedef struct { int64_t *p; size_t cap, n; double f; } Cv;
static void cvPush(Cv *v, int64_t x) {
    if (v->n == v->cap) {
        size_t ncap = (size_t)((double)v->cap * v->f); if (ncap <= v->cap) ncap = v->cap + 1;
        int64_t *np = (int64_t *)xreserve(ncap * 8, PROT_READ | PROT_WRITE);
        if (v->n) memcpy(np, v->p, v->n * 8);
        if (v->p) { munmap(v->p, v->cap * 8); n_munmap++; }
        v->p = np; v->cap = ncap;
    }
    v->p[v->n++] = x;
}

static char *arenaBase; static size_t arenaUsed, arenaCap = ARENA;
static void *arenaCarve(size_t bytes, size_t align) {
    size_t a = (arenaUsed + align - 1) / align * align;
    if (a + bytes > arenaCap) { fprintf(stderr, "arena full\n"); exit(1); }
    arenaUsed = a + bytes; return arenaBase + a;
}
typedef struct { int64_t *p; size_t cap, n; double f; } Av;
static void avPush(Av *v, int64_t x) {
    if (v->n == v->cap) {
        size_t ncap = (size_t)((double)v->cap * v->f); if (ncap <= v->cap) ncap = v->cap + 1;
        int64_t *np = (int64_t *)arenaCarve(ncap * 8, 16);
        if (v->n) memcpy(np, v->p, v->n * 8);
        v->p = np; v->cap = ncap;
    }
    v->p[v->n++] = x;
}

static double commitNs; static long commitN;
typedef struct { char *base; size_t committed, cap, n; } Pv;
static void pvNeed(Pv *v, size_t bytes) {
    if (bytes <= v->committed) return;
    size_t nc = (bytes + COMMIT - 1) / COMMIT * COMMIT;
    if (nc > RESERVE) nc = RESERVE;
    double t = now();
    if (mprotect(v->base + v->committed, nc - v->committed, PROT_READ | PROT_WRITE)) { perror("mprotect"); exit(1); }
    commitNs += now() - t; commitN++; n_mprotect++;
    v->committed = nc;
}
static void pvPush(Pv *v, int64_t x) {
    if (v->n == v->cap) {
        size_t ncap = (size_t)((double)v->cap * 1.1); if (ncap <= v->cap) ncap = v->cap + 1;
        pvNeed(v, ncap * 8); v->cap = ncap;
    }
    if ((v->n + 1) * 8 > v->committed) pvNeed(v, (v->n + 1) * 8);
    ((int64_t *)v->base)[v->n++] = x;
}

typedef struct { int64_t *p; size_t cap, n; double f; } Lv;
static void lvPush(Lv *v, int64_t x) {
    if (v->n == v->cap) { size_t ncap = (size_t)((double)v->cap * v->f); if (ncap <= v->cap) ncap = v->cap + 1; v->cap = ncap; }
    v->p[v->n++] = x;
}

static void accessTests(int64_t *p, size_t n, double *seq, double *rnd) {
    int64_t s = 0; double bestS = 1e30, bestR = 1e30;
    for (size_t i = 0; i < n; i++) s += p[i];
    for (int rep = 0; rep < 5; rep++) {
        double t1 = now();
        for (size_t i = 0; i < n; i++) s += p[i];
        double t2 = now();
        if ((t2 - t1) / 1e6 < bestS) bestS = (t2 - t1) / 1e6;
        double t3 = now();
        for (uint64_t k = 0; k < NR; k++) s += p[mix(k) % n];
        double t4 = now();
        if ((t4 - t3) / 1e6 < bestR) bestR = (t4 - t3) / 1e6;
        sink = s;
    }
    *seq = bestS; *rnd = bestR;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s copy|arena|plate|lazy|small <nar> <mode>|trim\n", argv[0]); return 2; }
    const char *arm = argv[1];
    double t0, t1, seq = 0, rnd = 0; long rss, vma1 = 0;

    if (strcmp(arm, "copy") == 0) {
        Cv v = {0, 0, 0, argc > 2 ? atof(argv[2]) : 1.1}; t0 = now();
        for (size_t i = 0; i < N; i++) cvPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = statusKb("VmHWM:"); accessTests(v.p, v.n, &seq, &rnd); vma1 = vmaCount();
        printf("arm=copy grow_ms=%.2f peak_kb=%ld seq_ms=%.3f rand_ms=%.3f cap=%zu mmap=%ld munmap=%ld vma=%ld\n",
               (t1 - t0) / 1e6, rss, seq, rnd, v.cap, n_mmap, n_munmap, vma1);
        return 0;
    }
    if (strcmp(arm, "arena") == 0) {
        arenaBase = (char *)xreserve(ARENA, PROT_READ | PROT_WRITE);
        Av v = {0, 0, 0, 1.1}; t0 = now();
        for (size_t i = 0; i < N; i++) avPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = statusKb("VmHWM:"); accessTests(v.p, v.n, &seq, &rnd); vma1 = vmaCount();
        printf("arm=arena grow_ms=%.2f peak_kb=%ld seq_ms=%.3f rand_ms=%.3f cap=%zu arena_used_kb=%zu mmap=%ld vma=%ld\n",
               (t1 - t0) / 1e6, rss, seq, rnd, v.cap, arenaUsed >> 10, n_mmap, vma1);
        return 0;
    }
    if (strcmp(arm, "plate") == 0) {
        Pv v = {0, 0, 0, 0}; v.base = (char *)xreserve(RESERVE, PROT_NONE);
        t0 = now();
        for (size_t i = 0; i < N; i++) pvPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = statusKb("VmHWM:"); accessTests((int64_t *)v.base, v.n, &seq, &rnd); vma1 = vmaCount();
        printf("arm=plate grow_ms=%.2f peak_kb=%ld seq_ms=%.3f rand_ms=%.3f cap=%zu commits=%ld commit_us=%.3f mprotect_ns=%.1f mmap=%ld vma=%ld\n",
               (t1 - t0) / 1e6, rss, seq, rnd, v.cap, commitN,
               commitN ? commitNs / commitN / 1e3 : 0.0, commitN ? commitNs / commitN : 0.0, n_mmap, vma1);
        return 0;
    }
    if (strcmp(arm, "lazy") == 0) {
        Lv v = {0, 0, 0, argc > 2 ? atof(argv[2]) : 1.1}; v.p = (int64_t *)xreserve(RESERVE, PROT_READ | PROT_WRITE);
        t0 = now();
        for (size_t i = 0; i < N; i++) lvPush(&v, (int64_t)i * 3 + 1);
        t1 = now(); rss = statusKb("VmHWM:");
        int64_t *p0 = v.p; accessTests(v.p, v.n, &seq, &rnd); vma1 = vmaCount();
        printf("arm=lazy grow_ms=%.2f peak_kb=%ld seq_ms=%.3f rand_ms=%.3f cap=%zu stable=%d mmap=%ld vma=%ld\n",
               (t1 - t0) / 1e6, rss, seq, rnd, v.cap, (int)(p0 == v.p), n_mmap, vma1);
        return 0;
    }
    if (strcmp(arm, "small") == 0) {
        size_t nar = argc > 2 ? (size_t)atol(argv[2]) : 100000;
        size_t mode = argc > 3 ? (size_t)atol(argv[3]) : 0;
        if (mode == 1) arenaBase = (char *)xreserve(ARENA, PROT_READ | PROT_WRITE);
        double t0 = now(); size_t total = 0;
        for (size_t a = 0; a < nar; a++) {
            if (mode == 2) {
                Lv v; v.p = (int64_t *)xreserve(4096 * 8, PROT_READ | PROT_WRITE); v.cap = 4096; v.n = 0;
                for (size_t i = 0; i < 512; i++) v.p[v.n++] = (int64_t)(a * 512 + i);
                total += v.n;
            } else if (mode == 1) {
                Av v = {0, 0, 0, 2.0};
                for (size_t i = 0; i < 512; i++) avPush(&v, (int64_t)(a * 512 + i));
                total += v.n;
            } else {
                Cv v = {0, 0, 0, 2.0};
                for (size_t i = 0; i < 512; i++) cvPush(&v, (int64_t)(a * 512 + i));
                total += v.n;
            }
        }
        double t1 = now(); rss = statusKb("VmHWM:"); vma1 = vmaCount();
        printf("arm=small%zu arrays=%zu grow_ms=%.1f peak_kb=%ld elems=%zu mmap=%ld munmap=%ld vma=%ld\n",
               mode, nar, (t1 - t0) / 1e6, rss, total, n_mmap, n_munmap, vma1);
        return 0;
    }
    if (strcmp(arm, "trim") == 0) {
        Lv v = {0, 0, 0, 1.1}; v.p = (int64_t *)xreserve(RESERVE, PROT_READ | PROT_WRITE);
        for (size_t i = 0; i < N; i++) lvPush(&v, (int64_t)i * 3 + 1);
        long rssA = statusKb("VmRSS:");
        size_t bytes = (v.n * 8 + COMMIT - 1) / COMMIT * COMMIT;
        double t = now();
        if (madvise(v.p, bytes, MADV_DONTNEED)) { perror("madvise"); return 1; }
        double dt = now() - t; n_madvise++;
        long rssB = statusKb("VmRSS:");
        printf("arm=trim bytes_mb=%zu madvise_us=%.1f rss_before_kb=%ld rss_after_kb=%ld madvise=%ld\n",
               bytes >> 20, dt / 1e3, rssA, rssB, n_madvise);
        return 0;
    }
    return 2;
}
