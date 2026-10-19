#!/usr/bin/env bash
# tests/reclaim-perf/gate.sh —— 回收性能判据（两次"先红后绿"都钉在这里）。
#
# 判据：开关开的 `cyclic_pressure` 在固定 N=200000 下，callgrind 的**总指令数 ≤ LIMIT**。
# 判据线随落地过一次就**收紧一次**（与 known-bad 棘轮同一条规矩：优化拿到的收益不许被付回）。
#
#   改动前实测（`build/extc` md5 5e51d77f…）：1 169 388 408 Ir
#   ① 分配点入口守卫落地（根表只在真要扫时物化）：510 475 244 Ir
#      —— 当时把线定在 701 633 044（改动前 × 0.6，解释见 README §3.2 的 56.1%）。
#   ② `owns` 登记表查询 O(1) 化落地（直接映射 + 校验，未命中回落二分）：404 358 444 Ir
#      —— 线收到 425 000 000。
#   ③ 赋值路径每个指针只查一次登记表（`storeRefOwned` 复用 slab；`ownSlab` 命中路径
#      省掉冗余比对，但保留"映射格非空"那一项）：339 186 860 Ir
#   ⑩ 删掉 `ExtcRcSlab.used`（只写不读的死字段）：195 517 873 Ir。
#   ⑨ 压力阈值缓存进区域（只在 live/floor/scale 变时重算）：195 939 957 Ir。
#   ⑧ 默认改成**无计数**（`EXTC_RC_NOCOUNT=1` 成为默认）：198 497 440 Ir（−24.6 %）。
#   ⑦ `extc_rc_storeRefOwned` 改成 `static inline`（只给 inline、不给 always_inline）：263 453 680 Ir。
#   ⑥ `regionOf` 结果做成函数级局部量（区域在帧内是不变量；协程不 hoist）：266 437 299 Ir。
#   ⑤ `rsIncN==0` 时跳过"撤跨区边"的走查（进程里一次跨区登记都没发生过 ⇒ 无物可撤）：
#      本形状 308 485 778 → **279 224 856 Ir**；同一守卫让 `rebuild` −10.0 %、`bt` −5.3 %。
#   ④ 热路径小助手一律 `static inline`（`ownSlab` 一次运行被调用上亿次，调用/返回本身就是一笔）：
#      **308 485 778 Ir** ⇒ 现行 LIMIT = 324 000 000（= 实测 × 1.05）。
#      注：**只给 `inline`，不给 `always_inline`** —— 强制内联量过：`bt` −14.2 % 但 `rebuild`
#      反而 +0.5 %（代码变大）；让编译器按代价模型决定，三个形状一起降
#      （`bt` −10.7 %、`rebuild` −9.9 %、本形状 −9.1 %）。
#   ⑪ 槽元数据瘦身（`task-19`）：每槽类型描述符**指针**（8 B）→ 16 位**类型号**（2 B）+ 每 slab
#      小类型表；槽步长 2 的幂 → 8 字节取整（由指针找槽号从一次移位变成一次乘倒数右移）。
#      本形状 **154 214 566 Ir** ⇒ 线收到 161 930 000（= 实测 × 1.05；中间态"只换类型号"是
#      165 100 449 Ir，被这一项的缓存收益盖过去了）。
#   ⑫ 标记/分配路径里重复计算的合并：每 slab 缓存类型号（lastTd/lastTid）、word/bit 一次算好
#      复用、同块子对象复用槽号、shade 拆「内联壳 + 冷半段」（标记外的赋值连调用都不产生）、
#      预取折进批量填充循环：本形状 **130 453 179 Ir** ⇒ 线收到 136 975 833（= 实测 × 1.05）。
#   ⑬ 批量填充先一次算好上限（批宽/预算/配额/栈深取 min），不再每弹一个重判四个条件、
#      各减一次预算和配额（批平均不到 2 个对象，固定开销摊不薄）：本形状 **128 473 126 Ir**
#      ⇒ 线收到 134 896 782（= 实测 × 1.05）。
#   ⑭ 触发判据 `shouldSweep` / 起轮信用 `roundOk` 标 `inline`（生成代码每个定点释放点都要问一次
#      ⇒ 调用/返回本身是一笔）：本形状 **126 954 696 Ir** ⇒ 线收到 133 302 430（= 实测 × 1.05）。
#   ⑮ 子对象循环里的同块去重不再验槽首（只按「包含该地址的槽」查 occ/mark 位；非槽首指针在
#      弹栈端本来就会被 ownSlab 判成非对象）：本形状 **125 487 946 Ir** ⇒ 线收到 131 762 343（= 实测 × 1.05）。
#   ⑯ alloc 不再逐次重验缓存块的尺寸类（`allocHot` 只有本函数一个写入点，s 只来自
#      classSlot/slabNew ⇒ 同尺寸类是不变式）：本形状 **125 022 751 Ir** ⇒ 线收到 131 273 889（= 实测 × 1.05）。
#   ⑰ `ownSlab` 不再显式挡空指针（`p == 0 ⇒ base == 0`，映射格/登记表/超大块表里都没有键 0
#      ⇒ 每一层探测都落空、返回 0 与显式检查相同）：本形状 **124 032 889 Ir** ⇒ 线收到 130 234 533（= 实测 × 1.05）。
#   ⑱ 插入屏障先查一张「已经黑过」的 1024 格直接映射缓存（指针 + 代际号），命中就不进慢路径、
#      连 ownSlab 的校验都省掉；失效靠四条清 mark 的路径各自代际++：本形状 **116 729 760 Ir**
#      ⇒ 线收到 122 566 248（= 实测 × 1.05）。
#   ⑲ 分配体在每个分配点内联（`always_inline`，`-DEXTC_RC_ALLOC_INLINE=0` 可关）：省掉每分配一次
#      调用 + 进出栈，编译器还能把常量尺寸折叠掉：本形状 **112 211 624 Ir** ⇒ 线收到 117 822 205
#      （= 实测 × 1.05）。`gate-mark-locality` 同口径下 P5 也降（Ir 215.7 M → 186.1 M，失手位数不动）。
#   ⑳ 标记步专用的 `ownSlab` 副本（`extc_rc_ownSlabMark`：函数体与原件逐字相同、只加
#      `always_inline`，只被 `markStep` 调用）。全站点强制内联会让 `cross_region` 这类形状慢两成，
#      只喂最热的那个调用点：本形状 **110 375 684 Ir** ⇒ 线收到 115 894 468（= 实测 × 1.05）。
#      两份体的逐字一致由 `check_ownslab_body.py` 守（`--selftest` 四条注入）。
#   ㉑ 槽大小计算 `extc_rc_slotSize` 强制内联：常量尺寸的分配点上槽步长变成常量，
#      `memset(p, 0, ss)` 才能展开成定长内联存储（不再每分配一次库调用）；同块子对象的去重
#      也不再查 `occ` 位（`mark` 位立着就是已黑）：本形状 **102 633 660 Ir** ⇒ 线收到
#      **107 765 343**（= 实测 × 1.05）。
#   ㉒ 批量填充把标记栈头/栈深提到局部（`void **_stk = r->mstk; size_t _sp = r->msp;`，弹完整批
#      再 `r->msp = _sp` 压回）：循环里不反复读 `r`；每对象初始化一次的 `initRef`/`initRefNoHold`
#      也强制内联（-O1 不内联就每个初始化点多一次调用+返回，自赋值常量点还能被折掉）：本形状
#      **98 924 997 Ir** ⇒ 线收到 **103 871 247**（= 实测 × 1.05）。
#   ㉓ 清扫按**字**批量释放（`extc_rc_freeSlotWord`）：一个死字里逐槽只做链头插，占用位/登记位、
#      字节账与 freedSlots、avail 回挂都按整字一次；`unlinkChildren` 与 rscnt 清零仍逐槽，
#      但「一次登记都没发生过」的分支提到字外（默认档 rsIncN 恒 0 ⇒ 整段不执行）：本形状
#      **94 587 496 Ir** ⇒ 线收到 **99 316 871**（= 实测 × 1.05）。
#   ㉔ 待扫对象少时单个处理（`EXTC_RC_MARK_SHALLOW`，默认 8）：栈深 ≤ 阈值就直接弹一个、扫一个，
#      不装批 —— 批装（数组/上限 min/填充循环/扫批循环）在栈深 ≈1 时是纯固定开销，预取提前量也
#      换不来东西；对象处理体移进文件级宏 `EXTC_RC_MARK_SCAN` 供两条路共用。同块子对象去重先查
#      `mark` 位（已黑最省一步），`occ` 位只作陈旧指针的第二道。本形状 **89 675 091 Ir** ⇒ 线收到
#      **94 158 845**（= 实测 × 1.05）。
#   ㉕ 标记步两笔代码形状：分配路径的推进计数器 `extc_rc_markAdvance` 强制内联（每个分派点都过它，
#      -O1 不跨调用点内联 ⇒ 每趟一次调用/返回）；扫描体里子对象循环改**指针步进**（`_op`/`_oend`，
#      不再每次重读 `t->nptr` 的边界与 `t->off[k]` 的基址）。本形状 **88 852 320 Ir** ⇒ 线收到
#      **93 294 936**（= 实测 × 1.05）。`cross_region` 形状 +0.25%（≈ +1 Ir/分配）是内联后的
#      codegen 抖动，归因到 `markAdvance` 内联那一半（单独量只内联 ≈ +19 950 Ir）。
#   ㉖ 浅栈阈值 8 → 64（去重更早生效）＋ 分配入口的 `extc_rc_why = "alloc"` 移进清扫分支（慢步归因
#      只在「分配顺手推进清扫」这一支需要，标记推进自己写 "advance"、finish/collect 各写各的 ⇒ 每分配
#      省一条存）。阈值是「去重收益 vs 预取收益」的折中，按形状实测取值（括号内为本形状 Ir）：
#      8（88 852 305）→ 32（87 922 567）→ **64（87 823 666）** → 128（87 910 790）→ 256（87 901 977）；
#      P5 这类深栈形状几乎永远走批路径，D1mr/DLmr 逐位不变（628 009 / 146 467）。两笔合计本形状
#      **87 360 471 Ir** ⇒ 线收到 **91 728 908**（= gate 自量 87 360 865 × 1.05）。
#   ㉗ 分配点的收尾守卫上提：`extc_rc_finish` 的首行就是「marking 且 seedsDone 且栈空」才真收尾，
#      而标记期**每分配**都调它一次 ⇒ 绝大多数调用只是调用+返回+早退。alloc 里就地判掉那两条
#      （已在 `if (r->marking)` 分支内），finish 内部的守卫保留作纵深防御。本形状
#      **86 646 193 Ir** ⇒ 线收到 **90 978 472**（= gate 自量 86 646 164 × 1.05）。
#   ㉘ 浅栈快路一次把浅栈排空（do/while 外壳）：批装在栈深≈1 时纯亏，所以浅栈弹一个扫一个；
#      原来是「弹一个 → 回外层 while → 重判 while/预算/浅栈条件 → 再弹」，排空循环把这重判摊到一次。
#      本形状 **86 174 849 Ir** ⇒ 线收到 **90 483 570**（= gate 自量 86 174 829 × 1.05）。注意：早先
#      按「调用行 Ir / 调用次数」对比浅栈 79.4 与批扫 56.2 归因到 codegen，per-path 插桩否定了这个
#      读法 —— 批扫 147 128 次里只有 44.1% 首发、56% 是重复（扫到 mark 位即退 ≈41 Ir），浅栈 99.1%
#      首发；两站均值不可直接比。排空循环自身的收益 ≈3.5 Ir × 135 760 ≈ 0.47 M，与实测吻合。
#   ㉙ 插入屏障的「已经黑过」缓存改成**旋转散列**（每格 8 字节只存地址，格号 = 地址>>3 + 代际号）：
#      老结构每格两字（指针 + 代际号），查询要两次装载；格号把代际号**旋进下标**后一次装载定命中，
#      且仍然精确（同格同指针 ⇒ 同地址；格号还要求代际号同余 ⇒ 同一窗口内只可能同代）。每 2^11 代
#      回绕前整表清零（16 KiB / 2^11 代 = 8 B/代），跨窗口的旧记录不可能复活。四处清 mark 的路径
#      统一调 `extc_rc_shadeNextGen()`。本形状 **84 575 386 Ir** ⇒ 线收到 **88 804 155**
#      （= 实测 × 1.05）。P5 同时变好：Ir 171 549 794 → 170 538 858（−0.59%）、D1mr 628 200 →
#      618 225（判据线 700 000 余量更厚）、DLmr 146 271。被否：xor-key 变体（key = 地址 ^ 代际号）
#      指令数略低但存在理论假命中（假阳性 = 白对象当黑）⇒ 一律不用。
#   ㉚ rc 类型描述符改**按布局去重**（src/back/codegen.c）：以前每个分配点各发一份 `ExtcRcType`，
#      同一个结构体的多个 `new` 站点就发多份指针不同的同构描述符 —— 每块 slab 的小类型表按指针
#      相等去重，于是表里堆同构项、`lastTd` 缓存因指针交替而永远失手。改成文件作用域、键 = 偏移表
#      文本后本形状 **83 837 253 Ir** ⇒ 线收到 **88 029 115**（= 实测 × 1.05）。同批 `extc_rc_regionOf`
#      改 `static inline`（调用的进入/返回占它三成成本；cycleratio 413 905 535 → 369 404 623 Ir，−10.75%，
#      本形状无变化）。判据 `tools/check_rc_desc_dedup.py` / `gate-rc-desc-dedup`。
#
# 口径与 `run.sh` 一致：pinned `extc` → `EXTC_RECLAIM=1` 生成 → `cc -std=c11 -fwrapv -O1`
# → `valgrind --tool=callgrind` 的 `summary:` 行。只写自己的临时目录，`src/` 一个字节不动。
#
# 用法：tests/reclaim-perf/gate.sh        退出码 0 = 绿（判据成立）
#   EXTC=...    指定编译器（默认 <repo>/build/extc）
#   N=200000    callgrind 的轮数（LIMIT 只对默认 N=200000 成立，改 N 请同时改 LIMIT）
#   LIMIT=...   覆盖判据线（调试用）
#
# 缺 valgrind：打印 `  --   跳过：…` 后**只跑**下面那半（槽号索引，判据不依赖 valgrind），退出码 0
#   —— `tools/check.py` 把这一项记 `skip`、不计数（判据没验就不许算通过；CI 装了 valgrind ⇒ 不许跳过）。
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
SRC=${SRC:-$ROOT/tests/reclaim-shapes/cyclic_pressure.extc}
EXTC=${EXTC:-$ROOT/build/extc}
CC=${CC:-cc}
N=${N:-200000}
LIMIT=${LIMIT:-88029115}          # 描述符去重 + regionOf 内联后实测 83837253 × 1.05（棘轮只许往下；上一线 88804155）

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skip() { printf '  --   跳过：%s\n' "$1"; }   # 缺件 ⇒ 明说"没验"（check.py 认这一行 ⇒ 记 skip、不计数）
VG=$(command -v valgrind || true)

