<!-- MEMORY-MODEL.md —— 技术展示：Arena 与 Pool 的内存管理模型（底层原理版）
     大纲（按作者指定）：1 Arena 是什么 · 2 Arena 提权机制与正确性证明 · 3 Arena 的优劣 ·
     4 Pool 是什么、为什么需要 · 5 Pool 与 Arena 如何联动 · 6 应用（无栈协程 / dyn Trait / …）
     取材：ARENA-FORMAL.md（形式模型与定理）· ARENA-SOUNDNESS.md（判据与反例）· POOLS.md ·
     CONCURRENCY.md · DYN.md · src/back/codegen.c 注释 · tests/*。数字都是实测；未修的一律写"未修"。 -->

# extC 的内存管理模型：Arena 与 Pool（原理篇）

> **一句话**：没有 GC，也没有裸 `malloc/free`。**Arena 管寿命**（区域 + 分配点，LIFO 回收），
> **Pool 管复用**（句柄 + 世代，可删可回收）。而"每个分配点该落在哪个区域"不是用户写的、
> 也不是随便挑的 —— 它是**一组约束的最小解**，由编译器解出来（这就是**提权**）。

---

# 1. Arena 是什么

## 1.1 三个概念，别混

| 概念 | 是什么 | 关键性质 |
|---|---|---|
| **块（block）** | 一块连续内存（`extc_ablock`，4096 字节起：`cap = n > 4096 ? n : 4096`）| bump 分配，O(1)，**不单独 free** |
| **地方（place / 作用域）** | 一个词法作用域；**它不是函数帧** | 组织单位是"作用域"，不是调用栈 |
| **区域（region）** | 一个**寿命**的抽象 —— **是地址的属性**，不是"一块内存" | `reg(a)` 一旦分配就定了（`ARENA-FORMAL.md` §1.4）|

**层号 = 词法层**：一个函数调用里 arena **入口建一次**、所有分配点共用，`__extc_a[]` 的
下标就是词法层号（`CONCURRENCY.md` §2.1，`src/back/codegen.c`）：

```c
static int32_t fill(int64_t n) {
    extc_arena __extc_a[3] = {0};          /* ← 入口建一次 */
    extc_arena_alloc(&__extc_a[1], ...)    /* ← 所有 new / alloc 用它 */
    extc_arena_release(&__extc_a[2]);      /* ← 块退出时释放那一格 */
}
```

⇒ 分配点**编译期定死**，运行期没有查找、没有链表遍历。

## 1.2 回收：不是"把指针 reset 回去"

`extc_arena_release` 走整条链、**保留一块**（`cap ≤ EXTC_ARENA_SPARE_MAX`，1 MB）当 spare，
其余 `free`。spare 这条规则是量出来的：**32e6 次 malloc+free 会让 churn 慢约 50 倍**。
字面意义的 mark/restore 要配预算才成立 —— `tests/arena` 把虚拟内存卡在 **150 MB** 跑
「**300 轮 × 1 MB**」并要求不许涨。

## 1.3 为什么"区域"必须是**一个**（#31 的根源）

`reg(a)` 是**地址的属性**。于是：

> 容器 `v` 的**槽位**在区域 `R_v` 里，它**里面的指针**指向区域 `ρ` 的对象；
> **拷贝 `v`**（返回、赋值、塞进别的 struct）只搬**槽位**（指针值），**不搬 `ρ` 的对象**
> ⇒ 只要 `v` 或它的副本活到某个时刻，`ρ` 就必须**也**活着。（`ARENA-FORMAL.md` §1.4）

⇒ `mk()` 里分配的 buffer 被 `return` 出去之后就是悬垂：槽位活着、buffer 的区域死了。
**这就是 #31 的全部内容**，也是第二节要解决的问题。

---

# 2. Arena 的提权机制与正确性证明

> 这一节是全文的核心：**"分配点落在哪个区域"是一个可判定、可证明的问题**，
> 提权（promotion）只是它的**解**。

## 2.1 要证的性质（一句话）

配置 `⟨σ, λ⟩`：`σ : Addr ⇀ Value`（`reg(a)` 记地址的区域），`λ : Reg → {live, dead}`。

* **安全（safety）**：一条 trace 里**每次 `deref` 都命中活区域**；
* 程序 sound ⟺ **所有 trace** 安全；
* 我们**唯一要推导的东西**：每个分配点 `S` 的区域 `α_S`。

## 2.2 两种流：地址流 vs 内容流（`ARENA-FORMAL.md` §2.1）

被调者往"家内存"（分配点 S 的区域 `H`）里存东西，值的来源只有两种：

| 名称 | 形状 | 值的区域 `ρ(val)` | 例 |
|---|---|---|---|
| **地址流 A** | 存一个**实参本身**（或字段）的地址 | `R_slot(实参)`（实参**槽位**的区域）| `n.owner = ref l` |
| **内容流 B** | 存一个**从实参里读出来的指针**（或派生）| `δ(实参)`（实参**所指对象**的区域）| `n.next = l.head` |
| （不计） | 新分配的 / `null` / 不可变标量 | `H` 自己 | `n.next = null` |

**引理 1（存储安全的两条前提）**：分配点 S 用区域 `H` 是 sound 的 ⟺ 对**每一个**可能被存进
S 的对象图的指针值 `p`，若 `reg(p) = ρ_p` 则 `H ⊒ ρ_p`（**H 活得 ≥ 被存对象的区域**）。

*证明（⇒ 的关键一步）*：设 `p` 被存进 H 里的槽位而 `H ⋢ ρ_p` ⇒ 存在 trace：`ρ_p` 先于 `H` 回收
（或二者不可比），该槽位仍可读（H 活着）⇒ 读出的 `p` 指向 dead 区域 ⇒ deref 悬垂，与 safety 矛盾。
（⇐：区域内对象与区域同生共死，`H ⊒ ρ_p` ⇒ `λ(H)=live ⟹ λ(ρ_p)=live`。）

> **推论 1a（提权的出发点就在这里）**：`H` 的下界由「**实际会流过 S 的那些值的来源**」决定，
> **而不是**由"实参自己住哪"决定。

## 2.3 约束系统：把引理 1 写成方程（`ARENA-FORMAL.md` §3）

**变量**：

| 变量 | 含义 |
|---|---|
| `α_S` | **分配点 S 的区域**（要求的解）|
| `ρ(e)` | 表达式/地方 `e` 求值出的**指针值的区域**（取"最坏的那只"）|
| `R(p)` | 地方 `p` 的**槽位区域** |
| `δ(v)` | 容器 `v` 的**对象图区域**（它里面的指针**允许**指向哪只区域 = 数据结构的 home）|
| `H_f` | 函数 `f` 的**区域形参**（生成物里的 `__extc_home`），调用点实例化 |

**约束**（每条直接来自引理 1 或"拷贝只搬指针"）：

```
(C1 存储)        p.f := e          ⟹  ρ(e) ⊒ R(p.f)
(C2 容器不变式)  x : T 含引用       ⟹  δ(x) ⊒ R(x)          /* 图里的对象要活过槽位 */
(C3 拷贝/返回)   x := y / return y ⟹  δ(y) ⊒ R(x) / δ(y) ⊒ R(dst)
(C4 分配)        S = new …         ⟹  α_S ⊒ δ(所有 S 的对象可能流入的容器)
(C5 调用实例化)  g(a⃗)              ⟹  用 g 的效果摘要实例化 H_g
```

**函数效果摘要**（§3.4，这套东西的心脏）—— 它回答的是「**`g` 往它的 `mut ref` 实参里
存了哪些区域的东西**」，而不是「实参住哪」：

```
Fresh(f,i): f 分配在 H_f 并最终存进 i 所指容器     → H_f ⊒ H_f（恒真）
Addr(f,i) : f 把「实参 j 的地址/字段地址」存进去    → 约束 R_j ⊒ H_f
Cont(f,i) : f 把「从实参 j 读出的指针」存进去       → 约束 ρ_j ⊒ H_f
```

⇒ **`Addr` 只在真有地址流时非空** ⇒ `push` 的 `Addr(push,0) = ∅`
⇒ 那条把 `H` 钉死在调用者帧上的约束**消失了** —— 这正是旧规则"过强"的地方。

## 2.4 定理 2：解存在且唯一 ⇒ **不需要用户注解**（提权是自动的）

> 约束都是 `X ⊒ ⨆ Yᵢ` 的形状（单调，join 只出现在右侧）⇒ 在**有限**区域格上由
> **Knaster–Tarski** 得**最小解存在且唯一**，可从 `X := ⊥`（最深、最短命的区域）出发
> 迭代到不动点；格有顶（`ρ_top` = 程序级区域）⇒ **解一定存在** ⇒
> **任何程序都不需要"用户显式指定 arena"**。

**推论 3.1（提权的语义，务必记住）**：最小解 = **满足全部约束的最短命选择** ⇒

* 循环里的临时容器**仍然落在块区域**（紧致性**自动**保住，不用打补丁）；
* **真正会逃出去的容器才被"提"到家的区域**（`ARENA_HOME`）。

⇒ **所谓"提权"不是一条额外规则，而是"取最小解"的副产品**：编译器先假设所有分配都最短命，
只在约束逼着它的时候才往上提，一次提够。

## 2.5 定理 3：soundness（证明骨架）

> **若程序对某个解 `α` 满足 §2.3 的全部约束，且实现按 `α` 分配，则所有 trace 安全。**

*证明（对 trace 归纳）*，不变量 **I**：

> **I**：任意时刻，对内存里任意活着的槽位 `p`（区域 `R(p)`）与它持有的指针 → 对象 `o`
> （区域 `ρ_o`）：`ρ_o ⊒ R(p)` 且 `λ(R(p)) = live ⟹ λ(ρ_o) = live`。

* **分配**：新对象在 `α_S ⊒ δ(…)` ⇒ 由 C1/C2 满足 I；
* **存储** `p.f := e`：由 C1 得新边满足 I，其余边由归纳假设；
* **容器不变式**：由 C2/C3（`δ(y) ⊒ R(x)`）与 I 得 `ρ ⊒ δ(y) ⊒ R(x)`；
* **返回 / 跨帧传递**：同 C3（返回值落点 `dst`），由**调用点实例化** `H_g` 保证；
* **回收**：区域按块 LIFO 回收；由 `⊒` 的传递性得 `λ(R)=live ⟹ λ(ρ)=live`；
* **deref**：命中的地址区域 `ρ` 与某活槽位同链 ⇒ 由 I 得 `ρ` live ⇒ **安全** ∎

## 2.6 提权的两个实例（一个放行、一个拦下）

**① "造完返回容器" —— 现在可以**证明**它安全**（旧规则甲′ 会拒绝它）：

```extc
fn mk() -> varArray<i32> {
    var v: varArray<i32> = varArray<i32>::withCap(0)
    v.push(42)
    return v
}
```

| 约束 | 来源 | 结论 |
|---|---|---|
| `Addr(push,0) = ∅` | §2.3：`push` 只存 `l.head`（内容流）与 `new`（Fresh）| 没有把 `H_push` 钉在 `R_v` 上 |
| `Cont(push,0) = {δ(l)}` | `n.next = l.head` | 需 `δ(l) ⊒ H_push` |
| `δ(v) = H_push`（归纳）| C4：`push` 分配进 `H_push`，而 `l` 就是 `v` | 代入得 `H_push ⊒ H_push` **恒真** |
| `H_push ⊒ R(caller 的落点)` | C3 + `mk()` 返回值落点 | 调用点把 `H_push` 与 `H_mk` 一起实例化 |

⇒ **解存在**（取 `H_push = H_mk = 调用者的家`）⇒ 由定理 3 **安全**。
旧规则拒绝它，**不是因为它不安全**，而是因为把 `Addr` 当成了非空。

**② 真 unsafe 的 `sneaky` —— 该拦就拦**：

```extc
fn sneaky(l: mut ref list) { var n: mut ref node = new node  n.owner = ref l }
```

`Addr(sneaky,0) = {R_slot(l)}` ⇒ 约束 `R_slot(l) ⊒ H_sneaky`：

* 调用点想让结果**逃逸**（`H = 调用者的家 > 调用者帧 = R_slot(l)`）⇒ **无解 ⇒ 编译错误**；
* 调用点只想**留在本帧**（`H = 调用者的帧`）⇒ **有解 ⇒ 放行**。

⇒ 这就是提权的行为特征：**该拦的拦、能放的放**（旧规则是"一律拦"）。

## 2.7 谁算这个解：层号的唯一权威（定案 68）

定理 3 把"谁算出 `α`"当实现细节 —— `ARENA-FORMAL.md` §10 把它钉死：
**`α` 由检查器算，codegen 只翻译**。

统一之前，同一件事**两个地方各算一遍**（检查器算 `Expr.arenaLevel`；codegen 又来一句
`if (hasHome) 用家 arena`），根因是**摘要/闭包跑得比查体晚**：

```
needsHome 的传递闭包  ⇐ 需要所有函数体都查完
检查器算层号          ⇐ 在查体当中（那时闭包还没跑）
```

⇒ 检查器给出的层号**偏深（块层）**；不一致时 codegen 的"家"更长命 ⇒ **安全**（所以没有洞），
但"安全"不等于"对"：那是**靠一句保守兜底掩盖了"同一件事算了两遍"**。
统一做法：闭包之后检查器**再定一次案**（`FuncDef.arenaSites`，`ARENA_HOME = -1` 表示"进家"），
codegen 的 `arenaRefAt`/`homeArg` 只做文本翻译。
**验收（最有说服力的一条）**：删掉 codegen 兜底后 **golden 88 个文件逐字节相同**。

## 2.7′ 提权在生成物里长什么样（机械证据）

把 §2.6 的 `mk()` 真编一遍，生成物里"家"是一个**形参**：

```c
static void      varArray_i32_push(varArray_i32 *self, int32_t v, extc_arena *__extc_home);
static void      varArray_i32_grow(varArray_i32 *self, extc_arena *__extc_home);
static varArray_i32 mk(extc_arena *__extc_home);
...
varArray_i32 v = (varArray_i32){ .buf = (slice_i32){
        .data = (int32_t *)extc_arena_alloc(&(*__extc_home), ...) } };   /* 分配落在家 */
