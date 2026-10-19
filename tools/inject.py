#!/usr/bin/env python3
# tools/inject.py —— **注入验牙**：证明一条判据在"它该抓的错"下真的会红，并保证还原。
#
# 为什么要有它：这一路下来，"判据能不能红"是靠手工做的（改源码 → 重编 → 跑判据 → 改回来）。
# 手工做法有两个问题：
#   1. **忘记还原**：注入留在工作树里，后面的结论全建立在被改过的编译器上；
#   2. 每次都要重新想"该改哪一处、该跑哪条判据"，于是容易只跑一条宽松的判据就算数。
#
# 这个工具把注入写成**声明**（哪一处、期望哪条判据红、为什么），执行时：
#   备份原文件（字节级）→ 打补丁 → 重编 → 跑判据 → **必须红**（否则工具失败）→
#   还原 → 重编 → 再跑判据 → **必须绿**（否则工具失败，提示还原出了问题）。
# 任何一步异常都会走 `finally` 还原，并在最后用字节比较确认文件与备份逐字相同。
#
# 用法：
#   python3 tools/inject.py --list
#   python3 tools/inject.py thr-stale                 # 跑一条
#   python3 tools/inject.py thr-stale --gate gate-reclaim-tax   # 换判据
#   python3 tools/inject.py thr-stale --no-verify-green         # 跳过"还原后必须绿"
#   python3 tools/inject.py --check-anchors                      # 只核对锚点（不重编、不跑判据）
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 每条注入：改哪一处（字节级精确匹配）、期望哪条 check.py 判据变红、为什么。
# `patches` 里 (相对路径, 原文, 替换文)；原文必须**恰好出现一次**。
SRC = "src/back/reclaim.c"
CODEGEN = "src/back/codegen.c"
PLAN = "src/plan/plan.c"


