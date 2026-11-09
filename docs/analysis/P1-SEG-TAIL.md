# 单步 p99.9 的真实构成：续扫段成本、宿主噪声地板与 chunk 的内存代价（v9）

日期：2026-11-09。输入：FREEZE v8 + 本轮 markResume 时间上限补丁。
冻结口径：reclaim.c = 3de5655435d4dfb2448b2168fee4ca19（4 156 行）、codegen.c = 2f560e2e6deef8c4927db4611344aecb（16 017 行，未改）、
tests/reclaim/driver.c = f45569f95eb58e791795229ec33dcd85（1 552 行）、build/extc = 1347363b9aaf511ece131de761d4a52c。

## 0 一句话结论

1. 本轮给续扫段加了**时间上限**（每 512 词读一次 CLOCK_MONOTONIC，超 40 µs 就存 cursor 返回），代价 gate Ir **+88,883（+0.10%）**；它使「段步时长」有了不依赖词长的硬界，但**没有**改善 p99.9。
2. 原因是口径错了：8192 词一段的**中位**成本只有 **7.92 µs（pd_bigobj）/ 13.08 µs（dense）**，而 p99.9 = 58.5/99.2 µs 是**宿主噪声**在短窗口上的投影，不是扫描工作量。旧标定把 p99.9 ÷ chunk 当「每词成本」（7.145/12.110 ns/word），把尾巴当成了成本，于是 B_seg 判据成了「尾巴 ≤ 1.5×尾巴」的循环论证。
3. 缩短 chunk 确实能把 p99.9 压到 50 µs 以下（2048 词：23–36 µs），但**破内存律**：G18 的 pd_bigobj 峰值 26,076 → **34,332 kB**（判据上界 26,624）。内存优先 ⇒ chunk 保持 8192。
4. 因此：「单步 p99.9 ≤ 50 µs」在本机、在 8 MiB 大对象形状上**不是 GC 参数能达成的目标**；可达且已验证的是 **p99 ≤ 50 µs**（实测 20.8/33.0 µs）＋ **over-1ms = 0**。要把 p99.9 也压下去，必须先让 chunk 能缩小，而前置条件是**分配准入按请求字节预留**（下一轮，见 §6）。

## 1 本轮改动（已落地，测试全绿）

- 新宏（src/back/reclaim.c，EXTC_RC_SCAN_CHUNK 之后、EXTC_RC_MARK_CHILD 之前）：
  - EXTC_RC_SEG_DEADLINE_US（默认 **40**，0 = 关）
  - EXTC_RC_SEG_CHECK_WORDS（默认 **512**，必须 ≥1）
- extc_rc_markResume 的扫描循环改成「每 CHECK_WORDS 词查一次 extc_rc_pauseNow()，超 DEADLINE_US 就 break」，并把**实际走过的词数**（walked）作为返回值 —— 时间截断后段步上报真实工作量，不再假装走满一个 chunk。
- 契约（源码注释同步）：
  P_step <= min(C_scan * EXTC_RC_SCAN_CHUNK, deadline + C_scan * EXTC_RC_SEG_CHECK_WORDS) + fixed step cost
  即暖存下由 chunk 界定；冷页/换页下由 deadline + 一个检查量化项界定（量化项 = 512 词 ≈ 一个 4 KiB 页的扫描成本）。
- 自测：tests/reclaim/driver.c 的两条分段断言从「每段恰一个 chunk / 段数恰 ceil(words/chunk)」改成「**不越过** chunk；有时间上限时只多分段、不并段」（判据仍必须成立，不是放宽为不判）。修前 ASan 档确定性红（通过 203，失败 1，段数 > want），修后：
  - tests/reclaim/run.sh 204/0；ASan 204/0；EXTC_RC_ALLOC_INLINE=0 204/0；chunk=1 13/0；chunk=1e9 红自测仍红。
  - tests/reclaim-perf/gate.sh = **87,240,547 ≤ 88,029,115 PASS**（相对无上限的 87,151,664，+88,883 Ir = **+0.10%**）。
  - G18：pd_bigobj 60 轮 = 200 轮 = **26,076 kB**（上界 26,624）、dense 112,092 kB —— 与 v8 逐 kB 相同（上限默认 40 µs，在暖步上从不触发）。

## 2 关键发现 A：p99.9 是尾巴，不是工作量

量具：tests/gc-p0/calib_cseg.py（pin 6，EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1，ROUNDS=200，pd 两形状各 3 reps，读生成产物里的 [probe-seg] 精确分位）；chunk 用 --cflags -DEXTC_RC_SCAN_CHUNK=N 在 cc 阶段覆盖（--chunk 参数会与产物里 #define 的 8192 冲突而报错，不要传）。

