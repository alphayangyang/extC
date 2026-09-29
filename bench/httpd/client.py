#!/usr/bin/env python3
"""纯净 socket 的 HTTP/1.1 判据客户端：不给外部工具（curl/ab）留依赖，也就能钉住每一条断言。"""
import os
import socket
import sys
import time

PORT = int(sys.argv[1])
STRICT = "--strict" in sys.argv
PID = int(sys.argv[2]) if len(sys.argv) > 2 else 0
HOST = "127.0.0.1"
fails = []


def recv_until(sock, buf, marker):
    while marker not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("连接提前关闭（等 %r）" % marker)
        buf += chunk
    head, rest = buf.split(marker, 1)
    return head, rest


def read_response(sock, buf, no_body=False):
    head, rest = recv_until(sock, buf, b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for ln in lines[1:]:
        if b":" in ln:
            k, v = ln.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    n = 0 if no_body else int(headers.get(b"content-length", b"0"))   # HEAD 的响应不带 body ✓
    while len(rest) < n:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("body 没读完")
        rest += chunk
    return status, headers, rest[:n], rest[n:]


class Conn:
    """一条 keep-alive 连接；对端关了就地重连（真实客户端的行为，也让 py-httpd 可比）。"""

    def __init__(self):
        self.s = None
        self.buf = b""

    def send(self, raw, no_body=False):
        for attempt in (0, 1):
            try:
                if self.s is None:
                    self.s = socket.create_connection((HOST, PORT), timeout=10)
                    self.buf = b""
                self.s.sendall(raw)
                return read_response(self.s, self.buf, no_body)
            except (RuntimeError, OSError):
                if self.s:
                    self.s.close()
                self.s = None
                self.buf = b""
                if attempt:
                    raise
        raise RuntimeError("重连后仍失败")


def check(name, cond, detail=""):
    if cond:
        print(f"  ok   {name}")
    else:
        fails.append(name)
        print(f"  FAIL {name}  {detail}")


def main():
    c = Conn()
    # ① 目录索引 + keep-alive（这条之后连接必须还活着，后面的请求共用它）
    st, hd, body, buf = c.send(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("GET / -> 200 + index.html", st == 200 and b"it works" in body, f"st={st}")
    lm = hd.get(b"last-modified")
    check("Last-Modified 存在", bool(lm), f"hd={hd}")
    # ② If-Modified-Since -> 304
    st, _, body, buf = c.send(b"GET /index.html HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: " + lm + b"\r\n\r\n")
    check("If-Modified-Since -> 304", st == 304 and body == b"", f"st={st}")
    # ③ HEAD：有 Content-Length、没 body
    st, hd, body, buf = c.send(b"HEAD /big.bin HTTP/1.1\r\nHost: x\r\n\r\n", no_body=True)
    check("HEAD -> 200 + 长度对 + 无 body",
          st == 200 and int(hd[b"content-length"]) == 262144 and body == b"", f"st={st}")
    # ④ 404
    st, _, _, buf = c.send(b"GET /nope HTTP/1.1\r\nHost: x\r\n\r\n")
    check("GET /nope -> 404", st == 404, f"st={st}")
    # ⑤ 路径穿越必须被拒（这条是安全判据，不是装饰）
    st, _, body, _ = c.send(b"GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
    check("GET /../etc/passwd 被拒（400/404，且没有内容）",
          st in (400, 404) and b"root:" not in body, f"st={st}")
    st, _, body, _ = c.send(b"GET /%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
    check("GET /%2e%2e/... 被拒（400/404）", st in (400, 404) and b"root:" not in body, f"st={st}")
    # ⑥ 子目录索引
    st, _, body, buf = c.send(b"GET /sub/ HTTP/1.1\r\nHost: x\r\n\r\n")
    check("GET /sub/ -> 200 + sub index", st == 200 and b"sub index" in body, f"st={st}")
    # ⑦ 方法：POST 必须被拒（extC 给 405；python -m http.server 给 501 —— 都是"不支持"）
    st, _, _, _ = c.send(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    check("POST 被拒（405/501）", st in (405, 501), f"st={st}")
    # ⑦b 请求头超过 1 KB ⇒ 431（策略：`MAXREQ` 就是 nginx `client_header_buffer_size` 的默认 1 KB）
    # 用**独立连接**判这条（1 KB 头上限是**本服务器**的策略 ⇒ 只在 --strict 下判）：431 之后服务器会关连接（还可能带 RST），共用连接会被重连路径搅进来
    if STRICT:
        big = socket.create_connection((HOST, PORT), timeout=5)
        big.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 2000 + b"\r\n\r\n")
        try:
            line = big.recv(256).split(b"\r\n")[0]
        except OSError:
            line = b"(reset)"
        big.close()
        check("超大请求头 -> 431", line.startswith(b"HTTP/1.1 431"), f"line={line!r}")
    # ⑧ 流水线：一口气两个请求，两条响应都要回来（`--strict`；python -m http.server 不支持）
    if STRICT:
        c.s.sendall(b"GET /small.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /small.txt HTTP/1.1\r\nHost: x\r\n\r\n")
        st1, _, b1, c.buf = read_response(c.s, c.buf)
        st2, _, b2, c.buf = read_response(c.s, c.buf)
        check("流水线两条响应", st1 == 200 and st2 == 200 and b1 == b"plain text\n", f"{st1}/{st2}")
    # ⑨ Connection: close 之后连接要关（`--strict` 才判：基线不保证）
    if STRICT:
        st, _, _, _ = c.send(b"GET /small.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        closed = c.s.recv(1) == b""
        check("Connection: close 后关闭", st == 200 and closed, f"st={st} closed={closed}")
    c.s.close()

    # ⑩ 吞吐：一条 keep-alive 连接上连打 N 个 256KB
    n = 200
    s = socket.create_connection((HOST, PORT), timeout=10)
    buf = b""
    st, _, body, buf = c.send(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
    if st != 200:
        fails.append("吞吐前预热失败")
        print("  FAIL 吞吐前预热失败")
    else:
        t0 = time.perf_counter()
        for _ in range(n):
            s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            st, _, body, buf = read_response(s, buf)
        dt = time.perf_counter() - t0
        rps = n / dt
        mbs = n * 262144 / dt / 1024 / 1024
        print(f"  ok   吞吐  ->  {rps:.0f} req/s · {mbs:.0f} MB/s（{n} × 256KB，一条连接）")
    c.s.close()

    # ⑪ 服务器峰值 RSS（直接问内核）
    if PID:
        try:
            for ln in open(f"/proc/{PID}/status"):
                if ln.startswith("VmHWM:"):
                    print(f"  ok   峰值 RSS  ->  {int(ln.split()[1]) / 1024:.1f} MB")
        except FileNotFoundError:
            pass

    print(f"失败 {len(fails)} 个（0 = 全过）")
    return 1 if fails else 0


sys.exit(main())
