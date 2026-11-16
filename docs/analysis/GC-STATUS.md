# 回收运行期：完成度与门禁地图

> 口径：v32 候选工作树（2026-11-16）。本文只回答三件事：**哪些判据在支撑「回收已完成」这句话**、**哪些地方还没有判据**、**动回收时该跑什么**。
> 机制描述在 `MEMORY-MODEL-CURRENT.md`，契约句在 `docs/design/RECLAMATION.md`，模型与标定在 `GC-ROOFLINE.md` / `CALIBRATION.md`，三次路线裁决分别在 `COUNT-TIER-ADR.md`、`UNLINK-DEFERRAL.md`、`CONVERGED-MODEL-PROTO.md`。本文不重复它们。

## 0. 一句话状态

- 正确性、内存界、停顿界、工作量界四条主线**都有可执行的判据**，且一次全绿（§4 是这一轮的原始输出摘要）。
- 仍开的口子集中在三类：①**度量口径**（标定表陈旧、LAT 不进 `all`、若干 N/A 项）；②**覆盖面**（位图登记的稠密侧、计数档的可用语料、形式化边界）；③**记录在案但未修的实现洞**（L5/L6、L10-a、R17/R18）。全表见 §5。
- 一条纪律贯穿全表：**N/A 不是 PASS**。样本不足、通道没触发、桶界不可证，都记 N/A 并停在汇总的最差行上。

## 1. 口径与指纹

E1/E3 之后两个大文件是「数组头 + 分节 `#include`」，编译器真正读的是渲染出来的单流文本。**溯源只认渲染文本的 md5 与生成 C 的逐字基线，不认 `build/extc` 的 md5**（DWARF 记录分节文件名与节内行号，同一份目标码换一种分节就会变）。

| 对象 | 形态 | 指纹 |
|---|---|---|
| `src/back/reclaim.c` | 数组头 241 行 + 24 条 `#include "rt/*.inc"` | 渲染文本 4,805 行、`93a53fde452c61b245db80fbfc3d1ba0` |
| `src/back/codegen.c` | 数组头 75 行 + 22 条 `#include "cg/*.inc"` | 渲染文本 16,429 行、`aaaaf36f73ff859044acb222934ce9c8` |
| `tests/reclaim/driver.c` | 单文件 | 1,637 行、`28dd4009ac7207f47e581a697c0d421f` |
| `build/extc` | 本机构建 | `cac2ac8c7f3de6e95587d938027e30cd`（随分节/注释变动，不作溯源） |
| 生成 C 逐字基线 | `tools/emitted_identity.py` | 215 个源 × {default, reclaim} = 430 行 |

取指纹的命令：

```sh
python3 tools/source_split.py --md5        # 两棵分节树的渲染文本 md5
python3 tools/emitted_identity.py --check  # 430 行生成 C 逐字节比对
md5sum tests/reclaim/driver.c              # 测试驱动是单文件
```

行为改动之后必须重冻结基线并写明理由：`python3 tools/emitted_identity.py --write --set core --why '<一句话>'`，理由写进表头第 4 行。

## 2. 门禁地图（172 项，按「它证明什么」分层）

`./check.sh` 只是编排层，检查项是数据（`tools/check.py`）。分组与项数：build 1 · core 2 · gate 61 · tests 67 · tools 4 · seam 9 · docs 13 · bench 15。`nobench` 跑除 bench 之外的全部；`docs` 只跑文档/工具项；`bench` 档（扰动普查、分片有界、标注表缩放）只在 `./check.sh` 全量里跑。

### 2.1 结构判据（源码形态：不许把承诺写成注释）

| 项 | 它证明什么 |
|---|---|
| `check-source-split` | 每个 `#include` 都有文件、没有空分节、渲染无残留（分节树 == 单流） |
| `gate-rt-defaults` | 运行期注释里声称的 `EXTC_RC_*` 默认值 == 产物里的 `#define` |
| `gate-skip-table` | 每处 `if (…) return` 都带 `/* skip:<id> */`，且与文档跳过表双向对齐（本轮 79 类标记、未标记早退 0） |
| `gate-invariant-table` | 运行期里的每个「义务 N」都在契约表里 |
| `gate-note-part` | 会登记跨区边 / 会交出 rc 槽的函数必须记 `partN`（单区域快路径的安全前提） |
| `gate-rc-plan` | 回收计划七个字段集固定、只在拥有它的函数里写、退化单向、idle 证明在 forced 返回之前 |
| `gate-inject-anchors` / `gate-doc-refs` | 32 条注入的 41 处原文各命中恰好 1 次；每条注入都出现在它声明的活文档里 |
| `gate-ownslab-body` / `gate-slot-unit` | 标记步里的 `ownSlab` 副本与原件逐字相同；槽粒度 8 字节的取整/下限/溢出/穷举 |

