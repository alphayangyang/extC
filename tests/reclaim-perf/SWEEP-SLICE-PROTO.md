# 清扫切片原型（`SWEEP-SLICE-PROTO`）—— 实现完成、**判据未达，如实负结果**

开关：**`EXTC_RC_SLICE_SWEEP`（编译期，默认关）**。默认档**行为不变** ✓（`md5` 已回退到 canonical
`e4c3a86960149e2d89dc22b9c1ebdef0`；原型源码留在 `/tmp/slicesweep/proto/*.on.c` 的生成物里，
可据其重建 `reclaim.c` 补丁）。**本文件记录：做了什么、量到什么、哪一条挡门。**

## 1. 原型做了什么（三件 + 两处临界段）

1. **游标 + 进行中状态**：`ExtcRcRegion` 加 `swSlab/swWord/swPass/swActive/swFreed/swTot/swChars/swRate/swCur/swP2Head`；
   `extc_rc_sweep` 在切片档变成"**开始一次清扫 + 跑首片（1 字）**"，其余由分配路径按配速推进。
   **最小可停点 = 一个字（64 槽）** ✓（与 `SWEEP-SLICE-RECON` 的结论一致）。
2. **分配按"已标记"发出**：`extc_rc_alloc` 里 `s->mark[word] |= bit`。**这一条是安全支点**（牙见 §3）。
   配套发现（原型期间修掉的真 bug）：`extc_rc_mark` 对"已标记"的对象**不再走它的孩子** ⇒
   必须**在 mark 前把全位图清零**（已加，`digraph` 每周期 1 468 字，代价可忽略）。
3. **第二趟 `bytes` 重算纳入切片**（`swPass=2`），并在完成时结算 `bytes/live/scale/thr`（中途不动）。
4. 两处临界段：`collect` 里"开始清扫"（不重入：`if (r->swActive) return 0;`）与清扫完成时的结算
   （都在同线程内，无 STW）。

## 2. 配速规矩（本件的核心设计点）与实测

**规则**：清扫开始那一刻，取"距上次清扫开始的分配次数（`swChars`）÷ 本次要扫的字数"作为 `swRate`
⇒ 每 `swRate` 次分配推进 1 个字；第一次（`swChars == 0`）取 1（宁可税重一点，也不让清扫走不完）。
**"补完"没有采用**：没有"欠账超限一次补完"的分支（那会把长停顿带回来）；欠账的表现是 `bytes` 涨、
下一次触发提前 —— 但实测这条**没走通**（见 §4）。

**税（墙钟，未插桩，5 次中位，`taskset -c 6`，跑前 load 0.10）**

| 形状 | 关档 | 开档 | 税 | 峰值 RSS 关→开 |
|---|---|---|---|---|
| `human_a` u=2000 | 22.43 ms | 33.79 ms | **1.51×** | 4.3 → **55.8 MB** ✗ |
| `human_a` u=4000 | 47.62 ms | 73.01 ms | **1.53×** | 4.5 → **111.1 MB** ✗ |
| `digraph` @1M | 482.29 ms | 514.41 ms | **1.07×** | 8.7 → **167.2 MB** ✗ |

（中间版本更差：第一版把"每次分配都调 `collect`"的**触发风暴**跑成 210 万次空 collect ⇒ 税 2.99×；
加了"清扫进行中不触发"后税降下来，但**触发变少 ⇒ 垃圾攒起来** ⇒ RSS ×10–20。）

## 3. 牙（必须有 ✓，实测红）

把 `alloc` 里那行"分配置 mark"拿掉（其余不动，切片档）：
* `digraph` @1M + ASan：`ERROR: AddressSanitizer: SEGV`，栈顶 `bench/gc/src/digraph.extc:219 in groupHasCycle`，
  **rc=1**，且 stdout 与关档**不一致** ⇒ **判据当场红** ✓；
* `human_a` u=4000：stdout 与关档**不一致** ⇒ 红 ✓。
⇒ "分配按已标记发出"这条前提**不是可选项**，且它的缺失**能被现有判据抓到**。

## 4. 判据逐条（量完按它裁）

