# bench/gc/v2 重测与旧数据对比（FREEZE v4）

**一句话结论**：用本轮冻结的编译器（FREEZE v4）重跑 `bench/gc/v2` 四形状六语言，extC 的吞吐相对 HEAD 85b8871b 在 **±7% 噪声带内**（确定性指令数 promote +8.2%、sizeratio +1.3%、chase +3.3%、cycleratio −0.9%），峰値 RSS **+0.6%~+3.5%**；停顿切片的最大値 sizeratio/cycleratio 下降、promote 略升、chase 切片数从 21 增到 1,255（GC 总时间 1.29→6.19 ms，占墙钟 0.8%→3.6%）。旧发布数据（2026-11-01 前后的 `REPORT.md`/`matrix.tsv`）与本次重跑之间的差异主要来自**编译器版本与机器漂移**，不能直接当作本轮的回归量。

## 0 证据口径

| 项 | 值 |
|---|---|
| 机器 | WSL2，Intel Core Ultra 9 275HX，nproc=24（load≈2.1） |
| 编译器（新） | `build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f`；源码 src/back/reclaim.c `ff42799b…`（3607 行）、src/back/codegen.c `cf288544…`（15994 行） |
| 编译器（对照/HEAD） | `/tmp/extc-base/build-base/extc` md5 `829e55a785d9c3f0ec973743e93cf320`，worktree 干净，HEAD `85b8871b` |
| 构建方式 | `bench/gc/v2/run.sh build --lang extc`，即 `$EXTC -w --no-line-map -I stdlib` + `cc -O2 -D_POSIX_C_SOURCE=200809L` |
| 计时 | `bench/gc/v2/shim.c`（`wait4` 内核值），`--pin 6`，每臂 9 次，**同轮交错**跑 hd/nd/ho/no 四臂 |
| 全语言重跑 | 同上，`run.sh bench --reps 5`，产物 `/tmp/v2/full`，TSV `/tmp/v2/full/matrix-after.tsv` |
| 指令数 | `valgrind --tool=callgrind` 跑 `align` 参数（promote 100000 / sizeratio 200000 / chase 65536·200000 / cycleratio 200000），确定性计数 |
| 旧数据 | `bench/gc/v2/matrix.tsv`、`REPORT.md`、`pause_v2.tsv`（旧编译器产物，入库于 2026-11-01 前后） |

三组数据的分工：**hd/nd A/B** 回答"本轮改动有没有让 v2 变慢/变胖"；**callgrind** 回答"工作量变了多少"（不受 VM 噪声影响）；**旧发布 vs 新全量** 回答"和已发布数字差多少"（混入了编译器版本漂移，只能看量级）。

## 1 吞吐：同机同轮 A/B（HEAD → v4）

`hd`=HEAD default、`nd`=v4 default、`ho/no`=两版 `EXTC_RC=0`（bump 臂）。中位取 9 次交错测量。

| shape | hd med ms | nd med ms | 新/旧 | hd min | nd min | 新/旧 |
|---|---:|---:|---:|---:|---:|---:|
| promote | 39.46 | 41.95 | **1.063×** | 38.10 | 39.69 | 1.042× |
| sizeratio | 25.07 | 23.45 | **0.935×** | 23.69 | 22.79 | 0.962× |
| chase | 167.68 | 173.21 | **1.033×** | 159.02 | 170.88 | 1.075× |
| cycleratio | 15.60 | 15.17 | **0.972×** | 14.80 | 14.67 | 0.991× |
| promote:off | 92.66 | 91.84 | 0.991× | 88.92 | 89.87 | 1.011× |
| sizeratio:off | 62.45 | 63.22 | 1.012× | 60.38 | 60.86 | 1.008× |
| chase:off | 120.06 | 121.27 | 1.010× | 115.25 | 116.40 | 1.010× |
| cycleratio:off | 24.04 | 24.00 | 0.999× | 23.17 | 22.57 | 0.974× |

**off 臂逐项不变**（±1%），符合预期：`EXTC_RECLAIM=0` 产物里没有回收集合运行期，本轮的 reclaim.c 改动不进入该臂；codegen 只多出少量根登记/宏定义代码，实测未可分辨。

## 2 吞吐：确定性指令数（不受 VM 噪声）

| shape | HEAD Ir | v4 Ir | 变化 |
|---|---:|---:|---:|
| promote default | 115,211,310 | 124,678,428 | **+8.2%** |
| sizeratio default | 22,812,974 | 23,118,726 | +1.3% |
| chase default | 21,504,392 | 22,209,323 | +3.3% |
| cycleratio default | 138,523,474 | 137,270,386 | −0.9% |
| promote/sizeratio/chase/cycleratio **off** | 64,438,099 / 13,045,001 / 11,264,144 / 70,410,391 | 同左（差 ≤14 条） | **0%** |

