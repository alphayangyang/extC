# region（命名区域）· 设计稿 —— **ECS 版**（2026-09-24 整篇重写）

> **状态**：方向已定（主人 2026-09-24：「**总之你参考 ECS**」· 注册表选 **(a) 每帧一个**）。
> 细节待批（§9 一张清单）。本文**重写过一次**：旧版把 region 当"能力值"，讨论 `@nocopy`／
> double-free／affine —— 那些结论**已被本版取代**，见文末「变更史」✓

---

## 1. 要解决的三个问题（主人原话）

1. **用户无法手动回收容器里的垃圾**（erase 掉的槽位/旧代缓冲永远留在 arena 里 ✗）
2. **无法复用**（同一块内存不能重新利用 ⇒ 反复 add/erase 就线性涨 ✗）
3. **指针跳转导致 SIMD 难以优化**（链式/树式结构，每次访问都是一次 cache miss ✗）

主要应用面是 **STL**：`varArray` · `string` · `set`，后续 `graph` 这一类封装 ✓
而 region 要支持泛型 ⇒ 泛型套泛型 ⇒ **region 套 region**（逻辑嵌套）✓

## 2. 为什么今天不行（量测，2026-09-24 当场跑的）

`/usr/bin/time -f "%M"` 峰值 RSS：

| 形状 | 峰值 RSS | 活跃数据 | 结论 |
|---|---|---|---|
| **A** 翻倍长到 1e5，但每轮在**会退出的块**里做（12 轮）| **2,640 KB** | — | **平** ✓ 块退出即还 |
| **B** **一个**数组从 1 翻倍长到 1e6（4 MB）| **9,900 KB** | 4,096 KB | **≈2.4×** ✗ 历代旧副本全留着 |
| **C** 每轮新建/重建，分配点落在**长命层**（12 轮）| **14,184 KB** | — | **随轮数线性涨** ✗✗ |
| **D** 一次要够 | **5,676 KB** | 4,096 KB | ✓ 只差进程基线 |

**两条规则**：arena 只在**分配点那一层的块退出**时回收 ⇒（i）长命层里任何分配都是永久的 ·
（ii）同一个容器对象复用则浪费有界（B ≈1× 活跃量），**每轮新建则无界**（C）✗✗
⇒ 服务型程序（跨请求状态、缓存、会话表）**必然**撞在（i）（ii）上 ✓

## 3. 设计：ECS 版

### 3.1 region = **注册表里的一个槽**，`new` 出来的是 handle

- `region`（对用户而言）是一个 **handle = 索引 + generation** ⇒ 是**普通值，可以自由复制** ✓
- 槽里放的是真正的 arena（块链）+ 父槽 + generation + 子链 ✓
- ⇒ **"拷贝容器 ⇒ 双主"不存在** ✓（持有者不是所有者，所有者是注册表/父节点）
  ⇒ 也因此**不需要 `@nocopy`** ✓（旧版那条撤回）

### 3.2 容器的**可变簿记也放进 region**，用户手上只有 handle

不是"struct 里塞一个指向 region 的指针"，而是：**struct 里只有 handle**
（`{ r: region, d: handle }`，`d` 指向**住在 region 里**的 descriptor）✓
⇒ "两个 header 共享一块内存 ⇒ free list 成环死循环"这个隐患**从根上消失** ✓
⇒ 代价：每次操作多一次查表（可批量校验 ✓）✓

### 3.3 失效 = **generation 校验 ⇒ trap**（主人 2026-09-24 定：「ECS 里的东西 trap 即可」）

`region::drop(rid)` · `region::reset(rid)` · **创建它的块退出** ⇒ **generation 加一** ⇒ 任何旧
handle 的访问 ⇒ **trap（带位置）** ✓

**为什么这是对的**（它其实正好合 extC 的既有口径：**bug ⇒ trap；条件 ⇒ 值**）：

| 情形 | 类别 | 怎么办 | 语言里的先例 |
|---|---|---|---|
| stale handle（drop/reset 之后再用）· 槽位越界 · 用已释放的 region | **程序 bug** | **trap**（带位置）| `a[i]` 越界就是 trap ✓ |
| "这个键/槽位没有东西" · 想先问一下 | **正常条件** | **值**：`option` / `?T` / `contains` / `valid` | `slice` 的 `hasAt` / `get` ✓ |
| 分配失败（region/arena 要不到内存）| 系统条件 | 运行时已 trap（带位置 ✓）| `extc_arena_alloc` 现有行为 ✓ |

