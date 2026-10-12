#!/usr/bin/env python3
"""tests/reclaim-gaps/audit-static.py —— 全语料生成物的**静态审计**（两个红计数）。

对每个能编译的 `.extc`（`EXTC_RECLAIM=1`，保留 `#line` 映射），扫生成物里的回收接线：

  ① **区域没有回收者**：数每个程序的
       alloc  = `extc_rc_alloc(` 调用点（有回收管辖的分配点）
       trig   = `extc_rc_maybeCollect(__extc_rcg…` 调用点（真触发）
       owe    = `extc_rc_owe(__extc_rcw…` 调用点（只记欠收）
       check  = `extc_rc_checkOwed(__extc_rcc…` 调用点（拥有者销账）
       srunk  = `extc_rc_storeRefUnknown(` 调用点（⇒ 目标区域运行期 `noSweep`）
     分类（`class` 列）：
       ok-trigger            有触发（本帧拥有的区域照常扫）
       owe-settled           不触发但记欠收、且有销账点（子集 B 的形状）
       owe-no-settle         不触发、只记欠收、**没有销账点** ⇒ 等到帧销毁
       silent-both           既不触发、也不记欠收 ⇒ 该函数的分配点没有回收者
       no-alloc              没有回收管辖的分配点（不适用）
     红计数① = `silent-both` + `owe-no-settle` 的程序数。

  ② **引用槽写入没有对应记账**：用生成物自己的 `ExtcRcType` 偏移表
     （`__extc_rcoN[]` 里的 `offsetof(TYPE, FIELD)`）取出"哪些字段是引用叶子"，
     再在 `#line` 之后的**用户代码**里找 `X->f = …` / `X.f = …` 形式、字段是引用叶子、
     而同一行没有 `extc_rc_*` 记账调用的写入。这是**启发式**（只有字段名、没有逐表达式类型），
     所以只报"候选 + 样本"，不声称每条都是真洞。红计数② = 候选写入的程序数。

用法：
    python3 tests/reclaim-gaps/audit-static.py                 # 全语料，打印摘要
    python3 tests/reclaim-gaps/audit-static.py --limit 40      # 冒烟
    python3 tests/reclaim-gaps/audit-static.py --gate          # 两个红计数非 0 ⇒ exit 1
    python3 tests/reclaim-gaps/audit-static.py --out DIR       # 原始 TSV 落盘目录

不改 `src/`、`tools/`、`tests/` 既有文件；只写 `--out`（默认 `tests/reclaim-gaps/out`）。
"""

import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CORPUS = ["examples", "tests", "bench", "stdlib", "tools/escape-corpus"]

RCO = re.compile(r"__extc_rco\d+\[\]\s*=\s*\{(.*?)\};", re.S)
OFF = re.compile(r"offsetof\(\s*(\w+)\s*,\s*(\w+)\s*\)")
FIRST_LINE = re.compile(r'^#line\s+\d+\s+"[^"]*\.extc"', re.M)


def count_call(text, pat, has_def):
    """数调用点。`has_def` = 这条模式也会命中运行期里的定义（那种要减去 1）。"""
    n = len(re.findall(pat, text))
    return max(0, n - 1) if has_def else n


