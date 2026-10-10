# 模块与 C 互操作现状

本文只写**现在有什么 / 怎么用**：每一句都能指到**源码位置**或**一条能跑的判据**。
待做不在这里（见 [`../TODO.md`](../TODO.md)），历史不在这里（见 [`../CHANGELOG.md`](../CHANGELOG.md)）。

## 1. 模块系统

实现全在 （文件头 是它对现状的自述）。编译产物始终是**一个 C 文件**：
`use` 是语义导入，不是文本 include。

### 1.1 一个文件 = 一个模块；路径怎么找

`use a::b` 找的是**文件 `a/b.extc`**；`use a::b::c` 找 `a/b/c.extc`。
搜索顺序：**入口文件所在目录 → 每个 `-I <dir>` → 标准库目录**；标准库目录 = `$EXTC_STD`，否则
`extc` 可执行文件旁边的 `../stdlib`。

| 写法 | 结果 | 判据 |
|---|---|---|
| `use sub::gamma`（磁盘 `sub/gamma.extc`） | 可以 | 实测 `env -C /tmp/usage-std/std-d-mod extc --run std-d-e5.extc` → `1` |
| `use alpha2`（磁盘上是目录 `alpha2/inner.extc`） | 不行：`cannot find module \`alpha2\`` | ；实测 |
| `use alpha2::inner`（磁盘 `alpha2/inner.extc`） | 可以 | 实测 → `2` |
| 模块里写 `main` | 编译期：`` `main` must live in the entry file, not in a module `` |  |
| `-I <stdlib 目录>` 显式指路 | 照样能编（特权层检查按规范化路径判断） |  的 `stdlib-via-I` 一节 |

### 1.2 四种 `use` 写法（都能用）

| 写法 | 带进来什么 | 位置 |
|---|---|---|
| `use a` | 限定名 `a::x`；此外 `a` 的**结构体类型名**可以裸写（实测） |  |
| `use a::{b, c}` | 恰好列出的名字，**可以裸写** `b` |  |
| `use a::*` | 该模块每一个**公开**名字 |  |
| `use a as al` | 短名换成 `al`，之后写 `al::x` |  |

不支持的写法，以及编译器给出的替代：

| 写法 | 结果 |
|---|---|
| `use a::{x as y}` | 解析错：`` error: expected `,` or `}` in the import list `` |
| `use a::x`（`x` 是**名字**不是模块） | `` note: `x` is a **name**, not a module -- `use` resolves module paths only. `` 随后是 `cannot find module` |
| 裸写另一个模块的 `fn` / 全局 | 编不过，见下 |

**裸名的实测口径**（同一台编译器、同一批探针）：

| 你写 | 现象 |
|---|---|
| `use alpha` + 裸 `open2()`（alpha 的公开 fn） | `` error: `open2` belongs to module `alpha` -- write `alpha::open2` `` |
| `use alpha` + 裸 `secret()`（alpha 的 `@private` fn） | `` error: call to undefined function `secret` ``（隐私错误不泄露"它存在"） |
| `use beta` + 裸 `G`（beta 的公开 `let`） | `` error: undefined name `G` `` |
| `use alpha` + 裸 `box`（alpha 的公开 struct，用作类型） | **可以** |

最后一行是现状里的一处**不对称**：`use a` 之后，`a` 的**结构体类型名**可以裸写，而它的函数/全局**不行**。

### 1.3 `@private` 现在挡什么 / 不挡什么

声明默认公开，`@private` 把它从"别的模块可命名"里拿掉。实测的边界：

| 形状 | 现象 | 判据 |
|---|---|---|
| `a::secret`（限定名） | `` error: `a::secret` is private to module `a` `` | 实测 |
| `use a::*` 之后裸写私有名字 | 同上（报限定名形式） |  |
| `use a::{secret}` | 同上 |  |
| 私有 **struct** 用裸类型名（`use a` + `var x: secretbox`） | **编不过，而且真的跑得起来**（见下） | 实测 |
| 私有 `let` 裸写 | `` error: undefined name `SG` `` | 实测 |

