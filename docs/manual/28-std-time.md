# 28. 时间（`std::time`）

**两个钟，问的是两个不同的问题，别混**：

| | 是什么 | 拿它干什么 | 会跳吗 |
|---|---|---|---|
| `time::now()` | 单调钟（`CLOCK_MONOTONIC`） | **测"这段代码花了多久"** | **不会**（这正是它存在的理由） |
| `time::real()` | 墙钟（`CLOCK_REALTIME`） | 问"现在几点" | 会（NTP、时区、管理员改表） |

两个都返回 **i64 纳秒**。为什么是整数纳秒：跨边界只走标量（见下），而整数纳秒既没有精度损失，
也不会把"精度是多少"藏进浮点里。

**计时用 `time::start()`**（闭包还不能当参数，所以没有 `time::perf(fn)` 那种东西 —— 计时是显式的）：

```extc
use std::time
use std::io

fn main() -> i32 {
  var n: i64 = time::now() % i64(200000) + i64(50000)
  var t = time::start()
  var s: i64 = 0
  var i: i64 = 0
  while i < n { s = s + i * i  i = i + i64(1) }
  io::cout << "took " << t.elapsedUs() << " us (sum=" << (s % i64(1000)) << ")\n"
  return 0 }
```

`timer` 上的五个问题：`elapsedNs()` / `elapsedUs()` / `elapsedMs()` / `elapsedSecs()`（f64 秒）/
以及 `time::now()` 本身。

**睡眠**：`time::sleepNs(ns)` / `sleepUs(us)` / `sleepMs(ms)` —— 至少睡够这么久（被信号打断会
自动接着睡剩下的）。

## 边界与已知取舍

- **分辨率 ≠ 精度**。在这台机器上，连续两次 `time::now()` 的最小正差值是 **11 ns**（走 vDSO 的
  `CLOCK_MONOTONIC`）。这是"读数能细到多少"，不是"计时准到多少" —— 后面那个取决于操作系统与硬件。
- 时钟在**运行期**（`src/base/extctime.c`，按需发射）：`clock_gettime` 要一个 `struct timespec`，而本项目的
  边界规矩是**平台结构体的布局不让 extC 知道**（`std::sys::net` 对 `epoll_event` 也是这么办的）。
  所以布局留在 C 里，extC 这边只看见一个 `i64`。不问时间的程序，产物里一行都不带。
- 产物用 `-std=c11`（严格 ISO）编译，而 `clock_gettime` / `nanosleep` 是 POSIX ⇒ 用到时间时，
  生成物开头会多一行 `#define _POSIX_C_SOURCE 200809L`（特性宏**必须在第一个 include 之前**，
  所以它是按需发在前言里的）。
- 计时循环要**折不掉**：`while i < 3000000 { s = s + i }` 会被 GCC 闭式求值（实测"耗时" 21 ns ✗）。
  让上界来自时钟（像上面那样）或者来自输入，量到的才是真的。
