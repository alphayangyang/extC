# ②（不发射 rc）能不能把停顿打下去：命中形状的挡路规则与代价（task-32）

> **只勘察**：不改判决/发射/阈值，不碰 `src/`；停顿数字**只从** `bench/gc/PAUSE-CORPUS.md`
> （task-29 的落盘画像）抄，理由原文**只从**现成诊断 `EXTC_DBG_RCIDLE=1` 读。
> 本文件由 `tools/rc-idle-blockers.py --report` 生成（数字自动抄/自动算，避免手抄错）。

## 0. 一句话结论

**下一件该放宽「B 目的地不是具名字段（下标/切片/解引用基）」**：它在命中集合里能**单独**消掉 **888 µs**，
而且是 `cyclic_pressure`/`shard_bands`/`digraph`/`persist_nodes` 这些**多规则**形状的共同前置；
估计改动 1 条判据路径、不需要新冻结字段（**估计**，不是测量）。

顺位第二件：**A 同一宿主的同一字段有两处站点（支配/互斥）** —— 它单独收益为 0，但 `B` 落地后
能和它一起把 **`persist_nodes`（2343.7 µs，占命中 Σmax 36.8%）** 从靶子上拿下来（该形状的规则
集合恰好是「同字段两站点 + 目的地不是具名字段」两条）。**F（聚合临时量，550.8 µs）**要一份新的
层事实，排在第三。

## 1. 集合对账（先把"多少个"钉死）

- 门槛（表里写的 `max ≥ 100 µs 或 占比 ≥ 1%`）命中 **20** 个；文档 §4.3 清单 **19** 个；任务书说 **18** 个。
- 差在 `tests/reclaim-gaps/shapes/base_call_unknown.extc`：**max 91.0 µs、占比 2.85%** ⇒ 按
  文档自己写的门槛（`占比 ≥ 1%`）**应当命中**，但 §4.3 漏列了它（表里 `命中` 列也是 `-`）。
  本报告以**门槛的字面集合（20 个）**为准，并在表 A 里把它的挡路理由一并列出 ⇒
  18 / 19 / 20 三个数都能对上：**18** = 任务书给的数（与文档 §4.3 差一个）；**19** = 文档 §4.3
  清单；**20** = 门槛字面。本报告分析的是 20（字面）并逐行给出前两者的对账，避免"按哪个数"之争。
- 其余 34 个发 rc 形状的 `max/占比` 也进了分母（35 个发 rc 形状 Σmax = **6382.9 µs**）。

## 2. 数据

### 表 A：命中形状 × 挡路理由（理由逐字取自 `EXTC_DBG_RCIDLE=1`）

