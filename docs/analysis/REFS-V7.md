# REFS-V7：src/back/reclaim.c 行号 v5/v6 → v7 语义映射

本表是 task-33 的交付物：把 docs/analysis/MEMORY-MODEL-CURRENT.md 与 docs/analysis/GC-ROOFLINE.md 里**所有**带 reclaim.c: 前缀的引用，按**语义**重指到 v7 冻结源码。**没有一处使用偏移量批量平移**；每条都先在 v7 源码里按标识符/语句定位，再抄 1–2 行源码作依据。

## 0 冻结口径

| 文件 | md5 | 行数 | 说明 |
|---|---:|---:|---|
| `src/back/reclaim.c` | `e17080d1c80513b27ca06f2bc6224b0f` | 4031 | v7 权威源码（task-27/28 之后）|
| `docs/analysis/MEMORY-MODEL-CURRENT.md` | `835ea9151d334db795f98e0924d409e9` | 1065 | 本表行号基于此快照 |
| `docs/analysis/GC-ROOFLINE.md` | `b9205482f82689566327f5bc1cb7e270` | 981 | 本表行号基于此快照 |

src/back/codegen.c 未变（d339382889b87c0dd18b357c69e2e779），本表不含 codegen 引用。文档被并行编辑时，`文档行` 列可能漂移，但 `唯一片段` 与 `原文引用` 两列仍可按文本定位。

## 1 读表须知：旧引用的三种错误形态

1. **A 型（首数已是 v6、次数是 v5）**：reclaim.c:1815-1484、1925-1629 这类成对数字，两侧年代不同，**不能取任何一个数**。典型：initRef、storeRefOwned、shade、shouldSweep、roundOk。
2. **B 型（两数都是 v5）**：reclaim.c:2503-2514 markBegin、3066-3074 sweepBegin、2451-2491 releaseOld。整段漂移 v6→v7 约 +300 行，机械加偏移也会错在函数边界。
3. **C 型（两个年代并存于同一节）**：regionFree 同时出现 1970-2019、2358-2424；GC 文档的附录 D 与 §6.7 混装 v5、v7 两套数字。

因此：**逐条按语义定位**，宁可标 medium 也不猜。

## 2 覆盖统计

| 文档 | 引用条数 | 涉及文档行 | high | medium | 已正确（delta=0）| 需要改 |
|---|---:|---:|---:|---:|---:|---:|
| MEMORY | 152 | 141 | 152 | 0 | 13 | 139 |
| GC | 44 | 39 | 43 | 1 | 2 | 42 |
| **合计** | **196** | **180** | **195** | **1** | **15** | **181** |

Lead 口径里的 187 处是上一轮抽取数；本表按当前文档实际文本重抽，得到 196 条（文档新增了 §6.7/§17.1 等行）。

## 3 MEMORY-MODEL-CURRENT.md 映射表

