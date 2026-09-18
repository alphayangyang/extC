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
| 字符串字面量 | `str` ⚠️ **临时，week-1 会改成 `Slice<u8>`** | `const char *` |
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
type Status = | ok | warn | error      // 前导 `|` 可写可不写
```

- 变体用 **`类型名.变体名`** 访问：`Status.ok`
- 变体名不必全局唯一 —— `Status.ok` 和 `Result.ok` 是两个不同的东西
- 只有**同类型**才能比较和赋值（不能拿 `i32` 跟枚举比）
- 枚举**自动有名字文本**（定案 11）：`println(s)` 直接打印 `warn`

```extc
type Status = | ok | warn | error

if s == Status.ok { ... }      // 可以
if s == 0 { ... }              // error: 类型不匹配
println(s)                     // warn
```

### 结构体类型

顶层 `struct` 定义的类型就是类型名，PascalCase：

```extc
struct Point {
    x: i32
    y: i32
}
```

### 引用类型 `ref T`

`ref T` 是「不可为空、不可做算术的引用」，在 C 里就是 `T *`。

**目前 `ref T` 只在函数参数上有用** —— 因为还没有「产生一个引用」的语法（`ref x` 表达式是 week-1 的任务 T3）。

```extc
fn moveBy(self: ref Point, dx: i32) {
    self.x = self.x + dx      // ref 用 `.` 访问字段，编译器生成 `->`
}
```

---

## 4. 变量与声明

```extc
let name: Type = 表达式      // 不可变绑定
var name: Type = 表达式      // 可变变量
var name: Type               // 零初始化（定案 8）
```

- **类型标注可以省**（局部变量）：`var y = 4` 从初始化式推导。
- **省略初始化式就必须写类型**：`var b: Board` 会**自动清零** ——
  C 里最大的 UB 来源之一就是「读到未初始化内存」，它只能在运行时发现；
  默认清零把它变成编译期就能保证的东西。
- `ref T` **不能零初始化** —— 它是不可为空的引用，没有「零值」。
- 给 `let` 赋值是**编译错误**。

```extc
let x = 1
x = 2              // error: cannot assign to `x`, which is a `let`

var b: Board       // 所有字段清零
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
let p: Point = { x: 1, y: 2 }    // 靠声明类型补全
let q = Point { x: 1, y: 2 }     // 写全类型名也行
let o: Point = {}                // 空字面量 = 零初始化（靠声明类型补全）
```

裸 `{}` 的类型**从上下文推导**，只在上下文能唯一确定类型的地方允许：

- `var x: T = {}`
- `return {}`（函数返回 `T`）
- `f({})`（形参类型是 `T`）

推导不出来就报错，**绝不猜**。

> `Name { ... }` 只在名字**首字母大写**时被当作结构体字面量 —— 用命名规范消歧义，省掉一个关键字。

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
struct Point {
    x: i32
    y: i32

    fn moveBy(self: ref Point, dx: i32, dy: i32) {
        self.x = self.x + dx
        self.y = self.y + dy
    }

    fn magnitudeSquared(self: ref Point) -> i32 {
        return self.x * self.x + self.y * self.y
    }
}

fn main() -> i32 {
    var p: Point = { x: 1, y: 2 }
    p.moveBy(3, 4)                  // 接收者自动取地址，调用点不用写 ref
    println(p.magnitudeSquared())   // 52
    return 0
}
```

细节：

- **接收者自动取地址**：`p.moveBy(...)` 里 `p` 是值、`self` 是 `ref Point`，编译器生成 `&p`。
  反过来（接收者是 `ref T`、`self` 是值类型）会自动解引用。
  > 规则：**`.` 本身就表示「在这个值上操作」**，所以方法接收者不用写 `ref`；
  > 而**自由函数的 `ref T` 实参必须在调用点写 `ref`**（见下）。
- **方法名有命名空间**：`Point.eq` 和 `Board.eq` 是两个不同的名字，
  顶层不用为了避冲突而发明 `point_eq` 这种名字。C 里生成 `Point_eq` 做区分。
