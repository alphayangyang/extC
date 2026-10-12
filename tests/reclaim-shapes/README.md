# `tests/reclaim-shapes/` —— 区域回收的**行为基线**

配套机制文档：[`docs/RECLAMATION.md`](../../docs/RECLAMATION.md)。
形状来源：该文档「要解决的问题」那一节 + [`prototype-reclaim/README.md`](../../prototype-reclaim/README.md)「压力版」。

## 这个目录的语义

**只钉判据，不做门禁。** 它把「区域回收」要覆盖的现实形状写成小的 `.extc` 程序，
逐条记录**当前的实际行为**：哪些判据已经成立（绿），哪些还没有（红）。

* `./run.sh` **永远返回 0**，红条目在表里标出来（同 `tests/arena-soundness/` 的口径：
  这是证据，不是回归判据）⇒ **不接进 `check.sh` 的门禁计数**。
* 每个形状 = 一次 `extc` + 一次 `gcc` + 一次运行，整体 < 1s；峰值内存取
  `/usr/bin/time -v` 的 `Maximum resident set size`。
* **开关下的判据单独一组**：`./run-switch.sh`（`EXTC_RECLAIM=1`）是**门禁** —— 只在开关
  打开时成立的断言放那里；关掉开关（`EXTC_RECLAIM=0 ./run-switch.sh`）它必须红。
  两个脚本的分工见下面「开关」一节。

## 现在的表（`./run.sh` 输出）

| 形状 | 编译 | 运行 | 峰值内存 | 备注 |
|---|---|---|---|---|
| `cond-save-in-loop` | ok | ok | 1.42 MB | **绿** 语义对（keep 最后一轮 ⇒ saved=7）|
| `reject_deep_store` | rej | — | — | **绿** 编译期挡住：`dies first (borrowed from depth 2, ... up to depth 1)` |
| `reject_deep_store_call` | rej | — | — | **绿** 编译期挡住：`points into a deeper scope (depth 2) > arena (depth 1)` |
| `cyclic-shape` | ok | ok | 1.42 MB | **绿** 有环：next/prev 与 child/parent 互指；fwd=back、tree、cache 全对 |
| `promote-place` | ok | ok | 1.42 MB | **绿** 生成物：`kept→__extc_a[1]`（循环外），`doomed→__extc_a[2]`（循环体，每轮 release）|
| `scale-growth N→4N` | ok | ok | ≈4.6 MB → ≈13.8 MB | **红** 峰值随轮数增长（≈3× @ ×4 轮）；开关**关**时回收未生效 ⇒ 界与轮数有关 |
| `cyclic-shape-pressure N→4N` | ok | ok | ≈10.0 MB → ≈34.7 MB | **红** 有环：桶满整条链退役 ⇒ 持有者计数永不归零；开关**关**时峰值随轮数涨（≈3.45×）。开关开的那条是门禁（`run-switch.sh`，≈1.0×）|

数字是量级参考（机器噪声 ±10~20%）。

## 开关（`EXTC_RECLAIM=1`）：哪些条目与开关无关，哪些只在开关下成立

`EXTC_RECLAIM=1` 只换**运行期**（codegen 发射 `src/back/reclaim.c` 的那段文本：位图 slab 分配、
指针赋值计数、`owns` 守卫、区域清扫），不改语义。所以：

| 条目 | 开关关 | 开关开 | 判据归属 |
|---|---|---|---|
| `cond_save`（`saved=7`）| 绿 | 绿 | **与开关无关**：语义钉在两处都成立 |
| `cyclic_shape`（fwd=back / tree / cache）| 绿 | 绿 | **与开关无关**：有环语义一样对 |
| `reject_deep_store*`（编译期拒）| 绿 | 绿 | **与开关无关**：这是前端判据 |
| `promote_place`（生成物归属）| 绿 | 绿 | **与开关无关**：提权归属由 codegen 的 arena 站点定 |
| 曾经的 SEGV 用例（见下）| — | **必须绿** | **只在开关下成立**：开关才引入 `owns` 守卫 |
| `scale_growth` N→4N（峰值比）| **红**（≈3.0×）| **绿**（≈1.0×）| **只在开关下成立**：界与轮数解耦 |
| `cyclic_pressure` N→4N（峰值比）| **红**（≈3.45×）| **绿**（≈1.0×）| **只在开关下成立**：**环**垃圾计数永不归零，只有清扫能收（见下「清扫真正上场的证据」）|

