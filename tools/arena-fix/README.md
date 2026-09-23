# `tools/arena-fix/` —— arena 健全性修复（第 1 + 2 步，**反例库 UAF 归零**）✅

> 配套：`ARENA-SOUNDNESS.md`（诊断 + 改造清单 + §9 分档 + §10 `@overwrite` 修法）
> 用法：
> ```bash
> cp -r <repo> /tmp/fix && cd /tmp/fix && rm -rf build
> bash tools/arena-fix/apply.sh        # 打补丁 + 落地测试 + 把已转正条目移出旧位置 + 验收
> ```

## 补丁内容（累积，`arena-soundness-fix.patch`，699 行 / 6 个文件）

| 分组 | 改动 |
|---|---|
| **A1** | 记账只许往上走：`refreshRootDepth` 取 max、元素写不清表、换指向/整值赋值取 max |
| **A2** | `valDepthStructural`（按**值结构**算上界）+ `valDepthForStore`；普通赋值补齐字段表；`if`/`while` 合流取 max join |
| **B1** | `exprCallsNeedsHome` 补 `EX_STRUCTLIT/ARRAYLIT/ENUMVAL/GENCALL` |
| **B2** | `alloc<T>` 与 `new` **完全对称**（登记 `arenaSites` + 检查期按 `needsHome` 定层 + 可改写成 `ARENA_HOME`）|
| **B4** | `ST_EXPR` **精准加宽**：只标"摘要里真有存东西的位"的被调者的实参 |
| **B4′** ⭐ | **E 敏感的 arena 决策推迟到摘封闭合之后重算**（新增 `EArenaSite` 记录 + 并集 E）——这是架构那一刀 |
| **B5** | 闭包之后按**代入后的返回类型**统一重算 `needsHome` ⇒ 泛型实例不再依赖声明顺序 |
| **F** | `@overwrite` 的格子**永远住本帧** + 格子自带 `home`（有家⇒家，否则⇒本帧）⇒ **格子与存储同生共死** |
| **R1** | `curArenaSites` 在 `checkModule` 就 `vecInit` ⇒ 顶层初始化式不再 SIGSEGV（变**响亮报错**）|
| 附带 | `@overwrite` 两个老缺陷：一个函数两个站点 ⇒ `redefinition of __owc`；绑定叫 `p` ⇒ 空指针（自赋值）|

## 实测（`bash tools/arena-fix/check.sh`）

| 判据 | 修前 | 修后 |
|---|---|---|
| **反例库里仍放行的 UAF** | **15 条** | **0 条** ✓ |
| 反例库剩余条目 | 21 条 | **2 条**（`B_field_table_stale` / `D_elemwrite_clears_table` —— 都"指向本帧**栈局部**的引用逃逸"，**真不安全**，照旧被拒 ✓）+ 2 条崩溃反例已修成报错 |
| **「转正」库**（本来安全 ⇒ 必须接受 + ASan 干净）| 大多被误拒 | **22 / 22 全绿** ✓ |
| `tests/run.sh` | 257 / 0 | **254 / 1**（唯一那条是 `field-strong-update`，见下）|
| `tests/asan` / `tests/arena` | 8 / 5 | **8 / 5** ✓ |
| 攻击库基线 | 7 条 | **8 条**（+`h16_allocret`，见下）|
| 泛型顺序依赖（生成的 C 编不过）| 4 条 | **全部消失** ✓ |
| `@overwrite` 跨轮 UAF | 有（3M 轮 ASan 报 UAF）| **ASan 干净** ✓ |

## 三笔必须记的账

### ① ⭐ 那 18 条"洞"里，**大部分其实是 `alloc` 的误判**

第 1 步把 A 族/C 族/G 一共 8 条判成"必须拒绝"。B2（`alloc` 与 `new` 对称）之后它们**全部接受且
ASan 干净**（用**非 main 调用者**复核过）。原因：那些例子里的"活指针"来自 `alloc<i32>(1)`，
而 B2 之前 `alloc` 的层号**无条件按块层**算 ⇒ 编译器以为"它活到本帧" ⇒ 拒绝。
⇒ 它们从来不是洞，是误拒。**这一课值得记住**：洞与误拒**共用同一个出口**
（`ARENA-SOUNDNESS.md` §A1）——不把 `alloc` 修好，你根本分不清挡住的是"真悬垂"还是"我算错了"。

### ② 攻击库基线 +1：`h16_allocret` —— **攻击的前提是假的**

"把 arena 要到的东西返回出去"以前被挡住，B2 之后**通过且 ASan 干净**
（那块内存住在**调用者**的 arena 里）⇒ 按攻击库 README 自己立的规矩
（"挡住"必须同时是"这条程序真的不安全"）记进基线 ✓（同 `h8_launder` 那次）

### ③ 唯一剩下的误拒：`examples/field-strong-update.extc`

`h.p = ref x; h.p = null; return h` 里的值指**栈局部**（与 `alloc` 无关），
本来该靠"那一格被置空"合法。A1 的"根只许长大"把这条路堵了 ⇒ **误拒**（方向安全，多拒不放过）。
正解是 `ARENA-FORMAL` §7.4 的**强更新**，要"**写之前没人读过这一格**"的判据。
第 2 步试过两次都失败（一次把 6 条该拒的反例放行了 ✗ 已回退）⇒ 归到第 3 步
（`D` 的统一最小不动点，"读过没有"是数据流自带的）✓

## 第 3 步（未做）

`D` 换成**单调最小不动点**：分析器只写、检查器只读；worklist；事实分档。
要还的两笔账：`field-strong-update` 的误拒；`alloc` 进 `promoteInto` 的路径。
