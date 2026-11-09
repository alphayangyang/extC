# extC memory-first 模型（E16 草案 v0.1）

> 任务：task-20 / E16。硬约束（用户 2026-11-09 指示，逐字）：「我认为应该尽可能压低内存占用，所以A先不管，可以试试5的那三条」
> 含义：**不得靠抬触发比 r 或抬 EXTC_RC_M_MAX 换墙钟**；只能在不增加内存的前提下减少 GC 工作时间。
> 配套脚本：`docs/analysis/model/roofline.py`（v1.2，含 memory-first 分支，panel (e)(f)），数值表 `roofline-table.md` §F/§G。

## 0. 结论（先看这一节）

| # | 结论 | 数值 | 证据 |
|---|---|---|---|
| C1 | 固定 M=3·live 时，GC 只占墙钟 4.1%–40.0%，四形状差异 10× | promote 40.0%、cycleratio 11.6%、sizeratio 10.4%、chase 4.1%（GC/wall_min） | [实测] rdtsc 拆分 + wall 9 reps |
| C2 | 「sweep 成本→0」**只有 cycleratio 可达**（全死 slab 整块重置）；promote/sizeratio **不可达** | cycleratio −6~9.5%；promote/sizeratio 的 −9.2%/−6.8% 只能读作**假设上界**（Lead 探针：full_dead = 0/879、3/2394） | [实测] sweep/wall 池子 + [实测] slabs/full_dead 可达性（§5.1） |
| C3 | 「mark 成本减半」（指针位图 + dirty-slab）可换 | promote −15.4%、chase −2.1%、sizeratio −1.8%、cycleratio −1.0% 墙钟 | [估算] φ=0.50 |
| C4 | alloc 快路是最大的池子但**未被标定** | 非 GC 占 60.0/89.6/95.9/88.4% 墙钟；alloc−40% 的上界收益 24.0/35.9/38.4/35.4% | [实测池子 + 估算 φ] |
| C5 | 内存已贴地板：理论 M_min=2.69L，实测 r_eff 地板 2.425 ⇒ 保守 M≥3.43L；现行隐式墙 M_max=3L | reclaim.c:952 `return r->live > 0 ? r->live * 3 : 0;` | [源码] + [实测] |
| C6 | 小活集形状的内存由 floor 决定，不由 L 决定 | sizeratio RSS=3.16×floor、cycleratio 2.96×floor；两者 M_max<floor（墙不可达，源码打印 `[mmax]`） | [实测] + [源码] |
| C7 | 「5% GC 税」在 M=3L 下不可达 | 需 c_t≤0.129 ns/B（实测 0.411，−69%）；sweep→0 也只能到税 12.1% | [模型] 不可达证明 §7.1 |
| C8 | 压内存到 r_eff=2 换 29% 堆、付 +5.8% 墙钟；再往下被 roundOk/pressure 地板挡住 | M: 1+κ_M·2.83=3.18L → 2.54L（模型口径，κ_M=U）；λ 0.2508→0.4395 | [模型] + [实测 r_eff] |
| C9 | **floor 是最后一块便宜旋钮，256 KiB 是拐点**（E18 实测） | sizeratio：RSS −35%、墙钟 +15%；cycleratio：−32%、+4%；64 KiB 再省 0.2–0.4 MB 但墙钟 +35~38%；promote（活集 1.8 MB > floor）完全无关 ⇒ 建议暴露为运行期旋钮、默认 1 MiB 不变 | [实测] E18（Lead，FREEZE v4 生成 C 重编 `-DEXTC_RC_FLOOR`，`--pin 6`，3 reps） |

**一句话（按可达性修正）**：promote/sizeratio 的 sweep 段**不是 elision 能拿走的**（死槽分散，full_dead≈0，只能逐槽 free）⇒ 这两个形状的唯一杠杆是 L2（mark）：promote ≤15.4%、sizeratio ≤1.8%；cycleratio 相反，97.8% 死槽落在 651 个 full-dead slab 里 ⇒ L1 可达 −6~9.5%，加 L2 上限 ~10.5%；chase ≤2.1%。内存本身**已无 r 方向空间**（r 方向只能加内存），但小活集形状还有一个**便宜旋钮已实测**：floor 1 MiB→256 KiB 换 sizeratio −35% RSS/+15% 墙钟、cycleratio −32%/+4%（E18，64 KiB 是负拐点）；另有一个**超出本文件范围**的研究项 L5：改分配器记账以消掉逐死槽 free（§5.5）。

## 1. 目的、约束与判据

### 1.1 问题

在 M（堆上界）与 P（单次停顿时限）都固定的前提下，回答三个问题：

1. 当前 extC 在四个形状上的 GC 时间预算花在哪儿（mark / sweep / 非 GC）？
2. 每个可动杠杆最多能换回多少墙钟，证据等级是什么？
3. 哪些目标是**结构性不可达**（给出证明，而不是调参调不出来）？

### 1.2 硬约束

- **M 不得增大**：不抬等效触发比 r，不抬 `EXTC_RC_M_MAX`（隐式默认 3·live，reclaim.c:952）。
- **P 不得增大**：单步预算 `MARK_BATCH_MAX=256`（reclaim.c:1026）/ `SWEEP_BUDGET=8`（reclaim.c:976）不动，压力乘法 `SWEEP_PRESSURE_MUL=4`（reclaim.c:994-996）不动。
- 只讨论内存侧（memory-first）；吞吐侧取舍（抬 r）已由 MEASUREMENTS-V2-AFTER §11.5 证明可行但费内存，本轮明确排除。

### 1.3 判据（可证伪）

- 判据 J1：模型 vs 实测的墙钟分解误差 ≤1.5×（本文件全部通过，见 §3.4）。
- 判据 J2：每个杠杆的收益必须是「实测池子 × 明确系数」，系数若是估算必须标 φ 并列入标定实验（§10）。
- 判据 J3：不可达结论必须是**闭式证明**（§7），不是「我们试过没成功」。

## 2. 口径（构建、档位、证据等级）

### 2.1 构建与源码状态（重要）

| 项 | 值 |
|---|---|
| 测量用构建 | `build-th/extc` md5 `c371e5253a0aa152af968298dfcfb9d1` |
| 该构建行为等价性 | callgrind Ir 与共享 `build-rf`（9fd0dcd8）**逐位一致** |
| FREEZE v4 源码 | reclaim.c `ff42799b…`（3607 行）、codegen.c `cf288544…`（15994 行） |
| 当前工作树源码 | reclaim.c `e353260b…`（**3671 行**，已是含 M_MAX/pressure 的新 patch 态）、codegen.c `cf288544…`（与 v4 一致） |
| HEAD | `85b8871b` |