指令数与墙钟方向一致但不成比例（promote Ir +8.2% 而中位 +6.3%；sizeratio Ir +1.3% 而墙钟 −6.5%），因为 v2 的墙钟含缺页与内存带宽成分——`extc:off` 臂在这些形状上反而更慢（promote 全量分配 107 MB）。**结论：本轮在 v2 上的确定性额外工作量 ≤ +8.2%（仅 promote），其余 ≤ +3.3%，cycleratio 为零。**

## 3 内存（峰値 RSS，9 次取最大）

| shape | hd kB | nd kB | 新/旧 | ho/no kB |
|---|---:|---:|---:|---:|
| promote | 7,704 | 7,784 | 1.010× | 109,564 / 109,564 |
| sizeratio | 7,688 | 7,828 | 1.018× | 157,948 / 157,948 |
| chase | 71,188 | 73,692 | **1.035×** | 62,140 / 62,140 |
| cycleratio | 7,712 | 7,760 | 1.006× | 44,476 / 44,476 |

上升最大的 chase 正好是唯一"活集接近隐式内存墙"的形状：v4 的 `M_max=3·L≈69.2 MB`（`[work] M_max=69206064`），实测 RSS 73.7 MB 即贴着 `M_max + 缓冲`；HEAD 没有这条墙，靠触发阈值自然停在 71.2 MB。**这是契约的直接代价：用 +3.5% 常驻换可预言峰值。**

## 4 与旧发布数据（matrix.tsv）对比

同一台机器、同一 `run.sh bench --reps 5`，旧入库值 → 本次全量重跑（六语言同场）：

| shape | extC arm | 旧 min ms | 新 min ms | 比 | 旧 中位 | 新 中位 | 比 | 旧 RSS | 新 RSS | 比 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| promote | default | 41.72 | 41.53 | 0.995× | 45.55 | 43.40 | 0.953× | 7,664 | 7,612 | 0.993× |
| promote | off | 82.22 | 81.12 | 0.987× | 90.07 | 85.93 | 0.954× | 109,564 | 109,564 | 1.000× |
| sizeratio | default | 29.49 | 23.83 | 0.808× | 30.67 | 24.31 | 0.793× | 7,568 | 7,708 | 1.018× |
| sizeratio | off | 66.19 | 60.96 | 0.921× | 71.05 | 62.90 | 0.885× | 157,948 | 157,948 | 1.000× |
| chase | default | 156.65 | 168.55 | 1.076× | 181.74 | 174.31 | 0.959× | 71,188 | 73,692 | 1.035× |
| chase | off | 110.96 | 115.52 | 1.041× | 145.37 | 128.71 | 0.885× | 62,204 | 62,136 | 0.999× |
| cycleratio | default | 16.86 | 15.14 | 0.898× | 17.31 | 16.31 | 0.943× | 7,404 | 7,656 | 1.034× |
| cycleratio | off | 24.42 | 22.47 | 0.920× | 25.32 | 24.84 | 0.981× | 44,540 | 44,476 | 0.999× |

**机器漂移对照**：同一次重跑里 36 个非 extC 臂（go/java/rust/node/cpp）的 min 之比中位 **1.016**（本次整体慢 1.6%），p10–p90 为 0.952–1.109。因此 sizeratio 的 −19% 与 chase 的 +7.6% 超出漂移带、需要解释：

- sizeratio 的 −19% 不能归给本轮（同机 A/B 是 −6.5%，且 HEAD-now 的 25.07 ms 已比旧表 29.49 ms 低 15%）⇒ **旧表与 HEAD-now 之间本身就存在编译器/构建差异**（旧表由 11-01 前后的编译器产物测得，不是 HEAD）。
- chase 的 +7.6% 里，同机 A/B 只占 +3.3%；其余来自漂移与机器状态。
- 旧表 `cpp:careful` 行与 `cpp:default` 几乎相同（77.4 vs 81.7 ms、RSS 均 191 MB），而本次重跑为 **19.4 ms / 7.6 MB**（REPORT 概览里 C++ 也是 77.4）⇒ **旧表该行记录的不是真正的 careful 二进制**（或当时代码不同），不要把它当基线。

## 5 跨语言位次（本次全量重跑，min ms）

