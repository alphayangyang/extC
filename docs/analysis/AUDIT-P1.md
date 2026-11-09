# AUDIT-P1：P1 四项改动、task-32 尾部归因、以及 §7/§19 文档口径的独立复核

> 审计者：doc-audit（teammate）。输入任务：Lead m03884（阶段 B GO）+ task-32 复核。
> 口径：只报不改（源码/tests/其他 docs 均未改动，唯一新增本文件）。中文，标识符英文，每条带 `路径:行号`。
> 行号时效：`docs/analysis/GC-ROOFLINE.md` = 1,026 行、md5 `21025ea8…`；`docs/analysis/MEMORY-MODEL-CURRENT.md` = 1,079 行、md5 `76cf4f91…`（两份仍在被 Lead 编辑，行号失配时按 § 标题+引文定位）。
> 构建口径：我自建 `build-da2`（`rm -rf build-da2 && make BUILDDIR=build-da2 -j24`，exit 0、0 warning，`build-da2/extc` md5 **10960ce7ba1183b07feffba561600a71**、3,020,176 B）。源码 md5 与 v7 冻结一致：`src/back/reclaim.c` = `e17080d1c80513b27ca06f2bc6224b0f`（4,031 行）、`src/back/codegen.c` = `d339382889b87c0dd18b357c69e2e779`（16,017 行）、`tests/reclaim/driver.c` = `7b9ac61a55c140640a5964ab74a8780e`（1,523 行）。同源码不同 BUILDDIR 的 extc md5 不同（调试段嵌路径），溯源只认源码 md5。

---

## §0 结论速览

- **独立门禁**：tests/reclaim 默认 **201/0**、`EXTC_RC_NOCOUNT=0` **257/0**（ASan/UBSan、ALLOC_INLINE=0、chunk=1 已内嵌该脚本）、`gate.sh` **87,135,451 Ir** ≤ 88,029,115、`gate-release.sh` **4/4**、**G18 PASS**、G8 PASS、G1① PASS；停顿侧 G1②/G3b 仍 RED（与 Lead 结论同向）；**G20 只有 promote 的改善可确定性复现**，sizeratio/cycleratio 两轮都落在噪声地板附近。
- **四项裁决本身**（chunk / 配额归因 / regionFree 两阶段 / G1② 量具）方向都成立，但 **§6.7.1 的 B/C 档表把 deadline 开/关的峰值混在一行**（A1，结论不变）；**§7 G20 行的 step_total 判据在 v7 上不成立**（A2）；**§6.7.3/§7 G3b 的 pb 100M 数字与自家证据 `P1-REGIONFREE-BATCH.md:85` 矛盾**（A3）。
- **task-32 复核**：`unacc≈0`、子块集中 96–99%、control rate 2.0–2.3e-5 都被日志支持；**方法学结论成立且比报告更强**（我独立重跑 control2：cpu/tick = **84.0/85.1/93.7%**，报告写 70–80%）；但 **§6.7.8「32 µs 结构性下限」高估约 5×**（A4）、**R13 把 58–871 µs 整体写成已归因**（A5）、**§7 G1② 行「58.06 双双超 64 µs」算术错且与 g1c.json 判决相反**（A6）。
- **A 类 12 条 / B 类 8 条 / C 类 9 条 / D 类 0 条**（内存安全类门禁全绿，未发现 D 类缺陷）。

---

## §1 独立门禁实跑（build-da2，机器 load 0.2–0.9）

| 门禁 | 判据/期望 | 我的实测 | 判定 |
|---|---|---|---|
| tests/reclaim/run.sh 默认 | 201 ok / 0 FAIL | **201/0**（含 ASan/UBSan 一遍、ALLOC_INLINE=0 出口、chunk=1 出口、chunk=1e9 红自测） | PASS |
| run.sh `EXTC_RC_NOCOUNT=0` | 257 ok / 0 FAIL | **257/0** | PASS |
| tests/reclaim-perf/gate.sh | ≤88,029,115 Ir（doc 87,135,465） | **87,135,451**（−14 Ir = −0.00002%） | PASS |
| tests/reclaim-gaps/gate-release.sh | 4/4 | **4/4**：sweeps 1/1、N vs 4N = 1.00×、关档 stdout 逐字、releases 19/79、slots 4,883/20,303、RSS 3.52/3.09/3.47 MB | PASS |
| G18（`/tmp/gc-da2/g18.json`） | peak ≤ 26,624 kB 且 ≤1.05×peak60；stdout 逐字 | 60 轮 = 200 轮 = **26,076 kB**、ratio 1.000、hits 0、checks 244,736/4,180,480、stdout `D n=60 made=60 s=0` 逐字、6 个 exit 全 0 | PASS |
| G8（`/tmp/gc-da2b`） | ≤1.1× | **0.520×**（T1 VmHWM 中位 1,580 / T2 3,040 kB；交叉 timemax 1,496/3,036 = 0.493×） | PASS |
| G1① | over1ms = 0 | cyc/pc/pb/pd 全 0 | PASS |
| G1② | 三通道 | (a) cyc RED（p99.9>10 µs）、pb 跨阈 N/A；(b) pd_bigobj **N/A**（跨桶）、dense **N/A**（p999 落 32–64 桶）；(c) pb regionfree max **8,397.0 µs** RED、dense **1,647.1 µs** RED，pd_bigobj 622.5 PASS | RED |
| G3b | 单次 cpu ≤1 ms | pd_bigobj 中位 **622.5 µs** PASS；dense 中位 **1,647.1 µs** RED；pb 50M sync **6.35–7.39 ms**、[chan] 7.36–8.40 ms RED | RED（同向） |
| G20 | 见 §2 | promote 改善、sizeratio 临界、cycleratio 回归 | 见 §2 |
| G4/G14/G15/G16/G17 | — | **只核对** p1-after 产物，未独立跑 stress.py | 只核对 |

