# GC Roofline 模型数值表 v1.4（由 roofline.py 自动生成，勿手改）

口径：A 段实测锚点 = build-th/extc 72c05dff（FREEZE v4 源码）；P/J 段 = build-p1/extc b9fd57843f2c734c16756f6850ace9c5（FREEZE v7：reclaim.c e17080d1、codegen.c d3393828）。两套构建不可混算。

约定：每个性能数字读作「数值 | 构建(HEAD + extc md5) | 档位 | 量具」。
档位：T0=不发射运行期（arena-only 形状）；T2=清扫档（默认 NOCOUNT=1, SWEEP_FREE=0, SHARDS=1）；T1=事件档（NOCOUNT=0, SWEEP_FREE=1；分层实测见 docs/analysis/MEASUREMENTS-TIER1.md：16B 慢 2.50x、456B 打平、32KB 快 3x）。
标注「模型」的行是推导值；c_r 仍是待标定项（CALIBRATION §11#1）。

| 量 | 数值 | 构建(HEAD+md5) | 档位(0/1/2+开关) | 量具 |
|---|---|---|---|---|
| == A. 实测锚点（task-13 标定，tier2 默认档） == |  |  |  |  |
| 标定构建 vs FREEZE 构建 | calprof 用 extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1)；FREEZE v4 构建 build-rf/extc md5 9fd0dcd861374668be3c74c342beac6f（3607 行源码，常量未随 v4 重测） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | md5sum + make BUILDDIR=build-rf |
| digraph 1M 轮数 / 分配字节 | 85 / 135,901,368 B | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 默认（RECLAIM=1, NOCOUNT=1, SHARDS=1） | calprof.py 轮表 |
| s_bar 平均对象字节 | 42.50 B（250k 42.49） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph | calprof：Sigma marked_bytes/Sigma marked_objs |
| L / H_trig p50 | 753,504 B / 2,130,240 B | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof 轮表 p50 |
| L_peak / M_peak | 2,332,880 B / 6,439,536 B（2.76 L） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof 峰值 |
| sigma / sigma_prev | 0.345 / 0.354（max 0.412） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof 轮表 |
| r = H/L_prev p50 / min | 2.83 / 2.425 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof 轮表 |
| F_slab = cap/H | 1.30（250k 1.26、p4 1.05） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph | calprof cap_bytes/H |
| tau_t / tau_s | 17.49 ns/obj / 0.458 ns/slot | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof：Sigma mark_ns/Sigma marked_objs、Sigma sweep_ns/Sigma cap_slots |
| c_t / c_s | 0.4114 ns/B / 0.0108 ns/B | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof 同口径；区间 0.379-0.457 / 0.0105-0.0121 |
| lambda_GC | 0.2508 ns/B（GC 占墙钟 7.76%） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof：(mark+sweep)_ns/alloc_bytes |
| a_all / a_mut | 0.309 / 0.335 GB/s | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof：alloc/wall、alloc/(wall-GC) |
| 单步停顿 mark p99（三 rep） | 4.36 / 4.79 / 6.42 us | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof per-step 数组（进程内计时） |
| 单步停顿 mark max（三 rep） | 170 / 52.5 / 490 us（不可用） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph 1M | calprof；WSL2 抢占噪声 |
| a_peak（S1 纯 arena） | >= 10.2 GB/s；p3 5-6 GB/s | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T0 不发射运行期（p1/p2/p3 无 region） | /usr/bin/time + VmHWM |
| spectrum_p4 lambda / a_all / a_mut / s_bar | 0.0245 ns/B / 3.166 / 3.433 GB/s / 32.00 B | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 spectrum_p4 8k | calprof |
| 峰值商 kappa_M = (peak-L)/H | 0.772（1M）/ 0.825（250k）/ 1.010（p4） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 digraph + p4 | calprof 峰值轮（CALIBRATION §3.6） |
| == B. 模型常量（推导，非实测） == |  |  |  |  |
| r_min = 1 + g*K/b | 1.690（g=2.76, K=64, b=256） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型：CALIBRATION §8 |
| sigma_max = b/(b+gK) | 0.592（实测 sigma_prev max 0.412，裕度 1.44x） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型 + calprof |
| P(b) = b*tau_t + 8*64*tau_s | b=256 -> 4.71 us（门禁 5.0 us） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型 |
| b*(P_max) | 256（被 MARK_BATCH_MAX=256 截断） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型 |
| M(r) = L*(1+r) | digraph 1M: 3L = 7.00 MB（实测 6.44，-8%） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型 + calprof |
| peak = L + kappa_M*H | kappa_M = 0.772（1M）/ 0.825（250k）/ 1.010（p4） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | calprof 峰值轮 |
| lambda(r) 误差 | -2.0% ~ 0.0%（6 数据集） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | calprof vs 模型 |
| == C. M 预算 -> 推荐参数（模型输出） == |  |  |  |  |
| M=7.0 MB | r_peak: 保守<=2.00 / 典型<=2.59 -> r_eff=2.99, sigma_eff=0.334, lambda=0.2277 ns/B, tax=7.0%, 可行=YES | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 目标 | 模型 |
| M=9.0 MB | r_peak: 保守<=2.86 / 典型<=3.70 -> r_eff=4.10, sigma_eff=0.244, lambda=0.1512 ns/B, tax=4.7%, 可行=YES | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 目标 | 模型 |
| M=12.0 MB | r_peak: 保守<=4.14 / 典型<=5.37 -> r_eff=5.77, sigma_eff=0.173, lambda=0.1033 ns/B, tax=3.2%, 可行=YES | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 目标 | 模型 |
| M=21.0 MB | r_peak: 保守<=8.00 / 典型<=10.36 -> r_eff=10.76, sigma_eff=0.093, lambda=0.0576 ns/B, tax=1.8%, 可行=YES | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 目标 | 模型 |
| M=28.0 MB | r_peak: 保守<=11.00 / 典型<=14.25 -> r_eff=14.65, sigma_eff=0.068, lambda=0.0452 ns/B, tax=1.4%, 可行=YES | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 目标 | 模型 |
| M < 最小可行 | M_min(保守) = L_peak*(1+r_min) = 6.28 MB | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型 |
| == D. 策略交叉：S2 清扫 vs S3 计数 == |  |  |  |  |
| c_r=0.117 ns/B（区间端） | r_cross=1+c_t/c_r=4.52 => M=12.9 MB 时计数开始占优 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 vs T1 | 模型 |
| c_r=0.230 ns/B（待标定） | r_cross=1+c_t/c_r=2.79 => M=8.8 MB 时计数开始占优 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 vs T1 | 模型 |
| c_r=0.411 ns/B（区间端） | r_cross=1+c_t/c_r=2.00 => M=7.0 MB 时计数开始占优 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 vs T1 | 模型 |
| 当前 r=2.83 的位置 | r_cross(c_r=0.23)=2.79 => 计数与清扫打平 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 vs T1 | 模型 |
| == E. 与旧值（v1.0）的差异 == |  |  |  |  |
| s_bar / sigma / r | 80 -> 42.50；0.7415 -> 0.345；1.349 -> 2.83 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | calprof（旧值无实测支撑） |
| c_t / c_s / tau_t | 0.18 -> 0.4114；0.045 -> 0.0108；14.4 -> 17.49 | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | calprof |
| lambda_GC / a_m | 0.212 -> 0.2508 ns/B；0.515 -> 0.309 GB/s | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | calprof |
| kappa | 20 us -> 0（不需要该项） | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T2 | 模型拟合，误差 <=2% |
| a_peak | 5.5 -> >=10.2 GB/s | HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828) / extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1) | T0 | p1 纯 arena |
| == F. memory-first 预算（M=3L 固定、r_policy=2；panel (e)(f) 的数据源） == |  |  |  |  |
| promote 墙钟/GC 分解 | wall=37.91ms; mark=11.66 (30.8%); sweep=3.51 (9.2%); nonGC=22.74 (60.0%); allocs=4.55M | T2 默认 bench 参数 | build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1（FREEZE v4 源码；与 build-rf 行为等价）；rdtsc 包裹 markStep/sweepStep 的副本量具 |  |
| sizeratio 墙钟/GC 分解 | wall=21.56ms; mark=0.78 (3.6%); sweep=1.46 (6.8%); nonGC=19.32 (89.6%); allocs=2.00M | T2 默认 bench 参数 | build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1（FREEZE v4 源码；与 build-rf 行为等价）；rdtsc 包裹 markStep/sweepStep 的副本量具 |  |
| chase 墙钟/GC 分解 | wall=144.16ms; mark=5.91 (4.1%); sweep=0.01 (0.0%); nonGC=138.24 (95.9%); allocs=1.11M | T2 默认 bench 参数 | build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1（FREEZE v4 源码；与 build-rf 行为等价）；rdtsc 包裹 markStep/sweepStep 的副本量具 |  |
| cycleratio 墙钟/GC 分解 | wall=13.11ms; mark=0.27 (2.1%); sweep=1.25 (9.5%); nonGC=11.59 (88.4%); allocs=1.82M | T2 默认 bench 参数 | build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1（FREEZE v4 源码；与 build-rf 行为等价）；rdtsc 包裹 markStep/sweepStep 的副本量具 |  |
| promote 杠杆可达性 | L1 不可达（full_dead=0/879 slab，仅覆盖 0.00% 死槽）；-9.2% 只能当假设上界；L2 mark-50%(phi 估算): -15.4%；L4 alloc-40%(上界): -24.0% | T2 默认 / 探针 T2 | slab 面积探针（Lead，FREEZE v4 生成 C）+ rdtsc 拆分 + 模型 |  |
| sizeratio 杠杆可达性 | L1 不可达（full_dead=3/2394 slab，仅覆盖 0.11% 死槽）；-6.8% 只能当假设上界；L2 mark-50%(phi 估算): -1.8%；L4 alloc-40%(上界): -35.9% | T2 默认 / 探针 T2 | slab 面积探针（Lead，FREEZE v4 生成 C）+ rdtsc 拆分 + 模型 |  |
| chase 杠杆可达性 | L1 n/a（该形状 sweep≈0）；L2 mark-50%(phi 估算): -2.1%；L4 alloc-40%(上界): -38.4% | T2 默认 / 探针 T2 | slab 面积探针（Lead，FREEZE v4 生成 C）+ rdtsc 拆分 + 模型 |  |
| cycleratio 杠杆可达性 | L1b 可达 -9.3% wall（97.8% 死槽落在 full-dead slab：651/756 slab）；L2 mark-50%(phi 估算): -1.0%；L4 alloc-40%(上界): -35.4% | T2 默认 / 探针 T2 | slab 面积探针（Lead，FREEZE v4 生成 C）+ rdtsc 拆分 + 模型 |  |
| M=3L 的 lambda 地板（r=2） | lam=c_t+2*c_s*F_slab=0.4395 ns/B => 税 12.8%（a_mut=0.34）；现状 r_eff=2.83: lam=0.2508 / 税 7.8% | T2 模型 | 模型（c_t/c_s/F_slab 实测） |  |
| r_eff 2.83 -> 2 的交换 | 省堆 29%（H: 2.83L -> 2L，M=L+kappa_M*H: 3.18L -> 2.54L）；付墙钟 +5.8% | T2 模型 | 模型 + KAPPA_M 实测 |  |
| 5% 税在 M=3L 下不可达 | 需 c_t <= 0.129 ns/B（实测 0.411，-69%）；即使 sweep->0 也只能到 lam=c_t=0.411 => 税 12.1% | T2 模型 | 模型（不可达证明） |  |
| promote 占用比 | RSS=6.65MB; M_max=5.13MB (RSS/M_max=1.30x); RSS/floor=6.34x | T2 默认 | /usr/bin/time -v 的 Maximum resident set size |  |
| sizeratio 占用比 | RSS=3.32MB; M_max=0.34MB (RSS/M_max=nanx); RSS/floor=3.16x | T2 默认 | /usr/bin/time -v 的 Maximum resident set size |  |
| chase 占用比 | RSS=75.46MB; M_max=69.21MB (RSS/M_max=1.09x); RSS/floor=71.96x | T2 默认 | /usr/bin/time -v 的 Maximum resident set size |  |
| cycleratio 占用比 | RSS=3.11MB; M_max=0.09MB (RSS/M_max=nanx); RSS/floor=2.96x | T2 默认 | /usr/bin/time -v 的 Maximum resident set size |  |
| 大活集形状 M 已在下界带 | M_min=L*(1+r_min)=2.69L；但实测 min r_eff=2.425 => 保守 M>=3.42L 才能 keep-up；现行 M_max=3L 与峰值 2.76L_peak（digraph 1M） | T2 模型+实测 | 模型 + calprof |  |
| 小活集形状内存由 floor 决定 | sizeratio RSS/floor=3.16x、cycleratio 2.96x、promote 6.34x；M_max 甚至 < floor（墙不可达） | T2 默认 | /usr/bin/time -v + [mmax] 日志 |  |
| == F2. E18 floor 下探实测（Lead；FREEZE v4 生成 C 重编 -DEXTC_RC_FLOOR，--pin 6，3 reps） == |  |  |  |  |
| sizeratio floor 下探 | 1MiB: 3244-3372 kB / 21.9-23.9 ms；256KiB: 2088-2092 kB / 26.4-27.2 ms；64KiB: 1896-1900 kB / 30.1-31.5 ms | T2 默认（重编） | v2run --pin 6 / 3 reps（Lead） |  |
| cycleratio floor 下探 | 1MiB: 3036-3036 kB / 13.0-14.4 ms；256KiB: 2076-2076 kB / 13.7-13.7 ms；64KiB: 1692-1692 kB / 18.6-19.1 ms | T2 默认（重编） | v2run --pin 6 / 3 reps（Lead） |  |
| promote floor 下探 | 1MiB: 6492-6492 kB / 41.6-43.1 ms；256KiB: 6492 kB（无变化）；64KiB: 6492 kB（无变化） | T2 默认（重编） | v2run --pin 6 / 3 reps（Lead） |  |
| E18 结论 | floor 只影响「活集 ≪ floor」的小堆形状（promote 活集 1.8MB > floor => 无关）；256 KiB 是拐点（64 KiB 再省 0.2-0.4MB 但墙钟 +35~38%）=> 暴露为运行期旋钮、默认 1 MiB 不变 | T2 默认 | E18 |  |
| G-MF4 判据修正 | floor 下探后 RSS/floor 自动变大（256KiB 时 8x）=> 判据改成绝对 MB：sizeratio/cycleratio 峰值 RSS <=2.2 MB（256KiB 实测 2.09/2.08 MB） | 草案 | panel (f) + E18 |  |
| == G. memory-first 门禁草案（固定 M=3L，不得抬 r/M_max） == |  |  |  |  |
| G-MF1 墙钟 | 每形状 wall 降幅 >=10%（按**可达**杠杆判）：promote 只有 L2(15.4%) 可过；cycleratio L1b 6~9.3% + L2 1.0% 临界；sizeratio L2 1.8% 不可过；chase GC 侧上限 4.1% 豁免 | 草案 | panel (e) |  |
| G-MF2 lambda | r_eff=2.83 下 lambda <=0.35 ns/B（当前 0.2508 已过）；M=3L(r=2) 下 <=0.44 已过但税 12.8% | 草案 | 模型 |  |
| G-MF3 RSS | RSS <=1.35x M_max（promote 1.30 贴线；chase 1.09 通过；sizeratio/cycleratio 走 G-MF4） | 草案 | panel (f) |  |
| G-MF4 小堆 | 峰值 RSS <=2.2 MB（绝对 MB；floor 下探后 RSS/floor 会变大到 8x）：floor=1MiB 时 3.24/3.04 MB，floor=256KiB 时 2.09/2.08 MB（E18） | 草案 | E18 + panel (f) |  |
| G-MF5 停顿 | p99 <=5us、max <=50us、>1ms = 0（沿用 GC-ROOFLINE G1） | 草案 | calprof per-step |  |
| G-MF6 sweep 占比 | sweep/wall <=5%，等效判据=「full-dead slab 覆盖 >=90% 死槽」。current: promote 9.2%（L1 不可达 => 现有元数据下无法通过）、cycleratio 9.5%（L1b 可达）、sizeratio 6.8%、chase 0.0% | 草案 | panel (e) + slab 面积探针 |  |
| == H. quota 模型（P 字节配额 + B 惰性 freelist；task-25 / QUOTA-MODEL.md 的数据源，panel (g)(h)） == |  |  |  |  |
| c_A = c_t + c_s*F_slab | 0.42544 ns/B（每字节一次 trace + 一次 sweep 的单价） | 模型（FREEZE v5 常量） | QUOTA-MODEL §2 T-Q1 |  |
| c* = lambda(r'=2) | 0.43948 ns/B（thr=max(2L,floor) 的合法下界处所需单价 => c* 覆盖整个合法策略空间） | 模型 | QUOTA-MODEL §2 T-Q1 |  |
| Q_lin = 64*tau_s/2^k_s + b*tau_t/2^k_m | 4.8305 ns/B => 过配 c* 的 10.99x / c_A 的 11.35x（原型 k_s=6, k_m=10） | 模型 | QUOTA-MODEL §2 |  |
| keep-up 容许的最松位移 | k_s <= 11.03、k_m <= 11.94（pops 通道更紧）；原型 6/10 => 余量 sweep 33x / pops 3.85x | 模型 | QUOTA-MODEL §2 |  |
| 内存界（真界化） | M_peak <= M_max + max(L,floor) + 0.686 L + n_max；L=753,504 B、M_max=3L=2.26 MB 时界 = 3.83 MB | 模型 + 源码契约 | QUOTA-MODEL §3 T-M2 |  |
| P_step 固定预算（b=256） | 4.71 us；5 us 门禁余量 0.288 us = 9.8 个 sweep word => 纯 sweep 配额可容 n <= 1141 B | 模型（tau_t/tau_s 实测） | QUOTA-MODEL §4 |  |
| 停顿∝n 的可接受性 | 该次分配自身 memset 0.125 ns/B，而 pointer 扫描 11 ns/word = 1.375 ns/B => 常数比 11x，不能无条件接受 => CAP 必须存在 | 估算（memset 带宽未实测） | QUOTA-MODEL §4.2 |  |
| SCAN_CHUNK 与 b 的联合约束【已作废，见 P 段】 | chunk=2048 字(16KiB)=22.5 us、512 字=5.63 us 均不可行；256 字 => max b=125；128 字 => max b=205；64 字 => max b=246 => 5 us 门禁要求 chunk <= 256 字且 b <= 128~205 | 模型 + [实测 11 ns/word] | panel (h) |  |
| P 原型实测：pd_bigobj 60 轮 | base 745.9 ms / 493,400 kB -> P 113.5 ms / 26,072 kB（时间 -84% / 内存 -97.6%）；200 轮 1,272.9 ms / 1,067,288 kB -> 202.4 ms / 26,072 kB | FREEZE v5 生成 C（build-lead5） | P0-PROTOTYPE §4.3 |  |
| P 对 v2 四形状中性 | wall 44.62->40.25 / 22.91->22.22 / 12.76->12.80 / 169.62->171.01 ms；RSS 均不变（5 reps 交错 median） | FREEZE v5 生成 C | P0-PROTOTYPE §4.4 |  |
| B：GC 步总时降幅 | promote -12.5%（17,752->15,528 us）、sizeratio -51.7%（2,581->1,247）、cycleratio -8.4%（471.8->432） | FREEZE v5 生成 C | P0-PROTOTYPE §1.3 |  |
| B：单步 max 降幅 | 160.4->57.5 us（-64%）、51.8->9.5（-82%）、39.0->16.9（-57%） | FREEZE v5 生成 C | P0-PROTOTYPE §1.3 |  |
| B：s_link 拟合 | promote 0.541 / sizeratio 0.793 / cycleratio 0.103（= -dT_step/phi_sweep，phi 用 build-th rdtsc 拆分） | 模型拟合（phi 为 tier2 实测） | QUOTA-MODEL §5.2 |  |
| B：max 步预测 vs 实测 | 预测 0.459/0.207/0.897 vs 实测 0.358/0.183/0.433 => 误差 +28%/+13%/+107%（cycleratio 失效：98% 死槽在 full-dead slab，v5 已有批量重置） | 模型 vs FREEZE v5 原型 | QUOTA-MODEL §5.3 |  |
| B：lambda 重算（r_eff=2.83） | delta_lambda = -0.0118 / -0.0172 / -0.0022 ns/B => 净变化在 harness 抖动（+-5%）内；实测墙钟 +2.3% / +1.5% / -9.4% | 模型 + FREEZE v5 原型 | QUOTA-MODEL §5.4 |  |
| B 适用范围（洞密度门槛） | 洞密度 > c_w/(64*b_link) = 3.9%~6.0%；1 MiB slab（24,672 槽）=> nfree >= 964~1483 槽（c_w≈5 ns/word、b_link 1.3~2.0 ns/slot） | 估算（c_w/b_link 未标定） | QUOTA-MODEL §5.5 |  |
| == I. quota 门禁草案 == |  |  |  |  |
| G-Q1 内存 | pd_bigobj 200 轮峰值 RSS <= 3L + 8 MiB（原型 26,072 kB 已过；base 1,067,288 kB 不过） | 草案 | P0-PROTOTYPE §4.3 |  |
| G-Q2 停顿【已作废，改为通道化，见 J 段】 | 配额开启后 p99 <= 5 us 且 max <= 50 us => 必须同时把 SCAN_CHUNK 降到 <= 256 字、b <= 128~205 | 草案 | panel (h) |  |
| G-Q3 中性 | 四 v2 形状墙钟 +-3%、RSS 不变（原型已过：-9.8% / -3.0% / +0.3% / +0.8%） | 草案 | P0-PROTOTYPE §4.4 |  |
| G-Q4 B 正确性 | G1/G2 的 max 与 p99.9 改善 >= 30%；计数档 218 断言 + ASan/UBSan 全绿 + 新断言「同一槽只入链一次」 | 草案 | P0-PROTOTYPE §1.4/§5 |  |
| == P. P1 修正（v7 冻结口径；锚点 build-p1/extc b9fd5784，A 段锚点仍为 build-th） == |  |  |  |  |
| P1：SCAN_CHUNK / 段步（pd p99.9） | 128/256/512/8192 字 = 1.65/1.84/8.19/34.08 us（dl800）；dl0 = 0.65/0.89/1.68/87.28 us => 旧断言「降到 128 是 5 us 前提」作废 | T2 v7 | P1-RQ3-CHUNK §2 [segsteps] |  |
| P1：dense 段步（dl0 p99.9） | 128/256/8192 = 1.34/3.54/59.71 us => 冷密形状按 1 ms 口径判，不参与 5 us 门禁 | T2 v7 | P1-RQ3 §2 表 A |  |
| P1：seg 通道拟合 | pd c_fix=1.14 us + 4.02 ns/word；dense 0.52 us + 6.82 ns/word（chunk=512 离群 +157% => 用 2x 保守界） | 模型 | 本文件 §6.7；panel (i) |  |
| P1：段效率 E(chunk) | pd 31.2/47.6/64.5/96.7%（128/256/512/8192）；dense 62.8/77.2/99.1% => chunk 越小固定开销占比越高 | 模型 | panel (i) |  |
| P1：markResume 快路径 | 一步内连续续扫多段、按字数计费 => W*(5us)=961 字（pd）/657（dense），远大于 chunk=128；a 上限 1.54/1.05 GB/s（sigma=1） | 模型 | MODEL-UPDATE-P1 §3.3 |  |
| P1：chunk 的内存代价 | pd_bigobj 200 峰值 26.6 MB(8192) -> 215 MB(128) = 8.1x，而 deadline 窗口可扫字数比仅 3.1x => 反馈放大，3.1x 是下界 | T2 v7 | P1-RQ3 表 B、§7 |  |
| P1：配额记账不是成本 | sizeratio 2M 分配仅 174 次 markAdvance、deadline checks=0；-D 变体 Ir 差 <0.1% => 旧「sizeratio 回归 = 配额记账」作废 | T2 v7 | P1-QUOTA-FASTPATH §0 |  |
| P1：sizeratio Ir 回归真因 | B 惰性 freelist 复用链重建 2,020 次 / 1.89M 洞 ~ 19.5M Ir；B 默认关（EXTC_RC_LAZY_FREE=0） | T2 v7 | P1-QUOTA-FASTPATH §0 |  |
| P1：elide 改 O(1) 卷计数器 | cycleratio Ir 273.19M -> ~264.3M（净省 8.73M，约 -3.1%），墙钟 15.51 -> 15.38 ms（-0.84%），切片 1221 -> 255（-79%） | T2 v7 | P1-QUOTA-FASTPATH §8 |  |
| P1：regionFree 通道 | pd_bigobj200 18.6-22.7 ms -> 663 us（sync 0.5 + queue 662.5）；dense 1,708 us（单块 8 MiB munmap 不可切）；pb 100M 14,762 us（sync 13,755.6）=> RED | T2 v7 | P1-REGIONFREE-BATCH；panel (j) |  |
| P1：1 ms 契约的 slab 上限 | S <= 1782 slab（561 ns/slab）；超出需 region 级 closing 队列（O(1) 挂链 + closing 标志拒绝 owns） | 模型 | MODEL-UPDATE-P1 §3.4 |  |
| P1：吞吐代价（相对 HEAD） | promote 1.054 / sizeratio 1.050 / chase 1.004 / cycleratio 1.007；callgrind promote 115.21M -> 129.13M Ir（+12.1%）= v4 内存墙 8.2% + quota 2.5% + 卷计数器 0.95% | T2 v7 | GC-ROOFLINE §6.7.5 |  |
| P1：P 通道判据 | P_step = max(P_adv, P_seg, P_sweepQ, P_rf, P_rel)；单值 5 us 口径作废，按通道给界（见 J 段） | 模型 | MODEL-UPDATE-P1 §3.1 |  |
| == J. G-P1 门禁（P1 落地后的判据草案） == |  |  |  |  |
| G-P1-1 seg 通道 | p99.9 <= min(1.5*c_seg*chunk+1us, 64us)；chunk=8192 时 1.5*4.02ns*8192+1 = 50.4 us；over1ms=0；n<1e5 判 N/A | 门禁 | GC-ROOFLINE §6.7.4 |  |
| G-P1-2 warm 尾部 | >50 us 占比 <= 1e-4 且 p99.9 <= 10 us（按 [segsteps]/[chan] 分通道；max 只作 OBS） | 门禁 | GC-ROOFLINE §6.7.4 |  |
| G-P1-3 cold-dense | tau_t <= 2 us、p99.9 <= 1 ms、over1ms=0（不参与 5 us 门禁） | 门禁 | GC-ROOFLINE §6.7.4 |  |
| G-P1-4 regionFree | 常态 <= 100 us/步、强制 <= 1000 us/步；sync 段只承诺 S <= 1782 slab，更大 region 登记契约外 | 门禁 | MODEL-UPDATE-P1 §3.4 |  |
| G-P1-5 配额记账 | B=0 时相对 Ir 回归 <= 0.5%（实测 <0.1%） | 门禁 | P1-QUOTA-FASTPATH §0 |  |
| G-P1-6 elide | 切片数 -70% 以上且 p99.9 不升；EXTC_RC_ELIDE_VERIFY=1 时 elideMismatch=0 | 门禁 | P1-QUOTA-FASTPATH §0、§4 |  |
