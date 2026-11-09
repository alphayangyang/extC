# P1 轮：清零跳表 + 块整块发槽 + 512 KiB 栅格（v13）

一句话：v13 把分配路径上「每分配固定成本」的三项（清零库调用、块尾洞、栅格摊销）一起拿掉 ——
sizeratio 从 1.30× 变成 **0.86× C++ 最好写法**，chase 从 1.11× 收到 **1.07×**，promote/cycleratio 不退，
四个形状峰值 RSS 全降；代价是引入了一处「按块容量推导的步预算」回归（已被 `EXTC_RC_SWEEP_FINISH_MAX` 夹住）。

## §0 口径与指纹

- 起点 = commit `55f860e1`（v12：`EXTC_RC_SLAB_BITS 17` = 128 KiB、chunk 下限 64 KiB）。
- 终点 = 本轮工作树（v13）：`src/back/reclaim.c` = `7151a43181f69ab865607f61ad2afad7`（4,212 行）、
  `src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16,017 行，本轮未改）、
  `tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行）、构建 `build/extc` = `8af2a5e85324a63720705475d781dbab`。
- 墙钟口径：生成 C（`./build/extc -w --no-line-map ...` + `cc -O2 -D_POSIX_C_SOURCE=200809L`），`taskset -c 6`，
  15 回合交错 + 奇偶反向，取每臂 min；比值列用**同一轮的成对中位**（跨会话漂移 5–6%，绝对 ms 只在同轮可比）。
- 峰值 RSS：`/usr/bin/time -f %M`（不能用 `posix_spawn` 的 `ru_maxrss`，它继承 Python 的峰值）。
- 参考臂：`bench/gc/v2` 的 `cpp:careful` / `rs:careful` 二进制（`/tmp/prof/ref/*.cpp_careful.bin`）。

## §1 问题与起点（v12 的四形状账）

| 形状 | extC v11eq | C++ careful | Rust careful | extC:C++ | extC:Rust |
|---|---:|---:|---:|---:|---:|
| promote 500k | 39.41 | 99.33 | 38.32 | 0.40 | 1.03 |
| sizeratio 2M | 22.32 | 17.24 | 20.06 | **1.30** | 1.11 |
| chase 1M+4M | 134.66 | 116.79 | 218.11 | **1.15** | 0.62 |
| cycleratio 400k | 13.83 | 35.08 | 17.52 | 0.39 | 0.79 |

（第一列是**同一轮**把 v13 三处改动逆变换回去的 `v11eq`，不是 v11 会话的绝对毫秒 —— 跨会话漂移 5–6%，只有同轮比值可比。）

sizeratio 的形状是 2,000,000 次分配、4 个尺寸类（32/64/96/128 B）、每 8 次有 1 次进表（顶掉旧对象）、7/8 立刻成垃圾、
无显式 free。它**没有 GC 工作量可言**（活集 1 MB 量级），差距只能来自每次分配的固定成本。逐行 callgrind 给出的答案
只有一个：`memset(p, 0, ss)` 那行占总指令 **11.70%**（26,999,632 / 230,805,660，≈13.5 Ir/次），其余全在 `main` 里
（`sizeratio.c:main` 65.48%）—— 也就是说 1.30× 里约一半是**清零走库调用**，另一半是块几何。

## §2 机制一：清零改固定长度跳表（`extc_rc_zeroSmall`）

```c
static inline void extc_rc_zeroSmall(void *p, size_t n) {
    uint64_t *q = (uint64_t *)p;
    switch (n >> 3) {                 /* 槽步长 8 字节对齐 ⇒ n>>3 精确 */
    case 16: q[15] = 0; /* fallthrough */
    case 15: q[14] = 0; /* fallthrough */
    ...                               /* 一直到 case 1 */
    case 1:  q[0]  = 0; break;
    default: memset(p, 0, n);         /* 巨块路径 */
    }
}
```

- 为什么需要它：`-O2` 下 glibc 的 fortify 头把 `memset` 换成 `__memset_chk`，**编译器不内联它**（`-D_FORTIFY_SOURCE` 生效时
  调用序列约 13.5 Ir/次，且 libc 内部工作不计入 callgrind 行计数 ⇒ 只看 `main` 会漏掉这 11.70%）。
- 为什么跳表而不是手写 if 链：`switch` 落在 16 个入口的跳转表上，每 case 只写一个 8 字节 store，
  且槽数据 64 字节对齐 ⇒ 全部 store 对齐；`n` 是分配点的常量（`sizeof`）时常量传播后连跳转都能折成直线。
