# extC 事件档（tier 1）实测：自动接线、税、内存与停顿

> 本文件是 `docs/analysis/MEASUREMENTS.md` 的姊妹篇。那份全部数据来自**清扫档（tier 2，默认 NOCOUNT=1 + 区域清扫）**；
> 本文件补上 **事件档（tier 1：计数开 `EXTC_RC_NOCOUNT=0` + 清扫关 `EXTC_RC_SWEEP_FREE=1`）** 的实测。
> 口径与那份一致：**真相在源码与生成的 `.c` 里**，`docs/**` 只当"待验证的断言"；每条结论带 `路径:行号`；
> 本机数字标「本机」，文档/他人数字标「文档」。未测到的写"存疑"。
> 唯一仓库写入 = 本文件；探针与产物全在 `/tmp/empirics-tier1/`（不进 `tests/`、不进 `src/`）。

## 0. 结论速览（每条都能在下面找到数据出处）

1. **自动事件档在本 HEAD 真的会生效**（覆盖旧文档"从不生效"的断言）：默认编译 `/tmp/empirics-tier1/probes/t1_leaf_churn.extc`，
   产物 `/tmp/empirics-tier1/out/t1_leaf_churn_A.c:13` = `#define EXTC_RC_NOCOUNT 0`、`:15` = `#define EXTC_RC_SWEEP_FREE 1`。
   接线在 `src/back/codegen.c:15609-15612`。
2. **但它只覆盖"单层 churn"**：把同一个人工形状加一级链（父对象持子引用），自动分析就退回档 2（产物 `1/0`）；
   强行打开档 1 会**泄漏子对象**——2,000,000 次"父→子"丢弃后 `freeSlot` 只释放了父的 1,999,999 个，子一个没释放，峰值 RSS **45.4 MB vs 档2 的 3.0 MB**（§5.3）。
3. **档 1 的税随对象大小反向变化**：16 B 叶结点上档 1 比档 2 慢 **+150%**（1.509 s vs 0.604 s，7.55 vs 3.02 ns/op）；
   456 B 对象上两者相同（0.101 s vs 0.101 s）；32 KB 大对象上档 1 反而**快 3×**（0.051 s vs 0.151 s，因为档 2 要为 226 MB 垃圾缓冲付缺页：sys 0.117 s vs 0.011 s）。
4. **档 1 根本没有"停顿"可测**：生成物在分配点用 `if (extc_rc_shouldSweep(...)) extc_rc_maybeCollect(...)` 守卫（`/tmp/empirics-tier1/out/t1_leaf_churn_A.c:3706,3720`），
   而档 1 的 `shouldSweep` 恒为 0（`src/back/reclaim.c:2650-2655`）⇒ `EXTC_RC_PAUSE=1` 在整个进程里**一行 `[pause]`/`[step]` 都不打**（§6.1）。
5. **档 1 的内存是"活集 + slab 复用"，档 2 的内存是"活集 × 学习到的 scale"**：32 KB × 1024 活集（32 MB）下
   档 1 峰值 33 MB（≈1.03× 活集，`slabNew=35`），档 2 峰值 226 MB（≈7.1× 活集，`slabNew=270`）——正好落在
   `max(2L,floor)×4=256 MB` 的缓冲上限内（`src/back/reclaim.c:2636-2642`）。回答"垃圾缓冲是否显著更小"：**是，约 6.8×**。
6. **两档都不在运行中把内存还给 OS**：丢弃 32 MB 活集后空转 250 ms，档 1 的 RSS 恒为 34 MB、档 2 恒为 66 MB；
   档 2 只在**进程退出**时掉到 17 MB（`extc_rc_regionDrop` 在 `main` 返回路径上，`/tmp/empirics-tier1/out/t1_bigdrop_idle_A.c:3742`；
   `regionFree` 只被 `regionDrop` 调用，`src/back/reclaim.c:2632-2635`）。所以"档 1 的 RSS 更早回落"**不成立**：档 1 是**不把缓冲建起来**。
   另外该空转本身也证明：**清扫只在分配点推进**，空闲期一次都不收（§5.2）。
7. **切片预算（8 bitmap words/步）不是停顿主项，是内存主项的驱动**：`[step] total` 只占墙钟 0.2%–7.9%，
   `[step] max` 16.8–94.5 µs、`over1ms=0`；但 32 KB 对象时清扫追不上分配，缓冲涨到 scale 上限并让
   **缺页**吃掉 78% 墙钟（§6.3、§8.4）。
8. **`gate-acyclic.sh` 在本 HEAD 是红的：12 通过 / 3 失败**，失败格是 ②a 与 ⑦ 两格，根因是**量具腐烂**而非档位误判：
   `sweeps.sh` 数的是 `[rccount] collect`，而增量路径走 `extc_rc_step`（`src/back/reclaim.c:2756-2765`），
   `extc_rc_collect` 只是"契约入口"（`:2766-2778`）。实测 `cyclic_pressure N=800000` 默认档
   `[rccount]` 无 collect 行、但 `[pause] collects=18` ⇒ **清扫确实发生了，collect 计数却是 0**（§7.2）。
