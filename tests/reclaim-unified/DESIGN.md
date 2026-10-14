# `tests/reclaim-unified/DESIGN.md` —— 区域回收**统一怎么做**（设计，未实现）

对象：分支 `main`，HEAD `9e62cb8`。`src/` 自 `8a8d96a` 起未改动（`git diff --stat 8a8d96a HEAD -- src/` 为空，
本轮复核过），所以本文件里对 `src/` 的引用与 RECON.md 同一份代码。

口径：每条标注证据等级 ——
**【本轮实测】**（我自己跑出原始输出）、**【读源】**（我逐行读过）、
**【他人复核】**（子代理复核，已注明）、**【推断】**（从源码/生成物推出，无端到端实测）。
**RECON.md 的主张不当证据用**：子代理逐条复核了它的 §B，其中 4 条不成立或与实际相反，
本文只用复核后的版本（见 §7）。

本轮**没有改 `src/`**；两个实验都只改 `/tmp` 里的生成物副本（§5）。本文件是新增文件。
一次 `tests/reclaim-gaps/run-gaps.sh` 跑出来的 `out/*.tsv` 已 `git checkout` 还原（见 §9）。

---

## 0. 一句话结论

1. **7 个收不掉的形状不是 7 个机制坏了，是同一份"种子完整性"在三个地方漏**：
   **① 跨帧看不见**（G1/G4）、**② 非帧存储没有登记点**（G2/G5/G6/G7）、
   **③ 粒度是"函数/区域"而不是"槽/帧"**（G3、`storeRefUnknown`）。
2. **旧的便宜修法（拥有者返回后销账）实测无效**：G1 的峰值产生在**被调者自己那个长循环里**，
   返回之后收只能把"最终字节"降下来，**峰值一分不回**（实测 18.3 MB → 19.5 MB，反而更差）。
3. **真正能让峰值有界的，是"被调者触发时看得见祖先的活引用槽"** —— 我用 `/tmp` 生成物原型
   模拟了这条链的核心：`home_cycle` rounds=1 N=640000 峰值 **18.3 MB → 5.1 MB**、
   `sweeps` **0 → 9**、stdout 逐字一致（§5.2）。这就是根链该做的最小形态。
4. 因此路线是：**链（跨帧）+ 登记点铺全（非帧）+ 粒度收到槽（函数/区域级判据退场）**，
   分步上，**每步先有红判据**；`owe/checkOwed/noSweep` 三样是这三条做完之后**删掉**的东西，
   不是要保留的基础设施。

---

## 1. 现状：种子链（复核过的地图）

**【读源】+【他人复核】一致**。种子只有两个来源：**静态根表**与**本区登记位**。

```
逃逸侧事实（Sym：depth/minPointee/fields[4]/initSrc/callSrc/otherSrc/outOfFrame/addressed）
  └─(唯一换算点) src/check/check_top.c:6196-6212 → planSetArenaLevel + anSetRefDepth
       └─ codegen 只读 planArenaLevel（codegen 不读 anRefDepth/anMinAt/anLexicalLevel…，
          清单在 src/plan/plan.c 的 planAnalysisFields，接缝闸门 tools/check_plan_seam.py）
            └─ rcRootDecl(codegen.c:3460) 把词法作用域里活着的引用槽**表达式**推进 g->rcRoots
                 └─ rcEmitTrigger(codegen.c:3267) 每个分配点把 rcGlobalRoots+rcRoots
                    拼成复合字面量 (void*[]){…}（rcRootTableText:3155）
                      └─ extc_rc_maybeCollect(reclaim.c:731) → collect → mark(reclaim.c:630)
```

关键的四个结构事实（都【读源】）：

| # | 事实 | 位置 |
|---|---|---|
| 1 | 根表是**值语义的复合字面量**，每个触发点当场物化；入口守卫只在 `shouldSweep` 真时才物化 | `codegen.c:3278-3280`、`:3155-3171` |
| 2 | 根表**按词法作用域**拼，跨帧看不见（家 arena / TLS 是调用链共享的） | `codegen.c:2899-2941` 块注释、`rcFrameOwnsArena:3139-3143` |
| 3 | 跨帧的补救是**欠收**：内层只记 `owe`，拥有者帧在**下一次把 home 传出去的调用之前**销账 | `rcEmitOwe:3191-3206`、`rcEmitOwedCheck:3224-3241`、`cgImplicitArgs:2053-2054` |
| 4 | 宿主证不出来 ⇒ **目标区域永久 `noSweep`**；`noSweep` 全仓没有清零点 | `reclaim.c:489-493`、`:503-518`；读点 `:706`、`:756` |

"别收"的出口实际有 **编译期 5 处 + 运行期 4 处**（子代理逐点复核，我抽查了其中 7 处）：

- 编译期：`needReclaim`（单元级，`codegen.c:9611-9617` + `scanUnitReclaim:9737-9757`）·
  `rcGlobalsUnreliable`（全单元，`:9437-9439`）· `rcRootsUnreliable`（**函数级粘性，9 个写入点**：
  `:3226/:3271` 4096 溢出 ×2、`:3354/:3364`、`:3463` 类型保守、`:5116/:5155` 协程帧、
  `:5487` match 绑定、`:6627` 函数入口；唯一复位也在 `:6627`）· `rcFrameOwnsArena` 假（帧×区域，
  `:3140`，改记欠收）· `@overwrite` **第三份内联判据**（`:5254`，连 `parTls/coroFunc` 都没排除）。
- 运行期：`noSweep`（区域级**永久**）· `owe`（区域级**一次**，恢复=拥有者下次带 home 的调用）·
  `shouldSweep` 压力线+`scale`（可恢复）· `checkOwed` 的 `!r->owe`。
- 认领不一致【他人复核，我读过这三处】：`shouldSweep` 只认 `noSweep`+压力（`reclaim.c:706-712`）、
  `maybeCollect` 只认 `shouldSweep`（`:732`）、`checkOwed` 只认 `owe`+`noSweep`（`:754/756`）
  —— **且 `checkOwed` 不过压力线**（它不调 `shouldSweep`）。

---

## 2. 缺口与病灶的映射（我复跑过的矩阵）

**【本轮实测】** `tests/reclaim-gaps/run-gaps.sh` 在 HEAD 上整跑一遍（数字与 README 记的一致，
例：`home_cycle_r1` 2.91× vs 记的 2.94×、`coro_frame_cycle` 3.00× vs 2.97×）：

