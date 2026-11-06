# tests —— 判据

74 个套件、1,800 多个 `.extc` 文件。**不要一个一个跑**：统一入口是仓库根上的 `./check.sh`，它调 `tools/check.py`，把每个套件按档位编排。

```sh
./check.sh smoke     # 改完代码第一件事：高信号集，约 10 秒
./check.sh docs      # 只跑文档与工具闸门，秒级
./check.sh ci        # CI 档：非基准全量，去掉绑本机工具链的 callgrind 判据
./check.sh nobench   # 非基准全量，约 220 秒
./check.sh           # 全部，含基准（要 g++ / rustc / go / java / node）
./check.sh --list    # 看项名、分组、最近耗时
```

CI（[`.github/workflows/ci.yml`](../.github/workflows/ci.yml)）只跑 `ci` 一档，因为它要求的环境正好是 `cc` + `python3`。

## 核心套件

| 组 | 套件 |
|---|---|
| 语言核心 | [`ops/`](ops/) [`ops2/`](ops2/) [`flow/`](flow/) [`io/`](io/) [`fstring/`](fstring/) [`generics/`](generics/) [`instimpl/`](instimpl/) [`lam/`](lam/) [`dyn/`](dyn/) [`iflet/`](iflet/) [`errors/`](errors/)（负例） [`defaults/`](defaults/) [`ergonomics/`](ergonomics/) |
| 内存与所有权 | [`pin/`](pin/) [`drop/`](drop/) [`move/`](move/) [`value/`](value/) [`pool/`](pool/) [`heap/`](heap/) [`arena/`](arena/) [`any/`](any/) |
| 回收机制 | [`reclaim/`](reclaim/) [`reclaim-shapes/`](reclaim-shapes/) [`reclaim-elide/`](reclaim-elide/) [`reclaim-idle/`](reclaim-idle/) [`reclaim-perf/`](reclaim-perf/) |
| 并发 | [`coro/`](coro/) [`par/`](par/) [`domain/`](domain/) |
| 外部接口 | [`cabi/`](cabi/) [`extbind/`](extbind/) [`extpkg-system/`](extpkg-system/) [`python-ext/`](python-ext/) |
| 工具链 | [`cli/`](cli/) [`lsp/`](lsp/) [`packages/`](packages/) |
| 产物质量 | [`cgen/`](cgen/)（`-Wall -Wextra -Werror` 编生成 C） [`frozen/`](frozen/)（AST 冻结） |
| 安全 | [`asan/`](asan/) [`hostile/`](hostile/) [`attacks/`](attacks/) [`escape-fuzz/`](escape-fuzz/) |

其余目录是专项与历史套件，同样接在 `tools/check.py` 里、同样参与 `nobench`。

## 一个套件的形状

每个 `tests/<name>/run.sh` 自己负责编译、运行、比对，约定是：

- 有 `run.sh` 就跑；没注册进 `tools/check.py` 的套件不会被任何档位跑到 —— 加套件时**必须**注册。
- 常用助手：`one`（跑一个正例、比单行 stdout）、`neg`（期望编译失败并匹配报文）、`dump_fail`（失败时打印产物便于定位）、`withrc`（要求生成 C 里真的有回收运行期符号）。
- 负例放在套件的 `errors/` 子目录或 `*_neg.extc`，断言的是**编译错误文本**，不是"退出码非零"。
- 涉及内存的套件再跑一遍 ASan/UBSan 变体；涉及回收的套件要断言生成 C 里有没有 `extc_rc_` 符号（否则"没跑回收"会被当成"回收很快"）。

## 加一个判据的顺序

1. 先写**会红的**用例，跑一遍看到它按预期失败（红）。
2. 改实现，直到它变绿。
3. 再补一条反例，防止规则被放宽。
4. 注册进 `tools/check.py`，跑 `./check.sh nobench`。
5. 结果写进 [`docs/changelog/`](../docs/changelog/) 当月页 —— 判据也是交付物，不能只有代码。
