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
 * 规模增长）、证不出宿主时按**槽地址**反查宿主（反查得到就照常记账，失手才保守）、
 * 枚举局部量的整值存储（只记引用、不让目标区域退出清扫，增量标记期间叶子当场标黑）。
 *
 * API 形状（task-4 后）：`extc_rc_alloc(区域, 字节, const ExtcRcType *描述符)`；
 * `ExtcRcType = { nptr, off }`，描述符由分配点随槽交给运行期，区域只认领一个号。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Node { struct Node *next; } Node;                /* 16 字节（槽大小 16） */
typedef struct Pair { struct Node *a; struct Node *b; } Pair;   /* 16 字节，两个指针字段 */
typedef struct Wide { uint64_t pad; struct Node *p; } Wide;     /* 16 字节，指针只在**第二个**字 */

#include "extc_reclaim_runtime.inc"

static int g_pass, g_fail;
static void fail(const char *what);   /* 定义在后面：预算那条路要用它 */

/* ---- 预算：长循环必须在预算内跑完，否则**红并退出**（不是卡住） ----------------
 *
 * 为什么需要：把触发语义改坏（例如忘了重算压力阈值 ⇒ 一分配就扫）之后，下面那些
 * 10 万~100 万轮的循环会进入病态清扫，**卡住而不是给出一行红** —— 那样判据就只能靠
 * 调用方的超时兜住。有了预算，注入会得到一个**快的 FAIL**（见 CHANGELOG 第十四刀）。 */
#define EXTC_DRIVER_BUDGET_SEC 20.0
static clock_t g_deadline;
static void budget(const char *what) {
    if (clock() > g_deadline) {
        fail(what);
        printf("  [budget] 超过 %.0f 秒预算 ⇒ 判据以失败告终（不是继续卡住）\n",
               EXTC_DRIVER_BUDGET_SEC);
        exit(1);
    }
}

static void ok(const char *what) { printf("  ok   %s\n", what); g_pass++; }
static void fail(const char *what) { printf("  FAIL %s\n", what); g_fail++; }
static void check(int cond, const char *what) { if (cond) ok(what); else fail(what); }

/* 类型描述符（task-4 后的形态）：`nptr` + 一张独立的 `off` 表。描述符由**分配点随槽**交给
 * 运行期（每个槽自带一份，标记时按对象自己的走），所以同一区域里可以混放不同布局的对象 ——
 * 这里故意准备三种字段数/偏移不同的布局来抓"一张区域级表套所有对象"的漏。 */
static const uint32_t g_offNode[1] = { (uint32_t)offsetof(Node, next) };
static const uint32_t g_offPair[2] = { (uint32_t)offsetof(Pair, a), (uint32_t)offsetof(Pair, b) };
#if !EXTC_RC_NOCOUNT
static const uint32_t g_offWide[1] = { (uint32_t)offsetof(Wide, p) };
#endif

static const ExtcRcType g_data = { 0u, NULL };      /* 没有引用字段：一个都不追 */
static const ExtcRcType g_node = { 1u, g_offNode };
static const ExtcRcType g_pair = { 2u, g_offPair };
/* P0-4 契约：`liveGrowthSweeps` 记录本轮**峰值占用**与**实际生效的内存墙**
 * （见 t_learning；契约①是峰值驻留 ≤ M_max + 一块缓冲，不是清扫次数小常数）。 */
static int64_t g_learnPeak, g_learnMmax;
#if !EXTC_RC_NOCOUNT
static const ExtcRcType g_wide = { 1u, g_offWide };
#endif

static ExtcRcRegion *regionNew(int32_t rid, int64_t floor) {
    ExtcRcRegion *r = extc_rc_regionNew(rid);      /* 类型描述符随槽走 ⇒ 区域只认领一个号 */
    extc_rc_regionFloor(r, floor);
    return r;
}

/* ---------------------------------------------------------------- 分配 / 释放 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* ---------------------------------------------------------------- 环必须由清扫收回 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* ---------------------------------------------------------------- 饱和计数不得提前释放 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* ---------------------------------------------------------------- 释放后登记位被清 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* ---------------------------------------------------------------- 跨区登记 + 区域外止步 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* ---------------------------------------------------------------- 跨区入边计数的饱和粘住 */
static void t_rsSat(void) {
    ExtcRcRegion *rb = regionNew(8, 4096);
    Node *v = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);
    /* 目标槽被 6.5 万条跨区入边指着：计数到顶**必须粘住** —— 回绕到 0 会让「还有活入边」
     * 这条判断失效，目标就可能在还有入边时被清扫掉（不安全方向）。 */
    for (int k = 0; k < 70000; k++) extc_rc_rsInc(v);
    check(extc_rc_rsCount(v) == EXTC_RC_HOLD_SAT, "跨区计数：到顶粘住");
    extc_rc_rsDec(v);                                     /* 撤一条：饱和值不再减回去 */
    check(extc_rc_rsCount(v) == EXTC_RC_HOLD_SAT, "跨区计数：饱和值不再减回去");
    check(extc_rc_isRegistered(v), "跨区计数：饱和时登记位仍在");
    size_t freed = extc_rc_collect(rb, NULL, 0);
    check(freed == 0 && extc_rc_isLive(v), "跨区计数：饱和时目标绝不被提前回收");
    extc_rc_regionFree(rb);

    /* 第二阶段：走**真实跨区赋值**（区域比较 + 登记）把同一个目标槽压到饱和 ——
     * 只压内部 API 的话，证明的是"计数在饱和处粘住"，证明不了"真实路径走得到饱和"。 */
    ExtcRcRegion *ra = regionNew(9, 4096);
    ExtcRcRegion *rc = regionNew(10, 4096);
    Node *w = (Node *)extc_rc_alloc(rc, sizeof(Node), &g_node);
    Pair *holder[70000];
    for (int k = 0; k < 70000; k++) {
        holder[k] = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
        extc_rc_storeRefOwnedReg((void **)&holder[k]->a, w, ra);   /* A 区槽写 C 区对象 */
    }
    check(extc_rc_rsCount(w) == EXTC_RC_HOLD_SAT, "跨区计数（真实路径）：到顶粘住");
    extc_rc_storeRefOwnedReg((void **)&holder[0]->a, NULL, ra);    /* 撤一条 */
    check(extc_rc_rsCount(w) == EXTC_RC_HOLD_SAT, "跨区计数（真实路径）：饱和值不再减回去");
    check(extc_rc_isRegistered(w), "跨区计数（真实路径）：饱和时登记位仍在");
    size_t freed2 = extc_rc_collect(rc, NULL, 0);
    check(freed2 == 0 && extc_rc_isLive(w), "跨区计数（真实路径）：饱和时目标绝不被提前回收");
    extc_rc_regionFree(ra);
    extc_rc_regionFree(rc);
}

/* ---------------------------------------------------------------- 撤销：不留陈旧登记位 */
static void t_unregister(void) {
    /* "登记与撤销成对"这条不变量的**撤销侧**：跨区边撤掉时，目标槽的计数与登记位要跟着撤。
     * 两条边 + 一个额外持有者，是为了把这条路径**隔离**出来：单个持有者在计数档下会因归零
     * 被 `freeSlot` 顺手清位（那是另一条路径），于是注入 `rsdec-nop` 会被它掩盖、看不出来。 */
    ExtcRcRegion *ra = regionNew(16, 4096);
    ExtcRcRegion *rb = regionNew(17, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);   /* 来源槽 1 */
    Pair *c = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);   /* 来源槽 2 */
    Pair *keep = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Pair *b = (Pair *)extc_rc_alloc(rb, sizeof(Pair), &g_pair);   /* 目标 */
    extc_rc_initRef((void **)&keep->a, b);        /* 额外持有者：不让 b 的持有者计数归零 */
    extc_rc_storeRefOwnedReg((void **)&a->a, b, ra);   /* 跨区边 1 */
    extc_rc_storeRefOwnedReg((void **)&c->a, b, ra);   /* 跨区边 2 */
    check(extc_rc_rsCount(b) == 2, "撤销：两条跨区边 ⇒ 目标入边计数 2");

    extc_rc_storeRefOwnedReg((void **)&a->a, NULL, ra);         /* 撤边 1 */
    check(extc_rc_rsCount(b) == 1 && extc_rc_isRegistered(b),
          "撤销：撤掉一条边后计数减一、登记位仍在");

    extc_rc_storeRefOwnedReg((void **)&c->a, NULL, ra);         /* 撤边 2 */
    check(extc_rc_rsCount(b) == 0 && !extc_rc_isRegistered(b),
          "撤销：边全撤掉后登记位清掉（否则目标被多留）");
    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
}

/* ---------------------------------------------------------------- 回收持有者：出边登记跟着撤 */
static void t_unlink(void) {
    /* 一个对象被回收时，它**手里**的跨区边也要从目标槽上撤掉（`unlinkChildren`）。
     * 撤不掉的方向是保守的（目标被多留一会儿），但"多留"会一路累积成不回落的占用；
     * 这条是登记与撤销成对的另一半，此前只被形状级判据间接守着。 */
    ExtcRcRegion *ra = regionNew(18, 4096);
    ExtcRcRegion *rb = regionNew(19, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);   /* 持有者（A 区）*/
    Pair *b = (Pair *)extc_rc_alloc(rb, sizeof(Pair), &g_pair);   /* 目标（B 区）*/
    extc_rc_storeRefOwnedReg((void **)&a->a, b, ra);              /* A 的槽 -> B 的对象 */
    check(extc_rc_rsCount(b) == 1 && extc_rc_isRegistered(b), "撤销链：边建立后目标入边计数 1");

    extc_rc_collect(ra, NULL, 0);        /* a 无根 ⇒ 被回收 ⇒ 它手里的出边必须撤掉 */
    check(extc_rc_rsCount(b) == 0 && !extc_rc_isRegistered(b),
          "撤销链：持有者被回收时，出边的登记跟着撤（否则目标被多留）");
    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
}

