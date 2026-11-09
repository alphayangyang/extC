# extC 分配成本归因（ALLOC-COST）

> 任务：E17 / task-21。目标是把"每分配成本"和"chase 的访存延迟"从口号变成可复现的数字，并回答三件事：
> (a) chase 每步 40–80 ns 是不是纯访存延迟；(b) 四形状 mutator 每分配的 ns 组成；(c) 哪些量能被 GC 侧改动影响。
>
> **口径声明**：本文所有 Ir / 缓存数据来自 callgrind（确定性）；所有 ns 都是**引用值**，来自
> `docs/analysis/MEASUREMENTS-V2-AFTER.md` §11.1 的冻结计时（本机交叉核对见 §1.4），本文**不产生新的墙钟测量**。
> 每条数字都标了来源（本机 callgrind / docs §11.1 / 本机交叉单测）。不确定的写在 §8。

---

## §0 结论速览

1. **四形状的 GC 指令份额**（callgrind，`-O2 -std=c11 -fwrapv`）：promote **39.2%**、sizeratio **21.6%**、chase **16.8%**、cycleratio **9.3%**；其中 promote 几乎全在 `extc_rc_markStep`（30.0%）。
2. **GC 的指令份额不是 GC 税的代理**：GC 墙钟份额 / GC Ir 份额 = promote **1.03**、cycleratio **0.99**、sizeratio **0.48**、chase **0.21**。GC 代码是顺序扫（高 IPC、miss 可重叠），只有 promote 的 GC 真的把时间花在访存上（GC 占全局 DLmr 的 **93.0%**）。
3. **chase 的 43.3 ns/步不是"纯 DRAM 延迟"**：每步只有 **0.089 次** last-level（≈DRAM）read miss，其中 **71.6% 来自 GC 标记**（0.0636/步）；walk 自身只有 **0.0251/步**。按 70–80 ns/miss 折算，DRAM 项 ≤ **7 ns/步（≤1/6）**；主体是 2.59 次/步的 L1 miss 命中 L2/L3 + 75.1 Ir/步。**THP 已实测排除**（§4.3）。
4. **每分配成本可以三分**：`总 Ir/分配 = 形状本体 + 内联 rc 快路径 + GC 命名函数`。promote 176.7 = **39.2 + 67.6 + 69.3**（+0.6 libc）。off 对照（`EXTC_RECLAIM=0`，纯 bump arena）的分配器只要 **31.0 Ir/alloc**，所以"回收机制在分配点"净增 **36.6 Ir/alloc**（promote）。
5. **`-fwrapv` 显著影响 promote 的指令数**：不带时 783.6 M Ir，带时 **804.1 M**（+2.6%）；其余三形状 <0.05%。本文一律用 `tests/gc-p0/run.sh` 默认 CFLAGS（含 `-fwrapv`）。
6. **λ 的两个口径必须按形状标定**：同一 GC Ir 折算成 ns 的系数（ns/Ir）跨形状差 **11×**（promote 0.0508 → chase 0.576）。promote 的 λ 两口径自洽到 2%（0.150 ns/B vs docs 0.147），chase 差 4.7×。
7. **用户硬约束下（不抬 r/thr、不抬 M_max）可动的量**：内联分配/屏障快路径（净增 36.6/29.5/60.5 Ir/alloc，promote/sizeratio/cycleratio；chase 23.2 Ir/步）、mark 扫描的访存结构（promote 占全局 DLmr 92.5%）、sweep/shouldSweep 的纯 ALU 成本。**不可动**的是形状本体与 chase walk 的串行 miss 链。

---

## §1 口径

### 1.1 机器与本报告唯一构建

| 项 | 值 |
|---|---|
| CPU | Intel Core Ultra 9 275HX（WSL2；`nproc=24`；`/proc/cpuinfo` 报 3072 MHz，是**名义基频**，见 §8-1） |
| 内核 | Linux 6.18.58.1-microsoft-standard-WSL2，x86_64 |
| 编译器（extC→C） | 本次唯一构建 `build-emp2/extc`，**md5 `41efaf5716b25e7ac0a8ddfd5cac5d24`** |
| 源版本 | 构建时校验 = FREEZE v4：`src/back/reclaim.c` md5 `ff42799bfa62c2c9b0ac5b238e7d05e9`（3607 行）、`src/back/codegen.c` md5 `cf2885440981b5c9d154927fb61441b9`（15994 行） |
| C 编译器 | gcc 15.2.0，`-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`（与 `tests/gc-p0/run.sh` 默认一致） |
| 量具 | `valgrind --tool=callgrind --cache-sim=yes` + `callgrind_annotate --auto=no --show-percs=no --threshold=99.99` |
| callgrind 自动探测的 cache | I1 64 KiB/64/16-way；D1 **48 KiB/64/12-way**；LL **36 MiB/64/18-way**（= 本机 L3） |
| 原始数据 | `/tmp/empirics/cost2/{c,bin,cg,log}`（临时目录，不入库）；复现脚本 `tests/gc-p0/cost/{run.sh,parse_callgrind.py}` |

