<!-- 本页原来是 docs/MANUAL.md 的「10. 已定案、但还没实现」一节（原 §10.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 9. 错误信息](15-errors.md) · [11. 还没定的 →](17-undecided.md)

---

# 10. 已定案、但还没实现

> **已完成（截至 2026-09-18）**：默认零初始化、方法进 struct 体内、`type` 枚举、
> `ref` 表达式与调用点显式、泛型单态化、prelude 机制。
> **`slice<T>` 索引与字符串库**、**固定数组 `[N]T`**、**切片视图 + 可写性**、
> **`option`/`result`/`?`**、**`::` 关联函数**、**位运算**、
> **引用语义（`mut ref` + 视图可写性）** 也都在跑了。
> **`*p` 显式解引用**、**`?T` 语法糖**、**可空引用 `?ref T` + `null` + 非空收窄**
> （见 §7.6）**2026-09-20 完成**
> ⇒ **链表/树/搜索函数今天就能写**（跨函数接线也已经通了：`new` + 逃逸提升）。
>
> **这张表 2026-09-22 逐行复核过**（= 已经在跑）—— 权威的"还剩什么"看 `PLAN.md`
>
> **2026-09-26 又复核一次**：模块系统与 `@recursive` 两行是旧的（前者**已落地** 后者**已推后**），
> 已就地改正。**以后要看"还剩什么"，只认 `PLAN.md`** —— 本表只作特性清单用，会滞后

