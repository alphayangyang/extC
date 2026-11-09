# P1 轮（v11）：分配准入按请求字节 —— 显式墙为「一个最大请求」留出空间

> 状态：**已落地、已验证、待提交**。源码口径 `src/back/reclaim.c` = `ab21781d29c7ee213afbaafc85b0cc1c` / 4,169 行；
> driver 口径 `tests/reclaim/driver.c` = `6e1bcec5e3320feb780b9567ed9bca4f` / 1,620 行；`build/extc` = `6f1213ea100acac6c12b61cb262776e7`。
> 对照基线 v9 = commit `8d0086f0`（reclaim.c `3de5655435d4dfb2448b2168fee4ca19` / 4,156 行）。
> 本文件记录的是**最终采纳的口径**：v10 的「无条件预留」在 dense 形状上付了 +27.3% 峰值，已被收窄成「只绑显式墙」，
> 收窄过程与证据见 §3。

## 1. 问题：触发线只看已分配的堆

回收的起轮判据是「已占用 payload 是否越过触发线」：

    extc_rc_shouldSweep(r)  <=>  r->bytes > r->thr            (reclaim.c:3708)
    r->thr = min( max(2*live, floor) * scale , M_max - live/4 ) (reclaim.c:3638, :3665)

其中 `r->bytes` 含**本次分配之前**已经落账的字节。于是存在一个时序缝隙：`r->bytes` 刚好在线下，
而这一笔请求本身是 8 MiB —— 判据放它进来之后 `r->bytes` 才越过线，回收要到**下一次**分配点才起，
这一笔最大请求因此在墙外又叠了一个在飞 payload。墙帽 `M_max - live/4` 里的 `live/4` 只是「一次扫描步的批大小上界」，
**与请求大小无关**：大对象程序（`pd_bigobj` 形状，请求 8 MiB）里它等于 2 MiB，远小于一个请求。

## 2. 机制：把「最大单次请求」记进 region，让显式墙为它预留

三处改动，全部在 `src/back/reclaim.c`，**热路径 0 条指令**：

1. **记账字段**（`reclaim.c:282`）：`struct ExtcRcRegion` 在 `thr` 之后加 `int64_t maxReq;` —— 本 region 见过的最大的
   单次请求（即 slab 的槽 stride）。region 由 `calloc` 建（`reclaim.c:2670`）⇒ 自动初始化为 0。
2. **登记点**（`reclaim.c:867`，在 `extc_rc_slabNew` 的 `s->ss = slotSize;` 之后）：

       if ((int64_t)slotSize > r->maxReq) r->maxReq = (int64_t)slotSize;

   只为两条**慢路径**付费：big 路径（槽容量 < 2，`slotSize` 就是请求字节）与「新 class 首槽」路径。
   已存在的 class 走 freelist 快路径，不进 `slabNew` ⇒ 分配热路径的指令数不变。
3. **墙帽**（`reclaim.c:3660-3669`，在 `extc_rc_thrUpd` 内）—— 预留**只在显式墙下生效**：

       _res = r->live / 4;                                        /* b = 256 => L/4：一次扫描步留下的批上界 */
       if (extc_rc_mmax > 0 && r->maxReq > _res) {                /* 显式墙才为一个最大请求留位 */
           _res = r->maxReq;
           if (_res > r->live) _res = r->live;                    /* 不许把线推到活集以下 */
       }
       _cap = _wall - _res;                                       /* 墙帽 */
       if (_cap < r->floor) { _cap = r->floor; ... [mmax] ... }
       if (thr > _cap) thr = _cap;

   观测：`extc_rc_maxReqNow` / `extc_rc_resNow`（`reclaim.c:983`、写入 `:3689`），`[work]` 行新增 `maxreq=%lld res=%lld`
   （格式串 `reclaim.c:3825`，参数 `:3835`）。

## 3. 为什么只绑显式墙（v10 → v11 的收窄，附 dense 回归证据）

v10 的写法是**无条件**把 `maxReq` 塞进墙帽。它在默认形状上不改变峰值，但在 `pd_pointer_dense` 上触发了 +27.3% 的峰值回归：

