# MODULES.md —— 模块系统该怎么做（**摆方案 + 摆证据，不替主人拍板**）

> **起因**（主人 2026-09-22）：
>
> > 「不赖，想做 IO 了。但是根据所有的已有 C 类语言（sb Python 除外），IO 库基本都是独立库。
> >  你有什么头绪吗。说白了，**在 IO 之前，应该推进 modules 了**。
> >  但是在此之前我想知道不同的语言是怎么做这个功能的。这个功能不难但是很重要」
>
> 本文三件事：① 为什么这事**必须**排在 IO 前面（论证，不是重复主人的话）
> ② **横向调研**：12 门语言各自怎么做、拿什么换什么 ③ 落到 extC 上的**候选方案 + 待拍板清单**
>
> ⚠️ 已拍板的标 ✅，未决的标 ⬜。**本文一行代码都没写**（跟 `IO.md` 一样的规矩）。

---

## 0. 一句话

> **IO 是库，库需要模块；模块是语言的事，IO 只是它第一个真客户。**

---

## 1. 为什么模块必须排在 IO 前面（四条，逐条给理由）

| # | 理由 | 不给会怎样 |
|---|---|---|
| 1 | **名字**：库要有一片自己的命名空间 | `read` / `write` / `open` / `file` 这些名字**一定会撞**用户的 ✓（现在 prelude 是硬塞进每个程序的，靠 `reserved` 挡重定义 ✗ —— 那只够 prototype，不够一个库）|
| 2 | **分层**：`IO.md` 自己就是三层（原语 / stdlib wrapper / 用户 `extern!`）| 没有模块 ⇒ 三层只能全塞进一个 prelude ✗（那个文件现在已经 313 行，加了 IO 就是 700+，而且**每行都进每个程序的 AST**）|
| 3 | **特权**：原语（`rawRead`/`rawWrite`）只有编译器能给 | 需要一个明确的"**只有系统模块能用特权**"的机制 ⇒ 那就是模块 + 可见性 ✓（否则任何用户代码都能假装自己是原语 ✗）|
| 4 | **缓存**：工具链自己也要读源文件、写生成的 C | 没有模块 ⇒ 编译器每次都得把整个 stdlib 重新解析一遍（**编译时长**这一维度直接买单 ✗ —— 而这正是 `bench/compile` 在量的东西）|

⚠️ 顺带一条**主人的观察需要补一句**：C 类语言的 IO 之所以是库，还因为**它们都有 C 互操作**
（Rust 的 `libc` / Zig 的 `@cImport` / Go 的 syscall 包）✗ —— extC **还没有**（`extern!` 只在
`LANGUAGE.md` §8.1 的"档 3"里记着）⇒ 所以 extC 的 IO 需要**两条腿**：
① 模块（本文）② 特权边界（原语 or `extern!`）✓

---

## 2. 横向调研：12 门语言怎么做（重点看"拿什么换什么"）

