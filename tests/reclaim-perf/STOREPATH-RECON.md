# P4 跨区检查闸门：能不能收窄（只读勘察）

**结论速览**
* **B（运行期收窄 `partN` 口径）不成立 ⇒ 闸门是承重的**：P4 上两个参与区域**都分配过 rc 槽**
  （r0：`sym`+`table`；r1：`unit_node`），且现有 `notePart` 的调用点**只会在"该区域确有一个 slab 对象"时触发**
  ⇒ "持有 rc 槽的区域数"与"参与区域数"**恒等**，收窄是空操作（既省不掉、也不会漏登记）。
* **A 指向一个具体可补的编译期事实，但不是"宿主是本帧新建"**：`storeRefOwnedReg` 在 P4 上**其实用了**
  （实测 **1,547,825 次调用** —— 与任务背景里的 "`storeRefOwnedReg` = 0" **不一致**，见 §3）；
  真正吃掉 **81.7%** `ownSlab` 的是 **`owner == 0` 的本地/形参槽存储**（3,552,955 次调用），
  而那条路径在**无计数档下被证明整个 gated 块是死代码**（`so == 0` ⇒ 既不 `rsDec` 也不 `rsInc`）。
  ⇒ **判"编译期补事实"**：给 `owner == 0` 的存储发一个专用入口（或直接复用裸 `storeRef`），
  **受影响 ≈ 69.7% 的存储入口、≈ 82% 的 `ownSlab` 调用**。