INJECTIONS = {
    "shade-noinval": {
        "why": "拿掉收窄清扫前的代际自增 ⇒ 那片 mark 被清之后，着色缓存还把对象记成黑的，"
               "屏障整段跳过、可达对象漏标",
        "gate": "reclaim-unit",
        # 打的是 releaseOld（覆盖点定点释放）那条失效路径，不是 begin/sweepBegin 的；
        # 后两条各有代际++ 兜底，只有这条是承重的（去掉它探针就漏标，见 changelog）。
        "patches": [(SRC, '    "    extc_rc_shadeNextGen();   /* 收窄清扫在下面清整片 mark：黑记录作废 */\\n",\n', "")],
    },
    "thr-stale": {
        "why": "拿掉 `collect` 末尾的 `thrUpd` ⇒ 压力阈值过期，触发语义坏掉",
        "gate": "reclaim-unit",
        "patches": [(SRC, '    "    r->live = r->markedBytes > 0 ? r->markedBytes : r->live;\\n",\n    "    extc_rc_thrUpd(r);\\n",\n', "")],
    },
    "rsinc-drop": {
        "why": "拿掉跨区边的登记（`rsInc`）⇒ 目标区域的清扫会放掉还被指着的对象",
        # **两处都要拆**：`storeRefOwned` 与第十二刀的 `storeRefOwnedReg` 各有一份登记逻辑；
        # 只拆前者时形状走的是后者 ⇒ 打不到活路径。
        "gate": "gate-reclaim-switch",
        "patches": [(SRC, '    "            if (host && sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }\\n",\n', ""), (SRC, '    "                if (sv && sv->region != r) { extc_rc_rsIncSlab(sv, v); r->foreign = 1; }\\n",\n    "                if (r->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : (ExtcRcSlab *)0; if (sold && sold->region != r) extc_rc_rsDecSlab(sold, old); }\\n",\n', "")],
    },
    "localstore-on-objhost": {
        "why": "把对象宿主（`hreg != NULL`）的引用存储也改发本地槽的只写入口 "
               "`extc_rc_storeRefLocal` ⇒ 跨区登记整段丢失（该入口只允许用在 `owner == 0` 的槽上）",
        "gate": "gate-reclaim-switch",
        # 打的是**发射点**（codegen 里 `hreg != NULL` 那条分支），不是运行期文本：
        # 只改 `storeRefOwned` 的兜底分支打不到热路径（那条分支的宿主 `hreg == NULL`）。
        "patches": [("src/back/codegen.c",
                     '                const char *hreg = rcFreshHostRegion(g, hostBase);\n                if (hreg) {\n',
                     '                const char *hreg = rcFreshHostRegion(g, hostBase);\n'
                     '                if (hreg && g->rcLocalStore) {   /* 注入 localstore-on-objhost */\n'
                     '                    cgLine(g, "extc_rc_storeRefLocal((void **)&(%s), (void *)(%s));", tgt, val);\n'
                     '                    return;\n'
                     '                }\n'
                     '                if (hreg) {\n')],
    },
    "sweep-never": {
        "why": "让 `shouldSweep` 永不触发 ⇒ 垃圾只等区域丢弃，峰值随轮数涨（**内存类**失效）",
        "gate": "gate-reclaim-switch",
        "patches": [(SRC, '    "    return r->bytes > r->thr && extc_rc_roundOk(r);   /* 起轮信用：独立计数器，定点释放不消耗 */\\n",\n', '    "    return 0;   /* 注入：永不触发 */\\n",\n')],
    },
    "overregister": {
        "why": "宿主证不出时也登记目标（第 18 轮那个真回归）⇒ 登记只增不减、峰值随轮数涨",
        "gate": "gate-reclaim-switch",
        # 两个变体各插一处（只插 `storeRefOwned` 打不到活路径 —— 与 `rsinc-drop` 同一条教训）。
        "patches": [
            (SRC, '    "            if (host && sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }\\n",\n    "            if (host && host->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : (ExtcRcSlab *)0; if (sold && sold->region != host) extc_rc_rsDecSlab(sold, old); }\\n",\n',
             '    "            if (host && sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }\\n",\n    "            if (sv) extc_rc_rsIncSlab(sv, v);   /* 注入：过度登记（第 18 轮那个回归） */\\n",\n    "            if (host && host->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : (ExtcRcSlab *)0; if (sold && sold->region != host) extc_rc_rsDecSlab(sold, old); }\\n",\n'),
            (SRC, '    "                if (sv && sv->region != r) { extc_rc_rsIncSlab(sv, v); r->foreign = 1; }\\n",\n    "                if (r->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : (ExtcRcSlab *)0; if (sold && sold->region != r) extc_rc_rsDecSlab(sold, old); }\\n",\n',
             '    "                if (sv && sv->region != r) { extc_rc_rsIncSlab(sv, v); r->foreign = 1; }\\n",\n    "                if (sv) extc_rc_rsIncSlab(sv, v);   /* 注入：过度登记（第 18 轮那个回归） */\\n",\n    "                if (r->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : (ExtcRcSlab *)0; if (sold && sold->region != r) extc_rc_rsDecSlab(sold, old); }\\n",\n'),
        ],
    },
    "perf-lookup": {
        "why": "让 `rcRegionRef` 恒返回 NULL ⇒ 触发/分配退回每次内联 `regionOf`（第 13 刀那笔优化没了）",
        # 原先挂在 `gate-reclaim-tax`：`bt` 那时还发射 rc，去掉区域指针复用会把税抬高。
        # 「区域不需要回收 ⇒ 整段不发射 rc」落地后 `bt` 的开/关档产物逐字节相同（税恒 1.000×）
        # ⇒ 这条注入在税判据上失去牙，改挂指令数棘轮（同一形状的回收开销被付回来）。
        "gate": "gate-reclaim-perf",
        "patches": [(CODEGEN, 'static const char *rcRegionRef(CG *g, const char *basePtr) {\n', 'static const char *rcRegionRef(CG *g, const char *basePtr) {\n    return NULL;   /* 注入：不复用区域指针 */')],
    },
    "count-noinc": {
        "why": "计数模式下持有者计数不再上升 ⇒ 最后一个持有者一放手就当场回收，悬垂",
        # **只**打在 `EXTC_RC_NOCOUNT=0` 那一支（两处：`holdUpdSlab` 与 `holdUpd`）；
        # 默认（无计数）档应当**完全不受影响** —— 这正是这两条计数档门禁存在的意义。
        "gate": "gate-reclaim-count-unit",
        "patches": [
            (SRC, '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i]++;\\n",\n',
             '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i] += 0;\\n",   /* 注入：计数不涨 */\n'),
            (SRC, '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i]++;   /* 义务 3：到顶粘住 */\\n",\n',
             '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i] += 0;   /* 义务 3：到顶粘住 */\\n",   /* 注入：计数不涨 */\n'),
        ],
    },
    "count-nosat": {
        "why": "计数模式下拿掉「到顶粘住」的守卫 ⇒ 计数溢出回绕到 0，最后一次放手就误回收",
        "gate": "gate-reclaim-count-unit",
        "patches": [
            (SRC, '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i]++;\\n",\n',
             '    "        s->hold[i]++;   /* 注入：不再粘住 */\\n",\n'),
            (SRC, '    "        if (s->hold[i] != EXTC_RC_HOLD_SAT) s->hold[i]++;   /* 义务 3：到顶粘住 */\\n",\n',
             '    "        s->hold[i]++;   /* 注入：不再粘住 */   /* 义务 3：到顶粘住 */\\n",\n'),
        ],
    },
    "count-nodec": {
        "why": "计数模式下持有者计数只增不减 ⇒ 归零当场回收永不发生，内存随轮数涨（**泄漏**）",
        "gate": "gate-reclaim-count-unit",
        "patches": [
            (SRC, '    "    s->hold[i]--;\\n",\n    "    if (s->hold[i] == 0) extc_rc_freeSlot(s, i);\\n",\n',
             '    "    s->hold[i] += 0;\\n",\n    "    if (s->hold[i] == 0) extc_rc_freeSlot(s, i);\\n",\n'),
            (SRC, '    "    s->hold[i]--;\\n",\n    "    if (s->hold[i] == 0) extc_rc_freeSlot(s, i);            /* 归零当场回收 */\\n",\n',
             '    "    s->hold[i] += 0;\\n",\n    "    if (s->hold[i] == 0) extc_rc_freeSlot(s, i);            /* 归零当场回收 */\\n",\n'),
        ],
    },
    "rsinc-nosat": {
        "why": "拿掉跨区入边计数的「到顶粘住」守卫 ⇒ 计数回绕到 0，\"还有活入边\"失效（不安全方向）",
        # 两处：`rsInc` 与 `rsIncSlab`。这条守卫与模式无关（登记在两种模式下都存在），
        # 所以默认档的单测就会红 —— 新补的 `t_rsSat` 正是为它写的。
        "gate": "reclaim-unit",
        "patches": [
            (SRC, '    "    if (s->rscnt[i] == EXTC_RC_HOLD_SAT) return;          /* 饱和粘住，永不误判归零 */   /* skip:sat-inc */\\n",\n', ""),
            (SRC, '    "    if (s->rscnt[i] == EXTC_RC_HOLD_SAT) return;   /* skip:sat-inc */\\n",\n', ""),
        ],
    },
    "realpath-noreg": {
        "why": "真实跨区赋值路径（`storeRefOwnedReg`）不登记目标 ⇒ 只走内部 API 的判据看不出来",
        # 打的正是 `storeRefOwnedReg` 的登记那一句：内部 API 阶段（直接调 `rsInc`）不受影响，
        # 所以能红的**只能**是"真实路径"那一组断言 —— 它们有没有牙，靠这条注入证明。
        "gate": "reclaim-unit",
        "patches": [(SRC, '    "            if (host && sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }\\n",\n',
                     '    "            if (host && sv && sv->region != host) { host->foreign = 1; }   /* 注入：真实赋值路径不登记 */\\n",\n')],
    },
    "noSweep-drop": {
        "why": "拿掉\"钉不住 ⇒ 目标区域退出清扫\"（义务 7 的最后兜底）⇒ 种子不完整的区域照样被扫（不安全方向）",
        # 两处：`storeRefUnknown` 的钉不住回落（OOM 兜底）与 `storeStructCons` 的保守分支。
        "gate": "reclaim-unit",
        "patches": [
            (SRC, '    "        if (r) r->noSweep = 1;\\n",\n',
             '    "        if (r) { /* 注入：不退出清扫 */ }\\n",\n'),
            (SRC, '    "        else if (rv) rv->noSweep = 1;\\n",\n',
             '    "        else if (rv) { /* 注入：不退出清扫 */ }\\n",\n'),
        ],
    },
    "freelist-stuck": {
        "why": "空闲链弹出后不前进 ⇒ 同一个槽被反复发出（两个活对象同址）。**ASan 抓不到这种别名**"
               "（内存是合法的）⇒ 只有'槽唯一'那条判据能抓",
        "gate": "reclaim-unit",
        "patches": [(SRC, '    "        s->freelist = (int64_t)s->tid[i] - 1;  /* 0 = 链尾；载荷不参与复用链（见 extc_rc_freeSlot） */\\n",\n',
                     '    "        s->freelist = (int64_t)i;   /* 注入：空闲链不前进（同一个槽反复发出） */\\n",\n')],
    },
    "register-noop": {
        "why": "`extc_rc_register` 不再置入边位 ⇒ 目标区域清扫时**种子少一个**，还被指着的对象会被放掉",
        # 默认档（`EXTC_RC_NOCOUNT=1`）里没有任何断言验「register 真的置了位」
        # （t_staleRegister/t_crossRegion 都在 `#if !EXTC_RC_NOCOUNT` 里）⇒ 只有计数档单测抓得住。
        "gate": "gate-reclaim-count-unit",
        "patches": [(SRC, '    "    if (s->rsbit[extc_rc_word(i)] & extc_rc_bit(i)) return;   /* skip:reg-dup */\\n",\n    "    s->rsbit[extc_rc_word(i)] |= extc_rc_bit(i); s->region->seedAny = 1;\\n",\n',
                     '    "    if (s->rsbit[extc_rc_word(i)] & extc_rc_bit(i)) return;   /* skip:reg-dup */\\n",\n    /* 注入：不置入边位 */\n')],
    },
    "rsdec-nop": {
        "why": "跨区计数不再减、登记位永不清 ⇒ 留下陈旧入边位（保守方向：目标被多留）",
        "gate": "reclaim-unit",
        "patches": [
            (SRC, '    "__attribute__((unused)) static void extc_rc_rsDec(void *t) {\\n",\n    "    ExtcRcSlab *s = extc_rc_ownSlab(t);\\n",\n    "    if (!s) return;   /* skip:not-own */\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    "    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) return;   /* skip:occ-free */\\n",\n    "    if (!s->rscnt || s->rscnt[i] == 0 || s->rscnt[i] == EXTC_RC_HOLD_SAT) return;   /* skip:sat-dec */\\n",\n    "    if (--s->rscnt[i] == 0) s->rsbit[extc_rc_word(i)] &= ~extc_rc_bit(i);\\n",\n',
             '    "__attribute__((unused)) static void extc_rc_rsDec(void *t) {\\n",\n    "    ExtcRcSlab *s = extc_rc_ownSlab(t);\\n",\n    "    if (!s) return;   /* skip:not-own */\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    "    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) return;   /* skip:occ-free */\\n",\n    "    if (!s->rscnt || s->rscnt[i] == 0 || s->rscnt[i] == EXTC_RC_HOLD_SAT) return;   /* skip:sat-dec */\\n",\n    "    if ((s->rscnt[i] += 0) == 0) s->rsbit[extc_rc_word(i)] &= ~extc_rc_bit(i);   /* 注入：计数不减，登记位永不清 */\\n",\n'),
            (SRC, '    "__attribute__((unused)) static void extc_rc_rsDecSlab(ExtcRcSlab *s, void *t) {\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    "    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) return;   /* skip:occ-free */\\n",\n    "    if (!s->rscnt || s->rscnt[i] == 0 || s->rscnt[i] == EXTC_RC_HOLD_SAT) return;   /* skip:sat-dec */\\n",\n    "    if (--s->rscnt[i] == 0) s->rsbit[extc_rc_word(i)] &= ~extc_rc_bit(i);\\n",\n',
             '    "__attribute__((unused)) static void extc_rc_rsDecSlab(ExtcRcSlab *s, void *t) {\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    "    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) return;   /* skip:occ-free */\\n",\n    "    if (!s->rscnt || s->rscnt[i] == 0 || s->rscnt[i] == EXTC_RC_HOLD_SAT) return;   /* skip:sat-dec */\\n",\n    "    if ((s->rscnt[i] += 0) == 0) s->rsbit[extc_rc_word(i)] &= ~extc_rc_bit(i);   /* 注入：计数不减，登记位永不清 */\\n",\n'),
        ],
    },
    "mark-nodesc": {
        "why": "标记时不标孩子（描述符字段读了却不推）⇒ 孩子少标、被提前回收（**不安全方向**）",
        "gate": "reclaim-unit",
        "patches": [(SRC, '    "        const uint32_t *_op = t->off; const uint32_t *_oend = _op + t->nptr;   /* 义务 4：按对象自己的描述符走 */ \\\\\\n",\n    "        for (; _op < _oend; ++_op) { \\\\\\n",\n    "            void *child = *(void **)((char *)o + *_op); \\\\\\n",\n    "        if (child) { \\\\\\n",\n    "            uintptr_t _cb = (uintptr_t)child & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1); \\\\\\n",\n    "            if (_cb == (uintptr_t)s) {   /* 同块：按「包含该地址的槽」的去重位跳 */ \\\\\\n",\n    "                size_t _off = extc_rc_offOf(s, child); \\\\\\n",\n    "                size_t _ci = extc_rc_slotOfOff(s, _off); \\\\\\n",\n    "                if (_ci < s->nslots) { \\\\\\n",\n    "                    uint64_t _cbit = extc_rc_bit(_ci); \\\\\\n",\n    "                    size_t _cw = extc_rc_word(_ci);   /* 去重只看 mark 位 */ \\\\\\n",\n    "                    if (s->mark[_cw] & _cbit) continue;   /* 已经黑过：弹栈端也会被同一道位挡下 */ \\\\\\n",\n    "                } \\\\\\n",\n    "            } \\\\\\n",\n    "            extc_rc_markPushFast(r, child); \\\\\\n",\n',
                          '    "        const uint32_t *_op = t->off; const uint32_t *_oend = _op + t->nptr;   /* 义务 4：按对象自己的描述符走 */ \\\\\\n",\n    "        for (; _op < _oend; ++_op) { \\\\\\n",\n    "            void *child = *(void **)((char *)o + *_op); \\\\\\n",\n    "        if (child) { \\\\\\n",\n    "            uintptr_t _cb = (uintptr_t)child & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1); \\\\\\n",\n    "            if (_cb == (uintptr_t)s) {   /* 同块：按「包含该地址的槽」的去重位跳 */ \\\\\\n",\n    "                size_t _off = extc_rc_offOf(s, child); \\\\\\n",\n    "                size_t _ci = extc_rc_slotOfOff(s, _off); \\\\\\n",\n    "                if (_ci < s->nslots) { \\\\\\n",\n    "                    uint64_t _cbit = extc_rc_bit(_ci); \\\\\\n",\n    "                    size_t _cw = extc_rc_word(_ci);   /* 去重只看 mark 位 */ \\\\\\n",\n    "                    if (s->mark[_cw] & _cbit) continue;   /* 已经黑过：弹栈端也会被同一道位挡下 */ \\\\\\n",\n    "                } \\\\\\n",\n    "            } \\\\\\n",\n    "            (void)child;   /* 注入：不标孩子 */ \\\\\\n",\n')],
    },
    "unlink-nop": {
        "why": "对象被回收时不撤它手里的跨区边 ⇒ 目标槽留下陈旧入边登记（保守方向：目标被多留）",
        "gate": "reclaim-unit",
        "patches": [
            (SRC, '    "            if (c) { ExtcRcSlab *cs = extc_rc_ownSlab(c); if (cs && cs->region != s->region) extc_rc_rsDecSlab(cs, c); }\\n",\n',
             '    "            if (c) { ExtcRcSlab *cs = extc_rc_ownSlab(c); if (cs && cs->region != s->region) (void)cs; }\\n",\n'),
            (SRC, '    "        if (c) { ExtcRcSlab *cs = extc_rc_ownSlab(c); if (cs && cs->region != s->region) extc_rc_rsDecSlab(cs, c); }\\n",\n',
             '    "        if (c) { ExtcRcSlab *cs = extc_rc_ownSlab(c); if (cs && cs->region != s->region) (void)cs; }\\n",\n'),
        ],
    },
    "occ-guard-drop": {
        "why": "跨区登记不再检查占用位 ⇒ 空闲槽也会被置入边位（保守方向：复用后目标被多留）",
        "gate": "reclaim-unit",
        "patches": [(SRC, '    "__attribute__((unused)) static void extc_rc_rsIncSlab(ExtcRcSlab *s, void *t) {\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    "    if (!(s->occ[extc_rc_word(i)] & extc_rc_bit(i))) return;   /* skip:occ-free */\\n",\n',
                     '    "__attribute__((unused)) static void extc_rc_rsIncSlab(ExtcRcSlab *s, void *t) {\\n",\n    "    size_t i = extc_rc_slotOf(s, t);\\n",\n    /* 注入：拿掉占用位守卫 */\n')],
    },
    "ownhot-nobase": {
        "why": "把「上一块」缓存的基址判等去掉 ⇒ 跨 slab 的指针也用缓存里的 slab 判定（校验会失败 ⇒ 活对象被当区域外跳过）",
        # 语义档（少算，不安全）⇒ 单测立刻抓得到；挂 `gate-reclaim-switch` 时被污染的
        # `ownSlab` 让那份大语料中的一个形状跑不完（判据红不了，只会超时）⇒ 挂单测。
        "gate": "reclaim-unit",
        "patches": [(SRC,
                     '    "__attribute__((unused)) static inline ExtcRcSlab *extc_rc_ownSlab(void *p) {\\n",\n    "    /* 空指针不单独挡：p == 0 ⇒ base == 0，而映射格、登记表、超大块表里都不会有键 0\\n",\n    "     * （slab 基址与超大块载荷地址都不可能是 0）⇒ 每一层探测都落空、ownBig 也返回 0，\\n",\n    "     * 结果与显式检查逐字相同。省掉每次调用的一次比较（热路径上每次 2 Ir）。 */\\n",\n    "    void *base = (void *)((uintptr_t)p & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1));\\n",\n    "    ExtcRcSlab *hot = extc_rc_ownHot;\\n",\n    "    if (hot && (void *)hot == base) {          /* skip:own-hot */\\n",\n',
                     '    "__attribute__((unused)) static inline ExtcRcSlab *extc_rc_ownSlab(void *p) {\\n",\n    "    /* 空指针不单独挡：p == 0 ⇒ base == 0，而映射格、登记表、超大块表里都不会有键 0\\n",\n    "     * （slab 基址与超大块载荷地址都不可能是 0）⇒ 每一层探测都落空、ownBig 也返回 0，\\n",\n    "     * 结果与显式检查逐字相同。省掉每次调用的一次比较（热路径上每次 2 Ir）。 */\\n",\n    "    void *base = (void *)((uintptr_t)p & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1));\\n",\n    "    ExtcRcSlab *hot = extc_rc_ownHot;\\n",\n    "    if (hot) {                                 /* 注入：基址判等去掉 */\\n",\n')],
    },
    "same-region-always": {
        "why": "把「同区」判定强制为真 ⇒ 跨区赋值也走免登记快路径（漏登记 ⇒ 悬垂）",
        "gate": "gate-reclaim-switch",
        "patches": [(CODEGEN,
                     "                    if (g->rcNoCount && vlvl != -2 && vlvl == hlvl)",
                     "                    if (g->rcNoCount)   /* 注入：判定恒真 */")],
    },
    "release-off": {
        "why": "把覆盖点定点释放的发射整个关掉 ⇒ `ring_per_round` 退回区域清扫（55 次扫 + 峰值"
               " 10.6 MB）—— 这条注入证明 `tests/reclaim-gaps/gate-release.sh` 的清扫次数/峰值/"
               "释放槽数三条判据**有牙**（关掉发射，判据必须红）",
        "gate": "gate-release",
        # 打在**发射入口**的第一条守卫上（`rcEmitStoreRelease`）：条件换成恒真 ⇒ 函数立刻
        # `return false`，后面的四条判据与发射全成死代码。死代码里那些变量仍被"使用"过，
        # 所以 `-Wunused-*` 不会因为这段注入报错（注入失败 = 编译失败，与"判据红"必须分开）。
        "patches": [(CODEGEN,
                     '    if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false;\n'
                     '    /* 分片',
                     '    if (1) return false;   /* 注入：关掉定点释放的发射 */\n'
                     '    if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false;\n'
                     '    /* 分片')],
    },
    "acyclic-always": {
        "why": "把「程序无环 ⇒ 事件档（计数、不扫）」的判定强制为真 ⇒ 有环形状的**环**垃圾永远收不掉"
               "（计数永不归零 + 不扫）⇒ 峰值 RSS 爆 —— 判错的方向是内存涨，不是悬垂；"
               "这条注入证明 `tests/reclaim-acyclic/gate-acyclic.sh` 的 RSS 上界判据**有牙**",
        "gate": "gate-acyclic",
        # 打在**分析侧**的结论读出口（`src/plan/plan.c` 的 `planFnAcyclic`）：codegen 经
        # `unitAllAcyclic`（codegen.c 的接线）读它 ⇒ 打这里走的是**整条链**——默认档下
        # 有环形状也会被判成无环/事件档，②a 因此变红。替换文带 `(void)f;` 是为了在
        # `-Werror` 下不产生"参数未用"，且不依赖函数体其余部分（原文只取声明+返回两行）。
        "patches": [(PLAN,
                     '    NodeResults *r = resultsOf(f, false);\n'
                     '    return r && resultsBit(r, PLAN_FN_ACYCLIC) ? r->fnAcyclic : false;\n',
                     '    (void)f;\n'
                     '    return true;   /* 注入：无环判定强制为真（强制事件档） */\n')],
    },
    "reclaim-idle-always": {
        "why": "把「这个区域不需要回收（判据 (a)(b)(c)）」的判定强制为真 ⇒ 会丢引用的形状也"
               "**整段不发射 rc 运行期** ⇒ 垃圾只等区域丢弃、峰值随轮数涨（**内存类**失效；"
               "不是悬垂）。这条注入证明 `tests/reclaim-shapes/run-switch.sh` 的 `N vs 4N`"
               "峰值判据（以及连带的开/关档语义判据）**有牙**",
        "gate": "gate-reclaim-switch",
        # 打在**分析侧**的结论读出口（`src/plan/plan.c` 的 `planFnReclaimIdle`）：codegen 经
        # `unitReclaimIdle`（codegen.c 的接线）读它 ⇒ 打这里走的是整条链 —— 强制档
        # （`EXTC_RECLAIM=1`）下会丢引用的形状也会被判成"回收无事可做"，`run-switch.sh` 的
        # ①b（cyclic_pressure N vs 4N 峰值 < 1.5×）因此变红。替换文带 `(void)f;` 的理由与
        # `acyclic-always` 同。
        "patches": [(PLAN,
                     '    NodeResults *r = resultsOf(f, false);\n'
                     '    return r && resultsBit(r, PLAN_FN_RECLAIM_IDLE) ? r->fnReclaimIdle : false;\n',
                     '    (void)f;\n'
                     '    return true;   /* 注入：回收无事可做的判定强制为真 */\n')],
    },
    "struct-shade-nop": {
        "why": "拿掉枚举局部量整值存储的逐叶着色 ⇒ 增量标记期间写进结构拷贝的引用"
               "这一轮标不黑、被本轮清扫放掉（**少算/不安全方向**）。默认档里 `extc_rc_inc`"
               "是空操作 ⇒ 登记单测红的那两条恰好只剩着色，判据有牙",
        "gate": "reclaim-unit",
        "patches": [(SRC,
                     '    "__attribute__((unused)) static void extc_rc_storeStructRooted(void *dst, size_t size) {\\n",\n'
                     '    "    size_t words = size / sizeof(void *);\\n",\n'
                     '    "    for (size_t i = 0; i < words; i++) {\\n",\n'
                     '    "        void *nw = ((void **)dst)[i];\\n",\n'
                     '    "        extc_rc_shade(nw);   /* 结构拷贝也是写引用：逐叶着色，否则增量标记会漏 */\\n",\n',
                     '    "__attribute__((unused)) static void extc_rc_storeStructRooted(void *dst, size_t size) {\\n",\n'
                     '    "    size_t words = size / sizeof(void *);\\n",\n'
                     '    "    for (size_t i = 0; i < words; i++) {\\n",\n'
                     '    "        void *nw = ((void **)dst)[i];\\n",\n')],
    },
    "par-tls-depth-noreset": {
        "why": "拿掉 worker 调回调之前的深度清零 ⇒ threads <= 1（或起不出线程）时回调在本线程"
               "内联跑，外层帧的深度还挂在 TLS 上，入口帧被当成内层帧、只记欠收 ⇒ 那只线程本地"
               "区域整片收不掉（**内存类**失效，不是悬垂）。判据是归属结构的运行期前提",
        "gate": "gate-par-tls-owner",
        "patches": [(CODEGEN,
                     '            "        extc_rc_tlsDepth = 0;\\n");\n',
                     '            "        extc_rc_tlsDepth = extc_rc_tlsDepth;   /* 注入：不再清零深度 */\\n");\n')],
    },
    "reg-retry-nocap": {
        "why": "拿掉无锁读者的重试上限 —— 与写侧抢几十次后不再持锁兜底，而是直接返回「查不到」"
               "⇒ 并发下可能把**刚登记**的 slab 当成区域外（方向保守：只多留不漏收），但那是意外的"
               "行为差异；这层兜底是「无锁读者最终一定看到权威表」的唯一保证",
        "gate": "gate-reclaim-conc",
        "patches": [(SRC,
                     '    "        if (tries >= 32) break;   /* skip:reg-race */\\n",\n',
                     '    "        break;   /* skip:reg-race */\\n",\n')],
    },
}


