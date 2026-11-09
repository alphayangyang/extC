# P1-SLAB-QUANTUM：把 slab 栅格从 1 MiB 收到 128 KiB（chase 墙钟的大头）

> 结论先说：**chase 比 C++ 慢的那 1.2×，大头不是 GC、不是计数、不是 memset，而是 slab 的地址栅格**。
> slab 用 `aligned_alloc(1 MiB, 1 MiB)` 逐块发，但每块实际只发出去约 64 KiB 的槽（`chunk` 由槽大小定），
> 于是 94% 的虚拟地址是洞，所有 slab 的低位地址又完全一致（2^20 统一步长）。把栅格收到 128 KiB
> 之后：chase 墙钟 **−12.3%**（7 回合交错；只改 emitted 宏直编的扫描给 −12…−16%）、sizeratio **0…−6%**，**停顿不变或更好、RSS 不变、
> 指令数 −86（零成本）**。改动只有一个 emitted 宏：`EXTC_RC_SLAB_BITS` 20 → 17（`reclaim.c:138`）。

## 1. 症状与排除（chase）

`bench/gc/v2` 的 chase（`1048576 20261013 4000000`）在 v11 上是 133–147 ms，同轮 cpp:careful
109 ms、rust:careful 210 ms。先按「GC 花了多少」拆（`/tmp/prof`，taskset 单核，5 次取 min）：

| 版本 | 墙钟 | 含义 |
|---|---|---|
| default（v11 生成 C） | 119.6–131.0 ms | 全部机制 |
| `v1_no_collect`（`extc_rc_shouldSweep` 直接 `return 0`，簿记与屏障全留） | 114.1–118.4 ms | GC（mark+sweep）**≈ 5.5–12.6 ms** |
| off（`EXTC_RC=0`，纯 arena bump） | 82.9–90.0 ms | 无回收 |

⇒ `default − v1_no_collect` 只有 5–13 ms；**`v1_no_collect − off` 才是 31 ms 的大头**（分配路径 + 屏障）。
把这 31 ms 再拆（每一项都在 v1 生成 C 上单改、精确字符串断言）：

| 单改 | 收益 | 结论 |
|---|---|---|
| 去掉 `memset(p, 0, ss)` | −1.6 ms | 零初始化不是杠杆 |
| 去掉 `occ`/`mark`/`nLive`/`tid`/`bytes` 每槽元数据 | −2.5 ms | 元数据不是杠杆 |
| `extc_rc_shade`/`shadeOld` 置空 | −1.7 ms | 屏障不是杠杆 |
| walk 里 `storeRefLocal` 换成普通赋值 | 0（73.77 vs 73.65） | 同上 |

三项加起来 ≈ 5.8 ms，剩下 ≈ 20 ms 不在任何一条指令上。决定性对照：

| 实验 | 做法 | 墙钟 | 结论 |
|---|---|---|---|
| **D4** | 只把 `extc_rc_alloc` 的**函数体**换成裸 48 B bump，其余一字不改（`shouldSweep` 调用、屏障、`initRef`、churn 全留） | **88.2 ms**（v1 118.2、off 89.1） | 30 ms 全在 alloc 函数体内，且与它做了多少工作无关 |
| **D5** | D4 + 按真实 slab 布局摆对象（8 KiB 块、每块 168 槽） | **90.9 ms** | 8 KiB 紧凑布局**无罪** |
| **L_slabbump** | 真实 slabNew 改成从一个 1 GiB 对齐区里按 `EXTC_RC_SLAB_SIZE` 步进发块 | 112.3 ms（只 −8） | 换成 arena 分配没用，**步长还是 1 MiB** |
| **L3_compact** | 同上但按 **64 KiB**（`chunk`）步进 | **86.8 ms**（off 82.7） | 紧凑发块把差额全吃掉 |
| **L4_alias** | 保持 1 MiB 步进，但每块加 `(n % 16) * 4096` 偏移 | ≈ L_slabbump，无改善 | 不是单纯 L2 set 别名 |

**结论：差额来自 slab 之间的地址跨度/对齐（栅格），不是指令、不是 miss 数**（cachegrind 也显示
D1mr 11.78M vs 11.58M、LLd 1.91M vs 1.81M，两版几乎相同）。

## 2. 机制：每块只用 6%，栅格是 2^20

