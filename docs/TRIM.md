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

### 追到根上：`poolSlice`/`poolResize` 的结果深度没有取自池所在的地方（2026-11-05）

用 `EXTC_DBG_FS=1` 把同一程序（`P2-7`）在 arena 底与板块底各编一遍，字段来源表只差两行：

```
arena  [fs-promote] head kind=23 depth=0 minReq=0 at=0
plate  [fs-promote] head kind=23 depth=1 minReq=1 at=0
```

`head` 的来源是 `v.get(k)` 取出来的元素。arena 底这一路是 **d2=0**（元素被要求活到 home），
所以 `new node` 的站点被提升到 home；板块底是 **d2=1**，于是站点留在本帧，函数退出后悬垂。
而 `head` 把 `want` 收紧到 0 之后，`promoteInto2` 要提升的表达式是 `poolSliceRaw`/`poolResizeRaw`
的 **调用**（字段槽 `vals` 的来源，`srckind=16`），不是元素自己的站点 —— 调用结果不可提升，
于是 `ok=false`，要求落空。

所以阻塞点不是"字段表断了"，而是**两条更基础的规则**：

1. `poolSlice<T>`/`poolResize<T>` 的结果，深度应当取自**池所在的 `@place`**（今天它们像
   "借自参数"的普通视图，落成了调用点的块级深度）；
2. 往容器里写元素时，元素的要求是"活得和容器的存储一样久"，而不是"和容器这个绑定一样久"。

两条合起来正是 `docs/TRIM.md` 里记的 P2 前置（"返回视图的深度取自接收者"）。做完这两条，
`P2-7` 与 `varArray` 搬板块应该一起转绿。

## `varArray` 搬板块：真正的阻塞点是"容器里装的东西从哪来"（2026-11-05）

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

相邻形状也挡住了：视图作为**实参**传进"可能保存它"的函数，现在按效果摘要的 `Addr`/`Cont`/`Other`
位在调用点拒绝，并建议 `snapshot()`（`view_arg_stored.extx` 是反例、`view_arg_snapshot.extx` 是正例）。
跨函数增长那一半依旧靠 `borrow()` 守卫，不是自动的。

### 试过：把"容器被交出时板生在调用者 place"当作开关（2026-11-05，证伪）

想法：`markCallHomeIfEscaping` 已经在"值被交出"时把 arena 站点提升到 home；照抄一句，让建池的调用在
同一个分支里把 zone 级降到 `ZONE_HOME`，容器里的元素就会被要求活到 home。

`P2-7` 的 `cur.next` 深度仍然是 1；产物里节点由 `build` 的帧 region 分配、退出时 `regionDrop`。原因是 `P2-7` 里**容器本身没有交出**——活下来的是容器里的元素（`head` 被返回）。所以
要提升的是"写进容器元素的 `new` 站点"，而不是容器的 place；这条链在 arena 底靠 `promoteFieldsAt`
走通（`[fs-promote] head depth=0`），在板块底走不通（`depth=1`），差别在元素槽的来源种类：
arena 是 `EX_NEW`（可提升），板块是 `EX_GENCALL`（`poolResizeRaw` 的调用结果，不可提升）。

结论：修点不在 `setCallZoneArg`/`markCallHomeIfEscaping`，而在"元素槽的来源不是分配站点时，
`promoteFieldsAt` 如何追到真正的分配站点"。这条要动 `fieldDepthEntry` 的来源记录，属 P1 的
`slice` 纯借用同一片；本轮不动。


## 已知的洞：视图作为实参、由被调方写进元素（2026-11-05）

<!-- fragment -->
```extc
fn stash(a: mut slice<slice<i64>>, s: slice<i64>) { a[0] = s }
stash(a[..], w.view())     /* 视图进了别人的元素；之后 w 增长再读 a[0][0] 是 UAF */
```

调用点规则只在该参数的效果摘要带"可能保存"位时触发；这里被存下去的是 `a` 的元素，摘要把"保存"
记在 `a` 上，`s` 没有被标。要挡它需要按实参形状推"被调方可能把第 i 个视图参数放进某个容器"，
比字段/元素两条规则大一圈。暂记为洞，判据（ASan 干净）等实现后补。

## 已解决：把 `varArray` 也搬到板块上

想让 `varArray<T>` 的存储从 arena 换成板块（`@place` + `poolSliceRaw`/`poolResizeRaw`），
这样 8e6 个 `i64` 的峰值从 129,320 KB 掉到 64,252 KB（与 `vector` 同档），`release()` 也能当场
还板。功能一直是对的，卡的是两处闸门：`tests/coro/statemachine.extc` 的
"argument 1 of `step` points into a deeper scope (depth 1) than the arena this call may store
it in (depth 0)"，以及 `tools/gate_escape.py` 的逃逸语料变红。

