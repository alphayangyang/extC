#!/usr/bin/env bash
# tests/io/run.sh —— **IO-0（定案 73 + 定案 74）的常设验收**
#
# 判据：
#   ① `use std::io` 能从 **stdin** 读（这一条是里程碑的门槛 ✓）
#   ② 两种风格**混着用**都对：`nextInt`（OI 式）+ `nextLine`（协议式）✓
#   ③ **三条路分得开**：EOF / 行太长 / 读错误（老 `readLine` 把后两条都当成 EOF ✗）
#   ④ `std::sys::io` 只管原语、`std::io` 是普通库（模块分层真的成立 ✓）
#      ⚠️ `sys` 不是"一个模块"，是**一条边界**的路径写法 ⇒ **按族分文件**
#         （后面还有 std::sys::{thread,time,net,proc}，全塞一个文件会变垃圾场 ✗）
#   ⑤ **不是逐字节读**：一次 64KB（老实现 50 万行要 2.3s，是"能跑但慢 190 倍"那种坏 ✓）
#   ⑥ **文件当输入源**（IO-2 ①）：`f.reader()` 之后 `nextInt`/`nextLine`/`nextToken`
#      与 stdin **逐字相同** —— 「读存档」和「读玩家输入」是同一段代码 ✓
#   ⑦ **退出码**（IO-2 ②）：`proc::exit(7)` ⇒ 输出在、退出码是 7、之后的语句不执行 ✓
#   ⑧ **终端 raw mode**（IO-2 ③）：非 tty ⇒ `failure(notATerminal)` 带位置；真 PTY
#      （`script`）⇒ 开得起来、`close()` 还原得了（**逐字节**比 termios，不比返回值 ✓）；
#      而且 **raw 模式下 trap** 时终端也被还回去（临终钩子，用 strace 量 `TCSETS` ✓）
#   ⑨ **CRLF**（定案 84）：`nextLine` 剥行尾 `\r`、`nextLineRaw` 原样、孤立 `\r` 是内容、
#      `\r\n` 的空行长度是 0、EOF 前最后一个 `\r` 也剥 ✓
#      外加**输出侧**：只发 `\n`，要 CRLF 得显式写（用 `od` 断言字节 `41 0d 0a 42 0a` ✓）
#      （这一条挡的是"Windows 上编辑、WSL 里跑"那类真事故：以前每行都多一个 `\r`，
#        长度也多 1 —— 用户的第一个文本程序就撞上了 ✗）
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 正例（两种风格混用：nextInt + nextLine）=="
if out=$(printf '5 7\nhello world\n99' | "$EXTC" --run tests/io/main.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "求和 = 12"      || ok=0
    echo "$out" | grep -qF "回显：hello world" || ok=0
    echo "$out" | grep -qF "下一个 = 99"     || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   read-stdin  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL read-stdin  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL read-stdin  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 流式 IO（cout << x / cin >> x：<< 与 >> 的重载 + 链式 + 共同缓冲）=="
if out=$(printf '40 2\n' | "$EXTC" --run tests/io/stream.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "控制台：x = 1 · pi = 3.5 · 对 = true" || ok=0
    echo "$out" | grep -qF "读到 x = 40 y = 2 和 = 42"        || ok=0
    echo "$out" | grep -qF "读好了"                          || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   stream  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL stream  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL stream  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi
# 读不到：目标保持原值 + inBad() 为真（EOF 是正常结局，不是崩溃 ✓）
if out=$(printf '' | "$EXTC" --run tests/io/stream.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "读到 x = 0 y = 0 和 = 0" || ok=0
    echo "$out" | grep -qF "读坏了（EOF / 错误）"      || ok=0
    if [ "$ok" = 1 ]; then echo "  ok   stream-eof  ->  目标保持原值 + inBad 为真 ✓"
    else echo "  FAIL file-eof  ->  $(echo "$out" | tr '\n' '|')"; fail=1; fi
else
    echo "  FAIL file-eof  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -4; fail=1
fi

echo "== 文件流式读取（定案 88 第 3 步：fin >> 整数 >> 一行 >> 一个字节）=="
if out=$("$EXTC" --run tests/io/stream-file.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "整数 = 42 · 一行 = hello world · 字节 = 88" || ok=0
    echo "$out" | grep -qF "接着 token = 剩下的"                        || ok=0
    if [ "$ok" = 1 ]; then echo "  ok   stream-file  ->  $(echo "$out" | tr '\n' '|')"
    else echo "  FAIL stream-file  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1; fi
else
    echo "  FAIL stream-file  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi
# 读到结束：目标**保持原值** + bad 置起（不 trap、不写 0 ✓）
if out=$("$EXTC" --run tests/io/stream-file.extc 2>&1); then
    if echo "$out" | grep -qF "结束时保持原值 = 999 · bad = true"; then
        echo "  ok   file-eof  ->  EOF 时目标保持原值 + bad 为真 ✓"
    else
        echo "  FAIL file-eof  ->  $(echo "$out" | tr '\n' '|')"; fail=1
    fi
else
    echo "  FAIL file-eof  ->  跑不起来"; fail=1
fi

echo "== 回归：struct 打印 + 跨模块全局常量 + reader（这三样一起曾经让编译器段错误 ✗）=="
if out=$(printf 'hello extC\n' | "$EXTC" --run tests/io/struct-print.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "p = point { x: 1, y: 2 }" || ok=0
    echo "$out" | grep -qF "读到：hello extC"             || ok=0
    echo "$out" | grep -qF "常量 = 1"                     || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   struct-print ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL struct-print ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL struct-print ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== CRLF（定案 84：nextLine 剥 \r · nextLineRaw 原样 · 孤立 \r 是内容）=="
# 输入：hello CRLF | world CRLF | a<孤立CR>b CRLF | 空行(CRLF) | zz CR(EOF)
if out=$(printf 'hello\r\nworld\r\na\rb\r\n\r\nzz\r' | "$EXTC" --run tests/io/crlf.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "trimmed = hello len 5" || ok=0   # ① 剥掉 \r
    echo "$out" | grep -qF "len 6"                 || ok=0   # ② 原样（world + \r）
    echo "$out" | grep -qF "len 3"                 || ok=0   # ③ 孤立 \r 是内容（a\rb）
    echo "$out" | grep -qF "blank   = len 0"       || ok=0   #    \r\n 的空行 = 空 ✓
    echo "$out" | grep -qF "eofCR   = zz len 2"    || ok=0   #   EOF 前最后一个 \r 也剥 ✓
    # ④ 输出侧：**只发 \n**，要 CRLF 只能显式写 —— 判据是**字节**，不是"看着像"
    tail5=$(echo "$out" | tail -c 5 | od -An -tx1 | tr -s ' ' | sed 's/^ //;s/ $//')
    [ "$tail5" = "41 0d 0a 42 0a" ] || { ok=0; echo "        (显式 CRLF 的字节是 [$tail5]，应为 [41 0d 0a 42 0a])"; }
    if [ "$ok" = 1 ]; then
        echo "  ok   crlf         ->  6 条口径全对（剥/留/孤立/空行/EOF + 显式 CRLF 的字节）✓"
    else
        echo "  FAIL crlf         ->  口径不对（$(echo "$out" | tr '\n' '|' | cat -v)）"; fail=1
    fi
else
    echo "  FAIL crlf         ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 三条路（EOF / 行太长 / 读错误 —— 必须分得开）=="
# ① EOF ⇒ success(0)（**不是错误** ✓）
if out=$(printf '' | "$EXTC" --run tests/io/eof.extc 2>&1) && echo "$out" | grep -qF "EOF ✓"; then
    echo "  ok   eof          ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL eof          ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
# ② 行比缓冲长 ⇒ failure(lineTooLong)，**绝不许静默切一半** ✗
if out=$(printf 'ab\n0123456789012345678\n' | "$EXTC" --run tests/io/line-too-long.extc 2>&1) \
   && echo "$out" | grep -qF "lineTooLong ✓"; then
    echo "  ok   line-too-long ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL line-too-long ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
# ③ read(2) 出错 ⇒ failure(readFailed)，**绝不许当成 EOF** ✗
if out=$("$EXTC" --run tests/io/read-failed.extc 2>&1) && echo "$out" | grep -qF "readFailed ✓"; then
    echo "  ok   read-failed  ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL read-failed  ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi

echo "== 文件当输入源（IO-2 ①：`f.reader()` —— 与 stdin 同一套 API）=="
# 判据：`nextInt` / `nextLine` / `nextToken` 在**文件**上逐条实测，
# 外加边界：关掉的句柄再要 reader ⇒ `failure(closed)` 带位置、不 trap ✓
if out=$("$EXTC" --run tests/io/file-reader.extc 2>&1); then
    ok=1
    echo "$out" | grep -qF "求和 = 46"        || ok=0
    echo "$out" | grep -qF "回显：hello file"  || ok=0
    echo "$out" | grep -qF "token = world"     || ok=0
    echo "$out" | grep -qF "下一个 = 99"       || ok=0
    echo "$out" | grep -qF "closed 带位置"     || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok   file-reader  ->  $(echo "$out" | tr '\n' '|')"
    else
        echo "  FAIL file-reader  ->  输出对不上（$(echo "$out" | tr '\n' '|')）"; fail=1
    fi
else
    echo "  FAIL file-reader  ->  编不过 / 跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 退出码（IO-2 ②：`proc::exit(code)` —— 它之前的输出要出来、之后的语句不执行）=="
# 判据三条：① 之前的 println 必须在（C 的 exit 会 flush 流 ✓）
#           ② shell 看到的退出码 == 给的那个数 ✓
#           ③ 它之后的 `return 0` 不许把退出码改回去 ✓
"$EXTC" tests/io/exit-code.extc -o build/exit-code.c >/dev/null 2>&1 \
  && ${CC:-cc} -std=c11 -O1 build/exit-code.c -o build/exit-code >/dev/null 2>&1
if [ -x build/exit-code ]; then
    out=$(./build/exit-code 2>&1); rc=$?
    if [ "$rc" = 7 ] && echo "$out" | grep -qF "退出前这一行要出来"; then
        echo "  ok   exit-code   ->  输出在「$out」· 退出码 = $rc ✓"
    else
        echo "  FAIL exit-code   ->  退出码 $rc（期望 7）· 输出「$out」"; fail=1
    fi
else
    echo "  FAIL exit-code   ->  编不过"; fail=1
fi

echo "== 终端 raw mode（IO-2 ③：非 tty ⇒ failure 带位置 · PTY ⇒ 开得起来也还原得了）=="
# 判据①：非 tty（CI 里就是）⇒ `failure(notATerminal)`，不 trap、不算失败 ✓
# 判据②：真 tty（用 `script` 起 PTY）⇒ raw 开成功 + `close()` 还原 ✓
"$EXTC" tests/io/raw-mode.extc -o build/raw-mode.c >/dev/null 2>&1 \
  && ${CC:-cc} -std=c11 -O1 build/raw-mode.c -o build/raw-mode >/dev/null 2>&1
if [ -x build/raw-mode ]; then
    out=$(./build/raw-mode < /dev/null 2>&1); rc=$?
    if [ "$rc" = 0 ] && echo "$out" | grep -qF "不是终端 ✓"; then
        echo "  ok   raw-mode(非 tty) ->  $out"
    else
        echo "  FAIL raw-mode(非 tty) ->  rc=$rc · 「$out」"; fail=1
    fi
    # ② PTY：`script` 不在就跳过（这一支是加分项，不是门槛 ✓）
    if command -v script >/dev/null 2>&1; then
        out=$(script -qec ./build/raw-mode /dev/null 2>&1 | tr -d '\r')
        if echo "$out" | grep -qF "raw 真的切过去了" && echo "$out" | grep -qF "还原后逐字节相同"; then
            echo "  ok   raw-mode(PTY)    ->  $(echo "$out" | tr '\n' '|')"
        else
            echo "  FAIL raw-mode(PTY)    ->  「$(echo "$out" | tr '\n' '|')」"; fail=1
        fi
    else
        echo "  skip raw-mode(PTY)    ->  没有 \`script\`（这一支跳过）"
    fi
else
    echo "  FAIL raw-mode  ->  编不过"; fail=1
fi

echo "== raw 模式下 trap ⇒ 终端必须被还回去（临终钩子）=="
# 判据两条，都要：
#   ① 这次运行里 `TCSETS` 出现**两次**：进入 raw 一次 + trap 还原一次 ✓
#   ② **第二次**带的是原始（cooked）标志（ISIG/ICANON/ECHO）——
#      本程序**从不调 `close()`**（它死在半路）⇒ 只可能是运行时的 trap 路径做的 ✓✓
# 量具是 strace（跟 tests/fs 用 strace 验证 fd 归属同一个口径：不猜，量 ✓）
if command -v strace >/dev/null 2>&1 && command -v script >/dev/null 2>&1; then
    script -qec "strace -f -e trace=ioctl -o build/raw-trap.ioctl ./build/raw-trap" /dev/null >build/raw-trap.out 2>&1
    n=$(grep -c "TCSETS" build/raw-trap.ioctl 2>/dev/null || echo 0)
    last=$(grep "TCSETS" build/raw-trap.ioctl 2>/dev/null | tail -1)
    if [ "$n" -ge 2 ] && echo "$last" | grep -qE "ISIG|ICANON"; then
        echo "  ok   raw-trap    ->  TCSETS x$n，最后一条是 cooked ✓（$(grep -o 'trap: index [0-9]* out of range' build/raw-trap.out | head -1)）"
    else
        echo "  FAIL raw-trap    ->  TCSETS x$n（期望 ≥2）· 最后一条：$(echo "$last" | cut -c1-60)"
        fail=1
    fi
else
    echo "  skip raw-trap    ->  没有 \`strace\`/\`script\`（这一支跳过）"
fi

echo "== 顺序（println 与 writeBytes 混用 —— 定案 75 修的真缺陷）=="
if out=$(printf '' | "$EXTC" --run tests/io/order.extc 2>&1) \
   && [ "$(printf '%s' "$out" | tr -d '\r')" = "A
B
C" ]; then
    echo "  ok   order        ->  A / B / C 顺序正确 ✓"
else
    echo "  FAIL order        ->  顺序乱了：$(printf '%s' "$out" | tr '\n' '|')"; fail=1
fi
# 一个**静态**判据：`writeBytes` 里必须真的先 flush（删掉它上面那条也会红 ✓）
if awk '/^fn writeBytes/,/^}/' stdlib/std/io.extc | grep -q 'flush()'; then
    echo "  ok   writeBytes  ->  里面有 flush()（顺序不可能错 ✓）"
else
    echo "  FAIL writeBytes  ->  少了 flush() ⇒ 与 println 混用会乱序 ✗"; fail=1
fi

echo "== writer（写文件 → 用 reader 读回，两边对称 ✓）=="
if out=$(printf '' | "$EXTC" --run tests/io/writer-file.extc 2>&1) && echo "$out" | grep -qF "回读 = 100/200 ✓"; then
    echo "  ok   writer-file  ->  $(echo "$out" | tr '\n' '|')"
else
    echo "  FAIL writer-file  ->  $(echo "$out" | tr '\n' '|')"; fail=1
fi
rm -f tests/io/writer-tmp.txt

echo "== 分层（std::sys::io = 特权层 · io 族，std::io = 普通库）=="
# ⚠️ 这是**结构检查**，不是编译器强制的（"只有特权模块能声明原语"那条还没做 ✗ 见定案 72）
if grep -q '^extern!' stdlib/std/sys/io.extc && ! grep -q '^fn main' stdlib/std/sys/io.extc; then
    echo "  ok   std::sys::io  ->  只有它声明 C 原语（+ 签字），没有 main ✓"
else
    echo "  FAIL std::sys::io  ->  形状不对 ✗"; fail=1
fi
if grep -q '^use std::sys::io' stdlib/std/io.extc && ! grep -q '^extern!' stdlib/std/io.extc; then
    echo "  ok   std::io   ->  普通库（自己不碰 extern，只用 std::sys::io ✓）"
else
    echo "  FAIL std::io   ->  形状不对 ✗"; fail=1
fi

echo "== 不是逐字节读（老实现 50 万行 2.3s —— 这条抓"能跑但慢 190 倍" ✓）=="
# 10 万行 × 3 个数 ≈ 1.7MB。逐字节读要 ~0.5s 以上；分块读是毫秒级 ✓
BIG=$(mktemp)
i=0
while [ $i -lt 100000 ]; do printf '123 456 789\n'; i=$((i+1)); done > "$BIG"
start=$(date +%s%N)
out=$(printf '' | "$EXTC" --run tests/io/sum-big.extc < "$BIG" 2>&1)
end=$(date +%s%N)
ms=$(( (end - start) / 1000000 ))
rm -f "$BIG"
# 期望和 = 100000 * (123+456+789) = 136800000
if echo "$out" | grep -qF "sum = 136800000"; then
    if [ "$ms" -lt 400 ]; then
        echo "  ok   分块读  ->  10 万行 ${ms}ms（< 400ms ✓ 逐字节要 ~500ms 以上）"
    else
        echo "  FAIL 分块读  ->  10 万行花了 ${ms}ms ⇒ 像是退回逐字节了 ✗"; fail=1
    fi
else
    echo "  FAIL 大输入求和  ->  $(echo "$out" | tail -2 | tr '\n' '|')"; fail=1
fi

exit $fail
