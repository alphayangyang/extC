# extC

extC 是一门编译到 C11 的语言，用编译期检查替代运行时机制。

> 凡是编译期能够证明的，运行时不留痕迹。

[![ci](https://github.com/alphayangyang/extC/actions/workflows/ci.yml/badge.svg)](https://github.com/alphayangyang/extC/actions)

## 特点

- **编译到 C11**。没有虚拟机、没有共享运行时库、没有追踪式垃圾回收 —— 运行期需要的东西随生成物一起
  发出来，生成的 C 面向人阅读，带 `#line` 映射指回源码行（`--no-line-map` 可关闭）。
- **内存分两层管**：每个词法作用域对应一只 arena，离开作用域即整体归还（语言不提供 `free`）；
  活得比作用域长的对象由引用计数按持有者归零释放，环则由**区域内的清扫**收掉。
  两者都不需要写屏障，也不扫整个堆 —— 机制与不变量见 [`docs/RECLAMATION.md`](docs/RECLAMATION.md)。
- **错误处理分两层**。编译期能够证明的（越界、除零、移位溢出、有损收窄、逃逸与借用）一律报编译错误；
  无法证明的保留为携带源码位置的运行时陷阱。不存在静默退化的路径。
- **唯一的逃生舱是 `!`**（`a[i]!`、`extern!`）。它的语义只有一条：调用者签字接受运行时的后果。
  因此 `grep -c '!'` 可以完整清点全部逃生舱。

## 示例

```extc
struct point { x: i32  y: i32 }

fn main() -> i32 {
    var p = point { x: 1, y: 2 }
    println("p = ", p)                 // p = point { x: 1, y: 2 }
    var a: [4]i32 = [1, 2, 3, ...]     // 定长数组，... 补零
    let s = a[1..3]                    // 切片视图；能证明的边界不生成检查
    println("s[0] = ", s[0])           // s[0] = 2
    return 0
}
```

完整巡礼见 [`examples/tour.extc`](examples/tour.extc)，语言用法见 [`docs/usage/`](docs/usage/)。

## 性能

六种语言（extC、C、C++、Rust、Go、Java）在五个重负载形状上的对比。数据全部从标准输入读入
（一个生成器产出一份输入，六种语言读同一份），每种语言使用各自的完整优化。
测量脚本见 [`bench/bigmatrix/`](bench/bigmatrix/)，完整结果见 [`RESULTS.md`](bench/bigmatrix/RESULTS.md)。

| | extC | C | C++ | Rust | Go | Java |
|---|---|---|---|---|---|---|
| 五形状几何平均（1.00 = 该形状内最快）| **1.16** | 1.63 | 1.67 | 1.75 | 1.74 | 2.29 |
| 峰值内存 | 五个形状均不高于 C | — | — | — | — | 高出 1.6 至 100 倍 |
| 交付文件 | 20 KB | 16 KB | 16–37 KB | 11.5 MB | 2.4 MB | 3–5 KB |

- `bt`（二叉树）与 `rebuild`（分配与回收）两项 extC 最快。
- `mandel`（浮点）与 `cdq`（随机访问）与 C 相差 5% 以内。
- `radix`（10⁷ 个 u32 排序）比 C 慢 1.64 倍。原因可量化：切片索引的边界检查，以及 `new` 的零初始化保证。

**表格里的数字是快照**（取自 [`bench/bigmatrix/RESULTS.md`](bench/bigmatrix/RESULTS.md)，生成于 2026-09-24），
重跑会得到新数 —— 与"通过/失败数不写在这里"是同一条规矩：会漂的副本不进本文件。

**另需注意**：那张快照早于回收接进默认路径。五个形状里有**两个**（`bt`、`rebuild` —— 逐结点分配）
会走回收档，运行开销明显上升；其余三个以数组/切片为主，不进回收档。倍率、剖析与归因见
[`tests/reclaim-perf/`](tests/reclaim-perf/README.md)。

另有三种**应用形状**上的 STL 横评（extC STL vs C++ STL vs Go）：会话/连接表（高频读写删的
哈希表）、路由表（有序表的前驱查找与区间扫描）、请求日志缓冲（动态字节缓冲）。测量脚本见
[`bench/app/`](bench/app/)，完整结果与归因见 [`RESULTS.md`](bench/app/RESULTS.md)。

## 快速开始

```sh
make -j"$(nproc)"                        # 只依赖 C11 标准库，产出 build/extc
./build/extc --run examples/hello.extc   # 生成 C、编译并运行
./tests/run.sh                           # 回归用例
./check.sh quick                         # 快速验收（完整模式另含黄金闸门与基准）
./check.sh                               # 完整验收：另含基准
make status                              # 重取上面两条的通过/失败数
```

**通过/失败数不写在这里** —— 手抄的副本一定会漂。跑 `make status` 取当前数。

| 目标 | 依赖 |
|---|---|
| `make`、`tests/run.sh`、`check.sh quick` | `cc`（gcc 或 clang）、`python3` |
| `check.sh`（完整模式，含基准）| 另需 `g++`、`rustc` |
| `bench/bigmatrix/run.sh`（六语言横评）| 另需 `go`、`java` |

常用驱动选项：

```sh
./build/extc <file.extc>            # 生成的 C 输出到 stdout
./build/extc -o out.c <file.extc>   # 写入文件
./build/extc --check-c <file.extc>  # 生成后再做一次 cc -fsyntax-only
./build/extc -O3 -march=native --run <file.extc>
./build/extc --help                 # 全部选项
```

## 文档

文档只有三类内容：

| 文档 | 内容 |
|---|---|
| [`docs/usage/`](docs/usage/) | **现在有什么 / 怎么用** |
| [`docs/RECLAMATION.md`](docs/RECLAMATION.md) | **回收机制**：为什么这么设计、遵守哪些不变量、还缺什么（证据入口在它的末节）|
| [`docs/TODO.md`](docs/TODO.md) | **有什么待做**（唯一清单） |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | **开发历史**（全仓库唯一） |
| [`docs/archive/`](docs/archive/README.md) | 历史快照（只作考古，**不要当现状读**） |

维护规矩见 [`docs/README.md`](docs/README.md)；注释规范见 [`docs/COMMENT-STYLE.md`](docs/COMMENT-STYLE.md)。

## 仓库结构

```
src/       编译器（C11 实现，按 pass 拆分）
stdlib/    prelude 与标准库（用 extC 写；std/sys 是唯一声明 C 原语的一层）
examples/  示例          tests/  回归用例      tools/  辅助工具
bench/     横向基准（含六语言矩阵与 bench/app 的三场景 STL 横评）   docs/   文档
prototype-reclaim/  回收机制的原型与数值（C 探针 + 跨语言对照）
proofs/             机制不变量的机器检查证明
```

## 许可证

Apache License 2.0，版权归 Yang Yang 所有。许可证全文见 [`LICENSE`](LICENSE)，归属声明见 [`NOTICE`](NOTICE)。
