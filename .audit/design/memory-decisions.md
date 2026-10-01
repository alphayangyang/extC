# 审计后的设计裁定 · 内存/arena/引用/健全性线

> **来源单元**（10 份，逐份读完；均在 `docs/topics/`）：`ARENA.md` · `ARENA-FORMAL.md` · `ARENA-MEMORY.md` ·
> `ARENA-NOTES.md` · `ARENA-SOUNDNESS.md` · `MEMORY-MODEL.md` · `MEMORY-SAFETY.md` · `SOUNDNESS.md` ·
> `REFS.md` · `HARDENING.md`
> **外部对照**（仅用于判"与代码不符"）：`.audit/findings-index.tsv`，以及 `src/` 的定点抽查
> （`raiseMutRefTargets` 已不存在、`addrMask/contMask/homeAddrMask/homeContMask/otherMask` 与
> `funcAllocates`/`depthComesFromAlloc2`/`escapees` 存在；`escapeesFor` 只写不读）。
> **日期口径**：文档时间跨度 2026-09-18 ~ 2026-09-29；同一问题多份文档口径不同时，本文按"最新定案"记，
> 并在"含糊/矛盾"里保留旧口径。
>
> **状态词表**：`已定案`（拍板）· `已定案·已落地`（代码里确认存在）· `已定案·未落地`（口径定了、代码没有）·
> `提案`（未拍板）· `已撤回`（曾决定、后来明确收回）· `设计史`（只作记录、不构成现状）·
> `已知缺口`（文档自认缺/未做）· `与代码不符`（文档口径与代码/审计实证冲突）。
> 每条给：**标题 · 出处 · 一句话陈述 · 状态 · 约束实现的地方**。

---

## 0. 单元文件各自的权威范围（读法）

- **ARENA.md**：设计起点（arena=词法作用域、"最浅实参"规则、owner depth、甲′）。§9.5 的 E2 已**自我撤回**，§10 声明被 ARENA-FORMAL 取代 ⇒ 只对"设计意图/取舍"有权威。
- **ARENA-FORMAL.md**：`at` 侧（分配区域该选哪只）的**当前权威**：两种流、约束系统 C1–C5、定理 2/3、定案 68、§9 调用点求解落地实录。
- **ARENA-SOUNDNESS.md**：`D` 侧（记账深度是不是上界）的**当前权威**：7 个反例、P1/P2/P3、档 0–3 改造清单、@overwrite 修法、路径下沉失败记录。
- **ARENA-NOTES.md**：仍然有效的坑与判据（路径下沉未落地、字段表冻结史、发布记录正解、两把尺子、哨兵）。
- **ARENA-MEMORY.md**：**设计史**，正文自述"§2 说的'已经做进去的两处'要按现在的代码复核"（:4-8）；根因/四坑/判据仍有效。附录 A/B 的约束解算在分支 `lvl-solver`，不在 main（:161-162）。
- **MEMORY-MODEL.md**：派生展示稿（Arena+Pool 原理、时序图），数值与结论取自上述文档，不是独立裁定源。
- **MEMORY-SAFETY.md**：对用户的承诺与 33 条机器可复核判据（`tools/memsafe/run.sh`），含 §6.1 池底容器的**明确免责**。
- **SOUNDNESS.md**：七条义务 O1–O7 的台账；池/dyn 档的缺口与修法。
- **REFS.md**：历史"体检报告"（:361 自述进度表可能滞后）；§4 是逃逸检查最早的规则草案。
- **HARDENING.md**：加固台账（已修/未修/待拍板 + 工作纪律）；与内存线相关的是 H5/H7/H12/K4/K7、③c 线程本地 arena、§0.4 约定。

---

## 1. depth / 层级：`depth(r) ≥ depth(v)` 的精确定义

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-01 | 深度是区域树的保序线性化，越小活得越久 | ARENA-FORMAL.md:42-45；ARENA.md:10 | `d(R) ∈ ℕ` 是区域树 `⊑_tree` 的线性化：`R ⊒ S ⟹ d(R) ≤ d(S)`；反向不成立（兄弟块同深度、互不包含）。 | 已定案（自认近似，代价见 §5.1#1） | 所有深度比较；`storeLayer`/`placeDepth` |
| M-02 | 存储安全判据 = `d(值) ≤ at(地方)` | ARENA-SOUNDNESS.md:67-72；ARENA-FORMAL.md:297-300；SOUNDNESS.md:33,81 | 检查器只问"被指对象活不活得够久"：`d = exprRefDepth(val); d <= at` 放行，否则拒绝；数字小=长寿。 | 已定案 | `check_escape.c:520-541`、`storeLayer`、`Expr.refDepth` |
| M-03 | 记账两条不变量：INV-H 上界性、INV-M 单调性 | ARENA-SOUNDNESS.md:84-92,379-391 | `D ≥ 真实层号`（记小=洞，记大=误拒）；`D` 只许往大改（max，不是覆盖）。 | 已定案；**实现三处写成覆盖** ⇒ 与代码不符 | `refreshRootDepth`、`check_stmt.c:335/398`、`noteFieldDepthWrite` |
| M-04 | `d(v)` 的定义 = 值里所有活指针所指对象的**最大**层号 | ARENA-SOUNDNESS.md:61-63 | `d(v)` 取"最深（数字最大）的那只"；`at(p)` 是 `p` 的存储层（`storeLayer`）。 | 已定案 | `exprRefDepth`、`Sym.refDepth`、字段表 |
| M-05 | **一个数答两个问题** | ARENA-SOUNDNESS.md:117,120,124-128 | `Sym.refDepth` 兼答"引用指着多深"与"聚合里面的引用多深"；`Expr.refDepth` 兼答"值的引用深度"与"分配点层号"。 | 已定案（承认的口径）→ 已知缺口 | `check_escape.c:40,96,114,132` |
| M-06 | 引用类型走 `typeContainsRef` 早退 ⇒ 引用值记 0 | ARENA-SOUNDNESS.md:124-128,167-173,§4.3 | `typeContainsRef` 对 `TY_REF` 自己答 false ⇒ `mut ref i32` 的记账深度是 0（真值=被指对象层号）⇒ 洞。 | **与代码不符**（文档定为档 0 必修 0-a） | `check_escape.c:96`、`check_lookup.c:295` |
| M-07 | 无引用类型（标量/纯值聚合）= 深度 0 | ARENA-FORMAL.md:81；ARENA-MEMORY.md:42-44 | 类型里不可能有引用 ⇒ 早退答 0；值拷贝不背源对象寿命（`valDepthForStore`/`typeCannotCarryRef`）。 | 已定案·已落地 | `exprRefDepth` 早退、`valDepthForStore` |
| M-08 | 参数 = 深度 0 | REFS.md:249,258；ARENA-FORMAL.md:322 (S2) | 形参的被指对象在调用者那边、活得比本函数长；因此"传给 `ref` 形参"在 REFS 时代恒真、不用查。 | 已定案（后被 §9 调用点求解细化） | `placeDepth(参数)=0`、`checkCallRefArgs` |
| M-09 | 全局/静态在虚根、深度 0；全局不能含引用 | ARENA.md:20,49,140；ARENA-SOUNDNESS.md:337 | 入口外声明的东西挂虚根（比谁都长寿），因此"全局不能含 ref"（连声明都拒）。 | 已定案；**例外**：顶层初始化式有 `new`/有家调用 ⇒ 编译器 SIGSEGV（R1，未修） | `checkGlobals`、全局初始化式检查 |
| M-10 | 字段/元素：4 格字段表 + `otherDepth`；路径不敏感取根绑定槽位深度 | ARENA-SOUNDNESS.md:118,164-166；ARENA-FORMAL.md:269 | 逐字段深度在 `Sym.fields[4].depth`，归不到格的进 `otherDepth`；`EX_FIELD/EX_INDEX` 用根绑定的**槽位**深度。 | 已定案（有意的近似）；元素写会清表 ⇒ **与代码不符**（反例 D） | `check_top.c:873-913`、`check_escape.c:140-144` |
| M-11 | 切片/视图按引用处理 | MEMORY-SAFETY.md:77,134,196 | 切片是"视图"，与引用同规则查（返回本帧切片被拒）；泛型按实例复查（`boxT<slice<u8>>::stash` 被拒）。 | 已定案·已落地 | `EX_SLICE` 深度、实例复查 `refChecks` |
| M-12 | `ARENA_HOME = -1` 编码对，但"家 ⇒ 深度 0"错 | ARENA-FORMAL.md:686-688；ARENA-SOUNDNESS.md:501,15 | 家是调用者按本次落点选的（可能是块 arena）⇒ 记 0 不成立；`ARENA_HOME=-1` 这个编码本身是对的（定案 68）。 | **与代码不符**（审计：调用点深度以 0 当 unset，后被更深的 mut ref 实参抬高 ⇒ UAF，`.audit/findings-index.tsv:9`） | `check_stmt.c:216-224`、`check_top.c:614-616`、`promoteInto2:343` |
| M-13 | 层号唯一权威：检查器算完、codegen 只翻译（定案 68） | ARENA-FORMAL.md:668-698；MEMORY-MODEL.md:191-208 | 闭包后检查器再定一次案（`FuncDef.arenaSites`），`arenaRefAt`/`homeArg` 只做文本翻译；删掉 codegen 兜底后 golden 88/95 文件逐字节相同。 | 已定案·已落地 | `FuncDef.arenaSites`、`arenaRefAt`、`homeArg`、`EXTC_DBG_ARENA` 漂移哨兵 |
| M-14 | 每个 `{}` 一只 arena；父子**物理平行**块链；`__extc_a[k]` 词法层 | ARENA.md:9,53-61；MEMORY-MODEL.md:21-36；ARENA-SOUNDNESS.md:46-54 | arena=词法作用域（隐式、匿名）；各自 bump、各自链，子扩容不冲击父；生成物 `__extc_a[maxLv+1]`（函数体 1、每进块 +1）。 | 已定案·已落地 | `blkMaxOfBlock`、`__extc_a`、`extc_arena_release`；`scopes.len` ↔ `blkLevel` 哨兵 |
| M-15 | 「最浅 `mut ref` 实参的 arena」= 语法猜，已被约束求解取代 | ARENA.md:23-33,196；ARENA-FORMAL.md:6,19-21,340-361 | 原 `home(f) = 最浅 mut ref 实参所在 arena`（`callHomeDepth`）只对**地址流** sound；改为 C1–C5 约束求解、调用点按实际流+落点实例化 `H`。 | 已定案（方向变更）；规则④作为 `Addr≠∅` 的约束保留 | `checkCallRefArgs`、调用点求解、`funcAllocates` |

