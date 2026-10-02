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
| ~~`coroBoxed`~~ | ~~1~~ | —— | **已清零**（X4 第三半）：那处回写（codegen 把模板的标记写进实例）在 X3 第四步就改成了就地推导；剩下的唯一写者 `check.c:456` 是**检查器**，本轮改成经 `planSetCoroBoxed` 写侧表 ⇒ **反向回写基线里真正的 AST 字段只剩 `coroKind` 1 处**（codegen 自己的 prepass 下标，见第 5 节） |

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
| `Expr.func`（调用点 → 实例） | **X3 的实例集显式化未完成**：一个调用点只有一个指针，而**泛型体的节点被所有实例共享** ⇒ 实例化时的重指"最后一个写赢"（第 6.3 节有实测：两个实例时 `f_i32` 既不发给 `g_i32`、也没被发出） | 实测 **12 写**（11 在 `check_expr.c`、1 在 `check_top.c:5207`）/ **56 直读** + codegen 54 处 `planCallee`。最小落地形态见 6.3：先封闭实例集，再把"重指"改成按（调用点，实例）记账 |
| ~~`FuncDef.tmpl`~~ | **已搬**（X4 第二半，见第 6 节末）：静态归属判清了属主与唯一写点，顺序风险也量清了（驱动两段 + `funcInstance` 不在 codegen 里） ⇒ 照 `instName` 的办法搬进侧表 | 搬迁后 AST 上已无此字段；`CallCheck.tmpl` **有意留在** `check_internal.h`（检查器内部、codegen 读点 0） |
| ~~`FuncDef.instName`~~ | **已搬**（X3 第六步）：1 写 / 11 读，与 `tmpl` 同族但**不需要**等实例集 —— 它只是"实例的 C 名" | 搬迁后 AST 上已无此字段 |
| `Stmt.forStep` | **半语法**：`parser.c` 两处写（`continue` 的 label 目标） | 留在 AST 是正确的；计划侧只该管「检查器替换后的那一半」（若有） |
| ~~`FuncDef.coroBoxed`~~ | **已搬**（X4 第三半，见第 6.2 节）：唯一写者在检查器、codegen 直读直写皆 0 | 搬迁后 AST 上已无此字段 |
| `FuncDef.coroKind` | **不是计划字段**：codegen 自己的 prepass 下标（handle 派发用），检查器读点 0 | 它唯一的问题是实现层面的：读点在 prepass **之前**就会执行 ⇒ **顺序依赖**。实测把它搬进 `CG` 自己的表 ⇒ 5 个 coro 用例红（生成物出现 `__extc_czh-1`）。要么保留并把「prepass 必须先于哪些读点」写进契约，要么把 prepass 提前 —— **不能只换存储位置** |
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

### 6.2 迁移记录（X4 第三半：`coroBoxed` 已落地）

**判据先行**（只读侦察的三条，与 `tmpl` 同形）：① 唯一写点在**检查器**（`check.c:456`，
"把帧强转成句柄"那一刻）；② codegen **直读 0、直写 0**（X3 第四步已把"模板标记写进实例"改成
就地推导，读点全经 `planCoroBoxed`）；③ 检查器读点只有一处（`check_top.c:6814/6826` 的
"协程是否需要 zone"）。⇒ 与 `tmpl` 同一配方，无顺序风险。

| 步骤 | 位置 |
|---|---|
| 加记录位 `PLAN_CORO_BOXED` + 槽位成员 + setter `planSetCoroBoxed` | `plan.c` |
| `planCoroBoxed` 改读侧表（未设置 ⇒ false） | `plan.c` / `plan.h` |
| 唯一写点改经 setter | `check.c:456`：`planSetCoroBoxed(got->sdef->coroOf, true)` |
| 读点改经访问器 | `check_top.c:6814`、`:6826` |
| 删字段 + 改注释 | `ast.h:710` 的 `bool coroBoxed` 删除，注释指向 `planCoroBoxed` |
| 棘轮 | `MOVED` 加 `"coroBoxed"` ⇒ AST 上已无 **16** 个字段 |

