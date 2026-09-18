# PLAN.md —— week-1：类型层地基

> **决策依据（主人的直觉 + 奶昔的细化）**：
>
> > **能随时加的是「语法」（数组、`for`、`match`、模块）；改起来贵的是「类型的形状」（泛型、`ref`、`Result`/`Option`、值语义）。**
>
> 数组和 `for` 加错了改一星期；泛型和 `ref` 的语义长歪了，**所有基于它的代码全得重写**。
> 所以 week-1 **一行语法糖都不碰**，全花在类型层。

---

## 任务

### T1 · 把类型信息从代码生成里拔出来（架构动作，最要紧）✅ 已完成

现在 `typeOf` 寄生在 `codegen.c` 里 —— 这是**架构错误**：类型检查必须**独立于**代码生成，否则每加一个特性，两边都要改一遍，而且会互相打架。

- 新建 `src/types.[ch]`：类型表（内建类型、struct 表、函数签名表）
- 新建 `src/check.[ch]`：一遍**独立的类型检查 pass**，给每个表达式定类型并做全部检查
- `codegen.c` 删掉 `typeOf`，改成读检查结果

**验收**：codegen 里不出现任何类型推导逻辑。✅

### T2 · 补全类型检查规则 + 好错误信息 ✅ 已完成

- 赋值 / 传参 / 返回 / 二元运算 / 字段 / 方法调用 的类型一致性
- 拓宽允许、**收窄禁止**、**无隐式转换**
- 错误信息统一 `expected X, found Y` + note

**验收**：`let x: i32 = "abc"` 由 extC **自己**报错。✅
（附带：字面量按值适配、无隐式真值转换、无公共类型报错、重名检查、`self` 形态检查）

### T3 · `ref` 升级成表达式 + 调用点显式（定案 10）✅ 已完成

- `ref` 从「类型修饰」升级成「**表达式**」：`ref s.board` 产生一个引用值
- 自由函数的 `ref T` 实参**必须在调用点写 `ref`**
- **方法接收者自动取地址**（`.` 表示「在这个值上操作」）
- `ref` 是**可变**引用 ⇒ 不能对 `let` 取引用
- 类型层为**逃逸检查留位**（现在不做检查，但 `Type` 已经能挂 arena 深度）

### 顺手三条 ✅ 已完成

| 定案 | 状态 |
|---|---|
| 8 · 默认零初始化（`var b: Board` 合法） | ✅ `ref T` 除外（没有零值） |
| 9 · 方法写在 struct 体内 | ✅ 自由函数带 `self` 现在报错 |
| 14 · `type` 枚举 | ✅ 含定案 11 的「枚举自动有名字文本」 |

### T4 · 泛型 + `Slice<T>` / `Array<T>`

- 泛型**靠 C 代码生成实现**（不建实例化图 —— 这是主人自己的直觉，比真单态化便宜）
- 第一条真实用例：`Slice<T>`
- 顺带执行定案 3：**`str` 作废，字符串字面量 = `Slice<u8>`**

### T5 · `Option<T>` / `Result<T,E>` + `?`

- 第一个「有真实语义」的泛型类型
- `?` 是**可见的、静态解析的、无栈展开的**转发 ⇒ 不违反 P

### 顺手：三条已定案但没实现

---

## 验收程序

week-1 结束时，下面这段必须**能编译、能跑、且类型错误能被 extC 自己抓到**：

```extc
type Status = | ok | warn | error

struct Counter {
    n: i32

    fn bump(self: ref Counter, by: i32) -> Result<i32, CounterError> {
        if by < 0 {
            return Err(CounterError.negative)
        }
        self.n += by
        return Ok(self.n)
    }
}

fn describe(s: Status) -> Slice<u8> {
    if s == Status.warn {
        return "careful"
    }
    return "fine"
}

fn tryBump(c: ref Counter, times: i32) -> Result<i32, CounterError> {
    var i = 0
    while i < times {
        c.bump(2)?          // 方法接收者自动取地址
        i += 1
    }
    return Ok(c.n)
}

fn main() -> i32 {
    var c: Counter = {}                  // 零初始化
    println("start: {}", c.n)
    let total = tryBump(ref c, 3)?       // 自由函数必须显式写 ref
    println("total = {}", total)
    println("{}", describe(Status.warn))
    return 0
}
```

它一次练到：`type` 枚举、方法进 struct、`ref` 表达式、零初始化、泛型（`Result`/`Slice`）、`?`、格式串。

---

## 不碰（week-2 以后）

数组、`for`（四种形态）、`match`、模块系统、`region` / 逃逸检查、`@recursive`。

> 这些**全是语法或局部机制**，加的时候不会推翻类型层。

---

## 做法：降级版五子棋当靶子

不再「先造语言再写程序」，而是：

1. 把主人的五子棋示例**裁成一个降级版**（切掉当前语言编不了的部分），放进 `examples/gomoku/`
2. **每完成一块语言特性，就把降级版往前推一格**（把被切掉的那段加回来）
3. 每一周结束都有个**真实程序在跑**，而不是只有一堆单元测试

这样「语言够不够用」永远由真实程序回答，不是由设计文档回答。
