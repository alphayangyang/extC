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

## `std::proc`

| 函数 | 签名 | 说明 |
|---|---|---|
| `exit` | `exit(code: i32)` | 以给定退出码结束进程（不返回；`main` 的返回值是常规路径）|
| `spawn` | `spawn() -> i32` | 复制出子进程：父进程得到子进程 id（> 0），子进程得到 0，失败得到 -1 |
| `wait` | `wait(pid: i32) -> i32` | 等一个子进程结束并返回它的退出码（`-1` 表示收不回来）|
| `self` | `self() -> i32` | 本进程的 id |
| `pin` | `pin(cpu: i64) -> i32` | 把本进程钉到一个核（`cpu < 0` 取消钉核）；返回 0 表示成功 |

**多进程是 extC 的第一并行形态**（`docs/topics/CONCURRENCY.md` 第 5.1 节）：内核按流哈希把新连接
分给各进程，因此进程之间不共享内存，也就不需要可发送性规则，`extc_pool_*` 与 `extc_task_*` 天然
各进程一份，某个 worker 崩溃只带走它自己的连接。配套只有两样：监听用
`net::extc_tcp_listen_shared`（在 `bind` 之前设 `SO_REUSEPORT`），启动用 `proc::spawn` 或脚本
`tools/extc-workers.sh`。完整例子见 `tests/coro/echo_server_fork.extc`。

两条纪律：

- **子进程当场 `exit`**。`fork` 之后两侧从同一行继续执行，子进程若返回原处，循环会再次 `fork`，
  如此重复即成炸弹。标准写法是 `let pid = proc::spawn()  if pid == 0 { proc::exit(serve(i)) }`；
- **每个 worker 自行钉核**。不钉核时调度器可能把 worker 放到小核上：同一负载实测 8 个 P 核给
  7.3x，而 16 个混合核只给约 10x。

实测（2026-09-28 · 8 个 P 核 · loopback · 256 连接 · ping-pong · `echo_server_fork.extc` 自行
`fork`，无外部启动器、无 `LD_PRELOAD`）：1 个 worker 327,556 req/s ⇒ 8 个 worker
**1,332,407 req/s（4.07x）**，流水线 4,963 MB/s，服务端 CPU 7.6 核，常驻 10 MB。同期 Go 1.26 的
八个 goroutine 8 线程为 304,764 req/s（0.93x），nginx 1.28.3 的八个 worker 为 1,064,775 req/s。

本族只保证 POSIX（Linux / macOS）：实现是 `extern!("libc")` 的 `fork`、`waitpid`、`getpid` 与
`sched_setaffinity`；Windows 需要另开一层。
