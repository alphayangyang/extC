#!/usr/bin/env bash
# extC vs C 性能对比（主人 2026-09-20）。
# 公平起见两边用**同一套旗子**（extC 的驱动用的是 -std=c11 -O1 -fwrapv），
# 另外加一组 -O2 做参考 —— 看清楚"差距有多少来自语言、多少来自优化级别"。
set -u
cd "$(dirname "$0")"
EXTC=../build/extc
CC="gcc -std=c11 -O1 -fwrapv"

for f in sieve matmul fib; do $EXTC -o $f.extc.c $f.extc; done
for f in sieve matmul fib; do
    gcc -std=c11 -O1 -fwrapv -o ${f}_extc  $f.extc.c -w
    gcc -std=c11 -O1 -fwrapv -o ${f}_c     $f.c
    gcc -std=c11 -O2         -o ${f}_extc_o2 $f.extc.c -w
    gcc -std=c11 -O2         -o ${f}_c_o2  $f.c
done
gcc -std=c11 -O1 -fwrapv -o sieve_chk  sieve_chk.c
gcc -std=c11 -O1 -fwrapv -o matmul_chk matmul_chk.c

bench() { local best=9999999 ms; for i in 1 2 3; do
    local t0=$(date +%s%N); "$@" >/dev/null 2>&1; local t1=$(date +%s%N)
    ms=$(( (t1-t0)/1000000 )); [ $ms -lt $best ] && best=$ms; done; echo $best; }

printf "%-24s %8s %8s %8s %8s %8s\n" "程序" "extC" "C:O1" "C:O1+检查" "extC:O2" "C:O2"
for f in sieve matmul; do
    printf "%-24s %8s %8s %8s %8s %8s\n" "$f" \
      "$(bench ./${f}_extc)" "$(bench ./${f}_c)" "$(bench ./${f}_chk)" \
      "$(bench ./${f}_extc_o2)" "$(bench ./${f}_c_o2)"
done
printf "%-24s %8s %8s %8s %8s %8s\n" "fib(32)" \
  "$(bench ./fib_extc)" "$(bench ./fib_c)" "-" "$(bench ./fib_extc_o2)" "$(bench ./fib_c_o2)"
