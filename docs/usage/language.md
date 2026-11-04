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
| 控制台 | 写：`println(x)` 内建，或 `use std::io` 后 `io::cout << x << "\n"`；读：默认"读一行再解析"——`use std::text::string` 的 `string::readLine()` + `string::toI64`/`toF64`（`io::cin >> x` 是 token 快路，见 §7.5） |
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
- `println` / `print` 是内建：`println(x, y)` 等价于把各值按序写出再换行，任何可打印值都行
  （含 `string`，按字节写出）。它们和 `io::cout` 写**同一个缓冲**（`extc_cout_put`），所以混写
  顺序与源码一致，不需要 `flush`；程序退出时缓冲会被倒出去。

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
  **值位置**（`let s: string = f"…"`，展开成 `std::text::string::from(文字段) + 槽 …` 的 `+` 链，
  结果是 `string`）。`std::text::string` 由糖自己登记成一条隐藏导入 —— 这个位置不需要写 `use`。
- 值位置也接受格式说明符：带 `:…` 的槽按**槽值的静态类型**落成
  `std::text::string::padI64` / `padF64` / `padText` 调用（`std::text::stringio` 同样由糖自己登记），
  语义与 `<<` 右侧的说明符逐条相同。
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
`std::text::string` 是标准库里可增长的那个类型（见 [`stdlib.md`](stdlib.md)）。

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

### 3.5 可调用的值：`fn(...)` 与闭包

`fn(i64) -> i64` 的值是一个**裸代码指针**：只有代码，没有环境。`fn` 类型**没有零值**，要"可能没有"就写 `option<fn …>`。

lambda 字面量能不能放进 `fn(...)`，取决于它有没有捕获：

- 不捕获任何外层变量 ⇒ 环境是空的，值就是一个代码指针，可以直接当 `fn(...)` 用；
- 捕获了外层变量 ⇒ 环境要跟代码一起走，值是**闭包**而不是指针，放进 `fn(...)` 会被拒，诊断会说明"捕获的状态要跟代码一起走"。

```extc
// 结果：40 8
use std::io

fn twice(f: fn(i64) -> i64, v: i64) -> i64 { return f(f(v)) }

fn main() -> i32 {
    var bias: i64 = 3
    let add = fn(x: i64) -> i64 { return x + bias }
    io::cout << twice(fn(x: i64) -> i64 { return x * 2 }, 10) << " " << add(5) << "\n"
    return 0
}
```

闭包自己可以直接调用（`add(5)`），也可以显式写 `add.call(5)`。要把"带状态的行为"当参数传，接受方拿一个泛型参数并在体内调用它的 `call`：

<!-- fragment -->
```extc
fn apply<F>(f: F, v: i64) -> i64 { return f.call(v) }
```

`apply(add, 1)` 得到 `4`。这条路径按实例静态分发，没有间接跳转；`fn(...)` 那条只有一个代码指针，调用是一次间接跳转。

**同一个容器装不同的闭包**：声明一个只有 `call` 方法的 trait，然后把闭包放进 `dyn`。闭包的环境类型由编译器合成、用户写不出 `impl`，所以这种 trait 的实现由编译器补上（前提是环境里没有引用/视图，那些捕获会被单独拒掉）：

```extc
// 结果：6 7 85
use std::io

trait IntFn { fn call(self: ref Self, a: i64) -> i64 }

fn main() -> i32 {
    var bias: i64 = 5
    var d: dyn IntFn = dyn IntFn(fn(x: i64) -> i64 { return x + bias })

    var k: i64 = 2
    var fns: varArray<dyn IntFn> = varArray<dyn IntFn>::withCap(4)
    fns.push(dyn IntFn(fn(x: i64) -> i64 { return x + k }))
    fns.push(dyn IntFn(fn(x: i64) -> i64 { return x * 10 }))
    fns.push(dyn IntFn(fn(x: i64) -> i64 { return x - 1 }))

    var sum: i64 = 0
    var i: i64 = 0
    while i < i64(fns.size()) {
        sum = sum + (fns.get(i)!).call(7)
        i = i + 1
    }
    io::cout << d.call(1) << " " << d.call(2) << " " << sum << "\n"
    return 0
}
```

