# `tests/reclaim-rootstack/` —— **运行期根栈**的落点、代价与收益（只勘察，不实现）

对象：分支 `main`（HEAD `a70dfad`，已含 `19fd567` = C 洞的修法 `rcFrameOwnsArena`）。
任务：回答「现在根表长在哪 / 要加根栈得动哪几处 / 四种方案的代价 / 110 个被移除触发的分类 /
目标数字怎么复现 / 边界情形 / 静态能省多少 / 风险面 / 做不做」。

**口径**：每一节都标注【实测】（有可复跑命令与原始输出，落在 `out/`）或【推断】（从源码/生成物
读出来的，没有端到端实测）。没查清的直接写**未查清**。

复跑：

```sh
tests/reclaim-rootstack/repro.sh            # 全部（要先有 build/extc；修前编译器自动 git archive 9305b17 + make）
tests/reclaim-rootstack/repro.sh scan       # 单节：scan|cost|peak|target|gates
```

修前的编译器由 `git archive 9305b17 | tar -x -C /tmp/rs-prefix && make` 得到（**不动工作树**）。
本目录只新增文件；`src/`、`tests/` 既有文件、`docs/`、`tools/` 一个字节没改。

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| 落点地图 | 根表**建在 6 处**（状态 / 每函数重置 / 声明登记 3 个入口 / 登记实现 / 块边界 / 全局），**消费只有 1 处**（`rcEmitTrigger`），另有 3 个分配点入口。加根栈要动 **10 处**（§1.2） |
| 方案② 代价 | 根表槽 **99.6% 是本帧局部量**（4734/4754）、0.4% 形参、**0 全局**【实测】；但真正要建表的只有 **139 帧 / 344 项**（K 最大 7）【实测】；模型增 ≈**622 行 / 19.9 KB = 生成物的 0.036%**【实测输入 + 模型】；压/弹只落在 **162/4922 = 3.3%** 的调用点【实测】 |
| 110 个触发分类 | 136 个被移除触发点：**home+非 main 129 个（根栈能救）** + **owcell 1 个（能救）** + **home+main 6 个（没有调用者，根栈救不了）** + TLS 0 + 协程 0【实测】。程序级：**104/110 可救、6/110 不可救** |
| 收益 | 104 个程序"能恢复清扫能力"，但逐个量峰值只有 **`bench/stress/stress.extc` 一个**有可测回归（10.60 MiB → 22.98 MiB）【实测】 |
| 目标数字 | 10 924 KB / 23 532 KB（`/usr/bin/time -v` 的 Maximum resident set size，跑生成物二进制）⇒ 10.92 MB / 23.53 MB【实测】；README 的 10.92 / 23.60 复现上了 |
| 边界 | 递归**要每帧一项**（现状已经是每帧求值，且有每帧入口钩子）；协程/`ext`/闭包 spawn **必须继续排除**，且要新增"根不可靠"帧标记；TLS 边界在 worker trampoline；入口守卫与根栈不冲突但压栈要无条件（或按 §5 判据收窄到 3.3%） |
| 静态省掉 | 细判据（被调者子树里真有 home 分配）省 **95.9%** 的调用点；现成的粗判据（`planUsesHome`）省 **91.4%**【实测】 |
| 建议 | **不做整条根栈**；要做只做 §7 的子集（首选 A：`main` 特例 ~5 行；其次 B：欠收标志 + 拥有者返回后收一次，能拿回 `stress` 那 12 MB 且不需要根栈）；根栈本体（C）列为"有设计验证再做" |

---

## 1. 落点地图

### 1.1 现在：根表建在哪、建了什么、在哪被消费【实测：源码 + 生成物】

**建（6 处）**

| # | 位置 | 建了什么 |
|---|---|---|
| 1 | `src/back/codegen.c:420-424` | 状态：`rcRoots`（`const char*` 向量 = 活着的引用槽**表达式**）、`rcRootMarks`（每层块进入时的 `rcRoots.len`）、`rcGlobalRoots`、`rcRootsUnreliable` |
| 2 | `src/back/codegen.c:6505-6512` | **每函数重置**：`rcRoots.len = 0`；`rcRootsUnreliable = needReclaim && (coroFunc \|\| rcGlobalsUnreliable)`；每个形参 `rcRootDecl` |
| 3 | `src/back/codegen.c:5180` / `5193` / `5230` | **声明登记**：`@overwrite` 绑定 / `var q = f()?` / 普通 `var\|let`，三条都调 `rcRootDecl` |
| 4 | `src/back/codegen.c:3341`（`rcRootDecl`）→ `:3233`（`rcRootLeaves`） | 类型扫描：`REF` 叶子把 C 左值表达式 push 进 `rcRoots`；定长数组展开成 `X.data[i]`、结构体展开成 `X.f`；枚举/泛型实参带引用 ⇒ `rcRootsUnreliable = true` |
| 5 | `src/back/codegen.c:4713` / `:4745` | **块边界**：进入块 `vecPush(&rcRootMarks) = rcRoots.len`，离开截回去（`genBlockBody`） |
| 6 | `src/back/codegen.c:9320` | 文件级全局引用槽 push 进 `rcGlobalRoots`（每个触发点都算根） |

