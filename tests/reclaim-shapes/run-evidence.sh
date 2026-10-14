#!/usr/bin/env bash
# tests/reclaim-shapes/run-evidence.sh —— **清扫真的上场了吗** + 与 C 原型三方对照
#
# 这不是门禁（门禁是 run-switch.sh 的 ①b）；这是**证据脚本**，回答两个问题：
#
#   ① 开关打开时峰值被压住，到底是**清扫**干的，还是计数归零就够了？
#      做法：同一份生成物、同一个负载，只动清扫的触发阈值 ——
#        · 变体 A：`-DEXTC_RC_FLOOR=(1<<40)`（生成物里的运行期是 `#ifndef` 守卫的宏）
#          ⇒ 区域占用永远到不了压力线 ⇒ `extc_rc_maybeCollect` 一次都不收。
#          如果峰值**重新随轮数涨**，就说明界不是计数给的（环垃圾计数永不归零），
#          只能是清扫给的。
#        · 变体 B：默认阈值 ⇒ 峰值平。
#      再加上开关关掉的基线，三者一比，因果就闭合了。
#      另附：gdb 断点直接数 `extc_rc_sweep` 的调用次数（有 gdb 才数）。
#
#   ② 与 `prototype-reclaim/stress.c`（C 原型）三方对照：同 N / 4N 的耗时、峰值 RSS、
#      是否随轮数增长。原型参数取 `N 1024 64 20000 48 <分片>`（缓存 1024、每 64 轮、索引
#      2 万项、48 字节节点），与 extC 形状的旋钮对齐；分片 1 与 8 各给一行。
#
# 用法：tests/reclaim-shapes/run-evidence.sh           （全部跑完，秒级）
# 峰值内存一律取 `/usr/bin/time -v` 的 Maximum resident set size。
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
TIME=${TIME:-/usr/bin/time}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

SRC=tests/reclaim-shapes/cyclic_pressure.extc
WANT_N="rounds=200000 retired=22208 live=128 fwd=25591744 back=25591744 idx=20000 cache=1024 hits=155520 depth=212 tree=true"
WANT_F="rounds=800000 retired=88832 live=512 fwd=409468672 back=409468672 idx=20000 cache=1024 hits=622272 depth=80 tree=true"
NO_SWEEP_FLOOR=1099511627776          # 1 TiB：区域占用永远到不了

pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }
mb()  { awk -v k="$1" 'BEGIN { if (k == "") printf "—"; else printf "%.2f MB", k/1024 }'; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b/a }'; }

# run_one <bin> [args...] → OUT_RC / OUT_RSS(KiB) / OUT_MS / OUT_OUT
run_one() {
    local t0 t1
    t0=$(date +%s%N)
    "$TIME" -v -o "$TMP/tv" "$@" >"$TMP/o" 2>"$TMP/e"
    OUT_RC=$?
    t1=$(date +%s%N)
    OUT_MS=$(( (t1 - t0) / 1000000 ))
    OUT_RSS=$(awk -F': ' '/Maximum resident set size \(kbytes\)/{print $2}' "$TMP/tv")
    OUT_OUT=$(cat "$TMP/o")
}

echo "== ① 清扫是不是压住峰值的那一层？（只动清扫触发阈值）=="

# 开关关：连回收运行期都不发射。
# **必须是 `=0`，不能是 `env -u`**：`EXTC_RECLAIM` 现在是**默认开**（`src/main.c` 的帮助文本、
# `codegen.c` 的 `scanUnitNeeds`），`unset` 那一档生成物与 `=1` **逐字节相同** ⇒ off_ratio
# 比的是开档对自己，第三条断言（"开关关峰值比 > 2"）永远红，且 ② 表里"开关关"那行名不副实。
EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "$SRC" -o "$TMP/off.c" >"$TMP/off.log" 2>&1 \
    && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/off" "$TMP/off.c" >>"$TMP/off.log" 2>&1