⚠️ **但有一条铁律会撞上**：trap 必须带**正确位置** ✓ 而**库里的 trap 会指到库自己的源码行** ✗
（`@inline` 也一样：`#line` 指向库源 ✓）⇒ 两条路：
- **(a) 查询式 API**（`valid(h)` / `tryGet(h)` / `contains(k)`）让调用者**先问** ⇒
  服务型不会因为一个陈旧 handle 就整个进程死掉 ✓ **零编译器改动** ✓
- **(b) `@trackCaller`**（小特性）：被标的方法额外收一对**隐藏参数**（调用点的 file + line），
  于是 trap 指到**用户那一行** ✓ —— 机制与隐藏 `__extc_home` 参数**同源** ✓ 小 ✓

⚠️ 这也是一处**有意偏离**：`prelude.extc` 顶上写着"会 trap 的库不符合 extC 的性格" ✗
⇒ 这里的口径是：**那条规矩管"条件"，不管"bug"** ✓ 容器内部的陈旧索引是 bug ⇒ 该响就响 ✓
（服务型想要"别死"，就用 (a) 的查询式 API ✓）

⚠️ 另一处**有意的降级**：容器不追求"编译期挡悬垂"，改用**校验过的索引** ✓
理由：`reset` 的时机本质上是运行时的（epoch 什么时候结束只有程序知道 ✓）⇒ 静态规则管不到 ✓
（extC 别处仍是"能证明就编不过" ✓ 这条降级**只限容器内部寻址**，不外溢 ✓）

### 3.4 块与 region 是**同一棵树**上的两种节点

- 嵌套是**逻辑**的：region 的父 = 另一个 region，**或者创建它的那个块（层）** ✓
- **释放粒度 = 子树**：`reset`/`drop` 递归到子 region；**块退出 ⇒ 挂在它下面的 region 子树一起释放** ✓
  （主人原话：「region 给一个指针给 arena 指向，回收的时候顺手把 region 回收了」✓）
- ⚠️ **不做 O(1) 逐对象释放/复用** —— 这条是共识：**这不可能** ✓ 摊销的批量释放是唯一的诚实答案 ✓
- **独立块**（已定）：每个子 region 至少一块 malloc ⇒ 换来**子容器能独立 `reset`** ✓
  （`cache.reset()` 才成立 ✓；叶子容器以后可以优化成共享父块 ✓）

### 3.5 注册表：**每帧一个**（选 a），挂在块 arena 的子链上

⇒ 用户**不用传**注册表 ✓ · "块退出 ⇒ region 子树一起没"自然成立 ✓
⇒ **region 的寿命 = 创建它的那个块** ✓（没有隐藏语义：释放点仍然写在程序里 —— `reset`/`drop`/块退出 ✓）
⭐ **由此一个好处**：工厂函数不靠"提升"就能安全 —— 想要跨作用域活着的容器，**由调用者建 region
再把 handle 传进去** ✓（「跨 region 也是传 region handle」✓）⇒ **完全不用碰逃逸/提升那个娇气的 pass** ✓✓

## 4. 连续性怎么保住：`buf<T>` + 原地扩展

`varArray` / `string` 要的不是"分配"，是**"一块能长大、而且始终连续"**的内存 ⇒ region 提供
**唯一一个**高级原语：

```extc
var b = r.buf<T>(cap)      // region 里的一块连续内存
b[i] · b.len
b.grow(n)?                 // 几乎总是**原地延长**（见下）
```
**为什么几乎总是原地**：region 是**私有的、安静的**（里面只有这个容器自己的几块东西）⇒ 那块内存
**通常是 region 里最后一块** ⇒ bump 指针一推就长大 ✓ **零拷贝、零旧副本** ✓；万一不是最后一块，
就在同一 region 里再要一块并拷（有界，且不产生永久垃圾 —— region 的寿命是统一的 ✓）
⇒ 于是 **`varArray`/`string` 放进 region 依然永远连续** ✓（**不 chunk 化** ✓；只有 churn 敏感的
容器才走"列 + handle" ✓）

## 5. 契约分工：谁给 `slice`、谁给 `copyInto`/`forEach`

