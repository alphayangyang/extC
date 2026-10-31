# extC 语言现状

本页写给要写 extC 程序的人：**现在能写什么、行为是什么**。
语言以外的内容见
[`stdlib.md`](stdlib.md)（标准库）与 [`modules-cabi.md`](modules-cabi.md)（模块与 C 互操作），
还没做的在 [`../TODO.md`](../TODO.md)。

编译器：`./build/extc [options] <file.extc>`；`--run` 编译并运行，`--build` 只编译，
`--dump-tokens` 只看词法，`--ccflag` / `-l` / `-L` / `--pkg-config` 透传 C 编译链接。
帮助：`./build/extc --help`。

---

## 1. 一个完整的程序

| 位置 | 写法 |
|---|---|
| 入口 | `fn main() -> i32 { ... }` 或 `fn main() { ... }`（无返回类型 = `void`，退出码 0） |
| 导入 | `use a::b` · `use a::b::*` · `use a::b::{x, y}` · `use a::b as c` |
| 预置 | `option` / `result` / `slice` / `varArray` / `unit` / `pcg32` / `coroutine` / `domain` 无需导入（prelude 里就有） |
| 控制台 | `use std::io` 后写 `io::cout << x << "\n"`；`io::cin >> x` 读 |
| 语句 | 不需要分号；写了分号会被忽略。换行只是**空白**，表达式在下一个记号接不下去时结束 |
| 续行 | 接不下去的记号**不能**起新行：`io::cout << a` 后换行再写 `<< b` 是错的（`<<` 不能续行）|
| 注释 | `// 行注释` · `/* 块注释 */`（**不嵌套**，遇到第一个 `*/` 就结束） |

语句之间**不必换行**：`let a = 1 let b = 2` 是两条声明。
反过来，`+` 写在下一行**开头**不会接上上一行：

```extc
// 结果：输出 8 / loop / loop
use std::io

fn add(a: i32, b: i32) -> i32 {
    return a + b
}

fn main() -> i32 {
    let x: i32 = 3
    var y: i32 = 4
    y = y + 1
    if x < y {
        io::cout << add(x, y) << "\n"     // 8
    } else {
        io::cout << x << "\n"
    }
    var i: i32 = 0
    while i < 2 {
        i = i + 1
        io::cout << "loop\n"
    }
    return 0
}
```

```extc
// 结果：输出 12 和 3
// —— `let a = 1 let b = 2` 是两条语句；`1 +` 换行接得上，`+ 1` 换行接不上
use std::io
fn main() -> i32 {
    let a = 1;                // 分号可写可不写
    let b = 2
    let c = 1 +
            2
    io::cout << a << b << "\n"     // 12
    io::cout << c << "\n"          // 3
    return 0
}
```

- `use` 可以写在文件里的任何位置（整个模块一起解析，实测把 `use std::io` 放在 `main` 之后照样跑）；名字是否解析得到由模块搜索路径（`-I <dir>`）决定。
- 顶层再写 `global` 是冗余但合法的自文档化：`global var x: i64` ≡ `var x: i64`。
- `println` / `print` 是**内建且已废弃**的函数：编译会报
  `warning: 'println' is deprecated`。控制台只有 `io::cout` 这一条路。

---

## 2. 词法

### 2.1 记号

| 记号 | 拼写 | 说明 |
|---|---|---|
| 整数 | `42` · `0x2A` · `0X2a` | 十进制或十六进制；范围是有符号 64 位，越界报 `integer literal exceeds the supported signed 64-bit range` |
| 浮点 | `3.14` · `1e9` · `2.5E-3` | `.` 后面必须跟数字（`1..2` 是区间、`1.foo` 是字段访问）；`e`/`E` 后可选 `+`/`-` |
| 字符 | `'a'` · `'\n'` · `'\x41'` | **就是一个整数常量**（携带该字节值）；恰好一个字节，多字节报错 |
| 字符串 | `"你好"` | 内容不含引号；**不能跨行**（跨行报 `unterminated string literal`） |
| 转义 | `\n \t \r \0 \\ \' \" \a \b \f \v \xHH` | 字符串与字符共用一份；`\xHH` 取 1–2 位十六进制；其他报 `unknown escape` |
| 标识符 | `[A-Za-z_][A-Za-z0-9_]*` | 源语言是 ASCII |

保留字：`fn let var if else while for return match break continue struct type true false ref mut use extern`。
`trait` / `impl` / `dyn` / `global` / `yield` / `trap` / `ext` 是**上下文词**（按形状识别），普通变量可以叫这些名字。

`__` 开头的标识符禁止使用（编译器自己生成的名字都用这个前缀）：

<!-- expect-error -->
```extc
// 结果：error: identifiers starting with `__` are reserved for the compiler
fn main() -> i32 { let __x = 1  return 0 }
```

### 2.2 格式化字符串 `f"…"`

- 标识符 `f` **紧挨着**引号（中间有空格就是两个记号）。
- 槽位 `{表达式}`；`{{` / `}}` 表示字面花括号；每槽只求值一次。
- 槽里的表达式可以含**字符串/字符字面量**：`f"c={"hi"}"` 的内层 `"` 属于槽，不结束格式串
  （`f"{ord('a')}"` 同理）。缺右引号时报 `unterminated f-string literal`，而不是把它当普通
  字符串续到行尾。
- 两个位置：`<<` 的**右操作数**（`io::cout << f"…"`，流式输出，逐段就地写出）与
  **值位置**（`let s: string = f"…"`，展开成 `stl::string::from(文字段) + 槽 …` 的 `+` 链，
  结果是 `string`）。值位置要求模块 `stl::string` 被 import 过（报错会提示）。
- 值位置也接受格式说明符：带 `:…` 的槽按**槽值的静态类型**落成
  `stl::stringio::padI64` / `padF64` / `padText` 调用（因此值位置带说明符时还需要
  `use stl::stringio`，报错会提示），语义与 `<<` 右侧的说明符逐条相同。
- 格式说明符：槽里写 `{值:说明符}`，语法 `[[fill]align][0][width][.prec][type]`。
  - 对齐 `<` 左 / `>` 右 / `^` 居中，前面可加一个填充字节（`{:*^8}`）；不写对齐时文本靠左、数字靠右。
  - `0` 是零填充（默认右对齐），负号在数字前：`{n:04}` 遇到 -5 输出 `-005`；宽度按**字节**算。
  - `.精度` 只对浮点有意义（`{v:.2f}`），类型码 `d` / `f` / `s` 只做一致性校验。
  - 能带说明符的只有输出流上的整数、浮点、`slice<u8>` / `mut slice<u8>`；别的类型或接收者不是流时报
    `` `T` cannot take a format specifier ``。
- 空槽 `{}`、未闭合 `{`、多余的 `}`、整体为空的 `f""`、空说明符 `{x:}` 都是编译错误。

```extc
// 结果：tab:[	] nl:[\n] quote:["] backslash:[\] hex:[A]
//       10 65 92
//       n=42 {brace} 世界
//       [    42][0042][3.14]
use std::io
fn main() -> i32 {
    io::cout << "tab:[\t] nl:[\\n] quote:[\"] backslash:[\\] hex:[\x41]\n"
    io::cout << i64('\n') << " " << i64('\x41') << " " << i64('\\') << "\n"
    let n = 42
    let v: f64 = 3.14159
    io::cout << f"n={n} {{brace}} 世界\n"
    io::cout << f"[{n:>6}][{n:04}][{v:.2f}]\n"
    return 0
}
```

---

## 3. 类型

### 3.1 内建类型

| 类型 | 位宽 | 零值 | 备注 |
|---|---|---|---|
| `i8 i16 i32 i64` | 8/16/32/64 | 0 | 有符号 |
| `u8 u16 u32 u64` | 8/16/32/64 | 0 | 无符号 |
| `f32 f64` | 32/64 | 0.0 | 浮点 |
| `bool` | — | `false` | 不能与整数互转 |
| `void` | — | — | 只作返回类型；`ref void` 是"没有操作的地址" |