```

⇒ **`H_f` 是调用点传进来的**（正是约束 C5"调用点实例化"）⇒ 同一个 `push` 在**不同调用点拿到不同的家**：

* `mk()` 的调用点传 **main 的家** ⇒ 返回出去的那个容器**活着**（这就是"提权"）；
* `main` 里临时用的 `wall` 传 **main 的块** ⇒ 出块即回收（这就是推论 3.1 说的"最短命"）。

⇒ 提权不是隐藏的魔法：它是**一个被解出来的形参**。用户既不写它，也无法写错它。

## 2.8 这条机制的补丁史（都是同一族）

| 洞/缺口 | 形态 | 结局 |
|---|---|---|
| `#31` / A3 | 槽位与 buffer 分属两区域 ⇒ `return` 后 buffer 悬垂 | **修**（甲′：`raiseMutRefTargets` + 惰性 `funcAllocates`）|
| 调用点求解 | "借来的值不可外存"这条**必然误拒**一整族正常写法 | **落地**（`ARENA-FORMAL.md` §9，2026-09-22）|
| `#1` | 枚举载荷逃逸检查有洞 ⇒ 悬垂/UB | **修**（2026-09-20）|
| 族 E | `dyn` 载荷的引用绕过作用域规则 | **修** |
| `H2` | home zone 传递闭包**只做了一层** ⇒ 跨两层调用**静默改坏**（`keys[0]` 从 `"k"` 变 `"a"`）| **修**（`f3b0c13`）+ 2026-09-28 复核 |

