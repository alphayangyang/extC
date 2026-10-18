/* ttab-scale.c —— 每 slab 小类型表的查找成本（extc_rc_typeIdx）缩放探针。
 *
 * 由 tests/reclaim-perf/gate-reclaim-tables.sh 编译。造一个假 slab，按 slabNew 的形状
 * 初始化小类型表（cap 8、2 号位起、1 号位固定兜底描述符），然后对 T 个不同描述符轮转
 * 调用 extc_rc_typeIdx，打印 T 与 N。判据（每次调用的指令数在 T=8 与 T=256 两档是否恒定）
 * 在脚本里量：描述符少的 slab 走线性扫，超过阈值后走 16 位开放寻址索引。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

#define MAXT 256
static const ExtcRcType g_types[MAXT];

int main(int argc, char **argv) {
    size_t T = argc > 1 ? (size_t)strtoull(argv[1], 0, 10) : 16;
    size_t N = argc > 2 ? (size_t)strtoull(argv[2], 0, 10) : 200000;
    if (T > MAXT) { fprintf(stderr, "T too big\n"); return 1; }

    ExtcRcSlab *s = (ExtcRcSlab *)malloc(sizeof *s);
    if (!s) { fprintf(stderr, "oom slab\n"); return 1; }
    memset(s, 0, sizeof *s);
    s->ttabCap = 8;
    s->ttabN = 2;
    s->ttab = (const ExtcRcType **)calloc(s->ttabCap, sizeof *s->ttab);
    if (!s->ttab) { fprintf(stderr, "oom ttab\n"); return 1; }
    s->ttab[1] = EXTC_RC_CONSERVATIVE;
    s->thash = NULL;
    s->thashCap = 0;

    volatile uint32_t sink = 0;
    for (size_t i = 0; i < N; i++) sink += extc_rc_typeIdx(s, &g_types[i % T]);

    printf("T=%zu N=%zu sink=%u\n", T, N, (unsigned)sink);
    return 0;
}
