# extC 的「面」：命名规范与样板

> **v2（纠正版）**
>
> ⚠️ 奶昔上一步猜错了轴。主人说「不喜欢 AI 给我的语法范式」时，指的是**命名规范**，不是语法风格 —— 主人喜欢 Go，Rust/Go 味的骨架没问题。上一版 `SYNTAX.md`（三个 C 味候选）是奶昔过度解读，已作废。

---

## 1. 主人的口味（从那一行 `var thisIsAGoodName: i32` 直接读出来的）

| 规矩 | 结论 |
|---|---|
| 变量 / 函数 / 方法 | **camelCase**（`thisIsAGoodName`、`longestRun`、`isEmpty`） |
| 类型 / 模块 | **camelCase**（`board`、`array`、`moveError`）—— **强制检查** |
| 变量的类型写在哪 | **名字后面**：`name: Type` |
| 声明关键字 | `var`（可变）/ `let`（不可变） |
| 整数类型 | **`i32` / `i64` / `u8`**，不是 C 的 `int` / `long` |
| 数组类型 | 前缀式：`[15][15]i32` |
| 泛型参数 | **首字母大写**：`T` / `K` / `V`（也可以写 `Element`）—— **强制检查** |

**类型名和泛型参数的大小写规则是强制检查的，因为它解决一个真问题**：

```extc
struct T { x: i32 }        // ✗ error: type name `T` must start with a lowercase letter
struct box<T> { ... }      // T 是参数 ✓

struct box<t> { value: t } // ✗ error: type parameter `t` must start with an uppercase letter
```

只靠约定的话，`struct T` + `struct box<T>` 是**能编译过**的 —— 里面的 `T` 会遮蔽外面的。
**能编译但读者看不懂，就是坏设计。** 强制大小写之后，两者**集合不相交 ⇒ 语法上不可能同名**。

**这就是 v0 丑的地方：不是骨架丑，是名字丑。** 看第 3 节的对照表。

---

## 2. 样板：同一段五子棋，按主人的规矩写

```extc
struct board {
    cell: [15][15]i32
    moves: i32
}

fn place(self: ref board, x: i32, y: i32, color: i32) -> result<(), moveError> {
    if self.cell[y][x] != EMPTY {
        return Err(moveError::Occupied)
    }
    self.cell[y][x] = color
    self.moves += 1
    return Ok(())
}

fn longestRun(b: ref board, x: i32, y: i32, dx: i32, dy: i32, color: i32) -> i32 {
    var n: i32 = 0
    for var i: i32 = 0; i < 5; i += 1 {
        let px: i32 = x + i * dx
        let py: i32 = y + i * dy
        if px < 0 || px >= 15 || py < 0 || py >= 15 { break }   // 这行检查让下面的下标被证明合法
        if b.cell[py][px] != color { break }
        n += 1
    }
    return n
}

fn main() -> i32 {
    var b: board = {}                    // 清零；b 属于 main 的 arena
    b.cell[7][7] = BLACK
    println(longestRun(b, 7, 7, 1, 0, BLACK))
    return 0                             // 全程序没有一个 malloc / free / static
}
```

### 方法语法：两种写法，主人挑

```extc
// 形式 1（v0 的写法）：首参数是 self
fn place(self: ref board, x: i32, y: i32, color: i32) -> result<(), moveError>

// 形式 2（Go 的 receiver 味）
fn (self: ref board) place(x: i32, y: i32, color: i32) -> result<(), moveError>
```

两者调用点都是 `b.place(7, 7, BLACK)`，**零差别** —— 纯粹看哪个读起来顺。

---

## 3. v0 的名字 → 新名字（主人要的那张「砍」表）