9. **旧文档的"自动档永不触发"与"+100%~+455%"要分开看**：前者被本机实测推翻；后者在"小对象、强制/自动档 1"方向上仍成立
   （本机 16 B：+150%，落在该区间内；文档 `docs/plans/TODO.md:64-66`），但**大对象方向反号**（档 1 快 3×）。

## 1. 机器、工具链、构建与档位快照

| 项 | 值 |
|---|---|
| 内核 / 平台 | `Linux Yeung 6.18.58.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC x86_64`（WSL2） |
| CPU / 内存 | `nproc=24`；`MemTotal 20478916 kB`（≈20.97 GB）；cgroup `memory.max=max` |
| 编译器 | `gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0`；生成物一律 `cc -O2 -std=c11` |
| 量具 | 自写 `/tmp/empirics/measure.py`（posix_spawn + `/proc/<pid>/status` 的 VmRSS/VmHWM 定点采样 + `wait4` 拿 rusage/退出码/耗时） |
| extC | `0.0.1-alpha`，git HEAD `85b8871b`，`src/` 干净；`build/extc` md5 `f4a0a9b62f4ac2fff1892f53dd34bc42` |
| 采样周期 | 50 ms（长命进程），25 ms（`t1_bigdrop_idle`，需要看清丢弃后的平台） |
| 跑法 | `./build/extc probe.extc -o out/X.c` → `cc -O2 -std=c11 out/X.c -o bin/X`；档位在**编译期**用环境变量选 |

### 1.1 三档是怎么选出来的（源码）

- 默认（档 2）：`src/back/codegen.c:13787-13795` —— `EXTC_RC_NOCOUNT` 默认 1；
  `src/back/codegen.c:13809-13814` —— `EXTC_RC_SWEEPFREE` 三态：unset=`0`（"由分析决定"）、`=1` 强制开、`=0` 强制关（`-1`）。
- 档 1（自动）：`src/back/codegen.c:15605-15612` —— 逐字注释：
  `"The event tier (the automatic half of EXTC_RC_SWEEPFREE): the whole unit is proved "acyclic and references live only in counted slots" => every unreachable object is eventually freed on the spot when its count reaches zero => sweeping has nothing to do."`
  代码 `if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) { g.rcSweepFree = 1; g.rcNoCount = 0; }`。
- 宏发射：`src/back/reclaim.c:2914`（NOCOUNT 1/0）、`:2916`（SWEEP_FREE 1/0）。
- 关回收（档 0）：`src/main.c:471` 说明 `EXTC_RECLAIM=0` 默认开、CHANGES THE OUTPUT；此时分配器是纯 bump 的 `extc_arena_*`
  （三档产物记号：`leaf` 形态档 1 = 1014 处 `extc_rc_` / 8 处 `extc_arena_`，档 2 = 1008 / 8；档 0 = **0 处** `extc_rc_` / 12 处 `extc_arena_`）。

### 1.2 探针（全部在仓库外）

| 探针 | 形状 | 用途 |
|---|---|---|
| `t1_leaf_churn.extc` | 16 B 结点，仓库 `acyclic_heapref` 形状：`var head = new node; head.next = new node; (*head.next).val = i`，N 轮 | 单层 churn，自动档 1 的**本命形状** |
| `t1_filldrop.extc` | 456 B 结点，`world.slots[1024]` 覆盖填再全置 null，PH=2 | 中等对象、活集 467 KB |
| `t1_bigfill.extc` | 32 KB 结点（`pad: [4095]i64`），同上，PH=3 | 大对象，活集 32 MB ⇒ 2L > FLOOR |
| `t1_bigdrop_idle.extc` | 32 KB × 1024 填满→丢弃→`sleepMs(250)`，PH=2 | 看"丢弃后 RSS 停在哪"与"空闲期是否回收" |
| `t1_chain2.extc` | 两级链：每轮 `w.head = new node; (*w.head).next = new node` | 档 1 的**盲点**：父死时子引用没人减 |
| 对照形状 | `tests/reclaim-shapes/cyclic_pressure.extc`（有环，仓库自带） | 档 1 的"不该进档"方向 + gate 复现 |

所有探针都不 deref 大对象的 payload（可空 ref 的下标窄化编译器不支持，见 §10.2），所以"页是否驻留"靠**元数据/槽位写入**而不是 payload 求和；
这一限制对"活集大小"的量级没有影响（slab 按槽大小分配，见 `src/back/reclaim.c:316-322`），但**不构成** payload 的访问局部性结论。

## 2. 方法学与口径

### 2.1 峰值 RSS：VmHWM/VmRSS 采样 + ru_maxrss 交叉验证

`/usr/bin/time -v` 与 `wait4` 的 `ru_maxrss` 在本机有 **≈11 MB 地板**（WSL2 进程镜像），所以 ≤11 MB 的场景一律以
`/proc/<pid>/status` 的 `VmHWM/VmRSS` 采样为准，`ru_maxrss` 只当上界交叉验证。表中两个数都列（`peakru` / `peak`）。

### 2.2 "稳态 RSS"= 采样序列**去掉尾部的退出样本**后的最后值

进程退出前 `extc_rc_regionDrop` 会把 slab 还给 malloc（见 §5.1），尾样本会掉到 0/17 MB；所以"稳态"取倒数第二个非零平台。
短命（<60 ms）进程采样可能只抓到 0；这类场景以 `peakru` 为准并在表里标 `†`。

