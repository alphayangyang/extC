# MEASUREMENTS-GC-P0 —— 阶段 0 门禁基线与 after（G8 / G9 / G4 / G1 / G12–G17）

> **状态**：before（HEAD `85b8871b`）与 **after（FREEZE v4）** 均已测完归档（§8）。§8 的 after 数字是
> **2026-11-09 09:47 之后（Lead m02638 通告后）** 在 `build-rf/extc`（md5 `9fd0dcd861374668be3c74c342beac6f`）上**串行**跑出的正式数据；
> **v3 及更早的 after 全部作废**（interim `f97a425f…`、v1 pb 挂死、v2「G1 全 0」、v3 旧版桶边界），
> v3 目录已归档为 `tests/gc-p0/out/after-v3-superseded-0915Z/`（§10）。
> 冻结源码：`src/back/reclaim.c` md5 `ff42799bfa62c2c9b0ac5b238e7d05e9`（3607 行）、
> `src/back/codegen.c` md5 `cf2885440981b5c9d154927fb61441b9`（15994 行）、
> `tests/reclaim/driver.c` md5 `9aae9cb59ff28ef4674c7c1690702d8e`（1216 行）；git HEAD `85b8871b` + 工作树 patch（未提交）。
> 同一源码的二次构建 md5 会因 `-g` 里嵌入的 build 目录路径不同而不同，**溯源以源码 md5 为准**（Lead m02638）。
> 所有数字均为**本机实测**，不是文档引用；与文档冲突处在 §7 标注。
> 本文件由 tests/gc-p0/run.sh 生成（可一键重跑，§6）。原始数据：
> `tests/gc-p0/out/baseline-head-85b8871b/`（before）与 `tests/gc-p0/out/after/`（after，含 `log/` 原始 stderr）。

---

## §1 机器与工具链

| 项 | 值（实测） |
|---|---|
| uname | `Linux Yeung 6.18.58.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC x86_64` |
| CPU | `nproc` = 24（WSL2 虚拟化；scaling_governor=none） |
| 内存 | MemTotal = 20,478,916 kB（≈19.5 GiB）；cgroup `memory.max=max` |
| 编译器（extC，before） | `build-emp/extc`，md5 `fad7a7667a2d9ad133c2d09af8c6f30e`（`make BUILDDIR=build-emp -j`） |
| 编译器（extC，**after/FREEZE v4**） | `build-rf/extc`，md5 `9fd0dcd861374668be3c74c342beac6f`（Lead m02638 的规范产物；同源二次构建） |
| C 编译器 | gcc 15.2.0（Ubuntu 15.2.0-16ubuntu1） |
| 生成物 CFLAGS | `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`（**POSIX 宏必须有**，否则 `[step]`/`[histo]` 全 0，§7-12） |
| 量具 | `/usr/bin/time`（GNU）、`/proc/<pid>/status` 2 ms 采样（tests/gc-p0/sample.py） |
| Python | 3.14.4 |

### 1.3 计时噪声地板（**G1 判据因此修订**）

本机（WSL2）的计时噪声地板远大于 50 µs，测法：纯 CPU 微基准 `tests/gc-p0/noise_floor.c`
（把 **200 次整数加法** 夹在时钟之间，无分配、无系统调用；n=200000、inner=200）。
2026-11-09 起该基准**同时打两个口径**：`wall`（CLOCK_MONOTONIC，含被抢占）与
`cpu`（CLOCK_THREAD_CPUTIME_ID，只算本线程执行）；每次迭代有 4 次 clock_gettime 调用，
故其绝对 mean（≈0.16/0.31 µs）包含计时开销，**只能比分布形状与尾部，不能当绝对工作量**。

| 观测（clock） | max µs | mean µs | over50 | p99.9 µs |
|---|---:|---:|---:|---:|
| v2 wall（3 reps，与基线同负载） | 94.3 / 77.9 / **180.9** | 0.309–0.319 | 3 / 1 / 3（n=200000） | 0.51–0.55 |
| v2 **cpu**（同一批 3 reps） | 93.9 / **34.1** / **180.6** | 0.164–0.168 | **1 / 0 / 3** | **0.296–0.313** |
| v1 wall（空载时 3 reps） | 23.8 / 36.3 / 41.1 | ≈0.05 | 0 / 0 / 0 | 0.1 |
| v1 wall（基线目录，负载较高，7 reps 最差） | 155.3 | 0.051 | 3/200000 | 0.1 |
| Lead 独立复测（wall，同日 load≈1.1） | 98.5 / 151.6 / 171.2 | 0.234–0.256 | over16=28–45，over64=2–13 | 未测 |

结论（**方法学**，不是 GC 的结论）：

1. `[step] max ≤ 50 µs` 在这台机器上**结构性不可达**：空转微基准自己就 100–250 µs 量级，
   与 GC 无关。§4.4 里 GC 的 step max 106–277 µs 落在同一噪声区间，**不能**用它判 GC。
2. 噪声地板随负载显著漂移（23.8 → 134.8 µs）；**必须每次与 GC 同机、同时钟、同规模一起测**，
   不能引用历史上的一次空转数字。
3. 现有插桩只有 `over16/over64/over256/over1ms` 四个累积桶（`src/back/reclaim.c:2678-2714`），
   **没有 >50 µs 的精确计数**。方向必须写对：>64 µs 的步一定 >50 µs ⇒ **over64/n 是下界**；
   >16 µs 的步包含 50–64 µs 的步 ⇒ **over16/n 是上界**。真实占比落在 [over64/n, over16/n] 内，
   下界 > 阈才判 RED，上界 ≤ 阈才判 PASS，跨阈记 N/A（§4.4 即按此判）。
4. p99.9 需要 P1-6 直方图（n≥1e5）；**未落地记 N/A，不得拿 max 代替**。
4b. **CPU 口径也不是零噪声**：同一批 3 reps 的 cpu max 仍有 34–181 µs、cpu over50 = 0–3/200000
   （p999 = 0.30–0.31 µs）。也就是说 WSL2 的线程 CPU 时钟本身有罕见尖峰（机制未定，**存疑**），
   但**占比 0–1.5e-5 远低于 ② 的常数阈 1e-4**、p999 也远低于 ③ 的 50 µs ⇒ 用 cpu 口径判 ②③ 是可行的；
   max 依旧不可用作判据（wall/cpu 两种口径的 max 都能到 180 µs）。
5. **2026-11-09 裁决（Lead；本文件此后一律按此判）**：② 的阈值固定为常数 **1e-4**，**不再乘"3× 空转"**——
   空转的 over50 率本身只有 0–1.5e-5 量级（7 reps 里 0–3 个事件），乘 3 只会让门禁在 N/A/RED 之间漂；
   空转基线照常跑，但**只作参考、不作分母**。同时新增**小样本豁免**：n<1e5 的用例 ② 判 **N/A 并报 max**
   （1 步即 2e-3，比例统计不可判），pd（≈510 步/rep）属此类，不得静默按字面判 RED。
   计时口径：默认墙钟；运行时加 EXTC_RC_STEP_CPU=1 后 [step] 改线程 CPU 时间（[pause] 保留墙钟），
   记录带 clock=wall|cpu、汇总按 clock 分表——停顿契约约束的是"工作量"，不是"被抢占多久"。
6. **直接后果（必须记住，不能当通过）**：在 4 个粗桶 + 常数 1e-4 下，四个用例的真实占比区间
   [over64/n, over16/n] **全部跨阈**（下界 1.6e-5–6.6e-5 < 1e-4 < 上界 2.0e-4–5.5e-1）⇒ **G1② 当前结构性 N/A**；
   唯一明显超标的 pd（下界 0.54）又因 n<1e5 豁免而判 N/A。也就是说：**直方图落地前 ② 只是信息项，不是门禁**。
   换线程 CPU 计时后 [step] 应基本去掉抢占噪声（mean 仅 0.46–1.28 µs），上界才有机会 ≤1e-4、② 才可判 PASS/FAIL。
7. **判据口径（Lead m01103）**：GC 的慢步是**真实工作量、不是抢占**——runtime-facts 用 `[top]` 归因
   （pc 50M：B=1 时慢步 dur 192.6 µs / cpu 194.1 µs；B=8 时 152.5 / 152.9，minflt=0；**本文件未独立复现**）。
   因此 after 跑**两遍**：默认墙钟 + `EXTC_RC_STEP_CPU=1`；**②③ 判据用 cpu 口径**，wall 只作观察。
   同时记录 `[chan]` 的 release / regionfree / collect 三通道分布（直方图与 [chan] 由 runtime-facts 落地），
   作为"停顿有界"的通道级证据；落地前这两项在 §8 记 N/A。

**构建隔离**：本任务只用 `build-emp/extc` 读编译，**不写 `build/`**（Lead 2026-11-09 约定）。
另：`build-emp/extc` 与 `build/extc`（md5 `f4a0a9b62f4ac2fff1892f53dd34bc42`）对同一 g8 源生成的 C 逐字一致；
两者 md5 不同仅因编译期 `EXTC_RC_TIME` 开关不同（`src/back/codegen.c:12395` 用 getenv 决定是否发 [rccount] 计数），不影响本文件任何数字。

---

## §2 量具与口径

### 2.1 峰值怎么读（**不用 ru_maxrss**）

WSL2 上 `posix_spawn` 的子进程 `ru_maxrss` 会被**父进程（python）的峰值污染**：
实测 `/bin/true` 经同一采样器也报 9,916 kB；本文件 g8_t2 的 `ru_maxrss` = 15,512 kB 而真实 `VmHWM` = 2,956 kB。
因此：

- **主量具**：`/proc/<pid>/status` 的 `VmHWM`（单调递增，末次采样 ≈ 峰值），采样间隔 2 ms；
- **交叉量具**：`/usr/bin/time -f '%M %e'`（time 的父进程很小，不被污染）；
- `ru_maxrss` 只作为 `ru_maxrss_polluted: true` 的参考字段写入 JSON，不参与任何判据。

### 2.2 重复与噪声

- 每用例 **3 reps**，表里给中位数；内存类（VmHWM）3 reps 离散 ≤ 8 kB（<0.3%）。
- 墙钟在 WSL2 上有 10–30% 噪声（见 pb：1.351–1.765 s）；**墙钟只作参考，不作判据**。
- 短进程（G8/G9 约 20 ms）2 ms 采样只能采到约 10 点；靠 VmHWM 单调性 + `/usr/bin/time` 交叉兜底
  （g8_t2：采样 2,956 vs time 3,024 kB，差 2.3%）。
- 采样器自身有开销（每 2 ms 读一次 status）；G9 的 2.7 ms 墙钟含此开销，不作性能数字用。

### 2.3 判据出处

判据来自 `docs/analysis/GC-ROOFLINE.md:461-471`（§7 门禁表）；其中 **G1 于 2026-11-09 按 §1.3 的噪声地板实测修订**（Lead 决定），其余门禁未变：

| 门禁 | 判据（逐字） | 出处 |
|---|---|---|
| G1（原） | max ≤50 µs、over-1ms = 0、mean ≤5 µs；p99 需先加直方图 | GC-ROOFLINE.md:463 |
| G1（**现行**） | ① over1ms 步数=0（**权威读数取 `[histo]` 中「严格 >1 ms 的桶和」（b4ms 起），无 `[histo]` 时退回 `[step] over1ms`**；parser 硬编码 v4 15 桶表，见 §10）；② >50 µs 步占比 ≤**常数 1e-4**，n<1e5 判 N/A+报 max（空转仅参考）；③ p99.9 ≤50 µs（需直方图，v4 起有 15 桶可判桶界）；**②③ 判据用 cpu 口径**（EXTC_RC_STEP_CPU=1），墙钟只作观察 | Lead 2026-11-09（§1.3 第 5/7 条）、m02523 |
| G4 | pb 20 B/node ±10%；pc ≤4 MB 恒定；注入 4× 循环垃圾 RSS ≤ M_max+8 MiB | :466 |
| G8 | 强制 T1 于 chain2 时峰值 ≤1.1× 同形状 T2 | :470 |
| G9 | 活任务存在时 `syspool::collect()`，任务帧仍可驱动、任务数不变 | :471 |
| G12 | 参数槽覆盖：**强制计数档（T3）** stdout 逐字 `alias check b.v=999` 且 exit 0；默认档仅观测（T2，PASS-by-design） | Lead m01196/m01267 |
| G13 | `copyInto` 逐字搬移：默认档（自动 T1）与强制计数档（T3）**都判**，stdout 逐字 `d0r=7` 且 exit 0 | Lead m01267/m01315/m01356 |
| G14（**final**） | 默认档循环 `copyInto` 引用元素：maxRSS(800k) ≤ **1.02× maxRSS(对照)** 且 maxRSS(800k) ≤ **4 MB**；ratio(800k/50k) 仅观测（Lead m01688：50k→100k 的 +576 kB 是一次性高水位，不是泄漏） | Lead m01688 |
| G15 | pool 槽里的引用赋值（循环 `poolSlice`→写 ref→`poolResize`→`poolGive`）：maxRSS(800k) ≤ **1.20×** maxRSS(50k)；**标注 XFAIL(known L10-a)**，预期 RED、不计入 after 回归、不作阻塞（给下一轮 P0-8 留量具；修好后硬判据不变） | Lead m01501 |
| G16 | 回归：sweep 中新建 slab 的错误标黑（§9）。N=2,000,000，3 reps：exit 全 0、未 timeout、stdout 含 `cnt=2000000` 与 `sum=1999999000000`、wall <5 s | Lead m02148(d) / §9 |
| G17 | 观察项（非门禁）：长命集+churn 下峰值随 `EXTC_RC_M_MAX` 单调（A 默认隐式 3·L / D 16MiB / B 64MiB / C 1TB） | Lead m01949① |
| L10-b | **已知限制（XFAIL，不进回归）**：目的 view 由循环体 arena 分配 ⇒ `slabOfInterior(base)` 失败 ⇒ 退回 noSweep ⇒ 线性泄漏；记录 maxRSS(800k)/maxRSS(50k) | Lead m01688 |
| L10-c | **已知限制（XFAIL，不进回归）**：目的 view 在 rc 且被反复覆盖 ⇒ `rsInc` 边不撤销 ⇒ 线性泄漏；记录 maxRSS(800k)/maxRSS(50k) | Lead m01688 |

---

## §3 用例清单