没有 `char`、没有 `string` 关键字。字符串字面量的类型是 `slice<u8>`（字节视图，零拷贝）；
`stl::string` 是标准库里可增长的那个类型（见 [`stdlib.md`](stdlib.md)）。

### 3.2 聚合

| 写法 | 例子 | 说明 |
|---|---|---|
| 结构体 | `struct pt { x: i32  y: i32 }` | 字段按**声明顺序**排布 |
| 方法 | 写在 `struct` 体内或 `impl pt { … }` 里 | 接收者必须显式声明：`self: ref pt` / `self: mut ref pt` |
| 无载荷枚举 | `type color = \| red \| green \| blue` | 零值 = 第一个变体 |
| 带载荷枚举 | `type shape = \| circle(f64) \| rect(f64, f64) \| dot` | 载荷没有字段名，按位置绑定；零值 = 第一个变体（其载荷必须可零初始化） |
| 定长数组 | `[3]i32`、`[15][15]i32` | 长度是类型的一部分；`[1, 2, ...]` 的 `...` 把剩下的补零 |
| 视图 | `slice<T>` | `{ data: ref T, len: i64 }`；`slice<u8>` 就是字符串 |
| 可空引用 | `?ref T`（可写：`mut ?ref T`） | 零值是 `null` |
| 函数类型 | `fn(i64) -> i64` | 裸代码指针，**没有零值**（可空写 `option<fn …>`） |
| 可空值 | `?T`（非引用）= `option<T>` | `?i64` ≡ `option<i64>` |

构造：

```extc
// 结果：1 3 9 / 2 / green / 42 / 3
use std::io
struct pt { x: i32  y: i32
    fn new(x: i32, y: i32) -> pt { return { x: x, y: y } }
    fn sum(self: ref pt) -> i32 { return self.x + self.y }
}
type color = | red | green | blue
type shape = | circle(f64) | rect(f64, f64) | dot
type maybe<T> = | nothing | just(T)
fn main() -> i32 {
    let a: pt = pt { x: 1, y: 2 }    // 带类型名的字面量
    let b: pt = { x: 3 }             // 裸 {}，类型从上下文来；省略的字段 = 零值
    let c = pt(4, 5)                 // T(...) 调用名为 new 的关联函数
    let d = shape.circle(2.0)        // 带载荷变体：类型名.变体名(载荷)
    let e = color.green              // 无载荷变体：类型名.变体名
    let f = maybe<i64>::just(42)     // 泛型枚举：名字<实参>::变体(载荷)
    let g: ?i64 = some(3)            // 类型已知时可省掉前缀（some/none/success/failure）
    io::cout << a.x << " " << b.x << " " << c.sum() << "\n"
    match d { circle(r) => { io::cout << r << "\n" } rect(w, h) => { io::cout << w << "\n" } dot => { io::cout << "dot\n" } }
    match e { red => { io::cout << "red\n" } green => { io::cout << "green\n" } blue => { io::cout << "blue\n" } }
    match f { nothing => { io::cout << "nothing\n" } just(v) => { io::cout << v << "\n" } }
    io::cout << (g ?? i64(0)) << "\n"
    return 0
}
```

### 3.3 零值与字面量适配

不带初始化式就**零初始化**；没有零值的类型必须给初始化式，否则报错。

| 类型 | 零值 |
|---|---|
| 数值 / `bool` | 0 / `false` |
| `struct` | 每个字段各取零值 |
| 定长数组 | 每个元素各取零值 |
| 无载荷枚举 | 第 **1** 个变体 |
| 带载荷枚举 | 第 **1** 个变体 + 其载荷的零值（所以 `\| circle(f64) \| dot` 可以，把 `slice<u8>` 放第一个变体不行） |
| `option<T>` | `none` |
| `result<T, E>` | `failure`（`E` 的零值） |
| `?ref T` | `null` |
| `ref T` | **没有零值**（`cannot zero-initialize …: it contains a reference`） |
| `fn(…) -> R` | **没有零值**（`cannot zero-initialize …: it contains a fn`；可空写 `option<fn …>`） |

字面量按**值**适配目标类型；但只有**裸字面量**会适配，取负号之后就不是：

```extc
// 结果：输出 200 1 -1
use std::io
fn main() -> i32 {
    let a: u8 = 200        // OK：200 放得进 u8
    let b: i32 = 1         // 裸整数字面量默认 i32
    // let c: i8 = -1      // ✗ error: initializer expects `i8`, found `i32`
    let d: i8 = i8(-1)     // OK：显式转换
    io::cout << i64(a) << " " << b << " " << i64(d) << "\n"
    return 0
}
```

字面量也能当**方法接收者**：`21.doubled()` 里 `21` 还没有自己的类型，于是在"字面量放得进的
宽度"里找方法集（与运算符找重载同一条规则，默认宽度 `i32` 上有同名方法时优先 `i32`）。所以
`impl u32 { fn triple(…) }` 之后写 `5.triple()` 就行，不必先 `u32(5)`。

```extc
// 结果：00000 / c=red / s=dot / o=none / r=failure 0
use std::io
struct pt { x: i32  y: i32 }
type color = | red | green | blue
type shape = | dot | circle(f64)
fn main() -> i32 {
    var p: pt
    var a: [3]i32
    var c: color
    var s: shape
    var o: ?i64
    var r: result<i64, i32>
    io::cout << p.x << p.y << a[0] << a[1] << a[2] << "\n"
    match c { red => { io::cout << "c=red\n" } green => { io::cout << "c=green\n" } blue => { io::cout << "c=blue\n" } }
    match s { dot => { io::cout << "s=dot\n" } circle(v) => { io::cout << "s=circle\n" } }
    match o { none => { io::cout << "o=none\n" } some(v) => { io::cout << "o=some\n" } }
    match r { failure(e) => { io::cout << "r=failure " << e << "\n" } success(v) => { io::cout << "r=success\n" } }
    return 0
}
```

### 3.4 转换

**隐式只允许无损**：

| 允许 | 条件 |
|---|---|
| 同符号整数拓宽 | `i32 → i64`、`u8 → u32` |
| 无符号 → 更宽的**有符号** | `u32 → i64`（`u64 → i64` 不行） |
| 整数 → 浮点 | 位数 ≤ 尾数（`f32` 24 位、`f64` 53 位）：`i32 → f64` 可以，`i32 → f32` 不行 |
| `f32 → f64` | |
| 有符号 → 无符号、任何收窄、浮点 → 整数 | **不允许**，必须写出来 |

显式转换写成 `T(x)`（`T` 是内建标量名）：

- 无损的转换不生成检查（`i32(u8value)` 是白写）。
- 有损转换（收窄、换符号、浮点→整数）**生成范围检查**，越界就 trap
  `value does not fit in the target type`（带 file:line，退出码 1）。
- `i64(p)` / `u64(p)` 是**取引用 p 的地址**（单向：数字不能变回引用）。
- `fn(i64)->i64(p)` 把非空 `ref void` 变成可调用的代码指针。

```extc
// 结果：300 300 200 3 7 4000000000 100
use std::io
fn main() -> i32 {
    let a: i32 = 300
    let b: i64 = a
    let c: f64 = a
    let d: u8 = u8(200)
    let e: i64 = i64(3.9)          // 浮点→整数：截断，带范围检查
    let f: f64 = f64(7)
    let g: i64 = u32(4000000000)
    let h: i8 = i8(100)
    io::cout << b << " " << c << " " << i64(d) << " " << e << " " << f << " " << g << " " << i64(h) << "\n"
    return 0
}
```

---

## 4. 泛型

| 能写 | 例子 |
|---|---|
| 泛型结构体 | `struct pair<A, B> { first: A  second: B }` |
| 泛型枚举 | `type maybe<T> = \| nothing \| just(T)` |
| 泛型自由函数 | `fn indexOf<T>(a: slice<T>, x: T) -> i64` |
| 显式实参 | `indexOf<i32>(v[..], 30)`、`maybe<i64>::just(42)` |
| 从实参推断 | `indexOf(v[..], 30)` |
| 从上下文推断 | `let p: pair<i64, u8> = dup(3, u8(4))`（`3` 不带 `A`，左边的注解补上） |

