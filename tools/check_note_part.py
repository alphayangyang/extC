#!/usr/bin/env python3
"""tools/check_note_part.py —— 守住「参与区域数」这条健全性不变量。

## 为什么需要这个门禁

`src/back/reclaim.c` 里有一条**单区域免登记快路径**：`extc_rc_partN`（"参与过 rc 的区域个数"）
为 1 时，赋值路径整段跳过跨区登记的查询与登记本身。它的健全性建立在一句话上：

> **每一个"会登记跨区边"或"会交出 rc 对象"的函数，都必须把自己涉及的区域记进 `partN`。**

这句话一旦被新代码破坏，失效方向是**不安全**的（该登记的没登记 ⇒ 目标区域的清扫可能在
还有活入边时把它放掉），而且**不会**被语义/输出类判据发现 —— 它需要一条结构性的门禁。
这个脚本就是那条门禁：解析**发射出来的运行期文本**，逐函数检查两条规则。

## 两条规则

* R1「会登记 ⇒ 必须记参与」：函数体里出现 `extc_rc_rsInc(` / `extc_rc_rsIncSlab(` 的调用，
  就必须同时出现 `extc_rc_notePart(`。
* R2「会交出槽 ⇒ 必须记参与」：函数体里把 `EXTC_RC_SLAB_DATA(...) + …` 赋给某个变量并且
  最终 `return` 那个变量（即把槽交给调用方当对象用），就必须同时出现 `extc_rc_notePart(`。

两条都允许**具名豁免**（`EXEMPT`），必须写明理由；豁免是显式的，不会随代码漂移悄悄放行。

## 自检

`--selftest` 用合成文本证明两条规则**都能红**（也证明豁免是豁免、不会误报）。
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 具名豁免：函数名 -> 理由（必须是"为什么这条规则对它不适用"，不是"先放过"）。
EXEMPT = {
    "extc_rc_bump":
        "bump 槽从不置占用位，rsIncSlab/rsDecSlab 对它们一律早退 ⇒ 按槽登记对它不适用，"
        "它的回收方式是整片 bumpReset ⇒ 不记 partN 是等价的（记上只会白关掉快路径）",
}

RE_REGISTER = re.compile(r"\bextc_rc_rsInc(?:Slab)?\s*\(")
RE_NOTE = re.compile(r"\bextc_rc_notePart\s*\(")
RE_SLOT = re.compile(r"\b(\w+)\s*=\s*[^;]*EXTC_RC_SLAB_DATA\s*\(")


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释（发射文本里有跨行块注释，和含花括号的注释）。"""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(text[i]); i += 1
    return "".join(out)


def functions(text: str):
    """(名字, 全段) —— 以发射文本的函数签名 + 花括号配对切分。

    签名可能**跨两行**（`static inline void f(void **slot, void *v,\n ExtcRcRegion *host) {`），
    第一版按行找 `{` 就漏掉了这种函数（`storeStructCons` 正是），于是规则根本没跑到它身上 ——
    门禁自己漏检，比没有门禁更危险。这里改成一次正则匹配整个签名（参数表允许跨行）。"""
    text = strip_comments(text)
    sig = re.compile(r"(?:__attribute__\(\(unused\)\)\s*)?static\s+(?:inline\s+)?"
                     r"[\w *]+?\b(\w+)\s*\([^;{]*\)\s*\{")
    pos = 0
    while True:
        m = sig.search(text, pos)
        if not m:
            return
        depth, j = 0, m.end() - 1
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield m.group(1), text[m.start():j + 1]
        pos = j + 1


