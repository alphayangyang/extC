# P0 落地报告：字节配额（P）+ 懒复用链重建（B）

> 证据约定：本文所有「源码行号」均指冻结口径下的 `src/back/reclaim.c`（运行期文本以逐行 C 字符串存放在 `RECLAIM_RT[]`，行号即物理行号）。凡标「未测/存疑」者为未验证推断，不作结论。
>
> - 源码：`src/back/reclaim.c` md5 `7c2a9cedd7edd54de0a378d8a507dcbe`（3801 行）
> - 构建：`make BUILDDIR=build-rf -j24` → `build-rf/extc` md5 `7019342b3e5ccddbe67cbdc96eae2395`
> - 测试驱动：`tests/reclaim/driver.c` md5 `83e167d761705268dbda3f89895ea574`
> - 不 git commit；未改动 `src/back/codegen.c`。

## 0 结论摘要

1. **P（字节配额）**：一次分配触发的回收推进量按**请求字节**计价，而不再只按「分配事件」推进。sweep 侧 `W_s = 8*pm + max(1, min(ceil(cursor_slab->nslots/64), QUOTA_MAX_WORDS))` 个 word；mark 侧 `W_m = min(n >> QUOTA_SHIFT_MARK, QUOTA_MAX_MARK)` 个 work unit。这是「内存上界 `M <= M_max + 缓冲`」的结构性前提：大分配无法用「很少的事件」跳过回收。
2. **物理上限**：`EXTC_RC_QUOTA_DEADLINE_US`（默认 800 µs，0=off）在 mark 配额循环里按每次 advance 读墙钟，单步上界 `D + 2 × segment`（一个 segment ≤ `SCAN_CHUNK` 个 word）⇒ 约 0.96–1.0 ms，落在 1 ms 直方图边界内。配额是「按字节挣得的信用」，deadline 是最后兜底。
3. **B（懒复用链重建）**：sweep 产生的空洞不再逐槽挂链，只记 `nfree`；块「满」的判据改为 `bump >= nslots && nfree == 0`；一旦需要复用而 `freelist == -1`，就从 `occ` 位图整链重建（一次 O(nwords)），并用实际链长覆写 `nfree`。收益是 sweep 不再为每个死槽写 `tid[]` 做链头插（每个死槽省若干 Ir），代价是复用前多一次位图扫描。
4. **验收（默认档）**：`tests/reclaim/run.sh` 181 通过 / 0 失败（ASan/UBSan 181/0、ALLOC_INLINE=0 181/0、chunk=1 10/0、chunk=1e9 红自测成立）；计数档 `EXTC_RC_NOCOUNT=0` 237/0（ASan 237/0）；`gate.sh` 86,618,661 Ir ≤ 88,029,115；三档生成物（默认 / NOCOUNT=0 / SWEEPFREE=1）经 `cc -Wall -Wextra -Werror=format` 零警告。
5. **G18**：`tests/gc-p0/src/pd_bigobj.extc` 200 轮峰值 26,636–26,648 kB（3 reps），deadline hits 0–1，单步 max 98.9–190.3 µs、over256=0；60 轮峰值 26,644–26,648 kB。指针稠密形状 `pd_pointer_dense` 200 轮峰值 112,280–112,284 kB，hits=0（deadline 未参与），单步 max 199.6–369.3 µs、over1ms=0。
6. **已知负面结果**：四 v2 形状里 `sizeratio` 的墙钟中位 +10.34%（callgrind Ir 229,272,719→243,292,809，+6.1%），其余三形状 ±1%；stdout 四形状全部逐字节相同，RSS 三形状中性、promote 降 8.8%。sizeratio 的 Ir 归因（markStep/shouldSweep/sweepStep 内联归属）不可靠，**未解释，计入遗留**。

## 1 机制 P：按字节计价的回收推进

### 1.1 sweep 侧

代码（`src/back/reclaim.c:1353-1359`，`extc_rc_alloc` 的 `if (r->sweeping)` 分支）：

