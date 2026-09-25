<!-- 本页原来是 docs/MANUAL.md 的「7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**）」一节（原 §7.9）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 7.8 驱动开关（`-O` / `-march`）](12-driver.md) · [8. 内建函数 →](14-builtins.md)

---

# 7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**）

```extc
var v: varArray<i32> = varArray<i32>::withCap(4)
v.push(7)                        // 满了自己扩（倍增）
println(v.size(), " ", v.get(0) ?? -1)
```

| 成员 | 说明 |
|---|---|
| `varArray<T>::withCap(n)` | 造一个容量 n 的（关联函数，要写全类型）|
| `size()` · `isEmpty()` | 长度 |
| `push(v)` | 尾部追加；满了**倍增**（0 ⇒ 先给 4）|
| `pop() -> ?T` | 空 ⇒ `none`（**不 trap** ⇒ 用 `??` 或 `match`）|
| `get(i) -> ?T` | 越界 ⇒ `none`（安全读）|
| `set(i, v)` | 写；`i ≥ cap` ⇒ **trap（带源码位置）** |
| `clear()` | 只把长度归零（**内存不动**）|

**三条设计决定**（跟 extC 的内存模型缝在一起）：

1. **它不带自己的 arena** —— 扩容时的 `new T[cap*2]` 分配到**拥有这个容器的那只 arena**
   （`ARENA.md` §1.2 那条规则）⇒ 容器活多久，存储就活多久
   （所以**没有** C++ 那种 `Alloc` 分配器模板参数）
2. **不能 free** ⇒ `clear()` 只归零，**没有 `shrink_to_fit`** —— 缩不回内存，
   有那个函数就是骗人
3. 读值用 `?T`（「可能没有」），越界由**下标检查**承担

**为什么是倍增**（不是拍脑袋、也不是抄 `std::vector`）：网上说"×2 最差"的论证
（Folly/FBVector：新块永远比之前所有块之和还大 `1+2+…+2ⁿ = 2ⁿ⁺¹−1` ⇒ 复用不了）
**前提是旧内存能被释放**，而 arena **不能 free** ⇒ 我们这边的指标是「累计分配 / 当前长度」，
倍增的最坏值是 **4**，且是**所有递增容量序列的渐近下确界**
（`examples/growth-factor.extc`：解析式 `r²/(r−1)` 在 r=2 取最小 + 精确 DP 验算；
文献与推导见 DECISIONS 定案 59）

**已知限制（两条，跟 `std::vector` 同类，但这里没有缓解）**：

- **扩容会让指向元素的引用失效**：`var p: mut ref i32 = ref v[0]` 之后 `v.push(..)`
  换了新 buffer，`p` 还指着旧 buffer —— arena 保证旧内存不被回收 ⇒ **不崩**，
  但写进去就是**静默写丢** ⇒ **别长期持有元素引用**，要改就用 `set(i, v)`
- `set(i, v)` 只在 `i ≥ cap` 时 trap；`i` 落在 `[len, cap)` 之间会写进未使用区
  （跟 C++ 的 `operator[]` 同款）⇒ **想安全读就 `get(i)`**

**明确不做**（免得以后有人问）：迭代器 / 范围 for（要等 `for` 和函数值）、
`at()` 那种"双入口"（extC 的 `[]` 已经 trap 带位置，不需要两个）、
分配器模板参数、`shrink_to_fit`、`vector<bool>` 位压缩
`insertAt` / `eraseAt` 留到第二版 —— 而且**先要定"引用失效"的策略**（见上面第一条）

---

**[索引](README.md)** · [← 7.8 驱动开关（`-O` / `-march`）](12-driver.md) · [8. 内建函数 →](14-builtins.md)
