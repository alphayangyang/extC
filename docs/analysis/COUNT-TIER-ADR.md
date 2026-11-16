# ADR：计数路线（event tier）的去留

状态：**接受**（2026-11-16）。本条 ADR 关闭 goal-986ac231 的「计数路线 ADR」一项。
范围：分档决策（idle / 事件档 / 清扫档）与「计数」作为回收手段的去留，不含 slab 布局本身。
口径：源码为 split 树（src/back/rt/*.inc、src/back/cg/*.inc），行号是**渲染文本**行号；渲染文本 = 编译器真正读到的 4,780 行 / 16,368 行。

## 1. 背景：三档从哪里来

每个 translation unit 在代码生成前被定一档，产物头部的 
`#define EXTC_RC_TIER n` 就是这一档的 grep 得到的一句话（发射点 src/back/cg/180-unreferenced-defs.inc:1532，判据四行紧随其后）：

| 档 | 宏组合 | 谁来推进回收 | 语料分布（794 个能独立编译的程序） |
|---|---|---|---|
| 0 无运行期 | 不发射 TIER 行 | 不回收：分配落在帧 arena / 池等由作用域或容器管理的地方，没有采集器 | 544（68.5%） |
| 1 事件档 | `EXTC_RC_NOCOUNT 0` + `EXTC_RC_SWEEP_FREE 1` | 计数归零即释放（无轮次、无清扫） | 2 |
| 2 清扫档 | `EXTC_RC_NOCOUNT 1` + `EXTC_RC_SWEEP_FREE 0`（默认） | 标记-清扫轮，按压力触发 | 248 |
| 3 计数+清扫 | 两者都开（`EXTC_RC_NOCOUNT=0` 强制） | 计数抢跑 + 清扫兜底 | 0（无程序落到这里） |

档 0 是静态证明的结果（idle 判据，含 §19.14 的第四判据 (d)），不是开关；档 1 由无环证明自动升级：
src/back/cg/210-criterion-d.inc:1951–1952

```c
if (g.rc.sweepFree != -1 && g.rc.reclaim && !g.rc.memGrowth && unitAllAcyclic(&g, m)) {
    g.rc.sweepFree = 1;
    g.rc.noCount  = 0;
}
```

两个开关只做诊断与 A/B：`EXTC_RC_NOCOUNT=0` 强制开计数（src/back/cg/190-lambda-envs.inc:634），
`EXTC_RC_SWEEPFREE` 三态 unset/1/0，置 1 时顺带把 noCount 拉成 0（同文件 647–653）。

## 2. 计数在两个地方存在，且都不是「对象头」

1. **持有者计数 hold**（档 1/3 的主路径）：`uint16_t *hold` 是每槽一张旁表，饱和值 `EXTC_RC_HOLD_SAT 65535u`
   （src/back/rt/00-head.inc:67–68），归零直接 `extc_rc_freeSlotCascade`。
   `EXTC_RC_NOCOUNT=1`（默认）时四个入口 `holdUpdSlab/holdUpd/inc/dec` 全部是空函数（src/back/rt/55-nocount.inc），
   连 `hold` 指针都不分配 —— 分配点 src/back/rt/33-hugefree.inc:147–153 用 `#if !EXTC_RC_NOCOUNT` 包住，注释写明了理由
   （关掉时「在一张 16 B 槽的 slab 上省下约 29k 条指令」）。
2. **跨区入边计数 rscnt**（与档无关，按需）：`uint16_t *rscnt` 也是旁表，**第一次出现跨区边时才 calloc**
   （src/back/rt/50-xregion.inc:42–44）；饱和值同 `EXTC_RC_HOLD_SAT`，置 0/置 1 时同步 `rsbit` 位。

两者的内存都记在 slab 账上（src/back/rt/41-budget.inc:66–67）：`hold` 与 `rscnt` 各 2 B/槽。对 48 B 槽是 +4.2% 的元数据，
40 B 槽是 +5%，16 B 槽是 +12.5%。

**没有一处把计数放进对象**。这是 2026-11-14 的硬约束（用户：普通对象布局要确定性布局，先不做计数优化）的直接后果：
对象仍是一个纯值块，`new T` 的地址、字段偏移、@sharesStorage 的共享语义都不随档位变化。

## 3. 决策

D1. **默认清扫档**（`EXTC_RC_NOCOUNT=1`、`EXTC_RC_SWEEP_FREE=0`）。计数不是默认，事件档只在无环证明成立时自动开启。
D2. **计数永不进对象头**。`hold` 保持 per-slot 旁表，只在该单元真的开计数时分配；对象布局与档位解耦。
D3. **事件档只能由证明给，不能由默认/开关给**。`unitAllAcyclic` 证明失败（含 criterion (d) 的 `g.rc.memGrowth` 单元）一律留在清扫档；
    强制开关（`EXTC_RC_SWEEPFREE=1`）只用于红证据，不进默认路径。
D4. **不把「计数+清扫」做成产品档**（档 3）。语义上它是两个机制的叠加：计数负责抢跑、清扫负责兜底，
    代价是两条路径同时存在（表更大、常量更多），而 248 个清扫档程序里没有一个因为「清扫太晚」而需要抢跑 —— 语料没有需求。
D5. **跨区入边计数保留**，因为它是跨区引用撤销的唯一账本（wmark 的登记位与 `rsbit` 都在它上面），与档位无关。

## 4. 理由（每条都有测量或语料支撑）

R1. **吞吐**：强制事件档（`EXTC_RC_NOCOUNT=0` + `EXTC_RC_SWEEPFREE=1`）在四个 gcbench 形状上是 1.9×–3.3× 慢（churn 1.9×、chase 2.25×、promote 3.25×）；
    最弱的指针追逐形状 chase 也有 2.25×。原因写在机制里：每一条引用写都要 `holdUpd`（找 slab → 查 occ → 饱和加减），
    而清扫档的热路径上一次都不做。证据：docs/analysis/T2-CAPACITY-AND-T1-WIDENING.md。
R2. **内存**：强制事件档在**有环**形状上不是省内存而是涨内存 —— 环的计数永远不归零，只能靠清扫兜底，而清扫已被关掉，
    于是 promote 的峰值从 5.7 MB 涨到 130.0 MB。这正是 D3 的理由：事件档的语义前提是「无环」，证明不成立时它不安全。
R3. **语料**：1,396 个 .extc 里能独立编译的 794 个，68.5% 连回收运行期都不发射（idle 证明），248 个清扫档，2 个计数档。
    也就是说「需要计数才能及时回收」的程序在语料里是 0.25%，而「被证明无环所以不需要清扫」的只有 0.25%（同一批 2 个）。
R4. **内存代价可量化且不便宜**：`hold` 是 2 B/槽；churn 形状（大量小对象、快速死）实测 T2 10.9 MB vs T1 12.0 MB（+10%），
    因为对象越小，2 B 的相对开销越大。
R5. **布局确定性**：把计数挪进对象头会让每个对象多 2–4 B、改变地址与字段偏移、破坏「同一份 .extc ⇒ 逐字节相同的 C」这一诊断性质
    （`tools/emitted_identity.py` 的 430 行基线就是靠它），也会让 @sharesStorage 的共享值语义变复杂。用户已明确否决这条路。
R6. **清扫侧已经不是瓶颈**：T2 的容量成本已由 slab 级归还（1420d5a3）、页级 madvise（8f42f61d）、自动路径（ef712794）、
    巨块丢弃（5fd00730）压到「每 MB 容量 0.85–16 µs」并随轮数稳定；清扫不再是「必须靠计数绕开」的东西。

## 5. 后果

正面：
- 热路径上没有任何计数写入，分配路径的 Ir 账单（docs/analysis/ALLOC-COST.md）里「内联 rc」那 67.6 Ir/分配 不含计数；
- 对象布局与档位无关，`emitted_identity` 可用作逐字节回归基线；
- 事件档仍然存在且被真正用上（语料里 2 个程序、以及任何被证明无环的单元），没有为了简化把它删掉。

负面 / 已知边界：
- 无环但**没被证明**的程序留在清扫档，多付清扫带宽（0.50–0.61 ns/KB）与「垃圾信用」内存（`thr` 到 3×L 之前不回收）；
- 语料对「计数能不能赢」几乎没有分辨力（只有 2 个计数档程序），R1/R2 的结论来自**强制开关**的实验，不是自然分布；
- 若未来出现实时性用户，事件档是唯一「没有回收轮次」的档（工作落在最后一次释放的那条语句上），但它要求证明 —— 证明的召回率（2/794）就是该路线的天花板。

## 6. 什么时候重开这条 ADR

满足任一条即重开（否则不再讨论计数）：
1. 出现有硬实时预算、且**能被证明无环**的用户程序（事件档的真实需求）；
2. 无环证明召回率从 2/794 显著提高（例如建造序/年龄单调证明落地），使事件档覆盖真实语料；
3. 计数的旁表成本能降到 1 B/槽以下（例如 255 饱和 + 分档存储），使 R4 的 +10% 变成噪声；
4. 布局约束被放宽（允许对象头，或允许非确定性布局）——此时 R5 失效，方案空间重新打开；
5. 出现「清扫档的下界也满足不了」的场景（例如 pause 预算 < 1 µs），只有计数能在最后一次释放的那条语句上完成回收。

## 7. 证据索引

| 事实 | 位置 |
|---|---|
| 档位标记与四档语义 | src/back/cg/180-unreferenced-defs.inc:1510–1536 |
| 无环自动升级（含 memGrowth 排除） | src/back/cg/210-criterion-d.inc:1944–1953 |
| 两个开关的解析 | src/back/cg/190-lambda-envs.inc:626–653 |
| 计数入口（关=空函数 / 开=hold 表） | src/back/rt/55-nocount.inc |
| hold 只在计数档分配 | src/back/rt/33-hugefree.inc:145–162 |
| rscnt 惰性分配 | src/back/rt/50-xregion.inc:40–45 |
| 2 B/槽记账 | src/back/rt/41-budget.inc:61–71 |
| 饱和值 | src/back/rt/00-head.inc:67–68 |
| 强制事件档的吞吐/内存 | docs/analysis/T2-CAPACITY-AND-T1-WIDENING.md |
| 语料分档 | paper/data/census.tsv（794 ok / 544 idle / 248 sweep / 2 count） |
| 清扫的容量成本 | docs/analysis/GC-ROOFLINE.md §6.10–§6.12、docs/analysis/UNLINK-DEFERRAL.md |
| 热路径 Ir 账单 | docs/analysis/ALLOC-COST.md |

## 8. 复现

```sh
# 分档（一句话，grep 生成物头部）
./build/extc P.extc -o P.c && grep -m1 'EXTC_RC_TIER' P.c
# 强制事件档（红证据；含环形状会涨内存）
./build/extc P.extc -o P.c && EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=1 ./build/extc P.extc -o P.c
# 关掉清扫但不开计数 ⇒ 不成立（sweepFree=1 会把 noCount 拉成 0）
```