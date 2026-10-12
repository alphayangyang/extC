#!/usr/bin/env bash
# tests/reclaim-shapes/hole_c/evidence.sh —— 计数路径定位的**硬证据**（只动 /tmp 里的生成物）。
#
# 往 `hole_c.extc` 的**生成 C**（不是 src/）插三处探针：
#   · `extc_rc_freeSlot`：进来时 `s->hold[i] != 0` 就记一笔（槽还有持有者却被释放）；
#   · `extc_rc_collect` ：清扫后打印 `nroots / marked / freed / freed_held / maxhold`。
#
# 为什么这一笔能定性：`extc_rc_holdUpd` 只在 `hold[i]` **减到 0** 时才调 `freeSlot`
# （生成物里就是 `if (s->hold[i] == 0) extc_rc_freeSlot(s, i);`），所以"进 freeSlot 时
# hold != 0"只可能来自 `extc_rc_sweep` —— 即：释放不是计数归零干的，是**清扫**干的。
#
# 期望输出（实测见 README.md）：`nroots=0` 且 `freed_held == freed`、`maxhold = 2`。
# 也就是说：清扫的种子（根表 + 跨区登记位）是空集，它把**还有 1~2 个持有者**的槽全放掉了。
#
# 退出码：0 = 证据成立（确实"种子为空时放掉了还有持有者的槽"）；2 = 环境/编译失败。
set -u
cd "$(dirname "$0")/../../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
SRC=tests/reclaim-shapes/hole_c/hole_c.extc
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

command -v python3 >/dev/null || { echo "需要 python3 才能插探针"; exit 2; }
[ -x "$EXTC" ] || { echo "缺少可执行编译器 $EXTC（先 make）"; exit 2; }

EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$SRC" -o "$TMP/probe.c" || { echo "生成失败"; exit 2; }

python3 - "$TMP/probe.c" "$TMP/probe2.c" <<'PY' || exit 2
import sys
src = open(sys.argv[1]).read()

# ① 探针计数器
anchor = 'static ExtcRcSlab *extc_rc_slabNew'
assert src.count(anchor) == 1, "anchor slabNew"
src = src.replace(anchor, 'static long long __probe_held, __probe_maxhold;\n' + anchor, 1)

# ② freeSlot：进函数时记录"还有持有者"
anchor = '    extc_rc_unlinkChildren(s, i);             /* 先撤掉它手里的跨区边（字段还完好） */'
assert src.count(anchor) == 1, "anchor freeSlot"
probe = ('    if (s->hold[i]) { __probe_held++; '
         'if ((long long)s->hold[i] > __probe_maxhold) __probe_maxhold = (long long)s->hold[i]; }\n')
src = src.replace(anchor, probe + anchor, 1)

# ③ collect：清扫后打印
anchor = '    size_t freed = extc_rc_sweep(r);'
assert src.count(anchor) == 1, "anchor collect"
dump = (anchor + '\n'
        '    fprintf(stderr, "PROBE collect nroots=%zu marked=%zu freed=%zu freed_held=%lld maxhold=%lld\\n",\n'
        '            nroots, marked, freed, __probe_held, __probe_maxhold);')
src = src.replace(anchor, dump, 1)

open(sys.argv[2], 'w').write(src)
print("探针已插入：freeSlot(hold!=0) + collect(nroots/freed/freed_held/maxhold)")
PY

"$CC" -std=c11 -fwrapv -O1 -o "$TMP/probe" "$TMP/probe2.c" || { echo "探针生成物编译失败"; exit 2; }
sh -c '"$0" >/dev/null 2>"$1"' "$TMP/probe" "$TMP/probe.err" 2>/dev/null; rc=$?

echo "== hole_c：清扫释放了【还有持有者】的槽（生成物探针）=="
echo "   运行退出码：$rc（139/134 = 提前释放后可观测的崩溃）"
grep -m1 'PROBE collect' "$TMP/probe.err" | sed 's/^/   /'
last=$(grep 'PROBE collect' "$TMP/probe.err" | tail -1)
echo "   最后一次清扫：$(printf '%s' "$last" | sed 's/PROBE collect //')"
echo "   （清扫次数：$(grep -c 'PROBE collect' "$TMP/probe.err")）"

nroots=$(printf '%s' "$last" | grep -o 'nroots=[0-9]*' | cut -d= -f2)
freed=$(printf '%s' "$last" | grep -o 'freed=[0-9]*' | cut -d= -f2)
held=$(printf '%s' "$last" | grep -o 'freed_held=[0-9]*' | cut -d= -f2)
maxh=$(printf '%s' "$last" | grep -o 'maxhold=[0-9]*' | cut -d= -f2)

if [ -n "$nroots" ] && [ "$nroots" = 0 ] && [ -n "$freed" ] && [ -n "$held" ] \
   && [ "$held" = "$freed" ] && [ "${maxh:-0}" -ge 1 ]; then
    echo
    echo "证据成立：nroots=0（种子空）· freed=$freed · freed_held=$held（全部还有持有者）· maxhold=$maxh"
    echo "⇒ 提前释放不是计数归零，是清扫在**根表为空**时放掉了 hold>0 的槽。"
    exit 0
fi
echo
echo "证据不成立：nroots=${nroots:-—} freed=${freed:-—} freed_held=${held:-—} maxhold=${maxh:-—}"
exit 1