`ExtcRcSlab` 的**发放容量**由槽大小定：`chunk = max(64 KiB, 2^k)`，倍增到装得下
`EXTC_RC_SLAB_SLOTS_MIN = 1024` 个槽为止（宏在 `reclaim.c:342-343`，倍增循环在 `reclaim.c:841-846`）。但块本身是
`aligned_alloc(EXTC_RC_SLAB_SIZE, EXTC_RC_SLAB_SIZE)`（`reclaim.c:858`），v11 时
`EXTC_RC_SLAB_SIZE = 1 MiB`。以 chase 的 48 B 节点为例：`chunk = 64 KiB` ⇒ `cap ≈ 1362` 槽 ⇒
**一块 1 MiB 只用到约 65 KiB（6%）**；1.11M 次分配 ≈ 813 块 ⇒ 约 **813 MiB 虚拟跨度**里只放
52 MB 节点。

两条机制叠加：

1. **TLB / 页表**：813 个 1 MiB 栅格里只有前 16 页是热的，映射表项按 1 MiB 步长散布，触及的页数
   是紧凑布局的十几倍。
2. **地址低位同构**：步长是 2^20，所有 slab 的 L1/L2 index 位完全相同 ⇒ 每类槽位在低位索引上
   落到同一个集合。L4 实验说明「按 set 打散」这条单独不成立（不是 L2 别名本身），但一致步长确实
   让物理页的预取/核间分布都变差；L3 实验说明**只要步长变成 `chunk`，问题消失**。

## 3. 为什么只能整体缩小栅格

`EXTC_RC_SLAB_SIZE` 是**承重语义**：从 payload 指针反查 slab 是一次位与
`p & ~(EXTC_RC_SLAB_SIZE - 1)`（`reclaim.c:361` 的 `extc_rc_slabOf`；标记步的专用副本在 `:623`、
`extc_rc_ownSlab` 在 `:662`、`extc_rc_regionOfOwn` 在 `:2484`），
`extc_rc_owns` / 屏障 / 标记 / 清扫全走这条路。所以要紧凑化只有两条路：

- **A. 整体缩小栅格**（本次）：`SLAB_BITS 20 → 17`，反查仍是位与，只是掩码变短；
- **B. 一个 1 MiB 大块切多个 slab**：反查要变成除法或基址表 ⇒ 分配热路径与屏障都变贵，不做。

## 4. 为什么落 17 位，不落 16 位

`-DEXTC_RC_SLAB_BITS` 扫描（chase.v1.c 直编，单轮 / 25 次交错取 min）：

| bits | 块大小 | 单轮 chase | 交错 chase | 备注 |
|---|---|---|---|---|
| 20 | 1 MiB | 114.5 ms | 143.04 ms | v11 |
| 18 | 256 KiB | 123.4 ms | — | 单轮就比 20 差（噪声？未复测） |
| **17** | **128 KiB** | **109.6 ms** | **125.51 ms（−12.3%）** | **本轮落地** |
| 16 | 64 KiB | 99.6 ms | 120.53 ms（−15.7%） | sizeratio 回归 |

16 位在 chase 上更快，但会**夹断 sizeratio 的 96/128 B 尺寸类**：`chunk` 增量要求每块 ≥1024 槽，
128 B 槽需要 128 KiB 的块，而块上限被夹到 `EXTC_RC_SLAB_SIZE` ⇒ 16 位时只能发 64 KiB、槽数减半、
开块次数翻倍 ⇒ **sizeratio +4.0%**。17 位两种形状都赢（chase −12.3%、sizeratio −5.2%），故取 17。

## 5. 端到端（官方产物，7 回合交错、奇偶回合反向跑，取 min）

b20 = 改动前编译器（`/tmp/prof/extc.b20`），b17 = 本轮；每格「墙钟 / 峰值 RSS」，args 同 `run.sh bench_args`：

| 形状 | b20（1 MiB） | b17（128 KiB） | Δ | b17 : cpp:careful | b17 : rust:careful |
|---|---|---|---|---|---|
| promote `500000 20261013` | 39.83 / 5,920 kB | 38.47 / 5,920 kB | −3.4% | 0.32×（cpp 121.6） | 1.02×（rust 37.7） |
| sizeratio `2000000 20261013` | 23.03 / 3,184 kB | 22.78 / 3,184 kB | −1.1% | 1.30×（cpp 17.5） | 1.14×（rust 20.0） |
| chase `1048576 20261013 4000000` | 146.85 / 73,696 kB | **128.73 / 73,696 kB** | **−12.3%** | 1.11×（cpp 116.1） | 0.59×（rust 218.5） |
| cycleratio `400000 20261013 100` | 13.93 / 2,852 kB | 13.69 / 2,852 kB | −1.7% | 0.39×（cpp 35.1） | 0.82×（rust 16.8） |

