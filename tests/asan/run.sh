#!/usr/bin/env bash
# **ASan 验收**：内存安全形状必须**真的**跑得干净（不是"编过了"就算）。
#
# 为什么要有这一支：本项目最近三个真 bug（PLAN #31 / #33 / #38）全都长成同一个样子 ——
# **检查器放行、生成的 C 编得过、退出码 0，但内存已经悬垂了** ✗
# 光看 `--run` 的退出码和打印是**抓不到**的（打印出过期数据也可能是"看着对"的）。
# 所以：每个用例都 `-fsanitize=address` 编一遍、跑一遍，
# 出现 `AddressSanitizer` 报告 = FAIL ✓
#
# ⚠️ gcc 不支持 ASan（比如缺 libasan）⇒ 整支**跳过并说明**，不算失败 ✓
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}   # 可以用 `EXTC=/path/to/extc` 拿别的编译器跑（证明这支验收真的会咬）
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 先探一下 gcc 能不能用 ASan
cat > "$TMP/probe.c" <<'EOF'
int main(void) { return 0; }
EOF
if ! gcc -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" >/dev/null 2>&1; then
    echo "  skip  gcc 不支持 -fsanitize=address（这一支跳过）"
    exit 0
fi

fail=0
for f in tests/asan/*.extc; do
    name=$(basename "$f" .extc)
    if ! "$EXTC" "$f" -o "$TMP/$name.c" >"$TMP/$name.cerr" 2>&1; then
        echo "  FAIL $name  ->  编译期就报错了"
        head -3 "$TMP/$name.cerr" | sed 's/^/        /'
        fail=1
        continue
    fi
    if ! gcc -O1 -g -fsanitize=address -o "$TMP/$name" "$TMP/$name.c" >"$TMP/$name.gerr" 2>&1; then
        echo "  FAIL $name  ->  生成的 C 编不过"
        head -3 "$TMP/$name.gerr" | sed 's/^/        /'
        fail=1
        continue
    fi
    out=$("$TMP/$name" 2>&1)
    if echo "$out" | grep -q "AddressSanitizer"; then
        echo "  FAIL $name  ->  $(echo "$out" | grep -m1 -o 'ERROR: AddressSanitizer:.*')"
        fail=1
    else
        # 输出断言（跟 examples 一个规矩：`// expect:` 必须出现在输出里）
        if want=$(grep -o '// expect:.*' "$f" | sed 's|// expect: *||' | head -1) \
           && [ -n "$want" ] && ! echo "$out" | grep -qF -- "$want"; then
            echo "  FAIL $name  ->  输出里没有「$want」（$(echo "$out" | tr '\n' '|')）"
            fail=1
        else
            echo "  ok   $name  ->  ASan 干净（$(echo "$out" | tr '\n' '|')）"
        fi
    fi
done
exit $fail