- 字段访问按类型决定用 `.` 还是 `->`，用户不用管。
- **自由函数不能有 `self` 参数** —— `self` 只属于方法。

## 7.5 引用：`ref` 是表达式

`ref T` 是「不可为空、不可做算术的**可变**引用」。

**`ref` 在调用点要显式写**（定案 10）——让读代码的人一眼看出这里传的是引用不是拷贝：

```extc
struct Counter {
    n: i32
    fn bump(self: ref Counter, by: i32) { self.n = self.n + by }
}

fn addTo(c: ref Counter, by: i32) {     // 自由函数的引用参数
    c.bump(by)                          // 方法接收者自动取地址
}

fn main() -> i32 {
    var c: Counter = { n: 10 }
    addTo(ref c, 7)                     // ← 必须写 ref
    println(c.n)                        // 17
    return 0
}
```

- 实参**本身就是引用**时，不用再写 `ref`，直接传。
- `ref` 是**可变**引用，所以**不能对 `let` 取引用**：
  ```extc
  let c: Counter = {}
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

**格式由编译器按静态类型选** —— 用户永远不写 `"%d"`。支持的类型：所有整数、浮点、`bool`、`str`、**枚举**。

```extc
println(42)         // 42
println(3.14)       // 3.14
println(true)       // true
println("hi")       // hi
println(Status.warn)// warn   ← 枚举自动有名字文本（定案 11）
```

> ⚠️ **格式串 `{}` 已定案要加**（编译期展开，不是运行时解析），但 week-0 还没实现。见第 10 节。

---

## 9. 错误信息

错误带**文件、行、列、源码上下文和插入符**：

```
examples/bad.extc:3:17: error: cannot assign to `x`, which is a `let`
      x = 2
          ^
  note: 改成 `var` 才能重新赋值（`let` 是不可变绑定）
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
- 不存在的变体（`Status.nope`）
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
| **泛型** | `Slice<T>` / `Array<T>`，靠 C 代码生成实现 | T4 |
| **字符串字面量 = `Slice<u8>`** | `str` 作废 | T4 |
| **`Option<T>` / `Result<T,E>` / `?`** | | T5 |
| **格式串 `{}`** | **编译期展开**，不是运行时解析；必须是字面量 | T5 之后 |
| **全局变量** | 全局 = 深度 0 的 arena，`static` 关键字因此消失 | week-2 |
| **`@main` 注解** | 标在任意函数上，不再硬编码 `main` | week-2 |
| **数组 + 下标** | `[15][15]i32`；越界规则见 `DESIGN.md` §4 | week-2 |
| **`for` 四种形态** | `for d in dirs` / `for i in 0..n` / `for d in -2..3` / C-style | week-2 |
| **`match`** | 语句和表达式都能用；能匹配变体和常量；臂可发散 | week-3 |
| **模块系统** | `module` / `export` / `import` / `::` | week-3 |
| **`region` / 逃逸检查** | **这是 extC 的命** | week-4 |

完整清单和理由见 [`DECISIONS.md`](DECISIONS.md) 和 [`PLAN.md`](PLAN.md)。

---

## 11. 还没定的

`Result<void,E>` 还是 `Result<(),E>`、`@main` 和 `module main` 的优先关系、`+=` 复合赋值、`&&`/`||` vs `and`/`or`、无返回值函数要不要强制 `-> void`、要不要做多错误报告。

---

## 12. 完整示例

见 `examples/`：

| 文件 | 演示 |
|---|---|
| `hello.extc` | 变量、`if/else`、`while`、函数调用、打印 |
| `fizzbuzz.extc` | `else if` 链、`%`、`while` |
| `types.extc` | 拓宽自动、字面量按值适配（T2） |
| `structs.extc` | struct、**方法写在 struct 体内**、裸 `{}` 推导、**零初始化** |
| `enums.extc` | `type` 枚举、`Status.ok`、枚举自动打印名字 |
| `refs.extc` | **`ref` 表达式**、自由函数实参写 `ref`、方法接收者自动取地址 |

跑测试：

```sh
./tests/run.sh     # 正例跑通 + 反例必须被编译期挡掉
```
