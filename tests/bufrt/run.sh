#!/usr/bin/env bash
# tests/bufrt/run.sh —— buf 基质发射文本的自测（增长预留 + 密排小载荷，共用一本账）。
#
# 被测的就是 `src/back/bufrt.c` 发射出来的那份文本：run.sh 先用仓库的发射口把它导出，
# 再和断言程序一起编译，所以这里断言的行为，就是生成文件里真正跑的那份代码的行为。
# 两种发射档都跑：`EXTC_BUF_MT=0`（单线程，锁编译掉）与 `=1`（能起线程，锁保留）。
#
# 用法：tests/bufrt/run.sh    退出码 0 = 全绿。
set -u
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=1; }

echo "== 发射（src/back/bufrt.c 的实际输出）=="
if ! "$CC" -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I src/base -I src/back \
        tests/bufrt/emit_main.c src/back/bufrt.c src/base/base.c -o "$work/emit" \
        2>"$work/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$work/emit.log"; exit 1
fi
ok "发射器编译（-Wpedantic 零警告）"

for mt in 0 1; do
    echo "== 断言（发射文本 + 驱动，EXTC_BUF_MT=$mt）=="
    if ! EXTC_BUF_MT_TEST=$mt "$work/emit" > "$work/rt.inc" 2>"$work/emit$mt.err"; then
        bad "发射失败（mt=$mt）"; sed 's/^/    /' "$work/emit$mt.err"; exit 1
    fi
    if [ ! -s "$work/rt.inc" ]; then bad "发射文本为空（mt=$mt）"; exit 1; fi
    if ! grep -q "#define EXTC_BUF_MT $mt" "$work/rt.inc"; then bad "发射文本没有写出 EXTC_BUF_MT=$mt"; fi
    cp "$work/rt.inc" "$work/extc_buf_runtime.inc"
    if ! "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -I "$work" \
            tests/bufrt/driver.c -o "$work/driver$mt" 2>"$work/drv$mt.log"; then
        bad "驱动编译失败（mt=$mt）"; sed 's/^/    /' "$work/drv$mt.log"; exit 1
    fi
    if ! "$work/driver$mt" > "$work/out$mt.log" 2>&1; then
        bad "断言失败（mt=$mt）"; sed 's/^/    /' "$work/out$mt.log"; exit 1
    fi
    sed 's/^/  /' "$work/out$mt.log" | tail -n +2
done

if [ "$fail" = 0 ]; then
    n=$(grep -c '^  ok' "$work/out0.log" 2>/dev/null || echo 0)
    echo "  通过 $(grep -c '  ok' "$work/out0.log") 条判据 × 2 个发射档"
fi
exit "$fail"