| | **平数组家**（单块 · 无洞 · 不跨块）| **region 容器**（dense/sparse · 列 · 有洞）|
|---|---|---|
| 零拷贝取视图 | `asSlice()` ✓ 保留 | **不给** ✗（原理上给不出：分块/有洞/会被 `reset` 挪走）|
| 拷出去 | 同上 | `count()` + **`copyInto(dest)`** ✓ 调用者给 buffer（不够 ⇒ `failure(destFull)` ✓）|
| 遍历 | 现有吃 `slice` 的算法 | **`forEach`** ✓ 零拷贝、按列遍历、可向量化（配 `@inline` ✓）|
| 改 | 自己改 | `set(h, v)` / `forEachMut`（明说：遍历期间只许改值，不许增删 ✓）|

- **两种契约用两个类型分开**（跟"读型/写型两个 struct"同一个道理 ✓），**不要**给 region 容器一个
  "看运气"的 `asSlice() -> ?slice<T>` ✗（那种 API 一定会被误用，而且误用时看起来是对的 ✗）
- **`string` 不跟着 chunked 化**：下游全吃 `slice<u8>`（print · IO · 哈希 · `extern!` 边界 ✓）⇒
  原则一句话：**「数据面用列 + handle，序列化面保持连续」** ✓
- **dense 要保住**：删除用 **swap-remove**（把末尾搬进空位）⇒ 前段永远连续 ⇒ 遍历可向量化 ✓
  `graph` 用 **CSR**（每节点一段连续边）✓ 这正是兑现"消灭指针跳转"的那个封装 ✓

## 5.5 用户会怎么用（默认路径应该是"零心智负担"的）

```extc
use std::set
var s = set<i32>::new()          /* region 在容器内部 —— 用户看不见，也不用管 ✓ */
s.insert(1)  s.insert(2)  s.insert(3)
s.erase(2)
if s.contains(2) { println("还有") }

/* 遍历：今天最自然的形状（迭代器结构体；`for`/lambda 落地后再加语法糖）*/
var it = s.iter()
while it.next() { println(it.value()) }

/* 想要 `slice`：一行拷出来，生命周期 = 当前块，用完随块回收 ✓ */
let view = s.toSlice()
println(view)

s.reset()                        /* 想手动回收时才有这一句（长命容器用）✓ */
```

**默认路径与今天的 `varArray` 一样自然** ✓：region 是容器的实现细节，用户只在
"想手动回收"（`reset`/`close`）或"写自己的 region 容器"（§6）时才见到它 ✓

**三处手感与 C++ 不同（要提前说清，避免"用起来别扭"的落差）**：

| # | 差异 | 对策 |
|---|---|---|
| 1 | **迭代**：今天没有 `for` / lambda（`forEach` 收不了函数值 ✗）| 容器提供 **`iter()`**（`while it.next()` ✓）与 **`atDense(i)`**（dense 下标循环 ✓）；`for`/lambda 落地后加糖 ✓ |
| 2 | **不能长期持有元素引用**（`ref T` 会挡住 erase 的自由 ✗）| 用 `at(h)` **瞬时**读、`set(h, v)` 写 ✓ 换来的是"**erase 永远不会让你的引用悬垂**" ✓（C++ 那边正是 UB 之源 ✓）|
| 3 | **拿不到 `slice`** | 容器直接给 **`toSlice()`**（内部拷到**当前块**的 arena ⇒ 生命周期 = 当前块、用完自动回收 ✓）· 或 `copyInto(dest)` 由调用者给 buffer ✓ |

## 6. 用户怎么写自己的 region 容器（骨架）

```extc
/* 一个"对象池"：region 由调用者给（跨作用域靠传 handle），descriptor 住在 region 里 */
struct pool<T> {
    r: region                 /* 注册表槽的 handle（值，可复制）*/
    d: handle                 /* region 里 descriptor 的 handle */

    fn withCap(r: region, n: i64) -> pool<T>    /* 建 descriptor + slots（r.buf<T>(n)）*/
    fn own(self: mut ref pool<T>, x: T) -> ?handle
    fn at(self: ref pool<T>, h: handle) -> ?ref T
    fn free(self: mut ref pool<T>, h: handle)   /* swap-remove + 槽位还给 free list */
    fn reset(self: mut ref pool<T>)             /* region::reset(self.r) ⇒ 子树级清空，块复用 */
    fn close(self: mut ref pool<T>)             /* region::drop(self.r)  ⇒ 连块一起还 */
}

/* 用法 */
var r = region::new()                            /* 建在**当前块**下 ⇒ 块退出即回收 ✓ */
var p = pool<i32>::withCap(r, 1024)
let h = p.own(42)?
...
p.reset()                                        /* 清空但复用块（churn 不涨）✓ */
```

