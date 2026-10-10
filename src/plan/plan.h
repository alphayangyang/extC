/**
 * @file plan.h
 * @brief 编译计划：代码生成读取"检查器决定了什么"的唯一读口。
 *
 * 计划存在按节点寻址的侧表里，所有读写都经过下面的访问器与 setter。
 * 每个字段的注释写明：谁写它、什么时候有效、它过期时生成物会怎样。
 *
 * 分层契约与不变量：
 *   - 检查器只通过 setter 写计划字段，代码生成只通过访问器读。codegen 直读计划字段会被
 *     `tools/check_plan_seam.py` 挡下。
 *   - 新增计划字段进侧表（配 setter 与访问器），不要加到 AST 节点上。
 *   - 少数字段仍读 AST 节点：解析后的被调用者（`Expr.func`，`planCallee`）与 `for` 的步进
 *     （`Stmt.forStep`，`planForStep`）。`Expr.func` 的指针由实例物化本身产生，搬进侧表要等
 *     实例集合变成显式的；`forStep` 有一半是语法（解析器拥有另一半），留在节点上是分工。
 */
#ifndef EXTC_PLAN_H
#define EXTC_PLAN_H

#include "ast.h"
#include "results.h"

/* ---- identity and instances ------------------------------------------------------- */

/**
 * @brief 取一个调用点当前解析到的被调函数。
 *
 * 泛型体里的一个调用点会被该体的每个实例共享；代码生成用 `planEnterFunc` / `planEnterInst`
 * 框出正在生成的那具函数体或类型实例，本函数优先返回为该体记录的那一份解析结果。
 * 不属于任何实例的调用点返回记录在节点上的那一份。
 *
 * @param e  调用点（或函数值）表达式。
 * @return   解析到的函数定义；内建、运算符、尚未解析以及没有记录时返回 NULL。
 */
FuncDef *planCallee(const Expr *e);

/**
 * @brief 判断一个函数是否被调用过。
 *
 * 代码生成只发出被调用过的函数，外加 `main`、`@export` 与 `dyn` 表项这些根。
 * 没有记录过调用时读作未使用，这正是安全方向：漏掉的调用点不会让产物里多出一具没人引用的函数体。
 *
 * @param f  函数定义。
 * @return   记录过调用则返回 true；没有记录时返回 false。
 */
bool planUsed(const FuncDef *f);

/**
 * @brief 记录一个函数是否被调用过。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否被调用。
 */
void planSetUsed(FuncDef *f, bool v);

/**
 * @brief 取一个绑定或变量语句生成时使用的 C 名。
 *
 * 参数不在这里：参数按值存储、自带字段，所以本函数不接受参数节点。
 *
 * @param node  绑定表达式或变量声明语句节点。
 * @return      记录下来的 C 名；没有记录或记录为空串时返回 NULL，调用方应回落到声明自己的名字。
 */
const char *planCName(const void *node);

/**
 * @brief 记录一个绑定或变量语句生成时使用的 C 名。
 *
 * @param node  绑定表达式或变量声明语句节点；为 NULL 时忽略本次写入。
 * @param name  C 名。
 * @param kind  该节点的种类（`RKIND_EXPR` 或 `RKIND_STMT`），用于结果槽的类型检查。
 */
void planSetCName(void *node, const char *name, ResultKind kind);

/* ---- struct-definition facts -------------------------------------------------------
 *
 * `builtinHolder` / `coroOf` 由代码生成读取，所以是计划事实；两个 `anXxx` 是检查器自己的
 * 工作状态，没有更晚的阶段读它们。两者共用同一个槽，但读者不同（分析侧清单是
 * `planAnalysisFields`）。 */

/**
 * @brief 判断这个结构体定义是不是某个内建类型的合成占位结构体。
 *
 * 这种占位结构体没有字段也没有布局，生成 C 时不为它声明结构体；它的方法照常按普通函数发出。
 *
 * @param sd  结构体定义。
 * @return    是内建占位结构体则返回 true；否则返回 false。
 */
bool     planBuiltinHolder(const StructDef *sd);

/**
 * @brief 记录一个结构体定义是不是内建类型的合成占位结构体。
 *
 * @param sd  结构体定义；为 NULL 时忽略本次写入。
 * @param v   是否为内建占位结构体。
 */
void     planSetBuiltinHolder(StructDef *sd, bool v);

/**
 * @brief 取一个协程帧结构体所属的协程函数。
 *
 * 帧结构体由检查器为每个协程合成；生成 C 时用这里记下的函数名把帧类型与 `$step` / `$next`
 * 函数拼在一起。
 *
 * @param sd  结构体定义（合成的帧结构体）。
 * @return    所属的协程函数；该结构体不是协程帧时返回 NULL。
 */
FuncDef *planCoroOf(const StructDef *sd);

/**
 * @brief 记录一个协程帧结构体所属的协程函数。
 *
 * @param sd  合成的帧结构体；为 NULL 时忽略本次写入。
 * @param f   所属的协程函数。
 */
void     planSetCoroOf(StructDef *sd, FuncDef *f);

/**
 * @brief 取 lambda 环境结构体上记下的用户所写签名。
 *
 * 环境结构体是编译器合成的；诊断里显示这个字符串，于是用户看到的是自己写的
 * `fn(i64) -> i64`，而不是生成的类型名。
 *
 * @param sd  结构体定义（lambda 的环境结构体）。
 * @return    记下的签名字符串；不是 lambda 环境结构体或没有记录时返回 NULL。
 */
const char *anLamSig(const StructDef *sd);

/**
 * @brief 记下 lambda 环境结构体上用户所写的签名。
 *
 * @param sd    lambda 的环境结构体；为 NULL 时忽略本次写入。
 * @param sig   用户写的签名字符串。
 */
void        anSetLamSig(StructDef *sd, const char *sig);

/**
 * @brief 判断这个结构体是否有方法会建池。
 *
 * 这是结构体一级的汇总：某个方法建池，则它的关联函数（构造器一族）也可能建池，
 * 池层级与隐藏 zone 参数的判断要算上这一层。
 *
 * @param sd  结构体定义。
 * @return    有方法建池则返回 true；没有记录时返回 false。
 */
bool        anMakesPoolAny(const StructDef *sd);

/**
 * @brief 记录这个结构体是否有方法会建池。
 *
 * @param sd  结构体定义；为 NULL 时忽略本次写入。
 * @param v   是否有方法建池。
 */
void        anSetMakesPoolAny(StructDef *sd, bool v);

/* ---- the effect closure's state ----------------------------------------------------
 *
 * `effState` 是闭包的工作状态（`EFF_NONE` = 未计算，`EFF_DONE` = 完成，
 * `EFF_IN_PROGRESS` = 计算中）；`effComplete` /
 * `effUnknown` 是结果（"这份摘要可信吗"）。三者都不被代码生成读取，属于分析侧。 */