echo "== 回收性能判据（EXTC_RECLAIM=1，N=$N）=="
echo "  形状   $SRC"
echo "  编译   EXTC_RECLAIM=1 extc -I <repo>/stdlib → cc -std=c11 -fwrapv -O1"
echo "  判据   总指令数 ≤ $LIMIT（现行线 = 最近一次实测 × 1.05；棘轮只许往下）"

# 固定编译器：`extc` 按自身所在目录找 stdlib ⇒ 拷到别处要显式 -I。
cp "$EXTC" "$TMP/extc"; chmod +x "$TMP/extc"

if ! EXTC_RECLAIM=1 "$TMP/extc" -I "$ROOT/stdlib" "$SRC" -o "$TMP/on.c" 2>"$TMP/gen.log"; then
    bad "生成失败"; sed 's/^/    /' "$TMP/gen.log"; exit 1
fi
if ! $CC -std=c11 -fwrapv -O1 -o "$TMP/on" "$TMP/on.c" 2>"$TMP/cc.log"; then
    bad "生成物编译失败"; sed 's/^/    /' "$TMP/cc.log"; exit 1
fi

if [ -z "$VG" ]; then
    skip "没有 valgrind（本机缺 callgrind）⇒ 本形状的指令数棘轮这轮没验（下面的槽号索引判据照跑）"
