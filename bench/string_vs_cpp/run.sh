#!/usr/bin/env bash
# bench/string_vs_cpp/run.sh —— extC `stl::string`（SSO）对 C++ `std::string`：一把跑完。
#
#   ① 自己编译两边：extC 前端吐 C，再用契约旗子编（`gcc -O2 -std=c11 -fwrapv`）；
#      C++ 直接 `g++ -O2 -std=c++17`。**不用 `--run`**（那会把编译时间算进去）。
#   ② 每个场景先**冷启动预热一次**（不计时），再 **best of 5** 计时：外部 python
#      `time.perf_counter()` 量整个进程（两边口径一样，程序里不放计时器）。
#   ③ RSS：每个场景单独再跑（默认 2 次取最大），用 `/usr/bin/time -v` 的
#      **Maximum resident set size**；没有 `/usr/bin/time` 时退化成
#      `resource.getrusage(RUSAGE_CHILDREN).ru_maxrss`（每次测量开一个干净的 python 进程，
#      所以那个高水位只属于这一个孩子）。
#   ④ 原始记录写 bench/string_vs_cpp/raw.txt，然后调 report.py 生成 RESULTS.md 的表。
#
# 用法：
#   bench/string_vs_cpp/run.sh                 # 全部 12 个场景（约 1~2 分钟）
#   RUNS=3 bench/string_vs_cpp/run.sh          # 想快一点
#   ONLY="find compare" bench/string_vs_cpp/run.sh
set -u

cd "$(dirname "$0")/../.." || exit 1
DIR=bench/string_vs_cpp
EXTC=${EXTC:-./build/extc}
OUT=${OUT:-$DIR/raw.txt}
RUNS=${RUNS:-5}
RSS_RUNS=${RSS_RUNS:-2}
SCENARIOS=${ONLY:-"short_many short_cross long_build_byte long_build_chunk reserve_growth read_at find compare substr_copy clone_many clear_shrink_cycle many_live_short"}

if [ ! -x "$EXTC" ]; then echo "找不到 extC 前端：$EXTC（先 make）" >&2; exit 1; fi
work=$(mktemp -d /tmp/bench_string.XXXXXX) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

# ---------------------------------------------------------------- 编译
echo "== 编译 =="
if ! "$EXTC" -w --no-line-map -o "$work/s.c" "$DIR/s.extc" 2>"$work/extc.err"; then
    echo "extC 前端失败：$(head -3 "$work/extc.err")" >&2; exit 1
fi
if ! gcc -O2 -std=c11 -fwrapv -o "$work/s" "$work/s.c" 2>"$work/gcc.err"; then
    echo "gcc 失败：$(head -3 "$work/gcc.err")" >&2; exit 1
fi
if ! g++ -O2 -std=c++17 -o "$work/cpp" "$DIR/cpp.cpp" 2>"$work/gxx.err"; then
    echo "g++ 失败：$(head -3 "$work/gxx.err")" >&2; exit 1
fi
echo "  extC: $EXTC -w --no-line-map -o s.c $DIR/s.extc && gcc -O2 -std=c11 -fwrapv -o s s.c"
echo "  C++ : g++ -O2 -std=c++17 -o cpp $DIR/cpp.cpp"

# ---------------------------------------------------------------- 旁证：不 release 的 20 字节串循环
# 这**不是** 12 个场景之一，是一条旁证：`string` 的板块住在池里，池的寿命挂在"地方"上，
# 循环体里 push 出来的池落在**函数**那个地方 —— 不 release 就不会在轮次之间回收。
# 只量 RSS，不入表。
probe=""
cat > "$work/probe.extc" <<'EOF'
use std::io
use stl::string
fn main(args: slice<slice<u8>>) -> i32 {
    var sum: i64 = 0
    var i: i64 = 0
    while i < i64(1000000) {
        var s: string
        var k: i64 = 0
        while k < i64(20) { s.push(u8(32 + k))  k = k + i64(1) }
        sum = sum + s.len() + i64(s.at(i64(19)) ?? u8(0))
        i = i + i64(1)
    }
    io::cout << "probe cs=" << sum << "\n"
    return 0
}
EOF
if "$EXTC" -w --no-line-map -o "$work/probe.c" "$work/probe.extc" 2>/dev/null \
   && gcc -O2 -std=c11 -fwrapv -o "$work/probe" "$work/probe.c" 2>/dev/null; then
    probe="$work/probe"