| 判据 | 目标 | 实测 | 判 |
|---|---|---|---|
| `human_a` u=2000/4000 **实测** max 停顿改善 | ≥2× | **未测到**：原型未进稳定态（RSS 涨 20×，`bytes` 结算与触发配平不对） | **✗** |
| `digraph` @1M p99 改善 | ≥15% | **未测到**（同上） | **✗** |
| stdout 三档一致（`EXTC_RECLAIM=0` / 关档 / 开档） | 逐字节一致 | ✓（**已达成**；`ha`/`dg` 三档一致） | ✓ |
| 墙钟代价 | 报出来 | **1.07–1.53×**（见 §2） | 记录 |
| 门禁 | 八钉 + 五个 gate + `smoke`/`docs` + `MIN_IDLE` 不变 + 孤儿 0 | **已回退到 canonical**，全部绿：`smoke 16/0`、`docs 12/0`、`gate-rc-idle-corpus`/`gate-reclaim-idle`/`gate-shard-cycles`/`gate-reclaim-tax` 绿、孤儿 0、`md5 = e4c3a869…` | ✓ |
| 牙 | 注入"分配不置 mark" ⇒ 红 | ✓（ASan SEGV + 跨档不一致） | ✓ |

## 5. 挡门是哪一条、性质是什么

**挡门 = 判据 1/2（停顿改善），根因是"配速 / `bytes` 结算"这一版实现**，不是"清扫不可切"：
* 切片本身成立（`SWEEP-SLICE-RECON` 的结构量与投影：A 上 −93% 的投影、最小可停点=字、片开销 1.5 ns）；
* 但**原型没有把清扫"跑得完"**：`bytes` 只在清扫完成时结算，而清扫的推进靠分配配速 ⇒
  要么触发风暴（每分配都调 collect），要么压制触发后**清扫/触发互相等**，垃圾攒起来（RSS ×10–20）。
  途中修了两个真 bug（mark 前必须清位图；pass 2 必须覆盖清扫期间新挂的 slab），但**配平仍未收敛**。
* **性质**：**这一版口径/实现的缺陷**（可修），不是结构性否定。下一步的三条修法（按优先级）：
  1. **不要在清扫期间完全压制触发**，改成"触发只推进、不开始新清扫"（避免风暴又不让 `bytes` 停摆）；
  2. **`bytes` 增量结算**：维护"清扫期间新建的 slab"链表，完成时只补这些 slab 的占用（现在靠重扫整表兜底）；
  3. **配速改按"分配的字节数"**而不是分配次数（`bytes` 本来就是现成的压力信号，避免小对象形状配速过慢）。

## 6. 未覆盖 / 未查清

* **停顿分布（PAUSE_DIST）没测**：原型未进稳定态，测出来的分布不可比 ⇒ **未测**（不算数）。
* 未做：`EXTC_RC_SLICE_SWEEP` 的翻默认（本件不翻 ✓）；多线程；`releaseOld` 定点释放与切片清扫的交互。
* **树状态**：`src/` **已完全回退**（`reclaim.c`/`reclaim.h`/`codegen.c` 三处），`md5 = e4c3a86960149e2d89dc22b9c1ebdef0` ✓，
  `smoke`/`docs`/四个 reclaim gate 全绿、孤儿 0 ✓；原型代码在 `/tmp/slicesweep/proto/*.on.c`（生成物）+ 本文件。

---

# 第二轮（v2：四条修法 + go/no-go）—— **门槛一/二都不过 ⇒ 收工负结果**

补丁已落仓库：**`tests/reclaim-perf/slicesweep-prototype.patch`**（相对 canonical 的完整 diff，
`git apply --check` **通过** ✓；本轮结束已 `src/` 回退，`md5 = e4c3a86960149e2d89dc22b9c1ebdef0` ✓）。

## 7. v2 实现的四条修法

1. **清扫期间触发只"推进"不"开始新清扫"**：`collect` 顶部 `if (r->swActive) { swAdvance(r); return 0; }`，
   `shouldSweep` 在 `swActive` 时不响（避免"每分配一次 collect"的风暴）；
2. **`bytes` 增量结算**：pass 2 完成时只补"清扫期间新挂到头部的 slab 前缀"（不重扫全表）；
3. **按分配字节配速**：清扫开始量 `swBPW = 区域字节 / 区域字数`，推进 `(分配字节增量)/swBPW` 个字；
4. **粗粒度 tick**：每 `EXTC_RC_SLICE_TICK` 次分配判一次（编译期 env，默认 64；本轮测 **8/64/256**）。

另保留上一轮查明并修掉的真 bug：**`mark` 前必须清全位图**（`mark` 对"已标记"对象不再走孩子）。

## 8. 本轮实测（`taskset -c 6`、5 次中位、跑前 load **0.34**、跑后 0.63）

