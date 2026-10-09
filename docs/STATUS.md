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
| **池"非头序删除 O(k²)"**（2026-10-09 修）| 判据 [`tests/pool/zt_drop_order.extc`](../tests/pool/zt_drop_order.extc) 先红后绿（1805→9.3 ns/池）；两条链加 `prev`，记录 88B→104B，`_Static_assert` 钉住 |

## 3. 未决

**全部未决设计只在 [`docs/DESIGN-OPEN.md`](DESIGN-OPEN.md) 里。** 一屏概览：

**2026-10-09 走完一轮问答，只剩一件没定：**

| 事项 | 结论 |
|---|---|
| **大对象值/句柄语义** | **搁置**（作者再想）——**唯一未决** |
| `slice<T>` → `view<T>` | **已定**：改，单独一个纯机械 commit |
| `mut` 权限模型 | **已定**：方向 A′，**以红判据为门槛**（无判据不实现——健全性风险在"判根"，见 DESIGN-OPEN §1.3）|
| `!` / `@unchecked` 收尾四问 | **已定**：`@unchecked` **全关、无例外清单**（作者改判）；`!` hoist **要修**（一修两治）|
| 失败处理 S1–S4 | **已定**：先 **S1 + S1b**（S1 零语言改动不会作废；S1b 的数字是 S4 的输入）|
| **纯增量**（可插队）| `f"…"` v2 · stdlib 补齐 · `if let` |
| **编译器内部**（可搁置）| 多文件输出/运行时独立 TU · 协程协议换枚举 · `dropUnusedLocals` |

## 4. 已知残留（**不是"全绿"**）

| 洞 | 位置 | 状态 |
|---|---|---|
| `yield ref <帧内局部>` 被前端接受、生成物是畸形 C | [`tools/gate-escape-known-bad.txt`](../tools/gate-escape-known-bad.txt) | 闸门基线上**还剩 1 条**，未修 |
| AST 冻结 R1 施工单（parser 之外写 AST）| [`tools/ast-freeze-known-bad.txt`](../tools/ast-freeze-known-bad.txt) | **67 条**，P2 卫生债 |
| ASan / 差分 / callsite / scale / golden / layering 闸门 | 各自 `tools/*-known-bad.txt` | **全部为空** ✓ |
| **产物指纹基线已过期** | [`tools/codegen-baseline-2026-10-05-w4.sha256`](../tools/codegen-baseline-2026-10-05-w4.sha256) | **本次改动之前就与 HEAD 差 410/414**（`!`/`@unchecked` 等改产物的工作没重取基线）⇒ **它已不能当"我改了没有"的预言机**。要么重取，要么明确放弃它 |

## 5. 下一步（只做一件）

**已完成**：① pool 的**非头序删除 O(k²)** —— 判据先行（红 → 绿），两条链加 `prev`，产物的实际半径
**162/414 份变**（全是发 pool 运行期块的程序；性质 = 两个字段 + 注释 + `_Static_assert`，已逐份核过）。

**下一件（在飞）**：② `!` 的 hoist —— **一修两治**（无故拒绝 `while f()! {}` + `p.get(h)!.x = v` 静默丢弃）。
施工单已备好：**检查器**的可写性检查要**递归 base 链**（`EX_SIGN` 要求操作数是 place）；**codegen** 把
语句 hoist 换成**每实例 unwrap 助手**（纯 C11 —— `-Wpedantic` 会拒 GNU 语句表达式，这是对 §1.4② 原方案的修正）。

**排队的（顺序已定）**：
1. ② 修完 → 2. **`view<T>` 改名**（纯机械 commit，等没别的改动在飞时做）→
3. **S1 checked 原语 + S1b trap 统计** → 4. **A′ 的红判据**（`tools/escape-corpus/` 加 mut 洗白形状）→
5. heap 的 `free` 那一半从 [`prototype-heap/`](../prototype-heap/README.md) 搬进 `std::heap` → 6. 账本收缩（删状态句 + `check_status.py` 闸门）。

**还有一件零风险的事没做**：**push 那 18 个 commit**（远端仍落后）。

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
