# AST 上的"注解"：谁写、谁读、什么时候有效（X0 盘点）

> 这份表回答一个问题：**哪些字段不是语法事实，而是"分析结果"或"编译计划"？** 它们是
> "AST 同时充当输入 / 分析产物 / 代码生成计划"这件事的具体清单，也是 X 批次解耦的对象。
>
> 计数方法：`grep` 统计 `->字段` 的写（`= ++ -- += …`）与读，按文件组分类
> （check = `check_*.c`，gen = `codegen.c`，other = `modules/types/dataflow/main/pools/coroutine`，
> front = `parser/lexer/ast`）。数字是**近似**（同一名字可能出现在不同结构体上），
> 但"哪一族、方向如何"是可靠的。复核脚本的口径写在 `.audit/X-LOG.md` 的 X0 一节。

## 1. 分类

| 类 | 含义 | 该住在哪 |
|---|---|---|
| ① 语法事实 | parser 定，分析与 codegen 都只读（`kind`/`name`/`line`/`type`/`body`/`params`/`ret`/`targs`/`sdef`/`owner`） | AST（**保持原样**） |
| ② 分析缓存 | 只在检查器内部用，可重算（`refDepth`/`origin`/`lexicalLevel`/`storedAt`/`heldSrc`/`addressed`/`outOfFrame`/`otherDepth`/`nfields`/`fieldsComplete`/`depth`/`effState`/`effComplete`/`addrMask` 等六个掩码/`addrFromLocal`/`freshCount`/`homeDepth`/`needsHome`/`paramSyms`/`callSrc…`） | 应当搬进"分析侧"（可留在 AST，但**只能经访问器**） |
| ③ 编译计划 | **codegen 必须读**，由检查器决定（下表第 2 节） | 计划侧（X1/X2 的目标） |
| ④ 身份 / 重指 | 把"名字"换成"身份"的地方（`func` 重指到实例、`tmpl`、`instName`、`cname`、`Sym.origin`、`FuncDef.paramSyms`） | 计划侧 + 节点上的绑定（现状混杂） |

## 2. ③ 编译计划族：检查器写 → codegen 读（**单向**耦合）

| 字段 | check 写 | gen 读 | 它是什么 | 陈旧会怎样 |
|---|---|---|---|---|
| `func` | 18 | **58** | 调用点解析到的**实例**（重指） | 生成物调用不存在的函数（P0-5 的 `f_T`） |
| `tmpl` | 2 | 15 | 实例 → 模板 | 摘要/名字取错 |
| `isCoro` | 3 | 14 | 是不是协程 | 帧布局错 |
| `cname` | 16 | 14 | 生成的 C 名 | 名字冲突/找不到（**名字权威**问题） |
| `coroNeedsZone` | 2 | 12 | 协程是否要 zone | 任务的地方释放错（R12/P0-20 一族） |
| `coroProto` | 3 | 11 | 协议方法 next/value/send | 驱动 NULL 帧 |
| `arenaLevel` | 5 | 9 | 分配站点的层 | **UAF**（P0-6c 那一族） |
| `zoneLevel` | 3 | 8 | 池站点的 zone 层 | 池提前/滞后回收（P0-2 的 zone 通道） |
| `yieldType` | 2 | 8 | yield 的类型 | 载荷布局错 |
| `usesHome` | 1 | 8 | 是否用 home arena | UAF / 内存膨胀 |
| `makesPool` | 4 | 8 | 是否建池（**闭包后才知道** ⇒ 三套延迟机制之一） | 池提权落不下去 |
| `instName` | 1 | 8 | 实例的 C 名 | 与 `cname` 同族 |
| `coroFrameType` | 2 | 6 | 协程帧类型 | 帧布局错 |
| `needTemp` | 3 | 5 | 该调用点需要临时量（P0-7） | 生成物重复求值 |
| `forStep` | 1 | 4 | `for` 的步进（**parser 也写** ⇒ 半语法半计划） | continue 走错步 |
| `mayUseArena` | 4 | 3 | 是否可能分配 | 传错 arena |
| `condAllocs` | 1 | 2 | 循环条件里有分配（定案 101②） | 每轮泄漏 |
| `callees` | 0 | 2 | 调用图 | 摘要闭包不全 |
| `arenaArg` | 6 | 1 | 调用点实参 arena | 与 `arenaLevel` 不一致 |