| 语言 | 模块单位 | 怎么声明 | 可见性 | 环 | 分别编译 |
|---|---|---|---|---|---|
| **C** | 文件（**文本包含**）| `#include "x.h"` | 靠**链接性**（`static` / `extern`）| ✅ 允许 | ✅ 真·分别编译（.o + 头文件）|
| **C++20 modules** | 显式 `module` 声明 + 分区 | `export module foo;` / `import foo;` | `export` 逐条标 | ✅ 允许 | ✅ 但是 **BMI 二进制格式各家自定** ✗ |
| **Modula-2**（1980）| **定义模块 + 实现模块**两个文件 | `DEFINITION MODULE Foo;` / `IMPLEMENTATION MODULE Foo;` | **显式导出列表**（`EXPORT QUALIFIED ...`）| ⚠️ 受限 | ✅ 真·分别编译（符号文件）|
| **Oberon**（1988）| 同 Modula-2 的**两文件**模型 | `MODULE Foo;` / 定义模块可自动抽取 | 显式导出（名字后加 `*`）| ⚠️ 受限 | ⭐ **"compile the world"**：整套源码一起编（换来了极强的跨模块优化与无接口漂移）|
| **Ada** | 包（**spec + body**）+ **子单元** `Parent.Child` | `package Foo is ... end Foo;` / `with Foo;` | spec 里声明的就是可见的 | ⚠️ 受限（`with` 图必须无环）| ✅ 强检查的分别编译 |
| **D** | **文件即模块**（显式声明路径）| `module a.b.c;` + `import a.b.c;` | `private`（默认 public）| ✅ 允许 | ✅ |
| **Java** | 包 + **JPMS 模块** | `package a.b;` / `module-info.java` | `public` / `private` / 包内 | ⚠️ 模块图无环 | ✅（.class + jar）|
| **C#** | 命名空间 + 程序集 | `namespace A.B;` / `using A.B;` | `public` / `internal` | ✅ 允许 | ✅ |
| **OCaml** | **编译单元 = 模块**（大写名字）| 文件名 `foo.ml` ⇒ 模块 `Foo` | `.mli` 接口文件（没有就全公开）| ⭐ 有**递归模块** `module rec` | ✅（.cmi 接口文件）|
| **Go** | **目录 = 包** | 目录里 `package foo` | ⭐ **首字母大写即导出**（没有关键字！）| ❌ **禁止** import 环 | ✅（包级编译 + 缓存）|
| **Rust** | **crate = 编译单元**；`mod` 树 | `mod foo;` / `use crate::foo::Bar;` | ⭐ **私有默认**，`pub` / `pub(crate)` | ❌ crate 之间禁止 | ✅（crate 级并行 + 增量）|
| **Zig** | **文件即模块**（`@import` 返回一个**类型**）| `const io = @import("std").io;` | `pub` 标记（其余文件内私有）| ✅ 允许 | ❌ 全程序编译（**没有独立 .o 文化**）|
| **Nim** | 文件即模块 | `import foo` | ⭐ 名字后加 `*` 才导出（**默认私有**）| ✅ 允许 | ✅ |

### 2.1 五条能从这张表里读出来的"规律"

