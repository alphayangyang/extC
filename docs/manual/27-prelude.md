# 标准库参考：`prelude`

prelude 是随每次编译自动前置的一小段 extC 源码（`stdlib/prelude.extc`），提供语言级设施：
`slice` / `option` / `result` / `varArray` / `unit`，以及下表的随机数发生器。

## `pcg32`：随机数

| 成员 | 签名 | 说明 |
|---|---|---|
| 构造 | `pcg32::seeded(seed: u64) -> pcg32` | 用种子构造 |
| 构造 | `pcg32::withStream(seed: u64, stream: u64) -> pcg32` | 指定种子的同时指定流（同一序列可用于需要独立子序列的场合） |
| 有界整数 | `pcg32::nextBounded(self: mut ref pcg32, bound: u64) -> u64` | 返回 `[0, bound)` 内的均匀整数 |
| 浮点 | `pcg32::nextFloat(r: mut ref pcg32) -> f64` | 返回 `[0, 1)` 内的浮点数 |
| 洗牌 | `pcg32::shuffleI32(r: mut ref pcg32, a: mut slice<i32>)` | 原地洗牌（Fisher–Yates） |

## `varArray<T>`：arena 底的动态数组

| 成员 | 签名 | 说明 |
|---|---|---|
| 借出可写视图 | `varArray::asMutSlice(self: mut ref varArray<T>) -> mut slice<T>` | **零拷贝**：视图指向该容器的存储 |
| 借出只读视图 | `varArray::asSlice(self: ref varArray<T>) -> slice<T>` | 同上（只读） |

两条规矩（实测行为，见「内部成员一览」之外的判据 `tests/stl/varArray_snapshot.extc` 与
`tests/stl/varArray_mutview_lost.extc`）：

1. **扩容即作废**：`push` 触发扩容会换块，旧视图**停在扩容前的快照**上 —— 读到旧值、写进去会静默丢；
2. **视图可带出调用**（缓冲由 home 机制放在调用者选的地方，比这次调用活得久），因此
   `sort(v.asSlice())` 排的是**真存储**，这正是零拷贝存在的理由。

## `pcg32` 的状态

| 成员 | 签名 | 说明 |
|---|---|---|
| 状态 | `pcg32.state: u64` | 发生器的内部状态；可取可写（存读档时直接存取），按约定属实现细节 |

## 通用算法

| 函数 | 签名 | 说明 |
|---|---|---|
| 排序 | `sort<T>(s: mut slice<T>)` | **原地**排序；元素类型需要在实例化时提供 `less` 方法（模板期不查，实例期报错并指出具体实例） |
| 批量拷贝 | `copyInto<T>(dst: mut slice<T>, src: slice<T>, n: i64)` | 把 `src` 的前 `n` 个元素拷进 `dst`（整段移动，不是逐元素循环） |

两者都是与具体容器无关的**切片级**算法：容器先借出视图（`toSlice` 拷贝或 `asSlice` 零拷贝），
再把视图交给算法。`copyInto` 也是 `append` 一族"整段追加"的实现基础。
