# P0 原型报告：页归还（G）、惰性 freelist（B）与字节节奏回收（P）

> 口径：所有实验都在 `build-lead5/extc`（源码 FREEZE v5：codegen.c `d339382889b87c0dd18b357c69e2e779`、reclaim.c `9af9ca9b8f6ad939a577f99404ddf319`）**生成的 C 上打文本补丁**完成，不改 `src/`；正确性判据=stdout 与未打补丁产物逐字节相同且 exit 0。计时用 `/tmp/v2/v2run --pin 6 --out`（wait4 内核值），四形状参数同 `bench/gc/v2`（promote 500000 20261013；sizeratio 2000000 20261013；cycleratio 400000 20261013 100；chase 1048576 20261013 4000000）。产物在 `/tmp/p0proto/{base,src,bin}`。

## §0 结论速览

| 机制 | 一句话 | 判定 |
|---|---|---|
| B 惰性 freelist | sweep 只清位图并把 slab 标脏，复用时一次性按位图重建链 | **值得进编译器**：墙钟/内存/RSS 全在噪声内（promote 42.78→43.77 ms），GC 步总时长 promote −12.5%、sizeratio −52%、cycleratio −8%；步 max promote 160.4→57.5 µs、sizeratio 51.8→9.5 µs、cycleratio 39.0→16.9 µs。对 G1/G2 停顿门禁是直接收益 |
| G 空 slab 页归还（madvise） | 当一个 slab 一个活槽都不剩时 `madvise(MADV_DONTNEED)` 它的载荷页 | **不进**：稳态 churn 下是负收益（cycleratio 12.76→20.40 ms，+60%，stime 8.0 ms 全在补页，minflt 435→9,565）；drop 后 RSS 也几乎不降（见 §2.5） |
| G-big 巨块载荷归还 | 巨块（huge block）的 payload 在整块变空时 free | 条件性正收益：一次运行可归还 483 MB；但稳态 churn 下每次归还都要重新 mmap+清零，墙钟 +20% |
| P 字节节奏回收（本轮发现） | 每分配一次，GC 推进的工作量**按分配字节数**而不是按「分配事件数」计 | **必须做**：`tests/gc-p0/src/pd_bigobj.extc` 60 轮从 493,400 kB/745.9 ms 降到 26,072 kB/113.5 ms（内存 −95%、快 6.6×）；200 轮 1,067,288 kB→26,072 kB（−97.6%）、1272.9→202.4 ms；四个 v2 形状完全中性 |

一句话：**B 换停顿，P 换内存有界，G 不划算**。P 是本轮唯一同时改善内存与时间的结果，且它恰好补上了当前内存上界证明的前提缺口（见 §4.5）。

## §1 B：惰性 freelist（sweep 不链接，复用时重建）

### §1.1 机制

现状（`src/back/reclaim.c:1296` `extc_rc_freeSlotLink`）：每释放一个槽就把它头插进 `slab->freelist`，链值存在 `tid[]`（槽号+1）。sweep 逐槽释放时，这就是「每槽一次链写」。

B 把它换成：

1. sweep 侧只清 `occ`/`rsbit`/`rscnt` 并给 slab 记一个 `nfree`（洞计数），**不再逐槽链接**；
2. 分配侧取槽顺序：① `freelist >= 0` 先弹出 → ② `bump < nslots` 拿新槽 → ③ 否则**一次**按 `valid & ~occ[w]` 扫完整个位图重建整条 freelist，再弹出。

每个 slab 只要被取用一次就付一次 O(nwords) 的位图扫描，摊到该 slab 的每个槽上仍是 O(1)，但省掉了「释放时每槽一次链写」。

### §1.2 三版演进（第一版踩的坑值得记）

- **v1（每次分配逐槽扫位图）**：功能正确但灾难性——promote 42.1→158.5 ms、RSS 6,492→133,212 kB、minflt 1,350→33,005；sizeratio 26.9→99.6 ms/3,048→146,988 kB；cycleratio 12.7→35.3 ms/2,844→53,916 kB。
  插桩（`[dbg] freed=4443345 push=0 pop=0 bump=4537723 bit=10611 new=1667`）显示 4.54M 次分配只有 12k 次走了位图，其余全在拿「从未发放」的槽 ⇒ 内存≈累计分配量 109 MB。
  根因不是位图慢，而是**满判据用错了状态**：判「这个 slab 还有洞吗」用的是扫描提示 `freeW`，而从未被扫过的满 slab 的 `freeW` 仍是 0，于是链遍历在链头（永远是最新 slab）就停住，旧 slab 永远选不到。
