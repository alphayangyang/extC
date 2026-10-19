#!/usr/bin/env python3
"""tools/check_par_tls_owner.py --- 守住「线程本地 arena 的归属在运行期判」这几条结构事实。

## 为什么需要这个门禁

parallel::run 的每个 worker 有自己的一只线程本地 arena（extc_tls_arena），整条 worker 调用链
（含它调用的所有函数）都往这只 arena 里分配。回收的触发点要用「本帧静态根表」当种子，而
**只有 worker 入口帧的调用者只有 C 运行期** —— 它的静态根表就是完整种子；内层帧的根表看不见
调用帧的活引用，在那只区域上照常扫就是把活对象当垃圾放。静态判据分不出来（同一个函数既能是
入口帧也能是内层帧），所以归属只能在运行期按**深度**判：

1. 每个 parTls 帧的序言发 EXTC_UNUSED int __extc_tlsOwner = (extc_rc_tlsDepth++ == 0);
   尾声在 __extc_ret: 之后发 extc_rc_tlsDepth--;（所有 return 都走这个标签）；
2. 触发点三态：if (__extc_tlsOwner) { 物化根表 + maybeCollect } else { 只记欠收 owe }；
3. worker 调回调**之前**把深度清零、之后恢复（threads <= 1 或起不出线程时回调在本线程内联
   跑，外层帧的深度还挂在 TLS 上；清零保证入口帧一定是拥有者）；
4. 线程本地区域随 arena 同寿丢弃两处：par_close 的每 worker arena、worker 局部 arena 收尾。

深度只加不配对 = 拥有者判定永久坏掉（要么永远扫、要么永远不扫）；清零/恢复漏掉 = 内联跑时
入口帧被当成内层帧、那只区域永远收不掉；漏掉区域丢弃 = 每轮 run 漏一只区域。这些都只在
parallel + 回收同时在场的程序里显形，9 形状指令数棘轮与单线程电池都看不见，所以钉在这里。

判据跑真实发射：正向并行程序（tests/reclaim-gaps/shapes/par_tls_cycle.extc）必须出现
上面 1/2/3/4；一个不带 parallel 的普通程序 ⇒ 深度变量只许有声明那一处（其余帧零税）。
--selftest 对发射物做 6 条注入，每条必须让判据变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
SHAPE = ROOT / "tests/reclaim-gaps/shapes/par_tls_cycle.extc"
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

DECL = "static _Thread_local int32_t extc_rc_tlsDepth"
PROLOGUE = "EXTC_UNUSED int __extc_tlsOwner = (extc_rc_tlsDepth++ == 0);"
EPILOGUE = "extc_rc_tlsDepth--;"
OWNER_IF = "if (__extc_tlsOwner) {"


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


def owner_branches(text):
    out = []
    pos = 0
    while True:
        i = text.find(OWNER_IF, pos)
        if i < 0:
            return out
        a = text.index("{", i)
        b = block_end(text, a)
        if b < 0:
            out.append((text[a:], None))
            return out
        j = b + 1
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if text.startswith("else", j):
            e = text.index("{", j)
            k = block_end(text, e)
            out.append((text[a:b + 1], text[e:k + 1] if k >= 0 else text[e:]))
            pos = k + 1 if k >= 0 else len(text)
        else:
            out.append((text[a:b + 1], None))
            pos = b + 1


def check(par, plain, codegen):
    bad = []
    for name, text in (("par_tls", par), ("plain", plain)):
        if text.count(DECL) != 1:
            bad.append("%s: depth variable declared %d times (want 1)" % (name, text.count(DECL)))
    depth_lines = [ln for ln in plain.split("\n") if "extc_rc_tlsDepth" in ln]
    if len(depth_lines) != 1 or DECL not in depth_lines[0]:
        bad.append("plain: non-parallel program uses owner depth on %d lines (want 1: the declaration)" % len(depth_lines))
    if "__extc_tlsOwner" in plain:
        bad.append("plain: non-parallel program emits the owner flag")
    if "extc_rc_regionDrop((void *)&local)" in plain:
        bad.append("plain: non-parallel program drops a worker-local region")
    if "extc_rc_regionDrop((void *)&r->arenas" in plain:
        bad.append("plain: non-parallel program drops par_close regions")
    np = par.count(PROLOGUE)
    ne = par.count(EPILOGUE)
    if np < 1:
        bad.append("par_tls: parTls frame prologue does not emit the owner flag")
    if np != ne:
        bad.append("par_tls: %d prologues vs %d epilogues (depth not paired)" % (np, ne))
    br = owner_branches(par)
    owned = [t for t in br if t[1] is not None]
    if len(owned) < 1:
        bad.append("par_tls: trigger site has no three-way dispatch (if (__extc_tlsOwner) has no else)")
    for yes, no in owned:
        if "extc_rc_maybeCollect(" not in yes:
            bad.append("par_tls: owner branch has no collection")
        if "extc_rc_owe(" not in no:
            bad.append("par_tls: inner-frame branch does not record owed-only")
        if "extc_rc_maybeCollect(" in no:
            bad.append("par_tls: inner-frame branch collects with incomplete roots")
    src = codegen.read_text() if codegen.exists() else ""
    if "__extc_tlsOwner && " not in src:
        bad.append("src/back/codegen.c: owed-check runtime ownership condition is missing")
    def at(needle):
        i = par.find(needle)
        return -1 if i < 0 else i
    i_save = at("int32_t __extc_depth = extc_rc_tlsDepth;")
    i_zero = at("extc_rc_tlsDepth = 0;")
    i_call = at("j->fn(")
    i_restore = at("extc_rc_tlsDepth = __extc_depth;")
    if min(i_save, i_zero, i_call, i_restore) < 0:
        bad.append("par_tls: worker depth save/zero/restore lines missing (%d/%d/%d/%d)"
                   % (i_save, i_zero, i_call, i_restore))
    elif not (i_save < i_zero < i_call < i_restore):
        bad.append("par_tls: worker depth save/zero/restore are not wrapped around the callback")
    close_drop = "extc_rc_regionDrop((void *)&r->arenas[i]);"
    local_drop = "extc_rc_regionDrop((void *)&local)"
    if close_drop not in par:
        bad.append("par_tls: par_close does not drop per-worker regions")
    if local_drop not in par:
        bad.append("par_tls: worker-local arena finish does not drop its region")
    i_close = at("EXTC_UNUSED static void extc_par_close(")
    i_cd = at(close_drop)
    i_loop = at("for (int64_t i = 0; i < r->workers; i++) {")
    if min(i_close, i_cd, i_loop) >= 0 and not (i_close < i_loop < i_cd):
        bad.append("par_tls: par_close region drop is not inside the per-worker loop")
    return bad


SELFTEST = [
    ("del-prologue", "par", PROLOGUE + "\n", ""),
    ("del-epilogue", "par", EPILOGUE + "\n", ""),
    ("inner-collect", "par", "extc_rc_owe(__extc_rcr0);", "extc_rc_maybeCollect(__extc_rcr0, (void *const *)0, 0);"),
    ("del-reset", "par", "extc_rc_tlsDepth = 0;\n", ""),
    ("del-close-drop", "par", "        extc_rc_regionDrop((void *)&r->arenas[i]);\n", ""),
    ("tax-plain", "plain", "    return __extc_ret_v;", "    extc_rc_tlsDepth--;\n    return __extc_ret_v;"),
]


def selftest(par, plain, codegen):
    bad = []
    for name, which, old, new in SELFTEST:
        text = par if which == "par" else plain
        if old not in text:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        mutated = text.replace(old, new, 1)
        got = check(mutated, plain, codegen) if which == "par" else check(par, mutated, codegen)
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
            par = emit(SHAPE.read_text(), "par", work)
            plain = emit(PLAIN, "plain", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(par, plain, CODEGEN)
    if args.selftest:
        bad += selftest(par, plain, CODEGEN)
    if bad:
        print("FAIL par_tls owner structure:")
        for b in bad:
            print("  - " + b)
        return 1
    n3 = len([t for t in owner_branches(par) if t[1] is not None])
    print("ok par_tls owner structure（序言/尾声 %d 对、三态 %d 处、区域丢弃 2 处）"
          % (par.count(PROLOGUE), n3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
