# 开发记录

> **规矩：每完成一块就追加一条。** 记的是「做了什么、为什么、发现了什么」——
> 不是 diff（那个 git 里有），而是**决策和发现的来龙去脉**。
>
> 倒序，最新的在最上面。

---

## 2026-09-18 · **`match` 落地（第一刀：无载荷枚举）** —— 主人问「是不是可以落地了」

主人的原话是：**「match 是不是可以落地了，因为我有个想法，就是 result 和 option 或许可以用 match 重写？」**

**这个想法比"更雅致"值钱得多** —— 我实测确认它撞上一个硬限制：

```extc
fn pick(b: bool) -> option<slice<u8>> { ... }
// error: ‘__extc_reference_has_no_zero_value__’ undeclared
```

`option` 现在是 `struct { has: bool, value: T }` —— **`none` 的时候那个 `value` 字段必须填点什么**，
而 `slice<u8>` 里含 `ref`，**ref 没有零值**，编译器只好塞个毒标记让 gcc 报错
（而且报在 **prelude 里**，用户看不到自己的代码）。
**用 tagged union，`none` 里根本没有 `value` 字段 ⇒ 限制从构造上消失** ✓
而这正是 IO 要返回的东西（`option<slice<u8>>` / `result<slice<u8>, ioError>`）。

### 于是切三刀，这次只做第一刀

| 刀 | 做什么 | 状态 |
|---|---|---|
| **1** | `match` 用在**今天的无载荷枚举**上 | ✅ **做完了** |
| 2 | 枚举**带载荷**（tagged union + 泛型枚举实例化）| ⬜ 下一刀 |
| 3 | **重写 prelude 的 `option`/`result`** + `?` 跟上 + 迁 examples | ⬜ |

**第一刀为什么这么切**：穷尽检查跟载荷**无关**，而无载荷**不需要 tagged union** ——
生成的 C 就是一个干净的 `switch`。**风险最小，而且语法是第二刀的超集，不返工** ✓

### 顺手发现的一个解析坑

`match e { ... }` 里的 `{` 会被当成**结构体字面量**（`e { 字段: 值 }`）⇒ 报 `expected ':'`。
解法是 `if` / `while` 已经有的那招：**把 `inCond` 打开**（条件里不许有结构体字面量）✓

### 它为什么对 extC 特别重要

`match` + 带载荷枚举是**函数指针/动态派发的替代品**（设计里"闭集事件用枚举"全靠它），
而且比 C 的函数指针表强在**穷尽检查**；也是自举里 AST 的形状 ✓

---

## 2026-09-18 · **`return failure(e)` 能裸写了** —— 因为主人说「result 很神秘，我不太会用」

做 IO 设计的时候主人提了一句：**「我感觉这个 result 很神秘，我现在自己其实不太会用」**。
这句话比任何性能问题都值得先处理 —— **一个安全机制如果用户不会用，那它就不是机制，是障碍。**

### 两层意思，两个不同的动作

| 主人说的 | 该怎么治 |
|---|---|
| 「**不太会用**」= 不知道该在什么时候写什么 | **补能抄的代码**：`MANUAL.md` 加「一页速查」，新写 `examples/result-usage.extc`（造 / 用 / 抛，各三行）|
| 「**很神秘**」= 写起来太啰嗦，看不出重点 | **改语法**：`return` 位置裸写 `success` / `failure` / `some` / `none` |

原来必须写：

```extc
return result<unit, gameError>::failure(gameError.occupied)
```

那串类型长到把重点盖住了 —— **而重点从来是「失败」本身**。`?` 负责收，`failure` 负责发，
两头都不该啰嗦。现在：

```extc
return failure(gameError.occupied)
```

### 为什么这不算「自动推导」

主人的口径是**「显式是对的，不要那么多自动推导」**，所以这一条我特意论证过边界：
**编译器没有猜任何东西** —— 它读的是你自己写的那一行 `-> result<unit, gameError>`。
省掉的纯粹是**重复劳动**。

> 跟定案 27（关联函数写全类型）不冲突：**那条管的是没有上下文的地方**，
> 这里上下文是**显式写出来的签名**。空的地方要显式，写过的不用再写一遍 ✓

### 实现：原地改写成 `EX_ASSOC`（零新机制）

`desugarBareCtor` 只做一件事：把这个 `EX_CALL` 节点改成 `EX_ASSOC`，填上
`typeName` / `targs` / `name` / `args` —— **后面的检查、逃逸、生成一条都不用动** ✓

⚠️ 一个 C 的坑：`Expr.u` 是 union，**得先把 `args` 拿出来再改 `kind`**，否则会被自己的新字段覆盖。

**限制收得很紧**（不扩权）：只有那四个名字、只有 `return` 位置、只有返回类型正好是
对应容器时、而且用户自己定义了同名函数时不认 ✓

---

## 2026-09-18 · **`let` 是命名，不是存储** —— BOOTSTRAP §8 第 2 步（104 测试全绿）

主人一开始就是这么说的：**「本质是在里面重新覆盖 a 的含义，而不是写入」**。
代码现在照着这句话长 —— 同一层可以再 `let` 一次同名。

### 想清楚的那一步：这句话到底承诺了什么

「重新覆盖含义」不是「把新值写进老地方」，而是**造一个新的绑定**。
所以老的那个**还在**，只是没名字了 —— **谁指着它，谁还指着它**：

```extc
var x: i32 = 7
var r = ref x
let x: i32 = 99     // 新名字，新存储
println(r)          // 7   ← 老的那个，一根汗毛都没动
r = 100             // 写穿到老的那个
println(x)          // 99  ← 新的那个
```

这条**正是「命名不是存储」的硬证据**：要是 `let x = 99` 是「写进老地方」，
`r` 读出来就该是 99。实测是 7 ✓ 这也是为什么它**不是语法糖**，而是语义。

### 顺手撞上的第二个 bug：`fn double(...)` 生成不出合法的 C

写测试时很自然地写了个 `fn double(n: i32) -> i32` —— 结果生成的 C 是
`int32_t double(int32_t);`，gcc 直接报错，**而且报错落在生成的 C 上**（用户看不到自己的源码）。

extC 里 `double` / `int` / `bool` 都只是**普通名字**（extC 的类型叫 `f64` / `i32` / `bool`），
所以这不是用户的错，是编译器该收掉的。加个 `__c` 后缀就行（`double` → `double__c`），
**extC 源码里的名字一个字都不改** ✓

这跟第 2 步是**同一条流水线**（「extC 名字 → C 名字」），所以顺手一起做了：
局部变量走检查器给的 `cname`，函数名走 `cSymName`，两张表共用 `base.c` 的关键字表。

### 实现要点（三处，都不大）

| 处 | 做什么 |
|---|---|
| `check.c` `declare` | 多一个 `shadow` 参数：`let` = 可遮蔽，`var` / 参数 = 不可 |
| `check.c` `lookup` | **从后往前**扫同一个作用域 —— 最新的 `let` 赢 |
| `check.c` `cNameFor` | 每个函数一套计数，`a` → `a__2`（单调不重复，避开模块级名字和 C 关键字）|

名字的**解析**在类型检查阶段定格（`Expr.u.ident.cname`），**代码生成只管照着印** ——
这个分工让代码生成那边只改了 4 个地方（`EX_IDENT` / 局部声明 / 两处形参表）✓

### 一个「没做」的决定

`var` **不允许**遮蔽同层 —— `var` 声明的是**存储**，同一层写两个同名 `var`
几乎肯定是想写 `x = ...`，那是 typo 不是意图。
参数按 `var` 算 ⇒ `let p = ...` 遮蔽参数可以，`var p = ...` 不行 ✓

---

## 2026-09-18 · **修好 soundness 洞** —— BOOTSTRAP §8 第 1 步（102 测试全绿）

`refDepth(函数调用) = 0` 是**错的上界**。实测：

```extc
fn identity(r: ref i32) -> ref i32 { return r }
fn bad() -> ref i32 { var x: i32 = 5  return identity(ref x) }
// main 里 let r = bad()  ⇒ 打印 0（应该是 5）⇒ 悬垂，但编译放行
```

### 为什么两个检查各自都对、合起来还是漏

| 检查 | 它看到什么 | 它为什么放过 |
|---|---|---|
| `identity` 的定义 | 参数深度 0 ≤ 返回要求 0 | **转发是对的**，它自己没毛病 ✓ |
| `bad` 的返回 | 函数调用深度 0 ≤ 0 | 它以为调用结果深度是 0 |

**错在信息在调用边界被丢掉了** —— 而 `r = pickB(ref outer, ref inner)`
（一个深一个浅，函数按情况返回）用同一个机制漏得一模一样。

### 修法只有一句话

> **引用深度（函数调用）= `max(所有实参的深度)`**

**证明**：被调函数能返回的引用只有两种来源 —— 它自己的**全局/静态**（深度 0），
或者**它的实参**（深度 ≤ `max(实参)`）。它返回不了自己的局部（那条规则本来就挡着）。
所以 `max(实参)` 是**正确的上界**，不是保守估计 ✓

### 但洞不止一个，而且**一个误报在给它站岗**

修的过程中撞上第三次的「④ 参数洗白」：`fn stash(s: slice<u8>) { G = s }` 放行 ——
参数把「借来的」洗成了「自己的」，深度归 0。补 `exprBorrowed` + `checkStoreEscape` 挡掉。

**然后想给「换个视图」补测试时才发现的**：`s = a[..]`（视图整体赋值）**本来就编不过**，
被当成「透过只读视图写元素」。也就是说 —— 之前**根本写不出**能触发上面两个洞的多组用例，
误报把洞盖住了。所以三个改动**必须一起落地** ✓

### 发现的模式（写进流程）

> **看到误报不要只想着「绕过它」，要问「它为什么会被触发」** —— 它可能在替真正的 bug 站岗。
> 这条是这个项目里第一次出现，但它的形状很常见：**一个假警报让一整类真 bug 变得不可达** ✓

**第 1 步的验收**（5 项全过，见 `BOOTSTRAP.md` §8）：
两个洞都变成编译错误，误报消失，**合法转发没有误报**，99 → **102 测试全绿** ✓

---

## 2026-09-18 · 换指向回来了 —— 因为需求出现了，而歧义有判据了

主人问：**「换指向一定要被禁掉吗」** —— 好问题，而且答案是不一定。

### 当初禁它的两个理由，现在一个消失一个有解

| 当时的理由 | 现在 |
|---|---|
| **`=` 有歧义**（写进去 vs 换指向） | **歧义有判据了**：看**右边**是什么 —— 值是写进去，引用是换指向 ✓ |
| **全仓库 0 处使用** | **需求出现了**：`varArray<T>` 增长时要换 buffer |

### 新规则（比「禁掉」更好，也更少特例）

| 右边 | 意思 | 生成 |
|---|---|---|
| **值** `T` | 写进它指向的地方 | `*p = v` |
| **引用** `ref T` | 换指向 | `p = q` |

**类型自己消歧义**，而且右边就在源码里看得见（P′）✓
变量和**字段**都适用（`self.p = target` 就是换 buffer 那个动作）。

### 为什么这次可以放心加

**纯加法**：以前 `p = <ref>` 是**报错**的 ⇒ 允许它不可能弄坏任何现有代码 ✓
这正好是奶昔写的那条排序规则（「改语义的必须早做，只加不改的随时能加」）的一次应用。

### 但要说清楚：它**解决不了** `varArray` 的真问题

```extc
fn push(self: mut ref varArray<T>, v: T) {
    let bigger = alloc<T>(self.cap * 2)   // 属于 **push 这一帧**
    self.data = bigger                    // 换指向 ✓ 现在合法了
}                                         // push 返回 ⇒ 这块内存没了 ✗
```

**根不在指向，在生命周期**：arena = 函数帧 ⇒ 方法分配的内存活不过方法返回。
DESIGN 早就定了推论：**返回引用 ⇒ 被引用者必须在调用者提供的 arena 里。**
所以 `varArray` 的落地方式要另外定（见下一条记录）。
## 2026-09-18 · B · 逃逸检查：extC 最后一个内存安全洞关上了

主人说「该写了，逃逸检查，这个好啊」—— 这确实是 extC 的命。

### 规则（DESIGN §2 早就定好，这次只是落地）

> **若引用 `r` 指向值 `v`，则 `depth(r) ≥ depth(v)`。**
> 人话：**引用不能活得比被指对象长。**

深度是**纯词法属性**：参数 = 0，函数体 = 1，每进一层块 +1。
**比两个整数**，不需要生命周期标注、不需要解约束、不需要流分析。

> **这就是「Rust 的安全 + 不写生命周期」的落点** —— 也是整个项目的定位里最难的那半句。

### 实现

| 东西 | 做法 |
|---|---|
| `Sym.depth` | 绑定上挂深度；`declare(..., depth)`，参数传 0，局部传当前作用域层数 |
| `Expr.refDepth` | 表达式里的引用**指向的活物**有多深（**类型里没引用就直接 0**，免得纯值拷贝被误报） |
| `exprRefDepth()` | `ref x` → x 的深度；`a[lo..hi]` → 底的深度；struct/数组字面量 → **递归取最大**；函数调用 → 0（由被调用者自己的返回检查担保） |
| 三道检查 | ① 返回 ② 局部初始化 ③ 给字段/元素赋值 |

### 洞关上了（实测）

