# 弱引用（设计稿 v1）

> 状态：**设计稿 v1，未实现** —— 这篇是判据来源。运行期、编译器内建、`std::data::weak` 三块都还没有；
> 实现按本文来，改语义先改本文。

弱引用是「指向一个由回收器管的对象的引用，但不让它活着」。本文只回答四件事：用户怎么写、
语义有三条、运行时怎么在 O(1) 里登记与摘链、以及哪些判据必须在实现时一起立。

## 1 为什么是独立类型，不是 `?ref`

对象上有三种「没有」可以同时出现，必须能分开说：

| 问题 | 类型形状 | 例 |
|---|---|---|
| 键在不在容器里 | 容器的 `at(k) -> ?V` | `map.at("x")` 返回 `?i64` |
| 值本身可空 | 值类型自己带 `?` | `?string` 的 `none` |
| 弱引用还活不活 | `weak<T>::upgrade() -> option<ref T>` | 对象被回收 ⇒ `none` |
| 从未绑定过 | 外层再加一层：`?weak<T>` | 结构体字段初始为 `none` |

`?ref T` 与 `option<T>` 在现在的语言里是**两个族**：

```text
error: `?` needs an `option<...>` or `result<...>`, found `?ref node`
note: `?` works on the `option` / `result` from the prelude.
```

所以弱引用把「失效」放进 `option`（`upgrade` 的返回类型），而不是放进 `?ref`。这样它今天就能吃 `?`
传导、零语言改动；反过来，`?ref` 的可空值 `null` 与「对象已被回收」在类型上也区分得开。

**为什么不用 `result<ref T, PointError>`。** 失效是**预期事件**，不是错误：它是回收器正常工作的一部分。
放进 `result` 有三个代价：① 每个调用点被迫写失败分支；② `?` 会把正常回收变成 early return 的可恢复
失败，调用者容易把它当「操作失败」重试；③ 与主流同族 API 不一致（Java `WeakReference.get`、C++
`weak_ptr::lock`、Python `weakref`、Swift `weak`、Rust `Weak::upgrade` 全部是 option / null）。

## 2 API

<!-- fragment -->
```extc
use std::data::weak

var w: weak<node> = weak<node>::of(ref n);      // 登记；目标必须由回收器管
let got: option<ref node> = w.upgrade();        // 目标还在就是 some(REF)，否则 none
let alive: bool = w.isValid();                  // 不取引用，只问活不活
w = weak<node>::of(ref other);                  // 覆盖：旧的登记当场摘掉
```

三条用户可见语义：

1. **`upgrade()` 的结果在被持有期间是强引用。** 它就是普通 `ref` 局部：回收器扫到栈根就保活。
   所以「拿到引用之后对象突然消失」不存在；消失只会发生在 `upgrade()` 返回 `none` 的那些调用。
2. **它的逃逸级别 = 调用点当前的 zone。** 与 `new` / `alloc` 同一条规则：在帧里登记的结果活不到
   外层作用域，除非提升（`promote`）。不新增一套逃逸求解器。
3. **目标必须是回收器管辖的对象。** 目标是帧 arena 的对象时 `of()` 运行期 `trap`（帧不回收，
   「失效」永远不会发生，弱引用没有意义）。放置是单元级判定、跨单元还可能是泛型调用，所以这条是运行期检查。

## 3 运行时：一张表 + 两侧登记

一次登记要在两侧同时留下痕迹：

- **目标侧**：从目标对象查到「挂在它上面的句柄链」，回收器释放该槽时必须能一次找到整条链；
- **所有者侧**：每份**存储**（帧 arena 块 / rc region）记一张**顺序数组**，存储整体释放时整批摘链、
  不依赖 `@drop` 钩子是否跑过（帧 arena 是整块释放的，钩子不一定跑）。

### 3.1 数据结构

```c
typedef struct { void *target; int32_t tNext, tPprev, wpos; } ExtcRcWeakCell;  /* 24 B，定长池 */
```

