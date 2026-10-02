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
| `substParams` / `substArgs` | **各 8** | 检查器的替换机制 | codegen **临时借用检查器的全局替换状态**来做实例代码生成 ⇒ 两条执行路径共用一份可变状态，顺序一变就错 |
| `used` | 4 | 检查器（"没被调用就不必复核"） | codegen 的遍历顺序**影响**后续复核行为 |
| `name` | 15 | 各方 | codegen 改名（去重）⇒ 名字不再等于解析结果 |
| `owSites` / `owLocal` | 各 3 | codegen 自己 | `@overwrite` 的站点表被 codegen 补齐（analysis 与 emission 混在一起） |
| `body` | 3 | 各方 | codegen 在 body 上挂东西 |
| `coroBoxed` | 1 | 检查器 + codegen | 装箱决定由 codegen 回写 |
| `ret` | 1 | 各方 | 同上 |

> 结论：**耦合不止"检查器写、codegen 读"这一半**；codegen 回写分析状态使"哪个 pass 先跑"
> 变成了语义问题（P0-5 的根因正是"实例物化＝重指节点"这一副作用）。

## 4. X 批次的动作（对应表里的族）

| 步 | 动作 | 覆盖 |
|---|---|---|
| X1 ✅ | 立 `src/plan.h`/`plan.c`：codegen 只经访问器读第 2 节的字段（182 个读点已收口）；**存储未动**；棘轮 `tools/check_plan_seam.py` 已接进 `check.sh` | 第 2 节全部 |
| X2 | 逐族把存储搬到计划侧（按节点寻址）：先 arena/zone 计划（`arenaLevel`/`zoneLevel`/`arenaArg`/`usesHome`/`mayUseArena`/`condAllocs`），再协程族（`isCoro`/`yieldType`/`coroFrameType`/`coroNeedsZone`/`coroProto`/`coroBoxed`），再名字族（`cname`/`instName`） | 第 2 节 |
| X3 | 实例集显式化：`func`/`tmpl` 的重指改为计划侧查表 + 封闭实例集 + 显式 worklist；**顺带切断第 3 节的反向写**（`substParams`/`substArgs` 改为显式传入） | 第 2、3 节 |
| X4 | AST 上只剩 ① 与"节点上的身份"，删死字段，更新本表与 `check_internal.h` 的权威节 | 全部 |
