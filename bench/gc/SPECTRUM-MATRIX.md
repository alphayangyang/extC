# 大横评：P1–P6 × {extC, Go, Java} 全默认 —— 墙钟 / 峰值 RSS / GC 停顿三把尺矩阵（`task-30`）

> **状态**：**已跑 ✓**（2026-10-15 12:32–12:40 · core 6 · 单会话）。本件补齐 `SPECTRUM.md` §4 里
> "**第 2 段（完整三把尺矩阵）未跑 ✗**" 那个缺口。
> 写作用域 `bench/gc/**` ✓；`src/**`、`tests/**`、`tools/**`、`docs/**` **未动** ✓；
> 插桩只在 **生成物副本**（`/tmp/bm`）✓。原始数据：`spectrum-matrix.tsv`、`spectrum-matrix-pause.tsv`。
>
> **标注约定**：`【实测】` = 本机跑出来的（原始行在 `/tmp/bm/`）；`【事实】` = 版本/环境命令行输出；
> `【推理】` = 由实测推出的判断，**不是**实测 ✗。三者不混。

---

## 0. 一句话结论

**六档里没有"谁全面赢"**：extC 在 P1/P3/P4 墙钟最快（P3 比 Go 快 **3.8×**、P4 快 **2.1×**），Go 在 P6 最快、
P2 三语言打平（319.5/400.4/346.9/322.8 ms，extC-O2 略最快）；**P5 上 extC 崩了（5.74× Go、19.1× Java）**，
而 P5 正是 extC 回收总工作量占墙钟 **47.8%** 的档（其余有回收的档最大 10.6%）。
**GC 停顿这把尺上，三家的"默认口径"根本不是一件事**：Go 的 ΣSTW 只有 0.02–0.2% 墙钟（但 assist+bg 最高吃掉 57% CPU），
Java 的 Σpause 在 P2/P5 占到 **42% 墙钟**、单次 max 42.5 ms，extC 的官方 `[pause]` 只报 **0.06–0.23 ms/跑**
（≈0% 墙钟）——但那只是它武装窗口里的子集，同一批运行的 `[step]` 总量是 **5.7 ms–2.92 s/跑**。
**P1/P3 上 extC 是"结构性 0 GC"（生成物里连回收运行期都没发射），不是"extC GC 很快"，不许这么读。**

---

## 1. 口径（先钉死）

### 1.1 形状与规模 N（**每档三语言同 N、同 seed**）

形状定义沿用 `SPECTRUM.md` §1：P1–P5 = `src/spectrum_pN.{extc,go}` + `src/SpectrumPN.java`；**P6 = `digraph`**
（`src/digraph.{extc,go,java}`）。seed 全部 `20261013`，argv 一律 `<N> 20261013`。

N 用 pilot 选（`/tmp/bm/logs/pilot.txt`、`verifyN*.txt`）：**目标 = 让最快那个臂 ≥ ~0.3 s**，三语言同 N。

| 档 | N | pilot 后最快臂（1 次，ms） | 选定理由 |
|---|---|---|---|
| P1 | **4 000 000** | extC-O2 326.6 | ≥0.3 s ✓ |
| P2 | **48 000 000** | Go 273.6 @40M ⇒ 拔到 48M | 40M 时最快 269 ms，略欠 ⇒ 48M（实测 320 ms）|
| P3 | **1 500 000** | extC-O1 348.2 | 1.25M 时 291 ms，略欠 ⇒ 1.5M |
| P4 | **28 000** | extC-O2 550.9 | ≥0.3 s ✓ |
| P5 | **20 000 000** | Java 319.8 | 10M 时最快只有 189 ms（Java）⇒ 16M 仍 264 ms ⇒ 20M |
| P6 | **1 000 000** | Go 512.6 | 与 `DIGRAPH.md` §6.2 的 1M 档同规模，可交叉核对 |

> ⚠️ **P5 的 N 是被"Java 最快"顶上去的**【实测】：P5 上 extC-O2 在 N=10M 就 1.76 s、20M 时 **6.10 s**，
> 为了让**最快臂**（Java）≥0.3 s 只能拔到 20M。代价是 extC 单次 6 s ⇒ 这一档是全矩阵的长杆（5 次 ≈ 31 s/臂）。
> 这是"最快臂 ≥0.3 s"这条规则在臂间差 19× 时的直接后果，**如实记录，不藏**。

### 1.2 三把尺

