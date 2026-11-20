# 名词账：数组、视图、容器

> 状态：**研究稿** —— 盘点现状、给出候选目标词汇与迁移面；尚未改动任何代码。
> 落地按 §6 的顺序逐步进行，每一步单独验收。

## 0. 目标与验收

用户口径：改文件从来不算代价，唯一标准是最终简洁统一、不造成混乱。
所以本页不问「改动多大」，只问「改完还剩几套说法」。

验收判据（全部可机检）：

1. **一个概念一个词**：同一事物在类型名、方法名、错误文案、手册正文里同名。
2. **一个动作一个入口**：拿视图只有一种写法，没有别名。
3. **一条规则没有位置例外**：「期望视图的位置自动借」覆盖实参、初始化、返回、字段。
4. **名字数上限**：手册入门部分出现的数组/视图名词数等于 §5 目标表（现在多 3 个）。
5. **没有死名字**：源码与活文档里不再出现取不到的定义、或教人写不存在的方法（§4 的 T1/T2）。

## 1. 一句话结论

视图这件事现在是 **1 个概念、4 个名字、7 条规则、2 套执行机制**（§2、§3）；
可以压成 **1 个概念、1 个名字、3 条规则、1 套执行机制**（§5 方案 A）。
其余改动是机械拼写替换（§6 迁移面），不含语义变化。

## 2. 现状：概念与它们的名字

| 概念 | 现在的名字 | 出处 |
|---|---|---|
| 借来的连续存储 | 类型 `slice<T>`；手册章节标题写「视图」；错误文案写 view | `stdlib/prelude.extc:struct slice`、`docs/usage/language.md` §7.3 |
| 可写权限 | 外层 `mut slice<T>`、元素层 `slice<mut T>` | `src/types/types.c:ttViewDowngradable` |
| 取只读视图 | `a[lo..hi]`、`.view()` | `.view()` 138 处 / 72 个 .extc |
| 取可写视图 | `a[lo..hi]`、`.viewMut()`、`.asMutSlice()` | viewMut 32 处 / 15 个文件；asMutSlice 13 处 / 7 个文件（只有 varArray） |
| 已失效的别名 | `.asSlice()` | 实现 0 处；活文档 0 处；changelog 与 archive 93 处 |
| 拥有型变长数组 | `vector<T>`、`varArray<T>` | 59 / 80 个 .extc |
| 底层存储 | `buf<T>`、`block<T>`、`ndarray<T>`、`ndview<T>` | `stdlib/std/data/`、`stdlib/std/nd.extc` |
| 内联定长 | `[N]T` | 语言级 |
| 帧/块分配 | `new T[n]` → `slice<T>` | 语言级 |

### 2.1 视图协议靠名字

识别「这是视图」是**类型名比较**：`viewElemOf`（`src/check/check_escape.c`）、
`ttIsViewType`（`src/types/types.c`）、`src/back/cg/30-expressions.inc`、`src/main.c`、
`src/check/check_lvl.c`、`src/check/check_top.c` —— 全仓 `\"slice\"` 字面量共 6 处。
所以改类型名是 6 处编译器改动 + 一次全仓拼写替换，不是新机制。

### 2.2 借出协议靠方法名

任何类型只要返回类型对得上（`view() -> slice<T>`、可写参数要 
`viewMut() -> mut slice<T>` 或 `asMutSlice() -> mut slice<T>`）就能被自动借出，
手册 §7.3 明说「借出不是内建名单」。提供 `view()` 的类型有 10 个：
`buf`、`block`、`vector`、`string`、`varArray`、`ndarray`、`plate`、
`borrowGuard`、`vectorGuard`、`stringGuard`。其中 9 个返回 `slice<T>`，
`ndarray.view()` 返回 `ndview<T>` —— 同名动作两个结果类型（T7）。
注意 `block.view(from, to)` 与 `plate.view(off, n)` 带参数：同名不同参数在不同类型上是合法的，
**同一类型上不允许重载**（实测 `duplicate method `get``），这条限制决定了 §5 的方案形状。