**消费（1 处 + 3 个分配点入口）**

| # | 位置 | 干了什么 |
|---|---|---|
| 1 | `src/back/codegen.c:3134` `rcEmitTrigger` | 把 `rcGlobalRoots + rcRoots` 拼成**复合字面量** `(void *[]){ … }`（`n==0` 时给 `(void *const *)0`），并发射入口守卫：`{ ExtcRcRegion *g = extc_rc_regionOf(区, lvl); if (extc_rc_shouldSweep(g)) extc_rc_maybeCollect(g, 表, n); }`（`:3159-3161`）；分片版在 `:3176-3187` |
| 2 | `src/back/codegen.c:3208` `rcAllocCore` | 先 `rcEmitTrigger`（`:3212`）再发射 `extc_rc_alloc(extc_rc_regionOf(…), …)`（片号 = `rcSite++ % N`） |
| 3 | 三个分配点 | `exNew` `new T`（`:3385`）、`alloc<T>(n)`（`:3509`）、`@overwrite`（`:5134`，区域键是 `(void *)__owcN->home`、`own = loc && !rcFuncUsesHome`，`:5133`） |
| 4 | 运行期 | `reclaim.c:729 extc_rc_maybeCollect` → `:713 extc_rc_collect` → `:628 extc_rc_mark(r, roots, nroots)` 播种 + `:674 extc_rc_sweep` 扫 `occ & ~mark` |
| 5 | 区域身份 | `reclaim.c:572 extc_rc_regionNew(rid)` + `codegen.c:8700-8717` 的按 key 认领表（key = arena 地址 + 片号）；帧销毁处 `cgReclaimDropArena`（`codegen.c:4931`） |
| 6 | 触发判据 | `reclaim.c:703 extc_rc_shouldSweep`（`noSweep` / `live*2` / `floor` / 自学习 `scale`）；`EXTC_RC_STATS` 在 `:457-468` |

关键：**根表不是一个常驻结构**，它是每个触发点上当场物化的一张复合字面量（值语义），
所以"给根栈留接口"不等于"把这张表搬到运行期"，而是要在 `roots[]` 这个入口上合成
"静态表 + 动态根栈"两张来源（`extc_rc_mark` 的播种循环只有一处）。

### 1.2 要加运行期根栈，得动的点（逐个列）

| # | 点 | 现状（行号） | 要做的改动 |
|---|---|---|---|
| 1 | 运行期文本 | `src/back/reclaim.c` 的 `RECLAIM_RT[]`（`:16-744`） | 新增：`extc_rc_rootsPush/Pop`（线程本地栈）、`extc_rc_rootsMarkUnreliable()`（不可靠帧标记）、`extc_rc_mark` 的种子 = 静态 `roots[]` + 根栈；`regionNew` 附近的区域归属校验 |
| 2 | 触发闸门 | `codegen.c:3105 rcFrameOwnsArena`（`parTls`/`coroFunc`/`ARENA_HOME` ⇒ false） | 放宽成"这只区域的种子完整才触发"：本帧拥有 **或** 根栈覆盖得到；`parTls` 的 TLS 区域要额外按"栈上全是本 worker 的帧"判断 |
| 3 | 触发发射 | `codegen.c:3134 rcEmitTrigger` / `:3208 rcAllocCore` | `frameOwned` 的来源换成新判据；可选：把根表实参换成"运行时根栈 + 本帧静态表"的合成入口 |
| 4 | 帧进入 | `codegen.c:6505-6512`（`genFunc` 序言，参数登记之后） | 建本帧的槽地址表 / 帧描述符并注册（push）。递归 ⇒ **每帧一次**，不能每函数一次 |
| 5 | 帧退出 | `codegen.c:6599` 的共享尾声 `__extc_ret:`（早退都 `goto` 到这里，`cgReturn` `:4062`） | 弹栈。`noArena` 的直返分支（`:4067-4074`）只在该函数可能 push 时才要管；按现有 plan，能到达 home 分配的函数一定 `noArena=false`（`:6349`） |
| 6 | 调用点 | `codegen.c:2032 cgImplicitArgs`（隐藏 home 实参的**唯一**发射点）/ `:4239 homeArg` | 在传 home 的调用点上前压/后弹（或不做调用点、改成帧入口注册 + 后代标记时查栈） |
| 7 | 不可靠根表 | `codegen.c:4997` / `:5036` / `:6506`（协程 spawn / 协程帧 / 协程 step 都置 `rcRootsUnreliable`） | 变成**帧级标记**并压进根栈：让后代知道"上层有帧的根看不见" ⇒ 共享区域不许扫 |
| 8 | plan 层 | `src/plan/plan.c:1461 planUsesHome`（"读 home 或往下传"，偏粗）/ `:1473 planMayUseArena` | 新增细判据"被调者**子树里真有** home/共享区域分配"（§5 需要它把压栈收窄到 3.3%） |
| 9 | TLS 边界 | `codegen.c:10133-10160` 的运行期文本 `extc_par_worker`（设 `extc_tls_arena` → 调 `j->fn`） | 根栈必须 `_Thread_local`；TLS 的"栈边界"就是这里 |
| 10 | 区域释放 | `codegen.c:4931 cgReclaimDropArena` / `reclaim.c:585 extc_rc_regionFree` | 帧销毁时根栈项必须先弹干净，且区域释放要能容忍栈里还挂着指向它的槽（或者按 owner 校验） |

