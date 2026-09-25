# 标准库参考：容器的共同面

`map` / `hashMap` / `linmap` / `pool` / `set` / `hashSet` / `linset` 共享一组同形操作。
各容器的**选择依据**（有序 vs 哈希 vs 线性、是否连续、能否装 `ref T`）另见各容器页。

| 操作 | 签名 | 说明 |
|---|---|---|
| 插入 | `insert(self: mut ref C, k: K) -> bool`（`pool` 为 `insert(self: mut ref pool<T>, v: T) -> handle`） | `map`/`hashMap`/`linmap`/`set` 返回"是否新插入"；`pool` 返回**句柄** |
| 删除 | `remove(self: mut ref C, k: K) -> bool`（`pool` 为 `remove(self: mut ref pool<T>, h: handle) -> bool`） | 不存在时返回 `false`，不是 trap |
| 容量 | `capOf(self: ref C) -> i64` | 当前容量（按约定属簿记，可达、不承诺兼容） |
| 池占用 | `bytesOf(self: ref C) -> i64` | 该容器在池里占用的字节数 |
| 末元素 | `set::last(self: ref set<T>) -> ?T` | 有序集合的最后一个元素；空集返回 `none` |
| 带父池构造 | `pool::withParent(parent: i64, n: i64) -> pool<T>` | 指定父池与初始容量 |

上表只列**同形**的部分：每个容器还有各自的核心操作（`map` 的有序查询与名次、`hashMap` 的
`get`/`find`、`pool` 的句柄校验与 `gather`、`set` 的集合运算），以各自的页为准。

## 同形操作的其余部分

| 操作 | 签名 | 说明 |
|---|---|---|
| 写入或替换 | `put(self: mut ref C, k: K, v: V) -> bool` | 键存在则替换、不存在则插入；返回"是否新插入"（`hashSet` / `set` / `linmap` / `map` / `hashMap` 同形） |
| 集合添加 | `linSet::add(self: mut ref linSet<T>, v: T) -> bool` | 线性集合的添加 |
| 重建 | `hashMap::rebuild(self: mut ref hashMap<K, V>, ncap: i64)` | 以新容量重建哈希表（O(n)） |
| 句柄比较 | `pool::same(a: handle, b: handle) -> bool` | 两个句柄是否指同一格同一代 |
