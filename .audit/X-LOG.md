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
