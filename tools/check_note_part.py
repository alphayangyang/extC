#!/usr/bin/env python3
"""tools/check_note_part.py —— 守住「参与区域数」这条健全性不变量。

## 为什么需要这个门禁

`src/back/reclaim.c` 里有一条**单区域免登记快路径**：`extc_rc_partN`（"参与过 rc 的区域个数"）
为 1 时，赋值路径整段跳过跨区登记的查询与登记本身。它的健全性建立在一句话上：

> **每一个"会登记跨区边"或"会交出 rc 对象"的函数，都必须把自己涉及的区域记进 `partN`。**

这句话一旦被新代码破坏，失效方向是**不安全**的（该登记的没登记 ⇒ 目标区域的清扫可能在
还有活入边时把它放掉），而且**不会**被语义/输出类判据发现 —— 它需要一条结构性的门禁。
这个脚本就是那条门禁：解析**发射出来的运行期文本**，逐函数检查静态规则，并配一条运行期守卫。

## 一条静态规则 + 一条运行期守卫

* **R2（静态）**「会交出槽 ⇒ 必须记参与」：函数体里把 `EXTC_RC_SLAB_DATA(...) + …` 赋给某个
  变量并且最终 `return` 那个变量（即把槽交给调用方当对象用），就必须同时出现
  `extc_rc_notePart(`。
* **R3（运行期）**：`extc_rc_regionFree` 里，`part == 0` 的区域**不得有占用槽**，有就
  `oops`（`R3: occupied slot in a region with part == 0`）。冷路径；`part` 为真时只有一次
  可预测分支。它覆盖静态规则看不见的那一半：只要有一条"产槽但不记参与"的新路径，运行到
  区域销毁就会当场炸，而不是等到某个区域的清扫把活对象放掉。

静态规则允许**具名豁免**（`EXEMPT`），必须写明理由；豁免是显式的，不会随代码漂移悄悄放行。

### 为什么没有「会登记 ⇒ 必须记参与」（原 R1）

登记（`rsInc`/`rsDec`）只作用在**已存在**的槽上，而任何槽的区域早在它诞生时（`extc_rc_alloc`，
即 R2 覆盖的那条路；`bump` 具名豁免）就已经记过参与 ⇒ **赋值侧再记一次是冗余的**，代价是每次
赋值一次 `ownSlab` 查找。实测（主树五形状；数字与口径见 `docs/CHANGELOG.md` 的
2026-10-15 条，这里不抄副本）：删掉赋值侧那两次 `notePart` 后 `shard_bands` 墙钟 −4 成量级、
窗口降一个数量级，`evict` −1.5 成量级，语义逐字不变、闸门全绿。R1 会把这份
冗余写成"必须保留"，所以删掉它，由 R3 从运行期守住同一条不变量。

**R3 能红的演示**（改完必须做一次）：把 `extc_rc_alloc` 里的 `extc_rc_notePart(r);` 删掉再跑
任何 reclaim 形状 ⇒ 静态 `FAIL [R2] extc_rc_alloc`（rc=1）**且**运行期
`trap: reclaim: R3: occupied slot in a region with part == 0`（rc=1）；撤回后绿。

## 自检

`--selftest` 用合成文本证明静态规则**能红**（"只登记、不产槽"在收窄后必须绿，也证明豁免是
豁免、不会误报）；R3 那一半的"能红"由上面的注入演示证明（它是运行期守卫，没有合成文本可喂）。
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
        notes = bool(RE_NOTE.search(body))
        handed = {m.group(1) for m in RE_SLOT.finditer(body)}
        returns_slot = any(re.search(r"\breturn\s+%s\s*;" % re.escape(v), body) for v in handed)
        if returns_slot and not notes:
            bad.append((name, "R2", "会交出 rc 槽却没记 partN（参与区域数会漏算这个区域）"))
    return bad


def emit_runtime(tmp: str, env=None) -> str:
    """编译并运行发射器，返回它吐出的运行期文本。

    env：给发射器的额外环境变量（档位开关，如 {"EXTC_RC_TIME": "1"}）；缺省 = 默认档。
    """
    exe = os.path.join(tmp, "emit")
    cc = os.environ.get("CC", "cc")
    p = subprocess.run([cc, "-std=c11", "-O1", "-I", f"{ROOT}/src/base", "-I", f"{ROOT}/src/back",
                        f"{ROOT}/tests/reclaim/emit_main.c", f"{ROOT}/src/back/reclaim.c",
                        f"{ROOT}/src/base/base.c", "-o", exe],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        print("发射器编译失败：\n" + p.stdout, file=sys.stderr)
        return ""
    run_env = dict(os.environ)
    if env:
        run_env.update(env)
    q = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       env=run_env)
    if q.returncode != 0:
        print("发射器运行失败：\n" + q.stdout, file=sys.stderr)
        return ""
    return q.stdout


SELFTEST_CASES = [
    # (名字, 文本, 期望是否有违规)
    ("只登记、不产槽 ⇒ 必须绿（登记只作用于已存在的槽，其区域在 alloc 侧已记参与）", """
static void extc_rc_storeX(void **slot, void *v, void *owner) {
    ExtcRcSlab *sv = extc_rc_ownSlab(v);
    if (sv) extc_rc_rsIncSlab(sv, v);
    *slot = v;
}
""", False),
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
    print(f"ok  参与区域数不变量成立（静态 R2：解析了 {n} 个函数；豁免 {len(EXEMPT)} 个，均写明理由）"
          f"｜运行期 R3 守卫在 extc_rc_regionFree（见模块 docstring 的红演示步骤）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