补充：`run.sh g1cpu` 不自建 cyc/pb/pc（只有 g4 段 build），首跑会 N/A；我用手工编译的三形状补跑（同 CFLAGS `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`）。

---

## §2 G20 独立交错 A/B（自建 harness，奇偶换序、9 reps、pin 6）

before = `build-lead5/extc`（md5 `9bef54886f49d174c01a0447be9ad3fc`，v5）；after = `build-da2/extc`（v7）。
A 组（`tests/gc-p0/ab.py`，wall 中位 + callgrind Ir，JSON `/tmp/gc-da2-ab/ab.json`）：

| 形状 | wall A→B | Δwall | ΔIr | ΔRSS | Δstep_total | ab.py 判决 |
|---|---|---|---|---|---|---|
| promote | 43.913→40.264 ms | **−8.31%** | −1.84% | −5.69% | **+0.03%** | RED（对称判据把改善判红） |
| sizeratio | 21.168→22.270 ms | **+5.20%** | +0.72% | +0.23% | −5.50% | RED |
| chase | 127.52→125.12 ms | −1.88% | +1.34% | +0.27% | −1.99% | PASS |
| cycleratio | 12.01→12.56 ms | +4.56% | +3.31% | +0.25% | −6.17% | RED |

B 组（自写 utime harness `/tmp/da2/ab2/res.json`，`/usr/bin/time` 的 wall/utime 中位）：promote wall 47.43→44.54（−6.09%）、**utime 41.30→35.49（−14.06%）**；sizeratio wall 24.14→24.22（+0.35%）、utime 21.62→22.62（+4.62%）；chase wall 130.91→129.42（−1.14%）、utime 101.12→109.76（+8.55%）；cycleratio wall 12.47→13.71（+9.93%）、utime 12.32→13.51（**+9.65%**）。

结论：
1. **promote 的改善是确定性的**（两组 A/B 同向：wall −6~−8%、utime −14%），RSS −5.7%。
2. **sizeratio 是临界回归**（+0.35% / +5.20% 两轮），和 doc §7 G20 行「未决」一致；但 doc 的「utime +5.1%、墙钟 +12.1%」用的是 Lead 的 ab8（v5→v7 ab8），与我这两轮不可逐值对齐（噪声地板 ±5–15%）。
3. **cycleratio 我测到 utime +4.6%~+9.7%**，比 doc 的「utime +1.9%、墙钟 +6.0% 临界」更差；但 off 臂 utime 散度 0.94–1.62，仍不能判回归。
4. **step_total 判据在 v7 上不成立**：doc §7 G20 行（`GC-ROOFLINE.md:743`）的判据含「promote step_total 降 ≥10%、sizeratio ≥40%」，而 v7 A/B 实测 **+0.03% / −5.50% / −6.17%**（promote / sizeratio / cycleratio）；doc 行内 verdict 只评 wall/utime/p99.9，把 §6.6（P0 轮 after = `build-rf 7019342b`）的 −20.2%/−56.4%/−10.8% 当成同一净账 ⇒ A2。
5. **工具与判据文字不一致**：`tests/gc-p0/ab.py:192-193` 的判决是 `abs(wall_delta_pct) <= 3.0`、`tests/gc-p0/g20.py:294` 是 `abs(dd) <= 3.0`，而 doc 判据写「**单侧** ≤+3%」⇒ promote −8.31% 被工具判 RED；`g20.py:279-280` 的 cycleratio 才是单侧（`cA <= cB*1.03`）。修法：把 ab.py/g20.py 的对称项改单侧（或 doc 改回「±3%」并说明改善也判红的原因）。

---

## §3 四项裁决逐项复核

### 3.1 项 1：SCAN_CHUNK 不缩小（保持 8192 + deadline 800）——**方向成立，但 §6.7.1 表两行混了 deadline 状态（A1）**

