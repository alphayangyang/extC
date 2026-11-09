# P0 实现（按字节配额 P + 惰性 freelist B）的 before/after 实测

> 任务：task-23。范围：P0（P 配额按字节 + B 惰性 freelist）落地进编译器后，用**同一台机器、同一组开关**
> 跑 before/after，回答两件事：(1) 大对象反复分配的内存是否被约束（G18，含 P 的单步停顿代价）；
> (2) 停顿收益是否落地（G20，收益主体是 B），以及既有门禁是否回归。
>
> **状态：已完成（before + after 都跑完）。** before = `./build-lead5/extc`（md5 `9bef54886f49d174c01a0447be9ad3fc`，未改编译器）；
> after = `./build-rf/extc`（md5 `7019342b3e5ccddbe67cbdc96eae2395`，对应源码 `src/back/reclaim.c` md5 `7c2a9cedd7edd54de0a378d8a507dcbe`（3801 行）、
> `src/back/codegen.c` md5 `d339382889b87c0dd18b357c69e2e779`（16 017 行；Lead 更正：原记 `cf288544…`/15994 是 v4 口径，实际 `codegen.c` mtime 12:11 早于 after 构建 16:01）、`tests/reclaim/driver.c` md5 `83e167d761705268dbda3f89895ea574`；Lead m03911 的 GO 构建）。
> 结果目录：`tests/gc-p0/out/p0-before` 与 `tests/gc-p0/out/p0-after`（均 REPS=3，after 的 G20 为 5 reps、pin=6）。
>
> **结论一句话**：G18 从 before 的 **2.163× 无界 RED** 变为 after 的 **26,264 kB 平台 PASS**（代价：step 数 ×61、GC 总时长 ×33）；
> G20 的三条 step_total 判据全 PASS（promote −20.2% / sizeratio −56.4% / cycleratio −10.8%），但汇总裁定 **RED**
> （promote step_max 中位 63.8 > 60 µs；promote wall −11.2% / rss −5.7% 越出 ±3%）；**同轮交错 A/B 确认 sizeratio 墙钟 +9.31%（Ir +6.00%）是真回归**（§4.2）。
>
> 数字来源标注：**【本机实测】** = 本文件 §3/§4/§5 的表；**【文档】** = 只在与原型/历史数字对照时出现，且逐处标注出处。
>
> before 锚点 = Lead 在 build-lead5/extc 上的实测（m03207），本机已独立复现（§3.1/§4.1），逐字一致（493,400 / 1,067,288 kB）。

---

## §0 结论速览（待 after 后定稿）

1. **归因先分清**（Lead m03278）：**P 的收益/代价主要落在 G18**（大对象不再累积、内存被配额压住，代价是大对象分配的单步可能变长）；**G20 的收益主体是 B**（惰性 freelist 把清扫从"每步大段"改成"小切片"）。G20 **只有 B 落地后才可能转绿**，P 落地本身不应被记为 G20 的功劳。
2. G18 before（本机复现）：60 轮 peak **493,400 kB**、200 轮 **1,067,288 kB**（2.16×，≈130 个 8 MiB big 未回收）⇒ 无界；before 的停顿列（60 轮 step_n=121 / total 1,325.8 µs / max 20.1 µs / collects=1；200 轮 step_n=428 / total 4,617.7 µs / max 35.8 µs / collects=7）。
3. G20 before（本机 1 rep 冒烟）：promote step_total **17,380.0 µs**、max 58.2 µs；sizeratio **2,537.8 µs**、24.2 µs；cycleratio **431.2 µs**、18.5 µs —— 与原型 before（17,752 / 2,581 / 471.8 µs）同量级。
4. 正式 before/after 均已跑完（2026-11-09 15:45 与 16:0x，串行窗口）：before = `./build-lead5/extc` md5 `9bef5488…`；after = `./build-rf/extc` md5 `7019342b…`（源码 `src/back/reclaim.c` `7c2a9ced…`/3801 行，Lead m03911 GO 构建）。跑前宏自检 after 产物：`QUOTA_MAX_MARK 1048576 / QUOTA_SHIFT_MARK 3 / QUOTA_MAX_WORDS 8192 / QUOTA_DEADLINE_US 800 / TIER 2 / NOCOUNT 1 / RELEASE_BUDGET 2048` 全部匹配 ⇒ 可作权威 after（§1.1、§7-7）。
5. G18 判据已按 doc-audit 复核收紧（Lead m03358 + m03449）：内存判据改为 live 从 `[work] M_max/3` 反读 + `peak200 ≤ 3·live+2·SLAB` + `peak200 ≤ peak60×1.05`，RSS 用 timemax 的 ru_maxrss；停顿拆成 **① 常规通道（n≤64 KiB）切片 ≤5 µs** 与 **② 大分配通道（p99.9 + over1ms=0 + P_step 闭式）**（§2.1）。
6. 新增 `pd_pointer_dense` 观察用例（8 MiB + 1M 非空指针 × 200 轮）：before 峰值 **140,504 kB**（≈8.8× 活集）、over1ms=**0**、p99.9 ≤32 µs，但 >8 µs 步占 2.9%，[top] 冷 pop 成本 ≈1.3–1.6 µs/pop（§3.3）。**只作观察，不阻塞 G18**。
7. **判据按 Lead m03564 + m03746 收紧**：(i) P_step 闭式改为**相加式** `P_step ≤ t_sweep(W_s) + c_u·W_m + c_a`；(ii) `q=8192` 是 43× 高估（`slotSize≥16 B ⇒ nslots<4096 ⇒ ceil/64≤64`）⇒ 紧上界 `W_s = 8·16+64 = 192 words`、标称 **145.2~145.7 µs**、`deadline 800 µs` 天花板 **≈1086.7 µs**；`c_a` 置 0（初版 0~18 µs 是拟合残差、不可伪证）；(iii) G18 ①「常规通道切片 ≤5 µs」在混合形状上**降级为不可判**，且 15 桶直方图上 ≤5 µs 只能给上界、**N/A ≠ PASS**；(iv) after 的 G18 表与 `pd_pointer_dense` 都要加 `deadline_hits`（`[quota]` 终值行 total；per-round 行只给 `round_hits`，不得覆盖 total —— m03775）与 `[step] max` 列；(v) before/after 闭式**不可比**（语义变了），现场只比 RSS/stdout/step_max/histo（§2.1）。
8. `pd_pointer_dense` 的 seg 占比（m03564 要求，before 实测）：mark 步 584,267 中 25,600 步（4.38%）在续扫大对象，累计 209,715,200 seg words = 200 轮 × 1,048,576 指针槽 ⇒ **每轮 128 段、每段一步**（§3.3）。


9. **G18 after = PASS（本机实测，§3.2）**：pd_bigobj 60 轮 **26,264 kB**、200 轮 **26,264 kB**（1.000×；before 493,400→1,067,284 = 2.163× RED）；live 反读 8,192 kB ⇒ 上界 26,624 kB，内存项 PASS；规模项 PASS；stdout 两档逐字 ok、exit [0,0]；`deadline_hits` 0/1；② 大分配通道 PASS（p99.9=64 µs ≤ 闭式 145.2–145.7 µs、over1ms=0）。**代价必须同表列出**：step_n 428→**26,116**（×61）、step_total 5,018.4→**165,611.9 µs**（×33）、step_max 66.0→**185.3 µs** —— 这是 P 用停顿换内存，不是回归，但也不是免费胜利。
10. **G20 汇总 RED**：三条 step_total 判据全 PASS（promote 18,406.9→14,692.6 µs = **−20.2%**、sizeratio 2,606.8→1,136.5 = **−56.4%**、cycleratio 413.7→369.2 = **−10.8%**；chase −3.2%），但 (i) promote `step_max` 跨 reps 中位 **63.8 µs > 60 µs**（观察口径，单次 max 不作判据）；(ii) promote wall **−11.2%**、rss **−5.7%** 越出 ±3% 对称判据（改善方向也照判 RED）；(iii) **sizeratio 墙钟 +2.0%（5 reps 顺序跑）/ +9.31%（同轮交错 A/B 9 reps）** ⇒ 与 runtime-facts 报的 +10.34% 同源，是真回归（§4.2）。
11. **sizeratio 回归归因（部分，§4.3）**：B 把 sweep/mark 从"每步大段"改成"每分配小切片"，在 sizeratio 这种 ≈12 ns/alloc 的短命形状上把每分配固定成本放大：Ir **+6.00%**（满条件行 +5.11M、freelist 探针 +6.00M，sweepStep 自身 −20.2M）；**D1mr −6.1%、DLmr −17.2%** ⇒ 不是访存变差；把 P 的 mark 配额压到 0（`-DEXTC_RC_QUOTA_MAX_MARK=0 -DEXTC_RC_QUOTA_DEADLINE_US=0`）收回 **−7.34%** 墙钟 ⇒ 与 P 的每分配配额记账同源。剩余 ≈+2% 墙钟未归因（callgrind 无 branch sim），如实写"部分未归因"。
12. **既有门禁 after 基本全绿（§5）**：G8 0.515×、G9 3/3、G12/G13 PASS、G14 对照比 1.000× PASS、G15 XFAIL 14.677×、G16 3/3 PASS、G4-Stress PASS（A/B/C = 9,104 / 41,820 / 82,956 kB）、G17 A<B≈C、L10-b 6.65× / L10-c 14.08×；**G1② 仍 RED**（pb 下界 3.31e-4、pd 5.62e-4），G1① PASS、③ 部分 PASS + 部分 N/A。`tests/reclaim/run.sh` 与 `tests/reclaim-perf/gate.sh` 在串行窗口由 Lead 执行，本文件的 after 列对这两行记"由 Lead 执行（§7-16）"，不由本机填。

