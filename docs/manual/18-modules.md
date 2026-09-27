<!-- 本页原来是 docs/MANUAL.md 的「12. 库与模块（2026-09-22 新增）」一节（原 §12.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 11. 还没定的](17-undecided.md) · [13. 完整示例 →](19-examples.md)

---

# 12. 库与模块（2026-09-22 新增）

> 这一节是**已经能跑的**：模块（定案 70）· 泛型自由函数（定案 71）· `extern!`（定案 72）·
> `std::io`（定案 73）没实现的一律不写在这里（见 `PLAN.md`）

### 12.1 模块：一个文件就是一个模块

```extc
// lib/util.extc
struct pair { a: i32  b: i32 }               // 类型名 camelCase（大写开头留给类型参数 T）

fn make(a: i32, b: i32) -> pair { var p: pair = { a: a, b: b }  return p }
fn total(p: pair) -> i32 { return p.a + p.b }

@private fn helper() -> i32 { return 1 }     // 藏起来：**别的模块引用不到**
```

<!-- manual-example: skip —— 这段是**多文件**示例（另一个文件不在这里） -->
```extc
// main.extc
use lib::util                                // **语义导入**（不是 C 的文本包含）

fn main() -> i32 {
    var p: util::pair = util::make(1, 2)     // 跨模块引用写**限定名**
    println("和 = ", util::total(p))
    return 0
}
```

| 规则 | 说明 |
|---|---|
| 模块路径 | `use lib::util` ⇒ `<项目根>/lib/util.extc`（项目根 = 入口文件所在目录；也可用 `-I <dir>`）|
| 短名 | 路径最后一段（`use lib::util` ⇒ 引用时写 `util::name`）|
| **全名** | **任意层都能写全名**（2026-09-23 起）：`std::sys::io::STDOUT` · `std::sys::io::write(…)` · `lib::sub::color.color.green` 不需要先 `use` 到"正好那一段" |
| **别名** | `use std::sys::io as sysio` ⇒ 之后写 `sysio::STDOUT` 别名只换**短名**，全名照样能用 |
| 可见性 | **默认公开**；要藏写 `@private`（别的模块引用它 ⇒ 编译错误）|
| 必须限定 | 其他模块的名字**必须**写作 `mod::name`（遗漏时报错并指出应写的内容）|
| 环 | **禁止** import 环（报错会说清是哪两个模块）|
| `main` | 只能写在**入口文件**里（模块是库）|
| 编译 | 仍然**只吐一个 .c**（`extc --run main.extc` 一条命令，`use` 的文件自动跟着编）|

**两层同名怎么办**（`std::io` 与 `std::sys::io` 的短名都是 `io`）—— 给其中一个起别名
这也是别名真正必要的场合（"从 stdin 读 + `open` 一个文件"恰好要这两层）：

```extc
use std::io                     // 短名 io（普通库那层）
use std::sys::io as sysio       // 短名 sysio（特权原语那层）
var r = io::readerOf(sysio::STDIN)          // 各用各的短名
let f = std::sys::io::STDOUT                // 全名也照样能写
```

没起别名时两条 `io` **必然撞车**，报的是 `both \`std::io\` and \`std::sys::io\` are used as \`io\``
（常设验收 `tests/qname/`，6 正例 + 1 反例）

```extc
use lib::sub::color             // 深层模块（lib/sub/color.extc）
let c: color::color = color::color.green     // 变体位置：限定名 + `.变体`
```

### 12.1.1 `use mod::*`：把名字**带进作用域**（2026-09-24 定案 85）

默认写法是限定名（`io::cout`），想让一个模块的**公开名字不限定可用**，就在 `use` 末尾写 `*`：

```extc
use std::io::*                 // io 的公开名字进入本文件的作用域
cout << "x = " << x + y << endl      // 不限定（这才是流式 IO 想写成的那句话）
cin >> a >> b
```

三条边界（都有常设验收，`tests/modules/`）：

- **`@private` 仍然够不着** —— 打开模块**不是**绕过隐私的路子
  （`error: \`lib::secret\` is private to module \`lib\``）
- **打开是每个文件各自的**：`a.extc` 里 `use b::*` **不会**让入口文件也能写裸 `bSecret`
- **两个被打开的模块重名 ⇒ 报错，不许静默挑一个**
  （`error: \`who\` is exported by both \`x1\` and \`x2\`, which are opened here`）

**只想要一两个名字**时用花括号（2026-09-24 同日补，同一张定案）：

```extc
use std::io::{cin, cout, endl}     // 只有这三个名字进来，别的仍要写 io::xxx
```

