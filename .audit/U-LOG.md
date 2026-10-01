# U 批次：接口统一 + 调试断言化

主人 2026-10-01 指定：**合并不同接口尽量统一** + **debug 模式下多布设 assert 替代哨兵**。

## U1（本轮完成）：断言基础设施

### 设计（以及为什么不是条件编译）

| 项 | 决定 | 理由 |
|---|---|---|
| `src/dbg.h` / `src/dbg.c` | 三条宏：`EXTC_DBG_ASSERT(cond)`（不变量）、`EXTC_DBG_FALLBACK(what)`（按设计不该走到的兜底）、`EXTC_DBG_NOTE(what)`（**已知会被走到的**保守路径） | 三类东西的处置不同：不变量破了就该 abort；兜底被走到是信号；活路径只能计数 |
| 开关 | **运行时**：环境变量 `EXTC_DBG=1`（`extcDbgHit` 立刻返回时只花一次分支） | ① 不需要第二套构建，同一只二进制两种用途；② **头文件里不能有条件编译** —— 本仓库 `tools/check_guards.py` 的房规是"include guard 内不许有第二个 `#endif`"，第一版把 `#ifdef EXTC_DEBUG` 写进 `dbg.h` 就被它判红（17 ok / 1 broken） |
| 命中形态 | 开关开着时打印 `[assert]` / `[fallback]` / `[note]` + `file:line` + 说明，前两者随后 `abort()` | 闸门靠"这只编译器在这里 abort"判红，不靠"输出里有字样" |

### 第一批布点（8 处）

| 位置 | 类型 | 理由 |
|---|---|---|
| `exprRefDepthPure` 递归预算 40 | FALLBACK | 原本 `return 0`（"活得最久"）⇒ 兜底方向反了，与 P0-6c 同族 |
| `exprRefDepthPure` 来源环 | FALLBACK | 同上（原本 `return 0`） |
| `promoteFieldsAt` 预算 32 | FALLBACK | 原本 `return true`（"提权成功"）⇒ 静默跳过 |
| `EArenaSite` 槽位 `rec->n <= 8` | ASSERT | 结构体只声明 8 槽 |
| 字段表 `s->nfields <= 4` | ASSERT | 表满转 `otherDepth` 是设计兜底，但越界不是 |
| 参数 Sym 表 `f->nParamSyms <= 64` | ASSERT | 与数组容量一致（P1-4 的 64 上限） |
| 调用点深度决策 `seen \|\| rec->n == 0` | ASSERT | **P0-2 的哨兵就在这里**：`nd == 0` 曾兼"没见过"与"活得最久"两义 |
| `promoteInto2` 预算 32 / 载体遍历预算 32 | **NOTE** | 见下 |

### 实测（这是本轮最有价值的产出）

| 运行 | 结果 |
|---|---|
| 发布（`EXTC_DBG` 不设） | `tests/run.sh` **325 / 0** |
| 断言版（`EXTC_DBG=1`） | `tests/run.sh` **325 / 0**，`[note]` 命中 **7** 次，`[assert]`/`[fallback]` 命中 **0** 次 |
| 语料（两种模式） | 闸门⑤ ok（18 份，基线 1 条 = P0-6e） |

⇒ **前六处"按设计不该走到"的路径，在整个套件 + 语料上确实没被走到**（它们现在被看守住了）；
而 `promoteInto2` 的 32 跳预算**在正常例子里就会命中 8 次**
（`coalesce`、`escape-promotion`、`list-return`、`new`、`nullable-ref`、`out-param`）——
所以它是**活路径**，`return false`（"提不动"）是它今天的语义：保守，但**静默**。
本轮把它从"假装不该发生"改成"可计数"，并把"这个预算是否太小 / 这条路径该不该保守"记成待研究项。

### 本轮踩的坑（值得记住）

第一版把 `EXTC_DBG_FALLBACK(...)` 写在 `if` **之后**、`return` **之前**：

```c
if (hops > 32) EXTC_DBG_FALLBACK("...");   /* 发布构建里展开成 ((void)0) */
return false;                              /* ← 变成了无条件执行 */
```

