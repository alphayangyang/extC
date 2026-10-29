#!/usr/bin/env bash
# bench/par/run.sh —— 数据并行（extC `par`）基准：构建、逐字节对拍、计时，
# 生成 bench/par/raw.txt 并调 report.py 写出 bench/par/RESULTS.md。
#
# 用法：
#   bash bench/par/run.sh                 # 默认口径：n=1<<18，RUNS=5
#   RUNS=3 SIZES="131072 262144" bash bench/par/run.sh
#
# 可覆盖环境变量：EXTC、CC、RUNS、PIN、SIZES、THREADS。
# 口径与判据见同目录 SPEC.md。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dir=bench/par
cd "$root"

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
RUNS=${RUNS:-5}
PIN=${PIN:-6-13}
SIZES=${SIZES:-"262144 1048576"}
THREADS=${THREADS:-"1 2 4 8"}
ITERS=${ITERS:-48}

die() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$EXTC" ] || die "找不到 extC 前端：$EXTC（先 make build）"
command -v "$CC" >/dev/null 2>&1 || die "找不到 C 编译器：$CC"
command -v python3 >/dev/null 2>&1 || die "找不到 python3"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/bench_par.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "== 构建各臂"
"$EXTC" -w --no-line-map -o "$tmp/par.extc.c" "$dir/par.extc" || die "extC 前端编译 par.extc 失败"
"$EXTC" -w --no-line-map -o "$tmp/seq.extc.c" "$dir/seq.extc" || die "extC 前端编译 seq.extc 失败"
"$CC" -O2 -std=c11 -fwrapv -pthread -o "$tmp/par" "$tmp/par.extc.c" || die "编译 par 失败"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/seq" "$tmp/seq.extc.c" || die "编译 seq 失败"
"$CC" -O2 -std=c11 -fwrapv -o "$tmp/c" "$dir/par.c" || die "编译 c 失败"

echo "== 逐字节对拍（等价性先于性能）"
ref=$("$tmp/seq" 4096 "$ITERS") || die "seq 运行失败"
for exe in "$tmp/par" "$tmp/c"; do
    got=$("$exe" 4096 "$ITERS") || die "$(basename "$exe") 运行失败"
    [ "$got" = "$ref" ] || die "对齐失败：$(basename "$exe") 输出 '$got'，seq 是 '$ref'"
done
for t in $THREADS; do
    got=$(EXTC_PAR_THREADS=$t "$tmp/par" 4096 "$ITERS") || die "par($t 线程) 运行失败"
    [ "$got" = "$ref" ] || die "对齐失败：par($t) 输出 '$got'，seq 是 '$ref'"
done
echo "   seq / par(1,2,4,8) / c 在 n=4096 上 stdout 逐字节一致：$ref"

export EXTC CC RUNS PIN SIZES THREADS ITERS
echo "== 计时（RUNS=$RUNS，pin=$PIN，n=$SIZES）"
python3 - "$tmp" "$root" <<'PY'
import os
import re
import subprocess
import sys
import time

tmp, root = sys.argv[1], sys.argv[2]
runs = int(os.environ["RUNS"])
sizes = [int(x) for x in os.environ["SIZES"].split()]
threads = [int(x) for x in os.environ["THREADS"].split()]
pin = os.environ.get("PIN", "").strip()

def pfx():
    return ["taskset", "-c", pin] if pin else []

def timed(exe, n, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    t0 = time.perf_counter()
    p = subprocess.run(pfx() + [exe, str(n), os.environ["ITERS"]], capture_output=True, text=True, env=env)
    dt = (time.perf_counter() - t0) * 1e3
    if p.returncode != 0:
        sys.exit(f"FAIL: {exe} n={n} 退出码 {p.returncode}: {p.stderr[:200]}")
    return dt, p.stdout

def rss_kb(exe, n, env_extra):
    timer = "/usr/bin/time"
    if not os.path.exists(timer):
        return 0
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.run(pfx() + [timer, "-v", exe, str(n), os.environ["ITERS"]], capture_output=True, text=True, env=env)
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", p.stderr)
    return int(m.group(1)) if m else 0

arms = [("seq", os.path.join(tmp, "seq"), {})]
for t in threads:
    arms.append((f"par{t}", os.path.join(tmp, "par"), {"EXTC_PAR_THREADS": str(t)}))
arms.append(("c", os.path.join(tmp, "c"), {}))

rows = []
for n in sizes:
    exp = None
    for name, exe, env_extra in arms:
        timed(exe, n, env_extra)  # 预热一次
        best, out = 1e18, None
        for _ in range(runs):
            dt, out = timed(exe, n, env_extra)
            best = min(best, dt)
        cs = re.search(r"cs=(-?\d+)", out).group(1)
        if exp is None:
            exp = cs
        elif cs != exp:
            sys.exit(f"FAIL: {name} n={n} cs={cs} 与 seq 的 {exp} 不一致")
        rss = rss_kb(exe, n, env_extra)
        rows.append((name, n, best, rss, int(cs)))
        print(f"   n={n:7d} {name:6s} {best:9.2f} ms  rss={rss:7d} kB")

machine = re.search(r"model name\s*:\s*(.+)", open("/proc/cpuinfo").read()).group(1).strip()
def first_line(cmd):
    try:
        return subprocess.run([cmd, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        return cmd

lines = [
    "bench/par/raw.txt —— 由 run.sh 生成，别手改（原始记录，RESULTS.md 由 report.py 从本文件生成）",
    f"machine\t{machine}",
    f"kernel\t{os.uname().sysname} {os.uname().release}",
    f"nproc\t{os.cpu_count()}",
    f"gcc\t{first_line(os.environ['CC'])}",
    f"extc\t{os.environ['EXTC']}",
    f"pin\t{pin or '(none)'}",
    f"runs\t{runs}",
    f"threads\t{' '.join(map(str, threads))}",
    f"sizes\t{' '.join(map(str, sizes))}",
    f"chain\t{os.environ['ITERS']}",
    f"flags\textC 前端 + cc -O2 -std=c11 -fwrapv -pthread；手写 C 同一组（无 -pthread）",
]
for name, n, ms, rss, cs in rows:
    lines.append(f"data\t{name}\t{n}\t{ms:.3f}\t{rss}\t{cs}")
open(os.path.join(root, "bench/par/raw.txt"), "w").write("\n".join(lines) + "\n")
print("== raw.txt 已写入")
PY

echo "== 生成 RESULTS.md"
python3 "$dir/report.py"
echo "== 完成：$dir/RESULTS.md"
