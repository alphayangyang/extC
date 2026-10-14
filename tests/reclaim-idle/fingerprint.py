#!/usr/bin/env python3
"""idle 发射行为的语料指纹：`freeze` 冻结基线，`check` 逐行比对。

idle 证明（"这个函数整段不需要回收运行期"）的判据在编译器里。重构它时，"没改变发射行为"
是一句**主张**；本工具把它变成可复跑的事实：同一份语料、同一把尺，重构前后逐行相同。

    python3 tests/reclaim-idle/fingerprint.py freeze   # 写 tools/idle-fingerprint-baseline.tsv
    python3 tests/reclaim-idle/fingerprint.py check    # 与基线逐行比对，任何一格不同 ⇒ exit 1

**编译器与源码树可选**（`--extc` / `--tree` / `--stdlib`）：基线必须钉在**重构前**的编译器上，
重构后再拿主树的编译器重跑同一口径。`--tree` 决定语料根与相对路径的基准目录；`--stdlib`
默认跟着编译器走（见下），只在编译器被搬运过时才需要显式给。

口径（详见同目录 README.md）：

1. 语料集合 = `tests/reclaim-perf/taxmap.py` 的 census 口径：roots `examples/` · `tests/` ·
   `bench/`，跳过 `archive` / `build` / `in` / `in-small` / `out` / `stamped` / `__pycache__`，
   每个 `*.extc` 一行，按仓库相对路径排序。
2. 每行六列（TAB 分列）：
     path · ok · md5_default · md5_reclaim0 · same · rc_syms
   `ok` = 默认档编译结果（OK / FAIL / TIMEOUT）；
   `md5_default` / `md5_reclaim0` = 两档生成的 C 的 md5（未编译出记 `-`）；
   `same` = 两档生成物是否逐字节相同（YES / NO，等价于"该程序不发射回收运行期"）；
   `rc_syms` = 默认档生成 C 里 `extc_rc_*` 标识符的排序去重集合（逗号连接）。
3. 头部记 `build/extc` 的 md5 与 `HEAD`（`--head` 可覆盖，用于没有 `.git` 的解包树）。
   **会漂的绑定写进产物，不写进散文。**

为什么用"生成物 md5 + rc 符号集"而不是诊断文案：`EXTC_DBG_RCIDLE` 之类的诊断输出是给人看的，
措辞与取值随时会变；判 idle 的**可观测后果**只有两件 —— 生成物变不变、发射了哪些运行期入口。
前者是逐字节的，后者是集合。诊断文案不进指纹。

确定性：在源码树根目录下用**相对路径**调用编译器（生成的 C 里内嵌源文件路径，绝对路径会让
指纹绑定 checkout 位置），并以 `-w --no-line-map` 生成；环境里继承来的 `EXTC_*` 全部剥掉
（调试/分片/档位开关都会改产物），只按档位设 `EXTC_RECLAIM=0`。

**`-I` 必须是编译器自己的 stdlib（绝对路径）**：相对的模块目录会被编译器按源码文件所在目录
解析（实测：`-I stdlib` 让 stdlib 的 `@builtin` 声明被误判成"非标准库"，同一文件由 OK 变 FAIL）；
而编译器认定"哪些文件是标准库"用的是 `<extc 所在目录>/../stdlib`（`/proc/self/exe`），
所以 `-I` 指到**别的树**的 stdlib 会让那棵树的标准库整片变成普通程序文件 —— 本工具因此
默认 `--stdlib = <extc>/../stdlib`，并显式设 `EXTC_STD` 把两侧钉成同一个根。
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE = os.path.join("tools", "idle-fingerprint-baseline.tsv")
TMP = "/tmp/reclaim-idle-fp"

# census 口径：与 `tests/reclaim-perf/taxmap.py` 的 CENSUS_ROOTS / CENSUS_SKIP_DIRS 一致。
CENSUS_ROOTS = ("examples", "tests", "bench")
CENSUS_SKIP_DIRS = {"archive", "build", "in", "in-small", "out", "stamped", "__pycache__"}

COLUMNS = ["path", "ok", "md5_default", "md5_reclaim0", "same", "rc_syms"]
RC_SYM = re.compile(r"extc_rc_[A-Za-z0-9_]+")
TIMEOUT_S = 120
KV = re.compile(r"([A-Za-z0-9_./-]+)=(\S+)")


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_head(tree):
    p = subprocess.run(["git", "-C", tree, "rev-parse", "--short", "HEAD"],
                       capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else "?"


def resolve(args):
    """把 `--tree` / `--extc` / `--stdlib` / `--head` 收成一组具体路径与绑定。"""
    tree = os.path.abspath(args.tree)
    extc = args.extc or os.path.join("build", "extc")
    extc = os.path.abspath(extc if os.path.isabs(extc) else os.path.join(tree, extc))
    # 编译器用 `<extc 所在目录>/../stdlib` 认定"哪些文件是标准库"（`/proc/self/exe`）。
    # `-I` 指到别处的 stdlib 会让同一份 stdlib 被当成普通程序文件（`@builtin` 报错）——
    # 所以默认跟着编译器走；`--stdlib` 只在编译器被搬运过时用。
    stdlib = os.path.abspath(args.stdlib) if args.stdlib else \
        os.path.normpath(os.path.join(os.path.dirname(extc), "..", "stdlib"))
    return tree, extc, stdlib, args.head or git_head(tree)


def binding(tree, extc, stdlib, head):
    """当前绑定：编译器 md5、编译器路径、编译器的 stdlib、源码树、HEAD。"""
    return {"md5": md5_file(extc) if os.path.exists(extc) else "missing",
            "HEAD": head, "extc": extc, "stdlib": stdlib, "tree": tree}


def find_sources(tree, only=()):
    out = []
    for root in CENSUS_ROOTS:
        base = os.path.join(tree, root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in CENSUS_SKIP_DIRS]
            for fn in sorted(filenames):
                if fn.endswith(".extc"):
                    rel = os.path.relpath(os.path.join(dirpath, fn), tree)
                    if not only or any(s in rel for s in only):
                        out.append(rel)
    return sorted(out)


def clean_env(reclaim_off, stdlib):
    """继承环境去掉全部 `EXTC_*`：调试/分片/档位开关都会改产物，指纹只许动一个旋钮。

    `EXTC_STD` 例外：它显式钉住 stdlib 根，好让"标准库文件"的认定与 `-I <stdlib>` 一致
    （编译器默认按 `/proc/self/exe` 找 `<extc>/../stdlib`；编译器被搬运过时两者会分叉）。
    默认情况下 `stdlib` 就是编译器自己的 stdlib，这一项是 no-op。
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("EXTC_")}
    env["EXTC_STD"] = stdlib
    if reclaim_off:
        env["EXTC_RECLAIM"] = "0"
    return env


