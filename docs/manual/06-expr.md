<!-- 本页原来是 docs/MANUAL.md 的「5. 表达式与运算符」一节（原 §5.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 4. 变量与声明](05-vars.md) · [6. 语句 →](07-stmt.md)

---

# 5. 表达式与运算符

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

**赋值 `=` 不是表达式，是语句** —— 所以 `a = b = c` 写不出来。复合赋值 `+=` `-=` `*=` `/=` `%=` 有（定案 92）；`++` / `--` 有意不做。

位运算只许**整数**（不是数字就行）：`1.5 & 2` 报错，`true & false` 也报错 ——
**`bool` 在 extC 里不是整数**（不像 C）。

于是 C 那个著名坑在这里是**编译错误**而不是静默算错：

```extc
if 6 & 3 == 3 { }     // error: cannot apply `&` to `i32` and `bool`
                      // C 里它是 6 & (3 == 3) = 0，一声不响
```

```extc
let lowbit = i & (-i)     // 树状数组的标准写法
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

要写字段或元素，接收者必须是 **`mut ref`**（定案 ㉝）—— `self: ref` 里写会报
"cannot write through a read-only reference"：

```extc
struct board {
    moves: i64
    cell: [2][2]i64

    fn clear(self: mut ref board) {   // 写字段/元素 ⇒ mut ref
        self.moves = i64(0)
        self.cell[0][0] = i64(0)
    }
}

fn main() -> i32 {
    var b: board = { moves: i64(1), cell: [[1, 2], [3, 4]] }
    b.clear()
    return i32(b.moves)              // 0
}
```

**写标量就写 `*p = v`**：引用本身是值，`p = p + 1` 会被拒（"`mut ref i64` is a **reference**,
not a value -- dereference it first: `*p`"）；显式解引用之后读写都通，
`examples/ref-scalar.extc` 就是这个形状。。

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
> if p == point { x: 1 } { }                // 报错：条件里的 `{` 是块
> if p == (point { x: 1 }) { }              // 加括号
> ```
>
> 规则写在**语法**里，不藏在**命名**里（以前是靠「首字母大写」当字面量，那是隐藏魔法）。
>
> 诊断信息同样会直接指出需要加括号：
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

> ### 返回值规定为 `bool` 的原因
>
> 不是随便规定的，是三个前提推出来的：
>
> 1. `a == b` 天然会被用在 `if` / `while` / `&&` / `||` 里
> 2. extC **没有隐式真值转换**（`if 1` 是错的）
> 3. `!=` 是靠 `==` 取反实现的
>
> ⇒ 所以 `==` 只能是 `bool`，否则它根本没法用在条件里。
>
> **需要其他结果时，改用别的方法名即可** —— `fn compare(...) -> ordering`、`fn diff(...) -> diff`
> 之类**没有任何限制**，随便返回什么。只有 `==` 这个名字被绑定了 `bool`，
> 因为它在源码里的位置决定了它是「一个条件」。
>
> 编译器在**写下定义处**即检查该规则，而不是等到被使用处才检查。

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

var a: wrapper<i32> = { value: 7 }     // i32 内建 →
var b: wrapper<tag> = { ... }          // tag 没定义 == →
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
if a == "hi" { }          // 逐字节比，true
if a == "ho" { }          // false
```

> 编译器**不生成**指针比较 —— 那正是 C 字符串最常见的假答案（`"hi" == "hi"` 在 C 里
> 靠编译器池化碰巧为真，换个写法就假）。extC 里 `==` 一定走内容。

---

**[索引](README.md)** · [← 4. 变量与声明](05-vars.md) · [6. 语句 →](07-stmt.md)
