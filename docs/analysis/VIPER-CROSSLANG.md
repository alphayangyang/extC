# Viper 1.0.6-dev.194 与 extC 的同卡具对比

本文记录一次外部对照实验：把 gcbench v2 的四个形状移植到 Viper（一个此前未见的、
Python 语法 + C 语义的语言），用与 extC 完全相同的计时卡具比较。目的是给「extC 的回收
开销与内存纪律」找一个独立的外部参照系，而不是给 Viper 做评测。

Viper 的分发物与工具链不在本仓库内，所有数字与结论都可由 `bench/viper/` 下的源码与
`bench/viper/setup.sh` 重放。

## 1 摘要

- Viper 侧的内存纪律是**手动 arena bump、永不释放**（与 extC `EXTC_RECLAIM=0` 同口径），
  因此它的时间与 RSS 和 extC 的 off 臂几乎重合：chase 97.4 vs 92.9 ms / 62.6 vs 62.1 MB、
  promote 76.0 vs 71.1 / 109.4 vs 109.6、cycleratio 24.3 vs 19.5 / 45.1 vs 44.5、
  sizeratio 53.7 vs 55.4 / 158.4 vs 157.9。**RSS 由内存纪律决定，不由语言或编译器决定。**
- extC 默认档（帧 region 回收）在三个分配密集形状上全面领先 Viper：
  promote 37.0 ms / 5.7 MB（快 2.05×、内存 1/19）、cycleratio 13.4 / 2.7 MB（快 1.81×、
  内存 1/17）、sizeratio 14.2 / 2.8 MB（快 3.78×、内存 1/57）。
- chase 是唯一反向的形状：Viper 97.4 ms 比 extC 默认档 111.7 ms 更快，与 extC off 臂同档。
  这与仓库内已归因的结论一致：chase 的活集≈堆，全量追踪标记在该形状是纯开销。
- Viper 的代码生成质量本身不差：chase 是六个臂里最快的「careful 语义」实现
  （97.4 vs C++ 105.9 ms）。但它的 cyc/size 分别比 C++/Rust 的 free/refcount 实现慢
  1.4–2.3× 与 3.5×，差别不在发射质量，而在**它不做回收也不 free**。

## 2 Viper 是什么（以分发物为证据）

- 分发物：`V_L64_010.tar.gz` 1,485,200 B，内含 `LICENSE`（MIT，Copyright (c) 2026 TermiNexus）、
  `viperc`（3,075,224 B，ELF 64-bit PIE x86-64，not stripped， `Target: x86_64-pc-linux-gnu`）与
  `includes/` 65 项（带 Python 风味的 stdlib：argparse、json、tomllib、numpy、torch、requests、
  memhub、vector、hashtable、coro、vthreading、lock/rwlock/spinlock、vprofile、traceback 等）。
- `viperc --version`：`Viper 1.0.6-dev.194 2026-11-03 Non-LTS`、`VRTE 0.2.4+297-Non-LTS`、
  `ViperC 0.1.1-dev.964`、`Tpv commit 2c65cea97f`、`Tpc commit 077d705609`、`VRTE commit a0a71e7a85`。
- 语言形态：Python 缩进语法（`import stdio`、`def f(a: int) -> int:`、`class V[T]():`、`for i in range(n)`），
  语义是 C 的：定宽整数族、`T*`、`rawptr`、`&x`/`*p`、`uint64(ptr)` 强转。**没有追踪式 GC**：
  `includes/memhub.vp`（2,350 行）只提供 `MemPool`（arena bump，不支持单 free）、`MemSlab`
  （定长块 + 位图 free）、`MemBuddy`（buddy + 合并 + canary/0xCDCD 检测）三个手动分配器，
  外加 `stdlib.malloc/free` 的 libc 直通。1.0.6 的变更日志提到的 `own` / `?` 是静态生命周期标注，
  不是运行时回收器。
- 编译管线（`--phase2command` dump 出来的真实命令）：`mlir-opt <f>.ll -o <f>.mlir.opt` →
  `mlir-translate --mlir-to-llvmir` → `llc -filetype=obj -relocation-model=pic` → `clang <objs> -o <exe>`。
  中间产物 `<f>.ll` 是 MLIR 的 LLVM dialect 文本（`llvm.func @main()`、`llvm.mlir.addressof`）。

## 3 Linux 构建破译（三个卡点）

官方产物是 Windows 优先的，Linux 上直接跑 `viperc` 会卡在三处：

1. `proj.build(selfmachine=True)` 失败：`ERROR [viperbuild]: selfmachine probe failed
   (PROCESSOR_ARCHITECTURE unavailable)`——它读 Windows 环境变量。解法：显式给
   `triple="x86_64-pc-linux-gnu"` 与 `datalayout="e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"`。
