# REFS-V29：v28 → v29 行号换算（src/back/reclaim.c）

本文件是**行号换算表**，不是代码说明。v29 的唯一改动文件是 `src/back/reclaim.c`（其余源码不动），
改动是「跨区出边的 64 B 组位图登记」（wmark）。

## 0. 指纹

| 口径 | 文件 | md5 | 行数 |
|---|---|---|---|
| v28（基点 = HEAD `630b6e06` 的运行期源码）| `src/back/reclaim.c` | `7a157eb5d30bb3b71b67c41eea1c6f07` | 4,664 |
| v29（本轮）| `src/back/reclaim.c` | `845c57e2b230202efcd04639e07664d9` | 4,738 |
| v29 | `src/back/codegen.c` | `3ae6e499b6a2d64668ff1daf2eed26a2`（未改）| 16,222 |
| v29 | `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f`（未改）| 1,637 |
| v29 | `build/extc` | `b5ad0e7cdcb7abbf0cd8f8f18b113570` | — |

净 +74 行（+84 −10 的 hunks 里有 10 处是**整行替换**，不计净增）。

## 1. 变化块（`git diff -U0` 的 17 个 hunk，按新旧行号）

| # | 旧行 | 新行 | 类型 | 内容 |
|---|---|---|---|---|
| 1 | 220 之后 | 221–241（+21）| 纯插入 | 宏块：`EXTC_RC_WMARK`（默认 1，0 = 位图前行为，供 A/B）、`EXTC_RC_WMARK_MIN_BYTES` 4096，以及 16 行说明（为什么大槽的撤销成本 ∝ 跨度、位图是超集、可靠性依赖既有登记规则）|
| 2 | 268 之后 | 290–293（+4）| 纯插入 | `struct ExtcRcSlab` 尾部两字段：`uint64_t *wmark;`、`uint32_t wmW;` |
| 3 | 324 之后 | 350–351（+2）| 纯插入 | `struct ExtcRcRegion` 加 `int32_t wmUnsafe;`（该区有「登记了边但叫不出源槽」的写 ⇒ 整区退回全扫描，单调置位）|
| 4 | 977 之后 | 1005–1013（+9）| 纯插入 | `extc_rc_slabNew`：跨度 ≥ 4096 时 `wmW = ceil(ss/4096)`、`wmark = calloc(nslots*wmW)`，失败 `extc_rc_oops` |
| 5 | 1633 | 1669（1→1）| 行替换 | slab 销毁的 free 列表加 `free(s->wmark)`（第一处）|
| 6 | 1931 之后 | 1968–1983（+16）| 纯插入 | 新函数 `extc_rc_wmarkSlot(void **slot)`（记录 64 B 组）+ 4 行注释 |
| 7 | 1935 | 1987–1988（1→2）| 行替换 | `extc_rc_rsIncSlab(ExtcRcSlab*, void*)` → 三参 `(..., void **srcSlot)`，函数体首行 `if (srcSlot) extc_rc_wmarkSlot(srcSlot);` |
| 8 | 1959 之后 | 2013–2031（+19）| 纯插入 | `extc_rc_unlinkChildren` 的 `EXTC_RC_CONSERVATIVE` 分支：先走位图（只读置位组、读后把该组位清零），否则退回全扫描 |
| 9 | 2646 | 2718（1→1）| 行替换 | `storeRefOwnedReg` 的 NOCOUNT 分支：`rsIncSlab(sv, v)` → `rsIncSlab(sv, v, slot)`，删 `host->wmUnsafe = 1` |
| 10 | 2657 | 2729（1→1）| 行替换 | `storeRefOwnedReg` 的计数分支：同上 |
| 11 | 2713 | 2785（1→1）| 行替换 | `storeRefOwned` 的 NOCOUNT 分支：`rsIncSlab(sv, v, slot)`，删 `wmarkSet` 调用 |
| 12 | 2727 | 2799（1→1）| 行替换 | `storeRefOwned` 的计数分支：同上 |
| 13 | 2931 | 3003（1→1）| 行替换 | `storeStructCons` 的整值拷贝：保留 `r->wmUnsafe = 1`（这条路径没有源槽可指）|
| 14 | 2968 | 3040（1→1）| 行替换 | `storeStructCopy`：同上 |
| 15 | 3192 | 3264（1→1）| 行替换 | slab 销毁的 free 列表加 `free(s->wmark)`（第二处）|
| 16 | 3847 | 3919–3920（1→2）| 行替换 | sweep 的 elide 整块复位：`memset(wmark)` |
| 17 | 3957 之后 | 4031（+1）| 纯插入 | bulk reset（`memset(occ)/memset(mark)` 之后）加 `memset(wmark)` |