| shape | extC | Go | Java | Rust | Node | C++ |
|---|---:|---:|---:|---:|---:|---:|
| promote | 41.5 (1.00×) | 110.4 | 105.2 | 39.5 (0.95×) | 111.6 | 105.6 |
| sizeratio | 23.8 (1.00×) | 46.7 | 100.3 | 21.5 (0.90×) | 94.4 | 19.4 (0.81×, careful) |
| chase | 168.5 (1.00×) | 164.2 | 191.5 | 240.2 | 1119.0 | 128.7 (0.76×, careful) |
| cycleratio | 15.1 (1.00×) | 34.8 | 79.1 | 19.1 (careful) / 53.1 | 65.3 | 36.3 (2.40×) |

位次与旧 REPORT 一致：extC 在 promote/sizeratio/cycleratio 属第一梯队（与 Rust 相差 ≤10%），chase 与 Go/C++ 同档但落后 C++ careful 24%。

## 6 停顿

### 6.1 旧发布（pause_v2.tsv，extc 行，`EXTC_RC_SLOWSTEP=0.001` 的 `[slow]` 事件）

| shape | events | p50 µs | p99 µs | p99.9 µs | max µs | 占墙钟 |
|---|---:|---:|---:|---:|---:|---:|
| promote | 20,648 | 0.40 | 3.20 | 34.68 | 116.40 | 18.6% |
| sizeratio | 5,776 | 0.30 | 2.30 | 8.65 | 53.00 | 6.5% |
| chase | 21 | 75.30 | 99.46 | 101.75 | 102.00 | 0.8% |
| cycleratio | 4,018 | 0.40 | 1.60 | 3.09 | 83.20 | 9.2% |

### 6.2 干净的内建计量（`EXTC_RC_PAUSE=1`，不开 SLOWSTEP 日志）

`[step]` 与 `[histo]` 统计的是**每一个 mark/sweep 切片**（分配点驱动的切片也算），`[pause]` 只统计分配点触发的整步。同一二进制、同一参数：

| shape | 版本 | 切片数 | 总 GC µs | max µs | >16 µs | >64 µs | GC/墙钟 |
|---|---|---:|---:|---:|---:|---:|---:|
| promote | HEAD | 20,648 | 15,806 | 76.1 | 88 | 1 | 40.1% |
| promote | v4 | 21,288 | 17,095 | 103.9 | 20 | 1 | 40.8% |
| sizeratio | HEAD | 5,776 | 2,197 | 27.6 | 2 | 0 | 8.8% |
| sizeratio | v4 | 2,044 | 2,408 | 27.3 | 3 | 0 | 10.3% |
| chase | HEAD | 21 | 1,287 | 100.5 | 16 | 14 | 0.77% |
| chase | v4 | 1,255 | 6,192 | 97.3 | 42 | 3 | 3.6% |
| cycleratio | HEAD | 4,018 | 1,792 | 162.1 | 3 | 1 | 11.5% |
| cycleratio | v4 | 1,221 | 1,523 | 44.7 | 1 | 0 | 10.0% |

v4 的 `[histo]`（15 桶，桶名是上界）逐形状：

- promote：`b1us=13838 b2us=5731 b4us=1459 b8us=218 b16us=22 b32us=13 b64us=6 b128us=1` ⇒ **99.8% 的切片 ≤8 µs，只有 1 片 >64 µs**。
- sizeratio：`b1us=814 b2us=984 b4us=229 b8us=10 b16us=4 b32us=3` ⇒ 全部 ≤32 µs。
- chase：`b1us=698 b4us=7 b8us=214 b16us=294 b32us=6 b64us=33 b128us=3` ⇒ 切片变多、单片变小（p50 亚 µs）。
- cycleratio：`b1us=315 b2us=865 b8us=39 b16us=1 b64us=1` ⇒ 全部 ≤64 µs，最大从 HEAD 的 162 µs 降到 44.7 µs。

### 6.3 方法学警告：SLOWSTEP 日志会污染停顿测量

同一台机器、同一二进制，只把 `EXTC_RC_SLOWSTEP=0.001` 打开（旧 pause 卡具的取数方式，每片都 fprintf 一行到 stderr），HEAD 的 `[slow]` 最大值从干净计量的 **76.1 µs 涨到 8,071 µs**（v4：103.9 → 4,242 µs），而 `<16 µs` 的占比几乎不变。⇒ **用日志方式取 ``[slow]`` 时 max/p99.9 不可作为判据**（本报告 §6.2 用 `[step]`/`[histo]`，不写日志）。旧 pause_v2.tsv 正是日志口径，不要把它的 max 与本轮干净值直接比较。

### 6.4 chase 的节奏变化（需归因）

