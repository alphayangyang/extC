# bench/httpd —— 最小 HTTP/1.1 静态服务器（extC：epoll + 协程 + sendfile）

"URL 解释器"那一线的第一格：**能用的静态服务器**，和 Python 的 `http.server` 同判据对照。

```sh
bash bench/httpd/run.sh
```

## 分工（这就是那层的形状）

| 哪一层 | 写在哪 | 为什么 |
|---|---|---|
| epoll / TCP / accept / nonblock / nodelay | `std::sys::net`（C 运行时 shim） | `epoll_event`、`sockaddr_in` **留在 C 侧**，extC 只拿 fd 与标量 |
| `open`/`fstat`/`sendfile`/HTTP 日期 | `std::sys::file`（新加的 shim，5 个函数） | `struct stat`／`off_t`／路径的 NUL 终止都在 C 侧；镜像 `struct stat` 是踩栈那一类风险 |
| 报文解析 / 路由 / 状态码 / keep-alive / 路径穿越防护 | `bench/httpd/httpd.extc`（约 300 行 extC） | 解析和状态机正是 extC 的边界检查与 trap 最值钱的地方 |

## 实测（2026-09-29，本机 loopback，单进程单线程）

| | extc-httpd | py-httpd（`python3 -m http.server`） |
|---|---|---|
| 功能判据 | **13/13** | 11/11（流水线与 `Connection: close` 只在 `--strict` 下判，基线不保证） |
| 吞吐（200 × 256KB，一条 keep-alive 连接） | **5376 req/s · 1344 MB/s** | 4368 req/s · 1092 MB/s |
| 峰值 RSS（`VmHWM`） | **1.9 MB** | 22.7 MB |

判据覆盖：目录索引 · `Last-Modified` · `If-Modified-Since` → **304** · `HEAD`（有长度无 body）·
`404` · **路径穿越 `..` 与 `%2e%2e` 都拒** · 子目录索引 · `POST` → 405 · **流水线**（两个请求一口气）·
`Connection: close` 后关闭。客户端是纯 socket 的 `client.py` —— 不依赖 curl/ab，每条断言都能钉住。

## 写这一格时踩到并修掉的五个 bug（都留在代码注释里）

1. **`hdrEnd` 指向最后一个头行的 CRLF 之前** ⇒ 循环在最后一行（`If-Modified-Since`）之前就停了，
   304 永远不生效 ✗。
2. **`mut ref` 出参不能 `*p = …`**（extC 只有字段/下标能透过引用写）⇒ 助手改成"返回新位置"。
3. **协程返回时没人关 fd**（仓库惯例是手动关）⇒ 4xx 之后客户端还在等一个永远不来的响应 ✗。
4. **缓冲区里已有完整请求时还先 `yield`** ⇒ 流水线的第二条永远等不到 ✗。
5. **早期 `continue` 没把请求从缓冲吃掉** ⇒ POST 之后所有请求都回 405 ✗（先拷头、再吃掉、然后解析）。

第 2 条是语言规则（照它写就对；`trap: slice 21..-1 is out of range` 那次是它顺手救的 ✓），
其余四条是服务器自己的状态机错误 —— 而它们**都是带位置的 trap 或明确的错误响应**暴露出来的，
没有一条变成静默错 ✓。

## 量测：把轴分开（`bench.py`，2026-09-29）

`client.py` 是**判据**，`bench.py` 是**量具**：小对象顺序 / 大对象顺序 / 大对象**流水线** /
8 连接并发，并读服务器自己的 CPU 时间（`/proc/<pid>/stat` ✓）。

| 档 | 结果 | 读法 |
|---|---|---|
| tiny（11 B，8 连接） | ~39–41k req/s | **客户端是 Python** ⇒ 这个数被客户端压住了 ✗（每请求的 Python 开销和服务器同量级） |
| big（256 KB，单连接顺序） | **1393 MB/s** | 与仓库自己的 echo server 单流 1,125 MB/s 同档 ✓ ⇒ 传输没问题 |
| big（256 KB，**8 连接**） | **830 MB/s** ✗ | **并发反而更慢** —— 发送端写不动时是**自旋**（仓库已知的 v1 缺口：yield 协议只能等可读） |
| 写死响应（不走文件） | 比走文件快 15% | ⇒ 文件那几个系统调用**不是**瓶颈 ✓ |

