# P1-RQ3：`EXTC_RC_SCAN_CHUNK` 取舍实验（task-26）

状态：实验完成，**默认值不改**（仍为 8192）。本文件是新量具 `[segsteps]` 的口径、chunk×deadline
的完整数据、以及"为什么单旋钮不可行、下一步该做什么"的证据。源码基线 v6
（src/back/reclaim.c md5 7c2a9cedd7edd54de0a378d8a507dcbe，3801 行）。

---

## 0. 结论（TL;DR）

1. **续扫段步（`seg>0`）的 p99.9 只由 chunk 决定，且 128/256 能达到 5 µs**：
   pd 形状 128→0.65–1.65 µs、256→0.89–3.54 µs；8192→34–87 µs。
2. **但 max 尾部与 chunk 无关**：所有 chunk 都有 58–871 µs 的离群段步（占 1e-4..1e-3，over1ms=0），
   归因为 `minflt=0 / first=0 / trans=0`，机制未定位（§11 存疑）。⇒ **`max ≤ 10 µs` 在任何 chunk 下都不可达**。
3. **内存才是 chunk 的代价**：保留 `EXTC_RC_QUOTA_DEADLINE_US=800` 时，128/256-word 的段步
   花不完一次 8 MiB 分配挣到的 mark 配额（剩余配额按次丢弃），mark 落后 ⇒ pd 峰值
   215–240 MB / 126–134 MB，而 8192 是 26.6 MB。
4. **关掉 deadline** 峰值回到 26.6 MB，四形状墙钟 ≤ +2.1%、RSS ≤ +4%，但单次 8 MiB 分配内
   GC 工作量从约 0.8 ms 涨到 1.6–2 ms（配额必须一次花完），chunk=128 时 chase 墙钟 +6.2%。
5. **决定：保持 8192 + deadline 800（chunk 不动）。** 三条量化理由：①内存优先是用户硬约束
   （m03301），128/256 + dl800 的 pd 峰值 215–240 / 126–134 MB，A 档 26.6 MB；②关 deadline 的
   128/256 档把停顿搬进 alloc 通道（单次 8 MiB 分配内 GC 0.8→1.5–2 ms、allocstall >1 ms 从 20 个
   涨到 179/196 个），且删掉了唯一把"一次分配内可花回收量"物理封顶的机制，理论停顿上界从
   `D + 2 segments ≈ 1 ms` 退化到 `quota × c_unit`（8 MiB >> 3 = 1,048,576 units ⇒ 10 ms 量级）；
   ③A 的两个通道各自有显式上界——常规/续扫步 p99.9 34.08 µs（对 50 µs 绝对兜底）、大分配通道
   ≤ `D + 2 segments` ≈ 0.96–1.0 ms（over-1ms=0）。即"用一次可界定的步时换掉一次不可界的
   分配内停顿"，与 memory-first + 停顿有界一致。若要改 chunk，前置条件是 **markResume 快路径**
   （一步内连续续扫多段、按字数预算 W 计费，摊掉每段固定开销）且保留 deadline 兜底。三档表见 §10。

---

## 1. 新增量具 `[segsteps]`（本任务交付，独立于 chunk 结论）

| 项 | 内容 |
| --- | --- |
| 语义 | 只统计**续扫段步**（markStep 进入时 `r->scanObj != 0`、`seg = extc_rc_markResume(r) > 0`） |
| 格式 | `[segsteps] n=… pops_tot=… max_us=… b1us=… b2us=… … b64ms=… bge64ms=…`（键名照 m04131） |
| 桶 | 与 `[histo]` 同 15 条 edges/名称：bucket i 装 `d <= edges[i]`，`bge64ms` 装 `> 64 ms` |
| 时钟 | 与 `[histo]` 完全同口径：`EXTC_RC_STEP_CPU=1` 且可用时为 CPU，否则墙钟（`_d` 的计算方式决定） |
| `pops_tot` | 这些段步的 `pops` 之和。结构上 `seg>0` 的步在 markStep 里 `pops++` 一次并 `budget=0` ⇒ 应为 `pops_tot == n`；不等就说明分段路径里混进了别的 pop |
| 无段步时 | 打印全 0（不省略行） |
| 实现 | 声明 src/back/reclaim.c:1100；记录 :2941–2943（markStep 的 pause 块内、`stepNote` 之后、`topNote` 之前）；打印 :3445–3447（`[histo]` 之后、`[top]` 之前） |
| 文档 | docs/design/RECLAMATION.md §四.一 量具段与 bullet 已登记 |