⚠ **复现前提**：报告写作时工作树已被后续 patch 改动（`src/back/reclaim.c` md5 变为 `c5e10f7a…`、`src/back/codegen.c` 变为 `d3393828…`）。复现本报告必须回到 FREEZE v4 的源再重建（或直接用 `build-emp2/extc` 的 md5 比对）。

### 1.2 四个形状与 bench 档参数

形状源：`bench/gc/v2/src/{promote,sizeratio,chase,cycleratio}.extc`。参数取 docs §11.1 的 **bench 档**：

| 形状 | 参数 | 语义（源行） | 分配次数 | 步数 | 分配字节 | B/alloc |
|---|---|---|---|---|---|---|
| promote | `500000 20261013` | 每轮 9 个短命结点 + 每 10 轮 1 个提升结点 + 每轮走 ≤8 长命（`bench/gc/v2/src/promote.extc:24-46`） | 4,550,000 | — | 109.2 MB | 24.0 |
| sizeratio | `2000000 20261013` | 200 万次大小混合分配、活集 1024 槽 | 2,000,000 | — | 160 MB | 80.0 |
| chase | `1048576 20261013 4000000` | 1M 结点 × 4 出边随机游走 4M 步，每 256 步顺手丢 4 个结点（`bench/gc/v2/src/chase.extc:43-55`） | 1,111,076（含 churn 62,500） | 4,000,000 | 53.3 MB | 48.0 |
| cycleratio | `400000 20261013 100` | 400k 组、每组 100 次分配后整组丢 | 1,821,441 | — | 43.2 MB | 23.7 |

`分配字节` 与 `λ` 的墙钟值来自 docs `MEASUREMENTS-V2-AFTER.md:200-205`（§11.1 表）。形状输出已自证参数（`/tmp/empirics/cost2/log/*.stdout` 首行）。

### 1.3 每形状的 GC 档位与内存墙（本机 callgrind 二进制上再跑一次 `EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，仅取插桩行，不取时间）

| 形状 | `[work]` 关键字段 | `[pause] collects` | `[step] n / total_us` | `[mmax]` |
|---|---|---|---|---|
| promote | `thr=3418672 mult=4 M_max_now=5128008 M_max_implicit=1 scale_max=1 pressure_steps=172 thr_max=3418672` | 83 | 21,288 / 17,892 | — |
| sizeratio | `thr=1048576 mult=4 M_max_now=344064 M_max_implicit=1 pressure_steps=308` | 171 | 2,044 / 2,559 | `memory wall unreachable: M_max=344064 floor=1048576` |
| chase | `thr=63438892 mult=4 M_max_now=69206064 M_max_implicit=1 scale_max=16 pressure_steps=430` | 2 | 1,255 / 5,627 | — |
| cycleratio | `thr=1048576 M_max_now=94008 M_max_implicit=1 pressure_steps=…` | 42 | 1,221 / 1,670 | `M_max=94008 floor=1048576; using floor` |

**`[step] total_us` 与 docs §11.1 的 GC 时间交叉一致**：promote 17.9 ms vs 16.4 ms、sizeratio 2.56 vs 2.41、chase 5.63 vs 6.19、cycleratio 1.67 vs 1.52（差 ≤9%）。⇒ 本报告用 Ir 拆分出的"GC 份额"与 docs 的"GC 墙钟份额"是同一批工作，可以互相换算（但不是常数系数，见 §5.3）。

⚠ `[mmax]` 的行只因隐式墙（3×L）早期小于 floor 而打印，每次运行都出现，违反 `bench/gc/v2` SPEC 的"stderr 必须为空"；已由 Lead 派给 runtime-facts 改为只在显式 `EXTC_RC_M_MAX` / 诊断开关时打印。本文的 `[mmax]` 读数按"诊断输出"使用，不作为判据。

### 1.4 ns 的来源与交叉单测

本文的 ns 一律取 docs §11.1（同一 FREEZE 构建、同一 bench 档）：

| 形状 | 墙钟 | GC 时间 | mutator 时间 |
|---|---|---|---|
| promote | 40.8 ms | 16.4 ms | 24.4 ms |
| sizeratio | 23.4 ms | 2.41 ms | 21.0 ms |
| chase | 173.2 ms | 6.19 ms | 167.0 ms |
| cycleratio | 15.2 ms | 1.52 ms | 13.7 ms |

**本机交叉单测**（一次性 sanity check，15 reps 中位、`taskset -c 23`、减去 hello 进程开销 0.82 ms、**不是**本报告的数据源）：