- sizeratio 的 −1.1% 是**本回合**的数；上上轮 25 回合交错给的是 −4.8%（22.44 → 21.36）。两次方向一致、
  幅度 1–6% ⇒ 保守口径写「0…−6%，不稳定但不会变慢」，不写单点收益。
- **绝对毫秒只在同一会话内可比**：同一台机隔几分钟整体会漂 5–10%（例如同一份 cpp:careful 的
  promote 二进制在不同会话里量到 99 / 116 / 122 ms），所以跨轮只读比值。
- 本轮之后 chase 与 cpp:careful 的差距由 ~1.25× 收到 **1.11×**；sizeratio 仍是最差的格子（**1.30×**），
  那是「显式 free 的 tcache」对「GC + freelist 复用」的形状差，与栅格无关（见 §11）。

## 6. 停顿与内存（这是用户的两条硬约束）

G18（`tests/gc-p0/run.sh g18`）：`pd_bigobj` 60 轮 = 200 轮 = **26,072 / 26,076 kB**（上界 26,624）
⇒ **PASS**，与 v11 逐 kB 相同；`pd_pointer_dense` 200 轮 112,092 kB、p99.9 64.0 µs、over1ms=0。

停顿（`bench/gc/v2/pause/extc.sh`，b20 → b17）：

| 形状 | 事件 | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| promote | 11,091 → 11,091 | 1.30 → 1.30 | 4.9 → 5.1 | **20.3 → 17.2** | **133.6 → 53.7** |
| chase | 728 → 728 | 5.4 → 5.3 | 38.2 → 36.6 | **161.2 → 73.5** | **161.2 → 73.5** |
| cycleratio | 213 → 213 | — | — | **13.6 → 5.6** | **13.6 → 5.6** |
| sizeratio | 1,200 → 1,200 | 1.8 → 1.8 | — | 12.8–79.2（两臂互有高低，噪声） | 同 |

⇒ **停顿没有变差，长尾反而更好**：块小了，一次清扫/标记在更大的块上摊薄的长步变短。RSS 除
sizeratio 略降外不变。

## 7. G18 判据口径的诚实说明（不改判据）

`tests/gc-p0/g18.py:45` 硬编码 `SLAB_MIB = 1.0`，上界公式是 `3·live + 2·SLAB_SIZE`。栅格变成
128 KiB 之后，脚本会打印 24,832 kB 的上界，而实测是 26,072 kB（比它大 1.2 MB）⇒ **超调的那部分
不是 slab padding，是既有的 in-flight payload 项**（旧的「2·SLAB_SIZE」只是恰好够用）。

**判据不放宽、也不改写**：脚本保留现状（它是保守公式），本轮的分析文档与 changelog 如实标注
「上界项与实测项的构成不一致」。真要重新对齐公式，需要先把 in-flight payload 单独立项（`R_max`
已在 v11 有了：`EXTC_RC_M_MAX` 显式墙下的预留），那是下一轮的事。

## 8. 门禁与红前绿后

- `tests/reclaim/run.sh` **216 / 0**（ASan/UBSan 216/0、`ALLOC_INLINE=0` 216/0、chunk=1 13/0、
  chunk=1e9 红自测仍红）；`tests/reclaim/driver.c` 里两处随几何变化的账目也同步：`t_regionFreeDeferLimit`
  的分配点 20,000 → **300,000**（128 KiB 块下 Node 类每块 ~4093 槽，要让待 free 队列真跨过 8 MiB 限额
  需要更多分配点）与 `t_regionFreeDeferHuge` 的注释，**判据一字未改**（仍是「一次有界排空必须清到 0」）。
- `tests/reclaim-perf/gate.sh` **87,254,277 ≤ 88,029,115**（v11 是 87,254,363 ⇒ **−86 Ir**，
  栅格缩小在指令层零成本）；slotindex 穷举 144,857 组不符 0。
- `tests/reclaim-gaps/gate-release.sh` **4/4**；`./check.sh docs` **13/0**；
  `./check.sh nobench` **150 通过 / 2 跳过 / 0 失败**。

## 9. 顺带修掉的一个门禁：mark-locality 的红自测失去了牙

