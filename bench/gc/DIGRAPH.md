# bench/gc 形状 7：`digraph` —— 错综复杂的有向图 × {extC, 现代 C++, Rust, Go}

> **状态**：**阶段 1（实现 + 编译 + 逐字节对齐）已完成** ✓；
> **阶段 2（计时 / 峰值 RSS）尚未跑** ✗ —— 等 lead 放行（另一个 teammate 的前后墙钟验收在飞）。
> 判据先立：同一 `N`/`seed` 下**四家输出必须逐字节一致**，否则数字不算数 ✗。

**文件**：实现 `src/digraph.{extc,cpp,rs,go}` · 卡具 `run-digraph.sh`（align / bench 两模式）·
量具垫片 `digraph_run.c` · 本报告 `DIGRAPH.md` · 形状表 `SHAPES.md` 第 7 行。

---

## 1. 这个形状想逼出什么

形状 2（`rebuild`）丢的是**无环**结构：引用计数一份不差，追踪 GC 和 RC 的差别只落在
"什么时候还"。真正把两者分开的是**环**：`shared_ptr` / `Rc` 的强引用成环之后，
持有者数永远不归零 ⇒ 不可达的环状垃圾**收不掉**，内存随轮数/规模线性涨 ✓
而追踪 GC（Go）与区域清扫（extC）能把它收回来。

`digraph` 就是**专门制造"不可达的环状垃圾"**的形状：
每个分量按构造是一个**真环**（首尾相接的环组）+ 弦边 + 命名回指，
每轮摘掉一批 handle（= 整个分量变成不可达），然后只遍历存活部分。
**环的闭合边计入 `backedges`，丢弃时还会现场验证该组真的含环（`cycles_dropped`）** ——
不用真算 SCC，也不靠"构造上应该成立"。

---

## 2. 规格（四家逐字一致）

### 2.1 结构与四步

- 入参：`argv[1] = N`（默认 **200000**，下限 256）、`argv[2] = seed`（默认 **20261013**）。
  **不看 stdin**；无浮点格式化；所有输出整数。
- **结点**：`id` · `gid`（所属组）· `payload` · `first`（**变长出边表**，头插单链）·
  `ord`（组内祖先链：新 → 老，结构用）· `back`（**命名回指**，指向组内更老的结点）·
  `gpin`（该结点"被跨组边指过"的单调标记）· 遍历用的 `mark/cmark/snext`。
- **边**：`to`（非空结点）· `next` · `kind`（1=环边 / 2=弦边 / 3=改边，进校验和）。
- **根集**：双向链 handle 表，每个组恰好一个 handle（`target`=环组 hub、`newest`、`size`）。
- **全局枢纽集**：开头建一个 **8 结点、永久存活**的组（`gpin` 全置位）；
  "10% 指向枢纽"就是指向它 —— 这是**跨组边**，会把目标组钉住（见 2.5）。
- 轮数 `R = 16 + N/4096`，每轮建 `B = max(24, N/R)` 个结点（组大小 `k = 3 + rnd%4`，
  组内首尾相接成环 + 每结点 `rnd%3` 条弦边）。四步：
  - **(a) 建**：建若干环组；闭合边 `newest → hub` 计入 `backedges`；
  - **(b) 改**：`M = 2 + rnd%3` 次，随机取一个存活组的随机结点，**删头边 + 加一条边**；
    目标分布 **60% 组内更老 / 20% 组内更新 / 10% 全局枢纽 / 10% 长程随机（可跨组）**；
  - **(c) 丢**：做 `groupsThisRound` 次"丢一个组"；随机取 handle，**优先摘无跨组入边的组**
    （最多探 8 个候选），摘之前**现场验证该组含环**；
  - **(d) 遍历**：从 handle 表出发（显式栈，`snext` 串）遍历存活部分，累加校验和。
- 反 DCE：存活集留到结尾 ✓；每轮遍历都读载荷并累加 ✓；结尾输出依赖全部存活结构 ✓。

### 2.2 PRNG（四家同一序列）

手写 **31 位 LCG**，只用 i64（四家都不溢出）：

```
state = (state * 1103515245 + 12345) mod 2^31      // 取模 = & 2147483647
```

`state` 初值 = `seed & 2147483647`。`rnd(m) = nxt() % m`（m > 0）。无浮点、无库差异。

### 2.3 校验和（**顺序无关的混合式**，不是朴素求和）

朴素求和/计数对"两张不同的图"会碰撞 ✗。这里改成混合式，且对遍历顺序**不敏感**
（避开四家 DFS 顺序的差异），但依赖 id/载荷/边结构/边类别：

```
mix(a,b) = ((a*2654435761 + b*40503 + 12345) & (2^62-1));  mix ^= mix >> 12;  mix &= 2^62-1
每轮: acc  = (acc  + Σ mix(id,payload) + rn*1000003 + re*31) & (2^62-1)
      xacc = xacc ^ ( ⊕ mix(src.id,dst.id) ^ kind*4099 ^ (rn*1000003 + re) ) & 2^62-1
sum  = (acc + xacc) & (2^62-1)
```

⚠️ **残留**：校验和终究是**抽查**而不是证明 —— 它无法排除"两张图恰好同值"。参与对齐判定的
不变量是**多条独立计数**：`nodes`、`edges`、`sum`、`backedges`、`groups_made`、`nodes_made`、
`dropped_groups`、`dropped_nodes`、`rejected_external`、`cycles_dropped`（下面第 4 节逐字节比对整两行）。

### 2.4 输出格式与计数器含义

程序 stdout **恰好两行整数**：

```
nodes=<存活结点数> edges=<存活边数> sum=<校验和>
backedges=<制造的回溯边数> groups_made=<建过的组数> nodes_made=<建过的结点数> dropped_groups=<摘掉的组数> dropped_nodes=<摘掉组里的结点数> rejected_external=<因有跨组入边而拒摘的次数> cycles_dropped=<被摘且现场验证含环的组数>
```

| 计数器 | 含义 | 为什么要有 |
|---|---|---|
| `backedges` | 每建一个环组 +1（闭合边 = 连回祖先） | 证明**环真的被制造** |
| `dropped_groups` / `dropped_nodes` | 被摘掉的组 / 组内结点数 | 证明**不可达分量真的被制造** |
| `rejected_external` | 探测到 handle 因"有跨组入边"而被拒的次数 | 证明丢组**确实要挑**，形状没有悄悄做空 |
| `cycles_dropped` | 摘掉且**现场 DFS 验证含环**的组数 | 证明摘掉的是**环状**垃圾，不是凑数的 DAG |
| `groups_made` / `nodes_made` | 建过的总量 | 与 dropped_* 一起给出"垃圾占比" |

### 2.5 两处设计决定（都是为"四家语义完全一致"服务的）

1. **跨组边 + `gpin` 钉住**：长程（10%）与全局枢纽（10%）可能指向别的组。任何跨组边
   `s→t`（`gid` 不同）都会把 `t.gpin` 置位（单调、永不清理）。丢组的条件就是
   "**该组内没有任何结点被跨组边指过**"。
   - 正确性：若一个组 `g` 从未被跨组边指过，则**没有任何其它组的结点能到达 `g` 的结点**
     （否则那条边会给某个 `g` 结点置位）⇒ 摘掉 `g` 只影响 `g` 自己 ✓
   - 这也让 **careful 档"显式断开"可以只在本组内做**（按 `gid` 收结点），
     不会把别的组的活结点误清 ⇒ 四个实现的"存活集"定义完全一致 ✓
   - 目标只从**存活 handle** 里选 ⇒ 永不指向已摘的组 ⇒ 不会有悬垂引用 ✓
2. **`back` 的语义**：它是"命名回指"（指向组内更老的结点）。extC / Go 没有强弱之分
   （区域清扫 / 追踪 GC 自己处理环）；**C++ `naive` 用 `shared_ptr`、`careful` 用 `weak_ptr`；
   Rust 同理（`Rc` / `Weak`）** —— 这正是要对照的那一维。
   `careful` 档在摘组前显式断开（`first` / `ord` / `back` 全清），环被打破、计数级联归零。

---

## 3. 四个实现 × 档位 + 构建命令原文

| 家 | 档位 | 源文件 | 构建命令（原文） |
|---|---|---|---|
| extC | 默认（回收开、计数**关**） | `src/digraph.extc` | `env -u EXTC_RECLAIM -u EXTC_RC_SHARDS $EXTC -O2 -I ../../stdlib -w -o build/digraph_extc.c src/digraph.extc && cc -O2 -o build/digraph_extc build/digraph_extc.c` |
| extC | `EXTC_RECLAIM=0` | 同上 | `env -u EXTC_RC_SHARDS EXTC_RECLAIM=0 $EXTC -O2 -I ../../stdlib -w -o build/digraph_extc_noreclaim.c src/digraph.extc && cc -O2 -o build/digraph_extc_noreclaim build/digraph_extc_noreclaim.c` |
| extC | **计数开**（`EXTC_RC_NOCOUNT=0`） | 同上 | `env -u EXTC_RECLAIM -u EXTC_RC_SHARDS EXTC_RC_NOCOUNT=0 $EXTC -O2 -I ../../stdlib -w -o build/digraph_extc_count.c src/digraph.extc && cc -O2 -o build/digraph_extc_count build/digraph_extc_count.c` |
| extC | `EXTC_RC_SHARDS=4` | 同上 | `env -u EXTC_RECLAIM EXTC_RC_SHARDS=4 $EXTC -O2 -I ../../stdlib -w -o build/digraph_extc_shards4.c src/digraph.extc && cc -O2 -o build/digraph_extc_shards4 build/digraph_extc_shards4.c` |
| 现代 C++ | `naive`（全 `shared_ptr`） | `src/digraph.cpp` | `g++ -O2 -std=c++17 -DDIGRAPH_NAIVE src/digraph.cpp -o build/digraph_cpp_naive` |
| 现代 C++ | `careful`（回指 `weak_ptr` + 显式断开，`std::vector` 收集） | 同上 | `g++ -O2 -std=c++17 -DDIGRAPH_CAREFUL src/digraph.cpp -o build/digraph_cpp_careful` |
| Rust | `naive`（全 `Rc<RefCell<Node>>`） | `src/digraph.rs` | `rustc -O --edition 2021 src/digraph.rs -o build/digraph_rs_naive` |
| Rust | `careful`（回指 `Weak` + 显式断开，`Vec` 收集） | 同上 | `rustc -O --edition 2021 --cfg careful src/digraph.rs -o build/digraph_rs_careful` |
| Go | `GOGC` 默认 / `GOGC=off` | `src/digraph.go` | `go build -o build/digraph_go src/digraph.go` |

- C++ 无 `new` / `delete` / `free`（只用 `std::make_shared` / `std::shared_ptr` / `std::weak_ptr` /
  `std::vector`）；Rust 无 `unsafe`；四家单线程。
- **careful 档的"断开那一趟"可以单独计时**：`DIGRAPH_BREAK_STATS=1` 时 C++/Rust 的 careful 产物
  把断开累计时间与次数写到 **stderr**（`break_ns=<总ns> break_calls=<次数>`），**stdout 一个字不变**
  ⇒ 不影响四家对齐判据。（extC / Go 没有"显式断开"这一步，无对应量。）
- extC 的 `EXTC_RECLAIM` / `EXTC_RC_SHARDS` 是**编译期**开关（同一份 `.extc` 源，三个产物）。
- 工具链（阶段 1 实测）：`cc`/`g++` = Ubuntu 15.2.0 · `rustc 1.93.1` · `go1.26.0` ·
  extC 编译器 = `build/extc` 的固定副本。**阶段 2 用 `/tmp/gcbench/extc.pin3`** =
  **`69a5b11962a0630735037abb2a840109`**（放行时的当前 `build/extc`；每次换编译器都重新核对 ✓）。
  上游在阶段 1→2 之间连着换了两次（都不是本形状引入，也不是我们能影响的）：
  - `3cf8ae968237ec93a43f12c2629f0943`（pin1，**阶段 1 对齐/测量用的那份**）
    → `ba9d5defea1433bf290aab6ff90c23b0`（pin2；修复"未初始化值"：`check_escape.c:1141`）
    → **`69a5b11962a0630735037abb2a840109`**（pin3；新增 `EXTC_RUN_DIR`，只改 `--run` 写到哪）；
  - **三代编译器下本形状的四份生成物与四个二进制全部逐字节相同**（证据见下）✓。

**extC 回收开关不是空转（阶段 1 编译期证据）**：

| 产物 | 行数 | 生成 C 的 md5（前 16 位） | `EXTC_RC_NOCOUNT` | `storeRefOwned*` 调用点 |
|---|---|---|---|---|
| 默认（回收开、计数关） | 2960 | `e192f36a916e4416` | **1（计数关）** | 73 |
| `EXTC_RECLAIM=0`（地板） | **1630** | `5f7b5437b106e88c` | 宏未定义（运行期不发） | **0** |
| **`EXTC_RC_NOCOUNT=0`（计数开）** | 2960 | `73ad0fc39598969d` | **0（计数开）** | 76 |
| `EXTC_RC_SHARDS=4` | 2960 | `ea0fcc17f384a68f`（与默认不同） | 1 | 76 |

