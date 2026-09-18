# extC 语言手册

> **⚠️ 这份手册只描述「已经实现」的东西。**
> 已定案但还没实现的在第 10 节，还没定的在第 11 节。
> 一份说谎的手册比没有手册更坏 —— 所以这里宁可写得少，也不写没实现的东西。
>
> 当前版本：**week-0**（2026-09-18）
>
> 设计理由看 [`DESIGN.md`](DESIGN.md)；为什么这么定看 [`DECISIONS.md`](DECISIONS.md)；接下来做什么看 [`PLAN.md`](PLAN.md)。

---

## 0. 三分钟上手

```sh
make                                   # 产出 build/extc
./build/extc --run examples/hello.extc # 生成 C、编译、直接运行
```

第一个程序：

```extc
// 这是注释
fn add(a: i32, b: i32) -> i32 {
    return a + b
}

fn main() -> i32 {
    let x: i32 = 3
    var y = 4                  // 局部变量可以省类型标注，从初始化式推导
    y = y + 1
    if x < y {
        print("sum = ")        // ⚠️ 格式串 {} 还没实现，先这样拼
        println(add(x, y))
    }
    return 0
}
```

extC 编译到 C，再由系统的 C 编译器编成可执行文件。生成的 C 带 `#line`，所以 **gcc 的报错会指回 `.extc` 的行号**。

---

## 0.5 内存安全：承诺 × 现状

> **这一章回答一个问题：extC 到底承诺了什么，今天实现了多少。**
> 每条都标了「实测状态」，不是设计愿望。

### 三条前提（一开始就定的）

| # | 前提 | 现在 |
|---|---|---|
| **G1** | **内存自动回收** —— 用户永不写 `malloc` / `free` / `close` | ✅ 语言里**连 `malloc` 都没有**（生成的 C 里出现 0 次） |
| **G2** | **第一个真实用户是五子棋**（不是自举） | ✅ 棋盘/五连判定/`result` 都能写 |
| **G3** | 能自举 | 🔸 **修正**：主人 2026-09-18 说「不一定完全自举」⇒ 降级成**可选的验证手段** |

### 一条原则

> **P：凡是编译期能证明的，运行时不留痕迹。**

它禁的是**隐藏的机制**（GC / 异常 / 运行时元数据 / 隐藏分配 / 运行时类型标签），
**不是数据拷贝**（值语义的拷贝是写在类型上的、看得见的）。

> **P′（同一句话的另一面）：不能证明的，语法上必须看得见。**

### 内存安全的九条承诺

| # | 承诺 | 状态 | 说明 |
|---|---|---|---|
| 1 | **无 GC、无引用计数** | ✅ | 从来就没有（运行时零分配） |
| 2 | **无异常、无栈展开** | ✅ | 失败归 `result`，`?` 展开成三句语句 |
| 3 | **未初始化 UB 消失** | ✅ | 默认零初始化：`var b: board` 自动清零 |
| 4 | **`ref` 不可为空** | ✅ | 没有 `null`；`ref` 也没有零值 |
| 5 | **越界 = trap 带 extC 位置** | ✅ | `a[i]` 越界 → `trap: index 7 out of range (length 3)` + 文件行号 |
| 6 | **切片越界** | ✅ | 能证明的**编译期报错**，不能证明的运行时 trap 带位置 |
| 7 | **只读 / 可写看得见** | ✅ | `ref T` / `mut ref T`、`slice<T>` / `mut slice<T>`（见 §3） |
| 8 | **算术无 UB** | ⚠️ **部分** | 溢出用 `-fwrapv` 兜成确定行为；**除零崩了没位置**、**移位超宽静默错**（待补） |
| 9 | **引用不能活得比被指对象长**（逃逸检查） | ⬜ **0%** | **这是目前唯一还开着的洞**，见下 |

### 唯一还开着的洞：逃逸

```extc
fn bad() -> slice<i32> {
    var a: [8]i32 = [1, 2, 3, 4, 5, 6, 7, 8]
    return a[..]        // ✗ 返回指向**已经死掉的局部数组**的视图 —— 今天编得过！
}
```

实测它不但编得过，还**跑出了 `0`** —— 也就是说它是**静默错**，
比崩掉更危险。**这就是 extC 距离「安全像 Rust」最远的一处。**

修法在 DESIGN §2 里早就定好了，而且**不需要堆**：

> **若引用 `r` 指向值 `v`，则 `depth(r) ≥ depth(v)`。**
> 深度是**纯词法属性**（几层 `{}`；函数帧 = 1，参数 = 0），**比两个整数**，
> 不需要生命周期标注 —— 这正是「Rust 的安全 + 不写生命周期」的落点。

⇨ 下一个大块就是它（见 [`REFS.md`](REFS.md) 块 B 与 [`BOOTSTRAP.md`](BOOTSTRAP.md) §7）。

---

## 1. 程序结构

一个 `.extc` 文件里，顶层只允许三种东西：

```extc
type 名字 = | 变体 | 变体      // 枚举（无载荷的标签联合）
struct 名字 { ... }            // 结构体定义
fn 名字(参数) -> 返回类型 { ... }   // 函数定义
```

**还没有模块系统**（`module` / `export` / `import` 是 week-2 的事，见第 10 节）。

程序入口必须是：

```extc
fn main() -> i32 { ... }
```

`main` 不能带参数；返回值就是进程退出码。

---

### prelude：自带类型不用 import

编译器自带一小段 extC 源码（`stdlib/prelude.extc`），**每次编译都先于你的文件被处理**。
里面定义的类型不用 import 就能用。现在有：

| 类型 | 方法 |
|---|---|
| `slice<T>` | `isEmpty()` · `hasAt(i)` · `get(i)` · `==` · `find(needle)` · `startsWith(prefix)` |

而且 **`slice<u8>` 就是字符串的类型** —— `println("hello")` 之所以能打文本，就是因为它是字节视图。