| 口径 | v9（HEAD `8d0086f0`） | v10（无条件预留） | v11（只绑显式墙） |
|---|---|---|---|
| `pd_pointer_dense` 200 轮 maxRSS | 112,092 kB（两次相同） | **142,620 kB**（三次逐 kB 相同） | **112,092 kB**（两次相同） |
| dense `peak_bytes`（生成 C 里临时加打点） | 89,095,920 B（85.0 MiB） | **115,390,736 B（110.0 MiB）** | 回落同 v9 |
| `pd_bigobj` 60 = 200 轮 | 26,076 kB | 26,076 kB | 26,072–26,076 kB（噪声内） |

定位过程（全部在生成的 C 上做受控 A/B，同一台机器、同一组 `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`）：

1. 用 `git worktree add -f /tmp/extc-v9 HEAD` + `make BUILDDIR=build-v9` 造 v9 编译器，生成 `dense-v9.c`（4,358 行）与
   `dense-v10.c`（4,367 行）；逐行 diff 只有 9 处（struct 字段、登记行、两个全局、thrUpd 的 `_res/_cap` 块、`[work]` 格式串与参数、`#line`）。
2. 逐块二分：只加 struct 字段 ⇒ 112,092；只加 `[work]` 两个占位参数 ⇒ 112,092；只加全局 ⇒ 112,092；字段 + 登记行 ⇒ 112,092。
   ⇒ **唯一的因果块是 thrUpd 的墙帽**。把 v10 的帽块换回 `_cap = _wall - r->live / 4`（其余全保留）⇒ **112,092**（两次）。
3. 机制：dense 的隐式墙是 `3·L`，早期 `live/4 < R_max = 8 MiB`，预留把触发线压低 ⇒ 起轮时机提前 ⇒ 自学习倍数一路养到
   `scale_max = 64`（v9 是 16）⇒ 阈值被顶到墙帽、`live` 估计反而更小（`M_max_now` 从 133.3 MB 降到 104.3 MB）⇒ 峰值抬高。
   即：**承诺本身（线 + 最大请求 ≤ 墙）在隐式墙下兑现不了**（`3·L` 自己随 `L` 涨），却要默认路径为它付内存账。

v11 的收窄：`if (extc_rc_mmax > 0 && ...)` —— 只有用户**显式**设了 `EXTC_RC_M_MAX`（内存有界模式）时才预留。
实测：dense 回到 112,092 kB（与 v9 逐 kB 相同），`pd_bigobj` 26,072–26,076 kB（噪声内）；显式墙下的契约仍由 §4 的用例证明。

## 4. 契约（形式化）

记 `R_max = r->maxReq`、`L = r->live`、`M = EXTC_RC_M_MAX`（**显式**）、`P = slab 填充余量`。

    M > 0（显式墙）时：  thr  <=  M - res,      res = min( max(L/4, R_max), L )
    =>  thr + R_max  <=  M - res + R_max  <=  M          （因为 res >= R_max）
    =>  M_peak  <=  thr + R_max + P  是一条**由构造保证**的上界
    隐式墙（M 未设，M = 3·L）时：  res = L/4，与 v9 逐字节相同（不预留）

读法：**显式墙之下，触发线之上永远放得下「一笔最大请求 + 它造成的垃圾」**，所以任何单个请求都不会被墙拒之门外，
也不会因为「线算得太低」而在一笔请求之后才发现超墙。相对旧帽 `M - L/4`，新的 `res` 只增不减 ⇒ 新上界弱强于旧上界
（旧帽在 `R_max > L/4` 时上界其实是 `thr + R_max = M - L/4 + R_max > M`，即**旧帽漏掉大请求这一项**）。

`_res <= L` 的夹取也给出反向保证：线不会低于活集，否则「每次分配都起一轮」，退化成抖动。

## 5. 红前绿后（判据先红，两个方向都验）

新用例 `t_admissionReserve`（`tests/reclaim/driver.c`，紧跟 `t_thresholdCache`）：

1. 分配 `B = 256 KiB` 的 big 对象（big 路径 ⇒ `slotSize == B`），断言 `r->maxReq == B`；空扫一次让 `live` 落地，
   断言 `r->live == B`、`r->scale > 1`。
2. **默认路径不变**（③）：隐式墙下按旧公式复算 `want = min(max(2*live*scale, floor), M_max - live/4)`（再 floor 兜底），
   断言 `r->thr == want` —— 这条就是 dense 回归的守卫。
