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
> ✅ **那个门槛早就跨过了**（2026-09-23 复核）：`new i32[4096]` / `new [N]T` / `new T[n]` 都在
> （PLAN §6 的 **A1** = 定案 56「`new` 永远是零 · 分配进当前块」）✓
> ⇒ 现在 IO 的缺口**不是"造不出 buffer"**，而是 `open`/块拥有文件 + `nextInt` 一族 + `reader`
> （见 §8 顺序表与 [`PLAN.md`](../../docs/PLAN.md) §1 主线的现状）✓
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
| 一次 `read(2)` syscall | **~0.13 µs**（2026-09-23 实测：page-cache 热的时候）|
| 内存里扫一个字节 | **~0.3 ns** |

**差几百倍。** 2026-09-23 实测（12MB / 100 万行，`-O2`，两者都读满）：

| 读法 | 时间 | syscall 次数 |
|---|---|---|
| 逐字节（老 `readLine`） | **1.444 s** | 1200 万 |
| 一次 64KB（`reader`） | **0.032 s** | ~183 |

⇒ **45×** ✓ 而且差距全在 `sys` 时间上（1.060s → 0.008s）—— 正是"每字节进一次内核"的代价 ✓

> 所以 **extC 里不提供逐字节的 `readLine`** —— 不为别的，
> **不能让"方便的那个"比"快的那个"慢 1000 倍**。C++ 提供了（`getchar` / 带同步的 `cin`），
> 于是几代 OI 选手的第一课就是"它为什么慢"。
>
> **设计目标一句话：让方便的那个就是快的那个。** ✓

### extC 的速度上限在哪

`rawRead` 就是一次裸 `read(2)`：**没有 stdio、没有 locale、没有同步、没有格式串**。
所以「整块读」这条路的上限就是 `read` 本身 —— 没有中间层可以慢。

---

## 2. 长度的类型：为什么是 `i64` 而不是 `u64` ✅ **已拍板（定案 69）**

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

✅ **已落地（定案 74）** —— 形状跟下面写的完全一致：

```extc
struct reader {
    fd:    i32
    chunk: mut slice<u8>    // ← **64KB，`readerOf` 自己给**（不是调用者给 ✓）
    len:   i64              // chunk 里有多少有效字节
    pos:   i64              // 已经消费到哪儿

    fn nextLine(self: mut ref reader, out: mut slice<u8>) -> result<i64, ioError>
    fn nextToken(self: mut ref reader, out: mut slice<u8>) -> result<i64, ioError>
    fn nextInt(self: mut ref reader) -> result<i64, ioError>
    fn skipSpace(self: mut ref reader) -> result<i64, ioError>
}

fn readerOf(fd: i32) -> reader
```

```extc
var line: [256]u8
var r = readerOf(io::STDIN_FD)
while true {
    let n = r.nextLine(line[..])?
    if n == 0 { break }              // 0 = EOF（**不是错误** ✓）
    ...
}
```

**每次 `read(2)` 管 64KB**（一整个 chunk），不是每字节一次 syscall ✓
**行缓冲仍然调用者给**（拷进 `out` ⇒ 你手上的 `line` 一直是你的 ✓）——
"块缓冲隐式给"跟"行缓冲调用者给"**不矛盾**：前者是**实现细节**（一次读多大），
后者是**数据归属**（那行字节归谁）✓
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

## 5. 文件归属：**块拥有它**（2026-09-24 定稿；本节原来写的是"帧拥有"）

```extc
fn readSource(path: slice<u8>, dest: mut slice<u8>) -> result<i64, ioError> {
    let f = fs::openRead(path)?      // 句柄的槽位在**当前块**里
    return readAll(f, dest)          // 块退出（含 return）⇒ 自动关 ✓
}
```

**归属 = 词法块**（帧只是最外层块）—— 跟内存**同一套规则、同一个深度数**：句柄住在
`__extc_fd[当前块深度]`，块退出时**先注销资源、再放内存**：

```c
extc_arena __extc_a[DEPTH] = {0};    /* 内存：职责不变，一个字不改 */
extc_fd    __extc_fd[DEPTH] = {0};   /* 内核句柄：与 arena **兄弟**，不归它管 */
...
extc_fd_release(&__extc_fd[3]);      /* 先注销资源 */
extc_arena_release(&__extc_a[3]);    /* 再放内存 */
```

⚠️ **为什么 fd 表挂在块上，而不是让 arena 顺带管资源**（2026-09-24 讨论定的）：
让 arena 长出"资源管理"这个第二职责 ⇒ 下一个 socket、再下一个线程句柄都会顺手挂上去
⇒ 核心抽象被稀释 ✗ ⇒ **arena 只放内存，fd 表是它的兄弟**：各自一行释放调用，
顺序（先资源后内存）写死在生成代码里 ✓

