# extC GC 优化方案：类 Roofline 的形式化策略模型（最终稿 v1.0）

> 状态：定稿草案，供用户评审。基线 **HEAD 85b8871b**，实测二进制 extc md5 f4a0a9b62f4ac2fff1892f53dd34bc42。
> 唯一实测来源：docs/analysis/MEASUREMENTS.md（310 行）+ docs/analysis/MEASUREMENTS-TIER1.md（350 行，事件档 / T1）。凡本文件出现的性能数字，均按四列规范读作
> **数值 | 构建(HEAD+md5) | 档位(0/1/2+全部开关) | 量具(VmHWM / [pause] / [step] / Ir)**；推导值一律标「模型」。
> 配套：docs/analysis/model/roofline.py（一键生成 roofline.png 与 roofline-table.md，退出码 0），
> 现状事实见 docs/analysis/MEMORY-MODEL-CURRENT.md，缺口对账见 docs/analysis/DOC-AUDIT.md。
> **不修改任何源码**；第 6 节的 file:line 是「要改的位置」，不是「已改」。
> 外部证据文件（本文件引用其行号）均在 `tests/reclaim-perf/` 下：`MARK-MODEL.md`、`SWEEP-CENSUS.md`、`TAXMAP.md`、`DIGRAPH.md`、`FINDINGS.md`、`SWEEP-SLICE-RECON.md` ⇒ 裸文件名一律指该目录。

---

## §0 一页结论

### 0.1 用户的三个硬要求，逐条对上

| 要求 | 现状判定 | 本方案的答复 |
|---|---|---|
| ① 内存有界 | **有条件成立**：rc region 有上界但非常量（M ≤ 2L + max(L,floor) + 取整，MEMORY-MODEL-CURRENT §18.4），且有 5 条永久泄漏路径（§1.3-3/4/5）与「只 free 不 munmap」 | 把上界写成一个**可配置的 M**：M ≥ L(1+128/b*)，并用 4 条门禁盯住泄漏路径 |
| ② 停顿有界 | **只在分配点推进通道成立**（[step] p99≈2 µs、>1 ms 0 次），regionFree / releaseOld / collect / settle 四个通道不成立（2344 µs 反例） | 把 P 界写成 P ≤ c_m·b + c_s·s + c_c·w + c_f·f + c_r·n + ε（沿用 MEMORY-MODEL-CURRENT §19.4），P0 先把主项 c_c·w（SCAN_CHUNK 55–100 µs/段）压下来 |
| ③ 类 GPU Roofline 的形式化模型 | 不存在 | §3：横轴 λ=每分配字节 GC 成本，纵轴 a=分配率；三条约束线 + 平台，**实际吞吐 = min(三线)**；ridge 点、闭式最优、可行域判据全部给出 |

### 0.2 给定 (M_max, P_max, 吞吐目标) ⇒ 推荐 (b, r, σ) 与可行性判定

模型闭式（推导见 §3.5、附录 A）：
b* = min(256, floor(P_max/τ_t))，r* = M/L − 64/b*，σ* = 1/r*，可行 ⇔ 128L/(M−L) ≤ b* ⇔ M ≥ L(1+128/b*)。

| 给定 | 推荐 (b, r, σ) | 模型 λ | 判定 | 不可行/不可用时先放宽 |
|---|---|---|---|---|
| M=1.11 MiB, P=4 µs | b=256, r=1.25, σ=0.800 | 1.048 ns/B | **临界**（两墙交于一点；税 ≈54%，实际不可用） | ① 提高 M（最便宜） |
| M=2 MiB, P=4 µs | b=256, r=2.45, σ=0.409 | 0.218 ns/B | **可行**（税 ≈11% @0.515 GB/s） | — |
| M=8 MiB, P=4 µs | b=256, r=10.53, σ=0.095 | 0.071 ns/B | **可行**（税 ≈3.7%） | — |
| M=2 MiB, P=1 µs | 无解：b_min=75.5 > b*=69 | — | **不可行** | ① P→2 µs（λ 只贵 6%）→ 可行；② 或 M→2.22 MiB |
| 现网 digraph (σ=0.742) | b=256, r=1.349（固定倍率 2 的实测值） | 0.763 ns/B（实测锚点 0.212） | 可行但**模型高估 3.6×**（§2.4 E2/E3/E5） | 先做标定，再调参 |

**放宽顺序（默认）**：M ↑ > P ↑ > floor ↓ > 只在 σ>σ* 时开计数 > 分片（当前不可用，见 §4.5）。

### 0.3 十二条结论（每条都能在正文找到出处）

1. **三档默认，不是一个默认**：T0 = unitReclaimIdle 整段不发射（codegen.c:13962-13986、:14062-14065）；T1 = 事件档 NOCOUNT=0/SWEEP_FREE=1（codegen.c:15604-15612）；T2 = 清扫档 NOCOUNT=1/SWEEP_FREE=0（codegen.c:13770、reclaim.c:2914/2916）。本文件所有 λ/σ/a 都在 **T2** 上标定。
2. **T0 零税已证**：pa 512M 次分配 1.50 s、RSS 1.5 MB 平坦、rc 记号 0、零停顿（[实测] T0 / VmHWM）。
3. **T2 的 GC 税在小 σ 形状上很低**：pe/pc 的 GC 工作量占墙钟 15–22%，反推 c_s ≈ 0.039–0.054 ns/B（[实测] T2 / [step] 减插桩）。
4. **计数很贵**：NOCOUNT=0 墙钟 ×1.76–4.01（pb/pe/pc），反推 c_r ≈ 0.117–0.411 ns/B，中位 0.23（[实测] T2+计数开 / wallclock）。**现语料里没有一处该开计数**。
5. **交叉点 σ\* ≈ 0.42（带 0.22–0.59）**：σ 越低（垃圾越多）清扫越划算；σ 越高（几乎全活）计数才有机会。固定倍率 2 ⇒ σ=0.5，正好落在交叉点附近 ⇒ 现在的配置是「两边都不占优」的临界区。
6. **内存墙**：σ ≥ L/(M − 64L/b)；**keep-up 墙**：σ ≤ b/(b+64)。两墙交于 M = L(1+128/b)，b=256 时 **M_min = 1.5L = 1.11 MiB**。
7. **停顿墙**：b ≤ min(256, P_max/τ_t)，τ_t=14.4 ns/obj ⇒ b=256 对应 **P=3.69 µs**；P=1 µs 时 b=69 < b_min=75.5 ⇒ **M=2 MiB 下不可行**（不是缓慢退化，是硬悬崖）。
8. **P 的主项不是 walk**：[step] 的 p99 2 µs、tail 86–314 µs 主要由**大对象分段扫描**造成（SCAN_CHUNK=65536 words，关分段后 1010–1044 µs）。⇒ P0 第一条就是缩小 SCAN_CHUNK。
9. **两个未界通道必须改**：regionFree（帧销毁，O(活槽) 无预算，pb 退出 1953.7→298.4 MB 那次）与 releaseOld（标记预算无穷，persist_nodes 2344 µs）——原样引用 MEMORY-MODEL-CURRENT §19.3。
10. **事件档（T1）的适用域是「无出边的死对象」，不是「无环」**：freeSlot 不级联（reclaim.c:1245-1264）⇒ 死一个父结点漏一棵子树：强制 T1 于 chain2 泄漏 2,000,000/2,000,000 个子结点（45.4 MB vs T2 的 3.0 MB，MEAS-T1 §5.3）；churn/rebuild 在 T1 下 831 MB / 521 MB、ring 8369 MB（tests/reclaim-acyclic/README.md）是同一机制。**税与内存实测**（MEAS-T1 §3/§8）：16 B 上 T1 比 T2 慢 2.50×（7.55 vs 3.02 ns/op），456 B 打平，32 KB 上 T1 快 3× 且峰值 1.03× 活集 vs T2 的 7.1×。⇒ 只在「大槽 / 高字节率」形状上双赢；放宽 acyclic 判据（codegen.c:13988-14031）**之前**必须先补 freeSlot 的子引用级联（P0-5）。
11. **分片不可用**：SHARDS=2/4 在单线程环负载上 ×10.16/×10.53 时间、RSS ×2120（6.1 GB），完全不回收 ⇒ 默认必须保持 1（codegen.c:14066-14091 的跨站点环裁决也是退 1 片）。
12. **λ 的绝对精度还差 3.6×**，模型给 0.763 ns/B、实测锚点 0.212 ns/B ⇒ 结论的**方向**可信、**数值**必须靠 E2/E3/E5（s̄、c_t、κ）标定后才能拿去定参。

---

## §1 现状：三档默认路径、已经能证的界、以及漏洞

### 1.1 三档默认路径（这是本文件的口径基线）

