# MEASUREMENTS-P1：P1 四项落地的实测与判定（chunk / 配额记账 / regionFree 批处理 / G1② 通道拆分）

状态：2026-11-09 18:51（机器负载 load 0.75 / 1.02 / 0.83，24 核，WSL2）。
冻结树（唯一口径，跑前逐条核验一致）：
| 文件 | md5 | 行数 |
|---|---|---|
| src/back/reclaim.c | e17080d1c80513b27ca06f2bc6224b0f | 4031 |
| src/back/codegen.c | d339382889b87c0dd18b357c69e2e779（v7 测量输入；提交树 md5 为 `2f560e2e6deef8c4927db4611344aecb`，仅 `typeHasRefSlots` 前向声明位置不同） | 16017 |
| tests/reclaim/driver.c | 7b9ac61a55c140640a5964ab74a8780e | 1523 |
git HEAD 85b8871b（工作树 patch，未提交）。本报告 after 一律来自自建 build-emp3/extc（md5 07b7be1a4a9accdcb8eb0c51bbe2fb7c），结果目录 tests/gc-p0/out/p1-after；before 一律取上一轮 v6 归档 tests/gc-p0/out/p0-after（build-rf/extc md5 7019342b3e5ccddbe67cbdc96eae2395）。
数字来源标注约定：未标来源者=本机实测（上述 build + OUT）；标「Lead /tmp/v2/ab7」「Lead /tmp/v2/attr」=Lead 的交错 A/B 与 Ir 归因（本报告直接引用，未重跑）；标文档路径者=引用他人文档。max 只作观察、不作判据；N/A ≠ PASS。

## §0 结论速览

