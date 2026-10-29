# RESULTS —— 2D 5 点 Jacobi 模板：extC / C / Fortran

> 由 `bench/fortran/report.py` 从 `raw.txt` 生成，**别手改**；口径与判据见 `SPEC.md`。

## 口径

| 项 | 值 |
| --- | --- |
| CPU | `Intel(R) Core(TM) Ultra 9 275HX` |
| 内核 | `Linux 6.18.54.1-microsoft-standard-WSL2` |
| 核数 | `24` |
| C 编译器 | `cc (Ubuntu 15.2.0-16ubuntu1) 15.2.0` |
| Fortran 编译器 | `GNU Fortran (Ubuntu 15.2.0-16ubuntu1) 15.2.0` |
| extC 前端 | `./build/extc` |
| 钉核 | `6` |
| iters | `200` |
| RUNS（取 best） | `5` |
| n | `384 768 1536` |
| O2 旗子 | extc/c: -O2 -std=c11 -fwrapv · fortran: -O2 |
| O3 旗子 | extc/c: -O3 -march=native -fwrapv · fortran: -O3 -march=native |
| 向量化（-O3，`loop vectorized` 处数） | extC 生成的 C：3 · 手写 C：3 |

三臂同算法、同参数、同 stdout 字节（cs 列逐字节相同才算做了等价工作）；时间由外部
`time.perf_counter` 包整个进程，预热 1 次，`RUNS` 次取 best；慢 > 1。

## 主表 -O2（契约口径）

| n | extC ms | C ms | Fortran ms | extC ns/cell | extC÷C | extC÷Fortran | Fortran÷C | cs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 12.26 | 12.42 | 12.32 | 0.42 | 0.99 | 1.00 | 0.99 | 66699387122 |
| 768 | 54.47 | 54.41 | 55.55 | 0.46 | 1.00 | 0.98 | 1.02 | 272117169232 |
| 1536 | 488.54 | 485.33 | 495.33 | 1.04 | 1.01 | 0.99 | 1.02 | 1099292897289 |

## 旁证 -O3 -march=native

| n | extC ms | C ms | Fortran ms | extC ns/cell | extC÷C | extC÷Fortran | Fortran÷C | cs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 7.87 | 7.77 | 8.33 | 0.27 | 1.01 | 0.95 | 1.07 | 66699387122 |
| 768 | 43.90 | 43.73 | 44.48 | 0.37 | 1.00 | 0.99 | 1.02 | 272117169232 |
| 1536 | 312.20 | 305.58 | 312.26 | 0.66 | 1.02 | 1.00 | 1.02 | 1099292897289 |

## 怎么读

- 主表（-O2）平均：extC 是 C 的 **1.00×**，是 Fortran 的 **0.99×**，Fortran 是 C 的 **1.01×**。
- `extC÷C` 读作 extC 相对同一份 C 后端（同一套 `-O2 -std=c11 -fwrapv`）的语言实现余量；`extC÷Fortran` 是本次 headline；`Fortran÷C` 是前端差异。
- 小规模时进程启动与首次触页占比不可忽略，看比值别只看绝对值；规模增大后受内存带宽与末级缓存容量约束（两块 buffer 约 2·8·n² 字节），比值会向 1 收敛。
- 从 -O2 到 -O3 -march=native，平均提速：extC **1.45×**、C **1.48×**、Fortran **1.44×**。

- 机制旁证（GCC `-fopt-info-vec`，-O3 档）：手写 C 标出 `loop vectorized` 3 处，extC 生成的 C 3 处。这与上一行「C/Fortran 从 -O2 到 -O3 明显提速、extC 不提速」的档位差异同向；此处只记录编译器自报与秒表，不推断因果。

## 读这份数字时要记住的

- 单机、单形状、双 buffer 内存带宽型 kernel；不代表编译期、泛型、字符串、GC 等维度。
- RSS 是各自运行时的常驻内存（kbytes），三臂的运行时/分配器不同，只作量级参考，不当排名。
- 有意没做：`@unchecked`（关边界检查）变体、多线程、cache-blocked 变体；见 `SPEC.md` §4。