### 2.3 mut 是权限，不是布局

`ttViewDowngradable` 的注释写明：`slice<mut slice<i32>>` 与 `slice<slice<i32>>` 是同一个 C 结构体，
权限只能丢不能加。`a[lo..hi]` 的权限由绑定推断：`var mid = a[2..5]` 得 `mut slice<i32>`，
`let v = a[1..3]` 得 `slice<i32>`（手册 §7.3）。

## 3. 当前规则清单（7 条）

这 7 条是使用者今天必须记住的全部内容，第 3 条是全部不适感的来源。

| # | 规则 | 证据 |
|---|---|---|
| R1 | 视图类型叫 `slice<T>`；能被借出的条件看返回类型，不看名单 | §2.1、§2.2 |
| R2 | 拿视图有 4 条路：`a[lo..hi]`、`.view()`、`.viewMut()`、`.asMutSlice()`，另有一条死路 `.asSlice()` | §2 |
| R3 | **自动借出只在调用位置**：实参与方法实参借，初始化和 return 不借 | `src/check/check_expr.c:lendViewArg`；实测 `sum(v)`/`sum(a)`/`sum(w)`/`sum(mk())` 全过，`var s: slice<i64> = v` 报错 |
| R4 | `mut` 有两层位置：外层（传参）与元素层（嵌套视图） | §2.3 |
| R5 | 视图失效分两套：容器搬家静态报错；`borrow()` 守卫是运行期 pin，搬家 trap | 静态：``v` changes the storage that `s` views`；运行期：`this buffer is frozen by a live borrow`（`stdlib/std/data/buf.extc`） |
| R6 | 视图不能活过存储：借用深度 ≤ 0 | 局部数组视图进字段返回被拒：「borrowed from depth 1, but this can only hold up to depth 0」 |
| R7 | 容器没有数组语法：`v[i]`、`v.len`、`s[1..3]` 都报错 | 「cannot index a value of type vector::vector<i64>」、`struct vector::vector has no field len`、「cannot slice a value of type string::string」（note：`Only a fixed array or a view can be sliced`） |

## 4. 冲突点（13 条）

每条都能独立修；列在一起是为了看清它们其实是同一件事。

| # | 现象 | 证据（实测/源码） |
|---|---|---|
| T1 | 手册教写 `asMutSlice()`，实现偏好 `viewMut()` | 手册 §7.3 五处（`fill(va)` 等价于 `fill(va.asMutSlice())`）；`feeldLendName` 先找 view/viewMut，找不到才试 asSlice/asMutSlice |
| T2 | 诊断教人写 `v.asSlice()`，而这个方法不存在 | `var s: slice<i64> = v` 的 note 原文 `lend a read-only view explicitly: `v.asSlice()``；照写报 `no method asSlice on varArray<i64>` |
| T3 | 同一动作两套隐式策略 | `sum(v)` 行，`var s: slice<i64> = v` 不行（后者 note 让人写死名字） |
| T4 | 死名字还留在检查器里 | `asSlice` 在 .extc 出现 0 次，却仍被 `feeldLendName` 当合法拼写 |
| T5 | 概念两个词 | 手册 §7.3 标题「视图」，正文与类型都写 slice；活文档 `slice` 1094 处、`视图` 57 处 |
| T6 | `mut` 两个位置 | 外层 `mut slice<T>` 695 处；元素层 `slice<mut T>` |
| T7 | `ndview<T>` 是另一套 view | 字段是 `data: mut slice<T>` + off/rank/shape/strides；`ndarray.view()` 返回它 |
| T8 | 两套等价的可变长容器 | `vector<T>` 59 个文件、`varArray<T>` 80 个文件，方法集重叠（push/pop/get/set/clear/view/snapshot…） |
| T9 | 字符串子段三套写法 | `s.sub(lo,hi)` 拷贝、`s.subView(lo,hi)` 借用、`s.view()[lo..hi]` 也借用（实测输出 `el`） |
| T10 | `subView` 注释与检查器行为不符 | 注释称检查器看不见，实测 `var v = s.view(); s.append(...)` 被拒 |
| T11 | 视图可手工构造 | `var s: slice<i64> = { data: ref a[0], len: i64(2) }` 编译通过 |
| T12 | 内建类型清单列了 `str`，但它不可用 | `var x: str` 报 `unknown type `str``，note 里仍列 str（`src/types/types.c`） |
| T13 | 视图能用、容器不能用数组语法 | 数组与视图可以下标/切片；`vector`/`string` 都不行 |