- 类型参数名**必须大写开头**（`type parameter 't' must start with an uppercase letter`），
  类型名必须小写开头，两套名字不会撞。
- **单态化**：每个实例生成一份独立的 C 函数/结构体。`f<i32>` 与 `f<i64>` 是两个 C 符号。
- 推断先看**实参**；实参带不出的类型参数，用**上下文期望的（返回）类型**补
  （`let p: pair<i64, u8> = dup(3, u8(4))`、`return`/实参/初始化位置同理）。两边都没有见证
  才报 `cannot infer type parameter 'T' of …`，此时写显式实参 `f<i32>(...)`。
  实参与上下文对不上的时候，仍由赋值/返回那一步报"期望什么、实得什么"。
- 类型参数上的运算符与方法**推迟到实例化再查**：`a == b` / `a != b` / `a.hash()` 在
  `T` 具体之后检查，缺了报 `` `indexOf_pt` needs `pt` to define `==` ``。
  `<` / `>` 同样推迟（`lt_plain needs plain to define <`）。
- 泛型体里**可以**调用泛型函数；实例化时调用点会重指到对应实例。
- 现在没有 `T: Trait` 上界语法；泛型体只能假设 `T` 支持 `==` / `!=` 与方法调用。

```extc
// 结果：1 1 2
use std::io
struct pair<A, B> { first: A  second: B
    fn getFirst(self: ref pair<A, B>) -> A { return self.first }
}
fn indexOf<T>(a: slice<T>, x: T) -> i64 {
    var i: i64 = 0
    while i < a.len {
        if a[i] == x { return i }
        i = i + 1
    }
    return 0 - 1
}
fn main() -> i32 {
    var p: pair<i32, bool> = { first: 1, second: true }
    var v: [3]i32 = [10, 20, 30]
    io::cout << p.getFirst() << " " << indexOf(v[..], 20) << " " << indexOf<i32>(v[..], 30) << "\n"
    return 0
}
```

---

## 5. 协议：`trait` 与 `dyn`

| 写法 | 例子 |
|---|---|
| 声明协议 | `trait Tag { fn tag(self: ref Self) -> i64 }` |
| 为某类型实现 | `impl Tag for box { fn tag(self: ref box) -> i64 { … } }` |
| 给类型挂普通方法 | `impl box { fn … }`（`impl` 也能给内建标量挂方法） |
| 静态分发 | 直接 `b.tag()` —— **不生成方法表、无间接调用** |
| 动态分发 | `dyn Tag(b).tag()`、把 `dyn Tag` 存进字段/数组/容器再 `xs[i].tag()` |

**`trait` 的规则**

- 协议名必须**大写开头**（第三个命名空间，不会和类型名/类型参数撞）。
- 协议体里只能有 `fn` 签名，没有函数体、不能有字段；`Self` 表示实现类型。
- 同一协议不能对同一类型实现两次（`` `Tag` is already implemented for `box` ``）；
  实现里的方法签名必须和协议一致；`impl` 的目标类型不能在别的模块里（orphan 规则）。
- 一个方法名在同一类型上不能既来自内在实现又来自协议，也不能重复实现。
- `impl` 的目标可以是**内建标量**：`impl i64 { fn double(self: ref i64) -> i64 { … } }`
  之后 `x.double()` 可用（实测 `5.double()` 输出 10）。

**`dyn` 的规则**

- `dyn Trait(值)` 把一个实现了该协议的值变成 `dyn Trait`；载荷类型必须实现了该协议。
- 调用点只知道 `dyn Trait`，方法经**表**派发；`dyn Trait` 是普通值，能存字段、进数组、
  进 `varArray<T>`、被函数返回（返回时提升进调用者的地方）。
- 接收者可以是 `ref dyn Trait` / `mut ref dyn Trait`。
- 拒绝的写法（都有明确诊断）：对 `dyn` 值取字段
  （`must be followed by a method call`）、契约方法没有 `self`
  （`has no 'self'`）、返回 `Self` 的方法（`returns 'Self' and cannot be dispatched through 'dyn'`）。
- 陈旧 `dyn` 值（它所在的地方已经退出）在派发前被挡住并 trap，不会调到别的实现。

```extc
// 结果：12 12
use std::io
trait Shape { fn area(self: ref Self) -> i64 }
struct rect { w: i64  h: i64 }
struct circle { r: i64 }
impl Shape for rect   { fn area(self: ref rect)   -> i64 { return self.w * self.h } }
impl Shape for circle { fn area(self: ref circle) -> i64 { return i64(3) * self.r * self.r } }
fn main() -> i32 {
    var xs: [2]dyn Shape
    xs[0] = dyn Shape(rect { w: i64(3), h: i64(4) })
    xs[1] = dyn Shape(circle { r: i64(2) })
    io::cout << xs[0].area() << " " << xs[1].area() << "\n"
    return 0
}
```

---

## 6. 变量与可变性

| 顶层 | 局部 | 含义 |
|---|---|---|
| `let NAME: T = e` | `let x: T = e` | 只读**命名**：不能重新赋值，也不能透过它写 |
| `var NAME: T = e` | `var x: T = e` | 存储：可以赋值、可以取可写引用 |
| （`global` 前缀） | `global let/var x` | 冗余写法，顶层本来就是全局 |
| `let NAME: T`（无 `=`） | `let x: T` | 零初始化（类型必须写出来） |
| `var A, B: T = e1, e2` | `var a, b: i32 = 1, 2` | 一条声明绑多个名字：类型写一次，所有名字共享 |
| `var A, B = e1, e2` | `var a, b = 1, 2.5` | 多名字声明不写类型：每个名字从自己的值推断 |
| `var A: T, B: U = e1, e2` | `var a: i32, b: i64 = 1, 2` | 多名字声明：每个名字各写各的类型 |

- 顶层初始化式必须是**常量**：字面量、字面量的算术、字符串、枚举常量、字段都是常量的
  结构体字面量。调用、别的全局名不行（`the global 'A' must be initialized with a constant`）。
- 含引用的类型不能零初始化成全局（`it contains a reference`）。
- `let` 是**命名**不是存储：同一层可以再 `let` 同名（遮蔽），甚至可以换类型；
  被遮蔽的旧名字连同它的存储留在原地，之前取到的引用仍然指向它。
- `var` 是存储：同一层重复声明同名报 `` `v` is already declared in this scope ``。
- 参数可以被同层的 `let` 遮蔽。
- 多名字声明 `var a, b: i32 = 1, 2` 只是一条声明绑多个名字，语义和两条单名字声明相同。
  类型可以只写在**最后一个**名字后面，表示前面没写类型的名字共享它（`var a, b: i32`）；
  也可以每个名字各写一个（`var a: i32, b: i64 = 1, 2`）；什么都不写时，每个名字从**自己的**值推断
  （`var a, b = 1, 2.5` ⇒ `a` 是 `i32`、`b` 是 `f64`）。混写时自带的类型优先，末尾的类型只填给前面没写的。
- 名字数必须等于初始化式数，对不上报 `` `var` binds 2 name(s) but gets 1 initializer(s) ``。
  写法是「先名字、再一个 `=`、再按顺序的值」，所以给每个名字各写一个初始化式（`var a = 1, b = 2`）
  报 `` expected one value per name, not `name = value` pairs ``。
- 一条声明里重复同一个名字报 `` duplicate name `a` in one declaration ``；`@overwrite` 只带一个名字。
- 不写类型就必须写值：`var a, b: i32` 是零初始化（两个名字都是零值），`var a, b` 报
  `` `var a` needs a type or an initializer ``。
- 顶层多名字声明与局部同形（`global var g1, g2: i64 = 10, 20`），初始化式仍然必须是常量。
- `mut` **不是变量的修饰符**，而是类型里的一部分：`mut ref T`、`mut slice<T>`（见「引用与视图」一节）。
  `let r: mut ref T = …` 是自相矛盾的，报 `` `let r` cannot be declared with a `mut` type ``。