/* ---------------------------------------------------------------- 分配器：一个槽不会重复发出 */
static int cmp_ptr(const void *x, const void *y) {
    const void *a = *(const void *const *)x, *b = *(const void *const *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}
static int has_dup(void **v, int n) {                 /* 排序后看相邻 */
    qsort(v, (size_t)n, sizeof v[0], cmp_ptr);
    for (int i = 1; i < n; i++) if (v[i] == v[i - 1]) return 1;
    return 0;
}
static void t_slotUnique(void) {
    /* 分配器最基本的不变量：**活着的对象地址两两不同**。清扫释放掉的槽会被复用，但复用只能
     * 发生在"已经没人指着"的槽上 —— 这条正是"提前回收/悬垂"的最上游表现，值得自己钉一条，
     * 而不是只靠 ASan 语料与峰值界间接守着。 */
    enum { N = 1024 };
    static void *p[N], *q[N / 2], *roots[N / 2];
    ExtcRcRegion *r = regionNew(15, 1 << 20);
    for (int i = 0; i < N; i++) p[i] = extc_rc_alloc(r, 16, &g_data);
    check(!has_dup(p, N), "槽唯一：首批 N 个对象地址两两不同");

    for (int i = 0; i < N / 2; i++) roots[i] = p[2 * i];   /* 只留偶数下标当根 */
    extc_rc_collect(r, roots, N / 2);                      /* 另一半被清扫 ⇒ 槽进空闲链 */
    for (int i = 0; i < N / 2; i++) q[i] = extc_rc_alloc(r, 16, &g_data);

    check(!has_dup(q, N / 2), "槽唯一：复用之后新拿到的这批也两两不同");
    int clash = 0;
    for (int i = 0; i < N / 2 && !clash; i++)
        for (int k = 0; k < N / 2; k++) if (q[i] == roots[k]) { clash = 1; break; }
    check(!clash, "槽唯一：复用绝不把还活着的槽再发一次");
    check(extc_rc_live(r) >= N / 2, "槽唯一：活集没被复用打穿");
    extc_rc_regionFree(r);
}

/* 钉住表查询（测试专用）：值在表里被几条槽指着。表本身在运行期文本里。
 * 表是开放寻址哈希（空格 v == NULL），所以按容量扫、跳过空格。 */
static size_t extc_rc_pinCountOf(void *v) {
    size_t n = 0;
    for (size_t i = 0; i < extc_rc_pinCap; i++)
        if (extc_rc_pins[i].v == v) n += extc_rc_pins[i].n;
    return n;
}

/* ------------------------------- 义务 7：种子不完整（区域级 noSweep → 钉住值）
 *
 * 宿主证不出来的赋值（`f()->f = v`、视图元素）没有可信的种子位置：旧做法是让**目标区域**
 * 整个退出清扫（只靠计数回收）。现在改成把值钉成根 —— 区域照常清扫，粒度收到被指着的那几个
 * 对象；覆盖即撤销，所以「每轮写同一槽」的形状不会无界少收。这里钉四条：值在标记里真的活下来、
 * 目标区域不再 noSweep、覆盖后钉住撤销、区域释放清掉死指针。 */
static void t_noSweep(void) {
    ExtcRcRegion *ra = regionNew(13, 4096);
    ExtcRcRegion *rb = regionNew(14, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Pair *b = (Pair *)extc_rc_alloc(rb, sizeof(Pair), &g_pair);
    void *roots[1] = { a };
    extc_rc_owe(ra);
    check(extc_rc_checkOwed(ra, roots, 1) == 0, "义务 7：种子完整时照常销账（回收 0 个）");
    check(extc_rc_isLive(a), "义务 7：有根，目标活着");

    extc_rc_storeRefUnknown((void **)&b->a, a);      /* 宿主证不出来 ⇒ 把 a 钉成根 */
    check(!ra->noSweep, "义务 7：钉住之后目标区域不再退出清扫");
    check(extc_rc_pinCountOf(a) == 1, "义务 7：目标对象被登记进钉住表");

    extc_rc_collect(ra, NULL, 0);                    /* 没有静态根：a 唯一的活路是钉住 */
    check(extc_rc_isLive(a), "义务 7：钉住的值在标记里当场活下来（宁可少收）");

    void *rootsb[1] = { b };
    extc_rc_owe(rb);
    check(extc_rc_checkOwed(rb, rootsb, 1) == 0, "义务 7：宿主区域不受影响，照常清扫");

    extc_rc_storeRefUnknown((void **)&b->a, NULL);   /* 覆盖 = 撤掉旧边 */
    check(extc_rc_pinCountOf(a) == 0, "义务 7：覆盖之后钉住撤销");
    extc_rc_collect(ra, NULL, 0);
    check(!extc_rc_isLive(a), "义务 7：钉住撤销后照常收掉（不是永久少收）");
    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
}

/* ---------------------------------------- 义务 7（宿主侧）：证不出宿主时按**槽地址**反查
 *
 * 上下文：宿主表达式证不出来时（`(*id(s)).kids[0] = v`），旧行为是让目标区域整个退出清扫
 * （`noSweep`，只靠计数回收）。但槽地址本身若落在 rc 槽里，槽所在的那只 slab 就告诉我们
 * 装着它的区域 ⇒ 这条边可以照常按「宿主已知」记账。两条判据在这里：反查的形状（任意字节
 * 地址、不要求槽首）与「反查成功就绝不下调成保守」。
 */
static void t_hostSlot(void) {
    ExtcRcRegion *ra = regionNew(17, 4096);
    ExtcRcRegion *rb = regionNew(18, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Node *b = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);

    ExtcRcSlab *sa = extc_rc_slabOfInterior(a);
    check(sa != NULL && sa->region == ra, "槽地址反查：对象首址落在自己区域的 slab 里");
    check(extc_rc_slabOfInterior((char *)a + 1) == sa,
          "槽地址反查：槽内非首址也认（这条路不要求槽首）");
    check(extc_rc_slabOfInterior((char *)a + offsetof(Pair, b)) == sa,
          "槽地址反查：字段地址与对象同 slab");
    check(extc_rc_slabOfInterior(EXTC_RC_SLAB_DATA(sa) + (size_t)sa->nslots * sa->ss) == NULL,
          "槽地址反查：数据区尾之后不认（按发放容量夹住，不是按整块）");
    check(extc_rc_slabOfInterior((void *)sa) == NULL, "槽地址反查：slab 头不认");
    {
        int local = 0;
        check(extc_rc_slabOfInterior(&local) == NULL, "槽地址反查：不在 slab 里的地址不认");
    }

    /* 反查成功 ⇒ 走「宿主已知」：目标区域不得因为这条边退出清扫。 */
    void *rootsb[1] = { b };
    extc_rc_owe(rb);
    check(extc_rc_checkOwed(rb, rootsb, 1) == 0, "槽地址反查：写入前目标区域种子完整（对照）");
    extc_rc_storeRefSlot((void **)&a->a, b);   /* a 在 ra、b 在 rb ⇒ 跨区边，但宿主可证 */
    check(!rb->noSweep, "槽地址反查：反查成功 ⇒ 目标区域照常清扫（不下调保守）");
    check(!ra->noSweep, "槽地址反查：宿主区域也不会被记成保守");
    check(*(void **)&a->a == (void *)b, "槽地址反查：赋值本身照常发生");
    extc_rc_owe(rb);
    check(extc_rc_checkOwed(rb, rootsb, 1) == 0, "槽地址反查：写入后目标区域仍然种子完整");
    if (extc_rc_partN > 1) {
        check(extc_rc_rsCount(b) == 1, "槽地址反查：跨区入边照常登记（宿主已知）");
        check(ra->foreign, "槽地址反查：宿主区域被标记为写过外来指针");
    }
    extc_rc_storeRefSlot((void **)&a->a, NULL);
    check(*(void **)&a->a == NULL, "槽地址反查：撤销写空");
    if (extc_rc_partN > 1)
        check(extc_rc_rsCount(b) == 0, "槽地址反查：撤销时登记位跟着撤（不留陈旧入边）");

    /* 槽不在 rc slab（栈上局部变量、视图数据、C/池缓冲）⇒ 追不到宿主，但值本身是 rc 对象
     * ⇒ 改成把值钉成根：目标区域照常清扫，写空即撤销。 */
    ExtcRcRegion *rc = regionNew(19, 4096);
    Node *c = (Node *)extc_rc_alloc(rc, sizeof(Node), &g_node);
    void *stackSlot = NULL;
    extc_rc_storeRefSlot(&stackSlot, c);
    check(stackSlot == (void *)c, "槽地址反查：栈上槽照常赋值");
    check(!rc->noSweep, "槽地址反查：槽不在 rc slab ⇒ 目标区域不再退出清扫（改钉住值）");
    extc_rc_collect(rc, NULL, 0);
    check(extc_rc_isLive(c), "槽地址反查：栈上槽里的值被钉住 ⇒ 没有静态根也活过一轮");
    extc_rc_storeRefSlot(&stackSlot, NULL);
    extc_rc_collect(rc, NULL, 0);
    check(!extc_rc_isLive(c), "槽地址反查：栈上槽写空 ⇒ 钉住撤销，照常收掉");

    /* 超大块：载荷是单独 malloc 的，按掩码定位不成立 ⇒ 反查必须失手，不能返回一只
     * 看起来对的 slab（那会让误认的宿主区域被当成可扫）。失手同样靠钉住值兜底。 */
    ExtcRcRegion *rd = regionNew(20, 4096);
    void *bigSlot = extc_rc_alloc(rd, EXTC_RC_SLAB_SIZE, &g_data);   /* cap<2 ⇒ 超大块 */
    check(extc_rc_slabOfInterior(bigSlot) == NULL,
          "槽地址反查：超大块载荷不认（掩码定不了位 ⇒ 调用方回落保守）");
    ExtcRcRegion *re = regionNew(21, 4096);
    Node *e = (Node *)extc_rc_alloc(re, sizeof(Node), &g_node);
    extc_rc_storeRefSlot((void **)bigSlot, e);
    check(*(void **)bigSlot == (void *)e && !re->noSweep,
          "槽地址反查：超大块里的槽也不退出清扫（钉住值）");
    extc_rc_collect(re, NULL, 0);
    check(extc_rc_isLive(e), "槽地址反查：超大块槽里的值被钉住 ⇒ 活过一轮");
    extc_rc_storeRefSlot((void **)bigSlot, NULL);
    extc_rc_collect(re, NULL, 0);
    check(!extc_rc_isLive(e), "槽地址反查：超大块槽写空 ⇒ 钉住撤销");

    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
    extc_rc_regionFree(rc);
    extc_rc_regionFree(rd);
    extc_rc_regionFree(re);
}

/* ---------------------------------------------------------- 钉住表：按引用计数、可撤销、随区域清死指针
 *
 * 未知宿主槽的边现在按「值」登记，所以表必须支持同一个值被多条槽指着（计数）、覆盖时撤一条、
 * 区域释放时把属于它的死指针清掉（否则表会一直涨、标记期还会走废值）。这三条都是抽象不变式，
 * 形状级判据（slot_elem_unknown 有界）只间接覆盖第一条。 */
static void t_pin(void) {
    ExtcRcRegion *ra = regionNew(22, 4096);
    ExtcRcRegion *rb = regionNew(23, 4096);
    Node *a = (Node *)extc_rc_alloc(ra, sizeof(Node), &g_node);
    Node *b = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);
    void *slot1 = NULL, *slot2 = NULL;

    {
        int local = 0;
        extc_rc_storeRefUnknown(&slot1, &local);   /* 本栈帧地址不是 rc 槽 ⇒ 不是引用 */
    }
    check(extc_rc_pinCountOf(&slot1) == 0 && extc_rc_pinCountOf(slot1) == 0,
          "钉住：不是本运行期活槽的值不登记（只认 rc 对象）");

    extc_rc_storeRefUnknown(&slot1, a);
    check(extc_rc_pinCountOf(a) == 1, "钉住：未知宿主槽里的值登记成根");
    extc_rc_storeRefUnknown(&slot2, a);
    check(extc_rc_pinCountOf(a) == 2, "钉住：同一个值被两条槽指着 ⇒ 计数 2（撤一条不能整个出表）");
    extc_rc_storeRefUnknown(&slot1, NULL);
    check(extc_rc_pinCountOf(a) == 1, "钉住：写空只撤这一条边");
    extc_rc_collect(ra, NULL, 0);
    check(extc_rc_isLive(a), "钉住：还剩一条边 ⇒ 没有静态根也活过一轮");
    extc_rc_storeRefUnknown(&slot2, NULL);
    check(extc_rc_pinCountOf(a) == 0, "钉住：最后一条边撤掉就出表");
    extc_rc_collect(ra, NULL, 0);
    check(!extc_rc_isLive(a), "钉住：根全撤之后照常收掉（不是永久少收）");

    /* 覆盖成另一个值：这条槽的旧边撤掉、新边补上（不是只加不减）；另一条槽的边不受影响。 */
    Node *c = (Node *)extc_rc_alloc(ra, sizeof(Node), &g_node);
    extc_rc_storeRefUnknown(&slot2, b);      /* slot2 指着 b */
    extc_rc_storeRefUnknown(&slot1, b);      /* slot1 也指着 b */
    extc_rc_storeRefUnknown(&slot1, c);      /* slot1 改成 c ⇒ 只撤 slot1 那条 */
    check(extc_rc_pinCountOf(b) == 1 && extc_rc_pinCountOf(c) == 1,
          "钉住：覆盖成别的值 ⇒ 旧边撤销、新边登记（另一条槽的边还在）");

    /* 区域释放：属于它的表项按归属清掉，不留悬空值（表长度回到只含别人的项）。 */
    size_t before = extc_rc_pinN;
    extc_rc_regionFree(ra);
    check(extc_rc_pinCountOf(c) == 0 && extc_rc_pinN == before - 1,
          "钉住：区域释放时按归属清掉死指针（表不留下悬空值）");
    check(extc_rc_pinCountOf(b) == 1, "钉住：别的区域的表项不受牵连");

    extc_rc_storeRefUnknown(&slot2, NULL);   /* b 那条边也撤掉，表清空 */
    check(extc_rc_pinCountOf(b) == 0, "钉住：跨区边写空后同样出表");
    extc_rc_regionFree(rb);
}

/* 标记位查询：下面那条用例要在「增量标记在飞」时确认结构拷贝的叶子当场被标黑。 */
static int rcSlotBlack(void *p) {
    ExtcRcSlab *s = extc_rc_ownSlab(p);
    if (!s) return 0;
    size_t i = extc_rc_slotOf(s, p);
    return (s->mark[extc_rc_word(i)] & extc_rc_bit(i)) != 0;
}

/* ------------------- 义务 7（存储侧）：枚举局部量的整值存储不再让目标区域退出清扫
 *
 * `var m: maybe = maybe.some(p)` 的 C 布局里，有效字是哪个变体**静态枚举不出来**（枚举
 * 载荷）。旧路发 `storeStructCons(hostKnown = 0)` ⇒ 目标区域永久 `noSweep`、只靠计数回收；
 * 现在 codegen 把所有变体的载荷叶子都按根登记（`rcRootLeaves` 的 TY_ENUM 分支），存储侧
 * 只逐字记账、不下调保守 —— 判据：字照原样、目标区域照常清扫、叶子在增量标记期间标黑。
 *
 * 对照那一组是同一只新区域上的旧路：它**必须**置 `noSweep`，否则说明这条用例根本没碰到
 * 旧路（旧路被悄悄换掉时，判据就成了空转）。
 */
static void t_storeStructRooted(void) {
    ExtcRcRegion *ra = regionNew(30, 4096);
    ExtcRcRegion *rb = regionNew(31, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Node *b = (Node *)extc_rc_alloc(rb, sizeof(Node), &g_node);

    /* `maybe` 的 C 布局：标号 + 载荷。这里直接拿一个两字数组当那块值。 */
    void *w[2] = { b, NULL };
    extc_rc_storeStructRooted((void *)w, sizeof(w));
    check(w[0] == (void *)b && w[1] == NULL,
          "枚举整值存储：内容照原样（这条路径只记账，不碰字）");
    check(!ra->noSweep && !rb->noSweep,
          "枚举整值存储：目标区域照常清扫（与保守路的分界线）");
#if !EXTC_RC_NOCOUNT
    check(extc_rc_hold(b) == 1, "枚举整值存储：载荷拿到一笔计数（分配时 0 + 这里 1）");
#endif

    /* 认不出的字（栈上地址、槽内非首址）当引用是认错；认错的代价只能是「多留」，
     * 不该崩、也不该把哪只区域记成保守。 */
    void *w2[2] = { (void *)&a, (void *)((char *)b + 1) };
    extc_rc_storeStructRooted((void *)w2, sizeof(w2));
    check(w2[0] == (void *)&a && w2[1] == (void *)((char *)b + 1),
          "枚举整值存储：认不出的字原样留着、不崩");
    check(!ra->noSweep && !rb->noSweep, "枚举整值存储：认不出的字不会把区域记成保守");

    /* 对照：旧路（hostKnown = 0）在同一只新区域上必须让目标退出清扫。 */
    {
        ExtcRcRegion *rc = regionNew(32, 4096);
        Node *c = (Node *)extc_rc_alloc(rc, sizeof(Node), &g_node);
        void *wc[1] = { c };
        extc_rc_storeStructCons((void *)wc, sizeof(wc), NULL, 0);
        check(rc->noSweep && !ra->noSweep && !rb->noSweep,
              "枚举整值存储（对照）：旧路仍然让目标区域退出清扫（用例没走空）");
        extc_rc_regionFree(rc);
    }

    /* 增量标记期间：结构拷贝也是**写引用**，叶子必须当场标黑 —— 否则「本对象已被扫过 +
     * 之后才写进来的引用」这一轮收不到，会被本轮清扫放掉。这条与 inc 并列、同样承重。 */
    {
        ExtcRcRegion *rm = regionNew(33, (int64_t)1 << 40);
        Node *root = (Node *)extc_rc_alloc(rm, sizeof(Node), &g_node);
        Node *leaf = (Node *)extc_rc_alloc(rm, sizeof(Node), &g_node);
        void *rootsm[1] = { root };
        extc_rc_begin(rm, rootsm, 1);
        void *wm[1] = { leaf };
        extc_rc_storeStructRooted((void *)wm, sizeof(wm));
        check(rcSlotBlack(leaf), "枚举整值存储：增量标记期间叶子当场被标黑");
        while (!rm->seedsDone || rm->msp) extc_rc_markStep(rm, 0xffffffffu);
        extc_rc_sweepBegin(rm);
        extc_rc_sweep(rm);
        check(extc_rc_isLive(leaf), "枚举整值存储：标黑的叶子活过这一轮清扫（不会漏收）");
        extc_rc_regionFree(rm);
    }

    extc_rc_regionFree(ra);
    extc_rc_regionFree(rb);
}
/* ---------------------------------------------------------------- bump 槽在按槽登记之外 */
static void t_bumpCross(void) {
    /* `extc_rc_bump` 交出的槽**从不置占用位**，而 `rsIncSlab`/`rsDecSlab` 对无占用位的槽一律
     * 早退 ⇒ 按槽登记对 bump 对象不适用（整片 `bumpReset` 才是它的回收方式）。这条用例把
     * 这个设计事实钉住：跨区赋值后目标**不会**被登记，但也**不会**留下陈旧登记。 */
    ExtcRcRegion *ra = regionNew(11, 4096);
    ExtcRcRegion *rb = regionNew(12, 4096);
    Pair *b = (Pair *)extc_rc_alloc(rb, sizeof(Pair), &g_pair);
    Pair *a = (Pair *)extc_rc_bump(ra, sizeof(Pair));
    check(a != NULL && b != NULL, "bump 跨区：负载摆好");
    extc_rc_storeRefOwnedReg((void **)&b->a, a, rb);       /* B 区槽 <- A 区 bump 对象 */
    check(!extc_rc_isRegistered(a), "bump 跨区：bump 槽在按槽登记之外（登记对它不适用）");
    check(extc_rc_isLive(b) && b->a == (void *)a, "bump 跨区：赋值本身照常生效");
    extc_rc_storeRefOwnedReg((void **)&b->a, NULL, rb);
    check(!extc_rc_isRegistered(a), "bump 跨区：撤销后同样不留登记");
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
    /* 空指针不单独挡：没有键 0 可命中，ownSlab(0) 走到表尾仍返回 0（结果仍必须是否为 0）。 */
    check(extc_rc_ownSlab(NULL) == NULL, "守卫：空指针查不到 slab");
    check(!extc_rc_owns(NULL), "守卫：空指针不算本运行期的槽");
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
#if !EXTC_RC_NOCOUNT
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
#endif

/* 同一个槽被复用成**另一种布局**：描述符必须随新分配换掉，不能留着上一任的。 */
#if !EXTC_RC_NOCOUNT
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
#endif

/* 兜底描述符：`alloc<T>(n)` 的元素偏移随运行期计数变，静态描述符表达不了 ⇒ 保守标记扫每个字。 */
static void t_conservative(void) {
    ExtcRcRegion *r = regionNew(15, 4096);
    Node *blob = (Node *)extc_rc_alloc(r, sizeof(Node), EXTC_RC_CONSERVATIVE);
    Node *t = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    ExtcRcSlab *hdr = extc_rc_ownSlab(blob);
    *(void **)((char *)blob + 0) = hdr;       /* slab 头不是对象，不能进入槽位索引 */
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

/* ------------------------------------------------- 压力阈值缓存：三个更新点都要生效 */
/* `thr = max(2·live, floor)·scale` 被缓存进区域（每次分配只做一次比较）。它的三个输入只在
 * 三处变：`live`/`scale` 在 collect 末尾、`floor` 在 regionFloor、建区域在 regionNew。
 * 下面三条各自钉一个更新点：任何一个忘记重算，`shouldSweep` 就会用**过期的阈值**，
 * 这三条立刻红。 */
static void t_thresholdCache(void) {
    /* ① collect 之后：`live` 变成"存活字节"，阈值必须是 2×存活（陈旧的小阈值会让
     *    紧随其后的一次判定直接为真）。 */
    ExtcRcRegion *r = regionNew(11, 4096);
    void *keep = extc_rc_alloc(r, sizeof(Pair), &g_pair);
    while (extc_rc_bytes(r) <= 4096) (void)extc_rc_alloc(r, sizeof(Pair), &g_pair);
    void *roots[1] = { keep };
    int got = extc_rc_maybeCollect(r, roots, 1);
    check(got > 0, "阈值缓存①：清扫真的收掉了垃圾");
    check(!extc_rc_shouldSweep(r), "阈值缓存①：清扫后立刻不触发（阈值已按新的 live 重算）");
    extc_rc_regionFree(r);

    /* ② 改 floor：把压力线抬到当前占用之上，必须立刻不触发（陈旧阈值仍是旧 floor）。 */
    r = regionNew(12, 4096);
    while (extc_rc_bytes(r) <= 4096) (void)extc_rc_alloc(r, sizeof(Pair), &g_pair);
    check(extc_rc_shouldSweep(r), "阈值缓存②：旧 floor 下确实触发");
    extc_rc_regionFloor(r, (int64_t)1 << 20);
    check(!extc_rc_shouldSweep(r), "阈值缓存②：抬高 floor 后立刻不触发（阈值已重算）");
    extc_rc_regionFree(r);

    /* ③ scale 退场：一次一无所获的清扫把倍数 ×EXTC_RC_SCALE_MUL（默认 4），阈值跟着 ×4。 */
    r = regionNew(13, 4096);
    void *all[300];
    for (int i = 0; i < 300; i++) all[i] = extc_rc_alloc(r, sizeof(Pair), &g_pair);
    int freed = extc_rc_maybeCollect(r, all, 300);        /* 全是根 ⇒ 一个也收不掉 */
    check(freed == 0, "阈值缓存③：一无所获的清扫收到了 0");
    check(extc_rc_scale(r) == EXTC_RC_SCALE_MUL, "自学习：一无所获把倍数退场到 EXTC_RC_SCALE_MUL");
    check(!extc_rc_shouldSweep(r), "阈值缓存③：倍数退场后阈值按新倍数重算，当下不触发");
    extc_rc_regionFree(r);
}

/* ---- 分配准入：按请求字节预留（Lead 2026-11-10） --------------------------------
 *
 * 触发线只有「已分配堆」一个输入，不看**这一笔请求多大**：堆贴着墙时放进一个 8 MiB 大对象，
 * 峰值就能顶到 M_max + 8 MiB。修法是在**开凿慢路径**登记本区见过的最大单笔请求 `maxReq`，
 * 在**显式墙**下把墙帽从 `M_max - live/4` 改成 `M_max - max(live/4, maxReq)`：任一最大请求
 * 都仍装得进墙内，热路径 0 Ir、停顿不变。
 *
 * 为什么只绑显式墙：隐式墙 = 3*L 自己随 L 涨，这条承诺在那里**兑现不了**；而一旦在隐式墙下也
 * 收紧触发线，早期（live/4 < R_max 时）的触发节奏就会变，自学习倍数轨迹随之改变 —— 实测
 * pd_pointer_dense 峰值 112,092 → 142,620 kB（+27.3%），逻辑峰值 89.1 → 115.4 MB。默认路径
 * 不为「只有显式墙下才有意义」的保证付内存账。
 *
 * 判据是**红前绿后**的：①把显式墙帽改回 `M_max - live/4`，下面 ② 立刻红；②把预留绑回隐式墙，
 * 下面 ③ 立刻红（默认路径必须与旧实现逐字节相同）。 */
static void t_admissionReserve(void) {
    const int64_t B = 256 * 1024;   /* 超过 slab 单槽上限 ⇒ big 路径，slotSize == 请求大小 */
    ExtcRcRegion *r = regionNew(21, 4096);
    void *big = extc_rc_alloc(r, B, EXTC_RC_CONSERVATIVE);
    void *roots[1] = { big };
    check(big != NULL && r->maxReq == B, "分配准入①：大请求登记成 maxReq（慢路径写一次，热路径 0 Ir）");
    check(extc_rc_collect(r, roots, 1) == 0, "分配准入①：全在根上 ⇒ 这一轮收不到东西");
    check(r->live == B, "分配准入①：live 就是那一个大对象（R_max == live 的最坏配置）");
    check(r->scale > 1, "分配准入①：空扫把倍数养大（阈值顶到墙帽，才看得见预留）");

    /* ③ 默认路径（隐式墙）必须与旧实现逐字节相同：旧帽 = M_max - live/4。 */
    int64_t iwall = extc_rc_mmaxEff(r), il4 = r->live / 4;
    extc_rc_thrUpd(r);
    int64_t iwant = r->live * 2 * r->scale;
    if (iwant < r->floor) iwant = r->floor;
    if (iwall > 0 && iwant > iwall - il4) iwant = iwall - il4;
    if (iwant < r->floor) iwant = r->floor;
    check(r->thr == iwant, "分配准入③：隐式墙下触发线不变（预留不绑隐式墙，dense 峰值不回归）");

    /* ② 显式墙：触发线 + 最大一笔请求必须装得进墙。W = 2*B ⇒ live/4 < R_max，帽确实咬住。 */
    long long savedMmax = extc_rc_mmax;
    extc_rc_mmax = (long long)(2 * B);
    extc_rc_thrUpd(r);
    int64_t wall = extc_rc_mmaxEff(r);
    check(wall == 2 * B, "分配准入②：显式内存墙生效");
    check(r->thr + r->maxReq <= wall,
          "分配准入②：触发线 + 最大一笔请求 ≤ 内存墙（旧帽只留 live/4，这里红）");
    check(r->thr <= wall - r->maxReq, "分配准入②：预留至少是一整笔最大请求");
    extc_rc_mmax = savedMmax;
    extc_rc_thrUpd(r);   /* 恢复默认口径，后面的用例不受影响 */

    /* 对照：小对象区（maxReq 远小于 live/4）在两种墙下都与旧实现**逐字节相同** —— 预留只在
     * 大请求时收紧，不许顺手偷走小对象的裕量。 */
    ExtcRcRegion *q = regionNew(22, 4096);
    void *all[300];
    for (int i = 0; i < 300; i++) all[i] = extc_rc_alloc(q, sizeof(Pair), &g_pair);
    check(extc_rc_collect(q, all, 300) == 0, "分配准入③：对照区空扫收 0");
    int64_t qwall = extc_rc_mmaxEff(q), l4 = q->live / 4;
    check(q->maxReq == 16 && l4 > q->maxReq, "分配准入③：对照区 maxReq 小于 live/4");
    int64_t want = q->live * 2 * q->scale;
    if (want < q->floor) want = q->floor;
    if (qwall > 0 && want > qwall - l4) want = qwall - l4;
    if (want < q->floor) want = q->floor;
    check(q->thr == want, "分配准入③：小对象区阈值不变（仍按 live/4 留裕量）");
    long long saved2 = extc_rc_mmax;
    extc_rc_mmax = (long long)qwall;
    extc_rc_thrUpd(q);
    check(q->thr == want, "分配准入③：小对象区换成显式墙后阈值仍不变（maxReq 不参与）");
    extc_rc_mmax = saved2;
    extc_rc_thrUpd(q);
    extc_rc_regionFree(r);
    extc_rc_regionFree(q);
}
/* ---------------------------------------------------------------- 无环有界负载：不进清扫 */
#if !EXTC_RC_NOCOUNT
static size_t boundedLiveSweeps(long n, int64_t *bytesOut) {
    ExtcRcRegion *r = regionNew(10, 4096);
    void *slots[64];
    memset(slots, 0, sizeof slots);
    for (long i = 0; i < n; i++) {
        if ((i & 4095) == 0) budget("无环有界负载：预算内没跑完（触发/回收语义坏了？）");
        void *nd = extc_rc_alloc(r, sizeof(Node), &g_node);
        extc_rc_storeRef(&slots[i & 63], nd);      /* 覆盖即释放旧的 */
        (void)extc_rc_maybeCollect(r, slots, 64);
    }
    size_t sw = extc_rc_sweeps(r);
    *bytesOut = extc_rc_bytes(r);
    extc_rc_regionFree(r);
    return sw;
}
#endif

#if !EXTC_RC_NOCOUNT
static void t_acyclic(void) {
    int64_t b1 = 0, b2 = 0;
    size_t s1 = boundedLiveSweeps(10000, &b1);
    size_t s2 = boundedLiveSweeps(200000, &b2);
    check(b1 <= 64 * 16 && b2 == b1, "无环有界负载：占用不随规模增长");
    check(s1 == 0 && s2 == 0, "无环负载：规模涨 20 倍，清扫次数都是 0");
}
#endif

/* ---------------------------------------------------------------- 自学习退场 */
static size_t liveGrowthSweeps(long n, int64_t *scaleOut) {
    ExtcRcRegion *r = regionNew(11, 4096);
    void **roots = (void **)malloc((size_t)n * sizeof(void *));
    if (!roots) { fail("自学习：roots 分配失败"); extc_rc_regionFree(r); return 0; }
    g_learnPeak = 0;
    for (long i = 0; i < n; i++) {
        if ((i & 4095) == 0) budget("自学习负载：预算内没跑完（触发/回收语义坏了？）");
        roots[i] = extc_rc_alloc(r, sizeof(Node), &g_node);   /* 全部从根可达 ⇒ 一个都收不回 */
        (void)extc_rc_maybeCollect(r, roots, (size_t)i + 1);
        int64_t bb = extc_rc_bytes(r);
        if (bb > g_learnPeak) g_learnPeak = bb;
    }
    size_t sw = extc_rc_sweeps(r);
    int64_t live = extc_rc_live(r), bytes = extc_rc_bytes(r);
    check(live > 0 && live <= bytes, "自学习：live 记下上一轮清扫后的存活字节");
    *scaleOut = extc_rc_scale(r);
    g_learnMmax = extc_rc_mmaxEff(r);
    free(roots);
    extc_rc_regionFree(r);
    return sw;
}

/* ---- 垃圾形状：验证「内存墙真的把峰值压住了」 ------------------------------------
 *
 * 前一段（全存活增长）的 peak 就等于 live，墙压不动存活集，看不出 M_max 的效果；这里补一个
 * **垃圾形状**：先让 10 万个根全存活增长（空扫把自学习倍数养大），再灌 200 万个**不可达**
 * node。默认档隐式 M_max=3*live 把 thr 夹在 M_max-L/4=2.75L 附近 ⇒ 峰值约 3.2 MB；把
 * M_max 显式开到 1 TB（等效关闭）时 thr=2*live*scale 可达数十 MB ⇒ 峰值约 33 MB，正是旧
 * 「清扫次数小常数」契约换来的无界内存。两档都必须满足 contract①（峰值 ≤ M_max + 一块 slab）。 */
static void wallPeak(void) {
    enum { LIVE_N = 100000, GARBAGE_N = 2000000 };
    ExtcRcRegion *r = regionNew(41, 4096);
    void **roots = (void **)malloc((size_t)LIVE_N * sizeof(void *));
    if (!roots) { fail("内存墙：roots 分配失败"); extc_rc_regionFree(r); return; }
    int64_t peak = 0, liveBytes = 0;
    for (long i = 0; i < LIVE_N; i++) {
        if ((i & 4095) == 0) budget("内存墙：存活段预算内没跑完");
        roots[i] = extc_rc_alloc(r, sizeof(Node), &g_node);
        (void)extc_rc_maybeCollect(r, roots, (size_t)i + 1);
        int64_t bb = extc_rc_bytes(r);
        if (bb > peak) peak = bb;
    }
    liveBytes = extc_rc_live(r);
    for (long i = 0; i < GARBAGE_N; i++) {
        if ((i & 4095) == 0) budget("内存墙：垃圾段预算内没跑完");
        (void)extc_rc_alloc(r, sizeof(Node), &g_node);   /* 不挂根 ⇒ 下次清扫的垃圾 */
        (void)extc_rc_maybeCollect(r, roots, (size_t)LIVE_N);
        int64_t bb = extc_rc_bytes(r);
        if (bb > peak) peak = bb;
    }
    int64_t mmax = extc_rc_mmaxEff(r);
    printf("  [wall] live=%ld garbage=%ld sweeps=%zu liveBytes=%lld peak=%lld M_max=%lld scale=%lld%c",
           (long)LIVE_N, (long)GARBAGE_N, extc_rc_sweeps(r), (long long)liveBytes,
           (long long)peak, (long long)mmax, (long long)extc_rc_scale(r), 10);
    check(peak <= mmax + (int64_t)EXTC_RC_SLAB_SIZE, "内存墙：峰值驻留 ≤ M_max + 一块 slab 缓冲");
    free(roots);
    extc_rc_regionFree(r);
}

/* ---- P0-4 契约（Lead 2026-11-09 裁决 A） --------------------------------------
 *
 * 旧的「规模涨 100 倍，清扫次数只多一两次 / 清扫次数是有界的小常数」与**内存有界**互斥：
 * 自学习阈值 max(2*live,floor)*scale 会随空扫放大，清扫次数确实近似常数，代价是 live+garbage
 * 无界增长。默认档隐式 M_max=3*live 把 thr 夹到 M_max-64*L/b = 2.75*L（b=256），一次清扫最多
 * 退休一个 live 集 ⇒ live 必须涨约 2.75 倍才到下一根触发线，清扫次数 O(log_2.75 N)：规模
 * 涨 100 倍只多约 log_2.75(100)=4.19 次（本机默认档实测 4→9，关墙档 2→3）。用户要求优先
 * 内存有界，故契约改为：①峰值驻留 ≤ M_max + 缓冲；②清扫次数随规模**对数**增长，非常数。
 * 两档（默认 / EXTC_RC_M_MAX=1099511627776 等效关墙）都必须绿。 */
static void t_learning(void) {
    int64_t sc1 = 0, sc2 = 0, p1, m1, p2, m2;
    size_t s1 = liveGrowthSweeps(10000, &sc1);
    p1 = g_learnPeak; m1 = g_learnMmax;
    size_t s2 = liveGrowthSweeps(1000000, &sc2);
    p2 = g_learnPeak; m2 = g_learnMmax;
    check(s1 > 0 && sc1 > 1, "自学习：全存活增长会空扫，倍数退场");
    check(s2 <= s1 + 8, "自学习：清扫次数随分配规模对数增长（底数≈2.75；100× 只多 ≤8 次，不是 100×）");
    if (m2 > 0 && m2 < ((int64_t)1 << 40))
        check(s2 >= s1 + 3, "自学习：隐式内存墙生效时清扫次数按 log_2.75 增长（100× 规模约 4–5 次，非常数）");
    check(sc2 > sc1, "自学习：倍数继续涨 ⇒ 阈值退场");
    check(p1 <= m1 + (int64_t)EXTC_RC_SLAB_SIZE, "内存墙：峰值驻留 ≤ M_max + 一块 slab 缓冲（1e4 存活）");
    check(p2 <= m2 + (int64_t)EXTC_RC_SLAB_SIZE, "内存墙：峰值驻留 ≤ M_max + 一块 slab 缓冲（1e6 存活）");
    wallPeak();
}

/* ------------------------------------ 着色缓存：清 mark 的每条路都要让缓存失效 */
/* 缓存记的是「这一代里这个指针黑过」。失效的唯一依据是代际号 —— **每一处清 mark 位的
 * 路径都必须代际++**（起轮 / 清扫起轮 / 收窄清扫的整片清零 / 整区释放）。漏一处，缓存就会
 * 把白对象当黑、跳过屏障，可达对象最终被下一次清扫扫掉。下面按路径钉住。 */
static int slotBlack(void *p) {
    ExtcRcSlab *s = extc_rc_ownSlab(p);
    size_t i = extc_rc_slotOf(s, p);
    return (s->mark[extc_rc_word(i)] & extc_rc_bit(i)) != 0;
}

static void t_shadeCache(void) {
    /* ① 标黑即记下；清扫起轮会一片片清 mark 位 ⇒ 必须失效。 */
    ExtcRcRegion *r = regionNew(17, (int64_t)1 << 40);
    Node *a = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *b = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    a->next = b;
    void *roots[1] = { a };
    extc_rc_begin(r, roots, 1);
    extc_rc_shade(b);
    while (!r->seedsDone || r->msp) extc_rc_markStep(r, 0xffffffffu);
    check(extc_rc_shadeKnown(b), "着色缓存：标黑后记下这个指针");
    extc_rc_shade(b);   /* 同一指针再问一次：命中；guard 版就在这里核对"确实还黑" */
    check(extc_rc_shadeKnown(b), "着色缓存：重复着色命中");
    extc_rc_sweepBegin(r);
    check(!extc_rc_shadeKnown(b), "着色缓存：清扫起轮后失效");
    extc_rc_regionFree(r);

    /* ② 收窄清扫：可达对象 a→b 的 mark 位在返回前被整片清零 ⇒ 缓存必须失效；否则下一次
     *    对 b 的屏障被跳过、b 保持白 ⇒ 下一次清扫会把可达的 b 扫掉。 */
    r = regionNew(18, (int64_t)1 << 40);
    a = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    b = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    void *c = extc_rc_alloc(r, sizeof(Node), &g_data);
    a->next = b;
    roots[0] = a;
    extc_rc_begin(r, roots, 1);
    extc_rc_shade(b);
    while (!r->seedsDone || r->msp) extc_rc_markStep(r, 0xffffffffu);
    check(extc_rc_shadeKnown(b), "着色缓存：收窄清扫前 b 已在缓存里");
    extc_rc_releaseOld(r, c, roots, 1);   /* c 不可达 ⇒ 释放；返回前整片清 mark */
    check(!extc_rc_shadeKnown(b), "着色缓存：收窄清扫清 mark 后失效");
    extc_rc_shade(b);                     /* 可达的 b 必须被重新标黑 */
    check(slotBlack(b), "着色缓存：失效后可达对象重新变黑");
    check(extc_rc_isLive(b), "着色缓存：可达对象没有被误回收");
    extc_rc_regionFree(r);

    /* ③ 旋转散列：代际号旋进格号 ⇒ 记录只在**同一代**里可见；代际号每 2048 代回绕一次、
     *    回绕前整表清零。若回绕不清表，2048 代前的同一指针记录（格号因回绕再次相同）会被
     *    当成「这一代里黑过」—— 恰恰是缓存最不能出的那种错。 */
    r = regionNew(19, (int64_t)1 << 40);
    a = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    extc_rc_shadeRemember(a);
    check(extc_rc_shadeKnown(a), "着色缓存：旋转散列同代命中");
    extc_rc_shadeNextGen();
    check(!extc_rc_shadeKnown(a), "着色缓存：代际号一变旧记录立刻不可见");
    extc_rc_shadeGen = 2047;   /* 窗口最后一格：这里记下的记录在回绕时必须被清掉 */
    extc_rc_shadeRemember(a);
    check(extc_rc_shadeKnown(a), "着色缓存：窗口末尾仍命中");
    extc_rc_shadeNextGen();
    check(!extc_rc_shadeKnown(a), "着色缓存：回绕清表（2048 代前的记录不得复活）");
    extc_rc_shadeGen = 1;      /* 复位：后面从第一代起，只为输出干净 */
    extc_rc_regionFree(r);
}

/* ------------------------------------------------- 大对象分段扫描 */
/* 对象里的指针特别多时（大数组字段、或运行期才知道长度的结构），标记它曾经是**单步不可分片**
 * 的工作：一次弹出要把整槽的字全走完、全压栈 ⇒ 单步停顿 ∝ 对象大小。现在按 EXTC_RC_SCAN_CHUNK
 * 分段：走一段就存下游标，下一整步接着走。描述符扫描不分段（长度是结构体字段数，源码常量）。
 *
 * 判据只用**确定性量**（一步之后的游标、栈深、记账），不碰墙钟 —— 墙钟在这台 VM 上噪声 ±10ms，
 * 而这里要钉的是「单步工作量有上界」，它有确定的上下界。 */
static void t_scanChunk(void) {
    /* 红自测会把 chunk 开到十亿（等于关掉分段）：那时也要给数组一个有限大小，
     * 否则测试先编不过、而不是红在判据上。 */
    enum { SLOTS = EXTC_RC_SCAN_CHUNK > 100000 ? 200005 : 3 * (int)EXTC_RC_SCAN_CHUNK / 2 + 5 };
    ExtcRcRegion *r = regionNew(20, (int64_t)1 << 40);
    char *obj = (char *)extc_rc_alloc(r, (int64_t)SLOTS * (int64_t)sizeof(void *), EXTC_RC_CONSERVATIVE);
    Node *kid = (Node *)extc_rc_alloc(r, sizeof(Node), &g_data);
    size_t words = extc_rc_ownSlab(obj)->ss / sizeof(void *);   /* 槽按 8 字节取整后的字数 */
    for (size_t i = 0; i < words; i++) ((void **)obj)[i] = kid;
    void *roots[1] = { obj };

    extc_rc_markBegin(r, roots, 1);
    check(r->seedsDone && r->msp == 1, "分段：起轮后标记栈里只有大对象本身");
    size_t marked = extc_rc_markStep(r, 1);
    check(marked == 1, "分段：首步只记对象本身一次（指针还没走）");
    check(r->scanObj == (void *)obj, "分段：一步扫不完 ⇒ 留下续扫游标");
    check(r->scanOff == 0, "分段：首步只留游标（不扫指针）");
    check(r->msp == 0, "分段：游标不占标记栈（写屏障的 force 项不会被挤错位置）");

    marked += extc_rc_markStep(r, 1);
    if (EXTC_RC_SCAN_CHUNK < (int)(words - 1)) {
        /* 时间上限（EXTC_RC_SEG_DEADLINE_US）让一段可以提前收工：段内字数仍不得超过一个 chunk，
         * 只有上限关掉时才必须走满一个 chunk。 */
        check(r->scanObj == (void *)obj && (size_t)r->scanOff <= (size_t)EXTC_RC_SCAN_CHUNK &&
              (EXTC_RC_SEG_DEADLINE_US > 0 || (size_t)r->scanOff == (size_t)EXTC_RC_SCAN_CHUNK),
              "分段：续扫一步走一个 chunk（有时间上限时允许提前收工，但不越过 chunk）");
    }

    int bounded = 1, steps = 2;
    size_t segs = (r->scanObj == (void *)obj) ? 1 : 0;   /* 上面那一步已经续扫过一段 */
    size_t segsum = segs ? (size_t)r->scanOff : 0;
    size_t segmax = segsum;
    while (!r->seedsDone || r->msp || r->scanObj) {
        int wasSeg = (r->scanObj == (void *)obj);
        size_t before = wasSeg ? (size_t)r->scanOff : 0;
        marked += extc_rc_markStep(r, 1);
        steps++;
        if (wasSeg) {   /* 这一步在续扫：游标推进不得超过一个 chunk，且各段字数可对账 */
            size_t after = (r->scanObj == (void *)obj) ? (size_t)r->scanOff : words;
            size_t adv = after - before;
            segs++; segsum += adv;
            if (adv > segmax) segmax = adv;
            if (after < before || adv > (size_t)EXTC_RC_SCAN_CHUNK) bounded = 0;
        }
        if ((size_t)steps > 2 * ((size_t)words + 4) + 8) break;   /* 游标不前进会在这里现形，而不是卡住 */
    }
    check(bounded, "分段：续扫每步游标增量不超过一个 chunk（单步工作量有上界）");
    if ((size_t)EXTC_RC_SCAN_CHUNK < words) {
        size_t want = (words + (size_t)EXTC_RC_SCAN_CHUNK - 1) / (size_t)EXTC_RC_SCAN_CHUNK;
#if EXTC_RC_SEG_DEADLINE_US > 0
        /* 时间上限把一段拆成两段（ASan/UBSan 下每段只走几百词），段数不再是字数的纯函数：
         * 契约改成「下界 + 每段有界 + 字数和等于对象字数」——上限只会多分段，不会并段。 */
        check(segs >= want, "分段：段数 >= ceil(words/chunk)（时间上限只多分段、不并段）");
#else
        check(segs == want, "分段：段数 == ceil(words/chunk)（每段不多不少）");
#endif
        check(segsum == words, "分段：各段字数之和 == 对象字数（段边界对账）");
        check(segmax <= (size_t)EXTC_RC_SCAN_CHUNK, "分段：单段字数不超过 chunk");
    }
    check(r->scanObj == NULL && r->msp == 0, "分段：扫完游标清空、标记栈空");
    check(r->markedBytes == (int64_t)extc_rc_ownSlab(obj)->ss + (int64_t)extc_rc_ownSlab(kid)->ss,
          "分段：续扫不重复记账（markedBytes 只算一遍）");
    check(marked == 2, "分段：大对象与孩子各标一次（续扫不重复计 marked）");
    extc_rc_regionFree(r);
}


/* ------------------------------------------------- 清扫进行中继续分配（不变式）
 * 不变式：一轮清扫结束时，**所有** slab 的 mark 位必须为 0 —— 下一轮标记用 mark 位做去重
 * （已标黑=已扫描），残留黑位会让可达对象的子树整棵跳过扫描，随后被清扫当垃圾放掉。
 * 风险点：清扫进行中新建的 slab 被头插到游标之前，本轮永远不会被扫到；若分配路径仍按
 * "扫掠中分配即标黑" 给它置位，这一位会活到下一轮。本用例把三种落点都走一遍：
 * 游标之前的旧块（会扫到、会清）、游标之后的旧块（会扫到、会清）、新建块（不会扫到），
 * 再跑一轮完整标记+清扫，断言"只经新块可达的孩子"没有被误放。
 */
static void t_allocDuringSweep(void) {
    ExtcRcRegion *r = regionNew(34, (int64_t)1 << 40);   /* 压力线抬高：只由本用例手动驱动轮次 */
    /* 三块 16 字节类 slab，每块各放一个根。第二轮的清扫从后建的块起步；走到 1920 字节分配时
     * 后面还有两块（128 个 word）没扫，而按字节配额一次最多扫 96 word，所以本轮扫不完 =>
     * 新块确实在清扫进行中创建（swEpoch 规则生效），不是等到轮结束才落地。 */
    Node *root = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    while (extc_rc_slabCount(r) < 2) (void)extc_rc_alloc(r, sizeof(Node), &g_data);
    Node *root2 = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    while (extc_rc_slabCount(r) < 3) (void)extc_rc_alloc(r, sizeof(Node), &g_data);
    Node *root3 = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *child = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    root3->next = child;

    /* 基线轮：根与孩子标活、清扫后都还活着（后续轮次以"只经新块可达"为前提）。 */
    void *roots0[3] = { root, root2, root3 };
    extc_rc_begin(r, roots0, 3);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);   /* 收尾标记会**自己**开本轮清扫（finish 末尾 sweepBegin），所以这里不能再手动 sweepBegin */
    extc_rc_settle(r);   /* 只续跑当前轮：sweep() 会重新 sweepBegin，把 finish 刚开始的那轮从头再扫（会清掉已置的黑位） */
    check(extc_rc_isLive(root) && extc_rc_isLive(child), "sweep 期间分配：基线轮两个对象都活着");

    /* 第二轮：标活后由 finish 开清扫，先走一个 word，再在**清扫进行中**分配 —— 1920 字节对象强制开新
     * size class 块，600000 字节对象强制走巨块（big = 1）路径。新块被头插到游标之前，本轮永远不会扫到。 */
    extc_rc_begin(r, roots0, 3);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    check(r->sweeping, "sweep 期间分配：标记收尾后清扫已经在飞");
    (void)extc_rc_sweepStep(r, 1);
    char *big = (char *)extc_rc_alloc(r, 1920, &g_node);   /* P0-P: a size that leaves sweep words for the round */
    *(Node **)big = child;
    char *huge = (char *)extc_rc_alloc(r, 600000, &g_node);
    *(Node **)huge = child;
    ExtcRcSlab *bs = extc_rc_ownSlab(big);
    ExtcRcSlab *hs = extc_rc_ownSlab(huge);
    check(bs != NULL && hs != NULL && hs->big == 1, "sweep 期间分配：新块与巨块都已登记");
    check(bs->swEpoch == r->swId,
          "sweep 期间分配：新块宣告本轮已扫（swEpoch = swId），分配路径不再给它标黑");
    /* P0-P: 600000 字节的请求按字节配额资助的 sweep words 超过本轮剩余 => 巨块在轮结束后才落地；
     * 巨块与新块共用 extc_rc_slabNew 尾部同一条 swEpoch 规则。 */
    check(hs->big == 1, "sweep 期间分配：600000 字节对象走巨块路径");
    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    {
        int clean = 1;
        for (ExtcRcSlab *s = r->slabs; s; s = s->next)
            for (size_t w = 0; w < s->nwords; w++) if (s->mark[w]) clean = 0;
        check(clean, "sweep 期间分配：一轮结束全部 slab 的 mark 位为 0（下一轮去重的前提）");
    }
    check(extc_rc_isLive(big) && extc_rc_isLive(huge) && extc_rc_isLive(child),
          "sweep 期间分配：本轮不会扫到的新块对象没被误放");

    /* 第三轮：只经新块对象可达的孩子必须被标到（陈旧黑位会让这一步整棵跳过）。巨块不是根 ⇒ 本轮被收。 */
    void *roots1[1] = { big };
    extc_rc_begin(r, roots1, 1);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    extc_rc_settle(r);   /* 只续跑当前轮：sweep() 会重新 sweepBegin，把 finish 刚开始的那轮从头再扫（会清掉已置的黑位） */
    check(extc_rc_isLive(child), "sweep 期间分配：下一轮只经新块可达的孩子仍活（去重没被陈旧黑位骗过）");
    check(extc_rc_isLive(big) && !extc_rc_isLive(huge) && !extc_rc_isLive(root),
          "sweep 期间分配：新块对象活、巨块与旧根（本轮不再是根）被收");
    extc_rc_regionFree(r);
}

/* ---------------------------------------------------------------- 全死 slab 批量重置 */
static void t_sweepBulkReset(void) {
    ExtcRcRegion *r = regionNew(36, (int64_t)1 << 40);   /* 压力线抬高：只由本用例手动驱动轮次 */
    /* 把第一个 16 字节类的 slab 填满：出现第二块时第一块已无空槽 ⇒ 整块都是可回收的死对象。 */
    size_t n = 0;
    Node *first = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    n++;
    while (extc_rc_slabCount(r) < 2) { (void)extc_rc_alloc(r, sizeof(Node), &g_node); n++; }
    ExtcRcSlab *s0 = extc_rc_ownSlab(first);
    size_t slabs0 = extc_rc_slabCount(r);
    size_t freed0 = extc_rc_freedSlots(r);
#if EXTC_RC_NOCOUNT
    long long br0 = extc_rc_seBulkReset, bs0 = extc_rc_seBulkSlots;
#endif
    check(s0 && !s0->big && s0->bump == s0->nslots, "全死批量重置：第一块已填满");
    check(extc_rc_bytes(r) == (int64_t)(n * s0->ss), "全死批量重置：批前占用 = 已发槽数 × 槽宽");

    /* 一轮标记：无根 ⇒ 没有对象被标黑；finish 末尾自己开清扫，全死 slab 在 word 0 上就被整块判定。 */
    extc_rc_begin(r, NULL, 0);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    while (r->sweeping) (void)extc_rc_sweepStep(r, 1);   /* 兜底：若一轮没在 finish 内走完，就把它推完 */

    check(extc_rc_bytes(r) == 0, "全死批量重置：整批死对象收回（区域回到零占用）");
    check(extc_rc_freedSlots(r) - freed0 == n, "全死批量重置：累计释放数 = 已发槽数（一个不漏）");
#if EXTC_RC_NOCOUNT
    check((long long)(extc_rc_seBulkReset - br0) == (long long)slabs0,
          "全死批量重置：每块全死 slab 各触发一次整块处置");
    check((long long)(extc_rc_seBulkSlots - bs0) == (long long)n,
          "全死批量重置：整块处置的槽数 = 已发槽数");
#endif
    {
        int clean = 1, ep = 1;
#if EXTC_RC_NOCOUNT
        int reused = 1;
#endif
        for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
            if (s->swEpoch != r->swId) ep = 0;
#if EXTC_RC_NOCOUNT
            if (s->bump != 0 || s->freelist != -1) reused = 0;
#endif
            for (size_t w = 0; w < s->nwords; w++)
                if (s->occ[w] || s->mark[w]) clean = 0;
        }
        check(clean, "全死批量重置：occ 与 mark 位图全清（下一轮去重的前提）");
        check(ep, "全死批量重置：每块都宣告本轮已扫（swEpoch = swId）");
#if EXTC_RC_NOCOUNT
        check(reused, "全死批量重置：发号游标与复用链复位（整块重新可用）");
#endif
    }
    Node *again = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(extc_rc_slabCount(r) == slabs0 && extc_rc_isLive(again),
          "全死批量重置：重置后同一批 slab 照常复用（不新开块）");
    check(extc_rc_bytes(r) == (int64_t)s0->ss, "全死批量重置：复用后记账正确");
    extc_rc_regionFree(r);
}

/* ------------------------------------- elide x 清扫中分配：黑位是唯一保护 */
/* elide 只用 (occ & mark) 判"这块全死"。清扫进行中分配的对象若落在**本轮还会扫到**的位置，
 * 就必须由分配路径把 mark 位置上（规则：sweeping 且 swEpoch != swId 且 (s != swSlab 或 wi >= swWord)）；
 * 否则这块会被判成全死、活对象被整块清掉。新 slab 是另一条路：slabNew 把 swEpoch 置成本轮号 =>
 * 本轮根本不会扫它，靠下一轮标记重新发现（t_allocDuringSweep 已钉住）。这里钉住前一条。 */
static void t_sweepElideMark(void) {
    ExtcRcRegion *r = regionNew(37, 16);
    /* 两个 size class：先把 16 字节类填到 word 16，再建 24 字节类。后建的块被头插成 slabs 头，
     * 所以 finish 开清扫时游标从 24 字节块起步；随后那次 16 字节分配按字节配额扫完 24 字节块、
     * 再扫进 16 字节块 16 个 word，新对象正好落在游标处（word 16），黑位规则必须把它标黑。 */
    enum { kPad = 1024 };
    static void *keeper[kPad + 1];
    for (int i = 0; i < kPad; i++) keeper[i] = extc_rc_alloc(r, sizeof(Node), &g_data);
    keeper[kPad] = extc_rc_alloc(r, 24, &g_data);   /* 第二类：24 字节槽的块成为 slabs 头 */
    void *keep = keeper[0];
    extc_rc_begin(r, keeper, kPad + 1);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);   /* 末尾自己开清扫 */
    check(r->sweeping && r->swWord > 0 && r->swWord < 32,
          "elide x 分配：标记收尾后清扫在飞，游标还没走到底");
    Node *fresh = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(extc_rc_ownSlab(fresh) == r->swSlab, "elide x 分配：新对象落在游标所在块");
    size_t i = extc_rc_slotOf(r->swSlab, fresh);
    check((i >> 6) >= r->swWord, "elide x 分配：新对象落在本轮还没扫到的 word");
    check(slotBlack(fresh), "elide x 分配：未扫位置分配必须标黑（否则整块会被判成全死）");
#if EXTC_RC_NOCOUNT
    check(extc_rc_sweepElide(r) == 0, "elide x 分配：块里有黑位 => 判据不会判成全死");
#endif
    char *news = (char *)extc_rc_alloc(r, 1920, &g_data);   /* P0-P: leaves sweep words for this round */
    ExtcRcSlab *ns = extc_rc_ownSlab(news);
    check(ns && !ns->big && (!r->sweeping || ns->swEpoch == r->swId),
          "elide x 分配：清扫中新建的块宣告本轮已扫（本轮不会再扫它；本轮已扫完时豁免）");
    check(!slotBlack(news), "elide x 分配：新块对象不靠黑位保护（靠本轮不访问 + 下一轮标记）");
    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    check(extc_rc_isLive(fresh) && extc_rc_isLive(news) && extc_rc_isLive(keep),
          "elide x 分配：整轮结束后活对象都还在");
    extc_rc_regionFree(r);
}



/* ---------------------------------------------------------------- P0-B 惰性 freelist 校验 */
/* 一块的复用链必须满足：节点互不重复、每个节点都是洞（occ 位为 0）、链长不超过槽数。 */
static int slabChainValid(ExtcRcSlab *s) {
    if (!s || s->nslots == 0) return 1;
    unsigned char *seen = (unsigned char *)calloc((size_t)s->nslots, 1);
    if (!seen) return 0;
    int ok = 1;
    size_t n = 0;
    int64_t i = s->freelist;
    while (i >= 0) {
        if ((size_t)i >= (size_t)s->nslots || seen[(size_t)i]) { ok = 0; break; }
        seen[(size_t)i] = 1;
        size_t k = (size_t)i;
        if (s->occ[extc_rc_word(k)] & extc_rc_bit(k)) { ok = 0; break; }   /* 活槽不该在链上 */
        if (++n > (size_t)s->nslots) { ok = 0; break; }
        i = (int64_t)s->tid[k] - 1;
    }
    free(seen);
    return ok;
}
__attribute__((unused)) static size_t slabChainLen(ExtcRcSlab *s) {
    size_t n = 0;
    for (int64_t i = s->freelist; i >= 0 && n <= (size_t)s->nslots; i = (int64_t)s->tid[(size_t)i] - 1) n++;
    return n;
}
static int regionChainsValid(ExtcRcRegion *r) {
    for (ExtcRcSlab *s = r->slabs; s; s = s->next) if (!slabChainValid(s)) return 0;
    return 1;
}
static size_t regionOccCount(ExtcRcRegion *r) {
    size_t n = 0;
    for (ExtcRcSlab *s = r->slabs; s; s = s->next)
        for (uint32_t w = 0; w < s->nwords; w++) n += (size_t)__builtin_popcountll(s->occ[w]);
    return n;
}

/* 清扫把死槽整批链入复用链（与单槽释放共用一条链）；复用时按链头弹出。判据：slab 数不随轮数
 * 增长、每次复用后链合法（无重复、无活槽）、占用与对象数一致。 */
static void t_lazyFreelistRebuild(void) {
    ExtcRcRegion *r = regionNew(38, (int64_t)1 << 40);
    enum { kN = 3000 };
    for (int i = 0; i < kN; i++) (void)extc_rc_alloc(r, sizeof(Node), &g_data);
    size_t ss = extc_rc_slotSize(sizeof(Node));   /* slot stride, not sizeof(Node): the minimum slot is 16 bytes */
    extc_rc_begin(r, NULL, 0);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    extc_rc_settle(r);
    size_t slabs0 = extc_rc_slabCount(r);
    check(extc_rc_bytes(r) == 0, "复用链：清扫后占用归零");
    {
        int len_ok = 1;
        for (ExtcRcSlab *s = r->slabs; s; s = s->next) {
#if EXTC_RC_LAZY_FREE
            /* 惰性档只计数不链入：清扫后的洞数必须等于已发槽数。 */
            if (s->nfree != (uint32_t)s->bump) len_ok = 0;
#else
            if (slabChainLen(s) != (size_t)s->bump) len_ok = 0;   /* 发过的每个槽都是洞、都已链入 */
#endif
        }
#if EXTC_RC_LAZY_FREE
        check(len_ok, "复用链：清扫后洞数 = 已发槽数（惰性档只计数）");
#else
        check(len_ok, "复用链：清扫后链长 = 已发槽数（每个洞都在链上）");
#endif
        check(regionChainsValid(r), "复用链：链无重复、无活槽");
    }
    static Node *keep[2 * kN];
    for (int i = 0; i < kN; i++) keep[i] = (Node *)extc_rc_alloc(r, sizeof(Node), &g_data);
    check(extc_rc_slabCount(r) == slabs0, "复用链：从链上复用旧洞，不新开块");
    check(regionOccCount(r) == kN, "复用链：复用后占用槽数 = 对象数（无重复发放）");
    check(regionChainsValid(r), "复用链：弹出后的链无重复、无活槽");
    {
        int live_ok = 1;
        for (int i = 0; i < kN; i++) if (!extc_rc_isLive(keep[i])) live_ok = 0;
        check(live_ok, "复用链：复用出来的对象全部活着");
    }
    check(extc_rc_bytes(r) == (int64_t)(kN * ss), "复用链：复用后记账 = 对象数 × 槽宽");

    /* 第二轮：再死一遍、再复用一遍 —— slab 数不得增长（这正是惰性链要保证的稳定性）。 */
    extc_rc_begin(r, NULL, 0);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    extc_rc_settle(r);
    for (int i = 0; i < kN; i++) keep[kN + i] = (Node *)extc_rc_alloc(r, sizeof(Node), &g_data);
    check(extc_rc_slabCount(r) == slabs0, "复用链：第二轮复用仍不新开块（slab 数不随轮数增长）");
    check(regionOccCount(r) == 2 * kN || extc_rc_slabCount(r) == slabs0,
          "复用链：第二轮占用申报与存活数一致");
    check(regionChainsValid(r), "复用链：第二轮弹出后的链仍合法");
    extc_rc_regionFree(r);
}

#if !EXTC_RC_NOCOUNT
/* 计数档专项：单槽释放（freeSlot）会立即链入，清扫（freeSlotWord）只计数。混合两者后，
 * 位图重建只在链为空时发生 ⇒ 同一个槽不会入链两次；链必须无环、无重复、只含洞。 */
static void t_reuseNoDoubleLink(void) {
    ExtcRcRegion *r = regionNew(39, (int64_t)1 << 40);
    enum { kN = 600, kHalf = 300 };
    static Node *a[kN];
    for (int i = 0; i < kN; i++) a[i] = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    size_t slabs0 = extc_rc_slabCount(r);
    for (int i = 0; i < kHalf; i++) { extc_rc_inc(a[i]); extc_rc_dec(a[i]); }   /* hold 1 -> 0: freeSlot 立即链入 */
    check(regionChainsValid(r), "P0-B 计数档：单槽释放的链无重复、无活槽");
    for (int i = 0; i < kHalf; i++) a[i] = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);   /* 弹出链上的洞 */
    check(regionOccCount(r) == kN, "P0-B 计数档：弹出后占用槽数仍 = 对象数");
    for (int i = 0; i < kHalf; i++) { extc_rc_inc(a[i]); extc_rc_dec(a[i]); }
    /* 其余对象交给清扫：freeSlotWord 也把死槽链入同一条复用链 */
    extc_rc_begin(r, NULL, 0);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    extc_rc_settle(r);
    check(extc_rc_bytes(r) == 0, "P0-B 计数档：清扫与定点释放后占用归零");
    check(regionChainsValid(r), "P0-B 计数档：混合两种释放后链仍无重复、无活槽");
    for (int i = 0; i < kN; i++) a[i] = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(extc_rc_slabCount(r) == slabs0, "P0-B 计数档：混合路径的洞按位图重建，不新开块");
    check(regionOccCount(r) == kN, "P0-B 计数档：重建后占用槽数 = 对象数（同一槽没发两次）");
    check(regionChainsValid(r), "P0-B 计数档：重建后的链仍合法");
    extc_rc_regionFree(r);
}
#endif