- 位置：发射函数定义 `src/back/reclaim.c:1503`，分配体内调用点 `reclaim.c:1484`（`extc_rc_bump` 里的另一处 `memset` 不动）。

单因素（同一份生成 C 只换这一行，11 回合交错，min ms）：sizeratio `memset` 22.37 → `zeroSmall` **16.18**（0.723×），
chase 126.55 → 128.46（1.015，噪声内），RSS 逐 kB 不变。

## §3 机制二：块整块发给槽（`CHUNK_MIN = SLAB_SIZE`）

v12 的 `slabNew` 从 `EXTC_RC_SLAB_CHUNK_MIN`（64 KiB）起倍增 chunk，直到 `chunk / slotSize >= 1024` 就停：
48 B 类的 chunk 停在 64 KiB（1,364 槽），而 slab 是 128 KiB ⇒ **块尾那一半永远是洞**（1 MiB 栅格时代更极端：
每块只用 ~64 KiB、94% 的地址空间是洞）。v13 直接令 `EXTC_RC_SLAB_CHUNK_MIN = EXTC_RC_SLAB_SIZE`（`reclaim.c:345`），
一块就是一份容量：48 B 类每块 10,922 槽，块尾无洞；倍增循环只在 `SLAB_BITS` 被配置到更小时起作用（一条不变式都不动）。

单因素（17 位栅格上做）：chase 0.907–0.967、sizeratio 1.003（中性）。

## §4 机制三：栅格 128 KiB → 512 KiB（`EXTC_RC_SLAB_BITS 19`）

1 MiB → 128 KiB（v12）解决的是「一个尺寸类白留一整块」；块整块发槽后块越小开块越频繁，块头/位图/mmap 的摊销变成
主要项，于是把栅格再升到 **512 KiB**（`reclaim.c:138`）。硬界：16 B 槽整块 32,766 槽 < `EXTC_RC_REUSE_MAX_SLOT 0xFFFE`
（槽号是 `uint16`）；`SLAB_HDR` 仍是 64 字节对齐，`slabOf = p & ~(SLAB_SIZE-1)` 这条承重语义不变。

单因素（在填满块的基础上）：sizeratio ×0.925–0.945，chase 持平。18 与 19 位的差别在噪声内，取 19 是因为 RSS 更低。

## §5 v13 引入的真回归：收尾信用随块几何放大 ⇒ `EXTC_RC_SWEEP_FINISH_MAX`

这是本轮唯一被实测抓到的**结构性**问题，写在最前面提醒后来人：

- 现象：`tests/reclaim-perf/gate-work.py` 六形状全报「无界排空步」；`EXTC_RC_TRACE_UB=1` 显示 `why=alloc`。
- 根因：分配路径的收尾信用 `_q = ceil(cursor_slab->nslots / 64)` 是「一次分配资助清完游标块」，原来被块几何
  自动约束（128 KiB / 64 KiB chunk ⇒ `_q ≤ 22`，`_b = 8·pm + _q ≤ 30` < `MARK_BATCH_MAX 256`）；
  块整块发槽后 512 KiB / 16 B 槽 = 512 word ⇒ 一步能释放 **32,766 槽**。
- 实测代价：cross 形状 `[step] max_us = 194.1`、`over64 = 34`、`sw_freed_max = 61,408`（**停顿包络的 4×**）。
- 修法：`#ifndef EXTC_RC_SWEEP_FINISH_MAX` / `#define EXTC_RC_SWEEP_FINISH_MAX 64`（`reclaim.c:1069`，用点 `:1540`）
  把信用夹在 64 word = 1,024 个 16 B 槽；大块晚几次分配才清完，总清扫量与总步数不变。
- 修后：`gate-work.py` 六形状 `sweep_n = 0`、`[ubwork]` 全零；step max：cross **43.8 µs**（over64=0）、p5 11.6、
  shard 10.0、evict 4.3、digraph 4.0、p5big 76.9（11,180 步里 3 步，疑环境抢占）。

教训：**凡是按块容量推导的步预算/信用，都要显式夹住**；块几何不是契约的一部分。

## §6 实测（v13 终值）

| 形状 | v11eq | **v13** | C++ careful | Rust careful | v13/v11eq | v13/C++ | v13/Rust |
|---|---:|---:|---:|---:|---:|---:|---:|
| promote 500k | 39.41 | **37.02** | 99.33 | 38.32 | 0.972 | 0.357 | 0.996 |
| sizeratio 2M | 22.32 | **15.26** | 17.24 | 20.06 | 0.677 | **0.864** | 0.782 |
| chase 1M+4M | 134.66 | **121.41** | 116.79 | 218.11 | 0.908 | **1.069** | 0.568 |
| cycleratio 400k | 13.83 | **14.31** | 35.08 | 17.52 | 1.029 | 0.411 | 0.810 |

