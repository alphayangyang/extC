# AST-WALKERS.md —— 遍历器去重（一处权威）

> 起因：作者 2026-09-26「现在 home zone 其实老是出 bug，你可以查一下有没有别的，
> 或者**一个功能两套完全一样的实现**」。查下来的结论比"两套"更严重：**≈8 套**，
> 而且仓库自己已经在注释里承认过它们**同时**犯同一个错。

## 0. 现状：同一个问题被手写了很多遍

| # | 遍历器 | 位置 | 问什么 | 有副作用吗 |
|---|---|---|---|---|
| 1 | `exprHasNew` / `stmtHasNew` | `check_top.c` | 体里有没有 `new` | 否 |
| 2 | `exprCallsNeedsHome` / `stmtCallsNeedsHome`（**precise/wide 双变体**） | `check_top.c:443-679` | 会不会走到需要家 arena 的被调者 | 否 |
| 3 | `exprCallsAllocator` / `stmtCallsAllocator` | `check_top.c`（≈5560 起） | 会不会走到分配者 | 否 |
| 4 | `exprUsesCname` / `stmtUsesCname` | `check_top.c:582-676` | 参数/名字有没有被读 | 否 |
| 5 | `markNamesInExpr` | `check_escape.c` | 标记名字（逃逸集） | **是** |
| 6 | `collectEffectsExpr` | `check_escape.c` / `check_top.c` | 汇总 effects | **是** |
| 7 | `collectOwCallsExpr` | `codegen.c` | 有没有 `@overwrite` 调用 | 否 |
| 8 | `rwExpr` | `modules.c`（loader） | 重写限定名 | **是** |

外加三层**传递闭包**（同一个"沿调用图走到不动点"的形状）：
`needsHome` 闭包（`check_top.c:4876`）· `makesPool` 闭包（`check_top.c:5338`）·
`funcAllocates` 的**惰性记忆化 DFS**（`check_top.c:5550`，注释里明说"这是 needsHome 闭包回答的同一个问题"）。

## 1. 已经发生过的真缺陷（都是这一类）

- **`EX_SLICE` 的三个子表达式**（对象 · 下界 · 上界）被**每一个**遍历器漏掉 ——
  `check_top.c:597-607` 原话："**每一个里它都是真缺陷，不只是少个警告**：边界里的调用本该进
  函数的 effect summary，边界里的限定名从未被重写"。受害者包括 `examples/gomoku-board.extc`
（`lo`/`hi` 被报告为"从未使用"）。
- **`EX_STRUCTLIT`** 里的调用被 `exprCallsNeedsHome` 漏掉 —— `check_top.c:543-551`：
  传递闭包因此报告"这个函数走不到需要家 arena 的被调者" ⇒ 不给它家 arena ⇒ 被调者分配到
  调用者的**块** arena（块结束即释放），而值的深度记作 0 ⇒ **悬垂**。

⇒ 结论：**每加一个 AST 种类，就有 8 个地方可能漏**；漏一个就是 soundness 洞（不是少个警告）。
这正是"home zone 老是出 bug"的结构性原因之一。

### 1.1 真实规模：**29 → 23 个**手写遍历器（2026-09-26 由判据量出，随迁移下降）

`tools/check_walkers.py` 把数量**写进判据**（`RATCHET`，随迁移只降不升；现已 23）：迁移一个就减一，**新增一个直接红**
（前两次人工统计分别是 17 与 41，都是错的 —— 第一次的枚举抽取跨了两个 enum、第二次又漏了语句遍历器；
教训：**这类清单必须由判据数，不能靠人数**。）

## 2. 目标：一处权威

一个通用访问器 + 若干小回调：

```c
/* 回调返回 false = 提前停止（保持原来"找到就退出"的语义与开销）。 */
typedef bool(*ExprVisit)(Checker *c, Expr *e, void *ctx);
bool walkExpr(Checker *c, Expr *e, ExprVisit fn, void *ctx);   /* 列举每个 AST 种类**一次** */
bool walkStmt(Checker *c, Stmt *s, ExprVisit fn, void *ctx);
```

每个问题变成 `walkExpr(c, e, visitorX, &ctx)`，**新增 AST 种类只改 `walkExpr` 一处**
闭包则统一成一个驱动：`closeOverCalls(all, ReachKind)`，`ReachKind` 的四种取值对应
`needsHome(wide)` / `usesHome(narrow)` / `makesPool` / `allocates`
（`funcAllocates` 的惰性记忆化保留 —— 它必须在闭包**之前**回答同一个问题，但它的**每节点判据**
改为调用同一处）

## 3. 迁移顺序（低风险 → 高风险，每步一个提交）