---

## 2. 方案对比（①/②/③/④）

### ④ `sweep` 遇 `hold != 0` 当活的 —— **为什么一定是错的**（不是候选）

* 环垃圾的 `hold` 恒 ≥ 1：`cyclic_pressure` 的退役链每个节点都有 `next/prev` 互指，
  `extc_rc_holdUpd` 的 `dec` 一次都到不了 0（`src/back/reclaim.c:418-430`）。
* 实测反证（`tests/reclaim-shapes/README.md`「数字 2」+ `run-evidence.sh`）：把清扫关掉
  （`-DEXTC_RC_FLOOR=1 TiB`）后 `cyclic_pressure` 的峰值从 7.61 MB 变成 **17.92 MB(N) / 63.54 MB(4N) = 3.55×**
  —— 环一个也收不掉。若 `sweep` 再把 `hold>0` 当活的，这一档就变成"永远"：清扫退化成空转。
* `hole_c` 的探针反过来给出这一档的另一面：`freed=65537 freed_held=65537 maxhold=2`
  —— 被误放的对象**恰好**都是 `hold>0`。所以"用 hold 当守卫"是把 C 洞换成一个更大的洞。

### ① 调用前把调用者根表**按值**压栈、返回弹栈

* 代价：每个"可能触 home"的调用点都要当场物化整张表。这与 `2bb4a1d` 之前每个分配点物化
  根表的形状**同一笔钱**：`tests/reclaim-perf/README.md` 的归因是根表物化占 **56.1%**（N=8e5），
  入口守卫把它压到 **−56.3%**（N=2e5，本轮复测 510 475 244 Ir ≤ 701 633 044）。① 等于把这笔钱
  从"分配点"搬到"调用点"：分配点的表只在 `shouldSweep` 时才物化，而调用点的表**不能**这么省
  （被调者才知道要不要扫）⇒ 收回 −56.3% 的一部分甚至全部。
* 风险：最低（就是现有字面量换个位置），健全性最好论证（值语义、没有陈旧槽问题）。
* 量化：见 §2.2 的"要压的调用点"——只有 3.3% 的调用点，但每一个都要付整表；
  `cyclic_pressure` 的表有 1412 项（根表字面量 128 KB 一份）⇒ 若它真有 home 调用，gate.sh 立刻红。

### ② 每帧建一张**引用槽地址表**（`&local` 的数组），调用点只压/弹一个指针 —— **Lead 首推**

* 代价分两块：**取地址**（引用局部量不能留在寄存器）与**每帧建一次表**。量化见 §2.2。
* 风险（这才是真代价，不在指令数）：
  1. 表的**生命周期纪律**：根栈要看见"整个调用栈的活引用"，所以表必须是**帧级并集**
     （帧里所有活着的引用槽），而块进出会改变活集合。现在 codegen 的做法是"按块截回 `rcRoots`"，
     **不在块退出清零引用槽**。若表用并集且不清零，`mark` 会读到陈旧指针 ⇒ 保守留住垃圾
     （方向安全、界变差）；若按作用域维护表，则每次块进出要改表（新纪律 + 新状态）。
  2. 取地址不可回退：被取地址的标量局部必须落栈，编译期无法再决定"留在寄存器"。
  3. 与不可靠帧的交互：协程 step（`rcRootsUnreliable`，`codegen.c:6506`）、`ext`/协程 spawn
     （`:4997`/`:5036`）、枚举负载绑定（`:5366`）这类帧的表**拼不出来**，必须压
     "根不可靠"标记，否则重新打开 home 触发就是把 C 洞请回来。
     （闭包没有专门分支：环境是函数的形参，走 `rcRootDecl` 的普通形参路径 —— 叶子可枚举就
     登记、枚举不出来就整体置不可靠。）
* 变量：把表做成**静态帧描述符**（`static const {offset}` + 帧基址，压一条指针），
  则帧入口不需要逐槽物化，取地址的代价仍在、但每条 push 都是 O(1) 且文本增量≈0。

### ③ 把触发**推迟到拥有它的那一帧**（被调者只置"欠一次收集"）

* 形状：`rcFrameOwnsArena` 为假时不再什么都不做，而是 `extc_rc_oweCollect(region)`；
  拥有者帧在自己的分配点或帧销毁前统一收取。拥有者帧的静态根表对它**天然完整**
  （这正是 `rcFrameOwnsArena` 的安全侧依据：外层拿不到指向本帧块 arena 的引用）。
* **谁是拥有者**：`ARENA_HOME` 区域是"家 arena 一路传下来的同一只"，它的所有者是**链根**，
  而在今日 codegen 里链根只能是 `main`（`main` 的 home 就是自己的 `&__extc_a[1]`，
  `codegen.c:6481-6484`）—— 正好覆盖 §3 那 104 个程序的形状。
