# `hole_c` —— 清扫在种子不完整时提前释放（UAF）的最小形状 + **修法**

**一句话结论**：这不是"计数某处把还有持有者的对象降到 0 释放了"，而是**清扫（region sweep）**在
**根表为空 / 不完整**的触发点上，把 `hold = 1~2`（还有持有者）的槽整片放掉了。错的那条路径是
**清扫的种子（根表）**，不是 inc/dec 的算术；`bt.extc` 的 `check` 读到空闲槽链号是后果。

**状态：已修（本轮）。** 修法是编译期判据 —— **触发点只在「本帧拥有」的区域上下手**：家 arena
（`(*__extc_home)`，调用者一路传下来的同一只）与线程本地 arena 上一个触发都不发；本帧自己的块
arena（`&__extc_a[lvl]`，`lvl>=1`）仍照常扫。见 `src/back/codegen.c` 的 `rcFrameOwnsArena` /
`rcEmitTrigger` 的 `frameOwned` 与文件头的「第三个安全方向」。**代价**见 §4.4：只往家 arena
分配的函数从此不触发，那类区域要等拥有它的帧自己分配时才扫、或等帧销毁；要同时保住健全与清扫
能力需要**运行期根栈**，那是下一步。

> 下面的 §2–§3 是**修之前**的实测与最小化过程（保留为证据）；§2.4 与 §4 是修后的状态。

---

## 1. 文件

| 文件 | 作用 |
| --- | --- |
| `hole_c.extc` | 最小形状：递归建一棵满二叉树，再递归走一遍数节点。24 行（非注释非空 17 行） |
| `run.sh` | **绿判据（门禁）**：开档/关档都必须 rc=0 且输出逐字正确；ASan 不许有 SEGV/UAF。**exit 0 = 绿（洞不在了）** |
| `evidence.sh` | 硬证据：往**生成物**（/tmp 里）插探针，量出"被放掉的槽都还有持有者、种子为空"（修前用；修后触发点被挡住，它报"证据不成立"是对的） |
| `caller_frame.extc` | 同一洞的**非递归**形态（修前症状是挂住 rc=124；现在也是 `run.sh` 的判据之一） |
| （真实语料档）| `tests/reclaim-shapes/persist_nodes.extc`（`bench/oi/persist/p3834_nodes.extc` 最小化版）+ `run-switch.sh` ①i |

```sh
bash tests/reclaim-shapes/hole_c/run.sh        # exit 0 = 绿（修之前：exit 1 且前两条 FAIL=复现成功）
bash tests/reclaim-shapes/hole_c/evidence.sh   # 修前 exit 0 = 证据成立；修后报"证据不成立"（不再有那次清扫）
```

开关口径：本分支（`reclaim-default`）回收**默认打开**；`EXTC_RECLAIM=0` 关掉，`EXTC_RECLAIM=1`
强制打开（不看分配点）。所以开档 = `EXTC_RECLAIM=1`，关档 = `EXTC_RECLAIM=0`。

---

## 2. 实测输出

### 2.1 `run.sh`（**修之前**：`exit 0` = 复现成功，洞还在）

```
== hole_c：回收档下 bt.extc 的提前释放（UAF）最小形状 ==
   源文件 tests/reclaim-shapes/hole_c/hole_c.extc（24 行）；期望 开档=红 / 关档=绿
  ok   开档（非 ASan）：rc=139 out='' ≠ 'n = 131071' —— 提前释放可观测
  ok   开档（ASan）：rc=1，报告命中 SEGV/UAF
       ── ASan 报告抬头 ──
       ==3070467==ERROR: AddressSanitizer: SEGV on unknown address (pc 0x55a6db7db292 bp 0x7fff7ad7fd50 sp 0x7fff7ad7fb50 T0)
           #0 0x55a6db7db292 in count /tmp/tmp.gsjyOWWHVB/ona.c:964
       rax = 0x1fffffffffffffff  rbx = 0xffffffffffffffff  rcx = 0x0000000000000000  rdx = 0x0000000000000002
       SUMMARY: AddressSanitizer: SEGV /tmp/tmp.gsjyOWWHVB/ona.c:964 in count
  ok   关档（EXTC_RECLAIM=0）：rc=0 out='n = 131071' —— 对照组绿
  ok   开档 + 深度 15（65535 节点 = 1.0 MiB - 16 B）：rc=0 —— 触发点由 1 MiB 下限把关
   生成物触发点（根表为空 = 外层调用帧的活引用不在种子里）：
       extc_rc_maybeCollect(__extc_rcg0, (void *const *)0, 0)

复现判据：通过 4，失败 0
```