---

# 3. Arena 的优劣

## 3.1 优

| 项 | 数字/事实 |
|---|---|
| 分配 | **~0.5 ns/次**（bump；对照 pool **26 ns**，`POOLS.md` §2.2）|
| 释放 | **不做逐个 free**：块退出整块回收（LIFO，O(1) 摊销）|
| 内存 | 块内 padding + 一块 spare（≤1 MB）；对照 pool 在小对象上放大 **2.9×** |
| 遍历 | 连续内存 ⇒ 对 SIMD 友好（"块反复 Bump"正是要保的东西）|
| 用户负担 | **零注解**（定理 2：解总存在）；连"该不该进家"都是自动的 |
| 失败模式 | 不安全就**编译期报错**，不是运行期踩雷 |

## 3.2 劣（诚实清单）

| 项 | 事实 |
|---|---|
| **长命层无界** | 分配点落在长命层就永不回收：每轮新建/重建、12 轮 ⇒ **RSS 14,184 KB 线性涨** |
| **不能逐个释放** | `#29` `varArray` 分块化 · `#30` 运行时原地扩：**待做** |
| **不能真收缩** | arena 里的 `new T[n]` 只能等地方结束（要真收缩得用 pool 的独立板块）|
| **四处故意的近似** | ① 深度**线性化**（寿命本是树，压成一个数 `depth`）② **路径不敏感**（只认绑定名，不认 `p.next.f`）③ **值不敏感**（不看 `cap` 够不够）④ **无 free** ⇒ 代价逐条记在 `ARENA-FORMAL.md` §5.1 |
| **误拒** | 命题 7.1：单调放大**永远安全** ⇒ **误拒不是 soundness 逼出来的**（§7.2）；真正的硬门槛是 §7.4：**没有别名信息 ⇒ 只能用弱更新** |
| **两个权威的历史包袱** | 曾"同一件事算两遍"，靠 codegen 保守兜底 ⇒ 已统一（定案 68）|

