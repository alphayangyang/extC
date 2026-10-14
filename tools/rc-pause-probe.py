#!/usr/bin/env python3
"""tools/rc-pause-probe.py —— 给生成物里的回收运行期插一个**停顿探针**（只测量，不改判据）。

用法：
    python3 tools/rc-pause-probe.py <生成的 C> > <插桩后的 C>
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 <插桩后的 C> -o prog && ./prog N

插入的东西只有两处，都在**发射出来的那份运行期文本**上：

  1. `extc_rc_collect` 前后加墙钟计时（`mark` 与 `sweep` 分开记），进程退出时打印一行
     `PROBE sweeps=… max_pause_us=… max_mark_us=… max_sweep_us=… total_pause_us=…`。
     “最大单次清扫耗时”就是 `max_pause_us`：一次 `extc_rc_collect` 从进到出的墙钟。
  2. 一个 `atexit` 钩子把累计值打印出来（既有 `PROBE` 行**原样、仍走 stdout** —— 门禁在读它 ✓）。
  3. 另注册一个 `atexit`：设 `DIGRAPH_PAUSE_DIST=1` 时把**逐次清扫时长分布**（分位 / 阈值桶 /
     排序后全部时长（n ≤ 1024）或前 50 + 后 50）打到 **stderr** 的 `PAUSE_DIST` 行 —— 不碰 stdout、不碰既有字段 ✓。

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
/* ⚠️ 既有 `PROBE` 行**原样保留**（`tests/reclaim-perf/gate-shard-cycles.sh` 读它的
 * `max_pause_us=`，且它按现状打 stdout ✗ 不许动）——新增的分布一律走**门控 stderr** 行 ✗。 */
static void extc_probe_dump(void) {
    printf("PROBE sweeps=%zu max_pause_us=%.1f max_mark_us=%.1f max_sweep_us=%.1f total_pause_us=%.1f\n",
           extc_probe_n, (double)extc_probe_max / 1000.0, (double)extc_probe_max_mark / 1000.0,
           (double)extc_probe_max_sweep / 1000.0, (double)extc_probe_sum / 1000.0);
}
/* ---- 逐次时长分布（门控：DIGRAPH_PAUSE_DIST=1，只写 stderr） ----
 * 环形数组记每一次 `extc_rc_collect` 的墙钟（进→出，ns；与 `max_pause_us` 同口径），
 * 汇总行给：样本数 / 溢出数 / p50,p90,p99,max / 阈值桶 / 排序后前 50 + 后 50。 */
#define EXTC_PROBE_RING 4096
static long long extc_probe_ring[EXTC_PROBE_RING];
static size_t extc_probe_ring_n, extc_probe_ring_over;
static void extc_probe_record(long long ns) {
    if (extc_probe_ring_n < EXTC_PROBE_RING) extc_probe_ring[extc_probe_ring_n++] = ns;
    else extc_probe_ring_over++;
}
static int extc_probe_cmp(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}
static void extc_probe_dist(void) {
    if (getenv("DIGRAPH_PAUSE_DIST") == (char *)0) return;
    size_t i;
    size_t b1 = 0, b4 = 0, b16 = 0, b64 = 0, b256 = 0, b1ms = 0, bge = 0;
    double p50 = 0, p90 = 0, p99 = 0, mx = 0;
    if (extc_probe_ring_n > 0) {
        qsort(extc_probe_ring, extc_probe_ring_n, sizeof(long long), extc_probe_cmp);
        for (i = 0; i < extc_probe_ring_n; i++) {
            long long d = extc_probe_ring[i];
            if (d < 1000LL) b1++;
            if (d < 4000LL) b4++;
            if (d < 16000LL) b16++;
            if (d < 64000LL) b64++;
            if (d < 256000LL) b256++;
            if (d < 1000000LL) b1ms++;
            else bge++;
        }
        p50 = (double)extc_probe_ring[(extc_probe_ring_n - 1) * 50 / 100] / 1000.0;
        p90 = (double)extc_probe_ring[(extc_probe_ring_n - 1) * 90 / 100] / 1000.0;
        p99 = (double)extc_probe_ring[(extc_probe_ring_n - 1) * 99 / 100] / 1000.0;
        mx = (double)extc_probe_ring[extc_probe_ring_n - 1] / 1000.0;
    }
    fprintf(stderr,
            "PAUSE_DIST n=%zu overflow=%zu p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f "
            "total_us=%.1f b_lt1us=%zu b_lt4us=%zu b_lt16us=%zu b_lt64us=%zu b_lt256us=%zu "
            "b_lt1ms=%zu b_ge1ms=%zu ",
            extc_probe_ring_n, extc_probe_ring_over, p50, p90, p99, mx,
            (double)extc_probe_sum / 1000.0, b1, b4, b16, b64, b256, b1ms, bge);
    /* n 小时给**全部**时长（便于把 REPS 轮次拼起来做精确分位）；n 大时才截断成前 50 + 后 50。 */
    if (extc_probe_ring_n <= 1024) {
        fprintf(stderr, "all=[");
        for (i = 0; i < extc_probe_ring_n; i++)
            fprintf(stderr, "%s%.1f", i ? "," : "", (double)extc_probe_ring[i] / 1000.0);
        fprintf(stderr, "]\n");
    } else {
        fprintf(stderr, "first=[");
        for (i = 0; i < 50; i++)
            fprintf(stderr, "%s%.1f", i ? "," : "", (double)extc_probe_ring[i] / 1000.0);
        fprintf(stderr, "] last=[");
        for (i = extc_probe_ring_n - 50; i < extc_probe_ring_n; i++)
            fprintf(stderr, "%s%.1f", i == extc_probe_ring_n - 50 ? "" : ",", (double)extc_probe_ring[i] / 1000.0);
        fprintf(stderr, "]\n");
    }
}
__attribute__((constructor)) static void extc_probe_arm(void) {
    atexit(extc_probe_dump);
    atexit(extc_probe_dist);
}
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
    extc_probe_record(extc_probe_t2 - extc_probe_t0);
    extc_probe_sum += extc_probe_t2 - extc_probe_t0;
    if (extc_probe_t2 - extc_probe_t0 > extc_probe_max) extc_probe_max = extc_probe_t2 - extc_probe_t0;
    if (extc_probe_t1 - extc_probe_t0 > extc_probe_max_mark) extc_probe_max_mark = extc_probe_t1 - extc_probe_t0;
    if (extc_probe_t2 - extc_probe_t1 > extc_probe_max_sweep) extc_probe_max_sweep = extc_probe_t2 - extc_probe_t1;
"""