| 尺 | 量法 | 谁量的 |
|---|---|---|
| ① 墙钟 | **best-of-5**，`CLOCK_MONOTONIC` 只包被测程序（不含 shell/taskset 启动） | `bench/gc/digraph_run.c`（既有的自帶垫片，`--pin 6`）|
| ② 峰值 RSS | **max-of-5**，`wait4` 的 `rusage.ru_maxrss`（**内核值**，不用 `RUSAGE_CHILDREN` 差值） | 同上（垫片自身 ~1.5 MB，避免量具地板假象）|
| ③ GC 停顿 | **另一轮**，只开日志/插桩、**不改策略**；每档 5 次运行**样本池化**后给分位 | 见 §4 |

- **钉核**：外层 `taskset -c 6` + 垫片 `sched_setaffinity({6})`（子进程 `fork` 继承）【实测：Go 的 `gctrace` 末字段 = `1 P`】。
- **不带插桩跑计时** ✓：计时轮的 stderr 全喂 `DEVNULL`；停顿轮是**独立**的进程与轮次。
- **每样本都校验产物**：stdout 与参考（extC-O2 首样本）**逐字节**比较；`rc != 0` 或行数/格式不符 ⇒ 该样本作废。
  本轮 **120/120 样本全部 `OK`、每臂 `valid 5/5`、无 MISMATCH、无 DISCARD**【实测】（`spectrum-matrix.tsv` 的 `validity` 列）。
- **同 seed 同输出（三语言逐字节）**【实测】：六个档在**选定 N** 下三语言 stdout `cmp` 逐字节相同，
  每样本 sha256 前 16 位一致（例如 P6 @1M 全部样本 = `087d1828327cb37c`；P5 @20M = `eda465c7a07daf0c`）。
  没有出现"不一致 ⇒ 作废"的档 ✓。

### 1.3 交错跑（防"某语言整段撞上负载窗口"）

同一档的 4 个臂**在同一 rep 内连续跑**，且**每个 rep 把臂顺序轮转一位**（Latin-square 式旋转）：
rep0 = O2,O1,Go,Java；rep1 = O1,Go,Java,O2；… rep4 = Java,O2,O1,Go。
每样本记 `/proc/loadavg` 首字段（`spectrum-matrix.tsv` 的 `load1` 列）。

**load 区间**【实测】：矩阵开跑 `load1=0.80`，收尾 `load1=1.08`（`/tmp/bm/logs/load_wall.txt`）；停顿轮 `0.47 → 1.16`
（`load_pause.txt`）。**全程没有别的队友跑基准**（开跑前 `list_agents` 全 idle，Lead 确认把 30–40 分钟窗口给我；
`docs-keeper` 只跑轻档文档检查）。⇒ 本轮不存在今天早些时候那种"并发污染"。

### 1.4 "全部默认"到底是什么意思（**不许读成"等预算"**）

| 语言 | 默认档的确切内容 | 证据 |
|---|---|---|
| **extC** | 回收**开**（`EXTC_RECLAIM` 未设）、计数**关**（`EXTC_RC_NOCOUNT` 未设）；**无内存上限**，只有回收阈值 | 生成物默认路径【实测】 |
| **Go** | `GOGC` 未设 ⇒ 运行时 **`GOGC=100`**；`GOMEMLIMIT` 未设 ⇒ **MaxInt64（无限制）**；`taskset -c 6` 下 **`GOMAXPROCS=1` / `NumCPU=1`** | `go1.26.0`；探针 `/tmp/bm/goprobe.go` 打印 `GOGC_actual=100 GOMEMLIMIT_bytes=9223372036854775807 GOMAXPROCS=1 NumCPU=1`【事实】 |
| **Java** | `javac`/`java` 默认 ⇒ **G1**；ergonomic **`MaxHeapSize=4116709376 B = 3.83 GiB = 1/4 × MemTotal 15.34 GiB`**；未加任何 `-Xmx/-XX` | `java -XX:+PrintFlagsFinal`【事实】 |

⇒ **RSS 那一列是"各自默认策略下的保留量"，不是等内存预算对照**：Go 可以一路涨到 `GOMEMLIMIT` 才被逼收，
Java 有 3.83 GiB 的 ergonomic 上限，extC 没有硬上限。**任何"谁 RSS 小是因为更省"的结论都要先扣掉这一条** ✗。

### 1.5 extC 两档编译口径

- 主档 **`-O2 -fwrapv`**（reclaim 形状的闸门口径）；附档 **`-O1 -fwrapv`**（`bench/run.sh` 口径）。
- **实测**：`extc -O2/-O1/-o`（`-o` 模式只生成 C）**不改变生成 C** —— 三份产物 `md5` 全同
  `5f3f1ba3ff6e531ef2732c1144173d35` ⇒ 两档就是**同一份生成 C 用 `cc -O2` vs `cc -O1` 编**（外加 `-fwrapv`）。
