# X 批次：把 AST 从"输入 / 分析结果 / 代码生成计划"三合一里解耦

主人 2026-10-02：**先处理第 1 项**（AST 与"分析产物/计划"解耦），goal 模式。

阶段：X0 盘点 → X1 立接缝（`plan` 访问器收口读点）→ X2 逐族搬家 → X3 实例集显式化 →
X4 收尾（AST 只剩语法事实与身份）。**每阶段行为不变**，验收 = 发布与 `EXTC_DBG=1` 两模式
325 测试 + 五道闸门 + 18 份语料 + walker/guards 全绿。

## X0（本轮完成）：盘点

**产物**：[docs/topics/AST-ANNOTATIONS.md](../docs/topics/AST-ANNOTATIONS.md) ——
字段 → 谁写 → 谁读 → 陈旧会怎样的权威表，按四类分（① 语法事实 / ② 分析缓存 / ③ 编译计划 /
④ 身份与重指）。复核脚本 `tools/x0_annotations.py` 已于 2026-10-02 **删除**：实测它的口径会产生大规模误报（把 `->name`/`->body` 这类**任何结构体都有的成员名**算成同一批字段、把基座是 codegen 自己结构体/局部变量的访问也算进去，还不剥注释与字符串字面量）——正是它当初把回写基线报成 47 的那一类错。可信的口径只有按**声明所在结构体**逐点归属的`tools/check_tmpl_owners.py`；下面各条都按它的口径重新量过。

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

## X3 第六步（本轮完成）：`FuncDef.instName` 搬进侧表（**14 个字段**已离开 AST）

**做法（含前置条件自检）**：脚本先检查"`instName` 恰好一处声明、且属于 `FuncDef`" ——
结果 `[(680, 'FuncDef')]` ✓ 才动手；不满足就**什么都不改**（这条自检是上轮 `tmpl` 踩坑后加的：
`tmpl` 在 `Type` 上也有，按名字迁移会连 `Type` 的一起改）。

**改动**：`plan.c` 加记录位 `PLAN_INST_NAME`、槽位成员、setter，访问器改读侧表；
`plan.h` 加声明（并注明 `tmpl` **不在此列**及其理由）；读点 11 处、写点 1 处改经访问器/setter
（全在 `check_top.c`）；`ast.h` 删字段；棘轮 `MOVED` 加 `instName`。

**验收**：构建零错误、tests **325/0**（发布与 `EXTC_DBG=1`）、`check.sh quick` **56/0**、
`[plan-seam] ok` —— AST 上已无 **14** 个字段（…/coroProto/instName）。

**X3 实例族还剩两件**：`FuncDef.tmpl`（53 读，**须先处理与 `Type.tmpl` 的同名**）与
`Expr.func`（18 写 + 85 检查器读；搬它必须与"物化唯一入口 + codegen 前封闭 + 显式 worklist"
一起做，否则只是把重指换了个地方）。

## X3 第七步（本轮·**失败并回退**）：消同名（`tmpl`）的两次尝试都不成立

**目标**：我交班笔记里写的下一步是"先解决 `FuncDef.tmpl` 与 `Type.tmpl` 的同名"。本轮发现
**笔记本身是错的**：`Type.tmpl` **根本不存在**（`grep` + 编译错误都证明）——真正的第三个同名者是
**`CallCheck.tmpl`**（声明在 `check_internal.h`，不在 `ast.h`）。
这已经是本批次**第五次**同一类错误（按名字猜归属），说明这条规则必须当成硬约束执行。

**尝试 1（编译器驱动重命名）**：把 `FuncDef.tmpl` 改名，然后"编译器报哪行就改哪行"，
自动改了 **41 处**（6 轮循环）——**看似成功**。
**尝试 2**：继续处理 `CallCheck.tmpl` 时暴露了方法的根本缺陷：
**旧名字同时属于两个结构体**（`FuncDef` 与 `CallCheck`），于是"编译器报的每一行"都还必须先判
**基座是哪个结构体**——这正是编译器驱动想绕开、却绕不开的那一步。
结果：`check_expr.c:3227` 的 `CallCheck` 点被错改成 `templateDef`，`check_top.c:5202/5835` 又因
另一处改名而报 `CallCheck has no member named 'tmpl'` ⇒ **构建失败**。

**处理**：`cp -r /tmp/rev/x3e_src_backup src` 整体回退（含 41 处自动改名一起丢弃）。
回退后：tests **325/0**（发布与 `EXTC_DBG=1`）、`check.sh quick` **56/0**、`[plan-seam] ok`、
`git status` 只剩 `?? proofs/`。

**结论 / 留给下一会话**：
1. 消同名**必须先做静态归属**（按**声明所在结构体**逐个基座确认），**不能**靠编译器报错反推 ——
   当旧名字属于 ≥2 个结构体时，"报错即该改"是错的；
2. 更省事的正路：**先给两个字段起不同名**（一次纯机械改名，每个基座用"类型注解/函数签名"判断），
   或者接受它们同名并**只在迁移时逐点判定**；
3. `Type.tmpl` 这个说法从交班笔记里**删掉**，改为 `CallCheck.tmpl`（声明 `check_internal.h`）。

## X3 第八步（本轮完成）：`tmpl` 的静态归属 —— 用脚本判，不再靠数数

**目标**（上一轮的两条结论之一）：消同名/迁移之前**必须先做静态归属**，即"每个基座到底是
哪个结构体"，且这一步不能靠编译器报错反推。

**产物**：`tools/check_tmpl_owners.py`（报告 + `--verify` 一行结论，**已接进 `check.sh`**）。
做法：① 读每个 `struct` 定义拿到**成员表**（唯一真源：哪些结构体声明了 `tmpl`）；
② 找出每个 `base.tmpl` / `base->tmpl` 访问点；③ 找回宿主函数，用"参数/局部声明、`(Struct *)` 强转、
返回该结构体的调用"三条规则定基座；④ **未定的一律打 UNKNOWN 而不是猜**。

**实测结论**（全仓 71 个 token / **43 个字段访问点，全部有属主，0 个 UNKNOWN**）：

| 属主 | 站点 | 写 | 读 |
|---|---|---|---|
| `FuncDef`（实例 → 模板，`ast.h:679`） | **40** | `check_top.c:4747`（`funcInstance` 唯一入口） | 39（`check_top.c` 36 / `check_expr.c` 2 / `check_lookup.c` 1 / `plan.c` 访问器 1） |
| `CallCheck`（延迟调用点，`check_internal.h:664`） | **3** | `check_expr.c:3227` | `check_top.c:5202`、`5835` |
| codegen | **0** | — | 14 个读点**全部**经 `planTemplate` |

**判据的负例已验**：临时在 `codegen.c` 加一行 `f->tmpl` 直读 ⇒ 当场报
`codegen reads `tmpl` directly at codegen.c:8794 (use planTemplate())`；还原后复绿。

**工具本身的三次翻车（都记下来，因为都是同一类错）**：
1. 按"闭合花括号后跟名字"认 `struct` 名 ⇒ 把**匿名 typedef struct**（`typedef struct { … } X;`）
   的成员算到了别的结构体上，于是 `FuncDef` 一度**根本不在**"声明了 `tmpl` 的结构体"里；
2. 参数表用 `find(")", …)` 取 ⇒ 在 `FuncDef *fi)` 这种嵌套括号里**截断**，导致
   12 个明明能判的站点被标成 UNKNOWN（一条 `toml` 式的假阴性）；
3. 一条正则同时匹配"字段访问"与"字段自身作基座"⇒ 同一位置产出两个站点、`UNKNOWN` 翻倍。
   **教训与主批次同一条**：归属/计数的脚本，**每个数字都要能被逐点复核**，不能只看总数对不对。

