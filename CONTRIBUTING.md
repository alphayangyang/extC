# 参与 extC

extC 是一个编译器（约 6.3 万行 C）加一个标准库（30 个模块、约 1.1 万行 extC）。它把 extC 编译成可读的 C11，再交给系统的 `cc`。**没有 LLVM，没有自己的后端**。

## 一分钟上手

```sh
make -j8                       # 构建编译器，约 1.5 秒
./build/extc run examples/hello-world.extc   # 跑第一个程序
./check.sh smoke               # 改完代码第一件事（约 10 秒）
```

不需要配置任何东西：只要 `cc` 与 `python3`。跑基准才需要 `g++` / `rustc` / `go` / `java` / `node` / `gfortran`。

## 改动之前

- **语言当前的口径**在 [`docs/usage/`](docs/usage/) —— 那是手册，不是提案。要改语义，先改文档再改实现。
- **不许破的决定**在 [`docs/design/CORE.md`](docs/design/CORE.md)；类型与存储的选型在 [`docs/design/TYPE-MODEL.md`](docs/design/TYPE-MODEL.md) 与 [`docs/design/MEMORY-MODEL.md`](docs/design/MEMORY-MODEL.md)。
- **待做清单**在 [`docs/plans/TODO.md`](docs/plans/TODO.md)。

## 提交一个改动

1. **先让判据红。** 新规则先写成会失败的用例，跑一遍确认它按预期失败。
2. **再让它绿。** 实现之后跑 `./check.sh smoke`，再跑 `./check.sh nobench`（约 4 分钟，含全部门禁）。
3. **不留回归。** `tests/run.sh`、`check.sh docs`、`check.sh nobench` 必须全绿；生成物、`.o`、跑出来的数据都不入库。
4. **写 changelog。** 每条改动在 [`docs/changelog/`](docs/changelog/) 当月页写三段：症状、结构（为什么这么改）、判据（哪条用例钉住它）。`python3 tools/check_changelog.py --gate` 会检查格式。
5. **提交信息**用一句话说清"改了什么"，必要时在正文写代价与判据。中文可以，英文可以，别混。

## 代码约定

| 位置 | 约定 |
|---|---|
| `src/` 注释 | **英文**，doxygen 风格（`@file` / `@brief` / `@param` / `@return`）；单行能说清的就单行。闸门：`tools/check_doxygen.py` |
| `src/` 禁写历史 | 不写"以前是 X、现在改成 Y"；历史只进 changelog。闸门：`tools/check_no_history.py` |
| 生成 C 的注释 | 面向读产物的人，同样英文 |
| `stdlib/` 注释 | 中文（与文档一致），模块级要写 `@code` 示例 |
| 文档 | 只写"现在有什么"，不写历史；不写"见 §3.2"这类编号引用，用链接。闸门：`tools/check_doc_refs.py`、`tools/check_doc_examples.py` |

## 门禁都在哪

`./check.sh --list` 会打印全部检查项。几个主要的：

- `tools/check.py` —— 编排层，套件清单与分档都在这里（加套件必须注册）。
- `tools/check_doxygen.py` / `check_no_history.py` / `check_walkers.py` / `check_ast_freeze.py` —— 源码结构闸门。
- `tools/check_doc_*.py` —— 文档事实、示例、链接。
- `tools/check_changelog.py` —— changelog 格式与分页。
- `tools/check_tree.py` —— 工作区不该出现的东西（裸产物、临时文件）。

## 许可

见 [`LICENSE`](LICENSE) 与 [`NOTICE`](NOTICE)。第三方代码在 [`third_party/`](third_party/)。
