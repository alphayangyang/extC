#!/usr/bin/env bash
# tests/reclaim-shapes/run.sh —— 「区域回收」的**行为基线**（只钉判据，不做门禁）
#
# 语义：**只记录现状**。红的条目在表里标出来，脚本照旧 `exit 0`
# （与 tests/arena-soundness/ 同一口径：这是证据，不是回归判据）。
#
# 覆盖（判据来源：docs/RECLAMATION.md「要解决的问题」+ prototype-reclaim/README.md「压力版」）：
#   ① cond-save-in-loop        条件保存在循环里          → 编译 + 语义（绿）
#   ② reject-deep-store        跨帧存储（直接）          → 必须报错（绿）
#   ② reject-deep-store-call   跨帧存储（经被调者）      → 必须报错（绿）
#   ③ cyclic-shape             双向链表 + 父指针树 + 缓存 + 索引 → 编译 + 运行（绿）
#   ④ promote-place            提权发生的位置            → 生成物结构断言（绿）
#   ⑤ scale-growth(N→4N)       同形状轮数 ×4              → 峰值 RSS 是否随轮数涨（预期红）
#   ⑥ cyclic-pressure(N→4N)    有环 + 大压力、轮数 ×4      → 峰值 RSS 是否随轮数涨（预期红）
#   ⑪ struct-copy(N→4N)        整值存储携带引用叶子        → 峰值 RSS 是否随轮数涨（预期红；开关开的那条门禁在 run-switch.sh ①f）
#                              （垃圾是环 ⇒ 计数永不归零；开关开的那条门禁在 run-switch.sh）
#
# ⚠️ 本脚本是**基线**：不设开关（`EXTC_RECLAIM` 默认**开** ⇒ 本脚本跑在开档），红条目照旧 exit 0。
# 开关（`EXTC_RECLAIM=1`）下**必须成立**的那组判据在 run-switch.sh（门禁，关掉开关必须红）。
# 峰值内存一律取 `/usr/bin/time -v` 的 Maximum resident set size。
# 时间：每个形状 ≈ 一次 extc + 一次 gcc + 一次运行，秒级；N=800000 那条也在 1s 量级。
set -u
cd "$(dirname "$0")/../.."          # 仓库根：与 tests/arena*/run.sh 同一约定

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 颜色只在真终端上加；绿/红两个字**始终**在文本里（管道里也能 grep）
if [ -t 1 ]; then G=$'\033[32m'; R=$'\033[31m'; O=$'\033[0m'; else G=; R=; O=; fi

table=$TMP/table.tsv
: >"$table"
row() { # 名称 编译 运行 峰值 颜色 绿/红 备注
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7" >>"$table"
}
# 表格按**显示宽度**对齐（列里有中文 ⇒ bash 的 %-Ns 按字节数数，会歪）
print_table() {
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$table" <<'PY'
import re, sys, unicodedata
ansi = re.compile(r'\x1b\[[0-9;]*m')
def w(s):
    s = ansi.sub('', s)
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in s)
head = ['形状', '编译', '运行', '峰值内存', '备注']
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8')]
cells = [head] + [[r[0], r[1], r[2], r[3], f'{r[4]}{r[5]}' + ('\x1b[0m' if r[4] else '') + f'  {r[6]}'] for r in rows]
width = [max(w(c[i]) for c in cells) for i in range(4)]
def line(c):
    return '  '.join(c[i] + ' ' * (width[i] - w(c[i])) for i in range(4)) + '  ' + c[4]
print(line(head))
print('  '.join('-' * width[i] for i in range(4)) + '  ' + '-' * 40)
for c in cells[1:]:
    print(line(c))
PY
    else
        # 退路：中文只出现在最后一列 ⇒ 前四列按字节补齐也齐
        printf '%-26s  %-6s  %-6s  %-18s  %s\n' "形状" "编译" "运行" "峰值内存" "备注"
        while IFS=$'\t' read -r a b c d e f g; do
            printf '%-26s  %-6s  %-6s  %-18s  %s%s%s  %s\n' "$a" "$b" "$c" "$d" "$e" "$f" "$O" "$g"
        done <"$table"
    fi
}
rss_mb() { awk -v k="$1" 'BEGIN { if (k == "") printf "—"; else printf "%.2f MB", k/1024 }'; }

