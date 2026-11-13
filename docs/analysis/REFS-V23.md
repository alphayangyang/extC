# REFS-V23：v22 → v23 的行号映射（页级归还）

本文件只做一件事：把 `docs/analysis/MEMORY-MODEL-CURRENT.md` 与 `docs/analysis/GC-ROOFLINE.md` 里按 v22 绝对行号写的锚点，换算到当前工作树（v23）的行号。**不许机械平移到别的版本**：映射按插入块分段，跨块平移会指错行。

## 1. 冻结快照（v23）

| 文件 | v22（提交 1420d5a3） | v23（当前工作树） | 变化 |
| --- | --- | --- | --- |
| `src/back/reclaim.c` | `8ed810ae8bc98221aad101a86436f4ff`（4,367 行） | `71daa809f13f885489e14918b5e56a7e`（**4,507 行**） | +140 行，11 个纯插入块 |
| `src/back/codegen.c` | `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行） | 同左（未改） | — |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f`（1,637 行） | 同左（未改） | — |
| `build/extc` | `e76860e5510069b482f9f2cf28e72053` | `10334a5fac66189da3914daaf0112afb` | 同源码、不同构建目录的 md5 不同，溯源只认源码 md5 |

## 2. 11 个插入块（v22 空位 → v23 插入行）

diff（difflib.SequenceMatcher，autojunk=False）给出 11 个 `insert` 块，**没有一处删除或替换** —— 所以 v23 是 v22 的超集，任何 v22 行都能唯一映射。

| # | v22 空位（在第几行之后） | v23 插入行 | 插入行数 | 内容 |
| --- | --- | --- | --- | --- |
| 1 | 91 | 92–94 | 3 | `#ifndef EXTC_RC_PAGE_TRIM / #define EXTC_RC_PAGE_TRIM 1`（必须在使用点之前；否则 alloc/slabNew 处的 `#if` 在定义前按 0 处理） |
| 2 | 261 | 265–268 | 4 | `ExtcRcSlab` 的 `pgLive`/`pgNext`/`allocN`/`pgAllocN` |
| 3 | 298 | 306–307 | 2 | `ExtcRcRegion` 的 `capBytes`/`trimCur` |
| 4 | 876 | 886–888 | 3 | `slabNew`：`r->capBytes += nslots * slotSize` |
| 5 | 913 | 926 | 1 | `slabNew`：`s->pgNext = -1` |
| 6 | 1721 | 1735–1737 | 3 | `extc_rc_alloc`：`s->allocN++`（热路径唯一新增指令） |
| 7 | 1896 | 1913 | 1 | `slabShrink`：`if (r->trimCur == s) r->trimCur = s->next`（防悬空游标） |
| 8 | 1910 | 1928–1930 | 3 | `slabShrink`：`r->capBytes -= nslots * ss` |
| 9 | 1917 | 1938–2055 | 118 | 页级归还整块：注释、宏、`MADV_DONTNEED`/`madvise`、`extc_rc_pageRel`/`pageTrimN`、`extc_rc_pageTrimSlab`、`extc_rc_pageTrimStep` |
| 10 | 3778 | 3917 | 1 | `extc_rc_sweepStep` 收尾：`extc_rc_pageTrimStep(r, EXTC_RC_PAGE_TRIM_BYTES)` |
| 11 | 4011 | 4151 | 1 | `[pages] rel_bytes=%lld blocks=%lld rel_mb=%.2f` 行（在 `[work]` 之前） |

## 3. 分段映射（v22 行 n → v23 行 n + offset）

| v22 行区间 | offset | v23 行区间 |
| --- | --- | --- |
| 1–91 | 0 | 1–91 |
| 92–261 | +3 | 95–264 |
| 262–298 | +7 | 269–305 |
| 299–876 | +9 | 308–885 |
| 877–913 | +12 | 889–925 |
| 914–1721 | +13 | 927–1734 |
| 1722–1896 | +16 | 1738–1912 |
| 1897–1910 | +17 | 1914–1927 |
| 1911–1917 | +20 | 1931–1937 |
| 1918–3778 | +138 | 2056–3916 |
| 3779–4011 | +139 | 3918–4150 |
| 4012–4367 | +140 | 4152–4507 |

## 4. v23 绝对锚点（新内容一律用这一列）

| 符号 | v23 行 | 说明 |
| --- | --- | --- |
| `EXTC_RC_PAGE_TRIM`（顶部声明） | 92–93 | 让使用点之前就可见 |
| `ExtcRcSlab.pgLive / pgNext / allocN / pgAllocN` | 265–268 | pass 幂等、续扫、quiet 闸门 |
| `ExtcRcRegion.capBytes / trimCur` | 306–307 | slack 闸门与轮转游标 |
| `slabNew` 记容量 | 886–888 | `capBytes += nslots*slotSize` |
| `extc_rc_alloc` 记分配 | 1735–1737 | `allocN++`（唯一热路径新增） |
| `extc_rc_slabShrink` | 1882（v22 同号） | 块级归还（§6.10） |
| 页级归还注释块 | 1938–1950 | 机制、下界、为什么只有稀疏块 |
| `EXTC_RC_PAGE_TRIM_BYTES` | 1956 | 256 KiB/次 |
| `EXTC_RC_PAGE_TRIM_VISIT_MAX` | 1959 | 16 块/次 |
| `EXTC_RC_PAGE_TRIM_MIN_SLACK` | 1962 | 8 MiB（slack 闸门） |
| `EXTC_RC_PAGE_TRIM_DENSE` | 1966 | 8（稠密闸门） |
| `EXTC_RC_PAGE_TRIM_ACTIVE` | 1968 | 64（quiet 闸门） |
| `MADV_DONTNEED` / `extern int madvise` | 1977 / 1981 | 不 include `<sys/mman.h>` 的原因写在注释里 |
| `extc_rc_pageRel / pageTrimN` | 1987–1988 | 计数器 |
| `extc_rc_pageTrimSlab` | 1991 | 单块 pass（位图 → 活页 → 空页 run → madvise） |
| `extc_rc_pageTrimStep` | 2037 | 区域转轮 + 三层闸门 |
| 调用点 | 3917 | 在 `shrinkDue = 1` 之后、pause 计时块之内 |
| `[pages]` 打印 | 4151 | 在 `[work]` 之前 |

## 5. 使用规则

1. 老文档里 1,600 多处 v17/v18 行号**不逐条重指**（见 MEMORY-MODEL 头部口径）；只有本轮新增内容用 v23 绝对行号，并标 `(v23)`。
2. 任何一次换源码版本，都先跑 `difflib` 出插入块（本节第 2 张表），再按第 3 张表平移；**不要**用单一 offset 平移全文 —— v22→v23 的 offset 在 0 到 +140 之间跳了 10 次。
3. 行号只用于源码溯源；判据与数字以 `docs/analysis/` 的实测表为准。
