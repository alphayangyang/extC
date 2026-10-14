#!/usr/bin/env python3
"""回收缺口矩阵（`tests/reclaim-gaps/`）的棘轮闸门。

`run-gaps.sh` 自己**恒 exit 0**（它只打印矩阵），所以它测出来的"哪些形状收不掉"在没有闸门时
可以悄悄漂移。这里把那 19 行规模判据变成一条有牙的判据，基线是
`tests/reclaim-gaps/gate-known-bad.txt`：

  · 新出现"收不掉"的形状 ⇒ **红**（回收能力退化了）；
  · 基线里已经不再"收不掉"的条目 ⇒ **红**（棘轮只往一个方向转，与 `tools/gate_scale.py`
    同一条规矩：做了优化就必须把基线一起缩掉，否则闸门会一直绿在一个过期基线上）。

用法：
    python3 tests/reclaim-gaps/gate.py                  # 跑一遍矩阵（约 1 分钟）再判
    python3 tests/reclaim-gaps/gate.py --scale FILE     # 用现成的 scale.tsv 判，不重跑
    python3 tests/reclaim-gaps/gate.py --update-baseline
    python3 tests/reclaim-gaps/gate.py --baseline FILE  # 换一份基线 ⇒ 自检"闸门有牙"

判据本身的口径（N vs 4N 峰值比 < 1.5 = 收得掉）写在 `run-gaps.sh` 与 `README.md`；
本文件只比较"谁收不掉"与"谁在 sanitizer 下不干净"，不重复口径。
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_BASELINE = HERE / "gate-known-bad.txt"
ASAN_BASELINE = HERE / "asan-known-bad.txt"
RUNNER = HERE / "run-gaps.sh"
RED_MARK = "收不掉"
HEADER = (
    "# 回收缺口矩阵的已知坏（基线）。格式：<形状名>\t<原因>\t<跟踪>\n"
    "# 规矩：每个\"收不掉\"的形状都必须在基线里；基线里已经不再\"收不掉\"的条目会让闸门变红\n"
    "#       （棘轮只往一个方向转）。形状与数字见 README.md 的 §1 矩阵。\n"
)
ASAN_HEADER = (
    "# 回收缺口形状的 sanitizer 已知坏（基线）。格式：<形状>\t<结论>\t<跟踪>\n"
    "# 规矩：任何 `asan=<非 0>` 都必须在基线里；基线里已不再失败的条目会让闸门变红（只许缩）。\n"
    "# 当前**没有已知坏**（README §7：所有 ASan/UBSan 跑都是 rc=0、无 SEGV/UAF/runtime error）。\n"
)


def read_baseline(path: pathlib.Path) -> set:
    names = set()
    for ln in path.read_text(encoding="utf-8").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        names.add(ln.split("\t")[0].split()[0])
    return names


def red_set(scale: pathlib.Path) -> set:
    """scale.tsv 的最后一列含"收不掉" ⇒ 这条形状收不掉。"""
    out = set()
    for ln in scale.read_text(encoding="utf-8").splitlines():
        f = ln.split("\t")
        if len(f) < 2 or f[0] in ("", "形状"):
            continue
        if RED_MARK in f[-1]:
            out.add(f[0])
    return out


def asan_failures(raw: pathlib.Path) -> list:
    """`raw.txt` 里每个形状的 ASan/UBSan 结论：只有 `asan=0` 算干净。

    这些形状本来就是"收不掉"的（界随规模涨），但**悬垂/越界**绝不能有 —— 本闸门把它从
    "只打印的观察值"变成断言：任何 `asan=<非 0>`（或带报告头 `[...]`）都算失败。
    """
    out = []
    name = "?"
    for ln in raw.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.startswith("### "):
            name = ln[4:].split()[0]
        elif ln.startswith("--- asan:"):
            note = ln[len("--- asan:"):].strip()
            if note != "asan=0":
                out.append((name, note))
    return out


def run_matrix():
    """跑一遍矩阵，返回 (scale.tsv 路径, raw.txt 路径, runner 输出)；没产出时 scale 为 None。"""
    outdir = tempfile.mkdtemp(prefix="gaps-gate.")
    env = dict(os.environ, OUTDIR=outdir)
    p = subprocess.run([str(RUNNER)], cwd=str(ROOT), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    scale = pathlib.Path(outdir) / "scale.tsv"
    if not scale.exists():
        return None, None, p.stdout
    return scale, pathlib.Path(outdir) / "raw.txt", p.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description="回收缺口矩阵的棘轮闸门")
    ap.add_argument("--scale", help="用现成的 scale.tsv，不重跑矩阵")
    ap.add_argument("--raw", help="用现成的 raw.txt（ASan 结论在里面；默认与 --scale 同目录）")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--asan-baseline", default=str(ASAN_BASELINE))
    ap.add_argument("--update-baseline", action="store_true",
                    help="把当前\"收不掉\"集合写回基线（做优化、缺口变少之后才用）")
    ap.add_argument("--update-asan-baseline", action="store_true",
                    help="把当前 sanitizer 失败的形状写回 ASan 基线")
    a = ap.parse_args()

    baseline_path = pathlib.Path(a.baseline)
    raw = None
    if a.scale:
        scale = pathlib.Path(a.scale)
        if not scale.exists():
            print(f"[reclaim-gaps] 找不到 {scale}")
            return 2
        raw = pathlib.Path(a.raw) if a.raw else scale.parent / "raw.txt"
    else:
        scale, raw, out = run_matrix()
        if scale is None:
            print("[reclaim-gaps] 跑矩阵没拿到 scale.tsv")
            sys.stdout.write(out[-2000:])
            return 2

    # ---- ① sanitizer：任何 `asan=<非 0>` 都算失败（棘轮，两个方向都红）----
    asan_path = pathlib.Path(a.asan_baseline)
    asan_now = asan_failures(raw) if raw and raw.exists() else None
    if asan_now is None:
        if not a.scale:
            print(f"[reclaim-gaps] 跑完矩阵却没拿到 {raw} ⇒ 无法判 sanitizer（不是跳过）")
            return 2
        print(f"[reclaim-gaps] 找不到 {raw} ⇒ 跳过 sanitizer 这一段（只有 scale 时无法判）")
    else:
        asan_now_names = {n for n, _ in asan_now}
        if a.update_asan_baseline:
            rows = [f"{n}\t{nl}\t见 README.md §1" for n, nl in sorted(asan_now)]
            asan_path.write_text(ASAN_HEADER + "\n".join(rows) + "\n", encoding="utf-8")
            print(f"[reclaim-gaps] sanitizer 基线已更新：{len(rows)} 条 → {asan_path}")
            if not a.update_baseline:
                return 0
        else:
            asan_base = read_baseline(asan_path) if asan_path.exists() else set()
            asan_new = sorted(asan_now_names - asan_base)
            asan_stale = sorted(asan_base - asan_now_names)
            if asan_new or asan_stale:
                for n, nl in sorted(asan_now):
                    if n in asan_new:
                        print(f"[reclaim-gaps] sanitizer 新失败：{n}（{nl}）")
                if asan_stale:
                    print(f"[reclaim-gaps] sanitizer 基线里的 {len(asan_stale)} 条已不再失败"
                          f"（棘轮只许缩，请跑 --update-asan-baseline）：" + ", ".join(asan_stale))
                return 1
            print(f"[reclaim-gaps] sanitizer：{len(asan_now_names)} 条失败，与基线一致"
                  f"（{len(asan_base)} 条）")

    # ---- ② 缺口棘轮：收不掉的集合（两个方向都红）----
    now = red_set(scale)
    if a.update_baseline:
        rows = [f"{n}\t收不掉(随规模涨)\t见 README.md §1" for n in sorted(now)]
        baseline_path.write_text(HEADER + "\n".join(rows) + "\n", encoding="utf-8")
        print(f"[reclaim-gaps] 基线已更新：{len(now)} 条 → {baseline_path}")
        return 0

    if not baseline_path.exists():
        print(f"[reclaim-gaps] 基线不存在：{baseline_path}（先 --update-baseline）")
        return 2

    base = read_baseline(baseline_path)
    new = sorted(now - base)
    stale = sorted(base - now)
    if new or stale:
        if new:
            print(f"[reclaim-gaps] 新出现的缺口 {len(new)} 条（收不掉的形状变多了）："
                  + ", ".join(new))
        if stale:
            print(f"[reclaim-gaps] 基线里的 {len(stale)} 条已不再\"收不掉\"（棘轮只许缩，"
                  f"请跑 --update-baseline）：" + ", ".join(stale))
        return 1

    print(f"[reclaim-gaps] 收不掉 {len(now)} 条，与基线一致（无新增、无过期）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