| 形状 | 本机净墙钟 | 折算 | docs | 偏差 |
|---|---|---|---|---|
| promote | 40.52 ms | 8.91 ns/alloc | 8.97 | −0.7% |
| sizeratio | 21.90 ms | 10.95 ns/alloc | 11.70 | −6.4% |
| chase | 188.89 ms | 172.6 ns/alloc = **47.2 ns/步** | 43.3 ns/步 | +9% |
| cycleratio | 13.36 ms | 7.33 ns/alloc | 8.35 | −12% |

⇒ docs §11.1 的时间在本机可复现（±12%，chase 偏差来自机器负载），因此 ns 列沿用 docs 值并在表里注明。

---

## §2 总量：四形状的函数级 Ir 与缓存 miss（本机 callgrind）

| 形状 | Ir | main self | GC Ir（`extc_rc_*`） | GC D1mr | GC DLmr | D1mr | DLmr |
|---|---|---|---|---|---|---|---|
| promote | 804,083,469 | 60.44% | **39.2%** | **81.6%** | **93.0%** | 6,682,876 | 1,107,375 |
| sizeratio | 228,912,109 | 77.05% | 21.6% | 53.4% | 34.7% | 424,261 | 1,491 |
| chase | 300,584,855 | 82.25% | 16.8% | 5.4% | **71.6%** | 10,377,556 | 354,818 |
| cycleratio | 274,116,151 | 87.75% | 9.3% | 15.7% | 1.7% | 79,624 | 1,257 |

### 2.1 每形状的 top 函数（self，share of Ir）

| 形状 | 第 1 | 第 2 | 第 3 | 第 4 |
|---|---|---|---|---|
| promote | `main` 60.44% | `extc_rc_markStep` **30.01%**（D1mr 5,405,839 / DLmr 1,024,392） | `extc_rc_sweepStep` 6.37% | `extc_rc_shadeSlow` 1.74% |
| sizeratio | `main` 77.05% | `extc_rc_sweepStep` 9.73%（D1mr 7,610） | `extc_rc_markStep` 7.76%（DLmr 256） | `extc_rc_shouldSweep` 3.22%（0 miss） |
| chase | `main` 82.25% | `extc_rc_markStep` 16.08%（DLmr 252,370） | `extc_rc_shadeSlow` 0.38% | `extc_rc_ownSlab` 0.23% |
| cycleratio | `main` 87.75% | `extc_rc_sweepStep` 7.14%（D1mr 9,067） | `pick` 2.63% | `extc_rc_markStep` 2.05% |

读法：
- **promote 的 GC 税 = mark 的访存税**：markStep 独占全局 D1mr 的 80.9%、DLmr 的 92.5%，而它自己只有 30.0% 的指令。这是四形状里唯一"GC 指令份额 ≈ GC 墙钟份额"的原因（§5.3）。
- **sizeratio/cycleratio 的 GC 税 = sweep 的 ALU 税**：sweep 占 9.7%/7.1% 指令但 D1mr 只有 7,610/9,067，几乎不碰 cache；`shouldSweep` 3.22% 且 0 miss，纯谓词成本。
- **chase 的 GC = 指令有份、墙钟没份**：markStep 16.1% 指令、DLmr 252,370（占全域 71.6%），但 GC 总墙钟只 3.6%（docs 6.19/173.2）。原因：标记是顺序扫（bitmap/slot 全扫），miss 可重叠（MLP 高）；walk 的 miss 是跨步依赖链，无法重叠。

---

## §3 每分配（每步）的指令分解：形状本体 / 内联 rc 快路径 / GC

方法：`on` = 默认档（回收开启）；`off` = `EXTC_RECLAIM=0` 生成（分配器换成纯 bump `extc_arena_*`，无 GC；**不是"GC 税=0"的等价对照**，因为分配器也换了，见 `src/main.c:471` "CHANGES THE OUTPUT"）。

- `内联 rc 快路径 Ir/alloc = main_on_Ir/alloc − main_off_Ir/alloc`：即 extC 在分配点/屏障内联进 `main` 的那部分（slot 计算、`storeRef*` 检查、`shouldSweep`/`pressure` 调用点、`ownSlab`）。
- `GC 命名函数 = Σ extc_rc_* self Ir`（callgrind 里它们不是内联的）。

### 3.1 表：每分配 Ir 三分（本机 callgrind；on/off 为总 Ir 比）

