#!/usr/bin/env bash
# tests/reclaim-idle/sample-asan-bounded.sh —— 抽样加严：判 idle / 仍发射 rc 两侧各 3 个形状。
#
# 语料指纹（`fingerprint.py check`）证明的是"生成物逐字节没变"；这一支补的是**运行时**的
# 两件事，且都不看编译器自己的报告：
#
#   ① 分类没变：每个形状的默认档产物与 `EXTC_RECLAIM=0` 逐字节相同（且不含 `extc_rc_*`）
#      ⇒ 判 idle；不同 ⇒ 仍发射 rc。期望值写死在下面（idle / pays），对不上就红。
#      同时把生成物的 md5 与 `tools/idle-fingerprint-baseline.tsv` 那一行对拍 ⇒ 该形状
#      **逐字节等于重构前冻结的产物** —— 运行时行为因此与重构前同源，不是"看起来还是那个类"。
#   ② 运行安全 + 有界：ASan 编一遍跑两个规模（出现 `AddressSanitizer` 即红）；峰值 RSS 用
#      `/usr/bin/time -f %M`（**不用 fork+wait4 的 ru_maxrss**：父进程 RSS 会污染子进程读数）。
#      N vs 4N 的比：判 idle 的一侧必须 < `BOUND`（默认 1.5×）；仍发射 rc 的一侧**只打印**
#      —— 它的有界性由回收侧的门禁管，不在这条判据的职责里。
#
# 用法：tests/reclaim-idle/sample-asan-bounded.sh
#   EXTC=… / CC=… / TIME=… / N=… / BOUND=…
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=${ROOT:-$(cd "$DIR/../.." && pwd)}
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
N=${N:-50000}
N4=$((N * 4))
BOUND=${BOUND:-1.5}
BASE=${BASE:-$ROOT/tools/idle-fingerprint-baseline.tsv}

# 名字 | 相对路径 | 期望类别。idle 侧取自 `gate-idle-bounded.sh` 的生效名单，
# rc 侧取 `gate-idle-bounded.sh` / `gate-idle-projection.py` 的对照（真的丢引用、必须仍付钱）。
SHAPES="
blind_localhold|tests/reclaim-acyclic/blind_localhold.extc|idle
bounded_churn|tests/reclaim-shapes/bounded_churn.extc|idle
rebuild_per_round|tests/reclaim-perf/shapes/rebuild_per_round.extc|idle
cyclic_pressure|tests/reclaim-shapes/cyclic_pressure.extc|pays
outer_alloc_inner_write|tests/reclaim-perf/shapes/outer_alloc_inner_write.extc|pays
rg_overwrite_ring|tests/reclaim-perf/shapes/refgraph/rg_overwrite_ring.extc|pays
"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

