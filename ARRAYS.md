# extC 数组设计（旧文档 × 当前标准的合并版）

> 来源：主人之前跟 AI 探讨的《extC 数组实现》。
> 本文按**现在的标准**逐条合并 —— 保留对的、砍掉被推翻的、补上还没定的。
>
> 现在的标准见 [`DESIGN.md`](DESIGN.md)（一条原则）和 [`DECISIONS.md`](DECISIONS.md)（已定案）。

## 实施状态

| 块 | 状态 | 说明 |
|---|---|---|
| §2 固定数组类型 `[N]T`（多维递归） | ✅ 已实现 | `TypeKind::TY_ARRAY`，`asize` 存长度 |
| §2 统一套 struct + C 名字修饰 | ✅ 已实现 | `array_15_i32` / `array_15_array_15_i32` |
| §2 初始化：零值默认 + 字面量 + `...` | ✅ 已实现 | 字面量严格计数；`...` 只能补尾 |
| §3 索引 `a[i]` + 边界检查 | ✅ 已实现 | 越界 trap 并报 extC 位置 |
| 数组 `==`（编译器生成） | ✅ 已实现 | `typeSupportsEq` 递归；`println` 调试打印 |
| §4 切片视图 `a[lo..hi]` | ✅ 已实现 | 四种写法 + 编译期证明 + 运行时 trap（见下） |
| §5 动态数组 | ✅ **已落地，但名字是 `varArray<T>`** | 在 `stdlib/prelude.extc` 里**用 extC 写**（不是编译器内建）—— `push`/`pop`/`get`/`set`/`size`/`asSlice`… ✓ ⚠️ 原文等的"arena"早就有了，而且**没有走"编译器内建动态数组"这条路**（能写在 prelude 里就写在 prelude 里 ✓）|

**验证**：`examples/arrays.extc`、`examples/slices.extc`、`examples/gomoku-board.extc`（G2 里程碑）；
`tests/run.sh` **251 通过**（2026-09-23 实测；原文写 60，那是很早期的数 ✗）。

---

## 0. 旧文档里**保留**的部分

### ① 最值钱的洞察：套 struct 换值语义

```c
int a[15];
int b[15];
b = a;          // ✗ C 里数组不能赋值
return a;       // ✗ 不能返回数组
```

```c
typedef struct { int32_t data[15]; } array_15_i32;
array_15_i32 a, b;
b = a;          // ✓ struct 可以赋值
return a;       // ✓ struct 可以返回
```

> **struct 包装让数组变成值类型。** 这就是 C++ 的 `std::array`。

**这条完全保留** —— 而且它跟 extC 的核心（默认值语义）是同一个方向。

### ② 四种形态的划分

固定数组 / 动态数组 / 切片 / 索引视图 —— 这个分类是对的，保留（但重新分组，见 §1）。

### ③ 多维切片只能在一行内切

`b[2][3..8]` ✓ 但 `b[2..5][3..8]` ✗（不连续）。**保留**，理由充分。

### ④ 逃逸检查覆盖数组

数组和普通变量一样受引用规则管。**保留**（等 week-4 落地）。

---

## 1. 重新分组：按「要不要分配」分

旧文档按「固定/动态/切片/视图」分。**现在要按「需不需要堆分配」分**，因为
**~~extC 现在还没有 arena~~**（那是 week-4）—— ✅ **早就有了**（2026-09-23 复核）：
**按块细化**（定案 55）+ `new` 永远是零（定案 56）+ 块级**提升**（定案 63）；
判据 `tests/arena/`（150MB 上限下循环 300×1MB 不涨）✓
⇒ 下面这张表的分类**照样成立**（"需不需要堆分配"仍然是那个判据），
只是"现在还没有"这句要改成"**有，而且按词法块细化**"✓

| 形态 | 例子 | 需要分配？ | 能做的时机 |
|---|---|---|---|
| **固定数组** | `[15][15]i32` | ❌ 栈 / 宿主 | ✅ **现在就能做** |
| **数组字面量** | `[1, 2, 3]` | ❌ 编译期 | ✅ **现在** |
| **切片视图** | `a[2..5]` → `slice<i32>` | ❌ 只是指针+长度 | ✅ **现在**（需先有数组）|
| **索引视图** | `arr[i..j]` | 同上 | ✅ 现在 |
| **slice 的索引** | `s[i]` | ❌ | ✅ **已完成**（T5a-1）|
| **动态数组** `array<T>` | 运行时长度 | ✅ **要 arena** | ❌ **等 week-4** |