### 2.2 语义判据（正确性）

| 项 | 它证明什么 |
|---|---|
| `tests` / `tests-assert` | 例子与反例全量；断言版在 `EXTC_DBG=1` 下跑到兜底即 abort |
| `reclaim-unit`（`tests/reclaim/run.sh`） | 判据驱动 216 条断言 + ASan/UBSan，含不内联档与「分段扫描关闭必须红」的自测 |
| `gate-hole-c` | 内层帧不得扫外层帧的家区域（开关两档输出逐字 + ASan 干净） |
| `gate-reclaim-evidence` | 关档涨 / 开档平 / 清扫永不触发涨（三方对照）+ gdb 数清扫次数 |
| `gate-foreign-skip` | 每个 `rsDec` 都被 `foreign` 把门、每个 `rsInc` 同行置位，且运行期守卫 `checks>0 bad=0`；具名豁免只有 `extc_rc_unlinkChildren` 与 `extc_rc_decOldRef` 两条，各自写清「为什么规则对它不适用」 |
| `gate-container-refs` / `gate-par-tls-owner` / `gate-coro-frame-roots` | 容器描述符、线程本地 arena 归属、协程帧字段根三处的发射形状各 6–9 条注入全红 |
| `tests/gc-torture` | 11 个对抗程序 + 一个 506 行 Datalog 真程序：stdout 逐字、RSS 上界、`over1ms=0`、`[wbudget]` 三上界 |

### 2.3 有界性判据（内存界 / 停顿界 / 工作量界）

这是「两个界」唯一的执行面。判据写作常量，不写作散文：

| 判据 | 常量与位置 |
|---|---|
| 一步 pop 上界 | `EXTC_RC_MARK_STEP_POP_MAX=512`；`tests/gc-p0/g1c.py` 的 `POP_MAX_LIMIT=512` |
| 一步 push 记账 | `EXTC_RC_MARK_PUSH_STEP=2048` + 批 200 × 扇出 32 ⇒ `PUSH_MAX_LIMIT=8448` |
| 标记栈软上限 | `EXTC_RC_MARK_STACK_SOFT=8192`（门禁按 2× 记 `MSCAP_LIMIT=16384`） |
| 描述符扇出 | 超 32 个引用域退回保守扫描（`FANOUT=32`） |
| 步内扫描额度 | `EXTC_RC_SCAN_CHUNK=8192` 词 |
| warm 常规通道 | `>50 µs` 占比 ≤ `1e-4`（扣空转噪声）且 p99.9 ≤ 10 µs |
| cold-dense 通道 | `tau_t = 步时/pops ≤ 2 µs`、p99.9 ≤ 1 ms、`over1ms=0`；`>50 µs` 只作观测 |
| 段步通道 | p99.9 ≤ min(1.5·c_seg(shape)·chunk + 1 µs, 64 µs 桶界)，`over1ms=0`，`max` 只观测 |
| 容量/释放通道 | `regionfree` / `release` / `trigger-step` 的 `max ≤ 1 ms`（目标 ≤ 0.2 ms） |
| 内存墙 | G18：`peak200 ≤ 3·live + 2·SLAB` 且 `≤ peak60 × 1.05`；G4-Stress：4× 注入下 `VmHWM ≤ M_max + 8 MiB`、末段相邻轮差 ≤2%、A<B<C |
| 线性泄漏 | L10：`RSS(800k)/RSS(50k) ≤ 1.25` 且 `rs_live ≤ 8` |

执行者：`tests/gc-p0/run.sh all`（G1①②、G4、G8、G9、G12–G16、G18、G20、G3b、L10、G15、G17 观察、LAT）、`gate-reclaim-work`（`[wbudget]` 的 walk/scan/push 三上界）、`gate-reclaim-idle` / `gate-idle-projection` / `gate-rc-idle-corpus`（被判 idle 的形状必须真的有界且产物逐字节 == 关档）、`tests/gc-torture`。

### 2.4 性能判据（棘轮：只许往下拧）

