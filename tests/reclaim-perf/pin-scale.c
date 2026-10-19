/* pin-scale.c —— 钉住表（宿主证不出来的槽）缩放的探针（由 tests/reclaim-perf/gate-pin-scale.sh 编译）。
 *
 * 只做一件事：在**发射出来的**运行期文本上，连续把 N 个对象钉成根（每个对象一条宿主槽，
 * 槽本身是探针的 malloc 数组、不在 rc slab 里 ⇒ 一定走 extc_rc_storeRefUnknown），打印表长。
 * 判据（每钉一个的指令数在 N 与 4N 两档是否恒定）在脚本里量，不在本文件里下结论。
 *
 * 由来：钉住表最初是「数组 + 线性扫」，每次登记/撤销都扫全表 ⇒ 钉 n 个是 O(n²)
 * （实测 5 万 0.26 s、20 万 5.23 s，4× 规模 20× 时间）。现行实现是开放寻址哈希表
 * （装载 ≤ 1/2、Knuth 回移删除），每钉一个 O(1)。--selftest 往 pinAdd 里注入一段
 * 线性预扫（语义等价、成本 O(表长)）现场证明判据量得到回退。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

static const ExtcRcType g_node = { 0u, NULL };

int main(int argc, char **argv) {
    size_t N = argc > 1 ? (size_t)strtoull(argv[1], 0, 10) : 4000;
    void **host = (void **)malloc(N * sizeof(void *));
    if (!host) { fprintf(stderr, "oom host\n"); return 1; }
    memset(host, 0, N * sizeof(void *));

    ExtcRcRegion *r = extc_rc_regionNew(1);
    extc_rc_regionFloor(r, (int64_t)1 << 40);

    for (size_t i = 0; i < N; i++) {
        void *v = extc_rc_alloc(r, 64, &g_node);
        extc_rc_storeRefUnknown(&host[i], v);
    }

    printf("pins=%zu cap=%zu\n", extc_rc_pinN, extc_rc_pinCap);
    return 0;
}
