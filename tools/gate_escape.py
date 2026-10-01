#!/usr/bin/env python3
"""Gate ⑤ · 逃逸健全性：这些程序**必须被拒绝**。

这是 R1/R3 批次的判据（审计 §7 的第五道闸门）。语料在 `tools/escape-corpus/`：每份都是
审计里某个"记账比真实情况小"的洞的最小复现——它们现在**被接受**，而生成物在 ASan 下真的
heap-use-after-free / SEGV / 编不过。所以判据只有一个方向：

    语料里的程序**必须**让编译器报错（带源位置）。
    被接受 ⇒ 红；基线里记着的是"已知还在的洞"，修好一条就从基线删一条（棘轮只往一个方向转）。

`control_*.extc` 是**对照组**：与某个洞同一形状、但已经能被正确拒绝的孪生程序。它们
**永远不许进基线**——否则这个闸门会退化成"一律拒绝就算过"。

用法：
    python3 tools/gate_escape.py [--update-allowlist] [--verbose]
    python3 tools/gate_escape.py --show <文件名>      # 打印某条现在为什么被接受/拒绝
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

CORPUS = os.path.join(G.ROOT, "tools", "escape-corpus")
ALLOW = os.path.join(G.ROOT, "tools", "gate-escape-known-bad.txt")


def expectation(path):
    """reject (default) or run -- see the module docstring."""
    txt = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"gate-expect:\s*([a-z]+)", txt)
    exp = m.group(1) if m else "reject"
    m2 = re.search(r"gate-expect-out:\s*(.+)", txt)
    return exp, (m2.group(1).strip() if m2 else "")


def check_one(path):
    """None when the program behaves as its gate-expect says, else the reason."""
    exp, want_out = expectation(path)
    out_c = "/dev/null" if exp == "reject" else "/tmp/rev/esc.c"
    rc, out = G.run([G.EXTC, "-w", path, "-o", out_c], timeout=90)
    if exp == "reject":
        if rc == 124:
            return "超时（既没拒绝也没报错）"
        if rc == 0:
            return "被接受（应当报错）"
        if ": error:" not in out:
            return "退出码非 0 但没有带源位置的 error 诊断：" + " ".join(out.split())[:120]
        return None
    if rc != 0:
        return "被拒绝了（应当接受）: " + " ".join(out.split())[:140]
    rc2, log = G.run(["gcc", "-std=c11", "-O1", "-g", "-fwrapv", "-fsanitize=address,undefined",
                      "-fno-sanitize-recover=all", "/tmp/rev/esc.c", "-o", "/tmp/rev/esc"], timeout=180)
    if rc2 != 0:
        return "生成的 C 编不过: " + " ".join(log.split())[-140:]
    rc3, run_out = G.run(["/tmp/rev/esc"], timeout=60, cwd=G.ROOT, env=G.asan_env())
    for bad in ("AddressSanitizer", "runtime error:", "LeakSanitizer", "trap:"):
        if bad in run_out:
            return "跑起来不干净(" + bad + "): " + " ".join(run_out.split())[:140]
    if want_out and want_out not in run_out:
        return "输出里没有 [" + want_out + "]: " + " ".join(run_out.split())[:140]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--show", metavar="FILE")
    a = ap.parse_args()

    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(CORPUS, "*.extc")))
    if a.show:
        for n in names:
            if a.show in n:
                r = check_one(os.path.join(CORPUS, n))
                print(f"{n}: {'拒绝 ✓' if r is None else r}")
        return 0

    fails = {}
    for n in names:
        r = check_one(os.path.join(CORPUS, n))
        if r:
            fails[n] = r
        elif a.verbose:
            print(f"  ok   {n}（已拒绝）")

    controls_bad = {k: v for k, v in fails.items() if k.startswith("control_")}
    print(f"[escape] 语料 {len(names)} 份：被接受 {len(fails)}（其中对照组 {len(controls_bad)}）")
    if a.update_allowlist:
        if controls_bad:
            print("  对照组被接受，拒绝写基线：", ", ".join(sorted(controls_bad)))
            return 1
        G.write_allow(ALLOW, fails)
        return 0

    allow = G.load_allow(ALLOW)
    bad_controls = {k: v for k, v in controls_bad.items() if k in allow}
    if bad_controls:
        print("  对照组进了基线，这是不允许的（闸门会退化成『一律拒绝』）：", ", ".join(sorted(bad_controls)))
    rc = G.report("escape", fails, allow, detail_lines=2, eligible=set(names))
    return rc


if __name__ == "__main__":
    sys.exit(main())