### 2.3 墙钟与 rep

长命场景（leaf 200 M）3 reps，墙钟逐 rep 相同到 1 ms；短命场景 2 reps。CPU 时间用 `wait4` 的 user/sys 拆分——
这是本文件区分"CPU 税"与"缺页税"的关键（§8.3）。

### 2.4 档位归属的核对（每个探针都做）

编译后逐份 `grep` 产物第 13/15 行左右的 `#define`：
档 1 = `NOCOUNT 0 / SWEEP_FREE 1`，档 2 = `1 / 0`，档 0 = 两个宏都不存在且 0 处 `extc_rc_`。
**不靠环境变量猜**：`EXTC_RC_SWEEPFREE` 是**编译期**开关（`src/back/codegen.c:13810-13813`；运行时再设无效）。

## 3. 表 A：档位 × 场景（本机，-O2，PAUSE 只在需要时打开并注记）

| # | 场景（活集） | 档 | 墙钟 (s) | user / sys (s) | peakru (MB) | 采样峰值 → 稳态 (MB) | ns/op 或 µs/alloc | 回收记号（`[rccount]`） | 结论 |
|---|---|---|---|---|---|---|---|---|---|
| A1 | leaf 16 B，N=200 M | **1**（自动） | 1.509 ×3 | 1.49 / 0.00 | 11 † | 1 → 1 | 7.55 | alloc 200,000,001；storeReg 200,000,001；**freeSlot 199,999,999**；slabNew **1** | **平坦**（200 M 次分配只占 1 个 slab） |
| A2 | 同上 | 2（`SWEEPFREE=0`） | 0.604 ×3 | 0.58 / 0.00 | 11 † | 2 → 2 | 3.02 | alloc/storeReg 200,000,001；slabNew 17；freeSlot 0（NOCOUNT 下该 tick 不发生） | 平坦；**比档 1 快 2.50×** |
| A3 | 同上 | 0（`RECLAIM=0`） | 1.609 | 0.42 / **1.14** | 3077 | 3019 → 2093（仍在涨） | 8.05（墙钟口径） | 无 rc 记号 | **无界线性增长**（200 M×16 B=3.2 GB）；墙钟被缺页拖住 |
| B1 | fd 456 B，2 M/phase×2（活 467 KB） | **1** | 0.101 ×2 | 0.077 / — | 11 † | 2 → 2 | 0.025 µs/alloc | alloc 4,000,001；slabNew **2**；freeSlot 4,000,000 | 平坦；fill 37/38 ms，drop 0/0 ms |
| B2 | 同上 | 2 | 0.101 ×2 | 0.072 / — | 11 † | 3 → 3 | 0.025 µs/alloc | alloc 4,000,001；slabNew 5 | 与档 1 **打平** |
| B3 | 同上 | 0 | 0.051 ×2 | 0.008 / — | 74 | 0（太短） | 0.013 µs/alloc | 无 rc 记号 | 纯 bump（**不代表回收**） |
| C1 | bf 32 KB，4096/phase×3（活 32 MB） | **1** | 0.051 ×2 | 0.017–0.023 / 0.011–0.017 | 33 | 0 †（采样没赶上） | 4.15 µs/alloc | alloc 12,289；slabNew **35**；freeSlot 12,288；fill 21/5/5 ms | 峰值 ≈1.03× 活集 |
| C2 | 同上 | 2 | 0.151 ×2 | 0.008–0.016 / **0.117–0.126** | **265** | **226–228 → 226** | 12.3 µs/alloc | alloc 12,289；slabNew **270**；fill 57/57/10 ms | **7.1× 活集**；78% 墙钟是缺页 |
| C3 | 同上 | 0 | 0.050–0.051 ×2 | 0.000–0.004 / 0.011–0.016 | 29 | 0 † | 4.1 µs/alloc | 无 rc 记号 | 纯 bump，per-phase release 后复用 |

`†` = `ru_maxrss` 地板（11 MB）或采样周期长于进程寿命，该格不可当"真实峰值"读。
B3/C3 是**语义参照**，不是"更好的回收"：档 0 的 arena 在块退出时 `extc_arena_release` 整片还掉，活集之外不留东西，
所以三者里它 RSS 最低；这与"回收质量"无关（对应 MEASUREMENTS.md §4 已说明的口径）。

## 4. 表 A2：单位成本（每 op / 每活对象字节）