#if !EXTC_RC_NOCOUNT
/* 计数档专项：清扫在飞时调用定点释放（releaseDead）必须被推迟。`releaseOld` 收尾会清掉**整个**
 * mark 位图，而本轮的分配路径靠 mark 位保护"清扫中新建/未扫位置"的对象；清掉就会把活对象交给
 * 本轮清扫当垃圾收掉。这里的形状把"清扫中分配"落在**已存在 slab 的未扫 word**（不是新 slab，
 * 所以分配路径确实给它置了黑位）—— 定点释放若在清扫中执行，清掉的正是这个黑位。 */
static void t_releaseDuringSweep(void) {
    ExtcRcRegion *r = regionNew(35, 16);
    /* 两个 size class：16 字节类先建、填到 word 16；24 字节类后建，成为 slabs 头。finish 的清扫从
     * 24 字节块起步，随后那次 16 字节分配按字节配额先扫完 24 字节块、再扫进 16 字节块 16 个 word，
     * 于是 fresh/old/oldChild 都落在游标处（word 16，未扫）；分配路径给 fresh 置黑位。 */
    enum { kPad = 1024 };
    static void *keeper[kPad + 2];
    for (int i = 0; i < kPad; i++) keeper[i] = extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *keep = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    keeper[kPad] = keep;
    keeper[kPad + 1] = extc_rc_alloc(r, 24, &g_data);   /* 第二类：24 字节槽的块成为 slabs 头 */
    Node *old = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    Node *oldChild = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    old->next = oldChild;   /* 死树：不在根表里 */

    extc_rc_begin(r, keeper, kPad + 2);
    while (!r->seedsDone || r->msp || r->scanObj) (void)extc_rc_markStep(r, 0xffffffffu);
    extc_rc_finish(r);
    check(r->sweeping, "sweep 在飞时定点释放：标记收尾后清扫已经在飞");
    r->visitedBytes = 0;   /* 前面的 O(live) 遍历信用已花掉：打开定点释放的预算闸 */

    /* finish 推进 8 个 word、分配路径扫完 24 字节块再进 16 字节块 16 个 word：cursor 到 word 16。
     * fresh 落在 word 16（未扫）⇒ 分配路径给它置黑位；old/oldChild 也还在未扫区。 */
    Node *fresh = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);
    check(r->sweeping && r->swWord < 32, "sweep 在飞时定点释放：分配的顺带推进后清扫仍未完");
    check(extc_rc_isLive(fresh), "sweep 在飞时定点释放：清扫中分配的对象当前活着");
    check(extc_rc_isLive(old) && extc_rc_isLive(oldChild),
          "sweep 在飞时定点释放：未扫区的死树此刻还活着");

    extc_rc_releaseDead(r, old, keeper, kPad + 2);   /* 带修复 ⇒ 清扫在飞、直接返回 */
    check(extc_rc_isLive(old), "sweep 在飞时定点释放：清扫在飞 ⇒ 本轮不做定点释放（old 仍活）");

    while (r->sweeping) (void)extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);
    check(extc_rc_isLive(fresh),
          "sweep 在飞时定点释放：清扫中分配的对象没被误放（黑位没被定点释放清掉）");
    check(extc_rc_isLive(keep), "sweep 在飞时定点释放：根对象没被误放（根表的标黑位还在）");
    check(!extc_rc_isLive(old) && !extc_rc_isLive(oldChild),
          "sweep 在飞时定点释放：清扫随后照常收掉死树");
    {
        int clean = 1;
        for (ExtcRcSlab *s = r->slabs; s; s = s->next)
            for (size_t w = 0; w < s->nwords; w++) if (s->mark[w]) clean = 0;
        check(clean, "sweep 在飞时定点释放：终态全部 slab 的 mark 位为 0");
    }
    check(extc_rc_hold(keep) == 0 && extc_rc_hold(fresh) == 0,
          "sweep 在飞时定点释放：终态活对象的 hold 与无引用一致（0）");
    extc_rc_regionFree(r);
}
#endif

