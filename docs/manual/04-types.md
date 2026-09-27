<!-- 本页原来是 docs/MANUAL.md 的「3. 类型」一节（原 §3.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 2. 词法](03-lexical.md) · [4. 变量与声明 →](05-vars.md)

---

# 3. 类型

### 内建类型

| 类别 | 名字 | C 里的对应 |
|---|---|---|
| 有符号整数 | `i8` `i16` `i32` `i64` | `int8_t` … `int64_t` |
| 无符号整数 | `u8` `u16` `u32` `u64` | `uint8_t` … `uint64_t` |
| 浮点 | `f32` `f64` | `float` / `double` |
| 布尔 | `bool` | `bool` |
| （字符串） | 没有内建字符串类型 —— 见下面的 **`slice<u8>`** | — |
| 空 | `void` | `void` |

**没有 `int` / `long` / `char` / `double`**（字符字面量 `'a'` 就是 `u8`，见 §2） —— 类型名一律带位宽（见 [`SYNTAX.md`](../SYNTAX.md) 的命名规范）。

字面量的默认类型：整数 `i32`，浮点 `f64`。

### 类型转换规则（T2 起）

extC **只自动做无损失的拓宽**；收窄 / 换符号 / 整数↔浮点**必须显式写出来**（`T(x)`）

| 转换 | 是否自动 |
|---|---|
| 同类型 | |
| `i8`→`i16`→`i32`→`i64` | |
| `u8`→`u16`→`u32`→`u64` | |
| `u*` → 更宽的 `i*`（如 `u32`→`i64`） | |
| `i*` → `u*`（一律，包括同宽） | 有损 |
| 能精确表示的整数 → 浮点（`i16`→`f32`、`i32`→`f64`） | |
| `i32` → `f32` | f32 尾数只有 24 位 |
| `f32` → `f64` | |
| `f64` → `f32` | 有损，必须显式写 `f32(x)` |

**字面量按值适配**（`DESIGN.md` §5 的「字面量类型推导」）：

```extc
let a: u8  = 200        // 200 放得进 u8 → 可以
let b: u16 = 60000      // 可以
let c: u8  = 300        // error: literal `300` does not fit in `u8`
let d: f64 = 7          // 整数字面量给浮点变量 → 可以
let e: f32 = 3.14       // 浮点字面量给 f32 → 可以
let f: i32 = 3.14       // error: 有损
```

### 显式转换 `T(x)`（PLAN #23）

C 的 `(T)x` 在 extC 里写成 **`T(x)`** —— 因为 extC 的 parser **不查符号表**，
`(T)x` 会跟"括号表达式"二义（C 靠符号表才分得开）。语义跟 C 一样，只是括号换了个位置：

```extc
var big: i64 = 300
var a: u8 = u8(big)          // 运行时 **trap**（装不下，带源码位置）
var b: i32 = i32(200)        //
var f: f64 = f64(7)          // 7 → 7.0
var t: i64 = i64(3.9)        // 3（向零截断 = C 的规则）
var g: f32 = f32(1.5)        // 浮点之间：就近舍入，超范围变 ±inf（C 的规则）
var u: i32 = i32(u8(200))    // **能证明装得下就不生成检查**（P）
```

| 转换 | 检查 |
|---|---|
| 拓宽（`i32`→`i64`、`u8`→`i32`、`i32`→`f64`）| 无（编译期就知道装得下）|
| 整数**收窄**（`i64`→`i32`、`i32`→`u8`）| **装不下 ⇒ trap（带位置）**|
| **换符号**（`i32`→`u32`、`u64`→`i64`）| 同上（负的转无符号 = trap）|
| **浮点 → 整数** | 超范围 / NaN ⇒ trap；否则向零截断 |
| 浮点之间（`f64`→`f32`）| 不做检查 —— 就近舍入，超范围取 ±inf（**C 的规则**；写出该转换即视为接受此语义）|

