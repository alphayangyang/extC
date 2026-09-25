#!/usr/bin/env bash
# bench/app/run.sh —— **综合应用场景横评：extC STL vs C++ STL vs Go**
#
# 与 `bench/stl`（单个容器的一招一式）不同，这里量的是**三种应用形状**里三个语言各自的
# 标准容器库合起来的表现：
#
#   A 会话 / 连接表    hashMapI64       60% 查 / 20% 建更 / 20% 删，活跃集固定
#   B 路由表           有序表           插入 + 前驱查找 + 区间扫描 + 删除
#   C 请求日志 / 缓冲  动态字节缓冲     追加 + 查找 + 截断（有界缓冲）
#
# 口径（与 bench/stl / bench/bigmatrix 一致，别松）：
#   · **同算法同参数**：每个场景三种语言读同一组规模参数，做同一串操作；
#   · 判据是**打印出来的校验和逐字节相同** —— 不同 ⇒ 这一格标 FAIL（比"慢"严重得多）；
#   · 时间与峰值 RSS 都是 best of RUNS（时间用 bash 内建 EPOCHREALTIME，微秒分辨率；
#     每个样本重复 K 次凑到约 0.2 s，避免把进程启动算进 ns/op；RSS 用 /usr/bin/time）。
#
# 用法：bash bench/app/run.sh          （结果写 bench/app/RESULTS.md，同时打到屏幕）
#   RUNS=1 bash bench/app/run.sh       （快速跑一遍）
#   SCALE=1 bash bench/app/run.sh      （只跑每个场景的最小规模）
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build/extc}
CC=${CC:-cc}
CXX=${CXX:-g++}
GO=${GO:-go}
RUNS=${RUNS:-3}
B=build/app
OUTF=$(mktemp)
RAW=$(mktemp)
mkdir -p "$B"

# 规模：A 以「活跃集」为规模（ops = 30 × active）；B / C 直接是操作数。
A_ACTIVE=${A_ACTIVE:-"4096 65536 1048576"}
B_OPS=${B_OPS:-"200000 1000000 2000000"}
C_OPS=${C_OPS:-"200000 2000000 20000000"}
if [ "${SCALE:-0}" = 1 ]; then A_ACTIVE="4096"; B_OPS="200000"; C_OPS="200000"; fi

MACHINE=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')
KERNEL=$(uname -r)
pass=0; fail=0

# ---------- 计时：微秒分辨率、每个样本重复 K 次 ----------
now_us() { local t=${EPOCHREALTIME}; printf '%s' "${t/./}"; }

# measure <标签> <可执行> <程序参数...>
# 回传：M_US（每次运行的微秒数，best of RUNS）· M_RSS（峰值 KB）· M_OUT（输出行）
M_US=0; M_RSS=0; M_OUT=""
measure() {
    local label=$1 exe=$2; shift 2
    local t0 t1 cal us k r c best="" rss
    t0=$(now_us); "$exe" "$@" >"$OUTF" 2>/dev/null; t1=$(now_us)
    cal=$((t1 - t0)); [ "$cal" -lt 1 ] && cal=1
    k=$((200000 / cal))                       # ≈0.2 s 一个样本
    [ "$k" -lt 1 ] && k=1
    [ "$k" -gt 4000 ] && k=4000
    for ((r=0; r<RUNS; r++)); do
        t0=$(now_us)
        for ((c=0; c<k; c++)); do "$exe" "$@" >"$OUTF" 2>/dev/null; done
        t1=$(now_us)
        us=$(((t1 - t0) / k))
        if [ -z "$best" ] || [ "$us" -lt "$best" ]; then best=$us; fi
    done
    "$exe" "$@" >"$OUTF" 2>/dev/null
    M_OUT=$(head -1 "$OUTF")
    rss=$( { /usr/bin/time -f "%M" "$exe" "$@" >/dev/null; } 2>&1 | tail -1 )
    case "$rss" in ''|*[!0-9]*) rss=0 ;; esac
    M_US=$best; M_RSS=$rss
    printf '    %-9s %10s ms  %9s ns/op  %7s MB  %s\n' \
        "$label" \
        "$(awk -v u="$M_US" 'BEGIN{printf "%.3f", u/1000}')" \
        "$(awk -v u="$M_US" -v n="$OPS" 'BEGIN{printf "%.2f", (n>0)?u*1000/n:0}')" \
        "$(awk -v k="$M_RSS" 'BEGIN{printf "%.1f", k/1024}')" "$M_OUT"
}