⇒ 发布构建里 `promoteInto2` 永远返回 false，`check.sh quick` 掉到 **46/9**（`store-promotion`、
`promote`、`arena` 等一片红）。**教训：宏必须放在分支里面**（`if (...) { MACRO(...); return X; }`），
否则"加断言"会悄悄改掉发布语义。这条已作为 U 批次的操作规则。

## U2（本轮完成）：单一指称 —— 第一刀（行为不变）

### 做了什么

**1. 唯一权威的文档块 + 共享遍历**（`src/check_internal.h`）：

过去"这个值活多深 / 它是谁"被问了一百多次，由**三套各写一遍的根遍历**回答，
而三者的差异只藏在循环体里：

| 入口 | 步进 | 跳数 | `EX_IDENT` 怎么解 |
|---|---|---|---|
| `placeRoot` | FIELD INDEX SLICE | 不限 | 查作用域（`lookup`） |
| `placeRootName` | FIELD INDEX DEREF | 不限 | 取**语法名字**（不解析） |
| `rootSymNoScope` | FIELD INDEX DEREF SIGN | 16 | 取节点上的绑定（`identBindOf`） |

现在这三份循环**合成一个** `extcRootLeaf(e, steps, maxHops)`（掩码 `EXTC_ROOT_*`），
三个入口各自只剩 2–3 行：**差异从"藏起来的实现"变成"写在调用点的掩码"**。
头文件里那张表 + **极性契约**（①②返回的是深度、`0` = 活得最久、数越小活得越久 ⇒ 兜底必须落
在更长寿一侧或拒绝）就是唯一权威；并写明"新代码不要再写第四份根遍历"。

**2. 掩码自身也是不变量**（U1 的断言用在这里）：

```c
EXTC_DBG_ASSERT(steps != 0 && (steps & ~(五个已知位)) == 0);
```

理由：空掩码/未知位是调用点的笔误，而"走不到根"会被上层当成"这不是一个地方"（保守），
**笔误不会报错、只会让分析悄悄变保守** —— 正是该断言的地方。

### 验收（U2 要求行为不变，所以两种模式都跑）

| 检查 | 结果 |
|---|---|
| 发布构建 | `tests/run.sh` **325 / 0** |
| 断言版（`EXTC_DBG=1`） | **325 / 0**（`[assert]`/`[fallback]` 仍为 0） |
| 闸门⑤·语料 | ok（18 份，基线 1 条）、断言版下同样 ok |
| `check.sh quick` | **55 / 0** |
| 闸门①checkc 全量 / ②ASan 全量 / ③差分 / ④规模 | 全绿（基线为空） |

### 本轮记下的两个**有待商量的**后续（不是 U2 的一部分，因为它们是行为变更）

1. `check_top.c` 逃逸集合登记里还有一处**按名字**找参数：
   `if (rn && (isEscapeeName(c, rn) || paramIndex(f, rn) >= 0))` —— 与 P0-6d 同一类
   （遮蔽参数时会认错），而这里**手边就有表达式**（`s->u.assign.target`），可以换成
   `paramIndexOfExpr`。它决定的是**逃逸集合 E**，进而决定 arena 落点 ⇒ 属于"改行为"的一步，
   需要按 R1/R3 的规矩（语料 + ASan + 判据）单独做，不能混进"行为不变"的 U2。
2. `paramIndex`（按名字）与 `paramIndexOfExpr`（按符号）**并存**：前者只在"手上只有名字"的
   调用点还需要。建议 U5 时把它改名成 `paramIndexByName` 并在头文件里注明"仅在只有名字时用；
   有表达式一律用身份版"，让两者不再看起来等价。

## U3（本轮完成）：写点单一入口 —— 第一刀（行为不变）

### 做了什么

**1. 一个正门 `noteStore(c, value, target, at, line)`**（声明在 `check_internal.h`，
定义在 `check_escape.c` 的 `recordStore` 旁边）：把一次写必须同时做的四件事按**固定顺序**收在一处 ——