def compile_one(tree, extc, stdlib, rel, reclaim_off, out_path):
    """编译一个源文件到 `out_path`，返回 (状态, md5, rc 符号集)。

    状态：OK / FAIL（编译器非零退出）/ TIMEOUT（超过每文件限额）。
    rc 符号集只对默认档取（关档的产物里没有回收运行期）。
    """
    argv = [extc, "-w", "--no-line-map", "-I", stdlib, rel, "-o", out_path]
    try:
        p = subprocess.run(argv, cwd=tree, capture_output=True, text=True,
                           env=clean_env(reclaim_off, stdlib), timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", None, None
    if p.returncode != 0:
        return "FAIL", None, None
    with open(out_path, "rb") as f:
        data = f.read()
    digest = hashlib.md5(data).hexdigest()
    if reclaim_off:
        return "OK", digest, None
    syms = ",".join(sorted(set(RC_SYM.findall(data.decode("utf-8", "replace")))))
    return "OK", digest, syms


def fingerprint(tree, extc, stdlib, rel, keep):
    """一个源文件的五列（`path` 之后的部分）。"""
    if keep:
        stem = rel.replace("/", "_")[:-5]
        on_path = os.path.join(TMP, stem + ".on.c")
        off_path = os.path.join(TMP, stem + ".off.c")
    else:
        on_path = os.path.join(TMP, "on.c")
        off_path = os.path.join(TMP, "off.c")

    status, on_md5, syms = compile_one(tree, extc, stdlib, rel, False, on_path)
    if status != "OK":
        return [status, "-", "-", "-", "-"]

    off_status, off_md5, _ = compile_one(tree, extc, stdlib, rel, True, off_path)
    if off_status != "OK":
        return ["OK", on_md5, "-", "-", syms or "-"]
    same = "YES" if on_md5 == off_md5 else "NO"
    return ["OK", on_md5, off_md5, same, syms or "-"]


def scan(tree, extc, stdlib, sources, keep=False, progress=True):
    os.makedirs(TMP, exist_ok=True)
    rows = {}
    t0 = time.time()
    for i, rel in enumerate(sources, 1):
        rows[rel] = fingerprint(tree, extc, stdlib, rel, keep)
        if progress and (i % 200 == 0 or i == len(sources)):
            print(f"  … {i}/{len(sources)} ({time.time() - t0:.0f}s)", file=sys.stderr)
    return rows


def render(meta, rows):
    lines = ["# reclaim-idle fingerprint v1",
             f"# build/extc md5={meta['md5']} HEAD={meta['HEAD']}",
             f"# extc={meta['extc']} tree={meta['tree']} stdlib={meta['stdlib']}",
             "# 档位：默认 vs EXTC_RECLAIM=0；生成参数 -w --no-line-map -I <stdlib>",
             f"# 语料：roots={'/'.join(CENSUS_ROOTS)}，跳过 "
             + "/".join(sorted(CENSUS_SKIP_DIRS)) + "（taxmap.py census 口径）",
             "# rc_syms = 默认档生成 C 里 extc_rc_* 的排序去重集合（逗号连接）",
             "\t".join(COLUMNS)]
    for rel in sorted(rows):
        lines.append("\t".join([rel] + rows[rel]))
    return "\n".join(lines) + "\n"


def parse_tsv(path):
    meta, columns, rows = {}, None, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                meta.update(dict(KV.findall(line)))
                continue
            parts = line.split("\t")
            if parts[0] == "path":
                columns = parts
                continue
            rows[parts[0]] = parts[1:]
    return meta, columns, rows


def tally(rows):
    c = {"OK": 0, "FAIL": 0, "TIMEOUT": 0, "YES": 0, "NO": 0}
    for r in rows.values():
        c[r[0]] = c.get(r[0], 0) + 1
        if r[3] in ("YES", "NO"):
            c[r[3]] += 1
    return c


def fmt_counts(counts):
    return (f"OK {counts['OK']} · FAIL {counts['FAIL']} · TIMEOUT {counts['TIMEOUT']} · "
            f"same=YES {counts['YES']} · same=NO {counts['NO']}")


def cmd_freeze(args):
    tree, extc, stdlib, head = resolve(args)
    sources = find_sources(tree, args.only)
    rows = scan(tree, extc, stdlib, sources, keep=args.keep)
    text = render(binding(tree, extc, stdlib, head), rows)
    out = os.path.abspath(args.out)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"wrote {out}: {len(rows)} sources ({fmt_counts(tally(rows))})")


