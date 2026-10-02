/* 编译计划（compile plan）—— codegen 与"分析产物"之间的**唯一读口**（X 批次：X1）。
 *
 * ## 为什么有这个文件
 *
 * 过去 AST 同时充当三样东西：parser 的**输入**、检查器的**分析产物**、codegen 的**计划**。
 * 于是"哪个字段什么时候有效"只能靠注释，而 pass 顺序一改就可能静默错（P0-5 的根因正是
 * "实例物化＝重指节点"这一副作用）。X0 的盘点（docs/topics/AST-ANNOTATIONS.md）把这件事
 * 变成了清单：20 个"计划"字段、以及**codegen 反过来写分析状态**的那一半。
 *
 * ## 现在的状态（X1：只有接缝，没有搬家）
 *
 * 这些访问器**暂时仍读 AST/FuncDef 上的原字段**，所以行为完全不变。它们的作用是：
 *   1. 把 codegen 的读点**收口到一个文件**；
 *   2. 让 X2 的"搬家"（把存储挪到计划侧表）**只需要改这一个文件**；
 *   3. 在本文件里逐字段写明：谁写、何时有效、陈旧会怎样。
 *
 * **codegen 不允许再直接读**下面这些字段；新增读点请在这里加访问器。
 * 反向的那一半（codegen 写 `substParams`/`substArgs`/`used`/`name`/`owSites`/`body`…）
 * 属于 X3，见 AST-ANNOTATIONS.md 第 3 节。
 */
#ifndef EXTC_PLAN_H
#define EXTC_PLAN_H

#include "ast.h"

/* ---- ④ 身份 / 实例（X3 会把"重指"换成计划侧查表） ---------------------------------- */

/* 调用点解析到的**实例**（可能为 NULL：内建/操作符/未解析）。
 * 写者：检查器的延迟调用不动点（`e->func = instance`）。
 * 何时有效：`PHASE_POST` 的不动点跑完之后（在那之前可能还是模板或 provisional）。
 * 陈旧：生成物会去调用一个**不存在**的 C 函数（P0-5 的 `f_T`）。*/
FuncDef *planCallee(const Expr *e);

/* 实例 → 它的模板（泛型实例才有；非实例为 NULL）。
 * 写者：实例物化（`funcInstance`）。陈旧：取名/取类型参数会取错。*/
FuncDef *planTemplate(const FuncDef *f);

/* 实例的 C 名（`instName`）与表达式的 C 名（`cname`）。名字权威问题见 X4。*/
const char *planInstName(const FuncDef *f);

/* ---- ③ arena / zone 计划（X2 的第一族：P0-2/P0-6c 的历史都在这） -------------------- */

/* 分配站点的层（`new`/泛型调用）。写者：eSites 的 arena 通道。
 * 陈旧：生成物把对象放进**活得不够久**的地方 ⇒ use-after-free（P0-6c 那一族）。*/
int  planArenaLevel(const Expr *e);
/* 调用点的 arena 实参（`ARENA_HOME` 或某层）。写者：eSites 的 arena 通道。*/
int  planArenaArg(const Expr *e);
/* 池站点的 zone 层。写者：eSites 的 zone 通道（只对 `makesPool` 的被调者）。
 * 陈旧：池提前/滞后回收（P0-2 的 zone 通道、P0-20 一族）。*/
int  planZoneLevel(const Expr *e);
/* 被调者是否用 home arena / 可能分配 / 建池。`makesPool` 要等调用图闭包 ⇒ 它是
 * "三套延迟机制"里 `lvlFacts` 存在的原因之一。*/
bool planUsesHome(const FuncDef *f);
bool planMayUseArena(const FuncDef *f);
bool planMakesPool(const FuncDef *f);
/* 循环条件里有分配（定案 101②：按轮释放）。写者：检查器；读点：codegen 的循环生成。*/
bool planCondAllocs(const Stmt *s);
/* `for` 的步进语句（**半语法半计划**：parser 建，检查器可能替换）。*/
Stmt *planForStep(const Stmt *s);
/* 这个调用点需要临时量（P0-7：实参求值顺序）。*/
bool planNeedTemp(const Expr *e);

/* ---- ③ 协程族（X2 的第二族） ------------------------------------------------------- */

bool   planIsCoro(const FuncDef *f);
Type  *planYieldType(const FuncDef *f);
Type  *planCoroFrameType(const FuncDef *f);
/* 协程是否需要 zone（R12：任务的地方释放按身份，不看深度）。*/
bool   planCoroNeedsZone(const FuncDef *f);
/* 协议种类（next/value/send 的下标）—— 检查器解析、codegen 驱动用。*/
int planCoroProto(const FuncDef *f);
/* 载荷是否装箱（写者曾包含 codegen ⇒ X3 要切断）。*/
bool   planCoroBoxed(const FuncDef *f);

/* ---- 写入方（检查器）的 setter（X2 第二步：过渡期同时写侧表与 AST 字段） ----
 *
 * X1 只收口了**读**点，而耦合的另一半（谁在什么时候写）过去散在检查器里。setter 让
 * "写入"也变成可以加断言、可以换存储的地方：侧表与字段不一致时，`EXTC_DBG=1` 会在
 * 访问器里当场响。*/
void planSetArenaLevel(Expr *e, int v);
void planSetZoneLevel(Expr *e, int v);
void planSetArenaArg(Expr *e, int v);
void planSetNeedTemp(Expr *e, bool v);

#endif /* EXTC_PLAN_H */