| 步 | 迁移 | 为什么这个顺序 |
|---|---|---|
| ~~1~~ | ~~`exprUsesCname`/`stmtUsesCname`~~ ** 2026-09-26 已完成** | 形状最干净。**生成物在 202 个程序上逐字节相同** ⇒ 迁移是行为保持的；`astWalkStmtChildren` 到位后，`exprUsesCname` 成了死代码，删掉 |
| ~~2~~ | ~~`exprHasNew`/`stmtHasNew`~~ ** 已完成** | 纯查询；生成物 216 个程序逐字节相同 |
| ~~3~~ | ~~`exprCallsAllocator`/`stmtCallsAllocator`~~ ** 已完成**（`funcAllocates` 的惰性记忆化外壳保留） | 纯查询，含循环防护；`exprCallsAllocator` 迁完成死代码，删掉 |
| ~~4~~ | ~~`exprCallsNeedsHome`/`stmtCallsNeedsHome`~~ ** 已完成**（precise/wide 收成一个策略字段 `HomeQ.precise`，入口仍是 `callsNeedsHome`/`callsUsesHome`） | 纯查询但**被 soundness 依赖** ⇒ 迁完跑了反例库 + ASan 推广 + 221 个程序逐字节 A/B |
| 5 | `markNamesInExpr` / `collectEffectsExpr` | **有副作用** ⇒ 必须保序遍历（先子后父/先父后子要一致） |
| 6 | codegen 的 `collectOwCallsExpr` | 跨到 codegen，注意它跑在检查之后 |
| 7 | loader 的 `rwExpr` | 跨到模块加载，最后做 |

## 3.1 判据自己也被查出三个 bug（诚实记账）

写通用访问器时顺手复核了 `check_walkers.py`，它自己有错 —— 而且**错的方式正是它要防的那一类**：

1. **枚举抽取跨了两个 enum**（`typedef enum {`…`} StmtKind;` 从**第一个** enum 起步）⇒ `StmtKind`
   被当成 36 种 ⇒ 每个语句遍历器都被报成"缺 26 个表达式种类"（第一轮那批噪声就是它）；
2. **`enclosing_function` 认错函数名**：它扫的是**剥掉注释后**的文本，而权威清单的标签在注释里 ⇒
   那一行变成前导空格被跳过，于是名字取成了上一个辅助函数；
3. **"宽遍历器"阈值是相对固定的 15**，实际上是在替上面第 1 条打补丁 ⇒ 改成**相对枚举**
（`len(labels)*10 >= len(kinds)*6`）。

另外 **`ST_BREAK`/`ST_CONTINUE` 进了 `LEAF`**：它们是"里面什么都没有"的语句，和 `EX_INT` 一类同理

## 4. 判据（每步都要有）

1. **机械判据（这次要新增）**：对照 `ExprKind` / `StmtKind` 的**枚举**，断言每个取值都被
   `walkExpr`/`walkStmt` 处理 —— 加一个 `EXTC_WALK_SELFCHECK`（像既有的 `EXTC_SELFCHECK`
   那样在检查器里跑一遍），或一个脚本核对 switch 的 case 集合。**这一条是本次去重的核心收益**：
   它把"漏一种形状"从"运行时悬垂"变成"开机自检报错"。
2. 每步：`check.sh` 全绿（现 37 quick / 44 full）。
3. 每步：**生成物逐字节 A/B**（同一批语料，改动前后各编一遍）—— 纯查询类遍历器的迁移应当
   **零差异**；有差异就必须解释清楚。
4. 第 4 步之后额外跑 `tests/arena-soundness/`（反例库）与 `tests/arena-promoted/`。

## 5.4 闭包收口（2026-09-26 完成）：四处 → 一处权威

目标点名的三处"同一个问题"（`needsHome` 闭包 / "Same shape as the closure above" 的 `makesPool` /
"the same question" 的 `funcAllocates`）**外加第四处同形的 `usesHome` 闭包**，现在共用：

| 权威 | 内容 |
|---|---|
| `closeReach` | **唯一的**"单调属性沿调用图取最小不动点"驱动（文件里此前有 4 个 `for(bool changed…)`，现在只剩这 1 个） |
| `getReach` / `setReach` | 唯一的"属性 ↔ 字段"映射（`ReachKind` = `REACH_HOME` / `REACH_POOL` / `REACH_ALLOC` / `REACH_USES_HOME`） |
| `bodyReaches` | **唯一的每节点判据**（"这个函数体直接够到该类被调者吗"）——每个属性各自的规则只写在这里 |
| `roundOverModule` | 唯一"遍历模块里的函数与结构体方法"的轮骨架 |

**故意保留的差异**（写进注释，不是遗漏）：`funcAllocates` 仍是**惰性 + 记忆化**的递归，且"正在计算中"
的函数算作会分配（保守方向）——因为它必须在闭包跑之前回答，且被问在检查过程中

**过程中我引入并当场修掉的一个 bug（如实记录）**：机械搬运 `makesPool` 循环体时漏掉了循环自己的
`changed = false;`，而此刻 `changed` 已是 `bool *` ⇒ 那行把**指针赋成 NULL**，下一句 `*changed = true`
就往地址 0 写 ⇒ **10 个文件段错误**。ASan 一键指到 `roundMakesPool … WRITE to 0x0` ⇒ 当轮修好，
重扫 0 崩溃、A/B 两边都 0 崩溃（这正是"每步都实测"的价值）