* 代价最低：不动根表、不动运行期标记路径；健全性最好论证。
* 收益取决于**拥有者什么时候收**：
  * 变体 ③a（拥有者自己的分配点 / 帧销毁）：`hole_c`/`caller_frame`/`persist_nodes` 这一档
    拥有者 `main` **没有**受管辖的分配点 ⇒ 只能等到帧销毁，而 `main` 的帧销毁 = 进程结束
    ⇒ 峰值一点不回。`stress` 也救不回（它的峰值在 300 轮循环中间）。
  * 变体 ③b（**拥有者在调用返回后查一次欠收标志**，用自己此刻的静态根表收）：
    这个时候栈上只有拥有者及其祖先（深帧已经退出），静态根表就是完整种子 ⇒ 健全；
    `stress` 的 12 MB 能回来（`buildList` 返回后 `main` 收）。
    代价是一条调用点检查 + 一个区域级标志（多线程要 TLS）。
* 与任务描述不同的地方：任务把它归到"被调者置标志"，但**被调者正是看不见完整种子的那一方**，
  所以这个标志只能是"请求"，真正动手的必须是拥有者 —— 且必须是 ③b 那种"拥有者还活着、
  深帧已退出"的时刻，否则种子仍然不完整。

### 2.1 方案对照表

| 方案 | 改动面 | 健全性论证 | 回收能力 | 主要风险 |
|---|---|---|---|---|
| ① 按值压 | 调用点（201 个 home 可达） | 最简单（值语义） | 与修前一样 | 每次调用物化整表 ⇒ 把 −56.3% 付回去 |
| ② 地址表 + 压指针 | 帧进入/退出 + 调用点 + 新运行期结构 | 中（种子完整；要防陈旧槽与不可靠帧） | 与修前一样 | 作用域/清零纪律、取地址钉栈、不可靠帧标记 |
| ③ 推到拥有者帧 | 运行期 + 一个"欠收"标志 + 拥有者侧的检查点（③b 在调用返回后） | 最好（种子就是拥有者自己的静态表） | ③a 只救"拥有者有分配点"的形状，其余等到帧销毁；③b 能救 `stress` 这一档 | ③a 对 `stress`/`hole_c` 无效；③b 要新调用点检查与线程本地标志 |
| ④ hold 守卫 | 1 行 | **错**（环 `hold`≥1） | 废掉清扫 | 换一个更大的洞 |

### 2.2 方案②的量化【实测】

**口径**：802 份"两版都编得过"的 `.extc` 用**修前的编译器**生成 C（`EXTC_RECLAIM=1`，
`tests/reclaim-rootstack/gen-corpus.sh`），再用 `cost-model.py` 静态清点。
生成物里触发点的形状：`… regionOf(区域, lvl) … maybeCollect(…, (void *[]){ (void *)(槽), … }, n) …`。

**(a) 根表槽按来源分类**（"需要取地址"的那一档）

| 口径 | 合计 | 本帧局部 | 形参 | 全局 | 未分类 |
|---|---|---|---|---|---|
| **出现次数**（每张表里每个槽各算一次）| 4754 | **4734（99.6%）** | 20（0.4%） | **0** | 0 |
| **去重**（帧 × 槽表达式）| 1686 | 1669（99.0%） | 17（1.0%） | 0 | 0 |

分组（`out/cost-model-*.txt`）：`tests/reclaim-shapes` 16 份 = 4511 次（4509 局部 / 2 形参），
去重 1471；`bench` 78 份 = 11 次（7 局部 / 4 形参），去重 10。

⇒ 任务里的前提"有多少是本帧局部量（要取地址）、多少是形参/全局（不要）"：
**按根表口径，几乎全是本帧局部量**；形参只占 0.4%，全局 0。

**(b) 真正要建表的量**（有引用槽 + 调用"可能触共享区域"的函数）

用 `(void **)&X` 扫描（codegen 每建一个引用槽都会发这条，含**从不进任何根表**的槽）：

* 全语料帧内声明的引用槽 **1039** 个（全是局部；形参走 `rcRootDecl` 不取地址，这套扫描数不到，
  但根表口径里形参只有 20 次 ⇒ 量级可忽略）。
* 其中"有引用槽、且调用可能触 home 的函数"的帧 = **139 帧 / 344 项**。
* K（每帧去重槽数）分布：`{1:22, 2:74, 3:12, 4:22, 5:6, 6:1, 7:2}`，**最大 7**，中位数 2。
* 关键形状：`cyclic_pressure` 的 `main` 有 1412 项根表，却**一个 home 可达调用点都没有**
  ⇒ 它根本不需要建表（这也解释了为什么 gate.sh 不会被 ② 弄红）。

**(c) 取地址对生成物规模的影响（行/字节）**

模型（帧入口 `K` 条 `__extc_rs[k]=&slot;` + 1 条声明 + 2 条 push/pop；每条 32 B）：

| 语料 | 生成物 | 建表帧 | 表项 | 压/弹调用点 | 模型增 | 占比 |
|---|---|---|---|---|---|---|
| 全部 802 份 | 55.12 MB | 139 | 344 | 162 | **≈622 行 / 19.9 KB** | **0.036%** |
| `tests/reclaim-shapes` | 1.16 MB | 3 | 6 | 3 | ≈12 行 / 384 B | 0.033% |
| `bench` | 6.25 MB | 4 | 7 | 6 | ≈15 行 / 480 B | 0.008% |
| `tests/escape-fuzz` | 8.68 MB | 104 | 285 | 122 | ≈493 行 / 15.8 KB | 0.182% |

