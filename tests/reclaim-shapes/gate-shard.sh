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
#      红了**才**复测一次，两对里各取较小值；复测仍越线才判红。理由：峰值 RSS 是跨环境的读数
#      （分配器与内核的高水位、页面粒度都在里面），而这条要钉的是「界随轮数涨」这个性质 ——
#      一次高水位不该判成回归。FAIL 行带原始 KB 与 SHARDS=1 的 4N 参考（只看比值分不清
#      「真没回收」还是「别的膨胀」）。
#
# 用法：tests/reclaim-shapes/gate-shard.sh
#   EXTC=… / CC=… / TIME=…   换工具链
#   N=…                      覆盖小规模（默认 200000，4N = 4×N）
#   SHARD_RATIO_MAX=…        覆盖判据 ③ 的上限（只给自检「闸门有牙」用，默认 1.5）
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

# 判据 ③ 的线；`peak` 依赖下面定义的 `run`（bash 只在调用时查函数）。
MAX=${SHARD_RATIO_MAX:-1.5}
peak()    { run "$1" "$2"; printf '%s' "$R_RSS"; }
ratioOf() { awk -v a="$1" -v b="$2" 'BEGIN{ if (a=="" || b=="") print "—"; else printf "%.2f", b/a }'; }
ratioOk() { [ "$1" != "—" ] && awk -v r="$1" -v m="$MAX" 'BEGIN{exit !(r < m)}'; }
minOf()   { awk -v a="$1" -v b="$2" 'BEGIN{ if (a=="") print b; else if (b=="") print a; else print (a<b?a:b) }'; }

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
    # 先测一对；不成对才复测一对，各取两次中的较小值 —— 峰值 RSS 是跨环境读数，一次高水位
    # 不算回归（口径同 `pool` 的墙钟：重复取最小 + 离散自检；理由见脚本头）。
    rssNa=$(peak "$exe8" "$N");  rss4a=$(peak "$exe8" "$N4")
    ratio=$(ratioOf "$rssNa" "$rss4a")
    raw="N=${rssNa} KB / 4N=${rss4a} KB"
    retried=
    if ! ratioOk "$ratio"; then
        retried=1
        rssNb=$(peak "$exe8" "$N"); rss4b=$(peak "$exe8" "$N4")
        rssN=$(minOf "$rssNa" "$rssNb"); rss4=$(minOf "$rss4a" "$rss4b")
        ratio=$(ratioOf "$rssN" "$rss4")
        raw="首对 N=${rssNa} KB / 4N=${rss4a} KB；复测 N=${rssNb} KB / 4N=${rss4b} KB ⇒ 取较小 N=${rssN} KB / 4N=${rss4} KB"
    fi
    if [ "$ratio" = "—" ]; then
        bad "$shape：拿不到峰值 RSS（$raw）"
    elif ratioOk "$ratio"; then
        ok "$shape：分片=8 下 N→4N 峰值比 ${ratio}× < ${MAX}×（界与轮数无关）"
        # 复测救回来的照样留痕：一次高水位是环境噪声的证据，不该被静默吃掉。
        [ -z "$retried" ] || printf '       注：首对不成对（N=%s KB / 4N=%s KB），复测取较小值后成立\n' "$rssNa" "$rss4a"
    else
        run "$exe1" "$N4"; ref=$R_RSS
        bad "$shape：分片=8 下 N→4N 峰值比 ${ratio}× ≥ ${MAX}×（界仍与轮数有关）｜$raw｜参考：SHARDS=1 的 4N 峰值 ${ref} KB"
    fi
done
exit "$fail"