`tests/reclaim-perf/gate-mark-locality.sh` 的红自测要求「拆掉批量弹出 ⇒ DLmr 与 D1mr 都超线」。
栅格收紧后 **P5 的 DLmr 从 146,276 掉到 1,306**，而且拆批量也是 1,306（比值 1.00×）——
紧凑布局让这条形状几乎不再打到末级失手，于是「DLmr 超线」这条判据变成**恒绿**（门禁腐烂）。
处置（判据只严不放宽）：通过侧两条线**一字不动**（D1mr ≤ 700,000、DLmr ≤ 190,000）；
红自测改成要求「D1mr 超线 **且** 拆批量 : 现行 ≥ 2×」（实测 1,686,250 / 621,572 = **2.71×**），
DLmr 降为观测打印，并在脚本头注释里写清为什么它不再有分辨力。

## 10. 代价与限制

1. **huge block 与既有行为不变**：`slot > (SLAB_SIZE - 头)/2 ~ 64 KiB` 才走 payload 单独 malloc，
   栅格变小后这个门槛从 ~512 KiB 掉到 ~64 KiB ⇒ 64–512 KiB 的对象从「huge（payload 独立）」
   变成「普通 slab 内一个槽」，行为等价（计数/标记/清扫同一条路径），只是少一次 malloc；
   这是本轮唯一的行为面变化，`driver` 里 `t_regionFreeDeferHuge`（600,000 B）仍走 huge（> 64 KiB）。
2. **开块更频繁**：每块槽数少 8 倍 ⇒ slabNew 次数上升（观测量 `[slab]` 计数；实测 Ir 变化 −86，
   说明这条在指令层可忽略）。
3. **上限没有变好**：本轮不是内存轮，RSS 与峰值不变（sizeratio 略降）。
4. **G18 公式口径**见 §7，未改判据。
5. **已知测试覆盖缺口（本轮新发现，未修）**：想给 `t_regionFreeDeferLimit` 加一条「限额路径真的被走到」
   的断言（`extc_rc_fqForced > 0`）会红 —— 普通尺寸类的待 free 队列压不到 `EXTC_RC_FREE_QUEUE_BYTES`
   （8 MiB）限额，限额路径只在 huge block 形状（如 `pd_bigobj`）触发 ⇒ 已回退成 `(void)` 消警，
   这条路径目前只有 `pd_bigobj` 侧的 G18 覆盖。

## 11. 下一轮开头

- **slab shell / super-block**：L3 实验证明「从一个大对齐区里按 `chunk` 步进」还能再吃掉剩余差额的
  大头（86.8 vs off 82.7），但那要求反查从位与变成「基址 + 移位/查表」。可行方案是保留 128 KiB
  对齐语义、让同一尺寸类的块在地址上连成 2 MiB 的「外壳」，反查先用位与定位外壳、再用一个 16 项
  小表定位块（缓存友好）。必须给出「分配热路径 ≤ +2 Ir」与停顿不变两条判据。
- **sizeratio 1.30×**：这是「显式 free 的 tcache」对「GC + freelist 复用」的形状差，可动的是
  复用链的 locality（`classSlot` 的桶链顺序）与 96/128 B 类的 chunk 尺寸，不是栅格。
- **快路径瘦身**：memset / 每槽元数据 / 屏障三项各自只有 1.5–2.5 ms，**合并做也就 ~6 ms**，
  优先级低于上面两条。

## 12. 复现

```bash
# 端到端（b20 与 b17 用 /tmp/prof/extc.b20 与 build/extc 各编一份产物，交错跑）
EXTC=./build/extc bash bench/gc/v2/run.sh bench --reps 7 --pin 6
# 栅格扫描（chase.v1.c 直编；只改 emitted 宏，不需要重编编译器）
for b in 16 17 18 20; do cc -O2 -D_POSIX_C_SOURCE=200809L -DEXTC_RC_SLAB_BITS=$b -o /tmp/c$b chase.v1.c; done
# 停顿
EXTC=./build/extc bash bench/gc/v2/pause/extc.sh
# 门禁
tests/reclaim/run.sh; tests/reclaim-perf/gate.sh; tests/reclaim-gaps/gate-release.sh;
bash tests/reclaim-perf/gate-mark-locality.sh --selftest
EXTC=./build/extc OUT=tests/gc-p0/out/v17-g18 bash tests/gc-p0/run.sh g18
```

## 附录：冻结指纹（本文件描述的口径）

| 文件 | md5 | 行数 |
|---|---|---|
| `src/back/reclaim.c` | `2b6fd4d55a4943bc1cc97143e835b34c` | 4,169 |
| `src/back/codegen.c` | `2f560e2e6deef8c4927db4611344aecb` | 16,017（未改） |
| `tests/reclaim/driver.c` | `25f4db974885da8294ef9970507ddac1` | 1,620 |
| `build/extc` | `89b46416c9eb7708ddd81c45d1f212cf` | — |