---

# 4. Pool 是什么，为什么需要它

## 4.1 一句话

> region 是一种基于 ECS/Handle 的内存管理模式，旨在高效利用内存、保证内存安全与 SIMD 优化，
> 同时允许用户手动介入 GC —— 用来弥补 **arena 在"删增动态度很大"的数据结构上的劣势**。
> （作者原话，`POOLS.md` §1）

它治三个具体病：① **无法回收容器垃圾**（erase 的槽、旧代缓冲留在 arena 里）；
② **无法复用**（反复 add/erase 线性涨）；③ **指针跳转**（链式/树式结构每次访问一次 cache miss，
SIMD 难优化）。

## 4.2 动机是量出来的（峰值 RSS，`/usr/bin/time -f "%M"`）

| 形状 | 峰值 RSS | 结论 |
|---|---|---|
| 翻倍长到 1e5，每轮在**会退出的块**里做（12 轮）| 2,640 KB | 平 |
| 一个数组翻倍长到 1e6（4 MB 活跃）| 9,900 KB | 约 2.4×（历代旧副本全留着）|
| 每轮新建/重建、分配点落在**长命层**（12 轮）| **14,184 KB** | 随轮数**线性涨** |
| 一次要够 | 5,676 KB | 只差进程基线 |

⇒ 服务型程序（跨请求状态、缓存、会话表）**必然**撞在上面 —— 这就是 pool 存在的理由。

## 4.3 反面也量过（别再走）

「**arena 全部改成 pool**」：同一形状（2e7 个 16 字节小对象）
**arena 0.01 s / 1,452 KB** vs **pool 0.52 s / 939 MB** ⇒ 每次分配 **0.5 ns 对 26 ns**（约 50 倍）、
占用 **2.9 倍**（块头 + malloc 头 + 对齐），而且语言层没有 `free`，用户调不了买来的那个能力。

## 4.4 Pool 的形状（ECS 三层）