编译器替闭包补 `impl` 的条件：trait 的整个接口就是一个 `call`（参数个数与类型、返回类型都要与闭包一致），并且闭包的捕获**全是普通值**。捕获了引用或视图的闭包会被拒 —— `dyn` 值是 `{pool, slot, generation}` 句柄，它表达不了"载荷指向某个帧"这件事，存下来就是悬垂指针。同理，捕获的值如果**类型依赖某个类型参数**也会被拒：一个 `dyn` 载荷只有一份布局，而那种闭包在泛型函数的每个实例里各有一份布局（见下节）。

### 3.6 泛型体里的 lambda：环境按实例布局

lambda 的值是编译器合成的一个环境结构体，字段就是被捕获的变量。处在**泛型函数体**里时，捕获的变量类型可能依赖类型参数：

```extc
use std::io

struct key { v: i64
    fn hash(self: ref key) -> i64 { return self.v * 2 }
}
struct item { n: i64
    fn hash(self: ref item) -> i64 { return self.n + 7 }
}

fn calc<K>(k: K, d: i64) -> i64 {
    let f = fn(x: i64) -> i64 { return x + k.hash() }   // 捕获 k: K
    return f(d)
}

fn main() -> i32 {
    var a: key = key { v: 4 }
    var b: item = item { n: 5 }
    io::cout << calc<key>(a, 1) << " " << calc<item>(b, 2) << "\n"
    return 0
}
```

（这里取 `key` 的 `hash` 是 `v * 2`、`item` 的是 `n + 7`，所以输出 `9 14`。）

这种闭包的环境**按实例各有一份布局**（这里两份：字段分别是 `key` 与 `item`），连 `call` 方法也各发一份，名字带实例后缀。普通闭包（捕获的都是与类型参数无关的值）仍然只有一份布局，不付这个代价。

- lambda 体里的方法调用按实例回放检查：如果某个实例没有那个方法，报的是 `` `calc_plain` needs `plain` to define `hash` ``，不是静默地当成 0。
- 泛型体里**不依赖**类型参数的捕获（例如捕获一个 `i64` 局部变量）照旧只有一份布局，所有实例共享。
- 装进 `dyn` 的那条路仍然拒绝捕获类型参数相关值的闭包（一份载荷对多份布局，没有可写的实现）。

### 3.7 动态值 `any`：类型只在运行期知道

有时一个值要装"什么类型都行"的东西：解析结果、日志参数、插件返回的数据。`any` 就是这种值——它
**自己拥有内容**，任一时刻只有一个确定的类型，可以换类型、可以返回、可以进容器。

```extc
use std::io

struct point { x: i64  y: i64 }

fn make(k: i64) -> any {
    if k == 0 {
        var n: i64 = 7
        return n
    }
    var p: point = point { x: i64(3), y: i64(4) }
    return p
}

fn main() -> i32 {
    var v: any = i64(1)          // 装箱：隐式，塞什么类型都行
    var a: i64 = v               // 拆箱：也是隐式 —— 上下文要 i64，就按 i64 取
    v = "abc"                    // 同一个槽换类型：旧的被替换，不是叠加
    println("a=", a, " v=", v)   // a=1 v=abc
    var r: point = make(1)       // 函数返回 any，拆成具体类型
    println("r=", r.x + r.y)     // r=7
    var n: i64 = make(0)
    println("n=", n)             // n=7
    return 0
}
```

（上面的输出是 `a=1 v=abc` / `r=7` / `n=7`。）

规则只有四条：