## 2. 分段换算（旧行 n → 新行）

| 旧行区间 | 新行 = 旧行 + | 备注 |
|---|---|---|
| 1–220 | 0 | 宏块之前 |
| 221–268 | +21 | 宏块之后、slab 结构之前 |
| 269–324 | +25 | `ExtcRcSlab` 两字段之后 |
| 325–977 | +27 | `wmUnsafe` 之后、`slabNew` 之前 |
| 978–1931 | +36 | `slabNew` 位图之后（含第一处 free 的整行替换，不改变位移）|
| 1932–1935 | +52 | `wmarkSlot` 之后；旧 1935 落到新 1987–1988 两行 |
| 1936–1959 | +53 | `rsIncSlab` 变三参之后 |
| 1960–3847 | +72 | `unlinkChildren` 位图分支之后；旧 3847 落到新 3919–3920 两行 |
| 3848–3957 | +73 | elide 复位之后 |
| 3958–4664 | +74 | bulk reset 之后；旧 4664（末行）→ 新 4738 |

## 3. 绝对锚点（v29 实测，供文档直接引用）

| 语义 | v29 绝对行 |
|---|---|
| 宏 `EXTC_RC_WMARK` / `EXTC_RC_WMARK_MIN_BYTES` | 221–241 |
| `ExtcRcSlab.wmark` / `.wmW` | 290–293 |
| `ExtcRcRegion.wmUnsafe` | 350–351 |
| `slabNew` 里位图分配 | 1005–1013 |
| `extc_rc_wmarkSlot` 定义 | 1972（注释 1968–1971）|
| `extc_rc_rsIncSlab` 三参定义 + 位图调用 | 1987–1988 |
| `extc_rc_unlinkChildren` 定义 | 2008 |
| `unlinkChildren` 的位图分支 | 2013–2031 |
| `storeRefOwnedReg` 定义 | 2702 |
| `storeRefOwned` 定义 | 2771 |
| 四处 `rsIncSlab(sv, v, slot)` | 2718 / 2729 / 2785 / 2799 |
| 整值拷贝的 `wmUnsafe`（两条）| 3003 / 3040 |
| `extc_rc_shade` | 2636 |
| `extc_rc_rsDecSlab` | 1998 |
| `[rt:wmark]` 的 free 两处 | 1669 / 3264 |
| elide 整块复位 | 3919–3920 |
| bulk reset | 4031 |
| `extc_rc_pageTrimStep` | 2222 |
| `extc_rc_maybeCollect` | 4524 |

## 4. 使用规则（与 REFS-V25/V28 相同）

1. **禁止单一 offset 平移**：位移在 0 / +21 / +25 / +27 / +36 / +52 / +53 / +72 / +73 / +74 之间跳变，按上表分段。
2. 文档里已有的 1,600 多处旧引用**不逐条重指**；只在被改到的段落附近换成 v29 绝对行号，其余保留旧号并在段落尾注明「行号口径见 REFS-V29」。
3. 任何行号断言都要 `grep -n` 复核（本文件的锚点表就是复核结果）。