**含糊 / 矛盾 / 缺口（§1）**

- **同一概念两种定义**：`depth` 到底指"槽位多深"还是"被指着的东西多深"——REFS.md:246 写 `depth(r) ≥ depth(v)`（r 是引用、v 被指对象），实现判据写 `d(v) ≤ at(p)`（v 是值、p 是槽位），**字母角色相反**，ARENA-SOUNDNESS.md:74-78 专门立"记法约定"救场；`Sym.refDepth`/`Expr.refDepth` 一个字段答两个问题（M-05）。
- **`0` 至少三义**：①"参数/家那一档、活得最久"（ARENA-FORMAL.md:322）②"站点还没定"的哨兵（ARENA-MEMORY.md:209 明确说那是哨兵态不是真值）③"根不是绑定的地方 ⇒ 活到永远"（审计 `.audit/findings-index.tsv:23`）——三义混用是 P0 洞的直接来源（`:9`）。
- **"深度线性化 sound 吗"文档自相矛盾**：ARENA-FORMAL.md:268 记"未证、欠一个证明、假阳性"，同文 :295-296 又下结论"① 不需要证伪，它是对的"；ARENA-SOUNDNESS.md:364-366 指出这一前后不一，并判定真正破的是 (S3) 上界性。
- **家 arena 三个口径**：`callHomeDepth`（最浅 mut ref 实参）／调用点约束求解（落点⊔Cont）／`ARENA_HOME` 被当深度 0（M-12）；方法接收者缺省时 `arenaArg = &__extc_a[1]`（被调者自己的帧），与"家=调用者选的那只"定义冲突（ARENA-SOUNDNESS.md:503）。
- **层级计数与代码不符**：`d.run {…}` 块不计入 `blkMaxLevel` ⇒ arena 数组少一层、越界写 `__extc_a[2]`（`.audit/findings-index.tsv:14`，另有 :52）。
- **`alloc<T>` 的层号**：文档要求与 `new` 对称（ARENA-SOUNDNESS.md:502），但审计发现 `dfValueDepth` 无调用者、`dataflow.c` 的 depth 参数从不参与比较（`.audit/findings-index.tsv:152-153`），`alloc` 仍不进 `promoteInto`（ARENA-SOUNDNESS.md:692）。

---