曾经的 SEGV 用例 = `tests/heap/loan`、`tests/heap/plate`、`tests/cabi/heapmain`、
`tests/coro/iterator_protocol`：板/池/栈/C 返回的指针不是 rc 槽，开关把计数挂在指针赋值上后这些
形状都会经过 `inc`/`dec`/`register`；`owns` 守卫必须一个字节都不碰（否则 `slabOf` 的按位与会
找到一段无关内存，板的 4 GiB 保留区甚至没 commit ⇒ SEGV）。

### `./run-switch.sh`（门禁，`EXTC_RECLAIM=1`）

判据：`scale_growth` N vs 4N 峰值比 **< 1.5**；`cyclic_pressure` N vs 4N 峰值比 **< 1.5**
且默认/开关下输出逐字一致；`cond_save` / `cyclic_shape` 语义不变；上表四个曾经的 SEGV
用例 **rc=0** 且输出逐字不变。实测（`通过 9，失败 0`）：

| 场景 | N=200000 | N=800000 | 比值 | 判定 |
|---|---|---|---|---|
| `scale_growth` 开关关 | 4.61 MB | 13.79 MB | **2.99×** | 红（`run-switch.sh` exit 1）|
| `scale_growth` 开关开 | 4.79 MB | 4.79 MB | **1.00×** | 绿（`run-switch.sh` exit 0）|
| `cyclic_pressure` 开关关 | 10.04 MB | 34.61 MB | **3.45×** | 红 |
| `cyclic_pressure` 开关开 | 7.61 MB | 7.61 MB | **1.00×** | 绿 |

这就是「判据有牙」的证据：把开关关掉（`EXTC_RECLAIM=0 tests/reclaim-shapes/run-switch.sh`），
两条峰值判据都必须失败（实测 exit 1，`通过 7，失败 2`）。


## 逐条

| 文件 | 覆盖 | 判据 |
|---|---|---|
| `cond_save.extc` | ① 条件保存在循环里（motivating shape）| 编译 + 语义（`saved=7`）|
| `reject_deep_store.extc` | ② 跨帧存储，直接形式 | 必须报 `dies first (borrowed from depth 2 ... depth 1)` |
| `reject_deep_store_call.extc` | ② 跨帧存储，经被调者发布（定案 67）| 必须报 `points into a deeper scope (depth 2) ... (depth 1)` |
| `cyclic_shape.extc` | ③ 双向链表 + 父指针树 + 缓存 + 索引 | 编译 + 运行 + 语义（两个方向的和相等、父指针指回根、缓存/索引命中）|
| `promote_place.extc` | ④ 提权发生的位置 | 生成物结构：逃逸的那份在循环体区域**之外** |
| `scale_growth.extc` | ⑤ 同一形状轮数 ×4 | 峰值 RSS 是否随轮数增长（阈值 1.5×）|
| `cyclic_pressure.extc` | ⑥ 有环 + 大压力（64 桶双向链 + 1024 槽缓存 + 2 万项索引 + 父指针树），轮数 ×4 | 峰值 RSS 是否随轮数增长；默认/开关语义一致；**清扫上场**（`run-evidence.sh`）|

### 跨帧存储的方向（②）

被拒的是「**更深的局部 → 更浅的容器**」：块里的局部对象（深度 2）把 `ref` 存进外层容器
（深度 1）⇒ 引用会活得比它指的东西长。反方向（外层局部存进更深的块容器）编译器**放行**，
那条不会悬垂 —— 判据钉的是会悬垂的那一侧。

直接形式与经被调者形式是同一个洞的两个出口：`stash` 只**发布**约束，调用点拿具体深度判。

### 提权看得见什么、看不见什么（④）

生成物里看得见的是**归属（placement）**：会逃逸的那一份 `new` 从一开始就分配进外层区域
（`extc_arena_alloc(&__extc_a[1], ...)` 出现在循环体内），不逃逸的那一份留在循环体区域
（`__extc_a[2]`，循环每轮 `release`）。`run.sh` 断言两个 arena 下标不同、且循环体区域的
`release` 在缩进 8 空格那一层出现而外层区域的没有。

**看不见**的是运行期的搬移：机制说的「先落当前区、真实逃逸时把那一份转移过去、地址不变」
在今日 codegen 里**没有对应的运行期动作**（没有 promote/transfer 调用）。所以这里断言的是
「存储确实归到了外层区域」，**不是**「发生了一次搬移」—— 不硬造。

### 有环形状（③）

`next/prev` 与 `parent/left/right` 天然互指 ⇒ 每对相邻节点就是一个环 ⇒ 引用计数永远不归零，
只能靠区域清扫（机制文档「四层」里的第四层；`EXTC_RECLAIM=1` 发射的运行期实现了它 ——
**真正上场的证据**见下面 ⑥）。缓存与索引是**跨结构引用**，对应 prototype-reclaim 里
「记忆集/跨区登记」要处理的那类边。

