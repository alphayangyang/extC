# REFS-V28 · v25 → v28 行号映射、变化块与绝对锚点

本文件是 v28（2026-11-14 工作树，**R-A 轮：标记路径由屋顶选，冷档走批量、暖档走浅栈**）的行号口径。v28 相对 v25 是**纯插入**的一轮：两个 hunk、共 +6 行，没有任何删除或改写，因此换算比 v25 简单得多 —— 但仍然**不允许**用一个常数平移整个文件，因为两段插入的位置不同（+3 与 +6 分界在第 3,620 行）。

## 0. 为什么仍然禁止单一 offset 平移

两个 hunk 分别在旧文件的第 3,338 行之后与第 3,619 行之后各插入 3 行。于是文件被切成三段：

- 第 1–3,338 行：偏移 0；
- 第 3,339–3,619 行：偏移 +3；
- 第 3,620 行之后：偏移 +6。

如果对全文用「旧行号 + 6」，那么第 3,619 行以及它之前的 281 行都会指到插入块之后的错位位置（最多偏 3 行）。本目录的规则不变：**旧引用不逐条重指，按符号检索 + 本文件的分段表换算**。

## 1. 指纹（本文件写定时的构建）

| 文件 | md5 | 行数 | 相对 v25 |
|---|---|---|---|
| `src/back/reclaim.c` | `7a157eb5d30bb3b71b67c41eea1c6f07` | **4,664** | +6 |
| `src/back/codegen.c` | `3ae6e499b6a2d64668ff1daf2eed26a2` | 16,222 | 0（未改） |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` | 1,637 | 0（未改） |
| `build/extc` | `e742e423dd5b901cf0405f5beb3a4cb7` | — | — |

v25 基点（`git` HEAD = `fe6c1433`）：`src/back/reclaim.c` = `0b91d96ee08ff5e2589136ca313aee4f`（4,658 行）、`src/back/codegen.c` = `3ae6e499b6a2d64668ff1daf2eed26a2`（16,222 行）。

溯源只认**源码 md5**：`build/extc` 的 md5 随 BUILDDIR 变化（同源码不同 BUILDDIR 会得到不同的 extc md5），不要用它判「是不是同一棵树」。

## 2. 变化块（reclaim.c，共 2 处，全部是字符串数组内的纯插入）

行号格式为「v25 的插入点 → v28 新行」。两处都在 `RECLAIM_RT[]`（发射文本数组）里，所以**生成 C 的字节差异只有这两处**，暖档形状在预处理后只多一个合取项。

| # | v25 | v28 | 内容 |
|---|---|---|---|
| 1 | 3,338 后 | 3,339–3,341 | 新增 `EXTC_RC_MARK_SHALLOW_WARM_ONLY`（默认 1）三行：`#ifndef` / `#define`（宏值 1 + 一长行注释：浅栈是暖档优化，活集过 `EXTC_RC_MARK_COLD_BYTES` 后 pop 是 DRAM miss，批量才重叠延迟；0 恢复纯深度规则，供 A/B 与大 cache 机器）/ `#endif` |
| 2 | 3,619 后 | 3,623–3,625 | 浅栈快路条件里插入 `#if EXTC_RC_MARK_SHALLOW_WARM_ONLY` + 合取项 `&& (int64_t)r->live <= (int64_t)EXTC_RC_MARK_COLD_BYTES`（行尾注释 roof picks the path）+ `#endif` |

净变化：4,658 → **4,664** 行（+6）。

### 2.1 语义（不是行号，但读这两个块必须先知道）

- 第 2 块插在**原条件行之后、`#if EXTC_RC_WORK_BUDGET` 之前**，所以完整的浅栈条件在生成 C 里排成：`if ((size_t)r->msp <= EXTC_RC_MARK_SHALLOW && budget > 0` → 新的 `#if` 守卫 + `&& r->live <= EXTC_RC_MARK_COLD_BYTES` → `&& wleft > 0` → `) {`。宏为 0 时预处理结果与 v25 **逐字相同**（这是 ctrl 臂的构造方式）。
- 冷档的**两个机制**因此叠加：`extc_rc_markStep` 里的 `b` 钳到 `EXTC_RC_MARK_COLD_BATCH`（=32，v25 已在），以及本条把浅栈路径**整体关掉**（新增）。后者才是 LAT 形状 2.60× 的来源：v25 的冷档在 `msp<=64` 时仍走浅栈，逐个 pop、不重叠 miss。
- 两处插入都是**发射文本**（C 字符串数组元素），不是 extc 编译器自身的预处理指令。R-A 的第一版把 `#if` 写成裸行，导致这两条指令作用于**编译器自身**的编译（宏未定义 ⇒ 0），新条件被整行排除出发射文本，A 完全不生效 —— 这是本轮最重要的一条教训，已写进 changelog。

