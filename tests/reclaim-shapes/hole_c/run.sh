#!/usr/bin/env bash
# tests/reclaim-shapes/hole_c/run.sh —— **C 洞的绿判据**（先赤后修；这一支现在是门禁）。
#
# 洞：清扫的静态根表是**词法作用域**的（本帧局部 + 形参 + 全局），**看不见调用栈上其他帧的
# 活引用**。内层帧给**外层区域**（家 arena，调用者一路传下来的同一只）分配 ⇒ 用内层的根表扫
# 外层的区域 ⇒ 种子不完整 ⇒ 把外层帧还持有的对象当垃圾放掉 ⇒ 真悬垂 UAF。
#
# 本目录三条形状（都在这里跑）：
#   · `hole_c.extc`      最小递归：外层调用帧的 `t` 连着已建好的子树，内层帧的触发根表是空的
#                        （`maybeCollect(区域, (void *const *)0, 0)`）⇒ 整棵放掉。
#   · `caller_frame.extc` 同一个洞的**非递归**形态：`main` 手里的链头是活引用，被调 `fresh()`
#                        只负责 `new`。修之前症状是挂住（rc=124）。
#   · 真实语料那一档（`bench/oi/persist/p3834_nodes.extc` 的最小化版）见
#                        `tests/reclaim-shapes/persist_nodes.extc` + `run-switch.sh` ①i。
#
# 修法（(a) 立刻健全，见 `src/back/codegen.c` 的「第三个安全方向」）：触发点只在**本帧拥有**的
# 区域上下手 —— 家 arena / 线程本地 arena 上一个触发都不发；本帧自己的块 arena 仍照常扫
# （外层帧拿不到指向它的引用），所以清扫能力没有被整体废掉（护栏见 `run-switch.sh` 与 README）。
#
# 开关口径（`reclaim-default` 分支起）：回收**默认打开**；`EXTC_RECLAIM=1` 强制打开，
# `EXTC_RECLAIM=0` 关掉。两档都跑、都要**输出正确**（关档那一档是"不空转"的对照：
# 它同样要生成、编译、运行、断言逐字输出）。
#
# 判据：
#   ① 开档（`EXTC_RECLAIM=1`）：`hole_c` rc=0 且输出逐字 `n = 131071`（修之前 rc=139）；
#   ② 开档 + ASan：rc=0，且报告里**没有** SEGV / heap-use-after-free；
#   ③ 关档（`EXTC_RECLAIM=0`）：rc=0 且输出逐字 `n = 131071`（对照组，不空转）；
#   ④ `caller_frame` 开档：rc=0（不挂住）且输出逐字 `n = 70000`；
#   ⑤ `caller_frame` 关档：rc=0 且输出逐字 `n = 70000`；
#   ⑥ 生效位置（生成物层面）：开档的 `hole_c` 生成物里**不发射** `extc_rc_maybeCollect`
#      —— 那两处分配都在家 arena 上，一个触发都不该发。
#   ⑦ 前提健全性：深度 15（65535 节点 = 1.0 MiB − 16 B）没过 `EXTC_RC_FLOOR`(1 MiB) ⇒ 开档
#      也不该崩、输出 `n = 65535`。这条钉住"① 的复现需要越过压力线"这个前提。
#
# 用法：bash tests/reclaim-shapes/hole_c/run.sh          exit 0 = 绿（洞不在了）
#       EXTC=/path/to/extc bash tests/reclaim-shapes/hole_c/run.sh
set -u
cd "$(dirname "$0")/../../.."          # 仓库根

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
SRC=tests/reclaim-shapes/hole_c/hole_c.extc
CF=tests/reclaim-shapes/hole_c/caller_frame.extc
WANT="n = 131071"                      # 2^17 - 1 个节点（深度 16 的满二叉树）
CF_WANT="n = 70000"
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

echo "== hole_c：清扫不得在种子不完整时下手（本帧拥有的区域才触发）=="
echo "   形状 $SRC / $CF；期望 开档=绿（修之前 rc=139 / 挂住）/ 关档=绿"

# ── ① 开档：非 ASan ──────────────────────────────────────────────────────────
if gen 开 on "$SRC" && cc_build on && gen_ok on; then
    run on; on_rc=$R_RC; on_out=$R_OUT
    if [ "$on_rc" = 0 ] && [ "$on_out" = "$WANT" ]; then
        ok "开档（EXTC_RECLAIM=1）：rc=0 out='$on_out' —— 外层帧的活引用没被放掉"
    else
        bad "开档：rc=$on_rc out='$on_out'（期望 rc=0 / '$WANT'）—— 提前释放仍在"
    fi
