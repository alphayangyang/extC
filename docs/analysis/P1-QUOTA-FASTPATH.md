# P1-QUOTA-FASTPATH — 配额记账、P0-B 旋钮、elide O(1) 与 ELIDE_VERIFY（收口版）

（作者：runtime-facts；覆盖 src/back/reclaim.c md5 `9411a8ce5960e7d5c02f8545bb545a98`，3890 行 = FREEZE v6 + task-26/27 改动；构建 build-p1/extc md5 `c5ba461109ec31ccae8f3885e850141c`）

## 0 结论摘要

1. **任务书的前提被实测推翻**：P0-P 的配额算术/循环/时钟读在代表性形状上几乎不执行（sizeratio 2M 次分配只调 `markAdvance` 174 次，deadline checks=0；`-DEXTC_RC_QUOTA_MAX_MARK=1`、`-DEXTC_RC_QUOTA_DEADLINE_US=0` 变体 Ir 差 <0.1%）。"信用累积 + 阈值触发"按证据不可能收回 sizeratio 回归，未实施。
2. **sizeratio 的 Ir 回归真因是 P0-B 的懒复用链重建**：sweep 不再逐槽建链，代价搬到 alloc 的整块重建（2M 分配上重建 2020 次、共 1.89M 洞；重建相关 ≈19.5M Ir，alloc 体内 166.4M→211.7M）。回退后 sizeratio Ir 243.26M→228.39M（build-lead5 基线 229.27M）。
3. **P0-B 保留为编译期旋钮 `EXTC_RC_LAZY_FREE`，默认 0（回退路径）**。两档语义等价、全绿（见 §6 三段证据）：Ir 上 B=0 明确更好；B=1 的墙钟收益在噪声内不可复现（我的 13 reps 是 +4.63%，Lead 独立 9 reps 是 −4.3% 且方向相反）⇒ 当前证据不足以支持默认开启。
4. **elide 的 O(1) 化（nMark/nLive roll-up）已落盘**：全死块判定只看 `s->nMark == 0` 与 rsbit 全字，不再扫 occ/mark 位图；整块重置后 `nLive/nMark/nfree` 全部归零。
5. **`EXTC_RC_ELIDE_VERIFY`（默认 0）已实现**：命中整块重置前重扫 occ/mark/rsbit，断言 mark/rsbit 全 0 且 popcount(occ)==nLive，不一致则 `extc_rc_elideMismatch++` 并**不重置**（roll-up 漂移会重置含活对象的 slab = UAF 级）。verify=1 跑默认/计数两档 run.sh 全绿，pd_bigobj 200 与 cycleratio 400k 的 `[sweepelide] ... mis=0`（日志见 §3.6）。
6. **当前树全绿**：默认 run.sh 184/0 ×3 配置（正常/ASan+UBSan/ALLOC_INLINE=0）+ chunk=1 13/0；计数档 240/0 ×3 + 13/0；`LAZY_FREE=1` 与 `ELIDE_VERIFY=1` 档同样全绿；gate.sh **86,852,961 Ir ≤ 88,029,115**；G18 pd_bigobj 200 RSS **26,068 kB**（LAZY=1 26,260；verify 26,072）、[step] over1ms=0。
7. **未达判据**：sizeratio 墙钟 +4.63%（目标 ≤+3%），按 Lead m04964 走"② 目标未达，记录在案"。
8. **cycleratio Ir 分解**（本轮实测，见 §8）：相对"无 roll-up 维护 + 无 elide"的对照 C，elide 省 11.4M Ir、roll-up 维护花 2.67M Ir，净 −8.73M（−3.1%）；相对 build-lead5 的 +8.89M 未再分解。

## 1 归因表（sizeratio，args `2000000 20261013`，callgrind -O2 -g）