| 形状 | 总 Ir/alloc | 形状本体（off main） | 内联 rc 快路径 | GC 命名 | 其他(libc) | off 总 Ir/alloc | off 里的 bump arena | on/off |
|---|---|---|---|---|---|---|---|---|
| promote | **176.7** | 39.2 | **67.6** | **69.3** | 0.6 | 71.8 | **31.0** | 2.46× |
| sizeratio | **114.5** | 27.7 | 60.5 | 24.7 | 1.6 | 68.6 | 31.0 | 1.67× |
| chase | **270.5** | 138.8 | 83.7 | 45.5 | 2.5 | 173.1 | 31.0 | 1.56× |
| cycleratio | **150.5** | 40.6 | 91.5 | 14.1 | 4.3 | 77.2 | 31.0 | 1.95× |

要点：
1. **纯 bump 分配器的成本是常数 31.0 Ir/alloc**（四形状完全一致，来自 off 档 `extc_arena_alloc`）。默认档在分配点花 60.5–91.5 Ir/alloc ⇒ **回收机制在分配点净增 29.5–60.5 Ir/alloc**。
2. 该净增里既有**屏障**（每次写引用），也有**每分配的谓词/簿记**（`shouldSweep`、`pressure`、`ownSlab`、`slotOf`）。sizeratio 的 `shouldSweep` 单独 3.22%、`checkedIndex` 0.87%、`storeRefSameRegion` 0.69%——这些就是可被内联裁剪的目标。
3. promote 的"内联 rc"67.6 Ir/alloc 里包含"每轮 9 次分配 + 1 次屏障写 `s.next = head`"（`bench/gc/v2/src/promote.extc:28-30`）。

### 3.2 chase：按步而不是按分配看（4M 步 / 1.11M 分配）

| 项 | Ir/步 | D1mr/步 | DLmr/步 |
|---|---|---|---|
| 总计 | **75.15** | **2.594** | **0.0887** |
| 其中 walk 本体（off main） | 38.56 | — | — |
| 其中内联 rc（屏障/分配点） | 23.25 | — | — |
| 其中 GC 命名 | 12.65 | 0.139 | **0.0636** |
| 推得 walk 自身的 DLmr | — | — | **0.0251** |

⇒ GC 的 DLmr 是 walk 的 2.5 倍，但 GC 墙钟只占 3.6%（§2.1 解释）。**"chase 慢是因为访存"这句话若指 DRAM，则主要责任不在 walk，而在 GC 标记的顺序扫；而 GC 标记的 miss 又能重叠，所以最终只占 3.6% 时间。**

---

## §4 (a) chase 的 40–80 ns/步 是不是纯访存延迟？

### 4.1 直接证据：每步只有 0.089 次 last-level read miss

| 口径 | 值 | 来源 |
|---|---|---|
| 墙钟 | 173.2 ms / 4M 步 = **43.3 ns/步** | docs §11.1（本机交叉 47.2 ns/步） |
| Ir | 75.15 /步 | 本机 callgrind |
| D1mr（L1 read miss） | **2.594 /步** | 本机 callgrind |
| DLmr（last-level read miss ≈ DRAM） | **0.0887 /步** | 本机 callgrind |
| D1mr − DLmr（L1 miss 命中 L2/L3） | **2.505 /步** | 本机 callgrind |

若 43.3 ns 全部由 DRAM 延迟解释，需要约 `43.3/75 ≈ 0.58` 次 miss/步（按 75 ns/miss）；实测只有 **0.089**，即 DRAM 项 ≤ `0.089 × 70–80 ns = 6.2–7.1 ns`，**≤ 1/6**。

### 4.2 工作集对照（同源同参，只把 m 从 1,048,576 降到 262,144，活集 12.6 MB < L3 36 MB）

| 指标 | m=1,048,576（bench） | m=262,144 | 变化 |
|---|---|---|---|
| Ir/步 | 75.15 | 69.58 | −7% |
| D1mr/步 | 2.594 | 2.577 | −1% |
| **DLmr/步** | **0.0887** | **0.0221** | **−75%** |
| DLmr 中 GC 部分 | 252,370（0.0636/步） | **259（0.00026/步）** | −99.9% |
| GC Ir 份额 | 16.8% | 9.8% | — |

⇒ 一旦活集装进 L3，**LL miss 掉 75%（GC 的 LL miss 掉 99.9%）而 Ir 几乎不动**。如果墙钟由 DRAM 主导，这一步应让每步省 ~5.7 ns（−13%）；这是**推断**（本次没做 m=262144 的原生计时，原因见 §8-4）。

### 4.3 结论（(a) 的答案）+ THP 负结果

