#!/usr/bin/env python3
"""「计数按编译期收窄」语料（`tests/reclaim-elide/`）的棘轮闸门。

`run.sh` 自身是**基线**（exit 0，只记录现状），所以它的红绿可以悄悄漂移。这里把它的判定表
变成有牙的判据，基线是 `tests/reclaim-elide/red-known-bad.txt`：

  · **新增红** ⇒ 红。两类：①「不该省」的形状丢了计数（= 被误省，健全性方向）；
    ②「该省」的形状按新的口径仍不该省（实现收窄时要动基线）。
  · **基线里已不再红** ⇒ 也红（棘轮只许缩：收窄实现一条、基线就删一条）。
  · **接线未生效**（生成 C 里 0 处计数调用）⇒ 直接 rc=2：那时「该省」的红是"判据还测不到"，
    不是"已省掉"，闸门在这种状态下**不允许绿**。

用法：
    python3 tests/reclaim-elide/gate.py                 # 跑语料再判
    python3 tests/reclaim-elide/gate.py --tsv FILE      # 用现成的判定表判（不重跑）
    python3 tests/reclaim-elide/gate.py --update-baseline
    python3 tests/reclaim-elide/gate.py --baseline FILE # 换一份基线 ⇒ 自检"闸门有牙"
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
RUNNER = HERE / "run.sh"
DEFAULT_BASELINE = HERE / "red-known-bad.txt"
HEADER = (
    "# 「计数按编译期收窄」语料的已知红（基线）。格式：<形状>\t<原因>\t<跟踪>\n"
    "# 规矩：每条红都必须在基线里；基线里已不再红的条目会让闸门变红（棘轮只许缩）。\n"
    "# 形状、为什么该省/不该省见 README.md §「三类该省与三个反面对照」。\n"
)


def read_rows(path: pathlib.Path):
    rows = []
    for ln in path.read_text(encoding="utf-8").splitlines():
        if not ln.strip():
            continue
        f = ln.split("\t")
        if len(f) < 7:
            continue
        rows.append({"name": f[0], "expect": f[1], "counts": f[2],
                     "color": f[3], "verdict": f[4], "note": f[5], "wired": f[6]})
    return rows


def run_corpus() -> pathlib.Path:
    tsv = pathlib.Path(tempfile.mkdtemp(prefix="elide-gate.")) / "verdicts.tsv"
    env = dict(os.environ, ELIDE_TSV=str(tsv))
    p = subprocess.run([str(RUNNER)], cwd=str(ROOT), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not tsv.exists():
        sys.stdout.write(p.stdout[-1500:])
        return None
    return tsv


def read_baseline(path: pathlib.Path):
    names = set()
    for ln in path.read_text(encoding="utf-8").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        names.add(ln.split("\t")[0].split()[0])
    return names


def criterion_selftest() -> bool:
    """先验判据本身有没有牙：`run.sh --selftest` 在合成样本上断言
    "prelude 体内那两行不计、用户区三种发射形状都数到"。口径坏掉的判据会让红绿都失去意义
    （本目录的上一版就是这样：`该省` 永远红、「不该省」被误省也永远绿）。"""
    p = subprocess.run([str(RUNNER), "--selftest"], cwd=str(ROOT),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        sys.stdout.write(p.stdout)
    return p.returncode == 0


def idle_shapes() -> set:
    """哪些形状**整单元不发射回收运行期**（判 idle）。

    判据与语料指纹同一口径：同一个形状在 `EXTC_RECLAIM=1` 与 `EXTC_RECLAIM=0` 下生成物**逐字节
    相同**。**判 idle ⇒ 根本没有计数可省**，本目录的"计数收窄"判据对它不适用（那是另一条判据的
    地盘：`gate-idle-projection` 的红集 + 语料指纹的发射类别）。这样 elide 判据与 idle 判据
    钉的是**同一个事实**的两面，不需要形状排除清单。"""
    idle = set()
    extra = os.environ.get("EXTRA_CFLAGS", "")
    for src in sorted(HERE.glob("*.extc")):
        outs = []
        for rec in ("1", "0"):
            dst = pathlib.Path(tempfile.mkdtemp(prefix="elide-idle.")) / "out.c"
            env = dict(os.environ, EXTC_RECLAIM=rec)
            cmd = [str(ROOT / "build" / "extc"), "-w", "--no-line-map", "-I",
                   str(ROOT / "stdlib"), str(src), "-o", str(dst)]
            if extra:
                cmd += extra.split()
            p = subprocess.run(cmd, cwd=str(ROOT), env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if p.returncode != 0 or not dst.exists():
                outs = []
                break
            outs.append(dst.read_bytes())
        if len(outs) == 2 and outs[0] == outs[1]:
            idle.add(src.stem.replace("_", "-"))
    return idle


def main() -> int:
    ap = argparse.ArgumentParser(description="计数收窄语料的棘轮闸门")
    ap.add_argument("--tsv", help="用现成的判定表，不重跑语料")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--update-baseline", action="store_true")
    a = ap.parse_args()

    if not a.tsv and not criterion_selftest():
        print("[elide] 计数口径自检失败 ⇒ 这张表的红绿不可信，闸门不允许绿")
        return 2

    if a.tsv:
        tsv = pathlib.Path(a.tsv)
        if not tsv.exists():
            print(f"[elide] 找不到 {tsv}")
            return 2
    else:
        tsv = run_corpus()
        if tsv is None:
            print("[elide] 跑语料没拿到判定表")
            return 2

    rows = read_rows(tsv)
    if not rows:
        print(f"[elide] 判定表为空：{tsv}（列数不足 7？）")
        return 2

    # 判 idle 的形状没有计数可省：从"接线"与"红"两侧都排除（理由见 `idle_shapes`）。
    idle = idle_shapes()
    if idle:
        print("[elide] 判 idle（不发射回收运行期，无计数可省）：" + ", ".join(sorted(idle)))

    # 判据必须先"测得到"：接线未生效时"该省"的红是假红。
    not_wired = [r["name"] for r in rows if r["wired"] != "yes" and r["name"] not in idle]
    if not_wired:
        print(f"[elide] 计数接线未生效（{len(not_wired)} 个形状的生成 C 里 0 处计数调用）"
              f"⇒ 判据测不到，不允许绿：" + ", ".join(not_wired))
        return 2

    now = {r["name"] for r in rows if r["verdict"] == "红" and r["name"] not in idle}
    base = read_baseline(pathlib.Path(a.baseline)) if pathlib.Path(a.baseline).exists() else set()
    base = base - idle          # 判 idle 的形状不再属于"计数收窄"这条判据的地盘

    if a.update_baseline:
        pathlib.Path(a.baseline).write_text(
            HEADER + "\n".join(f"{n}\t未实现收窄/计数仍发射\t见 README.md" for n in sorted(now)) + "\n",
            encoding="utf-8")
        print(f"[elide] 基线已更新：{len(now)} 条 → {a.baseline}")
        return 0

    added = sorted(now - base)
    gone = sorted(base - now)
    if added or gone:
        for r in rows:
            if r["name"] in added:
                print(f"[elide] 新红：{r['name']}（期望{r['expect']}；{r['note']}）")
        if gone:
            print(f"[elide] 基线里的 {len(gone)} 条已不再红（棘轮只许缩，请跑 --update-baseline）："
                  + ", ".join(gone))
        return 1

    print(f"[elide] 红 {len(now)} 条，与基线一致（{len(base)} 条；接线已生效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