# ---------- 构建 ----------
build_extc() { "$EXTC" "bench/app/$1.extc" -o "$B/$1.extc.c" >/dev/null 2>&1 \
    && $CC -O2 -std=c11 -fwrapv "$B/$1.extc.c" -o "$B/$1_extc" >/dev/null 2>&1; }
build_cpp() { $CXX -O2 -std=c++17 "bench/app/$1.cpp" -o "$B/$1_cpp" >/dev/null 2>&1; }
# Go 一个目录只装一个包，而这三个 .go 各自是独立的 `package main` ⇒ 按**文件**编
# （`go build x.go` 走 command-line-arguments 模式，不把同目录的兄弟文件拉进来）。
# `bench/bigmatrix/src/` 的 5 个 .go 也是这么放的，口径一致。
build_go()  { $GO build -o "$PWD/$B/$1_go" "bench/app/$1.go" >/dev/null 2>&1; }

# cell <us> <rss_kb> <ops> -> 「时间 · ns/op · 峰值」
cell() { awk -v u="$1" -v k="$2" -v n="$3" 'BEGIN{
    printf "%.3f s<br>%.2f ns/op<br>%.1f MB", u/1e6, (n>0)?u*1000/n:0, k/1024 }'; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b>0) printf "%.2f×", a/b; else printf "-" }'; }

# run_one <name> <标签> <ops> <程序参数...>
run_one() {
    local name=$1 label=$2 ops=$3; shift 3
    # ns/op 的分母是**这个场景真正的逻辑操作数**，不是规模参数：B 的规模参数只是"插入条数"，
    # 它的循环还做了 ops 次前驱查找 + ops/4 次区间扫描 + ops/2 次删除 ⇒ 一共 2.75×。
    case "$name" in
        route) OPS=$((ops * 11 / 4)) ;;
        *)     OPS=$ops ;;
    esac
    local -a args=("$@")
    if ! build_extc "$name" || ! build_cpp "$name"; then
        printf '  FAIL %s：C/C++ 侧编不过\n' "$name"; fail=$((fail+1)); return
    fi
    if ! build_go "$name"; then
        printf '  FAIL %s：Go 侧编不过（%s）\n' "$name" "$GO"; fail=$((fail+1)); return
    fi
    printf '  [%s] %s\n' "$name" "$label"
    measure extC      "$B/${name}_extc" "${args[@]}"; local ue=$M_US re=$M_RSS oe=$M_OUT
    measure "C++ STL" "$B/${name}_cpp"  "${args[@]}"; local uc=$M_US rc=$M_RSS oc=$M_OUT
    measure Go        "$B/${name}_go"   "${args[@]}"; local ug=$M_US rg=$M_RSS og=$M_OUT
    if [ "$oe" != "$oc" ] || [ "$oe" != "$og" ]; then
        printf '    FAIL 校验和不同：extC「%s」/ C++「%s」/ Go「%s」\n' "$oe" "$oc" "$og"
        fail=$((fail+1)); return
    fi
    pass=$((pass+1))
    printf '%s\t%s\t%s\t%s\n' "$name" "$label" extC "$ue" >> "$RAW"
    printf '%s\t%s\t%s\t%s\n' "$name" "$label" C++  "$uc" >> "$RAW"
    printf '%s\t%s\t%s\t%s\n' "$name" "$label" Go   "$ug" >> "$RAW"
    printf '| %s | %s | %s | %s | %s | %s | `%s` |\n' \
        "$label" "$(cell "$ue" "$re" "$OPS")" "$(cell "$uc" "$rc" "$OPS")" "$(cell "$ug" "$rg" "$OPS")" \
        "$(ratio "$ue" "$uc")" "$(ratio "$ue" "$ug")" "$oe" >> "$ROWS"
}

printf '== 构建三套实现 ==\n'
if [ ! -x "$EXTC" ]; then printf '  缺 %s，先 make\n' "$EXTC"; make -s -j"$(nproc)" || exit 1; fi
printf '  C++ ：%s\n' "$("$CXX" --version | head -1)"
printf '  Go  ：%s\n' "$("$GO" version)"
printf '%s\n' "----------------------------------------------------------------"

TABLE_HEAD='| 规模 | extC | C++ STL | Go | extC/C++ | extC/Go | 校验和 |
|---|---|---|---|---|---|---|'

