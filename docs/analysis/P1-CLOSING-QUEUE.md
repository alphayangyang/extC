# P1-CLOSING-QUEUE：region 级延迟关闭队列（G3b 闭合）

口径：v8 交付树，`src/back/reclaim.c` = `37c92f0b45180dfd65152801dfe48dfc`（4 128 行）、`src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16 017 行，未改）、`tests/reclaim/driver.c` = `a9ac2cf4e2f8f184ef9cb524232ac5d6`（1 543 行）、`build/extc` = `58402a50b8d2f05d5a11f49835c15835`。v7 行号（reclaim.c `e17080d1` / 4 031 行）与本文**不可混用**。

## 1. 要关掉的是什么

G3b：`extc_rc_regionFree` 在同步段逐 slab 做 O(#slabs) 的收尾工作。v7（两阶段）已经把 slab 的 `free()` 推进 free 队列，但 **`extc_rc_ownDel` 仍留在同步段**——因为注册表里残留的 slab 指针会被 `extc_rc_owns()` 当成活槽，stale 指针因此被误判为「仍可用」。v7 实测：

| 形状 | v7 `[chan] regionfree` | 机制 |
|---|---:|---|
| `pd_bigobj` 200 轮 | 493.2 µs（max 711.4，三源 493–743） | 已由 P1 两阶段压到达标线内 |
| `pd_pointer_dense` | 1,365.5–1,849 µs | 单个 8 MiB `munmap` 落在两次 deadline 检查之间，不可切 |
| `pb_escape_grow` 100M | 13.2–14.8 ms | 24,510 slab × ≈560 ns/slab，主项是取全局自旋锁的 `ownDel` |
| `pb_escape_grow` 50M | 6.35–8.40 ms | 同机制一半规模，交叉核对用 |

判据是「`regionFree` 单次 cpu ≤1 ms」；用户本轮给的目标更严：**单步 p99.9 ≈50 µs**。

## 2. 做法：把「注册表陈旧条目」换成「dead 挡板」

关键观察：`owns()` 只需要**语义**上回答「这个地址是不是本运行期的活槽」。v7 依赖「物理上从注册表摘除」来保证这一点，所以摘除不能推迟；但也可以用一个 O(1) 的标志位把整个 region 一次性判死。

1. `ExtcRcRegion` 增加三个字段（`reclaim.c:325-329`）：`int32_t dead`、`int32_t cqPad_`、`ExtcRcRegion *cqNext`（尾插链用）。
2. `extc_rc_owns`（`reclaim.c:693`）在取到 slab 后立刻 `if (s->region->dead) return 0;`——drop 一返回，`isLive`/按槽登记/持有判定全部立刻为假；注册表里的陈旧条目在语义上失效，但**内存仍有效**（slab 未 free、`s->region` 不悬垂），所以延迟中的读者不会拿到悬垂指针。
3. `extc_rc_regionFree`（`reclaim.c:2729-2822`）在默认档（`EXTC_RC_FREE_DEFER=1`）只做：`abort`（`:2751`，取消在飞轮次并配对减屏障参与数）→ `pinDropRegion`（`:2752`）→ `r->dead=1`（`:2757`）→ 尾插线程局部 closing queue（`:2758-2760`）→ `cqN++`（`:2761`）。R3 守卫（`part==0` 时整链扫 occ）只在 `#if !EXTC_RC_FREE_DEFER` 编译档保留（`:2740-2749`），deferred 档改由排空器逐 slab 查。
4. 排空器 `extc_rc_cqDrainCore(maxSlabs, deadlineUs)`（`reclaim.c:1771-1814`）每次从 region 链头摘**一块** slab（`:1777-1781`）：R3 守卫扫 occ（`:1782-1786`）→ `extc_rc_rsIncN` 非零时逐占用槽 `extc_rc_unlinkChildren`（`:1787-1793`）→ `extc_rc_ownDel`（`:1794`）→ `extc_rc_fqPushOne` O(1) 压进 free 队列（`:1795`）→ `cqSlabs++`（`:1796`）。region 的 slab 链空了才 free 每类桶/`mstk`/`cstk`/`region`（`:1799-1809`）并 `cqN--`（`:1810`）。预算与 free 队列同口径：`maxSlabs>0 && done>=maxSlabs` 或 `deadlineUs>0 && pauseNow()-t0>=deadlineUs*1000` 停（`:1775-1776`）。
5. 推进点复用既有开销：`extc_rc_fqDrainAuto`（`:1456`）**先跑 cq 再跑 fq**（`:1459`，空 cq 走 `skip:closing-empty`），调用点仍是 `extc_rc_slabNew` 开凿前与 `settle`；默认预算 `EXTC_RC_FREE_BUDGET_SLABS=8`、`EXTC_RC_FREE_DEADLINE_US=100`。积压上限 `EXTC_RC_CQ_MAX_REGIONS`（默认 32，`:824-825`）——超过就在 drop 内做**一次**有界排空（`:2764-2765`），保证 drop 自身也不无界。
6. 全量入口 `extc_rc_freeQueueDrainAll`（`:1470-1471`）先 `while (extc_rc_cqHead) extc_rc_cqDrainCore(0,0);` 再清 free 队列。
7. 观测：`[closing] regions_pending=… slabs_moved=… cap_regions=…`（`reclaim.c:3761-3763`），用来区分「队列为空」与「队列在积压」。

