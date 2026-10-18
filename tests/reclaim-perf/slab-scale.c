/* slab-scale.c —— 分配路径缩放的探针（由 tests/reclaim-perf/gate-slab-scale.sh 编译）。
 *
 * 只做一件事：在**发射出来的**运行期文本上连续分配 N 个同尺寸槽，打印存活 slab 数。
 * 判据（每分配指令数在 N 与 4N 两档是否恒定）在脚本里量，不在本文件里下结论。
 *
 * 脚本用 -DEXTC_RC_SLAB_BITS=14 把 slab 压到 16 KiB：槽 512 B ⇒ 每块约 31 槽，
 * 存活 slab 数随 N 线性涨。旧实现每次「热缓存失手」都从 r->slabs 线性扫到有空位的块，
 * 每分配的成本随存活 slab 数线性涨（这一点由 --selftest 注入旧扫描形状现场证明）。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

static const ExtcRcType g_data = { 0u, NULL };

int main(int argc, char **argv) {
    size_t N = argc > 1 ? (size_t)strtoull(argv[1], 0, 10) : 50000;
    size_t slot = argc > 2 ? (size_t)strtoull(argv[2], 0, 10) : 512;

    void **keep = (void **)malloc(N * sizeof(void *));
    if (!keep) { fprintf(stderr, "oom keep\n"); return 1; }

    ExtcRcRegion *r = extc_rc_regionNew(1);
    extc_rc_regionFloor(r, (int64_t)1 << 40);

    for (size_t i = 0; i < N; i++) keep[i] = extc_rc_alloc(r, slot, &g_data);

    printf("slabs=%zu\n", extc_rc_slabCount(r));
    return 0;
}
