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
- 任务跑完**不要再驱动**：句柄过期，`extc_task_alive` 会**大声 trap**（`exit(70)`）。`step` 用
  `done` 记住它。

## 两个驱动

- `step(s, i)` —— 推进一步（上面那套协议），返回它是否还活着。
- `runScripted(s, order)` —— **脚本化的就绪顺序**：按 `order` 里的下标依次推进，返回还剩几个活任务。
  判据先用它，因为它是**逐字节确定**的：不靠时间、不靠 socket、不靠内核调度顺序。
- `add(s, h)` —— 把一个句柄放进任务表（满了返回 `false`）。

真事件源（`epoll_wait`）是下一步：它挂在同一个 `step` 协议上，只是"谁就绪"改由内核告诉我们。

## 判据

`tests/coro/coro_sched.extc` —— 两个任务（step 分别 10 和 100）按交替的就绪顺序推进，断言退出码
**36**：`left * 100` 那一项就是"两个任务都跑完了"这条断言。