- **不是"纯访存延迟"**，更不是"DRAM 带宽/延迟主导"：DRAM 项 ≤ 1/6。
- 43.3 ns/步 的构成（推断，未用硬件计数器分解）：**2.5 次 L1 miss 命中 L2/L3（约 25–30 ns，含一次跨步依赖链）** + **75.1 Ir/步（约 10–15 ns，若 IPC 3.6）** + 分支/写分配等剩余。
- **THP 不是杠杆（Lead m03170 实测，本机未重跑）**：在 FREEZE v4 生成 C 的 `slabNew` 的 `aligned_alloc` 之后插入 `madvise(s, 1MiB, MADV_HUGEPAGE)`、加 `-D_GNU_SOURCE` 重编，5 reps 交替（`v2run --pin 6, 1048576 20261013 4000000`）：base 中位 **171.4 ms / RSS 73,692 kB**，THP 中位 **171.0 ms / RSS 相同** ⇒ 无差异。**chase 的 miss 主要发生在 L2/L3，不在 TLB/大页路径**，因此 THP/madvise 被明确排除在本报告的优化清单之外（§6）。
- 关键洞察：**chase 的"慢"来自低 IPC 的依赖链（1.74 G Ir/s），而 promote 能跑到 19.7 G Ir/s**（§5.1 的 ns/Ir）——两者差 11×，说明 chase 受限于串行 miss 延迟而非指令数；但这部分延迟主要发生在 L2/L3，且 walk 自身的 DRAM miss 只有 0.0251/步。
- 与 GC 的关系：GC 的 DLmr 更多（0.0636/步），但顺序扫可重叠，实测只占 3.6% 墙钟 ⇒ **chase 的优化收益不在 GC 侧**（与 docs `MEASUREMENTS-V2-AFTER.md:254` "与 GC 无关"一致，但要把"访存延迟"精确到"层级延迟 + 间接层"，而不是 DRAM，也不是 TLB）。

---

## §5 (b) 四形状 mutator 每分配的 ns 组成

### 5.1 表：ns 组成（ns 来自 docs §11.1；Ir 拆解来自本机 callgrind）

| 形状 | 总 ns/alloc | GC ns/alloc | mutator ns/alloc | mutator Ir 拆解 | ns/Ir | 隐含 G Ir/s |
|---|---|---|---|---|---|---|
| promote | **8.97** | 3.60（40.2%） | 5.36 | 106.8 = 形状 39.2 + rc 67.6 | 0.0508 | **19.7** |
| sizeratio | 11.70 | 1.21（10.3%） | 10.49 | 88.2 = 27.7 + 60.5 | 0.1022 | 9.8 |
| chase | 155.9（43.3 ns/步） | 5.57（3.6%） | 150.3（41.8 ns/步） | 75.1/步 = 38.6 + 23.2 + 12.7(GC) | 0.576 | **1.74** |
| cycleratio | 8.35 | 0.83（10.0%） | 7.52 | 132.1 = 40.6 + 91.5 | 0.0554 | 18.0 |

`ns/Ir = (总 ns/alloc) / (总 Ir/alloc)`，只对 promote 与 `[step]` 口径做过自洽校验（§5.3）；它不是 per-component 的 IPC，不能拿它逐组件折算（§8-2）。

### 5.2 promote 的"每分配固定成本 5.36 ns"拆开是什么

docs `MEASUREMENTS-V2-AFTER.md:252` 说瓶颈 1 是 "promote mutator 24.4 ms / 4.55 M = 5.36 ns/alloc，与策略无关"。本报告的拆解（按 promote 自己的 ns/Ir = 0.0508）：

| 组成部分 | Ir/alloc | ≈ns/alloc | 说明 |
|---|---|---|---|
| 形状本体（off main） | 39.2 | 1.99 | 9 次 new + 链写 + 每轮走 8 个长命 |
| **内联 rc 快路径** | **67.6** | **3.43** | 屏障 + 每分配簿记；**这才是"固定成本"的主体** |
| 合计 mutator | 106.8 | 5.42 | 与 docs 的 5.36 吻合（2%） |
| 对照：纯 bump（off）分配器 | 31.0 | 1.57 | 若分配器能降到这个水平，promote 可省 ~1.86 ns/alloc（**−21%** 墙钟） |

⇒ 瓶颈 1 的正确表述：**"每分配 5.36 ns" 里有 3.4 ns 是 extC 自己的 rc 分配/屏障路径，不是形状，也不是物理下限；物理下限（bump）只要 1.6 ns/alloc。**

### 5.3 为什么"GC Ir 份额"不能直接当"GC 税"

| 形状 | GC Ir 份额 | GC 墙钟份额 | 比值（墙钟/Ir） | 解释 |
|---|---|---|---|---|
| promote | 39.2% | 40.2% | **1.03** | GC 的 miss 占全局 DLmr 93% ⇒ 指令和访存都贵 |
| cycleratio | 9.3% | 10.0% | **0.99** | sweep 的 ALU 占比高，cache 友好 |
| sizeratio | 21.6% | 10.3% | **0.48** | sweep/shouldSweep 是纯 ALU，GC 每 Ir 比 mutator 便宜 2× |
| chase | 16.8% | 3.6% | **0.21** | 标记是顺序流式扫，miss 可重叠；walk 是串行依赖链 |

