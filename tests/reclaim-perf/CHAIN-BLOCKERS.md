# 理由链画像：每个形状要放宽哪几条规则才 idle（task-34）

> **只勘察**：放宽只在 `/tmp` 副本树里做（`EXTC_PROBE_RELAX`），`src/` 零改动、`build/extc` 不变、产物逐字节不变。停顿数字只从 `bench/gc/PAUSE-CORPUS.md` 抄（同规模口径，不外推）。
>
> **⚠️ 已更正（task-35）**：本文件初版（task-34）的探针把 B 实现成「一律放行」，而**真实现** B 只能分解『字面量下标 + 具名根的字段/解引用』——带调用的基（`(*id(s))`）与非字面量下标（`(*root)[i]`）真实现**仍然判否**。把探针改成忠实语义后重测：`persist_nodes`/`cyclic_pressure`/`shard_bands`/`digraph`/`base_call_unknown` 五个形状**链尾不是 idle**（stuck），`{A,B}` 不再是可走链。下表是**忠实探针**下的更正结果；复现：`python3 tools/rc-chain-probe.py --extc <副本> --allow-stuck --report …`。
>
> **更正续（task-35 实测）**：`persist_nodes` 的忠实链是 **A → B → N → E** —— N = B 的**非字面量下标扩展**（`(*root)[i]`；实测：只放宽 A,B 时线 98 仍报 B；加 N 后它变成 `a reference field is written inside a loop`；再加 **E** 才 idle）；而 **E 正是 task-18 钉住的不可走规则**（放开它 `outer_alloc_inner_write` 立刻 idle、`gate-idle-bounded` 红）。⇒ **`persist_nodes` 不是 A,B 能拿下的**：它要 A + B + B-with-非字面量下标 + **E 的细分**（`(*root)[i]` 写的是**不同元素槽**，与`outer_alloc_inner_write` 的『同一个槽被每轮改写』不是一回事）。
>
> 链是被**实测**走出来的（逐条放宽、重跑诊断）；改动量是**估计**。副本编译器：`/tmp/chain/build/extc`。

## 1. 每形状的理由链（有序；`→` 按第一次出现的顺序）

| 形状 | max µs | 占比 | **完整理由链** | 走到 idle 需放宽 | 链尾 idle? | 绊线体检 |
|---|---|---|---|---|---|---|
| `tests/reclaim-shapes/persist_nodes.extc` | 2343.7 | 10.54% | A→B | A,B | **否（stuck）** | **链尾不是 idle**（放宽完全部规则后仍被真实现挡住：B） |
| `tests/reclaim-shapes/cyclic_pressure.extc` | 505.4 | 26.14% | C→B→D | C,B,D | **否（stuck）** | **链尾不是 idle**（放宽完全部规则后仍被真实现挡住：B） |
| `tests/reclaim-shapes/live_churn.extc` | 411.1 | 20.75% | D | D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-shapes/cross_region.extc` | 354.0 | 17.03% | D | D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-shapes/cross_region_elem.extc` | 350.3 | 56.21% | B→A→D | B,A,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-shapes/owed_home.extc` | 280.8 | 47.11% | E→D | E,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc, outer_alloc_inner_write.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/slot_elem_known.extc` | 269.9 | 29.76% | B→A→D | B,A,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-shapes/shard_bands.extc` | 198.5 | 16.00% | C→B→D | C,B,D | **否（stuck）** | **链尾不是 idle**（放宽完全部规则后仍被真实现挡住：B） |
| `tests/reclaim-gaps/shapes/slot_elem_unknown.extc` | 177.0 | 3.60% | B→D | B,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/carray_slot.extc` | 173.7 | 8.49% | D | D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/struct_copy_slot.extc` | 157.8 | 20.73% | F→A→D | F,A,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/dyn_payload.extc` | 144.4 | 1.93% | G→F | G,F | 是 | 安全（三条仍发 rc） |
| `tests/reclaim-shapes/struct_copy.extc` | 142.9 | 20.45% | F→A→D | F,A,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `bench/gc/src/digraph.extc` | 139.0 | 14.64% | D→C→E→B | D,C,E,B | **否（stuck）** | **链尾不是 idle**（放宽完全部规则后仍被真实现挡住：B） |
| `tests/reclaim-perf/shapes/outer_alloc_inner_write.extc` | 134.1 | 14.19% | E | E | 是 | **不可走**（outer_alloc_inner_write.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/owcell_garbage.extc` | 127.2 | 5.13% | H→D→C | H,C,D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-shapes/scale_growth.extc` | 125.8 | 6.44% | D | D | 是 | **不可走**（cross_region.extc, scale_growth.extc, live_churn.extc 被判 idle） |
| `tests/reclaim-gaps/shapes/byval_temp.extc` | 125.1 | 5.97% | F | F | 是 | 安全（三条仍发 rc） |
| `tests/reclaim-gaps/shapes/struct_copy_ret_home.extc` | 125.0 | 5.99% | F | F | 是 | 安全（三条仍发 rc） |
| `tests/reclaim-gaps/shapes/base_call_unknown.extc` | 91.0 | 2.85% | B | B | **否（stuck）** | **链尾不是 idle**（放宽完全部规则后仍被真实现挡住：B） |

