# 标准库参考：`string`

`stl::string` 是**短串内联**的连续字节串（SSO，short string optimization）：**零值就是一块合法的内联空串**
——16 字节就地存储在值里，不建池、不分配；只有写到**第 17 个字节**才升级到池。升级之后是池底容器：
容量按 1.5 倍增长、`release()` 整批还给池。池底意味着**存储不外借**：凡交出内容的接口，要么拷贝
（`toSlice` / `sub`），要么明确签字（`subView`）。

用法：`use stl::string`。

## 内部布局

| 字段 | 含义 |
|---|---|
| `small` | 内联缓冲 `[16]u8`：零值即内联空串，`len()` 个字节就住在这里 |
| `n` | 当前字节数 |
| `cap` | **0 = 内联**（内容在 `small` 里）；非 0 = 池里，且 `cap` 就是池块容量（不小于 16） |
| `big` | `?mut slice<u8>`：**只有升级到池之后才有值**，指向池板块；存一次、之后复用 |
| `pid` / `pidGen` | 池下标与建池时的世代（升级时填写；内联态不参与判断）|

判据只有一条：**`cap == 0` 就是内联**。`pid` 不能当哨兵用（第一个池的下标恰好是 0）；内联态没有池，
所以短暂的串**完全不碰池**，也不参与池的登记与回收。

## 构造

| 写法 | 说明 |
|---|---|
| `var s: string` | 空串：内联、零分配、`capacity()` 为 0（0 表示"没有池"）|
| `string::new()` / `string::empty()` | 与零值**完全等价**的空串（模块级函数，写法直白些）|
| `string::withCap(n)` | 预分配 `n` 字节池容量（下限 16）：立刻建池，`capacity()` 不小于 16 |
| `string::create(n)` | 与 `withCap(n)` 同义（类型上的落点，模块级入口是 `withCap`）|
| `clone()` | 深拷贝，得到独立的一份（短串拷出来仍是内联的，不建池）|

```extc
use std::io
use stl::string

fn main() -> i32 {
    var s: string                     // 内联空串：零分配
    s.append("hi")                    // 3 个字节就地写进 small
    var t = string::new()             // 同上，写法直白
    t.append(s.toSlice())
    var u: string = string::withCap(i64(64))   // 显式要容量 ⇒ 建池
    u.append(t.toSlice())
    io::cout << s.len() << " " << u.capacity() << "\n"
    return 0
}
```

## 长度与容量

| 方法 | 说明 |
|---|---|
| `len()` | 当前字节数 |
| `capacity()` | 当前容量：**内联返回 0**（没有池）；池态返回池块容量（不小于 `len()`）|
| `isEmpty()` | `len() == 0` |
| `clear()` | 长度归零，**保留存储**（内联态保留那 16 字节，池态保留池块）|
| `shrink()` | 容量收缩到不小于当前长度（下限 16 字节）；**内联态没有可缩的，是空操作** |
| `release()` | 把池整批还回去；内联态是空操作；对**陈旧拷贝**是空操作（不会误放别的池）|

`release()` 之后这个值收成**空的、合法的内联串**：`len()` 与 `capacity()` 都是 0，可以继续当空串用。
`toSlice()` 之类的读取因此不会去碰已还给池的内存。

## 写入

| 方法 | 说明 |
|---|---|
| `push(b: u8)` | 追加一个字节 |
| `append(bytes: slice<u8>)` | 追加一段字节（整段 `memmove`，不是逐字节循环）|
| `appendByte(b: u8)` | 与 `push` 同义，供链式书写 |
| `removeFront(n)` | 从头部删除 `n` 个字节（余下部分前移）|

**升级规则**：前 16 个字节就地写进 `small`；第 17 个字节触发升级——建池、取一块 32 字节的板块、
把 16 个内联字节搬过去，此后 `capacity()` 就是池块容量，增长按 1.5 倍（换块时旧块当场还回去，
不留历代旧副本）。

## 读取

| 方法 | 说明 |
|---|---|
| `at(i) -> ?u8` | 第 `i` 个字节；越界返回 `none`（不是 trap）|
| `toSlice() -> slice<u8>` | **拷贝**整串内容（内联态也拷：`string` 是值）；结果落在调用者选的地方，可以存、可以返回 |
| `sub(lo, hi) -> slice<u8>` | **拷贝**子串（区间自动夹到合法范围）|
| `subView(lo, hi) -> slice<u8>` | **零拷贝视图**：只在字符串不再变化时有效，增长即失效；与 `!`、`extern!` 同类，**由调用者签字** |