chase 是唯一 GC 时间显著上升的形状：切片 21 → 1,255、总 GC 1.29 → 6.19 ms（占墙钟 0.8%→3.6%）、墙钟 +3.3%。同一形状 RSS 只从 71.2 → 73.7 MB，说明 v4 的隐式 `M_max=3·L` 让该形状**更早、更频繁地进入回收**（`[work] pressure_steps=430`、`scale_max=16`）。这是"内存墙换可预言峰值"的预期行为，但触发点还应做一次归因（属 E 系列未完成项），确认没有把一次清扫拆成过多小切片。

## 7 发现的回归与遗留

1. **stderr 噪声（新）**：`M_max` 小于 `floor` 时 v4 会打印一次 `[mmax] memory wall unreachable: M_max=… floor=…; using floor`（src/back/reclaim.c 的 thrUpd 内，`extc_rc_wallWarned`）。v2 的 SPEC 要求被测程序 **stderr 为空**，本次 promote/sizeratio/cycleratio 每次运行都会打 1 行。建议改成只在显式设置 `EXTC_RC_M_MAX` 时告警，或走 `EXTC_RC_STATS`/`EXTC_RC_PAUSE` 诊断开关。
2. **旧表 `cpp:careful` 行无效**（见 §4），重跑已自动修正。
3. **chase 切片数激增**（见 §6.4）需要在标定阶段归因；它不违反内存契约，但把 GC 时间占比从 0.8% 推到 3.6%。
4. 本机 `perf stat` 不可用（"No supported events found"，WSL2 无 PMU）⇒ 指令数只能用 callgrind 在小规模参数上取，不能直接对 bench 规模计数。

## 8 判读

1. **吞吐**：本轮改动在 v2 上的确定性附加工作量 ≤ +8.2%（promote 单形状），其余 ≤ +3.3%；墙钟全部落在 ±7% 噪声带内，**没有系统性回归**。给 digraph 记的 +5.2% 指令数在 v2 上没有被放大。
2. **内存**：+0.6%~+3.5%，且增量集中在"活集贴墙"的 chase；off 臂完全不变。
3. **停顿**：最大切片 cycleratio 162→44.7 µs、sizeratio 27.6→27.3 µs 但片数 2.8× 减少；promote 有一片 103.9 µs（HEAD 76.1 µs），仍在 0.1 ms 量级；chase 用更多更小的切片换回提前回收。
4. **与旧数据**：差异主要由编译器版本（旧表非 HEAD）与机器漂移（非 extC 臂中位 +1.6%）解释，唯一确定的本轮效应来自同机 A/B 与 callgrind 两列。

## 9 复现命令

```bash
# 1) HEAD 对照编译器
git worktree add /tmp/extc-base 85b8871b    # 已存在则跳过
(cd /tmp/extc-base && make BUILDDIR=build-base -j24)

# 2) 两套 v2 产物
EXTC=/tmp/extc-base/build-base/extc B=/tmp/v2/head bash bench/gc/v2/run.sh build --lang extc
EXTC=/home/alphayang/extC_Compiler/build-rf/extc B=/tmp/v2/new bash bench/gc/v2/run.sh build --lang extc

# 3) 交错 A/B（hd/nd/ho/no × 9 次，pin 6）：脚本 /tmp/v2/ab.sh
bash /tmp/v2/ab.sh

# 4) 判定性指令数
bash /tmp/v2/ir.sh          # callgrind；每行 shape build arm Ir

# 5) 干净停顿计量
EXTC_RC_PAUSE=1 /tmp/v2/new/promote.extc 500000 20261013
#   stderr 的 [step]/[histo] 即切片分布；不要同时设 EXTC_RC_SLOWSTEP

# 6) 旧口径复跑（会污染 max，仅供与 pause_v2.tsv 同法对照）
B=/tmp/v2/new bash bench/gc/v2/pause/extc.sh /tmp/v2/pause-new

# 7) 六语言全量重跑
EXTC=/home/alphayang/extC_Compiler/build-rf/extc B=/tmp/v2/full REPS=5 TSV=/tmp/v2/full/matrix-after.tsv \
  bash bench/gc/v2/run.sh bench
```

## 10 产物清单

| 路径 | 内容 |
|---|---|
| `/tmp/v2/head`, `/tmp/v2/new` | 两版四形状 extc default/off 产物与 `.extc.c` |
| `/tmp/v2/ab/raw_*.txt` | A/B 原始 9 次测量行（`ms= rss_kb= utime_ms= …`） |
| `/tmp/v2/ir.sh` 输出 | callgrind 指令数（本报告 §2） |
| `/tmp/v2/full/matrix-after.tsv` | 六语言 × 四形状 × 各臂的新全量表 |
| `/tmp/v2/diag_{head,new}_*.err` | `EXTC_RC_PAUSE=1` 的 `[pause]/[step]/[histo]/[work]` 原始行 |
| `/tmp/v2/pause-head`, `/tmp/v2/pause-new` | 旧日志口径卡具的重跑结果（含 `.events/.hist/.meta`） |