> **这就是旧文档最大的改动**：它把 `VarArray<T>` 写成 `malloc` + `free` + `Drop`，
> 而那正是后来被 arena 模型取代的东西（见 §5）。

---

## 2. 固定数组

### 类型语法

```extc
let a: [15]i32            // 一维
let b: [15][15]i32        // 二维
let c: [3][3][3]f64       // 三维
```

**`[N]T`，从外到内**（跟 v0 和旧文档一致）✓

### 编译到 C —— 统一套 struct

旧文档说：独立变量套 struct，**struct 成员用裸数组**（省一层）。**这一条要改。**

```c
// 旧文档（成员用裸数组）
typedef struct { int32_t cell[15][15]; } board;
board b;
b.cell[y][x]                     // 访问是 .cell[y][x]

// 合并版（统一套 struct —— 成员也套）
typedef struct { array_15_array_15_i32 cell; } board;
typedef struct { int32_t data[15]; } array_15_i32;
typedef struct { array_15_i32 data[15]; } array_15_array_15_i32;
board b;
b.cell.data[y][x]                // 访问是 .cell.data[y][x]
```

**为什么改？** 旧文档的理由是「成员位置已知，不需要赋值/返回，直接用裸数组更干净」——
但那是为了让**一种情况**更干净，而**引入了一条特例**。麻烦在于：

> **索引 `a[i]` 该生成 `a.data[i]` 还是 `a[i]`？**
> 光看类型分不出来 —— 得知道这个值是「独立变量」还是「struct 成员」。
> 那就是把**上下文**塞进了代码生成，而我们的规矩是 codegen 不做推理。

统一包装之后：**`a[i]` 永远生成 `a.data[i]`**，一条规则，零特例 ✓
（多出来的那一层在 C 里被完全优化掉 —— 布局一模一样。）

代价只是字面上多一个 `.data`。**用 `语法一致，不要有特例` 换掉了一个 `.data`，划算。**

### C 名字修饰

```
[N]T          → array_N_T            （递归，跟泛型实例同一套规矩）
[15][15]i32   → array_15_array_15_i32
```

旧文档写的是 `Array_15_15_int`（拍平）。**改成递归**，理由：
① 跟泛型实例的修饰方式统一（`pair_slice_u8_counter` 也是递归的）
② 类型上 `[15][15]i32` 本来就是「15 个 `[15]i32`」，递归才是它的真实结构

### 初始化

```extc
let a: [3]i32 = [1, 2, 3]

let b: [2][3]i32 = [
    [1, 2, 3],
    [4, 5, 6]
]

var zero: [3][4]i32        // 零初始化 —— 已经是默认行为，不需要 `[0; 3]` 语法
```

编译到 C：

```c
array_3_i32     a    = (array_3_i32){ .data = { 1, 2, 3 } };
array_2_array_3_i32 b = (array_2_array_3_i32){ .data = {
    (array_3_i32){ .data = { 1, 2, 3 } },
    (array_3_i32){ .data = { 4, 5, 6 } }
} };
array_3_array_4_i32 zero = {0};      // 零初始化：C 的 {0} ✓（已有机制）
```

> 旧文档的 `[0; 3][0; 4]`（全零语法）**砍掉** —— extC 已经有「省略初始化式即零初始化」
> （定案 8），再给数组一套专门语法是多余的特例。

### 元素类型与长度

- 长度必须是**编译期常量**（字面量；将来支持 `const` 之后可以是常量表达式）
- 所有元素必须**同一类型**（或能拓宽到同一个 —— 跟数组字面量的推导规则一致）

---

## 3. 索引与边界检查

### 语法

```extc
let x = a[i]
let y = b[i][j]          // 就是 (b[i])[j]
```

### 编译到 C

```c
int32_t x = a.data[i];
int32_t y = b.data[i].data[j];
```

### 边界检查

旧文档写的是：

```c
int x = (i < 15) ? a.data[i] : panic("index out of bounds");
```

**方向对，但有两个问题**：① `i` 被求值了两次（`i` 是函数调用就错了）② 还没定「可证明就不用检查」。

合并版：

```c
/* 生成的 C 里带一个运行时支撑函数（只出一次）*/
int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line);

/* 用逗号表达式让 i 只求值一次 */
int32_t x = a.data[extc_checkedIndex(i, 15, "foo.extc", 12)];
```