/**
 * @brief 取效应闭包的计算状态。
 *
 * 取值是 `EFF_NONE`（还没算）、`EFF_DONE`（算完了，可不可信看 `anEffComplete`）或
 * `EFF_IN_PROGRESS`（正在算——递归时再遇到就是环，摘要按不完整处理）。
 * 只有效应闭包自己读写这个值。
 *
 * @param f  函数定义。
 * @return   状态值；没有记录时返回 `EFF_NONE`。
 */
int  anEffState(const FuncDef *f);

/**
 * @brief 记录效应闭包的计算状态。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  状态值：`EFF_NONE`、`EFF_DONE` 或 `EFF_IN_PROGRESS`。
 */
void anSetEffState(FuncDef *f, int v);

/**
 * @brief 判断一个函数的效应摘要是否可信。
 *
 * 闭包合并被调者摘要时，只要有一个被调者算不完（在环上、或调用了没解析出来的东西），
 * 就把这里置为假；调用点据此走最保守的一档检查。
 *
 * @param f  函数定义。
 * @return   摘要可信则返回 true；没有记录时返回 false。
 */
bool anEffComplete(const FuncDef *f);

/**
 * @brief 记录一个函数的效应摘要是否可信。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  摘要是否可信。
 */
void anSetEffComplete(FuncDef *f, bool v);

/**
 * @brief 判断一个函数的效应摘要里是否有没能解析的调用。
 *
 * 有的话摘要永远不算完成，调用点按最保守的一档处理。
 *
 * @param f  函数定义。
 * @return   有未解析的调用则返回 true；没有记录时返回 false。
 */
bool anEffUnknown(const FuncDef *f);

/**
 * @brief 记录一个函数的效应摘要里是否有没能解析的调用。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否有未解析的调用。
 */
void anSetEffUnknown(FuncDef *f, bool v);

/* ---- effect summary ----------------------------------------------------------------
 *
 * 传递闭包后的掩码：一次对某函数的调用可能把那个参数的地址/内容存进调用者的帧
 * （`Addr`/`Cont`）、存进这个函数自己的 home arena（`home…`），或存到别处（`otherMask`）。
 * 只有检查器读它们。闭包自己的状态与它走的调用图（`callees`）是另一件事，属于 pass 状态。 */

/**
 * @brief 取「可能把参数的地址存进调用方帧」的参数位掩码。
 *
 * 第 i 位表示第 i 个参数的地址（或经它可达的字段的地址）可能被存进调用方的帧，
 * 因此该实参指向的存储必须活到目的地那么久。只有检查器读它。
 *
 * @param f  函数定义。
 * @return   位掩码；没有记录时返回 0。
 */
uint64_t anAddrMask(const FuncDef *f);

/**
 * @brief 记录「可能把参数的地址存进调用方帧」的参数位掩码。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  位掩码。
 */
void anSetAddrMask(FuncDef *f, uint64_t v);

/**
 * @brief 取「可能把从参数里读出的指针存进调用方帧」的参数位掩码。
 *
 * 第 i 位表示从第 i 个参数里读出的指针可能被存进调用方的帧，因而它指向的存储必须活到
 * 目的地那么久。只有检查器读它。
 *
 * @param f  函数定义。
 * @return   位掩码；没有记录时返回 0。
 */
uint64_t anContMask(const FuncDef *f);

/**
 * @brief 记录「可能把从参数里读出的指针存进调用方帧」的参数位掩码。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  位掩码。
 */
void anSetContMask(FuncDef *f, uint64_t v);

/**
 * @brief 取「流向无法归属到某个参数」的位掩码。
 *
 * 任何说不清的存储都记在这里；调用点读到非零就按最严格的一档处理。只有检查器读它。
 *
 * @param f  函数定义。
 * @return   位掩码；没有记录时返回 0。
 */
uint64_t anOtherMask(const FuncDef *f);

/**
 * @brief 记录「流向无法归属到某个参数」的位掩码。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  位掩码。
 */
void anSetOtherMask(FuncDef *f, uint64_t v);

/**
 * @brief 取「可能把参数的地址存进本函数 home arena」的参数位掩码。
 *
 * 第 i 位表示第 i 个参数的地址（或经它可达的字段的地址）可能被存进本函数自己的 home arena。
 * 只有检查器读它。
 *
 * @param f  函数定义。
 * @return   位掩码；没有记录时返回 0。
 */
uint64_t anHomeAddrMask(const FuncDef *f);

/**
 * @brief 记录「可能把参数的地址存进本函数 home arena」的参数位掩码。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  位掩码。
 */
void anSetHomeAddrMask(FuncDef *f, uint64_t v);

/**
 * @brief 取「可能把从参数里读出的指针存进本函数 home arena」的参数位掩码。
 *
 * 第 i 位表示从第 i 个参数里读出的指针可能被存进本函数自己的 home arena。只有检查器读它。
 *
 * @param f  函数定义。
 * @return   位掩码；没有记录时返回 0。
 */
uint64_t anHomeContMask(const FuncDef *f);

/**
 * @brief 记录「可能把从参数里读出的指针存进本函数 home arena」的参数位掩码。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  位掩码。
 */
void anSetHomeContMask(FuncDef *f, uint64_t v);

/**
 * @brief 判断一个函数是否把本帧某个局部变量的地址存了出去。
 *
 * 这样的调用是调用点必须拒绝的：局部变量的存储活不过本帧，而被存下的地址会活得更久。
 *
 * @param f  函数定义。
 * @return   存过本帧局部的地址则返回 true；没有记录时返回 false。
 */
bool     anAddrFromLocal(const FuncDef *f);

/**
 * @brief 记录一个函数是否把本帧某个局部变量的地址存了出去。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否存过本帧局部的地址。
 */
void anSetAddrFromLocal(FuncDef *f, bool v);

/**
 * @brief 取代码生成前导阶段分配给这个协程的编号。
 *
 * 句柄里的 kind 字段与调度域的任务表都用这个编号来分派到对应的 `$step` / `$next`。
 *
 * @param f  协程函数定义。
 * @return   编号；没有记录时返回 0。
 */
int      planCoroKind(const FuncDef *f);

/**
 * @brief 记录代码生成前导阶段分配给这个协程的编号。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param v  编号。
 */
void planSetCoroKind(FuncDef *f, int v);

/**
 * @brief 判断这个函数的函数体是否被 `dyn` 虚表 thunk 从外部点名。
 *
 * 这种引用在「没有任何名字提到就不发出」的裁剪里看不见，因此要把它记下来，函数体必须保留。
 *
 * @param f  函数定义。
 * @return   被 `dyn` 虚表点名则返回 true；没有记录时返回 false。
 */