三种写法范围从小到大、都并存：`io::cout`（限定）· `use std::io::{cin, cout}`（**推荐**：
范围小到可自行写出）· `use std::io::*`（整个模块）

**`use std::io::cin` 裸写不行**（`use` 的每一段都是**模块路径**，它去找 `std/io/cin.extc`）——
报错会指出应写作 `use std::io::{cin}`（不允许裸写的原因：其含义将取决于"当前是否存在该文件"，
哪天有人建了 `io/cin.extc` 就**静默变意思**）

限定名**照旧**能用（两条路并存）—— 定案 70 的"全名是权利"没有被削弱，
`*` / `{...}` 都是**用户显式写出来**的选择

v1 限制：**类型名**"必须限定"那条还没挡严（函数/全局已经挡严）；
`use` 不重新导出（没有 per-module 的可见性传递 —— 所以 `*` 也**不传递**）

### 12.2 泛型自由函数 `fn f<T>(…)`

```extc
fn indexOf<T>(a: slice<T>, x: T) -> i64 {
    var i: i64 = 0
    while i < a.len { if a[i] == x { return i }  i = i + 1 }
    return 0 - 1
}

indexOf(nums[..], 30)          // 从实参**推导**
indexOf<i32>(nums[..], 50)     // **显式实参**（`T` 只出现在返回类型时必须这么写）
```

- `T` 上的 `==` / `!=`（**推迟到实例化**再查"这个 `T` 有没有 `fn ==`"）
- `T` 上的 `<` / `>`（还没有比较协议 —— 要等函数值/协议那一步）
- 泛型体里**不能**再调用泛型函数（报错会说清原因，见 `PLAN #50`）

### 12.3 跟 C 打交道：`extern!` + 信任声明

```extc
extern!("libc") fn write(fd: i32, buf: ref u8, n: i64) -> i64
    effects Addr=0 Cont=0        // 我签字：**我不存你的指针** ⇒ 实参不受寿命约束

extern!("libc") fn fill(p: ref i32, n: i32) -> i32     // 没签字
// fill(ref x, 3)  ⇒ **编译错误**：C 可能把 `&x` 存到帧外（默认最保守）
```

- 参数/返回只用**标量或单指针**（`ref T` / `?ref T`）—— `slice<T>` 在 C 那边是两个参数
- 想调"往 buffer 里写"的那种（`read`/`write`），传 `s.data` 和 `s.len`
- `owned`（来自 C 的内存由本模块负责）**尚未实现** ⇒ 会明确报错（原计划等"块拥有资源"那套；定案 79 之后口径是**显式释放**，编译器只证明能证明的泄漏）

### 12.4 让编译器内联：`@inline`（2026-09-24 新增）

写在**函数声明前**，要求这个调用点必须被内联：

```extc
@inline fn sq(x: i64) -> i64 { return x * x }        // 普通函数
struct pt {
    x: i64
    @inline fn get(self: ref pt) -> i64 { return self.x }   // 方法（写在 struct 体内）
}
@private @inline fn twice(x: i64) -> i64 { return x + x }    // 与 @private 连用，顺序随意
```

**这是要求，而非建议。** ISO C 的 `inline` 只是提示，编译器可以不予采纳（而且**不会告知**）；
`@inline` 生成的 C 用 `always_inline`，它**必须**被满足

**为什么需要它**：库的热循环里，每字节一次方法调用的代价很大 ——
实测 `nextInt` 那条路，`nextByte` + `skipSpace` 两个方法占掉**全部指令的 51%**
（同一处改写循环结构又拿掉 48%，所以**两者是互补的**，见下面的"什么时候不该用"）

**三条会被编译期拒绝的写法**（都带位置，不是"生成的文件第几行"）：

| 写法 | 为什么拒 |
|---|---|
| `@inline` 标在**递归**函数上（含**互递归**）| `always_inline` 是要求，递归函数满足不了 ⇒ C 编译器会报 `inlining failed in call to 'always_inline'`，而那是生成文件里的位置 ⇒ 挪到源码这一行报 |
| `@inline` 标在 `extern` 声明上 | 没有函数体，没什么可内联的 |
| 写错的注解名（`@fast` / `@recursive` / `@main`）| 注解是**给编译器的指令**，写错必须报错；`@recursive` / `@main` **设计过但没实现**，所以明说"还没实现"而不是静默接受 |