| 病灶 | 形状（收不掉） | on 160k→640k | 比关档差 | sweeps |
|---|---|---|---|---|
| ① 跨帧看不见 · home | `home_cycle` rounds=1 | 6.17→17.92（**2.91×**）| **+6.6 MB** | **0** |
| ① 跨帧看不见 · TLS | `par_tls_cycle` | 8.54→22.79（**2.67×**）| **+6.5 MB** | **0** |
| ② 非帧 · 协程/ext 帧 | `coro_frame_cycle` / `ext_frame_cycle` | 5.90→17.71 / 5.96→17.71（**3.00×/2.97×**）| +6.5/+6.6 | **0** |
| ② 非帧 · 池缓冲 | `pool_ref_slot` | 6.11→17.92（**2.93×**）| **+6.6 MB** | **0** |
| ② 非帧 · 视图元素/调用基 | `slot_elem_unknown` / `base_call_unknown` | 12.29→33.11 / 12.29→33.17（≈**2.7×**）| 比 off 好 | 4/19（收的是别的区域）|
| ② 非帧 · dyn 载荷 | `dyn_payload` | 24.29→82.29（**3.39×**）| **+3.6 MB** | 2/9（dyn 池不回收）|
| ③ 粒度 · 一条 match 绑定 | `match_payload` | 6.11→17.98（**2.94×**）| **+6.6 MB** | **0** |

对照（同形状、只换"宿主可证/机制在"）：`slot_elem_known` 1.00× · `own_room_cycle` 1.01× ·
`home_cycle` rounds≥64 1.01×（子集 B 的销账点每轮都在跑）· `struct_copy_*` / `carray_slot` /
`owcell_garbage` / `byval_temp` / `closure_chain` 全 1.00×。

⇒ **③ 是最便宜的一类**（`match` 一条绑定不该关掉整个函数）；**① 是唯一被实测证明"必须动链"的一类**
（§5）；**② 里"协程/ext/TLS"要链的帧描述符，"池/视图/dyn"是登记点与宿主规则**。

---

## 3. 目标形态：一份事实 + 一条链 + 一条宿主规则

### 3.1 一条链：线程本地**槽描述符栈**（不是每帧 `void *[4096]`）

- **条目** = `(void *base, const ExtcRcDesc *desc)`：`base` 是这条绑定存储的地址，`desc` 是
  **该类型的引用叶子偏移表** —— 就是现有 `ExtcRcType{nptr, off[]}`（`reclaim.c:82-86`）的形态，
  加一个"保守档"变体（`size` 已知、逐字扫）。
- **压/弹**：函数入口压形参、声明处压绑定、**块边界弹出**（现有的 `rcRootMarks` 机制换成栈 mark
  即可，`codegen.c:4832/4863-4865`）；递归 ⇒ 每帧一项；帧退出弹到帧 mark。
- **O(1)/帧**：每帧不建数组，只压它与祖先的那几条绑定（`reclaim-rootstack/RECON.md` §2.2(b)
  实测：有引用槽且会触共享区域的帧 = 139，表项 **344**，每帧 K 最大 **7**、中位 **2**）。
- **触发点**：`if (shouldSweep(r)) maybeCollect(r)` —— **不再传根表实参**，运行期沿链取种子：
  `种子 = 链上每个 base+off 解引用 + 全局 + 本区 rsbit`。入口守卫保留（它省的是物化，不是判据）。
- **RcDyn 的两个好处是顺带的**：4096 上限消失（`codegen.c:3226/3271/3354/3364` 四条一起删）、
  每触发物化消失（`cyclic_pressure` 那条 1412 项的字面量不再出现在每个分配点上）。

**为什么"保守档"是关键**：现在的"枚举不出来"一律升级成**整函数不触发**（9 个写入点共用
一个布尔）。有了"逐字保守描述符"，**枚举不出来 = 这个槽扫宽一点**，不是"整个函数别扫" ——
`match` 载荷绑定、泛型实参带引用、`TY_ENUM`（`rcScanType:2992` 无条件保守）都是**帧里有地址的
C 局部量**，逐字扫是安全方向（`owns` 守卫会滤掉非 rc 值）。⇒ **`rcRootsUnreliable` 这个概念可以整
个删掉**，G3 的 2.94× 不需要新机制，只需要"该登记而没登记"补上。

### 3.2 一份事实：谁产出"活引用槽表"

分两级，**先做 A，B 是"净减代码"的那一步**：

- **级 A（机制先行，不动接缝）**：`codegen` 仍在声明处知道绑定，但产物从"表达式"变成
  "(base, desc)"；类型扫描仍用 `rcScanType/rcCollectOffsets`（现有 4 份枚举器的两份）。
- **级 B（真统一）**：把"类型 → 引用叶子偏移"这一份枚举**移进类型层一份**，逃逸侧的
  `Sym` 事实与回收侧都引用它；codegen 只经 `plan.h` 读"这个函数有哪些引用槽、各活到哪层"。
  代价与门禁：要动 `planAnalysisFields` 清单与 `check_plan_seam.py`（分析侧字段被 codegen 一读
  就必须搬成 `planXxx` 访问器）。收益：现有 **4 份编译期枚举器 + 2 份运行期描述符遍历**
  （`rcScanType/rcCollectOffsets/rcRootLeaves/rcLeafPaths`；`mark` 的 `:665-668` 与
  `unlinkChildren` 的 `:398-401`）收成 1 份枚举 + 1 份遍历。

### 3.3 一条宿主规则：目的地求值一次，逐叶落账；证不出宿主 ⇒ **钉住目标**而不是关掉区域

- **求值一次**：赋值的 place 先接进指针/临时量（现在 `cgSlotHost` 要求 `cgPurePlace`，
  非纯基直接 `UNKNOWN`，`codegen.c:5649-5654`）。
- **三种终点**（取代现在"LOCAL/OBJ/UNKNOWN"的三态）：
  1. 槽在**帧/全局**上 ⇒ 链已覆盖，只 `storeRef`（不发登记）；
  2. 宿主是**可证区域的 rc 对象** ⇒ 逐叶 `storeRefOwned`（现有 `rsInc/rsDec`，来源侧可撤销）；
  3. **其余（视图元素、池缓冲、dyn 载荷、C 存储）** ⇒ **钉住目标**：读旧值→写新值→
     `pin(v); unpin(old)`，按**每槽计数 + 登记位**实现（不是区域级 `noSweep`）。
- **为什么钉住比 `noSweep` 好**：`noSweep` 把**整个区域**的清扫能力永久扔掉（G6 实测 2.7×、
  另一个区域的 4/19 次清扫白扫）；钉住只让**被写进去的那个目标**活到被覆盖/容器消失，
  其余垃圾照收。**界与轮数解耦的关键是"覆盖时撤销"**：`slot_elem_unknown` 每轮都覆盖同一个槽
  ⇒ pin 数有界于"非帧引用槽的个数"（池容量/视图长度），**不随轮数增长**——这正是当前
  `storeRefUnknown` 不做、而 `rscnt` 对跨区边已经做了的那件事（`reclaim.c:97-101` 的注释）。
- **容器消失时撤销**（"登记点铺全"那一半）：容器是帧内局部（视图变量 / 池 handle）时，
  在它的**作用域出口**批量 `unpin`；容器是 C 返回的存储（`extern!`）时按**语义边界**处理
  （只能在文档里写成决定，见 `docs/RECLAMATION.md` 末节的边界条）。
- `dump`：`cgValueRefStore`（`codegen.c:5013-5034`）与 `storeStructCons`（`reclaim.c:503-518`）
  的"保守只加不减 + `noSweep`"也走同一条 `pin/unpin`，不再有第二套出口。