## 7. 不解决的 / 明确的边界

1. **epoch 内部的碎片**：region 里反复 erase/insert 仍要靠 **free list / dense-swap** 那套工程 ✓
   region 只把"整批还"变成可能，不是万能 ✗
2. **完全动态、说不出来的生命周期** ⇒ 出口只有 C 互操作（交给带 malloc/free 的库）✗
   或一个真 GC（会推翻"隐藏语义为零""过期字节确定"两根柱子 ⇒ 不建议）✓
3. **"顺手回收"只在块退出时发生** ⇒ 服务级长命容器（活在永不退出的块里）**永远不会被顺手销毁** ✗
   ⇒ 那类容器**必须显式 `reset()`/`close()`** ✓
   ⇒ 一句话：**这套方案保证"不漏"，不保证"自动收缩长命数据"** ✓
4. **`slice` 对 region 容器不再适用**（主人已确认）⇒ 用 §5 的 `copyInto`/`forEach` 兜 ✓
5. **原地扩展只是优化**：它救不了服务级长命容器（那要靠 `reset`）✓
6. ✅ **`region` 的设计不允许外部引用**（对外只给 handle）—— 这是**硬规则**，不是纪律：
   用户**拿不到**指向 region 内存的引用 ⇒ 用户可见的表面上不存在「引用悬垂」✓
   ⚠️ 唯一会出现这种引用的地方是**容器方法内部**（「查表拿到 descriptor」那一下），瞬时用完即弃 ✓
   ⭐ **而且现有规则本来就算得对，不需要新检查**（2026-09-24 核对 `src/check_escape.c`）：
   「调用结果的深度 = 实参与接收者的最大深度」—— `EX_METHOD` 取 `max(receiver, args)` ✓，
   `EX_CALL` 取 `max(args)` ✓
   ⇒ 库方法 `fn at(self: ref pool<T>, h) -> ?ref T` 返回的引用，深度 = **容器自己所在的层**
   = **region 所在的层** ✓ ⇒ 想把它存到更长命的地方，**照样编不过** ✓✓ 也就是说：
   **走"容器方法"这条路取引用是安全的，一个字的编译器改动都不需要** ✓
   ⚠️ 唯一的窄口子：**自由函数**（没有任何携带容器的实参，例如只在内部包一层
   `extc_region_alloc(rid, …)`）返回这种指针 ⇒ 深度会算成 **depth 0**（"哪儿都能存"）✗
   ⇒ 这才是「库纪律 / 可选小检查」真正的适用面，而且它很窄 ✓
   （结论：把纪律变成硬约束是可选的锦上添花，不是必需 ✓）

## 8. 落地分期（每期都能独立验收）

| 期 | 做什么 | 动编译器？ |
|---|---|---|
| **0** ✅ | **已落地 2026-09-24**：`stdlib/std/pool.extc`（slot map：稳定 handle + dense 前段 + free list + 世代）· 常设验收 `tests/pool/`（basic · api · churn · canary · ASan，挂成 `check.sh` 第 18 节）| **零改动** ✓ |
| **1** | 运行时：**区域注册表**（表 + free list + 世代 + 父链 + 释放遍历，~200 行 C）· 按需发射（照 `extc_raw_enter` 那条现成的路）· 块释放点与帧退多一行 `extc_region_release(&__extc_a[lvl])`（照我为 fd 写过、后来删掉的那段，一模一样的模式）| **两处 codegen 钩子**（小，模式已有 ✓）|
| **2** | **`new (r) T[n]`**：parser 一条语法 + 检查器给类型 + codegen 发射 `extc_region_alloc(rid, bytes)` 并转成 `T*` | parser/checker/codegen（**中**）|
| **3** | `buf<T>` 的**原地扩展** · region 版 `varArray`/`string` · `graph`/CSR | 运行时 + 库 ✓ |
| **4** | 「**region 内存的引用不许出函数**」——把 §7.6 那条**库纪律**变成硬约束（**很小**；完整深度传播并不需要 ⚠️）| checker（**小**）|

