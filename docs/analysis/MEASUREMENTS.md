# extC 内存管理模型：本机实测（MEASUREMENTS）

> 本文件只记录**本机实测**。每条源码断言都带 `路径:行号`；文档里的旧数字一律标注为「文档断言（引用）」。
> 所有探针源码、生成物、脚本、日志都在仓库外：`/tmp/empirics/`（探针 `probes/*.extc`、生成 `out/*.c`、日志 `out/gout/*`、门禁日志 `gates/*.log`）。
> 测量时间 2026-11-09 02:10–02:30 CST（同一登录会话，机器同时有队友在跑构建/测试，见 §2.4 噪声）。
> 目标 commit：`85b8871b`（`git status --short src/` 干净）；编译器二进制 `build/extc` md5 `f4a0a9b62f4ac2fff1892f53dd34bc42`，自报 `extC 0.0.1-alpha`。

---

## 0. 结论速览（每条都能在本文件找到数据出处）

1. **不逃逸的循环分配完全不进回收运行期**：探针 `pa_arena_nonescape` 生成的 C 里 `extc_rc_` 记号数为 **0**（纯 `extc_arena_*`），2,000,000 轮共 1.5 s，峰值 RSS **1.5 MB**、稳态 **1.5 MB**、**零停顿**（无 `[pause]` 输出）。这是全文最重要的一条：默认模型里"arena 路径"的代价≈0，回收运行期只为**逃逸/受管分配点**付费。
2. **逃逸对象线性累积无界**：`pb_escape_grow` 每节点 ≈ **20 B**（1M→21.5 MB、5M→101.5 MB、10M→201.6 MB、20M→401.4 MB，/usr/bin/time 的 `Maximum resident set size`），100M 节点 1.50 s 堆到 **1953.7 MB**，进程退出时 region drop 才把 RSS 从 1953.7 掉到 298.4 MB ⇒ **回收不缩容，RSS 峰值 = 历史峰值活集**。
3. **region 不缩容有直接证据**：`pg2_highwater` 先建 5,000,000 个长命对象（142 MB），随后整片 `w.head = null` 丢光（≈120 MB 活集变垃圾）并空转 1.2 s，**RSS 一个字节都没降**；之后的环垃圾 churn 把它推到 316.8 MB 并停住。
4. **环垃圾靠 sweep 能收住，但 RSS 不回到基线**：`pc_cycle_churn` 每轮造 2 个不可达 2-环（100M 轮 = 200M 次分配），默认档 0.80 s / 峰值 **2.9 MB**（9 个采样点全程 2.9 MB 恒定），说明 sweep 跟得上；但同程序 `EXTC_RECLAIM=0` 时 2.41 s / 峰值 **4615.1 MB** 且逐点线性上升（+1.9 GB/s）⇒ 无回收即无界。
5. **默认 no-count 是"用清扫换计数"**：打开计数（`EXTC_RC_NOCOUNT=0`）墙钟代价 **+76%…+301%**（pb ×1.76–1.81、pe ×2.14、pc ×3.89–4.01），与文档断言 +100%~+455% 同量级（§9）。
6. **分片开关在单线程环垃圾场景下是灾难**：`EXTC_RC_SHARDS=2` 墙钟 **8.13 s（×10.2）**、峰值 **6147.2 MB（×2120）**；`=4` 8.53 s / 6080.9 MB ⇒ 分片**完全不回收**这些环。`main.c:476` 已警告 `EXTC_RC_SHARDS=N ... CHANGES THE OUTPUT`。
7. **单次停顿有界，且界由预算常量决定，不由堆大小决定**：默认分片数=1、no-count 下，100M 次分配的 `[pause]` 最大收集耗时 16–68 µs、平均 ≈1 µs，占墙钟 **0.6%**；切片 `[step]` 的 p99 ≈ 2 µs（>5 µs 只占 0.07%），尾部 max 86–314 µs，`over1ms=0`。**唯一能突破 ms 级的是 >`EXTC_RC_SCAN_CHUNK` 的大对象**：关掉分段后同一程序单步 **1005–1044 µs**，默认分段时 **5–8 µs（collect 级）/ ≤115 µs（切片级）**。
8. **仓库自带的 `tools/rc-pause-probe.py` 在本 revision 上量不到 collect 级停顿**：它插桩的 `extc_rc_collect` 在生成物里是 `__attribute__((unused))` 且**没有任何调用点**（热路径全走 `extc_rc_step`），所以 `max_pause_us=0.0`、`PAUSE_DIST n=0` 是**结构性 0**，不是"没有停顿"（§6.3）。任何只看 `max_pause_us` 的门禁在本 revision 上是盲的。
9. 六个既有门禁在本机**全绿**（§7）：gate.sh 82,037,532 Ir ≤ 88,029,115；gate-work 6/6；tables 427.8/48.2 Ir；mark-locality D1mr 616,112 / DLmr 145,962；slab-scale 199.5 vs 197.5 Ir。

---

## 1. 机器、工具链、构建与开关快照

| 项 | 值 | 取法 |
|---|---|---|
| 内核 | `Linux Yeung 6.18.58.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC x86_64` | `uname -a` |
| CPU | `Intel(R) Core(TM) Ultra 9 275HX`，`nproc=24` | `/proc/cpuinfo`, `nproc` |
| 内存 | `MemTotal: 20478916 kB`（20.97 GB），cgroup `memory.max=max` | `/proc/meminfo` |
| 负载 | 测量窗口内 `/proc/loadavg` = `0.68 1.10 0.85`（1/445 可运行）| `/proc/loadavg` |
| C 编译器 | `gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0` | `cc --version` |
| valgrind | `valgrind-3.26.0`（门禁用） | `valgrind --version` |
| python | `3.14.4` | `python3 -V` |
| extc | `build/extc`，md5 `f4a0a9b62f4ac2fff1892f53dd34bc42` | `md5sum` |

