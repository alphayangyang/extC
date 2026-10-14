#!/usr/bin/env bash
# tests/reclaim-perf/gate.sh —— 回收性能判据（两次"先红后绿"都钉在这里）。
#
# 判据：开关开的 `cyclic_pressure` 在固定 N=200000 下，callgrind 的**总指令数 ≤ LIMIT**。
# 判据线随落地过一次就**收紧一次**（与 known-bad 棘轮同一条规矩：优化拿到的收益不许被付回）。
#
#   改动前实测（`build/extc` md5 5e51d77f…）：1 169 388 408 Ir
#   ① 分配点入口守卫落地（根表只在真要扫时物化）：510 475 244 Ir
#      —— 当时把线定在 701 633 044（改动前 × 0.6，解释见 README §3.2 的 56.1%）。
#   ② `owns` 登记表查询 O(1) 化落地（直接映射 + 校验，未命中回落二分）：404 358 444 Ir
#      —— 线收到 425 000 000。
#   ③ 赋值路径每个指针只查一次登记表（`storeRefOwned` 复用 slab；`ownSlab` 命中路径
#      省掉冗余比对，但保留"映射格非空"那一项）：339 186 860 Ir
#   ⑩ 删掉 `ExtcRcSlab.used`（只写不读的死字段）：195 517 873 Ir。
#   ⑨ 压力阈值缓存进区域（只在 live/floor/scale 变时重算）：195 939 957 Ir。
#   ⑧ 默认改成**无计数**（`EXTC_RC_NOCOUNT=1` 成为默认）：198 497 440 Ir（−24.6 %）。
#   ⑦ `extc_rc_storeRefOwned` 改成 `static inline`（只给 inline、不给 always_inline）：263 453 680 Ir。
#   ⑥ `regionOf` 结果做成函数级局部量（区域在帧内是不变量；协程不 hoist）：266 437 299 Ir。
#   ⑤ `rsIncN==0` 时跳过"撤跨区边"的走查（进程里一次跨区登记都没发生过 ⇒ 无物可撤）：
#      本形状 308 485 778 → **279 224 856 Ir**；同一守卫让 `rebuild` −10.0 %、`bt` −5.3 %。
#   ④ 热路径小助手一律 `static inline`（`ownSlab` 一次运行被调用上亿次，调用/返回本身就是一笔）：
#      **308 485 778 Ir** ⇒ 现行 LIMIT = 324 000 000（= 实测 × 1.05）。
#      注：**只给 `inline`，不给 `always_inline`** —— 强制内联量过：`bt` −14.2 % 但 `rebuild`
#      反而 +0.5 %（代码变大）；让编译器按代价模型决定，三个形状一起降
#      （`bt` −10.7 %、`rebuild` −9.9 %、本形状 −9.1 %）。
#   ⑪ 槽元数据瘦身（`task-19`）：每槽类型描述符**指针**（8 B）→ 16 位**类型号**（2 B）+ 每 slab
#      小类型表；槽步长 2 的幂 → 16 字节取整（由指针找槽号从一次移位变成一次乘倒数右移）。
#      本形状 **154 214 566 Ir** ⇒ 线收到 161 930 000（= 实测 × 1.05；中间态"只换类型号"是
#      165 100 449 Ir，被这一项的缓存收益盖过去了）。
#
# 口径与 `run.sh` 一致：pinned `extc` → `EXTC_RECLAIM=1` 生成 → `cc -std=c11 -fwrapv -O1`
# → `valgrind --tool=callgrind` 的 `summary:` 行。只写自己的临时目录，`src/` 一个字节不动。
#
# 用法：tests/reclaim-perf/gate.sh        退出码 0 = 绿（判据成立）
#   EXTC=...    指定编译器（默认 <repo>/build/extc）
#   N=200000    callgrind 的轮数（LIMIT 只对默认 N=200000 成立，改 N 请同时改 LIMIT）
#   LIMIT=...   覆盖判据线（调试用）
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
SRC=${SRC:-$ROOT/tests/reclaim-shapes/cyclic_pressure.extc}
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
N=${N:-200000}
LIMIT=${LIMIT:-161653535}          # 本地槽只写入口落地后实测 153955748 × 1.05（棘轮只许往下；上一线 161930000）

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== 回收性能判据（EXTC_RECLAIM=1，N=$N）=="
echo "  形状   $SRC"
echo "  编译   EXTC_RECLAIM=1 extc -I <repo>/stdlib → cc -std=c11 -fwrapv -O1"
echo "  判据   总指令数 ≤ $LIMIT（现行线 = 守卫 O(1) 化落地后实测 × 1.05）"

