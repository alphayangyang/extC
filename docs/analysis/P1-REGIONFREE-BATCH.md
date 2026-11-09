# P1-3 regionFree 延迟释放队列 + 有界排空（task-28）

> 冻结口径：`src/back/reclaim.c` md5 `e17080d1c80513b27ca06f2bc6224b0f`（4031 行）；
> `tests/reclaim/driver.c` md5 `7b9ac61a55c140640a5964ab74a8780e`（1523 行）；
> `build-p1/extc` md5 `b9fd57843f2c734c16756f6850ace9c5`；`docs/design/RECLAMATION.md` md5 `fcadf0eff03a7a923885b5d131c5a99a`。
> 本文所有行号以该 md5 为准；构建 `make BUILDDIR=build-p1 -j24`（零 warning）。

## 0. 结论（一屏）

1. `extc_rc_regionFree` 由「同步 free 每个 slab」改为两阶段：**同步段**只做 `abort`/`pinDropRegion`/`ownDel`（把 slab 从归属注册表摘掉）并把整条 slab 链推进 **thread-local 延迟队列**；**排空段**在后续分配点（`slabNew`）、`settle` 或线程/进程退出时按预算 `free()`。
2. 观测行 `[freequeue]`（`src/back/reclaim.c:3665`）与 `[regionfree] … sync_us= queue_us=`（`:3662`）给出队列字节/深度、累计排空数、强制排空次数、退出残留。
3. 预算：普通排空 `EXTC_RC_FREE_BUDGET_SLABS=8` 或 `EXTC_RC_FREE_DEADLINE_US=100`（先到先停）；队列字节超过 `EXTC_RC_FREE_QUEUE_BYTES=8 MiB` 时切换为硬截止 `EXTC_RC_FREE_HARD_DEADLINE_US=1000`（G3b 的 1 ms 判据）。全量排空只有一个入口 `extc_rc_freeQueueDrainAll()`。
4. 收益（pd_bigobj 200，单次进程退出 drop）：regionFree 通道 **18.6–22.7 ms → 0.7435 ms**（`sync_us=0.5`、`queue_us=742.4`）；RSS 26,456 kB（改前 26,460 kB，持平）。cycleratio 400k：通道 **134.3 µs**（sync 1.0、queue 132.7），stdout 与改前逐字节相同。
5. 同时修掉一个真实 bug：force（写屏障着色）条目在 `MARK_SCAN` 里被 **二次计 nMark**（verify 自检抓到 `nMark=3 pop=2`），会让 elide 的 O(1) 判定信任一个偏大的 nMark。修法：着色时计一次（`shadeSlow`，`:2063`），`MARK_SCAN` 只对非 force 条目计数（`:2919`）。
6. 未达标/限制（必须一起读）：**pb_escape_grow 100M 的 regionFree 仍 14.76 ms**，因同步段 `ownDel` 是 O(#slabs)（24,510 个 slab，约 560 ns/个），本设计把它留在同步段；dense 的 regionfree 通道 1,708.4 µs（单个 8 MiB huge payload 的 munmap 无法被 deadline 切开）；worker/协程退出全量排空需 codegen 一行（未改，codegen 冻结）。
7. 默认档与 `-DEXTC_RC_FREE_DEFER=0` 两档全绿：run.sh 默认 201/0、计数档 257/0、ASan 同数、gate 87,135,465 Ir ≤ 88,029,115、`ELIDE_VERIFY=1` 两档 markbegin-mismatch=0。

## 1. 机制与代码位置

| 机制 | 代码位置（`src/back/reclaim.c`，RECLAIM_RT 字符串行） | 语义 | 默认是否生效 |
|---|---|---|---|
| 开关与常量 | `:798-811` | `EXTC_RC_FREE_DEFER` 1=入队；`FREE_BUDGET_SLABS` 8；`FREE_DEADLINE_US` 100；`FREE_QUEUE_BYTES` 8 MiB；`FREE_HARD_DEADLINE_US` 1000 | 是（DEFER=1） |
| 线程状态 | `:813-818` | `_Thread_local extc_rc_fqHead/fqSlabs/fqMaxSlabs/fqBytes/fqMaxBytes`；跨线程只读累计 `fqDrained/fqForced` 用 `__atomic_fetch_add`；`rfSyncFrees/rfSyncSlabs/rfSyncMax/rfQueueMax` | 是 |
| 单 slab 字节数 | `:1364-1375` `extc_rc_fqSlabBytes` | huge block：`sizeof(ExtcRcSlab)+ss`；普通 slab 固定 `EXTC_RC_SLAB_SIZE`（1 MiB，虚拟地址空间口径）再加三张位图/可选数组/tid/ttab | 是 |
| 入队 | `:1399-1414` `extc_rc_fqPushChain` | 遍历链累加字节与个数（顺带找尾），头插 `fqHead`，更新水位；空链 `return 0`（`skip:freeq-none`） | 是 |
| 有界排空 | `:1415-1434` `extc_rc_fqDrainCore(maxSlabs, deadlineUs)` | 逐 slab：先摘链/减计数，再 `free(occ/mark/rsbit/hold/rscnt/tid/ttab/thash)`、huge 再 `free(data)`、`free(s)`；`maxSlabs<=0` 无个数限、`deadlineUs<=0` 无时间限；两处 break 带 `skip:freeq-budget`/`skip:freeq-deadline` | 是 |
| 自动排空 | `:1435-1444` `extc_rc_fqDrainAuto` | 空队列早退（`skip:freeq-empty`）；字节超限 ⇒ `forced++` + 硬截止 1000 µs；否则 8 slab/100 µs | 是 |
| 全量排空 | `:1446-1450` `extc_rc_freeQueueDrainAll` | 唯一无预算入口（线程/协程退出、driver） | 是（默认无调用者） |
| 调用点 1：分配 | `:822` `extc_rc_slabNew` 的 `EXTC_RC_TICK(4)` 之后 | 只在「要新切一块 slab」前排一点队：先偿还死 RSS 再向 OS 要内存；普通分配热路径不加成本 | 是 |
| 调用点 2：settle | `:3500` | settle 只排**一批**（普通预算），不排空 | 是 |
| drop 同步段 | `:2655-2710` | `abort`（取消在飞轮次）、`pinDropRegion`、对每个 slab（`rsIncN` 时）`unlinkChildren`、`ownDel`；`FREE_DEFER=0` 时同一位置执行原来的 9 个 free + payload + `free(s)`（`#if !EXTC_RC_FREE_DEFER`，`:2663-2665`、`:2680-2685`） | 是 |
| drop 排空段 | `:2688-2703` | `fqPushChain(r->slabs)` 后若字节超限立刻 `fqDrainCore(0,1000)`；pause 打开时记录 `sync_us`（push 完成点）与 `queue_us`（差值） | 是 |
| 观测 | `:3662` `[regionfree]`；`:3665` `[freequeue]` | 字段见 §1.1 | 是 |

### 1.1 观测行

- `[regionfree] n=… max_frees=… max_slabs=… sync_us=… queue_us=…`：`max_frees` 只数同步段的 `free()` 次数（`#if !FREE_DEFER` 分支里 `_frees += 9+big`；defer 档只剩 `cls` 链、`mstk/cstk`、`r` 等 ≤`#classes+3` 次，pd 实测 4）；`sync_us` = 第一阶段耗时，`queue_us` = 本次排空耗时；两者都只在 `EXTC_RC_PAUSE` 打开时计。
- `[freequeue] slabs_bytes=… slabs=… drained_total=… forced=… max_depth=… max_deferred_us=… left_slabs=… left_bytes=…`：当前队列（本线程最后状态）/跨线程累计排空 slab 数/队首读取的强制排空次数/深度峰值/最慢一次排空耗时/进程退出时残留（OS 回收）。
- `[sweepelide] … mis=…`：verify 模式下的 elide 自检计数（见 §3）。

## 2. 不变式与正确性论证

1. **已入队 slab 不可再被任何回收路径看到。** 前提是 `ownDel(s)` 发生在 `fqPushChain` 之前（`:2676`）：注册表按基址查 slab，`owns/ownSlab/mark/sweep/freeSlot/isLive` 都先问它；摘链后 stale 指针 `isLive` 为假、`storeRef` 不会登记它（driver 断言覆盖）。队列只用 `s->next` 串链，不碰 `s->region` 反指针，因此 `r` 可以立即 `free`（`:2703-2710`）。
2. **队列是线程局部的。** drop 与后续分配/结算在同一线程；跨线程只读 `drained/forced` 用原子加。不存在「别的线程排空本线程队列」的路径，故 `fqHead` 无需锁。
3. **huge block 的 payload 一起排队。** `:2678-2685` 把 `free(data)` 移进 `#if !FREE_DEFER`；defer 档由 `fqDrainCore` 的 `if (s->big) free(s->data)` 释放，字节数也按 `sizeof(ExtcRcSlab)+ss` 计入 `fqBytes`。
4. **预算只决定时机，不改变可达性**：入队时对象已全部死（region 被 drop，`abort` 已取消轮次），排空只释放内存；提前/延后都不影响程序语义。
5. **settle 只做有界排空**（`:3500`）；唯一全量入口是 `extc_rc_freeQueueDrainAll()`，设计上由线程/协程退出与 driver 调用。
6. **进程退出允许留队列**：at exit 时 `[freequeue]` 报 `left_slabs/left_bytes`，由 OS 回收；不承诺「退出前 RSS 归零」。
7. **defer=0 可回退**：同步段语句原样保留在 `#else`/`#if !FREE_DEFER` 分支；实测 `-DEXTC_RC_FREE_DEFER=0` 跑 run.sh 通过 184/0，恰好 = 默认档 201 条减去新增 17 条队列断言，其余断言逐条相同（断言集合等价，不是文本逐字节等价，见 §6.5）。

### 2.1 与 Lead m05183 七条要求逐条对应

| Lead 要求 | 落点 | 证据 |
|---|---|---|
| ① 两档预算 + 字节上限触发加强排空 | `:798-811`、`:1435-1444` | pd 200 `forced=1`、`[freequeue] left_*=0`；`-DEXTC_RC_FREE_QUEUE_BYTES=0` 必红 4 FAIL |
| ② 线程退出全量排空 / 进程退出报残留 | `:1446-1450`（入口）；**调用点需 codegen 一行** | 未接（codegen 冻结）；`[freequeue] left_slabs/left_bytes` 在 pb 上非 0（见 §5.3） |
| ③ huge block payload 也排队 | `:2688-2699`、`:1425` | driver `t_regionFreeDeferHuge`：drop 后队列 +1、DrainAll 后空；ASan 无泄漏 |
| ④ settle 只做有界排空 | `:3500` | run.sh 全绿；settle 路径不出现无预算调用 |
| ⑤ defer=0 等价且可回退 | `:2663-2685` 的 `#if !EXTC_RC_FREE_DEFER` | `CC=/tmp/rfwork/cc-defer0.sh bash tests/reclaim/run.sh` ⇒ 184/0（=201−17），断言集合与改前一致 |
| ⑥ driver ≥5 条断言 | `tests/reclaim/driver.c` `t_regionFreeDefer`/`t_regionFreeDeferHuge`/`t_regionFreeDeferLimit` | 共 17 条断言：队列变深/字节>0/同步 free 次数<9×slabs/stale `!isLive`/写入不复活/64 次分配后队列清空/DrainAll 后空/huge 排队/20×600 KB 超限 `fqBytes≤QUEUE_BYTES` 且 `forced` 增长 |
| ⑦ unlinkChildren 同步保留 | `:2670-2682` | 仅在 `extc_rc_rsIncN!=0`（计数 tier）执行，O(#该 region 占用槽)；G3b 的 ≤1 ms 只对默认 T2 承诺，T3 的同步段仍可能超标（见 §6.2） |

## 3. verify 自检（elide 的 nMark/nLive 不变量）

- 位置：`extc_rc_begin`（`:3756-3782`），`#if EXTC_RC_ELIDE_VERIFY`；进入新轮时遍历 `r->slabs`，断言 `s->nMark==0` 且所有 mark 字 popcount 之和为 0；不一致 `extc_rc_elideMismatch++` 并打 `[elide] markbegin-mismatch slab=%p nMark=%u pop=%llu`（每轮最多 3 条）。
- **为什么放 begin 而不是 markBegin**：`extc_rc_releaseDead`/`releaseOld` 的相位 1 也会调用 `markBegin`，那是「在已有轮次的 mark 位上再起一次收窄标记」，此时 mark 位非零是合法中间态；全零不变式只对真正的轮次起点（`begin`）成立。该位置由 driver 的 `t_shadeCache` 场景逼出来（先 begin+shade+drain，再 releaseOld）。
- **它抓到的真 bug**：`shadeSlow` 在白转灰时已经 `s->mark[wi] |= bit; s->nMark++` 并投递 force 条目，`MARK_SCAN` 处理 force 条目时又无条件 `s->mark[wi] |= bit; s->nMark++` ⇒ 同一 mark 位被计两次（verify 输出 `nMark=3 pop=2`，2 条）。后果：`nMark` 偏大，elide 的 `nMark==0` 判定会偏保守（少回收）或更糟——与 nLive 对照时把「全死」误判为「有活」。修法（`:2919`）：`MARK_SCAN` 只在 `!_force` 时置位并 `nMark++`，force 条目的位与计数交给 `shadeSlow`；两者相加恰为每个被着色对象一次。修后 verify 两档 + pd 200 + cycleratio + bench v2 四形状均 `markbegin-mismatch=0`。
- **gate Ir 归因**（`tests/reclaim-shapes/cyclic_pressure.extc` N=200000，callgrind）：A 当前树 88,303,849；B `-DFREE_DEFER=0` 88,334,528（延迟代码不是成本）；C 删两处 `fqDrainAuto()` 调用 88,312,492（调用点也不是成本）；D=C+把 `MARK_SCAN` 改回旧形（force 也计数）86,827,523 ⇒ **+1.48 M Ir 来自 force 双计修法的形状**；E=加 `if (!_force)` 两分支且 force 不计 87,137,896（采用）；F=D+删 `shadeSlow` 的 nMark++ 86,843,802（计数档可能下溢，未采用）。最终 gate 87,135,465 ≤ 88,029,115。

## 4. 验收数字（build-p1/extc 构建；命令见 §7）

| 项 | 判据 | 实测 | 结论 |
|---|---|---|---|
| 默认 run.sh | 全绿 | 通过 201 / 失败 0（ASan/UBSan 201/0、ALLOC_INLINE=0 201/0、SHADE_GUARD hits=1 bad=0、chunk=1 13/0、chunk=1e9 红自测成立） | PASS |
| 计数档 run.sh | 全绿 | 257 / 0（ASan 257/0） | PASS |
| verify 两档 | mismatch=0 | 默认 201/0、计数 257/0，markbegin-mismatch=0 | PASS |
| bench v2 四形状 verify | mismatch=0 | promote/sizeratio/chase/cycleratio 各 1 rep：exit 0、0 条 mismatch（日志 `tests/gc-p0/out/p1-verify/`） | PASS |
| gate.sh | ≤88,029,115 Ir | 87,135,465（槽号穷举 1,433,391 组不符 0） | PASS |
| skip 表 | 0 未标记/0 表缺 | ok 68 类（6 档并集） | PASS |
| 历史词 | 0 新增 | ok（存量 0） | PASS |
| doc refs | ok | 32 条注入 | PASS |
| pd_bigobj 200 | RSS 不回退、regionfree ≤1 ms | RSS 26,456 kB；`[regionfree] max_frees=4 max_slabs=4 sync_us=0.5 queue_us=742.4`；`[chan] regionfree max_us=743.5 b1ms=1`；`[step] max 158.4 µs over1ms=0` | PASS（≤1 ms；目标 0.2 ms 未达） |
| cycleratio 400k | stdout 逐字节同 + 通道 ≤1 ms | stdout md5 `e98affe9e34eccd68bc05dd5df93bf87`；`sync_us=1.0 queue_us=132.7`；`[chan] 134.3 µs`；`[sweepelide] bulk_reset=651 bulk_slots=1738046 swept=105 mis=0` | PASS |
| G18 | PASS | `G18_EXIT=0`：pd_bigobj 60 maxrss=26,072 kB / 200 maxrss=26,076 kB；dense 112,088 kB；`[chan]` 5 reps 合并 pd regionfree max=898.4 µs、collect 8.1 µs；dense regionfree max=**1,708.4 µs**（见 §6.3） | PASS（G18 判据），dense 通道观察项超 1 ms |
| pb_escape_grow 100M | Lead 期望 regionfree ≤1 ms | RSS 2,002,904 kB（改前 2,000,592）；`[regionfree] n=1 max_frees=3 max_slabs=24510 sync_us=13755.6 queue_us=1005.0`；`[freequeue] drained_total=194 forced=1 max_depth=24510 left_slabs=24316 left_bytes=25733331008`；`[chan] regionfree max_us=14762.1 b16ms=1` | **未达**（§6.1） |
| 删延迟必红 | defer 语义有牙 | `-DEXTC_RC_FREE_QUEUE_BYTES=0`（每次 drop 强制硬排空）⇒ 4 FAIL（队列未变深/字节=0/huge 未入队等），日志 /tmp/rfwork/t28-red.log | PASS |

## 5. 复现

```bash
make BUILDDIR=build-p1 -j24
bash tests/reclaim/run.sh
EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh
CC=/tmp/rfwork/cc-verify.sh bash tests/reclaim/run.sh          # -DEXTC_RC_ELIDE_VERIFY=1
CC=/tmp/rfwork/cc-defer0.sh  bash tests/reclaim/run.sh          # -DEXTC_RC_FREE_DEFER=0
CC=/tmp/rfwork/cc-qb0.sh     bash tests/reclaim/run.sh          # -DEXTC_RC_FREE_QUEUE_BYTES=0（必红）
EXTC=./build-p1/extc bash tests/reclaim-perf/gate.sh
EXTC=./build-p1/extc OUT=/tmp/rfwork/t28e/gc18 bash tests/gc-p0/run.sh g18

# 单形状（pd/cycleratio/pb）：生成后 -O2 编译，PAUSE/STEP_CPU 观测
./build-p1/extc tests/gc-p0/src/pd_bigobj.extc -o /tmp/pd.c
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L /tmp/pd.c -o /tmp/pd
EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 taskset -c 6 /tmp/pd 200
# pb 100M：tests/gc-p0/src/pb_escape_grow.extc 参数 100000000
```

## 6. 限制与存疑

1. **pb 100M 的 regionFree 同步段 13.8 ms**：defer 只推迟了 `free()`，同步段仍有「遍历 24,510 个 slab → `ownDel` → 链遍历算字节」= O(#slabs)；实测 `sync_us=13755.6`、总通道 14.76 ms。`ownDel` 不能推迟：队列 slab 若仍在注册表里，stale 指针会被 `owns()` 认成活对象并在已死槽上写；因此要 ≤1 ms 必须做「region 级 closing 队列」（同步段只摘 region 表 + abort + pinDropRegion + 把整条链挂起，`ownDel` 移进有预算的排空），代价是排队期间 `owns()` 仍认这些 slab（元数据仍活，安全但延迟可回收性），需另行论证。本任务未做。
2. **T3（计数档）同步段不承诺 ≤1 ms**：`extc_rc_rsIncN!=0` 时同步段按占用槽 `unlinkChildren`（O(占用槽×出边)），只对默认 T2 承诺 G3b。
3. **dense 的 regionfree 通道 1,708.4 µs > 1 ms**：`fqDrainCore` 的 deadline 只在两个 slab 之间检查，单个 8 MiB huge payload 的 `free()`（munmap/页表拆除，~15–300 µs）不可切分，且 drop 内先 push（同步段含 24,510 级链遍历）。pd_bigobj 无此问题（0.75 ms）。
4. **`max_deferred_us` 是「最慢一次排空耗时」**，不是队列元素驻留年龄；两者量纲不同，勿当延迟分布读。
5. **`-DEXTC_RC_FREE_DEFER=0` 的产物不可能与改动前逐字节相同**：发射文本必须包含新宏与新代码才能被 `-D` 选择；本报告的等价性证据是「同步语句原样保留在 `#else`/`#if !FREE_DEFER` 分支 + 断言集合与改前逐条一致（184/0 = 201−17）」。
6. **`left_bytes` 是地址空间口径**：普通 slab 固定按 1 MiB 计（`aligned_alloc(1MiB,1MiB)`），未触碰页不计 RSS，故 pb `left_bytes=25.7 GB` 而 RSS 2.0 GB。队列字节上限因此偏保守（更早触发 forced），不会低估。
7. **forced=1 是单次 drop 一次性触发**：`fqPushChain` 之后若 `fqBytes>8 MiB` 立即硬排空 1 ms；pb 剩余 24,316 个 slab 留在队列到进程退出（进程退出允许留队列）。
8. **worker/协程退出全量排空需 codegen 一行**：`src/back/codegen.c` 的 `extc_par_worker` 末尾（约 `:15070`）与协程 task 退出路径需各调一次 `extc_rc_freeQueueDrainAll()`；codegen 冻结，本任务只提供入口，未接线。当前全量排空只被 driver 调用。
9. **verify 只在 `-DEXTC_RC_ELIDE_VERIFY=1` 时存在**；默认档 elide 完全信任 `nMark/nLive`，若将来有人改这六处维护点（alloc/shadeSlow/freeSlot/freeSlotWord/sweepWord/releaseOld）必须开 verify 跑回归。
10. **regionFree 的 R3 守卫（`!r->part` 时逐 slab 扫 occ）仍同步**，成本 O(occ 字)，未纳入预算；但只在 `part==0` 的异常路径。

## 7. 附：改动清单（符号级）

- 新增常量/线程状态：`EXTC_RC_FREE_*`（`:798-811`）、`extc_rc_fq*`、`extc_rc_rfSync*`、`extc_rc_elideMismatch`（`:813-818`）。
- 新增函数：`extc_rc_fqSlabBytes`（`:1364`）、`extc_rc_fqPushChain`（`:1399`）、`extc_rc_fqDrainCore`（`:1415`）、`extc_rc_fqDrainAuto`（`:1435`）、`extc_rc_freeQueueDrainAll`（`:1446`）。
- `extc_rc_slabNew`：`fqDrainAuto` 调用（`:822`）。
- `extc_rc_regionFree`：同步段条件化（`:2663-2685`）、入队/强制排空/计时（`:2688-2703`）、`_frees/_slabs` 上报（`:2705-2708`）。
- `extc_rc_settle`：有界排空（`:3500`）。
- `extc_rc_begin`：verify 自检（`:3756-3782`）。
- `MARK_SCAN`（`:2917-2919`）与 `extc_rc_shadeSlow`（`:2063`）：force 条目 nMark 单计。
- 打印：`[regionfree]`/`[freequeue]`（`:3662-3665`）。
- `tests/reclaim/driver.c`：`t_regionFreeDefer`/`t_regionFreeDeferHuge`/`t_regionFreeDeferLimit` + main 调用（17 条断言，全部 `#if EXTC_RC_FREE_DEFER`）。
- `docs/design/RECLAMATION.md`：跳过表 `skip:freeq-none/empty/budget/deadline` 四行；量具段 `[regionfree]`/`[chan]` 加 `sync_us/queue_us`、新增 `[freequeue]` bullet。