**λ 的两个口径**（`λ = GC 时间/分配字节`）：promote 0.150 ns/B（`16.4ms/109.2MB`，docs 0.147）、sizeratio 0.0151、chase 0.116、cycleratio 0.0352；对应的指令口径 `λ_Ir = GC Ir/B` 为 promote **2.89**、sizeratio 0.309、chase 0.949、cycleratio 0.593。两口径比 = 上表比值 ⇒ **`λ = λ_Ir × ns_per_Ir(shape)`，ns_per_Ir 跨形状差 11×，"0.41 ns/B 的全局 c_t"只在 promote 这种吞吐受限形状成立。**

---

## §6 (c) 哪些量能被 GC 侧改动影响（含用户硬约束）

用户硬约束（Lead 转述，2026-11-09）：**"尽可能压低内存占用，所以 A（抬 r）先不管"** ⇒ 不许用抬 `thr` 倍数 / 抬 `EXTC_RC_M_MAX` 换性能。下表只列不改内存预算、不改触发频率的 GC 侧手段。

### 6.1 可动项（按实测上限排序）

| # | 可动量 | 实测证据（本机 callgrind） | 不改内存时的收益上限 | 风险 |
|---|---|---|---|---|
| 1 | **内联分配/屏障快路径** | 内联 rc 比 bump 多 **36.6 / 29.5 / 60.5 Ir/alloc**（promote/sizeratio/cycleratio），chase **23.2 Ir/步**；`shouldSweep` 单独 3.22%（sizeratio）0 miss | promote −21% 指令、cycleratio −40%、sizeratio −26%、chase −31% Ir/步（把快路径压到 bump 水平） | 屏障语义（写引用必须记账/着色）不能丢；`slotOf`/`checkedIndex` 是安全网 |
| 2 | **mark 扫描的访存结构** | promote markStep 30.0% Ir + DLmr 1,024,392（全局 92.5%）+ D1mr 5,405,839（80.9%）；chase mark 16.1% Ir 但仅 3.6% 墙钟 | promote 是唯一"指令=墙钟"的形状：mark Ir 减半 ⇒ 总时间 ~−15%；对 chase/sizeratio 只省 Ir 不省墙钟 | dirty-slab 只扫有分配的 slab / 每类型指针位图（word AND 跳非指针）要改描述符格式，不碰对象头 |
| 3 | **sweep 的 ALU 成本** | sweep 9.73%（sizeratio）、7.14%（cycleratio）、6.37%（promote）；D1mr 仅 7,610 / 9,067 / 32,674 | 上限 = Ir 份额（sizeratio −9.7%、cycleratio −7.1%） | 扫掠语义/预算不变；对 promote 有 4,067 DLmr，别指望纯 ALU 收益 |
| 4 | **屏障慢路径 `shadeSlow`** | 1.74%（promote，3,982 D1mr）、0.38%（chase） | ≤1.7% | 占比小，优先级最低 |
| 5 | **GC 的空间局部性（对 chase 唯一杠杆）** | walk 自身 DLmr 0.0251/步、D1mr 2.505/步命中 L2/L3；GC 占 3.6% 墙钟 | 只能通过对象布局/分配局部性间接影响 walk 的 L1/L2 miss | 属布局课题，不是 GC 记账逻辑；与"确定性布局"约束相关 |

### 6.2 不可动项（不要当优化目标）

- **形状本体**：promote 39.2、sizeratio 27.7、cycleratio 40.6 Ir/alloc；chase walk 38.6 Ir/步。这是基准程序的语义，改它等于改基准。
- **chase walk 的串行 miss 链**：43.3 ns/步 里主体是跨步依赖的 L1 miss（§4），GC 侧无法改（除非改对象布局，见 6.1-5）。
- **THP / 大页**：Lead 实测无差异（§4.3）⇒ 不是杠杆，不要重复投入。
- **触发频率/内存墙（r、thr、M_max）**：被用户硬约束排除；本报告所有 profile 的内存墙取默认（§1.3 的 `M_max_now`/`[mmax]`）。

### 6.3 与 docs §11.7 三条瓶颈的对照（修正版）