- 开档非 ASan **rc=139**（信号 11），连跑 8 次都是 139。
- ASan 报告里 `rbx = 0xffffffffffffffff` = 被当指针读的那个值，就是 `extc_rc_freeSlot` 写进槽头
  8 字节的**空闲链号**（槽头 = `left` 字段偏移 0；链尾写 `-1`）。`bt.extc` 那次读到的是
  `0xffde`（一个非链尾的槽号），同一个来源、同一个字段。
- 关档对照：`n = 131071`（= 2^17−1，深度 16 满二叉树节点数），rc=0。
- （生成物行号 `ona.c:964`/`1000` 会随 `src/` 改动漂，判据不看行号。）

### 2.1b 原文件对照（同一台机器、同一 `build/extc`）

```
EXTC_RECLAIM=1  bench/heavy/bt.extc  →  rc=139
EXTC_RECLAIM=1  + ASan               →  SEGV on unknown address 0x00000000ffde（空闲槽链号）
EXTC_RECLAIM=0  bench/heavy/bt.extc  →  rc=0，trees = 262405599
```

最小形状与原文件**同一签名**：走到被放掉的槽，把 `left` 位置上的空闲链号当指针解引用。

### 2.2 `evidence.sh`（探针，`exit 0`）

```
探针已插入：freeSlot(hold!=0) + collect(nroots/freed/freed_held/maxhold)
== hole_c：清扫释放了【还有持有者】的槽（生成物探针）==
   运行退出码：139（139/134 = 提前释放后可观测的崩溃）
   PROBE collect nroots=0 marked=0 freed=65537 freed_held=65537 maxhold=2
   最后一次清扫：nroots=0 marked=0 freed=65537 freed_held=65537 maxhold=2
   （清扫次数：1）

证据成立：nroots=0（种子空）· freed=65537 · freed_held=65537（全部还有持有者）· maxhold=2
⇒ 提前释放不是计数归零，是清扫在**根表为空**时放掉了 hold>0 的槽。
```

这一笔的**定性力量**：生成物里计数路径只有一条释放入口 ——
`extc_rc_holdUpd` 里的 `if (s->hold[i] == 0) extc_rc_freeSlot(s, i);`（reclaim.c 发出的运行期）。
所以"进 `extc_rc_freeSlot` 时 `hold[i] != 0`"**只可能**来自 `extc_rc_sweep`（sweep 不看 hold，
只按 `occ & ~mark` 放）。实测 65537 个被放掉的槽**全部**满足 `hold>0`（`maxhold=2`），
即：**计数器从来没提前归零；是清扫拿着空种子把活对象放掉了。**

### 2.3 `caller_frame.extc`（非递归形态）

```
EXTC_RECLAIM=1 …  timeout 5   → rc=124（挂住；走链踩进被复用的槽成环）
EXTC_RECLAIM=0 …              → n = 70000，rc=0
```

它的触发点同样是 `extc_rc_maybeCollect(区域, (void *const *)0, 0)`：`main` 手里的链头是活引用，
但它在**主调帧**里。洞**不是"递归"特有的**。

### 2.4 修复后的 `run.sh`（**绿**，`exit 0` = 洞不在了）