## 2. 逃逸集合与效果摘要：如何建立、何时失效、是否要不动点

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-16 | 三种流：`Fresh` / `Addr` / `Cont`（按实参编号） | ARENA-FORMAL.md:168-179 | 对每个函数 f、每个 `mut ref` 参数 i 算：自己分配进 H_f 的（Fresh）、存 `&实参 j` 的（Addr）、存"从实参 j 读出的指针"的（Cont）；约束分别恒真、`R_j ⊒ H_f`、`ρ_j ⊒ H_f`。 | 已定案·已落地 | `collectEffectsExpr`、`check_top.c:1162-1264`、`check_expr.c:1197-1205` |
| M-17 | `Addr` 只在真有地址流时非空 ⇒ 规则④只在 `Addr≠∅` 时施加 | ARENA-FORMAL.md:178-179,279-281,428 | `push` 从不存 `&l`（只存 `l.head`）⇒ `Addr(push,0)=∅` ⇒ 把它钉死在调用者帧上的约束消失；旧规则"无条件施加于所有实参"是 #34 的根因。 | 已定案·已落地（§9.5） | `checkCallRefArgs` 的 addrMaybe/contMaybe |
| M-18 | 摘要不完整 ⇒ 对每个含引用的实参按最坏查 | ARENA-FORMAL.md:603,614-615 | `otherMask` 非空/摘要不完整/递归环 ⇒ 退化成"调用点版 blanket"：对每个含引用实参要求 `exprRefDepth(arg) ≤ h`；"摘要不完整不许放行"是不可分割的一半。 | 已定案（fail-closed 口径） | `check_top.c:1190,1221,1249-1264` |
| M-19 | 摘要的建立 = 查体时收集；跨函数 = 惰性闭包 + memo + 环保护，拿不准保守 | ARENA-FORMAL.md:276-284,483-493；ARENA.md:329-333 | 效果摘要在检查函数体时收集；跨函数依赖按名字惰性闭包（照抄 `funcAllocates`：memo + 环保护 + 拿不准保守）；E 敏感的决策推迟到摘封闭合后（`EArenaSite`）重算。 | 已定案·已落地 | `funcAllocates`、`FuncDef.allocState`、`EArenaSite`、`computeEscapes` |
| M-20 | 逃逸集合 E：`E = 这个局部会不会被搬出本函数`；保守方向=宁可在 E 里 | ARENA.md:267-278 | AST 走一遍+不动点：`return e` 里出现的每个局部进 E、`p=e`（p 根是参数/全局）进 E、`x∈E` 传递、拿不准进 E；E 只决定"家选哪只"，不改变"能不能写"。 | 已定案 | `computeEscapes`、`c.escapees`、`callHomeDepth` |
| M-21 | E 的并集与"跨调用 memo" | ARENA-SOUNDNESS.md:656-669 | 查体时摘要未封闭 ⇒ 记 `EArenaSite`；收尾 pass 用**所有函数 E 的并集**重算 `arenaArg`（`c.escapees` 只是当前函数的）。 | 已定案·已落地；但 `escapeesFor` **只写不读**（`.audit/findings-index.tsv:142`）⇒ 文档暗示的跨调用 memo 不存在：与代码不符 | `check_top.c:5991-5997`、`check_internal.h:431-432` |
| M-22 | 摘要按**模板**一份，不按实例一份 | ARENA-FORMAL.md:646-651 | 模板里 `T` opaque ⇒ `typeContainsRef(T)` 判不出 ⇒ carrier 判据补 `mentionsParam`（含类型参数就当"带引用"）；`varArray<slice<u8>>` 与 `varArray<i32>` 共享一份保守摘要。 | 已知缺口（有意的保守；"按实例算摘要"另立 PLAN） | `collectEffectsExpr`、`typeContainsRef`、`mentionsParam` |
| M-23 | 失效规则 (S3)：任何能改变值内容的规则，必须把写进去的东西的深度**取上界记回** | ARENA-FORMAL.md:356-367；ARENA-SOUNDNESS.md:379-391 | `max` 不是覆盖；新加特性逐个问"这一步会不会让谁的记数变小"；已覆盖赋值/元素字段写/方法调用/出参，未覆盖 `for` 变量、闭包捕获、`match` 载荷绑定、模块级可变状态。 | 已定案（纪律）；实现多处覆盖 ⇒ **与代码不符**（含出参写入不产生 publication，`.audit/findings-index.tsv:36`） | `refreshRootDepth`、`noteFieldDepthWrite`、`noteOrigin`、`promoteInto2` |
| M-24 | 每个写绑定路径都要刷新字段表（P3） | ARENA-SOUNDNESS.md:389-391,§4.1-4.2 | 现在只有"字面量初始化"与"引用型换指向"两处刷新；普通赋值完全不动字段表 ⇒ 反例 A。 | 已知缺口（列为必修 P3）；审计再确认 origin 过期类型的洞（`.audit/findings-index.tsv:1,36`） | `check_stmt.c:229-233,348-357` |
| M-25 | 要不动点：三条不动点 + `D` 必须是单调数据流 | ARENA-FORMAL.md:483-493；ARENA-SOUNDNESS.md:464-473,§9.3 3-a | ①效果摘要不动点（跨函数惰性）②区域变量最小解迭代（Knaster–Tarski）③逃逸/落点闭包；`D` 必须从"单遍+破坏性更新+无 join"改成前向数据流（赋值/合流/循环取 max），否则"迟早有洞"。 | 已定案（诊断）；**仅部分落地**（事实表 replay 落地过，`D` 的数据流未落地） | `levelPass`、`LvlFact`、`dataflow.c`（审计 :152-153 说明这道闸门实际缺失） |
| M-26 | 事实表重放到不动点（约束解算第一版，分支上） | ARENA-MEMORY.md:107-124,159-201 | `promoteInto2` 入口记 `(值,目标层号)` 事实，取最小写进 `Expr.minAt`；收尾重放到不动点（上限 32 轮，实测 1 轮收敛）；`minAt==0`⇒家、`≥1`⇒第 k 层、`-1`⇒回 `lexicalLevel`。 | 设计史/提案——在分支 `lvl-solver`（`e4642d3`+`6d90954`），**不在 main**；坑 3（`generic_return_local` 该拒却过）健全性问题未查清 | `LvlFact`、`Expr.minAt`、`Checker.lvlSolving`、`promoteInto2` |
| M-27 | `minAt` 表达不了"必须活过本帧"；正解 = 沿"发布记录之间的边"传播 | ARENA-NOTES.md:103-159 | `D` 是上界、调小是收紧；C3 要的是"活过本帧"而目的地深度是 2——深度域里没有那个数；唯一表达需求的是发布记录 `at=0` ⇒ 最终把 `EX_IDENT` 的下降完全交给发布记录表（每个绑定走它所有被赋过的表达式），`heldSrc` 降为兜底。 | 已定案·已落地（转正库 22/22、golden 95 逐字节相同） | `StoreSite.target`、发布去重按(值,目的地)、`levelPass`、`Sym.heldSrc`、`originOf` |
| M-28 | 收尾 pass 只在逃逸时才把站点放进家（无条件兜底已删） | ARENA-MEMORY.md:3-8,18-27 | 曾经的 `if (site->kind==EX_NEW \|\| EX_GENCALL) site->arenaLevel = ARENA_HOME`（有家⇒全进家）是 98MB 根因；现行收尾 pass + `levelPass` 只在逃逸时进家。 | 已定案·已落地（专项性能未做进去） | 收尾 pass、`levelPass`、`promoteInto2` |

**含糊 / 矛盾 / 缺口（§2）**

- **"逃逸集合"有三种所指**：ARENA.md §9.4 的 E（局部名集合、AST 不动点）／ARENA-SOUNDNESS §11.2 的 `computeEscapes`+全函数并集／ARENA-FORMAL §3.4 的 `Fresh/Addr/Cont` 摘要——三者相关但**不是同一个集合**，文档从未并表定义。
- **摘要的"何时失效"没有正面规则**：只有 (S3) 的事后纪律与 P3 的"每个写路径都要刷新"；代码里 `fresh` 集合建一次、重定向不失效（`.audit/findings-index.tsv:44`）、`escapeesFor` 只写不读（:142）。
- **"不动点"的层次混用**：ARENA-FORMAL §8.1 三条不动点 vs ARENA-MEMORY 的"事实表 replay"（32 轮上限、实测 1 轮）vs ARENA-NOTES 的 `levelPass` 两段不动点——同一个词指三种不同迭代对象，落地状态分别是"部分落地/分支上/已落地"。
- **文档自称已落地而实际不在 main**：ARENA-MEMORY 附录 A/B 的约束解算（:159-244）标"机制通"，但只在 `lvl-solver` 分支；正文 :18-27 的"无条件兜底已删"才是 main 现状。

---