## 11. 逼近上限的四个探针（2026-11-09，同 build-rf 产物，单变量）

目的：回答「还能从哪里换性能」。每个探针只改一行/一个宏，用 `tests/gc-p0/tune/tune.sh` 复现；
口径同 §1（`/tmp/v2/v2run --pin 6`，5 reps，交错不可用故取中位；WSL2 噪声带 ±7%）。

### 11.1 每个形状的分配带宽 a、回收成本 lambda 与自有屋顶

分配字节由形状源码静态算出（promote 500000 轮 × (9 短命 + 0.1 提长命) × 24 B = 109.2 MB；
sizeratio 2 000 000 × 平均 80 B = 160 MB；chase 1 048 576 × 48 B + 15 625×4×48 B = 53.3 MB；
cycleratio 400 000 × 4.5 × 24 B = 43.2 MB）。GC 时间取 `[step] total_us`。

| 形状 | 分配字节 | GC 时间 | lambda [ns/B] | a [GB/s] | mutator 时间 | mutator a [GB/s] | 自有屋顶 1/(lambda+1/a_peak) | 命中率 | 主要墙 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| promote | 109.2 MB | 16.4 ms | 0.147 | 2.60 | 24.4 ms | 4.48 | 4.08 GB/s | 64% | GC 税 40.8% + 每分配固定成本 |
| sizeratio | 160 MB | 2.41 ms | 0.0151 | 6.82 | 21.0 ms | 7.61 | 8.84 GB/s | 77% | mutator 快路径（距平台 1.5x） |
| chase | 53.3 MB | 6.19 ms | 0.116 | 0.31 | 167 ms | 0.32 | 4.67 GB/s | 7% | 访存延迟（不在带宽平面内） |
| cycleratio | 43.2 MB | 1.52 ms | 0.035 | 2.85 | 13.7 ms | 3.17 | 7.52 GB/s | 38% | 每分配固定成本 7.6 ns |

mutator 时间 = 墙钟 − GC 时间（同一 rep，勿跨 rep 相减）；a_peak 取 10.2 GB/s（CALIBRATION §8 的下界）。

### 11.2 负结果一：切片粒度不是杠杆

promote 用 `-DEXTC_RC_SWEEP_BUDGET=64/256` 重编（默认 8）：切片数 21 288 → 9 438 / 8 172，
但 `[step] total_us` 14 855 / 15 325 / 15 173 µs（默认/64/256）**不变**，最大切片 43.8 → 79.1 / 80.3 µs。
结论：GC 成本是真工作量（按字节/对象），不是每片固定开销；调大 sweep 批量只增停顿、不省时间。

### 11.3 负结果二：小对象 memset 不是杠杆

把生成 C 里 24 B 槽的 `memset(p,0,ss)` 改成只对 ss>32 生效（语义上 promote 的 node 三个字段都会被写，
table 仍整片清零），promote 中位 34.0 → 35.1 ms（噪声内，未变快）。
结论：每分配清零在 -O2 下已内联展开，量级 ~1 ns 以下；不要从这里找收益。

### 11.4 负结果三：只抬 M_max 不改触发线无效

`EXTC_RC_M_MAX` = 12 MiB / 24 MiB：`[work] thr_max` 仍是 3 441 616 B（= 2 x live，wall 夹取未生效），
GC 16.6 → 15.7 / 15.8 ms（−5%，噪声边缘）。M_max 是**天花板**，不是目标线。

### 11.5 正结果：触发倍数（策略 r）是当前唯一免费的大杠杆

把 `thrUpd` 的 `thr = max(2*live, floor)` 改成 4*live / 8*live（同时 `EXTC_RC_M_MAX=32 MiB` 让墙不夹取）：

| 变体 | collects | GC 时间 | 墙钟中位 | RSS | 备注 |
|---|---:|---:|---:|---:|---|
| thr=2（现状） | 82 | 16.4 ms | 40.9 ms | 6 684 kB | 基线 |
| thr=4 | 37 | **9.6 ms** | **34.0 ms（−17%）** | 10 140 kB | 甜点 |
| thr=8 | 16 | 6.8 ms | 34.3–37.6 ms | 16 668 kB | 缺页 sys 7.5–11 ms，收益倒挂 |

