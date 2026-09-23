# `tests/arena-soundness/` —— arena 健全性的**反例库**（2026-09-23）

> 配套文档：[`ARENA-SOUNDNESS.md`](../../ARENA-SOUNDNESS.md)（形式化证伪 + 边界清单）
> 运行：`./run.sh`（全部 21 条）或 `./run.sh C`（只跑 C 打头的）

## 这个目录的语义是**反的**

`tests/errors/` 是"必须被拒"、`tests/asan/` 是"必须跑得干净"——**这里是"现在还能编过，
而且真的悬垂"**。所以：

| 输出 | 含义 | 该怎么办 |
|---|---|---|
| `ACCEPT+UAF` | **洞还在**（检查器放行 + ASan 报悬垂）✓ 证据成立 | 就是它要盯住的东西 |
| `REJECT` | **洞已修好**（现在被编译期挡住了）✓ | 把这一条**搬到 `tests/errors/`**，从这里删掉 |
| `CRASH` | 编译器**崩了**（不是报错）| R1 那一族：要的是**响亮的诊断** ✗ |
| `GCC-ERR` | 生成的 C 编不过（extC 自己的 bug）| R2 那一族 ✗ |
| `ACCEPT+CLEAN` | 反例**不成立**了（可能被"修"成了别的形状）| **复核**：要么是修好了，要么是反例退化了 |

⇒ **`./run.sh` 永远返回 0**（不参与 `check.sh` 的通过/失败计数）——这是**证据**，不是回归判据 ✓

## 五族（每族 = 一条坏掉的机制）

| 族 | 机制（哪一句写错了） | 成员 |
|---|---|---|
| **A · 藏在字面量里的调用看不见** | `exprCallsNeedsHome`（`check_top.c:273-322`）**没有 `EX_STRUCTLIT/EX_ARRAYLIT/EX_ENUMVAL/EX_GENCALL` 分支** ⇒ 传递闭包判"我没有有家被调者" ⇒ 调用点的 pending 站点永远不被改写成 `ARENA_HOME` ⇒ 被调者分配进**调用者的块 arena**，而出块就 `release` | `E_literal_hidden_home`（原 E）、`A2_literal_enum_hidden`、`A3_literal_arr_hidden`、`A4_literal_into_outerfield` |
| **B · 字段表/整根记账被**覆盖** | `refreshRootDepth`（`check_top.c:882`）是覆盖不是 `max`；非引用型目标的赋值**完全不更新字段表**（`check_stmt.c:229-233` 只覆盖字面量初始化）；`check_escape.c:132` 整值读取只看一个数；`check_escape.c:96` 早退把 `TY_REF` 判成"没有引用" | `A_field_root_lowered`、`B_field_table_stale`、`C1_if_join_fieldcell`、`C2_if_join_refbinding`、`C3_if_join_wholevalue`、`D_elemwrite_clears_table`、`G_stale_origin` |
| **C · 表达式语句不算逃逸** | `markNamesInStmt` 的 **`ST_EXPR → false`**（`check_top.c:777`）⇒ 经**调用**发布出去的局部不进 E ⇒ `callHomeDepth` / 方法接收者那一支把它当成"深度 1 = 调用者的帧" ⇒ `arenaArg = &本帧arena`（**被调者自己的帧！**）⇒ 调用一返回就 release | `B2_exprstmt_blindspot`、`B3_exprstmt_method` |
| **D · `ARENA_HOME` 被当成"深度 0"** | `ARENA_HOME` 实际是**调用者按"这次调用的结果存在哪"选的那只**（`markCallHomeIfEscaping`，`check_top.c:618`）；结果存在块里 ⇒ 家 = **块 arena**。而深度修正只覆盖 `EX_CALL/EX_METHOD`（`check_stmt.c:216-224`）⇒ `EX_ASSOC`/字面量包着的调用保持"深度 0" ⇒ 之后发布进调用者对象被放行 | `D1_assoc_publish_home`、`D2_publish_home_block` |
| **F · `@overwrite` 复用格活过了它赖以分配的那只 arena** ✅ **已修（方案 A/B2）** | 旧：格子住**调用者帧**、buffer 住**被调者帧** ⇒ 第二次执行同一调用点对着已 free 的块 `memset`。新：格子永远住本帧 + 格子自带 `home`（有家 ⇒ `__extc_home`，否则 ⇒ `&__extc_a[1]`）⇒ **同生共死** ⇒ 见 `ARENA-SOUNDNESS.md` §10 | `F_overwrite_reuse_cell`（修后 **ACCEPT+CLEAN** ⇒ 应从本库毕业，搬进 `tests/asan/`）|

## 另外三族（不是内存不安全，但是真缺陷）

| 族 | 现象 | 位置 |
|---|---|---|
| **R1 · 编译器崩** | 顶层初始化式里有 `new` / 有家调用 ⇒ **SIGSEGV**（本该报"全局只能用常量初始化"）| `check_expr.c:788`、`check_top.c:644` 往没 `vecInit` 的 `curArenaSites` push（`vecInit` 在 `check_top.c:1156`，而 `checkGlobals` 在 `:1528` 先跑）|
| **R2 · 生成的 C 编不过** | ① 泛型**实例先于模板**被创建 ⇒ `'__extc_home' undeclared`（**依赖声明顺序**）② 字面量里藏调用 + 函数没有别的分配点 ⇒ `'__extc_a' undeclared` | ① `check_top.c:1289/1550/1731-1742` vs `codegen.c:2012-2023`；② 同 A 族的谓词 + `mayUseArena`（`check_top.c:1781`）|
| **X · `extern!` 的签字是纯信任** | 签了 `effects Addr=0 Cont=0` 的声明 + C 那边真的把指针留下来 ⇒ 接受 + `heap-use-after-free`（arena 块）| `check_top.c:490-510`；**按定案 72 是设计选择**，但 arena 块被 free 时 C 那边还握着指针 ⇒ 至少该在文档/诊断里说清 |

