# `hole_c` —— `bench/heavy/bt.extc` 提前释放（UAF）的最小形状 + 计数路径定位

**一句话结论**：这不是"计数某处把还有持有者的对象降到 0 释放了"，而是**清扫（region sweep）**在
**根表为空**的触发点上，把 `hold = 1~2`（还有持有者）的槽整片放掉了。错的那条路径是
**清扫的种子（根表）**，不是 inc/dec 的算术；`bt.extc` 的 `check` 读到空闲槽链号是后果。

> 本轮**一行 `src/` 都没动**（`git status` 见文末）。`src/` 下 codegen 的行号会漂（另一位 agent
> 正在改），所以本文引用**函数名 + 生成物里的字面串**，行号只作参考。

---

## 1. 文件

| 文件 | 作用 |
| --- | --- |
| `hole_c.extc` | 最小形状：递归建一棵满二叉树，再递归走一遍数节点。24 行（非注释非空 17 行） |
| `run.sh` | 复现判据：开档必须红、关档必须绿。**exit 0 = 复现成功** |
| `evidence.sh` | 硬证据：往**生成物**（/tmp 里）插探针，量出"被放掉的槽都还有持有者、种子为空" |
| `caller_frame.extc` | 同一洞的**非递归**形态（定位辅助；症状是挂住，不做断言） |

```sh
bash tests/reclaim-shapes/hole_c/run.sh        # exit 0 = 复现成功（洞还在）
bash tests/reclaim-shapes/hole_c/evidence.sh   # exit 0 = 证据成立
```

开关口径：本分支（`reclaim-default`）回收**默认打开**；`EXTC_RECLAIM=0` 关掉，`EXTC_RECLAIM=1`
强制打开（不看分配点）。所以开档 = `EXTC_RECLAIM=1`，关档 = `EXTC_RECLAIM=0`。

---

## 2. 实测输出

### 2.1 `run.sh`（`exit 0`，洞还在）

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

### 4.3 给修的人的假设（**未验证**，按可信度排序）

1. **触发点的根表要能表达动态栈帧**：要么让分配点所在的函数在"有调用者可能持有本区活引用"时
   也判 `rcRootsUnreliable`（保守：不触发），要么把活引用放进一个运行期可枚举的根栈
   （调用方在调用前登记、返回后撤销），让 `extc_rc_mark` 能看见外层帧。
2. **清扫不吃 `hold>0` 的槽**：`extc_rc_sweep` 加一条 `hold != 0 ⇒ 当活的`（种子不全时最坏是晚收，
   符合运行期注释里"宁可晚收，不可早放"的方向）。代价是：真有环、且环上对象还被计数钉着时，
   清扫也收不了它 —— 但环的计数本来就永不归零，这一档本来就得靠清扫，两者需要一起想。
3. 只按"递归函数关触发"打补丁是**不够**的：`caller_frame.extc` 证明非递归的主调帧同样漏。

### 4.4 没把握处 / 局限

- **深度 16 卡在压力线边上**：触发需要 `bytes > EXTC_RC_FLOOR = 1 MiB`，深度 15 恰好是
  1.0 MiB − 16 B。若 `src/` 那边改了 `EXTC_RC_FLOOR` 或槽大小，最小深度会变；形状本身不受影响，
  把 `build(16)` 调大即可（`run.sh` 的第 ④ 条会因此失败，提示这个前提变了）。
- **崩溃点不唯一**：`hole_c` 稳定崩在走树的 `count`（139×8 次）；`caller_frame` 是挂住。
  两者都是"种子不全"的下游症状，但"第一种症状"依分配/复用的交错而变（我也见过崩在
  `extc_rc_alloc` 里读被写坏的空闲链的那种交错）。判据因此写成"rc≠0 **或** 输出不是 131071"。
- **没有验证修复**：`src/` 一行没动，4.3 全是假设；`evidence.sh` 只是测量，不是修复验证。
- 我只跑了本目录的形状与 `bt.extc` 的对照，**没跑完整测试套件**（按任务要求）。
- `src/back/codegen.c` 正被另一位 agent 修改：本文的生成物观察基于**当前工作树 + 当前
  `build/extc`**（HEAD `cb95a8c` + 未提交的 `codegen.c`/`main.c` 改动）。行号会漂。

---

## 5. 工作树自证

```
$ git status --short
 M src/back/codegen.c     ← 另一位 agent 的改动，不是我
 M src/main.c             ← 另一位 agent 的改动，不是我
?? tests/reclaim-shapes/hole_c/
```
