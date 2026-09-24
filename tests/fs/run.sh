#!/usr/bin/env bash
# tests/fs/run.sh —— **文件归属（定案 79：程序拥有）的常设验收**
#
# 判据五条：
#   ① 正例：循环里就地 open/close ⇒ **fd 号恒定**（1000 轮拿到同一个号）✓
#   ② 判据有牙（canary）：不 close 的同一段程序**必须失败**（fd 漂）✓
#      它同时钉住检查的**边界**：句柄交出去之后，编译期证明不了 ⇒ 归运行时管
#   ③ 编译期兜底：开出来全程没人关 ⇒ **编不过**（"nothing in this function closes it"）✓
#   ④ 双关安全：`close()` 幂等，第二次**不碰 `close(2)`** ⇒ 关不到别人的号 ✓
#   ⑤ 关闭后使用 ⇒ `failure(closed)`，**不 trap、不静默** ✓
#   ⑥ ASan 干净：库那几条路径（open/put/readAll/close）没有内存错误 ✓
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

echo "== 判据有牙（canary）：不 close 的同一段程序**必须**失败 =="
out=$("$EXTC" --run tests/fs/leak-canary.extc 2>&1); rc=$?
if [ "$rc" != 0 ] && echo "$out" | grep -qF "fd 漂了"; then
    echo "  ok   leak-canary    ->  $(echo "$out" | head -1 | tr -d '\n')  ✓ 判据会响"
else
    echo "  FAIL leak-canary    ->  泄漏时判据没响（rc=$rc）：$(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 编译期兜底：开出来全程没人关 ⇒ 编不过 =="
want=$(grep -m1 '^// expect-error:' tests/fs/errors/never-closed.extc | sed 's|^// expect-error: *||')
if out=$("$EXTC" tests/fs/errors/never-closed.extc -o /dev/null 2>&1); then
    echo "  FAIL never-closed    ->  应该报错但通过了（泄漏没被看见 ✗）"; fail=1
elif [ -n "${want:-}" ] && ! echo "$out" | grep -qF "$want"; then
    echo "  FAIL never-closed    ->  报错了但不是期望的那条：想要「$want」"; fail=1
else
    echo "  ok   never-closed    ->  $(echo "$out" | head -1 | sed 's/^[^ ]*: //' | cut -c1-62)"
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
if gcc -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" >/dev/null 2>&1; then
    ok=1
    for t in fd-constant close-twice closed-use; do
        if "$EXTC" "tests/fs/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
           && gcc -O1 -g -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
           && "$TMP/$t" >/dev/null 2>&1; then
            :
        else
            echo "  FAIL $t  ->  ASan 报错或构建失败"; fail=1; ok=0
        fi
    done
    [ "$ok" = 1 ] && echo "  ok   fd-constant · close-twice · closed-use  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
fi
rm -rf "$TMP"

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