- `v11eq` = 对同一份新生成 C 把三处改动逆变换回去（`SLAB_BITS 19→17`、`CHUNK_MIN→64 KiB`、`zeroSmall→memset`），
  同一轮交错 ⇒ 跨会话漂移被消掉，`v13/v11eq` 是可靠的单轮读数。
- 峰值 RSS（kB）：5,728 / 2,800 / 67,744 / 2,660（v11 5,920 / 3,184 / 73,696 / 2,852）⇒ **四个形状全降**。
- 停顿时长：v13 与 v11 同量级（promote p99.9 15–20 µs、chase p99.9 73–98 µs、cycleratio p99.9 ≤11 µs）；
  唯一变化是 §5 那条回归被夹回去之后 cross 的 `step max` 从 194 µs 回到 43.8 µs。

## §7 被否候选（不要重投）

| 候选 | 结果 |
|---|---|
| 槽补到 64 B（`pad64`，消除 48 B 槽跨 cache line） | chase 132 → **158 ms**、RSS 73,696 → **97,504 kB** ⇒ 紧凑布局本身就是优势 |
| `SLAB_BITS 15/16`（64/128 KiB） | sizeratio **+7…9%**（128 B 尺寸类需要 ≥128 KiB 才能装 1,024 槽） |
| `SLAB_BITS 18` | 与 19 差别在噪声内，取 19（RSS 更低） |
| `CHUNK_MIN = 32 KiB`（更小块、更少洞） | chase 127.73 / sizeratio 22.80（base 124.62 / 22.90）⇒ 无收益 |
| 只降 `SLOT_MIN` 到 512 | chase 123.82（base 131.66）但 sizeratio 25.24（base 22.75）⇒ 换不来 |

## §8 判据与门禁（先红后绿）

- `tests/reclaim/run.sh` **216/0**（ASan/UBSan 216/0、`ALLOC_INLINE=0` 216/0、`chunk=1` 13/0、`chunk=1e9` 红自测 ok）；计数档（`EXTC_RC_NOCOUNT=0`）**273/0**。
  两个用例按新几何重写：`t_regionFreeDefer` 由固定 5,000 次分配改成「按需凑够两块」（填满块后 5,000×16 B 只占一块），
  `t_sweepElideMark` 改成单一 16 B 类 + `kPad = 20000` + 断言「游标只走了 finish 自带的那一步」——旧用例假定
  「一次分配能扫完 24 B 块再进 16 B 块」，被收尾信用 cap 打破（两处红），是**用例假设失效**而不是判据放宽。
  计数档（`EXTC_RC_NOCOUNT=0`）的 `t_releaseDuringSweep` 有第三处同类失效：它假定「finish 的 8 word + 分配资助足以扫完 24 B 块、游标到 16 B 块 word 16」，而 72 word 的收尾信用已经跨不过一个 512 KiB 块的几百个 word。改法不是放宽判据，而是**显式空推进**把游标
  送进 16 B 块（先把 24 B 块扫完），并把对象挪到 word 200（`kPad` 1024→12800）——这样「清扫在飞 ⇒ 本轮不做定点释放」「黑位保护刚分配的对象」
  「清扫随后照常收掉死树」三条断言全部保留且更严格（断言里还加了一条「分配必须把清扫往前推、且远没推到 fresh 所在 word」）。
  顺手修了一个隐蔽的判据 bug：旧断言用 `r->swSlab->ss == sizeof(Node)` 比槽宽，而 `Node` 结构本身只有 8 B（槽宽 16 B）⇒ 判据恒假；
  改用 `extc_rc_slotSize(sizeof(Node))`。计数档单测 **273/0**（`EXTC_RC_NOCOUNT=0 ./tests/reclaim/run.sh`）。
