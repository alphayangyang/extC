# extC

extC 是一门编译到 C11 的语言：**内存在编译期证明安全，越界与错误在编译期拦下；运行期保持 C 的速度。** 没有虚拟机、没有共享运行库、没有追踪式收集器，也从不写生命周期标注或 `free`。编译器能证明的东西，运行期不留痕迹。

*[English](README.md)* · [![ci](https://github.com/alphayangyang/extC/actions/workflows/ci.yml/badge.svg)](https://github.com/alphayangyang/extC/actions)

## 快速上手

```sh
make -j"$(nproc)"                             # 只需要 cc；产物是 build/extc
./build/extc --version                        # 0.0.1-alpha、目标平台、标准库位置
./build/extc --run examples/hello-world.extc   # 生成 C、编译、运行
make install PREFIX="$HOME/.local"            # 可选：装成 extc 命令
```

只支持 **Linux / WSL2**（协程运行时绑定在 epoll 上）；支持面、alpha 承诺与版本策略见 [`RELEASE.md`](RELEASE.md)。入门导览见 [`../extc-docs/guide/getting-started.md`](../extc-docs/guide/getting-started.md)。

## 它的形状

| | extC |
|---|---|
| 目标 | C11。生成的 C 交给 `cc` 编译；`#line` 把报错映射回 `.extc` 源码（`--no-line-map` 关闭） |
| 运行期 | 没有共享运行库；`hello` 只依赖 libc，约 16 KB |
| 内存 | 以块级 arena 为底：离开块就归还整块；活得比块长的值由按区域的清扫覆盖（是否发射计数代码按编译单元决定，多数单元一行都不发；从不遍历整个堆）；语言里没有 `free` |
| 检查 | 编译器能证明的写成编译错误，证明不了的变成带源位置的陷阱。没有静默降级路径 |
| 逃生口 | 只有两个：`!`（先检查，检查不过就陷阱）与 `@unchecked`（唯一的 UB 通道）。都必须在源码里写出来 |

回收怎么工作、保证什么、还缺什么见 [`../extc-docs/design/RECLAMATION.md`](../extc-docs/design/RECLAMATION.md)；通俗版是 [`../extc-docs/guide/memory-model.md`](../extc-docs/guide/memory-model.md)。

## 一段程序，与它生成的 C

```extc
use std::io

struct point { x: i32  y: i32 }

fn main() -> i32 {
    var p = point { x: 1, y: 2 }
    io::cout << "p = " << p.x << ", " << p.y << "\n"   // p = 1, 2
    var a: [4]i32 = [1, 2, 3, ...]                      // 定长数组，... 补零
    let s = a[1..3]                                     // 切片视图；能证明的边界不插检查
    io::cout << "s[0] = " << s[0] << "\n"              // s[0] = 2
    return 0
}
```

生成的 C 刻意无聊：不用宏花招，不做花哨优化。要看完整翻译单元，跑 `./build/extc examples/hello.extc`；`--check-c` 还会对它跑一遍 `cc -fsyntax-only`。更完整的导览在 [`examples/tour.extc`](examples/tour.extc)；语言参考是 [`../extc-docs/reference/language.md`](../extc-docs/reference/language.md)。

## 编译期就拦下

能证明的错误不是警告，是编译错误。把上面的 `a[1..3]` 改成越界下标：

<!-- expect-error -->
```extc
fn main() -> i32 {
    var a: [4]i32 = [1, 2, 3, ...]
    return a[7]
}
```

编译器回答 `bad.extc:3:1: error: index 7 is not inside `[4]i32` (length 4)`，并注明它能证明下标越界。有损窄化同样编译不过 —— 把 `i32` 赋给 `i8` 会得到 `initializer expects i8, found i32`；窄化必须写出来。证明不了的东西也不会被悄悄放过：它变成带源位置的陷阱，而不是错误答案。

## 能写什么

| 方面 | 有什么 |
|---|---|
| 语言 | 泛型 · `trait` 与 `dyn` · 带载荷的枚举 · `option`/`result`/`?`/`??` · `match`（含 `_` 分支） · `if let`/`while let` · 多名字声明与并行赋值 · 切片与引用 · 运算符重载 · `f"…"`（带格式说明符） · `any` 按方法名派发 · 属性（[语言参考](../extc-docs/reference/language.md)） |
| 内存 | 词法块 arena · `array<T,N>` · `varArray<T>` · `buf<T>` · domain；只给真正需要的编译单元发射回收运行期（[语言参考](../extc-docs/reference/language.md)） |
| 并发与 I/O | 协程与调度器 · 线程 · epoll 事件循环 · 终端 raw 模式（[标准库](../extc-docs/reference/stdlib.md)） |
| 系统与 C | `extern!` 对接 C 原语 · `@export` 被 C 调用 · 动态加载 · 链接开关与 `--pkg-config`（[模块与 C 互操作](../extc-docs/reference/modules-cabi.md)） |
| 标准库 | 控制台与文件 · 时间 · 终端 · JSON · Base64 · sha1/sha256/hmac · socket · HTTP · WebSocket · 进程 · 线程 · 堆 slab · 动态加载 · 事件循环 · 容器（[标准库](../extc-docs/reference/stdlib.md)） |

## 四件很少同处一门语言里的事

### 编译器能检查的数据并行

```extc
use std::io

fn main() -> i32 {
    var n: i64 = 1000000
    var out: mut slice<i64> = new i64[n]
    par i in 0..n {                       // 循环体不许调函数、不许分配
        var x: i64 = i * 3 + 7
        var acc: i64 = 0
        while acc < 4 { x = (x * 1103515245 + 12345) % 2147483647  acc = acc + 1 }
        out[i] = x & 1023
    }
    var s: i64 = 0
    var k: i64 = 0
    while k < n { s = s + out[k]  k = k + 1 }
    io::println(f"sum = {s}")
    return 0
}
```

没有锁、没有原子操作、没有要推理的数据竞争：编译器证明每个 worker 只写自己的槽、只读共享数据，否则直接拒绝这个循环。速率与判据在 [`bench/par/`](bench/par/RESULTS.md)。

### 不用线程的协程

```extc
use std::io

fn counter(n: i64) -> coroutine<i64> {
    var i: i64 = 0
    while i < n { yield i  i = i + 1 }
}

fn main() -> i32 {
    var total: i64 = 0
    for x in counter(4) { total = total + x }     // 暂停的是真栈帧，不建线程
    io::println(f"total = {total}")
    return 0
}
```

`yield` 挂起的是编译器提前排好布局的栈帧；恢复它约一纳秒，不分配。同一个运行时驱动 epoll，单核对外服务 HTTP 见 [`bench/realsvc/`](bench/realsvc/) 与 [`bench/httpd/`](bench/httpd/README.md)。

### 所有权写成标签，而不是魔法类型名

```extc
use std::io
use std::data::buf

fn main() -> i32 {
    var b: buf<i64> = buf<i64>::withCap(4)     // 拥有自己的字节；`@noCopy` 让赋值成为移动
    var i: i64 = 0
    while i < 10 {
        b.push(i)                               // 借用期间增长会陷阱，而不会静默 UAF
        i = i + 1
    }
    io::println(f"len={b.len()} last={b.view()[9]}")
    return 0
}
```

三个标签承载语义 —— `@noCopy`（值位置即移动）、`@value`（可复制，带 `clone` 钩子）、`@drop`（释放，每条退出路径都跑钩子）—— 编译器并不认识 `buf`、`string` 或 `vector` 这些名字。自己写容器、贴上同样的标签，就得到同样的生命周期保证与同样的诊断。

### 能推理的回收

每次分配都属于一个词法块，离开块就整块归还；逃出块的值被 *提升* 到调用者的区域，只复制一个指针宽的槽 —— 所以引用从不被重写，栈也从不被扫描。真会成环时，用增量、按预算、自适应的标记清扫回收。而逐函数的 `idle` 证明（不两次写引用字段、不跨引用调用、不丢引用）让编译器给该函数发出**零计数、零清扫代码** —— 工作负载里几乎所有函数都不需要。内存保持有界而不是持续增长；长跑峰值对比见 [`bench/memstudy/`](bench/memstudy/) 与 [`tests/reclaim-perf/`](tests/reclaim-perf/README.md)。

## 快在哪、慢在哪

六种语言（extC、C、C++、Rust、Go、Java）跑五种重活：数据全部从 stdin 来（一个生成器产出一份输入，六种语言读同一份），每种语言用自己的完整优化，六份输出逐字节相同才算数。测量脚本在 [`bench/bigmatrix/`](bench/bigmatrix/)；完整结果在 [`RESULTS.md`](bench/bigmatrix/RESULTS.md)，下表是快照。

| | extC | C | C++ | Rust | Go | Java |
|---|---|---|---|---|---|---|
| 五种形状的几何平均（1.00 = 该形状最快） | **1.17** | 1.52 | 1.62 | 1.80 | 1.94 | 2.23 |
| 峰值内存 | 五种形状都不高于 C | — | — | — | — | 高 1.6× 到 96× |
| 交付二进制 | 21–25 KB | 16–20 KB | 16–37 KB | 11.5 MB | 2.4 MB | 3–5 KB |

- `rebuild`（分配 + 回收）上 extC 明显领先：比 C 快 5.8×；`bt`（分配 + 递归）、`cdq`（随机访问）、`mandel`（浮点）基本与 C 持平。
- 差的那格也在表里：`radix`（排 10⁷ 个 u32）比 C 慢 1.66× —— 账单是切片下标的边界检查，加上 `new` 的零初始化保证。
- [`RESULTS.md`](bench/bigmatrix/RESULTS.md) 里有一条比数字更值钱的记录：同一棵越界的二叉索引树，C 版本悄悄算出了一个数，extC 版本当场陷阱并打印源位置。

更贴近应用的对比，每份报告都带自己的判据与注意事项：[`bench/app/`](bench/app/RESULTS.md)（STL 容器对 C++ 与 Go） · [`bench/httpd/`](bench/httpd/README.md)（约 300 行 HTTP/1.1 服务） · [`bench/fortran/`](bench/fortran/RESULTS.md)（一个数值核函数对 C 与 gfortran） · [`bench/par/`](bench/par/RESULTS.md)（语言级 `par`） · [`bench/stream/`](bench/stream/RESULTS.md)（流水线的 DRAM 代价）。

## 凭什么相信

跑这些，数字就是新的（`make status` 打印通过/失败计数与未推送提交）：

```sh
make -j"$(nproc)"     # 只需要 cc；产物 build/extc
./tests/run.sh        # 回归：正例必须跑对，编译错误例必须被拦，运行期错误例必须真的带源位置陷阱
./check.sh quick      # 上面这些 + 闸门 + 分项子系统用例（需要 cc 与 python3）
./check.sh ci         # 同上，去掉 callgrind 判据（CI 用这个：指令数与 miss 数绑定本机工具链）
./check.sh            # 全量验收：再加上整套 ASan/UBSan 语料与基准（需要 g++、rustc）
```

`quick` 里的闸门不是抽样，而是一条条核对的不变量：尺寸（任何输入都必须出结论 —— 不崩、不挂） · 逃逸可靠性 · 差分（extC 与等价 C 必须同结论） · 实例接线 · 攻击库（接受集与基线逐行比对）。再上面是元闸门：手册覆盖 · 驱动开关页与 `extc --help` 一致 · 类型分支穷尽 · include guard · 并发守卫，以及检出文档项目时读文档项目的闸门。棘轮类（公开面覆盖、Doxygen 欠账）只许变小。

## 文档

今天的用法、机制与历史都在**独立的文档项目** [`../extc-docs/`](../extc-docs/)（兄弟克隆，不是子目录）：

- [`../extc-docs/guide/`](../extc-docs/guide/) —— 上手 · 内存模型 · 内部实现 · 编辑器支持
- [`../extc-docs/reference/`](../extc-docs/reference/) —— 语言 · 标准库 · 模块与 C 互操作
- [`../extc-docs/design/`](../extc-docs/design/) —— 机制：回收、类型与内存模型、并行、容器…
- [`../extc-docs/plans/`](../extc-docs/plans/) —— 还差什么（唯一清单）
- [`../extc-docs/devlog/changelog/`](../extc-docs/devlog/changelog/) —— 开发历史（唯一一份）
- [`../extc-docs/archive/`](../extc-docs/archive/) —— 历史快照（只做考古，**不要当现状读**）

本仓库只留随代码一起变的页：[`docs/README.md`](docs/README.md)（构建、能力、导航） · [`docs/flags.md`](docs/flags.md)（驱动开关，生成） · [`docs/surface.md`](docs/surface.md)（公开面，生成） · [`docs/SUGAR.md`](docs/SUGAR.md)（语法糖手册） · [`docs/COMMENT-STYLE.md`](docs/COMMENT-STYLE.md)（注释规则）。维护规则见 [`docs/README.md`](docs/README.md) 与 [`../extc-docs/STYLE.md`](../extc-docs/STYLE.md)。

## 仓库结构

```
src/        编译器（C11，按 pass 拆）        stdlib/  预置与标准库（用 extC 写）
tests/      回归（唯一入口 ./check.sh）       bench/   性能与对比台架
docs/       随代码变的文档页                 examples/  示例
tools/      辅助脚本与闸门                   third_party/  第三方绑定（-I 引入）
prototype/  早期原型（只做考古）              proofs/  机器校验的不变量证明
```

- 要改代码，先读 [`CONTRIBUTING.md`](CONTRIBUTING.md)。
- 每个基准测什么见 [`bench/README.md`](bench/README.md)；判据怎么组织见 [`tests/README.md`](tests/README.md)。文档项目检出在本仓库旁边（`../extc-docs`）；没有它时跨仓库文档闸门打印 `跳过` 并以 0 退出，不会失败。

## 许可证
Apache License 2.0，版权 Yang Yang。全文见 [`LICENSE`](LICENSE)，署名见 [`NOTICE`](NOTICE)。