⇒ 默认档**真的发射了回收运行期**；关档**真的不发**；分片旋钮产物不同（非空转）✓

**编译器两连换对本形状的影响：零** ✓（2026-10-13 实测；两代新编译器各比一次）

| 产物 | 生成 C md5（前 16，三代相同） | vs `3cf8ae96…`(pin1) | vs `ba9d5def…`(pin2) | vs `69a5b119…`(pin3) | 二进制 |
|---|---|---|---|---|---|
| 默认 | `e192f36a916e4416` | 逐字节相同 ✓ | 逐字节相同 ✓ | 逐字节相同 ✓ | 三代相同 ✓ |
| `EXTC_RECLAIM=0` | `5f7b5437b106e88c` | 逐字节相同 ✓ | 逐字节相同 ✓ | 逐字节相同 ✓ | 三代相同 ✓ |
| 计数开 | `73ad0fc39598969d` | 逐字节相同 ✓ | 逐字节相同 ✓ | 逐字节相同 ✓ | 三代相同 ✓ |
| `SHARDS=4` | `ea0fcc17f384a68f` | 逐字节相同 ✓ | 逐字节相同 ✓ | 逐字节相同 ✓ | 三代相同 ✓ |

⇒ 两次上游改动（"未初始化值赋初值" + "`EXTC_RUN_DIR` 只改 `--run` 写到哪"）**都没有触碰
digraph 形状的发射路径**；四份生成物、四个 `cc -O2` 二进制在三代编译器下全部逐字节相同
⇒ **阶段 1 的对齐结论对 pin3 同样成立**（放行后仍按协议重跑 30/30 复核 ✓）。
（我**没有**独立验证 `EXTC_RUN_DIR` 的"并行私有目录"行为 —— 那不属于本任务，且验证需要跑 `--run` ✗。）

⚠️ **量具教训（记下，免得下次误判）**：第一次 diff 报了 **930–944 行差异** ✗ —— 那**全是**
`#line` 指令与内嵌报错字符串里的**源文件路径**（我用绝对路径重编，而旧基线是相对路径 `src/digraph.extc`）
⇒ **phantom diff**。**同一形状的两次生成物对比，必须用同一 cwd + 同一路径写法**，否则先修调用方式再谈差异 ✓。

**再补一条（重要，影响归因）**：默认档生成物里是 `#define EXTC_RC_NOCOUNT 1` ⇒
**默认关闭"持有者计数"**，回收只靠**区域清扫**（从根槽/跨区种子出发标记、放掉没标上的）✓
⇒ 赋值路径上**没有**计数税（`#if EXTC_RC_NOCOUNT` 那段把 `holdUpdSlab` 的 inc/dec 整段省掉，
只剩一次 `notePart` + 普通 store）；而 `EXTC_RECLAIM=0` 连清扫都不发射。
（`EXTC_RC_NOCOUNT=0` 才打开计数；本报告把它列为**第 3 档**一起量 —— 见 §6.2。）
运行期另自带两个**只写 stderr** 的证据开关，阶段 2 归因可用（不影响 stdout 对齐）：
`EXTC_RC_STATS=1`（退出时打印 `rs_inc/rs_dec/rs_live/releases`）· `EXTC_RC_TIME=1`（回收分项计时）。

---

## 4. 对齐证据（阶段 1）

卡具：`bash run-digraph.sh align`（每个实现跑一次，逐字节 `diff` 两行输出；**不测时间**）。

**矩阵**：`N ∈ {256, 1000, 2000, 8000, 20000, 50000}` × `seed ∈ {1, 7, 12345, 20261013, 999983}`
= **30/30 组合，10 个产物（extC 四档 · C++ 两档 · Rust 两档 · Go 两 `GOGC` 档）输出逐字节一致** ✓
（原始记录见第 6.4 节；含后来补的 **`EXTC_RC_NOCOUNT=0` 计数开档** —— 语义与回收开关无关，
所以输出必须一字不差 ✓）。

样例（`extC default`，其余 9 个产物与之逐字节相同）：

```
$ ./build/digraph_extc 2000 20261013
nodes=30 edges=61 sum=358410608037337
backedges=455 groups_made=455 nodes_made=2038 dropped_groups=450 dropped_nodes=2008 rejected_external=274 cycles_dropped=450

$ ./build/digraph_extc 8000 20261013
nodes=49 edges=96 sum=2580018542102222
backedges=1783 groups_made=1783 nodes_made=8033 dropped_groups=1772 dropped_nodes=7984 rejected_external=712 cycles_dropped=1772

$ ./build/digraph_extc 50000 7
nodes=26 edges=52 sum=11142820010698236
backedges=11122 groups_made=11122 nodes_made=50030 dropped_groups=11117 dropped_nodes=50004 rejected_external=796 cycles_dropped=11117
```

> 注意：`EXTC_RECLAIM=0` 与 `EXTC_RC_SHARDS=4` 的**输出**也必须逐字节相同（只是回收策略/分片不同）——
> 这三行在阶段 1 全部一致 ✓（语义不因回收开关改变，这也是"性能判据不许靠改语义换"的体现）。

---

## 5. 形状压力证据（"形状真的在干活"）

用 `N=8000 seed=20261013` 的**程序自己的输出**（不是估计）：

| 量 | 值 | 说明 |
|---|---|---|
| `nodes_made` | 8033 | 建过的结点总数（≈ N） |
| `dropped_nodes` | 7984 | **99.4% 建出来的结点变成了不可达垃圾** |
| `dropped_groups` / `groups_made` | 1772 / 1783 | **99.4% 的组被摘掉** |
| `cycles_dropped` / `dropped_groups` | 1772 / 1772 = **1.000** | 摘掉的组**每一个都现场验证含环** |
| `backedges` | 1783 | 每组建 1 条闭合边 = 1783 个真环 |
| `rejected_external` | 712 | 丢组**确实在挑**（40% 的探测碰到被跨组边钉住的组） |
| 存活（结尾遍历） | `nodes=49 edges=96` | 存活集只有一"轮"的量级 |

### 调参前后（lead 点名要的）

丢组时**优先探 8 个候选**（跳过被钉住的）是特意做的。反事实：把探测上限改成 1
（即"随机取一个 handle，被钉住就放弃"），同一个 `N=8000 seed=20261013`：

| 版本 | 存活 `nodes` | `dropped_groups` / `groups_made` | `dropped_nodes` / `nodes_made` | `rejected_external` |
|---|---|---|---|---|
| 探 1 个（调参前） | **444** | 1689 / 1784 = 94.7% | 7591 / 8035 = 94.5% | 94 |
| **探 8 个（当前）** | **49** | 1772 / 1783 = **99.4%** | 7984 / 8033 = **99.4%** | 712 |

⇒ 不做"优先挑无跨组入边的组"，就有 ~5% 的组永远摘不掉 ⇒ **存活集被钉住的组撑大到 444（9×）**，
环状垃圾的绝对值与占比都少一截。当前版本把 `cycles_dropped/groups_made` 顶到 99.4% ✓
（注：两个版本的图状态会因丢组序列不同而不同，这是**设计反事实**，不是同图 A/B。）

---

## 6. 阶段 2：测量口径与结果（**已跑** ✓ 2026-10-13 · core 6 · 编译 `69a5b119…`）

### 6.1 口径（先立）

- 时间 = **best-of-5** 墙钟（`clock_gettime(CLOCK_MONOTONIC)` 只包被测程序）；
- 绑核 = `sched_setaffinity(0, {PIN})`（等价的 `taskset -c PIN`，但省掉 `taskset` 进程自身开销；
  子进程经 `fork` **继承**亲和性）；
- 峰值 RSS = `wait4` 的 `rusage.ru_maxrss` = **内核值**（**不用** `RUSAGE_CHILDREN` 差值 ✗）；
- `rc != 0` 或输出不是"恰好两行且格式对"的样本**一律丢弃** ✗（`run-digraph.sh` 会打印丢弃计数）；
- 至少两个规模：`N` 与 `4N`（看内存是否随规模线性涨）；
- 每张表都带**程序自己的输出**；
- 所有命令原文进报告（`run-digraph.sh` 对每格打印 `cmd:` 行）。

**量具：`digraph_run.c`（自带的极小 C 垫片，~1.5 MB RSS）**。为什么不用更省事的两种：

| 量法 | `/bin/true` 的峰值 RSS | 结论 |
|---|---|---|
| `/usr/bin/time -f %M` | 1476 KB | RSS 准 ✓，但 `%e` 只有 **10 ms** 分辨率 ✗（本形状样本只有几十 ms） |
| **`digraph_run`（本垫片）** | **1480 KB** | 准 ✓，且时间到 ns ✓ |
| python `posix_spawn` + `os.wait4` | 11708 KB ✗ | 解释器 ~10 MB，`CLONE_VM` 下共享 mm 被算进子进程 maxrss ✗ |
| python `fork` + `os.wait4` | 5960 KB ✗ | 同上（exec 前的 COW 也进了 maxrss）✗ |

（这是本会话第 N 次"量具本身比被测对象还大"——所以垫片自己必须小，且校准要贴在报告里。）

命令：
```
SKIP_BUILD=1 EXTC_BIN=/tmp/gcbench/extc.pin1 bash bench/gc/run-digraph.sh
# 放行后先 REPS=1 的 pilot（N=250000 / 4N=1000000），PIN 用 lead 指定的固定核（写进日志）；
# 最终矩阵的 REPS 由 pilot 的单价决定（目标：全矩阵 ≤ ~30 分钟）
# careful 档"断开那一趟"单独计时： DIGRAPH_BREAK_STATS=1 ./build/digraph_cpp_careful N SEED  （只看 stderr）
# extC 反常耗时的分项定位（只写 stderr，不动 stdout）：EXTC_RC_STATS=1 / EXTC_RC_TIME=1 ./build/digraph_extc N SEED
```

### 6.2 结果表（10 档 × {时间 best-of-5, 峰值 RSS max-of-5}）—— **实测** ✓

跑法（原文）：
```
taskset -c 6 env SKIP_BUILD=1 PIN=6 REPS=5 N1=250000 N2=1000000 EXTC_BIN=/tmp/gcbench/extc.pin3 bash bench/gc/run-digraph.sh
```
（先用 `REPS=1` 跑过 pilot；两种规模下 10 个产物**逐字节一致**才计时 ✓；20/20 格全部
`有效 5/5，丢弃 rc=0 lines=0 fmt=0` ✓）

| # | 实现 × 档位 | N=250k best(ms) | RSS(MiB) | N=1M best(ms) | RSS(MiB) | 4N/N **RSS** | 4N/N 时间 |
|---|---|---|---|---|---|---|---|
| 1 | extC 默认（回收开、计数关） | **50.3** | **7.9** | **538.8** | **11.6** | **1.47** ✓有界 | 10.70 |
| 2 | extC `EXTC_RECLAIM=0`（地板） | 46.9 | 34.3 | 612.8 | 132.9 | 3.87 ✗线性 | 13.06 |
| 3 | extC 计数开（`NOCOUNT=0`） | 120.7 | 8.2 | 1136.2 | 12.0 | 1.46 ✓有界 | 9.42 |
| 4 | extC `SHARDS=4` | 117.3 | 60.4 | 866.6 | 231.8 | **3.84 ✗线性（没收住）** | 7.39 |
| 5 | C++ `naive` | 104.9 | 70.6 | 1338.0 | **269.9** | 3.82 ✗线性 | 12.76 |
| 6 | C++ `careful` | 89.7 | 5.9 | 1209.2 | 9.2 | 1.56 ✓有界 | 13.49 |
| 7 | Rust `naive` | 84.8 | 53.5 | 1070.4 | **207.1** | 3.87 ✗线性 | 12.62 |
| 8 | Rust `careful` | 74.8 | 3.8 | 1016.5 | 6.2 | 1.63 ✓有界 | 13.59 |
| 9 | Go `GOGC` 默认 | 47.7 | 7.3 | 514.5 | 8.6 | 1.18 ✓有界 | 10.80 |
| 10 | Go `GOGC=off` | **47.3** | 37.0 | **485.2** | 142.0 | 3.84 ✗线性 | 10.27 |

**另外单列的行（lead 要求 3）** —— 时间差 / 内存差各一列（**实测**）

| 行 | 时间差 | 内存差（峰值 RSS） |
|---|---|---|
| C++ `careful − naive` | 250k **−15.2 ms（−14%）** · 1M **−128.9 ms（−10%）** | 250k **−64.7 MiB（−92%）** · 1M **−260.7 MiB（−97%）** |
| Rust `careful − naive` | 250k **−10.1 ms（−12%）** · 1M **−53.9 ms（−5%）** | 250k **−49.7 MiB（−93%）** · 1M **−200.9 MiB（−97%）** |
| C++ careful "断开那一趟" | **195 ns/次**（10.7 ms 总 / 54859 次）· **241 ns/次**（52.8 ms / 219025 次）= careful 总时间的 **4.4%** | （纯时间项） |
| Rust careful "断开那一趟" | **172 ns/次**（9.5 ms / 54859）· **205 ns/次**（45.0 ms / 219025）= **4.4%** | （纯时间项） |
| **extC 计数税**（计数开 − 默认） | 250k **+70.3 ms（+140%）** · 1M **+597.4 ms（+111%）** | 250k **+0.3 MiB** · 1M **+0.4 MiB** ⇒ **计数没换来释放** ✗（垃圾是环，计数归零收不掉） |
| **extC 回收税**（默认 − 地板 `RECLAIM=0`） | 250k +3.4 ms · 1M **−74.0 ms（默认反而更快）** | 250k **−26.4 MiB** · 1M **−121.3 MiB** ⇒ 这份负载上**清扫是赚的** ✓ |
| **extC `SHARDS=4` − 默认** | 250k +66.9 ms · 1M **+327.7 ms** | 250k **+52.5 MiB** · 1M **+220.2 MiB** ✗（分片在这形状上没界，见 §7.4） |