| 项 | 判据 |
|---|---|
| `gate-reclaim-perf` | `cyclic_pressure` N=200000 的 callgrind 总指令数 ≤ **88,029,115** |
| `gate-reclaim-perf-shapes` / `gate-reclaim-tax` | `bt` / `rebuild` 小输入各自的线；`bt` 开档/关档指令数比 ≤ 现行税线 |
| `gate-slab-scale` / `gate-pin-scale` / `gate-reclaim-tables` | 每分配 / 每钉一个 / 每次 add+del 的成本不随规模涨（N vs 4N ≤1.15、绝对值 ≤400），且注入线性形状会变红 |
| `gate-mark-locality` | 批量弹出不许被拆：P5 的 DLmr ≤ 190000、D1mr ≤ 700000 |
| `gate-reclaim-conc` | 登记表按代发布、无锁读者重试、线程本地热缓存、原子写屏障结构 + 8 路并行 ASan |

### 2.5 规模判据（N vs 4N）与语料判据

- `gate-reclaim-switch`（30 条形状：语义一致 + N vs 4N 峰值/登记量 < 1.5×）、`gate-shard`、`gate-shard-cycles`、`gate-reclaim-count`（计数档 31 条棘轮）、`gate-reclaim-count-cross`（三语言校验和一致）。
- `gate-reclaim-ledger`：全语料红计数（silent-both + owe-no-settle）与基线逐条相同，**只许缩**。这是「能力账本」，防止某个形状悄悄从「能判」退化成「判不了」。

### 2.6 文档判据

| 项 | 它证明什么 |
|---|---|
| `check-doc-anchors` | 三集：STRICT（只许引符号）· RATCHET（数字锚点只许减）· FROZEN（钉在旧树快照，头部写 md5 与行数）；所有符号必须能解析 |
| `check-no-history` | 活文档与注释里不许写历史（历史只进 `docs/CHANGELOG.md`）；`docs/analysis` 例外 |
| `check-changelog` | 条目落在当月页，索引月份表与月份页一致 |
| `check-code-examples` / `check-doc-examples` / `check-doc-facts` | 注释里的 `@code` 与活文档围栏里的 extC 程序必须真能编过/跑过；文档里写的 API 必须存在 |

## 3. 一次全绿的快照（2026-11-16，`taskset -c 6`，串行）

**注意**：`build/extc` 的 md5 是 `cac2ac8c7f3de6e95587d938027e30cd`，源码指纹见 §1。

| 命令 | 结果 |
|---|---|
| `./check.sh nobench` | 通过 **153**，跳过 2，失败 **0** |
| `./check.sh docs` | 通过 **14**，失败 **0** |
| `bash tests/reclaim/run.sh` | 通过 **216**，失败 0（不内联档同） |
| `bash tests/reclaim-perf/gate.sh` | **87,911,848** ≤ 88,029,115 |
| `bash tests/gc-torture/run.sh all` | 通过 **14**，xfail 0，失败 0 |
| `EXTC=./build/extc PAUSE=1 REPS=3 OUT=<dir> bash tests/gc-p0/run.sh all` | 逐用例 PASS，见下表 |
| `python3 tools/emitted_identity.py --check` | 430 行逐字节相同 |
| `python3 tools/source_split.py --check` | 24 + 22 节自洽 |
| `python3 tools/check_rc_plan.py` | 7 字段 / 24 处写 / 全在属主函数内 |

`gc-p0` 逐用例（同一 `OUT` 目录的 `summary.txt` 是权威读数）：

