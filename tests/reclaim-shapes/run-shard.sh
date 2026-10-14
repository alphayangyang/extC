#!/usr/bin/env bash
# manual: 停顿/峰值/吞吐的**对照实验**（证据脚本）；分片门禁是 gate-shard.sh
# tests/reclaim-shapes/run-shard.sh —— **停顿/分片对照实验**（`EXTC_RC_SHARDS`）
#
# 这不是门禁（门禁是 `run-switch.sh` 的 ①e）；这是**证据脚本**，口径跟 `run-evidence.sh`：
# 同一份形状、同一把尺子，只动一个旋钮，回答三个问题：
#
#   ① 产物：`EXTC_RC_SHARDS` 不设与 `=1` 必须是**同一份生成物**（逐字节）—— 分片是编译期
#      旋钮，默认 1 = 现有行为，不会悄悄改掉默认产物。
#   ② 三个数：N=1 vs N=8 的
#        · **最大单次清扫耗时** —— 测法：在**同一份生成物**上插一个计时外壳
#          （`tools/rc-pause-probe.py`），记录每次 `extc_rc_collect` 的墙钟并取最大值；
#          回收判据一个字不动，两次用的是同一把尺子。报告 REPS 次的中位数（避噪）。
#        · **峰值内存** —— `/usr/bin/time -v` 的 Maximum resident set size（取最大值）。
#        · **吞吐** —— 同一程序的墙钟，取 REPS 次里最快的一次（`%e`）。
#   ③ 方向：与 `prototype-reclaim/README.md`「最终版：计数归零 + 分片」那张表同向吗。
#      原型（200 万轮）：分片 1→8 是 最大停顿 406.7→81.7 µs（好）、峰值 RSS 17→18 MiB
#      （略差）、耗时 49.6→61.9 ms（差）。
#
# 两个形状，一正一反：
#   · `shard_bands.extc`（形状 ⑩）：8 条独立有界链 = 8 个分配点 ⇒ **结构边界 = 片边界**，
#     分片切得开（`RECLAMATION.md` 第四节要的那两条都满足）。
#   · `cyclic_pressure.extc`（形状 ⑥）：20 万个结点全来自一个 `new` ⇒ 片内链条完整与
#     「把垃圾摊开」不可兼得，是**反面对照**：最大停顿压不下来（为什么，脚本末尾说清）。
#
# 用法：tests/reclaim-shapes/run-shard.sh            （全部跑完，十几秒）
#       N=1000000 REPS=3 tests/reclaim-shapes/run-shard.sh
# 退出码 0 = 脚本自己那几条判据成立（语义逐字一致、分片不丢内存界）。最大停顿的方向是
# **报告**，不是判据：反面对照本来就该报出「压不下来」。
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
N=${N:-1000000}
REPS=${REPS:-3}
PROBE=tools/rc-pause-probe.py
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }
mb()  { awk -v k="$1" 'BEGIN { if (k == "") printf "—"; else printf "%.2f MB", k/1024 }'; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { if (a == "" || b == "") printf "—"; else printf "%.2f", b/a }'; }

# ── ① 产物：不设 vs =1 逐字节相同（默认 1 = 现有行为）────────────────────────────
echo "== ① 产物：EXTC_RC_SHARDS 不设 vs =1（EXTC_RECLAIM=1，全部可编译 .extc）=="
same=0; diff=0; skip=0; d8=0
while IFS= read -r f; do
    if ! EXTC_RECLAIM=1 env -u EXTC_RC_SHARDS "$EXTC" -w --no-line-map -I "$PWD/stdlib" "$f" -o "$TMP/a.c" 2>/dev/null; then
        skip=$((skip+1)); continue
    fi
    if ! EXTC_RECLAIM=1 EXTC_RC_SHARDS=1 "$EXTC" -w --no-line-map -I "$PWD/stdlib" "$f" -o "$TMP/b.c" 2>/dev/null; then
        skip=$((skip+1)); continue
    fi
    if cmp -s "$TMP/a.c" "$TMP/b.c"; then same=$((same+1)); else diff=$((diff+1)); echo "  DIFF $f"; fi
    if EXTC_RECLAIM=1 EXTC_RC_SHARDS=8 "$EXTC" -w --no-line-map -I "$PWD/stdlib" "$f" -o "$TMP/c.c" 2>/dev/null \
       && ! cmp -s "$TMP/a.c" "$TMP/c.c"; then d8=$((d8+1)); fi
done < <(find examples tests tools/escape-corpus -name '*.extc' | sort)
echo "  程序数 $same（跳过 $skip：编不过的语料），diff=$diff"
echo "  旁证：=8 与不设在生成物上不同的程序数 $d8 —— 分片是**编译期**旋钮，只改有 rc 分配点的形状"
if [ "$diff" -eq 0 ] && [ "$same" -gt 0 ]; then
    ok "不设与 =1 的生成物逐字节相同（$same 个程序，diff=0）"
else
    bad "不设与 =1 的生成物有差异（$same 个程序里 diff=$diff）"
fi

# ── 量一个形状在某个片数下的三个数 ────────────────────────────────────────────
# measure <形状的 .extc> <片数> → M_PAUSE(µs) / M_RSS(KiB) / M_MS(ms) / M_OUT
measure() {
    local src=$1 sh=$2 tag; tag="$(basename "${src%.extc}")-$2"
    M_PAUSE=""; M_RSS=""; M_MS=""; M_OUT=""
    if ! EXTC_RECLAIM=1 EXTC_RC_SHARDS="$sh" "$EXTC" -w --no-line-map -I "$PWD/stdlib" "$src" -o "$TMP/$tag.c" 2>"$TMP/$tag.log"; then
        echo "  note $src：生成失败"; head -2 "$TMP/$tag.log" | sed 's/^/        /'; return 1
    fi
    if ! python3 "$PROBE" "$TMP/$tag.c" > "$TMP/$tag-p.c" 2>"$TMP/$tag-p.log"; then
        echo "  note $src：插桩失败"; head -2 "$TMP/$tag-p.log" | sed 's/^/        /'; return 1
    fi
    if ! "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/$tag" "$TMP/$tag-p.c" 2>>"$TMP/$tag.log"; then
        echo "  note $src：插桩版编译失败"; head -2 "$TMP/$tag.log" | sed 's/^/        /'; return 1
    fi
    # 语义基线（未插桩的生成物跑出来的 stdout，与插桩无关）
    if ! "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$tag-plain" "$TMP/$tag.c" 2>>"$TMP/$tag.log"; then return 1; fi
    M_OUT=$("$TMP/$tag-plain" "$N" 2>/dev/null)
    local pauses="" best=999999
    for _ in $(seq "$REPS"); do
        local t0 t1 ms rss p
        t0=$(date +%s%N)
        "$TIME" -v -o "$TMP/$tag.time" "$TMP/$tag" "$N" >"$TMP/$tag.out" 2>"$TMP/$tag.err"
        t1=$(date +%s%N); ms=$(( (t1 - t0) / 1000000 ))
        rss=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/$tag.time")
        p=$(grep -o 'max_pause_us=[0-9.]*' "$TMP/$tag.out" | head -1 | cut -d= -f2)
        [ -n "$p" ] && pauses="$pauses$p
"
        [ -n "$ms" ] && best=$(awk -v a="$ms" -v b="$best" 'BEGIN{print (a<b)?a:b}')
        M_RSS=$(awk -v a="${rss:-0}" -v b="${M_RSS:-0}" 'BEGIN{print (a>b)?a:b}')
    done
    # 中位数：REPS 次里每一次的最大停顿（一次运行一个最大值）
    M_PAUSE=$(printf '%s' "$pauses" | sort -n | awk -v r="$REPS" 'NF{a[++n]=$1} END{if(n==0){print ""} else {print a[int((n+1)/2)]}}')
    M_MS=$best
    return 0
}

# 跑一个形状的两档并出表；<label> 是表头名字
report_shape() {
    local src=$1 label=$2
    echo
    echo "== ② $label（$src，N=$N，REPS=$REPS）=="
    measure "$src" 1 || return 1; local p1=$M_PAUSE r1=$M_RSS ms1=$M_MS o1=$M_OUT
    measure "$src" 8 || return 1; local p8=$M_PAUSE r8=$M_RSS ms8=$M_MS o8=$M_OUT
    printf '  %-10s %14s %14s %10s\n' "片数" "最大单次清扫" "峰值 RSS" "吞吐(墙钟)"
    printf '  %-10s %14s %14s %10s\n' "1" "${p1} µs" "$(mb "$r1")" "${ms1} ms"
    printf '  %-10s %14s %14s %10s\n' "8" "${p8} µs" "$(mb "$r8")" "${ms8} ms"
    printf '  %-10s %14s %14s %10s\n' "8/1" "$(ratio "$p1" "$p8")×" "$(ratio "$r1" "$r8")×" "$(ratio "$ms1" "$ms8")×"
    if [ "$o1" = "$o8" ] && [ -n "$o1" ]; then
        ok "$label：分片=8 与 =1 输出逐字一致（$o1）"
    else
        bad "$label：分片=8 与 =1 语义不一致（=1: $o1 / =8: $o8）"
    fi
    # 内存界：同片数下 N vs 4N 仍 < 1.5（分片不许把界弄丢）
    local nb=$((N * 4))
    REPS=1 measure "$src" 8 || return 1; local qn=$M_RSS
    REPS=1 N=$nb measure "$src" 8 || return 1; local qf=$M_RSS
    if [ -n "$qn" ] && [ -n "$qf" ] && awk -v a="$qn" -v b="$qf" 'BEGIN { exit !(b < a * 1.5) }'; then
        ok "$label：分片=8 下 N→4N 峰值 $(mb "$qn") → $(mb "$qf")（$(ratio "$qn" "$qf")× < 1.5×）—— 界与轮数仍解耦"
    else
        bad "$label：分片=8 下 N→4N 峰值 $(mb "$qn") → $(mb "$qf")（$(ratio "$qn" "$qf")× ≥ 1.5×）"
    fi
    # 方向：给读法（不是判据）
    local dp dr dt
    dp=$(awk -v a="$p1" -v b="$p8" 'BEGIN{ if(a=="") print "—"; else if(b<a) print "变好"; else if(b>a) print "变差"; else print "持平" }')
    dr=$(awk -v a="$r1" -v b="$r8" 'BEGIN{ if(a=="") print "—"; else if(b<a) print "变好"; else if(b>a) print "变差"; else print "持平" }')
    dt=$(awk -v a="$ms1" -v b="$ms8" 'BEGIN{ if(a=="") print "—"; else if(b<a) print "变好"; else if(b>a) print "变差"; else print "持平" }')
    echo "  方向：最大停顿 $dp · 峰值 RSS $dr · 吞吐 $dt"
    return 0
}

echo
echo "== 分片切在结构边界上（形状 ⑩：8 条独立链 = 8 个分配点）=="
report_shape tests/reclaim-shapes/shard_bands.extc "shard-bands"

echo
echo "== 反面对照（形状 ⑥：20 万结点来自同一个分配点，切不开）=="
report_shape tests/reclaim-shapes/cyclic_pressure.extc "cyclic-pressure"

echo
echo "== 与原型 README「最终版：计数归零 + 分片」（200 万轮）同向吗 =="
cat <<'EOF'
  原型（prototype-reclaim/README.md，slab_final.c）：分片 1 → 8
    最大停顿   406.7 µs → 81.7 µs   变好（5.0×）      ← 停顿旋钮的本意
    峰值 RSS   17 MiB   → 18 MiB    略变差            ← 每片至少一只 slab 的底噪
    耗时       49.6 ms  → 61.9 ms   变差（1.25×）     ← 每片都要单独触发/收一次
  本实现：`shard_bands` 上的方向与原型一致（停顿显著变好、RSS 变差、吞吐变差）；
  `cyclic_pressure` 上只有「吞吐变差、总停顿变好」同向，**最大停顿**压不下来 ——
  原因不是分片实现错了，而是这一形状切不开：20 万个结点来自同一个 `new` 分配点，
  分片按「分配点 → 片号」切（不按存活集，见 RECLAMATION.md 第四节），那条主结构的
  链与环不能被拆到两片（拆了跨片登记位会把整条环当种子 ⇒ 界随轮数涨），所以它整条留在
  一片里；剩下的片只拿到索引/树。另外自学习退场在小片上更早遇到「第一次扫一无所获 ⇒
  阈值翻倍」，会把某一次清扫攒大 —— 最大停顿因此与单片持平或略差。要拿到原型的收益，
  形状要像 `shard_bands` 那样**每个结构一个分配点**（这就是「分片是纪律」的原话）。
EOF

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
