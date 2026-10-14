#!/usr/bin/env bash
# tests/reclaim-perf/gate-shard-cycles.sh —— 分片在高环负载形状上**必须有界**，且旋钮不许被关掉。
#
# 两道：
#   ① 环负载形状 `bench/gc/src/digraph.extc`（只读引用，不改它）：
#        · 默认档的峰值必须 ≤ **0.5× 同 N 的关档基准**（N 与 4N 两档都成立）。
#          判据与同 N 的关档基准比：绝对峰值里混着每槽元数据这样的常数项，两个规模一比
#          就会被常数项掩盖 —— 4N/N 看着有界，实际量的是"常数地板还在不在"；与关档比
#          就把常数项消掉，留下的才是回收真正压住的增量；
#        · 默认档的 4N/N 必须 **< 关档的 4N/N**（回收不许让增长速度比不回收更差）；
#        · `SHARDS=4` 的 4N 绝对峰值 **≤ 2× 默认档**（分片不许把环负载抬起来）。
#     口径：`/usr/bin/time -v` 的 Maximum resident set size（自足，不依赖 bench/gc 的垫片）。
#     牙：注入 `sweep-never` ⇒ 前两条红、第三条仍绿（格与格独立，不互相掩盖）。
#   ② 不许把旋钮关掉：分片友好的对齐形状 `tests/reclaim-shapes/shard_bands.extc` 上
#        `SHARDS=4` 的**平均切片时长**（运行期 `[step]` 的 `total_us/n`，取 REPS 次最小值）
#        必须**明显低于** `SHARDS=1`（判据取 < 0.75× 留余量），两种片数都**不许有 ≥1ms 的切片**
#        （`[step] over1ms == 0`，窗口有绝对上界），且两种片数下程序输出逐字一致。
#        口径说明：旧判据比的是探针的 `max_pause_us`（只包 `collect`），热路径改走 `step` 后
#        恒为 0 ⇒ `0 < 0.75×0` 恒假。现在比**每个切片**的均值（~1e4 样本）＋ 绝对上界；
#        另加一条元判据：探针必须报告 `PROBE step_n > 0`（覆盖热路径），否则红。
#
# 为什么这样修：片边界必须与结构边界对齐。跨**站点**的可能环一旦被切开，跨片登记位就成了
# 环垃圾的种子，界随轮数涨。编译器现在（`scanUnitReclaim`）对"单元里存在跨站点可能环"的情形
# **拒绝分片**；单站点自环（`shard_bands` 每条链一个分配点）不受影响，停顿收益保留。
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
STDLIB=${STDLIB:-stdlib}
N1=${N1:-250000}
N2=${N2:-1000000}
SEED=${SEED:-20261013}
BAND_N=${BAND_N:-300000}
REPS=${REPS:-3}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

# ── 量具：跑一次并取内核报的峰值 RSS（KB）；子进程退出码写进 `rss_last_rc` ────────
rss_last_rc=0
rss_kb() {   # rss_kb <程序> <N>
    local t=$TMP/rss.time
    /usr/bin/time -v -o "$t" "$1" "$2" "$SEED" >/dev/null 2>/dev/null
    rss_last_rc=$?
    sed -n 's/.*Maximum resident set size (kbytes): \([0-9]*\)/\1/p' "$t"
}
build() {    # build <标签> <env...> -- 生成 C 再 cc
    local tag=$1; shift
    if ! env -u EXTC_RC_SHARDS -u EXTC_RECLAIM "$@" "$EXTC" -O2 -I "$STDLIB" -w --no-line-map \
         -o "$TMP/$tag.c" bench/gc/src/digraph.extc 2>"$TMP/$tag.err"; then
        bad "digraph 生成失败（$tag）：$(head -1 "$TMP/$tag.err")"; return 1
    fi
    $CC -O2 -o "$TMP/$tag" "$TMP/$tag.c" 2>>"$TMP/$tag.err" || { bad "cc 失败（$tag）"; return 1; }
    return 0
}

echo "== ① 环负载：digraph（N=$N1 / 4N=$N2）=="
build d1 env                                   || exit 1
build d4 env EXTC_RC_SHARDS=4                  || exit 1
build d0 env EXTC_RECLAIM=0                    || exit 1
r1a=$(rss_kb "$TMP/d1" "$N1"); q1a=$rss_last_rc
r1b=$(rss_kb "$TMP/d1" "$N2"); q1b=$rss_last_rc
r4a=$(rss_kb "$TMP/d4" "$N1"); q4a=$rss_last_rc
r4b=$(rss_kb "$TMP/d4" "$N2"); q4b=$rss_last_rc
r0a=$(rss_kb "$TMP/d0" "$N1"); q0a=$rss_last_rc
r0b=$(rss_kb "$TMP/d0" "$N2"); q0b=$rss_last_rc
if [ -z "$r1a$r1b$r4a$r4b$r0a$r0b" ]; then bad "量不到 RSS（/usr/bin/time -v 不可用？）"; exit 1; fi
if [ "$q1a$q1b$q4a$q4b$q0a$q0b" != "000000" ]; then
    bad "有程序非零退出（rc=$q1a/$q1b/$q4a/$q4b/$q0a/$q0b）⇒ RSS 不可读"; exit 1
