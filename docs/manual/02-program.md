<!-- 本页原来是 docs/MANUAL.md 的「1. 程序结构」一节（原 §1.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 0.5 内存安全：承诺 × 现状](01-safety.md) · [2. 词法 →](03-lexical.md)

---

# 1. 程序结构

一个 `.extc` 文件里，顶层只允许三种东西：

```extc
type 名字 = | 变体 | 变体      // 枚举（无载荷的标签联合）
struct 名字 { ... }            // 结构体定义
fn 名字(参数) -> 返回类型 { ... }   // 函数定义
```

**模块系统已经有了**：`use std::io`，别名写 `use std::sys::io as sysio`，
全限定名到处都能写（`io::cout`、`lib::sub::util::answer()`，见第 12 节）。

程序入口是 `main`，两种形状都行：

```extc
fn main() -> i32 { ... }                          // 不要命令行
fn main(args: slice<slice<u8>>) -> i32 { ... }    // 要命令行
```

`args[0]` 是程序名，字节**直接指向**操作系统给的参数块（不拷贝 验收 `tests/argv/`）。
**返回值就是进程退出码** —— 即使函数带 arena、走共用尾声，这个值也照样带得出来
（`fn main() -> i32 { var p = new i32  return i32(*p) }` ⇒ 退出码 `*p` 2026-09-26 修）。

---

### 递归：**允许**（互递归也允许），跑飞了有护栏

**递归不需要声明什么**，直接写就行；**互递归也允许**：

```extc
fn isEven(n: i64) -> bool { if n == 0 { return true }  return isOdd(n - 1) }
fn isOdd(n: i64)  -> bool { if n == 0 { return false } return isEven(n - 1) }   // 互递归
```

**跑飞了会被编译器的护栏挡住**（不是让 OS 报段错误、更不是无声挂住）：
凡是**传递地**调回自己的函数（`g → h → g` 里 `g` 也算），序言自增一个深度计数，
超过 `EXTC_REC_LIMIT`（**10 万**层）就 `trap`，**带源码位置**：

```
mut_bad.extc:1: trap: recursion too deep (unbounded recursion?)
```

代价只落在**需要它的函数**身上：**非递归函数不加守卫**（`tests/traps/` 钉着这条），
普通调用一分钱不花

> **为什么 10 万层总是安全的**：实测递归函数的栈帧 **24 字节**（`-O2` 下的 `fib`）
> ⇒ 10 万层 ≈ **2.4MB**，**小于**默认栈 8MB ⇒ **护栏总是先响，栈溢出在这门语言里见不到**
>
> 这也是 `@recursive`（把调用栈换成编译期定长的显式栈，设计见 `DESIGN.md` §3）
> **无限期推后**的实测理由：**它要解决的问题已经不存在了**

---

### prelude：自带类型不用 import

编译器自带一小段 extC 源码（`stdlib/prelude.extc`），**每次编译都先于使用者的文件被处理**。
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

> prelude 的用途：凡能**用 extC 写成**的标准设施，就用 extC 写，而不是写进编译器。
> 没有它，`slice<T>` 只能在编译器的代码生成器里硬编码 —— 那就成了**把库塞进编译器**。
> 验收方式很机械：`grep -in slice src/*.c src/*.h` 应该**一无所获**。
>
> 现在 `slice` 的方法很少（没有数组索引和 `option` 就写不出 `get`），
> 而且**空 slice 造不出来** —— `ref` 不可为空，长度 0 的切片没有合法的 `data`。

---

**[索引](README.md)** · [← 0.5 内存安全：承诺 × 现状](01-safety.md) · [2. 词法 →](03-lexical.md)
