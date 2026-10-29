# bench/par —— 数据并行基准（extC `par` vs 顺序）

## 1. 形状

一个**计算受限**的逐元素 map：每个元素跑一条固定长度的递推链，然后写回输出视图。

```
n       = 元素个数（argv[1]，默认 64）
iters   = 递推链长度（argv[2]，默认 48）
in, out = 两块 n 个 f64 的视图（清零后 in[i] = f64((i * 7 + i64(3)) % 17) / 17.0）
for i in 0..n:
    x = in[i]
    y = x
    k = 0
    while k < iters:
        y = y * 1.0000001 + x * 0.5 + 1.0
        x = x * 1.0000003
        k = k + 1
    out[i] = y
cs = sum(out)                       // 行主序、i 递增
输出唯一一行：`cs=<i64(cs * 1000000.0)> n=<n>`
```

主表口径 n = 1<<18（262144）与 1<<20（1048576），递推链 48 步：n=1<<20 时单线程约 40 ms，
线程启动开销被摊薄，规模效应看得清。

## 2. 臂

| 臂 | 写法 |
|---|---|
| `seq` | extC：`for i in 0..n { … }` |
| `par1` / `par2` / `par4` / `par8` | extC：`par i in 0..n { … }`，线程数用 `EXTC_PAR_THREADS` 固定 |
| `c` | 手写 C：`for (i = 0; i < n; i++) { … }`（**不检查下标的**顺序参照；见 §4 第 5 条） |

所有臂输出必须**逐字节相同**（含 `cs`），否则 FAIL —— 等价性先于性能。
`par*` 臂之间也必须一致：线程数只影响速度，不影响结果。

## 3. 口径

- 构建：extC 前端 `./build/extc -w --no-line-map -o X.c X.extc`，然后
  `cc -O2 -std=c11 -fwrapv -pthread`；手写 C 同一组旗子（`-O2 -std=c11 -fwrapv`）。
- 计时：外部 `python3 time.perf_counter` 包整个进程，**预热一次**，`RUNS` 次取 best；
  `bash bench/par/run.sh`（默认 `RUNS=5`）。
- 钉核：`taskset -c 6-13`（`PIN` 可覆盖）。`par` 臂的 worker 继承这 8 个核；`seq`/`c` 只用其中一个。
- 报告：`raw.txt`（原始记录，别手改）→ `report.py` → `RESULTS.md`。
- 指标：`ms`、`ns/cell = best_ms * 1e6 / n`、`有效 GB/s = 2 * n * 8 / best_s / 1e9`
  （每格读 in、写 out 两个 8 字节）。这两个数字是**内存侧旁证**：本 workload 计算受限，
  所以 ns/cell 随线程数下降；跑 stencil 一类的带宽受限负载时 GB/s 会贴机器上限。

## 4. 判据

1. 所有臂 stdout 逐字节相同；
2. `par8` 相对 `par1` 有可测加速（同一台机器、同一口径下比较，不写死倍数）；
3. `par1` 不比 `seq` 慢（`par` 机制本身不给顺序版加税；表里两者应基本相等）；
4. 报告里每个格子都有 ms、ns/cell、有效 GB/s 与 RSS；
5. 手写 C 臂**不检查下标**，所以它跑得比 `seq` 快是预期的（那不是 `par` 的问题）——读加速比请
   以 `seq` 或 `par1` 为基线，`c` 只用来标出"安全检查 + 未向量化"这条已知差距的上界。

## 5. 边界

- 单机 WSL2，`taskset` 只能绑 vCPU，宿主侧调度仍可能影响绝对值；比比值，别比跨会话绝对值。
- 不测二维折叠（v1 未支持）、不测归约并行合并（v1 归约是串行 `stl::seq`）。
- `par` 每次调用起线程（无池）—— 这正是本基准要暴露的固定开销：n 小的时候它主导，
  n=1<<18 时候摊薄。