**引用纪律**：§4/§5 的性能数字锚定 `build-th c371e525`（行为 = FREEZE v4）；源码行号一律按**当前工作树** reclaim.c（3671 行）核对给出，两套口径不得混引。

### 2.2 证据等级

| 标签 | 含义 | 本文件出现处 |
|---|---|---|
| [实测] | 本轮在 `build-th` 上直接测出，方法见 §3 | §4 全部拆分数、§6 RSS、r_eff 地板 |
| [源码] | 逐字引用 `src/back/reclaim.c`（当前工作树），可回归核对 | §6 墙契约、§7 不可达前提 |
| [估算] | 由 [实测] 池子 × 系数 φ 得到，φ 未标定 | §5 L2/L4 的 φ=0.50 / 0.40 |
| [模型] | 闭式推导（附录 A），输入是 [实测] 常量 | §7 不可达证明、§8 λ(r) 表 |

### 2.3 档位

全部数字都是 **T2 档**（默认：`EXTC_RECLAIM=1`、`EXTC_RC_NOCOUNT=1`（不计数，靠清扫）、`EXTC_RC_SWEEP_FREE=0`、`SHARDS=1`），bench 参数为 bench/gc/v2 默认规模（promote 500000 / sizeratio 2000000 / chase 1048576 4000000 / cycleratio 400000 100）。

## 3. 测量方法与误差

### 3.1 四个量具

1. **rdtsc 包裹 markStep/sweepStep**（/tmp 副本，仓库未改）：生成 C 里 `extc_rc_markStep` / `extc_rc_sweepStep` 改名 …Impl，插入 rdtsc 前后包裹的 `_w` 包装 + `__attribute__((destructor))` 报告；TSC 校准 3.071987 GHz。
2. **callgrind Ir 组件**：inline 默认构建（组件归属）+ noinline 构建（alloc body 上界，`-DEXTC_RC_ALLOC_INLINE=0`）。
3. **wall**：`perf_counter` 9 reps 取 min/median，含约 2–4 ms 进程启动。
4. **分配次数**：生成 C 的 alloc 定义后加 `#define extc_rc_alloc(...) (e16_inc(), extc_rc_alloc(__VA_ARGS__))` + destructor 计数。

### 3.2 误差源

- rdtsc 单帧开销 43 周期（≈14 ns，空 noinline 函数标定）：对 markStep/sweepStep（每步数百–数千周期）摊销 <1%，**对单次 alloc 不可用**（body 仅 ~20–40 周期）——这是本文件不直接测 alloc 单价的原因。
- wall 含进程启动 2–4 ms：对 chase（144 ms）<2%，对 cycleratio（13.1 ms）可达 20%；故占比一律用 **wall_min**（分母偏小 ⇒ GC 占比偏高，保守）。
- 交叉校验：本文件 GC/wall = 40.0/10.4/4.1/11.6%，MEASUREMENTS-V2-AFTER §6 用进程内计时给 40.8/10.3/3.6/10.0%，两两相差 ≤0.5pp。
- callgrind 与 wall 不可互换：inline/noinline A/B 在 cycleratio 上出现 +130% 病态（大对象 memset 被拆成调用），故 noinline 的 alloc Ir 只当**上界**用（§5 L4）。

### 3.3 采样

GC 拆分 3 reps（取中位）；wall 9 reps（取 min）；callgrind 1 rep（确定性计数器）；alloc 计数 1 rep（确定性）；PAUSE=1 日志 1 rep（只用于 thr_max/M_max/RSS 与步数，**不用于停顿分布**——日志污染停顿，见 MEASUREMENTS-V2-AFTER §4）。

### 3.4 自检

| 校验 | 值 | 判定 |
|---|---|---|
| GC 拆分之和 vs wall | promote 15.17 vs 37.91（差 22.74 非 GC）；四形状同式闭合 | 通过（恒等式） |
| rdtsc GC 总量 vs `[step] total_us` | promote 15.17 ms vs 16.82 ms（差 9.8%，含 step 记账其它开销） | 通过（≤1.5×） |
| 分配次数 vs MEASUREMENTS-V2-AFTER §11.7 | promote 4,550,001 vs 4.55M | 通过 |
| λ 模型复算（r_eff=2.83） | 0.2465 vs 实测 0.2508（−1.7%） | 通过 |
| Lead 探针交叉：promote 分配次数 | 4,550,001 vs 4,550,001 | 通过 |
| Lead 探针交叉：mark 置位次数 | 1,809,795 / 83 collect = 21.8k/collect，占期间分配 54.8k 的 39.8% | 参考（口径不同，不与 §4 混算） |

## 4. 四形状预算表（实测，M=3·live 固定）

### 4.1 墙钟三分

| 形状 | wall_min (ms) | mark (ms, %) | sweep (ms, %) | 非 GC (ms, %) | GC/wall | allocs | GC ns/alloc | 非 GC ns/alloc |
|---|---|---|---|---|---|---|---|---|
| promote | 37.91 | 11.66 (30.8%) | 3.51 (9.2%) | 22.74 (60.0%) | **40.0%** | 4.55M | 3.33 | 5.00 |
| sizeratio | 21.56 | 0.78 (3.6%) | 1.46 (6.8%) | 19.32 (89.6%) | **10.4%** | 2.00M | 1.12 | 9.66 |
| chase | 144.16 | 5.91 (4.1%) | 0.01 (0.0%) | 138.24 (95.9%) | **4.1%** | 1.11M | 5.34 | 124.4 |
| cycleratio | 13.11 | 0.27 (2.1%) | 1.25 (9.5%) | 11.59 (88.4%) | **11.6%** | 1.82M | 0.83 | 6.36 |

读数：

1. **GC/wall 的形状差异 10×**（promote 40.0% vs chase 4.1%）：不是 GC 变快了，而是 chase 的 mutator 每分配要做 124 ns 的随机访存。
2. **mark 与 sweep 的配比随形状反转**：promote mark:sweep≈3.3:1；sizeratio/cycleratio 反过来 1:1.9 和 1:4.6（活集小、cap 大 ⇒ 清扫相对贵）；chase 几乎纯 mark（sweep 0.2%）。
3. **GC ns/alloc 只有 0.83–5.34 ns**，而 MEASUREMENTS-V2-AFTER §11.7 的「每分配固定成本 5.36 ns」是**非 GC** 侧——两笔钱在同一量级，只砍 GC 最多拿回 C1 那 4%–40%。
4. **sweep 段的大小不等于它的可削减量**（本轮被实测修正）：promote 的 9.2% 里几乎全是**分散死槽的逐个 free**（每死槽 ≈2.0 ns），「跳过全活 slab」只能省字循环，而字循环只有 36k 字 ⇒ 收益 <0.5%。可达性见 §5.1。

### 4.2 GC→0 的墙钟上限（结构性天花板）