printf '\n== 场景 A：会话 / 连接表（hashMapI64 · 60%% 查 / 20%% 建更 / 20%% 删）==\n'
ROWS=$(mktemp); printf '%s\n' "$TABLE_HEAD" > "$ROWS"
p0=$pass
for a in $A_ACTIVE; do run_one session "active=$a · ops=$((30*a))" $((30*a)) $((30*a)) "$a"; done
A_ROWS=$(cat "$ROWS"); A_PASS=$((pass - p0)); rm -f "$ROWS"
printf '  通过 %d 项\n' "$A_PASS"

printf '\n== 场景 B：路由表（有序表 · 插入 / 前驱查找 / 区间扫描 / 删除）==\n'
ROWS=$(mktemp); printf '%s\n' "$TABLE_HEAD" > "$ROWS"
p0=$pass
for n in $B_OPS; do run_one route "ops=$n" "$n" "$n"; done
B_ROWS=$(cat "$ROWS"); B_PASS=$((pass - p0)); rm -f "$ROWS"
printf '  通过 %d 项\n' "$B_PASS"

printf '\n== 场景 C：请求日志 / 动态缓冲（追加 / 查找 / 截断）==\n'
ROWS=$(mktemp); printf '%s\n' "$TABLE_HEAD" > "$ROWS"
p0=$pass
for n in $C_OPS; do run_one log "ops=$n" "$n" "$n"; done
C_ROWS=$(cat "$ROWS"); C_PASS=$((pass - p0)); rm -f "$ROWS"
printf '  通过 %d 项\n' "$C_PASS"

printf '\n通过 %d 项，失败 %d 项\n' "$pass" "$fail"
rm -f "$OUTF"