| 步骤 | 调用 | 为什么顺序不能变 |
|---|---|---|
| ① 电平记录 | `recordStore` | 求解器要先看到"这个值被发布到 `at` 层"这个**要求** |
| ② 提权 | `promoteInto` | 提不动就保持保守（P0-16 的两个半边之一） |
| ③ 逃逸集合那一半 | `markCallHomeIfEscaping` | 与 arena 落点有关 |
| ④ 字段表 / 整值表 | `noteWholeValueDepthWrite`（`EX_IDENT`）/ `noteFieldDepthWrite`（字段、元素） | **必须在提权之后**：`promoteFieldsAt` 会读它，先写进表会让它在同一次调用里看到新条目 |

**2. 迁移的站点**：

| 站点 | 之前 | 现在 |
|---|---|---|
| `check_stmt.c` 赋值路径（`ST_ASSIGN`） | 站内手写 ① + ② + ③ + 一大段 ④（带 `curStoreVal` 的存取与 `valDepthForStore` 取值） | **一次 `noteStore(...)`** |
| `check_stmt.c` 两个返回站点（`e?` 分支与正常路径） | 各自手写 `recordStore(v, v, 0, line)` | 共享的一半收成 **`noteReturnPublish(c, v, line)`**（两站的后续步骤本就不同：一个接 `promoteInto`+`recordLvlFact`，一个接 `checkEscape`） |

### 验收（U3 同样要求行为不变：两种模式都跑）

| 检查 | 结果 |
|---|---|
| 发布构建 | `tests/run.sh` **325 / 0** |
| 断言版（`EXTC_DBG=1`） | **325 / 0**（`[assert]`/`[fallback]` 仍 0） |
| 闸门⑤·语料 | ok（18 份，基线 1 条） |
| `check.sh quick` | **55 / 0** |
| 闸门①checkc 全量 / ②ASan 全量 | 全绿（基线为空） |

### 还没收进来的形状（每个都**不同**，所以不能硬套 `noteStore`）

| 形状 | 为什么不同 | 下一步 |
|---|---|---|
| `unNarrow`（证明失效） | **不是写**，是"写让事实失效"；P0-17 的那一半 | 与 U5 一起，做成"写点失效"的统一入口 |
| `freshDrop`（fresh 集合失效） | 同上，但是**来源事实**的失效（P1-5） | 同上 |
| `recordLvlFact` / `recordLvlRejection`（延迟重放） | 是"留给末轮"的机制，不是当场发布 | 保持独立，在头文件形状清单里注明 |
| `check_top.c` 的出参发布（P0-16d 修的） | 发布到**另一个绑定**（目的实参），不是本站点自己写 | 可以收成 `noteOutParamPublish`，但它的专用槽与"只在可提升时发布"的条件要一起带过去 |
| `check_stmt.c:493`（结构体字面量的字段初始化）与 `722`/`761`（只有字段表那一半） | 只有 ④ 没有 ①：它们的电平记录在别处（字面量的 `origin` 链） | **先查清**它们的 `recordStore` 在哪，再决定是否合流 —— 不能凭"看起来像"就并进正门 |

## U4（本轮完成）：arena / zone 两条通道的调用点决策收成一份（行为不变）

### 做了什么

**一个共享函数**（`check_top.c`，`checkModule` 之前，两处调用都在它内部）：

```c
static int callSiteMinDestDepth(Checker *c, EArenaSite *rec)
```

语义（也写进了它的注释）：返回**目的地有多浅** —— 数越小活得越久；
`overflow`（实参没记全）⇒ 按"活得最久"算（−1，安全方向）；深度 0 的实参本身就让结果是 0；
逃逸集合命中的实参按 −1 算；循环用 `seen` 把"还没见过"与"活得最久"分开，
并在调试构建里 `EXTC_DBG_ASSERT(seen || rec->n == 0)` 看守这个分离。

**两处调用点各自只剩一行**：

| 通道 | 之前 | 现在 |
|---|---|---|
| arena（`want = (nd <= 0) ? ARENA_HOME : nd`） | 站内手写 `seen` + 循环；`overflow` 在前面 `continue` 掉 | `int nd = callSiteMinDestDepth(&c, rec);` |
| zone（`zwant = (nd <= 0) ? ZONE_HOME : nd`） | 站内手写 `overflow` 分支 + **第二份**同样的循环 | 同上（`overflow` 由共享函数内部处理） |