| chunk | 形状 | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| 8192 | pd_bigobj | 7.92 | 9.99 | 20.83 | **64.50** | 280.5 |
| 8192 | pd_pointer_dense | 13.08 | 15.90 | 33.0 | **84.69** | 412–600 |
| 4096 | pd_bigobj | 3.7–4.2 | — | 6.7–7.1 | 37.1 / 37.1 / 49.5 | 171–322 |
| 4096 | pd_pointer_dense | 6.2–6.4 | — | 15.7–16.8 | 52.3 / 56.6 / 61.7 | 220–485 |
| 2048 | pd_bigobj | 2.14–2.23 | 2.55–2.64 | 3.31–3.60 | **22.7 / 24.8 / 31.8** | 127–286 |
| 2048 | pd_pointer_dense | 3.12–3.15 | 3.89–3.92 | 6.81–7.40 | **33.8 / 35.5 / 33.9** | 194–403 |
| 8 | pd_bigobj | 0.145 | — | — | 0.67–2.77 | — |

（单位 µs；reps 之间用「/」分隔。）

- 「8192 words = 78–98 µs / 9.5–12 ns/word」是**把 max 族当成本**：真实每词成本取中位 = 7.92/8192 = **0.97 ns/word**（pd）、13.08/8192 = **1.60 ns/word**（dense）；chunk=2048 复算 2.16/2048 ≈ 1.06、3.12/2048 ≈ 1.52，一致。
- 时间上限对 p99.9 **无效**：-DEXTC_RC_SEG_DEADLINE_US=0/20/40 三档，pd p999 = 41.5–57.1 / 40.6–68.2 / 63.1–69.2，dense 78–111 / 68.9–79.3 / 75.3–93.0 —— 同一分布，差异在跨 rep 散布内。

## 3 关键发现 B：p99.9 由宿主噪声地板决定

证据 1（步级反例）：chunk=8 档的 [top] 抓到
kind=mark why=advance seg=8 pops=1 dur_us=351.4 cpu_us=351.4、
seg=0 pops=256 dur_us=351.2。
8 个词的段、1 次 pop 的步不可能真花 351 µs，且 cpu ≈ dur ⇒ CLOCK_THREAD_CPUTIME_ID 把被宿主抢占/steal 的时间计入了「CPU 时间」（与 P1-STEP-TAIL.md 的 task-32 结论一致）。

证据 2（噪声地板）：tests/gc-p0/noise_floor.c（cc -O2 -std=c11 -o /tmp/nf，taskset -c 6），只做「读时钟 + 空转」：

| 命令 | CPU 窗口 | >50 µs 事件 | p99.9 | max |
|---|---|---|---|---|
| /tmp/nf 2000000 500 | 0.37 µs | 71–118 / 2M | 0.42–0.52 µs | 177–704 µs |
| /tmp/nf 60000 10800 | 2.3 µs | 11–17 / 60k | 22–26 µs | — |
| /tmp/nf 1000000 10800 | 2.3 s 总 | 307（≈133 次/s） | 22.4 µs | 875 µs |

- 结论 1：在这台机器上，**任何** ≤13 µs 的 CPU 窗口，其 p99.9 都会被抬到 60–100 µs —— 与 GC 无关。
- 结论 2：GC 的 >50 µs 事件率 ≈ **200/s**（dense：70–114 次 / 32.5k 步 × 13 µs ≈ 0.42 s），与配对噪声率 133/s 同量级 ⇒ 尾部事件数与工作量无相关性。
- 结论 3（方法学）：p99.9 作为「GC 停顿」指标**在本机不可判**；可判的是 **p99（工作量项）**、**over-1ms = 0**，以及**配对的噪声对照**。

## 4 关键发现 C：chunk 下探能达标，但用内存换的

同一产物、只改 chunk（tests/gc-p0/run.sh g18，G18_PD_REPS=1，/usr/bin/time -f %M）：

| chunk | deadline | pd_bigobj 60 轮 | pd_bigobj 200 轮 | dense 200 轮 |
|---|---|---|---|---|
| 8192 | 40 | **26,076 kB** | 26,076 kB | 112,092 kB |
| 4096 | 40 | **34,332 kB** | 34,332 kB | 112,284 kB |
| 2048 | 40 | **34,332 kB** | 34,332 kB | 113,052 kB |
| 2048 | 0 | 34,332 kB | 34,332 kB | 113,048 kB |

- 判据：peak200 <= 3·live + 2·SLAB_SIZE（= 26,624 kB）且 peak200 <= peak60×1.05。chunk ≤ 4096 时**规模无关性仍成立**（60 = 200），但**绝对值破界 31%**（34,332 vs 26,624）。
- 增量正好 ≈ 8 MiB + 128 kB（= 一个 8 MiB 大对象 + 记账）：小 chunk 下标记推进变慢，使峰值多容纳一个在飞的 8 MiB 分配；这是**分配准入不看请求字节**的直接后果，不是 GC 工作量不足（总标记词数不变：两档 [segsteps] 分别 ~2.5 万 / 5.1 万步、每步走满 chunk）。
- 结论：在准入控制改成「按请求字节预留」之前，**chunk 不得小于 8192**（与 P1-RQ3-CHUNK.md 的内存优先裁决一致，本轮给出量化的破界值）。

