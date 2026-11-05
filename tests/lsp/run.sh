#!/usr/bin/env bash
# tests/lsp/run.sh —— 语言服务器（`tools/extc-lsp.py`）的判据。
#
# 编辑器是这套语言最缺的一环：没有 LSP，写 extC 就是纯文本。服务器本身不实现任何语言
# 规则，它只把编译器的三种查询模式接成 JSON-RPC：
#
#   ① `initialize` 声明的能力与客户端预期一致（同步方式 · outline · hover · definition · 补全）；
#   ② 干净文件 → `publishDiagnostics` 空；坏了以后 → 一条诊断，位置与报文对得上；
#   ③ `documentSymbol` 给出这个文件自己的声明（`fn` · `struct`），不含预置模块；
#   ④ `hover`/`definition` 落到符号上；`completion` 至少包含本文件符号与关键字；
#   ⑤ `didClose` 之后诊断被清空（否则编辑器会一直挂着一个不存在的告警）。
#
# 判据只跑一次 JSON-RPC 会话（`tests/lsp/drive.py`），用真实编译器；不写盘：缓冲区走
# `--source-stdin`，这条路径本身也是被测的一部分（编辑器永远在未保存状态下询问）。
set -u
cd "$(dirname "$0")/../.."
EXTC="$PWD/build/extc"
if [ ! -x "$EXTC" ]; then echo "  FAIL 没有 $EXTC（先 make）"; exit 1; fi
export EXTC
fail=0; pass=0
ok()  { pass=$((pass+1)); printf '  ok   %-24s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-24s %s\n' "$1" "$2"; }

# The VS Code client translates Windows paths to WSL ones; that logic is pure and is tested
# without an editor (skipped when node is not installed).
if command -v node >/dev/null 2>&1; then
    if node tests/lsp/paths.test.js >/dev/null 2>&1; then
        pass=$((pass+1)); printf '  ok   %-24s %s\n' "paths" "Windows↔WSL 路径翻译"
    else
        fail=$((fail+1)); printf '  FAIL %-24s %s\n' "paths" "路径翻译断言不成立"
        node tests/lsp/paths.test.js 2>&1 | sed 's/^/       /'
    fi
fi

out=$(python3 tests/lsp/drive.py 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
    bad "drive.py" "客户端脚本失败（rc=$rc）"
    printf '%s\n' "$out" | sed 's/^/       /'
    printf '\n通过 %d，失败 %d\n' "$pass" "$fail"
    exit 1
fi

check() {                        # check <判据名> <python 表达式（在结果里求值）>
    local name="$1" expr="$2"
    if printf '%s' "$out" | python3 -c "
import json, sys
res = json.load(sys.stdin)
ok = all($expr for _ in [0])
sys.exit(0 if ok else 1)
" 2>/dev/null; then
        ok "$name" ""
    else
        bad "$name" "表达式不成立：$expr"
        printf '%s\n' "$out" | sed 's/^/       /'
    fi
}

check "capabilities"   'res["capabilities"]["documentSymbolProvider"] and res["capabilities"]["hoverProvider"] and res["capabilities"]["definitionProvider"] and res["capabilities"]["textDocumentSync"] == 1'
check "clean-empty"    'res["clean"] == []'
check "broken-diag"    'len(res["broken"]) == 1 and "no field" in res["broken"][0]["message"] and "zz" in res["broken"][0]["message"] and res["broken"][0]["line"] == 9 and res["broken"][0]["severity"] == 1'
check "note-joined"    '"fields of point" in res["broken"][0]["message"]'
check "symbols"        '"main" in res["symbols"] and "area" in res["symbols"] and "point" in res["symbols"]'
check "symbols-scope"  'all(s in res["symbols"] for s in ["main", "point"]) and "println" not in res["symbols"]'
check "hover-fn"       'res["hover"] is not None and "fn area" in res["hover"]'
check "definition"     'res["definition"] is not None and res["definition"][1] == 8 and res["definition"][0].endswith("sample.extc")'
check "completion"     '"main" in res["completion"] and "fn" in res["completion"]'
check "closed-cleared" 'res["closed"] == []'

printf '\n通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