| 结构 | 形状 | 每句柄字节 |
|---|---|---|
| cell 池 | 定长数组 + `i32` 空闲链；`target == NULL` 表示已失效 | 24 |
| 目标侧链 | `i32 tNext` + `i32 tPprev`（**索引** hlist，`n` 号槽当哨兵头）| 已含在 24 里 |
| 全局地址表 | 开放寻址，`addr -> i32 cellHead`，**定长、永不 rehash** | 摊到表项 |
| 所有者侧 | 每份存储一个 `int32_t *wcel; uint32_t wn, wcap` 顺序数组；`cell->wpos` 指向自己在数组里的位置 | 4 |
| `weak<T>` 值 | 一个 `i32`（0 = 未绑定；cell 下标从 1 起，于是零值可安全 drop）| 4 |

合计 **28 B / 值**（24 的 cell + 4 的 owner 槽）。`weak<T>` 值本身 4 B：它只是一个下标。

**每个值一个独立的 cell，不共享、没有引用计数。** 拷贝就是再登记一次（O(1)，几次分配器弹链）：
两个副本各自失效、各自摘链，生命周期不纠缠；代价是拷贝多占 28 B。这样 `weak<T>` 保持 `@value`
可拷贝（字段、容器元素、`option` 载荷都能装），且「谁的存储死了就摘谁的」在两侧都只有一条路径。

### 3.2 四条时间线

| 事件 | 做什么 | 界 |
|---|---|---|
| `of(p)` | 查表得目标链头 → 弹一个 cell 挂到链头 → 追加到自己所属存储（调用点的 zone）的 `wcel` | O(1) |
| `upgrade(w)` | `cell->target != NULL` ⇒ 返回该地址（类型 `T`）| O(1) |
| `weak` 值 drop | 从目标链摘掉（`*tPprev = tNext`）+ owner 数组 swap-remove（`wpos`）+ 归还 cell | O(1) |
| 拷贝 | 等同于对新值再 `of(同一个 target)`；旧值已失效时新值也直接是失效态 | O(1) |
| 目标被回收 | 查表拿链头，逐个 `*tPprev = tNext; target = NULL` | O(k)，k = 该目标上的句柄数 |
| 存储整体释放 | 顺序遍历 `wcel`，逐条摘链 + 归还 cell | O(自己存储的句柄数) |

### 3.3 为什么是索引链、为什么不做 find-then-unlink

目标侧用**索引**而不是指针（[`bench/list`](../../bench/list/RESULTS.md) 的数字）：

| 场景 | 指针 hlist | 索引 hlist |
|---|---|---|
| 1M 节点随机顺序 O(1) 摘链 | 8.40 ns | **3.83 ns** |
| 1M 节点遍历一遍 | 3.23 ns | **1.06 ns** |

另两条同表结论决定两个取舍：

- **两星指针（`node **`）不更快**：4,096 节点扫一遍 3,179 vs 3,182 ns（0.09%，噪声内）。它省 8 B/节点，
  省的是空间，不是时间。cell 里存索引时两星写法自然消失（前驱槽就是 `i32 tPprev`）。
- **绝不对链做 find-then-unlink**：4,096 节点扫一遍要 3,182 ns，而位置已知时摘一个 8.40 ns。
  每条目标链通常只有 0–2 个句柄，但实现不能依赖这个分布 —— 链头由地址表直接给出。

### 3.4 失效与槽复用（ABA）

槽被复用后，旧句柄必须**永远**读不到新对象。三条规则合起来保证它：

1. 回收器在把槽放回空闲链**之前**清掉该目标上的全部句柄（`target = NULL`）；
2. 一次要清的 k 超过预算（默认 64）时，剩下的推入**待清队列**，由后续 `sweep` 步按预算消费；
   队列里的槽**不得**进入空闲链；
3. `upgrade` 只信 `cell->target`，不做「地址还活着就算活」的推断。

第 2 条是唯一的停顿来源：单步最多清 64 个，与 sweep 的槽预算同阶。清空之后槽才可复用，所以 ABA 不成立。

## 4 与回收器的接线点