```extc
fn bad() -> slice<i32> {
    var a: [8]i32 = [1, 2, 3, 4, 5, 6, 7, 8]
    return a[..]
}
```
```
error: this return value would hold a reference to a local variable that dies first
       (borrowed from depth 1, but this can only hold up to depth 0)
  note: A reference may not outlive what it points to. Borrow from a parameter
        (depth 0) or copy the data instead.
```

**改之前它不但编得过，还静默跑出了 `0`** —— 静默错比崩溃更危险。现在是编译错误。

而且**递归地看进 struct** 也管用：

```extc
struct holder { s: slice<i32> }
fn bad() -> holder {
    var a: [4]i32
    return holder { s: a[..] }     // ✗ 同样被拦住
}
```

### 合法代码一条都没误伤（94 个测试全绿）

`examples/escape.extc` 专门演示「什么可以」：

| 写法 | 为什么可以 |
|---|---|
| `fn rowView(b: ref board, y: i32) -> slice<i32> { return b.cell[y][0..4] }` | 借的是**参数**（深度 0）⇒ 活得比函数长 ✓ |
| `fn copyRow(b: ref board, y: i32) -> [4]i32` | 按值返回，不带引用 ✓ |
| 局部视图只在函数内用（`let v = a[..]`） | v 和 a **同深度** ⇒ 一起生一起死 ✓ |

> `rowView` 这种「从 `self` 借一片出去」是**视图存在的意义**，
> 逃逸检查必须放行它 —— 而它靠「参数深度 = 0」这一条就自然放行了 ✓

### 诚实说清楚：还有一条**故意不查**

**④ 把引用传给函数、函数把它存起来**：

```extc
var g: holder                     // （全局还没有，等 arena）
fn stash(s: slice<i32>) { g.s = s }
fn f() { var a: [4]i32  stash(a[..]) }   // 悬垂，今天查不出来
```

这需要**引用的「传染性」分析**（函数参数的引用会不会被存进比它更深的地方），
比深度模型贵得多。已经明确记成欠账（`REFS.md` §6 未决问题 5）。

**今天查不到它，但不慌**：① 还没有全局变量 ② `holder.s` 这种「引用字段」只能
在构造时设置（换指向已经取消）⇒ 这条路暂时走不通。

### 检查②③ 今天是安全网，将来会真用上

实测**今天只有①（返回）会触发**：② 因为「引用不到还没声明的内层变量」而实际上不可达；
③ 因为「给引用字段重新赋值」被「取消换指向」堵掉了。

但 arena 和全局变量一到，③ 立刻就是**主检查**：

```extc
var g: holder            // 全局 = 深度 0
fn f() { var a: [4]i32  g.s = a[..] }    // 0 ≥ 2 为假 ⇒ 正是这条规则挡住它
```

> 这也说明这条规则**不是为今天设计的，是为内存模型设计的** ——
> 现在放进去，等 arena 到位就自动生效。
## 2026-09-18 · A2-5：视图的可写性 —— extC 最后一个「会崩的洞」关上了

主人说「可以一键跟 Rust，但我想要更优雅」，然后自己想不到更好的。
奶昔给了一个答案，这一步把它落地了。

### 设计：`mut` 是**限定词**，不是第二个类型

| | Rust | extC |
|---|---|---|
| 可写性怎么表达 | `&T` / `&mut T`（两个**类型**） | **`mut` 限定词**：`ref T` / `mut ref T`、`slice<T>` / `mut slice<T>` |
| 切片的两种形态 | `&[T]` / `&mut [T]`（再造一对类型 + 各自的 impl） | **同一个 `slice<T>`、一套方法集** |
| 为什么 Rust 只能那样 | **切片在 Rust 里是内建类型** —— 它没有别的办法 | extC 的视图是 prelude 里的**普通结构体** ⇒ 限定词能通用 |

**实现上最漂亮的一点**：可写视图是一个**影子实例** ——
跟只读版**共用同一个 C 结构体名**、**不进实例表**（进去的话 codegen 会生成两份）。
于是「一个类型、一套方法集」真的成立了，而不是嘴上说说。

### 可写性**从源头继承**

| 从哪切出来 | 得到 |
|---|---|
| `var a` | `mut slice<T>` |
| `let a` | `slice<T>` |
| `"字面量"` | `slice<T>` |
| `mut ref` 参数 | `mut slice<T>` |
| 只读视图 | `slice<T>` |

⇒ **日常代码一个 `mut` 都不用写**，只有**声明**才写 —— 而签名一写，
调用点和读代码的人就都知道「这个函数会不会改你的数据」。

降级是自动的、单向的：`mut slice<T>` → `slice<T>` ✓（能写当然能读）；
反过来不行，必须显式写 `mut` ✓

### 关掉的**两个洞**（实测）

**洞①「按值参数偷写调用者的数组」** —— 以前签名完全看不出来：

```extc
fn fill(v: slice<i32>) { v[0] = 1 }     // 参数是副本，但元素是调用者的！
```
```
error: cannot write through `v`: it is a read-only view `slice<i32>`
  note: a view is read-only unless its type carries `mut`. Writing through a
        by-value view would change the caller's data without the signature saying so.
```

**洞②「写字符串字面量」** —— 以前**编得过、运行时段错误**：

```extc
var s = "abc"
s[0] = 88        // error: cannot write through `s`: it is a read-only view `slice<u8>`
```

> **② 是 extC 最后一个「会崩的洞」** —— 从今天起，
> 「按 P′ 该看见的东西」在语法/类型上都看得见了。

### 顺手修的两个小东西

- `ttRender` 不打印视图的 `mut`，于是错误信息成了
  「expects `slice<i32>`, found `slice<i32>`」—— 谁也看不懂。现在会打 `mut slice<i32>`。
- 降级规则原来只覆盖 `ref`，视图走不到 ⇒ 可写视图传给只读参数会误报。
  现在统一成「**`mut` 的东西可以当非 `mut` 的用**」，两种都适用。

### 现在 `slice` 的能力（`examples/mut-views.extc`）

```
一开始   [1, 2, 3, 4, 5, 6]
求和     21
反转中间 [1, 4, 3, 2, 5, 6]      ← 就地 reverse，签名说了它会改
填 9 之后 [1, 4, 3, 9, 9, 9]     ← 可写视图传参
只读数组求和 24
```

**`reverse` / `fill` 这种「通用原地算法」现在是类型安全的** ——
它们要是写不出来，「视图」这个抽象就没意义。
## 2026-09-18 · A2-3：只读引用真的只读了（签名终于说实话）

上一步留了一个**故意的中间状态**：`ref T` 名义上是只读，但写它还不报错。
这一步把闸门关上，然后让**编译器自己指出每一处该改成 `mut ref` 的地方**。

### 加了两道检查（各管一半）

| # | 检查 | 效果 |
|---|---|---|
| 1 | **沿「地方」往内走，遇到只读引用就拒** | `fn f(b: ref board) { b.x = 1 }` ⇒ 报错 |
| 2 | **会写的方法（`self: mut ref T`）要求接收者可写** | 只读引用上不能调 `bump()` |

关键区分：**「绑定是不是 `let`」和「路上有没有只读引用」是两件事** ——
`var` 的东西里也可能装着一个只读引用（比如 `fn f(v: ref board)` 里的 v）。
写进去要**两样都满足**。

### 最漂亮的一点：**「逐处判断」不用人做了**

原计划 A2-4 是「全仓库 ~80 处 `ref` 逐处人工判断该不该改成 `mut ref`」。
闸门一关，**编译器把每一处都指了出来** —— 实际只有 **9 处**：

| 文件 | 问题 | 修法 |
|---|---|---|
| `fenwick` | `add` / `brute::add` / `rng::next` 写 `self` | `mut ref self` |
| `generics` | `box::set` 写 `self` | `mut ref self` |
| `gomoku-board` | `board::place` 写 `self` | `mut ref self` |
| `option-result` | `place` / `placeLine` 写 `b` | `b: mut ref board` |
| `refs` | `counter::bump` 写 `self` | `mut ref self` |
| `structs` | `point::moveBy` 写 `self` | `mut ref self` |
| `tour` | `counter::bump` 写 `self` | `mut ref self` |

**而且有两条是「连锁」**（特别值钱，因为它证明了检查真的沿着调用图在走）：

```extc
fn below(self: ref rng, bound: i64) -> i64 { return self.next() % bound }
//  ↑ next() 要可写接收者 ⇒ below 也得改成 mut ref

fn addTo(c: ref counter, by: i32) { c.bump(by) }
//  ↑ bump() 要可写接收者 ⇒ addTo 的签名也得改
```

**光看函数体是看不出这种问题的** —— 是「标注了权限」之后检查器追出来的。

### prelude 一行都没改

`slice<T>` 的 6 个方法（`isEmpty` / `hasAt` / `get` / `==` / `find` / `startsWith`）
**全是只读的**，所以它们自动变成「`let` 的视图也能调」。

> 这本身就是对设计的验证：**当初把它们写成只读方法，今天白拿到一个好处。**

### 一路上的另一件事：错误信息

三道检查的信息都写清了「为什么」：

```
error: cannot write through a read-only reference
  note: `ref T` is a **read-only** borrow; writing through it needs `mut ref T`
        in the declaration. Read-only is the default so that a signature says what it does.
```

### 还剩下的（下一步是「视图的可写性」）

闸门只关了 `ref T`，**还有两个洞**是「按值传进来的视图」造成的：

| 洞 | 例子 | 为什么现在拦不住 |
|---|---|---|
| ① 按值参数写进调用者的数组 | `fn f(v: slice<i32>) { v[0] = 1 }` | 参数是**调用者的局部副本**（`mut = true`），但 slice 的内容是共享的 |
| ② 写字符串字面量 | `var s = "abc"` → `s[0] = 1` | 字面量在只读段；**视图没有记「它从哪切出来的」** |

两个洞是**同一个原因**：视图的可写性是**来源的属性**，而现在的 `slice<T>` 只有一种。
下一步就是把那个设计落下来（`mut` 当限定词、切出来时从源头继承）——
做完之后这两条都会变成编译错误。见 DECISIONS。

## 2026-09-18 · A2 第一步 + 第二步：标量引用活了，`swap` 能写了

主人拍板了引用语义的形状（**形状 3：值位置自动解引用 + `mut ref`**），
然后立刻开工。这一轮把地基和最要紧的那块做掉了。

### 做完了什么

| 步 | 内容 | 状态 |
|---|---|---|
| A2-1 | `mut ref T` 的解析 + 类型位 + **可写性从源头继承** | ✅ |
| A2-2 | **值位置自动解引用** + **写穿赋值** | ✅ |
| A2-3 | 把 `ref T` 真的当只读（写了就报错） | ⬜ 下一步 |
| A2-4 | 全仓库 ~80 处 `ref` 逐处判断读还是写 | ⬜ |

**结果：`ref i64` 从死类型变成了能用的东西** ——

```extc
fn bump(p: mut ref i64) { p = p + 1 }
fn swap(a: mut ref i64, b: mut ref i64) { let t = a  a = b  b = t }

var n: i64 = 10
bump(ref n)                 // n = 11
println(n)                  // 11 —— 透过引用读，自动解引用
let m: i64 = ref n          // 值位置 ⇒ 拷值
```

`swap` **以前根本写不出来**（标量 ref 既没有字段也没有元素，
「透过它写」表达不出来；能表达的「换指向」对标量毫无意义）。

### 一条规则解释所有情况

> **可写性看「这个值是从哪拿到的」，只有三个地方：**
> ① 绑定 `var`/`let`　② 参数 `mut ref`/`ref`　③ 字段的类型上写 `mut`

取引用时**权限从源头继承** ⇒ 日常代码**一个 `mut` 都不用写**，只有**声明**才写。
而且 `mut ref T` 能自动降级成 `ref T`（能写当然能读），反过来不行。

### 踩的坑（老毛病第 N 次）

**`ttCanWiden` 把 `ref i32` 放进了 `mut ref i32`** —— 因为 `intInfo()` 会把 ref 剥掉，
于是变成了「i32 拓宽到 i32」。加了守卫：**引用不参与拓宽**。

这已经是**第四次**同一类问题了：

| 次 | 哪里 | 症状 |
|---|---|---|
| 1 | `typeContainsRef` | 不代入泛型实参 ⇒ `option<slice<u8>>` 漏检 |
| 2 | `genStructLit` | 实例里字面量还是模板类型 ⇒ 零值退化成 `0` |
| 3 | `zeroValue` | 不代入实例上下文 ⇒ 递归死循环 |
| 4 | `ttCanWiden` / `intInfo` | 隐式抹掉 ref ⇒ 权限被抹平 |

**共性是：helper 为了「通用」而把信息丢掉，而丢掉的正好是这层要检查的东西。**
DEVLOG 里已经记了三次，这次是第四次 —— 值得当成一条设计纪律：
**凡是「把类型抹平」的 helper（`ttBase` / `intInfo` / 各种 `strip`），
调用点都要问一句「抹掉的那一层，这里要不要检查？」**

### 「换指向」正式取消

`r = ref b` 现在报错，而且信息写清了为什么：

```
error: cannot retarget a reference
  note: `=` on a reference writes **into** what it points to;
        rebinding was removed on purpose -- declare a new binding instead.
```

判据是当时查过的：全仓库 **0 处**使用、有替代写法、以后能**纯加法**加回来。

### 还剩什么（A2-3 是真正的破坏性那一步）