bool     planDynTable(const FuncDef *f);

/**
 * @brief 记录这个函数的函数体是否被 `dyn` 虚表 thunk 从外部点名。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否被 `dyn` 虚表点名。
 */
void planSetDynTable(FuncDef *f, bool v);

/**
 * @brief 取这个函数体里 `@overwrite` 复用站点的个数。
 *
 * 生成 C 时用这个计数决定要不要为复用站点准备隐藏的存储并传递它。只有检查器写它。
 *
 * @param f  函数定义。
 * @return   站点个数；没有记录时返回 0。
 */
int      planOwSites(const FuncDef *f);

/**
 * @brief 记录这个函数体里 `@overwrite` 复用站点的个数。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  站点个数。
 */
void planSetOwSites(FuncDef *f, int v);

/**
 * @brief 判断 `@overwrite` 复用站点的存储是否放在本函数自己的帧里。
 *
 * 为真表示每次调用各有自己的一块存储（`main`，或者会递归到自己的函数）；
 * 为假表示存储由调用点的帧持有，并作为不透明的隐藏参数传进来。
 *
 * @param f  函数定义。
 * @return   存储在本函数帧里则返回 true；没有记录时返回 false。
 */
bool     planOwLocal(const FuncDef *f);

/**
 * @brief 记录 `@overwrite` 复用站点的存储是否放在本函数自己的帧里。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否放在本函数帧里。
 */
void planSetOwLocal(FuncDef *f, bool v);

/**
 * @brief 取协程声明的返回类型（尚未代入类型实参的那一份）。
 *
 * 物化泛型实例时用它按类型实参替换出实例真正的返回类型。
 *
 * @param f  协程函数定义（模板）。
 * @return   声明的返回类型；不是协程或没有记录时返回 NULL。
 */
Type    *planCoroRetProto(const FuncDef *f);

/**
 * @brief 记录协程声明的返回类型（尚未代入类型实参的那一份）。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param t  声明的返回类型；不是协程时传 NULL。
 */
void     planSetCoroRetProto(FuncDef *f, Type *t);

/**
 * @brief 判断这个表达式在生成时是否要套一层解引用。
 *
 * 值的类型是 `ref T` 而它所在的位置需要一个 `T`，生成 C 时就写成 `*(...)`。
 *
 * @param e  表达式。
 * @return   需要解引用则返回 true；没有记录时返回 false。
 */
bool     planDeref(const Expr *e);

/**
 * @brief 记录这个表达式在生成时是否需要套一层解引用。
 *
 * @param e  表达式；为 NULL 时忽略本次写入。
 * @param v  是否需要解引用。
 */
void planSetDeref(Expr *e, bool v);

/**
 * @brief 判断这个协程调用是否要装箱成句柄来存储。
 *
 * 为真表示协程的值要存到活过当前 C 作用域的地方：生成 C 时把帧放进任务自己的 place，
 * 并把值做成 `{frame, kind, task}` 句柄。
 *
 * @param e  协程调用表达式。
 * @return   需要装箱则返回 true；没有记录时返回 false。
 */
bool     planBoxedCoro(const Expr *e);

/**
 * @brief 记录这个协程调用是否要装箱成句柄来存储。
 *
 * @param e  协程调用表达式；为 NULL 时忽略本次写入。
 * @param v  是否需要装箱。
 */
void planSetBoxedCoro(Expr *e, bool v);

/**
 * @brief 判断这个函数是否被 `ext` 当成任务启动。
 *
 * 生成 C 时只为这样的协程发出调度域需要的适配器，以及 `$step` / `$next` 的前置声明。
 *
 * @param f  函数定义。
 * @return   是 `ext` 的目标则返回 true；没有记录时返回 false。
 */
bool     planIsExtTarget(const FuncDef *f);

/**
 * @brief 记录这个函数是否被 `ext` 当成任务启动。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否是 `ext` 的目标。
 */
void planSetIsExtTarget(FuncDef *f, bool v);

/**
 * @brief 判断这个函数是否被 `parallel::run` 用作 worker。
 *
 * 语言层没有函数指针，生成 C 时只为这样的函数写一个 static trampoline 来取它的地址。
 *
 * @param f  函数定义。
 * @return   用作 worker 则返回 true；没有记录时返回 false。
 */
bool     planIsParWorker(const FuncDef *f);

/**
 * @brief 记录这个函数是否被 `parallel::run` 用作 worker。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否用作 worker。
 */
void planSetIsParWorker(FuncDef *f, bool v);

/**
 * @brief 判断这个函数里的 `new` 是否要用线程本地 arena。
 *
 * worker 的签名固定、分配逃不出去，所以线程本地是安全的；而帧 arena 是主线程的，
 * 两个 worker 同时用就是数据竞争。这条调用链上的函数都记这个标记。
 *
 * @param f  函数定义。
 * @return   用线程本地 arena 则返回 true；没有记录时返回 false。
 */
bool     planParTlsArena(const FuncDef *f);

/**
 * @brief 记录这个函数里的 `new` 是否要用线程本地 arena。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否用线程本地 arena。
 */
void planSetParTlsArena(FuncDef *f, bool v);

/* ---- per-function analysis facts ---------------------------------------------------
 *
 * 这些事实按节点槽存储。`funcInstance` 用 `*in = *tmpl` 做浅拷贝，槽不随拷贝走，所以
 * `planInheritFuncFacts` 必须在拷贝后立刻把它们继承到实例上。 */

/**
 * @brief 把模板函数的每函数分析事实搬到新物化的实例上。
 *
 * 实例是模板的浅拷贝（`*in = *tmpl`），而结果槽按节点指针索引，不会被这次拷贝带过来；
 * 所以拷贝之后必须立刻调用本函数，把模板的分析块与已写位掩码并到实例的槽里。
 *
 * @param in    通过浅拷贝新建的实例。
 * @param tmpl  模板函数。
 */
void planInheritFuncFacts(FuncDef *in, const FuncDef *tmpl);

/**
 * @brief 判断这个函数是不是关联函数。
 *
 * 没有 `self` 参数的函数属于类型但不作用于某个值（`point::origin()`），调用点必须写全类型名。
 *
 * @param f  函数定义。
 * @return   是关联函数则返回 true；没有记录时返回 false。
 */
bool anIsAssoc(const FuncDef *f);

/**
 * @brief 记录这个函数是不是关联函数。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否是关联函数。
 */
void anSetIsAssoc(FuncDef *f, bool v);

/**
 * @brief 判断这个函数体是否已经在写 lambda 的那个作用域里检查过。
 *
 * lambda 合成的 `call` 方法体在捕获集所在的作用域就地检查过，检查结构体方法的这一遍必须跳过它。
 *
 * @param f  函数定义（lambda 的 `call` 方法）。
 * @return   已检查过则返回 true；没有记录时返回 false。
 */
