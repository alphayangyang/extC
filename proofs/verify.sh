#!/usr/bin/env bash
# verify.sh —— 复核 proofs/ 下的机械化证明（Lean + Z3 两条独立路径）。
#
#   1. 编译三份 Lean 文件（有错即失败）
#   2. 剥掉注释后扫描 `sorry` / `admit` / 自定义 `axiom`
#   3. 检查公理依赖（不得出现 sorryAx）
#   4. 跑 Z3 小规模交叉复核
#
# 用法：bash proofs/verify.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [ -x "$HOME/.elan/bin/lean" ]; then
  LEAN="$HOME/.elan/bin/lean"
elif command -v lean >/dev/null 2>&1; then
  LEAN="$(command -v lean)"
else
  echo "没找到 lean。安装：curl -sSfL https://elan.lean-lang.org/elan-init.sh | sh -s -- -y"
  exit 2
fi

fail=0
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

echo "== 1/7 编译 Lean =="
if ! "$LEAN" -o ExtCRegion.olean ExtCRegion.lean >>"$OUT" 2>&1; then
  echo "  FAIL ExtCRegion.lean"; sed 's/^/       /' "$OUT"; exit 1
fi
echo "  OK   ExtCRegion.lean"
for f in ExtCPool.lean ExtCModel.lean ExtCPrecision.lean ExtCPath.lean ExtCGap.lean ExtCMemGap.lean; do
  if LEAN_PATH=. "$LEAN" "$f" >>"$OUT" 2>&1; then
    echo "  OK   $f"
  else
    echo "  FAIL $f"; sed 's/^/       /' "$OUT"; exit 1
  fi
done

for f in ExtCMechanism.lean ExtCArenaPool.lean; do
  if LEAN_PATH=. "$LEAN" "$f" >>"$OUT" 2>&1; then
    echo "  OK   $f"
  else
    echo "  FAIL $f"; sed 's/^/       /' "$OUT"; exit 1
  fi
done

echo "== 2/7 扫描未完成的证明 =="
if python3 sorry_scan.py; then
  echo "  OK   没有 sorry / admit / 自定义 axiom"
else
  echo "  FAIL 存在未完成的证明"; fail=1
fi

echo "== 3/7 公理依赖 =="
if grep -q "sorryAx" "$OUT"; then
  echo "  FAIL 有定理依赖 sorryAx"; fail=1
else
  echo "  OK   没有任何定理依赖 sorryAx"
fi
grep "depends on axioms" "$OUT" | sed "s/^'/       /" || echo "       （全部定理不依赖任何公理）"

echo "== 4/7 Z3 交叉复核 =="
Z3PATH=""
[ -d /tmp/z3lib ] && Z3PATH=/tmp/z3lib
if PYTHONPATH="$Z3PATH" python3 crosscheck_z3.py; then
  echo "  OK   SMT 交叉复核一致"
else
  echo "  FAIL SMT 交叉复核不一致（或缺 z3：pip install z3-solver）"; fail=1
fi

echo "== 5/7 路径敏感：接受集是否改变 =="
if python3 pathsensitivity.py >/dev/null 2>&1; then
  echo "  OK   路径敏感与合流判定完全一致（检查侧无增益）"
else
  echo "  FAIL 两者判定不一致"; fail=1
fi

echo "== 6/7 到类上限的精度距离 =="
if python3 precision_gap.py >/dev/null 2>&1; then
  echo "  OK   三层嵌套成立（Inferred ⊆ Best ⊆ Opt），距离已算出"
else
  echo "  FAIL 嵌套性被破坏"; fail=1
fi

echo "== 7/7 内存精度差距分类 =="
if python3 memory_gap.py >/dev/null 2>&1; then
  echo "  OK   内存差距类别已算出（阈值定律：超常数 ⟺ 逃逸轮数 o(n)）"
else
  echo "  FAIL 分类脚本出错"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then echo "全部通过。"; else echo "有失败项。"; fi
exit "$fail"
