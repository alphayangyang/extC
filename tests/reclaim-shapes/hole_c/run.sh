#!/usr/bin/env bash
# tests/reclaim-shapes/hole_c/run.sh —— 复现判据：回收档下 hole_c.extc 必须红（提前释放 UAF），
# 关闭档必须绿。**exit 0 = 复现成功**（与门禁脚本相反的方向：这一支是"缺陷还在"的证据脚本；
# 被门禁接进去时按反向断言用 —— 复现成功 ⇒ 门禁红）。
#
# 自包含：不读同目录以外的脚本状态（tests/reclaim-shapes 下既有的 run.sh / run-switch.sh /
# README.md 一个字都不碰）。
#
# 开关口径（`reclaim-default` 分支起）：回收**默认打开**，`EXTC_RECLAIM=0` 关掉，
# `EXTC_RECLAIM=1` 强制打开（不看分配点）。所以：
#   开档 = `EXTC_RECLAIM=1`（任务验收口径），关档 = `EXTC_RECLAIM=0`（对照）。
#
# 判据四条：
#   ① 开档 + 非 ASan：rc 非 0（信号 11）或输出不是 `n = 131071` —— 提前释放可观测；
#   ② 开档 + ASan：rc 非 0，报告里出现 SEGV / UAF；打印报告抬头（含空闲链号地址）；
#   ③ 关档（`EXTC_RECLAIM=0`）：rc=0 且输出逐字 `n = 131071` —— 对照组；
#   ④ 有牙的对照：深度 15 没过 `EXTC_RC_FLOOR`(1 MiB) ⇒ 开档也不崩、输出 `n = 65535`。
#
# 用法：bash tests/reclaim-shapes/hole_c/run.sh
#       EXTC=/path/to/extc bash tests/reclaim-shapes/hole_c/run.sh
set -u
cd "$(dirname "$0")/../../.."          # 仓库根

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
SRC=tests/reclaim-shapes/hole_c/hole_c.extc
WANT="n = 131071"                      # 2^17 - 1 个节点（深度 16 的满二叉树）
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

[ -x "$EXTC" ] || { echo "缺少可执行编译器 $EXTC（先 make）"; exit 2; }

gen() { # <开|关> <tag> <源文件> —— 生成 C（开档强制 EXTC_RECLAIM=1；关档 =0）
    if [ "$1" = 开 ]; then
        EXTC_RECLAIM=1 "$EXTC" -w --no-line-map "$3" -o "$TMP/$2.c" >"$TMP/$2.gen.log" 2>&1
    else
        EXTC_RECLAIM=0 "$EXTC" -w --no-line-map "$3" -o "$TMP/$2.c" >"$TMP/$2.gen.log" 2>&1
    fi
}
cc_build() { # <tag> [asan] —— 生成物 $TMP/<tag>.c → 可执行 $TMP/<tag>
    if [ "${2-}" = asan ]; then
        "$CC" -std=c11 -fwrapv -O1 -g -fsanitize=address -o "$TMP/$1" "$TMP/$1.c" \
            >"$TMP/$1.cc.log" 2>&1
    else
        "$CC" -std=c11 -fwrapv -O1 -o "$TMP/$1" "$TMP/$1.c" >"$TMP/$1.cc.log" 2>&1
    fi
}
gen_ok() { grep -q 'extc_rc_alloc' "$TMP/$1.c" 2>/dev/null; }
run() { # <tag> [asan] —— 跑；套一层 `sh -c` 把 bash 的 "Segmentation fault" 噪声留在子 shell 里
    if [ "${2-}" = asan ]; then
        ASAN_OPTIONS=detect_leaks=0 sh -c '"$0" >"$1" 2>"$2"' \
            "$TMP/$1" "$TMP/$1.out" "$TMP/$1.err" 2>/dev/null
    else
        sh -c '"$0" >"$1" 2>"$2"' "$TMP/$1" "$TMP/$1.out" "$TMP/$1.err" 2>/dev/null
    fi
    R_RC=$?
    R_OUT=$(cat "$TMP/$1.out")
}