# 固定编译器：`extc` 按自身所在目录找 stdlib ⇒ 拷到别处要显式 -I。
cp "$EXTC" "$TMP/extc"; chmod +x "$TMP/extc"

if ! EXTC_RECLAIM=1 "$TMP/extc" -I "$ROOT/stdlib" "$SRC" -o "$TMP/on.c" 2>"$TMP/gen.log"; then
    bad "生成失败"; sed 's/^/    /' "$TMP/gen.log"; exit 1
fi
if ! $CC -std=c11 -fwrapv -O1 -o "$TMP/on" "$TMP/on.c" 2>"$TMP/cc.log"; then
    bad "生成物编译失败"; sed 's/^/    /' "$TMP/cc.log"; exit 1
fi

if ! valgrind --tool=callgrind --callgrind-out-file="$TMP/cg.on" "$TMP/on" "$N" \
        >"$TMP/on.out" 2>"$TMP/on.vg"; then
    bad "callgrind 那跑失败"; tail -5 "$TMP/on.vg" | sed 's/^/    /'; exit 1
fi
IR=$(grep '^summary:' "$TMP/cg.on" | awk '{print $2}')
if [ -z "${IR:-}" ]; then bad "callgrind 输出没有 summary 行"; exit 1; fi

awk -v ir="$IR" -v lim="$LIMIT" -v base=1169388408 \
    'BEGIN { printf "  实测   总指令数 %d Ir（%.1f%% of 改动前）\n", ir, 100*ir/base }'

if [ "$IR" -le "$LIMIT" ]; then
    awk -v ir="$IR" -v lim="$LIMIT" -v base=1169388408 \
        'BEGIN { printf "  ok   回收性能成立：%d ≤ %d（相对改动前 −%.1f%%）\n", ir, lim, 100*(base-ir)/base }'
else
    bad "指令数 $IR > $LIMIT（回收开销被付回了：入口守卫或守卫 O(1) 化的收益丢了）"
    exit 1
fi

# ---- 槽号索引：域内穷举（健全性判据，不是性能判据）----------------------------------
# 槽步长按 16 字节取整 ⇒ "由指针找槽号"是乘倒数右移；算错一格就会把合法对象判成"不是本运行期的
# 槽" ⇒ 标记漏走它、清扫当垃圾放掉。判据用**发射出来的**那份运行期文本，不另抄实现。
echo "== 槽号索引（域内穷举：16..1 MiB 的每个 16 的倍数 × 全部合法槽号）=="
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I "$ROOT/src/base" -I "$ROOT/src/back" \
        "$ROOT/tests/reclaim/emit_main.c" "$ROOT/src/back/reclaim.c" "$ROOT/src/base/base.c" \
        -o "$TMP/emit" 2>"$TMP/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$TMP/emit.log"; exit 1
fi
if ! "$TMP/emit" > "$TMP/extc_reclaim_runtime.inc" 2>"$TMP/emit.err" \
        || [ ! -s "$TMP/extc_reclaim_runtime.inc" ]; then
    bad "发射运行期文本失败"; sed 's/^/    /' "$TMP/emit.err"; exit 1
fi
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -I "$TMP" "$DIR/slotindex.c" \
        -o "$TMP/slotindex" 2>"$TMP/slotindex.log"; then
    bad "slotindex 编译失败（-Werror）"; sed 's/^/    /' "$TMP/slotindex.log"; exit 1
fi
if out=$("$TMP/slotindex"); then
    printf '%s\n' "$out" | sed 's/^/  /'
    ok "槽号索引成立：域内穷举全中"
else
    printf '%s\n' "$out" | sed 's/^/    /'
    bad "槽号索引：域内穷举有不符（乘倒数右移不再等于整除 ⇒ 会漏认合法槽）"
    exit 1
fi
exit 0