私有 struct 的那条是现存的**可见性缺口**（两个文件；实测就是把下面这两份放同一目录跑）：

```text
// gamma2.extc
@private struct secretbox { v: i64 }

// main.extc
use gamma2
use std::io
fn main() -> i32 { var b: secretbox = { v: 1 }  io::cout << b.v << "\n"  return 0 }

$ extc --run main.extc
1
```
写成 `gamma2::secretbox` 则被 `` is private to module `` 挡住。
（ 把这类"扁平表 + 别名表"的边界写得很清楚：真正的按模块命名空间还没做。）

### 1.4 名字怎么共存：普通声明按模块改名，`extern!` 不改

模块的顶层声明会**合并进一张全局表**，编译器用"每模块改名"（`pair` → `pair$pair`）让同名共存
。实测的边界：

| 形状 | 现象 |
|---|---|
| 两个模块各声明一个 `fn helper`，入口文件也声明一个 `helper` | **编得过**（各自的被改名） |
| 两个模块逐声明重名 | 互不干扰 —— 正面判据 （ 的①） |
| 两个导入的末段同名（`a::util`、`b::util`） | `` error: both `a::util` and `b::util` are used as `util` `` |
| 两个模块都导出同名**裸类型**，而你要裸写 | `` error: ambiguous type `box` -- 2 modules export it, write `module::box` ``（实测；） |
| **`extern!` 的同一个 C 符号声明两次** | `` error: duplicate function `write` `` —— `extern!` 必须保持 C 符号名，所以它**不改名**：`use std::io`（依赖 `std::sys::io`，那里声明了 `read`/`write`/`open`/`close`，）之后自己再声明 `write` 就撞；实测一个模块里的 `extern!("libc") fn getpid()` 与用户文件里的同名声明同样撞 |

`$`（改名后的内部名字）**不许**出现在诊断、普通输出与调试开关里（ 的判据③）。

### 1.5 环检测

一个模块在**还在加载**时又被要求加载 = 环，直接拒绝（、）：

```text
$ ./build/extc --build -o /dev/null std-d-m14.extc      # cyc_a.extc 用 cyc_b，cyc_b.extc 用 cyc_a
./cyc_b.extc:1: error: import cycle: `cyc_a` is still being loaded
note:  two modules must not depend on each other (each one's type is
       needed to check the other). Put the shared part in a third module.
```

### 1.6 诊断质量：内部改名不许漏出来

模块会把自己的声明改名（`pair` → `pair$pair`）来避免撞车，但**诊断、输出、调试开关里不许出现
`$` 夹在标识符之间**（ 的判据③； 另要求
模块体内的诊断指到**模块文件**而不是入口文件）。

## 2. C 互操作

### 2.1 `extern!`：签名规则

形状：

```extc
use std::io
extern!("libc") fn getpid() -> i32
extern!("libc") fn strlen(s: ref u8) -> u64
    effects Addr=0 Cont=0

fn main() -> i32 {
    io::cout << "pid-positive=" << (getpid() > 0) << "\n"
    return 0
}
```
实测：`pid-positive=true`（`effects Addr=0 Cont=0` = "不存你给的指针"）。

`extern!("...")` 里的字符串是**哪个运行期**提供这个符号（`libc` / `extc-runtime` / `extc-mem` /
`extc-heap` / `extc-time`），函数**只声明不定义**。