- 采纳理由的可核验部分成立：`P1-RQ3-CHUNK.md:195-201` 的裁决表（A = 8192+dl800 ⇒ 26.6 MB；B = 256+dl0 ⇒ 26.6 MB 但 allocstall 196；B′ = 128+dl0 ⇒ 26.6 MB 但 chase +6.2%；C = 128+dl800 ⇒ 215–240 MB；C′ = 256+dl800 ⇒ 126–134 MB）；峰值随 1/chunk 放大的那段（`:164`：8192:26.8 → … → 256:126–134 → 128:215–240 → 64:404–413 MB）**全部是 deadline=800 开着的**；`:166` 明说「关掉 deadline（=0）后配额必须当场花完 ⇒ 峰值回到 26.6 MB」。
- 但 `GC-ROOFLINE.md:628-630`（§6.7.1 表）把 **B 写成「256 + 0（关）⇒ 126–134 MB」、C 写成「128 + 0 ⇒ 215–240 MB」**，把 dl800 的峰值挂到 dl0 配置上；同一行的段步 p99.9（1.84/1.65 µs）与 chase +6.2% 又是 dl0 档的数字。正确拆法：**B = 256+dl0（峰值 26.6 MB，破的是分配内停顿 196 个 >1 ms）；B′ = 128+dl0（峰值 26.6 MB，破的是 chase 墙钟 +6.2%）；C = 128+dl800（峰值 215–240 MB，破内存）；C′ = 256+dl800（126–134 MB）**。
- 影响：结论「采纳 A（8192+dl800）」不变（内存优先是用户硬约束，A 是唯一同时满足 26.6 MB 与分配内 over-1ms=0 的档），但**当前表格会让读者以为「小 chunk 天然 5–9× 内存」**，掩盖了真实代价是「dl0 时把停顿搬进分配通道」。

### 3.2 项 2：配额记账不是成本；B（LAZY_FREE）默认关——**结论成立；证据未归档（C）**

- `reclaim.c:1064` `#define EXTC_RC_LAZY_FREE 0`（默认值确认）；`ALLOC_INLINE` 等与文档一致。
- 0/1 两档的分配路径（`reclaim.c:1517-1594`）**语义等价**：满判据同形（lazy: `bump>=nslots && nfree==0`；非 lazy: `freelist<0 && bump>=nslots`），0 档的 `freelist` 只由 `freeSlot` 的无条件 `freeSlotLink`（`:1736`）写入，`nfree` 只在 lazy 档维护。⇒「0 档与 v6 语义等价」成立（结构体多一个 `nfree` 字段，不是逐字节）。
- 三处「B 是 +9.31% 真因」的证据（`GC-ROOFLINE.md:640-641`：174 次 markAdvance = 0.0087%、deadline checks=0、`-DQUOTA_MAX_MARK=1`/`-DQUOTA_DEADLINE_US=0` 的 Ir 差 <0.1%）**只有摘要、无归档**：`/tmp/v2/attr` 仅 3 个二进制，callgrind 原始输出未入库 ⇒ C1。

### 3.3 项 3：regionFree 两阶段——**机制成立；pb 数字错（A3）；未接线为真**

- 静态核对（v7）：`regionFree` `:2638-2729`，同步段 = R3 守卫 `:2649-2656`、`abort` `:2657`、`pinDropRegion` `:2658`、`#if rsIncN` `unlinkChildren` `:2671-2678`、**`ownDel` `:2679` 在 `fqPushChain` `:2691` 之前**（不可推迟的理由成立：队列 slab 若仍在注册表，`owns()` 会把 stale 指针当活对象）；队列字节超限时 `:2693-2696` 强制排水；`[freequeue]` 打印 `:3665`。
- 排空：`fqPushChain` `:1399-1413`、`fqDrainCore` `:1415-1433`（`maxSlabs<=0`/`deadlineUs<=0` 无界）、`fqDrainAuto` `:1435-1443`（默认 8 slab / 100 µs；队列 >8 MiB 切 1000 µs 硬截止 + `forced++`）、`freeQueueDrainAll` `:1446-1449`。
- **未接线为真**：`freeQueueDrainAll` 全仓调用者只有 `tests/reclaim/driver.c`（3 处），`src/back/codegen.c` 无调用 ⇒ worker/协程退出不排空（进程退出把队列留给 OS 是设计选择，但「线程退出」路径确实缺一个调用点）。
- **pb 100M 数字矛盾（A3）**：§6.7.3 表（`GC-ROOFLINE.md:652-654`）写「sync 7.5–8.3 ms（我另测 13.2 ms）、总 8.6 ms、24,510 slab」，同一段下一句又写「退出 `left_slabs=24 316`/`left_bytes=25.7 GB`」；而它引用的证据 `P1-REGIONFREE-BATCH.md:85` 是 `sync_us=13755.6 queue_us=1005.0 max_slabs=24510 left_slabs=24316 left_bytes=25733331008`、`MODEL-UPDATE-P1.md:172` 总 14,762 µs。§6.7 小结（`:620-622`）也写「pb 侧仍是 **13.2 ms** 的同步 ownDel」⇒ 文档自相矛盾。12,059/12.76 GB 是 **50M 形状**的数字（我实测 pb 50M：`max_slabs=12,255`、sync 6,351.9/7,153.1/7,393.1 µs、queue ≈1,002 µs、`left_slabs` 12,019–12,130、`left_bytes` ≈12.7–12.8 GB、518–603 ns/slab），被错放进 100M 行。