| 形状 | 语义 |
|---|---|
| `var x: any = e` / 实参 / `return e` | **装箱**（隐式、无损）：把值拷进 `any` 自己的一格里 |
| `var y: T = x` / `f(x)` / `x + 1` / `if x == …` | **拆箱**（隐式）：上下文要 `T` 就按 `T` 取 |
| `x == y`（两边都是 `any`） | 按描述符比较内容；类型不同就是 `false` |
| `print(x)` / `println(x)` | 按描述符打印；空槽打印 `<none>` |

拆箱是 extC 里**唯一一条可能运行期失败的隐式转换**：类型不符就停在那一行，并且说清两个类型——

```
this `any` holds a `i64`, not a `string$string`
```

它不会静默重解释字节，也不会"尽力转换"（`i32` 装进去、要 `i64` 出来是失败，不是拓宽）。写
`i64(1)` 而不是 `1`，否则字面量按自己的默认类型（`i32`）装进去。

- 表示是 **16 字节**（与 `slice` 同宽）：一个描述符 + 内容。8 字节以内且无引用的值直接内联，其余
  在创建它的 arena 里拷一份，所以 `any` 从不指向将死的临时量。
- `any` 能进 `varArray`、能作为字段、能被返回；`option<any>` 用来表示"可能没有"。
### 3.8 `any` 上的动态方法调用

`any` 上可以直接写方法调用：哪一个实现跑，由值**运行期持有的类型**决定。

```extc
use std::io

struct box { w: i64  h: i64
    fn area(self: ref box) -> i64 { return self.w * self.h }
}
struct circle { r: i64
    fn area(self: ref circle) -> i64 { return self.r * self.r * i64(3) }
}

fn main() -> i32 {
    var v: any = box { w: i64(3), h: i64(4) }
    var a: i64 = v.area()          // 12
    v = circle { r: i64(2) }
    a = v.area()                   // 12
    println("a=", a)
    return 0
}
```

编译器在**每个调用点**生成一个派发器（按描述符比较的一串 `if`，没有函数指针表），候选集合是"程序里
声明了这个名字的无参方法的类型"——闭合世界。两条规则值得记住：

- **编译期只查名字存在**：同一个名字只要有**一个**类型声明过就能编过。加类型、加同名重载都不会让已
  经编过的程序突然编不过（这是"加类型不破坏已有程序"的一部分）。
- **找不到就 trap，报文说出类型**：`trap: no `area` for a `line``。不是静默返回零值。
- 返回值类型是 `any`（被调方的返回类型在调用点不可知），所以 `var a: i64 = v.area()` 里的拆箱照旧：
  类型不符就停在这一行。
- 派发器怎么找候选：候选 ≤8 个时是一串 `if (__t == &T_desc)`（在热循环里常被内联成直接调用，实测
  ≤0.7 ns）；**候选 >8 个时改成一棵排好序的查找表 + 二分查找**，再按命中的下标跳转，成本随候选数
  只按 log 增长（12 个候选 ≈ 2 ns/次，40 个 ≈ 4 ns/次；线性链在 64 个候选时是 9 ns、256 个时 25 ns）。
  表在第一次调用时排序一次（描述符地址到链接期才定，编译期无法排好序），之后只比地址。
- v1 的限制：**不能带实参**；候选只覆盖普通结构体（泛型实例的方法暂不参与派发）。

### 3.10 读一个运行期才知道类型的值：`std::reflect`

描述符不只用来打印：`std::reflect` 把它们当数据读。

<!-- fragment -->
```extc
use std::json

var v: any = point { x: i64(1), y: i64(-2) }
var buf: mut slice<u8> = new u8[256]
let n: i64 = reflect::json(ref v, buf[..])     // {"x":1,"y":-2}，返回需要的字节数
let t: i64 = reflect::typeName(ref v, nm[..])  // "point"
```

