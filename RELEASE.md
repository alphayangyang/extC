# 发布与支持范围

这一页是**承诺**：alpha 期间什么叫"支持"、什么不在支持范围内、以及版本号怎么变。
凡是这里没写的，都不要假设它成立。

## 版本

版本只有一处定义：仓库根的 [`VERSION`](VERSION)（一行，例如 `0.0.1-alpha`）。
构建时它被写进 `build/version.h`，`extc --version` 与 `extc --help` 的首行都读它。

```console
$ extc --version
extc 0.0.1-alpha
target linux/x86_64; C backend: $CC (default `cc`)
stdlib: $EXTC_STD, else <directory of extc>/../stdlib
```

版本策略（语义化版本，alpha 阶段）：

| 变化 | 版本位 | alpha 期间的含义 |
|---|---|---|
| 语言语义、标准库不兼容改动 | **minor**（`0.x`） | 每个 `0.x` 都可能破坏旧程序，**不提供迁移期** |
| 修 bug、加功能但不破坏 | **patch** | 直接升级即可 |
| 预发布标记 | `-alpha` / `-beta` | 还没承诺兼容 |

`0.0.x` 阶段的**唯一承诺**：程序一旦通过检查，生成的 C 一定能被 `cc` 编过（`--check-c` 是这条承诺的自检）。
除此之外的 API、标准库形状、诊断文案都可能变。

## 支持平台

| 平台 | 状态 | 说明 |
|---|---|---|
| Linux x86_64（glibc ≥ 2.35） | **支持** | CI 覆盖 ubuntu-22.04 / ubuntu-24.04 / debian-12 |
| WSL2（Windows 上的 Linux） | **支持** | 与同一发行版的 Linux 行为一致；编辑器集成见 [`docs/topics/LSP.md`](docs/topics/LSP.md) |
| Linux aarch64 | 未测试 | 代码不假设 x86，但没人跑过 |
| macOS | **不支持** | 运行期协程绑 epoll；`tools/` 里的 shell 脚本假设 GNU 工具 |
| Windows 原生 | **不支持** | 同上；WSL2 是唯一受支持的 Windows 用法 |

**要求的工具**：C 编译器（`cc`，即 gcc 或 clang）与 `python3`（3.8+，跑构建闸门与工具）。
除此之外没有依赖：没有 CMake、没有 Rust 工具链、没有 Node、没有自动配置脚本。

## 安装

```console
$ make -j"$(nproc)"
$ make install PREFIX="$HOME/.local"
$ extc --version
```

`make install` 把二进制放进 `<PREFIX>/bin/extc`，标准库放进 `<PREFIX>/stdlib`。
**两个路径是一体的**：编译器按"二进制所在目录的 `../stdlib`"找标准库，所以只拷二进制、
不拷标准库，装出来的编译器连 `use std::io` 都编不过。要放进别处，用 `$EXTC_STD` 指定：

```console
$ EXTC_STD=/opt/extc/stdlib extc run main.extc
```

`make uninstall` 删掉这两处。

## alpha 承诺范围

**承诺**（会当作 bug 修）：

- 干净克隆能 `make -j"$(nproc)"` 成功，且**零警告**（CI 把警告当失败）。
- 类型检查通过的程序，生成的 C 能被 `cc` 编译（CI 在三个发行版上验证）。
- 六个基准形状的峰值内存与运行时行为与记录的数值同量级（见 [`bench/README.md`](bench/README.md)）。
- 文档里给出的示例能编过、能跑（示例有闸门）。
- `use std::…` 的标准库模块在安装后可用。

**不承诺**（alpha 期间可能变，或明确没做）：

- 语言语义稳定、标准库 API 稳定。
- 多线程 API 与多线程回收（机制已设计，实现在 alpha 之后，见 [`docs/design/PARALLEL.md`](docs/design/PARALLEL.md)）。
- macOS / Windows 原生支持。
- 调试信息与源码行号映射（`--no-line-map` 反而是常用档位）。
- 一次报出所有错误：编译器停下在**第一个**错误；警告会全部报出。
- 包注册中心：`extpkg` 只从 `packages.toml` 里的 URL 或系统 `pkg-config` 取包。
- 性能数字：跨语言表格是特定机器、特定负载的测量，不是承诺。

## 报告问题

一个有用的 bug 报告包含三样：`extc --version` 的输出、能复现的最小 `.extc` 文件、
以及期望行为与实际行为。生成的 C 也一并贴上的话，多数问题可以只靠它定位。

## 发布检查单（维护者）

1. `VERSION` 改成新版本号。
2. `make clean && make -j"$(nproc)"` 零警告；`./tests/run.sh` 全过；`./check.sh nobench` 全过。
3. `./check.sh docs` 全过（示例、开关页、手册与历史词闸门）。
4. 在**干净容器**里走一遍"克隆 → `make` → `make install` → 跑一个程序"（发行版矩阵见 CI）。
5. `docs/changelog/` 里这一版的条目齐全（`tools/check_changelog.py --gate`）。
6. 打 tag（`v0.0.1-alpha`）并附上这一版的 changelog 段与已知问题。