```extc
var n: i32 = 42
var s: slice<i32> = { data: ref n, len: 1 }    // 指向 n 的一片
println(s.isEmpty())     // false
println(s.hasAt(0))      // true
println(s.hasAt(1))      // false
```

> **为什么要有 prelude？** 因为「能在 extC 里写的东西，就在 extC 里写」。
> 没有它，`slice<T>` 只能在编译器的代码生成器里硬编码 —— 那就成了**把库塞进编译器**。
> 验收方式很机械：`grep -in slice src/*.c src/*.h` 应该**一无所获**。
>
> ⚠️ 现在 `slice` 的方法很少（没有数组索引和 `option` 就写不出 `get`），
> 而且**空 slice 造不出来** —— `ref` 不可为空，长度 0 的切片没有合法的 `data`。

---

## 2. 词法

| 元素 | 写法 |
|---|---|
| 行注释 | `// 到行尾` |
| 块注释 | `/* 可以跨行 */` |
| 语句结束 | **换行**。分号可选，写了会被忽略 |
| 字符串 | `"..."`，转义（`\n` `\t` `\"` `\\`）原样交给 C |
| 整数 | `42`、`0x1F` |
| 浮点 | `3.14`、`1e-3` |
| 布尔 | `true` / `false` |

**关键字**：`fn` `let` `var` `if` `else` `while` `return` `break` `continue`
`struct` `type` `true` `false` `ref` `mut`。内建类型名（`i32` `u8` `bool` …）不是关键字，
但也不能拿来当变量名。

**符号**：`+ - * / %` `== != < <= > >=` `&& ||` `& | ^ ~` `<< >>` `=`
`( ) { } [ ]` `, : ; .` `::` `->` `..` `...` `?` `ref`。

**连续多个换行会合并成一个语句结束符**，所以空行随便加。

```extc
let a = 1        // 这两个是两条语句
let b = 2
```

---

## 3. 类型

### 内建类型

| 类别 | 名字 | C 里的对应 |
|---|---|---|
| 有符号整数 | `i8` `i16` `i32` `i64` | `int8_t` … `int64_t` |
| 无符号整数 | `u8` `u16` `u32` `u64` | `uint8_t` … `uint64_t` |
| 浮点 | `f32` `f64` | `float` / `double` |
| 布尔 | `bool` | `bool` |
| （字符串） | 没有内建字符串类型 —— 见下面的 **`slice<u8>`** | — |
| 空 | `void` | `void` |

**没有 `int` / `long` / `char` / `double`** —— 类型名一律带位宽（见 [`SYNTAX.md`](SYNTAX.md) 的命名规范）。

字面量的默认类型：整数 `i32`，浮点 `f64`。

### 类型转换规则（T2 起）

extC **只自动做无损失的拓宽**，收窄一律禁止。

| 转换 | 自动？ |
|---|---|
| 同类型 | ✅ |
| `i8`→`i16`→`i32`→`i64` | ✅ |
| `u8`→`u16`→`u32`→`u64` | ✅ |
| `u*` → 更宽的 `i*`（如 `u32`→`i64`） | ✅ |
| `i*` → `u*`（一律，包括同宽） | ❌ 有损 |
| 能精确表示的整数 → 浮点（`i16`→`f32`、`i32`→`f64`） | ✅ |
| `i32` → `f32` | ❌ f32 尾数只有 24 位 |
| `f32` → `f64` | ✅ |
| `f64` → `f32` | ❌ |

**字面量按值适配**（`DESIGN.md` §5 的「字面量类型推导」）：

```extc
let a: u8  = 200        // 200 放得进 u8 → 可以
let b: u16 = 60000      // 可以
let c: u8  = 300        // error: literal `300` does not fit in `u8`
let d: f64 = 7          // 整数字面量给浮点变量 → 可以
let e: f32 = 3.14       // 浮点字面量给 f32 → 可以
let f: i32 = 3.14       // error: 有损
```

变量之间**没有**字面量适配：

```extc
var x: f64 = 1.5
var y: f32 = x          // error: expects `f32`, found `f64`
```

**没有隐式真值转换**，条件必须是 `bool`：

```extc
if 1 { }                // error: expected `bool`, found `i32`
```

**两种没有公共类型的运算会报错**，要先显式转：

```extc
var a: u32 = 1
var b: i32 = 2
let c = a + b           // error: `u32` and `i32` have no common type for `+`
```

### 枚举类型

```extc
type status = | ok | warn | error      // 前导 `|` 可写可不写
```

- 变体用 **`类型名.变体名`** 访问：`status.ok`
- 变体名不必全局唯一 —— `status.ok` 和 `result.ok` 是两个不同的东西
- 只有**同类型**才能比较和赋值（不能拿 `i32` 跟枚举比）
- 枚举**自动有名字文本**（定案 11）：`println(s)` 直接打印 `warn`

```extc
type status = | ok | warn | error

if s == status.ok { ... }      // 可以
if s == 0 { ... }              // error: 类型不匹配
println(s)                     // warn
```

### 泛型

```extc
struct pair<A, B> {
    first: A
    second: B

    fn getFirst(self: ref pair<A, B>) -> A {
        return self.first
    }
}

struct box<T> {
    value: T
    fn set(self: ref box<T>, v: T) { self.value = v }
    fn get(self: ref box<T>) -> T { return self.value }
}

fn main() -> i32 {
    var p: pair<i32, u8> = { first: 1, second: 2 }
    println(p.getFirst())            // 1

    var q: pair<bool, point> = { first: true, second: { x: 7, y: 8 } }
    println(q)                       // 实例也能自动调试打印

    var b: box<i64>
    b.set(42)
    println(b.get())                 // 42
    return 0
}
```

- 泛型参数写在 struct 名后：`struct Name<T> { ... }`
- 使用时**必须写出实参**：`Name<i32>`（不写是编译错误）
- 编译器**按实例生成 C**（单态化）：`pair<i32, u8>` → C 里的 `pair_i32_u8`，
  方法变成 `pair_i32_u8_getFirst`