---

## §1 口径（before/after 必须逐列一致）

### 1.1 构建与工具链

| 项 | before | after |
|---|---|---|
| extc 二进制 | `./build-lead5/extc`，**md5 `9bef54886f49d174c01a0447be9ad3fc`**（本机复核一致） | `./build-rf/extc`，**md5 `7019342b3e5ccddbe67cbdc96eae2395`**（Lead m03911 GO 的权威构建；同源码二次构建因 `-g` 路径不同会换 md5，溯源以源码 md5 为准） |
| 源码 md5 | 未单独记录（FREEZE v5 之前的 build-lead5 二进制锚点） | `src/back/reclaim.c` **`7c2a9cedd7edd54de0a378d8a507dcbe`**（3801 行）、`src/back/codegen.c` **`d339382889b87c0dd18b357c69e2e779`**（16 017 行）、`tests/reclaim/driver.c` **`83e167d761705268dbda3f89895ea574`**（1441 行；Lead 更正，原记 1216 是 v4 口径）；跑前在 after 产物上自检宏（`QUOTA_MAX_MARK 1048576 / QUOTA_SHIFT_MARK 3 / QUOTA_MAX_WORDS 8192 / QUOTA_DEADLINE_US 800 / TIER 2 / NOCOUNT 1`）全部匹配 |
| CFLAGS | `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L` | 同左（**不可省 POSIX 宏**：否则 `CLOCK_THREAD_CPUTIME_ID` 不暴露，cpu 口径静默降级，见 MEASUREMENTS-GC-P0.md §7-12） |
| cc | gcc 15.2.0 | 同左 |
| 机器 | Intel Core Ultra 9 275HX / WSL2，nproc=24，MemTotal 20,478,916 kB | 同左 |
| 结果目录 | `tests/gc-p0/out/p0-before`（`all` + `g20` + `g1cpu` + `l10`，2026-11-09 15:45–15:49） | `tests/gc-p0/out/p0-after`（`all` + `g1cpu` + `g20` + `g18` + `l10` + `g20cmp`，2026-11-09 16:0x；`summary.txt` 由 `summarize.py` 生成） |

### 1.2 量具与串行纪律

- G18 内存：`/usr/bin/time -f '%M'`（timemax，交叉）+ `sample.py` 的 `/proc/<pid>/VmHWM`；G18 停顿：`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1` 的原始 stderr（`[histo]` 15 桶 / `[step]` / `[top]` / `[pause]` / `[work]`）。
- G20：`bench/gc/v2` 四形状，bench 档参数；`bench/gc/v2/shim.c` 现场编 `v2run`（`--pin 6`，fork+wait4 取 ms/rss_kb/minflt），stderr 用同一套 `pause_parse.parse_text`。
- **串行**：G18/G20 每次只跑一个进程；正式 before/after 在 Lead 协调的无并发窗口内跑；跑前确认无其他 agent 在跑程序。
- 每次报告：reps、median、max；**max 只作观察不作判据**（WSL2 时钟噪声地板 78–181 µs，见 MEASUREMENTS-GC-P0.md §1.3）。

### 1.3 命令（可复制）

```bash
# before（build-lead5 = 未改编译器）
EXTC=./build-lead5/extc OUT=tests/gc-p0/out/p0-before REPS=3 G20_REPS=5 tests/gc-p0/run.sh g18
EXTC=./build-lead5/extc OUT=tests/gc-p0/out/p0-before REPS=3 G20_REPS=5 tests/gc-p0/run.sh g20
EXTC=./build-lead5/extc OUT=tests/gc-p0/out/p0-before REPS=3 tests/gc-p0/run.sh all

# after（build-rf = P0/B 落地后的权威构建）
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after REPS=3 G20_REPS=5 tests/gc-p0/run.sh g18
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after REPS=3 G20_REPS=5 tests/gc-p0/run.sh g20
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after REPS=3 tests/gc-p0/run.sh all

# 跨构建对照
python3 tests/gc-p0/g18.py compare tests/gc-p0/out/p0-before/g18.json tests/gc-p0/out/p0-after/g18.json
python3 tests/gc-p0/g20.py compare tests/gc-p0/out/p0-before/g20.json tests/gc-p0/out/p0-after/g20.json

# 既有门禁（串行窗口内跑；注意两者的 before 语义不同）
EXTC=./build-rf/extc tests/reclaim-perf/gate.sh   # 吃 EXTC 环境变量（默认 build/extc），callgrind Ir 棘轮
tests/reclaim/run.sh                              # 直接编 src/back/reclaim.c 的发射文本 ⇒ 跟随当前源码，before 需旧源码/隔离基线，不能与本构建对照

# after 其余子命令（已实际执行，本文件数字出处）
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after REPS=3 tests/gc-p0/run.sh g1cpu
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after REPS=3 tests/gc-p0/run.sh l10
EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after tests/gc-p0/run.sh g20cmp

# 同轮交错 A/B（裁决 sizeratio 回归；奇数轮 A→B、偶数轮 B→A）
python3 tests/gc-p0/ab.py --extc-a ./build-lead5/extc --tag-a before \
  --extc-b ./build-rf/extc   --tag-b after  --out /tmp/empirics/ab-full --reps 9 --pin 6

# P 配额归因对照（A=B=build-rf，B 关掉 mark 配额与 deadline）
python3 tests/gc-p0/ab.py --extc-a ./build-rf/extc --tag-a base \
  --extc-b ./build-rf/extc --tag-b quota0 \
  --extra-b "-DEXTC_RC_QUOTA_MAX_MARK=0 -DEXTC_RC_QUOTA_DEADLINE_US=0" \
  --out /tmp/empirics/ab-quota0 --reps 9 --pin 6 --shapes promote sizeratio
```

---

## §2 判据

### 2.1 G18（内存为主，硬；停顿分通道；Lead m03358 + m03449 + m03564）

| 项 | 判据 | 出处 |
|---|---|---|
| 内存（硬） | `peak200 ≤ 3·live + 2·SLAB_SIZE`，live 从 `[work]` 的 `M_max` 反读（`M_max = 3·live`；SLAB_SIZE = 1 MiB ⇒ 26 MiB 当 live=8 MiB）；RSS 取 **timemax 的 ru_maxrss**（`/usr/bin/time -f '%M'`），**不是 VmHWM** | Lead m03358 |
| 规模无关（硬） | `peak200 ≤ peak60 × 1.05` | 同上 |
| 行为（硬） | stdout 逐字相同 `D n=<rounds> made=<rounds> s=0`、exit 0；这两项取 **run_pause 进程**（与 RSS 的 timemax 进程是两个进程两口径，报告必须写清） | 同上 |
| 停顿 ①（硬，仅纯常规形状） | **常规分配通道**（n ≤ 64 KiB）：新增回收切片 **≤ 5 µs**（与 G1 同口径；由 `[histo]` 的 >4 µs / >8 µs 桶给上下界）。**Lead m03564：在 pd_bigobj 这种混合通道形状上不可判**（每轮 64 个 16 B leaf + 1 个 8 MiB big，插桩不计 n）⇒ 本形状一律写 N/A，不据此判产品红；纯常规形状的 ① 见 G1（cyc/pc/pb）。 | Lead m03449 + m03564 |
| 停顿 ②（硬） | **大分配通道**（n > 64 KiB）：p99.9（n ≥ 1e3 时）+ over1ms=0，并给 P_step 闭式对照；另报 `deadline_hits`（`[quota]` 行）与 `[step] max` 两列 | Lead m03449 + m03564 |
| 观察（不判） | 单步 max（WSL2 噪声地板 78–181 µs，只观察）；`[top]` 前 3 条是否 `kind ∈ {mark,sweep}` 且 `seg > 0` 对应本次大分配；`pd_pointer_dense` 的峰值 / [histo] 尾部 / over1ms / top3 / deadline_hits | m03381 / m03449 / m03564 |

**P_step 闭式（Lead m03746 修正：相加式、紧上界、c_a=0；m03449/m03564 两版作废）**：

```
P_step ≤ t_sweep(W_s) + c_u·W_m + c_a，c_a = 0
W_s = 8·pm + max(1, ceil(游标 slab->nslots / 64))   swept words
     ≤ 8·16 + 64 = 192 words      （cap 8192 不可达：slotSize≥16 B ⇒ nslots<4096）
W_m = 8192 work units（deadline 每 8192 units 检查一次；mark 配额上限 min(n>>3, 1<<20)）
deadline = EXTC_RC_QUOTA_DEADLINE_US = 800 µs（从 [quota] deadline_us= 实读，不硬编码）
物理天花板 = deadline + 2 × batch_us（DL6：首个 advance 在时钟起点之前，保守多算一段）
```

