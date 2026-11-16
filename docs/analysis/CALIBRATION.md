# extC GC 模型参数标定（阶段 3 / task-13）

- 状态：草案定稿候选 v1.0，2026-11-09
- 基线：`HEAD 85b8871b`；`src/back/codegen.c` md5 `66c4d9b86d176f80d94d73b2c0ef4075`；`src/back/reclaim.c` 为**工作站未提交修改** md5 `b220c4484fe2bd72a376de71c141f07b`（+108/−4 行）
- 编译器：`make BUILDDIR=build-th -j` ⇒ `build-th/extc` md5 `72c05dff0ef4f9052d145246f0b2d4c3`（patch 前同目录为 `b61c2e2f70237b115c8fdc8288273ea1`）
- 量具：`/tmp/cal/calprof.py`（本轮自建，源码级插桩，见 §2），所有数字来自 `build-th/extc` 生成的 .c 插桩后重编译
- 口径：本文件只做**参数标定**，不改变任何源码语义；每个数字标为 [实测] / [源码] / [推算] / [待标定]
- 与旧文档关系：`docs/analysis/GC-ROOFLINE.md` §2.2 的 `s̄=80B / σ=0.7415 / r=1.349 / H=1.049MB / a_m=0.515GB/s / λ=0.212ns/B` **全部被本轮实测替换**（见 §9），原因逐条记录在 §3/§5。

## §0 结论

1. **旧参数体系基本作废**：实测 `s̄ = 42.50 B`（旧 80）、`σ = L/H = 0.345`（旧 0.7415）、`r = H/L_prev = 2.83`（旧 1.349）、`H_trig p50 = 2.130 MB`（旧 1.049 MB）、`L p50 = 0.7615 MB`（旧 0.7778 MB）、`a_m(digraph 1M) = 0.309 GB/s`（旧 0.515）、`λ_GC = 0.2508 ns/B`（旧 0.212）。
2. **触发律读法正确**：`thr == max(2*max(live,0), floor) * max(scale,1)` 在 28 轮（250k）和 85 轮（1M）上逐行核对**无一例外**（0/113 不符）⇒ `src/back/reclaim.c:2636-2642` 的 `thrUpd` 语义已闭合。`scale` 全程为 0（= 每轮都扫出垃圾）⇒ **空扫 ×4 自学习路径在本语料上从未触发**，`2^40` 上限同理从未参与。
3. **稳态堆 ≈ 3× 活集，且 `3L` 是上界不是等式**：`thr = 2·live`（当 `2·live > floor`）+ `roundOk` 信用 ⇒ `H ≈ L + max(2L, floor) + ε` ⇒ 保守 `M = L(1+r)`。1M：`3·L_peak = 7.00 MB`，实测 `peak = 6.44 MB`；差额来自「标记期间上一轮垃圾正被 sweep 回收」，实测峰值商 `κ_M = (peak−L)/H = 0.772`（250k 0.825、p4 1.010）⇒ **`M = L + κ_M·H`，保守取 κ_M=1 得 3L**（§3.6）。250k：`L < floor/2` ⇒ `H ≈ L + floor`，实测 `H p50 = 1.613 MB` vs `L p50 + floor = 1.479 MB`（+9%）。**内存界的主导项是触发倍数 2，不是 slab 开销。**
4. **roundOk 信用恒被满足且是下界**：`min(H − L_prev) = 1,062,792`（250k）/ `1,105,616`（1M），相对 `floor = 1,048,576` 只有 +14 KB / +57 KB 余量 ⇒ 触发点的**硬下界**就是 `max(L_prev, floor)`，实测几乎不超付；p50 的超额（1.22 / 1.39 MB）来自 `thr=2L` 与批量/扫尾过冲，不是信用失效。
5. **成本函数标定成功**：`λ(r) = (c_t + c_s·F_slab·r) / (r − 1)`，6 个数据集（2 形状 × 2 规模 × 3 重复）误差 **−2.0% ~ 0.0%**（§5.2 表）。旧模型里的 `κ`（20 µs 占位）实测应为 **0**：不加任何额外项即吻合，`κ` 项删除。
6. **拆出的成本常量**：`c_t = 0.411 ns/被 trace 的载荷字节`（250k 0.379）、`τ_t = 17.5 ns/被 trace 对象`（250k 16.1、p4 6.8）、`c_s = 0.0108 ns/被 sweep 的 cap 字节`（250k 0.0116、p4 0.021）、`τ_s = 0.458 ns/槽`（250k 0.495、p4 0.66）、`F_slab = cap/H = 1.30`（250k 1.26、p4 1.05）。
7. **停顿模型吻合**：`P ≈ b·τ_t + 8·64·τ_s = 4.71 µs`（b=256）vs 三 rep 的 mark 步 p99 `4.36 / 4.79 / 6.42 µs`（中位 4.8，±10%）。但 **`max` 不能用于门禁**：三 rep 的 mark 步 max 分别 `170 / 52.5 / 490 µs`（9× 散布）——进程内计时包含被调度走的时间，WSL2 上纯噪声。
8. **trace-rate 墙未撞**：`min r' = 2.425`（1M）/ `3.23`（250k）vs 理论 `r_min = 1 + gK/b = 1.690` ⇒ 裕度 **1.44×**；`σ_prev = L_prev/H` 最大 0.412 vs `σ_max = b/(b+gK) = 0.592` ⇒ 同裕度。旧结论「唯一硬墙 σ_max=0.800、余量 1.08×」被彻底推翻。
9. **σ 定义坑（曾导致误判，必须写进任何引用）**：`σ = L_current/H`（本轮 marked）与 `σ_prev = L_prev/H`（上轮 live，驱动 roundOk 与 mark 推进）不是一回事。1M 的 `σ_current` p99 = **0.590**，恰好接近 0.592 曾让人以为撞墙；实际约束量是 `r' = H/L_prev`，其最小值 2.425 ≫ 1.69。**任何 σ 墙论断必须用 σ_prev。**
10. **S1（arena）平台实测下界 `a_peak ≥ 10.2 GB/s`**：`spectrum_p1`（512 B 对象 × 200k 次 = 102 MB）纯 arena 墙钟 0.01 s，RSS 1.8 MB；`spectrum_p3`（16 B 结点 × 200k 轮 ≈ 192 MB）0.03–0.04 s ⇒ 5–6 GB/s。digraph 1M 的 `a_m = 0.309 GB/s` ⇒ 距平台 **33×**，**digraph 是纯 mutator 受限，GC 税 7.8% 不是主因**。
11. **GC 税 = λ·a_m**：250k 的 λ 更小（0.1433）但 `a_m` 高 3.4×（1.053 GB/s）⇒ 税 13.1% 反而比 1M 的 7.8% 高。同一 GC 策略在快 allocator 形状上税翻倍——这是 Roofline 上「向黄铜区移动」的直接证据。
12. **计数 vs 清扫的交叉点被量化**：`r_cross = 1 + c_t/c_r`。若 `c_r ≈ 0.23 ns/B` 则 `r_cross = 2.79`，与当前有效 `r = 2.83` **几乎重合**；`c_r = 0.117` ⇒ 2.09（计数赢）；`c_r = 0.411` ⇒ 2.00（清扫赢）。`c_r` 尚无直接实测（§11#1），但与 `TAXMAP.md` 的 Ir 口径（计数贵 +100.9%~+455.2%，即 2.0–5.6×）方向一致 ⇒ **默认 `EXTC_RC_NOCOUNT=1` 合理**，除非把 `c_r` 压到 ≤ 0.23。
13. **patch 影响可忽略**：`reclaim.c` 的 SCAN_CHUNK 65536→8192 与 count-zero cascade 都不改变默认档轮结构——patch 前后 1M 的 `rounds=85`、`alloc_bytes=135,901,368` **逐字节相同**，时间差在噪声内。cascade 整段在 `#if !EXTC_RC_NOCOUNT` 内，默认档根本不编译。
14. **250k→1M 的 a_m 下降 3.4×（1.053→0.335 GB/s）不是 GC 变慢**，mutator 自身的 cache 行为变差（同形状 4× 规模）；GC 时间占比反而下降（13.1%→7.8%）。任何「大堆更慢」的结论必须扣掉这一项。
## §1 环境、构建与探测开销

### 1.1 两套编译器（patch 前后）

| 项 | patch 前 | patch 后 |
|---|---|---|
| `build-th/extc` md5 | `b61c2e2f70237b115c8fdc8288273ea1` | `72c05dff0ef4f9052d145246f0b2d4c3` |
| `src/back/reclaim.c` md5 | `d8ea376c88f78655cf56cccbcdba38e4` | `b220c4484fe2bd72a376de71c141f07b` |
| digraph 1M `rounds` / `alloc_bytes` | 85 / 135,901,368 | 85 / 135,901,368 |
| digraph 1M `Σmark_ns` | 31,560,000（pre 测 4.10 ms @250k、31.48 ms @1M 量级） | 31,190,000 |

