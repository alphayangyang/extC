# 标准库现状

本文只写**现在有什么 / 怎么用**：每一句都能指到**源码位置**或**一条能跑的判据**。
待做不在这里（见 [`../TODO.md`](../TODO.md)），历史不在这里（见 [`../CHANGELOG.md`](../CHANGELOG.md)）。

## 目录（导入路径）

| 你写 | 得到什么 |
|---|---|
| `use std::io` | 控制台与文件 I/O |
| `use stl::map` | 有序映射容器 |
| `use std::hash::sha256` | SHA-256 |
| `use std::coro::scheduler` | 协程调度器 |

`use a::b` 找的就是文件 `a/b.extc`；搜索顺序是**入口文件所在目录 → 每个 `-I <dir>` → 标准库目录**
（标准库目录 = `$EXTC_STD`，没设就是 `extc` 可执行文件旁边的 `../stdlib`）。
出错时编译器把这些候选原样列出来。

**一个文件 = 一个模块**，目录本身不是模块：

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

| 名字 | 是什么 | 位置 |
|---|---|---|
| `unit` | 空载荷：`result<unit, E>` 的"成功但没有值"那一格 |  |
| `option<T>` | `\| none \| some(T)`；零值 = `none`；取值的路只有 `match` 与 `?` |  |
| `result<T, E>` | `\| failure(E) \| success(T)`；零值 = `failure(E 的零值)` |  |
| `slice<T>` | `{ data: ref T, len: i64 }`；`s[i]` 是语言语法，越界带位置 trap |  |
| `varArray<T>` | arena 底的动态数组（见下） |  |
| `pcg32` | 显式状态的随机数引擎：`pcg32::seeded(42)` |  |
| `nextFloat` / `shuffleI32` | 0.0–1.0 的浮点、i32 视图洗牌 | 、|
| `coroutine<A, B>` | 协程的**接口标记**（帧由编译器按协程函数合成，不要拿它当变量类型） |  |
| `domain` | 域的**接口标记**，方法由编译器合成，库侧在 `std::sys::domain` |  |

`print(x)` / `println(x)` 是内建，但**已经弃用**：编译器每次都会说
``warning: `println` is deprecated``，并提示 `io::cout` 是唯一的控制台出口（见下面 `std::io` 一节）。

### `varArray<T>`：arena 底的动态数组

| 方法 | 签名 | 位置 |
|---|---|---|
| `withCap` | `fn withCap(cap: i64) -> varArray<T>` |  |
| `size` / `isEmpty` | `fn size(self: ref varArray<T>) -> i64` / `-> bool` | 、|
| `push` / `pop` | `fn push(self: mut ref varArray<T>, v: T)` / `fn pop(...) -> ?T` | |
| `get` / `set` | `fn get(self: ref varArray<T>, i: i64) -> ?T` / `fn set(self: mut ref varArray<T>, i: i64, v: T)` | |
| `clear` | `fn clear(self: mut ref varArray<T>)` | |
| `asSlice` / `asMutSlice` | `fn asSlice(...) -> slice<T>` / `-> mut slice<T>` | |
| `==` | 按内容比较 | |

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

导入名就是 `stl::<文件名>`：`use stl::vector`、`use stl::hashMap`、`use stl::stringio`……
这一族里的容器大多带 `@sharesStorage` + `@poolObject`：
**按值拷贝两份会共用同一块板块**，编译器会为此报警告；要独立副本必须显式 `clone()`
（判据  / `clone_ok.extc`，见 ）。

### `varArray` / `vector`：两个动态数组

| | `varArray<T>`（prelude） | `vector<T>`（`stl::vector`） |
|---|---|---|
| 存储来源 | arena 切片 | **自己的池**（`@poolObject`，） |
| 寻址 | 下标 | 下标（`len` / `capacity` / `get` / `set`） |
| 容量怎么涨 | 翻倍（下限 4） | **1.5 倍、原地长** |
| 能不能装引用 | 能（ 点名） | 存储是池的板块，取内容只经 `toSlice()` 拷贝 |
| 取内容 | `asSlice()` / `asMutSlice()` 借出视图 | `toSlice()` **拷一份**（没有零拷贝视图：`push`/`clear`/`shrink`/`release` 任一动作都能让它失效，） |
| 深拷贝 | 没有 `clone`；`==` 按内容比较 | `clone()` |
| 收尾 | 无 | `shrink()` / `release()`（含世代守卫，） |
| 迭代顺序 | 插入序 | 插入序（dense 连续） |

`vector` 的方法：`withCap` · `new` · `len` · `capacity` ·
`isEmpty` · `get`（给 `option<T>`） · `set`（越界返回 `false`，不 trap） · `toSlice` ·
`clone` · `push` · `pop` · `shrink` · `clear` · `release`。
`vector::from(xs)`从一段视图拷出一个容器。