## 3. 身份：用"名字"还是"符号/来源链"

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-29 | 路径不敏感：只认绑定名，不认 `p.next.field` | ARENA-FORMAL.md:269 | 深度只跟踪标识符；`EX_FIELD/EX_INDEX` 用根绑定槽位深度 ⇒ 用户必须写 `var n = p.next` 才能收窄；根槽位深度常比内容实际寿命短 ⇒ 误拒。 | 已定案（有意的近似，PLAN #14）；档 3 提案 | `placeDepth`、`exprRefDepth` 的 FIELD/INDEX 支 |
| M-30 | 名字只出现在祖先链上（引理 L1）⇒ 同链深度比较精确 | ARENA-FORMAL.md:302-313 | `lookup` 只沿当前作用域链向上找 ⇒ 命名地方的根绑定必是当前块的祖先；同一链上 `d ≤ d` 就是真实寿命序；"兄弟块不可比"不会出现在"命名地方 vs 命名地方"的比较里。 | 已定案（定理 6.2 的前提） | `check_lookup.c`、`placeDepth` |
| M-31 | 身份是符号（Sym）+ 来路（origin），不是名字 | ARENA-NOTES.md:47,140-159；ARENA-SOUNDNESS.md:119,309-323 | 提升走路沿 `Sym.origin`（只记声明时的初始化式）+ 字段表；`originOf` 压平会指到"最后一次赋值"，改用不压平的 `heldSrc`；最终权威是"值→目的地"的发布记录表。 | 已定案（方向）；实现有缺口（origin 不因字段写刷新 ⇒ 反例 G；审计确认同族洞） | `noteOrigin`、`Sym.origin`、`heldSrc`、`originOf`、发布记录表 |
| M-32 | 合流点"绑定可能装 A 也可能装 B"⇒ 单一"当前值"字段必然错 | ARENA-NOTES.md:140-159 | `Sym` 只有一个 `heldSrc`，而 `if` 汇合有两个可能 ⇒ 任何"当前值"字段只能描述源码顺序最后一次赋值 ⇒ 必须走遍所有发布记录。 | 已定案·已落地 | `levelPass` 两段、`EX_BIN` 分支、`valueLevel`/`levelOfValue` |
| M-33 | 参数身份按**名字**比 ⇒ 遮蔽时约束挂到错误实参 | `.audit/findings-index.tsv:24`（`src/check_escape.c:726`） | `valTracesToParam`/`destIsParam` 比较参数**名字**；局部遮蔽同名参数时，存储的寿命约束被发布到错误的实参上。 | **与代码不符**（审计） | `valTracesToParam`、`destIsParam`、`paramIndex` |
| M-34 | 没有别名分析（主人已拍板不做）；取地址跟踪是最小可用版本 | MEMORY-SAFETY.md:263-264；ARENA-FORMAL.md:433-458 | "a 和 b 指向同一个东西"编译器不管 ⇒ 少数形状误拒；强更新的硬门槛不是区域粒度而是别名信息，最小版本=每个绑定一个 bool"取过地址⇒禁止强更新"（`Sym.addressed`）。 | 已定案（不做别名分析）；取地址跟踪=提案（档 2） | `Sym.addressed`、`check_expr.c:580`、强更新判据 |

**含糊 / 矛盾 / 缺口（§3）**

- **"按名字"与"按符号"并存**：ARENA-FORMAL §6.2 的正确性论证**依赖**名字解析沿作用域链（L1），而审计证明参数匹配用名字会在遮蔽时出错（M-33）——同一"名字"机制，一处是承重墙、一处是洞。
- **`origin` 的语义边界不清**：文档说它"只记声明时的初始化式"（ARENA-NOTES.md:47），又要求它承担整个提升走路的来路；字段写不刷新 origin 直接产生反例 G（ARENA-SOUNDNESS.md:309-323）。
- **字段路径 vs 根槽位深度**：文档同时承认"路径不敏感是误差源"和"根槽位深度更短命 ⇒ 误拒"，但没有给出"什么时候该用哪一半"的判据。

---

## 4. 参数身份与调用点检查（哪个实参对应哪个位）

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-35 | 规则④：每个 `ref`/`mut ref` 实参 i 都要 `R_slot(arg_i) ⊒ H` | ARENA-FORMAL.md:100-104,601 | `checkCallRefArgs` 对每个 ref/mut ref 实参要求 `placeDepth(arg_i) ≤ h`；`h` = 本次调用里最浅的 `mut ref` 实参所在层（同一个数给所有位用）。 | 已定案（仅在 `Addr≠∅` 时施加，M-17） | `checkCallRefArgs`、`placeDepth` |
| M-36 | 方法接收者必须**前置**成 arg 0 | ARENA-FORMAL.md:641-643 | `EX_METHOD` 的 `params` 有 `self`、`args` 没有 ⇒ `params->len != args->len` 早退 ⇒ 一整类形状（`list.push(...)` 的 self）**静默没查**；修法=接收者前置进实参表（arg 0）。 | 已定案·已落地（文档称"这类静默跳过是最危险的一类"） | `EX_METHOD` 调用点、`checkCallRefArgs` |
| M-37 | 调用点检查必须跑在实参类型检查**之后** | ARENA-FORMAL.md:644-646 | 自由函数/关联函数的检查曾跑在实参检查前（`a->type == NULL` ⇒ `typeContainsRef` 恒假 ⇒ 静默跳过）；修法=三处调用点全部挪到实参检查之后。 | 已定案·已落地 | `checkCallRefArgs` 三处调用点 |
| M-38 | 被调者侧只对"来路能追到参数"的值放行 | ARENA-FORMAL.md:607-615 | 放行条件 `paramIndex(f, placeRootName(v)) >= 0`；追不到（调用结果/本帧局部/其它）⇒ 照旧拒（即 `otherMask` 记的东西，两边口径一致）。 | 已定案·已落地 | `paramIndex`、`placeRootName`、`otherMask` |
| M-39 | 摘要掩码 32 位 ⇒ >32 参数时 UB，index ≥ 32 永不检查 | `.audit/findings-index.tsv:43`（`src/check_top.c:2476`） | `1u << j` 在 j ≥ 32 时是 UB；第 32 个及以后的实参完全不进检查。 | **与代码不符**（审计） | `addrMask/contMask/homeAddrMask/homeContMask/otherMask` |
| M-40 | `fresh` 局部集合建一次、不因改指向失效 ⇒ 摘要为空、调用点规则被跳过 | `.audit/findings-index.tsv:44`（`src/check_top.c:2497`） | fresh 集合只由声明构建，retargeting 不使其失效 ⇒ 被调者效果摘要漏项。 | **与代码不符**（审计） | `collectEffects` 的 fresh 集 |
| M-41 | 调用点 `h` 与 E 联动的坑：`homeDepth = -1 ⇒ h = 0` 对本地 mut ref 实参过严 | ARENA-FORMAL.md:636-640 | 落地教训：两个检查各自决定跳不跳、别共用一个 early return（拆 `addrMaybe`/`contMaybe`），否则 `list-return` 被过严组合误拒。 | 已定案（教训进代码注释） | `addrMaybe`、`contMaybe`、`homeDepth` |
| M-42 | 泛型实参的 `Cont` 位只能保守（`T` opaque） | ARENA-FORMAL.md:646-651 | `typeContainsRef(T)` 判不出 ⇒ 含类型参数就当"带引用"；模板摘要=每次模板一份。 | 已知缺口（保守，误拒面更大） | `mentionsParam`、`collectEffectsExpr` |

**含糊 / 矛盾 / 缺口（§4）**

