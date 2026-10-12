#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/escape-fuzz/harness.py — 跑语料、抓报错、按判据聚类、打分类表。

口径（基线，不做门禁）：
  · extC 编过  ⇒ gcc -fwrapv -O1 -fsanitize=address 编过 + 跑过 + ASan 干净 + 输出与 C++ 一致
  · extC 被拒  ⇒ 抓**完整**报错文本，按判据聚类（语法/类型错单独一列，不算误拒）
  · C++ 那份   ⇒ g++ -O1 编过 + 跑过（就为了证明「堆式写法」在 C++ 里是普通程序）

退出码恒为 0（这是证据，不是门禁）。语法/类型错会显式列出——生成器必须把它们清零。
"""

import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CASES = os.path.join(HERE, "cases")
MIN = os.path.join(HERE, "min")
EXTC = os.environ.get("EXTC", os.path.join(ROOT, "build", "extc"))
CC = os.environ.get("CC", "cc")
CXX = os.environ.get("CXX", "g++")
JOBS = int(os.environ.get("JOBS", "8"))
TIMEOUT = 60

SYNTAX_MARKERS = (
    "expected ", "unknown type", "is a **reference**", "needs a non-null",
    "duplicate ", "cannot take a reference", "no method", "does not know which",
    "already a reference", "cannot index", "cannot infer the type",
    "mismatched", "undefined", "redeclar", "invalid", "unsupported",
    "cannot assign", "cannot use", "no field", "unknown field", "arity",
    "not a struct", "expects", "cannot appear inside a function body",
)

# 判据聚类：id -> (正则, 判定, 理由)
CLASSES = [
    ("E_DEPTH_A0_RET",
     r"this return value would hold a reference to a local variable that dies first"
     r".*up to depth 0",
     "真误拒",
     "返回值要活过本帧（at=0），但值的深度被登记为局部槽的深度（≥1），且该函数没有被认定"
     "为「返回引用的逃逸函数」（生成物里没有 __extc_home）⇒ 提权不到 0 层，直接拒。"
     "程序若按 new=堆 的语义看并不悬垂：节点本该随返回值活到调用者。"),
    ("E_DEPTH_A0_STORE",
     r"this reference would hold a reference to a local variable that dies first"
     r".*up to depth 0",
     "真误拒",
     "存储目标被判据读成层 0（调用者的 home）：要么目标经 `(*x).f` 而 x 的被指寿命证不出来"
     "（读成 0），要么目标在返回引用的函数里项目深度为 0。函数没有 home 时 0 层不可达 ⇒ 拒。"
     "cur 的赋值只要在语句**之前**出现过（flow=before）同一写法就通过 ⇒ 判据对词序敏感。"),
    ("E_DEPTH_AN",
     r"this reference would hold a reference to a local variable that dies first"
     r".*up to depth [1-9]",
     "该拒（真会悬垂）",
     "更深的局部对象被借进更浅的外层容器，出块即死 ⇒ 真的悬垂。这是深度规则的本职。"
     "本语料里 `new` 值不会落进这一类（提权能救）；落进来的是「借来的局部」"
     "（r_deep_store）。"),
    ("E_DEPTH_CALLRES",
     r"this reference would hold a reference to a local variable that dies first"
     r".*up to depth [1-9]",
     "真误拒（保守）",
     "与 E_DEPTH_AN 同一条判据、同一种文本，但值来自 `var p = mk(i)`：调用结果被登记成"
     "**绑定的槽深度**（循环体 2 / 函数体 1），而不是被调者拿到的 home 层。"
     "同一个调用点直接写成 `head = mk(1)`（不落中间绑定）就编得过 ⇒ 记账口径问题，"
     "程序不悬垂。"),
    ("E_ARG_DEPTH",
     r"points into a deeper scope .*than the arena this call may store it in",
     "真误拒（保守）",
     "实参指向比这次调用能存储的 arena 更深。与 E_DEPTH_AN 同根：调用结果的深度被算成块深度，"
     "于是「刚 new 出来的实参」被当成「指向块内局部」。若记账不改，拒是安全的一侧；"
     "按 new=堆 的语义它不该拒。"),
    ("E_GLOBAL",
     r"cannot zero-initialize the global|cannot infer the type of a bare",
     "该拒（语言规则，先于逃逸分析）",
     "全局是静态存储（层 0），含引用的全局根本声明不出来。与逃逸分析无关，"
     "是「new 不能存进全局」这条路上的第一块拦路石。"),
    ("E_ACCEPT_UAF",
     r"AddressSanitizer",
     "漏收（soundness hole，方向相反）",
     "extC 收下了，生成物却在 ASan 下 heap-use-after-free：`new` 落在本帧 arena、"
     "函数返回后被释放，而返回值/字段仍指向它。必须修：要么正确提权到 home，要么拒。"),
    ("E_RUN_OTHER",
     r".",
     "待看（非 ASan 的运行失败）",
     "运行期非零退出且不是 ASan 报错，需人工看。"),
]


def classify(err, row):
    """按报错文本 + 维度给一个类 id。"""
    if not err:
        return None, ""
    first = ""
    for line in err.splitlines():
        if "error" in line and "warning" not in line:
            first = line
            break
    text = err
    # 深度规则：先按 at（up to depth 0 / N）再按报错位置分。
    if re.search(r"would hold a reference to a local variable that dies first", text):
        if re.search(r"up to depth 0", text):
            if re.search(r"this return value would hold", text):
                return "E_DEPTH_A0_RET", first
            return "E_DEPTH_A0_STORE", first
        if row.get("flow") == "ret":
            return "E_DEPTH_CALLRES", first
        return "E_DEPTH_AN", first
    for cid, pat, _j, _r in CLASSES:
        if cid in ("E_DEPTH_A0_RET", "E_DEPTH_A0_STORE", "E_DEPTH_AN",
                   "E_ACCEPT_UAF", "E_RUN_OTHER"):
            continue
        if re.search(pat, text, re.S):
            return cid, first
    if any(m in text for m in SYNTAX_MARKERS):
        return "E_SYNTAX_TYPE", first
    return "E_OTHER", first


def run(cmd, timeout=TIMEOUT):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), \
            p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -9, "", "TIMEOUT"


def one_case(row, tmp, base=CASES):
    (name, fam, dest, flow, loc, order, narrow, expect) = row
    extc_path = os.path.join(base, name + ".extc")
    cpp_path = os.path.join(base, name + ".cpp")
    r = dict(name=name, fam=fam, dest=dest, flow=flow, loc=loc, order=order,
             narrow=narrow, expect=expect)
    c_path = os.path.join(tmp, name + ".c")
    rc, out, err = run([EXTC, extc_path, "-o", c_path])
    r["extc_rc"] = rc
    r["extc_err"] = err
    if rc != 0:
        r["cls"], r["first_err"] = classify(err, r)
        r["status"] = "REJECT"
    else:
        r["status"] = "ACCEPT"
        try:
            gen = open(c_path, encoding="utf-8", errors="replace").read()
            r["has_home"] = "__extc_home" in gen
            fname = {"home": "build", "nohome": "burn", "main": "main"}[loc]
            r["target_home"] = bool(re.search(
                r"\b" + fname + r"\(\s*(?:extc_arena|__extc_home)", gen)) or \
                bool(re.search(r"\b" + fname + r"\(extc_arena \*__extc_home", gen))
        except OSError:
            r["has_home"] = None
            r["target_home"] = None
        exe = os.path.join(tmp, name)
        rc2, out2, err2 = run([CC, "-fwrapv", "-O1", "-fsanitize=address",
                               "-o", exe, c_path])
        if rc2 != 0:
            r["status"] = "C_FAIL"
            r["first_err"] = (err2 or "").splitlines()[0] if err2 else ""
        else:
            rc3, out3, err3 = run([exe])
            r["run_rc"] = rc3
            r["run_out"] = out3
            if "AddressSanitizer" in err3:
                r["status"] = "RUN_BAD"
                r["cls"] = "E_ACCEPT_UAF"
                m_asan = re.search(r"ERROR: AddressSanitizer: ([a-z-]+)", err3)
                r["asan_kind"] = m_asan.group(1) if m_asan else "?"
                r["first_err"] = "AddressSanitizer: " + r["asan_kind"]
            elif rc3 != 0:
                r["status"] = "RUN_BAD"
                r["cls"] = "E_RUN_OTHER"
                r["first_err"] = (err3.splitlines() or [""])[0]
    if os.path.exists(cpp_path):
        exe = os.path.join(tmp, name + "_cpp")
        rc4, out4, err4 = run([CXX, "-O1", "-o", exe, cpp_path])
        r["cpp_ok"] = (rc4 == 0)
        if rc4 == 0:
            rc5, out5, err5 = run([exe])
            r["cpp_run_rc"] = rc5
            r["cpp_out"] = out5
        else:
            r["cpp_err"] = (err4 or "").splitlines()[0] if err4 else ""
    else:
        r["cpp_ok"] = None
    if r["status"] == "ACCEPT":
        r["match"] = (r.get("cpp_out") == r.get("run_out"))
    return r


def load_rows():
    rows = []
    with open(os.path.join(CASES, "index.tsv"), encoding="utf-8") as f:
        next(f)
        for line in f:
            line = line.rstrip("\n")
            if line:
                rows.append(tuple(line.split("\t")))
    return rows


def check_min(tmp):
    """min/ 里每个最小复现必须复现 manifest 里写的类。"""
    manifest = os.path.join(MIN, "manifest.tsv")
    if not os.path.exists(manifest):
        return []
    out = []
    with open(manifest, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            name, want = parts[0], parts[1]
            flow = parts[2] if len(parts) > 2 and parts[2] else "direct"
            note = parts[3] if len(parts) > 3 else ""
            loc = "home" if name.startswith("home_") else (
                "nohome" if name.startswith("nohome_") else "main")
            r = one_case((name, "-", "-", flow, loc, "-", "-", "0"),
                         tmp, base=MIN)
            got = r.get("cls") or ("ACCEPT" if r["status"] == "ACCEPT" else r["status"])
            first = r.get("first_err") or ""
            out.append((name, want, got, first.split("error:")[-1].strip()[:90], note))
    return out


def main():
    rows = load_rows()
    tmp = tempfile.mkdtemp(prefix="escape-fuzz-")
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        results = list(ex.map(lambda row: one_case(row, tmp), rows))
    results.sort(key=lambda r: r["name"])

    n = len(results)
    acc = [r for r in results if r["status"] == "ACCEPT"]
    rej = [r for r in results if r["status"] == "REJECT"]
    uaf = [r for r in results if r.get("cls") == "E_ACCEPT_UAF"]
    runother = [r for r in results if r.get("cls") == "E_RUN_OTHER"]
    cfail = [r for r in results if r["status"] == "C_FAIL"]
    syn = [r for r in results if r.get("cls") in ("E_SYNTAX_TYPE", "E_OTHER")]
    mism = [r for r in acc if r.get("match") is False]
    cpp_bad = [r for r in results if r.get("cpp_ok") is False]
    cpp_run_bad = [r for r in results if r.get("cpp_ok") and r.get("cpp_run_rc") != 0]
    a0 = [r for r in rej if r.get("cls") in ("E_DEPTH_A0_STORE", "E_DEPTH_A0_RET")]
    home_acc = [r for r in acc if r.get("has_home")]
    home_no = [r for r in acc if r.get("has_home") is False]

    buckets = {}
    for r in rej:
        buckets.setdefault(r.get("cls") or "E_OTHER", []).append(r)
    for r in uaf + runother:
        buckets.setdefault(r.get("cls") or "E_OTHER", []).append(r)

    def w(s):
        import unicodedata
        return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)

    def rowprint(cells, widths):
        return "  ".join(c + " " * (widths[i] - w(c)) for i, c in enumerate(cells))

    print("=" * 100)
    print("escape-fuzz 基线：%d 个形状（extC 对照 C++ 裸 new；被拒形状按判据聚类）" % n)
    print("=" * 100)
    print("  总数            : %d" % n)
    print("  extC 通过       : %d" % len(acc))
    print("  extC 被拒       : %d" % len(rej))
    print("    其中真误拒    : %d（E_DEPTH_A0_* + E_DEPTH_CALLRES + E_ARG_DEPTH）"
          % (len(a0) + len(buckets.get("E_DEPTH_CALLRES", []))
             + len(buckets.get("E_ARG_DEPTH", []))))
    print("    该拒(真悬垂)  : %d（E_DEPTH_AN）" % len(buckets.get("E_DEPTH_AN", [])))
    print("    设计限制      : %d（E_GLOBAL；先于逃逸分析）" % len(buckets.get("E_GLOBAL", [])))
    print("  通过但 ASan 脏  : %d（soundness hole，方向与误拒相反）" % len(uaf))
    print("  生成 C 编不过   : %d" % len(cfail))
    print("  输出不符 C++    : %d" % len(mism))
    print("  C++ 对照没编过  : %d（编过的都能跑：%d）"
          % (len(cpp_bad), len([r for r in results if r.get("cpp_ok")]) - len(cpp_run_bad)))
    print("  语法/类型错     : %d（生成器必须清零；这些不是误拒证据）" % len(syn))
    print("  通过的形状里：生成物带 __extc_home 的 %d 个，不带的 %d 个"
          % (len(home_acc), len(home_no)))
    th = [r for r in acc if r.get("target_home")]
    print("  其中**形状自己所在函数**拿到 home 的：%d 个（loc=home 且被收下的：%d/%d）"
          % (len(th), len([r for r in acc if r["loc"] == "home" and r.get("target_home")]),
             len([r for r in acc if r["loc"] == "home"])))
    a0_no = len([r for r in a0 if r["loc"] in ("main", "nohome")])
    a0_home = len([r for r in a0 if r["loc"] == "home"])
    print("  ── 关键交叉 ──")
    print("  E_DEPTH_A0_* 里 loc=main/nohome（0 层根本不可达）: %d；loc=home（home 没被认出来）: %d"
          % (a0_no, a0_home))
    print("  E_ACCEPT_UAF 里所在函数**有** home（提权没落到 home）: %d；**没有** home（漏判逃逸函数）: %d"
          % (len([r for r in uaf if r.get("has_home")]),
             len([r for r in uaf if not r.get("has_home")])))
    print("  E_DEPTH_CALLRES（同一调用点直接存就过、经中间绑定就拒）: %d" % len(buckets.get("E_DEPTH_CALLRES", [])))
    print()

    cls_order = []
    for cid in [c[0] for c in CLASSES] + ["E_SYNTAX_TYPE", "E_OTHER"]:
        if cid != "E_RUN_OTHER" and cid not in cls_order:
            cls_order.append(cid)
    hdr = ["类", "触发判据", "样例", "该不该修", "最小复现"]
    widths = [14, 30, 5, 12, 30]
    table = []
    repro = {
        "E_DEPTH_A0_STORE": "min/main_dfield_after.extc",
        "E_DEPTH_A0_RET": "min/home_ff_return.extc",
        "E_DEPTH_CALLRES": "min/home_ret_binding.extc",
        "E_DEPTH_AN": "min/deep_store.extc",
        "E_ARG_DEPTH": "min/param_ret_arg.extc",
        "E_GLOBAL": "min/global_ref.extc",
        "E_ACCEPT_UAF": "min/home_pfield_uaf.extc",
        "E_SYNTAX_TYPE": "—",
        "E_OTHER": "—",
    }
    for cid in cls_order:
        rs = buckets.get(cid)
        if not rs:
            continue
        j = dict((c[0], c[2]) for c in CLASSES).get(cid, "真误拒（保守）")
        pat = dict((c[0], c[1]) for c in CLASSES).get(
            cid, r"would hold a reference to a local variable that dies first .*up to depth [1-9]")
        table.append([cid, pat, str(len(rs)), j, repro.get(cid, "—")])
    for c in table:
        for i, v in enumerate(c):
            if i == 1:
                continue
            widths[i] = max(widths[i], w(v))
    print(rowprint([hdr[0], hdr[2], hdr[3], hdr[4]], [widths[0], widths[2], widths[3], widths[4]]))
    print("  ".join("-" * widths[i] for i in (0, 2, 3, 4)))
    for c in table:
        print(rowprint([c[0], c[2], c[3], c[4]], [widths[0], widths[2], widths[3], widths[4]]))
    print()
    print("判据原文（每类第一例，完整文本见 last-run.tsv）:")
    for cid in cls_order:
        rs = buckets.get(cid)
        if not rs:
            continue
        print("  [%s] %s" % (cid, (rs[0].get("first_err") or "").replace(ROOT + "/", "")[:160]))
    print()

    if syn:
        print("!! 语法/类型错（必须清零）：")
        for r in syn[:20]:
            print("   %-40s %s" % (r["name"], (r.get("first_err") or "")[:120]))
        print()
    if cfail:
        print("!! 生成 C 编不过：")
        for r in cfail[:20]:
            print("   %-40s %s" % (r["name"], (r.get("first_err") or "")[:120]))
        print()
    if mism:
        print("!! extC 与 C++ 输出不一致：")
        for r in mism[:20]:
            print("   %-40s extC=%r cpp=%r" % (r["name"], r.get("run_out"),
                                               r.get("cpp_out")))
        print()
    if cpp_bad:
        print("!! C++ 对照没编过：")
        for r in cpp_bad[:20]:
            print("   %-40s %s" % (r["name"], (r.get("cpp_err") or "")[:120]))
        print()

    mins = check_min(tmp)
    if mins:
        print("最小复现核对（min/manifest.tsv）:")
        ok = 0
        for name, want, got, line, note in mins:
            good = (want == got)
            ok += 1 if good else 0
            print("  %s %-26s 期望 %-18s 实得 %-18s %s" % (
                "OK " if good else "BAD", name, want, got, line))
        print("  → %d/%d 一致" % (ok, len(mins)))
        print()

    with open(os.path.join(HERE, "last-run.tsv"), "w", encoding="utf-8") as f:
        f.write("name\tfamily\tdest\tflow\tloc\torder\tnarrow\tstatus\tclass\t"
                "has_home\trun_out\tcpp_out\tfirst_err\n")
        for r in results:
            f.write("\t".join([
                r["name"], r["fam"], r["dest"], r["flow"], r["loc"], r["order"],
                r["narrow"], r["status"], r.get("cls") or "",
                str(r.get("has_home", "")) + "/" + str(r.get("target_home", "")),
                (r.get("run_out") or "").strip(),
                (r.get("cpp_out") or "").strip(),
                (r.get("first_err") or "").replace("\t", " ").replace(ROOT + "/", "")[:300],
            ]) + "\n")
    print("明细: %s" % os.path.join(HERE, "last-run.tsv"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