**至此 AST 上的"计划"字段只剩两个**：`Expr.func`（等实例集显式化）与 `Stmt.forStep`
（**半语法，有意保留**：parser 两处写）。`FuncDef.coroKind` 是**另一个问题**（codegen 自己的
prepass 下标，不是计划字段），它的"顺序依赖"结论见第 5 节。

### 6.3 `Expr.func` 的只读侦察（X3 第九步：现状、实测与最小落地形态）

**实测的规模**（剥注释/字符串后按基座判类型，脚本口径与第 6 节同一套）：

| 项 | 实测 | 说明 |
|---|---|---|
| `Expr.func` 写点 | **12** | `check_expr.c` 11（280/1073/1452/1485/1539/2115/2436/3145/3216/3503/3648）+ `check_top.c:5207`（`cc->node->func = inst`） |
| `Expr.func` 直读 | **56** | `check_top.c` 20 · `check_escape.c` 18 · `check_expr.c` 14 · `check_stmt.c` 2 · `check.c` 1 · `plan.c` 1（访问器本身） |
| codegen 读点 | **54 处 `planCallee`**，`->func` 直读 **0** | 已在 X1 收口 |
| 另一族同名字段 | 5 个写点 | `DeferredUse/RefCheck/OpCheck/CallCheck/MethodCheck` 的 `func` —— **不是** `Expr.func`（第 6 节的同类陷阱，别再数混） |
| codegen 回写 | **0** | 唯一的 `->func` 出现处是**注释**（`codegen.c:7206`） |

> 旧记录里"18 写 / 85 检查器读 / 58 codegen 读"是 X0 的 grep 估算（未剥注释、未分清 5 个
> 别的结构体）——**以本表为准**。

**写点的两种性质**（逐条看过）：

| 性质 | 写点 | 何时发生 |
|---|---|---|
| ① 普通解析：把调用点绑到**函数/方法/实例**（多数伴随 `used = true`） | 280、1073、1452、1485、1539、2115、3145、3503、3648（9 处，全在 `check_expr.c`） | 检查该调用点**当时** |
| ② **延迟重指**：泛型体里的调用点在**实例化时**才定 | 2436、3216（`check_expr.c`）、5207（`check_top.c` 的 `resolveDeferredCall`） | `checkModule` 的 **POST 不动点**里，按每个实例重跑一遍 |

**实测出的结构性问题（本轮最重要的发现）**：**一个调用点只有一个指针，而它可能属于多个实例。**
`funcInstance` 是浅拷贝 ⇒ 实例**共享**模板的函数体节点 ⇒ 泛型体里那个 `f(x)` 节点被
`g_i32` 与 `g_i64` **先后重指**，**只有最后一次写留下来**：

```extc
fn f<T>(x: T) -> T { return x }
fn g<T>(x: T) -> T { return f(x) }
fn main() -> i32 { var a: i32 = g(i32(9))  var b: i64 = g(i64(8))  println(a, " ", b)  return 0 }
```

* **一个实例时正确**：生成 `return f_i32(x);`
* **两个实例时错**：`g_i32` 与 `g_i64` **都**发 `return f_i64(x);`，而 **`f_i32` 根本没被发出**。

原因有两条，都在同一处：① 节点上的指针只剩最后一次写；② `used` 也挂在**实例**上，
而把调用点重指到 `f_i32` 的那一轮被覆盖 ⇒ `f_i32->used` 一直是 false ⇒ codegen 的
"没人调就不发"把 `f_i32` 剪掉。**这是"记账比真实情况小"的又一处出口**（第 0.2 节那台机器的
家族），也解释了为什么"只搬存储"没有意义：搬走之后**仍然是每个节点一个值**。

**它今天为什么没被闸门抓到（要诚实说清）**：生成的 C 是**合法 C**（`int64_t f_i64(int64_t)`
的结果隐式转回 `int32_t`），运行结果也对（`f` 是恒等函数，两种实例语义恰好相同），
闸门①②③只看"编得过/ASan 干净/与 C 同结论"，所以**都不响**。⇒ 要把它变成可判红的判据，
需要一条能**观察"哪个实例被调用"**的判据（例如让 `f` 的体依赖 `T` 的宽度），
而那需要语言层面写得出"依赖 T 的运算"—— 这一步留给实现之后。