`extc_checkedIndex` 越界时打印**带 extC 位置**的 trap 并退出：

```
foo.extc:12:9: trap: index 7 out of range for `[15]i32`
  note: the compiler could not prove this bound, so the check stayed at run time
```

**可证明时零开销**（DESIGN §4）：将来有了**范围类型**（`for i in 0..a.len` 里的 `i`
带着「已证明在范围内」的信息），这一层检查会被消掉。**在那之前一律检查** ——
安全优先，而且这是 P 说的「能证明的不留痕迹」，不是「不能证明的不许写」。

### 越界要优雅处理时

等 `option<T>` 到位（T5）再给 `a.at(i) -> option<ref T>`。
**现在 `a[i]` 越界就是 trap** —— 不给你一个错答案。

---

## 4. 切片视图

```extc
let a: [15]i32
let s1 = a[2..5]        // slice<i32>，长度 3
let s2 = a[2..]         // 到末尾
let s3 = a[..5]         // 从开头
let s4 = a[..]          // 整个数组
```

编译到 C（**两个界都是字面量时**，见下「编译期证明」）：

```c
slice_i32 s1 = (slice_i32){ .data = &a.data[2], .len = 3 };
```

**`slice<T>` 早就存在**（在 `stdlib/prelude.extc` 里，而且**字符串就是它**）。
这里补的是**从数组切出切片**的语法 `a[lo..hi]`。✅ 已实现。

### 省略的界 = 编译器补的字面量

`a[2..]` / `a[..5]` / `a[..]` 里省略的那一端，**由 check 阶段补成字面量**
（`a[2..]` → `a[2..15]`）。这样 codegen 只要看到「两个界都是字面量」
就知道范围可证明、不必生成检查 —— **而不是在 codegen 里再判一次「有没有省略」**。

### 编译期证明：能证明的，运行时不留痕迹（P）

| 情况 | 生成什么 |
|---|---|
| 底是固定数组，两个界都是字面量 | `&a.data[2]` / `.len = 3` —— **零检查** |
| 界里有变量 | helper 函数 + `extc_checkedRange`，越界 trap 带 extC 位置 |
| 界是字面量但越界 | **编译期报错**（`slice end 9 is not inside [5]i32 (length 5)`） |

第三条包含「单独一个界越界」（`a[i..20]` 对 `[15]i32`）和负数界（`a[0..-1]`）——
**这些跟另一个界是什么无关，所以永远错**，一律当场报。

### 底必须是「地方」（place）

切固定数组要**取元素的地址**，所以底只能是变量 / 字段 / 索引链：

```extc
let v = make()[1..3]     // ✗ error: cannot slice a temporary value
```

理由不只是「不好看」：那会切到函数返回值的临时存储上，是**指向已死对象**的视图。
切 **slice** 不受这条限制 —— 那只是对「指针+长度」做指针算术，
所以 `"abcdef"[1..3]` 合法（字节有静态生命期）。

完整的逃逸检查（`fn bad() -> slice<i32> { let a: [15]i32; return a[..] }`）**仍然等 week-4**。

### 多维只切最后一段

```extc
let b: [15][15]i32
let row = b[2]           // 类型是 [15]i32 —— 不是切片，就是那一行
let sub = b[2][3..8]     // slice<i32>
```

`b[2..5][3..8]` **不是二维切片**：它是「先切出 2..5 这几行的视图（类型 `slice<[15]i32>`），
再切那个视图」。所以它要么因 `8 > 5-2` 在运行时 trap，要么写出来根本用不了
（拿到的是「几行」而不是「一个格子」，类型对不上一眼就看得出来）。
**不连续的东西没法用一个指针+长度表示** —— 这条判断保留。

### 视图元素是 **lvalue**（重要）

`s[i]` 编译成 `(*s_index(v, i, file, line))` —— 索引原语**返回指针**，调用点解引用。

不这样做会有两个真 bug：

| 场景 | 按值返回索引时 | 返回指针后 |
|---|---|---|
| prelude 的 `slice<T>::==` 写 `self[i] != other[i]` | `fn ==(self: ref T, ...)` 收 `ref`，`&(临时值)` 在 C 里非法 → **`slice<struct>` 完全编不出来** | ✓ |
| `s[i] = x` / `s[i].field = x` | C 报 `lvalue required as left operand of assignment` | ✓ |

**所以视图是可写的**（不是「只读视图」）。这不是放松：`slice<T>` 里的 `data` 本来就是
`ref T`，而 extC 的规矩是**引用必须在类型里看得见**（P′）—— 看得到就是显式的。