```c
uint32_t _b = EXTC_RC_SWEEP_BUDGET * (uint32_t)(extc_rc_pressure(r) ? EXTC_RC_SWEEP_PRESSURE_MUL : 1);
ExtcRcSlab *_cs = r->swSlab;
uint32_t _q = _cs ? (uint32_t)(((uint32_t)_cs->nslots + 63u) >> 6) : 1u;   /* words to finish the slab at the cursor */
if (_q < 1u) _q = 1u;
if (_q > EXTC_RC_QUOTA_MAX_WORDS) _q = EXTC_RC_QUOTA_MAX_WORDS;
_b += _q;   /* P0-P: one allocation funds finishing the slab under the cursor; the cap bounds this allocation step */
extc_rc_sweepStep(r, _b);
```

- 固定预算 `8 * pm`（pm 为压力乘子，默认 1；接近内存墙时 `EXTC_RC_SWEEP_PRESSURE_MUL` 默认 4），**加上**「扫完游标所在 slab 所需的 word 数」`ceil(nslots/64)`，被 `QUOTA_MAX_WORDS` 截断。
- 语义：一次分配至少资助「把当前 slab 扫完」，因此大分配不会让清扫停在一个 slab 的中途；huge block（`nslots==1`）得到 `ceil(1/64)=1`。
- 这是**加法**而非乘法：固定预算仍保留；压力乘子只放大固定部分，不放大字节配额（字节配额本身已有上界）。

### 1.2 mark 侧

代码（`src/back/reclaim.c:1363-1389`，`if (r->marking) { if (r->msp != 0 || !r->seedsDone || r->scanObj) { ... } }`）：

```c
size_t _quota = (size_t)(n >> EXTC_RC_QUOTA_SHIFT_MARK);
if (_quota > (size_t)EXTC_RC_QUOTA_MAX_MARK) { _quota = (size_t)EXTC_RC_QUOTA_MAX_MARK; }
size_t _used = 0;
#if EXTC_RC_QUOTA_DEADLINE_US > 0
long long _dl0 = _quota >= 4096 ? extc_rc_pauseNow() : -1;
#endif
for (;;) {
    _used += extc_rc_markAdvance(r);
    ... /* deadline 检查：每次 advance 后读钟，超时 hits++ 并 break */
    if (_used >= _quota) break;
    if (!(r->msp != 0 || !r->seedsDone || r->scanObj)) break;
}
```

- work unit 的定义（`extc_rc_markAdvance` 尾，`:1325` 返回 `r->markWork - _w0`；`markStep` 在 `:2979` 累加 `pops + words + seg`）：**1 pop、1 个扫描位图 word、1 个分段续扫 word 各记 1 unit**。unit 只用于配速，不代表时间。
- `QUOTA_SHIFT_MARK=3` ⇒ 每 8 请求字节换 1 个 unit（约等于每 8 字节 payload 的 1 个指针字）。
- `QUOTA_MAX_MARK=(1<<20)`：一个 8 MiB 的指针稠密 big 能在一次分配内扫完（G18 需要），是 memory-first 默认；`-DEXTC_RC_QUOTA_MAX_MARK=8192` 是 pause-first 变体（见 §5 的三列表）。
- 进入条件：只有 mark 尚未完成（有标记栈、种子未扫完、或在大对象分段扫描）才走配额循环；已完成时这条路径不花时间。

### 1.3 deadline（物理单步上限）

`:1368-1390`。要点：

- `EXTC_RC_QUOTA_DEADLINE_US`（默认 800，0=off）。`_dl0` 只在 `_quota >= 4096` 时读取（小分配路径不读钟，避免每次分配的时钟开销）。
- **每次 advance 之后**都读钟，而不是按 unit 数打点：一次 pop 可能触发描述符字段扫描（`nptr` 个 offset），这部分不体现在 unit 里，按 unit tick 会漏掉。
- 上界：`D + 2 × segment`（首 advance 与触发 deadline 的在飞 advance；一个 segment ≤ `SCAN_CHUNK=8192` 个 word）。注释已与实现统一为 2 segments（`:1019`、`:1370` 附近）。
- 超时语义：累计 `extc_rc_quotaDeadlineHits`，并放弃本次分配剩余的配额（break），不结转。**这是权衡**：放弃配额会让本轮 drain 变慢，若频繁发生会推迟 sweep、抬高内存；所以默认取 800 µs（见 §5 的实测）。
- 可观测：`sweepBegin` 每轮打印/清零 per-round 计数（`:3200-3203`，键 `round_hits=` / `round_checks=`），终值行在 `pauseDump`（`:3459`，键 `hits=` / `checks=`），`statsDump` 行（`:2035`）的键改为 `hits_total=` 以避免 `hits` 键互相覆盖。

