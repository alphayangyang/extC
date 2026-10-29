#!/usr/bin/env bash
# bench/fortran/run.sh —— extC / C / gfortran 三臂横评：构建、逐字节对拍、计时，
# 生成 bench/fortran/raw.txt 并调 report.py 写出 bench/fortran/RESULTS.md。
#
# 用法：
#   bash bench/fortran/run.sh                    # 默认口径：-O2 主表 + -O3 旁证
#   RUNS=5 bash bench/fortran/run.sh             # 多跑几轮取 best
#   SIZES="512 1024" ITERS=100 bash bench/fortran/run.sh
#
# 可覆盖环境变量：EXTC、CC、FC、RUNS、PIN、ITERS、SIZES。
# 口径与判据见同目录 SPEC.md。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dir=bench/fortran
cd "$root"

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
FC=${FC:-gfortran}
RUNS=${RUNS:-5}
PIN=${PIN:-6}
ITERS=${ITERS:-200}
SIZES=${SIZES:-"384 768 1536"}

die() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$EXTC" ] || die "找不到 extC 前端：$EXTC（先 make build）"
command -v "$CC" >/dev/null 2>&1 || die "找不到 C 编译器：$CC"
command -v "$FC" >/dev/null 2>&1 || die "找不到 Fortran 编译器：$FC"
command -v python3 >/dev/null 2>&1 || die "找不到 python3"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/bench_fortran.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "== 构建三臂"
"$EXTC" -w --no-line-map -o "$tmp/stencil.extc.c" "$dir/stencil.extc" \
    || die "extC 前端编译失败"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/extc.O2" "$tmp/stencil.extc.c" || die "编译 extc.O2 失败"
"$CC" -O3 -march=native -fwrapv -o "$tmp/extc.O3" "$tmp/stencil.extc.c" || die "编译 extc.O3 失败"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/c.O2" "$dir/stencil.c" || die "编译 c.O2 失败"
"$CC" -O3 -march=native -fwrapv -o "$tmp/c.O3" "$dir/stencil.c" || die "编译 c.O3 失败"
"$FC" -O2 -o "$tmp/fortran.O2" "$dir/stencil.f90" || die "编译 fortran.O2 失败"
"$FC" -O3 -march=native -o "$tmp/fortran.O3" "$dir/stencil.f90" || die "编译 fortran.O3 失败"

echo "== 向量化报告（-O3 档，仅作旁证的机制依据）"
vec_extc=$("$CC" -O3 -march=native -fwrapv -fopt-info-vec -c -o "$tmp/vec_extc.o" \
    "$tmp/stencil.extc.c" 2>&1 | grep -c "loop vectorized" || true)
vec_c=$("$CC" -O3 -march=native -fwrapv -fopt-info-vec -c -o "$tmp/vec_c.o" \
    "$dir/stencil.c" 2>&1 | grep -c "loop vectorized" || true)
VEC_EXTC=${vec_extc:-0}
VEC_C=${vec_c:-0}
echo "   -fopt-info-vec 里 loop vectorized：extC 生成 C $VEC_EXTC 处，手写 C $VEC_C 处"

echo "== 逐字节对拍（等价性先于性能）"
for exe in "$tmp/extc.O2" "$tmp/c.O2" "$tmp/fortran.O2" \
           "$tmp/extc.O3" "$tmp/c.O3" "$tmp/fortran.O3"; do
    [[ $(basename "$exe") == extc.O2 ]] && continue
    ref=$("$tmp/extc.O2" 64 10) || die "extc.O2 运行失败"
    got=$("$exe" 64 10) || die "$exe 运行失败"
    [ "$got" = "$ref" ] || die "对齐失败：$(basename "$exe") 输出 '$got'，extC 是 '$ref'"
    got=$("$exe" 1536 2) || die "$exe 大网格运行失败"
    [ "$got" = "cs=1109534680147 n=1536" ] \
        || die "对齐失败：$(basename "$exe") 大网格输出 '$got'"
done
echo "   三臂 (64,10) / (1536,2) stdout 逐字节一致"