### 3.4 "别收"只剩一个出口

做完 3.1~3.3，**运行期区域上只剩一个状态**：`pinned`（有追不到宿主的入边，但每个都是**槽级 pin**，
不是区域级），它的语义 = "别把这些目标放掉"，**与"这个区域能不能扫"解耦**。于是：

- 删 `noSweep`（`reclaim.c:120` 字段、`:491`、`:515` 置位、`:706`、`:756` 读点）；
- 删 `owe/owed/checkOwed`（`:749-758`）与两个发射点（`rcEmitOwe:3191`、`rcEmitOwedCheck:3224`、
  `cgImplicitArgs:2053-2054`）；拥有者/子集 B 的概念随之消失；
- 删 `rcFrameOwnsArena`（`:3139`）与 `@overwrite` 那份内联判据（`:5254`）——触发不再依赖
  "本帧是否拥有"，因为链让任何触发点的种子都完整；
- 删 `rcRootsUnreliable` 的 9 个写入点与 `rcGlobalsUnreliable`（保守档取代）。

---

## 4. 不变量（缺一条就悬垂；写成代码注释 + 判据）

1. **链的完整性**：任何触发点，`种子 = 链上所有活槽（含所有活跃帧、含挂起协程帧的帧描述符）
   + 全局 + 本区登记位`。**任何一条活引用槽都必须能沿这三者之一到达**——这是全案唯一的核心不变量。
2. **压/弹配对**：每个 `push` 在同一词法作用域的出口 `pop`；帧入口/共享尾声（`__extc_ret:`）
   是只发一次的收口。
3. **保守档是宽扫不是漏扫**：枚举不出来的槽一律"逐字扫 + `owns` 守卫"，绝不"跳过"。
4. **pin 的成对**：每条 `pin` 有对应 `unpin`（覆盖时）或作用域出口批量撤销；跨区边的
   `rscnt` 规则（`reclaim.c:97-101`）原样适用于 pin。
5. **陈旧槽**：链上的条目在作用域出口必须弹掉；`mark` 对槽地址解引用时要过 `owns`
   （板/池/栈/C 指针）。
6. **挂起帧**：协程/ext 帧在挂起时必须把自己的帧描述符压进**该任务可见的链**（或链上标记
   "该任务不可扫"），恢复时弹；不能像今天这样"整函数不触发"。

---

## 5. 我自己做的两个决定性实验（都在 `/tmp`，未改 `src/`）

形状：`tests/reclaim-gaps/shapes/home_cycle.extc`（G1：被调者在调用者家 arena 里攒自环，
被调者自己是长循环）。口径：`EXTC_RECLAIM=1 ./build/extc` 生成 → `cc -std=c11 -fwrapv -O1` →
`/usr/bin/time -v` 取 Maximum resident set size；清扫次数用 `tools/rc-pause-probe.py`。

### 5.1 【本轮实测】**"拥有者返回后销账"救不了峰值**（证伪便宜修法）

在生成物里把 `owe` 销账点**补在 `build(per)` 返回之后**（同一个 `checkOwed`、同一个区域键、
根表 = `{keep}`）——这正是 `reclaim-rootstack/RECON.md` §7 的 ③b。

| 变体 | N=160000 | N=640000 | 备注 |
|---|---|---|---|
| 基线 | 6252 KB | 18348 KB | sweeps=0（矩阵口径）|
| 返回后销账 | 6504 KB | **19500 KB** | 插桩显示 `owed=1 bytes=10240016` → `checkOwed freed=640000 sweeps=1 bytes=16` |

⇒ 账**确实销掉了**（末态 16 字节），但**峰值一点没回**（反而 +1.15 MB）：
峰值出现在 `build` 的循环**进行中**，返回之后的收集只能在"水已经漫过"之后放水。
**结论：G1 的 2.91× 不可能靠 ③b 修**，必须让**被调者在循环里就能收**。

### 5.2 【本轮实测】**祖先槽可见 ⇒ 被调者自己能收，峰值有界**

模拟链的核心效果：加一个文件级槽 `__g1_ancestor`（模拟"祖先帧的活引用槽在链上可见"），
把 `build` 的两个 `owe` 换成 `maybeCollect(区域, {keep, __g1_ancestor})`，并让 `main` 在
下一轮调用前发布自己的 `keep`。

| 变体 | N=160000 | N=640000 | sweeps(N=640k) | stdout |
|---|---|---|---|---|
| 基线 | 6252 KB | 18348 KB | 0 | `rounds=1 per=… last=…` |
| **链模拟** | **5100 KB** | **5100 KB** | **9** | **逐字相同** |

⇒ 峰值 **18.3 → 5.1 MB**，且 **N 不变**（5100/5100）。**这就是根链要做的事的最小证明**：
被调者触发时能看见祖先的槽 ⇒ 它可以在自己的循环里扫共享区域 ⇒ 界与规模解耦。
（此原型是**模拟**：`__g1_ancestor` 是手工发布的一位；真实实现要由编译器把每个活绑定压链。
它证明的是机制有效，不是实现已完成——见 §8。）

---

## 6. 分步实施（每步：改什么 / 跑什么 / 红了说明什么）

**顺序原则：先有牙，再动机制；每一步只跑与该步相关的判据。**

### 步 0（先决条件）：先让判据"能红"，再动机制

**今天的接线现状**【他人复核，我抽查了 `hole_c` 与 `unset` 两条】：
回收在 `check.py` 里有 5 个专用项（旧 `check.sh` 没有，都是重写后新增）：
`gate-reclaim-evidence` / `gate-hole-c` / `gate-reclaim-perf` / `gate-reclaim-audit`
（`gate` 档，进 quick）与 `gate-reclaim-gaps`（`bench` 档）。

| 脚本 | 有没有牙 | 接进 CI? |
|---|---|---|
| `tests/reclaim/run.sh`（运行期语义 70 条断言 + ASan）| **有** | 是（传递）|
| `tests/reclaim-shapes/run.sh`（基线表）| 无（恒 exit 0）| 是（传递，等于没接）|
| `tests/reclaim-shapes/run-switch.sh`（30 条，①k 钉 `sweeps>0`）| **有** | **是**（传递）|
| `tests/reclaim-shapes/run-evidence.sh`（因果闭合 3 条 + gdb 数清扫）| **有** | **是**（`gate-reclaim-evidence`，5.5s）|
| `reclaim-shapes/hole_c/run.sh`（7 条，含待翻转的 ⑥）| **有** | **是**（`gate-hole-c`，1.2s）|
| `reclaim-perf/gate.sh`（≤ 701 633 044 Ir）| **有** | **是**（`gate-reclaim-perf`，3.1s）|
| `reclaim-gaps/ledger-gate.py`（全语料账本棘轮：13 条逐条比）| **有** | **是**（`gate-reclaim-ledger`，6.1s）|
| `reclaim-gaps/gate.py`（棘轮：9 条已知坏 + 每形状 sanitizer）| **有** | **是**（`gate-reclaim-gaps`，63s，`bench` 档）|
| `reclaim-gaps/run-gaps.sh`（矩阵 runner）| 无（恒 exit 0）| 否（它只产出表，棘轮在 `gate.py`）|
| `reclaim-elide/run.sh`（编译期收窄缺口）| 无（恒 exit 0）| 否（它只记录，棘轮在 `gate.py`）|
| `reclaim-elide/gate.py`（棘轮：4 条「该省」红 + 反空跑 rc=2）| **有** | **是**（`gate-reclaim-elide`，1.5s）|
| `tests/escape-fuzz/`（风险面 83/110 个程序在这里）| 脚本无牙（恒 exit 0）| **夹具已进 `gate-asan-corpus`**（119 个形状，其中 114 个真的发射回收运行期）；它自己的 harness 仍未接 |