**同时查实的顺序问题（决定能不能搬）**：驱动是**严格两段** —— `main.c` 先
`checkModule`（根 + 各模块体）并渲染完所有错误，**然后**才 `generateC`；而 `funcInstance`
在 `codegen.c`/`main.c` 里**零调用点** ⇒ **codegen 期间不会新造实例**。
⇒ `FuncDef.tmpl` 的顺序风险**低于** `coroKind`（后者读点跑在 prepass 之前），
可以照 `instName` 的办法搬进侧表（记录位 + setter/访问器 + 棘轮 `MOVED` 加一项）。
这也**推翻了第 5 节**把 `tmpl` 与 `Expr.func` 并成"等实例集一起做"的粗判：两者可以分开搬。

**写入权威处**：`docs/topics/AST-ANNOTATIONS.md` 新增第 6 节（结论表 + 判据 + 对下一步的意义）。

## X4 第二半（本轮完成）：`plan.h` / `plan.c` 的过时状态与旧名字残留

**出发点**（我开工前逐点核实的结果）：这两个文件是 X1/X2 新建的，注释还停在**当时的中间态**，
而 `COMMENT-STYLE.md` 第 1 条要求 `src/` 注释**英文、ASCII**（仓库里 347 处中文注释是**存量**，
`tools/scan_cjk.py` 一直红着，没有闸门）—— 于是"改准确"与"改合规"一次做完：

| 位置 | 原来（过时/错） | 现在（按代码实况） |
|---|---|---|
| `plan.h` 头 | "X1：只有接缝，没有搬家"、codegen 读点收口 | 已搬 14 个字段；侧表按节点寻址；仍留在 AST 的四个字段与原因；两条调用方规则 |
| `plan.h:99` / `plan.c:24` | "`Type.tmpl` 同名"（已被上一轮推翻） | "**两个结构体**有同名成员（`FuncDef`/`CallCheck`），迁移前按**声明所在结构体**确认"（本轮复核：`check_internal.h:1051` 那个 `FuncDef *tmpl` 是**函数形参**，不是第三个属主；工具只认"结构体成员+访问点"） |
| `plan.c` 头 | "X2 第二步过渡态：setter 同时写侧表与 AST 字段" | 权威已在侧表、AST 字段已删；槽位带 setMask（未设 = 默认值，不读 calloc 残留） |
| `plan.h:71` 三连注释 | "被调者是否用 home arena"（把 `usesHome` 与 `needsHome` 混为一谈） | 逐条分清：`usesHome`=这个函数**收**调用者的 home；`needsHome`=本体**够到**需要 home 的东西；`mayUseArena`=本体有没有自己的分配（省 arena 数组的优化） |
| `plan.c:90` | "X2 第三步：权威在侧表；AST 字段已删除" | 删（那句话属于提交历史，不属于代码） |

**验收（纯注释，行为不变）**：
* `python3 tools/comment_neutral.py HEAD src/plan.h src/plan.c` ⇒ **两文件都是 comments only**（同一判据，`grep` 不算）；
* `python3 tools/scan_cjk.py src/plan.h src/plan.c` ⇒ **0**（这两文件从此合规）；
* 构建零告警；tests **325/0**（发布与 `EXTC_DBG=1`，断言/兜底 **0** 命中，note ×84 与改前一致）；
* `check.sh quick` **56/0**；`[plan-seam] ok`；`--verify` ⇒ `[tmpl-owners] ok`。

**方法论**：一个文件被改过很多轮之后，**头部状态节是最先腐烂的注释**（它描述"整体进展"，
却随每次提交变旧）。本轮的判据是"**每句话都能指到当前代码**"：指不到就删或改写，
不保留任何"当时如此"的叙述 —— 进展留在 `.audit/*-LOG.md`，代码只写现在。

## 本轮收口：完整验收（提交 `2425ab1`）

| 检查 | 结果 |
|---|---|
| `./check.sh`（完整模式） | **68 节通过 / 0 失败**（新增 `tmpl-owners` 一节，quick 56 → 57） |
| `tests/run.sh` 发布 / `EXTC_DBG=1` | **325 / 0** / **325 / 0**；断言/兜底 **0** 命中；note ×84 |
| 五道闸门 | ④规模 13 例 ✓ · ⑤逃逸语料 18 份（接受 1 = 对照）✓ · ③差分 21 例 ✓ · ①checkc 976 文件（gcc+clang）✓ · ②ASan 217 正例 ✓ |
| 接缝与归属 | `[plan-seam] ok`（直读 0、AST 无 14 字段、回写 1=基线）；`[tmpl-owners] ok`（43 站点全有属主） |
| 纯注释判据 | `comment_neutral.py HEAD` ⇒ plan.h/plan.c **comments only**；`scan_cjk.py` ⇒ 0 |

> 过程里踩到的一个**验收陷阱**（值得记）：我为了省时间让 `check.sh` 与另一个
> `tests/run.sh` **并行**跑，结果发布版套件报了 5 条"编译/运行失败"（含 `borrowing`、
> `c-abi-tour` 这类稳定用例），单独重跑立刻 **325/0** —— 是并行争用，不是真红。
> **规则：全量验收不许与别的重活并行**；出现"从没红过的用例成片红"时，先怀疑环境。

## 下一步（已量清，可直接开工）：`FuncDef.tmpl` 搬进侧表

判据、站点表与顺序结论都在 `docs/topics/AST-ANNOTATIONS.md` 第 6 节。要做的三件事：
1. `plan.c` 加 `PLAN_TEMPLATE` 记录位 + 槽位成员 + `planSetTemplate`，`planTemplate` 改读侧表、
   未设置返回 NULL（与 `instName` 同形，可照抄）；
2. 唯一写点 `check_top.c:4747`（`in->tmpl = tmpl`）改经 setter；`ast.h:679` 删字段；
3. `tools/check_plan_seam.py` 的 `MOVED` 加 `"tmpl"`（至此 AST 上 15 个字段已搬）。

**注意两条**（本轮量出来的）：
* `*in = *tmpl;` 是**结构体整体拷贝**，它把 `tmpl` 也复制了一份；因为紧接着那一行就是
  `in->tmpl = tmpl;`，所以搬走字段不会漏写 —— 但**别把这两行的顺序改掉**（若将来删掉显式那行，
  必须同时给侧表补写）。
* `CallCheck.tmpl`（`check_internal.h:664`）**不搬**：codegen 读点 0，属检查器内部；
  若要消同名，那是**改名**的事，与迁移分开做。

## X4 第二半（续：本轮完成）：`FuncDef.tmpl` 搬进侧表 —— AST 上已无 **15** 个字段

**按上一条"下一步"执行，三处改动 + 一次批改**：

| 步骤 | 结果 |
|---|---|
| `plan.c` 加 `PLAN_TEMPLATE` + 槽位成员 `FuncDef *tmpl` + `planSetTemplate`；`planTemplate` 改读侧表（未设置 ⇒ NULL） | 与 `instName` 同形 |
| 唯一写点改经 setter | `check_top.c` `funcInstance`：`planSetTemplate(in, tmpl)`（仍在 `*in = *tmpl` 之后 —— 整体拷贝已不再带走这个指针） |
| 读点改经访问器 | `check_top.c` 22 处 + `check_lookup.c` 1 处（后者补 `#include "plan.h"`）；`check_expr.c` 1 处（`cc->tmpl = planTemplate(f) ? … : f`） |
| 删字段 + 注释 | `ast.h:679` 的 `FuncDef *tmpl` 删除，那段注释改为"反向指针在 plan 侧" |
| 棘轮 | `check_plan_seam.py` 的 `MOVED` 加 `"tmpl"` |

**顺序的价值（本轮最值得记的一条）**：字段**最后删**。删掉之前，编译器把每一个漏改的直读
**逐条点出来**（`check_expr.c:3227` 的两处 `f->tmpl`、`check_top.c` 的批改遗漏都是这样抓到的），
比棘轮更硬、比 grep 更准 —— 而 `grep` 在本批次已经错了五次。**做法固定为：先改读写两侧，
构建到零错误，再删字段。**

