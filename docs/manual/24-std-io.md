# 标准库参考：`std::io`

控制台与缓冲 I/O 的底层设施：`reader` 负责"从 fd 读、按块填充"，`writer` 负责"攒够再写"，
控制台输入的那份全局状态由 `in*` 一族读写。

用法：`use std::io`。

## `reader`：按块读

| 方法 | 说明 |
|---|---|
| `avail() -> i64` | 缓冲区里尚未消费的字节数（不触碰 fd） |
| `buffered() -> bool` | `pos < len`，即缓冲区是否还有字节 |
| `nextByte() -> result<i64, ioError>` | 下一个字节；返回值 `< 0` 表示读到结尾（结尾不是错误） |
| `fill() -> result<i64, ioError>` | 重新填满一个块；`0` 表示读到结尾 |
| `nextLine(out: mut slice<u8>) -> result<i64, ioError>` | 读一行，**不含换行**（CRLF 的 `\r` 一并去掉） |
| `nextLineRaw(out: mut slice<u8>) -> result<i64, ioError>` | 读一行并**保留磁盘上的原始字节**（含 `\r`）：进度条、按字节精确复制的场合用 |
| `nextToken(out: mut slice<u8>) -> result<i64, ioError>` | 读一个词（连续非空白字符） |

`nextLine` 与 `nextLineRaw` 是**两个名字各自对应一种语义**，而不是一个名字两种含义。

## `writer`：攒够再写

| 方法 | 说明 |
|---|---|
| `writerOf(fd: i32) -> writer` | 在指定 fd 上构造 writer |
| `write(data: slice<u8>) -> result<i64, ioError>` | 写入（缓冲区满时自动冲刷） |
| `flush() -> result<i64, ioError>` | 冲刷缓冲区 |

## 控制台输入状态

| 方法 | 说明 |
|---|---|
| `inSync() -> reader` | 取控制台输入的那份全局状态（缓冲位置在多次读取之间保持） |
| `inSave(r: ref reader)` | 读完之后把位置写回全局状态 |
| `inBad() -> bool` / `inClear()` | 查询 / 清除"上一次读取失败或已到结尾"的标志 |
| `istream::bad() -> bool` | 输入流级的失败标志（`>>` 失败时置起，不 trap） |


`>>` 重载（含 `string` 版本，见 [`stringio`](23-stl-stringio.md)）的实现都建立在这三条之上。

| 成员 | 说明 |
|---|---|
| `writeBytesRaw(buf: slice<u8>) -> i64` | 直接向标准输出写一段字节（不经缓冲与格式化），返回写入长度 |
| `writeBytes(buf: slice<u8>) -> i64` | 同上，但走常规路径（含必要的收尾） |
