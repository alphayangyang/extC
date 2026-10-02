# X 批次：把 AST 从"输入 / 分析结果 / 代码生成计划"三合一里解耦

主人 2026-10-02：**先处理第 1 项**（AST 与"分析产物/计划"解耦），goal 模式。

阶段：X0 盘点 → X1 立接缝（`plan` 访问器收口读点）→ X2 逐族搬家 → X3 实例集显式化 →
X4 收尾（AST 只剩语法事实与身份）。**每阶段行为不变**，验收 = 发布与 `EXTC_DBG=1` 两模式
325 测试 + 五道闸门 + 18 份语料 + walker/guards 全绿。

## X0（本轮完成）：盘点

**产物**：[docs/topics/AST-ANNOTATIONS.md](../docs/topics/AST-ANNOTATIONS.md) ——
字段 → 谁写 → 谁读 → 陈旧会怎样的权威表，按四类分（① 语法事实 / ② 分析缓存 / ③ 编译计划 /
④ 身份与重指）。复核脚本：`tools/x0_annotations.py`（`grep` 式计数，口径写在脚本里）。

**盘出来的三件事**（都比开工前的预期更具体）：

1. **③ 计划族 = 20 个字段**，其中被 codegen 读得最多的是 `func`（58 次，调用点重指到实例）、
   `tmpl`(15)、`isCoro`(14)、`cname`(14)、`coroNeedsZone`(12)、`coroProto`(11)、
   `arenaLevel`(9)、`zoneLevel`(8)、`yieldType`(8)、`usesHome`(8)、`makesPool`(8)、`instName`(8)。
2. **耦合是双向的**（这是本轮最重要的发现）：codegen **反过来写**分析状态 ——
   `substParams`/`substArgs` 各 **8** 次（借用检查器的全局替换状态做实例代码生成）、
   `used` 4 次（影响检查器"没被调用就不必复核"的判断）、`name` 15 次（去重改名）、
   `owSites`/`owLocal` 各 3 次、`body` 3 次、`coroBoxed`/`ret` 各 1 次。
   ⇒ "哪个 pass 先跑"因此变成**语义问题**（P0-5 的根因正是"实例物化＝重指节点"这一副作用）。
3. **半语法半计划的字段**存在：`forStep`（parser 也写）、`condAllocs`（检查器写、codegen 读）——
   搬家时要保留"语法那一半"的归属。

**本轮没有改任何代码**（X0 是纯盘点）。

## X1（本轮完成）：立接缝 —— `plan` 模块 + 棘轮

**产物**

* `src/plan.h` / `src/plan.c`：18 个计划字段的访问器，每个字段在**头文件里**写明
  "谁写 / 何时有效 / 陈旧会怎样"。**存储仍在 AST 上**（所以行为完全不变），
  目的是把 codegen 的读点收口到**一个文件** —— X2 的搬家因此只需改 `plan.c`。
* codegen.c 的读点全部改经访问器：`func` 58 + `tmpl` 15 + 其余 16 个字段 109 处 =
  **182 处**（含"调用基座"的第三遍 8 处）。
* `tools/check_plan_seam.py`：**棘轮** —— codegen 里再出现计划字段的直读即红，
  并提示对应的访问器。已接进 `check.sh`（`quick` 从 55 节变 **56 节**）。
  只判**读**：`x->field = …` 是写点（codegen 回写分析状态属于 X3）。
* **棘轮的负例验证**：临时在 codegen 里插一次 `e->isCoro` 直读 ⇒ 立刻报
  `src/codegen.c:2206 ->isCoro`；还原后复绿（证据在提交前的运行记录里）。

**本轮的三个技术教训**

1. **成员访问链的基座**：`s->u.var.init->func` 的基座是 `s->u.var.init` 而不是 `init` ——
   第一版正则按"最后一个标识符"抓，写出 `s->u.var.planCallee(init)` 这种非法代码
   （编译期才发现，且是**语义错**不是语法错：`planCallee` 被当成成员名）。
   修正：基座正则必须是**链式**的；第一遍跑完还要跑第二遍，因为
   `planCallee(e)->isCoro` 的基座变成了**函数调用**。