- 泛型 struct 的方法里，泛型参数可见（`fn get(self: ref box<T>) -> T`）

> ⚠️ **已知限制（「模板检查一遍」换来的代价）**
>
> 类型检查是对**模板**做的，所以模板里 `A` 和 `B` 是不确定的。要求两边同类型的操作写不出来：
>
> ```extc
> fn swap(self: ref pair<A, B>) {
>     let tmp: A = self.first
>     self.first = self.second   // error: assignment expects `A`, found `B`
> }
> ```
>
> 要写这种操作，就**用一个参数**：`struct pair<T> { first: T  second: T }`。
>
> 收益是：错误只报一次、错误信息指向模板而不是某个实例 ——
> 反面就是 C++ 那种「实例化时才炸出一屏模板错误」。

### 结构体类型

顶层 `struct` 定义的类型就是类型名，camelCase（**跟变量一样**，不用 PascalCase）：

```extc
struct point {
    x: i32
    y: i32
}
```

### 引用类型 `ref T` / `mut ref T`

- **`ref T`** —— **只读**借用（默认）
- **`mut ref T`** —— **可写**借用

两者都「不可为空、不可做算术」，在 C 里都是 `T *`（只读那档可以用 `const T *` 表示）。

```extc
fn peek(self: ref counter) -> i64 { return self.n }        // 只读：let 的也能调
fn bump(self: mut ref counter) { self.n = self.n + 1 }     // 可写：接收者必须可写
```

**可写性看「这个值是从哪拿到的」，只有三个地方**：

| 从哪拿到 | 可写性 |
|---|---|
| **绑定** | `var` 可写 / `let` 只读 |
| **参数** | `mut ref T` 可写 / `ref T` 只读 |
| **字段** | 类型上写 `mut` |

而**取引用时权限从源头继承**，所以日常代码**一个 `mut` 都不用写**：

```extc
var n: i64 = 10
bump(ref n)          // var ⇒ 取出来就是 mut ref ✓
let m: i64 = 7
// bump(ref m)       // ✗ 对 let 只能取到只读引用 ⇒ 交不给要写权限的参数
```

**降级是自动的、单向的**：`mut ref T` 可以当 `ref T` 用（能写当然能读）；
反过来不行，必须显式写 `mut` —— 「安全是默认」的直接体现。

#### 值位置自动解引用（`ref` 在表达式里的行为）

`ref T` 的值在**当值用**的时候自动解引用 —— 于是标量引用也能用了：

```extc
fn bump(p: mut ref i64) { p = p + 1 }        // 读 p 得值；写 p 写进所指的地方
fn swap(a: mut ref i64, b: mut ref i64) {
    let t = a
    a = b
    b = t
}

var n: i64 = 10
bump(ref n)              // n = 11
println(n)               // 11
let m: i64 = ref n       // 值位置 ⇒ 拷值
println(m)               // 11
```

两条要记住的规则：

- **`ref x` 自己永远不解引用** —— 否则就是「解掉自己刚取的那个引用」，自相矛盾。
  所以 `let r = ref n` 拿到的是**引用**，而 `let y = r` 拿到的是**值**。
- **「换指向」取消了**：`r = ref b` 是错的。`=` 在引用上只有一个意思
  ——**写进它指向的地方**。（换指向全仓库 0 处使用，见 [`DECISIONS.md`](DECISIONS.md)）

```extc
var r: mut ref i64 = ref n
r = r + 5                // ✓ 写穿：n 变成 16
// r = ref other         // ✗ cannot retarget a reference
```

#### 透过只读引用写 = 编译错误

```extc
fn f(b: ref board) { b.cell[0] = 1 }
// error: cannot write through a read-only reference
//   note: `ref T` is a **read-only** borrow; writing through it needs `mut ref T` …
```

这条会**顺着调用图追**：`rng::below` 因为里面调了 `next()`（要可写接收者），
自己的签名也得改成 `mut ref` —— 光看函数体是看不出这种问题的。

---

### 数组类型 `[N]T`

**长度写在前面**，因为「数组的长」是类型的一部分，跟 `[15][15]i32` 从左往右念一致。

```extc
var a: [5]i32                    // 5 个 i32，全零（零初始化是默认，不需要专门语法）
var board: [15][15]i32           // 15×15，递归 —— 等价于 [15]([15]i32)
let grid: [3][2]f64 = [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]
```

**字面量必须给全**（严格，防 off-by-one），末尾 `...` 表示「剩下的零」：

```extc
var b: [5]i32 = [1, 2, ...]      // → [1, 2, 0, 0, 0]
let c = [1, 2, 3]                // ✓ 长度 3 从字面量推出来
```

**数组是值类型**：赋值、传参、返回都是**整份拷贝**。

```extc
var x: [3]i32 = [1, 2, 3]
var y = x          // y 是独立的一份
y[0] = 99          // x[0] 仍是 1
```

> **为什么是值？** 沿用 extC 的核心（默认值语义、没有别名）。
> 900 字节的拷贝确实贵 —— 要省就用 `ref` 显式表达，**别让读者猜**。
> 编译到 C 时套一层 struct（`typedef struct { int32_t data[5]; } array_5_i32;`），
> 因为 C 的裸数组既不能赋值也不能返回。

**索引 `a[i]` 带边界检查**，越界 trap 并报 extC 位置：

```extc
println(a[0])      // 1
println(a[7])      // trap: index 7 out of range (length 5) —— 带文件名和行号
```

**切片视图 `a[lo..hi]`** —— 四种写法，**不拷贝数据**（见 [`ARRAYS.md`](ARRAYS.md) §4）：