```
== hole_c：清扫不得在种子不完整时下手（本帧拥有的区域才触发）==
  ok   开档（EXTC_RECLAIM=1）：rc=0 out='n = 131071' —— 外层帧的活引用没被放掉
  ok   开档（ASan）：rc=0，无 SEGV / UAF 报告
  ok   关档（EXTC_RECLAIM=0）：rc=0 out='n = 131071' —— 对照组绿（判据不空转）
  ok   caller_frame 开档：rc=0 out='n = 70000' —— 主调帧的活引用没被放掉（修之前 rc=124 挂住）
  ok   caller_frame 关档：rc=0 out='n = 70000' —— 对照组绿（判据不空转）
  ok   生成物：开档的 hole_c 不发射清扫触发（分配点全在家 arena 上 ⇒ 不扫外层区域）
  ok   开档 + 深度 15（65535 节点 = 1.0 MiB - 16 B）：rc=0 —— 触发点由 1 MiB 下限把关

判据：通过 7，失败 0
```

`bench/heavy/bt.extc` 与真实语料那一档（`persist_nodes`，N=50000）同样转绿：
`bt.extc` 开档/关档都 rc=0、`trees = 262405599`；`persist_nodes` 开档/关档 5 行输出逐字一致。

---

## 3. 最小化过程（怎么缩到 17 行代码的）

原文件 34 行（30 行代码）做了三件与洞无关的事：13 棵树逐层加深、深度 18 的大树、500 次重复
`check`。逐个去掉/二分：

| 试的 | 结果 | 说明 |
| --- | --- | --- |
| 原 `bt.extc`，回收档 | rc=139 / ASan SEGV | 复现基线 |
| 只留一次 `build(18)` + 一次 `check` | rc=139 | 循环与重复全部可去 |
| `check` 改成"只递归数节点"`count` | rc=139 | 洞与 `check` 的具体算术无关 |
| 去掉 `right` 字段（单链递归） | rc=0 | **必需分叉**：要有一棵"已建好、只被栈帧指着的子树" |
| 深度 15 | rc=0（n=65535 正常） | 差 16 B 没过 `EXTC_RC_FLOOR`=1 MiB |
| **深度 16** | **rc=139** | 最小可复现深度（触发需要 `bytes > 1 MiB`，节点槽 16 B） |
| 非递归形态：主调持链头 + 被调只 `new`（`caller_frame.extc`） | 挂住/SEGV | 同一洞的另一半，证明与递归无关 |

最终形状：4 行头注释 + 17 行代码。相对原文件：总行数 34 → 24（**−29%**），
非注释非空 30 → 17（**−43%**）。行数只降这么多是因为 `bt.extc` 本来就小；
**真正的缩小在运行时工作量**：原文件分配 ≈ 78.6 万个节点并做 500 次 × 52.4 万节点的遍历
（≈2.6 亿次访问），最小形状只分配 13.1 万个节点、遍历一次（≈2000× 更少）。

---

## 4. 定位结论

### 4.1 实测（有数字支撑）

1. 分配点的触发**根表是空的**。`hole_c`/`bt.extc` 的生成物里，`build`/`bottomUp` 的分配点是
   字面串 `extc_rc_maybeCollect(__extc_rcg0, (void *const *)0, 0)`（根数 0）。
2. 那次清扫 `nroots=0 marked=0 freed=65537`，而 `freed_held=65537`、`maxhold=2`：
   **标记一个都没标着**（种子只有"根表 + `rsbit`"，两者都空），**放掉的每一个槽都还有持有者**。
3. 深度 15（未过 1 MiB）rc=0、关档 rc=0 —— 说明崩溃确实由"清扫被触发"这一步引入。

### 4.2 从源码读出来的机制（推断，但每条都有生成物/源码字面串对应）

- **触发点发射**：`rcEmitTrigger`（`src/back/codegen.c`，注释在函数上方）把根表拼成
  `g->rcGlobalRoots + g->rcRoots`，然后发 `extc_rc_maybeCollect(区域, 根表, 根数)`。