3. **显式墙契约**（②）：临时把 `extc_rc_mmax` 设成 `2·B`（使 `live/4 < R_max` 且帽确实咬住）并重算阈值，断言
   `r->thr + r->maxReq <= wall` 且 `r->thr <= wall - r->maxReq`；然后把 `extc_rc_mmax` 还原并重算一次（不污染后续用例）。
4. 对照小对象区（300 个 `Pair` 全挂根）：断言 `q->maxReq == 16`、`live/4 > maxReq`，隐式墙与显式墙两种口径下 `q->thr` 都与旧公式相同。

**红自测 A**（把显式墙帽改回 `int64_t _cap = _wall - r->live / 4;`）⇒ `通过 214，失败 2`，恰好红在 ② 那两条。
**红自测 B**（把预留绑回隐式墙，即 `if (r->maxReq > _res) ...`）⇒ `通过 215，失败 1`，恰好红在 ③。
**绿**：现行源码 ⇒ `通过 216，失败 0`（含 ASan/UBSan 216/0、`ALLOC_INLINE=0` 216/0、chunk=1 13/0）。

## 6. 实测一：契约生效（显式墙，同一形状，chunk = 8192）

| 量 | 旧帽 `M - L/4` | 新帽 `M - max(L/4, R_max)`（显式墙） |
|---|---|---|
| `thr_max`（`[work]` 全轮最大触发线） | 23,068,672（22 MiB） | **16,777,264（16 MiB）** |
| `M_max` | 25,165,872 | 25,165,872（不变） |
| `maxreq` / `res` | 无字段 | 8,388,608 / 8,388,608 |

读数：`pd_bigobj` 的 `thr = max(2*live, floor)*scale` 本来就被 `2*L`（16 MiB）主导，旧帽 22 MiB 根本夹不到它 ——
**新帽把「墙外那一笔大请求」正式排除在可分配预算之外**，代价是显式墙下的可用垃圾预算从 22 MiB 降到 16 MiB（触发更勤）。

G18（`tests/gc-p0/run.sh g18`，产物 `tests/gc-p0/out/v11-g18`）：pd_bigobj 60 轮 = 200 轮 = **26,076 kB**，上界 26,624 kB，
余量 548 kB（2.1%）⇒ **PASS**；`pd_pointer_dense` 200 轮 = **112,092 kB**（与 v9 逐 kB 相同，v10 的 142,620 已消失）。

Ir 预算（`tests/reclaim-perf/gate.sh`）：**87,254,363 ≤ 88,029,115** 通过，相对 v9 的 87,151,664 只 **+102,699（+0.12%）**；
slotindex 域内穷举 1,433,391 组不符 0。

> 诚实结论：**本轮的墙钟/RSS 数字没有变好**（`pd_bigobj` 26,076 kB 与 v9 逐 kB 相同，落在噪声带内）。收益是结构性的：
> ①显式墙下的上界从「靠形状碰巧成立」变成「由构造保证」；②`thr_max` 降 6 MiB（可分配预算的可证明上界收紧）；③默认路径零扰动、零 Ir。

## 7. 实测二：2048 档仍然 RED，根因不是准入

预期「准入把线压低后 chunk 2048 也能过 G18」，实测**不成立**：

| 档 | `[work]` 逻辑峰值 | RSS（200 轮） | `[quota] deadline_us=800 hits` | `collects` |
|---|---|---|---|---|
| chunk 8192 | 24.0 MiB（25,168,896） | 26,460 kB | 4 | 198 |
| chunk 2048 | 32.0 MiB（33,558,528） | 34,716 kB（60 轮更差 42,972） | **100** | 100 |
| chunk 2048 + `-DEXTC_RC_QUOTA_DEADLINE_US=100000` | 24.0 MiB | **26,460 kB** | **0** | 200 |

决定性对照说明：2048 档多出的 8 MiB **不是**触发线/准入问题，而是 `EXTC_RC_QUOTA_DEADLINE_US = 800` 把
「一次 8 MiB 大分配要付的 assist 配额」**截断**了：段数 ×4（`SCAN_CHUNK` 2048 vs 8192）而每段固定开销不变，
于是一轮标记要跨两次大分配才走完，峰值多一个 8 MiB 的活垃圾（32 MiB = 4 个 payload：1 活 + 3 垃圾）。

