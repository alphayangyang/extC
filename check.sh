#!/usr/bin/env bash
# extC 全套自检：**一条命令**跑完 测试 + 攻击库 + 四语言横评
#   用法： ./check.sh          （全部）
#          ./check.sh quick    （只跑测试 + 攻击库，跳过基准）
set -u
cd "$(dirname "$0")"
# 非交互跑必须关掉 stdin：stdin 若指向已撤销的 pty，用例读终端会被 SIGTTIN **停住**
# （进程状态 `T`），`timeout` 管不到已停的进程 ⇒ 整个自检无限等下去，比报错难查得多。
# IO 节不需要真 tty（关掉 stdin 后 25 节全绿即证），所以这里一次关掉、每个子进程都继承。
exec </dev/null
pass=0; fail=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }

# ⚠️ 每一节都套 `timeout 600`：**编译器挂住 = 整个自检停在那里**，比报错难查得多
# （PLAN #59：泛型 fold 曾让层号求解器走 2^32 条路径 ⇒ 不返回、零诊断）。
# 超时 ⇒ 这一节直接判 FAIL（响亮），而不是让 check.sh 无限等下去 ✓
# 用例级的超时在 `tools/parrun.py` 里（每例 120 秒）✓
echo "== 构建 =="
if make -s >/tmp/extc-build.log 2>&1; then ok "make"; else bad "make"; cat /tmp/extc-build.log; exit 1; fi