| 门禁 | 用例 | 源文件 | 编译开关（extc env） | 负载语义 |
|---|---|---|---|---|
| G8 | g8_t1 | `tests/gc-p0/src/g8_chain2.extc` | `EXTC_RC_SWEEPFREE=1 EXTC_RC_NOCOUNT=0` | 两结点链，N=2,000,000 轮：每轮 `w.head = new node`，再 `(*w.head).next = new node`（父→子一条边）；父被下轮覆盖 |
| G8 | g8_t2 | 同上 | `EXTC_RC_SWEEPFREE=0` | 同上（强制清扫档对照） |
| G9 | g9 | `tests/zone/collect_with_live_task.extc` | 默认 | 协程 `mk(5)` 驱动 1 步后 `syspool::collect()`，再驱动到底 |
| G4 | pb | `tests/gc-p0/src/pb_escape_grow.extc` | 默认 | 逃逸对象持续累积：每个 node 挂进函数级 `w.head` 永不摘除，N=100,000,000 |
| G4 | pc | `tests/gc-p0/src/pc_cycle_churn.extc` | 默认 | 每轮建 2-环挂根再丢弃 ⇒ 不可达环垃圾，N=100,000,000（另跑 400,000,000 复验恒定） |
| G4/G1 | cyc | `tests/reclaim-shapes/cyclic_pressure.extc`（既有语料） | 默认 | 既有循环压力形状；G4 用 N=20M vs 80M 作 4× 代理，G1 用 N=20M |
| G1 | pc/pb 50M | 同上两探针 | 默认 | 运行时 `EXTC_RC_PAUSE=1` 观测 |
| G1 | pd | `tests/gc-p0/src/pd_bigobj.extc` | 默认 | 200 次 `new big`（`[1048576]?mut ref leaf`，8 MiB payload，走 huge block），挂根即丢 |
| G12 | g12_param | `tests/gc-p0/src/uaf_param_overwrite.extc` | 计数档：`EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0`；默认档：无 env | `fn clobber(p: mut ref node){ p = new node }`；main 里 `a.v=42424242` → clobber(a) → `new b` → `a.v=111` → 打印 `b.v`（参数槽覆盖后旧别名是否复活） |
| G13 | g13_bulk | `tests/gc-p0/src/uaf_bulk_copy.extc` | 同上（两档都跑） | `ref s[0]` 绕道形状：`q=ref s[0]; q.r=new node; q.r.v=7` → `copyInto<item>(d,s,1)` → `q.r=new node; b.v=555` → 打印 `d0r=`（逐字搬移后目标容器指针是否被判死） |
| G14 | g14 | `tests/gc-p0/src/leak_copy_refs.extc` | 默认（NOCOUNT 1 / SWEEP_FREE 0） | `buf/src` 各 `new item[1]`；每轮 `src[0].r = new node` 后 `copyInto<item>(buf, src, 1)`；N=50,000 与 800,000 两档（`timemax`） |
| G15 | g15 | `tests/gc-p0/src/leak_pool_pin.extc` | 默认（另在 T1/T3 观测，见 §5） | 每轮 `poolSlice<item>(rid,2)` → `s[0].r = new node` → `poolResize<item>(rid,s,4)` → `s[0].r`/`s[1].r = new node` → `poolGive<item>(rid,s)`；N=50,000 与 800,000 两档（`timemax`） |
| 观察项 | p03_ring_release | `tests/gc-p0/src/p03_ring_release.extc` | 默认 | 10 轮 × 400,000 结点环（`next` 回指头结点），每轮丢弃整环；测 P0-3 权衡形状峰值，**不判绿** |

**档位自证**（产物第 13/15 行，逐字）：

```
g8_t1: 13:#define EXTC_RC_NOCOUNT 0    15:#define EXTC_RC_SWEEP_FREE 1
g8_t2: 13:#define EXTC_RC_NOCOUNT 1    15:#define EXTC_RC_SWEEP_FREE 0
```

即 g8_t1 确为**事件档（T1：计数开 + 清扫永远不触发）**，g8_t2 为**清扫档（T2：计数关）**。
（自动档为何对 chain2 退回 T2、以及 T1 的泄漏机制，见 MEASUREMENTS-TIER1.md §5.3，本文件不重复论证。）

---

## §4 基线结果（before）

### 4.1 G8：强制 T1 vs T2 的峰值（chain2，N=2,000,000）

| 档 | 产物宏 NOCOUNT/SWEEP_FREE | VmHWM rep1/2/3 (kB) | 中位 (kB) | `/usr/bin/time` maxrss (kB) | 墙钟 (s) | exit |
|---|---|---:|---:|---:|---|---|
| T1（事件档） | 0 / 1 | 45,448 / 45,444 / 45,448 | **45,448** | 45,448 | 0.039–0.040 | 0,0,0 |
| T2（清扫档） | 1 / 0 | 2,960 / 2,956 / 2,956 | **2,956** | 3,024 | 0.013–0.014 | 0,0,0 |

ASAN：两侧生成物各用 `-O1 -g -fsanitize=address` 重编并跑 2,000,000（`tests/gc-p0/out/asan-g8-baseline/log/g8_t{1,2}_asan.*`）：
**rc=0、无 ASAN 报告**（基线 T1 虽有"父死留子"的逻辑泄漏，但地址从不复用，所以 ASAN 看不到 UAF——这与 G8 的"泄漏"判据无关，patch 后用于确认级联释放安全）。

- **T1/T2 = 45,448 / 2,956 = 15.38×**（用 time 交叉：45,448/3,024 = 15.03×）。
- 判据 ≤1.10× ⇒ **RED**。基线值与 MEASUREMENTS-TIER1.md §5.3 的 45.4 MB / 3.0 MB（15.1×）在 ±2% 内一致，
  说明该泄漏可跨轮复现（本轮是 `build-emp` 编译、`tests/` 下独立探针，仍复现）。
- 泄漏机制已在源码定位（供 patch 参考，不是本文件的结论）：
  `src/back/reclaim.c:1245-1264` 的 `extc_rc_freeSlot` 只做 freeSlotLink + 清槽位元数据 + classPush，
  **不减少 payload 里子引用的 hold**；减子引用只在清扫路径 `freeSlotWord`（`src/back/reclaim.c:1271-1279`）逐槽做，
  而 T1 的 `shouldSweep` 恒 0（`src/back/reclaim.c:2650-2658` 的 `#if EXTC_RC_SWEEP_FREE ... return 0; /* skip:sweepfree */`）。

### 4.2 G9：活任务存在时 `syspool::collect()`

3 reps 输出完全一致（逐字）：

```
stdout: live0=1 freed=1 live1=0
stderr: tests/zone/collect_with_live_task.extc:43: trap: driving a coroutine whose task has already ended (its frame was released with the task)
exit  : 70（3/3 reps）
```

解释（逐项对应判据）：`collect()` 返回 `freed=1`，即它把**仍在被句柄 `h` 持有的活任务**的 place 放掉了；
下一次 `h.next()`（用例 :43）命中 trap，进程以 70 退出 ⇒ **任务数从 1 变 0、任务不再可驱动**。
判据要求"任务帧仍可驱动、任务数不变" ⇒ **RED**。

ASAN：同一份生成物用 `-O1 -g -fsanitize=address` 重编（`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`），
结果 rc=70、**无 ASAN 报告**（`tests/gc-p0/out/baseline-head-85b8871b/log/g9_asan.err` 为空，只有 trap 行）。
注意：trap 在**用到已释放帧之前**就发生，所以 ASAN 的"无报告"**不能**证明 collect 路径没有 UAF——
只能证明本用例的触发点先于非法访存（见 §7 第 5 条）。

### 4.3 G4：内存界

| 子判据 | 用例 | 数值（VmHWM 中位 / time 交叉） | 结果 |
|---|---|---|---|
| pb 20 B/node ±10%（18–22 B） | pb N=100M | 2,000,504 kB / 2,000,400 kB ⇒ **20.49 B/node** | **PASS** |
| pc ≤4 MB 恒定 | pc N=100M | 2,916 kB = **2.85 MiB**（time 2,832） | **PASS** |
| 同上，恒定复验 | pc N=400M | **2,916 kB**（time 2,832；4× 轮数不变） | **PASS** |
| 注入 4× 循环垃圾 RSS ≤ M_max+8 MiB | cyc N=20M vs 80M（代理） | 6,280 / 6,276 kB ⇒ ratio **0.9994×**（time 6,244/6,272） | **PASS（仅代理）** |

**重要限定**：G4 的正式判据是"注入 4× 循环垃圾时 RSS ≤ **M_max**+8 MiB"，而 `M_max` 配置项属于 P0-4
（`docs/analysis/GC-ROOFLINE.md:430`），**当前源码未实现** ⇒ 该判据**当前不可判**。
本表用"N 与 4N 的峰值比 <1.5"作代理，证明的是"当前清扫能把循环垃圾收成常数"，不是 M_max 界本身。

pb 的 20.49 B/node 与 MEASUREMENTS.md §3 的 20 B/node 口径一致（16 B payload + ≈4.5 B 元数据）。
pc 的 2.85 MiB = 一个 slab 的常驻量级（`EXTC_RC_SLAB_SIZE` 1 MiB、`SLOTS_MIN` 1024，`src/back/reclaim.c:316-322`），
与 N 无关（100M 与 400M 完全相同）——这是"清扫档对环垃圾有界"的直接证据。

### 4.4 G1：停顿尾部（`EXTC_RC_PAUSE=1` 运行时观测，3 reps）

插桩**没有直方图**，只有 `[pause]`/`[step]` 的 max/total 与 over16/64/256/1ms 四个累积桶
（`src/back/reclaim.c:2678-2714`）⇒ **p50/p99 结构性不可得**，本表只给 max / mean / 桶。

| 用例（N） | collects | `[pause]` max µs（中位；3 reps） | `[pause]` mean µs（中位） | `[step]` n | `[step]` max µs（中位；最大） | `[step]` mean µs（中位） | over16 / over64 / over256 / over1ms | `[worst]` kind/why/budget |
|---|---|---:|---:|---:|---:|---:|---|---|
| cyc（20M） | 437 | **34**（39/22/34） | 4.90 | 195,947 | **134**（最大 277） | 0.963 | 91–114 / 5–7 / 0–1 / **0** | mark / advance / 256 |
| pc（50M） | 2,288 | **57**（57/39/75） | 1.12 | 212,784 | **114**（最大 160） | 0.460 | 32–60 / 2–4 / 0 / **0** | sweep / alloc / 8 |
| pb（50M） | 5 | **5**（29/4/5） | 2.40 | 106,728 | **135**（最大 160） | 1.277 | 46–86 / 5–8 / 0 / **0** | mark / advance / 256 |
| pd（200×8MiB） | 25 | **17**（20/17/6） | 1.96 | 643 | **172**（最大 173） | **44.16** | 355–356 / 341–353 / 0 / **0** | mark / advance / 0 |

判据对照（**现行**判据见 §2.3；原 max≤50 µs 已被 §1.3 的结构性噪声否决）：

桶的方向（关键）：`over64` 只数 >64 µs 的步，而 >64 ⊂ >50 ⇒ **over64/n 是真实 >50 µs 占比的下界**；
`over16` 数 >16 µs 的步，而 >16 ⊃ >50 ⇒ **over16/n 是上界**。故真实占比 ∈ [over64/n, over16/n]，
只有 **下界 > 阈** 才能判 RED、**上界 ≤ 阈** 才能判 PASS；两侧跨阈时只能记 N/A。

| G1 子判据 | cyc(20M) | pc(50M) | pb(50M) | pd(200) | 结论 |
|---|---|---|---|---|---|
| ① over1ms=0（3 reps 合并） | 0 | 0 | 0 | 0 | **PASS**（12/12） |
| ② >50 µs 占比区间 [over64/n, over16/n]（clock=wall，3 reps 合并） | 18/587,841 … 308/587,841 = **[3.06e-05, 5.24e-04]** | 10/638,352 … 130/638,352 = **[1.57e-05, 2.04e-04]** | 20/320,184 … 216/320,184 = **[6.25e-05, 6.75e-04]** | 1,046/1,929 … 1,066/1,929 = **[5.42e-01, 5.53e-01]** | **N/A（结构性不可判）**：四例区间全跨 1e-4；pd 明明超标但 n=1,929<1e5 触发小样本豁免，只报 max |
| ③ p99.9 ≤50 µs | — | — | — | — | **N/A**（无直方图，P1-6 未落地；不拿 max 代替） |
| （参考，非判据）max µs 中位 | 134（最大 277） | 114（最大 160） | 135（最大 160） | 172（最大 173） | 落在 §1.3 噪声地板同一区间 |

- ② 用**常数阈 1e-4**（§1.3 第 5 条裁决），空转不做分母。粗桶只能把真实占比夹在 [over64/n, over16/n]：
  四例的下界都 <1e-4、上界都 >1e-4 ⇒ **全部跨阈，判 N/A（不是 PASS）**。pd 的下界 54% 明显超标，
  但 n=1,929 远小于 1e5 触发小样本豁免 ⇒ **N/A + 报 max**（不静默判 RED）。
- 这是"粗桶 + 常数阈"的直接代价：**直方图（P1-6）落地前 ② 没有区分力**；若 EXTC_RC_STEP_CPU=1
  的线程 CPU 计时落地，[step] 会去掉抢占尾巴（mean 仅 0.46–1.28 µs），上界才可能 ≤1e-4。
  该限制与修法记在 §7 第 2 条。
- pd 的 643 步/rep 全是 8 MiB huge block 的分配/丢弃，单步 158–173 µs；**归因已做（§8.2）**：
  同一 build 把 SCAN_CHUNK 从 8192 改回 65536，尾巴立刻复现（over64 1→346–352/643，max 78–98→164–269 µs）。
- mean（0.96 / 0.46 / 1.28 / 44.16 µs）自本修订起**只作参考**，不再是判据（原判据里的 mean ≤5 µs 随之废止）。

---

## §5 判据对照（before / after）

> after 列口径：`build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f`（FREEZE v4）、
> `CFLAGS=-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`、`OUT=tests/gc-p0/out/after`、REPS=3、
> 2026-11-09 09:47 之后串行跑（wall + `EXTC_RC_STEP_CPU=1` 的 g1cpu 两遍；ALL_RC=0、G1CPU_RC=0）。原始 stderr 在 `out/after/log/`（索引见 §8.3）。

