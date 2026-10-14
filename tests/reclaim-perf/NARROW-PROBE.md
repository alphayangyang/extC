# 按「细分」重扫：每条细分能安全兑现多少停顿（task-37）

> **只勘察**：细分全在 `/tmp` 副本树里（`EXTC_PROBE_NARROW`）；`src/` 零改动、`build/extc` 不变、产物逐字节不变。停顿数字抄自 `bench/gc/PAUSE-CORPUS.md`（同规模口径，不外推）。
> 副本编译器：`/tmp/narrow/build/extc`。**每格都是实测**（编译产物数 `extc_rc_`），不是判断。

## 0. 细分假设（每条写清「窄在哪里」）

| 细分 | 可信 | 估计改动量 | 窄在哪里 |
|---|---|---|---|
| `NA2` | 是 | 4 | A：只放行『被覆盖的旧值另有根』（值 = 形参的投影、本帧不写该宿主字段） |
| `NB` | 是 | 2 | B：目的地允许非字面量下标（键 `base[]`；同叶槽两处写仍按 A 判） |
| `NE2` | 是 | 3 | E：元素槽**序列**（下标是标识符）不再要求『宿主本迭代新建』 |
| `NA` | **否（上界）** | 0 | A 的上界：两处写一律当互斥（不可信，只量『会怎样』） |
| `NE` | **否（上界）** | 0 | E 的上界：任何元素目的地都放行（不可信） |

## 1. 每形状：需要哪几条细分才 idle（实测）+ 六条对照体检

| 形状 | max µs | 占比 | 需细分 | 细分后 idle | 六条对照仍发 rc |
|---|---|---|---|---|---|
| `tests/reclaim-shapes/persist_nodes.extc` | 2343.7 | 10.54% | NA2+NB+NE2 | 是 | 是 |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 505.4 | 26.14% | — | **否** | — |
| `tests/reclaim-shapes/live_churn.extc` | 411.1 | 20.75% | — | **否** | — |
| `tests/reclaim-shapes/cross_region.extc` | 354.0 | 17.03% | — | **否** | — |
| `tests/reclaim-shapes/cross_region_elem.extc` | 350.3 | 56.21% | — | **否** | — |
| `tests/reclaim-shapes/owed_home.extc` | 280.8 | 47.11% | — | **否** | — |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 269.9 | 29.76% | — | **否** | — |
| `tests/reclaim-shapes/shard_bands.extc` | 198.5 | 16.00% | — | **否** | — |
| `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | 177.0 | 3.60% | — | **否** | — |
| `tests/reclaim-gaps/shapes/carray_slot.extc` | 173.7 | 8.49% | — | **否** | — |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 157.8 | 20.73% | — | **否** | — |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 144.4 | 1.93% | — | **否** | — |
| `tests/reclaim-shapes/struct_copy.extc` | 142.9 | 20.45% | — | **否** | — |
| `bench/gc/src/digraph.extc` | 139.0 | 14.64% | — | **否** | — |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 134.1 | 14.19% | — | **否** | — |
| `tests/reclaim-gaps/shapes/owcell_garbage.extc` | 127.2 | 5.13% | — | **否** | — |
| `tests/reclaim-shapes/scale_growth.extc` | 125.8 | 6.44% | — | **否** | — |
| `tests/reclaim-gaps/shapes/byval_temp.extc` | 125.1 | 5.97% | — | **否** | — |
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | 125.0 | 5.99% | （无需细分：现状） | 是 | 是 |
| `tests/reclaim-gaps/shapes/base_call_unknown.extc` | 91.0 | 2.85% | — | **否** | — |

## 2. 按细分组合聚合（**只算安全兑现**：对照全部仍发 rc）

| 细分组合 | 形状数 | Σmax µs | 占命中 Σmax | 估计改动量 | 形状 |
|---|---|---|---|---|---|
| **NA2+NB+NE2** | 1 | 2343.7 | 36.8% | 9 | `persist_nodes.extc` |
| **（现状）** | 1 | 125.0 | 2.0% | 0 | `struct_copy_ret_home.extc` |

## 3. 一句话结论

**下一件实现（按 Σmax ÷ 估计改动量）**：`NA2+NB+NE2`（Σ **2343.7 µs**，改 9）—— 这是本轮唯一
**新的**安全兑现。另一行空组合（`struct_copy_ret_home`，Σ 125.0 µs）**不是待办**：它就是 task-36
已经落地的 F 细分（现状），本表把它列出来只为显示"现状也能兑现 1 个形状"。

> 收益 = 这些形状的**停顿归零**（②不发射 rc 运行期）；收益数是 `PAUSE-CORPUS.md` 的 max 之和，改动量是**估计**。

## 4. 未覆盖与残留风险

- `NA`/`NE` 是**上界**（把整条条件去掉），只用来标出「最多能到哪」；它们**不是可实现的细分**（`NA` 会放行真正会丢根的覆盖，`NE` 会放行任意元素目的地）。
- 每条细分的**安全**只由六条对照 + 语料产物不变来判；通用性仍靠 `gate-idle-bounded` 的 N vs 4N 动态界兜（实现时要把它加进 SHAPES）。
- 只扫了 20 个命中形状；未命中形状的细分收益未量。
- **C/D/G/H 的细分：没提出、也没实测** —— 它们要的新事实（跨过程宿主、『另有根』的通用形式、
  效果签名）我这一轮没有实现，所以本报告对它们**既不说"安全"也不说"会破对照"**：是**没测到**
  （`evidence-first` 第 5 条：别把"不知道"说成"没问题"）。上表里那 18 个形状的"没有细分组合能
  让它 idle"**只对这 5 条细分成立**。
- 需要新冻结字段的细分（若有）单列在 §5。

## 5. 需要新冻结字段？（影响面待评估）

本轮的细分都只用**只读**事实（作用域深度、形参绑定、目的地表达式形态），**没有一条需要新冻结字段** —— 若实现时发现要加，先报 Lead 评估。

## 6. 唯一"安全兑现"那一格的动态实测（`persist_nodes` 的 argv 化副本）

`persist_nodes` 的 `N` 是编译期常量（`let N = 50000`）⇒ 副本里把它 argv 化（`N = args[1]`，
上限 50000；`Q = N/50`、`V = N`，形状不变），在**同一个二进制**上量 5k 与 20k：

| 档 | 5k RSS | 20k RSS | 比值 | 说明 |
|---|---|---|---|---|
| 回收档（现状 `build/extc`） | 4524 KB | 13224 KB | 2.91× | 有 rc 运行期 |
| **细分后 idle**（`NA2,NB,NE2`） | **3372 KB** | **9192 KB** | 2.73× | 无 rc 运行期 |
| 差 | **−1152 KB** | **−4032 KB** | — | idle **更省** |

**读法（重要，别误用）**：两个档都随 N 线性涨，因为**这个形状的活集本身就是 O(N)**（可持久化线段树
把每个版本的根都留在 `root[]` 里；`update` 造出来的节点全都被某些版本指着）⇒ `N vs 4N < 1.5` 这条
界**对它不适用**；可用的对照是"**同规模下 idle ≤ 回收档**"。这也意味着：日后真实现这条细分时，
`gate-idle-bounded` 要么给它写一条合适的界（活集 O(N) 的形状），要么用"idle ≤ rc + 六条对照"这条判据。
**这是判断，不是实测结论**（实测只有上面三个数）。
