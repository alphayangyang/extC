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
- `T` 上的 `<` / `>`（与上一行同款：模板放行，**推迟到实例化**再查这个 `T` 有没有 `fn <`）
- 泛型体能调用泛型函数了（`PLAN #50` 已解）

### 12.3 跟 C 打交道：`extern!` + 信任声明

**两条路：`extern!` 与 `std::dl`。** 都是"调 C"，区别只在**谁在什么时候把符号定下来**：

| | `extern!`（本节） | `std::dl`（§12.3.4） |
|---|---|---|
| 绑定 | **链接期**：符号在构建时就定了（库不在 ⇒ **链接失败**） | **运行期**：`dlopen` + `dlsym`，库跑起来才知道 |
| 用在哪 | libc / 系统库 / 本来就要链接的库；也是编译器发的运行期块（`extern!("extc-mem")` 那类）的声明口 | **插件**：可选的、按配置或用户选的、构建时不该依赖的库 |
| 签名 | `effects` 写在**声明**上，每个调用点按它检查 | 地址经显式转换拿到的 `fn` 值**没有签名** ⇒ 要放进**表槽**（槽上写 `effects`）才管用 |
| 代价 | 构建要能看见那个库 | 每个函数：名字 → `dlsym` → 判空 → 显式转换；`close` 要自己管 |

两条路走的是**同一套尺子**（一个机器字 / `@frozen` 按值）和**同一套 `effects` 语法**，所以从一条换到
另一条时只有"绑定方式"这一件事在变。判据一句话：**构建时就知道、也愿意依赖它 ⇒ `extern!`；跑起来才知道、或者不该依赖 ⇒ `std::dl`。**

（实测过的两个方向：`extern!("libcairo")` 会去**链接** —— 库只有 `libcairo.so.2`、没有 `.so` 和头文件
时，`gcc` 报 `undefined reference`；反过来，`dlopen` 进来的库一个符号也不能用 `extern!` 声明。
cairo 那次画图的绑定 20 个函数全是 `dlsym` + 表槽，见 [`C-ABI.md`](../topics/C-ABI.md) §9.11。）

```extc
extern!("libc") fn write(fd: i32, buf: ref u8, n: i64) -> i64
    effects Addr=0 Cont=0        // 我签字：**我不存你的指针** ⇒ 实参不受寿命约束

extern!("libc") fn fill(p: ref i32, n: i32) -> i32     // 没签字
// fill(ref x, 3)  ⇒ **编译错误**：C 可能把 `&x` 存到帧外（默认最保守）
```

四个键，都按**实参下标**（第 0 个是位 0）：`Addr=` 它存了 `&实参 i`；`Cont=` 它存了一个从实参里读出来
的指针；`Ret=0` **它返回的引用/视图不指向调用方的任何帧**；`Thread=0/1` 它碰不碰跨线程共享状态。
`Ret=0` 是"返回那一侧"的事实，也是**用户永远不用自己签**的那一条：谁能证明它，谁签一次 ——
板的四个出口在库里签了它（板是 `mmap` 来的，活到 `close`），于是

```extc
var v: mut slice<u8> = pl.alloc(n)!     // 板内视图，存进局部
c.func(v.data)                          // 交给任何没签字的 C 函数，都接受
```

而**帧内**的缓冲区（栈数组、`new`、arena）不在这个事实里：它们交给未签字的 C 仍然是编译错误 ✓
（这正是 `effects` 唯一必须存在的理由：把帧内地址**就地**交给 C 需要一句"我不留"）。带身体的
函数只能写 `Ret` 与 `Thread`（`Addr`/`Cont` 由身体自己算，写了会报错而不是静默失效）。

- 参数/返回只用**一个机器字**的东西：**标量**、**指向任何东西的指针**（`ref T` / `mut ref T` / `?ref T`，
  含 `ref void` —— C 的 `void *`），以及**函数指针** `fn(A) -> R`。指向什么不影响跨界：`ref point`、
  `ref Pair<i64, u8>`、**`ref slice<u8>`**（指向视图的**指针**，不是切片本身）都是那一个字。
  `slice<T>` **本身**在 C 那边是**两个**参数，所以它不跨界；想调"往 buffer 里写"的那种
  （`read`/`write`），传 `s.data` 和 `s.len`
