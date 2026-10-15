/* tests/reclaim-perf/slotindex.c —— 槽号索引的**域内穷举**判据。
 *
 * 为什么需要它：`extc_rc_slotSize` 按 8 字节取整（不是 2 的幂），所以"由指针找槽号"不再是一次
 * 移位，而是 `extc_rc_slotOfOff` 的**乘倒数右移**（`rcp = 2^42/ss + 1`）。它只要在某个 (ss, off)
 * 上算错一格，`ownSlab` 的槽首校验就会把**合法对象**判成"不是本运行期的槽" —— 标记漏走它 ⇒
 * 清扫把它当垃圾放掉 ⇒ 悬垂。所以这条不是性能判据，是健全性判据。
 *
 * 口径：`#include` **发射出来的那份**运行期文本（与 tests/reclaim/run.sh 同一条发射路径），
 * 直接调用里面的 `extc_rc_rcpOf` / `extc_rc_slotOf`，**不另抄一份实现**（抄一份就会漂移）。
 * 穷举：槽步长 16..1 MiB 的每个 8 的倍数 × 该 slab 里全部合法槽号，外加槽内偏移与"指针落在
 * slab 头里"两档必须被拒。
 *
 * 调用者：tests/reclaim-perf/gate.sh（`gate-reclaim-perf`）。退出码 0 = 全中。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

int main(void) {
    long long cases = 0, bad = 0, rejected = 0;
    /* 只需要地址算术，不真的用 1 MiB；给足一个 slab 的跨度。 */
    char *buf = (char *)calloc(1, EXTC_RC_SLAB_SIZE + 4096);
    if (!buf) {
        printf("slotindex: calloc 失败\n");
        return 2;
    }
    for (size_t ss = 16; ss <= (size_t)EXTC_RC_SLAB_SIZE; ss += 8) {
        ExtcRcSlab s;
        memset(&s, 0, sizeof s);
        s.ss = ss;
        s.rcp = extc_rc_rcpOf(ss);
        s.data = buf;
        s.nslots = (uint32_t)((EXTC_RC_SLAB_SIZE - EXTC_RC_SLAB_HDR) / ss);
        if (s.nslots == 0) s.nslots = 1;          /* 超大块那一档：载荷在别处，只有 0 号槽 */
        for (uint32_t k = 0; k < s.nslots; k++) {
            size_t off = (size_t)k * ss;
            size_t got = extc_rc_slotOf(&s, buf + off);
            cases++;
            if (got != k) {
                if (bad < 5) printf("  不符 ss=%zu k=%u off=%zu got=%zu\n", ss, k, off, got);
                bad++;
            }
            /* 槽首校验（`ownSlab` 用的那一条：k*ss == off）在槽内偏移上必须为假。 */
            size_t off2 = off + 1;
            size_t k2 = extc_rc_slotOfOff(&s, off2);
            if (k2 * ss == off2) {
                if (bad < 5) printf("  槽内偏移被当成槽首 ss=%zu off=%zu\n", ss, off2);
                bad++;
            } else {
                rejected++;
            }
        }
    }
    /* 指针落在 slab 头里（载荷起点之前）：off 是"负"的 size_t，乘倒数会回绕 —— 判据只有一条：
     * 它绝不能刚好满足 k*ss == off（那会让 owns/标记认下一段无关内存）。 */
    {
        ExtcRcSlab s;
        memset(&s, 0, sizeof s);
        s.ss = 80;
        s.rcp = extc_rc_rcpOf(80);
        s.data = buf + 64;
        s.nslots = 1000;
        for (size_t back = 1; back <= 63; back++) {
            size_t off = (size_t)0 - back;                 /* 头里的指针 */
            size_t k = extc_rc_slotOfOff(&s, off);
            if (k * s.ss == off) {
                printf("  slab 头里的指针被当成槽首 back=%zu\n", back);
                bad++;
            } else {
                rejected++;
            }
        }
    }
    free(buf);
    printf("槽号索引：域内穷举 %lld 组，不符 %lld 处；槽内/头内偏移被拒 %lld 处\n",
           cases, bad, rejected);
    if (bad) {
        printf("slotindex: FAIL\n");
        return 1;
    }
    printf("slotindex: ok\n");
    return 0;
}