patch 两处（均不改变本标定口径）：

1. `count-zero cascade`（`extc_rc_freeSlotCascade`，按 `#if !EXTC_RC_NOCOUNT` 编译）⇒ 默认档 `EXTC_RC_NOCOUNT=1` **根本不生成该代码**。
2. `EXTC_RC_SCAN_CHUNK` 65536 → **8192**（注释：扫描带宽约 1.1 ns/pointer，单步上界从 ~72 µs 降到 ~9 µs；promote 开销 65536→+0.20%、16384→+0.59%、8192→+0.99%、1024→+3.6%）。它只影响「一个保守扫描段」的时长，不影响轮数与标记总量。

⇒ 本文件所有数字对 patch 前后都成立；引用时以 patch 后 md5 为准。

### 1.2 形状与档位（重要结构发现）

| 形状 | 源 | 是否含 `ExtcRcRegion` | rc 轮次 | 用途 |
|---|---|---|---|---|
| `bench/gc/src/spectrum_p1.extc` | 512 B 对象 × 200k | **否（纯 arena）** | 0 | S1 平台 `a_peak` |
| `spectrum_p2.extc` / `p3.extc` | 16 B 结点 × 200k 轮 | **否（纯 arena）** | 0 | S1 平台 |
| `spectrum_p4.extc` | human_a，长命符号表 | 是 | 46（2k）/186（8k） | 高 `r`、低 `σ` 端 |
| `bench/gc/src/digraph.extc` | 图，边/结点 ≈ 2.2× | 是 | 28（250k）/85（1M） | 主标定形状 |

**含义**：`spectrum_p1/p2/p3` 不能用来说明 rc 成本（它们根本没有 region），但正好给出 S1 的实测吞吐。此前把它们当 rc 形状用的任何结论都要撤回。

### 1.3 探测开销（必须报告）

- 插桩只加 `clock_gettime(CLOCK_MONOTONIC)` 与数组写入：每轮 2 次、每步 1 次；1M 共 85 轮 + 12,212 步 ⇒ 约 1.2 万次计时，相对 456 ms 墙钟 < 0.2%。
- 计时块本身依赖 `EXTC_RC_PAUSE=1`（`reclaim.c:2668-2721` 的 pauseDump 同一开关），故所有测量都在 `EXTC_RC_PAUSE=1` 下做；该开关只增加计时，不改回收逻辑。
- 关掉 `EXTC_RC_PAUSE` 的对照运行给出相同 `rounds/alloc_bytes/peak`（§1.1）⇒ 轮结构不受量具影响。

## §2 量具 `calprof.py`

位置 `/tmp/cal/calprof.py`（**不进仓库**；复现见 §13）。用法：

```
python3 calprof.py gen.c gen.cal.c            # 读 argv[1] 写 argv[2]
env EXTC_CAL=/tmp/cal/x EXTC_RC_PAUSE=1 taskset -c 6 ./x.cal 1000000 20261013
# => /tmp/cal/x.cal.csv（每轮一行）+ stderr 的 CAL-summary / CAL-peak / CAL-pause 行
```

插桩点（行号指**生成物** `digraph.c`，不是 `reclaim.c`）：

| 位置 | 插入调用 | 记录 |
|---|---|---|
| `extc_ablock` typedef 后 | `static void cal_alloc(long long ss, long long cur);` 前向声明 | — |
| 运行时头部 | `cal_*` 函数体 + `clock_gettime` 包装 | 全局/每轮 peak、per-step 数组 |
| alloc 处 `r->bytes += (int64_t)ss;` 之后 | `cal_alloc(ss, r->bytes);` | 累计分配字节、当前堆、峰值 |
| `extc_rc_begin` 开头 | `cal_round_start(r);` | bytes/thr/live_prev/scale/floor/cap_bytes/slabs |
| `extc_rc_finish` 末尾（`thrUpd` 之后） | `cal_round_end(r);` | marked_bytes/marked_objs/mark_ns/round_ns/peak_bytes/sweeps |
| `extc_rc_markStep` 的 pause 计时块后 | `cal_mark_step(_d, marked, _hot);` | 每个 mark 步时长（ns） |
| `extc_rc_sweepStep` 的 pause 计时块后 | `cal_sweep_step(_d, _sw);` | 每个 sweep 步时长 |

CSV 列：`round,bytes,thr,live_prev,scale,floor,allocb,cap_slots,marked_bytes,marked_objs,mark_ns,round_dur_ns,cap_bytes,slabs,peak_bytes,sweeps,start_ns`。

**局限（影响结论强度，必须一起引用）**

1. 量的是**进程内墙钟**，在 WSL2 上会被调度抢占污染 ⇒ `max` 不可用（§7），`p99` 也不稳（±40%）。
2. 每轮 `mark_ns` 是该轮所有 mark 步之和，粒度到轮；单步分布靠 `CAL-pause` 行（内存里排序，上限 1<<20 个样本）。
3. `cap_bytes = Σ nslots·ss` 是**本轮结束时的存活 cap**，用作 sweep 分母；sweep 实际扫的是「上一轮结束时的 cap」，两者差一轮，可接受（cap 变化 < 5%/轮）。
4. 不测量 mutator 内部时间归属（那是 `perf`/`callgrind` 的事）。

## §3 E6：`L / H / thr / σ / r` 的相位口径（最先做、影响面最大）

### 3.1 问题

旧文档用 `L = 9723 槽 × 80 B = 777,840 B` 与 `H_trig = 1,049,000 B` 算出 `σ = 0.7415, r = 1.349`。但触发律 `thr = max(2·live, floor)` 在 `2·live > floor` 时要求 `H > 2L + L` ⇒ `r > 3`；`r = 1.35` 在代码上**不可能长期成立**。两个 p50 出自不同相位/不同规模，不能相除。

### 3.2 方法

逐轮记录 `(live_prev, marked_bytes, bytes@trigger, thr, floor, scale)`，并逐行验证恒等式；对 3 个重复运行做一致性检查（轮结构应完全确定）。

### 3.3 原始数据（digraph，默认档，`EXTC_RECLAIM` 未设 ⇒ 开；`EXTC_RC_NOCOUNT` 未设 ⇒ 1）

| 指标 | 250k（28 轮） | 1M（85 轮） |
|---|---|---|
| `alloc_bytes` | 33,198,024 | 135,901,368 |
| `alloc_n` | 781,661 | 3,199,093 |
| `L_prev = live_prev` p50 | 390,400 | 753,504（p90 1,697,288 / max 2,240,968） |
| `marked_bytes` p50 | 430,624 | 761,544（p90 1,762,912 / max 2,332,880） |
| `marked_objs` p50 | 10,151 | 17,946（p90 41,444 / max 54,890） |
| `bytes@trigger` p50 | 1,613,192 | 2,130,240（p90 4,123,312 / max 5,440,624） |
| `thr` p50 | 1,048,576（=floor） | 1,507,008 |
| `r = H/L_prev` p50 / min | 4.13 / 3.225 | 2.83 / **2.425** |
| `σ = L_current/H` p50 / p90 / p99 | 0.269 / 0.328 / — | 0.345 / 0.495 / **0.590** |
| `σ_prev = L_prev/H` p50 / max | 0.242 / 0.310 | 0.354 / **0.412** |
| `cap/H` p50 | 1.26 | 1.30 |
| `peak`（全程最大 `r->bytes`） | 2,145,816 | 6,439,536 |
| `min(H − L_prev)` | 1,062,792 | 1,105,616 |
| `s̄ = Σmarked_bytes/Σmarked_objs` | 42.49 B | **42.50 B** |
| `scale` 取值集合 | {0} | {0} |

p4（另一形状，用于交叉验证）：`L p50 = 9,536 / 12,256`、`H p50 = 1,062,176 / 1,064,832`、`σ = 0.009 / 0.012`、`r = 105.4 / 86.9`、`cap/H = 1.05 / 1.04`、`s̄ = 32.00 B`、`min(H−L_prev) = 1,048,576`（= floor，一字节不差）。

### 3.4 三个已验证的结构事实

1. **恒等式**：`thr = max(2·live_prev, floor)·max(scale,1)`，逐行核对 0/28 与 0/85 不符 ⇒ `thrUpd` 读法正确，`live` 就是上一轮 `markedBytes`（`reclaim.c:2731`），`floor=1MiB`（`reclaim.c:140-141`）。
2. **`scale` 恒 0**：由于每轮都 `swFreed > 0`（`reclaim.c:2598-2609` 的 `else r->scale = 0`），空扫退让分支**在本语料上不活跃**。任何「空扫 ×4 导致堆暴涨」的推理在本语料上无法验证（要构造全存活形状，见 §12 E9）。
3. **`min(H − L_prev) ≈ floor`**：250k 超付 14,216 B（1.4%）、1M 超付 57,040 B（5.4%）⇒ `roundOk` 的信用额 `max(live, floor)` 是触发点的**硬下界且几乎被精确执行**；p50 的额外膨胀（`H−L_prev` = 1.22 / 1.39 MB）来自 `thr = 2L` 与扫尾过冲，不是信用失效。