- extC 编译器身份：`build/extc` **md5 `c1c3a4399b0fa24506fd1b218fc45eb7`**（mtime 2026-10-15 11:48:30）。
- 构建命令原文：
  ```
  build/extc -w --no-line-map -I stdlib src/spectrum_pN.extc -o /tmp/bm/out/pN.c
  cc -O2 -fwrapv -o /tmp/bm/out/pN_o2 /tmp/bm/out/pN.c
  cc -O1 -fwrapv -o /tmp/bm/out/pN_o1 /tmp/bm/out/pN.c
  go build -o /tmp/bm/out/pN_go src/spectrum_pN.go
  javac -d /tmp/bm/classes src/SpectrumPN.java
  ```

### 1.6 预热假设（JVM 不许被 best-of-5 蒙过去）

每次采样都是**新进程**（跑手 `fork`+`exec`，没有进程内预热）。所以：
- JVM 每一格的数字**都含 JVM 启动 + 解释执行/JIT 预热**；best-of-5 只能缓解（去掉最坏的一次），**不能消除**。
- 小 N 下 JVM 会明显吃亏；本轮的 N 选择已经把"最快臂"顶到 ~0.3 s 以上，但 **Java 在 P2/P3 的启动+预热仍是可见项**：
  P3 的 Java（504.9 ms）比 extC-O1（348.0 ms）慢 45%，而它的 GC 只占 0.9% 墙钟 ⇒ 差额主要在启动/编译【推理，未分离归因】。
- **Go/extC 是 AOT 静态**，启动远小于 JVM【沿用 `DIGRAPH.md` §6.1 的量具校准：`/bin/true` 级 RSS 1476 KB；
  **本轮没有单独测 Go 启动** ⇒ 这条是引用，不是本轮实测 ✗】。

---

## 2. 第 1 把尺：墙钟（best-of-5, ms）

| 档 | N | extC `-O2` | extC `-O1` | Go 默认 | Java 默认 | 相对 Go（<1 更快）|
|---|---|---|---|---|---|---|
| **P1** 纯短命分配、无引用 | 4 000 000 | **326.9** (1.3%) | 531.9 (1.0%) | 530.8 (0.9%) | 465.9 (5.1%) | extC-O2 **0.62×** · O1 1.00× · Java 0.88× |
| **P2** 分块追加、活集单调长 | 48 000 000 | **319.5** (64%)⚠ | 400.4 (2.4%) | 346.9 (9.0%) | 322.8 (8.2%) | 0.92× · 1.15× · 0.93× |
| **P3** 每轮建链、轮末整片丢 | 1 500 000 | 403.9 (2.2%) | **348.0** (0.6%) | 1318.7 (0.7%) | 504.9 (10.4%) | 0.31× · **0.26×** · 0.38× |
| **P4** 长命符号表+短命 AST | 28 000 | **542.9** (7.0%) | 794.6 (4.6%) | 1141.6 (3.9%) | 770.0 (5.9%) | 0.48× · 0.70× · 0.67× |
| **P5** 滑窗+长命链式索引 | 20 000 000 | 6101.5 (1.2%) | 6137.4 (2.4%) | 1063.7 (4.9%) | **319.1** (5.4%) | **5.74×** · 5.77× · **0.30×** |
| **P6** `digraph` | 1 000 000 | 598.4 (3.4%) | 637.7 (3.3%) | **516.0** (2.6%) | 715.9 (2.5%) | 1.16× · 1.24× · 1.39× |

（括号 = 5 次样本的 spread `(max−min)/min`。⚠ P2 的 64% **全部来自 rep0 的首样本**：
`523.9 ms`，它是**整个矩阵的第一个样本**（首次触页/冷缓存），其余 4 次是 319–331 ms（±2%）。
best-of-5 不受影响；median-of-5 = 322.6 ms。）

### 判词（第 1 把尺）

- **extC-O2 赢 3 档**（P1 0.62×、P3 0.31×、P4 0.48×），**Go 赢 1 档**（P6：extC-O2 1.16×、Java 1.39×），
  **P2 三语言打平**（319.5/400.4/346.9/322.8 ms，四臂全在 1.25× 内），**Java 赢 1 档且赢得极大**（P5 0.30× Go）。
- **O1/O2 的方向是不一致的**【实测】：P4/P2 上 `-O2` 更快（P4 542.9 vs 794.6 = −32%），
  P3 上 `-O1` 更快（348.0 vs 403.9 = −14%），P1 上 `-O2` 快 39%，P5/P6 上差 ~1–6%。
  ⇒ **"同 Ir 两档差 ~30%"这句话只在部分形状成立，且方向会翻** ⇒ 报告里每个 extC 数字都必须带档位，不许合并成一个"extC"列。