2. **字段归属要先查再写**：我按 572 行的 `cname` 猜 `FuncDef.cname`，实际上那是另一个结构体；
   `Expr` 上根本没有 `cname`，`Sym` 也不在 `ast.h` 里（它是检查器概念）。
   ⇒ `planFuncCName` 直接删掉，名字族留到 X2 按**写者**分辨。
3. **收口必须钉住**：访问器本身不会阻止别人继续直读（字段还在），所以接缝需要有棘轮
   —— 这也是为什么 `check_plan_seam.py` 和 `check_walkers.py`/`check_guards.py` 是同一类东西。

## X2 第一步（本轮）：Expr 上的计划字段收进具名子结构 `Expr.plan`

**做了什么**：把 `Expr` 顶层与语法事实混在一起的四个**计划**字段 ——
`arenaLevel`、`zoneLevel`、`arenaArg`、`needTemp` —— 连同各自的注释块搬进
`struct { … } plan;`（`src/ast.h`），全仓 `->field` 引用改为 `->plan.field`（51 处，涉及
`check_escape.c` 12、`check_expr.c` 9、`check_top.c` 26、`plan.c` 4）。

**为什么先做这一步**（而不是直接上侧表）：
1. `Expr.plan` 让"AST 里哪一块**不是**语法"在**类型上**看得见 —— 这正是第 1 项耦合的根；
2. 之后若要真搬到侧表（按节点寻址），**只需要改 `plan.c` 一处**（写入侧表 + 访问器读侧表），
   检查器那 51 个引用可以先不动（它们读的是同一个子结构）；
3. 这一步是**纯搬移**：字段名、类型、初值、语义全不变，验收全绿。

**自检**：仓库里这 4 个名字的其余出现都在**注释**与一处调试 `fprintf` 的格式串里（逐条看过），
没有裸用的活代码。

**仍然待做（X2 第二步）**：把 `Expr.plan` 的**存储**真正移到计划侧表（按 `Expr*` 寻址），
写入经 setter、检查器读取经（检查器侧）访问器，并用 `EXTC_DBG` 断言"侧表与字段一致"
过渡一轮再删字段。四族里其余族（`Stmt.condAllocs`/`forStep`、`FuncDef` 的计划字段、
协程族）同样照此办理。

## X2 第二步（本轮完成）：Expr.plan 的存储搬到计划侧表（过渡态：影子）

**现在的结构**（`src/plan.c`）：
* 一张**按 `Expr*` 寻址**的开放寻址哈希表（`slotFor`/`grow`，键/槽分开存），跨模块存在 ——
  因为驱动是"**先查完所有模块，再逐模块 codegen**"（`main.c`），所以侧表不能按模块重置。
* **setter**（`planSetArenaLevel`/`ZoneLevel`/`ArenaArg`/`NeedTemp`）：过渡期**同时**写侧表与
  `Expr.plan` 字段。17 个写点全部改经 setter（`check_escape.c` 2、`check_expr.c` 6、`check_top.c` 9）。
* **访问器**（codegen 侧）：仍读 `Expr.plan`（权威未变 ⇒ 行为不变），但在 `EXTC_DBG=1` 下比较
  "侧表里应有的值"与字段值，**并且**在"字段非默认值却没有侧表记录"时响亮 —— 那正是
  "某个写点漏了 setter"。影子检查由 `extcDbgOn()` 保护，**发布版零开销**。

**覆盖性证据**：`EXTC_DBG=1` 跑完整套件，影子断言**零命中** ⇒ 四个字段的所有写点都经过 setter，
且侧表与字段处处一致（如果漏一个写点，第一次读它就会响）。