```extc
use std::io
use stl::vector
fn main() -> i32 {
    var v: vector<i64> = vector<i64>::new()
    v.push(i64(3))  v.push(i64(1))  v.push(i64(2))
    let s = v.toSlice()
    io::cout << "vector n=" << v.len() << " cap=" << v.capacity() << " sum=" << (s[0] + s[1] + s[2]) << "\n"
    return 0
}
```
实测：`vector n=3 cap=4 sum=6`。

### `string` + `stringio`：连续字节串与流式读写

`string`是 **SSO 字节串**：内联 16 字节，写满第 17 个字节才升级到池
；`cap == 0` 就是"还在内联"。零值合法且零分配。

| 方法 | 签名 | 位置 |
|---|---|---|
| 构造 | `string::create(c)` · 模块级 `string::withCap(c)` / `new()` / `empty()` / `from(b)` / `fromI64(n)` | |
| 读数 | `len` · `capacity` · `isEmpty` · `at(i) -> ?u8` | |
| 取内容 | `toSlice() -> slice<u8>`（**拷一份**）、`subView(lo, hi) -> slice<u8>`、`sub(lo, hi)` | |
| 拷贝 | `clone()`（内联态出来的仍是内联串） | |
| 比较 | `==` / `<`，右边可以是 `string` 也可以是 `slice<u8>`（所以 `s == "abc"` 成立） | |
| 拼接 | `concat(other) -> string`、`concatBytes(slice<u8>)`、就地 `append(slice<u8>)`、`push(u8)`、`appendByte` | |
| 查找 | `find(needle) -> ?i64`（无匹配 `none`）、`contains`、`startsWith`、`endsWith`、`hash()` | |
| 容量 | `grow()`（1.5 倍）、`shrink()`、`clear()`、`release()`、`removeFront(n)` | |
| 解析 | `string::toI64(b) -> ?i64` | |

**拿到内容只有 `toSlice()` 一条路（拷贝）**：池里不放视图出去。
`string` 的 `hash()` + `==` 齐了，所以它能直接当 `hashMap` 的键。

`stl::stringio` 用 `impl` 块把流式读写挂到 `string` 与 `io::istream` / `fs::ifstream` 上：

| 名字 | 签名 | 位置 |
|---|---|---|
| `string::readLineFrom` | `fn readLineFrom(self: mut ref string, r: mut ref io::reader) -> result<i64, io::ioError>` |  |
| `string::readAllFrom` | `fn readAllFrom(self: mut ref string, r: mut ref io::reader) -> result<i64, io::ioError>` | |
| `string::writeTo` / `writeLineTo` | `fn writeTo(self: ref string, w: mut ref io::writer) -> result<i64, io::ioError>` | |
| `io::istream >> string` / `fs::ifstream >> string` | 读一行（`\n` 与 CRLF 的 `\r` 都不算内容），**目标被替换** | |

```extc
use std::io
use std::fs
use stl::string
use stl::stringio

fn main() -> i32 {
    var r: fs::ifstream = fs::openRead("std-e-in.txt")?     // 内容 "alpha\nbeta\n"
    var s: string
    let n1 = s.readLineFrom(r.reader())?
    let n2 = s.readAllFrom(r.reader())?
    r.close()?
    io::cout << "n1=" << n1 << " n2=" << n2 << " s=" << s.toSlice() << "\n"
    return 0
}
```
实测：`n1=5 n2=5 s=alphabeta`（第一行不含换行）。

> 实测的坑：同一个程序里 `use stl::linmap` + `use stl::string`，并且真的实例化
> `linMap<i64, slice<u8>>`（`slice<u8>` 做值）时，生成的 C 里 `option_slice_u8` 会**重复定义**，
> C 编译失败。最小复现见本页末尾"今天会咬人的组合"第 3 条。

### `map` / `set`：**有序**（B+ 树）

裸名给**有序**实现：`map` / `set` 有序，`hashMap` / `hashSet` 哈希。

`map<K, V>`是 ORDER（16 键/节点）的 B+ 树，叶子按 `next` 串成链，所以
**按序遍历就是扫连续内存**；`cnt` 让 `keyAt` / `valAt` / `lowerBound` 变成 O(log n) 的排名查询
。**有序性来自键类型的 `<`**，`==` 由 `!(a<b) && !(b<a)` 派生
。

| 方法 | 签名 | 位置 |
|---|---|---|
| `withCap` / `new` | `fn withCap(want: i64) -> map<K, V>` / `fn new() -> map<K, V>` | |
| `len` / `capacity` 之类 | `len` · `nodeCount` · `capOf` · `height` | |
| `contains` / `get` / `[]` | `fn get(...) -> ?V`；缺键给 `none`（**不插入默认值**） | |
| `put` / `[]=` | `fn put(self: mut ref map<K, V>, k: K, v: V) -> bool`；**true = 键本来就在**（覆盖） | |
| `remove` | 返回是否删掉了；会借/并再平衡 | |
| 排名 | `lowerBound(k)`（严格小于 k 的个数）· `keyAt(i)` / `valAt(i)`（第 i 小） · `rankAt(i)` | |
| 两端 | `firstKey()` / `lastKey() -> ?K` | |
| `clone` / `clear` / `shrink` / `release` | 深拷贝、清空、缩高水位、整批还池 | |

