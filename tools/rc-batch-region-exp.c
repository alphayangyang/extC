/* tools/rc-batch-region-exp.c —— **手工构造的合成实验**（不是编译器自动生成的形状）。
 *
 * 问题：把"整批死掉的结构"放进它自己的区域、死时整区丢弃（regionFree）——
 *       相比今天"所有对象共用一个区域、靠清扫逐个放掉"，**扫描面与每次代价**差多少？
 *
 * 两个变体做同一批分配（构成照 digraph @250k 的实测：每批 ≈4 874 个 80 B 对象 +
 * ≈10 840 个 32 B 对象；常驻活集 ≈1 614 个对象），只在"怎么回收"上不同：
 *   A（今天）：全部对象共用一个区域；每轮结束后 extc_rc_collect(区域, 活根) ⇒ 标记 + 清扫。
 *   B（假设）：每批对象进**自己的区域**；每轮结束 extc_rc_regionFree(那个区域) ⇒ 整区丢弃。
 * 量：每轮耗时（p50/p90/max/total）、A 的扫过槽数与释放数、B 的 regionFree 耗时与 slab 数、
 * 两变体的峰值 RSS。
 *
 * 用法（先由 tools/rc-batch-region-exp.sh 生成 rt.inc）：
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O2 -I <dir-with-rt.inc> rc-batch-region-exp.c -o exp
 *   taskset -c 6 ./exp <rounds> A|B
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.inc"          /* 发射出来的那份运行期文本（与编译器同源） */

#define NODE_BYTES 80      /* digraph node 的尺寸档 */
#define EDGE_BYTES 32      /* digraph edge 的尺寸档 */
#define NODE_PER_BATCH 4874
#define EDGE_PER_BATCH 10840
#define LIVE_OBJS 1614

static long long ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long a = 0, b = 0;
    if (f) { if (fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; fclose(f); }
    return b * 4;
}
static int cmpll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static void report(const char *tag, long long *v, size_t n) {
    if (!n) return;
    qsort(v, n, sizeof v[0], cmpll);
    long long s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    printf("%s rounds=%zu p50_us=%.1f p90_us=%.1f max_us=%.1f total_us=%.1f\n",
           tag, n, v[n / 2] / 1000.0, v[n * 9 / 10] / 1000.0, v[n - 1] / 1000.0, s / 1000.0);
}

static uint32_t off0[1];
int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 50;
    char mode = argc > 2 ? argv[2][0] : 'A';
    int variantB = mode == 'B', variantC = mode == 'C';   /* C = 只分配、不回收 */
    int variantD = mode == 'D';                            /* D = 只量"新区域 2 块 slab 的建/毁" */
    ExtcRcType td = { 0u, off0 };                  /* 无引用字段：标记不追孩子 */

    ExtcRcRegion *home = extc_rc_regionNew(1);
    void **live = (void **)calloc(LIVE_OBJS, sizeof *live);
    for (int i = 0; i < LIVE_OBJS; i++) live[i] = extc_rc_alloc(home, NODE_BYTES, &td);

    long long *rt = (long long *)calloc(rounds, sizeof *rt);
    size_t slabs_sum = 0, freed_sum = 0, scanned_sum = 0;
    long rss0 = 0;
    for (int r = 0; r < rounds; r++) {
        ExtcRcRegion *batch = (variantB || variantD) ? extc_rc_regionNew(100 + r) : home;
        long long t0 = ns();
        if (variantD) {   /* 两个尺寸类各 1 个对象 ⇒ 两块 slab；然后整区丢弃 */
            (void)extc_rc_alloc(batch, NODE_BYTES, &td);
            (void)extc_rc_alloc(batch, EDGE_BYTES, &td);
            slabs_sum += extc_rc_slabCount(batch);
            extc_rc_regionFree(batch);
            rt[r] = ns() - t0;
            continue;
        }
        for (int i = 0; i < NODE_PER_BATCH; i++) (void)extc_rc_alloc(batch, NODE_BYTES, &td);
        for (int i = 0; i < EDGE_PER_BATCH; i++) (void)extc_rc_alloc(batch, EDGE_BYTES, &td);
        if (variantC) {
            /* 纯分配：不回收（内存随轮数涨，只用来拆出分配成本） */
        } else if (variantB) {
            slabs_sum += extc_rc_slabCount(batch);
            extc_rc_regionFree(batch);            /* 整区丢弃 = O(slab) */
        } else {
            size_t scan = 0;
            for (ExtcRcSlab *s = home->slabs; s; s = s->next) scan += s->nslots;
            scanned_sum += scan;
            size_t freed = extc_rc_collect(home, (void *const *)live, LIVE_OBJS);
            freed_sum += freed;
            slabs_sum += extc_rc_slabCount(home);
        }
        rt[r] = ns() - t0;
        if (r == 0) rss0 = rss_kb();
    }
    report(variantD ? "D(regionNew+2 slabs+regionFree only)" :
           variantC ? "C(alloc only, no reclaim)" :
           (variantB ? "B(per-batch region + regionFree)" : "A(today: shared region + collect)"), rt, rounds);
    printf("variant=%c rounds=%d slabs_allocated_total=%zu freed_total=%zu scanned_slots_total=%zu "
           "final_rss_kb=%ld rss_after_first_round_kb=%ld\n",
           mode, rounds, slabs_sum, freed_sum, scanned_sum, rss_kb(), rss0);
    for (int i = 0; i < LIVE_OBJS; i++) (void)live[i];
    extc_rc_regionFree(home);
    free(live); free(rt);
    return 0;
}