### 3.4 项 4：G1② 量具（三通道 + B_seg + max 降级）——**量具成立；两处结论需修**

- 量具正确性（独立核）：`g1c.py` 逐桶差 `c_all[i]-segcounts[i]`，任一 <0 ⇒ ERROR（`:685-689`）、`n_seg>n_all` ⇒ ERROR（`:680-683`）；`pause_parse.py` 的 15 桶边界与 `reclaim.c:1212-1213` 一致、`OVER1MS_FROM=11`；seg 定义 = `markResume` 返回字数 >0 的步（`:2967-2983`）✓。精度注：stale-cursor 步（`scanObj` 非 0 但 `markResume` 返回 0）不计入 `[segsteps]`，落进 (a) 分母 ⇒ (a) 的 over-50 占比偏保守（C9）。
- **task-32 复核（`docs/analysis/P1-STEP-TAIL.md`，99 行，md5 `93b1a482ca62c58011a40f3cfed6222d`；日志 `tests/gc-p0/out/p1-step-tail/`）**：
  - 支持：`unacc=0` 在 7 形状全部成立（`maxphase`/`top1` 行，探针把 `markResume` 切成 16×512-word 子块）；**子块集中 96–99%** 成立（pd_bigobj rep1 156.6/163.5=95.8%、rep2 322.4/325.6=99.0%；dense rep1 255.6/269.8=94.7%），chase 的 54/71/9% 是例外；max 步主导相位 resume/pop/diag 与表 1 一致（promote rep1 pop=153.6 µs/pops=256；dense rep1 diag=2,670.3 µs、cpu 仅 1.1 µs）；探针开销 +0–0.5%（dense 最高 +7%）。
  - **方法学结论成立且更强**：我独立重跑 `control.c`/`control2.c`（`cc -O2 -std=c11`，`taskset -c 6`，19:23 UTC、load≈0.35、各 3 reps）：control 25,000 over50 = 5/9/3（rate 1.25/2.25/0.75e-5）、worst 84.1/254.1/194.4 µs；control2 400,000 over50 = 111/136/107（rate 1.73/2.13/1.67e-5）、worst 520.3/458.6/469.0 µs、**stall_sum_cpu/stall_sum_tick = 9,310.8/10,940.2 = 85.1%、11,457.5/13,637.2 = 84.0%、9,563.3/10,210.3 = 93.7%**、worst_cpu 522.4/461.7/470.6 ≈ worst ⇒ 「本机 `CLOCK_THREAD_CPUTIME_ID` 把被抢占时间算进 CPU，`cpu_us≈dur_us` 不能当 GC 工作证据」**成立**；报告写 70–80% 是保守值。**但报告的 worst 870–2345 µs 在高负载时刻记录，空载机只 458–522 µs** ⇒ 该数字依赖测量时刻/load，应标时间戳与 load（C8）。
  - **32 µs 结构性下限与自身数据矛盾（A4）**：用同一批日志算实测每段成本——pd_bigobj `resume_loop=156,563.6 µs / resume_n=25,407` = **6.16 µs/段**（8192 words ⇒ 0.75 ns/word）；dense `414,807.7/32,512` = **12.76 µs/段**（1.56 ns/word）；p1-after `[segsteps]` 的 pd_bigobj 桶（b4us 5,870 / b8us 12,021 / b16us 7,282 / b32us 192 / b64us 31 / b128us 9 / b256us 2）⇒ **p50 4–8 µs、p99.9 58.06 µs**。所以 `GC-ROOFLINE.md:714` 的「32 µs（3.9 ns/word × 8192）」高估约 5×，且 3.9 ns/word 与 §6.7.4 的 7.087/11.077 ns/word、MEMORY-MODEL §19.1 的 9.5–12 ns/word **三处互斥**。正确写法：「一段典型 4–16 µs（p50 4–8 µs；固定成本主导），5 µs 预算 < 一段 ⇒ 必须按字数在段内停」。
  - **R13 泛化过强（A5）**：`GC-ROOFLINE.md:763` 把「58–871 µs」整体写成已归因，但 task-32 只测 chunk=8192 的 6 形状（段步 max 329/334 µs）；871 µs 来自 §6.7.1 的 128/256 观测，未在 task-32 中测量 ⇒ 应限定「chunk=8192 的 58–329 µs 已归因；小 chunk 的 871 µs 未测」。