- **v2（用洞计数 `nfree` 判满）**：RSS 回到基线，但每次分配仍要扫位图，墙钟 promote 43.3→46.3 ms（utime +10 ms）。GC 步时间已下降：promote [step] 17,818→14,755 µs、sizeratio 2,739→1,236 µs（max 199.6→15.4）。
  另一个坑：链遍历里还有一处 `freeW` 判据没同步改，改完 v2 才正常。
- **v3（每访问重建一次链）**：即 §1.1 的最终形态。

### §1.3 v3 实测（7 reps 交错，median）

| 形状 | 墙钟 base→B (ms) | RSS max base→B (kB) | minflt |
|---|---|---|---|
| promote | 42.78 → 43.77 (+2.3%) | 6,492 → 6,492 | 相同 |
| sizeratio | 23.54 → 23.89 (+1.5%) | 3,180 → 3,180 | 相同 |
| cycleratio | 13.72 → 12.43 (−9.4%) | 2,848 → 2,848 | 相同 |
| chase | 172.66 → 169.50 (−1.8%) | 73,696 → 73,696 | 相同 |

GC 步时间（`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，`[step]`）：

| 形状 | total µs base→B | max µs base→B |
|---|---|---|
| promote | 17,752 → 15,528 (−12.5%) | 160.4 → 57.5 |
| sizeratio | 2,581 → 1,247 (−52%) | 51.8 → 9.5 |
| cycleratio | 471.8 → 432 (−8%) | 39.0 → 16.9 |

⇒ B 不改变「总工作量必须做多少」，它改变的是**每步的形状**：把「释放时每槽一次链写」搬到「复用时每 slab 一次位图扫」，于是 sweep 步变短、尾部变矮。对要求「停顿时长有界」的门禁（G1/G2）是正向收益，对吞吐与内存是中性的。

### §1.4 进编译器前必须补的证明

打补丁的 v3 只在**清扫档（NOCOUNT=1）**下验证过。计数档（`EXTC_RC_NOCOUNT=0`）里 `freeSlot`（单槽释放）、`freeSlotCascade`（P0-5 级联）、`releaseOld`（定向释放，P0-3）都会释放槽：v3 依赖「重建只在 `freelist == -1` 时发生」，必须证明同一个槽不会被两条路径链接两次（否则 freelist 成环 ⇒ 同一槽发给两个对象 ⇒ 语义破坏）。建议判据：计数档 218 条断言全绿 + ASan/UBSan + 一条「同一槽只出现在链上一次」的运行时断言（debug 构建）。

## §2 G：空 slab 页归还（MADV_DONTNEED）

### §2.1 机制

`extc_rc_p0gHint(s, empty)`：当 slab 的 `occ` 全 0 时，对它的**载荷页**（跳过 slab 结构体所在的头部页）发 `madvise(MADV_DONTNEED)`；元数据（occ/mark/rsbit/tid/rscnt）都是独立 malloc，不受影响；`big` 巨块跳过。调用点两处：slab 级批量重置（P1-7 `extc_rc_sweepElide`）命中后、逐 word 清扫完成一个 slab 且 occ 全 0 时。`EXTC_P0G=1` 才开启。

### §2.2 打洞次数与代价

| 形状 | punch 次数 | 归还 KB | minflt 变化 |
|---|---|---|---|
| promote | 0 | 0 | — |
| chase | 0 | 0 | — |
| sizeratio | 3 | 300 | 不变 |
| cycleratio | 651 | 36,456 | 435 → 9,565 |

### §2.3 稳态 churn 下是负收益

7 reps 交错（`EXTC_P0G=1`）：cycleratio base 13.72 ms → G 20.40 ms（**+49%**，stime 8.0 ms 全在补页）；sizeratio 23.54 → 22.21（−5.6%，噪声内）；RSS max 2,844→2,944 kB（略升）。原因很直接：churn 形状的「空 slab」马上会被下一次分配复用，打洞省下的页立刻又缺回来，白白多付一次清零与页表开销。

### §2.4 小 slab 模式（`-DEXTC_RC_SLAB_BITS=16`，64 KiB）

sizeratio 25.36 ms（+7.7%）、cycleratio 13.96（持平）、promote 41.05、chase 150.57（vs base 172.66，−12.8%，**疑为臂顺序偏差，未反序复核**）；B+G+64 KiB 组合 sizeratio +33%（minflt 5,577）。⇒ 小 slab 也不划算：RSS 由触达页决定，而 slab 变小只是让「空」更容易出现，并没有减少触达页。

### §2.5 为什么「drop 之后 RSS 应该掉」这个直觉不成立（本轮最重要的否证）

我先写了一个 drop 负载（800k 结点链表 → 置空 → 60 轮 churn），基线 RSS 竟然从峰值 17.4 MB 掉了 5.8–7.2 MB，有没有 G 都一样。追查发现两件事：

1. `strace` 在整个运行里看到 **3,138 次 `munmap(0x…, 2101248)`**（= `aligned_alloc(1 MiB, 1 MiB)` 的载荷映射），时间均匀分布在 churn 期间；生成 C 里唯一释放 slab 的路径是 `extc_rc_regionFree`（`free(s)`，base/drop.c:2009），它只由 `extc_rc_regionDrop` 调用。
2. 而 `regionDrop` 在**每个函数返回前**都有一次：`chain()` 的 `__extc_ret:` 之后就是 `extc_rc_regionDrop(&__extc_a[1]); extc_arena_destroy(...)`。region 的身份是**该帧 arena 的地址**，所以每次调用 `chain()` 都自带一个 region，函数返回即整区释放（槽全部 free → slab free → munmap）。

⇒ 我原来的负载把 800k 结点分配在 `chain()` 这一帧里，`chain()` 一返回就整区归还了——**测试的其实是既有的「帧退出整区释放」路径，而不是 sweep 归还路径**。这也解释了为什么「RSS 会掉」：掉的是帧退出的那一下。

正确的负载必须把「分配 → 丢弃 → churn」全部放在 `main` 自己的帧里（main 的 region 活到进程结束）。改后（`drop3.extc`，800k 结点 + 20 轮 × 2,000 次 churn）实测：

| 变体 | drop 前峰值 | drop 后平台 | 说明 |
|---|---|---|---|
| base | 17,452 kB | 17,800 kB（平） | sweep 清了槽，但页不退 |
| T（提前触发，见 §4） | 17,496 | 18,600（平） | 只提前扫，不退页 |
| G+T | 17,580 | 15,288（−14%） | 打洞+提前扫，退了一点 |

连 G+T 也只退 2.3 MB，而不是整 12.8 MB 的 phase-1 数据：因为 churn 立刻把空出来的 slab 又用上了（页又被触达）。**结论：只要分配还在继续，页归还省下的内存马上会被重新触达；页归还不能替代「少分配」。**

### §2.6 G-big：巨块载荷归还

巨块（1 个槽、payload 单独 malloc）整块变空时 `free(s->data)` 并把 slab 从 region/大小类链/allocHot 里摘掉。实测 `bigchurn.extc`（60 轮：每轮 1 个 8 MiB 巨块 + 500 个小结点）：整轮累计归还 59 块/483,387 kB，但墙钟 16.9→20.4 ms（+20%，每次归还都要重新 mmap + memset 8 MiB），峰值 RSS 10,328→10,476 kB（不变，因为复用本来就不需要新页）。

⇒ 只有当「巨块是一次性的、且不会再被同类分配复用」时才有意义；那时 region drop 往往已经覆盖了它（除非巨块挂在 main/长命帧里）。判定：**不做**，除非将来引入「长命帧 + 一次性巨块」的用例。

## §3 T：live 崩塌后的提前触发（G 的必要配套）

### §3.1 缺口

`extc_rc_shouldSweep` 要求 `bytes > thr && roundOk`；`roundOk` 要求「自上一轮以来分配的字节 ≥ max(live, floor)」。live 是**上一轮**的标记结果，所以 live 崩塌（丢弃大结构）之后，下一轮要等分配量涨到**旧的** live 才开得起来。丢弃 12.8 MB 之后只做 2,000 次小分配（≈32 kB）时，sweep 根本不会跑 ⇒ 内存停在峰值。这不是 G 的问题，是**触发策略**的问题。

T 把 `roundOk` 的信用在开启时改成 `floor`（1 MiB）：live 崩塌后最多再分配 1 MiB 就会开一轮清扫。

### §3.2 实测

- drop3（§2.5 表）：T 单独不清退（只扫不退页，18,600 平）；G+T 退到 15,288（−14%）。
- 四个 v2 形状（5 reps 交错，median ms，base/T/G+T）：promote 40.97/41.27/40.39；sizeratio 25.54/23.88/23.38；cycleratio 12.76/12.69/**20.28**；chase 168.11/177.02/180.96。⇒ T 单开在稳态里中性；G+T 在 cycleratio 上因打洞补页而 +59%。

结论：T 是「让内存有机会退」的必要条件，但**只有配合页归还才看得见内存效果**；而页归还在稳态下是负收益。两者只在「live 崩塌 + 之后分配很少」的窗口里划得来，这个窗口需要策略判断（例如：距上次 sweep 已分配 floor 且利用率 bytes/live 高于阈值时才打洞），不是无条件开启。

## §4 P：字节节奏回收（本轮唯一同时降内存与降时间的机制）

### §4.1 问题：per-event 预算跟不上字节速率

`tests/gc-p0/src/pd_bigobj.extc` 每轮 `new big`（1M 个可空指针字段 = 8 MiB）后丢弃。实测（默认档）：`[pause] collects=1`、`[work] n=121`、`thr_max=1 MiB`；60 轮峰值 **493,400 kB**、200 轮 **1,067,288 kB**（≈ 轮数 × 8 MB），而活集只有 8 MB。

根因：标记/清扫的推进量是**每次分配事件**给一个固定批次（sweep 8 word、mark 256 pops 或 8,192 word 扫描），而这个程序的分配事件数极少（60 轮 × 65 次），标记要扫的是 60 × 1M 个字段的量级 ⇒ 扫不完 ⇒ 一轮都完不成 ⇒ 一个槽都没释放。于是内存按**累计分配量**线性增长。

这不是「GC 慢」，而是**配额与工作量的量纲不匹配**：工作量由字节（指针字段数）决定，配额由「分配次数」决定。

### §4.2 机制（原型）

在 `extc_rc_alloc` 里，把「每分配一次推进多少 GC 工作」从常数改成**按本次分配字节数**：

- sweep：`budget = max(8 × pressure, n >> 6)` words；
- mark：`batches = n >> 10` 个 `extc_rc_markAdvance` 批次（每批多扫一个 `EXTC_RC_SCAN_CHUNK` 窗口）。

两个都包在 `EXTC_P0P=1` 里。含义就是 Roofline 里的 keep-up 条件：**回收工作率必须与分配字节率成正比**；常数配额只能覆盖「每个对象都差不多大、分配事件足够多」的程序。

### §4.3 实测（pd_bigobj，live≈8 MB）

| 轮数 | 变体 | 墙钟 ms | 峰值 RSS kB | stime ms | minflt |
|---|---|---|---|---|---|
| 60 | base | 745.9 | 493,400 | 737.8 | 123,065 |
| 60 | P | **113.5** | **26,072** | 12.5 | 6,245 |
| 200 | base | 1,272.9 | 1,067,288 | 1,168.6 | 266,548 |
| 200 | P | **202.4** | **26,072** | 11.9 | 6,244 |

200 轮：内存 **−97.6%**、时间 **−84%**。两条曲线都会随轮数增长（base 线性涨，P 恒定 26 MB ≈ 3L 的既有上界）。stdout 与 base 逐字节相同。

时间也变快是因为 base 的时间几乎全是缺页/页表（stime 1.17 s），而 P 让回收跟上了，内存不涨也就没有这部分开销。

### §4.4 对四个 v2 形状的中性性（5 reps 交错，median）

| 形状 | base ms | P ms | RSS base/P (kB) |
|---|---|---|---|
| promote | 44.62 | 40.25 | 6,492 / 6,496 |
| sizeratio | 22.91 | 22.22 | 3,180 / 3,180 |
| cycleratio | 12.76 | 12.80 | 2,844 / 2,848 |
| chase | 169.62 | 171.01 | 73,692 / 73,696 |

小对象形状的 `n >> 6`/`n >> 10` 都≈0，所以 P 退化成现状，噪声内无差别；四个形状 stdout 与 base 逐字节相同（另测）。

### §4.5 P 与内存有界证明的关系（这是要点）

`docs/analysis/MEMORY-MODEL-CURRENT.md` §18 的上界 `M ≤ M_max + 缓冲`（并已降级为「经验上界」）隐含一个前提：**回收工作量跟得上分配字节速率**。pd 形状是该前提的反例——上界被突破 20×（493 MB vs 约 24 MB），且与 M_max 无关（M_max 只用于夹 `thr` 与加压清扫，从不拒绝分配）。

P 把「跟上」变成结构性的（配额 ∝ 字节），于是上界从「对某些形状经验成立」变成「对字节速率成立」。它也给出一个可执行的准入控制方向：当 `bytes` 已到 `M_max` 而回收仍跟不上时，分配点可以**同步多付**回收工作（不是后台线程，也不加 STW），最坏情况是「分配变慢」而不是「内存无界」——这正好对应用户的两条硬要求（内存有界、停顿有界：每次分配多付的工作量本身就是有界的）。

## §5 汇总：下一轮进编译器的顺序与判据

| 优先级 | 改动 | 位置 | 判据 |
|---|---|---|---|
| P0 | 字节节奏配额（P） | `src/back/reclaim.c` `extc_rc_alloc` 的两处步进（sweep budget / mark batches） | pd_bigobj 200 轮 RSS ≤ 3L+8 MiB 且墙钟 ≤ 现基线；四个 v2 形状 ±3% 内；**新增门禁 G18**（大对象内存有界） |
| P0 | 惰性 freelist（B） | `src/back/reclaim.c` `freeSlotLink`/`freeSlotWord`/`classSlot` + `ExtcRcSlab` 加 `nfree` | G1/G2 的 `max` 与 p99.9 改善；计数档 218 断言 + ASan 全绿；新增「同一槽只入链一次」断言 |
| P1 | live 崩塌触发（T）+ 有条件的页归还（G） | 触发：`roundOk`；页归还：slab 空且利用率低于阈值时打洞 | 只在 G18 与 G1 都不回退时开启；需先补「churn 期打洞补页 `stime`」判据 |
| 不做 | G 无条件打洞、G-big、64 KiB slab | — | 稳态负收益（§2.3/§2.5/§2.6） |

## §6 复现

```
# 生成未打补丁的 C（也可用 build-rf/extc，产物等价）
./build-lead5/extc -w --no-line-map -I stdlib bench/gc/v2/src/promote.extc -o /tmp/p0proto/base/promote.c

