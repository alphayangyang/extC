/* tests/bufrt/driver.c —— buf 基质的两个 regime 共用一本账：增长预留（range）与密排小载荷（slot）。
 *
 * 判据来自那次决定性的测量：20 万个 32 字节载荷如果各拿一个 2 MiB 量子，驻留内存 442 MB（池底是 18 MB）。
 * 这里钉住的就是「同尺寸类的槽要密排、槽还清了量子要退回 buddy、槽与保留区间互不干扰」。
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_buf_runtime.inc"

static int fails;
static int oks;

static void ok(const char *name) { printf("  ok   %s\n", name); oks++; }
static void bad(const char *name) { printf("  FAIL %s\n", name); fails++; }
static void check(int cond, const char *name) { if (cond) ok(name); else bad(name); }

#define N_SLOT 200000
static uint8_t *g_slot[N_SLOT];

/* 密排：同尺寸类的 20 万个槽必须挤在少数几个量子页里，而不是一个载荷一个量子。 */
static void test_dense(void) {
    long long before = extc_buf_live();
    int i, nulls = 0, broken = 0;
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (i = 0; i < N_SLOT; i++) {
        g_slot[i] = extc_buf_slotReserve(32);
        if (!g_slot[i]) { nulls++; continue; }
        if ((uintptr_t)g_slot[i] < lo) lo = (uintptr_t)g_slot[i];
        if ((uintptr_t)g_slot[i] > hi) hi = (uintptr_t)g_slot[i];
        g_slot[i][0] = (uint8_t)(i & 0xFF);
        g_slot[i][31] = (uint8_t)((i >> 8) & 0xFF);
    }
    check(nulls == 0, "20 万个 32 字节载荷全部拿到槽");
    check(extc_buf_live() - before <= (long long)16 * 1024 * 1024,
          "20 万个槽的地址空间不超过 16 MiB（密排，不是一个载荷一个量子）");
    check(hi - lo < (uintptr_t)16 * 1024 * 1024, "20 万个槽落在不超过 16 MiB 的地址区间里");
    check(extc_buf_span(g_slot[0]) == 32, "槽的 span 就是它的尺寸类（32 字节）");
    for (i = 0; i < N_SLOT; i++) {
        if (g_slot[i][0] != (uint8_t)(i & 0xFF) || g_slot[i][31] != (uint8_t)((i >> 8) & 0xFF)) broken++;
    }
    check(broken == 0, "槽互不重叠：写完再读，每个槽还是自己的内容");
    {
        int rel = 0;
        for (i = 0; i < N_SLOT; i++) if (extc_buf_slotRelease(g_slot[i]) != 0) rel++;
        check(rel == 0, "20 万个槽全部归还成功（没有「还不回去」的路径）");
        check(extc_buf_live() == before, "还清之后地址空间账回到原值（量子退回 buddy）");
    }
}

/* 量子生命周期：同尺寸类的槽相邻、还一个槽时量子还在、还清了量子立刻就退。 */
static void test_retire(void) {
    long long before = extc_buf_live();
    uint8_t *a = extc_buf_slotReserve(32);
    uint8_t *b = extc_buf_slotReserve(32);
    check(a && b && a != b, "两个 32 字节槽拿到不同地址");
    check(a && b && (size_t)(b - a) == 32, "同尺寸类的相邻槽步长等于尺寸类（32 字节）");
    check(extc_buf_live() - before == (long long)2 * 1024 * 1024, "一个量子记进账（2 MiB 地址空间）");
    if (a) extc_buf_slotRelease(a);
    check(extc_buf_live() - before == (long long)2 * 1024 * 1024, "还一个槽时量子仍在（账不变）");
    if (b) {
        b[0] = 0x5A;
        check(b[0] == 0x5A, "同量子里的另一个槽没被归还路径牵连");
        extc_buf_slotRelease(b);
    }
    check(extc_buf_live() == before, "最后一个槽还清后量子退回 buddy（账回到原值）");
}

