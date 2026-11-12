#!/usr/bin/env python3
"""tools/rc-pause-probe.py —— 给生成物里的回收运行期插一个**停顿探针**（只测量，不改判据）。

用法：
    python3 tools/rc-pause-probe.py <生成的 C> > <插桩后的 C>
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 <插桩后的 C> -o prog && ./prog N

插入的东西只有两处，都在**发射出来的那份运行期文本**上：

  1. `extc_rc_collect`（契约入口）与 `extc_rc_step`（**热路径入口**，merge 后分配点/销账点走这条）
     前后各加墙钟计时，进程退出时打印一行
     `PROBE sweeps=… max_pause_us=… … total_pause_us=… step_n=… step_max_us=… step_over64=…`。
     “最大单次清扫耗时”是 `max_pause_us`（一次 `extc_rc_collect` 从进到出）；热路径的对应量是
     `step_max_us` / `step_over*`（阈值桶：超 16/64/256 µs/1 ms 的步数）。**两个都要看**：
     只报 `max_pause_us` 会在"热路径改走 `step`"后恒为 0（`gate-shard-cycles` 因此假绿过一轮）。
  2. 一个 `atexit` 钩子把累计值打印出来（既有 `PROBE` 行**原样、仍走 stdout** —— 门禁在读它 ✓）。
  3. 另注册一个 `atexit`：设 `DIGRAPH_PAUSE_DIST=1` 时把**逐次清扫时长分布**（分位 / 阈值桶 /
     排序后全部时长（n ≤ 1024）或前 50 + 后 50）打到 **stderr** 的 `PAUSE_DIST` 行 —— 不碰 stdout、不碰既有字段 ✓。
     同一开关下**另起一行** `PAUSE_DIST_STEP`：热路径 `extc_rc_step` 的逐次时长，同样格式、同一套桶。
     两行必须分开看：今天的热路径走 `extc_rc_step`（`extc_rc_collect` 可以一次都不被调用），
     所以 `PAUSE_DIST n=0` 是**结构性 0**（冷入口没人调），热路径的分布看 `PAUSE_DIST_STEP`。

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
static long long extc_probe_stepN, extc_probe_stepMax, extc_probe_stepSum;
static long long extc_probe_stepOver16, extc_probe_stepOver64, extc_probe_stepOver256, extc_probe_stepOver1ms;
/* 热路径分布：`PAUSE_DIST` 只记冷入口 `extc_rc_collect`，而今天热路径走 `extc_rc_step`
 * ⇒ 同一份生成物上常常 `PAUSE_DIST n=0` 而 `step_n` 不为 0。这里把每一步也记进自己的环，
 * 单独打成 `PAUSE_DIST_STEP`（格式与 `PAUSE_DIST` 相同），不改变前者的口径。 */
#define EXTC_PROBE_SRING 65536
static long long extc_probe_sring[EXTC_PROBE_SRING];
static size_t extc_probe_sring_n, extc_probe_sring_over;
static void extc_probe_srecord(long long ns) {
    if (extc_probe_sring_n < EXTC_PROBE_SRING) extc_probe_sring[extc_probe_sring_n++] = ns;
    else extc_probe_sring_over++;
}
/* 热路径入口（`extc_rc_step`）的进→出墙钟：契约入口的 `max_pause_us` 量不到它。 */
static void extc_probe_steprec(long long ns) {
    extc_probe_srecord(ns);
    extc_probe_stepN++; extc_probe_stepSum += ns;
    if (ns > extc_probe_stepMax) extc_probe_stepMax = ns;
    if (ns >= 16000) extc_probe_stepOver16++;
    if (ns >= 64000) extc_probe_stepOver64++;
    if (ns >= 256000) extc_probe_stepOver256++;
    if (ns >= 1000000) extc_probe_stepOver1ms++;
}
static void extc_probe_dump(void) {
    printf("PROBE sweeps=%zu max_pause_us=%.1f max_mark_us=%.1f max_sweep_us=%.1f total_pause_us=%.1f"
           " step_n=%lld step_max_us=%.1f step_total_us=%.1f step_over16=%lld step_over64=%lld step_over256=%lld step_over1ms=%lld\n",
           extc_probe_n, (double)extc_probe_max / 1000.0, (double)extc_probe_max_mark / 1000.0,
           (double)extc_probe_max_sweep / 1000.0, (double)extc_probe_sum / 1000.0,
           extc_probe_stepN, (double)extc_probe_stepMax / 1000.0, (double)extc_probe_stepSum / 1000.0,
           extc_probe_stepOver16, extc_probe_stepOver64, extc_probe_stepOver256, extc_probe_stepOver1ms);
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
static void extc_probe_sdist(void) {
    if (getenv("DIGRAPH_PAUSE_DIST") == (char *)0) return;
    size_t i;
    size_t b1 = 0, b4 = 0, b16 = 0, b64 = 0, b256 = 0, b1ms = 0, bge = 0;
    double p50 = 0, p90 = 0, p99 = 0, mx = 0;
    if (extc_probe_sring_n > 0) {
        qsort(extc_probe_sring, extc_probe_sring_n, sizeof(long long), extc_probe_cmp);
        for (i = 0; i < extc_probe_sring_n; i++) {
            long long d = extc_probe_sring[i];
            if (d < 1000LL) b1++;
            if (d < 4000LL) b4++;
            if (d < 16000LL) b16++;
            if (d < 64000LL) b64++;
            if (d < 256000LL) b256++;
            if (d < 1000000LL) b1ms++;
            else bge++;
        }
        p50 = (double)extc_probe_sring[(extc_probe_sring_n - 1) * 50 / 100] / 1000.0;
        p90 = (double)extc_probe_sring[(extc_probe_sring_n - 1) * 90 / 100] / 1000.0;
        p99 = (double)extc_probe_sring[(extc_probe_sring_n - 1) * 99 / 100] / 1000.0;
        mx = (double)extc_probe_sring[extc_probe_sring_n - 1] / 1000.0;
    }
    fprintf(stderr,
            "PAUSE_DIST_STEP n=%zu overflow=%zu p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f "
            "total_us=%.1f b_lt1us=%zu b_lt4us=%zu b_lt16us=%zu b_lt64us=%zu b_lt256us=%zu "
            "b_lt1ms=%zu b_ge1ms=%zu ",
            extc_probe_sring_n, extc_probe_sring_over, p50, p90, p99, mx,
            (double)extc_probe_stepSum / 1000.0, b1, b4, b16, b64, b256, b1ms, bge);
    if (extc_probe_sring_n <= 1024) {
        fprintf(stderr, "all=[");
        for (i = 0; i < extc_probe_sring_n; i++)
            fprintf(stderr, "%s%.1f", i ? "," : "", (double)extc_probe_sring[i] / 1000.0);
        fprintf(stderr, "]\n");
    } else {
        fprintf(stderr, "first=[");
        for (i = 0; i < 50; i++)
            fprintf(stderr, "%s%.1f", i ? "," : "", (double)extc_probe_sring[i] / 1000.0);
        fprintf(stderr, "] last=[");
        for (i = extc_probe_sring_n - 50; i < extc_probe_sring_n; i++)
            fprintf(stderr, "%s%.1f", i == extc_probe_sring_n - 50 ? "" : ",", (double)extc_probe_sring[i] / 1000.0);
        fprintf(stderr, "]\n");
    }
}
__attribute__((constructor)) static void extc_probe_arm(void) {
    atexit(extc_probe_dump);
    atexit(extc_probe_dist);
    atexit(extc_probe_sdist);
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


def wrap_timed(src, sig, newname, ret, args, callargs, only_nonzero):
    """把 sig 对应的函数体重命名成 newname，并补一个同名计时包装。

    包装覆盖**所有出口**（含早退）。旧写法只把计时插在尾部 `return ...;` 前，
    早退路径全漏记（digraph 上曾把 4.7 ms 的停顿总量读成 28 us）。
    only_nonzero == "n" 时只在返回值非 0 时记录（markAdvance 的 tick 未到就是 no-op，
    每分配一次都走，记进来会淹没分布）。
    """
    if src.count(sig) != 1:
        return src, False
    start = src.index(sig)
    name = sig.split("(")[0].split()[-1]
    ob = start + sig.index("{")
    depth = 0
    k = ob
    while k < len(src):
        c = src[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    end = k + 1
    body = src[start + len(sig):end]
    renamed = sig.replace(name + "(", newname + "(", 1)
    if only_nonzero == "n":
        rec = "    if (_n != 0) extc_probe_steprec(extc_probe_ns() - _t0);\n"
    else:
        rec = "    extc_probe_steprec(extc_probe_ns() - _t0);\n"
    wrap = ("\n__attribute__((unused)) static %s %s(%s) {\n"
            "    long long _t0 = extc_probe_ns();\n"
            "    %s _n = %s(%s);\n"
            "%s"
            "    return _n;\n}\n") % (ret, name, args, ret, newname, callargs, rec)
    return src[:start] + renamed + body + wrap + src[end:], True

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
        # 增量形态（good-23 家族）：collect = 起轮/推进 + finish。按函数体插入计时：
        # t0 在签名后、t2 在 collect 自己的 `return r->swFreed;` 前。mark/sweep 不再分开记
        # （finish 内部才起清扫，边界拿不到），max_mark_us 记 0、max_pause_us 仍是进→出。
        SIG3 = "static size_t extc_rc_collect(ExtcRcRegion *r, void *const *roots, size_t nroots) {\n"
        if src.count(SIG3) != 1:
            sys.stderr.write("插桩失败：collect 形态不认识（整段 %d / 分步 %d）\n" % (src.count(OLD), src.count(OLD2)))
            return 1
        i = src.index(SIG3) + len(SIG3)
        j = src.find("    return r->swFreed;", i)
        if j < 0:
            sys.stderr.write("插桩失败：collect 形态不认识（签名有、收尾没有）\n")
            return 1
        body = src[i:j]
        src = (src[:i] + "    long long extc_probe_t0 = extc_probe_ns();\n" + body +
               "    long long extc_probe_t2 = extc_probe_ns();\n"
               "    extc_probe_n++;\n"
               "    extc_probe_record(extc_probe_t2 - extc_probe_t0);\n"
               "    extc_probe_sum += extc_probe_t2 - extc_probe_t0;\n"
               "    if (extc_probe_t2 - extc_probe_t0 > extc_probe_max) extc_probe_max = extc_probe_t2 - extc_probe_t0;\n"
               "    if (extc_probe_t2 - extc_probe_t0 > extc_probe_max_mark) extc_probe_max_mark = extc_probe_t2 - extc_probe_t0;\n"
               + src[j:])
    # `sweeps` 口径保持\"扫完的轮数\"：分步清扫在 completion 处计数（不是 collect 入口）
    src = src.replace("    r->sweeps++;", "    extc_probe_n++;\n    r->sweeps++;", 1)
    src = src.replace(MARKER, GLOBALS + "\n" + MARKER, 1)
    src = src.replace(OLD, NEW, 1)

    # 热路径停顿 = **一个有界工作单元**：mark 步（extc_rc_step）、mark 推进批
    # （extc_rc_markAdvance，只在真的推进时记）、sweep 步（extc_rc_sweepStep）。
    # 三者一律用包装而不是「在 return 前插一行」：后者只覆盖尾部那一条出口，
    # 早退路径全漏记（digraph 上曾把 4.7 ms 的停顿总量读成 28 us）。
    src, step_seen = wrap_timed(src, "size_t extc_rc_step(ExtcRcRegion *r, void *const *roots, size_t nroots) {\n",
                                "extc_rc_step_pbody", "size_t",
                                "ExtcRcRegion *r, void *const *roots, size_t nroots", "r, roots, nroots", "")
    src, adv_seen = wrap_timed(src, "size_t extc_rc_markAdvance(ExtcRcRegion *r) {\n",
                               "extc_rc_markAdvance_pbody", "size_t",
                               "ExtcRcRegion *r", "r", "n")
    src, sw_seen = wrap_timed(src, "int extc_rc_sweepStep(ExtcRcRegion *r, uint32_t budget) {\n",
                              "extc_rc_sweepStep_pbody", "int",
                              "ExtcRcRegion *r, uint32_t budget", "r, budget", "")

    # 自检：插桩必须**真的**插进去。任何一种"静默返回原文"都会让调用方把"没有探针"错读成
    # `sweeps = 0`（闸门文案会写成"清扫次数 = 无"，把人引到"回收没发生"的错误结论上 ——
    # 本仓库已经吃过一次：增量 collect 形态认不出时，`owed-home` 被误判成"家区域从没被扫过"）。
    # 所以这里对四件事硬断言；不过就 rc=1，让闸门红在"工具不兼容"上。
    checks = [
        ("计时探针 globals", "static void extc_probe_dump(void)" in src),
        ("完成计数（r->sweeps++ 处）", "extc_probe_n++;\n    r->sweeps++;" in src),
        ("collect 进→出计时", "long long extc_probe_t0 = extc_probe_ns();" in src
                          and "long long extc_probe_t2 = extc_probe_ns();" in src),
        ("atexit 挂载", "__attribute__((constructor)) static void extc_probe_arm" in src),
    ]
    if step_seen:
        checks.append(("mark 步计时（包装）", "extc_rc_step_pbody(" in src))
    if adv_seen:
        checks.append(("mark 推进计时（包装）", "extc_rc_markAdvance_pbody(" in src))
    if sw_seen:
        checks.append(("sweep 步计时（包装）", "extc_rc_sweepStep_pbody(" in src))
    bad = [name for name, ok in checks if not ok]
    if bad:
        sys.stderr.write("插桩失败（自检未通过）：%s —— 这是**工具不兼容**，不是目标程序 sweeps=0；"
                         "先修 tools/rc-pause-probe.py 的形态识别\n" % "、".join(bad))
        return 1
    sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
