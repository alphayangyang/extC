# 全语料停顿画像（**两版编译器**）

本文件有两节：**A = 上一版编译器（`task-29`，md5 `180e74b9…`）的原始记录**（保留、只整体下沉一级标题，
数字与结论**一字未改** ✓）；**B = 新编译器复测（`task-39`，md5 `e4c3a869…`）**。
两节的**口径完全相同**（7 目录 · `taskset -c 6` · 每形状 3 次中位 · argv 形状 N=100000 · `PAUSE_DIST` + 占比），
所以**可以直接对比** ✓。会漂的数字由生成器重跑（`bench/gc/pause_corpus.py`），**不手改旧表** ✓。

---

## A. 上一版编译器（`task-29` · md5 `180e74b947d4815bc16ef7a5858f8183`）—— 原始记录

## 全语料停顿画像（**两版编译器**）

本文件有两节：**A = 上一版编译器（`task-29`，md5 `180e74b9…`）的原始记录**（保留、只整体下沉一级标题，
数字与结论**一字未改** ✓）；**B = 新编译器复测（`task-39`，md5 `e4c3a869…`）**。
两节的**口径完全相同**（7 目录 · `taskset -c 6` · 每形状 3 次中位 · argv 形状 N=100000 · `PAUSE_DIST` + 占比），
所以**可以直接对比** ✓。会漂的数字由生成器重跑（`bench/gc/pause_corpus.py`），**不手改旧表** ✓。

---

### A. 上一版编译器（`task-29` · md5 `180e74b947d4815bc16ef7a5858f8183`）—— 原始记录

### 全语料**停顿画像**（task-29）—— 哪些形状真的有停顿问题

> **只测量，不改任何判决/发射** ✓。本文件由 `bench/gc/pause_corpus.py` 的落盘数据生成；
> 原始行：`/tmp/gcbench/pause-corpus/cur/rows.tsv`（当前编译器）、`pin3/rows.tsv`（pin3）、
> `wcc/wcc.tsv`（活集结构）；每次运行的**原始统计行**在 `cur/raw_PAUSE_DIST.log`（含 `all=[…]` 全序列）。
> 文档数字**只从这些落盘文件抄**，不另算 ✗。

#### 0. 一句话结论（**要打的目标形状**）

在 `digraph` 之外，**真正值得换靶的是三类**：
1. **量级最大**：`tests/reclaim-shapes/persist_nodes.extc` —— max **2344 µs**（两代编译器都 ~2.3–2.6 ms，
   是本表唯一过 ms 的）；WCC 显示 `removed=0` ⇒ **活集只涨不减**（垃圾真堆在标记集里 ✗）。
2. **占比最高的一族**：`owed_home`（**47%**，WCC 723‰、`inter=0` ⇒ 每次活集**整体换新**）、
   `cross_region_elem`（**56%**）、`slot_elem_known`（**30%**）、`cross_region`（**17%**）——
   这一族的问题**不是**回收正确性，而是**活集结构**（整体换新 / 高度碎片化）⇒ 决定“能不能切、能不能增量”。
3. **最有戏的一格**：`cyclic_pressure` —— WCC 最大分量只有 **10‰**（高度碎片化）+ 相邻两次 `collect`
   保留 **71%**（inter/marked）⇒ “**按引用闭包切**”或“**只重扫增量**”在这格最有希望 ✓；
   相反 `digraph`（WCC **1000‰**、76% 换新）与 `owed_home`（723‰、0% 保留）**都不适合切** ✗。

**反过来也要记**：`bench/gc/src/churn|rebuild` 在 pin3 下还在回收档（max 619/355 µs、占比 42%/41%），
**当前编译器已判 idle（结构性 0 次 collect）** ⇒ 编译器进步已把它们从这张表里拿掉 ✓。

#### 1. 口径（写清才能比 ✓）

| 项 | 值 |
|---|---|
| 编译器 | **当前** `build/extc` md5 **180e74b947d4815bc16ef7a5858f8183**（主表）；对照 **pin3** `69a5b11962a0630735037abb2a840109` |
| 形状范围 | `tests/reclaim-perf/shapes` · `tests/reclaim-shapes` · `tests/reclaim-gaps/shapes` · `bench/bigmatrix/src` · `bench/gc/src` · `bench/heavy` · `bench/app`（含 `gate-rc-idle-corpus` 固定清单的全部源 ✓） |
| 分类口径 | 生成物里 `extc_rc_` 的**整串出现次数**（`grep -o` 计数，不是行数）：0 ⇒ 不发 rc 运行期（结构性 0） |
| 规模 | 带 `main(args)` 的形状给 **N=100000**；给 N 会 trap 的（如 `home_cycle` 要 `rounds:per`）走 argv 候选梯 `N` → `1 N` → `N N` → 无 argv，取第一个 rc=0 的（**表里注明** ✓）；不带 argv 的按**源自带固定规模** |
| 插桩 | `tools/rc-pause-probe.py` **只插生成物副本** ✓；`DIGRAPH_PAUSE_DIST=1`，统计行只写 **stderr** |
| 采样 | 每形状 **3 次** · `taskset -c 6` · 取**中位**；同时量墙钟 ⇒ `占比 = 总停顿 / 墙钟` |
| 不回归 | 原档 stdout 与“插桩档 stdout 去掉 `PROBE ` 行”**逐字节相同** ✓（35/35 行 ok）；原档无诊断 env 时 **stderr = 0 字节** ✓ |
| 门槛 | **max ≥ 100 µs 或 占比 ≥ 1%** ⇒ 命中 **19/35** 个发 rc 的形状 |
| 实测 vs 外推 | 本表**全部实测**（同一规模内）；**不做跨规模外推** ✗（§6.9 已证同一形状 @250k 19.2% vs @1M 11.1%） |

#### 2. 分类：55 个形状 = 35 发 rc + 17 idle + 3 编译不过

**完全不发 rc（结构性 0 次 collect ⇒ 停顿天然为 0，显式报 0 ✓）—— 17 个**：

- `tests/reclaim-perf/shapes/rebuild_per_round.extc`
- `tests/reclaim-shapes/big_slot_reclaim.extc`
- `tests/reclaim-shapes/bounded_churn.extc`
- `tests/reclaim-gaps/shapes/closure_chain.extc`
- `bench/bigmatrix/src/bt.extc`
- `bench/bigmatrix/src/cdq.extc`
- `bench/bigmatrix/src/mandel.extc`
- `bench/bigmatrix/src/radix.extc`
- `bench/bigmatrix/src/rebuild.extc`
- `bench/gc/src/churn.extc`
- `bench/gc/src/rebuild.extc`
- `bench/heavy/bt.extc`
- `bench/heavy/mb.extc`
- `bench/heavy/rs.extc`
- `bench/app/log.extc`
- `bench/app/route.extc`
- `bench/app/session.extc`

**发 rc、但该规模下 0 次 collect 的形状**（**显式报 `n=0`** ✓，不是缺行 ✗）—— 13 个：