fi
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", a ? b/a : 0 }'; }
frac()  { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b ? a/b : 0 }'; }
echo "  默认档      $N1=$((r1a/1024))MB → $N2=$((r1b/1024))MB  4N/N=$(ratio "$r1a" "$r1b")"
echo "  SHARDS=4    $N1=$((r4a/1024))MB → $N2=$((r4b/1024))MB  4N/N=$(ratio "$r4a" "$r4b")"
echo "  关档基准    $N1=$((r0a/1024))MB → $N2=$((r0b/1024))MB  4N/N=$(ratio "$r0a" "$r0b")"
# ①a 回收必须比**同 N 的关档基准**小一半以上（两个规模都成立）。判据与同 N 的关档基准比：
#     绝对峰值里混着每槽元数据这样的常数项，两个规模一比就会被常数项掩盖（4N/N 看着有界，
#     其实量的是"常数地板还在不在"）。与关档比就把常数项消掉：回收那部分必须真的压住增量。
for pair in "$N1 $r1a $r0a" "$N2 $r1b $r0b"; do
    set -- $pair
    if [ "$(awk -v on="$2" -v off="$3" 'BEGIN{print (on <= 0.5*off) ? 1 : 0}')" = 1 ]; then
        ok "默认档 N=$1：峰值 ≤ 0.5× 关档（$(( $2 /1024))MB vs $(( $3/1024))MB，比 $(frac "$2" "$3")）"
    else
        bad "默认档 N=$1：峰值 > 0.5× 关档（比 $(frac "$2" "$3")）⇒ 回收没压住增量"
    fi
done
# ①b 回收不许让增长速度比不回收更差。`EXTC_RC_SHARDS > 1` 把一条环切开时，跨片登记位会把
#     环垃圾钉住 ⇒ 开档的 4N/N 会追平关档 ⇒ 这条红。
[ "$(awk -v on="$r1b" -v a="$r1a" -v off="$r0b" -v b="$r0a" 'BEGIN{print (a && b && on/a < off/b) ? 1 : 0}')" = 1 ] \
  && ok "默认档 4N/N < 关档 4N/N（$(ratio "$r1a" "$r1b") < $(ratio "$r0a" "$r0b")）" \
  || bad "默认档 4N/N ≥ 关档 4N/N（$(ratio "$r1a" "$r1b") vs $(ratio "$r0a" "$r0b")）⇒ 界比不回收还差"
# ①c 分片旋钮不许把环负载的绝对峰值抬起来（相对默认档）。
[ "$(awk -v a="$r1b" -v b="$r4b" 'BEGIN{print (b <= 2*a) ? 1 : 0}')" = 1 ] \
  && ok "SHARDS=4 的 4N 绝对峰值 ≤ 2× 默认档（$((r4b/1024))MB vs $((r1b/1024))MB）" \
  || bad "SHARDS=4 的 4N 峰值 > 2× 默认档（$((r4b/1024))MB vs $((r1b/1024))MB）"

echo "== ② 旋钮不许被关掉：shard_bands（对齐形状，N=$BAND_N，REPS=$REPS）=="
# 口径（2026-10-15 修）：merge 后热路径走 `extc_rc_step`，**契约入口**的 `max_pause_us` 恒 0，
# 旧判据 `0 < 0.75×0` 恒假。正确的窗口量是**每个切片**的停顿：运行期 `[step]` 现在把
# `markStep`/`sweepStep` 每次调用都记进 n/max/total + 阈值桶（over16/over64/over256/over1ms）。
# `max` 是长尾（同版本两次能差几倍，受抢占影响）⇒ 判据用**平均切片时长 total_us/n**
# （~1e4 个样本，稳定；取 REPS 次的最小值）＋ 绝对上界 `over1ms == 0`。
band() {     # band <片数> → "<平均切片µs>|<over1ms>|<n>|<maxµs>|<over64>|<语义输出sha12>"
    local sh=$1 f="tests/reclaim-shapes/shard_bands.extc"
    if [ "$sh" = 1 ]; then
        EXTC_RECLAIM=1 env -u EXTC_RC_SHARDS "$EXTC" -O2 -I "$STDLIB" -w -o "$TMP/b$sh.c" "$f" 2>/dev/null
    else
        EXTC_RECLAIM=1 EXTC_RC_SHARDS=$sh "$EXTC" -O2 -I "$STDLIB" -w -o "$TMP/b$sh.c" "$f" 2>/dev/null
    fi
    $CC -std=c11 -fwrapv -O1 -o "$TMP/b$sh" "$TMP/b$sh.c" 2>/dev/null || return 1
    local best="" ov1="" nn="" mx="" ov64=""
    for _ in $(seq 1 "$REPS"); do
        local line tot mean o64 o1
        line=$(EXTC_RC_PAUSE=1 "$TMP/b$sh" "$BAND_N" 2>&1 >/dev/null | grep '^\[step\]' | tail -1)
        nn=$(printf '%s\n' "$line"   | sed -n 's/^\[step\] n=\([0-9]*\) .*/\1/p')
        mx=$(printf '%s\n' "$line"   | sed -n 's/.* max_us=\([0-9.]*\) .*/\1/p')
        tot=$(printf '%s\n' "$line"  | sed -n 's/.* total_us=\([0-9.]*\) .*/\1/p')
        o64=$(printf '%s\n' "$line"  | sed -n 's/.* over64=\([0-9]*\) .*/\1/p')
        o1=$(printf '%s\n' "$line"   | sed -n 's/.* over1ms=\([0-9]*\).*/\1/p')
        [ -n "$nn" ] && [ "$nn" != 0 ] && [ -n "$tot" ] || return 1
        mean=$(awk -v t="$tot" -v k="$nn" 'BEGIN{printf "%.4f", t/k}')
        best=$(awk -v a="$best" -v b="$mean" 'BEGIN{ print (a=="" || b<a) ? b : a }')   # min：抗抢占
        ov64=$(awk -v a="$ov64" -v b="$o64" 'BEGIN{ print (a=="" || b<a) ? b : a }')    # min：抗抢占
        ov1=$(awk -v a="$ov1" -v b="$o1" 'BEGIN{ print (a=="" || b>a) ? b : a }')       # max：安全上界
    done
    local sha
    sha=$("$TMP/b$sh" "$BAND_N" 2>/dev/null | sha256sum | cut -c1-12)
    printf '%s|%s|%s|%s|%s|%s' "$best" "$ov1" "$nn" "$mx" "$ov64" "$sha"
}
IFS="|" read -r m1 o1 n1 x1 c1 s1 <<<"$(band 1)"; IFS="|" read -r m4 o4 n4 x4 c4 s4 <<<"$(band 4)"
if [ -z "$m1" ] || [ -z "$m4" ]; then
    bad "窗口读数没取到（$m1 / $m4）：运行期 [step] 行缺字段（over1ms/n/total_us）？"
