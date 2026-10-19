#!/usr/bin/env python3
"""tools/check_home_delegation.py --- 守住「拥有权委托」的两半：调用点发 flag、被调者按 flag 三态触发。

## 为什么需要这个门禁

家 arena 是调用者一路传下来的同一只。内核帧（rcFrameOwnsArena 为假）的静态根表看不见调用帧的
活引用，所以内核帧不扫、只记欠收（rcEmitOwe）。可拥有者如果**此后再也不分配**（var keep =
build(per) 之后 main 没有分配点），那笔欠账就一辈子没人还 —— home_cycle_r1 峰值比 3.04×。

修法是把「清扫权」当场委托给被调者：拥有者此刻没有任何活引用槽（根表为空）、调用点又是整条语句
的顶层表达式时，被调者的根表（形参 + 局部 + 全局）就是完整的种子，可以立刻用自己的根表扫一次。
两半必须成对：homeOwnArg 发 flag（0 / 1 / 转发 __extc_homeOwn），rcEmitTrigger 的
RC_OWN_NO 分支按 flag 三态（owner 分支物化根表并 maybeCollect，else 分支照旧 owe）。

判据跑真实发射：home_cycle 的 main 必须「顶层、无根」那一次发 1、「loop 里 keep 是根」那一次发 0；
build 体内必须 if (__extc_homeOwn) { ... maybeCollect ... } else { ... owe ... }；一个没有家 arena
形参的程序里不得出现这个 flag（零税）；再断 src/back/codegen.c 里那条规则的形状。
--selftest 做 9 条注入，每条必须变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
HOME = ROOT / "tests/reclaim-gaps/shapes/home_cycle.extc"
CODEGEN = ROOT / "src/back/codegen.c"

PLAIN = """use std::io

struct node {
    v: i64
    next: ?mut ref node
}

