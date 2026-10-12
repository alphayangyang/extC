#!/usr/bin/env bash
# tests/reclaim-perf/run.sh —— 复跑"回收运行期为什么慢"的归因矩阵。
#
# 只读 `tests/reclaim-shapes/cyclic_pressure.extc` 与 `prototype-reclaim/stress.c`，
# 只写自己的临时目录；`src/` 一个字节都不动。
#
# 三件事：
#   ① 生成开关开/关、以及各"单项消融/原型"变体（全部经 python 脚本改生成物副本）；
#   ② 校验所有变体与基线**输出逐字一致**（改运行期不该改语义）；
#   ③ 每份跑 callgrind，打印指令数总表 + 相对基线的差值（归因靠差值）。
#
# 用法：tests/reclaim-perf/run.sh
#   N=200000         # callgrind 的轮数（默认 2e5；矩阵规模小，整轮 ~1 分钟量级）
#   SMALL=20000      # 语义校验用的轮数
#   SKIP_PROTO=1     # 不编 C 原型（没有 cc 或不想等）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
SRC=${SRC:-$ROOT/tests/reclaim-shapes/cyclic_pressure.extc}
PROTO=${PROTO:-$ROOT/prototype-reclaim/stress.c}
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
N=${N:-200000}
SMALL=${SMALL:-20000}
FLOOR_TIB=1099511627776

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# ---- 固定编译器：`extc` 按**自身所在目录**找 stdlib，所以拷到别处时要显式 -I。------
cp "$EXTC" "$TMP/extc"; chmod +x "$TMP/extc"
EXTC_MD5=$(md5sum "$EXTC" | cut -d' ' -f1)
echo "== 回收运行期归因矩阵 =="
echo "  repo      $ROOT"
echo "  HEAD      $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '—')"
echo "  extc      $EXTC_MD5（已拷到 $TMP/extc 固定住；构建产物在本轮中途变化不影响结果）"
echo "  形状      $SRC"
echo "  规模      callgrind N=$N，语义校验 N=$SMALL"
if [ -n "$(git -C "$ROOT" status --porcelain -- src/back 2>/dev/null)" ]; then
    echo "  ⚠ src/back 有未提交改动（别人正在改），数字绑在上面这个 extc 的 md5 上"
fi

gen() { # <out.c> <env:on|off>；其余参数是 extc 参数
    local out=$1 mode=$2; shift 2
    if [ "$mode" = on ]; then EXTC_RECLAIM=1 "$TMP/extc" -I "$ROOT/stdlib" "$@" -o "$out"
    else unset EXTC_RECLAIM; "$TMP/extc" -I "$ROOT/stdlib" "$@" -o "$out"; fi
}

echo
echo "-- ① 生成变体 --"
gen "$TMP/on.c"  on  "$SRC" || { echo "编译 $SRC 失败"; exit 1; }
gen "$TMP/off.c" off "$SRC" || exit 1
python3 "$DIR/rootprobe.py"  "$TMP/on.c" "$TMP/probe.c"     >/dev/null
python3 "$DIR/latemark.py"   "$TMP/on.c" "$TMP/late.c"      >/dev/null
for m in guardfast guardfirst nohold noreg0; do
    python3 "$DIR/ablate.py" "$m" "$TMP/on.c" "$TMP/$m.c"   >/dev/null
done
for t in 2824 5648 11296; do
    python3 "$DIR/ablate.py" "pad$t" "$TMP/on.c" "$TMP/pad$t.c" >/dev/null
done
# "清扫永不触发"：同一份生成物，编译期把压力线宏换掉（见 ② 的 build）。
cp "$TMP/on.c" "$TMP/nosweep.c"
echo "  ok"

echo
echo "-- ② 编译 --"
build() { # <name>
    local n=$1
    case $n in
        nosweep) $CC -std=c11 -fwrapv -O1 -DEXTC_RC_FLOOR=$FLOOR_TIB -o "$TMP/$n" "$TMP/$n.c" ;;
        *)       $CC -std=c11 -fwrapv -O1 -o "$TMP/$n" "$TMP/$n.c" ;;
    esac
}
VARIANTS="on nosweep off guardfast guardfirst nohold noreg0 late pad2824 pad5648 pad11296"
for v in $VARIANTS; do build "$v" || { echo "  构建 $v 失败"; exit 1; }; done
build probe
if [ -z "${SKIP_PROTO:-}" ] && [ -f "$PROTO" ]; then
    $CC -std=c11 -fwrapv -O1 -o "$TMP/proto_O1" "$PROTO" 2>/dev/null
    $CC -std=c11 -O2          -o "$TMP/proto_O2" "$PROTO" 2>/dev/null