| 用例 | 读数 | 判定 |
|---|---|---|
| G8 档 1 完整性 | T1/T2 VmHWM = 0.467× | PASS |
| G9 任务根 | 三次 stdout 逐字 `live0=1 freed=0 live1=1 / tag=777 / got=20 steps=5` | PASS |
| G12 / G13 | 计数档与默认档 `alias check b.v=999` / `d0r=7` | PASS |
| G14 循环 copyInto | 800k 对照比 1.001×、3,428 kB | PASS |
| G16 sweep 中新建 slab | 三次 exit 0、输出正确 | PASS |
| G18 大对象反复分配 | 60 轮 = 200 轮 = 26,272 kB ≤ 26,624（3·live+2·SLAB） | PASS |
| G4 内存界 | pb 18.66 B/node、pc 3.29 MB、cyc N vs 4N 1.000× | PASS |
| G4-Stress（4× 注入） | 8,988 < 37,940 < 75,416 kB，末段相邻轮差 0.00% | PASS |
| L10-b / L10-c | 1.00× / 1.00×，`rs_live` 0 / 1 | PASS |
| G1② (a) warm | cyc `9.59e-05`、pb `2.39e-05`、pc `1.62e-05`（阈 1e-4） | PASS |
| G1② (a) cold-dense | `pd_pointer_dense` `1.17e-05`、`tau_t=11.397 µs`（该通道不判 5 µs 门禁，只判 ≤2 µs 并作 OBS）、p99.9 PASS | PASS |
| G1② (b) 段步 | `pd_bigobj` n_seg=122,741 p99.9 PASS；`pd_bigobj60` n_seg=7,487 **N/A**（样本 <1e5） | PASS / N/A |
| G1② (c) 容量通道 | `regionfree` 最大 1.3 µs、`release` 3.4 µs、`trigger-step` 198.1 µs（≤1 ms） | PASS |
| G1② 汇总最差 | N/A（`pd_bigobj60` 样本不足） | N/A ≠ PASS |
| G15 | 13.735× > 1.20× | **XFAIL(known L10-a)**，预期红 |
| G20 | `tag=gate32`、四形状 wall/RSS/stepTot 落盘 | 观察（跨 tag 用 `g20cmp`） |
| G17 | A=25.0 / D=23.0 / B=57.1 / C=57.1 MB（A<B≈C） | 观察（饱和归因见 D11） |

## 4. 欠账表

每一行都写了**为什么现在不修**与**重开条件**。这张表只许变短；新增行必须同时写重开条件。