# 开关开：默认阈值。
EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$SRC" -o "$TMP/on.c" >"$TMP/on.log" 2>&1 \
    && "$CC" -std=c11 -fwrapv -O1 -o "$TMP/on" "$TMP/on.c" >>"$TMP/on.log" 2>&1
# 开关开 + 清扫永不触发：改的是生成物里的 `#ifndef EXTC_RC_FLOOR` 宏。
"$CC" -std=c11 -fwrapv -O1 -DEXTC_RC_FLOOR=$NO_SWEEP_FLOOR -o "$TMP/nosweep" "$TMP/on.c" >>"$TMP/on.log" 2>&1
# 数清扫次数需要函数不被内联。
"$CC" -std=c11 -fwrapv -O1 -fno-inline -o "$TMP/on-nogdb" "$TMP/on.c" >>"$TMP/on.log" 2>&1

printf '  %-46s %14s %14s %8s\n' "变体" "N=200000" "N=800000" "比值"
for spec in off on nosweep; do
    case $spec in
        off)     name="开关关（无回收运行期）" ;;
        on)      name="开关开（默认阈值 1 MiB）" ;;
        nosweep) name="开关开 + 清扫永不触发（floor=1 TiB）" ;;
    esac
    run_one "$TMP/$spec" 200000; a_rc=$OUT_RC; a_rss=$OUT_RSS; a_out=$OUT_OUT; a_ms=$OUT_MS
    run_one "$TMP/$spec" 800000; b_rc=$OUT_RC; b_rss=$OUT_RSS; b_out=$OUT_OUT; b_ms=$OUT_MS
    r=$(ratio "$a_rss" "$b_rss")
    printf '  %-46s %14s %14s %8s\n' "$name" "$(mb "$a_rss")" "$(mb "$b_rss")" "${r}×"
    eval "${spec}_ratio=$r"; eval "${spec}_sem=$a_rc/$b_rc"
    eval "${spec}_wantA=$([ "$a_out" = "$WANT_N" ] && echo yes || echo no)"
    eval "${spec}_wantB=$([ "$b_out" = "$WANT_F" ] && echo yes || echo no)"
done

# 语义：三种变体在 N 与 4N 上的输出都必须与期望逐字一致（环结构在两种模式下都对）。
sem_all=yes
for spec in off on nosweep; do
    eval "x=\${${spec}_wantA}/\${${spec}_wantB}"; [ "$x" = "yes/yes" ] || sem_all=no
done
if [ "$sem_all" = yes ]; then
    ok "三种变体 N/4N 输出都与期望逐字一致（有环语义与清扫/开关无关）"
else
    bad "有变体的输出与期望不一致（形状或回收运行期语义变了）"
fi

if awk -v r="$on_ratio" 'BEGIN { exit !(r < 1.5) }'; then
    ok "开关开（默认）：峰值比 ${on_ratio}× < 1.5 —— 界与轮数解耦"
else
    bad "开关开（默认）峰值比 ${on_ratio}× ≥ 1.5"
fi
if awk -v r="$nosweep_ratio" 'BEGIN { exit !(r > 2.0) }'; then
    ok "清扫永不触发：峰值比 ${nosweep_ratio}× > 2 —— 界**不是**计数给的，是清扫给的（因果闭合）"
else
    bad "清扫永不触发时峰值比只有 ${nosweep_ratio}×：这份形状证明不了清扫的作用"
fi
if awk -v r="$off_ratio" 'BEGIN { exit !(r > 2.0) }'; then
    ok "开关关：峰值比 ${off_ratio}× > 2 —— run-switch.sh 里那条判据因此“有牙”"
else
    bad "开关关峰值比只有 ${off_ratio}×（判据没牙）"
fi

# 直接数清扫次数：gdb 断点 `extc_rc_sweep`。
echo
echo "  -- 清扫调用次数（gdb 断点，能数才数）--"
if command -v gdb >/dev/null 2>&1 && nm "$TMP/on-nogdb" 2>/dev/null | grep -q ' extc_rc_sweep$'; then
    cat >"$TMP/count.gdb" <<'GDB'
