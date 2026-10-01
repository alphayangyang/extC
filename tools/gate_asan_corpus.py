#!/usr/bin/env python3
"""Gate 2 · 正例生成物在 ASan+UBSan 下真的跑干净。

这是审计 §7 闸门 ②。为什么需要它：`--run` 默认 `-O2`，会把"从 NULL memmove"这类 UB
直接优化掉，测试反而变绿（审计 P0-8 就是这样藏的）。本闸门用 `-O1 -fno-sanitize-recover=all`
把 UB 变成**必须报**。

用法：
    python3 tools/gate_asan_corpus.py [--scope quick|full] [--update-allowlist] [--jobs N]

判据（tools/gate-asan-known-bad.txt 是基线）：
    · 新增 sanitizer 报告 / 新增运行失败 ⇒ 红；
    · 基线里已经不再失败的条目 ⇒ 红（棘轮）。
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatecommon as G

ALLOW = os.path.join(G.ROOT, "tools", "gate-asan-known-bad.txt")
# A report is `ERROR: AddressSanitizer ...` / `runtime error: ...` (UBSan) / a LeakSanitizer
# summary. `WARNING: AddressSanitizer failed to allocate` is **not** a report: with
# `allocator_may_return_null=1` it is exactly what `tests/traps/arena_oom.extc` wants --
# the 1 PB request fails, extC's own "out of arena memory" trap runs, and the test passes.
def san_hit(line):
    if "failed to allocate" in line or "allocator is out of memory" in line:
        return False
    if line.startswith("SUMMARY: AddressSanitizer") and "failed to allocate" in line:
        return False
    return ("ERROR: AddressSanitizer" in line or "runtime error:" in line
            or "LeakSanitizer" in line or "UndefinedBehaviorSanitizer" in line
            or "stack-buffer-overflow" in line or "heap-use-after-free" in line
            or "stack-use-after-scope" in line)


def is_trap_case(path):
    return "tests/traps/" in path or "/errors/" in path


def check_one(f):
    name = G.rel(f)
    out_c = os.path.join(G.GATEDIR, "asan", name.replace("/", "_") + ".c")
    os.makedirs(os.path.dirname(out_c), exist_ok=True)
    ok, diag = G.gen_c(f, out_c, timeout=90)
    if not ok:
        return None                       # extC 自己拒绝：不是本闸门的事
    rc, log = G.compile_c(out_c, cc="gcc", sanitize=True)
    if rc != 0:
        # A helper module (a file that declares types/traits for another program and has no
        # `main`) is not a program: `tests/impl/orphanmod.extc` is one, and linking it fails
        # with "undefined reference to `main`". Skip those instead of calling them failures.
        if "undefined reference to `main" in log or "undefined symbol: main" in log:
            return None
        return (name, "cc: " + (log.strip().splitlines() or ["?"])[-1])
    exe = out_c[:-2]
    # cwd is the repo root, which is where every suite's own run.sh runs its binaries from:
    # tests that open a relative file (`tests/impl/file_string.extc`) need it.
    rc, out = G.run([exe], timeout=60, cwd=G.ROOT, env=G.asan_env())
    if rc == 124:
        return (name, "run: 超时")
    hits = [l for l in out.splitlines() if san_hit(l)]
    if hits:
        return (name, "\n".join(hits[:4]))
    # The exit code is **not** a criterion: several positive programs return a computed value
    # (`tests/arena-promoted/R2a` returns the slice length, 4) and their own suite checks the
    # printed output. What this gate cares about is UB; an unexpected `trap:` is one.
    if "trap:" in out and not is_trap_case(f):
        return (name, "run: 非预期 trap：" + next(l for l in out.splitlines() if "trap:" in l)[:120])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scope", choices=["quick", "full"], default="full")
    ap.add_argument("--update-allowlist", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()

    G.ensure_gatedir()
    os.makedirs(os.path.join(G.GATEDIR, "asan"), exist_ok=True)
    files = G.positive_corpus(a.scope)
    fails = {}
    with G.ProcessPoolExecutor(max_workers=a.jobs or G.jobs()) as ex:
        for r in ex.map(check_one, files, chunksize=3):
            if r:
                fails[r[0]] = r[1]
    print(f"[asan] 跑了 {len(files)} 个正例（ASan+UBSan，-fno-sanitize-recover）")
    if a.update_allowlist:
        G.write_allow(ALLOW, fails)
        return 0
    return G.report("asan", fails, G.load_allow(ALLOW), detail_lines=4,
                    eligible={G.rel(f) for f in files})


if __name__ == "__main__":
    sys.exit(main())