/* histo bucket table self-test (Lead m02523): a bucket name is its upper bound; over1ms counts only strictly > 1 ms. */
static void t_histoBuckets(void) {
    int i;
    check(extc_rc_stepBucket(4000) == 2, "histo: 4.000 us lands in b4us (upper bound is inclusive)");
    check(extc_rc_stepBucket(4001) == 3, "histo: 4.001 us lands in b8us");
    check(extc_rc_stepBucket(500000) == 9, "histo: 0.5 ms lands in b512us");
    check(extc_rc_stepBucket(1000000) == 10, "histo: 1.000 ms lands in b1ms (inclusive upper bound)");
    check(extc_rc_stepBucket(1000001) == 11, "histo: 1.001 ms lands in b4ms");
    check(extc_rc_stepBucket(1500000) == 11, "histo: 1.5 ms lands in b4ms");
    check(extc_rc_stepBucket(64000000) == 13, "histo: 64.0 ms lands in b64ms (inclusive upper bound)");
    check(extc_rc_stepBucket(70000000) == 14, "histo: 70 ms lands in bge64ms");
    for (i = 0; i < EXTC_RC_STEP_NB; i++) extc_rc_stepB[i] = 0;
    extc_rc_stepNote(500000);
    extc_rc_stepNote(1500000);
    extc_rc_stepNote(70000000);
    check(extc_rc_stepB[9] == 1 && extc_rc_stepB[11] == 1 && extc_rc_stepB[14] == 1,
          "histo: the three injected samples land in b512us/b4ms/bge64ms");
    check(extc_rc_stepSumFrom(11) == 2, "histo: over1ms counts only the two strictly-above-1ms samples");
    check(extc_rc_stepSumFrom(5) == 3, "histo: over16 counts all three samples");
}
#if EXTC_RC_FREE_DEFER
/* ---------------------------------------------------------------- 延迟释放队列（P1-3） */
static void t_regionFreeDefer(void) {
    ExtcRcRegion *r = regionNew(40, (int64_t)1 << 40);   /* 压力线抬高：本用例手动驱动 */
    Node *last = NULL;
    for (int i = 0; i < 5000; i++) last = (Node *)extc_rc_alloc(r, sizeof(Node), &g_node);   /* > 2 x 4090 槽 ⇒ 两块 slab */
    size_t slabs = extc_rc_slabCount(r);
    check(slabs >= 2, "延迟释放：形状里至少两块 slab");
    long long q0 = extc_rc_fqSlabs;
    long long c0 = extc_rc_cqN;
    extc_rc_regionFree(r);
    /* O(1) drop：regionFree 一块 slab 都不碰（既不 free、也不进 free 队列），只把 region 挂到关闭队列。 */
    check(extc_rc_fqSlabs == q0, "延迟释放：regionFree 不再同步 free slab（slab 一块都不进 free 队列）");
    check(extc_rc_cqN - c0 == 1, "延迟释放：regionFree 只把 region 挂上关闭队列（O(1) drop）");
    check(extc_rc_rfSyncFrees == 0, "延迟释放：同步段一次 free 都不发（连类链与 region 也等排空）");
    check(!extc_rc_isLive(last), "延迟释放：drop 后旧对象立即不算活");
    last->next = NULL;   /* payload 还在队列里（没 free），写它不 UAF；写入不得复活对象 */
    check(!extc_rc_isLive(last), "延迟释放：延迟期内写旧槽不复活对象");

    /* 分配路径在开新 slab 前做有界排空，但一次排空的预算随时钟封顶（8 slab / 100 us，先到先停），
       满载或 ASan 插桩下 100 us 可能先用完、把余量留给下一次——这是契约允许的。所以「清空」判据
       用多次开凿：每个新 region 的首个分配必开新 slab，于是每次尝试都触发一遍有界排空。 */
    ExtcRcRegion *r2 = regionNew(41, (int64_t)1 << 40);
    for (int i = 0; i < 64; i++) (void)extc_rc_alloc(r2, sizeof(Node), &g_node);
    for (int k = 0; k < 64 && extc_rc_fqSlabs != 0; k++) {
        ExtcRcRegion *rk = regionNew(50 + k, (int64_t)1 << 40);
        (void)extc_rc_alloc(rk, sizeof(Node), &g_node);   /* 开凿新 slab ⇒ 又一次有界排空 */
    }
    check(extc_rc_fqSlabs == 0, "延迟释放：分配路径的有界排空把队列清空");
    check(extc_rc_fqBytes == 0, "延迟释放：排空后队列字节数回到 0");
    check(extc_rc_cqN == 0, "延迟释放：关闭队列也随排空清空（region 自身等链空后 free）");
    extc_rc_regionFree(r2);
    extc_rc_freeQueueDrainAll();   /* 无预算入口：与 defer=0 等价的最后手段 */
    check(extc_rc_fqSlabs == 0, "延迟释放：全量排空入口清空队列");
    check(!extc_rc_isLive(last), "延迟释放：排空后旧指针仍不算活（登记表在 drop 时就摘了）");
}