| 门禁 | 判据 | before 实测 | after 实测（FREEZE v4） | 结论 |
|---|---|---|---|---|
| G8 | T1 峰值 ≤1.1× T2 | 15.38×（45,448 vs 2,956 kB） | **0.479×**（1,572 vs 3,284 kB） | **before RED → after PASS** |
| G9 | 活任务 collect 后仍可驱动、任务数不变 | `freed=1`，任务 1→0，下一次 next() trap，exit 70 | 3/3 PASS：`live0=1 freed=0 live1=1` / `tag=777` / `got=20 steps=5`，exit 0、stderr 空 | **before RED → after PASS** |
| G4-a | pb 20 B/node ±10% | 20.49 B/node | 20.49 B/node | PASS（两侧一致） |
| G4-b | pc ≤4 MB 恒定 | 2.85 MiB（100M 与 400M 相同） | 3.17 MB（3,244 kB，100M） | PASS |
| G4-c（正式，stress） | 注入 4×L 循环垃圾：默认档 VmHWM ≤ 声明 M_max+8 MiB、稳态相邻轮差 ≤2%、峰值随 M_max 单调 | **不可判**（M_max 未实现，无 stress 量具） | **PASS**：peak(A)=9,148 kB ≤ M_max 7,200,144 B +8 MiB=15,223 kB；末 6 轮末 VmHWM 差 **0.00%**；A 9,148 < B 41,704 < C 93,432 kB；A pressure_steps=33 | **before 不可判 → after PASS**（量具 §8.1） |
| G4-c（旧代理） | cyc N 与 4N 峰值比 <1.5 | 0.9994× | 1.000×（6,048 / 6,048 kB） | 代理 PASS（不等价于正式判据） |
| G4-M_max | 报告 M_max 实际取值与注入 4× 后 RSS/M_max 比 | 不可判（`[work]` 无字段） | M_max（cyc `[work]` m_max）=**4,101,120 B**；stress A 档 `[work]` M_max=7,200,144 B（=7,031 kB），peak/M_max=**1.30×** | 可判（§8.1） |
| G4-pressure | `[work]` 的 `pressure=1` 次数 | n/a（字段未落地） | cyc 1 / pc 0 / pb 1 / pd 1，合计 **18**（3 reps 合并）；stress A 档 pressure_steps=**33**/rep | 已可观测 |
| G1① | over-1ms = 0（`[histo]` 严格 >1e6 ns 桶和，权威） | 0（12/12，wall） | cpu **1 / 537,750**（cyc r1 单步 **2,540 µs**，`[top] kind=mark why=advance words=0 seg=0 pops=256`）⇒ **RED**；wall 口径 0/537,750（max 277.8 µs）；复跑 8(cpu)+8(wall)+E16 20 reps 均 0 次 >1 ms（§7-17、§8.8） | **RED（字面，1 步；按 Lead m02795 不作剔除、不改 N/A）** |
| G1② | >50 µs 占比 ≤1e-4（常数阈；cpu 口径；n<1e5 ⇒ N/A+max） | wall：cyc [3.06e-5, 5.24e-4] / pc [1.57e-5, 2.04e-4] / pb [6.25e-5, 5.9e-4] 均跨阈；pd 5.42e-1 但 n<1e5 | cpu：cyc [7.25e-5, 2.34e-4] ⇒ N/A（跨阈）；pc [3.64e-5, 1.46e-4] ⇒ N/A（跨阈）；pb [1.39e-4, 4.23e-4] ⇒ **RED（下界>阈）**；pd n=1,296<1e5 ⇒ N/A | **RED（pb）+ N/A**；聚合 **RED** |
| G1③ | p99.9 ≤50 µs（cpu 口径，v4 15 桶直方图） | **N/A**（P1-6 直方图未落地） | cyc **p99.9=16 µs** PASS；pc **≤32 µs** PASS；pb **≤32 µs** PASS；pd 落 32–64 µs 桶（跨 50）⇒ N/A | **部分 PASS + pd N/A**（三例 PASS、一例桶界不可分；Lead m02795 裁决 3：不得写成整条 PASS；不再用 64 µs 上界冒充） |
| G12 | 参数槽覆盖：**强制计数档（T3）** stdout 逐字 `alias check b.v=999` 且 exit 0；默认档仅观测（T2，PASS-by-design） | 本机 HEAD/build-emp 强制 T3 均 **111**（与 doc-audit 一致）；默认档 T2 **999** | 强制 T3 `alias check b.v=999`（3/3 exit 0）；默认档 T2 也 999（观测） | **before RED → after PASS** |
| G13 | `copyInto` 逐字搬移：**默认档与强制计数档（T3）都判**，stdout 逐字 `d0r=7` 且 exit 0 | 本机 HEAD/build-emp 默认 T1 **d0r=555**、强制 T3 **d0r=555**（与 doc-audit 一致） | 默认档产物 **T2** `d0r=7`（S0 形状默认档已不可命中）+ 强制 T3 `d0r=7`，3/3 exit 0 | **before RED → after PASS** |
| G14 | 默认档循环 `copyInto` 引用元素：maxRSS(800k) ≤1.02×对照 且 ≤4 MB（ratio 800k/50k 仅观测） | **6.68×**（2,636→17,616 kB，隔离基线 `/tmp/mine2-src/build-mine2/extc` md5 `a9b391cf…`，与 doc-audit 的 6.7× 吻合） | 50k=2,652 / 100k=3,228 / 800k=3,228 / ctl=3,228 kB；对照比 **1.000×**、≤4,096 kB；记账调用点 2 | **before RED → after PASS** |
| G15 | maxRSS(800k) ≤1.20×maxRSS(50k)；**XFAIL(known L10-a)** | 14.97×（12,420→185,932 kB） | **14.985×**（12,664→189,776 kB，默认 T2） | **XFAIL（预期 RED，不阻塞）** |
| G16 | N=2M 3 reps exit 0、未 timeout、输出正确、wall<5 s | n/a（旧 bug 下 1.5M 起挂死） | exit=[0,0,0]、timed_out=[F,F,F]、wall_max=0.055 s、cnt/sum ok | **PASS** |

> **G12/G13 的形状、档位与开关口径（Lead m01196/m01267/m01269，本机复核）**
>
> 1. `EXTC_RC_NOCOUNT` / `EXTC_RC_SWEEPFREE` 是**编译器**开关（`src/main.c:494` 标注 "CHANGES THE OUTPUT"），
>    必须在 `./extc` 生成 .c 时传入；运行期 `EXTC_RC_NOCOUNT=0 ./prog` **无效**（产物里计数入口已编译掉）。
>    注意环境变量名是 **`EXTC_RC_SWEEPFREE`**（无第二个下划线；与本文件 §6 G8 的写法一致），
>    `EXTC_RC_SWEEP_FREE=0` 会被静默忽略（本机实测两种拼法只有前者改变产物宏）。
> 2. **G13 必须用 doc-audit 的绕道形状**：`var q: mut ref item = ref s[0]`。`ref` 取元素地址使宿主算堆，
>    过 `unitAllAcyclic` 判据 iv ⇒ **默认档自动进事件档 T1**（产物 `NOCOUNT 0 / SWEEP_FREE 1`，
>    本机 HEAD 与 build-emp 实测输出 **555**）⇒ 这是 **S0：默认路径可命中的 bug**。
>    旧形状（直接 `s[0].r = ...` 写入 / f-string）判据 iv 失败退回 T2，默认档看不到 bug，**已弃用**。
>    若 compile-facts 采用「分析侧退回清扫档」方案，默认档产物应变 T2 且输出 7（此时按输出=7 判，TIER 只记录）。
   **after 实测（FREEZE v4，§8.0）**：默认档产物确为 **T2**（`NOCOUNT 1 / SWEEP_FREE 0`，产物宏反推）
   且 stdout `d0r=7`、强制 T3 也 `d0r=7`，两档 3/3 exit 0 ⇒ 修复采用「分析侧退回清扫档」路线，
   该 S0 形状在默认档已不可命中（555 → 7）。
> 3. **强制计数档 = `EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0`（T3）**。只给 `NOCOUNT=0` 时该形状会被
>    自动升级成 T1（事件档），跑出来的并不是「强制计数档」（本机实测产物宏可证）——`run.sh` 已改成两个一起传。
> 4. G12 默认档产物 `NOCOUNT 1 / SWEEP_FREE 0`（T2）无 freeSlot 入口 ⇒ 输出 999，**PASS-by-design（不可达）**；
>    G12 只在强制 T3 判。
> 5. 产物 `EXTC_RC_TIER` 宏现为**四值**（compile-facts 已实现，`src/back/codegen.c:12616-12621`）：
>    `1`=事件档（NOCOUNT 0 / SWEEP_FREE 1）、`2`=清扫档（1 / 0，默认）、`3`=计数+清扫档（0 / 0），
>    `0`=无回收运行期（`needReclaim` false；**不发射该行**，`EXTC_RECLAIM=0` 同样落 0 且无行）。
>    本机 `build-emp/extc` md5 `f97a…` 早于该 patch，其生成物把强制 T3 标成 `TIER 2` 且注释
>    "holder counting off"（观测记录，非当前源码问题；新构建应标 3）。`summarize.py` 的**判据不依赖 TIER 宏**，
>    一律从 `NOCOUNT/SWEEP_FREE` 反推档位（防止旧二进制或宏标签漂移影响判定）。
>
> **G14 判据演进（Lead m01688）**：before 用的「800k ≤1.20×50k」已于 FREEZE 前改为
> **「maxRSS(800k, copy) ≤1.02×maxRSS(800k, 对照) 且 ≤4 MB」**（对照 = 同程序 argv[2]=`ctl`，
> 只跳过 `copyInto`）。原因：50k→100k 的 +576 kB 是一次性高水位（100k 起各点恒定），不是泄漏；
> ratio(800k/50k)=1.218× 仅作观测。before 的 6.68× 是隔离基线 `a9b391cf…` 上的旧判据红，
> after 在 FREEZE v4 上 **PASS**（对照比 1.000×、800k=3,228 kB ≤4 MB）。
>
> **G4 正式量具（Lead m02258）**：`run.sh g4` 只覆盖 G6/G7（pb/pc/cyc 代理），**不是 G4**。
> G4 判据由 `tests/gc-p0/stress.py`（形状 `tests/gc-p0/src/g4_stress.extc`，注入 4×L 循环垃圾，
> 三档 A 默认 / B 32 MiB / C 1 TB，各 3 reps）执行；判据与数字见 §8.1。

---

## §6 复现

```bash
cd /home/alphayang/extC_Compiler
make BUILDDIR=build-emp -j                     # 只读 build-emp，不碰 build/

# 全量墙钟（G8 + G9 + G4 + G1，约 20 s；每次重跑都新建 out 目录）
OUT=tests/gc-p0/out/baseline-head-85b8871b REPS=3 tests/gc-p0/run.sh all
# G1 的 CPU 口径：追加到同一 OUT（clock=cpu 写进 pauses.jsonl；判据用它）
EXTC_RC_STEP_CPU=1 OUT=tests/gc-p0/out/baseline-head-85b8871b REPS=3 tests/gc-p0/run.sh g1cpu
# 单门禁
tests/gc-p0/run.sh g8      # g8|g9|g12|g13|g14|g15|g16|p03|peak|l10|stress|g4|g1|g1cpu|noise
tests/gc-p0/run.sh noise   # 空转基准（v2 同时给 wall 与 cpu 两口径），必须与 GC 同 OUT 同负载跑
# G13 单独跑（Lead m02258：doc-audit 第二段用，两档都打、期望均 d0r=7）：
tests/gc-p0/run.sh g13
# G4 正式验收（注入 4×L 循环垃圾，3 档×3 reps；写 $OUT/g4_stress.json）：
tests/gc-p0/run.sh stress
#   ↑ 直接调 python3 tests/gc-p0/stress.py --extc build-rf/extc --out tests/gc-p0/out/after --reps 3
#     （--live 100000 --garb 400000 --rounds 100 为默认）；注意 g4 子命令只是 G6/G7 代理，不是 G4。
# G14 的"before 红"要带 P0-7b 记账的构建（HEAD 对 copyInto 不发射调用点 ⇒ 平坦但无意义）：
OUT=tests/gc-p0/out/after REPS=3 EXTC=build-emp/extc tests/gc-p0/run.sh g14
# G15 已知限制 L10-a（XFAIL，预期 RED；不计入回归）：
OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh g15
# P0-3 权衡形状（观察项，不判绿；summarize.py 单列 P0-3 段）：
OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh p03
# 环境透传：EXTC_RC_PAUSE / EXTC_RC_STEP_CPU 由 run.sh 转给被测进程（后者同时决定 clock 标签）
# after 全套（**FREEZE v4 实际执行的命令**；见 §8 开头；CFLAGS 必须带 POSIX 宏）：
CFLAGS="-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L" \
  EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 INTERVAL=2 tests/gc-p0/run.sh all
EXTC=./build-rf/extc EXTC_RC_STEP_CPU=1 OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh g1cpu
# G4 正式验收（须在 all 之后单独跑，写同一 OUT）：
EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh stress
# L10 XFAIL 记录：
EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh l10
```

> **artifact 说明（2026-11-09）**：旧版 `run.sh` 在**任何**子命令开始时都清空 `runs.jsonl`/`pauses.jsonl`；
> 08:04 单跑 `noise` 时把基线的两个 jsonl 清空了（`csv/` 与 `log/` 原始证据未受影响）。
> 已修：只有 `WHAT=all` 才清空。基线的 jsonl 由 `/tmp/empirics/rebuild_jsonl.py` 从 `csv/`/`log/` 重算，
> 行内带 `"reconstructed_from_csv": true`；数值与 §4 一致（例：G8 T1/T2 = 45,448/2,956 kB，15.375×）。

单用例的等价手工命令（G8 为例，`run.sh` 内部就是这样做的）：

```bash
EXTC_RC_SWEEPFREE=1 EXTC_RC_NOCOUNT=0 ./build-emp/extc tests/gc-p0/src/g8_chain2.extc -o /tmp/g8_t1.c
cc -O2 -std=c11 -fwrapv -o /tmp/g8_t1 /tmp/g8_t1.c
/usr/bin/time -f 'maxrss=%M wall=%e' /tmp/g8_t1 2000000
```

输出目录内容：`c/`（生成的 C）、`bin/`（可执行）、`csv/`（2 ms VmRSS/VmHWM 时间序列）、
`log/`（stdout/stderr/编译日志/档位宏）、`runs.jsonl`（每次运行）、`pauses.jsonl`（每个 pause rep 的解析结果）、
`summary.txt`（本文件 §4 的表）。全部在 `tests/` 下，不在 `docs/`。

---

## §7 未测 / 存疑（如实列，不猜）

1. **M_max（已解决，保留历史）**：before 时 `M_max` 未实现 ⇒ G4 正式判据"4× 注入 ≤ M_max+8 MiB"**不可判**，
   §4.3 只能用 N vs 4N 的代理。after 已实现（FREEZE v4）：`[work]` 行给出 `m_max`/`M_max`/`M_max_now`/`pressure_steps`，
   且新增 `[mmax] memory wall unreachable: M_max=<v> floor=1048576; using floor` 警告（隐式墙早期 < floor 时
   打一次、**不 trap**——FREEZE v4 实测见 §8.2）；正式量具 `tests/gc-p0/stress.py` 已落地（§8.1）。
   **残留存疑**：`[work]` 的 `M_max_now` 在默认隐式档会打出 **72**（远小于同行的 `M_max=7,200,144`，
   也小于 floor=1,048,576），语义未核（疑为"最后一次快照时刻的 3·L 估计"而非生效墙 ⇒ 本文件一律用
   `M_max` 字段判，`M_max_now` 只记录）。