| 形状 | GC→0 后 wall (ms) | 上限降幅 | 说明 |
|---|---|---|---|
| promote | 22.74 | **−40.0%** | 仍有 4.55M×5.00 ns 的 alloc 快路成本 |
| sizeratio | 19.32 | −10.4% | 剩 19.32 ms 是 2M 次近 10 ns/次的分配 + 160 MB 置零 |
| chase | 138.24 | −4.1% | 剩 138 ms 是链式随机访存（a_all 0.31 GB/s） |
| cycleratio | 11.59 | −11.6% | 剩 11.59 ms = 1.82M×6.36 ns |

结论：**chase 不在本文件的可优化范围**（GC 侧最多 −4.1%，且 sweep/mark 已接近 0/4%）；它的瓶颈是 mutator 访存（MEASUREMENTS-V2-AFTER §11.7③）。

### 4.3 callgrind Ir 交叉核对（inline 默认构建）

| 形状 | 总 Ir | main | markStep | sweepStep | 其它显著项 |
|---|---|---|---|---|---|
| promote | 124.68M | 90.55M (72.6%) | 19.40M (15.6%) | 9.54M (7.7%) | shadeSlow 1.27M |
| sizeratio | 23.12M | 15.02M | 1.75M | 2.21M | memset 2.70M (11.7%)、shouldSweep 0.73M、maybeCollect 0.71M |
| chase | 22.21M | 14.44M | 6.31M (28.4%) | ~0 | shadeSlow 0.42M |
| cycleratio | 137.27M | 117.65M (85.7%) | 2.79M | 9.79M | pick 3.60M |

Ir 视角与墙钟视角一致：sweepStep 的 Ir 份额 7.7/9.6/0.0/7.1% 与墙钟份额 9.2/6.8/0.0/9.5% 同序（chase 的 markStep 28.4% vs 墙钟 4.1% 差异大，因为 chase 的 Ir 被 cache miss 拖长，见 §3.2 的告警）。

## 5. 杠杆族：每条能换多少墙钟

记号：池子 = 该段实测墙钟（§4.1）；φ = 该段可归因削减比例；收益 ΔT/T = φ × 池子/wall。

| 杠杆 | 机制 | φ | 证据 | promote | sizeratio | chase | cycleratio |
|---|---|---|---|---|---|---|---|
| **L1a** 字循环省略（跳过全活 slab，需 dirty 位） | 只省 `sweepWord` 的读-与-写回，成本 ∝ cap words | 1.00 | [实测] full_dead | **≈0（不可达：full_dead=0）** | ≈0（3/2394） | 0 | 不适用（见 L1b） |
| **L1b** 全死 slab 整块重置（occ→0、bump=0、freelist=−1、记账） | 把 651 个 full-dead slab 的逐槽 free 换成 O(1) 重置 | 0.978（可达死槽占比） | [实测] 面积探针 + [估算] 判定成本 | 0（不可达） | ≈0（2,171/2.0M 槽=0.11%） | 0 | **−6~9.5%**（上限 −9.5%） |
| **L2** mark 减半（指针位图 + dirty-slab） | 每类型指针位图，word-AND 跳过非指针槽；只逐字扫有分配的 slab | 0.50 | [估算] | **−15.4%** | −1.8% | −2.1% | −1.0% |
| **L3** floor 下探（小活集，已暴露前是编译期宏） | floor 1 MiB→256 KiB（E18 实测拐点） | 省内存 | **[实测] E18** | 0（活集 1.8 MB > floor） | **RSS −35%，墙钟 +15%** | 0 | **RSS −32%，墙钟 +4%** |
| **L4** alloc 快路（含屏障检查、置零、元数据） | 见 MEASUREMENTS-V2-AFTER §11.7① | 0.40 | [上界/估算] | −24.0%（上界） | −35.9%（上界） | −38.4%（上界） | −35.4%（上界） |
| **L5** 逐死槽 free 的元数据改造（按 slab 记账 / 延迟 freelist 重建，让分散死槽变 O(slab)） | 见 §5.5 | 未标定 | [未实现·研究项] | 研究项（上限 = 整段 sweep 9.2%） | 研究项（6.8%） | — | — |

### 5.1 L1：sweep 段能不能变便宜——必须分成两个机制（本轮被实测修正）

现状：`extc_rc_sweepWord` 对每个**字**都做 `dead = s->occ[w] & ~s->mark[w]`，即使 `dead==0` 也要清 `s->mark[w]=0`（当前工作树 reclaim.c:3021-3025 逐字）：

```c
uint64_t dead = s->occ[w] & ~s->mark[w];
if (dead) {   /* word-wise bulk release (see extc_rc_freeSlotWord): per slot only the head insertion, the rest once per word */
    r->swFreed += (int64_t)extc_rc_freeSlotWord(s, (size_t)w, dead);
}
s->mark[w] = 0;
```

而 `extc_rc_freeSlotWord`（reclaim.c:1499-1545）**已经**把「占用位、rsbit、字节记账、freedSlots、size-class push、classPush」**按字**做了一次，逐槽只剩 freelist 头插：

```c
size_t i = base + b;
s->tid[i] = (uint16_t)(s->freelist + 1);
s->freelist = (int64_t)i;
```

（注释原文：`Word-wise bulk free during sweep: ... Of the per-slot steps only "head insertion" differs per slot (the link value differs); occupancy bit, registration bit, byte accounting, freedSlots and the size-class push happen once per word`）。

于是 sweep 段有两个**互不替代**的成本项：

- **L1a 字循环**：`dead` 计算 + `mark[w]=0`，成本 ∝ cap words（与死活无关）⇒ 只有「跳过全活 slab」（dirty 位）能省这一项。
- **L1b 逐死槽头插**：每个死槽两次依赖写（`tid[i]`、`freelist`），随机访问 `tid` 数组 ⇒ 只有「**全死 slab 整块重置**」（`occ[w]=0`、`mark[w]=0`、`bump=0`、`freelist=−1`、记账一次）能省这一项。

#### 可达性：Lead 的 slab 面积探针（FREEZE v4 生成 C，`/tmp/mf/base/*.c`）

量具：`sweepBegin` 快照每 slab 的 `popcount(occ)`，`sweepWord` 完成该 slab 时重算 `popcount(occ)`，差 = 该轮该死槽数；累计 stderr 原文：

| 形状 | slabs | full_dead | full_dead_slots | full_live | dead_slots | live_slots | words |
|---|---|---|---|---|---|---|---|
| promote 200k | 879 | **0** | 0 | 74 | 1,748,760 | 481,901 | 36,117 |
| sizeratio 2M | 2,394 | **3** | 2,171 | 171 | 1,995,915 | 195,544 | 44,289 |
| cycleratio | 756 | **651** | 1,739,978 | 63 | 1,778,862 | 54,651 | 30,870 |