# 正例：extc 生成 C → gcc → /usr/bin/time -v 运行。结果进 BR_* 全局。
# 用法：build_run <用例名> [程序参数...]
build_run() {
    local n=$1; shift
    BR_C=fail; BR_R="-"; BR_RSS=""; BR_OUT=""
    if "$EXTC" "tests/reclaim-shapes/$n.extc" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1 \
       && "$CC" -fwrapv -O1 -o "$TMP/$n" "$TMP/$n.c" >>"$TMP/$n.cerr" 2>&1; then
        BR_C=ok
        "$TIME" -v -o "$TMP/$n.time" "$TMP/$n" "$@" >"$TMP/$n.out" 2>"$TMP/$n.err"
        local rc=$?
        BR_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/$n.time")
        if [ "$rc" -eq 0 ]; then BR_R=ok; else BR_R="rc=$rc"; fi
        BR_OUT=$(tr '\n' '|' <"$TMP/$n.out")
    fi
}

# 反例：只要 extc 报错就算过（信号 = 崩了 ⇒ 不算过）。回显第一条 error。
reject() { # <用例名> <期望的错误子串>
    local n=$1 want=$2
    "$EXTC" "tests/reclaim-shapes/$n.extc" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1
    local rc=$?
    if [ "$rc" -ge 128 ]; then
        row "$n" "CRASH" "—" "—" "$R" "红" "编译器信号 $((rc-128))（要报错，不要崩）"
    elif [ "$rc" -ne 0 ] && grep -qF "$want" "$TMP/$n.cerr"; then
        row "$n" "rej" "—" "—" "$G" "绿" "编译期挡住：$(grep -m1 ': error:' "$TMP/$n.cerr" | sed 's/^.*: error: //' | cut -c1-72)"
    elif [ "$rc" -ne 0 ]; then
        row "$n" "rej" "—" "—" "$R" "红" "报错了，但不是期望的那条：$(grep -m1 ': error:' "$TMP/$n.cerr" | cut -c1-72)"
    else
        row "$n" "ok" "—" "—" "$R" "红" "**没有报错**（该拒的形状被放行了）"
    fi
}

echo "== tests/reclaim-shapes：区域回收行为基线（exit 0 = 记录，不是门禁）=="
echo

# ── ① 条件保存在循环里 ────────────────────────────────────────────────────────
build_run cond_save
if [ "$BR_C" = ok ] && [ "$BR_R" = ok ] && [ "$BR_OUT" = "saved=7|" ]; then
    row "cond-save-in-loop" "ok" "ok" "$(rss_mb "$BR_RSS")" "$G" "绿" "语义对（keep 最后一轮 ⇒ saved=7）"
else
    row "cond-save-in-loop" "$BR_C" "$BR_R" "$(rss_mb "$BR_RSS")" "$R" "红" "编译/运行/语义没同时成立（out=$BR_OUT）"
fi

# ── ② 跨帧存储被拒 ────────────────────────────────────────────────────────────
reject reject_deep_store      "dies first (borrowed from depth 2, but this can only hold up to depth 1)"
reject reject_deep_store_call "points into a deeper scope (depth 2) than the arena this call may store it in (depth 1)"

# ── ③ 有环形状 ────────────────────────────────────────────────────────────────
build_run cyclic_shape
if [ "$BR_C" = ok ] && [ "$BR_R" = ok ] && [ "$BR_OUT" = "fwd=28 back=28 tree=true cache=true|" ]; then
    row "cyclic-shape" "ok" "ok" "$(rss_mb "$BR_RSS")" "$G" "绿" "有环：next/prev 与 child/parent 互指；fwd=back、tree、cache 全对"