| 档 | 触发条件 | 发射的开关 | 回收器行为 | 实测 |
|---|---|---|---|---|
| **T0 不发射** | 整编译单元被 unitReclaimIdle 证明「从不进入需要回收的路径」（codegen.c:13962-13986），scanUnitReclaim 里 if (unitReclaimIdle(g, m)) { g->needReclaim = false; return; }（codegen.c:14062-14065） | 不发射任何 extc_rc_* | 只有静态 arena | pa：1.50 s / 1.5 MB 平坦 / 0 记号 |
| **T1 事件档** | EXTC_RC_SWEEPFREE 未设且**整单元**过 acyclic 判据：if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) { g.rcSweepFree = 1; g.rcNoCount = 0; }（codegen.c:15609-15612）。判据是 4 条合取（src/check/check_top.c:6166-6205 注释逐字 + codegen.c:13988-14031）：(i) 写进 ref 字段的值必须是本帧新鲜分配（直接 new/alloc，或来自 acyclic 且 retFresh 的被调函数）；(ii) 或 null；(iii) 值的层号**严格大于**宿主层号；(iv) 每个能丢弃引用的位置必须是被计数的 rc 槽（无 ref 局部量赋值、无涉及引用的 opaque 调用）。⇒ **比「无环」强得多** | NOCOUNT=0（计数入口真实）、SWEEP_FREE=1 ⇒ shouldSweep 恒 0（reclaim.c:2650-2659 的 #if EXTC_RC_SWEEP_FREE return 0;） | 只靠计数归零当场 freeSlot（**不级联**，reclaim.c:1245-1264） | **已实测**（MEAS-T1 §3/§5.3/§6.1）：leaf 16B×200M 自动进 T1，7.55 ns/op、峰值 1 MB、freeSlot 200M−1、slabNew 1、[pause]/[step] 结构性为 0；456B 与 T2 打平；32KB 上快 3× 且峰值 1.03× 活集（T2 为 7.1×）；父→子链 chain2 **自动退回 T2**（判据 iii），强制 T1 则泄漏 2M 个子结点（45.4 MB vs 3.0 MB） |
| **T2 清扫档（默认）** | 其余情况（含环 / 未证明） | NOCOUNT=1（codegen.c:13770、reclaim.c:2914 #define EXTC_RC_NOCOUNT 1；计数入口在 reclaim.c:1312-1327 变 no-op）、SWEEP_FREE=0 | 区域标记 + 步进清扫 | pc/pb/pe/pd/pg2 全部实测 |
| 无回收基线 | EXTC_RECLAIM=0 | 不发射运行期；**连分配器都换成纯 bump 的 extc_arena_\*** | 无 | pc 4615.1 MB / 2.41 s（**不是纯 GC 税对照**，见 §2.3） |

三档的开关级证据：DOC-AUDIT.md §0.1。注意 EXTC_RECLAIM=1 只是「允许发射」，真正的「是否需要」是上面三条证明判定的。

### 1.2 已经能证的界（5 条，都带出处）

**B1（T0 零税）**：T0 档程序不链接回收器调用点，GC 成本恒 0，内存上界 = 静态作用域内分配量之和。
证据：pa 512M 次分配、1.5 MB 平坦、[rc] count=0（[实测] T0）。

**B2（T2 内存有上界但不是常量）**：稳态峰值 H_peak = H_trig + drift，H_trig = max(2·L, floor)·scale（reclaim.c:2631-2642 的 thrUpd），floor=1 MiB（reclaim.c:140-141），scale 空扫 ×4（reclaim.c:150-151）、上限 2^40（reclaim.c:143-144）。
⇒ M ≤ 2L + max(L, floor) + slab 取整（MEMORY-MODEL-CURRENT §18.4）。pb 上表现为「上界 = 活集」（1953.7 MB，20 B/node，逐点线性），pc 上表现为 2.9 MB 恒定。

**B3（分配点通道的停顿有界）**：单步工作量由编译期常量封顶（MEMORY-MODEL-CURRENT §19.1-19.2），所以 P_step ≤ c_m·b + c_s·s + c_c·w + c_f·f + ε，与堆大小无关。
证据：100M 次分配后 [pause] max 仍 16–68 µs、[step] p99 2 µs、over1ms=0（[实测] T2 / [step]）。

**B4（keep-up 在 σ≈0 的环形状上成立）**：每次分配最多扫 SWEEP_BUDGET=8 words = 512 槽（reclaim.c:887-889），只要单次分配产生的待清槽 ≤512 就收得住。
证据：pc 峰值 2.9 MB 恒定；关回收 4615.1 MB（+1.9 GB/s）（[实测] T2 / VmHWM）。

**B5（安全性：错误方向是「漏」不是「悬垂」）**：无对象移动（地址从不移动，promotion 直接在静态层 arena 里分配），freeSlot 不级联（reclaim.c:1245-1264），计数偏高只会晚收（保守）。⇒ 任何策略失败都表现为内存涨，而不是野指针。

### 1.3 漏洞清单（8 条，全部带 file:line 与实测反例；这是优化方案必须闭合的部分）

1. **停顿界只覆盖一条通道**。regionFree（帧销毁）O(该 region 活槽)、无预算（reclaim.c:1970-2009；pb 退出 1953.7 MB→298.4 MB 那次释放）；releaseOld 标记预算无穷、最坏 O(子树)（reclaim.c:2442-2491；persist_nodes 2344 µs）；collect/settle/account 查询走契约入口、无界排空（reclaim.c:2611-2622、:2765-2775、:2851-2861）。**原样引用 MEMORY-MODEL-CURRENT §19.3**。准确表述是「分配点推进通道的停顿有界」，不是「停顿有界」。
2. **T1 事件档的计数不完备**：freeSlot 只释放该槽本身，不遍历子对象、不递减孩子 hold（reclaim.c:1245-1264），所以计数归零只对**叶子**有效；churn/rebuild 在 T1 下 831 MB / 521 MB、ring 8369 MB（tests/reclaim-acyclic/acyclic_heapref.extc:12-16 注释与 tests/reclaim-acyclic/README.md）。README.md 原话：「事件档（计数开 + 清扫关）实测在语料里没有一个能赢的形状」——**该结论被 MEAS-T1 按对象大小分层修正**：16 B 上 +150%（成立）、456 B 打平、32 KB 上 T1 反而快 3×（不成立）。
3. **region 表满即永久泄漏**：EXTC_RC_REG_MAX = 512（codegen.c:12415），extc_rc_regionOf 只在 regN < MAX 时登记（codegen.c:12423-12432），extc_rc_regionDrop 查不到 key 直接返回（codegen.c:12434-12443）⇒ 未登记 region 的 slab 永不归还。
4. **scale 上限事实上关闭回收**：EXTC_RC_SCALE_MAX = 2^40（reclaim.c:143-144），空扫一次 ×4（reclaim.c:2598-2609）⇒ 连续空扫后阈值等价无穷。
5. **noSweep/owe 无清零路径**：noSweep 仅在 reclaim.c:1761（pin 表满）与 :1821（宿主未知的保守 store）置位；T2 下该 region 永不回收（MEMORY-MODEL-CURRENT §18.3-2）。
6. **只 free 不 munmap**：regionFree 用 free(s->data) 把 slab 还给 malloc（reclaim.c:1970-2009），虚拟地址不归还 OS ⇒ **RSS = 历史峰值**。反例：pg2 先建 5M 长命（142 MB）→ 丢 120 MB 活集 → RSS 一个字节不降，后被 churn 推到 316.8 MB 停住（[实测] T2 / VmHWM）。
7. **分片旋钮不能用**：SHARDS=2 8.13 s / 6147.2 MB、=4 8.53 s / 6080.9 MB（对默认 ×10.16 / ×2120）（[实测] T2+SHARDS / VmHWM+wallclock）；codegen.c:14066-14091 的跨站点环裁决也把 digraph 退回 1 片。
8. **触发与内存压力脱钩**：shouldSweep 只看 r->bytes > r->thr && roundOk（reclaim.c:2650-2659），而 roundOk 用 allocBytes − roundVisitedBytes ≥ max(live,floor)（reclaim.c:2646-2649）；一次触发只收「最有垃圾」的一个 shard（codegen.c:4147-4172）⇒ 在 σ 接近 1 时触发频繁但每轮几乎收不到东西，λ 发散。

---

## §2 实测锚点与标定项（唯一实测来源：MEASUREMENTS.md + MEASUREMENTS-TIER1.md，2026-11-09）

### 2.1 稳态实测（四列规范，全部来自 MEASUREMENTS.md）

| # | 数值 | 构建(HEAD+md5) | 档位(0/1/2+开关) | 量具 | 出处 |
|---|---|---|---|---|---|
| M1 | pa：2M 轮 × 256 node = 512M 次分配，1.50 s，峰值 RSS **1.5 MB 平坦**，rc 记号 **0**，零停顿 | 85b8871b / f4a0…c42 | **T0**（unitReclaimIdle 整段不发射） | VmHWM + rc 计数器 | MEAS §0、§6.3 |
| M2 | pb：100M node，1.50 s，峰值 **1953.7 MB**，≈**20 B/node**，≈**1.30 GB/s**（逐点线性），退出时 regionFree 才降到 298.4 MB | 同上 | **T2**（NOCOUNT=1，SWEEP_FREE=0，SHARDS=1） | VmHWM | MEAS §0、§3 |
| M3 | pc：100M 轮（200M 次分配），0.80 s，峰值 **2.9 MB 恒定**；环垃圾 2.9 MB 不涨 | 同上 | **T2** | VmHWM | MEAS §0 |
| M4 | pc 无回收对照：2.41 s，**4615.1 MB**（+1.9 GB/s） | 同上 | **EXTC_RECLAIM=0**（连分配器换成纯 bump） | VmHWM | MEAS §0、§3 |
| M5 | pg2：先建 5M 长命（142 MB）→ 丢 120 MB 活集 → **RSS 一字节不降**，后被 churn 推到 316.8 MB 停住 | 同上 | **T2** | VmHWM | MEAS §0、§3 |
| M6 | 计数税：NOCOUNT=0 墙钟 **pb ×1.76–1.81、pe ×2.14、pc ×3.89–4.01** | 同上 | T2 + **NOCOUNT=0**（计数开，sweep 仍开） | wallclock | MEAS 表 A2 |
| M7 | 分片：SHARDS=2 **8.13 s / 6147.2 MB**（×10.16 / ×2120）、=4 **8.53 s / 6080.9 MB** | 同上 | T2 + **SHARDS=2/4** | VmHWM + wallclock | MEAS 表 A2 |
| M8 | [pause]（一次 collect）：pc 4577 轮，max **16–68 µs**、均值 ≈1.0 µs、占墙钟 0.6%；pe max 29 µs；pb 4.0 µs；pd 5–6 µs | 同上 | **T2** | **[pause]** | MEAS §6.3 |
| M9 | [step]（切片）：pc n=425,661，**p99 ≈2 µs**、>5 µs 仅 0.07%、tail **86–314 µs**、**over1ms=0** | 同上 | **T2** | **[step]** | MEAS §6.3、§6.4 |
| M10 | 大对象关分段：-DEXTC_RC_SCAN_CHUNK=1048576 ⇒ **1010–1044 µs**（默认 collect 5–8 µs / step ≤115 µs） | 同上 | T2 + **编译期关分段** | **[step]** | MEAS §6.6 |
| M11 | GC 实际工作量：[step] total 145–195 ms 减插桩 17–21 ms ⇒ **124–174 ms**，占 0.80 s 的 **15–22%** | 同上 | T2 | [step] − 插桩基线 | MEAS §6.2 |
| M12 | 六个门禁全绿：Ir 82,037,532 ≤ 88,029,115；gate-work 6/6；tables 427.8/48.2；mark-locality D1mr 616,112 / DLmr 145,962；slab-scale 199.5 vs 197.5 | 同上 | T2 | Ir（callgrind）/ 各自门禁 | MEAS §9 |