**结论**：我先前报的 "5376 req/s" 是个**混了轴又被客户端压住**的数 ✗，不是服务器的能力上限。
真要拿能力上限，得用非 Python 的压测器（`ab`/`wrk` 本机没有 ✗）。

### 试过的修法与结论（诚实记下）

给调度器加"等可写"（`yield waitWritable(fd)`，高位标记 ✓）并在 `EAGAIN` 时让出 —— **实测更慢**：
8 连接 256 KB 从 830 掉到 **337 MB/s** ✗。原因是每次**换方向**都 `epoll_ctl(DEL)+ADD`（`ADD` 在已
注册的 fd 上是 `EEXIST` ⇒ 只能先摘）⇒ 每请求两次抖动。**已整体撤回**（stdlib 改动会churn golden ✓）。
若再做，要一次只动一个变量：① 只把"解析挪出协程"（`planRequest`，见下）单独量；② "等可写"改用
`EPOLL_CTL_MOD` 单独量。

### 协程那条规则的原文（`docs/topics/CONCURRENCY.md` §4.4）

> | 被取**视图**、且视图跨挂起点的局部 | **不自动搬**（用户没写 `new` 就不进 arena，无隐式分配）⇒
> **编译期拒绝**，消息给两条改法：自己 `new` 到任务层 / 存偏移、恢复后重取 |
> | 帧里的局部再被取视图 | 同样（帧是个**可复制的值**）|

所以本文件最初那版 `var head: slice<u8> = hdr[..hdrEnd]` 被拒**是对的** —— `hdr` 虽然是
`new` 出来的，但**"对局部再取视图"**这一格按设计就拒 ✗。两条正解：把视图留在**不挂起的函数**里
（现在的 `planRequest(head: slice<u8>, …)` 就是这条 ✓），或者存偏移、恢复后重取 ✓。

## 对 nginx（`vs-nginx.sh`，wrk 多线程 C 客户端 + 24 核，2026-09-29）

`client.py`/`bench.py` 是**判据**；量具用 **wrk**（Python 客户端自己就是瓶颈：同一个服务器
Python 量到 39k req/s，wrk 量到 **183k** ✗⇒✓）。服务器是**多进程**（`extc_tcp_listen_shared`
就是 `SO_REUSEPORT` ⇒ 起 N 份共用一个端口，零代码改动 ✓）。

| | extC 1 进程 | **extC 8 进程** | nginx 8 worker |
|---|---|---|---|
| tiny（11 B，256 连接） | 183,383 req/s | **1,319,815 req/s** | 1,222,896 req/s |
| big（256 KB，128 连接） | 18.3 GB/s | **61.6 GB/s** | 62.1 GB/s |
| p50 / p99（tiny） | 1.31 ms / 28 ms | 155 µs / 24.7 ms | 174 µs / 14.4 ms |
| CPU（big，8 份合计） | — | 714% 单核 | 703% |
| 峰值 RSS | 1.9 MB | **24 MB** | ~44 MB |
| **单核**（big） | 18.3 GB/s | 7.7 GB/s/核 | 7.8 GB/s/核 |
| socket errors | 0 | **0** | 0 |

读法：单核比 nginx 快（tiny +20%、big **2.4×** —— `sendfile` + 更瘦的路径 ✓），8 进程打平
（tiny +8%、big −0.8%），CPU 效率打平，RSS 约一半。

**一个真协议 bug 是这轮抓到的**：撞上 keep-alive 上限（`MAXKEEP`）时我直接关 fd，没在那条响应里
发 `Connection: close` ⇒ wrk 每 1.27M 请求报 **53 次 read error** ✗（nginx 是体面关闭 ✓）。修完
errors 归零 ✓。
