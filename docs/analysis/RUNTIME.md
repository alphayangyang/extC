# extC 运行时内存管理：事实清单（以源码与产物为准）

> 证据规则：每条事实附 file:line。src/back/reclaim.c 是"发射文本的源码"（每一行是一个 C 源行，拼接后原样进入生成的 .c），因此该文件的行号既代表"运行时源码"，也代表产物中某段文本的**来源**；产物行号指 ./build/extc 生成的 .c。
> 标记约定：**[推断]** = 我从前述代码推出、没有注释或测试直接印证的结论；**[存疑]** = 在 src/ 内无法证实、或与仓库文档/Lead 转述不一致的断言；**[未接线]** = 代码已实现但没有任何 emitter 生成对它 的调用。
> 生成/比对方法、命令与结果见附录 A（字节级比对，证明"源码即产物"）。

## 0. 摘要（TL;DR）

1. 默认路径（EXTC_RECLAIM=1 且 EXTC_RC_NOCOUNT=1）既不是引用计数、也不是分代 GC，而是"每个 region 的**增量标记 + 步进清扫**"；计数入口在发射期被整段删除（reclaim.c:162-186 只在一层发射；1312-1327 把 inc/dec 变成空函数；codegen.c:13781 默认 rcNoCount=1）。默认回收完全依赖 sweep。
2. 内存只在 extc_rc_regionFree 里 free 给 libc：全文件没有 munmap/madvise/slab 缩容（3.4 节的 grep 证据），sweep 只把槽放回复用链，slab 与巨块载荷都不归还。
3. 停顿上界分两类：增量路径有常量上界（markAdvance 每步 256 单位、step 路径 8+min(wbDebt,64)、sweepStep 8 word = 512 槽）；但 collect/sweep/settle/bump 与查询接口（唯一例外 garbageHint，仅分片模式由 codegen.c:4179 调用）**没有任何 emitter 调用**（死代码，见 9.1 表），生产路径的单次无界点只有 extc_rc_regionFree（帧销毁 O(活槽)）与 releaseOld（受 budgetOk 闸控、单次 O(子树)）。因此“停顿有界”只对 mutator 路径成立，且“单位”不是时间单位（一单位可能是 SCAN_CHUNK=65536 word 的分段扫描）。
4. 触发阈值 thr = max(2*live, floor) * scale，floor=1 MiB（2636-2644），连续空扫让 scale 每次 ×4 直到 2^40（141-151）——**没有硬性内存上界**，只有分配量驱动的软触发。
5. 帧根链 [rt:chain] 已实现但**[未接线]**：codegen.c:12403-12407 显式传 0，整段不发射（产物确认缺失，附录 A）；TLS depth [rt:tlsdepth] 已接线（codegen.c:9841-9843、9965）。
6. Lead 转述的"arena 越界即 trap"不准确：extc_arena_alloc 从不越界，块满就新 calloc 一块，唯一 trap 是 OOM（codegen.c:12162，见第 2 节末行）。这属于"已知事实需修正"。
7. noSweep 一旦置位**永不清除**（全文件仅 reclaim.c:1766、1821 两处置位，除 regionFree 销毁外无清零）⇒ 该 region 在默认 NOCOUNT=1 层永不回收（shouldSweep 恒 0，reclaim.c:2656）；region 表（codegen.c:12425-12454）表满时也不登记新 region ⇒ 泄漏（注释 12405-12407）。

## 1. 发射模型：为什么"源码即产物"

| 机制 | 代码位置 | 说明 |
|---|---|---|
| 运行时文本表 | reclaim.c:91-2875 | static const char *const RECLAIM_RT[]，元素从 92 行起、表在 2870 行闭合；每元素一个 C 源行（自带换行），拼接即产物，无模板替换 |
| 分节标签 | 见 1.1 | 段标题行携带稳定标签 [rt:name]，用于按层整段跳过 |
| 按层跳过 | reclaim.c:40-50 rtSectionOn | 只有两段受控：[rt:counters] 需 timeOn（reclaim.c:162-186），[rt:chain] 需 chainOn（reclaim.c:2049-2084），其余总发射 |
| 发射期折叠 #if | reclaim.c:67-88 rtCfgIf | 只解析 EXTC_RC_NOCOUNT / EXTC_RC_TIME / EXTC_RC_SWEEP_FREE 三个宏的单条件（含 ! 取反）；可判定的 #if/#else/#endif 由发射器吞掉，未判定（#ifdef、#ifndef、比较式）原样保留给预处理器 |
| 未配对 #if 守卫 | reclaim.c:2888-2891 rtTextBug | 打印 "extc: internal error -- reclaim runtime text: ..." 并 exit(1) |
| 发射入口 | reclaim.c:2913-2993 reclaimEmitRuntime | 先写三个 #define（2914-2916），再逐行发射，末尾按需追加 extc_rc_storeRefLocal（2983-2986） |
| 唯一调用点 | codegen.c:12406-12407 | if (g->needReclaim) 下调用；参数含 g->rcNoCount、getenv("EXTC_RC_TIME")!=NULL、g->rcSweepFree==1、g->rcLocalStore==1、0（帧根链） |
| 按需性 | reclaim.h:1-20 | on-demand emission：没有需要回收的堆存储的程序一个字节都不带 |

### 1.1 段表（标签 / 源码行 / 内容 / 本产物是否发射）

| 标签 | 源码行 | 内容 | 默认是否发射 |
|---|---|---|---|
| [rt:head] | 92 | 义务清单、TICK 之外的头注释 | 是 |
| [rt:counters] | 162-186 | extc_rc_tc[8] 计数数组、extc_rc_treport、atexit 注册（TICK 宏本身在 156-160，属 [rt:head]，总发射） | 否（仅 EXTC_RC_TIME 非 NULL） |
| [rt:types] | 187-329 | ExtcRcType、ExtcRcSlab、ExtcRcClass、ExtcRcRegion、CONSERVATIVE | 是 |
| [rt:helpers] | 331-367 | slabOf、rcpOf、offOf、slotOf、bit/word、oops | 是 |
| [rt:slab] | 369-675 | owns 所需 slab 登记表（自旋锁、按代发布、ownMap、ownHot） | 是 |
| [rt:budget] | 1001-1026 | 统一工作预算（WORK_BUDGET=0 时三预算独立） | 是（#if 折叠部分） |
| [rt:xregion] | 1128-1234 | rsbit/rscnt 跨区入边、unlinkChildren | 是（#if 折叠部分） |
| [rt:nocount] | 1312-1362 | NOCOUNT=1 时计数入口变空函数；=0 时三阶段饱和计数 | 是（永远发射，内部分支） |
| [rt:shade-cache] | 1374-1462 | 写屏障 + 2048 格着色缓存 | 是 |
| [rt:xentry] | 1561-1629 | storeRefOwnedReg/Owned、统计 | 是（#if 折叠部分） |
| [rt:pin] | 1631-1750 | pin 表（宿主不可证时把值钉为根） | 是 |
| [rt:chain] | 2044-2079 | 帧根链表 framePush/Pop/Bad | 否（chainOn=0，未接线） |
| [rt:tlsdepth] | 2081-2089 | extc_rc_tlsDepth 与 owner 语义 | 是 |
| [rt:mark] | 2090-2430 | 标记：markBegin/markStep/markResume/mark、MARK_CHILD/MARK_SCAN | 是（#if 折叠部分） |
| [rt:release] | 2432-2525 | 定向释放 releaseOld/releaseDead/storeRefOwnedRelease | 是 |
| [rt:sweep] | 2531-2660 | sweepBegin/sweepWord/sweepStep/settle、thrUpd/roundOk/shouldSweep | 是 |
| [rt:counters] 之外：计时/暂停统计 | 2662-2724 | pauseNow/pauseDump/pauseArm（始终发射，受环境变量控制） | 是 |
| [rt:owe] | 2803-2849 | owe/owed/checkOwed 欠账结算 | 是 |
| 查询接口 | 2851-2869 | bytes/live/scale/sweeps/freedSlots/garbageHint/slabCount | 是 |

（注：[rt:counters] 标签出现在 reclaim.c:162 的"Per-component counting"段；2662-2724 的暂停统计没有标签、始终发射——产物中可见 pauseOn/pauseNow/pauseDump，但看不到 extc_rc_tc，附录 A 已核。）

## 2. 内存来源全景：受 GC 管理的只有一类

| 分配器 | 代码位置 | 分配原语 | 生命周期/释放 | 默认 |
|---|---|---|---|---|
| rc region（GC 管理） | reclaim.c:1055-1126 extc_rc_alloc | aligned_alloc 1 MiB slab（或巨块 malloc）+ 复用链 | 槽级：sweep/定向释放；region 级：regionFree | 开（EXTC_RECLAIM 默认 1，codegen.c:13774） |
| 巨块（big=1） | reclaim.c:786-794、2006 | calloc 头 + malloc(slotSize) 载荷 | 槽释放只进复用链；载荷只在 regionFree 时 free | 随 rc |
| bump 区 | reclaim.c:1896-1918 | rc slab 内 bump | bumpReset 整片回卷（O(1)）；否则等 regionFree | 随 rc（codegen 的 parse/临时对象） |
| arena | codegen.c:12130-12163 | calloc block（cap 从 64 起倍增，1 MiB 后不再倍增） | 作用域 extc_arena_release（留 1 个 spare block，其余 free）、extc_arena_destroy（含 spare） | 开（非 rc 程序的主要来源） |
| place 载荷区（dyn） | places.c（整段为发射文本，315 行） | calloc 一个 chunk + bump（extc_place_alloc） | place 结束整条 chunk 链 free；槽世代（zgen）使陈旧句柄失效 | 按需 |
| coroutine task arena | coroutine.c:50-57、97-150 | extc_arena_alloc | extc_task_end 时 extc_arena_destroy（不只是 release，注释 142-145） | 按需 |
| parallel worker arena | codegen.c:14685-14815 | 每 worker 一个 extc_arena（EXTC_PAR_REGION 时来自 extc_par_region） | worker 结束 destroy + regionDrop | 按需 |
| domain | domain.c | calloc/realloc | extc_dom_free | 按需 |
| plate | plate.c:29-38、62-73 | 不分配 | — | 无 |
| memfind | memfind.c | 不分配（libc memchr/memcmp/memmem） | — | 无 |

关键结论：**只有第一行（rc region）受标记-清扫管理**；arena/pool/domain 是"作用域/place/task 生命周期"的确定性内存，不受 sweep 管辖。因此"内存上界"必须按分配器分别讨论（3 节尾巴给出持续增长条件）。
## 3. 内存来源与上界（问题 1）

### 3.1 slab 几何与分配参数

| 机制 | 代码位置 | 常量 | 数值/公式 | 语义 | 默认是否生效 | 对停顿影响 | 对内存上界影响 |
|---|---|---|---|---|---|---|---|
| slab 地址空间 | reclaim.c:138、316 | EXTC_RC_SLAB_BITS / SLAB_SIZE | 20 / (1<<20)=1 MiB | slab 按 1 MiB 对齐，slabOf 一次按位与 | 是 | 无 | 每块 1 MiB 虚拟/物理 |
| slab 头尺寸 | reclaim.c:317 | EXTC_RC_SLAB_HDR | (sizeof(ExtcRcSlab)+63)&~63 | 载荷起点，64 字节对齐 | 是 | 无 | 每块固定开销 |
| 块起点 | reclaim.c:321 | EXTC_RC_SLAB_CHUNK_MIN | 64 KiB | 冷路径块大小从这里开始 | 是 | 无 | 小对象少浪费 |
| 最少槽数 | reclaim.c:322 | EXTC_RC_SLAB_SLOTS_MIN | 1024 | 块倍增直到满足它或到 1 MiB | 是 | slabNew 冷路径循环 <=4 次 | 决定每块槽数 |
| 块倍增算法 | reclaim.c:779-785 | — | chunk=64KiB；while(chunk<1MiB && chunk/slotSize<1024) chunk<<=1；cap=(chunk-HDR)/slotSize | 用除法避免 slotSize 乘积溢出 | 是 | 无 | 大槽块更小 |
| 巨块判定 | reclaim.c:786、789-794 | — | cap<2 ⇒ big=1：calloc 头 + malloc(slotSize) 载荷，nslots=1 | 槽 > ~512 KiB 时不 trap，载荷单独 malloc | 是 | 一次 malloc | 每巨块常驻一份载荷 |
| 槽数上限 | reclaim.c:807-808 | EXTC_RC_REUSE_MAX_SLOT | 0xFFFE (65534) | nslots 超过则 oops（复用链 next 用 16 位 tid 编码） | 是 | 冷路径 | 限制单块容量 |
| 槽粒度/对齐 | reclaim.c:687-691 | extc_rc_slotSize | (n+7)&~7；最小 16；8 字节粒度、非 2 的幂 | 语言最大自然对齐 8（注释 679-682） | 是 | 无 | 80 字节对象占 80 |
| 三张位图 | reclaim.c:814-816 | nwords=(nslots+63)/64 | occ、mark、rsbit 各 nwords*8 字节，calloc | 整段顺序遍历，必须整片清（注释 810-813） | 是 | 无 | 3 bit/槽 |
| holder 表 | reclaim.c:817-821 | — | hold = nslots*2 字节，仅 NOCOUNT=0 时 malloc | 计数关闭时连指针都不分配 | 默认不分配 | 无 | 默认省 2 B/槽 |
| 类型号表 | reclaim.c:822 | — | tid = nslots*2 字节（malloc，按槽惰性清） | 16 位类型号，0=无描述符 | 是 | 无 | 2 B/槽 |
| slab 内类型表 | reclaim.c:823-826、702 | TTAB_LINEAR_MAX=8 | ttabCap=8、ttabN=2、ttab[1]=CONSERVATIVE | 描述符按指针去重 | 是 | 无 | 小 |

