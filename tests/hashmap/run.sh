#!/usr/bin/env bash
# tests/hashmap/run.sh —— **开放寻址哈希表的常设验收**（../extc-docs/design/HASHMAP.md）
#
# 判据：
#   ① 正例：新键/覆盖/查/删/表位置遍历 ✓
#   ② **碰撞**：当场量出 5 只落在**同一个初始桶**的键（键不写死，哈希改了它自己会变红）⇒
#      线性探测全部找到；挖掉链条中间一个后剩下的仍找到（回移 + 探测链 ✓）；再插回去 ✓
#   ③ **churn**：反复 put+remove 同一个键 ⇒ 1e6 轮的峰值 RSS 与 1e5 轮相当 ✓
#   ④ **判据有牙**：只 put 不 remove ⇒ 必须涨（canary）✓
#   ⑤ **增长**：增量长到 1e6 的期末字节 == 一开始就预留到同一 cap 的期末字节（旧列当场还，不留历代副本）✓
#   ⑥ ASan 干净 ✓
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
EXTC=./build/extc
CC=${CC:-cc}
fail=0
mkdir -p build

run_case() {
    local t=$1 out want ok=1 p
    if ! out=$("$EXTC" --run "tests/hashmap/$t.extc" 2>&1); then
        echo "  FAIL $t  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1; return
    fi
    want=$(grep -o '// expect:.*' "tests/hashmap/$t.extc" | sed 's|// expect: *||' | head -1)
    IFS='|' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || ok=0; done
    if [ "$ok" = 1 ]; then echo "  ok   $t  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL $t  ->  输出里缺「$want」（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
}

echo "== 正例：新键 · 覆盖 · 查 · 删 =="
run_case basic
run_case structkey
run_case dense
run_case index
echo "== 值语义：读是拷贝、改要 get→put、取走要 take、删是 remove =="
run_case value
echo "== 遍历器：iter/next/value 迭代协议（for 与手写 while 同一个游标）=="
run_case iter
echo "== 碰撞：线性探测 + 回移（无墓碑）+ 探测链 =="
run_case collide

echo "== churn：1e6 轮与 1e5 轮的峰值 RSS 必须相当 =="
# `-fwrapv`：生成的 C 的**既定编译契约**（有符号溢出在 extC 里是有定义的绕回，见 MANUAL §8；
# 编译器驱动自己也是这么编的，见 src/main.c）。少这一条时，`-O2` 会按"乘法不会溢出"优化，
# 哈希混合这类依赖绕回的代码会静默变形 —— 2026-09-26 就因此把 grow 用例挂死过一次。
build_one() { "$EXTC" "tests/hashmap/$2.extc" -o "build/$1.c" >/dev/null 2>&1 \
    && "$CC" -O1 -std=c11 -fwrapv "build/$1.c" -o "build/$1" >/dev/null 2>&1; }
peak() { /usr/bin/time -f %M "./build/$1" "$2" 2>&1 >/dev/null | tail -1; }
if build_one map-churn churn && build_one map-leak churn-leak; then
    s=$(peak map-churn 100000); b=$(peak map-churn 1000000)
    if [ "$b" -le $(( s * 3 / 2 )) ]; then
        echo "  ok   churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ **平** ✓"
    else
        echo "  FAIL churn    ->  1e5: ${s} KB · 1e6: ${b} KB ⇒ 涨了 ✗（回移/增长有问题？）"; fail=1
    fi
    ls=$(peak map-leak 100000); lb=$(peak map-leak 1000000)
    if [ "$lb" -gt $(( ls * 2 )) ]; then
        echo "  ok   canary   ->  只 put 不 remove：${ls} KB → ${lb} KB ⇒ **判据会响** ✓"
    else
        echo "  FAIL canary   ->  没涨（${ls} → ${lb} KB）⇒ 上面那条没有牙 ✗"; fail=1
    fi
else
    echo "  FAIL churn  ->  编不过"; fail=1
fi

echo "== 增长：旧三列当场还，期末字节 == 同 cap 的预留版 =="
# 判据分两层。**第一层是精确字节账**（主判据）：增量长到 1e6 与一开始预留到**同一 cap** 的两臂，
# 期末 `bytesOf()` 必须逐字节相等 —— 一整代旧列（约 1.2M×17 B = 20 MB）不可能藏在这个等式里。
# 第二层是 cap 相等（先判）：增长算术一改，预留版的魔数就会失配，这一层让失败**指着原因**。
# 第二层的注释在 grow_rss_reserved.extc 里（cap = 19×2^16 的装载反算）。
if build_one grow-inc grow_rss && build_one grow-res grow_rss_reserved; then
    oi=$(./build/grow-inc); or=$(./build/grow-res)
    bi=$(echo "$oi" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p'); ci=$(echo "$oi" | sed -n 's/.*cap=\([0-9]*\).*/\1/p')
    br=$(echo "$or" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p'); cr=$(echo "$or" | sed -n 's/.*cap=\([0-9]*\).*/\1/p')
    gi=$(peak grow-inc 0); gr=$(peak grow-res 0)
    if [ -n "$ci" ] && [ -n "$cr" ] && [ "$ci" = "$cr" ] && [ -n "$bi" ] && [ "$bi" = "$br" ]; then
        echo "  ok   grow      ->  cap ${ci} / ${cr}，bytes ${bi} / ${br} B ⇒ **逐字节相当**（旧列全还了）✓ · 峰值 RSS ${gi} / ${gr} KB（参考）"
    else
        echo "  FAIL grow      ->  cap ${ci:-?} / ${cr:-?}，bytes ${bi:-?} / ${br:-?} B（旧列没还，或两边容量不同）✗"; fail=1
    fi
else
    echo "  FAIL grow rss ->  编不过"; fail=1
fi

echo "== ASan =="
TMP=$(mktemp -d)
printf 'int main(void){return 0;}\n' > "$TMP/p.c"
if gcc -fsanitize=address -o "$TMP/p" "$TMP/p.c" >/dev/null 2>&1; then
    ok=1
    for t in basic collide index dense value; do
        "$EXTC" "tests/hashmap/$t.extc" -o "$TMP/$t.c" >/dev/null 2>&1 \
          && gcc -O1 -g -fwrapv -fsanitize=address -o "$TMP/$t" "$TMP/$t.c" >/dev/null 2>&1 \
          && "$TMP/$t" >/dev/null 2>&1 || { echo "  FAIL $t -> ASan 报错"; fail=1; ok=0; }
    done
    "$EXTC" tests/hashmap/churn.extc -o "$TMP/c.c" >/dev/null 2>&1 \
      && gcc -O1 -g -fwrapv -fsanitize=address -o "$TMP/c" "$TMP/c.c" >/dev/null 2>&1 \
      && "$TMP/c" 20000 >/dev/null 2>&1 || { echo "  FAIL churn -> ASan 报错"; fail=1; ok=0; }
    [ "$ok" = 1 ] && echo "  ok   basic · collide · index · dense · value · churn  ->  ASan 干净"
else
    echo "  skip  gcc 不支持 -fsanitize=address"
fi
rm -rf "$TMP"
echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