```
zone（一个地方的索引头，独立旁链；作者："这样子你才能够枚举 zone，否则块一回退不就全爆了"）
  └── pool 树（容器套容器：逻辑父子）
        └── pool（单个容器的那一份：数据 + free list + 世代 + 父链）
```

* **句柄 = `(slot, gen)`**：稳定身份，旧句柄**必失效**；
* **没有"帧"**：组织单位是"一个地方"；pool **持有自己的内存**（独立板块）；
* 嵌套只是**逻辑**父子（实际都统一在同一个 arena 的寿命下）。

## 4.5 什么能进池、什么不能（三条，从作者一句话推出来）

> 你不能把块也生成 Handle 放到 Pool 里面，否则块反复 Bump 会导致 Pool 反复扩容，
> 那块的 Bump O(1) 的优势又没了。

1. **进池的必须是「等大 + 需要身份/复用」的元素**（值、节点、槽）；
2. **块不进池**（变长、无身份、只需 LIFO ⇒ 走链）；
3. **两层只共享寿命所有者，不共享数据结构**。

另有一条**结构上根本接不通**：块分配器活在生成器发的 C 运行期（`extc_ablock` + `malloc/free`
+ `char data[1]`），池库活在 extC 层 —— 后者拿不到 `void*`、没有指针转换、也不能把裸块当值。

---

# 5. Pool 与 Arena 如何联动

## 5.1 分工与共享

| | arena | pool |
|---|---|---|
| 管什么 | **寿命**（哪个地方、什么时候没）| **复用**（删了能不能再放、旧句柄怎么失效）|
| 内存来源 | 自己的块链 | **自己的板块**（不是借 arena 的切片）|
| 谁回收 | 块退出，LIFO | 池自己：`remove`/`clear`/`release`，**可真收缩** |
| 联动点 | 池的**生命周期**交给 arena 一起管（地方退出 ⇒ 池树一起走）| |

作者口径：**「pool 的内存是独立的一个板块，只是生命周期给 arena 一起管理。」**
⇒ 所以 `shrink`/`release` 能立刻还内存，而 arena 里的 `new T[n]` 只能等地方结束。

## 5.2 失效的两层机制（联动里最精巧的一处）

作者原始说法：

> 我在一个 while 里创建一堆 pool，然后给 zone 一个 color，每个元素也记一个 color，
> 每次检查元素的 color 和 zone 的 color 是否匹配；不匹配说明全死了，那就**全刷**，很爽。
> zone 本身固定、不会被移动、也不需要扩容。

落地时被钉成**两层**（只做一层都会漏，两种漏法都实测过）：

* **池级**：`pool.epoch` 对 `zones[pool.zone].color` —— 管「**整个 place 死了**」；
  代价是**一次固定加一次比较**，零走查、零重写；
* **槽级**：`handle.gen` 对 `slot.gen` —— 管 **epoch 内部**的槽位复用
  （只比色 ⇒ 同 epoch 内复用会"重新对上"，静默读到新元素；只比 gen ⇒ 跨 epoch 会漏）。

⇒ 句柄本身只要 `(slot, gen)`，池级那层比的是池对象自己的 epoch，不必往句柄里再加字段。

## 5.3 联动的机械判据（实测）

| 判据 | 数字 | 含义 |
|---|---|---|
| `pool` churn | 1e5 → 1e6 轮：1,660 KB → **1,724 KB** | 平的（元素等大 ⇒ free slot 复用率极高、几乎无碎片）|
| 不留槽位的对照（canary）| 9,916 → **67,324 KB** | 判据**有牙**：不回收就会涨 |
| 期 1 注册表 churn | 20 万次后 `live=0 cap=64 gen=0`（`tests/pool/run.sh` 的 `rt_churn`）| 容量停在高水位 ⇒ 内存平的机械判据 |
| 哈希墓碑 churn | 1e5 → 1e6：**1,660 → 1,660 KB** | 墓碑复用有效 |
| `map` 墓碑 canary | 8,380 → 55,036 KB | 同上 |

> **"容量停在高水位"不是缺陷**：这是"复用优先"的代价；要真收缩就用 `shrink`/`release`
> （池有独立板块才做得到）。

---

# 6. 应用：它们各自怎么长在这套模型上

## 6.1 无栈协程：**任务 Arena**

**前提是无栈**（`CONCURRENCY.md` §1：这一节不成立则全部作废）：
**帧是一个普通 extC 值** —— `yield` = 存 pc 后 `return`，`resume` 从 pc 继续。
于是协程不需要单独的栈、不需要复制栈、也**不引入第二套生命周期**。

**三件事天生对齐**（§2.1）：

1. arena 的分配点是**词法层**、一个调用入口建一次；
2. ⇒ **协程帧就是"某一层的一次分配"**，它跟具体某个块同寿；
3. 帧的寿命按「**往上追溯**」定（作者 2026-09-23 的答案）⇒ 提权机制**不用为协程改一行**。

**唯一要守的规则**（否则**静默错**）：