`json` 返回的是**需要**的字节数，即使它超过缓冲长度：先给小缓冲问长度、再给够大的写一次。
类型是 `ref` 的字段编码成 `null`（引用不跟进去）；枚举编码成变体名；`slice<u8>` 当文本转义。
编码不分配、不在用户代码里递归——走多深由类型图的深度决定（上限 32 层）。

不需要 `any` 的时候不要用它：`io::cout << x` 对具体类型本来就是零成本的结构化打印。

### 3.9 `any` 拥有它装的值

装箱是**拷贝**，不是挂一个指针：`var v: any = expr` 之后，`expr` 那边再改，箱内不变。

- 值本身的字节被复制进 arena（≤8 字节且不含视图时直接存在 `any` 里，零分配）。
- 值里含**视图**（`slice<T>`、字节视图、含视图的结构体）时，视图指向的字节也会被复制一份：
  `var v: any = buf[..]` 之后改 `buf`，箱内仍是旧字节。这就是"箱内值能活过它来源的帧"的原因。
- 值里含**引用**（`ref`/`?ref`）时复制的是引用本身（不深挖指向的对象）；它是否安全仍然由既有的深度
  检查决定 —— 把带引用的结构体当作 `any` 从函数里返回会被拒（"would hold a reference to a local
  variable that dies first"）。
- 代价：装一个**含视图**的值每箱要分配两块内存（结构体 + 视图字节），在热循环里反复装箱会让
  arena 持续增长（实测 16 字节结构体 + 16 字节视图 ≈ 25 ns/箱，其中大头是分配与缺页）。装箱一次、
  之后再改箱内值，或者热路径根本不进 `any`，是推荐的用法；纯标量/≤8 字节的值装箱 ≈ 0.6 ns。

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

### 3.11 `impl` 挂在泛型实例上：`slice<T>` 的方法

`impl` 的目标可以是一个**泛型实例**，方法就属于那个实例自己的方法集（不共享给 `slice<i32>`）：

<!-- fragment -->
```extc
impl slice<u8> {
    fn firstByte(self: ref slice<u8>) -> u8 { return self[0] }
    fn fillWith(self: mut ref slice<u8>, v: u8) { … }
}
```

接收者有两种写法，都能找到同一个方法集：只读视图 `arr[..]`，和 `new u8[n]` 产生的**可写视图**。
可写视图是只读实例的"影子"类型（`slice<mut T>`，与 `slice<T>` 是同一个 C 结构体、同一个名字），
所以它共用方法集，也能调只读方法；反过来不行 —— `mut` 是权限，只能丢不能凭空加。

运算符同理：`impl slice<u8> { fn ==(self: ref slice<u8>, other: ref slice<u8>) -> bool }` 之后，
两个可写视图可以直接 `a == b`（匹配时 `mut` 会被规范化掉）。

定点数组不行：`[N]T` 是**形状**而不是声明，没有方法集可挂，写 `impl [8]u8 { … }` 会报
`` `impl` cannot extend a fixed array `[N]T` `` —— 要么把方法写进它元素类型的 `struct` 里，
要么写在它切片后得到的 `slice<T>` 上。

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

- 取引用的表达式是 `ref x`，作用于变量、字段、`*p`，以及**定长数组的元素**：
  数组的存储就在帧里、永远不会搬家，所以引用不可能因为扩容失效；越界在**取引用那一刻**
  trap，不在解引用时。视图的元素不行（`ref v[1]` → `cannot take a reference to an element
  of a view`），因为视图背后的存储可能随容器扩容搬家；可增长容器的元素同理。
  视图的元素**分两种**：存储是这一帧自己取来的（`new T[n]`、以及它的窗口）可以取引用 ——
  存储不可能被谁搬走；指向容器板块的视图（`string::view()`、容器自己的视图）不行，扩容会
  把板块换掉。其他表达式（调用结果、算术结果）也不行。

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
- **字面量切片的深度是 0**：`let k: slice<u8> = "…"` 的存储是静态存储期（与全局同寿），
  所以绑成 `let` 之后可以跨 `yield` / `ext` 使用；局部数组的视图跨 `yield` 仍然被拒
  （那才是"指向别人栈帧"）。