else
    bad "开档：生成或编译失败（或生成物里没有回收运行期）"
    sed 's/^/       /' "$TMP/on.gen.log" "$TMP/on.cc.log" 2>/dev/null | head -6
fi

# ── ② 开档 + ASan ───────────────────────────────────────────────────────────
if gen 开 ona "$SRC" && cc_build ona asan; then
    run ona asan; asan_rc=$R_RC
    if [ "$asan_rc" = 0 ] && ! grep -qE 'SEGV|heap-use-after-free|DEADLYSIGNAL|AddressSanitizer: (stack|global)' "$TMP/ona.err"; then
        ok "开档（ASan）：rc=0，无 SEGV / UAF 报告"
    else
        bad "开档（ASan）：rc=$asan_rc，报告里有 SEGV/UAF —— 悬垂仍在"
        grep -m2 -E 'SEGV on unknown address|heap-use-after-free|SUMMARY: AddressSanitizer' "$TMP/ona.err" \
            | sed 's/^/       /'
    fi
else
    bad "开档（ASan）：生成或编译失败"
fi

# ── ③ 关档：对照组必须绿（不空转：同样生成/编译/运行/逐字比输出） ─────────────
if gen 关 off "$SRC" && cc_build off; then
    run off; off_rc=$R_RC; off_out=$R_OUT
    if [ "$off_rc" = 0 ] && [ "$off_out" = "$WANT" ]; then
        ok "关档（EXTC_RECLAIM=0）：rc=0 out='$off_out' —— 对照组绿（判据不空转）"
    else
        bad "关档：rc=$off_rc out='$off_out'（期望 rc=0 / '$WANT'）"
    fi
else
    bad "关档：生成或编译失败"
fi

# ── ④⑤ caller_frame：非递归形态（修之前挂住 rc=124） ────────────────────────
if gen 开 cf "$CF" && cc_build cf; then
    timeout 20 sh -c '"$0" >"$1" 2>"$2"' "$TMP/cf" "$TMP/cf.out" "$TMP/cf.err" 2>/dev/null
    cf_rc=$?; cf_out=$(cat "$TMP/cf.out")
    if [ "$cf_rc" = 0 ] && [ "$cf_out" = "$CF_WANT" ]; then
        ok "caller_frame 开档：rc=0 out='$cf_out' —— 主调帧的活引用没被放掉（修之前 rc=124 挂住）"
    else
        bad "caller_frame 开档：rc=$cf_rc out='$cf_out'（期望 rc=0 / '$CF_WANT'）"
    fi
else
    bad "caller_frame 开档：生成或编译失败"
fi

if gen 关 cfo "$CF" && cc_build cfo; then
    timeout 20 sh -c '"$0" >"$1" 2>"$2"' "$TMP/cfo" "$TMP/cfo.out" "$TMP/cfo.err" 2>/dev/null
    cfo_rc=$?; cfo_out=$(cat "$TMP/cfo.out")
    if [ "$cfo_rc" = 0 ] && [ "$cfo_out" = "$CF_WANT" ]; then
        ok "caller_frame 关档：rc=0 out='$cfo_out' —— 对照组绿（判据不空转）"
    else
        bad "caller_frame 关档：rc=$cfo_rc out='$cfo_out'（期望 rc=0 / '$CF_WANT'）"
    fi
else
    bad "caller_frame 关档：生成或编译失败"
fi

# ── ⑥ 生效位置：开档生成物里一个触发都不发（两处分配都在家 arena 上） ────────
if [ -f "$TMP/on.c" ]; then
    ntrig=$(grep -c 'extc_rc_maybeCollect(__extc_rcg' "$TMP/on.c" 2>/dev/null || true)
    if [ "${ntrig:-1}" = 0 ]; then
        ok "生成物：开档的 hole_c 不发射清扫触发（分配点全在家 arena 上 ⇒ 不扫外层区域）"
    else
        bad "生成物：开档的 hole_c 仍发射了 $ntrig 处清扫触发 —— 家 arena 上的触发没被挡住"
        grep -o 'extc_rc_regionOf([^,]*, [-0-9]*)' "$TMP/on.c" | sort -u | sed 's/^/       /' | head -4
    fi
else
    bad "生成物：没有开档生成物可查"
fi

# ── ⑦ 前提健全性：深度 15 没过 1 MiB 压力下限 ⇒ 开档也不该崩 ─────────────────
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

echo
echo "判据：通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
