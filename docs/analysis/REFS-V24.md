# REFS-V24：v23 → v24 的行号映射（块级归还接到 mutator 路径）

本文件只做一件事：把 `docs/analysis/MEMORY-MODEL-CURRENT.md` 与 `docs/analysis/GC-ROOFLINE.md` 里按 v23 绝对行号写的锚点，换算到当前工作树（v24）的行号。**不许机械平移到别的版本**：v24 是「11 处删除 + 3 处插入」，净 −1 行，offset 在 0 到 −3 之间跳，单一 offset 平移会指错行。

## 1. 冻结快照（v24）

| 文件 | v23（提交 8f42f61d） | v24（当前工作树） | 变化 |
| --- | --- | --- | --- |
| `src/back/reclaim.c` | `71daa809f13f885489e14918b5e56a7e`（4,507 行） | `711d2b02ade155be40b52508cf784397`（**4,506 行**） | −1 行，11 个 hunk |
| `src/back/codegen.c` | `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行） | 同左（未改） | — |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f`（1,637 行） | 同左（未改） | — |
| `build/extc` | `10334a5fac66189da3914daaf0112afb` | `effe8fe2f4330ea878347d4c6c54b2a1` | 同源码、不同构建目录的 md5 不同，溯源只认源码 md5 |

## 2. 11 个 hunk（v23 → v24）

`git diff -U0` 给出 11 个 hunk：9 处纯删除、2 处替换、1 处纯插入；**没有任何块是纯插入**（除 hunk 6 的一行）。

| # | v23 行 | v24 行 | 类型 | 内容 |
| --- | --- | --- | --- | --- |
| 1 | 305 | — | 删 1 | `ExtcRcRegion.shrinkDue` 字段声明；语义改由「轮末就是归还点」承担，标志位不再需要 |
| 2 | 886–888 | 885 | 3→1 | `slabNew` 的 `capBytes += nslots*slotSize` 去掉 `#if EXTC_RC_PAGE_TRIM` 包裹（`PAGE_TRIM=0` 时也要记，否则 slack 闸门永远拦住 slab 归还） |
| 3 | 1884 | 1881 | 1→1 | `slabShrink` 头注释：`per settle point` → `per closing point (the tail of extc_rc_sweepStep)` |
| 4 | 1892 | 1889 | 1→1 | `EXTC_RC_SHRINK_VISIT_MAX` 注释同上改名 |
| 5 | 1895 | 1892–1895 | 1→4 | `REL_MAX` 注释改名 + 新增 `#ifndef EXTC_RC_SHRINK_MIN_SLACK` / `#define … (8 << 20)` |
| 6 | — | 1900 | 插 1 | `extc_rc_slabShrink` 体首：`if (r->capBytes - (int64_t)r->bytes < (int64_t)EXTC_RC_SHRINK_MIN_SLACK) return 0;   /* skip:shrink-slack */` |
| 7 | 1928 | — | 删 1 | `slabShrink` 内 `capBytes -= …` 的 `#if EXTC_RC_PAGE_TRIM` |
| 8 | 1930 | — | 删 1 | 同上的 `#endif` |
| 9 | 3826 | — | 删 1 | `sweepBegin` 的 `r->shrinkDue = 0;` |
| 10 | 3916 | 3914–3916 | 1→3 | `sweepStep` 收尾：`r->shrinkDue = 1;` 换成注释 + `extc_rc_slabShrink(r);` + `extc_rc_fqDrainAuto();` |
| 11 | 3933 | — | 删 1 | `settle` 尾部的 `if (r->shrinkDue) { … }` 整行 |

## 3. 分段映射（v23 行 n → v24 行）

| v23 行区间 | offset | v24 行区间 |
| --- | --- | --- |
| 1–304 | 0 | 1–304 |
| 305 | — | 已删 |
| 306–885 | −1 | 305–884 |
| 886–888 | → | 885（三行并一行） |
| 889–1883 | −3 | 886–1880 |
| 1884–1891 | −3 | 1881–1888 |
| 1892–1894 | −3 | 1889–1891 |
| 1895 | → | 1892–1895 |
| 1896–1899 | 0 | 1896–1899 |
| 1900–1927 | +1 | 1901–1928 |
| 1928 | — | 已删 |
| 1929 | 0 | 1929 |
| 1930 | — | 已删 |
| 1931–3825 | −1 | 1930–3824 |
| 3826 | — | 已删 |
| 3827–3915 | −2 | 3825–3913 |
| 3916 | → | 3914–3916 |
| 3917–3932 | 0 | 3917–3932 |
| 3933 | — | 已删 |
| 3934–4507 | −1 | 3933–4506 |

注意 hunk 5 与 hunk 10 各自吞掉/吐出 2–3 行，所以 offset 在 −1/−3/0/+1/−2/−1 之间摆动；映射必须按区间查表。

## 4. v24 绝对锚点（新内容一律用这一列）

| 符号 | v24 行 | 说明 |
| --- | --- | --- |
| `ExtcRcSlab.prev` / `availPrev` | 223 / 255 | region 链与 size-class 链的双向摘链 |
| `slabNew` 记容量（无条件） | 885 | `capBytes += nslots*slotSize` |
| `extc_rc_fqDrainAuto` | 1531 | 有界排空：≤8 slab / 100 µs；`fqBytes > 8 MiB` 时才走 μs 级硬截止 |
| `extc_rc_slabShrink` | 1898 | 块级归还入口（`static long long`，返回入队块数） |
| `skip:shrink-null` | 1899 | 唯一的 null 早退 |
| `EXTC_RC_SHRINK_MIN_SLACK` | 1894–1895 | 8 MiB slack 闸门 |
| `skip:shrink-slack` | 1900 | slack 不足早退 |
| 摘链主循环 | 1903 | `while (s && seen < VISIT_MAX && rel < REL_MAX)` |
| `capBytes -= …` | 1929 | 块离开链时容量一起离开 |
| `extc_rc_pageTrimStep` | 2036 | 页级归还（v23 机制，本轮仅换调用位置） |
| `extc_rc_sweepStep` | 3848 | 轮末收尾分支在 3914–3918 |
| `extc_rc_slabShrink(r)` 调用点 | 3915 | **唯一的块级归还调用点**（在 pause 计时块内） |
| `extc_rc_fqDrainAuto()` 调用点 | 3916 | 同一步内排空一个有界批次 |
| `extc_rc_pageTrimStep(...)` 调用点 | 3917 | 与块级归还同一个收尾分支 |
| `extc_rc_settle` | 3922 | 不再摘块（只剩 free 队列排空） |
| `extc_rc_maybeCollect` | 4292 | 自动路径入口 |

## 5. 使用规则

1. 老文档里 1,600 多处 v17/v18 行号**不逐条重指**（见 MEMORY-MODEL 头部口径）；只有本轮新增内容用 v24 绝对行号，并标 `(v24)`。
2. 任何一次换源码版本，都先跑 `git diff -U0` 数 hunk，再按第 3 张表平移；**不要**用单一 offset 平移全文 —— v23→v24 的 offset 在 −3 到 +1 之间跳了 6 次。
3. 行号只用于源码溯源；判据与数字以 `docs/analysis/` 的实测表为准。