- **P3 的 3.8× 不是 GC 差**：Go 在这档真在堆上分配（`&node{...} escapes to heap`【事实：`go build -gcflags=-m`】），
  而 extC 的生成物里**一个 `malloc`/`extc_rc_alloc` 都没有**、全靠 `extc_arena_release` 每轮整片归还【实测】⇒ 差在**分配器**。

---

## 3. 第 2 把尺：峰值 RSS（max-of-5, MiB）

| 档 | extC `-O2` | extC `-O1` | Go | Java | Java / extC-O2 |
|---|---|---|---|---|---|
| P1 | **1.5** | 1.5 | 1.9 | 107.8 | **72.8×** |
| P2 | **375.2** | 375.2 | 426.6 | 443.2 | 1.18× |
| P3 | **1.5** | 1.5 | 8.2 | 108.1 | **73.0×** |
| P4 | **7.3** | 7.3 | 14.4 | 115.4 | 15.8× |
| P5 | 179.0 | 179.0 | **143.3** | 189.0 | 1.06× |
| P6 | 9.9 | **9.5** | 9.5 | 119.1 | 12.0× |

### 判词（第 2 把尺）

- **extC 赢 5/6 档**；P5 上 Go 最小（143.3 vs extC 179.0 vs Java 189.0），但三家同量级（1.3× 以内）。
- **P2/P5 是"活集本来就大"的两档**：P2 的活集就是 ~375 MiB 的链表（48M/256 块 × 2 KiB），谁的 RSS 都下不来
  ⇒ **这档的 RSS 比的是"活集"，不是"回收能力"**。数字自洽：extC 375.2 / Go 426.6 / Java 443.2 = 1 : 1.14 : 1.18。
- **P1/P3 的 72.8×/73.0× 是结构性差距**：这两档 extC 根本不进堆（P1/P3 生成物 `extc_rc_alloc` 0 处、`malloc` 0 处、
  `extc_rc_collect` 0 处），Go 在 P1 靠逃逸分析把 `&buf{}` 放栈上（所以 Go 也只有 1.9 MiB），
  **Java 的 `long[]`/对象逃逸分析消不掉 ⇒ 真分配**（P1 31 次 Young GC、P3 32 次）⇒ Java 的 108 MiB 里绝大部分是**堆本身**。
  ⇒ **这条差距是"谁能把分配消掉"，不是"谁 GC 快"** ✓。

---

## 4. 第 3 把尺：GC 停顿（用户最关心的那把）

### 4.1 口径：**三家的"停顿"不是同一种东西，extC 还有两个总体**

| 口径 | 定义 | 覆盖了什么 / 漏了什么 |
|---|---|---|
| **extC `pop=pause`** | 生成物 `[pause]` 行的总体：`extc_rc_maybeCollect` / `extc_rc_checkOwed` 里**武装窗口**（`extc_rc_pauseArm`）内的事件 | = 分配路径上的安全点步；**不含 `extc_rc_markAdvance` 的成批推进、不含 `settle`** ⇒ **是下界，不是全部停顿账** ✗ |
| **extC `pop=step`** | 生成物 `[step]` 行的总体：**每一次** `extc_rc_stepNote`（mark/sweep 子步，含 `settle`/`advance` 路径触发的） | = extC 在 **mutator 线程上同步花的回收时间**（extC 没有 GC 线程，这些时间应用就是干不了活）|
| **Go STW** | `GODEBUG=gctrace=1` 每行 `clock` 的**第 1 段 + 第 3 段**（`a+c`）| 标记/清扫的**并发**部分不计入；同行的 **assist+bg+idle（cpu 字段）单列**，否则会把"把活搬出暂停"读成"不花代价" ✗ |
| **Java Pause** | `-Xlog:gc:file=…` 里 `Pause …` 行的 `<x>ms`（默认 G1） | 真正的 STW；**并发标记周期（不带 Pause 的行）不计入** ⇒ 这是 Java 的**停顿**账，不是 GC 总 CPU 账 |

**extC 官方口径（`EXTC_RC_PAUSE=1`，生成物自带、只加 2 次 clock 读）与逐次分布（生成物**副本**插桩，只测量）的交叉核对**【实测】：
两者 n 与 max 完全一致（例：P6 `[pause] collects=88 max_us=1.8` ↔ 插桩 `n=88`；`[step] n=32011` ↔ 插桩 `n=32011`）⇒ 探针可信 ✓。

### 4.2 停顿矩阵（**per-run = 5 次运行池化后 ÷5；分位/ max = 5 次样本池化**）

