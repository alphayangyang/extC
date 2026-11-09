# P1-2b：markStep / 续扫段超长步（58–871 µs 尾部）子相位归因

结论先行：**尾部不是 GC 子相位的工作量**。对 6 个形状 × 3 reps 的相位分解显示，所有步的时长都能被 `setup/resume/seed/pop/diag/tail` 解释（`unacc≈0`）。与 `[top]` 的 58–871 µs 尾部对应的时长，按其触发机制分三类：

1. **环境抢占/调度干扰**（主因）：单次停顿**集中在一个 4 KiB 子块内**（周围子块 0.1–0.7 µs），`minflt=0`，`cpu_us` 有时等于 tick 有时≈0（说明本机 `CLOCK_THREAD_CPUTIME_ID` 不能区分被抢占时间）；在同一机器/同一核上用**纯指针遍历对照程序**（无 GC）复现同类尾部（870–2345 µs，rate≈2e-5/4 KiB 块，其中 70–80% 也计入 cpu）。
2. **对象冷页首次触碰**（`chase`）：续扫段每 64 KiB 触发 `minflt=16–49`，时长 100–143 µs（≈4–9 µs/缺页）；这是真实停顿时长，但来自缺页而不是扫描逻辑。
3. **批 pop 的真实工作**（`promote`/`pd_dense` 的 `pops=256` 步）：`minflt=0`，pop+scan ≈ 100%，单 pop ≈ 0.6–1.9 µs，量级 60–230 µs。

## 0. 冻结口径与「不改 src」

| 项 | 值 |
| --- | --- |
| `src/back/reclaim.c` | md5 `e17080d1c80513b27ca06f2bc6224b0f`，4031 行（未改动，结束复核同值） |
| `src/back/codegen.c` | md5 `d339382889b87c0dd18b357c69e2e779`（未改动） |
| 生成器 | `./build-lead7/extc`（3020184 B） |
| 插桩方式 | 在**生成 C** 上用行号补丁插 `rdtsc` 探针，`atexit` 打一行 `[bd2] ...`；只用 rdtsc + 文件级静态计数器，无系统调用 |
| 探针脚本 | `/tmp/rf3_bd2/patch_bd2.py` md5 `09d0caa5737343f80335437976dad4bc`（副本已归档到本报告同目录的日志目录） |

生成 C 里被插桩的函数（行号在原生成物中，六个形状 runtime 区逐字节相同，故行号通用于全部形状）：`extc_rc_markResume`(2518)、其逐字循环(2531)、`extc_rc_markStep`(2536)、续扫分支/seed/pop/scan/诊断块(2542/2553/2604/2615–2663/2664)、`r->markWork`(2712)。子相位：`setup`=函数序言到 `_b0=budget`；`resume`=scanObj 分支（含 markResume）；`seed`=种子循环；`pop`=批 pop+扫描整段；`collect`=批弹；`scan`=批扫（`collect`/`scan` 是 `pop` 的子集）；`diag`=stepNote/topNote/时钟等诊断；`tail`=到 return；`unacc`=total−(setup+resume+seed+pop+diag+tail)。

`markResume` 内部把一段 `EXTC_RC_SCAN_CHUNK=8192` words 切成 **16 个 512-word（4 KiB）子块**分别计时，只保留最慢一段的子块剖面（`[bd2] slowseg ... subs=`），用来区分「均摊的慢」与「一次连续停顿」。成本开销：探针对 `[step] total` 的影响 +0–0.5%（promote/dense 最高 +7%，因为 dense 有 700k 步、每步多 3 次钟读）；探针自身钟读总计 8 ms/77k 次（≈100 ns/次），单列在 `diag` 里。

## 1. 表 1：每形状 max 步的相位分解（3 reps，`PAUSE=1 STEP_CPU=1`，taskset -c 6）

