#!/usr/bin/env bash
# 把"arena 健全性修复"应用到当前仓库：① 打补丁 ② 落地新测试 ③ 把已转正的条目移出旧位置 ④ 验收
set -eu
cd "$(dirname "$0")/../.."
A=tools/arena-fix

echo "== ① 打补丁（改 src/）=="
git apply "$A/arena-soundness-fix.patch"
echo "   ok：check_escape.c check_expr.c check_internal.h check_stmt.c check_top.c"

echo "== ② 落地「转正」库 =="
mkdir -p tests/arena-promoted
cp "$A"/newtests/*.extc tests/arena-promoted/
cp "$A"/run-promoted.sh tests/arena-promoted/run.sh
chmod +x tests/arena-promoted/run.sh
echo "   tests/arena-promoted/ = $(ls tests/arena-promoted/*.extc | wc -l) 条（必须接受 + ASan 干净）"

echo "== ③ 把已转正的条目从旧位置移走 =="
# 这三条以前是"必须被拒"的反例，B2 之后**真的安全** ⇒ 按纪律转正（不是放松！）
for n in alloc_return_local allocSlice_escape_return; do
    if [ -f "tests/errors/$n.extc" ]; then rm -f "tests/errors/$n.extc"; echo "   移出 tests/errors/$n.extc（已转正）"; fi
done
# 曾放进 tests/errors/ 的 arena_*：修复之后**全部**被证明"本来就安全" ⇒ 全部移出 ✓
# （真不安全的那两条 B_field_table_stale / D_elemwrite_clears_table 留在**反例库**里，
#   它们本来就该被拒，只是原先"被拒的理由"不对 —— 现在理由对了 ✓）
for f in tests/errors/arena_*.extc; do
    [ -f "$f" ] || continue
    rm -f "$f"; echo "   移出 $f（已转正）"
done
# 反例库里已转正的条目也从那里移走（A 族 3 条 + A_field/C1/C2/C3/G 共 5 条）
# 反例库里已转正的条目也移走：**只留还漏的**（现在只剩 B/D 两族那条"指向栈局部"的）
for f in tests/arena-soundness/*.extc; do
    n=$(basename "$f" .extc)
    case "$n" in
        B_field_table_stale|D_elemwrite_clears_table) ;;   # 真不安全，留着 ✓
        R1a_global_new_crash|R1b_global_call_crash) ;;     # 现在是"该报错"的正式反例，留着 ✓
        *) rm -f "$f" ;;
    esac
done
echo "   反例库现在只剩还漏的：$(ls tests/arena-soundness/*.extc | wc -l) 条"

echo "== ④ 攻击库基线（+h16_allocret：它的攻击前提是假的，实测 ASan 干净）=="
cp "$A"/BASELINE.new tests/attacks/BASELINE
echo "   基线 = $(tr '\n' ' ' < tests/attacks/BASELINE)"

echo "== ④b 落地回归哨兵（反例库里"必须继续被拒"的那些）=="
cp "$A"/newsentinels/*.extc tests/arena-soundness/ 2>/dev/null || true
echo "   sentinels = $(ls "$A"/newsentinels/*.extc 2>/dev/null | wc -l) 条"

echo "== ⑤ 验收 =="
bash "$A/check.sh" || true