读数（**推翻本文件 v0.1 的 L1 建议**）：

1. **promote 上 L1a 收益 ≈ 0**：全部 word 只有 36,117 个（≈0.2M Ir），而 sweep 3.51 ms 的钱在 1.75M 个死槽的逐个头插上；`full_dead=0` 意味着没有任何 slab 能整块重置 ⇒ **promote 的 sweep 3.51 ms 不是 elision 能拿走的**。sizeratio 同理（`full_dead=3` 次、2,171 槽，占其 2.0M 死槽的 0.11%）。
2. **cycleratio 相反**：1,739,978 / 1,778,862 = **97.8%** 的死槽落在 651 个 full-dead slab 里 ⇒ L1b 是真机制；理论上限仍是 sweep/wall = −9.5%，扣掉判定成本后预期 **−6~9%**。
3. **每死槽价格 = 3.51 ms / 1,748,760 ≈ 2.0 ns/死槽**（promote），远高于按扫槽折算的 τ_s=0.458 ns ⇒ 钱在 `freeSlotWord` 的逐槽头插与随机 `tid[]` 写，不在位图读改写。要打掉 promote 的 sweep，只剩 **L5**（§5.5，改分配器记账），那是研究项。

**本文件的 L1 在 promote/sizeratio 上不可达，依据＝Lead 的 slabs/full_dead 探针，数据见 `docs/analysis/P1-7-SWEEP-ELIDE.md`（runtime-facts 落盘中）。** 因此 §0 C2、§4、§7.2 里的「sweep→0 = −9.2%」对 promote/sizeratio 只能读作**假设上界**，不得作为投资依据；promote 的唯一杠杆是 L2（mark）。

口径提醒：Lead 探针用 promote 200k（§2.3 的 align 参数族），本文件 §4 的 sweep 3.51 ms 用 promote 500k（bench 参数）；两者不混算，此处只用探针判断**机制可达性**，不用它替换收益数值。

### 5.2 L2：mark 减半的机制（指针位图）

现状 markStep 对每个活对象逐槽读 `ttab` 类型号、按类型走指针字段（reclaim.c:2193 附近 `r->markedBytes += (int64_t)s->ss;` 是记账，扫描主体在 MARK_SCAN 宏）。L2 给每个类型预编译一张「指针字位图」，扫描时 `for w: ptrs = desc->pbits[w]; if (ptr && ptrs) …`，非指针槽直接跳过；再配合 dirty-slab（只 mark 有分配的 slab 行）可把子对象扫描量再降。

- φ=0.50 是**估算**（理由：digraph 1M 每对象 2.76 个指针、载荷 42.5 B ≈ 5.3 个 8B 字 ⇒ 指针密度 ~52%，跳过非指针字的上界收益约 48%）。
- 收益：promote −15.4%（因为 promote 的 mark 占墙钟 30.8%），chase −2.1%，其它 ~1–2%。
- 代价：需要类型描述符多一行字位图（编译期生成，运行时只读）；对 42.5B 的小对象，位图精度受「对象内偏移对齐」限制，需 E17' 实测。

### 5.3 L3：floor 是小活集形状唯一的内存杠杆（E18 已实测）

机制：触发线 `thr = max(2·live, floor)·scale`（reclaim.c:3185-3218）；活集远小于 floor 时 `thr ≈ floor` ⇒ 堆峰值与 live 无关、只由 floor 定。这就是 §6.2 里 sizeratio/cycleratio 的 `M_max < floor`（源码打 `[mmax]`、退回 floor）与 RSS≈3×floor 的来源。

Lead 的 **E18**（FREEZE v4 生成 C 上重编 `-DEXTC_RC_FLOOR='((int64_t)1<<18)'` / `1<<16`，`-O2 -std=c11 -fwrapv`，`/tmp/v2/v2run --pin 6`，每变体 3 reps）：

| 形状 | floor 1 MiB（基线） | floor 256 KiB | floor 64 KiB |
|---|---|---|---|
| sizeratio 2M | 3,244–3,372 kB / 21.9–23.9 ms | 2,088–2,092 kB / 26.4–27.2 ms（**RSS −35%，墙钟 +15%**） | 1,896–1,900 kB / 30.1–31.5 ms（−42%，+35%） |
| cycleratio | 3,036 kB / 13.0–14.4 ms | 2,076 kB / 13.7 ms（**RSS −32%，墙钟 +4%**） | 1,692 kB / 18.6–19.1 ms（−44%，+38%） |
| promote 500k | 6,492 kB / 41.6–43.1 ms | 6,492 kB（**无变化**） | 6,492 kB（无变化） |

读数：

1. floor 只对「活集 ≪ floor」的小堆形状有效；promote 活集 ~1.8 MB > floor ⇒ 完全无关（也说明 §5.1 的 L1 与 floor 无关）。
2. **256 KiB 是拐点**：再降到 64 KiB 只多省 0.2–0.4 MB，墙钟却多付 20 个百分点。
3. 方向与 L1/L2 相反（省内存、费墙钟），且它是「**已在源码里但没被暴露**」的旋钮（`#ifndef EXTC_RC_FLOOR` / `#define EXTC_RC_FLOOR ((int64_t)1 << 20)`，reclaim.c:140-141）⇒ 建议做成**运行期开关**（`getenv` 读取），默认 1 MiB 不变，小堆形状按 R-MF3 打开。

### 5.4 L4：alloc 快路的收益是上界，不是承诺

非 GC 池子 60.0–95.9% 墙钟，但池子里混着形状自身工作（chase 的链式访存 124 ns/alloc、sizeratio 的 160 MB 置零）。可归因份额 φ=0.40 来自 noinline callgrind 的 alloc body Ir 占比（43.0/51.0/23.0/39.3%）——而 noinline 会放大（call/ret + 常量折叠丢失，cycleratio 的 A/B 出现 +130% 病态），所以这只能是**上界**。要把它变成承诺，必须做 E17（§10）。

### 5.5 L5（研究项）：消掉逐死槽头插

promote/sizeratio 的 sweep 段 = 分散死槽的逐槽头插（§5.1），**在现有元数据下不可消除**。要消掉必须改记账方式，候选（均为**未实现、未验证**）：

- **延迟 freelist 重建**：sweep 只清 `occ` 位图，freelist 在分配路径按需从位图取槽 ⇒ 把逐槽头插搬到分配侧（但 promote 有 4.55M 次分配，每次多一次位图查找，可能更贵）。
- **按 slab 记账**：`tid[i]` 链改为「按字的空槽位图 + ctz」，slab 只记 `deadSlots` 计数 ⇒ 需改 `alloc` 取槽路径（与 L4 同一块代码）。
- **判据**：必须先做 E17（alloc 单价），否则可能「省了 sweep、加了更贵的 alloc」。上限 = 整段 sweep（promote 9.2%、sizeratio 6.8%），实现风险高于 L1b/L2。

