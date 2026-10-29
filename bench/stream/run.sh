#!/usr/bin/env bash
# bench/stream/run.sh —— 流式管线的 DRAM 代价：构建、逐字节对拍、计时，
# 生成 bench/stream/raw.txt 并调 report.py 写出 bench/stream/RESULTS.md。
#
# 用法：
#   bash bench/stream/run.sh
#   RUNS=3 SIZES="131072 1048576" bash bench/stream/run.sh
#
# 可覆盖环境变量：EXTC、CC、RUNS、PIN、PARPIN、SIZES。
# 口径与判据见同目录 SPEC.md。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dir=bench/stream
cd "$root"

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
RUNS=${RUNS:-5}
PIN=${PIN:-6}
PARPIN=${PARPIN:-6-13}
SIZES=${SIZES:-"131072 1048576 4194304"}
REPS=${REPS:-4}
THREADS=${THREADS:-8}

FLAGS_O2="-O2 -std=c11 -fwrapv"
FLAGS_O3="-O3 -march=native -fwrapv"

die() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$EXTC" ] || die "找不到 extC 前端：$EXTC（先 make build）"
command -v "$CC" >/dev/null 2>&1 || die "找不到 C 编译器：$CC"
command -v python3 >/dev/null 2>&1 || die "找不到 python3"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/bench_stream.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "== 构建各臂"
"$EXTC" -w --no-line-map -o "$tmp/pipeline.extc.c" "$dir/pipeline.extc" || die "extC 前端编译 pipeline.extc 失败"
$CC $FLAGS_O2 -pthread -o "$tmp/extc.O2" "$tmp/pipeline.extc.c" || die "编译 extc.O2 失败"
$CC $FLAGS_O3 -pthread -o "$tmp/extc.O3" "$tmp/pipeline.extc.c" || die "编译 extc.O3 失败"
$CC -O2 -std=c11 -o "$tmp/c.O2" "$dir/pipeline.c" || die "编译 c.O2 失败"
$CC -O3 -march=native -o "$tmp/c.O3" "$dir/pipeline.c" || die "编译 c.O3 失败"
$CC -O2 -std=c11 -o "$tmp/ctuned.O2" "$dir/pipeline_tuned.c" || die "编译 ctuned.O2 失败"
$CC -O3 -march=native -o "$tmp/ctuned.O3" "$dir/pipeline_tuned.c" || die "编译 ctuned.O3 失败"

echo "== 逐字节对拍（等价性先于性能）"
ref=$("$tmp/extc.O2" 4096 1 "$REPS") || die "extc.O2 运行失败"
for m in 1 2 3; do
    got=$("$tmp/extc.O2" 4096 $m "$REPS"); [ "$got" = "$ref" ] || die "extC mode$m 输出 '$got'，mode1 是 '$ref'"
    got=$("$tmp/c.O2" 4096 $m "$REPS");     [ "$got" = "$ref" ] || die "C mode$m 输出 '$got'，mode1 是 '$ref'"
done
got=$(EXTC_PAR_THREADS=$THREADS "$tmp/extc.O2" 4096 4 "$REPS"); [ "$got" = "$ref" ] || die "par-fused 输出 '$got'，mode1 是 '$ref'"
for m in 2 3 5 6; do
    got=$("$tmp/ctuned.O2" 4096 $m "$REPS"); [ "$got" = "$ref" ] || die "ctuned mode$m 输出 '$got'，mode1 是 '$ref'"
done
echo "   extC(mode1-4) / C(mode1-3) / ctuned(mode2-3) 在 n=4096 上 stdout 逐字节一致：$ref"

export EXTC CC RUNS PIN PARPIN SIZES REPS THREADS FLAGS_O2 FLAGS_O3
echo "== 计时（RUNS=$RUNS，pin=$PIN，par-pin=$PARPIN，n=$SIZES）"
python3 - "$tmp" "$root" <<'PY'
import os, re, subprocess, sys, time

tmp, root = sys.argv[1], sys.argv[2]
runs = int(os.environ["RUNS"])
sizes = [int(x) for x in os.environ["SIZES"].split()]
pin = os.environ.get("PIN", "").strip()
parpin = os.environ.get("PARPIN", "").strip()
threads = os.environ.get("THREADS", "8").strip()
reps = int(os.environ.get("REPS", "1"))

def pfx(p):
    return ["taskset", "-c", p] if p else []