**本轮踩的三个坑（都记下来）**
1. **贪婪正则改坏了代码**：`=\s*([^;]+);` 会匹配 `==` 的第一半，还把 `plan.c` 里 setter 自身的
   `e->plan.arenaLevel = v;` 改成了递归调用 ✗。教训：**批量改写必须先排除 `==`（`=(?!=)`）并跳过
   plan.c**；这次靠 `/tmp/rev/x2b_src_backup` 快照整体回退，才没有留下半改状态。
2. **"恰好一次"的替换会静默漏掉重复文本**：`e->plan.arenaArg = ARENA_HOME;` 在 check_top.c 出现
   2 次，我的 `count == 1` 保护让它**一处都没换**（而不是换错）—— 这是好事，但要有"未命中清单"
   并逐一补（本轮补了 2 处）。
3. **新头文件要先想 include**：三个检查器文件加 setter 调用时忘了 `#include "plan.h"`；
   `dbg.h` 里加 `bool extcDbgOn(void)` 忘了 `<stdbool.h>`，而且 include 放错位置让
   `check_guards.py` 判红（**19 ok / 0 broken** 才是对的：include 必须放在守卫的 `#define` 之后）。

**下一步（X2 第三步）**：把权威翻到侧表 —— 访问器改读侧表、setter 不再写 `Expr.plan`、
然后把 `Expr.plan` 的四个字段从 `ast.h` 删掉（此时 `check_plan_seam.py` 可以升级为
"字段已不存在"的检查）。之后照同样办法办其余族（`Stmt.condAllocs`/`forStep`、
`FuncDef` 的 `usesHome`/`mayUseArena`/`makesPool`、协程族）。

## X2 第三步（本轮完成）：权威翻到侧表，`Expr.plan` 字段删除

**做了什么**
1. 检查器侧的**读点**（31 处：`check_escape.c` 13、`check_top.c` 15、`check_expr.c` 3）也改经访问器；
   余下 2 处基座是 `(*(Expr **)vecAt(...))` 形式（正则覆盖不到），显式手改。
2. `plan.c`：**访问器改读侧表**（未设置时返回旧字段的默认值 0/false）；**setter 不再写 AST 字段**；
   过渡期的"影子看守"整段删除。
3. `ast.h`：`Expr.plan` 子结构**删除**，那个位置留一段注释指向 `plan.c`/`plan.h`
   （"这个节点的编译计划不在这棵 AST 上"）。
4. `tools/check_plan_seam.py` 升级为**两条**检查：① codegen 直读计划字段 0 处；
   ② **AST 上不得再出现已搬走的字段**（`arenaLevel`/`zoneLevel`/`arenaArg`/`needTemp`）——
   这样以后没人能把耦合加回来而不被判红。

**验收（全量）**：tests **325/0**（发布与 `EXTC_DBG=1`）、`check.sh quick` **56/0**、
闸门⑤语料/①checkc 全量/②ASan 全量/③差分/④规模 **全绿**、walker 23 ✓、guards **19 ok / 0 broken**。

**一条诚实的性能说明**：权威翻到侧表之后，发布版的读路径多了一次哈希查找。
闸门④（规模）**只判结论、判不判耗时**，所以它通过**不能**证明没有性能影响；
本轮**没有**做前后耗时对比（旧二进制已被覆盖）。若以后关注编译速度，
可用 `tests/stl` 或 `bench/` 计时复测；真要优化，办法是把"计划槽位下标"缓存在节点上
（但那等于把字段加回来，需要先想清楚代价 —— 记在这里，不做）。

**下一步（X2 第四步）**：其余字段族照同一套路 —— `Stmt.condAllocs`/`forStep`、
`FuncDef` 的 `usesHome`/`mayUseArena`/`makesPool`、协程族
（`isCoro`/`yieldType`/`coroFrameType`/`coroNeedsZone`/`coroProto`/`coroBoxed`），
然后 `func`/`tmpl`/`instName`（X3 的实例集）。

## X2 第四步（本轮完成）：FuncDef 的 arena/pool 族 + Stmt.condAllocs 搬进侧表

