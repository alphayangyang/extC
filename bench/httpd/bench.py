#!/usr/bin/env python3
"""把几个轴分开量（上一个数把"每请求背着 256KB"混进去了）：
   tiny  小对象顺序（11 B）   -> 请求能力
   big   大对象顺序（256 KB） -> 单连接字节率
   pipe  大对象**流水线**      -> 传输能力（不等响应就发下一个）
   multi 8 条连接同时打小对象  -> 单线程调度器能榨出多少
每个档都报服务器的 CPU 时间（utime+stime）—— 忙等会在这里现原形。
"""
import socket
import sys
import threading
import time

PORT = int(sys.argv[1])
PID = int(sys.argv[2])


def cpu_ms():
    if not PID:
        return 0.0
    with open(f"/proc/{PID}/stat") as f:
        parts = f.read().split()
    return (int(parts[13]) + int(parts[14])) * 10.0  # 单位 10ms


def conn():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=20)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def read_n(s, buf, n):
    while len(buf) < n:
        chunk = s.recv(1 << 20)
        if not chunk:
            raise RuntimeError("closed")
        buf += chunk
    return buf[:n], buf[n:]


def read_resp(s, buf):
    while b"\r\n\r\n" not in buf:
        buf += s.recv(1 << 20)
    head, rest = buf.split(b"\r\n\r\n", 1)
    cl = 0
    for ln in head.split(b"\r\n"):
        if ln.lower().startswith(b"content-length:"):
            cl = int(ln.split(b":")[1])
    body, rest = read_n(s, rest, cl)
    return rest


def report(name, reqs, dt, cpu0, extra=""):
    c = cpu_ms() - cpu0
    print(f"  {name:8} {reqs:7d} 请求  {dt:6.3f}s  {reqs / dt:9.0f} req/s  "
          f"服务器CPU {c:7.1f}ms  {c * 1000 / reqs:6.1f}µs/req  {extra}")


def main():
    REQ_SMALL = b"GET /small.txt HTTP/1.1\r\nHost: x\r\n\r\n"
    REQ_BIG = b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n"

    # tiny
    s = conn(); buf = b""
    n = 5000
    s.sendall(REQ_SMALL); buf = read_resp(s, buf)          # 预热
    c0 = cpu_ms(); t0 = time.perf_counter()
    for _ in range(n):
        s.sendall(REQ_SMALL)
        buf = read_resp(s, buf)
    report("tiny", n, time.perf_counter() - t0, c0)
    s.close()

    # big（顺序：一条响应读完再发下一个）
    s = conn(); buf = b""
    n = 100
    s.sendall(REQ_BIG); buf = read_resp(s, buf)
    c0 = cpu_ms(); t0 = time.perf_counter()
    for _ in range(n):
        s.sendall(REQ_BIG)
        buf = read_resp(s, buf)
    dt = time.perf_counter() - t0
    report("big", n, dt, c0, f"{n * 262144 / dt / 1e6:.0f} MB/s")
    s.close()

    # pipe（一口气发 D 个，再收 D 个）
    D = 32
    ROUNDS = 10
    s = conn(); buf = b""
    c0 = cpu_ms(); t0 = time.perf_counter()
    for _ in range(ROUNDS):
        s.sendall(REQ_BIG * D)
        for _ in range(D):
            buf = read_resp(s, buf)
    dt = time.perf_counter() - t0
    reqs = D * ROUNDS
    report("pipe", reqs, dt, c0, f"{reqs * 262144 / dt / 1e6:.0f} MB/s")
    s.close()

    # multi（8 条连接各打小对象，服务器单线程）
    per = 800
    def worker():
        s = conn(); buf = b""
        for _ in range(per):
            s.sendall(REQ_SMALL)
            buf = read_resp(s, buf)
        s.close()
    c0 = cpu_ms(); t0 = time.perf_counter()
    ts = [threading.Thread(target=worker) for _ in range(8)]
    [t.start() for t in ts]; [t.join() for t in ts]
    report("multi", per * 8, time.perf_counter() - t0, c0, "(8 连接)")
    return 0


sys.exit(main())