**顺带的三处清理**（都在改动路径上，不是顺手扩张）：
1. `refCheckApplies` / `internLocalTypes` / 协程实例那一段里 `planTemplate(fi)` 一行调三次 ⇒
   改成局部变量 `tmpl`（一次查找，顺带把行宽压回 100 列内）；
2. `EXTC_DBG_HOME` 的调试输出里嵌了三层 `planTemplate` 三元式 ⇒ 提成 `ftName` 一行；
3. 两处**注释里点名了已删字段**（`fx->tmpl`、`cf->tmpl`）⇒ 改写成不依赖字段拼写的说法
   （`planTemplate(fx)` / "used to skip them"）——这正是上一轮记的"注释不能指向不存在的字段"。

**验收**：构建零告警；tests **325/0**（发布与 `EXTC_DBG=1`，断言/兜底 **0** 命中、note ×84 与改前一致）；
`[plan-seam] ok`（AST 上无 `tmpl`）；`[tmpl-owners] ok`（`FuncDef.tmpl` 已不在扫描范围，
只剩 `CallCheck` 的 3 个站点）；walker 23 ✓、guards 19 ok / 0 broken；`check.sh quick` 与全量见下条。

**工具侧的假红（记下来）**：字段搬走后，`check_tmpl_owners.py` 把 `plan.c` 里 `PlanSlot.tmpl`
那两处（**存储层自己**）算成"无属主"⇒ 正确的迁移反而判红。修法不是放宽判据，而是**说清扫描范围**：
`collect()` 跳过 `plan.c`，理由写在 docstring 里。教训：**搬家之后，"字段的存储"与"字段的使用"
同名**，扫描器必须显式说明它看的是哪一半。




**完整模式验收（提交 `1b5c3ee`）**：`./check.sh` **68 节 / 0 失败**；五道闸门全绿
（checkc **976 文件** gcc+clang · ASan **217 正例** · 差分 **21 例** · 规模 **13 例** · 逃逸语料 18 份）；
`[plan-seam] ok`（AST 上无 **15** 个字段）；`[tmpl-owners] ok`。

> 小提醒：完整模式会重写 `bench/stl/RESULTS.md`（基准数字），提交前 `git checkout --` 还原，
> 别把它带进功能提交。

## X 批次余下清单（下一步能开工的顺序）

| 序 | 事项 | 判据 / 代价 |
|---|---|---|
| 1 | **`coroBoxed` 搬侧表** | 与 `tmpl` **同形**：唯一写点 `check.c:456`（检查器），codegen 只经 `planCoroBoxed` 读（直读 0、直写 0），检查器读点 `check_top.c:6826` 一处 ⇒ 照抄本轮配方（记录位 + setter + 访问器 + 删字段 + `MOVED` 加一项），AST 将只剩 **2** 个计划字段 |
| 2 | `Expr.func` 的**实例集显式化** | 本批次剩下的**大头**：18 写（全在检查器）+ 85 检查器读 + 58 codegen 读（已收口）；要"物化唯一入口 + codegen 前封闭实例集 + 显式 worklist"三件事一起做。**先做只读侦察**（把 18 个写点逐个定位、判"是否都是同一个动作"），再决定搬不搬存储 |
| 3 | `Stmt.forStep` | **有意保留**（半语法：parser 两处写），已在表里写明理由 |
| 4 | `FuncDef.coroKind` | 顺序依赖（读点在 prepass 之前）⇒ 要么把"prepass 必须先于哪些读点"写进契约，要么把 prepass 提前；**不能只换存储位置**（已实测失败并回退） |

## X4 第三半（本轮完成）：`coroBoxed` 搬进侧表 —— AST 上只剩 **2** 个计划字段

**判据先行**（三条只读侦察，与 `tmpl` 同形）：① 唯一写点在**检查器**（`check.c:456`，
"把帧强转成句柄"那一刻）；② codegen **直读 0、直写 0**（X3 第四步已把"模板标记写进实例"
改成就地推导，读点全经 `planCoroBoxed`）；③ 检查器读点只两处（`check_top.c` 的
"协程是否需要 zone"）。⇒ 同一配方，无顺序风险。

| 步骤 | 位置 |
|---|---|
| `PLAN_CORO_BOXED` 记录位 + 槽位成员 + `planSetCoroBoxed` | `plan.c` |
| `planCoroBoxed` 改读侧表（未设置 ⇒ false） | `plan.c` / `plan.h` |
| 唯一写点改经 setter | `check.c:456` |
| 读点改经访问器 | `check_top.c` 两处 |
| 删字段 + 改注释 | `ast.h:710` |
| 棘轮 | `MOVED` 加 `"coroBoxed"` ⇒ AST 上已无 **16** 个字段 |

**顺带修掉的三处"注释指向已删字段/旧机制"**（都是上一轮那条规矩的延续）：
`check_top.c` 那句"`coroBoxed` is copied onto the instance"（搬走后不再成立）、
`codegen.c:6980` 的 prepass 说明、`plan.c` 头部"还留在 AST 的字段"清单。

**顺手纠正了表里两处陈旧行**（`docs/topics/AST-ANNOTATIONS.md` 第 5 节）：
`FuncDef.instName` 早在 X3 第六步就搬了、`coroBoxed` 本轮搬了 ⇒ 两行改标"已搬"；
`FuncDef.coroKind` 澄清为"**不是计划字段**"（codegen 自己的 prepass 下标，检查器读点 0），
它的问题是实现层面的**顺序依赖**，与"AST 充当计划"无关。

**验收**：构建零告警；tests **325/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中、note ×84）；
`check.sh quick` **57/0**；`[plan-seam] ok`（AST 上无 `coroBoxed`）；`[tmpl-owners] ok`；
完整模式见下条。

### X 批次剩余（至此只剩一件半）

| 序 | 事项 | 状态 |
|---|---|---|
| 1 | `Expr.func` 的实例集显式化 | **未做**（本批次唯一的大头）：18 写（全在检查器）/ 85 检查器读 / 58 codegen 读（已收口）。**先只读侦察 18 个写点**，判"是否都是同一个动作"，再决定搬不搬存储 |
| 2 | `Stmt.forStep` | **有意保留**（半语法，parser 两处写）——不是待办 |
| 3 | `FuncDef.coroKind` | 不是计划字段；顺序依赖留给"prepass 契约或提前"那一类（已实测不能只换存储位置） |

**完整模式验收（提交 `b970582`）**：`./check.sh` **68 节 / 0 失败**；五道闸门全绿
（checkc **976 文件** gcc+clang · ASan **217 正例** · 差分 **21 例** · 规模 **13 例** · 逃逸语料 18 份）；
`[plan-seam] ok`（AST 上无 **16** 个字段）；`[tmpl-owners] ok`。

## X 批次进度总览（截至本提交）

| 阶段 | 内容 | 状态 |
|---|---|---|
| X0 | 盘点表 `docs/topics/AST-ANNOTATIONS.md`（字段/写者/读者/四类归属） | ✅ |
| X1 | `plan.h`/`plan.c` 接缝 + codegen **182 个读点**收口 + 棘轮进 `check.sh` | ✅ |
| X2 | **13 个字段**搬离 AST（Expr 四／FuncDef arena-pool 四／协程五） | ✅ |
| X3 | 回写侧：棘轮 + 逐条复核（47→31→1）+ 切断 `coroBoxed` 回写；**再加本轮的 `coroBoxed` 字段搬迁** | ✅ |
| X4 | `instName`(X3 第六步) / `tmpl`(第二半) / `coroBoxed`(第三半) 搬入侧表；文档与理由成文 | ✅（**AST 上只剩 2 个**） |
| X3 余项 | **`Expr.func` 的实例集显式化** | ❌ 未做（唯一大头，先只读侦察 18 个写点） |

**现在 AST 上剩下的两个"计划"字段**：`Expr.func`（等实例集显式化）与 `Stmt.forStep`
（半语法，有意保留）。其余分析缓存族（②）不构成"AST 充当计划"的耦合，见第 5 节。