### 3.5 结论：自洽的一组参数

以 1M 为代表（p50）：

```
L = 753,504 B (0.719 MiB)      H = 2,130,240 B (2.032 MiB)
sigma = 0.345 (L_current/H)    r = 2.83 (H/L_prev)
thr = 1,507,008 B              floor = 1,048,576 B
s_bar = 42.50 B               cap/H = 1.30
```

**并且 `r` 不是常数**：它由 `thr = 2·L_prev` 与 `roundOk` 共同决定，p90 = 5.72（活集小的轮里 floor 主导）、min = 2.425（`thr` 恰好刚过 floor 的轮）。任何用「单一 r」的模型都必须声明取 p50。

### 3.6 峰值商 κ_M：为什么「M = 3L」是上界而不是等式

`calprof` 记录每轮的 `peak_bytes`（`cal_alloc` 里 `cur = r->bytes` 的滚动最大值）。取「活集最大的那一轮」（1M 恰好也是峰值轮）：

| 形状 | L_prev（峰值轮） | H（触发字节） | 实测 peak | κ_M = (peak − L)/H | peak/L |
|---|---|---|---|---|---|
| digraph 1M | 2,240,968 | 5,440,624 | 6,439,536 | **0.772** | 2.87 |
| digraph 250k | 586,160 | 1,890,520 | 2,145,816 | **0.825** | 3.66 |
| spectrum_p4 8k | 12,256 | 1,062,176 | 1,085,152 | **1.010** | 88.5 |

读法：标记期间上一轮的垃圾**正在被 sweep 回收**，所以峰值只保留了 `H` 的 77%–101%（p4 活集远小于 floor，回收不了多少 ⇒ 趋近 1）。因此

`M = L + κ_M·H`，κ_M ∈ [0.77, 1.01]（本机三形状）；**保守取 κ_M = 1 ⇒ M = L(1+r) = 3L**（`2L > floor` 区）。

对预算规划的含义：给定 `M_max` 时：

- **保守（保证不超）**：`r ≤ M_max/L − 1`（即 κ_M=1）；
- **典型（κ_M=0.772）**：`r ≤ (M_max/L − 1)/0.772`（≈ 保守值 × 1.30）。

1M 的 `L_peak = 2.333 MB`：`M = 7.0 MB` ⇒ 保守 `r≤2.00`、典型 `r≤2.59`；实测峰值轮 `r = 2.43` @ `peak = 6.44 MB`（`2.43 ≤ 2.59`，且 `6.44 = L + 0.772·H` 精确成立），与典型口径一致。**λ 必须用工作点的 r 算，不能用保守 r* 算**（否则 λ(2.00)=0.439 会与实测 0.2508 差 75%，纯属口径错误）。

## §4 E2：把 `c_t` 与 `c_s` 分开

### 4.1 问题

旧 `c_t = 0.18 ns/B` 是从 `τ_t = 14.4 ns/obj` 与 `s̄ = 80 B` **推算**的（`0.18 = 14.4/80`），不是实测；而 `s̄` 本身错了（80 → 42.5）⇒ `c_t` 旧值纯属巧合地接近真值。必须用「Σmark_ns / Σmarked_bytes」直接测。

### 4.2 方法

对每个数据集：`c_t = Σmark_ns / Σmarked_bytes`（每次标记的字节成本）、`c_s = Σsweep_ns / Σcap_bytes`（每字节存活容量的清扫成本）、`τ_t = Σmark_ns / Σmarked_objs`、`τ_s = Σsweep_ns / Σsweep_slots`。分母全部来自插桩记录，不做任何推算。

### 4.3 结果（8 个数据集）

| 数据集 | `c_t` ns/B | `c_s` ns/B | `τ_t` ns/obj | `τ_s` ns/slot | `F_slab` |
|---|---|---|---|---|---|
| dg250-pre | 0.3794 | 0.0117 | 16.12 | 0.500 | 1.26 |
| dg1M-pre | 0.4153 | 0.0106 | 17.65 | 0.452 | 1.30 |
| dg250-post | 0.3790 | 0.0116 | 16.10 | 0.495 | 1.26 |
| dg1M-post | **0.4114** | **0.0108** | **17.49** | **0.458** | **1.30** |
| dg1M-rep1 | 0.4142 | 0.0105 | 17.61 | 0.448 | 1.30 |
| dg1M-rep2 | 0.4570 | 0.0121 | 19.42 | 0.514 | 1.30 |
| p4-2k | 0.2128 | 0.0210 | 6.81 | 0.673 | 1.05 |
| p4-8k | 0.2367 | 0.0206 | 7.57 | 0.660 | 1.04 |

### 4.4 结论与误差

- **`c_t` 是 0.38–0.46 ns/B（digraph），跨 rep 变化 ±10%**；p4 只有 0.21–0.24。差异主要来自**指针密度**而非对象尺寸：p4 是长命符号表、几乎无子指针（`τ_t` 6.8 ns/obj），digraph 每对象约 2.76 个指针（`τ_t` 17.5 ns/obj）。**`c_t` 不能写成只依赖 `s̄` 的函数**，保守取 0.41，标注 −50%/+15% 不确定度。
- **`c_s` 稳定在 0.0106–0.0121 ns/B**（p4 0.021，因其 cap 里垃圾占比极低、每个字都要读位图）。取 `c_s = 0.011 ± 0.001`。
- `F_slab = cap/H = 1.30`（1M）是**内存乘数**，不是成本项：每轮扫的是 `1.30×H`，这是 `λ` 公式里乘 `r` 的原因（§5.2）。
- 误差来源：`mark_ns` 包含 `pauseNow()` 两次调用（~20–40 ns/步）；1M 每轮 143.7 步 ⇒ 每轮 +3–6 µs，对 300 µs 的轮 = 1–2%。

## §5 E3：`λ` 的绝对标定与成本函数

### 5.1 定义

`λ_GC = (Σmark_ns + Σsweep_ns) / alloc_bytes`（ns per allocated byte，**不**只是 per traced byte）。这是 GC 每分配一个字节要付的时间。

| 数据集 | `λ_GC` ns/B | GC 占总墙钟 |
|---|---|---|
| dg250-post | 0.1433 | 13.11% |
| dg1M-post | **0.2508** | **7.76%** |
| dg1M-rep1 | 0.2519 | 7.67% |
| dg1M-rep2 | 0.2788 | 7.66% |
| p4-2k | 0.0245 | 9.55% |
| p4-8k | 0.0245 | 7.76% |

### 5.2 成本函数（本轮最重要的模型修正）

每轮：标记成本 `= c_t·L`（标记整个活集），清扫成本 `= c_s·cap = c_s·F_slab·H`，本轮分配量 `= H − L_prev ≈ (r−1)·L`。故

```
lambda(r) = (c_t + c_s * F_slab * r) / (r - 1)
```

代入 `c_t=0.4114, c_s=0.0108, F_slab=1.30`：

| 数据集 | `r` p50 | `λ_model` | `λ_GC` 实测 | 误差 |
|---|---|---|---|---|
| dg250-post | 4.13 | 0.1404 | 0.1433 | **−2.0%** |
| dg1M-post | 2.83 | 0.2468 | 0.2508 | **−1.6%** |
| dg1M-rep1 | 2.83 | 0.2479 | 0.2519 | −1.6% |
| dg1M-rep2 | 2.83 | 0.2744 | 0.2788 | −1.6% |
| p4-2k | 105.43 | 0.0242 | 0.0245 | −1.2% |
| p4-8k | 86.89 | 0.0245 | 0.0245 | **0.0%** |

⇒ **误差 −2.0%~0.0%，无需任何额外项**。旧模型的 `κ`（20 µs 占位）实测为 **0**，应删除；同时旧模型「高估 19%」的说法作废——那是用了错误的 `s̄=80` 与 `σ=0.7415` 反推的结果。

### 5.3 由 `λ` 直接得到的 GC 税

`tax = λ_GC · a_m`（`a_m` = 扣掉 GC 时间的分配率）。1M：`0.2508 × 0.335 = 8.4%`（实测 7.76%，差 8%，因 `a_m` 与税互相定义）。**这就是「同策略、快 mutator 税更重」的量化形式**：250k 的 λ 只有 0.57×，但 a_m 高 3.1× ⇒ 税 1.7×。

## §6 E5：`τ_t` / `τ_s` 的跨规模与跨形状稳定性

| 维度 | `τ_t` ns/obj | `τ_s` ns/slot | 说明 |
|---|---|---|---|
| digraph 250k | 16.10 | 0.495 | 活集 0.43 MB |
| digraph 1M | 17.49 | 0.458 | 活集 0.76 MB，+8.6% |
| digraph 1M rep2 | 19.42 | 0.514 | 机器状态更差（整体时间 +11%） |
| p4 2k | 6.81 | 0.673 | 活集 10 KB、指针密度极低 |
| p4 8k | 7.57 | 0.660 | 同上 |

