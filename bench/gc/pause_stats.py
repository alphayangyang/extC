#!/usr/bin/env python3
# bench/gc/pause_stats.py —— 形状 7「停顿分布」的解析小工具（task-21）。
#
# 只管**解析与分组**，不产生停顿；所有输入都来自程序**只写 stderr** 的统计行 ⇒ 不碰 stdout ✓。
#
#   pause_stats.py gctrace <stderr 文件>   解析 `GODEBUG=gctrace=1` 的**每个 GC 周期**停顿
#                                          （`a+b+c ms clock` 里的 STW = a + c，b 是并发标记）
#   pause_stats.py break   <stderr 文件>   解析 C++/Rust `DIGRAPH_BREAK_STATS=1` 的 `BREAK_DIST`
#   pause_stats.py selftest                注入自检：**故意喂缺行的输入 ⇒ 必须非 0 退出**（证明会红）
#
# 输出（单行、可解析；缺行 ⇒ 非 0 退出 ✗）：
#   GO_DIST   source=gctrace n=… p50_us=… p90_us=… p99_us=… max_us=… total_ms=… b_lt…=…
#   GO_XCHECK gctrace_n=… memstats_num_gc=… max_diff_pct=… （两条路互证）
#   BREAK_DIST source=… n=… overflow=… p50_ns=… p90_ns=… p99_ns=… max_ns=… total_ms=… b_lt…=…
import re
import sys

GC_LINE = re.compile(r"^gc \d+ @\S+ \d+%: ([0-9.]+)\+([0-9.]+)\+([0-9.]+) ms clock")
MEMSTATS = re.compile(
    r"^GOSTATS num_gc=(\d+) pause_total_ms=([0-9.]+) pause_max_us=([0-9.]+) "
    r"pause_p99_us=([0-9.]+) samples=(\d+) wall_ms=([0-9.]+) pause_share=([0-9.]+)")
BREAK_DIST = re.compile(
    r"^BREAK_DIST source=(\S+) n=(\d+) overflow=(\d+) p50_ns=([0-9.]+) p90_ns=([0-9.]+) "
    r"p99_ns=([0-9.]+) max_ns=([0-9.]+) total_ms=([0-9.]+) "
    r"b_lt100ns=(\d+) b_lt200ns=(\d+) b_lt500ns=(\d+) b_lt1us=(\d+) b_lt4us=(\d+) "
    r"b_lt16us=(\d+) b_ge16us=(\d+)")