**每格的程序自己的输出（两行原文，10 产物逐字节一致）**

```
-- N=250000 --
nodes=3260 edges=6555 sum=3446564283351428429
backedges=55572 groups_made=55572 nodes_made=250091 dropped_groups=54859 dropped_nodes=246831 rejected_external=9926 cycles_dropped=54859
-- N=1000000 --
nodes=14077 edges=28166 sum=1191443054750093695
backedges=222130 groups_made=222130 nodes_made=1000455 dropped_groups=219025 dropped_nodes=986378 rejected_external=31837 cycles_dropped=219025
```
⇒ `dropped_nodes/nodes_made` = **98.7%** @250k、**98.6%** @1M；`cycles_dropped == dropped_groups`
（每一个被摘的组都现场 DFS 验证含环）✓ ⇒ 抓到的就是**环状垃圾**。

**extC 运行期分项计数（`EXTC_RC_TIME=1` 诊断档，只写 stderr；次数是精确的）**

| 档位 | alloc | storeOwned | collect(清扫) | slabNew | freeSlot | regionNew |
|---|---|---|---|---|---|---|
| 默认 @250k | 806350 | 20793577 | 67 | 6 | 775465 | 1 |
| 默认 @1M | 3223802 | **219087400** | 146 | 9 | **3166259** | 1 |
| 计数开 @1M | 3223802 | 219087400 | 146 | 9 | 3166259 | 1 |
| `SHARDS=4` @1M | 3223802 | 227980174 | **18** | 191 | **168904** | **4** |

⇒ 默认档 `freeSlot≈alloc`（几乎每个分配都被释放）⇒ 有界是**真的在回收**，不是"没分配"；
`SHARDS=4` 的 `freeSlot` 只有默认的 **5%**、`collect` 只有 **12%** ⇒ 这就是它 231.8 MiB 的机制。
（`EXTC_RC_STATS=1` 另外给出：默认/计数开 `rs_inc=rs_dec=rs_live=0`（单区域、无跨区登记）；
`SHARDS=4` `rs_inc=5674152 rs_dec=2204140 rs_live=3470012 releases=0`。）

### 6.3 每格命令原文（**实测**，卡具逐行打印）

构建命令见 §3；运行命令（每格，`cmd:` 行原文）：

```
env build/digraph_extc          {250000|1000000} 20261013
env build/digraph_extc_noreclaim {250000|1000000} 20261013
env build/digraph_extc_count     {250000|1000000} 20261013
env build/digraph_extc_shards4   {250000|1000000} 20261013
env build/digraph_cpp_naive      {250000|1000000} 20261013
env build/digraph_cpp_careful    {250000|1000000} 20261013
env build/digraph_rs_naive       {250000|1000000} 20261013
env build/digraph_rs_careful     {250000|1000000} 20261013
env -u GOGC  build/digraph_go    {250000|1000000} 20261013
env GOGC=off build/digraph_go    {250000|1000000} 20261013
```
统一由 `build/digraph_run --pin 6 --out <tmp> -- <cmd> <N> <seed>` 起（`fork`+`exec`，
`wait4.ru_maxrss` + `clock_gettime`）；外层 `taskset -c 6` 兜底绑核；
`extC` 编译器 md5 = `69a5b11962a0630735037abb2a840109`（= 当前 `build/extc`）。

### 6.4 原始样本（**实测**）

- 对齐矩阵（10 产物，pin3）：`/tmp/gcbench/phase2/align-sweep10-pin3.txt` = **30/30 PASS**；
  运行前每个规模还各做一次逐字节比对（`matrix.txt` 里 `✓ 全部实现逐字节一致`）✓
- 正式矩阵原始样本（每次的 ms / 内核 rss_kb）：`/tmp/gcbench/phase2/matrix-raw.txt`；摘录 1M：

```
digraph_cpp_naive   1M: 1342.0/276192 1344.9/276380 1364.5/276040 1338.0/276344 1339.7/276180
digraph_rs_careful  1M: 1016.5/ 6284 1052.9/ 6368 1021.4/ 6132 1041.9/ 6396 1027.7/ 6188
digraph_go(off)     1M:  488.9/145364  485.2/145364 494.2/145364 499.4/145364 491.6/145364
```
（全表在文件里；`best-of-5` 的 spread：时间 ~±5%，RSS 基本逐次相同 ⇒ 量具稳定 ✓）

- 阶段 1 的 30/30 对齐（旧编译器）与 pin3 下的 30/30 对齐**输出值完全一致**（同 §4）✓

**重复性复核（同一 grid 连跑两次，第二次是"干净重建 + 全矩阵"）** ✓

| 档位 | 1M best 第 1 轮 | 1M best 第 2 轮 | Δ |
|---|---|---|---|
| extC 默认 | 538.8 | 533.5 | −1.0% |
| extC `RECLAIM=0` | 612.8 | 619.2 | +1.0% |
| extC 计数开 | 1136.2 | 1170.7 | +3.0% |
| extC `SHARDS=4` | 866.6 | 874.3 | +0.9% |
| C++ `naive` | 1338.0 | 1375.7 | +2.8% |
| C++ `careful` | 1209.2 | 1207.1 | −0.2% |
| Rust `naive` | 1070.4 | 1079.9 | +0.9% |
| Rust `careful` | 1016.5 | 1026.2 | +1.0% |
| Go 默认 | 514.5 | 511.5 | −0.6% |
| Go `GOGC=off` | 485.2 | 494.8 | +2.0% |

⇒ 最大偏差 **3.0%**，RSS 逐次几乎不变（±0.3 MiB）⇒ 结论对 ±3% 的噪声不敏感 ✓
（第 2 轮原始样本：`/tmp/gcbench/phase2/matrix-run2.txt`。）

**⚠️ 第 3 轮的排除（污染，不采用）**：加了门控断言后想重测，20:19–20:21 跑了第 3 轮 ——
但那时 `region-proof` 的**语料级扰动普查**已经启动（`ps`：`python3 tests/perturb/census.py --report --jobs 12`、
另有 `bench` 占满一个核、`extc … --run tests/hash/bench.extc`；`uptime` load average **7.43**）✗。
证据：**源码一字未动的 extC 二进制**在 1M 上从 538.8 ms 变成 **747.4 ms（+39%）**，
计数开从 1136.2 → 1716.1 ms（+51%）⇒ 整轮作废（`matrix-run3.txt` 留档，仅作污染证据）。
**正文数字 = 干净窗口里的第 1、2 轮**（互差 ≤3.0%）✓ —— 明确不采用第 3 轮 ✗。

### 6.5 丢弃分类校准（**实测**，`bash run-digraph.sh selftest`）

喂 3 个故意坏掉的生产者 + 1 个好样本，看卡具是否**分因**丢弃（`REPS=3`）：

```
== selftest：丢弃分类校准（REPS=3, PIN=0）==
  好样本 extC        0.853 ms       2.2 MiB   [有效 3/3，丢弃 rc=0 lines=0 fmt=0]
  坏 退出码 3    ERROR：3/3 样本全部丢弃 [rc=3 lines=0 fmt=0]
  坏 只有 1 行   ERROR：3/3 样本全部丢弃 [rc=0 lines=3 fmt=0]
  坏 2 行格式错 ERROR：3/3 样本全部丢弃 [rc=0 lines=0 fmt=3]
```

（这是**校准**，不是形状结果；它证明"rc≠0 / 行数≠2 / 格式不对"三类各自计数 ✓，
而不是像既有 `run.sh` 那样给没跑起来的行照样印时间 ✗。）

### 6.6 哪些是实测、哪些是待填（一眼清单）

| 项 | 状态 |
|---|---|
| 四家 × 全部档位编译通过 | **实测** ✓ |
| 30/30 组合逐字节一致（**10 产物** × 10 不变量，pin3 下重跑） | **实测** ✓ |
| 形状压力计数（98.6–98.7% 环状垃圾、cycles_dropped==dropped_groups） | **实测** ✓ |
| 调参前后（探 1 vs 探 8） | **实测** ✓ |
| 量具校准（`/bin/true` 1480 KB vs `/usr/bin/time` 1476 KB；python 假象 5960/11708 KB） | **实测** ✓ |
| 丢弃分类校准（selftest；正式矩阵 20/20 格 `丢弃 rc=0 lines=0 fmt=0`） | **实测** ✓ |
| extC 四产物非空转（行数/md5/`NOCOUNT`/`storeRefOwned*` 各不相同） | **实测** ✓（编译期） |
| **10 档 × {250k, 1M} 时间 / 峰值 RSS** | **实测** ✓（§6.2） |
| **careful 断开成本**（差值 + `break_ns`/`break_calls`） | **实测** ✓（§6.2 单列行） |
| **extC 计数税 / 回收税 / 分片差** | **实测** ✓（§6.2 单列行 + §7.3） |
| **extC 运行期分项计数**（alloc/store/collect/freeSlot） | **实测** ✓（§6.2 末表） |
| 四问的归因（含"未归因"三条） | **实测 + 明确列出未归因** ✓（§7） |
| 三代编译器下生成物/二进制不变 | **实测** ✓（§3，编译期） |
| **careful 断开后"无外部强引用"断言**（门控档，red→green；54859/219025 次 0 失败） | **实测** ✓（§6.8） |
| 第 3 轮（污染）**已识别并作废** | **实测** ✓（§6.4 证据；不采用该轮数字 ✗） |

> 本表保留进最终报告（lead 要求 ✓）：它让读者一眼分清**预测（§6.7）/ 实测（§6.2–6.6）/ 结论（§7）**。

### 6.7 先立预期（**推理，不是实测** —— pilot 用来给量具做自检）

跑之前先把"如果一切正常，量级该长什么样"写下来，免得 pilot 出来一个反常值就被当结论收下：

- 按 §5 的计数：`nodes_made ≈ N`、`dropped_nodes/nodes_made ≈ 99.4%`、`edges ≈ 2.2 × nodes`、
  存活 ≈ 0.6%·N。于是**引用计数**侧（C++/Rust `naive`）驻留 ≈ 99.4%·N 个结点 + 其边；
  **追踪 / 区域回收**侧（Go 默认 / extC 默认 / 两个 `careful`）驻留 ≈ 存活集 + 各自地板/余量。
- 单结点足迹（64 位，含控制块/`Rc` 头，粗略）：C++ `make_shared<Node>` + 边 ≈ **250 B**；
  Rust `Rc<RefCell<Node>>` + `Rc<Edge>` ≈ **190 B**；Go `*Node` + `*Edge` ≈ **130 B**；
  extC arena 结点 + 边 ≈ **150 B**。⇒ `N=250k`：naive 侧 ~**50–65 MiB**、追踪侧 ~**个位数 MiB**；
  `4N=1M`：naive 侧 ~**200–260 MiB**、追踪侧仍应是**个位数～十几 MiB**。
- 两侧判据都是**斜率**：`4N/N` 的峰值 RSS 比 ≈ **4**（线性、收不掉）vs ≈ **1**（有界、收得掉）。
- 量具地板：Go 运行期 + libc + 页表 ⇒ 小 N 可能看到 **2–5 MiB** 的底；所以小 N 不说明问题，
  必须看 `N` 与 `4N` 的差。

**时间侧预期（同一次 run 的 9 档；⚠️ 全是推理）**

工作量对所有档位**完全一样**（同算法/同 PRNG/同分配次数 ≈ `N` 结点 + `2.2N` 边），
所以时间差只能来自三处：**分配快路径** · **赋值/写屏障税** · **回收工作**。逐档：