| 档 | 语言·总体 | 次数/跑 | Σ/跑 | 单次 max | p50 | p99 | Σ/跑 ÷ 墙钟 |
|---|---|---|---|---|---|---|---|
| **P1** | extC `[pause]` / `[step]` | **结构性 0** | 0 | — | — | — | **0%** |
| | Go STW | **0（显式 0）** | 0 | — | — | — | 0% |
| | Java G1 Pause | 31.0 | **4.248 ms** | 387 µs | 162 µs | 320 µs | 0.9% |
| **P2** | extC `pause` / `step` | 5 / **1216** | 0.010 ms / **5.735 ms** | 3.9 µs / 122.5 µs | 2.36 µs / 6.22 µs | 3.2 µs / 12.4 µs | 0.003% / **1.8%** |
| | Go STW（+GC CPU）| 6.0 | 0.068 ms（+**33.8 ms CPU**）| 19 µs | 10 µs | 18 µs | 0.02%（CPU 9%，中位 9% / max 12%）|
| | Java G1 Pause | 5.0 | **135.637 ms** | **42 492 µs** | 25 822 µs | 37 100 µs | **42.0%** |
| **P3** | extC `[pause]` / `[step]` | **结构性 0** | 0 | — | — | — | **0%** |
| | Go STW（+GC CPU）| 489.2 | 2.627 ms（+28.2 ms CPU）| 38 µs | 5 µs | 14 µs | 0.20%（CPU 2%）|
| | Java G1 Pause | 32.0 | 4.414 ms | 364 µs | 107 µs | 314 µs | 0.9% |
| **P4** | extC `pause` / `step` | 655 / **85 963** | 0.231 ms / **57.812 ms** | 167.2 µs / 198.2 µs | 0.22 / 0.70 µs | 1.4 / 1.8 µs | 0.04% / **10.6%** |
| | Go STW（+GC CPU）| 248.2 | 1.443 ms（+**369.3 ms CPU**）| 23 µs | 5 µs | 14 µs | 0.13%（**CPU 32%，中位 32% / max 33%**）|
| | Java G1 Pause | 12.0 | 16.240 ms | 1 716 µs | 1 370 µs | 1 708 µs | 2.1% |
| **P5** | extC `pause` / `step` | 41 / **163 297** | 0.058 ms / **2919.061 ms** | 20.9 µs / **4 865.8 µs** | 1.20 / 1.58 µs | 3.9 / 62.1 µs | 0.001% / **47.8%** |
| | Go STW（+GC CPU）| 30.6 | 0.255 ms（+**610.6 ms CPU**）| 21 µs | 9 µs | 14 µs | 0.02%（**CPU 57%，中位 43% / max 61%**）|
| | Java G1 Pause | 10.0 | **134.756 ms** | 17 028 µs | 13 434 µs | 16 133 µs | **42.2%** |
| **P6** | extC `pause` / `step` | 88 / **32 011** | 0.061 ms / **37.736 ms** | 3.9 µs / 335.3 µs | 0.47 / 1.21 µs | 2.5 / 3.7 µs | 0.01% / **6.3%** |
| | Go STW（+GC CPU）| 54.0 | 0.432 ms（+41.6 ms CPU）| 21 µs | 8 µs | 16 µs | 0.08%（CPU 8%）|
| | Java G1 Pause | 2.0 | 2.899 ms | 1 868 µs | 1 463 µs | 1 713 µs | 0.4% |

> **"可得的 p99"**：三语言**都拿到了**（extC = 生成物副本逐次插桩的池化 p99；Go/Java = 日志逐次停顿的池化 p99）。
> **没有"未获得"的格子** ✓。但注意 Go 的 p99 只是 **STW** 的 p99；extC 的 p99 只覆盖 **`[pause]`/`[step]` 各自的总体**。

### 4.3 判词（第 3 把尺）

1. **单次停顿：Go 压倒性**（5–38 µs，p99 ≤18 µs），Java 最差且**在 P2/P5 差得离谱**（单次 max 42.5 ms / 17.0 ms，
   p50 25.8 ms / 13.4 ms），extC 的**武装窗口最小**（`pause` p99 1.4–3.9 µs；把 `step` 也算进来 p99 ≤62.1 µs，
   仍比 Java 的 ms 级小两个数量级）但**总次数/总量不是**（见第 3 条）。
2. **Σ 停顿：Go 依旧压倒性**（0.068–2.63 ms/跑），Java 在 P2/P5 到 **135 ms/跑**（占墙钟 **42%**），
   extC `[pause]` 只有 0.01–0.23 ms/跑（占墙钟 ≤0.04%）——**但这行数字绝对不能用来说"extC 的 GC 停顿可忽略"** ✗，
   因为 extC 的回收不在那个窗口里（下一条）。
