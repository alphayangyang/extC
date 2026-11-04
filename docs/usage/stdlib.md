# 标准库现状

本文只写**现在有什么 / 怎么用**：每一句都能指到**源码位置**或**一条能跑的判据**。
待做不在这里（见 [`../TODO.md`](../TODO.md)），历史不在这里（见 [`../CHANGELOG.md`](../CHANGELOG.md)）。

## 目录（导入路径）

| 你写 | 得到什么 |
|---|---|
| `use std::io` | 控制台与文件 I/O（**门面**：实现分在 `std/io.extc`、`std/io/fmt.extc`、`std/io/err.extc`） |
| `use std::data::map` | 有序映射容器 |
| `use std::hash::digest` | SHA-256 |
| `use std::coro::scheduler` | 协程调度器 |

`use a::b` 找的就是文件 `a/b.extc`；搜索顺序是**入口文件所在目录 → 每个 `-I <dir>` → 标准库目录**
（标准库目录 = `$EXTC_STD`，没设就是 `extc` 可执行文件旁边的 `../stdlib`）。
出错时编译器把这些候选原样列出来。

**一个文件一个模块**：文件名默认就是模块名，文件第一行可以写 `module a::b` 改它的
**身份**（生成 C 的前缀随之变），路径仍只负责找文件。目录本身不是模块：

```text
$ extc --build -o /dev/null std-d-e6.extc        # 文件里写 `use alpha2`，磁盘上只有 alpha2/inner.extc
error: cannot find module `alpha2` (imported by std-d-e6.extc)
note:  `use a::b` looks for a file `a/b.extc`. Searched:
        ./alpha2.extc
        /home/alphayang/extC_Compiler/build/../stdlib/alpha2.extc
```

## 1. prelude：不用 `use` 就能用

prelude 是编译器在每个编译单元前先过一遍的一段 extC 源码。
下面这些名字**不写任何 `use`** 就能用：

| 名字 | 是什么 |
|---|---|
| `unit` | 空载荷：`result<unit, E>` 的"成功但没有值"那一格 |
| `option<T>` | `\| none \| some(T)`；零值 = `none`；取值的路只有 `match` 与 `?` |
| `result<T, E>` | `\| failure(E) \| success(T)`；零值 = `failure(E 的零值)` |
| `slice<T>` | `{ data: ref T, len: i64 }`；`s[i]` 是语言语法，越界带位置 trap；`hasAt(i)` 只问边界、不取值 |
| `varArray<T>` | arena 底的动态数组（见下） |
| `pcg32` | 显式状态的随机数引擎：`pcg32::seeded(42)` · `pcg32::withStream(seed, stream)` · `next()` · `nextBounded(bound)`（0 到 bound−1，用拒绝采样避开取模偏置）|
| `nextFloat()` / `shuffle(a)` | 0.0–1.0 的浮点、i32 视图原地洗牌（都是生成器的方法：`r.nextFloat()`）|
| `coroutine<A, B>` | 协程的**接口标记**（帧由编译器按协程函数合成，不要拿它当变量类型） |
| `domain` | 域的**接口标记**，方法由编译器合成，库侧在 `std::sys::domain` |

`print(x)` / `println(x)` 是内建，可以随便用：任何可打印值都行（`string` 按字节写出），
与 `io::cout` 共用同一个输出缓冲，混写顺序与源码一致。要"带条件的流"（`cerr`、`stringio`、
自定义 `ostream`）时用 `io::cout` 那一套。

### `varArray<T>`：arena 底的动态数组

| 方法 | 签名 |
|---|---|
| `withCap` | `fn withCap(cap: i64) -> varArray<T>` |
| `size` / `isEmpty` | `fn size(self: ref varArray<T>) -> i64` / `-> bool` | 、|
| `push` / `pop` | `fn push(self: mut ref varArray<T>, v: T)` / `fn pop(...) -> ?T` |
| `get` / `set` | `fn get(self: ref varArray<T>, i: i64) -> ?T` / `fn set(self: mut ref varArray<T>, i: i64, v: T)` |
| `clear` | `fn clear(self: mut ref varArray<T>)` |
| `view` / `asMutSlice` | `fn view(...) -> slice<T>` / `-> mut slice<T>` |
| `==` | 按内容比较 |

关键语义（都在  的注释里，逐条有位置）：

- **谁拥有**：没有自己的 arena；扩容时 `new T[cap*2]` 分配在**拥有 `self` 的那个 arena**。
- **能不能装引用**：能。 把 `varArray` 点名为"唯一能装引用的容器（arena 底 ⇒ 元素地址永不移动/复用）"。
- **容量怎么涨**：满时翻倍（下限 4）。
- **越界**：`get` 越界给 `none`（不 trap）；`set` 只在 `i` 超过 `cap` 时 trap，`i` 落在 `[len, cap)` 会写进未用槽。
- **扩容会让元素引用失效**：`v.push(..)` 换新块，持有旧 `mut ref` 再写是**静默丢**（旧内存还在，所以不崩）（；判据 ）。
- **`clear()` 只把 `len` 置 0**，内存不还（没有 `shrink_to_fit`）。

```extc
use std::io
fn main() -> i32 {
    var v: varArray<i32> = varArray<i32>::withCap(2)
    v.push(7)
    v.push(8)
    v.push(9)
    io::cout << v.size() << " " << (v.get(0) ?? -1) << "\n"
    return 0
}
```
实测 `extc --run`：`3 7`。

## 2. `stl::*`：容器

容器的**存储字段**（`vector.vals`、`map.vals`/`map.depth`、`hashMap.vals`、`pool.vals`）是公开的，因为
子系统代码（协程调度器的任务行、`hashSet` 的存活位、`stringio` 的窗口）要按位置读写；它们**不是 API**：
宽度、布局、语义都可能随实现变化，用户代码按容器的方法写。

导入名就是 `stl::<文件名>`：`use std::data::vector`、`use std::data::hashMap`、`use std::text::string`……
这一族里的容器大多带 `@sharesStorage` + `@place`（旧拼写 `@poolObject`）：
**按值拷贝两份会共用同一块板块**，编译器会为此报警告；要独立副本必须显式 `clone()`
（判据  / `clone_ok.extc`，见 ）。

这些容器的存储是它们**自己的板块**，不是帧里的一段：换缓冲时旧块当场归还，所以"反复建、反复丢"是
稳态内存。三种存储（帧、板块、对象表）各自适合什么、以及选错的代价，见[内存模型](../MEMORY-MODEL.md)。

### `varArray` / `vector`：两个动态数组

| | `varArray<T>`（prelude） | `vector<T>`（`std::data::vector`） |
|---|---|
| 存储来源 | arena 切片 | **自己的板块**（[内存模型](../MEMORY-MODEL.md)） |
| 寻址 | 下标 | 下标（`len` / `capacity` / `get` / `set`） |
| 容量怎么涨 | 翻倍（下限 4） | **1.5 倍、原地长** |
| 能不能装引用 | 能（ 点名） | 存储是池的板块，取内容只经 `snapshot()` 拷贝 |
| 取内容 | `view()` / `asMutSlice()` 借出视图 | `snapshot()` **拷一份**（没有零拷贝视图：`push`/`clear`/`shrink`/`release` 任一动作都能让它失效，） |
| 深拷贝 | 没有 `clone`；`==` 按内容比较 | `clone()` |
| 收尾 | 无 | `shrink()` / `release()`（含世代守卫，） |
| 迭代顺序 | 插入序 | 插入序（dense 连续） |

`vector` 的方法：`withCap` · `new` · `len` · `capacity` ·
`isEmpty` · `get`（给 `option<T>`） · `set`（越界返回 `false`，不 trap） · `snapshot` ·
`clone` · `push` · `pop` · `shrink` · `clear` · `release`。
`vector::from(xs)` 从一段视图拷出一个容器。

```extc
use std::io
use std::data::vector
fn main() -> i32 {
    var v: vector<i64> = vector<i64>::new()
    v.push(i64(3))  v.push(i64(1))  v.push(i64(2))
    let s = v.snapshot()
    io::cout << "vector n=" << v.len() << " cap=" << v.capacity() << " sum=" << (s[0] + s[1] + s[2]) << "\n"
    return 0
}
```
实测：`vector n=3 cap=4 sum=6`。

### `std::data::place`：`@place` 的公共面

一个标了 `@place` 的类型说："我的存储住在**一个地方**（今天是一个池），那个地方比我活得久"。
地方本身由运行期注册表管；这个模块只暴露它的**非视图**部分：

| 成员 | 作用 |
|---|---|
| `generation(p)` | 地方的世代（每次重置 +1）：容器把它与 id 一起存，还块前比对 |
| `bytes(p)` | 一个地方当前占着的字节数（判"长了但没留旧块"）|
| `liveBytes()` | 所有地方占着的字节数（判"退出后有没有漏"就比它进出两次）|
| `give(p, s)` | 把一块借自地方的字节还回去（`poolGive<T>` 的薄包装）|
| `free(p)` | 当场还掉一个地方（正常路径靠所在域退出整批收回，一般不用）|

**建池不在这个模块里，返回视图的原语也不在。** 两条都是刻意的：函数体会为池分配开一层自己的
地方，池于是随它返回而死，所以建池必须发生在你自己的 `@place` 类型的构造函数里（关联函数不另开
地方，`@inline` 也不改变这一点）；而 `poolSlice<T>` / `poolSliceRaw<T>` / `poolResize<T>` /
`poolResizeRaw<T>` / `poolGive<T>` 是**编译器内建** —— C 里声明不出"返回 `slice<T>`"，且包一层
普通函数会丢掉"这个视图是这个类型自己那个地方的"这条知识（赋给字段时被逃逸检查拒绝）。

```extc
use std::io
use std::data::place as place
use std::sys::pool as syspool

@place
struct grow {
    @private vals: mut slice<i64>
    @private n:    i64
    @private cap:  i64
    @private pid:  i64
    fn withCap(c: i64) -> grow {
        let pid: i64 = syspool::extc_pool_new(-1)
        var g: grow = { vals: poolSliceRaw<i64>(pid, c), n: 0, cap: c, pid: pid }
        return g
    }
    fn push(self: mut ref grow, v: i64) {
        if self.n == self.cap {
            let nc: i64 = self.cap + self.cap / 2 + 1
            self.vals = poolResizeRaw<i64>(self.pid, self.vals, nc)
            self.cap = nc
        }
        self.vals[self.n] = v
        self.n = self.n + 1
    }
}

fn main() -> i32 {
    var g: grow = grow::withCap(2)
    var i: i64 = 0
    while i < 100 { g.push(i)  i = i + 1 }
    io::cout << "n=" << g.n << " cap=" << g.cap << " bytes=" << place::bytes(g.pid) << "\n"
    return 0
}
```
实测：`n=100 cap=139 bytes=1112`（当前板块 139 × 8 字节，加上板块头；四次增长的旧块都还回去了）。