| 档位 | 时间预期 | 机制（为什么） | 内存预期 |
|---|---|---|---|
| Go `GOGC=off` | 最快组 | mcache 尺寸类 freelist 分配 + **完全不扫**（无 mark / 无 sweep / 无 assist）；写屏障仍在 | **最差**（线性涨） |
| extC `EXTC_RECLAIM=0` | 最快组 | 纯 arena bump + **计数关闭** + 连回收运行期都不发射 ⇒ 全流程**零回收税** | **最差**（线性涨） |
| Rust `naive` | 次快 | `Rc` 计数**非原子**；环垃圾不收 ⇒ 99.4% 对象**免析构**；但每对象/每边一次 `malloc` | 线性涨 |
| C++ `naive` | 中 | 同 Rust，但 `shared_ptr` 计数是**原子**的（每条边拷贝/析构各一次 RMW）+ `make_shared` | 线性涨（每结点足迹更大 ⇒ 比 Rust 更差） |
| Go `GOGC` 默认 | 中 | 有界，成本付在**并发 mark + 惰性 sweep + 分配 assist**：垃圾多 ⇒ sweep 面积大 | 有界（存活 + GOGC 余量） |
| extC 默认 | **应接近 naive 侧**（若 ≫ naive ⇒ 反常 ⇒ 先查） | bump 分配 + 无计数税；唯一大额开销是**区域清扫**（按内存压力触发、顺序扫位图字） | 有界（存活 + 阈值） |
| **extC 计数开**（`NOCOUNT=0`） | 比 extC 默认**更慢**（方向确定，幅度未知） | 只在赋值路径加计数；**幅度只能靠本形状实测** ✗ | 应仍有界；若明显更低 ⇒ "计数换来释放"成立 |
| Rust `careful` | 慢 | 每摘一组：`Vec` 收集 + 显式清链 ⇒ 99.4% 对象走**完整析构级联**（每对象一次 `free`） | **最好**（≈存活） |
| C++ `careful` | 慢（比 Rust 更慢） | 同上 + `std::vector` + 原子计数 + `make_shared` 块释放 | **最好**（≈存活） |

- **朴素 RC 的画像**：时间靠前、内存最差 —— 计数摊在赋值里、无扫描 ✓，代价是环状垃圾收不掉 ✗。
- **Go 的画像**：有界，把成本付在标记/assist 上；`GOGC=off` **应更快但内存更差** ✓。
- ⚠️ **`bt` in-small 的 `+9.4% Ir`（Lead 数据）不是这里的预期值** ✗：那是**另一个形状 + 另一份输入**
  （赋值密度不同）⇒ 本报告的图负载**计数税完全可能更大** ✓，那**是信息、不是异常** ✓。
  所以上表对"extC 计数开"只写**方向**（更慢、内存不更差就不亏），**幅度留给实测** ✓。
- **extC 的画像**：有界（清扫）且**默认无计数税** ⇒ 时间**应接近 naive 侧**；
  它**不会**靠"提前释放"占便宜（环状垃圾本来就要靠清扫收）⇒ 时间上不该出现"碾压 naive"的惊喜。
- 每轮遍历只覆盖**存活**（≈0.6%·N）⇒ 遍历**不应**主导时间；若某档明显慢，先核它的
  `nodes/edges` 输出有没有异常大（产物两行就是这条的检查）。

**反常值清单（pilot 遇到先查，别当结论）**

| 现象 | 先查什么 |
|---|---|
| `naive` 的 `4N/N` RSS 比 ≈ 1 | 环没收不掉这条不成立 ⇒ 查 drop 是否真发生（`dropped_*` 计数）、是否被别处回收 |
| `careful` 的 `4N/N` RSS 比 ≈ 4 | 显式断开没生效 ⇒ 查 `gid` 过滤、`first/ord/back` 是否真清 |
| extC 默认 ≫ naive（如 >3×） | 大概率是清扫 ⇒ 用 `EXTC_RC_STATS=1` / `EXTC_RC_TIME=1` 的 stderr 分项定位 |
| Go 默认 ≈ Go `GOGC=off` | GC 没被触发（N 太小）⇒ 加大 N 或查 `GOGC` 是否真透传 |
| 某档又快又小 | 怀疑没跑对：看**丢弃分因计数**（rc/lines/fmt）与产物两行是否正常 |

⚠️ 以上是**推理**（由计数、结构尺寸与生成物性质算出），**不是实测**；pilot 若与之显著矛盾
（例如 `careful` 也线性涨、或 `naive` 只有 2 MiB），**先查量具/实现，别当结论**。

#### 6.7.1 预测 vs 实测（**诚实对照**，实测值见 §6.2）

| 预测 | 实测 | 判定 |
|---|---|---|
| naive 侧 @250k ~50–65 MiB、@1M ~200–260 MiB | C++ 70.6→269.9；Rust 53.5→207.1 | **基本命中**（C++ 略高）✓ |
| 追踪侧个位数 MiB、`4N/N` ≈ 1；naive `4N/N` ≈ 4 | careful 3.8–5.9 / Go 7.3 / extC 7.9；比值 1.18–1.63 vs 3.82–3.87 | **精确命中** ✓（这就是本形状的核心判据） |
| Go `GOGC=off` 更快、内存更差 | 485.2 vs 514.5 ms（−5.6%）、142.0 vs 8.6 MiB | **命中** ✓ |
| "naive RC 时间靠前" | 实际 naive **最慢组附近**（C++ 1338、Rust 1070 ms），extC 默认/Go 才是最快（538/514） | **未命中** ✗ |
| "extC 默认应接近 naive，不该碾压" | extC 默认比 C++ naive **快 2.5×**、内存少 23× | **未命中（方向相反）** ✗ |
| extC 计数开"略慢" | **+111%（2.1×）** | **未命中** ✗ |

**为什么三条未命中（→ 这是本轮最有信息量的部分）**：我们把"回收税"当成了主项，
**实测的主项是"不回收的代价"** —— naive RC/`GOGC=off`/`RECLAIM=0` 把 130–270 MiB 常驻下来，
时间主要花在 **mmap/缺页/缓存**上，而不是回收本身；extC 默认/Go 因为回收，**又省内存又更快**。
⇒ "naive 时间最快"这条直觉在**大驻留、缓存不友好**的图形状上不成立 ✗（形状 1/2 那种小驻留负载上才成立）。
⇒ "extC 计数税 = 小百分比"也不成立：本形状 `storeOwned` 219M 次，赋值密度远高于 `bt` in-small ✓
（这正是 lead 提醒的"别拿别的形状的数当预期值"）。

---

### 6.8 诊断：careful 断开的"无外部强引用"断言（**red→green**）

任务书写的是"Rust careful 用 `Rc::strong_count` 断言/断开"。本实现在**默认档不编译**的
门控（`--cfg assert_refs` / `-DDIGRAPH_ASSERT_REFS`）下补了这个断言：断开一组之后，
该组的 `target`/`newest` 强计数必须恰好等于"handle 的槽 + 本函数局部 clone"——
这正是 §2.5 的 `gpin` 不变量（"无跨组入边 ⇒ 该分量独占"）的**可失败**版本。

- **先红** ✓：第一次跑 C++ 档**立刻 abort**：
  `ASSERT-REFS FAIL gid=616 target_count=1 newest_count=1 expect=2`
  ⇒ 查出来是**我自己的期望值写错**（C++ 版没有局部 clone，期望应是 1/2，不是 2/3）。
  这同时证明这个检查**真的会红**（不是装饰）✓。
- **后绿** ✓：修期望值后，4 个组合（Rust/C++ × 250k/1M）全部 `rc=0`、**断言 0 次失败** ——
  在 **54859（250k）/ 219025（1M）** 次"摘组"上，每一次断开后都只剩 handle 的引用 ⇒
  "无外部强引用"这半边**无反例** ✓。
- **门控块不改执行代码**（实测）：把同一份 `digraph.rs` 分别"带断言块 / 去掉断言块"编译，
  反汇编的**助记符序列逐条相同**（75023 条）⇒ md5 的差异只来自 rustc 的 crate 哈希/内嵌路径 ✓。
- **残留（诚实，照实保留）**：该断言只覆盖"断开后没有外部强引用"；"外部引用**本来**就不存在"
  由 `gpin` 单调标记的**构造**保证（§2.5），断言不重复证明这一半。另外门控块虽不改代码，
  但会让 Rust 产物的 md5 变（只要源码变），所以"交付源码 ↔ 第 1/2 轮二进制"**不是逐字节同一份**。
- **lead 裁决（2026-10-13）**：**不做**严格同源重测 ✓ —— 受影响的只是"交付源码 ↔ 已测二进制"的
  md5 一致性；Rust md5 变化来自 **rustc 把整份源码算进 crate 哈希**（不是代码变了），而
  "**助记符序列逐条相同（75023 条）**"是比 md5 更强的"执行代码没变"证据 ⇒ 干净窗口留给语料普查更值。
  ⇒ 本条**残留照实保留**（Rust 两产物 md5 变、C++ 四个仍逐字节相同；以助记符序列 + 输出一致补证）。

---

## 6.9 停顿**分布**（task-21，P1 接线 ✓ + P2 全矩阵 ✓）

> 目的：用户要看的是"**gc 几乎不触发**"的**手感** ⇒ 只看 max/总占比不够 ✗，要**分布**。
> 两阶段：**P1 = 接线 + 自检**（绿点 ✓）· **P2 = 10 臂 × {250k,1M} × REPS=5 × 两轮** ✓（本节）。
> pin：**`/tmp/gcbench/extc.pin3` = `69a5b11962a0630735037abb2a840109`** —— 与 **§6.2 同一 pin** ✓
> （记录：任务书写 `11579427…` 不在盘上；`build/extc` 在 task-21 期间被重建 `a3a8836b → 25a377d5`，
> 生成物已漂 155/155/233 行 ⇒ **本轮一律用 pin3 重出**，见 §6.9.6）。

### 6.9.1 口径（三家不同，**分开列，不许混比** ✗）

| 家族 | 停顿是什么 | 取自 | 是否 STW |
|---|---|---|---|
| extC 四档 | **一次区域清扫**（`extc_rc_collect` 进→出的墙钟） | `tools/rc-pause-probe.py` 的 `PAUSE_DIST`（门控 stderr）；既有 `PROBE`/`[pause]` 行互证 | 是（无并发、无写屏障） |
| Go 两档 | 每个 GC 周期的 **STW**（`a+b+c ms clock` 的 a+c；b 是并发标记） | `GODEBUG=gctrace=1` 逐周期 + 退出前 `ReadMemStats` 互证（`go1.26.0`） | 是（并发标记/清扫不在内） |
| C++/Rust `careful` | **显式断开那一趟**（收集 + 清链 + 级联析构） | `DIGRAPH_BREAK_STATS=1` 的 `BREAK_DIST` | **不是 STW** ✗（单线程程序没有"停别的线程"这回事） |

**反瞎判据**：每臂必须给出**可解析的统计行**；缺行 ⇒ FAIL ✗，但 **`NumGC=0` / "结构性 0 次清扫"是合法的值** ✓。
自检（**两层都证明会红**）：`pause_stats.py selftest` ⇒ 3 个缺行输入全 red ✅；把 extC 默认的 probe 副本换成普通二进制
⇒ harness 报 `FAIL：1/1 个样本缺停顿统计行` ✅（换回即绿）。

### 6.9.2 分布表（**实测**：每臂每规模把 REPS=5 次跑的逐次时长**池化**；括号内 = 第 2 轮）

**extC 清扫（µs；`<1/<4/<16/<64/<256µs/<1ms/≥1ms` 为累计桶）**

| 臂 | N | 样本数(池化) | p50 | p90 | p99 | max | 桶 | 总停顿(单跑 max-of-5) | 占总墙钟 |
|---|---|---|---|---|---|---|---|---|---|
| extC 默认 | 250k | 335 | **111.1** (115.0) | 220.0 (221.0) | 488.8 (529.0) | 880.3 (605.0) | `0/0/0/0/310/335/0` | 10.58 ms | **19.2%** |
| extC 默认 | 1M | 730 | **258.5** (263.6) | 997.2 (1116.8) | 1673.0 (1911.1) | 2316.0 (3217.0) | `0/0/0/0/364/657/73` | 63.95 ms | **11.1%** |
| extC `RECLAIM=0` | 250k/1M | 0 | — | — | — | — | — | 0 | 0 |
| extC 计数开 | 250k | 335 | 126.8 (127.1) | 244.5 (247.5) | 515.6 (562.8) | 771.1 (664.3) | `0/0/0/0/305/335/0` | 11.39 ms | 8.7% |
| extC 计数开 | 1M | 730 | 268.3 (303.1) | 1124.6 (1204.2) | 1732.7 (1938.5) | 2234.5 (2715.9) | `0/0/0/0/357/629/101` | 70.70 ms | 5.8% |
| extC `SHARDS=4` | 250k | 65 | **1056.0** (1138.0) | 4457.0 (4208.3) | 8154.7 (8007.2) | 8614.3 (8557.8) | `0/0/0/0/5/31/34` | 28.61 ms | 22.3% |
| extC `SHARDS=4` | 1M | 90 | **1950.5** (1941.9) | 16345.4 (16543.2) | 27119.3 (28118.4) | 29585.2 (30807.3) | `0/0/0/0/4/32/58` | 99.85 ms | 11.2% |

**Go 每周期 STW（µs；`n` = 5 次跑的周期总数，与 `NumGC` 一致）**

| 臂 | N | 样本数 | p50 | p90 | p99 | max | 桶 `<10/<50/<100/<250/<500/<1ms/≥1ms` | 总停顿(单跑) | 占总墙钟 |
|---|---|---|---|---|---|---|---|---|---|
| Go 默认 | 250k | 55（11 周期/跑） | **7.0** | 10.0 | 16.0 | 18.0 (19.6) | `45/55/55/55/55/55/0` | 0.090 ms | **0.20%** |
| Go 默认 | 1M | 271（54 周期/跑） | **11.0** | 15.0 | 29.0 | 63.0 | `102/270/271/271/271/271/0` | 0.953 ms | **0.19%** |
| Go `GOGC=off` | 250k/1M | **0**（显式 0 ✓） | — | — | — | — | — | 0 | 0 |