## X3 第九步（本轮完成）：`Expr.func` 的只读侦察 —— 并查出"共享调用点"这件真问题

**目标**：交班时定的是"先只读侦察 18 个写点，判是否同一个动作，再决定搬不搬存储"。侦察做了，
并把旧数字更正为实测值（口径：剥注释/字符串后按基座判类型）：

| 项 | 实测 | 旧记录 |
|---|---|---|
| `Expr.func` 写点 | **12**（`check_expr.c` 11 + `check_top.c:5207` 的 `cc->node->func`） | "18 写" |
| `Expr.func` 直读 | **56**（check_top 20 / check_escape 18 / check_expr 14 / check_stmt 2 / check.c 1 / plan.c 1） | "85 读" |
| codegen | **54 处 `planCallee`**，`->func` 直读 **0** | "58 读" |
| codegen 回写 | **0**（`codegen.c:7206` 那处是**注释**） | —— |
| 同名别的字段 | 5 个写点属 `DeferredUse/RefCheck/OpCheck/CallCheck/MethodCheck` | —— |

**写点分两种性质**：9 处是普通解析（检查该调用点**当时**，多数伴随 `used = true`），
3 处是**延迟重指**（2436/3216/5207，在 POST 不动点里按实例重跑）。

### 本轮最重要的发现：**一个调用点只有一个值，而它可能属于多个实例**

`funcInstance` 是**浅拷贝** ⇒ 实例共享模板的函数体 ⇒ 泛型体里的调用点被多个实例**先后重指**，
**只有最后一次写留下来**。实测（新探针 `examples/generic-shared-callsite.extc`）：

```extc
fn pick<T>(x: T) -> T { return x }
fn wrap<T>(x: T) -> T { return pick(x) }
var a: i32 = wrap<i32>(i32(9))   var b: i64 = wrap<i64>(i64(8))
```

* **一个实例**：生成 `return pick_i32(x);` ✓
* **两个实例**：`wrap_i32` 与 `wrap_i64` **都**发 `return pick_i64(x);`，
  而 **`pick_i32` 从未被发出**（`used` 挂在实例上，重指被覆盖 ⇒ 它一直是 false ⇒
  被 codegen 的"没人调就不发"剪掉）。

**为什么今天没有闸门抓到（诚实说清）**：生成物是**合法 C**（`int64_t` 结果隐式转回 `int32_t`），
运行结果也对（`pick` 是恒等函数）⇒ 闸门①②③（编得过 / ASan 干净 / 与 C 同结论）**都不会响**；
tests 的 `// expect:` 也只看输出。**要变成可判红的判据，得让 `pick` 的体依赖 `T` 的宽度**，
而那是实现落地之后的事（探针先留在 `examples/`，并在文件头写明它是"实现之后的判据"）。

**结论（决定下一步怎么做）**：**只搬存储没有意义**——搬走之后仍然是"每个调用点一个值"，
"最后一个写赢"只是换个地方发生。要按第 6.3 节写的最小形态做三件事：
① 不动点跑完后**封闭实例集**（此后只读，可断言）；② 把延迟重指改成**按（调用点，实例）记账**
（计划侧一张表，`planCallee` 在实例的发射上下文里查）；③ `used` 同族处理（或"被表引用的实例都算 used"）。
判据：两实例程序生成 `wrap_i32 -> pick_i32` 且 `pick_i32` 被发出。

**本轮的产出**：
* `docs/topics/AST-ANNOTATIONS.md` 新增 **6.3 节**（实测表 + 两种写点 + 发现 + 最小落地形态），
  第 5 节 `Expr.func` 行改为实测数字并指向 6.3；
* `examples/generic-shared-callsite.extc`（新探针；tests run.sh **325 → 326**，README 同步）；
* 旧数字（18/85/58）在本文件与表里都标注为"X0 的 grep 估算，以实测为准"。

**验收**：构建零告警；tests **326/0**（新增探针）；`--check-c` 对探针通过；
`[plan-seam] ok` / `[tmpl-owners] ok`。**没有改任何编译器代码**（纯侦察 + 判据）。

## X3 第九步·判据（本轮完成）：闸门⑥ —— 实例接线的结构判据

按"先做判据"的要求：给 6.3 那个缺陷做一条**能判红、且修好后会自动变绿**的判据。

**为什么必须新开一条**：那个缺陷的生成物**仍是合法 C、跑出来也对** ⇒ 闸门①（编得过）、
②（ASan 干净）、③（与等价 C 同结论）**都看不见它**。能看见的只有**结构**问题：
*每个被发出的实例，调的是不是它该调的那个*。

**闸门⑥**（`tools/gate_callsite.py`，已接进 `check.sh` quick → **58 节**）：

* 探针 `tools/callsite-corpus/*.extc`，每份配 `.expect`：`require <实例>`（必须被发出）、
  `call <实例> <被调实例>`；
* **期望按语义写，不按今天的输出写**。**负例已验**：把 `.expect` 改成"期望它调错的那个"
  ⇒ 闸门立刻变绿 ⇒ 证明判据确实由 `.expect` 驱动，而不是把今天的输出固化成"对"；
* 基线 `tools/gate-callsite-known-bad.txt` = **施工单**；清空基线 ⇒ 当场判红（负例已验）；
* **对照组** `control_single_instance.extc`（同形状、只一个实例，今天已绿）⇒ 证明不是"一律判红"，
  且**不许进基线**（当前基线里 control 数 = 0）。

**探针用"不可互换"的两种类型**（这是本轮的关键设计）：`meter` 结构体带 `+` 重载，
所以"调错实例"不是审美问题 —— 生成物在这一点上连 C 都编不过
（`incompatible type for argument 1 of 'pick_meter'`）。**但判据不写成"生成物编不过"**：
那只是"最后写赢"的一个副作用，换一下实例顺序方向就反过来（`wrap_meter` 调 `pick_i32`），
所以判据是那张结构表：

```
require wrap_i32 / wrap_meter / pick_i32 / pick_meter
call wrap_i32 pick_i32        ← 今天红：它调 pick_meter
call wrap_meter pick_meter
```

**今天的红**（基线那一条，两条都报出来）：`pick_i32` 从未被发出；`wrap_i32` 调 `pick_meter`。

**修好之后的判据**：本闸门绿（基线删空）+ `examples/generic-shared-callsite.extc` 的生成物里
`wrap_i32 -> pick_i32`、`wrap_i64 -> pick_i64` 且两个 `pick_*` 都被发出。

**验收**：`check.sh quick` **58/0**（新增一节）；`tests/run.sh` **326/0**；
`[callsite] ok（失败 1 已知 / 新增 0）`；负例两向都验过（清空基线 ⇒ 红；`.expect` 写反 ⇒ 绿）。

**完整模式验收（提交 `64a0b23`）**：`./check.sh` **69 节 / 0 失败**（新增闸门⑥一节，完整 68→69）；
五道老闸门全绿（checkc **977 文件** · ASan **218 正例** · 差分 21 · 规模 13 · 逃逸语料 18 份，
其中对照组 0 进基线）；**闸门⑥红得正确**：`[callsite] 失败 1（已知 1 / 新增 0），基线 1`；
`[plan-seam] ok`；tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中、note ×84）。

> 记一条口径：闸门⑥ **现在就该是红的**（基线 1 条 = 施工单）。它与其它闸门"全绿"的区别是
> **故意的**：判据先立、实现后做 —— 这也是本批次"判据先行"的用法。修好之后必须把那一行从
> `tools/gate-callsite-known-bad.txt` 删掉（棘轮只往一个方向转），届时闸门自然全绿。

## X3 第十步（本轮完成）：实例集显式化（第一刀）—— 按（调用点，实例）记账 + 发射上下文

**判据先行**：闸门⑥ 已在前一轮立好（两个探针 + `.expect` + 棘轮基线）。本轮把两个探针都修绿。

