<!-- 本页原来是 docs/MANUAL.md 的「7.8 驱动开关（`-O` / `-march`）」一节（原 §7.8）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 7.7 分配：`new`（`new T` / `new [N]T` / `new T[n]`）](11-alloc.md) · [7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**） →](13-vararray.md)

---

# 7.8 驱动开关（`-O` / `-march`）

```bash
extc foo.extc                  # 默认 -O2
extc -O3 foo.extc -o foo.c     # 指定优化级别（-O0..-O3）
extc -O2 -march=native --run foo.extc   # 让 gcc 用本机指令集（牺牲可移植性）
```

**实测教训**：这两项**收益完全看负载** —— 矩阵乘上 `-march=native` 曾让时间**翻倍**，
而 mandelbrot / binary-trees 在本机**几乎没变化**（`-O2` 已经吃干净了）⇒
**别把它当万能加速**，要压性能先看算法/数据布局（我们那边 matmul 的真正杠杆是**转置 b**）
默认 `-O2` 保持不变：`-O3` 收益小、`-march=native` 牺牲可移植性

---

**[索引](README.md)** · [← 7.7 分配：`new`（`new T` / `new [N]T` / `new T[n]`）](11-alloc.md) · [7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**） →](13-vararray.md)