**C++/Rust `careful` 断环那一趟（ns；⚠️ 非 STW；`n` = 全部断开次数）**

| 臂 | N | 样本数 | p50 | p90 | p99 | max | 桶 `<100ns/<200/<500/<1µs/<4µs/<16µs/≥16µs` | 总耗时 | 占墙钟 |
|---|---|---|---|---|---|---|---|---|---|
| C++ careful | 250k | 54859 | **188–196** | 263–278 | 384–438 | 54k–327k | `~1.8k/30k/54.6k/54.82k/54.84k/54.85k/≈6` | 10.6–12.1 ms | 11.6–12.1% |
| C++ careful | 1M | 219025 | **216–228** | 332–370 | 592–711 | 128k–739k | `3.1k/88k/213k/218.6k/218.9k/219.0k/≈35` | 52.3–59.2 ms | 4.5–4.6% |
| Rust careful | 250k | 54859 | **168–173** | 235–249 | 330–384 | 94k–312k | `3.5k/37.8k/54.7k/54.82k/54.84k/54.85k/≈7` | 9.7–10.5 ms | 12.1% |
| Rust careful | 1M | 219025 | **191–194** | 287–307 | 473–541 | 67k–374k | `7.3k/118k/216.5k/218.7k/218.95k/219.0k/≈26` | 44.9–47.6 ms | 4.4–4.5% |

**C++/Rust `naive`**：`n/a` —— 引用计数只做计数归零、**不扫描**，没有对应的停顿事件（不是"缺数据"）。

### 6.9.3 直方图（extC，池化；按上面 7 个桶）

```
extC 默认  @250k  <256µs ████████████████████████████████████████████ 310/335  ·  <1ms 335  ·  ≥1ms 0
extC 默认  @1M    <256µs ████████████████████████████ 364/730            ·  <1ms 657  ·  ≥1ms 73
extC 计数开 @1M    <256µs █████████████████████████████ 357/730          ·  <1ms 629  ·  ≥1ms 101
extC 分片4 @1M    <256µs 4/90 · <1ms 32/90 · ≥1ms ██████████████████ 58/90（p50 就 ~2 ms）
```
（`<1/<4/<16/<64µs` 四档**全为 0** ⇒ 没有"亚 64µs 的清扫"这种东西；最小的也在 100 µs 量级。）

### 6.9.4 预登记预期 vs 实测（**Lead 的预期未命中，照实写** ✗→信息 ✓）

| 臂 | Lead 预登记 | 实测 | 判定 |
|---|---|---|---|
| extC 默认 | max **µs 级**、绝大多数 **< 16 µs** | @250k **p50 111 µs / max 880 µs**；@1M **p50 258 µs / max 2.3–3.2 ms**；`<16µs` **一个都没有** | **未命中 ~100–1000×** ✗ |
| extC 默认总占比 | "很小" | @250k **19.2%**、@1M **11.1%** 的墙钟 | **未命中** ✗ |
| Go 默认 | STW 亚毫秒、次数多、总占比不高 | @1M **54 周期**、p50 11 µs、max 63 µs、总 0.95 ms（**0.19%**） | 方向命中 ✓（"亚毫秒"✓、"次数多"✓、占比**极低**✓） |
| C++/Rust careful | ≈4.4% 墙钟在断环 | @1M **4.4–4.6%** ✓（@250k 11.6–12.1%，因为总时间更短） | **命中** ✓ |

**为什么会差 ~100×**（Lead 已确认原因 ✓）：预登记是拿**环形状**（区域极小、最大单次清扫 2.3–2.9 µs）类比的 ✗；
`digraph` 是**大区域 + 98.6% 环垃圾** ⇒ 每次清扫要走很大一片 ✓。这条已由 Lead 记进 CHANGELOG 并列为新待办口径
（候选修法 = `task-20` 的**专属区域**：区域小 ⇒ 要扫的面也小）。

### 6.9.5 ⚠️ 本轮最要紧的发现：**这形状上 extC 的"停顿手感"明显比 Go 差** ✗

| | extC 默认 | Go 默认 | 比 |
|---|---|---|---|
| @250k 停顿次数 | **67 次清扫** | **11 个 GC 周期** | 6× 多 |
| @250k 单次 p50 / max | **111 µs / 880 µs** | **7 µs / 19.6 µs** | ~16× / ~45× |
| @250k 总停顿占墙钟 | **19.2%** | **0.20%** | **~96×** |
| @1M 停顿次数 | 146 次 | 54 周期 | 2.7× |
| @1M 单次 p50 / max | **258 µs / 3.2 ms** | **11 µs / 63 µs** | ~23× / ~51× |
| @1M 总停顿占墙钟 | **11.1%** | **0.19%** | **~58×** |

- Go 的 STW **对负载敏感**（同一二进制另一轮 max 40 µs ↔ 466 µs ✓）⇒ 上表取的是**两轮中更好的安静窗口**，
  但**结论方向不受影响**：Go 的 p50 也在 10 µs 量级、总占比 0.2% 量级。
- **extC `SHARDS=4` 更糟**（@1M max **29.6–30.8 ms**、p90 16.3 ms、p50 1.95 ms）⇒ 与 §7.4/task-13 的"分片没收住"同源；
  分片把一次大清扫**摊成了更长的时间**（且释放更少，见 §6.2）。
- 这正是"**gc 几乎不触发**"的量化：**Go 的手感是"几乎不触发"（0.2%）**；**extC 是"触发 67–146 次、每次百 µs 到 ms 级"** ✗。

### 6.9.6 pin / 可追溯性 / 命令原文（**实测**）

**pin 与 md5**（`/tmp/gcbench/pause/manifest.txt` 留档）：
- extC 编译器 = `/tmp/gcbench/extc.pin3` **`69a5b11962a0630735037abb2a840109`**（= §6.2 用的同一 pin ✓）；
  四份生成物与 pin3 基线**逐字节相同** ✓（`e192f36a… / 5f7b5437… / 73ad0fc3… / ea0fcc17…`）。
- 臂二进制 md5（节选）：`digraph_extc 98ffced9…` · `digraph_extc_noreclaim 806847af…` ·
  `digraph_extc_count 81bd3f0b…` · `digraph_extc_shards4 d6f41afd…` · `digraph_cpp_naive 263e56d4…` ·
  `digraph_cpp_careful b0b33495…` · `digraph_rs_naive 409e101f…` · `digraph_rs_careful 8451bdea…` ·
  `digraph_go ec05136c…` · probe 副本 `aba1741d… / 7e431648… / 8bd1b599…` · 垫片 `9b2bdd7d…`
- 工具链：`cc`/`g++` 15.2.0 · `rustc 1.93.1` · **`go1.26.0`**（gctrace 格式随版本变 ⇒ 记版本）· kernel 6.18 WSL2。
- ⚠️ **C++/Rust 臂的二进制与 §6.2 时不同**（§6.2 测量后我给 `careful` 加了 `BREAK_DIST`、给 Rust 加了门控断言）
  ⇒ 时间/RSS **不重测**（§6.2 保留），但 §6.9 的"断环分布"数字来自新二进制；门控开的计时读数本身包含
  一对 `clock_gettime`（~40–60 ns/次）⇒ 对 11 ms 量级的总量是 2–4% 的仪器开销，已在表里注明。

**每格命令原文**

```
extC 默认   : env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1 build/digraph_extc.probe        {250000|1000000} 20261013
extC 计数开 : env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1 build/digraph_extc_count.probe   {N} 20261013
extC 分片4  : env EXTC_RC_PAUSE=1 DIGRAPH_PAUSE_DIST=1 build/digraph_extc_shards4.probe {N} 20261013
extC 地板   : env build/digraph_extc_noreclaim {N} 20261013      （结构性 0：不发射回收运行期）
C++ careful : env DIGRAPH_BREAK_STATS=1 build/digraph_cpp_careful {N} 20261013   （非 STW）
Rust careful: env DIGRAPH_BREAK_STATS=1 build/digraph_rs_careful  {N} 20261013   （非 STW）
Go 默认     : env -u GOGC GODEBUG=gctrace=1 DIGRAPH_GO_STATS=1 build/digraph_go {N} 20261013
Go off      : env GOGC=off GODEBUG=gctrace=1 DIGRAPH_GO_STATS=1 build/digraph_go {N} 20261013
```
统一由 `build/digraph_run --pin 6 --out <tmp> --err <tmp> -- <cmd> <N> <seed>` 起（外层 `taskset -c 6`）。
探针链：`$EXTC -O2 -I ../../stdlib -w -o build/<t>.dist.c src/digraph.extc` →
`python3 ../../tools/rc-pause-probe.py build/<t>.dist.c > build/<t>.probe.c` → `cc -O2 -D_POSIX_C_SOURCE=200809L`。
两轮的 `uptime/load` 与时刻都记在 `manifest.txt`；本轮跑分在 Lead 指定的「机器已空」窗口内、core 6、`slab-region` 停手。

### 6.9.7 可选消融：那 **2.19 亿次** `storeOwned` 记账值多少 ms（**只归因、不进树** ✓，`src/` 未动）

问题："extC 默认为什么比 Go 慢 ~5%？"（538.8 vs 514.5 ms @1M）。
做法：把 pin3 生成物复制到 `/tmp`，把 `extc_rc_storeRefOwned` / `…Reg` 的**记账**（`notePart`/`partN`/跨区登记/计数）
删成空操作，**保留 `*slot = v` 存指针** ⇒ stdout 与原档**逐字节相同** ✓（语义没动，只少记账）；
交错 A/B、`taskset -c 6`、各 5 轮取 best：

| N | 原档 best | 消融档 best | Δ | 占原档 |
|---|---|---|---|---|
| 250k | 51.5 ms | 49.8 ms | **−1.7 ms** | 3.3% |
| 1M | 540.6 ms | 527.6 ms | **−13.0 ms** | 2.4% |

调用点核对：`notePart` 5→3、`holdUpdSlab` 6→2、`rsIncSlab` 5→1（其余调用点在别处，未消融）。
**读法**：2.19 亿次记账 ≈ **13 ms**（1M）—— 相对 extC↔Go 的 **24.3 ms** 差距解释掉约**一半** ✓；
**其余 ~13 ms 未归因**（候选：清扫/标记本身、分配与清零路径、代码结构；没有逐项拆 ✗）。
⇒ 这条**推翻**了"记账是 extC 慢的主因"的直觉 ✗（绝大多数记账被内存停顿掩盖了）。

**兼容性证据（不破既有门禁）** ✓：`PROBE sweeps=… max_pause_us=… max_mark_us=… max_sweep_us=… total_pause_us=…`
仍在 **stdout**、字段原样（`tests/reclaim-perf/gate-shard-cycles.sh` 读 `max_pause_us=`）；
新分布 `PAUSE_DIST` **只在门控 stderr**（门控关 ⇒ 0 行）；`all=[…]` 仅在 n ≤ 1024 时给（便于池化），n 大时退回 `first/last`。
**未改 `tests/`** ✓。10 臂 stdout 在**每个规模**都逐字节一致 ✓（两轮各自的一节 `✓ 全部实现逐字节一致`）。

---

## 6.10 JIT 家族：Java(JDK25) + Node(v24)（task-23；**阶段 1 对齐 + 阶段 2 冷/暖与事件采集都已完成** ✓）

> **§6.2 / §6.9 一个字都没改** ✓（本节的臂与数字独立成节）。extC 行仍用 **pin3 `69a5b119…`** ✓。

### 6.10.1 三个移植坑怎么处理的（**都靠对齐判据当场验** ✓）

| 坑 | 参照语义 | Java | Node（`bigint` 路） | Node（`split` 路） |
|---|---|---|---|---|
| LCG mod 2³¹ | `(s*1103515245+12345) & 2147483647` | `long` 算同一式 ✓ | **`Math.imul`** + `& 2147483647` ✓ | 同左 ✓ |
| `mixv` 掩 62 位 | `(a*2654435761+b*40503+12345) & (2^62-1)` | `long` 精确 ✓ | 全程 `BigInt` 精确 ✓（**慢 ⇒ 可能冤枉 Node** ✗，报告里注明） | 拆 `(hi,lo)` 两个 32 位半；**只在最后打印**用一次 `BigInt` 转字符串 ✓ |
| `x >> 12` | 参照里 x **先被掩到 < 2^62 ⇒ 符号位恒 0** ⇒ 算术右移 == 逻辑右移 | `long` 的 `>>` 即算术，正数等价 ✓ | `BigInt` 的 `>>` 即算术 ✓ | 用"高位 `hi>>>12`、低位 `((hi<<20)\|(lo>>>12))>>>0`"实现（等价于算术右移；**若参照哪天不再掩码 ⇒ 必须改成手动符号扩展** ✗，源码里写了这句警告） |

`split` 路的热路径（`mixv`/62 位加/异或）**全部内联**（不留每调用一次的数组分配 ⇒ 否则量到的是 V8 的 GC 而不是算术差异 ✗）。

### 6.10.2 预热口径（**定稿** 2026-10-14，先立后测 ✓）