`set<T>`就是"值丢掉"的 `map<T, u8>`：**一个实现，不会漂**
。`withCap` · `new` · `len` · `contains` · `put`（**true = 新元素**） ·
`remove` · `lowerBound` · `first` / `last` · `at(i)`（第 i 小） · `clone` · `clear`。

```extc
use std::io
use stl::map
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
use stl::set
fn main() -> i32 {
    var s: set<i64> = set<i64>::withCap(i64(4))
    s.put(i64(4))
    s.put(i64(2))
    io::cout << "first=" << (s.first() ?? -1) << " at1=" << s.at(i64(1)) << " lb3=" << s.lowerBound(i64(3)) << "\n"
    return 0
}
```
实测：`first=2 at1=4 lb3=1`。

### `hashMap` / `hashSet`：哈希

**键固定是 `i64`**：extC 没有 trait，编译器不能在类型参数上叫 `hash()`，所以哈希容器的键是具体的
`i64`，值才是泛型参数 `V`。键里可以打包两个 `i32`、一个 `pool` 句柄、
一个字符串表偏移。

`hashMapI64<V>`建在泛型 `hashMap<i64Key, V>`上；实现是
**线性探测 + 墓碑**，`cap` 是 2 的幂，探测下标是 `hash & (cap-1)`。

| 方法 | 签名 | 位置 |
|---|---|---|
| `withCap` | `fn withCap(want: i64) -> hashMapI64<V>` | |
| `len` / `capOf` / `deadOf` | `fn len(...) -> i64` 等 | |
| `contains` / `get` / `[]` | `fn get(...) -> ?V` | |
| `put` / `[]=` | `fn put(self: mut ref hashMapI64<V>, k: i64, v: V) -> bool`；**true = 键本来就在** | |
| `remove` / `clear` / `release` | 删、清、整批还池 | |
| 按桶扫 | `keyAt(i)` / `valAt(i)` / `liveAt(i)`（表位置；**不是插入序**） | |
| 按 dense 扫 | `denseLen()` / `keyAtDense(d)` / `valAtDense(d)` | |
| `clone` | 深拷贝 | |

重建有**两条触发线**：装载（`n + dead` 到 70% 容量）与墓碑密度（`dead` 到 25% 容量）
。

`hashSetI64`是"没有值的 map"（值是一格 `u8`），面与 `hashMap` 对齐，
**唯一差别**：`put` 返回 true 表示**新元素**。`withCap` · `len` ·
`contains` · `put` · `remove` · `clear` · `release` · `keyAt` · `liveAt` · `capOf` · `clone`。

```extc
use std::io
use stl::hashMap
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
use stl::hashSet
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

| `linMap<K, V>` | 签名 | 位置 |
|---|---|---|
| `withCap` | `fn withCap(want: i64) -> linMap<K, V>` | |
| `len` / `capOf` | `fn len(...) -> i64` / `-> i64` | |
| `find` / `contains` | `fn find(...) -> i64`（位置，`<0` = 没有） / `-> bool` | |
| `get` / `[]` | `fn get(...) -> ?V` | |
| `put` / `[]=` | 插入或覆盖；返回 true = 键本来就在 | |
| `remove` | swap-remove；返回是否删掉 | |
| `keyAt(i)` / `valAt(i)` | 第 i 个 entry（插入序） | |
| `clone` / `clear` / `grow` / `release` | | |

`linSet<T>`是 `linMap<T, unit>` 的薄包装：`withCap` · `add`（返回是否**新加**） ·
`contains` · `remove` · `len` · `clone` · `clear` · `release` · `at(i)`。

```extc
use std::io
use stl::linmap
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
use stl::linset
fn main() -> i32 {
    var s: linSet<i64> = linSet<i64>::withCap(i64(4))
    s.add(i64(7))
    io::cout << "has=" << s.contains(i64(7)) << " dup=" << s.add(i64(7)) << " len=" << s.len() << " at0=" << s.at(i64(0)) << "\n"
    return 0
}
```
实测：`has=true dup=false len=1 at0=7`。

### `pool<T>`：句柄寻址的槽表

值住在一个 **dense 连续**数组里，调用方拿的是**句柄**（`slot + generation + epoch`），不是指针
。**陈旧句柄不 trap**：查询式 API 给 `?T` / `bool`
（，`at(h)` 那种 trap 版要等 `@trackCaller`）。

| 名字 | 签名 | 位置 |
|---|---|---|
| `handle` | `struct handle { slot: i32, gen: i32, ep: i64 }`（普通值，拷贝无害） | |
| `same(a, b)` | `fn same(a: handle, b: handle) -> bool` | |
| `withCap` / `withParent` | `fn withCap(n: i64) -> pool<T>` / `fn withParent(parent: i64, n: i64)` | |
| `len` / `capacity` / `bytesOf` | | |
| `contains` / `get` / `set` | `fn get(...) -> ?T`；陈旧给 `none`；`set` 返回是否写成功 | |
| `insert` | `fn insert(self: mut ref pool<T>, v: T) -> handle`（优先复用洞链） | |
| `remove` | 返回是否删掉；**swap-remove**（dense 前缀保持连续）+ 该槽世代 +1 | |
| 迭代 | `atDense(i)` / `handleAtDense(i)` / `toSlice()`（拷一份） / `gather(hs, out)` | |
| `shrink` / `clear` / `release` | `clear` 翻 epoch（旧句柄一次比较全废），`release` 整批还 | |

```extc
use std::io
use stl::pool
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

