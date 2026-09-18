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

### 引用类型 `ref T`

`ref T` 是「不可为空、不可做算术的引用」，在 C 里就是 `T *`。

用 `ref x` 产生一个引用（见 §7.5），**不能为空** —— 所以省掉字段初始化时
编译器会给一个**明确的错误**，而不是塞个 `NULL` 埋雷。

```extc
fn moveBy(self: ref point, dx: i32) {
    self.x = self.x + dx      // ref 用 `.` 访问字段，编译器生成 `->`
}
```

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
let s1 = a[2..5]      // [30, 40, 50]      slice<i32>
let s2 = a[6..]       // [70, 80]          到尾
let s3 = a[..3]       // [10, 20, 30]      从头
let s4 = a[..]        // 整个数组
s1[0] = 999           // 视图是同一块内存 —— a[2] 也变成 999
```

**视图元素是可写的**（`slice<T>` 里的 `data` 本来就是 `ref T`，而引用在 extC 里必须显式可见）。

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
- 给 `let` 赋值是**编译错误**。

```extc
let x = 1
x = 2              // error: cannot assign to `x`, which is a `let`

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

| 优先级 | 运算符 |
|---|---|
| 1 | `f(x)` 调用、`a.b` 字段、`a.f(x)` 方法 |
| 2 | `-x`、`!x`（一元） |
| 3 | `*` `/` `%` |
| 4 | `+` `-` |
| 5 | `<` `<=` `>` `>=` |
| 6 | `==` `!=` |
| 7 | `&&` |
| 8 | `\|\|` |

**赋值 `=` 不是表达式，是语句** —— 所以 `a = b = c` 写不出来。

### 没有 `&` 运算符

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

`ref T` 是「不可为空、不可做算术的**可变**引用」。

**`ref` 在调用点要显式写**（定案 10）——让读代码的人一眼看出这里传的是引用不是拷贝：

```extc
struct counter {
    n: i32
    fn bump(self: ref counter, by: i32) { self.n = self.n + by }
}

fn addTo(c: ref counter, by: i32) {     // 自由函数的引用参数
    c.bump(by)                          // 方法接收者自动取地址
}

fn main() -> i32 {
    var c: counter = { n: 10 }
    addTo(ref c, 7)                     // ← 必须写 ref
    println(c.n)                        // 17
    return 0
}
```

- 实参**本身就是引用**时，不用再写 `ref`，直接传。
- `ref` 是**可变**引用，所以**不能对 `let` 取引用**：
  ```extc
  let c: counter = {}
  addTo(ref c, 1)      // error: cannot take a mutable reference to `c`, which is a `let`
  ```
- 只有变量和字段能取引用（`ref f()` 不行）。

> ⚠️ **逃逸检查还没做**（week-4）。现在能拿到指向函数局部变量的引用而不被检查。

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

> ✅ **刚做完（2026-09-18）**：默认零初始化、方法进 struct 体内、`type` 枚举、
> `ref` 表达式与调用点显式。下面剩下的都还没做。

| 特性 | 定案内容 | 计划 |
|---|---|---|
| **`slice<T>` / `array<T>`** | 泛型机制✅已通；容器本体要用 extC 预lude 写 | T4b |
| **`option<T>` / `result<T,E>` / `?`** | | T5 |
| **格式串 `{}`** | **编译期展开**，不是运行时解析；必须是字面量 | T5 之后 |
| **全局变量** | 全局 = 深度 0 的 arena，`static` 关键字因此消失 | week-2 |
| **`@main` 注解** | 标在任意函数上，不再硬编码 `main` | week-2 |
| ~~**数组 + 下标**~~ | ✅ **已完成** —— `[15][15]i32`、字面量、越界 trap（见 §3「数组类型」） | ✅ |
| ~~**切片视图 `a[lo..hi]`**~~ | ✅ **已完成** —— 四种写法 + 编译期证明 + 可写视图 | ✅ |
| **动态数组 `array<T>`** | 等 arena 到位 | week-4 |
| **`for` 四种形态** | `for d in dirs` / `for i in 0..n` / `for d in -2..3` / C-style | week-2 |
| **`match`** | 语句和表达式都能用；能匹配变体和常量；臂可发散 | week-3 |
| **模块系统** | `module` / `export` / `import` / `::` | week-3 |
| **`region` / 逃逸检查** | **这是 extC 的命** | week-4 |

完整清单和理由见 [`DECISIONS.md`](DECISIONS.md) 和 [`PLAN.md`](PLAN.md)。

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

跑测试：

```sh
./tests/run.sh     # 正例跑通 + 反例必须被编译期挡掉
```
