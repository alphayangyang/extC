/* tests/gc-p0/noise_floor.c —— 计时噪声地板（G1 判据的对照基准，墙钟 + 线程 CPU 双口径）
 *
 * 纯 CPU、无分配、无系统调用的空转微基准：每次迭代把 inner 次整数加法夹在两个时钟之间，
 * 分别用 CLOCK_MONOTONIC（墙钟，含被抢占）与 CLOCK_THREAD_CPUTIME_ID（线程 CPU，只算本线程
 * 实际执行）量同一个窗口。它量的不是 GC，而是**这台机器在当时的计时噪声地板**：
 * 墙钟口径会吸进调度抖动 / WSL2 宿主抢占，CPU 口径只剩纯代码执行时间。
 *
 * 为什么两个都要：Lead 2026-11-09 用 [top] 归因发现 GC 的慢步 dur≈cpu（pc 50M：B=1
 * dur 192.6 / cpu 194.1 µs；B=8 152.5 / 152.9）⇒ GC 长尾是真实工作量、不是抢占。
 * 所以判"GC 的 CPU 步长"必须和"空转的 CPU 步长"比；"空转的墙钟 max"只说明机器会抖，
 * 不能拿去和 GC 的 CPU 步长比（不同口径）。
 *
 * 用法：
 *   cc -O2 -std=c11 -o noise_floor tests/gc-p0/noise_floor.c
 *   ./noise_floor [n] [inner]          # 默认 200000 200（与 Lead 的对照基准同规模）
 *
 * 输出一行（机器可解析）：
 *   noise n=.. inner=.. wall_max_us=.. wall_mean_us=.. wall_over16=.. wall_over50=..
 *         wall_over50_ratio=.. wall_p999_us=.. cpu_max_us=.. cpu_mean_us=.. cpu_over16=..
 *         cpu_over50=.. cpu_over50_ratio=.. cpu_p999_us=..
 * over50_ratio = over50/n，即"超过 50µs 的窗口占比"。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

static double clk_us(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

typedef struct { double max, sum; long o16, o50, o64, o256, o500, o1000; } stat_t;

static void add(stat_t *s, double d) {
    s->sum += d;
    if (d > s->max) s->max = d;
    if (d > 16) s->o16++;
    if (d > 50) s->o50++;
    if (d > 64) s->o64++;
    if (d > 256) s->o256++;
    if (d > 500) s->o500++;
    if (d > 1000) s->o1000++;
}

static void report(const char *pre, stat_t *s, uint64_t *ns, long n) {
    qsort(ns, (size_t)n, sizeof(uint64_t), cmp_u64);
    /* p99.9：至少有 100 个样本在其之上的分位（n>=1e5 时成立） */
    long idx = (long)((double)n * 0.999);
    if (idx >= n) idx = n - 1;
    double p999 = (double)ns[idx] / 1000.0;
    printf(" %s_max_us=%.3f %s_mean_us=%.4f %s_over16=%ld %s_over50=%ld %s_over64=%ld "
           "%s_over256=%ld %s_over500=%ld %s_over1000=%ld %s_over50_ratio=%.3e %s_p999_us=%.3f",
           pre, s->max, pre, s->sum / (double)n, pre, s->o16, pre, s->o50, pre, s->o64,
           pre, s->o256, pre, s->o500, pre, s->o1000, pre, (double)s->o50 / (double)n, pre, p999);
}


/* ---- 停顿率探针（G1 的**外部环境地板**，Lead R3） ---------------------------------------------
 * 纯计算、无分配、无系统调用的连续循环，用 rdtsc 量**每一次循环窗口**的时长，数出 >10us / >50us
 * 的窗口个数与总时长 => 这台机器当时的「外部停顿速率」（次/秒）。这个速率与代码无关：它是宿主
 * 抢占 vCPU / 调度抖动造成的，落在任何被计时的工作里都会被算成「慢步」。
 * 为什么必须按秒给速率：G1 的 p99.9 判据是「count(>10us)/n」，而外部停顿是按**时间**到达的
 * （rate 次/秒）；一步被外部停顿打中一次就记一条，与这一步本身的预算无关。知道了 rate 与本次
 * run 的墙钟时长 T，就能算出「本 run 里必然有多少条慢步是环境造成的」（用探针的前后两次速率取
 * **较小值**，即最少扣减）。
 * 用法：noise_floor --rate <秒>
 */