**探针的统一构建/运行命令**（除注明外全表同此）：

```bash
# 1) 生成 C
./build/extc -w -I stdlib /tmp/empirics/probes/<probe>.extc -o /tmp/empirics/out/<probe>.c
# 对照档在生成前加环境变量（编译期开关）
EXTC_RC_NOCOUNT=0 ./build/extc -w -I stdlib /tmp/empirics/probes/<probe>.extc -o /tmp/empirics/out/<probe>_nc0.c
EXTC_RC_SHARDS=2  ./build/extc -w -I stdlib /tmp/empirics/probes/<probe>.extc -o /tmp/empirics/out/<probe>_sh2.c
EXTC_RECLAIM=0    ./build/extc -w -I stdlib /tmp/empirics/probes/<probe>.extc -o /tmp/empirics/out/<probe>_norc.c
# 2) 编译（矩阵数字全部来自 -O2；§6 的插桩对照用 -O1，见各表注）
cc -std=c11 -O2 /tmp/empirics/out/<probe>.c -o /tmp/empirics/bin_<probe>
# 3) 运行：测量器负责 100 ms 采样，并记录 VmHWM 与 rusage
python3 /tmp/empirics/measure.py '{"argv":["/tmp/empirics/bin_<probe>","<N>"],"env":{"EXTC_RC_PAUSE":"1"},"timeout":120,
  "stdout":"/tmp/empirics/out/gout/<case>_1.out","stderr":"/tmp/empirics/out/gout/<case>_1.err"}'
```

**开关语义（本机实证，不是推测）**：`EXTC_RC_NOCOUNT`、`EXTC_RC_SHARDS` 是**编译期**开关，进生成的 `#define` 与代码形态；`EXTC_RECLAIM=0` 也是生成期（连分配器形态一起换）。生成物里运行期 `getenv` 只剩 `EXTC_RC_PAUSE/EXTC_RC_SLOWSTEP/EXTC_RC_STATS/EXTC_RC_TRACE_UB` 四个。源码依据：
- `src/back/codegen.c:13768` `g->rcNShard = 1;`；`src/back/codegen.c:13770` `g->rcNoCount = 1;`（注释逐字：`The default is the **no-count** fast path; EXTC_RC_NOCOUNT=0 restores counting`）
- `src/back/codegen.c:13772` `getenv("EXTC_RC_SHARDS")`（`N > 1` 才生效，上限 64）；`src/back/codegen.c:13794` `getenv("EXTC_RC_NOCOUNT")`，只有 `strcmp(...,"0") == 0` 才把 `rcNoCount` 置 0
- `src/back/reclaim.c:2914` `bufPuts(out, noCount ? "#define EXTC_RC_NOCOUNT 1\n" : "#define EXTC_RC_NOCOUNT 0\n");`
- `src/back/reclaim.c:1312` 段头 `/* ---- `EXTC_RC_NOCOUNT=1`: the counting entries become no-ops [rt:nocount] ------`
- `src/main.c:471` `EXTC_RECLAIM=0  turn reclamation off; it is **on by default** -- **CHANGES THE OUTPUT**.`
- `src/main.c:473-478` 逐字：`A program with a governed allocation point (`new T` / `alloc<T>(n)` / `@overwrite`) gets the slab allocator / holder-count / region-sweep block; =1 **allows** it ... a unit proved to have no reclaim work emits no reclamation runtime at all`
- `src/main.c:494` `EXTC_RC_NOCOUNT=0    re-enable holder counting (it is **off by default**)`；`src/main.c:476` `EXTC_RC_SHARDS=N ... CHANGES THE OUTPUT`

---

## 2. 方法学与口径（读数字前必看）

### 2.1 RSS 口径：用 `/proc/<pid>/status` 的 VmHWM，不用 rusage
测量器 `/tmp/empirics/measure.py` 用 `posix_spawn` 起进程、每 100 ms 读一次 `/proc/<pid>/status` 的 `VmRSS/VmHWM`，同时 `wait4` 取 rusage。**posix_spawn 的 `ru_maxrss` 被 fork 前的解释器镜像污染**：小进程恒报 ≈11.5 MB（pa 的 `maxrss_kb=11824` 而 `peak_vmrss_kb=1524`）。所以本文件的"峰值 RSS"一律是 **VmHWM**；rusage 只用来交叉校验大数值（pb 的 `maxrss` 与 VmHWM 相差 <1%）。
仓库 `bench/gc/measure.py` 走 rusage 差值，同样有 ≈13 MB 地板（pa 报 `rss_mb=13.0`），小进程不可信、大进程可用（§9）。

### 2.2 "稳态 RSS" 的定义
取采样序列**末 20% 的中位数**。对持续增长的负载（pb、pc_norc、pc_sh2）这个数没有意义，表里写"—（无稳态）"，并给出逐点轨迹。

### 2.3 墙钟取 3 次的最小值/中位数
每 case 跑 3 次（`matrix.sh`），表里写 `min/median`。计时器是 `measure.py` 的 `time.time()` 包住整个子进程（含 exec）。