**搬走的字段**：`FuncDef.usesHome`、`FuncDef.mayUseArena`、`FuncDef.makesPool`、`Stmt.condAllocs`
（写点 10 处：`check_top.c` 6、`check_expr.c` 3、`check_stmt.c` 1；读点 16 处转访问器）。
侧表加了对应记录位（`PLAN_USES_HOME` / `PLAN_MAY_USE_ARENA` / `PLAN_MAKES_POOL` /
`PLAN_COND_ALLOCS`），setter/访问器各 4 个，读经 `planUsesHome`/`planMayUseArena`/
`planMakesPool`/`planCondAllocs`（未设置 ⇒ `false`）。

**`forStep` 有意留在 AST 上**：它是**半语法**字段（`parser.c` 两处写：`loopBody->forStep = step;`
与 `continue` 的 label 目标），X0 的分类表里就标了"半语法半计划"。
把它搬走会把"语法那一半"也拖进计划侧 —— 那不是解耦，是把耦合换个方向。**理由记在这里**。

**顺带解决的一处结构问题**：`ast.h` 里两个 inline 帮助函数
（`funcTakesHomeArena`/`funcTakesHomeZone`）读的正是这两个字段，而 `ast.h` **不能** include
`plan.h`（反向依赖）。做法：在这两个函数前**前向声明**两个 plan 访问器，并写明
"这两个问题是计划问题，答案在 plan.c；若将来需要第三个调用者，就把这两个帮助函数搬进 plan.h，
而不是把这份声明列表变长"。`funcTakesHomeZone` 那段注释里的 **review F9 已知分歧**原样保留。

**本轮踩的坑（第 N 次同类，新增一条规则）**：按"向上找注释块"的方式删字段，会把
**别处注释的尾巴**一起切掉（`ast.h` 当场语法错：`stray '`'`）。改为**按行号区间删**、
**从后往前删**（避免行号漂移），并且只删"字段行 + 紧邻它的注释块"。
—— 头文件里的成块删除，优先用行号手术，不要用"模式 + 向上回溯"。

**验收（全量）**：tests **325/0**（发布与 `EXTC_DBG=1`，断言/兜底 0）、`check.sh quick` **56/0**、
闸门⑤语料/①checkc 全量/②ASan 全量/③差分/④规模 **全绿**、walker 23 ✓、guards 19 ok / 0 broken、
`[plan-seam] ok`（AST 上已无这 8 个字段）。

**下一步（X2 第五步）**：协程族（`isCoro`/`yieldType`/`coroFrameType`/`coroNeedsZone`/`coroProto`；
`coroBoxed` 在 codegen 里被写，属 X3），然后 X3 的实例集（`func`/`tmpl`/`instName`）。

## X2 第五步（本轮完成）：协程族搬进侧表

**搬走的字段**：`FuncDef.isCoro`、`yieldType`、`coroFrameType`、`coroNeedsZone`、`coroProto`
（写点 12 处全在 `check_top.c`；读点 32 处转访问器 —— `check_top.c` 19、`check_stmt.c` 9、
`check_expr.c` 2、`check.c` 1、`parser.c` 1）。侧表加五个记录位，setter/访问器各 5 个。

**`coroBoxed` 有意不搬**：它有两个写者，其中一个是 **codegen**（`codegen.c:6986`；另一个是 `check.c:455`）。
在切断 codegen 的回写（X3）之前搬它，只会让"谁在什么时候写"更难看清。

**本轮发现（待查，未修）**：`parser.c` 里有一处读 `isCoro` —— 那是 `@export` 的检查
（"协程的值是帧句柄，不是 C 能调的函数"）。但 `isCoro` 的**写点全在检查器**（`check_top.c`），
解析期它恒为 `false` ⇒ **这条诊断很可能永远不会触发**（即 `@export` 加在协程上会被静默接受）。
本轮只把它从"直读字段"改成"读计划"（行为完全不变），**没有**去改语义。
要证实需要一个能触发它的最小程序，而我没找到 extC 协程声明的正确写法就停了 ——
探针 `@export coro fn …` 被判语法错（`expected fn, struct, type…, found coro`）。
**下一步**：从 `examples/task-place-release.extc` 里抄到正确拼写，再判这条诊断是死是活。