### `buf<T>`：**拥有**存储的可增长缓冲区

`vector<T>` 与 `buf<T>` 用同一套板块，区别只在**所有权怎么表达**：

| | `vector<T>` | `buf<T>`（`std::data::buf`） |
|---|---|---|
| 标了 `@noCopy` | 否 | **是**（值位置出现时**转移**，不是拷贝） |
| 谁拥有板块 | 值的一份拷贝看不到"谁拥有"，两个名字共享同一块板 | 转移语义保证**同时只有一个名字** |
| 离开作用域 | 不自动交还（要显式 `release()`） | **自动交还**（`@drop`） |
| 覆盖赋值 | 旧板留着（可能泄漏） | **旧板当场还回去** |
| 增长 | 1.5 倍、原地长 | 1.5 倍、原地长（旧块当场还） |

方法：`withCap(c)` · `len()` · `capacity()` · `push(v)` · `pop()`（`?T`） · `view()` / `viewMut()` ·
`clear()` · `clone()`（复制出一份独立存储）。

它标了 `@buffer`，所以 `new T[n]` 在 `buf<T>` 的注解上下文里就是它的构造函数：

<!-- fragment -->
```extc
use std::data::buf
var b: buf<i64> = new i64[4]      // 与 buf<i64>::withCap(4) 同一件事
b.push(7)
let s = b.view()
```

判据（`tests/stl/buf.extc`、`tests/stl/buf_rss.extc`）：8e6 个 i64 的增长峰值 **64 MB**，
同一形状走 `varArray`（arena 底）是 **130 MB** —— 前者是"一份"，后者是"历代之和"；
块退出后 `extc_pool_bytesLive()` 回到进入前的数。

### 自己写一个板块底的容器

`vector` 不是特例：任何一个标了 `@place` 的结构体都**拥有一块自己的地方（池）**，于是也能拿到"原地长、乱序归还"的存储。写自己的可增长容器就是四件事 —— 拥有、要、长、守卫与归还：

<!-- fragment -->
```extc
use std::sys::pool as syspool

@place
struct grow<T> {
    vals:   mut slice<T>
    n:      i64
    cap:    i64
    pid:    i64        /* 池的下标 */
    pidGen: u64        /* 建池时的世代号，用来认出陈旧拷贝 */

    /* ① 拥有：建一块自己的池，记下它的世代号 */
    fn withCap(c: i64) -> grow<T> {
        let rp = syspool::extc_pool_new(-1)
        let rpg = syspool::extc_pool_generation(rp)
        var cc: i64 = c
        if cc < 4 { cc = 4 }
        return grow<T> { vals: poolSliceRaw<T>(rp, cc), n: 0, cap: cc, pid: rp, pidGen: rpg }
    }

    /* ② 长：容量不够时向同一个池原地加长，1.5 倍 */
    fn push(self: mut ref grow<T>, v: T) {
        if self.n == self.cap {
            let ncap: i64 = self.cap + self.cap / 2
            self.vals = poolResizeRaw<T>(self.pid, self.vals, ncap)
            self.cap = ncap
        }
        self.vals[self.n] = v
        self.n = self.n + 1
    }

    /* ③ 守卫：这一份还指着自己的池吗？（别名 + release 之后必须能判出来） */
    fn stale(self: ref grow<T>) -> bool {
        if self.pid < 0 { return false }
        return syspool::extc_pool_generation(self.pid) != self.pidGen
    }

    /* ④ 归还：先收成 0 长度，再重置并丢弃池 */
    fn release(self: mut ref grow<T>) {
        if self.pid < 0 { return }
        if self.stale() { self.pid = -1  return }
        self.vals = poolResize<T>(self.pid, self.vals, 0)
        self.cap = 0
        syspool::extc_pool_reset(self.pid)
        syspool::extc_pool_drop(self.pid)
        self.pid = -1
    }
}
```

四步写全了，它就和内置容器一样是 O(活跃量)：手写的 800 万个 `i64` 实测峰值 **64 MB**，与 `vector` 相同。少任何一步的代价写在[内存模型](../../MEMORY-MODEL.md)里 —— 没有守卫是静默的悬垂写，不做原地长就退回"每轮新增一份、留到块退出"。


### `string` + `stringio`：连续字节串与流式读写

`string` 是 **SSO 字节串**：内联 16 字节，写满第 17 个字节才升级到池
；`cap == 0` 就是"还在内联"。零值合法且零分配。

| 方法 | 签名 |
|---|---|
| 构造 | `string::create(c)` · 模块级 `string::withCap(c)` / `new()` / `empty()` / `from(b)` / `fromI64(n)` |
| 读数 | `len` · `capacity` · `isEmpty` · `at(i) -> ?u8` |
| 取内容 | `snapshot() -> slice<u8>`（**拷一份**）、`subView(lo, hi) -> slice<u8>`、`sub(lo, hi)` |
| 拷贝 | `clone()`（内联态出来的仍是内联串） |
| 比较 | `==` / `<`，右边可以是 `string` 也可以是 `slice<u8>`（所以 `s == "abc"` 成立） |
| 拼接 | `concat(other) -> string`、`concatBytes(slice<u8>)`、就地 `append(slice<u8>)`、`push(u8)`、`appendByte` |
| 运算符 | `a + b`：返回**新值**的拼接。右操作数可以是 `slice<u8>`、`string`、整数（按十进制）、`f64`/`f32`（6 位小数，`stringio` 里）或 `bool`；要求 `a` 是 `string`。分配落在调用点的 arena 上，可以在函数里拼完返回 |
| 追加数字 | `appendI64(v)`；`appendF64(x, prec)`（在 `stringio` 里，定点 `prec` 位小数，上限 20）。拼一行报表少一次临时 `string` |
| 借出视图 | `view()`：只读字节视图，**不拷贝**；收 `slice<u8>` 的形参因此可以直接传 `string`（自动借出）。要独立拷贝仍写 `snapshot()` |
| 直接打印 | `io::cout << s`（`string` 的 `<<` 重载在 `std::text::string` 里，`use std::text::string` 就够，不用写 `s.snapshot()`） |
| 查找 | `find(needle) -> ?i64`（无匹配 `none`）、`contains`、`startsWith`、`endsWith`、`hash()` |
| 容量 | `grow()`（1.5 倍）、`shrink()`、`clear()`、`release()`、`removeFront(n)` |
| 解析 | `string::toI64(b) -> ?i64`；`string::toF64(b) -> ?f64`（整数/小数/指数写法，至少一位数字；不接受 `.5`、`inf`、`nan`、十六进制） |
| 切分 | `string::split(s: slice<u8>, sep: slice<u8>) -> varArray<slice<u8>>`（每段是 `s` 的零拷贝视图；空 `sep` 返回空数组；末尾空段也入列 ⇒ 段数 = 出现次数 + 1）|
| 拼接 | `string::join(parts: slice<slice<u8>>, sep: slice<u8>) -> string`（空数组给空串；`sep` 可以是空视图；一次按总长分配）|
| 裁剪 | `string::trim(s)` / `trimStart(s)` / `trimEnd(s)`：去掉两端 ASCII 空白（空格、`\t`、`\n`、`\r`、`\v`、`\f`），返回**原缓冲视图**（零拷贝）|
| 比两段字节 | `bytesEq(a, b)` / `bytesLt(a, b)`（模块级关联函数，直接比两段 `slice<u8>`）|

**拿到内容只有 `snapshot()` 一条路（拷贝）**：池里不放视图出去。
`string` 的 `hash()` + `==` 齐了，所以它能直接当 `hashMap` 的键。

`std::text::stringio` 用 `impl` 块把流式读写挂到 `string` 与 `io::istream` / `fs::ifstream` 上
（`io::cout << s` 的重载挂在 `std::text::string` 自己身上，所以打印只需要 `use std::text::string`，
这里列的是读与格式化）：

| 名字 | 签名 |
|---|---|
| `string::readLineFrom` | `fn readLineFrom(self: mut ref string, r: mut ref io::reader) -> result<i64, io::ioError>` |
| `string::readAllFrom` | `fn readAllFrom(self: mut ref string, r: mut ref io::reader) -> result<i64, io::ioError>` |
| `string::writeTo` / `writeLineTo` | `fn writeTo(self: ref string, w: mut ref io::writer) -> result<i64, io::ioError>` |
| `io::istream >> string` / `fs::ifstream >> string` | 读一行（`\n` 与 CRLF 的 `\r` 都不算内容），**目标被替换** |
| `string::readLine()` | `fn readLine() -> ?string`：从控制台读一行（自带 `io::inSync()` 的取还，读完调 `r.save()` 把控制台保存点挪到读到的位置；行尾 CRLF 不算内容；空行是 `some("")`，**EOF 才给 `none`**）|
| `string::readAll()` | `fn readAll() -> ?string`：把控制台剩下的输入一次读完（错误给 `none`）|
| `string::fromF64` | `fn fromF64(x: f64, prec: i64) -> string`：定点格式化成字符串（`prec` 位小数、四舍五入；`prec` 上限 20）|
| `string::padText` | `fn padText(v: slice<u8>, width: i64, fill: u8, align: i64) -> string`：按字节宽度排版（`align` 用 `io::LEFT`/`io::RIGHT`/`io::CENTER`；宽度不够不截断）—— 值 f-string 的 `{s:>8}` 走它 |
| `string::padI64` | `fn padI64(v: i64, width: i64, fill: u8, align: i64) -> string`：整数排版；`0` 填充时符号在前（`-5` 宽 4 ⇒ `-005`）—— 值 f-string 的 `{n:>4}` / `{n:03}` 走它 |
| `string::padF64` | `fn padF64(x: f64, width: i64, prec: i64, fill: u8, align: i64) -> string`：定点 + 排版（`prec` 上限 20）—— 值 f-string 的 `{x:8.2f}` 走它 |

```extc
use std::io
use std::fs
use std::text::string

fn main() -> i32 {
    var r: fs::ifstream = fs::openRead("std-e-in.txt")?     // 内容 "alpha\nbeta\n"
    var s: string
    let n1 = s.readLineFrom(r.reader())?
    let n2 = s.readAllFrom(r.reader())?
    r.close()?
    io::cout << "n1=" << n1 << " n2=" << n2 << " s=" << s.snapshot() << "\n"
    return 0
}
```
实测：`n1=5 n2=5 s=alphabeta`（第一行不含换行）。

> 实测的坑：同一个程序里 `use std::data::linmap` + `use std::text::string`，并且真的实例化
> `linMap<i64, slice<u8>>`（`slice<u8>` 做值）时，生成的 C 里 `option_slice_u8` 会**重复定义**，
> C 编译失败。最小复现见本页末尾"今天会咬人的组合"第 3 条。

### `map` / `set`：**有序**（B+ 树）

裸名给**有序**实现：`map` / `set` 有序，`hashMap` / `hashSet` 哈希。