## 2 机制 B：懒复用链 + nfree

### 2.1 数据结构

`ExtcRcSlab` 新增字段（`:250-253`）：

```c
uint32_t      nfree;     /* P0-B hole count: slots whose occupancy bit is clear but which are not necessarily on the reuse
                          * chain: the sweep leaves holes unlinked, and the reuse path rebuilds the chain from the
                          * occupancy bitmap once per visit. Invariant: the chain holds a subset of the holes, with no
                          * duplicates; a rebuild only runs while freelist == -1 (chain empty). */
```

`slabNew` 尾部初始化 `s->nfree = 0;`（`:851`）。sweep 的整块批量重置也置 0（`:3175`）。

### 2.2 分配端

`:1405-1460`：

- 「满」判据：`if (!s || (s->bump >= s->nslots && s->nfree == 0))`（`:1405`），大小类链的懒惰弹出循环同判据（`:1410`）。
- 取槽顺序：① `freelist >= 0` 弹一个，`s->nfree--`（`:1424-1428`）；② 仍有未发号槽（`bump < nslots`）则 `bump++`；③ 否则从 `occ` 位图重建整条链（`:1432-1449`）：逐 word 取 `occ` 的反码、`ctz` 逐位走、把槽号压入 `tid[]` 链，`chained++`；结束后 `s->nfree = (uint32_t)chained;`（**用位图实际链长覆写**，不信增量，顺带兜住 uint32 下溢）。
- 重建后仍空（位图陈旧 ⇒ 真的满）：`s->nfree = 0;` 并 `slabNew`（`:1455-1458`）。

### 2.3 释放端

- `freeSlot`（热路径定向释放/计数归零）：保留链头插 `freeSlotLink`，随后 `s->nfree++;`（`:1615`）。注释说明该槽已 occ=0，sweep 的 `dead = occ & ~mark` 不会再包含它，故 nfree 不会双计。
- `freeSlotWord`（sweep 整词批量释放）：不再逐槽链头插（只 `!NOCOUNT` 时清 `hold[]`），改为 `s->nfree += n;`（`:1664`，其上是 `:1661-1663` 的双计论证注释）。

### 2.4 不变式（B 的正确性）

1. **链是洞集合的子集，无重复**：链只由 `freeSlot`（此时 occ 已清、槽不在链上，因为它在用）或重建产生；重建只在 `freelist == -1` 时跑（链空），故不会把已有链项再插一次。
2. **nfree 是洞计数，不是链长**：`nfree` 只增（释放）只减（弹出）；重建后用位图真实链长覆写；「满」判据只在 `nfree == 0` 时成立，因此不会把「从未发号 + 有洞」的块误判为满。
3. **不可复用槽不会被当成可复用**：`bump >= nslots` 后所有槽都已发过号；重建只从 `occ == 0` 的位取槽，因此不会把活槽或从未发号槽放进链。
4. **计数档**：`freeSlotWord` 删掉的链头插在计数档下还要清 `hold[]`/维护 `rsbit`，实现保留在 `#if !EXTC_RC_NOCOUNT` 内；计数档路径不变。

## 3 正确性 / 不变式论证（P 与 B 的交叉）

- **P 不改变任何回收决策**：配额只决定「这次分配推进多少步」，`sweepStep` 的判据（`occ & ~mark`）、`markStep` 的判据（mark 位 dedupe）都未改动。P 只影响「何时推进」，不影响「谁死谁活」。
- **B 不改变任何可达性**：懒重建只在需要新槽时把已有洞接回链；链上的槽在重建前不会被复用，重建后每个洞恰好出现一次。
- **alloc-during-sweep × P 的交互**：P 的 sweep 预算（固定 8 + 当前 slab 的 words）通常足以扫完游标所在 slab，因此「在未扫 word 分配并标黑」的旧形状变得难以构造。这是测试形状问题，不是正确性问题：`slabNew` 的 `if (r->sweeping) s->swEpoch = r->swId;` 仍保证在 sweep 中新建的块不会被本轮扫到，也不留下黑位（P1-7 审计已逐类核对三种落点）。
- **B 与 elide 的交叉**：整块批量重置（P1-7 `extc_rc_sweepElide`）在重置后置 `s->nfree = 0;` 与 `bump = 0`、`freelist = -1`，块回到「从未发号」状态，与 B 的满判据一致。
- **计数档**：cascade（`extc_rc_freeSlotCascade`）只在 `#if !EXTC_RC_NOCOUNT` 段发射；默认档产物逐字节不含 cascade 符号。B 的 `nfree` 与 `hold` 无耦合。

