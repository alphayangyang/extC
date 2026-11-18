# hashMap 的形态（设计稿 v1，未实现）

## 0 一句话

一个哈希表本体只需要三样东西：一块桶数组、线性探测、重哈希。其余上千行都在回答同一个问题 —— **元素能不能被取引用**。这个问题应该在语言层一次答完，容器里不重答。

## 1 目标与非目标

目标：

- 每元素元数据从 20 B 降到 **0**；存储从五块（四列 + 值列）降到**两块**（桶 + 标签）；
- 访问**零拷贝**：拿到的就是桶里那几字节；
- **零编译器特例**：本稿不需要任何新的语言原语，pool 那一族特例整族消失；
- **不依赖 GC**：存储是 `dense::block<T>`（显式 `release`），与今天一致；
- 有硬判据（§8），其中 h1 峰值与 `datalog_lite` 的 RSS 红线是必须过的。

非目标：

- **不承诺元素地址稳定**（C++ 的 `unordered_map` 承诺它，代价是每元素一个节点、多 32~48 B）；
- **不支持跨结构修改持有元素引用**（需要那个能力用 `std::weak`）；
- **不做增长期的有界停顿**（理由见 §6，选择与 C++ 同一档的全量重哈希）；
- 不做并发/跨任务共享（今天的约束不变）。

## 2 结构

```
@noCopy struct hashMap<K, V> {
    @private bk:   block<kv<K, V>>   /* 桶：键与值内联 */
    @private tag:  block<u8>         /* 0 空 / 1 活，与桶一一对应 */
    @private n:    i64
    @private cap:  i64
    @private pins: i64               /* 活着的守卫个数，> 0 时结构被冻结 */
}

@private struct kv<K, V> { @private k: K   @private v: V }
```

桶 16 B（8 字节键 + 8 字节值）、标签 1 B，合计 **17 B/桶**。标签单独一列是为了不给 8 字节对齐付 7 B 的 padding：内联成 `struct { tag: u8, k: K, v: V }` 会变成 24 B/桶（+41%）。

存储复用 `stdlib/std/data/dense.extc` 的 `block<T>`，只用到 `[]`、`[]=`（即 `elemPut`，写**没有元素**的槽）、`take`（`elemTake`）、`dropAt`（`elemDrop`）、`growTo`/`shrinkTo`、`release` —— 全部是已实现的东西，**本稿不新增原语**。

## 3 不变量

- **I1** `bk` 与 `tag` 同长 = `cap`；`cap` 是块容量，`n` 是活元素数。
- **I2** `tag[i] == 0` ⟺ 桶 `i` 里没有元素。写有元素的槽必须先 `take` 出来（这就是 `elemPut` 与普通赋值的区别）。
- **I3** 装载 `n / cap ≤ 3/4`；超过就重哈希到 `max(8, n * 2)`（倍数取 2 的理由见 §6）。
- **I4** `pins > 0` 时任何结构修改（`put`/`remove`/`clear`/`release`/重哈希）一律 `trap`，文本与 `string` 的 `frozen()` 同形。
- **I5** 不承诺元素地址；对元素的引用只能活在守卫里（§4）。

## 4 API

```
at(k)        -> ?mapGuard<V>    /* 借用：读或改桶里的值 */
put(k, v)                       /* 移动进来 */
remove(k)    -> ?V              /* 搬出去，不是拷 */
contains(k)  -> bool
withCap(c)                      /* 预置容量，避开重哈希 */
len()        -> i64
clear()
```

守卫纯库代码，形状与 `stringGuard` / `borrowGuard<T>` 一致：

```
@private struct mapGuard<V> { @private r: ref V   @private p: mut ref i64 }
fn at(self: mut ref hashMap<K,V>, k: K) -> ?mapGuard<V> { ... self.pins = self.pins + 1 ... }
fn release(self: mut ref mapGuard<V>) { *self.p = *self.p - 1 }   /* @drop */
```

**没有** `slotHandle`、**没有** `entry`、**没有**返回 `ref V` 数组的迭代器。

## 5 索引、删除、增长

- 索引：`i = ((h >> 32) * cap) >> 32` —— 只用 64 位乘法，不需要 128 位（语言里也没有）。`h` 由 `K` 的哈希给出；`cap` 因此可以不是 2 的幂，装载率不必被 pow2 浪费。
- 探测：线性、步长 1、回绕；命中判定先看 `tag` 再看 `k == k`。
- 删除：**回移**（Knuth TAOCP 3 §6.4 alg R）—— 把后面的元素往前挪填洞，不留墓碑，因此不需要 `dead` 计数，也不需要「整表重建来清墓碑」。
- 增长：新块按 I3 的容量，逐个活桶 `take` 出来再 `put` 进新块（**是移动，不是拷贝**），旧块当场 `release`。