echo "== 测试（例子 / 反例 / trap）=="
# ⚠️ 已知误拒：`examples/field-strong-update.extc`（见 `KNOWN-ISSUES.md`）——
# 它**只**许以"一条已记档的失败"出现；**多出任何一条别的失败就算红** ✗
# （理由：把已知项算进基线，但绝不掩盖新问题 ✓）
if out=$(timeout 600 ./tests/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
else
    # ⚠️ 输出带 ANSI 颜色码 ⇒ 先剥掉再匹配（不剥的话 `^  FAIL` 一条都匹配不到 ✗ 踩过）
    plain=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
    # ⚠️ 白名单**现在是空的**（`field-strong-update` 的误拒 2026-09-23 已修 ✓）——
    # 机制留着，将来再有"已记档的失败"时按同样办法加一条，**不许**放宽成"忽略所有失败" ✗
    extra=$(printf '%s' "$plain" | grep '^  FAIL' || true)
    if [ -z "$extra" ]; then
        ok "$(echo "$out" | tail -1)（只差 field-strong-update —— 见 KNOWN-ISSUES.md ✓）"
    else bad "tests/run.sh"; echo "$out" | grep FAIL | head -5; fi
fi

echo "== 闸门·规模（任何输入都要给结论：不许 SIGSEGV / 不许挂住 —— 审计 §7 闸门④）=="
# 2026-09-30 P0 批次新增。基线在 tools/gate-scale-known-bad.txt：**修好的条目必须删掉**，
# 否则这条会红（棘轮只往一个方向转）。它挡的是"编译器自己崩/不返回"这一类。
if out=$(timeout 600 python3 tools/gate_scale.py 2>&1); then
    ok "$(printf '%s' "$out" | grep -m1 '^\[scale\]')"
else bad "tools/gate_scale.py（规模用例变红）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -12; fi

echo "== 断言版全量（EXTC_DBG=1：断言/兜底一旦被走到就 abort —— src/dbg.h）=="
# 关掉时每个断言点只花一次分支；打开后同一只二进制即可当断言版用（不需要第二套构建）。
# 语义：断言与"按设计不该走到的兜底"被走到 = 信号 ⇒ 这一节只在**没有**任何 abort 时才是绿。
if out=$(EXTC_DBG=1 timeout 1800 ./tests/run.sh 2>&1); then
    esc=$(EXTC_DBG=1 timeout 600 python3 tools/gate_escape.py 2>&1 | tail -1)
    if printf '%s' "$out" | tail -1 | grep -q "失败 0" && printf '%s' "$esc" | grep -q "^\[escape\] ok"; then
        notes=$(printf '%s' "$out" | grep -ac "\[note\]" || true)
        ok "断言版：$(printf '%s' "$out" | tail -1)；闸门⑤ ok；断言/兜底 0 命中；已知活路径 note×$notes" 
    else
        bad "断言版跑出问题：$(printf '%s' "$esc" | tail -1)"
        printf '%s\n' "$out" | grep -a "assert\]\|fallback\]" | head -5
    fi
else
    bad "断言版 tests/run.sh 变红（断言或兜底被走到 ⇒ 见上面的 [assert]/[fallback]）"
    printf '%s\n' "$out" | grep -a "assert\]\|fallback\]" | head -5
fi

echo "== 闸门·逃逸健全性（语料必须被拒绝；基线 = R1/R3 施工单 —— 审计 §7 闸门⑤）=="
# 语料在 tools/escape-corpus/：每份都是「记账比真实情况小」的洞的最小复现 —— 现在被接受，
# 生成物在 ASan 下真的 UAF / SEGV / 编不过。control_* 是孪生对照（已能正确拒绝），永远不许进基线。
if out=$(timeout 600 python3 tools/gate_escape.py 2>&1); then
    ok "$(printf '%s' "$out" | grep -m1 '^\[escape\]')"
else bad "tools/gate_escape.py（逃逸健全性语料变红）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -12; fi

echo "== 闸门·实例接线（每个被发出的实例必须调对它该调的那个 —— 生成物结构判据）=="
# 挡的是"一个调用点被多实例共享 ⇒ 只有最后一次重指留下"：生成物**仍是合法 C**、跑出来也对，
# 所以闸门①②③都看不见它。探针在 tools/callsite-corpus/，判据是每份的 `.expect`
# （要求哪些实例被发出、哪个实例该调谁）；基线 = 施工单，期望成立就删那一行。
if out=$(timeout 600 python3 tools/gate_callsite.py 2>&1); then
    ok "$(printf '%s' "$out" | grep -m1 '^\[callsite\]')"
else bad "tools/gate_callsite.py（实例接线变红）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -12; fi

echo "== 闸门·差分（同一段语义：extC 与等价 C 必须给出同一结论 —— 审计 §7 闸门③）=="
# 挡的是"静默算错"：浮点字面量、窄类型回绕、转换、求值次数、for+continue。
# 基线在 tools/gate-diff-known-bad.txt，同样只许缩小。
if out=$(timeout 600 python3 tools/gate_difffuzz.py 2>&1); then
    ok "$(printf '%s' "$out" | grep -m1 '^\[diff\]')"
else bad "tools/gate_difffuzz.py（差分用例变红）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -12; fi

echo "== arena（按块细化：150MB 上限下不许涨）=="
if out=$(timeout 600 ./tests/arena/run.sh 2>&1); then ok "$(echo "$out" | wc -l) 个用例"; else bad "tests/arena/run.sh"; echo "$out"; fi

echo "== 警告（该响的响 · 正例语料零误报 · \`-w\` 能关）=="
if out=$(timeout 600 ./tests/warnings/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（含正例语料零误报 ✓）"
else bad "tests/warnings/run.sh"; echo "$out"; fi

echo "== 泛型自由函数（PLAN #47：推导 / 显式实参 / 推迟的 T: ==）=="
if out=$(timeout 600 ./tests/generics/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 3 反例）"
else bad "tests/generics/run.sh"; echo "$out"; fi

echo "== 手册文体（正式书面语：无第一/第二人称、无口语、无反问）=="
if out=$(python3 tools/check_tone.py --gate 2>&1); then ok "$out"; else bad "手册文体新增了非正式写法"; echo "$out" | head -5; fi

echo "== 驱动开关页与 \`extc --help\` 一致（单一真源）=="
if out=$(python3 tools/gen_flags.py --check 2>&1); then ok "$out"; else bad "21-flags.md 已过期"; echo "  跑 python3 tools/gen_flags.py"; fi

echo "== 手册覆盖（驱动开关逐项 + 公开面棘轮）=="
if out=$(python3 tools/check_manual.py --gate 2>&1); then ok "$out"; else bad "手册覆盖出现新增欠账"; echo "$out" | head -6; fi

echo "== 公开面清单与源码一致（tools/manual-surface.txt）=="
if out=$(python3 tools/manual_surface.py --check 2>&1); then ok "$out"; else bad "公开面清单已过期"; echo "  跑 python3 tools/manual_surface.py"; fi

echo "== 手册 HTML 与 Markdown 一致（docs/manual/html 是生成物）=="
# 手册里的**完整示例**（带 fn main 的块）必须编得过：前四道闸门都不检查"话是不是真的"，
# 2026-09-27 的审计里就有三个示例编不过而没有任何闸门会红。有意的片段用
# `<!-- manual-example: skip -->` 显式跳过。
if out=$(timeout 600 python3 tools/check_manual_examples.py --gate 2>&1); then
    ok "$(printf '%s' "$out" | head -1)"
else
    bad "手册里有示例编不过（tools/check_manual_examples.py）"; printf '%s\n' "$out" | head -8
fi
if out=$(python3 tools/build_manual.py --check 2>&1); then ok "$out"; else bad "docs/manual/html 已过期"; echo "  跑 python3 tools/build_manual.py"; fi

echo "== stdlib/INDEX 与源码一致（impl 方法表不许漂）=="
if out=$(python3 tools/gen_index.py --check 2>&1); then ok "$out"; else bad "stdlib/INDEX 已过期"; echo "  跑 tools/gen_index.py 重新生成"; fi

echo "== 类型分支的穷尽性（新构造器不许静默走 default）=="
if out=$(python3 tools/check_tykind.py 2>&1); then
    ok "$(echo "$out" | grep -c '^') 行报告（5 处 kind-switch：1 处枚举齐全 + 4 处 default 带理由）"
else bad "tools/check_tykind.py"; echo "$out"; fi

echo "== impl 块（方法挂载点：内建标量也能挂 => hashMap<i64, V> 直接可用）=="
if out=$(timeout 600 ./tests/impl/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 8 反例项：含 coherence 重名与内建标量）"
else bad "tests/impl/run.sh"; echo "$out"; fi

echo "== dyn 阶段 1（构造即调用：派发经表、拒绝存储、生成物合同）=="
if out=$(python3 tools/check_walkers.py 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_walkers.py"; printf '%s\n' "$out" | sed 's/^/  /'; fi
# 计划接缝（X1）：codegen 只能经 src/plan.h 读"分析产物/编译计划"，直读即红。
if out=$(python3 tools/check_plan_seam.py 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_plan_seam.py"; printf '%s\n' "$out" | sed 's/^/  /'; fi
# AST 冻结（R1，解耦 P0）：parser 之外不许写 AST 节点成员；基线 = 施工单，只许减。
if out=$(python3 tools/check_ast_freeze.py 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_ast_freeze.py"; printf '%s\n' "$out" | sed 's/^/  /'; fi
# 只读边界（R3，解耦 P0）：不许 include 别的阶段的私有头（codegen→check_internal 是红的起点）。
if out=$(python3 tools/check_layering.py 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_layering.py"; printf '%s\n' "$out" | sed 's/^/  /'; fi
# 字段归属（X3）：同名 `tmpl` 属两个结构体（FuncDef / CallCheck）⇒ 按**声明所在结构体**认属主，
# 未定即红；顺带钉住"codegen 只经 planTemplate 读它"。这一节挡的是"按字段名改名/搬家"那类错。
if out=$(python3 tools/check_tmpl_owners.py --verify 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_tmpl_owners.py --verify"; printf '%s\n' "$out" | sed 's/^/  /'; fi
if out=$(timeout 120 python3 tools/check_switches.py 2>&1); then ok "$(echo "$out" | tail -1)"
else bad "tools/check_switches.py"; echo "$out" | head -8; fi
if out=$(python3 tools/check_concurrency_guards.py 2>&1); then ok "$(printf '%s' "$out" | tail -1)"
else bad "tools/check_concurrency_guards.py"; printf '%s\n' "$out" | sed 's/^/  /'; fi
if out=$(timeout 600 ./tests/coro/run.sh 2>&1); then ok "$(printf '%s' "$out" | grep -m1 '^通过')"
else bad "tests/coro/run.sh"; echo "$out"; fi
if out=$(timeout 600 ./tests/dyn/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（正例 + 拒绝存储/字段 + 派发经表 + 合同编译）"
else bad "tests/dyn/run.sh"; echo "$out"; fi

echo "== IO 第一块（定案 73：std::sys 原语 + std::io 库 —— 能从 stdin 读了）=="
if out=$(timeout 600 ./tests/io/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（stdin 读取 + 分层 + 流式：控制台 cin >> x · 文件 fin >> 整数 >> 一行 >> 一个字节 · cerr 与 stdout 分开）"
else bad "tests/io/run.sh"; echo "$out"; fi

echo "== extern! + 信任声明（定案 72：签字才放行 · 默认最保守）=="
if out=$(timeout 600 ./tests/extern/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 6 反例 · 指针过界：ref T / ref void / ?ref void）"
else bad "tests/extern/run.sh"; echo "$out"; fi

echo "== 链接通道（-l / -L / --ccflag / --pkg-config：extern! 的库名从此真的参与链接）=="
if out=$(timeout 600 ./tests/linkflags/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（三条路真跑 · flag 承重 · 模块自带 .link 自动链 · 链接需求写进生成物 · 不带 flag 产物一字不变；缺库显式跳过）"
else bad "tests/linkflags/run.sh"; echo "$out"; fi

echo "== @frozen（C-ABI.md §9.8：作者签字"布局就是 C 的布局" ⇒ 按值过界）=="
if out=$(timeout 600 ./tests/frozen/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（真调 libc 按值返回 · **C 侧镜像**比 sizeof/offsetof · 产物断言与镜像各有牙 · 5 反例）"
else bad "tests/frozen/run.sh"; echo "$out"; fi

echo "== C-ABI 线（C-ABI.md §9：跟 C 打交道，进与出两个方向）=="
if out=$(timeout 600 ./tests/cabi/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（@export：C 宿主**真的链接并调用** · .so 里 nm -D 看得见符号 · 10 种"没有那一个 C 符号"的反例）"
else bad "tests/cabi/run.sh"; echo "$out"; fi

echo "== 攻击测试（tests/hostile：不规范的 .so 打主程序 —— 判据是"怎么爆"）=="
if out=$(timeout 600 ./tests/hostile/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（设计赢：伪造指针被拒 · 板内 containment · 关板后硬缺页；攻击赢：食言/越界写栈/munmap 重映射/改表 —— 边界写在明处）"
else bad "tests/hostile/run.sh"; echo "$out"; fi

echo "== 契约验证（tests/contract：签字说错 ⇒ 当场可证伪；与 hostile 是一对孪生）=="
if out=$(timeout 600 ./tests/contract/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（诚实库 contract=holds · 食言库 + 隔离 rc=139 · 毒化路径 falsified · 对照组 silent=stale）"
else bad "tests/contract/run.sh"; echo "$out"; fi

echo "== 包验证工具（tools/extpkg.py：三态输出 · 工具自己有牙 —— 食言包必须被证伪）=="
if out=$(timeout 600 python3 tools/extpkg.py verify tests/pkg/honest 2>&1) \
   && echo "$out" | grep -q '通过 .*no-retain' && echo "$out" | grep -q '不可验证  handle-ownership'; then
    ok "诚实包：1 通过 + 1 不可验证（三态同现 · 台账不把「没验」说成「验过」）"
else bad "extpkg verify tests/pkg/honest"; echo "$out" | tail -8; fi
if out=$(timeout 600 python3 tools/extpkg.py verify tests/pkg/keeper 2>&1); then
    bad "食言包：应当被证伪（rc=1），实得 rc=0"; echo "$out" | tail -8
else
    if echo "$out" | grep -q '被证伪'; then
        ok "食言包：当场被证伪（rc=1 · 隔离页上的硬缺页）"
    else bad "食言包：输出里没有被证伪"; echo "$out" | tail -8; fi
fi

echo "== 哈希与 UUID（RFC 向量 · 与 Python hashlib 对拍 · 吞吐与内存）=="
if out=$(timeout 900 ./tests/hash/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（$(echo "$out" | grep -o 'sha256 [0-9]* MB/s' | head -1) · $(echo "$out" | grep -o '峰值 RSS [0-9]* KB' | head -1) · uuid5 与 Python 逐字节一致）"
else bad "tests/hash/run.sh"; echo "$out"; fi

echo "== JSON（与 Python 规范形对拍 · 坏输入必拒 · 攻击自己 fuzz · 零拷贝证据 · 吞吐与内存）=="
if out=$(timeout 900 ./tests/json/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（$(echo "$out" | grep -o 'parse [0-9]* MB/s' | head -1) · $(echo "$out" | grep -o '峰值 RSS [0-9]* KB' | head -1) · 17 条坏输入全拒 · 2 万轮变异零崩溃）"
else bad "tests/json/run.sh"; echo "$out"; fi

echo "== 事件循环的定时层（deadline 表 + epoll_wait 超时计算，离线判据）=="
if out=$(timeout 300 ./tests/loop/run.sh 2>&1); then
    ok "1 项（超时计算五个边界 · 到期恰好一次 · 等值稳定 · 表满响亮 · 500 个乱序 deadline 全取出）"
else bad "tests/loop/run.sh"; echo "$out"; fi

echo "== 裸 return 在 void 函数里（方法那一格曾经报假错）=="
if out=$(timeout 300 ./tests/voidret/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（方法/自由函数的裸 return 合法 · 非 void 的裸 return 仍然报错）"
else bad "tests/voidret/run.sh"; echo "$out"; fi

echo "== 包工具全链路（extpkg fetch/vendor/build：假 registry 走 file://，不联网）=="
if out=$(timeout 900 ./tests/extpkg/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（首次 fetch 写锁 · --offline 命中缓存 · 改锁一个字节必红 · 空缓存必红且消息可执行 · vendor 后清缓存仍能离线 build · 锁逐字节决定论 · build 自动补 vendor）"
else bad "tests/extpkg/run.sh"; echo "$out"; fi

echo "== 绑定生成器（tools/cbindgen.py：类型映射 · 零 effects · 生成物能编译并真调用）=="
if out=$(timeout 600 ./tests/cbindgen/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（8 条声明逐条对拍 · 零 effects · --check · 生成的绑定真 dlopen libc 调 getpid）"
else bad "tests/cbindgen/run.sh"; echo "$out"; fi

echo "== 真库（C-ABI.md §9.11：用 cairo 画一张画 —— dlopen 真第三方 .so，不链接它）=="
if out=$(timeout 600 ./tests/real-lib/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（dlopen libcairo + 20 个符号 + 签名的表槽 + 板里的画布 + PNG；'画对了'由像素数与四边判据钉住；无 cairo 的机器显式跳过）"
else bad "tests/real-lib/run.sh"; echo "$out"; fi

echo "== 板 / Heap（HEAP.md · C-ABI.md §9.10：保留便宜 · 按需 commit · 门有牙 · close 即 munmap）=="
if out=$(timeout 600 ./tests/heap/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（4 GiB 保留只涨 4 KB RSS · commit 1 MiB 粒度 · 板外指针被拒 · **close 之后再用 = SIGSEGV**）"
else bad "tests/heap/run.sh"; echo "$out"; fi

echo "== 模块（定案 70：语义导入 · 一个文件一个模块 · @private · 禁环）=="
if out=$(timeout 600 ./tests/modules/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（8 正例 + 14 反例 + mangle 判据 · 含 use mod::* 与 use mod::{a,b} 各 1 正例 + 6 条边界）"
else bad "tests/modules/run.sh"; echo "$out"; fi

echo "== ASan（内存安全的形状必须真的跑得干净）=="
if out=$(timeout 600 ./tests/asan/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 个形状 ASan 干净"
else bad "tests/asan/run.sh"; echo "$out"; fi

echo "== arena 层号（定案 68：检查器是唯一权威，codegen 只翻译 —— 不许漂）=="
# 哨兵 `EXTC_DBG_ARENA=1` 在**生成时**比对"检查器算的层号"与"codegen 当前块"✓
# 它不改变输出（golden 照旧逐字节相同 ✓），只是把"两个权威漂了"变成看得见的一行 ✗
# 全量编译一遍（串行 ~32s，是整个 quick 里最贵的单步）⇒ 交给 parrun 并行 ✓
if out=$(python3 tools/parrun.py --mode arena-scan 2>&1); then
    ok "$out"
else
    bad "arena 层号漂移"; echo "$out"
fi

echo "== 全限定名（PLAN #53：**全名是权利** · \`as\` 别名是方便）=="
if out=$(timeout 600 ./tests/qname/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（6 正例 + 1 反例 + 1 结构判据）"
else bad "tests/qname/run.sh"; echo "$out" | tail -8; fi

echo "== std::fs 命名规范（定案 77：读型/写型分开 ⇒ 误用**编不过**）=="
if out=$(timeout 600 ./tests/fs-shape/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（1 正例 + 2 反例 + 常量不外露 + 规范入档）"
else bad "tests/fs-shape/run.sh"; echo "$out" | tail -8; fi

echo "== 文件归属（定案 79：程序拥有 ⇒ 忘关报警告 · 双关安全 · ASan）=="
if out=$(timeout 600 ./tests/fs/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（fd 恒定 + canary + 泄漏警告 + readAll + 双关 + closed + ASan）"
else bad "tests/fs/run.sh"; echo "$out" | tail -8; fi

echo "== 命令行（IO.md §7：\`main(args)\` ⇒ args.len 含程序名 · 形状写错编译期挡住）=="
if out=$(timeout 600 ./tests/argv/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（正例 + 边界 + 无参哨兵 + 3 反例）"
else bad "tests/argv/run.sh"; echo "$out" | tail -8; fi

echo "== 池（期 0 库级 slot map + 期 1 运行期池注册表：handle · dense · churn 不涨 · 块退出带走子树 —— 见 POOLS.md）=="
if out=$(timeout 600 ./tests/pool/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（handle/dense/世代 · churn 平 · canary 会响 · ASan）"
else bad "tests/pool/run.sh"; echo "$out" | tail -8; fi

echo "== 有序 map<K, V>（POOLS.md §11：B 树；第一步是单节点有序表）=="
if out=$(timeout 600 ./tests/map/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
else bad "tests/map/run.sh"; echo "$out" | tail -8; fi

echo "== 实例化类型上的 impl 与 dyn（slice<u8> 挂方法 · 按实例隔离 · dyn 派发 · L 债金丝雀）=="
if out=$(timeout 600 ./tests/instimpl/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
if out=$(timeout 600 ./tests/lambda/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
if out=$(timeout 600 ./tests/ext/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
if out=$(timeout 900 ./tests/time/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
else bad "tests/time/run.sh"; echo "$out" | tail -8; fi
else bad "tests/ext/run.sh"; echo "$out" | tail -8; fi
else bad "tests/lambda/run.sh"; echo "$out" | tail -8; fi
else bad "tests/instimpl/run.sh"; echo "$out" | tail -8; fi

echo "== 期 0 · 哈希表（key→value 随机的正解：开放寻址 + 墓碑 · 见 POOLS.md §10.2）=="
if out=$(timeout 600 ./tests/hashmap/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（新键/覆盖/碰撞/墓碑 churn 平/canary/ASan）"
else bad "tests/hashmap/run.sh"; echo "$out" | tail -8; fi

echo "== 展示代码（examples/showcase-*.extc：文档里的六段，每次编译并运行）=="
if out=$(timeout 300 bash tests/showcase/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（Arena 返回值 · home 机制 · 池身份 · 视图 · impl · extern!）"
else bad "tests/showcase/run.sh"; echo "$out" | tail -8; fi

echo "== STL 库（一个库装所有动态容器；容器建在池上 —— 见 POOLS.md 期 3）=="
if out=$(timeout 600 ./tests/stl/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（vector：翻倍/dense 连续/shrink/clear · ASan）"
else bad "tests/stl/run.sh"; echo "$out" | tail -8; fi

echo "== 线性关联容器（std::linmap / std::linset：只用 ==，不依赖 #57 的 hash）=="
if out=$(timeout 600 bash tests/linmap/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（覆盖/缺失/删除/扩容 · 结构体键 · 集合）"
else bad "tests/linmap/run.sh"; echo "$out" | tail -6; fi

echo "== 注解（@inline 要真的生效 · 写错的注解必须编译期报错）=="
if [ -x tests/annot/run.sh ]; then
    if out=$(tests/annot/run.sh 2>&1); then ok "$(echo "$out" | tail -1)"
    else bad "tests/annot/run.sh"; echo "$out"; fi
fi

echo "== 运算符重载（方法名**就是**运算符：比较六个 + 算术五个 + 流 << >> · 按右操作数类型重名 · 泛型体里实例化时检查）=="
if out=$(timeout 600 bash tests/ops/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（具体类型 · 有牙 · 泛型体 · 流运算符 · 重名分派 · mangle · 9 反例 · 两条不许漏到 gcc）"
else bad "tests/ops/run.sh"; echo "$out" | tail -12; fi

echo "== @noCopy（状态有身份的类型不许按值拷：正例 4 件事 · 四个复制点全挡 —— 见定案 88）=="
if out=$(timeout 600 bash tests/nocopy/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（新值/ref 传参/读字段/调方法照旧 · 绑定·实参·字段·赋值四个复制点全挡 ✓）"
else bad "tests/nocopy/run.sh"; echo "$out" | tail -8; fi

echo '== 构造函数（`T(args)` 就是 `T::new(args)`：糖=显式 · 可失败 · 泛型 · 3 反例）=='
if out=$(timeout 600 bash tests/ctor/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（正例 3 · 反例 3 —— 无 new 教你怎么写 · 返回类型必须对 · 实参个数）"
else bad "tests/ctor/run.sh"; echo "$out" | tail -8; fi

echo "== 泛型组合矩阵（T 的位置 × 特性 · 13 格 · 缺口清单空 —— 见 GENERICS.md）=="
if out=$(timeout 600 bash tests/genmatrix/run.sh 2>&1); then
    ok "$(echo "$out" | grep -c '^  ok') 项（13 格全过 ✓ · #60/#61/#62/#63/#64/#65 六条当天撞到当天修完、当天搬进矩阵或正例 ⇒ 缺口清单空，机制留着）"
else bad "tests/genmatrix/run.sh"; echo "$out" | tail -8; fi

echo "== 头文件 include guard（内容不许落在 #endif 之后）=="
# 判据：每个 src/*.h 的**最后一个非空行**必须是 #endif。
# 为什么要有这一节：曾经有一段声明（连同它的文档注释）被追加到 types.h 的 #endif **之后**
# ⇒ 每被包含一次就重复声明一次。那次是靠 grep 撞出来的，这个脚本让它再也跑不掉 ✓
if out=$(python3 tools/check_guards.py 2>&1); then
    ok "$(echo "$out" | tail -1)"
else
    bad "tools/check_guards.py"; echo "$out" | head -8
fi

echo "== 攻击库（通过的必须是 BASELINE 里那几条 ⇒ 没放松）=="
now=$(mktemp)
# 27 个文件，串行编译时这一节要 ~20 秒 ⇒ 并行（结果集与串行逐字节一致 ✓）
python3 tools/parrun.py --mode compiles > "$now"
if diff -q tests/attacks/BASELINE "$now" >/dev/null; then
    ok "通过集合与基线一致（$(wc -l < tests/attacks/BASELINE) 条已知安全 + 其余全部被挡）"
else
    bad "攻击库的通过集合变了！"; diff tests/attacks/BASELINE "$now" | sed 's/^/        /'
fi
rm -f "$now"

if [ "${1:-}" != "quick" ]; then
    # 生成物黄金闸门：413 份逐字节比对 + **每份都必须是合法 C**。放在完整模式（要跑 413 次编译
    # 加 413 次 gcc，约一分钟）；quick 只跑测试与攻击库，保持快。
    echo "== 闸门·全语料 --check-c（挡"extc 报成功、生成物编不过"；gcc + clang —— 审计 §7 闸门①）=="
    # 2026-09-30 P0 批次新增。`-fsyntax-only` 看不到中端告警，这里用 `-c -O2`，
    # 并只对"指向生成器缺陷"的告警类别判红（tools/gatecommon.py 的 GEN_WARN）。
    if out=$(timeout 1800 python3 tools/gate_checkc.py --scope full 2>&1); then
        ok "$(printf '%s' "$out" | grep -m1 '^\[checkc\]')"
    else bad "tools/gate_checkc.py（全语料 --check-c 变红）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -14; fi

    echo "== 闸门·正例生成物 ASan+UBSan（-O2 会把 UB 优化掉，这里必须真跑 —— 审计 §7 闸门②）=="
    # 挡的是"生成物有 UB 却看起来正常"：从 NULL memmove（P0-8）、越界 arena 格（P0-11）等。
    if out=$(timeout 1800 python3 tools/gate_asan_corpus.py --scope full 2>&1); then
        ok "$(printf '%s' "$out" | grep -m1 '^\[asan\]')"
    else bad "tools/gate_asan_corpus.py（正例生成物出现 sanitizer 报告）"; printf '%s\n' "$out" | sed 's/^/  /' | tail -14; fi

    echo "== 生成物快照（**观察项**：与冻结哈希的差异不作为判据 —— 定案 98；判据只有「必须是合法 C」）=="
    # 2026-09-30 降级（所有者：「golden 应该降级，因为现在需要修理，代码相同不保证正确」）。
    # 「生成物能不能用」由闸门①（tools/gate_checkc.py：全语料 gcc+clang `-c -O2`）与
    # 闸门②（tools/gate_asan_corpus.py：ASan+UBSan 真跑）判；这里只报变化面 + 挡住非法 C。
    if out=$(timeout 900 ./tools/golden.sh 2>&1); then
        ok "$(printf '%s' "$out" | grep -m1 '^  ok' | sed 's/^ *//')"
        printf '%s\n' "$out" | grep '^  · ' | sed 's/^  /  /'
    else bad "tools/golden.sh（生成物里有非法 C）"; printf '%s\n' "$out" | tail -8; fi
    echo "== 基准：extC vs C =="
    ./bench/run.sh 2>&1 | tail -n +1 | sed 's/^/  /' | tail -12
    echo "== 基准：重负载（bt / radix / mandelbrot）=="
    ./bench/heavy/run.sh 2>&1 | sed 's/^/  /'
    echo "== 基准：STL 容器横评（extC 的容器 vs C++ STL；同算法同参数，校验和必须一致）=="
    # dyn Trait 基准：轻量档（测量不是闸门，但**校验和不符必须红** —— 见 bench/dyn/REPORT.md）
    if out=$(N=5000000 NIMM=200000 RUNS=1 ./bench/dyn/stress/run.sh 2>&1); then
        ok "dyn 极端压测（$(printf '%s' "$out" | grep -m1 '^通过')）"
    else bad "bench/dyn/stress"; printf '%s\n' "$out" | tail -8; fi
    if out=$(N=50000 ./bench/dyn/alloc/run.sh 2>&1); then ok "dyn 构造成本（拷贝 vs 分配）"
    else bad "bench/dyn/alloc"; printf '%s\n' "$out" | tail -6; fi
    if out=$(P=512 MIX=8 RUNS=1 ./bench/dyn/real/run.sh 2>&1); then
        ok "dyn 现实负载（$(printf '%s' "$out" | grep -m1 '^通过')）"
    else bad "bench/dyn/real"; printf '%s\n' "$out" | tail -8; fi
    if ./bench/stl/run.sh >/tmp/extc-bench-stl.log 2>&1; then ok "$(grep -m1 '^通过' /tmp/extc-bench-stl.log)"
    else bad "bench/stl"; tail -12 /tmp/extc-bench-stl.log; fi
    echo "== 基准：**综合应用场景横评**（会话表 / 路由表 / 日志缓冲；extC STL vs C++ STL vs Go）=="
    # 缩小规模当回归（与 compile/oi 那两节同一规矩），`WRITE_RESULTS=0` ⇒ 不冲掉完整表。
    if aout=$(SCALE=1 RUNS=1 WRITE_RESULTS=0 ./bench/app/run.sh 2>&1); then
        ok "综合场景横评（三语言校验和一致 ✓ · 完整表见 bench/app/RESULTS.md）"
    else
        bad "综合场景横评（构建失败 / 语言之间对拍不一致 ✗）"; echo "$aout" | tail -20 | sed 's/^/  /'
    fi
    echo "== 基准：随机负载压力（同种子对拍 C）=="
    ./bench/stress/run.sh 2>&1 | sed 's/^/  /'
    echo "== 基准：**编译时长**（合成大程序；顺带抓「生成的 C 编不过」那类问题）=="
    if cout=$(NS=500 RUNS=1 ./bench/compile/run.sh 2>&1); then
        echo "$cout" | sed -n '/^N /,$p' | sed 's/^/  /'
        if echo "$cout" | grep -q "ERR"; then bad "编译时长压测（有 ERR）"
        else ok "编译时长压测（N=500 全通）"; fi
    else
        bad "编译时长压测"
    fi

    echo "== 基准：**OI 数量级四语言横评**（三维偏序 CDQ+BIT；缩小规模当回归）=="
    if oout=$(N=500000 RUNS=1 ./bench/oi/run.sh 2>&1); then
        echo "$oout" | sed -n '/^语言/,/^$/p' | sed 's/^/  /'
        ok "OI 横评（四语言输出一致 ✓）"
    else
        bad "OI 横评（有构建失败 / 语言之间对拍不一致 ✗）"
        echo "$oout" | sed 's/^/  /' | tail -20
    fi

    echo "== 基准：**主席树四语言横评**（P3834；小规模 + 暴力对拍，缩小规模当回归）=="
    if pout=$(N=200000 Q=200000 V=200000 RUNS=1 ./bench/oi/persist/run.sh 2>&1); then
        echo "$pout" | sed -n '/^  小规模对拍/p;/^语言/,/^$/p' | sed 's/^/  /'
        ok "主席树横评（四语言一致 + 暴力对拍 ✓）"
    else
        bad "主席树横评（构建失败 / 对拍不一致 ✗）"
        echo "$pout" | sed 's/^/  /' | tail -20
    fi
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