**M11 的用法（本模型最重要的一条反推）**：pc 的 σ≈0（环垃圾，几乎全是垃圾），所以每分配字节成本趋近清扫地板
c_s = (124–174 ms) / (200M × 16 B) = **0.039–0.054 ns/B**。

### 2.2 从实测反推的模型常量（推导全部可复算；脚本 §1 同源）

| 常量 | 值 | 反推方式 | 置信度 |
|---|---|---|---|
| s̄（每槽载荷） | digraph **80 B**；pc/pb/pe **16 B**（2×i64） | digraph 见 SWEEP-CENSUS.md:37-38；本机形状按源码结构（node{val,next}） | 中（E6 标定） |
| a_m（受管分配率） | pa 5.46 / pe 4.57 / pc 4.00 / pb 1.07 GB/s | 分配次数 × s̄ / 墙钟：512M×16/1.50、200M×16/0.70、200M×16/0.80、100M×16/1.50 | 高（实测） |
| c_s（清扫地板，每分配字节） | **0.045 ns/B**（0.039–0.054） | M11：GC 时间 / 分配字节 | **高（本次实测）** |
| c_r（计数成本，每分配字节） | **0.23 ns/B**（0.117–0.411） | M6：Δρ = 1 − 1/slowdown，λ = Δρ / a_m ⇒ pe (1−1/2.14)/4.57=0.117；pc (1−1/3.95)/4.00=0.187；pb (1−1/1.785)/1.07=0.411（pb 那臂含 sweep，是上界） | 高（本次实测） |
| c_t（trace，每分配字节） | 0.18 ns/B（= τ_t 14.4 ns/obj ÷ 80 B） | MARK-MODEL.md:71-73（**未在 MEASUREMENTS 复测**） | 中（E2） |
| κ（每轮固定开销） | 20 µs（**占位**） | 无实测；用 [pause] 均值 ≈1 µs 与 collect 0.6% 墙钟反推数量级 | 低（E1） |
| τ_t / τ_s | 14.4 ns/obj / 0.63 ns/slot | MARK-MODEL.md:71-73、SWEEP-CENSUS.md:151 | 中（E5） |
| L（活集） | 9723 槽 × 80 B = 777,840 B = **0.742 MiB** | marked p50（MARK-MODEL.md:58-59）× s̄ | 中（E6） |
| H_trig（触发时堆量） | **1.049 MB**，⇒ σ = L/H = **0.7415**，r = 1.349 | 触发 bytes p50（MARK-MODEL.md:58-59） | 中（E6） |
| λ 实测锚点（digraph） | **0.212 ns/B**，⇒ ρ = λ·a = **10.9%**（与 SWEEP-CENSUS「collect 占墙钟 10.9%」自洽） | 54.32 ms collect / 256 MB 分配（3.2M 槽 × 80 B）；a_m = 256 MB / 0.497 s = 0.515 GB/s | 高（两次独立口径一致） |
| a_peak（mutator 平台） | **≥5.5 GB/s**（pa 实测 5.46） | T0 档纯 bump 分配上限 | 下界（E4） |

### 2.3 三个口径警告（不写清就会误判）

1. **EXTC_RECLAIM=0 不是「无 GC」基线**：它连分配器都换成纯 bump 的 extc_arena_*（§1.1），所以 M4 的 4615 MB / 2.41 s 同时包含「无回收」和「分配器不同」两个差异，只能当上界参照，不能当 GC 税分母。
2. **NOCOUNT=0 不是 T1 事件档**：M6 那组的 sweep 仍然开着（SWEEP_FREE=0），所以它测的是「计数 + 清扫」的税，是 c_r 的上界；纯计数成本已由 MEAS-T1 实测（自动档，见 §2.5）：16 B 上 7.55 vs 3.02 ns/op（T1 慢 2.50×）、456 B 打平、32 KB 上 T1 快 3×。
3. **λ 的两个分母**：per collect（触发字节）与 per allocated byte 差 (1−σ) 倍；本模型一律用 per allocated byte（因为 Roofline 的横轴必须是「每分配字节」）。用错分母会差 3–4×（§9 风险 R2）。

### 2.4 标定实验清单 E1–E9（谁测什么、怎么测、判据；E4/E6 先做）

| # | 标定什么 | 怎么测 | 判据 / 目标精度 | 优先级 | 谁 |
|---|---|---|---|---|---|
| E1 | κ（每轮固定开销） | EXTC_RC_TIME=1 数 collect 次数，配 wallclock 与 [pause] total：κ = (collect_total − 标记/清扫)/(轮数) | κ 误差 <30% | P1 | empirics |
| E2 | c_t（trace 单价） | 高 σ 形状（persist_nodes）+ mark-locality 门禁；直接量「mark 时间 / 被 trace 的字节」 | c_t ∈ [0.10, 0.30] ns/B | **P0** | empirics |
| E3 | λ 绝对标定 | 同一形状上同时数 alloc bytes 与 collect 时间，**排除 p50 口径**；digraph 上复现 0.212 与模型 0.763 的 3.6× 差 | 模型/实测 ≤1.5× | **P0** | empirics |
| E4 | a_peak 与 I_m | T0 档（pa 形状）叠加算术负载扫参：a_m 与 a_total 同时量；同时量 s̄ | 平台值与斜率 | **P0** | empirics |
| E5 | τ_t / τ_s 复测 | MARK-MODEL 与 SWEEP-CENSUS 的 14.4 ns / 0.63 ns 未在本 revision 复测；用 [step] walk/scan 计数 × 时间 | ±25% | P1 | empirics |
| E6 | s̄ / σ 标定 | digraph 每槽真实字节（对象图统计）+ marked 槽数；VmHWM 交叉校验 | σ 误差 <0.05 | **P0** | empirics |
| E7 | 纯计数成本（T1） | SWEEP_FREE=1 + NOCOUNT=0，强制 noSweep，量 store 税与 RSS 上界（含非叶子形状的泄漏量） | c_r 不带 sweep 项 | **P0**（依赖 MEASUREMENTS-TIER1） | empirics |
| E8 | regionFree / releaseOld 停顿归因 | 在两通道插桩（进入/退出），persist_nodes 上测 max | 归因清楚、max <100 µs | P1 | empirics |
| E9 | 尾部与相位 | b 扫参（64/128/256/512）× 高 σ 形状，量 P99.9 而非 p50；同时记 marked 数分布 | P99.9/中位 ≤5× | P2 | empirics |
| E10 | 大槽墙（T2 峰值 = 活集 × 学到的 scale） | 32 KB–1 MB 槽形状（bf 类）扫 b，量 VmHWM/活集比、[step]+[pause] 总时间中缺页 sys 的占比，并记录学到的 scale | 峰值 ≤1.2× 活集，或把 scale 上限显式写进契约；缺页占比 <30% | **P0**（见 §9 R11、MEMORY-MODEL-CURRENT §18.3-6） | empirics |

---

### 2.5 事件档（T1）实测锚点（MEASUREMENTS-TIER1.md，本机 -O2，HEAD+md5 同 §2.1）

| # | 数值 | 档位 | 量具 | 出处 |
|---|---|---|---|---|
| T1-1 | leaf 16 B×200 M：**7.55 ns/op**、峰值 1 MB、freeSlot 199,999,999、slabNew 1、[pause]/[step] **结构性为 0** | T1（自动档） | wallclock + VmHWM + [rccount] | MEAS-T1 §3 A1、§6.1 |
| T1-2 | 同形状 T2 对照：**3.02 ns/op**（T1 慢 2.50×）、slabNew 17 ⇒ 小对象上 T2 更快 | T2（SWEEPFREE=0） | 同上 | MEAS-T1 §3 A2 |
| T1-3 | fd 456 B：T1 0.101 s = T2 0.101 s（打平）；峰值 2 vs 3 MB（活集 0.47 MB） | T1 / T2 | 同上 | MEAS-T1 §3 B1/B2 |
| T1-4 | bf 32 KB（活集 32 MB）：T1 **0.051 s / 33 MB（1.03×）** vs T2 **0.151 s / 226 MB（7.1×，78% 墙钟是缺页，scale 学到 4）** | T1 / T2 | 同上 | MEAS-T1 §3 C1/C2、§5.4 |
| T1-5 | chain2（父→子，2 M 迭代）：自动档**退回 T2**（判据 iii 层号要求）；**强制 T1 泄漏 2,000,000 个子结点**（45.4 MB vs T2 3.0 MB） | T1 强制 / T2 | [rccount] + VmHWM | MEAS-T1 §5.3 |
| T1-6 | 丢 32 MB 活集 + 250 ms 空转：T1 RSS 恒 34 MB；T2 34→66 MB 平台、**只在 main 返回的 regionDrop 掉到 17 MB**（两档运行期都不还 OS） | T1 / T2 | VmRSS 采样 | MEAS-T1 §5.1-5.2 |
| T1-7 | 门禁腐烂：gate-acyclic.sh 12 通过 / 3 失败，根因是 sweeps.sh 数 [rccount] collect 而增量路径走 extc_rc_step（reclaim.c:2756-2778；实测 collect=0 而 [pause] collects=18） | T2 | 门禁脚本 | MEAS-T1 §7.1-7.2 |

