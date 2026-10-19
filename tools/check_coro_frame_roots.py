#!/usr/bin/env python3
"""tools/check_coro_frame_roots.py --- 守住「协程 step 的帧字段进根表、块 arena 归帧自己」。

## 为什么需要这个门禁

协程帧（churn$frame）由调用者的复合字面量建、跨 yield 活下来，step 的 C 形参只有那个帧指针。
把 coroFunc 整体当成 rcRootsUnreliable ⇒ step 一个触发都不发，帧里那只块 arena
（f->arena1）的函数级层只能等帧销毁才还。现在两头都改了：

1. genFunc 按 checker 排好的 coroFrame 表，把各字段的引用叶子登记成根（f->name）；帧起点零
   初始化、根表入口过 extc_rc_owns ⇒ 认错只多留；yield 不出带引用的值由 checker 保证；
2. rcFrameOwnsArena 不再挡 coroFunc：step 的块 arena（lvl >= 1）归帧自己；rcOwnKind 也不再
   整函数返回 RC_OWN_NO（但 parTls 一档仍要挡 coroFunc —— step 不发 __extc_tlsOwner 序言）。

判据跑真实发射：coro_frame_cycle 的 step 必须把 f->keep / f->p 放进某个 maybeCollect 的根数组、
触发装在 f->arena1 的块级区域上（lvl 1，不是家 arena -1），且不发 TLS 归属序言；一个不带
协程的普通程序一个帧字段根都不许有（精度）。--selftest 对发射物与生成器源码做 9 条注入，
每条必须让判据变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
SHAPE = ROOT / "tests/reclaim-gaps/shapes/coro_frame_cycle.extc"
CODEGEN = ROOT / "src/back/codegen.c"

PLAIN = """use std::io

struct node {
    v: i64
    next: ?mut ref node
}

fn mk(v: i64) -> mut ref node {
    var l: mut ref node = new node
    l.v = v
    l.next = l
    return l
}

