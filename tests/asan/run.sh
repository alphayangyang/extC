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
# LeakSanitizer 对这套内存模型没有意义（arena 到进程结束才释放），它在最小发行版里还会偶尔
# 在退出扫描时挂住 ⇒ 这里只抓越界与 use-after-free。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
# gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核会**偶发**在启动阶段自己 SEGV
# （stderr 只有 DEADLYSIGNAL，然后挂住）；实测 32 路并发 9/32 次，关掉地址随机化后 0/32。
# 原因与三条出路见 tests/asan-probe.sh。这里只对本套件关随机化。
if [ -z "${EXTC_ASAN_NO_SETARCH:-}" ] && command -v setarch >/dev/null 2>&1; then
    EXTC_ASAN_NO_SETARCH=1 exec setarch "$(uname -m)" -R bash "$0" "$@"
fi
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}   # 可以用 `EXTC=/path/to/extc` 拿别的编译器跑（证明这支验收真的会咬）
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 先探一下这台机器上的 ASan 运行时是不是可靠（gcc<=12 + 高熵 ASLR 会偶发在启动阶段自己 SEGV，
# 表现为"每次挂的用例都不一样"；探针与出路说明见 tests/asan-probe.sh）。
. tests/asan-probe.sh
if ! asan_probe; then
    echo "  skip  ASan 这一支跳过（原因见上）"
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
    if ! timeout 300 gcc -fwrapv -O1 -g -fsanitize=address -o "$TMP/$name" "$TMP/$name.c" \
                          >"$TMP/$name.gerr" 2>&1; then
        echo "  FAIL $name  ->  生成的 C 编不过"
        head -3 "$TMP/$name.gerr" | sed 's/^/        /'
        fail=1
        continue
    fi
    # 上限：一个卡住的用例不该让整支验收永久挂着（在最小发行版里实测踩到过）。
    # 编译与运行都要有界，超时按 FAIL 记 —— 卡住和不通过在这里是同一件事。
    out=$(timeout 60 "$TMP/$name" 2>&1); trc=$?
    if [ "$trc" = 124 ]; then
        echo "  FAIL $name  ->  运行超过 60s（卡住了）"
        fail=1
        continue
    fi
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