| 特性 | 定案内容 | 计划 |
|---|---|---|
| ~~**逃逸检查**~~ | 词法深度 `depth(r) ≥ depth(v)` —— **这是 extC 的命**，**已实现**（返回 / 局部 / 赋值 / **借来的值不进深度 0**，见 §0.5） | ~~下一步~~ **2026-09-18 完成** |
| ~~**arena 按块细化**~~ | 分配绑**词法作用域**（每个 `{}` 一只）：`extc_arena __extc_a[DEPTH] = {0}` + 进块 reset / 出块 release，`break`/`continue`/`return`/`?` 各释放该退的层。`alloc<T>(n)` 的引用深度 = **当前块深度** | **2026-09-20 完成**（`tests/arena/` 验收：150MB 上限下循环里分配 300×1MB 跑得完）|
| ~~**块级逃逸提升**~~ | `new` 的东西被**存进更外层**的地方 ⇒ 把那只 arena **提升**到那一层（**不拒绝**）：`arenaLevel = min(当前块, 各目的地)`。代价 = 这些分配活到那一层结束（最坏 = 函数级、自动清理的堆）；**没存出去的东西照样按块回收** | **2026-09-22 完成**（定案 63；`tests/asan/` 常设验收 + `examples/store-promotion.extc`）|
| ~~**借用规则挪到调用点**~~ | 被调者**只发布约束**（效果摘要 `Addr`/`Cont`），**调用点代入求解** ⇒ `varArray<slice<u8>>`（字符串表）· `varArray<varArray<T>>`（邻接表）· 通用 `stash(dest,v)` 全通 摘要不完整/带环 ⇒ 调用点**保守拒**（fail loudly）| **2026-09-22 完成**（定案 67 / PLAN #43；`ARENA-FORMAL` §9 + §9.5 落地实录；正例 `examples/container-of-view.extc` · `container-nested.extc`，反例 `tests/errors/stash_view_from_deeper.extc`）|
| ~~**`new` 的写法**~~ | `new node` / `new i32[1000]` / `new [4]i32`（清零），分配进**当前块**的 arena，存进更外层的地方会**提升**（定案 63）| **2026-09-20 完成**（A1）+ 提升 2026-09-22 |
| **`region` 显式命名**（**逃逸提升 A3 已完成**）| 跨函数接线（`fn build() -> mut ref node` / 出参 `mut ref`）**已经在跑**；**只剩"显式给一个分配命名区域"**（A4，主人说"不急"）| A4 待定 |  **2026-09-26 主人拍板：死了，不需要**（逃逸提升 A3 已覆盖实际需要）
| ~~**动态数组 `varArray<T>`**~~ | prelude 里用 extC 写：`{ buf: mut slice<T>, len: i64, home: ref arena }` + `new`/`push`/`get`(→`option<T>`)/`len`。**名字定案**：`array<T>` 会被误读成定长（主人原话「wc不要叫array啊，md我以为是定长的」），`vector` 太抽象 ⇒ **`varArray`** | arena 之后 |
| ~~**算术 UB 三处**~~ | 除零 trap 带位置、移位超宽取模（溢出已用 `-fwrapv` 兜住） | **已做**（`tests/traps/`：`div_zero` / `shift_too_big` / `index_out_of_range` / 两个转换 trap）|
| ~~**全局常量 / 全局变量**~~ | 全局 = 深度 0 的 arena；`static` 关键字因此消失。**定长全局不需要分配** | **已完成**（见 `examples/globals.extc`） |
| **输入（`reader` / `argv`）** | 全通了：**整块读（64KB）+ 内存里切**（`nextInt` / `nextLine` / `nextToken`）· `std::fs` 三个名字 · `close()` = 提交点 + **编译期查泄漏**（定案 79）· `main(args)` | 已完成 —— 五子棋能真的跟人下 |
| **格式串 `{}`** | **编译期展开**，不是运行时解析；必须是字面量 | 中 |
| ~~**`for` 四种形态**~~ | `for d in dirs` / `for i in 0..n` / `for d in -2..3` / C-style | **2026-09-26 完成**（定案 93；生成物就是 `var` + `while` 判据 `examples/for-loops.extc`）|
| ~~**`match`**~~ | 穷尽检查 + 无载荷枚举（语句，不是表达式）—— **带载荷也做完了**（见下一行）|
| ~~**带载荷枚举**~~（tagged union）| `type shape = \| circle(f64) \| rect(f64, f64)`，绑定载荷 `circle(r) => ...`。之后 `option<slice<u8>>` 才可能存在（`none` 里没有 `value` 字段可填），而且 `option`/`result` 可以重写成**普通枚举** | 中 |
| **`@main` 注解** | 标在任意函数上，不再硬编码 `main` | 低 |
| ~~**模块系统**~~ | **v1 已落地**（定案 70）：语义导入 · 一个文件一个模块 · `@private` · 禁环 · per-module 名字 用法是 `use a::b` / `use a::{b, c}` / `use a::*`（见 §12）| **2026-09-24 完成**；`PLAN` 表里挂 **模块 15 + 23 项**常设验收 |
| **`@recursive`** | 编译器展开成「显式栈 + 循环」，深度上限是编译期常数 | **无限期推后**（2026-09-24 主人拍板：「recursive 关键字可以无限期推后了，没啥意义」）—— **不是待办** |
| **线程** | 保守的 fork-join + 归约；「引用不过线程」 | 低 |

完整清单和理由见 [`DECISIONS.md`](../docs/DECISIONS.md)、[`PLAN.md`](../docs/PLAN.md)、
[`BOOTSTRAP.md`](../docs/topics/BOOTSTRAP.md)（依赖顺序与优先级）。

---

**[索引](README.md)** · [← 9. 错误信息](15-errors.md) · [11. 还没定的 →](17-undecided.md)

- **`trait` / `dyn Trait`**：**第一期已落地**（声明 · `impl Trait for T` · 六条检查 · 静态分发 ·
  静态方法表）。**第二期的阶段 1、2 已落地**：`dyn Trait(x).method()` 构造即调用、载荷**进池**、
  建池用**对象表模式**、派发前**校验世代**、陈旧值 **trap 而不是跳到另一个实现**（`tests/dyn` 9 条判据；
  设计与四阶段计划见 [`docs/topics/DYN.md`](../topics/DYN.md)）。**仍缺**：保存 dyn 值
  （`let d: dyn Tag = …`）与字段/容器元素 · 卸载墓碑 · `T: Trait` 上界 · 关联类型与关联常量 ·
  trait 默认方法 · 孤儿规则与跨 trait 撞名的专门诊断。