```extc
// 结果：2 3 2 3
use std::io
let LIMIT: i64 = 3
var counter: i64                 // 全局 var，零初始化
fn bump() { counter = counter + 1 }
fn main() -> i32 {
    let a: i32 = 1
    var b: i32 = 2
    b = b + 1
    let a: i32 = a + 1           // 同层遮蔽：新名字
    bump()
    bump()
    io::cout << a << " " << b << " " << counter << " " << LIMIT << "\n"
    return 0
}
```

<!-- fragment -->
```extc
var a, b: i32 = 1, 2        // 一条声明两个名字，共享 i32
let c, d = 1, 2.5           // 不写类型：c 是 i32，d 是 f64
var e: i64, f = 10, 20.5    // 各写各的：e 是 i64，f 是 f64
var g, h: i32               // 零初始化：两个都是 0
```

---

## 7. 引用与视图

### 7.1 三种引用

| 写法 | 含义 | 零值 | 取引用 | 写 |
|---|---|---|---|---|
| `ref T` | 只读借用（默认） | 无 | `ref x` | 不能 |
| `mut ref T` | 可写借用 | 无 | `ref x`（x 必须是可写的） | 能 |
| `?ref T` | 可能没有的借用 | `null` | `null` / `ref x` | 只有 `mut ?ref T` 能写 |

- 取引用的表达式是 `ref x`，**只能**作用于变量、字段或 `*p`；索引和调用结果不行
  （`ref a[1]` → `cannot take a reference to this expression`）。
- `ref x` 的权限由 `x` 决定：`x` 是 `var` 得到 `mut ref T`，`x` 是 `let` 得到 `ref T`。
  拿只读引用的地方要可写的会报 `initializer expects 'mut ref i32', found 'ref i32'`。
- 调用自由函数时引用实参要**写出来**：`addTo(ref c, 7)`；
  方法接收者**自动取地址**：`c.bump(5)`。
- 可空引用取值：`!`（先检查再 trap，见「错误与逃生舱」一节）或 `??` 兜底。

### 7.2 解引用与自动解引用

**值位置上没有隐式解引用**，要自己写 `*p`：

| 位置 | 行为 |
|---|---|
| `*p` 读 / `*p = v` 写 | 显式解引用；写要求 `mut ref` |
| `p.field` / `p.method()` | **自动穿透**（成员导航，不算读值） |
| `println(p)` / `print(p)` 的实参 | 自动解引用 |
| 其他（算术、传值参数、`io::cout << p`、`let v = p`） | 报错：`` `mut ref i32` is a reference, not a value -- dereference it first: `*p` `` |

### 7.3 视图 `slice<T>`

- 从数组切：`a[lo..hi]`、`a[lo..]`、`a[..hi]`、`a[..]`；视图还能再切。
- 视图是**指针 + 长度**，不拷贝数据：透过它写，原数组跟着变。
- `.len` 是普通字段；`v[i]` 索引越界 trap；`v.isEmpty()` / `v.get(i)` / `v.find(x)` 等在
  `slice` 上。
- **写权限来自类型**：`var mid = a[2..5]` 得到 `mut slice<i32>`，能写；
  `let v = a[1..3]` 得到 `slice<i32>`，写它报
  `` cannot write through `v`: it is a read-only view `slice<i32>` ``。
- 编译器能证明的界不生成检查（`a[0..3]`）；界是变量时生成带检查的 helper。
- **容器在视图参数上自动借出**：只读参数上 `total(va)`（`varArray<T>`）等价于
  `total(va.asSlice())`、`total(a)`（`[N]T`）等价于 `total(a[..])`；可写参数
  （`mut slice<T>`）上 `fill(va)` 等价于 `fill(va.asMutSlice())`、`fill(a)` 等价于
  `fill(a[..])`，后者要求容器本身是可写的地方（`var` 绑定），借用检查照常把关。
  方法实参同理（`s.append(bytes)` 的 `bytes`）。编译器在调用位置造的就是这些节点，
  借用与生命周期检查照常生效。元素类型对不上时（`varArray<i32>` 传给 `slice<i64>`）不借，
  诊断指出该写什么。
- **借出不是内建名单**：任何类型只要方法 `asSlice() -> slice<T>`（只读）或
  `asMutSlice() -> mut slice<T>`（可写）的**返回类型**对得上，就能出现在这类参数上 ——
  判据是签名，不是容器是哪个内建类型。`varArray` 与定长数组只是内建的两个现成做法。
  只有 `asSlice` 的类型不会被借成可写视图（可写参数仍要求 `asMutSlice`）。

```extc
// 结果：3 10 80 1059 / hello world 0
use std::io
fn sum(v: slice<i32>) -> i32 {
    var s: i32 = 0
    var i: i64 = 0
    while i < v.len { s = s + v[i]  i = i + 1 }
    return s
}
fn main() -> i32 {
    var a: [8]i32 = [10, 20, 30, 40, 50, 60, 70, 80]
    var mid = a[2..5]
    let tailOnly = a[6..]
    var all = a[..]
    mid[0] = 999
    io::cout << mid.len << " " << all[0] << " " << tailOnly[1] << " " << sum(a[1..4]) << "\n"
    let s = "hello world"
    io::cout << s[0..5] << " " << s[6..] << " " << s[2..2].len << "\n"
    return 0
}
```

### 7.4 整数组表达式 `out = a + b`

赋值右边是**逐元素的运算**、左边是可写视图时，编译器把它降成一条写循环：

- 写法：`out = a + b * k - 1`（`+ - * /`、一元 `+ -`、标量与字面量混合）；复合形式
  `out += a`、`out -= b`、`out *= k`、`out /= k` 等价于 `out = out <op> …`（目标每格读一次、
  写一次，别名安全）。
- 语义：`out[i] = a[i] + b[i] * k - 1`，`i` 从 0 到 `out.len`。**不隐式分配**：目标视图由
  调用者提供；每个参与视图必须与目标等长，否则 trap（不静默截断）。
- 别名是允许的：`out = out + a` 逐元素读写同一格，结果是新值。
- 裸视图名不是表达式：`buf = t` 仍是**视图值赋值**（换指针与长度），不是逐元素拷贝。
- 降糖产物就是手写循环的形状，所以边界检查消除、向量化与融合照常作用在它上面；实测与
  手写 C 的 AXPY 循环同一量级。`par` 体里暂不支持这种写法，仍写显式 `out[i] = …`。
- **rank-2 形状**（目标是 `ndview<T>`）：不展开循环，直接落成 `stl::nd` 的逐元素内核调用
  —— `dst = a + b` → `nd::addInto`、`a - b` → `subInto`、`a * b` → `mulInto`、
  `a * k` → `scaleInto`、`a + b * k` → `axpyInto`，复合形式同理（`dst += a` →
  `addInto(dst, dst, a)`）。形状在运行期才知道，所以内核返回 `bool`，编译器在外面补一条
  `if !… { trap("…same shape as the destination") }`。写出来的 C 就是一次内核调用加一次
  失败分支（形状不匹配才走 trap），热路径没有额外指令；`v[i, j]` 读写照旧是 `nd::at2` /
  `nd::set2`。

```extc
// 结果：out = 0 3 6 9 sum = 18
use std::io

fn axpy(a: slice<i64>, b: slice<i64>, out2: mut slice<i64>) {
    out2 = a + b * 2
}

fn main() -> i32 {
    var a: mut slice<i64> = new i64[i64(4)]
    var b: mut slice<i64> = new i64[i64(4)]
    var out: mut slice<i64> = new i64[i64(4)]
    for i in 0..4 {
        a[i] = i64(i)
        b[i] = i64(i)
    }
    axpy(a, b, out)
    var sum: i64 = 0
    for i in 0..4 {
        sum = sum + out[i]
    }
    io::cout << "out ="
    for i in 0..4 { io::cout << " " << out[i] }
    io::cout << " sum = " << sum << "\n"
    return 0
}
```

---

## 8. 内存模型

### 8.1 arena 按词法块

每次 `new` 从**当前块**的 arena 拿一块**清零**的内存。一个函数有若干块级 arena
（生成物里是 `extc_arena __extc_a[k]`），块退出时 `extc_arena_release` 回收，
函数退出时 `extc_arena_destroy`。所以循环里 `new` 不会无限涨内存：

