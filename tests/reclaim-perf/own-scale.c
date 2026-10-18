/* own-scale.c —— 登记表维护成本（ownAdd/ownDel）的缩放探针。
 *
 * 由 tests/reclaim-perf/gate-reclaim-tables.sh 编译。只做一件事：造 S 个**假 slab**
 * （big=0，只用被登记表碰到的字段），依次 extc_rc_ownAdd，再依次 extc_rc_ownDel，
 * 打印 S 与一个校验和。判据（每对 add+del 的指令数在 S 与 4S 两档是否恒定）在脚本里量。
 *
 * 为什么造假的 slab 而不真的分配：登记表按 slab 指针登记，与 slab 内容无关；造 S 个真
 * slab（每个一块）要的内存与 S 成正比，却只为了量一张哈希表的维护成本。
 * 基址必须按 EXTC_RC_SLAB_SIZE 对齐：探测的起始格是「基址 >> SLAB_BITS 乘常数」，
 * 小块 malloc 出来的地址只差几百字节、移位后几乎同键，量出来的是极端聚簇而不是稳态。
 * 也因此本探针**不量** ownSlab（那条路的成本结构由 gate-reclaim-perf /
 * gate-slab-scale 覆盖）。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

int main(int argc, char **argv) {
    size_t S = argc > 1 ? (size_t)strtoull(argv[1], 0, 10) : 1024;
    size_t base = EXTC_RC_SLAB_SIZE;

    ExtcRcSlab **v = (ExtcRcSlab **)malloc(S * sizeof *v);
    if (!v) { fprintf(stderr, "oom v\n"); return 1; }
    for (size_t i = 0; i < S; i++) {
        void *m = aligned_alloc(base, base);
        if (!m) { fprintf(stderr, "oom slab\n"); return 1; }
        memset(m, 0, sizeof(ExtcRcSlab));
        ExtcRcSlab *s = (ExtcRcSlab *)m;
        s->big = 0;
        s->ss = 16;
        v[i] = s;
    }

    volatile uintptr_t sink = 0;
    for (size_t i = 0; i < S; i++) extc_rc_ownAdd(v[i]);
    for (size_t i = 0; i < S; i++) sink += (uintptr_t)v[i];
    for (size_t i = 0; i < S; i++) extc_rc_ownDel(v[i]);

    printf("S=%zu sink=%zu\n", S, (size_t)sink);
    return 0;
}
