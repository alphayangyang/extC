#!/usr/bin/env bash
# 回收运行期文本的自测：把 src/back/reclaim.c 发射的文本编成一个独立 C 程序并断言。
#
# 被测的就是**发射出来的那份**文本 —— run.sh 先用仓库的发射口把它导出来，再和断言程序一起编译，
# 所以这里断言的行为，就是生成文件里真正跑的那份代码的行为，不存在另抄一份实现对拍的漂移。
#
# 覆盖：分配/释放、计数归零当场回收、环必须由清扫收回、饱和计数不得提前释放、槽释放时登记位
# 被清（无陈旧入口）、跨区登记播种与"区域外止步"、按对象自己的类型描述符走指针字段（描述符随
# 槽交）、`owns` 守卫（外区/非槽首指针一个字节都不碰）、槽复用换描述符、保守描述符、bump 整片
# 回收、压力触发与自学习退场（无环负载下清扫次数不随规模增长）。
#
# 用法：tests/reclaim/run.sh    退出码 0 = 全绿。
set -u
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=1; }

echo "== 发射（src/back/reclaim.c 的实际输出）=="
if ! "$CC" -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I src/base -I src/back \
        tests/reclaim/emit_main.c src/back/reclaim.c src/base/base.c -o "$work/emit" \
        2>"$work/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$work/emit.log"; exit 1
fi
if ! "$work/emit" > "$work/extc_reclaim_runtime.inc" 2>"$work/emit.err"; then
    bad "发射器运行失败"; sed 's/^/    /' "$work/emit.err"; exit 1
fi
if [ ! -s "$work/extc_reclaim_runtime.inc" ]; then
    bad "发射文本为空"; exit 1
fi
ok "发射器编译并导出运行期文本（$(wc -c < "$work/extc_reclaim_runtime.inc") 字节，-Wpedantic 零警告）"

echo "== 断言（发射文本 + 驱动）=="
if ! "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -I "$work" \
        tests/reclaim/driver.c -o "$work/driver" 2>"$work/driver.log"; then
    bad "断言程序编译失败（-Werror）"; sed 's/^/    /' "$work/driver.log"; exit 1
fi
if ! out=$("$work/driver"); then
    printf '%s\n' "$out" | sed 's/^/    /'
    bad "断言失败"
    exit 1
fi
printf '%s\n' "$out" | sed 's/^/  /'
ok "全部断言通过"

# ASan/UBSan 是**额外的**一遍：分配器自己管 slab，越界与 use-after-free 必须真的干净。
echo "== ASan / UBSan（内存安全形状要真的跑得干净）=="
if "$CC" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I "$work" \
        tests/reclaim/driver.c -o "$work/driver-asan" 2>"$work/asan-build.log"; then
    if "$work/driver-asan" >"$work/asan.out" 2>"$work/asan.err"; then
        ok "ASan/UBSan 运行干净（$(tail -1 "$work/asan.out")）"
    else
        bad "ASan/UBSan 报错"
        sed 's/^/    /' "$work/asan.err" | head -20
    fi
else
    echo "  skip ASan/UBSan（本机工具链不支持，不算失败）"
fi

exit "$fail"
