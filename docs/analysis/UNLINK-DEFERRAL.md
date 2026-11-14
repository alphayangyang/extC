# 大对象的跨区引用撤销：为什么把它摊到多步不是答案

本文件记录 tests/gc-torture 归档的 t04 缺口（一步 1.25 ms）的一次原型实验与它的否证结论。
原型不在本树里（tree 停在 86a9cb41 的运行期，`src/back/reclaim.c` md5 7a157eb5d30bb3b71b67c41eea1c6f07），
本文只记录机制、实测与判断，供下一轮选路线用。

## 1 缺口是什么

t04 的负载是「每轮分配一个 8 MiB 的结构体，丢掉上一轮那个」，该结构体内联 1,048,576 个可空引用域
（`ss = 8,388,616`）。字段数超过 `rcEmitTypeDesc` 的 32 项上限，描述符退回 `EXTC_RC_CONSERVATIVE`，
于是撤销它指向别的 region 的边时，`extc_rc_unlinkChildren` 的保守分支要按 `ss/8 = 1,048,576` 个词走一圈：

    p = EXTC_RC_SLAB_DATA(s) + i*s->ss;                  /* reclaim.c 发射文本：extc_rc_unlinkChildren */
    if (t == EXTC_RC_CONSERVATIVE) { size_t words = s->ss/sizeof(void*); for (w < words) ... }

实测（`EXTC_RC_STATS=1`、`EXTC_RC_PAUSE=1`、taskset -c 6）：t04 单次 `[segsteps]` `max_us` 1,248.8，
其中 `unlink` 通道 1,248.5 µs —— 这一步的时间几乎全部是这一个 walk，占 99.9%。
`tests/gc-torture/run.sh` 因此把它记为 xfail（已知开放缺陷，只许变严）。

要点：这不是「清扫扫多了」，而是**一个对象的撤销成本是 Θ(它自己的大小)**。任何按步切分的方案都必须回答
「切开之后，这块内存归谁持有、持有多久」。

## 2 原型：把 walk 做成可续（resumable）

做法（树外原型，基于 v28 的发射文本改）：给 `ExtcRcRegion` 加四个字段

    ExtcRcSlab *ulSlab;   uint64_t ulMask;   uint32_t ulSlot;   uint32_t ulCur;

`extc_rc_unlinkSome(s, i, &cur, limit)` 走至多 `limit` 个引用域并报告是否走完；`extc_rc_ulStep` 在 sweepStep
的预算里一次吃若干个 chunk（`EXTC_RC_UNLINK_CHUNK = 1024`，配 `_left` 预算，一步 7–9 个 chunk ≈ 8,192 个域）；
走完一个槽才调用 `extc_rc_freeSlotChain` + `extc_rc_freeSlotBook` 把它真正释放。

停顿确实降下来了。t11 探针（256 KiB 载荷、`a: [32768]?mut ref leaf`、2000 轮，跨区边由 callee 里的 `new leaf` 形成）：

| 臂 | `max_us` | `gt50` | RSS |
|---|---|---|---|
| base（86a9cb41） | 230.1 | 113 | 3,424 kB |
| 原型（`UNLINK_RETAIN_MAX = 4 MiB`，256 KiB 对象分块走） | 102.5 | 2 | 269,156 kB |
| 原型（`UNLINK_RETAIN_MAX = 0`，强制一步走完） | — | — | 3,044 kB |

第三行是关键对照：同一套代码，只要**不切开**，内存就回到 base 的水平。

## 3 否证：摊开等于把停顿换成滞留

t11 的 2000 轮里原型只完成了 990 次 walk（`ul_big=991`、`ul_done=990`、`ul_chunks=32,679 ≈ 33/对象`），
而登记出去的边是 16,000 条（8 条/轮）；`rs_live` 从 base 的 32 涨到 8,072，RSS 210 kB/轮线性上涨到 269 MB。
分配器跑在撤销前面，未走完的巨块一直被 `occ` 位钉住 —— 这就是 `EXTC_RC_UNLINK_RETAIN_MAX` 那个「保留界」
想兜住的东西，但它兜不住：t04 每轮分配 8 MiB，任何「每步 ≤ 8,192 域」的切分都需要 128 步才能走完一个对象，
按每轮一次收集算，滞留量就是 128 × 8 MiB。

一般的说法（这一节就是要写死的结论）：

    滞留量 ≈ (walk 域数 / 每步域数) × 分配速率 × 步间隔

停顿上界与滞留上界是同一个旋钮的两端，二者不能同时按「固定常数」压住：把一个 Θ(大小) 的工作摊到 k 步，
就必须持有那段内存到第 k 步，k 由停顿预算决定，滞留由 k × 分配速率决定。t04 的形状让这个乘积远大于任何合理预算。

顺带一个量具教训：原型第一版把 `#if` 写成了 `reclaim.c` 里的裸预处理行（没有引号），于是它作用于 **extc 编译器自身**
的编译（宏未定义 ⇒ 视为 0），新条件整行被排除出发射文本，三臂实测完全相同。发射文本里所有预处理指令必须是
字符串数组元素，这一点与 §19.12 记录的 v28 事故是同一个坑。

## 4 结论与两条候选路线

1. **接受 Θ(对象大小) 的撤销成本，并在契约里把它显式划出去**：单个对象的入边撤销是一步的工作单元，
   一步的上界写作 `b·τ_t + c + (该对象引用域数)·τ_word`，并在 `[chan]` 里用独立通道（`unlink`）报出来，
   而不是混进 sweep 通道。t04 的 1.25 ms 就变成「这个对象的引用域数量级」的直接结果，可预测、可声明。
2. **改注册粒度**：`[N]?mut ref T` 这类数组目前按元素逐个建边/撤边；若把「一段引用数组」注册成一个边区间
   （数组头存长度，撤销时只减区间里各目标一次或干脆只登记区间），撤销成本就与数组长度无关。
   这需要描述符/布局层面的改动，是唯一能让 t01/t04 这类形状同时满足停顿与内存两条界的路线。

在路线 2 落地前，t04 保持 xfail；不要用「摊到多步」的方式绕过它——上面三行实测就是反例。

## 5 复现

    # 归档缺口（期望：xfail，over1ms=3）
    EXTC=./build/extc bash tests/gc-torture/run.sh one t04_bigchurn

    # t11 探针：树外原型才有分块臂；base 臂用当前树的编译器
    ./build/extc /tmp/pp/t11.extc -o /tmp/u29/t11.c && cc -O2 -std=c11 -fwrapv \
        -D_POSIX_C_SOURCE=200809L -DEXTC_RC_PAUSE=1 -o /tmp/u29/t11 /tmp/u29/t11.c
    taskset -c 6 env EXTC_RC_STATS=1 /tmp/u29/t11 2000
