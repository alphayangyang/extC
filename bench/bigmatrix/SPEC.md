# bench/bigmatrix —— 六语言重负载横评（规格先定死，避免"各自写各的"）

**目的**：只看两件事 —— **运行时间** 与 **峰值 RSS**。每个程序打印的输出**只用来跨语言对拍**
（六语言必须逐字节一致，不一致的数字不算数 ✗）。

**六个语言**：extC · C · C++（**原生写法**，不是"C 加个 class"）· Go · Rust · Java

## 统一口径（六家一视同仁）

| 项 | 规矩 |
|---|---|
| 数据 | **全部从 stdin 读**，由 `gen.c` 用同一个 pcg32（种子写死）生成到 `in/*.txt` ⇒ 程序里**不许**自己生成随机数 ✗ |
| 读入写法 | 每家用**自己的最快合理读法**（自己写缓冲解析）。Java **不许用 `Scanner`**；C++ 用 `std::cin` + `sync_with_stdio(false)` 或手写 |
| 优化 | extC：生成的 C 用 `-O3 -march=native -fwrapv`；C：同上；C++：`-O3 -march=native -std=c++20`；Rust：`-C opt-level=3 -C target-cpu=native -C codegen-units=1`；Go：`go build`（Go 编译器默认就优化，不吃 `-march` ✓）；Java：`javac` + **默认 JIT**（RSS 里含 JVM 自身，表里注明 ✓） |
| 时间 | **best of N**（默认 5）—— 与 `bench/oi`、`bench/gc` 同口径 ✓ |
| RSS | `/usr/bin/time` 的 Maximum resident set size（Java 用 `-Xmx4g` 给足堆，别让 OOM 冒充快 ✓） |
| 对拍 | 六语言输出**逐字节一致**才算过；`run.sh` 会卡这一条 ✓ |

## 五个形状（都来自现有 bench，只是**数据改成 stdin**）

| # | 名字 | 出处 | 形状 | 输入（stdin 文本）| 输出 |
|---|---|---|---|---|---|
| 1 | `radix` | `bench/heavy/rs` | LSD 基数排序，**4 趟 × 8 bit**，n = 10⁷ u32 | 第 1 行 `n`，然后 n 个 u32 | `sorted=<0\|1> first=<a0> last=<an-1>` |
| 2 | `cdq` | `bench/oi/p3810` | 三维偏序（CDQ 分治 + 权值树状数组），n = 2·10⁶ | 第 1 行 `n`，然后 n 行 `a b c`（1..10⁶）| `sum=<Σ cnt_i·(ans_i−1) 按 u64 回绕> max=<max(ans_i−1)>` |
| 3 | `bt` | `bench/heavy/bt` | 完美二叉树：depth 4..16 各建一棵数节点；再建 depth 18 的一棵，数 500 遍 | 第 1 行 `maxdepth bigdepth iters` | `trees=<total>` |
| 4 | `mandel` | `bench/heavy/mb` | Mandelbrot（w×h，maxiter=200，double）| 第 1 行 `w h maxiter` | `mandel=<Σ iter> img0=<img[0]>` |
| 5 | `rebuild` | `bench/gc/rebuild` | 2 万轮 × 每轮 1000 节点链：**建 → 求和 → 全部丢**（回收压力）| 第 1 行 `rounds k` | `acc=<acc>` |

### `cdq` 的 `ans_i` 定义（写死，防止六家各理解一套）

1. 所有点按 `(a,b,c)` 字典序排序；
2. **相邻去重**并记重复个数 `cnt_i`；
3. 在去重后的序列上做 CDQ：算 `f_i = Σ cnt_j`，`j` 是所有满足 `a_j≤a_i 且 b_j≤b_i 且 c_j≤c_i` 的**去重点**（去重后 `a` 天然有序）；
4. `ans_i = f_i − 1`（减掉自己）；
5. 输出 `sum = Σ cnt_i · ans_i`（u64 回绕）与 `max = max_i ans_i`。

小规模用**暴力 O(n²)** 对拍（`bench/bigmatrix/check_brute.c`）✓

## 两条必须避开的坑（都在现有 bench 里实测过）

1. **`-O2/-O3` 会把 `malloc`+`free` 整对消掉**：`objdump` 里一个 `malloc` 都不剩 ⇒ 量出假数字 ✗
   ⇒ `rebuild` 的分配/释放走 **`volatile` 函数指针**，两边都是真调用 ✓
2. **循环里的临时变量声明到循环外 ⇒ 它真的会逃逸**（extC 13MB → 495MB ✗）：
   extC 版必须把每轮的节点留在**那一轮的块**里，才测得到"出块即还" ✓

## 目录

```
bench/bigmatrix/
  SPEC.md          本文件
  gen.c            输入生成器（pcg32，种子写死；文本输出）
  in/              生成出来的输入（.gitignore，太大不入库）
  src/<shape>.c    C 参考实现（其余语言的语义基准）
  src/<shape>.{extc,cpp,go,rs,java}
  check_brute.c    cdq 的暴力对拍
  run.sh           构建 + 对拍 + 计时 + 生成 RESULTS.md
  RESULTS.md       结果（自动生成，别手改）
```