**为什么整数收窄要 trap 而不是像 C 那样悄悄截断**：静默截断正是 extC 要干掉的那种
"看不出来的错"（P′）而能证明装得下时**一个检查都不生成**，所以不花钱

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

### `match`：穷尽检查的分支

```extc
type gameError = | outOfRange | occupied | gameOver

fn describe(e: gameError) -> slice<u8> {
    match e {
        outOfRange => { return "出界了" }
        occupied   => { return "那格已经有子了" }
        gameOver   => { return "棋局已经结束了" }
    }
}
```

- **漏一个变体就编不过** —— 这就是它存在的理由。
  用 `if e == gameError.outOfRange { ... } else { ... }` 时，
  以后给枚举**新增变体时，编译器不会提示遗漏**；`match` 会
- 分支名必须是**被 match 的枚举的变体**（写错会报出该枚举的所有变体）
- 分支体可以是**一个块**，也可以是**一个语句**（`outOfRange => n = 1`）
- 被 match 的表达式**只求值一次**（生成的 C 就是一个 `switch`）
- **是语句，不是表达式** —— 它不返回一个值（跟 `?` 同一个理由：C 没有语句表达式）。
  想往外传就在分支里 `return` / 赋值
- 暂时**没有 `_ =>` 兜底**：穷尽列出所有变体才是它的价值。

### 带载荷枚举（tagged union）：变体自己带数据

无载荷回答「**是哪一种**」；带载荷回答「**是哪一种 + 附带的东西是什么**」。

```extc
type shape = | circle(f64) | rect(f64, f64) | dot

let a = shape.circle(2.0)        // 构造：类型名.变体名(载荷...)
let b = shape.rect(3.0, 4.0)
let c = shape.dot                // 无载荷变体照旧

match a {
    circle(r)    => { println(r) }          // 载荷**绑定**出来，位置对位置
    rect(w, h)   => { println(w * h) }
    dot          => { println("点") }
}
```

C 里这个东西要**手写**，而且容易写错：

```c
struct shape { int tag; union { double r; struct { double w, h; } rect; } u; };
// 读 u.r 之前你得**自己记得**先看 tag 是不是 0 —— 忘了就是 bug
```

此处由编译器记录：**取载荷之前必须先 match 该变体**

**几条要记住的**：

| | |
|---|---|
| **零值 = tag 0** | 所以**变体顺序有意义**。`type box = \| nothing \| holding(slice<u8>)` 的 `var b: box` 合法（零值是 `nothing`）；把 `holding` 排在第一位就不合法（含 `ref` 的类型没有零值）|
| **打印** | 打印的是**变体名**（`circle`），载荷不打印 —— 想要具体格式自己写方法 |
| **没有 `==`** | C 里它是 struct，而 C 的 struct 不能用 `==` ⇒ 要比就 `match`，或者写一个方法 |
| **载荷里可以有 `ref` / 视图** | 这正是无载荷的 `option<T>` 做不到的事（见 `IO.md`：`none` 得给 `value` 字段填值，而 `ref` 没有零值）|



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
    fn set(self: mut ref box<T>, v: T) { self.value = v }   // 写字段 ⇒ mut ref
    fn get(self: ref box<T>) -> T { return self.value }
}

struct point { x: i32  y: i32 }

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

> **引用安全规则按实例复查**（2026-09-20 起）
>
> 模板期 `T` 不透明，"这个类型里有引用吗"答不了 —— 所以**涉及 `T` 的引用规矩
> （深度 / 借来的值 / 零值）会推迟到实例化，按实例再查一遍**：
>
> ```extc
> struct boxT<T> {
>     v: T
>     fn stash(self: mut ref boxT<T>, value: T) { self.v = value }
> }
> var b: boxT<i64> = { v: 5 }          // 通过（i64 里没有引用）
> b.stash(7)
> var c: boxT<slice<u8>> = { v: "abc" }
> c.stash(someSlice)                    // 报错，而且要**点名实例**：
> // error: in instance `boxT_slice_u8`: cannot store a borrowed value into
> //        something that outlives this call
> ```
>
> 同一条也管**零值**（`var local: T` ⇒ 实例化成 `slice<u8>` 就是一个 null 引用）
> 和**返回值**（返回本帧的 `T` ⇒ 悬垂）
> 实现见 `DECISIONS.md` 定案 54；**没有**为了安全把 `box<i64>::set` 这种正常写法拒掉