2. **G1② 的桶粒度偏薄（阈值问题已由 Lead 裁决为常数 1e-4）**：(a) before 无直方图（`src/back/reclaim.c` 只打 max/total + 4 桶）；**v4 起已换成 15 桶 `[histo]`**（§10），
   真实 >50 µs 占比只能夹在 [over64/n, over16/n] 里，§4.4 已按"下界>阈才 RED、上界≤阈才 PASS、否则 N/A"判；
   (b) "≤3× 空转"的阈值来自 `noise_floor` 在 n=200000 里只出现 0–3 次超阈事件 ⇒ 3× 阈在 **0（回退 1e-4）到 4.5e-05** 之间漂，
   统计上很薄：基线归档 7 reps 阈=4.5e-05（cyc/pc N/A、pb/pd RED），interim 归档 7 reps 阈=1.5e-05（四例全 RED），
   **同一份 GC 数据的结论会随空转 rep 翻转**——这正是要点，不能把"RED"当成 GC 的稳定属性。
   **建议**：P1-6 直方图落地后改用 p99.9 与精确占比；空转基准改 n≥1e6 并给 Wilson 区间；
   (c) pd 的每 rep 只有 ~432 步（after cpu 合并 1,296），1 步超阈就占 7.7e-4 > 1e-4 ⇒ 该用例上"比例判据"天然退化；
   Lead m01050 已裁决 **n<1e5 判 N/A + 报 max**（本文件照此执行，不再自行豁免）。
   **after 更新（v4，cpu 口径）**：pb 区间 [1.39e-4, 4.23e-4] 下界 > 阈 ⇒ **RED**（不是 N/A）；
   cyc [7.25e-5, 2.34e-4] 与 pc [3.64e-5, 1.46e-4] 均跨阈 ⇒ N/A；pd N/A(n<1e5, max=63.4 µs)。空转 cpu 口径 over50=1.50e-5（3 reps 最差）。
3. **pd 的 44 µs/step 已归因（见 §8.7）**：同一源、同一构建、只改生成物 `-DEXTC_RC_SCAN_CHUNK={8192,65536}`，
   65536 时 pd 200 次 8 MiB 对象 `[step]` total 28.6–31.5 ms、max 164–269 µs、over64 346–352/643；
   8192 时 total 4.50–4.51 ms、max 78–98 µs、over64 1–2/510 ⇒ 尾巴来自 **P0-1 大对象分段扫**
   （每步扫一个 SCAN_CHUNK 段；8 MiB 对象 1,048,576 字 = 128 段 vs 16 段）。
   **残留存疑**：大 chunk 档 `collects` 25 vs 小 chunk 档 7（3.6×）、total 6.4×，分配字节完全相同
   ⇒ 触发频率差异机制未定位（Lead 记为存疑 R13）。after（v4）的 pd cpu：mean=10.530 µs、max=63.4 µs、over1ms=0（3 reps 合并 1,296 步）。
4. **墙钟噪声**：WSL2 上 pb 1.351–1.765 s（±31%）；本文件所有内存结论不受影响（VmHWM ±8 kB），
   但**不得**用本文件的墙钟做吞吐回归判据。
5. **ASAN 覆盖有限**：G9 基线在 trap 处退出，ASAN 未执行"collect 之后用已释放帧"的访存路径；
   patch 后必须重跑 ASAN（`run.sh` 已自动做），并额外确认 `live0=live1=1、freed=0` 才算 G9 变绿。
6. **G9 根因的前提未逐点复核**："`extc_task_rootPush` 发射器零处、`extc_taskRootN` 恒 0"来自
   `docs/analysis/GC-ROOFLINE.md:432` 的断言；本轮未逐点搜发射器（成本高）。但基线 trap 行为本身
   已证明"保护不存在"，与前提方向一致。
7. **G8 的 45.4 MB 跨轮复现但只在默认 -O2 下**：未测 -O0/-O3、也未测不同 N 的斜率（只测 N=2M 单点）；
   若 patch 后仍红，需要斜率证据才能区分"常数漏"与"每轮漏 1 个"。
8. **采样间隔**：2 ms 对 G8/G9（~20 ms）只有约 10 个点；峰值靠 VmHWM 单调性 + time 交叉，
   g8_t2 采样 2,956 vs time 3,024（2.3%），已按较大值（time）校准比率。
9. **`tests/reclaim-shapes` 的 `promote-place` 行是 pre-existing 红（不是本轮回归）**：本机 HEAD
   `EXTC=./build/extc tests/reclaim-shapes/run.sh` 输出
   `promote-place ok ok 1.45 MB 红 生成物里看不出逃逸的那份归到外层区（kept=a[], doomed=a[], loop_release kept=0 doom=0）`；
   compile-facts 在隔离基线上同样红。根因是**断言腐烂**：`tests/reclaim-shapes/run.sh:136-138` 用
   `grep -o '__extc_a\[[0-9]*\]'` 取 arena 下标，而当前 codegen 在分配站点发射的是 region 句柄
   `__extc_rcr<N>`（本机生成物 `/tmp/empirics/pp/pp.c:3231`：`node * doomed = ((node *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(node), &__extc_rct0));`），
   `__extc_a[4]` 只剩声明（同文件 :3217），于是 kept/doomed 都取空 ⇒ 红。`tests/reclaim-shapes/README.md:27`
   声称"绿"已过时。after 报告按 **pre-existing** 记录，不计入本轮回归。
10. **G14（已解决，保留历史）**：after 在 FREEZE v4 上 PASS（§8.0）。下面是 before 期的构建断链记录——
    唯一测得 6.68× 的隔离基线
    `/tmp/mine2-src/build-mine2/extc`（md5 `a9b391cf9e44e84cb2b183db2ec7616c`）已于 08:36 被覆盖：
    新二进制 md5 `f61c8da5c9711b1ffec23b22e289eb7e`，其 codegen 已发射 `extc_rc_storeStructCopy`
    而配套 `src/back/reclaim.c` 尚无该符号 ⇒ cc 报
    `implicit declaration of function 'extc_rc_storeStructCopy'`，产物**链接失败**。
    HEAD `build/extc`（md5 `f4a0a9b6…`）对 `copyInto` **不发射调用点**（生成物只有
    `extc_rc_storeStructCons/Rooted` 的 unused 定义），实测 50k 2,636 kB / 800k 3,024 kB = **1.147×**，
    这是"没有记账"的平坦，**不是有界性证据**；`run.sh` 的 G14 段会打印
    "产物中 `extc_rc_storeStruct*` 出现 0 次"的告警把这一点写死。`summarize.py` 的 RED 分支已用
    伪造 timemax（2,636 / 17,616 kB）验证输出 `ratio = 6.683x [RED]`，**只验证了判据代码，不是测量**。
    runtime-facts 把 `extc_rc_storeStructCopy` 落进 reclaim 运行期后，after 第一批必须重跑 G14 拿红/绿。
    **结果**：FREEZE v4 上 G14 记账调用点 2 次、对照比 1.000×、800k=3,228 kB ⇒ **PASS**（判据 (b)，§5）。
11. **P0-3 权衡形状（已测，与 Lead 预期相反）**：`tests/gc-p0/src/p03_ring_release.extc`（10 轮 × 400k 结点环）
    已接入 `run.sh p03`（`summarize.py` 单列，**不判 PASS/RED**）。HEAD 本机实测峰值
    **41,616 kB（40.6 MB）/ 0.03 s**（与 Lead 报的 HEAD 45.8 MB 同量级，差异来自形状是我方等效环，
    非 runtime-facts 原始脚本）。**after（v4）实测 18,164 kB = 17.7 MB（3 reps，VmHWM median）**——比 HEAD 的
    40.6 MB **更小**，不是 Lead 预期的 ~107 MB（2.3×）。⇒ 在该等效形状上 **没有观察到** releaseOld 预算耗尽的代价；
    107 MB 那组数字来自 runtime-facts 自己的脚本（形状不同），两者不可直接互证，**记录不判**。
    **Lead 追问的 93,080 kB 出处（本块补）**：该数**不是**本文件的测量，出自 `docs/analysis/MEMORY-MODEL-CURRENT.md:853`（runtime-facts 的单次手工观测，日志未归档，原文自标「待复测」；doc-audit `AUDIT-GC-P0.md:384/661` 已把它标为 C 类不可追溯）。本机同形状快速对照（`tests/gc-p0/out/after/bin/p03_ring_release 10 400000`，1 rep，`/usr/bin/time -f %M`）：默认档 **18,008 kB**、`EXTC_RC_M_MAX=33554432` **42,392 kB**、`67108864` **47,768 kB**、`1099511627776` **47,768 kB** ⇒ 即使关墙也到不了 93 MB；93,080 应来自 interim 构建（其 `[work] M_max=103,125,744`、`thr=35,546,880`、`pressure_steps=1209`、`collects=13`）的不同自学习状态，**与本轮 after 不能互证**。交付文档 §18.5 建议改用本报告 §8.2 的 G17 可追溯数字，或在该处标注「旧构建单次观测、日志未归档」。
12. **`[pause]` 在 strict-ANSI 构建下不是墙钟（缺陷，已定位/已绕开）**：生成物不自己定义特性宏，而
    `-std=c11` 是 strict ANSI，glibc 因此不暴露 `CLOCK_THREAD_CPUTIME_ID`/`CLOCK_MONOTONIC`；
    运行时的 `#if defined(CLOCK_THREAD_CPUTIME_ID)` 为假 ⇒ `extc_rc_cpuNow()` 恒返回 -1
    （生成物 `cyc.c:2707-2716`）⇒ `[step]`/`[histo]`/`[top]`/`[chan]` 时长**恒 0**（G1② 假 PASS）；
    `pauseNow` 回退 `clock()*1e9/CLOCKS_PER_SEC`（同文件 `:2696-2705`）⇒ 旧构建的 `[pause]` 实际是
    **进程 CPU 时间（µs 粒度）**，不是墙钟。本机验证：同一份 `cyc.c` 只加 `-D_POSIX_C_SOURCE=200809L`
    就得到非零 `[step]`（cpu 口径）。**处置**：`run.sh` 的 CFLAGS 默认加该宏（`tests/gc-p0/run.sh:40-45`），
    `[clock] step=cpu pause=wall chan=cpu` 行自证；运行时兜底（`cpuNow()<0` 时关 `_usecpu` 改墙钟、
    `[clock]` 写 `cpu=unavailable`）已随 v4 落地（Lead m02148(a)），但正式 after 走的是「CFLAGS 带 POSIX 宏」路径，
    该兜底分支**未构造 strict-c11 产物单独验证（存疑）**。**第一轮 after 因 G1 全 0 作废**。
13. **G17 双配置峰值：A<D<B≈C，不是 A<B<C（观察项）**：形状 `tests/gc-p0/src/peak_cfg.extc`
    （live=200,000 长链 + 60 轮×400,000 链垃圾）。峰值（/usr/bin/time %M，3 reps 完全一致）：
    A 默认 **22,240** / D 16 MiB **25,692–25,696** / B 64 MiB **70,240** / C 1 TB **70,236–70,240** kB（FREEZE v4 复测）。
    饱和原因：触发阈值 `thr` 先于 `M_max` 成为约束（B 的 `thr_max=66,519,044`、C 的 `thr_max=75,496,960`；
    B/C pressure_steps=0），**不是墙没生效**。A 档启动即打 `[mmax] memory wall unreachable: M_max=397056`
    floor=1048576; using floor`，随后 `thr_max=12,932,348`、`pressure_steps=7,220`。`
14. **L10-b / L10-c 已知限制（XFAIL，不进回归）**：FREEZE v4 实测
    L10-b `leak_copy_arena_dest.extc` 50k=2,648 kB / 800k=17,624 kB = **6.66×**；
    L10-c `leak_copy_rc_churn.extc` 50k=6,676 kB / 800k=93,788 kB = **14.05×**；
    两例均默认档线性泄漏（机制：L10-b 目的 view 由循环体 arena 分配 ⇒ `slabOfInterior(base)` 失败 ⇒
    noSweep；L10-c rsInc 边不撤销），修复方向 P0-9/P0-10，本轮**只记录、不判**（§8.4）。
15. **G16 回归与影子检查支持"只有一处同类残留"（但非全形状证明）**：旧 bug 是 slabNew 把扫掠中新建的
    slab prepend 到 `r->slabs` 头、alloc 标黑规则仍置黑 ⇒ 黑位无人清 ⇒ 下一轮 markStep dedupe 跳过子树
    ⇒ sweep 释放活对象（§9）。修复后 pb 100M 20.49 B/node / **2.90 s / 2,000,408 kB（v4）**；G16 N=2M PASS（wall_max 0.055 s）。
    影子检查（给生成物注入"每轮 markBegin 统计 `occ&mark` 置位数"，正确应恒 0）在 cyc 20M（476 轮）、
    pc 50M（2,288 轮）、pd 200（4 轮）上均 **stale_rounds=0 / maxStale=0**，而修复前 pb 的 begin 有
    `staleMarked=859` ⇒ 这些形状无同类残留；**未覆盖全部形状**，不能外推为"全局无残留"。
16. **G1③ 的"≤64 µs 上界"是桶推论，不是直方图**：`P(>64 µs) ≤ 1e-3` ⇒ p99.9 ≤64 µs（**before 口径**；v4 15 桶直方图落地后 cyc/pc/pb 三例 p99.9 已可直接判：
    **16 / ≤32 / ≤32 µs**，pd 落 32–64 桶 ⇒ N/A，见 §5 与 §8.0）；pb 为 56/409,035=1.37e-4、
    pc 17/164,736=1.03e-4，均 ≤1e-3，故两例 p99.9 必 ≤64 µs。但 50–64 µs 之间是否有步**不可判**
    （`[histo]` 只有 16/64/256/1ms 桶），因此"p99.9 ≤50 µs"仍记 N/A，**不得**用 64 µs 上界冒充通过。

17. **cyc 的 2,540 µs 单步：字面 RED 但复跑不复现（待 Lead 裁决）**：正式 after 的 `cyc_pause_r1`（默认档、cpu 口径）出现 1 步
    **2,540.0 µs**：`[histo] b256us=3 b4ms=1`、`[top] rank=1 kind=mark why=advance dur_us=2540.0 cpu_us=2540.0`
    words=0 seg=0 pops=256 coll=109 step=26255`。`words=0/seg=0/pops=256` 表示该步没有扫任何字、只弹了 256 次标记栈`
    （同形状正常量级 ≈100–200 µs），2.54 ms 与工作内容明显不符。同轮 r2/r3 的 over1ms=0（max 187.5 / 90.9 µs）。
    同二进制同负载（`cyc 20000000`）复跑：**cpu 口径 8/8 over1ms=0（max 104–226 µs）**、**wall 口径 8/8 over1ms=0（max 65–242 µs）**；
    空转基准 10 reps 的 cpu max 最大 135.6 µs，也没有 >1 ms 尖峰。⇒ 判为 WSL2 线程 CPU 时钟偶发尖峰（环境伪影）的可能性大，
    **但未定位**。因此本文件**不擅自剔除**该步：G1① 按字面记 **RED（1/537,750 步）**。**Lead m02795 裁决**：不许重跑 r1 换绿、不许记 N/A；保留 RED 并注明「16 次复跑不复现 ⇒ 疑环境尖峰，不作剔除」。**E16 补充（§8.8）**：另跑 20 reps（cpu 口径、单 rep），`over1ms` 总和 **0**（0/20），per-rep `[step] max` = 88.4 / 190.4 / 664.7 µs（min/median/max），20/20 的最坏步仍为 `mark/advance words=0` ⇒ **不复现**，按裁决维持 RED。
    另：`after/log` 里 `peak_D_r2`（4,119.7 µs）、`stress_B_r1`（4,048.2 µs）、`stress_C_r3`（4,087.6 µs）也有单步 >4 ms，
    但三个用例的 `[clock]` 都是 `step=wall`（未开 STEP_CPU，`cpu_us=-0.0`）⇒ 墙钟含抢占，不能与 cpu 口径互比，且它们不是 G1 判据用例。