else
    row "cyclic-shape" "$BR_C" "$BR_R" "$(rss_mb "$BR_RSS")" "$R" "红" "编译/运行/语义没同时成立（out=$BR_OUT）"
fi

# ── ④ 提权发生的位置（生成物结构）─────────────────────────────────────────────
build_run promote_place
cfile=$TMP/promote_place.c
doom=$(grep 'doomed' "$cfile" | grep -o '__extc_a\[[0-9]*\]' | head -1 | tr -dc '0-9')
keep=$(grep 'kept'   "$cfile" | grep -o '__extc_a\[[0-9]*\]' | head -1 | tr -dc '0-9')
# 循环体里的 release（生成物里缩进 8 空格的那一层）
loop_doom=$(grep -c "^        extc_arena_release(&__extc_a\[$doom\]);" "$cfile" || true)
loop_keep=$(grep -c "^        extc_arena_release(&__extc_a\[$keep\]);" "$cfile" || true)
if [ "$BR_C" = ok ] && [ "$BR_R" = ok ] && [ "$BR_OUT" = "saved=3|" ] \
   && [ -n "$doom" ] && [ -n "$keep" ] && [ "$doom" != "$keep" ] \
   && [ "$loop_doom" -ge 1 ] && [ "$loop_keep" -eq 0 ]; then
    row "promote-place" "ok" "ok" "$(rss_mb "$BR_RSS")" "$G" "绿" "生成物：kept→__extc_a[$keep]（循环外），doomed→__extc_a[$doom]（循环体，每轮 release）；⚠️ 只见归属，运行时无搬移"
elif [ "$BR_C" = ok ]; then
    row "promote-place" "ok" "$BR_R" "$(rss_mb "$BR_RSS")" "$R" "红" "生成物里看不出逃逸的那份归到外层区（kept=a[$keep], doomed=a[$doom], loop_release kept=$loop_keep doom=$loop_doom）"
else
    row "promote-place" "$BR_C" "$BR_R" "$(rss_mb "$BR_RSS")" "$R" "红" "编译/生成物编不过"
fi

# ── ⑤ 规模行为：同一形状轮数 ×4 ───────────────────────────────────────────────
build_run scale_growth 200000
n_c=$BR_C; n_r=$BR_R; n_rss=$BR_RSS; n_out=$BR_OUT
build_run scale_growth 800000
f_c=$BR_C; f_r=$BR_R; f_rss=$BR_RSS; f_out=$BR_OUT
sem_ok=no
if [ "$n_c" = ok ] && [ "$n_r" = ok ] && [ "$f_c" = ok ] && [ "$f_r" = ok ] \
   && [ "$n_out" = "rounds=200000 saved=199999|" ] && [ "$f_out" = "rounds=800000 saved=799999|" ]; then
    sem_ok=yes