为什么需要它：`[step]` / `[histo]` 把 `seg=0` 的冷 pop 步、sweep 步与续扫段步混在一起，
而 chunk 只影响续扫段步。pd_pointer_dense 的 420 µs 级步是 `seg=0`、`pops=256` 的冷 pop 步
（1.3–1.9 µs/obj），与 chunk 无关；不单列就会把"冷 pop"误判成"段太长"。

driver 侧守恒断言（tests/reclaim/driver.c:t_scanChunk，:1019–1021）：
`segs == ceil(words/chunk)`、`segsum == words`、`segmax <= chunk`。
chunk=1e9 的红自测仍由既有"一步扫不完"断言给出。

---

## 2. 判据分类（Lead m04131）

| 类别 | 判据 | 本任务是否用它裁决 chunk |
| --- | --- | --- |
| 续扫段步 `seg>0`（结构上 pops=1） | p99.9 ≤ 5 µs、max ≤ 10 µs、over1ms=0 | **是，唯一口径** |
| warm 常规步 `seg=0`（cyc/pc/pb） | `>50 µs` 占比 ≤ 1e-4 且 p99.9 ≤ 10 µs | 只查不回退（本次无变化） |
| cold-dense `seg=0`（pd_pointer_dense） | 单列：τ_t=步时/pops（目标 ≤2 µs/obj）、p99.9 ≤1 ms、over1ms=0 | 不用于 5 µs 门禁 |

warm 形状实测（pc_cycle_churn 5M，默认档 vs chunk128+deadline0）：
两档 `[step] n=684`、max 5.7–6.3 µs、total 410–431 µs、`over64=0`、`[segsteps] n=0`
（该形状没有大对象，不产生续扫段步）⇒ 无回退。

---

## 3. 表 A：pd_bigobj 200 轮的续扫段步分布（pct 探针，µs）

| chunk | deadline | n | p50 | p90 | p99 | p99.9 | max | >5 µs | >10 µs | >1 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | 800 | 122,943 | 0.27 | 0.37 | 0.49 | **1.65** | 58.89 | 59 | 38 | 0 |
| 256 | 800 | 110,655 | 0.41 | 0.53 | 0.70 | **1.84** | 113.90 | 73 | 58 | 0 |
| 512 | 800 | 83,968 | 0.73 | 0.93 | 1.31 | 8.19 | 870.98 | 94 | 76 | 0 |
| 8192 | 800 | 25,407 | 6.93 | 9.32 | 12.48 | **34.08** | 157.10 | 17,584 | 1,018 | 0 |
| 128 | 0 | 1,089,536 | 0.26 | 0.33 | 0.42 | **0.65** | 120.80 | 351 | 200 | 0 |
| 256 | 0 | 811,071 | 0.38 | 0.49 | 0.61 | **0.89** | 162.67 | 442 | 300 | 0 |
| 512 | 0 | 405,567 | 0.59 | 0.78 | 0.96 | 1.68 | 172.62 | 293 | 231 | 0 |
| 8192 | 0 | 25,407 | 6.10 | 8.96 | 12.04 | **87.28** | 199.87 | 16,010 | 654 | 0 |

表 A2：pd_pointer_dense 200 轮（同一探针）

