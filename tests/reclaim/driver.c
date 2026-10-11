/* tests/reclaim/driver.c —— 回收运行期文本的断言程序。
 *
 * 被测的是 `src/back/reclaim.c` **发射出来的**那份文本（run.sh 先编译发射器，把文本导出成
 * extc_reclaim_runtime.inc，再和本文件一起编译）。所以这里断言的行为，就是生成文件里真正跑
 * 的那份代码的行为 —— 不是另抄一份实现来对拍。
 *
 * 覆盖：分配/释放、计数归零当场回收、环只能由清扫收回、饱和计数不得提前释放、槽释放时登记位
 * 被清（无陈旧入口）、跨区登记播种与"区域外止步"、按对象**自己的**类型描述符走指针字段
 * （描述符随槽交，同区混放不同布局）、`owns` 守卫（外区/非槽首指针一个字节都不碰）、
 * 槽复用换描述符、保守描述符、bump 整片回收、压力触发与自学习退场（无环负载下清扫次数不随
 * 规模增长）。
 *
 * API 形状（task-4 后）：`extc_rc_alloc(区域, 字节, const ExtcRcType *描述符)`；
 * `ExtcRcType = { nptr, off }`，描述符由分配点随槽交给运行期，区域只认领一个号。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node { struct Node *next; } Node;                /* 16 字节（槽大小 16） */
typedef struct Pair { struct Node *a; struct Node *b; } Pair;   /* 16 字节，两个指针字段 */
typedef struct Wide { uint64_t pad; struct Node *p; } Wide;     /* 16 字节，指针只在**第二个**字 */

#include "extc_reclaim_runtime.inc"

static int g_pass, g_fail;

static void ok(const char *what) { printf("  ok   %s\n", what); g_pass++; }
static void fail(const char *what) { printf("  FAIL %s\n", what); g_fail++; }
static void check(int cond, const char *what) { if (cond) ok(what); else fail(what); }

/* 类型描述符（task-4 后的形态）：`nptr` + 一张独立的 `off` 表。描述符由**分配点随槽**交给
 * 运行期（每个槽自带一份，标记时按对象自己的走），所以同一区域里可以混放不同布局的对象 ——
 * 这里故意准备三种字段数/偏移不同的布局来抓"一张区域级表套所有对象"的漏。 */
static const uint32_t g_offNode[1] = { (uint32_t)offsetof(Node, next) };
static const uint32_t g_offPair[2] = { (uint32_t)offsetof(Pair, a), (uint32_t)offsetof(Pair, b) };
static const uint32_t g_offWide[1] = { (uint32_t)offsetof(Wide, p) };

static const ExtcRcType g_data = { 0u, NULL };      /* 没有引用字段：一个都不追 */
static const ExtcRcType g_node = { 1u, g_offNode };
static const ExtcRcType g_pair = { 2u, g_offPair };
static const ExtcRcType g_wide = { 1u, g_offWide };

static ExtcRcRegion *regionNew(int32_t rid, int64_t floor) {
    ExtcRcRegion *r = extc_rc_regionNew(rid);      /* 类型描述符随槽走 ⇒ 区域只认领一个号 */
    extc_rc_regionFloor(r, floor);
    return r;
}