现在 `ref T` **还没有**被强制成只读 —— 所以 prelude 和 examples 里
那些 `self: ref board` 里写字段的代码**还能编过**。这是**故意的中间状态**：
不然后面一步会一次炸一片，没法定位。

A2-3 要做的是：**让「通过只读引用写」报错**，然后把 80 处 `ref` 逐处判断，
该写的地方改成 `mut ref`。做完那一步，「签名不说实话」才算真的解决。
## 2026-09-18 · 第一道真算法题：树状数组（顺手补了位运算，还挖出两个洞）

主人在奶昔做完 T5 之后问：**「现在是不是可以写一些基础算法题试试水了，比如树状数组」**

奶昔去试，第一行就卡住：`i & (-i)` **解析不了** —— 位运算在词法表里，但解析器从来没接。

### ① 位运算：优先级**照着 C 抄**

`& | ^ ~ << >>` 补齐，优先级跟 C（和 Go）一致。这是**故意**的：
写算法题的人手上有 C 的肌肉记忆，「设计得更好」的优先级只会天天出错。

**`<<` / `>>` 故意不进词法表**：`box<box<i32>>` 里的 `>>` 必须是两个独立的 `>`
（类型实参靠它配对）。要是词法层就合成一个 token，泛型嵌套类型当场解析不了 ——
C++ 当年正是栽在这上面。所以改成在**表达式**那一层用「两个相邻的 `<` / `>`」来认。

对照 C 验了优先级，四条全对：

```
1 | 2 ^ 3 & 2   → 1     （同 C：1|(2^(3&2))）
1 + 2 << 3      → 24    （同 C：(1+2)<<3）
12 & (-12)      → 4
15 >> 2 & 3     → 3
```

顺带发现一条：**`6 & 3 == 3` 在 extC 里是类型错误**，而在 C 里它是
`6 & (3==3)` = `6 & 1` = 0 —— 一声不响。差别在于 **extC 的 `bool` 不是整数**。
C 里最经典的优先级坑，在 extC 里变成了编译期就能看见的错。

### ② 顺手挖出来的两个洞（都是「静默错误」，不是崩溃）

写随机数发生器的时候想透过 `ref i64` 改种子，试出这一串：

```extc
fn bump(p: ref i64) { p = p + 1 }
```

编译**通过**，运行**没报错**，调用者的变量**没变**。看生成的 C：

```c
void bump(int64_t * p) { p = (p + 1); }     /* 指针算术！*/
```

**根因**：`ttIsNumeric()` 和 `literalFits()` 都经过 `ttBase()`，
而 `ttBase` 会把 `ref` 抹掉 —— 于是 `ref i64` 被当成数字，
`p + 1` 拿到了类型 `ref i64`，赋回给 `p` 类型也对，一路绿灯。

另一个同源：`p = 5` 也被放过去，生成的 C 报
`makes pointer from integer without a cast` —— **gcc 的原始错误漏到了用户面前**。

**修法**：`ref` 两边都必须真是引用（`checkAssignable` 里挡），
算术/位运算遇到 `ref` 一律报错：

```
error: cannot apply `+` to `ref i64` (a reference)
  note: a `ref` is not a number: in C this would silently become pointer arithmetic.
```

**这是 extC 存在的理由本身**：C 里「看起来对、其实在挪指针」的代码，
在这里必须在类型层被挡住。

### ③ 树状数组本身

`examples/fenwick.extc`，做成**自检**的（没有输入，也没有随机数库）：

```
随机对拍 20000 次，结果不一致的次数 = 0
区间和 [2,4]   = 9
前缀和 [1,5]   = 15
a[3] += 10 后：区间和 [2,4] = 19 · 前缀和 [1,5] = 25 · 单点 a[3] = 13
逆序对 [5,1,4,2,8,3] = 7
```

**树状数组跟暴力求和对拍 2 万次零不一致** —— 语言够用了。

顺便暴露两个「不是 bug 但要知道」的点：

- **数组长度必须是编译期常数**（`tree: [1025]i64`），还没有全局常量 ⇒ 最大值只能写字面量。
- **值语义的代价是实数**：`fenwick` 有 8KB。不过奶昔本来写的「不写 `ref self` 就会拷 8KB」
  **是错的** —— 真去试才发现**方法必须收 `ref self`**，编译器直接拦：
  ```
  error: `self` of `counter.bumpCopy` must be `ref counter`
  ```
  所以「不小心拷一份」这回事不会发生 ✓。值语义真正咬人的地方是**赋值和返回**：
  `var f2 = fenwick::new(8)`、`let x = b` 这种本来就要拷贝（这是值语义的定义）。
  **教训**：DEVLOG 里的话也得验过再写，「应该是这样」不算。
- **没法透过 `ref` 写标量**：随机数发生器的状态只能包一层 struct
  （`self.state = ...` ✓）。这个限制记进 DECISIONS 当待定项了。

## 2026-09-18 · T5b：`?` —— 展开成语句，因为 C 没有语句表达式

**形状**：`?` 是**语句级**的转发。C 里没有语句表达式（`({ ... })` 是 GNU 扩展），
所以要展开成三句：

```c
option_i64 __extc_try0 = <被 ? 的表达式>;      /* 求值**一次** */
if (!__extc_try0.has) return <失败值>;
i64 x = __extc_try0.value;                    /* 接着用载荷 */
```

**推论**：`?` 只在「整个语句能重写」的地方合法 —— 四个位置：
`f()?` / `let x = e?` / `x = e?` / `return e?`。
写在别处（`f(e? + 1)`）**报清楚的错**，而不是生成编不过的 C。

### 载荷类型可以不同，错误类型必须相同

这是奶昔想清楚的一个区分：

- **载荷**：里层 `result<i64, E>` 放进返回 `result<point, E>` 的函数 ✓ —— 要构造
  外层失败值（`.value = <point 的零值>, .err = 内层的 err`），这做得到。
- **错误**：必须一模一样 ✗ 否则「搬过去」就是在编造一个不存在的转换。
  `result<i64, a>` 的 `?` 出现在返回 `result<i64, b>` 的函数里 → 报错。

### 编译器认识的**协议**（第二条）

`?` 要知道 option/result 的标签字段叫什么。这跟视图的 `data` + `len` 是**同一种分工**：

> **语言认识协议，库提供结构。**

| 容器 | 编译器知道的 | 谁提供结构 |
|---|---|---|
| 视图 `slice<T>` | `data` + `len` | prelude |
| `option<T>` | `has` + `value` | prelude |
| `result<T,E>` | `ok` + `value` + `err` | prelude |

所以生成的是 `(result_point_gameError){ .ok = false, .value = ..., .err = ... }`
这样的字面量 —— 「构造」这件事本身还是 prelude 的字段，编译器只是知道它们的名字。
**这跟「把库塞进编译器」的区别在于：方法、语义、名字的取舍全在 extC 源码里，
编译器只多知道「哪个字段是标签」。**

### 踩的坑（一个自己造的）

接 `?` 的时候顺手整理 `ST_VAR` 的缩进，**把「无初始化式」那条路上的 `declare()` 删掉了** ——
于是 `var b: board` 静默地什么都没声明，直到 `b.cell[0][0] = 1` 报「undefined name」才露出来。

**奶昔自己的问题**：用 `sed` 按**行号**改代码，而行号在编辑过程中一直在变。
教训：**行号会漂**，改代码要按内容匹配（edit 的 old_string），不要按行号。

（这个坑被测试抓到了 —— 但抓到它的是一条手写的临时例子，不是回归套件。
说明**每次改完都得跑一遍全套**，这条纪律救了它。）

## 2026-09-18 · T5a：`option` / `result` —— 先把「怎么构造」这条路打通

**主人的反应值得记下来**：奶昔一口气抛了三个「构造怎么写 / 无载荷失败怎么写 /
`T` 不能含 `ref` 接受吗」的技术选择题，主人回了一句**「等等等等我看不懂了」**。

—— 这是奶昔的问题。**抽象选项不如能跑的代码**：主人一直是「看代码」的人
（README 维护规矩第 5 条就是他定的）。所以奶昔改成：讲清楚 `option`/`result`
**是什么**（用五子棋举例），写法细节自己按推荐的定，做完直接给能跑的例子。

### 做了三件事

**① `::` 关联函数** —— 写在 `struct` 体内但**不带 `self`** 的函数：

```extc
struct option<T> {
    fn some(v: T) -> option<T> { return { has: true, value: v } }
}
let o = option<i64>::some(42)
```

为什么非有它不可：查了一下，**泛型自由函数压根不解析**（`fn wrap<T>(...)` → `expected '('`），
而**方法必须有 `self`**。两样都没有 ⇒ **prelude 里根本没法写构造器**。
顺带它还是 `array<T>::new()` 的前置（ARRAYS.md 里卡住的那个问题）。

解析上的坑：`option<i64>::some` 里的 `<` 跟小于号撞车。做法是**先只看不动地**
扫一遍（平衡 `<` `>`，看后面是不是 `::`），是才真解析 —— 判据 `::` 不是合法运算符，
所以两种解释互斥。这样试探阶段不会喷假错误。

**② prelude 里的 `option` / `result` / `unit`**（都是 extC 源码）。

**③ 三处修出来的真 bug**（都不在 `option` 本身，是**新用法逼出来的老问题**）：

- **C 不允许空 struct**。`struct unit { };` 是 GNU 扩展，而且 `(unit){0}` 会报
  `excess elements in struct initializer`。修法：零字段的 struct 由编译器补一个
  `char __extc_empty;` —— 用户看不见，`(T){0}` 就合法了。
- **泛型实例里字面量记的类型还是「模板」**。`result<unit, E>::failure` 里的
  `{ ok: false, err: e }` 省略了 `value`，零值那条路拿到裸 `T` → 退化成 `0` →
  生成的 C 里 `.value = 0`（而 value 是 `unit`）。修法：`genStructLit` 开头
  `subst(g, e->type)` 先整体替换一次。
- **`typeContainsRef` 不代入泛型实参**。`option<slice<u8>>` 的 value 是 slice
  （里面有 `ref`）⇒ 没有零值，检查却整条漏过去，最后在生成的 C 里露出
  `__extc_reference_has_no_zero_value__`。修法：递归前 `ttSubstitute`。
  **同一个模式第三次出现了**（都是「递归时忘了实例上下文」）。

### 一个被编译器拦住的命名错误

奶昔本来把构造器写成 `result<T,E>::ok(v)` / `::err(e)` —— 编译 prelude 时当场报：

```
error: `result.ok`: a field and a method cannot share a name
```

标签字段已经叫 `ok` 了，而**字段和方法不许同名**。这条规则是主人早先定的，
这次它自己抓出了奶昔的疏忽。改成 `success` / `failure`。

### 顺手修的两个 C 层面的坑

**① `ref self` 方法不能作用在临时值上。** `firstEmpty(...).valueOr(-1)` 生成
`&(f())` —— C 里非法。第一反应是写 `&((T){ f() })`，**结果更糟**：

```
error: incompatible types when initializing type '_Bool' using type 'option_i64'
```

因为 `(T){ x }` 在 C 里**不是拷贝**，它拿 `x` 去初始化**第一个成员**（`has = <option>`）！
正确的写法是**单元素数组**的复合字面量：

```c
option_i64_valueOr((option_i64[]){ firstEmpty(...) }, -1)   /* 数组退化成 T* */
```

数组初始化是逐元素的，`{ f() }` 就是「用一个 T 初始化元素 0」。单独写了 5 行 C
验证过 `-Wpedantic` 下也干净。

**② 零字段 struct 的占位**（见上）。

**教训**：C 的复合字面量 `(T){x}` 和「拷贝构造」**长得一样但意思完全不同**，
这种「看起来对、其实初始化了别的成员」的坑，编译器给的错误信息离真因很远
（报 `_Bool` 初始化失败，而问题在 `{ }` 的语义上）。

## 2026-09-18 · 主人一句「我不是有 let 和 var 吗」点醒了奶昔

**起因**：奶昔把「视图元素该不该可写」当成一个**新设计问题**去问主人。
主人回：**「老大我不是有 let 和 var 吗，一个是相当于 const 一个就是不是 const」**

—— 对。**机制早就有，是奶昔实现漏了。**

奶昔去把四种写法全试了一遍：

| 写法 | 修之前 | 应该 |
|---|---|---|
| `let v = ...; v = ...` | ✗ 报错 ✓ | ✗ |
| `let n; ref n` | ✗ 报错 ✓ | ✗ |
| `let p: point; p.x = 99` | **✓ 放过去了** | ✗ |
| `let a: [5]i32; a[0] = 99` | **✓ 放过去了** | ✗ |
| `let v = a[..]; v[0] = 99` | **✓ 放过去了** | ✗ |

**根因**：赋值检查只看了裸标识符：

```c
if (s->u.assign.target->kind == EX_IDENT) { ...查 sym->mut... }
```

`p.x` / `a[0]` / `v[0]` 的 kind 都不是 `EX_IDENT`，于是**整条分支都不进**。
而这三个恰恰是「忘了写 `var`」最常犯的形态。

**修法**：加一个 `placeRoot()` —— 穿过字段和下标找到**根**变量，
`let` 管的就是这个根。取 `ref` 的检查也一并改成查根（`ref p.x` 里 p 是 `let` 也不行）。
顺带把 4 条错误信息里**粘在一起的英文句子**分开（少了个空格，
`writes.Use` 这种 —— 设计原则第 6 条说「错误信息是语言的一部分」，那空格也算）。

