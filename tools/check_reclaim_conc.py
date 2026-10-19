#!/usr/bin/env python3
"""tools/check_reclaim_conc.py --- 守住回收运行期共享表/共享计数的并发结构。

## 为什么需要这个门禁

并行 worker 各自持有自己那只回收区域，但运行期有几处**进程级共享**的状态：

1. slab 登记表（ownSlab 用它把地址映射回 slab）。写侧（slab 创建/释放）**不许 realloc 后
   free 旧表** —— 两个 worker 同时建 slab 时，一个线程正在旧表里探测、另一个把旧表 free，
   ASan 下就是 SEGV（8 路并行 400 次能复现 1~10 次）。所以走**按代发布**：写侧在
   extc_rc_regLock 下换代、原子发布新代，**旧代永不 free**；读侧无锁，探测落空后比对
   extc_rc_regSeq 决定是否重试。这类 UAF 只在多线程 + 回收同时在场时才显形，单线程棘轮
   与形状矩阵都看不见。

2. 标记者注册计数 extc_rc_wb：每只区域 begin 时 +1、finish/abort 时 -1，写入屏障靠它门控。
   多线程各自 begin/finish ⇒ 普通 int 的 ++/-- 会丢更新，一个线程减到 0 时另一个还在标记，
   下一个 finish 就 "R4: barrier participant underflow" 崩掉（实测 1/400）。必须原子。
   但原子操作本身要包进 noinline 冷函数：lock 前缀 + 内存屏障会让 GCC 不敢把 extc_rc_wb
   当普通全局，热路径每次 if (!extc_rc_wb ...) 都得重载（owed_home 实测 +1.76% Ir）。

3. ownHot 是「上一块命中的 slab」缓存，只能线程本地：共享的话，一个线程写入的是另一线程
   区域里的 slab，越过区域边界使用就是错的。

# 判据

结构（对真实发射物）：登记表按代发布（ownNextReg 不 free 旧代、ownAdd 持锁 + 原子 RELEASE
发布 + regSeq +1、ownFind 无锁读 + 两次 seq 比对 + 重试上限 + 持锁兜底）、ownHot 线程本地、
wb 声明是**普通 int**（_Atomic 会把热路径的读变成编译器屏障，chase 实测 +8.8% Ir），原子加/减包进 noinline 冷函数 extc_rc_wbAdd（三处调用：begin +1、finish/abort -1），三处取 slab 的入口都走「快速探测 + ownFind 兜底」。
运行期：ASan 编译的并行形状 8 路并行跑 512 次，任何一次非 0 退出（SEGV / underflow trap）
都算红。
--selftest 对发射物做 10 条注入（跳过原子发布、free 旧代、去掉重试上限、ownHot 去掉 TLS、
wb 声明退回 _Atomic、wb 助手体内不用原子、原子加写回热路径、助手不 noinline、热路径不走快速探测、
少一处 ownFind），每条必须让判据变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
SHAPE = ROOT / "tests/reclaim-gaps/shapes/par_tls_cycle.extc"

REG_TYPEDEF = "typedef struct ExtcRcReg {"
NEXT_REG_SIG = "static ExtcRcReg *extc_rc_ownNextReg(ExtcRcReg *cur, int big) {"
FIND_SIG = "static ExtcRcSlab *extc_rc_ownFind(const void *key, int big) {"
PUBLISH = "__atomic_store_n(&extc_rc_ownCur, rg, __ATOMIC_RELEASE);"
HOT_TLS = "static _Thread_local ExtcRcSlab *extc_rc_ownHot;"
WB_DECL = "static int extc_rc_wb;"
WB_HELPER_SIG = "__attribute__((unused, noinline)) static int extc_rc_wbAdd(int d) {"
WB_HELPER_ATOMIC = "return __atomic_fetch_add(&extc_rc_wb, d, __ATOMIC_RELAXED);"
WB_ADD_CALL = "extc_rc_wbAdd(1);"
WB_SUB_CALL = "extc_rc_wbAdd(-1);"
FIND_BASE = "extc_rc_ownFind(base, 0)"
PROBE_SIG = "static inline ExtcRcSlab *extc_rc_ownProbeFast(const void *key, int big) {"
PROBE_FALLBACK = "extc_rc_ownFind(key, 0)"
OWNBIG_SEQ_RETRY = "if (!s && __atomic_load_n(&extc_rc_regSeq, __ATOMIC_ACQUIRE) != s0) s = extc_rc_ownFind(p, 1);"
SHADE_SLOW_SIG = "__attribute__((unused, noinline)) static void extc_rc_shadeSlow(void *v) {"


def block_end(text, a):
    depth = 0
    i = a
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def body_of(text, sig):
    i = text.find(sig)
    if i < 0:
        return None
    a = text.index("{", i)
    b = block_end(text, a)
    return None if b < 0 else text[a:b + 1]


def emit(src_text, work):
    src = work / "par_tls.extc"
    out = work / "par_tls.c"
    src.write_text(src_text)
    env = dict(os.environ)
    env["EXTC_RECLAIM"] = "1"
    r = subprocess.run([str(EXTC), "-w", "--no-line-map", str(src), "-o", str(out)],
                       cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("emit failed (exit %d): %s"
                           % (r.returncode, (r.stderr or r.stdout).strip()[:400]))
    return out.read_text()


def check(text):
    bad = []
    if REG_TYPEDEF not in text:
        bad.append("登记表不是按代的 ExtcRcReg（并发发布结构缺失）")
    nr = body_of(text, NEXT_REG_SIG)
    if nr is None:
        bad.append("找不到 extc_rc_ownNextReg")
    else:
        if "free(" in nr:
            bad.append("ownNextReg 释放了旧代（并行读者可能还拿着它探测 ⇒ UAF）")
        if "return nr;" not in nr:
            bad.append("ownNextReg 没有返回新代")
    pub = body_of(text, "static void extc_rc_ownAdd(ExtcRcSlab *s) {")
    if pub is None:
        bad.append("找不到 extc_rc_ownAdd")
    else:
        if "extc_rc_regLockAcq();" not in pub or "extc_rc_regUnlock();" not in pub:
            bad.append("ownAdd 不在登记锁内写表")
        if PUBLISH not in pub:
            bad.append("ownAdd 没有原子 RELEASE 发布新代")
        if "__atomic_fetch_add(&extc_rc_regSeq, 1, __ATOMIC_RELEASE);" not in pub:
            bad.append("ownAdd 不递增 extc_rc_regSeq（读者无法判重试）")
    fnd = body_of(text, FIND_SIG)
    if fnd is None:
        bad.append("找不到 extc_rc_ownFind（无锁读者）")
    else:
        if fnd.count("__atomic_load_n(&extc_rc_regSeq, __ATOMIC_ACQUIRE)") < 2:
            bad.append("ownFind 没有在探测落空后复读 extc_rc_regSeq")
        if "__atomic_load_n(big ? &extc_rc_bigCur : &extc_rc_ownCur, __ATOMIC_ACQUIRE)" not in fnd:
            bad.append("ownFind 没有原子读发布指针")
        if "tries >= 32" not in fnd:
            bad.append("ownFind 没有重试上限（读者可能永远重试）")
        if "extc_rc_regLockAcq();" not in fnd:
            bad.append("ownFind 没有持锁兜底")
    if HOT_TLS not in text:
        bad.append("extc_rc_ownHot 不是线程本地（跨线程共享会越过区域边界）")
    if '_Atomic int extc_rc_wb' in text:
        bad.append("extc_rc_wb 是 _Atomic 类型（热路径的读变成编译器屏障，chase 实测 +8.8% Ir）")
    if 'extc_rc_ownProbeFast(base, 0);' not in text:
        bad.append("热路径没有先走 extc_rc_ownProbeFast（登记表探测每分配多 +6.3M Ir）")
    if WB_DECL not in text:
        bad.append("extc_rc_wb 声明丢了（普通 int 声明 + 原子写侧，见注释）")
    wbh = body_of(text, WB_HELPER_SIG)
    if wbh is None:
        bad.append("找不到 noinline 的 extc_rc_wbAdd（原子加/减必须包进冷函数，屏障渗进热路径 = owed_home +1.76% Ir）")
    elif WB_HELPER_ATOMIC not in wbh:
        bad.append("extc_rc_wbAdd 体内不是原子加/减（多线程 begin/finish 会丢更新 ⇒ underflow）")
    if text.count(WB_ADD_CALL) != 1:
        bad.append("extc_begin 的 +1 没有走 extc_rc_wbAdd(1)")
    if text.count(WB_SUB_CALL) != 2:
        bad.append("finish/abort 的 -1 没有都走 extc_rc_wbAdd(-1)")
    if "__atomic_fetch_add(&extc_rc_wb, 1, __ATOMIC_RELAXED);" in text:
        bad.append("原子加还写在热路径里（没有包进 noinline 冷函数，热路径的读变成编译器屏障，owed_home +1.76% Ir）")
    if "extc_rc_wb++" in text or "extc_rc_wb--" in text:
        bad.append("还有普通的 extc_rc_wb++/--（丢更新）")
    pr = body_of(text, PROBE_SIG)
    if pr is None:
        bad.append("找不到 extc_rc_ownProbeFast（热路径快速探测）")
    else:
        if PROBE_FALLBACK not in pr:
            bad.append("快速探测落空后没有回落到 ownFind 的 seq 重试协议")
        if pr.count("__atomic_load_n(&extc_rc_regSeq, __ATOMIC_ACQUIRE)") < 2:
            bad.append("快速探测没有在探测前后各读一次 extc_rc_regSeq（分不清权威落空与写侧刚动过）")
    nprobe = text.count("extc_rc_ownProbeFast(base, 0);")
    if nprobe < 3:
        bad.append("取 slab 的热路径入口没有全部走快速探测（找到 %d 处，至少要 3 处）" % nprobe)
    ob = body_of(text, "static ExtcRcSlab *extc_rc_ownBig(void *p) {")
    if ob is None:
        bad.append("找不到 extc_rc_ownBig")
    elif OWNBIG_SEQ_RETRY not in ob:
        bad.append("超大块探测落空后没有按 regSeq 回落 ownFind（会把刚登记的超大块当不在表里）")
    if SHADE_SLOW_SIG not in text:
        bad.append("extc_rc_shadeSlow 不是 noinline（冷半段内联决策会随运行期代码变大而翻转，chase 实测能抖 1.6% Ir）")
    return bad


def compile_asan(c_text, work):
    c = work / "par_tls.c"
    c.write_text(c_text)
    exe = work / "par_tls_asan"
    r = subprocess.run(["cc", "-std=c11", "-fwrapv", "-O1", "-g",
                        "-fsanitize=address,undefined", str(c), "-o", str(exe)],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        raise RuntimeError("cc -fsanitize failed: %s" % r.stderr.strip()[:400])
    return exe


def smoke(exe, threads=8, iters=64, n="640000"):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
    env["UBSAN_OPTIONS"] = "print_stacktrace=0"
    failures = []
    lock = threading.Lock()

    def loop():
        for _ in range(iters):
            r = subprocess.run([str(exe), n], capture_output=True, text=True, env=env, timeout=120)
            if r.returncode != 0:
                with lock:
                    failures.append((r.returncode, (r.stderr or "")[:300]))
                return

    ts = [threading.Thread(target=loop) for _ in range(threads)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return failures


SELFTEST = [
    ("publish-plain", PUBLISH, "extc_rc_ownCur = rg;"),
    ("free-old-gen", "nr->tab = nt; nr->cap = nc; nr->n = cur->n;",
     "free(cur->tab);\n    nr->tab = nt; nr->cap = nc; nr->n = cur->n;"),
    ("no-retry-cap", "if (tries >= 32) break;", "break;"),
    ("hot-not-tls", HOT_TLS, "static ExtcRcSlab *extc_rc_ownHot;"),
    ("wb-atomic-type", WB_DECL, "static _Atomic int extc_rc_wb;"),
    ("no-fast-probe", "extc_rc_ownProbeFast(base, 0);", "extc_rc_ownFind(base, 0);"),
    ("wb-plain-add", WB_HELPER_ATOMIC, "return extc_rc_wb + d;"),
    ("wb-inline-atomic", WB_ADD_CALL, "__atomic_fetch_add(&extc_rc_wb, 1, __ATOMIC_RELAXED);"),
    ("wb-helper-inline", WB_HELPER_SIG, "__attribute__((unused)) static int extc_rc_wbAdd(int d) {"),
    ("find-plain-probe", PROBE_FALLBACK + ";", "(ExtcRcSlab *)0;"),
    ("ownbig-no-seq", OWNBIG_SEQ_RETRY, "if (!s) s = (ExtcRcSlab *)0;"),
    ("shadeslow-inline", SHADE_SLOW_SIG, "__attribute__((unused)) static void extc_rc_shadeSlow(void *v) {"),
]


def selftest(text):
    bad = []
    for name, old, new in SELFTEST:
        if old not in text:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        txt = text.replace(old, new) if name == "no-fast-probe" else text.replace(old, new, 1)
        if not check(txt):
            bad.append("injection %s does not turn the check red (check is a no-op)" % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if not EXTC.exists():
        print("FAIL compiler not found: %s (run make first)" % EXTC)
        return 1
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        try:
            text = emit(SHAPE.read_text(), work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
        bad = check(text)
        if args.selftest:
            bad += selftest(text)
        if bad:
            print("FAIL reclaim concurrency structure:")
            for b in bad:
                print("  - " + b)
            return 1
        try:
            exe = compile_asan(text, work)
            fails = smoke(exe)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
        if fails:
            print("FAIL reclaim concurrency smoke（8 路并行 512 次，%d 次崩溃）:" % len(fails))
            for code, err in fails[:3]:
                print("  - exit %d: %s" % (code, err.replace("\n", " ")[:200]))
            return 1
    print("ok reclaim concurrency（按代发布 + 线程本地 ownHot + 原子 wb；8 路并行 512 次干净）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
