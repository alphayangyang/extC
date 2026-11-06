#!/usr/bin/env bash
# ASan 运行时**可用性探针**（被 tests/asan/run.sh 与需要跑 ASan 的套件 source）。
#
# 背景：gcc 12 及更早自带的 libasan 不认"高熵地址随机化"。内核把 `vm.mmap_rnd_bits` 提到 32
# 之后（新内核/新发行版），这类 libasan 会**偶发**在启动阶段自己 SEGV：stderr 只有一串
# `AddressSanitizer:DEADLYSIGNAL`（没有报告），进程随后挂住。表现为"每次挂的用例都不一样"。
# 已实测：同一台机器、同一个二进制，32 路并发下 9/32 次这样挂；`setarch -R`（关地址随机化）
# 之后 0/32。宿主 gcc 15 的 libasan 没有这个问题。
#
# 所以：这不是被测程序的错，测下去只会得到随机红。探针跑几遍，任何一次异常就判定"这台机器上
# ASan 运行时不可用"，调用方应当 **skip 并说原因**，而不是记成 FAIL。
#
# 用法：
#   . tests/asan-probe.sh
#   if asan_probe; then ...跑 ASan 断言...; else echo "  skip …"; fi
# 依赖：已经在仓库根目录（各套件开头都 `cd "$(dirname "$0")/../.."`）。

asan_probe() {
    _tmp=$(mktemp -d) || return 1
    printf 'int main(void) { return 0; }\n' >"$_tmp/p.c"
    if ! gcc -fwrapv -fsanitize=address -o "$_tmp/p" "$_tmp/p.c" >/dev/null 2>&1; then
        rm -rf "$_tmp"
        echo "  skip  gcc 不支持 -fsanitize=address（缺 libasan？）"
        return 1
    fi
    _runs=${EXTC_ASAN_PROBE_RUNS:-8}
    _i=0
    while [ "$_i" -lt "$_runs" ]; do
        _i=$((_i + 1))
        _out=$(ASAN_OPTIONS="detect_leaks=0" timeout 20 "$_tmp/p" 2>&1)
        _rc=$?
        if [ "$_rc" -ne 0 ] || printf '%s' "$_out" | grep -q 'AddressSanitizer'; then
            rm -rf "$_tmp"
            cat >&2 <<MSG
  skip  ASan 运行时在这台机器上不可靠（探针第 $_i/$_runs 次: rc=$_rc, $(printf '%s' "$_out" | head -1)）
        原因通常是 gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核：偶发在启动阶段自己
        SEGV（stderr 只有 DEADLYSIGNAL）。三条出路，任一即可：
          · 用 gcc>=13（或 clang>=17）跑 ASan 这一支；
          · 临场降低熵：sudo sysctl -w vm.mmap_rnd_bits=28；
          · 或关掉被测程序的地址随机化：setarch -R <被测程序>。
MSG
            return 1
        fi
    done
    rm -rf "$_tmp"
    return 0
}
