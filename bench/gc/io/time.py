#!/usr/bin/env python3
"""bench/gc/io/time.py —— 量 IO 形状的**中位耗时**（不是平均，也不是单次 ✓）

用法：  python3 bench/gc/io/time.py <程序.extc> <输入文件> [次数]
        python3 bench/gc/io/time.py --bin <可执行文件> <输入文件> [次数]

为什么不用 `time`：这些负载在 **10ms 量级**，`/usr/bin/time` 只有 10ms 分辨率 ✗
（单次量出来的"提升"可能只是四舍五入的结果）。这里用 `perf_counter` 逐个量、
**报中位数**：跑 7 次，中位数对"某一次被调度打断"不敏感 ✓

⚠️ 建二进制用的是**编译器默认的旗子**（`-O2 -fwrapv`，见 src/main.c:582）——
不然量的就不是用户拿到的东西 ✗
"""
import os
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(  # bench/gc/io/time.py -> 仓库根
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
EXTC = os.path.join(ROOT, "build", "extc")


def build(src: str, out: str) -> None:
    c = out + ".c"
    subprocess.run([EXTC, src, "-o", c], check=True, cwd=ROOT)
    subprocess.run(["cc", "-O2", "-fwrapv", "-o", out, c], check=True, cwd=ROOT)


def run(binary: str, data: str, n: int):
    with open(data, "rb") as fh:
        samples = []
        out = None
        for _ in range(n):
            fh.seek(0)
            t0 = time.perf_counter()
            out = subprocess.run([binary], stdin=fh, capture_output=True)
            samples.append((time.perf_counter() - t0) * 1000.0)
    return samples, out


def main() -> int:
    argv = sys.argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[0] == "--bin":
        binary, data = argv[1], argv[2]
        reps = int(argv[3]) if len(argv) > 3 else 7
    else:
        src, data = argv[0], argv[1]
        reps = int(argv[2]) if len(argv) > 2 else 7
        binary = os.path.join(tempfile.gettempdir(), "io_bench_" + os.path.basename(src)[:-5])
        build(src, binary)
    samples, out = run(binary, data, reps)
    med = statistics.median(samples)
    print(f"{os.path.basename(binary):24} 中位 {med:7.2f} ms   最小 {min(samples):7.2f}   全部 "
          + " ".join(f"{s:.2f}" for s in samples))
    if out.returncode != 0:
        print("   ⚠️ 退出码", out.returncode, out.stderr.decode()[:200])
    else:
        print("   输出:", out.stdout.decode().strip().replace("\n", " | ")[:120])
    return 0


if __name__ == "__main__":
    sys.exit(main())