`map<K, V>` 是 ORDER（16 键/节点）的 B+ 树，叶子按 `next` 串成链，所以
**按序遍历就是扫连续内存**；`cnt` 让 `keyAt` / `valAt` / `lowerBound` 变成 O(log n) 的排名查询。**有序性来自键类型的 `<`**，`==` 由 `!(a<b) && !(b<a)` 派生。

| 方法 | 签名 |
|---|---|
| `withCap` / `new` | `fn withCap(want: i64) -> map<K, V>` / `fn new() -> map<K, V>` |
| `len` / `capacity` 之类 | `len` · `nodeCount` · `capOf` · `height` |
| `contains` / `get` / `[]` | `fn get(...) -> ?V`；缺键给 `none`（**不插入默认值**） |
| `put` / `[]=` | `fn put(self: mut ref map<K, V>, k: K, v: V) -> bool`；**true = 键本来就在**（覆盖） |
| `remove` | 返回是否删掉了；会借/并再平衡 |
| 排名 | `lowerBound(k)`（严格小于 k 的个数）· `keyAt(i)` / `valAt(i)`（第 i 小） · `rankAt(i)` |
| 两端 | `firstKey()` / `lastKey() -> ?K` |
| `clone` / `clear` / `shrink` / `release` | 深拷贝、清空、缩高水位、整批还池 |

`set<T>` 就是"值丢掉"的 `map<T, u8>`：**一个实现，不会漂**。`withCap` · `new` · `len` · `contains` · `put`（**true = 新元素**） ·
`remove` · `lowerBound` · `first` / `last` · `at(i)`（第 i 小） · `clone` · `clear`。

```extc
use std::io
use std::data::map
fn main() -> i32 {
    var m: map<i64, i64> = map<i64, i64>::new()
    m.put(i64(2), i64(20))  m.put(i64(1), i64(10))
    io::cout << "map len=" << m.len() << " first=" << (m.firstKey() ?? -1) << " at0=" << m.keyAt(i64(0)) << "\n"
    return 0
}
```
实测：`map len=2 first=1 at0=1`。

```extc
use std::io
use std::data::map
fn main() -> i32 {
    var s: map<i64, bool> = map<i64, bool>::withCap(i64(4))
    s.put(i64(4), true)
    s.put(i64(2), true)
    io::cout << "lb3=" << s.lowerBound(i64(3)) << " has2=" << s.contains(i64(2)) << "\n"
    return 0
}
```
实测：`lb3=1 has2=true`。

### `hashMap` / `hashSet`：哈希

**键固定是 `i64`**：extC 没有 trait，编译器不能在类型参数上叫 `hash()`，所以哈希容器的键是具体的
`i64`，值才是泛型参数 `V`。键里可以打包两个 `i32`、一个 `pool` 句柄、
一个字符串表偏移。

模块级还有一个 `hashI64(k)`：`i64` 的哈希函数，`hashMapI64` 的键就用它。

`hashMapI64<V>` 建在泛型 `hashMap<i64Key, V>` 上；实现是
**线性探测 + 墓碑**，`cap` 是 2 的幂，探测下标是 `hash & (cap-1)`。

| 方法 | 签名 |
|---|---|
| `withCap` | `fn withCap(want: i64) -> hashMapI64<V>` |
| `len` / `capOf` / `deadOf` | `fn len(...) -> i64` 等 |
| `contains` / `get` / `[]` | `fn get(...) -> ?V` |
| `put` / `[]=` | `fn put(self: mut ref hashMapI64<V>, k: i64, v: V) -> bool`；**true = 键本来就在** |
| `remove` / `clear` / `release` | 删、清、整批还池 |
| 按桶扫 | `keyAt(i)` / `valAt(i)` / `liveAt(i)`（表位置；**不是插入序**） |
| 按 dense 扫 | `denseLen()` / `keyAtDense(d)` / `valAtDense(d)` |
| 按槽读写 | `slotOf(k) -> ?handle`（一次探测）/ `at(h) -> ?V` / `setAt(h, v)` / `entry(k, dflt) -> handle` |
| `clone` | 深拷贝 |
| `rebuild(ncap)` | 换一张 `ncap` 容量（2 的幂）的表，旧块当场还池 |

重建有**两条触发线**：装载（`n + dead` 到 70% 容量）与墓碑密度（`dead` 到 25% 容量）。

**槽（handle）用来携带"身份"，不是循环内的快速通道**。`slotOf` / `at` / `setAt` / `entry` 让
一次探测换出多次按槽访问，省掉重复的哈希；但每次按槽读都要回读槽数组并校验换代号与纪，所以
**热循环里直接用 `get` / `put`（或 `valAtDense`）更快**：`get` 探测一次后就拿 `slot[b]` 去读
dense 值列，同一条记录一次探测加一次索引，比"拿 handle 再 `at`/`setAt`"少两三次随机 load。
handle 的用处是让身份离开循环（存进数组、跨语句复用、跨扩容），它不是指针：值池扩容搬数据不会
让它悬垂，键被 `remove`（swap-remove，搬最后一个元素）或池清掉之后 `at` 还 `none`，而不是读到
别人的值 —— 这件事由池的换代检查兜住。

<!-- fragment -->
```extc
use std::io
use std::data::hashMap
fn bump(m: mut ref hashMapI64<i64>, k: i64) -> i64 {
    let h: handle = m.entry(k, i64(0))     // 身份要跨语句：一次探测换出槽
    let v: i64 = m.at(h) ?? i64(0)
    m.setAt(h, v + i64(1))                 // 按槽写回，不再探测
    return v + i64(1)
}
```


`hashSetI64` 是"没有值的 map"（值是一格 `u8`），面与 `hashMap` 对齐，
**唯一差别**：`put` 返回 true 表示**新元素**。`withCap` · `len` ·
`contains` · `put` · `remove` · `clear` · `release` · `keyAt` · `liveAt` · `capOf` · `clone`。

```extc
use std::io
use std::data::hashMap
fn main() -> i32 {
    var m: hashMapI64<i64> = hashMapI64<i64>::withCap(i64(4))
    let had0 = m.put(i64(9), i64(90))
    let had1 = m.put(i64(9), i64(91))
    let v = m.get(i64(9)) ?? -1
    io::cout << "had0=" << had0 << " had1=" << had1 << " v=" << v << "\n"
    return 0
}
```
实测：`had0=false had1=true v=91`。

```extc
use std::io
use std::data::hashMap
fn main() -> i32 {
    var s: hashSetI64 = hashSetI64::withCap(i64(4))
    let a = s.put(i64(5))
    let b = s.put(i64(5))
    io::cout << "new=" << a << " again=" << b << " len=" << s.len() << "\n"
    return 0
}
```
实测：`new=true again=false len=1`。

### `linMap` / `linSet`：线性（只要求键上的 `==`）

键不是整数、表又小的时候用它：查找是 O(n) 次 `==`，**不需要 hash**（类型参数上的 `==` 支持，
`hash` 不支持）。存储 **dense 且是插入序**；`remove` 是
**swap-remove**（最后一个填洞）⇒ 删过之后顺序不再保持。

| 方法 | 签名 |
|---|---|
| `withCap` | `fn withCap(want: i64) -> linMap<K, V>` |
| `len` / `capOf` | `fn len(...) -> i64` / `-> i64` |
| `find` / `contains` | `fn find(...) -> i64`（位置，`<0` = 没有） / `-> bool` |
| `get` / `[]` | `fn get(...) -> ?V` |
| `put` / `[]=` | 插入或覆盖；返回 true = 键本来就在 |
| `remove` | swap-remove；返回是否删掉 |
| `keyAt(i)` / `valAt(i)` | 第 i 个 entry（插入序） |
| `clone` / `clear` / `grow` / `release` | |

`linSet<T>` 是 `linMap<T, unit>` 的薄包装：`withCap` · `add`（返回是否**新加**） ·
`contains` · `remove` · `len` · `clone` · `clear` · `release` · `at(i)`。

```extc
use std::io
use std::data::linmap
fn main() -> i32 {
    var m: linMap<i64, i64> = linMap<i64, i64>::withCap(i64(4))
    m.put(i64(2), i64(20))
    m.put(i64(1), i64(10))
    io::cout << "n=" << m.len() << " k0=" << m.keyAt(i64(0)) << " v0=" << m.valAt(i64(0)) << " has2=" << m.contains(i64(2)) << "\n"
    return 0
}
```
实测：`n=2 k0=2 v0=20 has2=true`（插入序）。

```extc
use std::io
use std::data::linmap
fn main() -> i32 {
    var s: linSet<i64> = linSet<i64>::withCap(i64(4))
    s.add(i64(7))
    io::cout << "has=" << s.contains(i64(7)) << " dup=" << s.add(i64(7)) << " len=" << s.len() << " at0=" << s.at(i64(0)) << "\n"
    return 0
}
```
实测：`has=true dup=false len=1 at0=7`。

### `pool<T>`：槽回收工具（多数程序不需要）

这一层不是"容器的默认底座"，而是**槽回收**：给一批元素分配稳定的槽位、允许删除、并让句柄在槽被
复用之后**读不到**而不是读到别人。容器（`vector` / `string` / `hashMap` / `map` / `set`）用不到它 ——
它们的存储是自己的板块，迭代就是扫 dense 连续内存。什么时候才值得用 `pool`：

- 元素**有删有插**，而且调用方要长期记住"哪一个"（句柄代替指针，扩容搬数据不会悬垂）；
- 需要一个**稠密连续**的前缀来做批量遍历，同时允许中间删除（删除走 swap-remove，前缀不破）。

不值得用的场合也很明确：只在循环里读改写一段连续数据（用视图或容器的 dense 访问）、只是想动态
分配一批对象（用 `dyn` 或直接 `new`）。三种存储的分工见[内存模型](../MEMORY-MODEL.md)。

值住在一个 **dense 连续**数组里，调用方拿的是**句柄**（`slot + generation + epoch`），不是指针。**陈旧句柄不 trap**：查询式 API 给 `?T` / `bool`
（，`at(h)` 那种 trap 版要等 `@trackCaller`）。

| 名字 | 签名 |
|---|---|
| `denseLen` / `keyAtDense` / `valAtDense` | 按**插入序**（dense 前缀）遍历：`fn denseLen(self) -> i64` · `fn keyAtDense(self, d: i64) -> K` · `fn valAtDense(self, d: i64) -> V` |
| `handle` | `struct handle { slot: i32, gen: i32, ep: i64 }`（普通值，拷贝无害） |
| `same(a, b)` | `fn same(a: handle, b: handle) -> bool` |
| `withCap` / `withParent` | `fn withCap(n: i64) -> pool<T>` / `fn withParent(parent: i64, n: i64)` |
| `len` / `capacity` / `bytesOf` | |
| `contains` / `get` / `set` | `fn get(...) -> ?T`；陈旧给 `none`；`set` 返回是否写成功 |
| `insert` | `fn insert(self: mut ref pool<T>, v: T) -> handle`（优先复用洞链） |
| `remove` | 返回是否删掉；**swap-remove**（dense 前缀保持连续）+ 该槽世代 +1 |
| 迭代 | `atDense(i)` / `setAtDense(i, v)` / `handleAtDense(i)` / `snapshot()`（拷一份） / `gather(hs, out)` |
| `shrink` / `clear` / `release` | `clear` 翻 epoch（旧句柄一次比较全废），`release` 整批还 |