def violations(text: str):
    """返回 [(函数名, 规则, 说明)]；为空表示不变量成立。"""
    bad = []
    for name, full in functions(text):
        if name in EXEMPT:
            continue
        # 只在**函数体**里找规则：签名行本身含有函数名（`… extc_rc_rsInc(…) {`），
        # 直接搜整段会把定义当成调用（第一版就是这么误报的）。
        body = full.split("{", 1)[1] if "{" in full else full
        registers = bool(RE_REGISTER.search(body))
        notes = bool(RE_NOTE.search(body))
        if registers and not notes:
            bad.append((name, "R1", "会登记跨区边却没记 partN（参与区域数会漏算这个宿主）"))
        handed = {m.group(1) for m in RE_SLOT.finditer(body)}
        returns_slot = any(re.search(r"\breturn\s+%s\s*;" % re.escape(v), body) for v in handed)
        if returns_slot and not notes:
            bad.append((name, "R2", "会交出 rc 槽却没记 partN（参与区域数会漏算这个区域）"))
    return bad


def emit_runtime(tmp: str) -> str:
    exe = os.path.join(tmp, "emit")
    cc = os.environ.get("CC", "cc")
    p = subprocess.run([cc, "-std=c11", "-O1", "-I", f"{ROOT}/src/base", "-I", f"{ROOT}/src/back",
                        f"{ROOT}/tests/reclaim/emit_main.c", f"{ROOT}/src/back/reclaim.c",
                        f"{ROOT}/src/base/base.c", "-o", exe],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        print("发射器编译失败：\n" + p.stdout, file=sys.stderr)
        return ""
    q = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if q.returncode != 0:
        print("发射器运行失败：\n" + q.stdout, file=sys.stderr)
        return ""
    return q.stdout


SELFTEST_CASES = [
    # (名字, 文本, 期望是否有违规)
    ("会登记但不记参与 ⇒ 必须红", """
static void extc_rc_storeX(void **slot, void *v, void *owner) {
    ExtcRcSlab *sv = extc_rc_ownSlab(v);
    if (sv) extc_rc_rsIncSlab(sv, v);
    *slot = v;
}
""", True),
    ("会交出槽但不记参与 ⇒ 必须红", """
static void *extc_rc_allocX(ExtcRcRegion *r, size_t n) {
    ExtcRcSlab *s = r->slabs;
    char *p = EXTC_RC_SLAB_DATA(s) + (0 << s->shift);
    return p;
}
""", True),
    ("登记 + 记参与 ⇒ 必须绿", """
static void extc_rc_storeY(void **slot, void *v, ExtcRcRegion *host) {
    extc_rc_notePart(host);
    ExtcRcSlab *sv = extc_rc_ownSlab(v);
    if (sv) extc_rc_rsIncSlab(sv, v);
    *slot = v;
}
""", False),
    ("豁免函数不报（bump 不置占用位 ⇒ 登记对它不适用）⇒ 必须绿", """
static void *extc_rc_bump(ExtcRcRegion *r, size_t n) {
    char *p = EXTC_RC_SLAB_DATA(r->bumpSlab) + 0;
    return p;
}
""", False),
    ("只读槽（不 return 它）⇒ 必须绿", """
static void extc_rc_freeSlot(ExtcRcSlab *s, size_t i) {
    char *p = EXTC_RC_SLAB_DATA(s) + (i << s->shift);
    s->occ[0] &= ~1u;
}
""", False),
]


def selftest() -> int:
    bad = 0
    for name, text, expect_bad in SELFTEST_CASES:
        got = violations(text)
        ok = bool(got) == expect_bad
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"（{got[0][1]}）" if got else ""))
        bad += 0 if ok else 1
    print(f"自检：{'全部通过' if not bad else f'{bad} 条失败'}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    with tempfile.TemporaryDirectory() as tmp:
        text = emit_runtime(tmp)
    if not text:
        return 1
    bad = violations(text)
    if bad:
        for name, rule, why in bad:
            print(f"FAIL [{rule}] {name}：{why}")
        print(f"参与区域数不变量被破坏（{len(bad)} 处）")
        return 1
    n = sum(1 for _ in functions(text))
    print(f"ok  参与区域数不变量成立（解析了 {n} 个函数；豁免 {len(EXEMPT)} 个，均写明理由）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
