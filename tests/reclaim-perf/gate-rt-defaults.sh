#!/usr/bin/env bash
# tests/reclaim-perf/gate-rt-defaults.sh —— 生成物里"运行期注释**声称**的默认值"必须与同一份
# 产物里**真实的** `#define` 相等。
#
# 由来：这段运行期注释曾写着"默认 0 = 保持「计数 + 提前回收」"，而产物里真实的默认是
# `#define EXTC_RC_NOCOUNT 1`（计数入口整体编掉）—— **注释与事实漂移**，靠人读才发现。
# 本判据把它变成机器可查：注释里只要是"默认 `<VAR>=<值>`"这种**声称了具体值**的写法，
# 就与产物里的 `#define <VAR> <值>` 比；没声称值的不强求。
#
# 便宜：只编译**一份**默认档产物、不跑程序。`EXTC=` 可覆盖（变异体牙口就是这么打的）。
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
# 挑一个**默认档就会发射回收运行期**、且不被"事件档自动那一半"改写成 0 的形状：
# `cyclic_pressure.extc` 是环负载（`unitAllAcyclic` 不成立 ⇒ 不会自动切事件档）。
SRC=${SRC:-tests/reclaim-shapes/cyclic_pressure.extc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

if ! "$EXTC" -w -I stdlib "$SRC" -o "$TMP/rt.c" 2>"$TMP/err"; then
    bad "生成失败（$SRC）：$(head -1 "$TMP/err")"; exit 1
fi
if ! grep -q '^#define EXTC_RC_NOCOUNT' "$TMP/rt.c"; then
    bad "挑的源没发射回收运行期（$SRC）⇒ 判据会瞎"; exit 1
fi

# 注释里的声称值：`默认 \`VAR=值\``；`[`]` 的写法避免在双引号里跟命令替换撞。
claimed() { grep -oE '默认 `?'"$1"'=[0-9]+' "$TMP/rt.c" | head -1 | grep -oE '[0-9]+$'; }
actual()  { grep -oE "^#define $1 [0-9]+" "$TMP/rt.c" | head -1 | awk '{print $3}'; }

echo "== 默认档生成物：$SRC（只编译不跑）=="
n=0
for var in $(grep -oE '^#define (EXTC_RC_[A-Z_]+)' "$TMP/rt.c" | awk '{print $2}' | sort -u); do
    c=$(claimed "$var"); a=$(actual "$var")
    [ -z "$c" ] && continue                 # 没声称具体值 ⇒ 不管
    n=$((n+1))
    if [ "$c" = "$a" ]; then
        ok "$var：注释声称默认 $c，产物 #define $a —— 一致"
    else
        bad "$var：注释声称默认 $c，产物 #define ${a:-（没有）} —— **漂移**"
    fi
done
[ "$n" -gt 0 ] || bad "产物里没有一处\"声称了默认值\"的注释 ⇒ 判据会瞎（检查注释措辞是否变了）"

echo
echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ] || exit 1
echo "  ok   运行期注释里声称的默认值与产物一致"
