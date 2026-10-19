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
if ! out=$(timeout 120 "$work/driver"); then
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
    if timeout 300 "$work/driver-asan" >"$work/asan.out" 2>"$work/asan.err"; then
        ok "ASan/UBSan 运行干净（$(tail -1 "$work/asan.out")）"
    else
        bad "ASan/UBSan 报错"
        sed 's/^/    /' "$work/asan.err" | head -20
    fi
else
    echo "  skip ASan/UBSan（本机工具链不支持，不算失败）"
fi

# 着色缓存自检（EXTC_RC_SHADE_GUARD=1）：命中时重做一遍"确实还黑"的判定。它验证的是
# 「每一处清 mark 位的路径都自增了代际号」这条失效契约 —— 漏一条，缓存就会把白对象当黑、
# 跳过屏障，可达对象最终被清扫扫掉。bad 必须为 0 且命中次数必须 > 0（不然判据是空转）。
echo "== 着色缓存自检（EXTC_RC_SHADE_GUARD=1：命中必须仍是黑的）=="
if "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -DEXTC_RC_SHADE_GUARD=1 -I "$work" \
        tests/reclaim/driver.c -o "$work/driver-sg" 2>"$work/sg-build.log"; then
    if timeout 300 "$work/driver-sg" >"$work/sg.out" 2>"$work/sg.err"; then
        sg_line=$(grep '^\[sg\] hits=' "$work/sg.err" | tail -1)
        sg_hits=$(printf '%s\n' "$sg_line" | sed -n 's/^\[sg\] hits=\([0-9][0-9]*\) bad=\([0-9][0-9]*\)$/\1/p')
        sg_bad=$(printf '%s\n' "$sg_line" | sed -n 's/^\[sg\] hits=\([0-9][0-9]*\) bad=\([0-9][0-9]*\)$/\2/p')
        if [ -n "$sg_hits" ] && [ "${sg_bad:-1}" = 0 ] && [ "$sg_hits" -gt 0 ]; then
            ok "着色缓存自检：命中 $sg_hits 次，一次也没有把白对象当黑（bad=0）"
        else
            bad "着色缓存自检：bad=${sg_bad:-缺失} hits=${sg_hits:-缺失}（有清 mark 的路径忘了代际++）"
            printf '%s\n' "$sg_line" | sed 's/^/    /'
        fi
    else
        bad "着色缓存自检：guard 版断言失败"
        tail -5 "$work/sg.out" | sed 's/^/    /'
    fi
else
    bad "着色缓存自检：guard 版编译失败"
    sed 's/^/    /' "$work/sg-build.log" | head -10
fi

# 体积出口（-DEXTC_RC_ALLOC_INLINE=0）：分配体的 \`#else\` 分支（不内联的那份签名）也得能编、
# 能过同一套断言 —— 两份签名只差内联属性，任何一边腐坏都在这里现形。
echo "== 体积出口（-DEXTC_RC_ALLOC_INLINE=0：不内联的分配体也要过同一套断言）=="
if "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -DEXTC_RC_ALLOC_INLINE=0 -I "$work" \
        tests/reclaim/driver.c -o "$work/driver-noinl" 2>"$work/noinl-build.log"; then
    if out=$(timeout 120 "$work/driver-noinl"); then
        ok "不内联档编译并跑过同一套断言（$(printf '%s\n' "$out" | tail -1)）"
    else
        bad "不内联档断言失败"
        printf '%s\n' "$out" | sed 's/^/    /' | tail -10
    fi
else
    bad "不内联档编译失败"
    sed 's/^/    /' "$work/noinl-build.log" | head -10
fi

# 分段扫描出口（EXTC_DRIVER_ONLY_SCAN=1）：默认 chunk 下大对象一步扫不完要留下续扫游标；把 chunk
# 开到 1（每个多指针对象都真的分步续扫）仍然必须全绿 —— 续扫的每次推进都要接得上；反过来把 chunk
# 开到十亿（等于关掉分段）必须**红在分段断言上**，证明这组判据真的在量这个机制、不是空转。
echo "== 分段扫描出口（chunk=1 全绿；chunk=1e9 红在分段断言上）=="
scan_build() {   # $1 = chunk 值, $2 = 输出可执行
    "$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-unused-function -O1 -g \
        -DEXTC_RC_SCAN_CHUNK="$1" -DEXTC_DRIVER_ONLY_SCAN=1 -I "$work" \
        tests/reclaim/driver.c -o "$2" 2>"$2.log"
}
if scan_build 1 "$work/driver-chunk1"; then
    if out=$(timeout 120 "$work/driver-chunk1"); then
        ok "分段扫描：chunk=1 时每个多指针对象都分步续扫，探针仍全绿（$(printf '%s\n' "$out" | tail -1)）"
    else
        bad "分段扫描：chunk=1 的续扫断言失败"
        printf '%s\n' "$out" | sed 's/^/    /' | tail -10
    fi
else
    bad "分段扫描：chunk=1 档编译失败"
    sed 's/^/    /' "$work/driver-chunk1.log" | head -10
fi
if scan_build 1000000000 "$work/driver-chunkbig"; then
    if out=$(timeout 120 "$work/driver-chunkbig"); then
        bad "分段扫描：chunk=1e9（关掉分段）竟然全绿 —— 分段断言没有牙"
    elif printf '%s\n' "$out" | grep -q 'FAIL 分段：一步扫不完'; then
        ok "分段扫描红自测：chunk=1e9（关掉分段）红在分段断言上（判据有牙）"
    else
        bad "分段扫描：chunk=1e9 红了，但不是红在分段断言上"
        printf '%s\n' "$out" | sed 's/^/    /' | grep FAIL | head -5
    fi
else
    bad "分段扫描：chunk=1e9 档编译失败"
    sed 's/^/    /' "$work/driver-chunkbig.log" | head -10
fi
exit "$fail"