| 位置 | 动作 |
|---|---|
| free 槽之前（清扫档的 free 路径、计数档归零的那一次）| 查表、清链、必要时入待清队列 |
| 存储整体释放（arena 块退出、closing queue 归还 region 之前）| 顺序遍历 `wcel` 整批摘链 |
| `extc_rc_settle` 的步进 | 消费待清队列（与槽释放同一预算口径）|
| T0（不发运行期）| 程序里出现 `weak::of` ⇒ 强迫该编译单元走回收器；若放置仍是 T0，编译期报错 |
| T1（计数档）| 同一个接线点：计数归零即释放，清链在释放前 |

运行期探针一行：`[weak] cells= cap= cleared= trapped=`（`cells` 在册句柄数、`cleared` 累计置空数、
`trapped` 累计拒绝的 `of()` 数）。

## 5 编译器侧

三个内建，走现有 `@builtin` 机制（与 `extc_viewOf` / `alloc` 同一套）：

| 内建 | 签名 | 说明 |
|---|---|---|
| `weakOf<T>(p: ref T) -> weak<T>` | 登记 | 隐式传调用点的 zone，决定 owner 数组；目标不是回收器对象 ⇒ 运行期 trap |
| `weakUpgrade<T>(w: weak<T>) -> option<ref T>` | 读 | 结果是一个普通引用，逃逸级别按调用点 |
| `weakDrop(w: weak<T>)` | 摘链 | `weak<T>` 的 `@drop` 钩子调用它 |

`weak<T>` 的 clone / drop 钩子用现成的 `@value + cloneHook + @drop` 机制（`docs/design/DESTRUCTORS.md`），
不新增所有权规则。检查器只在三处加判定：`of` 的实参必须是 `ref T` 且 `T` 不是视图元素类型；
`upgrade` 的结果按普通 `ref` 求逃逸级别；`weak<T>` 可作结构体字段、容器元素、`option` 载荷（它是 4 B 的 `@value`）。

## 6 判据（实现时一起立）

| 判据 | 做法 |
|---|---|
| G23 不复活（ABA）| 10 万个对象反复「登记 weak → 释放 → 同尺寸重分配」；每次升级都必须 `none` 或指向**新**对象，不许指向被复用的旧地址 |
| 死亡停顿有界 | 一个对象上挂 1 万个句柄后释放它：`over1ms = 0`，且单步清链 ≤ 64（`[weak] cleared` 差值）|
| 拷贝语义 | `var b = a` 后原件离开作用域，`b.upgrade()` 仍有效；两者都离开后 `[weak] cells = 0` |
| 跨存储 | 对象在 region A、句柄值存在 arena B：B 退出后 A 的链上不再有该句柄（`cells` 与 `[weak]` 计数回到登记前）|
| T0 拒绝 | 只做 arena 分配的程序里 `of(帧对象)` ⇒ 运行期 trap 且文本指名「不是回收器对象」|
| 进出平衡 | 测试结束时 `[weak] cells = 0`，`buf` / `rc` 的计数器不变（句柄不干扰回收路径）|

实现落地时 `emitted_identity` 会变：生成的 C 多一个 `[rt:weak]` 块、`weak<T>` 的 clone/drop 钩子、
以及 `of` / `upgrade` 两个调用点。重冻结前必须逐程序核对差异只落在这几处。

## 7 边界与未做

- **只对单线程。** `par` 的 worker 各自持有 region，跨线程登记需要锁与「哪个线程清」的规则；并发设计暂缓。
- **不做 ephemeron。** 「键弱、值跟着键一起死」需要回收器传播可见性，是另一种机制（Java 的 `WeakHashMap`
  语义），不在本设计里。
- **不做「弱引用当容器键」的接口糖。** 容器先把 `at(k) -> ?V` 定完，弱引用只做「值可失效」这一件事。
- **cell 池与地址表定长。** 满了 `of()` 直接 `trap`（`EXTC_RC_WEAK_TABLE_MAX`，默认 1<<16 项）；
  不做扩容，因为扩容要移动 cell 地址、会让在册句柄的下标全部失效。
- **stdlib 层等容器定稿。** `std::data::weak` 的公开面（`of` / `upgrade` / `isValid`）可以后写；
  运行期与编译器内建不依赖容器实现，先做不冲突。