# 为什么 Go 的 GC 停顿这么快（`task-44`，调研非实现）

> **标注约定**（全文遵守 ✗）：`【实测】` = 本机跑出来的，带原始行；`【文献】` = 官方文档/设计文档/源码原文，带链接或
> `文件:行`；`【推理】` = 由上述两者推出来的判断，**不是**实测 ✗。三者不许混 ✗。
> 本件**只调研**：`src/` 未动 ✗、`tests/`/`docs/CHANGELOG.md` 未动 ✗；临时脚本与原始行都在 `/tmp/gopause/` ✓。

## 0. 一句话结论

**Go 快，不是因为它“GC 跑得少”，也不是因为并行** —— 是它把 **O(活集)** 的标记工作**搬出了暂停**：
同一形状、同一 N/seed、同一核（`taskset -c 6`，Go 自己看到的就是 **`1 P`**）下，【实测】
`@1M`：Go 跑了 **53 个 GC 周期**，留在 STW 里的**总共只有 0.346 ms**（p50 **6 µs**、max **17 µs**、占墙钟 **0.07%**），
而同一个暂停里 extC 要花 **63.95 ms**（占墙钟 11.1%、p50 258.5 µs、max 2.3–3.2 ms ✗）。
更硬的一刀：**人为把整轮 GC 塞回一个调用里**（`GOGC=off` + 每轮 `runtime.GC()`，260 次，仍是 1 P）——
Go 的 `runtime.GC()` **整调用**要 **0.70 ms 均值 / 2.04 ms 最大**（与 extC 每轮 `collect` 的 0.26–3.2 ms **同量级** ✓），
但它**真正的 STW 部分仍然只有 4–22 µs** ✓。
⇒ **Go 的优势几乎全部来自“不在暂停里干”**，不是“它干的活更便宜” ✓；而对 extC 来说，这条杠杆
（增量/并发标记 + 写屏障）**已经被本仓库自己的实测否掉了**（`task-31`：屏障代价下界 **+38%** vs 理想收益上限 **18%** ✗）。

---

## 1. 机制：哪些在 STW、哪些不在（【文献】+ 源码）