2. `compiler=["clang"]` 会拼出 `clang -filetype=obj -relocation-model=pic`，clang 不认识这两个参数。
   解法：`compiler=["llc"]`。
3. PhaseC 链接行硬带 Windows 导入库 `-lmsvcrt -lucrt -lmingwex -lkernel32`，Linux 上必然
   `cannot find -lmsvcrt`。解法：`linker=["/tmp/viper/linkwrap"]`，一个小 shell 包一层丢弃这四个
   参数后 `exec clang $args -lm`。

另外 `ircore.dll` 在 Linux 上不可用，PhaseB 会打印 `falls back to the legacy
mlir-opt/mlir-translate/llc flow`，所以机器上必须有 MLIR 21 的 `mlir-opt` 与 `mlir-translate`。
本机用的是 `mlir-21-tools` / `libmlir-21` 的 `21.1.8-6ubuntu1`（与系统 llc/clang 同版本），
`apt-get download` + `dpkg -x` 解到 `/tmp/mlirroot`，不动系统安装。完整步骤见 `bench/viper/setup.sh`。

打通后的端到端命令：

    export PATH=/tmp/mlirroot/usr/lib/llvm-21/bin:$PATH
    export LD_LIBRARY_PATH=/tmp/mlirroot/usr/lib/x86_64-linux-gnu:/tmp/mlirroot/usr/lib/llvm-21/lib
    cd bench/viper/chase && cp ../../viper/chase.vp . && /tmp/viper/V_L64/viperc --src chase.vp

## 4 对比方法

四个形状逐个从 `bench/gc/v2/src/*.extc` 移植，原则是**同算法、同 LCG、同求值顺序**：

| 形状 | Viper 端口的数据结构 | 内存纪律 |
| --- | --- | --- |
| chase | `node{k,v,o0..o3}`，48 B/节点（实测 `__sizeof__()`=48，与 C++ `struct Node` 同布局） | MemPool bump，永不释放 |
| promote | `node{k,v,next}` 链 + 500,000 个全垃圾对象 | MemPool bump，永不释放 |
| cycleratio | 256 槽窗口，槽成环，逐轮淘汰 | MemPool bump，永不释放 |
| sizeratio | 4 个尺寸类，`mp.alloc(32 + cls*32)` + `uint64` 表 | MemPool bump，永不释放 |

Viper 端口不释放任何内存，是因为它没有 GC、而 arena bump 是它最自然的批量写法；这与
extC 的 `EXTC_RECLAIM=0` 臂完全同口径，因此「Viper vs extC off」是**同纪律横比**，
「Viper vs extC default」量的是**回收换来的东西**。

卡具 `bench/viper/ab.py`：六个臂在 REPS=11 个回合里交错、奇偶反向，`taskset -c 6` 绑核，
每臂 stdout 与首臂逐字节比对，RSS 用 `/usr/bin/time -f %M` 取峰值，报每臂最小与中位。
所有臂的 stdout 都是 `SAME`。

## 5 结果

单位：min / 中位 ms · 峰值 RSS kB（REPS=11，交错，单核）。

| 形状 | Viper | C++ careful | C++ naive | extC default | extC off | Rust |
| --- | --- | --- | --- | --- | --- | --- |
| chase (1M/4M) | 97.4 / 102.0 · 62,596 | 105.9 / 109.7 · 77,624 | 107.4 / 113.7 · 81,452 | 111.7 / 117.6 · 67,740 | 92.9 / 97.8 · 62,136 | 190.2 / 203.7 · 92,140 |
| promote (500k) | 76.0 / 83.5 · 109,444 | 102.0 / 104.2 · 146,548 | 96.7 / 104.4 · 146,560 | 37.0 / 37.9 · 5,724 | 71.1 / 75.1 · 109,560 | 35.6 / 37.1 · 4,236 |
| cycleratio (400k) | 24.3 / 24.7 · 45,108 | 33.8 / 34.3 · 4,100 | 54.0 / 55.5 · 117,952 | 13.4 / 13.9 · 2,656 | 19.5 / 20.2 · 44,472 | 50.7 / 51.9 · 115,924 |
| sizeratio (2M) | 53.7 / 55.0 · 158,384 | 15.5 / 16.6 · 4,160 | 73.3 / 74.2 · 191,552 | 14.2 / 14.9 · 2,796 | 55.4 / 56.7 · 157,944 | 18.7 / 19.5 · 2,348 |