gen() { # <仓库相对源路径> <on|off> <输出.c>
    # 必须在 $ROOT 下用**相对**路径调用：生成的 C 里内嵌源文件路径，绝对路径会让 md5
    # 与基线（同样用相对路径冻结）对不上 —— 那是量法差异，不是发射行为差异。
    if [ "$2" = off ]; then
        (cd "$ROOT" && EXTC_STD="$ROOT/stdlib" EXTC_RECLAIM=0 "$EXTC" \
            -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$3")
    else
        (cd "$ROOT" && EXTC_STD="$ROOT/stdlib" "$EXTC" \
            -w --no-line-map -I "$ROOT/stdlib" "$1" -o "$3")
    fi
}

baseline_md5() { awk -F'\t' -v p="$1" '$1==p {print $3}' "$BASE"; }

rss_of() { # <二进制> <N> → stdout 峰值 KiB；程序 stdout/stderr 落 $TMP/t.out/$TMP/t.err
    timeout 180 "$TIME" -f %M "$1" "$2" >"$TMP/t.out" 2>"$TMP/t.err"
    local rc=$?
    if [ "$rc" != 0 ]; then return "$rc"; fi
    tail -1 "$TMP/t.err" | tr -dc '0-9'
}

asan_hit() { grep -h -m1 -o 'ERROR: AddressSanitizer:.*' "$TMP/t.err" "$TMP/t.out" 2>/dev/null | head -1; }

echo "== 抽样：判 idle / 仍发射 rc 两侧各 3 个（N=$N, 4N=$N4, 界 $BOUND×）=="
echo "   EXTC=$EXTC"
echo "   基线=$BASE"
echo
fail=0
for row in $SHAPES; do
    name=${row%%|*}; rest=${row#*|}
    rel=${rest%%|*}; want=${rest##*|}
    printf '%-24s (%s) ' "$name" "$want"
    if [ ! -f "$ROOT/$rel" ]; then
        bad "形状不存在（$ROOT/$rel）"; fail=1; continue
    fi
    if ! gen "$rel" on "$TMP/$name.c" || ! gen "$rel" off "$TMP/$name.off.c"; then
        bad "生成失败"; fail=1; continue
    fi
    md5=$(md5sum "$TMP/$name.c" | awk '{print $1}')
    base=$(baseline_md5 "$rel")
    if cmp -s "$TMP/$name.c" "$TMP/$name.off.c" && ! grep -q 'extc_rc_' "$TMP/$name.c"; then
        side=idle
    else
        side=pays
    fi
    if [ "$side" != "$want" ]; then
        bad "类别变了：期望 $want，实测 $side"; fail=1; continue
    fi
    if [ -z "$base" ]; then
        bad "基线里没有这一行（$rel）"; fail=1; continue
    fi
    if [ "$md5" != "$base" ]; then
        bad "生成物与冻结基线不同（基线 ${base:0:8} / 现在 ${md5:0:8}）"; fail=1; continue
    fi

    if ! $CC -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O2 -o "$TMP/$name" "$TMP/$name.c" \
            >"$TMP/$name.cc.log" 2>&1; then
        bad "生成的 C 编不过（$(sed -n 1p "$TMP/$name.cc.log")）"; fail=1; continue
    fi
    if ! $CC -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -g -fsanitize=address \
            -o "$TMP/$name.asan" "$TMP/$name.c" >"$TMP/$name.asan.cc.log" 2>&1; then
        bad "ASan 档编不过（$(sed -n 1p "$TMP/$name.asan.cc.log")）"; fail=1; continue
    fi

    rn=$(rss_of "$TMP/$name" "$N") || { bad "N=$N 运行失败"; fail=1; continue; }
    r4=$(rss_of "$TMP/$name" "$N4") || { bad "N=$N4 运行失败"; fail=1; continue; }
    rss_of "$TMP/$name.asan" "$N" >/dev/null || { bad "ASan N=$N 运行失败"; fail=1; continue; }
    hit=$(asan_hit); [ -n "$hit" ] && { bad "ASan 报告（N=$N）：$hit"; fail=1; continue; }
    rss_of "$TMP/$name.asan" "$N4" >/dev/null || { bad "ASan N=$N4 运行失败"; fail=1; continue; }
    hit=$(asan_hit); [ -n "$hit" ] && { bad "ASan 报告（N=$N4）：$hit"; fail=1; continue; }

    ratio=$(awk -v a="$r4" -v b="$rn" 'BEGIN{ if (b+0==0) printf "—"; else printf "%.2f", a/b }')
    note="与冻结基线逐字节同（${md5:0:8}）· 峰值 $((rn/1024))→$((r4/1024)) MB（${ratio}×）· ASan 干净"
    if [ "$side" = idle ]; then
        if awk -v r="$ratio" -v lim="$BOUND" 'BEGIN{ exit !(r+0 < lim+0) }'; then
            ok "idle · $note"
        else
            bad "idle 但峰值随规模涨：$note（界 ${BOUND}×）"; fail=1
        fi
    else
        printf '\033[32mok\033[0m   pays · %s（这一侧不判 %s×）\n' "$note" "$BOUND"
    fi
done
echo
if [ "$fail" = 0 ]; then echo "抽样全部通过"; else echo "抽样有失败"; fi
exit "$fail"
