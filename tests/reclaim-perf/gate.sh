#!/usr/bin/env bash
# tests/reclaim-perf/gate.sh —— 「分配点入口守卫」的性能判据（先红后绿的那一条）。
#
# 判据：开关开的 `cyclic_pressure` 在固定 N=200000 下，callgrind 的**总指令数 ≤ LIMIT**。
#   LIMIT = 改动前实测值 × 0.6 —— 对应「把根表物化拿掉 ≥40%」。
#   改动前实测（同一份 `build/extc`，md5 5e51d77f…）：1 169 388 408 Ir
#   ⇒ LIMIT = 701 633 044（= 改动前 × 0.6）。
#   为什么是 0.6：归因（同目录 README §3.2）说根表物化占 **56.1%**；入口守卫把它拿掉
#   ⇒ 期望落到 ~0.44×。0.6 这条线留了余量，同时**改前必红**（1.0× > 0.6×）。
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
LIMIT=${LIMIT:-701633044}          # 改动前实测 1169388408 × 0.6

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== 回收入口守卫：性能判据（EXTC_RECLAIM=1，N=$N）=="
echo "  形状   $SRC"
echo "  编译   EXTC_RECLAIM=1 extc -I <repo>/stdlib → cc -std=c11 -fwrapv -O1"
echo "  判据   总指令数 ≤ $LIMIT（= 改动前 1169388408 × 0.6）"

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
        'BEGIN { printf "  ok   入口守卫成立：%d ≤ %d（相对改动前 −%.1f%%）\n", ir, lim, 100*(base-ir)/base }'
    exit 0
fi
bad "指令数 $IR > $LIMIT（根表物化没被拿掉）"
exit 1
