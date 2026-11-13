#!/usr/bin/env bash
# tests/gc-p0/run.sh —— 阶段 0 门禁 G1/G4/G8/G9 的一键重跑（基线 + patch 后用同一条命令对比）
#
# 判据（docs/analysis/GC-ROOFLINE.md §7；G1 于 2026-11-09 按噪声地板实测修订，见 Lead 结论）：
#   G8  强制事件档（T1）跑 chain2：VmHWM ≤ 1.1× 同形状 T2，且 ASAN 两侧 rc=0 无报告
#   G9  有活任务时调 syspool::collect()：任务仍可驱动、任务数不变
#       after 期望 stdout 全文：live0=1 freed=0 live1=1 / got=20 steps=5 / tag=777，exit 0
#   G4  正式：注入 4×L 循环垃圾时默认档 VmHWM ≤ 声明 M_max + 8 MiB、稳态相邻轮差 ≤2%、
#       且峰值随 M_max 单调（A<B<C 或 A≈B<C）—— 由 stress 子命令（tests/gc-p0/stress.py）跑。
#       下面的 g4 子命令只是 G6/G7 代理：pb 20 B/node ±10%；pc 稳态 ≤4 MB；cyc N vs 4N <1.5x。
#   G1 ① over1ms 步数 = 0（权威读数 = [histo] 严格 >1 ms 的桶和：b4ms 起；无 [histo] 退回 [step]）；
#       ② 按通道拆分（Lead m04215/m04448/m04755，量具 tests/gc-p0/g1c.py）：
#          (a) warm 常规步（seg=0，cyc/pc/pb）：>50µs 占比 ≤ 1e-4（**常数阈值**，空转 over50 率是
#              0–1.5e-5 噪声）+ p99.9 ≤ 10 µs；cold-dense（pd_pointer_dense）不参与该门禁，只判
#              tau_t=步时/pops ≤2 µs（pops>=2）、p99.9 ≤ 1 ms、over1ms=0，>50 µs 只 OBS。
#          (b) 大对象续扫段步（seg>0，[segsteps]）：p99.9 ≤ min(1.5*c_seg*chunk+1 µs, 64 µs 桶界兜底)
#              且 over1ms=0；max 只 OBS。兜底是**桶界精度**（50 µs 落在 b32us–b64us 之间），
#              不是 50 µs 精确值——要精确到 50 µs 需精确分位插桩，列为后续（Lead m04755 B）。
#          (c) regionFree/release/collect 通道单列（G3b ≤1 ms，目标 ≤0.2 ms）。
#          分子/分母都是**同类通道**步数；n<1e5 ⇒ N/A（不是 PASS）。计时口径用 cpu：
#          设 EXTC_RC_STEP_CPU=1，记录里 clock=wall|cpu，汇总按 clock 分别成表。
#       ③ p99.9 ≤ 50 µs（旧混合分母口径，已降级为 legacy 对照；新判据见 ② 的通道拆分）
#   为什么改：本机（WSL2）计时噪声地板实测 max 23.8–171.2 µs —— 纯 CPU 空转微基准、
#   无分配无系统调用，over50 也常为 0 但 max 已 >50 µs ⇒ "max ≤ 50 µs" 结构性不可达、
#   与 GC 无关。故 G1 只看 over1ms（绝对）、>50µs 占比（相对空转）与直方图 p99.9。
#
# 用法：
#   tests/gc-p0/run.sh                # 全部（含空转基准，约 30–60 s）
#   tests/gc-p0/run.sh g8             # 只跑 G8（g8|g9|g12|g13|g14|g15|g16|g18|g20|p03|peak|l10|g4|g1|g1cpu|g1c|noise）
#   tests/gc-p0/run.sh g12            # G12/G13 计数档 soundness（编译期 EXTC_RC_NOCOUNT=0；两档都跑）
#   tests/gc-p0/run.sh g13            # 只跑 G13（uaf_bulk_copy，默认 T2 + 强制 T3 两档，期望 d0r=7）
#   tests/gc-p0/run.sh stress         # **G4 正式验收**：注入 4×L 循环垃圾（tests/gc-p0/stress.py，3 档×3 reps）
#   #   注意：g4 子命令只覆盖 G6/G7 代理（pb/pc/cyc），**不是** G4 判据；G4 看 stress。
#   tests/gc-p0/run.sh peak           # G17 内存墙双配置峰值（A 默认 / D 16MiB / B 64MiB / C 1TB）
#   tests/gc-p0/run.sh l10            # L10-b/L10-c 已知限制（XFAIL，只记录数字，不进回归）
#   tests/gc-p0/run.sh g18            # G18：pd_bigobj 60/200 轮 × 8 MiB（200 轮 G18_PD_REPS=5 reps 合并，
#                                     #   60 轮单 rep 走 case pd_bigobj60；判据 200 轮 ≤3L+2SLAB + stdout 逐字）
#   tests/gc-p0/run.sh g20            # G20：bench/gc/v2 四形状的 [step]/[histo]（P0-B 停顿收益；5 reps×4 形状）
#   tests/gc-p0/run.sh g20cmp         # G20 跨 tag 对照：BEFORE_G20=…p0-before/g20.json AFTER_G20=…p0-after/g20.json
#   EXTC_RC_STEP_CPU=1 tests/gc-p0/run.sh g1cpu   # G1 的 CPU 口径，追加到同一 OUT（clock=cpu）
#   G1C_C_SEG="pd_pointer_dense=34:52,pd_bigobj=9.5:12" tests/gc-p0/run.sh g1c
#                                   # (b) B_seg(shape)=1.5*c_seg(shape)*chunk+1us（c_seg 按形状实测，不得跨形状借）
#   G1C_C_SCAN_NS=27 tests/gc-p0/run.sh g1c   # 旧全局兜底（报告须标注可能跨形状）
#   G1C_SEG_MAX_US=6.2 tests/gc-p0/run.sh g1c # 直接给 B_seg（覆盖闭式）
#   # 正式 after 两遍：先 run.sh all（wall），再 EXTC_RC_STEP_CPU=1 run.sh g1cpu（cpu）。
#   # 判据用 cpu 口径；wall 只作观察（含抢占，与 GC 工作量不可比）。
#   EXTC=./build-emp/extc tests/gc-p0/run.sh all
#   OUT=tests/gc-p0/out/after tests/gc-p0/run.sh all   # 固定输出目录，便于 before/after 对比
#   正式 after 由 Lead 在所有人停手后串行跑（并发 make/bench 会污染 max 与 RSS）。
#
# 隔离约定（Lead 2026-11-09）：编译器只读 build-emp/extc，绝不 make 到 build/。
# 产物：${OUT/{c,bin,csv,log} + runs.jsonl + pauses.jsonl + summary.txt；全在 tests/ 下，不进 docs/。
set -u
cd "$(dirname "$0")/../.."

