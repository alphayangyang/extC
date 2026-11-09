# REFS-V4：MEMORY-MODEL-CURRENT.md 中 reclaim.c 行号引用的 v4 权威映射

> 本表只做一件事：把 `docs/analysis/MEMORY-MODEL-CURRENT.md` 里每一个 `reclaim.c:NNN` 引用，按语义重新定位到冻结版源码的权威行号，并给出可复核的源码摘录。
> 不做偏移机械平移：文档行号混杂多个年代，机械平移会指错函数（例如把 `thrUpd` 指到 `markResume`）。

## 0. 冻结口径（唯一有效）

| 对象 | 冻结值 | 校验 |
|---|---|---|
| `src/back/reclaim.c` | md5 `ff42799bfa62c2c9b0ac5b238e7d05e9`，3607 行（262464 B） | `md5sum` + `wc -l` |
| 编译器 | `build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f` | `md5sum` |
| 被映射文档 | `docs/analysis/MEMORY-MODEL-CURRENT.md` md5 `aa4776dfa2899f62a72e0b40dab169f0`，980 行 | `md5sum` |

本表所有 依据 列都可用 `sed -n 'Np' src/back/reclaim.c` 取出源码字符串行；去掉行首缩进、引号与行尾 `\n` 后即为该行运行期 C 文本。

## 1. 读表须知：文档行号有三种错误形态

| 形态 | 例（文档原文） | v4 真值 | 判别 |
|---|---|---|---|
| A. 首数已是 v4、次数是旧版行 | `initRef reclaim.c:1815-1484` | 1815-1821 | 首数可直接用，次数是旧版行，勿当结尾 |
| B. 两数都是旧版（整段迁移过） | `markBegin reclaim.c:2098-2109` | 2503-2514 | 必须按标识符重新定位 |
| C. 首数是 v4、次数是旧版（常见） | `shade reclaim.c:1791-1462` | 1791-1799 | 首数对；同段 `shadeOld:1468-1475` 指向 1805-1812 |

置信度：`high` = 依据行逐字含该语义且命名唯一，可直接替换；`medium` = 段定位正确但边界有 ±几行不确定；`low` = 需人工判定（本版无 low）。


