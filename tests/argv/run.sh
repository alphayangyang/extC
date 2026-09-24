#!/usr/bin/env bash
# tests/argv/run.sh —— **`main(args)` 的常设验收**（规范：docs/topics/IO.md §7）
#
# 判据三条：
#   ① 正例：`./prog alpha beta` ⇒ `args.len = 3`（**含程序名**，与 C 一致）、
#      两个实参原封到手 ✓
#   ② 边界：一个实参都不给 ⇒ `args.len = 1`（只有程序名）—— 没有 "+1"、没有哨兵 ✓
#   ③ 反例：形状写错（类型不对 / 参数太多 / 返回类型不是 `i32`）⇒ **编译期**报
#      **extC 的错**，而不是把 `int main(int argc, char **argv)` 的包装露给 gcc ✗
#   ④ 回归哨兵：`fn main()`（无参数）照旧 —— 序言只为 `main(args)` 发射 ✓
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

echo "== 正例：args.len 含程序名，实参原样到手 =="
if "$EXTC" tests/argv/args.extc -o build/argv_args.c 2>build/argv_args.err \
   && "$CC" -std=c11 -O1 build/argv_args.c -o build/argv_args 2>>build/argv_args.err; then
    out=$(./build/argv_args alpha beta)
    want=$(printf '3\nalpha\nbeta')
    if [ "$out" = "$want" ]; then
        echo "  ok   main(args)  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL main(args)  ->  期望「3|alpha|beta」，得到「$(echo "$out" | tr '\n' '|')」"; fail=1
    fi
    out=$(./build/argv_args)
    if [ "$out" = "1" ]; then
        echo "  ok   不给实参      ->  len = 1（= 程序名）"
    else
        echo "  FAIL 不给实参      ->  期望 1，得到「$out」"; fail=1
    fi
else
    echo "  FAIL main(args)  ->  编译不过"
    sed 's/^/        /' build/argv_args.err | head -6; fail=1
fi

echo "== 回归哨兵：fn main()（无参数）照旧 =="
if out=$("$EXTC" --run tests/argv/plain.extc 2>&1) && [ "$out" = "no args needed" ]; then
    echo "  ok   fn main()   ->  $out"
else
    echo "  FAIL fn main()   ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 反例：形状写错 ⇒ 编译期挡住（extC 的错，不是 gcc 的错）=="
for f in param-type too-many bad-return; do
    want=$(grep -m1 '^// expect-error:' "tests/argv/errors/$f.extc" | sed 's|^// expect-error: *||')
    if out=$("$EXTC" "tests/argv/errors/$f.extc" -o /dev/null 2>&1); then
        echo "  FAIL $f  ->  应该报错但通过了"; fail=1
    elif ! echo "$out" | grep -qF "$want"; then
        echo "  FAIL $f  ->  不是期望的那条：想要「$want」"
        echo "$out" | head -2 | sed 's/^/        /'; fail=1
    else
        # 取第一条 **error**：警告（`println` 弃置、未用参数）也写 stderr 而且排在前面 ✗
        echo "  ok   $f  ->  $(echo "$out" | grep -m1 ': error:' | sed 's/^[^ ]*: //' | cut -c1-56)"
    fi
done

if [ "$fail" = 0 ]; then echo "tests/argv: 全过"; else echo "tests/argv: 有失败"; fi
exit "$fail"