bool anLamChecked(const FuncDef *f);

/**
 * @brief 记录这个函数体是否已经在写 lambda 的那个作用域里检查过。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否已检查过。
 */
void anSetLamChecked(FuncDef *f, bool v);

/**
 * @brief 判断这个 lambda 的返回类型是否还没写、要由第一个 `return` 决定。
 *
 * 为真时，第一个带值的 `return` 会把它的类型填进去；填好之后这个标记就置回假。
 *
 * @param f  函数定义（lambda 的 `call` 方法）。
 * @return   要推断返回类型则返回 true；没有记录时返回 false。
 */
bool anLamInferRet(const FuncDef *f);

/**
 * @brief 记录这个 lambda 的返回类型是否还没写、要由第一个 `return` 决定。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否要推断返回类型。
 */
void anSetLamInferRet(FuncDef *f, bool v);

/**
 * @brief 判断这个函数是否需要自己的 home arena。
 *
 * 函数体里有 `new`，并且返回类型里含引用或视图，或者会通过出参往外存：这样的函数多收一个
 * 隐藏的 home arena 参数。这个问题沿调用图做闭包，所以「需要」是传递的。
 *
 * @param f  函数定义。
 * @return   需要自己的 home arena 则返回 true；没有记录时返回 false。
 */
bool anNeedsHome(const FuncDef *f);

/**
 * @brief 记录这个函数是否需要自己的 home arena。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  是否需要自己的 home arena。
 */
void anSetNeedsHome(FuncDef *f, bool v);

/**
 * @brief 取「这个函数会不会分配」的备忘状态。
 *
 * 0 表示还没算，1 表示会分配（直接有 `new`，或经被调者分配），2 表示不会，
 * 3 表示正在算——在环上按会分配处理，这是保守方向。
 *
 * @param f  函数定义。
 * @return   状态值；没有记录时返回 0。
 */
int  anAllocState(const FuncDef *f);

/**
 * @brief 记录「这个函数会不会分配」的备忘状态。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  状态值：0 未算、1 会、2 不会、3 正在算。
 */
void anSetAllocState(FuncDef *f, int v);

/**
 * @brief 取「这个函数会不会打印」的备忘状态。
 *
 * 0 表示还没算，1 表示会打印（直接有 `print` / `println`，或经被调者打印），2 表示不会，
 * 3 表示正在算——在环上按会打印处理。
 *
 * @param f  函数定义。
 * @return   状态值；没有记录时返回 0。
 */
int  anMayPrintState(const FuncDef *f);

/**
 * @brief 记录「这个函数会不会打印」的备忘状态。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  状态值：0 未算、1 会、2 不会、3 正在算。
 */
void anSetMayPrintState(FuncDef *f, int v);

/**
 * @brief 取收集这个函数效应时记下的新鲜局部名的个数。
 *
 * 「新鲜」指这个名字当时绑定的对象由 `new` 产生；往容器里存这样的值时不需要寿命约束。
 *
 * @param f  函数定义。
 * @return   记下的名字个数；没有记录时返回 0。
 */
int  anFreshCount(const FuncDef *f);

/**
 * @brief 记录收集这个函数效应时记下的新鲜局部名的个数。
 *
 * @param f  函数定义；为 NULL 时忽略本次写入。
 * @param v  名字个数。
 */
void anSetFreshCount(FuncDef *f, int v);

/**
 * @brief 取一个函数记录下来的分配站点列表。
 *
 * 列表里是函数体里每个可能分配的表达式：`new`，以及可能分配存储的调用。分配放置阶段按它逐站点
 * 决定 arena 层级；这是检查器自己的工作状态，后续阶段不读。泛型实例不单独收集，直接指向其模板的
 * 那一份列表。
 *
 * @param f  函数定义。
 * @return   分配站点向量；没有记录或 `f` 为 NULL 时返回空向量。
 */
Vec  anArenaSites(const FuncDef *f);

/**
 * @brief 记下一个函数的 arena 待决站点清单。
 *
 * 清单里既有分配点表达式，也有 arena 实参还没定的调用点，交给分析收尾之后的 pass 逐站决定
 * 层级。实例化出来的函数从它的模板抄走同一份清单。
 *
 * @param f  目标函数。
 * @param v  站点向量，元素为 `Expr *`。
 */
void anSetArenaSites(FuncDef *f, Vec v);

/**
 * @brief 取一个函数已登记的形参符号个数。
 *
 * @param f  目标函数。
 * @return   已登记的个数；没有记录时为 0，上限为 `EFF_MAX_PARAMS`。
 */
int  anParamSymCount(const FuncDef *f);

/**
 * @brief 设置一个函数已登记的形参符号个数。
 *
 * @param f  目标函数。
 * @param v  新的个数。
 */
void anSetParamSymCount(FuncDef *f, int v);

/**
 * @brief 取一个函数存放形参符号的可写数组。
 *
 * 数组容量固定，前 `anParamSymCount` 个元素有效；调用会顺带建好结果槽。
 *
 * @param f  目标函数。
 * @return   数组首地址；`f` 为 NULL 或建槽失败时为 NULL。
 */
void **anParamSyms(FuncDef *f);

/* ---- operator / comparison facts ---------------------------------------------------
 *
 * `planConvCheck` / `planNeedOp` 由代码生成读取（据此知道还有延迟的运算符要重新解析、
 * 一次窄化转换要带运行期检查）；其余 `anXxx` 是检查器自己的状态。 */

/**
 * @brief 取一个值必须活到的最浅层级。
 *
 * `-1` 表示还没有任何约束落到这个节点上；数值越小寿命要求越长。
 *
 * @param node  要查的节点。
 * @return      已记录的层级；该字段没写过时为 0。
 */
int  anMinAt(const void *node);

/**
 * @brief 记下一个值必须活到的最浅层级。
 *
 * @param node  目标节点。
 * @param v     层级；`-1` 表示尚无约束。
 */
void anSetMinAt(void *node, int v);

/**
 * @brief 判断一个值所在存储是不是被逐轮复用（`@overwrite` 的初始化式）。
 *
 * @param node  要查的节点。
 * @return      是则 `true`；没有记录时为 `false`。
 */
bool anReuse(const void *node);

/**
 * @brief 标记一个值所在存储会被逐轮复用。
 *
 * @param node  目标节点。
 * @param v     为 `true` 表示它的层级固定在本帧第 1 层。
 */
void anSetReuse(void *node, bool v);

/**
 * @brief 判断一个值是不是从别处借来的引用。
 *
 * @param node  要查的节点。
 * @return      是借来的引用则 `true`；没有记录时为 `false`。
 */
bool anBorrowed(const void *node);