- **§7 G1② 行 A 类硬错（A6）**：`GC-ROOFLINE.md:723` 写「(b) pd_bigobj p99.9 **58.06 µs**（B_seg=88.1）、dense **90.74 µs**（137.1）双双超 64 µs」——58.06 < 64（算术错），且 `tests/gc-p0/out/p1-after/g1c.json` 对 pd_bigobj (b) 的判决是 **N/A**（p99.9 落 32..64 跨桶，桶界兜底 64 µs 不可判；over50 4.58e-4/2.24e-3、over1ms=0、max 203），只有 dense (b) 是 **RED**（n=162,560、p999>64、B_seg=137.114、over50 1.56e-3/8.18e-3、max 256.9）。我 build-da2 复跑：pd_bigobj (b) N/A（n_seg 127,035、p999 32..64、max 163.7）、dense (b) 也 **N/A**（每 rep n_seg 32,512、>64 µs 仅 26/rep=0.08% ⇒ p999 落 32–64 桶）。即 dense 的 RED 依赖 bseg 精确分位（跨构建标定），同一形状在我的构建上不可判 ⇒ 行内应注「dense (b) 依赖 `bseg-cal` 标定口径，跨构建会翻 N/A」。

### 3.5 task-26/27/28 的三项验收复核（补 §1 之外）

- **task-26（collects 不膨胀 + 段长上界）**：G18 200 轮 collects **199**（≈1/轮，不随轮数涨）、60 轮 59；pd_pointer_dense 200 轮 640；`[step] max` 163.7 µs（我）/113.4（p1-after 单 rep），远低于 deadline+2 segments 的上界 1,086.7 µs，over1ms=0 ⇒ **collects 不膨胀与段长上界都成立**。注意：`[step]` 只覆盖 mark 步，regionfree 通道的 8.4 ms 不在其中（§1 已单列）。
- **task-27（信用守恒 / sizeratio 回归是否真收回）**：**没有完全收回**——v7 A/B 的 sizeratio 墙钟 +0.35%（我，9 reps utime 中位）/+5.20%（ab.py），utime +4.62%；doc 的 Lead ab8 是 +12.1% 墙钟/+5.1% utime。三组同向（回归 ≤ 噪声地板上沿），但**不能判「已收回」**；promote 反而确定改善（utime −14.06%）。机制侧按 §6.7.2 归因于 B 的懒链重建已默认关（`reclaim.c:1064`），残余 +5% 未归因（doc 归 L17）。
- **task-28（延迟释放队列不变式）**：静态成立——队列头/字节是 `_Thread_local`（`reclaim.c:813-815`）、累计计数用 `__atomic_*`（`:816`）；`ownDel` 在挂链前完成（`:2679` vs `:2691`），队列 slab 不会留在注册表；`unlinkChildren` 在 `#if rsIncN` 下同步做（`:2671-2678`，默认档 rsIncN=0 不执行）；排空预算 8 slab/100 µs、>8 MiB 切 1000 µs 硬截止、`freeQueueDrainAll` 是唯一无界入口（`:1446-1449`，生产代码无调用）。RSS 侧：G18 60=200=26,076 kB（上界 26,624、余量 548）、gate-release N vs 4N = 1.00×、pb 50M 退出 `left_slabs` 12,019–12,130（有界尾巴）⇒ **不变式成立**；唯一缺口仍是「worker/协程退出不排空」（设计选择，非安全缺陷）。

---

## §4 A 类：错误/不可复现断言（12 条，按严重度）