- **"哪个实参对应哪个位"其实有三种口径**：①按参数**名字**匹配（M-33 的实现在的做法，遮蔽即错）②按**位置**编号 `Addr(f,i)`（ARENA-FORMAL §3.4）③按**根名回溯** `paramIndex(f, placeRootName(v))`（§9.3）——文档没有说明三者何时一致。
- **掩码宽度与参数个数脱钩**：摘要是按位掩码，而语言不限制参数个数（审计 :43）；文档从未写"参数上限 32"这条隐含约束。
- **接收者作为 arg 0** 是在落地记录里补的（ARENA-FORMAL.md:641-643），**约束系统 §3.4 的定义里没有"self 是第 0 个实参"这句话**——形式定义与实现口径之间存在缺口。

---

## 5. 未知 / 无法判定时的保守方向（fail-open 还是 fail-closed）

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-43 | 总口径 = **fail-closed**：宁拒不放；误拒不是 soundness 逼出来的 | ARENA-FORMAL.md:387-399；ARENA-SOUNDNESS.md:446-448 | 命题 7.1：把分配区域换更长命**永远安全** ⇒ 任何被拒的安全程序都有重写（极端=全放程序级区域）；精确性只买内存，不买安全。 | 已定案（并被机械化成 `arenaLevel = min(当前块层, 所有值流向的目的地层)`，ARENA-FORMAL.md:406-420） | 提升规则、`arenaLevel` |
| M-44 | 拿不准的四条落点：进 E / 保守闭包 / 摘要不完整按最坏 / 提不动报错 | ARENA.md:270-277；ARENA-FORMAL.md:283,603；ARENA-SOUNDNESS.md:335,411 | ①E 分析"拿不准进 E"②跨函数"memo+环保护+拿不准保守"③摘要不完整 ⇒ 对每个含引用实参按最坏④`promoteInto` 提不动 ⇒ `ckError`（文档逐个走过：**没有"提不动却静默放行"的路径**）。 | 已定案（口径）；实现有 fail-open 破口（M-45） | `computeEscapes`、`funcAllocates`、`checkCallRefArgs`、`promoteInto2` 兜底 |
| M-45 | 实现里的 fail-open 破口（口径 fail-closed、代码不是） | `.audit/findings-index.tsv:9,23,25,44` | ①调用点深度以 0 当 "unset"（后被更深的 mut ref 实参抬高）②`placeDepth` 对"根不是绑定"的地方答 0=活到永远 ③`needsHome` 早退把"有家函数的借用往全局存"放行 ④fresh 集不失效。 | **与代码不符** | `check_top.c:6024`、`check_escape.c:147`、`check_escape.c:1447`、`check_top.c:2497` |
| M-46 | 分析超限要**响亮报错**，不许静默保守 | ARENA-FORMAL.md:519,563 | 不动点×实例数退化时给上限并报"分析超限"，跟"不许静默保守"同一条规矩。 | 已定案（纪律） | 求解器上限与诊断 |
| M-47 | 运行时失败一律带位置 trap；trap ≠ UB | SOUNDNESS.md:15-18,86,101 | "程序要么全程有定义，要么在失败那一刻 trap"；定理 = 检查器接受的程序每步要么保持 WF、要么 trap 转移。 | 已定案 | `extc_trap`/`extc_trapMsg`/`extc_die` |
| M-48 | 误拒与洞共用同一个出口 ⇒ 报错要能区分"你不安全"与"我算错了" | ARENA-SOUNDNESS.md:535-541,693；ARENA-SOUNDNESS.md:644-654 | 给每条记录的深度标来源（`exact`/`upper`/`unknown`），报错分两种话；`alloc` 修好之前"挡住的是真悬垂还是我算错了"分不清。 | 已知缺口（§11.4 仍欠） | `ckError` + `RefCheck` 记录的来源位 |

**含糊 / 矛盾 / 缺口（§5）**

- **三处 fail-open 与文档总口径直接冲突**（M-45）；文档在 ARENA-SOUNDNESS 里把这类叫"记账被覆盖/记小了"，但从未把"0 当哨兵"列为独立机制风险（直到审计）。
- **"保守"与"静默保守"被当成两件事**：ARENA-FORMAL.md:519 要求超限"响亮报错、不许静默保守"，而实现中大量静默跳过（M-36/M-37 已修，M-39/M-40/M-45 还在）——文档没有规定"跳过即错误"的兜底。
- **"拿不准"的判定没有统一入口**：`otherMask`、`!effComplete`、环保护、`homeDepth=-1` 各自按不同默认值走，ARENA-FORMAL §9.2 只给了三行示意表。

---