### 2.4 噪声：停顿插桩对调度敏感，单次数字不可当点值
同一二进制 `bin_pc_cycle_churn`（100M，`EXTC_RC_PAUSE=1`）连跑 3 次，`[step] max_us` = **314 / 114 / 86**，`[step] total_us` = **194603 / 197623 / 194324**；另一时段重跑 3 次得 max = **182 / 143 / 140**。⇒ **尾部分位数只能给区间，不能给点值**。墙钟受噪声影响小（pc 多次重跑 0.79–0.81 s），所以 A 表的耗时可信、B 表的 max 只能当上界样本。
另：`[step] total_us` 里含插桩自身的开销（每片 2 次 `clock_gettime`），见 §6.5 的换算。

### 2.5 探针清单（源码在仓库外，不进 tests/、不进 src/）

| 探针（`/tmp/empirics/probes/`） | 负载 | 期望 / 用途 |
|---|---|---|
| `pa_arena_nonescape.extc` | 每轮 256 个 `node` 建链+遍历，块作用域结束即丢，永不上根表 | 不进回收运行期；RSS 平坦 |
| `pb_escape_grow.extc` | 100M 节点头插进 `world.head` 且永不摘 ⇒ live 线性 | 测增长速率 / B/节点 |
| `pc_cycle_churn.extc` | 每轮 `new a`、`new b`，双向连成 2-环，上根后立刻 `w.ring = null` | 只有 sweep 能收的环垃圾 |
| `pd_bigobj.extc` | 每轮 `new big`（`[1048576]?mut ref leaf`，8 MiB，huge block）+ 64 个 leaf，随即丢弃 | 大对象分配/回收与分段扫描停顿 |
| `pe_floor_churn.extc` | 活集恒定 1 个对象，200M 次 `p = new node` | 压到 `EXTC_RC_FLOOR` 附近 |
| `pg2_highwater.extc` | phase1 建 5M 长命 node → `[phase] build_done` → sleep 1.2 s → `w.head = null` → `[phase] dropped` → sleep 1.2 s → 10M 轮环 churn | 直接观察"丢光活集后 RSS 是否回落" |

全部生成成功并嵌入预期运行期（`grep -c extc_rc_`）：pa **0**、pb 784、pc 787、pd 788、pe 780、pg2 793。输出内容自校验（例）：pb `B n=100000000 cnt=100000000 sum=4999999950000000`；pc `C n=100000000 made=100000000 dropped=100000000`；scanchunk `sum=549755289600 n=1048576`（与文件头期望值一致）。

---

## 3. 表 A：场景矩阵（默认档 = 编译默认，`EXTC_RC_PAUSE=1` 只观测）

| case | 负载量 N | 墙钟 min/med (s) | 速率 | 峰值 RSS (VmHWM) | 稳态 RSS | 是否无界增长 | exit | 备注 |
|---|---|---|---|---|---|---|---|---|
| pa | 2,000,000 轮 | 1.50 / 1.50 | 1.33M 轮/s | **1.5 MB** | **1.5 MB** | 否（平坦） | 0 | 生成物 0 处 rc 记号 ⇒ 零回收 |
| pb | 100,000,000 节点 | 1.50 / 1.60 | 66.7M 节点/s | 1953.7 MB | —（无稳态） | **是**，≈20 B/节点线性 | 0 | 全程增长的活集 |
| pc | 100,000,000 轮（=200M 次分配） | 0.80 / 0.80 | 250M 分配/s | **2.9 MB** | 2.9 MB | 否（9 点恒定） | 0 | 环垃圾被 sweep 收住 |
| pe | 200,000,000 次分配 | 0.70 / 0.70 | 286M 分配/s | **3.0 MB** | 3.0 MB | 否 | 0 | 活集=1，峰值≈3×FLOOR |
| pd | 200 × 8 MiB 大对象 | 0.30 / 0.30 | 667 大对象/s | 145.7 MB | 145.7 MB | 否 | 0 | huge block 路径 |
| pg（旧探针） | 2M 长命 + 4M 环 churn 轮 | 0.20 / 0.20 | — | 245.2 MB | — | 采样仅 2 点，弃用 | 0 | 已由 pg2 取代 |
| pg2 | 5M 长命 + 10M churn 轮 | 3.914 | — | **316.8 MB** | 316 MB（末段） | 否（但停在历史峰值之上） | 0 | 见 §4 轨迹 |

**pb 的逐点轨迹（rep1，100 ms 采样，MB）**：0.2@0 → 156.8@0.101 → 323.1@0.201 → 354.8@0.301 → 375.5@0.402 → 526.2@0.502 → 1367.4@1.104 → **1953.7@1.806–1.907（峰）** → 1209.1@2.007 → 298.4@2.107。
⇒ 末两点（2.0 s 后）的下降**不是回收**，是进程退出时 `extc_rc_regionFree` 把 slab 一次性 `free` 给 OS：`src/back/reclaim.c:1970` `static void extc_rc_regionFree(ExtcRcRegion *r) {`（释 occ/mark/rsbit/hold/rscnt/tid/ttab/thash 位图、huge block 的 `s->data` 与 slab 本身）。region 存活期间**不还 OS**，只有 free-slot 链复用 —— 这是"RSS=历史峰值"的机制来源。

**pe（压 `EXTC_RC_FLOOR`）**：`EXTC_RC_FLOOR` 默认 `((int64_t)1 << 20)`（`src/back/reclaim.c:141`），`r->floor = EXTC_RC_FLOOR;`（`src/back/reclaim.c:1922`）；分片时按 shard 再除（`src/back/codegen.c:4153-4172` `extc_rc_regionFloor`）。实测峰值 3.0 MB ≈ 3 个 1 MiB slab，**不是恰好 1 MiB**：触发线是 `max(2×live, floor)×scale`（见 §6.2 的 `shouldSweep/thrUpd`），所以"接近 FLOOR"意味着压力线≈下限而不是驻留=1 MiB。

