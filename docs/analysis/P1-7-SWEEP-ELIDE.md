# P1-7 全死 slab 批量重置（sweep elision）——实现、证据与限制

> 冻结口径：src/back/reclaim.c md5 9af9ca9b8f6ad939a577f99404ddf319（3667 行）、build-rf/extc md5 71e9be4fe5bafb66d9d4c58c74591307。
> 行号均为当前物理行；RECLAIM_RT[] 是逐行字符串，语义以符号名为准。

## 0 摘要

`extc_rc_sweepElide` 在一个 sweep 步的开头（slab 边界处）用一次 O(nwords) 位图判定，把「整块无任何标记槽」的 slab 批量重置：两张位图 memset、记账一次扣减、bump/freelist 复位、slab 留在 class 链上。它不改变任何回收决策（被重置的槽在逐槽路径上本来也全部是 dead），只把每槽约 11 Ir 的 `freeSlotWord` 换成每 slab 一次判定。

- **收益**：cycleratio（churn 形状，98% 死槽集中在全死 slab）Ir 274,061,478 → 264,298,098（−9,763,380，−3.56%）；promote +0.06%、sizeratio +0.25%（噪声级，见 §6）。
- **不改决策**：判据只覆盖「所有 word 上 occ & mark 都为空」的 slab；这些槽在逐槽路径上同样会被 `freeSlotWord` 全部释放。
- **只默认档生效**：计数档（`EXTC_RC_NOCOUNT=0`）下 helper 是返回 0 的 stub（`skip:elide-counting`），因为 hold/rscnt 必须逐槽维护。
- **不参与**：huge block（`big`）、region 曾有跨区引用（`foreign`）、`rsIncN != 0` 的进程、任何 slab 有 rsbit 置位。
- **停顿**：判定上界是 nwords 次位图读（16 B 槽在 1 MiB chunk 下 ≤1024 word，约 2–3 µs），实测 cycleratio 单步 max 5.2–9.9 µs、step 数 1221→255；pd 的 >50 µs 尾部来自 SCAN_CHUNK 的 mark 分段扫描，on/off 两侧都存在（§7）。

## 1 背景与判据

Lead 在冻结 v4 生成 C 上用逐 slab 探针（sweepBegin 记 popcount(occ)，sweepWord 整块完成时重算）得到三形状面积数据：

| 形状 | slabs | full_dead | full_dead_slots | full_live | dead_slots | live_slots | words |
|---|---|---|---|---|---|---|---|
| promote 200k | 879 | 0 | 0 | 74 | 1,748,760 | 481,901 | 36,117 |
| sizeratio 2M | 2394 | 3 | 2,171 | 171 | 1,995,915 | 195,544 | 44,289 |
| cycleratio | 756 | 651 | 1,739,978 | 63 | 1,778,862 | 54,651 | 30,870 |

判据由此确定：

1. 「整块全活则跳过」没有价值——word 循环总量只有 3.6–4.4 万 word（≈0.2M Ir），full_live 只占访问的 8%–29%。
2. promote 的 sweep 成本几乎全部来自 175 万死槽逐个 `freeSlotWord`（≈11 Ir/槽），且一个 slab 都不全死（长/短命对象在 slab 内交错）。
3. cycleratio 的死槽 98% 落在全死 slab（651/756 块访问、1.74M/1.78M 槽）⇒ 批量重置是唯一有面积的方向。

## 2 实现

| 机制 | 代码位置 | 常量/数值 | 语义 | 默认是否生效 | 对停顿的影响 | 对内存上界的影响 |
|---|---|---|---|---|---|---|
| 计数器 | src/back/reclaim.c:3019 | `extc_rc_seBulkReset / seBulkSlots / seSwept` | 重置块数 / 释放槽数 / 逐字完成块数 | 是 | 无 | 无 |
| 批量重置 | src/back/reclaim.c:3021-3058 | 判定 O(nwords) | 全死 slab 一次清零 | `#if EXTC_RC_NOCOUNT` | 每块 1 个预算单位 | 释放的槽立即回 bump 路径 ⇒ 峰值不升 |
| 计数档 stub | src/back/reclaim.c:3060-3064 | 返回 0 | 计数档走逐槽路径 | 计数档 | 无变化 | 无变化 |
| 整块完成计数 | src/back/reclaim.c:3084 | `extc_rc_seSwept++` | 逐字扫完整块 | 是 | 无 | 无 |
| 调用点 | src/back/reclaim.c:3100 | 循环内 `r->swWord == 0` 时 | 只在 slab 边界尝试 | 是 | 每块 1 单位 | 无 |
| 诊断输出 | src/back/reclaim.c:3325-3327 | `[sweepelide] bulk_reset=… bulk_slots=… swept=…` | PAUSE=1 退出时打印 | 仅 PAUSE=1 | 无（退出时一次） | 无 |