1. 全树外部件全绿：tests/reclaim/run.sh 默认档 rc=0（201 ok）、EXTC_RC_NOCOUNT=0 计数档 rc=0（257 ok）、ASan/UBSan 干净；tests/reclaim-perf/gate.sh rc=0，总指令 87,135,465 Ir ≤ 88,029,115；tests/reclaim-gaps/gate-release.sh rc=0，4/4（A 清扫 1/1、B 1.00×、C rc/stdout 逐字同、D releases 19/79 slots 4,883/20,303）。
2. 项 1（chunk 大小）：保持 SCAN_CHUNK=8192 + QUOTA_DEADLINE_US=800，是**负结果**（更小 chunk 不划算）。依据见 docs/analysis/P1-RQ3-CHUNK.md §0.5：内存优先硬约束下 128/256 + deadline 800 的 pd 峰值 215–240 / 126–134 MB，而 8192 档 26.6 MB；chunk=128 还使 chase 墙钟 +6.2%。该矩阵归档在 tests/gc-p0/out/p1-chunk/，本轮未重跑。
3. 项 2（配额记账）：**配额记账不是成本**。sizeratio 2M 次分配只 174 次 markAdvance、deadline checks=0，-D 变体（MAX_MARK=0 / DEADLINE=0）Ir 差 <0.1%；真因是 B（惰性 freelist）的链重建（2,020 次 / 1.89M 洞 ≈ 19.5M Ir），回退后 sizeratio Ir 243.26M→228.39M（build-lead5 229.27M）。默认 LAZY_FREE=0（=1 时 promote 0.996×、sizeratio 1.033×，故默认关是对的）。promote 的 quota 路径仍 +2.5% Ir（129.13M→125.88M）；elide 反事实 cycleratio Ir −8.73M（−3.1%，elide 省 11.40M / roll-up 花 2.67M）。以上均引 Lead /tmp/v2/attr 与 docs/analysis/P1-QUOTA-FASTPATH.md §0。
4. 项 3（regionFree 批处理）：pd_bigobj 200 轮 [regionfree] 总时长中位 **493.2 µs**（sync 0.5–0.6 + queue 398–711；max_frees=4、max_slabs=4；n=3–5 reps）⇒ PASS（目标 ≤0.75 ms、硬界 ≤1 ms）；before 18.6–22.7 ms（docs/analysis/P1-REGIONFREE-BATCH.md §0）。
5. 项 3 未闭合两例（如实登记 RED）：pd_pointer_dense **1,365.5 µs**（sync 346.9–369.4 + queue 1,000.7–1,004.6；max_slabs=855）——单 8 MiB huge payload 的 munmap 不可切，>1 ms；pb_escape_grow 100M **8,601.1 µs**（sync 7,546.9–8,321.6 + queue 1,003.3–1,010.2；max_slabs=12,255；[freequeue] left_slabs=12,059 / 12.76 GB）——sync 段是 ownDel 的 O(#slabs)，要 region closing 队列才能闭合。
6. 项 4（G1② 通道拆分）：判定 **RED**。warm 类 (a)：cyc n=502,242、>50 µs 占比跨桶（下界 1.08e-4 ≤1e-4<上界 3.13e-4 ⇒ over50 本身 N/A），但 p99.9 落 >16 µs 桶（>10 µs 判据）⇒ RED；pb n=160,149、over50 RED（4.25e-4–1.21e-3）且 p99.9 RED ⇒ RED；pc n=20,592 <1e5 ⇒ N/A。mixed 类（pd、pd_bigobj）(a) 结构性不可判 ⇒ N/A（Lead m03564）。cold-dense（pd_pointer_dense）：tau_t=578.9 µs / 256 pops = **2.261 µs/pop > 2.0** ⇒ RED；(b) p99.9 落 >64 µs 桶（B_seg=137.1 µs，但 min(B_seg,64)=64 生效）⇒ RED；(c) regionfree max 1,819.4 µs ⇒ RED。
7. G18 **PASS**：after 60 轮 26,076 kB、200 轮（5 reps 合并）26,072 kB，ratio 1.000；live 由 [work] M_max/3 反读 = 8,192 kB，上界 3·live+2·SLAB=26,624 kB；peak200 ≤ peak60×1.05；stdout 逐字 ok、exit 全 0。v6（p0-after）已是 26,264 kB 平台，v7 再降 192 kB；pre-P0 锚点（build-lead5）的 2.163× RED（493,400→1,067,284 kB）是 P 的收益对照、不属本表 before。
8. G18 大分配通道（②）PASS：p99.9=32 µs、over1ms=0、deadline_hits=0，远低于标称闭式 145.2–145.7 µs 与物理天花板 1,086.7 µs。代价是停顿次数与总 GC 时长（对照 pre-P0 锚点 build-lead5）：200 轮单 rep step_n 428→26,116、step_total 5,018.4→144,038.6 µs（×28.7）、step_max 66.0→113.4 µs——P 用停顿换内存。相对 v6（p0-after）则 step_n 相同、step_total −13.0%、p99.9 64→32 µs，v7 在停顿侧亦为净改善。
9. G20 **RED（字面）**：promote 的 p99.9 ≤64 µs 与 >64 µs 占比 4.509e-4 ≤5e-4 两条 PASS（cum64 中位 0.999729），但 promote 墙钟 42.92→44.362 ms（+3.4%）、chase 146.47→166.778 ms（+13.9%）超「单侧 ≤+3%」⇒ 汇总 RED。chase 的 +13.9% 与本轮 Lead 交错 A/B 的 1.004× 矛盾，序列比较受机器状态影响（两批 rep 区间重叠），归属以 Lead A/B 为准，见 §3.3。
10. 既有门禁 after 基本全绿：G8 0.518×（1,576/3,040 kB）、G9 3/3、G12 999、G13 d0r=7（默认 T2 与强制 T3）、G14 800k/ctl=0.936×（判据 ≤1.02）且 ≤4 MB、G16 3/3、G4 代理 pb 20.51 B/node、pc 2.93 MB、cyc 1.000×、G4-Stress A/B/C=9,108/41,820/83,040 kB PASS；G15 14.678× XFAIL（known L10-a）、L10-b 6.67× / L10-c 14.09× 为已知限制。
11. G1 其余两条：① over1ms=0，全部 PASS；③ cyc/pc p99.9 ≤32 µs PASS，pb/pd 落 32–64 µs 桶 ⇒ N/A（桶界精度不足，不是 PASS）。
12. 标定本身存疑：本轮 c_seg 比 v6 ref 高 61–70%（pd_bigobj 7.087 vs 4.16 ns/word；dense 11.077 vs 6.885），pin 与非 pin 两批相差 9–20%，归因未定。它只影响 (b) 的**宽**界（B_seg=88.1 / 137.1 µs），不改变 64 µs 桶界兜底下的 RED 结论。

## §1 口径

### 1.1 构建与宏自检

构建命令（一次成功，二次 make warning 计数 0）：

    rm -rf build-emp3 && make BUILDDIR=build-emp3 -j24

CFLAGS 与 gc-p0 全脚本一致：-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L（后者是 [step] cpu 口径与 [pause] 墙钟的前提，缺它 glibc 不暴露 CLOCK_THREAD_CPUTIME_ID，cpuNow() 返回 -1，见 MEASUREMENTS-GC-P0.md §7-12）。
产物宏自检（build-emp3 生成物；源侧常量出处）：
| 宏 | 值 | 源 |
|---|---|---|
| EXTC_RC_FREE_BUDGET_SLABS | 8 | src/back/reclaim.c:802 |
| EXTC_RC_FREE_DEADLINE_US | 100 | src/back/reclaim.c:805 |
| EXTC_RC_FREE_QUEUE_BYTES | (8 << 20) | src/back/reclaim.c:808 |
| EXTC_RC_QUOTA_SHIFT_MARK | 3 | src/back/reclaim.c:1037 |
| EXTC_RC_QUOTA_MAX_WORDS | 8192 | src/back/reclaim.c:1040 |
| EXTC_RC_QUOTA_MAX_MARK | (1 << 20) | src/back/reclaim.c:1044 |
| EXTC_RC_QUOTA_DEADLINE_US | 800 | src/back/reclaim.c:1058 |
| EXTC_RC_RELEASE_BUDGET | 2048 | src/back/reclaim.c:1079 |
| EXTC_RC_SCAN_CHUNK | 8192 | src/back/reclaim.c:2875 |

档位自证：EXTC_RC_TIER=2、EXTC_RC_NOCOUNT=1（默认档）。插桩行源：**[regionfree]** src/back/reclaim.c:3662、**[freequeue]** :3665、**[quota] 终值** :3672、**[quota] per-round** :3404。

### 1.2 量具与串行纪律

- 单机独占：Lead 已停手全部计时；本报告所有 wall/RSS 均为串行单跑。
- ① OUT=tests/gc-p0/out/p1-after EXTC=./build-emp3/extc REPS=3 tests/gc-p0/run.sh all（wall 口径）
- ② EXTC_RC_STEP_CPU=1、同 OUT、REPS=3、run.sh g1cpu（cpu 口径，追加记录，不改日志覆盖）
- ③ 标定单独跑、不计入 after：tests/gc-p0/calib_cseg.py --reps 3 --pin 6，结果写 tests/gc-p0/bseg-cal.tsv（9 列，src_md5 / extc_md5 / run_id 齐全）；随后 G1C_C_SEG="pd_pointer_dense=11.077,pd_bigobj=7.087" run.sh g1c
- ④ G18_PD_REPS=5 run.sh g18（pd_bigobj 与 pd_pointer_dense 各 5 reps 合并，[segsteps] n 才过 MIN_N=1e5）
- ⑤ G20_REPS=5 G20_PIN=6 run.sh g20，再 run.sh g20cmp（before=p0-after/g20.json，after=p1-after/g20.json）
- ⑥ 既有门禁：run.sh all 内的 G8/G9/G12/G13/G14/G15/G16/G4 代理/G4-Stress/G17/p03/l10；外部 tests/reclaim/run.sh 两档、tests/reclaim-perf/gate.sh、tests/reclaim-gaps/gate-release.sh
- 时钟口径：[clock] step=cpu pause=wall chan=cpu；max 只观察；所有「占比」判据用 cpu 口径，[pause] 保留墙钟（MEASUREMENTS-GC-P0.md §8 intro）。
- 记录数说明（重要）：run.sh 的 g1c 分支也会执行 G1 主循环并导出 EXTC_RC_STEP_CPU=1（脚本既有行为），故 pauses.jsonl 中 cyc/pc/pb/pd 的 cpu 记录各 6 条（g1cpu 3 + g1c 3）、wall 3 条；而 g1c.py 只读 log 目录下被最后一次覆盖的 3 个 .err ⇒ G1 汇总用 6 reps（1,004,484 步 cyc 等）、G1② 通道用 3 reps（502,242 步 cyc）。两者都过 MIN_N=1e5，判定一致；但「reps 数」在两处不可直接互比。

### 1.3 判据汇总

| 项 | 判据 | 结果 |
|---|---|---|
| 项1 chunk | 更低 chunk 不换内存/性能恶化（负结果即结论） | 保持 8192 + dl800 |
| 项2 配额记账 | 记账本身可忽略（Ir 差 <1%） | PASS（<0.1%） |
| 项3 regionFree | pd_bigobj 200 ≤0.75 ms（硬 ≤1 ms） | PASS（493.2 µs） |
| 项3b | 其余形状 ≤1 ms | dense RED（1,365.5 µs）、pb RED（8,601.1 µs） |
| 项4 G1② | warm (a) >50 µs ≤1e-4 且 p99.9 ≤10 µs；(b) p99.9 ≤min(B_seg,64 µs) 且 over1ms=0；(c) regionfree/release/collect ≤1 ms | RED（cyc/pb (a)、dense 三条） |
| G18 | peak200 ≤3·live+2·SLAB 且 ≤peak60×1.05；stdout 逐字 | PASS |
| G20 | promote p99.9 ≤64 µs 且 >64 µs ≤5e-4；四形状墙钟/RSS 单侧 ≤+3% | RED（promote +3.4%、chase +13.9% 墙钟） |
| 既有门禁 | 见 §5 | 全绿，除已知 XFAIL |

## §2 四项逐项判定

### 2.1 项 1：chunk 大小保持 8192 + deadline 800（负结果）

本轮不重跑 chunk 矩阵，直接引用 docs/analysis/P1-RQ3-CHUNK.md §0.5 的决定与 tests/gc-p0/out/p1-chunk/ 归档（含 README.md）。三条理由（原文口径）：
1. 用户硬约束是内存只能降不能升：chunk 128/256 + deadline 800 的 pd 峰值 215–240 / 126–134 MB，而 8192 档 26.6 MB。
2. chunk=128 使 chase 墙钟 +6.2%（四形状里 chase 是访存受限，chunk 变细只增加触发次数）。
3. 8192 档的续扫段成本已有 P_step 闭式与 (b) 门禁兜住。
判定：**保持 8192 + deadline 800（负结果，不是缺陷）**。本轮的 chunk 相关证据只有产物宏自检（SCAN_CHUNK 8192 @src/back/reclaim.c:2875）与 [segsteps] 段步 p99.9（§2.4）。

### 2.2 项 2：配额记账（P）不是成本

| 证据 | 数值 | 来源 |
|---|---|---|
| sizeratio 2M 分配的 markAdvance 次数 | 174 次；deadline checks=0 | docs/analysis/P1-QUOTA-FASTPATH.md §0 |
| -D 变体（MAX_MARK=0 / DEADLINE=0）Ir 差 | <0.1% | 同上（Lead /tmp/v2/attr） |
| 真因：B 惰性 freelist 链重建 | 2,020 次 / 1.89M 洞 ≈19.5M Ir | 同上 |
| 回退 B 后 sizeratio Ir | 243.26M → 228.39M（build-lead5 229.27M） | 同上 |
| LAZY_FREE=1 对比 | promote 0.996×、sizeratio 1.033× | Lead /tmp/v2/attr |
| promote 的 quota 路径开销 | 关 quota 后 Ir 129.13M→125.88M，即 +2.5% | Lead /tmp/v2/attr |
| elide 反事实 cycleratio | Ir −8.73M（−3.1%）；elide 省 11.40M / roll-up 花 2.67M | docs/analysis/P1-QUOTA-FASTPATH.md §0 |

本机独立侧证（p1-after 日志）：55 个用例日志的 [quota] 终值行 checks=0 / hits=0；仅 pd_bigobj 5 reps 有 checks=244,736 / 809,536 / 826,318 / 826,560 / 836,096 与 hits=0 / 2 / 2 / 5 / 0；per-round [quota] 行 15,186 条 round_checks=0，另 1,597 条 round_checks=4,224、27 条 4,096。说明：绝大多数形状根本不进 deadline 循环，配额记账的固定开销不可见；唯一的可见成本是 B 的重建（已在源码侧回退），因此项 2 结论是**记账不是成本**，剩余 +2.5% promote 开销属于 quota 循环的边际成本，记录但不在项 2 判据内。

### 2.3 项 3：regionFree 批处理

机制（docs/analysis/P1-REGIONFREE-BATCH.md §0 口径，源码行已核）：两阶段——同步段只做 abort/pinDrop/ownDel 入线程本地队列；排空在 slabNew / settle / 退出时执行，预算 FREE_BUDGET_SLABS=8（src/back/reclaim.c:802）或 FREE_DEADLINE_US=100（:805），队列超过 FREE_QUEUE_BYTES=8 MiB（:808）切 HARD 1000 µs 顶。可观测：[regionfree]（:3662）给 n/max_frees/max_slabs/sync_us/queue_us；[freequeue]（:3665）给 forced/max_depth/left_slabs/left_bytes。

| 形状 | [regionfree] 中位（3–5 reps） | sync / queue | max_slabs | before（v6） | 判定 |
|---|---|---|---|---|---|
| pd_bigobj 200 | **493.2 µs**（max 711.4） | 0.5–0.6 / 398–711 | 4 | 18.6–22.7 ms | PASS（≤0.75 ms） |
| pd_pointer_dense 200 | **1,365.5 µs**（max 1,370.7） | 346.9–369.4 / 1,000.7–1,004.6 | 855 | 1,708.4 µs | RED（>1 ms，单 munmap 不可切） |
| pb_escape_grow 100M | **8,601.1 µs**（max 9,324.9） | 7,546.9–8,321.6 / 1,003.3–1,010.2 | 12,255；left_slabs=12,059 / 12.76 GB | 14.2–14.8 ms | RED 未闭合 |
| cyc 20M（G1 旁证） | 365.0（max 436.6） | — | 43 | — | PASS |
| pc 50M（G1 旁证） | 144.1（max 169.7） | — | 17 | — | PASS |
| pd 200（G1 旁证） | 792.4（max 881.4） | — | — | — | PASS（≤1 ms，但 >0.75 ms 目标） |

解读：pd_bigobj 从 18.6–22.7 ms 降到 0.49 ms（约 40×），队列段 398–711 µs 占主导（同步段只剩 0.5 µs），符合「huge payload 的免费操作搬到批处理」的设计。dense 与 pb 的共同瓶颈都在 sync 段及不可切的单个 munmap：dense 的 855 个 slab 里有一个 8 MiB huge payload（sync 346.9–369.4 µs，queue 1,000.7 µs 到 HARD 顶）；pb 的 24,510 个 slab 里本轮只排掉 12,255 个（left_slabs=12,059 / 12.76 GB），ownDel 仍 O(#slabs)×~560 ns ⇒ 需 region closing 队列才是闭合解（Lead 已判 RED 未闭合）。

### 2.4 项 4：G1② 通道拆分（判据与判定）

本轮标定（calib_cseg.py，pin 6，3 reps，中位；写入 tests/gc-p0/bseg-cal.tsv 9 列）：
| 形状 | p99.9 µs | spread µs | c_seg ns/word | 每 rep n |
|---|---|---|---|---|
| pd_bigobj | 58.06 | 49.54–70.81 | 7.087 | 25,023 / 25,407 / 25,023 |
| pd_pointer_dense | 90.74 | 85.09–91.83 | 11.077 | 32,512 ×3 |

（unpinned 对照：pd_bigobj 53.23 µs / 6.497 ns，dense 75.46 µs / 9.211 ns；v6 ref 为 34.08 µs / 4.16 与 56.40 µs / 6.885 ⇒ 本轮高 70% / 61%，见 §7-1。）
B_seg = 1.5·c_seg·chunk/1000 + 1 µs ⇒ pd_bigobj 88.1 µs、dense 137.1 µs；warm 形状无 seg>0 步（[segsteps] n=0）⇒ 该通道 N/A，注明「无 seg>0 步」，不是 PASS。

| 用例 | 类别 | n（步） | (a) >50 µs 占比（区间） | (b) p99.9 | (c) max | 判定 |
|---|---|---|---|---|---|---|
| cyc | warm | 502,242（cpu 3 reps） | over50 跨桶 ⇒ N/A（下界 1.08e-4 ≤1e-4<上界 3.13e-4）；p99.9 落 >16 µs 桶 ⇒ RED | N/A（无段步） | collect 52.7 / regionfree 365.0 | (a) RED（由 p99.9 判） |
| pb | warm | 160,149（cpu 3 reps） | over50 RED（4.25e-4 – 1.21e-3）；p99.9 亦 RED | N/A | collect 81.6 / regionfree 8,601.1 | (a) RED |
| pc | warm | 20,592 | n<1e5 ⇒ N/A | N/A | collect 206.1 / regionfree 144.1 | N/A（样本不足） |
| pd | mixed | 75,069 | (a) 结构性不可判 ⇒ N/A | N/A（表无该形状标定） | collect 11.2 / regionfree 792.4 | (a) N/A by design |
| pd_bigobj | mixed | 126,651 | (a) N/A | N/A（原文 why：跨桶不可判（B_seg=88.1..88.1 µs、桶界兜底 64 µs）：p99.9 落在 32..64 µs 的跨桶区间）——N/A 非 PASS | regionfree 493.2 | (b) N/A 跨桶 |
| pd_bigobj60 | mixed | 无 [top] chunk | (a) N/A | N/A | — | N/A |
| pd_pointer_dense | cold-dense | 162,560（seg 步） | tau_t=2.261 µs/pop >2.0 ⇒ RED（p99.9 PASS ≤512 µs、over1ms PASS）；>50 µs 只 OBS（8.08e-5–1.18e-3） | RED（min(B_seg 137.1, 64)=64，p99.9 落 >64 µs 桶） | regionfree 1,819.4（n=5，over1ms=5）；collect 31.2 PASS | (a)(b)(c) 三条 RED |

判据来源：Lead m04242 / m04591 / m04755（MIN_N=1e5 保持、50 µs 兜底改 b64us 桶界、p99.9 ≤ min(1.5·c_seg·chunk+1 µs, 64 µs)、max 只 OBS、混合形状 (a) N/A）。N/A 一律不计 PASS。

## §3 G18 / G20 before → after

### 3.1 G18（大对象分配与回收）

| 指标 | before = v6（p0-after，build-rf 7019342b…） | after = v7（p1-after，build-emp3 07b7be1a…） |
|---|---|---|
| 60 轮 ru_maxrss | 26,264 kB | 26,076 kB（1 rep） |
| 200 轮 ru_maxrss | 26,264 kB | 26,072 kB（5 reps 合并） |
| ratio 200/60 | 1.000× ⇒ PASS | 1.000× ⇒ PASS |
| live（[work] M_max/3 反读） | 8,192 kB | 8,192 kB |
| 上界 3·live+2·SLAB | 26,624 kB | 26,624 kB |
| 200 轮 step_n / step_total / step_max | 26,116 / 165,611.9 µs / 185.3 µs | 131,245（5 reps 合并；单 rep 26,116）/ 720,193.1 µs（单 rep 144,038.6）/ 113.4 µs |
| 200 轮 collects / deadline_hits | 198 / 1 | 995（5 reps 合并；单 rep 198）/ 0（5 reps 合计）；60 轮 0 |
| [histo] 200 轮 | 中位 8 µs、p99.9=64 µs | 中位 8 µs、p99.9=32 µs（b1us 4,026 / b2us 195 / b4us 43,634 / b8us 56,192 / b16us 26,416 / b32us 652 / b64us 103 / b128us 27） |
| [top] 前 3 | mark/advance seg=8192 pops=1（185.3 / 176.8 / 170.0 µs） | 全 kind=mark why=advance seg=8192 pops=1（113.4 / 96.9 / 91.0 µs） |
| stdout / exit | 逐字对 | 逐字 ok / [0,0,0,0,0,0] |

历史对照（pre-P0 锚点，不属本表 before）：tests/gc-p0/out/p0-before（build-lead5 9bef5488…）60 轮 493,400 kB / 200 轮 1,067,284 kB = **2.163× RED**，200 轮 step_n=428、step_total=5,018.4 µs、step_max=66.0 µs ⇒ 这才是 P 的收益对照。
判定：**G18 PASS**（内存硬判据两条 + stdout + exit），v6 已绿、v7 进一步降 192 kB。② 大分配通道 PASS（over1ms=0、p99.9=32 µs ≤ 标称闭式 145.2–145.7 µs、天花板 1,086.7 µs）；① 混合形状 N/A（结构性不可判）。代价必须写清：相对 pre-P0 是 step 次数 ×61（428→26,116）、GC 总时长 ×28.7（5,018.4→144,038.6 µs）、step_max 66.0→113.4 µs；相对 v6 则 step_n 相同（26,116），step_total −13.0%（165,611.9→144,038.6 µs）、p99.9 64→32 µs——P 用更多停顿换内存，属设计取舍，不是「停顿变好」，但 v7 相比 v6 在停顿侧也是净改善。与 Lead 预演（60 轮 26,644–26,648 kB、200 轮 26,636–26,648 kB）差 −380~−576 kB，同量级。

### 3.2 G20（P0-B 停顿收益，四形状）

| 形状 | wall ms before→after | Δ | RSS kB before→after | Δ | step_total µs（观察）before→after | step_max（观察）before→after | 判定 |
|---|---|---|---|---|---|---|---|
| promote | 42.920 → 44.362 | +3.4% | 6,304 → 6,304 | 0% | 14,692.6 → 18,437.8 | 63.8 → 89.9 | RED（墙钟越 +3%） |
| sizeratio | 24.639 → 24.546 | −0.4% | 3,376 → 3,440 | +1.9% | 1,136.5 → 2,456.3 | 32.0 → 47.3 | PASS |
| chase | 146.470 → 166.778 | +13.9% | 74,080 → 74,080 | 0% | 5,046.5 → 5,497.7 | 103.7 → 115.9 | RED（墙钟；归属见 §3.3） |
| cycleratio | 13.150 → 13.306 | +1.2% | 3,236 → 3,236 | 0% | 369.2 → 385.5 | 6.8 → 7.1 | PASS |

停顿新判据（Lead m04972，弃 max）：promote 步 p99.9 ≤64 µs 且 >64 µs 占比 ≤0.05% ⇒ **PASS**（cum64 中位 0.999729，before 1.000000；>64 µs 占比 max 4.509e-4，before 6.313e-4）。sizeratio >64 µs 占比 max 8.33e-4（该形状不在此门禁内，只 OBS）；chase cum64=0.9959（n=726，只 OBS）。四形状 stdout sha256 逐字相同。
汇总判定：**G20 RED（字面）**——promote +3.4%、chase +13.9% 超「单侧 ≤+3%」。注意本轮口径已不再使用 v6 时代的 step_total 判据（≤−10% / ≤−40%）；表中 step_total 一列只作观察，sizeratio +116.1% 与 promote +25.5% 说明 P0-B 的收益主要落在内存侧（G18），停顿侧是**代价**而非收益，不能按旧口径记「收益缩水」。

### 3.3 chase 归属冲突（重要存疑）

本报告 G20 的 chase 用「同一 OUT 的 before 归档 vs 本轮 after 序列跑」得到 +13.9%；但两批 5 reps 的 wall 分布大量重叠：before [173.387, 146.470, 144.874, 142.410, 165.482]，after [166.778, 169.745, 170.493, 165.621, 148.511]。Lead 的同轮交错 A/B（/tmp/v2/ab7，9 reps，pin6，奇偶换序，HEAD build-base 829e55a7 vs build-lead7）为 **chase 131.95→132.47 ms = 1.004×**，即代码本身没有 chase 回归。结论：+13.9% 属机器状态/序列比较误差，**归属以 Lead 交错 A/B 为准**；G20 的字面 RED 保留（判据就是这么写的），但回归归属不成立，需 Lead 裁决 chase 是否改用交错 A/B 作为归属口径。promote 的 +3.4% 与 A/B 的 +5.4% 方向一致（Lead 口径更保守），可确认为真实回归。

## §4 G1①②③ 与 G3a/G3b

### 4.1 G1①（over1ms=0）
cpu 口径，cyc/pc/pb/pd 各 6 reps 合并（g1cpu 3 + g1c 的 3，见 §1.2 记录数说明）：全部形状 over1ms=0 ⇒ **PASS**。注意 cyc 的 max=624.4 µs、pb 464.8 µs、pd 764.5 µs 都 <1 ms 但远超 p99.9，max 只作 OBS（E16 已记 cyc 曾出现 2,540 µs 单步，本轮不复现）。

### 4.2 G1②（通道拆分）
见 §2.4：**RED**（cyc/pb warm (a)；dense (a)(b)(c)）。legacy 混合分母（不分通道的旧口径）同样 RED：pb 下界 4.84e-4、pd 下界 8.62e-4。

### 4.3 G1③（p99.9）
cyc p99.9=32 µs ⇒ PASS（≤32 µs 桶界，判据 50 µs 实际由桶界兜底）；pc p99.9=8 µs ⇒ PASS；pb/pd 的 p99.9 落在 32–64 µs 桶、桶界不足以证明 ≤50 µs ⇒ **N/A（桶界精度不足，不得记 PASS）**。

### 4.4 G3a / G3b 与 collect 通道
G3a（单次 regionFree 的 free 数上限 ≤10·slabs+#classes+3）：[regionfree] 观测 max_frees=3/4、max_slabs 最大 12,255 ⇒ 显然成立 ⇒ **PASS（计数）**。
G3b（regionFree/release/collect 单次 ≤1 ms，目标 ≤0.2 ms）：regionfree 见 §2.3（pd_bigobj PASS、dense/pb RED）；collect 通道 cpu 口径 cyc 109.2 µs / pc 206.1 µs / pd 11.2 µs（G1 批）⇒ ≤1 ms **PASS**，但 cyc/pc 未达 0.2 ms 目标（记录）。release 通道本轮未触发 ⇒ N/A。说明：现有脚本没有名为 G2 的门禁行，[chan] collect 通道即其数据源，口径记 N/A(no-such-gate)，见 §7-8。

## §5 既有门禁回归全表（v6 → v7）

| 门禁 | before（p0-after） | after（p1-after） | 判定 |
|---|---|---|---|
| G8（chain2 T1/T2 峰值比 ≤1.10×） | 1,564 / 3,036 kB = 0.515× | 1,576 / 3,040 kB = 0.518× | PASS |
| G9（带 live task 的 collect） | 3/3 PASS | 3/3 PASS（live0=1 freed=0 live1=1 / tag=777 / got=20 steps=5，exit 0） | PASS |
| G12（参数覆盖别名，强制 T3） | alias check b.v=999 | 同（默认 T2 也 999，PASS-by-design） | PASS |
| G13（bulk copy 引用，默认档 + T3） | d0r=7 | d0r=7（默认 T2 + 强制 T3） | PASS |
| G14（copyInto 引用元素，800k ≤1.02×ctl 且 ≤4 MB） | 2,656/3,040/3,040/ctl 3,040 kB = 1.000× | 2,656/3,040/2,844/ctl 3,040 kB（800k 对照比 0.9356×，ratio 800k/50k=1.0708×） | PASS |
| G15（pool-pin，L10-a） | 12,672→185,988 kB = 14.677× | 12,668→185,936 kB = 14.678× | XFAIL（已知，预期） |
| G16（sweep 黑位回归，N=2M） | 3/3 PASS，wall 0.048 s | 3/3 PASS，wall 0.044 s，exit 全 0 | PASS |
| G4 代理 pb（B/node） | 20.49 | 20.51 | PASS（±10% 内） |
| G4 代理 pc（VmHWM） | 2.92 MB | 2.93 MB | PASS（≤4 MB） |
| G4 代理 cyc（20M/80M ratio） | 4,984/4,988 = 1.001× | 4,988/4,988 = 1.000× | PASS |
| G4-Stress（A/B/C） | 9,104 / 41,820 / 82,956 kB | 9,108 / 41,820 / 83,040 kB | PASS（①≤15,223 ②末 6 轮差 0.00% ③A<B<C） |
| G17（双配置峰值，观察） | A 27,812 / D 25,700 / B=C 64,100 kB | A 27,812 / D 25,700 / B=C 64,100 kB | 观察：A>D、B=C（机制未定位） |
| P0-3 形状（观察） | 22,564 kB | 22,564 kB | 观察（pre-P0 锚点 17,908 kB ⇒ +26%，已知权衡，不判） |
| L10-b（已知限制） | 2,644→17,624 kB = 6.67× | 2,644→17,624 kB = 6.67× | XFAIL |
| L10-c（已知限制） | 6,664→93,792 kB = 14.08× | 6,656→93,796 kB = 14.09× | XFAIL |
| tests/reclaim/run.sh 默认 | rc=0 | rc=0（201 ok） | PASS |
| tests/reclaim/run.sh 计数档 | rc=0 | rc=0（257 ok） | PASS |
| ASan/UBSan | 干净 | 干净（含着色缓存/体积出口/chunk=1 与 1e9 分段出口） | PASS |
| tests/reclaim-perf/gate.sh | 未归档（Lead 树 v7 为 86,852,961） | 87,135,465 ≤ 88,029,115（2.7 s） | PASS |
| tests/reclaim-gaps/gate-release.sh | 4/4 | 4/4（A 1/1、B 1.00×、C 逐字同、D 19/79 slots 4,883/20,303） | PASS |

v6→v7 有变化的项（其余逐字同）：G8 +12/+4 kB、pd_bigobj RSS 26,264→26,072（−192 kB）、dense 111,900→112,092（+192 kB）、G14 的 800k 3,040→2,844 kB（−196 kB，方向好，只观察）、pc 2,992→2,996 kB、cyc 1.001→1.000、L10-b/c 同级、gate.sh Ir 87.1M（Lead 树 86.85M，差异在 build/量具批次）。

## §6 回归与归属（promote / sizeratio / chase）

- promote：本机序列 G20 +3.4% 墙钟，Lead 交错 A/B **1.054×（+5.4%）**；Ir（callgrind，align N）115,211,310→129,126,214 = **+12.1%**。分解：quota 路径 +2.5% Ir（关 quota 后 129.13M→125.88M）、FREE_DEFER=0 无差（129.14M）、剩余的 +9% 属 B/计数结构重建（v4 时 promote 已 124.68M ⇒ v4 累计 +8.2%、v4→v7 +3.6%）。结论：**真实回归，归因到 P0-B 的重建而非配额记账**。
- sizeratio：本机 G20 序列 −0.4%（不可信，序列噪声），Lead A/B **1.050×（+5.0%）**，Ir +2.4%（22,812,974→23,355,894）。与项 2 的结论一致：主因是 B 的 freelist 重建（回退后 Ir 降回 228.39M），默认 LAZY_FREE=0 是对的选择。
- chase：Lead A/B **1.004×**（131.95→132.47 ms），RSS 1.035×（+3.5%）；本机序列 +13.9% 判为噪声（§3.3）。chase 不在 P0-B 的停顿收益形状内，只受 RSS 影响。
- off 臂（EXTC_RECLAIM=0 纯 bump 对照）：Lead A/B 1.017 / 0.972 / 0.961 / 0.999 ⇒ 回收机制本身的净成本只在 promote/sizeratio 上可见。

## §7 存疑与未能测量

1. **c_seg 标定偏高 61–70%**：本轮 pd_bigobj 7.087 / dense 11.077 ns/word vs v6 ref 4.16 / 6.885。pin 与非 pin 两批相差 9–20%（pd_bigobj 58.06 vs 53.23 µs；dense 90.74 vs 75.46），归因未定（v7 改动 / WSL2 频率与 thermal / pin 选择）。影响：B_seg 变宽（88.1 / 137.1 µs），但 (b) 仍被 64 µs 桶界兜底压住，RED 结论不变；不影响 (a)。
2. **pd_bigobj 的每 rep [segsteps] n≈25k < MIN_N=1e5**：靠 5 reps 合并（126,651）过线；单 rep 只告警。若将来缩减 reps，会静默回到 N/A。
3. **pd_bigobj60 无 [top] chunk**：60 轮日志没有可用的 [top] 行，导致 B_seg N/A；标定表里 pd_bigobj60 靠最长名匹配命中 pd_bigobj 行，本轮接受（同形状），但报告需记住这是匹配而非独立标定。
4. **项 4 的 (b) 跨桶**：pd_bigobj p99.9 落 (32,64] µs，B_seg=88.1 µs 未越、64 µs 桶界未越，但 15 桶无法给出精确分位 ⇒ 记 N/A；要精确到 50 µs 需精确分位插桩（Lead m04755 已列为后续）。
5. **chase 归属冲突**（§3.3）：G20 字面 RED 保留，但代码回归不成立（Lead A/B 1.004×）；需 Lead 裁决 chase 是否改用交错 A/B 归属。
6. **pb regionFree 未闭合**：本机 8,601.1 µs（sync 7,546.9–8,321.6 + queue 1,003–1,010），left_slabs=12,059 / 12.76 GB；Lead 复核 14,214 µs（sync 13,208.8 + queue 1,003.9、max_slabs=24,510）——同一机制不同批次，量级差来自 slab 数与机器状态。需 region closing 队列（未实现）。
7. **dense 的 munmap 不可切**：1,365.5 µs（Lead 1,708.4 µs），单 8 MiB huge payload 的 munmap 在 sync 段（346.9–369.4 µs）+ 队列到 HARD 顶；属结构性 >1 ms，登记而非本轮修。
8. **G2 无同名门禁**：现有脚本只有 G1/G3a/G3b 命名；collect 通道数据在 §4.4 给出，但判据归属未在脚本中固化 ⇒ 记 N/A(no-such-gate)，不是 PASS。
9. **pressure=1 计数口径**：summarize 报「合计 49」，而按用例 dict 合计只有 5（每用例 0/1）⇒ 计数口径（步数 vs 用例数）未统一，不作为判据。
10. **T3 unlinkChildren 未承诺、worker 退出未接线**：docs/analysis/P1-REGIONFREE-BATCH.md §0 记 worker/协程退出时全量排空需 codegen 一行（未改）；本轮 tests/reclaim 的 ASan/chunk 出口都绿，但退出路径的队列残留只在 [freequeue] 里可见（dense left_slabs=670 / 731 MB、pb 12,059），不能排除「进程退出前未排空」的语义缺口。
11. **defer=0 无法逐字节等价**：禁止 defer 时同步段行为不同（FREE_DEFER=0 只作为对照档跑绿），没有逐字节等价性证据。
12. **G17 A>D 反转**：A 默认 27,812 kB > D（显式 16 MiB）25,700 kB，机制未定位；与 v6 记载相同，非本轮引入。

## §8 文件清单与复现

本轮新增/修改（只动 tests/gc-p0/ 与本文件，未改 src、未提交）：

| 文件 | md5 | 说明 |
|---|---|---|
| tests/gc-p0/run.sh | 1187b701e3efb27b7f1b912a629c175d | G18 段加 pd_bigobj60 符号链接（修 60 轮 spawn 失败）；g1c 段透传 src-md5/标定表 |
| tests/gc-p0/summarize.py | 188f3df7cb3965548226501aff0078cb | G18 按 (case,rep) 去重；G1② 新表头 |
| tests/gc-p0/g18.py | 98c2c09c78c2cf20ccf04526ae681b96 | 多 rep 合并 + 每 rep stdout 校验 + _pick_runs 去重 |
| tests/gc-p0/g1c.py | b599d947ea627f176b323120d9873143 | 通道拆分、warm/mixed/cold-dense 三类、B_seg 标定表 |
| tests/gc-p0/calib_cseg.py | 87ca1f0b59f38f1c5a0f5a1abf9a828e | c_seg 标定（3 reps 中位、pin 6） |
| tests/gc-p0/bseg-cal.tsv | b0a48238c6ffc8f7f413889cb0ce2fca | 9 列，本轮两行已用 e17080d1 / 07b7be1a 溯源 |
| docs/analysis/MEASUREMENTS-P1.md | 本文件 | — |

产物归档：tests/gc-p0/out/p1-after/（summary.txt、g18.json、g20.json、g1c.json、timemax*.txt、pauses.jsonl、runs.jsonl、noise.txt、g4_stress.json、log/ 下 111 个 .err + 102 个 .out、csv/ 67 个）；标定单独在 out/p1-cal/；chunk 矩阵归档 out/p1-chunk/（含 README.md）。before 数字出自 out/p0-after/（build-rf 7019342b…）。
复现命令（本次实际口令）：

    # 构建（0 warning）
    rm -rf build-emp3 && make BUILDDIR=build-emp3 -j24
    md5sum build-emp3/extc   # 07b7be1a4a9accdcb8eb0c51bbe2fb7c
    # ① wall 全套 → ② cpu 口径 → ③ 标定 → ④ G18 5 reps → ⑤ G20 5 reps + compare
    EXTC=./build-emp3/extc OUT=tests/gc-p0/out/p1-after REPS=3 tests/gc-p0/run.sh all
    EXTC_RC_STEP_CPU=1 EXTC=./build-emp3/extc OUT=tests/gc-p0/out/p1-after REPS=3 tests/gc-p0/run.sh g1cpu
    python3 tests/gc-p0/calib_cseg.py --extc ./build-emp3/extc --out tests/gc-p0/out/p1-cal \
        --cal tests/gc-p0/bseg-cal.tsv --reps 3 --pin 6 \
        --src-md5 "reclaim.c=e17080d1c80513b27ca06f2bc6224b0f,codegen.c=d339382889b87c0dd18b357c69e2e779" \
        --build-md5 07b7be1a4a9accdcb8eb0c51bbe2fb7c
    G1C_C_SEG="pd_pointer_dense=11.077,pd_bigobj=7.087" EXTC=./build-emp3/extc \
        OUT=tests/gc-p0/out/p1-after REPS=3 tests/gc-p0/run.sh g1c
    G18_PD_REPS=5 EXTC=./build-emp3/extc OUT=tests/gc-p0/out/p1-after tests/gc-p0/run.sh g18
    G20_REPS=5 G20_PIN=6 EXTC=./build-emp3/extc OUT=tests/gc-p0/out/p1-after tests/gc-p0/run.sh g20
    BEFORE_G20=tests/gc-p0/out/p0-after/g20.json AFTER_G20=tests/gc-p0/out/p1-after/g20.json \
        python3 tests/gc-p0/g20.py compare   # 或 run.sh g20cmp
    # 外部门禁
    EXTC=./build-emp3/extc bash tests/reclaim/run.sh
    EXTC_RC_NOCOUNT=0 EXTC=./build-emp3/extc bash tests/reclaim/run.sh
    EXTC=./build-emp3/extc bash tests/reclaim-perf/gate.sh
    EXTC=./build-emp3/extc bash tests/reclaim-gaps/gate-release.sh

自检（全部通过）：python3 tests/gc-p0/{g1c,g18,g20,summarize,pause_parse}.py --selftest；bash -n tests/gc-p0/run.sh。