| # | 欠账 | 现象与证据 | 为什么现在不修 | 重开条件 / 追踪处 |
|---|---|---|---|---|
| D1 | idle 投影欠账 7 条 | `tools/gate-idle-projection-known-bad.txt` 列 7 个仍发射回收运行期的形状（`spectrum_p2/p4`、`rg_walk_tree/rg_scan_parent/rg_walk_buckets/rg_insert_hash/rg_scan_graph`）；该组本身 ok，FAIL 行是欠账清单 | 这 7 个形状的 idle 证明需要新的分析（跨函数/容器/图遍历），不是回收器的活 | 清单只许删行；每删一行跑一次 `gate-idle-projection` |
| D2 | LAT（延迟屋顶形状）不进 `all` | `tests/gc-p0/run.sh lat` 存在、判据脚本 `lat_check.py` 有牙，但 `all` 里以 `|| true` 收尾；`R15` 说明该形状只能承诺均值 + 尾部率 | 该形状的 p99.9 尾部（64–128 µs 桶）机制未定位（TLB/页错误级），做成硬门禁会在机器抖动下翻红 | 尾部机制有归因实验后进门禁；`GC-ROOFLINE.md` §8.1 的 R15 |
| D3 | `pd_bigobj60` 的 (b) 记 N/A | n_seg=7,487 < 1e5（p99.9 样本不足） | 该用例设计就是 60 轮；抬轮数会改变 G18 的对照口径 | 需要 p99.9 时用 200 轮分支；`g1c.py` 的 `MIN_N=100000` 不回退 |
| D4 | L10-a / G15：池块里的引用永久 pin | 50k = 13,208 kB，800k = 181,416 kB（13.735×）；`rs_inc=0` | 池块是 foreign memory，只能靠 pin 表钉住；撤 pin 需要池块级的分代/写入登记，属池基质改造 | XFAIL 只许删不许加；`tests/gc-p0/run.sh g15` |
| D5 | 标定表陈旧（`bseg-cal.tsv`） | 表里 `c_seg`：`pd_bigobj` 3.841、`pd_pointer_dense` 5.997（2026-11-13）；今天同核同小时复测得 6.3–9.9，而含那一代代码的构建（`49d76213`）同样 6.294 / 9.920 ⇒ 漂移在机器不在源码。`summary.txt` 每次打印「标定行陈旧 ⇒ 必须重标定」 | 把今天的数字写进表会把门禁 (b) 放宽约 1.6–2×，违反「判据只严不放宽」 | 机器访存状态回到标定窗口（`memhier` 64 MiB 点 ≈146–157 ns）后重跑 `calib_cseg.py --rounds 1000 --min-n 100000`；`CALIBRATION.md` §17 |
| D6 | 位图登记的稠密侧 | 契约 `W_unlink(ss,m) ≤ min(ss/8, 8·m)` 的右端（每个 64 B 组都写过跨区指针）在语料里 0 个样本；`wmUnsafe`（整值拷贝/越界）会让该区永久退回全扫描 | 稠密侧的真实程序未出现；修法要么「按域数计费/声明」（A 路线），要么改布局（C 路线，需语言层拍板） | 出现稠密样本时先加 torture 用例再改机制；`WMARK-REACH.md`、`GC-ROOFLINE.md` §8.1 的 R17 |
| D7 | 计数档只用得上 2/794 | 语料 census：544/794（68.5%）不发射运行期、248 清扫档、2 计数档；强制事件档实测 churn 1.9×、chase 2.25×、promote 3.25× 慢，且有环形状强制 T1 反而涨内存（5.7 MB → 130 MB） | 路线裁决已定：默认清扫档、计数永不进对象头（确定性布局约束）、事件档只由无环证明给 | 只在「无环证明的覆盖面」变化时重开；`COUNT-TIER-ADR.md` 的 5 条重开条件 |
| D8 | 形式化只覆盖设计语义 | `proofs/*.lean` 13 模块、2,555 行、无 `sorry`/无 `axiom`：覆盖 World/Block/Region 的活体、Safe、精度、idle；对发射出的增量运行期（`rt/*.inc`）零建模。论文表 4 的定理 11/12/13 由预算计数 + 门禁承担 | 全量形式化是把「步机器 + 不变量 + 无条件进展」做出来，属独立工程（窄切片：Step 关系 + `work(step) ≤ K`） | 窄切片立项时开工；`paper/chapters/formal.tex` 表 4 |
| D9 | L10-c 的已知边界 | 拷贝撤销旧引用边的 pre-pass 只对「引用域可枚举」的元素类型发射；保守描述符（>32 引用域）元素没有 pre-pass | 保守元素的引用位置只能运行时扫描，做 pre-pass 等于把标记侧的成本搬到写侧 | 出现「大槽元素 + 拷贝覆盖」形状时再评估；`COMPILE-SIDE.md` §14.2 |
| D10 | L5 / L6 记录未修 | L5：并发 worker 下 region 表无锁（单 owner 线程模型下不触发）；L6：`noSweep` 置位后无清零路径 ⇒ 该 region 永不回收 | L5 需要每 worker 预建 region（并发模型改动）；L6 的出路是「分区 + 搬移」而不是加一个清零点 | 并发模型或分区落地时一起做；`GC-ROOFLINE.md` §8.2 的 L5/L6、§6.3 |
| D11 | G17 不满足 A<B<C | A=25.0 / D=23.0 / B=57.1 / C=57.1 MB：B 以上饱和。饱和原因是触发阈值 `thr`（自学习 scale）先于 `M_max` 成为约束 | 这是设计选择（触发律先于墙），不是缺陷；改它要动触发律 | 若要让 `M_max` 成为唯一约束，需把 `thr` 的 ceiling 与墙解耦；`GC-ROOFLINE.md` §7 的 G17 |
| D12 | 巨块丢弃的暂停与请求同阶 | `[chan] bigdrop` 在 grow11 上 max=1,389.7 µs（47 MB 丢弃），且不进 `[step]` 直方图（分配路径不计步） | 压速率会让容量重新跟随各代之和（4 MiB 档 578,444 kB）；要常数上界只能把丢弃挪到后台线程（当前运行期无独立线程） | 契约里保持 `P_step ≤ … + c_drop·N_req`（`c_drop=0.0295 ns/B`）；`GC-ROOFLINE.md` R18 |

### 4.1 与「GC 完成六项」的对应

| 六项 | 现状 |
|---|---|
| ①所有停顿/内存门禁可判（消灭 N/A 掩盖） | 已落地：`min` 时基、环境地板、结构性 vacuous、通道拆分；残余 N/A 只剩 D3 一处并有理由 |
| ②两条真红（pb warm 尾部、pc 一次性 1,322 µs） | 已清：本轮 pb `2.39e-05`、pc `1.62e-05`、`(c)` 通道最大 198.1 µs |
| ③L5/L6/L16 | L16（regionfree 通道 ≤1 ms）由 `(c)` 与 G3b 盯着、当前 1.3 µs；L5/L6 见 D10 |
| ④G20 utime / digraph §9.8 / SHARDS=4 归因 | G20 已落盘（`g20.json` + `g20cmp`）；后两项在 `bench` 档（`gate-shard-cycles`、`gate-shard`），只在 `./check.sh` 全量里跑 |
| ⑤计数路线 ADR | 已写：`COUNT-TIER-ADR.md`（5 条决策 + 6 条理由 + 5 条重开条件） |
| ⑥文档收口 + 本文 | 本文件 |

