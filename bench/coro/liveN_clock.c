/* liveN 的 C 侧支持：单调时钟 + /proc/self/statm 的常驻内存。没有 main —— 入口是 extC 那个。 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdint.h>

int64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t bench_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long size = 0, res = 0;
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &size, &res) != 2) res = -1;
    fclose(f);
    return res < 0 ? -1 : (int64_t)res * 4;      /* 页 = 4 KB ⇒ KB */
}