- **容器在视图参数上自动借出**：只读参数上 `total(va)`（`varArray<T>`）等价于
  `total(va.view())`、`total(a)`（`[N]T`）等价于 `total(a[..])`；可写参数
  （`mut slice<T>`）上 `fill(va)` 等价于 `fill(va.asMutSlice())`、`fill(a)` 等价于
  `fill(a[..])`，后者要求容器本身是可写的地方（`var` 绑定），借用检查照常把关。
  方法实参同理（`s.append(bytes)` 的 `bytes`）；**表达式位置也借**（`println(f"…")`、
  `take(f"seven")` 这类右值直接过）。编译器在调用位置造的就是这些节点，借用与生命周期检查照常生效。元素类型对不上时（`varArray<i32>` 传给 `slice<i64>`）不借，
  诊断指出该写什么。
- **借出不是内建名单**：任何类型只要方法 `view() -> slice<T>`（只读）或
  `asMutSlice() -> mut slice<T>`（可写）的**返回类型**对得上，就能出现在这类参数上 ——
  判据是签名，不是容器是哪个内建类型。`varArray` 与定长数组只是内建的两个现成做法。
  只有 `view` 的类型不会被借成可写视图（可写参数仍要求 `asMutSlice`）。

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
- **rank-2 形状**（目标是 `ndview<T>`）：不展开循环，直接落成 `std::nd` 的逐元素内核调用
  —— `dst = a + b` → `nd::addInto`、`a - b` → `subInto`、`a * b` → `mulInto`、
  `a * k` → `scaleInto`、`a + b * k` → `axpyInto`，复合形式同理（`dst += a` →
  `addInto(dst, dst, a)`）。形状在运行期才知道，所以内核返回 `bool`，编译器在外面补一条
  `if !… { trap("…same shape as the destination") }`。写出来的 C 就是一次内核调用加一次
  失败分支（形状不匹配才走 trap），热路径没有额外指令；`v[i, j]` 读写照旧是 `nd::at2` /
  `nd::set2`。接收者可以直接是 `ndarray<T>`：`grid[r, c] = x` 由编译器借出 `viewMut()`
  （读借 `view()`），不必先自己拿一份视图；`let` 的 ndarray 写仍被借用检查拒绝。

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

### 7.5 输入：默认"读一行再解析"

命令行程序的输入主路径是 `std::text::stringio` 的 `readLine()`（每次读一行、`?string`，EOF 给
`none`）加 `string::toI64` / `string::toF64`（`?i64` / `?f64`）。这样错误能指明是哪一行、
解析器可以单独测，循环也短：

<!-- fragment -->
```extc
use std::text::string
use std::io

fn main() -> i32 {
    var sum: i64 = 0
    while true {
        let line = string::readLine() ?? break
        if line.len() > 0 { sum = sum + (string::toI64(line) ?? 0) }
    }
    println(f"sum={sum}")
    return 0
}
```

`io::cin >> x` 仍在，但它只适合"一遍扫过成千上万个 token、不想每行分配"的快路：它按空白
切 token、没有行号，遇到坏输入只能拿到 `cin.bad`。要在两种之间选时，默认选上一段那种。

---

### 7.6 相邻循环的融合（临时视图消失）

写法和没有融合时**一字不差**，只是编译器把"写进临时视图、下一段再整块读"的两段合并成一个循环：

<!-- fragment -->
```extc
use std:io

fn pipeline(a: slice<f64>, b: slice<f64>, out: mut slice<f64>) {
    var tmpv = new f64[a.len]
    for i in 0..a.len { tmpv[i] = a[i] * 2.0 + b[i] }
    for i in 0..out.len { out[i] = tmpv[i] * 0.5 }
}
```