## ⚠️ 已经修好的那些（毕业名单）

| 文件 | 修法 | 出处 | 毕业去向 |
|---|---|---|---|
| `A_field_root_lowered` | `refreshRootDepth` 取 max（只许长大）| §9 档 0（A1）| `tests/errors/arena_root_depth_lowered.extc` ✓ |
| `B_field_table_stale` | 普通赋值也走 `noteFieldDepthWrite`；深度改用"值结构上界 ∪ exprRefDepth" | §9 档 0（A2 写点）| `tests/errors/arena_field_table_stale.extc` ✓ |
| `C1_if_join_fieldcell` | 控制流合流取 max join | §9 档 0（A2 join）| `tests/errors/arena_if_join_field_depth.extc` ✓ |
| `C2_if_join_refbinding` | 同上（引用型绑定那一支）| §9 档 0（A2 join）| `tests/errors/arena_if_join_ref_binding.extc` ✓ |
| `C3_if_join_wholevalue` | 同上（整值赋值那一支）| §9 档 0（A2 join）| `tests/errors/arena_if_join_whole_value.extc` ✓ |
| `D_elemwrite_clears_table` | 元素写不再清表（取 max）| §9 档 0（A1）| `tests/errors/arena_elem_write_clears_table.extc` ✓ |
| `G_stale_origin` | 字段写不再让 `origin` 过期（改走字段表/值结构上界）| §9 档 0（A2）| `tests/errors/arena_stale_origin.extc` ✓ |
| `F_overwrite_reuse_cell` | 格子与它管的存储对齐（格子自带 `home`）| §10 | 待搬（修法已验证，未进仓库）|

**第 1 步（A1+A2）实测**：仍放行的 UAF **15 → 8 条**（转成编译期 REJECT 的 7 条见上表）；
`tests/run.sh` 263 通过 / 1 失败（那个失败是**已知误拒** `examples/field-strong-update.extc`，
正解要做 `ARENA-FORMAL` §7.4 的强更新，归到第 3 步）⇒ 见 `tools/arena-fix/README.md` ✓

**2026-09-23 毕业后又转正 8 条**（A2/A3/A4 字面量族 + `A_field` + C1/C2/C3 + `G_stale_origin`）：
第 1 步把它们判成"必须拒绝"，B2（`alloc` 与 `new` 对称）之后它们**接受且 ASan 干净**
⇒ 那 8 条**从来不是洞**，而是 `alloc` 的层号被算错导致的误拒 ⇒ 搬进
`tests/arena-promoted/`（"**本来安全、以前被误拒**"库）✓
⚠️ **这一课值得记住**：洞与误拒**共用同一个出口**（`ARENA-SOUNDNESS.md` §A1）——
不把 `alloc` 那条修好，你根本分不清挡住的是"真悬垂"还是"我算错了" ✗

⇒ 毕业的意思是：**搬到 `tests/errors/`（必须被拒）/ `tests/asan/`（必须跑得干净）/
`tests/arena-promoted/`（本来安全，必须接受）**，然后从这里删掉 —— 本库只留"现在还漏的" ✓
⇒ 补丁在 `tools/arena-fix/`（`*.patch` + `check.sh`），**改的是 `src/`，所以本库的
   `run.sh` 会自动反映"洞还在/已修"** ✓

## 复现一个（手工）

```bash
cd <repo>
./build/extc tests/arena-soundness/A_field_root_lowered.extc -o /tmp/a.c   # 检查器接受
gcc -O1 -g -fsanitize=address /tmp/a.c -o /tmp/a && /tmp/a                 # heap-use-after-free
```
看 ASan 报告里的 `freed by ... extc_arena_release` ⇒ **确实是 arena 内存**，不是越界 ✓
（`B_field_table_stale` / `D_elemwrite_clears_table` 报的是 `stack-use-after-scope`——
那是"指向栈局部的引用逃出去了"，同一族记账缺口的另一个表现。）

## 对照（把洞钉在"那一句"上的实验，都实测过）

| 反例 | 只改一处 → 结果翻了 | 说明 |
|---|---|---|
| `A_field_root_lowered` | 删掉 `h3.p = null` ⇒ **被正确拒绝** | 洞在"覆盖"那一句，不在整体保守 |
| `B2_exprstmt_blindspot` | 把发布改成 `*out = l`（ST_ASSIGN）⇒ **ACCEPT + 干净** | 洞在 `ST_EXPR → false` |
| `D1_assoc_publish_home` | 换成普通自由函数调用 ⇒ **被正确拒绝** | 洞在"只修 `EX_CALL/EX_METHOD`" |
| `E_literal_hidden_home` | 加一个**可见**的 `mknode()` 调用 ⇒ **ACCEPT + 干净** | 洞在 `exprCallsNeedsHome` 缺分支 |
| `G_stale_origin` | 让 origin 里没有 `new` ⇒ **被正确拒绝** | 洞在 origin 不失效 |
| `F_overwrite_reuse_cell` | 把 `@overwrite` 去掉（每轮新分配）⇒ ASan 干净 | 洞在复用格与 arena 寿命由两条规则定 |
