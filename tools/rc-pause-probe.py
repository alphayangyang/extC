#!/usr/bin/env python3
"""tools/rc-pause-probe.py —— 给生成物里的回收运行期插一个**停顿探针**（只测量，不改判据）。

用法：
    python3 tools/rc-pause-probe.py <生成的 C> > <插桩后的 C>
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 <插桩后的 C> -o prog && ./prog N

插入的东西只有两处，都在**发射出来的那份运行期文本**上：

  1. `extc_rc_collect` 前后加墙钟计时（`mark` 与 `sweep` 分开记），进程退出时打印一行
     `PROBE sweeps=… max_pause_us=… max_mark_us=… max_sweep_us=… total_pause_us=…`。
     “最大单次清扫耗时”就是 `max_pause_us`：一次 `extc_rc_collect` 从进到出的墙钟。
  2. 一个 `atexit` 钩子把累计值打印出来。

判据一个字都不动：回收的触发阈值、标记规则、清扫规则全在被插桩的代码里原样保留，
所以 N=1 与 N=8 的数字是同一把尺子量的。`-D_POSIX_C_SOURCE=200809L` 是 `clock_gettime`
需要的（命令行定义先于所有头文件，生成物自己不带）。
"""

import sys

GLOBALS = r"""
/* ---- 停顿探针：由 tools/rc-pause-probe.py 插入（只测量，不改回收判据）---- */
#include <time.h>
static long long extc_probe_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static long long extc_probe_max, extc_probe_max_mark, extc_probe_max_sweep, extc_probe_sum;
static size_t extc_probe_n;
static void extc_probe_dump(void) {
    printf("PROBE sweeps=%zu max_pause_us=%.1f max_mark_us=%.1f max_sweep_us=%.1f total_pause_us=%.1f\n",
           extc_probe_n, (double)extc_probe_max / 1000.0, (double)extc_probe_max_mark / 1000.0,
           (double)extc_probe_max_sweep / 1000.0, (double)extc_probe_sum / 1000.0);
}
__attribute__((constructor)) static void extc_probe_arm(void) { atexit(extc_probe_dump); }
"""

OLD = """    size_t marked = extc_rc_mark(r, roots, nroots);
    (void)marked;
    size_t freed = extc_rc_sweep(r);
"""

NEW = """    long long extc_probe_t0 = extc_probe_ns();
    size_t marked = extc_rc_mark(r, roots, nroots);
    long long extc_probe_t1 = extc_probe_ns();
    (void)marked;
    size_t freed = extc_rc_sweep(r);
    long long extc_probe_t2 = extc_probe_ns();
    extc_probe_n++;
    extc_probe_sum += extc_probe_t2 - extc_probe_t0;
    if (extc_probe_t2 - extc_probe_t0 > extc_probe_max) extc_probe_max = extc_probe_t2 - extc_probe_t0;
    if (extc_probe_t1 - extc_probe_t0 > extc_probe_max_mark) extc_probe_max_mark = extc_probe_t1 - extc_probe_t0;
    if (extc_probe_t2 - extc_probe_t1 > extc_probe_max_sweep) extc_probe_max_sweep = extc_probe_t2 - extc_probe_t1;
"""

MARKER = "#ifndef EXTC_RC_SLAB_BITS"


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("用法：rc-pause-probe.py <生成的 C>\n")
        return 2
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        src = f.read()
    if src.count(MARKER) != 1:
        sys.stderr.write("插桩失败：找不到回收运行期（%r 出现 %d 次）\n" % (MARKER, src.count(MARKER)))
        return 1
    if src.count(OLD) != 1:
        sys.stderr.write("插桩失败：extc_rc_collect 的 mark/sweep 形态变了（匹配 %d 次）\n" % src.count(OLD))
        return 1
    src = src.replace(MARKER, GLOBALS + "\n" + MARKER, 1)
    src = src.replace(OLD, NEW, 1)
    sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