环境：`go version go1.26.0 linux/amd64`【实测】（`GOROOT=/usr/lib/go-1.26`）。
**注意**：官方 [GC 指南](https://go.dev/doc/gc-guide) 自述“describes the garbage collector **as of Go 1.19**”【文献】，
本机是 1.26 ⇒ 下面凡涉及实现细节的都以**本机 `GOROOT/src/runtime` 源码**为准 ✓。

### 1.1 GC 周期的阶段表

| 阶段 | 是否 STW | 源码位置（本机 1.26） | 干什么 |
|---|---|---|---|
| **清扫终止**（sweep termination） | **是** | `runtime/mgc.go:835 stopTheWorldWithSema(stwGCSweepTerm)`；枚举名 `runtime/proc.go:1453` | 停世界、收尾在飞的清扫、准备标记 |
| **并发标记扫描**（mark & scan） | **否** | `runtime/mgc.go:1672 gcBgMarkStartWorkers` / `:1750 gcBgMarkWorker` | 后台标记 worker 从根开始标；CPU 预算由 pacer 限 |
| **标记协助**（mark assist） | 否（在**分配者自己的线程**上） | `runtime/mgcmark.go:499 gcAssistAlloc` | 分配跑得比标记快时，沿分配路径“帮忙标” |
| **标记终止**（mark termination） | **是** | `runtime/mgc.go:1084 stopTheWorldWithSema(stwGCMarkTerm)` | 停世界、把剩余标记收尾/flush |
| **清扫**（sweep） | **否**（后台/增量） | `runtime/mgcsweep.go:272 bgsweep`、`:359 sweepone` | 后台 goroutine 增量清扫 span |
| 写屏障 | **不是暂停**，是 mutator 的税 | `runtime/mgc.go:259 writeBarrier.enabled = …`；`mbarrier.go:154/250/317` | 维持三色不变式，让“边改边标”成立 |
| pacing（何时开标） | 不是暂停 | `runtime/mgcpacer.go`（`gcGoalUtilization` 头注释） | 决定下一轮何时启动，**保证标记在堆目标前完成** |

**官方对 `gctrace` 三段的定义**【文献，`runtime/extern.go:114-135` 原文】：

```
	gc # @#s #%: #+#+# ms clock, #+#/#/#+# ms cpu, #->#-># MB, # MB goal, # MB stacks, #MB globals, # P
…
	The phases are stop-the-world (STW) sweep termination, concurrent
	mark and scan, and STW mark termination. The CPU times
	for mark/scan are broken down in to assist time (GC performed in
	line with allocation), background GC time, and idle GC time.
	If the line ends with "(forced)", this GC was forced by a
	runtime.GC() call.
```

⇒ **只有第 1、3 段是 STW**；中间那段（标与扫）与 mutator 并发；`cpu` 段拆成 **assist / background / idle**；
`(forced)` 正好用来标我们人为塞进暂停的那些周期 ✓（下面的强制档就靠它认 ✓）。

### 1.2 各机制各干什么（【文献】）

- **写屏障**：并发标记期间必须保证“已标的不会漏” —— 赋值时把被改的槽记下来（`mbarrier.go`）⇒ 代价是**每次指针写**的税，不是暂停 ✓。
- **mark assist**：分配速率超过标记进度时，**分配者自己**按比例做标记（`gcAssistAlloc`）⇒ 把 GC 成本摊到分配上，
  官方指南明确“`gcAssistAlloc` 累计 >5% 说明应用在跑赢 GC”【文献，GC 指南 “Identifying costs”】。
- **增量清扫**：清扫与 mutator 并发、后台 `bgsweep` 分片做【文献，源码】；官方指南还说
  “**sweeping is so much faster than marking and scanning that the cost is negligible**”【文献，GC 指南 “Understanding costs”】。
- **pacing / `GOGC` / `GOMEMLIMIT`**：目标堆 = `Live heap + (Live heap + GC roots) * GOGC/100`；
  `GOMEMLIMIT` 给总内存上限（Go 1.19+）【文献，GC 指南】。pacing 的作用是**让并发标记在堆目标到达前完成**，
  否则就靠 assist 补 ⇒ 这正是“暂停与堆大小解耦”的机制来源 ✓。
- **并行度**：官方指南提到“应用只用一个 goroutine 但 `GOMAXPROCS>1` 时 idle worker 的时间会掉下来”【文献】——
  ⇒ 并行主要影响**标记的吞吐/CPU 账**，不改变“标记不在暂停里”这件事 ✓。

---

## 2. 实测：同一把尺（`digraph` · `@1M` unless noted · `taskset -c 6` · 3 次取中位）【实测】

产物：所有格的 stdout 与基准**逐字节相同** ✓（`cmp` 过）。原始行全在 `/tmp/gopause/`（脚本 `run.sh`，解析见 §6）。

**⚠️ 一个方法论发现**【实测】：`taskset -c 6` 会让 Go **只看得到 1 个 P**（gctrace 末字段 `1 P`）⇒
① `GOMAXPROCS=1` 那一格与默认格**本来就是一回事**（实测两面一致 ✓）；② §6.9/§6.10 已有的 Go 数字
**本来就是单 P 口径** ✓ ⇒ 它天然就是**与 extC 最可比**的一格 ✓。真·多核另测了 `taskset -c 6-9`（4 P）✓。

| 格 | env | P | 周期/跑 | **ΣSTW** ms | p50 | max | Σ并发mark(墙钟) ms | assist CPU ms | bg CPU ms | idle CPU ms | forced | 墙钟 ms | STW 占墙钟 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **g0** 默认（= §6.9 口径） | — | 1 | 53 | **0.346** | **6 µs** | 17 µs | 185.6 | 36.8 | 2.7 | 0 | 0 | 503.6 | **0.07%** |
| **g1** `GOMAXPROCS=1` | GOMAXPROCS=1 | 1 | 53 | 0.363 | 6 µs | 17 µs | 190.9 | 36.7 | 3.7 | 0 | 0 | 513.1 | 0.07% |
| **g5** `GOGC=off` 不强制 | GOGC=off | 1 | **0** | —（显式 0 ✓） | — | — | — | — | — | — | 0 | 485.9 | — |
| **g3** 强制（1 P） | `GOGC=off FORCE_GC=1` | 1 | **260** | **1.143** | 4 µs | **22 µs** | 165.5 | 0 | 2.3 | 162.4 | 260 | 591.2 | 0.19% |
| **g4** 强制（重复组） | 同上（无 GOMAXPROCS，仍 1 P） | 1 | 260 | 1.181 | 4 µs | 20 µs | 165.9 | 0 | 1.9 | 163.4 | 260 | 580.6 | 0.20% |
| **g2** `GOGC=1`（`@250k`） | GOGC=1 | 1 | 33 | 0.172 | 4 µs | 35 µs | 14.4 | 8.9 | 0.1 | 0 | 0 | 55.6 | 0.03% |
| **g6** 真·4 P（`taskset -c 6-9`） | — | **4** | 60 | **2.304** ✗ | 34 µs | 125 µs | 47.3 | 5.5 | — | — | 0 | 471.2 | 0.48% |

**原始行（原文 ✓）**：

```
# g0（1 P，默认）
gc 1  @0.003s 5%: 0.004+1.1+0.003 ms clock, 0.004+0.29/0/0+0.003 ms cpu, 3->4->0 MB, 4 MB goal, 0 MB stacks, 0 MB globals, 1 P
gc 30 @0.161s 8%: 0.004+4.8+0.002 ms clock, 0.004+0.93/0/0+0.002 ms cpu, 3->4->2 MB, 4 MB goal, 0 MB stacks, 0 MB globals, 1 P

# g6（4 P）
gc 1  @0.005s 2%: 0.052+0.46+0.067 ms clock, 0.20+0.074/0.22/0.048+0.26 ms cpu, 3->4->0 MB, 4 MB goal, 0 MB stacks, 0 MB globals, 4 P
gc 30 @0.154s 2%: 0.021+0.77+0.024 ms clock, 0.084+0.055/0.67/0.23+0.097 ms cpu, 3->3->1 MB, 4 MB goal, 0 MB stacks, 0 MB globals, 4 P

# g3（GOGC=off + 每轮 runtime.GC()，1 P）—— 注意 (forced) 与“堆目标 = 天文数字”（GOGC 关了）
gc 100 @0.078s 1%: 0.003+0.41+0 ms clock, 0.003+0/0.006/0.40+0 ms cpu, 1->1->0 MB, 8532210231537 MB goal, 0 MB stacks, 0 MB globals, 1 P (forced)
gc 200 @0.331s 0%: 0.004+0.98+0 ms clock, 0.004+0/0.006/0.97+0 ms cpu, 1->1->1 MB, 8532210231537 MB goal, 0 MB stacks, 0 MB globals, 1 P (forced)

# g2（GOGC=1 @250k）
gc 1  @0.000s 23%: 0.004+0.22+0.002 ms clock, 0.004+0.083/0/0+0.002 ms cpu, 0->0->0 MB, 0 MB goal, 0 MB stacks, 0 MB globals, 1 P
gc 20 @0.025s 17%: 0.004+0.35+0 ms clock, 0.004+0.29/0/0+0 ms cpu, 1->1->0 MB, 1 MB goal, 0 MB stacks, 0 MB globals, 1 P
```

**强制档自己量的“整调用”耗时（stderr，原文 ✓）**：

```
FORCED n=260 total_ms=182.941 max_ms=2.037 avg_ms=0.704
FORCED n=260 total_ms=181.376 max_ms=2.228 avg_ms=0.698
FORCED n=260 total_ms=178.820 max_ms=1.778 avg_ms=0.688
```

### 2.1 这些实测说明了什么

1. **默认档（1 P）**：53 个周期里，**并发标记的墙钟总计 185.6 ms，但 STW 只有 0.346 ms**
   ⇒ 暂停/标记工作量 ≈ **0.19%**；assist 36.8 ms CPU（占墙钟 7.3%，落在官方指南“>5% 表示跑赢 GC”的区间 ✓）。
2. **`GOMAXPROCS=1` 与默认同值** ⇒ **Go 的暂停小不是靠并行** ✓（这一格就是与 extC 最可比的一格 ✓）。
3. **真·4 P**：并发标记墙钟 185.6→**47.3 ms**（≈4×，并行确实加快标记吞吐），
   但 **ΣSTW 反而涨到 2.304 ms（6.7× ✗）、max 17→125 µs** ⇒ **并行买的是吞吐，不是暂停** ✓。
4. **强制档**（`GOGC=off` + 260 次 `runtime.GC()`，1 P）：把**整轮 GC 塞进一个调用**后，
   整调用 **0.70 ms 均值 / 2.04 ms 最大**（与 extC `collect` 的 p50 258.5 µs / max 2.3–3.2 ms **同量级** ✓），
   但**真 STW 部分仍旧只有 1.143 ms/260 ≈ 4.4 µs 均值、22 µs 最大** ✓ ⇒
   **“必须在一个暂停里干完”时 Go 并不比 extC 快** ✓ —— 它快在**不把活放在暂停里** ✓。
   （注意：强制档的 `assist=0`、`idle` 顶上，是因为 `GOGC=off` 关掉了 pacing ⇒ 没有协助、标记 worker 以 idle 身份跑 ✓【推理，与源码 pacing 语义一致】。）
5. **`GOGC=1 @250k`**：33 个周期、STW 总计 0.172 ms、占墙钟 **0.03%** ⇒ 即使**把 GC 频率拉到最高**，
   **STW 依旧亚毫秒级** ✓ —— 这否掉了“Go 是因为 GC 触发得少才停顿小”这个直觉 ✗。
6. **`GOGC=off` 不强制**：**0 个周期**（显式 0 ✓，与 §6.9 的 `NumGC=0` 口径一致 ✓）。

### 2.2 与 extC 的同尺对照（两边都在 core 6 / `@1M` / seed `20261013`）【实测】

| 度量 | Go 默认（1 P） | extC 默认（§6.9） | 比 |
|---|---|---|---|
| GC 周期数/跑 | 53 | 146 次清扫 | Go 更少 |
| **Σ STW / Σ collect** | **0.346 ms** | **63.95 ms** | **extC 是 Go 的 185×** ✗ |
| 单次 p50 | 6 µs | 258.5 µs | 43× |
| 单次 max | 17 µs | 2.3–3.2 ms | ~135–190× |
| 占墙钟 | 0.07% | 11.1% | 159× |
| 墙钟 | 503.6 ms | 538.8 ms | Go −6.5% |
| GC 的 CPU 账 | assist 36.8 + bg 2.7 = **39.5 ms**（7.8% 墙钟） | **63.95 ms 全在暂停里**（11.1%） | — |
| **整轮做一件（强制对照）** | **0.70 ms 均值 / 2.04 max**（含并发段；其中 STW 仅 4.4–22 µs） | 0.26 ms p50 / 2.3–3.2 ms max（**全部是暂停**） | **同量级** ✓ |

⇒ 结论：**两家的标记工作量同量级（甚至 Go 更大：39.5 ms CPU + 185 ms 并发墙钟），
差别只在“这些工作有多少落在暂停里”：Go 0.19%，extC 100%** ✓。

---

## 3. 拆账：Go 的优势哪些能搬、哪些不能

### （甲）**移出暂停**（Go 的主要杠杆）

| 机制 | 量化（本次实测） | 单线程 app 可搬吗 |
|---|---|---|
| 并发标记（后台 worker） | 185.6 ms 墙钟在暂停外（1 P 下是**交错**执行，不是并行 ✓） | **形式可搬、代价致命**：需要写屏障维持快照 ⇒ 见 §4 ✗ |
| mark assist | 36.8 ms CPU 摊到分配路径（7.3% 墙钟） | 与分配耦合 ⇒ **结构上要求“随时能标”** ⇒ 同样要写屏障 ✗ |
| pacing（`GOGC`/`GOMEMLIMIT`） | 让标记在堆目标前完成 ⇒ 标记终止的收尾很短（p50 4–6 µs ✓） | **可搬**：本质是“预算调度”，但前提仍是增量标记 ✗ |
| mark termination 只做收尾 | p50 4–6 µs、max 17–22 µs（**与堆大小基本无关**：g2 堆 1 MB、g0 堆 3 MB、强制档 260 轮里堆在变，STW 仍 4–22 µs ✓） | **这是最值得注意的一条**（见下） |

### （乙）**串行/单线程也能搬**的部分

| 机制 | Go 的实测 | extC 现状 |
|---|---|---|
| 增量清扫（`bgsweep`） | 清扫根本不出现在 `gctrace` 的暂停段里 ✓【文献+实测】 | extC `sweep` 已只占 `collect` 的 1/3；`task-28` 实测**绝对上界 22.4%、可达 ≈ −6.8%** ⇒ **已不是瓶颈** ✓ |
| STW 只做 O(收尾) 而非 O(活集) | 强制档：整轮 0.70 ms，其中 STW 4.4 µs 均值 ⇒ **99.4% 的工作在暂停外** ✓ | extC 的 `collect` 是**整段 STW**（mark 2/3 + sweep 1/3）；且 `pause ≈ 13–14 ns × 活闭包`（`MARK-MODEL.md` §2.4）⇒ **暂停 ∝ 活集** ✗ |
| span/位图结构紧凑、暂停与堆解耦 | 堆从 1 MB 到几十 MB，STW 不随堆涨 ✓ | extC 的暂停随活闭包线性涨（同上）✗ |

---

## 4. 对 extC 的含义（结合本仓库已量过的路线）

**哪些 Go 经验还成立** ✓【推理，基于 §2/§3 的实测 + 本仓库实测】：

1. **“把 O(活集) 的工作搬出暂停”是唯一真正有效的杠杆** —— Go 的暂停 4–22 µs 与它的堆/活集**基本无关**（§3 甲表最后一行），
   而 extC 的暂停**就是** O(活闭包) ✓。这条方向上 Go 与 extC 的差距是**结构性的**，不是常数因子。
2. **但这个杠杆的入场费是写屏障** —— Go 能“边改边标”靠的是赋值路径上的屏障（`mbarrier.go`）；
   本仓库 `task-31` 已经用**只记录不做任何事**的版本量了这条路的**代价下界**：
   `cyclic_pressure @100k` 墙钟 **5.961 → 8.245 ms（+38.3%）** ✗，而“只重扫增量”的**理想收益上限只有 18% 墙钟** ⇒
   **代价是收益的两倍**，且“墙钟不变差 / 总占比不升”两条不可能同时成立 ✗。
3. **暂停与堆解耦（pacing）**单看不贵，但它**只在已有增量标记的前提下才有意义** ⇒ 与 (2) 是同一件事的另一面 ✗。

**哪些已经出局** ✗（都有本仓库实测背书）：

| 路线 | 判决 | 出处 |
|---|---|---|
| 写屏障 + 增量标记 | **出局**：代价下界 +38% > 理想收益 18% ✗ | `MARK-MODEL.md` §13（`task-31`） |
| 切活集 / 分区 / 按引用闭包切 | **出局**：`digraph` 活集 WCC 最大分量 **1000‰**、周转 76% ⇒ 不可切 ✗ | `MARK-MODEL.md` §9（`task-24`） |
| 增量重扫（复用上次标记） | **出局**：`digraph` 复用率低；唯一复用率高的 `cyclic_pressure` 被写屏障代价吃掉 ✗ | 同上 §9/§13 |
| mark 常数因子（更快的遍历） | **出局**：三项候选都够不到 −25%；`drain` 占 mark 99%、每对象 13–14 ns 已是常数下界 ✗ | `MARK-MODEL.md` §10（`task-27`） |
| sweep 相优化 | **出局**：绝对上界 22.4%、可达 −6.8% ⇒ 不为它开实现件 ✗ | `MARK-MODEL.md` §11（`task-28`） |

⇒ **一句话给 extC**：Go 的“停顿小”来自**增量/并发标记**这一条结构性选择，而它恰恰是**唯一一条 extC 已实测为负**
（写屏障代价 > 收益）的路 ✗；Go 剩下的可搬件（增量清扫、暂停与堆解耦）在 extC 上**已经达标或已非瓶颈** ✓
⇒ **单线程 extC 在这条线上没有新的可搬杠杆**，要变只能改**语义/约束**（例如放宽“引用计数语义 + 单线程停顿预算”的前提），
而不是继续调 `collect` 的实现 ✗【推理】。

---

## 5. 未覆盖与残留风险

- **单形状单规模**：全部实测只在 `digraph`（`@1M`，`GOGC=1` 那格 `@250k`）；**不外推**到别的分配画像 ✗。
- **强制档的活集与 extC 的活集不是逐字节可比的量**（Go 的堆量以 MB 打印、只有粗粒度；extC 是“活闭包对象数”）⇒
  §2.2 最后一行“同量级”是**量级判断**，不是同工作量的严格对照 ✗【推理】。
- **`GOMEMLIMIT` 本件没量**（只引文献）；**没量**逃逸分析/分配器/THP 等对 Go 侧的贡献 ✗。
- **`gctrace` 的“并发标记墙钟”不等于 CPU**（1 P 下标记被调度成细片）⇒ 我把 CPU 账（assist/bg/idle）与墙钟分开列了 ✓，
  但两者不能相加当总量 ✗。
- 官方 GC 指南自述描述的是 **Go 1.19**，本机 **1.26** ⇒ 实现细节以本机源码为准 ✓；指南只用于语义（`GOGC` 公式、`GOMEMLIMIT`、assist 阈值）✓。
- **不承诺“没有任何新风险”** ✗。

## 6. 复现命令与原始文件

```bash
# 基准与强制档（强制档 = digraph.go 的 /tmp 副本，逐轮插入 runtime.GC()；src/ 未动 ✗）
cd bench/gc && go build -o /tmp/gopause/digraph_go src/digraph.go
go build -o /tmp/gopause/digraph_forced /tmp/gopause/forced   # 副本，产物与基准逐字节相同 ✓
bash /tmp/gopause/run.sh                                     # 全部格（taskset -c 6，GODEBUG=gctrace=1，3 次）
# 真·4 P 那格：
env GODEBUG=gctrace=1 taskset -c 6-9 /tmp/gopause/digraph_go 1000000 20261013
```

- 原始行：`/tmp/gopause/g{0,1,2,3,4,5,6}_*.err.*`（gctrace）、`/tmp/gopause/matrix.log`（墙钟）、
  `/tmp/gopause/g3_forced_gomax1.err.*` 里的 `FORCED …` 行（整调用耗时）✓
- 强制档源码副本：`/tmp/gopause/forced/digraph_forced.go`（**只在 `/tmp`** ✓）

**引用**：
- [A Guide to the Go Garbage Collector](https://go.dev/doc/gc-guide)（`GOGC` 公式、`GOMEMLIMIT`、assist/后台标记、清扫成本可忽略）
- [Go 1.5 GC announcement](https://go.dev/blog/go15gc)（并发 GC 的原始设计说明；GC 指南“Additional resources”里推荐）
- 本机源码：`runtime/extern.go:114-135`（`gctrace` 格式与三段含义）、`runtime/mgc.go:835/1084/1672/1750/259`、
  `runtime/mgcmark.go:499`、`runtime/mgcsweep.go:272/359`、`runtime/mgcpacer.go:17-30`、`runtime/proc.go:1452-1453`
- 本仓库：[MARK-MODEL.md](../tests/reclaim-perf/MARK-MODEL.md)（§9/§10/§11/§13）、[CHAIN-BLOCKERS.md](../tests/reclaim-perf/CHAIN-BLOCKERS.md)、
  [DIGRAPH.md](DIGRAPH.md) §6.9/§6.10


---

## 7. Go 的“GC 吞吐代价”：`GOGC` 三档（`task-52` · @250k · 同一次会话）

**跑法**：`digraph` @**N=250000** seed=20261013 · `taskset -c 6`（⇒ Go 只看到 **1 P**）· `GODEBUG=gctrace=1` · 每臂 **5 次取中位** · A/B 都不带额外插桩 · 跑前/跑后 **load ≈ 0.24–0.41**（`/tmp/ggo/load.txt`）。**GC CPU%** = `gctrace` 每行开头那个 `#%` 字段（官方口径：**“percentage of time spent in GC since program start”**，`runtime/extern.go:123` ✓）。

| 臂 | 墙钟中位 ms | 周期数 | **ΣSTW ms** | 单次 p50 µs | 单次 max µs | 峰值 RSS MiB | **GC CPU% 中位/最大** | assist+bg CPU ms/跑 |
|---|---|---|---|---|---|---|---|---|
| `GOGC=off` | **48.1** | **0**（显式 0 ✓） | 0 | — | — | **36.4** | **0 / 0** | 0 |
| `GOGC=100`（默认） | 48.8 | 11 | 0.067 | 4.0 | 46.0 | **6.7** | **6% / 8%** | 2.9 |
| `GOGC=200` | 49.4 | 5 | 0.040 | 7.0 | 17.0 | 11.8 | **3% / 3%** | 1.5 |

**两个问题的回答**：
1. **关掉 GC 快不了多少** ✗：`GOGC=off` **48.1 ms** vs 默认 **48.8 ms** ⇒ **−1.4%**（噪声量级）；代价是峰值 RSS **6.7 → 36.4 MiB（5.4×）** ✓。⇒ 在本形状/规模上 **GC 的墙钟代价 ≈ 1%**，与 §6.9 的 STW 占比 **0.20%**、本表 ΣSTW **0.067 ms / 48.8 ms ≈ 0.14%** 一致 ✓。
2. **默认档 GC 拿走的 CPU ≈ 6%** ✓（`gctrace` 中位 **6%**、最大 **8%**）；`GOGC=200` 减半到 **3%** ✓（周期 5 vs 11，与官方“GOGC 翻倍 ⇒ GC CPU 大致减半”的模型一致 ✓）。与我们此前在 **@1M** 量的 assist+bg **39.5 ms / 7.8% 墙钟** **同量级、方向一致** ✓（本表用同一字段直接读出 assist+bg = **2.9 ms/跑** ⇒ 与 6% 自洽 ✓）。⚠️ 两处**规模不同**（本表 @250k、那次 @1M）⇒ 只作**同量级对照**，**不混算** ✗。

**口径注意（如实写 ✓）**：`GOGC=off` 的 RSS 涨到 36.4 MiB（>5×），**没有**为了跑完而暗中加 `GOMEMLIMIT` ✗（三档都无额外限制 ✓）；三档**同一次会话**跑完 ✓（原始行 `/tmp/ggo/GOGC=*.*.err`）。