| 能不能过 C 边界 | 类型 | 证据 |
|---|---|---|
| 能 | 标量（`i8`…`u64`、`f32/f64`、`bool`，`bool` 也当一个机器字） | ；`extern!("libc") fn f(b: bool) -> bool` 实测编过 |
| 能 | 指针：`ref T` / `?ref T` / `ref void`（C 的 `T*` / `void*`；`ref T` → `void*` 的抹除方向也开） | 、 |
| 能 | `fn(…) -> …` 函数指针 |  |
| 能 | **`@frozen` 结构体按值传/返回**（布局由 `@frozen` 承诺） |  的 `imaxdiv -> mixed2` |
| **不能** | `slice<T>` | `` error: `extern!` argument 1 has type `slice<u8>`, which cannot cross the C boundary `` |
| **不能** | 没标 `@frozen` 的结构体按值 | `` error: `extern!` argument 1 has type `point`, which cannot cross the C boundary `` |
| **不能** | `option<T>` / `result<T, E>` / `?i64` | `` error: `extern!` return type `option<i64>` cannot cross the C boundary ``（实测） |
| **不能** | 可变参数 `...` | 解析错：`` error: expected a parameter name, found `...` ``（实测） |
| **不能** | `owned`（内存归属标记） | `` error: `owned` is not implemented yet `` |

 把理由写死了：**C 的一个参数就是一个机器字**——标量、指针、函数指针；
`slice<T>` 是"指针 + 长度"两个 C 参数，映射表达不出来，所以库要自己把 `s.data` / `s.len` 拆开传。

### 2.2 `effects`：Addr / Cont / Ret / Thread

`effects` 是**作者对这次调用的断言**，写在**没有函数体**的声明上（`extern!` / `@builtin`），
因为编译器看不进去。不写 = 按最坏情况（"它可能把你的指针留下来"）。

| 键 | 允许的值 | 意思 |
|---|---|---|
| `Addr=` | 小整数（按参数位组成的掩码） | 这个调用会不会**存下**实参里的地址；`0` = 不存 |
| `Cont=` | 同上 | 存下的是不是"能继续往外传"的那种（可传递的容器） |
| `Ret=0` | `0` 或 `1` | 我返回的引用/视图**不指向调用方的帧**；`1` = 不提额外信息（与不写同义）。其它值报错：`` `Ret=2` is out of range (use 0 or 1) ``（实测） |
| `Thread=0` | `0` 或 `1` | `0` = 不碰跨线程共享状态（`parallel::run` 的 worker 里只许调这种） |

实测的边界：

- **不签字 = 最保守**：把本帧局部变量的地址交给没签字的 C 函数，编译期就挡住：

  ```text
  tests/extern/errors/no_effects.extc:3:1: error: argument 1 of `fill` may be kept by C forever, but it points into this frame (depth 1)
  ```
- 有函数体的函数也可以写 `effects`，但**只有 `Ret` 与 `Thread` 有意义**（`Addr`/`Cont` 由函数体自己算，
   说在这里写 `Addr` 应当拒绝）。**实测：`fn f(x: ref i64) -> i64 effects Addr=0 { ... }`
  编得过；而且 `EXTC_DUMP_EFFECTS=1` 下，写与不写的 `[effects] f …` 一行逐字节相同 ⇒ 对有函数体的函数，
  `Addr=` 不改变任何东西。**
- `Ret=0` 是标准库替所有使用者签的那一次：`std::heap` 的 `alloc` / `view` / `allocPtr` / `store` 都是有体的
  extC 函数，它们签"我返回的视图不指向调用方帧"，于是用户可以
  `var v = pl.alloc(n)!` 之后把 `v.data` 交给任何没签字的 C 函数（判据 ）。
- `Thread=0` 用在系统原语上：与
   起的一整排 socket/epoll 原语（注释写着"只碰实参，不碰跨线程共享状态
  ⇒ worker 里可用"）。

### 2.3 `@export`：extC 函数变成 C 符号

`@export` 只能贴在**自由函数**上；产出的 C 符号名就是源文件里那个名字（模块前缀被去掉），
定义与原型都是外部链接（ 用 `nm -D` 查 `T triple` / `T mid` / `T makePair`）。

