# 跨语言停顿分布（v2 停顿时长直方）

回答的问题：**同一形状下，各语言的 GC 停顿事件有多长、分布是什么形状**——不再只报 max。

## 0. 口径（重要）

每个语言报的是**它自己的运行期承认的停顿事件**（runtime-reported stop-the-world / pause）：

| lang | 事件来源 | 一句话定义 |
|---|---|---|
| extc | `EXTC_RC_SLOWSTEP=0.001` 的 `[slow]` 行 | 单个有界回收集合步（mark 或 sweep）的墙钟时长 |
| go | `GODEBUG=gctrace=1` 或 `runtime/metrics` | 每个 GC 周期里两段 STW（sweep termination + mark termination）的墙钟时长 |
| java | `-Xlog:gc` | 每条 GC 暂停事件（G1 或 SerialGC）的墙钟时长 |
| node | `--trace-gc-nvp` | 每次 GC（Scavenge / Mark-Compact…）的 pause |

定义**不是**同一件事，报告里必须逐语言标注 `source`；能对齐的是形状、分位、超阈占比、事件频度、占墙钟比例。

Rust / C++ 没有 GC（手工释放），不进本表。

## 1. 产物布局

```
bench/gc/v2/pause/
  README.md          本文件
  extc.sh go.sh java.sh node.sh        每个语言一个 runner（<lang>/ 目录是该 runner 的产物）
  extc/  go/  java/  java_serial/  node/   <arm> 目录
    <shape>.events   可选：每个停顿事件的时长，一行一个（单位 µs，浮点）
    <shape>.hist     必需：TSV，三列 `bucket_lo_us\tbucket_hi_us\tcount`（最后一档 hi 写 `inf`）
    <shape>.meta     必需：KEY=VALUE，一行一个
```

> `.events` / `.hist` 是 runner 写出的**原始流、不入库**（跑一遍即得）；入库的是 `.meta` 与聚合产物
> （`PAUSE.md` / `pause_v2.tsv` / `pause.png`）。见 `check.sh` 的 `check-tree`。

### 1.1 meta 必需键

| 键 | 含义 |
|---|---|
| `lang` | `extc` / `go` / `java` / `node` |
| `arm` | 臂名，与目录名一致（`extc` / `go` / `java` / `java_serial` / `node`） |
| `shape` | `promote` / `sizeratio` / `chase` / `cycleratio` |
| `args` | 传给形状的命令行参数（原样） |
| `events` | 停顿事件条数（0 必须显式写 0） |
| `total_us` | 停顿总时长 µs（= Σ 事件） |
| `wall_ms` | 该次运行（带诊断）的墙钟 ms |
| `wall_plain_ms` | 同形状不带诊断那次的墙钟 ms（`share_pct` 用它做分母） |
| `source` | 事件怎么来的，一句话（含具体 flag / godbg 变量） |
| `notes` | 自由文本；空写 `-` |

可加派生键（如 extc 的 `events_mark`/`total_us_mark`）。

### 1.2 可选但强烈建议：`total_gc_us`

`total_us` 只是**停顿**，而「停顿」的定义逐语言不同（§0）。所以报告还有第二张表
**「Σ GC 归因时间」**：该次运行里可观测到的**全部** GC 时间。请尽量提供：

| 臂 | `total_gc_us` 填什么 |
|---|---|
| extc | `step_total_us`（Σ 全部有界回收步；extC 没有独立 GC 线程 ⇒ 停顿就是全部 GC 工作） |
| go | Σ(A+B+C)（gctrace 每行三段之和；`GOMAXPROCS=1` 下并发标记也占同一条核） |
| java | Σ 暂停 + Σ 并发阶段（并发段要 `-Xlog:gc+phases`；SerialGC 全 STW ⇒ 与 `total_us` 同值） |
| node | Σ 每次 GC 的整段时长（`pause=` 只是其中被停止的一段） |

拿不到就写 `total_gc_us=-` 并在 `notes` 里说明原因——**不许瞎凑**。聚合器对 `-` 留空。

> 为什么必须有这一栏：extC 的停顿步就是它的**全部 GC 工作**（没有独立 GC 线程），
> 而 Go/Java/Node 的 STW 只是停止时间、并发阶段不计。只比停顿会出现
> 「extC 占墙钟 14% vs Go 0.2%」这种口径错配——那是定义差异，不是性能差。

## 2. runner 约定

```bash
bash bench/gc/v2/pause/<lang>.sh [outdir]      # 默认 outdir = bench/gc/v2/pause/<arm>
```

- 二进制/类文件从 `${B:-/tmp/v2matrix/build}` 取（`bash bench/gc/v2/run.sh build` 产出），**不要**另建构建体系；
  缺产物要报错退出，不要静默跳过。
- 形状与参数必须与 `bench/gc/v2/run.sh` 的 `bench_args` 一致：
  `promote 500000 20261013` · `sizeratio 2000000 20261013` · `chase 1048576 20261013 4000000` · `cycleratio 400000 20261013 100`。
- 臂的调用方式与 `run.sh` 的 `run_arm` 一致（Go 是 `GOMAXPROCS=1`，Node 默认 `JS_MODE=split`），只额外加诊断 flag。
- 全部用 `taskset -c ${PIN:-6}` 钉核。
- **runner 必须逐个串行跑**：所有臂都钉同一条核，并发跑会互相抢占（症状：同一臂两次 `wall_ms` 差 2×，
  `total_us` 也跟着跳）。取数前确认机器安静、没有别的跑分在跑。
- **stdout 必须逐字节不变**：每条形状跑两遍（带诊断 / 不带诊断），两臂 stdout 必须 `cmp` 相同，否则 runner 报错退出。
  诊断只许写 stderr 或独立文件。
- 每形状跑完打一行：`ok  <arm> <shape> events=N total_us=X wall_ms=Y`。
- `.hist` 用自己的原生分档即可（**不必**对齐公共阶梯）；聚合器会重新分档。
  零事件也必须产出 `events=0` 的 hist（全零 count）与 meta ⇒ 聚合器才能区分「没发生 GC」和「工具坏了」。
- 允许 `REPS`（默认 1）取最优一次；取哪一次要在 meta 的 `notes` 里写清。

## 3. 公共阶梯（聚合器重分档用，µs）

`[0,1) [1,2) [2,5) [5,10) [10,20) [20,50) [50,100) [100,200) [200,500) [500,1000) [1000,2000) [2000,5000) [5000,10000) [10000,inf)`

## 4. 聚合

```bash
python3 bench/gc/v2/pause_report.py bench/gc/v2/pause            # 出 PAUSE.md + pause.png + pause_v2.tsv
```

列：`shape arm events total_us share_pct p50_us p90_us p99_us p999_us max_us ge1ms`（分位优先用 `.events` 精确算，没给就按 hist 上界近似）。