```extc
// 结果：输出 1 42 4 0 true
// 生成物（build/lang-m-new.c）：块退出处 `extc_arena_release(&__extc_a[k])`
use std::io
struct node { value: i64  next: ?ref node }
fn main() -> i32 {
    var a: mut ref node = new node          // ① new T        -> mut ref T
    a.value = 1
    var arr: mut ref [3]i32 = new [3]i32    // ② new [N]T     -> mut ref [N]T
    (*arr)[1] = 42
    var buf: mut slice<i32> = new i32[4]    // ③ new T[n]     -> mut slice<T>（n 运行期才知道）
    buf[0] = 7
    // 新分配的一律是零：a.next == null、buf[3] == 0
    io::cout << a.value << " " << (*arr)[1] << " " << buf.len << " " << buf[3] << " " << (a.next == null) << "\n"
    return 0
}
```

- `alloc<T>(n)` 是内建原语（`EX_GENCALL`），返回 `mut ref T`；一次分配一个 `T`。
  要"n 个元素的 buffer"用 `new T[n]`。
- 分配失败 trap：`out of arena memory (this allocation wanted N bytes)`。
- **逃逸提升**：函数体里有分配、返回类型含引用/视图时，这个函数多收一个隐藏参数
  `extc_arena *__extc_home`，里面的 `new` 分配到**调用者选的那只** arena。
  生成物形如 `fn build(n, extc_arena *__extc_home)`；调用者把自己家的那只传下去。

```extc
// 结果：输出 6
// 生成物：static node * build(int64_t n, extc_arena *__extc_home)
use std::io
struct node { value: i64  next: ?ref node }
fn build(n: i64) -> mut ref node {
    var head: mut ref node = new node
    head.value = 0
    var cur: mut ref node = head
    var i: i64 = 1
    while i <= n {
        var cell: mut ref node = new node
        cell.value = i
        cur.next = cell
        cur = cell
        i = i + 1
    }
    return head
}
fn main() -> i32 {
    var l: mut ref node = build(3)
    var t: i64 = 0
    var c: ?ref node = l
    while c != null { t = t + c.value  c = c.next }
    io::cout << t << "\n"
    return 0
}
```

`@overwrite var n = new T`：让每轮循环**复用同一块存储**（活到本帧，不是块），
初始式必须恰好是 `new`，且绑定必须是 `var`。

### 8.2 `pool<T>` 与 `varArray<T>` 的区别

| | `varArray<T>`（prelude 里） | `pool<T>`（`stl::pool` 里） |
|---|---|---|
| 存储 | 一块连续 buffer，满了按 2 倍扩容 | 值放 dense 数组，另有 slot→dense 的映射 |
| 取 / 写 | `v.get(i) -> ?T`、`v.set(i, v)`（按值拷） | `p.get(h) -> ?T`、`p.set(h, v) -> bool`（按值拷） |
| 可寻址性 | `v[i]` **不行**（`cannot index a value of type 'varArray<i32>'`）；要借出视图：`v.asSlice()` / `v.asMutSlice()` → `slice<T>`，视图元素可索引、可写 | 没有按索引访问；`p.toSlice()` 借出活元素前缀的视图 |
| 装引用 / 视图 | 可以：`varArray<slice<u8>>`、`varArray<ref i32>` 都编过（存的时候按寿命检查） | 可以：`pool<slice<u8>>` 编过 |
| 回收 | **没有逐对象回收**：`clear()` 只把 `len` 归零、不还内存，也不能缩容 | **逐对象回收**：`remove(h)` 把最后一个值搬进空洞、该槽位"代 +1"；之后旧句柄 `get` 得 `none`（不会读到新值） |
| 句柄 | 下标（会因扩容/搬移失效） | `{ slot, gen, ep }`，陈旧句柄可判定 |

扩容的代价要记住：`push` 换新 buffer 后，**之前借出的视图仍指旧 buffer**。
下面的实测里写进旧视图的值不会出现在容器里（不崩，但静默分叉）：

```extc
// 结果：1 99 —— v.get(0) 还是旧 buffer 的 1，旧视图 s 里读到 99
use std::io
fn main() -> i32 {
    var v: varArray<i32> = varArray<i32>::withCap(2)
    v.push(1)
    var s: mut slice<i32> = v.asMutSlice()
    v.push(2)
    v.push(3)
    v.push(4)
    s[0] = 99
    io::cout << (v.get(0) ?? i64(-1)) << " " << s[0] << "\n"
    return 0
}
```

```extc
// 结果：stale=none slot=0 gen=1 / len=2
use std::io
use stl::pool
fn main() -> i32 {
    var p: pool<i64> = pool<i64>::withCap(i64(4))
    let h1 = p.insert(i64(111))
    let h2 = p.insert(i64(222))
    p.remove(h1)                     // 槽位换代
    let h3 = p.insert(i64(333))      // 复用同一格
    match p.get(h1) {
        some(v) => { io::cout << "stale=" << v << "\n" }
        none    => { io::cout << "stale=none slot=" << i64(h3.slot) << " gen=" << i64(h3.gen) << "\n" }
    }
    io::cout << "len=" << p.len() << "\n"
    return 0
}
```

---

## 9. 错误与逃生舱

### 9.1 运行时检查

编译器在下面这些地方插入检查；不满足就 `file:line: trap: 消息` 写到 stderr 并**退出码 1**：

| 检查 | 消息 |
|---|---|
| 下标越界 | `index 3 out of range (length 3)` |
| 除零 / 除溢出 | `division by zero` |
| 移位计数 | `shift count out of range` |
| 有损转换 | `value does not fit in the target type` |
| `!` 断言可空引用非空 | `the nullable reference was asserted non-null but is null` |
| `!` 断言 `option`/`result` 有载荷 | `the variant was asserted to hold a value but is none or failure` |
| 视图/复制目标已无存储 | `the view has no storage` |
| arena 分配失败 | `out of arena memory (this allocation wanted N bytes)` |
| 递归太深 | `recursion too deep (unbounded recursion?)` |

`main` 里的 `?` 失败也走这条路（`a step in 'main' failed: …`）。

### 9.2 `!`：先检查，再 trap

`p!`（`?ref T`）、`o!` / `r!`（`option` / `result`）会**先插入断言**，再取载荷：

```extc
// 结果：rc=1，stderr: lang-e-sign.extc:2: trap: the nullable reference was asserted non-null but is null
struct box { v: i64 }
fn pick(p: ?ref box) -> i64 { return p!.v }
fn main() -> i32 {
    let n: ?ref box = null
    return i32(pick(n))
}
```

`!` 可以作赋值目标（`m!.v = 9`），前提是 `m` 是可写的可空引用（`var m: mut ?ref box = ref b`）。

### 9.3 `@unchecked`：唯一的 UB 通道

`@unchecked` 标在一个函数/方法上，那个函数体里**不再生成任何运行时检查**（下标、除零、
移位、窄化、可空、范围、`copyInto` 计数）。检查消失后行为就是 C 的行为：

```extc
// 结果：退出码 255、没有任何 trap 消息（已缓冲的输出也没刷出来）
use std::io
@unchecked
fn divide(a: i64, b: i64) -> i64 { return a / b }
fn main() -> i32 {
    io::cout << divide(i64(6), i64(3)) << "\n"
    io::cout << divide(i64(6), i64(0)) << "\n"
    return 0
}
// 同一份代码不写 `@unchecked`：先输出 2，然后 stderr `…: trap: division by zero`，退出码 1
```

`@unchecked` 关不掉编译期检查（类型、越界常量、`match` 穷尽），只关运行期那些。
它不能标在 `extern!` 声明（没有函数体）、字段、`struct`、枚举或全局上。

### 9.4 `trap(msg)`

`trap("…")` 是语句，打印 `<file>:<line>: trap: <消息>` 并以 1 退出，之后的代码不执行：

```extc
// 结果：stdout 只有 before；stderr `<file>:3: trap: n must not be negative`；退出码 1
use std::io
fn check(n: i64) { if n < 0 { trap("n must not be negative") } }
fn main() -> i32 {
    io::cout << "before\n"
    check(i64(-1))
    io::cout << "after\n"
    return 0
}
```