### 三条释放路径（同一个句柄，各管一段，不重叠）

| 路径 | 什么时候用 | 错误谁管 |
|---|---|---|
| **块退出（隐式兜底）** | 默认。忘了关也不会漏 fd ✓ | 忽略（所以**写型必须配 `commit()?`**）|
| **`f.commit()?`**（写型专有）| 我要确认数据**安全落地** | **显式报错**：POSIX 的 `close(2)` 正是"延迟写错误"（EIO / 配额 / NFS）冒出来的地方 ✓ |
| **`close(f)!`** | 我就要**现在**放掉（更细的粒度）| **签字**：我保证它现在开着、且之后不再用它 ✓ |

（`!` 跟 `a[i]!` / `extern!` 同构：**我签字，接受运行时的后果** ✓）

### 两条必须钉死的细节

1. **幂等**：被 `close(f)!` 关过的槽位**必须标记**，块退出时不再关第二次 ✗✗
   否则 double close 关掉的是**别人刚开的 fd**（fd 号会被内核复用 ⇒ 这是真会伤人的一类 bug）。
2. **关闭后使用**：`close(f)!` 之后再用 `f.put(..)`：
   - **IO-1 先做运行时**：槽位带状态 ⇒ **trap，带源码位置**（不静默 ✓）
   - **将来做编译期**：让 `close` **消费掉**句柄（affine）⇒ 用就编不过 ✓
     （那就是"唯一所有权 / 转移"那套机制的第一个使用场景 ✓）

### 失败模式

`openRead` / `openWrite` / `openAppend` 一律返回 `result<..., ioError>`：
`EMFILE`（fd 耗尽，上限 1024）· 权限 · 路径不存在 ⇒ **`failure` 带位置**，不许 trap、不许静默 ✓

### 两条惯用法

1. **一个函数读一个文件**（最常用）：`open` 放函数开头 ⇒ 返回自动关 ✓
2. **批处理**（循环里逐个处理）：
   - 就地包一个块：`{ let f = fs::openRead(p)? ... }` ⇒ **出块即关**，fd 恒为 O(1) ✓
   - 逻辑稍大就写成 helper：每帧一个释放点，同样是 O(1) ✓

### 判据：什么资源**才配**由块退出自动释放（是判据，不是清单）

三条**同时**满足才行：**① 释放不会失败（没有信息可报）· ② 释放没有顺序 / 协议语义 ·
③ 只由标准库少数原语获取**。

| 资源 | ① | ② | ③ | 结论 |
|---|---|---|---|---|
| fd（读）| ✓ | ✓ | ✓ | **自动关** |
| fd（写）| ⚠️ `close` 可报延迟写错误 | ✓ | ✓ | **自动关 + 必须配 `commit()?`** |
| mmap（若将来做）| ✓ | ✓ | ✓ | 可以（它本来就算内存那一家）|
| socket | ✗ 发送缓冲的延迟错误 | ✗ **半关 / FIN / lingering 是协议语义** | ✓ | **永远显式** |
| 线程 / 进程句柄 | — | ✗ **join 是同步点** | ✓ | **永远显式** |

⚠️ 写成**判据**而不是清单：清单会被人往后加，判据不会 ✓

### 备选（没选，但记下来）

| 方案 | 好在哪 | 差在哪 |
|---|---|---|
| `with f = open(p) { ... }` 块 | 寿命精确到块 | 多一条语法 + 一个新作用域种类（那就是 `defer`）；而"块拥有"已经免费拿到它 ✓ |
| 原方案：**帧拥有**（一条 `extc_files` 链）| 概念最少（只有一条链）| **fd 要等到函数返回才放**；循环里就地 open/close 写不出来 ✗ |
| 只有显式 `close(f)!`，没有隐式兜底 | 最快，像 C | 「没有手动分配就没有手动释放」破了；**忘关是很自然的** ✗ |
| 让 arena 顺带管资源 | 少一条链 | 核心抽象长出第二职责 ⇒ socket / 线程会跟着挂上来 ✗ |

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

