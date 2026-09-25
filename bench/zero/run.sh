#!/usr/bin/env bash
# bench/zero/run.sh —— 「清零值多少钱」的基线（不清零路径的收益尺子）
#
# 三个探针：
#   probe_vs_zero    同一个形状（拿一块、丢一块）走清零 / 不清零两条路，差 = 清零的价格
#   probe_capacity   固定清零，容量 4KB → 64MB，看它是内存带宽还是页错误
#   probe_containers 用户可见的容器预分配（vector::withCap / pool::withCap）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
mkdir -p build

build_one() {
    "$EXTC" "bench/zero/$1.extc" -o "build/bz-$1.c" >/dev/null 2>&1 \
      && "$CC" -O1 -std=c11 "build/bz-$1.c" -o "build/bz-$1" >/dev/null 2>&1
}
best3() {   # 程序 -> best of 3 秒
    local b=99 t
    for _ in 1 2 3; do
        t=$( { /usr/bin/time -f %e "./build/bz-$1" >/dev/null; } 2>&1 | tail -1 )
        b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
    done
    printf '%s' "$b"
}
best3_argv() {   # 程序 参数... -> best of 3 秒（参数里带做法字母）
    local name=$1; shift
    local b=99 t
    for _ in 1 2 3; do
        t=$( { TIMEFORMAT=%R; time "./build/bz-$name" "$@" >/dev/null; } 2>&1 | tail -1 )
        b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
    done
    printf '%s' "$b"
}

echo "== 清零 vs 不清零（同一形状：拿一块 32 MB、只写首尾）=="
if build_one probe_raw_16m && build_one probe_zero_16m; then
    r=$(best3 probe_raw_16m); z=$(best3 probe_zero_16m)
    echo "  不清零（poolSliceRaw）: ${r}s · 清零（poolSlice）: ${z}s ⇒ 清零占 ${z}s 的绝大部分"
else
    echo "  编不过"; exit 1
fi

echo
echo "== 清零的价格随容量怎么走（同一形状，只换块大小）=="
for p in probe_raw probe_zero probe_vector_cap probe_pool_cap; do
    if build_one "$p"; then echo "  $(printf '%-20s' "$p") $(best3 "$p")s"; else echo "  $p 编不过"; fi
done

echo
ROUNDS=${ROUNDS:-20000000}      # 数据给大：O(1) 那版快到测不出，小了全是噪声
PER=${PER:-8}
echo "== epoch 染色 / 保留 / O(1) 清空：**并排对照**（${ROUNDS} 轮 × 每轮 ${PER} 条）=="
# A 每轮重建（drop+new，板块进 malloc）  B O(1) 清空（reset：翻代 + 游标归零）
# C 重建 + 保留池                      D 物理清空整张槽表（O(cap)）
if build_one probe_epoch_vs_clear; then
    for m in A B C D; do
        b=$(best3_argv probe_epoch_vs_clear "$ROUNDS" "$PER" "$m")
        out=$(./build/bz-probe_epoch_vs_clear "$ROUNDS" "$PER" "$m")
        ns=$(awk -v t="$b" -v n="$ROUNDS" 'BEGIN{printf "%.2f", t*1e9/n}')
        printf '  %s  %-9ss  %7s ns/轮   %s\n' "$m" "$b" "$ns" "$out"
    done
    echo "  （A/C 的 live=0：每轮都真的 drop 了；B/D 的 live=49152：池留着，这正是 O(1) 的前提）"
else
    echo "  probe_epoch_vs_clear 编不过"
fi

echo
echo "== 同一形状的 C++ 基线（vector<pair<int,int>>）=="
if [ -f bench/zero/cpp/epoch_vs_clear.cpp ] && command -v g++ >/dev/null 2>&1; then
    if g++ -O1 -std=c++17 -o build/bz-cpp-evc bench/zero/cpp/epoch_vs_clear.cpp 2>/dev/null; then
        for m in A B C; do
            b=99
            for _ in 1 2 3; do
                t=$( { TIMEFORMAT=%R; time ./build/bz-cpp-evc "$ROUNDS" "$PER" "$m" >/dev/null; } 2>&1 | tail -1 )
                b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
            done
            ns=$(awk -v t="$b" -v n="$ROUNDS" 'BEGIN{printf "%.2f", t*1e9/n}')
            printf '  C++ %s  %-9ss  %7s ns/轮\n' "$m" "$b" "$ns"
        done
    else
        echo "  g++ 编不过（这一支跳过）"
    fi
else
    echo "  跳过（没有 g++）"
fi

echo
echo "== 反例：判据有牙吗？让「每池成本 ∝ 池数」⇒ 曲线必须翘起来 =="
if build_one exit_cost_tooth; then
    printf '  有牙版 exit_cost_tooth：'
    for n in 200 2000 20000; do
        b=99
        for _ in 1 2; do
            t=$( { /usr/bin/time -f %e ./build/bz-exit_cost_tooth "$n" 4000000 >/dev/null; } 2>&1 | tail -1 )
            b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
        done
        printf 'N=%s:%sns ' "$n" "$(awk -v t="$b" 'BEGIN{printf "%.0f", t*1e9/4e6}')"
    done
    echo "⇒ 与「真的那版三个 N 都是 10.67ns」形成对照（tests/pool/run.sh 的判据上限 1.5）"
else
    echo "  exit_cost_tooth 编不过"
fi

echo
echo "参考（C，同一台机器，malloc+memset+free 每轮）："
cat <<'TXT'
    4 KB    28 ns/轮    (145 GB/s, 缓存内)
    400 KB  4,633 ns/轮  (88 GB/s, 缓存带)
    4 MB    76,025 ns/轮 (55 GB/s)
    16 MB   441,874 ns/轮(38 GB/s)
    64 MB   25,553,282 ns/轮 (2.6 GB/s ⇒ 掉出缓存，mmap/页错误的代价显形)
TXT