## 6. arena / zone / place 的所有权与释放（含协程、池、域）

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-49 | arena 回收 = 出作用域整条链还掉；保留 ≤1MB spare；不逐个 free | MEMORY-MODEL.md:38-43；ARENA-FORMAL.md:271 | `extc_arena_release` 走整链、按 `EXTC_ARENA_SPARE_MAX`(1MB) 留一块 spare，其余 `free`；`clear()` 不还内存、没有 `shrink_to_fit`；循环体那只每轮 reset；`tests/arena` 150MB 上限跑 300 轮×1MB。 | 已定案·已落地 | `extc_arena_release`、`EXTC_ARENA_SPARE_MAX` |
| M-50 | 函数任何出口放掉 1..maxLv 全部层；`break/continue` 下沉到循环层 | ARENA-SOUNDNESS.md:51-54 | 函数出口统一 release 全层（`__extc_ret:`）；进块时防御性清一次。 | 已定案·已落地 | `codegen.c:1745/1748/1917-1923/2182-2191` |
| M-51 | 家 arena 是隐藏形参 `__extc_home`，调用点实例化 | ARENA-SOUNDNESS.md:55-57；MEMORY-MODEL.md:210-228 | `needsHome` 的函数多收 `extc_arena *__extc_home`；`Expr.arenaArg`：`ARENA_HOME ⇒ __extc_home`，`k≥1 ⇒ &__extc_a[k]`；同一个 `push` 在不同调用点拿到不同的家。 | 已定案·已落地 | `needsHome`、`Expr.arenaArg`、`codegen.c:1521-1526,2012-2023` |
| M-52 | `new` 的四种形状与零初始化；`alloc<T>` 同为内建原语 | ARENA.md:170-184；MEMORY-SAFETY.md:32-38 | `new T`（清零）/`new T[n]`（运行时长度 ⇒ `mut slice<T>`）/`new [N]T`/`new T{…}`；`alloc<T>(n)` 同规则；`allocSlice<T>` 已删（定案 81）。 | 已定案 | `EX_NEW`/`EX_GENCALL`、`extc_arena_alloc`、`arenaAllocZero` |
| M-53 | arena 不是数据类型：匿名、进不了泛型参数、无手动 `free`；一批 API 作废 | ARENA.md:73-75,212-219 | `arenaOf()`/`allocFrom`/`varArray.home: ref arena`/手动 `free` 全部作废；显式指定仍是逃生舱（语法未定）。 | 已定案（语法未定：ARENA.md:111） | 无这些 API |
| M-54 | 池：独立板块、寿命交给 place/arena 管、可真收缩；句柄=(slot,gen) | MEMORY-MODEL.md:297-335；SOUNDNESS.md:79 | zone（地方头）→ pool 树 → pool；池持有自己的板块（不借 arena 切片）；`shrink`/`release` 立刻还内存；两层失效：池级 `epoch` 对 `zones[zone].color`、槽级 `handle.gen` 对 `slot.gen`。 | 已定案·已落地 | `extc_pool_*`、zone color、`extc_pool_new_at` |
| M-55 | 池的三条不变量：O3 失效换代、O4 只追加/墓碑、O5 派发键决定布局 | SOUNDNESS.md:34-40,60-71 | `reset` 必须 `generation++`；句柄寻址的存储只追加（对象表）或删除用墓碑；派发键（槽里的表指针+世代校验）决定布局。 | O3 已修；O4 只追加这半已落地、墓碑随阶段 3；O5 dyn 侧成立、**开放注册（第三期）未做** | `extc_pool_reset`、`extc_pool_new_table/kind`、`extc_dyn_slot` |
| M-56 | 块不进池、池不放块；两层只共享寿命所有者 | MEMORY-MODEL.md:309-319 | 进池的必须"等大+需要身份/复用"；变长无身份的块走链；块分配器在生成 C 运行期、池库在 extC 层，结构上接不通（拿不到 `void*`）。 | 已定案 | 池元素形状约束 |
| M-57 | 池底容器（stl）今天**不在**"不会悬垂"的承诺内 | MEMORY-SAFETY.md:219-259 | `vector`/`string`/`pool`/`map`/`hashMap`/`linMap`/`linSet` 的存储住池板块 ⇒ 逃逸分析无话语权；`return` 那一档仍 ASan 报错（`at==0 ⇒ ZONE_HOME`）；`let b = a` 是"板块共享、标量各一份"的半别名，比深拷贝和纯别名都危险。 | **已知缺口**（最明确的健全性缺口之一）；拷贝语义**待作者拍板** | 容器构造=逃逸站点、`extc_pool_new_at(parent, __extc_home_zone)`、`pidGen` 守卫、`clone()` |
| M-58 | 协程：任务 arena；`resume` 必须把帧自己的 arena 当 home 传下去 | MEMORY-MODEL.md:369-418,498-550 | 帧是普通 extC 值；**协程里 `new` 活到协程结束，不是活到这次 resume**；临时数据在调度器一侧分配；驱动前 `extc_task_alive(id)`，过期句柄带位置大声 trap（退出码 70）；静态守卫禁止 `$step` 出现 `extc_zoneTop`（只能读帧里记的 zone）。 | 已定案·已落地（`tests/coro` 27/27；`coroutine<A,B>` 未做） | `extc_taskArena`/`extc_taskZone`、`extc_task_alive`、`check_concurrency_guards.py` |
| M-59 | 跨 `yield` 的引用被拒（引用不跨 suspend 是设计限制） | MEMORY-MODEL.md:538-541；HARDENING.md:1211-1215 | 跨 yield 活着的局部不可以指向别人的存储（诊断 "lives across a `yield` and carries a reference"）；修法：体内 `new`（落进帧 arena）或只保偏移、resume 后再取视图。 | 已定案（K7 从"题错"改判为设计限制） | 协程帧布局检查 |
| M-60 | 并行 worker：`new` 走线程本地 arena；home 尾参由 trampoline 补传 | HARDENING.md:105-124 | worker 体内不许 `new`（③b 保守禁）→ ③c 放开为 `extc_tls_arena`；每个 worker 链上函数带 `parTlsArena`，有分配就多一个隐藏 `home` 尾参，trampoline 补传同一个线程本地 arena；安全性来自签名（worker 只能写 `out`、只能返回 `i64`）。 | 已定案·已落地（par 组 11/11）；审计：worker 链函数被普通调用时解引用 NULL 的 `extc_tls_arena`（`.audit/findings-index.tsv:28`） | `parTlsArena`、`arenaRefAt`、trampoline |
| M-61 | `@overwrite`：格子与它管的存储必须同生共死 | ARENA-SOUNDNESS.md:545-604 | 格子永远住本帧（`owLocal` 恒 true）、删掉被调者隐藏格子参数；格子带 `home`（有家⇒`__extc_home`，否则⇒`&__extc_a[1]`，永不 NULL）；分配点用 `(*__owc->home)`；只要有格子就必须吐 arena 数组。 | 已定案·已落地（3M 轮 ASan 干净）；代价：跨调用复用没了、每次调用分配一块 | `owLocal`、`extc_owcell.home`、`noArena` 判据、`__owc%d`/`__owp%d` |
| M-62 | 任务 zone 的回收必须与"任务是否真的结束"一致 | `.audit/findings-index.tsv:16`（`src/coroutine.c:84`）；`:55`（codegen.c:4521） | 文档口径：任务结束一次性 `extc_arena_release(&extc_taskArena[id])` + `zoneLeaveTo`；审计：按 LIFO 弹"任务的地方"会把仍存活的任务 zone 一起弹掉 ⇒ UAF；装箱泛型协程永不结束任务 ⇒ 泄漏且死句柄 trap 永不触发。 | **与代码不符**（审计；文档只写了理想路径） | `extc_task_end`、`zoneLeaveTo`、协程 zone 栈 |
| M-63 | `extern!`/`!`/`subView` 是 TCB：签字后信任，未签字退回最保守 | SOUNDNESS.md:42,102,107；HARDENING.md:1354-1364 | `extern!` + `effects Addr=0 Cont=0` 是"错了算我的"；未签字不许收本帧地址、不许跨边界传 `slice`/`struct`；`string.sub` 改拷贝、零拷贝改名 `subView` 并要求签字。 | 已定案（按定义不可证）；审计：trait 的 `effects Ret=0` 在 dyn 调用点被相信而不校验 impl（`.audit/findings-index.tsv:78`） | `parser.c:318/382`、`check_top.c:1082/1124/1140` |

**含糊 / 矛盾 / 缺口（§6）**

- **"域（zone）"的所有权文档有两个版本**：MEMORY-MODEL §6.5 的协程时序图（任务结束一次性 release + zoneLeaveTo）与审计实测的 LIFO 弹栈行为冲突（M-62）；MEMORY-SAFETY §6.1 又给出第三条路线（容器构造成为逃逸站点、池在调用者选的地方出生）。
- **池底容器的承诺边界**：MEMORY-SAFETY §1/§6 的"不会悬垂"与 §6.1 的免责**同文并列**，读者容易只看前者；且 33 条机器判据里一条池/容器都没有（§6.1 自认"机器复核的空白正好在这里"）。
- **拷贝语义未决**：MEMORY-SAFETY.md:256-259 列出"真 handle"与"不可拷贝"两条出路并写"**待作者拍板**"——这是内存线里唯一的显式未决拍板项。
- **`@overwrite` 的"跨调用复用"**被文档明确判为"不可能 sound"（ARENA-SOUNDNESS.md:595-604），但 §10.5 又把"要不要作为一等需求支持"留成待拍板。

---