- **形状**：第一段只写 `tmpv[i] = <纯表达式>`；第二段与它**相邻**、同一迭代空间（变量名与
  上下界相同）；`tmpv` 在第二段里只以 `tmpv[i]` 的形式读、第二段写的是别的视图；`tmpv` 在本块
  其它地方没被用到。命中就合成一个循环，临时视图连分配一起消失。
- **归约也折得进去**：第二段是 `let s = seq::sum(tmpv) ?? 0`（或 `seq::max/min`、或
  `for v in tmpv { acc = acc + v }`）时，累加直接进写循环，读一遍、不留临时数组。
- **判据**：两臂输出逐字节相同（`--no-fuse` 关掉融合做对照）；融合臂生成 C 里查不到那个临时视图；
  反例（`tmpv` 在别处被用、读的下标不同、右值里有调用）必须**不**融合。
- **边界**：只看同一个块里**相邻**的语句；不跨函数调用、不跨 `par`；容器形式只认 `+` 与"元素本身"。

`bench/fusion`（n=1048576、iters=8，`-O2`）实测：元素链 `fused/unfused` 0.54（快 1.86×），
流式归约 0.16（快 6.03×），与手写融合的差在两个百分点内。

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

| | `varArray<T>`（prelude 里） | `pool<T>`（`std::data::pool` 里） |
|---|---|---|
| 存储 | 一块连续 buffer，满了按 2 倍扩容 | 值放 dense 数组，另有 slot→dense 的映射 |
| 取 / 写 | `v.get(i) -> ?T`、`v.set(i, v)`（按值拷） | `p.get(h) -> ?T`、`p.set(h, v) -> bool`（按值拷） |
| 可寻址性 | `v[i]` **不行**（`cannot index a value of type 'varArray<i32>'`）；要借出视图：`v.view()` / `v.asMutSlice()` → `slice<T>`，视图元素可索引、可写 | 没有按索引访问；`p.snapshot()` 借出活元素前缀的视图 |
| 装引用 / 视图 | 可以：`varArray<slice<u8>>`、`varArray<ref i32>` 都编过（存的时候按寿命检查） | 可以：`pool<slice<u8>>` 编过 |
| 回收 | 存储是**板块**：扩容当场把旧板还给分配器，`release()` 也能提前还；`clear()` 只把 `len` 归零、板留着复用 | **逐对象回收**：`remove(h)` 把最后一个值搬进空洞、该槽位"代 +1"；之后旧句柄 `get` 得 `none`（不会读到新值） |
| 句柄 | 下标（会因扩容/搬移失效） | `{ slot, gen, ep }`，陈旧句柄可判定 |

扩容的规矩要记住：`push` 换掉存储之后，**之前借出的视图就作废了** —— 旧板已经还给分配器，
所以"增长之后还用旧视图"在同块内是编译错误（`\`v\` changes the storage that \`s\` views`），
跨帧则根本借不出去。用完再增长，或者增长之后重新取视图：

```extc
// 结果：1 2 —— 先借、先用，再增长；增长之后重新借
use std::io
fn main() -> i32 {
    var v: varArray<i32> = varArray<i32>::withCap(2)
    v.push(1)
    v.push(2)
    var s: mut slice<i32> = v.asMutSlice()
    s[0] = 99
    var first: i64 = i64(s[0])            /* 视图用完这里之后不再出现 */
    v.push(3)                             /* 增长：`s` 已经不在了 */
    io::cout << first << " " << (v.get(1) ?? i64(-1)) << "\n"
    return 0
}
```

### 8.3 循环里换缓冲：一条警告