def bare_writes(text):
    """② 的检测器：类型感知的启发式，返回未记账的直接引用槽写入行。"""
    # ── ② 引用槽写入没有记账（类型感知的启发式）──────────────────────────
    # 生成物自带两张表：① 每个结构的 C 定义（类型 → 字段 → C 类型）；② `__extc_rcoN[]` 里的
    # `offsetof(T, f)` = “T 的 f 是引用叶子”。ref 槽 = C 类型是 `T *` 且 T 在偏移表里出现。
    # LHS 的字段链逐段在**类型上**解析；只要有一支会落到非 ref 或判不出，就不算（宁可漏报）。
    structfields = {}
    for sm in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\};", text, re.S):
        tname, body = sm.group(1), sm.group(2)
        fields = {}
        for fm in re.finditer(r"([\w\*\s]+?)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;", body):
            fields[fm.group(2)] = fm.group(1).strip()
        structfields[tname] = fields
    reftypes = set()
    for body in RCO.findall(text):
        for ty, _fld in OFF.findall(body):
            reftypes.add(ty)
    m = FIRST_LINE.search(text)
    user = text[m.start():] if m else text
    lhs_re = re.compile(r"^\s*([^=;]+?)\s*(?<![!<>=])=(?!=)")
    bare = []

    def resolve(chain):
        """chain = ['kids', 'data'] → True/None。True = 唯一结论是 ref 槽。"""
        cands = None   # 当前可能的 C 类型集合
        for i, fld in enumerate(chain):
            if i == 0:
                cands = {structfields[t][fld] for t in structfields if fld in structfields[t]}
            else:
                nxt = set()
                for ct in cands:
                    base = ct.replace("*", "").strip()
                    if base in structfields and fld in structfields[base]:
                        nxt.add(structfields[base][fld])
                    else:
                        return None    # 这一段在这个类型上解析不出来
                cands = nxt
            if not cands:
                return None
        for ct in cands:
            base = ct.replace("*", "").strip()
            if not (ct.endswith("*") and base in reftypes):
                return None
        return True

    for ln in user.splitlines():
        if "extc_rc_" in ln:
            continue
        mm = lhs_re.match(ln)
        if not mm:
            continue
        core = mm.group(1).split("[")[0]            # 丢掉下标表达式（里面可能有文件名里的点）
        if "->" not in core and "." not in core:
            continue
        segs = re.findall(r"(?:->|\.)\s*(\w+)", core)
        if segs and resolve(segs) is True:
            bare.append(ln.strip()[:120])

    return bare