**bseg-cal.tsv 的处置**：该表本轮用 v9 产物（`build/extc` = `1347363b`、`reclaim.c` = `3de56554`）重标了 chunk=8192 的两行（`pd_bigobj` c_seg 7.874 / p99.9 64.50、`pd_pointer_dense` c_seg 10.338 / p99.9 84.69），仅为让 `g1c` 的溯源 md5 与本构建一致；`c_seg = p99.9 / chunk` 是 **max 族代理**，`B_seg = 1.5·c_seg·chunk` 因而是「把尾巴拟合尾巴」，**不得再当判据**（§5 口径修正表第 4 行）。v9 与 v8 的 p99.9 差异（58.53/99.20 → 64.50/84.69）在同一噪声带内，不是回归。

## 5 修正后的模型口径

| 量 | v8 文档口径 | v9 实测口径 | 说明 |
|---|---|---|---|
| 每词扫描成本 C_scan（暖） | 9.5–12 ns/word | **0.97 / 1.60 ns/word**（pd / dense，p50） | 旧值是 max 族 ÷ chunk |
| 一段 8192 words 的典型成本 | 78–98 µs | **7.92 / 13.08 µs** | 「78–98 µs」只在冷页/抢占下出现（max 族） |
| 单步 p99.9 | 58.5 / 99.2 µs（判据 B_seg） | 同值，但**归因为宿主噪声** | 噪声地板 p999 = 22–26 µs、事件率 133/s |
| B_seg = 1.5·c_seg·chunk + 1 µs | 判据 | **循环论证**（c_seg 由 p999 拟合 ⇒ 阈值 = 1.5×自身） | 量具项，下一轮改；当前 red 方向仍保守（不会把超标判成通过） |

**P_step 的新写法**（与源码注释一致）：
P_step = min(C_scan × CHUNK, DEADLINE + C_scan × CHECK_WORDS) + fixed
代入暖存：pd 0.97 ns/word × 8192 = 7.9 µs；dense 1.60 × 8192 = 13.1 µs。含大对象段的单步因此是 **8–13 µs 的工作项**，obs p99 = 20.8/33 µs 已含噪声（约 2–2.6×），over-1ms = 0。

## 6 现状与下一轮（按优先级）

1. **分配准入按请求字节预留**（唯一能把 chunk 降到 2048 的前置条件）：在大分配路径上，若 heap + n > M_max，先做**有界**回收；仍不满足时按「内存优先」推进到硬截止的全量回收，使峰值不随 chunk 变化。判据：chunk=2048 下 G18 = 26,076 kB 且 [segsteps] p99.9 ≤ 50 µs（精确分位）。
2. **(b) 判据改造**（量具）：把 B_seg 从「p999 拟合 p999」换成两种可判形式之一：
   - p99 ≤ 50 µs（工作量项，B99 = 3 × C_scan(p50) × chunk + 1 µs）；
   - 或「GC 的 >50 µs 事件率 ≤ 2 × 配对噪声率」＋ over-1ms = 0（尾部归因项）。
   两种都要在 bseg-cal.tsv 里带 p50/p99 列与 noise_floor 配对，源码 md5 变了必须重标。
3. **G1②(a)** warm 通道（over-50 µs 占比 4.25e-4–1.21e-3）同样应换成配对噪声率口径 —— 否则它测的是宿主负载，不是回收器。
4. 文档：本文 §5 的口径上线后，把 GC-ROOFLINE §6.7、MEMORY-MODEL §19.4 的 c_c、9.5–12 ns/word、78–98 µs 全部按「暖/冷两口径」重写（本轮只加了脚注，未整段重写）。

## 7 复现命令

    # 精确分位（chunk 在 cc 阶段覆盖）
    python3 tests/gc-p0/calib_cseg.py --extc ./build/extc \
      --src-md5 "reclaim.c=$(md5sum src/back/reclaim.c|cut -d' ' -f1),codegen.c=$(md5sum src/back/codegen.c|cut -d' ' -f1)" \
      --build-md5 $(md5sum build/extc|cut -d' ' -f1) --out tests/gc-p0/out/cq-ch2048 \
      --cflags "-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -DEXTC_RC_SCAN_CHUNK=2048"
    # 内存律（chunk 同上覆盖）
    CFLAGS="-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -DEXTC_RC_SCAN_CHUNK=2048" \
      EXTC=./build/extc OUT=tests/gc-p0/out/ab-ch2048 G18_PD_REPS=1 tests/gc-p0/run.sh g18
    # 噪声地板
    cc -O2 -std=c11 -o /tmp/nf tests/gc-p0/noise_floor.c && taskset -c 6 /tmp/nf 1000000 10800