**验收**：tests **325/0**（发布与 `EXTC_DBG=1`）、`check.sh quick` **56/0**、
闸门⑤语料/②ASan 全量 **全绿**、`[plan-seam] ok`（AST 上已无 **13** 个字段）。

## X3 第一步（本轮完成）：回写棘轮（把反向耦合成"只能减不能增"）

**做了什么**：`tools/check_plan_seam.py` 增加第三项检查 —— **codegen 回写分析状态的基线**：

```
[plan-seam] ok：codegen 直读计划字段 0 处（18 个字段）；AST 上已无 …13 个字段；
            codegen 回写 47 处（基线 47，X3 目标 0）
```

基线按字段记：`substParams` 8、`substArgs` 8、`name` 15、`used` 4、`owSites`/`owLocal`/`body`
各 3、`coroBoxed`/`ret`/`func` 各 1。**超过基线即红**；减少了会打一条 note 提醒收紧基线。
（按字段计数而不是按行号，所以对代码移动是稳的。）

**为什么先做这一步**：X0 的第 3 节把"codegen 反过来写分析状态"列为本批次最危险的一半 ——
`substParams`/`substArgs` 是**借用检查器的替换状态**、`used` 会让"没被调用就不必复核"的判断
依赖 codegen 的遍历顺序。先把它变成**可度量、只能减**的量，再逐处切断（X3 的后续步骤）。

## 顺带查实一条上一轮记的待查项：`@export` + 协程

用正确的协程写法（`fn f(...) -> coroutine<T>`）造了最小程序：

```extc
@export
fn tick(n: i64) -> coroutine<i64> { yield n; return n }
```

**结果：被拒绝**，但用的是 **C-ABI 那条规则** ——
"``@export` returns `coroutine<i64>`, which C cannot return`"（`ttCrossesC`），
**不是** `parser.c` 里那条协程专用消息（"A coroutine's value is a frame handle driven by a task…"）。
⇒ 结论：**结果是正确的（拒绝）**，但 `parser.c` 那条专用诊断**很可能依然永远不会触发**
（解析期 `isCoro` 恒为 false）。它不是洞，是一条**死消息**；要彻底确认需要找到能绕过 C-ABI
检查的路径（例如协程藏在别的返回形状里），留作观察项。

## X3 第二步（本轮）：一处**误判的更正** —— `substParams`/`substArgs` 不是回写

**怎么发现的**：按计划先做只读侦察，第一步就是核实 X0 表里"codegen 借用检查器的替换状态"
这条。查声明归属时发现：`substParams`/`substArgs` 有**两处同名不同属**的声明 ——

| 声明位置 | 属于谁 | 谁在用 |
|---|---|---|
| `codegen.c:146` | **`CG`（codegen 自己的状态结构）** | `g->substParams`（8 处写、若干读）—— 发实例代码时自己的替换上下文 |
| `check_internal.h:365` | `Checker` | 检查器复核实例时自己用（`c.substParams`，`check_top.c`/`check_escape.c`/`check.c`） |

⇒ X0 的计数脚本按 `->字段` 跨结构体统计，把两者算在了一起：**47 里有 16 处根本不是耦合**。

**更正**：
* `tools/check_plan_seam.py` 的回写基线从 **47 改到 31**（去掉这两个字段），并把更正理由写在
  基线表的注释里（含"归属必须按声明所在的**结构体**确认"这条方法教训）；
* 顺带删掉棘轮里我上一版留下的、针对这两个名字的**硬编码检查**（它在新基线下一跑就误报：
  `substParams: 现在 1，基线 0`）—— 这正是"棘轮本身也要被验收"的例子；
* `docs/topics/AST-ANNOTATIONS.md` 第 3 节相应行改为更正说明。

