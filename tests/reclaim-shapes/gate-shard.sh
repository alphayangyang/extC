#!/usr/bin/env bash
# tests/reclaim-shapes/gate-shard.sh —— **分片路径**（`EXTC_RC_SHARDS`）的门禁。
#
# 为什么需要它：分片是 codegen 的第四条发射路径（区域键 = 基址 + 片号，`rcAllocCore` /
# `rcEmitTrigger` / `rcEmitOwe` / `rcEmitOwedCheck` 各有一条 `nsh > 1` 分支）。
# `run-shard.sh` 只是**证据脚本**（停顿/峰值/吞吐对照），不进 CI；于是"分片打开时每个分配点
# 只落一片、片内链条完整、界与轮数无关"这些前提，**在 CI 里没有人替我们钉**。
#
# 三条判据：
#   ① **旋钮非空转**：`EXTC_RC_SHARDS=1` 与 `=8` 的生成物必须不同（否则下面两条是空跑）。
#   ② **语义一致**：同一形状在 1 与 8 下、两个规模上，标准输出逐字相同。
#   ③ **界与轮数无关**（分片=8）：N vs 4N 的峰值 RSS 比 < 1.5 —— 与 `run-switch.sh` 同一条口径。
#
# 用法：tests/reclaim-shapes/gate-shard.sh
#   EXTC=… / CC=… / TIME=…   换工具链
#   N=…                      覆盖小规模（默认 200000，4N = 4×N）
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
N=${N:-200000}
N4=$((N * 4))

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=1; }

build() { # <形状> <分片数> → 回显可执行路径；失败回显空
    local shape=$1 sh=$2
    local tag="$shape-$sh"
    EXTC_RECLAIM=1 EXTC_RC_SHARDS=$sh "$EXTC" -w --no-line-map \
        "tests/reclaim-shapes/$shape.extc" -o "$TMP/$tag.c" >"$TMP/$tag.log" 2>&1 || return 1
    "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$tag" "$TMP/$tag.c" >>"$TMP/$tag.log" 2>&1 || return 1
    echo "$TMP/$tag"
}

run() { # <可执行> <N> → 全局 R_RC / R_RSS / R_OUT
    local exe=$1 n=$2
    "$TIME" -v -o "$TMP/r.time" "$exe" "$n" >"$TMP/r.out" 2>"$TMP/r.err"
    R_RC=$?
    R_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/r.time")
    R_OUT=$(cat "$TMP/r.out")
}

echo "== 分片路径门禁（EXTC_RC_SHARDS，N=$N / 4N=$N4）=="
# `cross_region_elem` 是**跨区**形状（下标槽里的跨区入边 = 清扫种子的唯一来源）：
# 分片把区域键变成"基址 + 片号"，跨区登记与片内链条必须同时成立 —— 这条一起判。
for shape in shard_bands cyclic_pressure cross_region_elem; do
    exe1=$(build "$shape" 1) || { bad "$shape：SHARDS=1 生成/编译失败"; continue; }
    exe8=$(build "$shape" 8) || { bad "$shape：SHARDS=8 生成/编译失败"; continue; }

    # ① 旋钮非空转
    if cmp -s "$TMP/$shape-1.c" "$TMP/$shape-8.c"; then
        bad "$shape：SHARDS=1 与 =8 的生成物逐字节相同 ⇒ 分片没生效（下面两条是空跑）"
        continue
    fi

    # ② 语义一致（两个规模）
    for n in "$N" "$N4"; do
        run "$exe1" "$n"; rc1=$R_RC; out1=$R_OUT
        run "$exe8" "$n"; rc8=$R_RC; out8=$R_OUT
        if [ "$rc1" != 0 ] || [ "$rc8" != 0 ]; then
            bad "$shape N=$n：退出码 $rc1 / $rc8"; continue
        fi
        if [ "$out1" != "$out8" ]; then
            bad "$shape N=$n：SHARDS=1 与 =8 输出不一致"
            printf '        sh=1: %s\n        sh=8: %s\n' "$out1" "$out8"
        else
            ok "$shape N=$n：SHARDS=1 与 =8 输出逐字一致"
        fi
    done

    # ③ 界与轮数无关（分片=8）
    run "$exe8" "$N";  rssN=$R_RSS
    run "$exe8" "$N4"; rss4=$R_RSS
    ratio=$(awk -v a="$rssN" -v b="$rss4" 'BEGIN{ if (a=="" || b=="") print "—"; else printf "%.2f", b/a }')
    if [ "$ratio" = "—" ]; then
        bad "$shape：拿不到峰值 RSS"
    elif awk -v r="$ratio" 'BEGIN{exit !(r < 1.5)}'; then
        ok "$shape：分片=8 下 N→4N 峰值比 ${ratio}× < 1.5×（界与轮数无关）"
    else
        bad "$shape：分片=8 下 N→4N 峰值比 ${ratio}× ≥ 1.5×（界仍与轮数有关）"
    fi
done
exit "$fail"
