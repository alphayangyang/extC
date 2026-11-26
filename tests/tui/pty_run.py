#!/usr/bin/env python3
"""tests/tui/pty.py —— 在**真 PTY** 里跑 std::tui 的用例。

为什么必须有这一层：屏幕、按键、raw 模式全都只在「stdin 是终端」时才有意义。
管道里跑能编过、能退出，但 TCGETS/FIONBIO/TIOCGWINSZ 全部失败，真正的错误一条也
抓不到。PTY 是唯一能给出「一个假终端」的东西，而且它就在 Python 标准库里（pty）。

判据（每条都是**行为**，不是代码形状）：
  - 方向键 / 可打印字节 / Ctrl 组合 / 单独的 ESC 解码成预期的 key —— 逐个喂、逐个等，
    这样「把下一个键的首字节当成 ESC 序列的一部分」这种错一定会红；
  - 备用屏、藏光标、清屏、定位、颜色、退出序列按顺序出现在输出里；
  - 程序退出后 termios 与进程序前一模一样（显式 close 与 @drop 各测一遍）；
  - stdout 不是终端时 termSize 退回 80x24。

喂字节的节奏很重要：一次写 ESC [ A 与先写 ESC 再等程序读，是**两个不同**的实验。
前者测「三个字节一起到达」，后者测「单独按 ESC」；所以 steps 是 (bytes, 期望出现的
子串) 的序列，每一步都等期望子串出现后才发下一步。
"""
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
WORK = Path(tempfile.mkdtemp(prefix="extc-tui-"))


class Fail(Exception):
    pass


def build(name):
    src = Path(__file__).resolve().parent / f"{name}.extc"
    out = WORK / name
    cp = subprocess.run([str(EXTC), "--build", "-o", str(out), str(src)],
                        capture_output=True, text=True, timeout=180)
    if cp.returncode != 0:
        raise Fail(f"{name}: 编不过\n{cp.stdout}{cp.stderr}")
    return str(out)


def decode(b):
    return b.decode("utf-8", "replace").replace("\x1b", "<ESC>")


def run_pty(argv, steps, size=(40, 12), start_wait=0.4, timeout=5.0):
    """在 PTY 里跑 argv，按 steps 喂字节，返回 (输出, 退出码, 终端是否复原)。"""
    master, slave = pty.openpty()
    cols, rows = size
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                            close_fds=True)
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
    pump(None, time.time() + 0.2)          # 启动时打印的字节先收掉
    for data, marker in steps:
        os.write(master, data)
        if not pump(marker, time.time() + timeout):
            proc.kill()
            raise Fail(f"等不到 {marker!r}\n输出：{decode(out)}")
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise Fail(f"程序不退出；输出：{decode(out)}")
    pump(None, time.time() + 0.2)          # 退出前最后一批字节
    after = termios.tcgetattr(slave)
    os.close(master)
    os.close(slave)
    return out, rc, before == after


def need(cond, msg):
    if not cond:
        raise Fail(msg)


def in_order(out, needles):
    pos = -1
    for n in needles:
        i = out.find(n, pos + 1)
        need(i >= 0, f"缺少 {n!r}（顺序 {needles!r}）\n输出：{decode(out)}")
        pos = i


def case_keys():
    out, rc, same = run_pty(build("keys"), [
        (b"\x1b[A", b"k up"),
        (b"\x1b[B", b"k down"),
        (b"\x1b[C", b"k right"),
        (b"\x1b[D", b"k left"),
        (b"a", b"k rune 97"),
        (b"\x03", b"k ctrl 3"),
        (b"\x1b", b"k esc"),
    ])
    need(rc == 0, f"keys: 退出码 {rc}")
    in_order(out, [b"k up", b"k down", b"k right", b"k left",
                   b"k rune 97", b"k ctrl 3", b"k esc"])
    need(same, "keys: 退出后 termios 没有复原")
    return "7 键解码（方向键/字符/Ctrl/单独 ESC）"


def case_frame():
    out, rc, same = run_pty(build("frame"), [])
    need(rc == 0, f"frame: 退出码 {rc}")
    need(b"size 40x12" in out, f"frame: 尺寸不对\n输出：{decode(out)}")
    in_order(out, [b"\x1b[?1049h", b"\x1b[?25l", b"\x1b[2J"])
    in_order(out, [b"\x1b[2;3H", b"AB", b"\x1b[36m", b"C", b"\x1b[0m"])
    in_order(out, [b"\x1b[?25h\x1b[?1049l"])
    need(same, "frame: @drop 没有复原 termios")
    return "备用屏/清屏/定位/颜色，@drop 复原终端"


def case_close():
    out, rc, same = run_pty(build("close"), [])
    need(rc == 0, f"close: 退出码 {rc}")
    seq = b"\x1b[?25h\x1b[?1049l"
    need(out.count(seq) == 1, f"close: 退出序列出现 {out.count(seq)} 次（应恰好 1 次）\n输出：{decode(out)}")
    need(b"after close" in out, "close: close 之后的输出丢了")
    need(same, "close: 显式 close 没有复原 termios")
    return "显式 close 与 @drop 幂等：退出序列只发一次"


def case_non_tty():
    exe = build("non_tty")
    cp = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        timeout=30)
    need(cp.returncode == 0, f"non_tty: 退出码 {cp.returncode}")
    need(cp.stdout == b"size 80x24\n",
         f"non_tty: 应回 size 80x24，实际 {cp.stdout!r}")
    return "非终端时 termSize 退回 80x24"


def main():
    if not EXTC.is_file():
        print(f"  --   跳过：{EXTC} 不存在")
        return 0
    cases = [("keys", case_keys), ("frame", case_frame),
             ("close", case_close), ("non-tty", case_non_tty)]
    bad = 0
    for name, fn in cases:
        try:
            print(f"  ok   {name:<8} {fn()}")
        except Fail as e:
            bad += 1
            print(f"  FAIL {name:<8} {e}")
    if bad:
        print(f"tui: 失败 {bad} 个")
        return 1
    print(f"tui: 通过 {len(cases)} 个")
    return 0


if __name__ == "__main__":
    sys.exit(main())