> **什么时候不该用**（实测教训，2026-09-24 交错 A/B 重测）：
> 给 `std::io` 的 4 个 reader 方法标 `@inline`，**读 100 万行 8.88ms vs 8.85ms —— 差 ±0.3%，就是零**
> 原因**查到了**（不是猜）：这些函数在生成 C 里是 `static`，`-O2` 下 GCC **本来就全部内联掉了** ——
> `objdump` 里**既没有调用点、连函数体都不存在** ⇒ `always_inline` 无事可做
> **那 @inline 什么时候有用**：GCC **不肯**内联的时候 —— 运行时原语 `extc_arena_alloc`
> （分配快路径 + `malloc` 慢路径 + 调用点多 ⇒ 被 size 启发式挡掉）标上之后
> rebuild **100.7 → 25.7ms**、churn **140.9 → 38.2ms**（3.9×）
> ⇒ **先量再用**：`objdump` 里还有 `call` 才谈得上"内联能不能帮上忙"
### 12.3.1 `main` 里的 `?`：失败即 trap（2026-09-24，定案 89）

<!-- manual-example: skip —— 这段用 `...` 省略了中间部分 -->
```extc
fn main() -> i32 {
    var f = fs::ifstream("input.txt")?     // 打不开 ⇒ trap（带位置 + 退出码 1）
    ...
    return 0
}
```
`?` 的语义是"把失败交给外层"，而 `main` 的外层是**进程边界** ⇒ 交出去 = **说清楚然后死**
别的函数仍必须声明 `result` 才能用 `?`（`main` 返回类型不许改：C 入口返回 `int`）

### 12.3.2 五个流：`cin` / `cout` / `cerr` / `fin` / `fout`（2026-09-24）

```extc
use std::io::{cin, cout, cerr, endl}
use std::fs

fn log(n: i64) { cerr << "log: " << n << endl }        // 任何函数里都能写诊断

fn main() -> i32 {
    fs::openIn("input.txt")?
    fs::openOut("app.log")?
    var a: i64 = 0
    var b: i64 = 0
    cin >> a >> b                                       // 控制台读
    fs::fin >> a >> b                                   // 文件读（同一个形状）
    cout << a + b << endl                               // 正常输出 → stdout
    fs::fout << "结果 = " << a + b << endl              // 文件写（任何函数里都能写）
    cerr << "仅诊断" << endl                            // 诊断 → stderr
    fs::closeIn()?
    fs::closeOut()?
    return 0
}
```

- **`print` / `println` 打 stdout**（量过），**`cerr` 才打 stderr** 两条通道分开，
  所以 `prog > out.txt` 只收正常输出
- 文件的 `fin`/`fout` 是**程序级的那一个**（各一个 fd）⇒ 不用把流当参数一路传
  它们**不是**在静态段打开的（理由见 `docs/topics/IO.md`）：`main` 里一句话 + 定案 89 的 `?`

### 12.4.1 `@noCopy`：状态有**身份**的类型不许按值拷贝（2026-09-24，定案 88）

```extc
@noCopy
struct reader {
    fd:  i32
    len: i64
    pos: i64        // ← "读到哪了"就是它的**身份**
}
```

标了 `@noCopy` 的类型**只能按 `ref` / `mut ref` 传递**，按值拷贝**编译期报错**并给出修改方式：

    error: `reader` is `@noCopy`: it may not be copied by value
    note:  Pass it as `ref` / `mut ref` instead: `f(ref s)`, or declare the parameter `mut ref T`.
           The state it carries has an identity, so a copy would give two names to one position.

**四个复制点全挡**：绑定（`var b = a`）· 实参（`f(a)`）· 字段初始化（`{ c: a }`）· 赋值（`b = a`）
**而这四件事照旧成立**：造新值（`reader { fd: 0, len: 0, pos: 0 }` / 构造函数）· `ref` 传参 ·
读**字段**（`r.pos` 是 `i64`）· 在它上面**调方法**（接收者不是值位置）

**为什么需要它**（四个语言一致的规矩）：拷贝一个 `reader` ⇒ 两份对象、同一个缓冲、**两个位置**，
读起来互相穿插而两个名字都看不出来 —— C++ 删拷贝构造 · Rust 移动+借用 · Go 传指针 ·
Python 无隐式对象拷贝，说的都是这一件事 而它同时是**流式链式**（`fin >> x >> line`）的地基：
链式靠"返回的是同一个对象"，那就必须保证没人能悄悄拷它

**在库里已经用上**：`std::io` 的 `reader` / `writer` · `std::fs` 的 `ifstream` / `ofstream`
（它们本来就全程按 `ref` / `mut ref` 传 —— 加上注解后**一个调用点都没改**，这也验证了这个取舍）


### 12.5 输入输出：`std::io`（2026-09-24 更新）