EXTC=${EXTC:-./build-emp/extc}
CC=${CC:-cc}
# **必须**带 -D_POSIX_C_SOURCE=200809L：生成物不自己定义特性宏，而 -std=c11 是 strict ANSI，
# glibc 因此不暴露 CLOCK_THREAD_CPUTIME_ID / CLOCK_MONOTONIC 宏 ⇒ 运行时的
# `#if defined(CLOCK_THREAD_CPUTIME_ID)` 为假 ⇒ extc_rc_cpuNow() 恒 -1（EXTC_RC_STEP_CPU=1 时
# 所有 [step]/[histo]/[top]/[chan] 时长**恒 0**，G1② 会假 PASS），且 pauseNow 回退到 clock()
# （进程 CPU 时间，µs 粒度）而不是墙钟。2026-11-09 实测与归因见 MEASUREMENTS-GC-P0.md §9.1。
CFLAGS=${CFLAGS:--O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=${OUT:-tests/gc-p0/out/run-$STAMP}
WHAT=${1:-all}
# 2 ms：短进程（G8/G9 约 20 ms）必须在此之前至少采到一次，否则会漏峰值；
# VmHWM 单调递增，末次采样≈峰值。pb 2 s 也只有 ~1000 个采样点，CSV 可接受。
INTERVAL=${INTERVAL:-2}
REPS=${REPS:-3}
mkdir -p "$OUT"/c "$OUT"/bin "$OUT"/csv "$OUT"/log
# 只有全量跑才清空派生 JSON；否则单跑一个门禁（如 noise/g9）会抹掉同目录已有的其它记录。
if [ "$WHAT" = all ]; then
    : >"$OUT/runs.jsonl"
    : >"$OUT/pauses.jsonl"
else
    touch "$OUT/runs.jsonl" "$OUT/pauses.jsonl"
fi

if [ ! -x "$EXTC" ]; then echo "FATAL: no compiler at $EXTC (make BUILDDIR=build-emp -j)" >&2; exit 2; fi

build() { # name src [ENV=VAL ...]
    local name=$1 src=$2; shift 2
    if [ "$#" -eq 0 ]; then
        "$EXTC" "$src" -o "$OUT/c/$name.c" >"$OUT/log/$name.compile.log" 2>&1 || { echo "FAIL compile $name ($src)" >&2; return 1; }
    else
        env "$@" "$EXTC" "$src" -o "$OUT/c/$name.c" >"$OUT/log/$name.compile.log" 2>&1 || { echo "FAIL compile $name ($src)" >&2; return 1; }
    fi
    "$CC" $CFLAGS -o "$OUT/bin/$name" "$OUT/c/$name.c" >>"$OUT/log/$name.compile.log" 2>&1 || { echo "FAIL gcc $name" >&2; return 1; }
    return 0
}

defines() { # name —— 记录产物里的档位宏（第 13/15 行附近）
    grep -nE '^#define (EXTC_RC_NOCOUNT|EXTC_RC_TIME|EXTC_RC_SWEEP_FREE|EXTC_RC_TIER) ' "$OUT/c/$1.c" >"$OUT/log/$1.defines" 2>/dev/null || true
    echo "  [$1] $(tr '\n' ' ' <"$OUT/log/$1.defines")"
}

record() { # name rep rc json [tag] [args]
    python3 -c 'import json,sys
r=json.loads(sys.argv[4]); r.update(case=sys.argv[1], rep=int(sys.argv[2]), rc=int(sys.argv[3]), tag=(sys.argv[5] if len(sys.argv)>5 else ""), args=(sys.argv[6] if len(sys.argv)>6 else ""))
print(json.dumps(r))' "$1" "$2" "$3" "$4" "${5:-}" "${6:-}" >>"$OUT/runs.jsonl"
}

timemax() { # name [args...] —— 交叉量具：/usr/bin/time -f '%M'（time 的父进程很小，ru_maxrss 不被污染）
    local name=$1; shift
    local m w
    /usr/bin/time -f '%M %e' -o "$OUT/log/.tm" "$OUT/bin/$name" "$@" >/dev/null 2>&1
    read -r m w <"$OUT/log/.tm"
    echo "$name $* $m $w" >>"$OUT/timemax.txt"
}

noise() { # 空转基准：同机、同时钟、同规模（n=200000，inner=200），每轮 3 reps
    $CC -O2 -std=c11 -o "$OUT/bin/noise_floor" tests/gc-p0/noise_floor.c \
        >"$OUT/log/noise_floor.compile.log" 2>&1 || { echo "FAIL compile noise_floor" >&2; return 1; }
    : >"$OUT/noise.txt"
    : >"$OUT/stallrate.txt"
    for r in $(seq 1 "$REPS"); do
        "$OUT/bin/noise_floor" 200000 200 | tee -a "$OUT/noise.txt"
    done
    echo "  ↑ 这是计时噪声地板（不是 GC）：G1 的 >50µs 占比与它对比，p99.9 由 P1-6 直方图给"
}

stallfloor() { # name rep when —— 外部停顿率探针（不是 GC）：同核相邻时间窗里机器固有的停顿次数
    # 依据（Lead R3）：宿主抢占对 guest 不可见（PSI/steal 全 0，且 CLOCK_THREAD_CPUTIME_ID 把宿主抢占
    # 也算进本线程 CPU ⇒ cpu≈wall 同时膨胀），只有"同一台机器、同一核、紧邻时间窗"的纯计算对照能测出地板。
    # 门禁只把它的产物用于 p99.9 通道的环境扣减（见 tests/gc-p0/g1c.py 的 env_floor）。
    local name=$1 rep=$2 when=$3
    if [ ! -x "$OUT/bin/stallrate" ]; then
        $CC -O2 -std=c11 -o "$OUT/bin/stallrate" tests/gc-p0/noise_floor.c \
            >"$OUT/log/stallrate.compile.log" 2>&1 || { echo "FAIL compile stallrate" >&2; return 1; }
    fi
    "$OUT/bin/stallrate" --rate 0.30 2>/dev/null \
        | sed "s/^/stallfloor case=$name rep=$rep when=$when /" >>"$OUT/stallrate.txt"
}

run_plain() { # name rep [args...]
    local name=$1 rep=$2; shift 2
    local rec rc
    rec=$(python3 tests/gc-p0/sample.py --bin "$OUT/bin/$name" --args "$@" \
        --interval-ms "$INTERVAL" --csv "$OUT/csv/${name}_r${rep}.csv" \
        --stdout "$OUT/log/${name}_r${rep}.out" --stderr "$OUT/log/${name}_r${rep}.err")
    rc=$?
    record "$name" "$rep" "$rc" "$rec" "" "$*"
}

run_plain_to() { # name rep timeout_s [args...] —— 同 run_plain，但给 sample.py 传 --timeout（挂死回归用）
    local name=$1 rep=$2 tmo=$3; shift 3
    local rec rc
    rec=$(python3 tests/gc-p0/sample.py --bin "$OUT/bin/$name" --args "$@" \
        --interval-ms "$INTERVAL" --timeout "$tmo" --csv "$OUT/csv/${name}_r${rep}.csv" \
        --stdout "$OUT/log/${name}_r${rep}.out" --stderr "$OUT/log/${name}_r${rep}.err")
    rc=$?
    record "$name" "$rep" "$rc" "$rec" "" "$*"
}

run_pause() { # name rep [args...]
    local name=$1 rep=$2; shift 2
    local rec rc pr clock envs
    clock=wall
    envs=(--env "EXTC_RC_PAUSE=${EXTC_RC_PAUSE:-1}")
    if [ -n "${EXTC_RC_STEP_CPU:-}" ]; then envs+=(--env "EXTC_RC_STEP_CPU=$EXTC_RC_STEP_CPU"); clock=cpu; fi
    stallfloor "$name" "$rep" pre
    rec=$(python3 tests/gc-p0/sample.py --bin "$OUT/bin/$name" --args "$@" \
        --interval-ms "$INTERVAL" "${envs[@]}" \
        --csv "$OUT/csv/${name}_pause_r${rep}.csv" \
        --stdout "$OUT/log/${name}_pause_r${rep}.out" --stderr "$OUT/log/${name}_pause_r${rep}.err")
    rc=$?
    # 前后各一次：环境地板取两次里较小的一次（最少扣减 = 最严），见 g1c.py env_floor
    stallfloor "$name" "$rep" post
    record "$name" "$rep" "$rc" "$rec" "pause" "$*"
    pr=$(python3 tests/gc-p0/pause_parse.py "$OUT/log/${name}_pause_r${rep}.err")
    # 解析结果走 stdin 而不是 argv：pc 这种长跑会打出几千行 [quota]，pr 可达数百 KB，
    # 单个 argv 字符串超过 Linux 的 MAX_ARG_STRLEN(128 KiB) 会让 python3 报 "Argument list too long"。
    printf '%s' "$pr" | python3 -c 'import json,sys
d=json.loads(sys.stdin.read()); d.update(case=sys.argv[1], rep=int(sys.argv[2]), clock=sys.argv[3])
print(json.dumps(d))' "$name" "$rep" "$clock" >>"$OUT/pauses.jsonl"
}

if [ "$WHAT" = all ] || [ "$WHAT" = noise ] || [ "$WHAT" = g1c ] || [ "$WHAT" = g1cpu ]; then
    echo "== 空转基准 noise_floor（G1 噪声地板对照，n=200000 inner=200，$REPS reps）=="
    noise
fi

echo "== G8：强制事件档 vs 清扫档（chain2 形状，N=2,000,000，$REPS reps）=="
if [ "$WHAT" = all ] || [ "$WHAT" = g8 ]; then
    build g8_t1 tests/gc-p0/src/g8_chain2.extc EXTC_RC_SWEEPFREE=1 EXTC_RC_NOCOUNT=0 \
      && build g8_t2 tests/gc-p0/src/g8_chain2.extc EXTC_RC_SWEEPFREE=0 \
      && { defines g8_t1; defines g8_t2
           timemax g8_t1 2000000; timemax g8_t2 2000000
           for r in $(seq 1 "$REPS"); do run_plain g8_t1 "$r" 2000000; run_plain g8_t2 "$r" 2000000; done
           # ASAN：G8 两侧各跑一次，确认 patch 后级联释放没有 UAF/双重释放
           for t in g8_t1 g8_t2; do
               if $CC -O1 -g -std=c11 -fwrapv -fsanitize=address -o "$OUT/bin/${t}_asan" "$OUT/c/$t.c" >>"$OUT/log/${t}_asan.compile.log" 2>&1; then
                   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 timeout 120 "$OUT/bin/${t}_asan" 2000000 \
                       >"$OUT/log/${t}_asan.out" 2>"$OUT/log/${t}_asan.err"
                   echo "  $t asan rc=$?" | tee -a "$OUT/log/${t}_asan.out"
               else echo "  skip ASAN $t (libasan missing)" | tee -a "$OUT/log/${t}_asan.out"; fi
           done; }
fi

echo "== G9：活任务存在时 syspool::collect()（tests/zone/collect_with_live_task.extc）=="
if [ "$WHAT" = all ] || [ "$WHAT" = g9 ]; then
    if build g9 tests/zone/collect_with_live_task.extc; then
        for r in $(seq 1 "$REPS"); do run_plain g9 "$r"; done
        echo "  g9 stdout（全文）："; sed 's/^/    /' "$OUT/log/g9_r1.out" 2>/dev/null
        echo "  g9 stderr（首行）：$(head -1 "$OUT/log/g9_r1.err" 2>/dev/null)"
        # ASAN：同一份生成物（-O1 -g），确认 collect 路径本身没有 UAF/双重释放
        if $CC -O1 -g -std=c11 -fwrapv -fsanitize=address -o "$OUT/bin/g9_asan" "$OUT/c/g9.c" \
                >"$OUT/log/g9_asan.compile.log" 2>&1; then
            ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 timeout 60 "$OUT/bin/g9_asan" \
                >"$OUT/log/g9_asan.out" 2>"$OUT/log/g9_asan.err"; echo "  asan rc=$?" \
                | tee -a "$OUT/log/g9_asan.out"
        else echo "  skip ASAN (libasan missing)" | tee -a "$OUT/log/g9_asan.out"; fi
    fi
fi

# G12/G13 共用：强制计数档（T3）+ 默认档各跑 REPS 次并打印 stdout。
# 抽成顶层函数是为了让 `run.sh g13` 也能单独用（Lead m02258：doc-audit 需要可复制的 G13 命令）。
g12() { # case src expect pre_fix_value
    local case=$1 src=$2 expect=$3 pre=$4
    build "${case}_count" "tests/gc-p0/src/$src.extc" EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0 && defines "${case}_count" \
      && { for r in $(seq 1 "$REPS"); do run_plain "${case}_count" "$r"; done; }
    build "${case}_dflt" "tests/gc-p0/src/$src.extc" && defines "${case}_dflt" \
      && { for r in $(seq 1 "$REPS"); do run_plain "${case}_dflt" "$r"; done; }
    echo "  [$case] 计数档期望『$expect』（修复前=$pre）；实际 stdout: $(tr -d '\n' < "$OUT/log/${case}_count_r1.out" 2>/dev/null)"
    echo "  [$case] 默认档观测 stdout: $(tr -d '\n' < "$OUT/log/${case}_dflt_r1.out" 2>/dev/null)"
}

echo "== G12/G13：计数档 soundness（**编译期** EXTC_RC_NOCOUNT=0 强制计数；默认档仅观测）=="
# 注：EXTC_RC_NOCOUNT 是**编译器**开关（main.c:494 "CHANGES THE OUTPUT"），运行期设 env 无效——
# 必须在 $EXTC 生成 .c 时传入，产物头部 #define EXTC_RC_NOCOUNT 0 才是自证。
# 判据（Lead 2026-11-09）：计数档 stdout 逐字等于期望值且 exit 0。修复前（HEAD 复现）分别为
# 111 / 555：参数槽被覆盖后旧别名复活、copyInto 逐字拷贝后目标容器指针被判死。
# 默认档（NOCOUNT=1，无 freeSlot 入口）本机实测不可达 ⇒ 只记录、不判（doc-audit 正在判
# unitAllAcyclic 判据 iv 是否覆盖这两个模式）。
if [ "$WHAT" = all ] || [ "$WHAT" = g12 ]; then
    # 强制计数档 = T3：NOCOUNT=0（计数入口真实）+ SWEEP_FREE=0（关清扫，强制走计数路径）。
    # 只给 NOCOUNT=0 时，doc-audit 的 ref 绕道形状会被 unitAllAcyclic 自动升级成 T1（事件档，
    # SWEEP_FREE 1），那就不是"强制计数档"了（本机实测产物宏可证）。
    # G12：只有强制计数档判（默认档 T2 无 freeSlot 入口 ⇒ PASS-by-design，仅记录 999）。
    # G13：**两档都判**——doc-audit 的 `ref s[0]` 绕道形状让默认档自动进事件档 T1
    #      （本机 HEAD 实测产物 NOCOUNT 0/SWEEP_FREE 1、输出 555）⇒ 这是默认路径可命中的 S0 bug。
    g12 g12_param uaf_param_overwrite "alias check b.v=999" "111"
    g12 g13_bulk  uaf_bulk_copy      "d0r=7" "555"
fi

# G13 独立子命令（Lead m02258）：doc-audit 第二段要按这条跑，两档都必须打印、命令可复制：
#   tests/gc-p0/run.sh g13
# 期望两档 stdout 均为 d0r=7（修复前默认档 555 / 强制档 555）。
if [ "$WHAT" = g13 ]; then
    g12 g13_bulk uaf_bulk_copy "d0r=7" "555"
fi

echo "== G16 回归：sweep 中新建 slab 的错误标黑 ⇒ 活对象被误释放（2026-11-09 已修，reclaim.c:845-851）=="
# 原 bug：slabNew 把扫掠中新建的 slab prepend 到 r->slabs 头（cursor 之后，本轮永不扫到），
# alloc 的"扫掠中分配即标黑"仍给它置黑位 ⇒ 黑位无人清 ⇒ 下一轮 markStep 的 dedupe 跳过该子树
# ⇒ 下一轮 sweep 释放活结点 ⇒ 链成环、末尾遍历回跳、CPU-bound 挂死。
# 旧构建（build-frz/extc 在修复前）N=2,000,000 会挂死；新构建 ~0.04 s。判据：3 reps 全 exit 0、
# timed_out=false、stdout 含 cnt=2000000 且 sum=1999999000000、wall<5 s（挂死时 --timeout 25 会命中）。
# N 不要降到 1.5M 以下：实测旧构建 1.0–1.3M 正常，1.5M 起才暴露。
if [ "$WHAT" = all ] || [ "$WHAT" = g16 ]; then
    if build g16 tests/gc-p0/src/regress_sweep_black.extc; then
        defines g16
        for r in $(seq 1 "$REPS"); do run_plain_to g16 "$r" 25 2000000; done
        echo "  g16 stdout（rep1）：$(tr -d '\n' < "$OUT/log/g16_r1.out" 2>/dev/null)"
    fi
fi

echo "== G18：大对象反复分配（pd_bigobj：60/200 轮 × 8 MiB）+ 指针稠密观察（pd_pointer_dense）=="
# 内存判据（Lead m03358，doc-audit 复核口径）：live 不写死，从 [work] 的 M_max 反读（M_max=3·live）；
#   peak200 <= 3·live + 2·SLAB_SIZE 且 peak200 <= peak60×1.05（规模无关）；
#   RSS 取 timemax 的 ru_maxrss（/usr/bin/time -f '%M'），stdout/exit 取 run_pause 进程（两进程两口径）。
# 停顿列（Lead m03449）：① 常规分配通道（n<=64 KiB）新增回收切片 <= 5 us（与 G1 同口径）；
#   ② 大分配通道（n>64 KiB）单独报告：p99.9（n>=1e3 时）+ over1ms=0 + P_step 闭式对照。
# before 锚点（build-lead5，Lead 实测）：60 轮 493,400 kB / 745.9 ms；200 轮 1,067,288 kB / 1,272.9 ms；
#   [pause] collects=1、[work] n=121、thr_max=1 MiB ⇒ ≈130 个 big 未回收（RED 锚点）。
# 追加观察用例 pd_pointer_dense（Lead m03381，**不参与 G18 判定**）：8 MiB 里 1M 个非空指针 x 200 轮，
#   只报 peak/[histo] 尾部/over1ms/[top]，用来实测大分配通道 x 指针稠密的上界风险。
# 量具：timemax（/usr/bin/time -f '%M'）+ run_pause（sample.py 的 VmHWM/CSV，带 PAUSE=1 原始 stderr ⇒
#   [pause]/[step]/[histo]/[top]/[work] 落 log/）。run_pause 同时给退出码自证（trap 早退 ⇒ RSS 小 ⇒ 假 PASS）。
if [ "$WHAT" = all ] || [ "$WHAT" = g18 ]; then
    if build pd_bigobj tests/gc-p0/src/pd_bigobj.extc; then
        defines pd_bigobj
        # 60 轮对照与 200 轮共用同一个二进制，但日志/CSV 必须按 case 名分开（否则 r1 互相覆盖）。
        # run_pause 按 $OUT/bin/<case> 找二进制 ⇒ 给 60 轮一个符号链接，沿用 pd_bigobj60 这个 case 名。
        ln -sf pd_bigobj "$OUT/bin/pd_bigobj60"
        for n in 60 200; do timemax pd_bigobj "$n"; done
        # Lead m04755 A：200 轮跑 G18_PD_REPS（默认 5）次，5 rep 的 [segsteps] 合并约 12.7-16.3 万个
        # seg>0 步 ⇒ 才过 MIN_N=1e5（单 rep 只有 2.5-3.3 万 ⇒ (b) 通道会 N/A）。60 轮是内存判据的
        # 「规模无关」对照，只跑 1 rep，case 名单独用 pd_bigobj60——否则它会混进 200 轮的分母。
        # 显式 STEP_CPU=1（run_pause 只在环境已设时透传）：停顿列用 cpu 口径，max 只观察。
        EXTC_RC_STEP_CPU=1 run_pause pd_bigobj60 1 60
        for r in $(seq 1 "${G18_PD_REPS:-5}"); do
            EXTC_RC_STEP_CPU=1 run_pause pd_bigobj "$r" 200
        done
        echo "  g18 stdout 60 ：$(tr -d '\n' < "$OUT/log/pd_bigobj60_pause_r1.out" 2>/dev/null)"
        echo "  g18 stdout 200：$(tr -d '\n' < "$OUT/log/pd_bigobj_pause_r1.out" 2>/dev/null)"
        tail -2 "$OUT/timemax.txt"
    fi
    if build pd_pointer_dense tests/gc-p0/src/pd_pointer_dense.extc; then
        defines pd_pointer_dense
        timemax pd_pointer_dense 200
        # 同上（m04755 A）：dense 也跑 G18_PD_REPS 次，合并后 (b) 通道过 MIN_N。
        for r in $(seq 1 "${G18_PD_REPS:-5}"); do
            EXTC_RC_STEP_CPU=1 run_pause pd_pointer_dense "$r" 200
        done
        echo "  dense stdout 200：$(tr -d '\n' < "$OUT/log/pd_pointer_dense_pause_r1.out" 2>/dev/null)"
        tail -1 "$OUT/timemax.txt"
    fi
    # 把 [histo]/[step]/[top]/[work] 落成 g18.json，供 summarize 打两通道表与 compare 跨构建对照。
    python3 tests/gc-p0/g18.py json --out "$OUT" --tag "$(basename "$OUT")" || true
fi

echo "== G20：P0-B 的停顿收益（bench/gc/v2 四形状，PAUSE=1 + STEP_CPU=1，${G20_REPS:-5} reps）=="
# Lead m03207：判据 = promote [step] total ≤-10%、sizeratio ≤-40%、cycleratio ≤+3%、promote step max
# median ≤60 us（观察口径；单次 max 不作判据，WSL2 时钟噪声地板 78–181 us）；四形状 wall/RSS ±3%。
# 串行纪律：本段一次只跑一个进程；正式 before/after 必须在无人并发时跑（Lead 协调窗口）。
# before： EXTC=./build-lead5/extc OUT=tests/gc-p0/out/p0-before REPS=3 tests/gc-p0/run.sh g18 / g20
# after ： EXTC=./build-rf/extc     OUT=tests/gc-p0/out/p0-after  REPS=3 tests/gc-p0/run.sh g18 / g20
# 对照 ： BEFORE_G20=tests/gc-p0/out/p0-before/g20.json AFTER_G20=tests/gc-p0/out/p0-after/g20.json tests/gc-p0/run.sh g20cmp
if [ "$WHAT" = all ] || [ "$WHAT" = g20 ]; then
    python3 tests/gc-p0/g20.py run --extc "$EXTC" --cc "$CC" --cflags "$CFLAGS" \
        --out "$OUT" --reps "${G20_REPS:-5}" --pin "${G20_PIN:-6}" \
        --tag "${G20_TAG:-$(basename "$OUT")}" || true
fi

if [ "$WHAT" = g20cmp ]; then
    BEFORE_G20=${BEFORE_G20:-tests/gc-p0/out/p0-before/g20.json}
    AFTER_G20=${AFTER_G20:-tests/gc-p0/out/p0-after/g20.json}
    python3 tests/gc-p0/g20.py compare "$BEFORE_G20" "$AFTER_G20"
fi

echo "== G4 正式验收：注入 4×L 循环垃圾（tests/gc-p0/stress.py；3 档×$REPS reps）=="
# Lead m02258：g4 子命令只覆盖 G6/G7 代理，**不是** G4 判据。G4 判据是「默认档 VmHWM ≤ 声明 M_max
# + 8 MiB、稳态相邻轮差 ≤2%、峰值随 M_max 单调（A<B<C 或 A≈B<C）」。stress.py 直接跑形状与三档对照，
# 原始数据写 $OUT/g4_stress.json（summarize.py 读它出 G4-Stress 段）。
if [ "$WHAT" = all ] || [ "$WHAT" = stress ]; then
    python3 tests/gc-p0/stress.py --extc "$EXTC" --cc "$CC" --cflags "$CFLAGS" \
        --out "$OUT" --reps "$REPS" --live "${STRESS_LIVE:-100000}" \
        --garb "${STRESS_GARB:-400000}" --rounds "${STRESS_ROUNDS:-100}" || true
fi

echo "== G4 代理（pb 20 B/node；pc 稳态；cyc N vs 4N）=="
if [ "$WHAT" = all ] || [ "$WHAT" = g4 ]; then
    build pb tests/gc-p0/src/pb_escape_grow.extc \
      && build pc tests/gc-p0/src/pc_cycle_churn.extc \
      && build cyc tests/reclaim-shapes/cyclic_pressure.extc \
      && { timemax pb 100000000; timemax pc 100000000; timemax cyc 20000000; timemax cyc 80000000
           for r in $(seq 1 "$REPS"); do run_plain pb "$r" 100000000; run_plain pc "$r" 100000000; done
           run_plain cyc 1 20000000; run_plain cyc 2 80000000; }
fi

echo "== G14：默认档循环 copyInto 引用元素（P0-7b；判据 (b)：800k ≤1.02×对照 且 ≤4 MB）=="
# 判据来自 Lead m01688 裁决 (b)，取代 m01356 的 "800k ≤ 1.20×50k"（50k→100k 的一次性高水位不是泄漏）：
#   PASS = maxRSS(800k, copy) ≤ 1.02 × maxRSS(800k, 对照) 且 maxRSS(800k, copy) ≤ 4 MB。
#   对照 = 同一程序 argv[2]="ctl"（只跳过 copyInto 那一步，其余分配一样；不要写 "--ctl"，会被
#   sample.py 的 argparse 当未知选项而静默丢记录）。
#   50k/100k/800k 三点与 ratio(800k/50k) 照旧记录（只作观测）。
# 口径用 /usr/bin/time -f '%M'（timemax）：50k 档进程 <1 ms，sample.py 的 2 ms 采样拿不到点。
# 注意：HEAD 的 build/extc 对 copyInto 不发射任何记账 ⇒ 平坦不证明有界；"before 红"须在带
# P0-7b 记账的集成构建上量（见 MEASUREMENTS-GC-P0.md §5 G14 注）。
if [ "$WHAT" = all ] || [ "$WHAT" = g14 ]; then
    if build g14 tests/gc-p0/src/leak_copy_refs.extc; then
        defines g14
        timemax g14 50000
        timemax g14 100000
        timemax g14 800000
        timemax g14 800000 ctl
        # 退出码/输出自证：只用 timemax 会漏掉"trap 后早退 ⇒ RSS 小 ⇒ 假 PASS"
        run_plain g14 1 50000
        run_plain g14 2 800000
        run_plain g14 3 800000 ctl
        tail -4 "$OUT/timemax.txt"
    fi
fi

echo "== P0-3 权衡形状：10×400k 环的峰值（**已知权衡，不是门禁**；不判 PASS/RED）=="
# Lead m01373：releaseOld 预算（2048 步）耗尽后放弃本轮、改靠 sweep，runtime-facts 自测峰值
# 107 MB vs HEAD 45.8 MB（2.3×）。after 报告只记录本形状峰值，并与 HEAD 同形状对比。
if [ "$WHAT" = all ] || [ "$WHAT" = p03 ]; then
    if build p03_ring_release tests/gc-p0/src/p03_ring_release.extc; then
        defines p03_ring_release
        for r in $(seq 1 "$REPS"); do run_plain p03_ring_release "$r" 10 400000; done
    fi
fi


echo "== G15（XFAIL·known L10-a）：pool 槽里的引用赋值 ⇒ 永久 pin（判据 RSS(800k) ≤1.20×RSS(50k)）=="
# Lead m01501。预期 **RED**：pin 表项永久有效（extc_pool_give 不撤 pin），三档都不回收；
# 这是给下一轮 P0-8（pin 源区间撤销 / 池级 epoch）留的量具，**不计入 after 回归、不作阻塞**。
# 报告行写成 `G15 … XFAIL(known L10-a)`；修好后硬判据不变，自动应转 PASS。
if [ "$WHAT" = all ] || [ "$WHAT" = g15 ]; then
    if build g15 tests/gc-p0/src/leak_pool_pin.extc; then
        defines g15
        timemax g15 50000
        timemax g15 800000
        # 退出码自证：trap 早退会让 RSS 变小 ⇒ 假 PASS
        run_plain g15 1 50000
        run_plain g15 2 800000
        tail -2 "$OUT/timemax.txt"
    fi
fi

echo "== G17 内存墙双配置峰值：长命集+churn（A 默认隐式 3L / D 16MiB / B 64MiB / C 1TB）=="
# Lead m01949①：验证峰值是否随 M_max 单调（用于替换 MEMORY-MODEL §18.5 的不可追溯数字）。
# 形状：tests/gc-p0/src/peak_cfg.extc（live=200000 长链 + 60 轮 × 400000 链垃圾）。
# 量具：/usr/bin/time -f '%M'（VmHWM 交叉）+ [work] 的 thr_max/M_max/M_max_now/pressure_steps。
# 观察项，不判 PASS/RED；结论写 MEASUREMENTS-GC-P0.md §8.3。
if [ "$WHAT" = all ] || [ "$WHAT" = peak ]; then
    if build peak_cfg tests/gc-p0/src/peak_cfg.extc; then
        defines peak_cfg
        for cfg in A D B C; do
            case $cfg in
                A) mm="";; D) mm="EXTC_RC_M_MAX=16777216";;
                B) mm="EXTC_RC_M_MAX=67108864";; C) mm="EXTC_RC_M_MAX=1099511627776";;
            esac
            for r in $(seq 1 "$REPS"); do
                env $mm EXTC_RC_PAUSE=1 /usr/bin/time -f '%M %e' -o "$OUT/log/.tm" \
                    "$OUT/bin/peak_cfg" 200000 60 400000 \
                    >"$OUT/log/peak_${cfg}_r${r}.out" 2>"$OUT/log/peak_${cfg}_r${r}.err"
                read -r m w <"$OUT/log/.tm"
                echo "peak $cfg $r $m $w" >>"$OUT/timemax_peak.txt"
                echo "  peak cfg=$cfg r=$r maxrss=${m} kB wall=${w} s"
            done
        done
        grep -hE '^\[work\]|^\[mmax\]' "$OUT"/log/peak_A_r1.err "$OUT"/log/peak_D_r1.err "$OUT"/log/peak_B_r1.err "$OUT"/log/peak_C_r1.err 2>/dev/null | sed 's/ walk_gt.*//'
    fi