### 表 1-1 五种存储基质

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 53 | `reclaim.c:1289` | `extc_rc_alloc`（`reclaim.c:1289`） \| 不可达被标记清扫判定` | rc region（slab + 标记清扫） · new T、定长 new [N]T、alloc<T>(n)、单值 @o | `reclaim.c:1455` | `v7:1455 __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(Ex` | high |

### 5.2 slab 几何（reclaim.c:782-853）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 240 | `reclaim.c:782-853` | `## 5.2 slab 几何（`reclaim.c:782-853`）` | 5.2 slab 几何（） | `reclaim.c:820-894` | `v7:820 __attribute__((unused)) static ExtcRcSlab *extc_rc_slabNew(ExtcRcRegion *` | high |
| 244 | `reclaim.c:138` | `MiB, 1 MiB)` \| `reclaim.c:138/321/801（`aligne` | slab 对齐与总尺寸 · EXTC_RC_SLAB_BITS = 20 ⇒ EXTC_RC_SLAB_SIZE = 1 | `reclaim.c:138` | `v7:138 #define EXTC_RC_SLAB_BITS 20 ; v7:138 #define EXTC_RC_SLAB_BITS 20` | high |
| 245 | `reclaim.c:322` | `xtcRcSlab))` \| `reclaim.c:322` \|` | 头部 · EXTC_RC_SLAB_HDR = align64(sizeof(ExtcRcSlab)) | `reclaim.c:331` | `v7:331 #define EXTC_RC_SLAB_HDR  ((sizeof(ExtcRcSlab) + 63) & ~(size_t)63) ; v7:` | high |
| 246 | `reclaim.c:1309` | `数，**非 2 的幂** \| `reclaim.c:1309（`slotSize` 调用；1` | 槽步长 ss · 请求长度向上取 8 的倍数，非 2 的幂 | `reclaim.c:1510` | `v7:1510 size_t ss = extc_rc_slotSize(n); ; v7:1510 size_t ss = extc_rc_slotSize(` | high |
| 247 | `reclaim.c:326-327` | `1`，直到 1 MiB \| `reclaim.c:326-327,788-790` \|` | 块初始尺寸 · chunk = EXTC_RC_SLAB_CHUNK_MIN = 64 KiB，当 chunk/ss < | `reclaim.c:335-336` | `v7:335 #define EXTC_RC_SLAB_CHUNK_MIN ((size_t)64 * 1024) ; v7:336 #define EXTC_` | high |
| 248 | `reclaim.c:791-800` | ``nslots = 1` \| `reclaim.c:791-800` \|` | 槽数 · cap = (chunk - HDR) / ss；cap < 2 ⇒ huge block：头部照建，payl | `reclaim.c:830-846` | `v7:830 int big = (cap < 2);   /* fewer than two fit, so the payload is malloc'd ` | high |
| 249 | `reclaim.c:819-821` | `lloc**（要整片清） \| `reclaim.c:819-821` \|` | 位图 · occ、mark、rsbit 各 nwords = (nslots+63)/64 个 uint64_t，cal | `reclaim.c:858-860` | `v7:858 s->occ   = (uint64_t *)calloc(s->nwords, sizeof(uint64_t)); ; v7:859 s->m` | high |
| 250 | `reclaim.c:823-831` | `；`rscnt` 懒惰建 \| `reclaim.c:823-831（hold 注释/malloc` | 每槽元数据 · tid（2 B，类型号）总是 malloc；hold（2 B 计数）仅 NOCOUNT=0 时分配；rs | `reclaim.c:862-875` | `v7:862 * (~29k instructions on a slab of 16 B slots). When off, not even the poi` | high |
| 251 | `reclaim.c:828-831` | `1 固定是保守描述符） \| `reclaim.c:828-831（828 ttabCap=8、8` | 类型表 · ttab 容量 8，ttab[1] = EXTC_RC_CONSERVATIVE（号 1 固定是保守描述符） | `reclaim.c:866-870` | `v7:866 s->tid   = (uint16_t *)malloc(nslots * sizeof(uint16_t)); ; v7:867 s->tta` | high |
| 252 | `reclaim.c:844-845` | `）而不是 payload \| `reclaim.c:844-845（`freelist = -1`` | 复用链 · freelist 初始 -1；不预链全块，"未发放的槽走 bump，只有被释放的槽进 freelist"，链 | `reclaim.c:883-885` | `v7:883 s->freelist = -1;   /* empty head: the chain's next slot number lives in ` | high |
| 253 | `reclaim.c:330` | `ps`（复用链编码上限） \| `reclaim.c:330（常量）+ 812-813（守卫` | 槽号上限 · EXTC_RC_REUSE_MAX_SLOT = 0xFFFE，超了 oops（复用链编码上限） | `reclaim.c:339` | `v7:339 #define EXTC_RC_REUSE_MAX_SLOT 0xFFFEu ; v7:339 #define EXTC_RC_REUSE_MAX` | high |
| 255 | `reclaim.c:334` | `DATA`/`SlabOf`，`reclaim.c:334/344-346`；`regio` | 为什么"1 MiB 对齐"重要：从指针反查 slab 只需一次位与（EXTC_RC_SLAB_DATA/SlabOf，/ | `reclaim.c:343` | `v7:343 #define EXTC_RC_SLAB_DATA(s) ((s)->data) ; v7:343 #define EXTC_RC_SLAB_DA` | high |

### 5.3 大小类表

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 259 | `reclaim.c:211` | `LS_BUCKETS 32`，`reclaim.c:211`）。` | - ExtcRcClass { ss, head, next }，32 个桶（EXTC_RC_CLS_BUCKETS 3 | `reclaim.c:211` | `v7:211 #define EXTC_RC_CLS_BUCKETS 32u ; v7:211 #define EXTC_RC_CLS_BUCKETS 32u` | high |

### 5.4 分配快路径（reclaim.c:1289-1361，产物 evidence/example-new.c:945 起）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 263 | `reclaim.c:1289-1361` | `### 5.4 分配快路径（`reclaim.c:1289-1361`，产物 `evidence/e` | 5.4 分配快路径（，产物 evidence/example-new.c:945 起） | `reclaim.c:1455-1615` | `v7:1455 __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(Ex` | high |

### 6.1 运行期入口

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 289 | `reclaim.c:1815-1821` | `_rc_initRef` \| `reclaim.c:1815-1821` \| 新槽**首次**赋值 \|` | extc_rc_initRef · 新槽首次赋值 | `reclaim.c:2094-2101` | `v7:2094 __attribute__((unused)) static inline __attribute__((always_inline)) voi` | high |
| 290 | `reclaim.c:1824-1829` | `itRefNoHold` \| `reclaim.c:1824-1829` \| 检查器证明该槽"没有任何` | extc_rc_initRefNoHold · 检查器证明该槽"没有任何 dec"（planRcNoDec） | `reclaim.c:2103-2109` | `v7:2103 __attribute__((unused)) static inline __attribute__((always_inline)) voi` | high |
| 291 | `reclaim.c:1832-1840` | `rc_storeRef` \| `reclaim.c:1832-1840` \| 一般引用赋值 \| `sh` | extc_rc_storeRef · 一般引用赋值 | `reclaim.c:2111-2127` | `v7:2111 __attribute__((unused)) static void extc_rc_storeRef(void **slot, void *` | high |
| 292 | `reclaim.c:1925-1966` | `oreRefOwned` \| `reclaim.c:1925-1966` \| 编译器能证明**宿主 r` | extc_rc_storeRefOwned · 编译器能证明宿主 region | `reclaim.c:2205-2261` | `v7:2205 __attribute__((always_inline)) __attribute__((unused)) static inline voi` | high |
| 293 | `reclaim.c:1857-1896` | `RefOwnedReg` \| `reclaim.c:1857-1896` \| 已知宿主 region` | extc_rc_storeRefOwnedReg · 已知宿主 region 且已查表 | `reclaim.c:2136-2182` | `v7:2136 __attribute__((unused)) static inline void extc_rc_storeRefOwnedReg(void` | high |
| 294 | `reclaim.c:2091-2101` | `eRefUnknown` \| `reclaim.c:2091-2101` \| 源不可证（仅由 `sto` | extc_rc_storeRefUnknown · 源不可证（仅由 storeRefSlot 兜底可达） | `reclaim.c:2371-2381` | `v7:2371 __attribute__((unused)) static void extc_rc_storeRefUnknown(void **slot,` | high |
| 295 | `reclaim.c:2128-2133` | `toreRefSlot` \| `reclaim.c:2128-2133` \| 不知道槽属于哪个 reg` | extc_rc_storeRefSlot · 不知道槽属于哪个 region | `reclaim.c:2408-2421` | `v7:2408 __attribute__((unused)) static inline void extc_rc_storeRefSlot(void **s` | high |
| 296 | `reclaim.c:2143-2161` | `eStructCons` \| `reclaim.c:2143-2161` \| 整值 struct 赋值` | extc_rc_storeStructCons · 整值 struct 赋值、叶子不可枚举 | `reclaim.c:2423-2441` | `v7:2423 __attribute__((unused)) static void extc_rc_storeStructCons(void *dst, s` | high |
| 297 | `reclaim.c:2211-2219` | `tructRooted` \| `reclaim.c:2211-2219` \| 整值赋值，但根表已按所有` | extc_rc_storeStructRooted · 整值赋值，但根表已按所有 variant 静态展开 | `reclaim.c:2491-2499` | `v7:2491 __attribute__((unused)) static void extc_rc_storeStructRooted(void *dst,` | high |

### 6.2 屏障的实际成本：只在标记在飞时付

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 303 | `reclaim.c:1791-1799` | `_rc_shade` 快路径（`reclaim.c:1791-1799`）：`if (!extc_rc` | - extc_rc_shade 快路径（）：if (!extc_rc_wb \|\| !v) return; —— 没有标记 | `reclaim.c:2070-2083` | `v7:2070 __attribute__((unused)) static inline void extc_rc_shade(void *v) { ; v7` | high |
| 304 | `reclaim.c:1773-1788` | `ow`（`noinline`，`reclaim.c:1773-1788`）顺序：`!s \|\| !s->` | - 慢路径 shadeSlow（noinline，）顺序：!s \|\| !s-region \|\| (partN1 && ! | `reclaim.c:2051-2069` | `v7:2051 __attribute__((unused, noinline)) static void extc_rc_shadeSlow(void *v)` | high |
| 305 | `reclaim.c:1711-1745` | `- `shade` 缓存（`reclaim.c:1711-1745`）：**2048 格 × 8` | - shade 缓存（）：2048 格 × 8 B = 16 KB 静态；索引 ((addr3) + gen) & 20 | `reclaim.c:2009-2050` | `v7:2009 static uint64_t extc_rc_shadeGen = 1; ; v7:2010 static ExtcRcShadeCell e` | high |
| 306 | `reclaim.c:1805-1812` | `- `shadeOld`（`reclaim.c:1805-1812`）**只在 NOCOUNT=1` | - shadeOld（）只在 NOCOUNT=1（无计数档，即默认清扫档）时生效：函数体由 if EXTC_RC_NOC | `reclaim.c:2084-2091` | `v7:2084 __attribute__((unused)) static inline void extc_rc_shadeOld(void *const ` | high |

### 7.3 触发判据（逐字公式）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 355 | `reclaim.c:3178-3227` | `xtc_rc_thrUpd`（`reclaim.c:3178-3227`）：` | extc_rc_thrUpd（）： | `reclaim.c:3514-3563` | `v7:3514 __attribute__((unused)) static void extc_rc_thrUpd(ExtcRcRegion *r) { ; ` | high |
| 365 | `reclaim.c:3239-3249` | `c_shouldSweep`（`reclaim.c:3239-3249`）：` | extc_rc_shouldSweep（）： | `reclaim.c:3575-3585` | `v7:3575 __attribute__((unused)) static inline int extc_rc_shouldSweep(const Extc` | high |
| 376 | `reclaim.c:3231-3238` | `tc_rc_roundOk`（`reclaim.c:3231-3238`）：`allocBytes -` | extc_rc_roundOk（）：allocBytes - roundVisitedBytes = max(live, | `reclaim.c:3567-3574` | `v7:3567 __attribute__((unused)) static inline int extc_rc_roundOk(const ExtcRcRe` | high |
| 378 | `reclaim.c:3141-3150` | ``scale` 的自适应（`reclaim.c:3141-3150`）：一轮扫完**什么也没释放*` | scale 的自适应（）：一轮扫完什么也没释放 ⇒ scale = (scale0?scale:1) EXTC_RC_S | `reclaim.c:3477-3482` | `v7:3477 if (r->swFreed == 0) { ; v7:3478 r->scale = (r->scale > 0 ? r->scale : 1` | high |

### 7.4 "推进"有两个入口、单步预算不同

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 384 | `reclaim.c:3432` | `xtc_rc_step` \| `reclaim.c:3432`（由 `maybeCollec` | 触发点 extc_rc_step · （由 maybeCollect 在轮在飞时调） | `reclaim.c:3796-3803` | `v7:3796 __attribute__((unused)) static size_t extc_rc_step(ExtcRcRegion *r, void` | high |
| 385 | `reclaim.c:1250-1272` | `markAdvance` \| `reclaim.c:1250-1272`，由 `extc_rc_all` | 分配点 markAdvance · ，由 extc_rc_alloc 调 | `reclaim.c:1350-1373` | `v7:1350 __attribute__((unused)) static inline __attribute__((always_inline)) siz` | high |
| 387 | `reclaim.c:1263-1264` | `lamp 之后恒等于 256（`reclaim.c:1263-1264`；只有用 `-D` 把 `MA` | markAdvance 的窗口增益在默认参数下永不生效：基数 648 = 512 恒大于 MARK_BATCH_MAX  | `reclaim.c:1364` | `v7:1364 if (b > (size_t)EXTC_RC_MARK_BATCH_MAX * (size_t)_pm) b = (size_t)EXTC_R` | high |

### 7.5 谁有资格开启一轮：只扫"本帧拥有"的 region

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 395 | `reclaim.c:2486-2494` | `- `tlsDepth`（`reclaim.c:2486-2494`）：worker 入口帧（de` | - tlsDepth（）：worker 入口帧（depth==0，唯一调用者是 C 运行时）可以清扫（外层没有 extC | `reclaim.c:2799-2807` | `v7:2799 static _Thread_local int32_t extc_rc_tlsDepth __attribute__((unused)); ;` | high |

### 8.1 轮的开始：markBegin（reclaim.c:2503-2514）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 401 | `reclaim.c:2503-2514` | `开始：`markBegin`（`reclaim.c:2503-2514`）` | 8.1 轮的开始：markBegin（） | `reclaim.c:2808-2878` | `v7:2808 __attribute__((unused)) static void extc_rc_markBegin(ExtcRcRegion *r, v` | high |
| 411 | `reclaim.c:2462-2484` | `**未接线** \| 结构体在 `reclaim.c:2462-2484` 存在，但 `codegen.` | 帧根链 [rt:chain] · 未接线 | `reclaim.c:2767-2798` | `v7:2767 typedef struct ExtcRcFrameRoot { ; v7:2768 ExtcRcSlot *addr;    /* array` | high |
| 412 | `reclaim.c:2268-2290` | `区 \| **未接线** \| `reclaim.c:2268-2290` 有 `extc_rc_bum` | bump 区 · 未接线 | `reclaim.c:2548-2572` | `v7:2548 __attribute__((unused)) static void *extc_rc_bump(ExtcRcRegion *r, size_` | high |

### 8.2 黑化：MARK_SCAN + shade 缓存

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 416 | `reclaim.c:2585-2653` | `可达。`MARK_SCAN`（`reclaim.c:2585-2653`）先判 `mark` 去重，再` | - mark 位图：置位 = 黑 = 可达。MARK_SCAN（）先判 mark 去重，再判 occ。 | `reclaim.c:2897-2965` | `v7:2897 #define EXTC_RC_MARK_SCAN(_rawIn) do { \ ; v7:2898 uintptr_t _raw = (_ra` | high |

### 8.3 遍历顺序与批量

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 423 | `reclaim.c:2752` | `op **立即 scan**（`reclaim.c:2752/2764`），避免为小图付批量` | - 栈深 ≤ EXTC_RC_MARK_SHALLOW = 64 时走"浅栈直通"：逐个 pop 立即 scan（/27 | `reclaim.c:3064` | `v7:3064 if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0 ; v7:30` | high |
| 425 | `reclaim.c:1028` | `X` 直接 `#error`（`reclaim.c:1028`）。` | - POP BATCH_MAX 直接 error（）。 | `reclaim.c:1126` | `v7:1126 #if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX ; v7:1126 #if EXTC_R` | high |

### 8.4 精确 tracing vs 保守

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 436 | `reclaim.c:2562-2566` | `/scanOff`，分段续扫（`reclaim.c:2562-2566`）。` | - 大对象/保守对象：words EXTC_RC_SCAN_CHUNK = 8192（64 KiB/段）时不进标记栈，只 | `reclaim.c:2925-2927` | `v7:2925 if (words > (size_t)EXTC_RC_SCAN_CHUNK) {   /* very long: this step only` | high |

### 9.1 状态机

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 455 | `reclaim.c:3066-3074` | `\| 开始 \| `reclaim.c:3066-3074 sweepBegin` \| `` | 开始 · sweepBegin | `reclaim.c:3395-3408` | `v7:3395 __attribute__((unused)) static void extc_rc_sweepBegin(ExtcRcRegion *r) ` | high |
| 456 | `reclaim.c:3076-3086` | `\| 扫一个 word \| `reclaim.c:3076-3086 sweepWord` \| `d` | 扫一个 word · sweepWord | `reclaim.c:3410-3421` | `v7:3410 __attribute__((unused)) static int extc_rc_sweepWord(ExtcRcRegion *r) { ` | high |
| 457 | `reclaim.c:3090-3152` | `\| 推进 \| `reclaim.c:3090-3152 sweepStep` \| `w` | 推进 · sweepStep | `reclaim.c:3425-3487` | `v7:3425 __attribute__((unused)) static int extc_rc_sweepStep(ExtcRcRegion *r, ui` | high |
| 458 | `reclaim.c:3141-3150` | `\| 结算 \| `reclaim.c:3141-3150` \| `sweeping=0`` | 结算 · sweeping=0；swFreed==0 ⇒ scale=4（clamp）否则 scale=0；thrUpd | `reclaim.c:3477-3482` | `v7:3477 if (r->swFreed == 0) { ; v7:3478 r->scale = (r->scale > 0 ? r->scale : 1` | high |

### 9.2 步长与成本

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 462 | `reclaim.c:970` | `ord = 512 个槽**（`reclaim.c:970`）。` | - EXTC_RC_SWEEP_BUDGET = 8 ⇒ 每次 sweepStep 最多扫 8 个 64-bit wor | `reclaim.c:1013` | `v7:1013 #define EXTC_RC_SWEEP_BUDGET 8 ; v7:1013 #define EXTC_RC_SWEEP_BUDGET 8` | high |
| 463 | `reclaim.c:1506-1545` | `ord` 是**整词批量**（`reclaim.c:1506-1545`）：word 里每个 dead` | - freeSlotWord 是整词批量（）：word 里每个 dead 槽头插进 freelist，链的下一个槽号写在 | `reclaim.c:1765-1804` | `v7:1765 __attribute__((unused)) static size_t extc_rc_freeSlotWord(ExtcRcSlab *s` | high |

### 9.3 sweep 与 mark 位图的耦合（三条后果）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 468 | `reclaim.c:3004` | `点只续扫、**不新开标记**（`reclaim.c:3004` 注释）。` | 1. mark 位图按 word 顺序被清 ⇒ 标记必须从干净的 mark 位图开始；清扫未完成时触发点只续扫、不新开标 | `reclaim.c:3321-3324` | `v7:3321 *   - while a sweep is unfinished, trigger points only continue it and n` | high |
| 469 | `reclaim.c:1349-1353` | `分配的槽必须在分配点补黑**（`reclaim.c:1349-1353`），且**只标清扫游标尚未到达` | 2. 清扫期间分配的槽必须在分配点补黑（），且只标清扫游标尚未到达的位置（已扫过的位置标了会留下无人清的陈旧位）。判据是 | `reclaim.c:1603-1607` | `v7:1603 /* An allocation during sweep is marked black, **only at positions the c` | high |
| 470 | `reclaim.c:2947-2949` | `自己负责清整片 `mark`（`reclaim.c:2947-2949`），理由同上。`freeSlo` | 3. 定向释放自己负责清整片 mark（），理由同上。freeSlot 故意不清 mark（free 槽不参与本轮标记， | `reclaim.c:3264-3266` | `v7:3264 extc_rc_shadeNextGen();   /* the narrowed sweep clears the whole mark sp` | high |

### 9.4 slab 永不归还

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 475 | `reclaim.c:2358-2424` | `egionFree`（帧销毁，`reclaim.c:2358-2424`）：逐 slab `free`` | - slab 的唯一释放点是 extc_rc_regionFree（帧销毁，）：逐 slab free 位图、tid/t | `reclaim.c:2638-2729` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |

### 9.5 slab 级省略：整块全死时一次决策（P1-7，仅默认无计数档）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 485 | `reclaim.c:3021-3058` | `\| 位置 \| `reclaim.c:3021-3058 extc_rc_sweepEl` | 位置 · extc_rc_sweepElide（包在 #if EXTC_RC_NOCOUNT 内；计数档是 stub，r | `reclaim.c:3338-3388` | `v7:3338 __attribute__((unused)) static int extc_rc_sweepElide(ExtcRcRegion *r) {` | high |
| 485 | `reclaim.c:3060-3064` | `T` 内；计数档是 stub，`reclaim.c:3060-3064`） \|` | 位置 · reclaim.c:3021-3058 extc_rc_sweepElide（包在 #if EXTC_RC_N | `reclaim.c:3389-3393` | `v7:3389 __attribute__((unused)) static int extc_rc_sweepElide(ExtcRcRegion *r) {` | high |
| 486 | `reclaim.c:3100` | `\| 调用点 \| `reclaim.c:3100`：`sweepStep` 在` | 调用点 · ：sweepStep 在 slab 边界（swWord==0）先试；命中则 _left--、_trans++ | `reclaim.c:3435` | `v7:3435 if (r->swWord == 0 && extc_rc_sweepElide(r)) { _left--; _trans++; contin` | high |

### 10.2 运行期入口与三道闸

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 520 | `reclaim.c:2984-2990` | `\| 入口 \| `reclaim.c:2984-2990` \| `shade(v)` →` | 入口 · shade(v) → releaseDead(home, slot, roots, nroots) → sto | `reclaim.c:3301-3310` | `v7:3301 __attribute__((unused)) static void extc_rc_storeRefOwnedRelease(void **` | high |
| 521 | `reclaim.c:2973` | `\| 闸 1 \| `reclaim.c:2973` \| `home->noSwe` | 闸 1 · home->noSweep \ | `reclaim.c:3290` | `v7:3290 if (home->noSweep \|\| home->owe) return;   /* skip:nosweep */ ; v7:3290 i` | high |
| 522 | `reclaim.c:2975-2977` | `\| 闸 2 \| `reclaim.c:2975-2977` \| 旧值必须 `ownSla` | 闸 2 · 旧值必须 ownSlab(old) 且 s->region == home，否则返回（旧值不在本 regio | `reclaim.c:3292-3294` | `v7:3292 ExtcRcSlab *s = extc_rc_ownSlab(old); ; v7:3293 /* the old value is not ` | high |
| 523 | `reclaim.c:2978-2980` | `\| 闸 3 \| `reclaim.c:2978-2980` \| `!budgetOk(h` | 闸 3 · !budgetOk(home) ⇒ 返回；否则 releaseOld 后 home->visitedByte | `reclaim.c:3295` | `v7:3295 if (!extc_rc_budgetOk(home)) return;   /* skip:release-budget */ ; v7:32` | high |
| 524 | `reclaim.c:2315-2318` | `\| 信用 \| `reclaim.c:2315-2318` \| `budgetOk: a` | 信用 · budgetOk: allocBytes - visitedBytes >= max(live, floor) | `reclaim.c:2595-2607` | `v7:2595 __attribute__((unused)) static int extc_rc_budgetOk(const ExtcRcRegion *` | high |

### 10.3 主体（reclaim.c:2883-2954）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 526 | `reclaim.c:2883-2954` | `### 10.3 主体（`reclaim.c:2883-2954`）` | 10.3 主体（） | `reclaim.c:3206-3262` | `v7:3206 /* Phase 1 marks the live set from the other roots; phase 2 descends fro` | high |
| 534 | `reclaim.c:2863-2882` | `**安全性论证**（`reclaim.c:2863-2882` 逐字要点）：释放集是"同一时` | 安全性论证（ 逐字要点）：释放集是"同一时刻、同一 seed 集下 collect 会释放的集合"的子集（轻量级 tri | `reclaim.c:3206-3209` | `v7:3206 /* Phase 1 marks the live set from the other roots; phase 2 descends fro` | high |

### 10.4 成本：O(子树)，无界

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 542 | `reclaim.c:1668-1694` | `当场 freeSlot"路径（`reclaim.c:1668-1694`），两者互不替代。` | 它与计数层的关系：计数层（NOCOUNT=0）另有一条独立的"hold 减到 0 当场 freeSlot"路径（），两者 | `reclaim.c:1946-1978` | `v7:1946 __attribute__((unused)) static void extc_rc_holdUpdSlab(ExtcRcSlab *s, v` | high |

### 11.1 跨区边：rsbit 是缓存，rscnt 是精确计数

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 552 | `reclaim.c:216-250` | `\| `rsbit` \| 位图 `reclaim.c:216-250`；维护点 `1415/1426` | rsbit · 位图 ；维护点 1415/1426/1490 | `reclaim.c:219-225` | `v7:219 uint64_t     *occ;       /* occupancy bitmap */ ; v7:220 uint64_t     *ma` | high |
| 554 | `reclaim.c:1368-1372` | `\| 边的三处转移 \| `reclaim.c:1368-1372`（3 条逐字）；实现 `140` | 边的三处转移 · （3 条逐字）；实现 1408-1469 | `reclaim.c:1622-1626` | `v7:1622 * The count is transferred in three places: ; v7:1623 *   1. `extc_rc_st` | high |
| 555 | `reclaim.c:1453-1469` | `\| 撤销时机 \| `reclaim.c:1453-1469 unlinkChildren`` | 撤销时机 · unlinkChildren | `reclaim.c:1707-1725` | `v7:1707 __attribute__((unused)) static void extc_rc_unlinkChildren(ExtcRcSlab *s` | high |
| 556 | `reclaim.c:1373` | `\| 为什么不是只置不清 \| `reclaim.c:1373` 注释 \| 只置不清会让种子涨` | 为什么不是只置不清 · 注释 | `reclaim.c:1626` | `v7:1626 * So the number of registered bits equals the current number of live cro` | high |
| 557 | `reclaim.c:1377` | `\| 高水位 \| `reclaim.c:1377` `rsIncN/rsDecN` | 高水位 · rsIncN/rsDecN/rsHwm | `reclaim.c:1631` | `v7:1631 __attribute__((unused)) static int64_t extc_rc_rsIncN, extc_rc_rsDecN, e` | high |
| 558 | `reclaim.c:1393-1400` | `\| 运行期守卫 \| `reclaim.c:1393-1400` `EXTC_RC_FOREI` | 运行期守卫 · EXTC_RC_FOREIGN_GUARD（默认 0） | `reclaim.c:1648-1655` | `v7:1648 #define EXTC_RC_FOREIGN_GUARD 0 ; v7:1649 #endif` | high |

### 11.2 pin：把"值本身"钉成根

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 566 | `reclaim.c:1968-1981` | `\| 动机 \| `reclaim.c:1968-1981` 注释 \| 宿主（view 元` | 动机 · 注释 | `reclaim.c:2247-2260` | `v7:2248 /* ---- Slots whose host cannot be proven: pin the value itself as a roo` | high |
| 567 | `reclaim.c:1982-2046` | `\| 结构 \| `reclaim.c:1982-2046` \| 进程级开放寻址哈希（键=` | 结构 · 进程级开放寻址哈希（键=值指针，load ≤ 1/2，删除用 Knuth 后移），每项存 v/rg/n（rg  | `reclaim.c:2262-2325` | `v7:2262 typedef struct ExtcRcPin { void *v; ExtcRcRegion *rg; size_t n; } ExtcRc` | high |
| 568 | `reclaim.c:1979` | `\| 方向 \| `reclaim.c:1979` \| 只有 pin **不可能` | 方向 · 只有 pin 不可能（分配失败）才退回 noSweep —— 宁少收，不可错杀 | `reclaim.c:2258` | `v7:2258 * ; v7:2258 *` | high |
| 569 | `reclaim.c:1980` | `\| 线程 \| `reclaim.c:1980` \| pin 表**进程级**` | 线程 · pin 表进程级（region 可能被别的 worker sweep），insert/remove/轮起加锁； | `reclaim.c:2260` | `v7:2260 * Threads: the pin table is process-wide (a region may be swept by anoth` | high |
| 570 | `reclaim.c:2025-2046` | `\| `pinAdd` \| `reclaim.c:2025-2046` \| `!v \\|\\| !ow` | pinAdd · !v \ | `reclaim.c:2305-2326` | `v7:2305 __attribute__((unused)) static int extc_rc_pinAdd(void *v) { ; v7:2306 i` | high |
| 571 | `reclaim.c:2047-2060` | `\| `pinSub` \| `reclaim.c:2047-2060` \| `n--` 或擦除（Kn` | pinSub · n-- 或擦除（Knuth 后移） | `reclaim.c:2327-2340` | `v7:2327 static __attribute__((unused)) void extc_rc_pinSub(void *v) { ; v7:2328 ` | high |
| 572 | `reclaim.c:2062-2072` | `nDropRegion` \| `reclaim.c:2062-2072` \| region 死时删掉它` | pinDropRegion · region 死时删掉它名下所有 pin（由 regionFree 调） | `reclaim.c:2342-2357` | `v7:2342 static __attribute__((unused)) void extc_rc_pinDropRegion(ExtcRcRegion *` | high |
| 573 | `reclaim.c:2079-2087` | `\| `pinSeed` \| `reclaim.c:2079-2087`（`noinline`） \|` | pinSeed · （noinline） | `reclaim.c:2359-2369` | `v7:2359 static __attribute__((unused)) __attribute__((noinline)) void extc_rc_pi` | high |

### 11.3 noSweep：永久退出 sweep

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 579 | `reclaim.c:2091-2101` | ``pinAdd` 失败 \| `reclaim.c:2091-2101`（`2096-2098` 置位` | storeRefUnknown 的 pinAdd 失败 · （2096-2098 置位） | `reclaim.c:2371-2381` | `v7:2371 __attribute__((unused)) static void extc_rc_storeRefUnknown(void **slot,` | high |
| 580 | `reclaim.c:2143-2161` | `ostKnown==0` \| `reclaim.c:2143-2161`（`2158` 置位） \| 同` | storeStructCons 且 hostKnown==0 · （2158 置位） | `reclaim.c:2423-2441` | `v7:2423 __attribute__((unused)) static void extc_rc_storeStructCons(void *dst, s` | high |
| 581 | `reclaim.c:2480-2484` | `帧根链存在 bad 帧 \| `reclaim.c:2480-2484 chainBad` \| 调用方` | 帧根链存在 bad 帧 · chainBad | `reclaim.c:2785-2797` | `v7:2785 __attribute__((unused)) static int extc_rc_chainBad(void) { ; v7:2786 fo` | high |
| 586 | `reclaim.c:3243-3244` | `dSweep` 直接返回 0（`reclaim.c:3243-3244`，注释 "reclaimed` | - shouldSweep 直接返回 0（，注释 "reclaimed by counting only"）⇒ 永不触发 | `reclaim.c:3576` | `v7:3576 if (r->noSweep) return 0;   /* an in-edge whose host cannot be traced: t` | high |

### 12.1 默认（清扫档）：整层被编译掉

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 599 | `reclaim.c:1649-1664` | ``reclaim.c:1649-1664` 是 `[rt:nocount` | 是 [rt:nocount] 段的开关：EXTC_RC_NOCOUNT=1 时 holdUpdSlab/holdUpd/ | `reclaim.c:1934-1942` | `v7:1934 #if EXTC_RC_NOCOUNT ; v7:1935 __attribute__((unused)) static void extc_r` | high |
| 606 | `reclaim.c:3593` | `2. 发射器 `reclaim.c:3593` 写出 `#define EX` | 2. 发射器 写出 define EXTC_RC_NOCOUNT 1（=0 时写 0）。 | `reclaim.c:3957` | `v7:3957 bufPuts(out, noCount ? "#define EXTC_RC_NOCOUNT 1\n" : "#define EXTC_RC_` | high |
| 607 | `reclaim.c:1656` | `3. 运行时 `reclaim.c:1656` `#if EXTC_RC_N` | 3. 运行时 if EXTC_RC_NOCOUNT 把计数入口变成空操作。 | `reclaim.c:1934` | `v7:1934 #if EXTC_RC_NOCOUNT ; v7:1934 #if EXTC_RC_NOCOUNT` | high |

### 12.2 计数语义（NOCOUNT=0）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 616 | `reclaim.c:1682-1694` | ``holdUpd` 五步 \| `reclaim.c:1682-1694` \| ① `ownSlab(p` | holdUpd 五步 · ① ownSlab(p)，非本运行时槽 ⇒ 返回 ② occ 未置位 ⇒ 返回（free 槽不 | `reclaim.c:1960-1974` | `v7:1960 __attribute__((unused)) static void extc_rc_holdUpd(void *p, int delta) ` | high |
| 617 | `reclaim.c:1668-1678` | `holdUpdSlab` \| `reclaim.c:1668-1678` \| 已解析 slab 的版本` | holdUpdSlab · 已解析 slab 的版本，判据逐条相同（省一次 ownSlab） | `reclaim.c:1946-1958` | `v7:1946 __attribute__((unused)) static void extc_rc_holdUpdSlab(ExtcRcSlab *s, v` | high |
| 618 | `reclaim.c:1697-1698` | `\| `inc/dec` \| `reclaim.c:1697-1698` \| 只是 `holdUpd(` | inc/dec · 只是 holdUpd(p,+1) / holdUpd(p,-1) 的别名 | `reclaim.c:1975-1976` | `v7:1975 __attribute__((unused)) static void extc_rc_inc(void *p) { extc_rc_holdU` | high |

### 12.3 计数只在"指针赋值"上动

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 624 | `reclaim.c:1696` | ``reclaim.c:1696` 逐字："Counting h` | 逐字："Counting hooks only pointer assignments: data-field stor | `reclaim.c:1974` | `v7:1974 /* Counting hooks only pointer assignments: data-field stores do not go ` | high |
| 634 | `reclaim.c:123-126` | ``inc/dec`（附加准则 `reclaim.c:123-126`）——所以计数**不跟踪**` | 数据字段赋值不走 inc/dec（附加准则 ）——所以计数不跟踪 memset、结构体逐字节复制、外部 C 代码写入的引 | `reclaim.c:123-126` | `v7:123 * Two further criteria: ; v7:124 *  - marking stops at an object outside ` | high |

### 12.4 与 sweep 的分工、以及计数不准的后果

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 640 | `reclaim.c:1651-1656` | `是"减到 0 立即回收" \| `reclaim.c:1651-1656` 逐字 \|` | 分工 · 标记/清扫完全不依赖计数；计数的唯一角色是"减到 0 立即回收" | `reclaim.c:1928-1933` | `v7:1928 * ; v7:1929 * Marking/sweeping **do not depend on counts** -- `extc_rc_m` | high |
| 648 | `reclaim.c:156-160` | ``do{}while(0)`，`reclaim.c:156-160`）：`alloc(0) 129` | TICK 调用点（默认 EXTC_RC_TIME 关时全部编译成 do{}while(0)，）：alloc(0) 129 | `reclaim.c:156-160` | `v7:156 /* Per-component counting is off by default: the counting entry point is ` | high |

### 15.3 线程模型（准确表述）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 692 | `reclaim.c:400-407` | `\| slab 登记表（自旋锁 `reclaim.c:400-407` + 按代发布 + **旧代永` | slab 登记表（自旋锁 + 按代发布 + 旧代永不释放；读者无锁探测，ownFind miss 后读 extc_rc_ | `reclaim.c:403-416` | `v7:403 typedef struct ExtcRcReg { ExtcRcSlab **tab; size_t cap; size_t n; } Extc` | high |
| 704 | `reclaim.c:2096-2098` | `eep/owe 跨线程写 \| `reclaim.c:2096-2098/2158`（`noSweep`` | noSweep/owe 跨线程写 · /2158（noSweep 仅此两处写）、:3493（owe；3494 只读） | `reclaim.c:2376-2378` | `v7:2376 if (!extc_rc_pinAdd(v)) { ; v7:2377 ExtcRcRegion *r = extc_rc_regionOfOw` | high |
| 705 | `reclaim.c:2025-2046` | `n 表项 `rg` 缓存 \| `reclaim.c:2025-2046` \| 加锁维护；`rg` 指向` | pin 表项 rg 缓存 · 加锁维护；rg 指向的 region 可能已被别的线程 regionFree（pinDro | `reclaim.c:2305-2326` | `v7:2305 __attribute__((unused)) static int extc_rc_pinAdd(void *v) { ; v7:2306 i` | high |

### 16.1 诊断（仅显式开启时存在）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 715 | `reclaim.c:3283-3378` | `_RC_PAUSE=1` \| `reclaim.c:3283-3378`（`pauseDump`）、3` | EXTC_RC_PAUSE=1 · （pauseDump）、3379-3398（pauseArm：getenv 3382 | `reclaim.c:3619-3724` | `v7:3619 __attribute__((unused)) static void extc_rc_pauseDump(void) { ; v7:3620 ` | high |
| 716 | `reclaim.c:3393-3395` | `OWSTEP=<us>` \| `reclaim.c:3393-3395` 只设阈值，超阈值在 `282` | EXTC_RC_SLOWSTEP=<us> · 只设阈值，超阈值在 2820/3122 打印 [slow] | `reclaim.c:3740-3741` | `v7:3740 const char *_ss = getenv("EXTC_RC_SLOWSTEP"); ; v7:3741 extc_rc_slowUs =` | high |
| 717 | `reclaim.c:1905-1921` | `TC_RC_STATS` \| `reclaim.c:1905-1921`（`statsArm 1913` | EXTC_RC_STATS · （statsArm 1913） | `reclaim.c:2184-2203` | `v7:2184 __attribute__((unused)) static void extc_rc_statsDump(void) { ; v7:2185 ` | high |
| 720 | `reclaim.c:3441` | `tc_rc_collect`（`reclaim.c:3441`，标记到底再 settle）、` | "能全量回收"的接口存在但不是生产路径：extc_rc_collect（，标记到底再 settle）、extc_rc_s | `reclaim.c:3805-3814` | `v7:3805 __attribute__((unused)) static size_t extc_rc_collect(ExtcRcRegion *r, v` | high |

### 17.1 环境变量（改变"发射什么"）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 751 | `reclaim.c:926-938` | ``（负数按 0） \| 运行期 `reclaim.c:926-938`（`mmaxArm`） \| 内` | EXTC_RC_M_MAX · 未设或 0 = 隐式墙 3·live（负数按 0） | `reclaim.c:969-991` | `v7:969 __attribute__((unused)) static void extc_rc_mmaxArm(void) {` | high |
| 752 | `reclaim.c:990` | `越界 `#error`） \| `reclaim.c:990` \| 近墙（`bytes ≥` | EXTC_RC_SWEEP_PRESSURE_MUL · 4（1..16，越界 #error） | `reclaim.c:1089` | `v7:1089 #define EXTC_RC_SWEEP_PRESSURE_MUL 4 ; v7:1089 #define EXTC_RC_SWEEP_PRE` | high |
| 753 | `reclaim.c:1053` | `U` \| 未设 = 墙钟 \| `reclaim.c:1053` \| =1 时 `[histo` | EXTC_RC_STEP_CPU · 未设 = 墙钟 | `reclaim.c:1153` | `v7:1153 static int extc_rc_stepCpu;   /* EXTC_RC_STEP_CPU: time each step with t` | high |

### 17.2 编译期常量（全部 #ifndef，可 -D 覆盖）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 792 | `reclaim.c:1001` | `IFT_MARK` \| 3 \| reclaim.c:1001 \| mark 配额 = n >` | EXTC_RC_QUOTA_SHIFT_MARK · mark 配额 = n >> 3（每 8 请求字节 1 个工作单位 | `reclaim.c:1037` | `v7:1037 #define EXTC_RC_QUOTA_SHIFT_MARK 3         /* one mark work unit per 8 r` | high |
| 793 | `reclaim.c:1004` | `WORDS` \| 8192 \| reclaim.c:1004 \| sweep 配额上限（合法` | EXTC_RC_QUOTA_MAX_WORDS · sweep 配额上限（合法 slab 至多约 2048 words  | `reclaim.c:1040` | `v7:1040 #define EXTC_RC_QUOTA_MAX_WORDS 8192      /* hard cap on the extra sweep` | high |
| 794 | `reclaim.c:1008` | `MARK` \| 1<<20 \| reclaim.c:1008 \| mark 配额上限（rec` | EXTC_RC_QUOTA_MAX_MARK · 1<<20 | `reclaim.c:1044` | `v7:1044 #define EXTC_RC_QUOTA_MAX_MARK (1 << 20)  /* memory-first default: enoug` | high |
| 794 | `reclaim.c:1366` | `008 \| mark 配额上限（reclaim.c:1366 才真正钳位）；调小换停顿、但` | EXTC_RC_QUOTA_MAX_MARK · 1<<20 | `reclaim.c:1478` | `v7:1478 if (_quota > (size_t)EXTC_RC_QUOTA_MAX_MARK) { _quota = (size_t)EXTC_RC_` | high |
| 795 | `reclaim.c:1022` | `INE_US` \| 800 \| reclaim.c:1022 \| 配额循环的物理时间封顶（0` | EXTC_RC_QUOTA_DEADLINE_US · 配额循环的物理时间封顶（0=关）；判定点在 reclaim.c: | `reclaim.c:1058` | `v7:1058 #define EXTC_RC_QUOTA_DEADLINE_US 800 ; v7:1058 #define EXTC_RC_QUOTA_DE` | high |
| 795 | `reclaim.c:1379-1383` | `理时间封顶（0=关）；判定点在 reclaim.c:1379-1383 \|` | EXTC_RC_QUOTA_DEADLINE_US · reclaim.c:1022 | `reclaim.c:1489-1499` | `v7:1489 #if EXTC_RC_QUOTA_DEADLINE_US > 0 ; v7:1490 if (_dl0 >= 0) {` | high |
| 796 | `reclaim.c:1064` | `\| **0（默认关闭）** \| reclaim.c:1064 \| P0-B 惰性复用链重建（` | EXTC_RC_LAZY_FREE · 0（默认关闭） | `reclaim.c:1064` | `v7:1064 #define EXTC_RC_LAZY_FREE 0 ; v7:1064 #define EXTC_RC_LAZY_FREE 0` | high |
| 797 | `reclaim.c:1069` | `E_VERIFY` \| 0 \| reclaim.c:1069 \| **编译期宏**（`-D`` | EXTC_RC_ELIDE_VERIFY · 编译期宏（-D）：slab 级省略命中前重扫 occ/mark/rsbit | `reclaim.c:1069` | `v7:1069 #define EXTC_RC_ELIDE_VERIFY 0 ; v7:1069 #define EXTC_RC_ELIDE_VERIFY 0` | high |
| 798 | `reclaim.c:799` | `EE_DEFER` \| 1 \| reclaim.c:799 \| `regionFree`` | EXTC_RC_FREE_DEFER · regionFree 两阶段：1 = slab 链挂 thread-local | `reclaim.c:799` | `v7:799 #define EXTC_RC_FREE_DEFER 1              /* 1 = queue a dropped region's` | high |
| 799 | `reclaim.c:802` | `ET_SLABS` \| 8 \| reclaim.c:802 \| 队列排空每趟最多处理的 s` | EXTC_RC_FREE_BUDGET_SLABS · 队列排空每趟最多处理的 slab 数 | `reclaim.c:802` | `v7:802 #define EXTC_RC_FREE_BUDGET_SLABS 8       /* normal drain: at most this m` | high |
| 800 | `reclaim.c:805` | `INE_US` \| 100 \| reclaim.c:805 \| 队列排空每趟的时间上限（与` | EXTC_RC_FREE_DEADLINE_US · 队列排空每趟的时间上限（与 slab 数取先到者） | `reclaim.c:805` | `v7:805 #define EXTC_RC_FREE_DEADLINE_US 100      /* normal drain also stops afte` | high |
| 801 | `reclaim.c:808` | `YTES` \| 8 MiB \| reclaim.c:808 \| 队列字节上限；超过则改用硬` | EXTC_RC_FREE_QUEUE_BYTES · 8 MiB | `reclaim.c:808` | `v7:808 #define EXTC_RC_FREE_QUEUE_BYTES (8 << 20)  /* queue byte limit; above it` | high |
| 802 | `reclaim.c:811` | `NE_US` \| 1000 \| reclaim.c:811 \| 强制排空的时间上限（即 G` | EXTC_RC_FREE_HARD_DEADLINE_US · 强制排空的时间上限（即 G3b 的 1 ms 判据） | `reclaim.c:811` | `v7:811 #define EXTC_RC_FREE_HARD_DEADLINE_US 1000  /* forced drain bound: one pa` | high |

### 18.1 逐基质上界（可证）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 820 | `reclaim.c:2358-2424` | `Free`） \| §5、§9；`reclaim.c:2358-2424` \| pb ≈ 20 B/节点` | rc region · M_rc(t) = Σ_{r 存活} S_r(t)；S_r(t) ≤ 该 region 历史同时 | `reclaim.c:2638-2729` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |

### 18.2 触发线推出的稳态垃圾上界

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 829 | `reclaim.c:3239-3249` | `触发判据（`src/back/reclaim.c:3239-3249`）：`shouldSweep` | 触发判据（src/back/）：shouldSweep = bytes thr && roundOk；thr = max | `reclaim.c:3575-3585` | `v7:3575 __attribute__((unused)) static inline int extc_rc_shouldSweep(const Extc` | high |

### 18.3 真正的无界条件（一条都不能漏）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 852 | `reclaim.c:2096-2098` | `*：`noSweep` 只在 `reclaim.c:2096-2098`（pin 表满）与 `:215` | 2. noSweep / owe 永久置位：noSweep 只在 （pin 表满）与 :2158（宿主未知的保守 sto | `reclaim.c:2376-2378` | `v7:2376 if (!extc_rc_pinAdd(v)) { ; v7:2377 ExtcRcRegion *r = extc_rc_regionOfOw` | high |
| 854 | `reclaim.c:144` | `LE_MAX = 2^40`（`reclaim.c:144`）⇒ 连续空扫后阈值事实上无穷` | 4. 阈值上限：scale 可乘到 EXTC_RC_SCALE_MAX = 2^40（）⇒ 连续空扫后阈值事实上无穷 ⇒ | `reclaim.c:144` | `v7:144 #define EXTC_RC_SCALE_MAX ((int64_t)1 << 40) ; v7:144 #define EXTC_RC_SCA` | high |
| 854 | `reclaim.c:3185-3187` | `thr` 乘法溢出时被置 0（`reclaim.c:3185-3187`）⇒ 反而每次分配都扫。` | 4. 阈值上限：scale 可乘到 EXTC_RC_SCALE_MAX = 2^40（reclaim.c:144）⇒ 连 | `reclaim.c:3521` | `v7:3521 if (scale > 1 && thr > INT64_MAX / scale) { r->thr = 0; return; } ; v7:3` | high |
| 856 | `reclaim.c:3142-3147` | `scale**：`scale`（reclaim.c:3142-3147；空扫 ×4、上限 2^40）不` | 6. 空扫把上界整体乘 scale：scale（；空扫 ×4、上限 2^40）不只决定"多久扫一次"，它把 thr 直接 | `reclaim.c:3477-3482` | `v7:3477 if (r->swFreed == 0) { ; v7:3478 r->scale = (r->scale > 0 ? r->scale : 1` | high |

### 18.4 结论

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 864 | `reclaim.c:2358` | `egion 存活期不还 OS，`reclaim.c:2358` 只在 drop 时 free` | - 且 RSS = 历史峰值（region 存活期不还 OS， 只在 drop 时 free）。 | `reclaim.c:2638` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |

### 18.5 内存墙契约（本轮引入：P0-4 + P1-1 + P1-2）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 874 | `reclaim.c:926-938` | `数按 0)        // reclaim.c:926-938 的 mmaxArm` | M_max = EXTC_RC_M_MAX (未设/0 ⇒ 隐式 3·live；负数按 0) // 的 mmaxArm | `reclaim.c:969-991` | `v7:969 __attribute__((unused)) static void extc_rc_mmaxArm(void) {` | high |
| 875 | `reclaim.c:3178` | `b=256 ⇒ live/4；reclaim.c:3178/3152）` | thr = min( max(2·live, floor)·scale , M_max − live/4 ) // th | `reclaim.c:3514` | `v7:3514 __attribute__((unused)) static void extc_rc_thrUpd(ExtcRcRegion *r) { ; ` | high |
| 876 | `reclaim.c:952-966` | `// 近墙判据（两条，reclaim.c:952-966）` | pressure = (bytes ≥ 7/8·M_max) \|\| (thr0 && bytes 4·thr) // 近 | `reclaim.c:995-1007` | `v7:995 __attribute__((unused)) static inline int extc_rc_pressure(const ExtcRcRe` | high |
| 877 | `reclaim.c:990` | `// 默认 4（reclaim.c:990）` | 近墙时每步预算 × EXTC_RC_SWEEP_PRESSURE_MUL // 默认 4（） | `reclaim.c:1089` | `v7:1089 #define EXTC_RC_SWEEP_PRESSURE_MUL 4 ; v7:1089 #define EXTC_RC_SWEEP_PRE` | high |
| 881 | `reclaim.c:897-909` | `要涨 ~2.75× 才再触发（`reclaim.c:897-909` 上方注释逐字：an unbo` | - 代价（必须一并承诺）：夹紧 thr 使 sweep 次数从小常数变成 O(log N) —— 一轮最多退休一个活集， | `reclaim.c:931-951` | `v7:931 __attribute__((unused)) static size_t extc_rc_partN;` | high |
| 883 | `reclaim.c:3199-3207` | `; using floor`（`reclaim.c:3199-3207`）并退回 floor ⇒ 此时` | - 墙不可达警告：若 M_max 小到连 floor 都放不下（max(2L,floor) 压不到墙下），运行期打一次  | `reclaim.c:3539-3543` | `v7:3539 if (!extc_rc_wallWarned) {   /* report once, and only when the wall was ` | high |

### 18.6 前提的修正：回收配额按分配字节计（P0-P，已落地）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 896 | `reclaim.c:1000-1022` | `**已落地（P0-P；宏 `reclaim.c:1000-1022`、实现 `reclaim.c:` | 已落地（P0-P；宏 、实现 reclaim.c:1365-1390）：配额改为按分配字节给—— | `reclaim.c:1029-1059` | `v7:1029 * EXTC_RC_QUOTA_DEADLINE_US adds a physical ceiling to the mark quota lo` | high |
| 896 | `reclaim.c:1365-1390` | `:1000-1022`、实现 `reclaim.c:1365-1390`）**：配额改为按分配字节给—` | 已落地（P0-P；宏 reclaim.c:1000-1022、实现 ）：配额改为按分配字节给—— | `reclaim.c:1475-1508` | `v7:1475 if (r->marking) { ; v7:1476 if (r->msp != 0 \|\| !r->seedsDone \|\| r->scanO` | high |

### 19.1 单位预算（结构性，全部是编译期常量）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 913 | `reclaim.c:1005` | `MARK_STEP 8` \| `reclaim.c:1005/1008/1020/1023/` | 标记推进（分配点触发） · 种子 ≤ SEED_PUSHES 256 pushes / SEED_WORDS 512 w | `reclaim.c:1104` | `v7:1104 #define EXTC_RC_SEED_PUSHES 256  /* how many **seed slots** one step pus` | high |
| 914 | `reclaim.c:2562-2563` | `出记 cursor 续扫 \| `reclaim.c:2562-2563`、`extc_rc_markR` | 大对象扫描 · 每段 SCAN_CHUNK 8192 words = 64 KiB，超出记 cursor 续扫 | `reclaim.c:2874-2875` | `v7:2874 #ifndef EXTC_RC_SCAN_CHUNK ; v7:2875 #define EXTC_RC_SCAN_CHUNK 8192` | high |
| 914 | `reclaim.c:2655-2672` | `c_rc_markResume reclaim.c:2655-2672` \| 单步上界由**段长**而` | 大对象扫描 · 每段 SCAN_CHUNK 8192 words = 64 KiB，超出记 cursor 续扫 | `reclaim.c:2967-2983` | `v7:2967 __attribute__((unused)) static size_t extc_rc_markResume(ExtcRcRegion *r` | high |
| 914 | `reclaim.c:2552-2553` | `2 + 固定步成本`（源码注释 reclaim.c:2552-2553 明确含 fixed step` | 大对象扫描 · 每段 SCAN_CHUNK 8192 words = 64 KiB，超出记 cursor 续扫 | `reclaim.c:2857-2858` | `v7:2857 * Step bound model: a resumed segment walks min(EXTC_RC_SCAN_CHUNK, rema` | high |
| 915 | `reclaim.c:970` | `ords = 512 槽 \| `reclaim.c:970` \| 有界（槽数） \|` | 清扫 · 每次 sweepStep SWEEP_BUDGET 8 words = 512 槽 | `reclaim.c:1013` | `v7:1013 #define EXTC_RC_SWEEP_BUDGET 8 ; v7:1013 #define EXTC_RC_SWEEP_BUDGET 8` | high |
| 916 | `reclaim.c:1000-1022` | `advance 后检查） \| `reclaim.c:1000-1022`、`:1365-1390`、d` | 回收配额（P；分配点触发的第二通道） · Q_mark(n)=min(n>>3, 1<<20) unit、Q_sweep | `reclaim.c:1029-1059` | `v7:1029 * EXTC_RC_QUOTA_DEADLINE_US adds a physical ceiling to the mark quota lo` | high |
| 917 | `reclaim.c:980` | `2 中断 ⇒ 已释放保留 \| `reclaim.c:980`、函数体 `reclaim.c` | 定向释放 releaseOld · 有预算：EXTC_RC_RELEASE_BUDGET 2048 步，两相共用；pha | `reclaim.c:1079` | `v7:1079 #define EXTC_RC_RELEASE_BUDGET 2048 ; v7:1079 #define EXTC_RC_RELEASE_BU` | high |
| 917 | `reclaim.c:2883` | `aim.c:980`、函数体 `reclaim.c:2883`（放弃语义在函数头注释） \|` | 定向释放 releaseOld · 有预算：EXTC_RC_RELEASE_BUDGET 2048 步，两相共用；pha | `reclaim.c:3200-3271` | `v7:3200 __attribute__((unused)) static size_t extc_rc_releaseOld(ExtcRcRegion *r` | high |
| 917 | `reclaim.c:2895-2902` | `坏约 0.1–0.2 ms**（reclaim.c:2895-2902）。预算耗尽只少收不多收 \|` | 定向释放 releaseOld · 有预算：EXTC_RC_RELEASE_BUDGET 2048 步，两相共用；pha | `reclaim.c:3211-3224` | `v7:3211 uint32_t budget = EXTC_RC_RELEASE_BUDGET; ; v7:3212 while (!r->seedsDone` | high |
| 918 | `reclaim.c:2347-2356` | `n/cstk） \| 上界注释 `reclaim.c:2347-2356`、探针 `[regionfre` | regionFree · 不分批，但单次调用有显式上界：每次 drop ≤ 10·#slab + #classes +  | `reclaim.c:2620-2636` | `v7:2620 r->marking = 0; ; v7:2621 r->markDone = 0;` | high |
| 918 | `reclaim.c:3323` | `针 `[regionfree] reclaim.c:3323`、取消轮次 `extc_rc_` | regionFree · 不分批，但单次调用有显式上界：每次 drop ≤ 10·#slab + #classes +  | `reclaim.c:3662` | `v7:3662 fprintf(stderr, "[regionfree] n=%lld max_frees=%lld max_slabs=%lld sync_` | high |
| 918 | `reclaim.c:2337-2346` | ``extc_rc_abort reclaim.c:2337-2346` \| 不扫描活对象图；仅当 `` | regionFree · 不分批，但单次调用有显式上界：每次 drop ≤ 10·#slab + #classes +  | `reclaim.c:2617-2636` | `v7:2617 __attribute__((unused)) static void extc_rc_abort(ExtcRcRegion *r) { ; v` | high |

### 19.2 实测（-O2，默认档 = 档 2，MEASUREMENTS §6）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 931 | `reclaim.c:2895-2902` | `s**（每段计 1024 单位，reclaim.c:2895-2902）；代价是少收，残留垃圾交给 s` | releaseOld 有界（本轮 after） · 2048 步预算生效：无续扫时单次 ≤ 36 µs（τ_t=17.4 | `reclaim.c:3211-3224` | `v7:3211 uint32_t budget = EXTC_RC_RELEASE_BUDGET; ; v7:3212 while (!r->seedsDone` | high |
| 933 | `reclaim.c:2655-2672` | `返回走过的 word 数**（`reclaim.c:2655-2672`，return 在 :2671` | 续扫计量（v2 已修） · extc_rc_markResume 现在返回走过的 word 数（，return 在 :2 | `reclaim.c:2967-2983` | `v7:2967 __attribute__((unused)) static size_t extc_rc_markResume(ExtcRcRegion *r` | high |

### 19.3 目前不成立的通道（关键，必须写进结论）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 937 | `reclaim.c:2337-2346` | ``extc_rc_abort`（reclaim.c:2337-2346）只取消轮次，不扫描活对象图。它` | 1. regionFree（帧销毁）：不分批，但单次调用的 free 次数有显式上界（≤ 10·slab + class | `reclaim.c:2617-2636` | `v7:2617 __attribute__((unused)) static void extc_rc_abort(ExtcRcRegion *r) { ; v` | high |

### 19.4 形式化的停顿界

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 954 | `reclaim.c:2562` | `\| `SCAN_CHUNK` reclaim.c:2562 \|` | w · 每段扫描 word 数 | `reclaim.c:2874` | `v7:2874 #ifndef EXTC_RC_SCAN_CHUNK ; v7:2874 #ifndef EXTC_RC_SCAN_CHUNK` | high |
| 957 | `reclaim.c:980` | `RELEASE_BUDGET` reclaim.c:980 \|` | f · 单次释放槽数 | `reclaim.c:1079` | `v7:1079 #define EXTC_RC_RELEASE_BUDGET 2048 ; v7:1079 #define EXTC_RC_RELEASE_BU` | high |
| 959 | `reclaim.c:2358-2424` | `模 = n） \| 未标定 \| `reclaim.c:2358-2424`；§19.3-1 \|` | c_r · 每槽的区域级释放成本（regionFree 内逐槽归还 slab / 记账；未分批时规模 = n） | `reclaim.c:2638-2729` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |

### 19.5.1 G1② 的量具改成三通道（否则判据不可审计）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 981 | `reclaim.c:1100` | `量具实现：声明 `reclaim.c:1100`、记录 `:2941-2943` | 量具实现：声明 、记录 :2941-2943、打印 :3445-3447；empirics 侧 tests/gc-p0/ | `reclaim.c:1147` | `v7:1147 static long long extc_rc_segN, extc_rc_segPops, extc_rc_segMax, extc_rc_` | high |

### §20 与仓库既有文档的不一致清单（口径必须按本节改）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 995 | `reclaim.c:400-407` | `n.c:8903-9073`；`reclaim.c:400-407, 1985-1991, 926` | "无写屏障 / 无原子 / 无 STW" · 每次引用赋值都发射 storeRef（§6）；运行时含自旋锁与 __ato | `reclaim.c:403-416` | `v7:403 typedef struct ExtcRcReg { ExtcRcSlab **tab; size_t cap; size_t n; } Extc` | high |
| 997 | `reclaim.c:2358-2424` | `ry 旧代永不 free \| `reclaim.c:2358-2424, 390-393, 450-4` | "无共享大堆 / 无碎片" · slab 来自 malloc，regionFree 只 free 不 munmap；re | `reclaim.c:2638-2729` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |
| 1014 | `reclaim.c:1064` | `` 双计真 bug 并修 \| `reclaim.c:1064`、`:1069`；§19.5.` | —（P1 轮：惰性 freelist 与卷计数器） · P0-B 惰性 freelist 默认关（LAZY_FREE=0 | `reclaim.c:1064` | `v7:1064 #define EXTC_RC_LAZY_FREE 0 ; v7:1064 #define EXTC_RC_LAZY_FREE 0` | high |

## 4 GC-ROOFLINE.md 映射表

### §6 改造路线的实施状态（阶段 0 → 3）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 446 | `reclaim.c:1000-1022` | `预算与 mark 配额（宏 `reclaim.c:1000-1022`；实现 `reclaim.c:` | P0-12 回收配额按分配字节计（P） · extc_rc_alloc 的 sweep 预算与 mark 配额（宏 ；实 | `reclaim.c:1029-1059` | `v7:1029 * EXTC_RC_QUOTA_DEADLINE_US adds a physical ceiling to the mark quota lo` | high |
| 446 | `reclaim.c:1365-1390` | `:1000-1022`；实现 `reclaim.c:1365-1390`） \| **已落地**（`SH` | P0-12 回收配额按分配字节计（P） · extc_rc_alloc 的 sweep 预算与 mark 配额（宏 re | `reclaim.c:1475-1508` | `v7:1475 if (r->marking) { ; v7:1476 if (r->msp != 0 \|\| !r->seedsDone \|\| r->scanO` | high |
| 447 | `reclaim.c:250` | `cRcSlab.nfree`（`reclaim.c:250`）、热路径头插 `reclai` | P1-9 惰性 freelist（sweep 不链接、复用时按位图一次性重建；B） · ExtcRcSlab.nfree | `reclaim.c:256` | `v7:256 uint32_t      nfree;     /* EXTC_RC_LAZY_FREE holes: slots whose occupanc` | high |
| 447 | `reclaim.c:1597-1598` | `.c:250`）、热路径头插 `reclaim.c:1597-1598`、重建 `reclaim.c:` | P1-9 惰性 freelist（sweep 不链接、复用时按位图一次性重建；B） · ExtcRcSlab.nfree | `reclaim.c:1727-1733` | `v7:1727 __attribute__((unused)) static void extc_rc_freeSlotLink(ExtcRcSlab *s, ` | high |
| 447 | `reclaim.c:1434-1455` | `:1597-1598`、重建 `reclaim.c:1434-1455`、sweep 只记账 `rec` | P1-9 惰性 freelist（sweep 不链接、复用时按位图一次性重建；B） · ExtcRcSlab.nfree | `reclaim.c:1553-1571` | `v7:1553 /* Every slot was issued and the sweep left holes behind: rebuild the re` | high |
| 447 | `reclaim.c:1661-1664` | `455`、sweep 只记账 `reclaim.c:1661-1664` \| **已落地**（GC 步` | P1-9 惰性 freelist（sweep 不链接、复用时按位图一次性重建；B） · ExtcRcSlab.nfree | `reclaim.c:1798-1802` | `v7:1798 #if EXTC_RC_LAZY_FREE ; v7:1799 /* Lazy reuse (EXTC_RC_LAZY_FREE): count` | high |

### 6.6 P0 落地（P 字节配额 + B 惰性 freelist）：before / after 与代价

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 603 | `reclaim.c:979-1022` | `00` 是单步物理上限（注释 `reclaim.c:979-1022`，实现 `reclaim.c:` | P（按分配字节给回收信用）：sweep 侧 budget = 8·pressure + max(1, min(ceil( | `reclaim.c:1029-1059` | `v7:1029 * EXTC_RC_QUOTA_DEADLINE_US adds a physical ceiling to the mark quota lo` | high |
| 603 | `reclaim.c:1365-1390` | `c:979-1022`，实现 `reclaim.c:1365-1390`，钳位 `:1366`）。默认` | P（按分配字节给回收信用）：sweep 侧 budget = 8·pressure + max(1, min(ceil( | `reclaim.c:1475-1508` | `v7:1475 if (r->marking) { ; v7:1476 if (r->msp != 0 \|\| !r->seedsDone \|\| r->scanO` | high |
| 605 | `reclaim.c:1434-1455` | `的 `bump` 耗尽分支，`reclaim.c:1434-1455`），热路径 `freeSlot` | B（惰性 freelist）：sweep 只清 occ 位并给 nfree 记账，不再逐槽头插；复用该 slab 时按位 | `reclaim.c:1553-1571` | `v7:1553 /* Every slot was issued and the sweep left holes behind: rebuild the re` | high |

### 6.7.2 项 2：配额记账不是成本（假设被推翻）；B 默认关；elide 改 O(1)

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 643 | `reclaim.c:1601` | `计数器（`alloc` +1 `reclaim.c:1601`、`freeSlot` −1` | - elide 改 O(1)：slab 增加 nLive/nMark 卷计数器（alloc +1 、freeSlot − | `reclaim.c:1601` | `v7:1601 s->nLive++; ; v7:1601 s->nLive++;` | high |

### 6.7.3 项 3：regionFree 两阶段（pd 达标，pb 未闭合）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 648 | `reclaim.c:1399` | `c_fqPushChain` `reclaim.c:1399`）；排空点在 `slabNew` | 机制：同步段只做 abort + pinDropRegion + 逐 slab（rsIncN 时 unlinkChild | `reclaim.c:1399` | `v7:1399 __attribute__((unused)) static int64_t extc_rc_fqPushChain(ExtcRcSlab *h` | high |

### 6.7.4 项 4：G1② 量具重建（通道拆分 + B_seg + max 降级为 OBS）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 662 | `reclaim.c:1100` | `[segsteps]`（声明 `reclaim.c:1100`、记录 `:2941-2943` | - 新增 [segsteps]（声明 、记录 :2941-2943、打印 :3445-3447）：只统计 seg0（分段 | `reclaim.c:1147` | `v7:1147 static long long extc_rc_segN, extc_rc_segPops, extc_rc_segMax, extc_rc_` | high |

### 8.2 实现侧（已修 / 未修）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 745 | `reclaim.c:1365-1390` | `MB） \| **已修**（P：`reclaim.c:1365-1390`；G18 PASS 26,26` | L13 · 回收配额按事件数而非字节数给：分配事件少的大对象程序让标记/清扫永远跟不上，内存按累计分配量增长（pd_bi | `reclaim.c:1475-1508` | `v7:1475 if (r->marking) { ; v7:1476 if (r->msp != 0 \|\| !r->seedsDone \|\| r->scanO` | high |

### 附录 D 源码行号索引（FREEZE v5 口径）

| 文档行 | 原文引用 | 唯一片段 | 语义指向 | v7 行号 | 依据（v7 源码摘录）| 置信度 |
|---|---|---|---|---|---|---|
| 950 | `reclaim.c:1289` | `行期：alloc 快路径 \| `reclaim.c:1289` \|` | 运行期：alloc 快路径 | `reclaim.c:1455` | `v7:1455 __attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(Ex` | high |
| 951 | `reclaim.c:782` | `运行期：slab 新建 \| `reclaim.c:782` \|` | 运行期：slab 新建 | `reclaim.c:820` | `v7:820 __attribute__((unused)) static ExtcRcSlab *extc_rc_slabNew(ExtcRcRegion *` | high |
| 952 | `reclaim.c:875` | `内 1316-1341） \| `reclaim.c:875` \|` | 运行期：大小类链查找（classSlot；复用/发号在 alloc 内 1316-1341） | `reclaim.c:917` | `v7:917 __attribute__((unused)) static ExtcRcClass *extc_rc_classSlot(const ExtcR` | high |
| 954 | `reclaim.c:2358` | `行期：region 释放 \| `reclaim.c:2358` \|` | 运行期：region 释放 | `reclaim.c:2638` | `v7:2638 __attribute__((unused)) static void extc_rc_regionFree(ExtcRcRegion *r) ` | high |
| 955 | `reclaim.c:3178` | `运行期：阈值更新与夹取 \| `reclaim.c:3178` \|\|` | 运行期：阈值更新与夹取 | `reclaim.c:3514` | `v7:3514 __attribute__((unused)) static void extc_rc_thrUpd(ExtcRcRegion *r) { ; ` | high |
| 956 | `reclaim.c:3239` | `\| 运行期：触发判据 \| `reclaim.c:3239` \|` | 运行期：触发判据 | `reclaim.c:3575` | `v7:3575 __attribute__((unused)) static inline int extc_rc_shouldSweep(const Extc` | high |
| 957 | `reclaim.c:3453` | `\| 运行期：分配点推进 \| `reclaim.c:3453` \|` | 运行期：分配点推进 | `reclaim.c:3817-3830` | `v7:3817 __attribute__((unused)) static int extc_rc_maybeCollect(ExtcRcRegion *r,` | medium |
| 958 | `reclaim.c:2315` | `\| 运行期：预算信用 \| `reclaim.c:2315` \|` | 运行期：预算信用 | `reclaim.c:2595` | `v7:2595 __attribute__((unused)) static int extc_rc_budgetOk(const ExtcRcRegion *` | high |
| 959 | `reclaim.c:2503` | `\| 运行期：标记开始 \| `reclaim.c:2503` \|` | 运行期：标记开始 | `reclaim.c:2808` | `v7:2808 __attribute__((unused)) static void extc_rc_markBegin(ExtcRcRegion *r, v` | high |
| 960 | `reclaim.c:1250` | `：标记推进（一步一对象） \| `reclaim.c:1250` \|` | 运行期：标记推进（一步一对象） | `reclaim.c:1350` | `v7:1350 __attribute__((unused)) static inline __attribute__((always_inline)) siz` | high |
| 961 | `reclaim.c:2673` | `期：标记批量（一段扫描） \| `reclaim.c:2673` \|` | 运行期：标记批量（一段扫描） | `reclaim.c:2985` | `v7:2985 __attribute__((unused)) static size_t extc_rc_markStep(ExtcRcRegion *r, ` | high |
| 962 | `reclaim.c:2655` | `\| 运行期：大对象续扫 \| `reclaim.c:2655` \|` | 运行期：大对象续扫 | `reclaim.c:2967` | `v7:2967 __attribute__((unused)) static size_t extc_rc_markResume(ExtcRcRegion *r` | high |
| 963 | `reclaim.c:3066` | `\| 运行期：清扫开始 \| `reclaim.c:3066` \|` | 运行期：清扫开始 | `reclaim.c:3395` | `v7:3395 __attribute__((unused)) static void extc_rc_sweepBegin(ExtcRcRegion *r) ` | high |
| 964 | `reclaim.c:3090` | `\| 运行期：清扫一步 \| `reclaim.c:3090` \|\|` | 运行期：清扫一步 | `reclaim.c:3425` | `v7:3425 __attribute__((unused)) static int extc_rc_sweepStep(ExtcRcRegion *r, ui` | high |
| 965 | `reclaim.c:3155` | `\| 运行期：清扫结算 \| `reclaim.c:3155` \|` | 运行期：清扫结算 | `reclaim.c:3490` | `v7:3490 __attribute__((unused)) static void extc_rc_settle(ExtcRcRegion *r) { ; ` | high |
| 966 | `reclaim.c:2883` | `\| 运行期：定向释放 \| `reclaim.c:2883` \|` | 运行期：定向释放 | `reclaim.c:3200-3271` | `v7:3200 __attribute__((unused)) static size_t extc_rc_releaseOld(ExtcRcRegion *r` | high |
| 967 | `reclaim.c:1480` | `运行期：freeSlot \| `reclaim.c:1480` \|` | 运行期：freeSlot | `reclaim.c:1734` | `v7:1734 __attribute__((unused)) static void extc_rc_freeSlot(ExtcRcSlab *s, size` | high |
| 968 | `reclaim.c:1600` | `期：级联释放（P0-5） \| `reclaim.c:1600` \|` | 运行期：级联释放（P0-5） | `reclaim.c:1878` | `v7:1878 __attribute__((unused)) static void extc_rc_freeSlotCascade(ExtcRcSlab *` | high |
| 969 | `reclaim.c:1586` | `运行期：级联栈 push \| `reclaim.c:1586` \|` | 运行期：级联栈 push | `reclaim.c:1864` | `v7:1864 __attribute__((unused)) static int extc_rc_cascadePush(ExtcRcRegion *r, ` | high |
| 970 | `reclaim.c:1668` | `COUNT=1 的空桩） \| `reclaim.c:1668` \|\|` | 运行期：计数更新（真实计数体；1657-1659 是 NOCOUNT=1 的空桩） | `reclaim.c:1960` | `v7:1960 __attribute__((unused)) static void extc_rc_holdUpd(void *p, int delta) ` | high |
| 971 | `reclaim.c:2025` | `\| 运行期：pin 加入 \| `reclaim.c:2025` \|` | 运行期：pin 加入 | `reclaim.c:2305` | `v7:2305 __attribute__((unused)) static int extc_rc_pinAdd(void *v) { ; v7:2305 _` | high |
| 972 | `reclaim.c:2079` | `\| 运行期：pin 种子 \| `reclaim.c:2079` \|` | 运行期：pin 种子 | `reclaim.c:2359` | `v7:2359 static __attribute__((unused)) __attribute__((noinline)) void extc_rc_pi` | high |
| 973 | `reclaim.c:926` | `存墙 arm（P0-4） \| `reclaim.c:926` \|` | 运行期：内存墙 arm（P0-4） | `reclaim.c:969` | `v7:969 __attribute__((unused)) static void extc_rc_mmaxArm(void) { ; v7:969 __at` | high |
| 974 | `reclaim.c:3207` | `\| 运行期：墙不可达警告 \| `reclaim.c:3207` \|` | 运行期：墙不可达警告 | `reclaim.c:3539-3543` | `v7:3539 if (!extc_rc_wallWarned) {   /* report once, and only when the wall was ` | high |
| 975 | `reclaim.c:2350` | `ionFree 上界注释 \| `reclaim.c:2350` \|` | 运行期：regionFree 上界注释 | `reclaim.c:2630` | `v7:2630 * So one call issues at most #free <= 10 * #slabs + #classes + 3; it is ` | high |
| 976 | `reclaim.c:3323` | `egionFree 探针 \| `reclaim.c:3323` \|` | 运行期：regionFree 探针 | `reclaim.c:3662` | `v7:3662 fprintf(stderr, "[regionfree] n=%lld max_frees=%lld max_slabs=%lld sync_` | high |
| 977 | `reclaim.c:3342` | `k 行（M_max 等） \| `reclaim.c:3342` \|` | 运行期：work 行（M_max 等） | `reclaim.c:3689` | `v7:3689 fprintf(stderr, "[work] n=%lld walk_max=%lld scan_max=%lld push_max=%lld` | high |
| 978 | `reclaim.c:980` | `\| 运行期：释放预算常量 \| `reclaim.c:980` \|` | 运行期：释放预算常量 | `reclaim.c:1079` | `v7:1079 #define EXTC_RC_RELEASE_BUDGET 2048 ; v7:1079 #define EXTC_RC_RELEASE_BU` | high |
| 979 | `reclaim.c:2563` | `\| 运行期：分段段长常量 \| `reclaim.c:2563` \|` | 运行期：分段段长常量 | `reclaim.c:2875` | `v7:2875 #define EXTC_RC_SCAN_CHUNK 8192 ; v7:2875 #define EXTC_RC_SCAN_CHUNK 819` | high |
| 980 | `reclaim.c:990` | `\| 运行期：压力乘子常量 \| `reclaim.c:990` \|` | 运行期：压力乘子常量 | `reclaim.c:1089` | `v7:1089 #define EXTC_RC_SWEEP_PRESSURE_MUL 4 ; v7:1089 #define EXTC_RC_SWEEP_PRE` | high |
| 981 | `reclaim.c:1573` | `\| 运行期：级联调试宏 \| `reclaim.c:1573` \|` | 运行期：级联调试宏 | `reclaim.c:1851` | `v7:1851 #define EXTC_RC_CASCADE_DEBUG 0 ; v7:1851 #define EXTC_RC_CASCADE_DEBUG ` | high |

## 5 最该先修的 10 条（漂移最大）

| # | 文档:行 | 原文引用 | 语义 | v7 正确行号 | 漂移 |
|---:|---|---|---|---|---:|
| 1 | MEMORY:384 | `reclaim.c:3432` | 触发点 extc_rc_step · （由 maybeCollect 在轮在飞时调） | `reclaim.c:3796-3803` | +364 |
| 2 | MEMORY:606 | `reclaim.c:3593` | 2. 发射器 写出 define EXTC_RC_NOCOUNT 1（=0 时写 0）。 | `reclaim.c:3957` | +364 |
| 3 | MEMORY:720 | `reclaim.c:3441` | "能全量回收"的接口存在但不是生产路径：extc_rc_collect（，标记到底再 settle）、extc_rc_s | `reclaim.c:3805-3814` | +364 |
| 4 | GC:957 | `reclaim.c:3453` | 运行期：分配点推进 | `reclaim.c:3817-3830` | +364 |
| 5 | MEMORY:436 | `reclaim.c:2562-2566` | - 大对象/保守对象：words EXTC_RC_SCAN_CHUNK = 8192（64 KiB/段）时不进标记栈，只 | `reclaim.c:2925-2927` | +363 |
| 6 | MEMORY:716 | `reclaim.c:3393-3395` | EXTC_RC_SLOWSTEP=<us> · 只设阈值，超阈值在 2820/3122 打印 [slow] | `reclaim.c:3740-3741` | +347 |
| 7 | GC:977 | `reclaim.c:3342` | 运行期：work 行（M_max 等） | `reclaim.c:3689` | +347 |
| 8 | MEMORY:534 | `reclaim.c:2863-2882` | 安全性论证（ 逐字要点）：释放集是"同一时刻、同一 seed 集下 collect 会释放的集合"的子集（轻量级 tri | `reclaim.c:3206-3209` | +343 |
| 9 | MEMORY:883 | `reclaim.c:3199-3207` | - 墙不可达警告：若 M_max 小到连 floor 都放不下（max(2L,floor) 压不到墙下），运行期打一次  | `reclaim.c:3539-3543` | +340 |
| 10 | MEMORY:918 | `reclaim.c:3323` | regionFree · 不分批，但单次调用有显式上界：每次 drop ≤ 10·#slab + #classes +  | `reclaim.c:3662` | +339 |

## 6 已确认无需改的引用（delta = 0）

这些引用当前指向的 v7 行号在语义上已正确，**不要**再按旧偏移量改一次：

`reclaim.c:138`→`138`, `reclaim.c:211`→`211`, `reclaim.c:123-126`→`123-126`, `reclaim.c:156-160`→`156-160`, `reclaim.c:1064`→`1064`, `reclaim.c:1069`→`1069`, `reclaim.c:799`→`799`, `reclaim.c:802`→`802`, `reclaim.c:805`→`805`, `reclaim.c:808`→`808`, `reclaim.c:811`→`811`, `reclaim.c:144`→`144`, `reclaim.c:1064`→`1064`, `reclaim.c:1601`→`1601`, `reclaim.c:1399`→`1399`

其中 §17.1 的 FREE_DEFER=799、FREE_BUDGET_SLABS=802、FREE_DEADLINE_US=805、FREE_QUEUE_BYTES=808、FREE_HARD_DEADLINE_US=811、LAZY_FREE=1064、ELIDE_VERIFY=1069，以及 GC 文档的 fqPushChain=1399、alloc nLive++=1601 已是 v7 行号。

## 7 复现与校验

产物：

- 本文件 docs/analysis/REFS-V7.md；
- 补丁清单 /tmp/refs_v7_patch.tsv：三列 唯一片段<TAB>旧引用<TAB>新引用，不存文档行号（Lead 同时改文档时会漂移，片段仍可定位）；
- 校验脚本 /tmp/refs_v7_check.py：逐条断言片段在两份文档中**恰好出现 1 次**、片段包含旧引用、新引用是合法 v7 行范围。

```
python3 /tmp/refs_v7_check.py
# rows=196 lines=180 miss=0
```

miss=0 才可机械应用；MISS-NOT-UNIQUE 表示文档已被改到片段不再唯一，按 原文引用 重新定位。

## 8 未验证项

- GC:3453（运行期：分配点推进）标 medium：v6 的 3453 在 v7 没有唯一同名锚点；按上下文判为 extc_rc_maybeCollect（v7:3817-3830），但文档若本意是 extc_rc_step（v7:3796）需人工确认。
- 文档在并行编辑中（本表快照 md5 见 §0）；`文档行` 列可能已过期，`唯一片段` 列为定位主键。
- 本表只覆盖 reclaim.c: 前缀引用；文档里裸 :NNN（如 :1366、:3358-3368）未单列，随所属句子的主引用一起改。
- GC 文档 §6.7 由 Lead 手插的部分已是 v7，本表按当前文本逐条复核后标 delta=0，未改。
