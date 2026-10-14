#!/usr/bin/env python3
# bench/gc/jit_stats.py —— JIT 家族的停顿/编译事件解析（task-23 阶段 2 用）。
#
# 只管**解析**，不产生任何事件；输入都是诊断轮落下来的日志文件 ⇒ 不碰产物 stdout ✓。
#
# 用法：
#   jit_stats.py java-gc  <stderr 日志>    # java -Xlog:gc:stderr
#   jit_stats.py java-jit <stderr 日志>    # java -Xlog:jit+compilation:stderr
#   jit_stats.py node-gc  <stdout 日志>    # node --trace-gc（V8 只能走 stdout ✗，见 §6.10.4）
#   jit_stats.py node-opt <stdout 日志>    # node --trace-opt --trace-deopt
#   jit_stats.py selftest                  # 注入自检：缺事件行 ⇒ 必须非 0（会红）
#
# 输出：单行可解析；**"真的没有事件"必须显式报 0**（`n=0`），缺行才算 FAIL ✗。
import re
import sys

# [0.052s][info][gc] GC(0) Pause Young (Concurrent Start) (G1 Humongous Allocation) 111M->109M(248M) 1.110ms
JAVA_GC = re.compile(r"\[[0-9.]+s\]\[info\]\[gc\] GC\((\d+)\) Pause (\S+)[^0-9]*([0-9]+)M->([0-9]+)M\(([0-9]+)M\) ([0-9.]+)ms")
JAVA_GC_ANY = re.compile(r"\[info\]\[gc\]")
JAVA_JIT = re.compile(r"\[info\]\[jit,compilation\]\s+(\d+)\s+(\d+)\s+(\S+) \(([0-9]+) bytes\)")
JAVA_JIT_ANY = re.compile(r"\[info\]\[jit,compilation\]")

# [812518:0x2d21c000]       30 ms: Scavenge 5.7 (7.0) -> 5.3 (8.0) MB, pooled: 0 MB, 0.58 / 0.00 ms
NODE_GC = re.compile(r"^\S+\s+([0-9]+) ms: (\w[\w ]*?) ([0-9.]+) \(([0-9.]+)\) -> ([0-9.]+) \(([0-9.]+)\) MB.*?([0-9.]+) / ([0-9.]+) ms")
NODE_GC_ANY = re.compile(r"^\S+\s+[0-9]+ ms: ")
# [compiling method 0x... <JSFunction _TstNode (sfi = 0x...)> (target MAGLEV), mode: ConcurrencyMode::kConcurrent]
NODE_COMPILE = re.compile(r"^\[compiling method .*? \(target ([^)]+)\), mode: (\S+)\]")
NODE_MARKING = re.compile(r"^\[marking .*? for optimization to (\S+), ConcurrencyMode::(\S+), reason: (.*)\]")
NODE_DEOPT = re.compile(r"^\[bailout \(kind: (\S+), reason: (.*?)\): begin")


