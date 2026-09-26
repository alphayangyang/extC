/* liveN 的驱动程序：两批（冷/热），进程内量 RSS 差 ⇒ 每任务内存。 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define main __extc_generated_main
#include "liveN_gen.c"
#undef main

static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}
/* /proc/self/statm 第二列 = 常驻页数 */
static int64_t rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long size = 0, res = 0;
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &size, &res) != 2) res = -1;
    fclose(f);
    return res < 0 ? -1 : res * 4 / 1024   /* 页 = 4 KB */;
}

int main(int argc, char **argv) {
    int64_t n = argc > 1 ? atoll(argv[1]) : 10000;
    extc_coro *b1 = malloc((size_t)n * sizeof(extc_coro));
    extc_coro *b2 = malloc((size_t)n * sizeof(extc_coro));
    if (!b1 || !b2) { puts("malloc failed"); return 1; }

    int64_t r0 = rss_kb();
    int64_t t0 = now_ns();
    int64_t s1 = run_batch(n, (slice_coroutine_i64){ .data = b1, .len = n });
    int64_t cold = now_ns() - t0;
    int64_t r1 = rss_kb();

    t0 = now_ns();
    int64_t s2 = run_batch(n, (slice_coroutine_i64){ .data = b2, .len = n });
    int64_t warm = now_ns() - t0;
    int64_t r2 = rss_kb();

    printf("extC  n=%lld  live=%lld  cold=%.2f ns/个  warm=%.2f ns/个  每任务冷=%lld B 每任务热=%lld B"
           "  (sink=%lld)\n",
           (long long)n, (long long)live_count(),
           (double)cold / n, (double)warm / n,
           n ? (long long)((r1 - r0) * 1024 / n) : 0,
           n ? (long long)((r2 - r1) * 1024 / n) : 0,
           (long long)(s1 + s2));
    return 0;
}
