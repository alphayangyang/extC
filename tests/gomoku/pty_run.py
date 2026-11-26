#!/usr/bin/env python3
"""tests/gomoku/pty_run.py -- 在真 PTY 里跑 gomoku 的全屏前端。"""
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
WORK = Path(tempfile.mkdtemp(prefix="extc-gomoku-tui-"))


class Fail(Exception):
    pass


def decode(b):
    return b.decode("utf-8", "replace").replace("\x1b", "<ESC>")


def build():
    out = WORK / "gomoku"
    cp = subprocess.run([str(EXTC), "--build", "-o", str(out),
                         str(ROOT / "examples" / "gomoku")],
                        capture_output=True, text=True, timeout=300)
    if cp.returncode != 0:
        raise Fail(f"编不过\n{cp.stdout}{cp.stderr}")
    return str(out)


def run_pty(argv, steps, size=(80, 24), start_wait=0.5, timeout=20.0):
    master, slave = pty.openpty()
    cols, rows = size
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    out = b""

    def pump(until, deadline):
        nonlocal out
        while True:
            if until is not None and until in out:
                return True
            left = deadline - time.time()
            if left <= 0:
                return until is None
            ready, _, _ = select.select([master], [], [], min(0.05, left))
            if not ready:
                continue
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return False
            if not chunk:
                return False
            out += chunk

    time.sleep(start_wait)
    pump(None, time.time() + 0.3)
    for data, marker in steps:
        os.write(master, data)
        if not pump(marker, time.time() + timeout):
            proc.kill()
            raise Fail(f"等不到 {marker!r}\n输出：{decode(out)[-3000:]}")
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise Fail(f"程序不退出；输出：{decode(out)[-2000:]}")
    pump(None, time.time() + 0.3)
    after = termios.tcgetattr(slave)
    os.close(master)
    os.close(slave)
    return out, rc, before == after


def need(cond, msg):
    if not cond:
        raise Fail(msg)


def case_tui():
    exe = build()
    out, rc, same = run_pty([exe, "--tui"], [
        (b"\r", b"last: white"),      # Enter 落子后 AI 应一手，状态行显示白方最后一手
        (b"u", b"last: -"),           # 悔棋退两手后回到空盘
        (b"n", b"last: -"),           # 新开一局仍是空盘
        (b"q", None),                 # q 退出
    ])
    need(rc == 0, f"退出码 {rc}")
    need(b"gomoku" in out, "标题丢了")
    need(b"\x1b[?1049h" in out, "没有进备用屏")
    need(b"\x1b[?1049l" in out, "没有退出备用屏")
    seq = b"\x1b[?25h\x1b[?1049l"
    need(out.count(seq) == 1, f"退出序列出现 {out.count(seq)} 次")
    need(same, "退出后 termios 没有复原")
    return "方向键界面：落子→AI 应手、悔棋、新局、q 退出、终端复原"


def main():
    if not EXTC.is_file():
        print(f"  --   跳过：{EXTC} 不存在")
        return 0
    try:
        print(f"  ok   tui      {case_tui()}")
    except Fail as e:
        print(f"  FAIL tui      {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