| 项 | before（build-lead5 产物） | after（回退 P0-B 后） | Δ |
|---|---|---|---|
| 总 Ir | 229,272,712 | 228,392,449 | −0.4% |
| `extc_rc_sweepStep` | 22.61M | 2.39M（回退前）| sweep 不再建链 |
| alloc 体（noinline 口径）| 166.38M | 211.73M（回退前）| +45.3M |
| 重建块（2020 次/1.89M 洞）| — | ≈19.5M（回退后已删除）| tid 链 3.94M、while(h) 3.92M、ctz 3.77M、idx 3.77M |
| `markAdvance` 调用 | 174 次（0.0087% 分配）| 174 | 配额循环几乎不执行 |
| deadline checks | 0 | 0 | quota<4096 永不读钟 |

**负结果：cursor-hole 方案**（slab 加 `holeWord`，从游标逐 word `valid & ~occ` 找洞）：sizeratio Ir **299.4M（+56M）**，洞稀疏时每洞重扫 O(到下一洞 words)，摊销远差于整块建链，已回退。

## 2 墙钟归因（方法论与证据）

- perf stat 不可用（WSL 无 PMU "No supported events found"），只能 utime/callgrind/cachegrind。
- **测量教训**：分块跑 9 reps 会被频率/热漂移污染（同一二进制 A vs A-copy 也测出 +1.29%）。必须**每个 rep 内轮转所有二进制**（round-robin）。
- 混合体实验（runtime 文本 vs app 文本互换，11 reps 轮转中位）：before 22.13ms、P0-B 版 24.14ms、before-runtime+新 app **21.91ms**、新-runtime+before-app **24.19ms** ⇒ 差异来自**运行期文本**，应用代码逐字节相同。
- 逐特性关闭（对 P0-B 版生成 C 副本）：全开 23.79、去 `_b += _q` 23.01、去 elide 调用 22.22、去 deadline 23.64、去 MAX_MARK 23.02 ⇒ 单点变体噪声大（代码布局敏感），不可作因果结论。
- 决定性反证：当前树与基线在 sizeratio 上 **GC 步总时长 2553 vs 2592µs、collects 171 vs 171、[work] 175 vs 176**，而总墙钟差 ~1ms ⇒ 差额不在 GC 内。
- `-falign-functions=64 -falign-loops=64` 重编后差距未收敛（+8.4% 一次、+4.6% 另一次），进一步指向布局/噪声而非算法。

## 3 elide O(1) 化（nMark/nLive roll-up）

- `ExtcRcSlab` 新增 `uint32_t nLive; uint32_t nMark; uint32_t nfree;`（slabNew 全部置 0）。
- 维护点：alloc 置 occ 后 `nLive++`；marking/sweeping 分支置黑处 `nMark++`；`freeSlot` 清位后 `nLive--` 且原 mark 位为 1 时 `nMark--`；`freeSlotWord` `nLive -= n`；`sweepWord` 清 word 前 `nMark -= popcount(mark[w])`；`shadeSlow`/`MARK_SCAN` 置黑处 `nMark++`；releaseOld 收尾 memset mark 时 `nMark=0`；整块重置把 `nLive/nMark/nfree` 清零。
- `extc_rc_sweepElide`：`if (!s)`/`big`/`foreign` 早退 → `if (s->nMark != 0) return 0`（O(1)）→ `rsbit` 全字扫描早退（`skip:elide-xregion`）→ `live = s->nLive`，整块清零 occ/mark、`bump=0`、`freelist=-1`、记账、classPush、置 swEpoch、游标前进。
- 实测：sizeratio calls=2394 fast=2304 scan=90 resets=90 words=1440；cycleratio calls=756 fast=105 scan=651 resets=651 words=27,993。

## 3.5 `EXTC_RC_LAZY_FREE` 旋钮（默认 0）