## 3. 分段映射表（v25 → v28）

| v25 行号 n | v28 行号 | 说明 |
|---|---|---|
| n ≤ 3,338 | n | 插入点之前，偏移 0 |
| — | 3,339–3,341 | 纯新增（无旧对应） |
| 3,339 ≤ n ≤ 3,619 | n + 3 | 第一段插入之后 |
| — | 3,623–3,625 | 纯新增（无旧对应） |
| n ≥ 3,620 | n + 6 | 第二段插入之后 |

反查（v28 → v25）：`m ≤ 3,338` ⇒ `m`；`m ∈ {3,339, 3,340, 3,341}` ⇒ 新增行；`3,342 ≤ m ≤ 3,622` ⇒ `m − 3`；`m ∈ {3,623, 3,624, 3,625}` ⇒ 新增行；`m ≥ 3,626` ⇒ `m − 6`。

## 4. 绝对锚点（v28 实测 `grep -n`，供核对换算结果）

| 符号 / 文本 | v28 | v25（换算） |
|---|---|---|
| `extc_rc_slabNew` 定义 | 942 | 942 |
| `EXTC_RC_MARK_BATCH_MAX` | 1,253 | 1,253 |
| `EXTC_RC_MARK_COLD_BYTES` | 1,262 | 1,262 |
| `EXTC_RC_MARK_COLD_BATCH` | 1,266 | 1,266 |
| `EXTC_RC_MARK_STACK_SOFT` | 1,274 | 1,274 |
| `extc_rc_slabShrink` 定义 | 2,012 | 2,009 |
| `extc_rc_pageTrimStep` 定义 | 2,150 | 2,147 |
| **新增宏** `EXTC_RC_MARK_SHALLOW_WARM_ONLY` | **3,339–3,341** | —（新增） |
| `extc_rc_markResume` 定义 | 3,487 | 3,484 |
| `extc_rc_markStep` 定义 | 3,535 | 3,529 |
| 浅栈条件行（`r->msp <= EXTC_RC_MARK_SHALLOW`） | 3,622 | 3,619 |
| **新增守卫块**（`#if` + 合取项 + `#endif`） | **3,623–3,625** | —（新增） |
| `extc_rc_sweepElide` 定义（首份） | 3,919 | 3,913 |
| `extc_rc_sweepWord` 定义 | 3,991 | 3,985 |
| `extc_rc_sweepStep` 定义 | 4,006 | 4,000 |
| 收尾分支（shrink / fqDrainAuto / pageTrim） | 4,070–4,075 | 4,064–4,069 |
| `extc_rc_settle` 定义 | 4,080 | 4,074 |
| `extc_rc_thrUpd` 定义 | 4,104 | 4,098 |
| `extc_rc_markStep(r, extc_rc_stepBudget(r))` 调用 | 4,432 | 4,426 |
| `extc_rc_maybeCollect` 定义 | 4,450 | 4,444 |

## 5. 生成 C 的证据（可复现）

- 暖档形状：`./build/extc tests/gc-p0/src/lat_randchain.extc -o /tmp/lat_new.c`，在 `sed -n 3160,3168p` 处能看到三条新文本；`grep -c "roof picks the path"` = 1，`grep -c "EXTC_RC_MARK_SHALLOW_WARM_ONLY"` = 3。
- 对照：`cc -E -DEXTC_RC_MARK_SHALLOW_WARM_ONLY=0` 与 v25 的生成 C 预处理结果**逐字相同**（ctrl 臂）；默认构建在预处理后只多那个合取项。四条暖档形状的 `.text` 成对等长（42,898 / 42,930 等）。

## 6. 使用规则

1. 旧文档里按 v17/v18/v21/v22/v23/v24/v25 写下的 1,600 余处行号**不逐条重指**；要引用时先按**符号名**检索，再用 §3 的分段表把旧行号换成 v28。
2. 引用「新增机制」时用 §4 的绝对锚点，不要用换算值。
3. 任何脚本要写行号区间，必须用 §3 的分段函数，不得使用单一 offset。