/* discard 对槽必须是空操作：槽与邻居共页，madvise 会把活数据一起抹掉。 */
static void test_discard(void) {
    uint8_t *s[3];
    int i, j, intact = 1;
    long long before = extc_buf_live();
    for (i = 0; i < 3; i++) {
        s[i] = extc_buf_slotReserve(16);
        for (j = 0; j < 16; j++) s[i][j] = 0xAB;
    }
    check(extc_buf_discard(s[1], 16) == 0, "对槽调 discard 返回成功（它是空操作）");
    for (i = 0; i < 3; i++) for (j = 0; j < 16; j++) if (s[i][j] != 0xAB) intact = 0;
    check(intact, "discard 之后三个槽的字节都还在（没有把邻居的页丢掉）");
    for (i = 0; i < 3; i++) extc_buf_slotRelease(s[i]);
    check(extc_buf_live() == before, "三个槽还清后账回到原值");
}

/* 两个 regime 共存：槽与增长预留各自记账，互不影响。 */
static void test_mixed(void) {
    long long before = extc_buf_live();
    uint8_t *s1 = extc_buf_slotReserve(64);
    uint8_t *big = extc_buf_reserve((uint64_t)4 * 1024 * 1024);
    uint8_t *s2 = extc_buf_slotReserve(64);
    check(s1 && big && s2, "一个进程里同时拿到槽与 4 MiB 预留");
    check(extc_buf_span(big) == (int64_t)4 * 1024 * 1024, "预留区间的 span 是 4 MiB");
    check(extc_buf_live() - before == (long long)6 * 1024 * 1024, "账 = 2 MiB（量子）+ 4 MiB（预留）");
    big[0] = 0x11; big[4194303] = 0x22;
    check(big[0] == 0x11 && big[4194303] == 0x22, "预留区间的首尾字节可写可读");
    extc_buf_slotRelease(s1);
    extc_buf_slotRelease(s2);
    extc_buf_release(big, (uint64_t)4 * 1024 * 1024);
    check(extc_buf_live() == before, "槽与预留都还清后账回到原值");
}

/* 拒绝的边界：超出一个类的请求返回空（调用方走预留），非槽地址不许当槽还。 */
static void test_rejects(void) {
    uint8_t *big = extc_buf_reserve((uint64_t)2 * 1024 * 1024);
    check(extc_buf_slotReserve(131072) == (uint8_t *)0, "超过 64 KiB 的请求不是槽（返回空，交给预留）");
    check(extc_buf_slotReserve(65536) != (uint8_t *)0, "64 KiB 正好是最后那个尺寸类");
    if (big) {
        check(extc_buf_slotRelease(big) != 0, "预留区间的起点不许当槽归还");
        extc_buf_release(big, (uint64_t)2 * 1024 * 1024);
    }
}

/* 周转：反复要/还同一个尺寸类，账与地址都不漂（这是容器周转的那条路）。 */
static void test_churn(void) {
    long long before = extc_buf_live();
    int i, nulls = 0, rel = 0;
    for (i = 0; i < 100000; i++) {
        uint8_t *p = extc_buf_slotReserve(40);   /* 40 -> 类 64 */
        if (!p) { nulls++; continue; }
        p[0] = (uint8_t)(i & 0xFF);
        if (p[0] != (uint8_t)(i & 0xFF)) rel++;
        if (extc_buf_slotRelease(p) != 0) rel++;
    }
    check(nulls == 0, "10 万次周转每次都拿到槽");
    check(rel == 0, "10 万次周转的内容与归还都正确");
    check(extc_buf_live() == before, "周转结束后账回到原值");
}

int main(void) {
    printf("== buf 基质：两个 regime，一本账 ==\n");
    test_dense();
    test_retire();
    test_discard();
    test_mixed();
    test_rejects();
    test_churn();
    printf("通过 %d，失败 %d\n", oks, fails);
    return fails ? 1 : 0;
}