| 形状 | rep | 步数 | `[step]` max/µs | total/ms | max 步主导相位 | 该相位/µs | unacc/µs | 最慢续扫段 `slowseg`/µs | 其最大 4 KiB 子块占比 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pd_bigobj 200 | 1 | 26249 | 164.5 | 163.2 | resume | 163.9 | 0.0 | 163.5 | 96%（156.6 在 1 个子块） |
| pd_bigobj 200 | 2 | 26116 | 328.2 | 162.4 | resume | 326.2 | 0.0 | 325.6 | 99%（322.4） |
| pd_bigobj 200 | 3 | 26249 | 186.6 | 141.9 | resume | 185.0 | 0.0 | 184.4 | 99%（181.9） |
| pd_dense 200 | 1 | 737793 | 292.2 | 1197.6 | **diag** | 2670.3（tick）、cpu 仅 1.1 | 0.0 | 269.8 | 95% |
| pd_dense 200 | 2 | 737793 | 480.5 | 1167.8 | resume | 1782.6（cpu 50.1） | 0.0 | 1781.1 | 99%（1772.1） |
| pd_dense 200 | 3 | 737793 | 497.7 | 1165.1 | **diag** | 642.5（cpu 0.9） | 0.0 | 493.4 | 99% |
| promote | 1 | 11089 | 142.2 | 18.9 | pop | 153.6（pops=256, scan 153.5） | 0.0 | 32.6 | 95% |
| promote | 2 | 11089 | 62.0 | 19.6 | pop | 61.3（pops=256） | 0.0 | 60.6 | 94% |
| promote | 3 | 11089 | 46.8 | 19.1 | diag | 58.3（cpu 46.8） | 0.0 | 36.4 | 92% |
| sizeratio | 1–3 | 1200 | 15.2/48.3/13.1 | 2.42–2.50 | pop | 7.6/48.0/12.7（pops=8/1017/8） | 0.0 | —（无续扫段） | — |
| chase | 1 | 726 | 132.2 | 5.51 | resume | 130.4 | 0.0 | 129.4 | 54%（69.8；多子块慢） |
| chase | 2 | 726 | 114.7 | 5.41 | resume | 114.4 | 0.0 | 113.6 | 71%（80.8） |
| chase | 3 | 726 | 115.5 | 5.45 | pop | 133.0 | 0.0 | 108.4 | 9%（冷页均摊） |
| cycleratio | 1–3 | 213 | 9.0/5.9/5.5 | 0.36–0.38 | pop | 5.6/5.7/5.3 | 0.0 | — | — |

（`[step]` 的 max 与 `[bd2]` 的 max tick 可能差 1–2 µs，因为 `[step]` 只统计 `stepNote` 到诊断块内的时长；`cpu_us` 列来自运行期 `CLOCK_THREAD_CPUTIME_ID`。）

## 2. 表 2：>50 µs 步的相位归因（每形状 3 reps 汇总）

| 形状 | >50 µs 步数 | 占步数 | `resume` | `pop` | `diag` | `setup` | 触发机制（证据） |
| --- | --- | --- | --- | --- | --- | --- | --- |
| pd_bigobj | 8–18 / 26.2k | 3–7e-4 | **94%** | 0% | 4% | 2% | 环境抢占：单 4 KiB 子块 156–322 µs、邻块 0.1–0.7 µs；`PAUSE=2` 时 `minflt=0`（5 个 top 步全 0） |
| pd_dense | 107–117 / 699k | 1.6e-4 | 47% | 29% | 19% | 3% | 混合：`pops=256` 的 pop/scan 真实工作 + 单子块抢占（`minflt=0`）；rep1 `diag` 2670 µs 而 `cpu=1.1 µs` ⇒ 停顿落在“诊断段”且 Not-CPU |
| promote | 1–3 / 8.9k | 1–3e-4 | 15% | **68%** | 17% | 0% | 真实工作：top 步 `pops=256`、`minflt=0`、`dur 223 µs`（≈0.87 µs/pop）；其余为单子块抢占 |
| sizeratio | 0 | 0 | — | — | — | — | 无尾部 |
| chase | 4–9 / 557 | 7–16e-3 | **89%** | 9% | 2% | 0% | **冷页缺页**：top 步 `seg=8192`、`minflt=16/16/49/16/17`、`dur 99–143 µs`（16 页 × 4–9 µs）；最慢子剖面呈“首块 69.8 µs + 其余 2 µs + 末块 28.9 µs” |
| cycleratio | 0 | 0 | — | — | — | — | 无尾部 |

对照（无探针、未插桩的同一批生成物，`PAUSE=1`）：pd_bigobj max 424.4 µs / dense 286.4 / promote 179.7 / chase 87.0 / sizeratio 15.2 / cycleratio 5.9，`over1ms=0` 全部实现；说明尾部不是探针造成的。

## 3. 环境干扰的证据（对照实验）

`control.c`（md5 `67a18e8fe4a3e07ed099d0f8bac8c160`）：8 MiB 预热缓冲、按与续扫段完全相同的形状（16 × 512 words）遍历，无任何 GC 代码。5 reps：`over50=0/6/2/6/5`（每 400k 子块，rate ≤1.5e-5），`worst 90–512 µs`。
`control2.c`（md5 `64bff0a5350be90e1919b7d46f959ff3`）：400k 段 = 6.4M 子块，同时读 tick 与 cpu 时钟。3 reps：`over50=126/145/132`（rate 2.0–2.3e-5），`worst 870–2345 µs`，且 **stall 的 cpu 时间总和是 tick 总和的 70–80%** ⇒ 本机 `CLOCK_THREAD_CPUTIME_ID` 会把被抢占时间的大部分计入 CPU，因此 `cpu_us≈dur_us` **不能**作为“这是 GC 工作”的证据。
按子块归一化：pd_bigobj 3.6e-5、pd_dense 1.0e-5，与对照 2.0e-5 同量级；chase 6.7e-4 高出 1.5 个数量级，与其真实缺页（`minflt=16–49`）一致。