**意义**：P0-2 的哨兵 bug 曾经在**两条通道里各有一份**（修一条不会修另一条）；
从这一轮起，"目的地有多浅"只有一个实现，两条通道之间剩下的差别只有"`<= 0` 时取哪个 HOME"。

### 验收（行为不变：两种模式都跑）

| 检查 | 结果 |
|---|---|
| 发布构建 | `tests/run.sh` **325 / 0** |
| 断言版（`EXTC_DBG=1`） | **325 / 0**（`[assert]`/`[fallback]` 仍 0） |
| 闸门⑤·语料 | ok（18 份，基线 1 条） |
| `check.sh quick` | **55 / 0** |
| 闸门①checkc 全量 / ②ASan 全量 / ③差分 / ④规模 | 全绿（基线为空） |

`callSiteMinDestDepth` 在文件里出现 3 次 = 1 处定义 + 2 处调用 ✓（可用
`grep -c callSiteMinDestDepth src/check_top.c` 复核）。

## U5（本轮完成第一刀）：16 处 `return 0;` 逐条分类 + 哨兵改断言/显式状态

### 分类结果（16 处）

| 类别 | 处数 | 处置 |
|---|---|---|
| **合法语义**：`!e` / `!s` / `!t`（没东西可问） | 6 | 保留 |
| **合法语义**：类型里没有引用（`exprRefDepth` 的提前退出、标量/字面量/`null`） | 4 | 保留（原本就有注释） |
| **另一个轴**：`valDepthStructural` 的非引用绑定 | 1 | 补注释：这里答的是"**结构里有没有引用**"，不是"活多久"，0 是语义不是兜底 |
| **认不出的形状 ⇒ 0**（不安全的那一侧） | 3 | 见下（两处 NOTE、一处 FALLBACK） |
| 查表未命中（`--explain-memory` 的行号） | 2 | 非哨兵（未命中即"没有这一行"），低优先 |

### 改了什么

1. **删掉一处我自己在 U1 留下的死代码**：`exprRefDepthPure` 里那段环检测被写了两遍
   （第一遍带 `EXTC_DBG_FALLBACK`，第二遍是原来那句 `return 0;`）—— 第二次循环永不执行。
2. `exprRefDepthPure` 的 `if (!sy || !sy->origin) return 0;` **拆成两条**：
   `!sy`（ident 没解析过 —— 不该发生）→ `EXTC_DBG_FALLBACK("unresolved ident")`；
   `!sy->origin`（**合法**：参数的 origin 本来就是空的，深度就是 0）→ 保持 `return 0` 并注释。
3. `valDepthStructural` 的 `default`、`solvedValDepth` 的 `default` → `EXTC_DBG_NOTE`
   （"认不出的形状答 0"是不安全那一侧，所以先看清它是不是活路径，再决定要不要动）。

### 实测（这是本轮的判据）

| 布点 | 断言版命中 | 结论 |
|---|---|---|
| `exprRefDepthPure`: 未解析 ident（FALLBACK） | **0** | 真不变量 ✓ 现在被看守 |
| `valDepthStructural`: 认不出的形状（NOTE） | **0** | 真兜底 ✓（现有语料走不到） |
| `solvedValDepth`: 认不出的形状（NOTE） | **178 次 / 套件** | **活路径**，不是兜底 ⇒ NOTE 正确；同时记成**待研究**：认不出就答 0 是"没有引用"这一侧，与 P0-6c 同族，值得单独造探针 |

### 顺带完成的两件小事

* `paramIndex` → **`paramIndexByName`**（旧名残留 0 处），并在定义处写明：
  "**按名字**找参数，只在调用点手上只有名字时才用；有表达式一律用 `paramIndexOfExpr`（身份版）——
  参数遮蔽是合法语言特性，按名字匹配会认错人（P0-6d）"。
* `check.sh` 的断言版一节现在**报出 NOTE 计数**（观察项，不判红）：
  `断言版：… 断言/兜底 0 命中；已知活路径 note×N`。
  计数随套件路径而异，**复核命令**（读数是"行数"，不是"匹配数"）：
  `EXTC_DBG=1 ./tests/run.sh 2>&1 | grep -ac '\[note\]'` —— 本轮末实测 **84**；
  贡献者按大小是 `solvedValDepth: unrecognized shape` 与 `promoteInto2: recursion budget (32)`。