另一次独立会话的重放给出一致结论（Viper / extC off / extC default / C++ careful，ms）：
chase 99.3 / 96.2 / 111.6 / 104.4；promote 72.1 / 69.1 / 36.4 / 98.6；
cycleratio 22.9 / 18.2 / 13.4 / 33.7；sizeratio 54.9 / 55.1 / 14.7 / 16.0。
两轮之间的绝对漂移与仓库内记录的同机跨会话漂移同量级（5–13%），因此只有**同轮比值**可读。

## 6 解读

1. **RSS 由内存纪律决定。** Viper 与 extC off 的时间差 3–12%、RSS 差 0.7–1.3%（chase 62.6 vs 62.1 MB、
   sizeratio 158.4 vs 157.9 MB），远小于「同样算法、同样堆上界」下 extC default 的差距
   （sizeratio 2.8 vs 158 MB）。这两个语言一个 75,239 行、一个 3 MB 的编译器，在「不回收」这条
   纪律上跑出的数几乎一样——说明这几个形状量的主要是纪律与数据布局，不是编译器质量。
2. **extC 的回收在三个形状上是净收益，在 chase 上是净开销。** promote 上 extC default 比 Viper 快 2.05×
   且内存只有 1/19；chase 上它是最慢的臂。这与 `docs/analysis/P1-PARITY.md` 的分解一致
   （chase 活集≈堆 ⇒ 标记是纯开销；关回收 chase 快 11–17%）。
3. **Viper 的发射质量没问题。** chase 里它是六臂中最快的 careful 语义实现；它落后的两个形状
   （cycleratio 1.4–2.3×、sizeratio 3.5×，对 C++/Rust）恰好是「对手靠 free/refcount 反复复用小块」
   的形状，而 Viper 端口根本不复用内存。也就是说，这是**内存纪律的差异**，不是代码生成的差异。
4. **构建成本**：extC 编译 0.06 s + `cc -O2` 0.70 s = 0.76 s；ViperC 全流程（含 llc + link）0.78 s。
   产物：extC 70,192 B 对 Viper 78,760 B。都是「一次编译、链接系统 C/C++ 运行库」的模型。

## 7 口径与限制（诚实清单）

1. 单核（`taskset -c 6`），11 回合 min/中位；本机跨会话漂移 5–13%，只有同轮比值可读。
2. Viper 臂是「arena 不回收」，不是 Viper 的语言最优解。用 `MemSlab` / `MemBuddy` 手动 free 写
   也可以，那会得到更接近 C++ 的 cyc/size 与更高的实现复杂度；本实验刻意选择与 extC off 同口径，
   是为了让「回收 vs 不回收」这一个变量可读。
3. Viper 是 `1.0.6-dev.194 Non-LTS` 开发版，工具链 Windows 优先；Linux 上的 MLIR 21 与 linkwrap
   是本次为跑通而自备的，不是其官方支持路径。
4. 本次没有观测 Viper 的停顿：它没有回收器，GC 停顿这个概念在其当前实现里不存在。
5. 四个形状都只有一次进程生命周期、一次批量分配；没有测它的并发（`vthreading`）、
   也没有测它的 stdlib 路径（requests / json / numpy 等）。

## 8 复现

    sh bench/viper/setup.sh              # 取 MLIR 21 + 生成 /tmp/viper/linkwrap（不动系统）
    export PATH=/tmp/mlirroot/usr/lib/llvm-21/bin:$PATH
    export LD_LIBRARY_PATH=/tmp/mlirroot/usr/lib/x86_64-linux-gnu:/tmp/mlirroot/usr/lib/llvm-21/lib
    cd bench/viper/chase && /tmp/viper/V_L64/viperc --src chase.vp   # 产物在 __vpcbuild__/output/chase.exe
    ARMS="viper=...,cpp_careful=...,extc_default=..." REPS=11 python3 ../ab.py . chase 1048576 20261013 4000000

其余三个形状把 `chase` 换成 `promote` / `cycleratio` / `sizeratio`，参数分别为
`500000`、`400000 20261013 100`、`2000000 20261013`。

## 9 指纹

- extC 侧：`build/extc` md5 `8af2a5e85324a63720705475d781dbab`；`src/back/reclaim.c`
  `7151a43181f69ab865607f61ad2afad7`（4,212 行）；`src/back/codegen.c`
  `2f560e2e6deef8c4927db4611344aecb`（16,017 行）。
- Viper 侧：分发物 `V_L64_010.tar.gz`（1,485,200 B）；`viperc` 3,075,224 B，BuildID xxHash
  `7a190eb0d857149e`；端口源码即 `bench/viper/*.vp`。
- 卡具：`bench/viper/ab.py`、`bench/viper/setup.sh`；中间构建目录 `__vpcbuild__/` 不入库。