## 3. 为什么这是有界的

- **单步界**：一次 `regionFree` 的同步段 = 常数项（`abort`/`pinDropRegion`/置位/尾插）＋ 至多一次 `cqDrainCore(8 slab, 100 µs)`；一次 `fqDrainAuto` 的 cq 段同样受 8 slab/100 µs 约束。所以单步上界由预算常量给出，与 region 的 slab 数无关。
- **内存界**：closing 队列里滞留的 slab 仍是 RSS，由 `CQ_MAX_REGIONS` 的积压上限与「开凿压力即排空压力」两件事压住；drop 后若不再分配，队列不推进但也不会增长（RSS 峰值本来就是分配峰值）。G18 复核：`pd_bigobj` 60 轮 = 200 轮 = **26,076 kB**（上界 26,624），`pd_pointer_dense` 112,092 kB 与 v7 逐 kB 相同 ⇒ 队列未抬 RSS。
- **安全性方向**：跨 region 出边的撤销推迟，只会让目标暂时被保守保留（漏标方向，不是误释放方向）；payload 一律在 `unlinkChildren` 之后才 free，顺序与 v7 相同（先撤边 → 摘登记 → 压 free 队列 → 真 free）。
- **语义即时性**：`dead` 挡板保证 drop 返回后旧对象**立刻**不算活、写旧槽也**不再**复活对象——这两条正是 `tests/reclaim/driver.c` 的延迟释放断言，判据一字未放宽。

## 4. 实测（`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，cpu 口径）

| 形状 | v7 `[chan] regionfree` | v8 `[chan] regionfree` | v8 辅助观测 |
|---|---:|---:|---|
| `pd_bigobj` 200 轮 | 493.2 µs | **0.4 µs**（max 0.9） | `[regionfree] max_frees=0 max_slabs=0 sync_us≈0.2–1.9` |
| `pd_pointer_dense` | 1,365.5–1,849 µs | **1.6 µs**（max 2.5） | 8 MiB `munmap` 已移出同步段 |
| `pb_escape_grow` 100M | 13.2–14.8 ms | **1.7 µs**（max 2.1） | `[closing] regions_pending=1 slabs_moved=0` |
| cyc / pc | ≤1.4 µs | ≤1.7 µs | 同样只做常数项 |

通道级结论（`tests/gc-p0/g1c.py`，cpu 口径）：**(c) regionfree 通道 PASS**（判定线 ≤1 ms、目标 ≤0.2 ms，全部形状 max 0.4–1.7 µs）；用户目标「单步 p99.9 ≈50 µs」在桶界量具下达成——`[histo]` 15 桶的 p99.9 落在 ≤32 µs 桶（cyc/pc/pb），pd 两形状的精确分位见 `tests/gc-p0/bseg-cal.tsv`（B_seg 88.8/149.8 µs，那是 `seg>0` 的续扫通道，属 G1②(b)，与 regionFree 无关）。

## 5. 诚实限制（必须与数字一起引用）