/**
 * @brief 记下一个值是不是从别处借来的引用。
 *
 * @param node  目标节点。
 * @param v     借来的引用为 `true`。
 */
void anSetBorrowed(void *node, bool v);

/**
 * @brief 判断一个名字是不是已按限定名解析过（源码写了 `a::b`，或已由限定名 pass 定下）。
 *
 * @param node  要查的节点。
 * @return      已限定则 `true`；没有记录时为 `false`。
 */
bool anQualified(const void *node);

/**
 * @brief 标记一个名字已按限定名解析，后续 pass 不要再按本模块的名字表改写它。
 *
 * @param node  目标节点。
 * @param v     已限定为 `true`。
 */
void anSetQualified(void *node, bool v);

/**
 * @brief 判断一次显式转换要不要在运行期做范围检查。
 *
 * 整数加宽、浮点到浮点，以及引用转 64 位整数都不需要检查；其余收窄、变号与浮点转整数
 * 都要。
 *
 * @param e  转换表达式。
 * @return   要生成运行期检查则 `true`；没有记录时为 `false`。
 */
bool planConvCheck(const Expr *e);

/**
 * @brief 记下一次显式转换要不要在运行期做范围检查。
 *
 * @param e  转换表达式。
 * @param v  要检查为 `true`。
 */
void planSetConvCheck(Expr *e, bool v);

/**
 * @brief 判断一次二元运算是不是因为操作数是类型参数而推迟到实例化再解析。
 *
 * @param e  二元表达式。
 * @return   需要重新解析则 `true`；没有记录时为 `false`。
 */
bool planNeedOp(const Expr *e);

/**
 * @brief 记下一次二元运算需要推迟到实例化再解析。
 *
 * @param e  二元表达式。
 * @param v  需要重新解析为 `true`。
 */
void planSetNeedOp(Expr *e, bool v);

/* ---- call / receiver facts ---------------------------------------------------------
 *
 * 全部由代码生成读取：任务交给谁（`EX_EXT`）、一个 `EX_FIELD` 解析到了哪个字段、
 * 一个 `EX_ASSOC` 解析到了哪个实例类型、以及 `dyn` 调用怎么到达接收者。 */

/**
 * @brief 取 `ext` 任务被交给的那个域表达式。
 *
 * @param e  `ext` 调用节点。
 * @return   域表达式；没有记录时为 NULL。
 */
Expr     *planExtDom(const Expr *e);

/**
 * @brief 记下 `ext` 任务被交给的域表达式。
 *
 * @param e  `ext` 调用节点。
 * @param v  域表达式。
 */
void planSetExtDom(Expr *e, Expr *v);

/**
 * @brief 取字段访问节点解析到的字段定义。
 *
 * 表字段上的 `fn` 值也走这一格，调用点靠它拿到槽位的 effect 子句。
 *
 * @param e  字段访问节点。
 * @return   命中的字段定义；没有记录时为 NULL。
 */
FieldDef *planField(const Expr *e);

/**
 * @brief 记下字段访问节点解析到的字段定义。
 *
 * @param e   字段访问节点。
 * @param fd  命中的字段定义。
 */
void planSetField(Expr *e, FieldDef *fd);

/**
 * @brief 取关联项调用或枚举值节点解析到的具体类型。
 *
 * 代码生成用它取实例化之后的类型名。
 *
 * @param e  关联项调用或枚举值节点。
 * @return   解析到的类型；没有记录时为 NULL。
 */
Type     *planAssocOwner(const Expr *e);

/**
 * @brief 记下关联项调用或枚举值节点解析到的具体类型。
 *
 * @param e  关联项调用或枚举值节点。
 * @param t  解析到的类型。
 */
void planSetAssocOwner(Expr *e, Type *t);

/**
 * @brief 判断一次 `dyn` 方法调用的接收者是不是经由引用拿到的。
 *
 * @param e  方法调用节点。
 * @return   接收者经引用到达则 `true`；没有记录时为 `false`。
 */
bool     planDynRecvViaRef(const Expr *e);

/**
 * @brief 记下一次 `dyn` 方法调用的接收者是不是经由引用拿到的。
 *
 * @param e  方法调用节点。
 * @param v  经引用到达为 `true`。
 */
void planSetDynRecvViaRef(Expr *e, bool v);

/**
 * @brief 判断一次调用是不是通过函数值发起，而不是通过具名声明。
 *
 * @param e  调用节点。
 * @return   经函数值调用则 `true`；没有记录时为 `false`。
 */
bool     planCallViaFn(const Expr *e);

/**
 * @brief 记下一次调用是通过函数值发起的。
 *
 * @param e  调用节点。
 * @param v  经函数值调用为 `true`。
 */
void planSetCallViaFn(Expr *e, bool v);

/* ---- depth and origin facts --------------------------------------------------------
 *
 * 参数写成 `void *`：`refDepth` 既在 `Expr` 上也在检查器的 `Sym` 上，一个访问器同时回答两者。 */

/**
 * @brief 取一个节点上记录的引用/逃逸深度。
 *
 * 数值越大表示所在的块越深、寿命越短；`0` 表示本帧之外那一层，即调用者选定的寿命，同时也是
 * 没有记录时的默认值。同一个存取器既服务表达式，也服务检查器的绑定，所以参数是 `void *`。
 *
 * @param node  要查的节点。
 * @return      已记录的深度；没有记录时为 0。
 */
int  anRefDepth(const void *node);

/**
 * @brief 记下一个节点的引用/逃逸深度。
 *
 * @param node  目标节点。
 * @param v     深度；0 表示本帧之外那一层。
 */
void anSetRefDepth(void *node, int v);

/**
 * @brief 取一次调用的结果留在哪一层。
 *
 * `-1` 表示结果活得比本帧久，调用要把自己的 home arena 交出去；`>= 1` 表示结果留在那个显式
 * 块层级上；`0` 表示没有 `mut ref` 实参给出理由，交给最终 pass 按当前块定。
 *
 * @param node  调用节点。
 * @return      已记录的深度；没有记录时为 0。
 */
int  anHomeDepth(const void *node);

/**
 * @brief 记下一次调用的结果留在哪一层。
 *
 * 它与该调用实际往下传的 arena 实参必须一起更新，两者不能各说一套。
 *
 * @param node  调用节点。
 * @param v     结果所在层级；`-1` 表示比本帧久。
 */
void anSetHomeDepth(void *node, int v);

/**
 * @brief 取一个分配点最初所在的块层级。
 *
 * arena 层级可能被改写成 home 哨兵，词法层级因此单独留一格。
 *
 * @param node  分配点节点。
 * @return      已记录的层级；没有记录时为 0。
 */
int  anLexicalLevel(const void *node);

/**
 * @brief 记下一个分配点最初所在的块层级。
 *
 * @param node  分配点节点。
 * @param v     块层级。
 */