- `ref void` 是**看不进去**的句柄（`mmap` 那种）：`*p` / `p[0]` 编译期挡住 —— 没有元素类型就没有大小。
  要读就得在声明里写出真正的指向类型（`ref point`）
- 结构体**按值**传/返回：要**门票** —— 声明上写 `@frozen`（下一节）。它的意思是"布局就是 C 的布局"，
  因为布局不一致是**静默错**（C 按错的偏移读写，不 trap 不报错）。指针不需要门票：一个指针就是一个
  机器字（2026-09-29 放开，见 [`C-ABI.md`](../topics/C-ABI.md) §9.6/§9.7/§9.8）
- **借出去的内存，C 能改**：`mut ref T` 交出去之后，C 可以往 `T` 的字段里写任何值 —— 包括往
  `?ref` 字段里写一个**指针**，之后 extC 会当真去解引用它。这不是类型规则变松（extC 自己的代码照旧守规则），
  而是"**可写内存借给了外来代码**"这件事的固有含义。要挡就得**只读借出**（`ref T`，或大块输入用
  只读映射），或者在签字里说清 C 不写回指针
- `owned`（来自 C 的内存由本模块负责）**尚未实现** ⇒ 会明确报错（原计划等"块拥有资源"那套；定案 79 之后口径是**显式释放**，编译器只证明能证明的泄漏）

`Thread=` 是同一套签字里的**第三项**，为并行准备（2026-09-28 新增）：

```extc
extern!("extc-runtime") fn extc_sock_read(fd: i64, buf: ref u8, n: i64) -> i64
    effects Addr=0 Cont=0 Thread=0   // 只碰实参（fd 与缓冲区）⇒ worker 里可以调用

extern!("extc-runtime") fn extc_epoll_wait(ep: i64, timeout_ms: i64) -> i64
    effects Addr=0 Cont=0 Thread=1   // 碰实例里那个待处理队列 ⇒ 不该跨线程共享
```

- `Thread=0`：这次调用**不碰任何跨线程共享的状态**（模块级 `var`、运行期缓冲、输出流）⇒ 它允许出现在 worker 里；
- `Thread=1`（**缺省**）：它碰 ⇒ 保守处理。**没有 `effects` 子句时同样按 1 处理**，与 `Addr` 那条"默认最保守"是同一条规矩；
- 取值只允许 0 或 1（`Thread=2` 会报错）：这是关于**整次调用**的一个事实，不像 `Addr`/`Cont` 那样是按实参逐位的掩码；
- 它服务的是并行那一步：worker 体内只允许调用 `Thread=0` 的东西，于是诊断能说清原因（"`fs` 用了模块级缓冲"），而不是笼统地拒绝一切。

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

### 12.5 跳过边界检查：`@unchecked`（2026-09-27 新增）

写在**函数声明前**（与 `@inline` 同族，普通函数与 struct 体内的方法都可以），表示
**这个函数体里的元素下标不再生成边界检查**：

```extc
@unchecked
fn sumBytes(v: slice<u8>) -> i64 {          // 范围由紧邻的循环条件那一行保证
    var acc: i64 = 0
    var i: i64 = 0
    while i < v.len { acc = acc + i64(v[i])  i = i + i64(1) }
    return acc
}
```

**粒度是整个函数体**，没有 `unchecked { ... }` 块：读到一个函数体之前就能看见它不在承诺之内。
覆盖的是**元素下标** `x[i]` —— 内建数组 `[N]T` 与 `slice<T>` / `mut slice<T>` 都算；
**范围** `x[a..b]` 与 `copyInto` 的那一次计数检查不受影响（那是每次调用一次，不是每字节一次）。

**代价：越界从此是未定义行为。** 语言在别处承诺的「越界 = 带位置的 trap」在这一处被显式放弃
（[0.5 内存安全](01-safety.md) 的第 5 条因此标了例外）：不再有 `trap: index 7 out of range (length 3)`
那样的消息，越界读到什么、写到什么由生成的 C 决定。这条注解与 `extern!`、`!` 同类 ——
编译器证明不了的，由写的人签字。**不能指着某一行说"这里一定在范围内"，就不该标。**

**什么时候可以用**：范围就在紧邻的一行上。标准库里的每个用点都把那一行写在注释里
（`stdlib/stl/string.extc`）：