- **q 的紧上界（m03746② + Lead m03891 裁决）**：slab 的 chunk 从 64 KiB 起倍增，但只要求 `chunk/slotSize >= SLOTS_MIN=1024`；`slotSize` 最小 16 B ⇒ `ss=16` 时 `chunk=64 KiB`、`cap ≈ (65536-SLAB_HDR)/16 < 4096`，`ss` 越大 cap 越小，huge block 走 `cap=1` 路径。所以**合法 `nslots < 4096` ⇒ `ceil(nslots/64) ≤ 64` ⇒ `W_s ≤ 192 words`**。m03564 的 `q=8192 ⇒ W_s=8320` 高估 43×，作废。`src/back/reclaim.c:1005` 注释里的「legal slab nslots<=131072 ⇒ ceil/64<=2048」是**过松的旧估计**（doc-audit 同判）；`g18.py` 的 `W_s_bounds` 保留 `src_comment_1005=2176` 仅作附注，**不进主判据**。`nslots` 仍无现成打印（`[top]`/`[quota]` 都不带），after 用 /tmp 插桩实测 `_q` 分布后回填（§7-10）——实测只为记录分布，不改变上界结论。
- 数值（`c_word=9.5~12 ns/word`、`c_unit=17.5 ns`）：`t_sweep = 1.8~2.3 µs`、`c_u·W_m = 143.4 µs` ⇒ **标称 145.2~145.7 µs**；`deadline 800 µs + 2×143.4 µs` ⇒ **物理天花板 ≈1086.7 µs**。存疑档 `W_s=2176` ⇒ `t_sweep=20.7~26.1 µs`、标称 164.0~169.5 µs。
- **`c_a` 置 0（m03746③a）**：初版 `C_A_NS=(0,18000)` 只是为把标称拟合进 Lead 参考区间 0.22–0.26 ms 的残差，不可伪证；若要保留必须拆成具名项并独立标定。
- **before/after 的闭式不可比（m03746③b）**：before（build-lead5）是旧语义 `W_m=min(n>>3,1<<20)` units、无 deadline；after 是 8192 units + deadline 800 µs。现场只比 RSS / stdout / `[step] max` / `[histo]`，**不得拿闭式数做 before/after 对照**。
- **`deadline_hits` 与 `[step] max`**：`[quota]` 终值行 `[quota] deadline_us=… max_mark=… shift=… checks=… hits=<total>` 里的 `hits` 是「配额循环被 deadline 提前截断」的次数；per-round 行 `[quota] round=<k> round_hits=<r> round_checks=<c>` 只给本轮值（m03775：`hits` 只取终值行 total，per-round 不得覆盖 —— 修 doc-audit N-2；`pause_parse.py --selftest` 断言 12/800/9）。after 的 G18 表与 `pd_pointer_dense` 都必须记录它与 `[step] max`。
- **≤5 µs 在 15 桶直方图上不可证（m03746④）**：`ch1_verdict` 在 4–8 µs 桶非空时只能给 N/A（跨阈值），**N/A 不得记 PASS**；报告写「只能给上界」。
- **`[pstep]` 无发射器（m03746⑤）**：`grep -r "pstep" src/` = 0 处 ⇒ `pstep_lines` 必空；要按 sweep/mark 通道拆单步耗需 `[top]` 带 `w_s`/`w_m`（尚无），**拿到前不得声称已按通道归因**。
- 旧 m03449 式（`228.5~269.8 µs`）与 m03564 初版（`W_s=8320`、222.4~261.2 µs、天花板 393.4 µs）均已作废，仅在 §7-6 作修订史。`extc_rc_stepWork = (long long)(pops + words + seg)`（`:2931`）仍是 `[work]` 自证口径。`g18.py` 的 `closed_form()` 已按新语义实现（`G18_SLAB_NSLOTS` 可注入 nslots；`--selftest` 断言 `W_s=192`、标称 140–150 µs、天花板 ≈1086.7 µs、跨桶 N/A）。

实现：`tests/gc-p0/g18.py json`（把 60/200 轮的 histo/step/top3 落 `g18.json`）+ `tests/gc-p0/g18.py compare`（before/after 差分：Δslices、ΔGCus、Δmax、histo 桶差、top3 逐行对照，并输出 `G18-mem` / `G18-pause-max<=1ms` / `G18-pause-top` 三项判定）。`summarize.py` 的 G18 段也会打印同一批列。

### 2.2 G20（B 的停顿收益，硬 + 不回归）

| 项 | 判据（Lead m03207） |
|---|---|
| promote | `[step] total_us` 较 before 降 **≥10%** |
| sizeratio | 降 **≥40%** |
| cycleratio | 不回归（after ≤ before × 1.03） |
| promote 单步 | `[step] max` 的 **median（跨 reps）≤ 60 µs** —— 单次 max 不作判据 |
| 四形状 | 墙钟中位、RSS 中位相对 before 在 **±3%** 内；stdout 一致 |

实现：`tests/gc-p0/g20.py run`（PAUSE=1 + STEP_CPU=1 注入子进程环境；`--pin 6`；写 `g20.json`）+ `g20.py compare`。

### 2.3 其余既有门禁（回归用，判据见 MEASUREMENTS-GC-P0.md §2.3）

G1（① over1ms=0、② >50 µs 占比 ≤1e-4、③ p99.9 ≤50 µs；cpu 口径）、G2（p99.9 ≤200 µs）、G3a/G3b、G4（正式：4×L 循环垃圾 RSS ≤ M_max+8 MiB；代理 pb/pc/cyc）、G8（T1/T2 ≤1.10×）、G9、G12（999）、G13（d0r=7）、G14（800k ≤1.02×对照 且 ≤4 MB）、G16（sweep 黑位回归）、G17（观察项）、`tests/reclaim/run.sh` 两档、`tests/reclaim-perf/gate.sh`（callgrind Ir 棘轮）。**本文件只汇总结果，不复述判据细节。**

### 2.4 判定总表（本轮 before → after）

| 门禁 | 判据摘要 | before | after | 结论 | 量具 |
|---|---|---|---|---|---|
| **G18** | `peak200 ≤ 3·live+2·SLAB` 且 `peak200 ≤ peak60×1.05`；stdout 逐字、exit 0；② p99.9（n≥1e3）+ over1ms=0；① 混合形状 N/A | 493,400 → 1,067,284 kB（**2.163× RED**） | 26,264 → 26,264 kB（**1.000×**；26,264 ≤ 26,624 kB）；② PASS；① N/A | **PASS** | timemax `%M` + run_pause stderr + `g18.py` |
| **G20** | promote `step_total` ≤−10%、sizeratio ≤−40%、cycleratio 不回归；promote `step_max` 中位 ≤60 µs；四形状 wall/rss ±3%；stdout 一致 | 基线（promote 18,406.9 µs / sizeratio 2,606.8 / cycleratio 413.7） | step_total 三条全 PASS（−20.2% / −56.4% / −10.8%）；promote step_max 中位 **63.8 µs**；promote wall **−11.2%**、rss **−5.7%**；**sizeratio +9.31%（交错）** | **RED** | `g20.py compare`（5 reps，pin6）+ `ab.py`（9 reps 交错） |
| 既有门禁（G1/G4/G8/G9/G12/G13/G14/G15/G16/G17/L10） | 见 MEASUREMENTS-GC-P0.md §2.3 | 见 §5 before 列 | 见 §5 after 列（G1② 仍 RED、G15/L10 预期红，其余全绿） | 见 §5 | `summarize.py` + `summary.txt` |

---

## §3 G18：大对象反复分配（60/200 轮 × 8 MiB）

形状 `tests/gc-p0/src/pd_bigobj.extc`：每轮 `new big`（1M 个可空指针字段 = 8 MiB）后丢弃，活集恒 1 个 big（8 MiB）。

### 3.1 before（build-lead5 md5 `9bef5488…`，**本机已实测**，正式 REPS=3，2026-11-09 15:45；OUT=`tests/gc-p0/out/p0-before`）

| 轮数 | peak VmHWM | wall | step_n | step_total_us | step_max_us | collects | 判定 |
|---|---|---|---|---|---|---|---|
| 60 | **493,400 kB** | 0.47 s | 121 | 1,547.3 | 55.6 | 1 | 与 Lead 锚点（493,400 kB）逐字一致 |
| 200 | **1,067,284 kB** | 0.78 s | 428 | 5,018.4 | 66.0 | 7 | 2.163× ⇒ 无界（≈130 个 8 MiB big 未回收）；Lead 锚点 1,067,288 kB，差 4 kB（0.0004%） |

before 的 `[histo]`（15 桶，200 轮）：`b1us=22 b2us=8 b4us=5 b8us=0 b16us=348 b32us=41 b64us=3 b128us=1 b256us=0 b512us=0 b1ms=0 b4ms=0 b16ms=0 b64ms=0 bge64ms=0`（60 轮：`b1us=1 b16us=113 b32us=5 b64us=2`，其余 0）。两轮 n=121/428 均 <1e3 ⇒ 中位/p99.9 不可判。

before 的 `[top]` 前 3（200 轮）：
```
[top] rank=1 kind=mark why=advance dur_us=66.0 cpu_us=66.0 seg=8192 pops=1 coll=3 step=199 thr=23068672 mmax=25165824 scale=4 pressure=1
[top] rank=2 kind=mark why=advance dur_us=47.3 cpu_us=47.3 seg=8192 pops=1 coll=3 step=210 thr=23068672 mmax=25165824 scale=4 pressure=1
[top] rank=3 kind=mark why=advance dur_us=37.0 cpu_us=37.0 seg=8192 pops=1 coll=1 step=74 thr=1048576 mmax=0 scale=1 pressure=0
```

### 3.2 after（build-rf md5 `7019342b…`，**本机实测**，REPS=3，2026-11-09 16:0x；OUT=`tests/gc-p0/out/p0-after`）

