# REFS-V33：容量归还收敛成一个水位（v32 → v33 行号换算与锚点）

本轮（S1）只动运行期的**容量归还**一节：删掉 `slabShrink` / `pageTrim` / `EXTC_RC_SHRINK_GRACE` / `EXTC_RC_SHRINK_CAP_LIVE` 四个旋钮，换成一个水位（`extc_rc_surplus`）+ 一个 pass（`extc_rc_returnStep`）。物理上 `src/back/reclaim.c` 仍是 241 行（E1 起的数组头 + 24 条 `#include "rt/*.inc"`），**行号变化全部发生在渲染文本里**，所以本文件给的是渲染行（`tools/source_split.py` 的输出）的换算表。

## 0 口径

| 项 | v32（base = HEAD `90c876f1`，档一） | v33（本轮工作树） |
|---|---|---|
| `src/back/reclaim.c`（物理） | 241 行 / `75ecc57760aa8c8d92bef3f4bd452529` | 241 行 / **`a8706acf8b9b144399e46fa2c73ad000`** |
| 渲染文本 | **4,805 行** / `93a53fde452c61b245db80fbfc3d1ba0` | **4,780 行** / **`2ad2ae580109578b77d6f096214b19f1`** |
| `src/back/codegen.c`（物理） | 75 行 / `6ba135e22e86ec89d5cd8aa416a994bc` | 同左（未改） |
| codegen 渲染文本 | 16,429 行 / `aaaaf36f73ff859044acb222934ce9c8` | 同左（未改） |
| `tests/reclaim/driver.c` | 1,637 行 / `28dd4009ac7207f47e581a697c0d421f` | 同左（未改） |
| `build/extc` | `/tmp/s1b/wt/build-b/extc` = `d44f092dc8495e48ed6ee8e9ebf63559`（不同 BUILDDIR 的 md5 不同，溯源只认源码） | `b5a05be1ce5780d34a8f50b4e322322a` |

**为什么净 −25 行却是大改**：删掉的 `52-slab-release.inc`（86 行）+ `53-page-release.inc`（268 行）= 354 行；新增 `52-capacity-release.inc`（190 行）+ `53-slot-release.inc`（150 行）= 340 行；再减去 `00-head.inc` −3（删 `EXTC_RC_PAGE_TRIM` 提前定义）、`41-budget.inc` −3（删分配热路径的 `s->allocN++`）、`33-hugefree.inc` −1（删 `deadA`/`deadN` 初始化）、`21-wmark.inc` −8（删四个字段、改两条注释）、`62-sweep.inc` +8（删两处旧调用、加 `extc_rc_returnStep` 调用与 `[return]` 打印）= **−25 行**。

## 1 改动清单（11 个文件）

| 文件 | 变化 | 内容 |
|---|---|---|
| `src/back/reclaim.c` | 4 行（2 删 2 增） | include 列表：`rt/52-slab-release.inc` + `rt/53-page-release.inc` → `rt/52-capacity-release.inc` + `rt/53-slot-release.inc` |
| `src/back/rt/52-slab-release.inc` | **删除**（86 行） | `EXTC_RC_SLAB_KEEP*` / `SHRINK_*` 宏、`extc_rc_slabShrink` 全体 |
| `src/back/rt/53-page-release.inc` | **删除**（268 行） | `PAGE_TRIM_*` 宏、`pageTrimSlab`、`pageTrimStep`、`cqDrainCore` / `freeSlot*`（后三者在 v33 移到 `53-slot-release.inc`） |
| `src/back/rt/52-capacity-release.inc` | **新增**（190 行） | 五个 `RETURN_*` 宏 + `CAP_MULT` + `MADVISE_OK`、`extc_rc_surplus`、`extc_rc_pageWorth`、`extc_rc_pageTrimSlab`（从旧 53 搬入）、`extc_rc_slabUnlink`、`extc_rc_returnStep` |
| `src/back/rt/53-slot-release.inc` | **新增**（150 行） | 旧 53 的行 119 起（`cqDrainCore` / `freeSlotLink` / `freeSlot` / `freeSlotWord` 等）原样搬入 |
| `src/back/rt/00-head.inc` | −3 | 删 `#ifndef EXTC_RC_PAGE_TRIM` / `#define` / `#endif` 三行（分配路径不再需要它） |
| `src/back/rt/21-wmark.inc` | −8 +2 | `ExtcRcSlab` 删 `deadA`、`deadN`、`allocN`、`pgAllocN`；`pgLive` 注释改「last finished pass」，`trimCur` 注释改「capacity pass cursor」 |
| `src/back/rt/33-hugefree.inc` | −1 | `slabNew` 删 `s->deadA = 0; s->deadN = 0;` |
| `src/back/rt/41-budget.inc` | −3 | 分配热路径删 `#if EXTC_RC_PAGE_TRIM` / `s->allocN++;` / `#endif` |
| `src/back/rt/62-sweep.inc` | +8 / −? | 收尾分支 `extc_rc_slabShrink(r)` + `extc_rc_pageTrimStep(...)` → `extc_rc_returnStep(r)`；`[pages]` 后新增 `[return]` 行 |