```extc
var a: [8]i32 = [10, 20, 30, 40, 50, 60, 70, 80]
var s1 = a[2..5]      // [30, 40, 50]   mut slice<i32>（从 var 切出来 ⇒ 可写）
let s2 = a[6..]       // [70, 80]       到尾
let s3 = a[..3]       // [10, 20, 30]   从头
let s4 = a[..]        // 整个数组
s1[0] = 999           // 视图是同一块内存 —— a[2] 也变成 999
```

#### 视图的可写性：`slice<T>` / `mut slice<T>`

**只有一个视图类型、一套方法集** —— `mut` 是**限定词**，不是第二个类型
（不像 Rust 要给切片造 `&[T]` / `&mut [T]` 两个）。

**可写性从切出来的源头继承**：

| 从哪切出来 | 得到的类型 | 能写吗 |
|---|---|---|
| `var a` | `mut slice<T>` | ✅ |
| `let a` | `slice<T>` | ❌ |
| `"字面量"` | `slice<T>` | ❌（在只读段） |
| `mut ref` 参数 | `mut slice<T>` | ✅ |
| 只读视图 | `slice<T>` | ❌ |

```extc
var a: [6]i32 = [1, 2, 3, 4, 5, 6]
var mid = a[1..4]         // var ⇒ mut slice<i32> ✓ 能写
reverse(mid)              // 就地反转

let frozen: [3]i32 = [7, 8, 9]
// frozen[0] = 1          // ✗ let ⇒ 不能写
// fill(frozen[..], 0)    // ✗ 从 let 切出来是只读视图

var s = "abc"
// s[0] = 88              // ✗ 字面量是只读视图 ⇒ **编译错误（以前是段错误）**
```

#### 函数签名上的可写性

```extc
fn sum(v: slice<i32>) -> i32          // 只读：签名说了它不改；两种视图都能传
fn fill(v: mut slice<i32>, x: i32)    // 可写：调用者必须给得出手可写的东西
fn reverse(v: mut slice<i32>)         // 就地算法
```

**这条关掉的是「按值参数偷写调用者的数据」**：

```extc
fn fill(v: slice<i32>) { v[0] = 1 }
// error: cannot write through `v`: it is a read-only view `slice<i32>`
//   note: a view is read-only unless its type carries `mut`. Writing through a
//         by-value view would change the caller's data without the signature saying so.
```

降级是自动的、单向的：`mut slice<T>` 可以传给要 `slice<T>` 的地方；反过来不行。

**编译期能证明的，运行时不留痕迹**：

| 情况 | 结果 |
|---|---|
| 两个界都是字面量 | 生成的 C 里**零检查**，就是 `&a.data[2]` |
| 界里有变量 | 运行时检查，越界 trap 带 extC 位置 |
| 字面量界越界（含负数） | **编译期报错** |

```extc
let v = a[2..9]       // error: slice end 9 is not inside `[5]i32` (length 5)
```

**底必须是「地方」**：切固定数组要取元素地址，所以 `make()[1..3]` 被拒
（那会切到临时存储上）。切 **slice** 不受此限 —— `"abcdef"[1..3]` 合法。

---

### `option<T>` / `result<T,E>`：把「可能没有」「可能失败」写进类型

两个类型**都写在 `stdlib/prelude.extc` 里**（extC 源码），不用 import：

| 类型 | 意思 | 零值 |
|---|---|---|
| `option<T>` | 可能有值，也可能没有 | **就是「没有」**（`has = false`） |
| `result<T,E>` | 要么成功给你值，要么失败给你错误 | **失败侧**（`ok = false`） |

「零值就是没有 / 失败」不是巧合 —— 它是定案 8（默认零初始化）的直接结果，
所以不需要任何新机制。

```extc
let a = option<i64>::some(42)
let b = option<i64>::none()          // 也可以：var b: option<i64>  ← 零值就是 none
if a.has {
    println(a.value)
}
println(b.valueOr(-1))               // 没值就用兜底值

let r = result<unit, gameError>::failure(gameError.occupied)
if r.ok { ... } else { println(r.err) }
```

用**关联函数**构造：写在 `struct` 体内但**不带 `self`** 的函数，
调用时类型写全（不靠上下文猜，见 [`DECISIONS.md`](DECISIONS.md) 定案 27/29）：

```extc
struct box<T> {
    value: T
    fn make(v: T) -> box<T> { return { value: v } }   // 关联函数
}
let b = box<i32>::make(5)
```

### `?`：失败就顺着往上抛

```extc
fn placeLine(b: ref board, y: i32, from: i32, to: i32) -> result<unit, gameError> {
    var x: i32 = from
    while x <= to {
        place(b, x, y, 1)?           // ← 任何一步失败，整行就失败（可见、无栈展开）
        x = x + 1
    }
    return result<unit, gameError>::success(unit {})
}
```

**四个合法位置**（`?` 要在语句级展开成「求值一次 + 判断 + return」）：

| 写法 | 意思 |
|---|---|
| `f()?` | 这一步必须成功，否则整串失败 |
| `let x = e?` | 取出载荷 |
| `x = e?` | 同上，赋给已有的地方 |
| `return e?` | 把载荷装回外层类型 |

**载荷类型可以不同**（里层 `result<i64, E>` 放进返回 `result<point, E>` 的函数 ✓），
**错误类型必须相同** —— `?` 是原样转发，不编造转换。

⚠️ 别的写法（`f(e? + 1)`）会**报错**，不会生成编不过的 C。

⚠️ 已知限制：`option<T>` / `result<T,E>` 的 `T` **不能含 `ref`**
（`option<slice<u8>>` 不行）—— `None` 的那个 `value` 字段没有合法的值可填。

---

## 4. 变量与声明

```extc
let name: Type = 表达式      // 不可变绑定
var name: Type = 表达式      // 可变变量
var name: Type               // 零初始化（定案 8）
```

- **类型标注可以省**（局部变量）：`var y = 4` 从初始化式推导。
- **省略初始化式就必须写类型**：`var b: board` 会**自动清零** ——
  C 里最大的 UB 来源之一就是「读到未初始化内存」，它只能在运行时发现；
  默认清零把它变成编译期就能保证的东西。