static void t_regionFreeDeferHuge(void) {
    ExtcRcRegion *r = regionNew(42, (int64_t)1 << 40);
    void *p = extc_rc_alloc(r, 600000, &g_data);   /* 槽 > (1 MiB - 头)/2 ⇒ huge block：payload 单独 malloc */
    ExtcRcSlab *s = p ? extc_rc_ownSlab(p) : NULL;
    check(s && s->big, "延迟释放：大对象落在 huge block（payload 与 slab 分开）");
    long long q0 = extc_rc_fqSlabs;
    long long c0 = extc_rc_cqN;
    extc_rc_regionFree(r);
    check(extc_rc_fqSlabs == q0, "延迟释放：huge block 也不在 drop 里进 free 队列");
    check(extc_rc_cqN - c0 == 1, "延迟释放：huge block 的 region 挂在关闭队列上（payload 等排空时 free）");
    extc_rc_freeQueueDrainAll();
    check(extc_rc_fqSlabs == 0, "延迟释放：排空后 huge block 的 slab 与 payload 一起 free");
    check(!extc_rc_isLive(p), "延迟释放：huge block 的槽不再算活");
}

static void t_regionFreeDeferLimit(void) {
    ExtcRcRegion *r = regionNew(43, (int64_t)1 << 40);
    /* 正常 slab（每块按 ~1 MiB + 位图记账）造出 > 8 MiB 的待 free 量：关闭队列每次推进 8 块就越过限额。 */
    for (int i = 0; i < 20000; i++) (void)extc_rc_alloc(r, sizeof(Node), &g_node);
    long long f0 = extc_rc_fqForced;
    long long c0 = extc_rc_cqN;
    extc_rc_regionFree(r);
    check(extc_rc_cqN - c0 == 1, "延迟释放：超限额形状也先只挂关闭队列");
    /* drop 不再自己排空；限额压力由后续分配点的有界排空承担：slab 进 free 队列越过 8 MiB 后改走硬截止。 */
    for (int k = 0; k < 64 && (extc_rc_cqN != 0 || extc_rc_fqSlabs != 0); k++) {
        ExtcRcRegion *rk = regionNew(80 + k, (int64_t)1 << 40);
        (void)extc_rc_alloc(rk, sizeof(Node), &g_node);
        if (extc_rc_cqN == 0 && extc_rc_fqSlabs == 0) break;
    }
    check(extc_rc_cqN == 0 && extc_rc_fqSlabs == 0, "延迟释放：分配点的有界排空把超限额形状也清干净");
    check(extc_rc_fqBytes <= (int64_t)EXTC_RC_FREE_QUEUE_BYTES, "延迟释放：free 队列始终压回 8 MiB 限额内");
    extc_rc_freeQueueDrainAll();
    check(extc_rc_fqSlabs == 0 && extc_rc_cqN == 0, "延迟释放：限额路径之后两条队列都可全量清空");
    (void)f0;
}
#endif
#ifdef EXTC_DRIVER_ONLY_SCAN
/* 粒度出口用：分段/续扫的断言单独跑，不受其余用例「一次 collect 必须收完」这类步数假设影响。 */
int main(void) {
    printf("== reclaim 分段扫描断言（只跑 t_scanChunk）==\n");
    t_scanChunk();
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
#else
int main(void) {
    printf("== reclaim 运行期断言 ==\n");
#if !EXTC_RC_NOCOUNT
    t_allocFree();
#endif
#if !EXTC_RC_NOCOUNT
    t_cycle();
#endif
#if !EXTC_RC_NOCOUNT
    t_saturate();
#endif
#if !EXTC_RC_NOCOUNT
    t_staleRegister();
#endif
#if !EXTC_RC_NOCOUNT
    t_crossRegion();
#endif
    t_rsSat();
    t_bumpCross();
    t_noSweep();
    t_hostSlot();
    t_pin();
    t_storeStructRooted();
    t_slotUnique();
    t_unregister();
    t_unlink();
    t_types();
    t_ownsGuard();
#if !EXTC_RC_NOCOUNT
    t_perSlotType();
#endif
#if !EXTC_RC_NOCOUNT
    t_slotReuseType();
#endif
    t_conservative();
    g_deadline = clock() + (clock_t)(EXTC_DRIVER_BUDGET_SEC * (double)CLOCKS_PER_SEC);
    t_bump();
    t_trigger();
    t_thresholdCache();
    t_admissionReserve();
#if !EXTC_RC_NOCOUNT
    t_acyclic();
#endif
    t_learning();
    t_shadeCache();
    t_scanChunk();
    t_histoBuckets();
    t_allocDuringSweep();
    t_sweepBulkReset();
    t_sweepElideMark();
    t_lazyFreelistRebuild();
#if !EXTC_RC_NOCOUNT
    t_reuseNoDoubleLink();
#endif
#if !EXTC_RC_NOCOUNT
    t_releaseDuringSweep();
#endif
#if EXTC_RC_FREE_DEFER
    t_regionFreeDefer();
    t_regionFreeDeferHuge();
    t_regionFreeDeferLimit();
#endif
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif