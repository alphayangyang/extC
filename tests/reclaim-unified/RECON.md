# `tests/reclaim-unified/` —— 回收的根与宿主：**最统一形态**勘察 + 内部没对齐清单

对象：分支 `main`，HEAD `8a8d96a`（含 `19fd567` 的 C 洞收紧与 `c66e9f4` 的 A+B）。
本轮**只新增本目录**；`src/`、`docs/`、`tools/`、`tests/` 既有文件一个字节没改。
原型改的是 `/tmp/rcproto/`（`src/` + `stdlib/` + `Makefile` 的最小副本），
差分落在 `out/proto-diff/`。

口径：每一节标注 **【实测】**（有可复跑命令与原始输出）或 **【推断】**（读源码/生成物得出的，
没有端到端实测）。没查清的直接写 **未查清**。

---

## 0. 一句话结论

**最统一的形态 = 一份“活引用槽的事实”**：每个引用绑定（连同它属于哪一帧、值要活到哪一层）
由**逃逸分析**那一份事实产出，回收层只负责“把这份事实拼成一张任何触发点都走得到的根链”
（帧节点 + 槽地址数组 + 运行期 `_Thread_local` 链），**不再另立 `rcRoots` / `rcFrameOwnsArena` /
欠收 / `noSweep` 这套第二事实**。

- **净代码量：短期变多（原型 +206 行净，+149/−45 codegen、+58/−1 runtime），
  删特例后变少（可删 ≈100–150 行特例；走到候选 2 再删 4 份重复枚举 ⇒ −150~−250 行）。**
  详见 §D.4。
- **8 类缺口：覆盖 2 类（G1 拥有者只调一次、G6 的“帧内值”那一半），
  部分覆盖 2 类（G5 池 / G6 的宿主面），覆盖不了 4 类**
  （G2 协程帧 + `ext` 帧、G3 `match` 载荷、G4 TLS worker、G7 dyn 载荷）——
  逐条的**精确条件**见 §A.5。
- **原型：部分成立（诚实的“成立一半”）**（§C）。帧根链在
  `hole_c` / `caller_frame` / `persist_nodes` / `owed_home` 四个判据形状上 rc=0、输出逐字正确；
  `owed_home` 的清扫次数 **20**（与现状相同）、**不再依赖欠收/销账**；
  `cross_region` 与 `scale_growth` 的清扫次数、`rs_*`、峰值与现状**一致**（§C.3）。
  **但**：原型在 `cyclic_pressure` 的 N=20000 上 **> 240 s**（现状 0.10 s）——
  性能悬崖定位在 `main` 入口的**帧登记**（`void *__extc_rfs[4096]` × 两遍 1413 项写入），
  而不是标记/清扫路径（§C.4-1）。它给出的设计结论很硬：**帧数组必须是静态偏移表**
  （候选 2），不能是 4096 项的地址数组。
- **内部没对齐：11 条**（§B），最严重的三条：`noSweep`/`unreliable`/`owe`/`bad` 四种“别收”
  状态没有共同出口（B-1）；`fieldSlotKey`（checker）与 `cgSlotHost`（codegen）是同一规则的
  **两份拷贝**（B-2）；A+B 新加的“欠收不需要线程本地”那条注释**推理站不住**（B-3）。

---

## A. 最统一的实现是什么

### A.1 “谁在什么时候需要知道引用的位置”——四侧事实对照表【实测：源码】

| # | 侧 | 数据结构 / 状态 | 在哪算 | 结果存哪 | 回答的问题 |
|---|---|---|---|---|---|
| 1 | **逃逸分析**（`src/check/`）| `Sym.depth` / `minPointee` / `initSrc(+Depth)` / `fields[4]{name,src,srcSym,srcDepth,minReq}` / `otherSrc(+Sym,Depth,MinReq)` / `callSrc(+Depth,MinReq)` / `outOfFrame` / `addressed`（`check_internal.h:95-190`）| `slotDepth`（`check_escape.c:127`）、`storeLayer`（`:259`）、`destDerefSym`（`:191`）、`symMinPointee`（`:219`）、`fieldSlotKey`（`:458`）、`targetDepth`（`:489`）、`valDepthForStore`（`:411`）、`promoteInto2`（`:1128`）| 每个 `Sym`（绑定）身上 | “这个绑定是不是引用槽 / 它指向谁 / 值要活到哪一层 / 槽的字段路径是什么” |
| 2 | **计划层**（`src/plan/`）| 计划侧：`arenaLevel` / `arenaArg` / `zoneLevel` / `usesHome` / `mayUseArena` / `parTlsArena` / `deref`；分析侧（codegen **不许读**，`results.h:97`）：`refDepth` / `homeDepth` / `lexicalLevel` / `storedAt` / `minAt` / `needsHome` / `arenaSites` / `addrMask`… | `plan.c` 的 setter/闭包；分配层由 `checkModule` 末轮定 | `NodeResults` 每节点槽 | “这个分配点该进哪只 arena / 这次调用往下传哪只” |
| 3 | **回收层**（`src/back/codegen.c` 的 `rc*`）| `rcRoots`（槽**表达式**）/ `rcRootMarks` / `rcGlobalRoots` / `rcRootsUnreliable` / `rcFrameOwnsArena` / `rcLeafPaths` / `cgSlotHost` / `RcScan` 三态 | `rcScanType`（`:2982`）、`rcCollectOffsets`（`:3032`）、`rcRootLeaves`（`:3352`）、`rcLeafPaths`（`:3413`）、`rcRootDecl`（`:3460`）、`cgSlotHost`（`:4947`）| `CG` 上的向量 + 每个触发点的复合字面量 | “触发点上哪些槽活着 / 这只区域能不能扫 / 这条槽的宿主是谁” |
| 4 | **代码生成（非回收）** | `g->blkLevel` / `levelCap` / `arenaRefAt` / `cgIsRefType` / `blkMaxLevel` | `arenaRefAt`（`:4381`）、`blkMaxLevel`（`:4772`）、`cgIsRefType`（`:4897`）| `CG` 上的即时状态 | “这层块对应哪只 arena / 这个类型是不是引用” |

**结论：Lead 的假设成立（部分）。** 逃逸分析确实**早就**知道“哪些引用槽在哪、指向什么、
活到哪一层”（第 1 行），而且连**字段路径键**都有（`fieldSlotKey`，`check_escape.c:458`）；
回收层却把“槽的枚举”（`rcRootLeaves`/`rcLeafPaths`/`rcCollectOffsets` 三份同形递归）与
“宿主”（`cgSlotHost`）各自重算了一遍（第 3 行）。
但**不是全部重复**：逃逸侧的字段表只留**4 格**（`fields[4]`，`check_internal.h:95`），
超出 4 个引用字段的绑定就退化成 `otherDepth`，而回收要**逐叶**展开任意宽的结构体
（`rcCollectOffsets` 展开 1024 槽的 `world`）。
所以“统一”不是“直接把 `Sym` 拿来用”，而是**让回收的根表由同一个来源生成、只保留一份枚举器**。

### A.2 重叠 / 重复对照表【实测】

| 知识 | 逃逸侧（唯一来源） | 回收侧（重复实现） | 是否漂移 |
|---|---|---|---|
| “这个类型里哪些槽是引用、路径是什么” | `typeContainsRef`（`check_stmt.c:320/481/759/1154`）| `rcScanType`（`codegen.c:2982`）+ `rcCollectOffsets`（`:3032`）+ `rcLeafPaths`（`:3413`）+ `rcRootLeaves`（`:3352`）+ `cgIsRefType`（`:4897`）| **5 份**；`dyn` 上已漂移（见 §B-7）|
| “引用槽的宿主对象 / 路径” | `fieldSlotKey`（`check_escape.c:458`）+ `Sym.fields[].srcSym`（`check_internal.h:108`）+ `noteFieldSrc`（`check_top.c:2220`）| `cgSlotHost`（`codegen.c:4947`）三态 | **2 份同形规则**；对“视图元素 / 基表达式带调用”都答 UNKNOWN，但键的粒度不同（`[*]` vs 直接放弃）|
| “值要活到哪一层 / 进哪只 arena” | `anRefDepth` / `anMinAt` / `anStoredAt` / `anLexicalLevel`（`plan.c:1117/1030/1075/1121`）| `arenaLevel` / `arenaArg`（`plan.c:114/138`）+ codegen `arenaRefAt`（`:4381`）| 层号与 arena 名两套；`arenaDriftCheck`（`:4410`）在生成期又核一遍 |
| “这个函数会不会分配 / 要不要回收运行期” | `planMayUseArena`（`check_top.c:6368`）/ `anAllocState`（`plan.c:678`）| `scanUnitReclaim` / `rcBodyHasGovernedAlloc` / `rcAllocInExpr`（`codegen.c:9737/9713/9697`）| 2 份；口径不同（“可能分配” vs “回收管辖的分配”）|
| “这个槽的地址能不能取（纯地方）” | `addressed`（`check_internal.h:104`：地址被交出去过）| `cgPurePlace`（`codegen.c:4913`）| 2 份；逃逸侧知道“取过地址”，回收侧还在按 AST 形状现推 |
| 片 / 分片 | **plan 无** | `rcNShard` / `rcSite`（`codegen.c:407/410`）| 只有一份（可接受：纯粹的运行期切分）|

### A.3 最统一的形态：两个候选

#### 候选 1（最小改动）：**“帧描述符 + 根链”** —— 本轮原型落地的形态

- **统一的那个概念**：*“每个活引用槽连同它的宿主帧一起被登记，事实只有一个来源（`rcRootSlots`）”。*
  把触发点“当场物化值表”换成“帧入口登记地址数组 + 运行期沿链解引用”。
- **删掉什么**（逐个核对，见 §C.5 的实测）：
  | 预期能删 | 实际能否删 | 依据 |
  |---|---|---|
  | `rcFrameOwnsArena` | **不能直接删**（原型把它的判据退化成“永远真，除 `parTls`”）——它的**安全侧依据**（“外层帧拿不到指向本帧块 arena 的引用”）被根链取代了，但 `parTls` 那一档还没被覆盖 | §C.5 |
  | 欠收 / 销账（`owe` / `owed` / `checkOwed` / `rcEmitOwe` / `rcEmitOwedCheck`）| **可删** | 原型里 `owed_home` 不靠它拿到 **sweeps=20**（与现状相同，§C.3）|
  | `rcRootsUnreliable`（函数级）| **不能全删**：协程/spawn/枚举载荷三档仍然枚举不出槽，只能降级成**帧级 `bad` 标记**（`extc_rc_frameBad`）——语义从“本函数不触发”变成“整条链拒绝清扫” | §C.2 |
  | `noSweep` 出口 | **可删（条件见 §A.4）**：它是“宿主追不到”的兜底；根链只解决**帧内**槽，视图/池/板里的槽仍然追不到 | §B-1 |
  | `storeRefUnknown` | **可删（同一条件）** | 同上 |
- **新增什么**：运行期 `ExtcRcFrameRoot`（`{void **slots; size_t n; parent; int bad;}`）+
  `_Thread_local` 链头 + `framePush/framePop/frameBad/chainRoots/chainBad` + `__extc_rfs[4096]` 帧数组。
- **净代码量**：原型 +206 行（+149/−45 codegen、+58/−1 runtime）；删特例后估算 **−100~−150 行**（§D.4）。

#### 候选 2（最彻底）：**“逃逸事实 → 静态帧描述符”** —— 不做运行期数组

- **统一的那个概念**：*“事实只有一个来源：逃逸分析写下的 `Sym` 字段表 + `arenaLevel`”*。
  codegen **不再**自己做 `rcScanType`/`rcLeafPaths`/`cgSlotHost`，而是把逃逸分析已经算好的
  “哪些绑定是引用槽、槽内字段路径、值要活到哪一层”**物化成静态帧描述符**
  （`static const {offset} + 帧基址`），帧入口只压一条指针。
- **与候选 1 的差别**：帧数组不是 4096 项、也不取每槽地址；取地址仍要付（候选 1 的
  “不能留在寄存器”代价候选 2 一分不省，但省掉每帧 32 KB 的 `__extc_rfs` 与登记循环）。
- **前置条件（这是它比候选 1 贵的地方）**：逃逸侧的 `fields[4]` **必须扩成任意宽**
  （或改成“按类型的偏移表 + 每绑定一个基址”），否则 1024 槽的 `world` 这类形状描述不出来。
- **净代码量**：删掉 5 份类型枚举里的 4 份（保留一份共用的“类型 → 引用叶子偏移表”，
  它本来就以 `ExtcRcType` 的形式存在于生成物里）+ `cgSlotHost`；新增描述符物化 ≈80–120 行。
  **净减少**（估算 −150~−250 行），但改动面横跨 `check/`、`plan/`、`back/` 三层
  （会碰 `tools/check_plan_seam.py` 的接缝与 `planAnalysisFields` 清单）。

#### A.4 统一后覆盖不了的那一档（语义决定，不是缺陷）

**“板/池/C 栈/`dyn` 池这类不受管辖的内存里的引用槽”**：这些槽
（a）不在任何 C 局部量的地址上、（b）宿主对象本身不由 rc 分配、
（c）值可能被别的非 rc 指针持有。**唯一安全的做法是把目标钉住**
（`storeRefOwned` 的登记位 / 计数，或整片不扫）——**统一形态覆盖不到它们**，
清单见 §A.5。

### A.5 8 类缺口的覆盖核对【实测（原型）+ 推断】

| # | 缺口（`tests/reclaim-gaps/README.md`）| 统一形态覆盖？ | 精确条件 |
|---|---|---|---|
| G1 | 拥有者只调一次 / 被调者长循环（`home_cycle` r=1）| **✅ 覆盖** | 根链在**任一触发点**都看得见祖先帧的 `head`/`keep` ⇒ 不需要“下一次调用”才销账。原型 `caller_frame`（同一个洞的非递归形）rc=0、`n=70000` |
| G2 | 协程帧 / `ext` spawn 帧（`rcRootsUnreliable`）| **❌ 覆盖不了** | 挂起帧不在 C 栈上 ⇒ 链上看不见它 ⇒ **只要链上有 `bad` 帧，整条链拒绝清扫**。要做必须给挂起帧一段**可枚举的帧描述符**（协程帧结构体是 codegen 合成的，字段枚举得出来——这是可做的，但原型没做）|
| G3 | `match` 载荷绑定（`rcRootsUnreliable`）| **❌ 覆盖不了（原型如此）** | 绑定是 arm 里的 C 局部量、作用域到 `if` 块结束；原型仍走 `rcRootDecl`⇒`RC_LEAF_REFS` 的路径？**实测：原型对 `match_payload` 仍置 `rcRootsUnreliable`**，只是语义变成帧级 `bad`。修法是把 arm 绑定的槽也写进 `__extc_rfs`（作用域与块边界对齐），这需要“arm 进入/退出”的登记点 |
| G4 | TLS worker（`parTls`）| **❌ 覆盖不了（也更危险）** | 帧链是 `_Thread_local`，worker 线程的链只有自己的帧；TLS 区域由**整条 worker 调用链**共用 ⇒ 与 home 同构，但“归属帧”不唯一。**原型对它仍然 `frameOwned=false`**（未验证）|
| G5 | 池容器里的引用（`storeRefUnknown`+无触发）| **✅ 覆盖（触发面）/ ❌ 不覆盖（宿主面）** | 触发面：`pool` 那档的 `rcRootsUnreliable` 来自 `rcScanType` 对泛型实参含引用的保守判定 ⇒ 统一后仍 `bad`；宿主面：池缓冲是视图，槽不在帧地址上 ⇒ 只能保留登记/钉住 |
| G6 | 视图元素 / 基表达式带调用（`noSweep`）| **部分覆盖** | 若槽是**帧内**声明的（`v` 是局部量），它的值本身是根 ⇒ 目标被钉住；真正丢的是**目标区域退出清扫**（`noSweep`）。统一形态只把“值”当根，不改“宿主” ⇒ **`noSweep` 仍在**，除非把宿主也统一到逃逸侧的 `fieldSlotKey` 语义（候选 2）|
| G7 | `dyn` 载荷里的引用 | **❌ 覆盖不了（且是语义缺口）** | `rcScanType` 把 `TY_DYN` 归 `RC_NO_REFS`（`codegen.c:2989`），而逃逸侧明说 `dyn` 的载荷会被拷进池、引用要算（`check_escape.c:496` 注释）——**直接矛盾**（§B-7）。统一形态不解决：dyn 池不由 rc 管辖 |

**“统一形态覆盖不了”的单独清单**（语义决定，不是缺陷）：
协程帧、`ext`/domain 帧、TLS worker 区域、`dyn` 池、板/池/栈/C 返回的指针、
以及任何“槽不在 C 局部量地址上”的形状（视图元素、池缓冲、`dyn` 载荷）。
对它们**只能把目标钉住**（计数 + 登记位，或整片不扫）。

---

## B. 编译器内部**没对齐**的地方（完整清单）

标注：**在哪**（文件+函数）· **怎么不一致** · **判定**（无害 / 有风险 / 挡住统一）。

### B-1 四种“别收”的状态没有共同出口（概念级）【实测】

- **在哪**：`rcFrameOwnsArena`（`codegen.c:3139`，编译期“这只区域不扫”）·
  `rcRootsUnreliable`（`:424`，编译期“这个函数不触发”）·
  `owe`/`owed`/`checkOwed`（`reclaim.c:749-758`，运行期“欠一次收集”）·
  `noSweep`（`reclaim.c:120,491,515,706`，运行期“这只区域从此不扫”）·
  再加本轮原型的 `bad`（帧级）。
- **怎么不一致**：五者都表达“种子不完整 ⇒ 别扫”，但**作用域不同**
  （区域 / 函数 / 帧 / 链）、**生命周期不同**（静态 / 一次 / 永久）、
  **恢复方式不同**（等压力 / 等拥有者调用 / 永不恢复）。`extc_rc_shouldSweep`
  （`:705`）只认 `noSweep`，`maybeCollect` 只认 `shouldSweep`，
  `checkOwed` 自己再查一次 `noSweep`——**同一条规则散在三个函数里**。
- **判定**：**挡住统一**（这是 Lead 点名的核心）。

### B-2 `fieldSlotKey`（checker）与 `cgSlotHost`（codegen）是同一规则的两份拷贝【实测】

- **在哪**：`check_escape.c:458`（`fieldSlotKey`，产出 `a.b[0].c` 这样的键；动态下标记 `[*]`）·
  `codegen.c:4947`（`cgSlotHost`，返回 `RC_HOST_OBJ/LOCAL/UNKNOWN` 三态 + 宿主表达式）。
- **怎么不一致**：`fieldSlotKey` 对 `EX_SLICE` 之外的投影都能命名（含常量下标分格）；
  `cgSlotHost` 对**视图元素**与**基表达式带调用**直接答 UNKNOWN，
  而对**常量下标**不区分（都归到宿主对象，不记路径）。
  实测后果：`unknown_host.extc` 走 `storeRefUnknown` 而 `fieldSlotKey` 那条路
  **本来可以命名出 `next`**（`(*id(a)).next` 的根由 `destDerefSym` 给出）——
  保守方向反了（多丢了一份可以拿回的宿主）。
- **判定**：**有风险 + 挡住统一**（两份规则必然漂移，且漂移方向是“多丢回收能力”）。

### B-3 A+B 新注释的推理**站不住**：`owe` 不需要线程本地【实测：源码】

- **在哪**：`reclaim.c:744-745` 的注释“多线程下这笔账不需要线程本地：它只在 `parTls` 为假的
  调用链上被记、被销（TLS 一档归属说不清，一个欠收都不发），而那些帧都在主线程那一条调用链上”。
- **怎么不一致**：`extc_rc_owe` 把标志记在 **`ExtcRcRegion *r`** 上，
  而区域是**按 arena 地址认领**的（`codegen.c:8700-8717`）。
  两个线程各自有 `extc_tls_arena`（`strcmp` 文本里是 `_Thread_local`），
  所以 TLS 区域**不会**与主线程共享；但**`main` 的家 arena 可以被子线程拿到**
  （把 `mut ref`/`mut slice` 交给 `parallel::run` 的 worker 是语言的显式用法）。
  那时 worker 的 `new` 走 `extc_tls_arena`（`codegen.c:4385`）而不是 home，
  所以**今天确实碰不到**——但这是**两个巧合**（(a) `parTls` 下 `rcEmitOwe` 直接返回、
  (b) worker 的分配被改道 TLS），不是注释说的那条理由。
  换个方向即可反证：只要将来允许“worker 往调用者的 home 分配”（`planUsesHome` 的真实语义），
  这条注释立刻失效，而 `owe` 的标志在**共享区域**上 ⇒ 数据竞争。
- **判定**：**有风险**（注释把“今天碰不到”写成“结构上不需要”）。

### B-4 `parTls` 在 `rcFrameOwnsArena` 与 `rcEmitOwe` 里两条不同的“别收”【实测】

- **在哪**：`rcFrameOwnsArena`（`:3140`）`parTls ⇒ false`；`rcEmitOwe`（`:3192`）`parTls ⇒ return`。
- **怎么不一致**：前者说“本帧不拥有”，后者说“连账都不记”——两句都对，
  但**没有一处**说明“TLS 区域的欠收账该记在谁头上”，于是 TLS 档成了
  “既不触发也不欠收”的**第三种状态**（矩阵 G4 实测 `sweeps=0`）。
- **判定**：**挡住统一**（第三种状态没有名字）。

### B-5 `rcScanType` 对 `TY_DYN` 答 `RC_NO_REFS`，与逃逸侧矛盾【实测】

- **在哪**：`codegen.c:2989`（`TY_DYN` 与 `TY_VOID/TY_BUILTIN/...` 并列 ⇒ `RC_NO_REFS`）；
  `check_escape.c:496`（`targetDepth` 的注释：“`EX_DYN` 也不能走这个出口：`dyn T` 自身不带引用，
  但负载被**拷进池**，值会连带引用负载所指的东西”）。
- **怎么不一致**：同一个类型，逃逸侧按“载荷会被拷走、引用要算”处理，
  回收侧按“没有引用槽”处理 ⇒ `rcEmitTypeDesc` 给 dyn 载荷发**空描述符**
  （`codegen.c:3101`），标记到这个槽就停。而矩阵 G7 实测 `dyn_payload` on 比 off 差 **+3.6 MB**。
- **判定**：**有风险**（今天是已知限制 `DYN.md`，但两处口径必须写清一条为真）。

### B-6 `rcRootTableText` 在原型里成了死代码 —— 说明“两份来源”的耦合【实测】

- **在哪**：原型改完后 `rcRootTableText`（`:3160`）没有任何调用者，`make` 报
  `warning: 'rcRootTableText' defined but not used`（原始输出 `out/proto-build.log`）。
- **怎么不一致**：根表有**两个消费点**（`rcEmitTrigger` / `rcEmitOwedCheck`）与
  **两个装载器**（`rcRoots` / `rcGlobalRoots`），装法不一样（一个拼 `(void *)(x)`、
  一个先拼 `&(x)`）——这就是 Lead 说的“同一份知识被算了两遍”。
- **判定**：**无害（但暴露结构）**：统一后应只剩一个装载器。

### B-7 `rcRootsUnreliable` 的三个置位点语义不同【实测】

- **在哪**：`codegen.c:3463`（`rcRootDecl` 遇 `RC_CONSERVATIVE`：**整个函数**不触发）·
  `:5116`（协程 spawn：帧不在 C 栈上）· `:5155`（协程帧字段）· `:5487`（match 载荷绑定）。
- **怎么不一致**：前两者的“不能枚举”是**结构事实**（帧不在栈上 / 类型枚举不出），
  后者只是**一个 arm 里的局部量**；三者共用一个函数级布尔 ⇒ 一个 `match` 就把整个函数的
  清扫关掉（矩阵 G3 实测 2.93×）。
- **判定**：**挡住统一**（“不能枚举”的粒度应该是**槽/帧**，不是函数）。

### B-8 `noSweep` 的两个置位点与 `holds` 的交互【实测】

- **在哪**：`reclaim.c:491`（`storeRefUnknown`：置 `r->noSweep = 1`）与
  `:515`（`storeStructCons(hostKnown=0)`：同一个动作）。
- **怎么不一致**：`storeStructCons` 的注释（`:511-513`）说它的守卫“与 `storeRefOwned` /
  `storeRefUnknown` **逐字对齐**”——但 `storeRefUnknown` 的守卫是 `extc_rc_regionOfOwn(v)`，
  `storeStructCons` 是 `extc_rc_owns(nw)` + `extc_rc_regionOfOwn(nw)`
  （多一道 `owns`）。两句“逐字对齐”与代码不符（多出来的 `owns` 是必要的，
  但注释在骗下一个读者）。
- **判定**：**无害（注释）** —— 但正是 §B-1 那种“靠注释维持一致”的典型。

### B-9 `rcEmitOwedCheck` 的调用点条件在 prototype 前后不一致【实测】

- **在哪**：`codegen.c:2053-2054`（`cgImplicitArgs`）：只有 `rcFrameOwnsArena(g, hlvl)` 为真
  **才发**销账点。
- **怎么不一致**：它的另一半（`rcEmitOwe`）在 `rcFrameOwnsArena` 为**假**时发。
  于是“谁记的账谁来销”靠**同一个函数的两次求值**（分别在调用点与分配点）保持一致；
  只要两处读到的 `g->inMain`/`g->parTls` 不同（例如生成顺序改变），账就没人销。
  实测：`hole_c` 的 `main` 不调用任何东西 ⇒ 账挂着（矩阵 G1）。
- **判定**：**有风险**（隐式耦合，且已经有 G1 这个实际后果）。

### B-10 `arenaDriftCheck` 是第三份“层号”实现【实测】

- **在哪**：`codegen.c:4410`（`arenaDriftCheck`，`EXTC_DBG_ARENA` 下核 plan 层级与
  `arenaRefAt` 的结果）。
- **怎么不一致**：它只**在调试开关下**核对；默认路径下“plan 说第 k 层”与
  “codegen 选了第 k 只 arena”之间没有任何断言。这正是 §A.1 第 2、4 行重复的后果。
- **判定**：**无害（但说明重复是已知的）**。

### B-11 `cgSlotHost` 的三态与 `storeRefUnknown` 的“区域级永久退出”粒度不匹配【实测】

- **在哪**：`cgSlotHost`（`:4947`）返回 `RC_HOST_LOCAL` 时调用方**什么都不做**
  （注释 `:4940`：“静态根表覆盖”）；`storeRefUnknown`（`:5674`）则把**目标区域**永久 `noSweep`。
- **怎么不一致**：`RC_HOST_LOCAL` 的依据是“根表覆盖这条槽”，
  而根表是**词法**的（本帧）；如果这条槽在**外层帧**（`main` 的局部交给被调者写），
  被调者的根表**并不覆盖**它 ⇒ 同一个“LOCAL”判定在跨帧时失效。
  实测痕迹：矩阵 G5/G6 的 `noSweep` 都来自这一档。
- **判定**：**有风险**（`LOCAL` 应当读作“本帧的词法根表覆盖”，而不是“根表覆盖”）。

---

## C. 小原型（`/tmp/rcproto/`，不改仓库）

### C.0 做了什么

`/tmp/rcproto` = 仓库 `src/` + `stdlib/` + `Makefile` 的最小副本（3.0 MB，`make -j8` 可编）。
改动（`out/proto-diff/`，+149/−45 行 codegen、+58/−1 行 runtime）：

1. **运行期**：新增 `ExtcRcFrameRoot`（`{void **slots; size_t n; parent; int bad;}`）+
   `_Thread_local` 链头 + `framePush/framePop/frameBad/chainRoots/chainBad`；
   标记的种子从“调用点传进来的值表”改成“**沿链解引用每一帧的活槽**”。
2. **codegen**：每个函数在体入口声明 `ExtcRcFrameRoot __extc_rfn0; void *__extc_rfs[4096];`，
   把**体级引用绑定提前声明**（`ref` → `(T*)0`，结构体/数组 → `(T){0}`，原声明改成赋值，
   初始化式的执行次数不变），把槽地址写进 `__extc_rfs` 并 `framePush`；
   函数体末尾再写一遍（结构体/数组这类名字这时才在作用域里）；`__extc_ret` 与
   `noArena` 直返处 `framePop`。
3. **触发点**：`maybeCollect` 的实参换成 `chainRoots(&n)`（链不可用则整个跳过）；
   **不再看 `rcRootsUnreliable`**。
4. **没删**：`rcFrameOwnsArena` 仍然在（TLS 一档仍走 `false`）；`rcEmitOwe`/
   `rcEmitOwedCheck` 仍在，但 `owed_home` 的清扫**不再依赖**它们。

### C.1 判据一：四个 C 洞形状一个都没复发【实测】

命令：`out/proto-run.sh`（每形状：`EXTC_RECLAIM=1 ./build/extc -w --no-line-map … -o out/<s>.c`，
`cc -std=c11 -fwrapv -O1`，跑）。

| 形状 | 原型 rc | 原型输出 | 期望（现状判据）| 判定 |
|---|---|---|---|---|
| `hole_c` | **0** | `n = 131071` | rc=0、`n = 131071` | ✅ |
| `caller_frame` | **0** | `n = 70000` | rc=0、`n = 70000` | ✅ |
| `persist_nodes` | **0** | 5 行逐字 | 5 行逐字 | ✅ |
| `owed_home` | **0** | `n = 20000` | rc=0、`n = 20000` | ✅ |
| `cross_region` | **0** | `rounds=8 sink=80 last=8` | 同（关档也同）| ✅ |
| `cyclic_pressure` | **0** | `rounds=8 retired=0 live=8 fwd=28 back=28 idx=8 cache=1 hits=0 depth=1 tree=true` | 同 | ✅（小 N；大 N 见 §C.4-1）|

> 复跑脚本 `out/proto-run.sh` 只按默认参数跑（不传 N）——大 N 会踩 §C.4-1 的性能悬崖。

### C.2 判据二：`owed_home` 不再依赖欠收/销账【实测】

`owed_home` 插桩（`tools/rc-pause-probe.py`）：

```
原型  PROBE sweeps=20 max_pause_us=416.2 max_mark_us=115.3 max_sweep_us=313.7 total_pause_us=7220.9
现状  PROBE sweeps=20 max_pause_us=490.9 max_mark_us=218.3 max_sweep_us=297.4 total_pause_us=7387.7
```

**清扫次数相同（20 = 20）**；`max_mark_us` 从 218 降到 115（种子不再每次物化值表，
但每次清扫多一次 `malloc` + 链遍历）。原型里 `rcEmitOwedCheck` 的 `extc_rc_owed` 判据
**永远为假**（`build` 不再 `rcEmitOwe`，因为 `frameOwned` 为真）——也就是说
**拿回这 20 次清扫不需要欠收机制**。

### C.3 判据三：`cross_region` / `scale_growth` 与现状一致；`cyclic_pressure` 只测到小 N【实测】

（`base` = 仓库 `./build/extc`（HEAD `8a8d96a`）；`proto` = 原型；N 是判据里的规模参数）

| 形状 | N | base 峰值 | proto 峰值 | base sweeps | proto sweeps | base `rs_*` | proto `rs_*` | 输出哈希 |
|---|---|---|---|---|---|---|---|---|
| `cross_region` | 200000 | 12204 KB | 12380 KB | 4 | **4** | inc=200000 dec=196611 live=3389 hwm=65537 | **逐字相同** | 相同 |
| `cross_region` | 800000 | 14956 KB | 14892 KB | 18 | **18** | inc=800000 dec=786444 live=13556 hwm=65537 | **逐字相同** | 相同 |
| `scale_growth` | 200000 | 7532 KB | 7580 KB | 3 | **3** | 全 0 | 全 0 | 相同 |
| `scale_growth` | 800000 | 7596 KB | 7620 KB | 12 | **12** | 全 0 | 全 0 | 相同 |
| `cyclic_pressure` | 200000 / 800000 | 未测到（见 §C.4）| — | — | — | — | — | — |

`cross_region` N=200000 的 `rs_inc/rs_dec/rs_live/rs_hwm` 与
`tests/reclaim-shapes/README.md:517-524` 的基线**逐字相同**（200000/196611/3389/65537）
⇒ **统一形态没有动跨区登记那套机制**。

### C.4 没做成的事（诚实清单）

1. **`cyclic_pressure` 在大 N 上是原型最大的性能红旗（实测，且断点已定位到入口）**：

   | N | 原型 | 现状（base） |
   |---|---|---|
   | 1000 / 2000 / 4000 / 8000 | 0.10 s | 0.10 s |
   | 20000 | **> 240 s（被 kill；~100% CPU、RSS 稳定在 ~7.7 MB）** | **0.10 s** |
   | 200000 | 未测（同上）| 0.10 s |
   | 800000 | 未测（同上）| 未测 |

   用插桩版（`out/C-prototype-raw.txt`）定位：`main` 前三条 `extc_rc_alloc`
   正常打印（stdlib 初始化 / `world w` 的 `new`），**第一次清扫之前就卡住**；
   也就是说挂起**不在**标记/清扫路径，而在 **`main` 入口的帧登记**那一段：
   原型为 `main` 生成 `__extc_rfs[4096]`（32 KB）并在体首、体尾各写一遍 1413 个槽
   （零初始化 + 2826 条 store）。**推断**（未逐指令确认）：真正的元凶是把 32 KB 的栈数组
   交给 C 的零初始化（clang/gcc 会展开成 ~4000 条 store × 2 遍）+ `main` 那个
   1413 项的 `world` 初始化，在 `-O1` 下每个 `main` 帧一次、又被 `-O1` 拒绝优化掉。
   结论对设计的意义与 §D.1 相同：**帧数组不能是 `void *[4096]`**，
   必须走候选 2 的静态偏移表（每帧压一条指针）——**这是原型给出的最有用的一条硬证据**。
2. **`rcFrameOwnsArena` 没有真正删掉**：原型把它保留了（`frameOwned` 对非 TLS 恒真）
   ⇒ “统一后能删 `rcFrameOwnsArena`”这一条**只验证了非 TLS 侧**，
   TLS/协程侧**未验证**。
3. **协程 / `ext` / TLS / match / dyn / 池**：原型没碰（它们要么仍走 `rcRootsUnreliable`
   ⇒ 帧级 `bad`，要么 `frameOwned=false`）⇒ 这五档**未覆盖**，只是**没有被弄坏**。
4. **`__extc_rfs[4096]` 的上界**：占位是按现有根表上限（4096）写的；
   超过就**没有**任何检查（原型里删掉了 `> 4096` 的降级分支）⇒ 这是个已知的**未处理边界**。
5. **ASan 没跑系统性的**：只跑了四个 C 洞形状的普通构建（rc=0）；
   `hole_c` 的 ASan 判据（`hole_c/run.sh` ②）**未在原型上跑**。
6. **递归帧的链**：`hole_c`/`bt` 是递归档，原型 rc=0 ⇒ 每帧一项的语义成立（实测），
   但**深度很大时的栈占用**没有量。

---

## D. 代价与迁移

### D.1 取地址代价（原型实测 + 上轮勘察对照）

上一轮（`tests/reclaim-rootstack/RECON.md` §2.2）量的是**现状设计**：
139 帧 / 344 项需要建表、223 个标量局部要取地址（去重后 50 个），
根表字面量合计 134 883 B（占全语料生成物 0.24%）。

**统一形态下**（原型，同一语料口径）：
- **要取地址的槽数不变**（还是同一批 `rcRoots`），但**多了一份 4096 项的帧数组**：
  每帧 `4096 × 8 B = 32 KB` 栈（`__extc_rfs`），以及**每次触发**一次 `malloc/free`（`chainRoots`）。
  全语料 139 帧 × 32 KB = **4.4 MB 栈**（只是上界；实际只用到 `rcRootSlots.len` 项，
  但 C 数组是按 4096 声明的 ⇒ **每个帧都真的占 32 KB 栈**）。
  **这是原型最贵的一笔，也是它比现状差的主要地方**；
  `cyclic_pressure` 的 N=800000 挂在这一点上（§C.4-1）。
- **修法（候选 2 就直接把它删了）**：把 `void *__extc_rfs[4096]` 换成
  `static const uint32_t offsets[] + 帧基址`（同 `ExtcRcType` 的形态）⇒ 每帧只压一条
  指针 + 一个 n，栈代价 O(1)，取地址代价不变（那 223 个标量局部该落栈还是落栈）。

### D.2 标记遍历整条链的成本（量级）

- 现状：清扫占开关开的指令数 **6%**（`tests/reclaim-shapes/README.md:331-345`），
  清扫次数在 `cross_region` 上 N=200000 只有 **4** 次、N=800000 **18** 次（§C.3）。
- 统一形态：每次清扫多一次“链遍历 + `malloc` 根数组”，
  链长 = 调用深度（`hole_c` 递归 17 层、`persist_nodes` 50000 层递归里每层都有一帧）。
  **量级：O(深度 × 每帧槽数)**，而清扫本身是 O(区域占用)。
  在 `persist_nodes`（N=50000、递归深度 ~50000）上原型 rc=0、5 行输出逐字正确，
  但**没有量指令数** ⇒ **未查清**（这是候选 1 的真实风险点）。
- 注意：现状的“入口守卫”（`shouldSweep` 提到调用点）**在统一形态下必须保留**，
  否则链遍历会被算进每个分配点 ⇒ 把 −56.3% 付回去。原型保留了守卫
  （`extc_rc_shouldSweep` 仍是第一句）。

### D.3 协程帧 / spawn 帧 / worker 的登记与撤销点（具体到函数）

| 档 | 登记点 | 撤销点 | 现状 |
|---|---|---|---|
| 普通帧 | `genBlockBody`（`codegen.c:4872`，`blkLevel==1` 的第一条）| `cgReturn` 的 `noArena` 直返（`:4185`）与共享尾声 `__extc_ret:`（`:6720`）| 本轮原型落地 |
| 递归帧 | 同上（每帧一次；`hole_c` 深度 16 实测 rc=0）| 同上 | 实测成立 |
| 协程 step | 需要在 `genFunc` 的协程分支（`:6636-6660`）里压 `bad` 帧——**今天一个触发都不发** | `genCoroArenaDestroy`（`:4508` 附近）+ step 返回 | **未做** |
| `ext` spawn | `stVar` 的 spawn 分支（`:5089-5117`，帧当普通局部量造）| 帧交给 domain 之后不在栈上；需在 `extc_dom_add` 侧登记 | **未做** |
| worker | `genParTramp`（`:6840` 附近，设 `extc_tls_arena` 之后）| worker trampoline 返回 | **未做** |

### D.4 净代码量（硬指标）

| 口径 | 行数 |
|---|---|
| 原型：新增（codegen）| +149 |
| 原型：删除（codegen）| −45 |
| 原型：新增（runtime）| +58 |
| 原型：删除（runtime）| −1 |
| **原型净** | **+161 行**（不含注释块；含注释约 +206）|
| 可删的特例（估算）| 欠收/销账 ≈ 70 行（`rcEmitOwe` + `rcEmitOwedCheck` + 3 个运行期函数 + 调用点）· `rcFrameOwnsArena` ≈ 10 行 · `noSweep`/`storeRefUnknown` ≈ 25 行（**只在宿主统一之后**）· 4 份重复类型枚举中的 3 份 ≈ 120 行（候选 2）|
| **候选 1 净（删欠收 + 简化 owns）** | ≈ **−50 ~ −80 行** |
| **候选 2 净（删 4 份枚举 + `cgSlotHost`，加描述符物化）** | ≈ **−150 ~ −250 行**（估算）|