- `ref T` **不能零初始化** —— 它是不可为空的引用，没有「零值」。
- **`let` 是只读的，而且管的是「根」** —— 不只是不能重新绑定，
  **也不能透过它写字段 / 数组元素 / 视图元素**：

```extc
let x = 1
x = 2              // error: cannot assign to `x`, which is a `let`

let p: point = { x: 1, y: 2 }
p.x = 9            // error: cannot write through `p`, which is a `let`

let a: [3]i32 = [1, 2, 3]
a[0] = 9           // error: cannot write through `a`, which is a `let`

let n = 7
let r = ref n      // error: cannot take a reference through `n`, which is a `let`
                   //（`ref T` 是**可变**引用 —— 见 §7.5）
```

> **⚠️ 这条规则是「浅」的：它管名字，不管数据。**
> 把 `let` 视图拷给一个 `var`、或者传进函数，那边照样能写同一块内存；
> 调用一个 `self: ref T` 的方法也算（`let p; p.moveBy(1)` 现在是允许的）。
> 要管到数据层，可变性就得进**类型**（Rust 的 `&` / `&mut`）——
> 那是 week-4 引用规则的范围，见 [`DECISIONS.md`](DECISIONS.md) 定案 28。

```extc
var b: board       // 所有字段清零
var n: i32         // 0
var ok: bool       // false
```

### 作用域

块 `{ }` 引入新作用域。同名重复声明在同一作用域里是错误。

**还没有全局变量**（已定案要加，见第 10 节）。

---

## 5. 表达式与运算符

### 运算符（从高到低）

**优先级跟 C（和 Go）一致** —— 这是有意的：写算法题的人手上有 C 的肌肉记忆，
这里要是「设计得更好」反而天天出错。

| 优先级 | 运算符 |
|---|---|
| 1 | `f(x)` 调用、`a.b` 字段、`a.f(x)` 方法、`::` 关联函数 |
| 2 | `-x`、`!x`、`~x`（一元） |
| 3 | `*` `/` `%` |
| 4 | `+` `-` |
| 5 | `<<` `>>` |
| 6 | `<` `<=` `>` `>=` |
| 7 | `==` `!=` |
| 8 | `&`（按位与） |
| 9 | `^`（按位异或） |
| 10 | `\|`（按位或） |
| 11 | `&&` |
| 12 | `\|\|` |

**赋值 `=` 不是表达式，是语句** —— 所以 `a = b = c` 写不出来，`+=` 之类的复合赋值也还没有。

位运算只许**整数**（不是数字就行）：`1.5 & 2` 报错，`true & false` 也报错 ——
**`bool` 在 extC 里不是整数**（不像 C）。

于是 C 那个著名坑在这里是**编译错误**而不是静默算错：

```extc
if 6 & 3 == 3 { }     // error: cannot apply `&` to `i32` and `bool`
                      // C 里它是 6 & (3 == 3) = 0，一声不响
```

```extc
let lowbit = i & (-i)     // 树状数组的标准写法 ✓
let half = (l + r) >> 1
let flag = (x >> 3) & 1
```

> `>>` 对**有符号负数**是算术右移（gcc 的行为）。C 标准说这是 implementation-defined
> 而不是 UB，extC 只走 gcc，所以行为是确定的 —— 但知道一下比较好。

### `ref T` 不是数字

引用**不能做算术、不能比较、不能接收值**：

```extc
fn bump(p: ref i64) {
    let y = p + 1      // error: cannot apply `+` to `ref i64` (a reference)
    p = 5              // error: expects `ref i64`, found `i32` -- a value is not a reference
}
```

**为什么必须报错**：C 里 `p + 1`（p 是 `int64_t *`）是指针算术 ——
它编得过，但意思完全不是用户想的那样。

透过 `ref` 写**字段和元素**是允许的（这才是方法的用法）：

```extc
fn clear(self: ref board) {
    self.moves = 0              // ✓ 写字段
    self.cell[0][0] = 0         // ✓ 写元素
}
```

⚠️ **但「透过 `ref` 写一个标量」现在写不出来**（`p = p + 1` 会被拒）。
绕法：把值包进一个 struct 再透过 `self.field` 写。
要不要加解引用写法（比如 `p.*`）还是个待定项，见 [`DECISIONS.md`](DECISIONS.md)。

### 没有 `&` 取地址运算符

**用户写不出取地址。** 引用只通过 `ref T` 形参进入函数。这是「无裸指针」的实现方式。

### 结构体字面量

```extc
let p: point = { x: 1, y: 2 }    // 靠声明类型补全
let q = point { x: 1, y: 2 }     // 写全类型名也行
let o: point = {}                // 空字面量 = 零初始化（靠声明类型补全）
```

裸 `{}` 的类型**从上下文推导**，只在上下文能唯一确定类型的地方允许：

- `var x: T = {}`
- `return {}`（函数返回 `T`）
- `f({})`（形参类型是 `T`）

推导不出来就报错，**绝不猜**。

