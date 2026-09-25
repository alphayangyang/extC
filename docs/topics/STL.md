# STL 库：容器清单与成员函数表

`stdlib/stl/` 是**唯一**装动态容器的地方（一个库装所有容器，不是一个容器一个库）。本文件是成员函数面的
权威清单：表里的每一行都对应源码里的一个 `fn` 声明，改面就要同步改这里。

设计裁决与理由分散在两处，这里只列面：**命名**（`map`/`set` 是有序、`hashMap`/`hashSet` 是哈希、`lin*`
是只要求 `==` 的第三条路）见 `POOLS.md` §10；**B+ 树与池底表**的选型与布局见 `POOLS.md` §8、§11。

## 0. 概览

| 容器 | 键/元素要求 | 组织 | 查找 | 迭代顺序 | `[]` / `[]=` | release / shrink |
|---|---|---|---|---|---|---|
| `pool<T>` | — | 槽池（句柄寻址） | O(1) | 稠密 | 无 | 有 / 有 |
| `vector<T>` | — | 连续数组，下标寻址 | O(1) | 下标序 | 无 | 有 / 有 |
| `string` | — | 连续字节 | O(1) | 下标序 | 无 | 有 / 有 |
| `hashMap<K, V>` | `hash()` 与 `==` | 开放寻址 + 墓碑，**值住池** | O(1) 均摊 | 桶位置 / 稠密 | 有 / 有 | 有 / 无 |
| `hashMapI64<V>` | 键固定 `i64` | 上面那个的适配层（键走 `i64Key` 包装） | O(1) 均摊 | 桶位置 / 稠密 | 有 / 有 | 无 / 无 |
| `hashSetI64` | 元素 `i64` | `hashMapI64<u8>` | O(1) 均摊 | 桶位置 | 无 | 无 / 无 |
| `linMap<K, V>` | 只要 `==` | 稠密线性表 | O(n) | 插入序 | 有 / 有 | 无 / 无 |
| `linSet<T>` | 只要 `==` | 同上 | O(n) | 插入序 | 无 | 无 |
| `map<K, V>` | 只要 `<` | **B+ 树**（节点 16 键、节点住存储、叶子串链） | O(log n) | **有序** | 有 / 有 | 有 / 有 |
| `set<T>` | 只要 `<` | `map<T, u8>` | O(log n) | **有序** | 无 | 无 / 无 |

## 1. `pool<T>` —— `stdlib/stl/pool.extc`

句柄寻址的槽池，`hashMap` 的值、以及容器与运行期池的对应关系都建在它上面。

```
withCap(n) · len · capacity · contains(h) · get(h) -> ?T · set(h, v) -> bool
insert(v) -> handle · remove(h) -> bool
atDense(i) -> T · denseView() -> slice<T> · handleAtDense(i) -> handle
gather(hs, out) -> i64 · toSlice() -> slice<T>
grow · shrink · clear · release
```

内部：`pLow` / `pDense` / `pGen` / `pPack` / `pEpMask` / `pEp`（元数据打包）与 `pLive`（句柄校验）。

## 2. `vector<T>` —— `stdlib/stl/vector.extc`

```
withCap(c) · new() · len · capacity · isEmpty
get(i) -> option<T> · set(i, v) -> bool · denseView() -> slice<T>
push(v) · pop() -> option<T>
grow · shrink · clear · release
```

## 3. `string` —— `stdlib/stl/string.extc`

模块名与类型同名，构造用模块级 `withCap(c)`（`string::new(c)` 与该形状有歧义，见 DEVLOG 周期 9）。

```
withCap(c)（模块级）· new(c) · len · capacity · isEmpty · asSlice() -> slice<u8>
push(b) · append(s: slice<u8>) · appendByte(b)
grow · shrink · clear · release
```

## 4. `hashMap<K, V>` —— `stdlib/stl/hashMap.extc`

```
withCap(want) · len · capOf · find(k) -> i64 · contains(k)
[](k) -> ?V · get(k) -> ?V · []=(k, v) -> bool · put(k, v) -> bool · remove(k) -> bool
keyAt(i) -> K · valAt(i) -> V · liveAt(i) -> bool          按桶位置扫，i 在 [0, cap)
denseLen() · keyAtDense(d) -> K · valAtDense(d) -> V        稠密面：无空洞、连续
rebuild(ncap) · clear · release
```

`hashMapI64<V>`（同文件）：同上去掉 `find` / `rebuild` / `release`，键类型是 `i64`。
`i64Key`（同文件）：给 `i64` 补上 `hash()` 与 `==` 的键包装，用户一般不直接用。

## 5. `hashSetI64` —— `stdlib/stl/hashSet.extc`

无值版哈希集合；注意它的 `put` 与 map 家族**相反**（新元素为真）。

