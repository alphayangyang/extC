#!/usr/bin/env python3
"""tools/check_dyn_place.py --- 守住「dyn 载荷池生在站点那一层地方」。

## 为什么需要这个门禁

`dyn Trait(x)` 把载荷拷进 dyn 池，池的寿命 = 它被登记到的那个 place（zone）。构造点如果没有
自己的 place，池就登记在函数体那一层 —— 循环里每轮一份载荷攒到函数返回，回收再好也救不了峰值
（dyn_payload：开档 22.23 MB 到 80.17 MB，3.61×；关档同样涨）。

现在的规则：① checker 把构造点定为当前块那一层（`setCallZoneArg`），`exprMakesPool` 认
`EX_DYN` 则含 dyn 的块自己压一层 place；② 值逃逸时按建池调用的同一条规则把站点层提到
`ZONE_HOME`/目的层（`promoteInto2` 的 EX_DYN 分支），深度判定也把站点层算进去
（`dynSiteDepth`）；③ 发射物把 `zoneArgRef` 当第 4 实参传给 `extc_dyn_put`，运行期按这个 zi
建表（不是读 `extc_zoneTop`：提权之后构造在内层、池生在外层）；④ zi 不活则 trap，不静默挂到
当前栈顶。

判据跑真实发射：dyn_payload 的循环体必须自己压/弹 place，`extc_dyn_put` 的第 4 实参必须是那个
块层的 `__extc_zm<k>`；返回 dyn 的函数必须收 `__extc_home_zone` 并把句柄的池登在那儿；不含 dyn
的程序不得有任何 place/dyn 相关的运行期（零常态税）。再断四个 src 位置。
--selftest 做 9 条注入，每条必须变红。
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
DYN = ROOT / "tests/reclaim-gaps/shapes/dyn_payload.extc"
PROMO = ROOT / "tests/dyn/dyn_promoted_return.extc"
TOP = ROOT / "src/check/check_top.c"
ESCAPE = ROOT / "src/check/check_escape.c"
CODEGEN = ROOT / "src/back/codegen.c"
POOLS = ROOT / "src/back/pools.c"

PLAIN = """struct node {
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
    var s: i64 = 0
    var i: i64 = 0
    while i < 64 {
        a = mk(i)
        s = s + a.v
        i = i + 1
    }
    return 0
}
"""

ZONE_ENTER = "extc_pool_zoneEnter()"
ZONE_LEAVE = "extc_pool_zoneLeaveTo("
PUT_DEF = "ExtcDynHandle extc_dyn_put("
CALL_BLOCK = re.compile(r"extc_dyn_put\(\(const void \*\)&[^;]*?, (__extc_zm[0-9]+)\);")
CALL_HOME = re.compile(r"extc_dyn_put\(\(const void \*\)&[^;]*?, __extc_home_zone\);")
HOME_SIG = "static ExtcDynHandle make(int64_t __extc_home_zone) {"
TOP_POOL = "case EX_DYN: return true;"
EMIT_FMT = "&extc_vt$%s$%s, %s)"
PUT_SIG = "ExtcDynHandle extc_dyn_put(const void *payload, int64_t size, const void *vt, int64_t zi) {"
NEW_AT = "extc_pool_new_at(-1, zi);"
DEPTH_SIG = "static int dynSiteDepth(Expr *e, int d) {"


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


def loop_bodies(text):
    """切出所有 while 循环体（按花括号配平）。生成物里运行期文本先于 main、宏里也有 do-while，
    所以不能只取第一个；由调用方挑「含 place 操作」的那一个。"""
    out = []
    pos = 0
    while True:
        i = text.find("while (", pos)
        if i < 0:
            return out
        pos = i + 1
        j = text.find("{", i)
        if j < 0:
            continue
        depth = 0
        k = j
        body = None
        while k < len(text):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    body = text[j:k + 1]
                    break
            k += 1
        if body is not None:
            out.append(body)


def check(dyn, promo, plain, top, escape, codegen, pools):
    bad = []
    loops = loop_bodies(dyn)
    if not loops:
        bad.append("dyn_payload: no while loop in emission")
    else:
        if not any(ZONE_ENTER in b for b in loops):
            bad.append("dyn_payload: no loop body opens its own place (the dyn pool hangs off the function body again)")
        if not any(ZONE_ENTER in b and ZONE_LEAVE in b for b in loops):
            bad.append("dyn_payload: no loop body both opens and closes its own place")
    ncalls = dyn.count(PUT_DEF)
    if ncalls < 1:
        bad.append("dyn_payload: no `extc_dyn_put` in emission (the shape stopped constructing dyn)")
    got = len(CALL_BLOCK.findall(dyn))
    if got != ncalls:
        bad.append("dyn_payload: %d `extc_dyn_put` call(s) but only %d name the block place (`__extc_zm<k>`)" % (ncalls, got))
    pdefs = promo.count(PUT_DEF)
    if pdefs < 1:
        bad.append("dyn_promoted_return: no `extc_dyn_put` in emission")
    pgot = len(CALL_HOME.findall(promo))
    if pgot != pdefs:
        bad.append("dyn_promoted_return: %d `extc_dyn_put` call(s) but only %d name the caller place (`__extc_home_zone`)" % (pdefs, pgot))
    if HOME_SIG not in promo:
        bad.append("dyn_promoted_return: `make` does not take the caller place (`__extc_home_zone`)")
    if ZONE_ENTER in plain:
        bad.append("plain (no dyn): emission opens a place -- dyn/pool runtime is not gated")
    if ZONE_LEAVE in plain:
        bad.append("plain (no dyn): emission closes a place -- dyn/pool runtime is not gated")
    if PUT_DEF in plain:
        bad.append("plain (no dyn): emission has the dyn pool runtime")
    if TOP_POOL not in top:
        bad.append("src/check/check_top.c: exprMakesPool does not count `dyn` as a pool site")
    if DEPTH_SIG not in escape:
        bad.append("src/check/check_escape.c: dynSiteDepth is missing (the site place does not bound the value depth)")
    if escape.count("dynSiteDepth(") < 4:
        bad.append("src/check/check_escape.c: dynSiteDepth is not used on all three depth walks")
    if EMIT_FMT not in codegen:
        bad.append("src/back/codegen.c: `extc_dyn_put` is not emitted with the place argument")
    if PUT_SIG not in pools:
        bad.append("src/back/pools.c: `extc_dyn_put` does not take a place index")
    if NEW_AT not in pools:
        bad.append("src/back/pools.c: the dyn table is not born in the named place")
    return bad


SELFTEST = [
    ("del-loop-enter", "dyn", ZONE_ENTER, "extc_pool_zoneEnterX()"),
    ("del-loop-leave", "dyn", ZONE_LEAVE, "extc_pool_zoneLeaveToX("),
    ("put-to-top", "dyn", "__extc_zm2)", "extc_zoneTop)"),
    ("home-to-top", "promo", "__extc_home_zone)", "extc_zoneTop)"),
    ("plain-tax", "plain", "    return 0;", "    int64_t __extc_zm9 = extc_pool_zoneEnter();\n    return 0;"),
    ("src-not-pool", "top", TOP_POOL, "case EX_DYN: return false;"),
    ("src-put-3args", "codegen", EMIT_FMT, "&extc_vt$%s$%s)"),
    ("src-put-nozi", "pools", PUT_SIG, "ExtcDynHandle extc_dyn_put(const void *payload, int64_t size, const void *vt) {"),
    ("src-new-at-top", "pools", NEW_AT, "extc_pool_new_at(-1, extc_zoneTop);"),
]


def selftest(dyn, promo, plain, top, escape, codegen, pools):
    bad = []
    base = {"dyn": dyn, "promo": promo, "plain": plain, "top": top.read_text(),
            "escape": escape.read_text(), "codegen": codegen.read_text(),
            "pools": pools.read_text()}
    for name, which, old, new in SELFTEST:
        if old not in base[which]:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        args = dict(base)
        args[which] = base[which].replace(old, new)
        got = check(args["dyn"], args["promo"], args["plain"], str(args["top"]),
                    str(args["escape"]), str(args["codegen"]), str(args["pools"]))
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
            dyn = emit(DYN.read_text(), "dyn", work)
            promo = emit(PROMO.read_text(), "promo", work)
            plain = emit(PLAIN, "plain", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(dyn, promo, plain, TOP.read_text(), ESCAPE.read_text(),
                CODEGEN.read_text(), POOLS.read_text())
    if args.selftest:
        bad += selftest(dyn, promo, plain, TOP, ESCAPE, CODEGEN, POOLS)
    if bad:
        print("FAIL dyn place structure:")
        for b in bad:
            print("  - " + b)
        return 1
    print("ok dyn place（循环体自压 place、dyn 池生在该 place、逃逸站点提到家 place；无 dyn 程序零税）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