| chunk | deadline | n | p99.9 | max | >5 µs | >10 µs | >1 ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | 0 | 2,048,000 | 1.34 | 280.23 | 788 | 590 | 0 |
| 256 | 0 | 966,656 | 3.54 | 255.88 | 954 | 754 | 0 |
| 512 | 0 | 512,000 | 11.05 | 209.38 | 631 | 561 | 0 |
| 8192 | 0 | 32,512 | 59.71 | 260.66 | 31,312 | 23,420 | 0 |
| 128 | 800 | 2,048,000 | 1.39 | 178.99 | 919 | 693 | 0 |
| 256 | 800 | 966,656 | 1.98 | 246.00 | 757 | 636 | 0 |
| 8192 | 800 | 32,512 | 56.40 | 289.60 | 31,470 | 23,738 | 0 |

读法：
- p99.9 随 chunk 线性下降（工作量 ∝ chunk），128/256 满足 5 µs；
- **max 尾部在所有 chunk 都远超 10 µs**，且 >10 µs 的绝对步数在 128 与 8192 之间同一量级
  （几十~几百），不是"更长的段"造成的；
- over1ms 恒 0，所以 5 µs 门禁不是"有界性"问题，而是"尾部"问题。

---

## 4. 表 B：pd 200 轮的峰值内存与总步时（`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，2 reps）