**A1（§6.7.1 表 B/C 行，`GC-ROOFLINE.md:628-630`）**：把 dl800 的峰值（256⇒126–134 MB、128⇒215–240 MB）标到 dl0 配置上；dl0 的 256/128 峰值是 26.6 MB，破的是分配内停顿（allocstall 196/179）与 chase 墙钟 +6.2%。证据 `P1-RQ3-CHUNK.md:195-201、:164-167、:106-111`。修法：按上表拆 B/B′/C/C′ 四行。
**A2（§7 G20 行，`:743`）**：判据含「promote step_total 降 ≥10%、sizeratio ≥40%」，但 v7 A/B 实测 +0.03%/−5.50%；行内 verdict 只评 wall/utime/p99.9，把 P0 轮（`build-rf 7019342b`，§6.6 `:611-612` 的 −20.2%/−56.4%）当同一净账。修法：G20 分两行（P0 轮步总时长 / v7 墙钟+utime），或删掉 step_total 判据并写明「v7 上 P/B 的步总时长收益已被 P1 的 chunk/deadline 抵消」。
**A3（§6.7.3 表 + §7 G3b 行，`:652-654、:727`）**：pb 100M 写成 sync 7.5–8.3 ms / 总 8.6 ms / left 12,059/12.76 GB，而自家证据 `P1-REGIONFREE-BATCH.md:85` 是 sync **13,755.6 µs**、queue 1,005.0、max_slabs 24,510、left_slabs **24,316**/25.7 GB；`MODEL-UPDATE-P1.md:172` 总 **14,762 µs**；§6.7 小结 `:620-622` 自己写 13.2 ms。12,059/12.76 GB 是 50M 形状的数。修法：pb 100M ⇒ sync **13.2–14.2 ms**、通道 14.2–14.8 ms、left 24,316/25.7 GB；50M ⇒ 12,255 slab、sync 6.35–7.39 ms、left 12,019–12,130/12.7–12.8 GB（我的实测）。
**A4（§6.7.8 末句，`:714`）**：「chunk=8192 结构性下限 ≈32 µs（3.9 ns/word×8192）」与 R13/task-32 数据矛盾（实测 6.16/12.76 µs/段、0.75/1.56 ns/word；p50 4–8 µs；p99.9 58.06）。修法：删 32 µs，改「一段典型 4–16 µs，固定成本主导 ⇒ 5 µs 必须按字数在段内停」。
**A5（R13，`:763`）**：「58–871 µs 已归因」超出 task-32 实测范围（只 chunk=8192，max 329/334 µs；871 µs 来自小 chunk §6.7.1）。修法：限定句 + 标 870–2345 µs 为高负载观测（空载 458–522 µs）。
**A6（§7 G1② 行，`:723`）**：「58.06 µs … 双双超 64 µs」算术错；g1c.json 判 pd_bigobj (b)=N/A、仅 dense (b)=RED。修法：改为「pd_bigobj (b) N/A（跨桶）；dense (b) RED（bseg 标定口径，我构建上 N/A）」。
**A7（§7 after 列混构建，`GC-ROOFLINE.md:825` 声明 after=v5 `9fd0dcd8`，但 §6.6 用 `7019342b`，G18/G5/G1②/G3b 用 v7 `build-p1/p1-after`）**：具体偏差——G8 行 1,572/3,284 kB、0.479×（p1-after 产物 1,576/3,040、0.518×；我 1,580/3,040、0.520×）；G7 行 3,244 kB=3.17 MiB（p1-after 2,996 kB）；G4 行 9,148/41,704/93,432（p1-after 9,108/41,820/83,040）；G14 行 1.000×/3,228 kB（p1-after ratio 1.071×、800k 2,844、ctl 3,040）；G15 行 14.99×/189,776/12,664（p1-after 14.678×/185,936/12,668）；G16 行 wall 0.055 s（p1-after 0.044 s）。修法：每行标 build 尾号，或整表按同一 after 重生成。
**A8（MEMORY-MODEL §19.2 大对象行，`:927`「本轮 after」）**：n=510、max 78–98 µs、total 4,504–4,508、mean 8.8–8.9 与 65536 对照（n=643、164–269 µs、28,572–31,453）来自 `MEASUREMENTS-GC-P0.md:701`（build-emp `f97a425`、pd n=200、强制 `-DEXTC_RC_SCAN_CHUNK`）——**P0 轮、不是 v7**；v7 pd_bigobj 200 单 rep `[step]` n=26,116 / total 144,038.6 / max 113.4。修法：改标「P0 轮强制 chunk 对照」并把 v7 数放 §22-5。
**A9（MEMORY-MODEL §19.4，`:963`）**：用 P0 轮 `pd_bigobj after 单步 max 185.3 µs、step_max 中位 66→185`，与 §22-5 `:1071` 的 v7「×28.7（144,038.6 µs）、66→113.4」冲突。修法：§19.4 改 v7 口径（113.4 µs 单 rep / 163.7 我测），P0 旧值标注。
**A10（MEMORY-MODEL §22-2，`:1067`）**：仍写 regionFree「86–119 ms 级…G3b 稳健红，P0-2 下一轮第一优先」，与 §19.5 已落地两阶段矛盾（pd 493.2–743.5 µs、pb 13.2–14.2 ms）。修法：改「pd 已闭合（493–743 µs PASS）；dense 1,365–1,849 µs RED；pb 100M 13–15 ms RED；下一优先 = region 级 closing 队列（`P1-REGIONFREE-BATCH.md:109`）」。
**A11（§7 G17 行，`:741`）**：A=22,240 / D=25,696 / B=70,240 / C=70,236 kB、结论「A<D<B≈C」，来自 p0-before（`tests/gc-p0/out/p0-before/summary.txt:97-98` 等）；p1-after 实测 **A=27,812 / D=25,700 / B=C=64,100**、结论「A>D、B=C、A<B<C 不成立」（`tests/gc-p0/out/p1-after/summary.txt:96-105`；`MEASUREMENTS-P0-IMPL.md:308/347` 已记「after A 比 before 高 25%、顺序反转、机制未定位」）。修法：换 v7 数并把观察项结论改成「A>D；B=C；A<B<C 不成立」。
**A12（§7 G3a 行，`:726`）**：判据「free 次数 ≤10·#slabs+#classes+3，全部样本满足（pb 110190≤122433…）」在 v7 默认构建上不可复现——`reclaim.c:798-799` `EXTC_RC_FREE_DEFER` 默认 1，`_frees += 9+(big?1:0)` 只在 `#if !EXTC_RC_FREE_DEFER`（`:2663-2665`），`:2720` 只有 `_frees += 2`；我 pb 50M 的 `[regionfree]` 是 `max_frees=3`。修法：判据改「同步段 free 次数 ≤ #classes+3」+ 用 `[freequeue] drained_total` 计总量，或限定 DEFER=0 构建。

---

## §5 B 类：行号/口径漂移（8 条）