``tests/reclaim-gaps/shapes/coro_frame_cycle.extc`` ``tests/reclaim-gaps/shapes/cross_scope_cycle.extc`` ``tests/reclaim-gaps/shapes/ext_frame_cycle.extc`` ``tests/reclaim-gaps/shapes/home_cycle.extc`` ``tests/reclaim-gaps/shapes/match_payload.extc`` ``tests/reclaim-gaps/shapes/own_room_cycle.extc`` ``tests/reclaim-gaps/shapes/par_tls_cycle.extc`` ``tests/reclaim-gaps/shapes/pool_ref_slot.extc`` ``tests/reclaim-shapes/cond_save.extc`` ``tests/reclaim-shapes/cyclic_shape.extc`` ``tests/reclaim-shapes/generic_instance_desc.extc`` ``tests/reclaim-shapes/promote_place.extc`` ``tests/reclaim-shapes/unknown_host.extc``

**编译不过 —— 3 个**（照实列，不计入画像 ✗）：

- `tests/reclaim-shapes/reject_deep_store.extc` —— /home/alphayang/extC_Compiler/tests/reclaim-shapes/reject_deep_store.e
- `tests/reclaim-shapes/reject_deep_store_call.extc` —— /home/alphayang/extC_Compiler/tests/reclaim-shapes/reject_deep_store_c
- `tests/reclaim-gaps/shapes/global_slot.extc` —— /home/alphayang/extC_Compiler/tests/reclaim-gaps/shapes/global_slot.ex
  （`reject_deep_store*` 是**故意编译失败**的判据形状 ✓；`global_slot` 是“全局不能零初始化引用”的已知缺口形状）

两代对照：**当前 35 rc / 17 idle / 3 编译不过** vs **pin3 39 rc / 11 idle / 5 编译不过** ⇒ 差的正是 `churn`/`rebuild` 这类编译器进步 ✓。

#### 3. 全表（35 个发 rc 的形状，名字序；µs，墙钟 ms）

| 形状 | rc_occ | n | p50 | p90 | p99 | **max** | 总停顿 | 墙钟 | **占比** | argv | 命中 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `bench/gc/src/digraph.extc` | 636 | 19 | 120.6 | 127.8 | 134.5 | **139.0** | 2333.6 | 15.9 | **14.64%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/base_call_unknown.extc` | 498 | 3 | 89.5 | 89.5 | 89.5 | **91.0** | 268.0 | 9.4 | **2.85%** | 100000 | - |
| `tests/reclaim-gaps/shapes/byval_temp.extc` | 482 | 1 | 125.1 | 125.1 | 125.1 | **125.1** | 125.1 | 2.1 | **5.97%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/carray_slot.extc` | 481 | 1 | 173.7 | 173.7 | 173.7 | **173.7** | 173.7 | 2.0 | **8.49%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/coro_frame_cycle.extc` | 483 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.1 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/cross_scope_cycle.extc` | 486 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 43.8 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 479 | 1 | 144.4 | 144.4 | 144.4 | **144.4** | 144.4 | 7.5 | **1.93%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/ext_frame_cycle.extc` | 483 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.2 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/home_cycle.extc` | 493 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.8 | **0.00%** | 1 100000 | - |
| `tests/reclaim-gaps/shapes/match_payload.extc` | 486 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.3 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/owcell_garbage.extc` | 485 | 1 | 127.2 | 127.2 | 127.2 | **127.2** | 127.2 | 2.5 | **5.13%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/own_room_cycle.extc` | 485 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 41.7 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/par_tls_cycle.extc` | 482 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.3 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/pool_ref_slot.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.3 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/ring_per_round.extc` | 490 | 1 | 3.4 | 3.4 | 3.4 | **3.4** | 3.4 | 1902.6 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 498 | 15 | 223.0 | 242.9 | 260.9 | **269.9** | 3060.0 | 10.3 | **29.76%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | 498 | 3 | 97.4 | 97.4 | 97.4 | **177.0** | 353.1 | 9.8 | **3.60%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | 484 | 1 | 125.0 | 125.0 | 125.0 | **125.0** | 125.0 | 2.1 | **5.99%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 496 | 14 | 128.0 | 138.8 | 142.0 | **157.8** | 1774.3 | 8.6 | **20.73%** | 100000 | **是** |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 485 | 3 | 126.3 | 126.3 | 126.3 | **134.1** | 385.1 | 2.7 | **14.19%** | 100000 | **是** |
| `tests/reclaim-shapes/big_slot.extc` | 495 | 1 | 2.8 | 2.8 | 2.8 | **2.8** | 2.8 | 4.9 | **0.06%** | none（源自带规模） | - |
| `tests/reclaim-shapes/cond_save.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.0 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/cross_region.extc` | 488 | 2 | 337.4 | 337.4 | 337.4 | **354.0** | 704.0 | 4.1 | **17.03%** | 100000 | **是** |
| `tests/reclaim-shapes/cross_region_elem.extc` | 500 | 74 | 124.8 | 209.0 | 284.2 | **350.3** | 9716.7 | 17.3 | **56.21%** | 100000 | **是** |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 514 | 4 | 453.1 | 500.9 | 500.9 | **505.4** | 1728.6 | 6.6 | **26.14%** | 100000 | **是** |
| `tests/reclaim-shapes/cyclic_shape.extc` | 514 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.0 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/generic_instance_desc.extc` | 488 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.0 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/live_churn.extc` | 487 | 4 | 125.0 | 127.9 | 127.9 | **411.1** | 785.4 | 3.8 | **20.75%** | 100000 | **是** |
| `tests/reclaim-shapes/owed_home.extc` | 491 | 20 | 239.2 | 248.4 | 274.5 | **280.8** | 4882.7 | 10.4 | **47.11%** | none（源自带规模） | **是** |
| `tests/reclaim-shapes/persist_nodes.extc` | 498 | 2 | 296.1 | 296.1 | 296.1 | **2343.7** | 2668.1 | 25.3 | **10.54%** | none（源自带规模） | **是** |
| `tests/reclaim-shapes/promote_place.extc` | 488 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.3 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/scale_growth.extc` | 481 | 1 | 125.8 | 125.8 | 125.8 | **125.8** | 125.8 | 1.9 | **6.44%** | 100000 | **是** |
| `tests/reclaim-shapes/shard_bands.extc` | 517 | 24 | 73.9 | 80.9 | 83.8 | **198.5** | 1944.1 | 12.2 | **16.00%** | 100000 | **是** |
| `tests/reclaim-shapes/struct_copy.extc` | 498 | 14 | 120.2 | 134.1 | 138.6 | **142.9** | 1729.2 | 8.4 | **20.45%** | 100000 | **是** |
| `tests/reclaim-shapes/unknown_host.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.1 | **0.00%** | none（源自带规模） | - |

#### 4. 两版排序（门槛：max ≥ 100 µs 或 占比 ≥ 1%%）

##### 4.1 按 **max 停顿**（Top 12）