| chunk | deadline=800：RSS (kB) | `[step] n` | total (ms) | deadline=0：RSS (kB) | `[step] n` | total (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| 128 | 215,188–239,764 | 124,298–124,343 | 36.6–36.9 | 26,644 | 1,090,068 | 280.2–288.1 |
| 256 | 133,204 | 108,402–111,975 | 48.9–49.2 | 26,644 | 811,913 | 299.8–317.5 |
| 512 | 84,052 | 89,846–93,992 | 65.2–67.5 | 26,644 | 406,409 | 231.0–234.1 |
| 1024 | 51,220 | 68,380–68,397 | 84.2–86.2 | 26,644 | 203,657 | 202.5–226.6 |
| 2048 | 34,704 | 51,506–51,551 | 114.1–115.2 | 26,644 | 102,281 | 168.5–194.7 |
| 4096 | 34,708 | 28,530–30,694 | 106.9–108.9 | 26,644 | 51,593 | 165.8–178.1 |
| 8192 | 26,644 | 26,116–26,249 | 166.9–170.3 | 26,640–26,644 | 26,249 | 162.7–177.4 |

（`[step] total` 是全程序累计的"被采样步"时长，不是墙钟；deadline=800 时小 chunk 的 total 反而小，
因为每次分配只推进一个段就撞 deadline 退出——工作量没做完，不是省了。）

---

## 5. 表 C：bench v2 四形状（taskset -c 6，墙钟中位，各 5–8 reps；`chunk128/256 + deadline=0`）

| 形状 | base (µs) | c128d0 (µs) | Δ | c256d0 (µs) | Δ | RSS base/c128/c256 (kB) |
| --- | --- | --- | --- | --- | --- | --- |
| promote | 43,234 | 43,317 | +0.2% | 44,129 | +2.1% | 5,912 / 5,912 / 5,916 |
| sizeratio | 27,233 | 26,962 | −1.0% | 27,127 | −0.4% | 3,052 / 3,052 / 3,180 |
| chase | 152,598 | 162,123 | **+6.2%** | 154,937 | +1.5% | 73,692 / 73,692 / 73,696 |
| cycleratio | 16,044 | 15,363 | −4.2% | 16,110 | +0.4% | 2,848 / 2,848 / 2,852 |

- 墙钟回归只在 **chunk=128 的 chase（+6.2%，超 +3% 预算）**；chunk=256 四形状全在 +2.1% 内。
- sizeratio 的 RSS +4%（3,052→3,180 kB）落在该形状 base 的历史波动范围（3,052–3,180）内，不作结论。
- `chunk128 + deadline=800` 没做 bench（内存已否，见 §4）。

---

## 6. 表 D：分配级停顿（allocstall 探针，pd 200；只统计 >1 ms 的分配）

| 构建 | `[allocstall]` max | 1–4 ms 桶 | 4–16 ms 桶 |
| --- | --- | --- | --- |
| pd 8192 dl800 | 4,329.9 µs | 20 | 1 |
| pd 128 dl800 | 5,172.7 µs | 179 | 21 |
| pd 256 dl800 | 5,009.9 µs | 169 | 13 |
| pd 8192 dl0 | 4,504.5 µs | 14 | 0 |
| pd 128 dl0 | 5,568 µs | 134 | 1 |
| pd 256 dl0 | 5,360 µs | 196 | 0 |
| dense 8192 dl0 | 3,938.6 µs | — | — |
| dense 128 dl0 | 4,505.8 µs | — | — |

读法：pd 每轮 8 MiB 的 200 个大分配，其停顿主要由 payload 的 memset/首次触碰决定（基线 4.3–4.5 ms）；
小 chunk 把 GC 那部分叠上去，使约 200 个分配里 1–4 ms 档从 20 个涨到 134–196 个（≈ +0.8–1.1 ms/次）。
dense 的 2.1 亿次分配里只有约 200 次 >1 ms（大数组），同一结论。

---

## 7. 机制：小 chunk + deadline 为什么爆内存

一次 8 MiB 分配挣到 `W_m = min(n >> 3, 1<<20)` work units（`n` 是请求字节，见 P0-IMPL.md）。
mark 配额循环在**每次 advance 之后**检查 deadline（800 µs）；到点就 `break`，**剩余配额按次丢弃**
（不跨分配结转）。续扫段步在 markStep 里 `budget=0`，**一步只推进一段**，并支付一次固定开销：
markAdvance tick（每 64 次调用才真正推进）、配额循环、deadline 读钟、markStep 的记账/[top] 采样。

于是单位 deadline 窗口内能扫掉的字数 ≈ 窗口 / (固定开销 + chunk×c_scan)：
- chunk=8192：一段 ≈ 6–13 µs（walk 真实成本），窗口内能连续花掉配额；
- chunk=128/256：一段只有 0.3–0.5 µs 的 walk，但固定开销是它的数倍 ⇒ 800 µs 窗口只能推进少量段，
  剩下的配额丢掉 ⇒ mark 推进速度 < 分配速度 ⇒ 活集被"欠标"，峰值随 1/chunk 上升
  （8192:26.8 MB → 4096:34.7 → 2048:43.0 → 1024:59.8 → 512:84.6 → 256:126–134 → 128:215–240 → 64:404–413 MB）。

关掉 deadline（=0）后配额必须当场花完 ⇒ 峰值回到 26.6 MB，代价变成"分配内 GC 时间"：
一次 8 MiB 分配要连续跑完它挣到的配额（chunk=128 时约 1.6–2 ms，8192 约 0.8 ms），
表现为表 D 里 1–4 ms 档的分配数暴涨。**这就是"停顿×内存"的直接交换，不是实现 bug。**

---

## 8. cold-dense 的 420 µs 级步与 chunk 无关

pd_pointer_dense 的 `[top]` rank1 是 `pops=256, seg=0` 的 mark 步（`why=advance`），
即一次弹 256 个冷对象、每个 1.3–1.9 µs ⇒ 300–500 µs/步。它的 `[segsteps]` 段步
p99.9 是 1.34–3.54 µs（chunk 128/256），符合分段口径。按 m04131，它单列为 cold-dense：
τ_t = 步时/pops（本形状 1.3–1.9 µs/obj，目标 ≤2 µs/obj 达标）、p99.9 ≤1 ms、over1ms=0。
**不能用它去判 5 µs 门禁**；也不应为了它缩小 chunk（它的尾巴来自冷对象首次触碰，不来自段长）。

---

## 9. `releaseOld` 的续扫计费（`charge += EXTC_RC_SCAN_CHUNK / 8`）

- 该行把"续扫一段"折算成预算单位：8192/8 = 1024 units、128/8 = 16 units。
- 预算 `EXTC_RC_RELEASE_BUDGET` 本身按 units 计（默认 2048），所以 chunk 变小只会让一次
  `releaseOld` 能续扫更多段才耗尽预算——**改变的是释放的推进节奏，不是正确性**；
  早退时仍走"放弃 + 清 mark + 置紧迫"的既有收尾，不会漏清 mark。
- `[work]` / `[ubwork]` 的 `seg` 是**实际扫过的字数**，与预算单位无耦合 ⇒ 记账自洽。
  结论：chunk 改变不影响 release 语义，只影响"一次 release 暂停内能走多远"。

---

## 10. 决策：A 档（保持 8192 + deadline 800）的三档取舍表

| 档 | 配置 | 段步 p99.9 | 段步 max | pd 200 峰值 | 分配内 GC / allocstall >1 ms | bench v2 墙钟 | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **A（采纳）** | 8192 + dl800 | 34.08 µs | 157–241 µs | **26.6 MB** | ≈0.8 ms / 20 个 | 基准（±0.2%） | 采纳 |
| B | 256 + dl0 | 0.89–3.54 µs | 163–256 µs | 26.6 MB | ≈1.5 ms / **196 个** | ≤+2.1% | 否：停顿搬进整次分配 |
| B′ | 128 + dl0 | 0.65–1.10 µs | 121–280 µs | 26.6 MB | ≈1.6–2 ms / **179 个** | **chase +6.2%** | 否：超墙钟预算 |
| C | 128 + dl800 | 1.65 µs | 59 µs | **215–240 MB** | ≈0.8 ms | 未测（内存已否） | 否：破内存 |
| C′ | 256 + dl800 | 1.84 µs | 114 µs | **126–134 MB** | ≈0.8 ms | 未测（内存已否） | 否：破内存 |

采纳 A 的三条量化理由（与 §0 第 5 条一致，展开写）：

1. **内存优先是用户硬约束**（m03301）：C/C′ 的 pd 峰值 215–240 / 126–134 MB，是 A 的 5–9×；
   任何"保留 deadline、缩小 chunk"的档位都会落在这里。
2. **B/B′ 的代价正好打在停顿契约上**：①把"单步 ≤ 几 µs"换成"单次 8 MiB 分配内 1.5–2 ms"，
   allocstall >1 ms 从 20 个涨到 179/196 个，等于把停顿从可观测的分步通道搬进一个整次调用；
   ②deadline=0 删掉了唯一把"一次分配内可花的回收量"物理封顶的机制，理论停顿上界从
   `D + 2 segments ≈ 1 ms` 退化到 `quota × c_unit`（8 MiB >> 3 = 1,048,576 units ⇒ 10 ms 量级），
   契约上不可接受。
3. **A 的两个通道各自有显式上界**：常规/续扫步 p99.9 34.08 µs（50 µs 绝对兜底判 PASS）；
   大分配通道 ≤ `D + 2 segments` ≈ 0.96–1.0 ms（over-1ms=0，实测 p99.9 64 µs）。
   A 的 34 µs 是"用一次可界定的步时换掉一次不可界的分配内停顿"。

补充说明：
- C/C′ 直接排除（内存）；B′ 另因 chase +6.2% 超 ±3% 预算被否。
- **B/B′ 不是废弃数据**：若未来把大分配停顿预算放宽到 ≥2 ms 且允许去掉 deadline 兜底，
  B 是可选的"内存持平、段步 p99.9 0.89–3.54 µs、四形状 ≤+2.1%"档；下一轮 markResume 快路径
  可能让它以"快路径 + deadline 保留"的形态复活（此时可同时拿到小段长与步时上界）。
- 判据口径提醒：段步 p99.9 只看 `[segsteps]`；cold-dense 与 warm 常规步（`seg=0`）按 §2 的
  分类各自判，不用它判 5 µs。

## 11. 未验证项 / 局限

1. **max 尾部归因未定位**：PAUSE=2 下 top8 段步全部 `minflt=0 / first=0 / trans=0`、`seg=128 / pops=1`，
   却 80–196 µs（最高一次 1,596 µs）。排除缺页、跨 slab、冷块首扫；疑 THP/缓存冷读（CPU 时间真实）。
   未做 perf 采样，不下结论。
2. 表 A 的 pct 探针只对 `seg>0` 记账；探针自身的数组写入未从 `_d` 中扣除（量级 <0.1 µs，可忽略但未证）。
3. 表 B 的 `[step] total` 来自 runtime 自己的采样（受 PAUSE 开销影响），跨 chunk 比较可比、
   绝对值不可当墙钟。
4. bench v2 只测墙钟与 RSS，未重跑 callgrind Ir；chase 的 +6.2% 未做指令级归因。
5. sizeratio RSS +4% 落在 base 波动范围内，未加 reps 排除噪声。
6. 8192 的 `[segsteps]` max（157 µs）与更早的 `[top]` 观测一致，但 64 KB THP 边界效应未单独测量。
7. 单机（本机 6 号核）、单次会话；跨机器 c_scan 与固定开销会不同，绝对门禁值需按机重标。
8. `chunk=64` 的 404–413 MB 只跑了 pd 两 reps，未进表格正题（趋势已明确）。

---

## 12. 复现

```sh
# 编译器（v6 + [segsteps] 补丁）
make BUILDDIR=build-p1 -j24

# 生成被测程序（不改源码；chunk 用 -D 覆盖 #ifndef）
./build-p1/extc tests/gc-p0/src/pd_bigobj.extc -o /tmp/rfwork/chunk/pd.base.c
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L /tmp/rfwork/chunk/pd.base.c -o /tmp/rfwork/chunk/pd.base
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -DEXTC_RC_SCAN_CHUNK=128 -DEXTC_RC_QUOTA_DEADLINE_US=0 \
   /tmp/rfwork/chunk/pd.base.c -o /tmp/rfwork/chunk/pd.c128d0

EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 taskset -c 6 /tmp/rfwork/chunk/pd.c128d0 200
# 读 [segsteps] / [step] / [histo] / [top]，并用 /usr/bin/time -f %M 取 RSS

# 回归门禁
bash tests/reclaim/run.sh                                   # 191/0
EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh                 # 247/0
EXTC=./build-p1/extc bash tests/reclaim-perf/gate.sh        # 86,621,815 <= 88,029,115
python3 tools/check_skip_table.py
python3 tools/check_no_history.py --gate
```

---

## 13. 归档（供 doc-audit 独立复核）

原始日志、探针与脚本已归档到 `tests/gc-p0/out/p1-chunk/`（2.4 MB，含 README 索引）：

| 路径 | 内容 |
| --- | --- |
| `pct-summary.txt` | 15 个配置的 `[pct]` 段步分布（表 A/A2 的第二 rep） |
| `pd.pct_<chunk>_<deadline>.{out,pct.log}`、`dense.pct_*` | 探针版二进制与输出（19 个配置） |
| `pd.*.log / pd.*.err / dense.*.log / dense.*.err` | chunk×deadline 矩阵的 [pause]/[step]/[histo]/[top]/[work]/[quota]/[segsteps] 原始输出 |
| `<shape>.args` / `<shape>.base.c` / `<shape>.base` | bench v2 四形状的参数与生成物 |
| `timeit.py`、`mkprobe.py` | 15-rep 墙钟脚本、探针注入脚本 |
| `pd.pct.c`、`dense.pct.c`、`pd.stall.c`、`dense.stall.c` | 探针版生成 C（可重编译） |
| `rf3_task26_patch.py` | task-26 源码补丁（[segsteps] + driver 守恒断言） |

第二 rep 的段步 p99.9（`pct-summary.txt`）：pd 128/256/512/8192 = 1.55/1.83/8.15/47.53 µs、
dense 128/256/512/8192 = 1.38/1.90/12.98/65.05 µs——与 §3 表 A/A2 同一数量关系；
max 尾部两 rep 在 60–853 µs 间波动（§11 第 1 条）。