- **根表的范围**：`src/back/codegen.c` 的机制注释写得明明白白 ——
  "根表的来源是**词法作用域**……任一触发点的根表 = **外层块还活着的局部量 + 本函数形参 + 全局量**"；
  进入函数时 `g->rcRoots.len = 0` 再按形参重建（"回收的静态根表按函数重来"）。
  ⇒ **调用栈上其他帧里的活引用（外层 `build` 帧的 `t`）结构性地进不了这张表**。
  在 `build` 里，触发是 `tree *t = ...` 这条语句的**前缀**，此刻本帧连 `t` 都还没出生 ⇒ 根数 0。
- **同区边不当种子**：`extc_rc_storeRefOwned` 只对**跨区**边做 `rsInc/rsDec`（reclaim.c 的注释：
  "只有**跨区**边才动计数：同一条边在同区内，区域自己的标记走得到，不需要种子"）。
  于是 `t.left = build(...)` 这条同区边在 `rsbit` 里没有入口。
- **清扫不看 hold**：`extc_rc_sweep` 的判据是 `occ & ~mark`，与 `hold` 无关；
  `extc_rc_mark` 的种子只有 `roots[]` 与 `rsbit`。
- **安全网没盖到这一档**：机制注释里的两个安全方向之一是"**根表不完整就一个触发都不发**"
  （`rcRootsUnreliable`：枚举负载、泛型枚举、协程帧）。**"本函数的根表完整，但调用帧里还有活引用"
  这一档没有被判成不完整** —— 这是缺的那条判据。

### 4.3 修法（**已落地**：本帧拥有的区域才触发）

Lead 裁定的方向是「**种子不完整时宁可少收，不可照常扫**」—— 与 ⑨「跨区宿主证不出来 ⇒ 目标区域
退出清扫」同一精神。这一档不完整的是**调用栈上其他帧的活引用**，而根表按词法作用域拼，结构性
地看不见它们。

**修法（a）：触发点只在「本帧拥有」的区域上下手**（`src/back/codegen.c`）：

- 新判据 `rcFrameOwnsArena(g, lvl)`：`lvl == ARENA_HOME`（分配落在 `(*__extc_home)` —— 调用者
  一路传下来的家 arena）⇒ **假**；`g->parTls`（`extc_tls_arena`，整条 worker 调用链共用的那只）
  ⇒ **假**；协程帧本来就 `rcRootsUnreliable`，一并挡。只有本帧自己的块 arena
  （`&__extc_a[lvl]`，`lvl >= 1`）⇒ 真。
- `rcEmitTrigger` / `rcAllocCore` 多收一个 `frameOwned`：假就**一个触发都不发**。三个分配点
  各自给判据：`exNew` / `exGencall` 用 `rcFrameOwnsArena`；`@overwrite` 用
  `loc && !g->rcFuncUsesHome` —— 格子在本帧（`loc`）且本函数**不收** home 时，格子的 `home`
  才是本帧的 `&__extc_a[1]`；本函数收 home（`g->rcFuncUsesHome`，格子的 `home` = 调用者的家）
  或 `loc` 为假（codegen 期才生成的泛型实例，`owLocal` 仍是 false）时，格子的 `home` 落在
  **外层帧**上，同样不触发。
- 为什么"本帧自己的块 arena"是安全的：外层帧拿不到指向它的引用 —— 跨帧存储的坏方向
  （更深的局部 → 更浅的容器）被 checker 拒（②），会逃逸的值一开始就分配进**家 arena**
  （④ 提权）。所以在块 arena 上，本帧的静态根表就是完整的种子。
- 这**不是**把清扫废掉：环垃圾只要在拥有区域的帧自己的分配点旁边（`main` / 无 home 形参的
  函数），清扫照常上场 —— 护栏数字见 `tests/reclaim-shapes/README.md` 的「C 洞」一节。

❌ **绝对没做**：让 `extc_rc_sweep` 遇 `hold != 0` 就当活的。环垃圾的 `hold` 永远 ≥1，那等于
废掉清扫（环再也收不掉），是换一个更大的洞。

### 4.4 代价 / 后续（如实记）