def cmd_check(args):
    tree, extc, stdlib, head = resolve(args)
    base_path = os.path.abspath(args.baseline)
    base_meta, base_cols, base = parse_tsv(base_path)
    if base_cols != COLUMNS:
        print(f"SCHEMA: 基线的列是 {base_cols}，本工具是 {COLUMNS}")
        return 1
    if args.only:
        base = {k: v for k, v in base.items() if any(s in k for s in args.only)}

    cur = scan(tree, extc, stdlib, find_sources(tree, args.only), keep=args.keep)
    cur_meta = binding(tree, extc, stdlib, head)

    print(f"baseline: {base_path}")
    print(f"  build/extc md5={base_meta.get('md5', '?')} HEAD={base_meta.get('HEAD', '?')}"
          f" extc={base_meta.get('extc', '?')} stdlib={base_meta.get('stdlib', '?')}"
          f" tree={base_meta.get('tree', '?')}")
    print(f"current : build/extc md5={cur_meta['md5']} HEAD={cur_meta['HEAD']}"
          f" extc={cur_meta['extc']} stdlib={cur_meta['stdlib']} tree={cur_meta['tree']}")
    if (base_meta.get("md5"), base_meta.get("HEAD")) != (cur_meta["md5"], cur_meta["HEAD"]):
        print("NOTE: 编译器绑定不同（重建过编译器即如此）—— 是否零行为变化由下面的**逐行**比对裁决。")

    base_keys, cur_keys = set(base), set(cur)
    removed, added = sorted(base_keys - cur_keys), sorted(cur_keys - base_keys)
    changed = [(p, base[p], cur[p]) for p in sorted(base_keys & cur_keys) if base[p] != cur[p]]

    for p in removed:
        print(f"DIFF {p}: 基线里有、本次扫描没有（file removed or renamed?）")
    for p in added:
        print(f"DIFF {p}: 基线里没有、本次扫描多出来（new file?）")
    for p, old, new in changed:
        print(f"DIFF {p}:")
        for col, ov, nv in zip(COLUMNS[1:], old, new):
            if ov != nv:
                print(f"  {col}: {ov!r} -> {nv!r}")

    print(f"rows: baseline {len(base)} · current {len(cur)} · {fmt_counts(tally(cur))}")
    n_diff = len(removed) + len(added) + len(changed)
    if n_diff:
        print(f"FAIL: {n_diff} 行有差异（removed {len(removed)} · added {len(added)}"
              f" · changed {len(changed)}）")
        return 1
    print("PASS: 逐行相同（每一格）")
    return 0


def add_common(p):
    p.add_argument("--tree", default=REPO,
                   help=f"源码树（语料根 / stdlib / 相对路径基准；默认仓库根 {REPO}）")
    p.add_argument("--extc", default=None,
                   help="编译器二进制（默认 <tree>/build/extc；可为绝对或相对 <tree> 的路径）")
    p.add_argument("--stdlib", default=None,
                   help="模块搜索目录（默认 <extc 所在目录>/../stdlib，即编译器自己的标准库）")
    p.add_argument("--head", default=None,
                   help="写进头部的 HEAD（默认 git -C <tree> rev-parse；无 .git 的解包树要显式给）")
    p.add_argument("--only", action="append", default=[],
                   help="只处理路径含该子串的源文件（调试/最小复现用，可重复）")
    p.add_argument("--keep", action="store_true", help="保留生成的 C 到 " + TMP)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    fz = sub.add_parser("freeze", help="写基线 TSV")
    add_common(fz)
    fz.add_argument("--out", default=BASELINE, help=f"基线路径（默认 {BASELINE}）")
    fz.set_defaults(fn=cmd_freeze)

    ck = sub.add_parser("check", help="重跑并与基线逐行比对；有差异 exit 1")
    add_common(ck)
    ck.add_argument("--baseline", default=BASELINE, help=f"基线路径（默认 {BASELINE}）")
    ck.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
