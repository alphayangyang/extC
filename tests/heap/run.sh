#!/usr/bin/env bash
# tests/heap/run.sh —— **板（Heap）的常设验收**（HEAP.md；C-ABI.md §9.10）
#
# 判据：
#   ① 保留 4 GiB **不占物理页**：`/proc/self/statm` 的 resident 差要远小于 4096 KB（实测 4 KB）；
#   ② commit 按 CHUNK 走（1 MiB），不是整块保留区；
#   ③ **那道门有牙**：板外指针 `holds` 为假；`copyIn` 的目标不在板内 ⇒ -1；
#   ④ 不搬家：第二笔分配之后第一笔的视图仍然可写；
#   ⑤ **close 之后再用 = 硬 SIGSEGV**（退出码 139），而且崩溃前那行标记真的打出来了。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

echo "== ① 保留便宜 · ② 按需 commit · ③ 门有牙 · ④ 不搬家 =="
if out=$("$EXTC" --run tests/heap/plate.extc 2>&1); then
    # `// expect:` 只列**稳定**的部分；两个数字判据在下面单独量（阈值才是判据，不是那几个数字本身）。
    want=$(grep -o '// expect:.*' tests/heap/plate.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS=' ' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }; done
    # 数字判据（不是看一眼就过）：保留 4 GiB 而 RSS 差 < 4 MB；commit 粒度 ≤ 4 MiB
    rss=$(echo "$out" | grep -o 'rss_delta=[0-9]*' | cut -d= -f2)
    com=$(echo "$out" | grep -o 'commit=[0-9]*' | cut -d= -f2)
    if [ "${rss:-999999}" -ge 4096 ]; then echo "  FAIL 保留 4 GiB 却让 RSS 涨了 ${rss} KB（应远小于 4096）"; ok=0; fi
    if [ "${com:-999999}" -gt 4096 ]; then echo "  FAIL commit 粒度 ${com} KB（应 ≤ CHUNK = 1 MiB）"; ok=0; fi
    if [ "$ok" = 1 ]; then echo "  ok   plate  ->  $(echo "$out" | tr '\n' '|')（RSS 只涨 ${rss} KB / 保留 4 GiB · commit ${com} KB）"
    else echo "  FAIL plate  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL plate  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== ⑤ close 之后再用 = 硬缺页（退出码 139 = 128 + SIGSEGV）=="
# `--run` 只报驱动程序自己的退出码（实测 255），拿不到信号；所以这里**编出二进制直接跑**。
if "$EXTC" -w --no-line-map -o "$tmp/ac.c" tests/heap/afterclose.extc >/dev/null 2>&1 &&
   gcc -std=c11 -fwrapv -o "$tmp/ac" "$tmp/ac.c" 2>"$tmp/err"; then
    set +e
    out=$("$tmp/ac" 2>&1)
    rc=$?
    set -e
    if [ "$rc" = 139 ]; then
        if echo "$out" | grep -qF "closed"; then
            echo "  ok   afterclose  ->  close 之后写板内指针 ⇒ SIGSEGV（rc=139，崩溃前的标记打出来了）"
        else
            echo "  FAIL afterclose  ->  退出码对（139），但崩溃前的标记没打出来（分不清崩在哪一步）"; fail=1
        fi
    else
        echo "  FAIL afterclose  ->  期望 rc=139（硬缺页），实得 rc=$rc"; echo "$out" | sed 's/^/        /' | head -4; fail=1
    fi
else
    echo "  FAIL afterclose  ->  编不出来"; head -3 "$tmp/err" 2>/dev/null | sed 's/^/        /'; fail=1
fi

echo "== ⑥ 只读借出（mprotect PROT_READ）：借出期间只能读，写 ⇒ 硬缺页 =="
if out=$("$EXTC" --run tests/heap/loan.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/heap/loan.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS=' ' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }; done
    if [ "$ok" = 1 ]; then echo "  ok   loan  ->  $(echo "$out" | tr '\n' '|')（写 → 借只读 → 读得到 → 还回写权限 → 再写）"
    else echo "  FAIL loan  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL loan  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -5; fail=1
fi

echo "== ⑥ 只读借出的牙：借出期间写入 = 硬缺页（退出码 139）=="
if "$EXTC" -w --no-line-map -o "$tmp/lw.c" tests/heap/loanwrite.extc >/dev/null 2>&1 &&
   gcc -std=c11 -fwrapv -o "$tmp/lw" "$tmp/lw.c" 2>"$tmp/errlw"; then
    set +e
    out=$("$tmp/lw" 2>&1)
    rc=$?
    set -e
    if [ "$rc" = 139 ] && echo "$out" | grep -qF "loaned"; then
        echo "  ok   loanwrite  ->  只读借出后写入 ⇒ SIGSEGV（rc=139，崩溃前的标记可见）"
    else
        echo "  FAIL loanwrite  ->  期望 rc=139 且标记可见，实得 rc=$rc"; fail=1
    fi
else
    echo "  FAIL loanwrite  ->  编不出来"; head -3 "$tmp/errlw" 2>/dev/null | sed 's/^/        /'; fail=1
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