⚠️ 一个已知代价：`slice<u8>` 可以来自字符串字面量，而字面量在 C 里是只读段，
**往 `"abc"[0]` 写会崩**。extC 没有 `const`，所以这条暂时靠「别那么写」+ 文档。
真要做是在类型里区分「可变引用」，那是 week-4 引用规则的一部分。

---

## 5. 动态数组：`array<T>` 推迟到 arena 到位

旧文档的 `VarArray<T>`：

```
struct VarArray<T> {
    data: Box<[T]>
    len: size_t
    cap: size_t
}
impl Drop for VarArray<T> { fn drop(self) { free(self.data) } }
```

**这一整块跟现在的标准冲突**，逐条说：

| 旧文档 | 现在的标准 | 处置 |
|---|---|---|
| `impl Drop for` | **没有 trait、没有 impl 块** | 删 |
| `free(self.data)` | **用户永不写 free**（P + G1） | 删 |
| `__attribute__((cleanup(...)))` | 那是 RAII；extC 的释放由 **arena** 触发 | 删 |
| `Box<[T]>` | `Box` 已判为**冗余**（arena 拥有所有堆对象） | 改成 `ref T` |
| `data: null` | **`ref` 不可为空** | 要解决（见下） |
| `size_t` | 命名规范：用 `i64` | 改成 `i64` |
| `VarArray<T>` | `Var` 是废话（数组本来就变长） | 改成 `array<T>` |

### 合并后的形状

```extc
// stdlib/prelude.extc —— 用 extC 写（等 arena 到位）
struct array<T> {
    data: ref T
    len:  i64
    cap:  i64

    fn push(self: ref array<T>, v: T) { ... }   // 增长时**从 arena 拿新块**，旧块留给 arena
    fn get(self: ref array<T>, i: i64) -> option<ref T> { ... }
    fn isEmpty(self: ref array<T>) -> bool { return self.len == 0 }
}
```

**没有 `drop`** —— 缓冲区属于 arena，arena 结束一起释放。
**增长会留下垃圾**（旧块还不掉）—— 这是 arena 模型的真实代价，`src/base.h` 里已经记过一次。

### ⚠️ 两个必须先解决的问题

1. **空数组的 `data` 指向哪儿？** `ref` 不可为空，而长度 0 的数组没有合法的 data。
   （这跟「空 `slice` 造不出来」是同一个建模问题 —— 现在被 `""` 绕过去了，
   但数组没有这种天然哨兵。候选：arena 对 0 字节也返回一个合法指针 / 每个 T 一个静态哨兵。）
2. **怎么创建？** `array<i32>::new()` 需要 **`::`** 与关联函数；或者
   `fn newArray<T>() -> array<T>` 需要**泛型自由函数**。**两个都还没有。**

→ **所以动态数组放到 arena 到位之后**（week-4），跟这两个问题一起解决。

---

## 6. 砍掉的东西（汇总）

| 旧文档有 | 为什么砍 |
|---|---|
| `impl Drop for` / `Drop` 协议 | 没有 trait、没有 impl 块（`==` 也只靠方法名） |
| `free()` / `malloc()` | 用户永不写；分配走 arena（G1） |
| `__attribute__((cleanup))` | RAII；extC 的释放由 arena 触发 |
| `Box<[T]>` | `Box` 冗余（arena 拥有所有堆对象） |
| `data: null` | `ref` 不可为空 |
| `[0; 3][0; 4]` 全零语法 | 零初始化已经是默认（定案 8），不需要专门语法 |
| `size_t` | 命名规范用 `i64` |
| `VarArray<T>` | 改成 `array<T>`（camelCase；`Var` 是废话） |
| `panic(...)` | 改成 **trap**（带 extC 位置），且**不双求值** |

## 7. 还没定的

| 问题 | 影响 |
|---|---|
| **范围类型**（让索引可证明 ⇒ 零检查） | DESIGN §4 的核心想法；决定 `for i in 0..n` 的形状 |
| `for` 的四种形态 | 数组要遍历，绕不过去 |
| 数组字面量的类型推导细节（能否从上下文来） | 跟结构体字面量同一套规矩 |
| 多维数组作为函数参数的值拷贝成本 | `[15][15]i32` 是 900 字节，值传会拷贝 |
| `array<T>` 的名字与创建方式（见 §5） | 等 arena |