| 形状 | 现象（实测/判据） |
|---|---|
| `@export fn triple(x: i64) -> i64` | 产物里 `int64_t triple(int64_t x)` + 定义；`nm -D` 有 `T triple`（、） |
| `@export` + 返回 `@frozen` 结构体 | 可以（System V 上从 RAX:RDX 回来； 的 `makePair`） |
| `@export` 泛型函数 | `` error: `@export` on a generic function `` |
| `@export` 方法 | `` error: `@export` goes on a free function, not on a method ``（`.../method.extc`） |
| `@export` struct | `` error: `@export` goes on a function ``（`.../on_struct.extc`） |
| `@export` 全局 | `` error: `@export` goes on a function ``（`.../on_global.extc`） |
| `@export` + `extern!` | `` error: `@export` and `extern!` contradict each other ``（`.../extern_export.extc`） |
| C 关键字当导出名（`double`） | `` error: `@export` on `double`: that name is a C keyword, so the symbol would have to be renamed ``（`.../keyword.extc`） |
| `slice<T>` 参数 | `` error: `@export` parameter 1 has type `slice<u8>`, which C cannot pass ``（`.../slice_param.extc`） |
| 函数体分配并返回引用（多一个隐藏 home arena 参数） | `` error: `@export` on `make`, which takes a hidden arena parameter ``（`.../hidden_arena.extc`） |
| 两个导出函数同名 | `` error: `@export` name `init` is already taken by another exported function ``（`.../dup_main.extc`） |
| 写两遍 `@export` | `` error: `@export` appears twice ``（`.../twice.extc`） |

**实测的缺口（"接不通"）**：`@export` 的函数被**同程序的另一个模块**调用时，生成的 C 里定义叫 `ex`、
调用点却写 `expmod$ex` ⇒ C 编译不过：

```text
$ ./build/extc -w --no-line-map -o /tmp/usage-std/exp1.c std-d-exp1.extc
/tmp/usage-std/exp1.c:125:45: error: implicit declaration of function ‘expmod$ex’
```
（模块 `expmod.extc` 里 `@export fn ex(...)`；入口 `use expmod` 后调 `expmod::ex(...)`。同一份模块当**根文件**
单独编译时，`ex` 是正常的 `T` 符号。）

### 2.4 `@frozen`：布局就是 C 的布局

`@frozen` 只能贴在 **struct** 上（、`on_enum.extc` 反例：
`` error: `@frozen` goes on a `struct` ``；`on_impl.extc`：`` no annotation applies to an `impl` block ``；
`twice.extc`：`` error: `@frozen` appears twice ``）。

它换来两件事：

1. **按值过 C 边界**（没标的按值传会被 `cannot cross the C boundary` 挡住）。
2. **产物里自带布局断言**：生成的 C 在结构体后面写 `_Static_assert`（字段顺序 + 尾部无额外填充），
   把产物的 `offsetof` 钉住（实测：`@frozen struct pair2 { lo, hi }` 的产物里 `grep _Static_assert`
   有两条）。 的**镜像测试**再拿一份手写 C 结构体把
   `sizeof` 与每个字段的 `offsetof` 在编译期对上，并且把"调换字段顺序"做成有牙的反例。

`@frozen` 里的字段用不用 `@frozen`/是不是 `slice` 都编得过：实测
`@frozen struct s { a: slice<u8> }` 生成 `struct slice_u8 { … }; struct s { slice_u8 a; }`，作为
`extern!` 的参数也能编过（**手写 C 镜像是否与此逐字段一致，本页未验证**）。

### 2.5 `@noCopy`：有身份的值不许按值拷贝

`@noCopy` 贴 struct，规则是"**已经存在的值**不许按值拷贝"（ 的判据）。

| 形状 | 现象 |
|---|---|
| 造新值 `var a: counter = { pos: 0 }` | 可以 |
| 按 `ref` / `mut ref` 传、在它上面调方法、读字段 | 可以 |
| 绑定一个**已有的**值：`var b: counter = a` | 挡住（四个复制点：绑定 · 实参 · 字段初始化 · 赋值，各有一个反例文件） |

诊断口径（，实测）：

```text
std-d-nocopy1.extc:4:1: error: `tok` is `@noCopy`: it may not be copied by value
note: Pass it as `ref` / `mut ref` instead: `f(ref s)`, or declare the parameter `mut ref T`. The state it carries has an identity, so a copy would give two names to one position.
```

