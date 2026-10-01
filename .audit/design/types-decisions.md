# 类型系统 / 泛型 / trait / dyn / lambda / STL —— 审计后的设计裁定

> 单元：`docs/topics/{GENERICS,TRAITS,DYN,LAMBDA,STL,ARRAYS,AST-WALKERS,COMPILE-TIME,MIGRATION}.md`
> 核对范围：上述 9 份文档 + `src/` 对应实现（逐处给 file:line）+ `.audit/findings/*` 中同线缺陷。
> 生成于 2026-09-30；代码锚点以当日工作树为准（`build/extc` 02:20 构建）。
> 图例 —— 状态：**已定案**（文档+代码一致）· **提案**（文档有、代码无）· **缺口**（双方都承认没做）· **与代码不符**（文档与实现/实测不一致）。
> 每条一行，字段用 ` · ` 分隔：标题 · 出处 · 陈述 · 状态 · 约束实现处。

---

## 1. 泛型：实例化的时机与顺序

- **D1** 实例在**调用点**诞生 · 出处 [GENERICS.md:3-19](../../docs/topics/GENERICS.md#L3-L19)、[check_top.c:4453-4477](../../src/check_top.c#L4453-L4477) · 陈述：类型实例（`varArray<i32>`）由类型决定，自由函数的类型实参只能从调用点读出，所以 `funcInstance` 只在调用处创建实例 · 状态：**已定案** · 实现：`src/check_top.c:4524`。
- **D2** 「同一模板 + 同一组实参 = 同一实例」靠线性 interning 去重 · 出处 [check_top.c:4546-4555](../../src/check_top.c#L4546-L4555)、[GENERICS.md:96-99](../../docs/topics/GENERICS.md#L96-L99) · 陈述：复用已有实例，不分配第二个；一个实参组合恰好一个实例（`eeq2_i32` 类名唯一） · 状态：**已定案** · 注意：查重是 O(实例数) 线性扫描。
- **D3** 实例是模板的**浅拷贝**（共享 body），只有参数/返回类型被替换 · 出处 [check_top.c:4556-4581](../../src/check_top.c#L4556-L4581) · 陈述：`*in = *tmpl`；每个 `Param` 必须先拷贝再替换（曾因原地替换污染模板导致段错误）；`typeParams` 清空 · 状态：**已定案** · 实现同上。
- **D4** 实例自动进入本模块函数表供 codegen 发射 · 出处 [check_top.c:4597-4598](../../src/check_top.c#L4597-L4598) · 陈述：`vecPush(&c->m->funcs, in)`，codegen 像普通函数一样发它 · 状态：**已定案**。
- **D5** 模板体内调用泛型：先造「临时实例」（签名仍带 `T`）让外层模板体能查完，同时记录 `CallCheck` · 出处 [check_expr.c:3184-3215](../../src/check_expr.c#L3184-L3215)、[GENERICS.md:53-57](../../docs/topics/GENERICS.md#L53-L57) · 陈述：`idOf<T>` 这种临时实例自洽且把 `T` 当不透明；实例化时 `e->func` 被重定向到具体实例（`idOf_i32`） · 状态：**已定案**。
- **D6** 发射时跳过「签名里还带类型参数」的实例（#63 的类级修法） · 出处 [codegen.c:7022](../../src/codegen.c#L7022)、[codegen.c:5571-5578](../../src/codegen.c#L5571-L5578)、[GENERICS.md:53-57](../../docs/topics/GENERICS.md#L53-L57) · 陈述：判据 `funcSignatureMentionsParam`，临时实例只做检查、永不出现在生成物 · 状态：**已定案**。
- **D7** 延迟检查分四批、按实例重放：OpCheck（运算符）· DeferredUse（`#79` 结果用途）· MethodCheck（`#57` 对 `T` 调方法）· RefCheck（引用规则 + 零值 + `new T[n]` 尺寸） · 出处 [check_top.c:5019-5050](../../src/check_top.c#L5019-L5050)、[check_top.c:5617-5679](../../src/check_top.c#L5617-L5679)、[check_top.c:5770-5795](../../src/check_top.c#L5770-L5795) · 陈述：模板上 `T` 不透明，凡结论依赖 `T` 的规则都推迟到实例 · 状态：**已定案** · 实现：`runOpCheck:4665` / `runDeferredUse:4733` / `runMethodCheck:4782` / `runRefCheck:4844`。
- **D8** 带参数的实例（provisional）在重放时跳过 · 出处 [check_top.c:4977-4981](../../src/check_top.c#L4977-L4981) · 陈述：实参仍提及类型参数 ⇒ 不是真实例，`runXxx` 一律 `continue`，留给外层实例 · 状态：**已定案**。
- **D9** `resolveDeferredCall` 遇到「替换后仍带 `T`」就等待，不报错 · 出处 [check_top.c:4998-5012](../../src/check_top.c#L4998-L5012) · 陈述：`ttHasParam(sub)` ⇒ `return`（"这个实例还没准备好"）；实参槽为 NULL 也直接返回（推断失败已在模板上报过） · 状态：**已定案**。
- **D10** 延迟调用重放是**单遍**（无不动点迭代）⇒ 三链泛型 `f←g←m` 顺序相关 · 出处 [check_top.c:5596-5614](../../src/check_top.c#L5596-L5614) · 陈述：`c.callChecks` 只被遍历一次，先访问的 innermost 记录不会因为后建的 enclosing 实例被重访；声明顺序在内层在前时生成物引用从未发射的 `f_T` · 状态：**与代码不符（P0，见 §14-A）** · 证据：`.audit/findings/checktop-3.json`（`check_top.c:5596`），Lead 已复现；代码内层 `for (j < c.funcInsts.len)` 会随 `funcInstance` 增长，但这只救「同一条记录内新造的实例」，救不了已处理过的记录。
- **D11** 类型实例表 `tt->instances` 的字段物化是**边走边增长**的（事实上的不动点） · 出处 [check_top.c:5745-5752](../../src/check_top.c#L5745-L5752) · 陈述：循环每次重取 `instances.len`；替换字段类型时 `ttSubstitute` 会继续 intern 嵌套实例，故 `pool<V>`→`pool_i32` 能及时出现（#80） · 状态：**已定案** · 对比：D10 的调用记录没有这一性质。
- **D12** 实例相关的「太晚 intern」补丁有三处：`internLocalTypes`（局部声明类型）· 协程实例的 `coroSetup`/`coroFrameLay` 重跑 · 实例字段物化 · 出处 [check_top.c:5681-5708](../../src/check_top.c#L5681-L5708)、[check_top.c:4929-4975](../../src/check_top.c#L4929-L4975) · 陈述：codegen 在检查后建单元表，实例必须在检查收尾前全部进入 `tt->instances` · 状态：**已定案**（历史 attack W9/X9 的修法）。
- **D13** 每个实例的 effects 摘要必须清空后按实例重算 · 出处 [check_top.c:5797-5850](../../src/check_top.c#L5797-L5850) · 陈述：`*in = *tmpl` 是浅拷贝 ⇒ 先清 `addrMask/contMask/...`、`effState`、`callees` 再 `collectEffects`，否则叶计数翻倍、闭包读到模板的陈旧记账 · 状态：**已定案**。
- **D14** 实例化有硬上限：自由函数实例 ≤ **4096**、类型嵌套深度 ≤ **64**，超限报「compiler limit，请上报」并返回 NULL · 出处 [check_top.c:4480-4494](../../src/check_top.c#L4480-L4494)、[check_top.c:4526-4545](../../src/check_top.c#L4526-L4545) · 陈述：防「模板在自己的体内用更大的类型实例化自己」的失控（attack E3） · 状态：**已定案**。
- **D15** 上限触发时 `funcInstance` 返回 NULL，但两个调用点**不检查**直接 `inst->used = true` ⇒ 编译器 SIGSEGV · 出处 [check_expr.c:2404-2405](../../src/check_expr.c#L2404-L2405)、[check_expr.c:3179-3180](../../src/check_expr.c#L3179-L3180)、[check_top.c:5013-5016](../../src/check_top.c#L5013-L5016)（唯一检查了 NULL 的调用点） · 陈述：65 层 `box<...>` 或 4300 个实例即可打崩编译器 · 状态：**与代码不符（P0，见 §14-A）** · 证据：`.audit/notes.md` F-09（已复现）；`resolveDeferredCall` 是三条路径里唯一正确的。
- **D16** codegen 的实例表快照晚于检查：实例名与用户声明的冲突检查放在 `generateC` 开头 · 出处 [codegen.c:6683-6718](../../src/codegen.c#L6683-L6718) · 陈述：最后一次实例化波在检查之后，放检查器里「从来不会触发」 · 状态：**已定案**。
- **D17** 泛型函数的裸名不是值（无 `fn` 指针形态） · 出处 [check_expr.c:1043-1050](../../src/check_expr.c#L1043-L1050) · 陈述：一个实例一份 body ⇒ 一个编译期代码指针表达不了；`@export`/方法/协程同理各有专门拒绝 · 状态：**已定案**。
- **D18** GENERICS.md 把「T 出现位置 × 特性」做成矩阵，每格一个用例 · 出处 [GENERICS.md:26-38](../../docs/topics/GENERICS.md#L26-L38) · 陈述：判据落在 `tests/genmatrix/`（13 格），仍坏的形状放 `tests/canary-gaps/` 且断言必须继续坏 · 状态：**已定案（流程）** · 实现：`tests/genmatrix/run.sh`。
- **D19** 泛型判据的铁律：必须编译并运行生成的 C（`extc --run` 或 `-o` 后 gcc），`-o` 返回 0 ≠ 生成物编得过 · 出处 [GENERICS.md:20-24](../../docs/topics/GENERICS.md#L20-L24) · 陈述：`#61/#62` 在 `extc f.extc -o out.c` 下返回 0 · 状态：**已定案（流程）**。
- **D20** 加类型构造时必须扫整族按 kind 分派的函数（`ttEquals`/`ttRender` 曾漏 `TY_ENUM` ⇒ `option<T>` 两边同名却判不等） · 出处 [GENERICS.md:40-45](../../docs/topics/GENERICS.md#L40-L45) · 陈述：现行修法在 `ttEquals` 给 `TY_ENUM` 加「比较 edef + targs」分支 · 状态：**已定案** · 实现：`src/types.c:950-961`。
- **D21** 类型参数来自类型的视图 helper 必须「按需登记 + 在 helpers 区统一发射」 · 出处 [GENERICS.md:47-51](../../docs/topics/GENERICS.md#L47-L51) · 陈述：在调用点直接发射会把定义落进原型区（`invalid storage class for function`）；`tt->instances` 列表漏掉「元素类型来自类型参数」的视图实例（#62） · 状态：**已定案**。
- **D22** 检查器说行 ≠ 定义被发射：实例方法被找到时必须 `f->used = true` · 出处 [GENERICS.md:63-68](../../docs/topics/GENERICS.md#L63-L68) · 陈述：#64 的根因是 `runOpCheck` 检查通过却没标 used，codegen 只发 used 的方法 · 状态：**已定案** · 实现：`src/check_top.c:4721-4723`。
- **D23** 已知未覆盖格（诚实的洞） · 出处 [GENERICS.md:70-76](../../docs/topics/GENERICS.md#L70-L76) · 陈述：跨模块泛型（`use` 后实例化）·`result<T,E>` 构造与 match·泛型 × 引用参数·泛型 × 泛型枚举（`?option<T>`）·泛型 × 逃逸/arena 提升·泛型 × `@overwrite`·泛型 × extern/库边界 —— 矩阵里一格都没有 · 状态：**缺口**。
- **D24** 纪律：每加一个特性/修一个洞，回来给矩阵加一列/一格 · 出处 [GENERICS.md:78](../../docs/topics/GENERICS.md#L78) · 状态：**已定案（流程）**。

---

## 2. 实例命名、mangle 与 C 名保留

- **D25** 泛型函数实例的 C 名 = 模板名 + 每个实参 `_` + `ttMangle(实参)`（`max_i32`） · 出处 [check_top.c:4587-4595](../../src/check_top.c#L4587-L4595) · 陈述：名字由**创建实例的检查器**生成、写在 `instName` 上，codegen 直接采用 · 状态：**已定案** · 实现：`cFuncName` 优先返回 `f->instName`（[codegen.c:642-656](../../src/codegen.c#L642-L656)）。
- **D26** 类型 mangle 规则（`ttMangle`） · 出处 [types.c:478-525](../../src/types.c#L478-L525) · 陈述：`ref T`→`Ref_T`；`[N]T`→`array_N_T`（递归）；泛型实例→`<sdef.name>_<args…>`（递归）；`fn`→`extc_fn_<ret>__<param…>`；类型参数用参数名；内建/结构体/枚举用自身名 · 状态：**已定案** · 注意：`fn` 用 `__` 分隔，文档明说「两侧名字里含 `__` 才可能撞，codegen 有守卫拒绝」。
- **D27** 数组类型名递归、不拍平 · 出处 [ARRAYS.md:131-140](../../docs/topics/ARRAYS.md#L131-L140)、[types.c:483-485](../../src/types.c#L483-L485) · 陈述：`[15][15]i32 → array_15_array_15_i32`，与泛型实例同一套规矩（真实结构就是 15 个 `[15]i32`） · 状态：**已定案**。
- **D28** 方法 C 名 = 拥有者前缀 + `_` + `cSymName(方法名)`（+ 重载后缀）；实例接收者用实例名作前缀 · 出处 [codegen.c:642-685](../../src/codegen.c#L642-L685) · 陈述：`cMethodName` 不能用 `cFuncName`（callee 可能属于别的类型：在 `Wrapper<Point>` 里调 `Point.==`） · 状态：**已定案**。
- **D29** extC 运算符→C 标识符的固定映射：`==→eq`、`!=→ne`、`<→lt`、`<=→le`、`>→gt`、`>=→ge`、`+→add`、`-→sub`、`*→mul`、`/→div`、`%→rem`、`<<→shl`、`>>→shr`、`[]→idx`、`[]=→idxset` · 出处 [codegen.c:550-592](../../src/codegen.c#L550-L592) · 陈述：集合必须覆盖语言允许做方法名的每个运算符，漏一个会把 `ver_<` 之类直接贴进 C · 状态：**已定案**。
- **D30** 用户名字与 C 关键字冲突时加 `__c` 后缀（`fn double` → `double__c`），extC 源码一字不改 · 出处 [codegen.c:574-592](../../src/codegen.c#L574-L592) · 陈述：关键字表 `cIdentIsKeyword`（base.c）与检查器共用 · 状态：**已定案**。
- **D31** 运算符重载的 C 名后缀 `_<mangled 右操作数类型>`；`mut` 视图额外加 `mut_` 前缀 · 出处 [codegen.c:615-639](../../src/codegen.c#L615-L639) · 陈述：`ttEquals` 区分 mut 而 C 结构相同，不加前缀则两个 `<<` 重载在 C 里重定义；定义侧与调用点必须用**同一函数**算后缀 · 状态：**已定案** · 但后缀取值依赖**当前替换上下文** ⇒ 泛型方法定义与调用点会得到不同名字（P0，见 §14-A）。
- **D32** `__` 开头的标识符**禁止用户使用**（编译器保留） · 出处 [lexer.c:258-273](../../src/lexer.c#L258-L273) · 陈述：codegen 发明的所有名字（实例、方法 holder、视图 helper、表、运行期）都用该前缀，程序无法撞上；仍 push token 以给出清晰报错而非级联 · 状态：**已定案**。
- **D33** prelude 定义标记 `reserved`，用户重定义报「`X` is a reserved definition and cannot be redefined」 · 出处 [main.c:268-282](../../src/main.c#L268-L282)、[check_top.c:96-124](../../src/check_top.c#L96-L124) · 陈述：struct/type/func 进入 prelude 时打标；`builtinHolder`（如 `impl i64 {…}`）除外，允许程序继续给内建加方法 · 状态：**已定案**。
- **D34** `$` 不是合法 extC 标识符 ⇒ 名字里的 `$` 只可能来自 loader 的模块前缀或编译器合成名 · 出处 [codegen.c:594-600](../../src/codegen.c#L594-L600)（`exportName` 注释） · 陈述：`@export` 的 C 符号取 `$` 之后的源码名，不走 `cSymName`（需要改名的 `fn double` 在导出点直接拒绝，而非静默变 `double__c`） · 状态：**已定案** · 注意：合成名（`$lam$f$3`、`extc_vt$…`、`pair$make`）会**原样出现在 C 里**，依赖 gcc/clang 允许 `$` 的扩展；生成物合同是 `-std=c11`（未加 `-pedantic-errors`），所以这条是可移植性隐患而非当下红灯。
- **D35** 编译器生成的实例名与用户声明的冲突由 codegen 报错，但只覆盖 `TY_GENERIC` · 出处 [codegen.c:6683-6718](../../src/codegen.c#L6683-L6718) · 陈述：`struct pair_i64` 撞 `pair<i64>` 会报「give this declaration another name」；循环开头跳过非 `TY_GENERIC` ⇒ `array_N_T` 与泛型枚举实例名 `option_i32` **不在守卫内** · 状态：**与代码不符（P1，见 §14-A）** · 证据：`.audit/findings/types.json`（`types.c:483`，已复现：一个 C tag 两份 struct 定义、extc 仍返回 0）。
- **D36** dyn 表/键命名：实例键 `extc_vt$<Trait>$<Type>`（稳定键）、trait 一个统一签名 `struct extc_vt$<Trait>_t`、每 (trait,类型,方法) 一个 thunk `extc_th$<Trait>$<Type>$<method>` · 出处 [DYN.md:438-468](../../docs/topics/DYN.md#L438-L468)、[DYN.md:646-656](../../docs/topics/DYN.md#L646-L656) · 陈述：键沿用第一期、槽位顺序 = trait 声明顺序（将是 ABI） · 状态：**已定案** · 实现：`src/codegen.c:8108-8199`。
- **D37** lambda 的合成类型名 `$lam$<owner>$<序号>`，捕获字段名 `c0/c1…`（下标命名，因为两个捕获可能同名） · 出处 [check_expr.c:823-826](../../src/check_expr.c#L823-L826)、[check_expr.c:962](../../src/check_expr.c#L962)、[LAMBDA.md:132-134](../../docs/topics/LAMBDA.md#L132-L134) · 状态：**已定案**。
- **D38** 「用户名字保留」的总策略 = `__` 前缀禁令 + prelude `reserved` + 实例名冲突检查；没有别的保留名单 · 出处 [lexer.c:258-273](../../src/lexer.c#L258-L273)、[main.c:268-282](../../src/main.c#L268-L282)、[codegen.c:6683-6718](../../src/codegen.c#L6683-L6718) · 陈述：`extc_*`/`EXTC_*` 运行期名**没有**被拒（靠实例名检查与链接期兜底） · 状态：**缺口**（`array_`/泛型枚举名未保留是同一问题的实例，见 D35）。
- **D39** 死代码：`#define FUNC_INST_PREFIX "__extc_fi_"` 定义后从未使用 · 出处 [check_top.c:4478](../../src/check_top.c#L4478) · 状态：**与代码不符（卫生）**。

---

## 3. 类型参数推断（unify）与失败诊断

- **D40** `unifyTParams` 的三条规则 · 出处 [check_top.c:4621-4644](../../src/check_top.c#L4621-L4644) · 陈述：① `want` 是类型参数 ⇒ 槽空则填、已填则须 `ttEquals`；② `ref↔ref` 递归；③ `generic↔generic` 须同 sdef 同 arity 且逐参递归 · 状态：**已定案**。
- **D41** **有意/现实不支持**的 unify 形状：`T` 只出现在返回类型（或没有任何实参提到它）⇒ 必须显式写 `f<i32>(...)` · 出处 [check_top.c:4617-4619](../../src/check_top.c#L4617-L4619)、[check_expr.c:3150-3163](../../src/check_expr.c#L3150-L3163) · 陈述：诊断明说「write them out at the call to supply one that no argument mentions」 · 状态：**已定案**。
- **D42** **缺口**：泛型**枚举实例**（`option<T>`/`result<T,E>`）无法 unify · 出处 [check_top.c:4635-4644](../../src/check_top.c#L4635-L4644) · 陈述：`want`/`got` 都是 `TY_ENUM`（`ttEnumGeneric` 产物），既不是 `TY_GENERIC` 也不命中 `ref` 分支 ⇒ 落到 `if (ttHasParam(want)) return false;`，报「cannot infer」；而 `option<i64>` 的实参就在 `t->targs` 里 · 状态：**与代码不符（P1，见 §14-A）** · 证据：`.audit/findings/checkexpr-2.json`；本文实测 `fn f3<T>(o: option<T>) -> T` 调用被拒。**这是审计发现的「设计缺口被当成语言禁例」的典型**：文档（D23）只把 `result<T,E>` 列为未覆盖格，没有说「推断自身不支持」。
- **D43** 显式实参 `f<i64>(x)` 必须能真正喂给推断，不能只是装饰 · 出处 [check_expr.c:2417-2426](../../src/check_expr.c#L2417-L2426) · 陈述：`EX_GENCALL` 重写成 `EX_CALL` 时把实参经 `ExplicitTargs` side table 传给主路径；没有它时「碰巧推断得出才工作」 · 状态：**已定案**（attack B10/B3 的修法）。
- **D44** 推断失败诊断有两处、都点名：unify 返回 false 报「cannot infer type parameter(s) of `f`」；槽位为空报「cannot infer type parameter `T` of `f`」 · 出处 [check_expr.c:3150-3163](../../src/check_expr.c#L3150-L3163) 与 [check_expr.c:3166-3176](../../src/check_expr.c#L3166-L3176) · 状态：**已定案**（文本略有重复，可合并）。
- **D45** unify 的形状匹配是**结构性、只认精确形状**：`want` 提 `T` 但形状对不上（如 `slice<T>` vs `[3]i32`）即判不可推断 · 出处 [check_top.c:4642-4643](../../src/check_top.c#L4642-L4643) · 陈述：不找「最一般解」，不做上下文定型，不做跨参数一致性以外的推理 · 状态：**已定案**（有意从简）。
- **D46** 名字空间规则：类型小写 camelCase、类型参数首字母大写、trait 第三种名字也要求大写 · 出处 [parser.c:1345-1356](../../src/parser.c#L1345-L1356)、[parser.c:1048-1056](../../src/parser.c#L1048-L1056)、[TRAITS.md:11](../../docs/topics/TRAITS.md#L11) · 陈述：三类名字靠首字母互不相撞，`Self` 即借此成为「隐式类型参数」 · 状态：**已定案** · 注意：trait 的**显式**类型参数不要求大写（与 struct 检查不一致，P3 卫生项）。

---

## 4. trait / impl：一致性比什么、目标类型怎么办

- **D47** 拼写 `impl Codec for circle { … }`，与固有 `impl circle { … }` 只差一个 `for` 子句 · 出处 [TRAITS.md:10](../../docs/topics/TRAITS.md#L10) · 状态：**已定案** · 实现：`parseImpl`（[parser.c:1112+](../../src/parser.c#L1112)）。
- **D48** `Self` = trait 的**隐式类型参数**（每个 trait 方法自带 `typeParams` 且第一项是 `"Self"`），复用现有泛型实例化与实例期检查，不引入新关键字 · 出处 [TRAITS.md:11](../../docs/topics/TRAITS.md#L11)、[parser.c:1093-1102](../../src/parser.c#L1093-L1102) · 状态：**已定案**。
- **D49** trait 可声明**无 self 的关联函数**，但它们**永不动态分发**（object safety 第一条） · 出处 [TRAITS.md:12](../../docs/topics/TRAITS.md#L12)、[DYN.md:31](../../docs/topics/DYN.md#L31) · 状态：**已定案**。
- **D50** 第一期禁止跨 trait 撞名（两个 trait 在同一类型上声明同名方法 ⇒ 报错并指名两者） · 出处 [TRAITS.md:13](../../docs/topics/TRAITS.md#L13) · 陈述：靠「一个类型一份方法集」的重复检查兜；**当前消息不指名来源**（`` struct `box` has duplicate method `tag` ``），专门诊断仍欠 · 状态：**已定案（目标）/缺口（诊断）** · 出处见 [TRAITS.md:129-139](../../docs/topics/TRAITS.md#L129-L139)，实现为准处 `checkDeclarations`（[check_top.c:178](../../src/check_top.c#L178) 报 `` struct `box` has duplicate method `tag` ``）。
- **D51** trait 方法可见性 = 声明 trait 的模块被引入 · 出处 [TRAITS.md:14](../../docs/topics/TRAITS.md#L14) · 状态：**已定案**（沿用 `impl` 规则）。
- **D52** 孤儿规则：`impl` 必须与 trait 同模块或与类型同模块 · 出处 [TRAITS.md:15](../../docs/topics/TRAITS.md#L15)、[check_top.c:5458-5471](../../src/check_top.c#L5458-L5471) · 陈述：程序级方法集下，第三方模块的竞争实现会以「远处的重复方法」形式出现，所以在挂载点拒绝 · 状态：**已定案**。
- **D53** 一个 `(trait, 类型)` 只允许一份实现 · 出处 [TRAITS.md:16](../../docs/topics/TRAITS.md#L16)、[check_top.c:5315-5328](../../src/check_top.c#L5315-L5328) · 陈述：按 `traitName`+`typeName` 字符串在本模块 `m->impls` 内查重，报「first at line N」 · 状态：**已定案** · 注意：查重是字符串比较，`impl Codec<i64> for box` 与 `impl Codec<u8> for box` 视为同一个 `(trait,类型)` ⇒ **参数化 trait 的多个实例不能共存**（这是 D53 的副作用，文档未记）。
- **D54** 一致性比对**实际比什么** · 出处 [check_top.c:249-295](../../src/check_top.c#L249-L295) · 陈述：① 方法齐全（名字集合，挂载趟 [check_top.c:5332-5342](../../src/check_top.c#L5332-L5342)）；② `funcIsMethod(want)==funcIsMethod(have)`（有无接收者）；③ `params.len` 相等；④ 参数类型逐个 `ttEquals`（`Self` 用 `(trait,类型)` + trait 实参代入）；⑤ 返回类型 · 状态：**已定案（作为实现事实）**，但 ⑤⑥⑦ 三处有洞。
- **D55** 不比对：**接收者的 mut 位** · 出处 [check_top.c:267-284](../../src/check_top.c#L267-L284) · 陈述：参数循环从 `j=1` 起，`self` 被整段跳过 ⇒ trait 写 `mut ref Self`、impl 写 `ref box`（或反之）都能通过，只要 `funcIsMethod` 同为真 · 状态：**缺口（本文核出，未见于 findings）** · 本文实测：`trait Bump { fn add(self: mut ref Self, d: i64) -> i64 }` + `impl Bump for box { fn add(self: ref box, d: i64) -> i64 }` 零诊断通过 · 影响：`dyn` 的 thunk 按 trait 签名生成（receiver 擦成 `void *`，mut 差异被吞），静态调用则按 impl 自己的 mut 决定可写性 ⇒ 两个调用路径的写权限语义可以不一致。
- **D56** 不比对：**effects 子句** · 出处 [parser.c:1088-1096](../../src/parser.c#L1088-L1096)、[check_top.c:249-295](../../src/check_top.c#L249-L295) · 陈述：trait 方法走 `noBody` 路径（同 `extern!`），可以写 `effects Ret=0`；一致性检查只比名字与解析后的参数/返回类型，**没有任何一处**把该子句与实现比 · 状态：**与代码不符（P1 unsound，见 §14-A）** · 证据：`.audit/findings/parser-1.json`（已复现：`dyn` 调用点相信 trait 的 `Ret=0`，实现却返回指向死帧的视图 ⇒ ASan stack-use-after-scope）。
- **D57** 不比对：**无接收者方法的第一个参数** · 出处 [check_top.c:267](../../src/check_top.c#L267) · 陈述：`j=1` 无条件跳过接收者，而 trait 方法可以没有 `self` ⇒ 第 0 个真参数永远不比较（`trait Conv { fn conv(x: i64) }` 可被 `fn conv(x: bool)` 实现） · 状态：**与代码不符（P3，见 §14-A）** · 证据：`.audit/findings/checktop-1.json`。
- **D58** 返回类型只在**两侧都非 void** 时比较 · 出处 [check_top.c:285-294](../../src/check_top.c#L285-L294) · 陈述：`void` 的表示是 `ret == NULL`，守卫写成 `if (wantR && haveR && …)` ⇒ trait 返值、impl 返 void（或反之）全部放行；dyn 表按 trait 签名建 ⇒ thunk 从 void 调用里取值，生成物编不过 · 状态：**与代码不符（P1，见 §14-A）** · 证据：`.audit/findings/checktop-1.json`（已复现）。
- **D59** 目标类型的三条挂载路径：结构体/泛型声明走 `sdef`；**内建标量**走合成 holder（`Type.mholder`，进 `m->structs` 但不进类型表、不发 C struct）；**实例化类型**（`impl slice<u8>`）走按实例的合成 holder，名字用实例的 mangled 名（避免与 prelude 的 `slice` 声明撞 reserved 检查） · 出处 [check_top.c:5365-5443](../../src/check_top.c#L5365-L5443)、[TRAITS.md:152-161](../../docs/topics/TRAITS.md#L152-L161) · 状态：**已定案**。
- **D60** 但**一致性检查整体跳过** builtin 与视图/实例化目标 · 出处 [check_top.c:228-230](../../src/check_top.c#L228-L230)、[check_lookup.c:418-422](../../src/check_lookup.c#L418-L422) · 陈述：`structOf` 只认 `TY_STRUCT`/`TY_GENERIC`：`impl Trait for i64` 直接 `continue`；`impl Trait for slice<u8>` 拿到的是 `sdef`（泛型 slice 声明体）而不是挂着方法的 `mholder` ⇒ 方法名在 `sd->methods` 里找不到 ⇒ `have == NULL` ⇒ `continue`（「缺席已在挂载趟报过」）—— 但挂载趟查的是 `im->methods`，两边对不上，于是**签名比对静默不发生** · 状态：**与代码不符（P1 unsound，见 §14-A）** · 证据：`.audit/findings/checktop-1.json`；本文实测 `trait Codec { fn enc(...) -> i64 }` + `impl Codec for slice<u8> { fn enc(...) { } }` 零诊断通过。
- **D61** 方法集唯一权威 `methodSetOf(t) = t->mholder ? mholder : sdef` · 出处 [TRAITS.md:160-161](../../docs/topics/TRAITS.md#L160-L161)、[codegen.c:6667-6670](../../src/codegen.c#L6667-L6670) · 陈述：这一族 bug（表名不一致、表内容 `{NULL}`、方法被可达性剪枝）全部源于三个地方各推一遍「方法集在哪」；查找侧同规则（`findMethod` 先 `mholder`，[check_lookup.c:429-441](../../src/check_lookup.c#L429-L441)） · 状态：**已定案** · 反例：D60 的 `structOf` 不是这套权威的一部分。
- **D62** 参数化 trait：声明侧把显式参数接在 `"Self"` 之后（零新字段）；impl 侧 arity 必须给全，否则报「the trait has N type parameter(s), M given」；签名比对用 `(Self, T…)` 平行代入 · 出处 [parser.c:1093-1102](../../src/parser.c#L1093-L1102)、[check_top.c:5288-5307](../../src/check_top.c#L5288-L5307)、[check_top.c:236-248](../../src/check_top.c#L236-L248) · 状态：**已定案** · 文档 [TRAITS.md:191-202](../../docs/topics/TRAITS.md#L191-L202) 记的「声明已通、impl 侧段错误、改动全部回退」**已过时**：本文实测 `trait Codec<T>` + `impl Codec<i64> for box` 编译并运行成功（`--run` 退出码 3 = 1+2）。
- **D63** object safety 三条（泛型方法 / 返回 `Self` / 无 `self` 的关联函数）在 **`dyn` 使用点**报错，而不是禁止声明这样的 trait；静态调用仍合法 · 出处 [DYN.md:29-36](../../docs/topics/DYN.md#L29-L36)、[DYN.md:86-100](../../docs/topics/DYN.md#L86-L100) · 状态：**已定案** · 实现：检查器 `EX_METHOD` 的 dyn 分流处 + codegen 的 `unsafe` 槽。
- **D64** 第一期「明确不做」清单：`T: Trait` 上界 · `dyn Trait` 值 · 关联类型/常量 · trait 默认方法 · 动态分发 · 运行期注册（后两项由第二期/第三期接走） · 出处 [TRAITS.md:25-26](../../docs/topics/TRAITS.md#L25-L26) · 状态：**已定案（范围）**。
- **D65** 函数重载是**提案**：形状键从右操作数推广到全部参数、只认精确匹配、性质单调（随时可加、无迁移成本）；第一期不需要 · 出处 [TRAITS.md:143-146](../../docs/topics/TRAITS.md#L143-L146) · 状态：**提案**（代码无重载，仅运算符按右操作数重载）。
- **D66** 静态方法表：不用 `dyn` 的程序生成物里没有函数指针调用（判据 `vtable_no_call`） · 出处 [TRAITS.md:23-24](../../docs/topics/TRAITS.md#L23-L24)、[TRAITS.md:123-127](../../docs/topics/TRAITS.md#L123-L127) · 状态：**已定案**。
- **D67** 但代码只在 `tr->usedDyn` 的 trait 上发统一表（静态使用的 trait 一张都不发） · 出处 [codegen.c:8095-8107](../../src/codegen.c#L8095-L8107) · 陈述：TRAITS.md 仍写「为每个 `(trait, 类型)` 发一张 C 静态方法表」，未记这道门 · 状态：**与代码不符（文档滞后）** · 影响：`tests/impl` 的表判据必须用带 `dyn` 的夹具，读 TRAITS.md 的人会以为无 dyn 也有表。
- **D68** 槽位顺序 = trait 声明顺序、**不许**哈希/字母序/插入序；重排 trait 声明会静默改 ABI · 出处 [TRAITS.md:66](../../docs/topics/TRAITS.md#L66)、[DYN.md:677-678](../../docs/topics/DYN.md#L677-L678) · 状态：**已定案（ABI 级）**。
- **D69** trait 表发射点：声明随原型早发、定义在**全部按偏移重写的趟之后**追加 · 出处 [TRAITS.md:51-70](../../docs/topics/TRAITS.md#L51-L70)、[TRAITS.md:103-116](../../docs/topics/TRAITS.md#L103-L116)、[DYN.md:73-75](../../docs/topics/DYN.md#L73-L75) · 陈述：早发会让 `main` 里的派发报 undeclared；晚发会被 `dropUnreferenced` 的错误 span 削掉头部注释 · 状态：**已定案（踩过两次）**。
- **D70** 表引用的方法必须是可达性播种的**根**（`inTraitTable`），否则方法不被发射；且不能登记进 `deadFuncs`（那会让「没人调用就删」的趟删掉 thunk 的目标） · 出处 [TRAITS.md:89-108](../../docs/topics/TRAITS.md#L89-L108)、[TRAITS.md:178-187](../../docs/topics/TRAITS.md#L178-L187)、[codegen.c:6673-6683](../../src/codegen.c#L6673-L6683)、[codegen.c:6997](../../src/codegen.c#L6997)、[codegen.c:7027](../../src/codegen.c#L7027) · 状态：**已定案** · 残留：`markUncalledFunctions` 仍缺 dyn 豁免 ⇒ 被 dyn 表调用的方法被标 `EXTC_UNUSED`（P3 卫生，`.audit/findings/codegen-4.json`）。
- **D71** 统一签名：每 trait 一个 struct，字段类型 = trait 声明的 C 类型、接收者擦成 `void *`；不可派发的方法保留槽位但类型为 `void *`、值为 `NULL` · 出处 [DYN.md:438-468](../../docs/topics/DYN.md#L438-L468)、[codegen.c:8108-8118](../../src/codegen.c#L8108-L8118)、[codegen.c:8186-8198](../../src/codegen.c#L8186-L8198) · 状态：**已定案**。
- **D72** `__typeof__` 而非 `typeof`（生成物合同 `-std=c11` 下 GNU 扩展被关，实测编不过）；后来被「每 trait 统一签名」取代，只在历史记录里 · 出处 [TRAITS.md:118-121](../../docs/topics/TRAITS.md#L118-L121) · 状态：**已定案（历史）**。
- **D73** 撞名的专门诊断（指名两个 trait / 与固有方法区分）是**欠账**，锚点已写 · 出处 [TRAITS.md:129-139](../../docs/topics/TRAITS.md#L129-L139) · 状态：**缺口** · 翻面判据：`tests/impl/trait_collide_inherent.extc`、`trait_collide_cross.extc`。
- **D74** 「实现体不读 `self`」在 `-Wextra -Werror` 下失败（`markUnusedParams` 只遍历 `g.deadFuncs`，impl 附加的方法没登记）；修法被全量 413 文件 410 个变样挡住，必须先修文本 pass 的偏移失效 · 出处 [TRAITS.md:204-237](../../docs/topics/TRAITS.md#L204-L237)、[TRAITS.md:239-309](../../docs/topics/TRAITS.md#L239-L309) · 状态：**缺口（未修）** · 关联：`hashSet.extc` 挂死（D146）同属这片文本 pass 区域。
- **D75** TEXT pass 偏移失效 bug 的根因链：`dropRuntimeDefs` 改写文本后，`deadFuncs` 里「生成时捕获的 body/长度」失效；判定（countMentions 计数）先错、长度只是放大器 · 出处 [TRAITS.md:263-309](../../docs/topics/TRAITS.md#L263-L309) · 状态：**缺口（文档自述「时间段用尽，留给下一轮」）** · **审计后续**：同一区域已升级为 P0 挂死（见 D146）。

---

## 5. dyn：表、thunk、参数检查与引用传递

- **D76** `dyn Tag` 是**值类型**（载荷一份拷贝），不是引用；构造写作 `dyn Tag(x)`；`ref dyn Tag` 第二期不提供 · 出处 [DYN.md:13-27](../../docs/topics/DYN.md#L13-L27) · 状态：**已定案** · 注意：代码里 `ref dyn Tag` **可以写**（接收者经引用时 `(*handle)` 解引用，[codegen.c:1957-1965](../../src/codegen.c#L1957-L1965)），与文档「不提供」不一致 —— 文档滞后于实现。
- **D77** 表怎么建：每 trait 一个统一签名 struct + 每 `(trait, type, method)` 一个 thunk + 每 `(trait, type)` 一个实例；实例键不变、槽位顺序不变 · 出处 [DYN.md:645-650](../../docs/topics/DYN.md#L645-L650)、[codegen.c:8086-8203](../../src/codegen.c#L8086-L8203) · 状态：**已定案** · 泛型目标按**每个具体拼写**发（`impl<T> Tag for pair<T>` → `pair_i64` 各一张），靠 `substEnter` 让 thunk 体命名 `pair_i64_tag` · 实现见 [codegen.c:8143-8158](../../src/codegen.c#L8143-L8158)。
- **D78** thunk 怎么生成：`static <ret> extc_th$T$Type$m(void *self, <trait 其余参数>) { return Type_m((Type *)self, …); }`；返回/参数类型取 trait 声明（不是 impl），所以 impl 签名不符时是 thunk **强转**而不是报错（与 D54/D58/D60 的洞叠加） · 出处 [codegen.c:8162-8183](../../src/codegen.c#L8162-L8183)、[DYN.md:445-456](../../docs/topics/DYN.md#L445-L456) · 状态：**已定案（机制）**。
- **D79** 派发前校验（O5 核心）：`extc_dyn_slot` 五段链 —— 槽在界内 → `pid` 相符 → **槽世代**相符 → 池仍是对象表（kind==1）→ **池世代**相符；任一失败 `trap`（带源位置） · 出处 [DYN.md:211-228](../../docs/topics/DYN.md#L211-L228)、[pools.c:110-118](../../src/pools.c#L110-L118)、[DYN.md:653-656](../../docs/topics/DYN.md#L653-L656) · 状态：**已定案**。
- **D80** 表指针只存在**槽**里，值里只有 `{pid, slot, gen, pgen}` ⇒ 槽被复用成另一个实现时世代检查先失败，旧值永远读不到新实现 · 出处 [DYN.md:183-184](../../docs/topics/DYN.md#L183-L184)、[DYN.md:226-228](../../docs/topics/DYN.md#L226-L228)、[DYN.md:689-691](../../docs/topics/DYN.md#L689-L691) · 状态：**已定案**（POOL-SOUNDNESS E3 结构性排除）。
- **D81** dyn 池 = **当前 place 的对象表池**（`extc_pool_new_table`），惰性建、id 记在按 zone 索引的旁表；place 退出 ⇒ 池丢 ⇒ 世代前进 ⇒ 旧值 trap · 出处 [DYN.md:192-200](../../docs/topics/DYN.md#L192-L200)、[DYN.md:651-652](../../docs/topics/DYN.md#L651-L652)、[pools.c:110-118](../../src/pools.c#L110-L118) · 状态：**已定案** · 实现细节：不给 `ExtcZone` 加字段（怕碰发出字符串里的结构体）。
- **D82** 旁表的失效判据有洞：只记 `pid`、用 `generation == 0` 判陈旧 ⇒ 槽位被**另一张活着的对象表**复用时采纳别人的表，dyn 值活过自己的 place · 出处 [pools.c:110-114](../../src/pools.c#L110-L118) · 状态：**与代码不符（P2，见 §14-A）** · 证据：`.audit/findings/misc.json`（`pools.c:118`，已复现 alias2）。
- **D83** 槽回收：墓碑（live=0/pid=-1）+ 数组将增长时惰性清扫 + 空闲链复用 + `gen++`；**池 id 会被复用**（循环体 zone 下标每轮复用）是修前真因 · 出处 [DYN.md:556-573](../../docs/topics/DYN.md#L556-L573)、[DYN.md:577-608](../../docs/topics/DYN.md#L577-L608)、[pools.c:110-118](../../src/pools.c#L110-L118) · 状态：**已定案** · 判据：`dyn_slot_reclaim`（2000 次创建后 live=16，断言 ≤32）。
- **D84** 立即形式（`dyn T(x).m()`）**不进池**（路线 C）：表地址是编译期常量、接收者是表达式临时量；理由 ① 方法按引用收接收者 ⇒ 深度规则已禁止它逃出帧 ② 表达式内不可能陈旧 · 出处 [DYN.md:722-755](../../docs/topics/DYN.md#L722-L755)、[codegen.c:1972-1985](../../src/codegen.c#L1972-L1985) · 状态：**已定案** · 实测 `dynimm` 55.6 ns → 0.3 ns，指令 11.9 条/次（少于 C++ 虚调用 17.3）。
- **D85** 路线 C 的边界：只有「dyn 值是方法调用接收者」这一形态省池；赋值/传参/return/进字面量 ⇒ 句柄逃出表达式 ⇒ 一律进池；按「接收者类型是不是 `TY_DYN`」区分 · 出处 [DYN.md:747-755](../../docs/topics/DYN.md#L747-L755)、[codegen.c:1926-1932](../../src/codegen.c#L1926-L1932) · 状态：**已定案**。
- **D86** `mut ref Self` 的立即形式**必须先拷进栈临时量**（否则写穿源对象，与存储值改池拷贝的语义分叉）；判据 `dyn_imm_copy_semantics` · 出处 [DYN.md:752](../../docs/topics/DYN.md#L752)、[codegen.c:1976-1984](../../src/codegen.c#L1976-L1984) · 状态：**已定案**（代价对 lvalue 是一次 sizeof(T) 拷贝，实测仍 0.3 ns/次）。
- **D87** 引用怎么传：存储值派发只发 `extc_dyn_slot(<值>, "file", line)`，接收者取 `slot->addr`；接收者经 `ref`（`ref dyn Tag`）时 C 表达式是指向句柄的指针 ⇒ 必须 `(*expr)` 再传（`extc_dyn_slot` 按值收句柄） · 出处 [DYN.md:351-355](../../docs/topics/DYN.md#L351-L355)、[codegen.c:1957-1971](../../src/codegen.c#L1957-L1971) · 状态：**已定案**。
- **D88** dyn 调用点的参数检查：条数 = trait 声明参数 − 1（接收者）；逐个 `ttBase(wp->type)` vs `ttBase(at)`，`mentionsParam` 的形参跳过 · 出处 [check_expr.c:3453-3467](../../src/check_expr.c#L3453-L3467) · 陈述：`Self` 位置故意不比（具体类型此刻未知，object safety 已把这类方法挡在表外） · 状态：**与代码不符（两处 P1，见 §14-A）**：① `ttBase` 剥掉**所有** `ref` 层 ⇒ `ref i64` 形参收到裸值也放行，thunk 形参却是指针，生成物编不过；② 用 `checkExprInner` 取实参默认类型、不做 `adoptContextType`/`checkAssignable` ⇒ `d.add(3)` 合法实参被误拒（静态调用同一行合法）。证据：`.audit/findings/checkexpr-3.json`。
- **D89** 载荷必须已实现被点名的 trait（`dyn_implements`），检查在 `EX_DYN` 与 dyn 派发两处共用同一助手 · 出处 [DYN.md:403](../../docs/topics/DYN.md#L403)、[DYN.md:483](../../docs/topics/DYN.md#L483) · 状态：**已定案**。
- **D90** `TY_DYN` 类型没有声明点 ⇒ `ttEquals` 必须按**名字**比较（否则同一个 trait 的两个 `dyn` 类型指针不同而不相等，形参/字段赋值全失败）；`cType(TY_DYN)` 渲染为 `ExtcDynHandle` · 出处 [DYN.md:476-480](../../docs/topics/DYN.md#L476-L480)、[codegen.c:488-490](../../src/codegen.c#L488-L490) · 状态：**已定案**。
- **D91** 存储面已放开：`let` / 结构体字段 / 定长数组 / `varArray<T>`；两条写死的语义：① 逃出 place 的值被**提升**进调用者的 place（仍有效）② 容器 `clear()` **不**使 dyn 值失效（载荷在 dyn 池，不在容器缓冲） · 出处 [DYN.md:314-327](../../docs/topics/DYN.md#L314-L327)、[DYN.md:610-618](../../docs/topics/DYN.md#L610-L618) · 状态：**已定案**（保守但安全）· 判据 `dyn_promoted_return` / `dyn_container_clear`。
- **D92** 已知限制：容器元素不随容器回收（严格版要把容器的池 id 当 dyn 池的 `parent`，`extc_pool_new_table(parent)` 已支持）；是设计决定，留给后续 · 出处 [DYN.md:671-675](../../docs/topics/DYN.md#L671-L675) · 状态：**缺口（有意）**。
- **D93** 陈旧值 `trap`（决定 A）而非返回 `failure`；将来可加 `tryTag()`，不影响既有程序 · 出处 [DYN.md:38-43](../../docs/topics/DYN.md#L38-L43) · 状态：**已定案**。
- **D94** NULL 槽的语义（第三期约定）：表项为 `NULL` 时派发**不能直接跳**，必须是可检查的值（`failure`/`none`）——「条件给值、bug 才 trap」；这是「给 trait 追加方法后老模块仍可被调用」的兼容基础 · 出处 [DYN.md:705-707](../../docs/topics/DYN.md#L705-L707) · 状态：**提案/缺口**（今天 object-unsafe 方法发 `NULL`，但派发被检查器挡住，尚无值语义）。
- **D95** dyn 不是保留字：按**形状** `dyn <标识符>(` 判定（`examples/slices.extc` 有 `let dyn = a[lo..hi]`） · 出处 [DYN.md:80-82](../../docs/topics/DYN.md#L80-L82)、[DYN.md:516](../../docs/topics/DYN.md#L516)、[parser.c:3101](../../src/parser.c#L3101) · 状态：**已定案（纪律）**。
- **D96** 运行时按需发出：`Module.usesDyn` 由检查器在 `dynTraitOf` 置位，codegen 据此发 `poolsEmitDynRuntime` 与序言 3 行句柄声明 · 出处 [DYN.md:540-554](../../docs/topics/DYN.md#L540-L554)、[codegen.c:7065-7070](../../src/codegen.c#L7065-L7070)、[codegen.c:8461](../../src/codegen.c#L8461) · 状态：**与代码不符（P1，见 §14-A）**：`usesDyn` 只在**产生 dyn 值**时置位，仅在数据位置（字段/数组元素/泛型实参）**命名** dyn 类型会漏 `ExtcDynHandle` typedef，生成物编不过（`.audit/findings/genwarn.json`，已复现）。
- **D97** 三条纪律：① dyn 不是保留字 ② 检查趟有顺序，不要在检查器里「提前检查」接收者窥视类型（曾弄坏无关 stdlib 用例）③ codegen 读节点上的类型 ⇒ 凡「窥视」所得必须写回节点 · 出处 [DYN.md:665-669](../../docs/topics/DYN.md#L665-L669) · 状态：**已定案（纪律）**。
- **D98** 第三期（开放注册/动态链接）作者明确**不做**，但留门：三条不变量必须保持（表指针只在槽里 / 键是稳定 mangled 名 / 失效信号与模块解耦）+ 两处将来可改的位置（`ExtcDynSlot.vt` 换成注册项引用、加 `slot→module` 旁表）+ 接口版本「布局只能追加」 · 出处 [DYN.md:680-720](../../docs/topics/DYN.md#L680-L720)、[DYN.md:620-623](../../docs/topics/DYN.md#L620-L623) · 状态：**提案（作者明示暂不做）**。
- **D99** O5 已翻面：`SOUNDNESS.md` O4/O5、`POOL-SOUNDNESS.md` E3 标为已闭合，手册 §7.3 同步 · 出处 [DYN.md:396-412](../../docs/topics/DYN.md#L396-L412)、[DYN.md:617-618](../../docs/topics/DYN.md#L617-L618) · 状态：**已定案**。

---

## 6. lambda / 闭包

- **D100** 语法锁定 B：`fn(x: i64) -> i64 { … }`（复用 `fn`，零新符号）；返回类型可省（由体内第一个 `return <value>` 定）；可写捕获列表 `[mut ref x]`，符号本身待确认 · 出处 [LAMBDA.md:16-20](../../docs/topics/LAMBDA.md#L16-L20)、[LAMBDA.md:30-52](../../docs/topics/LAMBDA.md#L30-L52)、[parser.c:2975-3068](../../src/parser.c#L2975-L3068) · 状态：**已定案**（方括号是「本方案默认」，备选 `mut(ref x)` 未决）。
- **D101** 身份编译期唯一：每个 lambda 生成一个**唯一类型**（捕获结构体 + `call` 方法）⇒ 调用是直接调用、可内联、无间接调用表 · 出处 [LAMBDA.md:22-25](../../docs/topics/LAMBDA.md#L22-L25)、[check_expr.c:819-838](../../src/check_expr.c#L819-L838) · 状态：**已定案**。
- **D102** 传参走**单态化**：把闭包传给函数走泛型参数（`fn each<T, F>(v: []T, f: F)`）⇒ 每个闭包类型实例化一份，仍是直接调用 · 出处 [LAMBDA.md:26](../../docs/topics/LAMBDA.md#L26) · 状态：**已定案（设计）/缺口（实现）**：今天「把闭包传给泛型函数」报 `call to undefined function`（见 D107）。
- **D103** v1 **不引入可写的函数类型** `fn(T)->R`（一旦可写就要回答「两个不同闭包怎么统一」）；异构/运行期注册走 `pool<dyn Trait>` · 出处 [LAMBDA.md:65-67](../../docs/topics/LAMBDA.md#L65-L67)、[LAMBDA.md:87-113](../../docs/topics/LAMBDA.md#L87-L113) · 状态：**已定案** · 实测：`var f: fn(i64) -> i64 = …` 报 `expected a type, found 'fn'`，与决策一致。
- **D104** 捕获环境如何物化：读外层变量 ⇒ **按值**拷贝成字段（不写任何东西）；写外层变量 ⇒ **必须显式** `[mut ref x]`，字段类型 `mut ref T`，构造时取 `&x`；捕获 `ref T` 的值 ⇒ 按值拷引用；未提及 ⇒ 不捕获 · 出处 [LAMBDA.md:69-77](../../docs/topics/LAMBDA.md#L69-L77)、[check_expr.c:952-991](../../src/check_expr.c#L952-L991) · 状态：**已定案** · 校验：写了但未列入 ⇒ 报「the body writes `x`, but the capture list does not name it」；列入但没用 ⇒ 报「`x` is not an enclosing variable this body uses」（[check_expr.c:931-950](../../src/check_expr.c#L931-L950)）。
- **D105** 环境结构体的字段按**序**命名为 `c0/c1…`；体内被捕获名字改写成 `self->cN`（可写 `(*self->cN)`），实现靠给 `ident.cname` 赋值、**不动 AST** · 出处 [check_expr.c:775-803](../../src/check_expr.c#L775-L803)、[LAMBDA.md:127-136](../../docs/topics/LAMBDA.md#L127-L136) · 状态：**已定案** · 细节：`call` 的 `self` 是**只读** `ref <env>`（[check_expr.c:851-861](../../src/check_expr.c#L851-L861)）。
- **D106** 捕获集从**已解析的 Sym** 上读，不猜文本；体在**原地**检查（形参入 scope）⇒ 遮蔽自然正确 · 出处 [LAMBDA.md:127-135](../../docs/topics/LAMBDA.md#L127-L135)、[check_expr.c:880-915](../../src/check_expr.c#L880-L915) · 状态：**已定案**（名字预扫描会把 `var base` 遮蔽外层 `base` 判错）。
- **D107** 生命周期**不新增概念**：捕获结构体是普通值，装 `ref`/`mut ref` 就受既有逃逸/arena 检查；返回捕本帧局部的闭包由现有规则拒 · 出处 [LAMBDA.md:78-85](../../docs/topics/LAMBDA.md#L78-L85)、[LAMBDA.md:123](../../docs/topics/LAMBDA.md#L123) · 状态：**已定案**（作者注：「类型不可命名 ⇒ 值只能活在写它的作用域里」，所以整类逃逸问题**写不出来**）。
- **D108** 明确划出的边界（每条都报清楚、不静默）：闭包套闭包 · 体内 `new` · 把闭包传给泛型函数 · 返回闭包/声明闭包变量 · 出处 [LAMBDA.md:157-164](../../docs/topics/LAMBDA.md#L157-L164)、[check_expr.c:810-816](../../src/check_expr.c#L810-L816)、[check_expr.c:919-929](../../src/check_expr.c#L919-L929) · 状态：**已定案**。
- **D109** **泛型里的 T 捕获是未声明的静默失败** · 出处 [check_expr.c:823-826](../../src/check_expr.c#L823-L826)、[check_expr.c:958-973](../../src/check_expr.c#L958-L973)、[codegen.c:512-513](../../src/codegen.c#L512-L513) · 陈述：环境结构体每模板只生成一次（名字只带 owner+序号，与类型实参无关），字段类型直接取 `sy->type`（模板期是 `TY_PARAM`），既不做 `ttSubstitute` 也不按实例重建；codegen 对残留 `TY_PARAM` 一律给 `"int"` ⇒ `struct $lam$gen$0 { int c0; }`，`T=struct` 时生成物编不过、`T=i64/u64/f64` 时静默截断/类型混淆 · 状态：**与代码不符（P1，见 §14-A）**（LAMBDA.md §7 边界清单未列此条）· 证据：`.audit/findings/checkexpr-1.json`；本文实测：`fn gen<T>(x: T)` 内建闭包捕 `x`，`gen<point>(p)` 生成 `int c0`、gcc 报 `incompatible types when initializing type 'int' using type 'point'`。
- **D110** 走查器语义：闭包体归 `call` 方法自己的按函数分析，**不归外层函数**；外层透过闭包节点只看到环境字段值那几次读（与 `EX_STRUCTLIT` 同形） · 出处 [LAMBDA.md:145-148](../../docs/topics/LAMBDA.md#L145-L148)、[AST-WALKERS.md:9-18](../../docs/topics/AST-WALKERS.md#L9-L18) · 状态：**已定案**（走查器去重后 ALLOW 清空）。
- **D111** lambda 与 `coroutine<A,B>` 共用「环境显式化成结构体」的变换；协议方法必须用**标记自己的类型参数名**表达（替换按 `Type.param` 名字匹配） · 出处 [LAMBDA.md:166-184](../../docs/topics/LAMBDA.md#L166-L184) · 状态：**已定案** · 注：文档里「trait 今天不能带类型参数」（[LAMBDA.md:110-113](../../docs/topics/LAMBDA.md#L110-L113)）已被 D62 推翻（`trait Codec<T>` 现可用）。
- **D112** lambda 判据：10 正例 + 8 反例（`tests/lambda/`），实现步骤以「414 个生成 C 逐字节不变」为闸门 · 出处 [LAMBDA.md:115-126](../../docs/topics/LAMBDA.md#L115-L126) · 状态：**已定案（流程）**。

---

## 7. 零值与默认初始化

- **D113** 总纲：省略初始化式 = **零初始化**（C 的 `{0}`）；由「类型有没有零值」决定是否合法 · 出处 [ARRAYS.md:145-167](../../docs/topics/ARRAYS.md#L145-L167)、[check_stmt.c:250-275](../../src/check_stmt.c#L250-L275) · 状态：**已定案**。
- **D114** **没有零值**的类型：非空 `ref` · `fn` · 含它们的聚合（递归判定，结构体字段与枚举载荷都看） · 出处 [check_lookup.c:644-702](../../src/check_lookup.c#L644-L702)、[check_lookup.c:607-642](../../src/check_lookup.c#L607-L642) · 状态：**已定案**。
- **D115** **`?ref T` 的零值是 null**，这正是它存在的理由（链表/树节点的 `next`） · 出处 [check_lookup.c:674-676](../../src/check_lookup.c#L674-L676) · 状态：**已定案**。
- **D116** 带载荷枚举只看**第一个 variant** 的载荷，声明顺序有意义：`| nothing | holding(slice<u8>)` 可零值，反序不可 · 出处 [check_lookup.c:644-651](../../src/check_lookup.c#L644-L651)、[check_lookup.c:683-689](../../src/check_lookup.c#L683-L689) · 状态：**已定案**。
- **D117** **泛型实例怎么判定**：必须先把实参替换进字段/载荷再走判定（`option<i64>` 可以、`option<slice<u8>>` 不可以）；深度超过 `ZERO_VALUE_DEPTH_LIMIT=64` 保守判「无零值」 · 出处 [check_lookup.c:659-663](../../src/check_lookup.c#L659-L663)、[check_lookup.c:671-702](../../src/check_lookup.c#L671-L702)、[check_lookup.c:14](../../src/check_lookup.c#L14) · 状态：**已定案**。
- **D118** 泛型体里的零值检查走延迟机制：标注提及 `T` ⇒ `recordZeroCheck`；实例期 `runRefCheck(isZero)` 报「in instance `X`: `b` has no zero value (it contains a reference)」 · 出处 [check_stmt.c:252-255](../../src/check_stmt.c#L252-L255)、[check_escape.c:1485-1502](../../src/check_escape.c#L1485-L1502)、[check_top.c:4865-4877](../../src/check_top.c#L4865-L4877) · 状态：**已定案（设计）**。
- **D119** **缺口（P0）**：三个记录端（`recordRefCheck`/`recordNewSizeCheck`/`recordZeroCheck`）都以 `!c->curFunc->owner` 提前返回 ⇒ **自由泛型函数**的零值检查永不记录，而唯一的消费路径 `refCheckApplies` 恰好只认自由函数实例 · 出处 [check_escape.c:1473-1477](../../src/check_escape.c#L1473-L1477)、[check_escape.c:1495-1497](../../src/check_escape.c#L1495-L1497)、[check_escape.c:1526-1530](../../src/check_escape.c#L1526-L1530)、[check_top.c:4819-4823](../../src/check_top.c#L4819-L4823) · 陈述：`struct cellfn { f: fn(i32)->i32 }` + `fn mk<T>() -> T { var b: T return b }` 被接受，生成 `(cellfn){0}`，调用空函数指针 ⇒ SIGSEGV · 状态：**与代码不符（P0，见 §14-A）** · 证据：`.audit/notes.md` F-10、`.audit/findings/checktop-3.json`。
- **D120** 全局零初始化：全局是 depth 0，含引用的全局必须给初始化式（「没有可借的东西」） · 出处 [check_top.c:4401-4411](../../src/check_top.c#L4401-L4411) · 状态：**已定案**。
- **D121** `coroutine<T>` 局部变量**没有零值**（句柄无意义零 = null frame + bogus task），专门诊断要求初始化 · 出处 [check_stmt.c:243-250](../../src/check_stmt.c#L243-L250) · 状态：**已定案**。
- **D122** **缺口（P1）**：`typeLacksZeroValue` 不认 `coroutine<T>` 聚合形式与 `TY_DYN` ⇒ 字段/数组元素/泛型实参里的零 `coroutine` 句柄被放行，驱动 NULL 帧（SEGV）；dyn 零句柄会 trap（安全） · 出处 [check_lookup.c:671-702](../../src/check_lookup.c#L671-L702) · 状态：**与代码不符（P1，见 §14-A）** · 证据：`.audit/findings/lookup.json`（`check_lookup.c:671`）。
- **D123** **缺口（P1）**：泛型实例的零值在 codegen 里逐字段写 designator；`coroutine<T>` 实例的 C 类型是 `extc_coro`（真实句柄结构无 `unit` 字段）⇒ 生成 `(extc_coro){ .unit = false }`，生成物编不过 · 出处 [codegen.c:1527-1595](../../src/codegen.c#L1527-L1595)（designator 在 1574）、[codegen.c:485-499](../../src/codegen.c#L485-L499) · 状态：**与代码不符（P1，见 §14-A）** · 证据：`.audit/findings/genwarn.json`。

---

## 8. 数组与视图（ARRAYS.md）

- **D124** 固定数组 `[N]T` 从外到内、可递归多维；类型统一套 struct，**成员也套** ⇒ `a[i]` 永远生成 `a.data[i]`，一条规则零特例 · 出处 [ARRAYS.md:89-129](../../docs/topics/ARRAYS.md#L89-L129) · 状态：**已定案** · 理由：不把「这个值是独立变量还是 struct 成员」的上下文塞进 codegen。
- **D125** 初始化：字面量严格计数、`...` 只能补尾；省略初始化式即零值；`[0; N]` 全零语法**砍掉**（已是默认） · 出处 [ARRAYS.md:16](../../docs/topics/ARRAYS.md#L16)、[ARRAYS.md:142-167](../../docs/topics/ARRAYS.md#L142-L167) · 状态：**已定案**。
- **D126** 下标写读与边界检查：`extc_checkedIndex(i, n, file, line)` 越界 trap 并带 extC 位置；用逗号表达式让 `i` **只求值一次**；可证明时零检查 · 出处 [ARRAYS.md:176-227](../../docs/topics/ARRAYS.md#L176-L227)、[codegen.c:2492-2500](../../src/codegen.c#L2492-L2500)、[codegen.c:7170](../../src/codegen.c#L7170) · 状态：**已定案**。
- **D127** 切片 `a[lo..hi]`：四种写法；**省略的界由 check 阶段补成字面量**（`a[2..]`→`a[2..15]`），codegen 只判「两界都是字面量」⇒ 零检查，而不是再判「有没有省略」 · 出处 [ARRAYS.md:230-253](../../docs/topics/ARRAYS.md#L230-L253) · 状态：**已定案**。
- **D128** 界是字面量但越界（含负数界）⇒ **编译期报错**（`slice end 9 is not inside [5]i32`）；界里有变量 ⇒ 运行时 `extc_checkedRange` trap · 出处 [ARRAYS.md:255-264](../../docs/topics/ARRAYS.md#L255-L264)、[codegen.c:5352-5366](../../src/codegen.c#L5352-L5366) · 状态：**已定案**。
- **D129** 切固定数组的底必须是 **place**（变量/字段/索引链），不能切临时值（那会切到返回值临时存储 ⇒ 指向已死对象）；切 **slice** 不受限（`"abcdef"[1..3]` 合法） · 出处 [ARRAYS.md:266-278](../../docs/topics/ARRAYS.md#L266-L278) · 状态：**已定案**。
- **D130** 视图元素是 **lvalue**：索引原语返回指针、调用点解引用（`(*s_index(...))`）；故视图可写 · 出处 [ARRAYS.md:293-309](../../docs/topics/ARRAYS.md#L293-L309) · 状态：**已定案** · 理由：否则 `slice<struct>` 完全编不出来、`s[i].field = x` 报 lvalue required。
- **D131** 多维只切最后一段；`b[2..5][3..8]` **不是**二维切片（拿到的是「几行」的视图），不连续的东西没法用一个指针+长度表示 · 出处 [ARRAYS.md:56-58](../../docs/topics/ARRAYS.md#L56-L58)、[ARRAYS.md:280-291](../../docs/topics/ARRAYS.md#L280-L291) · 状态：**已定案**。
- **D132** 动态数组的**最终名字是 `varArray<T>`**，写在 `stdlib/prelude.extc`（不是编译器内建，也不是 `array<T>`） · 出处 [ARRAYS.md:20](../../docs/topics/ARRAYS.md#L20)、[prelude.extc:162-171](../../stdlib/prelude.extc#L162-L171) · 陈述：§5 的 `array<T>` 形状与「两个必须先解决的问题」已被 `varArray`（`buf: mut slice<T>` + `new T[cap]`）取代；「能写在 prelude 里就写在 prelude 里」 · 状态：**与代码不符（文档 §5 整体滞后）**。
- **D133** 数组 `==` 由类型系统递归支持、`println` 调试打印 · 出处 [ARRAYS.md:18](../../docs/topics/ARRAYS.md#L18) · 陈述：文档点名 `typeSupportsEq`，**该函数在 src/ 中不存在**（现为 `typeSupportsOp`，[check_top.c:4677](../../src/check_top.c#L4677)）；`[3]i32 == [3]i32` 实测可用（`--run` 返回 1） · 状态：**与代码不符（函数名过时，行为正确）**。
- **D134** 已知代价：`slice<u8>` 可来自字符串字面量（只读段），往 `"abc"[0]` 写会崩；extC 没有 `const`，暂时靠「别那么写」+ 文档 · 出处 [ARRAYS.md:306-309](../../docs/topics/ARRAYS.md#L306-L309) · 状态：**缺口（有意）**。
- **D135** 还没定的：范围类型（让索引可证明 ⇒ 零检查）· `for` 的四种形态 · 数组字面量的上下文推导 · 多维数组作参数的值拷贝成本 · 出处 [ARRAYS.md:382-390](../../docs/topics/ARRAYS.md#L382-L390) · 状态：**缺口**（`for x in c` 今天不行，检查器要 `iter` 方法，[LAMBDA.md:186](../../docs/topics/LAMBDA.md#L186)）。

---

## 9. STL：容器面与跨容器语义

- **D136** `stdlib/stl/` 是**唯一**装动态容器的地方（一个库装所有容器，不是一个容器一个库）；STL.md 是成员函数面的**权威清单**，改面必须同步 · 出处 [STL.md:1-7](../../docs/topics/STL.md#L1-L7) · 状态：**已定案（流程）**。
- **D137** 命名裁决：`map`/`set` 是**有序**（B+ 树）· `hashMap`/`hashSet` 是**哈希** · `lin*` 是只要求 `==` 的第三条路；理由分散在 POOLS.md §10/§8/§11 · 出处 [STL.md:6-7](../../docs/topics/STL.md#L6-L7)、[STL.md:11-22](../../docs/topics/STL.md#L11-L22) · 状态：**已定案** · 键要求：hash 族要 `hash()`+`==`，有序族只要 `<`，lin 族只要 `==`。
- **D138** `put` 返回值两族**相反**：`map`/`hashMap`/`linMap` 是「true = 键本来就在（这次是覆盖）」；`set`/`hashSet`/`linSet` 是「true = 这是新元素」；翻转点各带注释、是显式决定不是笔误 · 出处 [STL.md:122-124](../../docs/topics/STL.md#L122-L124) · 状态：**已定案** · 核对：`set.extc:36-37`（`if self.m.put(k, 0) { return false }`）。
- **D139** `[]` 与 `[]=` **互相独立**：`[]` 只读、缺键给 `none`（**没有默认插入**）；`[]=` 是 upsert（缺则新建、有则覆盖，返回值同 `put`）；只定义 `[]` 的类型不能下标写，检查器给可读诊断而不是「结果不可赋值」 · 出处 [STL.md:125-126](../../docs/topics/STL.md#L125-L126) · 状态：**已定案** · 实现：读侧转成 `EX_METHOD "[]"`（[check_expr.c:1672-1690](../../src/check_expr.c#L1672-L1690)）；写侧转成 `"[]="` 或报专门诊断（[check_stmt.c:565-591](../../src/check_stmt.c#L565-L591)）。
- **D140** `clear`/`shrink`/`release` 三档：`clear` 清空留容量；`shrink` 降**水印**（工作集；arena 不能逐块还内存，所以不是 RSS）；`release` 把运行期池整批还掉，之后该容器不能再当池用（陈旧拷贝被 `pidGen` 挡住） · 出处 [STL.md:127-128](../../docs/topics/STL.md#L127-L128) · 状态：**已定案**。
- **D141** 缺键不插值：所有 `get`/`[]` 都返回 `?V`，缺席就是 `none` · 出处 [STL.md:129](../../docs/topics/STL.md#L129) · 状态：**已定案**。
- **D142** 已知缺口（容器面）：`hashSetI64`/`set<T>` 没转发 `release`/`shrink`；`hashMapI64`/`linMap`/`linSet` 没有 `release`；`vector` 无 `insertAt`/`removeAt`；`string` 无 `pop`；`map::shrink` 只压节点存储、不压「每节点 17 格」的稀疏度 · 出处 [STL.md:131-136](../../docs/topics/STL.md#L131-L136) · 状态：**缺口**。
- **D143** 有序容器**不暴露元素句柄**是一处**有意的偏离**（POOLS.md §11 原话是「叶子存键 + 池句柄」）：扁平存储已提供可搬迁/连续/下标寻址，句柄只加一层间接；以后要稳定引用（ECS 那种）再补 · 出处 [STL.md:137-139](../../docs/topics/STL.md#L137-L139) · 状态：**已定案（有意偏离）**。
- **D144** **缺口**：`[]=` 目前只对「对象类型直接定义」生效；**泛型体内对类型参数做下标写入还没接上推迟检查（读侧同理）** · 出处 [STL.md:140](../../docs/topics/STL.md#L140) · 陈述：本文实测读侧确认 —— `fn get<T>(m: T, k: i64) -> i64 { return m[k] }` 报 `cannot index a value of type `T``（[check_expr.c:1706-1708](../../src/check_expr.c#L1706-L1708)：检查器根本不把 `T` 的 `[]` 当潜在方法，D7 的 `#57` 延迟只覆盖具名方法） · 状态：**缺口（已实测确认）**。
- **D145** 验收映射：`pool`→tests/pool；`vector`/`string`/`hashSetI64`/`set`→tests/stl；`hashMap`→tests/hashmap；`linMap`/`linSet`→tests/linmap；`map`/`set`→tests/map（含 RSS 两条 + ASan）；`check.sh quick` 节数与清单一一对应（当前 26 节） · 出处 [STL.md:142-152](../../docs/topics/STL.md#L142-L152) · 状态：**已定案（流程）**。
- **D146** **P0 缺陷**：`stdlib/stl/hashSet.extc` 作为**根文件**编译永不返回（`dropRuntimeDefs` 的 `continue` 跳过循环推进语句 ⇒ 100% CPU 死循环）；作为被 `use` 的模块正常 · 出处 [codegen.c:5790-5799](../../src/codegen.c#L5790-L5799) · 陈述：触发状态由上一轮 `dropRuntimeDefs` 自己制造（切出截断的 `#define EXTC_ZON`），下一轮 `defs==0` ⇒ `continue` 原地打转 · 状态：**与代码不符（P0，见 §14-A）** · 证据：`.audit/findings/hang-hashset.json`；本文实测 `timeout -s KILL 20 ./build/extc -w stdlib/stl/hashSet.extc -o /dev/null` → rc=124，而 `use stl::hashSet` 的程序 rc=0。

---

## 10. AST 遍历器（AST-WALKERS.md）

- **D147** 现状：同一问题被手写 **≈8 套**遍历器（另加 3 层传递闭包），仓库注释自己承认它们**同时**犯同一个错 · 出处 [AST-WALKERS.md:7-22](../../docs/topics/AST-WALKERS.md#L7-L22) · 状态：**已定案（问题陈述）**。
- **D148** 已发生的真缺陷：`EX_SLICE` 三个子表达式（对象/下界/上界）被**每一个**遍历器漏掉（effects 少汇总、限定名从未重写）；`EX_STRUCTLIT` 里的调用被漏 ⇒ 传递闭包误判「走不到需要 home arena 的被调者」⇒ 悬垂 · 出处 [AST-WALKERS.md:26-35](../../docs/topics/AST-WALKERS.md#L26-L35) · 状态：**已定案（历史缺陷）**。
- **D149** 目标：一个通用访问器 `walkExpr`/`walkStmt` + 小回调（回调返回 false = 提前停止，保持「找到就退出」的语义与开销）；闭包统一成 `closeOverCalls(all, ReachKind)`（四取值：needsHome(wide)/usesHome(narrow)/makesPool/allocates） · 出处 [AST-WALKERS.md:43-58](../../docs/topics/AST-WALKERS.md#L43-L58) · 状态：**提案/部分落地**（`astWalkExprChildren`/`astWalkStmtChildren` 已在 `src/ast.c:211/263`，`closeReach` 已收口）。
- **D150** 迁移顺序（低→高风险，每步一个提交）：1 `exprUsesCname` ✅ · 2 `exprHasNew` ✅ · 3 `exprCallsAllocator` ✅ · 4 `exprCallsNeedsHome` ✅ · 5 `markNamesInExpr`/`collectEffectsExpr`（有副作用，必须保序遍历）· 6 codegen `collectOwCallsExpr` · 7 loader `rwExpr` · 出处 [AST-WALKERS.md:60-70](../../docs/topics/AST-WALKERS.md#L60-L70) · 状态：**已定案（1-4 已完成，5-7 待做）**。
- **D151** 机械判据 `tools/check_walkers.py`：`RATCHET` 只降不升（现 **23**），新增手写遍历器**直接红**；数字必须由判据数、不能靠人数（前两次人工统计 17/41 都是错的） · 出处 [AST-WALKERS.md:37-41](../../docs/topics/AST-WALKERS.md#L37-L41)、[check_walkers.py:46](../../tools/check_walkers.py#L46) · 状态：**已定案**（文档 29→23 与 RATCHET=23 一致）。
- **D152** `EX_DYN` 曾不被任何手写遍历器处理（带 `default:` 的 switch **不报** `-Wswitch`）；16 处 case 补齐（含 `markNamesInExpr` 连 `EX_GENCALL` 都漏） · 出处 [AST-WALKERS.md:116-154](../../docs/topics/AST-WALKERS.md#L116-L154) · 状态：**已定案（止血完成，根治未做）**。
- **D153** 修复分级（诚实评估）：`exprUsesCname` 漏 ⇒ 多余「从未使用」警告（最易观察）；loader `rwExpr` 漏 ⇒ 载荷限定名不重写、生成物 C 名错；`collectEffectsExpr` 漏 ⇒ effects 少报；其余四个「理论上 H2 那类」，但 dyn 载荷不能带引用，爆炸半径受限 · 出处 [AST-WALKERS.md:128-138](../../docs/topics/AST-WALKERS.md#L128-L138) · 状态：**已定案（风险裁定）**。
- **D154** 闭包收口（已完成）：`closeReach` 是**唯一**「单调属性沿调用图取最小不动点」驱动（此前 4 个 `for(bool changed…)`）· `getReach`/`setReach` 唯一属性映射 · `bodyReaches` 唯一每节点判据 · `roundOverModule` 唯一轮骨架；**故意保留**的差异：`funcAllocates` 仍惰性+记忆化（必须在闭包前回答） · 出处 [AST-WALKERS.md:96-115](../../docs/topics/AST-WALKERS.md#L96-L115)、[check_top.c:5052-5104](../../src/check_top.c#L5052-L5104) · 状态：**已定案**。
- **D155** 判据：① 枚举全种类自检（`EXTC_WALK_SELFCHECK` 或 case 集合核对）② 每步 `check.sh` 全绿 ③ 每步**生成物逐字节 A/B** ④ 第 4 步后加跑 arena-soundness/arena-promoted · 出处 [AST-WALKERS.md:85-94](../../docs/topics/AST-WALKERS.md#L85-L94) · 状态：**部分落地** —— `tools/check_walkers.py` 已接进 `check.sh`；`EXTC_WALK_SELFCHECK` 这个环境变量**在 src/ 中不存在**（只有 `EXTC_SELFCHECK`，[check_top.c:6383](../../src/check_top.c#L6383)）⇒ ① 的实现形式与文档写的不符 · 状态：**与代码不符（细节）**。
- **D156** H2（home zone 传递闭包只做一层）是同片区域的另一症状，已修；本文件是那次合并的系统性延伸 · 出处 [AST-WALKERS.md:159-164](../../docs/topics/AST-WALKERS.md#L159-L164) · 状态：**已定案**。

---

## 11. 编译时间（COMPILE-TIME.md）

- **D157** 判据：与 gcc 对标（绝对量 + 每翻倍比值），不追求完美线性；当前 N=200 只花 gcc -O2 的 8.2% · 出处 [COMPILE-TIME.md:6-26](../../docs/topics/COMPILE-TIME.md#L6-L26) · 状态：**已定案（度量口径）**。
- **D158** 真正的收益全部来自三处**复杂度级**修复：`funcCallsItself`（O(N³)→O(N)，`468cd58`）· `unitOwns`（O(N²)→O(1) 等义缓存，`295f372`）· `countMentions`（扫描量 36×↓，`a425879`）；另有 4 处常数级 · 出处 [COMPILE-TIME.md:27-39](../../docs/topics/COMPILE-TIME.md#L27-L39) · 状态：**已定案**。
- **D159** 阶段表现在全部线性，只剩 `cg-locals`（×3.7）与 `check`（×3.6）呈**弥散**形态（访问量线性、每次操作代价随内存增长） · 出处 [COMPILE-TIME.md:38-39](../../docs/topics/COMPILE-TIME.md#L38-L39)、[COMPILE-TIME.md:78-101](../../docs/topics/COMPILE-TIME.md#L78-L101) · 状态：**已定案（结论）**。
- **D160** 测量手段可信/不可信清单：阶段计时与 callgrind 可信（`cgReport` 总计要读**最后一遍**）；perf 的 `--call-graph dwarf` 三次给出错误归属，全被阶段计时否证；指令占比 ≠ 墙上时间（`strstr` 72.89% 指令几乎不花时间） · 出处 [COMPILE-TIME.md:41-61](../../docs/topics/COMPILE-TIME.md#L41-L61) · 状态：**已定案（方法论）**。
- **D161** **已被实测否证的假说清单（别重走）**：19 条，含 `dropUnusedLocals` 批处理、`funcCallsItself` 记忆化、等义缓存无守卫版会**改输出**、`countMentions` 计数表墙钟零收益、arena 块 64KiB→1MiB / 4MiB+THP、`-O2/-O3` 重建编译器零收益、扫描字节不驱动时间（41GB→24GB 时间不动）· 出处 [COMPILE-TIME.md:63-76](../../docs/topics/COMPILE-TIME.md#L63-L76)、[COMPILE-TIME.md:102-115](../../docs/topics/COMPILE-TIME.md#L102-L115)、[COMPILE-TIME.md:154-168](../../docs/topics/COMPILE-TIME.md#L154-L168) · 状态：**已定案（负面结果归档）**。
- **D162** `checkFunc` 的**全模块符号扫描**是真 O(N²)（每函数 ~6400 次读），修法是「追加友好的 per-name `Vec<Sym*>`」，**不是**「守卫式索引重建」（那会把 O(N) 构建变 O(N²)，实测慢 2.6 倍） · 出处 [COMPILE-TIME.md:116-153](../../docs/topics/COMPILE-TIME.md#L116-L153) · 状态：**已定案（已修，`477b298`：ckB8 0.172→0.077 s）**。
- **D163** 预取是**选择性**杠杆：两处最热循环 −4.6%、再加 17 处累计 −6.7%，全仓 285 处 **+11%** 已退回；测量升级为 median-of-5（±1–2%） · 出处 [COMPILE-TIME.md:169-183](../../docs/topics/COMPILE-TIME.md#L169-L183) · 状态：**已定案**。
- **D164** 任何改动必须先过 **413 个生成 C 的逐字节比对**（sha256 基准），无收益或改输出的一律退回 · 出处 [COMPILE-TIME.md:84-86](../../docs/topics/COMPILE-TIME.md#L84-L86) · 状态：**已定案（闸门）** · 注：基准本身冻着一份非法 C（`examples__prelude.extc.c:123`，`TRAITS.md:243-250` 已证），说明闸门只保证「不变」不保证「正确」。

---

## 12. 自举与搬迁（MIGRATION.md）

- **D165** 原则：**能在 extC 里写的东西，就在 extC 里写**（作者 2026-09-18 定，除非很难/很影响效率） · 出处 [MIGRATION.md:3-5](../../docs/topics/MIGRATION.md#L3-L5) · 状态：**已定案（原则）**。
- **D166** 两阶段构建（部分自举）：① 纯 C 构建 extc ② 用 extc 编 `stdlib/*.extc` → `build/stdlib.c` ③ 链进 extc；现状**只做到把 `stdlib/prelude.extc` embed 成 `build/prelude_data.c`**，②③ 未实现 · 出处 [MIGRATION.md:16-24](../../docs/topics/MIGRATION.md#L16-L24)、[Makefile:42-47](../../Makefile#L42-L47) · 状态：**与代码不符（文档描述的机制只实现了一半）** · 附带好处（stdlib 每次改动都被「用 extC 编 extC」检验）目前只覆盖 prelude。
- **D167** 风险与退路：stdlib 崩了要保留 C fallback（编译期开关） · 出处 [MIGRATION.md:29-30](../../docs/topics/MIGRATION.md#L29-L30) · 状态：**提案**（代码无此开关）。
- **D168** 搬迁审计表：约 15 处适合搬、3 处首选（`ctxRenderDiag` · `ttCanWiden`+整数表 · 词法器）；卡点逐条列 · 出处 [MIGRATION.md:38-62](../../docs/topics/MIGRATION.md#L38-L62) · 状态：**提案（当时结论）**。
- **D169** §2「硬门槛」整节已全面过时：文档写「没有数组、索引、字符串操作、泛型、option、match」，今天全部存在（`slice<T>` 在 prelude、泛型/option/match 全线可用） · 出处 [MIGRATION.md:66-85](../../docs/topics/MIGRATION.md#L66-L85) · 状态：**与代码不符（历史快照，不应当作现状读）**。
- **D170** `str == str` 指针比较事故的处置（2026-09-18）：直接**报错**而不是安静给错答案；通用原则「**语言层承诺的语义，生成的 C 必须真的实现**——让 C 的默认行为偷偷替代，就制造了一个未来的陷阱」 · 出处 [MIGRATION.md:89-110](../../docs/topics/MIGRATION.md#L89-L110) · 状态：**已定案（原则）** · **现状核对**：`str` 今天在源码里根本解析不出（`unknown type `str``，虽然诊断的 builtin 列表里还列着它）；字符串字面量的类型是 `slice<u8>`，`"hi" == "hi"` 走 prelude 的按内容比较 ⇒ §3 的具体处置已被 T4c 取代，只剩原则仍有效。
- **D171** 建议的搬迁顺序：`ctxRenderDiag` → `ttCanWiden`+整数表 → 词法器 → `Buf`/`Vec` → `lookup`/`find*` · 出处 [MIGRATION.md:114-124](../../docs/topics/MIGRATION.md#L114-L124) · 状态：**提案**。

---

## 13. 跨主题：含糊、矛盾与未决（汇总）

- **A1** **「trait 一致性比什么」没有单一权威**：文档说「签名逐项相符」（[TRAITS.md:21](../../docs/topics/TRAITS.md#L21)），代码实际只比「名字 + 有无接收者 + 参数个数 + 参数类型 + 双非 void 的返回类型」（[check_top.c:249-295](../../src/check_top.c#L249-L295)）；**effects 完全没比**（D56）、**接收者 mut 没比**（D55）、**builtin/视图目标整段跳过**（D60）、**无接收者方法的第一参数不比**（D57）、**void↔非 void 放行**（D58）。这五处彼此叠加，任何一处单独读代码都「看起来像对的」。
- **A2** **「实例化时机」有两套语义**：类型实例表按增长迭代到不动点（D11），而延迟调用记录只重放一遍（D10）⇒ 同一个「晚一点才知道」的问题，一个修好了、一个没有；文档（GENERICS.md）把两者都当作已修的类（#60-#64），没有区分。
- **A3** **零值/引用检查的记录端以 `owner == NULL` 早退**（D119），而消费端只认 `tmpl`（自由函数）——记录端与消费端的判据**互为补集**，这条路径永远为空。文档层面完全没有记这件事。
- **A4** **`dyn` 的两份「能力声明」不一致**：文档说 `ref dyn Tag` 第二期不提供（[DYN.md:27](../../docs/topics/DYN.md#L27)），实现已支持（D76）；文档说 trait 不能带类型参数（[LAMBDA.md:110-113](../../docs/topics/LAMBDA.md#L110-L113)），实现已支持（D62）；文档说「impl 侧带参 trait 会段错误、改动已回退」（[TRAITS.md:191-202](../../docs/topics/TRAITS.md#L191-L202)），实测已可用。三处都属「历史记录未回填」，读文档的人会得出错误的能力边界。
- **A5** **「无 dyn 也发静态表」**：TRAITS.md §二/§四之一把表写成「为每个 `(trait,类型)` 发」，代码只在 `usedDyn` 的 trait 上发（D67）——这决定了 `tests/impl` 表判据必须带 dyn 夹具，是判据写法的分叉点。
- **A6** **`[]`/`[]=` 在泛型体内的延迟检查是空白**（D144）：STL.md 自己承认；实测读侧直接报「cannot index a value of type `T`」，而 `#57` 的延迟机制只覆盖具名方法 ⇒ 用泛型容器/泛型键的库代码写不出「对 `T` 下标」这一形状。
- **A7** **零值分类与「句柄型」类型脱节**：`coroutine<T>` 在**局部声明**处有专门诊断（D121），在**聚合/泛型实参**里却按普通 struct 判定（D122），在 **codegen 零值**里又按普通实例逐字段写 designator（D123）——同一个「句柄没有零值」的事实被写了三遍、只有一遍对。
- **A8** **`str` 自相矛盾**：被列为 builtin 类型（诊断里也印出来），却无法在源码里解析（D170）；MIGRATION.md §3 的处置记录看似仍是现状。
- **A9** **生成物依赖 C 方言扩展**：合成名（`$lam$…`、`extc_vt$…`、模块前缀 `lib$fn`）原样进 C；合同是 `-std=c11`（gcc/clang 容忍 `$`），但任何 `-pedantic-errors` 或非 GNU 工具链都会红。文档从未把「生成物需要 GNU 词法扩展」写成合同的一部分。
- **A10** **C 名保留策略不成体系**：`__` 前缀是硬禁令（D32）、prelude 名是 `reserved` 标记（D33）、实例名只有 `TY_GENERIC` 有冲突检查（D35/D38）；`array_N_T`、泛型枚举实例名、`extc_*` 运行期名都靠「用户不会那么起名」+ 链接期/编译期偶发报错。

## 14. 已复现/证据索引（本文实测或审计已复现）

**A. 与代码不符且严重（P0/P1）**

1. `check_expr.c:2404`（另 `:3179`）—— `funcInstance` 返回 NULL 未检查 ⇒ 65 层嵌套类型或 4300 实例打崩编译器（D15）。
2. `check_top.c:5596` —— 延迟泛型调用单遍重放 ⇒ 三链泛型按声明顺序生成 `f_T` 未定义（D10）。
3. `check_escape.c:1473/1495/1526` —— 自由泛型函数的零值/引用/`new` 尺寸检查永不记录 ⇒ `(cellfn){0}` 调空指针（D119）。
4. `check_top.c:228-230` + `check_lookup.c:418` —— builtin/视图目标的 trait 一致性整段跳过（D60）。
5. `check_top.c:285-294` —— 返回类型只在双非 void 时比较（D58）。
6. `check_top.c:267` —— 无接收者方法的第一参数永不比较（D57）。
7. `parser.c:1088` —— trait 的 `effects Ret=0` 无人校验，dyn 调用点却相信它（D56，ASan stack-use-after-scope）。
8. `check_expr.c:3457/3459` —— dyn 实参检查剥 `ref` 且不做上下文定型（D88：一个接受错程序、一个误拒对程序）。
9. `check_top.c:4635-4644` —— `option<T>`/`result<T,E>` 无法 unify（D42）。
10. `check_expr.c:971` —— 泛型函数里捕获 `T` 的 lambda 字段保留未替换类型参数（D109，生成物编不过或静默截断）。
11. `codegen.c:630` —— `opOverloadSuffix` 依赖调用方的替换上下文 ⇒ 泛型方法定义与调用点 C 名不同（D31）。
12. `codegen.c:5790-5799` —— `dropRuntimeDefs` 的 `continue` 跳过推进 ⇒ `stdlib/stl/hashSet.extc` 作为根文件挂死（D146，实测 rc=124）。
13. `pools.c:110-118` —— `extc_dynPoolOf` 只记 pid ⇒ dyn 值活过自己的 place（D82）。
14. `codegen.c:1574` / `check_lookup.c:671` —— `coroutine<T>` 的零值在聚合与 codegen 两侧判错（D122/D123）。
15. `codegen.c:7065` / `types.c:483` —— 只在数据位置命名 dyn 类型时不发 typedef；`array_N_T`/泛型枚举实例名未保留（D35/D96）。

**B. 缺口（文档与代码都承认）**：`T: Trait` 上界 · trait 撞名专门诊断 · 开放注册/动态链接 · `[]`/`[]=` 的泛型延迟 · 容器面的 6 处缺方法 · 泛型矩阵的 6 格 · 范围类型/`for` 形态 · 可写函数类型/`dyn` 闭包 · 文本 pass 偏移失效（TRAITS.md §八整节）。

**C. 提案（文档有、代码无）**：函数重载（D65）· NULL 槽的值语义（D94）· 卸载/重载留门（D98）· 自举第 ②③ 步（D166）· C fallback 开关（D167）· `EXTC_WALK_SELFCHECK`（D155）。

## 15. 结论速览

- **条数**：本节共 **171 条**决策/裁定（D1–D171），另 **10 条**跨主题含糊（A1–A10）、**15 条**与代码不符的高严重度项（§14-A）。
- **最关键 5 条**：
  1. **D1/D2/D3/D14** —— 实例在调用点诞生、按「模板+实参」intern、浅拷贝共享 body；上限 4096/64 是编译器自保。
  2. **D7/D10/D11** —— 延迟检查分四批按实例重放；类型表迭代到不动点，但**调用记录只走一遍**（P0）。
  3. **D54/D56/D58/D60** —— trait 一致性的**实际**比对面比文档小得多（effects/mut/无 self 首参/void/内建与视图目标全漏）。
  4. **D77/D79/D80/D84** —— dyn 的「统一签名表 + thunk + 槽内表指针 + 五段世代校验」是 O5 的结构性保证；立即形式走路线 C 不进池。
  5. **D104/D105/D109** —— lambda 的环境物化规则完整，但**泛型里的 `T` 捕获**是未声明的静默失败。
- **最严重含糊 3 个**：
  1. **A1** 「trait/impl 一致性比什么」有文档口径与代码口径两套，且代码口径有五处洞，互相叠加。
  2. **A2** 「实例化的不动点」在类型表与调用记录上是两种语义，文档把它们混为一谈。
  3. **A3** 延迟检查的记录端与消费端判据互为补集（`owner==NULL`），使自由泛型函数整条检查路径为空 — 这不是边界情况，而是**整类**。