**真实的回写清单（31 处，X3 目标 0）**：`name` 15、`used` 4、`owSites` 3、`owLocal` 3、
`body` 3、`coroBoxed` 1、`ret` 1、`func` 1。

**验收**：tests **325/0**、`check.sh quick` **56/0**、`[plan-seam] ok`（回写 31 = 基线 31）。

## X3 第三步（本轮完成）：逐条复核 31 处"回写" —— 真的只有 **1** 处

按计划做只读侦察的第二半：把 31 处回写**逐条**打开看（不是再数一遍）。结果又推翻了自己的计数：

| 字段 | 原记 | 实际 | 误报原因 |
|---|---|---|---|
| `name` | 15 | 0 | `d->name`/`df->name`/`idd->name` 的基座是 **codegen 自己的描述符结构**，不是 AST 节点 |
| `used` | 4 | 0 | 1 处在**注释**里、3 处在**生成 C 的字符串字面量**里（`a->top->used` 是生成物自己的字段） |
| `owSites`/`owLocal` | 6 | 0 | `g->owSites`/`g->owLocal` ⇒ **`CG` 自己的状态**（与上一步 `substParams` 同一类错） |
| `body` | 3 | 0 | `s->body`/`df->body` 同样是 codegen 描述符 |
| `ret` | 1 | 0 | 在 `cgLine(g, "%s->ret = %s;", …)` 的**字符串**里 |
| `func` | 1 | 0 | 在**注释**里 |
| **`coroBoxed`** | 1 | **1** | `cf->coroBoxed = true;`（`cf` 是 `FuncDef`）——**唯一真回写** |

**更正**：棘轮基线 **31 → 1**（只留 `coroBoxed`），并把"哪些是误报、为什么"写进基线表注释；
`AST-ANNOTATIONS.md` 第 3 节按逐条复核重写。

**方法教训（本批次第三次同类）**：`grep ->字段` 的统计**只能用来找候选** —— 它
① 分不清同名不同属（`CG.substParams` vs `Checker.substParams`）、
② 分不清"基座是不是 AST 节点"（`d->name`）、
③ 不会跳过字符串字面量与注释（`a->top->used`、`e->func`）。
**落表前必须逐条看**。这条已写进基线表注释与表正文。

**对 X3 的影响（重要）**：原以为"切断 codegen 回写"是一项大工程，实际只有 1 处；
而 `coroBoxed` 这一处**在当前驱动顺序下是无害的**（先查完所有模块、再逐模块 codegen ⇒
检查器看不到 codegen 的写），但它**依赖驱动顺序**，已记为 X4 的收尾项。
⇒ X3 剩余的重头戏是**实例集显式化**（`func`/`tmpl`/`instName` 搬到计划侧 + codegen 前封闭 +
显式 worklist），与"回写"无关。

**验收**：tests **325/0**、`check.sh quick` **56/0**、`[plan-seam] ok`（回写 1 = 基线 1）。

---

## 交班（2026-10-02）：X 批次在干净检查点停下

**为什么停**：本会话上下文预算已用尽。X3 剩下的**实例集显式化**需要通读并改动
`codegen.c`（8.8k 行）与 `check_top.c` 的不动点；在预算见底时开工的风险是留下半改状态
（本批次已出现两次，都靠快照救回）。当前树**干净、全绿、已提交**。

### 已完成（都可复核）