### 9.5 `option` / `result` 与 `?`

`option<T> = | none | some(T)`、`result<T, E> = | failure(E) | success(T)`。取值两条路：
`match`（穷尽）或 `?`（失败往上抛）。只关心一个变体时还可以用 `if let` / `while let`
（第 11.7 节）—— 它们是 `match` + `_` 兜底臂的糖。

`?` 出现在**语句的值位置**里：整条语句（声明 / 赋值 / 返回 / 裸调用 / `if` 条件）会被重写，
里面嵌套的每一个 `e?` 由检查器提前成一条 `var __extc_tqN = e?` 插在该语句之前 ——
求值一次、空就立刻返回，所以求值次数与时机都不变：

<!-- fragment -->
```extc
let v: i64 = parse(n)? + 1                  // 操作数里
add(parse(n)?, parse(m)?)                   // 调用实参里
if parse(n)? > i64(0) { … }                 // if 条件里
return success(parse(n)? * i64(10))         // 返回值的表达式里
```

**可能被跳过**的位置仍然拒绝（提前会改变求值次数/时机），报
`` `?` cannot appear here: this position may be skipped (the right side of `&&` / `||` / `??`, a loop condition, a lambda body). Bind it first: `let v = e?` ``：
`&&` / `||` 的右侧、`??` 的右侧、`while` 条件、lambda 体。
外层函数必须返回同族类型，且**错误类型必须一样**（`?` 原样转发、不转换）：
`` `?` on `option<i64>` needs the enclosing function to return `option<...>` ``、
`` `?` here would change the error type from `a` to `b` ``。

### 9.6 `??`：可能没有就兜底

- 左边可以是 `option` / `result` / `?ref`；右边类型必须对得上
  （`left of '??' is 'i64', which always has a value` 是对普通值用 `??` 的报错）。
- 右结合；**只算一边**（生成 C 的三元）；主体是调用时编译器先装进临时变量，只求值一次。
- 两个位置拒绝：`while` 条件（`?? here cannot be evaluated ahead of time`）和全局初始化式
  （`the global 'g' must be initialized with a constant`）。

```extc
// 结果：10 -1 / 11 / bad
use std::io
type err = | bad | worse
fn parse(n: i64) -> result<i64, err> {
    if n < 0 { return failure(err.bad) }
    return success(n * 2)
}
fn viaLet(n: i64) -> result<i64, err> {
    let v = parse(n)?
    return success(v + 1)
}
fn main() -> i32 {
    var good = parse(i64(5))
    var bad = parse(i64(-5))
    io::cout << (good ?? i64(-1)) << " " << (bad ?? i64(-1)) << "\n"
    io::cout << (viaLet(i64(5)) ?? i64(-1)) << "\n"
    match viaLet(i64(-5)) {
        success(v) => { io::cout << "ok\n" }
        failure(e) => { match e { bad => { io::cout << "bad\n" } worse => { io::cout << "worse\n" } } }
    }
    return 0
}
```

---

## 10. 注解

注解写在被修饰的声明**前面**，未知注解一律报错（不静默忽略）。

| 注解 | 能标在哪 | 现在的要求 | 写错的报错 |
|---|---|---|---|
| `@private` | 顶层声明；`struct` 的字段或方法 | 跨模块不可见 | `@private does not apply to this declaration` / `appears twice` |
| `@inline` | 函数 / 方法 | 调用必须内联；**递归（含互递归）不行** | `` `@inline` cannot be honoured: `f` reaches itself `` / `needs a body, and an extern declaration has none` |
| `@unchecked` | 函数 / 方法 | 该函数体不生成运行时检查 | `@unchecked applies to a function, not to a struct` / `not to a field` / `needs a body` / `appears twice` |
| `@noCopy` | `struct` | 只能按 `ref` / `mut ref` 传 | `` `counter` is `@noCopy`: it may not be copied by value `` |
| `@poolObject` | `struct` | 该类型拥有一个池，存储住在池的板块上 | `@poolObject goes on a struct` |
| `@sharesStorage` | `struct` | 按值拷贝时两份共用存储（容器那一族）；编译器给警告 | `@sharesStorage goes on a struct` |
| `@frozen` | `struct` | 布局就是 C 的布局，是按值过 `extern!` 边界的门票 | `@frozen goes on a struct` / `appears twice` |
| `@export` | **自由函数** | 生成 C 可见符号（非 `static`），C 可以直接调 | `@export goes on a function`（方法 / 泛型 / 协程 / `extern!` / 结构体 / 枚举 / 全局都拒） |
| `@builtin` | 函数（**没有函数体**） | 声明由编译器实现（stdlib 用：`parallel::run`、域、`extc_viewOf`） | `appears twice` |
| `@overwrite` | 局部 `var` 声明 | 复用同一块存储；初始式必须恰好是 `new` | `` `@overwrite` needs `var` `` / `` `@overwrite` only applies to `new` `` |
| `@recursive` / `@main` | — | 现在会被拒绝，编译器直说没有实现 | `` `@recursive` is not implemented yet ``（见 [`../TODO.md`](../TODO.md)） |

`impl` 块和 `trait` 声明身上**没有任何注解适用**（`no annotation applies to an impl block`）。

```extc
// 结果：编译通过、退出码 9
// 生成物里 @inline 的函数带 EXTC_INLINE；@export 的函数 `int64_t addTwice(int64_t a);` 不带 static
@inline
fn add(a: i64, b: i64) -> i64 { return a + b }
@export
fn addTwice(a: i64) -> i64 { return a + a }
fn main() -> i32 { return i32(add(i64(1), i64(2)) + addTwice(i64(3))) }
```

---

## 11. 语句与表达式

### 11.1 语句

`let`/`var`、`global let/var`、赋值（`=` `+=` `-=` `*=` `/=` `%=`）、并行赋值（`t1, t2 = e1, e2`）、
`if`/`else if`/`else`、`if let`/`while let`、`while`、`for`、`match`（含最后的 `_` 兜底臂）、`return`、`break`、`continue`、
块 `{ … }`、`trap(msg)`、`yield expr`、表达式语句。
`if`/`while` 的条件必须是 `bool`（`expected 'bool', found 'i32'`）。
`if`/`while`/`match` 的条件里不能直接写结构体字面量，要加括号
（`struct literal in a condition needs parentheses`）。
函数体内**不能**嵌套 `struct` / `fn` 声明。

### 11.2 `match` 是语句

- `match e { 变体 => 语句 … }`；分支体是**一条语句**，通常写成块。
- 默认必须**穷尽**所有变体：漏一个报
  `` `match` does not handle `blue` -- every variant must be listed ``。
- 想兜底就**显式**写最后一条 `_ => 语句`：它匹配上面没列出的所有变体，必须最后、不绑载荷
  （`` an arm after `_` can never match -- `_` must be the last arm `` /
  `` `_` matches any variant, so it binds no payload ``）。
- 载荷位写 `_` 是**丢弃该位**（`circle(_) => …`、`pair(a, _) => …`），和整条 `_` 臂不是一回事。
- 带载荷的变体用 `circle(r) => …` 按位置绑定；被 match 的表达式只求值一次。
- 它是语句不是表达式：想往外传值就在分支里 `return` / 赋值。

### 11.3 `for` 的形态

parser 把 `for` 脱糖成 `var` + `while`，共有三种脱糖形状；
按用法常被数成**四种**：区间、定长数组/视图、自定义迭代器、C 风格 —— 前两种共用"容器"这一形状。

| 形状 | 写法 | 说明 |
|---|---|---|
| 区间 | `for i in lo..hi { … }` | 半开区间；`i` 的类型取两端的**较宽者**（`hi` 能无损容纳 `lo` 时，`for i in 0..n` 且 `n: i64` ⇒ `i: i64`；反之 `for i in 0..k` 且 `k: i32` ⇒ `i: i32`）；**上界只求值一次** |
| 容器 | `for d in subj { … }` | `subj` 只能是名字、字段或 `*p`；元素是**拷贝**，改 `d` 不写回 |
| C 风格 | `for (init; cond; step) { … }` | 三段都是普通语句/表达式；`continue` 跳到 step |
| 并行 | `par i in lo..hi { … }` | 数据并行（多线程跑同一个循环体），形状与区间 for 相同；见 11.8 |

