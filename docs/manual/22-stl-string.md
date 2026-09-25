# 标准库参考：`string`

`stl::string` 是**池底**的连续字节串：内容连续（可整体取出传给 C 函数）、容量按 1.5 倍增长、
`release()` 整批还给池。池底意味着**存储不外借**：凡交出内容的接口，要么拷贝（`toSlice` / `sub`），
要么明确签字（`subView`）。

用法：`use stl::string`。

## 构造

| 方法 | 说明 |
|---|---|
| `string::new()` | 空串（首次增长时按 1.5 倍扩容） |
| `string::withCap(n)` | 预分配 `n` 字节容量 |
| `clone()` | 深拷贝，得到独立的一份（板与池记录都是新的） |

## 长度与容量

| 方法 | 说明 |
|---|---|
| `len()` | 当前字节数 |
| `capacity()` | 当前容量（不小于 `len()`） |
| `isEmpty()` | `len() == 0` |
| `clear()` | 长度归零，**保留容量** |
| `shrink()` | 容量收缩到不小于当前长度（下限 16 字节） |
| `release()` | 把整块存储还给池；对**陈旧拷贝**是空操作（不会误放别人的池） |

## 写入

| 方法 | 说明 |
|---|---|
| `push(b: u8)` | 追加一个字节 |
| `append(bytes: slice<u8>)` | 追加一段字节（整段 `memmove`，不是逐字节循环） |
| `appendByte(b: u8)` | 与 `push` 同义，供链式书写 |
| `removeFront(n)` | 从头部删除 `n` 个字节（余下部分前移） |

## 读取

| 方法 | 说明 |
|---|---|
| `at(i) -> ?u8` | 第 `i` 个字节；越界返回 `none`（不是 trap） |
| `toSlice() -> slice<u8>` | **拷贝**整串内容；结果落在调用者选的地方，可以存、可以返回 |
| `sub(lo, hi) -> slice<u8>` | **拷贝**子串（区间自动夹到合法范围） |
| `subView(lo, hi) -> slice<u8>` | **零拷贝视图**：只在字符串不再变化时有效，增长即失效；与 `!`、`extern!` 同类，**由调用者签字** |

## 查找与比较

| 方法 | 说明 |
|---|---|
| `find(needle: slice<u8>) -> i64` | 首次出现的位置；未找到返回 `-1`（线性时间，Two-Way 算法） |
| `contains(needle)` | 是否包含 |
| `startsWith(prefix)` / `endsWith(suffix)` | 前缀 / 后缀判定 |
| `bytesEq(a, b) -> bool` | 逐字节相等 |
| `bytesLt(a, b) -> bool` | 字典序小于 |
| `hash() -> i64` | 供 `hashMap<string, V>` 一类的键使用 |

## 拼接

| 方法 | 说明 |
|---|---|
| `concat(other: string) -> string` | 新串 = 自身 + `other` |
| `concatBytes(bytes: slice<u8>) -> string` | 新串 = 自身 + 一段字节 |

## 流式 I/O（在 `stl::stringio` 中挂载）

| 方法 | 说明 |
|---|---|
| `readLineFrom(r: mut ref io::reader) -> result<i64, io::ioError>` | 读一行追加进串（去掉行尾换行；CRLF 亦处理）；`0` 表示读到结尾 |
| `readAllFrom(r) -> result<i64, io::ioError>` | 读到结尾，每次整块追加 |
| `writeTo(w: mut ref io::writer) -> result<i64, io::ioError>` | 整串写给 writer（零拷贝直取，由 writer 拷进自己的缓冲） |
| `writeLineTo(w) -> result<i64, io::ioError>` | 整串 + 换行 |

## 按约定属于内部的成员

`buf` `n` `cap` `pid` `pidGen` `grow` `maxSuffix` 等：可达（编译器不阻止），但不受兼容性承诺保护。
完整清单一律见 [内部成员一览](20-internals.md)，不在此重复。