BREAK_OLD = re.compile(r"^break_ns=(\d+) break_calls=(\d+)$")


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    return sorted_vals[(len(sorted_vals) - 1) * p // 100]


def buckets(vals, edges):
    """cumulative: edges[i] 之下（含）个数；最后一个桶是 >= 最后一个 edge。"""
    out = [0] * len(edges)
    for v in vals:
        placed = False
        for i, e in enumerate(edges[:-1]):
            if v < e:
                out[i] += 1
                placed = True
                break
        if not placed:
            out[-1] += 1
    return out


def do_gctrace(path):
    pauses = []          # us
    mem = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = GC_LINE.match(line)
            if m:
                # STW = 第一段（mark setup）+ 第三段（mark termination），单位 ms
                pauses.append((float(m.group(1)) + float(m.group(3))) * 1000.0)
                continue
            m = MEMSTATS.match(line)
            if m:
                mem = dict(num_gc=int(m.group(1)), total_ms=float(m.group(2)),
                           max_us=float(m.group(3)), p99_us=float(m.group(4)),
                           samples=int(m.group(5)), wall_ms=float(m.group(6)),
                           share=float(m.group(7)))
    if mem is None:
        sys.stderr.write("FAIL: 缺 GOSTATS 行（ReadMemStats 互证拿不到）\n")
        return 1
    if not pauses and mem["num_gc"] != 0:
        sys.stderr.write("FAIL: 没有 gctrace 行，但 ReadMemStats 说 num_gc=%d ⇒ 采集缺失\n" % mem["num_gc"])
        return 1
    s = sorted(pauses)
    b = buckets(s, [10.0, 50.0, 100.0, 250.0, 500.0, 1000.0, float("inf")])
    total = sum(s)
    print("GO_DIST source=gctrace n=%d p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f total_ms=%.3f "
          "b_lt10us=%d b_lt50us=%d b_lt100us=%d b_lt250us=%d b_lt500us=%d b_lt1ms=%d b_ge1ms=%d"
          % (len(s), pct(s, 50), pct(s, 90), pct(s, 99), (s[-1] if s else 0.0), total / 1000.0,
             b[0], b[1], b[2], b[3], b[4], b[5], b[6]))
    gmax = s[-1] if s else 0.0
    md = 0.0
    if mem["max_us"] > 0:
        md = abs(gmax - mem["max_us"]) / mem["max_us"] * 100.0
    print("GO_XCHECK gctrace_n=%d memstats_num_gc=%d memstats_total_ms=%.3f gctrace_total_ms=%.3f "
          "memstats_max_us=%.1f gctrace_max_us=%.1f max_diff_pct=%.1f memstats_p99_us=%.1f "
          "memstats_share=%.5f samples=%d"
          % (len(s), mem["num_gc"], mem["total_ms"], total / 1000.0, mem["max_us"], gmax, md,
             mem["p99_us"], mem["share"], mem["samples"]))
    return 0


def do_break(path):
    dist = None
    old = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BREAK_DIST.match(line)
            if m:
                dist = m
                continue
            m = BREAK_OLD.match(line)
            if m:
                old = (int(m.group(1)), int(m.group(2)))
    if dist is None:
        if old is not None:
            # 兼容旧口径（只有总数）：明确降级说明，仍算"有行"但不是分布
            print("BREAK_DIST source=legacy n=%d overflow=0 p50_ns=0 p90_ns=0 p99_ns=0 max_ns=0 "
                  "total_ms=%.3f b_lt100ns=0 b_lt200ns=0 b_lt500ns=0 b_lt1us=0 b_lt4us=0 "
                  "b_lt16us=0 b_ge16us=0 (旧口径只有总数，分布缺)"
                  % (old[1], old[0] / 1e6))
            return 0
        sys.stderr.write("FAIL: 缺 BREAK_DIST / break_ns 行\n")
        return 1
    print("BREAK_DIST source=%s n=%s overflow=%s p50_ns=%s p90_ns=%s p99_ns=%s max_ns=%s total_ms=%s "
          "b_lt100ns=%s b_lt200ns=%s b_lt500ns=%s b_lt1us=%s b_lt4us=%s b_lt16us=%s b_ge16us=%s"
          % tuple(dist.groups()))
    return 0


def selftest():
    """注入自检：喂**故意缺行的**输入，每个分支都必须非 0 退出（证明反瞎判据会红）。"""
    import tempfile
    import os
    cases = [
        ("gctrace 缺 GOSTATS 行", "gctrace", "gc 1 @0.001s 0%: 0.1+0.2+0.01 ms clock, 0.4+0+0+0 ms cpu\n"),
        ("gctrace 有 memstats 但无 gc 行", "gctrace",
         "GOSTATS num_gc=7 pause_total_ms=0.5 pause_max_us=100.0 pause_p99_us=90.0 samples=7 wall_ms=10.0 pause_share=0.05\n"),
        ("break 缺行", "break", "no break stats here\n"),
    ]
    red = 0
    for name, fam, content in cases:
        fd, path = tempfile.mkstemp(prefix="pause-selftest-")
        with os.fdopen(fd, "w") as fh:
            fh.write(content)
        rc = do_gctrace(path) if fam == "gctrace" else do_break(path)
        os.unlink(path)
        print("SELFTEST %-28s expect=red got=%s %s" % (name, "red ✓" if rc != 0 else "green ✗ 门禁不会红", ""))
        if rc != 0:
            red += 1
    # 一条必须"绿"的对照：显式 NumGC=0（GOGC=off 的合法情形）
    fd, path = tempfile.mkstemp(prefix="pause-selftest-")
    with os.fdopen(fd, "w") as fh:
        fh.write("GOSTATS num_gc=0 pause_total_ms=0.000 pause_max_us=0.0 pause_p99_us=0.0 samples=0 wall_ms=1.0 pause_share=0.00000\n")
    rc = do_gctrace(path)
    os.unlink(path)
    print("SELFTEST %-28s expect=green got=%s" % ("NumGC=0（合法 0，不算缺行）", "green ✓" if rc == 0 else "red ✗"))
    return 0 if (red == 3 and rc == 0) else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        return selftest()
    if len(sys.argv) != 3 or sys.argv[1] not in ("gctrace", "break"):
        sys.stderr.write("usage: pause_stats.py {gctrace|break} <stderr-file> | selftest\n")
        return 2
    return do_gctrace(sys.argv[2]) if sys.argv[1] == "gctrace" else do_break(sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
