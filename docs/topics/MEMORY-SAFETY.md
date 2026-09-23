# MEMORY-SAFETY.md —— extC 的内存安全模型（怎么用 + 什么情况会怎样）

> **读者**：写 extC 的人（要判断"我这么写行不行"），以及想知道"编译器到底保证了什么"的人。
> **口径**：本文的每一条**都有实测用例**，而且**可机器复核** ——
> `bash tools/memsafe/run.sh`（33 条，逐条打印"期望 vs 实际"）。用例源码在 `tools/memsafe/qa/` ✓
> **一句话**：**没有 GC，没有 `free`，也没有悬垂指针** —— 前两个是设计，第三个是编译器保证的 ✓
>
> 相关文档：设计推导 [`ARENA.md`](ARENA.md)（含证明）· 形式化 [`ARENA-FORMAL.md`](ARENA-FORMAL.md) ·
> 被证伪过的尝试与判据 [`ARENA-NOTES.md`](ARENA-NOTES.md) · 语法 [`REFS.md`](REFS.md) ·
> **不在这里讲**：`varArray` 等标准库容器怎么用（见 [`LIBS.md`](LIBS.md)）✓

---

## 1. 三句话说完模型

1. **每个 `{}` 一块 arena。** 进块时它是空的，出块时**整块一起还掉**（不是逐个对象）✓
2. **分配只有两种**：`new`（堆上、零初始化）和栈上的普通局部变量（`var x: T`）✓
3. **编译器替你判断"能不能带出去"**：
   带得出去就**允许**（必要时把这块 arena 提升到更长寿的层），
   带不出去就**编译期报错**，并告诉你是哪一层的问题 ✓

> ⭐ 关键性质：**你永远不会拿到一个"已经开始悬垂"的引用** ——
> 要么编译器拒绝你，要么这块内存活到你需要的时候 ✓

---

## 2. 内存从哪来：`new` vs 栈

| 写法 | 住哪 | 零值？ | 谁能看见它 |
|---|---|---|---|
| `var x: T` | 当前块的栈帧 | 必须自己初始化（引用型必须给初值）| 本块内 |
| `new T` / `new [N]T` / `new T[n]` | 当前块的那只 **arena** | ✅ **一定是零**（运行时 memset）| 按 §3 的规则 |
| `alloc<T>(n)` `allocSlice<T>(n)` | 同上（内建原语）| ✅ 零 | 同上 |

- **`new` 出来的东西不在栈上**，所以它**可以活过本块** —— 这正是它存在的理由 ✓
- **栈上的东西一律不能活过本块**（编译器不让）✓
- `new` 的四种形状：`new T` · `new [N]T`（定长数组）· `new T[n]`（n 编译期不要求已知）·
  `new [N]T` 与 `allocSlice<T>(n)` 的区别只在"要不要 `mut slice` 视图"✓

```extc
fn ok_stack() -> i32 {
    var x: i32 = 5          // 栈上
    return x                 // 拷走值 ⇒ 可以 ✓
}

fn ok_heap() -> mut ref i32 {
    var p = new i32          // arena 上
    *p = 5
    return p                 // 带出去 ⇒ 编译器把它放对层 ✓
}
```

---

## 3. 核心规则：能带出去 / 不能带出去

一句话判据：**值的"来路"有多深，目的地就有多深**；
**往浅处（更长寿处）带** ⇒ 编译器把这块 arena **提升**；**提不动** ⇒ 报错 ✓

### 3.1 允许（编译器负责让它安全）

| 形状 | 例 | 为什么安全 |
|---|---|---|
| 返回值拷走 | `return x` | 值拷贝，不留指针 |
| 返回 `new` 出来的引用 | `return p`（`p = new T`）| 函数多收一个**隐藏参数**：调用者选的那只 arena（`__extc_home`）⇒ 它活到调用者的块 ✓ |
| 循环里建链表、`head` 在循环外 | `while … { var c = new n  c.next = h  h = c }` | `head = c` 把那个 `new` **提升**到 `head` 那一层 ✓ |
| 把新的东西写进实参指向的对象 | `fn fill(p: mut ref ?ref n) { p = new n }` | 实参的 arena 是"链上最浅"的那只，它比任何实参的存储都长寿 ✓ |
| 嵌套块里造、外层变量接住 | `{ var p = new n  keep = p }` | `keep` 比内层块长寿 ⇒ 提升 ✓ |

### 3.2 拒绝（编译期报错，带源码位置）

| 形状 | 例 | 报错 |
|---|---|---|
| 返回本帧栈局部的引用 | `fn f() -> ref i32 { var x = 5  return ref x }` | `this return value would hold a reference to a local variable that dies first` |
| 把本帧局部存到更长寿的地方 | `g = ref x`（`g` 在更外层）| `this reference would hold a reference to a local variable that dies first` |
| 把本帧局部的引用**包进容器**再带出去 | `var t: b = { p: ref x }  return t` | 同上（编译器会**穿透容器**看）✓ |
| 把本帧切片返回 | `return a[..]`（`a` 是本帧数组）| 同上（切片是"视图"，一样查）✓ |
| 循环里把局部地址存进循环外的变量 | `while … { keep = ref x }` | 同上 |