def scan(path, extc, tmpdir):
    out_c = os.path.join(tmpdir, os.path.basename(path).replace("/", "_") + ".c")
    try:
        r = subprocess.run([extc, "-w", path, "-o", out_c],
                           cwd=ROOT, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"path": path, "compile": "timeout"}
    if r.returncode != 0 or not os.path.exists(out_c):
        return {"path": path, "compile": "fail"}
    with open(out_c, "r", errors="replace") as f:
        text = f.read()

    alloc = count_call(text, r"extc_rc_alloc\(", True)
    trig = count_call(text, r"extc_rc_maybeCollect\(__extc_rcg", False)
    owe = count_call(text, r"extc_rc_owe\(__extc_rcw", False)
    check = count_call(text, r"extc_rc_checkOwed\(__extc_rcc", False)
    srunk = count_call(text, r"extc_rc_storeRefUnknown\(", True)
    # storeStructCons 里 hostKnown=0 的那一档（最后一个实参是 0）也是"目标区域退出清扫"
    srcons0 = len(re.findall(r"extc_rc_storeStructCons\([^;]*?,\s*0\);", text))

    if alloc <= 0:
        cls = "no-alloc"
    elif trig > 0:
        cls = "ok-trigger"
    elif owe > 0 and check > 0:
        cls = "owe-settled"
    elif owe > 0:
        cls = "owe-no-settle"
    else:
        cls = "silent-both"

    bare = bare_writes(text)
    return {
        "path": path, "compile": "ok", "alloc": alloc, "trig": trig, "owe": owe,
        "check": check, "srunk": srunk, "srcons0": srcons0, "class": cls,
        "bare": len(bare), "bare_samples": bare[:3],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--gate", action="store_true")
    ap.add_argument("--out", default=os.path.join(ROOT, "tests/reclaim-gaps/out"))
    ap.add_argument("--extc", default=os.path.join(ROOT, "build/extc"))
    ap.add_argument("--selftest", action="store_true",
                    help="验证 ② 的检测器有牙：往生成物里注入一条裸写入，必须被检出")
    args = ap.parse_args()

    if args.selftest:
        # 判据有牙的证据：干净生成物 0 条；把一条 storeRefOwned 换成裸写入后必须被检出。
        src = os.path.join(ROOT, "tests/reclaim-gaps/shapes/slot_elem_known.extc")
        with tempfile.TemporaryDirectory(prefix="rg-self-") as tmp:
            out_c = os.path.join(tmp, "sek.c")
            subprocess.run([args.extc, "-w", src, "-o", out_c], cwd=ROOT, check=True,
                           capture_output=True, timeout=60)
            text = open(out_c, errors="replace").read()
        clean = bare_writes(text)
        tgt = next((ln for ln in text.splitlines()
                    if "extc_rc_storeRefOwned" in ln and "kids.data" in ln), None)
        if tgt is None:
            print("selftest: 找不到可注入的 storeRefOwned 行 ⇒ 生成物形状变了")
            return 2
        doctored = text.replace(tgt, "        s->kids.data[0] = keeper;")
        got = bare_writes(doctored)
        okk = (len(clean) == 0 and len(got) == 1)
        print(f"selftest: 干净生成物候选 = {len(clean)}，注入一条裸写入后候选 = {len(got)} "
              f"⇒ {'ok（判据有牙）' if okk else 'FAIL'}")
        return 0 if okk else 1

    files = []
    for d in CORPUS:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dp, _dn, fn in os.walk(base):
            for f in sorted(fn):
                if f.endswith(".extc"):
                    files.append(os.path.relpath(os.path.join(dp, f), ROOT))
    files.sort()
    if args.limit:
        files = files[:args.limit]

    rows = []
    with tempfile.TemporaryDirectory(prefix="rg-audit-") as tmp:
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = [ex.submit(scan, p, args.extc, tmp) for p in files]
            for i, fu in enumerate(cf.as_completed(futs), 1):
                rows.append(fu.result())
                if i % 200 == 0:
                    print(f"  … {i}/{len(files)}", file=sys.stderr)

    ok = [r for r in rows if r["compile"] == "ok"]
    classes = {}
    for r in ok:
        classes.setdefault(r["class"], []).append(r)
    red1 = classes.get("silent-both", []) + classes.get("owe-no-settle", [])
    red2 = [r for r in ok if r["bare"] > 0]

    os.makedirs(args.out, exist_ok=True)
    tsv = os.path.join(args.out, "audit-static.tsv")
    with open(tsv, "w") as f:
        f.write("path\tcompile\tclass\talloc\ttrig\towe\tcheck\tsrUnknown\tsrCons0\tbare_ref_writes\n")
        for r in sorted(rows, key=lambda x: x["path"]):
            if r["compile"] != "ok":
                f.write(f"{r['path']}\t{r['compile']}\t-\t-\t-\t-\t-\t-\t-\t-\n")
            else:
                f.write(f"{r['path']}\t{r['compile']}\t{r['class']}\t{r['alloc']}\t{r['trig']}\t"
                        f"{r['owe']}\t{r['check']}\t{r['srunk']}\t{r['srcons0']}\t{r['bare']}\n")

    lines = []
    lines.append("== 全语料静态审计（生成物，EXTC_RECLAIM=1）==")
    lines.append(f"扫描 {len(files)} 个 .extc；编译成功 {len(ok)}；失败 {len(rows)-len(ok)}")
    lines.append("")
    lines.append("① 区域回收者分类（程序数）：")
    for k in ["ok-trigger", "owe-settled", "owe-no-settle", "silent-both", "no-alloc"]:
        lines.append(f"   {k:<16} {len(classes.get(k, [])):>5}")
    lines.append(f"   红计数①（silent-both + owe-no-settle）= {len(red1)}")
    lines.append("")
    lines.append("① 明细（silent-both / owe-no-settle，最多 60 条）：")
    for r in red1[:60]:
        lines.append(f"   [{r['class']:<14}] alloc={r['alloc']:<4} trig={r['trig']} owe={r['owe']} "
                     f"check={r['check']} srUnknown={r['srunk']}  {r['path']}")
    if len(red1) > 60:
        lines.append(f"   … 还有 {len(red1)-60} 条（完整见 audit-static.tsv）")
    lines.append("")
    lines.append(f"② 引用叶子字段的**裸写入候选**：{len(red2)} 个程序（启发式，样本如下）")
    for r in sorted(red2, key=lambda x: -x["bare"])[:25]:
        lines.append(f"   bare={r['bare']:<4} alloc={r['alloc']:<4} trig={r['trig']}  {r['path']}")
        for s in r["bare_samples"]:
            lines.append(f"        {s}")
    lines.append("")
    lines.append(f"原始 TSV → {tsv}")
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(args.out, "audit-static-summary.txt"), "w") as f:
        f.write(text + "\n")

    if args.gate and (len(red1) > 0 or len(red2) > 0):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