| 步 | 内容 | 判据 |
|---|---|---|
| X0 | 盘点表 → `docs/topics/AST-ANNOTATIONS.md`（字段/写者/读者/陈旧后果 + 四类归属） | 表在仓库里 |
| X1 | `src/plan.h`/`plan.c` 立接缝；codegen **182 个读点**收口；棘轮 `tools/check_plan_seam.py` 接进 `check.sh` | `[plan-seam] ok`；quick 55→56 |
| X2 | **13 个字段搬离 AST**：`arenaLevel`/`zoneLevel`/`arenaArg`/`needTemp`、`usesHome`/`mayUseArena`/`makesPool`/`condAllocs`、`isCoro`/`yieldType`/`coroFrameType`/`coroNeedsZone`/`coroProto` | 棘轮第二条检查（AST 上不得再出现这些字段）|
| X3-1 | 回写棘轮（按字段基线，只能减不能增） | 每次跑都报出规模 |
| X3-2 | 更正：`substParams`/`substArgs` 属 `CG` 自己 ⇒ 基线 47→31 | 表与注释都改了 |
| X3-3 | 逐条复核 31 处 ⇒ **真回写只有 `coroBoxed` 1 处** ⇒ 基线 31→1 | 棘轮报 `回写 1 处（基线 1）` |

### 未完成（已定界，下一会话可直接开工）

1. **X3 的实例集显式化**（本批次剩下的大头）：
   * `Expr.func`：**18 个写点全在检查器**（`e->func = instance` 的重指）、85 个检查器读、
     58 个 codegen 读（后者已在 X1 收口）；
   * `FuncDef.tmpl`：2 写、53 读（**注意**：`Type` 上也有一个同名的 `tmpl`，见下）；
   * `FuncDef.instName`：1 写、11 读；
   * 目标：物化唯一入口 + **实例集在 codegen 之前封闭** + `checkModule` 里那段不动点写成**显式 worklist**。
2. **X4 收尾**：AST 只剩语法事实与身份；`forStep`（半语法，**有意保留**，理由已记）；
   `coroBoxed`（唯一真回写，当前驱动顺序下无害但**依赖驱动顺序** ⇒ 要么切断、要么把顺序写进契约）；
   更新 `check_internal.h` 的权威节与 `AST-ANNOTATIONS.md`。

### 下一会话开工前必读的两条（本批次用代价换来的）

1. **`grep ->字段` 只能找候选，落表前必须逐条看** —— 本批次它错了四次：
   ① 同名不同属（`CG.substParams` vs `Checker.substParams`）；
   ② 基座不是 AST 节点（`d->name` 是 codegen 描述符）；
   ③ 字符串字面量与注释（`a->top->used`、`e->func`）；
   ④ **`Type.tmpl` 与 `FuncDef.tmpl` 同名**（本轮实测：按名字迁移会把 `Type` 的也改掉，
      编译期报 `incompatible pointer type` ⇒ 迁移前必须先按**声明所在结构体**确认基座类型）。
2. **成块删字段用行号区间、从后往前**（"模式 + 向上回溯注释"会把别处注释的尾巴切掉，
   当场语法错）；每次动手前 `cp -r src /tmp/...` 打快照，坏了一键回退。

## X3 第四步（本轮完成）：切断唯一那处真回写（`coroBoxed`）

**做了什么**：`codegen.c` 的协程 handle prepass 里原来是

```c
if (planTemplate(cf) && planCoroBoxed(planTemplate(cf))) cf->coroBoxed = true;   /* 写进实例 */
cf->coroKind = ck++;
if (planCoroBoxed(cf)) g.needCoroHandle = true;                                  /* 下一行又读出来 */
```

⇒ 把"模板的标记"**写回实例的 `FuncDef`**，再在下一行读出来。改成就地推导：

```c
bool boxed = planCoroBoxed(cf) || (planTemplate(cf) && planCoroBoxed(planTemplate(cf)));
```

语义等价（实例自己已有的标记仍然算数），但**少了一条"谁在什么时候写"的隐式依赖**。
实测：codegen 里没有别处读实例的这个字段（grep `coroBoxed` 只剩注释与访问器）。

**再按"逐条看"量了一遍剩余回写**（这次把**基座**也看）：`g->owSites`/`g->owLocal`/`g->coroFrame`
是 **`CG` 自己的状态**（我的脚本按字段名把它们错标成 `ast.FuncDef` —— **第四次**印证
"按名字归属不可靠"）；注释行、生成 C 的字符串字面量也都不算。
**真正剩下的只有 `coroKind` 1 处**（`cf->coroKind = ck++`，`cf` 是 `FuncDef *`）——
它是 codegen 自己的 prepass 下标，且 `grep` 显示**只有 codegen 用它**（检查器不读）。