结论：

- `τ_t` 的主体是「每对象固定开销 + 子指针扫描」，故随**指针密度**而非对象尺寸变化：digraph 每对象 2.76 个指针（`MARK-MODEL.md` 结构计数）⇒ 17.5 ns；p4 几乎无指针 ⇒ 6.8 ns。建模时 `τ_t` 必须按形状标定，不能跨形状复用。
- `τ_s` 与对象尺寸/活集关系弱（0.45–0.67），取 0.458（digraph）作为默认，误差 ±15%。
- **同形状 250k→1M 的 `τ_t` 上升 8.6%**，与「per-object cost 随堆增大数倍」的旧文档线索方向一致但幅度小得多（`docs/plans/TODO.md` 的「数倍」应指 mutator 侧，§0#14）。

## §7 E1：停顿时长（patch 后）

### 7.1 原始数据（`EXTC_RC_PAUSE=1`，进程内计时，单位 µs）

| 运行 | mark p50 | mark p99 | mark max | sweep p50 | sweep p99 | sweep max |
|---|---|---|---|---|---|---|
| 1M post | 2.42 | **4.36** | 170.2 | 0.23 | 0.43 | 26.3 |
| 1M rep1 | 2.42 | 4.79 | 52.5 | 0.23 | 0.41 | 12.5 |
| 1M rep2 | 2.53 | **6.42** | 489.8 | 0.24 | 0.48 | 214.7 |
| 250k post | 2.21 | 3.41 | 34.3 | 0.30 | 0.45 | 21.8 |

另外：每轮 mark 总时长 p50 = 0.30–0.32 ms，max = 1.08–1.45 ms（1M）。**这不是单步停顿**，是「一轮标记被摊到 143.7 步」的累计值；契约入口 `extc_rc_collect` 会一次排空整轮 ⇒ 那才是 ~1 ms 级停顿（`MEMORY-MODEL-CURRENT.md` §19.3）。

### 7.2 与模型对照

```
P(b) = b * tau_t + 8 * 64 * tau_s        # 每步：b 个 pop + 最多 8 个 word 的扫描
     = 256 * 17.49 + 8*64*0.458
     = 4.478 + 0.234 = 4.71 us
```

vs 实测 mark 步 p99 中位 4.8 µs ⇒ **误差 −2%**。模型成立。

### 7.3 结论与门禁含义

- 单步停顿模型 `P ≈ b·τ_t + W_sweep·64·τ_s` 可用于设计；`b` 是唯一可调放大项（`b ≤ 256` 由 `MARK_BATCH_MAX` 硬限）。
- **`max` 不可用于门禁判定**：三 rep 的 max 相差 9×，且 `EXTC_RC_SLOWSTEP`/`[work]` 打印的 max 同源。`GC-ROOFLINE.md:463` 的 G1「max ≤ 50 µs」在本机（WSL2）**不可判定**；应改用 p99（需直方图，`calprof` 的 `CAL-pause` 行即是）或 `perf sched` 外挂测量。
- p99 本身不稳定（4.36/4.79/6.42）⇒ 门禁应写成 `p99 中位数 ≤ 5 µs 且三次 rep 的 p99 ≤ 7 µs`，或先排除抢占（`taskset` 已用，仍不够；WSL2 无 RT 调度）。
- 当前 `b=256` 给出 p99 ≈ 4.7–6.4 µs，**贴着 5 µs 门禁**；若门禁收紧到 4 µs，须 `b ≤ 4e-6/17.5e-9 ≈ 228`。

## §8 trace-rate 墙的独立确认，以及 σ 定义的坑

### 8.1 推导

每个 `markAdvance` 步（每 `MARK_EVERY=64` 次分配调用一次）给出 `b = 256` 个 pop（预算：`64×8 + min(debt,512)`，被 `MARK_BATCH_MAX=256` 截断）。一步能处理的对象 ≈ `b` 个；一个对象平均推 `g = 2.76` 个指针（`MARK-MODEL.md` 结构计数：nptr/marked ≈ 2.76）。窗口内分配量 `H − L_prev` 提供 `(H−L_prev)/(K·s̄)` 步，故**可 trace 的推入数**

```
pops_avail = b * (H - L_prev) / (K * s_bar)
pushes_need = g * L_prev / s_bar
可行 <=> b*(r'-1)/K >= g   <=>   r' >= r_min = 1 + g*K/b = 1 + 2.76*64/256 = 1.690
```

等价地 `σ_prev ≤ b/(b+gK) = 256/432.64 = 0.592`。

### 8.2 实测对照

| 量 | 250k | 1M | 判据 | 结论 |
|---|---|---|---|---|
| `min r' = H/L_prev` | 3.225 | **2.425** | `≥ 1.690` | 裕度 **1.44×**（1M）/ 1.91×（250k） |
| `max σ_prev` | 0.310 | **0.412** | `≤ 0.592` | 裕度 1.44× |
| `σ_current` p99 | — | 0.590 | （无关） | **不要用这个量** |

### 8.3 坑（写进任何后续文档）

`σ = L_current/H`（本轮标记结果/触发堆）在 1M 上 p99 = 0.590，与 `σ_max = 0.592` 只差 0.3%——**纯巧合**。驱动 `roundOk` 与 `markAdvance` 的是**上一轮** `live`，即 `σ_prev`；`σ_current` 可以超过 `σ_max` 而系统仍然健康（活集在标记期间变大，本轮 marked 超过上轮 live）。用 `σ_current` 判墙会得到「余量 1.003×、岌岌可危」的错误结论；用 `σ_prev`/`r'` 得到「余量 1.44×、健康」。
## §9 参数替换表（旧 → 新）

| 参数 | 旧值（`GC-ROOFLINE.md` §2.2） | 新值 | 依据 | 变化 |
|---|---|---|---|---|
| `s̄` 平均对象字节 | 80 | **42.50** | §3.3 | −47% |
| `L` 活集 p50 | 777,840 | **753,504** | §3.3 | −3% |
| `H_trig` 触发堆 p50 | 1,049,000 | **2,130,240** | §3.3 | +103% |
| `σ = L/H` | 0.7415 | **0.345**（`σ_prev` 0.354） | §3.3 | −53% |
| `r = H/L_prev` | 1.349 | **2.83** | §3.3 | +110% |
| `τ_t` | 14.4 ns/obj | **17.49**（250k 16.10） | §4.3 | +21% |
| `τ_s` | 0.63 ns/slot | **0.458**（250k 0.495） | §4.3 | −27% |
| `c_t` | 0.18 ns/B | **0.4114**（250k 0.379） | §4.3 | +129% |
| `c_s` | 0.045（0.039–0.054） | **0.0108**（0.0105–0.0121） | §4.3 | −76% |
| `c_r` | 0.23（0.117–0.411） | **保持** 0.23（0.117–0.411） | 无新实测，§11#1 | — |
| `κ` | 20 µs（占位） | **0** | §5.2 | 删除 |
| `a_peak` | 5.5 GB/s | **≥ 10.2 GB/s**（下界） | §0#10 | +85% |
| `a_m`(digraph 1M) | 0.515 GB/s | **0.309**（`a_all`）/ 0.335（`a_mut`） | §5.1 | −40% |
| `λ_GC`(digraph 1M) | 0.212 ns/B | **0.2508** | §5.1 | +18% |
| `F_slab = cap/H` | 无 | **1.30**（250k 1.26） | §4.3 | 新增 |
| `g` 每对象推入数 | 无 | **2.76** | `MARK-MODEL.md` 结构计数 | 新增 |
| `r_min` | 无 | **1.690** | §8.1 | 新增 |
| `M_min` | `1.5·L` | **`(1+r)·L`（保守上界）/ `L+0.8·H`（典型）**；digraph 1M 分别为 7.0 / 5.9 MB，实测 6.44 | §0#3、§3.6 | +100% |

## §10 对现有文档与策略结论的影响

