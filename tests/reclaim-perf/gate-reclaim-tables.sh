#!/usr/bin/env bash
# tests/reclaim-perf/gate-reclaim-tables.sh —— 回收运行期两张「按结构体规模存活的表」的
# **缩放**判据：own 登记表（ownAdd/ownDel）与每 slab 的小类型表（typeIdx）。
#
# 由来（复杂度审查）：own 登记表曾是「按基址排序的数组 + 每次插入/删除 memmove 整段」，
# 维护成本 O(存活 slab 数)：S 块 slab 的整区释放是 O(S²) 的指针搬动（1 GiB 堆约 16k 块
# ⇒ 累计搬 ~1 GiB，10 GiB 堆到秒级）。小类型表曾是「每次分配线性扫本块落过的类型数」，
# 泛型单态化把上千个分配点塞进同一尺寸类时是每分配 O(T)（实测 T=1→256：133→770 Ir/分配）。
# 现行实现：own 登记表 = 开放寻址哈希（线性探测 + Knuth 回移删除），小类型表 = 描述符少时
# 线性扫、超过 EXTC_RC_TTAB_LINEAR_MAX 后建 16 位开放寻址索引。
#
# 为什么是 callgrind 指令数：确定量（同一二进制同一输入逐位相同），不受负载/频率影响；
# 这条判据要的是「每次操作的成本与表规模无关」这个结构性事实。探针量的都是**发射出来的**
# 那份运行期文本（emit_main 现场发射），不是另抄一份实现。
#
# 判据（三条）：
#   ① own 表：每次 add+del 的指令数 S=1024 与 S=4096 之比 ≤ RATIO（默认 2.0；实测新实现
#     ~0.92，把探测换成整表线性扫后 ~4×）
#   ② own 表：每次 add+del 的指令数绝对值 ≤ ABS（默认 800；实测 ~417）
#   ③ 小类型表：每次 typeIdx 的指令数 T=8 与 T=256 之比 ≤ RATIO_T（默认 2.0；实测 ~0.83，
#     强制走线性扫后 ~6×），绝对线 ≤ ABS_T（默认 200；实测 ~38）
#
# --selftest：分别把探测换回整表线性扫、把类型表的哈希分支短路成线性扫，要求①②③**变红**
#   （有牙），且真实运行时全绿（先绿后红都验到）。
#
# 缺 valgrind：打印一行 跳过：… 后退出 0（check.py 记 skip；CI 装了 valgrind ⇒ 不许跳过）。
#
# 用法：tests/reclaim-perf/gate-reclaim-tables.sh [--selftest]
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
CC=${CC:-cc}
S1=${S1:-1024}
RATIO=${RATIO:-2.0}
ABS=${ABS:-800}
T1=${T1:-8}
T2=${T2:-256}
TN=${TN:-200000}
RATIO_T=${RATIO_T:-2.0}
ABS_T=${ABS_T:-200}
BITS=12

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }

VG=$(command -v valgrind || true)

echo "== 登记表 / 小类型表缩放判据（S=$S1 / $((S1 * 4))，T=$T1 / $T2）=="

if [ -z "$VG" ]; then
    skip "没有 valgrind（本机缺 callgrind）⇒ 缩放判据这轮没验"
    exit 0
fi

echo "== 发射运行期文本 =="
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I "$ROOT/src/base" -I "$ROOT/src/back" \
        "$ROOT/tests/reclaim/emit_main.c" "$ROOT/src/back/reclaim.c" "$ROOT/src/base/base.c" \
        -o "$TMP/emit" 2>"$TMP/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$TMP/emit.log"; exit 1
fi
if ! "$TMP/emit" > "$TMP/extc_reclaim_runtime.inc" 2>"$TMP/emit.err" \
        || [ ! -s "$TMP/extc_reclaim_runtime.inc" ]; then
    bad "发射运行期文本失败"; sed 's/^/    /' "$TMP/emit.err"; exit 1
fi
ok "发射运行期文本（$(wc -c < "$TMP/extc_reclaim_runtime.inc") 字节）"

build_probe() {   # $1 = 运行期文本, $2 = 探针源文件, $3 = 可执行文件
    local inc=$1 src=$2 exe=$3 dir=$TMP/inc.$(basename "$3")
    mkdir -p "$dir" && cp "$inc" "$dir/extc_reclaim_runtime.inc" || return 1
    if ! $CC -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -fwrapv -I "$dir" \
            -DEXTC_RC_SLAB_BITS="$BITS" "$src" -o "$exe" 2>"$TMP/cc.log"; then
        bad "探针编译失败（$src）"; sed 's/^/    /' "$TMP/cc.log"; return 1
    fi
    return 0
}