同法施加到 sizeratio（23.5 → 23.0–23.8 ms，RSS 3.2 MB 不变）与 cycleratio（61.8 → 60.5–62.3 ms，RSS 3.0 MB 不变）**无变化** ——
与模型一致：只有 lambda 显著高于地板（0.014）的形状才值得抬 r；sizeratio 的 lambda 已是 1.08x 地板。

34.0 ms 已比旧表 rust 39.5 / cpp 41.7 快约 14%，说明「promote 输给 rust」不是 GC 算法差，而是默认触发线偏保守。

### 11.6 但旋钮不能单独上线：M_max 与 thr 现在互相锁死

隐式 `M_max = 3*live`，而墙夹取是 `thr <= M_max - live/4`（b=256 时 64*L/b = L/4）：
thr=4*live = 6.8 MB > 3live − live/4 = 4.7 MB ⇒ 不显式给 `EXTC_RC_M_MAX` 时 thr 会被夹回 ~4.7 MB，
实测收益被吃掉一半。所以上线顺序必须是：
1. 把触发倍数做成旋钮（`EXTC_RC_THR_MUL`，默认 2，取值 2..8，参与 thrUpd）；
2. 把隐式 M_max 从 `3*live` 改成「由目标 r 与 floor 推出」或要求显式预算；
3. 同步放宽 G4/G10 判据（峰值 <= M_max + 缓冲，M_peak <= (r+1)L + 缓冲）。

### 11.7 上限还差多少：三个可打的真瓶颈

| 瓶颈 | 证据 | 可回收量 | 打它的手段 | 需要先做 |
|---|---|---|---|---|
| 每分配固定成本 | promote mutator 24.4 ms / 4.55 M = **5.36 ns/alloc**，与策略无关；cycleratio 7.6 ns/alloc | promote 即使 GC=0 仍 4.48 GB/s（平台 44%） | 分配快路径内联、大小类缓存、owner/描述符查表缓存、屏障调用去虚拟化 | callgrind 小 N 的 per-alloc 归因（E16） |
| GC 单字节成本 c_t=0.41 ns/B | lambda 中的主项；清扫 F_slab=1.30 | 标记成本可望减半（lambda −30%~−50%） | 每类型指针位图（word AND 跳过非指针）、dirty-slab 只扫有分配的 slab | 描述符格式改动（不碰对象头，符合布局确定性约束） |
| 访存延迟（chase） | GC 仅 3.6%，a=0.31 GB/s，cpp careful 128.7 vs extC 168.5 ms | 24% 墙钟（对照 cpp） | 布局/预取/减少间接层；与 GC 无关 | 单独的 chase 课题 |

结论：**「逼近上限」= 先把 r 从 2 抬到 4（已测 −17%，代价 +3.5 MB），再啃每分配 5.4 ns 的 mutator 固定成本**；
lambda 的结构项（标记/清扫）只有在 r 抬到平台后才成为主项（当前 λ 每降一半，promote 只再省 ~5 ms）。

## §12 内存优先一轮（用户 m03301 之后）：三条瓶颈的实测天花板与负结果

> 约束：**不抬 r（触发倍数）、不抬 M_max**，只做「每分配固定成本」「λ 的 c_t（标记）」「chase 访存延迟」三条。
> 本节所有数字都来自 FREEZE v4 构建（build-rf/extc `9fd0dcd861374668be3c74c342beac6f`，源 reclaim.c `ff42799b…`/3607 行、codegen.c `cf288544…`/15994 行），除 §12.3 的 floor 变体与 §12.5 的补丁后测量。

### 12.1 sweep 的面积：跳过在 promote 上无效，在 cycleratio 上才有效

方法：在 FREEZE v4 的生成 C 上插桩（不改编译器）——`sweepBegin` 快照每个 slab 的 `popcount(occ)`；`sweepWord` 完成该 slab 时重算 `popcount(occ)`，差即本轮该死槽数；`atexit` 打印累计。

| 形状 | slabs（访问次数） | full_dead | full_dead_slots | full_live | dead_slots | live_slots | words |
|---|---:|---:|---:|---:|---:|---:|---:|
| promote 200k | 879 | **0** | 0 | 74 | 1,748,760 | 481,901 | 36,117 |
| sizeratio 2M | 2394 | 3 | 2,171 | 171 | 1,995,915 | 195,544 | 44,289 |
| cycleratio | 756 | **651** | 1,739,978 | 63 | 1,778,862 | 54,651 | 30,870 |

读法（这三条决定了投资排序）：

