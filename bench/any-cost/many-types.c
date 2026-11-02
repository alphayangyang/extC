/* Does the descriptor comparison get slow when there are many types?
 *
 * Models the three ways a language could branch on a dynamic type:
 *   chain   — linear compare against N known descriptors (what a `match` with
 *             N struct alternatives would compile to naively)
 *   binary  — binary search over descriptors sorted by address
 *   tag     — descriptor carries a small integer (or the site caches it), one
 *             table lookup, independent of N
 * plus the effect that actually bites: heterogenous DATA (cache misses), not
 * the comparison.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXT 512
#define NMAX 4194304

typedef struct {
    int kind;
    int tag;
    const char *name;
    size_t size;
} D;

typedef struct { const void *p; const D *d; } Any;

static D ds[MAXT];
static Any items[NMAX];
static const D *sorted[MAXT];
static int64_t payloads[NMAX];
static int tags_tab[MAXT];

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmpd(const void *a, const void *b) {
    const D *x = *(const D *const *)a, *y = *(const D *const *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int main(void) {
    for (int i = 0; i < MAXT; i++) {
        ds[i].kind = 0;
        ds[i].tag = i;
        ds[i].name = "t";
        ds[i].size = 8;
        sorted[i] = &ds[i];
        tags_tab[i] = i * 3;
        payloads[i] = i;
    }
    qsort(sorted, MAXT, sizeof sorted[0], cmpd);
    for (int i = 0; i < NMAX; i++) {
        items[i].p = &payloads[i];
        items[i].d = &ds[0];
    }

    volatile int64_t sink = 0;
    const int LOOPS = 20000000;

    printf("%-8s %10s %10s %10s\n", "types", "chain", "binary", "tag");
    for (int nt = 2; nt <= 256; nt <<= 1) {
        for (int i = 0; i < NMAX; i++) items[i].d = &ds[i % nt];
        double t0, t1;
        int64_t acc = 0;

        t0 = now_ns();
        for (int k = 0; k < LOOPS; k++) {
            Any w = items[k & (NMAX - 1)];
            for (int i = 0; i < nt; i++) {
                if (w.d == &ds[i]) { acc += i; break; }
            }
        }
        t1 = now_ns();
        double chain = (t1 - t0) / LOOPS;

        t0 = now_ns();
        for (int k = 0; k < LOOPS; k++) {
            Any w = items[k & (NMAX - 1)];
            int lo = 0, hi = nt - 1, hit = 0;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                if (sorted[mid] == w.d) { hit = mid; break; }
                if (sorted[mid] < w.d) lo = mid + 1; else hi = mid - 1;
            }
            acc += hit;
        }
        t1 = now_ns();
        double binary = (t1 - t0) / LOOPS;

        t0 = now_ns();
        for (int k = 0; k < LOOPS; k++) {
            Any w = items[k & (NMAX - 1)];
            acc += tags_tab[w.d->tag];
        }
        t1 = now_ns();
        double tag = (t1 - t0) / LOOPS;

        printf("%-8d %9.2fns %9.2fns %9.2fns\n", nt, chain, binary, tag);
        sink += acc;
    }

    /* heterogeneous data: 4M items, how many distinct types before the payload
     * pointer chase itself starts to cost? */
    printf("\n%-8s %10s %12s\n", "types", "ns/item", "d-ptr-cache");
    for (int nt = 1; nt <= 256; nt <<= 1) {
        for (int i = 0; i < NMAX; i++) {
            items[i].d = &ds[i % nt];
            ((int64_t *)items[i].p)[0] = i & 1023;
        }
        /* warm */
        int64_t acc = 0;
        for (int i = 0; i < NMAX; i++) acc += *(const int64_t *)items[i].p;
        double t0 = now_ns();
        for (int r = 0; r < 5; r++) {
            for (int i = 0; i < NMAX; i++) {
                const int64_t *p = (const int64_t *)items[i].p;
                acc += *p + (int64_t)items[i].d->tag;
            }
        }
        double t1 = now_ns();
        printf("%-8d %9.2fns\n", nt, (t1 - t0) / (5.0 * NMAX));
        sink += acc;
    }
    printf("sink=%lld\n", (long long)sink);
    return 0;
}