1. **「唯一硬墙 `σ_max=0.800`、余量 1.08×」作废**（`GC-ROOFLINE.md` §0/§5/§6）。真实约束有两条：**内存** `M ≈ (1+r)L`（digraph 1M 需 7.0 MB，实测 6.44）与**停顿** `P ≈ 4.7 µs`（门禁 5 µs，几乎贴线）；`trace-rate` 只是 1.44× 裕度的次级约束。
2. **图 (a) 的落点全部移动**：digraph 1M 从 `(λ=0.212, a=0.515)` → `(0.2508, 0.309)`；p4 从无 → `(0.0245, 3.43)`；新增 S1 平台点 `a_peak=10.2 GB/s`（λ→0）。旧图里「离平台 9.1×」应改为 **33×**（0.309 vs 10.2）。
3. **图 (b) 的 σ 可行带需重画**：内存侧给出 `σ ≳ 1/(1+r)`，当前 `r=2.83` ⇒ `σ ≥ 0.26`（p50 实测 0.345 ✓）；`trace-rate` 侧 `σ_prev ≤ 0.592`，**不紧**。旧图上 0.737–0.800 的窄带完全不存在。
4. **推荐触发参数不变（r=2，即 `thr=max(2L,floor)`），但理由变了**：不是「σ 余量只有 1.08×」，而是「在 `M≈3L` 的内存预算下 `r=2` 已经取到可行下界」。**要降 GC 税只能换内存**，且必须用工作点的 `r_eff` 算 λ（附录 B.5）：`M=12 MB` ⇒ 峰值轮 `r≤5.37`（典型）、`r_eff=5.77`、`λ=0.103`（税 3.2%，相对当前 7.8% **−59%**）；`M=21 MB` ⇒ `r≤10.36`、`r_eff=10.76`、`λ=0.058`（税 1.8%，−77%）。⚠ 若误用保守口径 `r=(M−L)/L` 再代入 λ（`M=7 MB ⇒ r=2.00, λ=0.439`）会得到与实测 0.2508 差 75% 的荒谬值——保守口径只用于**保证不超内存**，不用于估 λ。
5. **`b=256` 的判定从「P99 余量」改为「贴着 5 µs 门禁」**：模型 4.71 µs，实测 p99 4.36–6.42 µs。若门禁严格执行 5 µs，建议 `b=224`（P=4.15 µs）而不是 256；若接受「p99 中位 ≤5 µs」，保持 256。
6. **计数（S3）的判据被量化**：`r_cross = 1 + c_t/c_r`。`c_r=0.23 ⇒ r_cross=2.79 ≈ 当前 r=2.83` ⇒ 计数与清扫**打平**；`c_r≤0.117 ⇒ 2.09` 计数赢；`c_r≥0.411 ⇒ 2.00` 清扫赢。⇒ 默认 `NOCOUNT=1` 正确，但**任何把 `c_r` 降到 0.15 以下的实现（计数进对象头）会让 S3 在这些形状上翻盘**，与 `TAXMAP.md:436`「单价降到 ~3 Ir」的结论一致。
7. **S1（arena）不是「无回收所以不适用」**：实测 `a_peak ≥ 10.2 GB/s`，而 rc 形状的 `a_m` 只有 0.3–3.4 GB/s ⇒ **对这些形状 rc 的税是纯损失**。`spectrum_p1/p2/p3` 本来就不发射 region，这是构造上的正确选择；审计文档应明确「哪些形状根本不该用 rc」。
8. **`EXTC_RC_TIME` 不能被当作计时器**（`reclaim.c:162-177` 注释）：它只数调用次数。任何「tick 分解」数字必须来自外部 A/B 或 `calprof`。
9. **门禁数字**：`GC-ROOFLINE.md:463` 的 G1 需重写（`max` 不可判定，见 §7.3）；G4「pb 20 B/node ±10%」、G8「强制 T1 ≤1.1× T2」不受本轮影响；新增建议 G10「`M_peak ≤ 3·L_peak`」、G11「`r' ≥ 1.4×r_min`」。
10. **`MEMORY-MODEL-CURRENT.md` §18.4 的「M ≤ 2L + max(L,floor) + 取整」应改为 `M = L + κ_M·H`（保守 κ_M=1 ⇒ `(1+r)L`）**，其中 `r` 由 `thr` 倍数决定（默认 2）⇒ 对 `2L > floor` 的形状保守界是 `3L`，实测 6.44 MB（κ_M=0.772）。机器可检查的判据建议写成 `M_peak ≤ 3·L_peak`（G10）。

## §11 未标定 / 存疑（引用时必须带）

1. **`c_r`（计数单价）没有直接实测**。原因：默认档 `#if !EXTC_RC_NOCOUNT` 整段不编译，要测必须 `EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0` 两开关同开，属 E8；本轮为不污染基线未做。现用值 0.23（0.117–0.411）来自 `GC-ROOFLINE.md` 的推算，**不是实测**。
2. **`a_peak` 只是下界**：p1 的 0.01 s 分辨率 ⇒ ±10%；且只覆盖 512 B 与 16 B 两种对象尺寸，未测大对象/流式写。
3. **`g = 2.76` 来自旧提交的结构计数**（`MARK-MODEL.md` §2.2，工具 `tools/rc-mark-prof.py` 已对当前 HEAD 失效）。本轮**没有独立重测** `g`（插桩未记录 pushes）。由 `r' ≥ 1+gK/b` 只能得**上界** `g ≤ (min r' − 1)·b/K = 1.425×256/64 = 5.7`，与 2.76 相容但不算验证；`min r' = 2.425` 说明那轮离墙还有 `(2.425/1.69 − 1) ≈ 43%` 的距离。`g` 的真值在新 HEAD 上有 ~2× 不确定度 [待标定 E7]。
4. **`c_t` 的形状依赖没有模型**：0.21（p4）–0.46（digraph rep2），跨形状 2.2×。只有 2 个形状 ⇒ 不能外推。
5. **`p99` 不稳定（±40%）且 `max` 不可用**：WSL2 无实时调度，`taskset` 不能阻止宿主抢占。所有停顿数字应视为「本机、本时刻」；跨机复现需 `perf sched` 或裸金属。
6. **`κ=0` 只在 digraph/p4 上验证**：不能排除其他形状（写屏障密集、pinning 多）存在额外项。
7. **`p4` 的 sweep 占总 GC 的 90%**（λ 0.0245 中 0.022 是 sweep）⇒ 高 `r` 形状由 `c_s` 主导，而 p4 的 `c_s` 是 digraph 的 2×，`c_s` 同样有形状依赖。
8. **轮数与 `alloc_bytes` 对 patch 不敏感，但停顿对 SCAN_CHUNK 敏感**：patch 把 chunk 65536→8192 正是为了压单步上界；本轮 p99 测于 patch 后，patch 前的大对象步长未测。
9. **`SHARDS>1` 未标定**：分片会改变 `F_slab`、并行度与停顿，`DIGRAPH.md:303` 显示 SHARDS=4 更慢且 RSS +220 MiB ⇒ 当前不建议，但「并行度」这一维在模型里是缺的。
10. **`sweeps` 计数器与实际轮数差 1**（`sweeps` 在 `finish` 后 `sweepStep` 完成时自增），CSV 的 `sweeps` 列不能直接当轮数用；轮数以 `round` 列为准。

## §12 剩余标定实验（谁测 / 怎么测 / 判据）

| ID | 目标 | 方法 | 判据 | 优先级 |
|---|---|---|---|---|
| E7 | `g`（每对象推入数）新 HEAD 真值 | `calprof` 在 `markStep` 记录 `pops/pushes/children`（插桩已有 `cal_mark_step(_d, marked, _hot)`，加三个计数即可）；digraph 1M | 与 `r_min` 反解一致（±10%）；`g` 决定 `r_min` | P0 |
| E8 | `c_r` 计数单价 | `EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0` 重编，对比同 `λ_GC`；再用 `perf stat` 取 Ir | `c_r` 误差 ≤15%；判定 `r_cross` 落在 2.0–2.8 哪侧 | P0 |
| E9 | `σ→1` 的墙 | 构造全存活语料（如 `persist_nodes` 放大、或人为 pin 全堆），观察 `scale` 是否上升、`r'` 是否逼近 1.69 | `min r' ∈ [1.69, 1.9]`；`scale>0` 出现 | P0 |
| E10 | `a_peak` 精确值 | 独立微基准：1 s 以上、4 种对象尺寸、纯 bump；同时测 p3 的 linked-node 写 | 分辨率 ≤2% | P1 |
| E11 | 大对象 `SCAN_CHUNK` 与 p99 | patch 前/后同机对比（8192 vs 65536），用 `CAL-pause` 直方图 | p99 改善 ≥30% 且 promote 开销 ≤1% | P1 |
| E12 | 并行度（SHARDS） | 分片 1/2/4/8 跑 digraph 250k/1M，记录 `F_slab`、`M_peak`、p99 | 只有当 `M_peak` 不涨且 p99 降 ≥20% 才考虑 | P2 |
| E13 | `c_t` 形状依赖 | 4 种形状（指针密度 0/1/2.76/5+）× 2 尺寸 | 给出 `c_t(指针密度)` 曲线 | P2 |
| E14 | `κ_M` 形状依赖 | `calprof` 已在记 `peak_bytes`；对 6 形状 × 2 规模各取「活集最大的轮」，算 `(peak−L)/H` | 判据：`κ_M` 落在 [0.7, 1.05]；若某形状 <0.7 说明 sweep 抢在标记中段回收，内存模型需加时间项 | P1 |
| E15 | 三层 `r` 的口径审计 | 同一语料分别按「峰值轮 r」「全程 r_eff」「thr 倍数」出三张表，检查 `λ`/`M` 结论是否随口径翻转 | 任一预算结论必须给出三层 r；不一致处标注 | P0（文档级） |