> **`resume` 必须把「帧自己那只 arena」当 home 传下去**，而不是"恢复者的当前块"。

不然：调度器在一个**每轮退出**的块里 `resume`，协程内部 `new` 的状态就落在那个块里 ⇒ 下一轮悬垂。
由此得到要写进手册的**使用规矩**：

> **协程里 `new` 的东西活到协程结束，不是活到这次 resume。**
> 想要"这一次用完就扔"的临时数据，就在**调度器那一侧**分配。

**驱动复用已有协议，零额外机制**：

```extc
fn counter(n: i64) -> coroutine<i64> {      /* 返回类型写 coroutine<T> ⇒ 体内允许 yield */
    var i: i64 = 0                           /* 普通局部：跨挂起点活着 ⇒ 按值进帧 */
    while i < n {
        yield i                              /* `yield e` 是**语句**（v1 不是表达式） */
        i = i + 1
    }
}
let c: coroutine<i64> = counter(i64(3))      /* spawn = **就是调用**：帧生在这个调用点 */
for x in c { … }                             /* ① 复用迭代器协议（推荐） */
while c.next() { … c.value() … }             /* ② 显式：与用户迭代器同一套名字 */
```

**两个实测纠正**（编这两个例子时发现的，与 `CONCURRENCY.md` §4.4 的示例有出入）：

* **`for x in c` 今天不行** ✗：检查器报 `` no method `iter` on `coroutine<i64>` `` ⇒ 实际驱动是
  `while c.next() { … c.value() … }`（§4.4 里"① 复用迭代器协议（推荐）"那条与实现不一致）；
* **`send` 已经存在** ✓：同一个报错把方法集列了出来 —— `` methods of coroutine: next value send ``；
  所以缺的不是"往协程里送值"这个能力本身，而是**带类型的请求参数**（`coroutine<A,B>` 的第二参）。

**实测与欠账**：`tests/coro` **27/27**（调度器 · `epoll` · echo server · 帧布局 · 协程池 · 句柄 ·
状态机 · 迭代器协议）。欠账：**`coroutine<A,B>`（请求类型/send-in）未做**（实测
`` `coroutine` expects 1 type argument(s), got 2 ``）；`parallel`+channel 未做
（文档口径"并行是最后一件事"）；`blkSize` 需能显式指定；`POOLS.md` 期 2/3 未做。

## 6.2 dyn Trait：**Pool 保证安全**

```extc
trait Tag { fn tag(self: ref Self) -> i64 }
struct box { v: i64 }
impl Tag for box { fn tag(self: ref box) -> i64 { return self.v } }

dyn Tag(b).tag()          /* 阶段 1：构造 + 立即调用（不碰池） */
```

* **`dyn Tag` 是一个值类型**，载荷是**一份拷贝**（放进 dyn 池的槽里）
  ⇒ **不存在"dyn 值指向栈上临时量"的寿命问题** —— 与"引用不能活过它的帧"完全同构；
* 构造写成 `dyn Tag(x)`（与既有 `T(x)` 同形）；**object safety**：泛型方法、返回 `Self`、
  无 `self` 的关联函数**不能进表**，且在**使用点**报错（不是等到派发时才炸）。

**五条必须做对的事**（`DYN.md` §4）—— 它们全是 **pool 的能力**：

| # | 事 | 今天的机制 |
|---|---|---|
| 1 | dyn 用的池**只追加** | `extc_pool_new_table` + `extc_pool_kind`；`give`/`resize`/`_raw` 三处守卫 |
| 2 | 删除 = **墓碑** | 槽表里 `0` 表示空闲 ⇒ 天然墓碑 |
| 3 | 派发前**校验 `gen`** | **已落地**：`extc_dyn_slot` 的五段校验链 |
| 4 | 整池作废只需一次比较 | `extc_pool_reset` 的 `generation++` |
| 5 | 深度上界/效应声明只写在**被动态跨越的边界**上 | 静态部分照旧 |

⇒ **`dyn` 的安全性直接建立在 pool 的世代机制上**，没有另起一套运行期。
常设判据：`tests/dyn/`（含 `dyn_promoted_two_hops.extc`：**跨两跳返回的 dyn 值同样被提升** ——
即提权机制与 pool 的载荷拷贝**同时**起作用）、`tests/arena-promoted/`。
`DYN.md` §10 记着"卸载/重载**不做但留门**"及其不变量（**表指针只在槽里**）。

## 6.3 还有哪些应用面（同一套模型）

| 场景 | 用哪一层 | 说明 |
|---|---|---|
| STL 连续容器（`vector`/`string`/`varArray`）| arena + pool | 连续块走 arena、增长走池板块（期 2/3 落地中）|
| 频繁增删的容器（`pool`/`map`/`set`/`graph`）| pool | 元素等大 ⇒ free slot 复用率极高 |
| 随机 key→value | 哈希表（开放寻址 + 墓碑）| 墓碑 churn 1,660 → 1,660 KB（实测平）|
| 跨请求的长命状态 | **显式 pool**（不要长命层裸 arena 分配）| 否则就是 §4.2 那 14,184 KB |
| AST / 解析中间产物 | arena | 生命周期与"一次编译"同寿，零回收成本 |
| `map` 的手搓 freeList | arena（现状）| 原因与两条待裁决路线见 `PLAN.md` 的 `#84` |