```extc
use std::io
use std::data::pool
fn main() -> i32 {
    var p: pool<i64> = pool<i64>::withCap(i64(4))
    let h = p.insert(i64(42))
    let g = p.get(h) ?? -1
    let d = p.atDense(i64(0))
    let rm = p.remove(h)
    io::cout << "get=" << g << " dense=" << d << " rm=" << rm << " live-after=" << p.contains(h) << "\n"
    return 0
}
```
实测：`get=42 dense=42 rm=true live-after=false`。

### `sort`：原地比较排序

| 名字 | 签名 |
|---|---|
| `sort<T>` | `fn sort<T>(a: mut slice<T>)` |
| `isSorted<T>` | `fn isSorted<T>(a: slice<T>) -> bool` |

语义：introsort（median-of-three + 深度上限后退到 heapsort），**原地、零分配**，栈是 64 项定长数组；
小区间（≤ 12）走插入排序；**不承诺稳定**；比较用键上的 `<`。
调用点写 `seq::sort(v)`，**不要**写 `seq::sort<i64>(v)`（那会被读成"类型当值"，报
`` `i64` is a type, not a value -- did you mean `i64(x)`? ``）。

```extc
use std::io
use std::data::seq
fn main() -> i32 {
    var a: [5]i64 = [3, 1, 4, 1, 5]
    var v: mut slice<i64> = a[..]
    seq::sort(v)
    io::cout << a[0] << a[1] << a[2] << a[3] << a[4] << " sorted=" << seq::isSorted(v) << "\n"
    return 0
}
```
实测：`11345 sorted=true`。

### `seq`：序列归约

| 名字 | 签名 |
|---|---|
| `sum<T>` | `fn sum<T>(a: slice<T>) -> ?T` |
| `max<T>` | `fn max<T>(a: slice<T>) -> ?T` |
| `min<T>` | `fn min<T>(a: slice<T>) -> ?T` |
| `count<T>` | `fn count<T>(a: slice<T>, v: T) -> i64` |
| `sumBlocksI64` | `fn sumBlocksI64(a: slice<i64>, parts: mut slice<i64>) -> ?i64` |
| `maxBlocksF64` | `fn maxBlocksF64(a: slice<f64>, parts: mut slice<f64>) -> ?f64` |
| `minBlocksF64` | `fn minBlocksF64(a: slice<f64>, parts: mut slice<f64>) -> ?f64` |

语义：只读、**零分配**、顺序就是视图顺序（`sum` 的浮点累加也按从左到右，结果可复现）；
空视图返回 `none`（`count` 返回 0）—— 库不替你决定"空的时候算多少"，用 `??` 或 `match` 接。
`max` / `min` 用 `<` / `>`，相等时保留靠前的那个；`count` 用 `==`。
这些函数只碰 `Thread=0` 的数据，可以原样放进 `parallel::run` 的 worker 里。

`*Blocks*` 三兄弟是**固定分块的并行归约**：`a` 均分成 `parts.len` 段，`par` 让每段在自己的
worker 上算局部结果写进 `parts[b]`，再按段序串行合并。前置条件 `parts.len <= a.len`（每段至少
一个元素），不满足时返回 `none`。只有"结合后与串行逐位相同"的归约才在此列：整数加法（模算术）、
`f64` 的 `max`/`min`（取极值）。**浮点 `sum` 不在**——分块改变加法结合顺序，浮点加法不结合，
结果不能同时保证逐位相同；浮点求和仍用串行 `sum` 或"先算进数组再 `sum`"。典型用法：

```extc
use std::io
use std::data::seq

fn main() -> i32 {
    var a: mut slice<i64> = new i64[i64(4096)]
    var parts: mut slice<i64> = new i64[i64(8)]
    for i in 0..i64(4096) { a[i] = i }
    let s = seq::sumBlocksI64(a, parts) ?? i64(0)
    io::cout << f"s={s}\n"
    return 0
}
```

```extc
use std::io
use std::data::seq

fn main() -> i32 {
    var xs: [4]f64 = [1.0, 2.0, 3.0, 4.5]
    let v: slice<f64> = xs[..]
    let s = seq::sum(v) ?? f64(0.0)
    let mx = seq::max(v) ?? f64(0.0)
    let mn = seq::min(v) ?? f64(0.0)
    io::cout << "sum=" << i64(s * 1000000.0) << " max=" << i64(mx * 1000000.0) << " min=" << i64(mn * 1000000.0) << " c=" << seq::count(v, f64(3.0)) << "\n"
    return 0
}
```
实测：`sum=10500000 max=4500000 min=1000000 c=1`。

### `tensor`：二维 strided 视图（张量的最小一等公民）

| 名字 | 签名 |
|---|---|
| `tensor2<T>` | 结构体：`data: mut slice<T>`、`off`、`rows`、`cols`、`rs`、`cs`（步长以元素为单位） |
| `fromFlat<T>` | `fn fromFlat<T>(data: mut slice<T>, rows: i64, cols: i64) -> ?tensor2<T>` |
| `fromStrided<T>` | `fn fromStrided<T>(data: mut slice<T>, off: i64, rows: i64, cols: i64, rs: i64, cs: i64) -> ?tensor2<T>` |
| `empty<T>` | `fn empty<T>(data: mut slice<T>) -> tensor2<T>` |
| `nRows` / `nCols` / `rowStride` / `colStride` | `fn …(self: ref tensor2<T>) -> i64` |
| `get` / `set` | `fn get(self: ref tensor2<T>, r: i64, c: i64) -> ?T` · `fn set(self: mut ref tensor2<T>, r: i64, c: i64, v: T) -> bool` |
| `fill` | `fn fill(self: mut ref tensor2<T>, v: T)` |
| `sum` | `fn sum(self: ref tensor2<T>) -> ?T`（行主序：r 递增再 c 递增） |
| `transpose` | `fn transpose(self: mut ref tensor2<T>) -> tensor2<T>`（零拷贝：换形状与步长） |
| `subview` | `fn subview(self: mut ref tensor2<T>, r0: i64, c0: i64, rn: i64, cn: i64) -> ?tensor2<T>`（零拷贝：挪起点） |
| `copyFrom` / `addScaled` | `fn copyFrom(self: mut ref tensor2<T>, src: tensor2<T>) -> bool` · `fn addScaled(self: mut ref tensor2<T>, x: tensor2<T>, k: T) -> bool` |

**它是视图，不是容器**：一个 `tensor2<T>` 只有「底层存储 + 起点 + 形状 + 两个步长」，拷贝张量拷贝的是
这几个数（视图按设计共享存储）。元素下标是 `off + r*rs + c*cs`，所以连续矩阵、转置、子矩阵、步长视图
是同一个类型、都不复制数据。构造失败（形状为负、装不下、步长为负、最远元素越界）返回 `none` —— 库不 trap；
`empty` 是给 `??` 用的空视图兜底值。读在 `ref` 上、写在 `mut ref` 上；`fromFlat`/`fromStrided` 收
`mut slice<T>`（要写的张量必须从可写视图建）。表达式位置要写**不带模块前缀**的类型名
（`use std::nd::tensor` 之后 `tensor2<i64>::fromFlat(...)`），这是当前解析器的形状。

```extc
use std::io
use std::nd

fn main() -> i32 {
    var buf: mut slice<i64> = new i64[i64(12)]
    for i in 0..i64(12) { buf[i] = i }
    var m = tensor2<i64>::fromFlat(buf, i64(3), i64(4)) ?? tensor2<i64>::empty(buf)
    let g = m.get(i64(1), i64(2)) ?? i64(-1)
    m.set(i64(1), i64(2), i64(99))
    let s = m.sum() ?? i64(-1)
    var tr = m.transpose()
    let ts = tr.sum() ?? i64(-1)
    var sub = m.subview(i64(1), i64(1), i64(2), i64(3)) ?? tensor2<i64>::empty(buf)
    let ss = sub.sum() ?? i64(-1)
    io::cout << "g=" << g << " s=" << s << " ts=" << ts << " ss=" << ss << "\n"
    return 0
}
```

实测：`g=6 s=159 ts=159 ss=141`。细节见 `stdlib/stl/tensor.extc` 与 `tests/stl/tensor.extc`。

### `nd`：N 维 strided 视图（一个 buffer + 每维步长）

| 名字 | 签名 |
|---|---|
| `ndview<T>` | 结构体：`data: mut slice<T>`、`off`、`rank`、`shape: [8]i64`、`strides: [8]i64`（元素为单位） |
| `ndarray<T>` | 拥有存储的结构体：`data: mut slice<T>`、`rank`、`shape: [8]i64`；`zeros1/zeros2/zeros3/zeros4`、`allocate(rank, shape)`、`empty()` 造它 |
| `view` / `viewMut` | `fn view(self: ref ndarray<T>) -> ndview<T>` · `fn viewMut(self: mut ref ndarray<T>) -> ndview<T>`（零拷贝） |
| `rowMajor<T>` | `fn rowMajor<T>(data: mut slice<T>, rank: i64, shape: [8]i64) -> ndview<T>` |
| `strided<T>` | `fn strided<T>(data: mut slice<T>, off: i64, rank: i64, shape: [8]i64, strides: [8]i64) -> ?ndview<T>` |
| `count` / `isContig` | `fn count(self: ref ndview<T>) -> i64` · `fn isContig(self: ref ndview<T>) -> bool` |
| 逐元素 | `fn filled<T>(v, x)` · `addInto<T>(dst, a, b)` · `subInto<T>` · `mulInto<T>` · `scaleInto<T>(dst, a, k)` · `axpyInto<T>(dst, a, b, k)`（`dst = a + k*b`，全部写到目标、形状不一致返回 `false`） |
| `sumv<T>` | `fn sumv<T>(v: ndview<T>) -> ?T`（行主序：idx[0] 变化最慢；空视图 `none`） |
| `swapDims<T>` / `sub<T>` | `fn swapDims<T>(v, i, j) -> ?ndview<T>`（换维，零拷贝）· `fn sub<T>(v, i) -> ?ndview<T>`（取第 0 维的第 i 片，降一维） |
| `at2<T>` / `set2<T>` / `slice2<T>` | `fn at2<T>(v, i, j) -> T`（读一个元素；维度不足/越界 trap）· `fn set2<T>(v, i, j, x: T)`（写一个元素）· `fn slice2<T>(v, r0, r1, c0, c1) -> ?ndview<T>`（二维子矩形，零拷贝；越界或空区间 `none`）。**下标糖** `v[i, j]` / `v[r0..r1, c0..c1]` 就直译到这两个函数 |