### 3.2 ExtcRcRegion 字段（reclaim.c:262-314）

| 字段 | 行 | 语义 |
|---|---|---|
| slabs | 263 | 本 region 的 slab 单链表（只头插，只有 regionFree 摘） |
| bumpSlab / bump / bump_end | 264、267 | parse 区 bump 分配游标 |
| allocHot[32] | 265 | 按 (slotSize/8) 缓存最近可分配 slab |
| clsBkt[32] | 266 | 大小类到"可能有空槽"的 slab 链 |
| bytes | 268 | 当前占用载荷字节，O(1) 增量维护（alloc +ss / freeSlot -ss） |
| allocBytes | 269 | **单调**已分配字节，O(live) 遍历的信用基数 |
| visitedBytes | 270 | 上次定向释放遍历时的 allocBytes（定向释放的信用） |
| roundVisitedBytes | 271 | 轮信用：只在轮起止消耗，定向释放不花它 |
| live | 272 | 上次 sweep 后的活字节，触发用 2 倍 |
| thr | 273 | 缓存阈值 max(2*live, floor)*scale |
| floor | 274 | 压力下界 |
| scale | 275 | 连续空扫自学习倍数；0=未学习 |
| sweeps / freedSlots | 276 | 统计 |
| sweeping / swSlab / swWord / swId / swFreed | 278-283 | 步进 sweep 游标与轮号 |
| marking / markDone | 284-285 | 增量标记在飞标志/栈空标志（finish 需要根表，故由触发点做） |
| markedBytes | 286 | 本轮标记的载荷字节，完成时写入 live |
| markTick | 287 | 每 N 次分配推进一次 |
| markStartAlloc / wbDebt | 288-289 | 本轮起点；本轮新标灰对象数（分配点带息偿还） |
| seedSlab / seedWord / seedBits / seedsDone / seedAny | 290-295 | 跨区种子扫描游标；seedAny=false 时整段跳过 |
| foreign | 296-298 | 曾写入跨区值，单调置位；用于跳过旧值登记查找 |
| rid | 299 | region id（codegen 传 lvl*nsh+k 等） |
| noSweep | 300 | 义务 7：有不可追踪跨区入边 ⇒ 本 region 免扫 |
| part | 301 | 参与过 rc（alloc 或赋值），由 notePart 维护 |
| owe | 302-303 | 内层帧在压力线后往本 region 分配 ⇒ 拥有它的帧欠一次 sweep |
| mstk / msp / mscap | 304-305 | region 私有标记栈 |
| scanObj / scanOff | 306-312 | 大对象**分段扫描**游标（不放栈上，注释 306-310） |

### 3.3 FLOOR / SCALE_MAX / SCALE_MUL 各控制什么

| 常量 | 行 | 值 | 用途（精确） |
|---|---|---|---|
| EXTC_RC_FLOOR | 141 | (int64_t)1 << 20 = 1 MiB | regionNew 的初始 floor（1917-1926）；regionFloor(1928-1931) 可覆盖为 bytes>0?bytes:1；分片时 codegen 发 regionFloor(EXTC_RC_FLOOR/N)（codegen.c:4177） |
| EXTC_RC_SCALE_MAX | 144 | (int64_t)1 << 40 | sweepStep 收尾时 scale 的上限：scale=(scale>0?scale:1)*SCALE_MUL，clamp 到 SCALE_MAX（2600-2605） |
| EXTC_RC_SCALE_MUL | 151 | (int64_t)4 | 一次**空扫**（swFreed==0）就把阈值乘 4；真收到东西（swFreed>0）则 scale=0 归零（2600-2605） |

触发链（全部同一文件）：
- thrUpd（2636-2642）：scale=(r->scale>0?r->scale:1)；thr=r->live*2；if (thr<floor) thr=floor；if (scale>1 && thr>INT64_MAX/scale) thr=0（溢出=一分配就扫）；r->thr=thr*scale。
- roundOk（2646-2649）：max(live,floor) 的轮信用：allocBytes-roundVisitedBytes>=need。
- shouldSweep（2650-2660）：noSweep ⇒ 0；#if EXTC_RC_SWEEP_FREE ⇒ 0；否则 bytes>thr && roundOk(r)。
- live 更新：finish（2727-2743）live = markedBytes>0?markedBytes:live。

*对内存上界的影响*：thr 是唯一的"软上限"，且它可以被 scale 放大到 2^40 倍；没有硬上限（见 3.5）。

### 3.4 块是否 munmap 归还 OS

| 问题 | 结论 | 证据 |
|---|---|---|
| 有 munmap/madvise 吗 | 没有 | 对 reclaim.c 全文件 grep "munmap|madvise" 无匹配 |
| slab 在哪 free | 只在 extc_rc_regionFree：free(s->occ/mark/rsbit/hold/rscnt)、free(s->tid/ttab/thash)、if (s->big) free(s->data)、free(s) | reclaim.c:2009-2012 |
| sweep 会释放 slab 吗 | 不会；sweepWord 只 freeSlotWord + 清 mark word + 盖 swEpoch | reclaim.c:2561-2570（注释 2531-2545 也未提缩块） |
| 元数据 free 点 | 类型表 thash（714）、pin 表增长（1683）、class 节点（2014）、region 的 mstk/r（2017-2018） | 同上 |
| free 后 OS 是否立刻拿回 | **存疑**：这取决于 libc 对 aligned_alloc(1 MiB) 的实现（glibc 大块走 mmap 时 free 会 munmap），仓库内无证据 | 未验证 |

### 3.5 内存持续增长的条件（逐条，含行号）

1. **slab 数只增不减**：sweep/定向释放都只操作槽，唯一 free slab 的地点是 regionFree（2004-2007）⇒ 一个 region 的峰值就是它的常驻。
2. **region 生命周期绑定 arena**：region 由 arena 地址认领（codegen.c:12408-12424 注释），只有 cgReclaimDropArena（紧跟 extc_arena_destroy）才 regionDrop ⇒ 长寿命 arena（main、worker、task）里的 region 一直活。
3. **live 本身增长**：thr 随 2*live 上升（2638-2639），活集变大则允许的占用同步变大（这是"按需求"而非"按上限"）。
4. **scale 膨胀**：连续空扫 ×4（2602-2607）⇒ 阈值放大 ⇒ 更久不触发。空扫的成因见 2653-2654 注释：计数层已收完非环垃圾，占用≈活，所以"占用 > 2×live×倍数"只在环真的堆积时成立。
5. **noSweep 的 region 永不回收**：义务 7 的 region 在 settle 里直接放弃（2621）、shouldSweep 恒 0（2651）；其中若有环则是永久垃圾。
6. **owe 永不清偿**：内层帧分配进 outer region 只置 owe（2803-2818）；owner 帧若不再调用，则直到 regionFree 都不 sweep（[rt:owe] 段注释，reclaim.c:2808-2823：宁少不多，owner 永不再调用则 debt 永存、region 等帧销毁——松弛上界不是悬垂）。
7. **帧根链缺失导致的保守**：当前 frameRoots 未接线（codegen.c:12403-12407），不产生额外泄漏，但也就没有"链上帧根"这一层收敛 [推断]。
8. **region 登记表满即泄漏 region**：EXTC_RC_REG_MAX=512，表满时 regionNew 后**不登记**（codegen.c:12425-12454，注释逐字 "When the table is full a new region is not entered: leaking a region is preferred over freeing it early"）。
9. **bump 分配不进 sweep**：bump 槽不置 occ（1889-1907：注释明确 bump object is not a slab slot，不置 occ、不计入 region bytes；occ 只在 alloc 的 1112 置位），sweep 只看 occ ⇒ bump 对象不被回收，只能靠 bumpReset 整片回卷或 regionFree。
10. **巨块载荷不单独归还**：big=1 的载荷仅在 regionFree free（2006）；即使该槽被 sweep 释放，1 MiB+ 的载荷也留在 freelist 里。
11. **空 slab 不回收**：freeSlot 只会把 slab 压回大小类链（1263），不会 free 整块。
12. **元数据随 slab 常驻**：occ/mark/rsbit/tid（+NOCOUNT=0 时的 hold）直到 regionFree。

*结论*：rc 侧的内存上界 = Σ(每个存活 region 的 slab 峰值)。它不是常量上界，而是"分配速率 × 触发阈值 × 环堆积量"的函数；能真正"有界"的只有 arena/pool 那类作用域内存。

## 4. 分配：extc_rc_alloc 逐步流程（问题 2）

签名（逐字）：reclaim.c:1055

    __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(ExtcRcRegion *r, size_t n, const ExtcRcType *td) {

EXTC_RC_ALLOC_DECL 由 EXTC_RC_ALLOC_INLINE 决定：=1（默认）时是 always_inline inline，=0 时是 noinline（reclaim.c:1047-1053；理由注释 1039-1046：-O1 下每次分配约 13.5 Ir 的调用开销，展开成本约 2.7KB 机器码/分配点）。

| # | 代码位置 | 动作 | 语义要点 |
|---|---|---|---|
| 1 | reclaim.c:1056-1057 | EXTC_RC_TICK(0)；extc_rc_notePart(r) | 任何分配都先声明本 region 参与 rc（notePart 894-896 单调置 part / partN），否则 regionFree 的 R3 自检与 pin 的"进程级"判断会漏 |
| 2 | reclaim.c:1058-1065 | if (r->sweeping) { if (noSweep) sweeping=0; else sweepStep(r, EXTC_RC_SWEEP_BUDGET); } | 义务 7：被丢弃的 region 一个字节都不再扫；否则每分配推进 8 个 word（512 槽）的清扫 |
| 3 | reclaim.c:1066-1073 | if (r->marking) { if (msp!=0 \|\| !seedsDone \|\| scanObj) markAdvance(r); if (seedsDone && msp==0 && !scanObj) finish(r); } | 标记中每 64 次分配推进一次（1023-1038，单步上限 256 单位）；一旦标完**当场关屏障**，否则这一轮会闲置上万次分配 |
| 4 | reclaim.c:1074-1080 | ss = slotSize(n)；ac = ss/8；hot = ac<32 ? &r->allocHot[ac] : 0；s = hot ? *hot : 0 | 不校验 s->ss==ss：allocHot 只由本函数写，且 s 只来自 classSlot(r,ss)/slabNew(r,ss)，块寿命内 stride 不变（1078-1080） |
| 5 | reclaim.c:1081-1092 | if (!s \|\| (freelist<0 && bump>=nslots)) { c=classSlot(r,ss); s=c?c->head:0; while (满) { c->head=s->availNext; s->avail=0; s=c->head; } } | 大小类链"可能有空槽"的惰性弹出；每块最多弹一次 ⇒ 摊还 O(1)；不是线性扫 r->slabs（1082-1084） |
| 6 | reclaim.c:1093-1098 | s = slabNew(r,ss)；s->next=r->slabs；r->slabs=s；classPush | 冷路径：aligned_alloc 1 MiB + memset 头 + 三张 calloc 位图；不预链 free list（833-839） |
| 7 | reclaim.c:1099 | *hot = s | 大小类 O(1) 缓存 |
| 8 | reclaim.c:1101-1107 | if (freelist>=0) { i=freelist; freelist=tid[i]-1; } else { i=bump++; if (rscnt) rscnt[i]=0; } | 复用链头插在 **tid[]** 里（槽号+1，0=链尾），不写 payload：释放的槽是冷的，写 payload 要 RFO 拉整条 cache line；新槽的跨区入边计数懒惰清零 |
| 9 | reclaim.c:1108-1109 | p = SLAB_DATA(s) + i*ss；memset(p,0,ss) | 语言承诺"新对象全零"，也是标记的前提（未初始化指针字段会被当活指针） |
| 10 | reclaim.c:1110-1112 | wi=word(i); bi=bit(i); occ[wi] \|= bi | occupied 位图 = "本槽是活对象"，bump 槽不置位（1889-1907 注释） |
| 11 | reclaim.c:1113 | if (marking) mark[wi] \|= bi | 标记期间分配即标黑（新对象不可能死） |
| 12 | reclaim.c:1114-1118 | if (sweeping && s->swEpoch != r->swId && (s != r->swSlab \|\| wi >= r->swWord)) mark[wi] \|= bi | 只在游标**未到达**的位置标黑：已扫过的位置本轮不会再被看，标了会留下无人清理的陈旧 mark 位 |
| 13 | reclaim.c:1119 | tid[i] = typeIdx(s, td) | 16 位类型号：0=无描述符、1=CONSERVATIVE、>=2=ttab 索引（446-455） |
| 14 | reclaim.c:1120-1122 | if (!NOCOUNT) hold[i]=0 | 默认 NOCOUNT=1，整行被发射期折叠掉 |
| 15 | reclaim.c:1123-1124 | r->bytes += ss; r->allocBytes += ss | bytes 是 O(1) 增量维护的当前占用；allocBytes 单调不减（定向释放的信用基础） |
| 16 | reclaim.c:1125 | return p | 地址在块寿命内不变（无压缩） |

### 4.1 slabOf 与槽号反乘（O(1) 的基础）

| 操作 | 代码位置 | 实现（逐字要点） | 误差/边界 |
|---|---|---|---|
| extc_rc_slabOf(p) | reclaim.c:339-340 | return (ExtcRcSlab *)((uintptr_t)p & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1)); | slab 按 1 MiB 对齐，一次按位与即可由对象指针得 slab 头；plane/pool/stack 指针不在 slab 内，故必须先 owns（义务 6） |
| extc_rc_rcpOf(ss) | reclaim.c:348-350 | (uint64_t)1 << EXTC_RC_RCP_SHIFT) / ss + 1 | 倒数乘常数，stride 非 2 的幂也成立 |
| extc_rc_offOf(s,p) | reclaim.c:352-354 | (char *)p - SLAB_DATA(s) | SLAB_DATA 即 slab+SLAB_HDR |
| extc_rc_slotOfOff(s,off) | reclaim.c:355-357 | (size_t)(((uint64_t)off * s->rcp) >> EXTC_RC_RCP_SHIFT) | 商误差 < 2^-22；off/ss 均为 8 的倍数，最小非零商 8/ss >= 2^-17，故 floor 恒精确；tests/reclaim-perf/slotindex.c 穷举验证 |
| extc_rc_slotOf(s,p) | reclaim.c:358-359 | slotOfOff(s, offOf(s,p)) | |
| extc_rc_bit/word | reclaim.c:360-363 | 1ull<<(i&63)、i>>6 | 位图按 64-bit word 顺序遍历 |