要做的（按顺序）：

1. **链落地时翻转 `hole_c` ⑥**：今天它是"开档分配点**不发射**触发"才算 ok
   （`hole_c/run.sh:150-159`；复跑确认今天 = `maybeCollect` 0 / `owe` 1 / `checkOwed` 1），
   链做完必须改成"**发射**触发且 `sweeps > 0`"。门禁本身已接进 CI（`gate-hole-c`），
   到时候翻的是断言，不是接线。⑥ 的牙已注入验过：往生成物塞一行提到 `extc_rc_maybeCollect`
   的注释 ⇒ 该条 FAIL、脚本 rc=1。
2. **回收能力账本**（已落地，语料级）：`gate-reclaim-ledger` 把全语料分类
   （`ok-trigger` / `owe-settled` / `silent-both` / …）的红计数①做成棘轮，基线
   `audit-known-bad.txt`（13 条）。它给的是**程序级**账本；还差的是**逐站点**的
   触发/欠收/销账计数（`reclaim-rootstack/scan-triggers.py` 那条路：只观察、不认
   `owe`/`checkOwed`、快照停在 A+B 之前）——等链动到 codegen 时再补。
3. **关档一律显式 `=0`**（已落地）：`run-evidence.sh` 与 `reclaim-perf/run.sh` 都改了，
   `run-evidence.sh` 5 通过 / 0 失败；复核判据 `gate-reclaim-evidence`。
4. **缺口矩阵棘轮**（已落地）：`gate.py` + `gate-known-bad.txt`（今天 **9** 条：`home_cycle_r1`、
   `slot_elem_unknown`、`base_call_unknown`、`coro_frame_cycle`、`ext_frame_cycle`、
   `match_payload`、`par_tls_cycle`、`pool_ref_slot`、`dyn_payload`），接成 `gate-reclaim-gaps`。
   两个方向都红，注入验过。同一个闸门还断言**每个形状的 sanitizer 结论**（`asan=0`；
   基线 `asan-known-bad.txt` 当前为空），红向注入验过。全语料那一份棘轮见第 2 条。
5. **性能闸门**（已落地）：`gate-reclaim-perf`（3.1s，进 quick）；现行线 **324 000 000**，
   实测 308 485 778 Ir（相对最初 −73.6%）；红向注入验过（`LIMIT=1` ⇒ rc=1）。
6. **扩大 ASan 闸门的语料**（已落地）：`gate-asan-corpus` 的正例语料加进
   `tests/escape-fuzz/cases`（119 个形状，自足、不用 argv）与 `tests/reclaim-shapes/hole_c`（2 个）
   ⇒ **248 → 369 个程序**，9.4s。其中 **114/119** 个 escape-fuzz 形状真的发射回收运行期
   （不是白跑）。当前 1 条已知坏（`tools/escape-corpus` 那个畸形 C 的旧条目），新增 0。
   仍未做：`escape-fuzz` 自己的 harness 还是恒 exit 0；需要 argv 的回收形状
   （`tests/reclaim-shapes/*.extc`、`tests/reclaim-gaps/shapes/*.extc`）没进这份语料 —— 前者由
   `run-switch.sh`、后者由 `gate-reclaim-gaps` 的 sanitizer 那一段覆盖。

7. **`bt` 的开销归属（已量，抬高了"计数收窄"的优先级）**：`bt` 上 **92 % 的 `ownSlab`
   调用来自 `initRef`**，而它几乎全是"已有引用拷进局部绑定"（`var p = t`）；局部绑定
   唯一的销账机会是它自己被重新赋值，而 codegen **在块退出/函数返回都不发 `dec`**
   ⇒ 这些 `inc` 一笔也销不掉。归因与复现见
   [`tests/reclaim-perf/README.md`](../reclaim-perf/README.md) §10。
   消融还把它**从"Ir 账"升级成"墙钟账"**：删掉 `initRef` 里那一行计数，`bt` 592 → 251 ms
   （关档 244 ms），D1 缺失只降 12 %（§10.1）⇒ **指令受限，且受限在计数那一支**。
   ⇒ **在动链之前，先把"计数按编译期收窄"排到前面。**

   **健全性义务（动手前必须先立的线）**：`hold` 归零**当场就放**，所以
   * 能省的只有 `inc` 一侧；**省 `dec` 永远安全**（代价只是晚收）；
   * 判据是**对象级**的：该对象的**每一条**入边都不会有 `dec` 落下来（只盯一条边不够）；
   * 实现上要能把 `holdUpd +1` 与跨区登记 `rs` **分开省**（`rsbit` 是清扫的种子）。

   三类充分来源与可行性（详见 `tests/reclaim-elide/README.md` 与
   [`docs/RECLAMATION.md`](../../docs/RECLAMATION.md) 第五节末条）：
   ① 该对象的全部入边**统一决定**（全省则死期由区域/清扫兜底）—— 要有"对象级入边封闭"事实；
   ② 本函数 `new` 出来、别名集合封闭、各入边槽**此后再不被写** —— **最小的一类**，
      只需要新增**一格事实**："这个绑定/字段此后再不被写"（check 侧算、plan 侧暴露）；
   ③ 有一条持有边**在被省的边活着期间不会被销账**（**不是**"存在别的持有者"——
      持有边自己可能先死，实测反例 `keep_other_holder_dies`）。持有者在函数外时，
      要每参数一位 `mayDec` 效果 —— 这正是那笔最大的账（`var p = t`）所需的东西。

### 步 0.5：三条「把回收税压到 ~1.0×」的技术路线（2026-10-13 并行调查，全部在 /tmp 实测）

目标口径：**不需要回收的形状，回收开销趋近 0**。度量 = `tests/reclaim-perf/gate-tax.sh`
（`bt` 开档/关档 Ir；现在是 **4.637×**）。三条路线互不排斥，实测收益如下：