fn main() -> i32 {
    var a: mut ref node = mk(1)
    var s: i64 = a.v
    var i: i64 = 0
    while i < 64 {
        a = mk(i)
        s = s + a.v
        i = i + 1
    }
    io::cout << "s=" << s << "\\n"
    return 0
}
"""

BLOCK_REGION = "extc_rc_regionOf((void *)&(f->arena1), 1)"
HOME_REGION = "extc_rc_regionOf((void *)&(f->arena1), -1)"
KEEP_ROOT = "(void *)(f->keep)"
P_ROOT = "(void *)(f->p)"
COLLECT = "extc_rc_maybeCollect("
TLS_OWNER = "__extc_tlsOwner"
ROC = "g->rcRootsUnreliable = g->needReclaim && g->rcGlobalsUnreliable;"
ROC_OLD = "g->rcRootsUnreliable = g->needReclaim && (g->coroFunc != NULL || g->rcGlobalsUnreliable);"
CORO_LOOP = "for (size_t i = 0; i < cf->coroFrame.len; i++) {"
FRAME_OWNS = "if (g->parTls) return false;"
FRAME_OWNS_OLD = "if (g->parTls || g->coroFunc) return false;"
CORO_NO = "if (g->coroFunc) return RC_OWN_NO;"
RC_OWN_TLS = "    if (g->parTls) return (g->noArena || g->coroFunc) ? RC_OWN_NO : RC_OWN_TLS;"


def emit(src_text, tag, work):
    src = work / (tag + ".extc")
    out = work / (tag + ".c")
    src.write_text(src_text)
    env = dict(os.environ)
    env["EXTC_RECLAIM"] = "1"
    r = subprocess.run([str(EXTC), "-w", "--no-line-map", str(src), "-o", str(out)],
                       cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("emit %s failed (exit %d): %s"
                           % (tag, r.returncode, (r.stderr or r.stdout).strip()[:400]))
    if not out.exists():
        raise RuntimeError("emit %s wrote no output" % tag)
    return out.read_text()


def call_spans(text, head):
    """切出 head(...) 的每个实参括号区间（按括号配平）。"""
    spans = []
    pos = 0
    while True:
        i = text.find(head, pos)
        if i < 0:
            return spans
        a = text.index("(", i)
        depth = 0
        j = a
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append(text[a:j + 1])
        pos = j + 1


def check(coro, plain, codegen):
    bad = []
    spans = call_spans(coro, COLLECT)
    rooted = [s for s in spans if KEEP_ROOT in s and P_ROOT in s]
    if not rooted:
        bad.append("coro: no maybeCollect root array carries both f->keep and f->p (%d call sites)"
                   % len(spans))
    if coro.count(BLOCK_REGION) < 2:
        bad.append("coro: block arena (f->arena1, lvl 1) referenced %d times (want >= 2: alloc + trigger)"
                   % coro.count(BLOCK_REGION))
    if HOME_REGION in coro:
        bad.append("coro: trigger site runs on the home arena (lvl -1), which the frame does not own")
    if TLS_OWNER in coro:
        bad.append("coro: step emits the TLS owner prologue (coro step is not a parTls frame)")
    if "(void *)(f->" in plain:
        bad.append("plain: non-coroutine program emits coroutine frame-field roots")
    src = codegen.read_text() if codegen.exists() else ""
    if ROC not in src:
        bad.append("src/back/codegen.c: genFunc still marks coroutine step root tables unreliable")
    if ROC_OLD in src:
        bad.append("src/back/codegen.c: coroutine steps still mark the root table unreliable")
    if CORO_LOOP not in src:
        bad.append("src/back/codegen.c: coroutine frame-field root loop is gone")
    if FRAME_OWNS not in src:
        bad.append("src/back/codegen.c: block arenas are not owned by the frame (rcFrameOwnsArena)")
    if FRAME_OWNS_OLD in src:
        bad.append("src/back/codegen.c: rcFrameOwnsArena still excludes coroutine frames")
    if CORO_NO in src:
        bad.append("src/back/codegen.c: coroutine frames still return RC_OWN_NO unconditionally")
    return bad


SELFTEST = [
    ("del-f-p-root", "coro", ", " + P_ROOT, ""),
    ("zero-keep-root", "coro", KEEP_ROOT, "(void *)0"),
    ("home-arena-trigger", "coro", BLOCK_REGION + "; if (extc_rc_shouldSweep",
     HOME_REGION + "; if (extc_rc_shouldSweep"),
    ("tls-owner-coro", "coro", "EXTC_UNUSED static bool churn$step(struct churn$frame *f) {",
     "EXTC_UNUSED static bool churn$step(struct churn$frame *f) {\n    EXTC_UNUSED int __extc_tlsOwner = 1;"),
    ("plain-frame-root", "plain", "    return __extc_ret_v;",
     "    (void)(void *)(f->x);\n    return __extc_ret_v;"),
    ("src-coro-unreliable", "src", ROC, ROC_OLD),
    ("src-coro-no-own", "src", FRAME_OWNS, FRAME_OWNS_OLD),
    ("src-no-coro-loop", "src", CORO_LOOP, "for (size_t i = 0; i < (size_t)0; i++) {"),
    ("src-coro-own-no", "src", RC_OWN_TLS, "    if (g->coroFunc) return RC_OWN_NO;\n" + RC_OWN_TLS),
]


def selftest(coro, plain, codegen):
    bad = []
    src = codegen.read_text() if codegen.exists() else ""
    for name, which, old, new in SELFTEST:
        base = {"coro": coro, "plain": plain, "src": src}[which]
        if old not in base:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        mutated = base.replace(old, new)   # 全量替换：根数组那两条注入要同时打掉两处触发点
        if which == "coro":
            got = check(mutated, plain, codegen)
        elif which == "plain":
            got = check(coro, mutated, codegen)
        else:
            tmp = Path(tempfile.mkdtemp()) / "codegen.c"
            tmp.write_text(mutated)
            got = check(coro, plain, tmp)
        if not got:
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
            coro = emit(SHAPE.read_text(), "coro", work)
            plain = emit(PLAIN, "plain", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(coro, plain, CODEGEN)
    if args.selftest:
        bad += selftest(coro, plain, CODEGEN)
    if bad:
        print("FAIL coro frame roots structure:")
        for b in bad:
            print("  - " + b)
        return 1
    n = len([s for s in call_spans(coro, COLLECT) if KEEP_ROOT in s and P_ROOT in s])
    print("ok coro frame roots（帧字段根 %d 处、块 arena 触发 %d 处）"
          % (n, coro.count(BLOCK_REGION) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