- 语义：`0` = 每个洞在释放时立即链入 `freelist`（freeSlot 单槽链入、freeSlotWord 逐死槽链入）；`1` = sweep 只把洞计入 `s->nfree`，alloc 在该块 `bump>=nslots && nfree>0` 时**按 occ 位图整块重建一次链**再弹出。
- 不变式（两档共用）：`full = (bump >= nslots && nfree == 0)`；重建只在 `freelist == -1` 时发生 ⇒ 同一个洞不会二次入链；重建后用实际链长覆写 `nfree`（不信增量）。
- 默认 0 的理由：Ir（§6 第 1 段）B=0 好 19.5M；B=1 墙钟收益不可复现（§6 第 2 段）；懒重建与 elide/slabNew 清链交互是正确性敏感路径；两档停顿尾部无差别。
- 两档全绿证据：默认 run.sh：默认档 184/0、`LAZY_FREE=1` 184/0、`LAZY_FREE=1 + ELIDE_VERIFY=1` 184/0；计数档 240/0 与 `LAZY_FREE=1` 240/0。G18 pd_bigobj 200：LAZY=0 RSS 26,068 kB、LAZY=1 26,260 kB（门禁量级 26.4–26.5 MB）。

## 3.6 `EXTC_RC_ELIDE_VERIFY`（默认 0）

- 在整块重置的记账之前插入：`for w: if (mark[w] || rsbit[w]) { mismatch++; return 0; } _occ += popcount(occ[w]); if (_occ != nLive) { mismatch++; return 0; }`；早退带 `skip:elide-verify-mismatch` / `skip:elide-verify-nlive` 标记。
- 计数器 `extc_rc_elideMismatch` 进入 `[sweepelide]` 行：`[sweepelide] bulk_reset=… bulk_slots=… swept=… mis=0`。
- 证据（verify=1 编译生成 C）：
  - 默认 run.sh `ELIDE_VERIFY=1`：184/0 ×3 配置 + chunk=1 13/0。
  - pd_bigobj 200：`[sweepelide] bulk_reset=0 bulk_slots=0 swept=782 mis=0`、`[step] max_us=238.5 over1ms=0`。
  - cycleratio 400k：`[sweepelide] bulk_reset=651 bulk_slots=1738046 swept=105 mis=0`、stdout hash 与默认档相同（`e98affe9e34e`）。
  - 日志归档：tests/gc-p0/out/p1-verify/。

## 4 最终数字（当前树，build-p1/extc）

| 指标 | 值 |
|---|---|
| src/back/reclaim.c | md5 `9411a8ce5960e7d5c02f8545bb545a98`，3890 行 |
| build-p1/extc | md5 `c5ba461109ec31ccae8f3885e850141c` |
| tests/reclaim/driver.c | md5 `58fa1c9a4b86d8c4c4b9c15d7711afe2` |
| sizeratio Ir | 229,272,712 → 231,303,870（+0.89%）|
| sizeratio 墙钟（13 reps 轮转中位）| 22.66 → 23.71ms（**+4.63%**，未达 ≤+3%）|
| promote 墙钟 | 39.52 → 39.46ms（−0.15%，达标）|
| cycleratio Ir | 264,297,261 → 273,187,889（**+3.36%**，分解见 §8）|
| cycleratio 墙钟 | 13.40 → 13.72ms（+2.41%）|
| G18 pd_bigobj 200 | RSS 26,068 kB（LAZY=1 26,260；verify 26,072）、[step] max 106–238µs、over1ms=0、[quota] hits 1–2 |
| [sweepelide] pd | bulk_reset=0 bulk_slots=0 swept=782–786 mis=0 |
| chain2 2M | 默认 rss 3032 kB / 事件档 1492 kB，out_hash 两档相同 |
| gate.sh | 86,852,961 Ir ≤ 88,029,115 PASS |
| run.sh 默认 / 计数 | 184/0 ×3 + 13/0 / 240/0 ×3 + 13/0；LAZY=1、ELIDE_VERIFY=1 同 |

## 5 task-26 量具未被误删

`grep -c extc_rc_segN src/back/reclaim.c` = 3（声明/记录/打印三点）；`[segsteps]` 打印行与 markStep 记录点保持；driver 守恒断言保留：tests/reclaim/driver.c:1019 `check(segs == want, ...)`、:1020 `check(segsum == words, ...)`、:1021 `check(segmax <= ...)`（chunk=1e9 仍红）。