### 4.2 是否 O(1)

- 热路径（第 2-4、8-16 步）全是常数分支，无循环、无搜索：**O(1)**。
- 大小类链的 while 是惰性弹出，同一块最多弹一次 ⇒ 摊还 O(1)（1084-1085）。
- 冷路径 slabNew 一次要至少 1024 槽（EXTC_RC_SLAB_SLOTS_MIN=1024，reclaim.c:322；chunk 从 64 KiB 起倍增直到槽数 >= 1024 或到 1 MiB，reclaim.c:779-785）⇒ 摊到每槽 <= 1/1024 次分配。
- typeIdx 在 ttab 内 <= EXTC_RC_TTAB_LINEAR_MAX=8 个描述符时线性扫（<= 8 次指针比较），超过则建 16 位开放寻址索引转 O(1)（reclaim.c:693-705、702）。
- 严格说单次分配的代价含 memset(p,0,ss)：与**对象大小**成正比，不是常数。复杂度应写 O(1) 次分配器操作 + O(n) 清零。
- 若 slab 的 ttab 塞满 0xFFFF 个描述符则 oops("slab type table overflow")（reclaim.c:695-696 注释：静默退回 0 会让标记跳过引用字段、提前释放，是不安全方向，宁可当场 trap）。

### 4.3 OOM 行为

| 位置 | 触发 | 行为 |
|---|---|---|
| reclaim.c:777-843 slabNew | aligned_alloc(1MiB)/calloc/malloc 任一失败 | extc_rc_oops("slab allocation failed" / "oversized slot..." / "slab metadata allocation failed") → reclaim.c:364-367 fprintf(stderr,"trap: reclaim: %s\n") + exit(1) |
| reclaim.c:1055-1126 alloc | 本身不失败 | 没有返回 NULL 的路径，也没有"GC 后重试" |
| reclaim.c:1636-1740 pin 表增长 | realloc 失败 | pinAdd 返回 0 ⇒ storeRefUnknown 退回 noSweep（reclaim.c:1759-1769），不 trap |

结论：**堆 OOM = 进程终止**（trap），不是"返回 NULL 交给调用方"；运行时不因压力主动收缩堆（无 munmap）。

### 4.4 payload 对齐

- slotSize：(n+7)&~7，最小 16（reclaim.c:687-690）。8 字节粒度、非 2 的幂：80 字节对象占 80 字节槽（不占 128），注释 677-683 逐字说明"最大自然对齐是 8，语言无 vector/128 位类型"。
- 普通 slab：整块 aligned_alloc(1 MiB, 1 MiB)（796-797）；SLAB_HDR=(sizeof(ExtcRcSlab)+63)&~63（317），data=slab+SLAB_HDR（799）⇒ 槽首 8 字节对齐（ss 是 8 的倍数）。
- 巨块（big）：载荷单独 malloc(slotSize)（791-793），只有 1 个槽；对齐由 malloc 保证（不低于 max_align_t 的 16）■推断；巨块的地址识别不用按位与，靠 ownBig 精确比对槽首（reclaim.c:584-596）。
- shade 缓存/标记栈用"低 3 位为 0"编码"指针 vs 强制扫描哨兵"（reclaim.c:1370-1375），同样依赖 8 字节对齐。

## 5. 释放（问题 3）

### 5.1 定向释放：谁有资格证明子树死亡

| 层级 | 代码位置 | 判定 | 默认是否发生 |
|---|---|---|---|
| 编译器选择发射 | codegen.c:7132（storeRefOwnedRelease；注释 7047-7089） | 只在"带 home 的引用槽被覆盖"的特定形状下 | 是（但仅命中该形状） |
| 入口 | reclaim.c:2524-2530 | shade(v) → releaseDead(home, *slot, roots, nroots) → storeRefOwned(slot,v,owner) | — |
| 闸 1 | reclaim.c:2511-2514 | home->noSweep \|\| home->owe ⇒ 直接返回（根不完整，不准证明） | 是 |
| 闸 2 | reclaim.c:2515-2518 | 旧值必须 ownSlab(old) 且 s->region==home，否则返回（旧值不在静态给定 region，保守跳过） | 是 |
| 闸 3 | reclaim.c:2519-2522 | !budgetOk(home) ⇒ 返回；否则 releaseOld 后 home->visitedBytes=home->allocBytes | 是 |
| 信用 | reclaim.c:1943-1946 | budgetOk: allocBytes - visitedBytes >= max(live, floor) | 每花一次 O(live) 遍历，就要先攒够 max(live,floor) 字节的分配 |
| 主体 | reclaim.c:2456-2496 | ① extc_rc_mark(r,roots,nroots)（与 collect 同一 seed 集）② push old ③ 逐个 pop | — |
| pop 判据 | reclaim.c:2468-2488 | 不在本 region / 非槽首 / 未 occ ⇒ 停（区域外当活物）；**mark 位已置 ⇒ 停**（还有别的根或种子指向它，且其子树已在第 ① 步标黑）；否则先按描述符 push children 再 freeSlot（描述符与字段必须还完整，freeSlot 会覆写 payload 头） | — |
| 收尾 | reclaim.c:2490-2492 | shadeNextGen()，并对每个 slab memset(mark,0,nwords) | mark 位只有 sweep 会清，这里不清会留给下一轮当活物 |
| 统计 | reclaim.c:2495-2496 | relN++；relSlots += freed | 可由 EXTC_RC_STATS 打印（reclaim.c:1573-1589） |

安全性论证（reclaim.c:2447-2452 逐字要点）：释放集是"同一时刻、同一 seed 集下 collect 会释放的集合"的**子集**（轻量级 trial deletion）；判据只决定"这一趟值不值"；判多只可能来自不完整的 roots/seeds，而那是 collect 的前置条件本身，由 noSweep/owe/帧所有权守着。

回答"谁有资格证明子树死亡"：**不是计数**，而是同时满足——覆盖发生在带 home 的槽上 + 该帧的静态根表对 home 完整（无 noSweep/owe）+ 旧值确实在本 region 内 + 已攒够一次 O(live) 遍历的信用 + 旧值在"同一 seed 集"下不可达。计数层（NOCOUNT=0）另有一条独立的"hold 减到 0 当场 freeSlot"路径（reclaim.c:1336-1366），两者互不替代。

### 5.2 sweep 步进

| 阶段 | 代码位置 | 动作 |
|---|---|---|
| 开始 | reclaim.c:2552-2559 sweepBegin | shadeNextGen()；sweeping=1；swId++；swSlab=r->slabs；swWord=0；swFreed=0 |
| 扫一个 word | reclaim.c:2561-2571 sweepWord | dead = occ[w] & ~mark[w]；非 0 ⇒ freeSlotWord（批量 head 插链 + 每词一次记账）累加 swFreed；mark[w]=0（**只清当前 word**）；++swWord 到头则 s->swEpoch=swId、swSlab=s->next、swWord=0 |
| 推进 | reclaim.c:2575-2615 sweepStep(r,left) | while (left>0 && swSlab) 每 word 一步；扫完（swSlab==NULL）才结算 |
| 结算 | reclaim.c:2605-2613 | sweeping=0；swFreed==0 ⇒ scale=(scale>0?scale:1)*SCALE_MUL（clamp SCALE_MAX）；否则 scale=0；thrUpd(r)；sweeps++；返回 1 |
| 全量排空 | reclaim.c:2618-2628 settle | marking 且 !noSweep ⇒ 0xffffffff 排空 markStep 再 finish；!sweeping 返回；noSweep ⇒ sweeping=0 直接放弃（2621）；否则 while(!sweepStep(r,0xffffffff)){} |
| 便捷入口 | reclaim.c:2630-2634 extc_rc_sweep | sweepBegin + settle，返回 swFreed |

- 步长常量：EXTC_RC_SWEEP_BUDGET = 8（reclaim.c:889）⇒ 每次 sweepStep 最多扫 8 个 64-bit word = **512 个槽**；freeSlotWord 是整词批量（1271-1310）。
- 游标与 occ/rsbit 的关系：sweep 判据只有 occ & ~mark（2559-2561）。它**不动 occ**（occ 由 alloc 1112 置位、freeSlot 1254 清位），**不动 rsbit**（rsbit 由 rsInc/rsDec 与 freeSlot 1255 维护，义务 5），也不动 hold/rscnt。
- free 槽何时入复用链：freeSlotWord 里逐槽 head 插进 s->freelist（链值写 tid[i]，1271-1310）；下一次 alloc 第 8 步优先复用。
- 何时整 slab 空闲：**永不释放**。全文件无 munmap/madvise，slab 只在 regionFree 里 free（2004-2007）；整块空闲时它只是所有槽都进了 freelist，仍挂在 region->slabs 与大小类链上（1263 classPush）。
- "sweep 不动 bitmap"的确切后果：
  1. mark 位图按 word 顺序清（2563），所以**标记必须从干净的 mark 位图开始**；清扫未完成时触发点只续扫、不新开标记（2539 注释）。
  2. 清扫期间分配的对象必须在分配点补标黑（1114-1118），否则会被本轮当垃圾；只标游标未到之处，已扫过的位置标了会留下无人清的陈旧位。
  3. 定向释放自己负责清整片 mark（2485-2487），理由同上。
  4. freeSlot 故意不清 mark（义务 5 只要求清 rsbit）：free 槽不参与本轮标记，下一轮 sweep 扫到该 word 时会顺手清掉。

### 5.3 记账与"回收了多少"

| 字段 | 更新点 | 语义 |
|---|---|---|
| r->bytes | alloc +ss（1123）、freeSlot -ss（1260） | 当前占用 payload 字节，O(1) 增量，不重算 |
| r->allocBytes | alloc +ss（1124）、regionFloor 后不变 | **单调**累计已分配字节；定向释放/轮信用都基于它 |
| r->freedSlots | freeSlot 每次 ++（1261-1262）、freeSlotWord 每词累加 | 定义 = "每次 freeSlot 都计"，不是 slab 数 |
| swFreed | sweepWord 累加（2561）、sweepBegin 清零 | 本轮 sweep 释放的槽数，作为 step/collect/finish 的返回值 |
| r->live | finish 时 live = markedBytes>0 ? markedBytes : live（2731） | 上一轮标记得到的活字节；0 活时保留旧值（不让阈值塌到 floor 以下抖动） |
| r->visitedBytes / roundVisitedBytes | releaseOld 后 = allocBytes（2517）、begin/finish（2748-2749、2739-2740） | 定向释放信用 / 轮信用，两者独立（注释：共用一个会被饿死） |


## 6. 标记（问题 4）

### 6.1 根集合的构造：四个来源，默认只有一个真的被读

