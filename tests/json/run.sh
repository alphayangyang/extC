#!/usr/bin/env bash
# tests/json/run.sh —— JSON 判据：与 Python 的规范形对拍 · 坏输入必须拒绝 · 零拷贝证据 · 吞吐与内存。
set -u
cd "$(dirname "$0")/../.."
EXTC="./build/extc"; STD="$PWD/stdlib"
fail=0; pass=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { pass=$((pass+1)); printf '  ok   %-16s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-16s %s\n' "$1" "$2"; }
run() { "$EXTC" -w -I "$STD" --run "tests/json/$1.extc" 2>&1; }

echo "== 解析 → 重写：与 Python 的规范形逐字节对拍 =="
python3 - >"$tmp/want" <<'PY'
import json
docs = {
 "a": '{"content":"hi\\nthere","msg_seq":3,"xs":[1,2]}',
 "b": '{"d":{"author":{"member_openid":"ABC"},"content":"hello"},"t":true,"n":null,"e":[]}',
 "c": '{"unicode":"\\u4f60\\u597d\\u4e16\\u754c","pair":"\\ud83d\\ude00","q":"a\\"b\\\\c"}',
 "d": '[[1,2],[3,[4]]]',
 "e": '{"big":123456789012345,"neg":-42,"z":0}',
}
for k, s in docs.items():
    print(k, json.dumps(json.loads(s), ensure_ascii=False, separators=(',', ':')))
print("roundtrip ok")
PY
if out=$(run roundtrip) && diff <(echo "$out" | grep -v '^$') "$tmp/want" >"$tmp/d1" 2>&1; then
    ok roundtrip "5 份文档（含中文、代理对 😀、转义引号/反斜杠、空容器、null、大整数）逐字节一致"
else
    bad roundtrip "$(head -4 "$tmp/d1" | tr '\n' '|')"
fi

echo "== 坏输入必须拒绝（不是崩、不是静默接受）=="
if out=$(run bad) && echo "$out" | grep -q 'bad ok'; then
    ok bad-inputs "17 条：截断 · 缺括号/冒号/逗号 · 尾随垃圾 · 非法转义 · 裸控制字符 · 坏 \\u · 孤立代理（解码失败）· 前导零 · 池子不够 · 400 层嵌套"
else
    bad bad-inputs "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== 零拷贝的证据（改输入 ⇒ 视图跟着变）=="
if out=$(run zerocopy) && echo "$out" | grep -q 'zerocopy ok'; then
    ok zero-copy "字符串是输入上的视图，不是副本（这就是'内存少'的机制本身）"
else
    bad zero-copy "$(echo "$out" | tail -2 | tr '\n' '|')"
fi

echo "== 吞吐与内存 =="
if "$EXTC" -w -I "$STD" --build -o "$tmp/jbench" tests/json/bench.extc >/dev/null 2>&1; then
    if out=$(/usr/bin/time -v "$tmp/jbench" 2>"$tmp/tv") && echo "$out" | grep -q 'bench ok'; then
        rss=$(grep -o 'Maximum resident set size (kbytes): [0-9]*' "$tmp/tv" | grep -o '[0-9]*$')
        mbps=$(echo "$out" | grep -o 'parse [0-9]* MB/s' | grep -o '[0-9]*')
        if [ "${rss:-999999}" -lt 8192 ]; then ok memory "峰值 RSS ${rss} KB（零分配：节点池与缓冲都在栈上）"
        else bad memory "峰值 RSS ${rss} KB ≥ 8192 KB"; fi
        if [ "${mbps:-0}" -ge 200 ]; then ok throughput "$(echo "$out" | grep -E 'parse|build' | tr '\n' ' ' | sed 's/  */ /g')"
        else bad throughput "解析只有 ${mbps} MB/s（退化到逐字节检查了？）"; fi
    else
        bad bench "$(echo "$out" | tail -2 | tr '\n' '|')"
    fi
else
    bad bench "编不出来"
fi

printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
