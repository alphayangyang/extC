#!/usr/bin/env bash
# 时间库（`std::time`）的常设验收。
#
# 判据分两类：
#   ① **行为**：睡眠真的睡够（下界硬、上界松 —— CI 机器可能慢）、单调钟不倒退、分辨率亚毫秒、
#      忙循环能测出 > 0 的时间（而且那个循环是**折不掉**的：上界来自时钟）。
#   ② **按需发射**：不问时间的程序，产物里一行 `extc_time_*` 都不该有。
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
src() { printf '%s/%s' "$here" "$1"; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-26s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-26s %s\n' "$1" "$2"; }

run() { # 编 + gcc + 跑，把输出放进 $out
  out=""
  if ! "$EXTC" -w --no-line-map -o "$tmp/a.c" "$1" 2>"$tmp/e1"; then bad "$2" "extC: $(head -1 "$tmp/e1")"; return 1; fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/a" "$tmp/a.c" 2>"$tmp/e2"; then bad "$2" "gcc: $(head -1 "$tmp/e2")"; return 1; fi
  out=$("$tmp/a") || { bad "$2" "运行失败"; return 1; }
  return 0
}
get() { printf '%s\n' "$out" | sed -n "s/^$1=//p" | head -1; }

if run "$(src basic.extc)" basic; then
  sleep_ms=$(get sleep_ms); back=$(get backwards); tick=$(get min_tick_ns); busy=$(get busy_ns)
  [ -n "$sleep_ms" ] && [ "$sleep_ms" -ge 45 ] && [ "$sleep_ms" -le 5000 ] \
    && ok sleep-actually-sleeps "${sleep_ms} ms（要求 ≥45 且 ≤5000）" \
    || bad sleep-actually-sleeps "sleep_ms=[$sleep_ms]"
  [ "$back" = "0" ] && ok monotonic-never-backwards "10 万次采样 0 次倒退" \
    || bad monotonic-never-backwards "backwards=[$back]"
  [ -n "$tick" ] && [ "$tick" -gt 0 ] && [ "$tick" -lt 1000000 ] \
    && ok sub-millisecond-resolution "最小正差值 ${tick} ns（<1ms）" \
    || bad sub-millisecond-resolution "min_tick_ns=[$tick]"
  [ -n "$busy" ] && [ "$busy" -gt 0 ] \
    && ok times-a-real-busy-loop "折不掉的循环：${busy} ns" \
    || bad times-a-real-busy-loop "busy_ns=[$busy]"
fi

# ② 按需发射：不用时间的程序产物里不该有时间运行期
if "$EXTC" -w --no-line-map -o "$tmp/c.c" "$(src control_no_time.extc)" 2>"$tmp/e3"; then
  if grep -q "extc_time_" "$tmp/c.c"; then bad on-demand-runtime "产物里出现了 extc_time_*"
  else ok on-demand-runtime "不用时间的程序产物里 0 处 extc_time_*"; fi
else bad on-demand-runtime "extC: $(head -1 "$tmp/e3")"; fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