| 指标 | 档 1 | 档 2 | 档 0 | 说明 |
|---|---|---|---|---|
| leaf 16 B ns/op | **7.55** | **3.02** | 2.08（user/op） | 档 1 vs 档 2 = **+150%**；档 0 的墙钟值 8.05 含 3.2 GB 缺页，不能当 CPU 基线 |
| fd 456 B µs/alloc | 0.025 | 0.025 | 0.013 | 档 1 = 档 2 |
| bf 32 KB µs/alloc | **4.15** | 12.3 | 4.1 | 档 2 的 8.1 µs 差额 ≈ 缺页（sys 0.117 s / 12 288 alloc） |
| leaf 峰值 RSS / 活集 | ~1 MB / 2 | ~2 MB / 2 | 3.0 GB / 1024（末态） | 档 1/2 都停在地板（1 个 64 KB slab + 进程页） |
| fd 峰值 RSS / 活集 | 2 MB / 0.45 MB | 3 MB / 0.45 MB | 74 MB / 0.45 MB | slab 粒度（64 KB 起，`src/back/reclaim.c:321`） |
| bf 峰值 RSS / 活集 | **33 MB / 32 MB ≈ 1.03×** | **226 MB / 32 MB ≈ 7.1×** | 29 MB / 32 MB | 档 2 的倍数由 scale 决定（§5.4） |
| 每活对象字节（32 KB 对象） | ≈32.2 KB | ≈226 KB | ≈28 KB | 档 2 的"每活对象"里有 6.9 份垃圾/碎片 |

## 5. 表 A3：内存增长、回落与"空闲期"行为

### 5.1 丢弃活集后 RSS 停在哪（`t1_bigdrop_idle.extc`：1024×32 KB=32 MB 活集，丢弃→空转 250 ms→再来一轮）

| 档 | 墙钟 (s) | 采样序列（VmRSS, MB） | peakru | 结论 |
|---|---|---|---|---|
| 1 | 0.531 | 34（t=0.03 起）→ 34 → 34 → … → 34（退出前） | 34 | 丢弃 32 MB 后**不降**；slab 只在 size-class freelist 里 |
| 2 | 0.557 | 34 → 34 → 63（t=0.28，第二轮 fill）→ 66 → 66 … → **17（t=0.53，退出路径）** | 66 | 空闲期**一次都不收**；只在退出时 `regionDrop` 还 OS |
| 0 | 0.506 | 4 → 4 → 6 → 6 … → 0 | 11 | 每 phase `extc_arena_release` 整片还掉 |

档 1 的 `[rccount]` 该场景：`alloc 2049 / freeSlot 2048 / slabNew 35` —— 每轮 1024 个对象全部当场释放，**但 slab 数 35 不变**，
所以下一轮直接从 freelist 拿槽：这正是"档 1 峰值=活集"的机制；也说明**释放 ≠ 还 OS**。

档 2 在 250 ms 空闲里 RSS 不动，不是"清扫慢"，而是**没有分配点就没有清扫入口**：
生成物的触发点写在分配路径上（`/tmp/empirics-tier1/out/t1_bigdrop_idle_B.c:3705` 形状），
`maybeCollect` 只在 `alloc` 之后被问（`src/back/reclaim.c:2779-2801` 的"skip:no-trigger"分支）。空闲期不分配 ⇒ 不触发。

### 5.2 退出时的"回落"是 `regionDrop`，不是回收

`/tmp/empirics-tier1/out/t1_bigdrop_idle_A.c:3742`：`__extc_ret:` 之后 `extc_rc_regionDrop((void *)&__extc_a[1]);`。
`extc_rc_regionDrop` 才调 `extc_rc_regionFree`（`src/back/reclaim.c:2632-2635`），而 `regionFree` 自己标着
`__attribute__((unused))`（`:1970`）——全进程只有这一个调用点。所以运行期 RSS 单调，退出时一次性归还。
这条对两档相同，不是档 1 的缺点，但它**否掉了"档 1 RSS 会在运行中更早回落"这个假设**。

### 5.3 档 1 的盲点：父对象死了，子对象的 hold 没人减（`t1_chain2.extc`）

`freeSlot`（`src/back/reclaim.c:1245-1264`）只做 `freeSlotLink` + 清 occ/rsbit/hold/rscnt + 记账 + `classPush`，
**不调用 `unlinkChildren`**（只有 `if (extc_rc_rsIncN) extc_rc_unlinkChildren(s, i);`，`:1239`，而默认 `rsIncN=0`，
`:1240` 注释逐字 "with no registration ever made there is nothing to undo"）；减子引用 hold 的 `unlinkChildren` 只在**清扫**的
`freeSlotWord`（`:1271-1279`，`:1269` 注释 "unlinkChildren still runs per slot"）里逐槽跑——档 1 不扫，所以永远不跑。所以档 1（不扫）在"父→子"图上会漏掉整棵子树：

| 跑法 | 墙钟 (s) | peakru (MB) | `[rccount]` | 泄漏 |
|---|---|---|---|---|
| 强制档 1（`EXTC_RC_SWEEPFREE=1`） | 0.04 | **45.4** | alloc 4,000,001；storeReg 2,000,001；storeOwned 2,000,000；freeSlot **1,999,999**；slabNew 490 | 2 M 次迭代里**子结点 2,000,000 个一个没释放**（每轮 1 个） |
| 强制档 2（`=0`） | 0.01 | 3.0 | alloc 4,000,001；slabNew 17 | 无 |
| 默认（自动） | 0.01 | 3.0 | 产物 `NOCOUNT 1 / SWEEP_FREE 0` | 自动分析**拒绝了档 1**（这个形状不是"单层 churn"） |