| 路线 | 规则（一句话） | `bt` 的税 | 备注 |
|---|---|---|---|
| **A. 跨过程「本帧不会销账」效果** ✅**已落地** | 函数级最小不动点 `rcNoDec(F)`：F 及其被调者集合里没有引用槽写、没有解析不到的被调者/extern、没有并发 ⇒ `F` 里局部引用绑定的 `inc` 改发 `extc_rc_initRefNoHold` | **全输入 4.124× → 1.075×**；in-small **4.637× → 2.040×**（已实测，与模拟逐位吻合）| 只覆盖**纯读遍历**（`bt` 的 `check`）；`rebuild` 0 处可省、不误伤；判据 `elide_readonly_recursion` 转绿、两条反例语义敏感 |
| **B. 区域级「这片不需要提前回收」（Tier 1）** | 按**值的区域**分类（闭包引理：深度规则保证同一对象的各入边值深度相同 ⇒ 天然对象级统一），整片省 `inc`，**保留**跨区登记 `rs` 与清扫 ⇒ **无条件内存安全** | **1.945×**（Tier 2 再封到 1.756×；Tier 3「封住的区域改走普通 arena」估计 ≈1.0×，未实测） | 不需要新内存分析；Tier 2「封」必须加「分配点不在循环里」否则内存爆（实测 cp 3.56×、rebuild 2.87×）|
| **C. 对象级「账本只增不减」= 原 E2** | 对象本函数 `new` + 别名封闭 + 各入边槽此后再不被写 | **无收益**（`bt` 6 个计数点全被排除 → 税仍是 4.637×） | 价值在判据：E2 + 「静态 null 的 `inc`」两条一起能让 `reclaim-elide` 两行转绿；`0` 收益要说出来 |

**关键实测（口径之争）**：路线 A 只省 `check` 里那一处 `inc`，`bt` 的税就
in-small `4.637× → 2.041×`、mid（`12 14 200`）`4.199× → 1.185×`（我自己复跑）。
⇒ 残余的 **0.94×（in-small）**是**每对象的结构开销**（slab 分配/`regionOf`/`regionDrop`/
`unlinkChildren`），随遍历次数摊薄 ⇒ **"税→1.0"要在遍历占主导的口径上判**，
in-small（分配占主导）只适合当"结构开销"的棘轮。`gate-tax.sh` 的判据口径要按这条定。

**健全性边界（三条路线共同的前提，实测反例）**：
- 省 `inc` 才危险（`hold` 归零当场 `freeSlot`）；省 `dec` 永远安全。
- 必须**对象级统一**：只省一条边 ⇒ 输出变（`keep_two_edges`：`x` 5→1；`keep_other_holder_dies`：`v` 7→2）。
- **跨区登记 `rs` 不能省**（它是清扫种子）：实测省掉 ⇒ `cross_region_elem` **SIGSEGV**。
- 并发/未知被调者/FFI 回调/泛型实例/协程帧必须整片排除（路线 A 给了 3 个实测反例 CE1/CE2/CE6）。

**下一步（按证据排序）**：路线 A 是**唯一在真实输入上把税压到 1.075×**的设计，且落点明确
（check 侧新的 `mayDec` 不动点 + plan 侧一格 bool + `codegen.c:5332` 一个条件）；
路线 B 的 Tier 1 是"无条件内存安全"的整片省略，适合当第二条；路线 C 当最小红判据的先导。
落地前必须复核：`storeStructCons` 的"只加不减"（路线 B 报告里最可能的落地后真洞）、
FFI 回调重入（路线 A 未测到）、`noSweep` 区域。

### 步 0.6：Tier 3 —— 「这片不需要提前回收」⇒ 改走普通 arena（下一档性能，未实现）

**为什么只剩这条路**：`bt` 的残余成本里**计数只值 5.1 %**（消融上界），约 **87 % 是 rc 分配器
本身的结构开销**（`extc_rc_alloc` 的元数据/清零、每次赋值的两次 `ownSlab` 查询、触发检查、
帧销毁记账）。逐对象的账：开档比关档多 **≈445 Ir/对象**（全输入口径税 1.035×、占比 3.4 %）。

**为什么静态"无垃圾"证明对 `bt` 不成立**：`go` 的 `while d <= maxdepth` 每轮建一棵树、
上一轮的树变成垃圾（只靠区域整体释放兜底），循环上界来自**输入**。所以"分配有界"这条
静态证明拿不到 —— 而 `bt` 恰恰是用户要"零回收税"的形状。

**可行的形态：区域级的**运行期混合策略**（不需要静态证明）**
1. 区域开局是 **plain 模式**：分配走该区域自己的 arena（bump），不计入 rc 槽；
2. 维护 `r->plainBytes`；当 `plainBytes + bytes` 超过 `r->floor`（默认 1 MiB）时**切到 rc 模式**，
   之后的分配走 slab（可计数、可清扫）。切换是**单向**的。
3. 界的来源是**切换条件本身**：`plainBytes + bytes > floor` 才切 ⇒ 每个区域里"不可回收的
   plain 字节"**上界就是 floor**，与清扫跑不跑无关。补一条读源码得到的精度：`shouldSweep`
   确实以 `max(2·live, floor)` 为界，但 `checkOwed` **不复查 floor**（它只看 `owe` 标志，
   欠收是内层帧按当时的 `shouldSweep` 记下的）⇒ 少数情形下低于 floor 的区域也会被扫一次。
   这对本设计**无害**：清扫只遍历 rc slab，plain 对象（从 arena bump 出来的）**对清扫不可见**，
   本来就不在它的回收范围里。
4. `bt`（小输入 ≈3 034 个对象 ≈194 KiB < 1 MiB）⇒ 全部 plain ⇒ 收益上限就是关档（税 ≈1.0×）。

**落地前必须先钉的健全性义务（按危险程度排）**
1. **plain 宿主 → rc 目标的跨区边**：今天 `storeRefOwned` 只在 `so`（宿主是 rc 槽）非 0 时登记
   `rsInc`。宿主是 plain 对象时 `ownSlab` 返回 0 ⇒ **登记位缺失** ⇒ 目标所在区域清扫时会把它
   当垃圾放掉（`cross_region*` 两个形状正是这条的实测洞）。必须先决定：宿主证不出时按
   `storeRefUnknown` 那一档**保守处理（目标区域 `noSweep`）**，还是要求 plain 对象不参与跨区。
2. **块级 arena 的寿命**：plain 对象随**块**释放而死，rc 区域要到帧销毁 —— 区域比块活得久
   靠的是"块里的对象全部不可达"这条 checker 不变量。落 plain 之前要用
   `run-switch.sh` 的①s 那一族形状把这条不变量**再验一遍**。
3. **`@overwrite` 格子**（复用同一块存储、`memset` 清零）：与 plain 模式语义冲突 ⇒ 那一档排除。
4. **分片**：区域键含片号 ⇒ 模式天然按片独立；`gate-shard` 三条判据已在位上。
5. **触发/记账**：压力触发必须把 `plainBytes` 算进去，否则长驻循环里 plain 区会无界增长。

**先写红判据再动代码**（顺序照 §步 0 的规矩）：① `run-switch.sh` 的 N vs 4N 一族（长驻循环必须
仍 <1.5×）；② `cross_region_elem` / `struct_copy` / `slot_elem_*` 的跨区登记判据（漏登记 ⇒ 悬垂）；
③ `gate-asan-corpus` 全量；④ `gate-shard`。这四条现在都是绿的，落地过程中不许变红。