## 4 改动概览（函数 / 行号）

| 内容 | 位置（`src/back/reclaim.c`） |
| --- | --- |
| P 机制总注释（闭式、anchor、deadline 语义、18 ms UNTESTED） | :979-997 |
| 常量 SHIFT_MARK / MAX_WORDS / MAX_MARK / DEADLINE_US | :1001 / :1004 / :1008 / :1022 |
| alloc sweep 分支（字节配额） | :1353-1359 |
| alloc mark 配额循环（含钳位、deadline） | :1363-1389 |
| `extc_rc_markAdvance` 返回本步 work units | :1325（返回），:2979（`markWork` 累加） |
| `[quota]` 全局计数 | :928 |
| 每轮 `[quota] round_hits/round_checks` | :3200-3203 |
| 终值 `[quota]`（pauseDump / statsDump） | :3459 / :2035（`hits_total=`） |
| `ExtcRcSlab.nfree` + 不变式注释 | :250-253 |
| `slabNew` 初始化 `nfree=0` | :851 |
| alloc 满判据 / 取槽 / 位图重建 | :1405 / :1405-1458 |
| `freeSlot` `nfree++` | :1615 |
| `freeSlotWord` `nfree += n` + 双计论证 | :1661-1664 |
| elide 批量重置 `nfree=0` | :3175 |

## 5 常量与旋钮

| 常量 | 默认 | 语义 | 对停顿的影响 | 对内存上界的影响 |
| --- | --- | --- | --- | --- |
| `EXTC_RC_QUOTA_SHIFT_MARK` | 3 | 每 `2^3=8` 请求字节换 1 个 mark work unit | 单次分配的 mark 配额线性于字节数；被 MAX_MARK 截断 | 越大 ⇒ 单位字节的回收信用越少 ⇒ 大块垃圾回收更慢、峰值更高 |
| `EXTC_RC_QUOTA_MAX_WORDS` | 8192 | sweep 字节配额上限（word） | 8192 words ≈ 78–98 µs（9.5–12 ns/word，empirics）；对合法 slab（≤65536 槽 ⇒ ≤1024 words）**不生效**，只防御损坏/超大 nslots | 只在 nslots 异常大时生效 |
| `EXTC_RC_QUOTA_MAX_MARK` | `1<<20` | mark 配额上限（work units） | `1<<20` 的稠密最坏理论上界 ≈18 ms（`18 ms = 1<<20 × ~17.5 ns`，**UNTESTED**：需要一次分配请求的字节与稠密指针数组同时成立）；实测 pd 200 单步 max 不为该通道 | 太小会让 8 MiB big 的 mark 分段拖到下一次分配，峰值上升（见下表） |
| `EXTC_RC_QUOTA_DEADLINE_US` | 800（0=off） | mark 配额循环的物理墙钟上限 | 单步 `D + 2×segment` ≈ 0.96–1.0 ms（一个 segment ≤ 8192 words）；`_quota<4096` 时不读钟 | 超时放弃剩余配额 ⇒ drain 变慢 ⇒ 峰值上升；hits 持续增长即表示 deadline 在替代配额驱动回收 |
| `EXTC_RC_SWEEP_PRESSURE_MUL` | 4 | 接近内存墙时放大固定步预算（不含字节配额） | 每步固定部分 ×4 | 内存墙附近更快推进 |
| `EXTC_RC_SCAN_CHUNK` | 8192 | 大对象分段扫描的窗口（word） | `P_step <= C_scan × SCAN_CHUNK`（约 28–98 µs，依对象密度） | 与内存无直接关系 |

