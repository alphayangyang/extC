# 体量收敛：把"同一件事的几种答案"删到一种

这份清单是[核心](CORE.md)预算表的执行版。每一项都是实测的行数、判据数、语料里用了几次，以及
"能不能用核心表达"。删之前先看**依赖**：有些项必须等 `slice` 收成纯借用（P1）才动，顺序反了会
把库改两遍。

口径：行数用 `wc -l`；判据数按 `tests/<套件>/run.sh` 里的用例条目；用例数用
`grep -rE '<词>' stdlib tests examples bench` 的命中行数（同一行多次出现算一次）。数字是
2026-11 的实测，删完要更新。

## 一、候选与结论

| 项 | 现状 | 判据 | 语料用量 | 结论 |
|---|---|---|---|---|
| **计数层**（`src/back/reclaim.c` 的 counting 模式） | 2,988 行里的计数分支 | `tests/reclaim-perf/count*` | 0（不默认开） | **删**：计数税实测 1.4–16×，靠 `EXTC_RC_NOCOUNT=0` 才开；扫描层保留 |
| **`varArray` + `vector`** | `prelude.extc` 352 行 + `data/vector.extc` 196 行 | 两套都全 | 112 / 83 | **合并**：等 `slice` 收成纯借用后，`varArray` 变成 `buf<T>` 之上的别名（增长即归还旧块） |
| **三个映射**：`hashMap` / `map` / `linmap` | 540 / 939 / 227 行 | 各一套 | 149 / 28 / 15 | **先留三个**：三者要求的键能力不同（`hashMapI64` 要 `i64` 键、`map` 要 `K: <`、`linmap` 只要 `K: ==`）。删 `linmap` 的前提是"键类型能提供 `hash()`"或"`map` 接受只有 `==` 的键"，两者都还没有 ⇒ 降级为文档中的"只有 `==` 键时的退路" |
| **两套 JSON**：`std::json` 的 DOM + `any` 的 `extc_any_json` | 1,137 行 + 运行期文本 | 各一套 | 40 | **留一条路**：`std::reflect::json(ref any)` 打任意值；DOM 只用于"解析后反复查询"的场景 |
| **两套输出**：`io::cout` 链 + `print`/`println` 内建 | 内建 2 个 + `io` 的缓冲机制 | 两套都测 | 1,379（cout） | **留内建**：`print`/`println` 是 90% 的用法；`io::cout` 保留但文档上作为"需要控制格式/分块写"的入口 |
| **两种并行**：`par i in ..` + `parallel::run` | `par` 是语法、`run` 是内建 | 各一套 | 42 / 10 | **留 `par`**：`parallel::run` 只在需要"手工分块传偏移"时用；等 `par` 支持二维折叠后删 |
| **`stl::pool<T>`** | `data/pool.extc` 493 行 | 12 个用例 | 0（公开面用量） | **已降级**：文档标为"槽回收工具"，不是通用容器；下一步把它变成 `@place` 的一个用例 |
| **`linmap` 单独模块** | 227 行 | 15 处 | 15 | 与上表同一项，删模块、实现并入 `map` |

## 二、顺序（每步都要闸门绿）

1. **等 P1 的 `slice` 纯借用落地**。它决定 `varArray`/`vector` 合并的写法（不然要改两遍）。
2. **删计数层**：先删运行时分支与 `EXTC_RC_NOCOUNT` 相关产物，再把 `tests/reclaim-perf` 里
   依赖计数的用例改成"扫描臂"判据。注意：论文里"counting on"是一个消融臂，删掉要同步改论文
   的消融表与 limitation（这一步要单独提交，附论文改动）。
3. **合并容器**：`varArray` 变成 `buf<T>` 之上的别名 → 跑 `tests/stl`、`bench/app`、
   `bench/memstudy`（峰值数字会变，`RESULTS.md` 要更新）。
4. **合并映射**：`linmap` 的实现搬进 `map`，`use std::data::linmap` 变成 `map` 的别名；判据
   保留原来那 15 处用法。
5. **合并 JSON**：`std::json` 的 DOM 与 `reflect::json` 各留一个入口，删掉重复的转义实现
   （两份转义是错误来源）。
6. **合并输出与并行**：`parallel::run` 标 `@private` 或删除；`io::cout` 文档降级。
7. **清公开面**：每步之后重跑 `python3 tools/manual_surface.py`，把新增的 `@private` 记进
   `docs/usage/surface.md`，公开成员只许降。