## 2. 按「需放宽的规则集」聚合

| 规则集 | 形状数 | Σmax µs | 占命中 Σmax | 估计改动量（和） | 绊线体检 |
|---|---|---|---|---|---|
| **A,B** | 1 | 2343.7 | 36.8% | 6 | 安全 |
| **D** | 4 | 1064.6 | 16.7% | 5 | **不可走** |
| **C,B,D** | 2 | 703.9 | 11.0% | 11 | 安全 |
| **B,A,D** | 2 | 620.2 | 9.7% | 11 | **不可走** |
| **F,A,D** | 2 | 300.7 | 4.7% | 12 | **不可走** |
| **E,D** | 1 | 280.8 | 4.4% | 10 | **不可走** |
| **F** | 2 | 250.1 | 3.9% | 3 | 安全 |
| **B,D** | 1 | 177.0 | 2.8% | 7 | **不可走** |
| **G,F** | 1 | 144.4 | 2.3% | 8 | 安全 |
| **D,C,E,B** | 1 | 139.0 | 2.2% | 16 | 安全 |
| **E** | 1 | 134.1 | 2.1% | 5 | **不可走** |
| **H,C,D** | 1 | 127.2 | 2.0% | 14 | **不可走** |
| **B** | 1 | 91.0 | 1.4% | 2 | 安全 |

### 2.1 只保留**绊线安全**的集合（可走的链）

| 规则集 | 形状数 | Σmax µs | 占命中 | 估算改动量 | 备注 |
|---|---|---|---|---|---|
| **F** | 2 | 250.1 | 3.9% | 3 | byval_temp.extc，struct_copy_ret_home.extc… |
| **G,F** | 1 | 144.4 | 2.3% | 8 | dyn_payload.extc… |

## 3. 规则收窄：哪些其实是链中一环、哪些是瓶颈

- **出现在任何链里的规则**：A, B, C, D, E, F, G, H
- **真正被需要（走到 idle 必须放宽）的规则**：A, B, C, D, E, F, G, H
- **走到 idle 需要、但只在「绊线不可走」的链里出现的规则**（⇒ 不是瓶颈）：H
- **在绊线安全的链里被需要的规则 = 真正的瓶颈集**：A, B, C, D, E, F, G（其中 D, E, G 是死结 ⇒ 那条链实际走不通）
- **只作为链中一环、从未被需要**：（无）

## 4. 一句话结论

**下一件实现「F」这条链**：消掉 **250.1 µs**（占命中 Σmax 3.9%），估计改动量 3。

> 口径：命中集合 Σmax = 6376.7 µs；全部发 rc 形状 Σmax = 6382.9 µs；命中 20 个形状。停顿来源 `bench/gc/PAUSE-CORPUS.md`（N=100000、3 次取中位）。

## 5. 自检

- `python3 tools/rc-chain-probe.py --selftest`：认识/不认识/空原文三类，不认识的必须报错（沿用 `require_blockers` 的思路）。
- 每个形状都必须走到「产物 rc=0」或报出**链断**；`walk()` 对「还有 rc 却一条理由都没有」与「放宽后没有新规则可放」都直接中止。

## 6. 未覆盖与残留风险

- 副本的放宽是**整条规则**的开关（不是实现）：它答的是「若这条规则不存在，还差哪几条」，**不答**「这条规则能不能被安全地细分」。A/D 的细分方案要另立。
- 绊线体检只看 `cross_region`/`scale_growth`/`live_churn` 三条；其它形状的语义仍由各自电池看着（本件不改判决，风险为零）。
- 停顿是同一规模口径（argv 形状 N=100000），不做跨规模外推。