| 轮数 | peak ru_maxrss | wall | step_n | Δslices(vs before) | step_total_us | ΔGCus | step_max_us | deadline_hits | collects | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|
| 60 | **26,264 kB** | 0.08 s | 7,734 | +7,613 | 48,320.7 | +46,773.4 | 216.9（before 55.6） | 0 | 59 | 内存项 26,264 ≤ 26,624 kB PASS；规模项 PASS |
| 200 | **26,264 kB** | 0.26 s | 26,116 | +25,688 | 165,611.9 | +160,593.5 | 185.3（before 66.0） | **1** | 198 | **G18 PASS**（内存项 + 26,264 ≤ 26,264×1.05 + stdout/exit） |

- live 反读：`[work] M_max=25,165,872 B ⇒ live=8,388,624 B=8,192 kB`（`g18.json` 的 `work_mmax/live_bytes`）；上界 = 3·live + 2·SLAB = **26,624 kB**。after 两档 ru_maxrss 都 26,264 kB（余量 360 kB），**且 60 轮与 200 轮完全相同（1.000×）** ⇒ 大对象分配不再累积（before 2.163×）。
- `[histo]`（15 桶）：60 轮 `b1us=229 b2us=21 b4us=1747 b8us=3356 b16us=2309 b32us=52 b64us=16 b128us=3 b256us=1`，其余 0（n=7,734，中位 8 µs、p99.9=64 µs）；200 轮 `b1us=777 b2us=64 b4us=5490 b8us=11866 b16us=7700 b32us=161 b64us=45 b128us=8 b256us=5`，其余 0（n=26,116，中位 8 µs、p99.9=64 µs）。**b1ms 及以上恒 0 ⇒ over1ms=0**。
- `[top]` 前 3：60 轮 216.9 / 103.3 / 84.7 µs；200 轮 185.3 / 176.8 / 170.0 µs —— 全部 `kind=mark why=advance words=0 seg=8192 pops=1` ⇒ 都是**大对象续扫段**（8 MiB = 128 段 × 8192 words），对应本次大分配 ⇒ 软约束「top 含 mark/sweep 且 seg>0」PASS。
- `[quota]` 终值行：60 轮 `deadline_us=800 max_mark=1048576 shift=3 checks=244,736 hits=0`；200 轮 `checks=830,573 hits=1`（per-round 行 `round_hits=0 round_checks=4224`）。`hits` 只在 200 轮出现 1 次、不随轮数线性增长 ⇒ **deadline 不构成常态停顿源**。
- **代价必须同表读**：step_n 428→26,116（×61）、step_total 5,018.4→165,611.9 µs（×33）、step_max 66.0→185.3 µs。这是 P（按字节配额）把"一次扫一大段"拆成"每次分配一小片"的直接结果：GC 总工作量没降，只是被摊到更多步、每步更长（仍 **over1ms=0**）。**不能把 G18 的 PASS 说成"停顿也变好"**。
- ① 常规通道：**N/A（混合形状不可判，Lead m03564）**；参考界：200 轮 7,919 步 >8 µs、7,985 步 >4 µs、6,331 步 ≤4 µs（n=26,116）⇒ 5 µs 落在 4–8 µs 之间、**15 桶直方图没有 5 µs 这个切点**，只能给上界；**N/A ≠ PASS**（m03746④）。
- ② 大分配通道：**PASS**（over1ms=0；n≥1e3 ⇒ p99.9=64 µs；P_step 闭式标称 145.2–145.7 µs、天花板 1086.7 µs，实测 max 216.9 / 185.3 µs 落在两者之间）。
- 填表口径：RSS 取 timemax 的 `ru_maxrss`（`timemax.txt`：`pd_bigobj 60 26264 0.08` / `pd_bigobj 200 26264 0.26`），stdout/exit 取 run_pause 进程（两进程两口径）；before 同列见 §3.1。Δ 计算：Δslices = step_n − before step_n；ΔGCus = step_total_us − before step_total_us。
- 与 Lead m03911 转述的 runtime-facts 预演数（60 轮 26,644–26,648 kB、200 轮 26,636–26,648 kB、hits 0–1、max 98.9–190.3 µs）相比：本机 ru_maxrss **低 380–384 kB**（26,264 vs 26,644–26,648，同量级、同结论），max 本机 216.9/185.3 µs（预演 98.9–190.3），deadline_hits 一致（0/1）。差异归因于构建/机器噪声，不影响 PASS。


### 3.3 pd_pointer_dense 观察（大分配通道 × 指针稠密；**不参与 G18 判定**，Lead m03381）

形状 `tests/gc-p0/src/pd_pointer_dense.extc`：每轮 `new` 一个 8 MiB dense（`a: [1048576]?mut ref leaf`），1M 个槽**全部填非空指针**（每槽 `new leaf` 后写入），dense 存进 `w.cur` 保持到下一轮 ⇒ 下一轮 collect 时前一个 dense 仍可达，mark 必须扫 1M 个非空指针槽；活集 ≈ dense 8 MiB + 1M leaf 8 MiB ≈ 16 MiB；200 轮。stdout 期望 `PD n=200 made=200 s=1048575`。

| 构建 | 轮数 | ru_maxrss | wall | step_n | step_max | over1ms | p99.9 桶 | >8 µs 步 | collects | deadline_hits | [top] rank1 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| build-lead5（before） | 20 | 140,308 kB | 0.22 s | — | — | — | — | — | — | n/a（无 deadline 机制） | — |
| build-lead5（before，官方 before 全套） | 200 | **140,312 kB** | 1.89 s | 866,748 | 223.5 µs（另两次 420.9 / 485.9） | **0** | p99.9=32.0 µs、中位 1.0 µs | 25,069（2.9%；另两份 25,164 / 25,369） | 101 | n/a（before 无 `[quota]` 行） | `kind=mark why=advance words=0 seg=0 pops=256 dur=223.5 µs`（另两次 420.9 / 485.9） |
| build-rf（旧档，QUOTA_MAX_MARK=8192） | 200 | 140,504 kB | 2.15 s | 866,508 | 341.3 µs | **0** | b32us | 24,712（2.9%） | 101 | n/a（旧档） | `kind=mark why=advance seg=8192 pops=1 dur=341.3 µs` |
| **after（build-rf 权威，本机实测）** | 200 | **111,900 kB** | 1.96 s | 737,759 | 424.0 µs（另 277.6 / 203.6） | **0** | p99.9=64 µs、中位 1.0 µs | 32,805（4.4%） | 128 | **0**（`[quota] checks=0 hits=0`） | `kind=mark why=advance seg=8192 pops=1 dur=424.0 µs` |

- **after vs before（dense）**：ru_maxrss **140,312 → 111,900 kB（−20.3%）**、step_n 866,748→737,759、over1ms 0→0、>8 µs 25,069→32,805（2.9%→4.4%）、step_max 223.5→424.0 µs（observation only）、>256 µs 9→2。峰值下降来自 P 把大对象分配与回收压住（活集估 16 MiB 的 6.8× 而非 8.8×），但**慢尾没有消失**。
- **dense 的瓶颈不在 P / deadline 通道（Lead m03911 要求写清）**：after 的 `[quota] checks=0、hits=0、deadline_hits=0` ⇒ 配额循环根本没进入（该形状的分配多数 <32 KiB，按 `n>>3` 得到的 quota 小于检查粒度）；`[top]` rank1 是 `kind=mark why=advance pops=1 seg=8192` 的**大对象续扫段**，与 before 同类 ⇒ 慢尾来自既有的 mark 分段/描述符扫描，与 P 的配额通道无关。故该用例只作观察，不因 424 µs 判产品红（over1ms=0 是判据线）。
- 20 轮与 200 轮 ru_maxrss 几乎相同（140,308 vs 140,312 kB）⇒ 峰值有界；但峰值 ≈ 8.8× 的 16 MiB 活集（M_max 自学习到 139,201,488 B）。after 的对应值为 111,900 kB / M_max 127,492,464 B（≈6.8×）。
- `[histo]` 汇总（866,748 步）：`<=4 µs` 833,695 步（96.2%）、`>4 µs` 33,053、`>8 µs` 25,164（2.9%）、`>256 µs` 9、`>1 ms` **0**。
- 按 m03381 的观察项：**over1ms=0 ⇒ 不 RED**；p99.9 落在 `b32us` 桶（≤32 µs）。四次独立观测的 over1ms 全为 0（`b1ms` 及以上桶恒 0），`>256 µs` 有时 0–9 步。
- **① 在本用例上不可判（Lead m03564 降级；m03746④ 补充）**：>8 µs 的步占比 2.9%、步数 ≈ 4,334/轮 ⇒ 绝大多数步由 8B `leaf` 分配触发，但 `[histo]` 不按分配 n 分桶 ⇒ **不能把 slice 归因到通道**。按 m03564 统一写成「① 在混合形状上不可判，判据只对纯常规形状有效；大分配通道走 ②」，**不据此判产品红**；且 15 桶直方图上 **≤5 µs 不可证（只能给上界）、N/A ≠ PASS**；① 的纯常规样例见 G1（cyc/pc/pb）。按分配 n 分桶插桩是实现精确归因的前置条件（§7-8）。
- **seg 非 0 的步比例（Lead m03564 要求，before 构建实测）**：在 build-lead5 生成的 `pd_pointer_dense.c` 上插桩（只改 /tmp 副本，`/tmp/empirics/segcount/patch_seg.py` 给 markStep 的 `[step]` 记账处加计数器）跑 200 轮：mark 步 **584,267** 中 **25,600 步在续扫大对象（seg>0，占 mark 步 4.38%；占全部 `[step]` 866,748 步的 2.95%）**，累计 seg words **209,715,200 = 25,600 × 8192**，恰好等于 200 轮 × 1,048,576 个指针槽 ⇒ **每轮 1M 指针的 dense 对象被切成 128 段、每段一步地续扫**（`SCAN_CHUNK=8192`）。这说明大对象确实是分步续扫的、单步有界；after 构建上会用同一脚本重测 `_q`/`seg` 并回填。
- `[top]` rank1 是 `pops=256、words=0、seg=0、223–486 µs` 的 mark 步（四次观测）⇒ 冷访存逐 pop 成本 **≈0.9–1.9 µs/pop**，比闭式假设的 `c_unit≈17.5 ns/unit` 高约两个数量级 ⇒ 闭式的单位成本假设对**指针稠密对象**不成立。按 m03746 紧上界闭式（145.2~145.7 µs）比对：实测 max **超紧上界 1.5–3.3×**，但仍在 deadline 层天花板 1086.7 µs 内、over1ms 恒 0 ⇒ **闭式对指针稠密对象系统性低估**，下一轮必须按 n 分桶 + 冷/热 pop 分别标定（E29）。
- **形状偏差（必须注明）**：extC 逃逸检查不允许把"上一轮建好的 1M leaf 集"持久挂到函数级根（见下），因此每轮都重新分配 1M leaf ⇒ mark 的实际工作量比"指针指向长命对象"的设定更大，是**更保守的上界测试**。逃逸规则实测报错（原计划三条路径都被拒）：
  1. 循环体内 `w.leaves = p` 直接存 depth-2 局部 ref 进 depth-1 world ⇒ `error: this reference would hold a reference to a local variable that dies first (borrowed from depth 2, but this can only hold up to depth 1)`；
  2. 先放进局部 `q` 再 `holdDense(ref w, b)` ⇒ `error: argument 2 of holdDense points into a deeper scope (depth 2) than the arena this call may store it in (depth 1)`；
  3. helper 里 `(*b).a[i] = q`（q 为参数派生） ⇒ `error: cannot store a borrowed value into something that outlives this call`。
  可行写法只有 `(*b).a[i] = new leaf`（`new` 在本函数 home arena，可逃逸）。