修法落在 `src/check/check_escape.c` 的 `promoteInto2`：**建池分支**把池的 zone 级下调之后
`return true` 提前返回，跳过了"把接收者容器的字段来源一起提升"那趟走查。arena 底的容器没有
`@place`，根本不进这条分支，所以只有板块底漏：`v.push(p)` 存进去的 `p` 只被要求活到块级，而
`return head` 已经把链头交出去，返回即悬垂。

抽成 `promoteReceiverFields` 之后，两条义务在同一个分支里执行："池生在哪层"（zone 级）与
"池里元素活多久"（字段来源表）。另外 `borrowOwner` 的白名单补上 `asSlice`/`asMutSlice`，
与 `valueHoldsMovingView` 一致。

判据：`tools/gate_escape.py` 全绿、`tests/run.sh` 377/0、`tests/coro` 35/0、`tests/pool` 与
`tests/stl` 全过（金值不用改）、`check.sh smoke`/`docs` 绿；内存 8e6 个 `i64`
`peak=64056 KB`（对照 arena 版 129,720 KB）。

**顺带确认过的一条否证**：单独删 `calleeMakesPool` 末尾
`anIsAssoc(f) && anMakesPoolAny(owner)` 那行捷径，`gate_escape` 仍报 2 条 UAF ⇒ 它与本卡点无关，
保留。`promoteInto2` 的 `needsHome` 分支仍不走这趟走查：那是"callee 自己的新分配"，其内容已在
callee 体内按参数深度查过，与"结果指向接收者存储"不同类。

## 计数层：先不删（2026-11-05 决定）

删掉"计数"这一档在本轮被推迟。它不是单一开关：`EXTC_RC_SWEEPFREE=1` 的"事件档"（只扫有事件的区）
与编译器对"能证明无环"的程序自动开计数 + 不扫（`unitAllAcyclic`）都压在它上面，`tests/reclaim-acyclic`
的整套判据与论文里 acyclic 那条叙述也一样。

将来要删时，路是清楚的：计数档在 `src/back/reclaim.c` 里是**发射器字符串**，`rtCfgIf()` 在编译期按
`EXTC_RC_NOCOUNT` 丢掉分支，所以删掉分支后生成 C 应当逐字节相同。按这个判据验证过六个形状
（promote/sizeratio/chase/cycleratio/condsave/mutate）全部 `cmp` 相同；净删 103 行、15 个块。
两次失败的尝试留下教训：按行删会踩"一行里有多个字符串字面量"，按字面量删会误删嵌套的
`#if EXTC_RC_FOREIGN_GUARD`。

## 返回视图的深度：今天的边界（2026-11-05 诊断）

`FuncDef` 只有 `Type *ret`，没有函数级"返回值借自谁"的记录；返回处的深度由 `targetDepth()` 按语法重算，
再被 `poolCallDepth()` 抬到本函数块层级，随后又被 `promoteInto2()` 当成"分配点"改写。调用点不读 callee
的任何记录，而是按自己的实参重算同一公式。

后果是精度损失：一个"返回自己参数存储里的视图"的包装函数，其结果在调用点被当成新分配，于是要求它活得
比实际需要更久 —— `tests/genmatrix` 里那类形状因此被保守拒绝。修法是给 `FuncDef` 记 `retBorrowArg` /
`retBorrowField`，让调用点按 `max(slotDepth(arg), 该字段的存储深度)` 取数，并把 `promoteInto2` 的要求
下沉到存储的 `src`。这条改动是"删池特权"的前置。

## 已知的洞：容器的拷贝让两个名字共享一块板，视图失效只认其中一条路径

容器（`vector`/`varArray`/`string`/映射）是**可拷贝**的值，拷贝之后两个名字指向同一块板 —— 于是
"另一个名字增长会搬走板子"这件事，编译器只在**本函数内按名字**登记借出时能看见。放进结构体字段
再借出来，登记就断掉了：

<!-- fragment -->
```extc
var v = vector<i64>::new()
v.push(1)
var bx: box = { v: v }          /* 拷贝，两个名字一块板 */
var s: slice<i64> = bx.v.view()
grow(ref v)                      /* 板子被搬走 */
print(s[0])                      /* 悬垂：ASan heap-use-after-free */
```

这个形状在 `retBorrowArg` 之前就被接受（与"返回值深度"那条改动无关），复现见
`/tmp/hk/share.extc`。根治的办法是让容器不可拷贝（`@noCopy` + 转移），这正是"容器合并"要做的；
在那之前，`-Wowned-view` 与 `@sharesStorage` 的警告是唯一的提醒。值层面的冻结位（`borrow()`）能挡住
"同一个名字增长"，挡不住"另一个名字增长"。