| 名字 | 签名 | 位置 |
|---|---|---|
| `sort<T>` | `fn sort<T>(a: mut slice<T>)` |  |
| `isSorted<T>` | `fn isSorted<T>(a: slice<T>) -> bool` | |

语义：introsort（median-of-three + 深度上限后退到 heapsort），**原地、零分配**，栈是 64 项定长数组；
小区间（≤ 12）走插入排序；**不承诺稳定**；比较用键上的 `<`。
调用点写 `sort::sort(v)`，**不要**写 `sort::sort<i64>(v)`（那会被读成"类型当值"，报
`` `i64` is a type, not a value -- did you mean `i64(x)`? ``）。

```extc
use std::io
use stl::sort
fn main() -> i32 {
    var a: [5]i64 = [3, 1, 4, 1, 5]
    var v: mut slice<i64> = a[..]
    sort::sort(v)
    io::cout << a[0] << a[1] << a[2] << a[3] << a[4] << " sorted=" << sort::isSorted(v) << "\n"
    return 0
}
```
实测：`11345 sorted=true`。

## 3. `std::*`：模块

### `std::io`：控制台与缓冲的读写

| 名字 | 签名（节选） | 位置 |
|---|---|---|
| `ioError` | `\| readFailed(i32) \| lineTooLong(i64) \| writeFailed(i32) \| notATerminal(i32) \| closed(i32) \| destFull(i64)` |  |
| `cout` / `cerr` / `cin` | `ostream { fd: 1 }` / `errStream { }` / `istream { fd: 0 }` | |
| `endl` | `let endl: slice<u8> = "\n"` | |
| `ostream <<` | `i64` / `i32` / `f64` / `bool` / `slice<u8>` / `mut slice<u8>` | |
| `padText` / `padInt` / `padF` | 宽度 + 填充字节 + `LEFT`/`RIGHT`/`CENTER` | ；常量 |
| `writer` | `struct writer { fd, buf, len }`（`@noCopy`）；`write` / `flush` / `cap` | |
| `writerOf(fd)` | 造一个带 64 KiB 缓冲的 writer | |
| `reader` | `avail` / `buffered` / `fill` / `nextByte` / `eof` / `nextLine` / `nextLineRaw` / `skipSpace` / `nextToken` / `nextInt` | |
| `readerOf(fd)` | 造一个带 64 KiB 缓冲的 reader | |
| `istream >>` | `mut ref i64` / `mut slice<u8>` / `mut ref u8`；`bad()` / `clear()` | |
| `flushOut` / `writeBytes` / `writeBytesRaw` | 冲 stdio 侧 / 进 cout 缓冲 / 直写 fd 1 | |

关键语义：

- **EOF 是 `success(0)`，不是错误**；`readFailed` / `lineTooLong` / `writeFailed` / `closed` / `destFull` /
  `notATerminal` 是分开的失败格。
- `io::writer` 有**自己的** 64 KiB 缓冲：不 `flush()` 就退出，里面没冲的字节**丢**
  。
- `nextInt` 会**跳过非数字往前找**（`hello world 99` 读出 99）。
- `cin >> line` / `fin >> line` **不跳过空白**、**不返回长度**。
- 读全是阻塞式 `read(2)`：没有超时、没有非阻塞、没有 select/poll。
- `ostream` **没有** `<< u8`（报 ``'io::ostream' does not define '<<' for 'u8'``），`cerr` 也没有 `f64`；
  没有 `seek` / `tell` / `peek` / `unget`（`grep` 整个 `io.extc` 无这些名字）。

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

补充现状（源码位置都在 ）：

| 名字 | 位置 | 一句话 |
|---|---|---|
| `ifstream::new` / `readSome` / `readAll` / `reader` / `bad` / `clear` / `close` | | 读句柄；`close` 幂等；用到已关句柄是 `closed(fd)` 值，不是 trap |
| `ifstream >> i64 / slice<u8> / u8` | | 与 `cin >>` 同一套语义；失败置 `bad()`，目标保持原值 |
| `ofstream::new` / `append` / `put` / `close` | | `put` 直接 `write(2)`（**不缓冲**）；`close` 是提交点 |
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