## 7. 越界 / 除零 / 移位 / 收窄 / 空视图的 trap 契约

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-64 | 七条义务 O1–O7 + 编译器正确性（O2 是内存线的总纲） | SOUNDNESS.md:30-47 | O1 每个产生引用处有活性理由；**O2 借用记账必须是真实寿命的上界**；O3 失效事件在检查粒度上改版本；O4 句柄存储只追加/墓碑；O5 派发键决定布局；O6 动态失败全检查并映射 trap；O7 可信断言。 | 文档先记"4 条成立、3 条缺"（:4），后更新为"7 条全 + 编译器正确性持续义务"（:45-47）；O2 的 7 个反例仍在案 | 落点表 SOUNDNESS.md:32-42 |
| M-65 | O6 的 trap 集合：越界 · `copyInto` 计数 · 除零 · 溢出除法 · 浮点→整数 · 池 OOM · `step` 返回 `none` | SOUNDNESS.md:41；MEMORY-SAFETY.md:199-208,216 | 这些动态失败必须被检查并映射到带 `.extc` 位置的 trap；无限递归 `EXTC_REC_LIMIT`(10 万层) trap；arena 耗尽 `extc: out of arena memory` + 位置 + `exit(1)`；切片能编译期证明的越界编译期报错、其余运行时 trap。 | 已定案·已落地（实测越界 trap、f2i trap）；**移位与收窄不在契约里** | `codegen.c:609/633/5379/5514/5348/5356/5169` |
| M-66 | 空视图存储：索引前必须查 `!v.data` ⇒ trap "the view has no storage" | HARDENING.md:599-640 | 视图索引原来只查下标范围，`data==NULL && i==0` 时直写 null；修法=先查存储指针（复用 `extc_trapMsg`，不新增原语）；判据 `tests/traps/view_no_storage.extc`。 | 已定案·已落地（258/413 份语料变化、0 份非法 C） | `genViewIndexer`、`codegen.c:653`、`extc_trapMsg` |
| M-67 | `main` 掉出末尾 = `return 0`（不是 trap）；其它非 void 函数掉出末尾仍 trap | HARDENING.md:1219-1236 | C 约定 main 掉出末尾等价 `return 0` ⇒ codegen 对 main 发 `return 0;`（H12）；别的非 void 函数仍旧 trap（值真未定义）。 | 已定案·已落地（13 份基准重设） | `codegen.c:4601` |
| M-68 | 编译期能证的越界/类型错就编译期拒（数组与标量比较 ⇒ 描述符越界读） | HARDENING.md:666-692 | 数组只能和同一数组类型比（再判元素）；把判定交给 `typeSupportsOp` 这个唯一权威；删掉 `runOpCheck` 的提前返回。 | 已定案·已落地（零输出变化、无新误拒） | `check_escape.c` 数组规则、`runOpCheck`、`typeSupportsOp` |
| M-69 | 陈旧句柄必须 trap 且方法体未执行（dyn / 协程） | MEMORY-MODEL.md:552-597；SOUNDNESS.md:36 | `extc_dyn_slot` 六段校验（索引/槽占用/池对上/槽级 gen/池 kind/池级 generation）⇒ 陈旧 dyn 值不可能悄悄指向作废载荷；协程死句柄同理（退出码 70 + 位置）。 | 已定案·已落地（`tests/dyn` 判据） | `extc_dyn_slot`、`extc_pool_generation`、`pStale` |
| M-70 | 常量折叠溢出 ⇒ **报错**（H17）；生成的 C 里常量运算溢出**待拍板** | HARDENING.md:19-41（H17）；HARDENING.md:42-64（待拍板） | H17：大整数字面量的常量折叠溢出改 `ckError`（先判后算、带溢出探测）；§0.1.4 另记"字面量默认推 `i32`，两个大字的常量运算在生成的 C 里溢出"⇒ 报错/包一层整数运算，**待主人拍板**。 | H17 已定案·已落地；§0.1.4 **未决（待拍板）** | 常量折叠、（拟）生成式 |

**含糊 / 矛盾 / 缺口（§7）**

- **移位没有 trap 契约**：O6 的枚举里没有移位；审计实测 `<<`/`>>` 在 `i8/i16/u8/u16` 上按 C `int` 宽度算、从不截断（`.audit/findings-index.tsv:48`）——"收窄"在这里是**静默 MISCOMPILE**，既不是 trap 也不是编译错误。
- **收窄（narrowing）没有独立条目**：文档只把"浮点→整数"（f2i）写进 O6；f2i 恰为 2^63 时不 trap（`.audit/findings-index.tsv:66,103`），说明契约的边界值没定义。
- **null 解引用只在"空视图"这一处有 trap 条文**：`p[i]` 对 `?ref [N]T` 的解引用从不拒绝、也不 trap（`.audit/findings-index.tsv:3`）；`ref` 不可为空的承诺（MEMORY-SAFETY.md:95）在索引/切片路径上没有对应检查。
- **arena 尺寸上溢**：`extc_arena_alloc` 对 `n > INT64_MAX-7` 的取整溢出 ⇒ 返回大切片、小后备块（`.audit/findings-index.tsv:67`）；文档的 O6 只提"arena 耗尽"，没提"尺寸计算本身溢出"。

---

## 8. 决策纪律与验收判据（内存线专用）

| ID | 标题 | 出处 | 一句话陈述 | 状态 | 约束实现的地方 |
|---|---|---|---|---|---|
| M-71 | 两把尺子一起看 + 哨兵不许动 | ARENA-NOTES.md:175-186 | 每次同时跑 `tests/run.sh`（255/0）与 `tests/arena-promoted/run.sh`（22/22+ASan）；`generic_return_local` 必须仍被拒、`H1_strongupdate_missed` 必须仍 REJECT；`EXTC_SELFCHECK=1` 自检"层号与深度同数"（`ARENA_HOME ⇒ 0`）。 | 已定案 | `tests/*`、`tools/arena-fix/check.sh`、`EXTC_SELFCHECK` |
| M-72 | 攻击库的"挡住"必须同时是"这条程序真的不安全" | ARENA-FORMAL.md:627-634 | BASELINE 多一条 `h8_launder`：安全程序被误拒却记成战功（**假阳性被记成战功**）⇒ 纪律升级：同层合法 + 更深块仍拒（新增 `stash_view_from_deeper`）。 | 已定案（纪律升级） | `tests/attacks/BASELINE`、`tests/errors/stash_view_from_deeper.extc` |
| M-73 | "255/0 不是证据"：修误拒必须配对抗用例 | ARENA-SOUNDNESS.md:671-686 | 强更新修复让 255/0 全绿，对抗用例当场抓出 `stack-use-after-scope`（两条码路只接了一条）⇒ 回退并固化成哨兵；既有判据全绿 ≠ 没造新洞。 | 已定案 | `tests/arena-soundness/H1_strongupdate_missed.extc` |
| M-74 | 验收双向：该放的要放、该挡的要挡；golden 逐字节 | ARENA-FORMAL.md:284-288,524；ARENA-MEMORY.md:93-103 | 3 条 canary 转正例 + 攻击库基线一字不动，同时满足才算"推导落地"；改动要么 golden 逐字节不变（纯检查器），要么差异逐条说得清。 | 已定案 | `tools/golden.sh`、`tests/arena-soundness`、`tests/attacks`、`tools/arena-fix/check.sh` |
| M-75 | 不放松逃逸/级别分析；健全性修复自主落地；改了没效果一律回退并记档 | HARDENING.md:349-358,638-640 | golden 变化只有"由修 bug 引起"或"全局改名"两种允许来源；提交里的数字只写实测；健全性修复（UB/崩溃/生成物非法/挂死）自主落地并重设基准；试错失败要记档。 | 已定案（工作纪律） | `tools/golden.sh`、HARDENING 台账 |

**含糊 / 矛盾 / 缺口（§8）**

- **同一份文档里的数字互相冲突**：`tests/run.sh` 在 ARENA-MEMORY（256/0、255→256）、ARENA-SOUNDNESS（257/0→254/1→255/0）、ARENA-NOTES（255/0）之间反复变化，没有一处给出"当前应为多少"的权威表；`.audit` 也未收录运行时基线。
- **"纯检查器改动"的判据在不同轮次标准不同**：有时要求 golden 逐字节不变，有时允许"只多那两个新例子"（ARENA-FORMAL.md:663-664），判定标准写在正文而不是工具里。