规模取 8 个节点（秒级）；峰值只有进程底噪量级，所以这里**不设上界** —— 规模行为由 ⑤/⑥ 管。

### 红的那条（⑤）

`scale_growth.extc` 与 `cond_save.extc` 是同一个形状（条件保存），只是轮数由 argv 给：
`run.sh` 跑 `200000` 与 `800000` 两次。每一轮 `new` 出来的存储都可能进 `saved` ⇒ 被提到函数层
区域，函数返回前整片不回收 ⇒ 峰值 RSS 随轮数线性增长。

这正是 `RECLAMATION.md` 要解决的问题：「不可达但仍占存储的对象，其总量有界，且这个界与循环
轮数无关」。**开关关**时界与轮数有关 ⇒ 红；**开关打开**（`EXTC_RECLAIM=1`）后计数归零当场回收
+ 区域清扫兜环 ⇒ 峰值与轮数无关（实测 2.99× → 1.00×），这一条在
[`run-switch.sh`](run-switch.sh) 里被固化成门禁断言。

## 有环 + 大压力（⑥）：清扫真正上场的证据

⑤ 是**无环**的：每一轮的 `p` 只有一个持有者（`saved`），覆盖就归零 ⇒ **计数归零一层就够，
清扫从头到尾没参与**。⑥ 补的正是缺的那半：垃圾是**环**，计数**永远不归零**。

### 形状：为什么必须靠清扫

`cyclic_pressure.extc` 复刻 [`prototype-reclaim/stress.c`](../../prototype-reclaim/stress.c)
那个业务形状：**64 个桶（每桶 ≤8 节点）的双向链表 + 1024 槽缓存（第二持有者）+ 2 万项索引
（跨结构引用）+ 带父指针的树（每 16 轮挂一个）**。

环垃圾的来源是**桶满时整条链退役**：

```extc
pushFront(ref w, b, p)                       // p.next = 旧头、旧头.prev = p
if i % 64 == 0 { cachePut(ref w, c, b) }
if w.bkSize[b] > 8 { retireChain(ref w, b) } // 链头挂到 w.retire，桶清空；链内 next/prev 不动
```

退役链的**每个节点都还有入边**（`a.next=b` 与 `b.prev=a` 互指），计数永远 ≥1 ⇒
`extc_rc_holdUpd` 一次 `dec` 都到不了 0，**一个槽也放不掉**；同时它已经不在任何根之下
（唯一的外部引用 `w.retire` 被下一次退役覆盖）⇒ 是**不可达的环**。四层里能收它的只有
**区域清扫**。这跟 `stress.c` 里「引用计数在环面前几乎归零」是同一件事。

树的根槽是**定长环** `tree[256]`，每 16 轮占一格、每 256 个节点另起一代。理由：extC 的标记
按对象的**全部**指针字段走（`next/prev/child/parent`），如果让每个树节点都从上一个取 parent、
根槽又无界增长，那么只要最后一个节点还活着，就能沿 `parent` 走回全部历史节点 ⇒ 整条祖先链
永远是活的、内存照样涨。定长环 + 换代把祖先深度钉在 256 以内；换掉的那一代 child/parent
互指仍在 ⇒ 又是一批不可达的环垃圾。

### 语言约束：原型能随便写、extC 必须绕的地方

| 写法 | 结果 | 原因 |
|---|---|---|
| `(*head).prev = p`（`head` 是本地 `?mut ref`，`p` 是循环体局部）| **拒** | 解引用**不产生存储**：目的地的槽深度按保守读法取 0（`slotDepth(EX_DEREF) = targetDepth(head)`），而本地的被指物寿命未知 ⇒ 循环体局部（深度 2）存不进去 |
| `a.next = p`（`a` 是本帧的 `var a = new node`）| 过 | 目的地是本帧已知的存储 |
| `link(a, p)`（`fn link(n: mut ref node, p: mut ref node)`）| 过 | 判定推迟到调用点，目的地是形参 |
| 同一个循环体局部 `p` 连续传给**两个**「会存进外层」的 helper | 第二处**拒** | 报 `argument 3 ... points into a deeper scope (depth 2) ... (depth 1)` |
| `ref w.bkHead[b]`（数组元素取引用）| 拒 | 只有变量与字段能取引用 ⇒ 传 `ref w`，在 helper 里下标 |

