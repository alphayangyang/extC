# IO.md —— 输入输出现在在哪、该往哪走

> **起因**：主人说了两句话 ——
>
> > 「我在想，感觉是时候写IO了，但是具体怎么写」
> > 「读入必须要全面但是方便……而且一定要快，你知道的 cin 是一个（曾经）很缓慢的东西」
>
> 第二句**否掉了本文的第一版**：我原本把 `readLine` 定成「逐字节读」（理由是零隐藏状态），
> 那恰好就是 `cin` 的坑。本文是改过的版本。
>
> ⚠️ **本文只摆方案，不替主人拍板。** 已拍板的标 ✅，未决的标 ⬜。

---

## 0. 一句话

> **读 = 整块读 + 内存里切；写 = 内存里拼 + 整块写。**
> 编译器只给 4 个裸原语（`i64` 进 `i64` 出），方便的那一层**全部是 extC 写的**。

| 层 | 谁写 | 内容 |
|---|---|---|
| **1 · 原语** | 编译器生成（跟 arena 运行时并排） | `rawRead` / `rawWrite` / `rawOpen` / `rawClose` |
| **2 · prelude** | **extC 写**（`stdlib/prelude.extc`） | `file`、`ioError`、`readAll`、**切片解析函数族**、`reader`、`open` |
| **3 · 用户** | 主人 | 协议循环、读源文件、写生成的 C |

> ✅ **先决条件已满足（2026-09-20，第三刀）**：`option` / `result` 现在是**普通枚举**
> （`type option<T> = | none | some(T)`）⇒ **`option<slice<u8>>` 合法** ——
> 也就是「读到没有」和「一个零拷贝的视图」能装进同一个返回值里了
> （见 `examples/option-ref-payload.extc` 里那个逐行读取循环）。
> 以前这是本文的隐性障碍：`none` 的载荷没东西可填，而「ref 不可为空」是硬承诺 ✗
>
> ⬜ **剩下的门槛只有一个**：缓冲区的来源 —— `new i32[4096]`（PLAN §6 的 **A1**）。
> 有了它本文的 `reader` / `readAll` / `nextInt` 族就能按下面写的原样落地 ✓

---

## 1. 「快」到底由什么决定 —— 先把 `cin` 刨开

### `cin` 慢在哪（三条，全都不是"读得慢"）

| # | 原因 | 说明 |
|---|---|---|
| 1 | **`sync_with_stdio(true)`**（默认）| 每次抽取都要跟 C 的 `stdio` 同步 ⇒ 实际路径是**逐个字符**过 `stdio`，`cin` 自己**不缓冲** |
| 2 | **`cin.tie(&cout)`**（默认）| 每次输入前先把 `cout` flush 掉 —— 而 `cout` 的 flush 又走那个同步路径 |
| 3 | **`operator>>` 是格式化解析** | locale、`skipws` 哨兵、异常掩码、宽度……每读一个数字都要过一遍那套状态机 |

**标准解法是 `sync_with_stdio(false); cin.tie(nullptr);`** —— 快 5～10 倍。
⚠️ 但那两行是**隐藏状态的开关**：一个看不见的全局标志决定你的程序快不快。
**C++ 的解法是"关掉隐藏状态"；extC 的解法是"从来没有那个隐藏状态"** ✓

> 顺带：C 的 `scanf` 也不快（格式串**运行时解析**）；
> 真正快的一直是 `fread` / `read` **整块读**。

### 逐字节读有多贵（这就是"一定要快"的答案）

| 操作 | 数量级 |
|---|---|
| 一次 `read(2)` syscall | **~0.5–1 µs**（WSL 里还要更贵一点）|
| 内存里扫一个字节 | **~0.3 ns** |

**差 1000 倍以上。** 一个 10 万个数的输入，逐字节读 = **上亿次 syscall ≈ 100 秒**；
整块读进缓冲再在内存里切 = **0.1 秒**。

> 所以 **extC 里不提供逐字节的 `readLine`** —— 不为别的，
> **不能让"方便的那个"比"快的那个"慢 1000 倍**。C++ 提供了（`getchar` / 带同步的 `cin`），
> 于是几代 OI 选手的第一课就是"它为什么慢"。
>
> **设计目标一句话：让方便的那个就是快的那个。** ✓

