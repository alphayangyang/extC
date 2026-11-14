#!/usr/bin/env python3
"""tools/probe/rc-markcost.py — 从生成的 C 里切出回收运行期，或给它注入 markStep/sweepStep 计时。

两个用途（对应 docs/analysis/CALIBRATION.md 第 11 节的复现命令）：

  1) slice：把生成 C 里的运行期那一段切出来成 rt.inc，供 markcost.c 直接 #include；
  2) inject：把 markStep / sweepStep 包一层计时（时钟取 extc_rc_pauseNow），
     进程退出时（destructor）打 [mi] mark_ns / mark_n / ns_per_pop / sweep_ns / sweep_n。

为什么要注入：形如「每 pop 多少 ns」「每 KB 容量多少 ns」的量，运行期自己的 [step] 直方图给不出来
（[chan] 只覆盖 regionfree/release/trigger-step 三个非热路径通道）。注入不改语义：只加两次读钟。

用法：
  ./build/extc tests/gc-p0/src/pb_escape_grow.extc -o /tmp/pb.c
  python3 tools/probe/rc-markcost.py slice  /tmp/pb.c -o /tmp/rt.inc
  python3 tools/probe/rc-markcost.py inject /tmp/pb.c -o /tmp/pb-mi.c
  cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -o /tmp/pb-mi /tmp/pb-mi.c && /tmp/pb-mi 20000000
"""
import argparse
import sys

NL = chr(10)
BS = chr(92)
Q = chr(34)

MARK_HEAD = "__attribute__((unused)) static size_t extc_rc_markStep(ExtcRcRegion *r, size_t budget) {"
SWEEP_HEAD = "__attribute__((unused)) static int extc_rc_sweepStep(ExtcRcRegion *r, uint32_t budget) {"


def read(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)


def find_end(text, start):
    """start = 定义行里左花括号所在行首；返回匹配右花括号之后的位置。"""
    i = text.index("{", start)
    depth = 0
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise RuntimeError("unbalanced braces from offset " + str(start))


def slice_runtime(text):
    lines = text.split(NL)
    i = next(k for k, l in enumerate(lines) if "reclaim runtime:" in l)
    j = next(k for k, l in enumerate(lines) if "extc_rc_slabCount(const ExtcRcRegion" in l)
    end = next(k for k in range(j, len(lines)) if lines[k] == "}")
    return NL.join(lines[i:end + 1]) + NL


def inject(text):
    fmt = Q + "[mi] mark_ns=%lld mark_n=%llu ns_per_pop=%.3f sweep_ns=%lld sweep_n=%llu" + BS + "n" + Q
    counter = NL.join([
        "/* ---- injected accounting (rc-markcost.py) ---- */",
        "static long long extc_mi_mark_ns, extc_mi_sweep_ns;",
        "static unsigned long long extc_mi_mark_n, extc_mi_sweep_n;",
        "static void extc_mi_report(void) __attribute__((destructor));",
        "static void extc_mi_report(void) {",
        "    fprintf(stderr, " + fmt + ",",
        "            extc_mi_mark_ns, extc_mi_mark_n,",
        "            extc_mi_mark_n ? (double)extc_mi_mark_ns / (double)extc_mi_mark_n : 0.0,",
        "            extc_mi_sweep_ns, extc_mi_sweep_n);",
        "}",
        "",
    ])
    specs = [("markStep", "size_t", "ExtcRcRegion *r, size_t budget", "mark"),
             ("sweepStep", "int", "ExtcRcRegion *r, uint32_t budget", "sweep")]
    for name, ret, argdecl, tag in specs:
        head = "__attribute__((unused)) static " + ret + " extc_rc_" + name + "(" + argdecl + ") {"
        assert head in text, "definition not found: extc_rc_" + name
        renamed = head.replace("extc_rc_" + name + "(", "extc_rc_" + name + "_i(")
        text = text.replace(head, renamed, 1)
        pos = text.index(renamed)
        end = find_end(text, pos)
        wrapper = NL.join([
            "",
            "/* wrapper injected by rc-markcost.py: two clock reads per step */",
            "__attribute__((unused)) static " + ret + " extc_rc_" + name + "(" + argdecl + ") {",
            "    long long _t0 = extc_rc_pauseNow();",
            "    " + ret + " _k = extc_rc_" + name + "_i(r, budget);",
            "    extc_mi_" + tag + "_ns += extc_rc_pauseNow() - _t0;",
            "    extc_mi_" + tag + "_n += (unsigned long long)_k;",
            "    return _k;",
            "}",
            "",
        ])
        text = text[:end] + wrapper + text[end:]
    anchor = "static long long extc_rc_pauseNow(void);"
    assert anchor in text, "pauseNow forward declaration not found"
    pos = text.index(anchor)
    pos = text.index(NL, pos) + 1
    return text[:pos] + counter + text[pos:]


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("mode", choices=["slice", "inject"])
    ap.add_argument("genc", help="extc 生成的 C 文件")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args(argv)
    text = read(a.genc)
    out = slice_runtime(text) if a.mode == "slice" else inject(text)
    write(a.out, out)
    print("wrote", a.out, len(out.splitlines()), "lines (mode=" + a.mode + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))