| 根来源 | 代码位置 | 构造方式 | 默认是否生效 |
|---|---|---|---|
| 静态根表 roots[] | 由编译器生成并以参数传入：codegen.c:4143 发射 extc_rc_maybeCollect(%s, %s, %zu) 时传 roots/nroots；消费端 markBegin（reclaim.c:2103-2114）/ mark（2425-2430）/ collect（2766-2775） | markBegin 对 roots 中每个**非空且 owns** 的槽 push（义务 6：表外指针跳过，否则按位与会读无关内存） | **是**——唯一实际种子来源 |
| pin 表 | markBegin 2103 调 pinSeed(r)；pinSeed 1742-1750 | 进程级开放寻址表里属于本 region 的被钉值 push 进标记栈；pinAdd 由 storeRefUnknown（1754-1764）与 pin 语义维护 | 否（仅"宿主不可追踪、但值本身是 rc 对象"的路径） |
| 帧根链 [rt:chain] | 结构体 reclaim.c:2060-2084；markBegin 注释 2103-2104 明说 seeding 排在 step 3、**当前不读链**；chainBad 2075：链上任一帧 bad ⇒ 枚举不完整 ⇒ 拒绝 sweep | framePush/framePop/frameBad（2064-2076） | **未接线**：codegen.c:12403-12407 注释逐字 "The frame root chain (the [rt:chain] section in reclaim.c) has no emitter pushing into the chain yet, so pushing code would be all dead => passing 0 emits nothing"，整段按 tier 不发射（产物中缺失，见附录 A） |
| TLS depth | [rt:tlsdepth] 2081-2089；extc_rc_tlsDepth；codegen.c:9841-9843 序言、9965 尾声 | 不是根，而是"谁是 owner"的判据：worker 入口帧（depth==0）可触发 sweep，因为其唯一调用者是 C 运行时、外层没有 extC 活帧；内层帧只能记 owe | 是（仅 parallel::run worker 链） |
| bump 区 | reclaim.c:1896-1918 | 不置 occ ⇒ 不是活槽，不参与标记/清扫 | bump 当前**无发射器调用**（codegen.c 中无 extc_rc_bump/bumpReset 引用）——已实现、未接线 |

markBegin（reclaim.c:2103-2114）顺序：msp=0 → scanObj=0/scanOff=0（上轮大对象分段游标不延续）→ 逐 roots push → pinSeed(r) → seedSlab=r->slabs、seedWord=0、seedBits=0 → seedsDone = (slabs==NULL) || !seedAny（本 region 从没登记过跨区入边则整轮跳过种子扫描）。

### 6.2 黑化与 shade 缓存

| 机制 | 代码位置 | 语义 |
|---|---|---|
| mark 位图 | ExtcRcSlab.mark（reclaim.c:216-250，calloc 于 814-816） | 置位=黑=可达；MARK_SCAN 宏（2169-2237）先判 mark 位去重，再判 occ |
| shade 缓存 | reclaim.c:1401-1467 | 2048 格 × 8 B = 16 KB 静态；索引 ((addr>>3)+gen)&2047，代际旋进索引 ⇒ 同格同指针必属同代 ⇒ 命中精确；2048 代绕回前整表 memset 清零；注释自称 hint 不是 cache，冲突覆盖无所谓；命中率接近 72% |
| 写屏障慢路径 shadeSlow | reclaim.c:1441-1456（noinline） | 顺序：!s 或 !s->region 或 (partN>1 && !region->marking) ⇒ 返回；mark 位已置 ⇒ shadeRemember 后返回（多数调用在此退出）；未 occ ⇒ 返回；否则置 mark、remember、region->wbDebt++、markPushForce |
| 快路径 shade | reclaim.c:1459-1467 | !wb 或 !v 直接返回 —— 没有标记在飞时两条测试就出门 |
| 旧边屏障 shadeOld | reclaim.c:1473-1480（**仅 NOCOUNT**） | 无计数层的局部槽不做屏障，保护移到被覆盖的旧边：覆盖前先 shade 旧值。否则"从还没扫的对象读到白对象、该边又在堆上被删、白对象只剩局部槽一个来源"会被判死 |

### 6.3 markStep 的预算常量

预算单位不是字节：walk 一个对象 = 1、scan 一个 word = 1、push 一个槽 = 1（reclaim.c:1005-1006 注释）。

| 常量 | 行 | 数值 | 作用 |
|---|---|---|---|
| EXTC_RC_SEED_PUSHES | 896 | 256 | 每步最多推的种子槽数 |
| EXTC_RC_SEED_WORDS | 899 | 512 | 每步最多扫的 seed word 数（rsbit 位图） |
| EXTC_RC_MARK_BATCH_MAX | 911 | 256 | 一次 markStep 的 walk 上限；markAdvance 算出的 b 被 clamp 到它 |
| EXTC_RC_MARK_BATCH_POP | 914 | 200 | 批量 pop 的上限；且 #if POP > BATCH_MAX 直接 #error（918-920） |
| EXTC_RC_MARK_SHALLOW | 2131 | 64 | 标记栈深 <= 64 时走"浅栈直通"（逐个 pop 立即 scan），否则走批量 pop + prefetch |
| EXTC_RC_SCAN_CHUNK | 2147 | 65536 | 大对象/保守对象的分段扫描每段 word 数（游标记 scanObj/scanOff，不进标记栈） |
| EXTC_RC_MARK_STEP | 908 | 8 | stepBudget 的基数 |
| EXTC_RC_MARK_EVERY | 902 | 64 | 分配点每 64 次分配推进一次标记 |
| EXTC_RC_MARK_WINDOW | 905 | 2<<20 = 2 MiB | 分配漂移超过窗口则 markAdvance 预算 x8 |
| EXTC_RC_ASSIST_CAP | 999 | 512 | 单步 assist（wbDebt 偿还）上限 |
| stepBudget | 1015-1019 | MARK_STEP + min(wbDebt,64) = [8,72] | 只被 extc_rc_step 用（2760） |
| markAdvance | 1023-1038 | b = MARK_EVERY*MARK_STEP + min(wbDebt,512) = 512+debt；窗口外 x8；clamp 到 256 | 分配点路径（1057-1073 调） |

**默认参数下的两个"互相覆盖"事实（重要）**：
1. markAdvance 的基数 512 恒 > MARK_BATCH_MAX 256，clamp 之后 b 恒 = 256 ⇒ **MARK_WINDOW 的 x8 boost 在默认参数下永不生效**，分配点每次推进的单位数恒定 256（与漂移无关）。只有用 -D 把 MARK_EVERY/MARK_STEP 调小才会让窗口生效。
2. step() 路径每次 <= 72 单位（8+64 上限），而分配点路径每次 256 单位：**两条路径的单步上界不同**，文档与优化方案必须分开谈。

WORK_BUDGET（reclaim.c:1001-1013）：默认 0 = 关 ⇒ 三份预算（walk 256 / seed word 512 / seed pushes 256）**独立且在同一个 markStep 内叠加**（1003-1004 逐字："the three budgets are independent ... and the three stack in one step"）；W>0 时三者共用一个总数 wleft（2271），单步总工作量按构造 <= W（1007）。

### 6.4 精确 tracing vs conservative

| 类别 | 代码位置 | 行为 |
|---|---|---|
| 精确 tracing（默认） | 义务 4（reclaim.c:113-114）+ MARK_SCAN 2205-2237 | 按槽自带类型描述符（tid→ttab→ExtcRcType{nptr,off}）逐字段读指针，只走 region 内的槽 |
| CONSERVATIVE | 定义 209（(const ExtcRcType*)-1）、用法 2169-2237 | 唯一场景：alloc<T>(n) 且 T 含引用——元素 1..n-1 的偏移随运行期计数变，静态描述符表达不了。整槽按 word 当候选引用扫，**只会多留不会漏**（保守） |
| 无描述符 tid==0 | MARK_SCAN | 数据对象，直接跳过（不读任何 word） |
| region 外指针 | 义务附加准则 reclaim.c:123-126 | 标记在区域外对象处**停止，且不写它的 mark 位**（别的 region 的黑化会让它下次 sweep 跳过并释放其子） |
| 大对象 | 2239-2253 markResume、2147 SCAN_CHUNK | 超过 SCAN_CHUNK 个 word 的不放标记栈，只记 scanObj/scanOff 分段续扫 |

### 6.5 是否移动对象？是否 STW？

| 问题 | 答案 | 证据 |
|---|---|---|
| 是否移动对象 | **不移动**。无压缩、无 memmove；对象地址 = slab 基址 + 槽号 × stride，在 region 生命周期内恒定；markPush 存裸指针，pin 表以值指针为键，regionOf 以 arena 地址为键 | 全文件无 memmove/memcpy 搬移逻辑；地址只由 4.1 的槽公式给出；slab 永不重定位（无 munmap/mremap） |
| 是否 STW | **不 STW**。标记与 mutator 在同一线程内交错推进（分配点 markAdvance、触发点 markStep），用写屏障维持三色不变式；没有任何暂停其他线程的代码 | 6.2 屏障 + 9 节触发点 |
| 有没有"会长时间停"的入口 | 有：extc_rc_collect（2766-2775，契约"一次调用完成一整轮"）与 extc_rc_settle（2613-2623，排空标记+清扫） | collect **无任何发射器调用**（grep 全 src：只有定义与注释各 1 处；reclaim.h:58 只作文档提及）⇒ 已实现、未接线；settle 由查询函数 2851-2855 与 extc_rc_sweep 调用，而查询函数同样**未被 codegen 发射**（仅测试/诊断用） |

结论：生产路径上的停顿来自 (a) 触发点 step()：标记 <= 72 单位 + 一次 sweepStep 8 word（512 槽）；(b) 分配点 markAdvance：标记 256 单位 + sweepStep 8 word。两者都不含"整轮排空"，所以**停顿有界的唯一保证来自这些常量**；但单步扫 8 个 word 会同时 free 最多 512 个槽（freeSlotWord），其成本与槽数成正比，不是常数。

## 7. 跨区指针与登记（问题 5）

### 7.1 写引用槽的五个入口

| 入口 | 代码位置 | 何时发射 | 跨区登记 | 对 sweep 的影响 |
|---|---|---|---|---|
| extc_rc_initRef | reclaim.c:1483-1489 | 新槽首次赋值（codegen.c:4612/7299/7678） | 无（新槽只增目标计数；旧值是下一槽号，不可读） | 无 |
| extc_rc_initRefNoHold | reclaim.c:1492-1497 | 调用点由 planRcNoDec 选出（codegen.c:7687）：动态范围内无 dec，故不增计数 | 无 | 无 |
| extc_rc_storeRef | reclaim.c:1500-1508 | 一般引用赋值（codegen.c:7480） | shade(v)、shadeOld(slot)、写、inc(v)、dec(old)；**NOCOUNT 时 inc/dec 是空函数** | 无（NOCOUNT 默认下只是一个裸写 + 屏障） |
| extc_rc_storeRefOwned / OwnedReg | reclaim.c:1593-1634 / 1520-1559 | 编译器能证明宿主 region 时（codegen.c:7319/8745、8768） | (a) 新值跨区 ⇒ rsIncSlab + host->foreign=1；(b) 旧值只有在 host->foreign 时才查（foreign 单调置位，从不清，故大多数赋值跳过旧值 ownSlab） | 无 |
| extc_rc_storeRefUnknown | reclaim.c:1759-1769 | 源不可证（codegen.c 无直接发射；由 storeRefSlot 1791-1796 兜底） | 先 pinAdd(v)：成功 ⇒ 值成为种子，目标 region 照常 sweep；**pinAdd 失败（OOM）⇒ regionOfOwn(v)->noSweep=1**；然后 storeRef(slot,v) | **目标 region 永久退出 sweep** |
| extc_rc_storeRefSlot | reclaim.c:1796-1801 | 不知道槽本身属于哪个 region 时（codegen.c:7316/8709） | 先 slabOfInterior 反查宿主 slab，查到就 storeRefOwnedReg，查不到就 storeRefUnknown | 同上 |
| extc_rc_storeStructCons | reclaim.c:1811-1829 | 整值 struct 赋值、叶子不可枚举（codegen.c:7717/8787） | 逐 word 当候选引用**只增不减** + 登记跨区边；hostKnown==0 时目标 region noSweep=1（1821） | 可能永久退出 sweep |
| extc_rc_storeStructRooted | reclaim.c:1839-1847 | 整值赋值但编译器知道根表已按所有 enum variant 静态展开 | 逐 word 只增；**不**让 region 退出 sweep、**不**登记跨区边（注释 1826-1833：根表已完整） | 无 |

### 7.2 跨区边规则（rsbit 是缓存，rscnt 是精确计数）

| 机制 | 代码位置 | 语义 |
|---|---|---|
| rsbit | ExtcRcSlab.rsbit（226-250）、维护点 1180/1195/1255 | "该目标槽有**活**跨区入边"的缓存；0→1 置位、1→0 清位；饱和值粘住永不减（与 holder 同规则，义务 3） |
| rscnt | 同上；懒惰 calloc（1166-1172） | 每槽精确入边数（uint16 饱和 65535）；从没跨过区的 slab 不建表（省 2 B/槽） |
| 边的三处转移 | reclaim.c:1133-1137 逐字列 3 条；实现 1173-1234 | (1) storeRefOwned 一次赋值替换一条边（旧减新增，仅跨区）；(2) freeSlot 释放对象时丢掉它持有的跨区边；(3) regionFree 整区丢弃时按**仍活**对象批量减 |
| 撤销时机 | reclaim.c:1220-1236 unlinkChildren | 槽"即将不再是对象"前撤销其出边：字段与描述符必须还完整 ⇒ 所有调用点在写 free 链与清描述符之前 |
| 为什么不是只置不清 | reclaim.c:1137 注释 | 只置不清会让种子涨到每个对象；现在的登记位数 = 当前活跨区边数，与轮数解耦 |
| 高水位 | reclaim.c:1139-1141 rsIncN/rsDecN/rsHwm | rs_live = inc-dec = 当前活跨区边数；判据看 rs_hwm（撤销跟得上时只受 sweep 触发阈值约束） |
| 运行期守卫 | reclaim.c:1154-1165 EXTC_RC_FOREIGN_GUARD（默认 0） | 在"因为本 region 没有外来指针而跳过旧值查找"的点重算旧值 region 并计数；真出现未登记的外来旧值则 bad>0；判据要求 checks>0 && bad==0 |