结论：档 1 的"每个对象在计数归零时当场释放"只对**没有出边的死对象**成立；把自动档 1 推广到任意"无环"单元
（`unitAllAcyclic` 只证无环，见 `src/back/codegen.c:13988-14031`）会按"每死一个父结点漏一棵子树"的速度增长内存。
当前 HEAD 的缓解是**形状运气**（分析对链式写入要求"更年轻的一层"，我的链式形状自动退回档 2），不是机制保证：
一旦分析放宽，泄漏立刻出现。这一条应作为 GC-ROOFLINE 的硬约束（档 1 的适用域 = 单层 churn / 无出边的死对象）。

### 5.4 档 2 的缓冲倍数从哪来

`src/back/reclaim.c:2636-2642`：`thr = max(live*2, floor) * scale`；`scale` 初值 1，空扫后 ×`EXTC_RC_SCALE_MUL`(4)（`:151`），
真找到垃圾则复位；`live=32 MB` ⇒ `max(64 MB, 1 MB)=64 MB`，一次空扫后 `thr=256 MB`。实测峰值 226 MB
（`slabNew=270` 个 1 MB slab，RSS 226 MB）正好在该上限内。**档 2 的内存不是"活集的函数"，是"活集 × 学习到的 scale"**。

## 6. 表 B：停顿（`EXTC_RC_PAUSE=1`，只观测；`-O2` 二进制）

### 6.1 档 1：结构性地没有 `[pause]` —— 不是"没测到"，是"不可能发生"

- 生成物分配点：`{ if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, ...); }`（`/tmp/empirics-tier1/out/t1_leaf_churn_A.c:3706,3720`）。
- 档 1 的 `extc_rc_shouldSweep` 恒返回 0（`src/back/reclaim.c:2652-2655` `return 0; /* skip:sweepfree */`）⇒ `maybeCollect` 永远不被调用；
  `atexit(extc_rc_pauseDump)` 也在 `maybeCollect` 的触发分支之后才 arm（`:2780-2801`）。
- 实测：`leaf200_A_pause`（200 M 次分配、`EXTC_RC_PAUSE=1`）stderr **只有 `[rccount]`**，没有 `[pause]`/`[step]`；
  `bf_A_pause` 同理。墙钟 1.509→1.609 s 的 +100 ms 是 PAUSE 环境的启动开销，不是停顿。
- 所以档 1 的"停顿上界"= 单次 `freeSlot` 的 O(1) 工作（`:1245-1264`）。本机可给的实证上界：一次丢弃 1024 个 32 KB 对象
  `drop_ms=0`（<1 ms 覆盖 1024 次 freeSlot ⇒ 每次 <1 µs），`idle` 场景 `freeSlot 2048` 也在同一量级。**更细的分布测不出**（见 §9.2）。

### 6.2 档 2：`[pause]`/`[step]` 汇总（同批二进制，只开 PAUSE）

| 场景 | `[pause] collects` | pause max / total (µs) | `[step] n` | step max / total (µs) | over16 / 64 / 256 / 1ms | `[worst]` |
|---|---|---|---|---|---|---|
| leaf 16 B N=200 M | 305 | 5.1 / 122.8 | 41,785 | 40.8 / 13,894.6 | 3 / 0 / 0 / **0** | `kind=sweep why=alloc budget=8 dur=40.8 sweep_words=8 freed=512` |
| fd 456 B 2 M×2 | 1,525 | 49.7 / 1,484.9 | 24,459 | 94.5 / 8,017.4 | 3 / 1 / 0 / **0** | `kind=sweep why=alloc budget=8 dur=94.5 sweep_words=8 freed=256` |
| bf 32 KB 4096×3 | 8 | 17.3 / 56.3 | 161 | 16.8 / 319.1 | 1 / 0 / 0 / **0** | `kind=mark why=step budget=8 dur=16.8 pops=8 marked=8` |
| cyclic_pressure N=800 k（默认档） | 18 | 11.0 / 92.0 | 7,871 | 67.0 / 7,703.0 | 7 / 1 / 0 / **0** | `kind=mark why=advance budget=256 dur=67.0 pops=256 marked=128` |
| cyclic_pressure N=4 M（默认档） | 88 | 18.0 / 426.0 | 39,002 | 151.0 / 37,976.0 | 16 / 1 / 0 / **0** | `kind=mark why=advance budget=256 dur=151.0 pops=256 marked=224` |

读法（口径承 MEASUREMENTS.md §6）：`collects` 是**整轮清扫**次数；`[step]` 是切片步；`[pause].total` 只累计"真清扫"的墙钟。
本机 `[pause] total` 占墙钟：leaf 122.8 µs / 1.5 s ≈ 0.008%（但 `[step] total` 13.9 ms ≈ 0.9%）；fd 8.0 ms / 101 ms ≈ 7.9%；
bf 0.32 ms / 151 ms ≈ 0.2%；最大单步 151 µs（4 M 的 cyc），**从未出现 `over1ms`**。
没有直方图，`[step]` 只有上界桶（`[124](@ref` 见 reclaim.c:2678-2714 的格式），所以 **p50/p99 不可得**：
可给的是 `max`、`mean=total/n` 与"≥16 µs 的步数占比"（leaf 3/41785=0.007%，fd 3/24459=0.012%，bf 1/161=0.62%，cyc 4M 16/39002=0.041%）。

### 6.3 切片粒度是不是主项？