/* ---------------------------------------------------------------- 分配 / 释放 */
static void t_allocFree(void) {
    ExtcRcRegion *r = regionNew(1, 4096);
    check(extc_rc_bytes(r) == 0 && extc_rc_slabCount(r) == 0, "空区域：零占用、零 slab");

    Node *p = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *q = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(p && q && p != q, "分配：两个不同的槽");
    check(extc_rc_isLive(p) && extc_rc_isLive(q), "分配：占用位置位");
    check(extc_rc_hold(p) == 0 && extc_rc_hold(q) == 0, "分配：新槽还没有持有者");
    check(p->next == NULL && q->next == NULL, "分配：新槽清零（新分配读作 0）");
    check(extc_rc_bytes(r) == 32 && extc_rc_slabCount(r) == 1, "分配：占用字节与 slab 记账");

    extc_rc_initRef((void **)&p->next, q);
    check(p->next == q, "initRef：新槽写入正确（没把空闲链当旧指针读）");
    check(extc_rc_hold(q) == 1, "initRef：目标计数 +1");
    extc_rc_storeRef((void **)&p->next, NULL);
    check(extc_rc_hold(q) == 0 && !extc_rc_isLive(q), "storeRef：归零当场释放");
    check(extc_rc_bytes(r) == 16, "释放：占用字节回落");

    void *root = NULL;
    extc_rc_initRef(&root, p);
    check(extc_rc_hold(p) == 1, "initRef：根槽持有 +1");
    extc_rc_storeRef(&root, NULL);
    check(!extc_rc_isLive(p), "storeRef：最后一个持有者消失即释放");
    check(extc_rc_bytes(r) == 0, "释放：区域回到零占用");
    check(extc_rc_sweeps(r) == 0, "计数归零不走清扫");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 环必须由清扫收回 */
static void t_cycle(void) {
    ExtcRcRegion *r = regionNew(2, 4096);
    Node *a = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *b = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    extc_rc_initRef((void **)&a->next, b);
    extc_rc_initRef((void **)&b->next, a);
    check(extc_rc_hold(a) == 1 && extc_rc_hold(b) == 1, "环：两个对象的计数都不归零");
    check(extc_rc_bytes(r) == 32, "环：计数不会收回它");

    size_t freed = extc_rc_collect(r, NULL, 0);
    check(freed == 2, "环：清扫把两个都收回");
    check(!extc_rc_isLive(a) && !extc_rc_isLive(b), "环：槽已还回空闲链");
    check(extc_rc_bytes(r) == 0 && extc_rc_freedSlots(r) == 2, "环：占用与累计释放记账");
    check(extc_rc_sweeps(r) == 1 && extc_rc_scale(r) == 0, "环：真收到东西 ⇒ 自学习复位");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 饱和计数不得提前释放 */
static void t_saturate(void) {
    ExtcRcRegion *r = regionNew(3, 4096);
    Node *p = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    for (int i = 0; i < 70000; i++) extc_rc_inc(p);
    check(extc_rc_hold(p) == EXTC_RC_HOLD_SAT, "饱和：计数到顶粘住");
    for (int i = 0; i < 1000; i++) extc_rc_dec(p);
    check(extc_rc_hold(p) == EXTC_RC_HOLD_SAT, "饱和：饱和值不再减回去");
    check(extc_rc_isLive(p) && extc_rc_bytes(r) == 16, "饱和：对象绝不被提前释放");

    size_t freed = extc_rc_collect(r, NULL, 0);
    check(freed == 1 && extc_rc_bytes(r) == 0, "饱和：只能由清扫收回");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 释放后登记位被清 */
static void t_staleRegister(void) {
    ExtcRcRegion *r = regionNew(4, 4096);
    Node *b = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    extc_rc_register(b);
    check(extc_rc_isRegistered(b), "登记：目标槽登记位置位");

    void *root = NULL;
    extc_rc_initRef(&root, b);
    extc_rc_storeRef(&root, NULL);
    check(!extc_rc_isLive(b), "登记：计数归零释放槽");
    check(!extc_rc_isRegistered(b), "登记：释放槽时登记位被清（无陈旧入口）");

    Node *b2 = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(b2 == b, "登记：空闲链复用同一个槽");
    check(!extc_rc_isRegistered(b2), "登记：复用出来的新对象没有被登记");
    size_t freed = extc_rc_collect(r, NULL, 0);
    check(freed == 1 && !extc_rc_isLive(b2), "登记：陈旧入口不会把新对象救活");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 跨区登记 + 区域外止步 */
static void t_crossRegion(void) {
    ExtcRcRegion *ra = regionNew(5, 4096);
    ExtcRcRegion *rb = regionNew(6, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Node *b1 = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);
    Node *b2 = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);

    void *rootA = a;
    extc_rc_initRef((void **)&a->a, b1);      /* A 区 -> B 区：跨区边 */
    extc_rc_initRef((void **)&b1->next, b2);
    extc_rc_register(b1);                     /* 跨区入边登记在**目标槽**上 */

    size_t markedA = extc_rc_mark(ra, &rootA, 1);
    size_t freedA = extc_rc_sweep(ra);
    check(markedA == 1 && freedA == 0, "跨区：本区对象标上，区域外对象止步不标");
    check(extc_rc_isLive(b1) && extc_rc_isLive(b2), "跨区：A 的清扫没有碰 B 的对象");

    size_t freedB = extc_rc_collect(rb, NULL, 0);
    check(freedB == 0 && extc_rc_isLive(b1) && extc_rc_isLive(b2),
          "跨区：B 从入边位图播种，链条完整（残留标记会把孩子释放掉）");
    check(extc_rc_hold(b1) == 1 && extc_rc_hold(b2) == 1, "跨区：计数仍由指针赋值维护");
    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
}

/* ---------------------------------------------------------------- 按对象自己的类型走指针字段 */
static void t_types(void) {
    ExtcRcRegion *r = regionNew(7, 4096);
    Pair *p = (Pair *)extc_rc_alloc(r, sizeof(Pair), &g_pair);
    Node *x = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *y = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    extc_rc_initRef((void **)&p->a, x);
    extc_rc_initRef((void **)&p->b, y);       /* y 只由 Pair 的**第二个**字段指着 */
    check(p->b == y && x->next == NULL, "类型：负载摆好");

    Node *z = (Node *)extc_rc_alloc(r, sizeof(Node), &g_data);
    memset(z, 0x5A, sizeof(Node));            /* 数据对象里的垃圾绝不能被当指针走 */
    void *roots[2] = { p, z };
    size_t freed = extc_rc_collect(r, roots, 2);
    check(freed == 0, "类型：描述符字段数正确（漏一个字段就会释放对象）");
    check(extc_rc_isLive(p) && extc_rc_isLive(x) && extc_rc_isLive(y) && extc_rc_isLive(z),
          "类型：Pair 的两个字段与 Node 的字段都标上，数据对象不追");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 区域外/未知指针：一个字节都不碰 */
/* 板/池/栈/C 函数返回的指针不是本运行期的槽。inc/dec/register/标记前必须先过 owns；
 * 没有这条守卫，`slabOf` 的按位与会找到一段无关内存（板的 4 GiB 保留区甚至没 commit）⇒ SEGV。
 * 这里用"外区内存 + 槽内但不在槽首的指针"两种形状钉住它：全部操作都必须是空操作，
 * 且外区内存一个字节都不能被改。 */
static void t_ownsGuard(void) {
    ExtcRcRegion *r = regionNew(12, 4096);
    Node *inside = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);

    unsigned char *outside = (unsigned char *)malloc(64);
    memset(outside, 0xA5, 64);                 /* 填满可识别的花样 */

    check(!extc_rc_isLive(outside) && extc_rc_hold(outside) == 0,
          "守卫：外区内存不是活槽（isLive/hold 都判否）");
    check(!extc_rc_isRegistered(outside), "守卫：外区内存登记位恒 0");
    extc_rc_inc(outside);
    extc_rc_dec(outside);
    extc_rc_register(outside);
    check(!extc_rc_isRegistered(outside), "守卫：inc/dec/register 对外区指针全是空操作");

    int untouched = 1;
    for (int i = 0; i < 64; i++) if (outside[i] != 0xA5) untouched = 0;
    check(untouched, "守卫：外区指针那 64 个字节一个都没被改");

    /* 槽内但不在槽首：字段地址（+1）与"下一槽首"（该槽还空闲）。 */
    unsigned char *mid  = (unsigned char *)inside + 1;
    unsigned char *next = (unsigned char *)inside + sizeof(Node);
    check(!extc_rc_isLive(mid) && !extc_rc_isLive(next),
          "守卫：槽内非槽首 / 空闲槽都不是活对象");
    extc_rc_inc(mid); extc_rc_dec(mid); extc_rc_register(mid);
    extc_rc_inc(next); extc_rc_dec(next); extc_rc_register(next);
    check(!extc_rc_isRegistered(mid) && !extc_rc_isRegistered(next),
          "守卫：非槽首不登记");
    check(inside->next == NULL && extc_rc_isLive(inside) && extc_rc_hold(inside) == 0,
          "守卫：误传的指针没有波及真正的活槽");

    /* 外区指针当根：标记只碰本区（跨区"止步"的另一半，见 t_crossRegion）。 */
    void *roots[3] = { outside, mid, next };
    size_t freed = extc_rc_collect(r, roots, 3);
    check(freed == 1, "守卫：外区根不保护本区对象（此处唯一的活槽被正常收回）");
    extc_rc_regionFree(r);
    free(outside);
}

/* ---------------------------------------------------------------- 描述符随槽：按对象自己的走 */
/* Wide 的指针在**第二个**字，Node 在第一个字。若运行时用一张区域级表（或把某个对象的描述符
 * 套到别的对象上），要么漏标链尾、要么把数据字当指针读 ⇒ 这里会释放掉活对象。 */
static void t_perSlotType(void) {
    ExtcRcRegion *r = regionNew(13, 4096);
    Wide *w = (Wide *)extc_rc_alloc(r, sizeof(Wide), &g_wide);
    Node *x = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *y = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    w->pad = 0xDEADBEEFCAFEF00Dull;            /* 数据字：绝不能被当指针走 */
    extc_rc_initRef((void **)&w->p, x);
    extc_rc_initRef((void **)&x->next, y);

    void *roots[1] = { w };
    size_t freed = extc_rc_collect(r, roots, 1);
    check(freed == 0, "描述符随槽：Wide(off=8)→Node(off=0)→Node 的链完整标上");
    check(extc_rc_isLive(w) && extc_rc_isLive(x) && extc_rc_isLive(y),
          "描述符随槽：同一区域里三种布局同时活着");
    check(extc_rc_hold(x) == 1 && extc_rc_hold(y) == 1, "描述符随槽：计数仍由指针赋值维护");
    extc_rc_regionFree(r);
}

/* 同一个槽被复用成**另一种布局**：描述符必须随新分配换掉，不能留着上一任的。 */
static void t_slotReuseType(void) {
    ExtcRcRegion *r = regionNew(14, 4096);
    Wide *w = (Wide *)extc_rc_alloc(r, sizeof(Wide), &g_wide);
    void *root = NULL;
    extc_rc_initRef(&root, w);
    extc_rc_storeRef(&root, NULL);             /* 归零当场释放，槽进空闲链 */
    check(!extc_rc_isLive(w), "复用：Wide 槽已释放");

    Node *n = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(n == (Node *)w, "复用：空闲链复用同一个槽");
    memset((char *)n + 8, 0x5A, 8);            /* 第二个字是垃圾：Node 没有这个字段，绝不能追 */
    Node *z = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    extc_rc_initRef((void **)&n->next, z);

    void *roots[1] = { n };
    size_t freed = extc_rc_collect(r, roots, 1);
    check(freed == 0 && extc_rc_isLive(n) && extc_rc_isLive(z),
          "复用：新分配的描述符生效（残留的 Wide 描述符会走到垃圾字上）");
    extc_rc_regionFree(r);
}

/* 兜底描述符：`alloc<T>(n)` 的元素偏移随运行期计数变，静态描述符表达不了 ⇒ 保守标记扫每个字。 */
static void t_conservative(void) {
    ExtcRcRegion *r = regionNew(15, 4096);
    Node *blob = (Node *)extc_rc_alloc(r, sizeof(Node), EXTC_RC_CONSERVATIVE);
    Node *t = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    *(void **)((char *)blob + 8) = t;          /* 指针在运行期才知道的位置 */
    void *roots[1] = { blob };
    size_t freed = extc_rc_collect(r, roots, 1);
    check(freed == 0 && extc_rc_isLive(t),
          "兜底描述符：运行期才知道位置的指针也被保守标上");
    extc_rc_regionFree(r);
}


static void t_bump(void) {
    ExtcRcRegion *r = extc_rc_regionNew(8);   /* 解析区：不扫描，不需要类型描述符 */
    extc_rc_regionFloor(r, 4096);
    char *p1 = (char *)extc_rc_bump(r, 16);
    char *p2 = (char *)extc_rc_bump(r, 16);
    check(p1 && p2 && p2 - p1 == 16, "bump：载荷连续排布");
    size_t slabs = extc_rc_slabCount(r);
    extc_rc_bumpReset(r);
    char *p3 = (char *)extc_rc_bump(r, 16);
    check(p3 == p1, "bump reset：指针拨回，内存整片复用");
    check(extc_rc_slabCount(r) == slabs, "bump reset：不新增 slab");
    check(extc_rc_bytes(r) == 0, "bump：不计入占用字节（整片随 reset 归还）");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 压力触发 */
static void t_trigger(void) {
    ExtcRcRegion *r = regionNew(9, 4096);
    check(!extc_rc_shouldSweep(r), "触发：空区域不触发");

    void *keep = extc_rc_alloc(r, sizeof(Pair), &g_pair);
    for (int i = 0; i < 100; i++) (void)extc_rc_alloc(r, sizeof(Pair), &g_pair);
    check(extc_rc_bytes(r) < 4096 && !extc_rc_shouldSweep(r), "触发：低于压力线不触发");

    while (extc_rc_bytes(r) <= 4096) (void)extc_rc_alloc(r, sizeof(Pair), &g_pair);
    check(extc_rc_shouldSweep(r), "触发：超过压力线触发");

    void *roots[1] = { keep };
    int got = extc_rc_maybeCollect(r, roots, 1);
    check(got >= 0 && extc_rc_sweeps(r) == 1, "触发：maybeCollect 收了一次");
    check(extc_rc_isLive(keep), "触发：根上的对象活着（零计数也活）");
    check(extc_rc_scale(r) == 0, "自学习：真收到东西就复位");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 无环有界负载：不进清扫 */
static size_t boundedLiveSweeps(long n, int64_t *bytesOut) {
    ExtcRcRegion *r = regionNew(10, 4096);
    void *slots[64];
    memset(slots, 0, sizeof slots);
    for (long i = 0; i < n; i++) {
        void *nd = extc_rc_alloc(r, sizeof(Node), &g_node);
        extc_rc_storeRef(&slots[i & 63], nd);      /* 覆盖即释放旧的 */
        (void)extc_rc_maybeCollect(r, slots, 64);
    }
    size_t sw = extc_rc_sweeps(r);
    *bytesOut = extc_rc_bytes(r);
    extc_rc_regionFree(r);
    return sw;
}

static void t_acyclic(void) {
    int64_t b1 = 0, b2 = 0;
    size_t s1 = boundedLiveSweeps(10000, &b1);
    size_t s2 = boundedLiveSweeps(200000, &b2);
    check(b1 <= 64 * 16 && b2 == b1, "无环有界负载：占用不随规模增长");
    check(s1 == 0 && s2 == 0, "无环负载：规模涨 20 倍，清扫次数都是 0");
}

/* ---------------------------------------------------------------- 自学习退场 */
static size_t liveGrowthSweeps(long n, int64_t *scaleOut) {
    ExtcRcRegion *r = regionNew(11, 4096);
    void **roots = (void **)malloc((size_t)n * sizeof(void *));
    if (!roots) { fail("自学习：roots 分配失败"); extc_rc_regionFree(r); return 0; }
    for (long i = 0; i < n; i++) {
        roots[i] = extc_rc_alloc(r, sizeof(Node), &g_node);   /* 全部从根可达 ⇒ 一个都收不回 */
        (void)extc_rc_maybeCollect(r, roots, (size_t)i + 1);
    }
    size_t sw = extc_rc_sweeps(r);
    int64_t live = extc_rc_live(r), bytes = extc_rc_bytes(r);
    check(live > 0 && live <= bytes, "自学习：live 记下上一轮清扫后的存活字节");
    *scaleOut = extc_rc_scale(r);
    free(roots);
    extc_rc_regionFree(r);
    return sw;
}

static void t_learning(void) {
    int64_t sc1 = 0, sc2 = 0;
    size_t s1 = liveGrowthSweeps(10000, &sc1);
    size_t s2 = liveGrowthSweeps(1000000, &sc2);
    check(s1 > 0 && sc1 > 1, "自学习：全存活增长会空扫，倍数翻倍");
    check(s2 <= s1 + 2, "自学习：规模涨 100 倍，清扫次数只多一两次");
    check(s2 <= 6, "自学习：清扫次数是有界的小常数");
    check(sc2 > sc1, "自学习：倍数继续涨 ⇒ 阈值退场");
}

int main(void) {
    printf("== reclaim 运行期断言 ==\n");
    t_allocFree();
    t_cycle();
    t_saturate();
    t_staleRegister();
    t_crossRegion();
    t_types();
    t_ownsGuard();
    t_perSlotType();
    t_slotReuseType();
    t_conservative();
    t_bump();
    t_trigger();
    t_acyclic();
    t_learning();
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