## §13 复现

```bash
cd /home/alphayang/extC_Compiler
make BUILDDIR=build-th -j                       # build-th/extc
md5sum build-th/extc src/back/reclaim.c src/back/codegen.c

B=build-th/extc
$B -O2 -I stdlib -w -o /tmp/cal/digraph.c bench/gc/src/digraph.extc
python3 /tmp/cal/calprof.py /tmp/cal/digraph.c /tmp/cal/digraph.cal.c
gcc -O2 -o /tmp/cal/digraph.cal /tmp/cal/digraph.cal.c
for N in 250000 1000000; do
  env EXTC_CAL=/tmp/cal/out_$N EXTC_RC_PAUSE=1 taskset -c 6 /tmp/cal/digraph.cal $N 20261013
done
python3 /tmp/cal/analyze8.py                    # 汇总表
```

p4（另一形状）：

```bash
$B -O2 -I stdlib -w -o /tmp/cal/p4.c bench/gc/src/spectrum_p4.extc
python3 /tmp/cal/calprof.py /tmp/cal/p4.c /tmp/cal/p4.cal.c
gcc -O2 -o /tmp/cal/p4.cal /tmp/cal/p4.cal.c
env EXTC_CAL=/tmp/cal/p4b_2000 EXTC_RC_PAUSE=1 taskset -c 6 /tmp/cal/p4.cal 2000 20261013
```

S1 平台（无 rc，无需插桩）：

```bash
$B -O2 -I stdlib -w -o /tmp/cal/p1.c bench/gc/src/spectrum_p1.extc
/usr/bin/time -f '%e s %M KB' taskset -c 6 /tmp/cal/p1.c 200000 20261013
```

`calprof.py` 与 `analyze*.py` 目前在 `/tmp/cal/`，未入库（只读源码、不改仓库）；如需长期保留应移到 `tools/`（需 Lead 批准，因为那会新增仓库文件）。

## §14 内存层级常量（v26 新增：TLB 与 cache miss 的实测来源）

§2 的 `c_t` 与 `c_s`、以及 GC-ROOFLINE §3.11 的 `tau_pop` 屋顶，全部由本节三个通道给出。硬件：Intel Core Ultra 9 275HX（Arrow Lake HX）、WSL2、pin core 6、MemTotal 20.5 GB；L1d 48 KiB 12-way、L2 3 MiB 12-way（每核）、L3 36 MiB 12-way（shared 0-23）、line 64 B。`perf` 不可用（`kernel.perf_event_paranoid=2` ⇒ `perf stat` 报 `No supported events found`），所以**没有任何硬件计数器数据**，miss 数一律来自 callgrind `--cache-sim=yes` 的模拟或微基准。

### 14.1 常量表（每行给出通道与级）

| 常量 | 值 | 级 | 通道 | 出处 |
|---|---|---|---|---|
| f | 3.47 GHz | M | 微基准 | 4 条独立依赖链 × 400M 迭代 0.115 s（`/tmp/flg/cal.c`） |
| T_L1 | 0.78 ns/load | M | 微基准 | `memhier.c` 延迟梯 16-32 KiB |
| T_L2 | 1.8-5.4 ns/load | M | 微基准 | 延迟梯 256 KiB-2 MiB（2.98 / 3.44 / 4.45 / 5.37） |
| T_LL | 12.7-16.5 ns/load | M | 微基准 | 延迟梯 4-8 MiB（12.67 / 16.52） |
| T_DRAM | 97.9-156.6 ns/load | M | 微基准 | 延迟梯 16-256 MiB（依赖链，5 次取 min） |
| LLC_eff | 8-12 MB | M | 微基准 | 延迟梯 8→12 MiB 从 18.0 跳到 52.1；THP A/B 两种页大小同处跳 |
| C_STLB | 2048 项（4 KiB 页 ⇒ 8 MiB） | M | 微基准 | `memhier.c` TLB 梯：2048 项 18.02 ns、4096 项 21.49 ns |
| T_walk | 4-13.5 ns | M | 微基准 | TLB 梯 + THP A/B（同 footprint 差 4-30 ns，含与 DRAM 重叠） |
| B_read | 20.4-23.7 GB/s | M | 微基准 | 192 MiB 顺序读，3 轮 |
| B_write | 16.2-16.9 GB/s | M | 微基准 | 同上 |
| B_copy | 34-36 GB/s | M | 微基准 | 同上（r+w 计） |
| kappa_L(pop) | promote 0.00（`DLmr`=0 / 2,078,796）、sizeratio 0.00（0 / 175,275）、chase **0.092**（28,110 / 305,837） | M | callgrind | `--cache-sim=yes` 的**标记函数** `extc_rc_markStep` 自身 `DLmr` ÷ pop（`/tmp/cg5/*.ann`；全程序 `DLmr`/pop = 0.0006/0.006/0.28） |
| m（MLP） | 16 MiB 3.4、32 MiB 3.1、64 MiB 2.3、128 MiB 1.8、256 MiB 1.6 | D | 反推 | 梯延迟 ÷ 实测每 pop（`markcost` 11 点） |
| 大页可用性（rc 堆） | 不可用 | D | 机制 | slab 为 512 KiB 独立 mmap、非 2 MiB 对齐；THP 模式 madvise ⇒ 无法合成 |

### 14.2 与 §2 常量的换算

- `tau_pop = max(tau_issue, kappa_L·T_DRAM/m, b_pop/B_eff) + epsilon_tlb·1[W_pages > C_STLB]`（GC-ROOFLINE §3.11）。τ 是「每标记对象」口径，`c_t = tau_pop / s_bar` 是「每被标字节」口径；`s̄ = 42.5 B`（digraph 1M）。
- 校验：digraph 250k 注入实测 `tau_pop = 14.98 ns`、`s̄ = 42.5 B` ⇒ `c_t = 0.353 ns/B`；§2.2 的标定值 0.4114 比它高 14%，两个差源可核对（标定轮每步两次 `pauseNow` 20-40 ns；标定构建是 v7 系）。**§2.2 继续用 0.4114（偏保守），差值记 L。**
- `c_s = 0.0108 ns/B` 不动：受控探针 `markcost`（全存活、elide 不生效的最坏清扫）11 点给出 0.496-0.613 ns/KB = 0.00048-0.00060 ns/B，比 0.0108 小 18 倍——两者口径不同（`c_s` 含释放级联与每 slab 固定成本，`markcost` 只量位图扫描），所以 `c_s` 不能改用探针值；这条差异本身是本轮的一个结论：**清扫的扫描项是带宽型、常数级；`c_s` 里的大头是释放与元数据**。
- 冷档阈值：`EXTC_RC_MARK_COLD_BYTES = 16 MiB` ∈ [max(C_STLB·4 KiB, LLC_eff), 2×max(...)] = [8-12 MB, 16-24 MB] ✔。

### 14.3 复现（三条通道，全部可重跑）

```sh
cc -O2 -o /tmp/memhier tools/probe/memhier.c && taskset -c 6 /tmp/memhier 6      # 延迟梯/TLB 梯/带宽
cc -O2 -o /tmp/thpab   tools/probe/thpab.c   && taskset -c 6 /tmp/thpab 6        # 4 KiB vs 2 MiB A/B
./build/extc tests/gc-p0/src/pb_escape_grow.extc -o /tmp/pb.c
python3 tools/probe/rc-markcost.py slice  /tmp/pb.c -o /tmp/rt.inc             # 受控探针用运行期
python3 tools/probe/rc-markcost.py inject /tmp/pb.c -o /tmp/pb-mi.c            # 真实形状注入计时
cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -o /tmp/pb-mi /tmp/pb-mi.c
taskset -c 6 /tmp/pb-mi 20000000
cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -I/tmp -o /tmp/markcost tools/probe/markcost.c
taskset -c 6 /tmp/markcost 16 3 6
```

### 14.4 引用时必须带的三条限制

1. **LLC_eff 的机制是 U**：只做了页大小 A/B 与延迟梯，没有组冲突/stride 对照 ⇒ 「36 MiB、12-way 下有效容量 8-12 MB」是实测值 + 未分离的解释。
2. **m 的下降（3.4→1.6）是 D（反推）**：候选机制是位图 RMW 串行化（每 pop 两次位图读改写、第二次依赖第一次）与标记栈自身 miss，没有实验把两者拆开。
3. **phi_gc（GC 侧 DRAM 流量）只有理论值**：perf 不可用，没有用计数器验 ⇒ 带宽屋顶的四项里，`B_eff` 是 M、`phi` 是 D。

## §15 指令吞吐 Ir_per_s（v27 新增：§3.12 的 tau_issue 分母）