### 7.3 pin 语义

| 机制 | 代码位置 | 语义 |
|---|---|---|
| 动机 | reclaim.c:1636-1653 注释 | 宿主（view 元素/pool buffer 槽）编译期不可追踪但值本身是 rc 对象。老办法把**整个目标 region** 置 noSweep（永久退出 sweep，只能靠计数归零）；新办法把**值本身**钉成根 |
| 结构 | reclaim.c:1654-1714 | 进程级开放寻址哈希（键=值指针，load<=1/2，Knuth 后移删除），每项存 v/rg/n（rg 在登记时 ownSlab 算一次，轮起/region 死只比 region 指针） |
| 方向（失败怎么办） | reclaim.c:1647 | 只有 pin 不可能（分配失败）才退回 noSweep——宁少收，不可错杀 |
| 线程 | reclaim.c:1639-1653 | pin 表进程级（region 可能被别的 worker sweep），insert/remove/轮起加锁；热路径永不碰 |
| pinAdd | reclaim.c:1693-1714 | !v 或 !owns(v) ⇒ 返回 1（不是活槽，无需 pin）；加锁；负载 >1/2 ⇒ pinGrow（失败返回 0）；已存在则 n++；否则记 v/rg/n=1 |
| pinSub | reclaim.c:1715-1728 | n-- 或擦除（Knuth 后移） |
| pinDropRegion | reclaim.c:1730-1740 | region 死时删掉它名下所有 pin |
| pinSeed | reclaim.c:1747-1755（noinline） | 把本 region 的 pinned 值 push 进标记栈；pinN==0 时一次原子读即返回（最常见情况） |

### 7.4 noSweep：永久退出 sweep（内存泄漏风险的主要来源）

| 触发条件 | 代码位置 | 后果 |
|---|---|---|
| storeRefUnknown 的 pinAdd 失败 | reclaim.c:1759-1769（1761 置位） | 目标 region noSweep=1 |
| storeStructCons 且 hostKnown==0 | reclaim.c:1811-1829（1821 置位） | 同上 |
| 帧根链上存在 bad 帧 | reclaim.c:2080 chainBad | 调用方拒绝 sweep（同一决策） |
| **清除条件** | grep 实证：全文件只有 1761/1821 两处置位，**没有任何一处清零**（除 region 整体 regionFree 被销毁） | 该 region 在生命周期内**永久**不再 sweep |

noSweep 的实际效果（逐点）：
- shouldSweep 直接返回 0（reclaim.c:2656，注释"reclaimed by counting only"）⇒ 永不触发新轮。
- 分配点若发现 sweeping 在飞 ⇒ 当场作废、一个字节不再扫（1063）。
- maybeCollect 里 sweeping 分支 ⇒ 作废并返回 -1（2785）；checkOwed 同（2825）。
- settle 直接放弃（2621）；定向释放的闸 1 拒绝（2509）。
- **默认 NOCOUNT=1 时计数层整体编译掉** ⇒ noSweep 的 region **永远不会回收**（既不能 sweep，也没有 hold 归零路径）——这是"内存上界"论证里必须显式扣除的一块：上界退化为所有曾走 storeRefUnknown/storeStructCons(hostKnown=0) 的 region 的 allocBytes 总和，且与触发阈值无关。
- 触发条件在默认代码里有多常见：storeRefUnknown 只由 storeRefSlot 的兜底调用；codegen 在能证明宿主时走 OwnedReg，所以正常形状不触发；但"槽指针来自 C/库代码"的形状会触发（未量化，见 13 节）。


## 8. 计数层（仅 EXTC_RC_NOCOUNT=0 时存在）（问题 6）

### 8.1 默认：整层被编译掉

reclaim.c:1317-1332 是 [rt:nocount] 段的开关：**EXTC_RC_NOCOUNT=1 时 extc_rc_holdUpdSlab / holdUpd / inc / dec 都是空函数**（1320-1327），而且发射器会在发射期把整个 #if 分支折叠掉（rtCfgIf 67-88）——产物里连空函数都没有（附录 A 的产物 1202-1209 行可以看到折叠后的结果）。

默认值链路（三点实测，与 Lead 的已知事实一致）：
1. codegen.c:13781 g->rcNoCount = 1（注释："Default 1 = no counting, reclaimed by sweeping alone"）。
2. 发射器 reclaim.c:2919 写出 #define EXTC_RC_NOCOUNT 1（=0 时写 0）。
3. 运行时 reclaim.c:1324 #if EXTC_RC_NOCOUNT 把计数入口变成空操作。
反向开关：EXTC_RC_NOCOUNT=0 ⇒ codegen.c:13803-13806 置 rcNoCount=0；EXTC_RC_SWEEPFREE=1 也强制 rcNoCount=0（codegen.c:13821-13824）。

### 8.2 计数语义（NOCOUNT=0）

| 机制 | 代码位置 | 语义 |
|---|---|---|
| 每槽 hold | ExtcRcSlab.hold（uint16/槽）；**只有 !NOCOUNT 才分配**：slabNew 819-821 | 精确 holder 数，饱和值 EXTC_RC_HOLD_SAT=65535（154） |
| holdUpd 的 5 步 | reclaim.c:1350-1362 | ① ownSlab(p)，非本运行时槽 ⇒ 返回（1347，义务 6）② occ 未置位 ⇒ 返回（1349，free 槽不是活对象）③ delta>0：hold!=SAT 则 ++，**到顶粘住**（1351）④ delta<0：hold==0 或 hold==SAT 直接返回（1354，"饱和值永不再减"，否则会把未知误当 0）⑤ -- 后到 0 ⇒ **当场 freeSlot**（1356，早收） |
| holdUpdSlab | reclaim.c:1336-1346 | 已解析 slab 的版本，判据逐条相同（省一次 ownSlab） |
| inc/dec | reclaim.c:1365-1366 | 只是 holdUpd(p,+1)/holdUpd(p,-1) 的别名 |

"三阶段饱和"的代码对应（义务 3 的实现）：hold == 0（无入边）→ 1..65534（精确）→ == 65535（饱和 = **计数未知**，粘住永不减）。饱和的唯一目的：绝不把"未知"当成 0 而提前释放；这类槽只能交给 sweep。

### 8.3 计数只在"指针赋值"上动

reclaim.c:1364 逐字："Counting hooks only pointer assignments: data-field stores do not go through these two."

| 入口 | 位置 | 对计数的作用 |
|---|---|---|
| initRef（新槽首次赋值） | 1478-1484 | 写值 + inc(v)；**从不读旧值**（义务 1） |
| initRefNoHold | 1487-1492 | 只写不增；调用点由 codegen 的 planRcNoDec 选出（codegen.c:7687/7678），其动态范围内没有 dec，故不增不会失衡 |
| storeRef | 1495-1503 | old=*slot；写；inc(v)；dec(old) |
| storeRefOwnedReg / Owned | 1520-1559 / 1588-1629 | NOCOUNT=0 分支：old/sold/sv 三次 ownSlab + 跨区登记 + holdUpdSlab(±1)（1541-1557） |
| 定向释放后的旧槽 | 2449-2452 注释 | 旧槽已 free，随后 storeRefOwned 的 rsDecSlab/holdUpdSlab 都会在 occ 行返回 ⇒ 不会二次结算 |

数据字段赋值不走 inc/dec（附加准则 reclaim.c:123-126）——所以计数**不跟踪** memset/结构体逐字节复制/外部 C 代码写入的引用；这类引用只能靠静态根表 + 跨区登记 + pin 覆盖。

### 8.4 与 sweep 的交互、何时计数不准导致泄漏

| 情况 | 结论 | 证据 |
|---|---|---|
| 计数与 sweep 的分工 | 标记/清扫**完全不依赖计数**：mark 从根槽 + 跨区种子下降，sweep 释放未标记占用槽；计数的唯一角色是"减到 0 立即回收" | reclaim.c:1319-1321 逐字 |
| 环 | 环内对象互相持有 ⇒ hold 不归零 ⇒ 早收永不发生 ⇒ 交给 sweep（设计如此） | 义务 3 后的边界注释 128-130：计数只在入边上动，slot 到 0 释放后它指向的目标要等发射器先丢引用；没丢的等 sweep 整段杀 |
| 计数偏高（漏 inc/漏 dec 的反面：多 inc） | 只会**晚收**；若该 region 又能 sweep 则无泄漏，只是内存峰值变高 | 与 1314-1316 一致 |
| 计数饱和（>65535 入边） | 该槽永不早收 ⇒ 同样交给 sweep | 1351/1354 |
| 计数偏低（多 dec / 漏 inc） | 会**提前释放**=unsound。防线：occ 检查（1333/1349）、义务 1（新槽不读旧值）、发射器的 planRcNoDec 判据 | 1333/1349、108-109 |
| region 是 noSweep / owe 长期未结 | 计数是**唯一**回收途径；计数不准（偏高）或成环 ⇒ **永久泄漏** | 2651、7.4 节 |
| 计数层的代价 | 文档线索 docs/plans/TODO.md 称"计数层比清扫贵 +100%~+455%"，本次**未复核**（见 13 节） | — |
| 性能判据（源码内） | TICK 调用点：alloc(0) 1056、storeRefOwnedReg(1) 1523、storeRefOwned(2) 1590、collect(3) 2767、slabNew(4) 778、freeSlot(5) 1246、regionNew(6) 1918；默认 EXTC_RC_TIME 关，全部编译成 do{}while(0)（156-160） | — |

## 9. 触发与预算：所有调用点、频率、单次停顿上界（问题 7）

### 9.1 调用点全表（内存回收的每一次推进）

| 入口 | 发射位置 | 执行频率 | 运行时动作 | 单步代价的上界 |
|---|---|---|---|---|
| 触发点（分配点**语句前缀**） | codegen.c:4267 rcAllocCore 内调 rcEmitTrigger；单 shard 文本 4138（{ if (extc_rc_shouldSweep(rr)) extc_rc_maybeCollect(rr, roots, n); }）；多 shard 文本 4169-4180 | **每个分配点每次执行**；先付一次 shouldSweep 判断，绝大多数不到压力线 | shouldSweep 为真才物化全根表（约 1412 项，见 4106 注释）并调 maybeCollect | 压力线以下：O(1)；以上见下三行 |
| maybeCollect 的标记路径 | reclaim.c:2783-2806（marking 未完成 ⇒ markAdvance） | 每次触发 | markAdvance：256 单位 | 见 9.3 |
| maybeCollect 的清扫路径 | 2785-2786：sweeping ⇒ sweepStep(r, 8) | 每次触发（sweeping 在飞时） | 8 word = 512 槽 | 512 槽 × 每槽撤销出边 |
| maybeCollect 的新轮 | 2788-2795：!marking && (!shouldSweep \|\| !roundOk) ⇒ -1；否则 step(r,roots,n) | 每次触发 | step：begin（若未 marking）+ markStep(stepBudget=[8,72]) + finish + 一次 sweepStep(8 word) | 72 单位 + 512 槽 |
| 分配点标记推进 | reclaim.c:1067（markAdvance） | 每 64 次分配（EXTC_RC_MARK_EVERY=902） | 256 单位 | 见 9.3 |
| 分配点清扫推进 | reclaim.c:1059-1065 | 每次分配（sweeping 在飞时） | sweepStep(8 word) | 512 槽 |
| owe 记录 | codegen.c:4024/4023/4033（{ if (extc_rc_shouldSweep(x)) extc_rc_owe(x); }） | 每个分配点前缀（不拥有该 region 时） | 只置 r->owe=1（2816） | O(1) |
| owe 结算 | codegen.c:4085/4085/4092（checkOwed） | 每个分配点前缀（拥有者、且 owed(r) 为真） | sweeping ⇒ 续扫 8；marking ⇒ markAdvance；信用够 ⇒ step | 同 step |
| **collect** | reclaim.c:2771-2780 | **无发射器调用**（grep 全 src：仅定义 + 注释；reclaim.h:58 只在文档里列名） | 排空标记（0xffffffff）+ settle（整段清扫） | 无界（整轮） |
| **sweep** | reclaim.c:2630-2634 | **无发射器调用**（同上） | sweepBegin+settle | 无界（整段） |
| **settle** | 2613-2623；被 2627（sweep）、2773（collect）、2851-2855（查询函数）调用 | 查询/销毁/诊断时 | 排空在飞标记 + 整段清扫 | 无界 |
| 查询函数 | reclaim.c:2856-2874（bytes/live/scale/sweeps/freedSlots/garbageHint/slabCount） | **garbageHint 是唯一被发射器引用的查询**（codegen.c:4179，仅多 shard 模式）；bytes/live/scale/sweeps/freedSlots/slabCount 无发射器调用（诊断/测试） | garbageHint 不算账（不 settle，2862-2864）；其余先 settle ⇒ 可能排空整轮 | garbageHint O(1)；其余无界 |
| region 释放 | codegen.c:7341-7358 cgReclaimDropArena（9970 帧尾声调用）；协程 6239/7339；并行 worker 14728/14786 | 帧销毁 / worker 结束 | regionFree（1970-2019）：遍历全部 slab、按 occ 逐活槽撤销跨区边、free 元数据 | **O(该 region 活槽数)**，无界 |
| 分片模式的触发点 | codegen.c:4174-4185 | 每个分配点每次执行 | 循环 N 个 shard：regionOf（线性扫 region 表）+ **regionFloor(shard, EXTC_RC_FLOOR/N)** + shouldSweep + garbageHint；最后收垃圾最多的一个 | O(N × region 表长(<=512)) 的**每次分配**开销；且每次分配都把该 shard 的 floor 重置为当前 bytes（regionFloor 1928-1931） |