---

## 4. 表 A2：对照档（同一探针、不同编译期开关）

| case | 开关 | 墙钟 min/med (s) | 相对默认 | 峰值 RSS | 相对默认峰值 | 结论 |
|---|---|---|---|---|---|---|
| pb_nc0 | `EXTC_RC_NOCOUNT=0` | 2.71 / 2.81 | **×1.81 / ×1.76** | 2144.6 MB | +9.8% | 计数层直接给 pb +76…+81% 墙钟 |
| pc_nc0 | `EXTC_RC_NOCOUNT=0` | 3.11 / 3.21 | **×3.89 / ×4.01** | 3.0 MB | ≈0 | 环 churn 上计数层最贵（+289…+301%） |
| pe_nc0 | `EXTC_RC_NOCOUNT=0` | 1.50 / 1.50 | **×2.14** | 1.5 MB | −50% | +114% |
| pb_norc | `EXTC_RECLAIM=0` | 0.90 / 0.90 | ×0.60 | 1539.2 MB | −21.2% | **不是纯 GC 税**：分配器整体换成纯 bump arena |
| pc_norc | `EXTC_RECLAIM=0` | 2.41 / 2.41 | ×3.01 | **4615.1 MB** | ×1592 | 逐点线性：266@0.10→542@0.20→830@0.30→1114@0.40→1370@0.50→2624@1.20→4138@2.01（+1.9 GB/s） |
| pc_sh2 | `EXTC_RC_SHARDS=2` | 8.13 / 8.22 | **×10.16 / ×10.28** | **6147.2 MB** | ×2120 | 单线程环垃圾**完全不回收**；user 5.178 s / sys 2.98 s |
| pc_sh4 | `EXTC_RC_SHARDS=4` | 8.53 / 8.83 | ×10.66 / ×11.04 | 6080.9 MB | ×2097 | 同上 |

**pb_norc 的读法**：墙钟比默认快 40%，但峰值只小 21%，且它已经没有回收运行期、分配器换成纯 bump（`src/main.c:473-478`）。所以 pb_norc 的 0.90 s **不能**读成"GC 税 = 0.6 s"；它测的是"另一个分配器"。
**pc_sh2/sh4 的读法**：分片把每个逻辑 region 拆成 N 个运行期 region 各扫各的（`src/main.c:476-478`）。单线程下环垃圾全落在其中一个 shard，其它 shard 的 `[pause]` 显示 `collects=9 / max 4–6 µs`，但**总数停在 9 轮**、RSS 一路涨到 6 GB ⇒ 这是"分片在单线程下不收敛"的直接证据（不是停顿问题，是正确性/活性问题）。分片在仓库门禁 `gate-work` 的 `shard` 形状里是被覆盖的（`n=292 walk_max=127`），但那个形状不制造跨 shard 环。

---

## 5. 表 A3：内存增长速率与单位成本

| 量 | 数值 | 来源 |
|---|---|---|
| pb 每节点 RSS | **≈20 B/节点**（20M→401,424 KB） | `/usr/bin/time -v` `Maximum resident set size`，4 个点 1M/5M/10M/20M 线性 |
| pb 增长速率 | **≈1.30 GB/s**（1953.7 MB / 1.50 s） | 采样 |
| pc_norc 每分配 | **≈24.2 B/分配**（4615 MB / 200M） | 采样 |
| pc_sh2 每分配 | **≈30.7 B/分配**（6147 MB / 200M） | 采样 |
| pb 的分配单价（无回收档） | 1539.2 MB / 100M ≈ 15.4 B/节点 | 说明 slab 元数据在 no-count 档只占 ≈4–5 B/节点 |
| pd 大对象 | 每个 `big` 8 MiB payload（`[1048576]?mut ref`）；200 个 → 145.7 MB 峰值 | 采样 |

slab 常量（决定上面这些单位成本）：`EXTC_RC_SLAB_BITS 20` → `EXTC_RC_SLAB_SIZE ((size_t)1 << 20)`（`src/back/reclaim.c:137-138`、`:316`）；`slabNew` 里 `chunk > EXTC_RC_SLAB_SIZE` 时封顶、并按 `EXTC_RC_SLAB_SLOTS_MIN` 倍增（`src/back/reclaim.c:777-786`）；计数关闭时 hold 表不分配（`src/back/reclaim.c:817-831`），这解释了"no-count 更省内存"的 pb_nc0 +9.8% 反例 —— 见 §6.6。

---

## 6. 表 B：停顿分布

### 6.1 `EXTC_RC_PAUSE=1` 能给什么、不能给什么
`src/back/reclaim.c:2678` 是 `extc_rc_pauseDump`，`src/back/reclaim.c:2715` 是 `extc_rc_pauseArm`，`:2722` `if (extc_rc_pauseOn) atexit(extc_rc_pauseDump);`。它**只给** max/total 与四个累积桶（`over16/over64/over256/over1ms`），**没有 p50/p90/p99 直方图**。而且 arm 只发生在"真要开一轮"的分支之后（`src/back/reclaim.c:2788` `if (!r->marking && (!extc_rc_shouldSweep(r) || !extc_rc_roundOk(r))) return -1;   /* skip:no-trigger */`）⇒ **一次 collect 都没发生的运行完全没有任何 `[pause]`/`[step]` 输出**（pa、pa_norc、pb_norc、pc_norc 全属此类）。这是结构性 0，不是插桩失效。