| `at3<T>` / `set3<T>` | `fn at3<T>(v, i, j, k) -> T`（读三维视图一个元素；rank 不足或越界 trap）· `fn set3<T>(v, i, j, k, x: T)`（写一个元素）。**三下标糖** `v[i, j, k]` / `v[i, j, k] = x` / `v[i, j, k] += x` 直译到它们 |

**两下标糖**：`v[i, j]` 读一个元素（等价于 `nd::at2(v, i, j)`，越界 trap），
`v[i, j] = x` 写一个元素（等价于 `nd::set2(v, i, j, x)`；`v[i, j] += x` 一类复合赋值等价于
`nd::set2(v, i, j, nd::at2(v, i, j) + x)`），
`v[r0..r1, c0..c1]` 取一个二维子视图（等价于 `nd::slice2(...)`，越界给 `none`）。
糖只是把这三种写法直译成库函数调用 —— 没有新的语法节点，也没有新的运行期形状，所以转置、步长视图
（`nd::swapDims` 的结果）上同样能用。两下标要求"两个下标"或"两个区间"，一标一区间在编译期就报错。

**三下标糖**：`v[i, j, k]` 读、`v[i, j, k] = x` 写、`v[i, j, k] += x` 复合写都直译成 `nd::at3` /
`nd::set3`（规则与两下标完全相同，只是多一维）。超过三个下标仍报错（先切一个子视图再来）。

**直接写在 `ndarray` 上**：两/三下标糖的接收者可以是 `ndarray<T>` 本身，不必先
`var g: ndview<T> = grid.viewMut()` —— 编译器按需借出视图（读借 `view()`、写借 `viewMut()`），
可写性仍由借用检查把关（`let` 的 `ndarray` 写会报 `cannot assign to …`）。区间形式要兜底时
仍用视图：`nd::sumv(grid[0..1, 0..3] ?? grid.view())`。

**为什么不是 `slice<slice<T>>`**：嵌套切片每层是另一块 buffer 或指向别处的描述符，遍历时每行跳一次地址，
预取器跟不住。这里是一块 buffer + 每维步长，行主序时最内层步长为 1，N 维遍历在内存里仍是一条流；
`swapDims`、`sub`、切片都只改描述符。`rank` 是描述符里的运行期值（上限 8），所以三维、四维不需要整数泛型；
代价是跨函数边界后下标个数由运行期检查。库不 trap：rank 越界、形状为负、装不下返回 `none`。

```extc
use std::io
use std::nd

fn main() -> i32 {
    var a: ndarray<f64> = ndarray<f64>::zeros3(i64(2), i64(3), i64(4)) ?? ndarray<f64>::empty()
    var v: ndview<f64> = a.viewMut()
    nd::filled(v, f64(1.5))
    var b: ndarray<f64> = ndarray<f64>::zeros3(i64(2), i64(3), i64(4)) ?? ndarray<f64>::empty()
    nd::scaleInto(b.viewMut(), a.view(), f64(2.0))
    let s = nd::sumv(a.view()) ?? f64(0.0)
    let t = nd::sumv(b.view()) ?? f64(0.0)
    let tr = nd::sumv(nd::swapDims(a.view(), i64(0), i64(2)) ?? a.view()) ?? f64(0.0)
    io::cout << f"n={v.count()} s={i64(s)} t={i64(t)} tr={i64(tr)}\n"
    return 0
}
```

实测：`n=24 s=36 t=72 tr=36`。细节见 `stdlib/stl/nd.extc` 与 `tests/stl/nd.extc`。

二维视图上可以直接写下标糖（`use std::nd` 之后）：

```extc
// 结果：one=12 sub=66
use std::io
use std::nd

fn main() -> i32 {
    var a: ndarray<f64> = ndarray<f64>::zeros2(3, 4) ?? ndarray<f64>::empty()
    var v: ndview<f64> = a.viewMut()
    for r in 0..3 {
        for c in 0..4 {
            v.data[r * 4 + c] = f64(r * 10 + c)
        }
    }
    let sub: ndview<f64> = v[1..3, 1..3] ?? v
    let s: f64 = nd::sumv(sub) ?? f64(0.0)
    io::cout << f"one={v[1, 2]} sub={s}\n"
    return 0
}
```

## 3. `std::*`：模块

### `std::io`：控制台与缓冲的读写

| 名字 | 签名（节选） |
|---|---|
| `ioError` | `\| readFailed(i32) \| lineTooLong(i64) \| writeFailed(i32) \| notATerminal(i32) \| closed(i32) \| destFull(i64)` |
| `cout` / `cerr` / `cin` | `ostream { fd: 1 }` / `errStream { }` / `istream { fd: 0 }` |
| `endl` | `let endl: slice<u8> = "\n"` |
| `ostream <<` | `i64` / `i32` / `i16` / `i8` / `u32` / `u16` / `u8` / `f64` / `f32` / `bool` / `slice<u8>` / `mut slice<u8>` |
| `padText` / `padInt` / `padF` | 宽度 + 填充字节 + `LEFT`/`RIGHT`/`CENTER` | ；常量 |
| `writer` | `struct writer { fd, buf, len }`（`@noCopy`）；`write` / `flush` / `cap` |
| `writerOf(fd)` | 造一个带 64 KiB 缓冲的 writer |
| `reader` | `avail` / `buffered` / `fill` / `nextByte` / `eof` / `nextLine` / `nextLineRaw` / `skipSpace` / `nextToken` / `nextInt` |
| `readerOf(fd)` | 造一个带 64 KiB 缓冲的 reader |
| `istream >>` | `mut ref i64` / `mut slice<u8>` / `mut ref u8`；`bad()` / `clear()` |
| `flushOut` / `writeBytes` / `writeBytesRaw` | 冲 stdio 侧 / 进 cout 缓冲 / 直写 fd 1 |
| `print` / `println` | `fn print(s: slice<u8>)` / `fn println(s: slice<u8>)`（不用链式写法的一行输出，走 cout 同一缓冲；数字仍用 `io::cout`）|
| `eprint` / `eprintln` | `fn eprint(s: slice<u8>)` / `fn eprintln(s: slice<u8>)`（诊断通道，写 stderr；与 stdout 两条通道分开，`cerr` 是流式写法）|

**程序级单例**：`io::cout`、`io::cerr`、`io::writer` 的缓冲、`fs::fout`/`fs::fin` 以及
`io::fmt` 的格式化暂存（`FMTSTATE`，含 `FMTSTATE.text` 与 `FMTSTATE.pairs`）都是**整个程序一份**的状态，不是线程局部的；
`fs` 的两块缓冲与它们的 fd/位置也收在两个对象里（`fs::OUT`、`fs::IN`）。这不构成隐藏炸弹，
因为语言把它们挡在了唯一能并行的位置之外：`par` 的工作体**不允许调用任何函数**（编译期拒绝，
`tests/errors/` 有负例），所以工作线程碰不到这些缓冲；协程是协作式的，一次 `<<` 是"格式化 + 拷进缓冲"
的连续动作、只在 `yield` 处换人 ⇒ 一行不会被切开（`tests/io/concurrent-write.extc` 钉住这一点）。

低层格式化（`std::io::fmt` 的公开面，`std::io` 与 `std::fs` 共用；名字不带 `p` 前缀，但它们
**没有兼容性承诺**，热路径要用就直接调）：

| 名字 | 一句话 |
|---|---|
| `fmt::padText` / `fmt::fillBytes` | 把 `slice<u8>` 按宽度/填充字节/对齐写进 `FMT` 缓冲；`ostream.padText` 是它的糖 |
| `fmt::fmtF` | 定点格式化一个 `f64`（`|v| >= 1e15` trap）：`f` 说明符与 `padF` 的落点 |
| `fmt::flushCout` | 把 `cout` 缓冲冲给 stdio；`io::flushOut` 就是它 |
| `fmt::pairsReady` | 惰性建好格式化说明符查表（`FMT` 的懒初始化），`print`/`println` 先调它 |

关键语义：

- **EOF 是 `success(0)`，不是错误**；`readFailed` / `lineTooLong` / `writeFailed` / `closed` / `destFull` /
  `notATerminal` 是分开的失败格。
- `io::writer` 有**自己的** 64 KiB 缓冲：不 `flush()` 就退出，里面没冲的字节**丢**
  。
- `nextInt` 会**跳过非数字往前找**（`hello world 99` 读出 99）。
- `cin >> line` / `fin >> line` **不跳过空白**、**不返回长度**。
- 读全是阻塞式 `read(2)`：没有超时、没有非阻塞、没有 select/poll。
- `ostream` 收所有比 `i64` 窄的整数（`i8`/`i16`/`u8`/`u16`/`u32`）与 `f32`：窄位宽无损加宽到
  `i64`/`f64` 再打，调用点不用写 cast。`u64` 走自己的无符号格式化路径（`fmtU64Fast`，不经过
  `i64`，所以 `2^64-1` 打得出来）。`cerr` 与 `cout` 收同一套窄整数（`i8`/`i16`/`i32`/`u8`/`u16`/
  `u32`/`u64`），但**没有 `f64`/`f32`**（stderr 上没有浮点格式化路径）；`cerr`/`cout` 都没有
  `seek` / `tell` / `peek` / `unget`（`grep` 整个 `io.extc` 无这些名字）。

```extc
use std::io
fn main() -> i32 {
    var w: io::writer = io::writerOf(io::STDOUT_FD)
    w.write("writer-line\n")?
    w.flush()?
    io::cout << "cout=" << 42 << " " << 1.5 << " " << true << "\n"
    io::cout.padInt(i64(7), i64(4), u8(48), io::RIGHT) << "|\n"
    io::flushOut()
    io::cerr << "cerr-line\n"
    return 0
}
```
实测 stdout：`writer-line` / `cout=42 1.5 true` / `0007|`；stderr：`cerr-line`。

### `std::fs`：打开文件的三个名字

名字里必须能看出**读还是写、原内容还在不在**：

| 做什么 | 名字 | 返回 | 已有内容 |
|---|---|---|---|
| 读 | `fs::openRead(p)` | `fs::ifstream` | 不动 |
| 写（**截断**） | `fs::openWrite(p)` | `fs::ofstream` | 清掉 |
| 写（**追加**） | `fs::openAppend(p)` | `fs::ofstream` | 保留，写在后面 |
| 读全部 | `readAll(f, dest)` | `result<i64, ioError>` | 调用者给地方 |

**两个类型分开**：`fs::ifstream`（读型）· `fs::ofstream`（写型）。
读型上没有 `put`、写型上没有 `readSome` ⇒ **误用编不过**。

用户层代码里**不该出现** `O_*` 平台常量（它们只在 `std::sys::io` 定义）。

补充现状：