容器形状覆盖：定长数组、`slice<T>`（含字符串字面量）、协程帧，
以及任何声明了 `iter` / `next` / `value` 三个方法的用户类型
（`fn iter(self: ref Self) -> It`、`fn next(self: mut ref It) -> bool`、`fn value(self: ref It) -> T`）。
`varArray` **没有**这三个方法，直接迭它会报
`` no method `iter` on `varArray<i64>` ``——用 `v.asSlice()` 借出视图再迭。

```extc
// 结果：10 60 12 60
use std::io
fn main() -> i32 {
    var sum: i64 = 0
    for i in i64(0)..i64(5) { sum += i }
    var dirs: [3]i64 = [10, 20, 30]
    var tot: i64 = 0
    for d in dirs { tot += d }
    var k: i64 = 0
    for (var j: i64 = 0; j < 4; j += 1) { k += j * 2 }
    var v: slice<i64> = dirs[..]
    var s2: i64 = 0
    for d in v { s2 += d }
    io::cout << sum << " " << tot << " " << k << " " << s2 << "\n"
    return 0
}
```

### 11.4 运算符与重载

可重载的集合正好是这 13 个名字：

| 类别 | 运算符 | 返回类型要求 |
|---|---|---|
| 比较 | `==` `!=` `<` `<=` `>` `>=` | 必须是 `bool` |
| 算术 | `+` `-` `*` `/` `%` | 必须是**本类型**（按值） |
| 流 | `<<` `>>` | 本类型（按值）或 `mut ref` 本类型（有状态的流要链式） |

- 方法名就是运算符：`fn +(self: ref vec2, o: vec2) -> vec2`。
- `self` 必须是 `ref` / `mut ref` 本类型；右操作数可以是任意类型 —— 同一个运算符因此能
  按右操作数类型定义多份（`out << i64` 和 `out << f64` 共存）。
- `!=` 由 `==` 取反**派生**（不必自己写）；`>` 不派生，要自己写。
- `%` 对浮点永远不可用（`cannot apply '%' to 'f64'`），重载也不改变这一点。
- 运算符方法**可以分配**：调用点和方法调用走同一套 zone 记账（`setOpCallArgs` 里的
  `setCallZoneArg`），所以 `fn +(self: ref string, other: slice<u8>) -> string` 这种"分配并
  返回新值"的运算符是合法形状（见 `stl::string` 的 `+`：`s + "…"`、`a + b`）。
- 字符串的 `+` 与 `appendI64` / `appendF64` 覆盖两种拼法：表达式里拼用 `+`（返回新值），
  循环里拼用 `append*`（就地长）。
- 移位（`<<` / `>>`）的计数在运行期按左操作数**位宽**检查（越界 trap）；计数是**字面量**时
  检查器提前到编译期：`1 << 32` 报 `shift count 32 does not fit in `i32` (width 32)`，
  写 `i64(1) << 32` 才是 64 位结果。
- `&&` / `||` / `=` / `+=` 等不可重载。
- 运算符的实参按源码**从左到右**求值：一次重载运算符就是一次方法调用，生成器把左半边先物化
  成一条语句，因此 `io::cout << a() << b()` 里 `a()` 一定先跑。**边界**：原生运算符
  （`i32 + i32` 这类）与普通函数/方法调用的实参顺序仍按 C 的未指定顺序 —— 会把「释放」与
  「使用」写进同一条原生表达式或同一次调用的，照样要拆成两句。

```extc
// 结果：false 3 7  （a.x=2，b.x=1 ⇒ `a < b` 为 false）
use std::io
struct vec2 { x: i32  y: i32
    fn <(self: ref vec2, o: ref vec2) -> bool { return self.x < o.x }
    fn +(self: ref vec2, o: vec2) -> vec2 { return { x: self.x + o.x, y: self.y + o.y } }
}
fn main() -> i32 {
    var a: vec2 = { x: 2, y: 3 }
    var b: vec2 = { x: 1, y: 4 }
    var s: vec2 = a + b
    io::cout << (a < b) << " " << s.x << " " << s.y << "\n"
    return 0
}
```

### 11.5 输出与 `<<`

`io::cout <<` 现在只对 `i32` / `i64` / `f64` / `bool` / `slice<u8>`（以及 `mut slice<u8>`）
有 `<<`。枚举、结构体、其他类型要用 `<<` 输出就得自己写
（`impl io::ostream { fn <<(self: ref io::ostream, other: T) -> io::ostream { … } }`），
否则报 `` `io::ostream` does not define `<<` for `shape` ``。格式化串见第 2.2 节。

### 11.6 并行赋值

`t1, t2, … = e1, e2, …` 是**并行**赋值：右边先**全部求值**（从左到右），然后左边**全部写**
（从左到右）。所以 `a, b = b, a` 交换两个变量，而不是把两个都写成一个值。

- 每个目标都得是**地方**：变量、字段、下标/切片元素、`*p`，以及 place 的载荷 `o!`
  —— 和单目标赋值的左边同一套规则。
- 目标数必须等于值数：`` `2` target(s) but `1` value(s) ``。
- 多个目标只接受 `=`：`a, b += 1, 2` 报 `` expected `=` after the assignment targets ``。
- 它是一条语句，所以块里、`match` 分支体（不带块也行）、`for (init; cond; step)` 的 init 与 step
  里都能写；多名字声明同样能出现在这些位置。

```extc
// 结果：2 1 / 2 1 / 20 10
use std::io

type color = | red | green | blue
struct pt { x: i32  y: i32 }

fn main() -> i32 {
    var a, b: i32 = 1, 2
    a, b = b, a                            // 交换
    io::cout << a << " " << b << "\n"

    var p: pt = pt { x: 1, y: 2 }
    p.x, p.y = p.y, p.x                    // 字段交换
    io::cout << p.x << " " << p.y << "\n"

    var arr: [2]i32 = [10, 20]
    arr[0], arr[1] = arr[1], arr[0]        // 下标交换
    io::cout << arr[0] << " " << arr[1] << "\n"

    var c: color = color.red
    match c {
        red => a, b = b, a                 // 分支体不带块也能写
        green => { a = 0 }
        blue => { a = 0 }
    }

    for (var i, j: i32 = 0, 0; i < 3; i, j = i + 1, j + 10) { }
    return 0
}
```

### 11.7 `if let` / `while let`

只关心一个变体时，用 `if let` 代替写全的 `match`：

- `if let PAT = e { … }`：`e` 求值一次，形状是 `PAT` 就绑载荷跑块；否则跑 `else`（没有 `else` 就什么也不做）。
- `if let PAT = e { … } else if let PAT2 = e2 { … } else { … }`：链式写，按顺序试。
- `while let PAT = e { … }`：每轮重新求值 `e`；匹配就跑块，不匹配就结束循环；块里的
  `continue` 回到条件、`break` 跳出循环。
- `PAT` 只能是**一个**变体，按位置绑定载荷（`some(v)`、`pair(a, b)`）；载荷位写 `_` 表示丢弃
  （`some(_)`、`pair(a, _)`）。`_` 本身不能当 `if let` / `while let` 的模式。

两者都是 parser 期脱糖，没有新语义：

- `if let PAT = e { A } else { B }` 就是 `match e { PAT => { A } _ => { B } }`；
- `while let PAT = e { A }` 就是 `while true { match e { PAT => { A } _ => { break } } }`。

所以脱糖出来的 `_` 是**显式**的兜底臂 —— 手写 `match` 不写 `_` 仍然要求穷尽（见 11.2）。
`else` 里再嵌套 `if let`、块里再写 `if let` 都可以。