### 5.6 负结果：chase + `MADV_HUGEPAGE` 无效（不要重复投入）

Lead 在 chase 产物 `slabNew` 的 `aligned_alloc` 之后加 `madvise(s, 1<<20, MADV_HUGEPAGE)`（`-D_GNU_SOURCE` 编译），5 reps 交替：base 中位 **171.4 ms** / RSS 73,692 kB；THP 中位 **171.0 ms** / 同 RSS ⇒ **无差异**（THP 模式为 madvise，khugepaged 未及时合并，或 chase 的瓶颈不在 TLB）。故 THP 不列为 chase 的候选手段；chase 仍按 §7.3 走 mutator 访存（局部性 / alloc 快路），不在 GC 侧。

## 6. 内存模型：M 是被谁决定的

### 6.1 源码契约（逐字，当前工作树 reclaim.c 3671 行）

reclaim.c:903-905：

> Memory-wall contract (EXTC_RC_M_MAX, bytes; unset = implicit 3 x live): the trigger threshold is capped so that live + garbage stays near the wall, and near the wall the per-step budget grows instead of the trigger frequency.

reclaim.c:909-913：

> an unbounded trigger threshold keeps the sweep count a small constant as the live set grows 100x, but pays with unbounded resident memory -- live + garbage is not bounded. Capping the threshold trades that for a logarithmic sweep count ... the price of a bounded peak (memory is exchanged for work).

reclaim.c:946-954：

```c
static inline int64_t extc_rc_mmaxEff(const ExtcRcRegion *r) {
    extc_rc_mmaxArm();
    if (extc_rc_mmax > 0) return extc_rc_mmax;   /* skip:m-max-explicit */
    return r->live > 0 ? r->live * 3 : 0;   /* implicit 3 x live: 0 while unswept, i.e. no cap before the first round */
}
```

reclaim.c:3196-3214：

```c
/* Memory-wall contract: live + garbage <= M_max, so the trigger line is also capped at M_max - 64*L/b (b = 256 => L/4).
 * An implicit (unset) M_max is 3*L; a wall below floor cannot be met -- use floor and report once, never trap. */
int64_t _wall = extc_rc_mmaxEff(r);
if (_wall > 0) {
    int64_t _cap = _wall - r->live / 4;
    if (_cap < r->floor) { _cap = r->floor; ... fprintf(stderr, "[mmax] memory wall unreachable: ..."); }
    if (thr > _cap) thr = _cap;
}
```

⇒ 三条硬事实：

1. **隐式墙 = 3·live**（不是常数 MB）；墙 < floor 时回落到 floor 并只打印一次 `[mmax]`，不 trap（reclaim.c:3210）。
2. 触发线 `thr <= M_max − live/4`（b=256 时 64L/b = L/4）：给「在飞垃圾」预留 1/4 活集。
3. 近墙时**加每步工作量**（`EXTC_RC_SWEEP_PRESSURE_MUL=4`，reclaim.c:988-996）而不是加触发频率——源码注释明确写了「frequency cannot bound memory」。

### 6.2 实测占用（PAUSE=1 的 `[work]` 行 + `/usr/bin/time -v`）

| 形状 | thr_max (B) | M_max (B) | M_max/L | RSS (kB) | RSS/M_max | RSS/floor |
|---|---|---|---|---|---|---|
| promote | 3,418,672 | 5,128,008 | 3.00 | 6,492 | **1.30×** | 6.34× |
| sizeratio | 1,048,576 | 344,064 | 0.30（墙不可达） | 3,240 | n/a | **3.16×** |
| chase | 63,438,892 | 69,206,064 | 2.71 | 73,692 | **1.09×** | 71.96× |
| cycleratio | 1,048,576 | 94,728 | 0.09（墙不可达） | 3,036 | n/a | **2.96×** |

读法：

- **大活集形状**（promote L=1.71 MB、chase L=23.07 MB）：RSS/M_max = 1.09–1.30，超出部分 = slab 取整（1 MiB 粒度）+ 在飞垃圾 + 分配器开销；实测 promote 恰好落在 `F_slab=1.30` 上（CALIBRATION §3）。
- **小活集形状**（sizeratio L=114,688 B、cycleratio L=31,576 B）：M_max 甚至小于 floor（源码打印 `[mmax] memory wall unreachable`），实际 thr=floor=1 MiB ⇒ **RSS 与 L 无关**，约 3×floor。压内存只能压 floor/slab（L3）。

### 6.3 内存在 r 方向还剩多少空间

keep-up 的理论下界（附录 A.5）：r ≥ r_min = 1 + g·K/b = 1 + 2.76×64/256 = **1.69** ⇒ M ≥ 2.69L。

但实测 r_eff（=H/L_prev）的下限是 **2.425**（CALIBRATION §3，digraph 1M 全部轮次的最小值），说明在 roundOk 信用 + pressure 机制下实际达到的最小触发比高出理论 1.43×。于是：

| 口径 | r | M/L | 证据 |
|---|---|---|---|
| 理论下界 | 1.69 | 2.69 | [模型]
| 实测地板 | 2.425 | 3.43 | [实测] min r_eff
| 实测 p50 | 2.83 | 3.83（模型）；实测 peak 2.76–3.00 | [实测]
| 现行隐式墙 | — | **3.00** | [源码] 3×live |

⇒ 现行配置已经在 2.69–3.43L 的**下界带内**：想再省内存，只能 (a) 把 r_eff 压到 2.0–2.4（λ 从 0.2465 涨到 0.31–0.44，税 7.8%→9.5–12.8%，墙钟 +1.9%…+5.8%，且需要重做 roundOk 信用判据，E19），或 (b) 动小活集形状的 floor（L3）。

## 7. 不可达证明（闭式）

### 7.1 定理：M=3L 时「GC 税 ≤5%」不可达

设 a_mut = 0.335 GB/s（digraph 1M 扣 GC 后，CALIBRATION §5），则

  tax = λ/(λ + 1/a_mut) ≤ 0.05  ⇔  λ ≤ 0.05·(1/a_mut)/0.95 = 0.05×2.985/0.95 = **0.157 ns/B**。

而 M=3L 对应 r=2（thr=2·live），此时 λ = (c_t + c_s·F_slab·r)/(r−1) = c_t + 2c_sF_slab = 0.4114 + 0.0281 = **0.4395 ns/B**。

因 c_t=0.4114 本身 > 0.157，**即使 c_s→0（sweep 完全免费）也仍有 0.4114 > 0.157** ⇒ 证毕。需要的 c_t 上限：r=2 时 0.129 ns/B（实测的 31%，即 −69%）；r=2.83 时 0.247 ns/B（−40%）。