fi
echo "  ok"

echo
echo "-- ③ 语义校验（N=$SMALL，全部必须与 on 逐字一致）--"
"$TMP/on" "$SMALL" > "$TMP/ref.out"
ref=$(md5sum "$TMP/ref.out" | cut -d' ' -f1)
echo "  on           $(cat "$TMP/ref.out")"
for v in $VARIANTS; do
    [ "$v" = on ] && continue
    if "$TMP/$v" "$SMALL" > "$TMP/$v.out" 2>/dev/null && [ "$(md5sum "$TMP/$v.out" | cut -d' ' -f1)" = "$ref" ]; then
        printf '  %-11s 一致\n' "$v"
    else
        printf '  %-11s **不一致 / 崩**（该档只能当上界读）\n' "$v"
    fi
done
printf '  %-11s %s\n' "probe" "$("$TMP/probe" "$SMALL" 2>&1 >/dev/null | grep -c extc_probe) 行统计"

echo
echo "-- ④ callgrind（N=$N）--"
meas() { # <name> <bin> [args...]
    local n=$1 bin=$2; shift 2
    valgrind --tool=callgrind --callgrind-out-file="$TMP/cg.$n" "$bin" "$N" "$@" \
        >/dev/null 2>"$TMP/$n.vg" || true
    grep '^summary:' "$TMP/cg.$n" | awk '{print $2}' > "$TMP/$n.ir"
    callgrind_annotate --auto=no --threshold=100 "$TMP/cg.$n" > "$TMP/$n.annot" 2>/dev/null
    rm -f "$TMP/cg.$n"
    echo "  $n $(cat "$TMP/$n.ir")"
}
for v in $VARIANTS; do meas "$v" "$TMP/$v"; done
if [ -x "$TMP/proto_O1" ]; then
    meas proto_O1 "$TMP/proto_O1" 1024 64 20000 48 8
    meas proto_O2 "$TMP/proto_O2" 1024 64 20000 48 8
fi

echo
echo "-- ⑤ 指令数总表 --"
printf '  %-12s %16s %10s\n' 变体 Ir "每轮"
for v in $VARIANTS proto_O1 proto_O2; do
    [ -f "$TMP/$v.ir" ] || continue
    ir=$(cat "$TMP/$v.ir")
    awk -v n="$v" -v ir="$ir" -v N="$N" 'BEGIN{printf "  %-12s %16d %10.1f\n", n, ir, ir/N}'
done

echo
echo "-- ⑥ 归因：与基线的差值（每个差值 = 那一块的值）--"
on=$(cat "$TMP/on.ir")
delta() { awk -v b="$on" -v x="$1" -v n="$2" -v N="$N" \
    'BEGIN{printf "  %-34s %16d  %6.2f%% of on  %.2f 每轮\n", n, b-x, 100*(b-x)/b, (b-x)/N}'; }
delta "$(cat "$TMP/guardfirst.ir")" "根表物化 (on-guardfirst)"
delta "$(cat "$TMP/late.ir")"      "根表物化 (on-late) 校核"
delta "$(cat "$TMP/guardfast.ir")" "owns 二分 (on-guardfast)"
delta "$(cat "$TMP/nohold.ir")"    "计数上界 (on-nohold)"
delta "$(cat "$TMP/nosweep.ir")"   "标记+清扫 (on-nosweep)"
delta "$(cat "$TMP/noreg0.ir")"    "空 owner 登记 (on-noreg0)"
delta "$(cat "$TMP/off.ir")"       "回收总开销 (on-off)"
awk -v b="$on" -v x="$(cat "$TMP/pad2824.ir")" -v N="$N" \
    'BEGIN{calls=N+20000+int(N/16); d=calls*(2824-1412);
           printf "  %-34s %16d  %6.2f%% of on\n", "pad2824 多出的 (槽×分配点)", x-b, 100*(x-b)/b;
           printf "  %-34s %16.4f\n", "→ 每个(槽×分配点) 的指令数", (x-b)/d}'

echo
echo "-- ⑦ owns 守卫与根表的形状（探针，N=$N）--"
"$TMP/probe" "$N" 2>&1 >/dev/null | grep '^extc_probe ' | sed 's/^/  /'

echo
echo "-- ⑧ 关键函数的自身指令数（on）--"
awk 'match($0,/^\s*([0-9,]+) \(\s*([0-9.]+)%\)\s+\S+:(main|extc_rc_[A-Za-z]+)/,m){
        printf "  %-26s %16s  %s%%\n", m[3], m[1], m[2]}' "$TMP/on.annot" | head -14

echo
echo "完成。（临时目录已删；矩阵数字绑在上面那个 extc 的 md5 上）"