> **已知限制（「模板检查一遍」换来的代价）**
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
bump(ref n)          // var ⇒ 取出来就是 mut ref
let m: i64 = 7
// bump(ref m)       // 对 let 只能取到只读引用 ⇒ 交不给要写权限的参数
```

**降级是自动的、单向的**：`mut ref T` 可以当 `ref T` 用（能写当然能读）；
反过来不行，必须显式写 `mut` —— 「安全是默认」的直接体现。

#### 引用就是引用：当值用要写 `*p`

`ref T` / `mut ref T` 是**一等值**，不会因为"出现在值的位置"就自动解引用 —— 想拿所指的值，
显式写 `*p`（定案 ㉝）：

```extc
fn bump(p: mut ref i64) { *p = *p + 1 }      // 读要用 *p，写也要用 *p
fn swap(a: mut ref i64, b: mut ref i64) {
    let t: i64 = *a
    *a = *b
    *b = t
}

fn main() -> i32 {
    var n: i64 = 10
    bump(ref n)              // n = 11
    println(n)               // 11
    var r: ref i64 = ref n
    let m: i64 = *r          // 想拷值就显式解引用
    println(m)               // 11
    return 0
}
```

两条要记住的规则：

- **只有内建 `println` 打引用时自动解引用**（地址不可打印，`println(r)` 打的就是值）；
  其它地方当值用一律要写 `*p`，漏了会报
  `` `mut ref i64` is a **reference**, not a value -- dereference it first: `*p` ``。
- **`=` 在引用上只有一个意思：换指向**。写穿要写 `*p = v`。

```extc
fn main() -> i32 {
    var n: i64 = 10
    var r: mut ref i64 = ref n
    *r = *r + 5              // 写穿：n 变成 15
    println(n)               // 15

    var a: i64 = 1
    r = ref a                // 换指向：r 现在指 a
    *r = 7                   // 写穿：a 变成 7
    return 0
}
```

**字段里的引用也一样**：

```extc
struct slot { p: mut ref i64 }

fn main() -> i32 {
    var a: i64 = 1
    var b: i64 = 2
    var s: slot = { p: ref a }
    *s.p = 5                 // 写穿：a 变成 5
    s.p = ref b              // 换指向：字段现在指 b
    return 0
}
```

换指向也要过逃逸检查（新指向的东西不能活得比这个引用短）

#### 透过只读引用写 = 编译错误

```extc
fn f(b: ref board) { b.cell[0] = 1 }
// error: cannot write through a read-only reference
//   note: `ref T` is a **read-only** borrow; writing through it needs `mut ref T` …
```

**"穿过"有两种写法，两种都查**（PLAN #40/#41，2026-09-22 修齐）：

```extc
fn h(p: ref node) { (*p).val  = 7 }    // 显式穿过 `*p` ⇒ 要 mut ref 报错
fn k(p: ref node) {  p.next   = null } // 隐式穿过（字段住在 `*p` 里）⇒ 也要 mut ref 报错

fn ok(cell: mut ref node) { (*cell).next = null }   // mut ref ⇒ 合法
```

**判据一句话**：写一个「地方」要**穿过**哪些引用，每一只都得是 `mut ref`
**目标自己的类型不算** —— `cur = v`（换指向）写的是**槽位**，
跟"`cur` 是不是只读引用"无关（`var cur: ?ref node` 照样可以换指向）

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
let c = [1, 2, 3]                // 长度 3 从字面量推出来
```

**数组是值类型**：赋值、传参、返回都是**整份拷贝**。

```extc
var x: [3]i32 = [1, 2, 3]
var y = x          // y 是独立的一份
y[0] = 99          // x[0] 仍是 1
```