**`WARM=<n>` 默认 0 ⇒ 现有行为逐字不变** ✓：同一进程内跑 `n+1` 遍负载、**只打印最后一遍**的结果
（每遍 `resetAll()` ⇒ 结果与 n 无关 ✓，已用 `WARM=0/2/3` 逐字节相同验过）。`WARM_STATS=1` 把每遍耗时
写 **stderr**（`WARMSTAT warm=n iters_ms=…`）—— 插桩只写 stderr ✓。

**稳态判据（写死，可判定）**：

1. **暖值 = 末遍耗时**（`iters_ms` 最后一个数）；
2. **稳态成立 = 末遍与倒数第二遍互差 ≤ 5%**；不成立就**照实标"未达稳态"**（不减 k、不裁数据 ✗）；
3. **k 定为 3** —— 依据是初步数据：Java 冷 4.15 → 2.36/2.53 ms、Node split 5.10/5.94 → 2.26 ms
   ⇒ **前两遍都还偏慢**，取 3 遍热身（若阶段 2 实测仍未达稳态，就**在报告里写明并加大 k 重跑那一格**）；
4. **全序列 `iters_ms` 一律贴出**（冷/暖各一列，不藏中间值 ✓）。

**冷值口径**：`WARM=0`、**每个样本一个新进程**、best-of-5、core 6 ⇒ **冷值 = JVM/V8 启动 + 第一遍**
（表里必须注明 ✓）。暖值 = `WARM=3` 的**末遍**（同进程，不含启动）。

**启动单独量（把冷值拆开）** —— 两个空探针 `src/empty.{java,js}`：

| 探针 | 构建 / 运行 | 5 次启动（ms，**预采**） |
|---|---|---|
| Java 空程序 | `javac -d build/classes src/empty.java` · `java -cp build/classes empty` | 19.3 / 20.6 / 21.2 / 21.5 / 22.4（min **19.3**） |
| Node 空脚本 | `node src/empty.js` | 40.5 / 40.5 / 41.0 / 41.3 / 41.7（min **40.5**） |

⇒ 于是 `冷 − 启动 ≈ 第一遍`、`暖 ≈ 稳态做活`，冷/暖差能**归因**（不是"JIT 变快了"一句话 ✗）。

**为什么表里必须"两列"（含启动 / 扣启动）**：**Node 的启动 40.5 ms 已经 ≈ Go 跑完整个 @250k 的 47.7 ms**
（§6.2）⇒ 小 N 上"冷值"**几乎全是启动** ✗，只给一列会把"JIT 慢"和"运行时启动慢"混在一起误导读者 ✗。
故 §6.10.5 每个冷/暖各给两列：**含启动**（用户真实感受到的）与**扣启动**（算法/运行时的稳态差异）。
⚠️ 上面两行是**预采**（当时 load average ≈2.4，`slab-region` 的电池可能仍在跑）⇒ **阶段 2 在「机器已空」
窗口复核后以那时的数为准**。

**AOT 臂的"第一轮是否偏慢"复核（免费，用已落盘样本）** ✓：`/tmp/gcbench/phase2/matrix-raw.txt`、
`matrix-run2.txt`、`/tmp/gcbench/pause/round{1,2}-raw.txt` 里按格算 **rep1 / 中位数**：
**20+ 格全部落在 0.96–1.05**，`>1.15` 的格 **0 个** ⇒ **四家 AOT 都没有"第一次偏慢"** ✓
（与 Lead 的粗看一致；`matrix-run3` 是污染轮**不采用**）。⇒ **预热问题只属于 JIT 家族** ✓。

### 6.10.3 阶段 1 对齐（**实测** ✓）

- 13 臂（原 10 + **Java** + **Node bigint** + **Node split**）在 `N∈{256,1000,2000,8000,20000,50000}` ×
  `seed∈{1,7,12345,20261013,999983}` = **30/30 组合 stdout 逐字节一致** ✓（记录 `/tmp/gcbench/pause/align23.txt`）。
- `WARM=0/2/3` 下 Java 与 Node（两模式）**stdout 逐字节相同** ✓。
- 环境：`java` = **openjdk 25.0.4.1** ✓、`node` = **v24.21.0** ✓（**零依赖，不用 npm** ✓）；
  构建 `javac -d build/classes src/digraph.java`、运行 `java -cp build/classes digraph N seed` /
  `node src/digraph.js N seed`（`DIGRAPH_JS_MODE=bigint|split`）；卡具垫片改成 `execvp`（`java`/`node` 走 PATH）。

**JIT 预热的初步观察（单次，⏳ 不是结论）**：`WARM=2` 时 N=20000 —— Java `iters_ms=4.152, 2.360, 2.530`
（冷遍 **1.6–1.8×** 于暖遍）、Node split `5.097, 5.936, 2.258`（**前两遍都慢**）。⇒ "jit 要预热"这条**在数据里** ✗→✓。

### 6.10.4 GC 停顿 vs JIT 编译事件（**两列分开** ✓ 实测）

**流路由（实测，2026-10-14）** —— 这决定了插桩怎么写才不破坏 stdout 判据：

| 运行时 | 开关 | 实测流向 | 结论 |
|---|---|---|---|
| Java | `-Xlog:gc` | **stdout** ✗（默认） | **必须写 `-Xlog:gc:stderr`** ✓（实测 stdout=0、stderr=10 行） |
| Java | `-Xlog:jit+compilation` | stdout ✗ | **写 `-Xlog:jit+compilation:stderr`** ✓ |
| Node | `--trace-gc` | **stdout** ✗ | V8 本 build **没有**可用的文件日志：`--logfile`/`--log-all`/`--log-code` 实测**不产生日志文件**，而 `--log-all` 还会把 deopt 行喷到 stdout ✗ ⇒ **诊断轮只能让 trace 走 stdout，由卡具把 trace 行剥掉**再比产物两行 ✓ |
| Node | `--trace-opt` / `--trace-deopt` | **stdout** ✗ | 同上（剥掉后解析） |

⇒ **默认路径（不加任何 trace/日志开关）一字不变** ✓：诊断轮只用于 §6.10 的四列采集，13 臂的
stdout 逐字节一致判据不受影响；Node 的 trace 行由卡具按前缀剥离（**这一点在 §6.10.5 表下注明** ✗）。

**解析器** ✓：`bench/gc/jit_stats.py`（`java-gc|java-jit|node-gc|node-opt`），已用**真实格式**验证：

```
JAVA_GC  n=3 total_ms=3.477 p50=0.324 max=3.130  JAVA_GC_KINDS Cleanup=1 Full=1 Remark=1
JAVA_JIT events=4 total_bytes=82
NODE_GC  n=2 total_ms=1.110 max=0.580  NODE_GC_KINDS Scavenge=2
NODE_JIT compiled=4 marking=6 targets=MAGLEV:4 modes=ConcurrencyMode::kConcurrent:4 deopts=-
```
（上面是**探针小程序**的日志，**不是基准数据** ✗，只用来证明解析与"事件确实采得到"。）

**V8 的编译在后台线程** ✓ —— 实测 `--trace-opt` 的编译事件带 `mode: ConcurrencyMode::kConcurrent`
（并发编译），所以**主线程的停顿里看不到 JIT**；报告会把"编译事件数/目标/并发模式"单列，
而**不**把 JIT 编译并进 GC 停顿列 ✗（两列分开，正是任务书的要求）。

**诊断轮"剥 trace"的判据（写死，lead 要求 ✓）**

1. **锚定格式剥**（前缀匹配）—— **禁止**任何松规则（"含空格就丢" ✗、"行数少两行就算" ✗）。
   实测锚定集（对真实日志验证过覆盖率）：
   | 家族 | 锚定前缀 |
   |---|---|
   | Java GC | `^\[[0-9.]+s\]\[(info\|warning\|error)\]\[gc` |
   | Java JIT | `^\[[0-9.]+s\]\[(info\|warning\|error)\]\[jit` |
   | V8 GC | `^\[[0-9a-fx]+:[0-9a-fx]+\] +[0-9]+ ms: ` |
   | V8 opt/deopt | `^\[(completed \|)compiling \|^\[marking \|^\[completed optimizing \|^\[bailout \|^\[deoptimiz` |
2. **剥完必须**：① 恰好 **2 行**且匹配 `^nodes=…` / `^backedges=…`；② 与**同一臂、同一 N/seed 的普通轮 stdout**
   （**同一会话现取**）**逐字节相同** ⇒ 不一致就**报错** ✗（**不许**"硬比一下、不一致也算过"）。
3. **覆盖率自检**：诊断日志里**未被锚定集匹配的行必须为 0** —— 否则意味着有没识别的 trace 前缀 ⇒ FAIL ✗。

**这条判据抓过两次真错（red→green ✓✓）**：① 第一版 V8 opt 锚定集漏了 `[completed optimizing …`
（`--trace-opt` 实跑的 21 行里 **1 行未匹配** ✗ ⇒ 那行会被当成"产物"留下）；补进锚定集后
**V8 GC 3/3 + V8 opt 21/21 = 100% 覆盖** ✓；合成输入（产物 2 行 + 46 行真实 trace）剥完 → **恰好 2 行**，
且 sha 与参考产物**逐字节相同** ✓（`6cb24ab54278790d`）。
② 实跑 `--trace-opt --trace-deopt` 时又抓到**间歇出现**（~25% 的轮）的 `[aborted compiling … because: …]`
（20 行未匹配 1 行 ✗）⇒ 补进锚定集后 **20/20 轮未匹配行=0** ✓。两次都是"**松规则会静默放行、锚定+覆盖率自检不会**"的实证 ✓。

**反瞎判据（沿用 `NumGC=0` 口径 ✓）**：`jit_stats.py` 在"有采集但真的 0 事件"时输出 `n=0` 并 **rc=0** ✓，
在"缺采集"时报 FAIL ✗；自检 `python3 bench/gc/jit_stats.py selftest` = **4 红 + 1 绿**（绿的那格是
"有头部行、0 个 GC 事件"= 合法的 0）✓。

### 6.10.5 §6.10 结果表（**实测** ✓ 2026-10-14，core 6，两轮 + k=6 重跑）

**跑法（原文）**：`taskset -c 6 env SKIP_BUILD=1 PIN=6 REPS=5 SIZES="250000 1000000" EXTC_BIN=/tmp/gcbench/extc.pin3 bash run-digraph.sh jit`
（`jit` 模式 = 冷/暖采样 + 启动探针 + GC/JIT 诊断轮；两轮 = `round1/round2`；非稳态格用 `WARM_K=6` 重跑）。
每轮都先跑一遍 13 臂对齐：**两轮 × 两规模全部 `✓ 全部实现逐字节一致`** ✓（含 Java/Node）。

**启动探针（空程序，min/中位）** —— 第 2 轮（第 1 轮在括号里）：

| 探针 | 序列（ms） | min | 中位 |
|---|---|---|---|
| Java 空程序 | 19.7 / 19.7 / 19.3 / 21.6 / 22.0（r1: 19.8/20.8/20.1/20.7/18.4） | **19.28**（r1 18.39） | **19.70**（r1 20.06） |
| Node 空脚本 | 38.9 / **105.3** / 38.3 / 37.6 / 37.5（r1: 38.2/39.3/39.3/38.4/37.1） | **37.51**（r1 37.08） | **38.27**（r1 38.41） |

（Node 那次 105 ms 是单次抖动 ⇒ 报中位；min/中位两轮一致 ✓。）

**冷/暖结果表（两轮；`暖` 取 `WARM=3` 的末遍）**

| 臂 | 冷·含启动 250k | 冷·扣启动 250k | 暖·含启动 250k | 暖·扣启动 250k | 冷·含启动 1M | 冷·扣启动 1M | 暖·含启动 1M | 暖·扣启动 1M |
|---|---|---|---|---|---|---|---|---|
| **Java** | **131.0**（r1 140.3） | 111.7 | 323.8 | **40.1**（r1 40.3） | **718.5**（r1 729.4） | 699.2 | 2483.5 | **569.3**（r1 576.7） |
| **Node bigint**（⚠️ 可能低估 V8） | 151.5（149.1） | 114.0 | 383.8 | **69.5**（71.4） | 1141.5（1161.9） | 1104.0 | 4404.3 | **1074.6**（1075.6） |
| **Node split** | 140.4（141.4） | 102.9 | 349.3 | **61.8**（61.9） | 985.9（1010.5） | 948.4 | 3835.4 | **964.2**（978.0） |

**峰值 RSS（内核值，冷/暖 MiB）**：Java 84/117（250k）· 119/119（1M）；Node bigint 85/101 · 140/206；Node split 82/102 · 100/206。
（`暖` 的 RSS 含 4 遍累计 ⇒ 偏高，是"同进程多遍"的必然。）

**预热收益（扣启动，冷 → 暖）**：@250k **Java 2.78×**（111.7→40.1）· bigint 1.64×（114.0→69.5）· split 1.66×（102.9→61.8）；
@1M **Java 1.23×**（699.2→569.3）· bigint 1.03× · split **0.98×（无收益）**。
⇒ **规模越小，预热占比越大**（250k 上冷值里有 2/3 是"没热 + 启动"）✓ —— 这正是"JIT 要预热"的可量形态。