**问题回顾**（6.3 实测）：泛型体里的调用点是**一个被所有实例共享的节点**，而实例化时按实例
逐轮重指 ⇒ **只有最后一次写留下**；`used` 也挂在实例上 ⇒ 另一个实例既没被正确调用、也没被发出。

**做法**（三处）：

| 部件 | 改动 |
|---|---|
| 计划侧 | `plan.c` 新增替代表 `(调用点, 外层实例) -> 被调实例`（`planSetAltCallee`，幂等）；`planCallee` 先在**发射上下文**里查替代表，查不到才回落到节点上的 `Expr.func` |
| 发射上下文 | `planEnterFunc`/`planLeaveFunc`（函数实例，`codegen.c` 的 `genFunc` 前后）与 `planEnterInst`/`planLeaveInst`（泛型结构体的类型实例，方法体发射前后） |
| 检查器 | 12 个 `Expr.func` 写点全部改经 `planSetCallee`；`resolveDeferredCall` 增加两个参数（外层函数实例、外层类型实例），两处调用点分别传 `fi` 与 `inst` |

**为什么这样切**：**只有一个实例的调用点不受影响**（替代表查不到 ⇒ 走原指针）⇒ 这不是
"重新发明一遍重指"，而是只在多实例时多一行。发射上下文本来就存在（`substParams`/`substArgs`），
本轮把它**变成可查询的实例身份**：过去它只回答"T 是什么"，现在也回答"这个调用点是谁"。

**两个探针都转绿**（实测）：
* `shared_callsite_two_instances`：`wrap_i32 -> pick_i32`、`wrap_meter -> pick_meter`，
  `pick_i32` 与 `pick_meter` 都被发出；
* `shared_callsite_type_instance`（本轮新加）：`holder_i32_dup -> pick_i32`、
  `holder_i64_dup -> pick_i64`，两个 `pick_*` 都被发出。

**顺带解决的一半**：`used` 现在对每个被记录的实例都会置位（`resolveDeferredCall` 里那一行），
所以"没人调就不发"不再剪掉另一个实例。

**验收**：构建零告警；tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中、note ×84）；
**闸门⑥ 全绿、基线清空**（`[callsite] 失败 0，基线 0`）；`[plan-seam] ok`；完整模式见下条。

**没有做完、写在明处的**：`resolveDeferredCall` 仍会写一次节点上的兜底指针（最后一次赢）。
只有当"某调用点属于多个实例、其中某个实例没有被记录"时它才会显形（`planCallee` 回落到兜底值）。
两个探针与 326 个用例都覆盖不到这种"部分记录"，留给后续。

**完整模式验收（提交 `4bdd652`）**：`./check.sh` **69 节 / 0 失败**；六道闸门**全绿**
（checkc **977 文件** gcc+clang · ASan **218 正例** · 差分 **21 例** · 规模 **13 例** ·
逃逸语料 18 份 · **实例接线 2 个探针、基线 0**）；tests **326/0**（发布与 `EXTC_DBG=1`，
断言/兜底 **0** 命中、note ×84 不变）；`[plan-seam] ok`。

## X 批次状态（本轮之后）

| 阶段 | 内容 | 状态 |
|---|---|---|
| X0 | 盘点表（字段/写者/读者/四类归属） | ✅ |
| X1 | `plan` 接缝 + codegen 182 读点收口 + 棘轮 | ✅ |
| X2 | **13 个字段**搬离 AST | ✅ |
| X3 | 回写侧棘轮与逐条复核（47→31→1）+ 切断 `coroBoxed` 回写 | ✅ |
| X4 | `instName` / `tmpl` / `coroBoxed` 搬入侧表 + 文档成文 | ✅ |
| **X3 余项** | **实例集显式化第一刀**：按（调用点，实例）记账 + 发射上下文 | ✅（闸门⑥ 转绿、基线清空） |
| 余下 | `resolveDeferredCall` 仍写一次兜底指针（"部分记录"那种形状尚未覆盖）；`Expr.func` 仍是 AST 上的字段（**行为已正确**，要不要再搬是纯存储问题）；`Stmt.forStep` 半语法有意保留；`FuncDef.coroKind` 顺序依赖（非计划字段） | 记档 |

## 解耦 P0（本轮完成）：两条"能看见违规"的判据 + 棘轮基线

**背景**：X 批次把"计划"字段搬进了侧表，但那是**逐字段补丁**；按主流编译器（rustc/Clang/Go/
Swift/MLIR）的做法，"彻底解耦"缺的是**分析结果那一层**。目标架构与分期写在
[docs/topics/AST-DECOUPLING.md](../docs/topics/AST-DECOUPLING.md)。P0 = 先立判据。

**新增工具**：

* `tools/cscan.py`：共用的 C 感知扫描（剥注释/字面量、结构体成员、写/读判定、`Ratchet` 基线类）。
  与 `comment_neutral.py` 的规则**故意不同**（后者保留字面量，因为"消息不同不算同代码"；
  找成员访问的扫描器必须抹掉字面量，否则生成 C 里的 `"a->top->used"` 会被当成访问）——已写进模块头。
* `tools/check_ast_freeze.py`（**R1 · AST 冻结**）：parser / `plan.c` / `types.c` 之外，
  对 **AST 节点专属成员**的直接写。只统计"只在节点结构体上声明的成员名"（实测把 640 处降到
  **146 个 key**）。诚实说明：文本扫描分不清基座类型（`codegen.c` 那 9 处是它自己的描述符结构），
  所以这条基线是**方向指示器**，最终由"删字段 ⇒ 残留直写变编译错误"兜底。
* `tools/check_layering.py`（**R3 · 只读边界**）：跨阶段 include 私有头 + parser 反向依赖 plan。
  报出 3 条边：`codegen.c -> check_internal.h`（去掉它 **14 个编译错误**，真正依赖的是
  `isProtoType`/`isOverloadableOp`/`findOperator`/`findMethod` 这些**类型层谓词**）、
  `dataflow.c -> check_internal.h`、`parser.c -> plan.h`。

**基线（= 施工单，棘轮只许减）**：`tools/ast-freeze-known-bad.txt` 146 个 `文件:字段` key
（`check_expr.c` 54 · `check_top.c` 37 · `check_stmt.c` 15 · `modules.c` 9 · `codegen.c` 9 ·
`check_escape.c` 8 · `check.c` 6 · `check_lookup.c` 4 · 其余 6）；
字段侧最常被写的是 `type`(6) · `refDepth`(5) · `cname`(5) · `used`(3) · `modName`(3) · `minAt`(3) ·
`func`(3)…（与 X0 的"②分析缓存 / ③编译计划"两族吻合）。
`tools/layering-known-bad.txt` 3 条边。

**验收**：`check.sh` 完整模式 **71 节 / 0 失败**（quick 60）；tests **326/0**；
六道老闸门全绿（977 文件 checkc · 218 例 ASan · 21 例差分 · 13 例规模 · 18 份逃逸语料 ·
实例接线基线 0）；`[plan-seam] ok`；**`[ast-freeze]` 146 已知 / 0 新增 · `[layering]` 3 已知 / 0 新增**。
**零行为改动**（纯工具 + 棘轮 + 文档）。

**下一步（P1）**：`src/results.h/.c` —— 按 owner（函数/模块）一份结果包 + arena + 稠密节点 ID；
先把已收口、无反向写的一族（协程族 + arena/zone 族）从 `plan.c` 的侧表下沉进去，
`plan.c` 变成它的薄封装。判据仍是全套 + 两道新棘轮"只许减"。

## 解耦 P1（本轮完成）：结果层第一刀 —— `results.h/.c`（arena + 稠密节点 ID + 属主看守）

**做了什么**（提交 `90edb15`）