fi

echo "== L10-b/L10-c 已知限制（XFAIL/known-issue，**不进回归、不判 PASS/RED**，只记录 800k/50k ratio）=="
# Lead m01688：两例均为默认档线性泄漏，修复方向 P0-9/P0-10；数字写 MEASUREMENTS-GC-P0.md 已知限制节。
if [ "$WHAT" = l10 ]; then
    for k in b c; do
        case $k in
            b) nm=l10b;; c) nm=l10c;;
        esac
        case $k in
            b) src=tests/gc-p0/src/leak_copy_arena_dest.extc;; c) src=tests/gc-p0/src/leak_copy_rc_churn.extc;;
        esac
        if build "$nm" "$src"; then
            defines "$nm"
            timemax "$nm" 50000
            timemax "$nm" 800000
            run_plain "$nm" 1 50000
            run_plain "$nm" 2 800000
            tail -2 "$OUT/timemax.txt"
        fi
    done
fi

if [ "$WHAT" = g1cpu ] || [ "$WHAT" = g1c ]; then export EXTC_RC_STEP_CPU=1; fi
echo "== G1：停顿尾部（[pause]/[step]；clock=${EXTC_RC_STEP_CPU:+cpu}${EXTC_RC_STEP_CPU:-wall}）=="
if [ "$WHAT" = all ] || [ "$WHAT" = g1 ] || [ "$WHAT" = g1cpu ] || [ "$WHAT" = g1c ]; then
    # 四个形状必须由本段自建：g1cpu/g1c 单跑时不再依赖先跑过 all/g4（否则 sample.py 找不到
    # $OUT/bin/<shape> 会静默产出空 .err ⇒ g1c 把 (a)(b) 记成 N/A"（实测：2026-11-13 单跑 g1cpu 全 N/A）。
    # 混合形状按源文件名命名（pd_bigobj，不叫 pd）：(b) 的 B_seg 标定表按形状名索引，叫 pd 会解析不到
    # pd_bigobj 行（bseg-cal.tsv 键是 pd_bigobj/pd_pointer_dense）⇒ (b) 结构性 N/A。
    # 混合形状 pd_bigobj 的 (b) 样本由 **G18 段**提供（200 轮 × G18_PD_REPS=5 reps ⇒ 合并约 1.3-1.6e5
    # seg>0 步 > MIN_N=1e5）。本段**不要**再跑 pd_bigobj：G18 的 stdout 校验与 g20 的规模无关对照都读
    # log/pd_bigobj_pause_r*.{err,out}，本段换个轮数跑一次就会把它们覆盖（2026-11-13 实测：本段跑 600 轮
    # ⇒ G18 的 stdout 逐字校验拿到 600 轮输出 ⇒ G18 假 RED）。
    # pc 的轮数从 50M 抬到 150M：pc 每次 collect 只推进少量步，50M×3 reps 只有 41 184 步 < MIN_N=1e5
    # ⇒ (a) 结构性 N/A；150M×3 ⇒ 约 12 万步过线（耗时 0.34s→1s/rep，可接受）。判据线不动。
    # rel 是 (c) release 通道的**探针用例**（Lead R3）：release 的发射条件很窄，四个负载形状与 digraph
    # 的生成 C 里调用点都是 0 处 ⇒ 该通道在它们上面结构性不可达（g1c 需要 relsite 证据才记 vacuous）。
    # 只有本形状真的走 releaseDead（每轮 new 一个自环节点覆盖长命根槽 keep）；它每轮仅分配 1 个节点，
    # 不构成负载 ⇒ 步分布不判（run.sh 传 --chan-only rel:release，g1c 把它的 (a)/(b) 记 SKIP）。
    # 轮数 2e6：实测 release n=1（region 大部分时间在 sweeping，releaseDead 按设计直接返回），
    # 判据是单次 max<=1ms；耗时约 0.01s/rep。
    build pb tests/gc-p0/src/pb_escape_grow.extc \
      && build pc tests/gc-p0/src/pc_cycle_churn.extc \
      && build cyc tests/reclaim-shapes/cyclic_pressure.extc \
      && build pd_bigobj tests/gc-p0/src/pd_bigobj.extc \
      && build rel tests/gc-p0/src/rel_overwrite.extc \
      && { for r in $(seq 1 "$REPS"); do
               run_pause cyc "$r" 20000000
               run_pause pc "$r" 150000000
               run_pause pb "$r" 50000000
               run_pause rel "$r" 2000000
           done; }
