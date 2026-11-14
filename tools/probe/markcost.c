// markcost.c - 用真实回收运行期测「每 pop 的标记成本」与「每 KB 容量的清扫成本」随活集 footprint 的变化。
// 需要先切出 rt.inc：python3 tools/probe/rc-markcost.py slice <generated.c> -o <dir>/rt.inc
// 编译：cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -I<dir> -o /tmp/markcost tools/probe/markcost.c
// 读法见 docs/analysis/CALIBRATION.md 第 11 节。用法：taskset -c 6 /tmp/markcost 16 3 6
// 目的：把 c_t / tau_t 的 cache-miss 项测出来，与微基准的延迟梯对照。
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
typedef struct N48 { void *a, *b, *c, *d, *e, *f; } N48;
#include "rt.inc"
static const uint32_t g_off[6] = { 0, 8, 16, 24, 32, 40 };
static const ExtcRcType g_t = { 6u, g_off };
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void){uint64_t x=rs;x^=x<<13;x^=x>>7;x^=x<<17;rs=x;return x;}
int main(int argc,char **argv){
    if (argc < 2) { fprintf(stderr, "usage: %s live_mb [reps] [pin]\n", argv[0]); return 2; }
    size_t live_mb = (size_t)atol(argv[1]);
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    int pin = argc > 3 ? atoi(argv[3]) : 6;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(pin, &cs); sched_setaffinity(0, sizeof(cs), &cs);
    size_t n = (live_mb << 20) / sizeof(N48);
    if (n < 1024) n = 1024;
    N48 **v = (N48 **)malloc(n * sizeof(N48 *));
    size_t *perm = (size_t *)malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) perm[i] = i;
    for (size_t i = n; i > 1; i--) { size_t j = (size_t)(rnd() % i); size_t t = perm[i-1]; perm[i-1] = perm[j]; perm[j] = t; }
    ExtcRcRegion *r = extc_rc_regionNew(7);
    for (size_t i = 0; i < n; i++) { v[i] = (N48 *)extc_rc_alloc(r, sizeof(N48), &g_t); memset(v[i], 0, sizeof(N48)); }
    /* 链：按 perm 顺序连成一条覆盖全部对象的链（地址随机 => 无顺序预取），再给每个对象一条随机短边。
       根 = 链头，所以标记必须走满全部 n 个对象。 */
    for (size_t i = 0; i < n; i++) {
        v[perm[i]]->a = v[perm[(i + 1) % n]];
        v[perm[i]]->b = v[perm[rnd() % n]];
    }
    void *roots[1]; roots[0] = v[perm[0]];
    size_t marked_tot = 0, pops_tot = 0;
    double best_mark = 1e18, best_sweep = 1e18;
    size_t capBytes = 0, slabs = 0;
    for (int rep = 0; rep < reps; rep++) {
        extc_rc_begin(r, roots, 1);
        double t0 = now();
        size_t mk = 0;
        while (!r->seedsDone || r->msp || r->scanObj) mk += extc_rc_markStep(r, 0xffffffffu);
        double t1 = now();
        if (rep == 0) marked_tot = mk;
        if (t1 - t0 < best_mark) best_mark = t1 - t0;
        extc_rc_finish(r);
        double t2 = now();
        while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
        double t3 = now();
        if (t3 - t2 < best_sweep) best_sweep = t3 - t2;
        capBytes = (size_t)r->capBytes;
        slabs = extc_rc_slabCount(r);
        if (rep == 0) fprintf(stderr, "[rep0] marked=%zu cap=%zu slabs=%zu\n", mk, capBytes, slabs);
    }
    (void)pops_tot;
    printf("%6zu MB n=%8zu marked=%8zu slabs=%6zu cap=%10zu mark_ns/pop=%8.2f sweep_ns/slot=%7.2f sweep_ns/KB=%7.3f\n",
           live_mb, n, marked_tot, slabs, capBytes,
           best_mark * 1e9 / (double)(marked_tot ? marked_tot : 1),
           best_sweep * 1e9 / (double)(capBytes / sizeof(N48)),
           best_sweep * 1e9 / ((double)capBytes / 1024.0));
    free(perm); free(v);
    return 0;
}