void anSetLexicalLevel(void *node, int v);

/**
 * @brief 查一个值最近一次被发布到哪个层级。
 *
 * `-1` 表示还没见过它被发布；`0` 也是真实答案，表示发布到本帧之外。
 *
 * @param node  要查的值节点。
 * @return      发布层级；没有记录时为 -1。
 */
int  anStoredAt(const void *node);

/**
 * @brief 记下一个值被发布到的层级。
 *
 * @param node  目标值节点。
 * @param v     发布层级。
 */
void anSetStoredAt(void *node, int v);

/* ---- call-site markers for builtin forms -------------------------------------------
 *
 * 内建形式没有可用来识别它的被调用者，所以检查器给**调用节点**打标记，代码生成按标记识别。 */

/**
 * @brief 取 `parallel::run(worker, ...)` 调用里的 worker 函数。
 *
 * @param e  调用节点。
 * @return   worker 函数；不是这种调用时为 NULL。
 */
FuncDef *planParWorker(const Expr *e);

/**
 * @brief 记下 `parallel::run(worker, ...)` 调用里的 worker 函数。
 *
 * @param e   调用节点。
 * @param wf  worker 函数。
 */
void     planSetParWorker(Expr *e, FuncDef *wf);

/**
 * @brief 判断这个调用是不是构造一个域的 builtin 形式。
 *
 * 这种形式没有可依赖的被调者指针，所以由调用节点上的标记识别。
 *
 * @param e  调用节点。
 * @return   是则 `true`；没有记录时为 `false`。
 */
bool     planDomNew(const Expr *e);

/**
 * @brief 标记这个调用是构造一个域的 builtin 形式。
 *
 * @param e  调用节点。
 * @param v  是则为 `true`。
 */
void     planSetDomNew(Expr *e, bool v);

/**
 * @brief 判断这个调用是不是 `extc_viewOf` 这个没有函数体的 builtin 形式。
 *
 * 声明没有函数体，没有可依赖的被调者指针，所以由调用节点上的标记识别。
 *
 * @param e  调用节点。
 * @return   是则 `true`；没有记录时为 `false`。
 */
bool     planViewOf(const Expr *e);

/**
 * @brief 标记这个调用是 `extc_viewOf` 这个 builtin 形式。
 *
 * @param e  调用节点。
 * @param v  是则为 `true`。
 */
void     planSetViewOf(Expr *e, bool v);

/* ---- impl / trait / module facts ---------------------------------------------------
 *
 * 四项都由代码生成读取。`trait` / `target` 在检查器 intern 类型名时解析；`usedDyn` /
 * `usesDyn` 在见到 `dyn` 形式的那一刻设置，决定是否发出 dyn 运行期。 */

/**
 * @brief 取一个 `impl` 块实现的 trait。
 *
 * @param im  `impl` 块定义。
 * @return   实现的 trait；固有 `impl` 或尚未解析时为 NULL。
 */
TraitDef *planImplTrait(const ImplDef *im);

/**
 * @brief 记下一个 `impl` 块实现的 trait。
 *
 * @param im  `impl` 块定义。
 * @param tr  实现的 trait。
 */
void      planSetImplTrait(ImplDef *im, TraitDef *tr);

/**
 * @brief 取一个 `impl` 块解析到的具体类型。
 *
 * @param im  `impl` 块定义。
 * @return   目标类型；没有记录时为 NULL。
 */
Type     *planImplTarget(const ImplDef *im);

/**
 * @brief 记下一个 `impl` 块解析到的具体类型。
 *
 * @param im  `impl` 块定义。
 * @param t   目标类型。
 */
void      planSetImplTarget(ImplDef *im, Type *t);

/**
 * @brief 判断一个 trait 是否被 `dyn` 形式用到过。
 *
 * 只有用到过，它的虚表才会被生成。
 *
 * @param tr  目标 trait。
 * @return    用到过则 `true`；没有记录时为 `false`。
 */
bool      planUsedDyn(const TraitDef *tr);

/**
 * @brief 记下一个 trait 被 `dyn` 形式用到过。
 *
 * @param tr  目标 trait。
 * @param v   用到过为 `true`。
 */
void      planSetUsedDyn(TraitDef *tr, bool v);

/**
 * @brief 判断一个模块里是否出现过 `dyn` 形式。
 *
 * 出现过才需要生成 `dyn` 运行期。
 *
 * @param m  目标模块。
 * @return   出现过则 `true`；没有记录时为 `false`。
 */
bool      planUsesDyn(const Module *m);

/**
 * @brief 记下一个模块里出现过 `dyn` 形式。
 *
 * @param m  目标模块。
 * @param v  出现过为 `true`。
 */
void      planSetUsesDyn(Module *m, bool v);

/**
 * @brief 取一个实例是由哪个模板生成的。
 *
 * @param f  目标函数。
 * @return   模板函数；`f` 不是实例时为 NULL。
 */
FuncDef *planTemplate(const FuncDef *f);

/**
 * @brief 取一个实例的 C 名字。
 *
 * @param f  目标函数。
 * @return   C 名字；没有记录时为 NULL。
 */
const char *planInstName(const FuncDef *f);

/* ---- arena and zone plan ----------------------------------------------------------- */

/**
 * @brief 取一个分配点该用的 arena 层级。
 *
 * `ARENA_HOME` 哨兵表示用调用者给的 home arena；数值越大寿命越短。
 *
 * @param e  分配点表达式。
 * @return   已记录的层级；没有记录时为 0。
 */
int  planArenaLevel(const Expr *e);
/**
 * @brief 取一次调用往下传的 arena 实参。
 *
 * `-1` 表示调用者自己的 home arena，`>= 1` 表示那个块层级，`0` 表示当前块。
 *
 * @param e  调用节点。
 * @return   已记录的实参；没有记录时为 0。
 */
int  planArenaArg(const Expr *e);
/**
 * @brief 取一个池站点记录下来的 zone 层级。
 *
 * ZONE_HOME 表示用调用者收到的家 zone（库函数对地方透明，把选择权交给调用者），k >= 1 表示
 * 第 k 层块的 zone；站点没记录时返回 0。
 *
 * @param e  要查询的表达式（建池的调用点）。
 * @return   记录下来的 zone 层级；没记录时返回 0。
 */
int  planZoneLevel(const Expr *e);
/**
 * @brief 判断一个函数是否接收调用者的家 arena 作隐藏参数。
 *
 * 以「函数体真的读到了传入的家 arena，或把家 arena 往下交给被调者」为准；该值要等汇总闭包
 * 跑完才最终确定。
 *
 * @param f  要查询的函数。
 * @return   需要隐藏的家 arena 参数则为 true；没记录时返回 false。
 */