## 6 为什么增长是全量重哈希

三条路，只有一条不需要特例：

| 方案 | 单步 | 峰值 | 代价 |
|---|---|---|---|
| 全量重哈希（本稿） | O(n) | 旧 + 新（瞬时） | 与 C++ `unordered_map` 同一档；`withCap` 是逃逸门 |
| 增量迁移（双表） | O(批量) | 旧 + 新（全程） | 原型实测 83.9 B/元素 —— 等于没省，且旧表未迁完时探测链会断 |
| 页粒度分裂 | O(一页) | 等于稳态 | 需要「第 i 页在 `base + i * 4096`」= 指针算术；语言没有 ⇒ 要么加编译器原语（特例），要么每页一个 GC 对象（引入 GC 依赖） |

所以 Phase 1 选全量重哈希，代价写清楚：**h2 这类会增长的形状会看到 5 ms 量级的单步**（今天 `rebuild` 是 5.3 ms/次，不是回归），已知规模的形状（h1、`datalog_lite`）在 `withCap` 之后根本不重哈希。

## 7 删掉什么（这一节是设计的主体）

| 删 | 为什么它存在过 |
|---|---|
| `slots<T>` 的槽元数据（活槽编码、洞链 `holeHead`、`cursor`） | 池板块会复用 ⇒ 需要世代判陈旧；块不会被复用，句柄也没了 |
| `slotHandle`（槽号 + 世代） | 元素被搬走/删掉后旧句柄要能失效；现在不承诺地址，也不需要失效检查 |
| `slot` 列、`bucketOfDense` 反向列、稠密值列 | 值内联进桶之后这三列没有对应物 |
| `pStale` / `pPoison` / `pidGen` | 同上：世代与中毒都是池块复用的产物 |
| `rebuild` / `adopt` / `dead` / 装载 0.7 的 pow2 算术 | 换成 2 倍增长 + fastrange 索引 + 回移删除 |
| API `at(slotHandle)` / `slotOf()` / `remove(h)` | 换成吃键的 `at(k)` / `remove(k)` |

编译器的 **pool 特例族**（`isPoolPrimitiveName` / `checkPoolPrim` / `@poolObject` / `growMethodName` 的池分支）随这一轮整族消失；本稿不新增任何特例。

## 8 判据

- **J1** `bench/containers` 的 h1 峰值 RSS ≤ **120 MB**（今天 239.6 MB）；
- **J2** h1 查询墙钟 ≤ **1.3×** 同算法手写 flat 表；
- **J3** h1 插入 ≤ **1.2×** 同算法手写 flat 表；
- **J4** h2/h3（会增长的形状）总墙钟不高于今天，且重哈希次数/累计毫秒作为固定输出记录下来（不许藏在总量里）；
- **J5** `tests/gc-torture/src/datalog_lite.extc` 的峰值 RSS ≤ **32,768 kB**（今天是 36,804 kB 的 RED，bisect 指向 `5de8fd5d` 的七块布局）—— 不 xfail、不放宽；
- **J6** `tests/hashmap`、`tests/pool`、`tests/dense`、`bench/containers` 全绿；
- **J7** 全门禁 + `emitted_identity` 重冻结（变化只允许落在 hashMap/hashSet 相关程序）；
- **J8** 生成 C 里 `extc_pool_` 出现 **0** 次 —— pool 族退场的机械判据。

## 9 边界

- **值含 `@drop`**：`clear`/`release` 要逐桶跑钩子（有界、可预算）；整表丢弃不是丢引用而是走 `release`。
- **大值**：Phase 1 一律内联（`V` 大于一两个 cache line 时桶变稀疏，未来可给桶存指针的形态，不在本稿）。
- **迭代**：Phase 1 不提供迭代器（今天也没有）；需要遍历用 `keys() -> block<K>`，代价是一次 O(n) 拷贝，明确写在文档里。
- **跨修改持引用**：不支持，用 `std::weak`（已实现）。
- **借用期改结构**：运行期 `trap`，不是编译期诊断（要编译期诊断就得让检查器认识这个类型 = 特例）。

## 10 落地顺序

1. **本稿**（设计 + 与原型数字对齐）；
2. 换内部存储：`block<kv>` + `tag` 列，删 `slots`/`slot`/`bucketOfDense`/`rebuild`/`adopt`，先保持 `slotHandle` API 不动跑通全部测试；
3. 换 API：`at(k)` / `remove(k)` / 守卫，改 `tests/hashmap`、`tests/pool`、`bench/containers`、`datalog_lite`；
4. 删 `slots<T>` 与 `slotHandle`，pool 特例族退场，`emitted_identity` 重冻结 + 文档 + changelog。