def timed(exe, n, mode, env_extra, p):
    env = dict(os.environ)
    env.update(env_extra)
    t0 = time.perf_counter()
    r = subprocess.run(pfx(p) + [exe, str(n), str(mode), str(reps)], capture_output=True, text=True, env=env)
    dt = (time.perf_counter() - t0) * 1e3
    if r.returncode != 0:
        sys.exit(f"FAIL: {exe} n={n} mode={mode} 退出码 {r.returncode}: {r.stderr[:200]}")
    return dt, r.stdout

def rss_kb(exe, n, mode, env_extra, p):
    timer = "/usr/bin/time"
    if not os.path.exists(timer):
        return 0
    env = dict(os.environ)
    env.update(env_extra)
    r = subprocess.run(pfx(p) + [timer, "-v", exe, str(n), str(mode), str(reps)], capture_output=True, text=True, env=env)
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", r.stderr)
    return int(m.group(1)) if m else 0

PARENV = {"EXTC_PAR_THREADS": threads}
arms = [
    ("extc.O2", os.path.join(tmp, "extc.O2"), (1, 2, 3, 4)),
    ("extc.O3", os.path.join(tmp, "extc.O3"), (1, 2, 3, 4)),
    ("c.O2", os.path.join(tmp, "c.O2"), (1, 2, 3)),
    ("c.O3", os.path.join(tmp, "c.O3"), (1, 2, 3)),
    ("ctuned.O2", os.path.join(tmp, "ctuned.O2"), (2, 3, 5, 6)),
    ("ctuned.O3", os.path.join(tmp, "ctuned.O3"), (2, 3, 5, 6)),
]

rows = []
for n in sizes:
    exp = None
    for name, exe, modes in arms:
        for mode in modes:
            env_extra = PARENV if (name.startswith("extc") and mode == 4) else {}
            p = parpin if (name.startswith("extc") and mode == 4) else pin
            timed(exe, n, mode, env_extra, p)  # 预热一次
            best = 1e18
            out = ""
            for _ in range(runs):
                dt, out = timed(exe, n, mode, env_extra, p)
                best = min(best, dt)
            cs = int(re.search(r"cs=(-?\d+)", out).group(1))
            if exp is None:
                exp = cs
            elif cs != exp:
                sys.exit(f"FAIL: {name} mode={mode} n={n} cs={cs} 与参考的 {exp} 不一致")
            rss = rss_kb(exe, n, mode, env_extra, p)
            rows.append((name, mode, n, best, rss, cs))
            print(f"   n={n:8d} {name:9s} mode={mode} {best:9.2f} ms  rss={rss:7d} kB")

machine = re.search(r"model name\s*:\s*(.+)", open("/proc/cpuinfo").read()).group(1).strip()
def first_line(cmd):
    try:
        return subprocess.run([cmd, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        return cmd
cache = ""
try:
    for ln in subprocess.run(["lscpu"], capture_output=True, text=True).stdout.splitlines():
        if ln.startswith("L1d") or ln.startswith("L2") or ln.startswith("L3"):
            cache += ln.split(":")[0].strip() + " " + ln.split(":")[1].strip() + " ; "
except Exception:
    cache = ""

lines = [
    "bench/stream/raw.txt —— 由 run.sh 生成，别手改（原始记录，RESULTS.md 由 report.py 从本文件生成）",
    f"machine\t{machine}",
    f"kernel\t{os.uname().sysname} {os.uname().release}",
    f"nproc\t{os.cpu_count()}",
    f"caches\t{cache}",
    f"gcc\t{first_line(os.environ['CC'])}",
    f"extc\t{os.environ['EXTC']}",
    f"pin\t{pin or '(none)'}",
    f"parpin\t{parpin or '(none)'}",
    f"threads\t{threads}",
    f"reps\t{reps}",
    f"runs\t{runs}",
    f"sizes\t{' '.join(map(str, sizes))}",
    f"flags_O2\t{os.environ['FLAGS_O2']}",
    f"flags_O3\t{os.environ['FLAGS_O3']} -march=native",
]
for name, mode, n, ms, rss, cs in rows:
    lines.append(f"data\t{name}\t{mode}\t{n}\t{ms:.3f}\t{rss}\t{cs}")
open(os.path.join(root, "bench/stream/raw.txt"), "w").write("\n".join(lines) + "\n")
print("== raw.txt 已写入")
PY

echo "== 生成 RESULTS.md"
python3 "$dir/report.py"
echo "== 完成：$dir/RESULTS.md"