## 3. **codegen 反过来写 AST / 分析状态**（**反向**耦合，最危险的一半）

| 字段 | gen 写 | 它被谁读 | 后果 |
|---|---|---|---|
| ~~`substParams` / `substArgs`~~ | ~~各 8~~ | —— | **X3 更正：不是回写**。`CG`（`codegen.c:146`）里有一对**同名的自有字段**，`g->substParams` 指的是它；`Checker` 上那一对是检查器复核实例时自己用的。X0 的计数按字段名跨结构体统计，把两者混在一起了 ⇒ 真实回写基线是 **31** 不是 47 |
| **`coroBoxed`** | 1 | 检查器 + codegen（经 `planCoroBoxed`） | **唯一真正的回写**：codegen 给 `FuncDef` 打"载荷被装箱"的标记。当前驱动是"先查完所有模块、再逐模块 codegen"，所以这个写在检查器看来不可见；但**驱动顺序一变就会变成顺序依赖**（记在这里，属 X4 的收尾项） |

> **X3 更正的第二次**：上表最初列了 10 个字段 47 处，先去掉同名的 `substParams`/`substArgs`（16 处，
> 属 `CG` 自己）得 31；再**逐条人工复核**后只剩 **1** 处（`coroBoxed`）。其余误报的原因：
> `name`/`body` 的基座是 codegen 自己的描述符结构、`owSites`/`owLocal` 是 `CG` 自己的状态、
> `used`/`ret` 在生成 C 的**字符串字面量**里、`func` 在**注释**里。
> ⇒ **方法教训（第 3 次同类）**：`grep ->field` 的统计只能用来找候选；落表前必须逐条确认
> "基座是不是 AST 节点、是不是在字符串/注释里、字段属于哪个结构体"。

> 结论：**耦合不止"检查器写、codegen 读"这一半**；codegen 回写分析状态使"哪个 pass 先跑"
> 变成了语义问题（P0-5 的根因正是"实例物化＝重指节点"这一副作用）。

## 4. X 批次的动作（对应表里的族）

| 步 | 动作 | 覆盖 |
|---|---|---|
| X1 ✅ | 立 `src/plan.h`/`plan.c`：codegen 只经访问器读第 2 节的字段（182 个读点已收口）；**存储未动**；棘轮 `tools/check_plan_seam.py` 已接进 `check.sh` | 第 2 节全部 |
| X2 ◕（3/4 族已搬） | **第一步已完成**：Expr 的 `arenaLevel`/`zoneLevel`/`arenaArg`/`needTemp` 收进具名子结构 `Expr.plan`（「AST 里哪一块不是语法」在类型上看得见）。**第二步**：把 `Expr.plan` 的存储搬到计划侧表（按 `Expr*` 寻址），写入经 setter、检查器读取经访问器，过渡期用断言保证侧表与字段一致；其余族（`Stmt.condAllocs`/`forStep`、`FuncDef` 计划字段、协程族、名字族）照此办理 | 第 2 节 |
| X3 | 实例集显式化：`func`/`tmpl` 的重指改为计划侧查表 + 封闭实例集 + 显式 worklist；**顺带切断第 3 节的反向写**（`substParams`/`substArgs` 改为显式传入） | 第 2、3 节 |
| X4 | AST 上只剩 ① 与"节点上的身份"，删死字段，更新本表与 `check_internal.h` 的权威节 | 全部 |

## 5. 未迁移 / 不宜迁移的字段（X4 的义务：写明理由与代价）

