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
# 判据两条：① 每个形状的总指令数 ≤ 它的线（= 落地后实测 × 1.05，callgrind 的 Ir 是确定的）；
# ② 开档输出与关档（`EXTC_RECLAIM=0`）**逐字相同** —— 性能判据不许靠改语义换。
#
# 用法：tests/reclaim-perf/gate-shapes.sh
#   EXTC=... / CC=...             指定编译器
#   BT_LIMIT=... / RB_LIMIT=...   覆盖判据线（调试 / 自检"闸门有牙"）
#
# 缺 valgrind：每个形状打印 `  --   跳过：…`（指令数判据没验），**语义一致那半照判**；退出码 0
#   —— `tools/check.py` 把这一项记 `skip`、不计数（CI 装了 valgrind ⇒ 不许跳过）。
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
BIG=$ROOT/bench/bigmatrix
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
# 线 = 实测 × 1.05（`-O2`，`in-small`；callgrind Ir，逐次跑是确定值）：
#   bt     2 850 624 → 2 950 000 —— ②（区域不需要回收 ⇒ 整段不发射 rc）把 bt 的回收运行期
#          整段拿掉后从 3 091 855 掉到这里；线跟着往下拧（棘轮只许压）。
#   rebuild 15 037 125 → 15 790 000 —— 细化 (a′)+(b′)（"循环里建、每轮丢"）让 rebuild 也整单元
#          不发射 rc：从 23 075 542 掉到这里（-34.8%），线跟着往下拧（棘轮只许压）。
BT_LIMIT=${BT_LIMIT:-2950000}
RB_LIMIT=${RB_LIMIT:-15790000}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }   # 缺件 ⇒ 明说"没验"（check.py 认这一行 ⇒ 记 skip、不计数）
VG=$(command -v valgrind || true)

echo "== 回收性能判据（真实形状，小输入，-O2）=="

if [ ! -f "$BIG/in-small/bt.txt" ] || [ ! -f "$BIG/in-small/rebuild.txt" ]; then
    if ( cd "$BIG" && cc -O2 -o build/gen gen.c && ./build/gen small in-small ) >"$TMP/gen.log" 2>&1; then
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
    if [ "$ir" -le "$lim" ]; then
        ok "$shp：总指令数 $ir ≤ $lim（in-small/$shp.txt）"
    else
        bad "$shp：总指令数 $ir > $lim（回收开销被付回了）"; fail=1
    fi
}

one bt "$BT_LIMIT"
one rebuild "$RB_LIMIT"
exit "$fail"