**新增 5 个反例测试**：`let_write_field` / `let_write_index` / `let_write_view` /
`let_ref_field`，以及 `examples/slices.extc` 里把「透过视图写」那段改成了 `var`
（**例子被自己的新规则抓了一次，这是好事**）。

### 但这条规则是「浅」的 —— 奶昔必须说清楚

三个缺口还在：

1. `var w = v`（把 `let` 视图拷给 `var`）之后能写；
2. 传进函数之后能写（视图按值传递，拷的是指针）；
3. **`self: ref T` 的方法接收者不受限** —— `let p; p.moveBy(1)` 现在允许。

第 3 条不是懒，是**卡在 `ref T` 一个东西兼了两种含义**：
它既是「可变借用」，也是「大 struct 的只读共享」（不想拷贝 900 字节的棋盘）。
而方法体只检查一遍，编译器没法从签名上分辨「这个方法会不会写」。

所以要么接受现状，要么**把可变性放进类型**：
`ref T`（只读，`let` 也能调）/ `mut ref T`（可变，接收者必须 `var`）——
Rust 的 `&` / `&mut`。代价是**改每个签名**（含 prelude 的 6 个只读方法）
+ `slice<T>` 要分只读/可变两个类型。好处是能顺带根治
「写字符串字面量会崩」（只读视图的 `data` 就是只读引用）。

**奶昔的建议是先不动**：因为 week-4 的逃逸检查本来就要把可变性放进类型，
两件事一起做省一次全量改签名。**但这个要主人拍**（写进 DECISIONS 的待定项了）。

## 2026-09-18 · T5a-3 切片视图：P 在这里第一次真的省掉了一整类检查

**做了什么**：`a[lo..hi]` 四种写法、多维切最后一段、元素可写、越界 trap。

### P 不是口号 —— 它在这里变成了「生成的 C 里没有那个检查」

省略的界**由 check 阶段补成字面量**（`a[2..]` → `a[2..15]`）。
于是 codegen 只要看到「两个界都是字面量」就知道范围可证明：

```c
/* let v = a[2..5]  → */
slice_i32 v = (slice_i32){ .data = &(a.data[2]), .len = 3 };
```

**零检查、零函数调用。** 界里有变量时才走带 `extc_checkedRange` 的 helper。

而且证明的方向不只是「省检查」，还能**在编译期改错**：

```
error: slice end 9 is not inside `[5]i32` (length 5)
error: slice `3..1` ends before it starts
error: slice end -1 is not inside `[5]i32` (length 5)      ← `-1` 是 EX_UN，先教会编译器认字面量
```

关键判断：**单独一个界越界，跟另一个界是什么无关，所以永远错**。`a[i..20]` 对 `[15]i32`
不用等运行时就该报。

### 底必须是「地方」—— 一个刻意的部分检查

```extc
let v = make()[1..3]   // ✗ cannot slice a temporary value
```

切固定数组要**取元素地址**，所以底只能是变量/字段/索引链。这不是洁癖：
`make()` 返回值的临时存储活不过这一句，那是个**指向已死对象**的视图。
切 **slice** 不受限（只是指针算术），所以 `"abcdef"[1..3]` 合法。

完整的逃逸检查（返回局部数组的视图）**还是 week-4** —— 这是明确的、写在文档里的欠账。

### 三个「顺手撞出来的真 bug」

**① 视图索引器按值返回 → `slice<struct>` 根本编不出来。**
prelude 里 `slice<T>::==` 写 `self[i] != other[i]`，而 `fn ==(self: ref T, ...)` 收 `ref`，
`&(按值返回的临时值)` 在 C 里非法：
`error: lvalue required as unary '&' operand`。
同一个原因让 `s[i] = x` 也炸（`lvalue required as left operand of assignment`）。

修法：索引原语**返回指针**，`s[i]` 编译成 `(*slice_T_index(...))`。一举两得 ——
元素成了 lvalue，既能取地址比较，也能写。

**副作用（要主人确认）**：**视图从「只读」变成「可写」了**。
奶昔认为这不是放松：`slice<T>` 的 `data` 本来就是 `ref T`，
而 extC 的规矩是「引用必须在类型里看得见」（P′）—— 看得见就是显式的。
已知代价：`slice<u8>` 可能来自字符串字面量（C 里的只读段），**写它会崩**。
要根治得在类型里区分可变引用，那是 week-4 的事。

**② 泛型里比较数组元素报假错误。**
`slice<[6]i32>`（切出几行）的 `==` 要比较两行，而 codegen 里「数组派生的 `==`」
那条分支被 `!e->needEq` 挡着 —— 泛型里的比较**推迟到实例化**才解析，
代入实参后元素正好是数组，于是掉进方法查找、报
「`array_6_i32` needs to define `!=`」。**一个不存在的错误。**
修法：那条分支不能看 `needEq`。

**③ 非字节视图打印成 `slice { data: <ref>, len: 3 }`。**
`println(a[1..4])` 输出一个没有信息的地址占位。视图是「一片元素」，
那就按元素打 —— 现在输出 `[2, 3, 4]`。
顺带修了 `genPrintValue` 里泛型实例落到 `"?"` 的分支
（「struct 里放一个 `slice<i32>` 字段」以前打印出来是个问号）。

### 教训

**「顺手多试几个类型」是有回报的。** 前两个 bug 都不是切片逻辑本身错，
而是**切片把「视图」这个早就存在的东西第一次用到了 struct 元素上** ——
`slice<i32>` / `slice<u8>` 一路都正常，因为数值比较是 C 原生的、不需要取地址。
一旦元素是 struct，「取地址」这条隐藏依赖就露出来了。

**同一个东西第一次被新用法触及时，最容易暴露老问题。**

## 2026-09-18 · 示例代码抓出的真 bug：原型区和函数体区交错了

**怎么发现的**：T5a-2 提交之前，按 README 维护规矩第 5 条写一段示例代码演示新写法 ——
里面有个「元素是带 `fn ==` 的 struct 的数组」。extC 自己编得**很成功**（生成 C 没报错），
**gcc 编不过**：

```
build/demo.c:167:14: error: implicit declaration of function 'point_eq'
build/demo.c:190:6: error: conflicting types for 'point_eq'; have '_Bool(point *, point)'
```

**根因**：`generateC` 的后半段原来是「原型 → 函数体 → 原型 → 函数体」交错的：

1. typedef
2. struct 定义（有序）
3. `_debug` 原型 + 数组 `_eq` 原型
4. **实例的函数体**（数组 `==`、`_debug`）← **这里就调用了 `point_eq`**
5. struct `_debug` 函数体
6. 实例方法原型
7. **自由函数 / struct 方法的原型** ← `point_eq` 的原型在这儿，太晚了

数组的 `==` 是**编译器生成**的，它要调用元素类型的 `fn ==`，而那个函数的原型排在它后面。

**修法**：拆成两个**严格分离**的区域 —— 原型区（只许放原型）和函数体区。
代码里写了一句规矩注释：「这一区里只许放原型，函数体一律不许出现。」

**教训**（和 T4 的「构建系统」那次同源）：**顺序问题别靠「碰巧对了」，要结构上排掉。**
而且 —— **写示例代码不是走过场，它是测试**。README 维护规矩第 5 条这次真的赚回了一个 bug。

**回归保护**：`examples/array-of-struct.extc`（数组的 struct 元素、二维 struct 数组、
通过副本验证值语义）。因为 `tests/run.sh` 会跑 `examples/*.extc`，这个坑回不来了。

### 顺带发现的一个**语法缺口**（还没修，要问主人）

```extc
println(ps[0] == { x: 1, y: 2 })     // ✗ cannot infer the type of a bare `{}` here
println(ps[0] == point { x: 1, y: 2 }) // ✓ 写全名字就行
```

裸 `{}` 的规矩是「上下文钉死类型」（`var x: T = {}` / `return {}` / `f({})`）。
`==` 是**语法糖**，展开成 `T.==(lhs, rhs)` 之后右边其实**有**参数类型可推 ——
所以这里是个**可以补**的洞，不是设计上的不可能。

但补之前得想清楚 `ref` 那一维：`fn ==(self: ref point, other: point)` 和
`fn ==(self: ref point, other: ref point)` 都可能存在，
「拿左边的类型当右边的期望类型」在左操作数是 `ref T` 时会推错。
所以这条**留给主人定**（见 MANUAL「还没定的」）。

## 2026-09-18 · T5a-2 固定数组：五子棋棋盘能编能跑了

**目标**：让 `[15][15]i32` 这种棋盘真的能用 —— 这是 G2（第一个真实用户是五子棋）的硬门槛。

### 做了什么

`[N]T` 类型（多维递归）、字面量 `[1, 2, 3]`、索引 `a[i]`、数组 `==`、`println` 递归打印。

### 发现 / 决定

**① 统一套 struct，连成员也一样 —— 换来「索引永远不需要看上下文」。**

C 里数组不能赋值也不能返回，所以套 struct（`typedef struct { int32_t data[5]; } array_5_i32;`）。
关键是**成员也不特殊对待**：结构体里放数组字段也是套好的 `array_5_i32`，不是裸 `int32_t data[5]`。
代价是多一层名字，换来的好处是 **`a[i]` 永远编译成 `a.data[i]`** ——
不需要「这个表达式是不是成员」的上下文判断。**上下文相关的 codegen 是 bug 温床，宁可多一层。**

**② C 名字递归修饰**：`[15][15]i32` → `array_15_array_15_i32`。跟泛型实例共用 `instances` 表。

**③ 字面量严格计数 + 末尾 `...`。**

`let a: [3]i32 = [1, 2]` 报错（`needs 3 element(s), got 2`）。
主人要的语法糖是「少写几个零」，所以给 `[1, 2, ...]`（= 剩下补零）。
**`...` 只在末尾、且必须能从上下文拿到长度** —— 没有上下文时明确报错
（`... needs the array length from context`），不猜。

**④ 零初始化是默认，所以没有 `[0; 3]` 这种语法。**

定案 8 已经定了零初始化，再给一个「全零字面量」语法就是两套写法干一件事。

### 验证

```
$ ./build/extc --run examples/arrays.extc
1 / 0 / 1,1 / [[1, 0], [0, 1], [1, 1], [1, -1]] / [1, 2, 0, 0, 0] / true / false / true / 1

$ ./build/extc --run examples/gomoku-board.extc
moves = 9 / (7,11) = 1 / 黑棋竖着五连？true / 白棋竖着四列？false / 重复落子？ false / 出界？ false
```

越界 trap 带 extC 位置：

```
$ ./build/extc --run /tmp/oob.extc
/tmp/oob.extc:4: trap: index 7 out of range (length 3)
```

`./tests/run.sh` → **53 通过，0 失败**（新增 5 个反例：字面量多了/少了、`...` 没上下文、
索引不是整数、`[0]T`）。

### 坑

**`substEnter` 对 `TY_ARRAY` 解引用了 `inst->sdef`。**
泛型实例有 `sdef`（结构体定义），数组实例没有 —— 数组和泛型实例共用 `instances` 表，
所以进实例上下文时必须 `if (inst->kind != TY_GENERIC) continue;`。
注意：这是**共用表带来的类型混淆**，不是逻辑错误 —— 表共用是对的（都是「编译器生成的类型」），
漏的是「遍历时要按 kind 分流」。

还有一个小的：`println(dirs)` 一开始被拒（`isPrintable` 没有 `TY_ARRAY`）；
`a == c` 也被拒 —— 补上数组分支，并且 `typeSupportsEq` 要**递归**下去
（`[3][3]i32` 能不能比取决于 `i32` 能不能比）。

### 还没做

**切片视图 `a[lo..hi]`** —— 语法（`EX_SLICE`）已经能解析，codegen 里还是
`error: slicing is not implemented yet`。这是 T5a-3。

## 2026-09-18 · 主人问到点子上：`T` 会不会跟用户类型撞？

主人问：「直接用 `T` 真的是好的吗，万一用户自定义结构体叫 `T` 呢」

**奶昔去验了，真的能撞**：

```extc
struct T { x: i32 }        // 用户自定义的类型
struct box<T> { value: T } // 里面的 T 是参数还是那个结构体？
```
编译**通过**，行为是**遮蔽**（参数赢）。但**读者根本看不出来**。

奶昔之前只给了**约定**（「单个大写字母是通行写法，Go/Rust/TS 都这样」），
没给**保证**。这正是这一个晚上反复出现的同一个毛病：**把语义藏在命名约定里。**

### 修法：让两者集合不相交

强制两条：

- **类型名首字母必须小写**（camelCase）
- **泛型参数首字母必须大写**（`T` / `K` / `V`，也可以写 `Element`）

⇒ **语法上不可能同名。**

```
error: type name `T` must start with a lowercase letter
  note: Type names are camelCase (lowercase first letter). A leading uppercase
        letter is reserved for type parameters, so the two can never collide.

error: type parameter `t` must start with an uppercase letter
```

**「能编译但读者看不懂」是坏设计** —— 而这次的解法跟前面几次一样：
**把约定升级成检查**。

### 顺带：保留定义的完整处理（主人的原则）

主人同轮提出「不应该允许用户随意修改保留的定义（比如 slice）」。三件事：

1. **`reserved` 标记** —— prelude 的定义都标上，重定义时错误信息说清原因
2. **契约检查** —— prelude 加载后验证 `slice` 的形状，不符合就报 internal error
   （否则 prelude 改个字段名会变成「生成的 C 编译不过」这种莫名其妙的错误）