| 字段 | 为什么还在 AST 上 | 代价 / 下一步 |
|---|---|---|
| `Expr.func`（调用点 → 实例） | **X3 的实例集显式化未完成**：要"物化唯一入口 + codegen 前封闭实例集 + 显式 worklist"三件事一起做，单独搬存储没有意义 | 18 写（**全在检查器**）/ 85 检查器读 / 58 codegen 读（已收口）。已知风险：`e->func = instance` 的重指发生在不动点期间 —— **P0-5 的根因** |
| ~~`FuncDef.tmpl`~~ | **已搬**（X4 第二半，见第 6 节末）：静态归属判清了属主与唯一写点，顺序风险也量清了（驱动两段 + `funcInstance` 不在 codegen 里） ⇒ 照 `instName` 的办法搬进侧表 | 搬迁后 AST 上已无此字段；`CallCheck.tmpl` **有意留在** `check_internal.h`（检查器内部、codegen 读点 0） |
| `FuncDef.instName` | 与 `tmpl` 同族，等实例集一起做 | 1 写 / 11 读 |
| `Stmt.forStep` | **半语法**：`parser.c` 两处写（`continue` 的 label 目标） | 留在 AST 是正确的；计划侧只该管「检查器替换后的那一半」（若有） |
| `FuncDef.coroKind` | codegen 的**发射顺序**传递：读点在 prepass **之前**就会执行 | 实测把表搬进 `CG` ⇒ 5 个 coro 用例红（生成物出现 `__extc_czh-1`）。要么保留并把「prepass 必须先于哪些读点」写进契约，要么把 prepass 提前 —— **不能只换存储位置** |
| ② 分析缓存族（`refDepth`/`origin`/`lexicalLevel`/`storedAt`/`heldSrc`/`addressed`/`outOfFrame`/`otherDepth`/`nfields`/`effState`/六个掩码…） | 只在**检查器内部**使用（codegen 读点 0）⇒ 不构成「AST 充当计划」的耦合 | 未动。若要进一步解耦，应搬进与 plan 平行的「分析侧」结构，而不是继续留在 AST 上 |

## 6. `tmpl` 的静态归属（X3 第八步：只读侦察，无代码改动）

**结论**：`tmpl` **一名两属**，两处都**不是** codegen 的读点 ——
`FuncDef.tmpl`（`ast.h:679`）是实例 → 模板的反向指针，`CallCheck.tmpl`（`check_internal.h:664`）
是延迟调用点记下的"this call names which template"。全仓 `tmpl` 共 **71 个 token、43 个字段访问点**：

| 属主 | 站点 | 写 | 读 |
|---|---|---|---|
| `FuncDef`（实例 → 模板） | **40** | `check_top.c:4747`（`in->tmpl = tmpl`，`funcInstance` 建实例的唯一入口） | 39：`check_top.c` 36、`check_expr.c` 2、`check_lookup.c` 1、`plan.c:163`（即 `planTemplate` 访问器本身） |
| `CallCheck`（延迟调用点） | **3** | `check_expr.c:3227`（`cc->tmpl = f->tmpl ? f->tmpl : f`） | `check_top.c:5202`、`check_top.c:5835` |
| codegen | **0** | — | 14 个读点**全部**经 `planTemplate`（`codegen.c` 里 `->tmpl` 直读 0 处） |

**判据**：`python3 tools/check_tmpl_owners.py`（报告）/ `--verify`（一行结论，进 `check.sh`）。
脚本按**声明所在结构体**认属主：读每个 `struct` 定义拿到成员表，再对每个访问点找回宿主函数，
用"参数/局部声明、`(Struct *)` 强转、返回该结构体的调用"三条规则定基座类型 ——
**未定的一律打 UNKNOWN 而不是猜**。负例已验：临时在 `codegen.c` 加一行 `f->tmpl` 直读 ⇒ 当场判红。

**为什么必须用脚本而不是数数**（本批次同类错误已第五次）：`check_top.c` 的 62 个 token 里有
**11 个在注释里**（脚本先剥注释）；剩下的 51 个里，**38 个**是字段访问、**13 个**是
`funcInstance` 的**形参** `FuncDef *tmpl` 及其裸用（`*in = *tmpl`、`tmpl->params`…）——
手数第一版把"形参"与"字段"混在一起算，同一个 token 还会被数两遍（`in->tmpl = tmpl` 一行里
一个读一个写）。`tmpl` 既是字段名又是形参名，正是本批次反复撞上的那类陷阱。