## §5 rc region（5.1–5.5）

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 237 | `5.2 slab 几何（`reclaim.c:777-857`）` | slabNew 定义（文档 777 是旧版注释起点） | **782-853** | `v4:782` __attribute__((unused)) static ExtcRcSlab *extc_rc_slabNew(ExtcRcRegion… | high |
| 241 | `slab 对齐与总尺寸 \| `EXTC_RC_SLAB_BITS = 20` ⇒ `EXTC_RC_SLAB_SIZ…` | slab 对齐、总尺寸、分配调用 | **138（SLAB_BITS）/ 321（SLAB_SIZE）/ 801（aligned_alloc）** | `v4:138` #define EXTC_RC_SLAB_BITS 20<br>`v4:801` s = (ExtcRcSlab *)aligned_alloc(EXTC_RC_SLAB_SIZE, EXTC_RC_SLAB_SIZE); | high |
| 242 | `头部 \| `EXTC_RC_SLAB_HDR = align64(sizeof(ExtcRcSlab))` \| `…` | SLAB_HDR | **322** | `v4:322` #define EXTC_RC_SLAB_HDR  ((sizeof(ExtcRcSlab) + 63) & ~(size_t)63) | high |
| 243 | `槽步长 `ss` \| 请求长度向上取 8 的倍数，**非 2 的幂** \| `reclaim.c:1074（`sl…` | 槽步长 ss 计算 | **1309（alloc 内 slotSize 调用）/ 692-696（定义）** | `v4:1309` size_t ss = extc_rc_slotSize(n); | high |
| 244 | `块初始尺寸 \| `chunk = EXTC_RC_SLAB_CHUNK_MIN = 64 KiB`，当 `chunk…` | chunk 64KiB 起、按 1024 槽倍增 | **326-327（常量）/ 788-789（倍增循环）** | `v4:326` #define EXTC_RC_SLAB_CHUNK_MIN ((size_t)64 * 1024) | high |
| 245 | `槽数 \| `cap = (chunk - HDR) / ss`；`cap < 2` ⇒ **huge block**…` | cap<2 时巨块：头照建、payload 单独 malloc、nslots=1 | **791-799（huge block 分支）** | `v4:791` int big = (cap < 2);   /* fewer than two fit, so the payload is malloc'… | high |
| 246 | `位图 \| `occ`、`mark`、`rsbit` 各 `nwords = (nslots+63)/64` 个 `u…` | 三张位图 | **819-821（occ/mark/rsbit calloc）** | `v4:819` s->occ   = (uint64_t *)calloc(s->nwords, sizeof(uint64_t)); | high |
| 247 | `每槽元数据 \| `tid`（2 B，类型号）总是 `malloc`；`hold`（2 B 计数）**仅 NOCOUN…` | 每槽元数据 | **825（hold 仅 !NOCOUNT）/ 827-831（tid/ttab）/ 1403-1407（rscnt 懒建）** | `v4:825` s->hold  = (uint16_t *)malloc(nslots * sizeof(uint16_t)); | high |
| 248 | `类型表 \| `ttab` 容量 8，`ttab[1] = EXTC_RC_CONSERVATIVE`（号 1 固定是…` | ttabCap=8、ttabN=2、calloc、ttab[1]=CONSERVATIVE | **828-831** | `v4:828` s->ttabCap = 8; | high |
| 249 | `复用链 \| `freelist` 初始 -1；**不预链全块**，"未发放的槽走 bump，只有被释放的槽进 fre…` | freelist=-1、bump=0 | **844-845** | `v4:844` s->freelist = -1;   /* empty head: the chain's next slot number lives i… | high |
| 250 | `槽号上限 \| `EXTC_RC_REUSE_MAX_SLOT = 0xFFFE`，超了 `oops`（复用链编码上限…` | REUSE_MAX_SLOT 0xFFFE 与 oops 守卫 | **330（常量）/ 812-813（守卫）** | `v4:330` #define EXTC_RC_REUSE_MAX_SLOT 0xFFFEu | high |
| 252 | `**为什么"1 MiB 对齐"重要**：从指针反查 slab 只需一次位与（`EXTC_RC_SLAB_DATA`/`…` | 一次位与反查 slab | **334（SLAB_DATA）/ 344-346（slabOf）** | `v4:344` __attribute__((unused)) static inline ExtcRcSlab *extc_rc_slabOf(void *… | high |
| 256 | ``ExtcRcClass { ss, head, next }`，32 个桶（`EXTC_RC_CLS_BUCKETS…` | CLS_BUCKETS 32 | **211** | `v4:211` #define EXTC_RC_CLS_BUCKETS 32u | high |
| 260 | `5.4 分配快路径（`reclaim.c:1055-1126`，产物 `evidence/example-new.c:…` | alloc 快路径全文 | **1289-1361** | `v4:1289` __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(E… | high |

## §6 写屏障（6.1–6.2）

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 286 | ``extc_rc_initRef` \| `reclaim.c:1815-1484` \| 新槽**首次**赋值 \|…` | initRef | **1815-1821** | `v4:1815` __attribute__((unused)) static inline __attribute__((always_inline)) vo… | high |
| 287 | ``extc_rc_initRefNoHold` \| `reclaim.c:1824-1492` \| 检查器证明该槽…` | initRefNoHold | **1824-1829** | `v4:1824` __attribute__((unused)) static inline __attribute__((always_inline)) vo… | high |
| 288 | ``extc_rc_storeRef` \| `reclaim.c:1832-1503` \| 一般引用赋值 \| `s…` | storeRef | **1832-1840** | `v4:1832` __attribute__((unused)) static void extc_rc_storeRef(void **slot, void … | high |
| 289 | ``extc_rc_storeRefOwned` \| `reclaim.c:1925-1629` \| 编译器能证明*…` | storeRefOwned | **1925-1966** | `v4:1925` __attribute__((always_inline)) __attribute__((unused)) static inline vo… | high |
| 290 | ``extc_rc_storeRefOwnedReg` \| `reclaim.c:1857-1559` \| 已知宿主…` | storeRefOwnedReg | **1857-1896** | `v4:1857` __attribute__((unused)) static inline void extc_rc_storeRefOwnedReg(voi… | high |
| 291 | ``extc_rc_storeRefUnknown` \| `reclaim.c:2091-1764` \| 源不可证（…` | storeRefUnknown | **2091-2101** | `v4:2091` __attribute__((unused)) static void extc_rc_storeRefUnknown(void **slot… | high |
| 292 | ``extc_rc_storeRefSlot` \| `reclaim.c:2128-1796` \| 不知道槽属于哪个…` | storeRefSlot | **2128-2133** | `v4:2128` __attribute__((unused)) static inline void extc_rc_storeRefSlot(void **… | high |
| 293 | ``extc_rc_storeStructCons` \| `reclaim.c:2143-1824` \| 整值 st…` | storeStructCons | **2143-2161** | `v4:2143` __attribute__((unused)) static void extc_rc_storeStructCons(void *dst, … | high |
| 294 | ``extc_rc_storeStructRooted` \| `reclaim.c:2211-1842` \| 整值赋…` | storeStructRooted | **2211-2219** | `v4:2211` __attribute__((unused)) static void extc_rc_storeStructRooted(void *dst… | high |
| 300 | ``extc_rc_shade` 快路径（`reclaim.c:1791-1462`）：`if (!extc_rc_wb…` | shade 快路径 | **1791-1799** | `v4:1791` __attribute__((unused)) static inline void extc_rc_shade(void *v) { | high |
| 301 | `慢路径 `shadeSlow`（`noinline`，`reclaim.c:1436-1451`）顺序：`!s \|\…` | shadeSlow | **1773-1788** | `v4:1773` __attribute__((unused, noinline)) static void extc_rc_shadeSlow(void *v… | high |
| 302 | ``shade` 缓存（`reclaim.c:1396-1462`）：**2048 格 × 8 B = 16 KB 静态…` | shade 缓存 2048×8B、代际旋进 | **1711-1745（注释；实现 1730-1745）** | `v4:1733` static inline size_t extc_rc_shadeHash(void *v) { | high |
| 303 | ``shadeOld`（`reclaim.c:1468-1475`）**只在 NOCOUNT=1（无计数档，即默认清扫档…` | shadeOld（仅 NOCOUNT 生效） | **1805-1812** | `v4:1805` __attribute__((unused)) static inline void extc_rc_shadeOld(void *const… | high |

## §7 触发与推进（7.1–7.5）

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 352 | ``extc_rc_thrUpd`（`reclaim.c:2293-2645`）：` | thrUpd 定义；2293 只是前向声明 | **3123-3168** | `v4:3126` int64_t thr = r->live * 2; | high |
| 362 | ``extc_rc_shouldSweep`（`reclaim.c:3180-2664`）：` | shouldSweep | **3180-3190** | `v4:3181` if (r->noSweep) return 0;   /* an in-edge whose host cannot be traced: … | high |
| 373 | ``extc_rc_roundOk`（`reclaim.c:3172-2649`）：`allocBytes - roun…` | roundOk | **3172-3179** | `v4:3173` int64_t need = r->live > r->floor ? r->live : r->floor; | high |
| 375 | ``scale` 的自适应（`reclaim.c:2600-2608`）：一轮扫完**什么也没释放** ⇒ `scale…` | scale 自适应与结算 | **3087-3092** | `v4:3087` if (r->swFreed == 0) { | high |
| 381 | `触发点 `extc_rc_step` \| `reclaim.c:3372`（由 `maybeCollect` 在轮在…` | extc_rc_step 触发点 | **3372-3379** | `v4:3372` __attribute__((unused)) static size_t extc_rc_step(ExtcRcRegion *r, voi… | high |
| 382 | `分配点 `markAdvance` \| `reclaim.c:1023-1038`，由 `extc_rc_alloc…` | markAdvance | **1250-1272** | `v4:1250` __attribute__((unused)) static inline __attribute__((always_inline)) vo… | high |
| 384 | ``markAdvance` 的窗口增益在默认参数下**永不生效**：基数 `64*8 = 512` 恒大于 `MARK…` | 窗口 ×8 与 BATCH_MAX 截断 | **1263-1264** | `v4:1263` if (r->allocBytes - r->markStartAlloc > EXTC_RC_MARK_WINDOW) b *= 8; | high |
| 392 | ``tlsDepth`（`reclaim.c:2081-2089`）：worker 入口帧（depth==0，唯一调用者…` | tlsDepth | **2494（声明）/ 2486-2493（注释）** | `v4:2494` static _Thread_local int32_t extc_rc_tlsDepth __attribute__((unused)); | high |

## §8 标记

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 398 | `8.1 轮的开始：`markBegin`（`reclaim.c:2098-2109`）` | markBegin | **2503-2514** | `v4:2503` __attribute__((unused)) static void extc_rc_markBegin(ExtcRcRegion *r, … | high |
| 408 | `帧根链 `[rt:chain]` \| **未接线** \| 结构体在 `reclaim.c:2055-2079` 存…` | ExtcRcFrameRoot 结构体（帧根链未接线） | **2462-2467** | `v4:2462` typedef struct ExtcRcFrameRoot { | high |
| 409 | `bump 区 \| **未接线** \| `reclaim.c:1891-1913` 有 `extc_rc_bump/…` | bump/bumpReset | **2268-2290** | `v4:2268` __attribute__((unused)) static void *extc_rc_bump(ExtcRcRegion *r, size… | high |
| 413 | ``mark` 位图：置位 = 黑 = 可达。`MARK_SCAN`（`reclaim.c:2169-2237`）先判 …` | mark 去重、occ 判定、置黑 | **2599-2652（MARK_SCAN 体）** | `v4:2605` if (!_force && (s->mark[wi] & bit)) continue;   /* forced entries are s… | high |
| 420 | `栈深 ≤ `EXTC_RC_MARK_SHALLOW = 64` 时走"浅栈直通"：逐个 pop **立即 scan*…` | 栈深 ≤64 逐个 pop 立即 scan | **2752-2769（浅栈直通）/ 2536（MARK_SHALLOW）** | `v4:2752` if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0 | high |
| 422 | ``POP > BATCH_MAX` 直接 `#error`（`reclaim.c:918-920`）。` | POP>BATCH_MAX 直接 #error | **1027-1029** | `v4:1027` #if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX | high |
| 433 | `大对象/保守对象：words > `EXTC_RC_SCAN_CHUNK = 65536`（512 KiB/段）时不进…` | 大对象/保守对象分段扫描 | **2562-2563（SCAN_CHUNK，现默认 8192）/ 2611-2616（记游标）/ 2655-2672（markResume）** | `v4:2613` if (words > (size_t)EXTC_RC_SCAN_CHUNK) {   /* very long: this step onl… | high |

## §9 清扫

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 452 | `开始 \| `reclaim.c:2547-2554 sweepBegin` \| `shadeNextGen()`（…` | sweepBegin | **3012-3020** | `v4:3012` __attribute__((unused)) static void extc_rc_sweepBegin(ExtcRcRegion *r)… | high |
| 453 | `扫一个 word \| `reclaim.c:2556-2566 sweepWord` \| `dead = occ[…` | sweepWord | **3022-3032** | `v4:3025` uint64_t dead = s->occ[w] & ~s->mark[w]; | high |
| 454 | `推进 \| `reclaim.c:2570-2610 sweepStep` \| `while (left>0 && …` | sweepStep | **3036-3097** | `v4:3045` while (_left > 0 && r->swSlab) { | high |
| 455 | `结算 \| `reclaim.c:2600-2608` \| `sweeping=0`；`swFreed==0` ⇒ …` | 清扫结算（sweeping=0/scale/thrUpd/sweeps++） | **3086-3095** | `v4:3086` r->sweeping = 0; | high |
| 459 | ``EXTC_RC_SWEEP_BUDGET = 8` ⇒ 每次 `sweepStep` 最多扫 **8 个 64-bi…` | SWEEP_BUDGET 8 | **970** | `v4:970` #define EXTC_RC_SWEEP_BUDGET 8 | high |
| 460 | ``freeSlotWord` 是**整词批量**（`reclaim.c:1271-1310`）：word 里每个 de…` | freeSlotWord 整词批量 | **1506-1545** | `v4:1506` __attribute__((unused)) static size_t extc_rc_freeSlotWord(ExtcRcSlab *… | high |
| 465 | `1. `mark` 位图按 word 顺序被清 ⇒ **标记必须从干净的 mark 位图开始**；清扫未完成时触发点只…` | mark 位图按 word 清、未完成不新开标记 | **2993-2995（注释）/ 3029（mark[w]=0）** | `v4:3029` s->mark[w] = 0; | high |
| 466 | `2. **清扫期间分配的槽必须在分配点补黑**（`reclaim.c:1113-1118`），且**只标清扫游标尚未到…` | 清扫期间分配的槽在分配点补黑 | **1349-1353** | `v4:1349` /* An allocation during sweep is marked black, **only at positions the … | high |
| 467 | `3. 定向释放自己负责清整片 `mark`（`reclaim.c:2485-2487`），理由同上。`freeSlot…` | 定向释放收尾清整片 mark | **2947-2949** | `v4:2947` extc_rc_shadeNextGen();   /* the narrowed sweep clears the whole mark s… | high |
| 472 | `slab 的**唯一**释放点是 `extc_rc_regionFree`（帧销毁，`reclaim.c:2358-2…` | regionFree 是 slab 唯一释放点 | **2358-2424（上界注释 2347-2356）** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)… | high |

## §10 定向释放

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 502 | `入口 \| `reclaim.c:2519-2525` \| `shade(v)` → `releaseDead(ho…` | storeRefOwnedRelease 入口 | **2984-2990** | `v4:2984` __attribute__((unused)) static void extc_rc_storeRefOwnedRelease(void *… | high |
| 503 | `闸 1 \| `reclaim.c:2506-2509` \| `home->noSweep \\|\\| home-…` | 闸 1：noSweep / owe 直接返回 | **2973** | `v4:2973` if (home->noSweep \|\| home->owe) return;   /* skip:nosweep */ | high |
| 504 | `闸 2 \| `reclaim.c:2510-2513` \| 旧值必须 `ownSlab(old)` 且 `s->r…` | 闸 2：旧值须在 home | **2975-2977** | `v4:2975` ExtcRcSlab *s = extc_rc_ownSlab(old); | high |
| 505 | `闸 3 \| `reclaim.c:2514-2517` \| `!budgetOk(home)` ⇒ 返回；否则 `…` | 闸 3：budgetOk 与 visitedBytes 记账 | **2978-2980** | `v4:2978` if (!extc_rc_budgetOk(home)) return;   /* skip:release-budget */ | high |
| 506 | `信用 \| `reclaim.c:1938-1941` \| `budgetOk: allocBytes - visi…` | budgetOk 信用公式 | **2315-2318** | `v4:2315` __attribute__((unused)) static int extc_rc_budgetOk(const ExtcRcRegion … | high |
| 508 | `10.3 主体（`reclaim.c:2451-2491`）` | releaseOld 主体 | **2883-2954** | `v4:2883` __attribute__((unused)) static size_t extc_rc_releaseOld(ExtcRcRegion *… | high |
| 516 | `**安全性论证**（`reclaim.c:2442-2447` 逐字要点）：释放集是"同一时刻、同一 seed 集下 …` | 释放集是 collect 的子集（trial deletion） | **2863-2882（安全性论证注释，核心句 2874）** | `v4:2874` * Safety argument (the whole basis of this mechanism): the released set… | high |
| 524 | `它与计数层的关系：计数层（NOCOUNT=0）另有一条**独立**的"hold 减到 0 当场 freeSlot"路径…` | 计数层独立的 hold→0 早回收路径 | **1668-1698（计数路径 hold→0 cascade）/ 1649-1664（NOCOUNT 空函数）** | `v4:1677` if (s->hold[i] == 0) extc_rc_freeSlotCascade(s, i); | high |

## §11 跨区 / pin / noSweep

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 534 | ``rsbit` \| 位图 `reclaim.c:226-250`；维护点 `1180/1195/1255` \| "…` | rsbit 位图与三个维护点 | **221（rsbit 字段）/ 1415（置位）、1426（清位）、1453-1469（撤销）** | `v4:221` uint64_t     *rsbit;     /* cross-region registration bitmap: this slab… | high |
| 536 | `边的三处转移 \| `reclaim.c:1133-1137`（3 条逐字）；实现 `1173-1234` \| (1…` | 跨区边三处转移 | **1363-1372（注释）/ 1403-1469（实现）** | `v4:1363` /* ---- Source-side bookkeeping: counting and undoing cross-region in-e… | high |
| 537 | `撤销时机 \| `reclaim.c:1218-1234 unlinkChildren` \| 槽"即将不再是对象"前…` | unlinkChildren 撤销时机 | **1453-1469** | `v4:1453` __attribute__((unused)) static void extc_rc_unlinkChildren(ExtcRcSlab *… | high |
| 538 | `为什么不是只置不清 \| `reclaim.c:1137` 注释 \| 只置不清会让种子涨到每个对象；现在的登记位数 …` | 登记位数 = 当前活跨区边数 | **1365-1372** | `v4:1365` * `rsbit` is a **cache** of "this target slot has live in-edges from an… | high |
| 539 | `高水位 \| `reclaim.c:1139-1141` `rsIncN/rsDecN/rsHwm` \| `rs_l…` | rsIncN/rsDecN/rsHwm 高水位 | **1377** | `v4:1377` __attribute__((unused)) static int64_t extc_rc_rsIncN, extc_rc_rsDecN, … | high |
| 540 | `运行期守卫 \| `reclaim.c:1154-1165` `EXTC_RC_FOREIGN_GUARD`（默认 0…` | 运行期外区守卫 | **1394-1400（FOREIGN_GUARD 与打印/arm）** | `v4:1394` #define EXTC_RC_FOREIGN_GUARD 0 | high |
| 548 | `动机 \| `reclaim.c:1631-1648` 注释 \| 宿主（view 元素 / pool buffer …` | pin 动机注释 | **1968-1981** | `v4:1968` /* ---- Slots whose host cannot be proven: pin the value itself as a ro… | high |
| 549 | `结构 \| `reclaim.c:1649-1709` \| 进程级开放寻址哈希（键=值指针，load ≤ 1/2，删…` | pin 结构/哈希/pinAdd | **1982-2046** | `v4:1982` typedef struct ExtcRcPin { void *v; ExtcRcRegion *rg; size_t n; } ExtcR… | high |
| 550 | `方向 \| `reclaim.c:1642` \| 只有 pin **不可能**（分配失败）才退回 `noSweep`…` | 方向：pin 失败才退回 noSweep | **1979** | `v4:1979` * Direction: only when pinning is impossible (allocation failure) fall … | high |
| 551 | `线程 \| `reclaim.c:1634-1648` \| pin 表**进程级**（region 可能被别的 wo…` | 线程：进程级 pin 表、加锁 | **1980（+ 1985-1991 锁）** | `v4:1980` * Threads: the pin table is process-wide (a region may be swept by anot… | high |
| 552 | ``pinAdd` \| `reclaim.c:1688-1709` \| `!v \\|\\| !owns(v)` ⇒…` | pinAdd | **2025-2046** | `v4:2025` __attribute__((unused)) static int extc_rc_pinAdd(void *v) { | high |
| 553 | ``pinSub` \| `reclaim.c:1710-1723` \| `n--` 或擦除（Knuth 后移） \|` | pinSub | **2047-2060** | `v4:2047` static __attribute__((unused)) void extc_rc_pinSub(void *v) { | high |
| 554 | ``pinDropRegion` \| `reclaim.c:1725-1735` \| region 死时删掉它名下所…` | pinDropRegion | **2062-2072** | `v4:2062` static __attribute__((unused)) void extc_rc_pinDropRegion(ExtcRcRegion … | high |
| 555 | ``pinSeed` \| `reclaim.c:1742-1750`（`noinline`） \| 把本 region…` | pinSeed | **2079-2087** | `v4:2080` if (__atomic_load_n(&extc_rc_pinN, __ATOMIC_ACQUIRE) == 0) return;   /*… | high |
| 561 | ``storeRefUnknown` 的 `pinAdd` 失败 \| `reclaim.c:1754-1764`（`1…` | storeRefUnknown：pinAdd 失败 ⇒ 目标 region noSweep=1 | **2091-2101（置位 2096-2098）** | `v4:2096` if (!extc_rc_pinAdd(v)) { | high |
| 562 | ``storeStructCons` 且 `hostKnown==0` \| `reclaim.c:1806-1824`…` | storeStructCons 同方向 | **2157-2158（hostKnown==0 分支置 noSweep）** | `v4:2158` else if (rv) rv->noSweep = 1; | high |
| 563 | `帧根链存在 bad 帧 \| `reclaim.c:2075 chainBad` \| 调用方拒绝 sweep（同一决…` | chainBad（帧根链有 bad 帧） | **2480-2484** | `v4:2480` __attribute__((unused)) static int extc_rc_chainBad(void) { | high |
| 568 | ``shouldSweep` 直接返回 0（`reclaim.c:2651`，注释 "reclaimed by coun…` | SWEEP_FREE 下 shouldSweep 恒 0 | **3185** | `v4:3185` return 0;   /* skip:sweepfree */ | high |

## §12 计数层

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 581 | ``reclaim.c:1312-1327` 是 `[rt:nocount]` 段的开关：`EXTC_RC_NOCOUN…` | [rt:nocount] 段开关与发射期折叠 | **1649-1664（空函数）/ 1668-1698（真实现）/ 40-50 rtSectionOn、67-88 rtCfgIf** | `v4:1656` #if EXTC_RC_NOCOUNT | high |
| 588 | `2. 发射器 `reclaim.c:2914` 写出 `#define EXTC_RC_NOCOUNT 1`（=0 时…` | 发射器写 #define EXTC_RC_NOCOUNT | **3533** | `v4:3533` bufPuts(out, noCount ? "#define EXTC_RC_NOCOUNT 1" : "#define EXTC_RC_N… | high |
| 589 | `3. 运行时 `reclaim.c:1319` `#if EXTC_RC_NOCOUNT` 把计数入口变成空操作。` | 运行时 #if EXTC_RC_NOCOUNT | **1656** | `v4:1656` #if EXTC_RC_NOCOUNT | high |
| 598 | ``holdUpd` 五步 \| `reclaim.c:1345-1357` \| ① `ownSlab(p)`，非本运…` | holdUpd 五步 | **1682-1694** | `v4:1683` ExtcRcSlab *s = extc_rc_ownSlab(p); | high |
| 599 | ``holdUpdSlab` \| `reclaim.c:1331-1341` \| 已解析 slab 的版本，判据逐条…` | holdUpdSlab | **1668-1678** | `v4:1668` __attribute__((unused)) static void extc_rc_holdUpdSlab(ExtcRcSlab *s, … | high |
| 600 | ``inc/dec` \| `reclaim.c:1360-1361` \| 只是 `holdUpd(p,+1)` / …` | inc/dec 只是别名 | **1697-1698** | `v4:1697` __attribute__((unused)) static void extc_rc_inc(void *p) { extc_rc_hold… | high |
| 606 | ``reclaim.c:1359` 逐字："Counting hooks only pointer assignment…` | 计数钩子只挂指针赋值（逐字注释） | **1696** | `v4:1696` /* Counting hooks only pointer assignments: data-field stores do not go… | high |
| 616 | `数据字段赋值不走 `inc/dec`（附加准则 `reclaim.c:123-126`）——所以计数**不跟踪** `…` | 附加准则：数据字段赋值不动计数 | **123-126** | `v4:123` * Two further criteria: | high |
| 622 | `分工 \| 标记/清扫**完全不依赖计数**；计数的唯一角色是"减到 0 立即回收" \| `reclaim.c:13…` | 分工：标记/清扫不依赖计数 | **1651-1655** | `v4:1651` * Marking/sweeping **do not depend on counts** -- `extc_rc_mark` starts… | high |
| 630 | `TICK 调用点（默认 `EXTC_RC_TIME` 关时全部编译成 `do{}while(0)`，`reclaim.…` | TICK 调用点与默认空宏 | **158-160（TICK 空宏）/ 181-184（计时版）；调用点 783/1290/1481/1860/1927/2295/3382** | `v4:159` #define EXTC_RC_TICK(k) do { } while (0) | high |

## §1.5–§2 表 1-1

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 50 | `2 \| **rc region**（slab + 标记清扫） \| `new T`、定长 `new [N]T`、`a…` | alloc 定义（表 1-1 的 rc region 条目） | **1289** | `v4:1289` __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(E… | high |

## §13–§15 其余引用

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 674 | `slab 登记表（自旋锁 `reclaim.c:397-402` + 按代发布 + **旧代永不释放**；读者无锁探测…` | slab 登记表：自旋锁、代发布、旧代永不释放 | **388-407（锁与代发布）/ 545-561（ownFind 无锁读重试）** | `v4:402` __attribute__((unused)) static inline void extc_rc_regLockAcq(void) {<br>`v4:547` uint32_t seq = __atomic_load_n(&extc_rc_regSeq, __ATOMIC_ACQUIRE); | high |
| 686 | `noSweep/owe 跨线程写 \| `reclaim.c:1761/1821`（`noSweep` 仅此两处写）、…` | noSweep / owe 的写入与读取点 | **2096-2098（noSweep 写 1）/ 2158（写 2）/ 3433（owe 写）/ 3434（owe 读）** | `v4:2096` if (!extc_rc_pinAdd(v)) {<br>`v4:2158` else if (rv) rv->noSweep = 1;<br>`v4:3433` __attribute__((unused)) static void extc_rc_owe(ExtcRcRegion *r) { r->o… | high |
| 687 | `pin 表项 `rg` 缓存 \| `reclaim.c:1690-1709` \| 加锁维护；`rg` 指向的 re…` | pin 表项 rg 缓存 | **2025-2046（pinAdd 记录 rg）** | `v4:2025` __attribute__((unused)) static int extc_rc_pinAdd(void *v) { | high |

## §16 诊断 / 死代码

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 697 | ``EXTC_RC_PAUSE=1` \| `reclaim.c:2678-2714`（`pauseDump`）、271…` | EXTC_RC_PAUSE 输出与 arm | **3224-3318（pauseDump）/ 3319-3338（pauseArm）** | `v4:3224` __attribute__((unused)) static void extc_rc_pauseDump(void) { | high |
| 698 | ``EXTC_RC_SLOWSTEP=<us>` \| `reclaim.c:2719-2720` 只设阈值，超阈值在 …` | SLOWSTEP 只是告警阈值，不是人为延迟 | **3319-3338（SLOWSTEP 解析）；[slow] 打印 2819-2822（mark）/ 3067（sweep）** | `v4:3067` fprintf(stderr, "[slow] kind=sweep why=%s dur_us=%.1f sw_words=%lld fre… | high |
| 699 | ``EXTC_RC_STATS` \| `reclaim.c:1568-1584`（`statsArm 1583`） \…` | EXTC_RC_STATS 退出打印 | **1905-1921（statsDump / statsArm）** | `v4:1905` __attribute__((unused)) static void extc_rc_statsDump(void) { | high |
| 702 | `**"能全量回收"的接口存在但不是生产路径**：`extc_rc_collect`（`reclaim.c:3381`，…` | 全量回收入口存在但非生产路径 | **3381-3390（collect）/ 3112-3116（sweep）/ 3100-3110（settle）** | `v4:3381` __attribute__((unused)) static size_t extc_rc_collect(ExtcRcRegion *r, … | high |

## §17 默认参数总表

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 733 | ``EXTC_RC_M_MAX` \| 未设或 0 = 隐式墙 `3·live`（负数按 0） \| 运行期 `recl…` | mmaxArm 读 EXTC_RC_M_MAX | **926-938** | `v4:926` __attribute__((unused)) static void extc_rc_mmaxArm(void) { | high |
| 734 | ``EXTC_RC_SWEEP_PRESSURE_MUL` \| 4（1..16，越界 `#error`） \| `re…` | SWEEP_PRESSURE_MUL 常量 | **990** | `v4:990` #define EXTC_RC_SWEEP_PRESSURE_MUL 4 | high |
| 735 | ``EXTC_RC_STEP_CPU` \| 未设 = 墙钟 \| `reclaim.c:1047` \| =1 时 `…` | EXTC_RC_STEP_CPU | **1053-1055（stepCpu 变量）/ 3325-3329（pauseArm 解析）** | `v4:1053` static int extc_rc_stepCpu;   /* EXTC_RC_STEP_CPU: time each step with … | high |

## §18 内存上界

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 791 | `rc region \| `M_rc(t)` = Σ`_{r 存活}` `S_r(t)`；`S_r(t)` ≤ 该 r…` | regionFree 与 slab 字节上界 | **2358-2424** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)… | high |
| 800 | `触发判据（`src/back/reclaim.c:2650-2658`）：`shouldSweep = bytes >…` | 触发判据三函数 | **3180-3190（shouldSweep）/ 3123-3168（thrUpd）/ 3172-3179（roundOk）** | `v4:3188` return r->bytes > r->thr && extc_rc_roundOk(r);   /* round credit: a se…<br>`v4:3126` int64_t thr = r->live * 2;<br>`v4:3173` int64_t need = r->live > r->floor ? r->live : r->floor; | high |
| 823 | `2. **`noSweep` / `owe` 永久置位**：`noSweep` 只在 `reclaim.c:1761`…` | noSweep 永久置位、region 永不回收 | **2096-2098 / 2158（noSweep 两处置位）** | `v4:2158` else if (rv) rv->noSweep = 1; | high |
| 825 | `4. **阈值上限**：`scale` 可乘到 `EXTC_RC_SCALE_MAX = 2^40`（`reclaim…` | scale 上限与溢出兜底 | **144（SCALE_MAX）/ 3130（乘法溢出 ⇒ thr=0）** | `v4:144` #define EXTC_RC_SCALE_MAX ((int64_t)1 << 40)<br>`v4:3130` if (scale > 1 && thr > INT64_MAX / scale) { r->thr = 0; return; } | high |
| 827 | `6. **空扫把上界整体乘 scale**：`scale`（reclaim.c:2631-2642；空扫 ×4、上限 …` | 空扫把 thr 整体乘 scale | **3130** | `v4:3130` if (scale > 1 && thr > INT64_MAX / scale) { r->thr = 0; return; } | high |
| 835 | `且 **RSS = 历史峰值**（region 存活期不还 OS，`reclaim.c:1970` 只在 drop 时…` | RSS = 历史峰值 | **2358（仅 regionFree 才 free）** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)… | high |
| 845 | `M_max    = EXTC_RC_M_MAX   (未设/0 ⇒ 隐式 3·live；负数按 0)        …` | M_max = mmaxArm | **926-938** | `v4:926` __attribute__((unused)) static void extc_rc_mmaxArm(void) { | high |
| 846 | `thr      = min( max(2·live, floor)·scale , M_max − live/4 )…` | thr 公式与夹取 | **3123-3168（thr 3126、乘 scale 3132、夹墙 3152）** | `v4:3126` int64_t thr = r->live * 2; | high |
| 847 | `pressure = (bytes ≥ 7/8·M_max) \|\| (thr>0 && bytes > 4·thr…` | pressure 近墙判据 | **952-966** | `v4:952` __attribute__((unused)) static inline int extc_rc_pressure(const ExtcRc… | high |
| 848 | `近墙时每步预算 × EXTC_RC_SWEEP_PRESSURE_MUL                       …` | 近墙预算乘子 | **990** | `v4:990` #define EXTC_RC_SWEEP_PRESSURE_MUL 4 | high |
| 852 | `**代价（必须一并承诺）**：夹紧 thr 使 sweep 次数从小常数变成 **O(log N)** —— 一轮最多…` | M_max 注释里的 O(log N) 代价 | **897-909** | `v4:905` * trades that for a logarithmic sweep count: with thr <= 2.75 x live on… | high |
| 854 | `**墙不可达警告**：若 `M_max` 小到连 floor 都放不下（`max(2L,floor)` 压不到墙下），…` | 墙不可达的一次性警告 | **3144-3148** | `v4:3144` if (_cap < r->floor) { | high |

## §19 停顿上界

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 863 | `标记推进（分配点触发） \| 种子 ≤ `SEED_PUSHES 256` pushes / `SEED_WORDS …` | 种子/walk/批量预算常量 | **1005-1023** | `v4:1005` #define EXTC_RC_SEED_PUSHES 256  /* how many **seed slots** one step pu… | high |
| 864 | `大对象扫描 \| 每段 `SCAN_CHUNK` **8192** words = 64 KiB，超出记 cursor…` | 大对象分段与续扫 | **2562-2563（SCAN_CHUNK）/ 2655-2672（markResume）** | `v4:2562` #ifndef EXTC_RC_SCAN_CHUNK<br>`v4:2655` __attribute__((unused)) static size_t extc_rc_markResume(ExtcRcRegion *… | high |
| 865 | `清扫 \| 每次 `sweepStep` `SWEEP_BUDGET 8` words = 512 槽 \| `rec…` | SWEEP_BUDGET | **970** | `v4:970` #define EXTC_RC_SWEEP_BUDGET 8 | high |
| 866 | `定向释放 `releaseOld` \| **有预算**：`EXTC_RC_RELEASE_BUDGET 2048` …` | releaseOld 预算 | **980（RELEASE_BUDGET）/ 2894（预算使用）/ 2941-2945（phase2 切断）** | `v4:980` #define EXTC_RC_RELEASE_BUDGET 2048<br>`v4:2894` uint32_t budget = EXTC_RC_RELEASE_BUDGET;<br>`v4:2941` if (r->msp) {   /* budget cut the descent: the rest waits for the sweep… | high |
| 867 | ``regionFree` \| 不分批，但**单次调用有显式上界**：每次 drop ≤ **10·#slab + #…` | regionFree 单次 free 次数上界 | **2358-2424（上界注释 2347-2356）** | `v4:2348` * Bounded by construction: 9 frees per normal slab (occ/mark/rsbit/hold… | high |
| 880 | ``releaseOld` 有界（本轮 after） \| 2048 步预算生效：无续扫时单次 ≤ **36 µs**（…` | releaseOld 有界与其代价 | **2941-2945** | `v4:2941` if (r->msp) {   /* budget cut the descent: the rest waits for the sweep… | high |
| 882 | `续扫计量（v2 已修） \| `extc_rc_markResume` 现在**返回走过的 word 数**（recl…` | 续扫计量 | **2655-2672（markResume 返回段长）/ 2685-2690（seg 记账）/ 3282（[work] seg_max）/ 3310（[ubwork] seg_max）** | `v4:2655` __attribute__((unused)) static size_t extc_rc_markResume(ExtcRcRegion *…<br>`v4:3282` fprintf(stderr, "[work] n=%lld walk_max=%lld scan_max=%lld push_max=%ll… | high |
| 886 | `1. **`regionFree`（帧销毁）**：不分批，但单次调用的 `free` 次数有显式上界（≤ `10·#s…` | 帧销毁 regionFree 的成本口径 | **2358-2424** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)… | high |
| 903 | ``w` \| 每段扫描 word 数 \| **8192**（64 KiB，本轮 P0-1） \| `SCAN_CHU…` | w = SCAN_CHUNK | **2562-2563** | `v4:2562` #ifndef EXTC_RC_SCAN_CHUNK | high |
| 906 | ``f` \| 单次释放槽数 \| 清扫 ≤512；`releaseOld` ≤2048 步（本轮 P0-3） \| `…` | f 的释放槽数上界 | **980** | `v4:980` #define EXTC_RC_RELEASE_BUDGET 2048 | high |
| 908 | ``c_r` \| 每槽的**区域级**释放成本（`regionFree` 内逐槽归还 slab / 记账；未分批时规模…` | c_r 区域级释放成本 | **2358-2424** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)… | high |

## §20 与既有文档不一致

| 文档行 | 原文引用（≤60 字符） | 语义指向 | v4 正确行号 | 依据（v4 源码摘录） | 置信度 |
|---|---|---|---|---|---|
| 925 | `2 \| "无写屏障 / 无原子 / 无 STW" \| 每次引用赋值都发射 `storeRef*`（§6）；运行时含…` | “无原子”断言的实际反例 | **388-407（锁与代发布）/ 926-938（mmaxArm）** | `v4:402` __attribute__((unused)) static inline void extc_rc_regLockAcq(void) {<br>`v4:926` __attribute__((unused)) static void extc_rc_mmaxArm(void) { | high |
| 927 | `4 \| "无共享大堆 / 无碎片" \| slab 来自 `malloc`，`regionFree` 只 `free…` | “无共享大堆/无碎片”断言的边界 | **2358-2424（只 free 不 munmap）/ 452-465（旧代永不释放）** | `v4:2358` __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r)…<br>`v4:452` __attribute__((unused)) static ExtcRcReg *extc_rc_ownNextReg(ExtcRcReg … | high |
| 938 | `15 \| `gate-release` A: sweeps≤1 / D: release_slots>0 \| 夹紧…` | gate-release 的 budgetOk 信用被吃光 | **2883（releaseOld 入口）** | `v4:2883` __attribute__((unused)) static size_t extc_rc_releaseOld(ExtcRcRegion *… | high |

## 覆盖范围与统计

- 文档中**含 `reclaim.c` 引用**的行：131 行 / 140 个 ref token；本表逐 token 成行，共 **125 行**（§5–§12 全部 + §1.5/§2 表 1-1 + §13–§22）。
- 仅有文件名、没有行号的行（例如 doc:26 的证据约定、doc:941 的边界说明）不产生行号映射行。
- 置信度：全部 **high**（每条的依据行都在冻结源码里逐字命中该语义；边界只有 ±0 行）。

## 最该先修的 10 处（按“错得最远/被引用最多”排序）

| # | 文档写法 | v4 真值 | 后果 |
|---|---|---|---|
| 1 | `thrUpd reclaim.c:2293-2645` | 3123-3168 | 2293 只是前向声明，2645 已落在 `markResume` 里 |
| 2 | `markBegin reclaim.c:2098-2109` | 2503-2514 | 两数都是旧版 |
| 3 | `sweepBegin 2547-2554 / sweepWord 2556-2566 / sweepStep 2570-2610` | 3012-3020 / 3022-3032 / 3036-3097 | 清扫三连全部错位 |
| 4 | `releaseOld reclaim.c:2451-2491` | 2883-2954 | 整段迁移 |
| 5 | `regionFree reclaim.c:1970-2019 / 2358-2019` | 2358-2424 | 两个年代混用 |
| 6 | `freeSlotWord reclaim.c:1271-1310` | 1506-1545 | 与 `freeSlot` 段一起迁移过 |
| 7 | `budgetOk reclaim.c:1938-1941` | 2315-2318 | |
| 8 | `markAdvance reclaim.c:1023-1038` | 1250-1272 | 文档 1030-1031 的“窗口增益永不生效”应为 1263-1264 |
| 9 | pin 全段 1631-1764 | 1968-2101 | 动机/结构/方向/线程/pinAdd/pinSub/pinDropRegion/pinSeed 逐项已列出 |
| 10 | `[rt:nocount] 1312-1361`、发射 `#define 2914` | 1649-1698、3533 | 计数层整段迁移 |

## 复核方法（可重跑）

1. 冻结核对：`md5sum src/back/reclaim.c docs/analysis/MEMORY-MODEL-CURRENT.md; wc -l src/back/reclaim.c`。
2. 任取一行：`sed -n '2503,2514p' src/back/reclaim.c`，去掉行首缩进/引号/行尾 `\n` 后应逐字等于 依据 列摘录。
3. 语义判定顺序：先用文档行里的标识符（`extc_rc_*` / `EXTC_RC_*` / `[rt:*]`）在源码里定位定义行，再用依据行确认；**不用** `旧行号+偏移`。

## 限制与未验证项

- 本表按冻结 md5 生成；文档或源码任一被改动，行号即失效，需重跑同一方法。
- v4 行号是“定义行区间”，文档若需要“单行”引用，取区间首行（左列已注明具体子项时取括注行）。
- 文档里少数引用同时提到 `codegen.c` 行号（如 doc:50 的 4653-4667），不属于本表范围，未核。

## 附录 A：可直接机械套用的补丁清单（/tmp/refs_patch.tsv）

本表已机械化为一行一个替换：`/tmp/refs_patch.tsv`（UTF-8，三列：doc_line、old_substring、new_substring），配套校验器 `/tmp/refs_patch_check.py`。

- 规模：**155 条替换 / 121 个文档行**；文档中 125 个含 `reclaim.c` 行号引用的行里，4 行原引用就是 v4 真值（doc:50 1289、doc:256 211、doc:381 3372、doc:616 123-126），故不产生替换。
- 每条 `old_substring` 都在其 `doc_line` 行内**恰好出现 1 次**（校验器逐条 assert，miss=0）；同一行多引用拆成多条，互不重叠。
- 除行号外，只有 doc:433 一处**内容**同时过期：`EXTC_RC_SCAN_CHUNK = 65536`（512 KiB/段）→ `= 8192`（64 KiB/段），已含在清单里。
- 应用方式：按 doc_line 升序、行内按文件出现顺序做**字面**替换即可；不要重排行，不要动 markdown 的 `|` 分隔（替换文本里没有 `|` 字符）。
- 应用后的自检：替换后再跑一遍 `/tmp/refs_patch_check.py` 应报 miss=0（它直接对原文档校验，不会误报已应用）。

| 文档节 | 替换条数 |
|---|---|