## 三、每步的验收

- `tests/run.sh` / `smoke` / `docs` / `nobench` 全绿，`ast-freeze` 不涨。
- 相关套件用例数**不降**（合并的项要把两边的判据都保留到一个套件里）。
- 性能不回归：合并容器/映射后跑 `bench/app`（log/route/session）与 `bench/crazy`，比值在
  噪声内；峰值内存**必须**改善（`varArray` 8M 元素 129 MB → 约 64 MB）。
- 文档同步：`docs/usage/stdlib.md`、`docs/usage/surface.md`、changelog（一句症状 + 一句结构
  + 一句判据）。

## 四、不做的事

- 不为了行数删功能：删的是"第二次答案"，不是能力。
- 不动 `coroutine` 那条按名字派发的路径（4 处）——它是"协议"不是容器，等并发回收器定了再说。
- 不删 `any`/`dyn` 任何一方：它们是"结构 vs 名义"两种视图，共用 16 字节载体，不是重复答案。

### 已试过、目前不可行：把 `varArray` 也搬到板块上

想把 `varArray<T>` 的存储从 arena 换成板块（`@place` + `poolSliceRaw`/`poolResizeRaw`），
这样 8e6 个 `i64` 的峰值从 129,320 KB 掉到 64,252 KB（与 `vector` 同档，对照手写 `new`+翻倍
是 129,724 KB），`release()` 也能当场还板。**功能上全部通过**（正例、`release()` 归还、
反例里的越界 trap 都对），但两处闸门红：

- `tests/coro/statemachine.extc`：`step(ref f, ref st, …)` 报 "argument 1 of `step` points into a
  deeper scope (depth 1) than the arena this call may store it in (depth 0)"；
- `tools/gate_escape.py`：逃逸健全性语料变红。

根因（已用 `EXTC_DBG_REFARGS=1` 定位）：让容器成为"会建池的类型"之后，`tests/coro/statemachine.extc`
里 `handle()` 的效果摘要从 `addr=0x0 cont=0x0` 变成 `addr=0x1 cont=0x1`（它把视图存进了一个
板块底的容器），`step()` 继承这两个位；调用点随即检查"被存进去的值必须活得不比目的地短"，而
目的地的深度由 `targetDepth` 回答 —— **板块被建模成 home（深度 0）**，于是要求 `depth 0`，
`ref f`（深度 1）被拒。

也就是说卡点不是一个 if，而是**板块的深度模型**：今天的规则是"池生在 home"，而 `@place` 的语义是
"存储住在它所在的那个地方"。要让 `varArray` 搬上板块，得先让 `targetDepth`/`calleeCreatesPool`
这条链按"池出生的那个 place"给深度，而不是一律 home。这也是 P2 那条逃逸规则的同一件事：
`string`/`vector` 的板块现在借的是 home 的深度 0（所以它们当目的地时永远是"活得最久"），
更精确的模型是让板块随出生点。要重做这件事，得先把"容器的板块归谁、调用点的 home arena 取哪一层"
从"类型是否建池"里拆出来 —— 那正是 P2 剩下的那条逃逸规则（返回视图的深度取自接收者）。

数据留档：峰值对照见本文件开头的 `buf` 一行；行为差异（增长后旧视图从"静默快照"变成悬垂）
见 `docs/usage/language.md` §8.2 的 `varArray` 段（保持 arena 的说法，因为代码仍是 arena）。

下一轮要动的三个点（按数据流，不是按文件）：

1. **站点记账**：`src/check/check_top.c` 的 `noteArenaSite`（约 1630–1670）里 `if (depth < 2) return;`
   让"帧级（depth 1）的板块站点"保持效果摘要里的 `homeDepth = 0` —— 这就是"板块活得更久"的来源。
2. **收尾落位**：同一文件约 9297 行的 placement 把它翻成 `ARENA_HOME` 或具体块级；判据
   `anMinAt(site)`（0 ⇒ home）。
3. **消费方**：`checkCallRefArgs`（约 1300–1430）用 `homeDepth` 算 `h`，`h == 0` 就是
   "被存进去的值必须活到进程结束"。

判据（红→绿）：`tests/coro/statemachine.extc` 编过、`tools/gate_escape.py` 0 失败、
`tests/pool` 的金值（`rt_nest` 的板块字节数、`rt_nest_promote` 的 depth 数字）同步更新、
`tests/run.sh` 377/0。