## 5. 判据怎么被验牙

没有验牙的判据会在第一次重构里静默失明，所以每条判据都带一个「注入故障必须变红」的自测：

- `gate-inject-anchors`：`tools/inject.py` 的 32 条注入、41 处原文各命中恰好 1 次（重命名/挪行会让注入打在别处）。
- 注入的故障类型包括：累计桶界（历史 `[histo]` 边界恒真）、静默读钟（`cpu_us` 恒 0 的降级）、一次排空（假设一步能排空 free 队列）、失去 `foreign` 守卫、短批（拆掉批量弹出）。
- 各门禁自带 `--selftest` 与变异：`tools/check_mark_batch.py --selftest`、`tools/check_foreign_skip.py --selftest`、`tests/gc-p0/g1c.py` 的 S1–S35、`tools/check_rc_plan.py` 的 5 个变异、`tools/doc_anchors.py --selftest`。
- 负结果也留档：`UNLINK-DEFERRAL.md`（把大对象的撤销摊到多步被否证）、`WMARK-REACH.md`（位图的覆盖面与未验证端）、`CONVERGED-MODEL-PROTO.md`（收敛模型原型对拍）。

## 6. 复现命令速查

```sh
# 0) 构建（门禁读仓库根的 build/extc）
make -j8

# 1) 日常：改完代码第一件事
./check.sh smoke          # 约 10 s，高信号集
./check.sh nobench        # 约 230 s，非基准全量（153 项通过基线）
#    全量（含 bench 档：扰动普查、分片有界、标注表缩放）
./check.sh

# 2) 回收专用
bash tests/reclaim/run.sh                              # 216 条断言 + ASan/UBSan
bash tests/reclaim-perf/gate.sh                        # Ir ≤ 88,029,115
bash tests/gc-torture/run.sh all                       # 14 案：golden + RSS + 停顿 + 预算
EXTC=./build/extc PAUSE=1 REPS=3 OUT=/tmp/gc-p0 bash tests/gc-p0/run.sh all
python3 tests/gc-p0/summarize.py /tmp/gc-p0             # summary.txt 是权威读数

# 3) 文档与结构
./check.sh docs
python3 tools/source_split.py --check
python3 tools/emitted_identity.py --check

# 4) 标定（先过两道机器状态前置检查）
python3 tests/gc-p0/calib_cseg.py --extc ./build/extc --rounds 1000 --min-n 100000 --reps 3
```

口径纪律（写在这里是为了不用每次重新讨论）：

1. `gc-p0` 必须带 `-D_POSIX_C_SOURCE=200809L` 编生成 C，否则 `CLOCK_THREAD_CPUTIME_ID` 不可见、所有 `[step]` 时长恒 0（假 PASS）。
2. 停顿判据只看 CPU 口径或同时给两种口径；`wall` 含抢占，不能当「这是 GC 工作」。
3. A/B 必须同轮重建两个二进制；RSS 用 `/usr/bin/time -f %M` 或 `VmHWM`，不看采样器的峰值。
4. 标定前跑 `ipeak`（≤1.40× 参考）与 `memhier` 64 MiB 点（≤1.30× 参考）；两道都过不了就不标定。

## 7. 维护规则

1. **判据只严不放宽。** 红就修机制或修量具，不改判据去消红；`N/A` 不许当 PASS 记进汇总。
2. **棘轮只许往下。** Ir 线、税线、idle 欠账清单、`known-bad` 清单、标注表、跳过表，都只许删行/降值。
3. **行为改动要留痕。** 生成 C 变了就重冻结 `emitted_identity` 并写 `--why`；行号变了就写 `REFS-*.md` 的分段换算，**禁止单一 offset 平移**。
4. **历史只进 `docs/CHANGELOG.md`。** 活文档与注释写「现在是什么」，不写「以前是什么」；`docs/analysis` 是记录分析过程的例外。
5. **新增判据必须能红。** 先写红演示（注入故障 ⇒ 判据变红），再让它变绿；`gate-wiring` 要求每个 `run*`/`gate*` 脚本要么在电池里可达，要么在文件头自报 `# manual: 理由`。