### 6.2 触发线（决定"多久停一次"）
`src/back/reclaim.c:2646` `static inline int extc_rc_roundOk(const ExtcRcRegion *r)`、`:2650` `static inline int extc_rc_shouldSweep(const ExtcRcRegion *r)`：
```c
return r->bytes > r->thr && extc_rc_roundOk(r);   /* round credit: a separate counter, targeted release does not spend it */
```
（`src/back/reclaim.c:2658`）。`thrUpd`（`src/back/reclaim.c:2636`）把阈值定成 `max(2×live, floor) × scale`，一轮 sweep 一个都没收到时 `scale *= EXTC_RC_SCALE_MUL(4)`，上限 `EXTC_RC_SCALE_MAX (1<<40)`（`src/back/reclaim.c:144,151`）。**含义**：无环时"占用≈live"，压力线永远够不着；只有**环垃圾堆积**才把 `bytes` 推过 `thr` ⇒ pc 每 100M 次分配触发 4577 轮，pb 只触发 5 轮。

### 6.3 汇总（默认档，`EXTC_RC_PAUSE=1`，同一批 `-O2` 二进制）

| case | collects | `[pause]` max µs | `[pause]` total µs | `[step]` n | `[step]` max µs | over16 | over64 | over256 | over1ms | `[worst]` |
|---|---|---|---|---|---|---|---|---|---|---|
| pa | — | — | — | — | — | — | — | — | — | 无运行期 |
| pb 100M | 5 | 4.0 | 10.0 | 106,728 | 118–179（多 run） | 27 | 4 | 0 | 0 | mark/advance budget=256 dur 134，pops=256 marked=256 |
| pc 100M | **4577** | **16–68**（3 rep: 20/24/16；后段 44/68/43） | 4723–4890 | 425,661 | **86–314** | 29–77 | 4–14 | 0–1 | 0 | sweep/alloc budget=8 dur 86–314，sweep_words=8 freed=512 |
| pe 200M | 3051 | 29.0 | 3225 | 417,987 | 105 | 46 | 4 | 0 | 0 | sweep budget=8 dur 105 freed=512 |
| pd 200 | 25 | 5–6 | 24–31 | 643 | **112–256** | **355/643** | 314–350 | 0–1 | 0 | mark/advance budget=0 dur 112–256 **pops=1 marked=0** |
| pg2 | 320 | 2.7 | 385.9 | 6,992,581 | 162 | 206 | 33 | 0 | 0 | sweep/alloc budget=8 dur 162 freed=0 |
| cyc（仓库形状 200k） | 5(pause)/5(probe) | 0.0 | 0.0 | 5 | 9.0 | 0 | 0 | 0 | 0 | — |
| pc_sh2 | 9 | 4–6 | 15–20 | 166,680 | 161–182 | 67–203 | 12–24 | 0 | 0 | mark/advance budget=256 |
| pc_sh4 | 9 | 4.0 | 15 | 83,355 | 182 | 67 | 12 | 0 | 0 | mark/advance budget=256 |

**平均收集耗时** = total/collects：pc ≈1.0 µs、pe ≈1.06 µs、pd ≈1.0 µs、pb 2.0 µs、pg2 1.21 µs；占墙钟 pc 0.59%、pe 0.46%、pb 0.0007%、pd 0.008%。⇒ **collect 级停顿在 µs 量级，与控制堆大小无关**。

`[work]`（确定性工作量，不随调度抖动）关键读数：pc `n=4577 walk_max=1 scan_max=0 push_max=0`；pb `n=67848 walk_max=256`；pd `n=402 walk_max=1`；pg2 `n=1857 walk_max=256`；pc_sh2 `walk_max=200 scan_max=46 push_max=304，walk_gt16=98038 walk_gt64=47324，push_gt16/64=44672 push_gt256=4252`——**分片把单步工作量抬高了近两个数量级**（默认 shard=1 时 walk_max=1）。常量出处：`EXTC_RC_SWEEP_BUDGET 8`（`src/back/reclaim.c:889`）、`EXTC_RC_MARK_STEP 8`（`:908`）、`EXTC_RC_MARK_BATCH_MAX 256`（`:911`）、`EXTC_RC_SEED_WORDS 512`（`:899`）、`EXTC_RC_SEED_PUSHES 256`（`:896`）、`EXTC_RC_MARK_EVERY 64`（`:902`）。门禁 `gate-work.py` 就是拿 `[work]` 判"无界步"的（§7）。

### 6.4 切片级（`[step]`）CDF：只能靠 `EXTC_RC_SLOWSTEP` 阈值扫描
`EXTC_RC_SLOWSTEP=<us>` 在 `src/back/reclaim.c:2390`（mark 切片）与 `:2581`（sweep 切片）打印每一条超阈值的 `[slow]` 行；单位是 µs（`src/back/reclaim.c:2715` 一带把 `atof(getenv)*1000.0` 当 ns 用）。逐阈值重跑数 `[slow]` 行：

| 阈值 | pc（step_n=425,661） | pb（step_n=106,728） | pd（step_n=643） |
|---|---|---|---|
| 0.5 µs | 237,543（55.8%） | 78,210（73.3%） | 460（71.5%） |
| 1 µs | 301 | 59,926（56.1%） | 358（55.7%） |
| 2 µs | 258 | 2,142（2.0%） | 356 |
| 5 µs | 283 | 175（0.16%） | 355 |
| 10 µs | 83 | 108 | 355（55.2%） |
| 20 µs | 41 | 46 | 355 |
| 50 µs | 5 | 13 | 352 |
| 100 µs | 0 | 3 | 7 |
| max（`[step]`） | 95–314 µs | 118–179 µs | 112–256 µs |