---

## §3 形式化模型：两条硬约束下的最优 GC 策略

### 3.1 符号表

| 符号 | 含义 | 单位 | 现状值 / 来源 |
|---|---|---|---|
| a, a_m | 受管（rc region）分配率；a_total 含非受管 | GB/s | 实测 0.515–5.46 |
| I_m | mutator 每分配字节的计算成本（形状属性） | ns/B | 1/a_total − λ，见 §3.7 |
| L | 回收后的活集字节 | B | 0.742 MiB（digraph） |
| H | 触发时的堆量（= 触发线 thr） | B | 1.049 MB |
| σ | 存活率 = L/H | — | 0.7415 |
| r | 触发比 = H/L = 1/σ | — | 1.349 |
| M | 堆上界（提交内存） | B | 待定，见 §5 |
| P | 单次停顿时限（P99 或 max） | s | 待定，见 §5 |
| b | 单步 mark 预算（对象 pops） | 对象 | 默认 ≤256（MARK_BATCH_MAX） |
| K | MARK_EVERY = 每多少次分配推进一步 | — | 64 |
| s̄ | 每槽平均载荷 | B | 80（digraph）/ 16（pc,pb,pe） |
| c_t | trace 单价（每分配字节） | ns/B | 0.18（E2） |
| c_s | sweep 单价地板（每分配字节，σ→0） | ns/B | 0.045（0.039–0.054，实测） |
| c_r | 计数单价（每分配字节） | ns/B | 0.23（0.117–0.411，实测） |
| κ | 每轮固定开销 | s | 20 µs（占位，E1） |
| τ_t, τ_s | 每对象 trace / 每槽 sweep 成本 | ns | 14.4 / 0.63（E5） |
| ρ | GC 占墙钟比例 = λ·a_m | — | 实测 10.9%（digraph） |
| ρ_max | 允许的 GC 税上限（设计输入） | — | 建议 0.10–0.15 |
| λ | 每分配字节 GC 成本 | ns/B | 模型 / 实测 0.212（digraph） |
| f | 回收频率（轮/秒） | 1/s | f = a_m/(H−L) |
| W_step | 单步工作量 | B + 槽 | W_step = b·s̄ 字节 + 512 槽（两项量纲不同，见 L5；本文件不把两者相加成单一字节数） |

**口径警告（与 §2.4 的 E6 一起读）**：L = 9 723 槽 × 80 B = 0.742 MiB 是「一次 collect 的 marked 槽数 p50」，H = 1.049 MB 是「触发时堆字节 p50」，两者同出 `tests/reclaim-perf/MARK-MODEL.md:58-59`。它们的比给出 σ=0.7415、r=1.349，**与代码里的固定触发倍率 2 不符**（`thrUpd = max(2·live, floor)·scale`，`reclaim.c:2631-2642`；L=0.742 MiB 时 2L=1.48 MiB > floor=1 MiB）⇒ 这两个 p50 不是同一相位、同一口径，§0.3-12 的「模型比实测高 3.6×」至少一半来自这里。**定参前必须由 E6 把 L 与 H_trig 统一到同一相位。**

### 3.2 假设（每条都标了「若违反会怎样」）

- **A1（单线程、回收期 mutator 停）**：ρ 直接等于停顿时长占比。若引入并行回收，ρ 要乘 1/(1+并行度)，本模型整体除以并行度即可。
- **A2（不移动、T2 无写屏障、T1 只有 O(1) store 钩子）**：见 MEMORY-MODEL-CURRENT §6/§12。若引入移动式，必须重算 c_t 与 M（半空间 ×2）。
- **A3（稳态：形状平稳，L、σ 为常数）**：若形状随时间剧烈变化（短命进程、突发），σ 应按时间加权；模型给的是「最坏稳定相位」。
- **A4（周期性：每轮从 H 回落到 L）**：若一轮收不干净（shard 只收一个、scale 退让），实际 H 单调上升 ⇒ M 界的条件失效，这是 §1.3-8 的漏洞。
- **A5（浮动垃圾 ≤ (K/b)L）**：把「标记滞后一个推进间隔」的挂账按活集比例计入。**这是保守假设，不是精确界**；精确界需要 E9 的相位实验。若实际浮动更大（mark 尾巴 5.8×），σ_min 要相应抬高。
- **A6（单位成本与堆大小无关）**：忽略 cache 局部性的二阶效应；实测支持（100M 次分配后 [pause] max 不变，MEAS §6.3），但 L1 data miss 约占一半（旧文档线索），故 c_t 绝对值仍需 E2。
- **A7（region 生命周期 ≫ 一轮）**：短命 region 的 regionFree 不走 GC 通道，走 §6 的 P0-2 分批改造。
- **A8（无 munmap）**：M 是提交内存上界；RSS 不会因为 GC 而下降（§1.3-6）。

### 3.3 三档的成本函数（λ 的定义）

- **T0（不发射）**：λ=0，P=0，M = 静态作用域分配量之和。外生界，不需要模型。
- **T1（事件档）**：λ_E = c_r（每次 store 一次加减 + hold 饱和判断，O(1)）。P_E = O(1)/store。M 只对「叶子/无级联」形状有界（§1.3-2）。
- **T2（清扫档，默认）**：一轮成本 = trace L + sweep H + κ，摊到本轮新分配字节 (H−L)：

  λ_alloc(r) = (c_t·L + c_s·H + κ) / (H − L) = (c_s·r + c_t + κ/L) / (r − 1)    [ns/B]

  校验两端点：① r→∞（σ→0，几乎全是垃圾）λ_alloc → c_s = 0.045 ns/B ✓（实测 M11）；② r→1（σ→1）λ_alloc → ∞ ✓（这就是 §1.3-8 的发散）。

### 3.4 引理（三条约束 + 两条工具）

**L1（税恒等式）**：ρ = a_m·λ（GB/s × ns/B，量纲 1e−9·1e9 = 1）。⇒ 图上任意点离屋顶的垂直距离 = log(1/ρ)，这是「Roofline 读税」的根据。

**L2（GC 屋顶）**：要求 GC 最多占 ρ_max 墙钟 ⇒ a_m ≤ ρ_max/λ。(λ, a) 平面上斜率 −1 的直线。

**L3（内存墙）**：峰值堆 = H + 浮动 ≤ M。取 H = L/σ、浮动 = (K/b)L（A5）：

  L/σ + K·L/b ≤ M  ⇒  **σ ≥ L/(M − K·L/b) =: σ_min(M,b)**

  推导要点：K·L/b 是「一个推进间隔内尚未被标记的活字节」，K=64 是 MARK_EVERY，b 是每步 pops。σ_min 随 M 增大而下降，在 M = L(b+K)/b 处发散；若 M ≤ K·L/b 直接不可行。**保守上界（A5），非精确界。**

**L4（keep-up 墙）**：稳态下一轮必须 trace 完 L 字节。一轮内可用 pops = b·(H−L)/(K·s̄)（每 K 次分配 b 个 pops，每槽 s̄ 字节），待 trace 对象 = L/s̄：

  b·(H−L)/(K·s̄) ≥ L/s̄  ⇒  b(1−σ) ≥ Kσ  ⇒  **σ ≤ b/(b+K) =: σ_max(b)**

  这是**必要条件**（忽略 sweep 与常数项）。把 c_s 算进去的精确式：σ ≤ (b·c_t − K·c_s)/(c_t·(b+K))，b=256 时为 **0.75**（比 0.800 紧）。直觉：b 越大标记越能跟上；σ 越接近 1，每轮可回收垃圾越少，越要求标记更快。

**L5（停顿墙）**：沿用 MEMORY-MODEL-CURRENT §19.4：

  P_step ≤ c_m·b + c_s·s + c_c·w + c_f·f + c_r·n + ε

  其中 b≤256 pops、s≤512 words/256 pushes、w=SCAN_CHUNK=65536 words、f≤512 槽、n=region 活槽数。T2 分配点通道 n=0、f≤512，实测主项是 c_c·w（55–100 µs/段）⇒ **b ≤ min(256, floor(P_max/τ_t))**；τ_t=14.4 ns ⇒ b=256 对应 P=3.69 µs。

**L6（可行性条件）**：σ_min ≤ σ_max ⟺ L/(M − K·L/b) ≤ b/(b+K) ⟺ **M ≥ L(1 + 2K/b)**，等价于

  **128·L/(M − L) ≤ b**（K=64）

  b=256 时 M_min = 1.5L = 1.11 MiB。可行域 = { (b, σ) : 128L/(M−L) ≤ b ≤ min(256, P/τ_t)，σ_min(M,b) ≤ σ ≤ σ_max(b) }。

**L7（单调性）**：∂λ/∂r < 0（r>1）；r_max(M,b) = M/L − K/b 随 b 增大而增大。⇒ λ 在可行域角点 (b = b*, r = r_max) 取到最小。

### 3.5 定理 T1（闭式最优）与数值

