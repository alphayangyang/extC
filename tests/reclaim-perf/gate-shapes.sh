#!/usr/bin/env bash
# tests/reclaim-perf/gate-shapes.sh —— **真实形状**上的回收性能判据（`bt` / `rebuild`，小输入）。
#
# 为什么要另开一条：`gate.sh` 只钉合成形状 `cyclic_pressure`。第四刀把真实形状的指令数也降了
# 9~11 %，而它们当时**没有闸门** —— 收益被付回没人知道。这条把它们钉住。
#
# 口径（与 `bench/bigmatrix/run.sh` 同形，但**固定 `-O2`、不带 `-march=native`**：闸门要的是
# 可复现的指令数，不是最快的那份代码；小输入由 `bench/bigmatrix/gen.c small` 生成，它是
# `.gitignore` 里的生成物，缺了就地生成）：
#
#   EXTC_RECLAIM=1 extc → cc -std=c11 -O2 -fwrapv → valgrind --tool=callgrind
#
# 判据两条：① 每个形状的总指令数 ≤ 它的线（线按**工具链键**记在 shape-baselines.tsv 里）；
# ② 开档输出与关档（`EXTC_RECLAIM=0`）**逐字相同** —— 性能判据不许靠改语义换。
#
# 为什么线要按工具链记：callgrind 的 Ir 只对“同一份 C + 同一条工具链”是确定的 —— 换 gcc/glibc，
# 编译决定与库实现都会变（连 glibc 的 IFUNC 都会按 CPU 派发 memcpy）。同一条绝对线在别的机器上
# 是**假红**：同一份 extc 生成的 C，本机（gcc 15.2.0+glibc 2.43）bt 2 850 953，CI（gcc 13.3.0+
# glibc 2.39）bt 3 018 399。键不在表里 ⇒ 打出实测值并 `  --   跳过：…`（**明说没验**，不装绿）。
#
# 用法：tests/reclaim-perf/gate-shapes.sh
#   EXTC=... / CC=...             指定编译器
#   BT_LIMIT=... / RB_LIMIT=...   覆盖判据线（调试 / 自检"闸门有牙"）
#   SHAPE_TOOLKEY=...             覆盖工具链键（自检"表里没这个键就跳过"那条路）
#
# 缺 valgrind：每个形状打印 `  --   跳过：…`（指令数判据没验），**语义一致那半照判**；退出码 0
#   —— `tools/check.py` 把这一项记 `skip`、不计数（CI 装了 valgrind ⇒ 不许跳过）。
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
BIG=$ROOT/bench/bigmatrix
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
# 线按**工具链键**从表里取（列与来历见 tests/reclaim-perf/shape-baselines.tsv）：
#   键 = <cc -dumpfullversion>+glibc-<ldd --version 最后一列>；表里没有这个组合 ⇒ 判据没验。
BASE=$DIR/shape-baselines.tsv
CC_VER=$($CC -dumpfullversion 2>/dev/null || $CC -dumpversion 2>/dev/null || echo ?)
GLIBC_VER=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')
TOOLKEY=${SHAPE_TOOLKEY:-"$CC_VER+glibc-${GLIBC_VER:-?}"}
read -r TBL_BT TBL_RB <<<"$(awk -F'\t' -v k="$TOOLKEY" '!/^#/ && $1==k && $3!="" && $5!="" {print $3" "$5; exit}' "$BASE" 2>/dev/null)"
BT_LIMIT=${BT_LIMIT:-$TBL_BT}
RB_LIMIT=${RB_LIMIT:-$TBL_RB}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }   # 缺件 ⇒ 明说"没验"（check.py 认这一行 ⇒ 记 skip、不计数）
VG=$(command -v valgrind || true)

echo "== 回收性能判据（真实形状，小输入，-O2）=="
echo "  工具链键 $TOOLKEY · 线：bt ${BT_LIMIT:-（表里没有）} · rebuild ${RB_LIMIT:-（表里没有）}（$BASE）"

if [ ! -f "$BIG/in-small/bt.txt" ] || [ ! -f "$BIG/in-small/rebuild.txt" ]; then
    if ( cd "$BIG" && cc -O2 -o "$TMP/gen" gen.c && "$TMP/gen" small "$BIG/in-small" ) >"$TMP/gen.log" 2>&1; then
        echo "  注：小输入不在（生成物）⇒ 就地生成了一份（bench/bigmatrix/gen.c small）"
    else
        bad "生成小输入失败（bench/bigmatrix/gen.c）"
        tail -3 "$TMP/gen.log" | sed 's/^/    /'
        exit 1
    fi
fi

fail=0
one() { # <形状> <线>
    local shp=$1 lim=$2
    local on_c=$TMP/$shp.on.c off_c=$TMP/$shp.off.c
    EXTC_RECLAIM=1 "$EXTC" -I "$ROOT/stdlib" "$BIG/src/$shp.extc" -o "$on_c" >"$TMP/$shp.gen.log" 2>&1 \
        && $CC -std=c11 -O2 -fwrapv -o "$TMP/$shp.on" "$on_c" >>"$TMP/$shp.gen.log" 2>&1
    if [ ! -x "$TMP/$shp.on" ]; then
        bad "$shp：生成 / 编译失败"; tail -3 "$TMP/$shp.gen.log" | sed 's/^/    /'; fail=1; return
    fi
    EXTC_RECLAIM=0 "$EXTC" -I "$ROOT/stdlib" "$BIG/src/$shp.extc" -o "$off_c" >/dev/null 2>&1 \
        && $CC -std=c11 -O2 -fwrapv -o "$TMP/$shp.off" "$off_c" >/dev/null 2>&1

    local o_on o_off rc_on rc_off
    o_on=$("$TMP/$shp.on" < "$BIG/in-small/$shp.txt" 2>&1); rc_on=$?
    o_off=$("$TMP/$shp.off" < "$BIG/in-small/$shp.txt" 2>&1); rc_off=$?
    if [ "$rc_on" -eq 0 ] && [ "$rc_on" = "$rc_off" ] && [ "$o_on" = "$o_off" ]; then
        ok "$shp：开档/关档输出逐字一致（rc=0）"
    else
        bad "$shp：开档与关档输出不一致（rc $rc_on / $rc_off）"; fail=1; return
    fi

    if [ -z "$VG" ]; then
        skip "$shp：没有 valgrind ⇒ 指令数判据没验（开档/关档输出一致那条已判）"
        return
    fi
    if ! valgrind --tool=callgrind --callgrind-out-file="$TMP/$shp.cg" \
             "$TMP/$shp.on" < "$BIG/in-small/$shp.txt" >/dev/null 2>"$TMP/$shp.vg"; then
        bad "$shp：callgrind 那跑失败"; tail -3 "$TMP/$shp.vg" | sed 's/^/    /'; fail=1; return
    fi
    local ir
    ir=$(grep '^summary:' "$TMP/$shp.cg" | awk '{print $2}')
    if [ -z "${ir:-}" ]; then bad "$shp：callgrind 输出没有 summary 行"; fail=1; return; fi
    if [ -z "$lim" ]; then
        skip "$shp：$TOOLKEY 不在 shape-baselines.tsv 里 ⇒ 指令数判据没验（实测 $ir；抄进表里再 ×1.05 定线）"
        return
    fi
    if [ "$ir" -le "$lim" ]; then
        ok "$shp：总指令数 $ir ≤ $lim（in-small/$shp.txt）"
    else
        bad "$shp：总指令数 $ir > $lim（回收开销被付回了）"; fail=1
    fi
}

one bt "$BT_LIMIT"
one rebuild "$RB_LIMIT"
exit "$fail"