> ⚠️ 这些报错都长这样：**`%s would hold a reference to a local variable that dies first
> (borrowed from depth N, but this can only hold up to depth M)`** ——
> `N`/`M` 就是"来路多深 / 目的地多深"，**它们就是你改代码的线索** ✓

---

## 4. 引用：`ref` / `mut ref` / `?ref`

| 类型 | 可空？ | 能透过它写？ |
|---|---|---|
| `ref T` | ❌ 引用**永远不为 null** | ❌ 只读 |
| `mut ref T` | ❌ | ✅ `*p = v` |
| `?ref T` | ✅（零值 `null`）| ❌ |
| `mut ?ref T` | ✅ | ✅ |

- **`ref` 不可为空是语言承诺**：`var q: ref i32 = null` ⇒ 编译期报
  `` `null` here does not know which `?ref T` it is `` ✓
- **可写性只能变窄**：`mut ref T` → `ref T` 可以（✅），反过来 ❌
  （`initializer expects `mut ref n`, found `ref n``）
- **`=` 在引用上只表示"改指向"**，写穿必须显式 `*p = v` ✓
  （报错话术：`write through the reference instead: `*p = v``）
- 一组值的写法：`*p` 读 · `*p = v` 写 · `p = ref q` 改指向 ✓

---

## 5. Q&A —— 什么情况会有什么处理

> 下面每一条都跑过（§7 有命令和结果）。**"结果"那一栏是编译器的真实行为**，
> 包括我原本以为会通过、实际被拒的那几条（标了 ⚠️）✓

### 5.1 基本

**Q：`var p = alloc<i32>(1)` 然后 `*p = 7`，行吗？**
A：行 ✓ —— `alloc` 返回可写引用，写穿要写 `*p = 7`（`*` 不能省）。
结果：`OK`，输出 `7`。
⚠️ 注意**语句要换行或 `;` 分隔**：`var p = alloc<i32>(1)  *p = 7` 写在一行且只用空格隔开
⇒ 解析器报 `` expected an expression, found `=` ``（它把 `*p` 当成初始化式的一部分）✓

**Q：`new` 一个结构体、改字段、读回来？** A：`OK` ✓

**Q：取栈上变量的引用、当场读？** A：`OK` ✓（引用没离开本块）

### 5.2 带出去

**Q：函数返回 `new` 出来的引用？** A：`OK` ✓ —— 函数多收一个隐藏参数
`extc_arena *__extc_home`，`new` 分配到**调用者**选的那只 arena ⇒ 活到调用者那块结束 ✓

**Q：函数返回本帧栈局部的引用？** A：**编译期拒绝** ✓
`this return value would hold a reference to a local variable that dies first`
（堆上的可以，栈上的不行 —— **这是 extC 最容易踩的一条**）

**Q：把本帧局部的引用装进结构体、返回结构体？** A：**拒绝** ✓
编译器**穿透容器**看里面的引用，不会因为"包了一层"就放行 ✓

**Q：返回 `a[..]`（本帧数组的切片）？** A：**拒绝** ✓ 切片是**视图**，一样按引用处理 ✓

**Q：循环里建链表、`head` 在循环外，每轮 `new`？** A：`OK` ✓
`head = c` 把那个 `new` 的提升到 `head` 那一层 ⇒ 节点活到函数结束 ✓
（代价：这类分配不再每轮回收 —— 最坏是一个**函数级、自动清理的堆**；这是有意换的 ✓）

**Q：循环里 `new` 完就丢，内存会涨吗？** A：**不会** ✓
出块即回收（`tests/arena/` 在 150MB 上限下跑 300 轮 × 1MB 就是这个验收）✓

### 5.3 层级与控制流

**Q：嵌套块里 `new`，外层变量接住？** A：`OK` ✓ —— 提升到 `keep` 那一层 ✓

**Q：`if` 两支各写一次同一个变量，然后带走？** A：`OK` ✓
**`if` 汇合是编译器做对的事**：它取两支的**最小**（最长寿）那个需求 ✓
（这正是历史上最后一个悬垂 bug 的形状 —— 见 [`ARENA-NOTES.md`](ARENA-NOTES.md) §1.6 ✓）

**Q：中间经一个变量转手（`mid = c  h = mid`）？** A：`OK` ✓ 来路追得到 ✓

**Q：把本帧局部的地址存进循环外的变量？** A：**拒绝** ✓
（"块里造、块外存"是这个编译器盯得最紧的形状 ✓）

### 5.4 跨函数