measure() {   # $1 = 可执行文件, 其余 = 参数, 输出 Ir
    local exe=$1 tag=$TMP/cg.$$
    shift
    if ! "$VG" --tool=callgrind --callgrind-out-file="$tag" "$exe" "$@" \
            >"$TMP/run.log" 2>"$TMP/vg.log"; then
        bad "callgrind 跑失败（$exe $*）"; tail -5 "$TMP/vg.log" | sed 's/^/    /'; return 1
    fi
    local ir
    ir=$(grep '^summary:' "$tag" | awk '{print $2}')
    rm -f "$tag"
    if [ -z "${ir:-}" ]; then bad "callgrind 输出缺 summary"; return 1; fi
    echo "$ir"
    return 0
}

run_suite() {   # $1 = 运行期文本；打印三档判据并返回 0/1
    local inc=$1 p_own=$TMP/own.$$ p_ttab=$TMP/ttab.$$
    build_probe "$inc" "$DIR/own-scale.c" "$p_own" || return 1
    build_probe "$inc" "$DIR/ttab-scale.c" "$p_ttab" || return 1
    local o1 o4 t1 t2
    o1=$(measure "$p_own" "$S1") || return 1
    o4=$(measure "$p_own" "$((S1 * 4))") || return 1
    t1=$(measure "$p_ttab" "$T1" "$TN") || return 1
    t2=$(measure "$p_ttab" "$T2" "$TN") || return 1
    awk -v o1="$o1" -v o4="$o4" -v s1="$S1" -v t1="$t1" -v t2="$t2" \
        -v ratio="$RATIO" -v abs="$ABS" -v ro="$RATIO_T" -v abt="$ABS_T" \
        -v T1="$T1" -v T2="$T2" -v TN="$TN" '
    BEGIN {
        po1 = o1 / (2 * s1); po4 = o4 / (2 * 4 * s1); rown = po4 / po1
        pt1 = t1 / TN; pt2 = t2 / TN; rtab = pt2 / pt1
        printf "  own：S=%-6d 每次 add+del %6.1f Ir；S=%-6d %6.1f Ir（比 %.3f，线 ≤ %s，绝对线 ≤ %s）\n",
               s1, po1, 4 * s1, po4, rown, ratio, abs
        printf "  ttab：T=%-3d 每次 %6.1f Ir；T=%-3d %6.1f Ir（比 %.3f，线 ≤ %s，绝对线 ≤ %s）\n",
               T1, pt1, T2, pt2, rtab, ro, abt
        exit !((rown <= ratio) && (po4 <= abs) && (rtab <= ro) && (pt2 <= abt))
    }'
}

if ! run_suite "$TMP/extc_reclaim_runtime.inc"; then
    bad "登记表或小类型表的每次操作成本随表规模上涨（维护退化成线性了？）"
    exit 1
fi
ok "登记表维护成本与存活 slab 数无关"
ok "小类型表查找成本与块内类型数无关"

# ---- --selftest：分别注入两种线性形状，判据必须变红 -------------------------------
if [ "${1:-}" = "--selftest" ]; then
    echo "== 红自测：注入整表线性扫描的探测 =="
    if ! python3 - "$TMP/extc_reclaim_runtime.inc" "$TMP/linown.inc" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
pat = re.compile(r'    while \(tab\[i\] && extc_rc_ownKeyOf\(tab\[i\]\) != key\) i = \(i \+ 1\) & \(cap - 1\);\n')
new = '    for (i = 0; i < cap; i++) if (!tab[i] || extc_rc_ownKeyOf(tab[i]) == key) break;\n'
t2, n = pat.subn(new, t, count=1)
if n != 1:
    raise SystemExit("注入失败：找不到登记表探测循环（发射文本形状变了？）")
open(dst, 'w').write(t2)
PY
    then
        bad "红自测注入失败（own）"; exit 1
    fi
    if ! run_suite "$TMP/linown.inc"; then
        ok "红自测：登记表探测退回线性扫描后判据变红（有牙）"
    else
        bad "红自测失败：登记表线性扫描下判据仍为绿"; exit 1
    fi

    echo "== 红自测：把小类型表的哈希分支短路成线性扫 =="
    if ! python3 - "$TMP/extc_reclaim_runtime.inc" "$TMP/linttab.inc" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
pat = re.compile(r'    if \(s->thash\) \{')
t2, n = pat.subn('    if (0) {', t, count=1)
if n != 1:
    raise SystemExit("注入失败：找不到小类型表的哈希分支（发射文本形状变了？）")
open(dst, 'w').write(t2)
PY
    then
        bad "红自测注入失败（ttab）"; exit 1
    fi
    if ! run_suite "$TMP/linttab.inc"; then
        ok "红自测：小类型表退回线性扫后判据变红（有牙）"
    else
        bad "红自测失败：小类型表线性扫下判据仍为绿"; exit 1
    fi
fi

exit 0