| v0 | 现在 | 为什么 |
|---|---|---|
| `VarArray<T>` | `array<T>` | `Var` 是废话（数组本来就是变长的），而且跟 `var` 关键字撞车 |
| `wString` | `Utf32String` | `W` 来自 Win32 的 `wchar_t`，是 C 的历史包袱，不是 extC 的概念 |
| `hashMap<K, V>` | `map<K, V>` | Hash 是实现细节，不该进名字（Go / TS 都叫 map） |
| `is_empty()` | `isEmpty()` | camelCase |
| `to_bytes()` | `toBytes()` | camelCase |
| `checked_to_int()` | `toIntChecked()` | camelCase，且意图在前 |
| `slice<T>` | `slice<T>` | 类型也是 camelCase |
| `substr()` | `view()` | 零分配 —— 由 P 推出（能证明的拷贝就不该发生） |
| `string<T>` | 保留 | 主人要 Unicode，泛型留着 |
| `box<T>` | **待定** | 见 §6 |
| `string<T> { data: box<[T]> }` | `data: ref T` | 原写法是 `box<slice>` = 双重间接 + 缺 `cap` |

---

## 4. `static` 到底是什么（主人说不熟，奶昔讲一下）

C 的 `static` 一个关键字有三层意思，被骂的只是中间那层：

| 层 | 含义 | 危险度 |
|---|---|---|
| 文件作用域链接 | 「这个符号只在本文件可见」 | 无害，就是个 private |
| **函数内静态变量** | **这个局部变量活得比函数调用还久** | ★★★ 真坑：签名上看不出来的隐藏状态 |
| 静态存储期 / 零初始化 | 不占栈、开局清零 | 无害 |

**extC 的答案是推导出来的，不需要新规则：**

> **全局变量 = 深度 0 的 arena，永不结束。**

于是三件事同时成立：

1. **`static` 关键字消失** —— 你在最外层写 `var board: board` 就是全局，没有任何关键字要记。
2. **引用规则自动兜住危险用法** —— 全局（深度 0）想存一个指向局部（深度 ≥ 1）的 `ref`，`0 ≥ 1` 为假 → **直接编译错误**。没为全局写一行特殊规则。
3. **真正该死的那个 `static` 在语言里说不出来** —— 「局部但活得比局部久」在 arena 模型里无法表达：局部的作用域就是它的 arena，作用域结束它就死。**这比「禁止」更强 —— 是语法上不可达。**

主人当年觉得 `static` 「sb」，直觉对，但骂的对象其实是第 2 层那个隐藏状态。

---

## 5. Unicode 的答案顺手解决了 C 互操作的矛盾

主人说：「**同样的封装 + 针对性优化实现，我知道 C 的 mem/str 系列函数很快（虽然危险）**」。

这正是：**泛型靠代码生成，且允许按类型特化实现。**

| 类型 | `find` 的生成实现 |
|---|---|
| `string<u8>` | 调 `memchr` —— 就是 C 里最快的那种 |
| `string<u32>` | 按机器字比较的手写循环 |

接口一样，实现各自最优。**但这就把 v0 §6 的「不做 C 互操作」判死了** —— 要 `memchr` 级别的速度，就必须能碰 libc。

### 定案

> **标准库实现可以调用 libc（由编译器生成），用户代码不能。**
> 用户写不出 `extern`，但 `string<u8>.find` 内部就是 `memchr`。

**P 没被违反**：用户手上的逃生舱依然只有一个；编译器自己生成 `memchr` 不是逃生舱，是后端细节。

这一条同时给 v0 三处矛盾（§6 不做 / §8 往后放 / §10 当卖点）画了句号：**编译期悄悄用，用户用不到。**

---

## 6. 面还没定的三件事

1. **`box<T>` 还需要吗？** arena 已经拥有所有堆对象，`box` 只买到「比 arena 更早释放」。如果砍掉，v0 §4 的「五种指针替代品」直接降到三件（`ref T` / `option<T>` / 索引）。
2. **方法语法**：形式 1 还是形式 2（§2）？
3. **类型标注能不能省**：`var n = 0`（推导）还是必须 `var n: i32 = 0`（写全）？