判定顺序与早退标记（每个早退同行带 skip 标记，docs/design/RECLAMATION.md 跳过表有对应行）：

| 标记 | 条件 | 方向 | 原因 |
|---|---|---|---|
| `skip:elide-no-slab` | `!r->swSlab` | 无操作 | 清扫已到链尾 |
| `skip:elide-big` | `s->big` | 走逐槽 | payload 在 slab 外、单槽，`extc_rc_freeSlot` 是唯一正确路径 |
| `skip:elide-foreign` | `r->foreign` | 走逐槽 | region 曾有跨区引用，rsbit 可能非 0；`foreign` 单调置位，一旦置位该 region 永久放弃本优化 |
| `skip:elide-reachable` | 任一 word `occ & mark` 非 0 | 走逐槽 | 块内有存活/钉住对象 |
| `skip:elide-xregion` | 任一 word `rsbit` 非 0 | 走逐槽 | 块内有活跨区入边 |
| `skip:elide-counting` | 计数档 | 走逐槽 | hold/rscnt 必须逐槽递减与清零 |

全死时的动作（src/back/reclaim.c:3044-3058）：

```c
r->bytes -= (int64_t)live * (int64_t)s->ss;
r->freedSlots += (int64_t)live;
r->swFreed += (int64_t)live;
s->bump = 0;
s->freelist = -1;
memset(s->occ, 0, (size_t)s->nwords * sizeof(uint64_t));
memset(s->mark, 0, (size_t)s->nwords * sizeof(uint64_t));
if (!s->avail) extc_rc_classPush(r, s);
s->swEpoch = r->swId;
r->swSlab = s->next;
r->swWord = 0;
```

`live` 在判定循环里与 popcount(occ) 累加得到，不需要第二遍。`classPush` 保持与 `freeSlotWord` 相同的条件（`!s->avail`）——整块满时不在 class 链上，不 push 就再也拿不到这块，峰值内存会上升。

## 3 正确性论证

**(a) 回收决策不变。** 判据成立意味着对每个 word：`occ[w] & mark[w] == 0`。逐槽路径 `sweepWord` 计算 `dead = occ[w] & ~mark[w] = occ[w]`，对每个占用位调用 `freeSlotWord`，把该槽放回 freelist：occ 清 0、mark 清 0、`bytes -= n*ss`、`freedSlots += n`。批量重置做的是同一件事的整块版本（occ/mark 全清、bytes/freedSlots 同额扣减），额外把 bump/freelist 复位为空块。被重置的槽没有一个在逐槽路径上会被保留。

**(b) 「sweep 结束全 region mark 位为 0」不变式保持。** 重置显式 memset mark 位图，并置 `s->swEpoch = r->swId`、推进 cursor；整块逐字路径仍在 `s->mark[w] = 0` 后完成（:3081-3084）。下一轮 mark 的 dedupe（只看 mark 位）不会把可达对象当已扫描跳过。

**(c) pin / hold。** 每个 pinned 值在轮初由 `extc_rc_pinSeed` 压栈并标黑，因此 `occ & mark == 0` 的块不可能含 pinned 槽；计数档 hold>0 同理。`-DEXTC_RC_SHADE_GUARD=1` 下 helper 内自带自检（遍历 pin 表，若 pin 命中本块则 `extc_rc_sgBad++`），本仓库测试以 bad=0 通过。

**(d) rsbit / 跨区边。** `rsIncN == 0`（默认）时没有任何代码路径登记跨区入边；`foreign` 一旦置位该 region 立即永久退出本优化。因此参与重置的 slab 的 rsbit 必为 0（判定里仍逐 word 复查作为第二道闸）。

**(e) alloc-during-sweep 交叉（两项断言，tests/reclaim/driver.c:1141 起 t_sweepElideMark）。**
- *游标所在的旧 slab、游标之后的 word 里新分配对象*：alloc 的标黑规则 `s->swEpoch != r->swId && (s != r->swSlab || wi >= r->swWord)` 命中，新槽 mark 位置 1 ⇒ 该 word `occ&mark != 0` ⇒ elide 返回 0，绝不判全死。用例断言 fresh 落在 `r->swSlab`、`slotOf(fresh)>>6 >= r->swWord`、`slotBlack(fresh)` 为真，并断言 `extc_rc_sweepElide(r) == 0`。
- *sweep 进行中新建的 slab*：`slabNew` 在该 slab 上置 `s->swEpoch = r->swId`（src/back/reclaim.c:850 起），于是 alloc 不为它标黑；但它被头插到 `r->slabs` 的首部，而本轮 cursor 只沿 `s->next` 前进 ⇒ 本轮永远不会访问它，判定不覆盖它。下一轮开始前必经一次完整 mark 轮（`sweepBegin` 唯一调用点在 `finish`，且 `finish` 只在 mark 完成后执行），可达对象届时会被标黑。用例断言新 slab `ns->swEpoch == r->swId` 且 `!slotBlack(news)`，并在清扫结束后断言 fresh/news/keep 都仍活。