**两种输入风格并存**（都在同一个 `reader` 上，可以混着用）：

```extc
use std::io

fn run() -> result<unit, io::ioError> {
    var line: [64]u8
    var r = io::readerOf(io::STDIN_FD)   // 缓冲**不用你给**：64KB 是 new 出来的

    let a = r.nextInt()?                 // OI 式：跳过空白，不用先读成行
    let b = r.nextInt()?
    println("求和 = ", a + b)

    let n = r.nextLine(line[..])?        // 协议式：行是单位，读到换行为止（不含换行）
    io::flushOut()                       // 跟 writeBytes 混用**必须**刷，不然顺序会乱
    io::writeBytes("你说的是：")
    io::writeBytes(line[0..n])
    io::writeBytes("\n")
    return success(unit {})
}

fn main() -> i32 {
    match run() {
        success(u) => { return 0 }
        failure(er) => {
            match er {
                readFailed(fd)  => println("io: 读失败 (fd = ", fd, ")")
                lineTooLong(n)  => println("io: 一行太长了（", n, " 字节）")
                writeFailed(fd) => println("io: 写失败 (fd = ", fd, ")")
                /* 枚举是穷尽的：ioError 一共有六个变体（readFailed / lineTooLong /
                 * writeFailed / notATerminal / closed / destFull），少一个就编不过。 */
                notATerminal(fd) => println("io: 不是终端 (fd = ", fd, ")")
                closed(fd)       => println("io: 句柄已关 (fd = ", fd, ")")
                destFull(n)      => println("io: 目标缓冲满（", n, " 字节）")
            }
            return 1
        }
    }
}
```

**为什么读一行要包成一个 `reader`**：逐字节读是个坑 —— 老实现读 19MB / 50 万行要
**2.30s**，而整块读 **0.012s**（**190×**）。原因不是"读得慢"，是**每读一个字节都进一次内核**
⇒ 读 = **整块读（64KB）+ 内存里切**
> 还有第二半（2026-09-24）：**切的那一段也得手写** —— 逐字节调 `nextByte()` 是
> **一次方法调用一个字节**（它返回 `result` 结构体 ⇒ GCC 不肯内联）⇒
> `nextLine`/`nextToken`/`skipSpace` 的**缓冲热循环直接写进函数体**之后：
> 100 万行 **11.5 → 8.8ms** · 300 万词 **25.8 → 9.0ms**（同一个 `reader`，接口一个字没改）

| 名字 | 干什么 |
|---|---|
| `io::readerOf(fd)` | 建一个 `reader`（**隐式 64KB 缓冲**，住在创建点的块里 ⇒ 跟创建它的块同寿）|
| `r.nextInt()` | 读一个整数，**自己跳空白**（OI 式：不用先读成行再切）|
| `r.nextToken(buf)` | 读一个词（连续非空白字符）到指定 buffer |
| `r.nextLine(buf)` | 读一行到指定 buffer，**不含换行**（`\n` 被消费 ⇒ 续读位置正确）|
| `r.nextByte()` / `r.skipSpace()` / `r.eof()` | 逐字节 / 跳空白 / 结束了吗 —— **接口**，热路径不这么写 |
| `io::readSome(fd, buf)` | 从 fd 读一次，返回读到的字节数 |
| `io::writeBytes(buf)` | 把一整块字节写出去（fd 直写，**无缓冲**）|
| `io::flushOut()` / `flush()` | 把 `println` 那边的缓冲刷出去 |

**三条失败路径分得开**（P′：不能证明的，语法上必须看得见）：读到 EOF = `success(0)`
（**这不是错误**）· 行比 `out` 长 = `failure(lineTooLong)`（数据**没读全**）·
`read(2)` 出错 = `failure(readFailed)`（上次读的结果可能已经坏了）
> 老 `readLine` 把后两条**都当成 EOF** ⇒ 读错误看起来就是"文件结束了"，
> 而超长行会被**静默切成两"行"**所以它被删掉了，不是改名

分层：`std::sys::io`（**特权层 · io 族**：只有它写 `extern!` + 签字）· `std::io`（**普通库**：用 extC 写）
> `sys` 不是"一个模块"，是**一条边界**在路径上的写法 ⇒ **按族分文件** ——
> 后面还有 `std::sys::thread` / `std::sys::time` / `std::sys::net` / `std::sys::proc`
已落地：`open`/`close`（`std::fs`，定案 77）+ 归属（定案 79）+ `main(args)`

---

**[索引](README.md)** · [← 11. 还没定的](17-undecided.md) · [13. 完整示例 →](19-examples.md)