---

## 9. 三分类总表：已实现 / 文档自认缺口 / 尚未决定

**A. 文档称"已实现"（且代码/判据可复核）**
M-07 值拷贝不背寿命 · M-11 视图按引用 · M-13 层号唯一权威（定案 68）· M-14 每 `{}` 一 arena + 平行块链 ·
M-16/17 三流摘要与 `Addr≠∅` 才用规则④ · M-19 惰性闭包/`EArenaSite` · M-21 摘封闭合后重算 `arenaArg` ·
M-27 发布记录表 · M-28 收尾 pass 只在逃逸时进家 · M-32 合流走发布记录 · M-36 接收者 arg 0 ·
M-37 检查挪到实参检查后 · M-38 被调者侧按 `paramIndex` 放行 · M-49/50 回收与全层释放 · M-51 `__extc_home` ·
M-52 `new` 四形状 · M-54/55/56 池的板块/世代/只追加 · M-58 任务 arena 与死句柄 trap · M-60 worker 线程本地 arena ·
M-61 `@overwrite` 同生共死 · M-65 O6 的既有 trap · M-66 空视图 trap · M-67 main 掉出末尾 · M-68 数组比较收紧 ·
M-69 dyn 六段校验 · M-70 H17 常量折叠报错 · M-71~M-75 纪律与判据。

**B. 文档自认的缺口 / 未落地**
M-03 INV-M 被覆盖（档 0 必修）· M-06 `TY_REF` 早退答 0 · M-12 `ARENA_HOME` 当深度 0 · M-10 元素写清表·
M-22 摘要按模板不按实例 · M-24 字段表写点不全（P3）· M-25 `D` 未换成数据流 · M-26 约束解算只在 `lvl-solver` 分支 ·
M-29 路径不敏感（PLAN #14）· M-42 泛型 `Cont` 保守 · M-48 报错分不清"不安全/算错" · M-57 池底容器（`return` 档仍 UAF）·
M-62 任务 zone 回收（审计实测与文档冲突）· M-65 移位/收窄无契约 · 以及 ARENA-SOUNDNESS §11.4 四条欠账
（强更新误拒、`alloc` 不进 `promoteInto`、深度来源标注、泛型实例摘要精度）。

**C. 尚未决定 / 待拍板**
1. 容器拷贝语义（真 handle vs 不可拷贝 + `clone()`）——MEMORY-SAFETY.md:256-259 明写"待作者拍板"。
2. `@overwrite` 的"跨调用复用"要不要作为一等需求（ARENA-SOUNDNESS.md:615-624）。
3. 生成的 C 里大整数**常量运算**溢出：报错还是包整数运算（HARDENING.md:42-64）。
4. 显式指定 arena 的**语法**（ARENA.md:111；ARENA.md:71 已定"允许"、只缺拼写）。
5. 路径敏感/下沉（ARENA-NOTES.md:33 停下、ARENA-SOUNDNESS.md:698-803 未落地；"声明序"改动小而局部但未做）。
6. `coroutine<A,B>` 的第二类型参数与 `parallel`+channel（MEMORY-MODEL.md:415-418）。
7. O5 第三期"开放注册"（可注册表、ABI 版本握手、卸载墓碑）——SOUNDNESS.md:39 记为独立义务。

---

## 10. 矛盾与含糊处汇总（按严重度）

**S1 · `0` 的多义与 fail-closed 口径破裂（最严重）**
文档总口径是"宁拒不放"（M-43/M-44），但 `0` 同时表示"参数/家那一档""站点未定哨兵""根不是绑定 ⇒ 活到永远"三种意思（§1 含糊）。审计三处 P0/P1 直接由此产生：调用点深度以 0 当 unset 后被更深实参抬高（`.audit/findings-index.tsv:9`）、`placeDepth` 对非绑定根答 0（:23）、`needsHome` 早退放行（:25）。

**S2 · `ARENA_HOME` 的语义（"家=调用者选的那只" vs "家⇒深度 0" vs "家=被调者自己的帧"）**
ARENA-FORMAL.md:686-688 定编码、ARENA-SOUNDNESS.md:501 判"深度 0 这个假设错"、:503 记方法接收者缺省把家选成被调者帧。三者至今并存，且实现按第三种跑。

**S3 · `depth` 到底答哪个问题（槽位 / 被指对象 / 分配点层号）**
`Sym.refDepth` 与 `Expr.refDepth` 各自"一个数答两个问题"（M-05），配合 `typeContainsRef(TY_REF)=false` 的早退（M-06），使"引用值的深度"在实现里系统性偏小；ARENA-SOUNDNESS §4 的三个反例都是这条的实例。

**S4 · "深度线性化是否 sound"文档自我矛盾**
ARENA-FORMAL.md:268 说"欠一个证明"，:295-296 说"它是对的"；ARENA-SOUNDNESS.md:364-366 明确指出这处不一致，并给出一致结论（线性化在单链上精确，破的是上界性）。

**S5 · "逃逸/摘要"三个不同定义与"失效规则"缺失**
E（ARENA.md §9.4）／`computeEscapes`+并集（ARENA-SOUNDNESS §11.2）／`Fresh/Addr/Cont`（ARENA-FORMAL §3.4）从未并表；摘要何时失效只有事后纪律 (S3)/P3，代码里 `fresh` 不失效（:44）、`escapeesFor` 只写不读（:142）。

**S6 · `depth` 比较的字母角色相反**
REFS.md:246 `depth(r) ≥ depth(v)`（r 引用、v 被指对象）与实现 `d(v) ≤ at(p)`（v 值、p 槽位）读起来容易搞反方向；ARENA-SOUNDNESS.md:74-78 的"记法约定"是唯一防呆。

**S7 · 层级计数与释放路径的代码偏差**
`d.run{}` 不计入 `blkMaxLevel` ⇒ `__extc_a` 少一层越界写（`.audit/findings-index.tsv:14,52`）；任务 zone 按 LIFO 弹掉仍存活的 zone（:16）——都与文档"每进一层块 +1、零漂移"的表述冲突。

**S8 · trap 契约的空白面**
移位与收窄不进 O6；f2i 边界 2^63 不 trap；arena 尺寸上溢；null 解引用只覆盖"空视图"一档（§7 含糊）。

---

### 回报（≤200 字）

**共 75 条**（M-01…M-75），覆盖 depth 定义、逃逸集合/摘要、身份、参数身份、保守方向、所有权释放、trap 契约、纪律八节。
**最关键 5 条**：M-12（`ARENA_HOME` 当深度 0 ⇒ UAF）、M-13（层号唯一权威，定案 68）、M-17/M-18（`Addr≠∅` 才施规则④；不完整即保守拒）、M-25（`D` 必须是单调数据流）、M-43（fail-closed：误拒用内存换，不是 soundness 逼的）。
**最严重含糊 3 个**：①`0` 的三义（参数档/未定哨兵/活到永远）与 fail-closed 口径破裂（M-45）；②`depth` 一数两答 + `TY_REF` 早退答 0（M-05/M-06）；③家 arena 三口径（最浅实参/调用点求解/深度 0）自相矛盾（M-12、S2）。