- **清扫机会的损失（有实测数字）**：只往家 arena 分配的函数（递归构造、被调者用调用者的家、
  `@overwrite` 用 home 形参的格子）从此**不触发清扫**。那个区域只由**拥有它的那一帧**在自己也有
  分配点时清扫；拥有者一个分配点都没有时（`hole_c` / `caller_frame` / `persist_nodes` 都是这一档），
  整片要等帧销毁才还 —— 这是**上界放松**，不是悬垂。全量语料（1221 个 `.extc`）里修前有触发的
  208 个程序中，**110 个触发被全部移除**；逐个体检其中可运行的 109 个，**只有
  `bench/stress/stress.extc` 出现可测的峰值回归：10.92 MB → 23.60 MB**（rc=0、输出逐字一致），
  其余 Δ ≤ 0.06 MB；`bench/heavy/bt.extc` 的对照无效（修前 rc=-11，没跑完）。详见
  `tests/reclaim-shapes/README.md` 的「C 洞」一节。
- **后续 (b)：运行期根栈**（本轮不做）。调用前登记、返回后撤销外层帧在本区的活引用，让
  `extc_rc_mark` 看得见整个调用栈 ⇒ 既健全**又**保住清扫能力（`stress.extc` 那 12 MB 也就回来了）。
  代价是每次调用 O(1) 的压栈/弹栈（以及根表的运行期物化），入口守卫的 −56.3% 会付回去一部分；
  设计要点是把根栈与现有的 `rcRoots` 静态表接进同一个 `roots[]` 入口。
- **`main` 的家 arena 也一并收紧**（`ARENA_HOME` 一律不触发，不特例 `main`）：`main` 是唯一
  「家 arena 就是自己帧的 arena」的函数，认它要多一条判据；收紧只损失清扫机会、不损失健全。
- **`evidence.sh` 修后报"证据不成立"**：`hole_c` 的开档生成物里已经没有触发点，探针观察不到
  那次清扫 —— 这**正是修好的表现**，不是探针坏了。
- **没有验证到的地方**：`parTls`（worker 调用链）那一档只按"和家 arena 同构"推理收紧，
  语料里只有一个 `tests/errors/parallel_run_noargs.extc`（反例，不生成），**没有正向语料实测**。
  另外，`rcFrameOwnsArena` 的依据是"跨帧存储的坏方向被 checker 拒 + 逃逸值提权进家 arena"，
  这是对 checker 现有保证的信任，不是本轮新证的定理。
- **深度 16 卡在压力线边上**：触发需要 `bytes > EXTC_RC_FLOOR = 1 MiB`，深度 15 恰好是
  1.0 MiB − 16 B。若 `src/` 那边改了 `EXTC_RC_FLOOR` 或槽大小，最小深度会变；形状本身不受影响，
  把 `build(16)` 调大即可（`run.sh` ⑦ 会因此失败，提示这个前提变了）。
- **修前的崩溃点不唯一**：`hole_c` 稳定崩在走树的 `count`（139×8 次）；`caller_frame` 是挂住。
  两者都是"种子不全"的下游症状，但"第一种症状"依分配/复用的交错而变 —— 所以修后判据钉的是
  "开档 rc=0 且输出逐字正确"，不是"崩在某个具体函数"。

---

## 5. 工作树自证（修后）

```
$ git status --short
 M src/back/codegen.c                     ← 本轮 C 洞修复（rcFrameOwnsArena / frameOwned）
 M tests/reclaim-shapes/run-switch.sh     ← ①i / ①j 两条新判据
 M tests/reclaim-shapes/README.md         ← 「C 洞」一节
 M tests/reclaim-shapes/hole_c/run.sh     ← 由"复现判据"改成"绿判据"
 M tests/reclaim-shapes/hole_c/README.md  ← 本文
?? tests/reclaim-shapes/persist_nodes.extc ← 真实语料最小化版（N=50000）
```

修前的生成物观察基于当时的 `build/extc`（HEAD `9305b17`）；行号会随 `src/` 改动漂，本文引用的是
**函数名 + 生成物里的字面串**。