帧的存储按块归还，所以循环体里 `new` 出来的东西每轮都还。例外只有一个：**把新 buffer 写进
循环外声明的变量** —— 那块存储属于外层块，于是每轮新分配的那份在下一轮开始时还活着。回收器
最终会收掉被替换的块，但**峰值由收集底盘决定**而不是存活量：一百轮各 1 MiB 实测 11.8 MB，
同一行写进循环体只要 2.6 MB；关掉回收时是 103.8 MB、随轮数线性涨。程序行为完全正确、编译也
不报错，所以编译器对这个形状给一条警告：

<!-- fragment -->
```extc
var buf: mut slice<u8> = new u8[1]
var i: i64 = 0
while i < 1000 {
    buf = new u8[1024 * 1024]       // ← 这一行响警告
    i = i + 1
}
```

三种改法：把声明搬进循环体（每轮结束整块归还）；本来就是"每轮复用一个缓冲"的意图，用
`@overwrite`（见 8.1）；要变长就换成自带板块的容器（`vector`、`string`、`hashMap`），
它们原地扩容，增长历史不累积。要多少内存、差多少，见 [内存模型](../MEMORY-MODEL.md)。
警告不影响退出码，`-w` 全部关掉。

警告只管"把 `new` 写进外层的变量"这一种。值如果是从函数调用拿到的、中间还过了 `??`、
`some(...)` 或结构体字面量，存储跟着**存放它的那个块**走：写在循环体里就随循环体释放，
不会攒到函数退出。

```extc
// 结果：stale=none slot=0 gen=1 / len=2
use std::io
use std::data::pool
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
- 右边可以写 `break` / `continue`：`let v = maybe(i) ?? continue` 一行就是"求值一次、没有就
  跳转"，语义是 `if isNone(主体) { continue }`（`break` 同理），只允许出现在循环体里。
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
| `@place` | `struct` | 该类型拥有一个**地方**（今天是一个池）：存储住在那个地方的板块上，可以原地增长、寿命随所在的域；旧拼写 `@poolObject` 仍接受 | `@place goes on a struct` |
| `@sharesStorage` | `struct` | 按值拷贝时两份共用存储（容器那一族）；编译器给警告 | `@sharesStorage goes on a struct` |
| `@frozen` | `struct` | 布局就是 C 的布局，是按值过 `extern!` 边界的门票 | `@frozen goes on a struct` / `appears twice` |
| `@export` | **自由函数** | 生成 C 可见符号（非 `static`），C 可以直接调 | `@export goes on a function`（方法 / 泛型 / 协程 / `extern!` / 结构体 / 枚举 / 全局都拒） |
| `@builtin` | 函数（**没有函数体**） | 声明由编译器实现（stdlib 用：`parallel::run`、域、`extc_viewOf`） | `appears twice` |
| `@overwrite` | 局部 `var` 声明 | 复用同一块存储；初始式必须恰好是 `new` | `` `@overwrite` needs `var` `` / `` `@overwrite` only applies to `new` `` |
| `@ext` | `impl` 块 | 块扩展的是**别的模块**声明的类型（见 [`modules-cabi.md`](modules-cabi.md) 的 §1.4b） | `` `@ext` applies to an `impl` block `` / `` `@ext` on an impl block of the type's own module `` |
| `@recursive` / `@main` | — | 现在会被拒绝，编译器直说没有实现 | `` `@recursive` is not implemented yet ``（见 [`../TODO.md`](../TODO.md)） |

`trait` 声明身上**没有注解适用**；`impl` 块只接受 `@ext`（其余注解报 `no annotation applies to an impl block`）。

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
| 容器 | `for d in subj { … }` | `subj` 可以是任意表达式（不是 place 时编译器先收进隐藏绑定，求值一次）；元素是**拷贝**，改 `d` 不写回 |
| C 风格 | `for (init; cond; step) { … }` | 三段都是普通语句/表达式；`continue` 跳到 step |
| 并行 | `par i in lo..hi { … }` | 数据并行（多线程跑同一个循环体），形状与区间 for 相同；见 11.8 |

容器形状覆盖：定长数组、`slice<T>`（含字符串字面量）、协程帧，
以及任何声明了 `iter` / `next` / `value` 三个方法的用户类型
（`fn iter(self: ref Self) -> It`、`fn next(self: mut ref It) -> bool`、`fn value(self: ref It) -> T`）。
`varArray` / `string` 这类**有 `view()` 的容器会被自动借出**：`for d in va` 等价于
`for d in va.view()`，不用手写 `.view()`。`vector<T>` 故意不提供零拷贝视图（`snapshot` 拷贝），
迭它要显式写 `for d in v.snapshot()`。

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
  返回新值"的运算符是合法形状（见 `std::text::string` 的 `+`：`s + "…"`、`a + b`）。
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

归约是第二形状，不是我行我素的第三种语法：先 `par` 写进数组，再用 `std::data::seq` 的
`sum` / `max` / `min` / `count` 合并（`seq::sum(xs) ?? f64(0.0)`）。这两个形状合起来覆盖
"切块算 + 汇总"这条主线。

### 11.9 默认参数

形参可以带默认值，写了默认值的那些可以**在调用点省略**（省略的只能是尾部）：

```extc
// 结果：30 / 7 / 7
use std::io