echo "== hole_c：回收档下 bt.extc 的提前释放（UAF）最小形状 =="
echo "   源文件 $SRC（$(wc -l < "$SRC") 行）；期望 开档=红 / 关档=绿"

# ── ①② 开档：非 ASan 与 ASan 各跑一遍（两个 tag，互不覆盖）──────────────────
if gen 开 on "$SRC" && cc_build on && gen_ok on; then
    run on; on_rc=$R_RC; on_out=$R_OUT
    if [ "$on_rc" != 0 ] || [ "$on_out" != "$WANT" ]; then
        ok "开档（非 ASan）：rc=$on_rc out='$on_out' ≠ '$WANT' —— 提前释放可观测"
    else
        bad "开档（非 ASan）：rc=0 且 out='$WANT' —— 复现失败（洞已被修？）"
    fi
    if gen 开 ona "$SRC" && cc_build ona asan; then
        run ona asan; asan_rc=$R_RC
        if [ "$asan_rc" != 0 ] && grep -qE 'SEGV|heap-use-after-free|DEADLYSIGNAL' "$TMP/ona.err"; then
            ok "开档（ASan）：rc=$asan_rc，报告命中 SEGV/UAF"
            echo "       ── ASan 报告抬头 ──"
            grep -m1 -E 'SEGV on unknown address|heap-use-after-free' "$TMP/ona.err" | sed 's/^/       /'
            grep -m1 -E '^    #0 ' "$TMP/ona.err" | sed 's/^/       /'
            # 解引用到的值就是空闲链号/链尾：单看寄存器行（`rbx` = 被当指针读的那个值）
            awk '/Register values:/{getline; print "       " $0; exit}' "$TMP/ona.err"
            grep -m1 -E 'SUMMARY: AddressSanitizer' "$TMP/ona.err" | sed 's/^/       /'
        else
            bad "开档（ASan）：rc=$asan_rc，但报告里没有 SEGV/UAF"
        fi
    else
        bad "开档（ASan）：生成或编译失败"
    fi
else
    bad "开档：生成或编译失败（或生成物里没有回收运行期）"
    sed 's/^/       /' "$TMP/on.gen.log" "$TMP/on.cc.log" 2>/dev/null | head -6
fi

# ── ③ 关档：对照组必须绿 ─────────────────────────────────────────────────────
if gen 关 off "$SRC" && cc_build off; then
    run off; off_rc=$R_RC; off_out=$R_OUT
    if [ "$off_rc" = 0 ] && [ "$off_out" = "$WANT" ]; then
        ok "关档（EXTC_RECLAIM=0）：rc=0 out='$off_out' —— 对照组绿"
    else
        bad "关档：rc=$off_rc out='$off_out'（期望 rc=0 / '$WANT'）"
    fi
else
    bad "关档：生成或编译失败"
fi

# ── ④ 有牙的对照：深度 15 没过 1 MiB 压力下限 ⇒ 开档也不该崩 ─────────────────
sed 's/build(16)/build(15)/' "$SRC" >"$TMP/d15.extc"
if gen 开 d15 "$TMP/d15.extc" && cc_build d15; then
    run d15; d15_rc=$R_RC; d15_out=$R_OUT
    if [ "$d15_rc" = 0 ] && [ "$d15_out" = "n = 65535" ]; then
        ok "开档 + 深度 15（65535 节点 = 1.0 MiB - 16 B）：rc=0 —— 触发点由 1 MiB 下限把关"
    else
        bad "开档 + 深度 15：rc=$d15_rc out='$d15_out'（期望 rc=0 / 'n = 65535'）"
    fi
else
    bad "开档 + 深度 15：生成或编译失败"
fi

# ── 附：生成物里的触发点根表（信息，不做断言）────────────────────────────────
if [ -f "$TMP/on.c" ]; then
    echo "   生成物触发点（根表为空 = 外层调用帧的活引用不在种子里）："
    grep -o 'extc_rc_maybeCollect(__extc_rcg[0-9]*, (void \*const \*)[^,]*, [0-9]*)' "$TMP/on.c" \
        | head -1 | sed 's/^/       /'
fi

echo
echo "复现判据：通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