推论：L1b（**仅 full-dead 占比高的形状可达**，如 cycleratio）+ L2（mark−50%）一起上，最多把 c_t 有效值降到 0.206，r=2.83 下 λ=0.206/1.83=0.113 + sweep 0 = 0.113 < 0.157 ⇒ **5% 税在 r=2.83 下靠 L1b+L2 是可达的**（税 3.65%，但只对 cycleratio 类形状成立；promote/sizeratio 拿不到 L1b 那部分，λ=0.206/1.83+0.0217=0.134 ⇒ 税 4.3%，M=3L(r=2) 下仍不可达（λ=0.206 > 0.157））。这是「memory-first 与税目标冲突」的定量表述。

### 7.2 定理：L1 的墙钟收益上限 = sweep/wall，但**可达性**由 full_dead 决定

T = T_mark + T_sweep + T_rest，三项互不耦合（SWEEP_BUDGET 64/256 的 A/B 显示 GC 总时间不变，说明 sweep 是真工作量而非每片开销，MEASUREMENTS-V2-AFTER §11.2）。若能把 T_sweep **整体**消掉 ⇒ ΔT/T = T_sweep/T（**上界**：promote 9.2%、cycleratio 9.5%、sizeratio 6.8%、chase 0.0%）。

但「整体消掉」要求死槽**成块**。由 §5.1 的分解与 Lead 探针：

  T_sweep = T_words(∝ cap words) + T_headins(∝ dead slots)

- L1a 只能删 T_words：promote 的 words=36,117（≈0.2M Ir）⇒ 收益 <0.5% 墙钟；
- L1b 只能删「落在 full-dead slab 里的 T_headins」：promote full_dead=0 ⇒ 0；sizeratio 2,171/1,995,915 = 0.11% ⇒ ≈0；cycleratio 1,739,978/1,778,862 = 97.8% ⇒ **可达 0.978 × 9.5% ≈ 9.3%（扣判定成本 6~9%）**。

故本文件对四形状的 **L1 可达收益**取（promote 0、sizeratio 0、chase 0、cycleratio 6–9.3%），其余一律按上界标注。

### 7.3 推论：chase 类形状不在 GC 侧

GC/wall = 4.1%，且其中 99.8% 是 mark；即便 GC→0，wall 也只能 144.16→138.24 ms（−4.1%）。对比 cpp careful 128.7 ms（MEASUREMENTS-V2-AFTER §11.7③），差距 24% 全部在 mutator 访存。

### 7.4 推论：alloc 快路的收益只能给上界

非 GC 池 60.0–95.9% 含形状自身工作；noinline callgrind 给的 alloc body Ir 占比（43.0/51.0/23.0/39.3%）被 noinline 放大（§3.2 病态 A/B），只能当上界。故 §5 的 L4 列必须读作「≤24.0% 等」，不能写进承诺或门禁。

## 8. 与 roofline 模型的对接（固定 M 后只剩纵向移动）

GC-ROOFLINE.md 的平面是 (λ, a)：屋顶 a ≤ ρ/λ，点越靠左下越好。**M 固定 ⇒ r 被锁死 ⇒ 横坐标 λ 也被锁死**；memory-first 的杠杆只能让点**竖直上移**（同 r 下 λ 变小），而不是沿屋顶下滑（那需要增大 r）。

λ 分解（r=2.83，实测 c_t/c_s/F_slab）：

| 项 | 公式 | 值 (ns/B) | 占比 | 被哪个杠杆打 |
|---|---|---|---|---|
| λ_mark | c_t/(r−1) | 0.2248 | 91.2% | L2（×0.5）、L4（部分） |
| λ_sweep | c_s·F_slab·r/(r−1) | 0.0217 | 8.8% | L1（→0） |
| 合计 | | 0.2465（实测 0.2508，−1.7%） | 100% | |

固定 M 的口径下，r 与 λ/税/墙钟的关系（digraph 1M，a_mut=0.335）：

| r_eff | M/L（保守 1+r） | λ (ns/B) | GC 税 | 相对现行墙钟 | 可行性 |
|---|---|---|---|---|---|
| 2.00 | 3.00 | 0.4395 | 12.8% | +5.8% | 需重做 roundOk（E19），实测未观测到 |
| 2.425 | 3.43 | 0.3126 | 9.5% | +2.0% | 实测地板 |
| 2.83（现行） | 3.83 | 0.2465 | 7.8% | 0 | 已达成 |
| 3.50 | 4.50 | 0.1842 | 5.8% | −1.9% | 费内存（本轮禁止） |
| 4.00 | 5.00 | 0.1559 | 5.0% | −2.8% | 费内存（本轮禁止） |

⇒ 结论：在 (λ,a) 平面上，**M 固定时点的唯一可动方向是 λ↓（竖直上移），且幅度受 §5 的 L1/L2 上限约束（λ→0.113 是现行 r 下的最好情况，−54%）**。想再上移就得降 r（税涨），想右移就得升 r（内存涨），二者都被 §6.3 的带子夹住。

## 9. 决策准则（memory-first，8 条）

| # | 触发条件（可测） | 动作 | 预期收益 | 判据 |
|---|---|---|---|---|
| R-MF1 | 先看 `full_dead/slabs`（§5.1 探针） | ≥50% ⇒ 做 L1b（全死 slab 批量重置）；≈0 ⇒ **不要做 L1**，转 L2/L5 | cycleratio −6~9.3%；promote/sizeratio 0（不可达） | cycleratio 的 sweep 段降 ≥50% 且 RSS 不变 |
| R-MF2 | mark/wall > 3% | 做 L2（指针位图 + dirty slab）——**promote/sizeratio 的唯一杠杆** | promote −15.4%（上界）、sizeratio −1.8%、cycleratio −1.0%、chase −2.1% | 分解项 T_mark 降 ≥30%（φ 实测值，见 E17'） |
| R-MF3 | M_max < floor（源码打 `[mmax]`）或活集 ≪ floor | 不碰 r；把 floor 暴露为运行期旋钮并设 **256 KiB**（E18 拐点） | sizeratio RSS −35%/墙钟 +15%；cycleratio −32%/+4% | RSS 降 ≥30% 且墙钟涨幅 ≤15% |
| R-MF4 | 目标税 ≤5% | 先查 §7.1：r=2 需 c_t ≤0.129（−69%），r=2.83 需 ≤0.247（−40%） | L1b+L2 只在 r=2.83 且仅 cycleratio 类形状勉强够（3.65%）；promote/sizeratio 只能到 4.3%；M=3L 不够 | 达不到就改口径为「税 ≤8%」 |
| R-MF5 | 想把墙钟降 >10% 且允许费内存 | 抬 r（MEASUREMENTS-V2-AFTER §11.5：r=4 −17%、+3.5 MB） | −17% | 本文件禁止路径，需用户明确许可 |
| R-MF6 | 形状是 chase 类（a_all < 0.5 GB/s、GC/wall <5%） | 不在 GC 侧投资 | GC→0 仅 −4.1% | 预算转 alloc 快路/访问局部性 |
| R-MF7 | 外部给定 EXTC_RC_M_MAX | 用 §8 表反查 λ/税，再定 floor；若墙 < floor 会静默回落 | 避免配额浪费 | `[mmax]` 不出现 |
| R-MF8 | 任何 L1–L4 落地 | A/B 必须看**分解项**（rdtsc 段）而非总墙钟 | 否则 2–4 ms 启动噪声淹没信号 | wall_min 9 reps + 分段 3 reps |