⇒ pc/pb 的切片中位数 **<0.5 µs**，>5 µs 只占 0.07–0.16%，>50 µs 约 0.001%。**pd 呈"平台"**：约 55% 的切片落在 50–100 µs 之间 —— 与 huge block 按 `EXTC_RC_SCAN_CHUNK=65536` 字分 16 段扫描吻合（每段 ≈55–100 µs）。
**告警**：①SLOWSTEP 的 `fprintf` 本身扰动后续切片，计数随 run 抖动且**非严格单调**（pc 在 2 µs 与 5 µs 阈值下 258 vs 283 行）；②0.5 µs 档里 pc 有 44% 的切片测得 ≤1 ns（严格大于 + 时钟粗糙）⇒ **不要把 0.5 µs 档当 p50**。

### 6.5 两个插桩器在同一进程里的对照（解释 `[step]` ≠ collect 停顿）
在 `pc` 的生成物上跑 `bench/gc/pause_dist.sh` → `tools/rc-pause-probe.py` 插桩，再带 `EXTC_RC_PAUSE=1` 同时开两个插桩（同一进程两个计数器）：

```text
PROBE sweeps=4577 max_pause_us=0.0 step_n=4577 step_max_us=51.9 step_total_us=1705.2 step_over16=3 step_over64=0
PAUSE_DIST n=0 overflow=0 p50_us=0.0 ...            ← 结构性 0
[pause] collects=4577 max_us=51.9 total_us=1913.2
[step] n=425661 max_us=178.8 total_us=145471.5 over16=37 over64=4
```
**读法**：`PROBE` 的 `step_*` 与 `[pause]` 一致（51.9 µs / 1.7–1.9 ms）⇒ 两者都量 `extc_rc_step` 入口；`[step]` 的 n 是它的 93 倍、total 大 85 倍 ⇒ `[step]` 量的是**切片**，且**大部分切片不在 `extc_rc_step` 调用内**：生成物 `out/pc_cycle_churn.c:925` 是分配点上的 `extc_rc_markStep(r, b);`、`:954` 是 `extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);`（assist 路径），而 `:2492` 的 `extc_rc_step` 只做一片。⇒ **"总回收工作量"要读 `[step]`，单次 collect 停顿要读 `[pause]`/`PROBE step_max`，两者不能混用**。
`[step] total` 的插桩开销估算：425,661 片 × 2 次 `clock_gettime` × ≈20–25 ns ≈ **17–21 ms**；pc 的 `[step] total` 145–195 ms 减掉它 ≈ **124–174 ms 真实回收工作，占 0.80 s 墙钟的 15–22%**（这是"默认 no-count 仍有回收税"的量化，估计值，标注为估算）。

### 6.6 大对象：唯一能突破 ms 级停顿的路径（分段是硬闸）
仓库自带探针 `tests/reclaim-perf/scanchunk.extc`（`struct big { a: [1048576]?mut ref node }`，文件头注释断言：关分段 1226 µs / 默认 65536 字 85 µs）。本机实测（`cc -std=c11 -fwrapv -O2`，`EXTC_RC_PAUSE=1`）：

| 构建 | `[pause]` max | `[step]` max | over1ms | over256 | 墙钟 | 峰值 RSS |
|---|---|---|---|---|---|---|
| 默认（`EXTC_RC_SCAN_CHUNK=65536`） | 5–8 µs | 115 / 72 µs | 0 | 0 | 0.03 s | 32.2 MB |
| `-DEXTC_RC_SCAN_CHUNK=1048576`（等效关分段） | **1010–1044 µs** | **1005–1039 µs** | **1** | **1** | 0.03 s | 32.2 MB |

⇒ (a) 分段把"单步 ∝ 对象大小"压成"单步 ∝ 65536 字"，实测 **≈1.0 ms → ≈100 µs**（同数量级回归，文档的 1226→85 µs 方向一致）；(b) 即便分段，单次 collect 仍可 >1 ms（`[pause] max 1010 µs`），因为一轮 collect 内可能连扫多个切片；**(c) 因此"停顿有界"目前的真实含义是"受 `SCAN_CHUNK` 与一轮内切片数约束"，不是受堆大小或 step 预算约束**。
源码：`EXTC_RC_SCAN_CHUNK 65536`（`src/back/reclaim.c:2146-2150`），超长对象只记 cursor（`:2197`），续扫在 `extc_rc_markResume`（`:2248`）。

---

## 7. 表 C：既有门禁（本机实跑，全绿）

驱动脚本 `/tmp/empirics/gates.sh`（每项 `timeout 900`），日志 `/tmp/empirics/gates/<name>.log`。全套 6 项在数分钟内跑完。**这些数字用各门禁自己的编译选项（如 `-O1`/callgrind），与 §3 的 `-O2` 不可直接比。**