```
withCap(want) · len · contains(k) · put(k) -> bool · remove(k) -> bool
keyAt(i) · liveAt(i) · capOf · clear
```

## 6. `linMap<K, V>` —— `stdlib/stl/linmap.extc`

键只需要 `==`，所以任意类型（含结构体）今天都能当键；表是稠密的、按插入序。

```
withCap(want) · len · capOf · find(k) -> i64 · contains(k)
[](k) -> ?V · get(k) -> ?V · []=(k, v) -> bool · put(k, v) -> bool · remove(k) -> bool
keyAt(i) -> K · valAt(i) -> V · clear · grow
```

## 7. `linSet<T>` —— `stdlib/stl/linset.extc`

```
withCap(want) · add(v) -> bool · contains(v) · remove(v) -> bool · len · at(i) -> T · clear
```

## 8. `map<K, V>` —— `stdlib/stl/map.extc`（B+ 树）

公开面：

```
withCap(want) · new() · len · nodeCount() · height()
contains(k) · [](k) -> ?V · get(k) -> ?V · []=(k, v) -> bool · put(k, v) -> bool · remove(k) -> bool
lowerBound(k) -> i64                                          第一个不小于 k 的排名
keyAt(i) -> K · valAt(i) -> V                                 第 i 小；有序遍历 = i 从 0 到 len-1
firstKey() -> ?K · lastKey() -> ?K
clear · release · shrink
```

内部（不面向用户）：`bn` / `cntOf` / `leafOf` / `kn` / `vn` / `kd` / `nx` / `setN` / `setCnt` / `setKey` /
`setVal` / `setKid` / `setNext` / `growStores` / `newNode` / `kidsCnt` / `subMinKey` / `lowerIn` /
`childSlot` / `leafFor` / `rankAt` / `freeNode` / `dropChild` / `borrowLeft` / `borrowRight` / `mergeLeft` /
`mergeRight`。

## 9. `set<T>` —— `stdlib/stl/set.extc`

```
withCap(want) · new() · len · contains(k) · put(k) -> bool（新元素为真）· remove(k) -> bool
lowerBound(k) -> i64 · at(i) -> T · first() -> ?T · last() -> ?T · clear
```

## 10. 跨容器语义约定

- **`put` 的返回值**：`map` / `hashMap` / `linMap` 是 **true = 键本来就在**（这一次是覆盖）；`set` / `hashSet` /
  `linSet` 是 **true = 这是新元素**。这套差异是显式翻转的，不是笔误（翻转点就在 `set::put` 与
  `hashSetI64::put` 里，各带一句注释）。
- **`[]` 与 `[]=` 互相独立**：`[]` 只读、缺键给 `none`（**没有默认插入**）；`[]=` 是 upsert（缺则新建、
  有则覆盖，返回值同 `put`）。只定义 `[]` 的类型不能通过下标写入，检查器会给出可读诊断而不是"结果不可赋值"。
- **`clear` / `shrink` / `release` 三档**：`clear` 清空留容量；`shrink` 降**水印**（工作集；arena 不能逐块
  还内存，所以不是 RSS）；`release` 把运行期池整批还掉，之后该容器不能再当池用（陈旧拷贝会被 `pidGen` 挡住）。
- **缺键不插值**：所有 `get` / `[]` 都是 `?V`，缺席就是 `none`。

## 11. 已知缺口

- `hashSetI64` 与 `set<T>` 没有把 `release` / `shrink` 转发出来；`hashMapI64`、`linMap`、`linSet` 也没有
  `release`（后两者不登记运行期池）。
- `vector` 没有 `insertAt` / `removeAt`（只有 `push` / `pop` / `set`）；`string` 没有 `pop`。
- `map::shrink` 只压节点存储，不压"每节点 17 格"的稀疏度。
- 有序容器不暴露元素句柄。这是一处**有意的偏离**（POOLS.md §11 的原话是"叶子存键 + 池句柄"）：扁平存储已经
  提供可搬迁、连续、下标寻址，句柄只会给每次读加一层间接，而这个面暴露的是条目、不是元素身份。若以后要拿
  map 里的元素做稳定引用（ECS 那种），再补。
- `[]=` 目前只对"对象类型直接定义"生效；泛型体内对类型参数做下标写入还没接上推迟检查（读侧同理）。

## 12. 验收在哪

| 容器 | 套件 |
|---|---|
| `pool` | `tests/pool/` |
| `vector` / `string` / `hashSetI64` / `set<T>` | `tests/stl/` |
| `hashMap` | `tests/hashmap/` |
| `linMap` / `linSet` | `tests/linmap/` |
| `map` / `set<T>`（有序） | `tests/map/`（`sorted` · `bounds` · `stress` · `churn` · `shrink` · `release` · `index` · `ends` · `structkey` + RSS 两条 + ASan） |

`check.sh quick` 的节数与本文件列出的套件一一对应（当前 26 节）。
