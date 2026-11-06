#!/usr/bin/env bash
# arena 健全性**反例库**（2026-09-23）—— 配套 `docs/topics/ARENA-SOUNDNESS.md`
#
# 每一条都是**当前编译器会接受、运行时真的悬垂**的程序 ⇒ 这是 soundness 的证伪证据，
# 所以**不接进 `tests/run.sh`**（那里是"必须被拒/必须跑对"的回归）。
# 用法： `./run.sh`      —— 逐条编译 + ASan 跑，打印判定
#        `./run.sh A`   —— 只跑文件名以 A 打头的那条
#
# ⚠️ 判据：`ACCEPT + ASan UAF` = 反例成立（洞还在）；
#          `REJECT`               = 洞已被修好（这条就该从这里毕业、搬进 tests/errors/）✓
set -u
# LeakSanitizer 对这套内存模型没有意义（arena 到进程结束才释放），它在最小发行版里还会偶尔
# 在退出扫描时挂住 ⇒ 这里只抓越界与 use-after-free。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
# gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核会**偶发**在启动阶段自己 SEGV
# （stderr 只有 DEADLYSIGNAL，然后挂住）；实测 32 路并发 9/32 次，关掉地址随机化后 0/32。
# 原因与三条出路见 tests/asan-probe.sh。这里只对本套件关随机化。
if [ -z "${EXTC_ASAN_NO_SETARCH:-}" ] && command -v setarch >/dev/null 2>&1; then
    EXTC_ASAN_NO_SETARCH=1 exec setarch "$(uname -m)" -R bash "$0" "$@"
fi
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
    if ! gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$n" "$TMP/$n.c" >"$TMP/$n.gerr" 2>&1; then
        printf '  \033[33mGCC-ERR\033[0m %-34s %s\n' "$n" "$(head -1 "$TMP/$n.gerr" | cut -c1-60)"
        fail=$((fail+1)); continue
    fi
    out=$(timeout 20 "$TMP/$n" 2>&1); rc=$?
    kind=$(printf '%s' "$out" | grep -m1 -o 'ERROR: AddressSanitizer: [a-z-]*' || true)
    if [ -n "$kind" ]; then
        freed=$(printf '%s' "$out" | grep -c 'extc_arena_release' || true)
        printf '  \033[31mACCEPT+UAF\033[0m %-34s %s（arena 回收帧出现 %s 次）\n' "$n" "$kind" "$freed"
        fail=$((fail+1))
    elif printf '%s' "$out" | grep -q 'SILENT-WRONG'; then
        # 编过、跑完、ASan 干净，但结果被**静默改坏** ⇒ 同样是洞，而且更阴（没有崩溃可查）
        printf '  \033[31mACCEPT+WRONG\033[0m %-32s 静默改坏结果（不 trap、ASan 不报，rc=%s）\n' "$n" "$rc"
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