补一条排序准则（**按可达性，不按段大小**）：**先探针再定杠杆**——`full_dead/slabs ≥50%` 才做 L1b；否则先做 L2（mark，四形状里三个有正收益）；**L3 与 L1/L2 分开评估**（前者省内存、后者省墙钟，方向相反）；**L5 必须先标定 E17**（否则可能用 sweep 换更贵的 alloc）；**L4 最后做且只能当上界**。

## 10. 标定实验清单（E17–E22）

| 编号 | 谁测什么 | 怎么测 | 判据 | 优先级 |
|---|---|---|---|---|
| **E17** | alloc 快路单价（含屏障检查、置零、元数据） | 生成 C 的 alloc 体里每 64 次分配插一次 rdtsc（AM 计数）；另做纯分配微基准：工作集 1/2/4 MiB × 对象 8/24/64 B × 1e6/1e7 次 | 15 reps min 的方差 <3%；得到 ns/alloc 与工作集曲线 | **P0** |
| **E17'** | 指针位图真实跳过量（φ_mark） | markStep 里计数「被 word-AND 跳过的非指针字数 / 总字数」 | 实测 φ 与 0.50 差 <20% | **P0** |
| **E18** ✅ 已完成（Lead） | floor 1 MiB→256/64 KiB 的 RSS/墙钟交换 | FREEZE v4 生成 C 重编 `-DEXTC_RC_FLOOR='((int64_t)1<<18)'` / `1<<16`，`/tmp/v2/v2run --pin 6`，每变体 3 reps | sizeratio −35% RSS/+15% 墙钟（256K）、−42%/+35%（64K）；cycleratio −32%/+4%、−44%/+38%；promote 无变化 ⇒ **256 KiB 是拐点，做成运行期旋钮** | ✅ 达成 |
| **E19** | roundOk/pressure 地板 | 显式 `EXTC_RC_M_MAX` ∈ {2.69L, 3L, 3.43L, 4L}，测 r_eff 分布、`thr`、`[mmax]` 次数、墙钟 | 找出 r_eff 是否随 M_max 线性降、最小可行 M | P1 |
| **E20** | κ_M 口径复核 | 用 calprof 的 (peak, L_peak, H_trig) 三元组重算 κ_M（§12#2 的矛盾） | 三个数据集口径一致 | P2 |
| **E21** | **L1b** 原型（全死 slab 批量重置） | `sweepBegin` 后对 `popcount(occ)==0` 的 slab 直接 `occ/mark` 清零 + `bump=0` + `freelist=-1` + 记账一次，跳过逐槽头插 | cycleratio sweep 段降 ≥50%、RSS 不变、`tests/reclaim*` 全绿 | **P0** |
| **E22** | L5 可行性（改 freelist 记账） | 先用 E17 得到分配侧单价，再估「延迟重建」在 promote 4.55M 次分配上的额外成本 | 新增分配成本 < 省下的 3.51 ms | P2 |

## 11. 门禁草案（可落成 gate）

| 门禁 | 内容 | 当前值 | 判定 |
|---|---|---|---|
| G-MF1 墙钟 | 固定 M=3L 下每形状 wall 降幅 ≥10% | 可达杠杆：promote L2 15.4%（过）、cycleratio L1b 6~9.3% + L2 1.0%（临界）、sizeratio L2 1.8%（不可过）、chase 上限 4.1%（豁免） | 只有 promote 稳过；cycleratio 需 L1b 取上界；sizeratio/chase 需豁免或改判据 |
| G-MF2 λ | r_eff=2.83 下 λ ≤0.35 ns/B；M=3L(r=2) 下 ≤0.44 | 0.2508 已过；0.4395 已过但税 12.8% | 通过（阈值需按档位复核） |
| G-MF3 RSS/M_max | ≤1.35× | promote 1.30（贴线）、chase 1.09 | 通过（promote 无余量） |
| G-MF4 小堆绝对 RSS | sizeratio/cycleratio 峰值 RSS ≤2.2 MB（改成**绝对 MB**，因为 floor 下探后 RSS/floor 自动变大到 8×） | floor=1 MiB：3.24/3.04 MB；floor=256 KiB：2.09/2.08 MB（E18） | 按绝对 MB 判定则**可通过**（需先暴露 floor 旋钮） |
| G-MF5 停顿 | p99 ≤5 µs、max ≤50 µs、>1 ms = 0 | 沿用 GC-ROOFLINE G1（b=256 时 P=4.71 µs、p99 中位 4.8 µs） | 通过（余量 1.06×） |
| G-MF6 sweep 占比 | sweep/wall ≤5%；或「full-dead slab 覆盖 ≥90% 死槽」的等效判据 | promote 9.2（L1 不可达 ⇒ 现有元数据下**无法通过**）、cycleratio 9.5（L1b 可达）、sizeratio 6.8、chase 0.0 | **两个未过**：cycleratio 靠 L1b；promote 需 L5 改记账，建议并列等效判据 |

## 12. 存疑与未验证（必须带进结论）