> 取值为值语义，与 extC 的核心一致（默认值语义、无别名）。
> 900 字节的拷贝确实贵 —— 要省就用 `ref` 显式表达，**别让读者猜**。
> 编译到 C 时套一层 struct（`typedef struct { int32_t data[5]; } array_5_i32;`），
> 因为 C 的裸数组既不能赋值也不能返回。

**索引 `a[i]` 带边界检查**，越界 trap 并报 extC 位置：

```extc
println(a[0])      // 1
println(a[7])      // trap: index 7 out of range (length 5) —— 带文件名和行号
```

**切片视图 `a[lo..hi]`** —— 四种写法，**不拷贝数据**（见 [`ARRAYS.md`](../topics/ARRAYS.md) §4）：

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
| `var a` | `mut slice<T>` | |
| `let a` | `slice<T>` | |
| `"字面量"` | `slice<T>` |（在只读段） |
| `mut ref` 参数 | `mut slice<T>` | |
| 只读视图 | `slice<T>` | |

```extc
var a: [6]i32 = [1, 2, 3, 4, 5, 6]
var mid = a[1..4]         // var ⇒ mut slice<i32> 能写
reverse(mid)              // 就地反转

let frozen: [3]i32 = [7, 8, 9]
// frozen[0] = 1          // let ⇒ 不能写
// fill(frozen[..], 0)    // 从 let 切出来是只读视图

var s = "abc"
// s[0] = 88              // 字面量是只读视图 ⇒ **编译错误（以前是段错误）**
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

**第三刀（2026-09-20）之后它们就是两个普通的泛型枚举**（`stdlib/prelude.extc` 里只有两行）：

```extc
type option<T>    = | none | some(T)
type result<T, E> = | failure(E) | success(T)
```

| 类型 | 意思 | 零值 |
|---|---|---|
| `option<T>` | 可能有值，也可能没有 | **就是 `none`**（tag 0） |
| `result<T,E>` | 成功时为值，失败时为错误 | **`failure(E 的零值)`**（tag 0） |

「零值就是 `none` / `failure`」不是巧合 —— 它是定案 8（默认零初始化）的直接结果
（**所以变体顺序有意义**：写成 `| some(T) | none` 就是错的）。
因为它们是普通枚举，`option<slice<u8>>` **合法**（里面有 ref 也照样有零值）——
这正是 IO 需要的东西（见 `examples/option-ref-payload.extc`）。

**`?T` 就是 `option<T>` 的语法糖**（定案 ㊻）—— 类型位置写 `?i64` 跟写 `option<i64>` 完全一样：

```extc
fn half(n: i64) -> ?i64 {            // ≡ -> option<i64>
    if n % 2 != 0 { return none }    // 裸写 `none`（类型从返回类型来）
    return some(n / 2)               // 裸写 `some(x)`
}

struct config { retries: ?i32 }      // 字段也能用
var c: config                        // option 有零值 ⇒ 不用初始化式
c.retries = some(3)                  // 赋值位置也认裸构造器
println(pick(none, some(9)))         // 实参位置也认

let a = option<i64>::some(42)        // 想写全名当然也可以（不靠上下文猜）
let b = option<i64>::none()          // 也可以：var b: option<i64>  ← 零值就是 none
match a {
    some(v) => { println(v) }
    none    => { println("没有") }
}

let r = result<unit, gameError>::failure(gameError.occupied)
match r {
    success(u) => { println("成功了") }
    failure(e) => { println("失败：", e) }      // e 就是那个错误枚举
}
```

#### 一页速查：拿到一个 `option` / `result` 之后能干什么

```extc
// ① 造一个 —— 裸写的 `some` / `none` / `success` / `failure` 在**类型已知**的四个
//    位置都认：return、带标注的 var、赋值、实参（类型不清楚就要写全名）
fn find(...) -> option<i32> { return some(3) }        // 有
fn find(...) -> option<i32> { return none }           // 没有（括号也能省）