def pct(sorted_vals, p):
    return sorted_vals[(len(sorted_vals) - 1) * p // 100] if sorted_vals else 0.0


def dump_ms(tag, vals_ms):
    s = sorted(vals_ms)
    edges = [0.01, 0.1, 1.0, 5.0, 20.0, 100.0]
    b = [sum(1 for v in s if v < e) for e in edges]
    b.append(sum(1 for v in s if v >= edges[-1]))
    total = sum(s)
    print("%s n=%d total_ms=%.3f p50_ms=%.3f p90_ms=%.3f p99_ms=%.3f max_ms=%.3f "
          "b_lt10us=%d b_lt100us=%d b_lt1ms=%d b_lt5ms=%d b_lt20ms=%d b_lt100ms=%d b_ge100ms=%d"
          % (tag, len(s), total, pct(s, 50), pct(s, 90), pct(s, 99), (s[-1] if s else 0.0),
             b[0], b[1], b[2], b[3], b[4], b[5], b[6]))
    return 0


def do_java_gc(path):
    vals = []
    kinds = {}
    any_line = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if JAVA_GC_ANY.search(line):
            any_line = True
        m = JAVA_GC.search(line)
        if m:
            vals.append(float(m.group(6)))
            kinds[m.group(2)] = kinds.get(m.group(2), 0) + 1
    if not any_line:
        sys.stderr.write("FAIL: java-gc 日志里连 [info][gc] 都没有（是缺采集，不是 0 事件）\n")
        return 1
    rc = dump_ms("JAVA_GC", vals)
    print("JAVA_GC_KINDS " + " ".join("%s=%d" % kv for kv in sorted(kinds.items())))
    return rc


def do_java_jit(path):
    events = 0
    total_bytes = 0
    any_line = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if JAVA_JIT_ANY.search(line):
            any_line = True
        m = JAVA_JIT.search(line)
        if m:
            events += 1
            total_bytes += int(m.group(4))
    if not any_line:
        sys.stderr.write("FAIL: java-jit 日志里连 [info][jit,compilation] 都没有（缺采集，不是 0 事件）\n")
        return 1
    print("JAVA_JIT events=%d total_bytes=%d" % (events, total_bytes))
    return 0


def do_node_gc(path):
    vals = []
    kinds = {}
    any_line = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if NODE_GC_ANY.match(line):
            any_line = True
        m = NODE_GC.match(line)
        if m:
            vals.append(float(m.group(7)))          # 第一个数 = 本次 GC 的停顿（ms）
            k = m.group(2).strip()
            kinds[k] = kinds.get(k, 0) + 1
    if not any_line:
        sys.stderr.write("FAIL: node-gc 日志里没有 V8 trace 行（缺采集，不是 0 事件）\n")
        return 1
    rc = dump_ms("NODE_GC", vals)
    print("NODE_GC_KINDS " + " ".join("%s=%d" % kv for kv in sorted(kinds.items())))
    return rc


def do_node_opt(path):
    comp = {}
    modes = {}
    deopt = {}
    marking = 0
    any_line = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if NODE_COMPILE.match(line) or NODE_MARKING.match(line) or NODE_DEOPT.match(line):
            any_line = True
        m = NODE_COMPILE.match(line)
        if m:
            comp[m.group(1)] = comp.get(m.group(1), 0) + 1
            modes[m.group(2)] = modes.get(m.group(2), 0) + 1
            continue
        m = NODE_MARKING.match(line)
        if m:
            marking += 1
            continue
        m = NODE_DEOPT.match(line)
        if m:
            deopt[m.group(1)] = deopt.get(m.group(1), 0) + 1
    if not any_line:
        sys.stderr.write("FAIL: node-opt 日志里没有编译/反优化事件行（缺采集，不是 0 事件）\n")
        return 1
    print("NODE_JIT compiled=%d marking=%d targets=%s modes=%s deopts=%s"
          % (sum(comp.values()), marking,
             ",".join("%s:%d" % kv for kv in sorted(comp.items())) or "-",
             ",".join("%s:%d" % kv for kv in sorted(modes.items())) or "-",
             ",".join("%s:%d" % kv for kv in sorted(deopt.items())) or "-"))
    return 0


def selftest():
    import os
    import tempfile
    cases = [
        ("java-gc 缺 [info][gc]", "java-gc", "this is not a gc log\n"),
        ("java-jit 缺 [jit,compilation]", "java-jit", "[0.001s][info][gc] GC(0) Pause Young 1M->1M(2M) 0.1ms\n"),
        ("node-gc 无 trace 行", "node-gc", "nodes=1 edges=2 sum=3\n"),
        ("node-opt 无事件行", "node-opt", "nodes=1 edges=2 sum=3\n"),
    ]
    fn = {"java-gc": do_java_gc, "java-jit": do_java_jit, "node-gc": do_node_gc, "node-opt": do_node_opt}
    red = 0
    for name, fam, content in cases:
        fd, path = tempfile.mkstemp(prefix="jit-selftest-")
        with os.fdopen(fd, "w") as fh:
            fh.write(content)
        rc = fn[fam](path)
        os.unlink(path)
        print("SELFTEST %-28s expect=red got=%s" % (name, "red ✓" if rc != 0 else "green ✗ 门禁不会红"))
        if rc != 0:
            red += 1
    # 必须"绿"的对照：**真的 0 事件**（有头部行，但没有 GC/编译事件）
    fd, path = tempfile.mkstemp(prefix="jit-selftest-")
    with os.fdopen(fd, "w") as fh:
        fh.write("[0.002s][info][gc] Using G1\n")
    rc = do_java_gc(path)
    os.unlink(path)
    print("SELFTEST %-28s expect=green got=%s" % ("java-gc 真的 0 事件（有头部）", "green ✓" if rc == 0 else "red ✗"))
    return 0 if (red == 4 and rc == 0) else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        return selftest()
    if len(sys.argv) != 3:
        sys.stderr.write("usage: jit_stats.py {java-gc|java-jit|node-gc|node-opt} <log> | selftest\n")
        return 2
    fam = sys.argv[1]
    fn = {"java-gc": do_java_gc, "java-jit": do_java_jit, "node-gc": do_node_gc, "node-opt": do_node_opt}
    if fam not in fn:
        sys.stderr.write("unknown family: %s\n" % fam)
        return 2
    return fn[fam](sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