**B1 §17.2 常量表行号列（`MEMORY-MODEL-CURRENT.md:756-800`）**：26 行漂移（doc 值 → v7 实际）：SLAB_SIZE 321→330、SLAB_HDR 322→331、CHUNK_MIN 326→335、SLOTS_MIN 327→336、REUSE_MAX_SLOT 330→339、RCP_SHIFT 352→361、OWNMAP_BITS 417→426、TTAB_LINEAR_MAX 707→716、SWEEP_BUDGET 970→1013、SEED_PUSHES 1005→**1104**、SEED_WORDS 1008→**1107**、MARK_EVERY 1011→**1110**、MARK_WINDOW 1014→**1113**、MARK_STEP 1017→**1116**、BATCH_MAX 1020→**1119**、BATCH_POP 1023→**1122**、ASSIST_CAP 1226→**1326**、RELEASE_BUDGET 980→**1079**、PRESSURE_MUL 990→**1089**、CASCADE_DEBUG 1573→**1851**、WORK_BUDGET 1239→**1339**、ALLOC_INLINE 1282→**1384**、FOREIGN_GUARD 1394→**1648**、SHADE_GUARD 1750→**2028**、MARK_SHALLOW 2536→**2841**、SCAN_CHUNK 2563→**2875**。已正确的行：SLAB_BITS 138、FLOOR 141、SCALE_MAX 144、SCALE_MUL 151、HOLD_SAT 154、CLS_BUCKETS 211、QUOTA_{SHIFT,MAX_WORDS,MAX_MARK,DEADLINE} 1037/1040/1044/1058、LAZY_FREE 1064、ELIDE_VERIFY 1069、FREE_DEFER 799、FREE_BUDGET_SLABS 802、FREE_DEADLINE_US 805。
**B2 §19.1 常量锚点（`:913-914`）**：`reclaim.c:1104/1008/1020/1023/1226/1017` 只 1104 对 ⇒ 1008→1107、1020→1119、1023→1122、1226→1326、1017→1116。
**B3 §7 G18 行（`:742`）**：代价列 step_n 26,116 / total 144,038.6 是**单 rep**，而 `tests/gc-p0/out/p1-after/g18.json` 的 rounds[200] 是 **5 reps 合并**（131,245 / 720,193.1）；`MEASUREMENTS-P1.md:150` 两者都记 ⇒ 行内应标「单 rep，括号内为 5-rep 合并」。
**B4 §7 G1② 行 pb (a) 4.25e-4–1.21e-3 是 p1-after 值**；我 build-da2 是 9.99e-5–4.00e-4（跨阈 ⇒ N/A，over50 上界 <1e-4）。区间随构建/rep 变，应标 build 与 reps。
**B5 §6.7.3 三源差异**：pd_bigobj 493.2 中位/711.4 max（doc）、622.5（我 5 reps 中位）、743.5（runtime-facts）；dense 1,365.5/1,370.7（doc）vs 1,647.1/1,849.2（我）⇒ 10–35% 跨构建/负载散度，行内未标。
**B6 附录 D（`:978`）口径过期**：仍写「FREEZE v5 = 交付树、reclaim.c `9af9ca9b`/3,667 行、driver `c4ccf26a`」，而交付树已是 v7（`e17080d1`/4,031、driver `7b9ac61a`/1,523）。
**B7 §19.2 `:914`「8192 段实测 78–98 µs（9.5–12 ns/word）」**：该数是 P0 轮含冷页/抢占的 max 族，task-32 显示同段典型 4–16 µs、0.75–1.56 ns/word ⇒ 应标「max 族、含冷页」或改区间。
**B8 §6.7.3 pd_bigobj 判据写「PASS（≤0.75 ms）」**：0.75 ms 是目标值，G3b 门禁是 ≤1 ms ⇒ 措辞应区分「目标 0.75 / 判据 1 ms」。

---

## §6 C 类：缺证据/需补归档（9 条）