---

## §4 G20：四形状的停顿收益（bench/gc/v2，bench 档）

### 4.1 before（build-lead5 md5 `9bef5488…`，**本机已实测**，正式 REPS=5、pin=6、PAUSE=1+STEP_CPU=1，OUT=`tests/gc-p0/out/p0-before`）

| 形状 | wall_med (ms) | rss_med (kB) | step_total (µs) | step_max (µs) | collects |
|---|---|---|---|---|---|
| promote | 48.323 | 6,684 | **18,406.9** | 89.5（worst 176.3） | 83 |
| sizeratio | 24.148 | 3,436 | **2,606.8** | 32.4（worst 64.1） | 171 |
| chase | 149.170 | 73,884 | 5,211.4 | 103.4（worst 105.7） | 2 |
| cycleratio | 13.035 | 3,228 | **413.7** | 6.9（worst 16.0） | 42 |

与原型 before 对照（Lead m03207）：promote 17,752 µs、sizeratio 2,581 µs、cycleratio 471.8 µs ⇒ 本机 promote +3.7%、sizeratio +1.0%、cycleratio −12.3%（同量级；单次 max 有 >2× 离散，不作判据）。

### 4.2 after（官方 compare：build-rf md5 `7019342b…`，REPS=5、pin=6、PAUSE=1+STEP_CPU=1，**本机实测**，OUT=`tests/gc-p0/out/p0-after`）

| 形状 | wall_med (ms) | Δwall | rss_med (kB) | Δrss | step_total (µs) | Δstep_total | step_max (µs) | collects |
|---|---|---|---|---|---|---|---|---|
| promote | 42.920 | **−11.2%** | 6,304 | **−5.7%** | **14,692.6** | **−20.2%**（判据 ≤−10% **PASS**） | 63.8（worst 181.6） | 92 |
| sizeratio | 24.639 | **+2.0%** | 3,376 | −1.7% | **1,136.5** | **−56.4%**（判据 ≤−40% **PASS**） | 32.0（worst 56.4） | 171 |
| chase | 146.47 | −1.8% | 74,080 | +0.3% | 5,046.5 | −3.2% | 103.7（worst 513） | 2 |
| cycleratio | 13.150 | +0.9% | 3,236 | +0.2% | **369.2** | **−10.8%**（判据 ≤+3% 不回归 **PASS**） | 6.8（worst 13.4） | 42 |

逐条判据（Lead m03207）：

| 判据 | 结果 | 判定 |
|---|---|---|
| promote `step_total` 降 ≥10% | 18,406.9 → 14,692.6 µs（−20.2%） | **PASS** |
| sizeratio `step_total` 降 ≥40% | 2,606.8 → 1,136.5 µs（−56.4%） | **PASS** |
| cycleratio 不回归（≤ before×1.03） | 413.7 → 369.2 µs（−10.8%） | **PASS** |
| promote `step_max` 跨 reps 中位 ≤60 µs | **63.8 µs**（reps: 37.6 / 43.7 / 75.4 / 63.8 / 181.6） | **RED**（观察口径，单次 max 不作判据；m03207 只对 median 判） |
| 四形状 wall 相对 before ±3% | promote **−11.2%**、sizeratio +2.0%、chase −1.8%、cycleratio +0.9% | **RED**（promote 越界，方向是改善也照判；sizeratio 顺序跑 +2.0% 未超阈，**但同轮交错 A/B 是 +9.31% ⇒ §4.3**） |
| 四形状 rss 相对 before ±3% | promote **−5.7%**、sizeratio −1.7%、chase +0.3%、cycleratio +0.2% | **RED**（promote 越界，改善方向） |
| stdout 一致 | 四形状 `stdout_sha256` 全等 | **PASS** |