bool planUsesHome(const FuncDef *f);

/**
 * @brief 判断一个函数体自己会不会分配存储。
 *
 * 为 false 时函数体不分配任何东西，代码生成就不为它声明 arena 数组、也不发释放调用。
 *
 * @param f  要查询的函数。
 * @return   可能分配则为 true；没记录时返回 false。
 */
bool planMayUseArena(const FuncDef *f);

/**
 * @brief 判断一个函数会不会建池，直接建或经由被调者到达都算。
 *
 * 该值要等调用图闭包跑完才最终确定。
 *
 * @param f  要查询的函数。
 * @return   会建池则为 true；没记录时返回 false。
 */
bool planMakesPool(const FuncDef *f);
/**
 * @brief 判断一个循环的条件里会不会分配存储。
 *
 * 为 true 时代码生成给循环包一层每轮回收的 arena，让条件里的分配在轮末归还。
 *
 * @param s  要查询的循环语句。
 * @return   条件里分配则为 true；没记录时返回 false。
 */
bool planCondAllocs(const Stmt *s);
/**
 * @brief 记录一个声明的初始化式是否指向帧外存储。
 *
 * 帧外存储包括全局、`global` 声明、`effects Ret=0` 的结果与字符串字面量；这条结果在作用域
 * 还活着时写下，供函数体检查完之后再问。
 *
 * @param s  变量声明语句。
 * @param v  初始化式指向帧外存储则为 true。
 */
void planSetVarInitOutOfFrame(Stmt *s, bool v);

/**
 * @brief 读回一个声明的初始化式是否指向帧外存储。
 *
 * @param s  要查询的变量声明语句。
 * @return   初始化式指向帧外存储则为 true；没记录时返回 false。
 */
bool planVarInitOutOfFrame(const Stmt *s);
/**
 * @brief 取脱糖 `for` 的步进语句。
 *
 * 步进语句是语法，写在循环体上，因为 `continue` 必须能走到它；循环被改写成迭代器之后步进
 * 不再存在，这一处返回 NULL。
 *
 * @param s  要查询的循环体语句。
 * @return   步进语句；步进已被丢弃、`s` 为空或没有步进时返回 NULL。
 */
Stmt *planForStep(const Stmt *s);
/**
 * @brief 判断一个表达式是否要先把某部分算进临时变量。
 *
 * 该表达式（或其参数、元素计数）在生成的 C 里会出现不止一次，或需要在循环外只求值一次时
 * 记为 true，代码生成据此先发一条临时变量声明。
 *
 * @param e  要查询的表达式。
 * @return   需要临时变量则为 true；没记录时返回 false。
 */
bool planNeedTemp(const Expr *e);

/* ---- coroutine family -------------------------------------------------------------- */

/**
 * @brief 判断一个函数是不是协程。
 *
 * 协程的值是一个帧句柄，而不是一个普通 C 函数。
 *
 * @param f  要查询的函数。
 * @return   是协程则为 true；没记录时返回 false。
 */
bool   planIsCoro(const FuncDef *f);
/**
 * @brief 取协程 `yield` 交出的值类型。
 *
 * 对 `coroutine<A, B>` 取最后一个类型参数，对单参数的 `coroutine<T>` 取那一个参数。
 *
 * @param f  要查询的函数。
 * @return   yield 值的类型；不是协程或没记录时返回 NULL。
 */
Type  *planYieldType(const FuncDef *f);
/**
 * @brief 取协程帧的 C 结构体类型。
 *
 * 帧结构体的字段布局与代码生成发出来的字段一致，驱动协程的协议方法挂在它上面。
 *
 * @param f  要查询的函数。
 * @return   帧结构体类型；不是协程或还没建立帧时返回 NULL。
 */
Type  *planCoroFrameType(const FuncDef *f);
/**
 * @brief 判断一个协程是否需要属于自己的地方（zone）。
 *
 * 会建池、或帧曾被装箱成句柄的协程总要有一个任务，它的地方由任务持有；不需要时直接驱动
 * 步进函数。
 *
 * @param f  要查询的函数。
 * @return   需要自己的 zone 则为 true；没记录时返回 false。
 */
bool   planCoroNeedsZone(const FuncDef *f);
/**
 * @brief 取一个合成协议方法的协议编号。
 *
 * 协议方法没有函数体，代码生成在调用点内联展开，编号说明它是哪一个方法：帧上的
 * next/value/send 记为 1/2/5，句柄上的记为 3/4/6。
 *
 * @param f  要查询的函数。
 * @return   协议编号；普通函数返回 0。
 */
int planCoroProto(const FuncDef *f);
/**
 * @brief 判断一个协程的帧是否在某个赋值处被转换成过句柄。
 *
 * 装箱后的协程总会得到一个任务，所以它算作需要自己的 zone；这条事实在发生转换的地方写下。
 *
 * @param f  要查询的函数。
 * @return   帧曾被装箱成句柄则为 true；没记录时返回 false。
 */
bool   planCoroBoxed(const FuncDef *f);

/* ---- setters (written by the checker) ----------------------------------------------
 *
 * 每次写都经过 setter，写点因此是唯一能统一加断言或换存储的地方。节点为 NULL 的 setter 是
 * 空操作：有些计划问题是针对不存在的节点问的。 */

/**
 * @brief 记录一个分配站点要用的 arena 层级。
 *
 * @param e  分配站点表达式（`new` 或可能分配的调用）。
 * @param v  arena 层级：ARENA_HOME 表示家 arena，k >= 1 表示第 k 层块的 arena。
 */
void planSetArenaLevel(Expr *e, int v);

/**
 * @brief 记录一个池站点要用的 zone 层级。
 *
 * 只往下调：同一个节点可能被检查不止一次，而层级越小越长寿，所以取更小的那个。
 *
 * @param e  池站点表达式。
 * @param v  zone 层级：ZONE_HOME 表示用调用者收到的家 zone，k >= 1 表示第 k 层块的 zone。
 */
void planSetZoneLevel(Expr *e, int v);

/**
 * @brief 记录一个调用往下传的 arena 参数。
 *
 * @param e  调用点表达式。
 * @param v  arena 参数：ARENA_HOME 表示调用者自己的家 arena，k >= 1 表示第 k 层块的 arena。
 */
void planSetArenaArg(Expr *e, int v);

/**
 * @brief 记录一个表达式需要临时变量。
 *
 * @param e  表达式。
 * @param v  需要临时变量则为 true。
 */
void planSetNeedTemp(Expr *e, bool v);
/**
 * @brief 记录一个函数是否接收调用者的家 arena 作隐藏参数。
 *
 * @param f  函数。
 * @param v  需要隐藏的家 arena 参数则为 true。
 */
void planSetUsesHome(FuncDef *f, bool v);