| 名字 | 签名 | 位置 |
|---|---|---|
| `terminal` | `struct terminal { fd, saved[128], rawbuf[128], on }`（两个缓冲是不透明的 `struct termios`） |  |
| `terminal::raw` / `restore` / `close` | 都返回 `result<unit, io::ioError>`；`close` 就是 `restore`，幂等 | |
| `rawTerminal(fd)` | `fn rawTerminal(fd: i32) -> result<terminal, io::ioError>` | |

语义：声明了 `close` ⇒ 它是**资源**，"开了没关"编译器会提醒；
不是终端时给 `failure(notATerminal(fd))`（不是 trap）；trap 路径会经运行期钩子把终端还原
（；判据  里 `raw-trap` 一节）。
**没有** size / color / 光标 / 清屏 / 按键解码（`grep` 整个 `term.extc` 只有 raw 开关）。

```extc
use std::io
use std::term
fn main() -> i32 {
    match term::rawTerminal(io::STDIN_FD) {
        failure(e) => { match e { notATerminal(fd) => { io::cout << "not-a-tty fd=" << fd << "\n" }
                                 readFailed(fd) => { io::cout << "readFailed\n" }
                                 lineTooLong(n) => { io::cout << "lineTooLong\n" }
                                 writeFailed(fd) => { io::cout << "writeFailed\n" }
                                 closed(fd) => { io::cout << "closed\n" }
                                 destFull(n) => { io::cout << "destFull\n" } } }
        success(t) => { var tt: term::terminal = t
                        tt.restore()?
                        tt.close()?
                        io::cout << "raw-ok\n" } }
    return 0
}
```
实测（stdin 不是终端，`</dev/null`）：`not-a-tty fd=0`。

### `std::time`：两个钟 + 秒表

| 名字 | 签名 | 位置 |
|---|---|---|
| `now()` | 单调钟，纳秒；**不会倒退**，测时长用它 |  |
| `real()` | 墙钟，纳秒（Unix epoch 起算）；会跳（NTP/时区/改表） | |
| `sleepNs` / `sleepUs` / `sleepMs` | 阻塞当前线程 | |
| `timer` + `start()` | `elapsedNs` / `elapsedUs` / `elapsedMs` / `elapsedSecs`；`t0` 是 `@private` | |

没有 `perf(fn)`：闭包不能当参数。

```extc
use std::io
use std::time
fn main() -> i32 {
    let a = time::now()
    time::sleepMs(i64(2))
    var t = time::start()
    time::sleepUs(i64(1000))
    io::cout << "mono-advances=" << (time::now() > a) << " elapsedUs-positive=" << (t.elapsedUs() > i64(0)) << " real-positive=" << (time::real() > i64(0)) << "\n"
    return 0
}
```
实测：`mono-advances=true elapsedUs-positive=true real-positive=true`。

### `std::json`：零拷贝解析 + 零分配写入

| 名字 | 签名 | 位置 |
|---|---|---|
| 节点种类 | `OBJ=1` `ARR=2` `STR=3` `NUM=4` `BOOL=5` `NUL=6`；`MAXDEPTH=64` |  |
| `node` / `parsed` | 带载荷枚举 `\| obj \| arr \| text \| number \| flag \| nul` / `\| ok(i64) \| err(i64)` | |
| `parse(text, nodes)` | 节点池**由调用方给**；池满报错，不静默截断 | |
| `isOk` / `rootOf` / `errAt` | 投影（也可以直接 `match`） | |
| 访问器 | `kindOf` · `countOf` · `rawOf`（原文视图） · `escOf` · `unescape(text, nodes, idx, out)` · `strOf(..., scratch)` | |
| 结构访问 | `member` · `keyAt` · `valueAt` · `elem` · `find(text, nodes, root, "a.b.0")` | |
| 取值 | `numOf -> f64` · `intOf -> i64`（只在 ≤18 位十进制时给值，否则 0） · `boolOf` | |
| `builder` | `builder::open(buf) -> ?builder`（含引用，**没有零值**）；`ok` `len` `beginObj` `endObj` `beginArr` `endArr` `key` `str` `int` `boolean` `nul` `raw` `flt` | |

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

### `std::b64`：Base64

| 名字 | 签名 |
|---|---|
| `encodedLen(n)` / `decodedMax(n)` | 目标缓冲该多大 |
| `encode` / `decode` | `fn encode(src: slice<u8>, out: mut slice<u8>) -> i64`；标准字母表（RFC 4648 第 4 节） |
| `encodeUrl` / `decodeUrl` | URL 安全字母表（`-` `_`，RFC 4648 第 5 节）；两种都**带 `=` 填充** |

返回值是**写出的字节数**；缓冲不够 / 非法字符 / 长度不是 4 的倍数都给 `-1`
（、）。不做流式。