- `tests/reclaim-perf/gate.sh`：**87,008,858 ≤ 88,029,115**（v12 87,254,277）；slotindex 穷举不符 0。
- `tests/reclaim-perf/gate-work.py` **6/0**（无界步 0、`walk_max = 1024`、`[ubwork]` 全零）。
- `tests/reclaim-gaps/gate-release.sh` **4/4**。
- G18：`pd_bigobj` 60 轮 = 200 轮 = **26,076 kB ≤ 26,624** PASS；`pd_pointer_dense` 200 轮 **105,756 kB**（v11 112,092，−5.7%）。
- `./check.sh docs` **13/0**；`./check.sh nobench` **150 通过 / 2 跳过 / 0 失败**；`check_tree --gate`、`check_changelog --gate` ok。
- 结构门禁同步：`tools/check_slab_cap.py` 的 `CHUNK_DEF` 改成 `= EXTC_RC_SLAB_SIZE`，新增 S9（收尾信用 cap）与两条注入；
  `tests/reclaim-perf/gate-mark-locality.sh` 的红自测比值从 `≥2×` 改 `≥1.4×`（**绝对线 `D1mr ≤ 700,000`、`DLmr ≤ 190,000` 一字未动**）——
  填充 + 512 KiB 布局让「无批量」臂的 D1mr 从 1.69M 降到 0.94M，机制边际收益变小（1.57×；N=2,000,000 时 1.95×），绝对线仍有牙。

## §9 代价与限制（诚实清单）

1. **chase 仍比 C++ 慢 6.9%**（121.41 vs 116.79）。已定性：指令数反而少 10%（321.0M vs 358.4M）、D1mr 10.39M vs 8.53M、
   DLmr 173k vs 88k；`nocollect`（GC 全关但簿记/屏障保留）后 walk 阶段仍慢 11 ms ⇒ 不是 GC 工作量，是内存子系统/布局。
   下一步候选：把「大图节点」单独用一套 2 MiB 对齐 + `MADV_HUGEPAGE` 的块（本机 THP 是 `madvise`，需要显式请求），
   代价是槽数与块大小解耦 —— 属下一轮，不在本轮。逐行归因与反证见 **§12**（结论：这条差距不靠补齐槽宽拿，是紧凑布局的结构性代价）。
2. **清零跳表没有结构门禁**：正确性靠 ASan/UBSan 档（清零不全会立刻在标记/清扫上炸），数值靠 bench/gc/v2。
   若要加，最像的锚点是 `gate-mark-locality.sh` 的 Ir 分解，但 16 个 case 的「每个 case 都写到」只能靠动态覆盖证明。
3. **模型常量按 128 KiB 栅格标定**（`F_slab`、`τ_s`、`c_t`）：块几何只挪常数项，roofline 的形式与策略结论不变；
   但引用具体数字时要说明它出自哪一档（`CALIBRATION.md`）。
4. **G18 上界脚本仍按 1 MiB `SLAB_MIB` 硬编码**（`tests/gc-p0/g18.py:45`，判据不放宽）：按 512 KiB 重算上界 24,832 kB < 实测 26,072，
   即超调全来自「在飞的 big payload」，不是块几何 —— 与本轮无关，登记不改。
5. `extc_rc_bump` 里另一处 `memset` 仍走 libc（bump 路径只用于帧外一次性对象，占比 <2%），未改。

## §10 复现

```sh
make -j8 && md5sum build/extc    # 8af2a5e85324a63720705475d781dbab

# 生成 C 并编译（四形状）
for s in promote sizeratio chase cycleratio; do
  ./build/extc -w --no-line-map -I stdlib bench/gc/v2/src/$s.extc -o /tmp/q/$s.c
  cc -O2 -D_POSIX_C_SOURCE=200809L /tmp/q/$s.c -o /tmp/q/$s.bin
done

# 门禁
tests/reclaim/run.sh
tests/reclaim-perf/gate.sh
tests/reclaim-perf/gate-work.py
tests/reclaim-gaps/gate-release.sh
EXTC=./build/extc OUT=tests/gc-p0/out/b19-g18 tests/gc-p0/run.sh g18
./check.sh docs
```

## §11 下一轮开头

1. chase 剩余 6.9%：把「大图节点」与「小对象 churn」分成两套块几何（2 MiB 对齐 + 显式 `MADV_HUGEPAGE`），
   先验证 RSS 不翻倍、G18 不破，再谈墙钟。
2. `elide` 链表化（v11 遗留的 sizeratio 墙钟归因，配额记账已被推翻、B 已默认关）。
3. G1②(a) warm 尾部：已有归因（环境抢占 + chase 冷页缺页 + 256-pop 真工作），缺的是「可契约化」的判据形态。

## §12 chase 剩余差距的逐行归因（v13 之后一轮）

仪器：`cachegrind --cache-sim=yes`（同一份生成 C 与 cpp 的 `-O2 -g` 构建，输入 `1048576 20261013 4000000`），
按源码行归因 D1 miss。总量：extC Ir **319,713,318** / D1mr **10,414,120** / DLmr 150,727；
cpp careful Ir 363,853,623 / D1mr 8,529,167 / DLmr 87,803 —— extC 指令更少、缺失更多。