fi

# release 通道的结构性证据（Lead R3）：数每个用例**生成 C** 里 extc_rc_storeRefOwnedRelease 的调用点。
# 调用点 0 处 ⇒ 该通道在本形状结构上不可能触发（n==0 是必然而非采样不足）⇒ g1c 记 vacuous PASS；
# pd_pointer_dense 必须在内：它是唯一的 cold-dense 形状，此前缺 relsite 证据 ⇒ 该用例恒 N/A（Lead R7 补上）。
# 缺这份文件时 g1c 不 vacuous（保持 N/A），有调用点而 n==0 也不 vacuous —— 不许无证据变绿。
if [ "$WHAT" = all ] || [ "$WHAT" = g1 ] || [ "$WHAT" = g1cpu ] || [ "$WHAT" = g1c ]; then
    : >"$OUT/relsite.txt"
    for name in cyc pb pc pd_bigobj pd_pointer_dense rel; do
        [ -f "$OUT/c/$name.c" ] || continue
        n=$(grep -c "extc_rc_storeRefOwnedRelease((" "$OUT/c/$name.c" || true)
        echo "relsite case=$name sites=$n" >>"$OUT/relsite.txt"
    done
fi

# G1② 通道拆分（Lead m04215 / task-29）：(a) 常规步 seg=0、(b) 大对象续扫段 seg>0、
# (c) regionFree/release/collect 单列。需要运行时发射 [segsteps] 行；缺该行时脚本记
# N/A(no-segsteps)（不是 PASS）。cpu 口径，max 不作判据，n<1e5 ⇒ N/A。
if [ "$WHAT" = all ] || [ "$WHAT" = g1 ] || [ "$WHAT" = g1cpu ] || [ "$WHAT" = g1c ]; then
    echo "== G1② 通道拆分（tests/gc-p0/g1c.py；判据见脚本头）=="
    # (b) 判据（Lead m04591）：p99.9 <= B_seg(shape) 且 p99.9 <= 50us 绝对兜底 且 over1ms=0；
    # max 只记 OBS（尾部与 chunk 无关、机制未定位，task-32 再议）。
    # B_seg(shape) = 1.5*c_seg(shape)*chunk/1000 + 1us，c_seg 来自 tests/gc-p0/bseg-cal.tsv
    # （每个权威 build 用自身 [segsteps] p99.9÷chunk 标定一次；chunk 锁 8192）。
    # G1C_BSEG_CAL 可换表；G1C_C_SEG/G1C_C_SCAN_NS/G1C_SEG_MAX_US 是手工覆盖（非门禁口径）。
    g1c_extra=()
    g1c_extra+=(--bseg-cal "${G1C_BSEG_CAL:-tests/gc-p0/bseg-cal.tsv}")
    if command -v md5sum >/dev/null 2>&1 && [ -f "$EXTC" ]; then
        g1c_extra+=(--build-md5 "$(md5sum "$EXTC" | awk '{print $1}')")
        # Lead m04755 C：溯源以**源码 md5** 为准（同源码二次构建的 extc md5 会因 -g 路径不同而不同）。
        if [ -f src/back/reclaim.c ] && [ -f src/back/codegen.c ]; then
            g1c_extra+=(--src-md5 "reclaim.c=$(md5sum src/back/reclaim.c | awk '{print $1}'),codegen.c=$(md5sum src/back/codegen.c | awk '{print $1}')")
        fi
    fi
    if [ -n "${G1C_C_SEG:-}" ]; then g1c_extra+=(--c-seg "$G1C_C_SEG"); fi
    if [ -n "${G1C_C_SCAN_NS:-}" ]; then g1c_extra+=(--c-scan-ns "$G1C_C_SCAN_NS"); fi
    if [ -n "${G1C_SEG_MAX_US:-}" ]; then g1c_extra+=(--seg-max-us "$G1C_SEG_MAX_US"); fi
    python3 tests/gc-p0/g1c.py --dir "$OUT/log" --glob '*_pause_r*.err' \
        --noise "$OUT/noise.txt" --json "$OUT/g1c.json" \
        --stallrate "$OUT/stallrate.txt" --runs "$OUT/runs.jsonl" \
        --relsite "$OUT/relsite.txt" --chan-only rel:release --require-workbound \
        ${g1c_extra[@]+"${g1c_extra[@]}"} || true
fi

echo "== 汇总 =="
python3 tests/gc-p0/summarize.py "$OUT" | tee "$OUT/summary.txt"