---

## §8 after（FREEZE v4：2026-11-09 09:47 之后，`tests/gc-p0/out/after`）

> **正式 after 的执行条件（实际满足）**：所有人停手后串行跑；编译器 `build-rf/extc` md5
> `9fd0dcd861374668be3c74c342beac6f`（FREEZE v4；源码 `src/back/reclaim.c` `ff42799b…` 3607 行 /
> `src/back/codegen.c` `cf288544…` 15994 行 / `tests/reclaim/driver.c` `9aae9cb5…` 1216 行）；
> `CFLAGS=-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`（POSIX 宏根因见 §7-12）；
> `OUT=tests/gc-p0/out/after`、REPS=3、`INTERVAL=2`。
> ```bash`
> EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 INTERVAL=2 tests/gc-p0/run.sh all       # ① 墙钟 + G4/G8/G9/G14/G16
> EXTC=./build-rf/extc EXTC_RC_STEP_CPU=1 OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh g1cpu   # ② cpu 口径
> EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh stress     # ③ G4 正式量具
> EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh l10        # ④ XFAIL 记录
> ````
> 实际执行记录：① `PHASE all wall=57.04 s`、`ALL_RC=0`；② `G1CPU_RC=0`；③ `STRESS_RC=0`；④ 补跑。
> **②③ 判据用 cpu 口径**，wall 只作观察；`[clock]` 行自证 `step=cpu pause=wall chan=cpu`。
> **作废**：interim（`f97a425f…`，§8.6）、v1（pb 挂死）、v2（G1 全 0）、v3（旧 7 桶桶边界）都不算正式数字，
> v3 目录归档在 `tests/gc-p0/out/after-v3-superseded-0915Z/`。日志索引见 §8.3。
>
> **三条口径声明（Lead m02784/m02795，doc-audit 只读核对用）**：
> a) after 全部数字出自 `build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f` + `CFLAGS=-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L` + `REPS=3`（G4-Stress 为 3 档×3 reps；E16 为单 rep×20 次，见 §8.8）；
> b) `max` 只用 **cpu 口径**（`EXTC_RC_STEP_CPU=1`）且**不作判据**（判据是 ① over1ms=0、② >50 µs 占比、③ p99.9）；
> c) over1ms **以 `[histo]` 15 桶为权威**（严格 >1e6 ns，b4ms 起；legacy 7 桶只作历史存档，见 §10）。

### 8.0 填表规范（列名与口径固定，勿改）

| 列 | 含义 |
|---|---|
| `clock` | `wall` = CLOCK_MONOTONIC（含抢占）；`cpu` = EXTC_RC_STEP_CPU=1 的线程 CPU（GC 工作量） |
| `n` | 该用例 reps 合并后的 `[step]` 步数，并给 n/rep；n<1e5 时 ② 记 N/A 且必须报 max |
| `REPS` | 该门禁的重复次数（G1 默认 3；改过要在 after 行里写明） |
| 宏 | `EXTC_RC_NOCOUNT / EXTC_RC_SWEEP_FREE / EXTC_RC_SCAN_CHUNK / EXTC_RC_RELEASE_BUDGET / EXTC_RC_TIER` 的实际值（产物 `c/*.c` 头部） |
| 结论 | `PASS / RED / N/A(原因) / 不可判(原因)`；**N/A 不是 PASS**，不得在总结里算绿 |

| 门禁 | 判据（现行） | before | after（FREEZE v4） | Δ | 结论 |
|---|---|---|---|---|---|
| G8 | T1/T2 ≤1.10× | 15.375×（45,448 / 2,956 kB） | **0.479×**（1,572 / 3,284 kB） | −14.90× | **PASS** |
| G8-ASAN | 两侧 rc=0 且无报告 | 两侧 rc=0 无报告（T1 高于 T2） | **两侧 rc=0、无 ASAN 报告**，且 T1 峰值 1,572 < T2 3,284（0.479×） | — | **PASS** |
| G9 | 可驱动、任务数不变：stdout 全文 `live0=1 freed=0 live1=1 / got=20 steps=5 / tag=777`，exit 0 | exit 70，`live0=1 freed=1 live1=0`（trap） | **3/3 PASS**：stdout 全文 `live0=1 freed=0 live1=1` / `tag=777` / `got=20 steps=5`；exit 0、stderr 空 | 绿 | **PASS** |
| G9-ASAN | rc=0 且无报告 | rc=70（trap 先于访存） | **rc=0、无 ASAN 报告** | — | **PASS** |
| G4-a | pb 20±10% B/node | 20.49 | 20.49（time 交叉 2,000,408 kB） | 0 | PASS |
| G4-b | pc ≤4 MB 恒定 | 2.85 MiB | 3.17 MB（3,244 kB，100M） | +0.32 MiB | PASS |
| G4-c（正式） | 4× 注入 ≤M_max+8 MiB 且稳态、单调 | 不可判（M_max 未实现） | **PASS**（9,148 ≤ 15,223 kB；末 6 轮差 0.00%；A 9,148<B 41,704<C 93,432） | — | **PASS**（§8.1） |
| G4-c（旧代理） | cyc N vs 4N <1.5× | 0.9994× | 1.000×（6,048 / 6,048 kB） | 0 | 代理 PASS |
| G4-M_max | M_max 实际取值 + RSS/M_max | 不可判 | cyc `[work]` m_max=**4,101,120 B**；stress A 档 M_max=**7,200,144 B**（7,031 kB），peak/M_max=1.30× | — | 可判（§8.1） |
| G4-pressure | `[work]` pressure=1 次数 | n/a | 合计 **18**（cyc 1/pc 0/pb 1/pd 1，3 reps）；stress A 档 33/rep | — | 已观测 |
| G1① | over1ms = 0（cpu；v4 用 `[histo]` >1e6 ns 桶和） | 0（wall，12/12） | cpu **1/537,750**（cyc r1 单步 **2,540 µs**，words=0/seg=0/pops=256，复跑 16 次不复现，§7-17） | +1 步 | **RED（字面；16+20 次复跑不复现，疑环境尖峰；按 Lead m02795 不剔除、不改 N/A）** |
| G1② | >50 µs 占比 ≤1e-4（cpu；n<1e5 ⇒ N/A+max） | wall 口径 N/A | cpu：cyc N/A（[7.25e-5,2.34e-4] 跨阈）；pc N/A（[3.64e-5,1.46e-4] 跨阈）；pb **RED（下界 1.39e-4）**；pd N/A(n<1e5) | — | **RED（pb）+ N/A**（聚合 RED） |
| G1③ | p99.9 ≤50 µs（cpu，v4 15 桶） | N/A | cyc **16 µs** PASS；pc **≤32 µs** PASS；pb **≤32 µs** PASS；pd 落 32–64 桶 ⇒ N/A | — | **部分 PASS + pd N/A**（三例 PASS、一例桶界不可分；不得写整条 PASS） |
| G14 | 800k ≤1.02×对照且 ≤4 MB | 6.68×（2,636→17,616 kB，隔离基线 `a9b391cf…`） | 50k=2,652 / 100k=3,228 / 800k=3,228 / ctl=3,228 kB；**对照比 1.000×**、≤4,096 kB；记账调用点 2 | 6.68×→1.00× | **PASS** |
| G15 | 800k ≤1.20×50k；**XFAIL(known L10-a)** | 14.97×（12,420→185,932 kB） | **14.985×**（12,664→189,776 kB，默认 T2） | — | **XFAIL（预期 RED，不阻塞）** |
| G16 | N=2M 3 reps exit 0、未 timeout、输出正确、wall<5 s | n/a（旧 bug 下 1.5M 起挂死） | exit=[0,0,0]、timed_out=[F,F,F]、wall_max=0.055 s、cnt/sum ok | — | **PASS** |
| ASAN | G8/G9 无 UAF/双重释放 | G8 两侧 rc=0；G9 rc=70 | G8 两侧 rc=0 无报告；G9 rc=0 无报告 | — | **PASS** |

| 观察项（**非门禁**） | 口径 | before | after | 说明 |
|---|---|---|---|---|
| P0-3 权衡形状 | 10 轮×400k 环的峰值 VmHWM | 41,616 kB / 40.6 MB | **18,164 kB / 17.7 MB**（3 reps median） | after 反而比 HEAD 小，**未观察到** Lead 预期的 ~107 MB/2.3×（形状不同，见 §7-11），不判 |
| G17 双配置峰值 | 长命+churn，峰值随 M_max | 未测 | A 22,240 / D 25,696 / B 70,240 / C 70,236 kB | A<D<B≈C：thr 先于 M_max 约束，见 §7-13 |
| L10-b / L10-c | 已知限制线性泄漏 | 未测 | 6.66× / 14.05×（§8.4） | XFAIL，不进回归 |
| 影子检查 | 标记完整性（staleMarked） | pb 修复前 begin staleMarked=859 | cyc/pc/pd stale_rounds=0（§8.5） | 仅覆盖三形状 |

> **pre-existing 红的写法（Lead m01315）**：修复轮次之前就存在、且 HEAD 与隔离基线同样红的条目
> （已知：`tests/reclaim-shapes/run.sh` 的 `promote-place` 行，见 §7-9；`tests/reclaim-perf` 若也红照此办理）
> 一律标 **pre-existing**，不得算作本轮回归；只有「before 绿 → after 红」才是回归。

### 8.1 G4 正式验收：每轮注入 4×L 循环垃圾（`tests/gc-p0/stress.py`）

> Lead m02258：`run.sh g4` 只是 pb/pc/cyc 的 G6/G7 代理，**不能**当 G4（用户第一条硬要求「内存有界」）的验收；本节才是 G4 的验收量具。
> 本节 A/B/C 三档同时也是 Lead m01949① 要的「双配置 peak 对比」——按 Lead m02416(5) 合并进 stress.py，不再单列第二套实验。

**形状**（`tests/gc-p0/src/g4_stress.extc`）：先建 `live=100000` 个结点的长链（根 `w.live` 永不摘除）⇒ 活集 L；每轮再建 `garb=400000`（=4L）个结点，两两互指成 2-环，经 `fn makeRing(w: mut ref world, r: i64)` 逃逸到 `w.ring`（rc 区），轮末 `w.ring = null` 丢根 ⇒ 每轮 4L 全部成为不可达循环垃圾；每轮 stderr 打 `R <k>`；采集线程每 2 ms 读 `/proc/<pid>/status` 的 VmHWM，按轮号取**轮末**值 ⇒ 轮末峰值序列；共 100 轮；末尾遍历活集输出 `G4S live= rounds= garb= dropped= sum=`（sum 自证活集未被误回收）。

**命令**（3 档 ×3 reps；写 `tests/gc-p0/out/after/g4_stress.json`，原始 stderr 在 `log/stress_<cfg>_r<rep>.err`，时间序列在 `csv/`）：

```bash
EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh stress
```

工具：`build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f`（FREEZE v4）；`CFLAGS` 同 §8 开头；`EXTC_RC_PAUSE=1` 只作观测（取 `[work]` / `[mmax]` 证据），不影响峰值口径。

| 档 | `EXTC_RC_M_MAX` | `[work]` M_max（声明值） | VmHWM 峰值（3 reps） | peak/M_max | `pressure_steps` | `[mmax]` 警告 |
|---|---|---|---|---|---|---|
| A 默认（隐式 3·L） | 不设 | 7,200,144 B | **9,148 kB**（9148 / 9148 / 9132） | 1.30× | 33 | 无 |
| B 显式 32 MiB | 33554432 | 33,554,432 B | **41,704 kB**（41704 ×3） | 1.27× | 98 | 无 |
| C 关墙 | 1099511627776 | 1,099,511,627,776 B | **93,432 kB**（93432 ×3） | 0.00009× | 0 | 无 |

**判据与结果**（判据逐字见 §2.3 的 G4-c 行）：

- ① 默认档 VmHWM ≤ 声明 M_max + 8 MiB：`9,148 ≤ 7,200,144/1024 + 8,192 = 15,223 kB` ⇒ **PASS**；
- ② 稳态（第 k 轮与第 k+1 轮差 ≤2%、不单调涨）：末 6 个轮末间隔相邻差 max = **0.00%**，末 8 轮 R93–R100 **全为 9,148 kB** ⇒ **PASS**；
- ③ 次序 A<B<C 或 A≈B<C：**A 9,148 < B 41,704 < C 93,432 kB** ⇒ **PASS**（墙真正约束峰值，不是 A≈B≈C）。

⇒ **G4-Stress：PASS**；`stress.py` 退出码 0，`g4_stress.json` 的 `verdict` 字段同为 PASS。

**四点方法论保留**：

1. **垃圾必须逃逸到 rc 区才有 GC 行为**。第一版把 2-环结点放在循环体局部（不逃逸到函数级根），循环体词法层 arena 在轮末 `extc_arena_release` 直接 bump 回收 ⇒ 三档峰值全 6,112 / 5,920 kB 恒定、`pressure_steps=0`、A≈B≈C。**测 GC 的形状必须让对象逃逸**（本例用 `makeRing` 写 `w.ring`）。
2. **本形状 A 档不出现 `[mmax]` 警告**：live=100k 时隐式墙 `M_max=7,200,144 B` > floor 1 MiB；只有启动早期 L 很小时（如 `[mmax] memory wall unreachable: M_max=397056 floor=1048576; using floor`）才警告一次并把下界抬到 floor，**不 trap**（§7-13、§8.2）。
3. **`pressure_steps` 不是判据**：C 档为 0 是 1 TiB 墙不可达；B 档 98 是 32 MiB 墙下扫掠更频繁。它只说明「墙被触发过」。A 档 33 说明默认档确实进过压力路径。
4. **`A≈B<C` 是可接受的合理现象，不是失败**（Lead m02523）：峰值不一定顶到 M_max——触发阈值 `thr`（自学习 scale ≈ 上轮存活字节 × 倍数）可能先于 M_max 成为约束。§8.2 的 G17 就是实例：显式 64 MiB 与 1 TiB 两档峰值都是 70,240 kB（`thr_max` 饱和在 66.5–75.5 MB，远低于墙）。因此判据只排除 **A≈B≈C**（墙完全没起约束）；`A≈B<C` 应解释为「墙不是唯一约束」，`stress.py` 的 `judge()` 与顶部 docstring 已写死该说明。

### 8.2 G17 显式 M_max 与峰值（长命 + churn；**观察项，不判 PASS/RED**）

> 目的：给 MEMORY-MODEL §18.5 一组**可追溯**的「峰值随 M_max 变化」数字。形状与 §8.1 不同：这里是**长命活集 + 每轮 churn**，用来观察 `thr` 自学习与 `M_max` 谁先成为约束。

形状 `tests/gc-p0/src/peak_cfg.extc`：先建 `live=200000` 的长链（永不摘），再 60 轮各造 400000 个结点链并丢根（每轮 `drop=null`）；每轮 stderr 打轮号；stdout `G17 live= rounds= garb= dropped= cnt= sum=`。

```bash
EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh peak
```

