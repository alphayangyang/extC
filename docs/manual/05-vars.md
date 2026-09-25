<!-- 本页原来是 docs/MANUAL.md 的「4. 变量与声明」一节（原 §4.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 3. 类型](04-types.md) · [5. 表达式与运算符 →](06-expr.md)

---

# 4. 变量与声明

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

> **这条规则是「浅」的：它管名字，不管数据。**
> 把 `let` 视图拷给一个 `var`、或者传进函数，那边照样能写同一块内存；
> 调用一个 `self: ref T` 的方法也算（`let p; p.moveBy(1)` 现在是允许的）。
> 要管到数据层，可变性就得进**类型**（Rust 的 `&` / `&mut`）——
> 那是 week-4 引用规则的范围，见 [`DECISIONS.md`](../docs/DECISIONS.md) 定案 28。

```extc
var b: board       // 所有字段清零
var n: i32         // 0
var ok: bool       // false
```

### 作用域

块 `{ }` 引入新作用域，内层**遮蔽**外层。

**同一层里 `let` 可以遮蔽 `let`** —— 因为 **`let` 是命名，不是存储**：

```extc
let a: i32 = 1
let a: i32 = a + 1        // 新的名字指向新的值；右边那个 `a` 是上一个
let a: i32 = a * 10
```

- 老的那个绑定**还在**，只是没名字了：谁指着它，谁还指着它
  ```extc
  var x: i32 = 7
  var r = ref x
  let x: i32 = 99      // 新名字
  println(r)           // 7   ← 老的那个
  r = 100              // 写穿到老的那个
  ```
- 遮蔽可以**换类型**（`let v: i32 = 1` 之后 `let v: slice<u8> = "hi"`）——
  赋值做不到这件事，正好说明它是新绑定
- 生成的 C 里第二个以后的名字改叫 `a__2`；**extC 源码里的名字一个字都不改**。

**`var` 不行**：`var` 声明的是**存储**，同一层两个同名 `var` 几乎肯定是想写 `a = ...` ⇒ 编译错误
（见 `tests/errors/redeclare_var.extc`）。参数按 `var` 算，所以 `let p = ...` 可以遮蔽参数，
`var p = ...` 不行。

**名字撞上 C 的关键字没关系**：extC 里 `double` / `int` / `bool` 都只是普通名字
（extC 的类型叫 `f64` / `i32` / `bool`），生成的 C 里它们改叫 `double__c`，照样编得过、调得到

### 全局

顶层 `let` / `var` 是全局（深度 0，活得比谁都长）。局部可以遮蔽全局。

---

**[索引](README.md)** · [← 3. 类型](04-types.md) · [5. 表达式与运算符 →](06-expr.md)
