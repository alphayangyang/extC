# logstat — 一个能跑的真程序

读一份 TSV 日志、流式聚合成报表；自带数据生成器，也能把报表用 HTTP 提供出来。
**一个小项目**（`src/` 下五个文件），三个子命令是三个 `@module` 入口 —— 一个二进制带三个入口，
分发表由编译器生成。

```
ts <TAB> path <TAB> status <TAB> lat_us <TAB> bytes
```

## 跑

```sh
./build/extc run examples/logstat -- -m gen access.tsv 2000000    # 写 ~78 MB
./build/extc run examples/logstat -- -m summary access.tsv        # 报表
./build/extc run examples/logstat -- -m serve access.tsv 8080 &   # 报表已渲染，直接应答
curl -s localhost:8080/summary     # JSON
curl -s localhost:8080/text        # 与人看的同一张表
./build/extc run examples/logstat -- --help                       # 列出三个入口
```

## 文件

| 文件 | 作用 |
|---|---|
| `src/main.extc` | 项目入口：不带 `-m` 时打印用法；`use` 进来三个模块 |
| `src/core.extc` | 真正的活：聚合表、流式扫描、报表、数据生成器、HTTP 服务 |
| `src/gen.extc` | `@module("gen", …)` —— 参数校验 + 调 `core::genFile` |
| `src/summary.extc` | `@module("summary", …)` —— 校验 + `core::scanFile` + `core::reportNow` |
| `src/serve.extc` | `@module("serve", …)` —— 校验 + `core::serveFile` |

## 它演示了什么

- **一个二进制、多个入口（`@module`）**：`logstat --help` 列出三个 `-m` 名字与描述，
  `-m` 由编译器生成的查表分发；模块之间是普通 `use core` + `core::…` 调用。
- **内存不随输入长**：256 KiB 块流式读，行在原地解析，聚合表有界（状态表 600 项、
  延迟 32 桶、路径 4096 条封顶后计入 `other`）。实测峰值 RSS **2.2 MB**，
  0.7 MB 与 388 MB 的输入一样。
- **没有逐记录分配**：每行不 `new` 任何东西；整个程序只有三处 `new`——扫描缓冲、
  生成缓冲、每连接的 8 KiB 读缓冲，生命周期都是"一次运行/一个连接"。
- **协程服务**：`serve` 一个线程、一个 epoll 循环、每连接一个协程，没有线程池；
  应答从预渲染缓冲里拷出去，`yield` 等可读而不是忙等。
- **`for` 与 `while` 各干各的**：计数型循环一律 `for i in 0..n`（含 `for k in lo..hi + 1`），
  `while` 只留给"靠计算推进"的循环 —— 十进制转换、部分写补齐、读块循环、连接循环。

## 诚实的边界

- p50/p99 是 **1 ms 桶**的近似（延迟直方图 32 桶），不是精确分位数。
- `gS` 与两套渲染缓冲是程序级的：本程序一次只处理一份文件，所以够用。
- top-5 用选择排序，O(5·n_paths)，路径数封顶 4096。
- 没有模板、没有框架：十进制格式化是手写的 `putInt`。