**T1**：在 M ≤ M_max、P ≤ P_max 双约束下，使 λ 最小的策略是

  **b* = min(256, floor(P_max/τ_t))**，**r* = M_max/L − K/b***，**σ* = 1/r***，**H* = r*·L**，
  **f* = a_m·σ* / (L(1−σ*))**，**W_step = b*·s̄ + 512 槽**。

  证明：L7 单调性 ⇒ b 与 r 同时取上界；b 上界由 L5 给，r 上界由 L6 的等号给。

| 配置 | b | r* | σ* | H* | λ (ns/B) | 税 @a=0.515 | 税 @a=4.0 | 判定 |
|---|---|---|---|---|---|---|---|---|
| M=2 MiB, P=4 µs | 256 | 2.45 | 0.409 | 1.90 MB | 0.218 | 11.2% | 87%（不可用） | 对低 a 形状可行 |
| M=8 MiB, P=4 µs | 256 | 10.53 | 0.095 | 8.19 MB | 0.071 | 3.7% | 28% | 可行 |
| M=1.11 MiB（=1.5L） | 256 | 1.25 | 0.800 | 0.97 MB | 1.048 | 54% | — | 临界，实际不可用 |
| M=2 MiB, P=2 µs | 139 | 2.24 | 0.446 | 1.74 MB | 0.1327 | 6.8% | 53% | 可行（λ 只贵 6%） |
| M=2 MiB, P=1 µs | 69 | — | — | — | — | — | — | **不可行**（b<75.5） |

三点读法：
1. **λ 对 r 极敏感**：r 从 1.25 提到 2.45，λ 从 1.048 掉到 0.218（4.8×）⇒「触发倍率」是**第一旋钮**，不是 sweep 预算。
2. **r 的收益按 1/(r−1) 衰减，没有「地板拐点」**：λ = c_s + (c_s+c_t+κ/L)/(r−1)。r−1 = c_t/c_s ≈ 4（r≈5、M≈3.7 MiB）只是 c_t 项降到与 c_s 同量级的位置，那里 λ ≈ 2.3·c_s = 0.105，**不是**地板。模型边际（中心差分，每 +1 MiB 的 λ 降幅）：M=2 MiB 处 38.3%/MiB，4 MiB 处 14.1%，8 MiB 处 4.6%，12 MiB 处 2.3%，16 MiB 处 1.3%，20 MiB 以上 <1%；λ 落到 1.5×地板需 r≈12.1（M≈9.2 MiB），落到 1.2×地板需 r≈28.9（M≈21.6 MiB）。
3. **P 与 M 可互换**：P 从 4 µs 收到 2 µs 只贵 6%，再收到 1 µs 直接不可行 ⇒ **P 的边际收益在 2 µs 附近拐弯**（非光滑悬崖），这正是 §0.2「不可行时先放宽 M 再放宽 P」的依据。

### 3.6 定理 T2（清扫 vs 计数）与实测对照

扫描档用 T2、计数档用 T1，两者每分配字节成本：λ_S2(r) = (c_s·r + c_t + κ/L)/(r − 1)，λ_S3 = c_r。S3 占优 ⟺ λ_S3 < λ_S2 ⟺ **r < r*_cross = (c_t + κ/L + c_r)/(c_r − c_s)** ⟺ **σ > σ*_cross = 1/r*_cross**。

| c_r 取值 | r*_cross | σ*_cross | 结论 |
|---|---|---|---|
| 0.117（乐观，pe 实测） | 4.49 | **0.22** | 只有 σ>0.22 时计数才可能赢 |
| 0.23（中位） | 2.36 | **0.42** | 交叉点正好落在现网固定倍率 2（σ=0.5）附近 |
| 0.411（保守，pb 上界） | 1.69 | **0.59** | 只有 σ>0.59 时计数才赢 |

**实测对照（本模型唯一的「可证伪」检验）**：

| 形状 | 实测 σ | 模型预测 | 实测结果 | 一致？ |
|---|---|---|---|---|
| pc | ≈0（环垃圾，2.9 MB 恒定） | 清扫应大胜（σ≪0.22） | 计数 ×3.89–4.01 慢 | 是 |
| pe | ≈0 | 清扫胜 | 计数 ×2.14 慢 | 是 |
| pb | ≈0.5（边） | 接近交叉点，计数应略差 | 计数 ×1.76–1.81 慢 | 是（方向对，幅度在误差带内） |
| digraph | 0.742 | σ>0.59 ⇒ 计数应**略占优** | 计数 **+100.9% Ir**（更慢） | 否 |

对 digraph 的不一致有两种解释：① 该形状 c_r 显著高于 0.411（边多、store 密度高）；② c_t 比 0.18 小。要翻案需 c_r ≥ 0.9 ns/B，证据不足 ⇒ **本文件立场：保持 NOCOUNT=1 默认；只在 σ>0.6 且 E7 给出 c_r≤0.2 时按形状开计数。**

### 3.7 与 GPU Roofline 的映射（为什么横轴是 λ 而不是「净垃圾字节」）

| GPU Roofline | 本模型 | 说明 |
|---|---|---|
| 横轴 arithmetic intensity (FLOP/byte) | **λ = 每分配字节的 GC 成本 (ns/B)** | 分配产出垃圾、回收消费这些字节；强度 = 每字节产出要付多少回收时间 |
| 纵轴可达算力 (GFLOP/s) | **a = 受管分配率 (GB/s)** | 与「可达性能」同构：分配率就是这套 GC 的吞吐 |
| 计算屋顶 y = P_peak/x | **GC 屋顶 a = ρ_max/λ** | 斜率 −1 的预算线；ρ_max 是允许的 GC 税 |
| 带宽平台 | **mutator 平台 a_plat = 1/I_m** | 形状属性：a_total = 1/(I_m+λ)，故 I_m = 1/a_total − λ |
| ridge point | **λ_ridge = ρ_max·I_m**（ρ_max=1 时 = 1/a_plat） | 交叉点处 GC 税 = 50% |
| 实际性能 = min(屋顶, 平台) | **a_ss = min(ρ_max/λ, a_plat)** | 低于两者 ⇒ 还有别的瓶颈（调度、内存延迟） |

**坐标选择的三点理由**（回答「为什么不是净垃圾字节」）：
1. 两条硬墙（M、P）都作用在 **σ 与 b** 上，而 σ 与 λ 一一对应（r = 1/σ，λ = λ(r)）⇒ 在 (λ, a) 平面上两面墙是**竖线**，网格更干净；
2. 净垃圾字节 (H−L) 不是独立变量：它由触发策略决定，用它会掩盖「触发点就是决策变量」这一事实；
3. 策略族（arena / 清扫 / 计数 / 分代）在 λ 上的差异是**一条线 vs 一个平台**，正好是 Roofline 的语言。

**「(λ,a) 平面上墙是竖线」的严格说明**：给定 (M, b)，内存约束给 σ 下界 σ_min、keep-up 给上界 σ_max；可行 σ 区间非空时任一可行 σ 对应唯一 λ；且**降低分配率 a 不会让墙移动**（墙只依赖 M、b、σ）⇒ 墙是竖线。反过来在 (σ, P) 平面，σ_min 随 M 下降、σ_max 随 b 上升 ⇒ 墙是斜的。这就是图 (a) 与图 (b) 的分工。

**extC 各形状落在哪**（图 (a) 的读数）：

| 形状 | λ (ns/B) | 实测 a (GB/s) | GC 屋顶 ρ=1 | 自身平台 a_plat | 判定 |
|---|---|---|---|---|---|
| pa（T0） | 0 | 5.46 | ∞ | 5.46 | 平台受限（分配 only 上限） |
| pe | 0.045 | 4.57 | 22.2 | 5.72 | **mutator 受限**（税 20.6%） |
| pc | 0.045 | 4.00 | 22.2 | 5.05 | **mutator 受限**（税 18.0%） |
| pb | 0.045 | 1.07 | 22.2 | 1.18 | mutator 受限（活集线性增长，非 GC 之过） |
| digraph | 0.212 | 0.515 | 4.71 | **0.578** | **mutator 受限**：只差自身平台 12%，但离 GC 屋顶 9.1× |

⇒ **现在 extC 的瓶颈不是 GC 吞吐而是 mutator 本身**（digraph 的 I_m = 1/0.515 − 0.212 = 1.73 ns/B）。这解释了为什么 TAXMAP 里「把 ownSlab 整个白送」也只改善清扫侧 2.5–17×、整体仍慢：屋顶不在 GC 那一侧。**所以优化 GC 的目标不是「更快的 GC」，而是「用更少的 M 换同样的税」，以及把 P 的尾部收进界内。**

### 3.8 图的读法与自检（roofline.py）

| 面板 | 内容 | 怎么读 |
|---|---|---|
| (a) | (λ, a) 平面：3 条 ρ 预算屋顶 + mutator 平台 + 各形状实测点 + 推荐配置竖线 | 点在屋顶下方越远 = 税越高；点在平台上 = mutator 受限；竖线 =「两墙同时紧」的配置 |
| (b) | 可行带：σ_min(M,b) 下降曲线、σ_max(b) 水平线、M=2/8 MiB 标注 | 两线之间才是可行 σ；M < L(1+128/b) 时无交 |
| (c) | λ_alloc(r) 单调降、P_need(r)=64τ_t/(r−1) 单调升、可行 r 区间 | 两墙夹出最优 r；r 再大 λ 也只到 c_s 地板 |
| (d) | b 的 Pareto：M_min/L = 1+128/b 与 P = b·τ_t | b 越大越省内存、越长停顿；b=256 是源码上限 |

