#!/usr/bin/env python3
"""make_stamped_app.py —— 由 bench/app 的三个**业务形状源码**派生"应用侧逐单位时间戳"版本。

口径（与 bench/gc/stamped/make_stamped.py 同一套纪律；详见 bench/app/STALL.md §1）：

- 打戳点**只在业务单位边界**，用各语言自己的单调钟：
    extC `time::now()`（= extc_time_mono_ns ⇒ clock_gettime(CLOCK_MONOTONIC)）
    C++  `std::chrono::steady_clock`
    Go   `time.Since(base)`（time.Now 的单调部分；**不是** UnixNano 的墙钟）
- **戳数 = 单位数 + 1**：第一次 `us()` 记 t0，此后每次 `us()` 记一个增量，循环结束后补最后一次。
- 数据只写**戳文件**（路径 = argv 最后一项），stdout **一个字节都不动**。
- 文件格式（三语言逐字节一致）：
    40 字节头（小端）+ (nstamps-1) 个 unsigned-LEB128 增量
      0..7   magic  "USAPPST1"
      8..15  u64    nstamps
      16..23 i64    t0（ns，单调钟原值）
      24..31 u64    nneg（负增量个数；负值已按 0 记入流）
      32..39 u64    novf（缓冲溢出丢掉的字节数）
- **不猜**：每处替换都断言锚点在原文里恰好出现一次；少一次/多一次就整体失败退出。
- 派生文件落在 bench/app/stamped/{extc,cpp,go}/；本脚本**只读** bench/app/*.{extc,cpp,go}，不写它们。

用法：python3 bench/app/make_stamped_app.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = HERE
MAGIC = "USAPPST1"

# ---------------- extC helper ----------------
EXTC_HELP = '''
use std::time
use std::sys::io as sysio

/* ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
 * 只写**戳文件**（路径来自 argv 最后一项）；stdout 一个字节都不动。
 * 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
 * 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
 * ============================================================================ */
let US_CAP: i64 = {cap}
var USB: [{cap}]u8
var USBL: i64 = 40
var USN: i64 = 0
var UST0: i64 = 0
var USPREV: i64 = 0
var USNEG: i64 = 0
var USOVF: i64 = 0
var USSTART: bool = false
var USPB: [512]u8
var USPL: i64 = 0

fn usByte(b: u8) {
    if USBL < US_CAP { USB[USBL] = b  USBL = USBL + i64(1) } else { USOVF = USOVF + i64(1) }
}

fn usSet64(off: i64, v: u64) {
    var k: i64 = 0
    while k < i64(8) { USB[off + k] = u8(v & u64(255))  v = v >> u64(8)  k = k + i64(1) }
}

fn us() {
    let t = time::now()
    if !USSTART {
        USSTART = true
        UST0 = t
        USPREV = t
        USN = i64(1)
        return
    }
    var d: i64 = t - USPREV
    if d < i64(0) { USNEG = USNEG + i64(1)  d = i64(0) }
    USPREV = t
    USN = USN + i64(1)
    var v: u64 = u64(d)
    while v >= u64(128) {
        usByte(u8((v & u64(127)) | u64(128)))
        v = v >> u64(7)
    }
    usByte(u8(v))
}

fn usSetPath(p: slice<u8>) {
    var k: i64 = 0
    while k < p.len && k < i64(511) { USPB[k] = p[k]  k = k + i64(1) }
    USPL = k
    USPB[k] = u8(0)
}

fn usDump() {
    USB[0] = u8(85)  USB[1] = u8(83)  USB[2] = u8(65)  USB[3] = u8(80)
    USB[4] = u8(80)  USB[5] = u8(83)  USB[6] = u8(84)  USB[7] = u8(49)
    usSet64(i64(8), u64(USN))
    usSet64(i64(16), u64(UST0))
    usSet64(i64(24), u64(USNEG))
    usSet64(i64(32), u64(USOVF))
    if USPL == i64(0) { return }
    let fd = sysio::open(USPB[..USPL].data, sysio::O_WRONLY | sysio::O_CREAT | sysio::O_TRUNC, i32(420))
    if fd < i32(0) { io::cerr << "USAPP: open stamp file failed\\n"  return }
    var done: i64 = 0
    while done < USBL {
        let n = sysio::write(fd, USB[done..].data, USBL - done)
        if n <= i64(0) { break }
        done = done + n
    }
    sysio::close(fd)
}
'''

# ---------------- C++ helper ----------------
CPP_HELP = '''
/* ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
 * 只写**戳文件**（路径 = argv 最后一项）；stdout 一个字节都不动。
 * 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
 * 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
 * ============================================================================ */
#include <chrono>
#include <cstdint>
#include <fstream>
#include <vector>

static const std::size_t US_BODY = 40;
static std::vector<uint8_t> g_usBuf;
static std::size_t g_usCap = 0;
static long long g_usN = 0, g_usT0 = 0, g_usPrev = 0, g_usNeg = 0, g_usOvf = 0;
static bool g_usStart = false;

static inline long long usNow() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static inline void us() {
    long long t = usNow();
    if (!g_usStart) {
        g_usStart = true;
        g_usT0 = t;
        g_usPrev = t;
        g_usN = 1;
        return;
    }
    long long d = t - g_usPrev;
    if (d < 0) {
        g_usNeg++;
        d = 0;
    }
    g_usPrev = t;
    g_usN++;
    unsigned long long v = (unsigned long long)d;
    while (v >= 128) {
        if (g_usBuf.size() < g_usCap) g_usBuf.push_back((uint8_t)((v & 127u) | 128u));
        else g_usOvf++;
        v >>= 7;
    }
    if (g_usBuf.size() < g_usCap) g_usBuf.push_back((uint8_t)v);
    else g_usOvf++;
}

static void usReserve(long long units) {
    g_usCap = (std::size_t)(units * 5 + 64);
    g_usBuf.reserve(g_usCap);
}

static void usDump(const char* path) {
    uint8_t hdr[US_BODY];
    for (std::size_t i = 0; i < US_BODY; i++) hdr[i] = 0;
    const char* m = "USAPPST1";
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)m[i];
    unsigned long long vals[4] = {(unsigned long long)g_usN, (unsigned long long)g_usT0,
                                  (unsigned long long)g_usNeg, (unsigned long long)g_usOvf};
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) hdr[8 + j * 8 + i] = (uint8_t)(vals[j] >> (8 * i));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)hdr, (std::streamsize)US_BODY);
    if (!g_usBuf.empty()) f.write((const char*)g_usBuf.data(), (std::streamsize)g_usBuf.size());
    f.close();
}
'''

# ---------------- Go helper ----------------
GO_HELP = '''
// ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
// 只写**戳文件**（路径 = os.Args 最后一项）；stdout 一个字节都不动。
// 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
// 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
// ============================================================================
var (
	usBuf   []byte
	usCap   int64
	usN     int64
	usT0    int64
	usPrev  int64
	usNeg   int64
	usOvf   int64
	usStart bool
	usBase  time.Time
)

func usReserve(units int64) {
	usCap = units*5 + 64
	usBuf = make([]byte, 0, usCap)
	usBase = time.Now()
}

func us() {
	t := time.Since(usBase).Nanoseconds()
	if !usStart {
		usStart = true
		usT0 = t
		usPrev = t
		usN = 1
		return
	}
	d := t - usPrev
	if d < 0 {
		usNeg++
		d = 0
	}
	usPrev = t
	usN++
	v := uint64(d)
	for v >= 128 {
		if int64(len(usBuf)) < usCap {
			usBuf = append(usBuf, byte(v&127|128))
		} else {
			usOvf++
		}
		v >>= 7
	}
	if int64(len(usBuf)) < usCap {
		usBuf = append(usBuf, byte(v))
	} else {
		usOvf++
	}
}

func usDump(path string) {
	hdr := make([]byte, 40)
	copy(hdr, "USAPPST1")
	put := func(off int, val uint64) {
		for i := 0; i < 8; i++ {
			hdr[off+i] = byte(val >> (8 * uint(i)))
		}
	}
	put(8, uint64(usN))
	put(16, uint64(usT0))
	put(24, uint64(usNeg))
	put(32, uint64(usOvf))
	f, err := os.Create(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, "USAPP: create stamp file failed:", err)
		return
	}
	f.Write(hdr)
	f.Write(usBuf)
	f.Close()
}
'''

# ---------------------------------------------------------------- 形状表
# units：单位数关于 ops 的表达式（与源码里四个/一个循环的迭代次数逐字对应）
# extc_use：helper 插在哪个 use 之后
# extc_path / cpp_path / go_path：把"戳文件路径"从 argv 最后一个实参取出来的锚点与插入文本
SHAPES = {
    "session": dict(
        units="ops",
        cap=10_000_114,                      # 40 + 2_000_002*5 + 64（默认 ops=2000000）
        extc_use="use stl::hashMap\n",
        extc_path_anchor="    if args.len > i64(2) { active = toInt(args[2], active) }\n",
        extc_path_ins="    if args.len > i64(3) { usSetPath(args[3]) }\n",
        extc_loops=["    while i < ops {\n"],
        extc_loops_ins="        us()\n",
        extc_tail="    io::cout << \"a.len=\" << live.len()",
        cpp_include="#include <unordered_map>\n",
        cpp_path_anchor="    if (argc > 2) active = toInt(argv[2], active);\n",
        cpp_argc_n=3,
        cpp_loops=["    for (int64_t i = 0; i < ops; i++) {\n"],
        cpp_tail="    printf(\"a.len=%lld a.hits=%lld a.acc=%lld\\n\",",
        go_import_last='\t"os"',
        go_path_anchor="\tif len(os.Args) > 2 {\n\t\tactive = toInt(os.Args[2], active)\n\t}\n",
        go_argc_n=3,
        go_loops=["\tfor i := int64(0); i < ops; i++ {\n"],
        go_tail="\tfmt.Printf(\"a.len=%d a.hits=%d a.acc=%d\\n\", len(m), hits, acc%1000000007)",
    ),
    "route": dict(
        units="ops + ops + ops/4 + ops/2",
        cap=27_500_104,                      # 40 + 5_500_000*5 + 64（默认 ops=2000000）
        extc_use="use stl::map\n",
        extc_path_anchor="    if args.len > i64(1) { ops = toInt(args[1], ops) }\n",
        extc_path_ins="    if args.len > i64(2) { usSetPath(args[2]) }\n",
        extc_loops=[
            "    while i < ops {\n        u = u * u64(6364136223846793005) + u64(1442695040888963407)\n        let key = i64(u & u64(4294967040))\n",
            "    while i < ops {\n        u = u * u64(6364136223846793005) + u64(1442695040888963407)\n        let q = i64(u & u64(4294967295))\n",
            "    while i < ops / i64(4) {\n",
            "    while i < ops / i64(2) {\n",
        ],
        extc_loops_ins="        us()\n",
        extc_tail="    io::cout << \"b.ins=\"",
        cpp_include="#include <map>\n",
        cpp_path_anchor="    if (argc > 1) ops = toInt(argv[1], ops);\n",
        cpp_argc_n=2,
        cpp_loops=[
            "    for (int64_t i = 0; i < ops; i++) {\n        u = u * 6364136223846793005ULL + 1442695040888963407ULL;\n        int64_t key = (int64_t)(u & 0xFFFFFF00ULL);\n",
            "    for (int64_t i = 0; i < ops; i++) {\n        u = u * 6364136223846793005ULL + 1442695040888963407ULL;\n        int64_t q = (int64_t)(u & 0xFFFFFFFFULL);\n",
            "    for (int64_t i = 0; i < ops / 4; i++) {\n",
            "    for (int64_t i = 0; i < ops / 2; i++) {\n",
        ],
        cpp_tail="    printf(\"b.ins=%lld b.lkp=%lld b.scan=%lld b.del=%lld b.len=%lld\\n\",",
        go_import_last='\t"sort"',
        go_path_anchor="\tif len(os.Args) > 1 {\n\t\tops = toInt(os.Args[1], ops)\n\t}\n",
        go_argc_n=2,
        go_loops=[
            "\tfor i := int64(0); i < ops; i++ {\n\t\tu = u*6364136223846793005 + 1442695040888963407\n\t\tkey := int64(u & 0xFFFFFF00)\n",
            "\tfor i := int64(0); i < ops; i++ {\n\t\tu = u*6364136223846793005 + 1442695040888963407\n\t\tq := int64(u & 0xFFFFFFFF)\n",
            "\tfor i := int64(0); i < ops/4; i++ {\n",
            "\tfor i := int64(0); i < ops/2; i++ {\n",
        ],
        go_tail='\tfmt.Printf("b.ins=%d b.lkp=%d b.scan=%d b.del=%d b.len=%d\\n", ins, lkp%1000000007, scan, del, len(m))',
    ),
    "log": dict(
        units="ops",
        cap=10_000_114,
        extc_use="use stl::string\n",
        extc_path_anchor="    if args.len > i64(1) { ops = toInt(args[1], ops) }\n",
        extc_path_ins="    if args.len > i64(2) { usSetPath(args[2]) }\n",
        extc_loops=["    while i < ops {\n"],
        extc_loops_ins="        us()\n",
        extc_tail="    io::cout << \"c.appended=\"",
        cpp_include="#include <string>\n",
        cpp_path_anchor="    if (argc > 1) ops = toInt(argv[1], ops);\n",
        cpp_argc_n=2,
        cpp_loops=["    for (int64_t i = 0; i < ops; i++) {\n"],
        cpp_tail="    printf(\"c.appended=%lld c.trunc=%lld c.find=%lld c.acc=%lld c.len=%lld\\n\",",
        go_import_last='\t"os"',
        go_path_anchor="\tif len(os.Args) > 1 {\n\t\tops = toInt(os.Args[1], ops)\n\t}\n",
        go_argc_n=2,
        go_loops=["\tfor i := int64(0); i < ops; i++ {\n"],
        go_tail='\tfmt.Printf("c.appended=%d c.trunc=%d c.find=%d c.acc=%d c.len=%d\\n",\n\t\tops*REC, trunc, findSum%1000000007, acc, int64(len(buf)))',
    ),
}


def sub1(text, old, new, tag):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"[make_stamped_app] 锚点不唯一（{n} 次）：{tag}\n  <<{old[:160]}>>")
    return text.replace(old, new, 1)


def read(p):
    with open(p, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write(p, s):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)


def gen_extc():
    out = []
    for name, sp in SHAPES.items():
        t = read(os.path.join(APP, name + ".extc"))
        t = sub1(t, sp["extc_use"], sp["extc_use"] + EXTC_HELP.replace("{cap}", str(sp["cap"])),
                 name + ":help")
        t = sub1(t, sp["extc_path_anchor"], sp["extc_path_anchor"] + sp["extc_path_ins"],
                 name + ":path")
        for k, loop in enumerate(sp["extc_loops"]):
            t = sub1(t, loop, loop + sp["extc_loops_ins"], f"{name}:loop{k}")
        t = sub1(t, sp["extc_tail"], "    us()\n    usDump()\n" + sp["extc_tail"], name + ":tail")
        write(os.path.join(HERE, "stamped", "extc", name + "_us.extc"), t)
        out.append(name + "_us.extc")
    return out


def gen_cpp():
    out = []
    for name, sp in SHAPES.items():
        t = read(os.path.join(APP, name + ".cpp"))
        t = sub1(t, sp["cpp_include"], sp["cpp_include"] + CPP_HELP, name + ":help")
        path_ins = (f'    const char* usPath = (argc > {sp["cpp_argc_n"]}) '
                    f'? argv[argc - 1] : "";\n'
                    f'    usReserve((long long)({sp["units"]}));\n')
        t = sub1(t, sp["cpp_path_anchor"], sp["cpp_path_anchor"] + path_ins, name + ":path")
        for k, loop in enumerate(sp["cpp_loops"]):
            t = sub1(t, loop, loop + "        us();\n", f"{name}:loop{k}")
        t = sub1(t, sp["cpp_tail"], "    us();\n    usDump(usPath);\n" + sp["cpp_tail"], name + ":tail")
        write(os.path.join(HERE, "stamped", "cpp", name + "_us.cpp"), t)
        out.append(name + "_us.cpp")
    return out


def gen_go():
    out = []
    for name, sp in SHAPES.items():
        t = read(os.path.join(APP, name + ".go"))
        last = sp["go_import_last"]
        t = sub1(t, last + "\n)", last + "\n\t\"time\"\n)", name + ":import")
        path_ins = ("\tusPath := \"\"\n"
                    "\tif len(os.Args) > " + str(sp["go_argc_n"]) + " {\n"
                    "\t\tusPath = os.Args[len(os.Args)-1]\n"
                    "\t}\n"
                    "\tusReserve(" + sp["units"] + ")\n")
        t = sub1(t, sp["go_path_anchor"], sp["go_path_anchor"] + path_ins, name + ":path")
        for k, loop in enumerate(sp["go_loops"]):
            t = sub1(t, loop, loop + "\t\tus()\n", f"{name}:loop{k}")
        t = sub1(t, sp["go_tail"], "\tus()\n\tusDump(usPath)\n" + sp["go_tail"], name + ":tail")
        # helper 放在 import 块之后
        j = t.index(")", t.index("import (")) + 1
        while t[j] == "\n":
            j += 1
        t = t[:j] + GO_HELP + t[j:]
        write(os.path.join(HERE, "stamped", "go", name + "_us.go"), t)
        out.append(name + "_us.go")
    return out


def main():
    a, b, c = gen_extc(), gen_cpp(), gen_go()
    print(f"extC={len(a)} C++={len(b)} Go={len(c)} ⇒ bench/app/stamped/{{extc,cpp,go}}")
    print(f"magic={MAGIC} 头=40 字节（小端）+ unsigned-LEB128 增量")
    return 0


sys.exit(main())