export EXTC CC FC RUNS ITERS SIZES PIN VEC_EXTC VEC_C
echo "== 计时（RUNS=$RUNS，pin=${PIN:-无}，iters=$ITERS，n=$SIZES）"
python3 - "$tmp" "$root" <<'PY'
import os
import re
import subprocess
import sys
import time

tmp, root = sys.argv[1], sys.argv[2]
level_flags = {
    "O2": ("-O2 -std=c11 -fwrapv", "-O2"),
    "O3": ("-O3 -march=native -fwrapv", "-O3 -march=native"),
}
arms = ["extc", "c", "fortran"]
runs = int(os.environ["RUNS"])
iters = int(os.environ["ITERS"])
sizes = [int(x) for x in os.environ["SIZES"].split()]
pin = os.environ.get("PIN", "").strip()

def pfx():
    return ["taskset", "-c", pin] if pin else []

def timed(exe, n):
    cmd = pfx() + [exe, str(n), str(iters)]
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True)
    dt = (time.perf_counter() - t0) * 1e3
    if p.returncode != 0:
        sys.exit(f"FAIL: {exe} {n} 退出码 {p.returncode}: {p.stderr[:200]}")
    return dt, p.stdout

def rss_kb(exe, n):
    timer = "/usr/bin/time"
    if not os.path.exists(timer):
        return 0
    cmd = pfx() + [timer, "-v", exe, str(n), str(iters)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", p.stderr)
    return int(m.group(1)) if m else 0

rows = []
for level in ("O2", "O3"):
    for n in sizes:
        exp = None
        for arm in arms:
            exe = os.path.join(tmp, f"{arm}.{level}")
            timed(exe, n)  # 预热一次（冷启动，页表/文件缓存）
            best, out = 1e18, None
            for _ in range(runs):
                dt, out = timed(exe, n)
                best = min(best, dt)
            cs = re.search(r"cs=(-?\d+)", out).group(1)
            if exp is None:
                exp = cs
            elif cs != exp:
                sys.exit(f"FAIL: {arm} {level} n={n} cs={cs} 与 extC 的 {exp} 不一致")
            rss = rss_kb(exe, n)
            rows.append((arm, level, n, best, rss, int(cs)))
            print(f"   {level} n={n:5d} {arm:8s} {best:9.2f} ms  rss={rss:7d} kB")

machine = re.search(r"model name\s*:\s*(.+)", open("/proc/cpuinfo").read()).group(1).strip()
def ver(cmd):
    try:
        return subprocess.run([cmd, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        return cmd

lines = [
    "bench/fortran/raw.txt —— 由 run.sh 生成，别手改（原始记录，RESULTS.md 由 report.py 从本文件生成）",
    f"machine\t{machine}",
    f"kernel\t{os.uname().sysname} {os.uname().release}",
    f"nproc\t{os.cpu_count()}",
    f"gcc\t{ver(os.environ['CC'])}",
    f"gfortran\t{ver(os.environ['FC'])}",
    f"extc\t{os.environ['EXTC']}",
    f"pin\t{pin or '(none)'}",
    f"iters\t{iters}",
    f"runs\t{runs}",
    f"sizes\t{' '.join(map(str, sizes))}",
    f"flags_O2\textc/c: {level_flags['O2'][0]} · fortran: {level_flags['O2'][1]}",
    f"flags_O3\textc/c: {level_flags['O3'][0]} · fortran: {level_flags['O3'][1]}",
    f"vec_O3_extc\t{os.environ.get('VEC_EXTC', '0')}",
    f"vec_O3_c\t{os.environ.get('VEC_C', '0')}",
]
for arm, level, n, ms, rss, cs in rows:
    lines.append(f"data\t{arm}\t{level}\t{n}\t{ms:.3f}\t{rss}\t{cs}")
open(os.path.join(root, "bench/fortran/raw.txt"), "w").write("\n".join(lines) + "\n")
print("== raw.txt 已写入")
PY

echo "== 生成 RESULTS.md"
python3 "$dir/report.py"
echo "== 完成：$dir/RESULTS.md"