- 停顿：**不是**。`[step] total` 占比 0.2%–7.9%，`max` 151 µs，`over1ms=0`。
- 内存：**是驱动项**。`EXTC_RC_SWEEP_BUDGET 8`（`src/back/reclaim.c:889`）是"每步只扫 8 个 bitmap word"；
  大对象场景下清扫吞吐跟不上垃圾产生速率 ⇒ `bytes` 涨到 `thr`（§5.4）⇒ 新 slab 全落成缺页：
  bf 档 2 的 sys 时间 0.117–0.126 s / 墙钟 0.151 s = **78%**，而档 1 只有 0.011–0.017 s。
  所以"切片预算"在本负载上的代价记在**内存与缺页**账上，不是停顿账上——这正是 Roofline 模型里"吞吐约束"与"延迟约束"要分开的原因。
- 例外：`EXTC_RC_SCAN_CHUNK`（默认 65536，`:2146-2150`）能把单步拉到 ms 级（MEASUREMENTS.md §6.6 已测：改成 1 M 时 `over1ms=1`），
  但本任务的三档负载都没碰到该路径。

## 7. 表 C：本机实跑的门禁

### 7.1 `tests/reclaim-acyclic/gate-acyclic.sh`：12 通过 / 3 失败（15.3 s）

| 格 | 结果 | 数值 / 证据 |
|---|---|---|
| ①a 事件档清扫次数 = 0 | ok | 强制档 1，`[rccount]` 无 collect |
| ①b stdout 与 `EXTC_RECLAIM=0` 逐字相同 | ok | sha256 `05db7717d1dc…` |
| ①c 事件档峰值 ≤ 默认 ×1.5 | ok | 1.45 MB vs 1.45 MB（1.00×） |
| ②a 有环形状默认档清扫次数 > 0 | **FAIL** | 实测 collect=0（`SWEEP_FREE=0`，见 §7.2） |
| ②b 开关非空转对照 | note | 因 ②a 红而跳过 |
| ③ 红证明（强制档 1 于有环形状） | note | 无对照可立（②已红） |
| ④ blind-localhold 已不红 | ok | RSS 1.00× |
| ⑤ 端到端 hole_c（0 rc 调用点 ⇒ 整段不发运行期） | ok | a–d 全过；峰值 3.49 MB vs 强制档 3.49 MB |
| ⑥ 端到端 acyclic_heapref（自动 `SWEEP_FREE=1`、collect=0） | ok | a–d 全过；峰值 1.45 MB，1.00× |
| ⑦ 安全格 cyclic_pressure N=800 k | **FAIL** | 真路径 `SWEEP_FREE=0` 且 collect=0，脚本判"没拒绝" |
| ⑦ 安全格 scale_growth N=800 k | **FAIL** | 同上 |

全绿的历史数字（文档口径）见 `tests/reclaim-acyclic/README.md:47-67` 与 TAXMAP §7；**本机 HEAD 已不复现**。

### 7.2 三个失败的根因：量具腐烂，不是档位误判

`sweeps.sh` 把"清扫次数"定义为 `[rccount] collect calls=N`（`tests/reclaim-acyclic/sweeps.sh:1-20` 的注释与解析），
但运行时的增量路径是 `extc_rc_step`（`src/back/reclaim.c:2756-2765`），每轮结束在 `extc_rc_finish` 里 `sweeps++`（`:2608`）；
`extc_rc_collect`（`:2766-2778`，逐字注释 `"Contract entry (API / unit tests / settle / account queries): finish one round within the call."`）
在普通程序里**根本不会被调用**——它只给 API/单元测试/settle 用。所以：

- 事件档 collect=0 是"真"（该路径不调用 collect），①a 通过但**证据力弱**；
- 有环形状默认档 collect=0 也是"真"，**但它同时在清扫**：同一份二进制加 `EXTC_RC_PAUSE=1` 得 `collects=18 max_us=11.0`（N=800 k）/ `collects=88`（N=4 M），
  `[worst] kind=mark why=advance` 也在动。②a 因此红，⑦ 跟着红。
- 文档断言 `README.md:61`："同一形状默认档 collect=28 → 事件档 collect=0" —— 本机 HEAD 上默认档 collect=**0**，该断言**已过时**（量具/实现之一变过）。

这是 gate rot 的典型形态：判据没有错，量具与实现脱钩了（`[rccount]` 组件表见 `src/back/reclaim.c:171-184`，`collect` 是 index 3，只有 `extc_rc_collect` 记它）。
建议（不属本任务）：把 `sweeps.sh` 的"清扫次数"改成 `[pause] collects`（需要 `EXTC_RC_PAUSE=1` 运行时 env + arm 点）或给 `step` 的轮完成单独计数。

### 7.3 其他门禁

本任务只跑 tier 1 直接相关的 `gate-acyclic.sh`；MEASUREMENTS.md §7 已记录另外六个门禁在 HEAD 全绿（本机、同一天、同一 `build/extc` md5），
本文件不重复跑（时间预算）。`bench/gc/pause_dist.sh` 在档 1 上仍会返回 `PAUSE_DIST_STRUCTURAL0`（原因同 MEASUREMENTS.md §6.5：`extc_rc_collect` 无调用点），
本任务未再跑。

## 8. 四个必答问题