| # | 形状 | max µs | p50 | 占比 | 墙钟 ms | WCC 最大分量 |
|---|---|---|---|---|---|---|
| 1 | `tests/reclaim-shapes/persist_nodes.extc` | **2343.7** | 296.1 | 10.5% | 25.3 | 213‰ / inter=20271 of 138870 |
| 2 | `tests/reclaim-shapes/cyclic_pressure.extc` | **505.4** | 453.1 | 26.1% | 6.6 | 10‰ / inter=106226 of 150136 |
| 3 | `tests/reclaim-shapes/live_churn.extc` | **411.1** | 125.0 | 20.7% | 3.8 | 999‰ / inter=0 of 65537 |
| 4 | `tests/reclaim-shapes/cross_region.extc` | **354.0** | 337.4 | 17.0% | 4.1 | 0‰ / inter=0 of 65537 |
| 5 | `tests/reclaim-shapes/cross_region_elem.extc` | **350.3** | 124.8 | 56.2% | 17.3 | 1‰ / inter=1083063 of 1179662 |
| 6 | `tests/reclaim-shapes/owed_home.extc` | **280.8** | 239.2 | 47.1% | 10.4 | 723‰ / inter=0 of 400000 |
| 7 | `tests/reclaim-gaps/shapes/slot_elem_known.extc` | **269.9** | 223.0 | 29.8% | 10.3 | 0‰ / inter=155292 of 250110 |
| 8 | `tests/reclaim-shapes/shard_bands.extc` | **198.5** | 73.9 | 16.0% | 12.2 | 111‰ / inter=0 of 1944 |
| 9 | `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | **177.0** | 97.4 | 3.6% | 9.8 | — |
| 10 | `tests/reclaim-gaps/shapes/carray_slot.extc` | **173.7** | 173.7 | 8.5% | 2.0 | — |
| 11 | `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | **157.8** | 128.0 | 20.7% | 8.6 | 0‰ / inter=6255 of 76214 |
| 12 | `tests/reclaim-gaps/shapes/dyn_payload.extc` | **144.4** | 144.4 | 1.9% | 7.5 | 0‰ / inter=0 of 0 |

##### 4.2 按 **总停顿占墙钟比**（Top 12）

| # | 形状 | 占比 | max µs | n | 墙钟 ms | WCC 最大分量 |
|---|---|---|---|---|---|---|
| 1 | `tests/reclaim-shapes/cross_region_elem.extc` | **56.21%** | 350.3 | 74 | 17.3 | 1‰ / inter=1083063 of 1179662 |
| 2 | `tests/reclaim-shapes/owed_home.extc` | **47.11%** | 280.8 | 20 | 10.4 | 723‰ / inter=0 of 400000 |
| 3 | `tests/reclaim-gaps/shapes/slot_elem_known.extc` | **29.76%** | 269.9 | 15 | 10.3 | 0‰ / inter=155292 of 250110 |
| 4 | `tests/reclaim-shapes/cyclic_pressure.extc` | **26.14%** | 505.4 | 4 | 6.6 | 10‰ / inter=106226 of 150136 |
| 5 | `tests/reclaim-shapes/live_churn.extc` | **20.75%** | 411.1 | 4 | 3.8 | 999‰ / inter=0 of 65537 |
| 6 | `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | **20.73%** | 157.8 | 14 | 8.6 | 0‰ / inter=6255 of 76214 |
| 7 | `tests/reclaim-shapes/struct_copy.extc` | **20.45%** | 142.9 | 14 | 8.4 | 0‰ / inter=6255 of 76214 |
| 8 | `tests/reclaim-shapes/cross_region.extc` | **17.03%** | 354.0 | 2 | 4.1 | 0‰ / inter=0 of 65537 |
| 9 | `tests/reclaim-shapes/shard_bands.extc` | **16.00%** | 198.5 | 24 | 12.2 | 111‰ / inter=0 of 1944 |
| 10 | `bench/gc/src/digraph.extc` | **14.64%** | 139.0 | 19 | 15.9 | 1000‰ / inter=3706 of 112156 |
| 11 | `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | **14.19%** | 134.1 | 3 | 2.7 | 1000‰ / inter=1 of 6 |
| 12 | `tests/reclaim-shapes/persist_nodes.extc` | **10.54%** | 2343.7 | 2 | 25.3 | 213‰ / inter=20271 of 138870 |

##### 4.3 门槛命中（19 个）

- `tests/reclaim-shapes/persist_nodes.extc`（max 2344 µs / 占比 10.5%）
- `tests/reclaim-shapes/cyclic_pressure.extc`（max 505 µs / 占比 26.1%）
- `tests/reclaim-shapes/live_churn.extc`（max 411 µs / 占比 20.7%）
- `tests/reclaim-shapes/cross_region.extc`（max 354 µs / 占比 17.0%）
- `tests/reclaim-shapes/cross_region_elem.extc`（max 350 µs / 占比 56.2%）
- `tests/reclaim-shapes/owed_home.extc`（max 281 µs / 占比 47.1%）
- `tests/reclaim-gaps/shapes/slot_elem_known.extc`（max 270 µs / 占比 29.8%）
- `tests/reclaim-shapes/shard_bands.extc`（max 198 µs / 占比 16.0%）
- `tests/reclaim-gaps/shapes/slot_elem_unknown.extc`（max 177 µs / 占比 3.6%）
- `tests/reclaim-gaps/shapes/carray_slot.extc`（max 174 µs / 占比 8.5%）
- `tests/reclaim-gaps/shapes/struct_copy_slot.extc`（max 158 µs / 占比 20.7%）
- `tests/reclaim-gaps/shapes/dyn_payload.extc`（max 144 µs / 占比 1.9%）
- `tests/reclaim-shapes/struct_copy.extc`（max 143 µs / 占比 20.5%）
- `bench/gc/src/digraph.extc`（max 139 µs / 占比 14.6%）
- `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc`（max 134 µs / 占比 14.2%）
- `tests/reclaim-gaps/shapes/owcell_garbage.extc`（max 127 µs / 占比 5.1%）
- `tests/reclaim-shapes/scale_growth.extc`（max 126 µs / 占比 6.4%）
- `tests/reclaim-gaps/shapes/byval_temp.extc`（max 125 µs / 占比 6.0%）
- `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc`（max 125 µs / 占比 6.0%）

⚠️ **占比要连着墙钟读**：这批形状**本身很小**（墙钟 2–152 ms、`n` 少到 1–488 次）⇒ “占比高”多半是**分母小** ✗；
判断手感要看 **max 的绝对量级**（多数 100–500 µs，只有 `persist_nodes` 过 ms）。

#### 5. 活集结构（可选，`tools/rc-live-components.py`，Top 形状）