| 名字 | 一句话 |
|---|---|
| `ifstream::new` / `readSome` / `readAll` / `reader` / `bad` / `clear` / `close` | 读句柄；`close` 幂等；用到已关句柄是 `closed(fd)` 值，不是 trap |
| `ifstream >> i64 / slice<u8> / u8` | 与 `cin >>` 同一套语义；失败置 `bad()`，目标保持原值 |
| `ofstream::new` / `append` / `put` / `close` | `put` 直接 `write(2)`（**不缓冲**）；`close` 是提交点 |
| `openRead` / `openWrite` / `openAppend` | | `openWrite` 带 `O_TRUNC`，`openAppend` 带 `O_APPEND` |
| `fs::fout` / `openOut` / `flushOut` / `closeOut` | | **程序级**输出流，带缓冲（`<<` 接受 `slice<u8>` / `mut slice<u8>` / `i64` / `bool`） |
| `fs::fin` / `openIn` / `closeIn` | | 程序级输入流 |
| `outStream::failed` / `inStream::bad` | | 失败标志 |

```extc
use std::io
use std::fs
fn main() -> i32 {
    var w: fs::ofstream = fs::openWrite("std-a-fs1.txt")?
    w.put("hello\n")?
    w.close()?
    var a: fs::ofstream = fs::openAppend("std-a-fs1.txt")?
    a.put("again\n")?
    a.close()?
    var r: fs::ifstream = fs::openRead("std-a-fs1.txt")?
    var buf: [64]u8
    let n: i64 = r.readAll(buf[..])?
    r.close()?
    io::cout << "read " << n << " bytes: " << buf[0..n]
    return 0
}
```
实测：`read 12 bytes: hello` ⏎ `again`（截断再追加，两段都在）。
 另有一条闸门：断言 `openRead` / `openAppend` / `ifstream` / `ofstream` 这几个词
留在本文件里。

### `std::term`：终端 raw 模式

| 名字 | 签名 |
|---|---|
| `terminal` | `struct terminal { fd, saved[128], rawbuf[128], on }`（两个缓冲是不透明的 `struct termios`） |
| `terminal::raw` / `restore` / `close` | 都返回 `result<unit, io::ioError>`；`close` 就是 `restore`，幂等 |
| `rawTerminal(fd)` | `fn rawTerminal(fd: i32) -> result<terminal, io::ioError>` |

语义：声明了 `close` ⇒ 它是**资源**，"开了没关"编译器会提醒；
不是终端时给 `failure(notATerminal(fd))`（不是 trap）；trap 路径会经运行期钩子把终端还原
（；判据  里 `raw-trap` 一节）。
**没有** size / color / 光标 / 清屏 / 按键解码（`grep` 整个 `term.extc` 只有 raw 开关）。

```extc
use std::io
fn main() -> i32 {
    match io::rawTerminal(io::STDIN_FD) {
        failure(e) => { match e { notATerminal(fd) => { io::cout << "not-a-tty fd=" << fd << "\n" }
                                 readFailed(fd) => { io::cout << "readFailed\n" }
                                 lineTooLong(n) => { io::cout << "lineTooLong\n" }
                                 writeFailed(fd) => { io::cout << "writeFailed\n" }
                                 closed(fd) => { io::cout << "closed\n" }
                                 destFull(n) => { io::cout << "destFull\n" } } }
        success(t) => { var tt: io::terminal = t
                        tt.restore()?
                        tt.close()?
                        io::cout << "raw-ok\n" } }
    return 0
}
```
实测（stdin 不是终端，`</dev/null`）：`not-a-tty fd=0`。

### `std::loop`：两个钟、秒表与定时表

| 名字 | 签名 |
|---|---|
| `now()` | 单调钟，纳秒；**不会倒退**，测时长用它 |
| `real()` | 墙钟，纳秒（Unix epoch 起算）；会跳（NTP/时区/改表） |
| `sleepNs` / `sleepUs` / `sleepMs` | 阻塞当前线程 |
| `timer` + `start()` | `elapsedNs` / `elapsedUs` / `elapsedMs` / `elapsedSecs`；`t0` 是 `@private` |