对照：现有触发点根表**字面量**合计 134 883 B（0.24%）；在 `tests/reclaim-shapes` 那 16 份里占
**11.11%**（几乎全是 `cyclic_pressure` main 那一条 1412 项的表）。

**(d) "不能留在寄存器"的真实打击面**：去重后的 1686 个根表槽里 **1446 个是 `X.data[i]` 这类
数组元素**（本来就在内存里），只有 **223 个（13%）是标量局部**；按表达式去重只有 **50 个**
（最常见 `head`×53 帧、`a`×33、`cur`×27）。⇒ 取地址的代价是**有界且小**的；真正的成本是
(b) 里那 344 项的表维护纪律（作用域/陈旧槽），不是寄存器压力。

---

## 3. 收益估算：110 个被移除触发的分类【实测】

### 3.1 普查复现（口径与 `hole_c/README.md` 的 110 一致）

`out/scan-triggers.txt`（`scan-triggers.py --pre /tmp/rs-prefix/build/extc --post build/extc`）：

```
语料目录       tests,bench,examples,stdlib
文件数         1221
两版都编过     802
修前有触发的程序 208
触发全被移除的程序 110（含触发点 154 个）    ← 154 = 全移除(136) + 部分移除(18)
部分移除的程序  18
触发点总数     修前 329 → 修后 175
```

精确对上 `hole_c/README.md` 的 **1221 / 208 / 110**。

### 3.2 136 个"被移除触发点"逐个分类（`out/trigger-sites.csv`）

分类键 = 修前触发点的**区域表达式** × **所在函数**（是不是 `main`、是不是自递归）：

| 区域形状 | 所在函数 | 触发点数 | 根栈能救回？ | 判定依据 |
|---|---|---|---|---|
| `(*__extc_home)` | 非 main、非递归 | **126** | ✅ | 函数签名带 `__extc_home` 形参 ⇒ home 由**调用者**传入 ⇒ 外层帧存在；触发时它的静态表只覆盖自己那一帧，根栈补的正是外层帧 |
| `(*__extc_home)` | 自递归 | **3** | ✅ | 同上，每帧一项（`hole_c.build`、`bench/heavy/bt.bottomUp`、`bench/bigmatrix/src/bt.bottomUp`）|
| `(void *)__owcN->home` | 非 main | **1** | ✅ | `tests/asan/overwrite-callee.extc` 的 `stash`：`own = loc && !rcFuncUsesHome`（`codegen.c:5133`）为假 ⇒ 格子的 home 是**调用者**的家 arena |
| `(*__extc_home)` | **main** | **6** | ❌ | main 的 home 就是自己的 `&__extc_a[1]`（`codegen.c:6481-6484`），**没有调用者**；根栈补不出东西。这 6 个全在 `tests/escape-fuzz/`，修法是另一条：放开 main 特例 |
| `(*extc_tls_arena)` | — | **0** | — | 全语料没有能编译的正向 `parallel::run` 程序（唯一源码 `tests/errors/parallel_run_noargs.extc` 是负例）；生成物里 `extc_tls_arena` 出现 0 次 |
| 协程帧 `f->arenaN` | — | **0** | — | 21 个含协程的程序修前触发点就是 **0**（`rcRootsUnreliable`，`codegen.c:6506`）；`ext` 任务同理（`codegen.c:4997`/`5036`） |

**程序级**（`out/trigger-sites.csv` 聚合）：

| 程序的移除形状 | 程序数 | 根栈能救 |
|---|---|---|
| 只有"非 main、非递归"的 home 触发 | 100 | ✅ |
| 只有递归 home 触发 | 3 | ✅ |
| 只有 owcell 触发 | 1 | ✅ |
| 只有 main 的 home 触发 | 6 | ❌ |
| **合计** | **110** | **104 / 110** |

补充实测两条：

* 130 个非 main 触发点所在函数，在生成物里**全部是 `static`** ⇒ 不存在"C 侧调用者不会压栈"的例外。
* 每程序被移除的触发点数：1 个的 88 个程序，2 个的 18 个，3 个的 4 个。
* 110 个程序的分布：`tests/escape-fuzz` 83、`tests/arena-promoted` 16、`tests/reclaim-shapes` 3、
  `examples` 2、`bench/{stress,heavy,bigmatrix}` 各 1、`tests/{asan,attacks,reclaim-elide}` 各 1。

### 3.3 收益有多大：110 个程序逐个量峰值【实测】

`out/peak-regression.txt`（`peak-regression.py`，修前/修后各生成一份 C、同一把尺子
`cc -std=c11 -fwrapv -O1`、`/usr/bin/time -v` 取 Maximum resident set size）：

* 107/110 两侧都 rc=0 且量到峰值；**|Δ| ≥ 0.1 MiB 的只有 1 个**：
  `bench/stress/stress.extc`：**10.605 MiB → 22.980 MiB（Δ = +12.375 MiB）**。