T1–T5 是一件事：**同一个动作有多个名字**。T6–T13 是另一件事：**同一个概念有多个实现**。

## 5. 目标词汇：三个候选

### 方案 A（推荐）：一个词到底

| 概念 | 现在 | A 之后 |
|---|---|---|
| 视图类型 | `slice<T>` | `view<T>` |
| 可写视图 | `mut slice<T>` | `mut view<T>` |
| 嵌套元素权限 | `slice<mut T>` | `view<mut T>` |
| 取只读 | `[..]` / `view()` / `asSlice()`（死） | `[..]` / `view()` |
| 取可写 | `[..]` / `viewMut()` / `asMutSlice()` | `[..]` / `viewMut()` |
| 自动借出 | 只在实参、方法实参 | 所有期望视图的位置 |
| 字符串子段 | `sub` / `subView` / `view()[..]` | `view()[lo..hi]`（借）、`string::from(s.view()[lo..hi])`（拷） |
| 拥有型变长 | `vector` / `varArray` | `vector` |
| 底层存储 | `buf` / `block` / `ndarray` / `ndview` | 名字不变；从手册入门退出，只进 design 文档 |

规则数从 7 降到 3：

1. 视图是 `view<T>`；写权限用 `mut` 前缀（每一层各一个）。
2. 拿视图：`a[lo..hi]`、`.view()`、`.viewMut()`；期望视图的位置自动借。
3. 视图的有效期到它借的那段存储改变为止：搬家静态报错，`borrow()` 守卫运行期 trap。

被删掉的名字：`asSlice`（死）、`asMutSlice`（并入 viewMut）、`varArray`（并入 vector）、
`sub` / `subView`（并入 `view()[lo..hi]`）。`ndview` 保留：改名后它自然读作 nd + view，不再是撞名。

### 方案 B：保留 `slice` 名字，只收敛入口与规则

改 T1–T4、T9–T11、T13，不改类型名。好处是迁移面砍掉一半；代价是概念仍有两个词（「视图」与 slice），
手册标题与类型名永远对不上，且与 ARRAY.md 计划中的 `view1/view2` 命名分叉。

### 方案 C：只改文案与规则（最小）

只修 T1、T2、T10、T12 四条 stale 文案，其余不动。适合只想要「不再教错东西」，不追求统一。

## 6. 迁移面与落地顺序

| 改动点 | 位置 | 规模 |
|---|---|---|
| `.extc` 拼写 | 538 个文件 `slice<` 2235 处 + 214 个文件 `mut slice<` 695 处 | 机械替换 |
| 生成 C | `slice<` 617 处（都在测试与 bench 的产物里） | 由 `emitted_identity` 重新冻结 |
| 活文档 | `slice<` 1094 处 / 98 文件、`asMutSlice` 34 处 | 机械替换 + 5 处句子重写 |
| 编译器 | 6 处 `\"slice\"` 字面量 + 视图相关诊断文案 | 少量 |
| 测试期望 | `tests/errors` 里含 `slice` 的 `expect:` 行 1 条 | 1 处 |
| 脚本 | `tools/probe` 1 处 | 1 处 |

每一步单独提交、单独验收；前一步不依赖后一步的收益：

