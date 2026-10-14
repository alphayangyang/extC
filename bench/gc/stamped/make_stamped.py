#!/usr/bin/env python3
"""make_stamped.py —— 由**形状源码**派生出"应用侧逐单位时间戳"版本（task-87 / USER-STALL）。

口径（详见 bench/gc/USER-STALL.md §1）：
- 打戳点**只在形状源码的业务单位边界**，用各语言自己的单调钟：
    extC `time::now()`（= extc_time_mono_ns = clock_gettime(CLOCK_MONOTONIC)）
    Go   `time.Since(base)`（time.Now 的单调部分）
    Java `System.nanoTime()`
- 戳只写 **stderr**（`US <ns>` 一行一个 + 末尾 `USMETA n=<count> gc=<...>`），stdout 逐字节不变。
- 单位时长 = 相邻戳之差；最后一个边界戳在单位循环结束后补一次 ⇒ 戳数 = 单位数 + 1。

**不猜**：每处替换都断言锚点在原文里恰好出现一次；少一次/多一次就整体失败退出。
生成物落在 bench/gc/stamped/{extc,go,java}/。

用法：python3 make_stamped.py           # 从 bench/gc/src 与 stamped/src 读，写 stamped/{extc,go,java}
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GC = os.path.dirname(HERE)              # bench/gc
SRC = os.path.join(GC, "src")

EXTC_HELP = """
use std::time
var USN: i64 = 0
var US: [1000002]i64
fn us() { if USN < 1000002 { US[USN] = time::now()  USN = USN + 1 } }
fn usDump() {
    io::cerr << "USMETA n=" << USN << "\\n"
    var usk: i64 = 0
    while usk < USN { io::cerr << "US " << US[usk] << "\\n"  usk = usk + 1 }
    io::cerr << "USEND\\n"
}
"""

GO_IMPORTS = {
    'import ("fmt";"os";"strconv")':
        'import ("bufio";"fmt";"os";"runtime";"strconv";"time")',
}

GO_HELP = """
// ---- 应用侧单位时间戳（USER-STALL）：只写 stderr，stdout 不变 ----
var usSt []int64
var usBase time.Time

func usInit(cap int64) { usSt = make([]int64, 0, cap); usBase = time.Now() }
func us()             { usSt = append(usSt, time.Since(usBase).Nanoseconds()) }
func usDump() {
	w := bufio.NewWriterSize(os.Stderr, 1<<20)
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Fprintf(w, "USMETA n=%d gc=%d\\n", len(usSt), m.NumGC)
	for _, v := range usSt {
		fmt.Fprintf(w, "US %d\\n", v)
	}
	w.Flush()
}
"""

JAVA_HELP = """
    // ---- 应用侧单位时间戳（USER-STALL）：只写 stderr，stdout 不变 ----
    static long[] usSt; static int usN; static long usBase;
    static void usInit(long cap) { usSt = new long[(int) cap]; usN = 0; usBase = System.nanoTime(); }
    static void us() { if (usN < usSt.length) usSt[usN++] = System.nanoTime() - usBase; }
    static String usGC() {
        StringBuilder b = new StringBuilder();
        for (java.lang.management.GarbageCollectorMXBean g
                : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans()) {
            if (b.length() > 0) b.append('+');
            b.append(g.getName().replace(' ', '_')).append(':').append(g.getCollectionCount());
        }
        return b.toString();
    }
    static void usDump() {
        StringBuilder sb = new StringBuilder(1 << 21);
        sb.append("USMETA n=").append(usN).append(" gc=").append(usGC()).append('\\n');
        for (int i = 0; i < usN; i++) sb.append("US ").append(usSt[i]).append('\\n');
        System.err.print(sb); System.err.flush();
    }
