# SPEC —— 2D 5 点 Jacobi 模板（extC / C / Fortran 三臂）

本目录换一个**新 workload**，横评三臂：extC（前端吐 C，再由 cc 编译）、C（同一份旗子）、
gfortran（Fortran 前端）。目的不是证明谁赢，而是把「同一份数值 kernel 交给三个前端」的
口径、对拍与数字钉在仓库里；换编译器方案比「印象慢」更可靠。

本目录只测**一个形状、三个规模、两个优化档**，属于轻量横评；六臂全矩阵不在此处。

## 1. 形状

2D 5 点 Jacobi 模板迭代，`f64`，二维网格摊成一维行主序数组
（`idx = i * n + j`，`i` 是行、`j` 是列）。

```text
输入：n（网格边长，n >= 3），iters（迭代轮数）
分配：a、b 两块 n*n 的 f64，全部清零
初始化：对 i, j ∈ [0, n-1]:
            a[i*n + j] = f64((i*7 + j*13) % 17) / 17
迭代 iters 轮，每轮：
    对 r, c ∈ [1, n-2]:
        b[r*n + c] = (a[(r-1)*n + c] + a[(r+1)*n + c]
                    + a[r*n + c - 1] + a[r*n + c + 1]) * 0.25
    交换 a 与 b 的角色
输出（恰好一行）：cs=<整数> n=<n>
    cs = int64( (按 i 外、j 内行主序累加最终 a 的 2D 和) * 1000000.0 )
```

边界（`i` 或 `j` 为 `0` 或 `n-1` 的格子）**不参与更新**：每轮只写内圈，边界格的内容来自
该 buffer 被分配/写入的那一世。因为 a、b 在交换，内圈读到的边界值在两块 buffer 的
内容之间交替 —— 这是伪代码的确定行为，三臂必须逐位一致，不是"零边界"或"复制边界"。

命令行：`<程序> [n] [iters]`，缺省 `n=64`、`iters=10`。规格默认计时参数由 `run.sh`
给出（`n = 384 768 1536`，`iters = 200`）。

## 2. 三臂

| 臂 | 源 | 编译 |
| --- | --- | --- |
| extC | `stencil.extc` | `./build/extc -w --no-line-map -o X.extc.c` 后 `cc -O2 -std=c11 -fwrapv` |
| C | `stencil.c` | `cc -O2 -std=c11 -fwrapv`（与 extC 后端同一份旗子） |
| Fortran | `stencil.f90` | `gfortran -O2` |

旁证档：三臂都用 `-O3 -march=native`（extC 仍保留 `-fwrapv`）各跑一遍，回答"优化档拉满
后结论是否变"。

**extC 臂的写法**：按这门语言的惯用写法来写（range-for 遍历、并行赋值 `a, b = b, a`
换 buffer、`stl::string::toI64` + `??` 解析参数、f-string 输出），而不是把 C 逐行翻译。
横评要比的是"同一份 kernel，用各语言的日常写法"，所以这臂不刻意回避糖。`for` 拖糖会
在每个循环体套一层作用域，但代码生成只在块体真碰本层 arena 时才发释放，这条紧循环的
循环体里没有分配，所以糖不额外带每轮的 `extc_arena_release`（`extc_arena_release` 在
整个生成的 C 里只剩函数体那一层）；写法之间的差异因此不是本 bench 要单独拆的变量
（SPEC §4）。区间循环的循环变量按上界放宽（`for r in 1..(n - 1)` 的 `hi` 在 `n: i64`
时也是 `i64`，于是 `r`、`c` 都是 `i64`），体内下标算术不再逐元素 `(int64_t)` 提升 ——
这也是 -O3 档内层 Jacobi 循环能被 GCC 向量化的原因（自报 3 处，与手写 C 相同）。

## 3. 判据

- **等价性先于性能**：三臂在 (64,10)、(1536,2) 上 stdout **逐字节相同**，且实测各轮
  cs 不随重复运行变化；不一致直接 FAIL，不读秒表。
- 时间：外部 `time.perf_counter` 包**整个进程**，冷启动预热 1 次，`RUNS=5` 次取 best。
- 钉核：`taskset -c 6`（`PIN` 可覆盖）。
- 内存：`/usr/bin/time -v` 的 `Maximum resident set size (kbytes)`，每格一次。
- 比值列：慢 > 1，`extC÷C` 是语言/检查开销，`extC÷Fortran` 是本次 headline，
  `Fortran÷C` 是前端差异。另给 `ns/cell = best_ms*1e6/(n*n*iters)`。
- 数字由 `report.py` 从 `raw.txt` 生成进 `RESULTS.md`，**别手改**。

## 4. 没做的（有意留白）

- 不对 `@unchecked` / 关闭边界检查的 extC 额外臂做对照（那要把 SPEC 再钉一层语义，
  且是"语言检查开销"的另一个问题）；想看时可以单独起一个 bench。
- 不测多线程、不测 BLAS/gem 类库，不做 cache-blocked 变体；本 workload 就是朴素
  双 buffer 内存带宽型 kernel。
- 规模只到 1536²（两块 buffer 约 37.7 MB），不追求跨 NUMA / 跨 socket 结论。