**Q：把 `ref x`（我本帧的局部）传给一个会把它存起来的函数？** A：**看情况** ——
如果那个函数把值存到"活得比调用还久"的地方，**调用点就会报错** ✓
实测：`argument 2 of `stash` carries a reference into a deeper scope (depth …)` ✓

**Q：⚠️ 我以为能过的反例**：把**本帧数组的切片**传进 `boxT<slice<i32>>::stash`
（`self.v = value`）—— 我原以为会被接受，**实际被拒** ✓
理由：`stash` 要的是"活得不比它自己短"的实参，而切片指着**本帧栈上的数组** ⇒ 深度不够 ✓
**⇒ 这就是"编译器比你严"的例子；遇到这种报错，看 `depth N / depth M` 那两个数** ✓

**Q：`fn mk(p: mut ref i32) { var q = new i32  p = q }` —— 能行吗？** A：`OK` ✓
（`new` 分配进**最浅的那只**实参 arena ⇒ 活得比 `p` 指向的存储更久 ✓）

### 5.5 引用与可空

**Q：`var q: ref i32 = null`？** A：**拒绝** ✓
`` `null` here does not know which `?ref T` it is `` —— `ref` 承诺不为空，
要可空就写 `?ref T` / `mut ?ref T` ✓

**Q：`?ref T` 怎么取值？** A：**先证明非空**（`if p != null` 收窄，或直接 `p!`）✓
`println(*(p!))` ⇒ `OK` ✓

**Q：`mut ref` 能当 `ref` 用吗？反过来呢？** A：**只能变窄** ✓
`mut ref n` → `ref n` 可以；`ref n` → `mut ref n` 报
`initializer expects `mut ref n`, found `ref n`` ✓

**Q：`*p = v` 但 `p` 是 `ref T`？** A：**拒绝** ✓ `cannot write through a read-only reference` ✓

**Q：透过 `ref` 参数改结构体字段（`p.v = 9`）？** A：**拒绝** ✓（同上）

**Q：`var r: mut ref n = ref a` 之后 `r = ref b`？** A：`OK` ✓ ——
**`=` 只改指向**，`a` 不变；写穿要 `r.v = 9`（那时改的是 `b`）✓

### 5.6 泛型

**Q：泛型 `struct box<T>` 的方法怎么写？** A：**写在 struct 体内** ✓
⚠️ 写在体外（Go 那种）⇒ 报 `` unknown type `T` `` ✓ —— 这是**语法**问题，不是内存问题 ✓

**Q：泛型方法把 `T` 存进 `self.v`，遇到 `T = slice<i32>` 会怎样？** A：**按实例判** ✓
`boxT<i32>::stash` 合法；`boxT<slice<u8>>::stash` 会因为"切片可能指向更浅处"被拒 ✓
（机制：模板先记下规则，**每个实例**再判一遍 —— 见 `PLAN.md` §0.4 #44 ✓）

### 5.7 什么情况"运行时才处理"

**Q：无限递归？** A：**运行时 trap，带源码位置** ✓
生成代码里有 `EXTC_REC_LIMIT`（10 万层）护栏；实测
`examples/…:1: trap: recursion too deep (unbounded recursion?)` ✓

**Q：arena 要不到内存？** A：`extc: out of arena memory` + **文件:行号**，`exit(1)` ✓

**Q：数组越界？** A：**切片**在编译期能证明的就编译期报错；
其余**运行时 trap**（安全，不是静默踩内存）✓

---

## 6. 编译器给你保证了什么（可以依赖的三条）

1. **不会悬垂**：任何"引用比它指的东西活得久"的程序要么**编不过**，
   要么编译器**已经把那块内存提升**到够用的层 ✓
2. **不会静默踩内存**：越界、递归过深、arena 耗尽 —— 都是**带位置的 trap** ✓
3. **不会忘释放也不会重复释放**：没有 `free` 这个概念，出块整块回收 ✓

**代价（诚实说）**：
- 块内分配的东西**不能**活过这个块 —— 除非编译器能提升它；提升不了就报错（要你改代码）✓
- 提升是**保守**的：一个站点只要有一条路径要逃逸，整块就按最坏那条定 ⇒ 可能多占内存 ✓
- **没有别名分析**：`a` 和 `b` 指向同一个东西这件事编译器不管 ⇒ 少数形状会被误拒
  （已知边界，主人已拍板不做）✓

---

## 7. 怎么自己复核这份文档

用例在 `tools/memsafe/qa/`（每个 `.extc` 一个场景），跑法：

```bash
bash tools/memsafe/run.sh          # 逐条编译+运行，打印"期望 vs 实际"
```

判据：**每条都必须与本文表格一致**。任何一条不一致 ⇒ 要么是文档过时，要么是编译器回归，
**两者都要立刻查**（这份文档的价值就在于它可被机器复核 ✓）。

本文写作时的实测（2026-09-24）：**33 条用例，全部与文档一致** ✓
