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
- **只在 `<<` 的右操作数位置合法**（`io::cout << f"…"`）；别处报
  `an f-string is only valid as the right-hand side of '<<'`。
- 格式说明符（`{x:>8}` / `{:04}` / `{:.2f}`）**现在不支持**，写 `<` 之外的说明符报
  `format specifiers are not implemented yet`（见 [`../TODO.md`](../TODO.md)）。
- 空槽 `{}`、未闭合 `{`、多余的 `}`、整体为空的 `f""` 都是编译错误。

```extc
// 结果：tab:[	] nl:[\n] quote:["] backslash:[\] hex:[A]
//       10 65 92
//       n=42 {brace} 世界
use std::io
fn main() -> i32 {
    io::cout << "tab:[\t] nl:[\\n] quote:[\"] backslash:[\\] hex:[\x41]\n"
    io::cout << i64('\n') << " " << i64('\x41') << " " << i64('\\') << "\n"
    let n = 42
    io::cout << f"n={n} {{brace}} 世界\n"
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

- 类型参数名**必须大写开头**（`type parameter 't' must start with an uppercase letter`），
  类型名必须小写开头，两套名字不会撞。
- **单态化**：每个实例生成一份独立的 C 函数/结构体。`f<i32>` 与 `f<i64>` 是两个 C 符号。
- `T` 只出现在返回类型时推断不出来，必须写显式实参（`cannot infer type parameter 'T' of …`）。
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

- 顶层初始化式必须是**常量**：字面量、字面量的算术、字符串、枚举常量、字段都是常量的
  结构体字面量。调用、别的全局名不行（`the global 'A' must be initialized with a constant`）。
- 含引用的类型不能零初始化成全局（`it contains a reference`）。
- `let` 是**命名**不是存储：同一层可以再 `let` 同名（遮蔽），甚至可以换类型；
  被遮蔽的旧名字连同它的存储留在原地，之前取到的引用仍然指向它。
- `var` 是存储：同一层重复声明同名报 `` `v` is already declared in this scope ``。
- 参数可以被同层的 `let` 遮蔽。
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
`match`（穷尽）或 `?`（失败往上抛）。

`?` **只能**出现在这四个位置（整条语句可以重写的地方）：

| 位置 | 写法 |
|---|---|
| 语句 | `place(b, x, y, p)?` |
| 声明 | `let v = parse(n)?` / `var v = parse(n)?` |
| 赋值 | `v = parse(n)?` |
| 返回 | `return parse(n)?` |

别处报 `` `?` may only be used as `f()?`, `let x = e?`, `x = e?` or `return e?` ``。
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

`let`/`var`、`global let/var`、赋值（`=` `+=` `-=` `*=` `/=` `%=`）、
`if`/`else if`/`else`、`while`、`for`、`match`、`return`、`break`、`continue`、
块 `{ … }`、`trap(msg)`、`yield expr`、表达式语句。
`if`/`while` 的条件必须是 `bool`（`expected 'bool', found 'i32'`）。
`if`/`while`/`match` 的条件里不能直接写结构体字面量，要加括号
（`struct literal in a condition needs parentheses`）。
函数体内**不能**嵌套 `struct` / `fn` 声明。

### 11.2 `match` 是语句

- `match e { 变体 => 语句 … }`；分支体是**一条语句**，通常写成块。
- 必须**穷尽**所有变体，没有 `_` 兜底：漏一个报
  `` `match` does not handle `blue` -- every variant must be listed ``。
- 带载荷的变体用 `circle(r) => …` 按位置绑定；被 match 的表达式只求值一次。
- 它是语句不是表达式：想往外传值就在分支里 `return` / 赋值。

### 11.3 `for` 的形态

parser 把 `for` 脱糖成 `var` + `while`，共有三种脱糖形状；
按用法常被数成**四种**：区间、定长数组/视图、自定义迭代器、C 风格 —— 前两种共用"容器"这一形状。

| 形状 | 写法 | 说明 |
|---|---|---|
| 区间 | `for i in lo..hi { … }` | 半开区间；`i` 的类型从 `lo` 来；**上界只求值一次** |
| 容器 | `for d in subj { … }` | `subj` 只能是名字、字段或 `*p`；元素是**拷贝**，改 `d` 不写回 |
| C 风格 | `for (init; cond; step) { … }` | 三段都是普通语句/表达式；`continue` 跳到 step |

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
- `&&` / `||` / `=` / `+=` 等不可重载。

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