### 5.1 旋钮—代价三列表（pd_bigobj 200 轮，PAUSE=1 STEP_CPU=1）

| 配置 | 峰值 RSS | 单步 max / over256 | [quota] hits |
| --- | --- | --- | --- |
| 默认：MAX_MARK=`1<<20`，deadline=800 | 26,636–26,648 kB（3 reps） | 98.9–190.3 µs / 0 | 0–1 |
| `-DEXTC_RC_QUOTA_MAX_MARK=8192`，deadline=800 | 26,648 kB | 154.9–351.1 µs（不同 reps）/ 0–1 | 1 |
| deadline=250（pause-first 变体） | 34,712 kB | 261.0 µs / 1 | 100 |
| deadline=0（off） | 26,460–26,648 kB | 约同默认 | 0 |

结论：在当前代码上，把 MAX_MARK 降到 8192 并不能换来足够小的峰值收益（峰值相同），而 250 µs 的 deadline 反而破坏 G18 峰值（34.7 MB）。因此默认取 memory-first（MAX_MARK=1<<20）+ 800 µs deadline。

## 6 验收数字（最终源码 7c2a9ce / 构建 7019342）

### 6.1 测试

- 默认档：`bash tests/reclaim/run.sh` → **181 通过 / 0 失败**；同轮含 ASan/UBSan 181/0、SHADE_GUARD bad=0、`ALLOC_INLINE=0` 181/0、`chunk=1` 10/0、`chunk=1e9` 红自测成立。
- 计数档：`EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh` → **237 通过 / 0 失败**（ASan 237/0）。
- `gate.sh`：`EXTC=./build-rf/extc bash tests/reclaim-perf/gate.sh` → PASS，**86,618,661 Ir ≤ 88,029,115**（槽号索引域内穷举 1,433,436 组、不符 0 处）。
- `python3 tools/check_skip_table.py` → ok（62 类标记、未标记早退 0）；`python3 tools/check_no_history.py --gate` → ok。
- 生成物三档（默认 / NOCOUNT=0 / SWEEPFREE=1）`-Wall -Wextra -Werror=format` 零警告。

### 6.2 G18 / 内存

| 形状 | 峰值 RSS（3 reps） | [step] max / over1ms | [quota] hits |
| --- | --- | --- | --- |
| pd_bigobj 60 轮 | 26,644–26,648 kB | 83.8–302.9 µs / 0 | 0–1 |
| pd_bigobj 200 轮 | 26,636–26,648 kB | 98.9–190.3 µs / 0 | 0–1 |
| pd_pointer_dense 200 | 112,280–112,284 kB | 199.6–369.3 µs / 0 | 0（checks=0） |

- dense 的 `checks=0` **与请求字节无关**（8 MiB = `1<<20` units，远超 4096）：探针显示每次 8 MiB 分配发生时 `marking=0`（没有 mark 轮在飞）⇒ **配额循环根本没进**；即使进入，`_dl0` 也只在 `_quota >= 4096` 时才读时钟，而叶子形状的 `n=8` 只给 `quota=1`。其慢尾来自既有 `markResume` 分段续扫（`[top]` pops=1、seg=8192、246.9–296.4 µs），**不是配额通道**（结论不变、理由更正，Lead 采纳 doc-audit §13.3-A1）。
- 单步 max 的 rep 间波动（83.8–302.9 µs）是实测噪声，不是随分配大小单调：中位仍在 100 µs 量级，over1ms=0。

### 6.3 chain2（G8）

- 默认档 `rss_kb=3032`；事件档 `EXTC_RC_SWEEPFREE=1` `rss_kb=1496`；两档 `out_hash=3474c4732ebf09607277cd58cf167bd71e56a70da4328bcec78b47c2850a9564`（200 万节点）。

### 6.4 四个 v2 形状（对照构建 build-lead5/extc）

