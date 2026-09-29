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