### extC 的速度上限在哪

`rawRead` 就是一次裸 `read(2)`：**没有 stdio、没有 locale、没有同步、没有格式串**。
所以「整块读」这条路的上限就是 `read` 本身 —— 没有中间层可以慢。

---

## 2. 长度的类型：为什么是 `i64` 而不是 `u64` ⬜

**这是主人问的那个问题**（「返回长度为什么要用 i64 不用 u64 啊不懂，我想知道市面上怎么做的」）。

### 先看市面

| 语言 | 签名 | 长度的类型 | 错误放哪儿 |
|---|---|---|---|
| **C / POSIX** | `ssize_t read(int fd, void *buf, size_t count)` | **signed**（`ssize_t`） | **带内哨兵** `-1`（外加 `errno`）|
| **C** `fread` | `size_t fread(void *p, size_t sz, size_t n, FILE *f)` | **unsigned** | 隐藏状态：得再去问 `ferror()` |
| **C++** `istream::read` | 返回 `istream&`，字节数看 `gcount()` | signed（`streamsize`）| 流状态位 |
| **Java** | `int InputStream.read()` | **signed** | `-1` = EOF，失败抛异常 |
| **Go** | `Read(p []byte) (n int, err error)` | **signed**（`int`）| 第二个返回值 |
| **Rust** | `fn read(&mut self, buf: &mut [u8]) -> io::Result<usize>` | **unsigned**（`usize`）| `Result` 里 |
| **Zig** | `fn read(self: File, buffer: []u8) !usize` | **unsigned**（`usize`）| `!` 错误联合里 |

### 真正的规律：取决于**错误放哪儿**