**(f) 只默认档生效。** 计数档 helper 是 stub，逐槽路径原样保留；默认档里 `hold`/`rscnt` 连内存都不分配（`#if !EXTC_RC_NOCOUNT` 折叠），因此整块重置无需触碰它们。

## 4 复现 Lead 探针（[sweepelide] 计数）

EXTC_RC_PAUSE=1，after 二进制；三形状各跑一次：

| 形状 | bulk_reset | bulk_slots | swept |
|---|---|---|---|
| promote 500k | 0 | 0 | 2,601 |
| sizeratio 2M | 3 | 2,171 | 2,391 |
| cycleratio 400k | 651 | 1,739,978 | 105 |

与 Lead 探针的 full_dead / full_dead_slots 逐位一致（promote 0/0、sizeratio 3/2,171、cycleratio 651/1,739,978）。

## 5 收益（Ir）

口径：bench/gc/v2 的 bench_args（promote 500000 20261013、sizeratio 2000000 20261013、cycleratio 400000 20261013 100）；`$EXTC -w --no-line-map -I stdlib <src> -o x.c` + `cc -O2 -D_POSIX_C_SOURCE=200809L`；Ir 取 `valgrind --tool=callgrind` 的 summary 行。

| 形状 | before（elide 关） | after（elide 开） | 差值 |
|---|---|---|---|
| promote | 783,472,742 | 783,943,671 | +471k（+0.06%） |
| sizeratio | 228,672,466 | 229,234,757 | +562k（+0.25%） |
| cycleratio | 274,116,151 | 264,298,098 | **−9.76M（−3.56%）** |

before 编译器：/tmp/rt-be/build-be/extc（md5 687157182cdb83dc4c85beb778567e，构建方式 = HEAD worktree + 当前 src + 把调用点改成 `if (0 && extc_rc_sweepElide(r))` 的 reclaim.c）。after：build-rf/extc（md5 71e9be4f…）。

墙钟预期：cycleratio 的 sweep 只占墙钟约 1.25/13.1 ms，其余是分配/标记/应用逻辑；−3.56% Ir 对应的墙钟上限约 −1.5%~−2.5%，不应期待 −9%。实测墙钟分辨率不足（taskset -c 6 三次 reps 均 0.01–0.02 s），以 Ir 为准。

## 6 promote 为什么不可能赢（负结果留档）

promote 的 175 万死槽均匀散布在 879 个 slab 里，slab 内长/短命对象交错 ⇒ 全死 slab 数 0。批量重置的判定对每块都要 O(nwords) 读，却永远在第一个有标记槽的 word 上早退，因此只付判定成本（+0.06%）。sizeratio 同理（2394 块里只有 3 块全死，2,171 槽）。这是「回收形态决定优化上限」的例子：只有 churn（cycleratio）才产生整块同批死亡。

## 7 停顿证据

口径：`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 taskset -c 6`，各 3 reps；[step] 与 [histo] 的时长是 CPU 时钟（`[clock] step=cpu`）。

**cycleratio**

| 版本 | [step] n | max_us（3 reps） | over16 | [histo] 主要桶 |
|---|---|---|---|---|
| elide on | 255 | 5.2 / 5.3 / 9.9 | 0 | b1us 113–125、b2us 88–98、b8us 40–41 |
| elide off | 1221 | 5.6 / 37.7 / 21.6 | 0 / 1 / 1 | b1us 303–312、b2us 851–868、b8us 41、偶发 b64us |

elide 把步数从 1221 压到 255（每块一次判定替代 32 word 的逐槽释放），单步 max 从偶发 37.7 µs 降到 ≤9.9 µs。

**pd（tests/gc-p0/src/pd_bigobj.extc 参数 200，即 200×8 MiB）**

| 版本 | [step] n | max_us（3 reps） | >50 µs reps | [top] 首名 |
|---|---|---|---|---|
| elide on | 428 | 30.6 / 78.6 / 38.4 | 1/3 | `kind=mark why=advance dur_us=78.6 cpu_us=78.6 seg=8192 pops=1` |
| elide off | 432 | 165.9 / 70.8 / 36.0 | 2/3 | `kind=mark why=advance`（同 seg=8192 形状） |

