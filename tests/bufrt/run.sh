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

for mt in 0 1; do for rc in 0 1; do
    echo "== 断言（发射文本 + 驱动，EXTC_BUF_MT=$mt EXTC_BUF_RC=$rc）=="
    if ! EXTC_BUF_MT_TEST=$mt EXTC_BUF_RC_TEST=$rc "$work/emit" > "$work/rt.inc" 2>"$work/emit$mt$rc.err"; then
        bad "发射失败（mt=$mt rc=$rc）"; sed 's/^/    /' "$work/emit$mt$rc.err"; exit 1
    fi
    if [ ! -s "$work/rt.inc" ]; then bad "发射文本为空（mt=$mt rc=$rc）"; exit 1; fi
    if ! grep -q "#define EXTC_BUF_MT $mt" "$work/rt.inc"; then bad "发射文本没有写出 EXTC_BUF_MT=$mt"; fi
    if [ "$rc" = 1 ]; then
        if ! grep -q "extc_rc_pinDropRange" "$work/rt.inc"; then bad "回收档在场时 releaseRefs 没有调 pin 表（mt=$mt）"; fi
    else
        if grep -q "extc_rc_" "$work/rt.inc"; then bad "回收档缺席时发射文本里仍有 extc_rc_ 名字（mt=$mt）"; fi
    fi
    cp "$work/rt.inc" "$work/extc_buf_runtime.inc"
    if ! "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -I "$work" \
            tests/bufrt/driver.c -o "$work/driver" 2>"$work/drv$mt$rc.log"; then
        bad "驱动编译失败（mt=$mt rc=$rc）"; sed 's/^/    /' "$work/drv$mt$rc.log"; exit 1
    fi
    if ! "$work/driver" > "$work/out$mt$rc.log" 2>&1; then
        bad "断言失败（mt=$mt rc=$rc）"; sed 's/^/    /' "$work/out$mt$rc.log"; exit 1
    fi
    sed 's/^/  /' "$work/out$mt$rc.log" | tail -n +2
done; done

if [ "$fail" = 0 ]; then
    echo "  通过 $(grep -c '  ok' "$work/out00.log") 条判据 × 2 个线程档 × 2 个回收档"
fi
exit "$fail"