**2026-10-13 第三轮：读码 + 实测后**闭档**（Tier 3 这条路在当前契约里不成立）**

把共存期的健全性问题追到底，结论是结构性的：

1. **清扫的活不靠计数，靠标记**：`extc_rc_mark` 从根槽出发，遇到的对象只在**有 slab/类型描述符**
   时才继续下钻。plain 对象没有描述符 ⇒ 标记**走不进它** ⇒ 它手里那条边看不见。
2. 于是 plain 宿主持有的 rc 目标**可能被清扫当垃圾放掉**（悬垂）。共存期只有两个出路：
   （a）在赋值点把目标登记成种子 —— **没有撤销路径**（plain 宿主没有可走的类型描述符）⇒
   登记量只增不减（实测 `rs_inc` 0 → 46 万，四个形状峰值随规模涨，见 CHANGELOG）；
   （b）把目标所在区域标成 `noSweep` —— 有界（一个标志位），但那只区域从此不收垃圾，
   而那正是"需要有界峰值"的那一族形状。
3. 限制到 home 区域救不了：**home 区域恰恰是循环里垃圾积累的地方**（`cyclic_pressure` 的全部
   结点都来自同一只 home 区域的分配点）。
4. 限制到"单区域"程序也救不了：预算耗尽之后，plain 对象与 rc 对象就在**同一只区域**里共存，
   第 2 条的悬垂/无界二选一照旧。

⇒ **Tier 3 需要一条"plain 部分不引用 rc 部分"的健全性证明**（"no mixed references"）才能开。
而整程序证明又要"总分配量有界"，对 `bt` 恰是输入决定的 —— 拿不到。

**2026-10-13 第四轮：把"无计数 + 只靠清扫"在真实形状上量了 —— 内存可行，且运行期本来就支持**

关键发现：**标记与清扫不依赖计数**。`extc_rc_mark` 从根槽 / 已登记的跨区种子出发，靠每个槽自己的
类型描述符（`s->type[i]`）下钻；`extc_rc_sweep` 放掉未标记的占用槽。计数的唯一作用是
**提前回收（计数归零立刻放）**，不是健全性、也不是有界性。于是"去掉计数"不需要重写堆。

**实验**（改生成物副本、不动编译器）：把 `inc`/`dec`/`holdUpd`/`holdUpdSlab` 全部早退（= 只靠清扫），
在四个垃圾密集形状上跑两个规模：

| 形状 | N vs 4N 峰值（基线）| N vs 4N 峰值（无计数）| 语义 |
|---|---|---|---|
| `cyclic_pressure` | 8040 → 7980 KB（1.00×）| 8040 → 8044 KB（1.00×）| 一致 ✓ |
| `cross_region` | 12396 → 14892 KB（1.20×）| 12012 → 14892 KB（1.24×）| 一致 ✓ |
| `struct_copy` | 8748 → 8748 KB（1.00×）| **6636 → 6636 KB（1.00×，低 24%）** | 一致 ✓ |
| `shard_bands` | 4332 → 4396 KB（1.01×）| 4332 → 4332 KB（1.00×）| 一致 ✓ |

⇒ 峰值**仍然有界**（全部 < 1.5×），有的还更低：清扫的触发节奏与计数提前回收大致同频。

**收益（第 20 轮同一批消融给出）**：`bt` mid 的整个赋值路径值 6.33 M（5.7 %）、分配器机器 2.70 M（2.6 %）；
把赋值路径退化到"只写 + 登记"后 mid **1.080× → 1.026×**、in-small **1.686× → 1.505×**
（后者剩余全是分配器机器）。

**2026-10-13 第五轮：机制已落地（开关 `EXTC_RC_NOCOUNT`），只差"翻默认"这一步**

开关 + 判据都就位（见 CHANGELOG 第十刀）：`NOCOUNT=1` 下 `run-switch` **31/0**、ASan 语料
**新增 0**、gaps 集合**逐条相同**，性能双向受益（`bt` mid 1.080→1.057、in-small 1.686→1.608，
垃圾密集形状 −22~−25 %）。新增判据 ①l（`bounded_churn.extc`）把"低于 floor 的 churn"写成
判据并验了牙（无计数 + 不触发清扫 ⇒ 2.70× ⇒ 红）。**翻默认前只剩两件事**：
① 显式处置计数收窄那一套（`rcNoDec` 分析 + `reclaim-elide` 判据）在该模式下的意义；
② 把证据表附在改动说明里。

**原先担心的一条（读码所得）**：触发阈值是 `max(2·live, floor)`。
没有计数 ⇒ `live` 不再因提前回收下降 ⇒ **低于 floor 的churn 区域永远不触发清扫、无界增长**。
⇒ 阈值要改成**固定字节预算 + 滞回**（`bytes > budget`，budget 与 live 无关），并为此
重定 gaps 的"收不掉"基线（那是**能力口径**的变化，要显式重新推导、不是改数字）。

**2026-10-13 残余的两条路都被测量否掉（第十二~十五刀之后）**：`bt` mid 还剩
**赋值路径（值侧查表 + 登记比较）≈ 0.3 %** 与**分配器机器 ≈ 2.4 %**。前者：手工原型只值
−0.32 %（要"返回新分配"的效果汇总，多轮工程换 0.3 %，不值）。后者：`memset` 定长清零
零收益、每个对象的其余工作（slab 选择/空闲链/占用位/类型/字节计数）都必需 ⇒
**剩下的第二档只能来自堆/回收模型的重设计**（见下）。目标口径当前：全输入 1.015×（1.5 %）、
mid 1.037×（3.6 %）。

**替代方向（记为后继设计，不是本轮落地项）**：让**每个对象都带类型描述符**、**彻底去掉计数**
（改为压力触发下的标记-清扫 + 免费链表复用）：赋值路径就退化成一次普通写入（不再有
`ownSlab`/`holdUpd`），分配只多一个小头部。代价是"计数归零立刻回收"这个即时性没了、
垃圾要等一次清扫 —— 峰值行为要重新用 `run-switch` 的 N vs 4N 那族棘轮钉一遍。这是堆的
重设计（多轮工程量），但它同时解决性能与健全性，值得作为下一个大方向。

**2026-10-13 第二轮实现尝试：完整实现后按判据回退（结论：这条路要先解决"兜底必须有界"）**

第二次把 plain 池（区域自管块链 + 全局预算 + `anyRc` 快速分支）真做出来过：`bt` in-small
**1.711× → 1.100×**、mid 1.088× → **1.029×**。但 `run-switch` 的 6 条**峰值/登记量**棘轮变红
（`cyclic_pressure` 19→69 MB，`cross_region` `rs_inc` 40 万 → 160 万且**从不撤销**）。
真因是为 plain 宿主加的 `storeRefOwned` 兜底：它对**同区域**、宿主非 rc 槽的边也登记，
而这种登记没有撤销路径 ⇒ 目标永久当种子 ⇒ 收不掉。**方向保守 ≠ 有界。**
（详细数据与教训见 CHANGELOG 同日条目；本轮已整体回退，基线逐位恢复。）