| 门禁 | 结果 | 数值 | 判据 |
|---|---|---|---|
| `tests/reclaim-perf/gate.sh` | **PASS** | 总指令 **82,037,532 Ir**；相对改动前 **−93.0%**（标称 7.0% of 改动前） | `≤ LIMIT 88,029,115` |
| 同上（slotindex 穷举） | **PASS** | 1,433,436 组，不符 **0**，被拒 1,433,499 | 0 不符 |
| `gate-rt-defaults.sh` | **PASS** | 注释声称 `EXTC_RC_NOCOUNT` 默认 1，产物 `#define 1` 一致；通过 1 失败 0 | 声称=实际 |
| `gate-work.py` | **PASS 6/6** | p5 `n=1703 walk_max=256 scan=0 push=0`；p5big `n=7969` 同；cross `n=102 walk_max=256 scan_max=512 push_max=319`；shard `n=292 walk_max=127`；evict `n=45 walk_max=194`；digraph `n=5 walk_max=183`；每形状 3 次逐字相同、无界步 0 | `walk_max≤MARK_BATCH_MAX` 等 |
| `gate-reclaim-tables.sh` | **PASS** | own `S=1024 427.8 Ir`、`S=4096 394.6 Ir`（比 0.923）；ttab `T=8 48.2 Ir`、`T=256 39.8 Ir`（比 0.825） | 比 ≤2.0；绝对 ≤800 / ≤200 |
| `gate-mark-locality.sh` | **PASS** | Ir 143,680,154；**D1mr 616,112**；**DLmr 145,962** | `D1mr ≤700,000`、`DLmr ≤190,000` |
| `gate-slab-scale.sh` | **PASS** | `N=50000 199.5 Ir/分配`（slab 1613）、`N=200000 197.5 Ir/分配`（slab 6452） | 比 ≤1.15 |
| `bench/gc/pause_dist.sh pa_arena_nonescape.c` | **PASS（结构性 0）** | `PAUSE_DIST_STRUCTURAL0 file=... rc_marks=0`，exit 0 | 产物无 rc 运行期 |
| `bench/gc/pause_dist.sh pc_cycle_churn.c` | **插桩成功，但分布为空** | `PAUSE_DIST_INSTRUMENTED rc_marks=787`，运行后 `PAUSE_DIST n=0`（§6.3/§6.5） | 见 §10 存疑 1 |

---

## 8. 这些数据能证明 / 不能证明什么

**能证明**
1. 默认档下"不逃逸的循环分配"零回收开销：生成物无 rc 运行期、RSS 平坦、无停顿（pa）。
2. 逃逸对象的增长是**线性且无界的**，单价 ≈20 B/节点，增速 ≈1.3 GB/s（pb 4 点标定）。
3. **回收不缩容**：region 存活期 slab 不还 OS（`reclaim.c:1970` 只在 drop 时 free），因此 RSS = 历史峰值 × 活跃 slab 的并集；pg2 在丢掉 ≈120 MB 活集后 RSS 保持 142 MB 不变，是直接证据。
4. 环垃圾能被 sweep 收住且 RSS 有界（pc 2.9 MB 恒定），无回收时无界（pc_norc 4615 MB）。
5. 计数层（`NOCOUNT=0`）在三种负载上都是 +76%…+301% 墙钟，属于"默认关闭"的合理选择。
6. 停顿的上界由**预算常量**给出：`SWEEP_BUDGET=8` 字/切片、`MARK_BATCH_MAX=256` pops、`SCAN_CHUNK=65536` 字/段；实测切片 tail ≤314 µs、collect ≤68 µs（除大对象 >1 ms 的 collect）。
7. 大对象分段是**必需**的：关掉后单步 1.0 ms；打开后 ≈0.1 ms（scanchunk 双构建对照）。
8. 六个门禁在本机与当前 HEAD 上全绿，且数字与判据同量级（A 表/C 表口径不同不可混比）。

**不能证明**
1. 不能证明"RSS 严格 = 历史峰值活集"：pg2 的峰值（316 MB）高于 phase1 活集（≈142 MB），因为 sweep 落后于分配，新 slab 在旧 slab 释放前就分配了。只能证明**单调不减**与**退出前不回落**。
2. 不能给出 p50/p99 的**点值**：`EXTC_RC_PAUSE` 无直方图；SLOWSTEP 扫描有打印扰动（§6.4 告警）；且同二进制 run-to-run 尾部抖动 3–4×。
3. 不能把 pb_norc 的 0.90 s 当"无 GC 基线"：它同时换了分配器（`main.c:473-478`）。
4. 不能证明分片能达到"降停顿"：本机单线程环负载下分片既不回收（RSS ×2120）又慢 10×；**分片对停顿的效果在 >1 线程或多个独立 region 的负载上未测**。
5. 不能证明计数层代价的**逐形状**分布：只有 3 个负载的总墙钟（文档 TAXMAP 第 9 节有逐形状数字，本机未复核）。
6. 不能证明 `[step] total` 的绝对值：含插桩 clock 开销（≈17–21 ms 估算）。

---

## 9. 本机数字 vs 文档数字（来源标注）

| 断言 | 文档值（引用） | 本机实测 | 判定 |
|---|---|---|---|
| 打开计数比清扫贵 | +100%~+455%（`docs/plans/TODO.md:65` 逐字：`+100%~+455% —— 钱花在"地址→槽查表 + 旁路元数据读改写"`） | pb ×1.76–1.81、pe ×2.14、pc ×3.89–4.01（+76%…+301%） | **同量级，本机偏低端**；负载不同不可逐点对齐 |
| 大对象关分段 1226 µs / 默认 85 µs | `tests/reclaim-perf/scanchunk.extc:3-4` 文件头注释 | 关分段 **1005–1044 µs**、默认 **5–8 µs（collect）/ ≤115 µs（切片）** | 方向一致；本机 collect 级比注释的"85 µs"更低，切片级接近 |
| sweep 预算 8 words/step | `src/back/reclaim.c:889` `EXTC_RC_SWEEP_BUDGET 8` | `[worst] kind=sweep budget=8 sweep_words=8` | 一致（本机读到） |
| mark 每 64 次分配推进 | `src/back/reclaim.c:902` `EXTC_RC_MARK_EVERY 64` | `[work] walk_max=256 = MARK_BATCH_MAX`、`[worst] mark advance budget=256` | 一致 |
| 默认 no-count | `src/back/codegen.c:13770`、`src/main.c:494` | 生成物 `#define EXTC_RC_NOCOUNT 1`（`gate-rt-defaults.sh` 复核通过） | 一致 |
| 仓库 bench/gc 的 RSS 有 13 MB 地板 | —（本机观察） | `bench/gc/measure.py`：pa `13.0 MB`、pc `13.0 MB`，而 VmHWM 1.5/2.9 MB | 口径缺陷，小进程不可用 |