| 部件 | 内容 |
|---|---|
| `src/results.h` | `NodeId`（稠密整数，0 = 无）、`ResultKind`、`NodeResults`（**一个节点一份结果槽**）、`nodeIdOf` / `resultsOf` / `resultsById` / `resultsAs` |
| `src/results.c` | 两个数组 + 一个指针→id 哈希：`g_nodes[id]`、`g_results[id]`，**id 就是下标**；id 首次需要结果时分配（parser 一行不改），分配后永不变 |
| `src/plan.c` | 删掉 `PlanSlot` 与 `slotFor` 指针哈希表；32 个函数改走 `resultsAs(...)`；文件只剩"字段什么意思 + 怎么记 + 两个替代表" |

**两个来自调研的设计决定**（写进 `results.h` 的理由里）：
1. **id 而不是指针**：指针被回收后按地址查表会**静默命中旧条目**（LLVM pass manager 文档自陈此坑）；
   id 可存、可比较、可校验。与 rustc `ItemLocalId`（"dense range … a `Vec` instead of a tree or
   hash map"）与 Cranelift `PrimaryMap`/`SecondaryMap` 同形。
2. **属主看守**（rustc `validate_hir_id_for_typeck_results`）：`NodeResults.kind` 由第一个写者盖章，
   之后每次访问比对；不一致是**编译器 bug** ⇒ `EXTC_DBG=1` 响亮 abort，发布版保持旧行为。

**验收**：构建零告警；tests **326/0**（发布）· **326/0**（`EXTC_DBG=1`，**属主看守 0 命中**、
断言/兜底 0 命中、note ×84 不变）；`check.sh` 完整模式 **71/0**（含 `[ast-freeze]` / `[layering]` /
`[callsite]` 三条棘轮、六道老闸门：977 文件 checkc · 218 例 ASan · 21 例差分 · 13 例规模 ·
18 份逃逸语料）；`[plan-seam] ok`。**行为不变**。

**顺手记下的"这一刀不包含什么"**：`NodeId` 还没有按 owner 分组（现在是一张全局稠密表）；
`ResultKind` 只覆盖 plan 拥有的族——**②分析缓存族**（`refDepth`/`origin`/六个掩码…，即
`[ast-freeze]` 那 146 个 key 的主体）仍写在 AST 上，进 `NodeResults` 还是进检查器自己的结构，
是 P2 的内容。

**备份**（P1 开工前按要求做的）：`~/extC-backup/extC-bundle-20261002-162302.git`（完整 history，
`git bundle verify` 通过）+ `extC-worktree-*.tar.gz`（完整工作树，含未跟踪的 `proofs/`）+
`SHA256SUMS-*.txt`；并**演练过还原**（clone → `make` → tests 326/0 → 两条棘轮 ok）。

## 解耦 P2 第一刀（本轮完成）：`forStep` 归语法 + 消掉 parser 反向依赖

**两件事，一个目标：让 parser 只写语法、不读计划。**

**(1) `Stmt.forStep` 是语法，判断才是结果。** 它由 parser 写在循环体上（`continue` 要跳到它，
少了它任何含 `continue` 的 `for` 都死循环，审计 P0-15）。检查器唯一要对它做的判断是
"**步进没了**"（循环改到迭代器上时 `next()` 推进）⇒ 那是**结果**：

| 之前 | 现在 |
|---|---|
| `inner->forStep = NULL;`（**改树**） | `planSetForStepDropped(inner)`（记进 `results` 层，字段一字节不动） |
| `planForStep` 直接读字段 | 先问结果层"是否被撤销"，再回落语法字段 |

**(2) `parser.c` 不再 include `plan.h`。** 它借计划层只为一处：`@export` 上的协程诊断。
那处是**解析期读检查器才写的计划值**（`planIsCoro` 由 `check_top.c` 设置）⇒ **恒 false，死消息**
（W1-LOG 已记档）。改用**语法判据**：解析期返回类型直接写成 `coroutine<...>` 即协程。

**实测（这刀的关键证据）**：`@export fn tick(n: i64) -> coroutine<i64>` 现在报**更准确**的那条
诊断（`` `@export` on a coroutine ``），不再是泛化的"C-ABI 不能返回 `coroutine<i64>`"；
两者都拒绝同一个程序。协程**不能写成类型别名**（实测 `type co = coroutine<i64>` 解析期就报错）
⇒ 两种判据覆盖面**恰好相同**，语法判据更强也够用。

**棘轮结果**：`[layering]` **3 → 2**（`parser.c -> plan.h` 删除）；
`[ast-freeze]` **146 → 145**（`check_stmt.c:forStep` 删除）。两条都只往一个方向转。

**验收**：构建零告警；tests **326/0**（发布）· **326/0**（`EXTC_DBG=1`）；`tests/coro` **29/0**；
`[plan-seam] ok`、`[tmpl-owners] ok`、两条棘轮 ok；完整模式见下。

**新发现的判据空白（记下来，P2 后续）**：`[ast-freeze]` 只看"直接写 AST 成员"，
**看不见"经 results 层写一个语法字段"**——`planSetForStepDropped` 正属这一类。
堵它需要一张"哪些节点字段是语法、不许经分析层写"的名单，在搬 ②分析缓存族时一起定。

## 解耦 T0（本轮完成）：结果按 owner 归属（盖章 + 共享标记 + 观察模式）

**做了什么**（提交 `efc5d3a` + 收尾 `8693663`）

| 部件 | 内容 |
|---|---|
| owner 进出 | `resultsEnterOwner` / `resultsLeaveOwner` / `resultsCurrentOwner`；检查器在两处设置：`checkFunc` 进函数体、`CheckLambda` 进 lambda 体 |
| 盖章 | 槽位第一次创建时把**当前 owner** 记进 `NodeResults.owner`（模块级工作 owner 为 NULL ⇒ 不校验） |
| **语义修正（实测得出）** | owner 取**函数体的模板**而非实例：`wrap<i32>` 与 `wrap<i64>` **共享同一函数体**，同一批节点被两个实例先后访问；用实例当 owner 会把正常形状判成错 |
| 共享标记 | 只被单个 body 访问的槽位第一次被另一个 body 碰到 ⇒ 标 `shared` 并只记一次。**共享是正常结果**：泛型体节点本来就被所有实例访问 |
| 观察模式 | `EXTC_DBG_OWNER=1` 打印；`EXTC_DBG=1` 下**暂不** abort |

**为什么暂不硬判（这是本轮最有价值的实测）**：把"首次跨 body"直接当断言 ⇒ **3 个泛型用例红**
（`generic-calls-generic` / `generic-instance-order` / `generic-shared-callsite`），
报的是同一件事：**同一节点上的分析结果被多个实例先后写**——即**最后写赢**，正是 T4 要消除的耦合。
⇒ 这条判据量的是"**还没修完的量**"，不是"有没有 bug"；T4 把字段归属做完后升为硬判据。

**本轮的数字**：当前代码上 `EXTC_DBG=1 EXTC_DBG_OWNER=1 ./tests/run.sh` **0 次跨 body 首次越界**
（此前那 4 次来自 `generic-shared-callsite` 的实例循环——它是 T4 的施工单基线）。

**顺带清掉三条假阳性**：`NodeResults.owner` 与 `FuncDef.owner` 同名 ⇒ `[ast-freeze]` 把它误报成新增。
按既有纪律加 `AMBIGUOUS_NAMES`（明确列出"节点与非节点结构体同名"的成员并说明理由），
基线 **145 → 142**。同时把新开关 `EXTC_DBG_OWNER` 登记进 `--help` 并重生成手册（`gen_flags.py` +
`build_manual.py` 两份生成物）。

**T0 的边界（说清楚）**：id 仍是一张**全局稠密表**（owner 只做校验、不切分区）；
"每个 owner 一张小表"留给 T4 —— 现在做分区没有收益，反而 T4 还要再改一次。

**验收**：`check.sh` 完整 **71/0**（quick 60/0）；tests **326/0**（发布与 `EXTC_DBG=1`，
断言/兜底 0 命中）；六道老闸门全绿（977 文件 checkc · 218 例 ASan · 21 例差分 · 13 例规模 ·
18 份逃逸语料 · 实例接线基线 0）；四条棘轮 `[ast-freeze]`(142)/`[layering]`(2)/`[plan-seam]`/
`[callsite]` 全 ok。

