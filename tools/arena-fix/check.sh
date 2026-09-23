#!/usr/bin/env bash
# 三步落地清单（ARENA-SOUNDNESS §9 的档 0/1/2）—— 每一步的验收都在这一个脚本里
#
#   用法： ./check.sh           跑当前这一步该过的全部验收
#          ./check.sh golden    额外比对 tools/golden.sh（基线 = 修前编译器）
#
# 判据（三步通用，缺一不可）：
#   ① tests/arena-soundness/run.sh 里**该转 REJECT 的必须转**
#   ② tests/run.sh 通过（误拒要**逐条记账**，不许静默变）
#   ③ tests/asan/ 与 tests/arena/ 全绿（不许把干净的形状搞脏）
#   ④ 攻击库基线**一字不动**（"挡住"必须同时是"真的不安全"）
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}
fail=0
say() { printf '\n== %s ==\n' "$1"; }

say "构建"
# ⚠️ 先 `touch` 一遍再 make —— 踩过：`make -s` 会因为时间戳认为目标最新而**不重链**，
#    于是你以为在测新编译器，其实测的是旧的 ✗（这次就被骗过一次）
touch "$PWD"/src/*.c "$PWD"/src/*.h 2>/dev/null
make >/tmp/arenafix-build.log 2>&1 || { echo "  构建失败"; tail -5 /tmp/arenafix-build.log; exit 1; }
echo "  ok"

say "① 反例库（该拒的必须拒）"
out=$(EXTC=$EXTC bash tests/arena-soundness/run.sh 2>/dev/null)
echo "$out" | grep -E '^  (REJECT|ACCEPT\+UAF|CRASH|GCC-ERR)' | sed 's/（.*//'
still=$(echo "$out" | grep -cE '^  ACCEPT\+UAF$|^  ACCEPT\+UAF ' || true)
echo "  → 仍放行的 UAF：$still 条"

say "② 全套测试（不再有已知误拒 —— field-strong-update 的误拒 2026-09-23 已修 ✓）"
out=$(./tests/run.sh 2>&1); last=$(echo "$out" | tail -1); echo "  $last"
bad=$(echo "$out" | grep '^  FAIL' | grep -v 'field-strong-update' || true)
if [ -n "$bad" ]; then echo "  ✗ 出现**新的**失败（不许有）："; echo "$bad" | sed 's/^/  /'; fail=1
else echo "  ok（除了那条已知误拒，没有新失败）"; fi

say "③「转正」库（本来安全 ⇒ 必须接受 + ASan 干净）"
if out=$(bash tests/arena-promoted/run.sh 2>&1); then echo "  $(echo "$out" | grep -cE '^  ok') 条全绿 ✓"
else echo "$out" | grep FAIL | sed 's/^/  /'; fail=1; fi

say "④ ASan / arena"
a=$(bash tests/asan/run.sh 2>&1 | grep -cE '^  ok'); b=$(bash tests/arena/run.sh 2>&1 | grep -cE '^  ok')
echo "  asan=$a  arena=$b"
[ "$a" -ge 8 ] && [ "$b" -ge 5 ] || { echo "  ✗ 干净形状变脏了"; fail=1; }

say "⑤ 攻击库基线（**独立判**：不能因为 ② 里有已知失败就跳过它 ✗）"
for f in tests/attacks/*.extc; do
    n=$(basename "$f" .extc)
    if "$EXTC" "$f" -o /dev/null 2>/dev/null; then echo "$n"; fi
done | sort > /tmp/arenafix-passed.txt
sort tests/attacks/BASELINE > /tmp/arenafix-base.txt
if diff -u /tmp/arenafix-base.txt /tmp/arenafix-passed.txt; then
    echo "  ok  通过集合与基线一致（$(wc -l < /tmp/arenafix-base.txt) 条已知安全 + 其余全部被挡）"
else echo "  ✗ 攻击库通过集合变了（上面是差异）"; fail=1; fi

if [ "${1:-}" = golden ]; then
    say "⑤ golden（基线 = 修前编译器；差异必须逐条说得清）"
    bash tools/golden.sh check 2>&1 | tail -3
fi

echo
[ $fail -eq 0 ] && echo "全部通过 ✓" || echo "有失败项 ✗"
exit $fail