## 4. 判据回答

- **「58–871 µs 具体花在哪个子相位」**：segment 步全部花在 `resume`（`extc_rc_markResume` 的逐字循环，100%），批步花在 `pop`（含 `scan`）；`diag` 也会“中招”（dense rep1：tick 2670 µs，cpu 1.1 µs），因为抢占可以落在任何代码位置。
- **「由什么触发」**：① 环境抢占/调度（多数，`minflt=0`、单子块一次性、可被纯遍历对照复现）；② 对象冷页首次触碰（仅 chase 这类每轮新建 8 MiB 对象的形状，`minflt=16–49`）；③ 256-pop 批的真实工作（promote/dense，60–230 µs）。**没有**证据指向 slab 元数据缺页、锁或写屏障刷写（`first=0`/`trans=0`/`minflt=0` 的 top 步占比与相位分解都不支持）。
- **对 P1-2 / chunk 的含义**：把 tail 归因于 chunk 长度不成立——`seg=8192` 的尾部是环境/缺页，不是每字成本；chunk 调小只会把步数乘上去（task-26 已证），不会压低这个尾部。

## 5. 限制与下一步

1. 生成 C 插桩口径只能看到 markStep/markResume 内部；被抢占发生在任意指令，因此“不可归因到具体 GC 子相位”是**平台测量口径的上限**，不是遗漏的相位。
2. `pd_bigobj` 的 94% 尾部被判为环境干扰，但同一机器上无法完全排除 8 MiB 对象的 TLB/页表缓存效应；若要一刀切结论需在隔离核/非虚拟化硬件上复测（/sys 显示 THP=`madvise`、`AnonHugePages=0`，排除 THP 零页）。
3. 停机建议给 G1：用 `[segsteps]` 的 p99.9 + `over1ms=0`（现全部为 0），max 只作诊断；对 chase 类形状把“冷页缺页”单列为 allocation-side 成本，或引入分配后预热（不在本任务范围）。

## 6. 复现与产物 md5

```
# 生成（runtime 行号对全部形状一致）
./build-lead7/extc tests/gc-p0/src/pd_bigobj.extc -o c/pd_bigobj.c
./build-lead7/extc -w --no-line-map -I stdlib bench/gc/v2/src/<shape>.extc -o c/<shape>.c
python3 -I patch_bd2.py c/<shape>.c c/<shape>.bd2.c
cc -O2 -g -std=c11 -D_POSIX_C_SOURCE=200809L c/<shape>.bd2.c -o bin/<shape>.bd2
EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 taskset -c 6 ./bin/<shape>.bd2 <args>
cc -O2 -std=c11 control.c -o control && taskset -c 6 ./control 25000
cc -O2 -std=c11 control2.c -o control2 && taskset -c 6 ./control2 400000
```

| 文件 | md5 |
| --- | --- |
| patch_bd2.py | 09d0caa5737343f80335437976dad4bc |
| summarize.py | 5cc31448824d28f388eb6924d9832267 |
| control.c / control2.c | 67a18e8fe4a3e07ed099d0f8bac8c160 / 64bff0a5350be90e1919b7d46f959ff3 |
| c/pd_bigobj.c / c/pd_dense.c | 47184db44e1d49e722822b7c7c444000 / 45d0776a00eec08c9a7a931eb6556827 |
| c/promote.c / c/sizeratio.c | 9dc0f6320cacf5bd44abb2b397b417a6 / ddeb129a04f43bd049861d98336fcf4c |
| c/chase.c / c/cycleratio.c | b4a2d75dd8cb3bcedb5848134cfbd0a8 / 1ee598ed7586b624a959dcc3ee6bc18c |
| c/pd_bigobj.bd2.c / bin/pd_bigobj.bd2 | a74b496a506872cef912adc615aaac1b / f6a7e5c1f2e0f88914d48c81b0e66307 |
| bin/chase.bd2 | 4307da1e424304ef4e773e2ca132d1ba |

原始日志（每形状 rep1–3 + `PAUSE=2` 的 minflt 运行）：`tests/gc-p0/out/p1-step-tail/`。
