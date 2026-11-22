#!/usr/bin/env python3
"""tools/check_foreign_skip.py —— 守住「跳过旧值登记」这条健全性不变量（与 R2 同源的结构门禁）。

## 不变量

`src/back/reclaim.c` 的 `storeRefOwnedReg` / `storeRefOwned` 里，赋值侧对**旧值**的跨区登记撤销
（`sold = ownSlab(old)` + `rsDecSlab(sold, old)`）被"这只区域有没有出现过外来指针"（`region->foreign`）
把门：`foreign == 0` 时旧值只可能是 null 或同区 ⇒ 查表可省。健全性靠两句话：

1. **跳过只在 `!foreign` 时发生**：每一个 `rsDecSlab`/`rsDec` 调用点都必须在 `foreign` 为真的分支里
   （否则"没有外来指针"的前提被绕过，可能漏撤一条跨区入边 ⇒ 目标区域的清扫会提前放掉被指对象）。
2. **`foreign` 只在真的登记了跨区边时被置位**：每一个 `rsIncSlab`/`rsInc` 调用点都必须在**同一行**
   把宿主区域的 `foreign` 置 1（否则旧值真的可能是外来指针时却走了跳过分支）。

第二句失效是**不安全**方向，而且语义/输出判据看不出来 —— 所以要有这条结构门禁。
`extc_rc_unlinkChildren` 是**具名豁免**：对象被释放时它手里的跨区边必须无条件撤掉，与 `foreign` 无关。

## 一条静态规则 + 一条运行期守卫

* **FS1（静态）**：store 助手里每一处 `rsDecSlab(`/`rsDec(` 调用的**同行**必须出现 `foreign`。
* **FS2（静态）**：整个发射文本里每一处 `rsIncSlab(`/`rsInc(` 调用的**同行**必须出现 `foreign = 1`。
* **FS3（静态）**：跳过判定必须存在（`->foreign` 作为条件出现），且 `foreign` 字段在 slab 结构里。
* **G1（运行期守卫）**：用 `-DEXTC_RC_FOREIGN_GUARD=1` 编 P4 跑一次，要求打印
  `[fg] checks=N bad=M` 且 **N > 0、M == 0**；**拿不到这一行 ⇒ 插桩失败，判红**（不许伪装成绿）。

## 等价形状：条件编译分支里的共用函数体

P1 重构把 `extc_rc_alloc` 的 slab 选取条件写成「`#if` 一种条件带开括号、`#else` 另一种条件带开括号，
函数体与闭括号共用」的形状 —— 两份开括号只配一个闭括号。逐字符无条件配平的 `functions()` 会把两个开
括号都算上，于是函数体一路吞到后面的函数（`rsInc`/`rsDec` 自己的**定义行**、具名豁免的
`extc_rc_unlinkChildren` 都算成 `extc_rc_alloc` 的违例），6 条 FS 全红。**这是切函数体的边界错了，
不是不变量被破坏**：判据本身不动，文本一个字符都不删，两个分支仍逐行检查。

`functions()` 因此识别 `#if/#ifdef/#ifndef/#elif/#else/#endif`：遇到 `#elif/#else` 把配平深度回到
`#if` 时的值（每个分支各自完整），`#endif` 弹栈。这样每个分支独立配平、或各开一个括号共用闭括号
的形状都能切出正确函数体；**没有丢分支、没有放宽判据**，只在配平时把同一条件编译组的两个开括号
当作两个候选而不是两个真实嵌套。等价形状的回归在 `--selftest` 里：新形状必须绿，且两个分支里
各注入一次违例都必须变红。

## 自检 / 红演示

`--selftest` 用合成文本证明 FS1/FS2/FS3 规则**能红**；真实树上的注入演示见
`文档项目 devlog/changelog/` 对应条目（删掉一处 `foreign = 1` ⇒ FS2 红；把 `foreign` 从 rsDec 行拿掉 ⇒ FS1 红；
让守卫计数非 0 ⇒ G1 红）。`--text <文件>` 可以用**别的文本**跑静态规则（红演示就是这么做的）。
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHAPE = "bench/gc/src/spectrum_p4.extc"
SHAPE_N = "20000"

# 具名豁免：函数名 -> 理由（必须是"这条规则对它不适用"，不是"先放过"）。
EXEMPT = {
    "extc_rc_unlinkChildren":
        "对象被释放时要把它手里的跨区边**无条件**撤掉（目标槽还活着的入边少一条是一个真实的"
        "跨区边消失）⇒ 与宿主区域有没有出现过外来指针无关，不需要 foreign 把门",
    "extc_rc_decOldRef":
        "槽里的旧值被覆盖之前，要把旧值持有的跨区入边**无条件**撤掉（不撤就是一个真实的跨区边"
        "永远挂在目标上，目标永远不释放）⇒ 这条撤销与宿主区域有没有出现过外来指针无关；"
        "rscnt 未分配时 rsDecSlab 自身是 no-op，所以不需要 foreign 把门",
}

RE_RSDEC = re.compile(r"\bextc_rc_rsDec(Slab)?\s*\(")
RE_RSINC = re.compile(r"\bextc_rc_rsInc(Slab)?\s*\(")
RE_FOREIGN_SET = re.compile(r"->foreign\s*=\s*1")
RE_FOREIGN_GUARD = re.compile(r"->foreign\b")
# 条件编译指令：只用于切函数体时的花括号配平（见 functions()），不参与判据本身。
# 长名在前，避免 "ifdef" 被 "if" 抢先匹配。
RE_CPP = re.compile(r"#[ \t]*(ifdef|ifndef|elif|else|endif|if)\b")


def strip_comments(text: str) -> str:
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
    """按花括号配平切出 (函数名, 函数体文本)。

    **条件编译感知**：发射文本里有「`#if` 一种条件带开括号、`#else` 另一种条件带开括号，
    函数体与闭括号共用」的形状（P1 重构的 `extc_rc_alloc` 就是）。逐字符无条件配平会把两个
    开括号都算上、只看见一个闭括号，于是函数体一路吞到后面的函数（`rsInc`/`rsDec` 自己的
    定义行、具名豁免的 `unlinkChildren` 的 `rsDec` 全被算成 `extc_rc_alloc` 的违例）。
    这里识别 `#if/#ifdef/#ifndef/#elif/#else/#endif`：遇到 `#elif/#else` 把配平深度回到
    `#if` 时的值（每个分支各自完整），`#endif` 弹栈。**文本一个字符都不删** —— 两个分支
    都照常逐行检查，判据本身没有放松，只是函数体边界切对了。
    """
    text = strip_comments(text)
    sig = re.compile(r"(?:__attribute__\(\(unused\)\)\s*)?static\s+(?:inline\s+)?"
                     r"[\w *]+?\b(\w+)\s*\([^;{]*\)\s*\{")
    pos = 0
    while True:
        m = sig.search(text, pos)
        if not m:
            return
        depth, frames, j = 0, [], m.end() - 1
        while j < len(text):
            ch = text[j]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    break
            elif ch == "#" and not text[text.rfind("\n", 0, j) + 1:j].strip():
                eol = text.find("\n", j)
                if eol < 0:
                    eol = len(text)
                d = RE_CPP.match(text, j)
                if d:
                    kind = d.group(1)
                    if kind in ("if", "ifdef", "ifndef"):
                        frames.append(depth)
                    elif kind in ("elif", "else"):
                        if frames:
                            depth = frames[-1]   # 每个分支都从 #if 时的深度开始
                    elif frames:                  # endif
                        frames.pop()
                    j = eol
            j += 1
        yield m.group(1), text[m.start():j + 1]
        pos = j + 1


def static_violations(text: str):
    """[(规则, 函数, 说明)]；空 = 静态不变量成立。"""
    bad = []
    stripped = strip_comments(text)
    if not re.search(r"int32_t\s+foreign\s*;", stripped):
        bad.append(("FS3", "-", "slab 结构里没有 `int32_t foreign;` —— 跳过旧值登记的前提位不存在"))
    if not RE_FOREIGN_GUARD.search(stripped):
        bad.append(("FS3", "-", "发射文本里没有 `->foreign` 判定 —— 跳过路径没有把门（或被整体删掉）"))
    for name, full in functions(text):
        if name in EXEMPT:
            continue
        body = full.split("{", 1)[1] if "{" in full else full
        for line in body.splitlines():
            if RE_RSDEC.search(line) and "foreign" not in line:
                bad.append(("FS1", name, f"rsDec 没被 foreign 把门：{line.strip()[:90]}"))
            if RE_RSINC.search(line) and not RE_FOREIGN_SET.search(line):
                bad.append(("FS2", name, f"rsInc 没同行置 foreign=1：{line.strip()[:90]}"))
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


def guard_check(tmp: str, guard_c: str = "", guard_n: str = SHAPE_N) -> int:
    """G1：编 P4（带守卫宏）跑一次；要求 checks>0 且 bad==0，拿不到行 ⇒ 红。"""
    cc = os.environ.get("CC", "cc")
    extc = os.path.join(ROOT, "build", "extc")
    if not os.path.exists(extc):
        print("  FAIL G1：build/extc 不存在（先 make）", flush=True)
        return 1
    c = guard_c or os.path.join(tmp, "p4.c")
    e = os.path.join(tmp, "p4")
    if not guard_c:
        env = dict(os.environ, EXTC_STD=os.path.join(ROOT, "stdlib"), EXTC_RECLAIM="1")
        g = subprocess.run([extc, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"),
                            os.path.join(ROOT, SHAPE), "-o", c], capture_output=True, cwd=ROOT, env=env)
        if g.returncode != 0:
            print(f"  FAIL G1：P4 生成失败：{g.stderr[:200]!r}", flush=True)
            return 1
    b = subprocess.run([cc, "-O2", "-DEXTC_RC_FOREIGN_GUARD=1", "-std=c11",
                        "-D_POSIX_C_SOURCE=200809L", "-fwrapv", "-o", e, c], capture_output=True)
    if b.returncode != 0:
        print(f"  FAIL G1：守卫构建失败：{b.stderr[:200]!r}", flush=True)
        return 1
    r = subprocess.run([e, guard_n], capture_output=True, text=True, timeout=1800)
    m = re.search(r"\[fg\] checks=(\d+) bad=(\d+)", r.stderr)
    if not m:
        print("  FAIL G1：拿不到 `[fg]` 行 —— **插桩失败/工具不兼容**（不是「没有可跳过的旧值」）", flush=True)
        return 1
    checks, bad = int(m.group(1)), int(m.group(2))
    if checks <= 0:
        print(f"  FAIL G1：`[fg] checks=0` —— 守卫没跑到跳过路径上（**插桩失效**，不许当绿）", flush=True)
        return 1
    if bad != 0:
        print(f"  FAIL G1：守卫抓到 {bad} 次「旧值是外来指针却没登记过」⇒ foreign 不变量被破坏", flush=True)
        return 1
    print(f"  ok   G1：P4 上跳过 {checks} 次旧值查表，前提全部成立（bad=0）", flush=True)
    return 0


SELFTEST_GOOD = """
struct ExtcRcSlab { int32_t foreign; };
static void extc_rc_storeRefOwnedReg(void **slot, void *v, void *host) {
    if (sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }
    if (host->foreign) { void *old = *slot; ExtcRcSlab *sold = old ? extc_rc_ownSlab(old) : 0; if (sold && sold->region != host) extc_rc_rsDecSlab(sold, old); }
}
static void extc_rc_storeStructCons(void *dst, void *owner) { extc_rc_rsInc(nw); r->foreign = 1; }
"""

# P1 的等价形状（回归用）：`extc_rc_alloc` 的 slab 选取条件写成 `#if`/`#else` 各带一个开括号、
# 函数体与闭括号共用；其后还有 `rsInc`/`rsDec` 的**定义**、具名豁免的 `unlinkChildren`、
# 以及一个 `#if`/`#else` 两个分支里各有一处 rsInc/rsDec 调用的 store 助手。朴素配平会把
# `#if` 分支的两个开括号都算上，函数体一路吞到后面 ⇒ 好文本变红（6 条 FS 的假阳性）。
SELFTEST_CPP = """
struct ExtcRcSlab { ExtcRcRegion *region; int32_t foreign; };
static void *extc_rc_alloc(void *r, size_t n) {
    void *s = 0;
#if EXTC_RC_LAZY_FREE
    if (!s || (bump >= nslots && nfree == 0)) {
#else
    if (!s || (freelist < 0 && bump >= nslots)) {
#endif
        s = extc_rc_slabNew(r, n);
    }
    return s;
}
/* 以下是 alloc 之后的函数：朴素配平会把它们一起吞进 alloc 的函数体。 */
__attribute__((unused)) static void extc_rc_rsInc(void *t) { (void)t; }
__attribute__((unused)) static void extc_rc_rsDec(void *t) { (void)t; }
__attribute__((unused)) static void extc_rc_rsIncSlab(ExtcRcSlab *s, void *t) { (void)s; (void)t; }
__attribute__((unused)) static void extc_rc_rsDecSlab(ExtcRcSlab *s, void *t) { (void)s; (void)t; }
__attribute__((unused)) static void extc_rc_unlinkChildren(ExtcRcSlab *s, size_t i) {
    void *c = ((void **)s)[i];
    if (c) { ExtcRcSlab *cs = extc_rc_ownSlab(c); if (cs && cs->region != s->region) extc_rc_rsDecSlab(cs, c); }
}
static void extc_rc_storeRefOwnedReg(void **slot, void *v, ExtcRcRegion *host) {
#if EXTC_RC_NOCOUNT
    if (extc_rc_partN > 1) {
        if (host && sv && sv->region != host) { extc_rc_rsIncSlab(sv, v); host->foreign = 1; }
        if (host && host->foreign) { void *old = *slot; ExtcRcSlab *sold = extc_rc_ownSlab(old); if (sold && sold->region != host) extc_rc_rsDecSlab(sold, old); }
#else
    if (host) {
        ExtcRcRegion *r = host;
        if (sv && sv->region != r) { extc_rc_rsIncSlab(sv, v); r->foreign = 1; }
        if (r->foreign && sold && sold->region != r) extc_rc_rsDecSlab(sold, old);
#endif
        *slot = v;
    }
}
"""


def selftest() -> int:
    fail = 0

    def expect(name, text, want_any, want_fn=None):
        nonlocal fail
        v = static_violations(text)
        got = {(r, fn) for r, fn, _ in v}
        if want_any is None:
            ok = not v
        elif want_fn is not None:
            ok = (want_any, want_fn) in got
        else:
            ok = want_any in {r for r, _ in got}
        if ok:
            print(f"  ok   selftest {name}")
        else:
            print(f"  FAIL selftest {name}：期望 {want_any or '空'}"
                  f"{'/' + want_fn if want_fn else ''}，得到 {sorted(got)}")
            fail = 1

    # ---- 原有形状（单签名、无 #if 分支）----
    expect("好文本必须绿", SELFTEST_GOOD, None)
    expect("删掉 foreign=1 ⇒ FS2 红",
           SELFTEST_GOOD.replace("extc_rc_rsInc(nw); r->foreign = 1;", "extc_rc_rsInc(nw);"),
           "FS2", "extc_rc_storeStructCons")
    expect("删掉 rsDec 行的 foreign ⇒ FS1 红",
           SELFTEST_GOOD.replace("if (host->foreign) { void *old", "if (1) { void *old"),
           "FS1", "extc_rc_storeRefOwnedReg")
    expect("删掉字段 ⇒ FS3 红", SELFTEST_GOOD.replace("int32_t foreign;", ""), "FS3")

    # ---- P1 等价形状（#if/#else 各带开括号、共用函数体与闭括号）----
    expect("等价形状（#if 分支共用函数体）必须绿", SELFTEST_CPP, None)
    expect("等价形状 #if 分支删 foreign=1 ⇒ FS2 红",
           SELFTEST_CPP.replace("extc_rc_rsIncSlab(sv, v); host->foreign = 1;",
                                "extc_rc_rsIncSlab(sv, v);"),
           "FS2", "extc_rc_storeRefOwnedReg")
    expect("等价形状 #else 分支删 foreign=1 ⇒ FS2 红（另一分支没被丢掉）",
           SELFTEST_CPP.replace("extc_rc_rsIncSlab(sv, v); r->foreign = 1;",
                                "extc_rc_rsIncSlab(sv, v);"),
           "FS2", "extc_rc_storeRefOwnedReg")
    expect("等价形状 #if 分支 rsDec 不再把门 ⇒ FS1 红",
           SELFTEST_CPP.replace("if (host && host->foreign) { void", "if (host) { void"),
           "FS1", "extc_rc_storeRefOwnedReg")
    expect("等价形状 #else 分支 rsDec 不再把门 ⇒ FS1 红（另一分支没被丢掉）",
           SELFTEST_CPP.replace("if (r->foreign && sold", "if (sold"),
           "FS1", "extc_rc_storeRefOwnedReg")
    return fail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", help="用这个文件当发射文本（红演示用）；默认现场发射")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--no-guard", action="store_true", help="只跑静态规则（合成文本演示时用）")
    ap.add_argument("--guard-c", default="", help="用这个生成物 C 跑 G1（红演示用）；默认现场生成")
    ap.add_argument("--guard-n", default=SHAPE_N, help="G1 跑那个形状时的参数（默认 P4 的 20000）")
    args = ap.parse_args()
    if args.selftest:
        print("== check_foreign_skip selftest（合成文本）==")
        rc = selftest()
        print("通过" if rc == 0 else "失败")
        return rc

    tmp = tempfile.mkdtemp(prefix="fs-")
    try:
        if args.text:
            text = open(args.text, encoding="utf-8", errors="replace").read()
            src_desc = args.text
        else:
            text = emit_runtime(tmp)
            src_desc = "现场发射（src/back/reclaim.c）"
            if not text:
                return 1
        print(f"== gate-foreign-skip：跳过旧值登记的不变量（{src_desc}）==")
        bad = static_violations(text)
        if bad:
            for rule, fn, why in bad:
                print(f"  FAIL [{rule}] {fn}：{why}")
        else:
            print("  ok   FS1/FS2/FS3：每个 rsDec 都被 foreign 把门，每个 rsInc 都同行置位，前提位存在")
        rc = 1 if bad else 0
        if not args.no_guard and (not args.text or args.guard_c):
            rc |= guard_check(tmp, args.guard_c, args.guard_n)
        print()
        if rc:
            print("失败：跳过旧值登记的不变量被破坏（或守卫插桩失效）")
        else:
            print("  ok   跳过旧值登记：静态前提成立，运行期守卫 0 违例")
        return rc
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