**对下一步的意义**：

1. **`FuncDef.tmpl` 的迁移与"重指"无关，且顺序风险已量清**：唯一写点在 `funcInstance` 里
   （`*in = *tmpl` 之后那一行），属"实例物化"这一个动作；而**驱动是严格两段**——
   `checkModule`（根 + 各模块体）跑完、错误渲染完，才开始 `generateC`（`main.c`），
   且 `funcInstance` 在 `codegen.c`/`main.c` 里**零调用点** ⇒ codegen 期间不会新造实例。
   ⇒ `tmpl` 的时间序风险**低于** `coroKind`（后者读点跑在 prepass 之前），可以照 `instName`
   的办法搬：加记录位 + setter/访问器，棘轮 `MOVED` 加一项。这纠正了第 5 节把 `tmpl` 与
   `Expr.func` 并成"等实例集一起做"的粗判 —— 两者可以先分开量、分开搬。
2. **真正的前置条件是驱动的两段式**：眼下是"先查完所有模块、再逐模块 codegen"（`main.c`），
   侧表因此跨模块存活；`FuncDef` 实例在检查期建、在 codegen 期读，跨模块也存在。
   这与 `instName`（已搬）同形，所以 `tmpl` 可**照 `instName` 的办法**搬：加记录位 + setter/访问器，
   棘轮 `MOVED` 加一项。**代价**：得先离线构造"实例集封闭"的探针，确认 codegen 期间不再出现新实例。
3. **`CallCheck.tmpl` 不搬**：它只活在检查器的延迟表里，codegen 读点 0 ⇒ 不构成"AST 充当计划"，
   留在 `check_internal.h` 是对的（若要消同名，是**改名**问题，与迁移分开做）。

### 6.1 迁移记录（X4 第二半：已落地）

**做了什么**（与 `instName` 同一套路）：

| 步骤 | 位置 |
|---|---|
| 加记录位 `PLAN_TEMPLATE` + 槽位成员 `FuncDef *tmpl` | `plan.c` |
| setter `planSetTemplate`；`planTemplate` 改读侧表（未设置 ⇒ NULL） | `plan.c` / `plan.h` |
| 唯一写点改经 setter | `check_top.c` `funcInstance`：`planSetTemplate(in, tmpl)` |
| 读点改经访问器 | `check_top.c` 22 处、`check_lookup.c` 1 处（后者补 `#include "plan.h"`） |
| 删字段 + 改那段注释（注释里不能再指一个不存在的字段） | `ast.h:679` |
| 棘轮加一项 | `tools/check_plan_seam.py` 的 `MOVED` ⇒ AST 上已无 **15** 个字段 |

**判据**：`[plan-seam] ok`（AST 上无 `tmpl`）；`[tmpl-owners] ok`（`FuncDef.tmpl` 已不在扫描范围内，
只剩 `CallCheck` 的 3 个站点）；tests **325/0**（发布与 `EXTC_DBG=1`，断言/兜底 0 命中、note ×84 不变）。

**一条顺带得到的保障**：字段删掉之后，**任何漏改的直读都会变成编译错误**
（实测：`check_expr.c` 与 `check_top.c` 的遗漏点正是被编译器逐条点出来的），
比棘轮更硬 —— 这也是"先改读点、最后删字段"这个顺序的价值。

**过程教训（工具侧）**：`tmpl` 搬走之后，`check_tmpl_owners.py` 一度把 `plan.c` 里
`PlanSlot.tmpl` 那两处（存储层自己）算成"无属主"。这类"字段的**存储**与字段的**使用**同名"
必须显式排除，否则工具会在正确的迁移之后报假红；判据是"**扫描范围要能说清为什么**"——
现在它只扫 `plan.c` 之外的文件，并把这条理由写在 `collect()` 的 docstring 里。