```extc
use std::io
use std::b64
fn main() -> i32 {
    var enc: [64]u8
    let n = b64::encode("hello", enc[..])
    var dec: [64]u8
    let m = b64::decode(enc[0..n], dec[..])
    io::cout << enc[0..n] << " " << dec[0..m] << " " << b64::decode("!!!!", dec[..]) << "\n"
    return 0
}
```
实测：`aGVsbG8= hello -1`（`aGVsbG8=` 与 `echo -n hello | base64` 一致）。

### `std::hash`：sha1 / sha256 / hmac

| 名字 | 签名 | 位置 |
|---|---|---|
| `sha256::sum(data, out) -> bool` | `out.len < 32` 给 `false` |  |
| `sha256::hex(digest, out) -> i64` | 十六进制，返回长度 | |
| `sha256::state` + `init` / `update` / `final` | 分块哈希 | |
| `sha1::sum` / `hex` / `state`（20 字节摘要） | | 、|
| `hmac::sha256(key, msg, out) -> bool` | `out.len < 32` 给 `false`；键长 > 64 先散列一次（RFC 2104） |  |
| `hmac::sha1(key, msg, out) -> bool` | | |

零分配（state + 64 字节块都在栈上）。

```extc
use std::io
use std::hash::sha256
use std::hash::sha1
use std::hash::hmac
fn main() -> i32 {
    var d: [32]u8
    sha256::sum("abc", d[..])
    var hex: [64]u8
    let hn = sha256::hex(d[..], hex[..])
    io::cout << "sha256=" << hex[0..hn] << "\n"
    var d1: [20]u8
    sha1::sum("abc", d1[..])
    var h1: [40]u8
    let h1n = sha1::hex(d1[..], h1[..])
    io::cout << "sha1=" << h1[0..h1n] << "\n"
    var mac: [32]u8
    hmac::sha256("key", "msg", mac[..])
    var mh: [64]u8
    let mhn = sha256::hex(mac[..], mh[..])
    io::cout << "hmac=" << mh[0..mhn] << "\n"
    return 0
}
```
实测（与 `sha256sum` / `sha1sum` / `openssl dgst -sha256 -hmac key` 三方对齐）：
`sha256=ba7816bf…20015ad` · `sha1=a9993e36…9cd0d89d` · `hmac=2d93cbc1…1b1b8c628`。

### `std::net`：套接字与名字解析

`use std::net` 是公开门面；epoll 三件**不在这里**（属于 `std::loop` 与协程调度器）
。约定与内核一致：失败给**负数**，`>= 0` 是 fd / 字节数。

| 名字 | 签名 | 位置 |
|---|---|---|
| `sockPair(out)` | 一对已连接的 AF_UNIX fd |  |
| `accept` / `read` / `write` | `fn read(fd: i64, buf: mut slice<u8>) -> i64` 等 | |
| `nonblock` / `nodelay` / `shutdownWr` / `timeout` / `localPort` | | |
| `tcpListen` / `tcpListenShared` / `tcpConnectTo` | `SO_REUSEPORT` 版给多进程分片 | |
| `dnsLookup(host, out)` / `resolve(addr, out)` | 主机名 → IPv4；`resolve` 也接受字面量 | |

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

| 名字 | 签名 | 位置 |
|---|---|---|
| `parseRequest(raw) -> request` | 拆请求行 + 头 |  |
| `request::path` / `query(name)` / `header(name)` / `length` / `isGet` / `isPost` | | |
| `parseResponse(raw) -> response` | 拆状态行 + 头 | |
| `response::header` / `length` / `isOk` / `is(status)` | | |
| `reasonOf` / `headerOf` / `contentLength` / `pathOf` / `queryOf` | 自由函数版本 | |
| `beginResponse` / `putHeader` / `putHeaderI64` / `endHeaders` / `simpleResponse` / `beginRequest` | 拼字节 | |
| `decode(src, dst)` | 百分号解码 | |
| `conn::open` / `send` / `readResponse` / `exchange` / `get` / `post` / `close` | 一条连接上的往返 | |
| `session::make` / `ok` / `exchange` / `get` / `post` / `close` | 会自己重连的那种 | |

解析与拼字节都**离线可跑**（判据 ）：

```extc
use std::io
use std::http
fn main() -> i32 {
    let raw: slice<u8> = "GET /a/b?x=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc"
    var r: http::request = http::parseRequest(raw)
    io::cout << "method=" << r.method << " path=" << r.path() << " host=" << (r.header("host") ?? "none") << " q=" << (r.query("x") ?? "none") << " len=" << r.length() << " isGet=" << r.isGet() << "\n"
    let resp: slice<u8> = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"
    var p: http::response = http::parseResponse(resp)
    io::cout << "status=" << p.status << " reason=" << http::reasonOf(p.status) << " ok=" << p.isOk() << " len=" << p.length() << "\n"
    return 0
}
```
实测：`method=GET path=/a/b host=h q=1 len=3 isGet=true` / `status=200 reason=OK ok=true len=2`。