1. **三个形状都只在进程退出前 drop 一次**，之后不再分配 ⇒ cq 没机会被开凿点排空（`slabs_moved=0`、`[freequeue] left_slabs=0`），滞留 slab 由 OS 在进程结束时回收。「同步段归零」的准确含义是：O(#slabs) 的工作从同步段搬到了有预算的排空器，**而该排空器在这三个形状里不再被触发**。
2. **队列确实会归零**，但证据来自驱动而不是形状：`tests/reclaim/driver.c` 的 `t_regionFreeDefer` / `t_regionFreeDeferHuge` / `t_regionFreeDeferLimit`（204/0）用「每个新 region 的首个分配必开凿新 slab ⇒ 触发一次有界排空」最多 64 次尝试，断言最终 `cqN==0 && fqSlabs==0`；`extc_rc_freeQueueDrainAll` 另作硬判据（一次调用必须清到 0，含 ASan/UBSan 与 `ALLOC_INLINE=0` 三档）。
3. **长跑 + 反复 drop 且继续分配的形状还没构造出来**：region 寿命 = 帧 arena 寿命，普通程序 n=1；`?mut ref` 全局零初始化被语言禁止（`cannot zero-initialize the global ... it contains a reference`），所以「反复 drop 后继续分配」的压测只能靠 driver 人工驱动。因此「队列积压 → 内存」的旁证只有 G18。
4. **预算常量仍是旋转门**：`FREE_BUDGET_SLABS=8` / `FREE_DEADLINE_US=100` / `CQ_MAX_REGIONS=32` 三个旋钮互相耦合；把它们调大能把「排空不彻底」的概率压下去，代价是单步变长。本轮没有重新标定这三者（G3b 已不是瓶颈，真正卡住 p99.9 的是 `seg>0` 的续扫通道）。

## 6. 门禁与复现

```text
make -j8                                  # build/extc = 58402a50…
tests/reclaim/run.sh                      # 204/0（+ ASan/UBSan 204/0、ALLOC_INLINE=0 204/0、chunk=1 13/0）
tests/reclaim-gaps/gate-release.sh        # 4/4
tests/reclaim-perf/gate.sh                # 87,151,664 ≤ 88,029,115（v7 87,135,465，+0.02%）
./check.sh nobench                        # 150 通过 / 2 跳过 / 0 失败
./check.sh docs                           # 13/0
EXTC=./build/extc OUT=tests/gc-p0/out/cq-all bash tests/gc-p0/run.sh all
EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 EXTC=./build/extc OUT=tests/gc-p0/out/cq-all bash tests/gc-p0/run.sh g1cpu
```

手工单形状（log 里看 `[chan] regionfree` 与 `[closing]`）：

```text
./build/extc -o /tmp/cqb/pd_bigobj tests/gc-p0/src/pd_bigobj.extc && EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 /tmp/cqb/pd_bigobj
./build/extc -o /tmp/cqb/pb tests/gc-p0/src/pb_escape_grow.extc && EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 /tmp/cqb/pb 100000000
```

## 7. 与 v7 文档的关系

- `GC-ROOFLINE.md` §6.7.3 的 v7 两阶段表保留作对照，v8 行加在同表；G3b 行改为「已闭合」，L15 改为「已修」，结论段改为 v8。
- `MEMORY-MODEL-CURRENT.md` §19.5 是这一轮的完整叙述（机制/判定/诚实限制），§19.2 表与 §19.4 的成本项同步到 v8。
- `docs/design/RECLAMATION.md` 的探针段落新增 `[closing]` 与「同步段 O(1)」契约；跳过表新增 `skip:closing-empty`、`skip:dead-region` 两行（`tools/check_skip_table.py` 70 类标记、未标记早退 0）。

## 8. 下一轮的开头

1. 构造「长跑 + 反复 drop 且继续分配」的形状（或给 driver 加持续 drop 的循环用例），把 closing 队列的**稳态积压**画出来，并把 `CQ_MAX_REGIONS`/预算三者重新标定。
2. `seg>0` 续扫通道：pd/dense 段步 p99.9 = 58.53 / 99.20 µs（`bseg-cal.tsv`）是现在唯一离 50 µs 最近的尾部；需要 markResume 快路径（E29–E33）而不是缩小 `SCAN_CHUNK`（P1-RQ3 已判负结果）。
3. G1②(a) 的 over-50 µs 占比（pb 4.25e-4–1.21e-3）按 `P1-STEP-TAIL.md` 的三类归因收口：环境抢占为主、冷页缺页次之、256-pop 真工作再次。
