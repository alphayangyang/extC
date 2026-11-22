# extC 编译器仓库文档

状态：当前口径 · 版本 0.0.1-alpha · 平台 Linux x86_64 / WSL2

本目录**只放随代码一起变的东西**。活文档、设计、计划、开发历史与分析都在兄弟仓库
[`../extc-docs/`](../extc-docs/)（独立项目），不在这里。第一次来，先看根 [README.md](../README.md)。

## 这是什么

extC 编译到 C11：**内存安全、边界与错误检查放进编译期，运行期留给 C 的速度**。没有虚拟机、
没有共享运行时库、没有追踪式回收；写代码既不用标生命周期，也不用调 `free`。

## 构建与验收

```sh
make -j"$(nproc)"                          # 只需要 cc，产出 build/extc
./build/extc --version
./build/extc --run examples/hello-world.extc
./tests/run.sh                             # 全量回归（正例跑对 / 反例被拦 / 运行期错误真的 trap）
./check.sh quick                           # 回归 + 闸门 + 各子系统套件（要 cc 和 python3）
./check.sh docs                            # 只跑文档闸门（含文档项目）
./check.sh                                 # 全量验收（含 ASan/UBSan 与基准）
```

依赖只有 `cc` 与 `python3`；跨语言基准另需 `g++` / `rustc` / `go` / `java` / `gfortran`。
支持平台、alpha 承诺边界与版本策略见根 [RELEASE.md](../RELEASE.md)。

## 能力简表

| 面 | 现在有什么 |
|---|---|
| 语言 | 泛型 · `trait` 与 `dyn` · 带载荷枚举 · `option`/`result`/`?`/`??` · `match` · `if let`/`while let` · 切片与引用 · 运算符重载 · `f"…"` · 注解 |
| 内存 | 按词法块的 arena · `array<T,N>` · `varArray<T>` · `buf<T>` · 域；只给真需要回收的单元发回收运行期 |
| 并发与 I/O | 协程与调度器 · 线程 · epoll 事件循环 · 终端 raw 模式 |
| 系统与 C | `extern!` 声明 C 原语 · `@export` 反向给 C 调 · 动态加载 · 链接开关与 `--pkg-config` |
| 标准库 | 控制台与文件 · 时间 · 终端 · JSON · Base64 · sha1/sha256/hmac · 套接字 · HTTP · WebSocket · 进程 · 线程 · 堆板 · 动态加载 · 事件循环 · 容器 |

逐项现状以文档项目的 [reference/](../extc-docs/reference/) 为准。

## 本目录的页

| 页 | 内容 | 谁生成 |
|---|---|---|
| [flags.md](flags.md) | 驱动开关（`extc --help` 的镜像，单一真源） | `tools/gen_flags.py` |
| [surface.md](surface.md) | 标准库公开面清单（手册覆盖率的判据） | `tools/manual_surface.py` |
| [SUGAR.md](SUGAR.md) | 语法糖手册：写法 ↔ 展开 ↔ 求值次数 ↔ 用例 | 手写，与 `tests/sugar/` 双向绑定 |
| [COMMENT-STYLE.md](COMMENT-STYLE.md) | 注释规范：`src/` 英文 Doxygen、`stdlib/` 中文 + `@code` 示例 | 手写 |

生成的页不手改：改源码或生成器，重跑工具；`./check.sh docs` 会核对它们与真源一致
（`gen-flags` / `manual-surface` / `gen-index` 三项）。

## 文档项目（兄弟仓库）

活文档与历史在独立仓库 `~/extc-docs`，与编译器仓库**兄弟克隆**（不是子目录）：

| 位置 | 内容 |
|---|---|
| `../extc-docs/guide/` | 上手、内存模型、内部结构、编辑器支持 |
| `../extc-docs/reference/` | 语言、标准库、模块与 C 互操作 |
| `../extc-docs/design/` | 机制设计：回收、类型与内存模型、并行、容器、哈希表…… |
| `../extc-docs/plans/` | 待做清单与计划 |
| `../extc-docs/devlog/` | 开发历史（按月 changelog）、分析报告、整理记录 |
| `../extc-docs/archive/` | 历史快照，只做考古，**不当作现状** |

跨仓库约定：文档项目默认 `$HOME/extc-docs`，用 `EXTC_DOCS` 覆盖；文档项目里的符号锚点对着
`../extC_Compiler` 的源码解析，用 `EXTC_SRC` 覆盖。**文档项目不在时**，跨仓库闸门打印
`  --   跳过：…` 并退出 0：明说没验，不混进通过。

跨仓库的文本引用（注释、脚本输出、bench 报告）统一写 `../extc-docs/…`，即相对编译器仓库根的兄弟目录路径；
Markdown 链接按所在文件的实际深度重算。

写作与维护规则见文档项目的 [STYLE.md](../extc-docs/STYLE.md)：活文档只写现状、不写历史；
引用符号不引用行号；示例必须能编过；历史进 `devlog/changelog/`。

## 改文档时

1. 改的是随代码走的页（本目录 4 页、根 README / CONTRIBUTING / RELEASE）→ 跑 `./check.sh docs`。
2. 改的是活文档、设计或历史 → 在 `~/extc-docs` 里改，跑那边的闸门，再回本仓库跑 `./check.sh docs`
   核对符号锚点与示例（`doc-project-anchors` / `doc-project-facts` / `doc-project-examples`）。
3. 生成物（`docs/flags.md`、`docs/surface.md`、`stdlib/INDEX`）只改生成器，不手改。
