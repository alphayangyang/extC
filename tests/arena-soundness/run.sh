#!/usr/bin/env bash
# arena 健全性**反例库**（2026-09-23）—— 配套 `ARENA-SOUNDNESS.md`
#
# 每一条都是**当前编译器会接受、运行时真的悬垂**的程序 ⇒ 这是 soundness 的证伪证据，
# 所以**不接进 `tests/run.sh`**（那里是"必须被拒/必须跑对"的回归）。
# 用法： `./run.sh`      —— 逐条编译 + ASan 跑，打印判定
#        `./run.sh A`   —— 只跑文件名以 A 打头的那条
#
# ⚠️ 判据：`ACCEPT + ASan UAF` = 反例成立（洞还在）；
#          `REJECT`               = 洞已被修好（这条就该从这里毕业、搬进 tests/errors/）✓
set -u
cd "$(dirname "$0")"
EXTC=${EXTC:-../../build/extc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
sel=${1:-}

pass=0; fail=0
for f in [A-Z]*.extc; do
    case "$f" in ${sel}*) ;; *) continue ;; esac
    n=${f%.extc}
    "$EXTC" "$f" -o "$TMP/$n.c" >"$TMP/$n.cerr" 2>&1
    rc=$?
    if [ $rc -ge 128 ]; then
        # ⚠️ 编不过**不算修好**：这一族是 R1（顶层初始化式 ⇒ SIGSEGV）。
        #    要的是**响亮报错**，不是崩溃 ✓
        printf '  \033[31mCRASH\033[0m   %-34s 编译器信号 %d（要报错，不要崩）\n' "$n" "$((rc-128))"
        fail=$((fail+1)); continue
    fi
    if [ $rc -ne 0 ]; then
        printf '  \033[32mREJECT\033[0m  %-34s （洞已修：%s）\n' "$n" "$(head -1 "$TMP/$n.cerr" | cut -c1-60)"
        pass=$((pass+1)); continue
    fi
    if ! gcc -O1 -g -fsanitize=address -o "$TMP/$n" "$TMP/$n.c" >"$TMP/$n.gerr" 2>&1; then
        printf '  \033[33mGCC-ERR\033[0m %-34s %s\n' "$n" "$(head -1 "$TMP/$n.gerr" | cut -c1-60)"
        fail=$((fail+1)); continue
    fi
    out=$(timeout 20 "$TMP/$n" 2>&1 || true)
    kind=$(printf '%s' "$out" | grep -m1 -o 'ERROR: AddressSanitizer: [a-z-]*' || true)
    if [ -n "$kind" ]; then
        freed=$(printf '%s' "$out" | grep -c 'extc_arena_release' || true)
        printf '  \033[31mACCEPT+UAF\033[0m %-34s %s（arena 回收帧出现 %s 次）\n' "$n" "$kind" "$freed"
        fail=$((fail+1))
    else
        printf '  \033[33mACCEPT+CLEAN\033[0m %-31s ← 反例不成立了？请复核\n' "$n"
        fail=$((fail+1))
    fi
done
echo
echo "  洞还在 = $fail 条 · 已修 = $pass 条"
echo "  ⚠️ 本库的语义是反的：**通过 ≠ 好**。修好一条就把它搬到 tests/errors/ 并在这里删掉 ✓"
exit 0