排除项：`-DEXTC_RC_SEG_DEADLINE_US=0` 无变化（仍 34,716 / 32 MiB）；`MALLOC_MMAP_THRESHOLD_=131072` 更差（42,588）、
`MALLOC_TRIM_THRESHOLD_=0` 无变化 ⇒ 不是 glibc 保留，逻辑峰值本身就是 32 MiB。

预留乘数不是旋钮：在生成 C 上把 `r->maxReq` 改 `r->maxReq * mul`（mul = 1/2/3）三档结果**逐字节相同** —— `res` 被
`if (res > live) res = live;` 夹住，放大后仍等于 `live`（8 MiB），帽仍是 16 MiB。
极端对照：把帽改成 `res = min(2*R_max, wall - live)`（线降到 8,388,656 = L）后，2048 档**仍是** 34,716 kB / 32 MiB ⇒
**限制在 assist 预算，不在触发线**。

## 8. 裁决

**chunk 默认保持 8192**。让 2048 过 G18 的唯一办法是放宽 800 µs 的 assist 预算（放宽后会破「大分配通道 `over1ms = 0`」
这条停顿判据）—— 按「判据只严不放宽」，不接受。2048 记为：「受 assist 预算约束，不是内存准入约束」（见 `P1-RQ3-CHUNK.md`）。

## 9. 诚实限制

1. 本轮的**观测收益为零**（RSS 峰值不变），可主张的只有契约与上界；不要把「thr_max 降 6 MiB」说成内存下降 ——
   `pd_bigobj` 的峰值由 `2*L` 与在飞请求决定，`thr_max` 不是它的瓶颈。
2. 预留**只在显式墙下生效**：默认（隐式 `3·L`）运行完全不预留。这是 v10 → v11 的收窄，代价是「默认档也能容一笔最大请求」
   这条更强的性质没有拿到；换来的是 dense 峰值不回归（+27.3% → 0）。
3. `maxReq` 是**单调**量（只增不减）：一个显式墙程序若早期分配过一次 8 MiB、之后全变小对象，`res` 会一直停在 8 MiB，
   触发线被永久压低（更勤的回收、更高 CPU）。这是有意的保守方向（内存优先），但**没有衰减机制**。
4. `maxReq` 记的是**槽 stride**，不是真实请求字节：class 复用下同一 class 的槽 stride 相同，因此它等于该 class 的
   最大单次占用；`copyInto`/`poolResize` 这类 bulk 路径走的是既有 class，故 `maxReq` 不会因一次批量复制而虚高。
5. 长跑 + 反复 `regionFree` + 继续分配的形状仍未构造（region 寿命 = 帧 arena 寿命，普通程序 n = 1）；
   与 `P1-CLOSING-QUEUE.md` 的限制是同一条。

## 10. 门禁复现（现行源码全绿）

    ./tests/reclaim/run.sh            # 216/0（含 ASan/UBSan、ALLOC_INLINE=0 各 216/0；chunk=1 13/0）
    ./check.sh docs                   # 13/0
    ./check.sh nobench                # 150 通过 / 2 跳过 / 0 失败（v10 时点；v11 复跑同口径）
    tests/reclaim-perf/gate.sh        # 87,254,363 <= 88,029,115
    tests/reclaim-gaps/gate-release.sh# 4/4
    python3 tools/check_tree.py --gate
    python3 tools/check_changelog.py --gate
    # G18：EXTC=./build/extc OUT=tests/gc-p0/out/v11-g18 tests/gc-p0/run.sh g18

## 11. 下一轮开头（按优先级）

1. `G1②(a)` 归因：warm 步 over-50 µs 占比（pb 4.25e-4–1.21e-3、cyc 1.39e-4–4.52e-4）仍是唯一未定性的通道；
   需要把「环境抢占」与「真工作」切开（cpu 口径已经计入被抢占时间，是本轮之前就有的量具偏差）。
2. `G3b` 已闭合（v8 closing queue）；`G20` 的 sizeratio 墙钟 +9.31% 未收回 —— 若再动，先做 `elide` 链表化而不是配额记账
   （配额记账已被证否，见 `P1-QUOTA-FASTPATH.md`）。
3. 若要做「停顿时长有界」的下一档：单步 p99.9 现为 64.50/84.69 µs（`bseg-cal.tsv`，v9 标定），
   与 50 µs 目标的差距来自 `SCAN_CHUNK` 的固定段成本，不是本轮的准入。