---

## 6.4 时序图 ①：提权 —— 一次调用里"家"怎么往下传

例子（就是 §2.6 的 `mk()`，实编过，运行退出码 2）：

```extc
fn mk() -> varArray<i32> {
    var v: varArray<i32> = varArray<i32>::withCap(0)
    v.push(i32(42))
    return v
}
fn main() -> i32 {
    var w:   varArray<i32> = mk()                                  /* w 要活到 main 结束 */
    var wall: varArray<i32> = varArray<i32>::withCap(0)            /* 本帧用完即弃 */
    wall.push(i32(7))
    return i32(w.len + wall.len)
}
```

```
main                          mk(H_mk = main 的家)          withCap/push(H)          extc_arena_alloc
 │                                    │                          │                        │
 │─ mk(main 的家) ───────────────────▶│                          │                        │
 │                                    │─ withCap(H_mk) ─────────▶│                        │
 │                                    │                          │─ alloc(&H_mk) ────────▶│  ← 缓冲区落在「家」
 │                                    │─ v.push(42, H_mk) ──────▶│                        │
 │                                    │                          │─ grow → alloc(&H_mk) ─▶│  ← 扩容也落在家
 │◀── 返回 v（缓冲区在家 ⇒ 活着）─────│                          │                        │
 │                                                                                        │
 │─ wall = withCap(main 的块) ────────────────────────────────▶ alloc(&块) ──────────────▶│  ← 最小解：能短就短
 │   （出块 ⇒ 整块回收）                                                                   │
```

**校验点**：① `Addr(push,0)=∅` ⇒ 没有约束把家钉在 `mk` 的帧上（§2.3）；② 调用点实例化 `H_mk`（C5）
⇒ 同一个 `push` 在两处拿到不同的家；③ 生成物里可见 `__extc_home` 形参（§2.7′）。

## 6.5 时序图 ②：无栈协程 —— **任务 Arena**

例子（实编、实跑，输出 `0 10 20`）：

```extc
use std::io
fn worker(n: i64) -> coroutine<i64> {
  var i: i64 = 0
  while i < n {
    var box: mut ref i64 = new i64      /* ← `new` 落在**帧自己的 arena** */
    *box = i * i64(10)
    yield *box
    i = i + 1
  }
}
fn main() -> i32 {
  var c: coroutine<i64> = worker(i64(3))          /* spawn = 就是调用 */
  while c.next() { io::cout << c.value() << "\n" }   /* 驱动 */
  return 0
}
```

```
main                      worker(=$spawn)        extc_taskArena[id]           runtime 校验
 │                              │                       │                        │
 │─ 调用 worker(3) ────────────▶│                       │                        │
 │                              │─ zoneEnter() ─────────────────────────────────▶│ 进入任务自己的 place
 │                              │  （帧记住 zone：`extc_taskZone[id] = zone`）   │
 │                              │─ new i64 ────────────▶│ extc_task_alloc(id,…)  │  ← **不走恢复者的块**
 │                              │─ yield *box（存 pc、return）                    │
 │◀── 控制权回到 main ──────────│                       │                        │
 │─ c.next() ──────────────────▶│ extc_task_alive(id) ──────────────────────────▶│ 句柄还活着吗？
 │                              │─ 从帧里的 zone 继续（**不重新推导**，rule 1）   │
 │                              │─ 再 new ⇒ 仍进 taskArena[id]                   │
 │   … 循环 3 次 …              │                       │                        │
 │─ 结束 ⇒ extc_task_end(id) ──▶│ extc_arena_release(&extc_taskArena[id]) ──────▶│ **一次性**回收整个任务
 │                              │ extc_pool_zoneLeaveTo(zone) ─────────────────▶│ 离开任务 place（zone 色翻位）
```

**三处校验**（都能跑出来）：

1. **编译期**：跨 `yield` 活着的局部**不可以指向别人的存储** —— 我第一版就踩到了，诊断是
   `` `box` lives across a `yield` and carries a reference ``，note 给出两个修法
   （**在体内 `new`** ⇒ 落进帧自己的 arena；或"只保偏移、resume 后再取视图"）；
2. **运行期（驱动前）**：`extc_task_alive(id)`；过期句柄**必须大声 trap** ——
   `tests/coro/coro_copy_expired.extc` 实测：**退出码 70** +
   `…coro_copy_expired.extc:15: trap: driving a coroutine whose task has already ended (its frame was released with the task)`
   （**带源码位置**，不是 use-after-free）；