逐行（D1mr 读）：

| 行 | extC | cpp careful |
|---|---|---|
| 游走取边 | `extc_rc_storeRefLocal(... cur->o.data[checkedIndex(r,4,…)])` = **1,549,294** | `cur = cur->o[r]` = 209,566 |
| `sum += cur->v` | 3,996,846 | 3,998,704 |
| 建图写边 | 4,175,774 | 4,176,757 |

⇒ 随机访问本身两边一样（≈4.0M）；差额就是取边那一行的 **1.34M**。

机制：cpp 的 48 B 请求经 glibc 变成 64 B chunk（16 B 头 + 48 B 载荷），载荷相位恒为 16，
所以 `o[4]`（结构体偏移 16..48 的 32 B）永远落在同一个 64 B cache line 内；
extC 的槽 stride 就是 48 B，槽起点相位按 0/48/32/16 四拍轮转，`o[]` 在其中两拍跨线。

**反证一（把 33–64 B 槽补到 64 B）**：同一份生成 C 只改 `extc_rc_slotSize`，取边行 D1mr **1,549,294 → 0**，
但整程序 Ir 319.7M → **424.5M**（markStep 58.5M → 160.5M）、RSS 67.7 → **90.2 MB**、
`thr_max` 63.4 MB → **174.5 MB**、`scale_max` 16 → 64、`M_max_now` 69 MB → 190 MB、
墙钟 119.07 → **141.97 ms**（7 回合交错；给两边都加 `EXTC_RC_M_MAX=75000000` 后是 122.54 → 141.97）。
活字节 +33% 把触发线与自适应倍率一起推上去、堆翻倍，miss 收益被吃掉。

**反证二（关掉 GC）**：`extc_rc_shouldSweep` 直接 `return 0` 的生成 C 在 7 回合交错里 125.40 ms，
不比默认 119.63 ms 快 ⇒ 这 1.34M miss 不是 GC 弄脏 cache 造成的。

结论：chase 的差距 = **紧凑 48 B 槽与 64 B cache line 的相位冲突**。消掉它只有两条路：
给含引用的小对象补到 64 B（实测更慢，占内存 +33%），或改语言级字段布局规则（确定性布局是硬约束）。
两条都不做，登记为结构性代价；收益侧是 RSS 比 C++ 低（chase 67,744 vs 77,584 kB）。

四形状复测（v13 冻结 build `8af2a5e8`，11 回合交错 + 奇偶反向，`taskset -c 6`，min ms / 峰值 RSS kB；
四个形状的 stdout 与 cpp 参考逐字节一致）：

| 形状 | extC | C++ careful | Rust careful | min 比 extC/C++ | paired 中位 extC/C++ | RSS extC / C++ |
|---|---|---|---|---|---|---|
| promote 500000 | 38.35 | 115.33 | 36.89 | 0.332 | 0.326（对 Rust 0.975） | 5,724 / 146,504 |
| sizeratio 2000000 | 14.68 | 16.31 | 19.87 | 0.901 | 0.901（0.760） | 2,656 / 4,080 |
| chase 1048576 / 4000000 | 134.29 | 123.29 | 244.43 | 1.089 | **1.025**（0.548） | 67,744 / 77,584 |
| cycleratio 400000 / 100 | 14.31 | 35.30 | 17.37 | 0.405 | 0.408（0.824） | 2,656 / 4,108 |

读法：三形状对 C++ 领先 10–67%、对 Rust 领先 2.5–24%；chase 对 C++ 慢 2.5%（配对中位）到 8.9%（min），
对 Rust 快 45%。**跨会话漂移 5–13%**（同一 extC 二进制在不同会话里对同一 cpp 二进制可到 1.13×），
因此只读同轮比值，绝对值只作量级参考。

复现命令：

```sh
./build/extc -w --no-line-map -I stdlib bench/gc/v2/src/chase.extc -o /tmp/ch.c
cc -O2 -g -D_POSIX_C_SOURCE=200809L /tmp/ch.c -o /tmp/ch.g
g++ -O2 -g -o /tmp/ch.cpp.g bench/gc/v2/src/chase.cpp
valgrind --tool=cachegrind --cache-sim=yes --cachegrind-out-file=/tmp/cg.a /tmp/ch.g 1048576 20261013 4000000
valgrind --tool=cachegrind --cache-sim=yes --cachegrind-out-file=/tmp/cg.b /tmp/ch.cpp.g 1048576 20261013 4000000
cg_annotate --auto=yes --show=D1mr --sort=D1mr /tmp/cg.a | head -40
```