1. **文本包含（C）和语义导入（其余全部）是分水岭** ✗
   C 的 `#include` 是**把文本粘进来** ⇒ 宏会泄漏、包含顺序有语义、编译单元之间靠"重复声明"
   对齐（ODR 问题）。**1979 年的 Modula-2 就已经不这么干了** ⇒ 语义导入是几十年的一致答案 ✓
   ⭐ 而 C++20 的教训是：**语义导入需要"模块接口"这个额外产物**（BMI）⇒
   各家格式不一样、构建系统要扫描依赖 ⇒ 到 2025 年"怎么分发模块化的库"**还没有共识** ✓
   （见 [P2581R2](https://rap.no/JTC1/SC22/WG21/docs/papers/2022/p2581r2.pdf) 与
   [SG15 关于 BMI 的讨论](https://lists.isocpp.org/sg15/2026/01/3009.php)）
2. **可见性有三种流派**：
   - **显式导出列表**（Modula-2 / Oberon / Ada spec）：模块**自己**说"我对外给这些" ✓
   - **逐条标记**（Zig `pub` / Nim `*` / C++ `export`）：写在声明上 ✓
   - **默认私有 + 逐条放开**（Rust）/ **大写即导出**（Go）/ **默认公开**（D/C#）✗
3. **"环"不是语法问题，是"你能不能单独理解一个模块"的问题** ✓
   Go / Rust 直接禁（好实现、缓存友好）；C / Zig / D 允许（灵活，但接口分析要跑到不动点）
4. **分别编译才是模块系统真正的分水岭**（比语法重要十倍）✗
   有它 ⇒ 能缓存、能并行、能分发二进制库；没它 ⇒ 模块就只是"名字空间 + 可见性"
5. **Oberon 的反例值得记住**：它**故意**放弃分别编译（"compile the world"），
   换来的是"整套源码一起优化 + 没有接口漂移 + 没有陈旧对象文件"✓
   ⇒ **这正好是 extC 今天的形状**（单 TU、全程序编译）⇒ 主人可以**先要好处、后付代价** ✓

---

## 3. extC 今天的形状（拿上面那张表当镜子）

| 事实 | 出处 | 对模块设计的约束 |
|---|---|---|
| **只吐一个 .c**，所有非 `main` 函数加 `static` | PLAN #37（换向量化 3.2×）| 真·多 TU 会**破坏那个优化** ✗ ⇒ 要么保持单 TU，要么只对跨模块的符号放开 `static` |
| prelude **嵌在编译器二进制里**（`tools/embed`）| `Makefile` / `src/prelude.c` | stdlib 变成"普通模块"之后，**内建 prelude 这一层要重新定位** ✓（Rust 的 prelude / Haskell 的 `Prelude` 就是那个角色：**默认导入 + 可覆盖**）|
| **没有 C 互操作**（`extern!` 未实现）| `LANGUAGE.md` §8.1 档 3 | 库碰不到系统调用 ⇒ IO 必须**要么**是语言原语（`IO.md` 档 1）**要么**等 `extern!` ✓ |
| `reserved`（prelude 的符号不许重定义）| `src/check_top.c` | 那是"内建模块"的雏形 —— 模块系统只是把它** generalize** ✓ |
| **parser 不查符号表** | DECISIONS #23（`T(x)` 而非 `(T)x` 就是这个原因）| ⭐ 模块路径必须**纯语法可解析** ⇒ `use std.io`（点号路径）比 Rust 2018 那套 `crate::` / `self::` / `super::`（要靠符号表分流）**更适合 extC** ✓ |
| arena / 无 free / 深度 0 = 全局 | `ARENA-FORMAL` | 模块级可变状态 = **深度 0** ⇒ 跨模块全局会被模块系统放大 ✗ |
| **(S3) 记账不变式** | `ARENA-FORMAL` §6.6 | 模块化引入的"跨模块可变状态"**必须先过 (S3) 检查**（"这一步会不会让谁的记数变小"）✓ |
| 没有线程、明确不做开放注册 | `LANGUAGE.md` §7 | ⇒ 初始化顺序问题**可以简化**（没有并发、没有析构、没有开放注册）✓ |

---

## 4. 三个候选方案（摆出来，等主人拍）

### 方案 A · **语义化单 TU**（Oberon / Zig 味）

```extc
// std/io.extc
module std.io                      // 显式声明（文件即模块，声明必须跟路径一致）

export fn readLine(f: mut ref file, buf: mut slice<u8>) -> i64 { ... }   // 导出
fn rawRead(fd: i32, buf: mut slice<u8>) -> i64 { ... }                    // 模块内私有
```

```extc
// main.extc
use std.io                        // 语义导入（不是文本包含 ✗）

fn main() -> i32 {
    var buf: [256]u8
    let n = std.io.readLine(ref f, buf[..])
    ...
}
```

- **编译仍然是"一个 .c"** ⇒ 保住 `static` + 跨模块内联/向量化（PLAN #37 的 3.2× 不丢）✓
- 模块 = **命名空间 + 可见性 + 依赖图**（不做二进制接口）
- 代价：**没有增量编译**（改一行全量重编 ✗）；库只能**以源码分发**（extC 现在本来就是源码即库 ✓）
- 实现量：小（解析 `module`/`use` + 两遍式符号表 + 可见性检查）

### 方案 B · **真·分别编译**（Modula-2 / Ada 味）

```extc
// std/io.extc —— 定义模块（接口）
module std.io
export file, readLine, readAll

// std/io.impl.extc —— 实现模块
module std.io impl
```

- 每个模块编出 `.o` + **接口文件**（自己定格式；里面存导出符号的**签名摘要**，像 Modula-2 的符号文件）✓
- 导出符号用**非 static** 链接 ⇒ 跨模块调用（`-O2` 时靠 LTO 找回内联）
- **必要条件**（不给就是一堆"陈旧接口"事故 ✗）：接口版本/摘要一致性检查 + 构建系统（依赖图 + 缓存）
- 代价：ABI 冻结、构建系统复杂度、PLAN #37 那个优化要重新谈 ✗
- 收益：**增量编译**（改一个模块只重编它 + 依赖者）⇒ `bench/compile` 的编译时长维度直接改善 ✓

### 方案 C · **只做命名空间**（C++ 味）

```extc
namespace std.io { ... }          // 没有可见性、没有依赖图
```
- 最便宜，但**解决不了第 1 节的四条**（库要独立/分层/特权/缓存）✗ ⇒ **不建议**（列出来是为了拍死它）

---

## 5. 它跟 IO 怎么接（`IO.md` 的三档落到模块上）

| `IO.md` 的档 | 放哪 | 可见性 | 谁签字 |
|---|---|---|---|
| **档 1 · 原语**（`rawRead`/`rawWrite`/`rawOpen`/`rawClose`）| **一个特权模块**（名字待定：`sys.io` / `builtin.io`）| **只有它**能声明原语；用户**看不见也改不了** | 编译器（`reserved` 的推广 ✓）|
| **档 2 · stdlib wrapper**（`file` / `reader` / `readLine` / `nextInt` 一族）| `std.io`（**普通 extC 模块**）| `export` 点名 | 库作者 |
| **档 3 · 用户自建** | 用户模块里的 `extern!` 声明 | `extern!` 自带"**必须签字**" | **用户**（P′：不可证必须响亮 ✓）|

⇒ 于是 `IO.md` 的"零新规则"承诺仍然成立：**模块系统不碰借用/逃逸检查一行**，
它只管"谁看得见谁" ✓

---

## 6. ⬜ 待主人拍板（六个问题，按重要性排）

| # | 问题 | 选项 | 我的建议 |
|---|---|---|---|
| **Q1** | **模块单位** | 文件 / 目录 / 显式声明 | **文件 + 显式 `module` 声明**（声明跟路径必须一致 ⇒ 不用查符号表就能定位 ✓）|
| **Q2** | **导入方式** | 文本包含 / 语义导入 | **语义导入**（extC 没有宏，文本包含毫无价值 ✗）|
| **Q3** | **可见性** | 显式导出列表（Modula-2）· 逐条 `export`（Zig）· 默认私有（Rust）| **逐条 `export`** ✓（跟 `mut`/`ref` 一样"写了才给"⇒ 跟 extC 的"显式"一致）|
| **Q4** | **环** | 允许（C/Zig）· 禁止（Go/Rust crate）| **禁止** ✓（缓存友好、错误信息简单；代价：将来要 `mut ref` 回调就用传参解决）|
| **Q5** | **分别编译** | 先 A（单 TU）后 B · 一步到位 B | ⭐ **先 A**：先把"库 = 模块"这件事做成，**保住 PLAN #37 的优化**；B 单独排（它是构建系统的活，不是语言的活）✓ |
| **Q6** | **prelude 的地位** | 内建（现在）· 显式 `import std` · **自动导入 + 可覆盖** | **自动导入 + 可覆盖** ✓（Rust 的 prelude 就是这个形状：`core`/`std` 的关键名字默认在作用域里，别的要写 `use`）|

---

## 7. 怎么证明"模块做对了"（验收，先写好再动手 ✓）

1. **把 `stdlib/prelude.extc` 拆成 `std.core` / `std.io` 两个模块**，而
   **80 个 examples + 两份 OI 横评 + 五子棋 + prelude 自己一行都不用改** ✓
   （靠 Q6 的"自动导入" ✓ —— 这条同时验证了 prelude 的重新定位）
2. **golden 逐字节不变**（模块是编译期概念，不该改生成物 ✓）
3. **可见性真的挡得住**：新的反例（`tests/errors/`）——
   用私有符号 / 调没 import 的模块 / 环 / 路径与声明不符 ⇒ 四条都要**编译期**报错 ✓
4. **`IO.md` 的三档落在正确的模块里**：原语模块只导出给 `std.io`，
   用户模块**拿不到**原语（拿不到 ⇒ 非法；要特权就得自己写 `extern!` 签字 ✓）
5. **编译时长**：`bench/compile` 的 N=500 那一格**不明显变差**（如果 Q5 选 A）✓
6. **同一套常设验收照跑**：244 测试 · ASan 8 · arena 5 · 攻击库基线一字不动 ✓

---

## 9. 目标长相（示例代码 —— **还没实现**，但每一行都按 extC 已有规则写 ✓）

> 主人的要求：「你要不直接给一些你期待中的带模块/库的示例代码给我看看吧。
> **但是无论如何，我总不能真的跟 C++ 一样雷霆展开吧**」
>
> 设计目标一句话：**一个模块 = 一个文件；一个 `use` 就够；没有 .h、没有重复声明、没有构建系统** ✓

### 9.1 特权模块：`std/sys.extc`（只有它能碰系统）

```extc
// std/sys.extc —— 特权模块。别的模块**拿不到它**（见 §5 的可见性规则）✓
extern!("libc") fn read(fd: i32, buf: mut slice<u8>) -> i64
    effects Addr=0 Cont=0          // 我签字：只往你给的 buffer 里写，不存你的指针 ✓
extern!("libc") fn write(fd: i32, buf: slice<u8>) -> i64
    effects Addr=0 Cont=0
extern!("libc") fn open(path: slice<u8>, flags: i32) -> i32
    effects Addr=0 Cont=0
extern!("libc") fn close(fd: i32) -> i32
    effects Addr=0 Cont=0

let STDIN: i32 = 0
let STDOUT: i32 = 1
```

⚠️ C 那侧的签名是 `read(int, void*, size_t)` —— extC 的 `slice<u8>` 在边界上**展开成两个参数** ✓
（§5 的映射表；路径那种 NUL 字符串要顺手写个 `cstr()` 转换器 ✓）

### 9.2 普通库：`std/io.extc`（用 extC 写，没有任何特权）

```extc
// std/io.extc —— 一个文件就是一个模块 ✓
use std::sys                                   // 只 import 我需要的那一个 ✓

type IoError = | notFound | denied | other(i32)

struct File {
    fd: i32
    @private name: slice<u8>                   // 默认公开，**要藏才写** ✓ 不雷霆展开
}

fn open(path: slice<u8>) -> result<mut ref File, IoError> {
    let fd = sys::open(path, 0)
    if fd < 0 { return failure(denied) }
    var f: mut ref File = new File             // 帧拥有：函数一返回，文件自动关 ✓（IO.md §5）
    f.fd = fd
    f.name = path
    return success(f)
}

fn readLine(f: mut ref File, buf: mut slice<u8>) -> result<i64, IoError> {
    var n: i64 = 0
    while n < buf.len {
        let got = sys::read(f.fd, buf[n..])?   // `?` = 失败就顺着往上抛 ✓
        if got == 0 { break }                  // EOF ✓
        if buf[n] == u8(10) { break }          // '\n' ✓
        n = n + got
    }
    return success(n)
}
```

⭐ **注意这里没有的东西**：没有头文件、没有"声明抄两遍"、没有 `#pragma once`、
没有构建清单、**没有一行效果摘要在手写**（编译器从源码算 ✓ —— `LIBS.md` §4.2 说的就是它，
用户只在报错信息里见到它 ✓）

### 9.3 用户库：`algo/sort.extc`（顺路暴露一个真缺口）

```extc
// algo/sort.extc
type order = | asc | desc                    // 闭集"比较方式" = enum + match（不需要闭包 ✓）

fn sortI32(a: mut slice<i32>, o: order) { ... }

// ⚠️ 而这个"本该长这样"的泛型版，**今天写不出来** —— `fn f<T>(…)` 泛型自由函数还不支持 ✗
// fn sort<T>(a: mut slice<T>, less: fn(ref T, ref T) -> bool) { ... }
```

⇒ **库最先撞上的三件事**（都不是这次的模块讨论，但会立刻挡路）：

| # | 缺什么 | 为什么库立刻就要它 | 大小 |
|---|---|---|---|
| 1 | **泛型自由函数** `fn f<T>(…)` | `sort`/`map`/`max` 这些**没法挂在某个 struct 上** ✗（实测：`fn maxOf<T>` = 语法错误）| 中 |
| 2 | **函数值（6a，不捕获）** | 比较器/回调的参数位（`less: fn(ref T, ref T) -> bool`）✗ | 中 |
| 3 | **`extern!` + 签字** | 库碰系统的唯一入口（IO 的前提 ✓）| 中 |

（现在的替代写法：`sortI32`/`sortF64` 一个类型一个函数，或者把比较器写成 enum + match ✓ 能用，就是啰嗦 ✓）

### 9.4 游戏库 + 主程序：`gomoku/board.extc` 与 `gomoku.extc`

```extc
// gomoku/board.extc —— 一个游戏的库（闭集操作，不需要闭包 ✓）
type cell = | empty | black | white
type move = | place(x: i32, y: i32) | undo
type outcome = | playing | win(cell) | draw

struct Board {
    @private grid: [225]cell
    @private n: i32
    side: cell
}

fn empty(n: i32) -> Board { var b: Board  b.n = n  b.side = cell.black  return b }
fn play(b: mut ref Board, m: move) -> outcome { ... }
fn show(b: ref Board) { ... }                 // 打印棋盘（用 println ✓）
fn parse(s: slice<u8>) -> move { ... }        // "8 8" / "undo" ⇒ move ✓
```

```extc
// gomoku.extc —— 主程序：两行 import，然后就是主循环 ✓
use std::io
use gomoku::board

fn main() -> i32 {
    var b: board::Board = board::empty(15)     // 类型也带模块名 ✓（`board::Board`）
    var line: [64]u8

    while true {
        board::show(ref b)                     // 函数也带模块名 ✓（`board::show`）
        let f = io::open("")?                  // 略：真实写法是 io::stdin()
        let n = io::readLine(f, line[..])?
        match board::play(ref b, board::parse(line[0..n])) {
            win(c) => { board::show(ref b)  println("赢了：", c)  return 0 }
            draw   => { println("平局")  return 0 }
            playing => { }
        }
    }
    return 0
}
```

**跑它只要一条命令** ✓（`use` 的文件自动跟着编，最后合成一个 `.c`）：

```sh
extc --run gomoku.extc        # 找模块：先看本目录，再看 -I 给的那些，最后看 $EXTC_HOME/std
```

### 9.5 为什么这不叫"雷霆展开"（C++ 同一件事要写什么）

```cpp
// geometry.h ── 头文件（声明 + 私有字段被迫暴露 ✗）
#pragma once
#include <vector>
namespace geo {
struct Point { double x, y; };
class Polygon {
public:
    explicit Polygon(std::vector<Point> pts);
    double area() const;          // ← 声明
private:
    std::vector<Point> pts_;
    mutable double cached_ = -1; // ← 内部细节也在这个"给外面看"的文件里 ✗
};
}

// geometry.cpp ── 实现（同样的签名**再抄一遍** ✗）
#include "geometry.h"
namespace geo {
Polygon::Polygon(std::vector<Point> pts) : pts_(std::move(pts)) {}
double Polygon::area() const { /* … */ }    // ← 抄第二遍
}
```

| | C++ | extC（上面那套） |
|---|---|---|
| 声明写几遍 | **两遍**（.h 一遍、.cpp 一遍）✗ | **一遍** ✓ |
| 私有细节 | 必须写在头文件里 ✗ | `@private` 藏着 ✓ |
| 重复包含 | `#pragma once` / include guard ✗ | 不存在（语义导入 ✓）|
| 构建清单 | CMake/Makefile + 链接 | `extc --run gomoku.extc` ✓ |
| 模块级私有函数 | `static`（还得自己想）| 默认就是**文件内私有** ✓ |
| 跨模块内联 | 要 LTO | **天生一个 TU** ⇒ 不用 ✓ |

### 9.6 这些写法里，哪些今天就有、哪些要新做

| 写法 | 今天 | 工作量 |
|---|---|---|
| `use std::io` · `io::open(…)` · `board::Board` | ❌ 新 | 模块系统本体（`MODULES.md` §4 方案 A）|
| `@private` | ❌ 新（**注解机制已有**：`@overwrite` 就是它 ✓ 加一项白名单即可）| 一行 |
| `extern!("libc") … effects Addr=0 Cont=0` | ❌ 新 | `LIBS.md` LQ3（签字）|
| `result` / `?` / `failure(…)` / enum + match | ✅ **今天就能跑** | —— |
| `slice` 切片 `line[0..n]` · `[64]u8` · `ref`/`mut ref`/`?ref` | ✅ 今天就能跑 | —— |
| `new File` + "帧拥有 ⇒ 自动关" | ⚠️ 设计已定（`IO.md` §5），未实现 | 检查器**零新规则** ✓ |
| `fn sort<T>(…)` 泛型自由函数 | ❌ 今天没有 | 中（§9.3 那张表）|

---

## 10. 「按实例做增量编译」这件事（主人 2026-09-22 的问题）—— **先量，再决定**

> 主人的想法：「在编译的时候 `varArray` 也有可能被优化成某种**头文件**，按照**实例**来进行
> **增量编译**，这样就避免了**实例单态化展开爆炸**（虽然编译时硬盘空间可能比较多，
> 因为要创建很多文件，但这是**编译时可选优化**）」

**技术上成立，而且 extC 的挂钩点现成** ✓

| 现成的东西 | 在哪 |
|---|---|
| 实例**驻留表**（同一个实例全局只有一份）| `tt->instances`（`types.c` 的 `ttGeneric`）|
| 实例名字是**确定性**的（`varArray_i32`）| `ttMangle` |
| 每个实例的函数**独立生成**、按名字去重 | `codegen.c` 遍历 `tt->instances` |
| 只生成"真被调到的"方法 | `FuncDef.used`（#42(c)）|

⇒ 换句话说：**"实例"在 extC 里已经是个一等公民**，加一层"按 `(模板, 实参, 编译器版本)`
缓存产物"并不需要动语言的语义 ✓（这正是 C++ 的 `extern template` / 显式实例化、
C++20 modules 的 BMI、Rust 的 codegen unit 在做的事；Zig/Go 则干脆全量重编 ✓）

### 10.1 但**先量** —— 实测单态化根本不是瓶颈（2026-09-22）

`bench/compile` 那个大程序（1000 个 struct + 1000 个函数 + 5 个泛型实例）：

| 指标 | 数字 |
|---|---|
| 生成 C | **38 039 行 / 1.25 MB** |
| 其中**泛型实例**相关的行 | **4 390 行 = 11.5%** ✗（不是主因）|
| 前端（extC→C）| **29 ms** |
| gcc 编这份 C | **681 ms** ← **占端到端 96%** |
| 同形**手写** C（4 008 行）的 gcc 时间 | 210 ms |

⇒ **病根不在单态化，在"吐出来的 C 又大又啰嗦"**（38k 行 vs 4k 行 = 9.5 倍）✗
生成 C 的体积分解（实测）：`#line` 指令 **17%** · 空行 **8%** · arena 样板 **8%** ·
每个方法"原型 + 定义"两行（1000 个方法 ⇒ 2000 个函数符号）✓

### 10.2 所以更划算的杠杆顺序（都不动语言）

| 顺位 | 做什么 | 实测/代价 |
|---|---|---|
| 1 | `--no-line-map`（已经有的开关）| 行数 −17%；gcc **681 → 630 ms（−7%）** ✓ 代价 = 错误映射回 .extc 行号 ✗ |
| 2 | 把生成的 C 瘦下来（空行/样板/冗长的类型名）| 未量；这是"编译时长"那一维的正主 ✓ |
| 3 | **ccache 式的产物缓存**（extC 吐的是 C ⇒ 缓存那步 gcc 白拿 ✓）| 零语言改动；键 = 生成的 C 内容 ✓ |
| 4 | **按实例一个 `.o` + 接口摘要**（主人这个想法）| 这才轮到它；代价见下 |

### 10.3 真要做"按实例 .o"时的代价（诚实清单）

- **丢跨实例内联**：多 TU ⇒ 要 LTO 才能找回（而"单 TU + 全 static"正是 #37 那 3.2× 的来源 ✗）
- **缓存键**必须含：编译器版本 · 优化旗子 · 目标平台 · 接口摘要 ⇒ 任一不同就得重编 ✓
- **陈旧产物**：要有校验（Modula-2 的符号文件就是干这个的）✓
- **文件数量**：主人自己也提到了（硬盘换时间 ✓ 值得，但要先量出来值多少）

⇒ **结论**：记成 PLAN 行（可选优化），**前置 = 先把 1/2/3 试掉**（它们不需要动语言 ✓）