1. **promote 一个 slab 都不全死**（长短命对象在 slab 内交错）⇒ 「全死整块回收」收益为 0；「全活跳过」只覆盖 74/879 = 8% 的访问，而 word 循环总量只有 36,117 word（≈0.2 M Ir）。theory 的 MEMORY-FIRST-MODEL §5.1 已据此把这条拆成 L1a（不可达，promote/sizeratio）与 L1b（可达，cycleratio）。
2. promote 的 sweep 成本 100% 落在 175 万个死槽的逐个 `freeSlotWord` 上 ⇒ 想让 promote 的 sweep 变便宜，必须让**释放死槽本身**变便宜，而不是跳过扫描。
3. **cycleratio 相反：97.8% 的死槽集中在 full_dead slab 里**（651/756 次访问全死）⇒ 「全死 slab 批量重置」是唯一真收益点（实测见 §12.5）。

复现：`/tmp/mf/probe/patch2.py`（promote）、`/tmp/mf/probe/patch4.py`（sizeratio/cycleratio）；产物 `/tmp/mf/probe/*_probe.c`；运行 `./bin/*_probe <bench args>`，stderr 出 `[probe] slabs=… full_dead=…` 一行。脚本只在生成 C 上插桩，不需要重编编译器。

### 12.2 负结果：chase 的 TLB/THP 不是杠杆

在 chase 产物的 `slabNew` 里 `aligned_alloc` 之后插 `madvise(s, 1 MiB, MADV_HUGEPAGE)`（`-D_GNU_SOURCE` 编，系统 THP 策略是 `madvise`）：

| 变体 | 墙钟中位（5 reps，`v2run --pin 6`） | RSS |
|---|---:|---:|
| base | 171.4 ms | 73,692 kB |
| + MADV_HUGEPAGE | 171.0 ms | 73,692 kB |

⇒ 无差异。empirics 的 callgrind 归因（ALLOC-COST.md §4）也支持这一点：chase 每步只有 0.0887 次 last-level read miss（其中 walk 自身 0.0251），主体是 2.594 次/步 L1 miss 命中 L2/L3 ⇒ 延迟在层级缓存，不在 TLB/DRAM。**THP 已实测排除，不要再投。**

### 12.3 floor 下探：memory-first 下唯一还没被暴露的旋钮

`EXTC_RC_FLOOR`（`#ifndef` 保护，1 MiB）决定小活集形状的触发线；用 `-DEXTC_RC_FLOOR='((int64_t)1<<18)'`（256 KiB）与 `1<<16`（64 KiB）重编生成 C，3 reps、`v2run --pin 6`：

| 形状 | floor=1 MiB | floor=256 KiB | floor=64 KiB |
|---|---|---|---|
| sizeratio 2M | 3244–3372 kB / 21.9–23.9 ms | **2088–2092 kB（−35%）** / 26.4–27.2 ms（+15%） | 1896–1900 kB（−42%）/ 30.1–31.5 ms（+~35%） |
| cycleratio | 3036 kB / 13.0–14.4 ms | **2076 kB（−32%）** / 13.7 ms（+4%） | 1692 kB（−44%）/ 18.6–19.1 ms（+38%） |
| promote 500k | 6492 kB / 41.6–43.1 ms | 6492 kB（不变） | 6492 kB（不变） |

读法：floor 只对「活集 ≪ floor」的小堆形状有效，**256 KiB 是拐点**（再降到 64 KiB 只多省 0.2–0.4 MB，墙钟却 +35–38%）。promote 活集 ~1.8 MB > floor ⇒ 完全无关。建议把 `EXTC_RC_FLOOR` 暴露成运行期旋钮（默认 1 MiB 不变），小堆形状可选 256 KiB 换 −1 MB RSS / +4~15% 墙钟。

### 12.4 三条瓶颈的天花板（不抬 r / M_max 的前提下）

| 形状 | 墙钟 | GC（mark/sweep） | 非 GC | 可拿回的量 | 唯一杠杆 |
|---|---:|---|---:|---|---|
| promote | 37.9 ms | 11.66 + 3.51 ms（40.0%） | 22.74 ms | 标记减半 ⇒ −15.4%；sweep 跳过 ⇒ 0（§12.1） | mark 的访存结构与重扫（指针位图/dirty slab） |
| sizeratio | 21.6 ms | 0.78 + 1.46 ms（10.4%） | 19.32 ms | ≤ −8.6% | 内联分配快路径（净增 29.5 Ir/alloc） |
| cycleratio | 13.1 ms | 0.27 + 1.25 ms（11.6%） | 11.59 ms | −6~9.5%（批量重置，实测 §12.5） | 全死 slab 批量重置 |
| chase | 144.2 ms | 5.91 + 0.01 ms（4.1%） | 138.24 ms | ≤ −2.1% | 布局/预取（与 GC 无关） |