⇒ **仍然可能成立的唯一形态：只在"单区域"程序上开 plain**（全部 rc 分配落在同一只区域 ⇒
不存在跨区边 ⇒ 兜底不可达；区域预算 + 全局预算给界）。落地前需要：
① 在**分配之前**静态判定"单区域"（`planArenaLevel` 全为 HOME、无分片、无协程 rc 点）；
② 一条判据钉住"多区域程序整片不开"（否则又是一次 6 红的回归）；
③ `run-switch` 已在电池里（`gate-reclaim-switch`），这是这类改动的最低验收线。

**2026-10-13 的第一轮实现尝试（已整体回退；四条结论留给下一次干净落地）**
1. **plain 池要用「区域自己管的 malloc 块链」，不要用帧 arena**：运行期文本（`reclaim.c`）**发射在
   原语块之前**，所以 `extc_rc_alloc` 里够不到 `extc_arena_alloc`（函数指针回调也试过：prim 的
   **发射顺序不等于表顺序**，脚手架同样落在 `extc_arena_alloc` 之前 ⇒ 编译不过）。
   区域自管的块链自给自足，且寿命与区域**逐字相同**（帧 arena 是块级释放，比区域早）。
2. **独立编译那份文本时必须默认关闭**：`tests/reclaim/run.sh` 用 driver 编译运行期文本、断言槽位与
   计数行为；plain 模式一开，低于 floor 的分配就没有 slab，那些断言会红。⇒ 需要一个显式的
   `extc_rc_plainEnable(1)` 开关（由 codegen 发、或由 driver 分别测两种模式）。
3. **开之前要审计"每个 `new` 都有 rc 槽"这一族假设**：`run-switch.sh` 的 ①k（`sweeps>0`）、
   `gate-reclaim-evidence`、gaps 的"收不掉"比值、`promote_place` 的字节断言都可能因为
   "低于 floor 不再扫/不再计数"而改变预期 —— 这些是**行为变化**、不是 bug，但要逐条复核。
4. 跨区登记那条兜底（`else if (sv) rsIncSlab`）与 plain 模式是**同一个包**：只有 plain 宿主出现时
   才需要它，所以一起进退，不留死代码。已实测的悬垂证据见上一轮（宿主实参改 0 ⇒ SIGSEGV）。

### 步 1：运行期加链的数据结构（**不动 codegen**）

- 改什么：`reclaim.c` 的 `RECLAIM_RT[]` 加 `_Thread_local` 槽栈（push/pop/frameMark）、
  `ExtcRcDesc`（`{nptr,off}` + 保守档 `{size}`）、`mark` 的播种从 `roots[]` 扩成"链 + roots"。
- 跑什么：`run-switch.sh`（全绿）与 `cross_region`/`cyclic_pressure` 的 `sweeps`/`rs_*`
  （应逐字不变，因为 codegen 还没用链）。
- 红了说明：运行期新结构影响了既有路径（不该）。

### 步 2：codegen 压链（与旧根表**并存**）

- 改什么：`rcRootDecl` 的产物从"表达式"改成"(base, desc) 压栈"；块边界用栈 mark；
  函数入口/共享尾声压弹；**触发点暂时仍用旧根表**。
- 跑什么：`run-switch.sh` 全量；`hole_c` 7 条 + `gate_asan_corpus.py`。
- 红了说明：压/弹点漏了或早了 ⇒ **真悬垂**（最先红的是 ①i/①j 那两条）。

### 步 3：触发点换种子到链（开 ①）

- 改什么：`rcEmitTrigger` 不再拼根表实参；`rcFrameOwnsArena` 判据退场（`exNew`/`exGencall`/
  `@overwrite` 三处）；`rcEmitOwe`/`rcEmitOwedCheck` 与 `cgImplicitArgs:2053` 的销账点一起删。
- 跑什么：①l（home_cycle r=1）、①p（par_tls）、①i/①j、`gate.sh`（≤ 701 633 044 Ir）。
- 红了说明：链不全（漏了某条声明路径）⇒ C 洞复发；或静态收窄失效 ⇒ 性能账被付回。

### 步 4：登记点铺全（吃掉 ② 与 ③）

| 形状 | 补什么 | 判据 |
|---|---|---|
| `match_payload`（G3）| arm 绑定是 C 局部量：**登记成槽**（保守档也行），作用域 = arm | ①o |
| `pool_ref_slot`（G5）| 池缓冲的引用槽 → `pin/unpin`（覆盖撤销；池 block 出口批量撤销）| ①q |
| `slot_elem_unknown`/`base_call_unknown`（G6）| place 求值一次 + 逐叶 `pin/unpin`，**不再 `noSweep`** | ①m（对照 `slot_elem_known` 必须 1.00×）|
| `coro_frame_cycle`/`ext_frame_cycle`（G2）| 挂起帧的**帧描述符**压链（合成帧字段静态可枚举）| ①n |
| `dyn_payload`（G7）| `rcScanType` 对 `TY_DYN` 的 `RC_NO_REFS` 与逃逸侧口径对齐（`check_escape.c:491-495`）| ①r |

### 步 5：删旧机制（净减代码的那一步）

删 `noSweep`、`owe/owed/checkOwed`、`rcRootsUnreliable` 全部 9 个写入点、
`rcGlobalsUnreliable`、`rcFrameOwnsArena`、`@overwrite` 内联判据；根表实参从运行期接口移除。
- 跑什么：`run-switch.sh` + `gate.sh` + `gate_checkc.py`（全语料 `--check-c`）+ golden（观察项）。
- 红了说明：还有形状依赖被删的出口 ⇒ 回到步 4 补登记点。

### 步 6（可后置）：计数按编译期收窄

`tests/reclaim-elide/` 已有绿判据与红判据；收益 ≤ 10.7%（`reclaim-perf/README.md` §5.1 第 4 条）。
与链不冲突，单独一轮。

---

## 7. RECON.md 里**不成立/与实际相反**的 4 条（子代理逐条复核，我只用复核后的版本）

| 条 | RECON 的主张 | 实际 |
|---|---|---|
| B-2 | `(*id(a)).next` 上 checker **本来可以**命名宿主，保守方向反了 | **不成立**：`fieldSlotKey` 只给 `.next`，根要 `destDerefSym`，而它要求 `*` 后是具名绑定、调用基返 NULL（`check_escape.c:198`、`:994-995`）⇒ checker 同样拿不回 |
| B-6 | 两个装载器"装法不一样（一个 `(void*)(x)`、一个 `&(x)`）" | **不成立**：两者走同一格式化器（`codegen.c:3164/3167`）；`&(x)` 是原型新增的向量 |
| B-9 | 销账点与记账点"可能读到不同的 `inMain/parTls`" | **不成立**：两者按函数设置、帧内不变；真正会不同的是 `lvl` 实参；真正的洞是 `@overwrite` 的**第三份**内联判据（`codegen.c:5254`）|
| B-11 | `RC_HOST_LOCAL` 在跨帧时失效 | **机制不可达**：`LOCAL` 只由 `EX_IDENT` 终止给出（`:4980-4982`），被调者无法用 `EX_IDENT` 命名外层帧局部 |