fi

# ---------------------------------------------------------------- 量
# 计时与 RSS 全在 python 里做：一个进程一个场景（RSS 才干净），计时口径两边完全一样。
SCENARIOS="$SCENARIOS" RUNS="$RUNS" RSS_RUNS="$RSS_RUNS" OUT="$OUT" \
S_EXTC="$work/s" S_CPP="$work/cpp" PROBE="$probe" EXTC_BIN="$EXTC" DIR="$DIR" \
python3 - <<'PY' || exit 1
import os, re, resource, statistics, subprocess, sys, time

scenarios = os.environ["SCENARIOS"].split()
runs      = int(os.environ["RUNS"])
rss_runs  = int(os.environ["RSS_RUNS"])
out       = os.environ["OUT"]
bins      = {"extc": os.environ["S_EXTC"], "cpp": os.environ["S_CPP"]}
probe     = os.environ["PROBE"]
have_time = os.path.exists("/usr/bin/time")

CS = re.compile(rb"\bcs=(-?\d+)")
RSS_TIME = re.compile(rb"Maximum resident set size \(kbytes\):\s*(\d+)")

def rss_of(argv):
    """跑一次，拿峰值 RSS（KB）。优先 /usr/bin/time -v；没有就开一个干净的 python 子进程读 getrusage。"""
    if have_time:
        p = subprocess.run(["/usr/bin/time", "-v"] + argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        m = RSS_TIME.search(p.stderr)
        if not m:
            raise SystemExit("拿不到 RSS：%s" % p.stderr[-300:])
        return int(m.group(1)), p.stdout
    code = ("import resource,subprocess,sys;"
            "subprocess.run(sys.argv[1:],stdout=subprocess.DEVNULL);"
            "print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)")
    p = subprocess.run([sys.executable, "-c", code] + argv, stdout=subprocess.PIPE)
    return int(p.stdout.split()[0]), b""

def cs_of(stdout, what):
    m = CS.search(stdout)
    if not m:
        raise SystemExit("没打印校验和：%s -> %r" % (what, stdout[-200:]))
    return m.group(1).decode()

def measure(impl, path, sc):
    # 冷启动预热一次（不计时、不入账）
    subprocess.run([path, sc], stdout=subprocess.DEVNULL, check=True)
    times, checks = [], []
    for _ in range(runs):
        t0 = time.perf_counter()
        p = subprocess.run([path, sc], stdout=subprocess.PIPE, check=True)
        times.append((time.perf_counter() - t0) * 1000.0)
        checks.append(cs_of(p.stdout, "%s %s" % (impl, sc)))
    if len(set(checks)) != 1:
        raise SystemExit("同一个程序 5 次校验和不一样：%s %s %r" % (impl, sc, checks))
    rss, stdout = max((rss_of([path, sc]) for _ in range(rss_runs)), key=lambda x: x[0])
    if cs_of(stdout, "rss-run %s %s" % (impl, sc)) != checks[0]:
        raise SystemExit("RSS 那一次的校验和与计时那几次不一致：%s %s" % (impl, sc))
    return min(times), rss, checks[0], times

def tool(cmd):
    try:
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode().splitlines()[0]
    except Exception:
        return "?"

machine = "?"
for line in open("/proc/cpuinfo", encoding="utf-8", errors="replace"):
    if line.startswith("model name"):
        machine = line.split(":", 1)[1].strip(); break
cores = os.cpu_count()
kernel = os.uname().release

hdr = []
hdr.append("# " + "=" * 76)
hdr.append("# bench/string_vs_cpp/raw.txt —— 由 run.sh 生成，别手改（report.py 从这里出表）")
hdr.append("# 生成: " + time.strftime("%F %T"))
hdr.append("# 机器: %s · %s" % (machine, kernel))
hdr.append("# nproc: %d" % (cores,))
hdr.append("# gcc : " + tool(["gcc", "--version"]))
hdr.append("# g++ : " + tool(["g++", "--version"]))
hdr.append("# extC: " + os.environ["EXTC_BIN"])
hdr.append("# 编译: %s -w --no-line-map -o s.c %s/s.extc && gcc -O2 -std=c11 -fwrapv -o s s.c"
           % (os.environ["EXTC_BIN"], os.environ["DIR"]))
hdr.append("#       g++ -O2 -std=c++17 -o cpp %s/cpp.cpp" % os.environ["DIR"])
hdr.append("# 计时: python time.perf_counter，冷启动预热 1 次 + best of %d（毫秒，含进程启动）" % runs)
hdr.append("# RSS : %s，每格单独跑 %d 次取最大" %
           ("/usr/bin/time -v 的 Maximum resident set size (kbytes)" if have_time
            else "resource.getrusage(RUSAGE_CHILDREN).ru_maxrss", rss_runs))
hdr.append("# 反折叠: 内容恒定的两个循环接受可选 argv[2] 盐水（默认 0，字节与不加盐相同）；")
hdr.append("#         C++ 侧 short_many 另有一道 asm volatile barrier（见 cpp.cpp / RESULTS.md）")
hdr.append("# 行格式: data <impl> <scenario> <best_ms> <rss_kb> <checksum>")
hdr.append("# " + "=" * 76)

res, spread = {}, []
for sc in scenarios:
    for impl in ("extc", "cpp"):
        best, rss, cs, times = measure(impl, bins[impl], sc)
        res[(impl, sc)] = (best, rss, cs)
        spread.append("%-18s %-4s min=%8.3f med=%8.3f max=%8.3f" % (sc, impl, min(times), statistics.median(times), max(times)))
        print("  %-4s %-18s %9.3f ms  RSS %7d KB  cs=%s" % (impl, sc, best, rss, cs), flush=True)

rows = ["data %-4s %-18s %9.3f %8d %s" % (impl, sc, res[(impl, sc)][0], res[(impl, sc)][1], res[(impl, sc)][2])
        for sc in scenarios for impl in ("extc", "cpp") if (impl, sc) in res]

# 校验和两边必须逐位相同 —— 不同就是没做等价的工作
bad = [(sc, res[("extc", sc)][2], res[("cpp", sc)][2]) for sc in scenarios
       if ("extc", sc) in res and ("cpp", sc) in res and res[("extc", sc)][2] != res[("cpp", sc)][2]]

# 地板：同一个口径量"什么都不做"的那两个二进制（进程启动 + 运行时），不进结果表
floor = {}
for impl in ("extc", "cpp"):
    best, rss, cs, _ = measure(impl, bins[impl], "noop")
    floor[impl] = (best, rss)
    print("  floor %-4s noop %9.3f ms  RSS %7d KB  cs=%s" % (impl, best, rss, cs), flush=True)

probe_out = []
if probe:
    rss, stdout = rss_of([probe])
    probe_out.append("# 地板 noop extc_ms=%.3f extc_rss=%d cpp_ms=%.3f cpp_rss=%d"
                      "（进程启动 + 运行时；每一格的计时都含这个常数项）"
                      % (floor["extc"][0], floor["extc"][1], floor["cpp"][0], floor["cpp"][1]))
    probe_out.append("# 旁证 probe_no_release_short_cross rss_kb=%d cs=%s（1M 轮 20 字节串，**不** release；"
                      "校验和与场景 short_cross 相同 ⇒ 干的是同一件事，只是没归还）" % (rss, cs_of(stdout, "probe")))
    print("  probe(no release)  RSS %d KB" % rss, flush=True)

os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
with open(out, "w", encoding="utf-8") as f:
    f.write("\n".join(hdr + [""] + rows + [""] + probe_out + [""]))
    f.write("# 离散度（best of %d 的原始三次：最小 / 中位 / 最大，毫秒）\n" % runs)
    for s in spread:
        f.write("#   " + s + "\n")
    f.write("\n")
    if bad:
        f.write("# !!! 校验和不一致：%r\n" % (bad,))
    else:
        f.write("# 12 个场景的校验和：两边逐位相同 ✓\n")

if bad:
    print("\n校验和不一致：%r" % (bad,), file=sys.stderr)
    raise SystemExit(1)
print("\n原始记录：%s（校验和两边逐位相同 ✓）" % out)
PY

# ---------------------------------------------------------------- 出表
python3 "$DIR/report.py" "$OUT" || exit 1
echo "完成：$OUT + $DIR/RESULTS.md"
