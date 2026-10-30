#!/usr/bin/env bash
# tests/fuse/run.sh —— **S1 元素链融合的常设验收**（第 2 层第一个形状）
#
# 判据（每一条都能红）：
#   ① 正例：融合开/关两种编译下输出逐字节相同，且与期望行一致；
#   ② 正例：融合后的生成 C 里**没有**那个临时视图（融合真的发生了），关掉时它还在；
#   ③ 反例（tmp 复用它处 / 倒序下标 / 右值有调用 / 读写同一视图）：不融合（临时视图仍在）且输出正确。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
tmp=$(mktemp -d)
fail=0

bad() { echo "  FAIL $1"; fail=1; }
ok()  { echo "  ok   $1"; }

# expect_run <名字> <源文件> <期望输出> [额外 extc 参数…]
expect_run() {
    local name=$1 f=$2 want=$3; shift 3
    local out
    if ! out=$("$EXTC" -w --run "$@" "$f" 2>&1); then bad "$name：编译/运行失败"; echo "$out" | head -4 | sed 's/^/        /'; return 1; fi
    if [ "$out" = "$want" ]; then ok "$name -> $out"; else bad "$name：期望「$want」，得到「$out」"; return 1; fi
}

# gen_c <源文件> <输出 C> [额外参数…]
gen_c() {
    local f=$1 o=$2; shift 2
    "$EXTC" -w --no-line-map "$@" -o "$o" "$f" || { bad "生成 C 失败：$f"; return 1; }
}

echo "== 正例：相邻的写/读循环融合，临时视图消失 =="
expect_run fuse/pipeline tests/fuse/pipeline.extc "s=116.0"
expect_run fuse/pipeline-nofuse tests/fuse/pipeline.extc "s=116.0" --no-fuse
gen_c tests/fuse/pipeline.extc "$tmp/fused.c" || true
gen_c tests/fuse/pipeline.extc "$tmp/plain.c" --no-fuse || true
if grep -q "slice_f64 tmpv" "$tmp/fused.c"; then bad "融合后生成 C 里还有 tmpv"; else ok "融合后 tmpv 已消失"; fi
if grep -q "slice_f64 tmpv" "$tmp/plain.c"; then ok "--no-fuse 保留了 tmpv"; else bad "--no-fuse 把 tmpv 也删了"; fi

echo "== 反例：不满足资格时不许融合（临时视图仍在）且输出正确 =="
check_neg() {   # check_neg <名字> <文件> <期望输出>
    local name=$1 f=$2 want=$3
    expect_run "$name" "$f" "$want" || return
    gen_c "$f" "$tmp/neg.c" || return
    if grep -q "slice_f64 tmpv" "$tmp/neg.c"; then ok "$name 未融合"; else bad "$name 被错误融合"; fi
}
check_neg fuse/neg_reuse    tests/fuse/neg_reuse.extc    "out0=5.0 extra=42.0"
check_neg fuse/neg_index    tests/fuse/neg_index.extc    "out0=7.0"
check_neg fuse/neg_call     tests/fuse/neg_call.extc     "out3=9.0"
check_neg fuse/neg_readwrite tests/fuse/neg_readwrite.extc "out0=0.0 out3=6.0"

rm -rf "$tmp"
[ "$fail" -eq 0 ]