| 用点 | 范围由哪一行保证 |
|---|---|
| `bytesEq` | `if a.len != b.len { return false }` ⇒ 两侧同长，接着 `while i < a.len` |
| `bytesLt` | 循环条件本身：`i < a.len && i < b.len` |
| `hash` | 循环条件 `i < v.len`（`v` 是函数自己取的视图，中途不变）|

**什么时候不能用**：范围靠"跨函数的不变式"或"两个字段一致"维持的时候。反例就在同一个文件里：
`string::push` 的 `self.big![self.n] = b` 依赖"载荷长度等于 `cap`"这条**跨函数**不变式 ——
若手工拼出一个 `{ cap: 8 }` 而载荷为零值切片的 `string`，带检查的下标给的是带位置的 trap，
标注之后就是写空指针。所以它**没有**标注，这是有意的。

**判据**（`tests/annot/run.sh`，四条）：

1. 同一个程序标注与不标注，输出**逐字节相同**（`sed` 现生成孪生文件，保证只差那一行）；
2. 生成物：`build/extc --no-line-map -o f.c f.extc` 之后，标注过的函数体里
   `extc_checkedIndex(...)` / `<view>_index(...)` 出现 **0 次**，同一文件里未标注的函数体里**仍在**
   （自由函数与方法各一对；把全部下标都标上时，整份生成的 C 里一条检查都不剩）；
3. 反例全部编译期报错：`extern!` 上没有身体、字段上、`struct` 上、同一函数上写两遍；
4. 写错的注解名照旧报错（注解是给编译器的指令，不接受静默忽略）。

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

### 12.3.1 `@frozen`：布局**就是** C 的布局（2026-09-29，见 [`C-ABI.md`](../topics/C-ABI.md) §9.8）

```extc
@frozen struct mixed {          // ← 一句签字，换一张"按值过 C 边界"的门票
    a: i8
    b: i64
    ok: bool
    p: mut ref i64
    arr: [4]u8
}

@frozen struct idx { quot: i64  rem: i64 }
extern!("libc") fn imaxdiv(num: i64, den: i64) -> idx    // 结构体**按值**返回，可以了
```

- **它是一句签字**：这个类型的字节布局 = C 为这份字段表排出来的布局（字段按声明顺序、按 C 的对齐与
  填充、里面**没有别的东西**）。门票是**按值过界**：参数、返回值、`fn` 签名的参数都行。没标记的结构体
  照旧过不去 —— 指针不受影响，一个指针就是一个机器字，不需要承诺布局。
- **编译器不审里面放了什么**：`slice<T>`、枚举、泛型参数都可以放，那是作者的自由，代价是 C 那边得
  照抄我们发射的形状（与 `effects` 同一类：签了字，出事算作者的）。**想传切片仍旧写 `s.data` / `s.len`**：
  视图在 C 那边是两个参数，`@frozen` 不会替它变成一个。
- **编译器欠的另一半是硬的**：它**永不重排字段、永不加隐藏字段**，并且在产物里给每个 `@frozen` 类型
  发两条**平台无关**的断言（相邻字段的 `offsetof` 递增 · `sizeof` 不超过"最后一个字段 + 它的对齐"）。
  于是将来任何一次静默漂移都变成产物**编不过**，而不是某个调用点读出错的值。
- **它管不住的那件事**：C 那边手写的结构体是不是真对得上 —— 编译器看不见对面。这一格用**镜像测试**
  验收（`tests/frozen/`）：产物与一份手写的等价 C 结构体放进同一个翻译单元，`sizeof` 与**每个字段**的
  `offsetof` 编译期相等；而且两条断言各自**有牙**（把字段顺序调换必须编不过，否则断言就是摆设）。

### 12.3.2 `@export`：把 C 符号交出去（2026-09-29，见 [`C-ABI.md`](../topics/C-ABI.md) §9.9）

`extern!` 是"我调 C"，`@export` 是**另一面**："C 调我"。

```extc
@frozen struct pair2 { lo: i64  hi: i64 }

@export fn triple(x: i64) -> i64 { return x * 3 }
@export fn makePair(lo: i64, hi: i64) -> pair2 { return pair2 { lo: lo, hi: hi } }
```