3. **静态守卫**：`tools/check_concurrency_guards.py` 强制"`$step` 里**不许出现** `extc_zoneTop`"
   （即 step 只能读帧里记着的 zone，不能重新推导 —— 这就是 §2.5 那条"必须传帧自己的 arena"的机械保证）。

⇒ **"任务 Arena"就是把每一个任务变成 arena 的一个实例**：帧、帧里的 `new`、容器板块全挂在
`extc_taskArena[id]` 上，任务结束**一次性**回收（`extc_arena_release` + `zoneLeaveTo`）。

## 6.6 时序图 ③：dyn 的一次派发与**六段校验**

例子（实编、实跑，输出 `7 10`）：

```extc
trait Tag { fn tag(self: ref Self) -> i64 }
struct box   { v: i64 }   impl Tag for box   { fn tag(self: ref box)   -> i64 { return self.v } }
struct other { w: i64 }   impl Tag for other { fn tag(self: ref other) -> i64 { return self.w * i64(2) } }

fn pick(k: i64) -> dyn Tag {
    if k == 0 { return dyn Tag(box   { v: 7 }) }
    return dyn Tag(other { w: 5 })
}
fn main() -> i32 {
    var d: dyn Tag = pick(i64(0))    io::cout << d.tag() << "\n"    /* 7  */
    var e: dyn Tag = pick(i64(1))    io::cout << e.tag() << "\n"    /* 10 */
    return 0
}
```

```
pick / main                    extc_dyn_put（装盒）            extc_dyn_slot（**派发前校验**）      虚表
 │                                   │                                 │                        │
 │─ dyn Tag(box{7}) ────────────────▶│ 载荷**拷贝**进 dyn 池的槽        │                        │
 │                                   │ 槽表 += {pid, gen}              │                        │
 │◀── 句柄 {slot, pid, gen, pgen} ───│                                 │                        │
 │─ d.tag() ────────────────────────────────────────────────────────▶│                        │
 │                                   │                                 │─ 六段校验（下表）──────▶│
 │                                   │                                 │─ 取表项 ⇒ 调用 ───────▶│
```

生成物里的**六段校验**（`extc_dyn_slot`，逐字抄自编译输出）：

```c
if (h.slot < 0 || h.slot >= extc_dynN)      trap "stale `dyn` value";                        /* 1 索引在界内   */
if (s->pid < 0)                             trap "stale `dyn` value (slot recycled)";        /* 2 槽还占着     */
if (s->pid != h.pid)                        trap "stale `dyn` value";                        /* 3 池对上       */
if (s->gen != h.gen)                        trap "stale `dyn` value (slot recycled)";        /* 4 **槽级**世代 */
if (extc_pool_kind(h.pid) != 1)             trap "stale `dyn` value";                        /* 5 池还是 dyn 的*/
if (extc_pool_generation(h.pid) != h.pgen)  trap "stale `dyn` value (its pool was reset, or its place is gone)";
                                                                                             /* 6 **池级**世代 */
```

⇒ 第 6 条就是 §5.2 那两层失效的落点：**"池被 reset，或者它那个 place 没了"** ——
所以一个 `dyn` 值**不可能**悄悄指向已经作废的载荷；错了就是**带源码位置的 trap**。
配合 §6.5 第 2 条的实测（退出码 70 + 位置），这套校验是**可演示**的，不是纸面承诺。

# 7. 一页速查

**三条规则**：① 分配点落在哪，由**最小解**定（提权自动）② 池有独立板块，寿命跟着地方
③ 块不进池、池不放块，两层只共享寿命。

**三条命令**：

```bash
./check.sh                                     # 完整 43 节（语义/警告/反例/套件/基准）
EXTC=$PWD/build/extc ./tests/coro/run.sh       # 协程 27 项
EXTC=$PWD/build/extc ./tests/dyn/run.sh        # dyn（表进池 + 世代校验，22 项）
```

**证据索引**：

| 结论 | 出处 |
|---|---|
| 区域是地址属性 · 两种流 · 引理 1 · 约束系统 · 定理 2/3 · 四处近似 · 定案 68 | `docs/topics/ARENA-FORMAL.md` §1–§10 |
| 检查器实际判据 · 三类"记小了" · 7 个反例 | `docs/topics/ARENA-SOUNDNESS.md` |
| pool 模型 · 边界三条 · arena-vs-pool 实测 · 染色两层 | `docs/topics/POOLS.md` §1/§2/§7 |
| 无栈 · 帧寿命 · `resume` 的 home 规则 · `coroutine<A,B>` 欠账 | `docs/topics/CONCURRENCY.md` §1/§2/§4 |
| dyn 值形状 · object safety · 五条义务 · 留门 | `docs/topics/DYN.md` §2/§4/§10 |
| 生成物形状（`__extc_a[]`、层号、spare 块）| `src/back/codegen.c` |
| 反例与判据 | `tests/arena-soundness/`、`tests/arena-promoted/`、`tests/pool/`、`tests/pool-soundness/`、`tests/coro/`、`tests/dyn/`、`tests/asan/`、`tests/attacks/`、`tests/nocopy/` |