def read(rel):
    return (ROOT / rel).read_text()


def write(rel, text):
    """原子替换：先写临时文件再 `os.replace`。

    为什么要这样：`copy` 模式用 `rsync --link-dest` 做**硬链接副本**（整树 3.7 GB、
    `bench/` 占 3.5 GB，真复制一次要 30 s）。硬链接下"就地截断写"会**改到原树**，
    而"写新文件再改名"只会断开这一条链接 ⇒ 原树一个字节都不动。"""
    import os
    dst = ROOT / rel
    tmp = dst.with_name(dst.name + ".inject-tmp")
    tmp.write_text(text)
    os.replace(tmp, dst)


def build():
    """(是否成功, 输出)。输出留给失败时打印 —— 注入写坏了代码（语法/未使用变量）在
    `-Werror` 下会编不过，那**不是**"判据红"，必须区分开。"""
    p = subprocess.run(["make", "-j8"], cwd=str(ROOT), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode == 0, p.stdout


def run_gate(name: str):
    p = subprocess.run(["./check.sh", "--only", name], cwd=str(ROOT),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def check_anchors() -> int:
    # 便宜的自检：锚点是**逐字**匹配源码的，发射文本一改行/一加注释锚点就漂。
    # 漂了之后 inject.py 只会在真去跑那条注入时才报错，而它不在 `check.sh` 电池里
    # ⇒ 可能一漂好几轮没人发现（这一轮就是 16 处一起漂）。检查本身不重编，毫秒级。
    bad = 0
    total = 0
    for nm, spec in INJECTIONS.items():
        for i, (rel, old, new) in enumerate(spec["patches"]):
            total += 1
            p = ROOT / rel
            if not p.exists():
                print(f"  STALE {nm}[{i}]  目标文件不存在：{rel}", file=sys.stderr)
                bad += 1
                continue
            n = p.read_text().count(old)
            if n != 1:
                print(f"  STALE {nm}[{i}]  原文在 {rel} 里出现 {n} 次（要求恰好 1 次）", file=sys.stderr)
                bad += 1
            elif old == new:
                print(f"  NOOP  {nm}[{i}]  原文与替换文相同 ⇒ 注入什么也没做（{rel}）", file=sys.stderr)
                bad += 1
    if bad:
        print(f"注入锚点自检：{bad} 处陈旧 —— 源码演进后锚点漂了，请按当前源码重锚", file=sys.stderr)
        return 2
    print(f"  ok   注入锚点自检：{len(INJECTIONS)} 条注入的 {total} 处原文各自命中恰好 1 次，且 old != new")
    return 0


def has_real_fail(out: str) -> bool:
    """门禁输出里有没有**判据本身**打出来的 FAIL。

    多形状门禁（`run-switch.sh` 那种）在注入下常有一两个形状「生成/编译失败」或
    「探针不可用」—— 那是注入把某个形状的**前提**改了（例如把 rc 运行期整段不发射，
    依赖 rc 符号的插桩工具就不可用了），不是判据没牙。只要还有一条别的 FAIL，就
    说明判据抓住了这个错。
    """
    skip = ("生成/编译失败", "编译失败", "探针不可用", "编不过")
    for line in out.split("\n"):
        if "FAIL" not in line:
            continue
        if any(s in line for s in skip):
            continue
        return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--gate")
    ap.add_argument("--no-verify-green", action="store_true")
    ap.add_argument("--tree", choices=("copy", "inplace"), default="copy",
                    help="copy：在临时副本里改/编/跑（原树一个字节都不动，默认）；"
                         "inplace：就地改（快，但 SIGKILL/断电会留下注入态）")
    ap.add_argument("--keep", action="store_true", help="保留临时副本（失败后排查用）")
    ap.add_argument("--check-anchors", action="store_true",
                    help="只核对每条注入的原文在目标文件恰好出现一次（且 old != new）；不重编、不跑判据")
    args = ap.parse_args()

    if args.check_anchors:
        return check_anchors()

    if args.list or not args.name:
        for nm, spec in INJECTIONS.items():
            print(f"  {nm:14s} 期望判据 {spec['gate']:22s} —— {spec['why']}")
        return 0
    spec = INJECTIONS.get(args.name)
    if not spec:
        print(f"没有这条注入：{args.name}", file=sys.stderr)
        return 2
    gate = args.gate or spec["gate"]

    work = ROOT
    tmp = None
    if args.tree == "copy":
        import shutil, subprocess as sp, tempfile
        base = pathlib.Path(os.environ.get("EXTC_INJECT_TMP", str(ROOT / "build")))
        base.mkdir(parents=True, exist_ok=True)
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="inject-", dir=str(base)))
        dest = tmp / "tree"
        orig = ROOT
        # 硬链接副本：除 `build/`（要重新编）之外全部 link 过去，秒级完成。
        r = sp.run(["rsync", "-a", "--exclude", "build/", "--link-dest", str(orig),
                    str(orig) + "/", str(dest)], stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
        if r.returncode != 0:
            shutil.copytree(orig, dest, symlinks=True,
                            ignore=shutil.ignore_patterns("build", "*.o"))
        work = dest
        print(f"== 硬链接副本 {work}（原树不动：写入走临时文件 + rename，只会断开链接）==")
        globals()["ROOT"] = work
        globals()["ORIG"] = orig
    done = {"yet": False}

    def cleanup():
        if done["yet"]:                 # atexit 与收尾各调一次；幂等
            return
        done["yet"] = True
        if tmp and not args.keep:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)
        elif tmp:
            print(f"   （副本保留在 {tmp}）")

    # **兜底**：`main()` 里有多条提前 return（判据没红、编译失败、原文找不到…）；
    # 不兜底就会把整棵副本树留在磁盘上（几 GB，而 `/tmp` 常是几 GB 的 tmpfs ⇒ 一次 ENOSPC）。
    import atexit
    atexit.register(cleanup)

    backup = {}
    for rel, old, _new in spec["patches"]:
        text = read(rel)
        if text.count(old) != 1:
            print(f"注入原文在 {rel} 里出现 {text.count(old)} 次（要求恰好 1 次）—— 判据/源码漂了？",
                  file=sys.stderr)
            return 2
        backup[rel] = text
    print(f"== 注入 {args.name}：{spec['why']}（期望 {gate} 红）==")
    status = {"rc": 0, "note": ""}
    try:
        for rel, old, new in spec["patches"]:
            write(rel, read(rel).replace(old, new, 1))
            print(f"   打补丁 {rel}")
        okb, bout = build()
        if not okb:
            status["rc"] = 1
            status["note"] = ("注入把代码写坏了（编不过）—— 这**不算**判据红，请换一种改法：\n"
                              + "\n".join(l for l in bout.split("\n") if "error" in l)[:600])
        else:
            rc, out = run_gate(gate)
            if rc == 0:
                status["rc"] = 1
                status["note"] = f"FAIL：注入之后 {gate} 仍然是绿的 —— 这条判据抓不住这个错！"
            elif not has_real_fail(out) and ("编译失败" in out or "生成/编译" in out):
                status["rc"] = 1
                status["note"] = ("注入让门禁**编不过**（不是判据红）—— 请换一种改法：\n"
                                  + "\n".join(l for l in out.split("\n") if "error" in l or "FAIL" in l)[:600])
            else:
                line = [l for l in out.split("\n") if "FAIL" in l]
                print(f"   ok：注入后 {gate} 变红（{line[0].strip() if line else '见输出'}）")
    finally:
        # 还原必须在 finally 里做，但**不能**在 finally 里 return（会吞掉异常）。
        for rel, text in backup.items():
            write(rel, text)
        same = all(read(rel) == text for rel, text in backup.items())
        print(f"   还原：{'逐字相同 ✓' if same else '**不一致 ✗**（工作树可能被留在注入态！）'}")
        if not same:
            status["rc"] = 1
            status["note"] = "还原后文件与备份不一致 —— 请手工检查工作树！"
        else:
            okr, rout = build()
            if not okr:
                status["rc"] = 1
                status["note"] = ("还原后编译失败\n"
                                  + "\n".join(l for l in rout.split("\n") if "error" in l)[:600])
    if status["rc"]:
        print(f"   {status['note']}", file=sys.stderr)
        return status["rc"]
    if not args.no_verify_green:
        rc, _ = run_gate(gate)
        if rc != 0:
            print(f"   FAIL：还原之后 {gate} 仍然红 —— 注入留下了后遗症", file=sys.stderr)
            return 1
        print(f"   ok：还原后 {gate} 复绿 ✓")
    if args.tree == "copy":
        import hashlib
        def h(p):
            return hashlib.sha256(p.read_bytes()).hexdigest()
        changed = [rel for rel in backup if h(ORIG / rel) != hashlib.sha256(backup[rel].encode()).hexdigest()]
        print(f"   原树核对：{'未被改动 ✓' if not changed else '**被改动了 ✗** ' + str(changed)}")
        if changed:
            return 1
    cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