| 档 | `EXTC_RC_M_MAX` | VmHWM median | `[work] thr_max` | `[work] M_max` | 隐式? | `pressure_steps` |
|---|---|---|---|---|---|---|
| A 默认 | 不设 | **22,240 kB / 21.7 MB** | 12,932,348 | 14,108,016 | 1 | 7,220 |
| D 显式 16 MiB | 16777216 | 25,696 kB / 25.1 MB（25504–25696） | 16,187,396 | 16,777,216 | 0 | 4,676 |
| B 显式 64 MiB | 67108864 | 70,240 kB / 68.6 MB（70236–70240） | 66,519,044 | 67,108,864 | 0 | 0 |
| C 关墙 | 1099511627776 | 70,236 kB / 68.6 MB（70236–70240） | 75,496,960 | 1,099,511,627,776 | 0 | 0 |

`/usr/bin/time -f %M` 每档每 rep 的 maxrss 见 `timemax_peak.txt`（A 三 rep 全 22,240；D 25,696/25,504/25,696；B 70,236/70,240/70,240；C 70,236/70,236/70,240）。

**结论（如实）**：次序是 **A < D < B ≈ C**，**不是** Lead 判据里的严格 A<B<C。

- 显式墙 ≤64 MiB 时峰值随墙上升：21.7 → 25.1 → 68.6 MB；
- B 与 C **饱和在 70,236–70,240 kB**，原因**不是墙没生效**，而是**触发阈值 `thr`（自学习 scale）先于 `M_max` 成为约束**：C 档 `thr_max=75,496,960`（≈72 MB）远小于 1 TiB 墙，分配按 thr 触发回收，峰值自然停在 thr（+churn 缓冲）量级；B 档 `thr_max=66,519,044` 同样 < 64 MiB 墙；
- A 档启动时出现一次 `[mmax] memory wall unreachable: M_max=397056 floor=1048576; using floor`（隐式 3L 早期小于 floor），运行时抬到 floor 并继续增长到 14.1 MB。

### 8.3 日志与数据索引（`tests/gc-p0/out/after/`）

| 路径 | 内容 |
|---|---|
| `summary.txt` | `summarize.py` 全文（门禁表 + G17 + L10 + G1 + [chan]） |
| `g4_stress.json` | G4-Stress 结构化结果（`args` / `runs` / `verdict` / `detail`） |
| `timemax.txt` | `/usr/bin/time -f %M` 的 maxRSS 交叉（pb/pc/cyc/g8/g14/g15/l10b/l10c） |
| `timemax_peak.txt` | G17 三档每 rep 的 `peak <cfg> <rep> <maxrss_kB> <wall_s>` |
| `pauses.jsonl` | 每个 `EXTC_RC_PAUSE=1` 运行一行解析结果（含 `clock`、`clock_fields`、`histo`、`work`、`chan`） |
| `runs.jsonl` | 每次运行的 VmHWM / wall / exit / `tag`（`tag=pause` 为 G1 负载） |
| `noise.txt` | 空转基准（wall 与 cpu 双口径，3 reps） |
| `log/` | **196 个文件**（80 个 `.err` + 71 个 `.out` + 19 个编译日志 + defines 等）：`<case>[_<variant>]_r<rep>.err` / `.out`、`<case>.compile.log`、`<case>.defines`；pause 变体为 `<case>_pause_r<rep>.err` |
| `csv/` | 56 个 `<case>[_pause]_r<rep>.csv`（`t_ms,rss_kb,hwm_kb`，2 ms 采样）；G4-Stress 为 `stress_<cfg>_r<rep>.csv` |
| `c/`、`bin/` | 生成物 `.c` 与可执行文件（宏自证看 `c/*.c` 头部 `#define EXTC_RC_*`） |

常用证据（原始 stderr）：

- 口径自证：`[clock] step=cpu pause=wall chan=cpu`（若 strict-ANSI 下 cpu 不可用，v4 会打 `cpu=unavailable` 并退回墙钟）；
- 步长/桶：`[step] n=… max_us=… total_us=… over16=… over64=… over256=… over1ms=…`、`[histo] n=… b1us=…`；
- 最坏步归因：`[top] rank=1 kind=mark why=advance dur_us=151.7 cpu_us=151.7 … chunk=8192`；
- 工作量与墙：`[work] … thr_max= … mmax= … scale_max= … pressure_steps= … M_max= … M_max_now= … M_max_implicit= …`；
- 通道：`[chan] name=regionfree n=1 max_us=76701.3 total_us=…`（按 case+clock+name 合并在 `summary.txt`）。

```bash
grep -h clock tests/gc-p0/out/after/log/pb_pause_r1.err
grep -h work  tests/gc-p0/out/after/log/stress_A_r1.err
grep -h chan  tests/gc-p0/out/after/log/pb_pause_r1.err
```

> **数据目录状态（v4 定版）**：`out/after/` 已按 FREEZE v4 从空目录重建（无 smoke 残留），`summary.txt` 140 行；v3 旧目录整体归档为 `out/after-v3-superseded-0915Z/`。所有 after 数字以本目录 `summary.txt` / `g4_stress.json` / `timemax*.txt` / `pauses.jsonl` 为准。

### 8.4 L10-b / L10-c 已知限制（**XFAIL/known-issue，不进回归**）

两条都是默认档（T2）线性泄漏，只记录 `maxRSS(800k)/maxRSS(50k)`，不判 PASS/RED（Lead m01688）：

| 用例 | 机制 | 50k | 800k | ratio | 状态 |
|---|---|---|---|---|---|
| `leak_copy_arena_dest.extc`（L10-b，P0-9） | 目标 view 由**循环体 arena** 分配 ⇒ `slabOfInterior(base)` 失败 ⇒ 退回 `extc_rc_storeStructCons(hostKnown=0)`，把源 region 置 `noSweep`（`src/back/reclaim.c:2158`：`else if (rv) rv->noSweep = 1;`，无清除点）⇒ 该 region 永不扫 ⇒ 线性泄漏 | 2,648 kB | 17,624 kB | **6.66×** | XFAIL(known L10-b) |
| `leak_copy_rc_churn.extc`（L10-c，P0-10） | 目标 view 在 rc，但每轮被 `copyInto` **覆盖** ⇒ 旧值的 `rsInc` 边不撤销 ⇒ 源结点永远像被引用 ⇒ 线性泄漏 | 6,676 kB | 93,788 kB | **14.05×** | XFAIL(known L10-c) |

```bash
EXTC=./build-rf/extc OUT=tests/gc-p0/out/after REPS=3 tests/gc-p0/run.sh l10
```

> 行号说明：`hostKnown=0` 分支落点是 `src/back/reclaim.c:2144-2158`；L10-c 的「`rsInc` 边不撤销」来自 doc-audit 的源码追踪，本机**只验证了数字**，撤销点具体行号**未逐行核对（存疑）**。

### 8.5 影子检查：标记完整性（查 pb bug 同类残留）

**方法**：`/tmp/empirics/shadow/patch.py` 对 FREEZE v3 的 `build-frz` 生成的 `.c` 注入只读探针（修复已进 v4 源码，逻辑不变）——每轮 `extc_rc_markBegin` 进入前遍历所有 slab 的每个 word，统计 `popcount(occ & mark)`（正确实现下必须恒 0，因为上一轮 sweep 应清掉所有被扫过的 mark 位），非 0 时打印涉及 slab 的 `swEpoch/nslots/ss/bump`；进程退出打 `[dbg] SUMMARY rounds=… stale_rounds=… maxStale=…`。**只改 /tmp 生成物，不碰源码。**

| 形状 | 轮数 | `stale_rounds` | `maxStale` |
|---|---|---|---|
| cyc（cyclic_pressure 形状，N=20M） | 476 | **0** | 0 |
| pc（环垃圾，N=50M） | 2,288 | **0** | 0 |
| pd（200×8 MiB 大对象） | 4 | **0** | 0 |
| pb（对照：修复**前**） | — | — | `begin staleMarked=859`、`finish unmarked=526,836` |

⇒ 三个形状**没有**第二处同类残留；pb 的 859 / 526,836 是 §9 那个已修 bug 的直接证据。

**局限**：只覆盖 cyc/pc/pd/pb 四个形状，**不是全形状证明**；g12/g13/g15 等小形状未单独做影子检查（其修复后行为由门禁数字覆盖）。

### 8.6 中间版 after（**interim，非正式**：2026-11-09 08:07 构建，patch 仍在工作树）


> 不是正式 after：当时 `src/back/reclaim.c`/`codegen.c` 仍有未完成的改动（`M_max` 未实现），
> 且机器上同时有别的 agent 在 `make -j24`。**正式数字以 Lead 在所有人停手后串行重跑的
> `tests/gc-p0/out/after` 为准**；本节只证明 patch 方向对、门禁可判定。
> 编译器 `build-emp/extc` md5 `f97a425f9a772daae156a29185790edf`（08:07:07），
> 原始数据 `tests/gc-p0/out/after-interim/`。

本中间版落到工作树的关键机制（行号为当时工作树，不是提交）：

- 任务根登记：`g->taskRoots = 1` 默认开（`src/back/codegen.c:13978`；`=0` 逐字节恢复旧产物，`src/back/codegen.c:13975-13981`），
  发射 `extc_task_rootPush`/`pop`（`src/back/codegen.c:6152` 一带的 `cgTaskRootPush`/`cgTaskRootPopAt`）。
- 级联释放：`extc_rc_freeSlotCascade` 在 hold 归零时当场回收（`src/back/reclaim.c:1444-1446`、`:1521`、`:1537`）。
- `EXTC_RC_SCAN_CHUNK` 65536 → **8192**（`src/back/reclaim.c:2347`）。
- 新增 `EXTC_RC_RELEASE_BUDGET 4096`（`src/back/reclaim.c:901-902`）、`EXTC_RC_TIER` 自证宏（`src/back/codegen.c:12518`）。

| 门禁 | 判据 | before | interim after | Δ | 结论 |
|---|---|---|---|---|---|
| G8 | T1/T2 ≤1.10× | 15.375×（45,448 / 2,956 kB） | **0.527×**（1,560 / 2,960 kB；time 交叉 1,484 / 3,024） | −14.85× | **PASS** |
| G8-ASAN | 两侧 rc=0 且无报告 | 两侧 rc=0 无报告 | **T1 峰值比 T2 低（0.527×）且两侧 rc=0、无 ASAN 报告** | — | **PASS** |
| G9 | 可驱动、任务数不变 | exit 70，live 1→0 | **3/3 PASS**，stdout 全文：`live0=1 freed=0 live1=1` / `tag=777` / `got=20 steps=5`，exit 0，stderr 空 | 绿 | **PASS** |
| G9-ASAN | rc=0 无报告 | rc=70（trap 先于访存） | **rc=0、无 ASAN 报告** | — | **PASS** |
| G4-a | pb 20±10% B/node | 20.49 | 20.49 | 0 | PASS |
| G4-b | pc ≤4 MB | 2.85 MiB | 2.85 MiB | 0 | PASS |
| G4-c | 4× ≤M_max+8 MiB | 不可判（代理 0.9994×） | 不可判（代理 0.999×） | — | 不可判（`M_max` 未实现） |
| G1① | over1ms=0 | 0 | 0（12/12） | 0 | PASS |
| G1② | >50 µs 占比 ≤1.5e-5（当时口径与阈值，**已被 §8.0 修订取代**） | — | **RED**（四例下界全超过阈） | — | RED |
| G1③ | p99.9 ≤50 µs | — | **N/A**（当时无直方图） | — | N/A |

> **表到此截断**：interim 当时只跑了这些门禁（G12–G17 尚未建立）。interim 的 G1 用 wall 口径、阈值 1.5e-5，
> 与正式 after 的 cpu 口径 + 常数 1e-4 不同，**不可混比**；正式数字见 §8.0。
### 8.7 pd 尾巴归因：SCAN_CHUNK 8192 vs 65536（**归因成立**）

问题：before 里 `pd`（200×8 MiB 大对象）`[step]` 有 341–353/643 步 >64 µs、mean 44.16 µs、max 173 µs，尾巴来自哪里？

**方法**：同一份源 `/tmp/empirics/pd-attr/pd.c`、同一个编译器（`build-emp/extc` md5 `f97a425f9a772daae156a29185790edf`），**只改生成物**的 `-DEXTC_RC_SCAN_CHUNK={8192,65536}`；各 3 reps，`EXTC_RC_PAUSE=1`，`n=200`（**必须带 argv 参数 200**，源里默认 20）。

| `SCAN_CHUNK` | `[step] n` | `over16` | `over64` | max | total | collects | mean |
|---|---|---|---|---|---|---|---|
| 8192 | 510 | 6–7 | 1–2 | 78–98 µs | 4,504–4,508 µs | 7 | 8.8–8.9 µs |
| 65536 | 643 | 355 | 346–352 | 164–269 µs | 28,572–31,453 µs | 25 | 44.4–48.9 µs |

65536 档与 before（341–353/643、44.16 µs、173 µs）**同族** ⇒ 尾巴的机制是 `EXTC_RC_SCAN_CHUNK`。

**机制**（生成物逐字）：每个 mark 步走 `size_t en = st + (size_t)EXTC_RC_SCAN_CHUNK;` 字的段。8 MiB 对象 = 1,048,576 个指针字 ⇒ chunk=8192 需 **128 段**、chunk=65536 只需 **16 段**；实测每步上界 ≈ **3.4 ns/word**（8192: 34 µs；65536: 219 µs）。

**为什么 `[work]` 看不出来（重要教训）**：两档 `[work]` **完全相同**（8192: n=403；65536: n=402；`walk_max=1`、`scan_max=0`、`push_max=0`、六个 gt 桶全 0）。`[work]` 的 `scan` 只统计 seed 阶段 rsbit 字数；大对象分段续扫走 `extc_rc_markResume()`，其中只 `pops++`、不增 `words`（`words++` 只在 seed 循环），`[step]` 记录处 `if (_hot) extc_rc_workNote(marked, words, pushes);` ⇒ 219 µs 的 advance 步是 `pops=1 words=0`。**`scan_max` 相同不能推出段成本相同**，要靠 `[histo]`/`[top]`：8192 档 `[histo]` max 34–37 µs、b16us=385–389、b256us=0；65536 档 max 205–219 µs、b256us=353–355，`[top]` 全为 `kind=mark why=advance dur=133–219 µs`。

**线性扫（n=200/400/800，分配字节完全相同）**：

| chunk | steps | total | collects |
|---|---|---|---|
| 8192 | 510 / 1,036 / 2,088 | 4.52 / 8.91 / 18.3 ms | 7 / 13 / 25 |
| 65536 | 643 / 1,285 / 2,569 | 28.7 / 57.8 / 116.7 ms | 25 / 47 / 91 |

steps 只差 1.2–1.26×，但 collects 差 **3.6×**、total 差 **6.4×** ⇒ **「每秒 GC 工作量」不是不变量**，停顿契约只能按**每步**承诺（≤ C·`SCAN_CHUNK`，C≈3.4 ns/word），不能按每秒工作量承诺。