另有三条**部分成立但需要修正**：B-1（规则是 `shouldSweep:706` 与 `checkOwed:756` 的**两份**
`noSweep` 判据，`maybeCollect:732` 只是转调，且 `checkOwed` **不过压力线**）；
B-7（`rcRootsUnreliable` 不是 3 个而是 **9 个**写入点）；B-3（区域认领在 `codegen.c:8823`，
RECON 引的 `8700-8717` 是别的 pass）。

---

## 8. 收益 / 代价 / 未查清

**收益（有实测锚的）**：7 个红形状的界（§2 表）——每个都有一条现成的 ①l~①s 可接；
`cyclic_pressure` 的每触发物化与 4096 上限消失（`reclaim-perf` 已实测入口守卫值 −56.5%，
链与它不冲突：守卫保留）。
另一档**已经落地**的性能账（与链无关，独立成立）：`owns` 守卫的登记表查询 O(1) 化（命中缓存 +
未命中回落二分）+ **赋值路径上每个指针只查一次登记表**（`*Slab` 记账变体、命中路径去冗余比对）
+ **热路径小助手 `static inline`**，
N=2e5 `cyclic_pressure` `1 169 388 408 → 510 475 244 → 404 358 444 → 339 186 860 → **308 485 778** Ir`；
真实形状 `rebuild` `420 → 330 ms`、`bt` `1120 → 570–610 ms` ——
**后两刀的墙钟与 Ir 脱钩**：三个形状是内存/分配受限，减指令不必然变快（这一点写在 §10）。
判据 `gate.sh` 的线收到 324 000 000。细节与负结果见
[`tests/reclaim-perf/README.md`](../reclaim-perf/README.md) §5.1 第 2 行与 §9。

**净代码量（【推断】，只有基数是实测）**：

| 步 | 新增 | 删除 | 净 |
|---|---|---|---|
| 步 1 运行期槽栈 + `ExtcRcDesc` | ~60 行运行期文本 | — | +60 |
| 步 2 codegen 压/弹 + mark | ~80 | ~20（`rcRootLeaves` 的字符串形态）| +60 |
| 步 3 触发换种子 | ~10 | `rcEmitOwe`/`rcEmitOwedCheck`/`rcFrameOwnsArena`/销账点 ≈ **90** | **−80** |
| 步 4 登记点 + pin/unpin | ~60（pin/unpin + 描述符保守档）| `storeRefUnknown`/`storeStructCons` 的 `noSweep` 分支 ≈ 25 | +35 |
| 步 5 删旧机制 | — | `rcRootsUnreliable` 9 处 + `noSweep` 字段/读点 + 全局档 ≈ **120** | **−120** |
| 级 B（可后置）| 一张共享枚举器 ~80 | 3 份重复枚举器 ≈ **250** | **−170** |

⇒ **级 A 全程 ≈ 净 −100 行；级 B 再 −170 行**。这条与 `RECON.md` §D.4 的估算（候选 1 净 +161、
候选 2 才净减）**方向一致但更省**，差别的来源是：把"每帧 4096 地址数组"换成"每帧 K 条描述符"
之后，原型里最大的那笔新增（帧数组 + 逐槽登记循环）不存在了。**这是推断，不是实测**：
`RECON.md` 的 +161 是**原型实测**出来的，本表是逐点估的。


**代价（`reclaim-rootstack/RECON.md` 实测的输入，我未重跑）**：要建表的帧 139 / 表项 344、
压弹落在 162/4922 = 3.3% 的调用点、223 个标量局部要取地址（去重后 50）。
**本设计把"每帧 `void *[4096]`"（原型的性能悬崖：`cyclic_pressure` N=20000 > 240 s）换成
"每帧 K 条 (base,desc)"（K 中位 2、最大 7）+ 一个共享栈，所以悬崖的两个来源都不在**——
但这条是**【推断】**，原型的悬崖只在 RECON 里量过，**没有**在链描述符形态上复测。

**未查清 / 风险**：
1. **链描述符形态的端到端性能没测**：只有 §5.2 的功能证明与 §8 的 K 分布输入。
   做之前先做一个 20 行的微原型（`cyclic_pressure` + `home_cycle`），比 `gate.sh` 基线。
2. **取地址的代价**：223 个标量局部会被钉到栈上（`RECON §2.2(d)`）；`-O1` 下的真实影响未测。
3. **pin 的界**：`pin/unpin` 有界于"非帧引用槽个数"是**推断**（§3.3），要拿 `slot_elem_unknown`
   与 `pool_ref_slot` 各做一个原型量 4N 比。
4. **挂起协程帧的链**：帧描述符怎么与"当前线程的链"接（挂起时压、恢复时弹，还是任务自带链头）
   未设计到函数级；`ext` 帧交给 domain 之后的归属也未查。
5. **TLS 交叉线程**：今天 worker 的分配被改道 `extc_tls_arena`，所以"main 的家 arena 给 worker"
   碰不到；**未实测**，且一旦允许就是共享区域的跨线程根问题。
6. **`rcScanType` 对 `TY_ENUM` 无条件保守**：改成逐字保守档之后影响多少语料，未量。
7. **本设计的排序假设**：步 3 就删 `rcFrameOwnsArena`/owe 是最激进的；若步 2/3 的判据出现
   无法解释的红，**退回"链与 owe 并存"**（链只用于 ① 的两条形状），把 ① 与 ②/③ 的收益分开拿。

---

## 9. 复跑

```sh
# 缺口矩阵（我复跑过；恒 exit 0，数字在 out/scale.tsv）
tests/reclaim-gaps/run-gaps.sh
# 判据现状（有牙的那条：owed_home 的 sweeps=20）
tests/reclaim-shapes/run-switch.sh
# 性能闸门（入口守卫后的基线 ≤ 701 633 044 Ir）
tests/reclaim-perf/gate.sh

# 本文 §5 的两个实验（只碰 /tmp）
mkdir -p /tmp/g1proto && cd /tmp/g1proto
EXTC_RECLAIM=1 <repo>/build/extc -w --no-line-map <repo>/tests/reclaim-gaps/shapes/home_cycle.extc -o base.c
cc -std=c11 -fwrapv -O1 -o base base.c
# 5.1：在 `build(per)` 之后补一条 {... extc_rc_checkOwed(区域, (void*[]){keep}, 1); }
# 5.2：把两个 owe 换成 maybeCollect(区域, {keep, 发布过的祖先槽})，main 在下一轮调用前发布 keep
/usr/bin/time -v ./base 1 640000          # 18348 KB
python3 <repo>/tools/rc-pause-probe.py base.c > probe.c   # 探针：PROBE sweeps=0
```

> 跑 `run-gaps.sh` 会**覆盖** `tests/reclaim-gaps/out/*.tsv`（入库文件）。本轮跑完已
> `git checkout -- tests/reclaim-gaps/out/{matrix,scale,raw}.*` 还原；复跑前先记下这一点。
