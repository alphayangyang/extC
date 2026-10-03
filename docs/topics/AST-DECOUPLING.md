# AST 解耦：目标架构与分期（2026-10-02）

> **一句话结论**：现在的混乱**不需要重写 4 万行**，但**必须换一层**——
> 缺的不是"更多接缝"，而是**分析结果那一层**（一个 owner 一份结果包，按节点 ID 寻址，
> 只读冻结后写一次）。这一层主流编译器都有，extC 一直没有。

---

## 1. 判断：重写还是换层？（先给量出来的东西）

| 量的是什么 | 实测 | 含义 |
|---|---|---|
| 全仓规模 | `src/` **40 230 行**（22 个 `.c` + 16 个 `.h`） | 重写是一次数月工程 |
| 最大的两个文件 | `codegen.c` **8 797** · `check_top.c` **7 147** | 全仓 **40%** 在这两个文件里 |
| AST 本体 | `ast.h` 1 122 行、**20 个结构体 / 297 个成员**（`Expr` 87、`FuncDef` 53、`Stmt` 40） | AST 本体不算失控 |
| 文件级静态可变态 | **~130 个**（`parser.c` 39、`codegen.c` 29、`check_top.c` 27） | 隐藏状态的来源 |
| 分层违规 | **`codegen.c:23` include `check_internal.h`（检查器的私有头）**；反过来 `parser.c` include `plan.h` | 两条边都错 |
| 那条边到底借了什么 | 去掉这个 include ⇒ **14 个编译错误**：`isProtoType` / `isOverloadableOp` / `findOperator` / `findMethod`… | codegen 借的是**类型层谓词**，但它们住在检查器的私有头里 |

**结论**：乱的地方是**一处层缺失**，不是全仓腐烂：

1. **AST 节点同时充当语法树和分析结果表**（297 个成员里有一大批不是语法）；
2. **codegen 没有"已分析程序的只读视图"可用**，于是伸手去拿检查器的私有头；
3. **`check_top.c` 7 147 行**里不动点引擎、实例化、逐实例复核、诊断挤在一起。

⇒ **换层**：把"分析结果"变成一等公民（一个 owner 一份结果包 + 节点 ID 寻址 + 冻结语义），
`codegen` 只依赖那一层；`check_top.c` 再按阶段切分。**编译逻辑一行不改**，
改的是"数据住在哪、谁能写、谁能读"。

**为什么不是重写**：仓库最值钱的资产是**行为判据**（326 个用例 · 六道闸门 ·
977 文件 `--check-c` 全语料 · 218 例 ASan · Lean/Z3 证明）。这些资产**只认行为**——
换层可以在每一步全绿的前提下推进；重写要重建全部这些，而且是在没有它们保护的窗口里重建。

---

## 2. 主流编译器怎么做（调研结论，带出处）

> 这一节是 `AST-DECOUPLING` 的**依据**：下面每条都来自真实编译器的官方文档/源码，
> 不是风格偏好。调研仍在补全（rustc / Clang / Go / Swift / MLIR 五家）。