| 形状 | collect | Σ活集 | 保留 inter | 新增 | 消失 | 两次间新分配 | **最大分量占活集** | 读法 |
|---|---|---|---|---|---|---|---|---|
| `bench/gc/src/digraph.extc` | 19 | 112156 | 3706（3%） | 108450 | 102398 | 309251 | **1000‰** | **不可切**（一个大分量） |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 1 | 0 | 0（0%） | 0 | 0 | 65537 | **0‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 15 | 250110 | 155292（62%） | 94818 | 64095 | 294921 | **0‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 14 | 76214 | 6255（8%） | 69959 | 60340 | 294930 | **0‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 3 | 6 | 1（17%） | 5 | 3 | 196607 | **1000‰** | **不可切**（一个大分量） |
| `tests/reclaim-shapes/cross_region.extc` | 2 | 65537 | 0（0%） | 65537 | 0 | 131075 | **0‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-shapes/cross_region_elem.extc` | 74 | 1179662 | 1083063（92%） | 96599 | 95932 | 296923 | **1‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 4 | 150136 | 106226（71%） | 43910 | 1901 | 119097 | **10‰** | **碎片化 ⇒ 可切** |
| `tests/reclaim-shapes/live_churn.extc` | 4 | 65537 | 0（0%） | 65537 | 0 | 296611 | **999‰** | Σ活集=0 ⇒ collect 什么都没保住 |
| `tests/reclaim-shapes/owed_home.extc` | 20 | 400000 | 0（0%） | 400000 | 380000 | 1220000 | **723‰** | **不可切**（一个大分量） |
| `tests/reclaim-shapes/persist_nodes.extc` | 2 | 138870 | 20271（15%） | 118599 | 0 | 118599 | **213‰** | 中间态；removed=0 ⇒ 只涨不减 |
| `tests/reclaim-shapes/shard_bands.extc` | 24 | 1944 | 0（0%） | 1944 | 1863 | 784593 | **111‰** | 中间态 |
| `tests/reclaim-shapes/struct_copy.extc` | 14 | 76214 | 6255（8%） | 69959 | 60340 | 294930 | **0‰** | **碎片化 ⇒ 可切** |

#### 6. 判据证据

- **反瞎（缺行会红）** ✓：① 单元：`python3 bench/gc/pause_corpus.py --selftest` = 缺行/格式错 **两格都红** ✓；
  ② **端到端注入**：`--only cyclic_pressure --inject-missing`（**不设** `DIGRAPH_PAUSE_DIST`）⇒ 该行 `no_stat`、
  脚本打印 `FAIL：1 个形状取不到 PAUSE_DIST（缺行 ✗）` 且 **rc=1** ✓（原始输出在 `/tmp/gcbench/pause-corpus/inject/`）。
- **“真的没有 collect”显式报 0** ✓：`idle` 行在 TSV 里 `n=0`（不是缺行）；发 rc 但该规模没清扫的形状同样出 `n=0` 的合法统计行。
- **不回归** ✓：35/35 发 rc 行“产物逐字节相同 + 原档 stderr 0 字节”全 ok；探针只插 `/tmp` 副本，`src/` 未动 ✗。
- **电池**（见 §8）：`docs` 12/0 ✓、`smoke` 16/0 ✓；**孤儿 0** ✓（`pgrep` 收尾检查）。

#### 7. 未覆盖与残留风险

- **规模单一**：argv 形状一律 N=100000（固定规模形状按各自默认）⇒ 本表是**同一规模内的排序** ✓，
  不是“这些形状在任何规模都这样” ✗；跨规模已验证过反例（§6.9 digraph @250k vs @1M）。
- **单核（core 6）、单 seed**；每个形状 3 次取中位（`n` 很小的形状，分位本身抖动大）。
- **wall 含进程启动**（约 1–40 ms，见 §6.10 的启动探针）⇒ 小形状的“占比”分母被抬高，读数偏保守。
- **3 个形状编译不过**（其中 2 个是故意）⇒ 它们**没有**画像 ✗；WCC 只覆盖 11 个 Top 形状（其余未做，非 0）。
- **不计入判决** ✓：本普查只测量，未改任何发射/回收逻辑，也未改 `src/`、`tests/`。

#### 8. 复现命令（原文）

```bash
### 当前编译器（主表）
python3 bench/gc/pause_corpus.py --compiler /home/alphayang/extC_Compiler/build/extc \
        --n 100000 --reps 3 --timeout 45 --out /tmp/gcbench/pause-corpus/cur
### 对照 pin3
python3 bench/gc/pause_corpus.py --compiler /tmp/gcbench/extc.pin3 --n 100000 --reps 3 \
        --timeout 45 --out /tmp/gcbench/pause-corpus/pin3
### 反瞎注入（必须 rc=1 + 该行 no_stat）
python3 bench/gc/pause_corpus.py --only cyclic_pressure --inject-missing --reps 1 \
        --out /tmp/gcbench/pause-corpus/inject; echo rc=$?