- **错误用带内哨兵（`-1`）⇒ 必须 signed**。C/POSIX 的 `ssize_t` 是**被迫的**，不是设计得好 ——
  SEI CERT 专门有一条规则反对这种接口：
  [Avoid in-band error indicators while designing interfaces](https://wiki.sei.cmu.edu/confluence/pages/diffpagesbyversion.action?pageId=88026489&selectedPageVersions=41&selectedPageVersions=42)；
  [POSIX read 的签名](https://manpages.ubuntu.com/manpages/resolute/man2/read.2.html)。
- **错误搬进类型（`Result` / `!`）⇒ unsigned 才成立**（Rust、Zig）✓
- **Go 是最像 extC 的处境**：它把错误拆出去了，照理可以 unsigned，**但还是用 `int`** ——
  因为 Go 的惯例是「整数就是 `int`，除了位运算没人用 `uint`」。

### extC 的答案：`i64`（四条理由）

**① 索引和长度必须是同一个类型 —— 这条最硬。** 仓库里现在就是：

```extc
struct slice<T> { data: ..., len: i64 }        // stdlib/prelude.extc:98
extc_checkedIndex(int64_t i, int64_t n, ...)   // 生成的 C
```

**Rust 用 `usize` 的真正原因也是这个** —— `usize` 就是它的索引类型，**不是为了表达"非负"**。
extC 的索引是 `i64` ⇒ 长度若用 `u64`，`buf[0..n]` 每次都要转一次，
而 extC 的转换是**显式的**（没有 `as` 链，得写 `.checked_to_int()?`）
⇒ 每个调用点多一行噪音 + 一个失败分支，**换来一个已经知道的事实** ❌

**②「非负」不是类型该表达的东西** —— `result<i64, ioError>` 已经保证了成功路径上 `n ≥ 0`。
再用 `u64` 说一遍是**重复的不变量**，而重复的不变量里总有一个会说错。

**③ `u64` 在 extC 里的定位是「按无符号算」的类型，不是「非负的数」的类型。**

| 用 `u64` | 用 `i64` |
|---|---|
| 位板 / 掩码 / 哈希（五子棋 bitboard、fenwick）| **长度、下标、计数、偏移、返回值** |
| 想显式要 **wrap-around 语义**的算术 | 一切普通算术 |

一句话规则：**长度和下标一律 `i64`** ✓

**④ 顺带消掉一类 C 的经典 bug**：`n` unsigned、`i` signed 时 `buf.len - n` 会隐式转 unsigned，
负数变巨正数。表面统一成 `i64` ⇒ 这整类不存在（跟 P 同源：能不存在就不存在）✓

### 诚实的那一半

**如果 extC 的索引类型当初定的是 `u64`，今天就该写 `u64`。**
这不是「signed 更安全」，是**一致性**。

**但原语那层无论如何必须 signed**：`rawRead` 用 `-errno` 当哨兵（第 3 节），只能返回 `i64`。
prelude 那层把错误搬进 `result` 之后理论上可以换 `u64`，但**我们两层都用 `i64`**，
prelude 直接透传，一次转换都不做 ✓

---

## 3. 原语层：`i64` 进 `i64` 出，不许读 `errno`

```c
// 生成的 C 里，跟 extc_arena_* 并排
int64_t extc_read (int32_t fd, uint8_t *buf, int64_t n);   // <0 = -errno, 0 = EOF
int64_t extc_write(int32_t fd, const uint8_t *buf, int64_t n);
int64_t extc_open (const uint8_t *path, int64_t len, int64_t flags);
int64_t extc_close(int64_t fd);
```

**为什么不是 `fopen`**：`FILE*` 是**隐藏的堆分配 + 隐藏的缓冲区** ——
你看不见它缓冲多大、什么时候 flush、失败时把错误记在哪。这正是 P 要禁的形态 ✓
（而且 extC 的 arena 本来就自己管内存，再叠一层 stdio 缓冲毫无意义。）

**错误码必须走返回值，不许读 `errno`**：`<errno.h>` 在 BOOTSTRAP §4.3 已经被判过死刑
（「隐藏的全局状态，而且失败靠读一个全局变量正是 P 要禁的形态」）。
所以原语返回 **`-errno`** ✓

---

## 4. prelude 层：整块读 + 内存切（**全是 extC 写的**）

### 4.1 数据源两种，一个函数读完

```extc
// 读到 dest 满或 EOF；返回读到的字节数（0 = 一上来就 EOF）
fn readAll(f: file, dest: mut slice<u8>) -> result<i64, ioError>
```

```extc
// ★ OI 主力写法：整块读 + 内存里切（1 次 syscall 管 4KB）
var data: [1 << 20]u8
let n = readAll(stdin(), mut data[..])?
let input = data[0..n]

var cur: i64 = 0                  // ← 游标是**你自己的变量**，看得见
let t    = nextInt(input, mut cur)?
let a    = nextInt(input, mut cur)?
let b    = nextInt(input, mut cur)?
```

**这就是最快的写法**（任何语言里都是），而且它**零隐藏状态** ——
缓冲是你给的数组，游标是你给的 `i64`。跟 `cin` 的差别不是"我们缓冲了"，
是**我们的缓冲和游标都在源码里** ✓

### 4.2 切片解析函数族（纯 extC，可读可改）

全部落在 `slice<u8>` 上，游标一律是调用者传进来的 `mut ref i64`：

```extc
fn skipSpace(s: slice<u8>, mut i: mut ref i64) -> unit
fn atEnd    (s: slice<u8>, i: i64) -> bool

fn nextInt  (s: slice<u8>, mut i: mut ref i64) -> result<i64, ioError>   // 跳空白 + 解析
fn nextToken(s: slice<u8>, mut i: mut ref i64, out: mut slice<u8>) -> result<i64, ioError>
fn nextLine (s: slice<u8>, mut i: mut ref i64, out: mut slice<u8>) -> result<i64, ioError>
fn parseInt (s: slice<u8>) -> result<i64, ioError>          // 整个切片当一个数字
```

⚠️ **这些是 stdlib 里的普通 extC 代码**（跟 `fenwick.extc` 一样），
**主人想看就看、想改就改** —— 这跟 `cin` 那个"黑盒 + 隐藏开关"是根本区别 ✓

### 4.3 流式/协议场景：`reader`（字段全可见）

gomoku 引擎这种**一行一行来、而且不能假设能把输入全塞进内存**的场合：

```extc
struct reader {
    f:     file
    chunk: mut slice<u8>    // ← 调用者给的块缓冲，比如 [4096]u8
    len:   i64              // chunk 里有多少有效字节
    pos:   i64              // 已经消费到哪儿
}

fn readerOf(f: file, chunk: mut slice<u8>) -> reader
fn readLine(r: mut ref reader, out: mut slice<u8>) -> result<i64, ioError>   // 拷进 out ✓
fn nextIntR(r: mut ref reader) -> result<i64, ioError>
```

```extc
var chunk: [4096]u8
var line:  [256]u8
var r = readerOf(stdin(), mut chunk[..])
while true {
    let n = readLine(mut r, mut line[..])?
    if n == 0 { break }
    ...
}
```

**每次 `rawRead` 管 4KB**（一整个 chunk），不是每字节一次 syscall ✓
**行是拷进 `out` 的**，所以不存在"这个视图下次调用就失效"的隐藏约定 ——
你手上的 `line` 一直是你的 ✓

> **为什么要有 chunk 和 out 两块 buffer**：缓存块和行是两件事 ——
> 一次 `read` 可能拿回三行半，剩下的半行得**留在某个地方**，
> 那个"某个地方"就是 `r.chunk`（**看得见**）。而且它是 extC 结构体的**字段**，
> 不是藏在 `FILE*` 里的 ✓

---

### 4.4 格式化输入 ✅（主人 2026-09-18：「没有格式化输入吗……我觉得这个一定要有啊」）

**`scanf` 有两条罪**，而 extC 能把两条**同时**去掉：

| `scanf` 的毛病 | extC 怎么解 |
|---|---|
| 格式串**运行时**解析（慢 + 出错才发现）| **格式串在编译期展开**，运行时一个字符都不留 |
| 类型**不检查**（`%d` 配 `float` 是 UB）| 展开出来的就是类型化调用 ⇒ **对不上编不过** |

**A ·「格式 = 函数名」（类型驱动，今天就能有）**

```extc
let t = r.nextInt()?                    // 整数
let x = r.nextFloat()?                  // 浮点
let w = r.nextWord(mut buf[..])?        // 一个词（到空白为止）
let l = r.line(mut buf[..])?            // 一行（到 `\n` 为止）
let c = r.nextChar()?                   // 一个非空白字节
let ok = r.nextBool()?                  // true / false
```

C++ 的 `cin >> x` 就是这个形状。**它已经是"格式化输入"了** ——
只不过**格式不是字符串，是函数名** ⇒ 编译期知道类型、运行时零解析 ✓

**B ·「格式串在编译期展开」（更漂亮，以后加）**

```extc
struct date { y: i32  m: i32  d: i32 }

let today = r.next(date, "{i32}-{i32}-{i32}")?
// 编译期就展开成：
//   today.y = nextInt(self)?   需要 '-'     today.m = nextInt(self)?   ...
```

`scanf` 真正值钱的是**一眼看清整个布局**（`"%d-%d-%d"`），extC 把这个留下，
但**格式串在编译期就消失了** ⇒ 零运行时解析、类型不匹配编不过 ✓
这就是 P 那句口号（*凡是编译期能证明的，运行时不留痕迹*）用在输入上。

> ⭐ **而且对称**：extC 的输出**已经是**编译期检查的格式了 ——
> `println("a = ", x)` 里每个参数的类型编译器都查过。
> **输入当然也该有同一档的东西。** ✓

⚠️ B 需要「编译期读一个字符串字面量」的能力（现在没有）。
好消息是它**不影响 A 的形状** —— A 就是 B 的展开结果，B 只是把那些调用写得更短 ✓

> **为什么"行"必须调用者给地方**（主人 2026-09-18 拍板：「lines 肯定是我给地方，
> 这是允许的，也是合理的，总比拷贝好」）：
> **字节总得落在某处**。你给 = 看得见；它给 = 藏起来（Go 的 `Scanner.Text()` 是拷贝）。
> 而且主流全都是"你给"：C `fgets(buf, n, f)`、C++ `getline(cin, s)`、Rust `read_line(&mut s)` ✓
> **`nextInt` 这种返回数字的不用给** —— 数字是值，拷一份就行。

---

## 5. 文件归属：**帧拥有它** ✅（主人 2026-09-18 拍了「1 的话感觉不赖」）

```extc
fn readSource(path: slice<u8>, dest: mut slice<u8>) -> result<i64, ioError> {
    let f = open(path)?          // f: mut ref file
    return readAll(f, dest)      // 函数返回 ⇒ 自动关 ✓
}
```

**`open()` 返回 `mut ref file`，那个 ref 指向帧 arena 里的一个槽位 ⇒ 深度 1**
（跟 `alloc<T>(n)` 一模一样）。帧对象跟 arena 并排：

```c
int main(void) {
    extc_arena __extc_arena; extc_arena_init(&__extc_arena);
    extc_files __extc_files; extc_files_init(&__extc_files);   // ← 新增
    ...
    extc_files_release(&__extc_files);    // 先关文件
    extc_arena_release(&__extc_arena);    // 再放内存（顺序不能反）
    return 0;
}
```

### 为什么它**零新规则**

不动逃逸检查一行 —— 第 1 步刚修的规则正好接住所有洗白路径：

| 想干什么 | 谁拦住 |
|---|---|
| `fn openHere() -> mut ref file { let f = open("x")?  return f }` | 老规则：深度 1 > 0 ❌ |
| `fn fwd(f: mut ref file) -> mut ref file { return f }` | ✅ 合法（**转发是好的**）|
| `fn launder() -> mut ref file { return fwd(open("x")?) }` | **第 1 步的 `max(实参)` 规则** ❌ |
| `fn stash(f: mut ref file) { G = f }` | **第 1 步的「借来的值不能存进深度 0」** ❌ |

> 这是 `alloc` 那个机制的**第二次使用**：「**帧拥有内存（arena）和资源（文件），
> 两者都在返回时死，两者都是深度 1，同一个检查器管**」✓

### 代价

| 代价 | 说明 |
|---|---|
| **fd 要等到函数返回才放**（上限 1024）| 写法上就是「**一个函数读一个文件**」—— 恰好是自举要的形状 |
| 循环里开 500 个文件不写 helper ⇒ 攒 fd | 真需要时 `close(f)!` 是现成的口子 |

### 备选（没选，但记下来）

| 方案 | 好在哪 | 差在哪 |
|---|---|---|
| `with f = open(p) { ... }` 块 | 寿命精确到块，循环里开文件不攒 fd | 多一条语法 + 一个新作用域种类；它**就是** `defer` |
| 显式 `close(f)` | 最快，像 C | 「没有手动分配就没有手动释放」破了；**忘关是很自然的** |
| **`f.close()!` 只作逃生舱** | 两个好处都有一点 | `!` = 我签字：提前关 = 可能 use-after-close（fd 号会复用）⇒ 那确实是运行时后果 ✅ |

> 建议 **IO-2 再加 `close(f)!`**，IO-0/IO-1 不加（P：不做没人用的特性）✓

---

## 6. `print` / `println`：不缓冲，而且是**故意的**

- **不可失败**：往 stdout 写失败（`| head` 之类）基本等于进程要死了（SIGPIPE）；
  让每个 `println` 都写 `?` 是纯噪音。
- **不缓冲**：一次 `println` = 一次 `write(2)`。
  - 终端场景**正是要这样**（立刻看见），缓冲反而碍事；
  - 要刷量（几十万行）就**在内存里拼完，一次 `writeAll`** —— 这也是最快的：
    ```extc
    var buf: [1 << 16]u8
    var len: i64 = 0
    // ……往 buf 里 append，满了就 writeAll(stdout(), buf[0..len])?
    ```
  - **不搞"看不见的输出缓冲"**：那会偷偷改变"什么时候能看见输出"（崩了以后还剩多少、
    跟 stderr 怎么交错）—— 那正是 P 要禁的隐藏语义。C 的 `printf` 有这种缓冲，
    大家忍了；extC 让**谁来缓冲**变成调用者的选择 ✓

---

## 7. argv：`fn main(args: slice<slice<u8>>) -> i32`

**不做 `args()` 内建** —— 它**返回不了**：数组得放进调用者的帧 arena ⇒ 深度 1 ⇒
「不能返回」这条自己就挡住了 ✓ 而 argv 本来就是 main 的参数：

```extc
fn main(args: slice<slice<u8>>) -> i32 {
    if args.len < 2 { println("用法: prog <文件>")  return 1 }
    let path = args[1]
    ...
}
```

生成 C 侧 `int main(void)` → `int main(int argc, char **argv)`，
再把 argc/argv 包成 view 数组放进 main 自己的帧 arena（约 10 行）✓
多入口（定案 6）时每个入口自己声明，没有魔法 ✓ 不想要就写 `fn main() -> i32` ✓

---

## 8. 顺序（三段，全是加法）

| 段 | 内容 | 做完能干什么 | 大小 |
|---|---|---|---|
| **IO-0** | 原语 `rawRead`/`rawWrite` + `file`/`ioError` + **`readAll`** + **切片解析函数族**（`nextInt`/`nextToken`/`nextLine`/`skipSpace`）+ `eprintln` | **OI 式输入**能用了；gomoku 引擎能读协议（`reader` 也在这一段或下一段）| ~150 行 |
| **IO-1** | `open` + 帧拥有的 `extc_files` + `readAll(f, …)` + `reader` + `main(args)` | 自举的门槛（读源文件、写生成的 C）| ~120 行 |
| **IO-2** | `exit(code)`、`close(f)!`、`writer`（可见缓冲）、termios raw mode | TUI + 刷量输出 | 中 |

> BOOTSTRAP §4.3 已经定过 TUI 那条：**不包 ncurses**，只要「读一个字节 + 开关 raw mode」
> 那么小的原语 ✓

---

## 9. 未决问题 ⬜

| # | 问题 | 摆着的选项 |
|---|---|---|
| 1 | ~~**长度的类型**~~ ✅ **`i64`**（**2026-09-22 主人拍板**，见 `DECISIONS.md` 定案 69：跟索引 / `slice.len` 同类型、**负数有意义**（失败/EOF 那条路走得通）⇒ 少一堆显式转换 ✓ 代价 = 丢了"长度不可能为负"那层静态保证，靠运行时检查 + trap 兜 ✓）| —— |
| 2 | **原语的名字** | `rawRead` / `readRaw` / `sysRead` / 干脆 `read` |
| 3 | **`readAll` 读满之后剩的怎么办** | 本文选「返回读到的字节数，剩下的还在 fd 里，再调一次」（零状态）；另一条是 `readExactly`（读不满就报错）—— 两个都要吗 |
| 4 | **格式化输入 B（编译期格式串）什么时候做** | 现在就跟 A 一起规划接口形状 vs 先只做 A（`nextInt` 那一族），B 记为「以后」|
| 5 | **`{ }` 块要不要变成释放点** | 现在是「帧 = 释放点」；块级释放要 arena 支持 mark/release |
| 6 | **`reader` 的缓冲谁给** ✅ | 主人 2026-09-18：`readerOf(stdin())` 不用给（内部 `alloc` 4KB），行缓冲 `line(mut buf[..])` **必须给** |

### 已经拍板的（不用再想）

| 事项 | 结论 |
|---|---|
| 行缓冲谁给 | **调用者给**（「总比拷贝好」）✓ |
| `reader` 是什么 | **一个普通 struct + 一大堆方法**，底下全部调同一套 `rawRead` ✓ |
| 格式化输入 | **一定要有**（见 4.4：A 今天、B 以后）✓ |
| 文件归属 | **帧拥有**（第 5 节）✓ |
| `readAll` + 游标的定位 | **榨性能专用**，不是日常写法（日常是 `reader`）✓ |
| `return failure(e)` / `success(...)` | **能裸写了**（定案 49）—— 因为主人说「result 很神秘，我不太会用」✓ |
| `scan(mut a, mut b, ...)` | **要，但走「真变参」，而且现在不写**（主人：细节多、工程量大，`nextInt` 那一族现在够用）✓ |
| 长度 `i64` vs `u64` | ✅ **定了 `i64`**（2026-09-22，定案 69）—— 第 2 节的市面对照仍然值得读 ✓ |

> ⏸ **IO 暂时停在这里**：设计都在本文，代码一行没写。
> 主人 2026-09-18：「然后后面 IO 我想再讨论」。