- **G20 汇总裁定：RED**（1 条硬判据红 = promote step_max 中位 63.8 > 60；另有 2 条 ±3% 对称判据因改善方向被判红，如实记录，不解释成失败）。
- **量具**：`g20.py compare`（`tests/gc-p0/out/p0-{before,after}/g20.json`）：`bench/gc/v2/shim.c` 现场编 `v2run --pin 6`（fork+wait4 取 wall/rss/minflt），子进程注入 `EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，stderr 用 `pause_parse.parse_text` 解析；每形状 5 reps 取中位，`stdout_sha256` 比对一致性。cmd：`EXTC=./build-rf/extc OUT=tests/gc-p0/out/p0-after tests/gc-p0/run.sh g20cmp`。
- **官方 compare 的 sizeratio +2.0% 是顺序跑的低估**：5 reps 的顺序排布把 before 的首个 rep 放在冷启动位置，且 before/after 之间隔着整段套件。**同轮交错 A/B（§4.3）给出 +9.31%**，与 runtime-facts 报的 +10.34% 同源 ⇒ 以交错值为准。
- 原型 after（Lead m03207，供对照，**不是本机数字**）：promote 15,528 µs / max 57.5；sizeratio 1,247 / 9.5；cycleratio 432 / 16.9 ⇒ 本机 promote 14,692.6（−5.4%）、sizeratio 1,136.5（−8.9%）、cycleratio 369.2（−14.5%），同方向且更优；**promote max 63.8 vs 原型 57.5 仍越 60 µs 线**，属构建间噪声 + 本机 WSL2 噪声（§7-4），不作为分歧。

### 4.3 同轮交错 A/B（裁决 sizeratio 回归；**本机实测**，9 reps、pin=6，A=build-lead5 / B=build-rf，奇偶轮交换顺序）

| 形状 | wall A→B (ms) | Δwall | Ir A→B | ΔIr | Δrss | Δstep_total | 判定（abs(Δwall)≤3%） |
|---|---|---|---|---|---|---|
| promote | 46.148 → 42.984 | **−6.86%** | 804,669,727 → 820,354,900 | +1.95% | −5.69% | −16.3% | PASS（改善） |
| sizeratio | 22.372 → 24.455 | **+9.31%** | 229,478,466 → 243,258,299 | **+6.00%** | +0.12% | −56.9% | **RED（回归确认）** |
| chase | 150.653 → 148.381 | −1.51% | 300,607,799 → 310,786,400 | +3.39% | +0.27% | −2.5% | PASS |
| cycleratio | 12.797 → 12.950 | +1.20% | 264,352,279 → 275,599,338 | +4.25% | +0.25% | −8.2% | PASS |

（`ab.py` 对 promote 也按 ±3% 对称判据标 RED，方向是改善 ⇒ 本表把它记为 PASS-改善；sizeratio 的 +9.31% 越阈且方向是变慢 ⇒ 真回归。over1ms 在两构建的四形状全为 0；`[quota] hits` A 侧无 `[quota]` 行（before 无 deadline 机制）、B 侧 0。）

**sizeratio 回归归因（能定到的部分）**：

1. **没有可关 B（惰性 freelist）的编译宏**：`grep -r "EXTC_RC_LAZY_FREE\|LAZYFREE" src/` = 0 处。B 是 `extc_rc_slab` 结构里的 `freelist` 字段（`src/back/reclaim.c:238`、`:253`、`:262` 注释；alloc 热路径 `:1424-1426` 弹链、`freeSlot` `:1597-1598` 头插），**没有开关可做"只关 B"的对照** ⇒ Lead 给的 `-DEXTC_RC_LAZY_FREE=0` 路线不可行，如实记录。
2. **生成物的每分配热路径差异（可逐行指认）**：before 的 alloc 里是 `if (r->sweeping) ... else extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET * (pressure?MUL:1));` + `if (r->marking) { if (...) extc_rc_markAdvance(r); ... }`；after 换成 sweep 侧 `_q = ceil(cursor_slab->nslots/64)`（封顶 `QUOTA_MAX_WORDS`）后 `_b += _q; extc_rc_sweepStep(r,_b);`，以及 mark 侧 `_quota = n>>3`（封顶 `QUOTA_MAX_MARK`）的 `for(;;) ... while (_used < _quota ...)` 配额循环；slab 满条件改成 `if (!s || (s->bump >= s->nslots && s->nfree == 0))`，freelist 弹槽检查移到**每次分配**（before 只在 sweep 分支里）。sizeratio 每轮 10 次分配、2,000,000 轮 ⇒ 这些每分配固定成本被放大 2M 次。
3. **行级 Ir 差分（callgrind，`-g` 重编，bench 参数 2000000 20261013）**：满条件行 **+5.11M**（B 14.68M vs A 9.56M）、新增 `if (s->freelist >= 0)` **+6.00M**、弹链体 9.41M→8.94M、mark 位扫循环（while(h)/idx/ctz）约 −0.5M、`sweepStep` 自身 22.61M→2.39M（−20.2M，after 新增 `[sweepelide] bulk_reset=96 bulk_slots=14,042`）；**净 Ir +13.8M = +6.0%**。
4. **不是访存变差**：`--cache-sim=yes` 下 **D1mr 428,109→401,951（−6.1%）、DLmr 1,481→1,227（−17.2%）**，而 Dr 45.6M→52.3M（+14.6%）、Dw 37.1M→39.8M（+7.4%）⇒ 指令与访存**条数**上升、miss **变少**；ns/Ir 0.0976→0.1013（+3.8%）⇒ 单位指令更慢，指向前端/分支/调度（callgrind 无 branch sim，未测）。
5. **与 P 的配额记账同源（对照实验）**：A=B=build-rf，只给 B 加 `-DEXTC_RC_QUOTA_MAX_MARK=0 -DEXTC_RC_QUOTA_DEADLINE_US=0`（9 reps、pin6）⇒ sizeratio wall **24.88→23.05 ms（−7.34%）**、Ir 245,599,926（+0.96%）、step_max 中位 59.6→13.4 µs；promote 仅 −1.80%。即**把每个分配的 mark 配额记账关掉能收回约 7.3% 墙钟**（但会改变 GC 节奏：mark_n 170→308、bulk_reset 96→3、`[step]` total 1,182→1,088 µs，GC 总时间量级不变）。sizeratio 的 `[quota]` 是 `checks=0`（配额 <4096 时不读时钟）⇒ **不是 deadline 通道**，就是每分配记账成本。
6. **结论**：sizeratio 的 +9.31% 中约 **+7.3 个百分点可归因到 P 的每分配配额记账**（外加 B 的 freelist 探针 +6.00M Ir），剩余 ≈ **+2 个百分点未归因**（前端/分支/微架构）。**不得写成"完全归因"**。产品含义：在"每分配只有 ~12 ns 固定预算"的形状上，P/B 的每分配记账是净成本；G20 的 step_total 收益（−56.4%）与这笔墙钟成本同时存在，判据必须分开看。

---

## §5 既有门禁回归（before / after 都为本机实测）

before = build-lead5 md5 `9bef5488…`，OUT=`tests/gc-p0/out/p0-before`（`all` + `g1cpu` + `l10`，REPS=3）；
after = build-rf md5 `7019342b…`，OUT=`tests/gc-p0/out/p0-after`（`all` + `g1cpu` + `g20` + `g18` + `l10`，REPS=3）。
数字出处：两侧的 `summary.txt`（`summarize.py` 生成）与 `g18.json` / `g20.json` / `timemax.txt` / `pauses.jsonl`。

| 门禁 | before（build-lead5） | after（build-rf） | 判定 |
|---|---|---|---|
| G1① over1ms=0（cpu） | **PASS**（cyc/pc/pb/pd 全 0） | **PASS**（四用例 `[histo]` 的 b1ms 及以上桶全 0；pd 的 **wall** 口径有 1 步 2,196.8 µs，含抢占，cpu 口径 0） | 不回归 |
| G1② >50 µs 占比 ≤1e-4（cpu） | **RED**（汇总；pb 下界 1.30e-4 > 1e-4；cyc/pc 跨阈 N/A；pd n<1e5 N/A） | **RED**（pb 下界 3.31e-4、pd 下界 5.62e-4；cyc 区间 5.18e-5–2.15e-4 N/A；pc 区间 4.86e-5–1.46e-4 N/A） | 仍红、未恶化到新量级，但**未转绿** |
| G1③ p99.9 ≤50 µs（cpu） | **部分 PASS + pd N/A**（cyc/pc/pb ≤32 µs；pd 桶界 32–64 µs 不可判） | **部分 PASS + 部分 N/A**（cyc 16 µs、pc 16 µs PASS；pb 64 µs、pd 64 µs 落 32–64 桶界 ⇒ N/A） | 与 before 同态 |
| G2（p99.9 ≤200 µs） | n/a（不在 gc-p0 套件；见 tests/reclaim-perf） | n/a | 未覆盖 |
| G3a / G3b | n/a（同上） | n/a | 未覆盖 |
| G4 正式（4×L 注入） | **PASS**（A 9,144 ≤ 15,223 kB；末 6 轮差 0.00%；A<B<C） | **PASS**（A 9,104 / B 41,820 / C 82,956 kB；A 9,104 ≤ 15,223；末 8 轮恒 9,104、差 0.00%；A<B<C；M_max A=7,200,144 B；pressure_steps 98/98/0） | 通过 |
| G4 代理（pb/pc/cyc） | pb 20.49 B/node、pc 2,992 kB、cyc 20M/80M 1.023× | pb 20.49 B/node（2,000,408 kB / 2.90 s）、pc 2,992 kB、cyc 4,984/4,988 = 1.001× | 通过 |
| G8（T1/T2 ≤1.10×） | **PASS** 0.494×（1,496/3,032 kB） | **PASS 0.515×**（1,564/3,036 kB）；ASAN 两侧 rc=0 无报告 | 通过 |
| G9 | **PASS**（3/3，stdout 全文一致，exit 0） | **PASS**（3/3：live0=1 freed=0 live1=1 / tag=777 / got=20 steps=5，exit 0；ASAN rc=0） | 通过 |
| G12（999）/ G13（d0r=7） | G12 强制 T3 PASS（999）；G13 默认 T2 PASS（d0r=7）+ T3 PASS | 同（G12 强制 T3 'alias check b.v=999'、默认 T2 亦 999；G13 默认 **T2** 'd0r=7'、强制 T3 'd0r=7'） | 通过 |
| G14（1.02×对照 且 ≤4 MB） | **PASS**（50k 2,652 / 100k 3,036 / 800k 3,036 / ctl 3,036 kB；对照比 1.000×；记账 2 次） | **PASS**（50k 2,656 / 100k 3,040 / 800k 3,040 / ctl 3,040 kB；对照比 1.000×；≤4,096 kB；记账 2 次；ratio 800k/50k=1.145× 仅观测） | 通过 |
| G15（XFAIL L10-a） | 14.985×（12,664→189,776 kB） | **14.677×**（12,672→185,988 kB）⇒ XFAIL 预期 RED | 预期红（已知限制未修） |
| G16 | **PASS**（3/3，exit 0、无 timeout、cnt/sum 正确、wall 0.044 s） | **PASS**（3/3，exit 0、无 timeout、cnt/sum 正确、wall 0.048 s） | 通过 |
| G17（观察） | A 22,240 / D 25,696 / B 70,240 / C 70,240 kB；A 有 `[mmax] M_max=397056 floor=1048576; using floor` | A 27,812 / D 25,700 / B 64,100 / C 64,100 kB（thr_max 17,457,748/16,251,908/66,583,556/67,239,424；ps 8,886/4,714/0/0；A 有同类 `[mmax] M_max=559,776 floor=1048576; using floor`） | 观察项：after 仍是 A<B≈C，但 **A 的峰值比 before 高 25%（27.8 vs 22.2 MB）**，同时 D 持平、B/C 略降——顺序反转属自学习态/构建差异，不判 |
| L10-b / L10-c（XFAIL） | 6.67×（2,644→17,624 kB）/ 14.07×（6,664→93,792 kB） | **6.65×**（2,648→17,620 kB）/ **14.08×**（6,664→93,796 kB） | 预期红（已知限制未修，数字不变） |
| `tests/reclaim/run.sh` 两档 | 未由本机执行 | 未由本机执行 | **由 Lead 在串行窗口执行**（§7-16）；本文件不填 |
| `tests/reclaim-perf/gate.sh`（Ir 棘轮） | 未由本机执行 | 未由本机执行 | 同上（§7-16） |

- **P0-3 权衡形状（观察）**：10×400k 环峰值 **22,564 kB = 22.0 MB**（after；before 17,908 kB）⇒ 内存升 26%，是 P0-3 已知代价（releaseOld 预算耗尽改靠 sweep），不作判据。
- before 的旁证：`[work] pressure=1` 合计 11（pd_bigobj 1 / pd_pointer_dense 1 / cyc 1 / pc 0 / pb 1 / pd 1）；G4 段 `[work]` 取到的 M_max 最大正值 = 1,360,583,952 B（pb 100M 档）。
- after 的旁证：`[work] pressure=1` 合计 **15**（pd_bigobj 1 / pd_pointer_dense 1 / pb 1 / pd 1 / 其余 0，按用例汇总）；G4 段 M_max 最大正值仍 = 1,360,583,952 B（该值单位/口径待核，见 §7-14）。
- G1 的空转基线与噪声地板见 MEASUREMENTS-GC-P0.md §1.3（本文件不重复；after 的 noise 在 `p0-after/noise.txt`）。

## §6 与原型数字的差异

1. **G20 before 的量级对齐**：原型 promote step_total 17,752 → 本机正式 before 18,406.9（+3.7%）；sizeratio 2,581 → 2,606.8（+1.0%）；cycleratio 471.8 → 413.7（−12.3%）——同量级；差异来源是负载/构建（原型构建与 build-lead5 差在 P0 半落地），不影响 before/after 同构建对照。
2. **G20 after 的量级对齐**：原型 promote 15,528 µs → 本机 14,692.6（−5.4%）；sizeratio 1,247 → 1,136.5（−8.9%）；cycleratio 432 → 369.2（−14.5%）——同方向且本机略优。**唯一方向性差异**：原型 promote max 57.5 µs（<60 线）vs 本机中位 63.8 µs（>60 线）⇒ 该判据在两构建间不稳，建议下一轮把 promote 单步判据改成 `[histo]` 的 p99.9 或 >64 µs 占比（与 G1 同口径），而不是 `[step] max` 的中位。
3. **promote 单步 max 的跨构建离散**：原型 160.4 µs vs 本机 before median 89.5 / worst 176.3 µs ⇒ 单次 max 在两构建都跨 2×，再次说明 max 只能观察、不能作判据。
4. **G18 的 P 代价（Δslices / ΔGCus）与 B 的收益（G20 step_total 降）是两个方向**，必须分开列，不能相抵成"净收益"：G18 after 的 GC 总时长从 5,018.4 → 165,611.9 µs（×33），同时 G20 的 promote/sizeratio step_total 大幅下降（−20.2% / −56.4%）——两者形状不同、口径不同，绝不能相加。
5. **sizeratio 的墙钟回归（+9.31% 交错）不在原型报告的判据范围内**（原型只报了 step_total 收益）；本轮新增的同轮交错 A/B 把它暴露出来（§4.3）。

---

## §7 局限与存疑

1. **串行纪律的实际执行**：before（15:45–15:49）与 after（16:0x）都在 Lead 指定的串行窗口内跑（其它 agent 停手）；after 跑前在产物上自检 6 个宏（§1.1）。**after 的 G20 官方 compare 用 5 reps 顺序跑；同轮交错 A/B 是另一次独立测量（§4.3）**，两者不可混用同一个 Δ 值。
2. **G18 停顿只有 1 rep/轮数**（run.sh 的 g18 用 rep 1/2 分别跑 60/200）：`[step] max` 是单次观测，只能作观察项；软约束 max ≤1 ms 因此也只作"是否出现毫秒级单步"的粗判。
3. **G4 段的 M_max 字段会被 G18 的 pd_bigobj 污染**：`summarize.py` 的 G4 段从 `[work]` 取 m_max，G18 冒烟里取到 `m_max=0`（pd_bigobj 的 [work] 行没有 m_max）。正式跑时应以 stress.py 与 cyc 的 [work] 为准（待修/待核）。
4. **WSL2 时钟噪声**：单次 max 不可作判据；cpu 口径依赖 `-D_POSIX_C_SOURCE=200809L`，漏掉会静默降级成 0（已知缺陷，见 MEASUREMENTS-GC-P0.md §7-12）。
5. **并行纪律**：G18/G20 的墙钟与 step 时间对并发负载敏感；若正式 before/after 之间其他 agent 跑过程序，数字作废重跑。

6. **P_step 闭式三段修订史（当前生效 = m03746 版，见 §2.1）**：m03449 旧式 `W_s=8·pm+min(n>>6,8192)`、`W_m=min(n>>3,1<<20)` ⇒ 228.5–269.8 µs；m03564 相加式但 `q=8192` 未收紧 ⇒ 222.4~261.2 µs、天花板 393.4 µs（`q` 高估 43×）；m03746 紧上界 `q≤64`、`W_s≤192`、`c_a=0`、deadline 实读 800 µs ⇒ **标称 145.2~145.7 µs、天花板 ≈1086.7 µs**。**不确定性在 `nslots` 与源码 `:1005` 注释（`ceil/64≤2048`）的冲突**，闭式**只作量级对照，不作硬判据的上界证明**。
7. **build-rf 在正式 GO 前一直在动（历史，已解决）**：15:47 曾观测到 `./build-rf/extc` md5 `bffd9c9fafc2ff47e82150b0f27f7d5d`（产物已含 `QUOTA_MAX_MARK (1<<20)` 与 `DEADLINE_US 800`，而更早的 850e5d29… 还是 8192/250 旧档）。**正式 after 用的是 Lead m03911 GO 的构建 md5 `7019342b…`**（源码 `reclaim.c 7c2a9ced…`），跑前自检产物宏 `QUOTA_MAX_MARK 1048576 / QUOTA_SHIFT_MARK 3 / QUOTA_MAX_WORDS 8192 / QUOTA_DEADLINE_US 800 / RELEASE_BUDGET 2048 / TIER 2 / NOCOUNT 1` 全部匹配。§3.3 里那行"build-rf（旧档，QUOTA_MAX_MARK=8192）"保留为历史参照，**不是 after**。
8. **① 常规通道在混合形状上不可判（Lead m03564 裁决，不是产品红）**：`pd_bigobj`/`pd_pointer_dense` 每轮既有 64 个 16 B leaf、又有 1 个 8 MiB big，`[histo]` 不按分配 n 分桶 ⇒ 无法把 >8 µs 的步归因到哪条通道。报告统一写「① 在混合形状上不可判，判据只对纯常规形状有效；大分配通道走 ②」，① 的纯常规样例看 G1（cyc/pc/pb）。要精确归因必须实现按 n 分桶的 `[histo]`（未实现）。
9. **指针稠密冷 pop 成本远超闭式假设**：`[top] rank1 pops=256、words=0、seg=0、223–486 µs` ⇒ ≈0.9–1.9 µs/pop，而闭式用 `c_unit≈17.5 ns/unit`。按 m03746 紧上界（145.2~145.7 µs），实测 max **超紧上界 1.5–3.3×**（但 < 天花板 1086.7 µs、over1ms=0）⇒ 闭式对**指针稠密**对象系统性低估，下一轮必须按 n 分桶 + 冷/热 pop 分别标定（E29）；可能与每 pop 随机访存、cache miss 相关，未做 callgrind 归因。
10. **W_s 的输入 nslots 已由几何证明上界（Lead m03891 裁决），但具体分布仍未观测**：`chunk/slotSize >= SLOTS_MIN=1024` + `slotSize≥16 B` ⇒ `nslots<4096` ⇒ `q≤64`、`W_s≤192`（结论确定；源码 `:1005` 的 131072 是过松旧估计，只留附注）。`nslots` 具体取值既不在 `[top]` 也不在 `[quota]` 里，故 after 仍用 /tmp 插桩记录 `_q` 实际分布（不为定上界，只为验证上界不被突破 + 记录 seg 分段步数）。脚本 `/tmp/empirics/segcount/patch_q.py` 已就绪（注入 `[dbgq] q_steps/q_sum/q_max/q_cap steps/seg_steps/seg_words`；`--check` hits `{globals:1,q:1,mark:1,sweep:1,dump:1}` = OK）。
  - **预跑验证（非权威，仅供脚本定稿）**：用 16:01 的中间构建 `build-rf/extc`（md5 `7019342b3e5ccddbe67cbdc96eae2395`、`src/back/reclaim.c` md5 `7c2a9cedd7edd54de0a378d8a507dcbe`）生成 `pd_bigobj.c`、插桩后跑 **20 轮**（`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，只验证插桩不取计时）：`[dbgq] q_steps=18 q_sum=1152 q_max=64 q_cap=0 steps=2444 seg_steps=2367 seg_words=19,390,464` ⇒ **实测 `q_max=64` 恰好等于几何上界、`q_cap=0`（从未撞 8192 cap）**，`seg_words` 恰为 `2367×8192`（每段一步、无跨段）。正式 after 的产物**已带 [quota] 终值行**（§3.2：`deadline_us=800 … checks=830,573 hits=1`）⇒ `deadline_hits` 列可直接读；但正式 after **未再跑该插桩**（插桩会改变时序，只用于定稿脚本），故 §3.2 的 q/seg 分布只由上述 20 轮预跑代表，正式轮次的 q 分布**未测**，记存疑。
