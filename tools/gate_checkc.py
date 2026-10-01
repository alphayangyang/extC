#!/usr/bin/env python3
"""Gate 1 · 全语料 `--check-c`：挡住"extc 报成功、生成物编不过 / 有可疑告警"。

这是审计 §7 闸门 ①。它回答的问题只有一个：
**存在这样的文件吗——`./build/extc` 退出码 0，而 gcc/clang 编不过（或有本文列出的告警）？**

用法：
    python3 tools/gate_checkc.py [--scope quick|full] [--update-allowlist] [--jobs N]

判据（tools/gate-known-bad.txt 是基线）：
    · 新失败 ⇒ 红；
    · 基线里已经不再失败的条目 ⇒ 红（棘轮：修好就要把条目删掉）。
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

ALLOW = os.path.join(G.ROOT, "tools", "gate-known-bad.txt")


def check_one(args):
    f, ccs = args
    name = G.rel(f)
    out_c = os.path.join(G.GATEDIR, "checkc", name.replace("/", "_") + ".c")
    ok, diag = G.gen_c(f, out_c, timeout=90)
    if not ok:
        return None                      # extC 自己拒绝了：不是本闸门的事
    out = []
    for cc in ccs:
        rc, log = G.compile_c(out_c, cc=cc)
        if rc != 0:
            first = [l for l in log.splitlines() if " error:" in l or "Error:" in l]
            out.append(f"{cc}: {first[0] if first else log.splitlines()[:1]}")
            continue
        for line in log.splitlines():
            if G.GEN_WARN.search(line):
                out.append(f"{cc}: {line.strip()}")
    if out:
        return (name, "\n".join(out[:6]))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scope", choices=["quick", "full"], default="full")
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()

    G.ensure_gatedir()
    os.makedirs(os.path.join(G.GATEDIR, "checkc"), exist_ok=True)
    ccs = ["gcc"] if a.scope == "quick" else ["gcc", "clang"]
    files = G.corpus(a.scope)
    fails = {}
    with G.ProcessPoolExecutor(max_workers=a.jobs or G.jobs()) as ex:
        for r in ex.map(check_one, [(f, ccs) for f in files], chunksize=4):
            if r:
                fails[r[0]] = r[1]
    print(f"[checkc] 扫了 {len(files)} 个文件（{'+'.join(ccs)}）")
    if a.update_allowlist:
        G.write_allow(ALLOW, fails)
        return 0
    return G.report("checkc", fails, G.load_allow(ALLOW), detail_lines=4,
                    eligible={G.rel(f) for f in files})


if __name__ == "__main__":
    sys.exit(main())
