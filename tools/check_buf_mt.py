#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""闸门·buf 基质的两条编译期决定：回收量子缓存 + 锁只在需要时发射。

买的是什么：bench/containers v4（20 万个小容器生死循环）在池版本上是 4 ms，在预留基质的第一版
上是 13 ms。两处归因手术把它拉回 3 ms：

  ① 回收量子：give() 先把 order-0 块挂进 cache（mag0）再去合并，carve() 先看 cache 再切大块。
     合并/切分的 15 层记账在 4 KiB 的容器上就是每容器的全部开销。
  ② 锁只在需要时：arena 是进程级结构，只有程序能起线程时才需要锁；par 是 extC 程序起线程的
     唯一途径，所以发射器在函数体全部发射完之后（g->needPar 已定）写 EXTC_BUF_MT。

判据只有结构 + 红自测：这三条都是性能/正确性的编译期选择，行为判据看不见回潮（把 mag0 删掉
仍然正确，只是慢 4 倍；把 MT 恒写 1 也仍然正确，只是每容器多两个原子）。数字记在
文档项目 design/BUF-INVARIANT.md 与 bench/containers/RESULTS.tsv，这里只判结构。

用法：
    python3 tools/check_buf_mt.py             判据（需要 build/extc）
    python3 tools/check_buf_mt.py --selftest  红自测：每条判据都要能被一个变异打红
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTC = os.path.join(ROOT, "build", "extc")

PROBE_ST = """// 单线程：buf 基质的锁应当被编译掉（EXTC_BUF_MT 0）。
use std::io
use std::data::buf

fn main() -> i32 {
    var b: buf<i64> = buf<i64>::withCap(4)
    var i: i64 = 0
    while i < 100 { b.push(i)  i = i + 1 }
    io::cout << b.len() << "\\n"
    return 0
}
"""

PROBE_PAR = """// 有 par：同一块基质必须留着锁（EXTC_BUF_MT 1）。
use std::io
use std::data::buf

fn main() -> i32 {
    let n: i64 = 256
    var acc: mut slice<i64> = new i64[n]
    var b: buf<i64> = buf<i64>::withCap(4)
    var i: i64 = 0
    while i < 100 { b.push(i)  i = i + 1 }
    par j in 0..n { acc[j] = j }
    io::cout << b.len() << " " << acc[0] << "\\n"
    return 0
}
"""


def emit(src: str, out: str) -> str:
    with open(out + ".extc", "w", encoding="utf-8") as f:
        f.write(src)
    p = subprocess.run([EXTC, "-w", "--no-line-map", "-o", out, out + ".extc"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("extc 失败：" + p.stderr.strip()[:400])
    with open(out, encoding="utf-8") as f:
        return f.read()


def p_st(x: str) -> bool:
    return "#define EXTC_BUF_MT 0" in x


def p_par(x: str) -> bool:
    return "#define EXTC_BUF_MT 1" in x


def p_mag(x: str) -> bool:
    # give() 把 order-0 块挂进 cache，然后才进入合并循环。两处都要在 buf 基质自己的那段里找：
    # 回收运行期也有一个同形的 buddy 变量，从文件头开始找会先撞上它。
    i = x.find("a->mag0 = b;")
    j = x.find("buddy = b ^ ((int32_t)1 << k);", i + 1) if i >= 0 else -1
    return i >= 0 and j > i


def p_pop(x: str) -> bool:
    return "if (ord == 0 && a->mag0 >= 0)" in x


def p_last(x: str) -> bool:
    # 单线程的 span 快路必须整体在 #if !EXTC_BUF_MT 里：多线程时那对 lastAt/lastSpan 无锁读
    return x.count("#if !EXTC_BUF_MT") == 5 and "if (at == extc_buf_lastAt)" in x


def p_lock(x: str) -> bool:
    return ("while (__atomic_exchange_n(&extc_buf_lock, 1, __ATOMIC_ACQUIRE))" in x
            and x.count("#if EXTC_BUF_MT") >= 2)


def main() -> int:
    selftest = "--selftest" in sys.argv[1:]
    if not os.access(EXTC, os.X_OK):
        print("  FAIL build/extc 不存在（先 make build/extc）")
        return 1
    with tempfile.TemporaryDirectory() as d:
        st = emit(PROBE_ST, os.path.join(d, "st"))
        par = emit(PROBE_PAR, os.path.join(d, "par"))

    checks = [
        ("单线程：EXTC_BUF_MT 0", p_st(st)),
        ("有 par：EXTC_BUF_MT 1", p_par(par)),
        ("give 先回收再合并（mag0）", p_mag(st) and p_mag(par)),
        ("carve 先看回收缓存", p_pop(st) and p_pop(par)),
        ("span 快路只在单线程档", p_last(st)),
        ("锁的原子交换在 MT 档里", p_lock(par)),
    ]
    bad = 0
    for name, ok in checks:
        if ok:
            print("  ok   " + name)
        else:
            print("  FAIL " + name)
            bad = 1

    mutations = [
        ("MT 恒写 0", lambda x: x.replace("#define EXTC_BUF_MT 1", "#define EXTC_BUF_MT 0"), p_par, par),
        ("MT 恒写 1", lambda x: x.replace("#define EXTC_BUF_MT 0", "#define EXTC_BUF_MT 1"), p_st, st),
        ("give 不回收（删 mag0 挂链）", lambda x: x.replace("            a->mag0 = b;\n", ""), p_mag, st),
        ("carve 不看缓存", lambda x: x.replace("if (ord == 0 && a->mag0 >= 0)", "if (0)"), p_pop, st),
        ("span 快路去掉单线程闸", lambda x: x.replace("#if !EXTC_BUF_MT", ""), p_last, st),
        ("锁不再在 MT 闸内", lambda x: x.replace("while (__atomic_exchange_n(&extc_buf_lock, 1, __ATOMIC_ACQUIRE)) { }",
                                                "while (0) { }"), p_lock, par),
    ]
    caught = 0
    for name, mut, pred, text in mutations:
        m = mut(text)
        if m == text:
            print("  FAIL 变异没生效：" + name)
            bad = 1
            continue
        if pred(m):
            print("  FAIL 变异没被抓到：" + name)
            bad = 1
        else:
            caught += 1
    if selftest and caught != len(mutations):
        print("  FAIL 红自测只抓到 %d/%d" % (caught, len(mutations)))
        bad = 1
    if bad:
        return 1
    print("ok buf substrate decisions (%d checks, %d mutations caught)" % (len(checks), caught))
    return 0


if __name__ == "__main__":
    sys.exit(main())
