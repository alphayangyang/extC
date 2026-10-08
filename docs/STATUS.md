# extC 现状（唯一入口）

> **全仓库只有这一份文件描述"当前状态"。** 其它文档要么是**参考**（手册、设计论证），要么是**档案**
> （决策、日志、审计证据）——它们在正文第一行申明自己的角色。
>
> **新对话从这里开始。不要再新建"当前状态/交接/路线图"文档** —— 本仓库以前同时存在六份自称入口的
> 文档，结果互相矛盾，读的人只能更乱。状态有一处就够。
>
> 最后核对：**2026-10-08**（数字由命令产生，见 §1；不手抄。）

---

## 1. 实测状态

| 项 | 命令 | 结果 |
|---|---|---|
| 构建 | `make -j"$(nproc)"` | **0 诊断** |
| 回归用例 | `./tests/run.sh` | **330 通过 / 0 失败** |
| 快速验收 | `./check.sh quick` | **72 通过 / 0 失败** |
| 工作树 | `git status` | **干净** |
| 未 push | `git rev-list --count @{u}..HEAD` | **17 个 commit**（⚠️ 未同步到远端）|

三个数字用 `make status` 重取。**不要把这些数字抄进别的文档** —— 抄了就会漂，
本仓库以前每一份过期的状态都是在别人的文档里手抄的一份副本。

## 2. 已落地（不要再讨论）

| 事 | 证据 |
|---|---|
| `global`（函数内持久存储）| `tests/coro` 35/35 |
| `f"…"` v1（编译期展开成 `<<` 链，与手写逐字节相同）| `tests/fstring` 9/9 |
| **codegen 按需发射** W0/W1/W2a/W2b-1/W4 + 大函数拆分（1621→157 行）| 414 份产物指纹逐字节核过 · 生成物 −5.7% |
| **`!` / `@unchecked` 语义修复**：`!` = 插检查 + 带 `file:line` 的 trap；`@unchecked` = 唯一 UB 通道 | `tests/annot` 27/0 · 新增 `tests/traps/sign_*` |
| 2026-09-30 全量审阅的 P0 批次（22 条）| 四道闸门 known-bad 基线**已清空** |

## 3. 未决

**全部未决设计只在 [`docs/DESIGN-OPEN.md`](DESIGN-OPEN.md) 里。** 一屏概览：

| 母题 | 待拍 |
|---|---|
| **可见性 vs 书写负担** | 大对象值/句柄语义 · `slice<T>`→`view<T>` 改名 · `mut` 权限模型 A′/B′/C′ · `!` 收尾四问 |
| **默认行为 + 逃生门** | 失败处理 S1/S1b/S2/S3/S4（已定：不用 effects · 放弃 longjmp · 纯值 `err` 载体 · mayFail 进 fn 类型）|
| **纯增量**（可插队）| `f"…"` v2 · stdlib 补齐 · `if let` |
| **编译器内部**（可搁置）| 多文件输出/运行时独立 TU · 协程协议换枚举 · `dropUnusedLocals` |

## 4. 已知残留（**不是"全绿"**）

| 洞 | 位置 | 状态 |
|---|---|---|
| `yield ref <帧内局部>` 被前端接受、生成物是畸形 C | [`tools/gate-escape-known-bad.txt`](../tools/gate-escape-known-bad.txt) | 闸门基线上**还剩 1 条**，未修 |
| AST 冻结 R1 施工单（parser 之外写 AST）| [`tools/ast-freeze-known-bad.txt`](../tools/ast-freeze-known-bad.txt) | **67 条**，P2 卫生债 |
| ASan / 差分 / callsite / scale / golden / layering 闸门 | 各自 `tools/*-known-bad.txt` | **全部为空** ✓ |

## 5. 下一步（只做一件）

1. **push 那 17 个 commit** —— 零风险、让 CI 验证、保住成果；
2. 然后**只挑一件**：
   - 想立刻手感变好 ⇒ **`f"…"` v2**（纯增量，落点已定）；
   - 想把"太容易 trap"变成数字 ⇒ **S1 + S1b**（便宜）；
   - 想先把语义定下来 ⇒ **只写规则表**，`mut`/`string` 那边留到想清楚。

> 一次只开一个母题。本仓库的混乱史里，每一次都是同时追多个主题造成的。

## 6. 文档地图（谁是什么角色）

| 文档 | 角色 | 什么时候读 |
|---|---|---|
| **本文件** `STATUS.md` | **当前状态（唯一入口）** | 每次接手 |
| [`DESIGN-OPEN.md`](DESIGN-OPEN.md) | **未决设计** | 要拍板语义时 |
| [`manual/`](manual/README.md) | 语言参考（用户面） | 查语法/标准库 |
| [`SPEC.md`](SPEC.md) · [`DESIGN.md`](DESIGN.md) | 定位与原则 | 想知道"为什么这样设计" |
| [`topics/`](topics/) | 专题论证（研究材料） | 深入某个机制 |
| [`DECISIONS.md`](DECISIONS.md) | **档案**：决策登记册 | 查"以前定过没有" |
| [`PLAN.md`](PLAN.md) | **档案**：缺陷编号权威（`PLAN #NN`）+ 里程碑 | 查某个编号 |
| [`DEVLOG.md`](DEVLOG.md) | **档案**：开发日志（倒序） | 查"当时怎么想的" |
| [`DESIGN-RESOLUTIONS.md`](DESIGN-RESOLUTIONS.md) · [`STALE-AUDIT.md`](STALE-AUDIT.md) | **档案**：审计期裁定 R1–R18 / 过时清单 | 追溯审计结论 |
| [`reviews/`](reviews/) · [`../.audit/`](../.audit/README.md) | **档案**：审计报告与原始证据 | 复核某条 finding |

**规矩**：档案只追加、不回改，且**不描述当前状态**；`PLAN #NN` 编号以 [`PLAN.md`](PLAN.md) 为准。
