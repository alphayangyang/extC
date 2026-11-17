#!/usr/bin/env bash
# bench/containers/run-cg.sh —— 用 callgrind 把「慢在哪」从墙钟里拆出来：指令数 vs 末级缺失
#
# 背景：h1/h5 上 extC 比手写开放寻址表慢 1.7×，两种解释都说得通——(a) 内存访问更多（五条列、
# 稠密值列要多一次依赖读），(b) 指令更多（每次下标一个边界检查、方法没内联、哈希表头每次重读）。
# 这个脚本用 callgrind 的 cache 模拟同时给出两侧的数：I refs（指令）与 LL misses（末级缺失）。
# 判据：如果两侧 LL misses 相同而 I refs 差 3–4×，那么差距是指令数而不是内存流量。
#
# 用法：bash bench/containers/run-cg.sh [h1|h5 ...]（默认 h1 h5）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
PIN=${PIN:-6}
SHAPES=${*:-h1 h5}
B=${B:-$(mktemp -d)}; trap 'rm -rf "$B"' EXIT
command -v valgrind >/dev/null || { echo '缺 valgrind（apt install valgrind）' >&2; exit 2; }

$CXX -O2 -g -std=c++17 -o "$B/flat" bench/containers/h_flat.cpp || exit 1

report() {   # report <标签> <ops> <valgrind 输出文件>
    local name=$1 ops=$2 f=$3
    local ir dr d1mr llm
    ir=$(grep -o 'I *refs: *[0-9,]*' "$f" | tr -d ' ' | cut -d: -f2 | tr -d ',')
    dr=$(grep -o 'D *refs: *[0-9,]*' "$f" | tr -d ' ' | cut -d: -f2 | tr -d ',')
    d1mr=$(grep -o 'D1  misses: *[0-9,]*' "$f" | tr -d ' ' | cut -d: -f2 | tr -d ',')
    llm=$(grep -o 'LL misses: *[0-9,]*' "$f" | tr -d ' ' | cut -d: -f2 | tr -d ',')
    awk -v n="$name" -v ir="$ir" -v dr="$dr" -v d1="$d1mr" -v ll="$llm" -v ops="$ops" \
        'BEGIN{printf "%-6s Ir=%12d (%.1f/op)  Drefs=%12d  D1mr=%10d  LLmiss=%10d (%.2f/op)\n", n, ir, ir/ops, dr, d1, ll, ll/ops}'
}

for s in $SHAPES; do
    case $s in
        h1) src=h1_cap.extc ; ops=8000000  ; arg=h1 ;; 
        h5) src=h5_miss.extc ; ops=12000000 ; arg=h5 ;; 
        *) echo "unknown shape: $s" >&2; exit 2 ;;
    esac
    "$EXTC" -w --no-line-map -o "$B/$s.c" "bench/containers/$src" || exit 1
    $CC -O2 -g -fwrapv -std=c11 -o "$B/$s" "$B/$s.c" || exit 1
    taskset -c "$PIN" valgrind --tool=callgrind --cache-sim=yes --callgrind-out-file=/dev/null \
        "$B/$s" >/dev/null 2>"$B/$s.vg"
    taskset -c "$PIN" valgrind --tool=callgrind --cache-sim=yes --callgrind-out-file=/dev/null \
        "$B/flat" "$arg" >/dev/null 2>"$B/$s.flat.vg"
    echo "== $s（$ops 次操作：插入 + 查询/未命中）"
    report extc "$ops" "$B/$s.vg"
    report flat "$ops" "$B/$s.flat.vg"
done