1. **wall_min 含进程启动 2–4 ms**：cycleratio 的占比误差可达 20%（13.11 ms 里可能 2–3 ms 是准备）。故 §4 的百分比只用于形状间比较，不用于绝对承诺。
2. **κ_M 一律标 U（未标定，且互相矛盾）**：CALIBRATION §3.6 给 κ_M=0.772（定义 (peak−L)/H），但用 digraph 1M 的 (6.44 MB, 2.33 MB, 2.13 MB) 复算得 1.93；源码隐式墙 3·live 与 peak 2.76·L_peak 也不自洽。本文件 §6 只用**源码契约（3·live）+ 实测 RSS**，任何地方不得把 0.772 当已标定量引用（MEMORY-MODEL §18.4 同样改用 RSS 比）。E20 复核。
3. **φ_mark=0.50、φ_alloc=0.40 是估算/上界**：L2/L4 的全部收益数字都带这个不确定性；未经 E17/E17' 不能进承诺。
4. **L1 可达性依赖 Lead 的 slab 面积探针**（§5.1 表，FREEZE v4 生成 C，promote 200k 口径）；探针脚本尚在 runtime-facts 落盘（`docs/analysis/P1-7-SWEEP-ELIDE.md`）⇒ 表里的具体数字本文件无法独立重跑。但机制判断（`full_dead≈0` ⇒ 没有 slab 可整块重置）不依赖具体数值。
5. **promote sweep 3.51 ms 的内部拆分未测**：目前只有聚合值「每死槽 ≈2.0 ns」（§5.1 读数 3）；逐槽头插 vs `tid[]` 随机写 vs 字循环各占多少，要等 E21/L5 原型插桩。故 L1b 的「扣判定成本后 −6~9%」是估算。
6. **noinline Ir 不能换算 ns**：cycleratio 的 inline/noinline A/B 出现 +130% 病态（大对象 memset），故 L4 只能是上界。
7. **r_eff 只在 digraph 1M 上标定过**（calprof），§8 的表只对该形状有效；其它形状的 r_eff 未测。
8. **bench 参数与 align 参数不可混比**：callgrind 用 align（promote 100000 等），wall/rdtsc 用 bench 参数，本文件两者只做趋势对照，不混算。
9. **EXTC_RC_M_MAX 与 pressure 是当前工作树的新代码**（e353260b，3671 行），FREEZE v4（3607 行）没有；§4 的数字来自行为等价的 v4 构建，§6 的契约引用来自新代码 ⇒ 若两者语义有差，§6.1 的「thr ≤ M_max − live/4」需要在 v4 上复算（E19）。
10. **停顿未在本轮重测**：P 的结论沿用 GC-ROOFLINE G1 的 4.71 µs（模型）+ 4.8 µs（p99 中位），memory-first 的杠杆都不改单步预算，故预期不变，但 L2 若改扫描结构需重测。
11. **Lead 的补充口径（供 E17 用，勿与 §4 混算）**：promote 500k（bench 参数）allocs=4,550,001（与本文件一致）；`marked`（mark 位真正置位次数，83 次 collect 累计）=1,809,795 ⇒ 每次 collect 标记 21.8k 对象，而每 collect 期间分配 54.8k ⇒ **mark 访问量 = allocs 的 39.8%**；`[chan] collect work_sum=4,361,283` 是**死槽释放数**（不是标记数）；`[histo]` 桶加权的 GC 总时间 ≈20.2 ms（与本文件 mark+sweep 15.17 ms 同量级，差在 release/regionFree/启动）。
12. **§5.5 L5 的三个候选都是设计草案**，无实现、无微基准；它只把「promote 的 sweep 在现有元数据下不可达」这句指向下一轮研究，不构成收益承诺。
13. **E18 的口径**：Lead 用 FREEZE v4 生成 C **重编**（`-DEXTC_RC_FLOOR` 覆盖、`-O2 -std=c11 -fwrapv`、`--pin 6`、3 reps），与本文件 §4 的 build-th 构建不同 ⇒ 绝对值略有差（sizeratio 基线 3,244–3,372 kB vs 本文件 3,240 kB，同量级）；趋势结论（256 KiB 拐点）不受影响。
14. **THP 负结果是单形状结论**：chase 上 `MADV_HUGEPAGE` 无差异（§5.6），不能外推到其它形状；其它形状未测。

## 13. 复现

```sh
# 1) 构建（不要碰 build/、build-frz、build-rf）
make BUILDDIR=build-th -j
# 2) 生成一个形状的 C 与可执行
./build-th/extc -w --no-line-map -I stdlib bench/gc/v2/src/promote.extc -o /tmp/e16/promote.c
cc -O2 -g -D_POSIX_C_SOURCE=200809L -o /tmp/e16/promote.bin /tmp/e16/promote.c
# 3) 墙钟（9 reps，取 min）
python3 /tmp/e16/wall.py promote '/tmp/e16/promote.bin 500000 20261013'
# 4) GC 拆分（rdtsc 包装副本，仓库未改）
python3 /tmp/e16/instrument.py /tmp/e16/promote.c /tmp/e16/promote.ins.c
cc -O2 -g -D_POSIX_C_SOURCE=200809L -o /tmp/e16/promote.ins.bin /tmp/e16/promote.ins.c
EXTC_RC_NOCOUNT=1 /tmp/e16/promote.ins.bin 500000 20261013 2>&1 >/dev/null | grep '\[e16\]'
# 5) 占用与墙契约
EXTC_RC_PAUSE=1 /tmp/e16/promote.bin 500000 20261013 2>&1 >/dev/null | grep -E '^\[(work|mmax)\]'
# 6) 图与表
python3 docs/analysis/model/roofline.py   # -> roofline.png / roofline-table.md
```

## 附录 A. 推导

### A.1 GC 时间恒等式与税

设 B 为分配字节、T = B/a_all（含 GC）为墙钟、T_mut = B/a_mut（不含 GC）。则

  T = T_mut + T_GC ⇒ 1/a_all = 1/a_mut + λ，其中 λ ≡ T_GC/B [ns/B]；tax ≡ T_GC/T = λ·a_all。

### A.2 λ 的分解

一轮：trace 活集 L 字节、扫 cap 字节 F_slab·H = F_slab·r·L（H=r·L 为触发堆）、可达垃圾 (r−1)L。故每分配字节的 GC 成本

  λ(r) = [c_t·L + c_s·F_slab·H] / [(r−1)L] = (c_t + c_s·F_slab·r)/(r−1)。

r=2.83 时 c_t=0.4114、c_s=0.0108、F_slab=1.30 ⇒ 0.2465（实测 0.2508，−1.7%）。

### A.3 税目标反解

λ ≤ 0.05 的税 ⇔ λ ≤ 0.05·(1/a_mut)/0.95；在 M=3L（r=2）下 λ=c_t+2c_sF ⇒ c_t ≤ 0.129 ns/B。

### A.4 M 下界（keep-up）

每 K=64 次分配推进一次 markStep，一步最多 b=256 个对象出栈（MARK_BATCH_MAX），每对象平均 g=2.76 个指针 ⇒ 每步最多 trace b 个对象、覆盖 g·K 个新对象 ⇒ 需要 b·(每步分配对象数 K) 与 g·K 匹配 ⇒ r ≥ 1 + g·K/b = 1.69 ⇒ M = L(1+r) ≥ 2.69L。

### A.5 RSS 与 M_max 的换算

RSS ≈ (标记期在飞垃圾 + 触发线下的活集 + slab 取整) ≈ M_max × F_slab；实测 promote 1.30×、chase 1.09×（chase 活集大，取整占比小）。小活集形状 M_max<floor ⇒ RSS ≈ 3×floor，与 L 无关。