/**
 * @brief 记录一个函数体自己会不会分配存储。
 *
 * @param f  函数。
 * @param v  可能分配则为 true。
 */
void planSetMayUseArena(FuncDef *f, bool v);

/**
 * @brief 记录一个函数会不会建池，直接建或经由被调者到达都算。
 *
 * @param f  函数。
 * @param v  会建池则为 true。
 */
void planSetMakesPool(FuncDef *f, bool v);
/**
 * @brief 记录一个循环的条件里会不会分配存储。
 *
 * @param st  循环语句。
 * @param v   条件里分配则为 true。
 */
void planSetCondAllocs(Stmt *st, bool v);
/**
 * @brief 记录一个脱糖 `for` 的步进语句已不再存在。
 *
 * 循环被改写成迭代器之后由迭代器的 `next()` 推进，`continue` 不再需要跳到步进标签，该标签
 * 也就不再发出。
 *
 * @param st  承载步进语句的循环体语句。
 */
void planSetForStepDropped(Stmt *st);

/**
 * @brief 记录一个函数是不是协程。
 *
 * @param f  函数。
 * @param v  是协程则为 true。
 */
void planSetIsCoro(FuncDef *f, bool v);

/**
 * @brief 记录一个协程 `yield` 交出的值类型。
 *
 * @param f  协程函数。
 * @param v  yield 值的类型。
 */
void planSetYieldType(FuncDef *f, Type *v);

/**
 * @brief 记录一个协程帧的 C 结构体类型。
 *
 * @param f  协程函数。
 * @param v  帧结构体类型。
 */
void planSetCoroFrameType(FuncDef *f, Type *v);

/**
 * @brief 记录一个协程是否需要属于自己的地方（zone）。
 *
 * @param f  协程函数。
 * @param v  需要自己的 zone 则为 true。
 */
void planSetCoroNeedsZone(FuncDef *f, bool v);

/**
 * @brief 记录一个合成协议方法的协议编号。
 *
 * @param f  合成的协议方法。
 * @param v  协议编号：帧上的 next/value/send 为 1/2/5，句柄上的为 3/4/6。
 */
void planSetCoroProto(FuncDef *f, int v);
/**
 * @brief 记录一个协程有没有被装箱成句柄。
 *
 * 帧值被强制转换成 `coroutine<T>` 句柄的地方写下这一位；记下之后这个协程总会得到任务，即使没有
 * 被显式 spawn，因而算作需要 zone。
 *
 * @param f  协程函数定义；为 NULL 时忽略本次写入。
 * @param v  曾被装箱成句柄则为 true。
 */
void planSetCoroBoxed(FuncDef *f, bool v);
/**
 * @brief 记录一个实例生成时使用的 C 名。
 *
 * 名字由模板名接上各类型实参的改写串拼成，实例化时写一次，代码生成发定义和调用点都用它。只保存
 * 指针，字符串要在后续读取之前一直有效。
 *
 * @param f     实例函数定义；为 NULL 时忽略本次写入。
 * @param name  C 名。
 */
void planSetInstName(FuncDef *f, const char *name);

/* ---- writing the resolved callee (the checker) ------------------------------------- */

/**
 * @brief 记录一个调用点解析出的被调函数，作为默认答案。
 *
 * 泛型体里的调用点被该体的所有实例共享，各实例的答案由 `planSetAltCallee` 按体记录；本函数写的
 * 是站点不属于任何实例时读到的那一份，没有发射上下文时 `planCallee` 返回它。
 *
 * @param e       调用点（或函数值）表达式；为 NULL 时忽略本次写入。
 * @param callee  解析到的函数定义；没有解析结果时可以为 NULL。
 */
void planSetCallee(Expr *e, FuncDef *callee);
/**
 * @brief 按包含它的那具函数体或类型实例，记录一个调用点解析出的被调函数。
 *
 * 泛型体里的一个调用点被该体的所有实例共享，各实例可以解析到不同的函数（`wrap<i32>` 调
 * `pick_i32`，`wrap<meter>` 调 `pick_meter`），所以答案按（调用点，包含体）成对记录，`enclosing`
 * 与 `inst` 至少要给一个。同一对重复写入只覆盖被调函数，因为延迟调用闭包会重放多轮。
 *
 * @param e          调用点表达式；为 NULL 时忽略本次写入。
 * @param enclosing  包含该调用点的函数实例；在方法体里解析时传 NULL。
 * @param inst       包含该调用点的类型实例；在自由函数体里解析时传 NULL。
 * @param callee     该包含体内解析到的函数定义。
 */
void planSetAltCallee(Expr *e, FuncDef *enclosing, const Type *inst, FuncDef *callee);

/* ---- the emission context (code generation) ----------------------------------------
 *
 * 代码生成用这一对函数给"正在发出哪个函数"加括号，`planCallee` 才能回答"在这个实例的函数体里，
 * 这个调用点解析到谁"。离开不是可选项：不离开，下一个函数的发出会继续看到上一个实例的答案。 */

/**
 * @brief 记下代码生成正在发出的函数。
 *
 * 设置之后，`planCallee` 优先返回为该函数体记录的那份调用点解析结果；发完一具函数体要调用
 * `planLeaveFunc` 清掉，否则下一具函数体会读到上一个实例的答案。
 *
 * @param f  正在发出的函数定义。
 */
void planEnterFunc(const FuncDef *f);

/**
 * @brief 清除代码生成正在发出的函数。
 *
 * 与 `planEnterFunc` 成对使用：发完一具函数体后清掉，`planCallee` 才不再按上一个函数实例作答。
 */
void planLeaveFunc(void);
/**
 * @brief 记下代码生成正在发出方法体的类型实例。
 *
 * 泛型结构体的方法体被该结构体的每个类型实例共享，方法体里的调用点因此按类型实例记录，
 * `planCallee` 据此作答。要与 `planLeaveInst` 成对使用。
 *
 * @param t  正在发出其方法体的类型实例。
 */
void planEnterInst(const Type *t);

/**
 * @brief 清除代码生成正在发出的类型实例。
 *
 * 与 `planEnterInst` 成对使用；清掉后 `planCallee` 不再按该类型实例作答。
 */
void planLeaveInst(void);
/**
 * @brief 记录一个实例由哪个模板函数实例化而来。
 *
 * 模板指针存在 plan 侧表、不放在节点字段上：同一个指针决定实例的名字、类型实参与共享哪具函数体。
 * 实例化在给实例做完浅拷贝之后写下它，因为浅拷贝带不上这个指针。
 *
 * @param f     实例函数定义；为 NULL 时忽略本次写入。
 * @param tmpl  实例化所用的模板函数定义。
 */
void planSetTemplate(FuncDef *f, FuncDef *tmpl);

#endif /* EXTC_PLAN_H */
