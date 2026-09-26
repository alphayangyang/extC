# 协程调度器（`std::coro::scheduler`）

**一句话**：一个 `while` 循环 + 一个事件源，握住一组 `coroutine<i64>` 句柄轮流推进。

## 形状

```extc
use std::coro::scheduler as sched

var s: sched::loop                                           /* 零初始化：只装标量（epoll、listener、计数）*/
if !sched::init(ref s, i64(-1)) { return i32(1) }            /* -1 = 不 accept，只驱动任务 */
var tasks: sched::tasks<i64> = { rows: sched::newRows() }    /* 任务表：调用者自己的容器 => 会长 */

tasks.add(ref s, worker(i64(10)))
tasks.add(ref s, worker(i64(100)))

var order: mut slice<i64> = new i64[12]
var k: i64 = 0
while k < i64(12) { order[k] = k % i64(2)  k = k + 1 }       /* 交替就绪 */

var left: i64 = tasks.runScripted(ref s, order)
```

三个类型各管一段：

| 类型 | 装什么 |
|---|---|
| `row` | 一行任务：句柄 `h`、两个标记、它当前注册在 epoll 里的 fd |
| `loop` | 只有标量：epoll 实例、listener、活任务数、本轮就绪 fd 的小数组 |
| `tasks<T>` | **任务表**：包着 `vector<row>`，句柄是 24 字节的普通值，容器内部是 `mut slice` => 元素是可写位置 |

各类型的成员一览：

| 成员 | 归属 | 作用 |
|---|---|---|
| `rows` | `tasks<T>` | 表体：一个 `vector<row>` |
| `live` | `loop` | 还没跑完的任务数 |
| `ready` / `nready` | `loop` | 本轮收上来的就绪 fd 与其个数 |
| `started` | `row` | 这一行是否已经启动过（第一次只 `next`，不 `send`） |
| `finish` | `tasks<T>` | 收尾一行：标 `done`、清掉登记、`live` 减一 |

任务表**会长、没有容量上限**，跑完的行由 `add` 就地改写复用（长跑服务器因此不会把表撑大）。表必须建在
**驱动它**的那个作用域里：存储是池的板块，生命周期跟着那个 place 走。库另外给的是裸容器 `newRows()`：
包装结构体不算容器 => 从函数里返回它会被"返回带引用的值"拒，因此组装在调用点写一行字面量。

## 挂起约定（最容易写错的一条）

- 协程 `yield` 一个**请求**（这里是它想等的编号），恢复后**重试**自己的操作。
- 驱动器 `send(v)` **本身就会恢复**协程 ⇒ 请求必须在恢复**之前**读出来：先 `value()` 看它挂起时
  留下的请求，再 `send(请求)` 把批准结果送进去 —— 本次恢复读到的正是它。写成"恢复 + 回显"一气呵成
  会**错开一格**（实测踩出来的）。
- 任务跑完**不要再驱动**，也**不要再读**：句柄过期，`extc_task_alive` 会**大声 trap**
  （`exit(70)` + 带源码位置的消息）。`step` 用 `done` 记住它；**`value()` 只在 `next()`/`step()`
  返回 true 之后有效** —— 想要"最后一个值"就得在最后一次成功推进时把它记下来，跑完再读会 trap
  （帧已经随任务回收了）。

## 两个驱动

- `tasks.step(ref s, i)` —— 推进一步（上面那套协议），返回它是否还活着。
- `tasks.runScripted(ref s, order)` —— **脚本化的就绪顺序**：按 `order` 里的下标依次推进，返回还剩几个
  活任务。判据先用它，因为它是**逐字节确定**的：不靠时间、不靠 socket、不靠内核调度顺序。
- `tasks.add(ref s, h)` —— 把一个句柄放进任务表（先复用跑完的行，没有再长一行）。
- `tasks.count()` —— 表里有多少行（含已跑完、等待复用的那些）。
- `sched::init(ref s, listener)` —— 建 epoll 实例并记下 listener（`i64(-1)` = 只驱动任务、不 accept）。
- `tasks.pump(ref s, timeout_ms)` —— **一轮**事件循环：注册请求 ⇒ 等 ⇒ **把这一轮就绪的全部处理掉**。
  listener 可读时它**accept 一个连接并把新 fd 返回给调用者**（用哪种协程去接它由调用者决定），
  没有新连接返回 -1、超时返回 -2。两条实测来的性能约定：请求没变就不再 `epoll_add`（内核里那条登记
  还在，重复注册只是白费一次系统调用）；就绪 fd 先收进小数组再一遍扫表（每个事件扫一遍全表是 O(N^2)）。
  另外 listener 是 level-triggered：一轮只 accept 一个，**遇到 listener 事件即停止排空**，
  否则排空循环会一次次拿到同一个 fd 而死转。