最终写法：**每个新对象只经由一个 helper 的一次调用完成逃逸**（`p`→`pushFront`、
`e`→`idxPut`、`t`→`treePut`），需要「第二持有者」时让 helper 从**帧内字段**
（`w.bkHead[b]`）里取那份引用，而不是把循环体局部再传一次。这条正是原型与 extC 的差异：
原型里 `init_ref(&g_head[b]->prev, nd)` 随手就写，extC 里要给目的地一个**本帧已知的深度**。

### 数字 1：N vs 4N 峰值（门禁）

`run-switch.sh` 的 ①b；输出逐字一致（`rounds/retired/live/fwd/back/idx/cache/hits/depth/tree`）。

| 场景 | N=200000 | N=800000（4N）| 比值 |
|---|---|---|---|
| `EXTC_RECLAIM` 关 | 10.04 MB | 34.61 MB | **3.45×**（红，> 2）|
| `EXTC_RECLAIM=1` | 7.61 MB | 7.61 MB | **1.00×**（绿，< 1.5）|

默认与开关下输出**逐字一致** ⇒ 回收只换运行期、不改语义。

### 数字 2：清扫真的上场了吗（`run-evidence.sh`）

「峰值被压住」有两个可能：计数归零收掉了，或者清扫收掉了。**同一份生成物**只动清扫的
触发阈值（生成物里的运行期是 `#ifndef EXTC_RC_FLOOR` 守卫的宏），因果就闭合了：

| 变体 | N=200000 | N=800000 | 比值 | 读法 |
|---|---|---|---|---|
| 开关关（无回收运行期）| 10.04 MB | 34.61 MB | 3.45× | 基线 |
| 开关开 + **清扫永不触发**（`-DEXTC_RC_FLOOR=1 TiB`）| 17.92 MB | 63.54 MB | **3.55×** | 环垃圾计数永不归零 ⇒ 计数那层一个也收不掉；峰值**重新随轮数涨** |
| 开关开（默认阈值 1 MiB）| 7.61 MB | 7.61 MB | **1.00×** | 界是清扫给的（因果闭合）|

直接数（`gdb` 断点 `extc_rc_sweep`，N=800000、开关开）：**清扫被调用 28 次**。
三份变体的 N/4N 输出都与期望逐字一致。

### 数字 3：三方对照（同 N / 4N，48 字节节点）

| 实现 | 耗时 ms（N / 4N）| 峰值 RSS（N / 4N）| 是否随轮数增长 |
|---|---|---|---|
| extC 默认（开关关）| 8 / 17 | 10.11 / 34.67 MB | **是**（3.45×）|
| extC `EXTC_RECLAIM=1` | 52 / 183 | **7.61 / 7.61 MB** | 否（1.00×）|
| C 原型 `stress.c` 分片=1 | 11 / 27 | 9.48 / 9.48 MB | 否（1.00×）|
| C 原型 `stress.c` 分片=8 | 16 / 41 | 13.67 / 15.67 MB | 否（1.15×）|
| C 原型 `stress.c` 分片=8（200 万轮）| 90 | 16.61 MB | 否 |

原型参数 `N 1024 64 20000 48 <分片>`（缓存 1024 / 每 64 轮 / 索引 2 万项），与 extC 形状的
旋钮对齐；`/usr/bin/time -v` 的 Maximum resident 计。**两边都做到了「有环也不漏」**，
extC 侧峰值甚至略低（7.6 vs 9.5 MB）。

### 差距与原因（extC 比原型慢 3.2×/4.5×）

callgrind，N=800000：extC 开关开 **3.805 G 条指令**，清扫从不触发 **3.581 G**（94%），
开关关 **0.116 G**；C 原型分片=8 **0.621 G**。也就是说：

* **清扫+标记只占开关开那一跑的 ~6%**（`-DEXTC_RC_FLOOR=1 TiB` 只省下 6%）——
  慢的不是清扫，是**每次分配都要付的固定开销**；
* `main` 自身 2.523 G（66%）：清扫触发写在**每个分配点**的语句前缀里，根表是当场物化的
  复合字面量 `(void *[]){ … 1410 项 … }`（1024 缓存 + 256 树根 + 128 桶 + 少量局部）。
  87 万次分配 × ~1410 个槽 ≈ **12 亿次装载/存储**，几乎全在这里。原型只在**真的要扫**时
  （28 次）才凑根表。
* `extc_rc_ownLower` + `extc_rc_ownSlab` = 0.616 G（16%）：计数/登记前必须问 `owns`
  （slab 注册表二分 + 槽首/占用位校验）——**健全性要付的钱**：板/池/栈/C 返回的指针不是
  rc 槽，原型假定「所有指针都是 slab 指针」，extC 不行。
