# P0 落地模型：字节节奏配额（P）与惰性 freelist（B）的形式化

> task-25（Lead m01364）。用户已决定把 P0 的 P（按字节收费的回收配额）与 B（惰性 freelist）落进编译器，本文件是它们的模型与边界。
> 配套可执行脚本：docs/analysis/model/roofline.py（§6.6 quota 分支，panel (g)(h)，一键重生成 roofline.png / roofline-table.md 的 H/I 段）。
> 硬约束：**不抬 r / thr / M_max**（用户 m03301：「尽可能压低内存占用」）、**无后台线程、无 STW 扩增、对象头不变**。
> 本文件不改 src/、tests/。

## 0 结论（先看这张表）

| # | 结论 | 依据/等级 |
|---|---|---|
| C1 | 配额函数 q(n) 的**名义容量** Q_lin = 64·τ_s/2^k_s + b·τ_t/2^k_m = **4.83 ns/B**，是「每字节一次 trace+一次 sweep」单价 c* = λ(r'=2) = **0.4395 ns/B** 的 **11.0 倍** | [模型] + [实测] τ_t/τ_s（§2.3） |
| C2 | keep-up 允许的最松位移是 **k_s ≤ 11.03、k_m ≤ 11.94**（pops 通道更紧）；原型用 6/10 ⇒ 过配 sweep 32.7×、pops 3.85×。把 k_s 从 6 抬到 11、k_m 从 10 抬到 11 仍满足全部条件，Q_lin 降到 2.20 ns/B | [模型]（§2.3） |
| C3 | 守恒条件成立时，内存界从经验上界变成**真界**：M_peak ≤ M_max + max(L,floor) + **0.686 L** + n_max；比现行契约多出的 0.686 L 是 mark 期在飞垃圾，必须写进契约 | [模型]（§3，T-M2） |
| C4 | 单步停顿 P_step(n) 在原型语义下**没有上界**：n ≥ 1 KiB 就触发 1 个 advance，若它扫满一个 8192-word chunk 就是 **90.1 µs**（实测段 78–98 µs）。**P 不能只加配额、必须同时给 CAP 与 SCAN_CHUNK 上限** | [模型] + [实测 11 ns/word]（§4） |
| C5 | 5 µs 门禁下的可行组合：chunk=256 字 ⇒ b ≤ 125；chunk=128 字 ⇒ b ≤ 205；chunk=64 字 ⇒ b ≤ 246。**推荐 chunk=128 字（1 KiB）+ k_m=10 + b=192**：128 字恰好等于指针密集分配所需的 1/8 word/B 容量下界 | [模型]（§4.4–4.5） |
| C6 | B 把每槽释放成本从 sweep 期（逐槽链写）搬到复用期（每 slab 一次位图扫 + 重建整条链）。拟合的链写占比 s_link = 0.541/0.793/0.103（promote/sizeratio/cycleratio） | [模型拟合] + [原型实测]（§5.2） |
| C7 | B 买的是**停顿尾部**不是吞吐：GC 步总时 −12.5%/−51.7%/−8.4%，单步 max 160.4→57.5、51.8→9.5、39.0→16.9 µs；而墙钟变化 +2.3%/+1.5%/−9.4%/−1.8% 全部落在 ±0.06 ns/B 的噪声底内 ⇒ Δλ 不可判 | [原型实测]（§5.3–5.4） |
| C8 | B 的模型在 cycleratio 上失效（预测 max 比 0.897 vs 实测 0.433）：98% 死槽在 full-dead slab，v5 已有批量重置（P1-7）⇒ 平均 s_link 不代表 max 步。需先做 E26（每步死槽分布）再谈推进 | [模型] vs [原型]（§5.3） |
| C9 | P 对四个 v2 形状吞吐中性（−9.8%/−3.0%/+0.3%/+0.8%），内存不变；对 pd_bigobj 60 轮是 −84% 时间 / −97.6% 内存，200 轮从 1,067,288 kB 降到 26,072 kB | [原型实测]（§3.5） |
| C10 | 待标定项：c_chunk（只有 9.5–12 ns/word 一个口径）、memset 带宽、c_w 与 b_link（决定 B 的洞密度门槛 3.9%–6.0%）、原型 quota 在计数档的覆盖面 | [存疑]（§10） |

一句话：**P 把「回收跟得上分配」从隐含前提变成可签名条件，代价是把内存契约写成 M_max + max(L,floor) + 0.686L + n_max；B 只改善停顿尾部，吞吐中性；两者都不动 r/thr/M_max。**

## 1 口径与符号

证据等级：**[实测]** 本仓 tier2 量具实测（calprof / rdtsc 拆分 / /usr/bin/time）；**[源码]** 可 grep 的常量或逻辑（给 file:line）；**[模型]** 本文件推导；**[估算]** 未标定；**[原型]** FREEZE v5 生成 C 文本补丁实测（不是编译器内实现，见 docs/analysis/P0-PROTOTYPE.md）。

常量（FREEZE v5，来自 tests/reclaim-perf/MARK-MODEL.md:71-73 与 GC-ROOFLINE §3）：c_t = 0.4114 ns/B、c_s = 0.0108 ns/B、F_slab = 1.30、τ_t = 17.49 ns/obj、τ_s = 0.458 ns/slot、b = MARK_BATCH_MAX = 256、K = 64、g = 2.76、s̄ = 42.5 B、W_s = SWEEP_BUDGET = 8 words、FLOOR = 1 MiB、L p50 = 753,504 B、M_max 隐式 = 3L。

推导用的派生量：w_1 = 64·τ_s = **29.31 ns/word**（一个 word 覆盖 64 槽）；A_adv = b·τ_t = **4.477 µs/advance**；c_chunk = **11 ns/word** [实测 9.5–12，MEMORY-MODEL-CURRENT §19.1]。

符号：n=单次分配字节数；q(n)/Q(n)=预付工作量（word 数 / 折算 ns）；c=每字节所需服务能力；k_s/k_m=配额位移指数；CAP_w/CAP_b=每步配额上限；P_step=单分配点停顿；s_link=每槽释放成本里链写的占比；c_f_sweep / c_f_rebuild=B 拆分后的两项。

## 2 配额函数 q(n) 与守恒条件

### 2.1 定义（原型 P0-PROTOTYPE §4 的机械形式化）

sweep 通道：q_s(n) = max(W_s, floor(n / 2^k_s)) words，其中 k_s = 6（即每 64 分配字节多扫一个 word）。
mark 通道：q_m(n) = floor(n / 2^k_m) 个 advance，k_m = 10；每个 advance 多扫一个 EXTC_RC_SCAN_CHUNK 窗口（8192 words = 64 KiB，[源码] reclaim.c EXTC_RC_SCAN_CHUNK）。

Q(n) = w_1·q_s(n) + A_adv·q_m(n)；线性渐近 Q_lin = w_1/2^k_s + A_adv/2^k_m。

### 2.2 守恒条件（T-Q1）

**T-Q1（配额守恒）**：若存在 c > 0 使得对任意窗口 [t_0,t_1] 有
  Σ q(n_i) ≥ c · Σ n_i
（求和遍历窗口内所有分配点），且分配点在不工作时不丢弃预算（H2），则「回收工作量 ≥ 垃圾产生速率」成立，即 P 把 MEMORY-MODEL-CURRENT §18 的 M ≤ M_max + 缓冲 从**经验上界**升级为**真界**（定理见 §3）。

c 的取法：c 必须 ≥ 合法策略空间里的最大单价 λ(r')。触发律 thr = max(2·live, floor)·scale（[源码] src/back/reclaim.c:3185-3218）保证 r' = H/L_prev ≥ 2，故
  c* = λ(2) = (c_t + 2·c_s·F_slab)/1 = 0.4114 + 2·0.02808 = **0.4395 ns/B**。
注意不要取 λ(r_min=1.69) = 0.6306：r_min 是 pops 通道的 keep-up 下界（GC-ROOFLINE §3.6），不是触发律可达的下界；只有当将来放开 thr 到 1.2L 时才需要把 c 抬到 λ(1.2)。

### 2.3 原型的过配倍数与允许的最松位移

Q_lin = 29.31/64 + 4477.4/1024 = 0.4581 + 4.3725 = **4.8305 ns/B** ⇒ 过配 c* 的 **10.99×**（过配 c_A = c_t + c_s·F = 0.4254 的 11.35×）。

过配不是错误，是设计冗余；但要知道冗余在哪里：
- **sweep 通道单独就够**：k_s=6 时每分配字节预付 τ_s = 0.458 ns/B = 1.04× c*。q_s 把「1 word = 64 槽」当成「至少覆盖 64 字节」，而真实槽密度只有 1 槽 / s̄ = 1/42.5 B ⇒ sweep 容量天然放大 42.5×；keep-up 只需 F_slab/(64·s̄) = 4.78e-4 word/B ⇒ 2^k_s ≤ 2092 ⇒ **k_s ≤ 11.03**。
- **pops 通道是紧的一侧**：指针密度 g = 2.76 指针/对象 ⇒ 需求 g/s̄ = 0.0649 pops/B，容量 b/2^k_m ⇒ 2^k_m ≤ 3942 ⇒ **k_m ≤ 11.94**；scan 通道不紧（≤1/8 word/B ⇒ k_m ≤ 16）。
⇒ **k_s = 11、k_m = 11** 仍满足全部守恒条件，Q_lin = 29.31/2048 + 4477.4/2048 = **2.20 ns/B**（5.0× c*），分配点开销降 2.2 倍。

### 2.4 点态条件的锯齿与窗口条件

点态 Q(n)/n ≥ c* 并不处处成立：q_s 带 floor ⇒ Q/n = 29.31·floor(n/64)/n，最坏点 n = 1023 B 时 = 439.7/1023 = 0.430 ns/B（−2.2% vs c*）。但：
(i) n ≤ 512 B 时 W_s = 8 下限兜住（Q/n 从 4.83 单调升到 0.458）；(ii) n ≥ 1024 B 时 mark 通道把 Q/n 抬到 ≥ 2.2 ns/B；(iii) 真实混合以 42.5 B 对象为主 ⇒ 点态 Q/n = 234.5/42.5 = **5.52 ns/B** = 12.6× c*，窗口平均条件轻松成立。
⇒ 结论：**T-Q1 是窗口条件，不是点态条件**；只有「纯 512–1023 B 中等对象、且全是垃圾」这种混合才会让配额低于 c* 约 2–3%（U4）。

## 3 内存界：从经验上界到真界（T-M2）

### 3.1 现状为什么只是经验上界

MEMORY-MODEL-CURRENT §18.3 的第 1 条无界条件就是「垃圾产生速率 > 清扫带宽（每分配最多 8 word = 512 槽）」。当前分配路径**从不拒绝分配**，唯一的动作是加压清扫（pressure ⇒ 每步预算 ×4）与夹 thr；一旦清扫带宽跟不上，thr 的夹取就是空文。反例：tests/gc-p0/src/pd_bigobj.extc 60 轮 493,400 kB、200 轮 1,067,288 kB，而 M_max = 3L ≈ 24 MB（突破 20–44×，[实测] MEMORY-MODEL-CURRENT §18.6）。

### 3.2 T-M2（内存真界）

设 (H1) 窗口守恒 T-Q1 成立，c = c*；(H2) noSweep/owe 不永久置位、roundOk 信用按轮清零；(H3) thr 被夹在 M_max − 64L/b（[源码] reclaim.c:3196-3214 的 _wall − live/4，b=256）。则任意时刻

  bytes ≤ M_max + max(L, floor) + (c_t/c − 64/b)·L + n_max

代值 c = c*、64/b = 0.25 ⇒ **M_peak ≤ M_max + max(L,floor) + 0.686 L + n_max**。

证明骨架（附录 A.1 给细节）：
(i) 任一窗口内产生的可回收垃圾 G ≤ 分配字节 A，回收它所需工作量 ≤ (c_t + c_s·F)·G ≤ c*·A ≤ 窗口累计服务能力 ⇒ 上一轮清扫必定覆盖本窗口垃圾 ⇒ 稳态只有「一轮在飞」的垃圾驻留；
(ii) 一轮在飞期间的峰值 = thr + 一轮内垃圾 ≤ thr + max(L,floor)（roundOk 信用，[源码] reclaim.c:3234-3241）+ (c_t/c)L（mark 期新分配：配额已被上一轮占用，故最多再攒 c_t·L/c 字节）；
(iii) 代入 thr ≤ M_max − 64L/b 得证；n_max 是「触发后单次分配越过 thr」的项。

### 3.3 紧度与收紧路径（依赖 M_max / L / b）

- 与 M_max/b 的关系：c* 与 M_max 无关（**内存目标不改变单价需求**），界里唯一与 M_max 有关的是夹取松弛 64L/b（b=256 ⇒ 0.25L；b=128 ⇒ 0.5L）；**降低 b 同时恶化停顿余量与内存界**（与 GC-ROOFLINE §3.5 一致）。
- 与 L 的关系：加性常数 c_t/c·L = 0.936L，加上 max(L,floor)。故契约的形式必须是「M_max + 常数×活集」，不能写「M_max + 缓冲」。
- 收紧路径：①「先收费后分配」⇒ 消掉 n_max（代价：单次 O(n) memset 与收费叠在同一分配点）；②把夹取从 64L/b 改成 (c_t/c)L ⇒ 消 0.25L；③指针位图把 c_t 降到 ≈0.2 ns/B ⇒ 0.936L 降到 0.45L（省 0.49L）。
- 与 §18.4 的队列形式互补：M_peak = L(1+κ_M·r')（κ_M 实测 0.772/0.825/1.010）是「默认档自发峰值」，T-M2 是「夹 M_max 后的上界」，实际取小者。

### 3.4 数值

L = 753,504 B（p50）、M_max = 3L = 2.26 MB ⇒ 界 = 2.26 + 1.05 + 0.52 = **3.83 MB**（比 M_max 高 1.57 MB，69%）。digraph 1M 的表征是 L_peak = 2.33 MB、实测峰值 6.44 MB：队列形式给 L_peak(1+κ_M r') ≈ 6.3 MB，两者一致（[实测]）。

### 3.5 反例侧的实测（原型）

pd_bigobj 60 轮：base 745.9 ms / 493,400 kB（stime 737.8 ms、minflt 123,065）→ P 113.5 ms / 26,072 kB（stime 12.5 ms、minflt 6,245）；200 轮 base 1,272.9 ms / 1,067,288 kB → 202.4 ms / 26,072 kB（[原型] P0-PROTOTYPE §4.3）。四个 v2 形状中性（[原型] §4.4）。

## 4 停顿界（T-P2）与 CAP/SCAN_CHUNK 的联合选择

### 4.1 公式

  P_step(n) ≤ b·τ_t + (min(CAP_w, q_s(n)) − W_s)^+ · 64τ_s + min(CAP_b, q_m(n)) · chunk_w · c_chunk

原型语义（P0-PROTOTYPE §4 的两处步进）等价于 CAP_w = CAP_b = ∞ ⇒ 第三项无界。任务书里给的 max 形式（P_step ≤ max(固定预算, c_sweep·cap(n), c_mark·cap(n))）只是本式的下界（原型是叠加不是取大）。

### 4.2 「停顿 ∝ 本次分配大小」能接受吗？

不能无条件接受。该次分配自身的 memset 是 O(n)（按 8 GB/s 记 0.125 ns/B [估算]），而 pointer 扫描是 11 ns/word = **1.375 ns/B** [实测] ⇒ 常数比 **11×**：一次大的指针数组分配，GC 扫描比它自己的 memset 贵一个量级。所以「停顿 ∝ n」只在 n ≤ 1 KiB 的小段可接受；n ≥ 数 KiB 必须靠 CAP 截断，否则一个 8 MiB 指针数组分配点就能造出毫秒级停顿，而截断的代价落在内存界上（§4.3 K2）。

### 4.3 5 µs 门禁下的 n 阈值

b=256 的固定预算 P_step = 4.712 µs，余量只有 **0.288 µs = 9.8 个 sweep word**：
- 只开 sweep 配额（不加 CAP）：n > 512 B 后每 64 B 加 29.31 ns ⇒ **n ≤ 1,141 B** 能过门禁；b=224 ⇒ n ≤ 2,363 B；b=192 ⇒ n ≤ 3,585 B；b=160 ⇒ n ≤ 4,807 B。
- 一旦开 mark 配额：n ≥ 1,024 B 就产生 1 个 advance ⇒ 若该 advance 扫满 8192-word chunk，P_step 跳到 **≈95 µs**（90.1 µs 扫描 + 4.7 µs 预算）⇒ **直接破门禁**。
⇒ 必须在同一 patch 里把 SCAN_CHUNK 降下来（这是本文件对 P0 落地最重要的修正项，E23 必须先实测 pd_bigobj 的单步 max）。

### 4.4 chunk 与 b 的联合可行表（b·τ_t + chunk·11ns + 8·64·τ_s ≤ 5 µs）

| chunk（words / KiB） | 扫描成本 | 允许的最大 b |
|---|---|---|
| 8192 / 64 | 90.11 µs | 不可行（负数） |
| 2048 / 16 | 22.53 µs | 不可行 |
| 1024 / 8 | 11.26 µs | 不可行 |
| 512 / 4 | 5.63 µs | 不可行 |
| 256 / 2 | 2.82 µs | 125 |
| 128 / 1 | 1.41 µs | 205 |
| 64 / 0.5 | 0.70 µs | 246 |

### 4.5 CAP 选择准则

- K1（停顿）：64τ_s·CAP_w + CAP_b·chunk_w·c_chunk ≤ P_max − P_fixed。
- K2（容量）：CAP 不得让 keep-up 失效。sweep 侧大对象**不痛**——big payload 在 big slab 里只占 1 槽（[源码] regionFree 里 s->big 单独 free），所以 **CAP_w 取 8–32 足够**；mark 侧才是大对象的钱（指针数组要扫全），所以 **CAP_b 由 chunk 决定**，1–2 个 advance/步。
- K3（chunk 与 k_m 耦合）：每次 advance 只扫一个窗口，容量 = chunk_w/2^k_m words/B 必须 ≥ 1/8 word/B（指针密集上界）⇒ chunk_w ≥ 2^k_m/8。k_m = 10 时下界是 **128 words**。
⇒ **推荐配置：k_s = 10–11、k_m = 10、chunk = 128 words（1 KiB）、b = 192、CAP_w = 8、CAP_b = 1**：停顿 = 3.59 + 0.23 + 1.41 ≈ 4.7 µs（过 5 µs 带），扫描容量恰好 = 需求（1.0×），sweep 容量 5.5× 富余（面板 (h) 的蓝线）。

## 5 B（惰性 freelist）：成本拆分与 λ 重算

### 5.1 拆分

现状：每释放一槽就把槽号头插进 slab->freelist（链值存在 tid[] = 槽号+1，[源码] src/back/reclaim.c:1296 的 extc_rc_freeSlotLink）。B 改成：sweep 只清 occ/rsbit/rscnt 并累加 nfree；分配侧顺序为 freelist 弹出 → bump 未用完 → 否则一次扫 valid & ~occ 重建整条链（[原型] P0-PROTOTYPE §1）。
于是每槽释放成本拆成两项：
- c_f_sweep：sweep 期只清位图元数据（B 之后保留的部分）；
- c_f_rebuild：重建时一次扫整块位图（nwords·c_w），**摊到该次重建后发出的 nfree 个槽**上。
λ_B(r) = λ(r) − s_link·(c_s·F_slab·r)/(r−1) + (nwords·c_w/nfree)/s̄

### 5.2 s_link 拟合（用 GC 步总时的降幅反推）

ΔT_step/T_step^base = −φ_sweep·s_link，φ_sweep = sweep 步时间 / (mark+sweep)（用 build-th 的 rdtsc 拆分：promote 0.231、sizeratio 0.652、cycleratio 0.822、chase 0.002）。代入 ΔT_step = −12.5%/−51.7%/−8.4% ⇒ **s_link = 0.541 / 0.793 / 0.103**。

### 5.3 预测 vs 实测（单步 max）

按「max 步是 sweep 主导」预测 比值 = 1 − s_link：

| 形状 | 预测 max 比 | 实测 max 比 | 误差 | 说明 |
|---|---|---|---|---|
| promote | 0.459 | 0.358（160.4→57.5 µs） | +28% | 低估收益：max 步的链写密度高于平均 |
| sizeratio | 0.207 | 0.183（51.8→9.5 µs） | +13% | 同向，误差可接受 |
| cycleratio | 0.897 | 0.433（39.0→16.9 µs） | **+107%** | 模型失效 |

cycleratio 失效的原因是可以解释的：它 98% 的死槽在 full-dead slab 里（Lead 的 slab 面积探针：651/756 slab 覆盖 97.8% 死槽），而 FREEZE v5 已含 P1-7 的「全死 slab 批量重置」⇒ 平均 s_link 只有 0.103，但 max 步恰恰是那些没被批量重置的散洞步。⇒ **平均 s_link 不能预测 max 步**（需 E26）。

### 5.4 λ 重算与墙钟

Δλ_sweep = −s_link·(c_s·F·r)/(r−1)，r_eff = 2.83 时 c_s·F·r/(r−1) = 0.02171 ns/B：
promote −0.0118、sizeratio −0.0172、cycleratio −0.0022 ns/B；r_eff = 2.4 时 −0.0130/−0.0191/−0.0025。
重建项 (nwords·c_w/nfree)/s̄ 是正的，量级 0.002–0.05 ns/B（取决于 nfree），与上式同阶 ⇒ 净变化在噪声内。
实测墙钟 +2.3%/+1.5%/−9.4%/−1.8%，换成 Δλ 是 **+0.0090 / +0.0022 / −0.0299 / −0.058 ns/B**。注意 chase 的 B 在本形状上几乎不该起作用（sweep ≈ 0），它的 −0.058 ns/B 就是 **harness 噪声底**（WSL2、load 1–3、±5% 墙钟抖动，[原型] §7）。⇒ promote/sizeratio/cycleratio 的墙钟变化都不可判；**B 的吞吐中性结论成立，它的收益是停顿尾部**。

### 5.5 适用范围（什么时候 B 划算）

B 划算 ⇔ 重建成本低于省下的链写成本。单位槽口径的盈亏平衡点 = 洞密度 c_w/(64·b_link)；c_w ≈ 5 ns/word、b_link ≈ 1.3–2.0 ns/slot [估算] ⇒ **洞密度 ≥ 3.9%–6.0%**（1 MiB slab ≈24,672 槽，即 nfree ≳ 960–1,490 槽）。promote/sizeratio 的散洞形状落在门槛附近 ⇒ 收益主要体现为「尾巴变矮」（C7）；cycleratio 靠 P1-7 批量重置拿走大头；chase 类 GC ≈ 0 的形状没有可省的链写 ⇒ 应让 B 自适应关闭（R-Q8）。

## 6 与硬约束的一致性

| 约束 | 是否满足 | 论证 |
|---|---|---|
| 不抬 r/thr/M_max | ✅ | 配额不改 thrUpd（reclaim.c:3185-3218）与夹取；r_policy 仍 =THR_MUL=2；隐式 M_max=3L 不变 |
| 内存只降不升 | ✅ | T-M2 与 §18.4 都是上界；实测 RSS 四形状不变、pd_bigobj −97.6% |
| 无后台线程 / 不扩 STW | ✅ | 配额在分配点同步付费，不新增线程或 stop-the-world；最坏是分配变慢（P0-PROTOTYPE §4.5） |
| 对象头不变 / 单线程 | ✅ | B 只改 slab 的 freelist 表示（tid[] 链 + nfree）；计数与配额仍在 ExtcRcRegion 字段，无原子 |
| 吞吐回归 | ⚠️ | P 四形状 ±3%（−9.8/−3.0/+0.3/+0.8）；B ±0.06 ns/B 噪声内不可判 |

## 7 预测表（FREEZE v5 常量）

| 量 | 公式 | 值 |
|---|---|---|
| c_A / c* | c_t + c_s·F / λ(r'=2) | 0.4254 / **0.4395 ns/B** |
| Q_lin（原型 6/10） | 64τ_s/2^k_s + b·τ_t/2^k_m | **4.8305 ns/B**（10.99× c*） |
| Q_lin（k_s=11,k_m=11） | 同上 | 2.20 ns/B（5.0× c*） |
| k 上界 | k_s ≤ log2(64s̄/F)、k_m ≤ log2(b·s̄/g) | 11.03 / 11.94 |
| 内存界 | M_max + max(L,floor) + (c_t/c*−64/b)L + n_max | 例 L=753,504 B、M_max=3L ⇒ 3.83 MB |
| P_step(b=256) / 纯 sweep 配额的 n 上限 | b·τ_t + 8·64·τ_s / 64(W_s+slack/64τ_s) | 4.712 µs（余量 9.8 word）/ 1,141 B |
| chunk 上限与 b | §4.4 | ≤256 字 ⇒ b≤125；128 字 ⇒ b≤205 |
| B：s_link / Δλ(r_eff=2.83) | ΔT_step/T_step/φ_sweep / −s_link·c_sFr/(r−1) | 0.541/0.793/0.103；−0.0118/−0.0172/−0.0022 ns/B |
| B：单步 max / 墙钟（±0.06 ns/B 噪声） | 实测 | 160.4→57.5、51.8→9.5、39.0→16.9 µs；+2.3/+1.5/−9.4/−1.8% |

## 8 决策准则（可操作）

- **R-Q1 先定 c**：c = c* = λ(2) = 0.4395 ns/B；只有当 thr 允许降到 2L 以下时才需要 λ(r') 重算（c 与 M_max 无关）。
- **R-Q2 抬 k**：k_s 从 6 → 11、k_m 从 10 → 11 仍满足全部守恒条件（余量分别 32.7× → 1.0×、3.85× → 1.0×），Q_lin 从 4.83 降到 2.20 ns/B；**别把 k_m 抬过 11.94**（pops 通道）。
- **R-Q3 降 chunk**：SCAN_CHUNK 从 8192 字降到 ≤256 字，推荐 128 字（1 KiB）；它与 k_m=10 恰好匹配指针密集分配的 1/8 word/B 需求。
- **R-Q4 降 b**：一旦开 chunk 通道，b 必须 ≤205（128 字模型）；推荐 b=192。
- **R-Q5 CAP**：CAP_w=8–32（sweep 侧 big payload 只占 1 槽，不必大），CAP_b=1–2。
- **R-Q6 契约写法**：内存契约写 M_max + max(L,floor) + 0.686L + n_max，不要写「M_max + 缓冲」（少 0.686L）；要更紧就先做「先收费后分配」与精确夹取（§3.3）。
- **R-Q7 B 的推进条件**：先把 s_link 从平均升级为每步分布（E26），确认 max 步是典型 sweep 步；计数档必须补「同一槽只入链一次」的断言 + 218 断言 + ASan（P0-PROTOTYPE §1.4）。
- **R-Q8 B 的自适应**：洞密度 < 3.9% 的形状（含 GC≈0 的 chase）应关 B 或按 slab 自适应，否则重建成本大于链写节省。
- **R-Q9 门禁顺序**：G-Q1（内存）→ G-Q2（停顿，含 chunk 降低）→ G-Q3（中性）→ G-Q4（B 正确性）；G-Q2 是唯一可能因 P 本身而破的门禁。

## 9 标定实验清单（E23–E28）

| 编号 | 测什么 | 怎么测 | 判据 | 优先级 |
|---|---|---|---|---|
| E23 | P 的单步停顿分布 | EXTC_RC_PAUSE=1 + EXTC_RC_SLOWSTEP 跑 pd_bigobj 200 轮，记录 max/p99 与 n 的散点 | p99 ≤ 5 µs、max ≤ 50 µs、>1 ms = 0 | P0 |
| E24 | c_chunk | 扫 chunk ∈ {64,128,256,512,1024,8192}，在指针密集对象上量 ns/word | 现 9.5–12 区间收敛到 ±1 ns/word | P0 |
| E25 | (k_s,k_m) 扫参 | 生成 C 里把两个指数做成可配，扫 {6,8,10,11,12}²，四 v2 形状 + pd_bigobj | pd_bigobj 200 轮 RSS ≤ 3L+8 MiB 且四形状墙钟 ±3% | P0 |
| E26 | B 的每步死槽分布 | sweepStep 内按步统计 dead 槽直方图，重算 max 步而非平均 | cycleratio 的 max 预测误差 ≤ ±25% | P1 |
| E27 | c_w / b_link | 重建路径计时（nwords·c_w 与 nfree 的关系）、链写路径计时 | 洞密度门槛的误差 ≤ ±30% | P1 |
| E28 | 「先收费后分配」 | 把 n_max 项移出（先付费再分配），测大分配点的停顿叠加 | 内存界收紧 0.13L（n_max 项消失）且停顿不变差 | P2 |

## 10 存疑（未验证，禁止当结论用）

- **U1** c_chunk = 11 ns/word 只有 8192-word 段一个口径（78–98 µs），是「扫指针数组 + push 子对象」的混合成本而非纯内存读；chunk 变小后常数会变 ⇒ 面板 (h) 的阈值是量级估计。
- **U2** memset 带宽 8 GB/s（0.125 ns/B）是估算，未在本机实测。
- **U3** 原型配额是否覆盖所有分配入口（含计数档）未验证；生成 C 改的是同一个 extc_rc_alloc，理论上覆盖，但计数档的释放路径（freeSlot/freeSlotCascade/releaseOld）不在配额内。
- **U4** 窗口条件依赖「分配混合以 ≤512 B 对象为主」；中等对象（512–1023 B）为主的工作负载点态配额低 2–3%。
- **U5** T-M2 的 (H2) 在 noSweep/owe 路径下不成立（MEMORY-MODEL-CURRENT §18.3 #2）；配额救不了这两种状态，需要单独门禁。
- **U6** B 的计数档正确性只有静态论证（P0-PROTOTYPE §1.4），218 断言未跑。
- **U7** 噪声底 ±0.06 ns/B 是用 chase 的 −1.8% 墙钟反推的；真实抖动可能更大，结论仍只是「不可判」。
- **U8** P/B 的全部数字来自生成 C 文本补丁（build-lead5），不是编译器内实现；进编译器后必须重跑 G-Q1..G-Q4。

## 11 复现

    python3 docs/analysis/model/roofline.py

产物：docs/analysis/model/roofline.png（4×2，panel (g)(h) 为 quota 分支）、docs/analysis/model/roofline-table.md（H/I 段）、以及 stdout 的 quota 自检块。脚本退出码 0、无网络、不写仓库外路径。

## 附录 A 推导

**A.1 T-M2 的排队论证**。窗口内分配 A 字节、将成垃圾 G ≤ A；回收 G 需 c_t·G + c_s·F·G ≤ (c_t+c_sF)G ≤ c*·A ≤ 窗口服务能力 ⇒ 窗口末上一轮能追平 ⇒ 稳态至多一轮在飞。峰值出现在「上一轮 mark 已花掉 c_t·L 的服务能力、本轮垃圾还在攒」的时刻 ⇒ 驻留垃圾 ≤ max(L,floor) + c_t·L/c*。代入 thr 夹取得 T-M2。

**A.2 Q_lin 的推导**。q_s(n) = n/2^k_s words（大 n 极限），每 word 64 槽 × τ_s ⇒ 每分配字节 = 64τ_s/2^k_s；q_m(n) = n/2^k_m advance，每 advance = b·τ_t ⇒ 每字节 = b·τ_t/2^k_m。两者相加即 Q_lin。

**A.3 k 上界**。sweep 需求：F·(1/s̄) 槽/B ÷ 64 槽/word = F/(64s̄) word/B ≤ 1/2^k_s。pops 需求：g/s̄ pops/B ≤ b/2^k_m。scan 需求：≤ 1/8 word/B ≤ chunk_w/2^k_m。

**A.4 chunk 与 b 的联合界**。P_step ≥ b·τ_t + chunk_w·c_chunk（当至少一个 advance 扫满窗口），解 b·τ_t ≤ P_max − chunk_w·c_chunk − W_s·64τ_s 得 §4.4 的 max b。