### `std::ws`：WebSocket 帧

| 名字 | 签名 | 位置 |
|---|---|---|
| 操作码 | `OP_CONT=0` `OP_TEXT=1` `OP_BIN=2` `OP_CLOSE=8` `OP_PING=9` `OP_PONG=10`；`GUID` |  |
| `acceptFor(key, out)` | `Sec-WebSocket-Accept`（base64(SHA1(key + GUID))） | |
| `requestInto(host, path, key, out)` | 拼握手请求 | |
| `frameInto(op, fin, payload, mask, out)` | 拼一帧 | |
| `hdrLen` / `opOf` / `finOf` / `maskedOf` / `lenOf` / `maskAt` / `unmask` | 拆帧 | |

判据 。`acceptFor` 与 RFC 6455 的例子逐字节一致：

```extc
use std::io
use std::ws
fn main() -> i32 {
    var acc: [64]u8
    let n = ws::acceptFor("dGhlIHNhbXBsZSBub25jZQ==", acc[..])
    var m: [4]u8 = [1, 2, 3, 4]
    var frame: [64]u8
    let fl = ws::frameInto(ws::OP_TEXT, true, "hi", m[..], frame[..])
    io::cout << "accept=" << acc[0..n] << " op=" << ws::opOf(frame[0..fl]) << " fin=" << ws::finOf(frame[0..fl]) << " masked=" << ws::maskedOf(frame[0..fl]) << " len=" << ws::lenOf(frame[0..fl]) << "\n"
    return 0
}
```
实测：`accept=s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`（与 `openssl dgst -sha1 -binary | base64` 一致）·
`op=1 fin=true masked=true len=2`。

### `std::proc`：进程

| 名字 | 签名 | 位置 |
|---|---|---|
| `exit(code)` | 立即结束进程；**不 flush 你自己的 `writer`** |  |
| `spawn()` | POSIX `fork`：父得子 id，子得 0，失败 -1；**子从这一句继续** | |
| `wait(pid)` | 回收并给退出状态（被信号杀死的报 0） | |
| `self()` | 本进程 id | |
| `pin(cpu)` | 绑核（`cpu < 0` 解除），返回 0 成功 | |

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

`parallel::run(worker, <视图实参>…, n, threads) -> i32`是 `@builtin`：
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

| 名字 | 签名 | 位置 |
|---|---|---|
| `loop` | 调度器状态，**只有标量**（含引用的结构体没有零值） |  |
| `tasks<T>` | 任务表（调用者自己的容器）：`rows` / `runq` / `rqHead` / `freeq` / `acceptq` | |
| `tasks::add` | `fn add(self: mut ref tasks<T>, s: mut ref loop, h: coroutine<i64>) -> bool` | |
| `tasks::step` / `resumeRow` / `finish` / `runScripted` | 推进一行 / 一路推到挂起 / 收尾 / 按顺序推进 | |
| `tasks::pump` / `runEpoll` | 真事件循环：`>=0` 新 accept 的 fd，`-1` 处理了事件，`-2` 超时 | |
| `newRows()` / `newQ()` / `init(s, listener)` / `pendingTimers(s)` | 造表 / 造队列 / 初始化 / 定时器计数 | |

判据在 （脚本化驱动的 `coro_sched.extc` 用退出码自证，`coro_tasks.extc` 也是）：

```text
$ ./build/extc --run tests/coro/coro_sched.extc ; echo $?
36
$ ./build/extc --run tests/coro/coro_tasks.extc ; echo $?
10
```

### `std::heap`：板（一块连续内存 + 一道门）

一次 `mmap` 保留地址空间（`PROT_NONE` + `MAP_NORESERVE`），`alloc` 按需 `mprotect` 成 RW；**地址一给出就不变**；
每个出口验一次"在板内"（`holds`）；`close` 是 `munmap`，之后再用板内指针是硬缺页。`@noCopy`
。

| 名字 | 签名 | 位置 |
|---|---|---|
| `PAGE` / `CHUNK` / `DEFAULT_RESERVE` | `4096` / `1 MiB` / `4 GiB` | 、|
| `plate::open(reserve) -> ?plate` | 保留一片地址空间 | |
| `reservedBytes` / `committedBytes` / `usedBytes` / `remaining` / `isClosed` | | |
| `alloc(n) -> ?mut slice<u8>` · `allocPtr(n) -> ?ref u8` | 申请（`effects Ret=0`：返回的视图不指向调用方帧） | |
| `view(off, n)` · `viewAt(p, n)` · `holds(p)` · `holdsRange` · `holdsView` | 板内视图 / 从裸指针过门造视图 / 区间检查 | |
| `copyIn` / `copyOut` / `store` | 进/出板 | |
| `loanRO` / `loanRW` | 改页权限（临时只读 / 可写） | |
| `quarantine` / `unquarantine` / `poison` | 隔离 / 解除 / 填毒 | |
| `close()` | `munmap`，双关安全 | |