fn scale(n: i64, by: i64 = i64(2), plus: i64 = i64(0)) -> i64 { return n * by + plus }

struct counter {
    n: i64
    fn bump(self: ref counter, by: i64 = i64(1)) -> i64 { return self.n + by }
}

fn main() -> i32 {
    var c: counter = counter { n: 6 }
    println(scale(i64(15)))                 // 30：(by=2, plus=0)
    println(scale(i64(3), i64(1), i64(4)))  // 7
    println(c.bump())                       // 7：(by=1)
    return 0
}
```

三条规则，都是**声明处**就能查出来的：

1. **默认值必须是编译期常量** —— 字面量，或对它的一次转换/取负（`i64(0)`、`-i64(3)`）。
   默认值会被复制进每一个省略该实参的调用点，读不到被调方的局部变量，所以这不是限制而是
   唯一能工作的形状。写成 `b: i64 = a` 报 `a default value must be a compile-time constant`。
2. **默认值必须形成后缀** —— 一旦某个形参有默认值，它后面的形参都必须有。
   写成 `fn f(a: i64 = i64(1), b: i64)` 报 `parameter `b` comes after a parameter with a
   default value, so it needs one too`（"省略的实参"只能是尾部，否则调用点无法判定省略的是哪一个）。
3. **`self` 不能有默认值**，`extern!` / `@builtin` 声明也不能有 —— 后者是对已有 C 函数的承诺，
   那个函数不知道默认值这回事。

实参个数不对时，诊断给的是**区间**而不是一个数：

```
error: `scale` expects 1 to 3 argument(s), got 0
error: `scale` expects at most 3 argument(s), got 4
```

实现上没有动调用约定：省略的实参在**检查器**里被补成一棵克隆的常量表达式，代码生成照旧把
所有实参传满，所以生成的 C 与"用户自己写全"完全一样。

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
| `var v: varArray<T>` 上 `v[i]` | `cannot index a value of type 'varArray<i32>'`（用 `get`/`set`/`view`） |
| 嵌套超过 64 层块 | `nesting too deep in a block (the compiler's limit is 64 levels)` |
| 表达式树超过 2048 层（`a + a + …` 的长链，或机器生成的表达式） | `expression too deep to check (the compiler's limit is 2048 levels)` |

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
- 定点格式化在 `|v| >= 1e15` 时**运行时 trap**（`io::fmtF` 的边界）；要打这么大的数就先用 `<<`，
  或自己缩小数量级。
- 没有 `+` / `#` / 千分位之类的符号选项，也没有 `e` / `x` 等类型码：`d` / `f` / `s` 只做
  类型一致性校验，不换写法。