3. **确认了一件事：extC 天生防住「给已有类型加方法」** ——
   没有 impl 块、方法必须写在 struct 体内 ⇒ 用户**没有办法**扩展 `slice`。
   这不是运气，是「语法一致、不要有特例」的副产品。

**测试** 43 → 46 个（三个反例：`type_name_uppercase` / `type_param_lowercase` / `redefine_slice`）。

---

## 2026-09-18 · T5a-1 落地：`slice` 索引 + 字符串库（全部用 extC 写）

主人说「现在的 slice 用起来有点怪」。奶昔诊断：**怪的不是缺语法糖，是缺索引** ——
它是个「只能问长度、读不了内容」的东西。

### 关键发现：不用等数组

奶昔本来以为「索引 = 数组」，但其实 `slice` 的索引**单独就能做**，
而且字符串字面量已经能造出 slice 了（T4c）—— **不需要数组当垫脚石**。

于是拆成两小步，先做小的那个，怪味当场消失：

```extc
let s = "hello world"
println(s[0])                    // 104
println(s == "hello world")      // true  ← 按内容比较！
println(s.find("world"))         // 6     ← 词法分析器的核心
println(s.startsWith("hello"))   // true
```

**那个之前被拒绝的「`str ==` 是指针比较」的陷阱，从这里开始变成正确的按内容比较。**

### 设计：语言认识「协议」，库提供「方法」

索引要么让编译器认 slice 的协议（`data` + `len`），要么就得暴露一个**无检查的原语**：

```extc
// 反面教材：如果编译器不认协议，库只能这么写
fn get(self: ref slice<T>, i: i64) -> T {
    if i < 0 || i >= self.len { ... }     // ← 每个方法都要自己查一遍，漏一个就 UB
    return readAt(self.data, i)            // ← 而这个原语本身是 UB 的
}
```

**所以让编译器认识协议不只是更清晰，它是唯一能同时保住「库用 extC 写」和「没有 UB」的做法。**

一句话概括这个模式：

> **语言认识某个东西的「协议」（因为它有语法），库提供它的「方法」。**

| 语法 | 语言必须认识的协议 | 库提供什么 |
|---|---|---|
| `a == b` | 有个叫 `==` 的方法 | 那个方法的实现 |
| `a[i]` | 它是视图：`data` + `len` | `get` / `==` / `find` 全部方法 |
| `for x in xs`（将来） | 它有迭代器 | 迭代器的实现 |

**协议是小的、固定的；方法是多的、可变的。**

### 一个细节：索引原语**按值**收视图

如果直接生成 `a.data[checked(i, a.len, ...)]`，那 **`a` 会被求值两次** ——
`f()[i]` 就会调用 `f` 两次。所以生成的是每类型一份的原语：

```c
uint8_t slice_u8_index(slice_u8 v, int64_t i, const char *file, int line) {
    if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);
    return v.data[i];
}
```

**按值收视图 ⇒ 实参只求值一次**，而且不用为「值 / 引用」写两条路径。
越界时 trap 会带上 extC 的 file/line（由调用点传进去）：

```
foo.extc:12:9: trap: index 99 out of range (length 11)
```

### 主人在这一轮补的原则：保留定义不许乱改

> 「不应该允许用户随意修改保留的定义（比如 slice 这种）的重载，这会导致混乱」

**好消息：我们天生就防住了。** 因为 extC **没有 impl 块、方法必须写在 struct 体内** ——
**「给已有类型加方法」在语法上不可达**。这不是运气，是「语法一致、不要有特例」的副产品。

另外做了两件把**隐式契约变成显式检查**的事：

1. **`reserved` 标记**：prelude 里的定义都标上。用户重定义时报
   `` `slice` is a reserved definition and cannot be redefined ``，
   note 说明它来自 `stdlib/prelude.extc`、是语言契约的一部分。
2. **契约检查**：prelude 加载后验证 `slice` 真的是「第一个字段 `data: ref T`、第二个 `len`」。
   不检查的话，prelude 改个字段名会变成**「生成的 C 编译不过」**这种莫名其妙的错误：

```
extc: internal error -- the prelude's `slice` does not have the shape
the compiler expects (`data: ref T` then `len`)
```

### 验收

```sh
grep -cE 'struct slice|slice_[a-z]' src/*.c src/*.h    # ① 结构和方法：一无所获 ✓
grep -in 'slice' src/*.c src/*.h                        # ② 只剩三处「规定」+ 注释
```

三处**规定**：① 字符串字面量是什么类型 ② 字节序列怎么输出 ③ 视图的协议是 data+len。
**没有一处是实现。**

**测试** 43 个全过（`examples/strings.extc` 更新成展示新能力）。

---

## 2026-09-18 · `examples/tour.extc`：一份能跑的完整语言巡礼

主人问「来个完整的示例代码展示这个语言」。写了 `examples/tour.extc`，
把现在能用的东西全用上，而且**输出是真实的**：

```
=== extC 语言巡礼 ===
counter { label: alpha, value: 210, st: done }
counter { label: beta, value: 330, st: done }
a == b ? false
a.labelSize() = 5
a.describe()  = done
a.st == done  = true
pair { first: 6765, second: true }
pair { first: nested, second: counter { label: alpha, value: 210, st: done } }
counter { label: , value: 0, st: idle }
area-ish = 28.2743
small = 200  widened = 200
```

一份代码编出这些 C 结构体（单态化）：

```c
typedef struct state state;
typedef struct counter counter;
typedef struct pair_i64_bool pair_i64_bool;
typedef struct pair_slice_u8_counter pair_slice_u8_counter;
```

**注意 `pair_slice_u8_counter`** —— 泛型实参里嵌了一个**泛型实例**（`slice<u8>`），
名字修饰是递归的，而且**结构体定义顺序是按依赖排出来的**（T4c 踩的那个循环依赖坑就在这里）。

**一句话**：`println(a)` 能打出 `counter { label: alpha, value: 210, st: done }`
—— 递归打印 struct、枚举打名字、字节视图打文本，**用户一行 printf 都没写**。

---

## 2026-09-18 · T4c 落地：字符串就是 slice<u8>，`str` 内建类型没了

```extc
let s = "hello"          // 类型是 slice<u8>
println(s)               // hello
println(s.len)           // 5      —— len 是个普通字段
println(s.isEmpty())     // false  —— prelude 里的方法
```

生成的 C 只有这么点：

```c
slice_u8 s = (slice_u8){ .data = (uint8_t *)"hello", .len = sizeof("hello") - 1 };
```

**零分配、零拷贝。** 而且**长度交给 C 的 `sizeof` 算** —— 这是个很省事的决定：
转义（`\n`、`\t`）和 UTF-8 全都不用语言自己处理，C 已经算对了。

### 编译器还需要知道 slice 的两件事（都不是实现）

1. **字符串字面量是什么类型** —— 这是**语言规定**，所以 `check.c` 要查一次 `slice` 这个名字
2. **字节序列按文本输出** —— 这是**输出约定**（`printf("%.*s", len, data)`）

于是验收标准修得更精确了：

```sh
grep -inE 'struct slice|slice_[a-z]' src/*.c src/*.h   # ① 结构和方法：必须空
grep -in 'slice' src/*.c src/*.h                        # ② 只允许那两条规定 + 注释
```

**容器长什么样、有哪些方法，编译器一无所知。**

### 踩到的坑

**① `player` 包含 `slice<u8>`，而实例结构体排在普通 struct 后面 —— 循环依赖。**
写了「先普通 struct 再泛型实例」的规则，但两者**会互相包含**。修法是**按依赖顺序出定义**：

- 先出**全部** `typedef struct X X;`（指针字段只需要这个）
- 再按依赖序出 struct 体（迭代到没有进展为止；有环就硬出，让 C 去报 ——
  值语义的环本来就不是合法类型）

一劳永逸，将来 `array<point>` 之类也不会有问题。

**② `println(s)` 打成了 debug 形式** —— 因为字节视图的判断排在了 struct/泛型分支**后面**，
永远到不了。调顺序就好，但这个 bug 说明**分支顺序本身就是逻辑**。

**③ 顺手把「空 slice 造不出来」这个已知限制部分解决了**：`""` 是合法的 ——
它指向一个长度为 0 的字节视图 ✓。

### 还不能做的（诚实记录）

**字符串比较要等索引。** 按内容比较必须逐个看字节，而那要 `data[i]` —— 索引要等数组落地
（week-2）。现在报错说得很清楚：

```
error: `slice` does not define `==`, so it cannot be compared
```

**宁可报错，也不给一个指针比较的假答案。**

**测试** 41 → 42 个（新增 `examples/strings.extc`）。

---

## 2026-09-18 · 类型名改成 camelCase + 用「位置规则」替掉大小写魔法

主人问：「为什么要首字母大写？」—— 问得好，因为**那不是审美，是 parser 的消歧义手段**，
而且奶昔之前没交代过：

```c
if (at(p, "{") && isUpperCase(t->text)) return parseStructLit(p, t->text);
```

`if cond { }` 和 `let p = point { ... }` 里两个 `{` 光看 token 分不出来，
所以当时用「首字母大写」当判据。**这是个隐藏魔法** —— 跟之前那个「约定方法名叫 `eq`」
是同一类东西。

### 提案与评估

主人先提了更雷霆的方案：**每个块都显式标种类**（`do` = 栈 / `region` = 堆）。
奶昔的评估是**方向对但过度**：

- ✅ 抓到了真事实：**在 extC 里每个块确实就是一个 arena**（不是比喻，是定义）
- ❌ 但「`do` = 在栈里」这个语义**我们没有** —— extC 里块本身就是 arena 边界，
  没有栈/堆的语言层区分；`do` 如果什么都不改变，就是必须写的噪音
- ❌ **最关键**：`region` 之所以一眼可见，正因为周围都是普通的 `{ }`。
  如果每个块都写 `do`，`region` 就淹没在一堆 `do` 里 —— **反着破坏了 P′**
- ❌ 还有硬伤：`fn f() do { }` / `struct P do { }` 要不要写？不写就有例外，写就满屏 `do`

> **P′ 要求的是「要变化的东西必须看得见」，不是「所有东西都写一遍」。**
> extC 的显式原则是「**例外要响**」，不是「默认要吵」。

主人也澄清了一个诉求：**不希望运行时隐性推导** —— 奶昔解释了「编译期的类型确定」
和「运行时的隐性推导（interface{} / 鸭子类型 / 反射）」是两回事，后者才违反 P。

另外交代清楚了一件奶昔一直没说明白的事：
**`region` / arena / 逃逸检查现在一个都没实现**，它们全是设计文档里的东西（week-4）。
编译器 `src/base.h` 里那个 `Arena` 是编译器**自己**的内存管理器（C 写的），
跟 extC 语言层的 arena 同名但无关。

### 定案（主人选 B）

- **类型名也 camelCase**：`slice<T>` / `point` / `gameError`（内建类型本来就是小写，现在一致了）
- **消歧义改成 Go 的位置规则**：在 `if` / `while` 的**条件位置**里 `{` 属于代码块，
  其他位置 `标识符 {` 就是结构体字面量。括号里恢复成普通表达式。
- 唯一代价：`if p == point { x: 1 } { }` 要写 `if p == (point { x: 1 }) { }`
- **类型参数保留单个大写字母**（`T` / `K` / `V`）—— 通行写法，而且一眼区分「占位符 / 真类型名」

**顺手做了一条好报错**（认得出「忘了加括号」）：

