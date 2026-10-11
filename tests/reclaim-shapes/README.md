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

曾经的 SEGV 用例 = `tests/heap/loan`、`tests/heap/plate`、`tests/cabi/heapmain`、
`tests/coro/iterator_protocol`：板/池/栈/C 返回的指针不是 rc 槽，开关把计数挂在指针赋值上后这些
形状都会经过 `inc`/`dec`/`register`；`owns` 守卫必须一个字节都不碰（否则 `slabOf` 的按位与会
找到一段无关内存，板的 4 GiB 保留区甚至没 commit ⇒ SEGV）。

### `./run-switch.sh`（门禁，`EXTC_RECLAIM=1`）

判据：`scale_growth` N vs 4N 峰值比 **< 1.5**；`cond_save` / `cyclic_shape` 语义不变；
上表四个曾经的 SEGV 用例 **rc=0** 且输出逐字不变。实测：

| 场景 | N=200000 | N=800000 | 比值 | 判定 |
|---|---|---|---|---|
| 开关关 | 4.61 MB | 13.79 MB | **2.99×** | 红（`run-switch.sh` exit 1）|
| 开关开 | 4.79 MB | 4.79 MB | **1.00×** | 绿（`run-switch.sh` exit 0）|

这就是「判据有牙」的证据：把开关关掉这条断言必须失败。


## 逐条

| 文件 | 覆盖 | 判据 |
|---|---|---|
| `cond_save.extc` | ① 条件保存在循环里（motivating shape）| 编译 + 语义（`saved=7`）|
| `reject_deep_store.extc` | ② 跨帧存储，直接形式 | 必须报 `dies first (borrowed from depth 2 ... depth 1)` |
| `reject_deep_store_call.extc` | ② 跨帧存储，经被调者发布（定案 67）| 必须报 `points into a deeper scope (depth 2) ... (depth 1)` |
| `cyclic_shape.extc` | ③ 双向链表 + 父指针树 + 缓存 + 索引 | 编译 + 运行 + 语义（两个方向的和相等、父指针指回根、缓存/索引命中）|
| `promote_place.extc` | ④ 提权发生的位置 | 生成物结构：逃逸的那份在循环体区域**之外** |
| `scale_growth.extc` | ⑤ 同一形状轮数 ×4 | 峰值 RSS 是否随轮数增长（阈值 1.5×）|

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
只能靠区域清扫（机制文档「四层」里的第四层，今日未实现）。缓存与索引是**跨结构引用**，
对应 prototype-reclaim 里「记忆集/跨区登记」要处理的那类边。

规模取 8 个节点（秒级）；峰值只有进程底噪量级，所以这里**不设上界** —— 规模行为由 ⑤ 那条管。

### 红的那条（⑤）

`scale_growth.extc` 与 `cond_save.extc` 是同一个形状（条件保存），只是轮数由 argv 给：
`run.sh` 跑 `200000` 与 `800000` 两次。每一轮 `new` 出来的存储都可能进 `saved` ⇒ 被提到函数层
区域，函数返回前整片不回收 ⇒ 峰值 RSS 随轮数线性增长。

这正是 `RECLAMATION.md` 要解决的问题：「不可达但仍占存储的对象，其总量有界，且这个界与循环
轮数无关」。**开关关**时界与轮数有关 ⇒ 红；**开关打开**（`EXTC_RECLAIM=1`）后计数归零当场回收
+ 区域清扫兜环 ⇒ 峰值与轮数无关（实测 2.99× → 1.00×），这一条在
[`run-switch.sh`](run-switch.sh) 里被固化成门禁断言。

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

门禁那组一次跑完（绿；`EXTC_RECLAIM=0` 前置则 ① 红 ⇒ exit 1）：

```bash
tests/reclaim-shapes/run-switch.sh
EXTC_RECLAIM=0 tests/reclaim-shapes/run-switch.sh   # 复现"关掉即红"
```

`EXTC_DBG_HOME=1 ./build/extc ...` 也会打每个函数的 home/zone 与 arena 站点（输出含整个
stdlib，噪声大；本目录的结构断言直接读生成物，更稳）。