**一致性自检**（脚本 §7 每次运行都打印）：图 (b) 标注的 σ_min 与表 3.5、公式 L3 同源同一函数，输出
M=2.00 MiB → 0.409；M=8.00 MiB → 0.095；M=1.11 MiB（=1.5L）→ 0.800；σ_max(128/256/512) = 0.667/0.800/0.889；
M_min(256) = 1.50L = 1.11 MiB。图与表冲突时以脚本输出为准（脚本是唯一源）。

---

## §4 策略族的形式化与判定（S1–S5）

每条策略用三元组 **(c, P 需求, M 需求)** 描述，并给出可行域。c 一律是「每分配字节的 GC 成本 λ」，P 一律是「单步最坏停顿」，M 一律是「稳态提交内存上界」。

### 4.1 总表

| 策略 | 实现状态 | c = λ (ns/B) | P 需求 | M 需求 | 可行域 | 实测/源依据 |
|---|---|---|---|---|---|---|
| **S1 arena+bump（T0）** | 默认（所有函数帧 + 逃逸层） | **0** | **0**（确定性，无回收步） | 静态：Σ 各词法层最大值（单函数内），逃逸层 = 全程序存活 | 生命周期静态可判、无跨层共享 | FINDINGS.md:16 C 手写 bump 6.9 ms/13.0 MB vs extC arena 364.5 ms/13.0 MB；spare 缓存收益 2.57×；always_inline 后 100.7→25.7 ms |
| **S2 region sweep（T2，现默认）** | 默认（EXTC_RECLAIM=1 且 NOCOUNT=1） | λ_alloc(r) = (c_s·r + c_t + κ/L)/(r−1)，r=1.35 时 **0.212** 实测 | b·τ_t = **3.69 µs**（b=256），尾部到 2.34 ms（其他通道，见 §6） | L/σ + K·L/b；b=256 时 1.5L = **1.11 MiB** 起 | σ_min ≤ σ ≤ σ_max；128L/(M−L) ≤ b ≤ 256 | MEAS：digraph λ=0.212、ρ=10.9%、[pause] p99 3.7 µs；SWEEP-CENSUS.md:150 每轮 67,707 槽恒定 |
| **S3 计数归零（T1）** | 开关（EXTC_RC_NOCOUNT=0；SWEEPFREE 事件档强制开） | c_r = **0.23**（0.117–0.411 区间） | **O(1)/store**（~1 次多指令序列，未单独标定） | 无环：≈L；有环：**无界**（无 drop point 时永不归零） | 引用图无环、无跨 region 悬挂 | TAXMAP.md:373-376 +100.9%（p5）~+455.2%（digraph）；:388 ownSlab ≈33 Ir/次；**MEAS-T1 自动档 T1 vs T2**：16 B +150%、456 B 0%、32 KB −66%，内存 1.03× vs 7.1× 活集，停顿结构为 0 |
| **S4 分代/复制** | **未实现（判定否）** | ≥ 2c_t（复制两侧各读一次 + 写一次 + 更新指针）；半空间 ×2 | 复制整代（秒级，除非增量 + 卡表） | 半空间 **2L**，另加卡表/pin 表 | 需移动、需地址稳定 ⇒ 与 arena/逃逸地址稳定契约冲突 | MARK-MODEL.md:203-223 WCC 不可分（250k 50/50 次为单分量；1 M p50 单分量）；:365 写屏障 +38.3% 墙钟 > 理想收益上限 18% |
| **S5 混合（region 内按 σ 选 T2/T1）** | 未实现（可实现） | min(c_r, λ_alloc(r)) ⇒ **0.10–0.23** 估算 | max(两通道单步) = 3.69 µs + c_r·store | max(两策略 M) | 每 region 的 σ 可估、且 σ 判据稳定 | §3.6 T2 交叉点；digraph 实证 c_r 高（见 §3.6 不一致） |

### 4.2 每个策略「先撞哪面墙」

| 策略 | 内存墙 M | 停顿墙 P | keep-up 墙 | 判定 |
|---|---|---|---|---|
| S1 | **先撞**：静态界 = 词法层峰值，循环内累积型直接 OOM（arena 越界 trap） | 不适用（无步） | 不适用 | 对「叶子/短寿命」最优；对「长寿命/跨层共享」不可用 |
| S2 | 条件成立（σ 可控） | **热路径成立**，其他三条通道不成立（§1.3） | 条件成立（b≥183.6 才 keep-up） | 现默认，且是**唯一已在生产路径自洽**的策略 |
| S3 | 无环时最省（M≈L）；有环时**不成立**（MEMORY-MODEL-CURRENT §19.3） | 单步最小 | 不适用（事件驱动，无轮） | 只在 σ>0.6 且 c_r 低的形状上可能赢；digraph 实测否 |
| S4 | 半空间 2L 起，比 S2 差 | 复制是整代暂停 | 不适用 | **否**（WCC 不可分 + 地址稳定 + 屏障税） |
| S5 | 与 S2 同（下界由最坏通道定） | 与 S2 同 + 计数通道 | 条件同 S2 | 唯一有希望把 λ 从 0.212 压到 ~0.10 的方向，但需 E7 标定 |

### 4.3 四个可调维度的形式化（分片 / 切片 / 分区 / 并行）

设 shard 数 = n_s（按分配站点序号取模）、切片大小 = s_片（字节/次）、region 分区数 = n_r、并行回收线程 = n_p。

| 维度 | 对 c 的影响 | 对 P 的影响 | 对 M 的影响 | 实测/结论 |
|---|---|---|---|---|
| 分片 n_s | c 不变（同一轮总工作量固定） | P 除以 n_s（每次只收 1 片） | **M 乘以 n_s**（n_s−1 片待在堆里） | SHARDS=4：250k +66.9 ms、1M +327.7 ms、RSS +52.5/+220.2 MiB ⇒ **在这形状上没界**（DIGRAPH.md:303）⇒ 保持 n_s=1 |
| 切片 s_片 | 每次切片固定开销 1.5 ns | P 上限 ≈ s_片/扫描带宽 | 无关 | digraph p50 67,707/256 ≈ 264 片，切片开销占「被切掉的停顿」0.5–0.7%（SWEEP-SLICE-RECON.md:13,75,85）⇒ 切片便宜，但**分片不便宜** |
| 分区 n_r | 无（每 region 独立记账） | 无 | M 由最大 region 定，非总和 | region 是天然的并行/分片单位，比站点分片正确 |
| 并行 n_p | ρ/n_p | **不变**（单步仍在一核上做完） | 不变 | 当前单线程无原子；引入需 E8 |

### 4.4 结论（策略层）

1. **S1 是地基，不是竞争者**：它零税、零停顿，但界是静态的。语料 1234 个程序里 **618 个（50%）** 被静态证明「回收空闲」、产物中根本没有回收运行期（`same=YES 618`，`tests/reclaim-idle/report-refactor.md:64`）⇒ 这半个语料整体就是 S1 形态，GC 策略对它们零影响。
2. **S2 是唯一可交付的默认**：它同时满足 M 的条件界与热路径 P 界；缺陷在三条非热路径通道（§1.3），而不是策略本身。
3. **S3 是形状特化选项，实测按「每节点字节 s̄」分层**：小对象上交叉点 σ*_cross ≈ 0.42（16 B 上 +150% 判负）；但大槽（32 KB）上 T2 的税变成缺页、T1 双赢（−66% 墙钟、1.03× vs 7.1× 活集）⇒ 分界不只是 σ，还有 s̄（这是 §9 R11 的新墙）。全局开仍然禁止。
4. **S4 判否**：三条独立理由（不可分、地址稳定契约、屏障税 > 收益上限），任一条即可否。
5. **S5（按 region 选 T2/T1）有了实测收益上界**：32 KB 形状上 T1 的 λ ≈ 0.127 ns/B（4.15 µs/32 KB）且内存 1.03× 活集，T2 同形状 12.3 µs/alloc、7.1× 活集 ⇒ 大槽形状上 S5 可做到「墙钟 ÷3、内存 ÷7」；小槽形状继续走 T2。前提是 P0-5（freeSlot 级联），否则 S5 会把泄漏带进默认路径。

---

## §5 推荐默认参数（可执行清单）

### 5.1 参数推荐表

| 参数 | 现值 | 推荐值 | 依据 | 风险/前提 |
|---|---|---|---|---|
| 触发比 r | **固定 2**（thr = max(2·live, floor)·scale，reclaim.c:2631-2642） | **r 自适应 = M_max/L − 64/b**（T1），并保留 scale 自学习为上限保护 | λ ∝ 1/(r−1)，r 是第一旋钮（§3.5） | 需新增 M_max 配置；L 用上一轮 markedBytes 估计 |
| b（MARK_BATCH_MAX） | 256（源码上限） | **256** | b=256 已在 keep-up 侧给出 183.6 的余量 1.39× | 无 |
| SCAN_CHUNK | **65536 words**（reclaim.c:2146-2147） | **8192 words** | L5 的 c_c·w 是停顿主项；8192 让长对象段从 55–100 µs 降到 ~7–12 µs | 需 E3 验证分段开销；长对象多分 8 段 |
| M_min | 无显式值（由 floor 1 MiB 间接给） | **1.5L**（b=256 时的可行性下界，L6） | 128L/(M−L) ≤ 256 | L 变化时需重算 |
| 触发线 | thr = max(2·live, floor)·scale，floor=1 MiB（reclaim.c:137-151） | **thr = min(原式, M_max − 64·L/b)** | 内存墙 L3 | 需 M_max 配置入口 |
| NOCOUNT | 1（默认关计数），但 acyclic 判据会把整个 unit 切到 0（codegen.c:15609-15612） | **保持现状（默认 1 + 自动档）** | §3.6 digraph 反证；MEAS-T1：自动档只在「单层 churn」上生效，chain2 自动退回 T2 | 放宽判据需先做 P0-5 |
| SHARDS | 1 | **保持 1** | DIGRAPH.md:303 分片无界 | — |