| 形状 | max µs | 占比 | n | 挡路理由（原文，该形状**第一条** `idle=0`）| 该形状命中的**规则**条数 | 单一规则? | 备注（依据见 §3）|
|---|---|---|---|---|---|---|---|
| `tests/reclaim-shapes/persist_nodes.extc` | 2343.7 | 10.54% | 2 | line 52: the same field of the same host is written at two sites | 2 | 否（2 条） | — |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 505.4 | 26.14% | 4 | line 85: the host object is not a local created by `new` in this frame | 3 | 否（3 条） | — |
| `tests/reclaim-shapes/live_churn.extc` | 411.1 | 20.75% | 4 | line 42: a reference is assigned to a stack slot (a local/parameter, or a field of one) | 1 | 是 | **需新事实** |
| `tests/reclaim-shapes/cross_region.extc` | 354.0 | 17.03% | 2 | line 48: a reference is assigned to a stack slot (a local/parameter, or a field of one) | 1 | 是 | **绊线** |
| `tests/reclaim-shapes/cross_region_elem.extc` | 350.3 | 56.21% | 74 | line 69: the destination is not a named field of a named object | 1 | 是 | — |
| `tests/reclaim-shapes/owed_home.extc` | 280.8 | 47.11% | 20 | line 36: a reference field is written inside a loop, so one object can be overwritten | 2 | 否（2 条） | — |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 269.9 | 29.76% | 15 | line 42: the destination is not a named field of a named object | 1 | 是 | — |
| `tests/reclaim-shapes/shard_bands.extc` | 198.5 | 16.00% | 24 | line 39: the host object is not a local created by `new` in this frame | 3 | 否（3 条） | — |
| `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | 177.0 | 3.60% | 3 | line 44: the destination is not a named field of a named object | 1 | 是 | — |
| `tests/reclaim-gaps/shapes/carray_slot.extc` | 173.7 | 8.49% | 1 | line 35: a reference is assigned to a stack slot (a local/parameter, or a field of one) | 1 | 是 | **随 B 一起** |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 157.8 | 20.73% | 14 | line 45: a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies | 1 | 是 | **需新事实** |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 144.4 | 1.93% | 1 | a `dyn` call: the callee is not resolved | 1 | 是 | — |
| `tests/reclaim-shapes/struct_copy.extc` | 142.9 | 20.45% | 14 | line 84: a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies | 1 | 是 | **需新事实** |
| `bench/gc/src/digraph.extc` | 139.0 | 14.64% | 19 | line 100: a reference is assigned to a stack slot (a local/parameter, or a field of one) | 4 | 否（4 条） | — |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 134.1 | 14.19% | 3 | line 51: a reference field is written inside a loop, so one object can be overwritten | 1 | 是 | **绊线** |
| `tests/reclaim-gaps/shapes/owcell_garbage.extc` | 127.2 | 5.13% | 1 | line 32: `@overwrite` reuses one piece of storage, so the previous object is dropped | 2 | 否（2 条） | — |
| `tests/reclaim-shapes/scale_growth.extc` | 125.8 | 6.44% | 1 | line 40: a reference is assigned to a stack slot (a local/parameter, or a field of one) | 1 | 是 | **绊线** |
| `tests/reclaim-gaps/shapes/byval_temp.extc` | 125.1 | 5.97% | 1 | line 33: a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies | 1 | 是 | **需新事实** |
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | 125.0 | 5.99% | 1 | line 33: a reference-carrying aggregate is produced as a temporary value; its references lose this root when it dies | 1 | 是 | **需新事实** |
| `tests/reclaim-gaps/shapes/base_call_unknown.extc` | 91.0 | 2.85% | 3 | line 42: the destination is not a named field of a named object | 1 | 是 | — |

### 表 B：按规则分组（分组键 = 诊断原文，去掉站点行号）

| 规则（原文 → 条目）| 形状数 | 单规则形状数 | Σmax µs（全部）| 单规则 Σmax | 其中**净收益**（无新事实/非绊线）| 占命中 Σmax | 可修性 |
|---|---|---|---|---|---|---|---|
| **A 同一宿主的同一字段有两处站点** | 1 | 0 | 2343.7 | 0.0 | **0.0** | 36.8% | 可修（需要支配/互斥分析） |
| **D 引用写进栈上槽（局部量/形参/局部聚合的字段）** | 5 | 4 | 1203.6 | 1064.6 | **0.0** | 18.9% | 部分是死结（要分类） |
| **B 目的地不是具名字段（下标/切片/解引用基）** | 4 | 4 | 888.2 | 888.2 | **888.2** | 13.9% | 可修（已规划） |
| **C 宿主不是本帧 new 出来的局部量（形参/长命对象）** | 2 | 0 | 703.9 | 0.0 | **0.0** | 11.0% | 死结（当前口径下） |
| **F 带引用的聚合被产生成临时量** | 4 | 4 | 550.8 | 550.8 | **0.0** | 8.6% | 可修（需要一份新的层事实） |
| **E 循环里写 ref 字段（宿主不是本迭代新建）** | 2 | 1 | 414.9 | 134.1 | **0.0** | 6.5% | 死结（含 task-18 的常驻对照） |
| **G `dyn` 调用：被调者看不清** | 1 | 1 | 144.4 | 144.4 | **0.0** | 2.3% | 死结 |
| **H `@overwrite` 复用同一格** | 1 | 0 | 127.2 | 0.0 | **0.0** | 2.0% | 死结（语义如此） |

> 合计：命中集合 Σmax = **6376.7 µs**；35 个发 rc 形状 Σmax = **6382.9 µs**；命中集合里**单规则**形状的 Σmax = **2782.1 µs**，其中**净收益**（现成事实、非绊线、非死结规则）= **888.2 µs**。多规则形状（6 个）要同时改 2 条以上才可能 idle。

### 表 C：排序建议（收益 ÷ 估计改动量；**改动量是估计**，收益是测量）

| # | 规则 | 单规则 Σmax µs | 净收益 µs（无新事实/非绊线）| 作为**前置**被几个多规则形状依赖 | 估计改动量 | 历史先例 |
|---|---|---|---|---|---|---|
| 1 | 目的地不是具名字段（下标/切片/解引用基） | 888.2 | 888.2 | 4 个（`persist_nodes.extc`, `cyclic_pressure.extc`, `shard_bands.extc`, `digraph.extc`）| 中（1 条判据路径 + 1 个目的地分解 helper；估计不需要新冻结字段） | 无（新面，RECLAMATION 第五节已规划） |
| 2 | 带引用的聚合被产生成临时量 | 550.8 | 0.0 | 0 个（）| 中高（新的临时量层事实；可能要进冻结清单） | 无（task-17 之后的下一个同类） |
| 3 | 同一宿主的同一字段有两处站点 | 0.0 | 0.0 | 1 个（`persist_nodes.extc`）| 中（分支互斥/支配；估计无需新冻结字段） | 无 |
| 4 | 引用写进栈上槽（局部量/形参/局部聚合的字段） | 1064.6 | 0.0 | 5 个（`cyclic_pressure.extc`, `owed_home.extc`, `shard_bands.extc`, `digraph.extc`…）| ①中新事实（可能进冻结清单）＝中高；③死结，别碰 | **有**：task-16/17 放宽了 ref 标识符槽 |
| 5 | 宿主不是本帧 new 出来的局部量（形参/长命对象）（死结） | 0.0 | 0.0 | 3 个（`cyclic_pressure.extc`, `shard_bands.extc`, `digraph.extc`）| 高（跨过程/宿主收敛），且单规则收益为 0（见分组表） | 无 |
| 6 | 循环里写 ref 字段（宿主不是本迭代新建）（死结） | 134.1 | 0.0 | 2 个（`owed_home.extc`, `digraph.extc`）| 不修（判据本身正确） | task-17 放宽了「宿主本轮新建」那一半；剩下的是死结 |
| 7 | `dyn` 调用：被调者看不清（死结） | 144.4 | 0.0 | 0 个（）| 不修 | 无 |
| 8 | `@overwrite` 复用同一格（死结） | 0.0 | 0.0 | 1 个（`owcell_garbage.extc`）| 不修 | 无（语义死结） |

## 3. 可修性（判断，逐条挂在诊断原文上）

* **D 引用写进栈上槽（局部量/形参/局部聚合的字段）**（5 个形状）：task-16/17 已成功放宽过一支（ref 标识符槽 + 全部写入都随本迭代释放）。剩下的三类：① 本帧游标（`live_churn` 的 `head = c`、`digraph` 的 `p = p.next` / `cur = (*cur).next`）——被丢的对象**另有根**持着，需要「不是最后持有者」的新事实（elide 那条线的义务 ③）；② 局部聚合的字段/元素槽（`carray_slot` 的 `arr[i % 8] = p`、`digraph` 的 `c.head = null`）——与 B 同一件工作；③ **故意的不健全面**：`keeper = t`（cross_region）、`saved = p`（scale_growth）——槽活得比迭代长、对象被提到家区域，放宽就是界随轮数涨，它们正是 `run-switch`/`gate-idle-bounded` 钉的绊线。 —— *估计改动量：①中新事实（可能进冻结清单）＝中高；③死结，别碰*
* **B 目的地不是具名字段（下标/切片/解引用基）**（4 个形状）：RECLAMATION 第五节「宿主」条已点名：`idleHostIdent`/`acTargetSlotIsHeap` 只认具名字段链，`(*root)[0]`、`s.kids[0]`、`c.hubs[0]`、`w.tail[k]` 一律答 NULL。把目的地求值一次、逐叶落账即可（现有 arena 层/循环深度事实够用）。 —— *估计改动量：中（1 条判据路径 + 1 个目的地分解 helper；估计不需要新冻结字段）*
* **F 带引用的聚合被产生成临时量**（4 个形状）：判据 (c) 的原话：临时量的存储属于哪一层 arena 由逃逸分析定，可能的形态是「本帧的家房间」⇒ 界随轮数涨。修法不是放宽，而是给**临时量**也建一份与 arena 同尺的事实（同 task-17 给绑定建的那份），能在『本块内产生、本块内丢』时放行。 —— *估计改动量：中高（新的临时量层事实；可能要进冻结清单）*
* **G `dyn` 调用：被调者看不清**（1 个形状）：动态派发看不见被调者，参数敏感的保守判否是正确方向；放宽需要去虚化/效果签名，是另一条线。 —— *估计改动量：不修*
* **E 循环里写 ref 字段（宿主不是本迭代新建）**（2 个形状）：剩下的全是**同一个对象被每轮改写**：`outer_alloc_inner_write` 是 task-18 专门立的常驻对照（去掉循环深度条件就必须红）；`owed_home` 的 `p` 被 `cur = p` 提到函数层、`digraph` 的宿主是形参/长命对象。放宽它们 = 把界放走。 —— *估计改动量：不修（判据本身正确）*
* **A 同一宿主的同一字段有两处站点**（1 个形状）：`persist_nodes` 的 `update` 在两个分支各写一次 `n.l` ⇒ **每个对象运行时只写一次**，但静态是两处站点。放宽要能证「两处站点互斥（同一对象只走其中一处）」。 —— *估计改动量：中（分支互斥/支配；估计无需新冻结字段）*
* **C 宿主不是本帧 new 出来的局部量（形参/长命对象）**（2 个形状）：`(*a).next = b` 里 `a` 是形参：调用方可能对同一个对象反复调用 ⇒ 无法证明「每对象写一次」。要放宽得做跨过程的「宿主收敛」（RECLAMATION 第五节第一条），本任务范围外。 —— *估计改动量：高（跨过程/宿主收敛），且单规则收益为 0（见分组表）*
* **H `@overwrite` 复用同一格**（1 个形状）：`@overwrite` 的字面语义就是「复用同一格、丢掉上一个对象」；那一格活过调用/迭代 ⇒ 丢的就是长命对象，判否正确。 —— *估计改动量：不修*

## 4. 排序建议（收益 ÷ 估计改动量）

见表 C。**收益是测量的**（`PAUSE-CORPUS.md` 的 max 相加，标明口径）；**改动量是估计**，
只按"要碰几条判据路径 / 是否需要新冻结字段"粗分三档。

## 5. 自检与证据

- **反瞎**：`python3 tools/rc-idle-blockers.py --selftest` —— **五类**合成输入（正常 / 缺行 /
  空 `reason` / 格式不认识 / `idle` 值非法）里，后四类都必须报错；不报错返回 1。本轮**先红后绿**：
  第一次跑"缺行"那格没报错、自检返回 **1**；补上形状级 `require_blockers` 后五格全过、返回 0。
- **每个形状都落到一条理由**：表 A 的每一行都有"挡路理由（原文）"列；脚本对"一个发 rc 的形状
  一条 `idle=0` 都没有"**直接中止**（`DiagError`），不产出"说不清"的行。
- **理由不发明分类**：分组键就是诊断 `reason=` 的逐字原文；原文若不在规则表里，脚本报错。
- **只勘察**：`src/` 未动；产物对 pin 逐字节不变、关诊断时 stderr 0 字节（见汇报）。
- 停顿数字口径见 `bench/gc/PAUSE-CORPUS.md` §1（N=100000、3 次取中位、`taskset -c 6`）。

## 6. 未覆盖与残留风险

- 停顿数字是**同一规模内**的（argv 形状一律 N=100000；固定规模形状按自带规模）⇒ 组间比较
  只在同一口径下成立，**不做跨规模外推** ✗。
- "单一规则"是按**规则**判的（诊断行号前缀不算另一条规则；`byval_temp` 的两行是同一句话）；
  多规则形状要同时改 2 条以上才可能 idle，本表的"可消 µs"对它们一律不计入（保守）。
- `可修性/改动量` 是**判断**，依据是 `docs/RECLAMATION.md` 第五节已有的规划条目与会话内的
  成功先例（task-16/17），不是实测；真正落地哪条要在下一件里用动态安全网验。
- 命中集合里 `cross_region`/`scale_growth` 是**绊线**（判 idle 会让 run-switch ①c 的"登记真的在发"
  落空 / task-17 实测界涨到 2.12×）⇒ 表 A「备注」列标**绊线**、净收益列把它们扣掉。
  `live_churn` **不是**死结：它需要「被丢对象另有根」的新事实（现成 (b′) 在它上面实测 2.07×），
  表 A 标的是**需新事实**，不要与绊线混为一谈。