`subView` 的视图在内联态指向**值自己**（`small`），所以除了"别再改它"，还多一条"别把它拷走"；
池态的视图指向池板块，与升级之前同一条规矩。要长期持有就用 `toSlice` 拷一份。

## 查找与比较

| 方法 | 说明 |
|---|---|
| `find(needle: slice<u8>) -> i64` | 首次出现的位置（**最左**）；未找到返回 `-1`；空模式返回 `0` |
| `contains(needle)` | 是否包含（走 `find` 的结果）|
| `startsWith(prefix)` / `endsWith(suffix)` | 前缀 / 后缀判定（一次整块比较）|
| `bytesEq(a, b) -> bool` | 逐字节相等（内层循环跳过边界检查 —— 长度已在上面对上）|
| `bytesLt(a, b) -> bool` | 字典序小于（同上）|
| `hash() -> i64` | 供 `hashMap<string, V>` 一类的键使用 |

**查找在运行期做**：`find` 调 `std::sys::mem` 的 `extc_memFind`，内部是 libc 的 `memmem`
（glibc 里是 Two-Way 加 `memchr` 首字节扫描；非 GNU 平台回退成 `memchr` 加 `memcmp`）。
交给 libc 的理由是**量出来**的：extC 自己那份 Two-Way 算法没错，但每个字节都过一次下标边界检查 ——
`bench/string_vs_cpp` 的 find 一格因此是 3172.7 ms 对 libstdc++ 的 138.8 ms；改走 `memmem` 之后
落到同一量级（12 个场景的校验和两边逐位相同，语义没动）。
语义判据一个字没改：`tests/stl/stringFind.extc` 仍是穷举对拍（`{a,b}` 上 1..4 的模式 × 0..10 的文本
加逐条边界），自重叠模式（`aa` 在 `aaaa` 里）仍然返回 `0`。

`bytesEq` / `bytesLt` / `hash` 的内层循环标了 `@unchecked`（§[12.5](18-modules.md)）：
范围由紧邻的那一行循环条件保证，源码注释里逐条写明了是哪一行。

## 拼接

| 方法 | 说明 |
|---|---|
| `concat(other: string) -> string` | 新串 = 自身 + `other` |
| `concatBytes(bytes: slice<u8>) -> string` | 新串 = 自身 + 一段字节 |

两条都从**零值**开始拼，所以结果不超过 16 字节时同样是内联的（不建池）；超过了才升级。

## 流式 I/O（在 `stl::stringio` 中挂载）

| 方法 | 说明 |
|---|---|
| `readLineFrom(r: mut ref io::reader) -> result<i64, io::ioError>` | 读一行追加进串（去掉行尾换行；CRLF 亦处理）；`0` 表示读到结尾 |
| `readAllFrom(r) -> result<i64, io::ioError>` | 读到结尾，每次整块追加 |
| `writeTo(w: mut ref io::writer) -> result<i64, io::ioError>` | 整串写给 writer（零拷贝直取，由 writer 拷进自己的缓冲）|
| `writeLineTo(w) -> result<i64, io::ioError>` | 整串 + 换行 |

## 按约定属于内部的成员

`small` `big` `n` `cap` `pid` `pidGen` `grow` 等：可达（编译器不阻止），但不受兼容性承诺保护。
存储字段从升级前的单个 `buf` 变成了 `small` / `big` 两态，判据只有一个 `cap == 0`。
完整清单一律见 [内部成员一览](20-internals.md)，不在此重复。

## 造一个 `string`：`string::from`

```extc
use stl::string

fn main() -> i32 {
  let s = string::from("abc")   // 拷贝一段字节；`from` 是这里唯一顺手的起点
  return i32(s.len()) }
```

没有它就只能 `withCap` + `append` 两行起步，而且 `var s: string = "abc"` 是**写不出来**的
（字面量的类型是 `slice<u8>`，extC 不做隐式转换 —— 作者口径：只加 `from`，不开特殊权限）。