**用途**：把「每 pop 的发射成本」从同义反复（拿 τ_pop 当 tau_issue）变成两个可独立测的量之一；另一项是 Ir_pop（callgrind 的 `extc_rc_markStep` 独占 Ir ÷ pop 数）。

**探针**：`tools/probe/ipeak.c`（8 / 16 条独立累加链 + 空 `__asm__` 屏障）。

**本机实测（Intel Core Ultra 9 275HX，WSL2，pin 6，gcc 13.3 -O2 -fno-tree-vectorize）**：

| 混合 | Ir/iter（callgrind 差商） | ns/iter（3 rep 最小） | Ir/s |
|---|---|---|---|
| 8 链 · 11 Ir/iter | 11.0008 | 0.324 | 33.95 G |
| 16 链 · 24 Ir/iter | 24.0013 | 0.679 | 35.35 G |

取 **Ir_per_s = 34.6 G**（两混合差 4.4%，取均值）。

**口径与限制（引用时必须带）**：

1. 这是**纯 ALU、寄存器驻留**的天花板，比任何摸内存的循环都高 ⇒ 由它算出的 `tau_issue = Ir_pop/Ir_per_s` 是**下界**，不是预测值。
2. 它吸收 IPC×f，所以**不要**再用 §2.5 的 `f = 3.47 GHz` 当分母：3.47 GHz 是「4 条依赖链每迭代」的吞吐（≈1 iter/ns），与时钟不是一回事。若要反推时钟：34.6 G / IPC，IPC 需另行标定（本机未做，属 U）。
3. `-fno-tree-vectorize` 必须带：否则循环被 SIMD 化，Ir/iter 与 ns/iter 同时变，靠假设的 Ir/iter 会得出错误的 Ir/s。
4. 换机器必须重测（LLC/前端宽度/频率都变），并把新值写回 §3.12 的表与 `tools/probe/ipeak.c` 注释。

**判据**：同一二进制、同 pin、3 rep 的 ns/iter 极差 ≤2%；两混合（11 vs 24 Ir/iter）的 Ir/s 差 ≤10%（本机 4.4%）。任一不满足 ⇒ 频率/热降频/负载干扰，整组数不可用。

## §16 延迟屋顶的 τ_pop 与 MLP（v28 新增：式 3.13 的 T_LL 与 m）

§15 给的是**发射项** `Ir_pop/Ir_per_s`（每 pop 的指令时间）。式 3.13 的另一项是访存项：一旦标记的每一步都落在 LLC 之外，`τ_pop` 由「一次依赖加载的延迟 ÷ 同时在飞的 miss 数」给出。这两条常数在本机实测如下，来源与限制逐条列出。

### 16.1 常量表

| 常量 | 值 | 口径 | 来源 |
|---|---|---|---|
| `T_LL`（LLC 外单次依赖加载） | **90 ns**（区间 78-157 ns） | 指针追逐，活集 128-256 MB | `tools/probe/memhier.c`（§14.1）；取 128 MB 点 97.9 ns 与 256 MB 点 102 ns 的下沿，再按 R-C 形状的 166 MB footprint 取 90 ns |
| `κ_L`（每 miss 覆盖的字节） | 1 line = 64 B | 相邻 node 不共享 line | R-C 形状 node=32 B + 随机链 ⇒ 一跳一条 line |
| `m`（同时在飞的 miss 数） | **1**（浅栈）/ **4.8**（深栈批量） | 同一随机链、同一 Ir、同一 miss 数，只换路径 | §6.16.3 三口径表（Ir/D1/LLd 逐位相同，墙钟 0.88 vs 0.37 s ⇒ m 比 2.4） |
| `c_fix`（步的固定开销） | **0.2 µs** | 每步除 pop 之外的时钟读与状态检查 | R-D 预测文件，实测 mean_step − b·τ_pop 的残差 |

### 16.2 由常量到预测

    tau_pop(W) = T_LL(W) / (kappa_L * m(path)) + Ir_pop / Ir_per_s
    mean_step(b) = b * tau_pop + c_fix

- **暖档（活集 < 16 MiB，冷档阈值见 §3.5）**：全部命中 L1-L3，`T_LL` 项退化为 §14.1 的 L1/L2 延迟（0.78/1.8-5.4 ns），此时 `tau_pop ≈ Ir_pop/Ir_per_s`（§15 的 2.76-5.59 ns），语料六个形状实测 3.66-14.41 ns 全部落在这一支。
- **冷档（活集 > 16 MiB 且每 pop 一次跨页跳）**：`m=1` 时 `tau_pop ≈ 90 ns`，实测 77.9-87.9 ns（残差 +3%~+15%）；`m=4.8` 时 `tau_pop ≈ 20 ns`，实测 20.5-22 ns。
- **判据（LAT-b）**：同一形状随机/顺序的 `tau` 比 ≥5（实测 12.67），顺序对照 `tau_seq ≤ 12 ns`（实测 6.8）——这一条把「形状确实是延迟屋顶」与「只是慢」分开。

### 16.3 引用时必须带的三条限制

1. **`m` 不是机器常数，是步形态常数**。它由「一次 pop 之后是否立刻扫」决定：浅栈路径 `m=1`，深栈批量路径 `m≈4.8`。所以 `τ_pop` 不能写成「本机恒定 90 ns」；正确写法是 `T_LL/m(path)`，并给出该路径对应的 `b`。
2. **`T_LL` 是「冷、随机、无预取」的上界**。同 footprint 下顺序访问只有 6.8 ns/pop（12.7× 差），THP 开启与否只差 4-30 ns；因此 `T_LL` 只在「每 pop 跳一条新 line」时成立（LAT-b 与 §6.16.2 都在守这个前提）。
3. **尾部不在式 3.13 内**。单 pop 步的 p99.9 落在 64-128 µs 桶（b=1 仍有 5/2.44M 步 >64 µs），这是「单次走表+页错误级代价」，与 `b` 无关（§8.1 R15）；式 3.13 只承诺均值，LAT-d 因此只判 `over1ms=0`。

### 16.4 复现

    ./build/extc tests/gc-p0/src/lat_randchain.extc -o /tmp/lat/rand.c
    cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -o /tmp/lat/rand /tmp/lat/rand.c
    taskset -c 6 env EXTC_RC_PAUSE=1 /tmp/lat/rand 4 2>&1 >/dev/null | grep -E "step|wbudget"
    # m 的对照：同源码加 -DEXTC_RC_MARK_SHALLOW=0（深栈批量），其余不变

两条数字口径：`tau = [work] 总 step 时间 / [wbudget] pops_tot`（**跨路径不可比**，pops_tot 的分母来源不同，见 §6.16.3）；`m = tau(shallow)/tau(deep)`。

## §17 标定的机器状态前置检查（v32 新增：为什么两道）

`tests/gc-p0/bseg-cal.tsv` 把 seg 步的 p99.9 换算成 `c_seg`（ns/word），门禁 (b) 用它判 `p99.9 ≤ B_seg`。
这张表是**在某一台机器的某一刻**量出来的，而 `c_seg` 不是一个纯代码常数：pd 形状的活集（几十到几百 MB）远超 LLC，
每一步都在 LLC 之外跳，所以它的尾部是**访存延迟**的函数。标定工具因此必须在动手之前证明「这台机器此刻处于标定条件」，
否则一次被抢占的测量会永久放宽门禁门槛（判据只严不放宽）。

### 17.1 第一道：指令吞吐（`--no-speed-check` 可跳过）

- 探针：`tools/probe/ipeak.c`（8 条独立累加链，空 asm 屏障防 DCE/向量化），编译命令在工具里固定为 `cc -O2 -fno-tree-vectorize`。
- 参考值 `SPEED_REF_NS = 0.324` ns/iter（§15 标定的同一口径），容差 `SPEED_TOL = 1.40`。
- 超过上限即 `SystemExit`：此时标定会把降频写进 `c_seg`。

### 17.2 第二道：访存延迟（`--no-latency-check` 可跳过）

- 探针：`tools/probe/memhier.c` 的延迟梯，只跑到 **64 MiB 一点**（`memhier <cpu> 67108864`，新增的第二参数；不必付 256 MiB 的代价）。
- 参考值 `LAT_REF_NS = 156.6`（2026-12-03 实测带 97.9–156.6 的上界，§14.1），容差 `LAT_TOL = 1.30`。
- 这一道是必须的：**算力额定并不代表访存额定**。2026-11-16 的一个窗口里 ipeak 全程 0.331–0.342 ns/iter（额定），
  而 64 MiB 随机环是 **338.04 ns/load**（参考带上界的 2.2 倍），晚些时候回到 145–152 ns。

### 17.3 漂移取证（2026-11-16，同一小时、同一核、三条臂）

