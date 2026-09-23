# `ARENA-STATE.md` —— 交接笔记（压缩上下文用，2026-09-23）

> 用途：把"现在做到哪、账怎么记、下一步是什么"压成一页。**详细推导仍在
> `ARENA-SOUNDNESS.md`**（证伪 + 边界 + §9 分档 + §10/§11/§12）；这一页只放**状态与下一步**。

---

## 1. 仓库现状（已提交，可工作）

| 提交 | 内容 |
|---|---|
| **`9e5b2b8`** | 修 arena 健全性：反例库 15 条 UAF → **0**（7 个源文件 + 验证库 + 文档）|
| `964427f` | 清理：删掉中途失败的旧脚本 `tools/arena-fix/step1.py` + `blocks/` |

**验收状态（刚复核过）**

| 判据 | 结果 |
|---|---|
| 反例库仍放行的 UAF | **0** |
| 反例库剩余 | 3 条＝`B_field_table_stale` `D_elemwrite_clears_table`（**真不安全**，照旧被拒）+ `H1_strongupdate_missed`（**回归哨兵**）|
| 「转正」库 `tests/arena-promoted/` | **22/22 接受 + ASan 干净** |
| `tests/run.sh` | **255 通过 / 0 失败** ✓（原误拒 `field-strong-update` 已修）|
| `tests/asan` / `tests/arena` | 8 / 5 |
| 攻击库基线 | **8 条**（原 7 + `h16_allocret` —— 它的攻击前提被证伪：那块内存住在**调用者**的家 arena 里）|
| `bash check.sh`（全量） | **16 通过 / 0 失败** ✓（白名单**已清空**：不再需要"已知误拒"这一档）|
| golden | 差异全属"函数现在收到家 arena / 隐藏格子参数消失"那一类 |
| 编译 | 零警告；编译时长无可测差异 |

**回退**：`git revert 9e5b2b8`，或 `/tmp/ROLLBACK`（合并前 `src/` 副本）。

## 2. 关键文件

| 文件 | 是什么 |
|---|---|
| `ARENA-SOUNDNESS.md` | 主文档：证伪（§4–5）+ 边界（§6）+ 改造清单（§9）+ `@overwrite`（§10）+ 落地结果（§11）+ 路径敏感「下沉」设计与实测（§12）|
| `KNOWN-ISSUES.md` | 已记档的误拒（`field-strong-update`）+ 未完成项 |
| `tools/arena-fix/arena-soundness-fix.patch` | 累积补丁（已 apply 进仓库；留着便于对拍）|
| `tools/arena-fix/apply.sh` / `check.sh` | 一键应用 / 五道验收 |
| `tests/arena-soundness/` | 反例库（3 条）· `tests/arena-promoted/`（22 条 · 必须接受且 ASan 干净）|

## 3. 已做掉的修复（一句话各一条）

`A1` 记账只许往上走（`refreshRootDepth` 取 max、元素写不清表、换指向/整值赋值取 max）·
`A2` `valDepthStructural`（按值结构算上界）+ `valDepthForStore` + **普通赋值补齐字段表** + `if`/`while` **合流取 max** ·
`B1` `exprCallsNeedsHome` 补 4 个 case · `B2` `alloc<T>` 与 `new` **完全对称** ·
`B4` `ST_EXPR` **精准**加宽 · **`B4′` E 敏感决策推迟到摘封闭合之后重算**（同时修好 B2/B3/D1/D2 四族）·
`B5` `needsHome` 收尾按代入后返回类型统一重算 · `F` `@overwrite` 格子自带 `home`、与存储**同生共死** ·
`R1` `curArenaSites` 提前 `vecInit`（顶层初始化式不再 SIGSEGV）。

## 4. 仍然欠的（都在 `KNOWN-ISSUES.md` / `ARENA-SOUNDNESS.md` §11.4）

1. ~~`examples/field-strong-update.extc` 的误拒~~ ✅ **已修（2026-09-23，层 1 第一步）**：
   给 `Sym` 加 `fieldsComplete`（**表完整才允许强更新**）⇒ `tests/run.sh` **255/0**，
   且哨兵 `H1_strongupdate_missed` **仍然被拒** ✓ 细节见 `KNOWN-ISSUES.md`。
   ⚠️ **层 1 还没做完**：这只做了"事实要标完整性"这一小步；"读跟踪 / 分析器只写检查器只读"
   仍然是路线图（见第 5 节）。
2. **路径敏感的 arena 选择（「下沉」）** —— §12。实测靶子：同一 `new` 站点，只读不外存
   **1.6MB** vs 多一条外存分支 **33.2MB**（200 万轮）。机制跑通过一次，判据我写错 5 次 ⇒ 未落地。
3. `alloc<T>` 不进 `promoteInto`；报错分不清"真不安全"与"我算错了"（§A1）。

## 5. ⭐ 下一步 = 层 1 数据流（主人已拍板"先做 1"）

**目标**：把 `D`（深度事实）从"散在 AST 里被随手改的字段"变成**按程序点算出来的表** ——
也就是 `ARENA-SOUNDNESS.md` §9 的 **A5：分析器只写、检查器只读**。

**不做什么**（避免变成大重写）：不碰区域求解、不碰 codegen、不建显式 CFG。

**做什么**
1. 事实**只有一张表**：每个绑定 `(refDepth, 每字段深度)`，**只由一个函数写**（`factSet` / `factJoin`）；
2. 检查器拆成**两遍**：第一遍只**写事实**（赋值 = max；`if`/`while` 出口 = join），
   第二遍才**读事实 + 报错**；
3. 读跟踪随之**免费** ⇒ 强更新不再是特例 ⇒ 第 4 节第 1 条自动消失；
4. 层 2（真 CFG）留给路径敏感。


**验收判据（四条，缺一不可）**
1. `examples/field-strong-update.extc` **转绿**（`tests/run.sh` 变 **255/0**）；
2. ⭐ `tests/arena-soundness/H1_strongupdate_missed.extc` **必须仍然 REJECT**
   （它已经抓到我一次"255/0 但是假的"）；
3. `B_field_table_stale` / `D_elemwrite_clears_table` **仍然被拒**；
4. 转正库 **22/22** 不许变脏；攻击库基线**一字不动**；`tests/asan` 8 / `tests/arena` 5。

**工作纪律（这次踩过的坑，必须守）**
- ⚠️ **不许批量正则替换**：这次 4 次文件损坏全出在批量替换上；
- ⚠️ **改一处立刻 `make`**（用 `touch src/*.c && make`，别信 `make -s` ——
  它曾因时间戳认为目标最新而**不重链**，让我测了好几轮旧二进制）；
- ⚠️ **先把判据写成可独立测的小函数 + 调试开关**，再动大函数；
- ⚠️ 工作副本放 `/tmp`，**不要**在仓库里试错（仓库只在验收通过后提交）。