产物里这两行是**外部链接**的 C 符号，名字就是这里写的那个（模块前缀 `plugin$` 会被去掉），
所以 C 那边既可以**直接链接**，也可以 `dlsym(h, "triple")`：

```c
int64_t triple(int64_t);                       /* C 侧手写声明 */
struct pair2 { int64_t lo, hi; };              /* @frozen 让两边布局是同一个约定 */
```

- **签名要是 C 能写的**：每个参数/返回值一个机器字（标量 · 指针 · 函数指针），或者一个 `@frozen`
  结构体（按值也行）。`slice<T>` 在 C 那边是两个参数，所以不能当一个 —— 尺子与 `extern!` 是
  **同一把**（`ttCrossesC`）。
- **不能有隐藏参数**：函数体里分配且返回引用的函数会多一个 home arena 参数，C 的调用对不上 ⇒
  拒绝（要导出就让它只在自己块里分配，或者导出一个不做分配的包装）。
- **名字是契约**：C 关键字这种必须改名的名字会被拒（编译器不会悄悄改一个 C 那边猜不到的名字）；
  两个导出重名也拒（模块前缀去掉之后就是同一个符号）。
- **泛型 / 方法 / 协程 / `extern!` 不能导出**：它们没有"那一个 C 符号"（泛型每个实例一个函数体 ·
  方法的名字带接收者 · 协程的值是帧句柄 · `extern!` 已经是别人家的符号）。
- 没有 `main` 的文件也能编（模块/插件就是这样）—— `@export` 自己就是根，不会被可达性剪枝剪掉。

### 12.3.3 表槽的签名：`fn` 字段上的 `effects`（2026-09-29）

宿主在运行期把函数地址填进表里，所以"这个函数不留我的指针"这句话**没有声明可写** —— 写在**槽**上：

```extc
struct extc_heap_api {
    size:  u64
    ctx:   ?ref void
    alloc: fn(i64, ?ref void) -> ref u8 effects Addr=0 Cont=0
    free:  fn(ref u8, ?ref void) -> i32 effects Addr=0 Cont=0
}
```

- 语义与 `extern!` 上那条**完全一样**（同一把尺子）：签了字的槽，`ref` 实参可以指向**帧内**；
  没签字的槽按"它可能留住一切"算 —— 与"没签字的 `extern!`"同一档，于是帧内指针被拒。
- **签名在槽上，不在类型上**：`fn(A) -> R` 这个类型没有地方写 `effects`。所以**从槽里直接调**
  带着签名，`var g = api.alloc` 拷到局部之后就没有了（按最保守算）。要传帧内指针就从槽里调。
- `Thread=` 在字段上暂时**报错**：读它的那条检查是冲着已解析的调用去的，运行期填的槽不是。

### 12.3.4 `std::dl`：加载一个 C 模块（2026-09-29，见 [`C-ABI.md`](../topics/C-ABI.md) §9.9）

```extc
use std::dl

var h: ?ref void = dl::open("build/plugin.so")   // 字面量在 C 那边本来就是 NUL 结尾的
if h == null { /* 打不开：dlerror() 能问为什么 */ }
var raw: ?ref void = dl::sym(h, "init")
if raw == null { /* 没有这个符号 */ }
var init: fn(mut ref table) -> i32 = fn(mut ref table) -> i32(raw)   // ← 这一句是作者的签字
init(ref t)
dl::close(h)
```

- `open` / `sym` 失败都返回 `null`（要原因就自己声明 `dlerror`）。名字参数必须是 **NUL 结尾**的
  缓冲区：字符串字面量本来就满足，运行期拼出来的名字先用 `cstr` 补一个终止符 —— 规矩与
  `std::fs` 的文件名**完全一样**，理由也一样（C 的长度在终止符里，切片的长度在值里）。
- **反方向**（C 返回一个字符串）：`cstrLen(p)` 数到终止符为止（上限 `CSTR_MAX = 1024`，超了返回
  `-1`），`viewCStr(p)` 直接给一个不含终止符的视图 —— `cairo_version_string`、`dlerror` 这类
  都靠它。数长度的写法本身说明了一件事：语言里没有指针算术，所以每往前一格都要重新要一个更长的
  视图（`extc_viewOf`），代价 O(n²)，对几十字节的字符串完全够。