仓库 `bench/gc/measure.py`（best-of-3，N=20,000,000，输出 `time_ms rss_mb rc`）本机复跑：pa `13967.1/13.0/0`、pb `382.1/392.0/0`、pc `132.8/13.0/0`、pe `55.7/13.2/0`、pb_nc0 `621.6/430.3/0`、pc_nc0 `575.6/13.1/0`、pb_norc `179.7/308.9/0`、pc_norc `370.7/924.1/0`、pa_norc `14251.3/13.0/0`。趋势与 §3/§4 一致（pb_nc0/pb ≈1.63×、pc_nc0/pc ≈4.3×、pb_norc 更快但 RSS 更大）。

---

## 10. 未能测量 / 存疑（必须带进结论）

1. **`PAUSE_DIST` 在本 revision 结构性为空（存疑但证据强）**：`tools/rc-pause-probe.py` 只给 `extc_rc_collect` 记分布，而生成物里该函数是 `__attribute__((unused)) static size_t extc_rc_collect(...)`（`out/pc_cycle_churn.c:2501`）且唯一调用点是 `extc_rc_maybeCollect` 走 `extc_rc_step`（`:2528/2576`）。所以 `max_pause_us=0.0 / PAUSE_DIST n=0`。**未验证**：其他形状（例如 settle 路径 `:2835` 一带）是否会在某些程序里调用 collect；若会，本结论对该形状不成立。
2. **p50/p99 无点值**：只能用 SLOWSTEP 阈值扫描给区间；扫描本身有扰动、非单调。
3. **分片对停顿的效果未测**：只测了单线程环负载（灾难性）。多线程/多 region 分片需另建负载。
4. **`EXTC_RC_TIME=1` 的组件调用次数未采**（需重编译；时间预算内优先做了 A/B/C 表）。
5. **`[ubwork]` 全 0**：除结构性无事件外，尚未构造出触发 `EXTC_RC_SWEEPFREE=1` 事件档的负载 ⇒ 事件档（计数+不清扫）的停顿/内存未测。
6. **未测 `EXTC_ARENA_FILL`、`EXTC_RC_LOCALSTORE=0`、`EXTC_RC_SLAB_BITS=14` 在探针上的表现**（后者只在 gate-slab-scale 里跑过，199.5 vs 197.5 Ir）。
7. **噪声**：测量窗口内有队友并发跑构建/测试（loadavg 0.68–1.10，24 核），停顿尾部的绝对值应视为"含干扰上界"；复现时建议 `nice -n -5` 且无并发。
8. **pd 的输出 `s=0`**：探针只打印 64 个 leaf 的 `v` 之和，未单独校验；不影响 RSS/停顿读数，但"内容正确性"未逐元素校验。
9. **huge block 的 free 行为**：只从 `slabNew`（`reclaim.c:777-843`）与 `regionFree`（`:1970-2008`）读到"drop 时 free"，未用 `valgrind massif` 独立验证其归还时机。

---

## 11. 复现清单

| 文件 | 作用 |
|---|---|
| `/tmp/empirics/probes/{pa_arena_nonescape,pb_escape_grow,pc_cycle_churn,pd_bigobj,pe_floor_churn,pg2_highwater}.extc` | 6 个探针源码（extC） |
| `/tmp/empirics/measure.py` | `posix_spawn` + 100 ms `/proc` 采样 + `wait4`，输出 JSON |
| `/tmp/empirics/matrix.sh` | A/A2 表驱动（14 case × 3 rep） |
| `/tmp/empirics/gates.sh` | 6 个门禁驱动；日志 `/tmp/empirics/gates/*.log` |
| `/tmp/empirics/out/*.c` | 生成物（其中 `pc_cycle_churn.c:925/954/2003/2126/2318/2492/2501/2528` 是本文件多处引用的行号） |
| `/tmp/empirics/out/res/*.json`、`out/gout/*` | 每 rep 的采样与 stderr（`[pause]/[step]/[work]/[worst]` 原文） |

关键复跑命令（停顿插桩）：

```bash
cd /home/alphayang/extC_Compiler
EXTC_RECLAIM=1 ./build/extc -w -I stdlib tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/empirics/out/cyc.c
bash bench/gc/pause_dist.sh /tmp/empirics/out/cyc.c /tmp/empirics/out/cyc.probe.c   # 只插桩
cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 /tmp/empirics/out/cyc.probe.c -o /tmp/empirics/bin_cyc_probe
DIGRAPH_PAUSE_DIST=1 /tmp/empirics/bin_cyc_probe 200000   # N 是运行期 argv
# 分段对照（同一份生成物，只改宏）
cc -std=c11 -fwrapv -O2 -DEXTC_RC_SCAN_CHUNK=1048576 /tmp/empirics/out/scanchunk.c -o /tmp/empirics/bin_scanchunk_nochunk
EXTC_RC_PAUSE=1 /tmp/empirics/bin_scanchunk_nochunk 1048576
```