| 形状 | 关档 | tick=8 | tick=64 | tick=256 | 阈值 |
|---|---|---|---|---|---|
| `human_a` u=2000 | 22.26 ms / RSS 4 332 KB | 35.57 ms (**1.598×**) / 44 136 KB (**10.19×**) | 34.71 ms (**1.559×**) / 44 076 KB (10.17×) | 35.39 ms (1.590×) / 44 076 KB (10.17×) | 税 ≤1.3× ✗、RSS ≤1.5× ✗ |
| `human_a` u=4000 | 48.03 ms / 4 524 KB | 146.89 ms (**3.058×**) / 58 540 KB (12.94×) | 144.38 ms (**3.006×**) / 58 476 KB (12.93×) | 145.04 ms (3.020×) / 58 540 KB (12.94×) | ✗ ✗ |
| `digraph` @1M | 477.57 ms / 8 748 KB | 550.20 ms (**1.152×**) / 131 500 KB (15.03×) | 648.25 ms (**1.357×**) / 105 708 KB (12.08×) | 660.04 ms (1.382×) / 97 260 KB (11.12×) | 税 ≤1.15× ✗、RSS ≤1.5× ✗ |

**stdout 三档一致 ✓**（`EXTC_RECLAIM=0` / 关档 / 开档；`ha` 2000/4000、`dg` 100k 三档逐字节一致）；
**牙 ✓**：拿掉"分配置 mark" ⇒ `digraph` @1M + ASan **`SEGV`，栈顶 `bench/gc/src/digraph.extc:76 in mixv`，rc=1**，
stdout 与关档**不一致**。

## 9. go/no-go 裁决（先写死、按它裁）

| 门槛 | 要求 | 实测 | 判 |
|---|---|---|---|
| 一（税） | `human_a` u=2000/4000 ≤**1.3×**、`digraph` ≤**1.15×** | ha **1.56–3.06×**、dg **1.15–1.38×** | **✗** |
| 二（RSS） | ≤ 关档 **1.5×** | **×10.2–15.0** | **✗** |
| 三（停顿，仅在前两条都过时才看） | ha max ≥2×、dg p99 ≥15% | **未测**（前两条不过，按规则收工） | — |

**裁决：收工负结果。** 挡门性质（按 Lead 要求二选一）：**不是"配速可压但收益不足"，而是"粒度/配速这一版
跟不上垃圾增长"** —— 依据：
* **第④条（tick 8/64/256）实测对税几乎没有影响**（三个值同量级，dg 里 t8 甚至最好）⇒
  "每次分配都判一次"的那几纳秒**不是主因**（Lead 读数指向的第④条**被数据否掉**）；
* 真正的循环是**清扫推进速度 < 垃圾增长 ⇒ 区域滚大（RSS ×10–15）⇒ 更多 slab/更多分配/更多扫描 ⇒
  税进一步涨**（ha4 3.0× 里绝大部分来自内存滚大，不是清扫本身）。字节配速的分母 `swBPW = 区域字节/字数`
  随垃圾一起涨 ⇒ **自反馈**，这是 v2 配速设计的缺口。
* 若要续做，下一步该是"**把分母从整区字节改成'存活字节/字数'**"或"**按剩余字数与 `bytes/thr` 的比值
  自适应加速**"，而不是继续调 tick 粒度。

## 10. 重建与运行（补丁在仓库里）

```sh
# 1) 重建原型编译器（默认档不受影响；开关默认关）
cd <repo> && git apply tests/reclaim-perf/slicesweep-prototype.patch && make -j8
# 2) 开档编译一个形状（tick 8/64/256 可选）
EXTC_RECLAIM=1 EXTC_RC_SLICE_SWEEP=1 EXTC_RC_SLICE_TICK=64 \
  build/extc -O2 -I stdlib -w -o /tmp/x.c bench/gc/src/human_a.extc && cc -O2 -o /tmp/x /tmp/x.c
taskset -c 6 /tmp/x 4000        # 关档对照：同一形状不带 EXTC_RC_SLICE_SWEEP
# 3) 牙：把生成物里 `s->mark[...] |= extc_rc_bit(i);`（alloc 里那一行）删掉，再用 ASan 编译运行 ⇒ 必红
# 4) 回退：git apply -R tests/reclaim-perf/slicesweep-prototype.patch && make -j8
```

**本轮树状态**：`src/` 已回退到 canonical（`md5 = e4c3a86960149e2d89dc22b9c1ebdef0` ✓）；
`smoke 16/0`、`docs 12/0`、`gate-rc-idle-corpus`/`gate-reclaim-idle`/`gate-shard-cycles`/`gate-reclaim-tax` 全绿、
`MIN_IDLE=17` 未动、孤儿 0 ✓。