```extc
// 结果：some 42 / empty / 0 1 / done
use std::io

fn next(v: i32) -> ?i32 {
    if v < 2 { return some(v) }
    return none
}

fn main() -> i32 {
    let o: ?i32 = some(42)
    if let some(v) = o {
        io::cout << "some " << v << "\n"
    } else {
        io::cout << "none\n"
    }

    let n: ?i32 = none
    if let some(_) = n {
        io::cout << "has\n"
    } else {
        io::cout << "empty\n"
    }

    var k: i32 = 0
    while let some(v) = next(k) {
        io::cout << v << " "
        k = v + 1
    }
    io::cout << "done\n"
    return 0
}
```

### 11.8 `par`：数据并行

`par i in lo..hi { … }` 把一个循环变体交给多个线程跑。它是**唯一的并行动词**：一个循环嵌套就是
一个内核（kernel）形状，检查能在一个词法块里做完 —— 这也是将来换设备后端时不用改用户代码的原因。

规则只有五条（细节和判据见 `docs/PARALLEL.md`）：

- 输入**只读共享**：体内读的视图按引用捕获，所有 worker 看同一份数据；内建标量（整数、`f32`/
  `f64`、`bool`）按值捕获，进入 worker 前求值一次（worker 拿自己的副本，写副本不影响调用者）；
- 输出**恰好一个**：体内只许写 `out[i]`，`out` 是一个 `mut slice<T>`，下标**恰好是循环变量本身**
  （一个 worker 只写自己那一格，所以不用锁）；
- **二维折叠**：内层再写一个 `par` 不会另开一批线程 —— 它折叠进同一个内核，写
  `out[r * cols + c]`（下标只由并行循环变量、整数常量和按值捕获的标量构成）；
- 体内**不分配、不调用函数**（v1 比设计更严：连纯函数调用也不许），不许把控制流带出并行体
  （`return` / `break` / `continue` / `yield` / `ext`）；
- 体内声明的 `var` 是 worker 私有的，随便写。

`lo..hi` 的两端必须能无损进 `i64`（`par i in 0..n` 可以）；空区间一次都不跑、不 trap。
写别的视图、下标不合形状、把输出视图当输入读、捕获结构体/引用这类没有捕获故事的值，都会在编译期被拒
（`` cannot call a function inside a par loop `` / `` this par loop may write only its own output slot `` /
`` only read-only views and scalar values can be captured by a par loop `` 等）。
v1 只折叠两层、体内禁一切调用、泛型/协程体里不能写 `par`、下标必须是 `i64` 能无损容纳的整数。

线程数用环境变量 `EXTC_PAR_THREADS` 覆盖（默认 `_SC_NPROCESSORS_ONLN`，上限 256）；线程数只影响
速度，不影响结果。生成的 C 用 pthread，链接时要加 `-pthread`。

```extc
// 结果：sum=499500
use std::io

fn scale(src: slice<f64>, dst: mut slice<f64>) {
    par i in 0..dst.len { dst[i] = src[i] * 1.0 }
}

fn main() -> i32 {
    let n: i64 = i64(1000)
    var a: mut slice<f64> = new f64[n]
    var b: mut slice<f64> = new f64[n]
    for i in 0..n { a[i] = f64(i) }
    scale(a, b)
    var s: f64 = 0.0
    for v in b { s = s + v }
    io::cout << f"sum={i64(s)}\n"   // "sum=499500"
    return 0
}
```

二维折叠与标量捕获：

```extc
// 结果：s=18090
use std::io

fn parFill(rows: i64, cols: i64, dst: mut slice<f64>) {
    par r in 0..rows {
        par c in 0..cols {
            dst[r * cols + c] = f64(r * i64(100) + c)
        }
    }
}

fn main() -> i32 {
    let rows: i64 = 9
    let cols: i64 = 5
    var a: mut slice<f64> = new f64[rows * cols]
    parFill(rows, cols, a)
    var s: f64 = 0.0
    for v in a { s = s + v }
    io::cout << f"s={i64(s)}\n"   // "s=18090"
    return 0
}
```

归约是第二形状，不是我行我素的第三种语法：先 `par` 写进数组，再用 `stl::seq` 的
`sum` / `max` / `min` / `count` 合并（`seq::sum(xs) ?? f64(0.0)`）。这两个形状合起来覆盖
"切块算 + 汇总"这条主线。

---

## 12. 结果：现在写不出来的写法

| 写法 | 报错 |
|---|---|
| `f().field = v` | `cannot assign to this expression: it is not a place` |
| `f()!.field = v`（`!` 操作数是调用） | 同上；备注列出**能**作赋值目标的形状：变量、place 的字段/下标/切片、`*` 解引用、place 的载荷 `o!` |
| `let v = a[1..3]` 后 `v[0] = 9` | `` cannot write through `v`: it is a read-only view `slice<i32>` `` |
| `ref a[1]` / `ref s[1]` / `ref f()` | `cannot take a reference to this expression`（只有变量、字段、`*p` 能取引用） |
| `var y = p + 1`（`p: mut ref i32`） | `` `mut ref i32` is a **reference**, not a value -- dereference it first: `*p` `` |
| `io::cout << p`（`p` 是引用） | 同上 |
| `f() ?? -1` 放进 `while` 条件 | `` `??` here cannot be evaluated ahead of time `` |
| `let a: i8 = -1` | `initializer expects 'i8', found 'i32'`（取负号后不再是裸字面量） |
| `a == b`（带载荷枚举） | `` `shape` is an enum with a payload, so it has no `==` ``（要比就 `match` 或自己写 `fn ==`） |
| `let f: fn(i64) -> i64`（无初始化式） | `cannot zero-initialize 'f': it contains a fn`（`fn` 没有零值） |
| `i8(300)` | 编译通过；运行时 trap `value does not fit in the target type` |
| `var v: varArray<T>` 上 `v[i]` | `cannot index a value of type 'varArray<i32>'`（用 `get`/`set`/`asSlice`） |
| 嵌套超过 64 层块 | `nesting too deep in a block (the compiler's limit is 64 levels)` |

能写但值得记住的两条：

- `(*f()).field = v` 能写，而且实测 `f` **只被求值一次**（`var calls` 计数为 1）。
- `m!.field = v` 能写，前提是 `m` 是可写的可空引用（`var m: mut ?ref box = ref b`）。

---

## 13. 已知局限

多名字声明与并行赋值是语法糖：除纯字面量外，右边的每个值先落进一个隐藏临时量，再逐个目标写回。
由此能撞上的局限：

- `@noCopy` 类型不能拿来交换：`a, b = b, a` 里每个值都要过一遍临时量，
  报 `` `counter` is `@noCopy`: it may not be copied by value ``。
- 右值是「返回含引用聚合的函数调用」、而目标在外层或循环里时，会误报 borrow-from-local：
  先 `var t = make()` 再 `b = t` 不行，直接写 `b = make()` 可以。
- 右值是裸结构体字面量 `{}` 时临时量定不了型（`` cannot infer the type of a bare `{}` here ``），
  直接单目标赋值则可以。`null` 没有这个问题：它是纯字面量，直接走带目标类型上下文的赋值。
- `@sharesStorage` 类型每个临时量各刷一条 `copies by value but **shares storage**` 警告，
  交换一次会看到两条。

彻底解决要新增 StmtKind（让并行赋值成为一等语句），属后续工作。

`if let` / `while let` 是 `match` + 显式 `_` 兜底臂的糖，由此能撞上的局限：

- 只能测**一个**变体：没有 guard（`if let some(v) = o && v > 0`）、没有 `let … else`、
  也没有或模式（`a | b`）；两个变体都要处理就还是写 `match`。
- `_` 整臂只能最后一条、不能绑载荷；`if let` / `while let` 的模式不能是 `_` 本身
  （要忽略载荷就写 `some(_)`）。

`f"…"` 的格式说明符只覆盖输出流上的三种值（整数、浮点、`slice<u8>`）：

- 宽度按**字节**算（中文一个字算 3 字节）。
- 定点格式化在 `|v| >= 1e15` 时**运行时 trap**（`io::pFmtF` 的边界）；要打这么大的数就先用 `<<`，
  或自己缩小数量级。
- 没有 `+` / `#` / 千分位之类的符号选项，也没有 `e` / `x` 等类型码：`d` / `f` / `s` 只做
  类型一致性校验，不换写法。
