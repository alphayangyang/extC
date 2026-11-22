# 语法糖手册

> 状态：**手册** —— 同一个意思，更短的写法都有哪些。

这一页只回答三件事：**短在哪、展开成什么、什么时候会踩到**。语言规则本身写在[语言现状](../extc-docs/reference/language.md)，
写法收益的量尺写在[简洁度](../extc-docs/plans/CONCISENESS.md)。

读法：每条糖一行，四列 —— 写法、展开形状（编译器把它重写成什么）、求值次数与边界、用例。
用例在 `tests/sugar/` 下，文件里的 `// expect:` 行是判据；跑全部用例：`./tests/sugar/run.sh`。

双向门禁：`python3 tools/check_sugar_manual.py --gate`（`./check.sh docs` 的一部分）要求手册与用例
互相点名 —— 表里写了糖却没有用例在跑它，或者新加了用例却没进表，任一侧都红。

## 声明

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `var a, b: i32 = 1, 2` | 两条单名 `var` 声明 | 标注写一次、所有名字共享；末尾的标注只填给前面没写的名字 | `tests/sugar/decl_multi.extc` |
| `var a, b = 1, 2.5` | 两条单名声明，各自的类型从自己的值推断 | 每个名字独立推断：`a` 是 `i32`、`b` 是 `f64` | `tests/sugar/decl_infer.extc` |
| `var a: i32, b: i64 = 1, 2` | 逐名标注的两条声明 | 混写时名字自带的标注优先，末尾标注只填没写的 | `tests/sugar/decl_pername.extc` |
| `global var g1, g2: i64 = 10, 20` | 顶层同样是两条声明 | 顶层初始化式必须是常量；不带值的零初始化就是静态零 | `tests/sugar/decl_global.extc` |

## 赋值

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `a, b = b, a` | 右边逐项求值进临时量，再逐个写回左边 | 交换不需要第三个变量；多目标只接受 `=` | `tests/sugar/assign_swap.extc` |
| `p.x, arr[0], *q, o! = …` | 逐个目标按 place 写回 | 字段、下标、解引用、载荷四类目标同一套规则，可以混用 | `tests/sugar/assign_places.extc` |
| `p.x, p.y = p.y, p.x` | 同上，目标是同一结构体的两个字段 | 结构性交换读的是旧值 | `tests/sugar/assign_fields.extc` |
| `a, b = f(), g()` | 先全部求值，再全部写回 | 两个右值都看得见旧值；求值顺序从左到右 | `tests/sugar/assign_order.extc` |

## 控制流

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `match c { red => a, b = b, a … }` | 臂体就是一条语句 | 不带花括号的臂体只能放一条语句，多条要写块 | `tests/sugar/sugar_arm.extc` |
| `for (var i, j: i64 = 0, 0; i < 3; i, j = i + 1, j + 10)` | `var` 加 `while` | init 里可以写多名字声明或并行赋值；step 的更新读同一轮旧值 | `tests/sugar/sugar_for.extc` |
| `if let PAT = e { … } else { … }` | `match e { PAT => { … } _ => { … } }` | `e` 求值一次；`else` 里可以继续嵌套 `if let` | `tests/sugar/sugar_try_arms.extc` |
| `while let PAT = e { … }` | `while true { match e { PAT => { … } _ => { break } } }` | 每轮重新求值 `e` | `tests/sugar/sugar_try_scrutinee.extc` |

## 失败与兜底

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `let v = e?` | 语句里的每个 `e?` 提前成一条同形的局部绑定，插在这条语句之前 | 求值一次，空就立刻返回；`&&` / `\|\|` 右侧、`??` 右侧、`while` 条件、lambda 体里不行 —— 那些位置可能根本不求值 | `tests/sugar/sugar_try_arms.extc` |
| `match` / `if let` / `while let` 的臂体与 scrutinee 里的 `?` | 同样提前成语句前的局部绑定 | 臂体被选中后顺序跑一次，scrutinee 在任何臂之前求值一次；`?` 剥掉外层 `option` 后，payload 本身得是枚举才能 `match` | `tests/sugar/sugar_try_scrutinee.extc` |
| `e ?? 兜底` | 三元：有值取左、没值取右 | 主体求值一次（主体是调用时先落进临时量）；右结合；右边可以写 `break` / `continue` | `tests/sugar/sugar_try_arms.extc` |

## 字面量与输出

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `v * 2`（`v` 是自定义类型） | 运算符按右操作数找重载 | 字面量只是「能放进哪些宽度」，不自己定类型；默认宽度 `i32` 上有同名重载时优先 `i32` | `tests/sugar/sugar_op_literal.extc` |
| `seq::count(ys, 5)` | 调用，字面量实参按形参类型适配 | 字面量实参不替泛型类型参数作主，`T` 由别的实参或写出的类型实参定 | `tests/sugar/sugar_generic_literal.extc` |
| `var c: box<i64> = box { v: 2 }` | 泛型结构体字面量 | 实例由期望类型补全；裸字面量字段按期望类型适配 | `tests/sugar/sugar_structlit_generic.extc` |
| `io::cout << u8v << f32v` | `<< i64(…)` / `<< f64(…)` | 位宽比 `i64` 窄的整数与 `f32` 都无损加宽过去 | `tests/sugar/sugar_print_narrow.extc` |
| `io::cout << u64v` | `<< u64` 自己的十进制路径 | 加宽到 `i64` 会把上界变成负数 | `tests/sugar/sugar_print_u64.extc` |
| `for i in 0..n`（`n: i64`） | `var` 加 `while` | `i` 取两端的较宽者；上界只求值一次；上界装不下下界时宽度不动 | `tests/sugar/sugar_range_type.extc` |

## 预算内的长链

| 写法 | 展开成什么 | 求值次数与边界 | 用例 |
| --- | --- | --- | --- |
| `1 + 1 + … + 1`（1500 个操作数） | 左结合嵌套加法 | 深度预算之内必须编过；求值次数等于操作数个数 | `tests/sugar/sugar_long_chain.extc` |
| 150 段 `else if` | 嵌套 `if` | 深度预算之内照常跑 | `tests/sugar/sugar_elseif_chain.extc` |

## 不在这张表里的

这一页只收「同一个意思的第二种更短写法」。`?` / `??` 的完整规则、`for` 的几种形态、
`f"…"` 格式化串、`match` 的穷尽性，属于语言本身的规则，写在[语言现状](../extc-docs/reference/language.md)。

## 加一条糖

1. 在 `tests/sugar/` 写用例：文件名以 `decl_` / `assign_` / `sugar_` 开头（`./tests/sugar/run.sh`
   按这三个前缀跑），文件里写 `// expect:` 行作为判据。
2. 在对应的表里加一行，四格都填：写法、展开形状、求值次数与边界、用例。
3. `./check.sh docs` 会跑双向门禁 —— 任一侧漏了都红。