* 其余 106 个 |Δ| ≤ 0.0625 MiB（进程底噪量级）。
* 3 个不可比：`bench/heavy/bt.extc`（修前 rc=139）、`hole_c.extc`（修前 rc=139）、
  `caller_frame.extc`（修前 rc=124）——正是 C 洞本身。

⇒ **收益面 104 个程序，可测收益只落在 1 个程序上**（与 `hole_c/README.md` §4.4 一致）。

### 3.4 目标数字怎么复现【实测】

`out/target-stress.txt`：

```sh
EXTC_RECLAIM=1 /tmp/rs-prefix/build/extc -I stdlib bench/stress/stress.extc -o /tmp/stress-pre.c
EXTC_RECLAIM=1 ./build/extc              -I stdlib bench/stress/stress.extc -o /tmp/stress-post.c
cc -std=c11 -fwrapv -O1 /tmp/stress-pre.c  -o /tmp/stress-pre
cc -std=c11 -fwrapv -O1 /tmp/stress-post.c -o /tmp/stress-post
/usr/bin/time -v /tmp/stress-pre    # Maximum resident set size
/usr/bin/time -v /tmp/stress-post
```

实测（各 3 次）：

| 二进制 | KB | MB（10⁶） | MiB |
|---|---|---|---|
| 修前（`9305b17`） | 10 860 / 10 924 / 10 860 | **10.86–10.92** | 10.60–10.67 |
| 修后（HEAD `a70dfad`） | 23 532 / 23 532 / 23 532 | **23.53** | 22.98 |

⇒ README 的 **10.92 MB → 23.60 MB** 复现上了（±0.3%）。口径确认：**落在生成物二进制**上的
`/usr/bin/time -v` 的 `Maximum resident set size`，不是 `extc --run`（那计的是 extc 自己）。

两个附带实测（都同一份形状、只换运行期、四行输出逐字一致）：

* `EXTC_RECLAIM=0`（关掉回收）修后编译器：**16 620 KB = 16.23 MiB**。
  ⇒ 修后（23.53 MB）比"整块关掉回收"还差 **≈42%**：损失不是"退化成无回收"，而是
  rc slab 攒在**没人扫的 home 区域**里。这一条对"要不要修"是加权的。
* `stress` 修前生成物里 `extc_rc_maybeCollect(__extc_rcg…)` 只有 **2** 处
  （都在 `buildList`，`regionOf((void *)&((*__extc_home)), -1)`），不是 README 说的 4 处
  —— 口径差异记在这里，数字不影响结论。

---

## 4. 边界情形

| 情形 | 结论 | 依据（行号 / 实测） |
|---|---|---|
| **递归** | 根栈必须**每帧一项**（不是每函数一项）；现状**支持**这个语义：静态根表存的是**表达式**，触发时按当前帧求值；每帧入口已有钩子 `extc_rec_enter`（发射 `codegen.c:6502`、原语 `:8413`、递减 `:4047` / `:4069`）；帧退出有**唯一共享尾声** `__extc_ret:`（`:6599`），早退都 `goto` 到这里 ⇒ push/pop 各一处就够 | 实测 `hole_c`（深度 16，`build` 的触发根表为空）+ `persist_nodes`（`update` 根表 `{z, prev}`）+ `bt.bottomUp` 都是递归档；`out/trigger-sites.csv` 里 `is_recursive=yes` 3 条 |
| **协程** | 现有 `rcRootsUnreliable` **已足够**挡住洞（挂起后帧不在 C 栈上，静态枚举不出帧 ⇒ 触发一个都不发）。但**根栈要重新打开共享区域触发时，这个挡法不够**：要新增"根不可靠"的**帧级标记**，让不可靠帧的后代也不许扫共享区域 | `codegen.c:6506`（协程 step）、`:4997`/`:5036`（spawn）；实测 21 个含协程的程序修前触发 **0** |
| **`parTls` / TLS** | 整条 worker 调用链共用 `extc_tls_arena` ⇒ 根栈边界在 `extc_par_worker`：设 `extc_tls_arena` 之后、第一次 `j->fn` 之前；根栈必须**线程本地**（父子线程不共享）。**未实测**（没有正向语料） | `codegen.c:10133-10160`（worker 运行期文本）、`:10142-10147`（设 TLS）、`:6716`（worker 签名收 `extc_tls_arena`）、`:3106`（`parTls ⇒ 不触发`）；实测全语料 `extc_tls_arena` 出现 **0** 次 |
| **`ext`（spawn）** | 与协程同一档：`ext f(x)` 要求被调是协程；帧在 spawn 处当**普通局部量**造好后交给 domain，之后不在 C 栈上 ⇒ `rcRootsUnreliable`、触发 0。根栈**不能**覆盖它 | `codegen.c:2386-2393`（"build the frame as a plain local"）、`:4997`；实测 3 个能编的 `tests/ext/*` 触发 0、`examples/task-place-release` 等含 task 的程序触发 0 |
| **`@overwrite`** | 触发路径**与普通调用不同**：区域键 = `(void *)__owcN->home`，层级取 `planArenaLevel(nx)`，`own = loc && !rcFuncUsesHome`。全语料修前 6 个 owcell 触发，修掉 1 个、留 5 个 ⇒ 根栈要按"格子的 home 归谁"决定压谁的根 | `codegen.c:5100-5136`；实测 `grep -c 'owc[0-9]*->home'` = 6 |
| **入口守卫** | 守卫省的是**根表物化的值**（`codegen.c:3159-3161` 把 `shouldSweep` 提到调用点）；根栈的压/弹在**主调**侧，主调看不到被调者的 `shouldSweep` ⇒ **压栈不能无条件省掉**，但可以：(a) 按 §5 的静态判据收窄到 3.3% 的调用点；(b) 把表做成静态帧描述符（压一条指针）⇒ 与守卫不冲突。实测 `gate.sh` 基线 510 475 244 Ir ≤ 701 633 044，而 `cyclic_pressure` 的 home 可达调用点是 **0** ⇒ 静态判据下这条门禁不动 | `out/reclaim-perf-gate-baseline.txt`、`out/run-switch-baseline.txt`（通过 28，失败 0）、`out/cost-model-all.txt` |