## 6 P0-B 证据三段

1. **我的测量（13 reps round-robin 中位）**：sizeratio 22.66 → 23.71ms（+4.63%），Ir 229.27M → 231.30M（+0.89%）；promote −0.15%；cycleratio +2.41%。B=1 版（回退前）实测 Ir 243.26M，B=1 在 sizeratio 上更差。
2. **Lead 独立 A/B（`/tmp/v2/v2run --pin 6`，9 reps 奇偶换序）**：promote v6(B=1) 中位 39.87ms vs p1(B=0) 41.67ms ⇒ B=1 约 −4.3%（与我的数字方向一致但幅度不同）；sizeratio v6 中位 26ms vs p1 28ms，**方向与我相反**，且 22ms 程序在 load≈3 时散布 22–146ms、噪声区间完全重叠 ⇒ 无法分辨 4% 差异。
3. **结论**：B 的墙钟收益在噪声内**不可复现** ⇒ 默认 0；若将来需要，先建低负载测量协议（**utime 为主、≥21 reps、CI**）再评。不写成"B 无收益"，只写"当前证据不足以支持默认开启，且 Ir 明确变差"。

## 7 为什么没做到 ≤+3%（再做需要什么）

- 剩余差额与 Ir、GC 步时、collects 均不相关；在 runtime 文本层做任何小改动都会因布局移位产生 ±3–8% 的 sizeratio 墙钟抖动（同二进制重编差异亦达 +1.3%）。要把该形状噪声压到 ≤3%，需要**更稳的测量协议**（固定频率/大 reps/utime 口径 + 置信区间），而不是继续改算法。
- 建议 empirics 在 G4 判定中用"多形状 + Ir + RSS"而非单一形状墙钟。

## 8 cycleratio Ir 分解（本任务新增）

在同一份生成 C 上做三份测量（callgrind，400000 20261013 100）：

| 变体 | Ir | 说明 |
|---|---|---|
| V0 当前（roll-up 维护 + O(1) elide）| 273,188,831 | 交付态 |
| V1 = V0 但 elide 调用关闭（`if (0)`）| 284,594,252 | 只保留 roll-up 维护，退回逐 word 扫 |
| V2 = V1 再去掉 roll-up 维护（alloc/freeSlot/freeSlotWord/sweepWord 的 nLive/nMark 更新）| 281,919,512 | ≈ 无 roll-up、无 elide |

- **O(1) elide 的收益**：V1 − V0 = **11.40M Ir**（cycleratio 上 651 次整块重置代替 1.74M 死槽逐槽扫描）。
- **roll-up 维护的成本**：V1 − V2 = **2.67M Ir**（≈+0.95%）。
- **净效果**：V0 比 V2 少 8.73M（−3.1%）。
- **未分解部分**：V2 比 build-lead5 的 264.30M 高 17.6M，V0 高 8.89M；两者源码不同（lead5 = P1-7 elide + P0-P，无 roll-up、无 P0-B 回退），未能在同一生成 C 上对齐，故 +8.89M 只作为"其他/口径差异"记账，不归因到某一函数。

## 9 存疑 / 未验证

1. build-lead5 与当前树无法在同一生成 C 上对齐，cycleratio 的 +8.89M 未逐函数归因（§8）。
2. 墙钟差异的性质（I-cache/分支别名 vs 未识别的微架构效应）未用 PMU 证实（本机无 PMU）。
3. 单形状墙钟结论不稳，需要 empirics 的稳定测量协议复核。
4. `LAZY_FREE=1` 与 elide/slabNew 清链的交互只由 run.sh 两档 + pd G18 覆盖，未做专项 fuzz；默认 0 因此是"减少表面积"的选择。
5. idle 指纹基线需在冻结后 refreeze（新增 `nfree` 字段、`extc_rc_elideMismatch` 与 `[sweepelide] mis=` 改变 rc_syms）。
