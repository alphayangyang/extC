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
| 向量化（-O3，`loop vectorized` 处数） | extC 生成的 C：0 · 手写 C：3 |

三臂同算法、同参数、同 stdout 字节（cs 列逐字节相同才算做了等价工作）；时间由外部
`time.perf_counter` 包整个进程，预热 1 次，`RUNS` 次取 best；慢 > 1。

## 主表 -O2（契约口径）

| n | extC ms | C ms | Fortran ms | extC ns/cell | extC÷C | extC÷Fortran | Fortran÷C | cs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 24.36 | 12.74 | 12.96 | 0.83 | 1.91 | 1.88 | 1.02 | 66699387122 |
| 768 | 99.11 | 58.70 | 59.55 | 0.84 | 1.69 | 1.66 | 1.01 | 272117169232 |
| 1536 | 683.24 | 658.26 | 660.95 | 1.45 | 1.04 | 1.03 | 1.00 | 1099292897289 |

## 旁证 -O3 -march=native

| n | extC ms | C ms | Fortran ms | extC ns/cell | extC÷C | extC÷Fortran | Fortran÷C | cs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 26.98 | 8.33 | 8.75 | 0.91 | 3.24 | 3.08 | 1.05 | 66699387122 |
| 768 | 106.36 | 45.03 | 45.74 | 0.90 | 2.36 | 2.33 | 1.02 | 272117169232 |
| 1536 | 686.36 | 385.90 | 384.50 | 1.45 | 1.78 | 1.79 | 1.00 | 1099292897289 |

## 怎么读

- 主表（-O2）平均：extC 是 C 的 **1.55×**，是 Fortran 的 **1.53×**，Fortran 是 C 的 **1.01×**。
- `extC÷C` 读作 extC 相对同一份 C 后端（同一套 `-O2 -std=c11 -fwrapv`）的语言实现余量；`extC÷Fortran` 是本次 headline；`Fortran÷C` 是前端差异。
- 小规模时进程启动与首次触页占比不可忽略，看比值别只看绝对值；规模增大后受内存带宽与末级缓存容量约束（两块 buffer 约 2·8·n² 字节），比值会向 1 收敛。
- 从 -O2 到 -O3 -march=native，平均提速：extC **0.94×**、C **1.51×**、Fortran **1.50×**。

- 机制旁证（GCC `-fopt-info-vec`，-O3 档）：手写 C 标出 `loop vectorized` 3 处，extC 生成的 C 0 处。这与「extC 的 -O3 档相比 -O2 几乎不提速，而 C/Fortran 明显提速」的现象一致；此处只记录编译器自报与秒表，不推断因果。

## 读这份数字时要记住的

- 单机、单形状、双 buffer 内存带宽型 kernel；不代表编译期、泛型、字符串、GC 等维度。
- RSS 是各自运行时的常驻内存（kbytes），三臂的运行时/分配器不同，只作量级参考，不当排名。
- 有意没做：`@unchecked`（关边界检查）变体、多线程、cache-blocked 变体；见 `SPEC.md` §4。