- 运行参数：promote `500000 20261013`、sizeratio `2000000 20261013`、chase `1048576 20261013 4000000`、cycleratio `400000 20261013 100`；编译 `-O2 -std=c11 -D_POSIX_C_SOURCE=200809L`。
- stdout：四形状全部逐字节相同（md5 promote `f49b180f…`、sizeratio `357f44e0…`、chase `783e4f34…`、cycleratio `e98affe9…`）。
- RSS（new/base）：promote 5920/6492（−8.8%）、sizeratio 3056/3052（+0.13%）、chase 73696/73692（+0.005%）、cycleratio 2848/2844（+0.14%）。
- 30 rep 交错墙钟中位：promote −0.16%、chase −0.98%、cycleratio −0.09%、**sizeratio +10.34%**（超出 ±3%）。
- GC 步（PAUSE=1，base→new）：promote `[step]` n=21288→11088、total 18,402→15,222 µs、max 154.3→150.9；sizeratio n=2041→1200、total 2,630.8→1,048.9 µs、max 83.7→8.9。GC 步总时长不回归。

### 6.5 [quota] 两行样例（pd_bigobj 200，默认档）

```
[quota] round_hits=0 round_checks=4096
[quota] round_hits=0 round_checks=0
[quota] deadline_us=800 max_mark=1048576 shift=3 checks=836096 hits=0 hits_round=0
```

（per-round 行每轮打印一次并清零；终值行在退出 dump 时打印。）

## 7 复现

```bash
make BUILDDIR=build-rf -j24
bash tests/reclaim/run.sh                      # 默认档 181/0
EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh    # 计数档 237/0
EXTC=./build-rf/extc bash tests/reclaim-perf/gate.sh
python3 tools/check_skip_table.py
python3 tools/check_no_history.py --gate

# G18
./build-rf/extc tests/gc-p0/src/pd_bigobj.extc -o /tmp/pd.c
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L /tmp/pd.c -o /tmp/pd
/usr/bin/time -f 'RSS=%M' env EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1 /tmp/pd 200

# 旋钮对照
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -DEXTC_RC_QUOTA_MAX_MARK=8192 /tmp/pd.c -o /tmp/pd8192
cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -DEXTC_RC_QUOTA_DEADLINE_US=250 /tmp/pd.c -o /tmp/pd250
```

## 8 局限与未测风险

1. **markCarry 已移除**：配额按「每次分配」结算，不跨分配结转剩余信用。800 µs 的收益来自「一次 drain 能在 800 µs 内完成」，而不是结转。
2. **deadline 超时丢弃剩余配额**：break 后本次分配不再推进；若 hits 持续增长，配额不再主导，内存上界依赖 deadline 的重复触发，峰值可能上升（观测 `[quota] hits` / `hits_round`）。
3. **18 ms 理论最坏未测**：`QUOTA_MAX_MARK × ~17.5 ns ≈ 18 ms` 是「一次分配请求的字节数与稠密指针数组同时成立」时的上界；本文没有该形状的实测，不能当作实测值引用。
4. **deadline 时钟固定墙钟**：`EXTC_RC_QUOTA_DEADLINE_US` 用 `pauseNow()`（墙钟）；而 `[step]`/`[histo]` 在 `EXTC_RC_STEP_CPU=1` 时是线程 CPU 时钟。两者口径不同，比较时需注意。
5. **sizeratio 墙钟 +10.34% / Ir +6.1% 未解释**：profile 归因受内联影响不可靠；待后续用更细的计数点复核。
6. **regionFree 仍是独立无界通道**：不在 P 的配额内（`pb_escape_grow` 10^8 参数：`max_frees=220,377`、`[chan] regionfree max_us≈167.7 ms`）。本任务不处理。
7. **markAdvance 单步预算下限 512 > MARK_BATCH_MAX 256**：`wbDebt` 的「带息偿还」实际上被 256 的截断吃掉，debt 助推无效（与本任务的配额循环无关，属既有行为）。
8. **freeSlot 双释放成链环风险**：若调用方对同一槽重复释放，`nfree` 会虚高且链可能成环；现有测试与 ASan 未触发，但代码未加防御（属既有契约：调用方不得双释放）。

## 9 与「sweep 次数小常数」契约的关系

P 的字节配额会让 sweep 在每次大分配后推进到「扫完当前 slab」，从而**增加** sweep 次数；这是内存有界的代价（更多工作、更短步长）。旧契约断言「sweep 次数是小的常数」在夹紧/配额下不成立；现行判据是「峰值驻留 ≤ M_max + 缓冲」与「总 step 时间不回归」。两项由 §6.2/§6.4 覆盖。