"""


def sub1(text, old, new, tag):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"[make_stamped] 锚点不唯一（{n} 次）：{tag}\n  <<{old[:120]}>>")
    return text.replace(old, new, 1)


def read(p):
    with open(p, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write(p, s):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)


# ---------------------------------------------------------------- extC
# (源文件, 插入 us() 的锚点(循环头原文), 末尾补戳+落盘的锚点)
EXTC = {
    "spectrum_p1": (
        "    while i < n {\n",
        "    io::cout << \"n=\" << n << \" sum=\" << sum << \"\\n\"",
    ),
    "spectrum_p2": (
        "    while i < n { push(x, nxt() % 1000)  i = i + 1 }\n",
        "    var sum: i64 = 0",
    ),
    "spectrum_p3": (
        "    while r < rounds {\n",
        "    io::cout << \"rounds=\" << rounds",
    ),
    "spectrum_p4": (
        "    while u < units {\n",
        "    io::cout << \"units=\" << units",
    ),
    "spectrum_p5": (
        "    while i < ops {\n",
        "    io::cout << \"ops=\" << ops",
    ),
    "digraph": (
        "    while round < R {\n",
        "    var total: i64 = (acc + xacc) & MASK",
    ),
}
# 每个形状的"单位"边界在循环体里的判定（P2 是每 256 次追加一块）
EXTC_UNIT = {
    "spectrum_p1": "        us()\n",
    "spectrum_p2": "        if i % 256 == 0 { us() }\n",
    "spectrum_p3": "        us()\n",
    "spectrum_p4": "        us()\n",
    "spectrum_p5": "        us()\n",
    "digraph": "        us()\n",
}


def gen_extc():
    out = []
    for name, (loop, tail) in EXTC.items():
        t = read(os.path.join(SRC, name + ".extc"))
        t = sub1(t, "use std::io\n", "use std::io\n" + EXTC_HELP, name + ":use")
        if loop.endswith("{\n"):          # 多行循环：锚点就是循环头 ⇒ 直接接在 { 之后
            t = sub1(t, loop, loop + EXTC_UNIT[name], name + ":loop")
        else:                             # 单行循环（P2）：整条换掉，注入必须落在体内
            t = sub1(t, loop,
                     "    while i < n {\n" + EXTC_UNIT[name] +
                     "        push(x, nxt() % 1000)  i = i + 1\n    }\n",
                     name + ":loop")
        t = sub1(t, tail, "    us()\n    usDump()\n" + tail, name + ":tail")
        write(os.path.join(HERE, "extc", name + "_us.extc"), t)
        out.append(name + "_us.extc")
    return out


# ---------------------------------------------------------------- Go
GO = {
    "spectrum_p1": (
        "\tfor i := int64(0); i < n; i++ {\n\t\tb := &buf{}",
        "\tusInit(n + 2)\n\tfor i := int64(0); i < n; i++ {\n\t\tus()\n\t\tb := &buf{}",
        "\tfmt.Printf(\"n=%d sum=%d\\n\", n, sum)",
    ),
    "spectrum_p2": (
        "\tfor i := int64(0); i < n; i++ { push(x, nxt()%1000) }",
        "\tusInit(n/256 + 2)\n\tfor i := int64(0); i < n; i++ {\n\t\tif i%256 == 0 {\n\t\t\tus()\n\t\t}\n\t\tpush(x, nxt()%1000)\n\t}",
        "\tvar sum, idx int64",
    ),
    "spectrum_p3": (
        "\tfor r := int64(0); r < rounds; r++ {\n",
        "\tusInit(rounds + 2)\n\tfor r := int64(0); r < rounds; r++ {\n\t\tus()\n",
        "\tfmt.Printf(\"rounds=%d nodes=%d sum=%d\\n\", rounds, total, sum)",
    ),
    "spectrum_p4": (
        "\tfor u := int64(0); u < units; u++ {\n",
        "\tusInit(units + 2)\n\tfor u := int64(0); u < units; u++ {\n\t\tus()\n",
        "\tfmt.Printf(\"units=%d symbols=%d asts=%d sum=%d\\n\", units, t.count, asts, sum)",
    ),
    "spectrum_p5": (
        "\tfor i := int64(0); i < ops; i++ {\n",
        "\tusInit(ops + 2)\n\tfor i := int64(0); i < ops; i++ {\n\t\tus()\n",
        "\tfmt.Printf(\"ops=%d idx=%d sum=%d\\n\", ops, x.n, sum)",
    ),
    "digraph": (
        "\tfor round := int64(0); round < rounds; round++ {\n",
        "\tusInit(rounds + 2)\n\tfor round := int64(0); round < rounds; round++ {\n\t\tus()\n",
        "\ttotal := (acc + xacc) & mask",
    ),
}


def gen_go():
    out = []
    for name, (loop, loop_new, tail) in GO.items():
        t = read(os.path.join(SRC, name + ".go"))
        if name == "digraph":
            t = sub1(t, '\t"os"\n', '\t"bufio"\n\t"os"\n', name + ":import")
        else:
            t = sub1(t, 'import ("fmt";"os";"strconv")', GO_IMPORTS['import ("fmt";"os";"strconv")'], name + ":import")
        t = sub1(t, loop, loop_new, name + ":loop")
        t = sub1(t, tail, "\tus()\n\tusDump()\n" + tail, name + ":tail")
        # 把 helper 放在 import 块之后（单行 import 与多行 import 都要能收）
        j = t.index(")", t.index("import (")) + 1
        if t[j] == "\n":
            j += 1
        t = t[:j] + GO_HELP + t[j:]
        write(os.path.join(HERE, "go", name + "_us.go"), t)
        out.append(name + "_us.go")
    return out


# ---------------------------------------------------------------- Java
JAVA = {
    "SpectrumP1": (
        "        for (long i = 0; i < n; i++) { Buf b = new Buf();",
        "        usInit(n + 2);\n        for (long i = 0; i < n; i++) { us(); Buf b = new Buf();",
        "        System.out.print(\"n=\" + n + \" sum=\" + sum + \"\\n\");",
    ),
    "SpectrumP2": (
        "        for (long i = 0; i < n; i++) push(x, nxt() % 1000);",
        "        usInit(n / 256 + 2);\n        for (long i = 0; i < n; i++) { if (i % 256 == 0) us(); push(x, nxt() % 1000); }",
        "        long sum = 0, idx = 0;",
    ),
    "SpectrumP3": (
        "        for (long r = 0; r < rounds; r++) {\n",
        "        usInit(rounds + 2);\n        for (long r = 0; r < rounds; r++) {\n            us();\n",
        "        System.out.print(\"rounds=\" + rounds + \" nodes=\" + total + \" sum=\" + sum + \"\\n\");",
    ),
    "SpectrumP4": (
        "        for (long u = 0; u < units; u++) {\n",
        "        usInit(units + 2);\n        for (long u = 0; u < units; u++) {\n            us();\n",
        "        System.out.print(\"units=\" + units + \" symbols=\" + t.count + \" asts=\" + asts + \" sum=\" + sum + \"\\n\");",
    ),
    "SpectrumP5": (
        "        for (long i = 0; i < ops; i++) {\n",
        "        usInit(ops + 2);\n        for (long i = 0; i < ops; i++) {\n            us();\n",
        "        System.out.print(\"ops=\" + ops + \" idx=\" + x.n + \" sum=\" + sum + \"\\n\");",
    ),
}
JAVA_DIGRAPH = (
    "        for (long round = 0; round < rounds; round++) {\n",
    "        usInit(rounds + 2);\n        for (long round = 0; round < rounds; round++) {\n            us();\n",
    "            traverse(c, epoch);\n        }\n    }\n",
    "            traverse(c, epoch);\n        }\n        us();\n        usDump();\n    }\n",
)


def gen_java():
    out = []
    for cls, (loop, loop_new, tail) in JAVA.items():
        t = read(os.path.join(HERE, "src", cls + ".java"))
        t = sub1(t, loop, loop_new, cls + ":loop")
        t = sub1(t, tail, "        us();\n        usDump();\n" + tail, cls + ":tail")
        t = sub1(t, "    public static void main", JAVA_HELP + "\n    public static void main", cls + ":help")
        write(os.path.join(HERE, "java", cls + ".java"), t)
        out.append(cls + ".java")
    # digraph：runOnce 里的轮循环
    t = read(os.path.join(SRC, "digraph.java"))
    t = sub1(t, JAVA_DIGRAPH[0], JAVA_DIGRAPH[1], "digraph:loop")
    t = sub1(t, JAVA_DIGRAPH[2], JAVA_DIGRAPH[3], "digraph:tail")
    t = sub1(t, "    public static void main", JAVA_HELP + "\n    public static void main", "digraph:help")
    write(os.path.join(HERE, "java", "digraph.java"), t)
    out.append("digraph.java")
    return out


def main():
    a, b, c = gen_extc(), gen_go(), gen_java()
    print(f"extC={len(a)} Go={len(b)} Java={len(c)} ⇒ bench/gc/stamped/{{extc,go,java}}")
    return 0


sys.exit(main())