**GC 停顿 vs JIT 编译事件（两列分开 ✓）**

| 臂 | GC 事件（250k） | GC 事件（1M） | JIT 编译事件（250k / 1M） |
|---|---|---|---|
| Java | **n=0（显式 0 ✓）** | **n=0（显式 0 ✓）** | events=**131**（9968 B）/ **118**（7413 B），两轮逐字相同 |
| Node bigint | n=23（total 6.1–9.8 ms；p50 0.26；max 0.46–3.5） | n=52（total 35.7–36.8 ms；p50 0.56；p90 1.15；max 1.5） | compiled=**34**（MAGLEV 24 + TURBOFAN 10）、marking 35、**全部 `ConcurrencyMode::kConcurrent`**、deopt-eager 4 |
| Node split | n=15（total 5.0–8.7 ms；p50 0.33） | n=33（total 16.9–17.4 ms；p90 0.92；max 2.1） | compiled=**33**（MAGLEV 23 + TURBOFAN 10）、marking 34、全部 kConcurrent、deopt-eager 8 |

- **Java 在这形状上默认参数下 0 次 GC**（显式报 0，不是缺采集 ✓）⇒ Java 的冷/暖差**几乎全是 JIT**，
  不是 GC —— 这就是"**JIT 编译 vs GC 停顿必须分开**"的实证 ✓。
- **V8 的编译事件全部是 `kConcurrent`** ⇒ 编译在**后台线程**，主线程的 GC 停顿里看不到 JIT ✓
  （所以 JIT 事件**单列**、绝不并进 GC 列 ✗）。
- GC 停顿量级：@1M Node bigint total **35.7 ms**（p50 0.56 ms）；而 §6.9 里 **Go @1M 总 STW 0.95 ms**、
  **extC 总清扫 63.9 ms** ⇒ 三家的"停顿手感"完全不同的量级 ✓。

**稳态判据结果（§6.10.2 写死的口径，**不裁数据** ✓）**

| 格 | `WARM=3` 稳态 | `WARM=6` 重跑 | 逐遍序列（k=6，ms） |
|---|---|---|---|
| Java @250k | 未达（2/5） | **稳态 5/5** ✓ | 93.9,112.2,43.4,41.2,40.2,39.8,**40.4** |
| Node split @250k | 未达（1/5 → 3/5） | 未达（4/5） | 100.9,70.3,67.1,62.7,63.3,63.1,**66.0** |
| Node bigint @250k | 5/5（r2） | **未达（0/5）** ✗ | 110.7,84.4,74.4,70.6,70.0,71.2,**83.9** ← 末遍反而变慢 |
| Java / bigint / split @1M | 稳态 5/5 | — | — |

**Node bigint @250k 为什么加大 k 也不稳态**（照实写，机制有证据 ✓）：热到 ~70 ms 后**末遍跳回 ~80–84 ms**
（5 个样本全部如此）；同一次 `k=6` 的暖 RSS 从 k=3 的 **101 MiB 涨到 137 MiB** ⇒ 是**晚到的 GC**
落在末遍（每个热身遍都留 ~85 MiB 垃圾，进程内累计）。⇒ 这一格的"暖值"应读作
**"预热后 + 末遍碰上一次 GC"**，而不是稳态纯做活时间。（这也是为什么表里同时给 `iters_ms` 全序列 ✓。）

**公平性说明** ✓：单线程做活；**一律默认参数、不做调优** ✗（`-Xmx`/`--max-old-space-size` 另立任务）；
Java 的 GC/JIT 走 `:stderr`、Node 的 trace 走 stdout（**诊断轮**由卡具按锚定前缀剥行 + 与普通轮 sha 比对 ✓）；
运行时后台线程（V8 编译线程、JVM JIT/GC 线程）如实注明 ✓；Node 的 64 位策略两条路都报 ✓。

**Node trace 的落点**：V8 的 `--trace-gc/--trace-opt` 只能走 **stdout** ✗（文件日志在本 build 不可用，
见 §6.10.4）⇒ 上表的 Node 行是**诊断轮**：卡具先把 trace 行剥掉、只留程序自己的两行做比对（**恰好 2 行**
且与普通轮 stdout **逐字节相同** ✓），trace 行交给 `jit_stats.py`；**默认路径不带这些开关 ⇒ 13 臂 stdout 一字不变** ✓。

---

## 6.11 同压力下的 JIT 家族停顿对照（`task-45`，2026-10-14）

> **口径警告（写在表头 ✓）**：Java（G1）/ Node（V8）/ Go 的“停顿”是**并发/后台收集器**语义下的暂停，
> extC 的 `collect` 是**串行 stop-the-world** ⇒ **两者不可严格混比** ✗（这条与 `GO-PAUSE.md` 同一姿势 ✓）。
> 本节只回答“**在同形状/同规模/同 seed 下、让它们也被迫真回收时，三把尺分别谁赢**” ✓。
> 全部格：`taskset -c 6`（⇒ JVM/V8 也只看到 **1 CPU**，与 `GO-PAUSE.md` 的 `1 P` 发现同源 ✓）、
> `digraph @N=1000000 seed=20261013`，**产物逐字节一致** ✓（extC 的**探针副本**另带 1 行 `PROBE …` ⇒ 比对时按 §6.9 口径剥掉：
> 剥后 **5 条 extC × Java 两档 × Node 两档的 sha 全为 `087d1828327cb37c`** ✓；Node 诊断轮的 `--trace-gc` 剥行自检也过了
> ——**未匹配 0、恰好 2 行、与普通轮逐字节相同** ✓）。

### 6.11.1 “被迫真回收”的压法（**选法写清** ✓，不是调优 ✗）

| 运行时 | 压之前 | 候选扫描（各 1 次） | 选定 |
|---|---|---|---|
| Java | 默认 `-Xmx`（≈内存 1/4）**本来就有 2 次 Pause** ✓ | `-Xmx128m`=4 次 / `96m`=5 / **`64m`=8** / `48m`=10 / `32m`=16（**无一 OOM** ✓，RSS 90→67 MB） | **`-Xmx64m`**（8 次/跑 ⇒ 5 次跑 ≥40 样本 ✓；`32m` 会把压力推得比 extC 更极端 ✗） |
| Node | 默认 **32 个 GC 周期** ✓ | `--max-old-space-size=48` = **32（没变 ✗）**；**`--max-semi-space-size=4` = 77** ✗→✓ | **`--max-semi-space-size=4`**（年轻代压小才真加压；只压老年代不动 GC 次数 ✓ 这是个真发现 ✗） |

- Java 用 `-Xlog:gc:stderr`（**默认落 stdout ✗ ⇒ 必须 `:stderr`**，与 §6.10.4 同坑 ✓），暂停 = `GC(n) Pause …` 行 ✓。
- Node 用**诊断轮 + 锚定剥行**（`--trace-gc` 只能走 stdout ✗）：剥完**恰好 2 行**、与普通轮 stdout **逐字节相同**、
  **未匹配行 = 0** ✓（三个自检全过 ✓）。
- extC 用 `rc-pause-probe` 的 `PAUSE_DIST`（当前绿点 md5 **`e4c3a869…`** ✓）；**Go 两行直接引用 `GO-PAUSE.md`，不重跑** ✓。

### 6.11.2 三把尺（@1M；样本数=池化；`Σ/跑`=单次运行的总暂停）

| 臂 | 收集器口径 | 样本数 | **p50** | p90 | p99 | **max** | **Σ/跑** | 墙钟(含启动) ms | 启动 min ms | 扣启动 ms | **峰值 RSS** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **extC 默认**（`e4c3a869`） | **串行 STW** | 780（5×156） | **172.2 µs** | 736.7 | 1130.1 | **1469.1 µs** | **53.40 ms** ✗ | **483.0** ✓ | ~0（静态二进制） | ≈483 | **8.9 MiB** ✓ |
| Java 默认 | 并发/后台 | 16（8×2） | 1426 µs | 1750 | 1774 | 2087 | 2.77 ms | 700.5 | 18.0 | 682.5 | 118.2 MiB |
| Java `-Xmx64m` | 并发/后台 | 40（5×8） | 879 µs | 1076 | 1168 | **1360 µs** | 6.36 ms | 685.3 | 18.0 | 667.3 | 70.8 MiB |
| Node 默认（split） | 并发/后台 | 31（5×6.2） | **520 µs** | 1410 | 2170 | 2560 µs | 4.54 ms | 986.8 | 37.8 | 949.0 | 99.6 MiB |
| Node `--max-semi-space-size=4` | 并发/后台 | 77（5×15.4） | 860 µs | 1420 | 1660 | 2600 µs | 12.80 ms | 1006.9 | 37.5 | 969.4 | 93.3 MiB |
| **Go 默认**（`GO-PAUSE.md` g0） | 并发/增量 | 53/跑 | **6 µs** | — | 13 | **17 µs** | **0.346 ms** | 503.6 | 未测 | — | 8.6 MiB（§6.2） |
| Go `GOGC=1`（`GO-PAUSE.md` g2，**@250k** ✗ 不同规模 ⇒ 只作参考） | 并发/增量 | 33/跑 | 4 µs | — | — | 35 µs | 0.172 ms | 55.6 | — | — | — |

**原始行**（原文 ✓）：
```
PAUSE_DIST n=156 … p50_us=172.2 p90_us=736.7 p99_us=1012.7 max_us=1469.1 total_us=53324.1   # extC，5 次池化后重算
java-default   n=16 p50=1.426 p90=1.750 p99=1.774 max=2.087 total=22.158 wall=700.5 rss=121040 {'Young': 16}
java-xmx64m    n=40 p50=0.879 p90=1.076 p99=1.168 max=1.360 total=31.798 wall=685.3 rss=72508  {'Young': 40}
node-default   n=31 p50=0.520 p90=1.410 p99=2.170 max=2.560 total=22.680 wall=986.8 rss=101940 {'Scavenge': 31}
node-semi4     n=77 p50=0.860 p90=1.420 p99=1.660 max=2.600 total=64.010 wall=1006.9 rss=95500 {'Scavenge': 76, 'Mark-Compact': 1}
```
（原始文件：`/tmp/gjit/`；extC 的 5 条 `PAUSE_DIST` 在 `/tmp/gjit/extc/e{1..5}.txt` ✓。Java 的 `total` 是**样本池化和** ⇒
`Σ/跑 = total ÷ reps`（16/8=2、40/5=8、31/5=6.2、77/5=15.4 次/跑 ✓）。

### 6.11.3 一句话结论（谁赢哪把尺）

- **单次停顿：extC 赢 JIT 家族** ✓ —— p50 **172 µs** vs Java/Node **520–1426 µs**；max **1469 µs** vs **1360–2600 µs**
  （与**压限后的 Java 基本打平**，比 Node 好 ~1.8×）✓。⇒ **“我们单次停顿比 JIT 后的 Java/Node 还差”这个说法不成立** ✓。
- **总停顿量：extC 输** ✗ —— **53.40 ms/跑** vs Java 2.77–6.36 ms、Node 4.54–12.80 ms（**差 4–19×**），
  根因是**次数**：extC **156 次**/跑 vs Java 2–8、Node 6.2–15.4 ⇒ **问题的形状是“次数多”，不是“单次长”** ✓✓。
- **墙钟：extC 赢** ✓ —— **483 ms** vs Java 685–700、Node 987–1007（Go 503.6 紧随）。
- **峰值 RSS：extC 赢约 8–13×** ✓ —— **8.9 MiB** vs Java 70.8–118.2、Node 93.3–99.6 MiB（Go 8.6 MiB 同级 ✓）。
- **Go 仍压倒性**（Σ 0.346 ms、max 17 µs）—— 但那是**并发/增量**口径，**与 extC 的串行 STW 不是同一件事** ✗（见 `GO-PAUSE.md`）。

### 6.11.4 顺带：它们的回收运行期**有没有写屏障税**（可选，**文献级** ✗ 未在本机源码核实）