# B / G / T / P 四个变体（都是文本补丁，锚点 count==1 才写）
python3 /tmp/p0proto/patch.py --b /tmp/p0proto/base/promote.c /tmp/p0proto/src/promote.b.c
python3 /tmp/p0proto/patch.py --g /tmp/p0proto/base/promote.c /tmp/p0proto/src/promote.g.c
python3 /tmp/p0proto/p0t.py /tmp/p0proto/base/promote.c /tmp/p0proto/src/promote.t.c
python3 /tmp/p0proto/p0p.py /tmp/p0proto/base/promote.c /tmp/p0proto/src/promote.p.c
cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L /tmp/p0proto/src/promote.p.c -o /tmp/p0proto/bin/promote.p

# 交错计时（EXTC_P0T / EXTC_P0G / EXTC_P0P 都是原型的开启开关，未设则行为等同 base）
REPS=5 python3 /tmp/p0proto/ab2.py base,p promote,sizeratio,cycleratio,chase

# 大对象内存界（关键实验）
EXTC_P0P=1 /tmp/v2/v2run --pin 6 --out -- /tmp/p0proto/bin/pd.p 200
```

## §7 已知局限

1. 全部是**生成 C 上的文本原型**，不是编译器补丁；进 `src/` 时要重新按源码结构实现（B 需要改 `ExtcRcSlab` 布局与三处释放路径；P 需要定义「分配字节 → 工作配额」的换算常量并标定）。
2. B 的计数档安全性只做了静态论证（§1.4），没有跑计数档全量断言；进编译器前必须补。
3. T/G 的「窗口判定」没有做策略实验：只用固定阈值看过一次（§3），没有扫参；因此 §5 把 T/G 列为 P1 而非 P0。
4. 计时机器是 WSL2（load 1–3），墙钟抖动 ±5%；所有比较都用同轮交错 + median 降低偏差，但 1–2% 的差异不可信。
5. pd_bigobj 的增量标记复杂度是「每个对象的指针字段数」，P 的 `n >> 10` 常数是按该形状手调的，需要标定（目标：mark 速率 ≥ 分配字节率 × 每字节要扫的字段数）。
6. 没有跑 callgrind（本轮判据是墙钟+停顿+内存）；进编译器时建议补 Ir 与 `[step]` 分布。
