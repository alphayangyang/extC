# bench —— 性能与对比卡具

这里的每个目录是一个**自足的卡具**：自带源码、自带 `run.sh`、自带判据。共同规矩只有三条：

1. **每臂的输出必须逐字节相同**（除非目录里写明"这是故意不对称的臂"）。跑不动或对不上就是红。
2. **`run.sh` 是唯一入口**，不带参数就跑完整一轮；报告写在目录里的 `RESULTS.md` 或 `out/`。
3. **库外的输入由生成器现场造**（`gen/` 或 `*.c`），不把大数据入库 —— `bench/io` 的 3.3 GB 输入就是这么来的（`.gitignore` 里）。

跑之前先 `make -j8`。所有数字都只在本机、本工具链下有意义。

## 主线（论文与 README 引用）

| 目录 | 量的是什么 | 需要的工具链 |
|---|---|---|
| [`realsvc/`](realsvc/) | 四轴真实负载横评：ETL 解析聚合 / 会话表 + TTL 回收 / 协程 TCP 应答 / 多核变换归约，extC 对 C++、Go、Rust | `cc` `g++` `go` `rustc` |
| [`memstudy/`](memstudy/) | 回收机制消融：6 个形状 × 11 个臂（关回收、提权、清扫、分片、预算…） | `cc` |
| [`gc/`](gc/) | GC 敏感形状横评，含跨语言臂（C++/Rust/Go/Java/Node 的公平与不回收两档）与暂停统计 | `cc` `g++` `rustc` `go` `java` `node` |
| [`app/`](app/) | 三个综合应用：日志管线 / 路由匹配 / 会话表，extC STL 对 C++ STL 对 Go | `cc` `g++` `go` |
| [`crazy/`](crazy/) | 150 万行 TSV → 解析 → 按 uid 聚合 → 编 JSON → 加权归约，extC 对 Rust、Node、Python | `rustc` `node` `python3` |
| [`stream/`](stream/) | 同一条逐元素管线在三种形状下的 DRAM 代价（物化 / 融合 / 流式） | `cc` |

## 语言特性单点

每个目录量一个语言机制，判据是"改动前后的时间差"，不是跨语言排名。

| 目录 | 量的是什么 |
|---|---|
| [`par/`](par/) | `par` 数据并行：串行 vs 多线程，逐字节对拍 |
| [`fusion/`](fusion/) | 元素链融合（同一份源码编两次，默认融合 / `--no-fuse`） |
| [`any-cost/`](any-cost/) | `any` 的成本：装箱、拆箱、动态派发、深拷贝 |
| [`dyn/`](dyn/) | `dyn Trait` 动态派发成本 |
| [`stl/`](stl/) | extC 容器对 C++ STL（同算法、同参数、输出必须逐位相同） |
| [`string_vs_cpp/`](string_vs_cpp/) | `string`（SSO）对 `std::string`：追加、查找、切分 |
| [`vararray/`](vararray/) | `varArray` 对 `vector`，校验和必须一致 |
| [`coro/`](coro/) | 协程挂起 / 恢复的纳秒成本 |
| [`runtime/`](runtime/) | 语言核心运行期与产物质量（行数、二进制大小、编译时间） |
| [`compile/`](compile/) | 编译时长压力：语料越大，编译时间怎么长 |
| [`zero/`](zero/) | "清零值多少钱"——所有基准的基线尺子 |
| [`zoneexit/`](zoneexit/) | 一个地方建 N 个池再退出，池边界本身的代价 |
| [`poolgc/`](poolgc/) | 池容器的 shrink 到底有没有用 |
| [`stress/`](stress/) | 压测：extC 与 C 同种子同序列，结果必须逐位相同 |

## 跨语言横评（需要多套工具链）

| 目录 | 量的是什么 | 需要的工具链 |
|---|---|---|
| [`bigmatrix/`](bigmatrix/) | 六语言重负载矩阵（含编译时间与峰值内存） | `cc` `g++` `go` `java` `rustc` `node` |
| [`multi/`](multi/) | mod / sieve / matmul 三题 × C / extC / JS | `cc` `node` |
| [`coro6/`](coro6/) | 六语言 × 协程 × 多进程（1/4/8） | 同上 |
| [`io/`](io/) | IO 速度大横评（流式读写、C++ `cin/cout` 两档） | `cc` `g++` |
| [`oi/`](oi/) | OI 数量级四语言横评 | `cc` `node` 等 |
| [`numpy-matmul/`](numpy-matmul/) | 同一份活：numpy 对照手写循环 | `python3` `numpy` `cc` |
| [`heavy/`](heavy/) | 重负载 extC 对 C，同算法同参数 | `cc` |
| [`echo/`](echo/) | echo server 压测：同条件 + 自然多核两列 | `cc` `go` |
| [`httpd/`](httpd/) | 最小 HTTP/1.1 静态服务器的功能判据与数字 | `cc` |
| [`llm-decode/`](llm-decode/) | extC 端到端跑 tiny LLM 的 decode | `cc`，外加 [`third_party/ggml`](../third_party/ggml/) |
| [`fortran/`](fortran/) | extC / C / gfortran 三臂 2-D Jacobi | `cc` `gfortran` |

## 规矩

加一个新卡具时：

- 目录里必须有 `run.sh`（可执行、无参数跑完）与 `README` 或 `RESULTS.md`（写清口径、机器、版本）。
- 源码里写清"哪一臂是公平臂、哪一臂故意不对称"。不对称要写进报告，不能只写比值。
- 生成的输入与产物全部进 `.gitignore`，只把生成器入库。
- 计时必须绑核（`taskset -c N`）并报 **min 或 median**，别报单次；跨语言比较时同一台机、同一轮交错跑。
- 跑出来的原始数字（`.tsv` / `.log`）留在 `out/` 且不入库；结论进 `RESULTS.md` 并同时写进 [`docs/changelog/`](../docs/changelog/)。