3. ⚠️ **本轮最要紧的口径发现**：extC 的 `[pause]` **严重低估**真实回收时间。
   同一批运行里 `[step]` 总量是 `[pause]` 的 **250–50 000×**：P5 `pause` Σ 0.058 ms vs **`step` Σ 2 919 ms（墙钟的 47.8%）**；
   P4 0.231 ms vs **57.8 ms（10.6%）**；P6 0.061 ms vs **37.7 ms（6.3%）**；P2 0.010 ms vs **5.7 ms（1.8%）**。
   机制：`extc_rc_maybeCollect` 里**成批推进（`markAdvance`）发生在 `pauseArm` 之前**、`settle` 路径也不武装
   ⇒ 武装窗口只圈住了极少数"一步做完"的事件。**只看 `EXTC_RC_PAUSE=1` 会得出"extC 几乎不停顿"的错误结论。**
   （这正是任务提醒里"`pauseArm` 不包 `settle`"的实测放大版；**只报告，不修**。）
4. ⚠️ **旁证：仓库自带的 `tools/rc-pause-probe.py` 在当前代码形态下会给出"0 次停顿"的假象**【实测，4 档各 1 次】：
   它把 `PAUSE_DIST` 的环形记录插在 `extc_rc_collect`（**冷契约入口**）里，而热路径走 `extc_rc_step`
   ⇒ 在**选定 N** 下四档全部 `PAUSE_DIST n=0`，而**同一个进程**里 `[pause] collects=5/655/41/88`、
   `[step] n=1216/85963/163297/32011`（P2/P4/P5/P6）⇒ 说"0 次停顿"是量具口径错，不是事实。
   它的 `step_n/step_max_us` 字段仍有效（`step_max_us=1.7` 那类数字可用），**但 `PAUSE_DIST` 那行不能用来断言停顿为 0** ✗。
   （**只报告，不修 `tools/**`**。）
5. **Java 的"停顿"在 P2/P5 是真的贵**：135 ms/跑、占墙钟 42%，而且它**没有并发逃逸**——
   G1 的 Young evac 是 STW。这解释了为什么 Java 在 P5 墙钟最快（对象短命、10 次 Young 就够）却在停顿尺上最差。
6. **Go 的"停顿小"要配着 assist+bg 读**：P4 的 ΣSTW 只有 1.44 ms，但 assist+bg+idle = **369.3 ms CPU（32.4% 墙钟）**；
   P5 是 0.25 ms vs **610.6 ms（57.4%）** ⇒ **Go 的停顿优势是"把活搬出暂停"买来的**，不是活更少。
   `taskset -c 6` 下 Go 只有 1 P，这些活与 mutator 抢同一个核（所以它仍体现在墙钟上）。
   （与 `GO-PAUSE.md` §2 的机制结论一致 ✓。）

---

## 5. 每档一句话判词（谁赢哪把尺 · 差多少 · 形状解释了哪部分）

- **P1（纯短命分配、无引用）**：墙钟 extC-O2 赢（0.62× Go）；RSS extC 赢 72.8×（1.5 vs 107.8 MiB）；停顿
  **extC 结构性 0**（不进堆：生成物 0 个 `malloc`/`extc_rc_*`）· **Go 0 个 GC 周期**（`&buf{} does not escape` ⇒ 上栈），
  Java 31 次 Young Σ4.25 ms（0.9% 墙钟）。
  **形状解释了全部**：无引用 ⇒ 三家都不需要追踪；Java 的 `long[]` 消不掉 ⇒ 它是唯一真在分配的。
- **P2（分块追加、活集单调长）**：**三把尺全打平**（墙钟 319.5/400.4/346.9/322.8；RSS 375–443 MiB 同量级）——
  这是六档里唯一"活集就是全部成本"的形状。停顿账却完全不同：Java Σ135.6 ms = **42% 墙钟**、
  Go ΣSTW 0.068 ms（+9% CPU）、extC `[step]` 5.7 ms（1.8%）。
- **P3（每轮建链、轮末整片丢）**：extC-O1 赢墙钟（0.26× Go，**3.8× 快**）、RSS 赢 73.0×；
  三家的 Σ停顿都 ≤0.2% 墙钟 ⇒ **这档的墙钟差是"分配器 vs 堆分配"，不是 GC**：
  Go 真在堆上分配 94.5M 个 node（`escapes to heap`），extC 每轮 `extc_arena_release` 整片归还（生成物 0 个 `malloc`）。