### 9.2 触发条件公式

| 组成部分 | 代码位置 | 公式/常量 | 说明 |
|---|---|---|---|
| 压力线 | reclaim.c:2641-2647 thrUpd | thr = max(2 * live, floor) * max(scale,1)，溢出则 thr=0 | live = 上轮标记到的活字节（2731）；floor 由 regionNew 设 EXTC_RC_FLOOR=1 MiB（141、1917-1926）或 regionFloor 改写 |
| 触发判据 | reclaim.c:2655-2665 shouldSweep | noSweep ⇒ 0；EXTC_RC_SWEEP_FREE ⇒ 0；否则 (bytes > thr) && roundOk(r) | 注释：计数已收掉一切非环，占用≈活，故"占用 > 2×live×倍数"只在环真堆积时成立 |
| 轮信用 | reclaim.c:2651-2654 roundOk | allocBytes - roundVisitedBytes >= max(live, floor) | 与定向释放的 visitedBytes 信用**分开**（否则轮起会被饿死）；begin 时把 roundVisitedBytes=allocBytes（2748-2749）⇒ 开轮就花掉信用 |
| 自学习倍数 | reclaim.c:2605-2610 | 空扫（swFreed==0）⇒ scale = max(scale,1)*SCALE_MUL(4)，clamp SCALE_MAX=2^40；真收到 ⇒ scale=0 | 语义："连续空扫 = 这里没有环" ⇒ 阈值乘 4；真收到垃圾 ⇒ 立即回到 1 倍 |
| 收益判据（定向释放用） | reclaim.c:1943-1946 budgetOk | allocBytes - visitedBytes >= max(live, floor) | 花一次 O(live) 的定向遍历前必须先攒够这么多分配 |
| 根表过大 | codegen.c:4077/4128/4289/4308/4324 | 根数 > 4096 ⇒ rcRootsUnreliable=true | 之后 4198 直接不发射任何触发点 ⇒ **该函数完全不回收**（退化为泄漏，方向安全） |

### 9.3 "单次停顿真的有界吗"——常量与真实成本

| 步骤 | 名义预算常量 | 真实成本 | 是否有界 |
|---|---|---|---|
| 标记 walk | EXTC_RC_MARK_BATCH_MAX=256 单位 / 步（分配点）；stepBudget=[8,72]（触发点） | 1 单位 = 一次 MARK_SCAN：读该对象描述符的 nptr 个指针（nptr 为 uint32，描述符未限字段数）+ 同 slab 去重 | 否（≈256 × nptr；nptr 无上界约束） |
| 保守/大对象扫描 | EXTC_RC_SCAN_CHUNK=65536 word/段 | 每段 512 KiB 内存读过 | 是（常量 64K word） |
| 种子扫描 | EXTC_RC_SEED_WORDS=512 word + EXTC_RC_SEED_PUSHES=256 槽/步 | 512 word 位图 + 最多 256 次 push | 是 |
| 清扫 | EXTC_RC_SWEEP_BUDGET=8 word/步 | 每 word 最多 64 槽；整词批量 free 最多 64 槽，且每槽 unlinkChildren 读其 nptr 个字段 | 否（≈512 × nptr） |
| 根表物化 | 无预算 | 每次真正触发时把 ~1412 项（4106 注释；上限 4096）做成复合字面量传参 | 是（<=4096 项） |
| 批量 pop | EXTC_RC_MARK_BATCH_POP=200 | 200 次 pop + prefetch | 是 |
| finish/begin | O(1) | — | 是 |
| regionFree | 无预算 | O(该 region 活槽数) + 每槽 unlinkChildren | **否** |
| collect/settle/查询 | 无预算 | 整轮/整段 | **否**（但当前未接线，除 settle 在查询中） |

结论（必须写清楚，避免"有界"被误读）：
1. 增量路径的步数是有界的（常量乘积），但**每步的时间成本与对象的指针字段数 nptr 成正比，而 nptr 没有被运行时限制**。要给出形式化的停顿上界，必须把"扫描字段数"也纳入预算（当前 WORK_BUDGET 只在 markStep 内部计 walk/scan-word/push，sweep 侧的 free 成本完全不计）。
2. 真正无界的三处：regionFree（帧销毁）、collect/settle（已实现但未接线/仅查询）、以及 EXTC_RC_PAUSE 之外不可见的 memset/malloc 冷路径（slabNew 初始化 1 MiB + 三张位图 calloc）。
3. 若 EXTC_RC_SHARDS=N>1：触发点每次分配要做 N 次 regionOf（线性扫 region 表，最长 512）+ N 次 shouldSweep + N 次 garbageHint，且每次分配都重置 floor —— 这会**放大每个分配点的固定开销**并把停顿切成 N 份。
4. 默认参数下"漂移 x8 boost"永不生效（6.3 已证），所以停顿上界比代码注释暗示的更稳定；反过来，调小 MARK_EVERY/MARK_STEP 才会让 x8 生效。

### 9.4 WORK_BUDGET / PAUSE / TICK 三者的作用

| 开关 | 默认 | 作用 | 是否影响默认路径 |
|---|---|---|---|
| EXTC_RC_WORK_BUDGET | 0（reclaim.c:1012） | 0 = 三份预算独立叠加；W>0 = walk/scan-word/push 共用一个总数 wleft（2271），单步 <= W（1007 逐字"bounded by construction to <= W"） | 否（默认 0） |
| EXTC_RC_PAUSE | 无（env） | =1 时 pauseArm 注册 atexit(pauseDump)，退出打印 [clock]/[pause]/[step]/[histo]/[top]/[regionfree]/[chan]/[work]/[wbudget]/[worst]/[ubwork]/[guard]（`pauseDump`；行号随冻结版本漂移，以符号为准）。`=2` 及以上额外每步读 `/proc/self/stat` 取 minor fault 增量（`[top] … minflt=N`；读不到为 -1） | 否（只观测） |
| EXTC_RC_STEP_CPU | 无（env） | =1 时 `markStep`/`sweepStep`/`chanNote` 用 `CLOCK_THREAD_CPUTIME_ID`（线程 CPU 时钟）计时，`[clock]` 行标 `step=cpu chan=cpu`；默认墙钟。glibc 未暴露该 clock（如 strict `-std=c11` 未定义 `_POSIX_C_SOURCE`）时**静默降级为墙钟**并在 `[clock]` 打 `cpu=unavailable`（绝不打印 0） | 否（只观测） |
| EXTC_RC_M_MAX | 无（env，运行期 `getenv`+`atoll`） | 显式内存墙（字节）；未设时隐式 `3 × live`。`thrUpd` 夹紧 `thr = min(thr, M_max − live/4)`（`live/4 = 64·L/b`，b=256 字节/对象量级 ⇒ 常驻预算 M_max 下留 64 个对象的工作空间）；`M_max` 太小时退回 floor 并 stderr 报一次 `[mmax] memory wall unreachable` | 否（默认隐式墙生效；显式设置才改上界） |
| EXTC_RC_SLOWSTEP | 无（env） | 人为给每步加 us 级延迟（2719），用于把停顿放大到可观测 | 否 |
| EXTC_RC_TIME | 无（env；codegen.c:12406 判 getenv!=NULL，**设成 "0" 也开**） | 打开 TICK 分组件计时 + [rt:counters] 段 | 否 |
| EXTC_RC_STATS | 无（env） | 退出时打印 rs_inc/rs_dec/rs_live/rs_hwm/releases/release_slots（1568-1584 statsArm 1583） | 否 |
| EXTC_RC_FOREIGN_GUARD / SHADE_GUARD | 0 / 0（1159、1413） | 调试用运行期守卫（跨区旧值不变量 / shade 命中正确性） | 否 |

### 9.5 诊断口径（v2 实现，G1 判据用）

`[step]`/`[chan]` 的时长和 `[histo]` 都跟随同一个时钟源（`EXTC_RC_STEP_CPU=1` ⇒ 线程 CPU 时钟，否则墙钟；`[clock]` 行首字段写明 `step=`/`pause=`/`chan=` 三者**实际**来源，`pause` 恒为墙钟）。

`[histo]` 是逐桶计数的 log2 直方图（`EXTC_RC_STEP_NB = 15`，名字=桶**上界**且含等号）：`b1us b2us b4us b8us b16us b32us b64us b128us b256us b512us b1ms b4ms b16ms b64ms bge64ms`；`b64ms` 是 `(16 ms, 64 ms]`，`bge64ms` 是 `>64 ms`。细分桶是为了能直接读出 16–64 µs 区间与 1–64 ms 长尾，`[step]` 的 `over16/over64/over256/over1ms` 是**严格 >** 阈值的累计计数，由同一组桶累加（`stepSumFrom(5)/(7)/(9)/(11)`）。v17 起 `[histo]` 与 `[segsteps]` 还各打 `gt10us`/`gt50us`/`gt64us` —— 严格 `>10/>50/>64 µs` 的**精确样本计数**，判据线落在桶界之间时直接读它（旧产物无此三字段）。

`[chan]` 对三条同步通道各打一行（`name=regionfree|release|trigger-step`；v16 及更早那一格叫 `collect`，量的其实是触发路径上的一次 step，v17 起改名，解析器两名字都收，字段 `n/max_us/total_us/work_max/work_sum` + 15 桶）；`[top]` 给最慢的 8 步（`kind/why/dur_us/cpu_us/freed/words/seg/pops/coll/step/first/trans/minflt/chunk`，mark 步的 `seg` 是续扫过的 word 数，`chunk` = `EXTC_RC_SCAN_CHUNK`）。

**量具陷阱（已入源码兜底）**：生成物**不定义** `_POSIX_C_SOURCE`；strict `-std=c11` 编译时 glibc 不暴露 `CLOCK_THREAD_CPUTIME_ID`/`CLOCK_MONOTONIC`，`extc_rc_cpuNow()` 返回 -1，若直接相减会得到 0 ⇒ 所有时长显示 0、G1「无超时」假 PASS。现在 `extc_rc_cpuClock()` 首用探测，`cpuOk=0` 时强制走墙钟并在 `[clock]` 打 `cpu=unavailable`。实测（`/tmp/ring_med.extc` 50×3000）：strict c11 ⇒ `[clock] step=wall pause=clock chan=wall cpu=unavailable`、`[step] n=208 max_us=14.0`；`-D_POSIX_C_SOURCE=200809L` ⇒ `[clock] step=cpu pause=wall chan=cpu`、`[step] n=208 max_us=21.6`（两档数字都是非零的**真实值**，口径见 `[clock]`）。

### 9.6 regionFree 的成本口径：free() 次数不是停顿上界（可引用结论）

- `extc_rc_regionFree` 是**同步批量 free**：abort 只取消在飞的轮次、不排空；随后逐 slab 释放元数据
  （occ/mark/rsbit/hold/rscnt/tid/ttab/thash）、巨块 payload、slab 本身，另加每 `ExtcRcClass` 一次、
  `mstk` 一次、`cstk` 一次、region 一次。**free() 调用次数上界 ≈ `10·#slabs + #classes + 3`**
  （cstk 计入则 +1）。它**不扫描活对象图**；唯一随活对象增长的是 `rsIncN≠0` 时的 `unlinkChildren`
  （O(占用槽 × 出边)）与 `pinDropRegion`。
- **`free()` 次数不是停顿上界**：成本主要由被 unmap 的映射页数决定。实测（`build-rf`，默认档，
  `EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`）：
  - pd 形状（`tests/gc-p0/src/pd_bigobj.extc 200`，200 次 8 MiB 大对象逐次 drop）：
    `[regionfree] max_frees=1331 max_slabs=133`、`[chan] regionfree max_us=18561.3–22673.2`
    ⇒ 约 **14–17 µs/free**，由 8 MiB payload 的页表拆除主导。
  - pb 形状（`tests/gc-p0/src/pb_escape_grow.extc 100000000`，退出前一次大 drop）：
    `[regionfree] max_frees=220377 max_slabs=24486`、`[chan] regionfree max_us≈167746.7`
    ⇒ 单次 drop ≈ **167.7 ms**，是真实的**无界停顿通道**；P0-2 的分批 drop 明确不做，因此判据只能
    要求这类 drop 只出现在帧销毁/进程退出（G3）。