- `tasks.runEpoll(ref s, timeout_ms)` —— **真事件源**：`epoll_wait`（走 `std::sys::net` 的
  `extern!("extc-runtime")`）。协议与 `runScripted` 同一条，只是"谁就绪"改由内核告诉我们：

  1. 每个任务 push 到"要么在等真实事件、要么结束"为止；
  2. 它 `yield` 的请求就是它想等的 fd，**在产生的同一轮**就注册进 epoll（晚一轮会空等超时）；
  3. `epoll_wait` 报了就绪 fd 之后，用 `send(fd)` 恢复在等它的那个任务；
  4. **请求为负 = "别等，立刻再推我"** —— 协程收尾时用它交回最后一个值，否则它会去等一个已经被
     自己读空的 fd，永远不醒（这是实测踩出来的）。

  读写用运行期的 `recv`/`send`，**不是** `read`/`write`：后两个名字已经被 `std::sys::io` 用另一套
  签名占了，而两者会落在同一个编译单元里（`<unistd.h>` 一 include 就冲突）。

## 单线程循环里的铁律：**别阻塞在读上**

循环长这样：

```extc
while 还有活干 {
    var fd: i64 = tasks.pump(ref s, i64(0))      /* 0 超时 = 立刻回来 => 判据里快而确定 */
    if fd >= i64(0) { tasks.add(ref s, conn(fd, buf)) }    /* 新连接 => 建一个任务 */
    /* 收数据要用**非阻塞** fd：收到了就收，收不到就继续 pump，下一轮再试 */
}
```

这条是实测踩出来的：判据第一版在循环**之后**用一个阻塞 `read` 收齐所有回声 —— 只要有任何一个
回声还没到，进程就**永远停在那里**（>60 秒被杀 ✗）。单线程里"等"只能靠 `pump`，不能靠阻塞调用。

## 判据

`tests/coro/coro_sched.extc` —— 两个任务（step 分别 10 和 100）按交替的就绪顺序推进，断言退出码
**36**：`left * 100` 那一项就是"两个任务都跑完了"这条断言。

`tests/coro/coro_table.extc` —— 逃逸规则精度的判据：**泛型容器包着 `vector`，方法里往表里 push 句柄**，
退出码 **41**。它钉住的是"往自己的容器里存一个句柄"这件事不再被误拒。

`tests/coro/coro_accept.extc` —— 一个 listener 接 40 条连接：任务跑完把行还回来复用，退出码 **52**。

`tests/coro/coro_epoll.extc` —— **8 条 AF_UNIX 连接、单线程、一个 epoll 循环**：每条连接一个协程，
先写请求再跑循环，最后收回声累加，退出码 **36**（= 1+2+…+8），并要求 **ASan 干净**。数据在
socketpair 的内核缓冲里 ⇒ 立刻可读 ⇒ 判据不靠 sleep、不靠时间。

## 一个能连的 echo server

`tests/coro/echo_server.extc` 是一个**手动连**的 echo server（刻意不注册进任何套件：它会一直服务到
被中断）：

    build/extc -w --no-line-map -o /tmp/es.c tests/coro/echo_server.extc
    gcc -std=c11 -fwrapv -O2 -o /tmp/es /tmp/es.c && /tmp/es
    printf 'hello\n' | nc 127.0.0.1 7654

每连接的读缓冲是 16 KB（`new u8[16384]`）：压测要求两侧同一个缓冲大小，extC 侧最初是 1 KB 而参照
实现是 64 KB，于是流水线形态读出一个 5 倍的假差距（统一之后那几格反超）。

固定端口 7654（`SO_REUSEADDR` 已开），accept 出来的连接会设上 `TCP_NODELAY`（`extc_sock_nodelay`）：
少了它，小批量、多轮写会撞上 Nagle 与 delayed ACK 凑出来的 ~40ms 停滞（实测：流水线形态、1024 字节回显，
从 363 req/s 变成 73,736 req/s）。每条连接一个协程，流式回显，对端半关就地收尾并 `close`
（用的是 `std::sys::io` 的 `close`，不需要额外原语）。判据 `tests/coro/coro_echo.extc` 用同一个形状：
4 条 TCP 连接、每连接两批数据（3 + 2 字节）、半关写端、收齐回声、任务全部收尾，退出码 50，并要求
ASan 干净与常驻有界。

**一条仍未解决的协议限制**：v1 的请求只能表达"等这个 fd **可读**"——正数表示等该 fd 可读，负数表示
立刻再推。需要"等可写"的场景（对端读得慢、发送缓冲满）目前没有表达方式，因此连接协程的写法是
**读完就地写回**（小批量下 `send` 立刻成功）。请求类型（以及 `coroutine<A,B>`）是这条的正式出路。