标准库里的 `@noCopy`：`std::io` 的 `writer`、`std::fs` 的
`ifstream` / `ofstream`（、）、`std::heap` 的 `plate`
、`std::json` 的 `builder`、
`std::loop` 的 `timers`。

### 2.6 `std::dl`：动态加载

| 名字 | 签名 | 位置 |
|---|---|---|
| `open(path) -> option<ref void>` | `dlopen(..., RTLD_NOW)`；失败 `none` |  |
| `sym(h, name) -> option<ref void>` | `dlsym` | |
| `symOrNull(h, name) -> ?ref void` | 要自己判空（打人话）时用它 | |
| `close(h) -> i32` | `dlclose` | |
| `cstr(s)` / `cstrLen(p)` / `viewCStr(p)` | extC ↔ C 字符串的两种方向 | |
| `RTLD_LAZY` / `RTLD_NOW` / `RTLD_GLOBAL` | `1` / `2` / `256` | |

规则：`path` 必须 **NUL 结尾**（字面量直接传；运行期拼的先用 `cstr` 补 0），因为 extC 的切片不在值里带终止符
。`dlsym` 回来的是**地址，不是类型**；变成可调用的值必须调用方写一次显式转换
（`fn(i64) -> i64(raw)`），那句话就是签字。

```extc
use std::io
use std::dl
fn main() -> i32 {
    var h: ref void = dl::open("./std-d-probe.so")!     // tests/cabi/dlprobe.c 编出的 .so
    var raw: ref void = dl::sym(h, "triple")!
    var triple: fn(i64) -> i64 = fn(i64) -> i64(raw)
    let t = triple(14)
    let rc = dl::close(h)
    io::cout << "triple=" << t << " close=" << rc << "\n"
    return 0
}
```
实测：`triple=42 close=0`。

> **求值顺序的坑**：`io::cout` 的 `<<` 链实参是**从右往左**求值的（实测 `io::cout << "x=" << a() << b()`
> 里 `b()` 先跑）。所以 `io::cout << triple(14) << dl::close(h)` 会先 `dlclose` 再调用 ⇒ 段错误。
> `close` 永远另起一句、先把值取出来。完整对照见 [`stdlib.md`](stdlib.md) 第 4 节。

### 2.7 今天接不通的（清单）

| 想做的事 | 现状 | 证据 |
|---|---|---|
| `use a::{x as y}`（给名字起别名） | 解析错 | 实测：`` expected `,` or `}` in the import list `` |
| 另一个模块的 `fn` / 全局裸写 | 必须限定写 `a::x`（全局报 `undefined name`） | 实测 |
| `@export` 泛型 / 方法 / struct / 全局 / 与 `extern!` 同用 | 编译期拒绝（各有专门消息） |  |
| `@export` 的函数被同程序另一模块调用 | 生成物调用 `mod$fn`、定义是 `fn` ⇒ C 编译不过 | 实测 `implicit declaration of function 'expmod$ex'` |
| `extern!` 收/发 `slice<T>`、非 `@frozen` 结构体按值、`option`/`result` | 编译期拒绝 | 、实测 |
| `extern!` 可变参数 | 解析错（没有 `...`） | 实测 |
| `extern!` 的 `owned`（内存归属） | `` `owned` is not implemented yet `` |  |
| 用户文件里声明 `@builtin` | `` `@builtin` declarations are standard-library only `` | 、实测 |
| 用户文件 import `std::sys::heap` | 被挡（特权层） | 、 |
| 用户文件 import 其余 `std::sys::*` | **能**（`io` / `net` / `pool` / `domain` 实测都编过、也能调）——这条边界目前是约定 | 实测 |
| `@private` struct 用裸类型名 | **能**（限定名反被挡） | 实测（见 1.3） |
| 模块按目录 import（`use alpha2` 找 `alpha2/…`） | 不行：模块 = 一个文件 | 、实测 |
| 两个模块共享一个"模块"（两个文件一个模块） | 不支持：一个文件一个模块 |  |
