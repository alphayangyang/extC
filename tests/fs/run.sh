#!/usr/bin/env bash
# tests/fs/run.sh —— **文件归属（定案 79：程序拥有）的常设验收**
#
# 判据五条：
#   ① 正例：循环里就地 open/close ⇒ **fd 号恒定**（1000 轮拿到同一个号）✓
#   ② 判据有牙（canary）：不 close 的同一段程序**必须失败**（fd 漂）✓
#      它同时钉住检查的**边界**：句柄交出去之后，编译期证明不了 ⇒ 归运行时管
#   ③ 编译期兜底：开出来全程没人关 ⇒ **警告**（"nothing in this function closes it"）✓
#      判据：编译成功 + 警告带位置 + `-w` 关得掉（不是错误 —— 留到进程结束是合法选择）✓
#   ④ 双关安全：`close()` 幂等，第二次**不碰 `close(2)`** ⇒ 关不到别人的号 ✓
#   ⑤ 关闭后使用 ⇒ `failure(closed)`，**不 trap、不静默** ✓
#   ⑥ `readAll` 两条路：读到 EOF ✓ / 目标太小 ⇒ `destFull`（不是静默短读 ✗）✓
#   ⑦ ASan 干净：库那几条路径（open/put/readAll/close）没有内存错误 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0
mkdir -p build

echo "== 正例：循环里就地 open/close ⇒ fd 号恒定 =="
if out=$("$EXTC" --run tests/fs/fd-constant.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "fd 恒定 = "  || ok=0
    echo "$out" | grep -qF "号可复用"     || ok=0
    if [ "$ok" = 1 ]; then echo "  ok   fd-constant    ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL fd-constant    ->  $(echo "$out" | tr '\n' '|')"; fail=1; fi
else
    echo "  FAIL fd-constant    ->  跑不起来"; echo "$out" | head -4 | sed 's/^/        /'; fail=1
fi

echo "== 程序级输入/输出流（fs::fin / fs::fout：各一个 fd，任何函数里都能用）=="
if out=$("$EXTC" --run tests/fs/global-io.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "两个整数 = 7 35"        || ok=0
    echo "$out" | grep -qF "一行 = hello fin"       || ok=0
    echo "$out" | grep -qF "别的函数里读到 line = 42" || ok=0
    # 文件内容也要对（证明 fs::fout 那条路真的写进去了 ✓）
    if ! grep -qF "一行 = hello fin" build/global-io-out.txt 2>/dev/null; then ok=0; fi
    if ! grep -qF "别的函数里读到 line = 42" build/global-io-out.txt 2>/dev/null; then ok=0; fi
    if [ "$ok" = 1 ]; then
        echo "  ok   global-io  ->  $(echo "$out" | tr '\n' '|')（文件内容也对 ✓）"
    else
        echo "  FAIL global-io  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL global-io  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 判据有牙（canary）：不 close 的同一段程序**必须**失败 =="
out=$("$EXTC" --run tests/fs/leak-canary.extc 2>&1); rc=$?
if [ "$rc" != 0 ] && echo "$out" | grep -qF "fd 漂了"; then
    echo "  ok   leak-canary    ->  $(echo "$out" | head -1 | tr -d '\n')  ✓ 判据会响"
else
    echo "  FAIL leak-canary    ->  泄漏时判据没响（rc=$rc）：$(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 编译期兜底：开出来全程没人关 ⇒ **警告**（不是错误）=="
# ⚠️ 定案 79 之后这条是**警告**：故意留到进程结束是合法选择，编译器只证明"没人关"，
#    决定权留给程序 ✓ ⇒ 判据三条：① 编译**成功** ② 警告带位置地响 ③ `-w` 关得掉 ✓
W=tests/warnings/never-closed.extc     # 用例住在警告套件里（那边另有它自己的断言）
if out=$("$EXTC" "$W" -o /dev/null 2>&1) \
   && echo "$out" | grep -qF "is opened here and nothing in this function closes it"; then
    if "$EXTC" -w "$W" -o /dev/null 2>&1 | grep -q "warning:"; then
        echo "  FAIL never-closed    ->  \`-w\` 没关掉警告"; fail=1
    else
        echo "  ok   never-closed    ->  $(echo "$out" | head -1 | sed 's/^[^ ]*: //' | cut -c1-56)"
    fi
else
    echo "  FAIL never-closed    ->  警告没响（或编译失败了）：$(echo "$out" | head -2 | tr '\n' '|')"; fail=1
fi

echo "== readAll 的两条路：读到 EOF / 目标太小 ⇒ destFull（不是静默短读）=="
if out=$("$EXTC" --run tests/fs/read-all.extc 2>&1) \
   && echo "$out" | grep -qF "readAll = 17" && echo "$out" | grep -qF "destFull 带位置"; then
    echo "  ok   read-all       ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL read-all       ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 双关安全：close() 幂等，第二次不碰 close(2) =="
if out=$("$EXTC" --run tests/fs/close-twice.extc 2>&1) \
   && echo "$out" | grep -qF "双关安全" && echo "$out" | grep -qF "b 写成功"; then
    echo "  ok   close-twice    ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL close-twice    ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 关闭后使用 ⇒ failure(closed)，不 trap =="
if out=$("$EXTC" --run tests/fs/closed-use.extc 2>&1) \
   && echo "$out" | grep -qF "closed 带位置" && echo "$out" | grep -qF "不 trap"; then
    echo "  ok   closed-use     ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL closed-use     ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== ASan：库那几条路径必须干净 =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/probe.c"
if gcc -fwrapv -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" >/dev/null 2>&1; then
    ok=1
    for t in fd-constant close-twice closed-use read-all; do
        if "$EXTC" "tests/fs/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
           && gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
           && "$TMP/$t" >/dev/null 2>&1; then
            :
        else
            echo "  FAIL $t  ->  ASan 报错或构建失败"; fail=1; ok=0
        fi
    done
    [ "$ok" = 1 ] && echo "  ok   fd-constant · close-twice · closed-use · read-all  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
fi
rm -rf "$TMP"

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
