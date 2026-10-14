#!/usr/bin/env bash
# tests/reclaim-acyclic/sweeps.sh —— 量一个 `.extc` 程序的三个动态数：
#
#   ① **清扫次数**（`EXTC_RC_TIME=1` 编出的生成物在退出时把每个回收组件的**调用次数**
#      打到 stderr；这里解析 `[rccount] collect calls=N`）；
#   ② **峰值 RSS**（`/usr/bin/time -f %M`，对**生成物二进制**取，不是 `extc` 自己）；
#   ③ **stdout 与退出码**（同一份生成物再跑一趟，stdout 落盘 ⇒ 供逐字比对）。
#
# 它是**量具**，不做断言（判据在 `gate-acyclic.sh`）。输出是一组 `key=value` 行（stdout），
# 便于 `awk`/`eval` 取用；失败时也尽量把原因写成 key=value，并返回非 0。
#
# 用法：
#   tests/reclaim-acyclic/sweeps.sh <prog.extc> [程序的 argv...]
#
# 环境（都可有可无）：
#   EXTC=./build/extc   `CC=cc`   `TIME=/usr/bin/time`
#   CFLAGS_EXTRA=...    gcc 追加参数（词分割；例：`-fsanitize=address`）
#   WORK=<dir>          scratch（默认 `mktemp -d /tmp/extc-sweeps-XXXXXX`）
#   KEEP=1              留住 scratch（失败排查）
#   **其他 EXTC_* 环境变量原样透传给编译器** —— 这就是"档"的选择方式：
#     `EXTC_RC_SWEEPFREE=1`（事件档：计数开 + 不扫）· `EXTC_RECLAIM=0`（无回收基线）
#
# 输出：
#   compile_ok=1|0       两个档的 extc+gcc 都成功才为 1
#   run_ok=1|0           纯跑那一趟的进程正常（退出码记在 rc，非 0 不算失败，程序自己可能返非 0）
#   sweeps=N             collect 的调用次数（**没有 collect 行 ⇒ 0**）
#   sweeps_measurable=1|0  stderr 里到底有没有 `[rccount]` 行 —— 0 表示 EXTC_RC_TIME 没生效，
#                        此时 `sweeps` **不可信**（别把"没量到"读成 0），判据必须先看这一位
#   rccount_lines=N      `[rccount]` 行数（旁证）
#   rss_kb=N             纯跑那一趟的峰值 RSS（KiB）
#   rc=N                 纯跑那一趟的退出码
#   out_hash=HEX         stdout 的 sha256
#   out_file=PATH        stdout 落盘路径（逐字比对用）
#   bin=PATH             纯跑那一趟的生成物二进制
#   src=PATH             输入 .extc 的绝对路径
#   extc_md5=HEX         用的编译器 md5（构建中途换掉了要能看出来）
#   work=DIR             scratch
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
CFLAGS_EXTRA=${CFLAGS_EXTRA:-}

src=${1:-}
if [ -z "$src" ]; then
    echo "用法：tests/reclaim-acyclic/sweeps.sh <prog.extc> [argv...]" >&2
    exit 2
fi
shift
case "$src" in /*) ;; *) src="$ROOT/$src" ;; esac
if [ ! -f "$src" ]; then
    echo "sweeps.sh：没有这个文件：$src" >&2
    exit 2
fi
if [ ! -x "$EXTC" ]; then
    echo "sweeps.sh：编译器不可执行：$EXTC" >&2
    exit 2
fi

work=${WORK:-$(mktemp -d /tmp/extc-sweeps-XXXXXX)}
mkdir -p "$work" || exit 2

# `EXTC_RC_TIME=1` 只对**计数**那一趟设；纯跑那一趟用 `env -u` 明确摘掉，免得调用方
# 已经 export 了它、让"纯跑"偷偷带上一份计数代码（那样 RSS 就不是默认档的了）。
EXTC_FLAGS=(-w --no-line-map -I "$ROOT/stdlib")
CC_FLAGS=(-std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1)
# shellcheck disable=SC2206  # 有意的词分割：CFLAGS_EXTRA 是空格分隔的参数表
CC_FLAGS+=($CFLAGS_EXTRA)

# ---- ① 计数档：编译（EXTC_RC_TIME=1）→ 运行 → 解析 stderr 的 [rccount] 表 ----------------
count_ok=0
if EXTC_RC_TIME=1 "$EXTC" "${EXTC_FLAGS[@]}" "$src" -o "$work/count.c" 2>"$work/count.compile.err" \
   && "$CC" "${CC_FLAGS[@]}" -o "$work/count" "$work/count.c" 2>"$work/count.cc.err"; then
    count_ok=1
fi
count_rc=""
: >"$work/count.err"
if [ "$count_ok" -eq 1 ]; then
    "$work/count" "$@" >"$work/count.out" 2>"$work/count.err"
    count_rc=$?
fi

rccount_lines=$(grep -c '^\[rccount\]' "$work/count.err" 2>/dev/null || true)
rccount_lines=${rccount_lines:-0}
sweeps_measurable=0
sweeps=""
if [ "$rccount_lines" -gt 0 ]; then
    sweeps_measurable=1
    sweeps=$(sed -n 's/^\[rccount\] collect *calls=\([0-9][0-9]*\)$/\1/p' "$work/count.err" | head -1)
    # 计数生效但 collect **一次都没被调用** ⇒ 那行被 `if (extc_rc_tc[i])` 略掉了 ⇒ 0。
    sweeps=${sweeps:-0}
fi

# ---- ② 纯跑档：编译（无 EXTC_RC_TIME）→ /usr/bin/time 取峰值 RSS + stdout ---------------
plain_ok=0
if env -u EXTC_RC_TIME "$EXTC" "${EXTC_FLAGS[@]}" "$src" -o "$work/plain.c" 2>"$work/plain.compile.err" \
   && "$CC" "${CC_FLAGS[@]}" -o "$work/plain" "$work/plain.c" 2>"$work/plain.cc.err"; then
    plain_ok=1
fi
run_ok=0
rss_kb=""
rc=""
if [ "$plain_ok" -eq 1 ]; then
    "$TIME" -f '%M' -o "$work/rss" "$work/plain" "$@" >"$work/plain.out" 2>"$work/plain.err"
    rc=$?
    rss_kb=$(tr -d ' \n' <"$work/rss" 2>/dev/null || true)
    run_ok=1
fi

out_hash=$(sha256sum "$work/plain.out" 2>/dev/null | cut -d' ' -f1 || true)

compile_ok=0
[ "$count_ok" -eq 1 ] && [ "$plain_ok" -eq 1 ] && compile_ok=1

extc_md5=$(md5sum "$EXTC" 2>/dev/null | cut -d' ' -f1 || true)

cat <<EOF
compile_ok=$compile_ok
run_ok=$run_ok
sweeps=$sweeps
sweeps_measurable=$sweeps_measurable
rccount_lines=$rccount_lines
count_rc=$count_rc
rss_kb=$rss_kb
rc=$rc
out_hash=$out_hash
out_file=$work/plain.out
bin=$work/plain
src=$src
extc_md5=$extc_md5
work=$work
EOF

if [ "${KEEP:-0}" = "1" ]; then
    echo "（scratch 保留：$work）" >&2
else
    rm -rf "$work"
fi

[ "$compile_ok" -eq 1 ] || exit 1
exit 0