else
    echo "  平均切片：SHARDS=1 ${m1}µs（n=$n1 · max=${x1}µs · over64=$c1 · over1ms=$o1）· SHARDS=4 ${m4}µs（n=$n4 · max=${x4}µs · over64=$c4 · over1ms=$o4）· 4/1=$(awk -v a="$m1" -v b="$m4" 'BEGIN{printf "%.2f", a?b/a:0}')"
    [ "$o1" = 0 ] && [ "$o4" = 0 ] \
      && ok "两种片数都没有 ≥1ms 的切片（over1ms=0/0）⇒ 窗口有绝对上界" \
      || bad "出现 ≥1ms 的切片（over1ms=$o1/$o4）⇒ 窗口没有绝对上界"
    [ "$c1" -le 5 ] 2>/dev/null && [ "$c4" -le 5 ] 2>/dev/null \
      && ok "两种片数的 >64µs 长切片都受控（over64=$c1/$c4 ≤ 5，REPS 取最小）" \
      || bad ">64µs 的长切片变多（over64=$c1/$c4 > 5）⇒ 切片预算没兜住（注入 SWEEP_BUDGET=1e8 时这里是 73/73）"
    [ "$(awk -v a="$m1" -v b="$m4" 'BEGIN{print (b < 0.75*a) ? 1 : 0}')" = 1 ] \
      && ok "SHARDS=4 的平均切片明显小于 SHARDS=1（< 0.75×）⇒ 旋钮没被关掉" \
      || bad "SHARDS=4 没拿到切片收益（${m4}µs vs ${m1}µs）——分片被钉死了"
fi
[ "$s1" = "$s4" ] && ok "shard_bands 两种片数输出逐字一致（sha $s1）" \
                  || bad "shard_bands 两种片数输出不同（$s1 vs $s4）"

# 元判据：探针必须覆盖热路径（`PROBE step_n > 0`）。merge 后热路径走 `extc_rc_step`，
# 只包 `collect` 的旧探针会让 `max_pause_us` 恒 0（历史洞：`0 < 0` 恒假、闸门假绿）。
probe_covers_step() {
    python3 tools/rc-pause-probe.py "$TMP/b1.c" > "$TMP/b1-p.c" 2>/dev/null || return 1
    $CC -std=c11 -D_POSIX_C_SOURCE=200809L -fwrapv -O1 -o "$TMP/b1-p" "$TMP/b1-p.c" 2>/dev/null || return 1
    local out sn
    out=$("$TMP/b1-p" "$BAND_N" 2>/dev/null)
    sn=$(printf '%s\n' "$out" | grep -o 'step_n=[0-9]*' | head -1 | cut -d= -f2)
    [ -n "$sn" ] && [ "$sn" -gt 0 ] 2>/dev/null
}
probe_covers_step && ok "停顿探针覆盖热路径（PROBE step_n > 0）" \
                  || bad "停顿探针没覆盖热路径（PROBE 缺 step_n 或 = 0）⇒ 窗口判据会重新变瞎"

echo
echo "通过 $pass，失败 $fail"
[ "$fail" = 0 ] || exit 1
echo "  ok   分片在高环负载形状上有界，且对齐形状仍有停顿收益"