- 推论：G3 的判据必须是 `max_frees ≤ 10·max_slabs + #classes + 3` **并且** drop 出现在帧销毁/进程
  退出；不能把"free 次数少"当成"停顿有界"。
## 10. 线程语义（问题 8）

### 10.1 有保护的共享状态

| 结构 | 代码位置 | 保护方式 |
|---|---|---|
| slab 登记表（owns 的基础） | reclaim.c:369-675；锁 397-402（__atomic_exchange_n acquire / store release 自旋） | 写侧（slab 创建/释放）在 extc_rc_regLock 下串行；读者无锁：读当前代指针 (tab,cap,n) 自洽；增长建**新代**，**旧代永不释放**（别的线程可能仍在探测）；ownFind 探测 miss 后再读 extc_rc_regSeq，变了就重试，32 次后加锁复查（540-556） |
| _Thread_local ownHot | reclaim.c:598-628 | 每线程缓存最近一次 slab；命中需校验 k<nslots && k*ss==off |
| ownMap[4096] | reclaim.c:412-413 | 进程级直接映射缓存，命中需校验基址/stride/offset；写侧在锁下 |
| pin 表 | reclaim.c:1639-1755 | 进程级 + 自旋锁（pinLockAcq 1649）；热路径永不碰 |
| extc_rc_wb（屏障参与者计数） | reclaim.c:921、928-930 | __atomic_fetch_add/sub（noinline 冷函数，避免热路径每次重载） |
| extc_rc_regSeq | 见上 | 原子读写 |

### 10.2 没有保护的状态（关键）

以下全部是**普通字段、无原子、无锁**：region->bytes / allocBytes / live / thr / scale / marking / sweeping / bump / freelist / slabs / seedsDone / wbDebt；slab->occ / mark / rsbit / hold / rscnt / tid / freelist / bump；shade 缓存（wb 之外）。⇒ **同一个 region 在同一时刻只能被一个线程分配、赋值、标记、清扫**。

运行时靠三条约定成立，而不是靠锁：
1. region 由 **arena 地址**认领（codegen.c:12408-12414 注释 + regionOf 12423-12433）；每个 worker 有**私有 arena**（codegen.c:14723-14740 extc_par_open 为每个 worker 建一个 arena；14743-14750 worker 里 extc_tls_arena 指向自己的那份）。
2. worker 线程序言保存并把 extc_rc_tlsDepth 清 0（codegen.c:14783-14791），使 worker 入口帧成为 owner；尾声恢复（14770-14772）。TLS owner 判据在触发点做运行期三分支（codegen.c:4227-4235：if (__extc_tlsOwner) 触发 else owe）。
3. 只有 owner 帧才触发 sweep；内层帧只记 owe（[rt:tlsdepth] 2081-2089 + owe 段 2803-2849）。

### 10.3 已实现代码里的线程隐患（未验证）

| 隐患 | 位置 | 说明 |
|---|---|---|
| region 表无锁 | codegen.c:12425-12454（extc_rc_regKey/regVal/regN 是普通 static 数组，regionOf/regionDrop 无锁） | 若两个 worker 线程同时首次分配（regionOf miss ⇒ regionNew + 写表）或同时帧结束（regionDrop），会数据竞争。当前并行运行时每个 worker 用**自己的** arena 与 region，但首次 regionNew 仍写同一张全局表 ⇒ **理论上有竞争窗口**；是否真的会并发取决于 extc_par_open 是否预建 region（未验证，见 13 节） |
| noSweep/owe 字段跨线程写 | reclaim.c:1766/1821、2816 | 若一个线程向别的线程拥有的 region 写入 storeRefUnknown/storeStructCons，会无锁写 noSweep |
| pin 表项 rg 缓存 | reclaim.c:1695-1714 | 加锁维护，但 rg 指向的 region 可能已被别的线程 regionFree（pinDropRegion 在 regionFree 里 1971-2019 调用，加锁）⇒ 有处理 |
| 结论 | — | "多线程下安全"的准确表述：**每个 region 单线程独占时安全**；共享 region（例如把同一 home arena 传给多个 worker）不安全，且运行时没有检测。是否被语言/编译器的并行规则禁止：未验证 |



## 11. [rt:head] 的 7 条 soundness 义务（逐字抄录 + 解释）（问题 9）

出处：src/back/reclaim.c:106 标题 "Soundness obligations (missing one dangles, so these are not optimizations)"。以下英文为**逐字**抄录（含原断行合并），解释为本文件的分析。

**义务 1**（reclaim.c:108-109）
"A fresh slot and a slot whose old value is overwritten are two operations. A fresh slot holds the next free slot number; dereferencing that as an old pointer crashes; the compiler knows statically which it is, so there are two entry points: initRef only writes and bumps the target count, storeRef reads the old value."
解释：新槽的 payload 里可能是"下一个空闲槽号"（复用链值存在 tid[]，但旧实现/其他状态同样不可读），把它当指针读会崩。因此赋值分两个入口：initRef（1478-1484，只写 + inc）与 storeRef（1495-1503，读旧值 + dec）。codegen 必须静态判定用哪个（codegen.c:4612/7299/7469/7676/7678）。违反 = 读未初始化内存/野指针。
对应的实现约束：initRefNoHold（1487-1492）只在"动态范围内无 dec"的证明下使用（planRcNoDec）。

**义务 2**（reclaim.c:110-111）
"The registration bitmap is persistent and deduplicated; the sweep does not clear it. Clearing it forgets cross-region in-edges, and the next round frees objects that another region still points at."
解释：跨区登记位（rsbit）不是"每轮清零的种子集"，而是由 rscnt 精确入边计数维护的**持久**状态（0→1 置位、1→0 清位，见 [rt:xregion] 1130-1137）。sweep **不得**清它（sweepWord 只清 mark[w]，2556-2566），否则下一轮把别的 region 还指着的对象当垃圾。注意与 [rt:xregion] 的"1→0 清位"并不矛盾：清位只由计数撤销（rsDec/freeSlot/regionFree）做，sweep 不做整体清空。违反 = 跨 region 提前释放。

**义务 3**（reclaim.c:112）
"Counts saturate in three stages, and the saturated value means unknown. At the ceiling the count sticks and never decreases again, so it is never mistaken for zero; hand those to the sweep."
解释：hold/rscnt 都是 uint16，饱和值 EXTC_RC_HOLD_SAT=65535（154）。三阶段 = 0（无入边）/ 1..65534（精确）/ 65535（饱和 = 未知，粘住永不减，1351/1354、1179/1195）。饱和后**不能**再减：一旦减到 0 就会把"未知"误当成"无入边"而提前释放（unsound）。这类槽交给 sweep。违反 = 提前释放。

**义务 4**（reclaim.c:113-114）
"Marking walks pointer fields with the object's own type descriptor. The allocation site hands the descriptor to the runtime with the slot (each slot stores one pointer); walking with the wrong descriptor, or treating a missing one as present, misses marks."
解释：每个槽存一个 16 位类型号（tid），指向 slab 的 ttab（按指针相等去重）；标记严格按 ExtcRcType{nptr, off} 给出的字段偏移读指针（MARK_SCAN 2205-2237）。用错描述符、或把"没有描述符"（tid==0）当成有描述符，都会漏标 ⇒ 漏标 = 活对象被当垃圾扫掉。typetable 满（0xFFFF）时 oops 而不是静默退 0（695-696 注释），正是这条义务的实现。
保守例外：CONSERVATIVE（209）只用于 alloc<T>(n) 且 T 含引用的动态元素偏移，整槽按 word 扫，只会多留不会漏。

**义务 5**（reclaim.c:115）
"Releasing a slot must clear its registration bit. Otherwise a new object reusing the slot looks live through the stale entry and is missed."
解释：freeSlot 必须清 rsbit（1255，freeSlotWord 同样）与 rscnt（1257 附近）以及 hold（1256-1258）；否则槽被复用时，陈旧的登记位会让新对象"看起来有活跨区入边"，从而在需要被当种子时行为错乱 / 或被误判为活（漏收）。注意它**不**要求清 mark 位（mark 只由 sweep 按 word 顺序清）。

**义务 6**（reclaim.c:116-118）
"Only slots allocated by this runtime may be counted/registered/marked. A pointer from a plate/pool/stack/C allocation is not such a slot; the bitwise AND in slabOf lands in unrelated memory, which segfaults. Every slab is entered at creation into a table sorted by base address, and count/register/mark first ask owns; a slot absent from the table (or already free) is skipped entirely."
解释：slabOf 只是一次按位与（339-340），对非 slab 指针会把无关内存当 slab 头读，直接 segfault（reclaim.h 特别提到 plate 的 4 GiB 预留甚至未提交）。因此 slab 创建即登记进按基址组织的表（ownAdd 841、[rt:slab] 369-675），inc/dec/register/mark 都先问 owns/ownSlab；表外或 occ 已清的槽**整段跳过**。这也解释了为什么注释强调注册表用哈希+按代发布而非排序数组：整区释放若用排序数组会变成块数平方（369-395）。

**义务 7**（reclaim.c:119-121）
"When the host cannot be proved, the target region must leave the sweep. The cross-region registration sits on the target and the source object does not know it, so it cannot be withdrawn; the static root table has no such slot either. Then mark the target's region as not swept (rather collect less), and never sweep as usual with incomplete seeds -- the entry point is extc_rc_storeRefUnknown."
解释：跨区入边登记**挂在目标槽上**，源对象（宿主）不知道自己被登记；一旦宿主不可追踪，这条边既无法撤销、也不在静态根表里 ⇒ 该 region 的种子集不完整 ⇒ sweep 会把仍被引用的对象当垃圾。唯一安全动作：把目标 region 置 noSweep（1761），宁少收不可错杀。所有 noSweep 相关路径都在执行这条义务（1063/2621/2651/2785/2825/2829/2509）。pin 机制（1631-1750）是它的"更好版本"：把值本身钉成根，从而**不**必让整个 region 退出 sweep。

**两条附加准则**（reclaim.c:123-126）
1. "marking stops at an object outside the region and does not write its mark bit -- an object of another region left marked makes its next sweep skip it and free its children. Anything outside the region counts as live."（124-125）
2. "assigning a data field calls neither inc nor dec; only a change of reference touches a count."（126）

**一条边界**（reclaim.c:128-130）
"counts move only on in-edges. When a slot reaches zero and is released, the targets it pointed at reach zero only after the emitter drops them before the object dies (clear the field / unlink the chain); counts not dropped wait for the sweep to kill that section as a whole. This is on the rather-collect-late side: a dead object only keeps others alive a little longer; it never dangles."
解释：freeSlot（NOCOUNT=0 的早收）**不**递减它指向的对象的 hold（128-130 明说"reaches zero only after the emitter drops them before the object dies"）；所以死对象的子对象可能被多留一段时间，等 sweep 整段回收。方向始终是"晚收不悬垂"。

## 12. 开关与常量总表

### 12.1 环境变量 / 编译器开关（改变"发射什么"）

| 开关 | 类型 | 默认 | 定义/读取位置 | 作用 |
|---|---|---|---|---|
| EXTC_RECLAIM | env | 开（未设=开） | codegen.c:13772-13774："The reclamation switch: on by default, EXTC_RECLAIM=0 turns it off, and a nonzero value forces it on" | =0 关闭整个回收：分配点改走 extc_arena_alloc（4622-4640 的 else 分支），不发射运行时文本；**不是纯 GC 税对照**（连分配器都换成纯 bump） |
| EXTC_RC_NOCOUNT | env | **1（无计数）** | codegen.c:13781 默认 1；=0 时 13792-13795 置 0；EXTC_RC_SWEEPFREE=1 也强制 0（13810-13813） | =1 ⇒ 计数层整体编译掉；=0 ⇒ 恢复 hold 计数 + 归零即收 |
| EXTC_RC_SWEEPFREE | env | 三态（未设则由 unitAllAcyclic 决定；=1 强制开；=0 强制关） | codegen.c:13821-13824；运行时 shouldSweep 2652-2655 直接返回 0 | 开 = "整程序证明无环"⇒ 不做 sweep（早收已收干净） |
| EXTC_RC_LOCALSTORE | env | 1 | codegen.c:13789-13797 | =0 关闭 no-count 层的局部槽 write-only store（extc_rc_storeRefLocal，2978-2987） |
| EXTC_RC_SHARDS | env | 1，上限 64 | codegen.c:13775-13788 | 分片数 N；floor 按 N 除（4172），每次触发只收垃圾最多的 shard |
| EXTC_RC_TIME | env | 无（getenv != NULL 即开，**设成 "0" 也开**） | codegen.c:12406；运行时 156-184、[rt:counters] 162-186 | 打开 TICK 分组件计时 + 计数器段 |
| EXTC_RC_PAUSE | env | 无 | 运行期 `extc_rc_pauseArm` / `extc_rc_pauseDump` | =1 打印停顿统计（[clock]/[pause]/[step]/[histo]/[top]/[regionfree]/[chan]/[work]/[wbudget]/[worst]/[ubwork]/[guard]）；=2 另读 /proc/self/stat 的 minor fault 增量 |
| EXTC_RC_STEP_CPU | env | 无 | 运行期 `extc_rc_pauseArm` / `extc_rc_cpuClock` | =1 时 [step]/[histo]/[chan]/[top] 用线程 CPU 时钟（滤掉调度噪声）；clock 不可用（strict c11 未定义 _POSIX_C_SOURCE）时降级为墙钟并在 [clock] 打 cpu=unavailable |
| EXTC_RC_M_MAX | env | 无（隐式 3×live） | 运行期 `extc_rc_mmaxArm`/`extc_rc_mmaxEff`/`extc_rc_thrUpd` | 显式内存墙（字节）：thr 被夹到 M_max − live/4，常驻预算 ≤ M_max + 缓冲；[work] 打 M_max/thr/M_max_implicit |
| EXTC_RC_SLOWSTEP | env | 无 | reclaim.c:2724 | 人为加 us/步，放大停顿便于观测 |
| EXTC_RC_STATS | env | 无 | reclaim.c:1573-1589（statsArm 1583） | 退出时打印 rs_inc/rs_dec/rs_live/rs_hwm/releases/release_slots |