⚠️ **为什么第 0 期不能分配"带类型的 region 内存"**：库拿不到这种能力 ✗ ——
`extern!` 不许返回 `slice<T>`（会变成两个 C 参数 ✗）· extC 也没有指针转换 ✗ ⇒
**要么加 `new (r) T[n]`（第 2 期），要么第 0 期先用今天的 `new T[n]`** ✓
⇒ 第 0 期照样能拿到「槽位复用 + 数据导向」，只是拿不到「批量还」✓ 我认为这样分期更诚实 ✓

⭐ **ECS 纪律消掉了最大的一块**：因为**引用不出 region**（对外只给 handle，是普通值、不携带寿命）
⇒ 检查器**不需要知道 region 的寿命** ⇒ 原本最大的那件（完整深度传播）**根本不用做** ✓✓
第 4 期只剩「把库纪律变成一条小检查」，甚至可以一直不做 ✓ 这是「参考 ECS」换来的最大工程收益 ✓

**每期判据**：`tests/run.sh` / `check.sh quick` / `memsafe` 不回归 ✓ · 新增：churn 内存**平** ·
`drop` 之后用旧 handle ⇒ `failure(staleHandle)` **带位置** ✓ · 遍历吞吐有对照数字 ✓

## 9. 待批的点

1. §3.3 的两条都请确认：① **bug ⇒ trap、条件 ⇒ 值**这条界线 ✓ ② trap 的**位置**走
   (a) 查询式 API（零编译器改动）还是 (b) `@trackCaller`（小特性，trap 指到用户那一行）？
2. §3.4 **`reset` 也递归到子树**（父 epoch 重置 ⇒ 子 region 一起重置）——同意吗？
3. §4 `buf<T>` 作为 region 的**唯一**高级原语（`new (r) T[n]` 是它的退化形态）——同意吗？
4. §5 **两种契约用两个类型分开**、`string` 保持连续——同意吗？
5. §8 分期：**先从第 0 期（纯库注册表 + handle + 校验）开工**——同意吗？
6. 跨线程：handle 转移的规则（是否要求"一个 region 同时只有一个写者"、要不要 affine 一次一个线程）
   —— 留给并发那条线，还是现在一起定？

## 变更史（3 行，供读过旧版的人对照）

- 旧版把 region 当**能力值**（不可复制、affine、`@nocopy`）⇒ 现在**不需要**：所有者是注册表/父节点，
  handle 是普通值 ✓
- 旧版写"拷贝 ⇒ double free"⇒ **撤回**（销毁由父节点的子链驱动，只销毁一次）✓
- 旧版担心"动态层 / 改层编码 / 碰逃逸 pass"⇒ ECS 版**不需要**（寿命 = 创建它的那个块 ✓）

---

## 10. 期 0 的实测（2026-09-24）

| 形状 | 1e5 轮 | 1e6 轮 | 结论 |
|---|---|---|---|
| `tests/pool/churn.extc`（insert+remove，活跃 ≤2）| **1,660 KB** | **1,724 KB** | **平** ✓（10 倍轮数，内存不变）|
| `tests/pool/churn-leak.extc`（**不** remove，canary）| 9,916 KB | **67,324 KB** | 明显涨 ⇒ **判据有牙** ✓ |

⇒ 「用户无法回收容器垃圾 / 无法复用」这两条**在零编译器改动的情况下已经解决** ✓
（剩下的第 3 条「指针跳转妨碍 SIMD」与「批量还给 arena」要期 1+）

**落地物**：`stdlib/std/pool.extc` · `tests/pool/{basic,api,churn,churn-leak}.extc` + `run.sh` ✓

## 11. 代码组织（主人 2026-09-24 的建议）

⭐ **期 1 起，动编译器的那部分单开文件**：`src/regions.c` + `src/regions.h` ——
运行时的区域注册表 · 释放钩子 · 以后的 `new (r) T[n]` 发射，都放这里 ✓
现有文件只留「一两行调用点」（`codegen.c` 里按需发射那一处 + 释放点那一行）✓
理由：region 是一个**新子系统**，散进 `check_expr.c`/`check_top.c`/`codegen.c` 会变成
"哪儿都有一点、哪儿都不完整" ✗（这个项目已经吃过一次：逃逸 pass 的几处 case）