```
error: struct literal in a condition needs parentheses: `(point { ... })`
  note: Inside an `if` / `while` condition a `{` starts the body block.
        To write a struct literal there, wrap it in parentheses.
```

> **规则写在语法里，不藏在命名里。** 这跟「`==` 要显式定义」是同一条路线的第三次落地。

改动：parser 加 `inCond` 状态（约 20 行），删掉 `isUpperCase`；
31 个文件的类型名改名（例子 / 测试 / prelude / 文档）；
`REVIEW-gomoku-sample.md` 不动（那是主人原样本的审读记录，里面的 PascalCase 是样本自带的）。

**测试** 39 → 40 个。

---

## 2026-09-18 · T4b 落地：prelude 机制通了，slice<T> 是 extC 写的

**目标**：`slice<T>` 用 extC 写在 `stdlib/prelude.extc` 里，编译器源码里一行硬编码都没有。

**验收（可机械检查）**：
```sh
grep -in slice src/*.c src/*.h     # 一无所获 ✓
```

**做法**：`tools/embed.c`（C 写的）把 `stdlib/prelude.extc` 变成 C 字节数组 → 链进编译器。
`main` 里 parse 两遍（prelude 用自己的 `Ctx`，路径显示成 `<extc prelude>`），
再合并进同一个 `Module`，然后 check 一次、生成一次。`parseModule` 从「重置」改成「追加」。

```extc
// stdlib/prelude.extc —— 用 extC 写的
struct slice<T> {
    data: ref T
    len: i64
    fn isEmpty(self: ref slice<T>) -> bool { return self.len == 0 }
    fn hasAt(self: ref slice<T>, i: i64) -> bool { return i >= 0 && i < self.len }
}
```

用户不用 import 就能用，而且**泛型实例照常单态化**（`slice_i32` / `slice_u8`），
自动调试打印也跟着来。

### 踩到的两个坑

1. **跨表 bug（很值得记）**：最初给 prelude 自检开了一张**独立的类型表**。
   但类型是**驻留**的、相等是**指针比较** —— 于是 prelude 里的 `bool` 和合并后的 `bool`
   成了两个指针，报出一句鬼话：`` return value expects `bool`, found `bool` ``。
   **修法：全程只有一张类型表**（加了 `ttRegister`，可重复调用、按名字去重）。
   > 教训：**驻留 + 指针比较 ⇒ 表的生命周期必须是全局的**，不能一个阶段一张。

2. **符号名不一致**：`embed` 生成的是 `<符号名>_len`，而 `prelude.c` 声明了另一个名字
   → `undefined reference`。统一成 `extc_prelude` / `extc_prelude_len`。

### 一个设计决定：prelude 出错算「编译器坏了」

prelude 用**自己的 Ctx** 检查，出错时报：

```
extc: internal error -- the bundled prelude does not compile
<extc prelude>:33:1: error: return value expects `i32`, found `str`
```

因为 prelude 是**编译器自带的** —— 它出错就说明编译器坏了，不是用户的问题。
这样错误位置永远正确（不会显示成用户的文件名），代价是 prelude 被检查两遍（它很小）。

### 顺手修的一个真问题

`slice` 含 `ref` 字段 ⇒ `var s: slice<i32>` 零初始化会造出**空引用**。
把「`ref T` 不能零初始化」这条规则**扩展到「含 ref 的 struct」**：

```
error: cannot zero-initialize `s`: it contains a reference
  note: `ref` is a non-nullable reference, so it has no zero value -- and neither does any struct that contains one
```

**测试** 38 → 39 个（新增 `examples/prelude.extc`）。

---

## 2026-09-18 · 诊断信息统一英文（主人要求）

主人指出：**`error:` 是英文、`note:` 是中文**，不统一 → 以后 notes 全部用英文。

改法：把 `ctxError` / `ckError` 的 note 参数（含 `bufPrintf` 拼出来的「字段：」「变体：」那些）
全部翻成英文，共 45 处。

顺手一并改掉剩下两处「编译器输出」：
- `--help` 的用法文本
- **生成的 C 里的头注释**（"Generated by the extC compiler -- do not edit by hand."）

理由：文档和代码注释继续用中文（那是给主人看的），
但**用户看到的东西**（error / note / help / 生成代码）统一英文 —— 一个程序两种语言很怪。
写进 README 第 6 条维护规矩。

**测试** 38 个全过（测试只比对 `error:` 那一行，不受影响）。

---

## 2026-09-18 · 「为什么 `==` 必须返回 bool」—— 不是拍的，是推出来的

主人问：「为什么要强制成 bool 啊，那不就很蠢了，如果我就是想要别的定义呢」

**这次奶昔要替这条规则辩护** —— 它不是随便定的，是三个已有前提推出来的：

1. `a == b` 天然会被用在 `if` / `while` / `&&` / `||` 里
2. extC **没有隐式真值转换**（`if 1` 是错的 —— 这是早就定下的，C 的真值转换是经典 bug 源）
3. `!=` 是靠 `==` 取反实现的

⇒ 所以 `==` 只能是 `bool`。**除非放弃 ② 或 ③，否则没有中间地带。**

**反例也支持这个选择**：Python 的 `__eq__` 可以返回任何东西，
代价就是 numpy / pandas 那个著名的 `truth value of an array is ambiguous` ——
用户写 `if a == b` 却得到一个数组，然后在一个完全无关的地方炸。
Rust（`PartialEq::eq -> bool`）和 Haskell（`(==) :: a -> a -> Bool`）都定死在 `bool`。

**但主人「想要别的定义」这个诉求是合理的**，所以给出口：
**换个方法名就行**。

```extc
fn compare(self: ref point, other: point) -> ordering { ... }   // 随便返回什么
fn diff(self: ref point, other: point) -> diff { ... }           // 随便返回什么
```

**只有 `==` 这个名字被绑定了 `bool`**，因为它在源码里的位置决定了它是「一个条件」。

顺手把错误信息的 note 改成**讲理由 + 给出口**：

```
error: operator `!=` must return `bool`
  note: `a == b` gets used in `if` / `&&` / `||`, and extC has no implicit truthiness;
        `!=` is also derived by negating `==`.
        To return something else, use a different method name (`compare` / `diff` etc.).
```

> 一条约束如果说不清「为什么」，它就是武断的；说得清，它就是设计。

---

## 2026-09-18 · 主人抓到第二个洞：运算符签名检查放错了位置

主人问：

> 「`!=` 真的可以退回吗，万一我的相等返回值是 `void` 或者是别的不能取反的东西呢，那不是炸了」

**验下去发现真的有洞**，而且比预想的更宽：

1. 定义了签名错误的 `fn !=` 但**没直接用到** → 一直没人查（编译器安静地生成 C）
2. 泛型里 `a != b` 推迟到实例化，codegen 找到了那个 `void` 的 `!=`
   → **直接把 C 的 `invalid use of void expression` 漏给了用户**

根因：**运算符签名只在「使用处」检查**。所以「没被用到的定义」和「绕过使用处的路径」
都能漏过去。

**修法：把签名检查挪到「定义处」**（`checkOperatorSig`，在 `checkMethodShape` 里对所有方法跑）。
于是无论从哪条路径使用它都是安全的，而且**错误指向定义那一行**，比指向某个使用点好得多。

顺手加了六条反例：`op_ret_void` / `op_ret_wrong` / `op_arity` / `op_param_type` /
`op_self_value` / `op_free_fn`。

> **教训（可推广）：约束要在「产生」的地方检查，不要在「消费」的地方检查。**
> 在消费处检查 = 检查的覆盖面取决于有多少消费点、以及消费点走哪条路径 ——
> 那是一张漏得很有创意的网。

**测试** 32 → 38 个。

---

## 2026-09-18 · `==` 改成**显式定义 `fn ==`**（主人第二次提案，改对了）

主人先问「为什么不能早点支持 operator==」，奶昔做了——但**第一版用的是「约定方法名 `eq`」**。
主人立刻指出问题：

> 「现在用户要知道 `eq` 是 `==`，那不是很难受吗」

**这个批评是对的，而且它违反的是 extC 自己的原则**：
「`==` 去找一个叫 `eq` 的方法」是**隐藏约定** —— 读代码的人看到 `p1 == p2`
不知道行为从哪来。这跟「不能证明的，语法上必须看得见」是打架的。

**改成让用户直接把 `==` 写出来**：

```extc
struct point {
    x: i32
    y: i32

    fn ==(self: ref point, other: point) -> bool {     // ← 不查约定名
        return self.x == other.x && self.y == other.y
    }
}
```

- C 里拼成 `point_eq`（C 的标识符不能叫 `==`）—— 加了 `cSymName` 做这层映射
- `!=` 可以单独定义；不定义就退回用 `==` 取反
- **`fn ==` 必须写在 struct 体内**（自由函数不能定义运算符）
- **错误信息变成自解释的**：
  - `` `tag` does not define `==`, so it cannot be compared `` + 「在 `tag` 里定义它即可：`fn ==(self: ref tag, other: tag) -> bool`」
  - `` `thing.==` has the wrong signature ``
  - `` `wrapper_tag` needs `tag` to define `==` ``

**教训：一个「约定名字」看起来省事，但它把语义藏起来了 ——
而 extC 的全部卖点就是「不藏东西」。宁可多打四个字符（`fn ==` vs `fn eq`），也不留隐藏约定。**

（实现只多了一个 `expectFuncName`：函数名位置接受标识符或可重载运算符 token。）

**测试** 32 个全过。

---

## 2026-09-18 · `==` 支持（主人要求提前做）+ 工具改用 C

主人问「为什么不能早点支持运算符重载 operator== 这种」——
**问得对，奶昔之前把 `==` 排到 T4b 之后是判断失误。它现在就能做，而且应该现在做**
（因为它把之前那个「`str ==` 变指针比较」的临时补丁换成了**真规则**）。

**实现**：`==` / `!=` 是语法糖，不是万能比较。

| 两边 | 生成 |
|---|---|
| 数值 / bool / 枚举 | C 的原生 `==` |
| struct / 泛型实例 | 找 `eq` 方法 → `Type_eq(&a, b)`；**没有就报错** |
| `str` | **拒绝**（它的 `==` 是指针比较） |

泛型里含类型参数的比较**推迟到实例化**再检查（`Expr.needEq` + `Checker.eqChecks`），
错误信息会点明是哪个实例：`` `wrapper_tag` needs `tag` to have an `eq` method ``。

### 踩到的三个坑

1. **codegen 不知道内建类型原生可比** —— check 的推迟复查通过了 `i32`，
   但 codegen 生成时又去找 `eq`，于是 `wrapper<i32>` 报「int32_t 需要 eq 方法」。
   codegen 也得有一份"原生可比"的判断。
2. **`cFuncName` 带的是「当前正在生成的实例」前缀** —— 在 `wrapper<point>` 里调
   `point.eq`，它拼出了 `wrapper_point_eq`。**被调用的方法属于别的类型，必须用
   `f->owner` 而不是 `g->ownerPrefix`**。为此把方法名解析单独抽成 `cMethodName`。
3. **`eq` 的 `other` 按值传更顺手** —— 取 `ref` 时调用点的实参要写 `ref`
   （方法参数的规则），所以 `same(self, other: wrapper<T>)` 比 `other: ref wrapper<T>` 好用。

### 顺带：工具改成 C

主人提醒「不要使用 python」—— 对，尤其这是 C 程设作业。
`tools/embed.py` 改成 **`tools/embed.c`**（把文件嵌成 C 字节数组，给 T4b 的 prelude 用）。
好处：**make 只需要一个 C 编译器**，不引入任何解释器依赖。
这个工具跟编译器一样跑一次就退出，所以也**不手动 free**。

**测试**：28 → 32 个（新增正例 `examples/eq.extc` + 三个反例：
`eq_missing` / `eq_deferred` / `eq_bad_sig`）。

---

## 2026-09-18 · 泛型约束定案 + prelude 说清楚

主人对 `pair<A,B>.swap` 写不出来这件事的判断：**「因为类型是静态检查的，除非重载运算符否则
都不应该支持，我觉得限制是对的。」** —— 采纳，并写进 DESIGN：

- **不做运算符重载**（用户可扩展的多态 = 一大片特性面）
- **不做 trait / interface 约束**（同上，而且是运行时多态的入口，违反 P）
- **约束靠签名表达**：需要两边同类型就用一个参数 `struct pair<T>`

同时把 **prelude** 的定义写清楚（奶昔之前用黑话没说清）：

> prelude = 每次编译都**先于用户文件**被 parse + check 的一段 extC 源码，用户不用 import。
> 存在理由是「能在 extC 里写就在 extC 里写」：没有它，`slice<T>` 只能在 codegen 里硬编码
> （❌ 把库塞进编译器）；有了它，`slice<T>` 就是一个用 extC 写的 prelude 类型（✅）。

### ⚠️ 顺带暴露一个真缺口：需要「T 有某种能力」时怎么办

`map<K,V>` 的 `find`（要 `K` 能比较）、`slice<T>.indexOf`（要 `T` 能比较）——
模板检查时不知道 `K` 能不能比。三个出路（推迟到实例化 / 具体类型专用 / 最小能力约束）
记在 DESIGN 里，**T4b 先走「具体类型专用」**（`slice<u8>` 是眼下的真实需求），
`map<K,V>` 真需要时再定。

---

## 2026-09-18 · T4a：泛型 + 单态化通了

**做了什么**：`struct Name<T> { ... }` 声明 + `Name<Args>` 使用 + 单态化。
一份 extC 源码 → 编译器按实例生成 N 份 C。

```
pair<i32, u8>    →  pair_i32_u8_getFirst / pair_i32_u8_getSecond
pair<bool, point> →  pair_bool_point_...
box<i64>         →  box_i64_set / box_i64_get
box<point>       →  box_point_set / box_point_get
```

四份实例同时工作，嵌套 struct、自动调试打印都对。

**设计**：check 只对**模板**检查一遍（用 `TY_PARAM` 表示 `T`）；
codegen 在「实例上下文」里把 `T` 换成实参。实例**驻留**（全局一份）。

### 踩到的三个坑（都不是小坑）

1. **参数化实例混进了实例表** → codegen 去生成 `box_T_set` 这种东西。
   检查 `ref pair<A, B>` 时会产生「实参是类型参数」的实例 —— 那只是拿来比类型的，
   **绝不能进实例表**。修法：只有**完全具体**的实例才驻留。
2. **泛型实例的字段必须用它自己的实参替换，不能用环境里碰巧留着的上下文。**
   踩得很惨：`zeroValue` 里 `subst` 在没有上下文时原样返回，于是它自己递归自己 →
   **栈溢出**，而且 ASAN 报的是「stack-overflow in zeroValue」这一行重复几十遍。
   **教训：泛型相关的地方，「用谁的上下文」必须显式进出配对，不能靠环境。**
3. **实例结构体必须排在普通 struct 之后**（`box<point>` 的字段是 `point`）；
   而且 `_debug` 要先出原型（实例和普通 struct 会互相递归打印）。

另外还漏了一次：`cType` 只在「裸 `TY_PARAM`」上做替换，但 `ref pair<A, B>` 外层是 `ref`
—— 必须**整体**替换一次。

### 发现的真限制：模板检查的代价

`pair<A,B>` 里的 `swap`（`self.first = self.second`）**写不出来** ——
模板里 `A` 和 `B` 不确定相等，checker 只能报 `expects A, found B`。

要写这种操作就**用一个参数**：`struct pair<T> { first: T  second: T }`。

这是「检查一遍、错误只报一次、错误指向模板」的代价；反面是 C++ 那种实例化时才炸。
**这是一条要写进 MANUAL 的已知限制。**

**测试**：27 → 28 个（新增正例 `examples/generics.extc`）。

---

## 2026-09-18 · 搬迁审计：编译器里哪些该用 extC 写

主人问「前面有什么东西适合用 extC 写」。写了 `MIGRATION.md`，逐函数体检。

**先定了机制**（这是关键）：**两阶段构建 = 部分自举**
```
① 纯 C 构建 extc  →  ② 用 ① 编 stdlib/*.extc  →  ③ 链进 extc
```
于是最终编译器的一部分**已经是用 extC 写的**，而且是自举的直通路。
附带好处可能是最大的：**stdlib 每改一次都被「用 extC 编 extC」这个循环检验一遍**。

**审计结果**：约 15 处适合搬，3 处首选。
- ✅ 适合搬：`ctxRenderDiag`、字符分类、各数据表、`ttCanWiden`+整数表、`ttEquals`/`ttRender`、
  `find*`/`lookup`、`cType`/`zeroValue`/`genPrintValue`、`Buf`/`Vec`、**整个词法器**
- ❌ 留：`Arena`（内存模型本身）、`Ctx` 形状、AST 遍历骨架、`main` 的文件/进程 IO

**但诚实的结论是：现在几乎什么都搬不了。**
因为 extC 还没有**数组、索引、字符串操作、泛型** ——
想搬 `ttCanWiden` 要字符串相等，想搬字符分类要 `s[i]`，想搬 `ctxRenderDiag` 要字符串拼接。

> **所以 T4 不只是为了写五子棋 —— 它同时是「把编译器自己搬进 extC」的开门钥匙。**

### 审计中抓到一个真 bug：`str == str` 是**指针比较**

`if a == b` 生成的 C 是 `const char *a; const char *b; if ((a == b))` —— 比较的是指针。
它「看起来对」只是因为 **gcc 把相同内容的字面量折叠到了同一地址**（默认行为）。
一旦字符串能从别处来（参数、拼接、文件），会立刻判不等，**而且错得无声无息**。

处置：现在 `str` 的 `==`/`!=` **直接报错**，而不是安静地给错答案。

> 通用原则：**语言层承诺的语义，生成的 C 必须真的实现 ——
> 让 C 的默认行为偷偷替代，就制造了一个未来的陷阱。**

---

## 2026-09-18 · 自动调试打印 + 一条新原则 + 三个 `str` 零值 landmine

**主人定了新原则：「能在 extC 里写的东西，就在 extC 里写」**（除非很难 / 很影响效率）。
把它写进 `DESIGN.md`，并划了分界线，其中两个易混点单独说明：

1. **编译器「生成」代码 ≠ 把库塞进编译器。** `<Type>_debug` / `<Type>_name` 是前者 ——
   等价于 Rust 的 `#[derive(Debug)]`。extC 没有反射，「遍历所有字段」这句话在语言里
   **写不出来**，只能由编译器生成。而 `array<T>.push` 是后者，它写得出来。
2. **这条原则直接决定 T4 怎么设计**：泛型不应该意味着把容器硬编码进 codegen，
   而应该是「**容器用 extC 源码写，编译器按实例生成 C**」。

**自动调试打印完成**：每个 struct 生成 `<Type>_debug`，递归打印所有字段
（嵌套 struct 递归、枚举打名字、零初始化也能直接打）。

```
point { x: 3, y: 4 }
player { name: naixi, score: 100, alive: true, pos: point { x: 3, y: 4 }, color: green }
```

### 一次测试揪出三个同源的 landmine

`println` 一个零初始化的结构体，打出 `name: (null)`。顺藤摸下去发现是**同一个根因的三个分身**：

> `str` 是**不可为空**的，但 C 里 `const char *` 的「零」是 `NULL`，
> 而 `printf("%s", NULL)` 是 **UB**（glibc 恰好打 `(null)` 骗过你）。

1. `var s: str` 零初始化 → NULL（先修了这条，但没修好）
2. `var p: player`（含 `str` 字段）→ `{0}` 把字段也变成 NULL ← **真正的根因**
3. **`player { score: 7 }` 这种省略字段的字面量** → C 自动零填充，同样产生 NULL

修法：零值要**递归**算 —— 含 `str` 的 struct 逐字段写出零值（不含的仍用 `{0}` 省事）；
结构体字面量**所有字段都写出来**，省略的填零值，不让 C 去零填充。

**教训：语言层说「不可为空」，生成的 C 就必须真的不可为空 —— 不能让 C 的「零」偷偷变成空指针。**
extC 的每个不方便之处都可能藏着一个 C 的 UB，这类地方要主动去搜。

**测试**：26 → 27 个（新增 1 个正例 `examples/debug.extc`）。

---

## 2026-09-18 · 调试策略

主人问「调试怎么办，C 最搞笑的一个东西就是 gdb」。

**先承认 gdb 的「搞笑」是哪几处**：要另开工具、另学心智模型、要记得 `-g`；
一开优化变量就 `<optimized out>`；有 UB 时断点停在意想不到的地方；
`p *ptr->next->data` 跟源码长得完全不一样。

**extC 的调试哲学跟安全哲学同源**：调试图难的根源是「运行时才暴露的东西」，
而 P 已经把能证明的都挪到编译期了 —— **剩下需要运行时观察的东西本来就少得多**。

定了三层：① 挡在编译期（好诊断，已有）② 挡不住的在运行时留最好的现场（trap 报告）
③ 剩下的交给 gdb，但让它看到的像 extC。

**相对 C 最大的一条改进**：C 里越界是 UB（可能什么都不发生、可能崩在几十行外、
可能被优化掉）；extC 里它是**带源码位置、带值、带调用链的 trap**。

**四件便宜且高价值的事**：
1. **自动调试打印** ← 最值钱。定案 11 已给枚举生成 `<Type>_name`，同一机制给 struct
   生成 `<Type>_debug`（**递归**打印字段），于是 `println(s)` 对任何类型都能用。
2. trap 报告像语言的一部分，不是段错误。
3. `--checks=all` 调试模式：release 消掉能证明的检查，调试模式一个不消 + 打开契约断言。
   就是 Rust `debug_assertions`，但这里更自然 ——「能证明 / 不能证明」本来就是编译器的内部状态。
4. 把「生成的 C 对调试器友好」写成原则：不用宏、不做内联花招、布局 1:1、变量名保持原名。
   → extC 生成的 C 天生是**「优化过的调试构建」**，这在 C 生态里是奢侈品。

**明确不做**：自建调试器（编译到 C 白送 gdb/lldb）、REPL（跟编译到 C 冲突，
但改一行重编重跑已经是毫秒级）。

---

## 2026-09-18 · 多线程模型：主人提案 → 定案

主人提了一个模型：**保守** —— 值被别的线程用着时谁也碰不了，除非那个线程被释放；
要用就得把修改**传回父线程**合并（他猜是乐观锁）。

**方向是对的，而且它整个可以静态检查** —— 这是关键：

| 规则 | 靠什么静态保证 |
|---|---|
| 值送进线程要**移动** | 移动语义（值语义的延伸） |
| 线程活着时父线程**看不见**它 | 词法作用域（跟 arena 同一套机器） |
| 线程结束后拿回结果 | 结果落在父线程 arena |
| 两个线程不能同时持有同一个值的 `ref` | **引用不过线程** |

于是**零运行时簿记** —— 完全符合 P。

**但「乐观锁」那一半被劝退了。** 乐观锁要在冲突时重试，前提是**知道冲突了**，
也就是必须**跟踪读集**；而读集跟踪是运行时簿记，**直接违反 P**。
（这其实就是 STM，软件事务内存 —— 它难做正是因为这个。）

**换成「归约」**：每个 worker 产出一个值，父线程用一个**显式给出的合并函数**合起来。

- 不需要冲突检测（用户写的不是冲突处理，是合并函数）
- 不需要读集跟踪（零运行时簿记）
- 合并函数可结合可交换时，**结果与线程数、调度顺序完全无关**

最后一条是最大的胜利：

> **1 个线程和 8 个线程跑出来的结果一模一样，只是快慢不同。**
> 这杀掉了一整类「有时候对有时候错」的 bug —— 而这类 bug 在 C 的线程里是常态。

**第一个真实用户是现成的**：五子棋的根分裂并行搜索（每线程搜一个子树，最后取最大）
就是标准 fork-join + 归约，天然确定。

**一条纪律**：worker 里做纯计算，有副作用的活（I/O）留在父线程 —— 不可重放的东西没法归约。

---

## 2026-09-18 · 两个问题：异常要不要？多线程要不要？

**问题一：有必要写 try/catch/throw 吗？**

**不需要 —— 而且理由比「违反 P」硬得多。**

异常存在的理由只有两个，而这两个在 extC 里都已经没了：

1. **不用改签名就能往上抛** → 被 `?` 干掉了（一个字符、可见、零成本、静态解析）
2. **出错时自动清理资源** → **被 arena 干掉了**。这是关键：函数返回 `Err`，它的帧 arena
   照样词法结束、一起释放。**异常最核心的卖点在 extC 里直接蒸发。**

更根本的一条：**异常和 arena 天生冲突。** `throw` 穿过一个 `region` 块时谁来释放那个 arena？
要么给每层栈帧加 unwinding cleanup（运行时簿记 ⇒ 违反 P），要么泄漏。

顺带定了一条区分（很多语言混在一起是错的）：

| | 用什么 | 例子 |
|---|---|---|
| 可恢复的失败 | `result<T,E>` + `?` | 文件不存在、落子位置被占 |
| **bug** | **trap**（直接崩） | 数组越界、除零 |

把 bug 也做成可捕获的异常，会让「我懒得处理」伪装成「我 catch 一下就算了」。

**待定项被暴露出来**：`?` 要求错误类型兼容。`fn f() -> result<T, E1>` 里 `?` 一个
`result<U, E2>` 需要转换，Rust 用 `From` 解决。extC 现在没有 `From` —— 这是个真缺口。

**问题二：多线程要不要？**

**不做**（两个真实目标都是单线程的）。**但记下一条现在就要守住的红线 —— 引用不过线程。**

`ref T` 绑在 arena 上、arena 绑在词法作用域上、词法作用域活在某个线程上。
守住这条，将来加线程是**加法不是重写**，还白拿一个很强的保证：

> **共享 = 拷贝。要跨线程就得拷贝。引用跨不出去 ⇒ 数据竞争在类型层面不可表达。**

这正是 Rust 用 `Send`/`Sync` 费力达到的，而 extC 靠「值语义 + arena」自然得到。
将来只做**消息传递 + 值拷贝**，永远不做共享内存 + 锁。

---

## 2026-09-18 · T3 + 顺手三条：`ref` 表达式 / 零初始化 / 方法进 struct / `type` 枚举

**做了什么**：
- **零初始化**（定案 8）：`var b: board` 合法，自动清零。C 里最大的 UB 来源之一
  （读到未初始化内存）**从语言里消失**。`ref T` 不能零初始化 —— 它是不可为空的引用。
- **方法写在 struct 体内**（定案 9）：parser 在 struct body 里允许 `fn`；check 按接收者
  类型找方法；codegen 做 C 名字修饰（`point_eq`）。**自由函数带 `self` 现在直接报错**。
- **`type` 枚举**（定案 11/14）：`type status = | ok | warn | error`，`status.ok` 访问变体；
  **枚举自动有名字文本**（codegen 生成 `<Type>_name`），`println(s)` 直接可用。
- **`ref` 升级成表达式**（定案 10 / T3）：`ref x` 是表达式；自由函数的 `ref T` 实参
  必须在调用点写 `ref`；方法接收者自动取地址。`ref` 是**可变**引用，所以不能对 `let` 取。

**意外的好收益**：方法进 struct 之后**方法名有命名空间了** —— `point.eq` 和 `board.eq`
是两个不同的名字，顶层不用为了避冲突而发明 `point_eq`。这是对「丑名字」的系统性改善。

**一个必须记的坑（浪费了半小时）**：
改 `ast.h` 让 `sizeof(StructDef)` / `sizeof(FuncDef)` 变了，但 **Makefile 没有头文件依赖**，
只有部分 `.o` 重编 → **新旧 ABI 混在一起 → 段错误**。而且症状看起来完全像代码 bug：
`var p: P = { x: 1 }` 报「推导不出类型」、`examples/structs.extc` 直接崩。

定位过程值得记：ASAN 下**不崩**（只报 arena 的「内存泄漏」—— 那是设计如此，从不 free）；
单独用 `-O0` 编译也能正确输出；只有 `-O1` 的增量构建崩。
最后 `make clean` 一下全好了。

**修法**：Makefile 加 `-MMD -MP` 自动生成头文件依赖。
**教训：「代码看起来对但行为诡异」时，先怀疑构建系统。**

**测试**：19 → 26 个（6 正例 + 20 反例），全过。

---

## 2026-09-18 · T1 + T2 落地：类型检查独立成 pass

**做了什么**：
- 新增 `src/types.[ch]`（**类型表**）。类型是**驻留（interned）**的 —— 一个类型只有一份实例，
  所以「类型相等」就是「指针相等」，validate 全部退化成指针比较。
- 新增 `src/check.[ch]`：一遍**独立的类型检查 pass**，把结果写回 AST
  （`Expr.type` / `Expr.func` / `Expr.field` / `Stmt.type`）。
- `codegen.c` 里**删光了类型推理** —— 作用域跟踪、名字解析、参数个数检查全部搬走，
  它现在只负责翻译。文件反而短了一截。
- parser 不再碰类型表：它只造 `TY_UNRESOLVED` 的「类型名」，由 check 解析。

**为什么**：类型检查寄生在代码生成里的话，每加一个特性两边都要改，而且会互相打架。

**T2 顺带做完**：`let x: i32 = "abc"` 以前漏给 gcc，现在由 extC 自己报错。

**落地的规则**：
- 只自动做**无损失**的拓宽，收窄一律禁止
- **字面量按值适配**（`let x: u8 = 200` 可以，`= 300` 报「literal `300` does not fit in `u8`」）
- 没有隐式真值转换（`if 1` 报错）
- 没有公共类型的运算（`u32 + i32`）报「no common type」

**踩的坑**：局部变量声明的类型标注忘了解析（`let x: i32` 里的 `i32` 还是「未解析的名字」），
一个根因炸出三处报错，而且症状看着像「i32 不等于 i32」这种鬼话。
**教训：parser 造「名字」、check 解析「名字」这个分工，要在每个入口都记一遍。**

**测试**：10 个 → 19 个（4 正例 + 15 反例），全过。

---

## 2026-09-18 · week-1 计划定案

**做了什么**：定了 week-1 的范围 —— **类型层地基，一行语法糖都不碰**。

**为什么**：主人问「哪个最好用且最有价值」，并给出了直觉「类型安全优先级比较高，否则后面很难改」。
奶昔把这个直觉精确化成一句话：

> **能随时加的是「语法」（数组、`for`、`match`、模块）；改起来贵的是「类型的形状」（泛型、`ref`、`result`/`option`、值语义）。**

数组和 `for` 加错了改一星期；泛型和 `ref` 的语义长歪了，所有基于它的代码全得重写。

**产出**：`PLAN.md`（T1 拔类型信息 / T2 真类型检查 / T3 `ref` 升级 / T4 泛型 / T5 `option`+`result`）。

---

## 2026-09-18 · 五子棋示例代码审读 → 第二批定案

**做了什么**：主人给了一份完整的五子棋示例（`board`/`errors`/`sandbox`/`ai`/`main` 五模块），逐行读完，写出 `REVIEW-gomoku-sample.md`。

**最大收获**：数整份代码里**看得见的安全机制**只有 `ref` 一处，**机制痕迹**只有 `region search` 一处 —— 其余全是普通高层写法。
**主人要的是一门「表面普通的语言」，安全全沉底。** 这回头解释了最早那句「这个 idea 有点丑」：丑的不是糖多，是文档里 `VarArray`/`wString` 那几个名字和「四个替代」的腔调。

**定案 5 条**：格式串（编译期展开）、默认零初始化、方法进 struct 体内、`ref` 调用点显式、枚举自动有名字文本。

**发现 4 个**：

1. **格式串其实可以不违反 P** —— 违反 P 的是「运行时解析」，不是「有花括号」。编译器编译期展开就没有运行时解析。
   而且它**比多参数顺序打印更安全**：`{}` 个数和实参个数能在编译期检查。
2. **这段代码正好证明「arena 必须是词法的」** —— `alphabeta` 每个节点都调 `candidates()` 分配数组。动态 arena 会让它们全部活着（O(节点数)，直接爆）；词法 arena 是 O(深度×分支)。
   顺带收紧 `region` 的定义：它**只是给一个词法作用域起名字**，不是「子树 arena」。
3. **发散臂** —— `match` 是表达式，但示例里有臂是 `continue`。类型检查器必须要有 never 类型。
4. **示例里的 `sandbox` undo 栈跟主人自己的 gomoku DESIGN v1.3 矛盾** —— v1.3 明写「全项目零 unmake，拷贝沙箱替代 make/unmake」。这份是 AI 按通用套路写的。
   **教训：同一个主人，对着 AI 说的和写在设计文档里的是两套，要以认真复审过的那份为准。**

### P 的两处重要澄清（本日最值钱的产出）

**① P 禁的是「隐藏的机制」，不是「数据拷贝」。**
界线：**这件事是不是用户看不见、也无法从源码推断的机制？**

| 例子 | 判定 |
|---|---|
| GC / 异常展开 / vtable | ❌ 违反（看不见时机、控制流、调用目标） |
| 运行时解析格式串 | ❌ 违反 |
| 结构体拷贝 / 零初始化 | ✅ 不违反（**值语义是语言规则，不是隐藏机制**） |

不这么划，P 会连 `x + y` 都禁掉。

**② 零初始化是 P 的胜利。**
C 最大的 UB 来源之一是「读到未初始化内存」，它**只能在运行时发现**；默认清零把它变成**编译期就能保证**的东西。代价是 memset，而编译器还能在能证明「马上会被完全覆盖」时消掉它 —— 那就是「能证明的不留痕迹」在起作用。

---

## 2026-09-18 · week-0 实现（改用 C）

**做了什么**：把编译器从 Python 改成 **C**（因为这是主人的 C 程设作业，而且 C 版才有可能变成 extC 版）。
`src/` 8 个文件，全链路 `extC → C → gcc → 可执行` 跑通，`tests/run.sh` 10/10。

**关键决定**：写这份 C 的时候**按 extC 的规矩来** —— arena bump 分配、从不 free、无 static 可变全局（状态全挂 `Ctx` 显式传递）、函数 camelCase / 类型 PascalCase。
于是这个编译器成了 **extC 设计的第一号用户**：那些规矩在真实程序里好不好用，写的时候当场就知道。

**三个发现**：

1. **arena 模型很好用** —— `Ctx` 传着走一点也不烦，不用想 free 太爽了。（主人五子棋引擎里的 `SearchCtx` 果然是同一路写法。）
2. **arena 的代价也暴露了** —— `Vec`/`Buf` 增长时旧块**还不了**，只能留给 arena。编译器无所谓，但这是真代价：`array<T>` 要不要 `reserve(n)`？
3. **「不建 AST」在写完第三行代码时就撞墙了** —— `typeOf` 必须在表达式生成阶段可用（`println` 按静态类型选格式、字段访问要判 `.` 还是 `->`、`let` 检查要可变性），这些信息只在符号表和类型里，也就是只在「建了 AST」的世界里。
   > 注意措辞：**AST 本身不难**，难的是绕过它之后那些检查没处放。
4. **抓到一个真 bug**：`#line` 的文件名打成了 `(null)`（`CG.path` 忘了赋值）。修完之后 gcc 的报错能准确指回 `.extc` 的第 3 行并带插入符。

---

## 2026-09-18 · 语法决策第一批（6 条）

分号（换行即结束）、类型标注（局部可省 / 签名与字段必须写）、字符串字面量（`slice<u8>` 视图）、全局变量（加，深度 0 的 arena）、常量与枚举（`const` + 简单 `type`）、程序入口（`@main` 注解）。

**连带影响**：`slice<T>` 要泛型 ⇒ 泛型变 week-1 必做；`str` 作废 ⇒ `println` 要认识 `slice<u8>`；`@main` ⇒ 代码生成要产出 C `main` 当 shim。

---

## 2026-09-18 · `@recursive`：把调用栈变成编译期数据

主人补了一条 AI 没写的特性：`@recursive` 让编译器把递归展开成显式栈。

**奶昔的判断：它不是「帅」，它是 P 的直接推论** —— 调用栈是隐藏的运行时机制（深度由 OS 决定、溢出是运行时崩溃），把它变成显式数据正是 P。

**两个漂亮的地方**：

1. **静态上限买到一整条链**：深度上限是编译期常数 ⇒ 帧数组是编译期定长 ⇒ 递归全部内存在编译期确定 ⇒ 零堆、零分配、不可能 OOM。帧数组连堆都不用进。
2. **它没有引入任何新语义**：展开后就是「定长帧数组 + 循环 + 下标」⇒ **递归深度 = 数组下标**，深度溢出 = 数组越界，完全复用已有机制。

**主人贡献了一个好点子**：`@recursive` = 编译器展开，`@recursive!` = 真递归。
奶昔发现这**把 `!` 升格成规则了**：`!` = 「这里我签字，接受运行时的后果」（`a[i]!` 和 `@recursive!` 同一个含义）。于是 `!` 成了 **P′ 的语法化身**，而且**危险可以在源码里数出来**（`grep -c '!'` 就是一次逃生舱审计）。

**v2 范围**：只做「直接递归」和「递归调用在循环里」两档 —— 后者正好覆盖五子棋的 VCF/VCT。互相递归**报错，不静默退化**。

---

## 2026-09-18 · 问诊 + 命名规范纠正

**主人的回答推翻了奶昔一个判断**：

| 奶昔以为 | 实际 |
|---|---|
| 主人喜欢 C 语法，不喜欢 Go/Rust 味 | ❌ 主人**喜欢 Go**，Rust/Go 骨架没问题 |
| 「不喜欢语法范式」= 嫌语法风格 | ❌ 指的是**命名规范** |

主人的口味：`var thisIsAGoodName: i32` —— `name: Type`、camelCase、PascalCase 类型、`i32` 不用 `int`。
所以 v0 丑的地方**不是骨架，是名字**：`VarArray`（`Var` 是废话还撞关键字）、`wString`（Win32 遗留）、`is_empty`（该 camelCase）、`box<[T]>`（双重间接且缺 `cap`）。

**另一个重要背景**：主人是 **OI 出身** —— 习惯全局数组、不手动释放内存、重视性能；也因此知道 C 的 `mem*`/`str*` 很快（虽危险）。
这条直接判死了 v0「不做 C 互操作」：要 `memchr` 级别的速度就必须能碰 libc。
**定案：标准库实现可以调 libc（编译器生成），用户代码不能。**

**关于 `static`**：主人说它「sb」，但后来承认不太清楚它干嘛（OI 很少用）。
奶昔解释了 C 的 `static` 有三层意思，被骂的是中间那层（**函数内静态变量 = 活得比函数调用久的局部变量**，签名上看不出来的隐藏状态）。
**extC 的答案（推导出来的，不需要新规则）**：全局 = 深度 0 的 arena ⇒ ① 不需要 `static` 关键字 ② 引用规则自动禁止全局存局部地址 ③ 真正危险的那个 `static` **在语法上不可达**（局部的作用域就是它的 arena）。

**另一条纠正**：主人说「不建 AST」，奶昔上一版判它出局。
问清楚之后发现原话是「**不解析整个 C**」—— 这句话里揉着两件事，被 AI 总结时扩大了：
- 「不解析 C」✅ 保留（透传的 C 不用解析）
- 「不建 AST」❌ 出局（被检查的是 **extC**）
**不解析 C ≠ 不解析 extC。**

---

## 2026-09-18 · 设计收敛：10 条原则 → 1 条

主人说「这个 idea 有点丑」。奶昔找到病根：**这门语言是用否定句定义的**（v0 的定位是四个「替代」），像一份对其他语言的投诉清单，不像一门语言。

**手术结果**：把 v0 §11 的 10 条原则收敛成**一条**：

> # P：凡是编译期能证明的，运行时不留痕迹。
> ### P′（逆否）：不能证明的，语法上必须看得见。

v0 的十条里，**三条是 P 的化身，两条是 P′ 的化身**；剩下五条不是原则，是预算、策略、审美和手段。

**最大的结构发现**：v0 把「arena / 作用域释放」和「逃逸检查」当成两个设计块，其实 **arena 不是第二条原则，是 P + 「内存自动回收」这个前提的推论**（P″）。一条规则吃掉 v0 §3 的三套机制（局部变量/box/region 全都是 arena）。

**顺手解决**：
- **全局变量** = 深度 0 的 arena（见上）
- **返回值**：引用规则自动禁止返回指向自己栈帧的引用 ⇒ 要返回引用就必须由调用者提供 arena。**这不是我们挑了 Zig 的风格，是规则不允许别的写法。**
- **越界**：范围类型让可证明的索引零开销。⚠️ 但这条**当场被五子棋证伪了一次** —— 初版写「不可证明就编译错误」，而五子棋满屏 `b.cell[py][px]`，全是运行时算出来的索引，那样五子棋根本写不出来。改成「`a[i]` 带检查（可证明则消除）」，范围类型从**门槛**降级成**优化**。

**判出局**（v0 里删掉的东西）：不建 AST、`From`/`Into`、`string<T>` 泛型、`wString`、`substr`、`to_bytes`、`VarArray`/`hashMap` 这些名字、全部 6 种转换里的冗余……

---

## 2026-09-18 · 项目起点

主人的动机：**「我要在 C 程设期末大作业写五子棋人机对战，现在是学期初，我闲得慌，而且 C 实在太不安全了，我感觉我会写出一大堆 bug。」**

奶昔据此把第一目标从「四个月自举」改成 **G2：能写出五子棋**。自举退成第二个目标。
理由：五子棋要的东西比自举少得多；而且验收标准变得**近、可测、就是主人本来要做的事**。

⚠️ 同时提醒了一个硬冲突：**期末只剩十几周，语言不可能赶上作业**。两者必须分开 —— 五子棋照原计划用 C 写，extC 是并行的副线。
---