> **位置规则（跟 Go 一样）**：`name { ... }` 是结构体字面量，
> 但在 `if` / `while` 的**条件位置**里，`{` 属于代码块 —— 想在那里写字面量就加一对括号：
>
> ```extc
> let p = point { x: 1 }                    // 可以：这里 `{` 就是字面量
> if p == point { x: 1 } { }                // ✗ 报错：条件里的 `{` 是块
> if p == (point { x: 1 }) { }              // ✓ 加括号
> ```
>
> 规则写在**语法**里，不藏在**命名**里（以前是靠「首字母大写」当字面量，那是隐藏魔法）。
>
> 报错也会直接告诉你加括号：
> ```
> error: struct literal in a condition needs parentheses: `(point { ... })`
>   note: Inside an `if` / `while` condition a `{` starts the body block. To write a struct literal there, wrap it in parentheses.
> ```

### `==` 与运算符定义

`==` / `!=` 是**语法糖**，不是万能比较：

| 两边是什么 | 比较方式 |
|---|---|
| **整数 / 浮点 / `bool` / 枚举** | 编译器直接生成 C 的 `==` |
| **struct / 泛型实例** | 找它定义的 **`fn ==`**；**没定义就报错** |
| **数组 `[N]T`** | **编译器递归生成** —— 逐元素比（元素类型必须能比） |
| **`slice<T>`**（含字符串） | prelude 里定义了 **`fn ==`**，**按内容**逐元素比 |

想让自己的类型能比，就在 struct 里**显式定义 `==`**：

```extc
struct point {
    x: i32
    y: i32

    fn ==(self: ref point, other: point) -> bool {
        return self.x == other.x && self.y == other.y
    }
}

let a: point = { x: 1, y: 2 }
let b: point = { x: 1, y: 2 }
println(a == b)      // true —— 生成 point_eq(&a, b)
```

**把 `==` 写出来，而不是约定一个隐式的方法名** —— 读代码的人一眼就知道
`p1 == p2` 的行为从哪来。（在 C 里它被拼成 `point_eq`，因为 C 的标识符不能叫 `==`。）

**签名约定（在定义处强制检查）**：`fn ==(self: ref T, other: T 或 ref T) -> bool`

> ### 为什么返回值必须是 `bool`？
>
> 不是随便规定的，是三个前提推出来的：
>
> 1. `a == b` 天然会被用在 `if` / `while` / `&&` / `||` 里
> 2. extC **没有隐式真值转换**（`if 1` 是错的）
> 3. `!=` 是靠 `==` 取反实现的
>
> ⇒ 所以 `==` 只能是 `bool`，否则它根本没法用在条件里。
>
> **想要别的结果？换个方法名就行** —— `fn compare(...) -> ordering`、`fn diff(...) -> diff`
> 之类**没有任何限制**，随便返回什么。只有 `==` 这个名字被绑定了 `bool`，
> 因为它在源码里的位置决定了它是「一个条件」。
>
> 编译器在**你写下定义的那一刻**就查这条，而不是等到某处用到它才查。

- `other` 取**值**时，`a == b` 生成 `point_eq(&a, b)`，最省事 —— 小类型建议这样。
- `other` 取 **`ref`** 时，生成 `point_eq(&a, &b)`（编译器自动加 `&`）；
  但要是**手动**写 `a.==(b)`，`other` 是 ref 就必须写 `a.==(ref b)` ——
  因为**方法参数**（不是接收者）是引用时，调用点要显式写 `ref`。

**`!=` 可以单独定义；不定义的话退回用 `==` 取反**（因为 `==` 的返回值被强制成 `bool`，
所以取反永远合法）：

```extc
if a != b { }        // 没定义 `fn !=` 时，生成 !(point_eq(&a, b))
```

**运算符必须写在 struct 体内**（它是一个方法）：

```extc
fn ==(self: ref point, other: point) -> bool { ... }    // error: 自由函数不能定义运算符
```

**泛型里的 `==` 会推迟到实例化才检查**：

```extc
struct wrapper<T> {
    value: T

    fn same(self: ref wrapper<T>, other: wrapper<T>) -> bool {
        return self.value == other.value      // T 能不能比？实例化时才知道
    }
}

var a: wrapper<i32> = { value: 7 }     // i32 内建 → ✓
var b: wrapper<tag> = { ... }          // tag 没定义 == → ❌
// error: `wrapper_tag` needs `tag` to define `==`
```

> **这是「不引入 trait」的代价**：错误晚到实例化，但信息里会点明是哪个实例。
> 收益是语言里**一个新概念都不加** —— `fn ==` 就只是一个方法。

### 字符串就是 `slice<u8>`

**extC 没有内建的字符串类型。** 字符串字面量就是**指向字节的视图**：

```extc
let s = "hello world"    // 类型是 slice<u8>

println(s)                       // hello world —— 字节视图按文本打印
println(s.len)                   // 11  —— len 是个普通字段
println(s[0])                    // 104 —— 索引（越界会 trap，带位置）
println(s.get(1))                // 101

println(s == "hello world")      // true —— **按内容比较**
println(s.find("world"))         // 6
println(s.startsWith("hello"))   // true
```

**索引 `s[i]` 是带边界检查的**：可证明时零开销（将来有范围类型之后），
不能证明时生成检查、越界就 **trap 并报出 extC 的位置**。

而且 prelude 里的实现**没有一行指针算术，也没有一行手工边界检查** ——
`self[i]` 那一条语法让编译器生成带检查的索引原语。

生成的 C 只有这么点：

```c
slice_u8 s = (slice_u8){ .data = (uint8_t *)"hello", .len = sizeof("hello") - 1 };
```

**零分配、零拷贝**，而且**长度是显式的** —— 不像 C 的 `char *` 得靠 `\0` 猜（那正是 `strlen`
又慢又危险的原因）。长度交给 C 的 `sizeof` 算，所以转义和 UTF-8 都不用语言自己处理。

**字符串比较是按内容的**，靠 prelude 里的 `fn ==` 实现：

```extc
let a = "hi"
if a == "hi" { }          // ✓ 逐字节比，true
if a == "ho" { }          // ✓ false
```

> 编译器**不生成**指针比较 —— 那正是 C 字符串最常见的假答案（`"hi" == "hi"` 在 C 里
> 靠编译器池化碰巧为真，换个写法就假）。extC 里 `==` 一定走内容。

---

## 6. 语句

```extc
let / var 声明

x = 表达式                       // 赋值（目标是变量或字段）

if 条件 { ... } else if 条件 { ... } else { ... }

while 条件 { ... }

return 表达式
return                           // 无返回值函数

break
continue

{ ... }                          // 裸块，引入作用域
表达式                            // 表达式语句
```

`else` 前面可以换行（不受「换行即语句结束」影响）。

**还没有 `for`** —— 四种形态全部已定案，见第 10 节。

---

## 7. 结构体与方法

**方法写在 `struct` 体内**（定案 9），首参数必须是 `self: ref 本类型`：

```extc
struct point {
    x: i32
    y: i32

    fn moveBy(self: ref point, dx: i32, dy: i32) {
        self.x = self.x + dx
        self.y = self.y + dy
    }

    fn magnitudeSquared(self: ref point) -> i32 {
        return self.x * self.x + self.y * self.y
    }
}

fn main() -> i32 {
    var p: point = { x: 1, y: 2 }
    p.moveBy(3, 4)                  // 接收者自动取地址，调用点不用写 ref
    println(p.magnitudeSquared())   // 52
    return 0
}
```

细节：

- **接收者自动取地址**：`p.moveBy(...)` 里 `p` 是值、`self` 是 `ref point`，编译器生成 `&p`。
  反过来（接收者是 `ref T`、`self` 是值类型）会自动解引用。
  > 规则：**`.` 本身就表示「在这个值上操作」**，所以方法接收者不用写 `ref`；
  > 而**自由函数的 `ref T` 实参必须在调用点写 `ref`**（见下）。
- **方法名有命名空间**：`point.eq` 和 `board.eq` 是两个不同的名字，
  顶层不用为了避冲突而发明 `point_eq` 这种名字。C 里生成 `point_eq` 做区分。
- 字段访问按类型决定用 `.` 还是 `->`，用户不用管。
- **自由函数不能有 `self` 参数** —— `self` 只属于方法。

## 7.5 引用：`ref` 是表达式

`ref T`（只读）/ `mut ref T`（可写）都「不可为空、不可做算术」。

**`ref` 在调用点要显式写**（定案 10）——让读代码的人一眼看出这里传的是引用不是拷贝：

```extc
struct counter {
    n: i32
    fn bump(self: mut ref counter, by: i32) { self.n = self.n + by }   // 会写 ⇒ mut
    fn peek(self: ref counter) -> i32 { return self.n }               // 只读 ⇒ ref
}

fn addTo(c: mut ref counter, by: i32) {     // 自由函数的可写引用参数
    c.bump(by)                              // 方法接收者自动取地址
}

fn main() -> i32 {
    var c: counter = { n: 10 }
    addTo(ref c, 7)                     // ← 必须写 ref
    println(c.peek())                   // 17
    return 0
}
```

**取到哪种引用，由「这个地方可不可写」自动决定**（不用你写 `mut`）：

| 对什么取引用 | 得到 |
|---|---|
| `var` 变量 | `mut ref T` |
| `let` 变量 | `ref T`（只读借用**随时可以取**） |
| `mut ref` 参数 / `ref` 参数 | 跟着参数本身的可写性 |
| `var` 的字段/元素 | `mut ref T` |

- 实参**本身就是引用**时，不用再写 `ref`，直接传。
- **`let` 的变量可以取只读引用**（「只是想读一下」），但交不给要写权限的参数：
  ```extc
  let c: counter = { n: 1 }
  println(read(ref c))     // ✓ 只读借用
  addTo(ref c, 1)          // ✗ expects `mut ref counter`, found `ref counter`
  ```
- 只有变量和字段能取引用（`ref f()` 不行）。
- 值位置上自动解引用（`println(r)` 打的是**值**）；**`ref x` 自己不解引用**。见 §3。

> ⚠️ **逃逸检查还没做** —— 现在能拿到指向函数局部变量的引用/视图而不被检查，
> 而且**它是静默错**（不是崩溃）。这是 extC 唯一还开着的内存安全洞，见 §0.5。

---

## 8. 内建函数

### `print` / `println`

```extc
println(x)          // 打印后换行
print(x)            // 不换行
println()           // 只换行
println(a, b, c)    // 按顺序连续打印，中间没有分隔
```

**格式由编译器按静态类型选** —— 用户永远不写 `"%d"`。支持的类型：所有整数、浮点、`bool`、**枚举**、**struct**（递归）、**`slice<u8>`（按文本）**。

```extc
println(42)         // 42
println(3.14)       // 3.14
println(true)       // true
println("hi")       // hi
println(status.warn)// warn   ← 枚举自动有名字文本（定案 11）
```

> ⚠️ **格式串 `{}` 已定案要加**（编译期展开，不是运行时解析），但 week-0 还没实现。见第 10 节。

---

## 9. 错误信息

错误带**文件、行、列、源码上下文和插入符**：

```
examples/bad.extc:3:17: error: cannot assign to `x`, which is a `let`
      x = 2
          ^
  note: use `var` to allow reassignment (`let` is an immutable binding)
```

现在检查这些，**全部由 extC 自己报，不再漏给 gcc**：

**名字与结构**
- 未定义名字、未知函数、未知字段、未知类型
- 重名的 struct / 函数 / 字段
- 参数个数不符
- 给 `let` 赋值
- 裸 `{}` 推导不出类型

**类型（T2）**
- 初始化式 / 赋值 / 实参 / 返回值 / 结构体字段的类型不符
- 有损转换（收窄）
- 整数字面量超出目标类型范围
- 条件不是 `bool`
- 两种类型没有公共类型的运算

**结构 / 枚举 / 引用（T3）**
- 重名的 struct / type / 函数 / 字段 / 方法 / 变体
- 字段和方法同名
- 不存在的变体（`status.nope`）
- `self` 不在 struct 体内、`self` 类型不对、`self` 不是首参数
- 对 `let` 取引用、对不是变量/字段的东西取引用
- 自由函数的 `ref` 实参没写 `ref`
- 零初始化 `ref T`

**只报第一个错误**，没有错误恢复。

---

## 10. 已定案、但还没实现

> ✅ **已完成（截至 2026-09-18）**：默认零初始化、方法进 struct 体内、`type` 枚举、
> `ref` 表达式与调用点显式、泛型单态化、prelude 机制。
> **`slice<T>` 索引与字符串库**、**固定数组 `[N]T`**、**切片视图 + 可写性**、
> **`option`/`result`/`?`**、**`::` 关联函数**、**位运算**、
> **引用语义（`mut ref` + 视图可写性）** 也都在跑了。
>
> 下面剩下的**都还没做**。

| 特性 | 定案内容 | 计划 |
|---|---|---|
| **逃逸检查** | 词法深度 `depth(r) ≥ depth(v)` —— **这是 extC 的命**，也是唯一还开着的内存安全洞（见 §0.5） | **下一步** |
| **arena / `region`** | 分配绑词法作用域，永不 `free`；`region` = 显式命名的 arena | arena 到位之后（今天完全不需要分配） |
| **动态数组 `array<T>`** | 等 arena 到位 | arena 之后 |
| **算术 UB 三处** | 除零 trap 带位置、移位超宽取模（溢出已用 `-fwrapv` 兜住） | 小活，随时 |
| **全局常量 / 全局变量** | 全局 = 深度 0 的 arena；`static` 关键字因此消失。**定长全局不需要分配** | 中 |
| **输入（`readLine` / `argv`）** | 由调用者给 buffer，零分配零隐藏状态 | 中 —— 五子棋能真的跟人下的门槛 |
| **格式串 `{}`** | **编译期展开**，不是运行时解析；必须是字面量 | 中 |
| **`for` 四种形态** | `for d in dirs` / `for i in 0..n` / `for d in -2..3` / C-style | 中 |
| **`match` + 带载荷枚举** | 语句和表达式都能用；能匹配变体和常量；臂可发散 | 中 |
| **`@main` 注解** | 标在任意函数上，不再硬编码 `main` | 低 |
| **模块系统** | `module` / `export` / `import` | 低 —— 主人说不一定完全自举 ⇒ 优先级降了 |
| **`@recursive`** | 编译器展开成「显式栈 + 循环」，深度上限是编译期常数 | 低 |
| **线程** | 保守的 fork-join + 归约；「引用不过线程」 | 低 |

完整清单和理由见 [`DECISIONS.md`](DECISIONS.md)、[`PLAN.md`](PLAN.md)、
[`BOOTSTRAP.md`](BOOTSTRAP.md)（依赖顺序与优先级）。

---

## 11. 还没定的

`result<void,E>` 还是 `result<(),E>`、`@main` 和 `module main` 的优先关系、`+=` 复合赋值、`&&`/`||` vs `and`/`or`、无返回值函数要不要强制 `-> void`、要不要做多错误报告。

**`==` 右边的裸 `{}` 不推**（✅ 已定，见 [`DECISIONS.md`](DECISIONS.md) 定案 27）：

```extc
println(ps[0] == { x: 1, y: 2 })        // ✗ cannot infer the type of a bare `{}` here
println(ps[0] == point { x: 1, y: 2 })  // ✓ 写全名字
```

`==` 是语法糖，展开成 `T.==(lhs, rhs)` 后右边**确实有**参数类型可推 —— 所以这是
「能推但不推」。**主人定调：显式是对的，不要那么多自动推导。**
（顺带避开一个坑：`fn ==(self: ref point, other: point)` 与 `other: ref point` 都存在时，
「拿左边类型当右边期望类型」在左操作数是 `ref T` 时会推错。）

---

## 12. 完整示例

见 `examples/`：

| 文件 | 演示 |
|---|---|
| `tour.extc` | **语言巡礼** —— 一份能跑的完整示例，把现在能用的东西全用上了 |
| `hello.extc` | 变量、`if/else`、`while`、函数调用、打印 |
| `fizzbuzz.extc` | `else if` 链、`%`、`while` |
| `types.extc` | 拓宽自动、字面量按值适配（T2） |
| `structs.extc` | struct、**方法写在 struct 体内**、裸 `{}` 推导、**零初始化** |
| `enums.extc` | `type` 枚举、`status.ok`、枚举自动打印名字 |
| `refs.extc` | **`ref` 表达式**、自由函数实参写 `ref`、方法接收者自动取地址 |
| `generics.extc` | **泛型 `struct Name<T>`**：四份实例同时工作、嵌套 struct、实例的自动调试打印 |
| `eq.extc` | **显式定义 `fn ==`**：普通 struct、`!=` 取反、泛型里推迟到实例化检查 |
| `debug.extc` | **自动调试打印**：递归打印 struct、枚举打名字、零初始化直接打 |
| `prelude.extc` | **prelude 里的 `slice<T>`** 直接用，两份实例 |
| `strings.extc` | **字符串 = `slice<u8>`**：字面量、`len`、prelude 的方法、空串、转义 |
| `arrays.extc` | **固定数组 `[N]T`**：多维、字面量、`...` 补零、越界 trap、值语义 |
| `slices.extc` | **切片视图**：四种写法、编译期证明的零检查、透过视图写 |
| `mut-views.extc` | **`slice<T>` / `mut slice<T>`**：源头继承、就地 `reverse`/`fill`、签名说实话 |
| `refs.extc` · `mut-ref.extc` | **`ref T` / `mut ref T`**：只读借用 vs 可写借用、`let` 也能借 |
| `ref-scalar.extc` | **标量引用**：自动解引用、写穿、**`swap`**（以前写不出来） |
| `array-of-struct.extc` | 数组元素是带 `fn ==` 的 struct（含一个**回归 bug** 的守卫） |
| `option-result.extc` | **`option` / `result` / `?`**：五子棋的落子与寻位 |
| `fenwick.extc` | **树状数组**：对拍 2 万次零不一致 + 逆序对（第一道真算法题） |
| `gomoku-board.extc` | **五子棋棋盘**：G2 里程碑 |

跑测试：

```sh
./tests/run.sh     # 正例跑通 + 反例必须被编译期挡掉
```