* `extc_rc_holdUpd` + `inc/storeRef` ≈ 0.35 G（9%）：计数。
* 每轮指令：extC 开关开 ≈ 4 757，原型分片=8 ≈ 776（**6.1×**）；吞吐差（4.5×）小于指令差，
  说明 extC 那段根表物化被 GCC 向量化得不错。

**`slab_final.c` 有而这里还没有的**（都在 `RECLAMATION.md`「要做的」清单上）：

* **计数按编译期收窄**：extC 在**每个**指针赋值上发 `inc`/`dec`；原型靠静态唯一性证明
  （`EVENT`/`NOHOLD`）省掉大部分。这是最大的一笔税，也是上面 9% 的直接来源。
* **触发点与根表按需化**：原型按内存压力、在触发时才凑根；extC 的阶段①做成「每个分配点
  前物化全量根表」⇒ 固定开销压过清扫本身（见上）。
* **分片旋钮**：`slab_final.c` 有 `NSHARD`（1→32；停顿 p50 355→82 µs）与停顿分布采样；
  extC 侧靠**词法区域**天然分片，但运行期没有可调分片数、也不采停顿分布。
* **事件回收**（静态唯一性下当场回收）与 **环的可观测性**（让「这里真有环」可统计，
  自学习退场更准）。

**已经对齐的**：位图 slab、槽空闲链、饱和计数、`rsbit` 跨区登记（持久去重、释放清位）、
`owns` 守卫（原型没有）、自学习退场（空扫翻倍）、静态根表由编译器给。

## 复现一条（手工）

```bash
cd <repo>
./build/extc tests/reclaim-shapes/cond_save.extc -o /tmp/cond.c   # 看生成物（提权看得见归属）
gcc -fwrapv -O1 /tmp/cond.c -o /tmp/cond && /tmp/cond
# 峰值 RSS 要**对生成物二进制**取（`extc --run` 时被计的是 extc 自己，不是程序）
./build/extc tests/reclaim-shapes/scale_growth.extc -o /tmp/scale.c
gcc -fwrapv -O1 /tmp/scale.c -o /tmp/scale
/usr/bin/time -v /tmp/scale 800000     # Maximum resident set size（开关关：≈13.8 MB）

# 开关打开：同一份形状 ⇒ 峰值不随轮数涨（≈4.8 MB）
EXTC_RECLAIM=1 ./build/extc tests/reclaim-shapes/scale_growth.extc -o /tmp/scale-r.c
gcc -fwrapv -O1 /tmp/scale-r.c -o /tmp/scale-r
/usr/bin/time -v /tmp/scale-r 800000
```

门禁那组一次跑完（绿；`EXTC_RECLAIM=0` 前置则 ① 与 ①b 都红 ⇒ exit 1）：

```bash
tests/reclaim-shapes/run-switch.sh                        # 通过 9，失败 0
EXTC_RECLAIM=0 tests/reclaim-shapes/run-switch.sh         # 通过 7，失败 2（复现"关掉即红"）
```

有环大压力这一条的**全套证据**（阈值对照 + gdb 清扫计数 + 三方对照）：

```bash
tests/reclaim-shapes/run-evidence.sh
```

单跑 ⑥ 这个形状：

```bash
./build/extc tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/cyc.c
gcc -fwrapv -O1 /tmp/cyc.c -o /tmp/cyc && /tmp/cyc 200000     # 开关关：峰值≈10 MB（N）→≈34.6 MB（4N）
EXTC_RECLAIM=1 ./build/extc tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/cyc-r.c
gcc -fwrapv -O1 /tmp/cyc-r.c -o /tmp/cyc-r
/usr/bin/time -v /tmp/cyc-r 200000                            # 开关开：≈7.6 MB
/usr/bin/time -v /tmp/cyc-r 800000                            # 4N 还是 ≈7.6 MB
# 把清扫关掉（生成物里的阈值是 #ifndef 守卫的宏）⇒ 峰值重新随轮数涨
gcc -fwrapv -O1 -DEXTC_RC_FLOOR=1099511627776 /tmp/cyc-r.c -o /tmp/cyc-nosweep
/usr/bin/time -v /tmp/cyc-nosweep 800000                      # ≈63.5 MB（3.55×）
```

`EXTC_DBG_HOME=1 ./build/extc ...` 也会打每个函数的 home/zone 与 arena 站点（输出含整个
stdlib，噪声大；本目录的结构断言直接读生成物，更稳）。