- `close` 收掉句柄。
- **`fn(A) -> R(ptr)` 这条转换只为 C 的 `void *` 存在，而且要求非空**：`dlsym` 回来的是
  `?ref void`，先判空（语言本来就有的空值规则），判完收窄成 `ref void`，转换才合法。
  它只有**一个方向** —— 代码指针再也变不回数据指针，要把它交给 C 就放进 `fn` 字段或
  `fn` 参数里。

### 12.3.5 一张表交给 C 模块：`@export` + `fn` 槽 + `std::dl`（2026-09-29）

三样东西合起来就是"C 模块 + 宿主"这条线（完整例子见 `tests/cabi/heapmain.extc` 与
`tests/cabi/heapmod.c`）：

<!-- manual-example: skip -->
```extc
@frozen struct heapapi {                  // 表：给 C 模块看的接口
    size: i64
    ctx: ?ref void
    alloc: fn(i64, ?ref void) -> ref u8 effects Addr=0 Cont=0   // 槽上签字
}

var plate: [256]u8                        // 板（全局 ⇒ 深度 0，借给 C 合法）
var api: heapapi = heapapi { size: i64(3), ctx: null, alloc: hostAlloc }
//          ↑ 全局的初始化器里可以写函数名：函数地址是常量

fn hostAlloc(n: i64, ctx: ?ref void) -> ref u8 {
    var off: i64 = used
    used = used + n
    return plate[off..].data               // 板内地址：视图的 data（`ref plate[i]` 不行）
}

fn main() -> i32 {
    var h: ?ref void = dl::open("plugin.so")
    var raw: ?ref void = dl::sym(h, "init")
    var init: fn(mut ref heapapi) -> ref u8 = fn(mut ref heapapi) -> ref u8(raw)
    var p: ref u8 = init(ref api)          // 模块经表回调宿主，再把板内指针还回来
    var addr: u64 = u64(p)                 // 指针 → 整数：**一次**区间检查
    if addr >= base && addr < base + u64(256) { io::cout << i64(*p) }
    return 0
}
```

> 这一块是**梗概**（省略了 `used`/`base` 的记账与函数的其余部分），所以显式跳过"手册示例必须编得过"
> 那道闸门；能编能跑的那份在 `tests/cabi/heapmain.extc`（每次 `check.sh` 都会真编真跑）。

几条边界，都是这几格新定的：

- **函数地址是常量**：全局初始化器里可以写函数名（表必须在声明处初始化，因为 `fn` 没有零值）。
- **板内的地址**用 `plate[i..].data` 取（`ref plate[i]` 不行），而且这个视图**不能先存进局部**
  （逃逸分析只看到局部，看不到它指向全局）。
- **`u64(p)` / `i64(p)` 只有一个方向**：数字变不回引用 ⇒ 指针能被验证、不能被伪造；
  `u32(p)`（会截断）仍被拒。
- **模块返回的指针必须自己检查**再解引用 —— 它不是语言担保的东西，是 C 给的。

### 12.3.6 `std::heap`：板（Heap）—— 一块连续内存 + 一道门（2026-09-29，见 [`HEAP.md`](../topics/HEAP.md)）

给外来 C 库用的一片连续内存。设计的三句话：**一次保留、按需 commit、永不搬家**；每个出口都
验一次"这个指针在板内"；寿命显式（`close` 就是 `munmap`）。

```extc
use std::heap

var pl: heap::plate = heap::plate::open(heap::DEFAULT_RESERVE)!   // 4 GiB 地址空间，不占物理页
var s: mut slice<u8> = pl.store("hello plate")!                   // 申请 + 拷进去，拿回板内视图
io::cout << pl.holds(s.data)                                      // true：一次区间比较
io::cout << pl.holds(other[..].data)                              // false：板外的不是板内的
pl.close()                                                        // munmap；之后再用板内指针就崩
```