```extc
use std::io
use std::heap
fn main() -> i32 {
    var pl: heap::plate = heap::plate::open(heap::DEFAULT_RESERVE)!
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

| 名字 | 签名 | 位置 |
|---|---|---|
| `open(path) -> option<ref void>` | `path` **必须 NUL 结尾**：字面量直接传，运行期拼的先 `cstr` |  |
| `sym(h, name) -> option<ref void>` | `dlsym`；给的是**地址，不是类型** | |
| `symOrNull(h, name) -> ?ref void` | 要自己判空时用它 | |
| `close(h)` / `cstr(s)` / `cstrLen(p)` / `viewCStr(p)` | 关 / 补终止符 / C 字符串长度 / C 字符串 → 视图 | |

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
实测：`triple=42 close=0`。**注意 `close` 必须另起一句**（见"今天会咬人的组合"第 1 条）。

### `std::mem`：字节块的查找与比较

| 名字 | 签名 | 位置 |
|---|---|---|
| `find(hay, needle) -> i64` | 最左匹配偏移；没有 `-1`；空模式 `0` |  |
| `contains(hay, needle) -> bool` | | |
| `eq(a, b) -> bool` | 整块比较（`n <= 0` 算相同） | |
| `copy(dst, src) -> i64` | 原地整块拷贝；装不下 `-1` | |

`stl::string` 的 `find` / `contains` 建在它上面。

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

### `std::loop` 与 `std::uuid`

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
use std::uuid
fn main() -> i32 {
    var t: loop::timers
    t.init()
    t.add(i64(100), i64(7))
    t.add(i64(50), i64(8))
    var out: [4]i64
    let due = t.popDue(i64(100), out[..])
    var id: [16]u8
    uuid::v5dns("example.com", id[..])
    var u: [36]u8
    let n = uuid::format(id[..], u[..])
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
|---|---|---|
|  | `read` / `write` / `open` / `close`、`O_*`、`STDIN/STDOUT/STDERR`、cout 运行期 | `std::io`、`std::fs` |
|  | `tcgetattr` / `tcsetattr` / `cfmakeraw` / `TCSANOW` / raw 钩子 | `std::term` |
|  | `extc_time_mono_ns` / `extc_time_real_ns` / `extc_time_sleep_ns` | `std::time` |
|  | `extc_file_open` / `extc_file_close` / `extc_sendfile` | `std::fs`（`struct stat` 不过界） |
|  | 裸 fd、epoll、socket、DNS 运行期 | `std::net`、`std::loop`、`std::coro::scheduler` |
|  | `mmap` / `mprotect` / `munmap`、`extc_viewOf`、`extc_memCopy` | `std::heap` |
|  | `fork` / `waitpid` / `getpid` / `sched_setaffinity` / `exit` | `std::proc` |
|  | 运行期池记录 + 板块原语 | `stl::pool` / `stl::vector`（编译器合成调用） |
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

1. **`io::cout` / `<<` 链的实参是从右往左求值的。** 一条 `<<` 链里写两个有副作用的调用，右边的先跑。
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
   实测输出：`BAchain=12` / `ABsum=3` —— `<<` 链里 `b()` 先于 `a()`，而 `+` 是左到右。
   **后果**：`io::cout << f(x) << dl::close(h)` 会**先关**再调，调用已卸载的代码 ⇒ 段错误
   （实测：`./std-d-dl1.bin` 报 `Segmentation fault`，退出码 139）。
   资源释放（`close`）永远**另起一句**、先把要打印的值取出来。

2. **`??` 在一条语句里会"提前"算。** 编译器自己会拦下一部分：

   ```text
   error: `??` here would run **before** the call that comes earlier in this statement; split the line so the order you read is the order it runs
   note: The subject of `??` is computed into a temporary **before the whole statement** (that is what makes it run only once). ...
   ```
   实测（`io::cout << "had=" << m.put(k, v) << " v=" << (m.get(k) ?? -1)`）：拆成 `let had = ...` /
   `let v = m.get(k) ?? -1` 再打印即可。

3. **`use stl::linmap` + `use stl::string` + 实例化 `linMap<i64, slice<u8>>`**：生成的 C 里
   `option_slice_u8` 重复定义，C 编译失败：

   ```text
   build/…c:371:8: error: redefinition of 'struct option_slice_u8'
   extc: C compiler failed (exit 1) on `build/…c`
   ```
   把值类型换成 `i64`（不产生 `option<slice<u8>>`）或去掉 `stl::string` 就过。

4. **`use std::io` 会把 `std::sys::io` 的名字一起并进那张扁平表**，于是用户文件里不能再声明
   `read` / `write` / `open` / `close`：

   ```text
   std-d-w4.extc:2:1: error: duplicate function `write`
   ```
   （ 声明了这四个。普通声明会按模块改名，而 `extern!` **不改名**
   ——它必须保持 C 符号名，所以同一个 C 符号声明两次就撞；模块合并会把依赖一起并入。）