复现（全部只读；产物写到 `/tmp`）：
```bash
build/extc -w --no-line-map -I stdlib bench/gc/src/spectrum_p4.extc -o /tmp/recon/p4.c
cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o /tmp/recon/p4 /tmp/recon/p4.c
EXTC_RC_STATS=1 taskset -c 6 /tmp/recon/p4 2000        # rs_inc=0 ✓ 与背景一致
# 动态入口分布（生成物里 `#define EXTC_RC_TIME 0` 是硬 define，必须改 /tmp 副本再编）：
sed 's/^#define EXTC_RC_TIME 0$/#define EXTC_RC_TIME 1/' /tmp/recon/p4.c > /tmp/recon/p4tick.c
cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o /tmp/recon/p4tick /tmp/recon/p4tick.c
taskset -c 6 /tmp/recon/p4tick 2000
```

## 1. A 部分：三条入口的选择条件与行号

`src/back/codegen.c`（引用槽赋值的发射路径，`refStore` 分支）：

| 行号 | 条件 | 发出的入口 |
|---|---|---|
| 6068-6075 | `cgSlotHost` 判宿主；`h != RC_HOST_LOCAL` 且追不出纯地方基 ⇒ `hostUnknown` | 6095 `storeRefUnknown` |
| **6098** | `hreg = rcFreshHostRegion(g, hostBase)` —— **宿主是"本帧新局部量"**（在 `rcFresh*` 表里）时非空 | ↓ |
| 6108-6113 | `hlvl` = 宿主在该表里的层；6108 要求 `hostBase` 是 `EX_IDENT` | |
| 6114-6119 | `vlvl` = **值**若是 `EX_IDENT` 且也在该表里时的层（否则 `-2`） | |
| **6125** | `g->rcNoCount && vlvl != -2 && vlvl == hlvl` | 6126 `storeRefSameRegion`（`skip:same-region`） |
| **6128** | 否则（有 `hreg`） | 6128 `storeRefOwnedReg(..., hreg)` |
| 6134-6139 | `hostBase == NULL` 且目标是 `EX_IDENT` ⇒ 试 `rcEmitStoreRelease`（覆盖点定点释放） | 该释放路径 |
| **6141** | 其余 | `storeRefOwned(tgt, val, 0)` —— **`owner` 为空串 `"0"`** |

运行期（`src/back/reclaim.c`，生成物字符串）：
* 484/489-490：`extc_rc_partN` 与 `extc_rc_notePart`（**只增不减**）；**494** 在 `extc_rc_alloc` 里无条件记；
  **718** 在 `storeRefOwnedReg` 里记宿主区域；**767** 在 `storeRefOwned` 里记 `regionOfOwn(owner)`（owner 为空则空操作）；
  **828** 在 `storeRefCons` 里记。
* **719** `storeRefOwnedReg` 的 `if (extc_rc_partN > 1) { … }`：块里做 `*slot` 旧值/新值两次 `ownSlab` + `rsDec/rsInc`。
* **775** `storeRefOwned` 的同形闸门：`so = owner ? ownSlab(owner) : 0`；`sold = old ? ownSlab(old) : 0`；
  `sv = v ? ownSlab(v) : 0`；**`if (so) { … }`** —— `owner == 0` 时 **`so == 0` ⇒ 整个块只做两次查询、不做任何记账**。

### P4 的 12 个存储点（生成物实测，静态）
| 生成物行 | 入口 | 目标 | `owner` |
|---|---|---|---|
| 1874 / 1946 / 1977 / 1989 / 1968 / 1890 | `initRef` ×6 | 新槽 | — |
| **1877** | **`storeRefOwnedReg`** | `s->next`（`s` 是 r0 里本条函数新分配的 `sym`） | `__extc_rcr0` |
| 1878 | `storeRefOwned` | `tab->buckets.data[hash]`（视图元素） | 非 0 |
| 1897 / 1983 / 1993 | `storeRefOwned` | `cur` / `ast` / `cur`（**局部游标槽**） | **`0`** |
| **1982** | **`storeRefOwnedReg`** | `p->sib`（`p` 是 r1 里本函数新分配的 `unit_node`） | `__extc_rcr1` |
| `SameRegion` / `Unknown` | **0 处** | | |

`EXTC_RC_DBG_LEVEL=1`（codegen.c:6120）在 P4 上只打 **2 行**，就是那两个 Reg 站点：
```
[rclevel] host=__extc_rcr0 hlvl=-1 planLvl=0 eqPlan=0 freshLvl=-2 eqFresh=0
[rclevel] host=__extc_rcr1 hlvl=2  planLvl=0 eqPlan=0 freshLvl=-2 eqFresh=0
```
⇒ **`SameRegion` 为 0 的直接原因**是**值侧事实缺失**：两个站点的值都不是"本帧新局部量的标识符"
（`freshLvl=-2`），所以 `vlvl == hlvl` 永不成立（`storeRefSameRegion` 还额外要求 `rcNoCount`）。

## 2. 动态入口分布（实测，units=2000）

| 入口 | 调用次数 | 占比（存储类） |
|---|---|---|
| `alloc` | 1,547,826 | — |
| **`storeReg`（`storeRefOwnedReg`）** | **1,547,825** | **30.3%** |
| **`storeOwned`（`storeRefOwned`）** | **3,552,955** | **69.7%** |
| `collect` | 47 | — |
| `slabNew` / `freeSlot` / `regionNew` | 4 / 1,521,662 / **2** | — |

* `rs_inc=0 rs_dec=0 rs_live=0 rs_hwm=0 releases=0`（**跨区边一次都没发生** ✓ 与背景一致）。
* `regionNew=2` ⇒ 运行期真的建了 **2 个区域** ✓（与 `partN=2` 相容）。
* **`storeOwned` 的 3.55M 里 ≥98.9% 是 `owner == 0` 的本地槽**：三条 `owner=0` 站点都在热循环里
  （`symtabGet` 的 probe 循环、AST 建链、AST 走链），而唯一带对象宿主的 `storeRefOwned`（行 1878）
  在插入路径上，其执行次数 ≤ stdout 的 `inserts + misses = 11,081 + 24,497 = 35,578`
  ⇒ `(3,552,955 − 35,578) / 3,552,955 = 98.99%`（用实测计数作上界的**推算**，非逐站点测量）。

**这解释了 81.7%**：本地槽站点每次调用做两次 `ownSlab`（旧值 + 新值），而 `so == 0` ⇒ 两次查询的结果
**被立刻丢掉**。3.55M × 2 ≈ 7.1M，占背景里 `ownSlab` 总数 8.64M 的 **≈82%** ✓（与 81.7% 一致）。
⇒ **最小改动面**：`owner == 0` 时改发"本地槽入口"（只写 + 计数档下的 holdUpd；无计数档下等价于裸写），
     把那个 gated 块整段删掉。**注意**：此等价只在**无计数档**成立（`EXTC_RC_NOCOUNT=0` 时同一入口还要
     `holdUpdSlab(sv/sold)`），所以专用入口要按 `#if EXTC_RC_NOCOUNT` 分档。

**受影响比例**：存储入口 **69.7%**（3,552,955 / 5,100,780）、`ownSlab` **≈82%**。

## 3. 与任务背景不一致的一处（必须记一笔）