OLD2 = '    size_t marked = extc_rc_mark(r, roots, nroots);\n    (void)marked;\n    r->visitedBytes = r->allocBytes;   /* 这次**标记遍历**吃掉的信用 */\n    extc_rc_sweepBegin(r);\n    extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);   /* 只做一步；余下在分配路径上续扫 */\n    return r->swFreed;\n'
NEW2 = '    long long extc_probe_t0 = extc_probe_ns();\n    size_t marked = extc_rc_mark(r, roots, nroots);\n    long long extc_probe_t1 = extc_probe_ns();\n    (void)marked;\n    r->visitedBytes = r->allocBytes;\n    extc_rc_sweepBegin(r);\n    extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET);\n    long long extc_probe_t2 = extc_probe_ns();\n    extc_probe_record(extc_probe_t2 - extc_probe_t0);\n    extc_probe_sum += extc_probe_t2 - extc_probe_t0;\n    if (extc_probe_t2 - extc_probe_t0 > extc_probe_max) extc_probe_max = extc_probe_t2 - extc_probe_t0;\n    if (extc_probe_t1 - extc_probe_t0 > extc_probe_max_mark) extc_probe_max_mark = extc_probe_t1 - extc_probe_t0;\n    if (extc_probe_t2 - extc_probe_t1 > extc_probe_max_sweep) extc_probe_max_sweep = extc_probe_t2 - extc_probe_t1;\n    return r->swFreed;\n'

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
    if src.count(OLD) == 1:
        src = src.replace(OLD, NEW, 1)
    elif src.count(OLD2) == 1:
        src = src.replace(OLD2, NEW2, 1)
    else:
        sys.stderr.write("插桩失败：collect 形态不认识（整段 %d / 分步 %d）\n" % (src.count(OLD), src.count(OLD2)))
        return 1
    # `sweeps` 口径保持\"扫完的轮数\"：分步清扫在 completion 处计数（不是 collect 入口）
    src = src.replace("    r->sweeps++;", "    extc_probe_n++;\n    r->sweeps++;", 1)
    src = src.replace(MARKER, GLOBALS + "\n" + MARKER, 1)
    src = src.replace(OLD, NEW, 1)
    sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