### 8.1 档 1 的 RSS 是否比档 2 更早回落？

**不。运行期两档都不回落，档 1 只是不把缓冲建起来。**
证据：§5.1 的空转表（档 1 = 34 MB 平台跨 drop+250 ms；档 2 = 66 MB 平台）；§5.2 的 `regionDrop` 只在 `main` 返回时调用；
`regionFree` 是 `__attribute__((unused))` 且只有 `regionDrop` 一个调用点。两档的 slab 都进 size-class freelist（`:1263` `classPush`），不还给 OS。
差异只出现在"峰值"：`bf` 档 1 33 MB vs 档 2 226 MB（§3）。

### 8.2 档 1 的"免费早收"是否让垃圾缓冲显著小于档 2 的 `max(2L,floor)`？

**是，约 6.8×**：32 KB × 1024 活集（32 MB）下档 1 峰值 33 MB（1.03× 活集），档 2 峰值 226 MB（7.1× 活集 = `max(2L,floor)=64 MB` × scale 4 的上限内）。
`fd`（456 B）档 1 2 MB vs 档 2 3 MB（活集 0.47 MB）——倍数小是因为对象小、清扫跟得上（§6.3）。
`leaf`（16 B）两者都在地板（1 MB vs 2 MB）。所以"显著更小"只在**大对象 / 高分配字节率**时成立。

### 8.3 档 1 的税相对档 0 / 档 2 各多少？

| 场景 | vs 档 2（墙钟） | vs 档 0（墙钟 / user CPU） | 备注 |
|---|---|---|---|
| leaf 16 B | **+150%**（1.509 vs 0.604） | 0.94× 墙钟 / **3.55× user** | 档 0 的墙钟被 3.2 GB 缺页惩罚（sys 1.14 s），per-op CPU 税看 user：档 1 1.49 s vs 档 0 0.42 s |
| fd 456 B | 0%（0.101 vs 0.101） | 1.98× / 9.6× | 档 0 0.051 s，user 仅 0.008 s |
| bf 32 KB | **−66%（档 1 快 3×）**（0.051 vs 0.151） | 1.02× / 约 5–10×（user 0.017–0.023 vs 0.000–0.004 s，粒度太粗） | 档 2 的差额是缺页（sys 0.117 s） |

一句话：**档 1 的税是"每 op 常数 CPU"（小对象上贵 2.5×），档 2 的税是"缓冲 × scale 的缺页"（大对象上贵 3×）**。
两者谁贵取决于**每节点字节 × 分配速率**，这正是 GC-ROOFLINE 应形式化的分界。

### 8.4 切片预算（walk 批 / SEED 预算）在这批负载上是否成为主项？

**停顿上不是，内存/缺页上是。** `[step] total` 占墙钟 0.2%–7.9%（§6.2）；但 32 KB 场景档 2 的缓冲涨到 226 MB 且 78% 墙钟是缺页。
机制：`EXTC_RC_SWEEP_BUDGET 8` bitmap words/步（`src/back/reclaim.c:889`）是**固定步长**，而每分配产生的垃圾字节随对象变大而线性增长，
⇒ 超过某个"每节点字节"阈值后清扫吞吐 < 垃圾产生速率，`bytes` 只能靠 `scale` 抬阈（最终到 `EXTC_RC_SCALE_MAX`，`:144`）。
本机阈值粗估：`fd` 456 B/alloc 时缓冲 ≈ 平（3 MB），`bf` 32 KB/alloc 时缓冲 = 7× 活集 ⇒ 分界在"每分配垃圾字节 ~ 数 KB"量级（存疑，未做参数扫描）。

## 9. 这些数据能证明 / 不能证明什么

### 9.1 能证明

1. HEAD 上自动事件档**会生效**（产物 0/1 + `[rccount]` 计数路径 + 零 collect 入口），旧文档"永不触发"过时。
2. 档 1 的内存上界是"活集 + slab 复用碎片"，档 2 的是"活集 × scale"；大对象下差 6.8×。
3. 档 1 的税是 CPU 常数项（16 B 上 +150%），档 2 的税在大对象时是缺页（78% 墙钟）。
4. 档 1 对"父→子"死对象**泄漏子树**：2 M 次迭代漏 2 M 个子结点、45.4 MB vs 3.0 MB；`freeSlot` 不调用 `unlinkChildren`（源码可逐字核对）。
5. `gate-acyclic.sh` 在本 HEAD 红 3 格，根因是 `[rccount] collect` 与增量路径脱钩（有 `[pause] collects` 交叉证据）。

### 9.2 不能证明 / 未测

