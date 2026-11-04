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

### `varArray` 搬板块：真正的阻塞点是"容器里装的东西从哪来"（2026-11-05）

extern 默认掩码修好之后可以把 `varArray` 的存储换成板块，功能与内存都对（8e6 个 `i64`
峰值 129,320 KB → 64,252 KB），`tests/run.sh` 也是 377/0。但 `tools/gate_escape.py` 的
`P2-7_container_before_uaf` 变红，形态是"容器装 `mut ref node`、函数返回链头"：

- arena 底：节点的分配点被提升到 home（生成 C 里 `extc_rc_regionOf(&(*__extc_home), -1)`），
  返回的引用活得过函数；
- 板块底：分配点留在本帧（`extc_rc_regionOf(&(__extc_a[1]), 1)`），函数退出 `regionDrop` 之后
  返回的链头悬垂。

机制上，"容器里装的值会被 `promoteFieldsAt` 顺着容器的字段表提升"这条链是断的：字段表记的是
"这个字段的值来自哪里"，而板块底 `grow` 里出现了 `self.vals = poolResizeRaw(self.pid, self.vals, …)`
—— `vals` 的字段来源是 `poolResizeRaw` 的返回值，于是 `push` 存进去的元素来源不在字段表里，
`v.get(k)` 提升不到它们。

所以这不是"再补一条判据"能收的：要么让字段表理解"增长调用不改变元素来源"，要么给"装引用的
容器"保留 arena 存储（纯数据元素才用板块）。两条都要先在 `src/check/check_escape.c` 的
`promoteFieldsAt` / `classifyStoredValue` 里把字段来源链讲清楚，再动 prelude。

## 视图经字段跨函数逃逸：已挡住（2026-11-05）

`b.s = b.v.view()` 之后在别的函数里 `b.v.push(...)`，再读 `b.s[0]` 是 ASan 抓得到的
heap-use-after-free 而编译器不报错。现在"把会移动存储的视图存进字段"这一步就被拒绝
（`field_store_view.extx` 钉住），存快照仍然可以（`field_store_snapshot.extx`）。

仍是洞的相邻形状：把视图作为**实参**传进一个会保存它的函数（编译器不读函数体，判断不了
"这个函数留不留"）；跨函数增长那一半依旧靠 `borrow()` 守卫，不是自动的。

## 已试过、目前不可行：把 `varArray` 也搬到板块上

想把 `varArray<T>` 的存储从 arena 换成板块（`@place` + `poolSliceRaw`/`poolResizeRaw`），
这样 8e6 个 `i64` 的峰值从 129,320 KB 掉到 64,252 KB（与 `vector` 同档，对照手写 `new`+翻倍
是 129,724 KB），`release()` 也能当场还板。**功能上全部通过**（正例、`release()` 归还、
反例里的越界 trap 都对），但两处闸门红：

- `tests/coro/statemachine.extc`：`step(ref f, ref st, …)` 报 "argument 1 of `step` points into a
  deeper scope (depth 1) than the arena this call may store it in (depth 0)"；
- `tools/gate_escape.py`：逃逸健全性语料变红。

**根因**（`EXTC_DBG_REFARGS=1` 显示）让容器成为"会建池的类型"
之后，`handle()` 从 `addr=0x0 cont=0x0` 变成 `addr=0x1 cont=0x1`，`step()` 继承这两位，
于是调用点开始跑 `checkCallRefArgs` 的目的地推断。这个推断做的是
"取所有**带引用的** `ref` 实参里最浅的一个当目的地"，在 `step(ref f, ref st, input[..], out)` 里
唯一命中 `typeContainsRef` 的实参是 **`out`**（`mut ref string`），而 `out` 是调用者 `runFeed`
的形参 ⇒ `slotDepth(out) == 0`（"活得最久"）⇒ `h = 0` ⇒ 深度 1 的 `f` 被拒。
用 `EXTC_DBG_DEST=1` 打的 `[dest] step arg3 out d2=0` 就是这一步。

所以卡点**不是**"板块被建模成 home"（先前那条推断不成立：`varArray` 在 arena 版本里也有
`mut slice` 字段，`typeContainsRef` 一样为真；差别只在于板块版把"提升"变成了"存进板块"，
于是效果位被置上、目的地推断才被启用）。真正的问题是**目的地是按"哪个实参带引用"猜的，
而不是按"这个函数到底往哪个参数里存"**：`step` 只往 `st` 里的容器存，`out` 只是被 `append`
拷了字节，但两者都成了目的地，取最小就塌成 0。

修法（下一步，已明确）：把效果摘要补一位 **每参数的目的地掩码** —— 在 `classifyStoredValue`
的"存进自己的参数"分支里记下目标参数下标，再在调用点把这些位按实参根换成调用者的参数位；
只有 `destMask` 非空且映射完整时才用它算 `h`，否则退回今天这条保守的启发式。
不能直接把 callee 的 `destMask` 当 caller 的参数位并集（那会给出错误的、更宽松的 `h`）。

判据（红→绿）：`tests/coro/statemachine.extc` 编过、`tools/gate_escape.py` 0 失败、
`tests/pool` 的金值（`rt_nest` 的板块字节数、`rt_nest_promote` 的 depth 数字）同步更新、
`tests/run.sh` 377/0。
