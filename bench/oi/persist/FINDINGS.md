## 发现了什么（手写，2026-09-22；上面那张表是 `run.sh --report` 自动生成的，别手改 ✓）

### ① extC 与"C + 手工边界检查"几乎完全重合 ⇒ 它的开销**就是检查本身**

| | 运行 ms | 相对 C | RSS MB |
|---|---|---|---|
| C（`malloc` 池子 + `free`）| 2150 | 基准 | 249 |
| **C（+ 边界检查，形状跟 extC 生成的一样）** | **2280** | **+6.0%** | 249 |
| **extC（`new [POOL]pnode` 池子）** | **2290** | **+6.5%** | 249 |

⇒ 把 extC 生成物里的边界检查**用宏补在手写 C 上**，C 就从 2150 掉到 2280 ✗
⇒ 而 extC 只比"C + 检查"再慢 **10 ms（0.4%）** ✓
⇒ 结论（跟上一份横评一致，这次更干净）：**extC 的运行时开销 ≈ 安全检查本身，
   codegen/编译器那部分可以忽略** ✓（"extC 慢是因为安全"这句话，拆开以后**基本成立**——
   但慢的那 6% 全部是 P′ 想要的那个东西，不是白扔的 ✓）

### ② extC 的「每节点 `new`」是这个负载的**杀手级写法**

```extc
fn update(z: ref pnode, prev: ref pnode, lo: i32, hi: i32, pos: i32) -> mut ref pnode {
    var n: mut ref pnode = new pnode      // ← 每个节点一次，arena 撞指针 ≈ 两条指令
    ...
}
```

- **2200 ms —— 比它自己的池子版（2290 ms）还快 4%** ✓
  原因：指针访问**不做下标检查**（池子版每次 `(*pool)[i]` 都要查一遍）✓
- **RSS 500 MB**（池子版 249 MB）：这是**节点布局**的钱，不是分配器的 ——
  双指针 + `sum` = 20→24 字节/节点，而索引式是三个 `i32` = 12 字节 ✓
- **池子大小不用估、末尾不用 `free`、没有 GC** —— 全程序就一句 `new pnode` ✓

⇒ 这就是 extC 想卖的东西：**"不写 free 的指针式数据结构"**（C 里同样形状见下一行 ✗）

### ③ C 的「每节点 `malloc`」是同一件事的反面教材

| | 运行 ms | RSS MB |
|---|---|---|
| C（`malloc` 池子 + `free`）| 2150 | 249 |
| **C（每节点 `malloc` + 逐个 `free`）** | **4960（2.3×）** | **808（3.2×）** |

21M 次 `malloc` 的**元数据**（glibc 每块 16 字节头 + 对齐）就吃掉 300+ MB，
再加上 21M 次 `free` 的时间 ✗ ⇒ **arena 的价值，在这张表上就是这一行** ✓

### ④ "容器不预估大小"的两家不同命

| | 运行 ms | RSS MB | 增长走的是 |
|---|---|---|---|
| C++（`vector` `reserve` 到位）| 2140 | 251 | —— |
| **C++（`vector` 自己长）** | 2290 | **395** | 新分配 + **拷贝** + 释放 ⇒ 峰值 ≈ 1.6× |
| Rust（`Vec::with_capacity`）| 2110 | 249 | —— |
| **Rust（`Vec` 自己长）** | 2100 | **249** | **`realloc`**（glibc 对大块能原地扩 ⇒ 不翻倍）|

⇒ 同样的"倍增"策略，**Rust 那行 RSS 一格没涨**，C++ 那行涨到 395MB ✓
机制上说得通：Rust 的 `Vec` 增长走 `realloc`；libstdc++ 的 `vector` 是
"`_M_allocate` + `uninitialized_copy` + `_M_deallocate`"（**不**走 `realloc`）✗
（这条是**从机制推的**，不是逐行读源码读出来的 —— 记着别当定理用 ✓）

### ⑤ 构建 / 二进制 / 生成物 / 源码长度

| | 构建 ms | 二进制 KB | 源码行（非注释）| 生成的 C |
|---|---|---|---|---|
| C | 152 | 15 | 145 | —— |
| extC（池子）| 155（前端 **3 ms** + gcc 编 501 行）| 16 | 72 | 501 行 / 25 KB |
| extC（指针）| 162（前端 **3 ms** + gcc 编 528 行）| 16 | 76 | 528 行 / 26 KB |
| Rust | 251 | 11536 | 121 | —— |
| C++ | 414 | 24 | 105 | —— |

- **extC 前端只花 3 ms** ⇒ 用户感受到的构建时间 ≈ 编那 500 行 C 的时间
  （跟手写 C 的 152 ms 基本一样 ⇒ "编译慢"跟 extC 的语法无关，全是 gcc 的活 ✓）
- **源码行数：extC 72 行 < C++ 105 < Rust 121 < C 145** ✓
  （extC 连 `for`/`++`/`+=` 都没有，却是最短的：池子 + 索引本来就不需要什么样板 ✓）
- **二进制**：extC 16 KB ≈ C 15 KB（同一个后端）✓；Rust 11.5 MB 是**静态链接 std** ✗
  （要 `-C prefer-dynamic` 或 `strip` 才能横比 —— 这一格别当语言开销读）
- ⚠️ **C++ 构建最慢（414 ms）**（`<iostream>` + `<vector>` 的模板实例化），
  而运行时间它是最快之一 ⇒ **构建时间和运行时间常常往相反方向走** ✓

### ⑥ 顺手在 extC 里撞到的一条**真实约束**（值得记）

一百万个"版本根"（`[N]?ref pnode` / `[N]ref pnode`）**当全局**会被拒 ✗：

```
error: cannot zero-initialize the global `ROOT`: it contains a reference
note: a global is depth 0, so a reference inside it has nothing to borrow from
      without an initializer.
```

⇒ 这是 P′ 的体现（**非空引用不可能凭空为零**），不是 bug ✓
但 C 里 `Node *root[N];` 全局是常规写法 ⇒ 从 C 过来的人会卡一下 ✓
出路是**放进 arena**：`new [N]?ref pnode`（可空 ⇒ 零值是 `null` ✓）+ 进树处 `!` 签字非空 ✓
见 `bench/oi/persist/p3834_nodes.extc` ✓（"哨兵节点"同样放在 arena 里：`(*z).l = ref *z` ✓）