set pagination off
set confirm off
set $sw = 0
break extc_rc_sweep
commands
silent
set $sw = $sw + 1
continue
end
run 800000
printf "SWEEPS=%d\n", $sw
GDB
    sw=$(timeout 300 gdb -batch -x "$TMP/count.gdb" "$TMP/on-nogdb" 2>/dev/null | awk -F= '/^SWEEPS=/{print $2}')
    if [ -n "${sw:-}" ] && [ "$sw" -gt 0 ]; then
        ok "N=800000 开关开：extc_rc_sweep 被调用 $sw 次（清扫确实执行）"
    else
        bad "gdb 数不到 extc_rc_sweep（没拿到次数）"
    fi
else
    echo "  note 没有 gdb 或符号不可用，跳过（上面的阈值对照已经足够）"
fi

# ── ② 三方对照 ────────────────────────────────────────────────────────────────
echo
echo "== ② 三方对照：extC 关档（EXTC_RECLAIM=0）/ extC 开档（=1）/ C 原型 stress.c（同 N / 4N）=="
PROTO=prototype-reclaim/stress.c
proto_ok=no
if [ -f "$PROTO" ] && "$CC" -O2 -std=c11 -o "$TMP/stress" "$PROTO" >"$TMP/stress.log" 2>&1; then
    proto_ok=yes
else
    echo "  note 编不出 C 原型（$PROTO），这一节跳过"; head -3 "$TMP/stress.log" 2>/dev/null | sed 's/^/        /'
fi

printf '  %-34s %-22s %-26s %s\n' "实现" "耗时 N / 4N (ms)" "峰值 RSS N / 4N" "是否随轮数增长"
for spec in off on; do
    [ "$spec" = off ] && name="extC EXTC_RECLAIM=0（关档）" || name="extC EXTC_RECLAIM=1（开档）"
    run_one "$TMP/$spec" 200000; a_ms=$OUT_MS; a_rss=$OUT_RSS
    run_one "$TMP/$spec" 800000; b_ms=$OUT_MS; b_rss=$OUT_RSS
    eval "r=\$${spec}_ratio"
    grew=$([ "$(awk -v r="$r" 'BEGIN{print (r>1.5)?"y":"n"}')" = y ] && echo "是（${r}×）" || echo "否（${r}×）")
    printf '  %-34s %-22s %-26s %s\n' "$name" "$a_ms / $b_ms" "$(mb "$a_rss") / $(mb "$b_rss")" "$grew"
done
if [ "$proto_ok" = yes ]; then
    for NS in 1 8; do
        run_one "$TMP/stress" 200000 1024 64 20000 48 "$NS"; a_ms=$OUT_MS; a_rss=$OUT_RSS
        run_one "$TMP/stress" 800000 1024 64 20000 48 "$NS"; b_ms=$OUT_MS; b_rss=$OUT_RSS
        r=$(ratio "$a_rss" "$b_rss")
        grew=$([ "$(awk -v r="$r" 'BEGIN{print (r>1.5)?"y":"n"}')" = y ] && echo "是（${r}×）" || echo "否（${r}×）")
        printf '  %-34s %-22s %-26s %s\n' "C 原型 stress.c（分片=$NS）" "$a_ms / $b_ms" "$(mb "$a_rss") / $(mb "$b_rss")" "$grew"
    done
    # 原型自己在 README 里的量级：200 万轮
    run_one "$TMP/stress" 2000000 1024 64 20000 48 8; ms2=$OUT_MS; rss2=$OUT_RSS
    printf '  %-34s %-22s %-26s %s\n' "C 原型 stress.c（分片=8，含 200 万轮）" "$ms2（200 万轮）" "$(mb "$rss2")" "否（41 MiB 量级）"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