> 存疑（R13）：collects 差 3.6× 的机制**未定位**（自学习 scale 与 chunk 的耦合？），只记录现象。
> 原始数据：`/tmp/empirics/pd-attr/{pd.c,pd_def,pd_chunk64k}`。
> 后续：`SCAN_CHUNK` 默认已改 8192（FREEZE 系列），`tests/reclaim-perf/scanchunk.extc` 用 `-DEXTC_RC_SCAN_CHUNK=1048576` 可复现单步 1,010–1,044 µs、`over1ms=1`。

### 8.8 G1① 的红是否可复现（E16，Lead m02795 裁决 2）

**问题**：正式 after 的 `cyc_pause_r1`（§7-17）出现 1 步 2,540.0 µs ⇒ G1① 记 1/537,750。需判定这是可复现缺陷还是环境尖峰。

**方法**（只跑 cyc、单 rep × 20 次；不重跑门禁、不改数据）：

```bash
mkdir -p tests/gc-p0/out/after/cyc-spike
for i in $(seq 1 20); do
  EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 tests/gc-p0/out/after/bin/cyc 20000000 \
    >/dev/null 2>tests/gc-p0/out/after/cyc-spike/r$i.err
done
```

产物：`tests/gc-p0/out/after/cyc-spike/r1..r20.err`（20 个原始 stderr）+ `worst_top.txt`（每个 rep 的 `[worst]`/`[top]` 行摘录）。

**结果**：

| 指标 | 值 |
|---|---|
| `over1ms` 总和（20 reps，histo 权威） | **0**（0/20 rep 出现 >1 ms 步） |
| per-rep `[step] max_us` min / median / max | **88.4 / 190.4 / 664.7 µs** |
| 20/20 rep 的最坏步形态 | `kind=mark why=advance words=0 seg=0`、`pops`=256–1024（与正式 r1 的 2,540 µs 同类形态） |

**结论**：**不复现**（0/20；20 次最坏只到 664.7 µs）。按 **Lead m02795 裁决 1**：不重跑 r1 换绿、不记 N/A，G1① **保留 RED(1/537,750)** 并注明「16 + 20 次复跑不复现 ⇒ 疑 WSL2 线程 CPU 时钟尖峰或一次未归因的无线程工作量步；**不作剔除**」。

### 8.9 产物档位宏自证（G8/G9/G12/G13，Lead m02795 表）

用 **`build-rf/extc`（md5 `9fd0dcd8…`）只编译不运行**，取产物头部 `#define` 的最终值（`grep -E '^#define EXTC_RC_(NOCOUNT|SWEEP_FREE|TIER) '`）。每用例各编两档：**默认** 与 **`EXTC_RC_SWEEPFREE=1`**（G9 用其真实源路径 `tests/zone/`）。

| 用例（源） | 编译期 env | `EXTC_RC_TIER` | `EXTC_RC_NOCOUNT` | `EXTC_RC_SWEEP_FREE` | 对应门禁数字 |
|---|---|---|---|---|---|
| `g8_chain2`（`tests/gc-p0/src`） | 默认 | **2** | 1 | 0 | G8 的 **T2**（3,284 kB） |
| `g8_chain2` | `EXTC_RC_SWEEPFREE=1` | **1** | 0 | 1 | G8 的 **T1**（1,572 kB；run.sh 另加 `EXTC_RC_NOCOUNT=0`，宏值同上） |
| `collect_with_live_task`（`tests/zone`） | 默认 | — | — | — | G9（产物**不发射**回收运行期，见下） |
| `collect_with_live_task` | `EXTC_RC_SWEEPFREE=1` | — | — | — | 同上 |
| `uaf_param_overwrite`（`tests/gc-p0/src`） | 默认 | **2** | 1 | 0 | G12 默认档（观测，PASS-by-design） |
| `uaf_param_overwrite` | `EXTC_RC_SWEEPFREE=1` | **1** | 0 | 1 | —（非门禁档；仅证 `SWEEPFREE=1` 自动升 T1） |
| `uaf_bulk_copy`（`tests/gc-p0/src`） | 默认 | **2** | 1 | 0 | G13 **默认档**（`d0r=7`，S0 可达） |
| `uaf_bulk_copy` | `EXTC_RC_SWEEPFREE=1` | **1** | 0 | 1 | —（同上） |

补充 1：门禁实际用的**强制计数档 = T3**（`EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0`）——`uaf_param_overwrite` 与 `uaf_bulk_copy` 两例均得 **`EXTC_RC_TIER=3 / NOCOUNT=0 / SWEEP_FREE=0`**（`build-rf` 实测；对应 §5 的 G12/G13 判据）。G8 的 T1 在 run.sh 里是 `SWEEPFREE=1 + NOCOUNT=0`，宏值与上表 `SWEEPFREE=1` 行相同。

补充 2（新发现，供 doc-audit 只读核对）：**G9 的"档位"不适用**——`tests/zone/collect_with_live_task.extc` 只用 `syspool`/`coroutine`（无 `new`/引用逃逸），产物里 `extc_rc_` 记号 **0 处**、三个宏都不发射 ⇒ `log/g9.defines` 为空；G9 的判据是任务帧完整性与 `collect()` 行为，与 GC 档位无关。

---

### 8.10 G2 / G3 判据行（doc-audit D1 补齐；`[chan]` 与 `[regionfree]` 的原始口径）

§8.1–§8.9 覆盖了 G4/G8/G9/G12/G13/G14/G16/G1①，**G2（`collect`/`account` 单次成本）与 G3（`regionFree` 次数与时间）此前只出现在 GC-ROOFLINE §7，本报告缺判据行**（D1）。此处补齐，数字全部取自 `tests/gc-p0/out/after/pauses.jsonl`（`[chan]`/`[regionfree]` 合并 3 reps）与本轮**独立复核**（`AUDIT-GC-P0.md` §18.2，`build-da`）。

| 门禁 | 判据（v4 定稿） | after（`build-rf`，3 reps） | 独立复核（`build-da`） | 判定 |
|---|---|---|---|---|
| G2 | `collect`/`account` 单次 **p99.9 ≤ 200 µs**（n ≥ 1e3，cpu 口径；2302 样本的单次 max 不作契约，见 GC-ROOFLINE §8.1 R6） | cyc collect **p99.9 ≤ 64 µs**；max 记录 cyc 215.3 µs（n=2202）、pb 14.3、pc 34.8、pd 21.6 µs；>128 µs 样本仅 3/4404 | cyc collect **max 91.2 µs**（n=2202）；另 10 独立 reps 16.6–51.0 µs | **PASS（p99.9）**；旧 max 判据的 RED 不可审计 |
| G3a | `regionFree` 单次 free 次数 ≤ `10·#slabs + #classes + 3`，且只在帧销毁/退出 | cyc 517≤573、pc 156≤173、pb 110190≤122433（12243 slabs）、pd 1331≤1333（133 slabs） | 同向一致（逐项相同） | **PASS（计数）** |
| G3b | `regionFree` 单次 **cpu ≤ 1 ms**（停顿契约硬项；cpu 与 wall 都记） | pb **cpu 86.2 ms / wall 74.8–82.4 ms**（n=3）；pd cpu 27.5 / wall 22.0–25.8 ms；cyc 0.67/0.61 ms；pc 0.22/0.15 ms | pb **cpu 99.0 / wall 94.3 ms**；pd 29.1/29.1；cyc 0.67/0.61；pc 0.22/0.15 ms | **RED（本轮最大未闭合项）** |

**口径澄清（重要）**：① `[chan]` 的时钟**跟 `[step]` 走**——设 `EXTC_RC_STEP_CPU=1` 且 `CLOCK_THREAD_CPUTIME_ID` 可用时 `extc_rc_chanClock()` 返回 cpu 时间（`src/back/reclaim.c:1137`、`:1056-1059`、`:3198-3218`），生成物 `[clock] step=cpu pause=wall chan=cpu` 自证；`src/back/reclaim.c:1130-1131` 的注释写「Channels are timed with the wall clock, like `[pause]`」**已过时**（doc-audit C4，冻结期未改源码，记入下一轮注释修正）。② 因此 G3 的 86.2/27.5 ms 是 **cpu**，wall 分别 74.8–82.4 / 22.0–25.8 ms；上一版 GC-ROOFLINE §7 曾误标为「墙钟」。③ `regionFree` **只发生在帧销毁/退出**（`[regionfree]` n=1/进程），所以它不破坏「单步停顿」，但仍是**同步长停顿**：pb 帧销毁一次 ~86–99 ms。

**量具修订（C3）**：`tests/gc-p0/summarize.py:625` 原先只判 `v == "RED"`，而 G1② 的单例结论是 `RED(下界>阈)` ⇒ 聚合层漏判、打印「N/A」。已改为 `v.startswith("RED")`（一行，无判据变化），并在 `tests/gc-p0/out/after/` 重生成 `summary-fixed.txt`（`summary.txt` 保留为冻结证据）；修正后 G1② 汇总行打印 **RED**（cpu 口径）。

---

## §9 已修缺陷：sweep 在飞时新建的 slab 被错误标黑（FREEZE v2 → v3）

这是 after 第一次正式跑被卡死（`timemax pb 100000000` 2:40 不动）的根因，也是「after 作废第 1 次」的原因。

### 9.1 现象与复现矩阵

`tests/gc-p0/src/pb_escape_grow.extc`（逃逸链：每轮 `new node` 后头插，最后遍历求和）在**默认档（隐式 `M_max`≈3L 夹紧）**下：

| N | 默认档 | `EXTC_RC_M_MAX=1099511627776`（关墙） |
|---|---|---|
| 1,000,000 | 0.02 s / 21,652 kB / rc=0 / sum 正确 | 0.02 s / rc=0 |
| 1,100,000 / 1,200,000 / 1,300,000 | 0.02 s / rc=0 | — |
| **1,500,000** | **挂死**（>10 s，VmRSS 2,056 kB 不动，CPU-bound） | — |
| 2,000,000 | 20 s timeout / 30,824 kB | — |
| 5,000,000 | 20 s timeout / 90,732 kB | — |
| 10,000,000 | 30 s timeout / 120,684 kB | **0.13 s / 201,748 kB / rc=0** |
| 10,000,000 + `M_MAX=67108864` | 30 s 挂死 | — |

⇒ 只在**默认隐式夹紧、sweep 真正发生**时出现；挂死是 GC 侧活锁（CPU-bound），不是内存压力或「慢」。

### 9.2 证据链

1. **core**（attach 被禁：`yama ptrace_scope=1`、`perf_event_paranoid=2`；`core_pattern=core` 可用）：挂死后 `kill -ABRT` 取 core（`/tmp/core.110254`，约 763 MB）。栈顶就是 pb 末尾遍历 `while q != null { … q = (*q).next }`，`i=n=2,000,000`、`w.cnt=2,000,000`、`s=837,311,176,887,474`（正确 sum≈2.0e12 的 419×）。
2. **走链**（`/tmp/walk.py`，gdb python）：`DUP n=1473164 addr=0x716a3420fff0 first=60099`、walked 1,473,164、uniq 1,473,164 ⇒ **链里有环**（活结点槽位被提前释放、槽位随后被新分配复用，旧 `next` 指向复用的新结点，遍历回跳）。
3. **插桩**（只改 /tmp 生成物 `/tmp/pb2e.c`，不碰源码）：在 `markBegin` 前从本轮 root 走链查 mark 位，2M 时第 3 轮打印 `begin staleMarked=859`（来自一个 64 KiB slab：`swEpoch=0`、`nslots=4084`、`bump=4084`，本轮从未被扫到）；同轮 finish `unmarked=526,836`（可达活结点未被标黑）⇒ 随后 sweep 把它们当 dead 释放。

### 9.3 根因（逐字）

1. sweep 进行中新建的 slab 被 `extc_rc_ownAdd(s)` **prepend 到 `r->slabs` 头**（`src/back/reclaim.c:851`），而 sweep 游标在本轮起点（`sweepBegin`：`r->swSlab = r->slabs;`，`src/back/reclaim.c:3012-3020`）⇒ **本轮永远扫不到这个 slab**。
2. alloc 的「扫掠中分配即标黑」规则仍给它标黑（`src/back/reclaim.c:1348`）：

```c
if (r->marking) s->mark[wi] |= bi;   /* allocated during marking means marked black */
if (r->sweeping && s->swEpoch != r->swId
    && (s != r->swSlab || wi >= r->swWord))
    s->mark[wi] |= bi;
```

（`src/back/reclaim.c:1348`、`:1351-1353`；第二个条件只看 `swSlab`/`swWord` 游标，**没有**看「这个 slab 本轮是否会被扫到」。）
3. `markBegin` **不清 bitmap**（`src/back/reclaim.c:2503-2512`：只 `msp=0`、重置 `scanObj/scanOff`、push roots、`pinSeed`、设 `seedSlab/seedWord/seedBits/seedsDone`）；mark 位只在 sweep 逐 word 扫到时清零（`extc_rc_sweepWord`：`s->mark[w] = 0;`，`src/back/reclaim.c:3029`）⇒ 这个 slab 的黑位**永久残留**。
4. 下一轮 MARK_SCAN 把 mark 位当「已扫描」去重（`src/back/reclaim.c:2605`）：

```c
if (!_force && (s->mark[wi] & bit)) continue;   /* forced entries are still scanned */
```

⇒ 跳过一个可达结点、**整棵子树不标黑** ⇒ 下一轮 sweep 把它当 dead 释放 ⇒ 活结点被复用 ⇒ 悬垂指针/链成环。

### 9.4 修复（一行 + 注释）

`slabNew` 尾部、`s->bump = 0;`（`src/back/reclaim.c:845`）之后、`extc_rc_ownAdd(s);`（`:851`）之前：

```c
/* A slab created **while a sweep is in flight** is prepended at the head of the slab list, i.e. *behind* the cursor, so this round never
 * reaches it. Declaring it part of the swept set (swEpoch = swId) keeps the allocation-time rule in extc_rc_alloc from marking its slots
 * black: no sweep pass would ever clear such a bit (mark rounds do not clear the bitmap), and the next mark round would treat a reachable
 * object as already scanned, skip its subtree, and the sweep after that would free live objects. */
if (r->sweeping) s->swEpoch = r->swId;
```

（`src/back/reclaim.c:846-850`，代码在 `:850`。）

**语义**：该 slab 本轮不被扫 ⇒ 分配时**不标黑**才是对的（黑位无人清）；下一轮 `sweepBegin` 的 `r->swId++` 后它 `swEpoch != swId` ⇒ 正常参与扫掠；本轮期间它的子边由下一轮 mark 从根扫描覆盖。

### 9.5 验证

- `/tmp/pb2f`（修复后生成物）：N=2M / 10M / 100M 全 rc=0、sum 正确，每轮 `beginStale=0`、`unmarked=0`；100M = 2.84 s / 2,000,600 kB；
- FREEZE v4 正式 G4：pb 100M **20.49 B/node / 2.90 s / 2,000,408 kB**；
- 新增回归 `tests/gc-p0/src/regress_sweep_black.extc`（G16，N=2,000,000）：3 reps exit 0、未 timeout、`cnt=2000000 sum=1999999000000`、wall_max 0.055 s ⇒ PASS；**N 不得降到 1.5M 以下**（旧 bug 在 1.0–1.3M 正常，1.5M 起暴露）；
- 影子检查（§8.5）：cyc/pc/pd 的 `stale_rounds=0`；修复前 pb 的 `staleMarked=859`。