| 运行时 | 写屏障 | 来源 |
|---|---|---|
| Java（G1） | **有**：标记用 **SATB（snapshot-at-the-beginning）pre-write barrier** + 卡表 | Oracle《HotSpot GC Tuning Guide》G1 章【文献】 |
| Node（V8） | **有**：写屏障用于记录 old-to-new（**generational 屏障常开** ✓）并在增量/并发标记期间阻止黑→白、记录 old-to-old | V8 官方 `src/heap/WRITE_BARRIER.md`【文献：[链接](https://chromium.googlesource.com/v8/v8.git/+/refs/heads/main/src/heap/WRITE_BARRIER.md)】 |
| extC 默认 | **无**（默认档不发 rc、也没有屏障路径；它的成本在**引用计数/清扫**那一侧 ✗） | 本仓库 `gate-rc-idle-corpus` + §6.2 |

⇒ **JIT 家族的“短暂停”是用写屏障税换来的** ✓（与 `GO-PAUSE.md` §4 给 extC 的结论同一条：想换增量就得付屏障 ✗）。

### 6.11.5 未覆盖与风险

- **口径不同**（并发/后台 vs 串行 STW）⇒ 只比“三把尺”，**不比机制** ✗；Java/Node 的暂停里**不含**并发阶段的工作 ✓。
- **单形状（`digraph`）、单规模（@1M）、单核（core 6）**；Java/Node 的启动已扣（`扣启动` 列），extC/Go 是静态/低启动 ✓（Go 启动未测）。
- Java 默认档只触发 **2 次/跑**（16 样本）⇒ max 的样本量小 ✓ 已写明；`-Xmx`/`--max-semi-space-size` 是**造压力**、不是调优 ✗。
- Node 那条 `--max-old-space-size` **加不上压力**（GC 次数不变 ✓）—— 若要压老年代压力，得另找手段 ✗（未做）。
- **不承诺“没有任何新风险”** ✗。

---

## 7. 四个问题的回答（**实测**，数字全部来自 §6.2）

### 7.1 谁在什么规模上开始"收不掉"

**从第一个规模起就收不掉** —— 只要环状垃圾占 98%+，全 `shared_ptr`/`Rc` 的引用计数**一个都收不了**：

| | N=250k RSS | N=1M RSS | 4N/N |
|---|---|---|---|
| C++ `naive`（全 `shared_ptr`） | 70.6 MiB | **269.9 MiB** | **3.82** |
| Rust `naive`（全 `Rc`） | 53.5 MiB | **207.1 MiB** | **3.87** |
| 对照：Go 默认 | 7.3 | 8.6 | 1.18 |
| 对照：Rust `careful` | 3.8 | 6.2 | 1.63 |
| 对照：extC 默认 | 7.9 | 11.6 | 1.47 |

`dropped_nodes/nodes_made` = 98.7%（250k）/ 98.6%（1M），且 `cycles_dropped == dropped_groups`
（每个被摘组现场验环）⇒ 这些正是**不可达的环状垃圾**。斜率 ≈ **4 = 与 N 成正比** ✓。
"不回收"的签名在另两档也复现：Go `GOGC=off` 37.0→142.0（3.84×）、extC `RECLAIM=0` 34.3→132.9（3.87×）。

### 7.2 careful 档的代价（单列）

**反直觉：在这份负载上 careful 比 naive 又快又省**（不是"稳但慢"）：

| 差值 | 时间 | 峰值 RSS |
|---|---|---|
| C++ `careful − naive` | −15.2 ms（−14%）@250k · **−128.9 ms（−10%）**@1M | −64.7 MiB（−92%）· **−260.7 MiB（−97%）** |
| Rust `careful − naive` | −10.1 ms（−12%）· **−53.9 ms（−5%）** | −49.7 MiB（−93%）· **−200.9 MiB（−97%）** |

**机制（有直接证据）**：naive 的 270 MiB 常驻让分配器/页表/缓存付出代价；careful 只多花
**4.4% 的运行时间**在"断开那一趟"（`DIGRAPH_BREAK_STATS=1`：C++ 195/241 ns 每次，
Rust 172/205 ns 每次；总数 = 摘掉的组数 54859/219025）就把 98.6% 的对象真正释放 ✓。
⇒ 这门语言里"对付环"的**运行期**工程费**很便宜**（组只有 3-6 个结点，断开是 O(组大小)）。
**未量化的一半**：类型/API 的工程负担（弱引用、逐类引用显式断开、漏一处就泄漏或悬垂）
不体现在时间/RSS 上 —— 归"未归因"。

### 7.3 extC 的位置

- **相对 Go 默认（追踪 GC）**：时间 **1.05×**（538.8 vs 514.5 ms @1M；250k 1.06×），
  RSS **1.3×**（11.6 vs 8.6 MiB @1M）⇒ **同一量级，Go 略快略省**。
  这个差应归"语言/运行期"（Go 的 mcache 分配快路径 + 存活集更紧凑），**不是回收策略差异** ✓
  —— 两者都是有界（4N/N = 1.47 vs 1.18）。
- **相对 naive 引用计数**：extC 默认 @1M 比 C++ `naive` **快 2.5×**（538.8 vs 1338.0 ms）、
  峰值 RSS **少 23×**（11.6 vs 269.9 MiB）；比 Rust `naive` 快 **2.0×**、少 **17.9×**。
- **extC 的"地板"档反而更差**：`RECLAIM=0` 612.8 ms / 132.9 MiB ⇒ 相对默认**慢 74 ms、多 121 MiB**
  ⇒ 这个形状上**清扫是赚的**（不回收就要不停 mmap/缺页）✓ —— 与"回收=纯税"的直觉相反。
- **计数税**（目标线③的直接证据）：`NOCOUNT=0` **+597.4 ms（+111%）**，而内存**几乎不动**
  （12.0 vs 11.6 MiB）⇒ 这份图负载上**计数没有可换的释放**（垃圾是环，持有者数永不归零）。
  ⇒ 默认关计数是对的；也说明"计数只在能换来释放时才开"这条在**图/环负载**上尤其成立 ✓
  （与 lead 在 `bt` 上量到的 +9.4% Ir 量级不同 —— 本形状赋值密度高得多，**这是信息不是异常** ✓）。
- **`SHARDS=4` 在这形状上没收住** ✗（见 7.4）：**lead 已立 `task-13`**（判据：本形状 `SHARDS=4`
  的 `4N/N < 1.5×`，今天 **3.84×** ⇒ 必先红；且不许丢掉 `shard_bands` 的分片停顿收益）——
  **不在本任务动手** ✓。

### 7.4 诚实归因（哪些是回收、哪些是语言/库/stdlib、哪些归不了）

**归因到"回收"** ✓
- naive RC 的 200–260 MiB ⇐ 环收不掉（98.6% 的结点是环状垃圾；`cycles_dropped == dropped_groups`）；
- Go `off` / extC `RECLAIM=0` 的 132–142 MiB ⇐ 完全不回收；
- careful / Go 默认 / extC 默认的低驻留 ⇐ 回收真的发生（extC 默认 @1M：`freeSlot 3166259 ≈ alloc 3223802`，
  `collect=146` 次清扫；`rs_inc=rs_dec=rs_live=0` ⇒ 单区域、无跨区登记）；
- extC 计数开的 +111% 时间 ⇐ **纯计数税**（`storeOwned` 219M 次都走计数路径），内存不降 ⇐ 环不归零。

**归因到"语言/库/stdlib"** ✓
- C++ `naive` 269.9 vs Rust `naive` 207.1 MiB（**1.30×**）⇐ 每结点足迹
  （`shared_ptr` 控制块 + 原子计数 vs `Rc` 内联头）；
- Go 最快（514.5 ms）⇐ mcache 尺寸类分配快路径；extC 与它同量级但要
  `extc_rc_alloc` + 219M 次 `storeOwned` 记账；
- Rust `careful` 比 C++ `careful` 快 16%（1016.5 vs 1209.2 ms）⇐ `RefCell` 无原子 vs
  `shared_ptr` 原子计数（+ `Vec` vs `std::vector`）。

**未归因** ✗（观察到现象/机制，但没落到源码级结论）
- **`EXTC_RC_SHARDS=4` 为什么没收住**：证据齐（RSS 3.84× 线性；`freeSlot` 只有默认的 5%、
  `collect` 只有 12%、`regionNew 1→4`、`rs_live=3470012`），但"分片下清扫为什么收不到"
  没有追到源码级结论 ⇒ **未归因**（本任务不许改编译器）。
  **归口**：lead 已立 **`task-13`**（"把同一 SCC 的站点钉到同一片"；判据 = 本形状 `SHARDS=4`
  的 `4N/N < 1.5×`，今天 3.84× 先红；不许丢 `shard_bands` 的停顿收益；`SHARDS=1` 逐字节不变）
  ⇒ **现象 + 机制留在本报告 ✓，源码级结论交 `task-13` ✓**。
- **全部档位 4N/N 时间比 7.4–13.6（都超线性）**：形状本身在 N 上超线性
  （每轮丢组要 `handleAt` 走 O(存活 handle 数)，而 R 与存活组数都随 N 变），四家一致 ⇒
  不区分实现；但"形状超线性"贡献多少没有单独量化 ⇒ **未归因**。
- careful 档的**工程复杂度**代价（类型/API 负担）⇒ 未量化 ⇒ 未归因。

---

## 8. 未覆盖项与残留风险（诚实清单）

**电池（本任务改动后的实测）**
- **task-21 期间**（当时 `slab-region` 的 `reclaim.c` 在飞）：`docs` = **12/0** ✓；
  `smoke` = **15/1**、`nobench` = **98/1** —— **同一个失败**：
  `gate-note-part` 报 `FAIL [R2] extc_rc_ownSlab：会交出 rc 槽却没记 partN`。
- **归因（不是本任务引入）** ✓：该 checker 读 `src/back/reclaim.c` + `tests/reclaim/emit_main.c`（`tools/check_note_part.py:115`）；
  而 `src/back/reclaim.c` 现有 **764 行未提交改动**（`git diff --stat`；同一 owner 还在改 `codegen.c` 645 行），
  失败点正是那些改动里的 `ownSlab`/`ownMap` 快路径 ✗。本任务只改 `bench/gc/**` 与 `tools/rc-pause-probe.py`，
  且探针只注入**生成物的 /tmp 副本**，不碰 `src/` ⇒ **与本任务无关**（该红点应由 `slab-region` 收口）。
- **task-23 期间复跑（收口后）**：`docs` **12/0** ✓、`smoke` **16/0** ✓、`nobench` **99/0** ✓（全绿，与 §6.9 时的 sliver-region 在飞状态对比）；
  `nobench` 里唯一的 `FAIL` 字样是**故意注入的红证明**（"把事件档强制施加到有环形状 ⇒ 成功判据必须 FAIL"，
  见 `tests/reclaim-*` 的 ③ 红证明），不是真失败 ✓。

**未覆盖（明确没测）**
- Java 未纳入本形状；可选的手写 C `malloc/free` 对照未做。
- `EXTC_RC_SHARDS` 只测了 4（未测 2/8）；`EXTC_RC_NOCOUNT=0` 已测（§6.2 第 3 档）✓。
- 多线程 / 协程 / TLS 形状未测；Go 用默认 `GOMAXPROCS`（GC 后台标记线程存在，
  "单线程"只对**用户代码**成立）。
- **`EXTC_RUN_DIR` 的"并行私有目录"行为未验证** ✗（不属于本任务；且验证要跑 `--run`）。
  本任务只验证了它**不改生成物**（§3）✓。
- 被钉住组的**总数**没有单独输出（只有 `rejected_external` 探测计数）。
- 时间只测了一个 seed（20261013）；RSS 与输出不随 seed 变（对齐矩阵跨 5 个 seed ✓），
  但时间对 seed 的敏感性未测。

**实测中发现、但**未归因**的现象（已写进 §7.4）**
- **`EXTC_RC_SHARDS=4` 在这形状上没收住**：RSS 60.4→231.8 MiB（3.84×，与 naive RC 同量级）；
  机制证据：`freeSlot` 3.17M→0.17M、`collect` 146→18、`regionNew` 1→4、
  `rs_inc=5674152 rs_dec=2204140 rs_live=3470012 releases=0`。
  ⇒ 现象与机制清楚，**源码级结论未做**（本任务不许改编译器）⇒ **lead 已立 `task-13` 归口** ✓
  （判据：本形状 `SHARDS=4` 的 `4N/N < 1.5×`，今天 3.84× 必先红；不许丢 `shard_bands` 停顿收益）。
- 四家 `4N/N` **时间**比 7.4–13.6（都超线性，形状本身在 N 上超线性）⇒ 不区分实现，但未量化归因。

**残留风险**
- extC 编译器在阶段 1→2 之间被上游换了**两次**：`3cf8ae96…` → `ba9d5def…` → **`69a5b119…`**
  （阶段 2 用 `/tmp/gcbench/extc.pin3`）。**两代新编译器下本形状生成物/二进制均逐字节不变**（§3）✓；
  若**再变** ⇒ 重新 pin + 重跑全部 extC 行。
- 峰值 RSS 有量具地板（Go 运行期/libc/页表）：小 N（≤ 250k）时地板主导 ⇒ 结论都建立在
  **N 与 4N 的斜率**上，而不是绝对值。
- 校验和是抽查不是证明（§2.3）；对齐靠 10 条不变量联合判定。
- 绑核 ≠ 隔离：**实测到一次真污染** —— 第 3 轮跑在 `region-proof` 的语料级普查期间
  （`census.py --jobs 12`，load 7.43），同一份 extC 二进制 +39% ⇒ **该轮整轮作废**（§6.4）✗。
  正文只采用干净窗口的第 1、2 轮（互差 ≤3.0%）✓。⇒ **下一次机器不空时不要跑计时**。
- **交付源码 ↔ 测量二进制的严格同一性**：测量后源码里加了**默认不编译**的门控断言（§6.8）。
  C++ 四个二进制与测量时**逐字节相同** ✓；Rust 两个二进制 md5 变了（rustc 把整份源码算进 crate 哈希），
  但已用**助记符序列逐条相同（75023 条）**证明执行代码不变 ✓，且输出逐字节一致 ✓。
  **lead 裁决：不做严格同源重测** ✓（md5 差来自 rustc 的 crate 哈希；助记符序列逐条相同是更强的证据；
  干净窗口留语料普查）⇒ 残留照实保留。
- 不承诺"没有任何新风险"。