**最小落地形态（本轮只侦察，未施工）**：

1. **封闭实例集**：不动点跑完后，把 `c.funcInsts`（+ `tt->instances`）当作**只读**集合，
   再进入"解析所有调用点"的最后一轮 —— 这一步保证"重指"之后不再有新实例产生（可断言）。
2. **按实例解析**：把 ② 的重指从"写节点"改成"**按（调用点，实例）记录**"——
   即计划侧一张 `(Expr*, 实例下标) -> 被调实例` 的表；`planCallee` 在某个实例的发射上下文里
   查这张表（上下文已经有了：`g->substParams`/实例本身）。
3. **`used` 也按实例记**（`used` 现在是 `FuncDef.used`，同样被共享写覆盖）；
   或者改成"凡是被 (2) 表引用到的实例都算 used"。
4. 判据：上面那个两实例程序生成 `g_i32 -> f_i32`、`g_i64 -> f_i64` 且 `f_i32` **被发出**；
   再加一条**负例探针**（构造"能看出被调实例不同"的程序）进 `tests/`。
   **只搬存储不做 1–3，等于把"最后一个写赢"换个地方发生。**




### 6.4 判据（闸门⑥：实例接线的结构判据）

**为什么需要一条新的判据**：6.3 那个缺陷**生成物仍是合法 C、跑出来也对**，
所以"编得过"（闸门①）／"ASan 干净"（闸门②）／"与等价 C 同结论"（闸门③）**都看不见它**。
能看见它的只有**结构**问题：*每个被发出的实例，调的是不是它该调的那个*。

**闸门⑥**（`tools/gate_callsite.py`，已接进 `check.sh`）：

* 探针在 `tools/callsite-corpus/*.extc`，每份配一个 `.expect`：

  ```
  require wrap_i32        # 这个实例必须**被发出**（有定义）
  call wrap_i32 pick_i32  # 这个实例的函数体必须调它
  ```

* **期望必须按语义写，不按今天的输出写** —— 否则闸门只会给 bug 背书。
  （已验证：把 `.expect` 改成"期望它调错的那个"⇒ 闸门就绿了 ⇒ 说明判据确实由 `.expect` 驱动。）
* 基线 `tools/gate-callsite-known-bad.txt` = **施工单**（棘轮：期望成立就从基线删那一行）；
  临时把基线清空 ⇒ 闸门当场判红（负例已验）。

**当前探针**（`shared_callsite_two_instances.extc`）：一个调用点、两个实例，其中一个是
**带 `+` 重载的结构体** —— 两种类型**不可互换**，所以"调错实例"不是审美问题：

```
require wrap_i32 / wrap_meter / pick_i32 / pick_meter
call wrap_i32 pick_i32        ← 今天错：它调 pick_meter
call wrap_meter pick_meter
```

**今天的红**（基线里那一条，两条都成立）：`pick_i32` 从未被发出；`wrap_i32` 调 `pick_meter`。
生成物在这一点上连 C 都编不过（`incompatible type for argument 1 of 'pick_meter'`）——
**但要注意顺序**：编不过只是"最后写赢"的一个副作用，换一下实例顺序就变成另一个方向
（`wrap_meter` 调 `pick_i32`）；所以判据不能写成"生成物编不过"，必须是上面那张结构表。

**修好之后的判据**：`gate_callsite.py` 绿（基线删空），且 `examples/generic-shared-callsite.extc`
的生成物里 `wrap_i32 -> pick_i32`、`wrap_i64 -> pick_i64` 且两个 `pick_*` 都被发出。

**对照组**：`tools/callsite-corpus/control_single_instance.extc` —— 同一形状、只**一个**实例，
今天已经是绿的（`wrap_i32 -> pick_i32`）。它的作用与闸门⑤ 的 `control_*` 一样：
证明这条闸门不是"一律判红"，并**永远不许进基线**（已机械化：基线里 0 个 control）。