⇒ **“是否真统一”的答案是分阶段的**：只做候选 1 是**净增**（+161 − 可删的 ~100 ⇒ 仍为正）；
走到候选 2 才**净减**。这正是“不要停在‘一个根表 + 一条宿主规则’”这句要求的含义。

### D.5 迁移顺序（每一步改什么、跑哪条判据、红了说明什么）

| 步 | 改什么 | 跑什么 | 红了说明什么 |
|---|---|---|---|
| 0 | 先补**“重新打开 home 触发”的红判据**（现在 `hole_c/run.sh` ⑥ 是“不发射触发”的绿，语义要翻转）| `tests/reclaim-shapes/hole_c/run.sh` | 判据本身没改 ⇒ 后面所有步都在没牙的判据上做 |
| 1 | 运行期加帧链 + `bad`（**不动 codegen**）| `cross_region` / `cyclic_pressure` 的 `sweeps`/`rs_*`（应逐字不变）| 运行期新结构影响了既有路径（不该）|
| 2 | codegen：帧入口登记**体级引用绑定**（候选 1 第一半），触发点仍用旧值表 | 全量 `run-switch.sh`（30 条）| 登记/作用域错 ⇒ **真悬垂**（最先红的是 ①j/①i）|
| 3 | 触发点把种子换成帧链（删 `rcRootsUnreliable` 的早退）| `hole_c/run.sh` 7 条 + `run-switch.sh` + `gate_asan_corpus.py` | 链不全（漏了某条声明路径）⇒ C 洞复发 |
| 4 | 删欠收/销账（`rcEmitOwe`/`rcEmitOwedCheck`/`rcEmitOwe` 调用点 + 运行期 3 函数）| `owed_home` 的 `PROBE sweeps > 0`（应仍是 20）| 清扫次数掉了 ⇒ 链没有覆盖到 `build` 的分配点 |
| 5 | 把 `__extc_rfs[4096]` 换成静态帧描述符（候选 2）| `reclaim-perf/gate.sh`（≤ 701 633 044 Ir）+ `cyclic_pressure` N=800000 | 取地址/物化代价退回去 ⇒ 性能账被付回 |
| 6 | 统一宿主：`cgSlotHost` 换成 `fieldSlotKey` 的语义，删 `noSweep`/`storeRefUnknown` | `unknown_host` ①d′ + `cross_region_elem` ①d + 池/dyn 形状 | 宿主观不一致 ⇒ ①d′ 的生成物判据红（那是**语义**问题）|
| 7 | 覆盖协程 / `ext` / worker（各自的登记与撤销点，§D.3）| `run-switch.sh` ③（四个曾经的 SEGV 用例）+ `coro` 语料 | 帧标记错 ⇒ 在挂起帧上照常扫 |

**每一步都只跑与该步相关的判据**（不跑完整套件，按要求）。

---

## E. 未查清 / 没把握（诚实清单）

1. **`cyclic_pressure` N=800000 的原型对比没跑完**（§C.4-1）：原型在该形状上要么极慢、
   要么挂住。**未查清**是栈占用、`memset` 还是链遍历导致的；这直接影响候选 1 的可行性。
2. **标记遍历整条链的指令数量级未测**（§D.2）：只有“清扫占 6%”这个现状数字与
   `persist_nodes` 的正确性实测，没有 Ir/吞吐。
3. **`rcFrameOwnsArena` 在 TLS/协程档能否真删**未验证（§C.4-2）。
4. **`bad` 帧的传播范围**：原型把“链上有枚举不全的帧”处理成“整条链拒绝清扫”，
   于是 `match_payload` 这类**一个 arm 绑定**会让调用它的祖先也一起收不了。
   这是保守方向，但**没有量化它比现状差多少**（现状是“本函数不触发”，
   原型是“本函数 + 它的所有后代/祖先链上的触发都不扫”）——**未查清**。
5. **`__extc_rfs` 超 4096 项的行为**：原型删掉了降级分支 ⇒ 未定义（§C.4-4）。
6. **`dyn` 载荷**：`rcScanType` 与 `targetDepth` 的矛盾（§B-5）只做了**文本对照**，
   没有做“给 dyn 载荷发保守描述符”的实验。
7. **板（`heap::plate`）**：与 `tests/reclaim-gaps/README.md` §6 一样，语言层没有
   “板里的 typed 引用槽”这一形状 ⇒ **测不出**。
8. **逃逸侧 `fields[4]` 的深度**（候选 2 的前置条件）：没有量“超过 4 个引用字段的绑定”
   在全语料里有多少个 ⇒ 候选 2 的改动面是估的。
9. **ASan**：原型只跑了普通构建（§C.4-5）。
10. **没有跑完整测试套件**（按要求）。

---

## F. 复跑

```sh
# 原型（不改仓库）：/tmp 副本 + 打补丁 + 跑判据
tests/reclaim-unified/out/proto-run.sh            # 打印 §C.1 的表
tests/reclaim-unified/out/compare.sh              # 打印 §C.3 的表（base vs proto）

# 差分
tests/reclaim-unified/out/proto-diff/codegen.diff     # 原型对 src/back/codegen.c 的改动
tests/reclaim-unified/out/proto-diff/reclaim.diff     # 原型对 src/back/reclaim.c 的改动
```

> 复跑会**重写** `out/proto-build.log` 与 `out/C-prototype-raw.txt`：这两个是裸转储、**不入库**
> （`check.sh` 的 `check-tree` 盯着这条）。上面列的 `.sh` 与 `proto-diff/*.diff` 是复现脚本与原型记录，在库里。
