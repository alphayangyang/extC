#!/usr/bin/env bash
# tests/frozen/run.sh —— **`@frozen`（作者签字"布局就是 C 的布局"）的常设验收**
# 设计与理由：docs/topics/C-ABI.md §9.8。
#
# 判据（四正四反 + 两条**有牙**）：
#   ① 正例：真调 libc 的**按值返回**（imaxdiv）· 字段形状凑齐（i8/f64/bool/指针/数组/嵌套 frozen）·
#      经 `fn` 值按值传参 ⇒ 输出与 `// expect:` 一致 ✓
#   ② **镜像测试**：产物 + 一份手写等价 C 结构体放进同一个翻译单元 ⇒ `sizeof` 与**每个字段**的
#      `offsetof` 编译期相等，运行期再打印一行 ✓（这是唯一能验"与 C 想的一样"的办法）
#   ③ **产物自带的两条断言有牙**：把产物里 `struct inner` 的两个字段调换顺序 ⇒ 必须**编不过** ✓
#      （若它照样编过，那两条 `_Static_assert` 就是摆设 ✗）
#   ④ **镜像有牙**：把镜像里同样的两个字段调换 ⇒ 必须编不过 ✓
#   ⑤ 五条反例：没标记的按值 · `@frozen` 标在函数/枚举/impl 上 · 写两遍 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（真调 libc 的按值返回 + 字段形状 + 经 fn 值按值传参）=="
if out=$("$EXTC" --run tests/frozen/frozen.extc 2>&1); then
    want=$(grep -o '// expect:.*' tests/frozen/frozen.extc | sed 's|// expect: *||' | head -1)
    ok=1
    IFS=' ' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }; done
    if [ "$ok" = 1 ]; then echo "  ok   frozen  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL frozen  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL frozen  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 镜像测试（C 侧同布局的结构体：sizeof 与每个字段的 offsetof）=="
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
if "$EXTC" -w --no-line-map -o "$tmp/frozen.c" tests/frozen/frozen.extc >/dev/null 2>&1; then
    cat tests/frozen/mirror-pre.h "$tmp/frozen.c" tests/frozen/mirror.c > "$tmp/mirror.c"
    if gcc -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/mirror" "$tmp/mirror.c" 2>"$tmp/err"; then
        if mout=$("$tmp/mirror"); then echo "  ok   mirror  ->  $mout"
        else echo "  FAIL mirror  ->  跑起来失败"; fail=1; fi
    else
        echo "  FAIL mirror  ->  与 C 侧镜像对不上"; head -4 "$tmp/err" | sed 's/^/        /'; fail=1
    fi

    # ③ 产物的断言有牙：把 struct inner 的 x/y 调换 ⇒ 顺序断言必须响
    sed -e 's/int64_t x;/int64_t __sw;/' -e 's/int64_t y;/int64_t x;/' -e 's/int64_t __sw;/int64_t y;/' \
        "$tmp/frozen.c" > "$tmp/swapped.c"
    cat tests/frozen/mirror-pre.h "$tmp/swapped.c" tests/frozen/mirror.c > "$tmp/swapped_all.c"
    if gcc -std=c11 -w -o "$tmp/swapped" "$tmp/swapped_all.c" 2>"$tmp/err2"; then
        echo "  FAIL teeth-order  ->  字段调换之后**照样编过** ⇒ 产物里那两条 @frozen 断言是摆设 ✗"; fail=1
    elif grep -q "@frozen" "$tmp/err2"; then
        echo "  ok   teeth-order  ->  字段调换被抓（$(grep -m1 -o 'extC @frozen[^"]*' "$tmp/err2"))"
    else
        echo "  FAIL teeth-order  ->  编不过，但**不是** @frozen 断言报的"; head -3 "$tmp/err2" | sed 's/^/        /'; fail=1
    fi

    # ④ 镜像有牙：把镜像里 inner 的两个字段调换 ⇒ offsetof 断言必须响
    sed -e 's/int64_t x;/int64_t __sw;/' -e 's/int64_t y;/int64_t x;/' -e 's/int64_t __sw;/int64_t y;/' \
        tests/frozen/mirror.c > "$tmp/mirror_swapped.c"
    cat tests/frozen/mirror-pre.h "$tmp/frozen.c" "$tmp/mirror_swapped.c" > "$tmp/mirror_swapped_all.c"
    if gcc -std=c11 -w -o "$tmp/mirror_swapped" "$tmp/mirror_swapped_all.c" 2>"$tmp/err3"; then
        echo "  FAIL teeth-mirror ->  镜像调换之后照样编过 ⇒ 镜像测试是摆设 ✗"; fail=1
    else
        echo "  ok   teeth-mirror ->  镜像调换被抓"
    fi
else
    echo "  FAIL 生成产物失败（frozen.extc）"; fail=1
fi

echo "== 反例（都必须编译期挡住）=="
check_err() {
    local f=$1 want=$2 out
    if out=$("$EXTC" "$f" -o /dev/null 2>&1); then
        echo "  FAIL $(basename "$f")  ->  **编过了**（应该报错 ✗）"; fail=1; return
    fi
    echo "$out" | grep -qF -- "$want" || { echo "  FAIL $(basename "$f")  ->  消息里没有「$want」"; fail=1; return; }
    echo "  ok   $(basename "$f")  ->  $(echo "$out" | grep -m1 'error:' | cut -c1-84)"
}
check_err tests/frozen/errors/not_frozen.extc 'cannot cross the C boundary'
check_err tests/frozen/errors/on_fn.extc      '`@frozen` goes on a `struct`'
check_err tests/frozen/errors/on_enum.extc    '`@frozen` goes on a `struct`'
check_err tests/frozen/errors/on_impl.extc    'no annotation applies to an `impl` block'
check_err tests/frozen/errors/twice.extc      '`@frozen` appears twice'
# 边界的老反例也要与新的门票口径一致：slice 与**没标记**的结构体都过不去 ✓
check_err tests/extern/errors/slice_param.extc    'cannot cross the C boundary'
check_err tests/extern/errors/struct_by_value.extc 'cannot cross the C boundary'

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