| 名字 | 作用 |
|---|---|
| `plate` / `plate::open(n)` | 板本身 / 保留 `n` 字节地址空间（失败返回 `none`） |
| `alloc(n)` | 申请 `n` 字节，返回 `?mut slice<u8>`（板满 = `none`），commit 按 `CHUNK` 粒度 |
| `allocPtr(n)` | 同一个申请的**裸指针**出口（交给 C 的门时用这个：视图落进局部会丢掉逃逸深度） |
| `view(off, n)` | 板内 `[off, off+n)` 的视图（区间要在已 commit 的板内） |
| `loanRO(off, n)` / `loanRW(off, n)` | 只读借出 / 还回写权限（`off` 页对齐；借出期间写 ⇒ **SIGSEGV**） |
| `store(src)` | 申请 + `copyIn`，一步拿到板内视图 |
| `copyIn(dst, src)` / `copyOut(src, dst)` | 整块拷进 / 拷回；**两边都要过那道门**（不在板内返回 `-1`） |
| `view(off, n)` | 板内 `[off, off+n)` 的视图（也要过门：区间在保留区内**且已 commit**）。**把板内内存交给会留着它的 C 函数时，内联写 `pl.view(off, n)!.data`** —— 先存进局部的视图在逃逸分析眼里只有本帧寿命 |
| `holds(p)` / `holdsRange(p, n)` / `holdsView(v)` | 那道门：一次（或两次）区间比较（门函数用 `holdsRange`：`extc_viewOf` 会给调用者带上隐藏 arena，地址就不是 `fn` 值了） |
| `reservedBytes()` / `committedBytes()` / `usedBytes()` / `remaining()` / `isClosed()` | 账（读法） |
| `reserved` / `committed` / `used` / `closed` | 同上的字段（也直接读得到） |
| `close()` | `munmap`（双关安全）。**关完之后板内指针再用 = SIGSEGV**，这是设计 |
| `PAGE` / `CHUNK` / `DEFAULT_RESERVE` | 页、commit 粒度（1 MiB）、默认保留（4 GiB） |

三条要知道的边界：

- **不搬家**：`alloc` 只会往前推 `used`，地址一给出就不变 ⇒ 模块手里的板内指针一直有效（到
  `close` 为止）。这也是"老视图在新分配之后仍然可写"的原因。
- **不做世代、不做 use-after-free 检查**：那是外来库自己的事。少检验换来的是关板之后再用
  **硬缺页**（SIGSEGV），而不是悄悄读到别人的数据。
- **忘了 `close` 今天只是漏到进程结束**（`close` 方法的存在让"开了没关"有编译期提醒）；Arena
  兜底（`HEAP.md` §3）还没做。

### 12.3.7 绑一个库：`tools/cbindgen.py`（2026-09-29）

一个动态库不需要为它单独建 stdlib 模块，也不需要手写声明：读头文件，产出用到的那部分。

```sh
tools/cbindgen.py --include cairo.h -I /usr/include/cairo \
    --only cairo_paint,cairo_create,cairo_arc -o myproj/cairoapi.extc
```

产出两样东西（`tests/real-lib/cairoapi.extc` 是 cairo 的实例）：

```extc
struct api {
    handle: ?ref void
    paint: fn(?ref void) -> void              /* C: cairo_paint */
    image_surface_create_for_data: fn(?ref u8, i32, i32, i32, i32) -> ?ref void
    ...
}
let CAIRO_FORMAT_ARGB32: i32 = 0
fn open(lib: slice<u8>) -> ?api { … }         /* dlopen + 每个符号一句 dlsym + 显式转换 */
```

用法就是 `use myproj::cairoapi` 之后 `var c = cairoapi::open("libcairo.so.2")!`，然后 `c.paint(cr)`。

**工具不做的三件事**，都是刻意的：

- **不生成 `effects`**：槽不签 = 最坏情况 = **安全的那一侧**（帧内地址会被拒）。只有"把帧内地址
  **就地**交给 C"那几个函数要人签一句 `Addr=0 Cont=0`（§12.3：那是 `effects` 唯一存在的理由）。
- **不假装布局**：不认识的指针一律 `?ref void`。要读结构体字段就自己写 `@frozen` 镜像（§12.3.1），
  按类型撒谎没有任何检查会拦得住。
- **不猜**：不支持的东西（按值传的结构体、变参……）会**列出来并以非 0 退出**；枚举常量由 clang
  真求值（隐式值也不猜）。宁可不生成，也不要一个签名错的绑定。

头文件从哪来：`-dev` 包，或者不要 root 的 `apt-get download libcairo2-dev && dpkg-deb -x …`。
生成物入库，`--check` 进闸门（头文件不在的机器上显式跳过）。

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