> ⭐ **2026-09-23 规范已定（定案 77；实现还没动手）**：主人指出「**open 不够清晰，
> 因为我不知道打开的是读还是写**」✗ ⇒ 定成
> `fs::openRead(p) -> inputFile` · `fs::openWrite(p) -> outputFile`（**截断**）·
> `fs::openAppend(p) -> outputFile`（**追加**）✓
> ⭐ **读型 / 写型是两个 struct** ⇒ "把写型当读型用"是**编译期错误** ✓
> ⚠️ `O_*` 与 POSIX 的数**只许出现在 `std::sys::io`**（本文 §1 那张分层表里）
> ⇒ `file` 拆成 `inputFile` / `outputFile`，并且**挂在 `std::fs`**（不是 prelude ✓）
> 规范全文 [`SYNTAX.md`](../../docs/SYNTAX.md) §3′ · 常设验收 `tests/fs-shape/` ✓
>
> ⚠️ **状态口径（2026-09-23）**：本文里的"进度/状态"表**可能滞后** —— "还剩什么"的权威只看 [`PLAN.md`](../../docs/PLAN.md) §0.4（缺陷清单）+ §1（主线），或直接跑 `examples/` 实测 ✓

| 段 | 内容 | 做完能干什么 | 进度（2026-09-23 实测） |
|---|---|---|---|
| **IO-0** | 原语 `read`/`write` + **`reader`** + **切片解析函数族** + `ioError` | **OI 式输入**能用了；gomoku 能读协议 | 🟢 **主体已落地（定案 74，2026-09-23）** —— 原语（`stdlib/std/sys/io.extc`，`extern!` 签字）+ **`reader`（隐式 64KB）** + **`nextLine`/`nextToken`/`nextInt`/`skipSpace`** + `ioError` 两条 + `writeBytes`/`flushOut`/`flush()` ✓ 验收 `tests/io/`（7 条，含**三条路**与**分块读性能**）⬜ **还欠**：`readAll`（读满一个大 buffer）· 格式化输入 B |
| **IO-1** | `open` + **块拥有**的 `extc_fd[DEPTH]`（+ `commit()?` / `close(f)!`）+ `readAll(f, …)` + `reader` + `main(args)` | 自举的门槛（读源文件、写生成的 C） | ⬜ **没有**
| **IO-2** | `exit(code)`、`close(f)!`、termios raw mode | TUI + 刷量输出 | 🟡 **一半**：**`writer`（可见缓冲）已落地**（`stdlib/std/io.extc`，验收 `tests/io/` 的 writer-file ✓）—— ⚠️ 这一格原来写着"`writer` ⬜ 没有"，那是在它落地**之前**写的，没跟着改 ✗（2026-09-24 修正）；剩下的 `exit(code)` / `close(f)!` / termios ⬜ **没有** |

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
| 6 | ~~**`reader` 的缓冲谁给**~~ ✅ **已定案（定案 74，2026-09-23 落地）** | **块缓冲隐式给**（`readerOf(fd)` 内部 `new u8[65536]`，落到调用点的 arena ⇒ 跟 reader 同寿 ✓）；**行缓冲调用者给**（拷进 `out` ✓）。⚠️ 本文 §4.3 原来写的是"调用者给块"，跟这里**自相矛盾** —— 已按后者统一 ✓ |

### 已经拍板的（不用再想）

| 事项 | 结论 |
|---|---|
| 行缓冲谁给 | **调用者给**（「总比拷贝好」）✓ |
| `reader` 是什么 | **一个普通 struct + 一大堆方法**，底下全部调同一套 `rawRead` ✓ |
| 格式化输入 | **一定要有**（见 4.4：A 今天、B 以后）✓ |
| 文件归属 | **块拥有**（第 5 节 · 定案 78）✓ |
| `readAll` + 游标的定位 | **榨性能专用**，不是日常写法（日常是 `reader`）✓ |
| `return failure(e)` / `success(...)` | **能裸写了**（定案 49）—— 因为主人说「result 很神秘，我不太会用」✓ |
| `scan(mut a, mut b, ...)` | **要，但走「真变参」，而且现在不写**（主人：细节多、工程量大，`nextInt` 那一族现在够用）✓ |
| 长度 `i64` vs `u64` | ✅ **定了 `i64`**（2026-09-22，定案 69）—— 第 2 节的市面对照仍然值得读 ✓ |

> ⭐ **2026-09-22：第一块落地了**（定案 73）—— `stdlib/std/sys/io.extc`（原语 + 签字）+
> `stdlib/std/io.extc`（普通库：`readLine` / `writeBytes` / `flushOut`）+ 内建 `flush()` ✓
> 用户程序 `use std::io` 就能从 **stdin** 读了（`tests/io/` 是常设验收 ✓）
> ⏸ **剩下的**（`open`/`close` + **块拥有文件** · `main(args)`）还没做 ——
> 其中"块拥有文件"跟 `extern!` 的 `owned` 是**同一个前置** ✓
> 主人 2026-09-18：「然后后面 IO 我想再讨论」。