# ---------- 汇总（几何平均） ----------
SUMMARY=$(awk -F'\t' '
{ k=$1; l=$3; t=$4+0; if (t>0) { s[k"|"l]+=log(t); n[k"|"l]++ } }
END {
  split("session route log", ks, " ")
  for (i=1;i<=3;i++) { k=ks[i]
    if (n[k"|extC"]>0 && n[k"|C++"]>0 && n[k"|Go"]>0) {
      ge=exp(s[k"|extC"]/n[k"|extC"]); gc=exp(s[k"|C++"]/n[k"|C++"]); gg=exp(s[k"|Go"]/n[k"|Go"])
      printf "| %s | %.4f s | %.4f s | %.4f s | %.2f× | %.2f× |\n", k, ge/1e6, gc/1e6, gg/1e6, gc/ge, gg/ge
    }
  }
}' "$RAW")
rm -f "$RAW"

# ---------- 结果落盘（这张表由本脚本生成，别手改） ----------
# `WRITE_RESULTS=0` 时不落盘：缩小规模的回归跑（check.sh 那一节）不能把完整规模那张表冲掉。
if [ "${WRITE_RESULTS:-1}" = 1 ]; then
{
    printf '# bench/app —— 综合应用场景横评（extC STL vs C++ STL vs Go）\n\n'
    printf '> **自动生成**（`bash bench/app/run.sh`），别手改。\n'
    printf '> 口径：三种语言**同算法同参数**，跑同一串操作，打印出来的校验和**逐字节相同**才算数；\n'
    printf '> 时间与峰值 RSS 都是 `best of %s`（时间用 bash 内建 `EPOCHREALTIME`，每样本重复 K 次凑到约 0.2 s）。\n\n' "$RUNS"
    printf '机器：%s · kernel %s\n\n' "$MACHINE" "$KERNEL"
    printf '## 三套实现与编译口径\n\n'
    printf '| | extC | C++ | Go |\n|---|---|---|---|\n'
    printf '| 编译 | `extc` 生成 C，`cc -O2 -std=c11 -fwrapv` | `g++ -O2 -std=c++17` | `go build`（默认） |\n'
    printf '| 会话表 | `stl::hashMapI64<session>` | `std::unordered_map` | `map[int64]Session` |\n'
    printf '| 路由表 | `stl::map<i64,i32>`（B+ 树） | `std::map`（红黑树） | `map` + 排序切片 + 二分 |\n'
    printf '| 日志缓冲 | `stl::string`（池底） | `std::string` | `[]byte` |\n\n'
    printf '`-fwrapv` 不是「多加的旗子」：extC 把有符号溢出定义成绕回（`docs/MANUAL.md` §8），\n'
    printf '编译器驱动自己也是这么编的（`src/main.c`）⇒ 这是生成的 C 的既定编译契约。\n\n'
    printf '## 场景 A：会话 / 连接表\n\n'
    printf '60%% 查 / 20%% 建更 / 20%% 删，活跃集固定；键是自增连接 id（`i %% active`），相位 `i %% 977`\n'
    printf '（素数，与 2 的幂互质 ⇒ 同一个 key 的一生里三种操作交错）。**这一格量的是哈希表在\n'
    printf '「高频删」下的退化**：墓碑密度一高，miss 查询就开始跨墓碑 —— 2026-09-26 那个 256 倍\n'
    printf '修复（`hashI64` 加乘法混合 + 墓碑密度触发重建）就是这条曲线里抓出来的。\n\n'
    printf '%s\n\n' "$A_ROWS"
    printf '## 场景 B：路由表（前驱查找 + 区间扫描）\n\n'
    printf '键是 /24 前缀，值是下一跳。四个阶段：插入/更新 ops 条 → **前驱查找**（最长前缀匹配的\n'
    printf '实质：最后一个 ≤ q 的前缀）ops 次 → **区间扫描** ops/4 次（数一个 /16 段里有多少条）→\n'
    printf '删除 ops/2 条。数据结构**本来就是三种**（B+ 树 / 红黑树 / map+排序切片）—— 这是\n'
    printf '「各语言里最自然的那个有序表」的对比，不是同一个结构的对比。表里的 `ns/op` 分母是\n'
    printf '**真实操作数**（插入 + 查找 + 扫描 + 删除 = 2.75 × ops），不是规模参数。\n\n'
    printf '%s\n\n' "$B_ROWS"
    printf '## 场景 C：请求日志 / 动态缓冲\n\n'
    printf '定长 24 字节的记录追加进去；每 512 轮在缓冲里 `find` 上一条记录的前 4 字节\n'
    printf '（它只可能出现在**末尾** ⇒ find 扫满整条缓冲）；缓冲到 16 KB 就**截断**掉最老的一半。\n'
    printf 'extC 的 `string` 没有 `erase`，截断是**字节循环**（`dropFront`：缓冲区字段是公开的\n'
    printf '`mut slice<u8>`）；C++ 用 `erase(0,n)`（memmove）、Go 用 `copy` + 重新切片 ——\n'
    printf '逻辑同一件事，机制各语言不同，这一格本来就是「缺什么就付什么」。\n\n'
    printf '%s\n\n' "$C_ROWS"
    printf '## 汇总（三个规模的时间几何平均）\n\n'
    printf '| 场景 | extC | C++ STL | Go | C++/extC | Go/extC |\n|---|---|---|---|---|---|\n'
    printf '%s\n\n' "$SUMMARY"
    cat <<'EOF'
## 结论与分析（2026-09-26）

> 下面引的**精确数字**来自**独立测量**（callgrind、消融、以及"手写 C 用同一份四列布局"的
> 对照），不随上面那张表 3~5% 的跑动漂移；表里能读到的量级一律写「见上表」。

**A 会话表 —— 这一格 2026-09-26 被修掉了一大块。** 修前 extC 在 1M 活跃集上是 C++ 的近 9 倍、
还比 Go 慢 15%；修后是 C++ 的 **7.5 倍**、**在最大规模上反超 Go（0.98×）**（三个规模的几何
平均仍输 10% —— 小规模那两档 Go 更快）。

**先看这 7.5 倍花在哪**（同机同批交错测，比值稳定）：

| | ns/op | |
|---|---|---|
| extC 生成代码（修后） | 41.1 | |
| **手写 C · 同样的四列 SoA / 同样的 hash / 同样的负载** | **24.8** | ⇒ 生成代码这一半 ≈ 16 ns/op |
| 手写 C · cap 从 2× 降到 1×（表小一半） | 20.7 | 表尺寸只值 17% |
| 手写 C · keys+slot 合并成 AoS（三列） | 25.8 | **更慢** ⇒ "少碰一条 cache line"不是答案 |
| C++ `unordered_map` | 5.7 | 剩下的 4.4× 在数据结构/算法 |

⇒ **两条账各占一半，修法完全不同**：

1. **生成代码那一半 —— 这一轮做掉了（zone 钩子，端到端 14.5%）**。
   `calleeMakesPool` 第一句对 `@poolObject` 类型**一律**返回真（它回答的是「这个类型**拥有**
   池」，逃逸/提权要的），于是 `hashMap::get` 也被当成「会建池」⇒ 循环体每轮压/弹一次 zone
   （callgrind 里 `zoneEnter` 9.87% + `zoneLeaveTo` 13.91% = **23.8% 指令**；只删生成物里那两行
   的消融值是 **25.3%**）。只拆谓词没用：`#57` 让泛型体里 `k.hash()` 这类**对类型参数的协议
   方法**在节点上**不写 func**，而模板与实例**共用同一个 FuncDef** ⇒ 模板轮一标真，实例轮的
   精确解析就永远没机会生效。修法 = 拆谓词（`calleeCreatesPool`）+ 闭包跳过泛型模板轮、在实例
   轮按实参代入解析（`runMethodCheck` 同一套）+ codegen 侧装解析钩子。判据
   `tests/pool/rt_zone_ondemand.extc` 专门加了调 `hashMapI64::put/get/remove` 的循环体
   （**旧编译器 3 次 zoneEnter、新的 2 次** ⇒ 判据有牙）。详见 DEVLOG 2026-09-26（第十一段续四）。
2. **数据结构那一半 —— 没动，而且不该"随手改布局"**。四列 SoA 对 C++ 的「桶数组 + 节点」值
   **4.4×**，但消融显示它不是「每列各一次 cache miss」那种可以机械消掉的账：**合并两列更慢**
   （25.8 对 24.8）、**表减半只拿回 17%**。真要动就是**换设计**（值内联进桶 / AoS 节点 /
   换探测策略），得带着 `bench/app` 当尺子单独一轮。
   指令条数那一层仍是 C++ 的 **2.1×**（callgrind **223 对 105 条/操作**）—— 每一条切片下标都带
   边界检查，这是 extC 安全承诺的**明账**。cache-sim（2M 次操作）：D1 miss **4.05M 对 1.15M**。

   顺带**排除两个候选**（在**修前**那一版上量的，结论与 zone 无关）：**按值返回 `?session`** ——
   把 32 字节的会话换成 8 字节重跑（校验和不变），extC 47.35 对 49.55 ns/op，几乎不动 ⇒ 不是
   拷贝的账；**墓碑重建** —— 把触发线整个关掉，49.47 对 49.62 ns/op（0.16 ns/op）⇒ 也不是它的账。

**B 路由表**：extC **赢 C++**（200 万条时约 1.9 倍，见上表）—— 与 `bench/stl` 里
「B+ 树 vs 红黑树」同向（cache miss 少、遍历是叶子链线性扫描）。Go 最快，但它
用的**不是**同一棵树：`map` 负责点操作、排序切片负责区间 —— 这是「各语言最自然的做法」的
对比，不是同一个数据结构的对比。

**C 日志缓冲**：extC 稳定在 C++/Go 的 **1.9~2.0×**。两处都是明账：`string` 没有 `erase` /
memmove 原语，截断是**字节循环**（C++ 一条 `erase`、Go 一条 `copy`）；`find` 是朴素扫描。
这一格是「缺什么就付什么」的直接体现。

EOF
    printf '## 读这张表前要知道的三件事\n\n'
    printf '1. **extC 的容器是有界检查的**：切片索引越界会带源位置 trap，而 C++/Go 那边是裸索引。\n'
    printf '   这一项算 extC 的明账（也是它安全承诺的价格）。\n'
    printf '2. **RSS 不是同一种口径**：Go 运行时（GC 预留、栈池）本身就占几十 MB；extC 与 C++ 更接近。\n'
    printf '   看**比值**，不要读单个数。\n'
    printf '3. **WSL2 + 笔记本 CPU 有 3~5%% 波动**：看量级与排序，不要读最后一位小数。\n\n'
    printf '校验和：场景 A %d 项 · B %d 项 · C %d 项，**合计 %d 项通过 / %d 项失败**。\n' \
        "$A_PASS" "$B_PASS" "$C_PASS" "$pass" "$fail"
} > bench/app/RESULTS.md
printf '结果写入 bench/app/RESULTS.md\n'
else
    printf '（WRITE_RESULTS=0：跳过 RESULTS.md，避免缩小规模的回归跑冲掉完整表）\n'
fi

[ "$fail" = 0 ]