| docs §11.7 的说法 | 本报告的证据 | 修正/补充 |
|---|---|---|
| 瓶颈 1：每分配固定成本 5.36 ns/alloc，"与策略无关" | 5.36 = 形状 1.99 + **rc 快路径 3.43**；bump 只要 1.57 | 固定成本**主要是 rc 分配/屏障**，不是形状；改 GC 分配路径可动它（上限 −21% promote） |
| 瓶颈 2：GC 单字节成本 c_t=0.41 ns/B | `λ_Ir` 按形状为 2.89/0.309/0.949/0.593 Ir/B；ns_per_Ir 0.0508→0.576 | **c_t 必须按形状×机器标定**；promote 的 0.150 ns/B 与 docs 0.147 自洽 2%，chase 差 4.7× |
| 瓶颈 3：chase 访存延迟 24% 墙钟，"与 GC 无关" | DRAM 项 ≤1/6；GC 占 71.6% 的 DLmr 但只 3.6% 墙钟；walk 串行 miss 0.0251/步；THP 无差异 | "与 GC 无关"成立；但"访存延迟"应改写为 **L2/L3 层级延迟 + 间接层**，不是 DRAM，也不是 TLB |

### 6.4 内存侧的对照：floor 下探（E18，Lead m03170 实测，本机未重跑）

本报告只谈"不抬内存"的手段；但既然用户的第一硬约束是"压低内存"，这里记录唯一"降内存"的便宜旋钮及其代价（详见 `docs/analysis/MEASUREMENTS-GC-P0.md` §12）：

| 形状 | 默认 `EXTC_RC_FLOOR=1MiB` RSS / 墙钟 | `-DEXTC_RC_FLOOR='((int64_t)1<<18)'`（256 KiB）RSS / 墙钟 | 变化 |
|---|---|---|---|
| sizeratio | 3,244–3,372 kB / 21.9–23.9 ms | 2,088–2,092 kB / 26.4–27.2 ms | **−35% / +15%** |
| cycleratio | 3,036 kB / 13.0–14.4 ms | 2,076 kB / 13.7 ms | **−32% / +4%** |
| promote | 6,492 kB / 不变 | 6,492 kB / 不变 | 0（活集 ~1.8 MB > floor） |

再降到 64 KiB 只多省 0.2–0.4 MB，但墙钟 +35~38% ⇒ **256 KiB 是拐点**。3 reps、`v2run --pin 6`。⇒ 这是一个真实的"内存↓、时间↑"权衡点，可写进 Roofline 的可行域，但不能既降内存又不付时间。

---

## §7 能证明 / 不能证明

**能证明（callgrind，确定性，可重跑）**
1. 四形状的总 Ir、逐函数 self Ir、D1mr/DLmr（§2）——同一二进制重跑逐字相同。
2. `内联 rc = main_on − main_off`、`bump arena = 31.0 Ir/alloc`（四形状一致，§3）。
3. chase 每步的 miss 结构：2.594 D1mr、0.0887 DLmr、GC 0.0636 DLmr；工作集缩小 4× 后 DLmr 掉 75%（§4.2）。
4. GC Ir 份额与 GC 墙钟份额的偏离（比值 0.21–1.03，§5.3）——**用两套独立数据（callgrind + docs 计时）交叉得到**。
5. `-fwrapv` 对 promote 指令数 +2.6%（§8-3），说明"指令数"口径对 CFLAGS 敏感。

**不能证明**
1. 组件级 ns 与 IPC：没有硬件计数器（`perf_event_paranoid=2`），只有形状平均 ns/Ir。
2. 43.3 ns/步 的逐项时间分解：只有 miss 计数与 Ir 计数，没有周期/停顿原因计数器。
3. m=262144 的墙钟收益：没做原生计时（§8-4）。
4. GC 切片/停顿：属 `tests/gc-p0` §8 的口径，本报告不重复。

---

## §8 存疑 / 未验证

