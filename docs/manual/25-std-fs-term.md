# 标准库参考：`std::fs` 与 `std::term`

## `std::fs`

文件流：`ifstream` / `ofstream` 之外，还有一组**与 `io::reader` 对接**的设施，使文件与控制台走同一条读取路径。

| 成员 | 说明 |
|---|---|
| `inSync() -> io::reader` | 取标准输入对应的 reader（与控制台状态共用缓冲） |
| `inSave(r: ref io::reader)` | 读完之后写回缓冲位置 |
| `outPut(s: slice<u8>)` | 向标准输出写一段字节（不经 writer 缓冲） |
| `ifstream::reader() -> mut ref io::reader` | 交出**内部那个** reader（不是新造的）⇒ 与 `>>` 混用不会跳字节 |
| `ifstream::readSome(out) -> result<i64, io::ioError>` | 读至多 `out.len` 字节 |
| `ifstream::readAll(out) -> result<i64, io::ioError>` | 读到结尾 |
| `ifstream::close()` / `ofstream::close()` | 关闭 |

## `std::term`

终端原始模式：进入后按键即时到达程序（不回显、不被行编辑吃掉），退出前必须恢复。

| 成员 | 说明 |
|---|---|
| `rawTerminal(fd: i32) -> result<terminal, io::ioError>` | 在 fd 上进入原始模式，返回可恢复的句柄 |
| `restore() -> result<unit, io::ioError>` | 恢复进入之前的状态（**必须**在退出路径上调用） |
| `raw() -> result<unit, io::ioError>` | 同一个句柄上的重复进入（幂等语义以实现为准） |
| `on: bool` | 当前是否处于原始模式（字段；按约定属状态，不承诺兼容） |

## 打开与关闭（含标准流的重定向）

| 成员 | 签名 | 说明 |
|---|---|---|
| 打开读 | `openRead(path: slice<u8>) -> result<ifstream, io::ioError>` | 返回可读文件流 |
| 打开写 | `openWrite(path: slice<u8>) -> result<ofstream, io::ioError>` | 截断写 |
| 打开追加 | `openAppend(path: slice<u8>) -> result<ofstream, io::ioError>` | 追加写 |
| 重定向输入 | `openIn(path: slice<u8>) -> result<unit, io::ioError>` | 把标准输入指向该文件：此后 `io::cin` 与 `fs` 的读取都走它 |
| 重定向输出 | `openOut(path: slice<u8>) -> result<unit, io::ioError>` | 把标准输出指向该文件 |
| 关闭重定向 | `closeIn()` / `closeOut() -> result<unit, io::ioError>` | 结束重定向 |
| 写入 | `ofstream::put(buf: slice<u8>) -> result<i64, io::ioError>` | 写一段字节，返回写入长度 |
| 失败标志 | `ifstream::bad() -> bool` | 上一次读取是否失败或已到结尾（与 `io::istream` 的 `bad()` 同义） |