static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}
static inline uint64_t rdtsc_now(void) { return (uint64_t)__builtin_ia32_rdtsc(); }
static double tsc_per_us(void) {   /* 用 20ms 的墙钟标定 TSC 频率（TSC 恒定、与频率缩放无关） */
    double t0 = now_us();
    uint64_t c0 = rdtsc_now();
    while (now_us() - t0 < 20000.0) { }
    double t1 = now_us();
    uint64_t c1 = rdtsc_now();
    double dt = t1 - t0;
    return dt > 0 ? (double)(c1 - c0) / dt : 1000.0;
}
static int rate_mode(double secs) {
    double tpu = tsc_per_us();
    if (tpu <= 0) tpu = 1000.0;
    volatile long acc = 0;
    long iters = 0, g10 = 0, g50 = 0, g100 = 0;
    uint64_t sum10 = 0, sum50 = 0, maxd = 0;
    double t0 = now_us();
    uint64_t pv = rdtsc_now();
    for (;;) {
        for (long j = 0; j < 64; j++) acc += j + iters;
        uint64_t q = rdtsc_now();
        uint64_t d = q - pv;
        pv = q;
        iters++;
        double dus = (double)d / tpu;
        if (dus > 10.0) {
            g10++; sum10 += d;
            if (dus > 50.0) { g50++; sum50 += d; }
            if (dus > 100.0) g100++;
            if (d > maxd) maxd = d;
        }
        if (now_us() - t0 >= secs * 1e6) break;
    }
    double el = (now_us() - t0) / 1e6;
    if (el <= 0) el = secs;
    printf("stallrate elapsed_s=%.4f iters=%ld tsc_per_us=%.2f over10=%ld over50=%ld over100=%ld "
           "rate10_per_s=%.1f rate50_per_s=%.1f max_us=%.1f sum10_ms=%.2f sum50_ms=%.2f\n",
           el, iters, tpu, g10, g50, g100, (double)g10 / el, (double)g50 / el,
           (double)maxd / tpu, (double)sum10 / tpu / 1000.0, (double)sum50 / tpu / 1000.0);
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--rate") == 0) return rate_mode(atof(argv[2]));
    long n = (argc > 1) ? atol(argv[1]) : 200000;
    long inner = (argc > 2) ? atol(argv[2]) : 200;
    if (n <= 0) n = 200000;
    if (inner < 0) inner = 0;
    uint64_t *w = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *c = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    if (!w || !c) { fprintf(stderr, "oom\n"); return 2; }

    stat_t sw = {0}, sc = {0};
    volatile long acc = 0;

    for (long i = 0; i < n; i++) {
        double w0 = clk_us(CLOCK_MONOTONIC);
        double c0 = clk_us(CLOCK_THREAD_CPUTIME_ID);
        long a = acc;
        for (long k = 0; k < inner; k++) a += k;
        acc = a;
        double c1 = clk_us(CLOCK_THREAD_CPUTIME_ID);
        double w1 = clk_us(CLOCK_MONOTONIC);
        double dw = w1 - w0, dc = c1 - c0;
        w[i] = (uint64_t)(dw * 1000.0);
        c[i] = (uint64_t)(dc * 1000.0);
        add(&sw, dw);
        add(&sc, dc);
    }

    printf("noise n=%ld inner=%ld", n, inner);
    report("wall", &sw, w, n);
    report("cpu", &sc, c, n);
    printf("\n");
    free(w);
    free(c);
    return 0;
}