---

## 5. 能不能静态省掉：只对"可能给外层区域分配"的调用压/弹

**判据（推断 + 实测输入）**：调用点 A→B 需要压 A 的根栈，当且仅当 **B 的调用子树里存在
落进共享区域（home / TLS）的分配**。理由：只有共享区域会被 A 的引用指到、也只有它的清扫需要
A 的根；B 自己帧的块 arena 外层拿不到（`rcFrameOwnsArena` 的安全侧依据）。

* 现成但偏粗的 `planUsesHome`（`plan.c:1461`：读 home 或**往下传**）已能筛掉大部分：
  被调者有 home 形参的调用点 422/4922 = 8.6% ⇒ 省 **91.4%**。
* 细判据（子树里真有 `(*__extc_home)` / owcell-home 分配）需要一个新的 plan bit：
  201/4922 = 4.1% ⇒ 省 **95.9%**【实测】。
* 真正压**非空表**的调用点 162 个（3.3%）【实测】。
* 分组：`bench` 721 个调用点里 7 个（省 **99.0%**）；`tests/reclaim-shapes` 70 里 4（省 **94.3%**）。

**方向**：多压无害（多一份种子只会多留），**少压 = 漏根 = 悬垂**。所以判据必须按"可能"取上界
（保守），实现上宁可多压几个调用点。

---

## 6. 风险面：会动的既有闸门，与"红了说明什么"

基线（本轮在 HEAD `a70dfad` 上实测，`out/`）：

| 闸门 | 位置 | 现状 | 根栈会怎么把它弄红 | **红了说明什么** |
|---|---|---|---|---|
| `run-switch.sh`（28 条） | `tests/reclaim-shapes/run-switch.sh` | **通过 28，失败 0**（`out/run-switch-baseline.txt`） | ①i/①j 要求 `hole_c`/`caller_frame`/`persist_nodes` 开档关档都 rc=0 且输出逐字 —— 这是重新打开 home 触发后**最先红的** | 根栈压栈点漏了/弹早了/表有陈旧槽 ⇒ 种子不完整 ⇒ **真悬垂**（P0） |
| ①b/①c/①e/①f 峰值比 | 同上 | 绿 | 触发集合变了若把清扫的统计/峰值搞坏 | 清扫重新上场后收到东西了，但**界**或**语义**被改（后者更严重） |
| `reclaim-perf/gate.sh` | `tests/reclaim-perf/gate.sh` | **510 475 244 Ir ≤ 701 633 044（−56.3%）**，exit 0（`out/reclaim-perf-gate-baseline.txt`） | 压/弹若退化成"每个调用点无条件"：`cyclic_pressure` 有 11 个调用点、**0 个 home 可达** ⇒ 正常实现不该动它 | 静态收窄判据失效（退化成全调用点压栈）⇒ 性能账被付回去 |
| `hole_c/run.sh`（7 条） | `tests/reclaim-shapes/hole_c/run.sh` | 绿（读 §4.4 数字） | 同上，最直接的健全性判据 | 同一个 C 洞复发 |
| `run-evidence.sh` | `tests/reclaim-shapes/run-evidence.sh` | 绿（清扫次数不归零） | 若为了"健全"把共享区域的清扫整体关掉，`cross_region`/`cyclic_pressure` 的 `sweeps` 归零 | 修法走了**废掉清扫**那条错路（等价于 ④ 的精神） |
| `gate_checkc.py`（全语料生成物 gcc+clang `-c -O2`） | `check.sh:396` | 绿 | 新生成的 C 有非法构造（如把帧地址写进静态初始化、类型不匹配） | 生成物不再是合法 C |
| `gate_asan_corpus.py`（正例 ASan+UBSan 真跑） | `check.sh:403` | 绿 | 悬垂地址被 mark 解引用、槽越界 | 生成物有 UB —— 最直接的安全红灯 |
| `tools/golden.sh` | `check.sh:409` | **观察项**（哈希差异不判红，只判"必须是合法 C"） | 生成物字节会变（预期） | 只是影响面，不是缺陷 |
| 全量 1221 程序输出对比 | 手动/闸门外的口径 | 逐字一致 | 清扫重新上场改变了可观测输出 | **语义被改**（最高优先级） |
| `bench/heavy/bt.extc`、`tests/reclaim-shapes/persist_nodes.extc`、`bench/oi/persist/p3834_nodes.extc` | ①②i/①j | bt 开档 rc=0、`trees=262405599`；persist_nodes 5 行逐字 | 重新打开 home 触发后若根栈不全，bt 立刻回到 rc=139 | 真实语料上的同一洞复发 |
| 曾经的 SEGV 用例（`tests/heap/loan`/`plate`/`cabi/heapmain`/`coro/iterator_protocol`） | run-switch ③ | rc=0 | 根栈若对所有指针做登记/标记会踩到板/池/栈/C 的指针 | `owns` 守卫被绕过 ⇒ SEGV |

