#!/usr/bin/env bash
# tests/pool/run.sh —— **期 0（slot map / ECS 底座）的常设验收**（docs/topics/REGIONS.md）
#
# 判据：
#   ① 正例：`insert`/`get`/`set`/`remove`/`len` + dense↔handle 双向 + `toSlice` ✓
#   ② 失效：`remove` 之后旧 handle **失效**（世代 +1 ⇒ contains=false ✓）
#   ③ **churn 不涨**：1e6 轮的峰值 RSS 必须与 1e5 轮**相当**（可见的判据）✓
#   ④ **判据有牙**：同一个形状**不 remove** ⇒ 必须明显涨（canary：9.9MB → 67MB ✓）
#   ⑤ 复用容量：`clear()` 之后 len=0、再插不重新分配 ✓
#   ⑥ ASan 干净 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

# 跑一个 `// expect: a|b|c` 形状的用例
run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/pool/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/pool/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

echo "== 正例：稳定 handle · dense 连续 · 世代失效 · API 面 =="
run_case basic
run_case api
run_case gather

echo "== churn：1e6 轮的峰值 RSS 必须与 1e5 轮相当（还槽位 ⇒ 平 ✓）=="
build_one() {   # 名字 文件
    "$EXTC" "tests/pool/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
      && "$CC" -O1 -std=c11 "build/$1.c" -o "build/$1" >/dev/null 2>&1
}
peak() {        # 程序 轮数 -> 峰值 RSS(KB)
    /usr/bin/time -f %M "./build/$1" "$2" 2>&1 >/dev/null | tail -1
}
if build_one pool-churn churn && build_one pool-leak churn-leak; then
    s=$(peak pool-churn 100000);  b=$(peak pool-churn 1000000)
    if [ "$b" -le $(( s * 3 / 2 )) ]; then
        echo "  ok   churn      ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ **平** ✓（10 倍轮数，内存不变）"
    else
        echo "  FAIL churn      ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ 涨了 ✗（槽位没复用？）"; fail=1
    fi
    ls=$(peak pool-leak 100000); lb=$(peak pool-leak 1000000)
    if [ "$lb" -gt $(( ls * 2 )) ]; then
        echo "  ok   canary     ->  不 remove：${ls} KB → ${lb} KB ⇒ **判据会响** ✓"
    else
        echo "  FAIL canary     ->  不 remove 竟然没涨（${ls} → ${lb} KB）⇒ 上面那条判据没有牙 ✗"; fail=1
    fi
else
    echo "  FAIL churn  ->  编不过"; fail=1
fi

echo "== ASan：这些路径必须干净 =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in basic api; do
        if "$EXTC" "tests/pool/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
           && gcc -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
           && "$TMP/$t" >/dev/null 2>&1; then :; else echo "  FAIL $t  ->  ASan 报错"; fail=1; ok=0; fi
    done
    if "$EXTC" tests/pool/churn.extc -o "$TMP/churn.c" >/dev/null 2>&1 \
       && gcc -O1 -g -fsanitize=address -o "$TMP/churn" "$TMP/churn.c" >/dev/null 2>&1 \
       && "$TMP/churn" 20000 >/dev/null 2>&1; then :; else echo "  FAIL churn  ->  ASan 报错"; fail=1; ok=0; fi
    [ "$ok" = 1 ] && echo "  ok   basic · api · churn  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
fi
rm -rf "$TMP"

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