11. **`deadline_hits` 无历史基线，只有 after 内部读数**：before 构建（build-lead5）没有 deadline 机制 ⇒ 无 `[quota]` 行；after 实测 `hits = 0（60 轮）/ 1（200 轮）`，`checks 244,736 / 830,573` ⇒ 每 4,000 次检查才命中 1 次，**不随轮数线性增长**、也不是常态停顿源。另外 `pd_pointer_dense` 的 `checks=0 hits=0` ⇒ 该形状没进入配额循环。

12. **G18 的内存上界曾被单位 bug 放大 1024×（tests 侧已修）**：`live` 来自 `[work] M_max/3`，**单位是字节**；`g18.py` 与 `summarize.py` 早期把它当 kB 用 ⇒ 打印过 `live=8,388,624 kB`、上界 `25,167,920 kB`。修复后 `g18.json` 带 `live_bytes`（8,388,624 B）+ `live_kb`（8,192），两处 `g18.json`（`p0-before`/`p0-after`）已重生成，`g18.py`/`summarize.py` 的 `--selftest` 通过。**下游引用 §3.2 上界时必须用修复后的 json**。
13. **sizeratio 的 +9.31% 是"部分归因"**（§4.3）：约 7.3 个百分点可归到 P 的每分配配额记账（把 `MAX_MARK=0 / DEADLINE_US=0` 后墙钟回落 −7.34%），约 +2 个百分点未归因（callgrind 无 branch sim、ns/Ir 升 3.8%）。**不得写成完全归因，也不得因"step_total 降了"忽略墙钟回归**。注意这里用 `-D` 覆盖的是**产物编译期宏**（`QUOTA_MAX_MARK/QUOTA_DEADLINE_US` 在源码里是 `#ifndef` 默认），不是运行期 env。
14. **G4 段的 M_max = 1,360,583,952 B 口径存疑**：该值由 `summarize.py` 从各用例 `[work]` 里取"最大正值"，来源是 **pb 100M 档**，单位是字节；它与 G4-Stress 的 A 档 `M_max=7,200,144 B`（stress.py 自读声明值）**不是同一口径**，不能混用，也不能当"内存墙"线。本轮 G4 的正式判定一律用 stress.py 的 A/B/C 三档（§5）。
15. **G17 的次序在 after 变成 A 27,812 > D 25,700 kB（before 是 A 22,240 < D 25,696）**：A 是隐式 3·L、D 是显式 16 MiB，理论上 A 的墙更紧、峰值不该更高。观测到的反转与 A 档的 `[mmax] memory wall unreachable: M_max=… floor=…; using floor` 路径同时出现（自学习 scale 早期把 M_max 压到 floor 以下），**机制未定位**；G17 是观察项、不判 PASS/RED，此处只登记不一致，供 theory 侧核对。
16. **`tests/reclaim/run.sh` 与 `tests/reclaim-perf/gate.sh` 不由本机执行**：两者都在 Lead 的串行窗口内跑（前者直接编 `src/back/reclaim.c` 的发射文本，跟随**当前源码**；用 build-lead5/build-rf 二进制对照没有意义，before 需要旧源码或隔离基线）。本文件 §5 对这两行只写"由 Lead 执行"，**不给结论**。
17. **闭式与实测的差（G18 ②）**：闭式标称 145.2–145.7 µs 基于 `c_unit=17.5 ns/unit`、`c_word=9.5–12 ns/word` 两个本机未标定的常数；实测 `[step] max` 185–217 µs 落在标称与天花板 1086.7 µs 之间，**闭式只作量级对照，不作上界证明**（§2.1 已声明）。同轮交错 A/B 的 Ir 是每形状各跑一次 callgrind（非 reps 中位），只用于量级归因。