注：`run-shard.sh` ① 比的是"同一个编译器的 `EXTC_RC_SHARDS` 不设 vs =1"（`run-shard.sh:46-65`），
不是冻结基线 ⇒ 根栈不会让它红。

---

## 7. 建议

**结论：不做整条根栈。** 依据（全是本轮实测）：

1. **收益面 104 个程序，可测收益只有 1 个**（`stress` +12.4 MiB）；其余 106 个都在底噪内。
2. 那个唯一收益还不是"回到无回收"，修后（23.53 MB）比 `EXTC_RECLAIM=0`（16.23 MB）**更差 42%**。
3. 代价不是指令数（压/弹只落在 3.3% 的调用点、表最大 7 项），而是**新纪律**：帧级并集表、
   作用域/陈旧槽清理、协程/`ext` 的"根不可靠"帧标记要新造。
4. 一旦重新打开 110 个程序的 home 触发，上面 §6 那一排健全性闸门全部要重新过 —— 而它们
   现在的绿恰恰是 `19fd567` 用"少收"换来的。

如果要动，按性价比排序做**子集**：

| 优先级 | 子集 | 收益 | 工作量（函数 / 行数 / 新运行期结构） |
|---|---|---|---|
| **A（推荐先做）** | **`main` 特例**：`rcFrameOwnsArena` 对"home 就是本帧 arena"的 main 返回真 | 6 个 escape-fuzz 程序恢复（无实测量级） | 1 个函数 / ~5 行 / 无。风险：main 触发时更深帧不在栈上（推断），须由 `hole_c/run.sh` + `run-switch.sh` 钉住 |
| **B（推荐试的第二件）** | **欠收标志 + 拥有者调用返回后收一次**（方案 ③b）：被调者置区域级 `owe` 标志；`ARENA_HOME` 区域的拥有者是链根（今日只能是 `main`），它在**每次调用返回后**查一次标志、用**自己此刻的静态根表**收一次 —— 不需要根栈、不需要地址表 | `stress` 那 12 MB 回来（`buildList` 返回后 main 收）；`hole_c`/`caller_frame`/`persist_nodes`/`bt` 是它的现成健全性判据 | 运行期 ~20 行（标志 + `oweCollect`/`checkOwed`）+ codegen 调用点检查 ~10 行 + 拥有者识别（`isMain`）~5 行；**无新运行期数据结构**（只有区域上的一个 bit/计数，多线程要 TLS）。风险：必须钉住"返回点上拥有者的静态表就是完整种子"——深帧都已退出，这是这套方案的全部依据 |
| **C（最后考虑）** | 根栈本体（方案②，只覆盖 `ARENA_HOME`，TLS/协程先不覆盖，但要给不可靠帧压"禁止扫共享区"标记） | 与修前持平（104 个程序） | 6-8 个 codegen 函数 + 4 个运行期入口 + 1 个 plan bit / **净 ~150-250 行** / 新运行期结构：线程本地根栈（push/pop/标记）+ 帧描述符 |

**先决条件（无论做哪个）**：先补一条"重新打开 home 触发"的红判据（现在的 `hole_c/run.sh` 是
"不发射触发"的绿，重新打开后判据语义会翻转），否则 A/B/C 都是在没有牙的判据上做。

---

## 8. 未查清

* ② 的**端到端动态代价**（指令数/吞吐/峰值）没有实测：只有静态的"要建表的帧 139 / 表项 344 /
  压/弹 162 个调用点"与 §2.2(c) 的文本模型。要拿真数字得先在 /tmp 生成物上做原型。
* **TLS 一档没有正向语料**（全语料 0 个能编译的 `parallel::run` 程序），TLS 边界只是从
  `extc_par_worker` 与 `parTls` 推出的，**未实测**。
* "无触发的帧"的表项数用 `(void **)&X` 扫描器近似；`rcRootDecl` 登记的**形参**槽不取地址，
  这套扫描数不到（根表口径里形参占 20/4754，可忽略，但不是零）。
* §7-A（main 特例）的健全性只做了推理，**未实测**。
* "104 个程序在根栈下峰值会回到修前"是**假设**：实测到的只有修前的峰值，除 `stress` 外差异都在噪声内。
* 方案 ③b（欠收标志 + 拥有者返回后收）没有做原型：它"能在 `stress` 上拿回 12 MB"是从
  "返回点上深帧已退出 ⇒ 拥有者静态表完整"推出来的，**未实测**；`hole_c`/`caller_frame`/
  `persist_nodes`/`bt` 只是它现成的健全性判据，没有跑过 ③b 的实现。