- **P4（长命只读符号表 + 每单元短命 AST）**：**三把尺三个赢家** —— 墙钟 extC-O2（0.48× Go）、
  RSS extC-O2（7.3 vs Go 14.4 / Java 115.4 MiB）、**单次停顿没有单一赢家**：
  extC 的 p50/p99 最小（`pause` 0.22/1.4 µs、`step` 0.70/1.8 µs）但 `step` max 被一个 **167 µs** 的离群步
  拖到 Go（23 µs）的 7×；Java 最差（p99 1708 µs，比 extC `step` p99 大 **950×**）。
  但 Go 在这档**最慢**且 GC CPU **32%**（assist+bg 369 ms），extC 的 `[step]` 占墙钟 10.6% ⇒
  **"写足迹小、活集稳"的符号表形状对 extC 有利，对 Go 的分配路径不利**。
- **P5（滑窗 + 长命链式索引、写密集/偶发插入）**：**Java 全面赢**（墙钟 0.30× Go、19.1× extC-O2；
  RSS 与 Go/extC 同量级）；**extC 崩了**：墙钟 5.74× Go，且 `[step]` Σ **2 919 ms = 47.8% 墙钟**
  ⇒ 这档 extC 的回收**就是**墙钟大头（**报告不修**：`src/**` 未动）。
  Java 赢的原因也清楚：10 次 Young、对象短命；但它为此付出 42.2% 墙钟的 STW。
- **P6（`digraph`，单巨分量/换血 96.7%）**：Go 赢墙钟（extC-O2 1.16×、Java 1.39×）；
  RSS：Go 9.5 / extC 9.9 MiB 同级，Java 119.1 MiB（12.0×）；停顿 Go STW 54 次 Σ0.43 ms max 21 µs，
  extC `[step]` 32 011 次 Σ37.7 ms（6.3%），Java 只触发 2 次（Σ2.9 ms）但单次 1.9 ms。

---

## 6. 与既有记录的交叉核对（**跨会话 ⇒ 只作同量级核对** ✗）

| 量 | 本次（2026-10-15） | 既有记录 | 差 |
|---|---|---|---|
| P6 extC-O2 墙钟 @1M | 598.4 ms | `DIGRAPH.md` §6.2：538.8 ms（2026-10-13）| +11% |
| P6 Go 墙钟 @1M | 516.0 ms | §6.2：514.5 ms | +0.3% |
| P6 extC RSS @1M | 9.9 MiB | §6.2：11.6 MiB | −15% |
| P6 Go RSS @1M | 9.5 MiB | §6.2：8.6 MiB | +10% |
| P6 Go ΣSTW @1M | 0.432 ms / 54 周期 | `GO-PAUSE.md` g0：0.346 ms / 53 周期 | +25% / 同量级 |
| extC `[step]` 口径存在 | 本轮实测有 `[step]` 行 | `SPECTRUM.md` §2/§5 未提 `[step]` | 本轮补齐 |

⇒ P6 与历史**同量级**（±11%），可互相印证；但**编译器 md5/机器状态可能不同**（§6.2 记录的是 `69a5b119…`，
本轮是 `c1c3a439…`）⇒ **不许把两张表混算** ✗。

---

## 7. 缺失 / 失败 / 不可比（**如实清单**）

1. **没有作废的臂** ✓：120/120 墙钟样本 `OK`、6/6 档三语言逐字节一致、无 `rc != 0`、无 `DISCARD`。
2. **P1/P3 的 extC 停顿不是"0"，是"结构性 0"**：生成物里 `extc_rc_collect` **0 处**、`extc_rc_alloc` **0 处**、
   `malloc` **0 处**（`/tmp/bm/out/p1.c`、`p3.c`）⇒ 它压根没发射回收运行期，**不是"extC GC 很快"**。
3. **extC 的 `EXTC_RC_PAUSE=1` 不是完整停顿账**（§4.3-3）⇒ 主表同时给两个总体；**任何单看 `[pause]` 的结论都作废** ✗。
4. **`tools/rc-pause-probe.py` 的 `PAUSE_DIST` 在当前形态下恒为 0**（§4.3-4）⇒ 本轮改用**自己的生成物副本插桩**
   （`/tmp/bm/patch_dist.py`，只测量、不改策略），并在 TSV 里标 caliber。**这是本轮唯一"另起量具"的地方**。
5. **单次运行的数字**：本轮所有停顿数字都是 **5 次运行池化**；唯一"单次"的是 §4.3-4 的 `PAUSE_DIST n=0` 旁证（4 档各 1 次，只用于证明"那行恒 0"）。
6. **不可比项**（不许把数字直接相除的就这些）：
   - RSS 是**各自默认策略**下的保留量（Go 无 `GOMEMLIMIT`、Java 3.83 GiB ergonomic 上限、extC 无上限）；
   - extC 的 `[step]` 是 **mutator 线程上的同步 CPU/墙钟**，Go 的 assist+bg 是 **CPU 时间（可并发）**，
     Java 的 Pause 是 **STW 墙钟** —— 三者语义不同，**只并排，不相减** ✗；
   - JVM 每格含启动 + JIT 预热；extC/Go 不含；
   - P2 的 rep0 首样本是全矩阵第一个样本（冷缓存），spread 列 64% 的成因；
   - `taskset -c 6` 是**单核口径**：Go 只有 1 P、Java 的 GC worker 与 mutator 争同一核 ⇒ 不是"默认机器"。