fi
if [ "$sem_ok" = yes ] && [ -n "$n_rss" ] && [ -n "$f_rss" ]; then
    grew=$(awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { print (b > a * 1.5) ? "yes" : "no" }')
    ratio=$(awk -v a="$n_rss" -v b="$f_rss" 'BEGIN { printf "%.2f", b/a }')
    if [ "$grew" = yes ]; then
        row "scale-growth N→4N" "ok" "ok" "$(rss_mb "$n_rss") → $(rss_mb "$f_rss")" "$R" "红" "峰值随轮数增长（${ratio}× @ ×4 轮）；相同形状，回收/提权搬移未实现 ⇒ 界与轮数有关"
    else
        row "scale-growth N→4N" "ok" "ok" "$(rss_mb "$n_rss") → $(rss_mb "$f_rss")" "$G" "绿" "峰值不随轮数增长（${ratio}× @ ×4 轮）—— 内存有界"
    fi
else
    row "scale-growth N→4N" "$n_c/$f_c" "$n_r/$f_r" "$(rss_mb "$n_rss") → $(rss_mb "$f_rss")" "$R" "红" "两次运行没同时跑对（N: $n_out / 4N: $f_out）"
fi

# ── ⑥ 有环 + 大压力：同一个有环形状轮数 ×4（开关关 ⇒ 环垃圾攒着，红）──────────────
# 形状与 ⑤ 的区别是**垃圾是环**：桶满时整条双向链退役，链内 next/prev 互指 ⇒ 持有者计数
# 永不归零 ⇒ 计数归零（开关的第三层）一个也放不掉；开关**关**时区域也不回收（函数返回才归还），
# 所以峰值随轮数线性涨。开关打开后的界由 run-switch.sh 的 ①b 断言（≈1.0×）。
build_run cyclic_pressure 200000
y_c=$BR_C; y_r=$BR_R; y_rss=$BR_RSS; y_out=$BR_OUT
build_run cyclic_pressure 800000
z_c=$BR_C; z_r=$BR_R; z_rss=$BR_RSS; z_out=$BR_OUT
cyc_n_want="rounds=200000 retired=22208 live=128 fwd=25591744 back=25591744 idx=20000 cache=1024 hits=155520 depth=212 tree=true|"
cyc_f_want="rounds=800000 retired=88832 live=512 fwd=409468672 back=409468672 idx=20000 cache=1024 hits=622272 depth=80 tree=true|"
if [ "$y_c" = ok ] && [ "$y_r" = ok ] && [ "$z_c" = ok ] && [ "$z_r" = ok ] \
   && [ "$y_out" = "$cyc_n_want" ] && [ "$z_out" = "$cyc_f_want" ] \
   && [ -n "$y_rss" ] && [ -n "$z_rss" ]; then
    y_ratio=$(awk -v a="$y_rss" -v b="$z_rss" 'BEGIN { printf "%.2f", b/a }')
    if awk -v a="$y_rss" -v b="$z_rss" 'BEGIN { exit !(b > a * 1.5) }'; then
        row "cyclic-shape-pressure N→4N" "ok" "ok" "$(rss_mb "$y_rss") → $(rss_mb "$z_rss")" "$R" "红" "有环（桶满整链退役 ⇒ 计数永不归零）：开关关 ⇒ 峰值随轮数增长（${y_ratio}×）；开关开由 run-switch.sh 断言 < 1.5×"
    else
        row "cyclic-shape-pressure N→4N" "ok" "ok" "$(rss_mb "$y_rss") → $(rss_mb "$z_rss")" "$G" "绿" "同形状峰值不随轮数增长（${y_ratio}× @ ×4 轮）"
    fi
else
    row "cyclic-shape-pressure N→4N" "$y_c/$z_c" "$y_r/$z_r" "$(rss_mb "$y_rss") → $(rss_mb "$z_rss")" "$R" "红" "两次运行没同时跑对（N: $y_out / 4N: $z_out）"
fi

# ── ⑩ 分片旋钮的有利形状：8 条独立链 = 8 个分配点（开关关 ⇒ 环垃圾攒着，红）────────
# 每条链满 9 个结点就整条退役 ⇒ 环垃圾。8 个分配点各占一片 ⇒ `EXTC_RC_SHARDS=8` 切得开：
# 停顿/内存/吞吐的三组数字与「切不开」的反面对照见 run-shard.sh；这里只做与 ⑥ 同档的
# 基线观察（开关关 ⇒ 没有回收层 ⇒ 峰值随轮数涨）。
build_run shard_bands 200000
sbn_c=$BR_C; sbn_r=$BR_R; sbn_rss=$BR_RSS; sbn_out=$BR_OUT
build_run shard_bands 800000
sbf_c=$BR_C; sbf_r=$BR_R; sbf_rss=$BR_RSS; sbf_out=$BR_OUT
if [ "$sbn_c" = ok ] && [ "$sbn_r" = ok ] && [ "$sbf_c" = ok ] && [ "$sbf_r" = ok ] \
   && [ -n "$sbn_rss" ] && [ -n "$sbf_rss" ]; then
    sb_ratio=$(awk -v a="$sbn_rss" -v b="$sbf_rss" 'BEGIN { printf "%.2f", b/a }')
    if awk -v a="$sbn_rss" -v b="$sbf_rss" 'BEGIN { exit !(b > a * 1.5) }'; then
        row "shard-bands N→4N" "ok" "ok" "$(rss_mb "$sbn_rss") → $(rss_mb "$sbf_rss")" "$R" "红" "8 条独立链：开关关 ⇒ 峰值随轮数增长（${sb_ratio}×）；开关开 + 分片=8 见 run-shard.sh"
    else
        row "shard-bands N→4N" "ok" "ok" "$(rss_mb "$sbn_rss") → $(rss_mb "$sbf_rss")" "$G" "绿" "峰值不随轮数增长（${sb_ratio}× @ ×4 轮）"
    fi
else
    row "shard-bands N→4N" "$sbn_c/$sbf_c" "$sbn_r/$sbf_r" "$(rss_mb "$sbn_rss") → $(rss_mb "$sbf_rss")" "$R" "红" "两次运行没同时跑对（N: $sbn_out / 4N: $sbf_out）"
fi

# ── ⑪ 整值存储携带引用叶子：`s.w = wrap { n: keeper }`（开关关 ⇒ 跨区入边不登记，红）──────
# 目标 `s.w` 是结构体（不是 `ref T`）⇒ 旧 codegen 发裸复合字面量拷贝：槽写了却没 `inc`、
# 没登记跨区边。开关**关**时没有清扫/计数层，程序跑得对（存储不回收），峰值随轮数涨 ⇒
# 基线里这条红；开关打开后由一个 `inc` + 一条登记兜住，判据在 run-switch.sh ①f（≈1.0×）。
build_run struct_copy 200000
scn_c=$BR_C; scn_r=$BR_R; scn_rss=$BR_RSS; scn_out=$BR_OUT
build_run struct_copy 800000
scf_c=$BR_C; scf_r=$BR_R; scf_rss=$BR_RSS; scf_out=$BR_OUT
if [ "$scn_c" = ok ] && [ "$scn_r" = ok ] && [ "$scf_c" = ok ] && [ "$scf_r" = ok ] \
   && [ "$scn_out" = "rounds=200000 sink=19999900000 want=19999900000 ok=true|" ] \
   && [ "$scf_out" = "rounds=800000 sink=319999600000 want=319999600000 ok=true|" ] \
   && [ -n "$scn_rss" ] && [ -n "$scf_rss" ]; then
    sc_ratio=$(awk -v a="$scn_rss" -v b="$scf_rss" 'BEGIN { printf "%.2f", b/a }')
    if awk -v a="$scn_rss" -v b="$scf_rss" 'BEGIN { exit !(b > a * 1.5) }'; then
        row "struct-copy N→4N" "ok" "ok" "$(rss_mb "$scn_rss") → $(rss_mb "$scf_rss")" "$R" "红" "整值存储的跨区入边：开关关 ⇒ 峰值随轮数增长（${sc_ratio}×）；开关开由 run-switch.sh ①f 断言 < 1.5×"
    else
        row "struct-copy N→4N" "ok" "ok" "$(rss_mb "$scn_rss") → $(rss_mb "$scf_rss")" "$G" "绿" "峰值不随轮数增长（${sc_ratio}× @ ×4 轮）"
    fi
else
    row "struct-copy N→4N" "$scn_c/$scf_c" "$scn_r/$scf_r" "$(rss_mb "$scn_rss") → $(rss_mb "$scf_rss")" "$R" "红" "两次运行没同时跑对（N: $scn_out / 4N: $scf_out）"
fi

# ── 表 ────────────────────────────────────────────────────────────────────────
print_table
echo
echo "绿 = 该形状的判据现在成立；红 = 判据未成立（基线要盯住的）。"
echo "本目录是**基线**：exit 0，不接进 check.sh 的门禁计数 —— 见 README.md。"
echo "开关（EXTC_RECLAIM=1）下的门禁断言见 run-switch.sh（关掉开关必须红）。"
exit 0