`loop` 里没有收回调的计时器：把行为当参数有两种写法 —— 不捕获的 lambda 直接当 `fn(...)` 传（[可调用的值](language.md#35-可调用的值fn-与闭包)），带状态的闭包交给泛型参数并在体内调 `call`。

```extc
use std::io
use std::loop
fn main() -> i32 {
    let a = loop::now()
    loop::sleepMs(i64(2))
    var t = loop::start()
    loop::sleepUs(i64(1000))
    io::cout << "mono-advances=" << (loop::now() > a) << " elapsedUs-positive=" << (t.elapsedUs() > i64(0)) << " real-positive=" << (loop::real() > i64(0)) << "\n"
    return 0
}
```
实测：`mono-advances=true elapsedUs-positive=true real-positive=true`。

### `std::reflect`：读一个运行期才知道类型的值

`any` 里装的类型只有运行期知道，所以"读它"必须走编译器发射的类型描述符。这一层是描述符的
只读访问器：不分配、不在用户代码里递归。

| 函数 | 说明 |
|---|---|
| `reflect::json(ref v, out: mut slice<u8>) -> i64` | 按字段/变体表编码成 JSON 写进 `out`，返回**需要**的字节数（可超过 `out.len`） |
| `reflect::typeName(ref v, out: mut slice<u8>) -> i64` | 拷出运行期类型名（`i64`、`point`、空槽 `<none>`） |

`ref`/`?ref` 字段编码成 `null`（不跟进去）；枚举编码成变体名；`slice<u8>` 当文本转义；深度上限 32。

### `std::json`：零拷贝解析 + 零分配写入

| 名字 | 签名 |
|---|---|
| 节点种类 | `OBJ=1` `ARR=2` `STR=3` `NUM=4` `BOOL=5` `NUL=6`；`MAXDEPTH=64` |
| `node` / `parsed` | 带载荷枚举 `\| obj \| arr \| text \| number \| flag \| nul` / `\| ok(i64) \| err(i64)` |
| `parse(text, nodes)` | 节点池**由调用方给**；池满报错，不静默截断 |
| `isOk` / `rootOf` / `errAt` | 投影（也可以直接 `match`） |
| 访问器 | `kindOf` · `countOf` · `rawOf`（原文视图） · `escOf` · `unescape(text, nodes, idx, out)` · `strOf(..., scratch)` |
| 结构访问 | `member` · `keyAt` · `valueAt` · `elem` · `find(text, nodes, root, "a.b.0")` |
| 取值 | `numOf -> f64` · `intOf -> i64`（只在 ≤18 位十进制时给值，否则 0） · `boolOf` |
| `builder` | `builder::open(buf) -> ?builder`（含引用，**没有零值**）；`ok` `len` `beginObj` `endObj` `beginArr` `endArr` `key` `str` `int` `boolean` `nul` `raw` `flt` |

语义：**解析不分配**（节点池是调用方的）、**字符串零拷贝**（`str` 是指向输入的视图，`esc` 标志说
要不要先解码）、**写入不分配**（直接拼进调用方缓冲）、**递归深度上限 64**。
越界 / 类型不符的访问器一律给"没有"。

```extc
use std::io
use std::json

fn main() -> i32 {
    var pool: [64]json::node
    let body: slice<u8> = "{\"a\":[1,2,3],\"b\":true,\"c\":\"x\\ny\"}"
    let p = json::parse(body, pool[..])
    if !json::isOk(p) { io::cout << "parse-fail at " << json::errAt(p) << "\n"  return 1 }
    let root = json::rootOf(p)
    io::cout << "kind=" << i64(json::kindOf(pool[..], root)) << " n=" << json::countOf(pool[..], root)
    let e1 = json::elem(pool[..], json::find(body, pool[..], root, "a"), i64(1))
    io::cout << " a1=" << json::intOf(body, pool[..], e1)
    let cb = json::find(body, pool[..], root, "c")
    var scratch: [64]u8
    let cn = json::unescape(body, pool[..], cb, scratch[..])
    io::cout << " c=" << scratch[0..cn] << "\n"

    var buf: [128]u8
    var w: json::builder = json::builder::open(buf[..])!
    w.beginObj()
    w.key("k"); w.str("v")
    w.key("n"); w.int(i64(7))
    w.endObj()
    io::cout << "built=" << buf[0..w.len()] << " ok=" << w.ok() << "\n"

    let bad = json::parse("[1,", pool[..])
    io::cout << "bad-ok=" << json::isOk(bad) << " at=" << json::errAt(bad) << "\n"
    return 0
}
```
实测：`kind=1 n=3 a1=2 c=x`（换行）`y` / `built={"k":"v","n":7} ok=true` / `bad-ok=false at=3`。

### `std::text::string`：Base64（`b64Name` 之外的两个函数）

| 名字 | 签名 |
|---|---|
| `encodedLen(n)` / `decodedMax(n)` | 目标缓冲该多大 |
| `encode` / `decode` | `fn encode(src: slice<u8>, out: mut slice<u8>) -> i64`；标准字母表（RFC 4648 第 4 节） |
| `encodeUrl` / `decodeUrl` | URL 安全字母表（`-` `_`，RFC 4648 第 5 节）；两种都**带 `=` 填充** |

返回值是**写出的字节数**；缓冲不够 / 非法字符 / 长度不是 4 的倍数都给 `-1`
（、）。不做流式。

```extc
use std::io
use std::text::string
fn main() -> i32 {
    var enc: [64]u8
    let n = string::encode("hello", enc[..])
    var dec: [64]u8
    let m = string::decode(enc[0..n], dec[..])
    io::cout << enc[0..n] << " " << dec[0..m] << " " << string::decode("!!!!", dec[..]) << "\n"
    return 0
}
```
实测：`aGVsbG8= hello -1`（`aGVsbG8=` 与 `echo -n hello | base64` 一致）。

### `std::hash`：sha1 / sha256 / hmac

| 名字 | 签名 |
|---|---|
| `digest::sha256.sum(data, out) -> bool` | `out.len < 32` 给 `false` |
| `digest::sha256.hex(digest, out) -> i64` | 十六进制，返回长度 |
| `digest::sha256State` + `init` / `update` / `final` | 分块哈希 |
| `digest::sha1.sum` / `hex` / `state`（20 字节摘要） | | 、|
| `digest::hmac.sha256(key, msg, out) -> bool` | `out.len < 32` 给 `false`；键长 > 64 先散列一次（RFC 2104） |
| `digest::hmac.sha1(key, msg, out) -> bool` | |

零分配（state + 64 字节块都在栈上）。

```extc
use std::io
use std::hash::digest
fn main() -> i32 {
    var d: [32]u8
    digest::sha256.sum("abc", d[..])
    var hex: [64]u8
    let hn = digest::sha256.hex(d[..], hex[..])
    io::cout << "sha256=" << hex[0..hn] << "\n"
    var d1: [20]u8
    digest::sha1.sum("abc", d1[..])
    var h1: [40]u8
    let h1n = digest::sha1.hex(d1[..], h1[..])
    io::cout << "sha1=" << h1[0..h1n] << "\n"
    var mac: [32]u8
    digest::hmac.sha256("key", "msg", mac[..])
    var mh: [64]u8
    let mhn = digest::sha256.hex(mac[..], mh[..])
    io::cout << "hmac=" << mh[0..mhn] << "\n"
    return 0
}
```
实测（与 `sha256sum` / `sha1sum` / `openssl dgst -sha256 -hmac key` 三方对齐）：
`sha256=ba7816bf…20015ad` · `sha1=a9993e36…9cd0d89d` · `hmac=2d93cbc1…1b1b8c628`。

### `std::net`：套接字与名字解析

`use std::net` 是公开门面；epoll 三件**不在这里**（属于 `std::loop` 与协程调度器）。约定与内核一致：失败给**负数**，`>= 0` 是 fd / 字节数。

| 名字 | 签名 |
|---|---|
| `sockPair(out)` | 一对已连接的 AF_UNIX fd |
| `accept` / `read` / `write` | `fn read(fd: i64, buf: mut slice<u8>) -> i64` 等 |
| `nonblock` / `nodelay` / `shutdownWr` / `timeout` / `localPort` | |
| `tcpListen` / `tcpListenShared` / `tcpConnectTo` | `SO_REUSEPORT` 版给多进程分片 |
| `dnsLookup(host, out)` / `resolve(addr, out)` | 主机名 → IPv4；`resolve` 也接受字面量 |

**没有 `close(fd)`**：`grep` 整个 `net.extc` / `std/sys/net.extc` 只有 `shutdownWr`，关闭 fd 的原语只在
`std::sys::io::close`（特权层，见本页末）。

```extc
use std::io
use std::net
fn main() -> i32 {
    var sp: [2]i64
    let rc = net::sockPair(sp[..])
    let w = net::write(sp[0], "ping")
    var buf: [8]u8
    let r = net::read(sp[1], buf[..])
    io::cout << "pair=" << rc << " wrote=" << w << " read=" << r << " got=" << buf[0..r] << "\n"
    return 0
}
```
实测：`pair=0 wrote=4 read=4 got=ping`。

### `std::http`：请求/响应解析与连接

| 名字 | 签名 |
|---|---|
| `parseRequest(raw) -> request` | 拆请求行 + 头 |
| `request::path` / `query(name)` / `header(name)` / `length` / `isGet` / `isPost` | |
| `parseResponse(raw) -> response` | 拆状态行 + 头 |
| `response::header` / `length` / `isOk` / `is(status)` | |
| `reasonOf` / `headerOf` / `contentLength` / `pathOf` / `queryOf` | 自由函数版本 |
| `beginResponse` / `putHeader` / `putHeaderI64` / `endHeaders` / `simpleResponse` / `beginRequest` | 拼字节 |
| `decode(src, dst)` | 百分号解码 |
| `conn::open` / `send` / `readResponse` / `exchange` / `get` / `post` / `close` | 一条连接上的往返 |
| `session::make` / `ok` / `exchange` / `get` / `post` / `close` | 会自己重连的那种 |
| `request` 的字段 | `method` · `target` · `version` · `headers` · `body` · `complete`（请求行 + 头是否读全）|
| `response` 的字段 | `version` · `status` · `reason` · `headers` · `body` · `complete` |
| `conn` 的字段 | `fd` · `rest`（keep-alive 下还剩多少字节没被上一条响应消费）|
| `session` 的字段 | `c`（一条连接）· `out`（请求头缓冲）· `buf`（接收缓冲，响应里的切片指向它）|

解析与拼字节都**离线可跑**（判据 ）：

```extc
use std::io
use std::net
fn main() -> i32 {
    let raw: slice<u8> = "GET /a/b?x=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc"
    var r: net::request = net::parseRequest(raw)
    io::cout << "method=" << r.method << " path=" << r.path() << " host=" << (r.header("host") ?? "none") << " q=" << (r.query("x") ?? "none") << " len=" << r.length() << " isGet=" << r.isGet() << "\n"
    let resp: slice<u8> = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"
    var p: net::response = net::parseResponse(resp)
    io::cout << "status=" << p.status << " reason=" << net::reasonOf(p.status) << " ok=" << p.isOk() << " len=" << p.length() << "\n"
    return 0
}
```
实测：`method=GET path=/a/b host=h q=1 len=3 isGet=true` / `status=200 reason=OK ok=true len=2`。

### `std::ws`：WebSocket 帧

| 名字 | 签名 |
|---|---|
| 操作码 | `OP_CONT=0` `OP_TEXT=1` `OP_BIN=2` `OP_CLOSE=8` `OP_PING=9` `OP_PONG=10`；`GUID` |
| `acceptFor(key, out)` | `Sec-WebSocket-Accept`（base64(SHA1(key + GUID))） |
| `requestInto(host, path, key, out)` | 拼握手请求 |
| `frameInto(op, fin, payload, mask, out)` | 拼一帧 |
| `hdrLen` / `opOf` / `finOf` / `maskedOf` / `lenOf` / `maskAt` / `unmask` | 拆帧 |

判据 。`acceptFor` 与 RFC 6455 的例子逐字节一致：

```extc
use std::io
use std::net
fn main() -> i32 {
    var acc: [64]u8
    let n = net::acceptFor("dGhlIHNhbXBsZSBub25jZQ==", acc[..])
    var m: [4]u8 = [1, 2, 3, 4]
    var frame: [64]u8
    let fl = net::frameInto(net::OP_TEXT, true, "hi", m[..], frame[..])
    io::cout << "accept=" << acc[0..n] << " op=" << net::opOf(frame[0..fl]) << " fin=" << net::finOf(frame[0..fl]) << " masked=" << net::maskedOf(frame[0..fl]) << " len=" << net::lenOf(frame[0..fl]) << "\n"
    return 0
}
```
实测：`accept=s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`（与 `openssl dgst -sha1 -binary | base64` 一致）·
`op=1 fin=true masked=true len=2`。

### `std::proc`：进程

| 名字 | 签名 |
|---|---|
| `exit(code)` | 立即结束进程；**不 flush 你自己的 `writer`** |
| `spawn()` | POSIX `fork`：父得子 id，子得 0，失败 -1；**子从这一句继续** |
| `wait(pid)` | 回收并给退出状态（被信号杀死的报 0） |
| `self()` | 本进程 id |
| `pin(cpu)` | 绑核（`cpu < 0` 解除），返回 0 成功 |

```extc
use std::io
use std::proc
fn main() -> i32 {
    let pid = proc::spawn()
    if pid == i32(0) { io::cout << "child\n"  io::flushOut()  proc::exit(i32(3)) }
    io::cout << "child-status=" << proc::wait(pid) << "\n"
    return 0
}
```
实测：`child` / `child-status=3`。

### `std::parallel`：线程（可切块的计算）

`parallel::run(worker, <视图实参>…, n, threads) -> i32` 是 `@builtin`：
语言没有一等函数值，"把 worker 放到线程上跑"必须由编译器生成 trampoline。

- worker 形状：`fn worker(id: i64, lo: i64, hi: i64, <只读视图>…, out: mut slice<T>) -> i32`。
- **恰好一个 `mut slice<T>`** 按 `[lo, hi)` 切成互不重叠的一段；只读视图**每个 worker 拿到同一份**（用绝对下标）。
- 返回**第一个非零**的 worker 返回值（0 = 全成功）；每个 worker 有自己的 arena。
- worker 体内只许调 `Thread=0` 的东西或同样形状的顶层函数（检查是传递的，深度上限 16）；模块级 `var` 一律拒绝
  。

```extc
use std::io
use std::parallel

fn worker(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 {
    var i: i64 = lo
    while i < hi { out[i - lo] = i * i  i = i + 1 }
    return 0
}
fn main() -> i32 {
    var buf: [8]i64
    var out: mut slice<i64> = buf[..]
    let rc: i32 = parallel::run(worker, out, i64(8), i64(2))
    io::cout << "rc=" << rc << " sum=" << (buf[0] + buf[1] + buf[2] + buf[3] + buf[4] + buf[5] + buf[6] + buf[7]) << "\n"
    return 0
}
```
实测：`rc=0 sum=140`（= 0+1+4+9+16+25+36+49）。

### `std::coro::scheduler`：协程调度器

一个 `while` 循环 + 一个事件源。协程返回类型写 `coroutine<T>`；`yield e` 挂起（**负数请求 = 别等，立刻再推我**），
`send(v)` 把值送回去，驱动侧就是 `next()` / `value()` / `send(v)`。

| 名字 | 签名 |
|---|---|
| `loop` | 调度器状态，**只有标量**（含引用的结构体没有零值） |
| `tasks<T>` | 任务表（调用者自己的容器）：`rows` / `runq` / `rqHead` / `freeq` / `acceptq` |
| `tasks::add` | `fn add(self: mut ref tasks<T>, s: mut ref loop, h: coroutine<i64>) -> bool` |
| `tasks::step` / `resumeRow` / `finish` / `runScripted` | 推进一行 / 一路推到挂起 / 收尾 / 按顺序推进 |
| `tasks::pump` / `runEpoll` | 真事件循环：`>=0` 新 accept 的 fd，`-1` 处理了事件，`-2` 超时 |
| `newRows()` / `newQ()` / `s.init(listener)` / `s.pendingTimers()` | 造表 / 造队列 / 初始化 / 定时器计数 |

判据在 （脚本化驱动的 `coro_sched.extc` 用退出码自证，`coro_tasks.extc` 也是）：

```text
$ ./build/extc --run tests/coro/coro_sched.extc ; echo $?
36
$ ./build/extc --run tests/coro/coro_tasks.extc ; echo $?
10
```

### `std::mem` 的板：一块连续内存 + 一道门

一次 `mmap` 保留地址空间（`PROT_NONE` + `MAP_NORESERVE`），`alloc` 按需 `mprotect` 成 RW；**地址一给出就不变**；
每个出口验一次"在板内"（`holds`）；`close` 是 `munmap`，之后再用板内指针是硬缺页。`@noCopy`。

| 名字 | 签名 |
|---|---|
| `PAGE` / `CHUNK` / `DEFAULT_RESERVE` | `4096` / `1 MiB` / `4 GiB` | 、|
| `plate::open(reserve) -> ?plate` | 保留一片地址空间 |
| `reserved` / `committed` / `used`（字段，字节数）+ `reservedBytes` / `committedBytes` / `usedBytes` / `remaining` / `isClosed`（方法） | |
| `alloc(n) -> ?mut slice<u8>` · `allocPtr(n) -> ?ref u8` | 申请（`effects Ret=0`：返回的视图不指向调用方帧） |
| `view(off, n)` · `viewAt(p, n)` · `holds(p)` · `holdsRange` · `holdsView` | 板内视图 / 从裸指针过门造视图 / 区间检查 |
| `copyIn` / `copyOut` / `store` | 进/出板 |
| `loanRO` / `loanRW` | 改页权限（临时只读 / 可写） |
| `quarantine` / `unquarantine` / `poison` | 隔离 / 解除 / 填毒 |
| `close()` | `munmap`，双关安全 |

```extc
use std::io
use std::mem
fn main() -> i32 {
    var pl: mem::plate = mem::plate::open(mem::DEFAULT_RESERVE)!
    var v: mut slice<u8> = pl.alloc(u64(8))!
    v[0] = u8(42)
    let held = pl.holds(v.data)
    io::cout << "used=" << i64(pl.usedBytes()) << " committed=" << i64(pl.committedBytes()) << " v0=" << i64(v[0]) << " holds=" << held << "\n"
    let rc = pl.close()
    io::cout << "close=" << rc << " isClosed=" << pl.isClosed() << "\n"
    return 0
}
```
实测：`used=4096 committed=1048576 v0=42 holds=true` / `close=0 isClosed=true`。

### `std::dl`：动态加载

| 名字 | 签名 |
|---|---|
| `open(path) -> option<ref void>` | `path` **必须 NUL 结尾**：字面量直接传，运行期拼的先 `cstr` |
| `sym(h, name) -> option<ref void>` | `dlsym`；给的是**地址，不是类型** |
| `symOrNull(h, name) -> ?ref void` | 要自己判空时用它 |
| `close(h)` / `cstr(s)` / `cstrLen(p)` / `viewCStr(p)` | 关 / 补终止符 / C 字符串长度 / C 字符串 → 视图 |

把 `dlsym` 回来的地址变成可调用的值，要调用方写一次**显式转换**（`fn(i64) -> i64(raw)`）——
那句话就是签字。

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
实测：`triple=42 close=0`。`<<` 链从左到右求值（见"今天会咬人的组合"第 1 条），`close`
仍建议另起一句：原生运算符与普通调用的实参顺序没有保证。

### `std::mem`：字节块的查找与比较、板

| 名字 | 签名 |
|---|---|
| `find(hay, needle) -> i64` | 最左匹配偏移；没有 `-1`；空模式 `0` |
| `contains(hay, needle) -> bool` | |
| `eq(a, b) -> bool` | 整块比较（`n <= 0` 算相同） |
| `copy(dst, src) -> i64` | 原地整块拷贝；装不下 `-1` |

`std::text::string` 的 `find` / `contains` 建在它上面。

```extc
use std::io
use std::mem
fn main() -> i32 {
    let at = mem::find("hello world", "world")
    io::cout << "find=" << at << " miss=" << mem::find("abc", "z") << " eq=" << mem::eq("ab", "ab") << " contains=" << mem::contains("abc", "bc") << "\n"
    var dst: [4]u8
    let n = mem::copy(dst[..], "wxyz")
    io::cout << "copied=" << n << " dst=" << dst[0..n] << "\n"
    return 0
}
```
实测：`find=6 miss=-1 eq=true contains=true` / `copied=4 dst=wxyz`。

### `std::uuid`

`std::loop` 是事件循环的**策略层**：一张按 deadline 排序的表 + 几个纯函数，**没有系统调用、没有 C**，
所以定时语义可以离线判据。`timers` 是 `@noCopy`、容量固定 `CAPACITY = 1024`
（、）：`init` · `add(deadline, tag) -> i64`（表满 `-1`，相同 deadline 保持登记顺序）·
`count` · `next() -> i64`（最近的 deadline；空表 `-1`）· `timeoutMs(now, cap)`（**向上取整**，绝不早醒）·
`popDue(now, out) -> i64`（每个 deadline 恰好取出一次，`out` 满就停手）· `cancel(tag) -> bool` ·
`runOnce(ep, t, tags, now, capMs)`。

`std::uuid` 只做 v5：`v5(ns, name, out)` · `v5dns` · `v5url` · `v5oid` · `v5x500` · `format(id, out) -> i64`
（、）。

```extc
use std::io
use std::loop
use std::hash::digest
fn main() -> i32 {
    var t: loop::timers
    t.init()
    t.add(i64(100), i64(7))
    t.add(i64(50), i64(8))
    var out: [4]i64
    let due = t.popDue(i64(100), out[..])
    var id: [16]u8
    digest::v5dns("example.com", id[..])
    var u: [36]u8
    let n = digest::format(id[..], u[..])
    io::cout << "next=" << t.next() << " due=" << due << " tag0=" << out[0] << " uuid=" << u[0..n] << "\n"
    return 0
}
```
实测：`next=-1 due=2 tag0=8 uuid=cfbff0d1-9375-5685-968c-48ce8b15ae17`
（`python3 -c "import uuid;print(uuid.uuid5(uuid.NAMESPACE_DNS,'example.com'))"` 给同一个值）。

### 用户层不该直接用：`std::sys::*`

`std::sys::*` 是**声明 C 原语的特权层**：它能把任意 `(指针, 长度)` 变成视图，能 map/unmap 任意地址段。
其中只有 `std::sys::heap` **真的被挡住**——它是  那张表里唯一一项：

```text
$ ./build/extc tests/modules/errors/privileged/main.extc -o /dev/null
tests/modules/errors/privileged/main.extc:3: error: `std::sys::heap` is the standard library's **privileged layer**: ...
note:  importing it from a program would step around the whole safety net (bounds checks, escape checks, the plate's range check), so only the standard library may.
note:  what a program normally needs is `std::heap`: `plate::open` / `alloc` / `view(off, n)` / `viewAt(p, n)` / `holds(p)` are the checked doors.
```

| 特权层文件 | 装的是 | 用户面对应的公开层 |
|---|---|
|  | `read` / `write` / `close`、`O_*`、`STDIN/STDOUT/STDERR`、cout 运行期 | `std::io`、`std::fs` |

> **为什么 `open` 不在这儿**：glibc 把 `open` 声明成变参（`open(const char *, int, ...)`）。协程运行期
> 会 `#include <fcntl.h>`，所以只要程序同时 import 了协程调度器与 `std::fs`，产物里那条定参原型就是
> `conflicting types for 'open'` —— **编译期直接失败**（实测：只 import 都编不过）。现在 `open` 是运行期
> shim（`extc_file_open_at` / `extc_file_open_cstr`，`stdlib/std/sys/file.extc`），extC 侧不再声明它。
|  | `tcgetattr` / `tcsetattr` / `cfmakeraw` / `TCSANOW` / raw 钩子 | `std::term` |
|  | `extc_time_mono_ns` / `extc_time_real_ns` / `extc_time_sleep_ns` | `std::time` |
|  | `extc_file_open` / `extc_file_open_at` / `extc_file_open_cstr` / `extc_file_close` / `extc_sendfile`、`open` / `openAt` | `std::fs`（`struct stat` 不过界）；`open` / `openAt` 是这一层的 shim，不是 libc extern —— 见下条 |
|  | 裸 fd、epoll、socket、DNS 运行期 | `std::net`、`std::loop`、`std::coro::scheduler` |
|  | `mmap` / `mprotect` / `munmap`、`extc_viewOf`、`extc_memCopy` | `std::heap` |
|  | `fork` / `waitpid` / `getpid` / `sched_setaffinity` / `exit` | `std::proc` |
|  | 运行期池记录 + 板块原语 | `std::data::pool` / `std::data::vector`（编译器合成调用） |
|  | 任务表运行期（`extc_task_begin` / `_zone` / `_end` / `_live`） | `std::coro::scheduler` |
|  | 域的运行期入口 | 编译器 + 库 |

**实测：除了 `std::sys::heap`，其余 `std::sys::*` 都能被用户代码 import 并直接调用**
（`use std::sys::io` / `net` / `pool` / `domain` 都编过；直接调 `sysio::write` / `sysio::close` 也跑得通）。
所以这条边界目前是**约定**，不是强制（ 也这么写）。
另有两条**真的挡**：`@builtin` 声明只能出现在标准库里（、）：

```text
$ ./build/extc --build -o /dev/null std-d-priv6.extc      # 用户文件里写 `@builtin fn f() -> i64`
std-d-priv6.extc:2:1: error: `@builtin` declarations are standard-library only
note:  Builtin primitives are how the standard library names what the runtime implements; a program uses the library's checked doors instead ...
```

## 4. 今天会咬人的组合

1. **原生运算符的实参顺序仍是 C 的未指定顺序。** `<<` 链的每一步都是重载运算符，从左到右
   求值（先算左边、再算右边），所以 `io::cout << "x" << a() << b()` 里 `a()` 先于 `b()`；
   而**原生**运算符（`i64 + i64` 这类）和普通函数调用的实参顺序仍按 C 的未指定顺序。
   实测：

   ```extc
   use std::io
   fn a() -> i64 { io::cout << "A"  return 1 }
   fn b() -> i64 { io::cout << "B"  return 2 }
   fn main() -> i32 {
       io::cout << "chain=" << a() << b() << "\n"
       var x: i64 = a() + b()
       io::cout << "sum=" << x << "\n"
       return 0
   }
   ```
   实测输出：`chain=A1B2` / `ABsum=3`（`<<` 链保证先 A 后 B；`+` 这一行恰好也是 A 先，但没有保证）。
   **后果**：`io::cout << f(x) << dl::close(h)` 今天是安全的（链从左到右），但把
   `dl::close(h)` 写进原生运算符或普通调用的实参里，仍可能先关再调 ⇒ 段错误。
   资源释放（`close`）**另起一句**、先把要打印的值取出来，永远是最稳的写法。

2. **`??` 在一条语句里会"提前"算。** 编译器自己会拦下一部分：

   ```text
   error: `??` here would run **before** the call that comes earlier in this statement; split the line so the order you read is the order it runs
   note: The subject of `??` is computed into a temporary **before the whole statement** (that is what makes it run only once). ...
   ```
   实测（`io::cout << "had=" << m.put(k, v) << " v=" << (m.get(k) ?? -1)`）：拆成 `let had = ...` /
   `let v = m.get(k) ?? -1` 再打印即可。

3. **`use std::data::linmap` + `use std::text::string` + 实例化 `linMap<i64, slice<u8>>`**：生成的 C 里
   `option_slice_u8` 重复定义，C 编译失败：

   ```text
   build/…c:371:8: error: redefinition of 'struct option_slice_u8'
   extc: C compiler failed (exit 1) on `build/…c`
   ```
   把值类型换成 `i64`（不产生 `option<slice<u8>>`）或去掉 `std::text::string` 就过。

4. **`use std::io` 会把 `std::sys::io` 的名字一起并进那张扁平表**，于是用户文件里不能再声明
   `read` / `write` / `open` / `close`：

   ```text
   std-d-w4.extc:2:1: error: duplicate function `write`
   ```
   （ 声明了这四个。普通声明会按模块改名，而 `extern!` **不改名**
   ——它必须保持 C 符号名，所以同一个 C 符号声明两次就撞；模块合并会把依赖一起并入。）