**棘轮基线 → `{coroKind: 1}`**，并在注释里写清"`coroBoxed` 已切断、这一处为何保留"。

**验收**：tests **325/0**（发布与 `EXTC_DBG=1`）、`check.sh quick` **56/0**（含 coro 套件）、
`[plan-seam] ok`。

## X3 第五步（本轮·**失败并回退**）：把 `coroKind` 搬进 `CG` 不可行

**想法**：`coroKind` 只有 codegen 读写（`grep` 全仓只有 `ast.h` 的声明与 `codegen.c` 的使用），
于是把它从 `FuncDef` 搬进 codegen 自己的状态（`CG.coroIdx` 表 + `coroKindOf(g, f)` 查询），
目标是让回写基线变 **0**。

**结果**：**5 个用例红**（`coro*` 相关），`check.sh quick` 从 56/0 掉到 **48/5**。
原因很清楚：读点（`codegen.c:2496`/`3966`）在 **prepass 建表之前**就会执行
⇒ `coroKindOf` 返回 −1 ⇒ 生成物里出现 `__extc_czh-1`。也就是说
**这处"回写"承担的是"发射顺序"的传递**，不是可以随手搬走的缓存。

**处理**：从快照整体回退（`cp -r /tmp/rev/x3c_src_backup src` + `git checkout -- tools/check_plan_seam.py`
—— 我的补丁还把棘轮脚本改出了语法错，一并还原）。回退后：tests **325/0**（发布与 `EXTC_DBG=1`）、
`check.sh quick` **56/0**、`[plan-seam] ok`、`git status` 只剩 `?? proofs/`（另一个会话的在制品）。

**留给 X4 的结论**：`coroKind` 这 1 处要么保留（并把"prepass 必须先于哪些读点"写进契约），
要么把 prepass 提前到所有读点之前 —— **不能**只换存储位置。这正是本批次反复学到的同一课：
搬之前先看清**读写顺序**，而不只是"谁引用它"。

## X4 第一半（本轮完成）：把「未迁移/不宜迁移」写进权威处（目标原文的义务）

**做了什么**（纯文档，零代码风险，但仍跑全量验收）：

1. `docs/topics/AST-ANNOTATIONS.md` 新增**第 5 节**：逐字段写明"为什么还在 AST 上 / 代价与下一步" ——
   `Expr.func`（等实例集三件事一起做；P0-5 的根因）、`FuncDef.tmpl`（**`Type.tmpl` 同名陷阱**，
   迁移前必须按声明所在结构体逐点确认）、`instName`、`Stmt.forStep`（半语法，**有意保留**）、
   `FuncDef.coroKind`（**顺序**依赖，不能只换存储位置）、以及 ② 分析缓存族（codegen 读点 0，
   不构成"AST 充当计划"的耦合，若要继续解耦应搬进与 plan 平行的分析侧结构）。
2. `src/check_internal.h` 的权威节前加一段：**"编译计划不在这棵 AST 上"** —— 计划在
   `src/plan.c`/`plan.h`，codegen 只经访问器读，棘轮钉住三件事；并列出**已搬走**与**尚未搬**
   的字段清单（后者指向第 5 节）。

**验收**：构建零告警、tests **325/0**、`check.sh quick` **56/0**、`[plan-seam] ok`。

**X 批次至此的状态**：X0 ✅、X1 ✅、X2 ✅（13 个字段）、X3 ✅（回写侧：棘轮 + 逐条复核 + 切断
`coroBoxed`；`coroKind` 一次失败尝试已回退并记档）、X4 ◐（文档那一半完成；剩下的是
**实例集显式化**本身 —— `Expr.func`/`tmpl`/`instName` 的搬迁与"codegen 前封闭 + 显式 worklist"，
需要通读 `codegen.c`，留给预算充足的会话）。