背景写 "`storeRefSameRegion` = 0、`storeRefOwnedReg` = 0"。我这边：
**`storeRefOwnedReg` 静态 2 处、动态 1,547,825 次**（`[rccount] storeReg`）。
`storeRefSameRegion` **确为 0**（静态 0 处、`#if EXTC_RC_NOCOUNT` 下 `freshLvl=-2` 永不满足）。
⇒ 结论不受影响（81.7% 的来源与 Reg 是否被用无关），但**"Reg 入口 0"这一条我无法复现**：
按实测它承担了 30.3% 的存储；若背景那份数字来自另一份构建/另一套计数，以那边为准即可，
本报告只声明我量到的数（含 `[rccount]` 原文）。

## 4. B 部分：两个区域是什么？能否收窄 `partN`？

| 区域 | 生成物来源 | 分配过的 slab 对象 |
|---|---|---|
| `__extc_rcr0` | `extc_rc_regionOf((void *)&((*__extc_home)), -1)`（`symtabPut` 里，= 调用方家区域）；`main` 里同名局部 = `regionOf(&__extc_a[1], 1)` | **行 1873 `sym`、行 1945 `table`** |
| `__extc_rcr1` | `extc_rc_regionOf((void *)&(__extc_a[2]), 2)` | **行 1976 `unit_node`** |

* **两个区域都持有 rc 槽**（各自的类型描述符 `__extc_rct0`/`__extc_rct1`/`__extc_rct2` 都带引用偏移表，
  且都走 `extc_rc_alloc`）⇒ **不存在"只参与记账、不持有槽"的区域** ⇒ **B 的假设不成立**。
* **建议的口径本身就是空操作**：`extc_rc_notePart` 的四个调用点（`alloc` 494、`storeRefOwnedReg` 718、
  `storeRefOwned` 767、`storeRefCons` 828）**都要求该区域已经有一个 slab 对象**（`alloc` 直接是它，
  其余走 `extc_rc_regionOfOwn`，而它只对 slab 槽返回非空——bump 对象不是 slab 槽）。
  纯 bump/解析区**已经被刻意排除**（`extc_bump` 的注释，reclaim.c:888-900：bump 槽从不置占用位、
  按槽登记不适用、记上只会白白关掉快路径）。⇒ "持有 rc 槽的区域" 与 "参与区域" **恒等**。
* **健全性正面回答**：因为口径恒等，收窄**不会漏掉任何真实跨区边**（也**不会**把 `partN` 从 2 降到 1）。
  `rs_inc=0` 的成因是"这一轮真没发生跨区存储"，**不是**"闸门开着却漏登记"；反过来说，
  只要两个区域都存在，就**不能**假定没有跨区边（值可以来自另一区）——所以闸门必须保持开着。

## 5. 判据结论

* **B 不成立**（两个区域都持有槽；建议口径与现状恒等）⇒ **闸门在 P4 上是承重的**，运行期收窄这条路**关闭**。
* **A 指向具体可补的编译期事实** ⇒ 判 **"编译期补事实/专用入口"**：
  * 事实：**`owner == "0"`（本地/形参槽）** —— 它在 6141 行已经确定可知；
  * 改动面：codegen.c 6141 的分支在 `EXTC_RC_NOCOUNT` 下发专用入口（或复用 `storeRef`），
    改一处；运行期加一个 3 行的 `storeRefLocal`（无计数档）；
  * 受影响：**存储入口 69.7%**、**`ownSlab` ≈82%**（≈7.1M 次 → 0）；
  * 风险：无计数档下等价性可由"`so == 0` ⇒ 块内无副作用"直接证明；计数档**不要动**（那条路要做 holdUpd）；
    网兜 = `gate-reclaim-*`/`gate-shards` 的跨区登记量棘轮 + `EXTC_RC_STATS` 的 `rs_hwm` + ASan 语料。
* **"宿主是本帧新建"这个事实不需要补**：它已经在用（`storeRefOwnedReg` 1.55M 次）；
  `SameRegion` 缺的是**值侧**事实（值 = 本帧新局部量的标识符）——P4 的值都来自符号表/AST 指针，
  这一条在当前形状上**没有可兑现面**（0 处静态、0 次动态）。

**未查清**：① 背景里 `storeRefOwnedReg = 0` 与我的 1,547,825 不一致（见 §3，我未能复现对方口径）；
② 3.55M 中"本地槽"与"对象宿主"的精确拆分（只有上界推算 98.99%，没有逐站点计数）；
③ `partN` 本身没有任何仪器打印，我用 `regionNew=2` + 两区域都有分配 + 背景实测值三方面交叉确认。
