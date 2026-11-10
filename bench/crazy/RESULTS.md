# bench/crazy：同一份输入、五种写法

五语言读同一份 **150 万行 / 44 MB** 的 TSV，做同样四件事：解析、按用户分组聚合、
把每 16 行拼成 JSON（extC 用运行期 `any` + 描述符编码，另外几家手工/内置编码）、加权归约。
**六份输出行逐字节相同**。时间单位 ms，`taskset -c 0`、5 轮取最小。

| 实现 | 代码行 | 解析 | 聚合 | JSON | 归约(1 线程) | 总耗时 | 相对 Rust |
|---|---|---|---|---|---|---|---|
| extC | 182 | 83 | 27 | 14 | 18 | **142** | 1.04× |
| Rust 1.93 | 128 | 79 | 23 | 10 | 35 | **147** | 1.00× |
| Viper 1.0.6 | 295 | 172 | 16 | 14 | 18 | **220** | 0.67× |
| Viper 1.0.6 + opt -O2 | 295 | 88 | 11 | 7 | 12 | **118** | 1.24× |
| Node 24 | 64 | 83 | 97 | 41 | 21 | **241** | 0.61× |
| Python 3.14 | 64 | 674 | 949 | 173 | 947 | **2744** | 0.05× |

Viper 两行是同一份 `bench/crazy/viper/main.vp` 的两次编译：`Viper 1.0.6` 走 `viperc` 的
Linux 流程（`mlir-opt` → `mlir-translate` → `llc`），`+ opt -O2` 只在 llc 之前插入 LLVM
中间端优化（`bench/viper/llcopt/llc` 把 `opt -O2` 放在 `llc` 前）。extC 走 `cc -O2`、Rust 走
`rustc -O`，所以带 opt 的那一行才是同口径比较；不带 opt 的那行是 ViperC 出厂 Linux 行为。

## 并行归约（extC `par` vs Rust `std::thread`）

同一份行数据、同样的 8 轮累加，单线程 vs 8 线程（`taskset -c 0` 对 `taskset -c 0-7`）：

| 实现 | 1 线程 | 8 线程 | 加速比 |
|---|---|---|---|
| extC | 18 ms | 7 ms | **2.50×** |
| Rust 1.93 | 35 ms | 9 ms | **4.09×** |

Node、Python 与 Viper 这一轴只跑单线程：Node 的 worker 要额外序列化，Python 的 GIL 让线程帮不上
（要用多进程 = 每个进程各自重新解析，不是同一件事），Viper 的 `vthreading` 是裸线程库、没有
对应的结构化归约端口，所以都不列。

checksums（六者相同）：`rows=1500000 slow=258441 top=147593056 hist=627197546 json_bytes=6595334 json_cs=759097693 par=375484695`