### 12.2 编译期常量（全部 #ifndef，可 -D 覆盖；默认值即"默认路径"）

| 常量 | 默认 | 行 | 作用 | 影响停顿/内存 |
|---|---|---|---|---|
| EXTC_RC_SLAB_BITS | 20 | 138 | slab = 2^20 = 1 MiB，slabOf 一次按位与 | slab 是内存归还的最小单位（永不归还） |
| EXTC_RC_FLOOR | 1<<20（1 MiB） | 141 | region 初始压力地板；分片时除以 N | 内存下界；也是 roundOk/budgetOk 的下限 |
| EXTC_RC_SCALE_MAX | 1<<40 | 144 | 空扫倍数上限 | 阈值上限 ≈ 2^40 × max(2live,floor) ⇒ 事实上等于"不再回收" |
| EXTC_RC_SCALE_MUL | 4 | 151 | 连续空扫一次乘 4 | 阈值增长步长 |
| EXTC_RC_HOLD_SAT | 65535 | 154 | 计数饱和值 | 计数层未知态 |
| EXTC_RC_SLAB_SIZE | 1<<SLAB_BITS | 316 | 1 MiB | 对齐/按位与 |
| EXTC_RC_SLAB_HDR | (sizeof(ExtcRcSlab)+63)&~63 | 317 | slab 头 64 对齐 | data 起点 |
| EXTC_RC_SLAB_CHUNK_MIN | 64 KiB | 321 | slab 首次 chunk 下限 | 小对象 slab 的粒度（倍增到 1024 槽） |
| EXTC_RC_SLAB_SLOTS_MIN | 1024 | 322 | 每 slab 最少槽数 | slabNew 摊还 |
| EXTC_RC_REUSE_MAX_SLOT | 0xFFFE | 325 | 复用链 next 编码上限（tid 里存 slot+1） | 超大 slab（>65534 槽）直接 oops |
| EXTC_RC_RCP_SHIFT | 42 | 347 | 倒数乘移位宽度 | 槽号 O(1) |
| EXTC_RC_CLS_BUCKETS | 32 | 211 | 大小类哈希桶数 | 分配冷路径 O(1) |
| EXTC_RC_OWNMAP_BITS | 12（4096 项） | 412-413 | owns 直接映射缓存 | 分配热路径 |
| EXTC_RC_TTAB_LINEAR_MAX | 8 | 702 | slab 内类型表线性扫上限 | 标记/分配常数 |
| EXTC_RC_SWEEP_BUDGET | 8 word | 889 | 每次 sweepStep 的 word 数 | 停顿（512 槽） |
| EXTC_RC_SEED_PUSHES | 256 | 896 | 每步种子 push 上限 | 停顿 |
| EXTC_RC_SEED_WORDS | 512 | 899 | 每步种子 word 上限 | 停顿 |
| EXTC_RC_MARK_EVERY | 64 | 902 | 分配点每 64 次分配推进标记 | 频率 |
| EXTC_RC_MARK_WINDOW | 2<<20（2 MiB） | 905 | 漂移窗口，超过则 x8（默认参数下不生效，见 6.3） | 停顿 |
| EXTC_RC_MARK_STEP | 8 | 908 | stepBudget 基数 | 停顿 |
| EXTC_RC_MARK_BATCH_MAX | 256 | 911 | walk 批上限 | 停顿 |
| EXTC_RC_MARK_BATCH_POP | 200 | 914 | 批量 pop 上限 | 停顿 |
| EXTC_RC_ASSIST_CAP | 512 | 999 | assist 单步上限 | 停顿 |
| EXTC_RC_WORK_BUDGET | 0（关） | 1012 | >0 时三份预算合并为一个总数 | 停顿的形式化上界开关 |
| EXTC_RC_ALLOC_INLINE | 1 | 1048 | alloc 强制内联到每个分配点 | 代码尺寸/速度 |
| EXTC_RC_FOREIGN_GUARD | 0 | 1159 | 跨区旧值运行期守卫 | 仅调试 |
| EXTC_RC_SHADE_GUARD | 0 | 1413 | shade 缓存正确性守卫 | 仅调试 |
| EXTC_RC_MARK_SHALLOW | 64 | 2131 | 浅栈直通深度 | 停顿 |
| EXTC_RC_SCAN_CHUNK | 65536 word | 2147 | 大对象分段扫描段长 | 停顿（512 KiB/段） |
| EXTC_RC_REG_MAX | 512 | codegen.c:12426 | region 表容量；满则新 region 不登记（泄漏 region） | 内存（表外 region 永不 regionFree） |
| EXTC_ARENA_SPARE_MAX | 1<<20 | codegen.c:12137 | arena release 时保留的 spare block 上限 | 每 arena 至多留 1 MiB |


## 13. 未验证项 / 存疑清单

| # | 条目 | 现状 | 需要的证据 |
|---|---|---|---|
| 1 | 计数层比清扫贵 +100%~+455% | 来自 docs/plans/TODO.md 的断言，**本次未复核实测** | 用 EXTC_RC_TIME=1 + 同一基准在两档 NOCOUNT 下跑分 |
| 2 | "plate 的 4 GiB 预留" | reclaim.h 的注释提到 plate 指针可能落在未提交的 4 GiB 预留里；但 src/back/plate.c（74 行）**只有 extc_viewOf 与 extc_memCopy，没有任何 mmap/预留代码** | 找到真正的 plate 内存来源（可能在 extC 库层或另一仓库），否则该断言来源不明 |
| 3 | arena 是否"越界即 trap" | 源码显示 extc_arena_alloc 不越界：容量不够就新 malloc 一块（cap 倍增、上限 1 MiB 后不再倍增），唯一 trap 是 OOM（codegen.c:12162 "trap: out of arena memory"） | 若 Lead 的已知事实写"越界即 trap"，需按此修正 |
| 4 | 多线程 region 表竞争是否真会发生 | regionOf/regionDrop 操作的是无锁 static 数组（codegen.c:12425-12454）；extc_par_open（14706-14715）只建 arena 不建 region ⇒ 首次分配时每个 worker 都可能并发写表 | 并行测试 + TSan；或确认并行路径是否预建 region |
| 5 | 停顿的真实分布 | **部分已解决**：v2 的 `[step]`/`[histo]` 提供 15 桶直方图 + `[top]` 最慢 8 步（含 `freed/words/seg/pops/minflt` 归因），可算 p99/p99.9；但本项目**没有采集到全语料的数字**（不在本文件范围，见 docs/analysis/MEASUREMENTS-GC-P0.md）；且 `[top]` 只存 8 条、不是完整分布 | 用 `EXTC_RC_PAUSE=1`（或 `PAUSE=2` 加 minflt）跑语料，按 `[histo]` 出 p99/p99.9 与 over1ms |
| 6 | "三阶段饱和"的第三种解读 | 代码只体现 0 / 精确 / 饱和三态；"three stages"是否另指设计阶段，**属推断** | 设计文档/提交记录（无源码证据） |
| 7 | noSweep 的真实触发频率 | 未统计；取决于 storeRefUnknown / storeStructCons(hostKnown=0) 在测试中的出现次数 | 在 EXTC_RC_STATS 或加日志统计 noSweep 置位次数 |
| 8 | 分片模式 regionFloor 每次分配重置 floor | 代码事实（4172 在触发循环内每次调用）；语义是否符合设计意图**存疑**（注释未说明） | 设计意图文档 |
| 9 | EXTC_RC_TIME 设成 "0" 也开 | codegen.c:12406 用 getenv(...)!=NULL 判断，属代码事实；是否 bug 存疑 | 约定文档 |
| 10 | markAdvance 的 x8 boost 默认永不生效 | 由 512 > MARK_BATCH_MAX(256) 推出，属**代码推论**（常量关系）；是否有意留调参余量存疑 | 设计意图 |
| 11 | slotSize 注释与实现不一致 | 注释 677-678 说"至少够 8 字节 free 链头"，实际最小槽 16（688-690）且复用链值存在 tid[] 而非 payload（1238-1243） | 只需记录，不影响正确性 |
| 12 | reclaim.c 实际行数 | 任务给的是 2889 行，实测 **2988 行**（wc -l） | 以文件为准 |
| 13 | 逐字比对覆盖面 | 只对 5 个段做了字节等价比对（types/helpers/tlsdepth/release/owe），其余段因发射期 #if 折叠未逐字比（只验证段存在与语义对应） | 可对每段做"折叠后文本"等价比对（工作量大） |
| 14 | region 表满时的行为 | 代码事实：表满不登记（12405-12407 注释 "leaking a region is preferred over freeing it early"）；但泄漏量级未测 | 构造 >512 region 的程序 |
| 15 | 编译器侧 RcOwn 三态（YES/NO/TLS）的判定完整规则 | 本文件只覆盖触发发射的表层；判定规则在 codegen.c:4202 之外（RcOwn 计算处）未逐一核验 | 见 codegen 分析（其他 teammate 的文档） |

## 附录 A "源码即产物"的逐字比对（实测）

**命令**（在当前仓库根执行）：

    ./build/extc tests/reclaim-shapes/cyclic_shape.extc -o /tmp/rt-check.c

- 退出码 0；产物 3417 行。
- 产物第 13-15 行（逐字）：

        #define EXTC_RC_NOCOUNT 1
        #define EXTC_RC_TIME 0
        #define EXTC_RC_SWEEP_FREE 0

  与 reclaim.c:2919-2921 的三条 bufPuts 一一对应（NOCOUNT 默认 1 在产物里直接可见）。
- 产物 61-62 行（逐字）：

        #ifndef EXTC_RC_SLAB_BITS
        #define EXTC_RC_SLAB_BITS 20

**方法**：从 reclaim.c:92-2993 逐行用正则 ^ 空白 \* "(.*)" 空白 ,? 空白 $ 提取 C 字符串字面量并反转义（\n \t \" \\ \0 \r \xNN 六种），按 [rt:name] 切成 17 段，再在产物中查找"该段全文"，判字节相等。

| 段 | 源码起点 | 段长（字节） | 产物行区间 | 结果 |
|---|---|---|---|---|
| [rt:types] | reclaim.c:187 | 12189 | 84-228 | **EXACT（字节完全相等）** |
| [rt:helpers] | reclaim.c:331 | 2505 | 228-266 | **EXACT** |
| [rt:tlsdepth] | reclaim.c:2086 | 984 | 1837-1846 | **EXACT** |
| [rt:release] | reclaim.c:2437 | 6704 | 2174-2272 | **EXACT** |
| [rt:owe] | reclaim.c:2808 | 5260 | 2538-2611 | **EXACT** |
| [rt:counters] | reclaim.c:162 | — | 产物中不存在 | 整段不发射（rtSectionOn 40-50：仅 timeOn） |
| [rt:chain] | reclaim.c:2049 | — | 产物中不存在 | 整段不发射（仅 frameRoots；codegen 传 0） |

- 其余段（head/slab/budget/xregion/nocount/shade-cache/xentry/pin/mark/sweep）在产物中**存在但非逐字相等**：因为发射器把可判定的 #if EXTC_RC_NOCOUNT/TIME/SWEEP_FREE 分支折叠掉了（rtCfgIf 67-88）。例：[rt:head] 的首个差异在产物第 16 行，正是 158-184 的 #if !EXTC_RC_TIME 把 TICK 定义折叠所致。
- 逐字样例（产物 236-238 行 = reclaim.c:339-340）：

        static inline ExtcRcSlab *extc_rc_slabOf(void *p) {
            return (ExtcRcSlab *)((uintptr_t)p & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1));
        }

- 逐字样例（产物 1191 行起，[rt:nocount] 段注释 = reclaim.c:1322）：

        **EXTC_RC_NOCOUNT=1 is the default: the counting entries are compiled out entirely** --

  紧随其后的 extc_rc_holdUpdSlab / holdUpd / inc / dec 在产物里都是空函数体（对应 reclaim.c:1325-1332），证明"默认关闭计数"不是文档口号，而是**产物里可读的代码**。

结论：运行时字符串表与产物**逐字节一致**（在未被 #if 折叠的段上 5/5 全等，缺失段与折叠段的原因都能由发射器代码解释）。所以本文件对 reclaim.c 的引用可以等价于对生成 .c 的引用。

