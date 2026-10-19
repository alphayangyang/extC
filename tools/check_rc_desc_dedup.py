#!/usr/bin/env python3
"""tools/check_rc_desc_dedup.py --- 守住「rc 类型描述符按布局去重」的结构事实。

## 为什么需要这个门禁

发射侧不许**每个分配点各发一份** `ExtcRcType` 描述符：同一个 `struct node` 只要有 8 个
`new node` 站点，就会有 8 份内容逐字相同的描述符。代价落在分配路径上：每块 slab 的小类型表
（`ttab`）按**指针相等**去重，8 个不同指针就是 8 个表项；`extc_rc_typeIdx` 在表项少时线性
扫，`lastTd` 缓存（"上一次落表的描述符"）也因为指针交替而永远失手。tests/reclaim-shapes/
shard_bands.extc（8 个节点分配点）实测分配路径占该形状总指令数三成以上，去重后总指令数
305,392,672 → 234,792,096（−23.1%）；cyclic_pressure −0.7%、promote −0.4%。

去重是**零运行期成本**的：描述符是纯数据（{指针个数, 偏移表}），同布局共用一份后，
标记/释放/`typeOf` 行为逐字相同（它们只读这张表的内容，从不比较描述符的"身份"）。所以
退化只可能发生在**发射侧**，本工具就守发射侧：

1. 同布局的多个分配点必须引用**同一个** `&__extc_rctN`；
2. 每种布局只发**一份**（偏移表内容互不相同）；
3. 每个分配点引用的描述符都有定义（文件作用域的拼装没被漏掉）；
4. 定义必须落在**文件作用域**（块作用域正是"每点一份"的老形态）。

判据跑真实编译器：写一个 5 个分配点、3 种布局的合成程序，EXTC_RECLAIM=1 发射，然后
解析生成的 C。--selftest 在这个真实发射物上做 3 条注入（每点一份 / 退化成块作用域 /
漏掉拼装），每条必须变红。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))

# 5 个分配点、3 种布局：leaf 两处（mkLeaf + main）、pair 两处（mkPair + main）、box 一处。
# 引用局部量 q2 被重新赋值 ⇒ unitReclaimIdle 判否，回收运行期一定会发射。
PROG = """use std::io

struct leaf { v: i64  next: ?mut ref leaf }
struct pair { a: i64  b: i64  tail: ?mut ref leaf }
struct box { x: i64  y: i64  z: i64  slot: ?mut ref leaf }

fn mkLeaf(v: i64) -> mut ref leaf {
    var l: mut ref leaf = new leaf
    l.v = v
    return l
}

fn mkPair(a: i64) -> mut ref pair {
    var p: mut ref pair = new pair
    p.a = a
    return p
}