| 步骤 | 内容 | 判据 |
|---|---|---|
| S1 文案与规则 | 修 T1/T2/T10/T12；手册统一到一个说法（视图 = 借来的连续存储） | `./check.sh docs` 全绿，`tests/errors` 期望串不变 |
| S2 入口收敛 | 删 `asSlice`/`asMutSlice` 识别；`varArray.asMutSlice` 改 `viewMut`；新增门禁 `tools/check_vocab.py`（禁死名字出现在 src、stdlib、活文档） | 新门禁 + 全量构建 |
| S3 自动借出统一 | `lendViewArg` 提到「期望视图类型」的判定，覆盖初始化、return、赋值、结构体字段；补 `tests` 用例 | 新增用例全绿，既有 `tests/run.sh` 377/0 |
| S4 改名 `slice`→`view` | 机械替换 + 6 处字面量 + 诊断文案；重冻结 `emitted_identity` | 全门禁：`tests/run.sh`、`check.sh nobench`、`docs`、`reclaim*`、`gc-p0`、`gc-torture` |
| S5 容器收敛 | `varArray` 退役（80 个文件迁移）、`buf`/`block` 退出入门手册、删 `sub`/`subView` | 容器四方 bench 判据不劣化，`nobench`/`docs` 全绿 |

## 7. 待拍板

| # | 问题 | 我的建议 |
|---|---|---|
| Q1 | 类型名用 `view<T>` 还是留 `slice<T>` | `view<T>`：让「视图」这个概念只有一个词；rename 是 6 处字面量 + 机械替换 |
| Q2 | 入口是否只留 `view()`/`viewMut()`，删 `asSlice`/`asMutSlice` | 删；用户自己写的容器不再认别名 |
| Q3 | 自动借出统一到所有期望视图的位置 | 统一（规则 3 的「没有位置例外」） |
| Q4 | `varArray` 退役、`vector` 唯一 | 退役；80 个文件同批迁移 |
| Q5 | `buf`/`block` 是否退出入门手册 | 退出：它们是存储基质的实现细节，进 design 文档即可 |
| Q6 | 与 `docs/design/ARRAY.md` 的多维计划对齐 | rank-1 就叫 `view<T>`，将来二维叫 `view2<T>` |

## 8. 取证

拼写计数（全仓精确计数，脚本一次性跑）：

| 范围 | `slice<` | `mut slice<` | `asMutSlice` | `asSlice` | `.view()` | `viewMut` |
|---|---|---|---|---|---|---|
| `.extc` | 2235 / 538 文件 | 695 / 214 | 13 / 7 | 0 | 138 / 72 | 32 / 15 |
| `.c .h .inc` | 617 / 224 | 64 / 18 | 44 / 12 | 24 / 4 | 15 / 4 | 228 / 42 |
| `.md` | 1094 / 98 | 291 / 60 | 34 / 11 | 93 / 23 | 53 / 14 | 30 / 10 |

行为实测（`./build/extc --run`）：

- `sum(v)` / `sum(a)` / `sum(w)` / `sum(mk())` 全过；`var s: slice<i64> = v` 报错（R3）。
- `a[lo..hi]` 可切片；`s.view()[1..3]` 输出 `el`；`vec.viewMut()[0] = i64(9)` 写穿生效。
- `s[1..3]` 报 `cannot slice a value of type string::string`（R7）。
- `v[0]`、`v.len` 报错（R7）。
- 视图可以从参数返回、可以存进字段（深度 0）；局部数组视图进字段返回被拒（R6）。
- 同一类型两个同名方法被拒：`struct `pt` has duplicate method `get``。
- `var x: str` 报 `unknown type `str``（T12）。

## 9. 不在这页

存储基质（帧 arena / 缓冲区预留 / rc 槽）与回收策略不在本页：它们决定「存储活多久」，
本页只决定「借来的那段存储叫什么、怎么拿」。前者见 `docs/design/MEMORY-MODEL.md` 与 `BUF-INVARIANT.md`。