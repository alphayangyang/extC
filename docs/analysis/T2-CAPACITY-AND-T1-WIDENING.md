# T2 清扫的容量成本（第 4 项）与 T1 拓宽的收益（第 2 项）

> 探针跑在当前树：src/back/reclaim.c md5 `61c9fad7f67b6b73937030f32736baef`（4,305 行）、src/back/codegen.c md5 `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行）。数字是单机单次（taskset -c 6），用来判量级与形状，不用于跨轮对比。

## §1 第 4 项：T2 清扫按容量计费，稀疏活集下开销仍高 —— 属实

### 1.1 机制（源码位置）

1. 清扫以**位图词**为单位，不以对象为单位。`extc_rc_sweepWord`（src/back/reclaim.c:3637）对一个 64 槽词做四件事：读 occ/mark、对 dead 位批量调 `extc_rc_freeSlotWord`、清 mark、把 `nMark` 减掉本词 popcount（src/back/reclaim.c:3644）。每词的固定工作与该词里有几个死槽无关。
2. 逐词推进、按预算切步：`extc_rc_sweepStep`（src/back/reclaim.c:3652）在一个 slab 的首词先试整 slab 判定 `extc_rc_sweepElide`（src/back/reclaim.c:3662），否则 `extc_rc_sweepWord`（src/back/reclaim.c:3664）。
3. 整 slab 跳过的条件是 `s->nMark != 0` 就返回 0（src/back/reclaim.c:3616 与 3570），即「本 slab 本轮没有任何可达槽，且没有跨区登记位 rsbit」。这不是存活率阈值：**只要一个 slab 里有 ≥1 个长命对象，该 slab 就得逐词扫完**，代价与该 slab 的容量成正比（nwords × 每词工作），不随存活比例下降。

所以成本函数是 Λ_sweep = c_s · (被扫字节)，而「被扫字节」在稀疏活集下的取值≈全区容量；整死 slab 跳过是**常量因子**优化，不是渐进优化。

### 1.2 探针与实测

装置：`tests/reclaim/run.sh` 的发射文本（`emit_main.c` + src/back/reclaim.c 导出 `extc_reclaim_runtime.inc`），探针自己 include 该运行期并调 `extc_rc_regionNew` / `extc_rc_alloc` / `extc_rc_collect`；`CLOCK_MONOTONIC` 包住一次 collect，`-DEXTC_RC_NOCOUNT=1` 进 T2，`-D_POSIX_C_SOURCE=200809L` 才看得到 CLOCK。48 B 槽、6 个指针描述符；16 B 槽、1 个指针。

48 B 槽，64 MB 区，单次 collect（ns/KB 按 payload 容量折）：

| 活集形状 | 时间 | ns/KB | 读法 |
|---|---|---|---|
| 全死（129/129 slab 走 elide） | 56.7 µs | 0.86 | 地板 = 每 slab memset 两张位图 |
| 全区 1 个存活对象 | 65.6 µs | 1.00 | 单个长命对象几乎免费 |
| 每 MB 1 个存活（约一半 slab 不能跳过） | 531.6 µs | 8.11 | 64 个不可跳过 slab × 7.4 µs |
| 50% 存活 | 12,475.9 µs | 190 | 含标记，不与上面直接比 |

- 16 MB 区的同序数字：全死 14.3 µs / 0.87 ns/KB、1 个存活 16.0 µs / 0.98、每 MB 1 个存活 123.1 µs / 7.51。
- 单个**不能跳过**的 512 KiB slab ≈ 7.4 µs（由 64 个不可跳过 slab 的 475 µs 增量算出），即 10,922 槽 × ≈0.68 ns/死槽。
- 外推最坏（每个 slab 恰好 1 个存活对象，全部容量都不能跳）：≈16 ns/KB = 0.016 ns/B；64 MB 区单次 collect ≈ 1.0 ms。
- 16 B 槽同一规律：全死 4.1–7.3 ns/KB、每 MB 1 个存活 26 ns/KB（0.026 ns/B、死槽 0.21 ns/槽）、半活 447–729 ns/KB。

### 1.3 结论与判据

- 用户的描述成立：清扫成本按**容量**走；有分散长命对象时，全死 slab 跳过不再触发，成本落在 0.0075–0.016 ns/B 带内，是全死地板的 9–19 倍。
- 这一带正覆盖 roofline 标定的 `c_s = 0.0108 ns/B`，说明该常数量的就是「稀疏活集下的容量清扫」，不是乐观值。
- 因此「把清扫从按容量改成按存活」需要换机制（卡片/记忆集式写屏障，或分区化让长命对象集中到可整块跳过的 slab），而不是调参；在现行设计里，elide 已经把可跳过的情形压到地板。
- 可写进契约的判据：单次 collect 的清扫成本落在 `0.85 ns/KB（elide 地板）..16 ns/KB（每 slab 1 个存活）`，即每 MB 容量 0.85–16 µs；任何声称「稀疏活集下清扫仍贵」的说法必须落在这条带里，否则是别的阶段（标记/释放）在付费。

## §2 第 2 项：T1 拓宽的必要性 —— 实测不支持

### 2.1 语料事实

`paper/data/census.tsv`（1,396 行，列 path/status/tier/bytes/lines/rc_marks）分层：fail/-1 602（不编译，多为负例目录）、ok/-1 544（68.5%，编译但不发射回收代码）、ok/2 248（清扫档）、ok/1 **2**。即在 794 个进入回收体系的程序里，事件档只占 0.25%。

### 2.2 强制事件档的代价（`EXTC_RC_NOCOUNT=0` + `EXTC_RC_SWEEPFREE=1`，codegen 发射 `#define EXTC_RC_TIER 1`，判定与打印在 src/back/codegen.c:12659–12663）