1. **AST 构造后不可变，是写进词条里的硬规则。**
   rustc 术语表原话："Because the AST and HIR are immutable once created, we often carry extra
   information about them in the form of hashtables, indexed by the id of a particular node."
   —— [rustc-dev-guide glossary · side tables](https://rustc-dev-guide.rust-lang.org/appendix/glossary.html#side-tables)
   **注意是 id，不是指针。**

2. **侧表用稠密整数 ID 索引，理由是性能与稳定性。**
   rustc 的 `ItemLocalId` 文档：一个 owner 内的 id 占**从 0 开始的稠密区间**，于是
   "a mapping that maps all or most nodes within an 'item-like' to something else can be
   implemented by a **Vec** instead of a tree or hash map"。
   —— [rustc_hir_id](https://doc.rust-lang.org/nightly/nightly-rustc/rustc_hir/hir_id/struct.ItemLocalId.html)

3. **一个 owner 一份结果包，而不是把字段散在节点上。**
   rustc 的 `TypeckResults` 就是一组 `ItemLocalMap<...>`（`node_types`、
   `type_dependent_defs`、`field_indices`、`adjustments`…），**每个 body owner 一个结构体**。
   —— [rustc_middle::ty::TypeckResults](https://github.com/rust-lang/rust/blob/master/compiler/rustc_middle/src/ty/typeck_results.rs)

4. **指针做键有文档记录的坑（LLVM 自己写了）。**
   "If a function is deleted in a module pass, its address is still used as the key for cached
   analyses" —— [LLVM NewPassManager](https://llvm.org/docs/NewPassManager.html)
   ⇒ IR 可能被改写时，主流做法是 **ID/下标寻址**，不是裸指针。

5. **"写一次"的语义结果可以留在节点上 —— 但仅限写一次。**
   Clang 的 `Expr` 带 `setType`/`setValueKind`，由 Sema 写一次、之后只读；
   其余（CFG、ParentMap、逐 `Decl` 分析）住在 `AnalysisDeclContext`，
   用 `DenseMap<const Decl*, unique_ptr<AnalysisDeclContext>>` 缓存。
   ⇒ **可落地的规则**："由**唯一一个阶段写一次**的语义结果可以留在节点上；
   **任何更晚算出来的、可重算的、多写者的**都必须搬走。"
   —— [Clang AnalysisDeclContext](https://clang.llvm.org/doxygen/classclang_1_1AnalysisDeclContext.html)

6. **失效要按"种类"分类，不是一个 dirty 位。**
   Swift 的 SIL 分析用 `InvalidationKind`（Nothing / Instructions / Calls / Branches / Effects…）
   且由每个分析自己判断是否真的失效。
   —— [SILAnalysis InvalidationKind](https://github.com/swiftlang/swift/blob/main/docs/SILAnalysis.md)

7. **小编译器也可以把分析槽放在节点上 —— 但要显式、单写者、有阶段归属。**
   Go 的 `ir.Node` 有 `Esc()`/`SetEsc()`（注释就写 "Storage for analysis passes"）、
   `Typecheck() uint8`（0/1/2 = 未查/查过/查中），`Name.Opt any // for use by escape or slice analysis`。
   —— [Go ir package](https://github.com/golang/go/blob/master/src/cmd/compile/internal/ir/node.go)

8. **"一层抽象一层 IR"胜过"给 AST 加注解"**（MLIR 的立身之本；Swift 的 AST→SIL 同构）。
   ⇒ extC 的对应物不是再建一套 IR，而是**建那一层"已分析程序"的视图**。

---

## 3. 目标架构（换层，不换逻辑）

```
parser ──► AST（冻结：构造后只读） ──► checker ──► Results（一等公民，ID 寻址）──► codegen
   │            │                          │                  │                      │
   │            │                          │                  │                      └─ 只 include results.h
   │            │                          │                  └─ 一个 owner 一份结果包（函数/模块）
   │            │                          └─ 唯一写者：写一次，不再"最后写赢"
   │            └─ 只有语法事实 + 解析期绑定
   └─ 语法糖在解析期脱糖（`for` 的下标/步进属于语法）
```

**三条硬规则**（可判红，不是口号）：

| # | 规则 | 判据（工具） |
|---|---|---|
| R1 | **AST 冻结**：parser 返回后，`ast.h` 的节点成员**不得再被写** | 新脚本：`check_ast_freeze.py` —— 除 parser 外的文件里出现"写 AST 成员"即红（白名单逐条 + 理由） |
| R2 | **结果只有一份**：每个 owner 的结果只在 `Results` 里，节点上不再有第二份 | 扩 `check_plan_seam.py`：节点上出现 `Results` 已有的字段即红 |
| R3 | **只读边界**：`codegen.c` 不得 include 检查器私有头；它只能 include `results.h`/`plan.h` 这类**已发布视图** | `check_layering.py`：include 图 + 允许列表（当前 `codegen.c:23` 是红的起点） |

**寻址**：从"裸指针哈希表"改成 **arena + 稠密 ID**（rustc/LLVM 的做法）。
好处不只是性能：① ID 与节点一一对应且**可校验**；② 侧表可以按 owner 整体丢弃/重算；
③ 不再有"指针做键"的悬垂/错配风险。

---

## 4. 分期（每期都可独立验收；**每一步行为不变**）

| 期 | 做什么 | 判据 | 为什么这个顺序 |
|---|---|---|---|
| **P0** | **立判据**：`check_ast_freeze.py`（R1）、`check_layering.py`（R3），进 `check.sh`；先把当前违规数**冻结成基线**（棘轮只减不增） | 两个脚本能报出"现在有多少处违规"，基线入档 | 与"判据先行"一致：**先能看见，再动手**；否则换层过程本身就是新的不可见风险 |
| **P1** | **结果层落地**：`src/results.h/.c` —— 按 owner（`FuncDef*`/`Module*`）一份结果包 + arena + 稠密 ID；先只搬**已经很干净的一族**（协程族 + arena/zone 族，即 `plan.c` 现在那批），`plan.c` 变成它的薄封装 | 行为不变（326/0 + 六道闸门 + 两道棘轮）；`plan.c` 的侧表实现下沉到 `results.c` | 先搬"已经收口、无反向写"的一族，**用最小风险验证新层的形状** |
| **P2** | **AST 冻结前置**：把 parser 之外对 AST 的写**逐条处理**（`forStep` 归 parser、身份绑定改成"写一次"或进 Results） | `check_ast_freeze.py` 基线降到 0（或每条都有书面豁免） | R1 是"彻底"的前提：不冻结，任何 Results 都会被节点上的第二份污染 |
| **P3** | **codegen 换视图**：`codegen.c` 不再 include `check_internal.h`；把那 14 处借用的谓词抽成共享只读模块（`typequery.*`）；`exprRefDepth` 之类"检查器概念"不许进 codegen | `check_layering.py` 绿；生成物逐字节不变（`tools/golden.sh` 的观察项 + 全语料 `--check-c`） | 这条边是**当前最脏的一条**，而且改动面大 ⇒ 放 P3，等 Results 就位后有干净的落点 |
| **P4** | **`Expr.func` / `forStep` 收尾**：`Expr.func` 进 Results（按（调用点，实例）已是现状，改成 ID 寻址的只读视图）；`forStep` 归 parser 侧的语法结构 | 闸门⑥ 仍绿；`check_ast_freeze.py` 与 `check_plan_seam.py` 全绿 | 这两件是 P1–P3 的**验收对象**，不是起点 |
| **P5** | **失效与阶段契约**（Swift/LLVM 那一层）：`check_top.c` 按阶段切成显式函数 + 每个 pass 声明"读什么/写什么/作废什么"，由脚本判红 | 阶段契约脚本绿；`check_top.c` 从 7 147 行降到可读的分段 | 这是"下一个 bug 概率"的下降，放在结构稳定之后 |

**每期的验收一律是**：`tests/run.sh` 两模式（发布 + `EXTC_DBG=1`，断言/兜底 0 命中）·
`check.sh` 完整模式（当前 69 节）· 六道闸门 · `plan-seam` / `tmpl-owners` / （新的）`ast-freeze` / `layering` ·
生成物观察项不出现非预期漂移。

---

## 5. 与之前做法的区别（为什么这次是"彻底"）

| 之前（X 批次） | 这次（换层） |
|---|---|
| 逐字段族搬，每族自造接口 | **先定层**：一个 owner 一份结果包，字段是包里的成员 |
| 裸指针哈希侧表 | **arena + 稠密 ID**（rustc/LLVM 的做法） |
| 反向写只"棘轮盯着" | **R1 冻结 + R3 只读边界**：写 AST 与借私有头都判红 |
| 不变量写在注释里 | 不变量=脚本判据；阶段契约（P5）显式声明 |
| 一次只解决一个症状 | 先建"能看见违规"的两条判据（P0），再动结构 |

---

## 6. 明确不做的

- **不重写编译逻辑**（`check_*` 的判定规则、`codegen` 的发射规则一行不改）。
- **不建第二套 IR**（MLIR 那套对 4 万行的编译器不划算；要的是"已分析程序的只读视图"）。
- **不动已冻结的行为资产**：326 用例、六道闸门、语料、证明。
- **不为了对称而搬**：`Stmt.forStep` 归 parser（语法），不塞进 Results。

---

## 7. P0 已落地：两条"能看见违规"的判据（2026-10-02）

**加了什么**

| 判据 | 脚本 | 基线 | 现在报什么 |
|---|---|---|---|
| **R1 · AST 冻结** | `tools/check_ast_freeze.py` | `tools/ast-freeze-known-bad.txt`（**146** 个 `文件:字段` key） | parser / plan.c / types.c 之外，对 **AST 节点专属成员**的直接写 |
| **R3 · 只读边界** | `tools/check_layering.py` | `tools/layering-known-bad.txt`（**3** 条边） | 跨阶段 include 私有头、parser 反向依赖 plan |

两条都进 `check.sh`（quick **60** 节，原 58），棘轮只许减：**修好一条就删一行**，
否则 `STALE` 判红。

**基线告诉我们的（按文件 / 按字段）**

| 文件 | key 数 | 说明 |
|---|---|---|
| `check_expr.c` / `check_top.c` | 54 / 37 | 检查器把分析状态写在 AST 上，大头在这两个文件 |
| `check_stmt.c` / `check_escape.c` / `check_lookup.c` / `check.c` | 15 / 8 / 4 / 6 | 同族 |
| `modules.c` / `codegen.c` | 9 / 9 | 模块装配与 codegen（codegen 那 9 个是它自己的描述符结构，属**判据已知的假阳性**，见下） |
| 字段侧 | `type`(6) · `refDepth`(5) · `cname`(5) · `used`(3) · `modName`(3) · `minAt`(3) · `func`(3) … | 与 X0 的"②分析缓存 / ③编译计划"两族吻合 |

**R3 的三条边**（就是 P3/P2 的施工单）：

1. `codegen.c -> check_internal.h` —— 去掉它 14 个编译错误 ⇒ 真正依赖的是
   `isProtoType` / `isOverloadableOp` / `findOperator` / `findMethod` 这几个**类型层谓词**（P3：抽成共享只读模块）；
2. `dataflow.c -> check_internal.h` —— 数据流助手写在检查器的 `Stmt` 事实上（P3 同批）；
3. `parser.c -> plan.h` —— parser 反向依赖计划层（`for` 的步进今天记在 plan 上）（P2：`forStep` 归 parser）。

**判据自己的诚实说明（写进脚本头注释，也写在这里）**

- 文本扫描**分不清基座类型**：`codegen.c` 那 9 处写的是它**自己的描述符结构**（`name`/`text`），
  不是 AST 节点。这类假阳性靠两件事收敛：① 只统计"**只在节点结构体上**声明的成员名"（已做，
  把 640 处降到 146 个 key）；② **P2 删字段时由编译器兜底**——字段删掉之后，任何残留直写都是
  **编译错误**，比任何文本判据都硬。
- 所以这条基线的定位是**方向指示器**：它的价值在于"只许减"，以及让"还剩多少"每天可见。

**这一轮的性质**：纯加工具 + 两条棘轮，**零行为改动**（tests 326/0、`check.sh quick` 60/0）。

**P0 完整验收（提交 `4a96110`）**：`./check.sh` **71 节 / 0 失败**（quick 60 / 完整 71，新增两节）；
六道老闸门全绿（checkc **977 文件** · ASan **218 例** · 差分 21 · 规模 13 · 逃逸语料 18 · 实例接线基线 0）；
tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中）；`[plan-seam] ok`；
**`[ast-freeze]` 违规 146（已知 146 / 新增 0）· `[layering]` 违规 3（已知 3 / 新增 0）**。
零行为改动。

---

## 8. P1 已落地：结果层的第一刀（`results.h/.c`：arena + 稠密节点 ID + 属主看守）

**做了什么**

| 部件 | 内容 |
|---|---|
| `src/results.h` | `NodeId`（稠密整数，0 = 无）、`ResultKind`、`NodeResults`（**一个节点一份结果槽**）、三个入口：`nodeIdOf` / `resultsOf` / `resultsById`、`resultsAs`（带属主看守） |
| `src/results.c` | 两个数组 + 一个指针→id 的哈希：`g_nodes[id]`、`g_results[id]`。id **就是下标** ⇒ 查表是数组索引，不再是"裸指针哈希"。id 在**首次需要结果时**分配（parser 一行不用改），分配后**永不变** |
| `src/plan.c` | 原来的 `PlanSlot` 结构体与 `slotFor` 指针哈希表**删除**，全部改走 `resultsAs(...)`；`plan.c` 只剩"每个字段什么意思 + 怎么记 + 两个替代表" |

**为什么 id 不是指针**（写进 `results.h` 的理由）：指针一旦被回收，按地址查表会**静默命中旧条目**——
LLVM 的 pass manager 文档自己写了这个坑（"If a function is deleted in a module pass, its address is
still used as the key for cached analyses"）；而 id 是可以存、可以比、可以校验的值。
这条与 rustc `ItemLocalId`（"dense range ... can be implemented by a `Vec` instead of a tree or
hash map"）与 Cranelift `PrimaryMap`/`SecondaryMap` 是同一个形状。

**属主看守（rustc 的 `validate_hir_id_for_typeck_results` 那一招）**：`NodeResults.kind` 由**第一个写者**
盖章，之后每次访问都比对；不一致就是**编译器 bug**（不是程序错误）⇒ `EXTC_DBG=1` 下**响亮 abort**，
发布版保持旧行为（假阳性不会破坏发布版）。32 个 plan 函数都带上了自己的 kind。

**验收**：构建零告警；tests **326/0**（发布）· **326/0**（`EXTC_DBG=1`，**属主看守 0 命中**、
断言/兜底 0 命中）；`[plan-seam] ok`；`[ast-freeze]` / `[layering]` / `[callsite]` 基线不动。
**行为不变**（存储位置与寻址方式变了，语义没变）。

**还没做、留给 P2/P3 的**：`NodeId` 还没有按 owner 分组（现在是一张全局稠密表）；
`ResultKind` 只覆盖 plan 拥有的那几族字段——②分析缓存族（`refDepth`/`origin`/六个掩码…）仍写在 AST 上，
它们进 `NodeResults` 或进检查器自己的结构，是 P2 的内容。

---

## 9. P2 第一刀：`forStep` 归语法 + 消掉 parser 反向依赖

**两件事，一个目标：让 parser 只写语法，不读计划。**

**(1) `Stmt.forStep` 是语法，判断才是结果。** 这个字段是脱糖后的 `for` 的步进语句，
`continue` 要跳到它上面（C 的 `for` 在 `continue` 出栈时会跑步进；少了它任何含 `continue`
的 `for` 都死循环，审计 P0-15），所以它由 **parser** 写在循环体上。检查器唯一要对它做的判断是
"**步进没了**"（循环被改到迭代器上时 `next()` 负责推进）—— 那是个结果，不是语法：

| 之前 | 现在 |
|---|---|
| 检查器 `inner->forStep = NULL;`（**改树**） | `planSetForStepDropped(inner)`（记在 `results` 层，`Stmt.forStep` 一个字节不动） |
| `planForStep` 直接读字段 | 先问结果层"这一步是否被撤销"，再回落到语法字段 |

**(2) `parser.c` 不再 `#include "plan.h"`。** 它借计划层只为一处：`@export` 上加协程的诊断。
那处是**解析期读检查器才写的计划值**（`planIsCoro` 由 `check_top.c` 在类型阶段设置）——
所以它**恒为 false**，是条死消息（W1-LOG 早先已记为"死消息，不是洞"）。
改法是**用语法判据**：解析期函数返回类型若直接写成 `coroutine<...>`，就是协程。

**实测（这是这刀的关键证据）**：`@export fn tick(n: i64) -> coroutine<i64>` 现在报的是
**解析器那条更准确的诊断**（`` `@export` on a coroutine ``），而不再是"C-ABI 不能返回
`coroutine<i64>`"那条泛化诊断。两者都**拒绝**同一个程序，只是消息更准。
协程不能写成类型别名（实测 `type co = coroutine<i64>` 在解析期就报错），
所以两种判据的覆盖面**恰好相同**——语法判据既更强也够用。

**棘轮结果**：`[layering]` **3 → 2**（`parser.c -> plan.h` 已删）；
`[ast-freeze]` **146 → 145**（`check_stmt.c:forStep` 已删）。两条都是"只许减"的方向。

**顺带得到的一条新判据空白（记下来，P2 后续）**：`[ast-freeze]` 只看"直接写 AST 成员"，
**看不见"经 results 层写一个语法字段"**（`planSetForStepDropped` 就属于这一类）。
要堵这个口子需要一张"哪些节点字段是**语法**、不许经分析层写"的名单 ——
在把 ②分析缓存族搬走时一起定，那时每个字段的归属都要逐条写清。

---

## 10. P2 分类：145 个 key 各归何处（含判据与分辨率边界）

**目标**：动手搬之前，先把 `[ast-freeze]` 那 145 个 `文件:字段` key **按归属分桶**。
归属由"**谁读它**"决定，不由"它在哪个结构体上"决定：

| 桶 | 判据 | 归宿 |
|---|---|---|
| **SYNTAX** | parser 写、按语法语义读 | **留在节点上**；其他阶段只读 |
| **PLAN** | **codegen 读**（经访问器） | `results` 侧表（`plan.h` 是它的读口） |
| **ANALYSIS** | 只有检查器读/写（codegen 读点 0） | 检查器自己的**逐节点分析结构**（Clang 的 `AnalysisDeclContext`、rustc 的 `TypeckResults` 就是这个形状） |
| **CODEGEN-OWN** | codegen 自己写自己读 | 归 `CG`，**不该在 AST 上** |

### 10.1 分类结果（按实测的 codegen 读点逐个核过）

**① PLAN 族 —— 基本已搬完，只剩一个真字段：**

| 字段 | 状态 |
|---|---|
| `Expr.func` | **仍在 AST**（行为已正确，见第 6.3/6.4 节）；搬它是 P4 |
| `cname` / `used` / `dynTable` / `coroFrame` / `boxedCoro` / `viewOf` … | 经 `plan.h` 访问器读（`[plan-seam]` 钉着 18 个字段）；`cname`/`used` 仍在 AST，但**读点已收口** |
| **`FuncDef.coroKind`** | **误报**：codegen 自己的 prepass 下标（检查器读点 0）⇒ 桶应是 **CODEGEN-OWN**，不是 AST 状态 |

**② 扫描的假阳性（必须点名，否则名单不能用）：**

* `g->owSites` / `g->owLocal` / `g->coroFrame` —— `CG` 自己的状态，被"按字段名跨结构体统计"误算成 `FuncDef` 的字段（**这是本批次第 6 次同类错**）；
* `.u` / `.type` / `.args` / `.call` / `.init` / `.cond` / `.recv` / `.left` / `.right` / `.text` / `.index` / `.field` / `.count` —— 这些是**节点 union 的成员**与**节点自身字段**，名字在多个结构体上重复。按名字分类时它们必然串味 ⇒ **这类格子只能逐点核，不能按名字判**。

**③ ANALYSIS 族 —— 真正要搬的主体**（codegen 按名字读点 0，已逐个 grep 确认）：

| 属主结构体 | 字段 |
|---|---|
| `Expr`（24） | `refDepth` `homeDepth` `lexicalLevel` `storedAt` `borrowed` `minAt` `reuse` `qualified` `arenaArgPending` `assocOwner` `callViaFn` `convCheck` `domNew` `dynRecvViaRef` `dynTrait` `extDom` `needOp` `obj` `operand` `parWorker` `isParWorker` `payloadType` `modPrefix` `tname` `srcName` `sym` `viewOf` |
| `FuncDef`（25） | `addrMask` `contMask` `otherMask` `homeAddrMask` `homeContMask` `addrFromLocal` `allocState` `arenaSites` `callees` `effState` `effComplete` `effUnknown` `freshCount` `needsHome` `nParamSyms` `paramSyms` `isAssoc` `isExtTarget` `lamChecked` `lamInferRet` `mayPrintState` `parTlsArena` `dynTable`(读点已收口) `owner`(语法) |
| `StructDef` | `builtinHolder` `coroOf` `lamSig` `makesPoolAny` `nfields` `fieldsComplete` |
| `Stmt` | `bindAnn` `bindCName` |
| `ImplDef` / `TraitDef` / `Module` / `UseDecl` / `GlobalDef` | `trait` `target` / `usedDyn` / `usesDyn` / `file` `unit` `path` / `ann` |

**④ 分辨率边界（诚实说明）**：上面的分桶由两份**只读扫描**支撑——`tools/check_ast_freeze.py`
（谁写）与 `tools/check_tmpl_owners.py` 的口径（谁读）。它能把"**codegen 读点 0**"这一条判得很准
（决定 PLAN vs ANALYSIS 的那一问），但**分不清 union 成员与同名字段**。
所以 ③ 里那些"多结构体同名"的格子（`type`/`u`/`args`…）**没有按名字定罪**；
真正动手时每一族都以"**删字段 ⇒ 残留直写变编译错误**"兜底（第 7 节那条）。

### 10.2 施工顺序：按**拓扑序**（不是按风险）

顺序由**依赖边**决定，边有四类：① 存储形状（谁需要谁的寻址方式）；② 同一写点（一次写要同时改两处）；
③ 只读者集合（读者固定在哪个文件 + 由哪个工具钉住）；④ 不动点（谁在实例化循环里被写）。

**先更正上一版的一个错误分桶**：`cname` 一族**不是 PLAN**。实测 `codegen.c` 里的 `cname` 全部是
**参数/描述符结构**（`p->cname`、`pp->cname`、`s->u.var.cname` 经 `u.var` 包装），**没有一处直接读节点**；
`plan.h` 里也**没有** `cname` 访问器。所以 `cname` 一族是 **ANALYSIS**（检查器在绑定期写、检查器读），
它出现在 `[ast-freeze]` 名单里是"按名字统计"的又一处假阳性。

| 序 | 内容 | 它依赖什么（前置） | 为什么必须在这个位置 |
|---|---|---|---|
| **T0** | **`results` 按 owner 分区**：`NodeId` 从"全局稠密"变成"每条 dense id = (owner, 局部序号)"；访问器带 owner 校验 | `results.as` 已有 ID 分配与属主看守，只需把 ID 空间按 owner 切 | **它是所有"按实例寻址"的前置**：`func` 的替代表、`used` 的逐实例发射、分析族的逐 owner 分组都要它。不先做，后面每族都要各造一张表 |
| **T1** | **身份族 + 发射门：`Expr.func` 与 `FuncDef.used` 一起搬** | T0 | 二者**同写点**：`resolveDeferredCall` 里 `inst->used = true` 与"重指到 inst"是一次决定；不动点又在**实例循环**里写它们 ⇒ 两者共用"按实例寻址"的存储。`used` 还有 **3 个写者**（检查器 20 处、codegen 3 处、`base.c` 1 处）—— 它是"多写者"字段（Clang/Go 的规则：多写者、可重算的**必须**离开节点） |
| **T2** | **`cname` 一族**（`Expr.cname` / `Stmt.u.var.cname` …） | 只依赖 T0 的存储（若要逐 owner 存） | **只读者已经固定**（检查器内 32 读 / 16 写，codegen 直读 0）⇒ 与后续各族的**优先级无关**，谁先只取决于 T0 是否就绪 |
| **T3** | **`FuncDef.coroKind` 归 `CG`** | **与 T0 无关**（codegen 自己的 prepass 下标） | 独立支线；唯一前置是 X3 第五步实测到的**顺序约束**（读点在 prepass 之前）⇒ 要连"prepass 提前或写成契约"一起做 |
| **T4** | **ANALYSIS 族**：先立"**检查器的逐节点分析结构**"（`Expr` 24 个 / `FuncDef` 25 个 / `StructDef` 6 个 …） | T0（按 owner 分区） | 这一族的读者全在检查器内部（codegen 直读 0），**不需要 plan 的替换机制**；它们要的是"按 owner 的逐节点槽位"，也就是 T0 的形状。注意 `refDepth`/`borrowed`/`nfields` 这类的属主**同时**是 AST 与 `Sym`（两处同名）⇒ 搬之前必须按声明结构体逐点认，正是 `[tmpl-owners]` 那条纪律 |

**一条边是跨族的**：`results` 是 `func`/`used`/分析族**共同的存储底座** ⇒ 它在拓扑序上**最先**，
即使它现在看起来"已经做完了"（P1 只做了"全局稠密"，还没做"按 owner 分区 + owner 校验"）。

**为什么不按风险**：风险排序会让"最安全的先做"，但每个族的**判据完全一样**（全套 + 棘轮只减），
所以"安全"不产生依赖；而错误顺序的代价是**返工**——例如先搬 `func`（若不先按实例分区）就要
先造一张实例表，等 T0 落地后那张表又得重做一遍。

---

## 11. T0 已落地：结果按 owner 归属（分区 + 盖章 + 观察模式）

**做了什么**

| 部件 | 内容 | 位置 |
|---|---|---|
| owner 进出 | `resultsEnterOwner` / `resultsLeaveOwner` / `resultsCurrentOwner`；检查器在两处进入函数体（`checkFunc`）与 lambda 体（`CheckLambda`）时设置 | `results.h/.c`、`check_top.c:4347`、`check_expr.c:891` |
| **盖章** | 槽位第一次被创建时，把**当前 owner** 记进 `NodeResults.owner`（模块级工作 owner 为 NULL ⇒ 不参与校验） | `results.c` |
| **语义修正（实测得出）** | owner 是**函数体的模板**，不是实例：`wrap<i32>` 与 `wrap<i64>` **共享同一个函数体**，同一批节点被两个实例先后访问。用实例当 owner 会把正常形状判成错 | `check_top.c` |
| **共享标记** | 一个"只被单个 body 访问"的槽位第一次被另一个 body 碰到时，标 `shared` 并只记一次。**共享是正常结果，不是失败**：泛型体的节点本来就被所有实例访问 | `results.c` |
| 观察模式 | `EXTC_DBG_OWNER=1` 打印每次"首次跨 body"；`EXTC_DBG=1` 下**暂不** abort（原因见下） | `results.c` |

**为什么现在只是观察、不是硬判据**：把 `!shared` 直接当断言（`EXTC_DBG=1` 下 abort）实测
**3 个泛型用例红**（`generic-calls-generic` / `generic-instance-order` / `generic-shared-callsite`），
报的都是同一处：**同一节点上的分析结果被多个实例先后写**——也就是**最后写赢**，正是 T4 要消除的耦合。
⇒ 这条判据不是"有没有 bug"的问题，而是**还没修完的量**：它在 T4 完成字段归属之后才会干净，
届时把它从观察模式升为硬判据（`EXTC_DBG=1` abort）。

**这一轮量到的**：`EXTC_DBG=1 EXTC_DBG_OWNER=1 ./tests/run.sh` 在**当前代码**上
**0 次跨 body 首次越界**（因为那 3 个用例里触发的是 `planSetForStepDropped` 之后的路径，
而它现在已改成结果层写入）；此前报出的 4 次来自 `generic-shared-callsite` 的实例循环。
两个数字都记在这里，作为 T4 的基线。

**验收**：构建零告警；tests **326/0**（发布）· **326/0**（`EXTC_DBG=1`，断言/兜底 0 命中）；
`[ast-freeze]` / `[layering]` / `[plan-seam]` / `[callsite]` 基线不动；**行为不变**。

**T0 到此的边界（写清楚，免得越权）**：id 仍是**一张全局稠密表**（owner 只做校验，不切分区）；
"按 owner 的池"（`PrimaryMap` 那种每人一张小表）留给 T4 —— 那时每个 owner 的结果真的只属于它，
才有必要把存储也分开。现在做分区没有收益，反而会让 T4 再改一次。

---

## 12. T1 已落地：身份族与发射门（`Expr.func` + `FuncDef.used` 一起搬）

**为什么必须一起搬**（拓扑序的依据，见 10.2）：二者**同写点**——不动点里"把调用点重指到 inst"
与"把 inst 标成被调用"是**一次决定**（`resolveDeferredCall`）；只搬一个，不动点就会同时写两个存储。
`used` 另有 3 个写者（检查器 20 处、codegen 1 处、`base.c` 1 处——后者是**另一个结构体**的 `used`，
块缓冲区的已用字节数，保留）。

**做了什么**

| 部件 | 内容 |
|---|---|
| 存储 | `NodeResults.func` 与 `NodeResults.used` 两个槽位字段；`PLAN_CALLEE` / `PLAN_USED` 两个位 |
| 写口 | `planSetCallee`（已有）与新的 `planSetUsed`；检查器 20 处写 `used` 全部改经 setter |
| 读口 | `planUsed`（新）与 `planCallee`（已有）；检查器 4 处、codegen **6 处**读 `used` 改经访问器 |
| 删字段 | `ast.h` 的 `Expr.func` 与 `FuncDef.used` **删除**（两段注释改为指向 plan 侧） |
| 棘轮 | `[plan-seam]` 的 `MOVED` 加 `func`/`used`（AST 上不得再出现，共 **17** 个字段） |

**迁移手法（这一轮最值得记的经验）**：

1. **先双写、后切读、最后删字段**：`planSetCallee`/`planSetUsed` 先同时写侧表与原字段，
   访问器**优先读侧表、回落原字段** ⇒ 每一步都全绿（326/0）；
2. **删字段让编译器逐条点名**：删掉后 `make` 报 **55 处** `'Expr' has no member named 'func'`，
   按 (文件,行) 逐行把 `X->func` 改成 `planCallee(X)` —— **比按名字批量替换安全得多**
   （我第一版用"标识符白名单"批量替换，误伤了 STL 里一个局部结构体的同名成员，
   靠快照整体回退；第二版用编译器点名的位置，一次到位）；
3. **同名成员要人工判**：`->func` 在 `DeferredUse`/`CallCheck`/`MethodCheck`/`RefCheck`/`OpCheck`
   上是**另一个结构体的字段**，不是 `Expr.func`；`planCallee(du->call)` 与 `du->call->func`
   是两种东西。编译器报"某结构体没有 planCallee"正是在提示这一点。

**验收**：构建零告警；tests **326/0**（发布）· **326/0**（`EXTC_DBG=1`，断言/兜底 0 命中）；
`check.sh quick` **60/0**；`[plan-seam] ok`；`[ast-freeze]` **142 → 136**（6 条 key 消失：
`check.c:func`、`check_escape.c:func`、`check_expr.c:func`/`used`、`check_stmt.c`、`base.c:used`）；
`[layering]` 不动。**行为不变**。

**T1 之后 AST 上还剩什么**：`[ast-freeze]` **136** 个 key，主体是 ②分析缓存族（T4）。
`[plan-seam]` 的 18 个"计划字段"里，只剩 `Stmt.forStep`（半语法，有意保留）。

---

## 13. T2 已落地：绑定名（`Expr.u.ident.cname` + `Stmt.u.var.cname`），并划出一条**按值类型不可入表**的边界

**这一刀的对象是"生成的 C 名"** —— 检查器在把名字绑定到声明（`Sym`）时决定，codegen 打印它。
分类时它被算作 ANALYSIS，实测后确认它其实是三件不同的事：

| 落点 | 位置 | 处理 |
|---|---|---|
| `Expr.u.ident.cname` | `ast.h`（union 成员） | **搬进结果层**（`planCName` / `planSetCName`，`PLAN_CNAME` 位） |
| `Stmt.u.var.cname` | `ast.h`（union 成员） | 同上 |
| `Param.cname` | `ast.h`（**独立字段**） | **有意保留在 AST 上**（理由见下） |
| `Sym.cname` | `check_internal.h` | 本来就不在 AST 上 |

**划出的边界（本轮最重要的实测结论）**：`Param` 是**按值存放**的类型 —— 协程帧的参数表
（`vecPush(&f->coroFrame)`）会**复制**它。而结果层是**按节点指针寻址**的：拷贝出来的 `Param`
取不到原来的槽位。实测：把 `Param.cname` 也放进侧表 ⇒ **每个协程入口函数都丢掉参数名**
（生成物里 `int64_t n` 变成 `int64_t`），`tests/coro` 从 29/0 掉到 8/21。
⇒ **按值传递的结构体不能进"按指针寻址"的侧表**；这不是实现细节，是接口的适用条件，
已写进 `plan.h`/`plan.c`/`ast.h` 三处注释。

**另一个语义坑（同一次实测）**：`planCName` 一开始把**空字符串**当"有名字"返回，而调用方的
惯用写法是 `p->cname ? p->cname : p->name`（检查器确实会写 `""` 表示"没有自己的名字"）。
返回 `""` 会**赢过那个回落**，于是发出没有标识符的 C 参数。修法：**空名视同未设置（返回 NULL）**，
与字段时代的语义一致。

**改动**：`results.h` 加槽位；`plan.c` 加位 + 访问器/写入器（带 `ResultKind` 供属主看守）；
`check_expr.c` 3 处写、`check_stmt.c` 2 处写改经 `planSetCName`；31 处读（check_expr 2 ·
check_stmt 3 · check_top 12 · codegen 12 · dataflow 2）改经 `planCName`；`ast.h` 删两个 union 成员。
`dataflow.c` 补 `#include "plan.h"`（它读绑定名，属检查器侧）。

**判据/验收**：构建零告警；tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中）；
**`tests/coro` 29/0**（这一刀最容易伤到的地方，因为协程帧按值存参数）；
`[ast-freeze]` **136 → 135**；`[layering]`/`[plan-seam]`/`[callsite]`/`[tmpl-owners]` 不动。

**过程记录（一次回退与一次重放）**：第一次尝试把 `Param` 也一起搬，发现协程崩了以后，
我用快照回滚时**取早了一个备份点**，把 `Expr`/`Stmt` 那半也一起回退了。重放时把设计改正
（Param 留字段）后一次通过 —— 教训：**回滚点要按"这次改动的起点"取，不是按"最近一次快照"取**；
两次快照（`/tmp/rev/t2_base` 与 `/tmp/rev/t2_wip`）的差别正是这个。

---

## 14. T4 第一族已落地：`StructDef` 的 4 个字段 + 结果层分出的"分析侧"

**分类的修正**：`StructDef` 那一族按"名字"算是 5 个，逐个看写者后是 **4 个**：

| 字段 | 谁写 | 结论 |
|---|---|---|
| `builtinHolder` | 检查器（2 处） | **PLAN**（codegen 读 1 处） |
| `coroOf` | 检查器（1 处） | **PLAN**（codegen 读 4 处） |
| `lamSig` | 检查器（1 处） | **ANALYSIS**（codegen 读点 0） |
| `makesPoolAny` | 检查器（1 处） | **ANALYSIS**（codegen 读点 0） |
| ~~`srcName`~~ | **parser**（1 处）+ 模块层（2 处） | **SYNTAX**：它是"声明写的名字"，不是分析结果 ⇒ **不动**（记在这里，免得下次又按名字算进 ANALYSIS） |

**结果层分出的两个受众（本轮的结构性改动）**：`NodeResults` 里现在有一块 `an`（analysis），
装**只有检查器读写**的字段，和 plan 字段同槽但**不同受众**。两者的界线由 `plan.c` 里的
`ANALYSIS_FIELDS`（具名清单）钉住：**一个字段一旦被 codegen 读，就必须从这张清单里移出、
并配一个 `planXxx` 访问器**。这样"两个受众"不会悄悄合并 —— 这正是上一轮 `[ast-freeze]`
暴露出来的问题（分析状态与计划状态在 AST 上混住）。

**为什么第一族选它**：它是拓扑序上**依赖最少**的一族（按值传递风险没有、跨实例风险没有、
`used`/`func` 那种不动点耦合没有），正好用来验证"分析侧"这个形状；形状一旦站住，
`Expr`（26 个）与 `FuncDef`（26 个）两大族就按同一套路推。

**改动**：`results.h` 加 4 个槽位 + `an` 块；`plan.c` 加 4 个 setter/访问器
（`planBuiltinHolder`/`planSetBuiltinHolder`、`planCoroOf`/`planSetCoroOf`、`anLamSig`/`anSetLamSig`、
`anMakesPoolAny`/`anSetMakesPoolAny`）与 `ANALYSIS_FIELDS`；5 处写、15 处读改经访问器
（check.c 5 · check_expr 1 · check_stmt 1 · check_top 3 · codegen 5 · main.c 1）；
`ast.h` 删 4 个字段并改掉两处提到它们的注释；`main.c` 补 `#include "plan.h"`。

**棘轮**：`[ast-freeze]` **135 → 131**；`[layering]`/`[plan-seam]`/`[callsite]`/`[tmpl-owners]` 不动。

**验收**：构建零告警；tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中）；
`tests/coro` 29/0；`check.sh quick` 60/0。

## 15. T4 第二族已落地：`impl` / `trait` / `module` 的 4 个字段

同一套路（先加存储与 API → 改读写 → 删字段 → 棘轮），第二个家族：

| 字段 | 属主 | 写者 | codegen 读 | 桶 |
|---|---|---|---|---|
| `target` | `ImplDef` | 检查器（解析类型名时 1 处） | 4 | **PLAN** |
| `trait` | `ImplDef` | 检查器（1 处） | 2 | **PLAN** |
| `usedDyn` | `TraitDef` | 检查器（`dyn` 出现那一刻，1 处） | 1 | **PLAN** |
| `usesDyn` | `Module` | 检查器（同上，1 处） | 2 | **PLAN** |

**同名陷阱又出现两次**（这轮的直接证据）：`->target` 在 `StoreSite`/`RefCheck`（检查器自己的记录）
上也有，`->trait` 只在 `ImplDef` 上——所以批量替换时**必须限定基座**（`im->target`），
不能按字段名扫。这也解释了为什么 `[ast-freeze]` 那 131 → 128 只降了 3 而不是 4：
`TraitDef.usedDyn` 与 `Module.usesDyn` 各只有 1 处写，而 `target` 的另外几处写属于别的结构体。

**改动**：`results.h` 加 4 个槽位；`plan.c` 加 4 组 setter/访问器
（`planImplTrait`/`planSetImplTrait`、`planImplTarget`/`planSetImplTarget`、
`planUsedDyn`/`planSetUsedDyn`、`planUsesDyn`/`planSetUsesDyn`）；
4 处写、27 处读改经访问器（check_expr 3 · check_top 15 · codegen 9）；
`ast.h` 删 4 个字段（其中两处是多行注释的尾巴，删字段时留下**悬空注释**、当场语法错——
又是一次"删字段要连注释一起看"）。

**棘轮**：`[ast-freeze]` **131 → 128**；其余四条不动。

**验收**：构建零告警；tests **326/0**（发布与 `EXTC_DBG=1`）；`tests/coro` 29/0。