### 验收

| 检查 | 结果 |
|---|---|
| 发布构建 / 断言版 | **325 / 0** / **325 / 0** |
| `check.sh quick` | **55 / 0** |
| 闸门⑤·语料 · ①checkc 全量 · ②ASan 全量 · ③差分 · ④规模 | 全绿（基线为空；语料基线 1 条 = P0-6e） |

## U5 补记（本轮末）：`solvedValDepth` 的"认不出 ⇒ 0"是**活路径**，记成候选第 14 条

### 证据链

1. 三个外部调用者里，`check_top.c:5067` 是**延迟复核**（`runRefCheck`，电平稳定后的最后一道判定），
   另两处是调试转储与记录 ⇒ 这里答 0（"活得最久"）意味着**延迟检查会放行** ⇒ 与 P0-6c 同族。
2. 细分实测：套件里按行计 84 条 `[note]`，其中该兜底贡献最大；把形状名打进 NOTE 后，
   **178 次匹配全部落在"其它"** —— 即命中者**不是** `EX_ENUMVAL`/`EX_ARRAYLIT`/`EX_REF`/`EX_TRY`/
   `EX_METHOD`/`EX_ASSOC`/`EX_EXT`（这些一次都没被走到），而是字面量那类。
3. 把"可能带引用的形状"单列出来布 `EXTC_DBG_FALLBACK` 之后，**断言真的响了 3 次**
   ⇒ 除字面量外，还有 `EX_CONV` / `EX_LAMBDA` 之类**没被定位到**的形状会走到这里。
   （`EXTC_DBG_ASSERT_MSG` 目前不收 varargs，所以那次没能把 kind 数值打出来；
   下一步要么给 `extcDbgNote` 加一个 printf 变体，要么在本地临时打印。）

### 为什么现在**没有**逐个列形状（一条仓库规则，值得记住）

把形状逐个列全，会让 `solvedValDepth` 变成**第 24 个手写遍历**，而本仓库有一条
**walker 预算**：`tools/check_walkers.py` 报 `hand-written walkers: 23 (ratchet 23)`，
`docs/topics/AST-WALKERS.md` 的口径是 **"migrate one instead of adding another"** ⇒
`check.sh quick` 当场从 55/0 掉到 **54/1**。所以这条要么先**迁移一个旧遍历**（用通用
`astWalkExprChildren`）再腾出名额，要么就用通用遍历重写它 —— 这正是 U 批次本身的方向。

### 结论（候选第 14 条）

**`solvedValDepth` 对若干可达形状答 0**（延迟复核路径上的"活得最久"），与 P0-6c 同族。
下一步（下轮）：① 给 NOTE 加 printf 变体，打印 kind 数值定位到具体形状；
② 逐个判定"该形状能不能带引用"；③ 能带的按"深度取自操作数/载荷"修（**行为变更**，
按 R1/R3 的规矩：语料 + ASan + 判据），顺带把 walker 名额通过迁移腾出来。



* **U2 单一指称**：`placeRoot` / `placeRootName` / `placeDepth` / `exprRefDepth` / `identBindOf` /
  `rootSymNoScope` / `paramIndex` / `paramIndexOfExpr`（100+ 调用点）收敛为语义互不重叠的一小组入口，
  每个入口在头文件里写清"回答哪个问题 + 极性契约"；**行为不变**，用 325 测试 + 5 闸门 + 语料验。
* **U3 写点单一入口**：7 种 30 处 → 一个 `noteStore` 正门（P0-17/P1-5 的失效必须仍生效）。
* **U4 arena/zone 两条通道的调用点决策收成一份实现**（P0-2 的哨兵曾在两条里各一份）。
* **U5 哨兵 → 断言/显式状态**：审核 checker 里的 16 处 `return 0`（用 `EXTC_DBG_ASSERT`/`FALLBACK`
  逐条分清"合法语义"与"兜底"），并把 U1 的 NOTE 计数接成可比的观察项。