```

---

#
## B. 新编译器复测（`task-39` · md5 `e4c3a86960149e2d89dc22b9c1ebdef0` · 2026-10-14）

### B.1 口径（与 A 相同 ✓ + 一条**实测出来的铁律** ✓）

| 项 | 值 |
|---|---|
| 编译器 | **md5 `e4c3a86960149e2d89dc22b9c1ebdef0`**（= `build/extc` = `/tmp/extc.pin38new` 同 md5 ✓） |
| **跑的位置** | **树内 `build/extc`** ✓ —— 本轮实测：**同一个 md5 放在 `/tmp` 会让 2 个形状报错** ✗（见 B.4.1）；表里数字来自**树内**那次运行（`new2/`） |
| 形状数 | **57**（比 A 多 2 个：`tests/reclaim-perf/shapes/{field_overwrite_lost_root,fixed_slot_overwrite}.extc`，本轮期间新增的源） |
| 规模/重复 | argv 形状 **N=100000** · 每形状 **3 次取中位** · `taskset -c 6` · argv 候选梯同 A |
| 门槛 | **max ≥ 100 µs 或 占比 ≥ 1%** ⇒ 命中 **20/35**（**重算值** ✓；早先写的 `18/35` 是门槛实现 bug，见 B.5 的修正块 ✗✓） |
| 原始行 | 树内：`new2/raw_PAUSE_DIST.log`；`/tmp` 副本那次：`new/raw_PAUSE_DIST.log`；WCC = `wcc3/wcc.tsv` |
| 复现 | `python3 bench/gc/pause_corpus.py --compiler /home/alphayang/extC_Compiler/build/extc --n 100000 --reps 3 --timeout 45 --out /tmp/gcbench/pause-corpus/new2` ✓（**注意：用树内编译器**；若要复制到 /tmp，见 B.4.1 的铁律 ✓） |

### B.2 分类：57 形状 = **35 发 rc + 19 idle + 3 编译不过**

**逐形状差分（A → B）**：

| 形状 | A | B | 说明 |
|---|---|---|---|
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | rc | **idle** | A 里 125.0 µs ⇒ **归零** ✓ |
| `tests/reclaim-perf/shapes/field_overwrite_lost_root.extc` | — | **rc** | 本轮新增源 ⇒ 发 rc |
| `tests/reclaim-perf/shapes/fixed_slot_overwrite.extc` | — | **rc** | 本轮新增源 ⇒ 发 rc |
| `tests/reclaim-shapes/persist_nodes.extc` | rc | **idle** | A 的 **max 榜首（2343.7 µs）** ⇒ **归零** ✓ |

⇒ **35 rc / 19 idle / 3 编译不过**（A 是 35/17/3）：**rc 数逐项对得上** ✓
（A 的 35 − 2 个归零 + 2 个新增源 = **35** ✓；idle 17 + 2 = **19** ✓；编译不过 3 = 2 个故意 + `global_slot` ✓）。

> **与 `/tmp` 副本那次的差别**：副本那次是 **33 rc / 19 idle / 5 编译不过** ✗ —— 差的 2 个"编译不过"**全是环境性失败**（见 B.4.1），**不是** rc/idle 的分类漂移 ✓（那 2 个形状在树内是 rc ✓）。

### B.3 两个关键形状：`persist_nodes` 与真实语料 `p3834_nodes` —— **显式“无 collect”，不伪造 0** ✓

探针的既有行为是**拒绝伪造 0** ✓（找不到回收运行期就报错），所以这两行如实写成**两个独立信号**：

| 形状 | 产物里 `extc_rc_` 记号 | 探针 | 结论 |
|---|---|---|---|
| `tests/reclaim-shapes/persist_nodes.extc` | **0** | **拒绝插桩**（`插桩失败：找不到回收运行期`，rc=1） | 结构性无停顿 ✓（A 里是 2343.7 µs ✗） |
| `bench/oi/persist/p3834_nodes.extc`（**真实语料**） | **0** | **拒绝插桩**（同上，rc=1） | 结构性无停顿 ✓ |

**`p3834_nodes` 真实负载实测（`taskset -c 6`，垫片给内核峰值 RSS）**：

| 运行 | 墙钟 ms | 峰值 RSS KB |
|---|---|---|
| run1 | 3087.0 | 508332 |
| run2 | 3005.7 | 508332 |
| run3 | 3039.2 | 508332 |
| **中位/峰值** | **3039.2 ms（3.04 s）** | **508332 KB = 508.3 MB** |
| `EXTC_RECLAIM=0` 档（对照） | 3035.1 | 508332 |

- **产物对照**：默认档 vs `EXTC_RECLAIM=0` 档 **逐字节相同** ✓（5 行，sha `350f7e0191eae07b`）⇒
  “回收档与不回收档产生**同一个二进制行为**” = “**结构性 0**”的最硬证据 ✓；
- **旧值我没有复现**（手上**没有** `180e74b9` 的编译器副本 ✗）⇒ 旧值（4.33 s / 765.9 MB）标注为 **lead/task-36–38 记录**；
  新值（3.04 s / 508.3 MB）是我自己量的 ✓ ⇒ 墙钟 ↓30%%、峰值 RSS ↓34%%（按 lead 记录算；我只对 B 侧负责 ✓）。

### B.4 全表（35 个发 rc 的形状，名字序；µs / 墙钟 ms）

| 形状 | rc_occ | n | p50 | p90 | p99 | **max** | 总停顿 | 墙钟 | **占比** | argv | 命中 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `bench/gc/src/digraph.extc` | 636 | 19 | 121.0 | 123.9 | 128.9 | **180.2** | 2356.6 | 15.8 | **14.88%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/base_call_unknown.extc` | 498 | 3 | 92.7 | 92.7 | 92.7 | **97.4** | 283.8 | 9.7 | **2.91%** | 100000 | - |
| `tests/reclaim-gaps/shapes/byval_temp.extc` | 482 | 1 | 125.4 | 125.4 | 125.4 | **125.4** | 125.4 | 2.1 | **6.02%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/carray_slot.extc` | 481 | 1 | 126.3 | 126.3 | 126.3 | **126.3** | 126.3 | 2.0 | **6.27%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/coro_frame_cycle.extc` | 483 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.1 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/cross_scope_cycle.extc` | 486 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 41.9 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 479 | 1 | 144.2 | 144.2 | 144.2 | **144.2** | 144.2 | 7.6 | **1.90%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/ext_frame_cycle.extc` | 483 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.1 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/home_cycle.extc` | 493 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.9 | **0.00%** | 1 100000 | - |
| `tests/reclaim-gaps/shapes/match_payload.extc` | 486 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.2 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/owcell_garbage.extc` | 485 | 1 | 125.6 | 125.6 | 125.6 | **125.6** | 125.6 | 2.5 | **4.93%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/own_room_cycle.extc` | 485 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 41.7 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/par_tls_cycle.extc` | 482 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.4 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/pool_ref_slot.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 2.2 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/ring_per_round.extc` | 490 | 1 | 3.6 | 3.6 | 3.6 | **3.6** | 3.6 | 1893.5 | **0.00%** | 100000 | - |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 498 | 15 | 221.4 | 243.6 | 247.1 | **250.5** | 3063.2 | 10.2 | **30.15%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | 498 | 3 | 92.4 | 92.4 | 92.4 | **106.4** | 285.6 | 9.9 | **2.87%** | 100000 | **是** |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 496 | 14 | 120.5 | 138.7 | 145.2 | **167.5** | 1790.0 | 8.9 | **20.11%** | 100000 | **是** |
| `tests/reclaim-perf/shapes/field_overwrite_lost_root.extc` | 489 | 2 | 157.5 | 157.5 | 157.5 | **197.1** | 354.6 | 3.7 | **9.59%** | 100000 | **是** |
| `tests/reclaim-perf/shapes/fixed_slot_overwrite.extc` | 488 | 1 | 143.7 | 143.7 | 143.7 | **143.7** | 143.7 | 2.3 | **6.35%** | 100000 | **是** |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 485 | 3 | 125.5 | 125.5 | 125.5 | **128.4** | 379.2 | 2.7 | **13.86%** | 100000 | **是** |
| `tests/reclaim-shapes/big_slot.extc` | 495 | 1 | 3.2 | 3.2 | 3.2 | **3.2** | 3.2 | 5.3 | **0.06%** | none（源自带规模） | - |
| `tests/reclaim-shapes/cond_save.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.3 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/cross_region.extc` | 488 | 2 | 360.8 | 360.8 | 360.8 | **378.8** | 742.3 | 4.4 | **16.77%** | 100000 | **是** |
| `tests/reclaim-shapes/cross_region_elem.extc` | 500 | 74 | 125.6 | 210.9 | 272.3 | **282.3** | 9727.5 | 17.5 | **55.63%** | 100000 | **是** |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 514 | 4 | 405.0 | 469.9 | 469.9 | **491.5** | 1648.7 | 6.4 | **25.85%** | 100000 | **是** |
| `tests/reclaim-shapes/cyclic_shape.extc` | 514 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.2 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/generic_instance_desc.extc` | 488 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.4 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/live_churn.extc` | 487 | 4 | 124.7 | 129.0 | 129.0 | **411.5** | 788.2 | 3.8 | **21.03%** | 100000 | **是** |
| `tests/reclaim-shapes/owed_home.extc` | 491 | 20 | 240.3 | 249.6 | 254.4 | **292.7** | 4886.2 | 10.4 | **47.02%** | none（源自带规模） | **是** |
| `tests/reclaim-shapes/promote_place.extc` | 488 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.4 | **0.00%** | none（源自带规模） | - |
| `tests/reclaim-shapes/scale_growth.extc` | 481 | 1 | 124.1 | 124.1 | 124.1 | **124.1** | 124.1 | 2.1 | **5.95%** | 100000 | **是** |
| `tests/reclaim-shapes/shard_bands.extc` | 517 | 24 | 73.1 | 76.1 | 80.4 | **91.7** | 1788.6 | 11.5 | **15.50%** | 100000 | - |
| `tests/reclaim-shapes/struct_copy.extc` | 498 | 14 | 122.6 | 141.1 | 142.8 | **148.6** | 1789.4 | 8.5 | **21.07%** | 100000 | **是** |
| `tests/reclaim-shapes/unknown_host.extc` | 481 | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | 1.2 | **0.00%** | none（源自带规模） | - |

**idle（结构性 0，显式报 0 ✓）19 个**：

- `tests/reclaim-perf/shapes/rebuild_per_round.extc`
- `tests/reclaim-shapes/big_slot_reclaim.extc`
- `tests/reclaim-shapes/bounded_churn.extc`
- `tests/reclaim-shapes/persist_nodes.extc`
- `tests/reclaim-gaps/shapes/closure_chain.extc`
- `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc`
- `bench/bigmatrix/src/bt.extc`
- `bench/bigmatrix/src/cdq.extc`
- `bench/bigmatrix/src/mandel.extc`
- `bench/bigmatrix/src/radix.extc`
- `bench/bigmatrix/src/rebuild.extc`
- `bench/gc/src/churn.extc`
- `bench/gc/src/rebuild.extc`
- `bench/heavy/bt.extc`
- `bench/heavy/mb.extc`
- `bench/heavy/rs.extc`
- `bench/app/log.extc`
- `bench/app/route.extc`
- `bench/app/session.extc`

**编译不过 3 个**（**全部非环境性** ✓）：

- `tests/reclaim-shapes/reject_deep_store.extc` —— `/home/alphayang/extC_Compiler/tests/reclaim-shapes/reject_deep_store.e`
- `tests/reclaim-shapes/reject_deep_store_call.extc` —— `/home/alphayang/extC_Compiler/tests/reclaim-shapes/reject_deep_store_c`
- `tests/reclaim-gaps/shapes/global_slot.extc` —— `/home/alphayang/extC_Compiler/tests/reclaim-gaps/shapes/global_slot.ex`
  （`reject_deep_store*` 是**故意编译失败**的判据形状 ✓；`global_slot` 是“全局不能零初始化引用”的已知缺口形状）

#### B.4.1 「同一个 md5、换个位置就报错」——**环境性失败**（已归因 ✓，非编译器回归 ✗）

`ext_frame_cycle` / `par_tls_cycle` 在 **`/tmp` 副本**下 rc=1，在**树内** `build/extc`（**同 md5** ✓）下 rc=0 ✓。
实测矩阵（`-w --no-line-map`，形状 `par_tls_cycle.extc`，`@builtin` 那条诊断）：

| 用法 | 结果 |
|---|---|
| ① 树内 `build/extc` + **相对** `-I stdlib` | ✗ rc=1（`@builtin declarations are standard-library only`） |
| ② 树内 `build/extc` + **绝对** `-I <树>/stdlib` | ✓ rc=0（482 个 rc 记号）← **A 节与本次 B 节都用这种** ✓ |
| ③ `/tmp` 副本 + 相对 `-I` | ✗ rc=1 |
| ④ `/tmp` 副本 + **绝对** `-I` | ✗ rc=1（**只给 `-I` 不够** ✗） |
| ⑤ 树根副本 `./extc_copy` + 相对/绝对 `-I` | ✗ rc=1 |
| ⑥ `/tmp` 副本 + `EXTC_STD=<树>/stdlib` + **无 `-I`** | ✓ rc=0（482） |
| ⑦ `/tmp` 副本 + `EXTC_STD=<树>/stdlib` + 绝对 `-I` | ✓ rc=0（482） |
| ⑧ 树内 + 无 `-I` + 无 `EXTC_STD`（默认） | ✓ rc=0（482） |

**机制**（源码级 ✓）：`src/back/modules.c:1737 isStdlibFile()` 把**模块文件的规范化路径**与加载器的
`stdDir` 比前缀；`stdDir` 默认来自编译器**自己的安装位置**（所以副本放在别处 ⇒ 树里的 stdlib 文件
被当成"非标准库" ⇒ `@builtin` 报错 ✗），而 `-I` 只是**追加搜索目录**、**不改 `stdDir`** ⇒
相对 `-I` 解析出的相对路径永远匹配不上绝对 `stdDir`（连树内都失败 ✗，①）；`EXTC_STD` 才是覆盖 `stdDir` 的开关
（编译器自己的提示里就写了它 ✓，`modules.c:453`）。

⇒ **铁律**（写进复现命令 ✓）：**pin 对照要么用树内 `build/extc` + 绝对 `-I <树>/stdlib`，要么复制到 `/tmp` 时显式设 `EXTC_STD=<树>/stdlib`**；**只给 `-I` 不够** ✗。
影响面：**57 个形状里只有 2 个**（`ext_frame_cycle`、`par_tls_cycle`，都 `use` 了特权 stdlib 模块）受影响，
且这 2 个既不是 rc 也不是 idle ⇒ **对排序与结论无影响** ✓。

### B.5 本阶段的账（`task-43` · **只算账** ✓ · 数字全部由 A/B 表**逐行重算** ✓）

**口径**（两侧完全相同 ✓）：`N=100000`（不带 argv 的形状按自带规模）· `taskset -c 6` · 每形状 **3 次取中位** ·
A = 旧编译器 md5 `180e74b947d4815bc16ef7a5858f8183` · B = 新编译器 md5 `e4c3a86960149e2d89dc22b9c1ebdef0`（树内 ✓）。
**形状集要分开算** ✗：A 是 35 个发 rc（55 形状）、B 是 35 个发 rc（57 形状）——
B 多出的 2 个源是本阶段**新增**的（A 从没量过），而 A 的 2 个形状在 B 里**归零**（不再是 rc）✓。

**① 总账（Σ max，µs；`Σ max` = 所有发 rc 形状的 max 之和）**

| 集合 | 形状数 | Σ max（A） | Σ max（B） | Δ | Δ% |
|---|---|---|---|---|---|
| 共同集 `A∩B` | 33 | 3914.2 | 3679.9 | **−234.3** | **−6.0%** |
| **归零项**（A 是 rc、B 是 idle） | 2 | 2468.7 | **0** | **−2468.7** | **−100%** |
| **like-for-like = A 量过的 35 个形状** | **35** | **6382.9** | **3679.9** | **−2703.0** | **−42.3%** ✓ |
| B 侧**新增源**（A 没有） | 2 | —（A 无） | 340.8 | n/a | n/a |
| **B 现总量（含新增源）** | 35 | — | **4020.7** | — | — |

⇒ **一句话**：这个 goal 把**可对照的停顿预算**从 **6382.9 µs 打到 3679.9 µs（−42.3%）** ✓；
其中 **−2468.7 µs（91.3%）来自两个形状“整类消失”**，**−234.3 µs（8.7%）来自共同集上的常数因子改善** ✓。

**② 辅助口径**

| 指标 | A（旧） | B（新） | 说明 |
|---|---|---|---|
| Σ 总停顿（`total_us` 之和） | 33160.3 | 30680.0（**−7.5%**） | 共同集上 30367.2 → 30181.7 |
| Σ 占比（各形状占比之和） | 334.0% | 328.7% | ⚠️ **比值之和**，不是总占比 ✗；且小形状分母小、**含进程启动**（1–40 ms ✗）⇒ **以 Σ max 为主、占比为辅** ✓ |
| 命中门槛的形状数 | **20/35** | **20/35** | 门槛 = **max ≥ 100 µs 或 占比 ≥ 1%**；**没变** ✗（诚实负结果：本阶段砍掉的是**量级**，不是**命中数**） |

> ⚠️ **口径修正（我自己发现的实现 bug ✗✓）**：本文件早先 A 节/B.1 里的“命中 **18/35**”是用一个**写错的门槛实现**算的
> （占比列是分数，却被拿去与 `1.0` 比较 ⇒ 实际退化成“只测 max ≥ 100 µs”）。按**写明的门槛**重算是 **A 20/35、B 20/35** ✓；
> 边界形状是 `base_call_unknown`（A 91.0 µs / 占比 2.85%）与 `shard_bands`（B 91.7 µs / 占比 15.5%）——
> 这两个只有“占比”那一条会命中 ✓。**A 节按指示一字未动** ✓（B.1 的该行已按重算值标注）。

**③ 归零项单列**（这是本阶段唯一“整类消失”的收益 ✓）

| 项 | A（旧编译器） | B（新编译器） | 证据 |
|---|---|---|---|
| `tests/reclaim-shapes/persist_nodes.extc` | rc，**max 2343.7 µs（A 榜首）** | **结构性 0** ✓ | 产物 `extc_rc_` 记号 **0** ✓ + **探针拒绝插桩**（rc=1 ✓） |
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | rc，max 125.0 µs | **结构性 0** ✓ | 同上 ✓ |
| `bench/oi/persist/p3834_nodes.extc`（**真实语料**，不在普查范围） | 墙钟 **4.33 s** / 峰值 **765.9 MB**（**lead/task-36–38 记录** ✗ 我无 `180e74b9` 副本可复现） | 墙钟 **3.04 s**（**−29.8%**）/ 峰值 **508.3 MB**（**−33.6%**）（**我实测** ✓，3 次：3087.0/3005.7/3039.2 ms，RSS 508332 KB 恒定 ✓） | 默认档与 `EXTC_RECLAIM=0` 档**逐字节相同** ✓（sha `350f7e0191eae07b`） |

**④ 剩余靶子 + “已判不可走”清单（✓ **别当待办** ✗；出处是**落盘文件原文** ✓，不是我转述 lead ✗）**

B 节里**还在发 rc** 的前几名（按 max）：`cyclic_pressure` **491.5** · `live_churn` 411.5 · `cross_region` 378.8 ·
`owed_home` 292.7 · `cross_region_elem` 282.3 · `slot_elem_known` 250.5 · `digraph` 180.2。
**但其中绝大多数已经被判“不可走 / 兑现 0”，下面是第一手清单** ✓（我**没有**复跑这些判决 ✗ —— 本件只算账；要复核请看原文 ✓）：

| 形状 | 判的是什么 | 出处（落盘，可核 ✓） | 状态 |
|---|---|---|---|
| `carray_slot` | `D1`（元素槽 vs 真栈槽）让它**真 idle**，但 **N vs 4N = 2.12× ⇒ 破界** | `docs/CHANGELOG.md:98`；`tests/reclaim-perf/CHAIN-BLOCKERS.md`（链 `D`，绊线体检**不可走**） | **不可走** ✗（实测 ✓） |
| `cyclic_pressure` / `shard_bands` | 链头 **C**；`C1`/`C1u`、`C2`/`C2u` **兑现 0**，真正的链是 **C→D** | `docs/CHANGELOG.md:101–112`（`C2u` 表 + “忠实版 `C2` 是 `C2u` 的真子集 ⇒ 不可能兑现更多”）；`tests/reclaim-perf/NARROW-PROBE-DEH.md`；`CHAIN-BLOCKERS.md` | **不可走** ✗（**实测兑现 0** ✓✓） |
| `live_churn` / `cross_region` / `scale_growth` | 链头 **D**；`D2`（槽 def-use）按“本迭代窗口”实现**会破这三条钉住的对照** | `CHAIN-BLOCKERS.md`（链 `D` ⇒ 不可走）；`tests/reclaim-perf/IDLE-BLOCKERS.md`（这三条**本身就是绊线** ✗） | **不可走** ✗（绊线身份是实测 ✓；`D2` 那一支**只是影响面评估、未实现** ✗，见 `NARROW-PROBE-DEH.md:98` 自述“以下是判断，不是实测”） |
| `slot_elem_unknown` / `digraph` / `owed_home` / `struct_copy_slot` | 链含 **D**，或 **F 的站点在循环里** | `CHAIN-BLOCKERS.md` 的逐形状完整链（`B→D` / `D→C→E→B` / `E→D` / `F→A→D`） | **不可走** ✗（实测链 ✓） |
| `base_call_unknown` | 基表达式带调用 ⇒ **仍然是 B**；且 max **91 µs < 100 µs** 门槛 | `CHAIN-BLOCKERS.md`（链 `B`，stuck）；本文件 B.4 表 | **不在靶上** ✓ |

⚠️ **两条“新面孔”要分清** ✗：`field_overwrite_lost_root`（197.1 µs）与 `fixed_slot_overwrite`（143.7 µs）是 **`task-38` 加的专用对照** ✓ ——
它们**必须继续发 rc**，所以**不是待优化项** ✓（本阶段新增，A 节没有它们 ✓）。

**⑤ 实测 vs 预测（分开标 ✗）**
本节**所有数字都是复算实测** ✓（A/B 两张表的逐行值，脚本 `bench/gc/pause_corpus.py` 的落盘 TSV）；
lead 的两条**预测**另标：分类预期 `33 rc/19 idle`（实测树内是 `35/19/3`，差在 2 个**环境性**失败，见 B.4.1 ✓）、
新榜首预期 ~505 µs（实测 **491.5 µs** ✓）。

### B.6 两版排序（新编译器，树内运行）

**按 max 停顿（Top 12）**

| # | 形状 | max µs | p50 | 占比 | 墙钟 ms | WCC 最大分量 | A 里 max |
|---|---|---|---|---|---|---|---|
| 1 | `tests/reclaim-shapes/cyclic_pressure.extc` | **491.5** | 405.0 | 25.9% | 6.4 | 10‰ / inter=106226 of 150136 | 505.4 µs |
| 2 | `tests/reclaim-shapes/live_churn.extc` | **411.5** | 124.7 | 21.0% | 3.8 | 999‰ / inter=0 of 65537 | 411.1 µs |
| 3 | `tests/reclaim-shapes/cross_region.extc` | **378.8** | 360.8 | 16.8% | 4.4 | 0‰ / inter=0 of 65537 | 354.0 µs |
| 4 | `tests/reclaim-shapes/owed_home.extc` | **292.7** | 240.3 | 47.0% | 10.4 | 723‰ / inter=0 of 400000 | 280.8 µs |
| 5 | `tests/reclaim-shapes/cross_region_elem.extc` | **282.3** | 125.6 | 55.6% | 17.5 | 1‰ / inter=1083063 of 1179662 | 350.3 µs |
| 6 | `tests/reclaim-gaps/shapes/slot_elem_known.extc` | **250.5** | 221.4 | 30.1% | 10.2 | 0‰ / inter=155292 of 250110 | 269.9 µs |
| 7 | `tests/reclaim-perf/shapes/field_overwrite_lost_root.extc` | **197.1** | 157.5 | 9.6% | 3.7 | 0‰ / inter=0 of 0 | （idle/无） |
| 8 | `bench/gc/src/digraph.extc` | **180.2** | 121.0 | 14.9% | 15.8 | 1000‰ / inter=3706 of 112156 | 139.0 µs |
| 9 | `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | **167.5** | 120.5 | 20.1% | 8.9 | 0‰ / inter=6255 of 76214 | 157.8 µs |
| 10 | `tests/reclaim-shapes/struct_copy.extc` | **148.6** | 122.6 | 21.1% | 8.5 | 0‰ / inter=6255 of 76214 | 142.9 µs |
| 11 | `tests/reclaim-gaps/shapes/dyn_payload.extc` | **144.2** | 144.2 | 1.9% | 7.6 | 0‰ / inter=0 of 0 | 144.4 µs |
| 12 | `tests/reclaim-perf/shapes/fixed_slot_overwrite.extc` | **143.7** | 143.7 | 6.4% | 2.3 | 500‰ / inter=0 of 2 | （idle/无） |