### 5.2 三个算例（直接可抄进配置）

| 场景 | L | 给 M_max | 推荐 (b, r*, σ*) | λ | 说明 |
|---|---|---|---|---|---|
| digraph 现网 | 0.742 MiB | 无（现状） | (256, 1.349, 0.742) | 0.212 | 实测点 |
| 收紧内存 | 0.742 MiB | **2 MiB** | (256, 2.45, 0.409) | 0.218 | 税与现在持平（0.218 vs 0.212），但 M 有了硬上界 |
| 放宽内存换低税 | 0.742 MiB | **4 MiB** | (256, 5.14, 0.194) | **0.106** | 税近乎腰斩（10.9%→5.5% @a=0.515）；λ 仍是地板 0.045 的 2.3 倍 |
| 极限 | 0.742 MiB | 8 MiB | (256, 10.53, 0.095) | 0.071 | 8 MiB 处边际 4.6%/MiB、12 MiB 处 2.3%/MiB；要 <1%/MiB 得 M≥20 MiB |

### 5.3 不可行时的放宽顺序（这是 Lead 要的三元组回答）

给定 (M_max, P_max, 吞吐目标)：先由 T1 算 (b*, r*, σ*)，再判 σ_min ≤ σ* ≤ σ_max 与 b* ≥ 128L/(M−L)。不可行时**按下列顺序**放宽（每个减 λ 的边际收益）
1. **放宽 M_max**（最便宜）：λ ∝ 1/(r−1)，M 从 1.5L 到 2.45L，λ 从 1.05 掉到 0.22（4.8×）；每 +1L 平均 −30%~−45%。
2. **放宽 P_max**：P 4 µs→2 µs 只贵 6%（b 256→139）；P<1 µs 处**不可行悬崖**（b<75.5 违反 L6），不要线性外推。
3. **放宽 ρ_max（税上限）**：等价于把 GC 屋顶线整体下移，只在 mutator 受限的形状上合法（digraph 有 9.1× 余量，pc/pe 只有 1.1×）。
4. **最后才动策略**：换 S3/S5 需要 σ 与 c_r 的标定证据（§3.6），不是参数问题。

---

## §6 改造路线（P0 / P1 / P2）

### P0（不改变语义，只把无界通道变成有界）

| # | 改造 | 位置 | 现状问题 | 判据 |
|---|---|---|---|---|
| P0-1 | SCAN_CHUNK 65536 → 8192（可配） | src/back/reclaim.c:2146-2150、:2197、:2248 | 单段扫描 65536 words 是停顿主项（L5 的 c_c·w，实测 55–100 µs/段） | G1：P99 ≤5 µs、max ≤50 µs |
| P0-2 | regionFree 的 slab 释放分批/预算化 | src/back/reclaim.c:1970-2019 | 帧销毁 O(活槽)，无预算；pb 退出 1953.7→298.4 MB（§19.3 第一条） | G2：单次 regionFree ≤1 ms |
| P0-3 | releaseOld 的标记预算化 | src/back/reclaim.c:2451-2491（`while (r->msp)` 无预算） | persist_nodes 单步 2344 µs（§19.3 第二条） | G3：max ≤50 µs |
| P0-4 | 触发线与 M_max 绑定 + r 自适应 | src/back/reclaim.c:137-151、:2631-2642；调用点 src/back/codegen.c:4138 | 固定 r=2 时 M 无显式上界，scale 可退到 2^40 | G4：注入 4× 循环垃圾时 RSS ≤ M_max + 8 MiB |
| P0-5 | **freeSlot 补子引用级联**（计数档）：freeSlot 释放槽时按类型描述符对每个指针子槽做 hold--（现在只在 rsIncN 时才于 freeSlotLink 跑 unlinkChildren） | src/back/reclaim.c:1245-1264、:1238-1243、:1271-1279 | T1 对「父→子」死对象泄漏整棵子树（45.4 MB vs 3.0 MB）；这是放宽 acyclic 判据 / 启用 S5 的**前置条件** | G8：强制 T1 于 chain2 时峰值 ≤1.1× 同形状 T2（当前 15.1×） |
| P0-6 | **补任务根登记**：把 `extc_task_rootPush` 的发射器接上（任务开始处登记可驱动句柄槽），或让 `extc_task_collect` 只释放“确认已无句柄”的 place | 发射侧 src/back/codegen.c（`extc_task_begin` 有 4 处发射点 2986/3220/7385/7416，rootPush 零处）；运行期 src/back/coroutine.c:69/:74 | `extc_taskRootN` 恒 0 ⇒ 注释承诺的“按编译器可见句柄判活”这层保护完全不存在：`syspool::collect()` 一律放掉所有 depth==0 的活任务 place。当前靠调用者断言（`stdlib/std/sys/pool.extc:52-55`）掩盖，属潜伏的 use-after-free 面 | G9：`collect()` 在有活任务时调用，任务帧仍可驱动（现有 `tests/zone/` 两个用例都不覆盖这情形） |

> **P0-3 的最便宜实现**：不需要新状态机。releaseOld 是纯 DFS（`extc_rc_markPush` + `while (r->msp)`），源码注释已声明“少释放是保守方向”（reclaim.c:2442-2447），所以预算耗尽时**直接停止扩展、清 mark、返回**即可，未释放的对象退回增量标记/清扫。调用侧本来就有 `extc_rc_budgetOk` 闸（reclaim.c:2504-2505），只是函数内部没有计数。
>
> **P0-6 的严重性**：不是现行崩溃（API 契约要求调用者自称无句柄），而是“注释承诺的保护不存在”——一旦有程序按注释（collect 会保留仍可达的任务）使用，就是 use-after-free。修复成本低（发射 4 个登记点），建议与 P0-5 同批做。

### P1（把「条件成立」变「无条件成立」）

| # | 改造 | 位置 | 说明 |
|---|---|---|---|
| P1-1 | roundOk 加内存压力项 | src/back/reclaim.c:2646-2649 | 现在只看 allocBytes 与 live；压力下应允许提前开轮 |
| P1-2 | sweep 预算与 SCAN_CHUNK 联动 | src/back/reclaim.c:887-889 | 8 words/step 是 512 槽/步；与 mark 预算共同决定 W_step |
| P1-3 | REG_MAX 满表行为明确化 | src/back/codegen.c:12415-12443 | 512 region 表满后的行为需显式化（丢注册 = 不安全或漏收） |
| P1-4 | 事件档自动判定的可观测性 | src/back/codegen.c:15608-15611 | 现在自动开 SWEEPFREE 时无输出，需 dump 判定依据 |
| P1-5 | 门禁量具修复：sweeps.sh 改数 [pause] collects 或 extc_rc_step 计数 | tests/reclaim-acyclic/sweeps.sh；src/back/reclaim.c:2756-2778 | 现在数 [rccount] collect，而增量路径走 extc_rc_step ⇒ 读数恒 0（实测 collect=0 而 [pause] collects=18），gate-acyclic 12 通过 / 3 失败（MEAS-T1 §7.2） |
| P1-6 | 停顿直方图插桩（替代 p99 门禁） | src/back/reclaim.c:2678-2714 | 现在只有 max/total + 4 个上界桶 ⇒ p50/p99 结构性不可测（MEAS-T1 §9.2），G1 只能判 max/mean/over1ms |

### P2（研究/收益未证）

| # | 方向 | 判据 |
|---|---|---|
| P2-1 | S5 混合：region 内按 σ 选 T2/T1 | E7 给 c_r≤0.2 且 σ 判据稳定；收益 λ 0.212→~0.10 |
| P2-2 | 分区并行（n_p>1） | 需先证明单步可切；当前单线程无原子（A1） |
| P2-3 | 归还 RSS（madvise/munmap） | 现在 M 是提交内存界、RSS=历史峰值（A8，§1.3-6） |

---

## §7 门禁（回归用）

| 门禁 | 判据 | 量具 |
|---|---|---|
| G1 停顿尾部 | **max ≤50 µs、over-1ms = 0、mean ≤5 µs**；p99 需先加直方图（P1-6） | EXTC_RC_PAUSE 内建计数（MEAS §2） |
| G2 契约入口 | collect/account 路径 ≤200 µs | 同上 |
| G3 regionFree | 单次 ≤1 ms | 同上 |
| G4 内存界 | pb 20 B/node ±10%；pc ≤4 MB 恒定 | RSS/VM 采样 |
| G5 指令数 | digraph Ir ≤88,029,115（**门禁上限**；当前实测 82,037,532，`tests/reclaim-perf/gate.sh:88`） | callgrind（TAXMAP 方法） |
| G6 六门禁全绿 | tests/reclaim-perf 既有 6 项 | 既有 harness |
| G7 分片数 | SHARDS=1（不得默认开） | 配置断言 |
| G8 档 1 完整性 | 强制 T1 于 chain2 时峰值 ≤1.1× 同形状 T2 | VmHWM（MEAS-T1 §5.3 的方法） |
| G9 任务根完整性 | 在活任务存在时调用 `syspool::collect()`，任务帧仍可驱动、任务数不变 | 新增 tests/zone 用例 + ASAN |

---

## §8 与旧文档/草案的差异清单（哪些断言被推翻）

| 旧断言 | 出处 | 本文件的修正 |
|---|---|---|
| 「默认开计数」 | 若干早期文档 | 默认 **NOCOUNT=1（关）**（codegen.c:13787-13794） |
| 「计数比清扫贵 +100%~+455%」作为普遍结论 | docs/plans/TODO.md | 只在 σ≪σ*_cross 的形状上成立；σ>0.6 时模型预测相反（§3.6） |
| 「EXTC_RECLAIM=0 是纯 GC 税对照」 | 旧口径 | 否：=0 时分配器整体换成 extc_arena_*（§1.1） |
| 「M 有界」 | MEMORY-MODEL-CURRENT 早期版本 | 只是**条件界**：三档里只有 S1/S2 在 A4 成立时有界（§1.2） |
| 「P 有界」 | 同上 | 热路径成立；collect/settle/releaseOld/regionFree 不成立（§1.3，§19.3） |