## 解耦 T1（本轮完成）：身份族与发射门一起搬（`Expr.func` + `FuncDef.used`）

**为什么必须一起搬**：二者**同写点**——不动点里"把调用点重指到 inst"与"把 inst 标成被调用"
是**一次决定**（`resolveDeferredCall`）；只搬一个，不动点就会同时写两个存储。
`used` 另有 3 个写者（检查器 20 处、codegen 1 处、`base.c` 1 处）。

**做了什么**（提交 `272a0da`）

| 部件 | 内容 |
|---|---|
| 存储 | `NodeResults.func` / `NodeResults.used`；`PLAN_CALLEE` / `PLAN_USED` 两位 |
| 写口 | 新增 `planSetUsed`；检查器 20 处 `->used = true|false` 全改经它 |
| 读口 | 新增 `planUsed`；`check_top.c` 4 处、**codegen 6 处**改经访问器 |
| 删字段 | `ast.h` 的 `Expr.func` 与 `FuncDef.used` 删除 |
| 棘轮 | `[plan-seam]` 的 `MOVED` 加 `func`/`used`（**17** 个字段不得再出现在 AST 上） |

**迁移手法（本轮最值钱的经验，已写进 AST-DECOUPLING 第 12 节）**：
1. **先双写 → 后切读 → 最后删字段**：`planSet*` 同时写两处、访问器优先读侧表并回落原字段
   ⇒ 每一步都全绿（326/0）；
2. **删字段让编译器逐条点名**：删掉后 `make` 报 **55 处** `'Expr' has no member named 'func'`，
   按 (文件,行) 逐行改成 `planCallee(X)`。**我第一版按"标识符白名单"批量替换，误伤了 STL 里
   一个局部结构体的同名成员**（`d->…`），靠快照整体回退；第二版用编译器点名，一次到位。
3. **同名成员必须人工判**：`->func` 在 `DeferredUse`/`CallCheck`/`MethodCheck`/`RefCheck`/`OpCheck`
   上是**另一个结构体的字段**；`planCallee(du->call)` 与 `du->call->func` 是两种东西。
   编译器报"某结构体没有 planCallee"正是在提示这一点。

**棘轮结果**：`[ast-freeze]` **142 → 136**（6 条 key 消失：`check.c:func`、`check_escape.c:func`、
`check_expr.c:func`/`used`、`check_stmt.c`、`base.c:used`）；`[layering]` 不动；
`[plan-seam]` 现在钉住 **17** 个字段。

**验收**：`check.sh` 完整 **71/0**（quick 60/0）；tests **326/0**（发布与 `EXTC_DBG=1`，
断言/兜底 0 命中、note ×84 不变）；六道老闸门全绿（checkc 977 文件 · ASan 218 例 · 差分 21 ·
规模 13 · 逃逸语料 18 · 实例接线基线 0）；四条棘轮 ok。**行为不变**。

**T1 之后**：AST 上还剩 **136** 个 `[ast-freeze]` key，主体是 ②分析缓存族（T4）；
`[plan-seam]` 的 18 个"计划字段"里只剩 `Stmt.forStep`（半语法，**有意保留**）。

## 事后排查（本轮）：两个"神秘进程" = harness 超时杀不干净留下的孤儿

**现象**：`ps` 里两个 `build/recursion_too_deep` 各转 **31 / 34 分钟**、100% CPU、零输出
（父进程是 `/init` ⇒ 已被孤儿化）。

**查证（三步，都不靠猜）**：
1. 直接跑那条用例：**0.2 秒**内正常打印 `trap: recursion too deep (unbounded recursion?)`，
   退出码 1 —— **陷阱本身是好的**，连跑 5 次结果一致；`-O2` 编译出的二进制单独跑也正常。
2. 所以孤儿不是"用例坏了"，而是**harness 杀进程的方式**：`tools/parrun.py` 的 `sh()` 用
   `subprocess.run(..., timeout=120)`，超时时 Python 只杀**直接子进程**（`extc`），
   而 `extc --run` 会 `fork` 出编译好的程序（`src/main.c:119`）⇒ **那个程序活了下来**。
   一次超时 = 一个孤儿，且它在 `communicate()` 的管道上写、在 CPU 上空转。
3. 复现验证：用同款 `sh()` + 一个"派生子进程并长睡"的命令 ⇒ 超时后**残留**；
   改成**进程组杀**后 ⇒ 超时后**(无)**残留。

**修法（两处 harness，都不改编译逻辑）**：
* `tools/parrun.py` 的 `sh()` 与 `tools/gatecommon.py` 的 `run()`：子进程用
  `start_new_session=True` 起（自成进程组），超时时 `os.killpg(os.getpgid(pid), SIGKILL)`
  杀**整组**，再 `communicate(timeout=10)` 回收输出；返回码仍是 124 + `(timed out)`。
* 各加一个收尾守卫 `reap_orphans()`：`pgrep -f '^build/[A-Za-z0-9_]+$'` 找到跑飞的编译产物
  ⇒ **杀掉并点名打印**（`[leak] 清理了 N 个…`）。parrun 在打印"通过/失败"之前调用，
  闸门侧留给后续接进 `check.sh`。
* **判据（都实测过）**：超时后无残留；正常路径输出不变（`tests/run.sh` 326/0、
  `check.sh quick` 60/0）；收尾守卫在无残留时静默（0 次输出）。

**过程中的一次自伤（记下来）**：插入守卫的脚本把 `SCAN_MODES` 分支的 `return 0` 误当成
main 的收尾，导致 `parrun` **提前返回**、`tests/run.sh` 的算术报错（`pass + ` 空值）。
`tests/run.sh` 立刻报出来了 —— 这正说明"每次改完都跑套件"这条纪律值钱。已修回 326/0。

**诚实说明**：孤儿**为何**撞上 120 秒超时没能在事后复现（该用例 0.2 秒、确定性）。
能确证的是两件事：① 用例现在正确 trap；② harness 原先在超时后会留孤儿、现在不会。

## 解耦 T2（本轮完成）：绑定名搬进结果层 —— 并划出"按值类型不可入表"的边界

**对象**："生成的 C 名"（检查器绑定时决定、codegen 打印）。分类时算作 ANALYSIS，实测后
确认它是**三件事**：

| 落点 | 处理 |
|---|---|
| `Expr.u.ident.cname`、`Stmt.u.var.cname`（union 成员） | **搬进结果层**（`PLAN_CNAME` + `planCName`/`planSetCName`） |
| `Param.cname`（**独立字段**） | **有意留在 AST** —— 理由见下 |
| `Sym.cname` | 本来就不在 AST 上 |

**边界（本轮最重要的实测结论）**：`Param` 是**按值存放**的类型，协程帧的参数表会**复制**它
（`vecPush(&f->coroFrame)`），而结果层按**节点指针**寻址 ⇒ 拷贝取不到原槽位。
实测把 `Param.cname` 一起搬 ⇒ **每个协程入口函数丢掉参数名**（生成物里 `int64_t n` 变成
`int64_t`），`tests/coro` **29/0 → 8/21**。⇒ **按值传递的结构体不能进"按指针寻址"的侧表**。
这条已写进 `plan.h`/`plan.c`/`ast.h` 三处注释（适用条件，不是实现细节）。

**另一个语义坑**：`planCName` 起初把**空字符串**当"有名字"返回，而调用方惯用
`p->cname ? p->cname : p->name`（检查器确实写 `""` 表示"没有自己的名字"）⇒ 空串赢过那个回落，
发出**没有标识符的 C 参数**。修法：**空名视同未设置（返回 NULL）**，与字段时代语义一致。

**改动**（提交 `0abae52`）：`results.h` 加槽位；`plan.c` 加位 + 访问器/写入器（带 `ResultKind`
供属主看守）；`check_expr.c` 3 处写、`check_stmt.c` 2 处写改经 setter；31 处读
（check_expr 2 · check_stmt 3 · check_top 12 · codegen 12 · dataflow 2）改经访问器；
`ast.h` 删两个 union 成员；`dataflow.c` 补 `#include "plan.h"`。