fn main() -> i32 {
    var l1: mut ref leaf = mkLeaf(1)
    var l2: mut ref leaf = new leaf
    l2.v = 2
    var p1: mut ref pair = mkPair(3)
    p1.b = l1.v
    var p2: mut ref pair = new pair
    p2.a = l2.v
    p2.tail = l1
    var b1: mut ref box = new box
    b1.slot = l2
    b1.x = p1.a + p2.a
    var sink: i64 = b1.x + p1.b
    var q: ?mut ref leaf = p2.tail
    if q != null { sink = sink + (*q).v }
    var q2: ?mut ref leaf = l2
    q2 = l1
    if q2 != null { sink = sink + (*q2).v }
    io::cout << "sink=" << sink << "\\n"
    return 0
}
"""

ARRAY = re.compile(
    r"^([ \t]*)__attribute__\(\(unused\)\) static const uint32_t (__extc_rco\d+)\[\] = \{ (.*?) \};$",
    re.M)
DESC = re.compile(
    r"^([ \t]*)__attribute__\(\(unused\)\) static const ExtcRcType (__extc_rct\d+) = "
    r"\{ (\d+)u, (__extc_rco\d+) \};$", re.M)
ALLOC_REF = re.compile(
    r"extc_rc_alloc\(__extc_rcr\d+, \(int64_t\)sizeof\(\w+\), &(__extc_rct\d+)\)")


def emit(work: Path):
    """用真实编译器发射一次，返回生成 C 的文本；失败时抛 RuntimeError。"""
    src = work / "prog.extc"
    out = work / "prog.c"
    src.write_text(PROG)
    env = dict(os.environ)
    env["EXTC_RECLAIM"] = "1"
    cmd = [str(EXTC), "-I", str(ROOT / "stdlib"), str(src), "-o", str(out)]
    r = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("编译器退出码 %d：%s" % (r.returncode, (r.stderr or r.stdout).strip()[:400]))
    if not out.exists():
        raise RuntimeError("编译器没有写出生成物")
    return out.read_text(), out


def count_items(body: str) -> int:
    """偏移表里顶层逗号分出的项数（offsetof(a, b) 内部的逗号不算）。"""
    depth = 0
    items = 1
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            items += 1
    return items

def check(text):
    """返回问题列表；空列表 = 结构事实成立。"""
    bad = []
    arrays = {name: (body.strip(), indent) for indent, name, body in ARRAY.findall(text)}
    descs = {name: (int(n), arr, indent) for indent, name, n, arr in DESC.findall(text)}
    refs = ALLOC_REF.findall(text)

    if len(descs) != 3:
        bad.append("描述符份数不是 3（3 种布局）：%d 份 %s"
                   % (len(descs), sorted(descs)))
    if len(arrays) != len(descs):
        bad.append("偏移表份数 %d 与描述符份数 %d 不一致（每个描述符一份偏移表）"
                   % (len(arrays), len(descs)))
    bodies = [body for body, _ in arrays.values()]
    if len(set(bodies)) != len(bodies):
        dup = [b for b in set(bodies) if bodies.count(b) > 1]
        bad.append("同一种布局的偏移表发射了多份：%r（描述符按布局去重，旧实现在每个分配点各发一份）"
                   % dup)
    for name, (n, arr, indent) in descs.items():
        if indent:
            bad.append("%s 不在文件作用域（块作用域 = 每个分配点各发一份的老形态）" % name)
        if arr not in arrays:
            bad.append("%s 引用的偏移表 %s 没有定义" % (name, arr))
        elif count_items(arrays[arr][0]) != n:
            bad.append("%s 的指针个数 %d 与偏移表项数不符：%r" % (name, n, arrays[arr][0]))
    for name, (body, indent) in arrays.items():
        if indent:
            bad.append("偏移表 %s 不在文件作用域" % name)

    if len(refs) != 5:
        bad.append("分配点引用数不是 5（合成程序有 5 个 new 站点）：%d 个 %s"
                   % (len(refs), sorted(refs)))
    counts = sorted(refs.count(s) for s in set(refs))
    if counts != [1, 2, 2]:
        bad.append("每个描述符被引用的分配点数不是 {1,2,2}：%s"
                   "（同布局的分配点必须共用同一份描述符）" % counts)
    missing = sorted(set(refs) - set(descs))
    if missing:
        bad.append("分配点引用了没有定义的描述符：%s（文件作用域的拼装漏了？）" % missing)
    dead = sorted(set(descs) - set(refs))
    if dead:
        bad.append("有描述符没有被任何分配点引用：%s（去重表把死描述符留下来了）" % dead)

    return bad


def selftest(base):
    fails = 0

    def expect_red(name, text):
        nonlocal fails
        bad = check(text)
        if bad:
            print("  ok   红自测「%s」：%s" % (name, bad[0]))
        else:
            print("  FAIL 红自测「%s」：注入后判据没变红" % name)
            fails += 1

    syms = ALLOC_REF.findall(base)
    if len(syms) != 5:
        print("  FAIL 自测：合成发射物里分配点不是 5 个（%d），注入无从下手" % len(syms))
        return fails + 1
    # ① 老形态：把一个分配点的描述符复制一份、让它单独引用。
    first = base.index("__attribute__((unused)) static const uint32_t __extc_rco0[]")
    first_end = base.index("\n", base.index("static const ExtcRcType __extc_rct0", first))
    per_site = base[:first_end + 1] + (
        "__attribute__((unused)) static const uint32_t __extc_rco9[] = { offsetof(leaf, next) };\n"
        "__attribute__((unused)) static const ExtcRcType __extc_rct9 = { 1u, __extc_rco9 };\n"
    ) + base[first_end + 1:]
    per_site = per_site.replace("&__extc_rct0)", "&__extc_rct9)", 1)
    expect_red("同布局每点一份", per_site)

    # ② 定义退化成块作用域（缩进）。
    block = re.sub(r"(?m)^(__attribute__\(\(unused\)\) static const ExtcRcType )", r"        \1", base)
    expect_red("定义退化成块作用域", block)

    # ③ 漏掉文件作用域拼装：删掉所有定义，引用仍在。
    dropped = re.sub(
        r"(?m)^__attribute__\(\(unused\)\) static const (uint32_t __extc_rco\d+\[\]|ExtcRcType __extc_rct\d+).*\n",
        "", base)
    expect_red("漏掉文件作用域拼装", dropped)
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true", help="注入式自测：每条判据都要能变红")
    args = ap.parse_args()
    if not EXTC.exists():
        print("  FAIL %s 不存在（先 make）" % EXTC)
        return 1
    work = Path(tempfile.mkdtemp(prefix="extc-rc-dedup-"))
    try:
        try:
            text, out = emit(work)
        except RuntimeError as e:
            print("  FAIL 合成程序发射失败：%s" % e)
            return 1
        if args.selftest:
            fails = selftest(text)
            print()
            if fails:
                print("失败 %d" % fails)
                return 1
            print("  ok   rc 描述符去重自测：3 条注入全部变红")
            return 0
        bad = check(text)
        for b in bad:
            print("  FAIL %s" % b)
        if bad:
            return 1
        print("  ok   rc 描述符去重：3 种布局 / 5 个分配点共用 3 份文件作用域描述符")
        return 0
    finally:
        shutil.rmtree(str(work), ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())