else
    if ! valgrind --tool=callgrind --callgrind-out-file="$TMP/cg.on" "$TMP/on" "$N" \
            >"$TMP/on.out" 2>"$TMP/on.vg"; then
        bad "callgrind 那跑失败"; tail -5 "$TMP/on.vg" | sed 's/^/    /'; exit 1
    fi
    IR=$(grep '^summary:' "$TMP/cg.on" | awk '{print $2}')
    if [ -z "${IR:-}" ]; then bad "callgrind 输出没有 summary 行"; exit 1; fi

    awk -v ir="$IR" -v lim="$LIMIT" -v base=1169388408 \
        'BEGIN { printf "  实测   总指令数 %d Ir（%.1f%% of 改动前）\n", ir, 100*ir/base }'

    if [ "$IR" -le "$LIMIT" ]; then
        awk -v ir="$IR" -v lim="$LIMIT" -v base=1169388408 \
            'BEGIN { printf "  ok   回收性能成立：%d ≤ %d（相对改动前 −%.1f%%）\n", ir, lim, 100*(base-ir)/base }'
    else
        bad "指令数 $IR > $LIMIT（回收开销被付回了：入口守卫或守卫 O(1) 化的收益丢了）"
        exit 1
    fi
fi

# ---- 槽号索引：域内穷举（健全性判据，不是性能判据）----------------------------------
# 槽步长按 8 字节取整 ⇒ "由指针找槽号"是乘倒数右移；算错一格就会把合法对象判成"不是本运行期的
# 槽" ⇒ 标记漏走它、清扫当垃圾放掉。判据用**发射出来的**那份运行期文本，不另抄实现。
echo "== 槽号索引（域内穷举：16..1 MiB 的每个 8 的倍数 × 全部合法槽号）=="
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -O1 -g -I "$ROOT/src/base" -I "$ROOT/src/back" \
        "$ROOT/tests/reclaim/emit_main.c" "$ROOT/src/back/reclaim.c" "$ROOT/src/base/base.c" \
        -o "$TMP/emit" 2>"$TMP/emit.log"; then
    bad "发射器编译失败"; sed 's/^/    /' "$TMP/emit.log"; exit 1
fi
if ! "$TMP/emit" > "$TMP/extc_reclaim_runtime.inc" 2>"$TMP/emit.err" \
        || [ ! -s "$TMP/extc_reclaim_runtime.inc" ]; then
    bad "发射运行期文本失败"; sed 's/^/    /' "$TMP/emit.err"; exit 1
fi
if ! $CC -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -I "$TMP" "$DIR/slotindex.c" \
        -o "$TMP/slotindex" 2>"$TMP/slotindex.log"; then
    bad "slotindex 编译失败（-Werror）"; sed 's/^/    /' "$TMP/slotindex.log"; exit 1
fi
if out=$("$TMP/slotindex"); then
    printf '%s\n' "$out" | sed 's/^/  /'
    ok "槽号索引成立：域内穷举全中"
else
    printf '%s\n' "$out" | sed 's/^/    /'
    bad "槽号索引：域内穷举有不符（乘倒数右移不再等于整除 ⇒ 会漏认合法槽）"
    exit 1
fi
exit 0