三句话：

1. **promote 的 40% GC 里，sweep 那 9.2% 是拿不回来的**（§12.1 实测），只能打 mark（11.66 ms）。mark 的工作量是「每轮把活集全扫一遍」，要减只能减少重扫（dirty-slab / 指针位图），不是微优化。
2. **sizeratio/cycleratio/chase 的 88–96% 时间在 mutator 自身**：GC 归零也只省 4–12%，真正能动的是分配点内联快路径（净增 29.5/60.5 Ir/alloc）与数据布局。
3. 因此 memory-first 下的总天花板约为：promote −15%、sizeratio −9%、cycleratio −6~10%、chase −2%；**用户禁止的抬 r（2→4，实测 −17%）才是唯一 >10% 的单步收益**，这条要如实写进结论。

### 12.5 本轮补丁的实测（P1-7 批量重置 + P1-8 事件档 initRef）

口径：串行、机内无其他负载；A/B **三臂交错 9 reps**（`v2run --pin 6`，stdout 与参考逐字节比对）：`hd` = HEAD 85b8871b 基线编译器、`be` = elide 前（build-rf `9fd0dcd8…`）、`nd` = 最终树（`src/back/reclaim.c 9af9ca9b…` 3667 行 + `src/back/codegen.c d3393828…` 16017 行，我自建 `build-lead5/extc 9bef5488…`；不同 BUILDDIR 的 extc md5 不同，**溯源以源码 md5 为准**）。

| 形状 | hd 中位 | be 中位 | nd 中位 | nd/be | nd/hd | RSS 最大（hd / be / nd，kB） |
|---|---:|---:|---:|---:|---:|---|
| promote | 41.46 ms | 44.07 ms | 43.59 ms | 0.989 | 1.051 | 7,792 / 7,708 / 7,760 |
| sizeratio | 27.70 ms | 26.28 ms | 25.94 ms | 0.987 | 0.936 | 7,764 / 7,780 / 7,708 |
| chase | 186.33 ms | 197.13 ms | 190.27 ms | 0.965 | 1.021 | 71,188 / 73,692 / 73,692 |
| cycleratio | 15.68 ms | 15.51 ms | 15.38 ms | 0.991 | 0.981 | 7,824 / 7,780 / 7,800 |

读法：`nd/be` 的差异就是本轮补丁（elide + `[mmax]` 静默 + P1-8）的全部影响，四项都在 0.965–0.991（不劣于 be），但同轮 `hd/be` 本身就有 0.95–1.06 的漂移 ⇒ **墙钟只给「无回退」的定性结论，收益判定以 Ir 为准**（cycleratio −3.56%）。RSS 三项之差 ≤1.5%，属 slab 取整与运行时文本长度差。

停顿（`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`、`taskset -c 6`、cycleratio，[histo] 单 rep 抽样）：

| 版本 | 步数 n | b1us | b2us | b4us | b8us | b16us | b32us |
|---|---:|---:|---:|---:|---:|---:|---:|
| be（无 elide） | 1,221 | 307 | 853 | 16 | 41 | 2 | 2 |
| nd（有 elide） | 255 | 102 | 106 | 4 | 42 | 1 | 0 |

⇒ 步数 **−4.8×**、b16us 2→1、b32us 2→0；nd 的 `[sweepelide]` = `bulk_reset=651 bulk_slots=1739978 swept=105`，与 §12.1 探针的 `full_dead=651 / full_dead_slots=1,739,978` **逐位一致**。runtime-facts 的 3 reps 版本（on max 5.2/5.3/9.9 µs、off max 5.6/37.7/21.6 µs）同向。

P1-8（事件档 `initRef` → `inc`）：事件档强制 500k 指令 1,380,289,371 → 1,365,221,270（−1.09%）；**TIER 2 / TIER 3 / 无 rc 单位产物逐字节相同** ⇒ 默认路径不可能回退。

stderr 契约：最终编译器四个形状默认档 **stderr 0 字节**；elide 前（`be`）在 sizeratio/cycleratio 上仍打 `[mmax] memory wall unreachable: …`，最终版静默 ⇒ `bench/gc/v2` SPEC 的「stderr 必须为空」满足。

我的独立复核（与 runtime-facts 不同 BUILDDIR）：`tests/reclaim/run.sh` **169/0**、计数档 `EXTC_RC_NOCOUNT=0` **218/0**、`tests/reclaim-perf/gate.sh` **86,354,215 Ir ≤ 88,029,115**、`tests/reclaim-gaps/gate-release.sh` **2/4**（A/D 两条旧断言，§10 已登记）。