---

## 8. 未覆盖与残留风险

**未覆盖**（明确没测的）：
- 只测 **6 个形状各 1 个 N**（表见 §1.1）；**不外推**到别的规模/画像。
- **只有默认档**（任务要求）：没测 `GOGC`/`GOMEMLIMIT`、没测 Java `-Xmx`/SerialGC/ZGC、没测 extC `SHARDS`/`RECLAIM=0`。
- **未分离归因**：Java 的 JIT 编译时间 vs GC 时间；Go 的启动；extC 的 `markAdvance`/`settle` 各占 `[step]` 多少。
- **未测 tail latency**（只测吞吐与停顿分布）；**未做第二轮重复**（单会话；每档 5 次 spread 已记录）。
- **未量** `P5` 上 extC 为何超线性（1.2M→47.6 ms、10M→1.76 s、20M→6.10 s，≈O(n^1.7)）——**只报告现象**。

**残留风险**（即使上面都对，仍可能翻的假设）：
- **单核 pin 是最大的一条**：换成多核，Go 的并发标记/JVM 的 GC worker 会更快，extC 不变 ⇒
  P4/P5 上的差距会缩小；`GO-PAUSE.md` §2 已实测 Go **4 P 时 ΣSTW 反而涨**（0.346→2.304 ms）
  ⇒ **多核下的停顿排序可能反转** ✗。
- 量具地板：垫片自身 ~1.5 MB；**任何 <2 MiB 的 RSS 格子（P1/P3）都有 ~1.5 MB 的地板成分**。
- 首样本冷缓存效应（P2 rep0）：best-of-5 已规避，但 **spread 列不能当稳定性指标**读。
- `[step]`/`[pause]` 的插桩副本额外做了一次可增长数组 append（**没有**额外 clock 读）；
  P4 的分布插桩轮 ms = 567–586，无插桩轮 = 542.9–580.9（**高 2–4%，与 spread 同量级，未完全排除**）⇒
  插桩副本的数字**只用于停顿分布，一律不进墙钟表** ✓。
- **不承诺"没有任何新风险"** ✗。

---

## 9. 复现（命令原文）

```bash
# 0) 选 N 的 pilot（每形状若干 N × 4 臂 × 1 次）
taskset -c 6 python3 /tmp/bm/pilot.py P1 P2 P3 P4 P5 P6
taskset -c 6 python3 /tmp/bm/verifyN.py

# 1) 构建（§1.5 原文；产物在 /tmp/bm/out、类在 /tmp/bm/classes）

# 2) 墙钟 + RSS：4 臂同档交错、best-of-5、无插桩
taskset -c 6 python3 /tmp/bm/drive.py wall > /tmp/bm/raw_wall.tsv

# 3) GC 停顿：extC EXTC_RC_PAUSE=1 / Go GODEBUG=gctrace=1 / Java -Xlog:gc:file=
taskset -c 6 python3 /tmp/bm/drive.py pause > /tmp/bm/raw_pause.tsv
python3 /tmp/bm/patch_dist.py /tmp/bm/out/pN.c /tmp/bm/out/pN_dist.c   # N∈{2,4,5,6} 生成物副本插桩（只测量）
cc -O2 -fwrapv -D_POSIX_C_SOURCE=200809L -o /tmp/bm/out/pN_dist /tmp/bm/out/pN_dist.c
taskset -c 6 python3 /tmp/bm/redist.py > /tmp/bm/raw_dist.tsv
# 旁证：自带探针的 PAUSE_DIST（4 档各 1 次，预期 n=0）
for pp in "P2 48000000" "P4 28000" "P5 20000000" "P6 1000000"; do set -- $pp; \
  EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1 /tmp/bm/${1}_probe $2 20261013 >/dev/null 2>/tmp/bm/out/oldprobe_${1}.err; done

# 4) 整理
python3 /tmp/bm/finalize.py    # => spectrum-matrix.tsv / spectrum-matrix-pause.tsv
```

**原始文件**（本机 `/tmp/bm/`）：`raw_wall.tsv`（120 样本）、`raw_pause.tsv`、`raw_dist.tsv`、
`pause/*.err`（extC `[pause]`/`[step]`、Go gctrace）、`pause/java_*.gc`（G1 日志）、`logs/*`（pilot/verifyN/load/构建日志）。