fn main() -> i32 {
    var keep: mut ref node = new node
    keep.v = 0
    keep.next = keep
    var i: i64 = 1
    while i < 1025 {
        var p: mut ref node = new node
        p.v = i
        p.next = p
        keep = p
        i = i + 1
    }
    io::cout << "last=" << keep.v << "\\n"
    return 0
}
"""

FLAG_PARAM = "EXTC_UNUSED int32_t __extc_homeOwn)"
CALL_ONE = "build(per, &__extc_a[1], 1)"
CALL_ZERO = "build(per, &__extc_a[1], 0)"
FLAG_IF = "if (__extc_homeOwn) {"
OWNER_BRANCH = "extc_rc_maybeCollect("
OWE = "extc_rc_owe("
ALLOC = "extc_rc_alloc("
BUILD_SIG = "static node * build("

TOP_GUARD = 'if (!site || site != g->topExpr) return "0";'
FORWARD = 'return "__extc_homeOwn";'
TRAMP_ZERO = 'if (g->needReclaim) bufPuts(&args, ", 0");'
GEN_TOP = "genTopExpr(g, "
TRIGGER_FLAG = 'pfLine(g, "if (__extc_homeOwn) {");'


def emit(src_text, tag, work):
    src = work / (tag + ".extc")
    out = work / (tag + ".c")
    src.write_text(src_text)
    env = dict(os.environ)
    env["EXTC_RECLAIM"] = "1"
    r = subprocess.run([str(EXTC), "-w", "--no-line-map", str(src), "-o", str(out)],
                       cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        raise RuntimeError("emit %s failed (exit %d): %s"
                           % (tag, r.returncode, (r.stderr or r.stdout).strip()[:400]))
    if not out.exists():
        raise RuntimeError("emit %s wrote no output" % tag)
    return out.read_text()


def fn_body(text, sig):
    """切出一个函数的函数体（按花括号配平）；找不到返回 None。

    用 rfind：前置声明也匹配同一个签名，取最后一次出现才是定义。
    """
    i = text.rfind(sig)
    if i < 0:
        return None
    j = text.index("{", i)
    depth = 0
    k = j
    while k < len(text):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
        k += 1
    return None


def _body_from(text, i):
    j = text.index("{", i)
    depth = 0
    k = j
    while k < len(text):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
        k += 1
    return None


def main_body(text):
    i = max(text.find("int main(int argc, char **argv) {"), text.find("int main(void) {"))
    if i < 0:
        return None
    return _body_from(text, i)


def if_true_branch(body, needle):
    """切出 needle 那个 if 的真分支，找不到返回 None。"""
    i = body.find(needle)
    if i < 0:
        return None
    j = body.index("{", i + len(needle))
    depth = 0
    k = j
    while k < len(body):
        if body[k] == "{":
            depth += 1
        elif body[k] == "}":
            depth -= 1
            if depth == 0:
                return body[j:k + 1]
        k += 1
    return None


def check(home, plain, codegen):
    bad = []
    hm = main_body(home)
    if hm is None:
        bad.append("home_cycle: main() not found in emission")
    else:
        if CALL_ONE not in hm:
            bad.append("home_cycle: the top-level call site (no live ref slots) does not pass"
                       " the delegation flag 1")
        if CALL_ZERO not in hm:
            bad.append("home_cycle: the call site that runs while keep is a live root does"
                       " not pass 0 (it must keep owing, not delegate)")
    bh = fn_body(home, BUILD_SIG)
    if bh is None:
        bad.append("home_cycle: build() body not found in emission")
    else:
        if FLAG_IF not in bh:
            bad.append("home_cycle: build() does not branch on the delegation flag")
        else:
            owner = if_true_branch(bh, FLAG_IF)
            if owner is None or OWNER_BRANCH not in owner:
                bad.append("home_cycle: the owner branch of the delegation flag does not"
                           " materialise the root table and sweep (maybeCollect)")
        if OWE not in bh:
            bad.append("home_cycle: the else branch does not record the owed sweep")
    if home.count(FLAG_PARAM) != 2:
        bad.append("home_cycle: the hidden delegation parameter is missing from the prototype"
                   " or the definition (found %d)" % home.count(FLAG_PARAM))
    if ALLOC not in plain:
        bad.append("plain: control program is vacuous (no rc allocation at all)")
    if "__extc_homeOwn" in plain:
        bad.append("plain: a program with no home-arena parameter pays the delegation flag"
                   " (zero-tax broken)")
    src = codegen.read_text() if codegen.exists() else ""
    if TOP_GUARD not in src:
        bad.append("src/back/codegen.c: homeOwnArg does not require the call to be the"
                   " statement top-level expression")
    if FORWARD not in src:
        bad.append("src/back/codegen.c: homeOwnArg does not forward the received flag")
    if TRAMP_ZERO not in src:
        bad.append("src/back/codegen.c: the parallel worker trampoline does not pass 0")
    if GEN_TOP not in src or src.count(GEN_TOP) < 5:
        bad.append("src/back/codegen.c: fewer than 5 statement top-expression sites go through"
                   " genTopExpr")
    if TRIGGER_FLAG not in src:
        bad.append("src/back/codegen.c: the RC_OWN_NO trigger does not emit the delegation"
                   " three-state")
    return bad


SELFTEST = [
    ("del-flag-param", "home", FLAG_PARAM, "int32_t)"),
    ("flag-always-one", "home", CALL_ZERO, CALL_ONE),
    ("no-owner-branch", "home", FLAG_IF, "if (0) {"),
    ("no-else-owe", "home", OWE, "extc_rc_oweX("),
    ("plain-tax", "plain", "    int32_t __extc_ret_v;",
     "    if (__extc_homeOwn) { }\\n    int32_t __extc_ret_v;"),
    ("src-no-top-guard", "src", TOP_GUARD, 'if (0) return "0";'),
    ("src-no-forward", "src", FORWARD, 'return "0";'),
    ("src-no-tramp-zero", "src", TRAMP_ZERO, ""),
    ("src-no-trigger-flag", "src", TRIGGER_FLAG, 'pfLine(g, "if (0) {");'),
]


def selftest(home, plain, codegen):
    bad = []
    src = codegen.read_text() if codegen.exists() else ""
    for name, which, old, new in SELFTEST:
        base = {"home": home, "plain": plain, "src": src}[which]
        if old not in base:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        mutated = base.replace(old, new)
        if which == "home":
            got = check(mutated, plain, codegen)
        elif which == "plain":
            got = check(home, mutated, codegen)
        else:
            tmp = Path(tempfile.mkdtemp()) / "codegen.c"
            tmp.write_text(mutated)
            got = check(home, plain, tmp)
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
            home = emit(HOME.read_text(), "home", work)
            plain = emit(PLAIN, "plain", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(home, plain, CODEGEN)
    if args.selftest:
        bad += selftest(home, plain, CODEGEN)
    if bad:
        print("FAIL home arena delegation structure:")
        for b in bad:
            print("  - " + b)
        return 1
    print("ok home delegation（顶层无根调用发 1、有根调用发 0；被调者按 flag 三态触发；无家 arena 的程序零税）")
    return 0


if __name__ == "__main__":
    sys.exit(main())