bench/gc/v2 四形状（默认臂，各 1 次，taskset -c 6）：

| 形状 | 默认档 时间 / RSS | 强制 T1 时间 / RSS | 读法 |
|---|---|---|---|
| promote | 0.04 s / 5.7 MB | 0.13 s / 130.0 MB | RSS 涨 23×：回收依赖清扫档 |
| sizeratio | 0.01 s / 2.7 MB | 0.07 s / 169.1 MB | 同上 |
| cycleratio | 0.01 s / 2.7 MB | 0.04 s / 52.8 MB | 同上 |
| chase | 0.12 s / 67.7 MB | 0.27 s / 67.6 MB | 无环、内存持平，**慢 2.25×**（每引用写屏障） |

专为事件档构造的两个无环探针（`/tmp/pp/t1list.extc`：循环建 400 万节点全活链表；`/tmp/pp/churn.extc`：20 轮 × 50 万节点建/拆）：

| 探针 | 默认档（T2） | 强制 T1 |
|---|---|---|
| 全活链表 4M 节点 | 0.08 s / 74.6 MB | 0.08 s / 82.3 MB（计数存储 +7.7 MB） |
| churn 20×500k | 0.10 s / 10.9 MB | 0.19 s / 12.0 MB（1.9× 慢，内存不降） |

### 2.3 结论

- 事件档买到的是「停顿归零 + 内存只绑活集」，不是吞吐。它每个引用写要付一次计数屏障、每槽要留计数存储；在 T2 已把峰值压在 ~3L 且整死 slab 能 elide 到地板的工作负载上，这些付出没有对应收益：churn 类慢 1.9×、chase 慢 2.25×、全活链内存反升。
- 所以「拓宽 acyclicity 证明」不是可换吞吐的杠杆：把 248 个清扫档程序判成事件档，换来的是更强的停顿承诺，代价是 1.9–2.25× 的墙钟（以及计数存储）。当前四形状的吞吐目标反而会退。
- 若仍要拓宽，唯一有意义的形态是「建造序（年龄单调）证明」：只允许新对象指向更老对象 ⇒ 不可能成环（覆盖 prepend、树、构建器返回的链）。它服务的是需要零停顿的用户，不是当前基准；并且它不能省掉构建时的计数屏障，所以上面 1.9× 的账仍然要付。
- 建议：不动 acyclicity 证明；把力气放到（第 3 项）测试规模、（第 5 项）随机估计器、（第 1 项）形式化，以及把 T2 的容量成本固定在 §1.3 的判据上。

## §3 复现

    cc -std=c11 -O1 -I src/base -I src/back tests/reclaim/emit_main.c src/back/reclaim.c src/base/base.c -o /tmp/emit && /tmp/emit > /tmp/extc_reclaim_runtime.inc
    cc -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -DEXTC_RC_NOCOUNT=1 -I /tmp -o /tmp/probe48 /tmp/pp/sparse48.c
    taskset -c 6 /tmp/probe48

    export EXTC=build/extc
    $EXTC -w -I stdlib /tmp/pp/t1list.extc -o /tmp/list.t2.c
    EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=1 $EXTC -w -I stdlib /tmp/pp/t1list.extc -o /tmp/list.t1.c
    cc -O2 -D_POSIX_C_SOURCE=200809L -o /tmp/list.t2 /tmp/list.t2.c; cc -O2 -D_POSIX_C_SOURCE=200809L -o /tmp/list.t1 /tmp/list.t1.c
    /usr/bin/time -f "%e s %M kB" taskset -c 6 /tmp/list.t2; /usr/bin/time -f "%e s %M kB" taskset -c 6 /tmp/list.t1