| 臂 | 源码 | pd_bigobj `c_seg` | pd_pointer_dense `c_seg` | ipeak |
|---|---|---|---|---|
| patched | 当前树（HEAD + L10 修复） | 6.421 | 8.803 | 0.342 |
| era1113 | `49d76213`（2026-11-13 的代码） | 6.294 | 9.920 | 0.335 |
| base | `a5ae1446`（本轮前的 HEAD） | 7.110 | 7.375 | 0.331 |
| **表里记的 11-13 值** | `61c9fad7` 那一代 | **3.841** | **5.997** | — |

三条臂全部比表高 1.6–2.1×，**包括那一代代码本身**（era1113）。⇒ 漂移不在源码：
同一份 11-13 代码今天重跑也复现不出来。而三段测量里 ipeak 一直是 0.33–0.34，只有访存那一路出现过 338 ns 的窗口。

### 17.4 结论与规则

1. **表不重写**。拿今天（访存被抬高）的数字写进 `bseg-cal.tsv` 会把门禁 (b) 放大约 1.6–2×，违反「只严不放宽」。
   现状是：表里的常数比今天的实测更严，门禁仍按它判，pd 两个形状当前 PASS。
2. **重标定必须过两道前置检查**，并通过 `--src-md5`（`tools/source_split.py --md5` 的渲染文本 md5）与 `--build-md5` 记录来源。
3. **`c_seg` 量的是整步**（探针插在 `extc_rc_stepNote(_d)` 之后、`if (seg > 0)` 之前），所以它包含步内的容量尾部工作：
   v23 的页归还、v24 的自动块归还与 free 队列排空、v31 的巨块丢弃都在 `extc_rc_sweepStep` 的收尾里。
   这些工作各有自己的预算（≤256 KiB/次、≤16 块/次、40/50 µs 硬截止），但形式上属于「一步」，所以契约写成
   `P_step ≤ b·τ_t + c + c_seg·chunk`，其中 `c_seg` 是**整步/chunk**，不是「扫描/chunk」。
4. **样本量门槛**：`MIN_N = 100000`。p99.9 需要至少 1e5 个 seg>0 步；200 轮只有约 2.5e4，样本不足时工具**失败**（不再记 N/A 放行）。

### 17.5 复现

```sh
# 合法标定（两道前置检查都会打印）
python3 tests/gc-p0/calib_cseg.py --extc ./build/extc \
    --src-md5 "$(python3 tools/source_split.py --md5 | tr '\n' ' ')" \
    --build-md5 "$(md5sum build/extc | cut -d' ' -f1)" --rounds 1000 --min-n 100000
# 只看机器状态（不标定）
cc -O2 -fno-tree-vectorize -o /tmp/ipeak tools/probe/ipeak.c && taskset -c 6 /tmp/ipeak 8 400000000
cc -O2 -o /tmp/memhier tools/probe/memhier.c && taskset -c 6 /tmp/memhier 6 67108864
```

## 附录 A 原始数据汇总（`analyze8.py` 输出）

| 数据集 | 轮 | 分配 MB | `a_all` GB/s | `a_mut` GB/s | `λ_GC` ns/B | `λ_model` | 误差 | `τ_t` ns | `τ_s` ns | `c_t` ns/B | `c_s` ns/B | GC% |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dg250-pre | 28 | 33.20 | 0.926 | 1.068 | 0.1436 | 0.1366 | −4.9% | 16.12 | 0.500 | 0.3794 | 0.0117 | 13.29 |
| dg1M-pre | 85 | 135.90 | 0.307 | 0.332 | 0.2526 | 0.2438 | −3.5% | 17.65 | 0.452 | 0.4153 | 0.0106 | 7.75 |
| dg250-post | 28 | 33.20 | 0.915 | 1.053 | 0.1433 | 0.1363 | −4.8% | 16.10 | 0.495 | 0.3790 | 0.0116 | 13.11 |
| dg1M-post | 85 | 135.90 | 0.309 | 0.335 | 0.2508 | 0.2419 | −3.6% | 17.49 | 0.458 | 0.4114 | 0.0108 | 7.76 |
| dg1M-rep1 | 85 | 135.90 | 0.305 | 0.330 | 0.2519 | 0.2430 | −3.5% | 17.61 | 0.448 | 0.4142 | 0.0105 | 7.67 |
| dg1M-rep2 | 85 | 135.90 | 0.275 | 0.298 | 0.2788 | 0.2688 | −3.6% | 19.42 | 0.514 | 0.4570 | 0.0121 | 7.66 |
| p4-2k | 46 | 48.29 | 3.891 | 4.301 | 0.0245 | 0.0233 | −5.2% | 6.81 | 0.673 | 0.2128 | 0.0210 | 9.55 |
| p4-8k | 186 | 196.35 | 3.166 | 3.433 | 0.0245 | 0.0236 | −3.6% | 7.57 | 0.660 | 0.2367 | 0.0206 | 7.76 |

注：附录表的 `λ_model` 用未乘 `F_slab` 的旧式 `(c_t+c_s·r)/(r−1)`，故误差 −3.5%~−5.2%；§5.2 表用带 `F_slab` 的新式，误差缩到 −2.0%~0.0%。**正式引用请用 §5.2**，附录保留以显示改进幅度。

## 附录 B 关键推导

### B.1 `λ(r)`

一轮内：`mark = c_t·L`；`sweep = c_s·cap = c_s·F_slab·H = c_s·F_slab·r·L`；分配量 `A = H − L = (r−1)L`。故 `λ = (c_t + c_s F_slab r)/(r−1)`。对 `r` 单调递减，下界 `c_s F_slab = 0.014 ns/B`（纯清扫）。

### B.2 `r_min`（trace-rate）

见 §8.1。`pops_avail = b(H−L)/(K s̄)`、`pushes_need = g L/s̄` ⇒ `b(r−1) ≥ gK` ⇒ `r_min = 1 + gK/b`。`b=256, K=64, g=2.76 ⇒ 1.690`。

### B.3 `M(r)`

稳态触发点：`H = L + max(2L, floor) + ε`（`thr=2L` 与 `roundOk` 的组合，§3.4）；峰值 `M = L + κ_M·H`，`κ_M < 1` 因为标记期间 sweep 已在回收上一轮垃圾（§3.6）。故

```
保守（保证不超）:  M_max = L(1 + r)
典型（κ_M ≈ 0.8）: M_max = L + 0.8·H = L(1 + 0.8r)
```

digraph 1M：`L_peak = 2.333 MB`，`r = 2.43` @ 峰值轮 ⇒ 保守 7.68 MB、典型 6.87 MB、实测 6.44 MB ⇒ 实测 κ_M = 0.772（比 0.8 再低 4%）。反解 `M=7 MB`：保守 `r≤2.00`、典型 `r≤2.59`；再按 `r_eff = r_peak + 0.4`（实测峰值轮 2.43 → 全程 2.83）得 λ 用的 `r_eff`。

### B.4 `P(b)`

`P = b·τ_t + W_sweep·64·τ_s`（每步最多 `b` 个 pop + `W_sweep=8` 个 word，每 word 64 槽，每槽 `τ_s`）。`b=256 ⇒ 4.71 µs`。

### B.5 双目标最优

给定 `M_max` 与 `P_max`：

```
b* = min( 256 , (P_max - 8*64*tau_s)/tau_t )
r_cons = (M_max - L_peak)/L_peak        # 保守（κ_M=1）
r_typ  = 1.25 * r_cons                  # 典型（κ_M=0.8，仅估算 λ 用）
可行 ⇔ r_cons >= r_min(b*) = 1 + gK/b*   # 硬判据用保守口径
λ* = (c_t + c_s*F_slab*r_use)/(r_use - 1)  # r_use = min(r_typ, ...) 工作点
```

代入 1M：`b* = min(256, (5e-6−0.234e-6)/17.49e-9) = min(256, 272) = 256`；`r_min = 1.69`。

| `M_max` | 峰值轮 `r` 保守（κ=1） | 峰值轮 `r` 典型（κ=0.772） | 全程 `r_eff = r_typ + 0.4` | `λ` | 税（`a_all`=0.309） |
|---|---|---|---|---|---|
| 7.0 MB | 2.00 | 2.59 | 2.99 | 0.228 | **7.0%** |
| 12 MB | 4.14 | 5.37 | 5.77 | 0.103 | 3.2% |
| 21 MB | 8.00 | 10.36 | 10.76 | 0.058 | 1.8% |

口径说明（**三层 `r` 别混**）：① 峰值轮 `r = H/L_prev`，决定内存（实测 2.43）；② 全程 `r_eff`，决定平均 λ（实测 2.83）；③ `thr` 倍数，是政策旋钮（默认 2）。`M=7 MB` 行给 `λ=0.228 / 税 7.0%`，实测同预算 `0.2508 / 7.76%`（差 −9%，因 `r_eff` 取 2.99 略高于实测 2.83）——**本表唯一的可自校准行吻合**。⇒ **默认档不是调参失误：7.8% 的税就是 3L 内存预算下的合理代价**；要降税只能加内存（`M=12 MB` 时 −59%）。