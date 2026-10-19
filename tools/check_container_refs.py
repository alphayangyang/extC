#!/usr/bin/env python3
"""tools/check_container_refs.py --- 守住「容器描述符里的引用只看字段，元素引用交给写入屏障」。

## 为什么需要这个门禁

池/板容器（pool<T> / vector<T>）的元素住在**容器自己的板**上，不在 C 描述符的字段里：描述符
字段是三块视图 + 几个整数。rcScanType 早先对「泛型实参带引用」一律返回 RC_CONSERVATIVE，
于是 pool<mut ref node> 这种局部量让整个函数 rcRootsUnreliable ⇒ 一个触发都不发、欠收也没人
还（pool_ref_slot 峰值比 3.04×、sweeps 0/0）。

现在的规则：先扫字段；字段里一个引用叶子都没有（s == RC_NO_REFS）而泛型实参带引用，说明引用
只在板的元素里 —— 描述符本身没有引用槽，根表不欠这笔账，触发照常发；元素里的引用由写入点的
屏障负责（storeRefSlot 查得到 rc 槽就按槽记账，查不到就把值钉成根，见 [rt:pin]）。字段里**也**
看得见引用（内联数组、按值内嵌的引用结构体）时仍然走保守路（rcRootsUnreliable + 整值保守存储）
—— 这是安全方向的窄化，不是全面放开。

判据跑真实发射：pool_ref_slot 的 main 必须有 maybeCollect、不得对 p 发整值保守存储；一个
wrap<box>（泛型实参 box 含 mut ref 字段）的 main 必须发整值保守存储且一个触发都不发（窄化没
被放开）。再断 src/back/codegen.c 里那条规则的形状。--selftest 做 6 条注入，每条必须变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
CONTAINER = ROOT / "tests/reclaim-gaps/shapes/pool_ref_slot.extc"
CODEGEN = ROOT / "src/back/codegen.c"

NARROW = '''use std::io

struct node {
    v: i64
    next: ?mut ref node
}

struct box {
    p: mut ref node
}

struct wrap<T> {
    v: T
}

fn mk(v: i64) -> mut ref node {
    var l: mut ref node = new node
    l.v = v
    l.next = l
    return l
}

fn main() -> i32 {
    var a: mut ref node = mk(1)
    var b: box = { p: a }
    var w: wrap<box> = { v: b }
    var s: i64 = a.v
    var i: i64 = 0
    while i < 64 {
        a = mk(i)
        s = s + a.v
        i = i + 1
    }
    io::cout << "w=" << w.v.p.v << " s=" << s << "\\n"
    return 0
}
'''

COLLECT = "extc_rc_maybeCollect("
CONS = "extc_rc_storeStructCons("
ALLOC = "extc_rc_alloc("
MAIN_ARGS = "int main(int argc, char **argv) {"
MAIN_VOID = "int main(void) {"
POOL_CONS = "storeStructCons((void *)&(p)"
WRAP_CONS = "storeStructCons((void *)&(w)"
RULE = "if (targRefs && s != RC_NO_REFS) return RC_CONSERVATIVE;"
TARGSCAN = "bool targRefs = false;"
FIELDSCAN = "RcScan s = rcScanFields(g, &t->sdef->fields, depth);"
BLANKET = ("            if (rcScanType(g, *(Type **)vecAt(&t->targs, i), depth + 1) != RC_NO_REFS)\n"
           "                return RC_CONSERVATIVE;")


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


def body_of(text):
    """切出 main 的函数体（按花括号配平）；带参数与不带参数两种签名都认。"""
    i = max(text.find(MAIN_ARGS), text.find(MAIN_VOID))
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


def check(cont, narrow, codegen):
    bad = []
    cm = body_of(cont)
    nm = body_of(narrow)
    if cm is None:
        bad.append("container: main() not found in emission")
    else:
        if COLLECT not in cm:
            bad.append("container: main() has no sweep trigger (the generic descriptor still"
                       " shuts rcRootsUnreliable off)")
        if POOL_CONS in cm:
            bad.append("container: main() still emits the conservative whole-value store for the"
                       " pool descriptor")
    if ALLOC not in narrow:
        bad.append("narrow: control program is vacuous (no rc allocation at all)")
    if nm is None:
        bad.append("narrow: main() not found in emission")
    else:

        if WRAP_CONS not in nm:
            bad.append("narrow: wrap<box> local lost its conservative whole-value store"
                       " (the rule was widened, not narrowed)")
        if COLLECT in nm:
            bad.append("narrow: main() now emits a trigger although a ref-carrying generic"
                       " struct still hides leaves outside the root table")
    src = codegen.read_text() if codegen.exists() else ""
    if TARGSCAN not in src:
        bad.append("src/back/codegen.c: generic case does not compute targRefs")
    if FIELDSCAN not in src:
        bad.append("src/back/codegen.c: generic case does not scan the descriptor fields")
    if RULE not in src:
        bad.append("src/back/codegen.c: the targRefs-and-fields narrowing rule is gone")
    if BLANKET in src:
        bad.append("src/back/codegen.c: generic case is back to the blanket conservative scan")
    return bad


SELFTEST = [
    ("del-trigger", "cont", COLLECT, "extc_rc_maybeCollectX("),
    ("add-cons", "cont", "    int32_t __extc_ret_v;",
     "    extc_rc_storeStructCons((void *)&(p), sizeof(p), (void *)0, 0);\n    int32_t __extc_ret_v;"),
    ("del-cons", "narrow", "extc_rc_storeStructCons((void *)&(w), sizeof(wrap_box), (void *)0, 0);", ""),
    ("add-trigger", "narrow", "    int32_t __extc_ret_v;",
     "    if (1) extc_rc_maybeCollect((ExtcRcRegion *)0, (void *const *)0, 0);\n    int32_t __extc_ret_v;"),
    ("src-old-blanket", "src", RULE, "return s;"),
    ("src-over-relax", "src", RULE, "if (0) return RC_CONSERVATIVE;"),
]


def selftest(cont, narrow, codegen):
    bad = []
    src = codegen.read_text() if codegen.exists() else ""
    for name, which, old, new in SELFTEST:
        base = {"cont": cont, "narrow": narrow, "src": src}[which]
        if old not in base:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        mutated = base.replace(old, new)
        if which == "cont":
            got = check(mutated, narrow, codegen)
        elif which == "narrow":
            got = check(cont, mutated, codegen)
        else:
            tmp = Path(tempfile.mkdtemp()) / "codegen.c"
            tmp.write_text(mutated)
            got = check(cont, narrow, tmp)
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
            cont = emit(CONTAINER.read_text(), "cont", work)
            narrow = emit(NARROW, "narrow", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(cont, narrow, CODEGEN)
    if args.selftest:
        bad += selftest(cont, narrow, CODEGEN)
    if bad:
        print("FAIL container descriptor refs structure:")
        for b in bad:
            print("  - " + b)
        return 1
    print("ok container descriptor refs（容器 main 有触发、无整值保守存储；窄化控制无触发）")
    return 0


if __name__ == "__main__":
    sys.exit(main())