---

## §8 本次改动的文件（tests/ 与 docs/analysis/，未改 src/）

| 文件 | 改动 |
|---|---|
| `tests/gc-p0/src/pd_bigobj.extc` | 头注释改为 G18 口径；`parseN` 默认 20→60 |
| `tests/gc-p0/src/pd_pointer_dense.extc`（新） | 指针稠密观察用例：8 MiB dense × 1M 非空指针 × 200 轮，stdout `PD n=200 made=200 s=1048575`；不参与 G18 判定 |
| `tests/gc-p0/g18.py`（新） | G18 量具：`json`（pd_bigobj 60/200 + pd_pointer_dense 的 histo/step/top3/work → `g18.json`，含 P_step 闭式分量）+ `compare`（Δslices/ΔGCus/桶差/top3 对照 + 内存/规模/①② 判定）+ `--selftest` |
| `tests/gc-p0/g20.py`（新） | G20 量具：`run`（四形状 × reps，注入 PAUSE=1/STEP_CPU=1，--pin）+ `compare` + `--selftest` |
| `tests/gc-p0/pause_parse.py` | 抽出 `parse_text(txt)`，供 g18.py / g20.py 复用 |
| `tests/gc-p0/run.sh` | 新增 `g13` / `g18` / `g20` / `g20cmp` 子命令；G18 段显式 `EXTC_RC_STEP_CPU=1 run_pause`，追加 `pd_pointer_dense` 观察用例，并写 `g18.json` |
| `tests/gc-p0/summarize.py` | 新增 G18 段（内存/规模/stdout 硬判据 + 两通道表与 ①② 判定 + P_step 闭式 + pd_pointer_dense 观察块 + top3 对应性）与 G20 段（读 `g20.json`）；**修 G18 的 live 单位 bug**（见下） |
| `tests/gc-p0/ab.py`（新） | 同轮交错 A/B 量具：奇偶轮交换 A/B 顺序、`--reps`/`--pin`、`--extra-a/--extra-b` 传**产物编译期** `-D` 覆盖、每形状跑一次 callgrind 取总 Ir、写 `ab.json`；判据 `|Δwall| ≤ 3%`。§4.3 的两张表与 sizeratio 归因实验均由它产出 |
| `tests/gc-p0/g18.py` / `summarize.py`（修） | live 单位 bug：`[work] M_max/3` 是**字节**，早期当 kB 用 ⇒ 上界放大 1024×；改为 `live_bytes`（保留 `live_kb` 兼容旧 json），上界按字节算。`p0-before`/`p0-after` 两处 `g18.json` 重生成 |
| `docs/analysis/MEASUREMENTS-P0-IMPL.md` | 本文件：状态段、§0 结论 4/9–12、§1.1 after md5 与源码 md5、§1.3 after 子命令与 `ab.py`、§3.2 after 表、§3.3 dense after、§4.2 after 判定表 + §4.3 A/B 与归因、§5 全表 after 列、§6 原型差异、§7 12–17 |

自检：`python3 tests/gc-p0/g18.py --selftest`、`python3 tests/gc-p0/g20.py --selftest`、`python3 tests/gc-p0/summarize.py --selftest`、`python3 tests/gc-p0/pause_parse.py --selftest`、`python3 -m py_compile tests/gc-p0/ab.py`、`bash -n tests/gc-p0/run.sh` 全部通过。

---

## §9 Lead 侧独立复验（`build-lead6`，v6 = P/B 落地后）

构建：`rm -rf build-lead6 && make BUILDDIR=build-lead6 -j24` ⇒ exit 0、**0 warning**、`build-lead6/extc` md5 `38886950080f7a071e01674157ab4ee9`；源码 `src/back/reclaim.c` `7c2a9cedd7edd54de0a378d8a507dcbe`（3801 行）、`src/back/codegen.c` `d339382889b87c0dd18b357c69e2e779`（16017 行）、`tests/reclaim/driver.c` `83e167d761705268dbda3f89895ea574`（1441 行）。

| 项 | 命令 | Lead 实测 | 结论 |
|---|---|---|---|
| 默认档断言 | `bash tests/reclaim/run.sh` | **通过 181，失败 0**（含 ASan/UBSan 181/0、`ALLOC_INLINE=0` 181/0、`chunk=1` 10/0、`chunk=1e9` 红自测有牙） | PASS |
| 计数档断言 | `EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh` | **通过 237，失败 0**（含 ASan 237/0） | PASS |
| 指令数 | `EXTC=./build-lead6/extc bash tests/reclaim-perf/gate.sh` | **86,618,661 Ir ≤ 88,029,115**（比 v5 的 86,316,991 多 264k，余量 1.6%） | PASS |
| 定向释放/清扫门禁 | `EXTC=./build-lead6/extc bash tests/reclaim-gaps/gate-release.sh` | **通过 4/4**（A sweeps 1/1、B 1.00×、C 逐字相同、D releases 19/79） | PASS；v5 的 2/4 由 P 配额修复 |

⇒ 与 §1–§8 的 before/after 表一致；`gate-release` 的 A/D 两项从「已知红」变绿，原因是 P 配额改变了 `visitedBytes` / `budgetOk` 的信用节奏（不是改断言）。

### §9.1 Lead 补充（doc-audit 阶段二采纳项）

- **`summary.txt` 的 1024× 残留已清**：`tests/gc-p0/out/p0-after/summary.txt` 与 `p0-before/summary.txt` 原第 51 行打印的是旧 bug 的 `live = 8388624 kB / 上界 = 25167920 kB`（字节当 kB）；json 侧早已修好（`live_kb=8192`）。本轮用修好的 `summarize.py` 重新生成两个 `summary.txt`（口径 `live = 8192 kB`、上界 `26,624 kB`），原文件留档为 `summary-orig-1024bug.txt`；判据结论不变（after 1.000× PASS / before 2.163× RED）。
- **G20 判据建议（Lead 采纳）**：promote 单步不用 `max`（5 reps 由 1 个 277.6 µs 支配），改 **`p99.9 ≤64 µs` 且 `>64 µs 占比 ≤0.05%`**（after 实测 32 µs / 0.0126%，before 0.0113%）；墙钟/RSS 改**单侧**（≤+3%）。按此改判后 promote PASS，**真回归只剩 sizeratio 墙钟**（官方 +2.03%、独立 +3.27%；Ir +6.00% 两次逐位相同）。`quota0` 对照收回 **4.0–7.3 pp**（官方顺序跑 7.34、独立构建 4.01），故归因写区间而非单值。
- **本文件 md5 / 行数**：本文件在 doc-audit 的 GO 之后又被 Lead 追加了 §9（Lead 复验）与本节 ⇒ GO 里记的 `cc7cd35e…/369 行` 已过期；现为 **384 行**（`md5sum` 以当前文件为准，doc-audit §13.1 记录的两套口径已对齐）。