---

## §9 未标定项与风险（都在等实验）

| # | 项 | 现状 | 影响 | 归属 |
|---|---|---|---|---|
| R1 | c_r 区间 0.117–0.411 太宽 | 只有 3 个形状的 Ir 反推 | T2 交叉点 σ*_cross 在 0.22–0.59 漂移 | E7 |
| R2 | c_t 与 sweep 地板 c_s 的分离 | 由 λ 端点反推，非独立测量 | λ 模型在 r 大时误差进 c_s | E2 |
| R3 | κ（每轮固定开销）20 µs 是占位 | 无实测 | 只影响小 L 形状 | E1 |
| R4 | 浮动垃圾系数 K/b 是保守假设 A5 | 未验证 | σ_min 偏保守（可能高估内存） | E9 |
| R5 | mark 尾巴 5.8×、11.8× | 已观测但未建模 | P99 的尾部解释 | E6 |
| R6 | λ 模型在 pc 上高估 3.6× | §3.3 模型 vs 实测 0.045 vs 0.16 | 对小 L 形状不适用 | E2 |
| R7 | a_peak 5.5 GB/s 只是下界 | pa 形状实测 | ridge 点位置不确定 | E4 |
| R8 | ~~tier1 实测~~ | **已由 MEAS-T1 闭合**（§2.5、§4.1 S3 行） | 只覆盖 3 个形状 × 2 档，自动档触发面未扫全语料 | done |
| R9 | P99 无单点值（只有 3.7 µs 一个形状） | — | 门禁 G1 的普适性 | E5 |
| R10 | 每槽载荷 s̄=80 B 是 digraph 值 | 其他形状 16 B | λ 的归一化基准 | E2 |
| R11 | **大槽（s̄ ≳ 数 KB）的墙未建模**：T2 峰值 = 活集 × 学到的 scale（实测 7.1×，上界 max(2L,floor)×scale），税变成缺页（78% 墙钟），与 c_s 地板无关 | 只有 bf 一个形状（MEAS-T1 §3 C1/C2、§5.4） | 模型在大槽形状上低估 T1，且给不出 T2 的确定性内存界；P 门禁也不能只看 µs/段 | E10 |
| R12 | 停顿分布不可测：插桩无直方图 | [step] 只有 max/total + 4 桶（MEAS-T1 §9.2） | G1 的 p99 无法判，E9 只能给 max | P1-6 |

---

## 附录 A 关键推导

**A.1 内存墙（L3）**：设峰值出现在「下一轮触发瞬间」，此时堆 = 上一轮活集 L + 本轮新分配（含未标记活字节）：
  H = L + (H − L) = L/σ；再加未标记滞后 (K/b)L（A5）⇒ L/σ + KL/b ≤ M ⇒ σ ≥ L/(M − KL/b)。
  等号即最优（T1 取 r = M/L − K/b）。

**A.2 keep-up（L4）**：本轮必须 trace 的活对象数 = L/s̄；每 K 次分配给 b 个 pops，本轮共 (H−L)/K·b 个 pops ⇒ b(H−L)/(K s̄) ≥ L/s̄ ⇒ b(H−L) ≥ KL ⇒ b(1−σ) ≥ Kσ。
  含 c_s 的精确式：把「b 个 pops 的成本」换成「b·c_t + K·c_s」（每 K 次分配还要扫 K 次 sweep 预算），得 σ ≤ (b c_t − K c_s)/(c_t(b+K))。

**A.3 最优性（T1）**：λ(r,b) 对 r 严格递减；可行 r 的上界 r_max(b) = M/L − K/b 对 b 递增 ⇒ 最优在 (b_max, r_max(b_max))；b_max = min(256, floor(P/τ_t)) 由 L5。

**A.4 交叉点（T2）**：解 (c_s r + c_t + κ/L)/(r−1) = c_r ⇒ r(c_r − c_s) = c_t + κ/L + c_r ⇒ r*_cross 如 §3.6。

**A.5 频率**：f = a_m/(H−L) = a_m σ/(L(1−σ))；T1 的 σ* 代入即 f*。

## 附录 B 复现

```bash
cd /home/alphayang/extC_Compiler
python3 docs/analysis/model/roofline.py    # 生成 roofline.png + roofline-table.md，退出码 0，无网络
```

脚本 §7 打印的自检值必须与 §3.5/§3.8 的表一致；不一致以脚本为准（脚本是唯一源）。

## 附录 C 存疑清单（明确列为未验证）

1. 图 (a) 的 mutator 平台 5.5 GB/s 是「分配 only」上界，**不是** digraph 的平台（digraph 自身平台 0.578 GB/s，I_m=1.73 ns/B）。
2. σ_min 的浮动项 (K/b)L 是保守假设，实际浮动可能更大（mark 尾巴），故 M 的建议值是**上界**。
3. tier1（事件档）数字已由 MEASUREMENTS-TIER1 补齐（§2.5 的 T1-1..T1-7）；但只覆盖 3 个形状 × 2 档，自动档在全语料上的触发面未扫。
4. c_r、c_t、κ 的绝对值均在标定中（E1/E2/E7），λ 模型在 pc 上已知高估 3.6×。
5. 单步总预算 EXTC_RC_WORK_BUDGET 默认 0 ⇒ 各预算独立叠加，**无单步总界**（reclaim.c:1001-1013）：本模型的 P 是「各通道分别有界」，不是「总和有界」。

---

## 附录 D 源码行号索引（本文件引用的全部位置）

| 位置 | 内容 | 本文件引用处 |
|---|---|---|
| src/back/codegen.c:13770 / 13794 / 13811-13813 | rcNoCount 默认 1（不计数，只清扫）；EXTC_RC_NOCOUNT=0 覆盖；SWEEPFREE 三态（未设=0 / =0 封死自动档 / =1 强制计数） | §1.1、§3.3、§4.1 |
| src/back/codegen.c:15609-15612 | SWEEPFREE 自动档语义（unitAllAcyclic ⇒ SWEEPFREE=1 且 NOCOUNT=0） | §4.1 |
| src/back/codegen.c:4133-4143 | 分配点单分片发射 shouldSweep/maybeCollect | §6 P0-4 |
| src/back/codegen.c:12119-12151 | extc_ablock / arena alloc / release / destroy | §1.2、§4.1 S1 |
| src/back/codegen.c:12415-12443 | EXTC_RC_REG_MAX 512 region 表 | §6 P1-3 |
| src/back/codegen.c:15608-15611 | 事件档自动判定（unitAllAcyclic ⇒ SWEEPFREE+NOCOUNT=0） | §6 P1-4 |
| src/back/reclaim.c:137-151 | SLAB_BITS 20（1 MiB）、FLOOR 1 MiB、SCALE_MAX 2^40、SCALE_MUL 4 | §1.2、§5.1 |
| src/back/reclaim.c:887-914 | SWEEP_BUDGET 8、SEED_PUSHES/WORDS、MARK_EVERY/WINDOW/STEP/BATCH_MAX | §3.4 L5、§5.1 |
| src/back/reclaim.c:998-1037 | ASSIST_CAP 512、WORK_BUDGET 0（三预算独立叠加）、stepBudget、markAdvance | §3.4 L5、附录 C-5 |
| src/back/reclaim.c:1312 起 | [rt:nocount]：计数入口变 no-op | §1.1、§3.3 T1 |
| src/back/reclaim.c:1960-1969 | abort（marking 位清零、wb 计数） | §1.3-1 |
| src/back/reclaim.c:1970-2019 | regionFree：O(#slab) 次 free ＋（存在跨区登记时）O(占用槽×出边) 的 unlinkChildren ＋ pinDropRegion；abort（:1960）只取消轮次不作排空 | §1.3-1、§6 P0-2 |
| src/back/reclaim.c:2324-2382 | markStep walk（浅栈 + 批量 pop + 预算） | §3.4 L5 |
| src/back/reclaim.c:2451-2491 | releaseOld：无预算标记（`while (r->msp)`） | §1.3-2、§6 P0-3 |
| src/back/reclaim.c:2555-2575 | sweepWord / sweepStep（8 words/步） | §1.2、§3.4 |
| src/back/reclaim.c:2598-2609 | sweep 完成：空扫 scale×4、非空归零、thrUpd | §1.2、§3.4 L3 |
| src/back/reclaim.c:2611-2628 | settle（无界排空，非热路径） | §1.3-3、§6 G2 |
| src/back/reclaim.c:2631-2659 | 触发注释、thrUpd（2×live/floor/scale）、roundOk、shouldSweep | §1.2、§3.4 L3/L6、§5.1 |
| src/back/reclaim.c:2725-2801 | finish / begin / step / collect / maybeCollect（三入口） | §1.3、§3.4 |
| src/back/reclaim.c:2851-2861 | 账号查询 = settle = 无界排空 | §1.3-3 |
| src/back/reclaim.c:2889 起 | 运行时字符串表（[rt:*] 分段）起点 | §1.1 |
| src/back/reclaim.c:2914 | 发射 `#define EXTC_RC_NOCOUNT 1` | §1.1 |

（MEASUREMENTS.md / MEASUREMENTS-TIER1.md / MARK-MODEL.md / SWEEP-CENSUS.md / TAXMAP.md / DIGRAPH.md / FINDINGS.md / MEMORY-MODEL-CURRENT.md 的行号已在 §2.1 与各表内就地标注。）
