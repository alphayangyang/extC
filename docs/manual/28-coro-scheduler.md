# 协程调度器（`std::coro::scheduler`）

**一句话**：一个 `while` 循环 + 一个事件源，握住一组 `coroutine<i64>` 句柄轮流推进。

## 形状

```extc
use std::coro::scheduler as sched

var s: sched::loop                          /* 零初始化：任务数 0 */
sched::add(ref s, worker(i64(10)))
sched::add(ref s, worker(i64(100)))

var order: mut slice<i64> = new i64[12]
var k: i64 = 0
while k < i64(12) { order[k] = k % i64(2)  k = k + 1 }   /* 交替就绪 */

var left: i64 = sched::runScripted(ref s, order)
```

类型 `loop` 里是一张**固定容量**的任务表 `tasks`（`[8]coroutine<i64>`）：句柄是 24 字节的普通值，
数组元素就是**可变位置**，所以可以直接 `s.tasks[i].next()` / `.send()`。另有 `done`、`started`
两个标记数组、已放入的任务数 `n` 和还活着的任务数 `live`。

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

- `step(s, i)` —— 推进一步（上面那套协议），返回它是否还活着。
- `runScripted(s, order)` —— **脚本化的就绪顺序**：按 `order` 里的下标依次推进，返回还剩几个活任务。
  判据先用它，因为它是**逐字节确定**的：不靠时间、不靠 socket、不靠内核调度顺序。
- `add(s, h)` —— 把一个句柄放进任务表（满了返回 `false`）。

- `runEpoll(s, timeout_ms)` —— **真事件源**：`epoll_wait`（走 `std::sys::net` 的
  `extern!("extc-runtime")`）。协议与 `runScripted` 同一条，只是"谁就绪"改由内核告诉我们：

  1. 每个任务 push 到"要么在等真实事件、要么结束"为止；
  2. 它 `yield` 的请求就是它想等的 fd，**在产生的同一轮**就注册进 epoll（晚一轮会空等超时）；
  3. `epoll_wait` 报了就绪 fd 之后，用 `send(fd)` 恢复在等它的那个任务；
  4. **请求为负 = "别等，立刻再推我"** —— 协程收尾时用它交回最后一个值，否则它会去等一个已经被
     自己读空的 fd，永远不醒（这是实测踩出来的）。

  读写用运行期的 `recv`/`send`，**不是** `read`/`write`：后两个名字已经被 `std::sys::io` 用另一套
  签名占了，而两者会落在同一个编译单元里（`<unistd.h>` 一 include 就冲突）。

## 判据

`tests/coro/coro_sched.extc` —— 两个任务（step 分别 10 和 100）按交替的就绪顺序推进，断言退出码
**36**：`left * 100` 那一项就是"两个任务都跑完了"这条断言。

`tests/coro/coro_epoll.extc` —— **8 条 AF_UNIX 连接、单线程、一个 epoll 循环**：每条连接一个协程，
先写请求再跑循环，最后收回声累加，退出码 **36**（= 1+2+…+8），并要求 **ASan 干净**。数据在
socketpair 的内核缓冲里 ⇒ 立刻可读 ⇒ 判据不靠 sleep、不靠时间。
