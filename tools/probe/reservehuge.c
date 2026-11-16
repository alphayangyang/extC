#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Does a reserved growable array grow faster with 2 MiB pages?
 *   plain  RW NORESERVE reservation (4 KiB pages), grow = bump
 *   huge   same, but 2 MiB-aligned + MADV_HUGEPAGE
 * Reports grow wall clock, peak RSS, and how much of the range is AnonHugePages.
 * usage: reservehuge plain|huge <melems>   (8M = 64 MiB, 32M = 256 MiB)
 */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static long statusKb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; long kb = -1; size_t kl = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (strncmp(line, key, kl) == 0) { kb = atol(line + kl); break; }
    fclose(f); return kb;
}
static void rangeKb(const char *base, size_t bytes, long *rss, long *huge) {
    FILE *f = fopen("/proc/self/smaps", "r"); char line[512]; unsigned long lo, hi;
    int in = 0; *rss = 0; *huge = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) in = ((char *)lo <= base && base < (char *)hi);
        else if (in && strncmp(line, "Rss:", 4) == 0) *rss += atol(line + 4);
        else if (in && strncmp(line, "AnonHugePages:", 14) == 0) *huge += atol(line + 14);
    }
    fclose(f);
}
static inline uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
#define RESERVE (1ull << 30)
#define NR      2000000
static volatile int64_t sink;
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s plain|huge <melems>\n", argv[0]); return 2; }
    int huge = strcmp(argv[1], "huge") == 0;
    size_t n = (size_t)atol(argv[2]) << 20;
    size_t align = huge ? (2u << 20) : 4096;
    char *raw = (char *)mmap(NULL, RESERVE + align, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); return 1; }
    char *base = (char *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    if (huge && madvise(base, RESERVE, MADV_HUGEPAGE)) { perror("madvise"); return 1; }
    int64_t *p = (int64_t *)base; size_t cap = 1024, i;
    double t0 = now();
    for (i = 0; i < n; i++) {
        if (i == cap) { cap = (size_t)((double)cap * 1.1); if (cap <= i) cap = i + 1; }
        p[i] = (int64_t)i * 3 + 1;
    }
    double t1 = now();
    long peak = statusKb("VmHWM:"), rss = 0, hugeKb = 0;
    rangeKb(base, (size_t)n * 8, &rss, &hugeKb);
    int64_t s = 0; double bestS = 1e30, bestR = 1e30;
    for (size_t k = 0; k < n; k++) s += p[k];
    for (int rep = 0; rep < 5; rep++) {
        double a = now(); for (size_t k = 0; k < n; k++) s += p[k]; double b = now();
        if ((b - a) / 1e6 < bestS) bestS = (b - a) / 1e6;
        double c = now(); for (uint64_t k = 0; k < NR; k++) s += p[mix(k) % n]; double d = now();
        if ((d - c) / 1e6 < bestR) bestR = (d - c) / 1e6;
        sink = s;
    }
    printf("arm=%s melems=%zu grow_ms=%.2f peak_kb=%ld range_rss_kb=%ld anon_huge_kb=%ld seq_ms=%.3f rand_ms=%.2f\n",
           argv[1], n >> 20, (t1 - t0) / 1e6, peak, rss, hugeKb, bestS, bestR);
    return 0;
}
