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

## 键的哈希

| 函数 | 签名 | 说明 |
|---|---|---|
| `hashI64(k: i64) -> i64` | 整数键的哈希（供 `hashMapI64` 一类使用；`i64` 的 `hash` 方法即由 `impl i64` 提供，见语言页的 `impl` 一节） |

## 遍历与名次（有序 / 线性 / 哈希容器同形）

| 操作 | 签名 | 说明 |
|---|---|---|
| 取键 | `keyAt(self: ref C, i: i64) -> K` | 第 `i` 个**存活**键（`0 ≤ i < len()`），按容器的迭代次序 |
| 取值 | `valAt(self: ref C, i: i64) -> V` | 与 `keyAt(i)` 配对的取值 |
| 名次 | `map::lowerBound(self: ref map<K, V>, k: K) -> i64` | **首个不小于 `k` 的键的名次**，即严格小于 `k` 的键的个数；借助子树规模，O(log n) 而非遍历 |
| 名次反查 | `map::rankAt(self: ref map<K, V>, i: i64) -> i64` | **第 `i` 小的条目**所属的叶子位置，编码为 `leafId * 16 + position`（`16` 即 `ORDER`，一个叶子不会超过它）。之所以编码成一个整数：语言里没有元组类型 |
| 下界 | `set::lowerBound(self: ref set<T>, k: T) -> i64` | 集合上同义的名次查询 |
| 集合取键 | `hashSet::keyAt(self: ref hashSetI64, i: i64) -> i64` | 哈希集合的第 `i` 个元素 |

`map` 的迭代次序是**键的升序**；`hashMap` / `linmap` 是插入与重排之后的物理次序（`linmap` 保持插入次序，
哈希表在 `rebuild` 之后次序会改变）。

## 造一个"装着内容"的容器：`from`

没有隐式转换，也没有"字面量偷偷采纳上下文类型" —— 所以从一段内容造容器要**明写**：

```extc
use stl::string
use stl::vector

fn main() -> i32 {
  let s = string::from("abc")            // slice<u8> → string（拷贝）
  var a: [3]i64 = [i64(1), i64(2), i64(3)]
  let v = vector::from(a[..])            // slice<T> → vector<T>（拷贝；T 靠推断，不用写）
  return i32(s.len() + v.len()) }
```

`vector::from` 那条要个**有名字的数组**：字面量是临时的，而"不能切临时值"是有意的规矩
（视图必须有主人）。字符串字面量没这个问题 —— 它本身就是 `slice<u8>`，直接给 `string::from`。
