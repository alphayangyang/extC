#!/usr/bin/env python3
"""能挂住多少条空闲 keep-alive 连接（以及每连接花服务器多少内存）。
  用法：python3 conns.py <port> <server_pid> <N>
"""
import os
import socket
import sys
import time

PORT = int(sys.argv[1]); PID = int(sys.argv[2]); N = int(sys.argv[3])
HOST = "127.0.0.1"


def rss_kb(pid):
    for ln in open(f"/proc/{pid}/status"):
        if ln.startswith("VmRSS:"):
            return int(ln.split()[1])
    return 0


socks = []
t0 = time.perf_counter()
fail = 0
for i in range(N):
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
        s.sendall(b"GET /small.txt HTTP/1.1\r\nHost: x\r\n\r\n")
        s.setblocking(False)
        socks.append(s)
    except OSError:
        fail += 1
        if fail > 50:
            break
    if i % 2000 == 0 and i:
        # 把这些连接的响应读掉（不然服务器的写缓冲会堆着）
        for s in socks:
            try:
                s.recv(65536)
            except BlockingIOError:
                pass
print(f"  建连 {len(socks)}/{N}（失败 {fail}），耗时 {time.perf_counter() - t0:.1f}s")
print(f"  服务器 RSS = {rss_kb(PID) / 1024:.1f} MB")
import os
hold = float(os.environ.get("HOLD", "1"))
time.sleep(max(hold, 0.2))
print(f"  挂住 {hold:.0f}s 后：RSS = {rss_kb(PID) / 1024:.1f} MB ⇒ 每连接约 {(rss_kb(PID) * 1024) / max(len(socks), 1):.0f} 字节")
for s in socks:
    s.close()