结论：

1. **elide 判定自身不引入新的 >50 µs 通道。** 判定是 nwords 次位图读，最坏 1024 word（16 B 槽 × 1 MiB chunk ⇒ 65,536 槽 / 64）约 2–3 µs；实测 elide 主导的 cycleratio 全 reps max ≤9.9 µs。
2. **pd 的尾部是 mark 分段扫描**：`EXTC_RC_SCAN_CHUNK=8192` × C_scan≈3.4 ns/word ≈28 µs，叠加噪声后偶发 78.6/165.9 µs；两侧都存在，且 elide off 的尾部更高。故不把 pd 的 >50 µs 记在本优化账上；该通道由 SCAN_CHUNK 决定（P0-1 已定的步界模型 P_step ≤ C_scan·CHUNK + 固定开销）。
3. **P1-7b（把判定按 word 分段）暂无必要**：判定上界 2–3 µs ≪ 50 µs；登记为后备手段，仅当未来 slab 尺寸或槽粒度变化使 nwords 增大一个数量级以上时再启用。

## 8 限制与后继

| 限制 | 后果 | 后继 |
|---|---|---|
| `r->foreign` 单调置位 | 一旦 region 有过跨区引用，永久放弃本优化（只损失性能，不影响正确性） | 若要把优化扩到多 region 程序，需要可撤销的 foreign 记账（P1-8 范畴） |
| `rsIncN != 0` 不参与 | 事件档/计数档的跨区边必须逐槽撤销 | 与 hold 级联同批处理 |
| `big` slab 不参与 | huge block 仍逐槽 freeSlot | 单槽块批量收益为 O(1)，无需优化 |
| 判定在最坏情况仍 O(nwords) | 一块 1024 word 约 2–3 µs，且只在全死块上付满 | 若要再压，P1-7b 按 word 分期 |
| 「全活跳过」不做 | 已由 §1 面积数据否决（full_live 仅 8–29% 访问） | 不再考虑 |

## 9 不可做（理由）

**事件档 shade/shadeOld no-op：不做。** 事件档（`EXTC_RC_SWEEP_FREE=1`）下 `collect()`/`markBegin()`/`markStep()` 仍是对外运行期入口，tests/reclaim 的 driver 直接在事件档调用 collect；shade 是增量标记的正确性义务（写屏障保证新值在标记期间被标黑），把它编译成 no-op 会让这些入口在事件档产生漏标 ⇒ 不允许。compile-facts 测到的 −3.39%（promote 事件档）不能作为采纳理由。

## 10 门禁、测试与冻结

| 项 | 结果 |
|---|---|
| `make BUILDDIR=build-rf -j24` | exit 0，`-Wall -Wextra -Wpedantic` 零警告 |
| 默认档 `bash tests/reclaim/run.sh` | **169/0**（含新用例 t_sweepBulkReset、t_sweepElideMark 共 8 断言） |
| ASan/UBSan | 169/0 |
| `EXTC_RC_SHADE_GUARD=1` | hits=1 bad=0 |
| `-DEXTC_RC_ALLOC_INLINE=0` | 169/0 |
| 分段扫描出口 | chunk=1 → 10/0；chunk=1e9 红自测成立 |
| 计数档 `EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh` | **218/0**（含 ASan 218/0、ALLOC_INLINE=0 218/0） |
| 默认档 gate.sh | 86,354,215 Ir ≤ 88,029,115 |
| `python3 tools/check_skip_table.py` | ok：62 类标记（6 档并集）、未标记早退 0 |
| 生成物（默认/计数/事件三档） | `-Wall -Wextra -Werror=format` 零警告 |
| 删除 elide 调用 | run.sh 必红 3 FAIL（判据有牙） |

冻结 md5：src/back/reclaim.c `9af9ca9b8f6ad939a577f99404ddf319`（3667 行）、tests/reclaim/driver.c `c4ccf26a4d202fd92e82e0a7663a9b2c`、docs/design/RECLAMATION.md `64c5aa2bceb8514b350b44f684c74e7e`、build-rf/extc `71e9be4fe5bafb66d9d4c58c74591307`。

**idle 指纹基线需 refreeze**：新符号 `extc_rc_sweepElide`/`extc_rc_seBulkReset`/`extc_rc_seBulkSlots`/`extc_rc_seSwept` 与 `[sweepelide]` 输出改变非 idle 程序的 rc_syms 列；命令见 tests/reclaim-idle/fingerprint.py freeze。