**按总占比（Top 12）**

| # | 形状 | 占比 | max µs | n | 墙钟 ms | WCC 最大分量 | A 里 占比 |
|---|---|---|---|---|---|---|---|
| 1 | `tests/reclaim-shapes/cross_region_elem.extc` | **55.63%** | 282.3 | 74 | 17.5 | 1‰ / inter=1083063 of 1179662 | 56.21% |
| 2 | `tests/reclaim-shapes/owed_home.extc` | **47.02%** | 292.7 | 20 | 10.4 | 723‰ / inter=0 of 400000 | 47.11% |
| 3 | `tests/reclaim-gaps/shapes/slot_elem_known.extc` | **30.15%** | 250.5 | 15 | 10.2 | 0‰ / inter=155292 of 250110 | 29.76% |
| 4 | `tests/reclaim-shapes/cyclic_pressure.extc` | **25.85%** | 491.5 | 4 | 6.4 | 10‰ / inter=106226 of 150136 | 26.14% |
| 5 | `tests/reclaim-shapes/struct_copy.extc` | **21.07%** | 148.6 | 14 | 8.5 | 0‰ / inter=6255 of 76214 | 20.45% |
| 6 | `tests/reclaim-shapes/live_churn.extc` | **21.03%** | 411.5 | 4 | 3.8 | 999‰ / inter=0 of 65537 | 20.75% |
| 7 | `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | **20.11%** | 167.5 | 14 | 8.9 | 0‰ / inter=6255 of 76214 | 20.73% |
| 8 | `tests/reclaim-shapes/cross_region.extc` | **16.77%** | 378.8 | 2 | 4.4 | 0‰ / inter=0 of 65537 | 17.03% |
| 9 | `tests/reclaim-shapes/shard_bands.extc` | **15.50%** | 91.7 | 24 | 11.5 | 111‰ / inter=0 of 1944 | 16.00% |
| 10 | `bench/gc/src/digraph.extc` | **14.88%** | 180.2 | 19 | 15.8 | 1000‰ / inter=3706 of 112156 | 14.64% |
| 11 | `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | **13.86%** | 128.4 | 3 | 2.7 | 1000‰ / inter=1 of 6 | 14.19% |
| 12 | `tests/reclaim-perf/shapes/field_overwrite_lost_root.extc` | **9.59%** | 197.1 | 2 | 3.7 | 0‰ / inter=0 of 0 | （idle/无） |