// ② 用的时候只有两招：
//    招数一：`match` —— **读值唯一的标准姿势**（穷尽检查：少写一个变体编不过）
match find(3) {
    some(v) => { println("找到了 ", v) }
    none    => { println("没有") }
}

//    招数二：`?` —— 不成功我就不干了（顺着往上抛）
let v = find()?          // 成功 → v 是里面的值；失败/没有 → 顺着往上抛
                         //   （本函数的返回类型必须装得下那个失败）
```

#### 读值的三个记号（`?` 那一族，全是**编译器**提供的语法，不是库）

| 记号 | 意思 | 代价 |
|---|---|---|
| `match` | 分支，穷尽检查 | 零 |
| `e?` | 不成功就顺着往上抛 | 零 |
| **`e ?? 兜底`** | 可能没有就兜底（`?ref` 也能用：null 就换一个）| 零（C 三元，**只算一边**）|
| **`e!`** | **调用者签字**：直接取值（无值或为 null 即视为断言失败）| 零（**不生成任何检查**）|

```extc
let v = opt ?? -1                 // valueOr 就是它 —— 而且对所有 T 通用
println(opt! + 1)                 // 我保证有值（签字，无检查）
println(p!.value)                 // ?ref：我知道非空
```

⇒ 所以 **prelude 里不需要 `unwrap()` / `valueOr` / `has` / `ok`** ——
那些名字当年存在，只是因为当时 `match` 不了、也没有 `??` / `!`
（也因此**不用**去给枚举加方法、也不用泛型自由函数：语法糖把这件事解决掉了）

`!` 表示"**断言由调用者负责**"，不是"检查的宽松版本"：确实没有值即为 UB。
需要可报错的版本时，改写为 `match` 并给出所需的失败处理

`?` 的语义是「**不成功即向上返回**」。它在**四个位置**合法（见下节）。

用**关联函数**构造：写在 `struct` 体内但**不带 `self`** 的函数，
调用时类型写全（不靠上下文猜，见 [`DECISIONS.md`](../DECISIONS.md) 定案 27/29）：

```extc
struct box<T> {
    value: T
    fn make(v: T) -> box<T> { return { value: v } }   // 关联函数
}
let b = box<i32>::make(5)
```

#### 在 `return` 位置上可以**裸写**构造器（定案 49）

```extc
fn at(...) -> option<i32> {
    if x < 0 { return none() }               // ← 不用写 option<i32>::none()
    return some(b.cell[y][x])
}

fn place(...) -> result<unit, gameError> {
    if 那格有子 { return failure(gameError.occupied) }
    return success(unit {})
}
```

**类型从函数签名里写好的返回类型来** —— 这不是"推导"：
编译器**不做推断**，依据的是源码中写出的那一行 `-> result<unit, gameError>`。
省掉的纯粹是重复劳动

只有**这四个名字**（`success` / `failure` / `some` / `none`），而且**类型必须是已知的** ——
能认的四个位置：**`return` / 带标注的 `var` / 赋值 / 实参**。
类型不清楚（比如裸的 `var x = some(1)`）就照旧走普通查找，报"未定义的名字"。
名字被用户自己的函数占着时也照旧走普通查找

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

**载荷类型可以不同**（里层 `result<i64, E>` 放进返回 `result<point, E>` 的函数），
**错误类型必须相同** —— `?` 是原样转发，不编造转换。

别的写法（`f(e? + 1)`）会**报错**，不会生成编不过的 C。

**`?` 的载荷可以有 `ref` / 视图**（`option<slice<u8>>`、`result<slice<u8>, E>` 都行）——
第三刀把 `option`/`result` 改成普通枚举之后就自然成立了（tag 0 的载荷根本不存在）。

已知限制：`?` 是**原样转发**，所以里层的错误类型必须跟外层**一模一样**；
而且 `?` 只在上面那四个位置合法（`f(e? + 1)` 要自己拆成两句）。

---

**[索引](README.md)** · [← 2. 词法](03-lexical.md) · [4. 变量与声明 →](05-vars.md)