1. **CPU 频率不可信**：WSL2 `/proc/cpuinfo` 报 3072 MHz（基频）。promote 的 19.7 G Ir/s 若按 3.07 GHz 折算需要 IPC 6.4（不可能），按 275HX 最高 5.4 GHz 折算 IPC 3.6（合理）。**本报告不报 IPC/周期**，只用 Ir 与 ns。
2. **组件级 ns 是"形状平均 ns/Ir"的近似**：`内联 rc` 与 `GC` 各有一个 Ir 数，但没有各自的 IPC。唯一做过的自洽校验是 promote（mutator 106.8 Ir × 0.0508 = 5.42 ns vs docs 5.36 ns，差 2%）；sizeratio/cycleratio 未校验。
3. **`-fwrapv` 敏感性**：promote 783,553,859 Ir（`-O2`）→ 804,083,982（`+ -fwrapv`），+2.6%；sizeratio 228,668,679 → 228,912,109（+0.1%）、chase 300,268,329 → 300,584,855（+0.1%）、cycleratio 274,088,461 → 274,116,151（+0.01%）。本文用 `-fwrapv` 口径（与 `tests/gc-p0/run.sh` 一致）。**与旧报告（GC-ROOFLINE 引用的 +8.2%/+1.3%/+3.3%/−0.9%）比较时注意 CFLAGS 差异。**
4. **m=262144 未做原生计时**：只做了 callgrind（Ir/DLmr）。"装进 L3 应省 ~5.7 ns/步"是推断；要证实需要同机原生计时，考虑到并行 agent 的计时噪声（`MEASUREMENTS-GC-P0.md` §1.3 的噪声地板），本报告刻意不做。
5. **callgrind 的 cache 模型**：`DLmr` 只计 read miss；`D1mw`/`DLmw`（写 miss）本报告未解读；真实核的写分配、硬件预取、流式检测都比 callgrind 乐观或悲观，**只能用来看相对量与结构，不能当绝对延迟**。
6. **形状集合**：只有 docs §11.1 的 4 个 bench 形状；`align` 档（promote 100000 等）只在早期探索里跑过，未纳入本报告。
7. **工作树已变**：写作时 `src/back/reclaim.c`/`src/back/codegen.c` 已被后续 patch 改动（md5 `c5e10f7a…`/`d3393828…`）；本报告的唯一构建是 `build-emp2/extc` md5 `41efaf57…`（FREEZE v4 源）。复现前先核对。
8. **`main` 的 self Ir 含内联的 rc 代码**：callgrind 不会把被内联的 `extc_rc_alloc`/`storeRef*` 单列为函数，所以 `main` 的 60–88% 里既有形状本体也有 rc 快路径；本文用 `on − off` 差分把它拆开（§3），差分的正确性依赖"off 档只是换分配器、形状代码相同"这一假设（生成物里形状部分确实相同，但 `main` 的寄存器分配/内联决策可能略有不同——未逐指令 diff）。
9. **THP 实验的行为差异**：Lead 的 THP 对照是在生成 C 上手工插 `madvise`（非源码级 patch），RSS 相同的结论只覆盖 slab 区的 huge page 化；若未来改成"对象区整体 huge page 化 + 分配器适配"，结论可能不同（本报告按"当前实现下 THP 无收益"记录）。

---

## §9 复现（写入 `tests/gc-p0/cost/`，不依赖 /tmp）

```bash
# 0) 需要 FREEZE v4 的源（或等价的 build-emp2/extc md5 41efaf5716b25e7ac0a8ddfd5cac5d24）
cd /home/alphayang/extC_Compiler
md5sum build-emp2/extc                # 应为 41efaf5716b25e7ac0a8ddfd5cac5d24

# 1) 四形状 ×（默认档 + EXTC_RECLAIM=0 对照）→ callgrind → 表
EXTC=./build-emp2/extc OUT=/tmp/gc-alloc-cost tests/gc-p0/cost/run.sh

# 2) 只跑一个形状 / 跳过 off 对照
SHAPES=chase NO_OFF=1 EXTC=./build-emp2/extc OUT=/tmp/gc-alloc-cost tests/gc-p0/cost/run.sh

# 3) 已有 profile 时单独解析（Ir + 缓存 miss + 每分配分解）
python3 tests/gc-p0/cost/parse_callgrind.py /tmp/gc-alloc-cost/cg/promote.out     --off /tmp/gc-alloc-cost/cg/promote_off.out --allocs 4550000 --wall-ms 40.8 --gc-ms 16.4

# 4) chase 工作集对照（m=262144）
./build-emp2/extc bench/gc/v2/src/chase.extc -o /tmp/chase.c
cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -o /tmp/chase /tmp/chase.c
valgrind --tool=callgrind --cache-sim=yes --callgrind-out-file=/tmp/chase.out     /tmp/chase 262144 20261013 1000000
python3 tests/gc-p0/cost/parse_callgrind.py /tmp/chase.out --allocs 277772 --steps 1000000

# 5) 每形状的 GC 档位/内存墙（只取插桩行，不取时间）
EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 /tmp/gc-alloc-cost/bin/promote 500000 20261013 2>&1 >/dev/null |     grep -E '^[(work|pause|step|mmax)]'
```

**文件索引**

| 文件 | 作用 |
|---|---|
| `tests/gc-p0/cost/run.sh` | 生成/编译/callgrind 四形状 + off 对照 + 打印表；不测时间 |
| `tests/gc-p0/cost/parse_callgrind.py` | 解析 `callgrind_annotate --auto=no` 输出；算 GC 份额、每分配/每步分解 |
| `docs/analysis/ALLOC-COST.md` | 本文（数字与结论；§8 存疑清单） |
| `docs/analysis/MEASUREMENTS-V2-AFTER.md` §11.1 / §11.7 | 本报告引用的 ns 与"三瓶颈"原文 |
| `docs/analysis/MEASUREMENTS-GC-P0.md` §12 | THP 负结果 + E18 floor 下探的完整记录 |
| `docs/analysis/GC-ROOFLINE.md` §4.3 / §5.1 | 与本文对接的四个可调维度与参数表 |