## 2 分段换算表（v32 渲染行 → v33 渲染行）

按「等于段」查表；段外的行是被替换/删除的块，见 §3。**禁止用单一 offset 平移**（历史上机械平移曾把 `thrUpd` 指到 `markResume`）。

| v32 行 | v33 行 | 位移 |
|---|---|---|
| 1–91 | 1–91 | 0 |
| 95–285 | 92–282 | −3 |
| 287 | 284 | −3 |
| 290 | 285 | −5 |
| 293–333 | 286–326 | −7 |
| 335–1065 | 328–1058 | −7 |
| 1067–1899 | 1059–1891 | −8 |
| 1903–2079 | 1892–2068 | −11 |
| 2093 | 2095 | +2 |
| 2098 | 2098 | 0 |
| 2103 | 2101 | −2 |
| 2107 | 2104 | −3 |
| 2110 | 2107 | −3 |
| 2198–2199 | 2112–2113 | −86 |
| 2201 | 2115 | −86 |
| 2203–2207 | 2117–2121 | −86 |
| 2209 | 2123 | −86 |
| 2211 | 2125 | −86 |
| 2213 | 2127 | −86 |
| 2218–2230 | 2149–2161 | −69 |
| 2232–2235 | 2163–2166 | −69 |
| 2239–2255 | 2170–2186 | −69 |
| 2257–2262 | 2188–2193 | −69 |
| 2273 | 2231 | −42 |
| 2281 | 2252 | −29 |
| 2282 | 2254 | −28 |
| 2283–4212 | 2258–4187 | −25 |
| 4217–4449 | 4191–4423 | −26 |
| 4450–4805 | 4425–4780 | −25 |

对照：物理文件行号（1–241）**完全不变**，因为 `reclaim.c` 只是 include 列表；`0-head` / `21-wmark` / `31-slab` / `41-budget` / `62-sweep` 等分节文件的**节内**行号只在 §1 列出的那几处变化。

## 3 被替换/删除的块（按 v32 行号）