1. **p50/p99 停顿分布**：插桩只有 max/total + 4 个上界桶（`src/back/reclaim.c:2678-2714`），无直方图 ⇒ 只能给 max/mean/桶占比。
2. **档 1 的细化停顿**：结构上 `maybeCollect` 不被调用 ⇒ 没有停顿可采样；单次 `freeSlot` 的分布需改源码插桩（本任务不改源码）。
3. **自动档的覆盖面**：只测了 leaf（自动 1）与 chain2（自动 2）两个形状；没有扫语料统计"多少形状真的进档 1"。
4. **payload 访问局部性**：探针不 deref 大对象 payload（可空 ref 下标窄化不支持），所以 `bf` 的"活集"是分配字节而非被触摸过的字节。
5. **scale 复位条件**（"真找到垃圾则复位"）：只从源码注释确认（`src/back/reclaim.c:2631-2633`），未做"有环/无环混合"的形状来观测 scale 轨迹。
6. **SHARDS>1 × 档 1**：未测；`scanUnitReclaim` 对"可能有跨点环"的单元拒绝分片（`src/back/codegen.c:14066-14069`），档 1 与其交互存疑。
7. **`[worst] why` 的语义**（walk/scan/push 三预算在档 1 全 0）：本批只看到 `sweep/alloc`、`mark/step`、`mark/advance` 三种，未穷举。
8. **长时间稳态**（分钟级、活集震荡）未测；本批最长 1.6 s。

## 10. 本机数字 vs 文档数字（来源标注）

| 断言 | 来源 | 本机结论 |
|---|---|---|
| 自动事件档"实际等于永不触发" | `tests/reclaim-perf/TAXMAP.md:243-248`（§7.1） | **推翻**：`leaf` 自动 0/1 生效（§0.1） |
| 计数比清扫贵 +100%~+455% | `docs/plans/TODO.md:64-66`；`tests/reclaim-acyclic/README.md:9-11` | **部分成立**：16 B 上 +150%；456 B 上 0%；32 KB 上 **−66%**（反号） |
| 红证明：强制档 1 施加到有环形状上必须炸（脚本阈值 `MIN_BLOWUP=5`） | `tests/reclaim-acyclic/gate-acyclic.sh:1-30`、`README.md:63-67` | 本机该格因 ② 先红而跳过；同方向证据是本文件的 `chain2` 泄漏（15.1×，45.4 vs 3.0 MB） |
| 默认档 `cyclic_pressure` collect=28 | `tests/reclaim-acyclic/README.md:61` | **过时**：本机 collect=**0**（`[rccount]` 无该行），但 `[pause] collects=18` |
| 档 2 缓冲 = `max(2L,floor)×scale` | `src/back/reclaim.c:2636-2642` | **实测吻合**：32 MB 活集 → 峰值 226 MB（上限 256 MB） |
| sweep 预算 8 words/step | `src/back/reclaim.c:889` | 实测 `[worst] budget=8 sweep_words=8` 逐字吻合 |

## 11. 未能测量 / 存疑（必须带进结论）

1. p50/p99：插桩无直方图（同上 §9.2）；若需要，得加桶或改用外部 perf/eBPF 采样。
2. 档 1 单次 `freeSlot` 的分布：只有 "1024 次丢弃 <1 ms" 的聚合上界；"每次 <1 µs"是推论不是逐次测量。
3. 自动档 1 的**触发面**：只看了两个形状；`unitAllAcyclic`（`src/back/codegen.c:14001`）的真实覆盖率未知。
4. `chain2` 的自动档退回档 2 是**形状运气**还是分析规则（"更年轻的一层"）保证，未逐条核对 `planFnAcyclic` 的判定树；本文件只报了"退回"这一事实。
5. 档 2 在 `bf` 的 `[step] n=161` 与 `slabNew=270` 的量级关系未完全闭合（缺页账能解释墙钟，但"哪一步分配第 270 个 slab"未逐事件追踪）。
6. `gate-acyclic.sh` 的 ②b/③ 两格因 ② 红而跳过 ⇒ 本文件没有"强制档 1 于有环形状"的独立复现（只有 `chain2` 的无环泄漏）。
7. 未跑 `bench/gc/measure.py` 与 `pause_dist.sh`（前者是 best-of-N 的旧量具，后者对档 1 结构性返回 0）；MEASUREMENTS.md §7 已有清扫档口径。
8. 所有数字是 WSL2（虚拟化、共享内核）单机；跨平台/裸机的绝对值和缺页成本会不同。

## 12. 复现清单

```bash
# 0) 环境
uname -a; nproc; free -k | head -2; gcc --version | head -1; md5sum build/extc
# 1) 探针（仓库外）
ls /tmp/empirics-tier1/probes/*.extc
# 2) 三档编译（档位 = 编译期 env）
./build/extc /tmp/empirics-tier1/probes/t1_leaf_churn.extc -o /tmp/empirics-tier1/out/x_A.c
EXTC_RC_SWEEPFREE=0 ./build/extc ... -o .../x_B.c
EXTC_RECLAIM=0       ./build/extc ... -o .../x_C.c
cc -O2 -std=c11 x_A.c -o x_A
# 3) 档位自检（必须是 0/1、1/0、无宏）
grep -n "define EXTC_RC_NOCOUNT\|define EXTC_RC_SWEEP_FREE" /tmp/empirics-tier1/out/*.c
# 4) 计数（EXTC_RC_TIME 是**编译期**开关）
EXTC_RC_TIME=1 ./build/extc ... -o .../x_A_t.c && cc -O2 -std=c11 x_A_t.c -o x_A_t
# 5) 测量（VmRSS/VmHWM 采样 + rusage）
python3 /tmp/empirics/measure.py ...   # 或 /tmp/empirics-tier1/drive.py
# 6) 门禁
bash tests/reclaim-acyclic/gate-acyclic.sh
```