### B.7 一句话结论（**新编译器下“要打的目标形状”**）

- **新的 max 榜首是 `cyclic_pressure`（491.5 µs，占比 25.9%）** ✓（原榜首 `persist_nodes` 已归零 ✗）；
  **但 `task-34`/`task-37` 已判它“不可走”** ⇒ 榜首 ≠ 可动的靶子 ✗（如实记录 ✓）。
- **占比最高的一族没变**：`cross_region_elem`（55.6%）、`owed_home`（47.0%）、`slot_elem_known`（30.1%）——
  问题仍是**活集结构**（整体换新 / 碎片化），不是回收正确性。
- **活集结构上最有戏的仍是 `cyclic_pressure`**（WCC **10‰**、保留 **71%** ⇒ 可切/可增量 ✓），
  但**不可走** ⇒ 结论落在“**结构上可切、按判决不值得动**” ✓。
- **`digraph`**：max 180.2 µs / 占比 14.9%（A 里 139.0 µs / 14.6%），WCC 仍 **1000‰**（不可切）✓。
- **真实负载首次“停顿归零”实证** ✓：`p3834_nodes` 从“有 collect”变成**结构性 0**，实测 **3.04 s / 508.3 MB**，
  且与 `EXTC_RECLAIM=0` 档**逐字节相同 + RSS 相同** ✓。

### B.8 B 节的未覆盖与风险

- **规模单一**（N=100000）、**单核 core 6、单 seed**；**不做跨规模外推** ✗。
- `p3834_nodes` 的**旧值未复现**（无 `180e74b9` 副本 ✗）⇒ 旧值来自 lead 记录，我只对 B 侧数字负责 ✓。
- 本轮学到 **pin 副本的位置会改变行为** ✗（B.4.1）：以后每次 pin 对照都要按铁律跑 ✓；
  本表的树内运行已按铁律 ✓，`/tmp` 那次只作环境证据保留 ✓。
- WCC 只覆盖 **14 个 Top 形状**（其余未做，非 0）。
- 只测量：不改判决/发射，未动 `src/`、`tests/` ✗；**不承诺“没有任何新风险”**。