C1 §6.7.2 的三证据（174 次 markAdvance、2,020 次重建/1.89M 洞/≈19.5M Ir、两变体 Ir 差 <0.1%）无原始日志/ callgrind 归档（`/tmp/v2/attr` 只有 3 个二进制）。
C2 `P1-RQ3-CHUNK.md` 表 A 第一 rep、表 C bench 墙钟、表 D allocstall 无仓库归档（`tests/gc-p0/out/p1-chunk` 只有 pct 第二 rep；`/tmp/rfwork/chunk/pd.n2048.err` 42,964 kB 与 doc 34,704 不一致）。
C3 ab7（HEAD→v5）RSS 比不可逐值复现（`ab7.sh` 自删覆盖；我算 1.005/0.998/1.035/0.995 vs doc 0.996/0.994/1.035/0.990，差 ≤1.4%）。
C4 `tests/gc-p0/out/p1-after/summary.txt:53` 的 G18 RED 是旧量具产物（exit 列表含 None、未按 (case,rep) 去重）；doc 的 PASS 与 g18.json 一致 ⇒ 归档需重生成。
C5 c_seg 标定（7.087/11.077 ns/word）原始 pct 输出未入库（仅 `bseg-cal.tsv` 摘要）；且标定 build `07b7be1a` 与 v7 交付构建 `10960ce7` 非同一二进制，跨构建复用未证等价（我只有用 `G1C_C_SEG` 复用才可比）。
C6 工具判据不一致：`ab.py:192-193`/`g20.py:294` 对称 `abs(Δ)<=3` vs doc「单侧 ≤+3%」⇒ 改善也判红（promote −8.31% RED）。
C7 G18 我 26,076 vs doc 26,072（+4 kB 跨构建）；doc 写「60 = 200 = 26,072」应写「同一次跑内 60 = 200」。
C8 task-32 的 worst 870–2345 µs 依赖负载（空载 458–522 µs）⇒ 报告应标测量时刻/load（报告只写了 5 条 limitations，无 load 字段）。
C9 `[segsteps]` 不含 stale-cursor 步（`scanObj` 非 0、`markResume` 返回 0）⇒ (a) 分母含这类步，(a) 的 over-50 占比偏保守；建议在 g1c.py 注一句。

---

## §7 D 类：内存安全（0 条）

阶段 B 的所有内存安全门禁全绿：tests/reclaim 201/0（含 ASan/UBSan 与 `detect_leaks=0` 的界判据）、NOCOUNT=0 257/0、G18 ASan 侧（`tests/gc-p0` 内）干净、gate-release 4/4、G8 ASan 两侧干净。**未发现 D 类缺陷**，无需单发 Lead。
残余风险（静态，未触发）：① 队列 slab 的 `s->region` 悬垂只由「`ownDel` 在挂链前完成」隔离，若未来把 `ownDel` 推迟必须同时加 closing 标志（`P1-REGIONFREE-BATCH.md:109` 已写）；② `EXTC_RC_ELIDE_VERIFY=1` 的重扫断言只在 `-D` 构建下存在，默认档不验证 `popcount(occ)==nLive`；③ `freeQueueDrainAll` 无生产调用点，进程退出路径依赖 OS 回收（设计选择，但若未来加入「运行期销毁 worker」会漏排空）。

---

## §8 建议的最小修正清单（按优先）

1. **A1**（§6.7.1 表拆 B/B′/C/C′）——影响「为什么保持 8192」的论证可读性。
2. **A3**（pb 100M sync 改 13.2–14.2 ms、left 24,316/25.7 GB；50M 单列）——同一文档自相矛盾，最易被抓。
3. **A2**（G20 行分 P0 轮/v7 两行，或删 step_total 判据）——v7 上该判据不成立。
4. **A6**（G1② 行 58.06 不是 >64；pd_bigobj 判 N/A）——判据可审计性。
5. **A4/A5**（删 32 µs 下限；R13 限定 chunk=8192 与负载条件）。
6. **A8/A9/A10**（MEMORY-MODEL 的 P0 轮旧数三处：§19.2 大对象行、§19.4 185.3、§22-2 regionFree 86–119 ms）。
7. **A7**（§7 after 列标 build 尾号或整表重生成）。
8. **A11/A12**（G17 换 v7 数并改结论；G3a 改判据或限定 DEFER=0）。
9. **B1/B2/B6**（§17.2 整表 26 行、§19.1 5 锚点、附录 D 冻结口径改 v7）。
10. **C6**（ab.py/g20.py 对称判据改单侧，或 doc 改文字）。

复算值备份（供 Lead 抽查）：build-da2 = `10960ce7ba1183b07feffba561600a71`；G18 = 26,076/26,624；gate.sh = 87,135,451；gate-release = 4/4（19/79、4,883/20,303）；G8 = 1,580/3,040 = 0.520×；A/B promote = −8.31%/−14.06% utime；sizeratio = +5.20%/+4.62%；cycleratio = +4.56%/+9.65%；step_total = +0.03%/−5.50%/−6.17%；control2 cpu/tick = 84.0/85.1/93.7%。

---

## §9 独立跑 vs 只核对（分界）

- **独立跑（我自建 build-da2 或自写 harness）**：tests/reclaim 两档、gate.sh、gate-release、G18、G8、G1①/G1②（含 g1c 三通道）、G3b（pb 50M 是我自己编的形状）、G20 交错 A/B（ab.py + utime harness）、task-32 的 control/control2 重跑。
- **只核对（未重跑，仅对照归档/证据文档）**：G4-Stress（stress.py）、G14/G15/G16/G17（p1-after 产物与 doc 行）、`P1-RQ3-CHUNK` 的 bench/allocstall 表、ab7/ab8 的原始日志（未入库）、MEASUREMENTS-P1 的 interim 数字。
- **未做**：`EXTC_RC_M_MAX` 各档实测（G4 系列）、`-DEXTC_RC_SCAN_CHUNK` 变体重测（沿用 P1-RQ3 归档）、跨构建 bseg 标定等价性证明（C5）。