### 9.6 为什么门禁没提前抓到（如实）

before 只测到 pb 100M 的 B/node 与 pc 的峰值，两者都**没有**检查「链的完整性/是否成环」；旧 build 在 pb 100M 上没复现挂死（缺陷由 FREEZE v2 的默认档夹紧把 sweep 真正推进到该路径后才暴露——**这一点是推断，未逐版本 bisect，存疑**）。G16 的判据（输出 `cnt`/`sum` 正确 + 未 timeout）是对该类的正面覆盖。

---

## §10 量具与口径演进（**四轮 after 作废/被取代**，唯一有效口径是 v4）

本节记录为什么 §8 里只有一轮 after 是有效的，以及量具如何演进——避免后续把作废数字当基线。

| 轮次 | 时间 | 构建 | 结果 | 作废原因 |
|---|---|---|---|---|
| after v1 | 08:48–08:52 | `build-frz/extc`（FREEZE v1） | 卡在 G4 的 `timemax pb 100000000`，2:40 不动（CPU 时间=墙钟、VmRSS 1,044,780 kB 不再涨），套件被 kill | **pb 挂死** = §9 的 slab 黑位 bug（FREEZE v2 仍在）；`timemax` 无 timeout ⇒ 套件会无限等 |
| after v2（第一轮正式） | 09:11 | `build-frz/extc` md5 `59171671…`（FREEZE v3） | G4/G8/G9/G12/G13/G14/G15 正常，但 **G1 的 cpu 口径全 0**：`[step] max_us=0.0 total_us=0.0`、`[histo]` 全落 `b1us`、`[chan]` 全 0.0 | **时钟静默降级**：生成物不定义 `_POSIX_C_SOURCE`，run.sh 用 `-std=c11`（strict ANSI）⇒ glibc 不暴露 `CLOCK_THREAD_CPUTIME_ID`/`CLOCK_MONOTONIC` ⇒ `extc_rc_cpuNow()` 返回 -1 ⇒ `_d = (-1)-(-1) = 0`；同因 `pauseNow` 回退 `clock()*1e9/CLOCKS_PER_SEC` ⇒ **该轮（及更早所有 `-std=c11` 构建）的 `[pause]` 实为进程 CPU 时间（µs 粒度）**，不是墙钟。G1 数据无效 |
| after v3（第二轮正式，**已被取代**） | 09:14–09:15 | `build-frz/extc` md5 `59171671…`（FREEZE v3），`CFLAGS` 加 `-D_POSIX_C_SOURCE=200809L` | 当时数字有效（`[clock] step=cpu pause=wall chan=cpu` 自证），但用的是**旧 7 桶** `[histo]` | **作废**：15 桶定版后旧桶不可比；目录归档为 `tests/gc-p0/out/after-v3-superseded-0915Z/` |
| after v4（**最终有效**） | 2026-11-09 09:47 之后 | `build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f`（FREEZE v4），`CFLAGS=-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L` | **有效**：§8.0/§8.1/§8.2 全部数字出自该轮；`ALL_RC=0`、`G1CPU_RC=0`、`STRESS_RC=0`；`[clock] step=cpu pause=wall chan=cpu`；G1① 有 1 步 2,540 µs（§7-17） | 未作废（唯一有效口径） |

**v4 相对 v3 的改动（量具侧，Lead m02523/m02638）**：`[histo]`/`[chan]` 7 桶 → **15 桶定版**（`EXTC_RC_STEP_NB=15`，edges ns `1000,2000,4000,8000,16000,32000,64000,128000,256000,512000,1000000,4000000,16000000,64000000`；桶名 `b1us b2us b4us b8us b16us b32us b64us b128us b256us b512us b1ms b4ms b16ms b64ms bge64ms`，`bn`=桶上界，`over1ms`=**严格 >1e6 ns 的桶和，从 b4ms 起**）；`[clock]` 探测兜底（`cpu=unavailable`，`src/back/reclaim.c:3226-3229`）；计数档 `releaseOld` 在 sweep 在飞时跳过（`src/back/reclaim.c:2966`、`:2979`）。

**最终有效口径（Lead m02638）**：`build-rf/extc` md5 `9fd0dcd8…`（源码 `reclaim.c` `ff42799b…` / `codegen.c` `cf288544…` / `driver.c` `9aae9cb5…`）+ `CFLAGS` 含 `-D_POSIX_C_SOURCE=200809L`；G1②③ **只在 `[clock]` 的 `step=cpu` 时判**；`pause=clock` 表示该行是进程 CPU 时间口径，必须在报告里标注。**同一源码二次构建 md5 因 `-g` 路径不同而不同，溯源以源码 md5 为准。**

**重要补充：`[histo]` 桶边界 bug（本机发现，v4 在途版本，已由 runtime-facts 修）**

- **现象**：v4 在途源码（`src/back/reclaim.c` md5 `bad19f34f0bd0beb727bc9eec2a83401`，3607 行）里 `#define EXTC_RC_STEP_NB 14`（`:1046`）、桶名（`:1048`）为 `b1us b2us b4us b8us b16us b32us b64us b128us b256us b512us b1024us b4096us b1ms bge1ms`，但 `extc_rc_stepBucket` 的 edges（`:1112-1113`）是 ns `1000,2000,4000,8000,16000,32000,64000,128000,256000,512000,1024000,4096000,1000000000` ⇒ **名字与边界整体错位**：`b1ms` 实际是 `[4096 µs, 1 s)`、`bge1ms` 是 `≥1 s`，而 `over1ms = stepSumFrom(13)` 只数最后一个桶（≥1 s）。
- **后果**：G1① `over1ms=0` 近似恒真 ⇒ **假 PASS**；d>64 µs 的步被记进 `b1ms`，`[histo]` 的尾部完全不可信。
- **处置**：empirics 在适配 parser 时发现并报 Lead（m02523），Lead 确认是真 bug 并交 runtime-facts 按**定版口径**修：15 桶 + 上面的 edges 表 + `over1ms` 从 `b4ms` 起算。**v4 定版构建必须以 `[histo]` 行里出现 `bge64ms` 为自证**（旧 7 桶产物只有 `bge1ms`）。

**解析器适配（v4 定版）**：

- `tests/gc-p0/pause_parse.py`：硬编码表 `HISTO_EDGES_NS`/`HISTO_NAMES`/`OVER1MS_FROM=11`，`parse_histo_line()` 按表解析（**不再用通用正则猜桶名**，避免 `b1ms`/`b1024us` 串味；`buckets` 第 i 项上界 = edges[i]，末尾 `bge64ms` 上界记 `None`）；`over1ms = sum(counts[11:])`；带 `--selftest`；`clock_fields`（`step/pause/chan/cpu`）不变。
- `tests/gc-p0/summarize.py`：`normalize_histo()` 把 v4 dict/字符串与**旧 7 桶**（`b1us/b4us/b16us/b64us/b256us/b1ms/bge1ms`）统一成 15 桶（旧桶按上界 µs→ns 二分映射，`bge1ms` 计入 `over1ms` 并标 `legacy`）；**G1① 以 histo 的 `over1ms` 为权威**，无 `[histo]` 才退回 `[step]`，两者不一致时打印警告；quantile 用桶表上界（尾部落 `bge64ms` 记 `>64000 us`，G1③ 判 RED）；`--selftest` 自测（0.5 ms/1.5 ms/70 ms 各 1 ⇒ `over1ms=2`、G1① RED；全 0 ⇒ PASS；旧 7 桶 `bge1ms=2` ⇒ RED）。
- **自检命令**（量具自身可测，连打两行）：

```bash
python3 tests/gc-p0/pause_parse.py --selftest   # ⇒ SELFTEST PASS（over1ms 期望 2 / 0）
python3 tests/gc-p0/summarize.py --selftest     # ⇒ SELFTEST PASS（含 G1① RED/PASS 与尾部桶 inf）
```

- 回归 smoke（用**旧 v3** after 数据，验证 legacy 路径不炸）：`[clock]` 行解析出 `step=cpu pause=wall chan=cpu`；G1 cyc `p99.9=16 µs`、② N/A（区间跨阈）、③ PASS(<=32 us)；pb ② RED（下界 1.06e-4）、③ N/A（桶界 32–64）；pd ② N/A（n<1e5）；每个 case 都标 `（histo 为旧 7 桶 legacy 映射）`。

**量具教训（保留下次用）**：

1. **不要用 `ru_maxrss`**：WSL2 上 `posix_spawn` 子进程的 `ru_maxrss` 会被父进程（python）污染（`/bin/true` 报 9,916 kB；g8_t2 ru=15,512 vs 真实 VmHWM 2,956）⇒ 一律 `/proc/<pid>/status` 的 `VmHWM` + `/usr/bin/time -f %M` 交叉。
2. **短命进程（<1 ms）采样不到**：G14 的 50k 用 `timemax`（`/usr/bin/time -f %M`）而不是 2 ms 采样。
3. **`timemax` 必须带 timeout**：pb 挂死时无 timeout 的 `/usr/bin/time` 会无限等（after v1 的教训）⇒ `run_plain_to` 已加 `--timeout`。
4. **argv token 不能以 `--` 开头**：`sample.py` 的 argparse 会把 `--ctl` 当未知选项、静默丢记录（G14 改用单 token `ctl`）。
5. **`io::cout` 的多行 `<<` 链不合法**（`error: expected an expression, found '<'`），必须写成一行或分多次输出。
6. **strict ANSI 与 POSIX 宏**：任何依赖 `CLOCK_*` 的运行时插桩，编译时必须显式 `-D_POSIX_C_SOURCE=200809L`（§7-12），否则插桩静默为 0。

> 补充：v4 定版后由 Lead 在所有人停手后**串行**重跑，并重建 `tests/gc-p0/out/after/`；届时 §8.0/§8.1/§8.2 的数字需整体替换（结构不变）。

---

## §11 E17 分配成本归因（详见 `docs/analysis/ALLOC-COST.md`）

本节只给结论与指针；全部 Ir/缓存数字、逐形状表与复现命令在 **`docs/analysis/ALLOC-COST.md`**（callgrind，`-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`，构建 `build-emp2/extc` md5 `41efaf5716b25e7ac0a8ddfd5cac5d24`，源 = FREEZE v4）。

**E17（chase 归因）结论**：chase（`1048576 20261013 4000000`）的 **43.3 ns/步不是纯 DRAM 延迟**——每步只有 **0.0887 次** last-level read miss（其中 GC 标记 0.0636、walk 自身 0.0251），按 70–80 ns/miss 折算 DRAM 项 **≤7 ns/步（≤1/6）**；主体是 **2.594 次/步 L1 miss 命中 L2/L3** + **75.15 Ir/步**（walk 38.56 + 内联 rc 23.25 + GC 12.65）。工作集从 1,048,576 缩到 262,144 时 **DLmr/步 −75%**（GC 的 DLmr −99.9%）而 **Ir/步 只 −7%** ⇒ 延迟在层级缓存，不在 DRAM。

**每分配三分（本机 callgrind）**：promote 176.7 = 形状 39.2 + 内联 rc **67.6** + GC 69.3 Ir/alloc；纯 bump 分配器只要 **31.0 Ir/alloc**（四形状一致）⇒ 回收机制在分配点净增 36.6/29.5/60.5 Ir/alloc（promote/sizeratio/cycleratio），chase 23.2 Ir/步。

**GC Ir 份额 ≠ GC 税**：墙钟份额/Ir 份额 = promote 1.03、cycleratio 0.99、sizeratio 0.48、chase 0.21；`λ = λ_Ir × ns_per_Ir(shape)`，ns_per_Ir 跨形状差 11×（promote 0.0508 → chase 0.576），所以"0.41 ns/B 的全局 c_t"只在 promote 成立。

---

## §12 两个负结果（E18 等，Lead m03170 实测，本机未重跑；**下一轮不要重复投入**）

### 12.1 chase 不是 TLB / 大页问题（THP 负结果）

- 做法：在 FREEZE v4 生成的 chase C 里，`slabNew` 的 `aligned_alloc` 之后插 `madvise(s, 1MiB, MADV_HUGEPAGE)`，加 `-D_GNU_SOURCE` 重编；`v2run --pin 6, 1048576 20261013 4000000`，**5 reps 交替**跑 base 与 THP 两个二进制。
- 结果：base 中位 **171.4 ms / RSS 73,692 kB**；THP 中位 **171.0 ms / RSS 相同** ⇒ **无差异**。
- 结论：chase 的 miss 主要发生在 L2/L3（每步 2.594 次 L1 miss 中 2.505 次命中 L2/L3），**不在 TLB/大页路径**。THP、`madvise`、透明大页调参**不是 chase 的杠杆**，不要重复投入。
- 与 §11 的关系：这与"DRAM 项 ≤1/6"互相印证；chase 的 43.3 ns/步 主体是层级延迟 + 间接层，不是缺页/大页。

### 12.2 E18：`EXTC_RC_FLOOR` 下探（memory-first 唯一剩下的便宜旋钮）

- 做法：加 `-DEXTC_RC_FLOOR='((int64_t)1<<18)'`（256 KiB，默认 1 MiB，见 `src/back/reclaim.c:140-142`）重编；`v2run --pin 6`，**3 reps**。
- 结果（RSS / 墙钟中位）：

| 形状 | 默认（1 MiB floor） | 256 KiB floor | 变化 |
|---|---|---|---|
| sizeratio | 3,244–3,372 kB / 21.9–23.9 ms | 2,088–2,092 kB / 26.4–27.2 ms | **RSS −35%，墙钟 +15%** |
| cycleratio | 3,036 kB / 13.0–14.4 ms | 2,076 kB / 13.7 ms | **RSS −32%，墙钟 +4%** |
| promote | 6,492 kB / 不变 | 6,492 kB / 不变 | 0（活集 ~1.8 MB > floor） |

- 再降到 **64 KiB**：只多省 0.2–0.4 MB，但墙钟 **+35~38%** ⇒ **256 KiB 是拐点**。
- 结论：这是真实的"内存↓ / 时间↑"权衡点，可写进 Roofline 的可行域；**不能既降内存又不付时间**。适合按形状/负载选择（sizeratio/cycleratio 受益，promote 无感）。

### 12.3 `[mmax]` 违反 bench/gc/v2 SPEC「stderr 必须为空」

- 现状：`[mmax] memory wall unreachable: M_max=… floor=…; using floor` 在 sizeratio/cycleratio/promote **每次运行**都打到 stderr（隐式墙 3×L 早期小于 floor 时的告警）。这违反 `bench/gc/v2` SPEC 的"stderr 必须为空"，会污染 A/B 对比与外部基准复核。
- 处置：Lead 已派 runtime-facts 改为**只在显式设置 `EXTC_RC_M_MAX` 或诊断开关时打印**。
- 口径：本报告 §1.3 的 `[mmax]` 读数按"诊断输出"使用、不作判据；A/B 脚本比较 stderr 时按**修复后应为空**记。