| v32 行 | v33 行 | 性质 |
|---|---|---|
| 92–94 | 92 | 删 `#ifndef EXTC_RC_PAGE_TRIM` 三行（`00-head.inc`） |
| 286 / 288–289 / 291–292 | 283 / 285 / 286 | `pgLive` 注释改写 + 删 `deadA`/`deadN`/`allocN`/`pgAllocN`（`21-wmark.inc`） |
| 334 | 327 | `trimCur` 注释改「capacity pass cursor」 |
| 1066 | 1059 | 删 `s->deadA = 0; s->deadN = 0;`（`slabNew`，`33-hugefree.inc`） |
| 1900–1902 | 1892 | 删分配热路径 `#if EXTC_RC_PAGE_TRIM` / `s->nLive++` 前的 `allocN++` 三行（`41-budget.inc`） |
| 2080–2197 | 2069–2113 | **整节替换**：`Dead-slab release` → `Capacity release: one watermark, one bounded pass`；`SLAB_KEEP*` / `SHRINK_*` 宏与 `slabShrink` 换成 `CAP_MULT` / `RETURN_*` 宏与 `surplus` |
| 2200 / 2202 / 2208 / 2210 / 2212 / 2214–2217 | 2114 / 2116 / 2122 / 2124 / 2126 / 2128–2148 | `#error` 文案、`MADVISE_OK` 平台块、`pageTrimSlab` 前后（多出 `surplus`/`pageWorth` 与新计数器） |
| 2231 / 2236–2238 / 2256 | 2162 / 2167–2169 / 2187 | `pageTrimSlab` 内部注释与 `pgLive` 收尾（不再写 `pgAllocN`） |
| 2263–2272 | 2194–2230 | **整块替换**：`pageTrimStep`（round-robin + DENSE/ACTIVE 猜测）换成 `slabUnlink` + `returnStep` 的头 |
| 2274–2280 | 2232–2251 | `returnStep` 的循环体（死块摘链、页释放条件） |
| 2281–2283 | 2252–2257 | 循环尾：`s = n;`、`r->trimCur = s;`、水位复验 |
| 4213–4216 | 4188–4190 | 收尾注释 + 调用点改 `extc_rc_returnStep(r)`（`62-sweep.inc`） |
| 4450 | 4424 | 新增 `[return] steps=… rel_slabs=… page_mb=… page_blocks=… cap_mb=… live_mb=…`（`62-sweep.inc`） |

## 4 v33 绝对锚点（渲染行，`tools/source_split.py` 口径）

| 主题 | 位置（v33 渲染行） |
|---|---|
| 机制说明注释块（为什么一个水位、为什么一代余量、为什么存量） | 2070–2091 |
| `EXTC_RC_CAP_MULT`（默认 2） | 2092–2095 |
| `EXTC_RC_RETURN_BYTES`（256 KiB） | 2096–2098 |
| `EXTC_RC_RETURN_VISIT_MAX`（256 块） | 2099–2101 |
| `EXTC_RC_RETURN_SLAB_MAX`（16 块） | 2102–2104 |
| `EXTC_RC_RETURN_PAGE_MAX`（8 块） | 2105–2107 |
| `EXTC_RC_RETURN_FLOOR`（16 MiB 复用存量） | 2108–2112 |
| `#error`（块大于 512 KiB 需要更宽的活页摘要） | 2113–2115 |
| `MADVISE_OK` 平台块（`madvise` 声明 + `EXTC_RC_MADVISE_OK`） | 2116–2127 |
| `extc_rc_slabRel` / `extc_rc_retN` / `extc_rc_capMax` / `extc_rc_capLiveMax` 计数器 | 2128–2135 |
| `extc_rc_surplus`（水位判据，`skip:return-watermark`） | 2136–2139 |
| `extc_rc_pageWorth`（算术充分条件） | 2142–2147 |
| `extc_rc_pageTrimSlab`（从旧 53 搬入，去掉 `pgAllocN` 写入） | 2149–2193 |
| `extc_rc_slabUnlink`（摘链 + `capBytes -=` + `ownDel` + `fqPushOne`） | 2196–2214 |
| `extc_rc_returnStep`（`skip:return-null` 2218 / `quiet` 2221 / `empty` 2223；循环 2229–2254） | 2216–2258 |
| 调用点（`extc_rc_sweepStep` 收尾：`returnStep` + `fqDrainAuto`） | 4188–4190 |
| `[return]` 探针 | 4424 |

## 5 使用规则

1. **换算按 §2 查表，禁止单一 offset 平移。** 本轮位移从 0 跳到 −86 再回到 −25，任何常数都用不了。
2. **生成 C 变了**：运行期发射文本变了 ⇒ `tools/emitted_identity.py --check` 会报 changed（core 集 272 项），本轮已重冻结基线；重冻结只允许在「生成 C 的变化就是本轮预期的那条」时做，并写进 changelog 的判据段。
3. **物理行号不变不等于内容不变**：`src/back/reclaim.c` 仍是 241 行，但 `#include` 的两行换了；`grep reclaim.c:NNN` 类工具一律改读渲染文本（`tools/cscan.py` / `tools/source_split.py` 已就位）。
4. **旧文档里的 v31 行号全部作废**：`REFS-V31.md` 仍然有效（它描述 v30→v31），但任何「v31 → 当前」的换算必须走本表。