**棘轮**：`[ast-freeze]` **136 → 135**；`[layering]`/`[plan-seam]`/`[callsite]`/`[tmpl-owners]` 不动。

**验收**：`check.sh` 完整 **71/0**；tests **326/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中）；
**`tests/coro` 29/0**（这一刀最容易伤的地方）；六道老闸门全绿（checkc 977 文件 · ASan 218 例 ·
差分 21 · 规模 13 · 逃逸语料 18 · 实例接线基线 0）；跑完 `pgrep` **无孤儿**（harness 修复有效）。

**过程记录（一次回退与一次重放）**：第一次把 `Param` 一起搬，协程崩；用它回滚时**取早了一个
备份点**（把 `Expr`/`Stmt` 那半也一起回退，工作区回到 `52471db`）。重放时先把设计改正
（`Param` 留字段）再一次通过。教训：**回滚点要按"这次改动的起点"取，不按"最近一次快照"取** ——
这次两处快照（`t2_base` 与 `t2_wip`）的差别正是这个。

## 解耦 T4（本轮完成三族）：分析侧落地 —— `[ast-freeze]` **135 → 125**

**T4 的做法**：给结果层分出"**分析侧**"受众（`NodeResults.an`），装**只有检查器读写**的字段；
与 plan 字段同槽、不同受众。界线由 `plan.c` 的 `ANALYSIS_FIELDS`（具名清单）钉住：
**一个字段一旦被 codegen 读，就必须移出该清单并配 `planXxx` 访问器**。
三族都用同一套路：加存储与 API → 改读写（编译器点名补漏）→ 删字段（连注释一起看）→ 棘轮。

| 族 | 字段 | 桶 | 提交 |
|---|---|---|---|
| `StructDef` | `builtinHolder` `coroOf`（PLAN）· `lamSig` `makesPoolAny`（ANALYSIS） | 4 | `ee08b6c` |
| `impl`/`trait`/`module` | `ImplDef.target` `ImplDef.trait` `TraitDef.usedDyn` `Module.usesDyn` | 4 | `c9a720c` |
| `Expr` 调用点标记 | `parWorker` `domNew` `viewOf` | 3 | `1ab3525` |

**分类被实测纠正的三处**（都记在文档里）：
* `StructDef.srcName` 是 **parser/模块层**写的"声明名" ⇒ **语法，不动**（按名字统计会把它算成 ANALYSIS）；
* `ImplDef.target` 与 `StoreSite/RefCheck.target` **同名不同属** ⇒ 批量替换必须限定基座 `im->target`
  （本批次第 7 次同类陷阱）；
* `TraitDef.usedDyn` / `Module.usesDyn` 各只有 1 处写，所以"按 key 数"看这一族只降 3 而不是 4。

**删字段连带注释**：第二族删字段时留下**多行注释的尾巴**（`* uniform method tables emitted …`），
当场语法错 —— 又一次印证"删字段要连注释一起看"（第一次是 X4 的 `ast.h`）。

**当前基线构成（125 条）**：`check_expr.c` 45 · `check_top.c` 31 · `check_stmt.c` 13 ·
`modules.c` 9 · `codegen.c` 9 · `check_escape.c` 7 · `check_lookup.c` 4 · `check.c` 4 · 其余 3。
**剩下的主体就是两大族**：`Expr`（26 个字段，最大的是 `refDepth` 28 写/36 读）与
`FuncDef`（26 个字段，含六个效果掩码与 `effState`/`effComplete`/`needsHome`），
外加 `Sym` 上的 6 个（`nfields`/`fieldsComplete`/`callSrc`/`callSrcDepth`/`callMinReq`/`refDepth`）。

**验收**：`check.sh` 完整 **71/0**（T4a、T4b 各跑过一次完整模式）；tests **326/0**
（发布与 `EXTC_DBG=1`，断言/兜底 0 命中）；`tests/coro` **29/0**；`check.sh quick` **60/0**；
六道老闸门全绿；四条棘轮（`[ast-freeze]` 125 · `[layering]` 2 · `[plan-seam]` · `[callsite]` 基线 0）ok。

## 解耦 T4 第七族（`FuncDef` 的 13 个字段）：**未通过，已整体撤回** —— 记下证据与结论

这一族按前六族的同一套路做（存储 + 访问器 → 迁移读写 → 删字段 → 棘轮），
结果 **`tests/run.sh` 1/41**（基线与前六族都是 326/0）。我花了很长时间定位，**没有找到根因**，
按"不许带着半成品前进"的纪律**整体撤回**；只留下两项**独立价值已验证**的改进。

**撤回前的实测证据（都记下来，接力的人不用重跑）**：

1. **症状**：`stdlib/std/io.extc:182` 与 `stdlib/std/dl.extc:52` 报
   `this return value would hold a reference to a local variable that dies first (borrowed from depth 1,
   but this can only hold up to depth 0)` —— 即**转义检查器把合法代码判成错**，且发生在**标准库**里。
2. **不是悬垂指针**（我最初的猜测）：把结果表初值调到 1M（永不 `realloc`）**不改变失败数**。
   （不过这条排查独立发现了**真实的潜在缺陷**，见下，已修。）
3. **不是"槽位与字段不同步"这么简单**：让**全部 12 个访问器直读原字段**、setter 只写槽位
   ⇒ 141 个失败（符合预期，因为没人写字段）；让**读写都走字段** ⇒ 仍 41 个失败
   （**这一条最反常**：字段路径是迁移前的原始语义，本该全绿）。
4. **探针抓到的具体形状**：在 setter 里对拍 `f->isAssoc == v`（v=1）时，`f->isAssoc` 读出来是 **0**，
   而同一次调用里 `f->name` 正常（`withStream`）。反汇编确认：写的是 `NodeResults+0xcf`（= `an.isAssoc`，
   与 `offsetof` 一致）、置位 `btsq $0x34`、随后 `cmp %bpl,0x2e0(%rbx)`（= `FuncDef.isAssoc`）
   ⇒ **槽位写对了，但字段是 0**。而该字段在全仓**只有 setter 一处写**（`grep '->isAssoc\s*=[^=]'` 为空）。
   ⇒ 有一条我**还没找到的路径**在写这个字段或改这个对象。
5. **`gdb` 现场**：`checkMethodShape`（`check_top.c:338`）调 `anSetIsAssoc(f, true)`，
   `f=0x555555a46fe8`、`f->name="withStream"`、`f->isAssoc=false`。
6. **消歧已完成（保留）**：`CG`（codegen 自己的发射状态）**也有** `owSites`/`owLocal`，
   与 `FuncDef` 的同名字段撞名；已把 `CG` 的改名为 `owSitesOwn`/`owLocalOwn`（7 + 5 处）。
   这是本族的前置条件，**独立提交**。

**保留的两项（各自独立可验收）**：
* `CG` 的 `owSitesOwn`/`owLocalOwn` 改名 —— 消掉"同名不同属"的歧义；
* **结果表改为"非移动"**（指针块表，`ID_BLOCK=1024`）：原来用 `realloc` 增长，
  **增长时所有 `NodeResults*` 立即悬垂**，而访问器的写法正是"先读旧值、再写新值"
  （`r->an.arenaSites = v`，`v` 从 `r` 里取）⇒ 这是一个**真实的潜在缺陷**（本族第一次把 `Vec`
   放进槽位才暴露）。改成块表后指针终生有效。已过全套验收。

**下一步的建议（给接力的人）**：从"为什么 `f->isAssoc` 会是 0"这个**单点**入手，
用一个最小复现（只迁 `isAssoc` 一个字段）而不是整族；本族的其余 12 个字段与它同批，
其中 `needsHome`/`arenaSites` 直接决定转义检查的宽严，**先把单点问题解决再整族推进**。
