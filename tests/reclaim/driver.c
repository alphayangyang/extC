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

/* ---------------------------------------------------------------- 义务 7：种子不完整就退出清扫 */
static void t_noSweep(void) {
    /* 宿主证不出来的跨区赋值（`f()->f = v`、视图元素）没有可信的种子位置。这时**唯一安全的方向**
     * 是让目标所在区域整个退出清扫（只靠计数回收）—— 扫一个种子不完整的区域就可能把还被指着的
     * 对象放掉。这条不变量此前**只有形状级判据间接守着，单测里一条都没有**。 */
    ExtcRcRegion *ra = regionNew(13, 4096);
    ExtcRcRegion *rb = regionNew(14, 4096);
    Pair *a = (Pair *)extc_rc_alloc(ra, sizeof(Pair), &g_pair);
    Pair *b = (Pair *)extc_rc_alloc(rb, sizeof(Pair), &g_pair);
    void *roots[1] = { a };
    extc_rc_owe(ra);
    check(extc_rc_checkOwed(ra, roots, 1) == 0, "义务 7：种子完整时照常销账（回收 0 个）");
    check(extc_rc_isLive(a), "义务 7：有根，目标活着");

    extc_rc_storeRefUnknown((void **)&b->a, a);      /* 宿主证不出来 ⇒ 只能保守 */
    extc_rc_owe(ra);
    check(extc_rc_checkOwed(ra, roots, 1) == -1, "义务 7：之后目标区域一个字节都不扫（退出清扫）");
    check(extc_rc_isLive(a), "义务 7：保守之后目标仍然活着（宁可少收）");

    void *rootsb[1] = { b };
    extc_rc_owe(rb);
    check(extc_rc_checkOwed(rb, rootsb, 1) == 0, "义务 7：宿主区域不受影响，照常清扫");
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

    /* ③ scale 翻倍：一次一无所获的清扫把倍数 ×2，阈值跟着 ×2。 */
    r = regionNew(13, 4096);
    void *all[300];
    for (int i = 0; i < 300; i++) all[i] = extc_rc_alloc(r, sizeof(Pair), &g_pair);
    int freed = extc_rc_maybeCollect(r, all, 300);        /* 全是根 ⇒ 一个也收不掉 */
    check(freed == 0, "阈值缓存③：一无所获的清扫收到了 0");
    check(extc_rc_scale(r) == 2, "自学习：一无所获把倍数翻到 2");
    check(!extc_rc_shouldSweep(r), "阈值缓存③：倍数翻倍后阈值 ×2，当下不触发");
    extc_rc_regionFree(r);
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
    for (long i = 0; i < n; i++) {
        if ((i & 4095) == 0) budget("自学习负载：预算内没跑完（触发/回收语义坏了？）");
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
}

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
#if !EXTC_RC_NOCOUNT
    t_acyclic();
#endif
    t_learning();
    t_shadeCache();
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