## 5.5 已发现的具体缺陷：`EX_DYN` 没有被任何手写遍历器处理（2026-09-26 审计）

**机制**（这是本文档最重要的发现）：这些遍历器是**手写 `switch` + `default:`** ⇒
`-Wswitch` **不会**因为新增一个 `ExprKind` 而报警。对比之下，`codegen.c` / `check_expr.c` 里
那些**没有 `default`** 的 switch 在 `EX_DYN` 加进来时报了警、也被修了；**带 `default` 的
这一批静默漏掉**

**证据**：`ExprKind` 共 26 个取值（`ast.h:88-135`），其中带子表达式的有 20 个；
完整读过的两个遍历器 —— `exprCallsNeedsHome`（`check_top.c:495-571`）与
`exprUsesCname`（`check_top.c:583-650`）—— 的 `case` 集合包含全部那些种类，**唯独没有
`EX_DYN`**，而 `EX_DYN` 的载荷就在 `u.dynv.payload` 里（`ast.h:134`）。

**后果分级**（诚实评估，别夸大）：

| 遍历器 | 漏看的后果 | 严重度 |
|---|---|---|
| `exprUsesCname` | 只出现在载荷里的参数/局部被报"从未使用" ⇒ **多余警告**（零告警规则下可见） | 轻，但**最容易观察** |
| loader 的 `rwExpr` | 载荷里的限定名不被重写 ⇒ 生成物里的 C 名错误（C 编译期就会炸） | 中 |
| `collectEffectsExpr` | 载荷里调用的 effects 不进汇总 ⇒ effects 少报 | 中（可用 `--dump-effects` 观察） |
| `exprHasNew` / `exprCallsAllocator` / `exprCallsNeedsHome` / `markNamesInExpr` | 理论上是 H2 那一类（arena/逃逸判错 ⇒ 悬垂） | **可能重，但今天难触发**：dyn 载荷不能携带引用（载荷会被拷进池），所以"载荷里的分配/引用逃逸"多数形状本来就不合法 ⇒ 实际爆炸半径受限 |

**已写的探针**：`tests/dyn/dyn_payload_param.extc`（钉住第一行：`p` 只在载荷里被用 ⇒
修好后必须**零告警**且输出 `p=7`）。另外两条（限定名重写、effects 汇总）留作迁移时的判据。

**修法（2026-09-26 已落地一半）**
- **止血**：**16 处** `case` 补齐（10 处由 `EX_GENCALL` 路标手查、另 6 处由下面那个机械判据查出，
  含 `markNamesInExpr` 连 `EX_GENCALL` 都漏了）。每处都走 `u.dynv.payload`，并留了注释说明为什么；
- **机械判据**：[`tools/check_walkers.py`](../../tools/check_walkers.py) —— 认出"会递归的 kind switch"
（结构识别：体内再次调用所在函数），要求它覆盖该枚举的**每一种**带子节点的取值；
  **已接进 `check.sh`** ⇒ 下次再加 AST 种类，漏掉会**开机报错**而不是运行时悬垂；
- **判据**：`tests/dyn/dyn_payload_param.extc`（只出现在载荷里的参数曾报"未使用"）已注册进
  `tests/dyn/run.sh`（`dyn_payload_walks`：零告警 + `p=7`）；
- **已关闭（2026-09-26 同日）**：4 个"深度"遍历器补齐了 `EX_TRY`/`EX_CONV`/`EX_BIN`/`EX_UN`（外加
  `valDepthStructural`/`exprRefDepthPure` 的 `EX_SLICE`）。**为什么这不是洁癖**：`valDepth` 的
  安全网是 `max(exprRefDepth, valDepthStructural)`，而**两者对 `?` 和转换都返回 0** ⇒
  一个经 `let s = g()?` 拿到、确实带引用的值被报成深度 0 ⇒ **低估 = 不安全方向**（深度决定存储要活多久）。
  **实测**：`EXTC_DBG_RHO` 的 `UNDER` 条数前后**完全一致**（34/28/28/34：那是"词法上界"诊断本身的悲观，
  语料并不走这些路径 ⇒ 属预防性修复），而生成物在 **216 个程序上逐字节相同** ⇒ 零附带影响
  `check_walkers.py` 的 `ALLOW` 现在是**空的**，门控全绿。

**根治**（仍未做）：本文档 §2 的通用访问器 —— 它一次性把 8 个遍历器的 `EX_DYN`（以及未来任何新种类）
补齐；§4.1 的机械判据则保证**下次**新增 AST 种类时立刻报错，而不是靠人去数 8 个 switch。

## 6. 与 H2 的关系

H2（home zone 传递闭包只做一层，2026-09-26 已修）是**同一片区域**的另一个症状：
闭包/遍历的"传递性"被多处各自实现，任一处少一层就静默失效。修 H2 时已经做过一次小合并
（调用点的 zone 层级改到 `makesPool` 闭包**之后**结算，与 arena 用同一个 `nd`）；
本文档是那次合并的**系统性延伸**。
