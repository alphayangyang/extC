# AUDIT-P0-IMPL：P（配额按字节）+ B（惰性 freelist）落地的独立复核

- 状态：**阶段一（静态清单）完成；落地版静态复核到冻结快照 `7c2a9ce`（2026-11-09 16:05）完成；阶段二（Lead m03238 GO 后）的独立门禁已在 build-da（md5 `21051bbf…`）上跑完，见 §13（16:19–16:45），本文件为终稿**。**15:45 增补**：§10.7（Lead m02954 裁决：memory-first + `EXTC_RC_QUOTA_DEADLINE_US=250µs` 兜底，含阶段二三行停顿通道判据）、§10.8（`QUOTA_MAX_WORDS` 永不生效的精确核算：slotSize 下限 16B ⇒ `nslots<4096`）、§10.9（deadline 审计清单 DL1–DL8）、§11（P0-B 静态不变式审计 B-I1..I9）。**15:53 增补**：§10.10（deadline 首次落地的逐条静态审计：DL1/DL4 ✓、DL2/DL5/DL7/DL8 为 C、DL3/DL6 为 A；默认值 250→800 与 m02954 冲突；`markCarry` 出现后被移除；1382-1383 重复检查残留）。**15:30 增补**：G18/G20 门禁已落地（`run.sh:242-291`、`summarize.py:376-428`、`g18.py`、`g20.py`），已按落地版审计（§4.0/§4.1）；P/B 已于 15:32–15:38 落地，基线见 §0.1.1。**16:05 冻结增补**：§10.11（G18 门禁脚本侧 A×2：deadline 常数 250 过期、W_s 用永不生效的 cap）、§10.12（15:56:15 快照复audit：DL3/DL6/DL5 已修、N-1..N-4）、§10.13（**冻结快照 7c2a9ce 收口 N-1..N-4**；与 P0-IMPL §6.2 的 dense `checks=0` 解释矛盾）、§12（阶段二待跑清单 + 11 条 gate 命令表 + 独立重算清单 + 判据修正建议）。
- 原则：**只报不改**。不修改 `src/`、`tests/`、其他 `docs/`；本文件（`docs/analysis/AUDIT-P0-IMPL.md`）是唯一写入。不 `commit`。
- 分级：**A**=错误断言（与源码/实测矛盾）；**B**=行号/口径偏移；**C**=缺证据（无法从原始日志/命令重算）；**D**=真 bug（语义或内存安全）。

## §0 口径、证据与当前状态

### §0.1 当前树（预落地基线）

| 项 | 值 | 证据 |
|---|---|---|
| git HEAD | `85b8871b` | `git rev-parse HEAD` |
| `src/back/reclaim.c` | md5 `9af9ca9b8f6ad939a577f99404ddf319`，**3667 行** | `md5sum` |
| `src/back/codegen.c` | md5 `d339382889b87c0dd18b357c69e2e779` | `md5sum` |
| 口径 | 源码 FREEZE **v5**（与 `docs/analysis/P0-PROTOTYPE.md:3` 自述的 build-lead5 源码 md5 一致） | 两处 md5 相同 |
| P 标记 | **无**：`grep -n "QUOTA_MAX\|QUOTA_SHIFT\|nfree\|P0P" src/back/reclaim.c` 无命中 | 证 P/B 尚未落地 |
| `docs/analysis/P0-IMPL.md` | **不存在** | `ls` |
| task-24 | `pending`、`blocked_by task-22/23`、claim 被拒（`not ready to claim`） | task board |

⇒ 阶段一只能做**静态清单 + 预置风险**；任何门禁数字、停顿数字、RSS 数字现在都**未测**，不得写成本轮结论。

> 15:30 变化：G18/G20 门禁脚本已落地（见 §4.0），但 P/B 源码仍未落地（上表 grep 仍为空）⇒ 阶段二开跑前必须重取 `reclaim.c`/`codegen.c` md5，不能沿用 §0.1 的基线。

### §0.1.1 落地后基线（P/B 已落地，供阶段二重取）

| 项 | 值（快照时点 15:38:17） | 证据 |
|---|---|---|
| `src/back/reclaim.c` | md5 `533fd3d63f9536b307c9b61ced2017bf`，**3740 行**，mtime **15:38:15** | `md5sum` / `wc -l` / `ls -l` |
| 早前落地快照 | md5 `b9225d8ed9a445cc4de9c760b4146d66`，3740 行，mtime 15:32（**已被上含取代**；树仍在动） | 同上 |
| **最新快照（§10 依据）** | md5 `abef7e345e70689859a5d81428265193`，**3750 行**，mtime **15:41:02**（上两行均被取代；P 语义已改、D1/D2 已修，见 §10） | `md5sum` / `wc -l` |
| **更新快照（§10.7/§11 依据）** | md5 `ad8054f59c83798385d84fff3b8cbd2f`，**3751 行**，mtime **15:42:57**；`grep -n "DEADLINE\|deadline"` 仍**零命中** ⇒ deadline 未落地 | `md5sum` / `wc -l` / `grep` |
| **deadline 落地快照（§10.10 依据）** | md5 `f7f7011e57ff9c9e31935d9e3dcf1491`，**3794 行**，mtime **15:52:59**；`build-rf/extc` md5 `bffd9c9fafc2ff47e82150b0f27f7d5d`（同刻）。15:48:48 首版 md5 `ad32f8ab344a89679f405b7b1e323090`/3788 行（默认 250）；15:50:54 `f2aa4b27…`/3798 行曾出现 `markCarry` 结转；15:52:59 结转被移除、默认改 800 | `md5sum` / `wc -l` / `ls -l` / `sed` |
| **冻结快照（§10.13/§12 依据，唯一有效）** | `reclaim.c` md5 **7c2a9cedd7edd54de0a378d8a507dcbe**、**3801 行**；`build-rf/extc` **7019342b3e5ccddbe67cbdc96eae2395**；`driver.c` **83e167d761705268dbda3f89895ea574**；`P0-IMPL.md` c5a715ed…（233 行）。Lead m03189 宣布源码冻结；此前各行均被取代 | `md5sum` / `wc -l`（已复核一致） |
| `src/back/codegen.c` | md5 `d339382889b87c0dd18b357c69e2e779`（未变；本轮无编译侧改动） | `md5sum` |
| 参考产物（仅线索） | `build-rf/extc` md5 `ea655ff4c883963cf0c5536eb180d8a1`（15:38:15） | `md5sum` |
| P/B 标记 | `QUOTA_*` 宏 `982-995`；sweep 配额 `1320-1327`；mark 配额 `1333-1337`；B 的 `nfree` 12 处（§9.7） | `grep -n` |
| 阶段二自建 | 编译器：`make BUILDDIR=build-da -j24`（Makefile 默认 `-std=c11 -Wall -Wextra -Wpedantic -O1 -g`，要求 warning=0）；生成 C：run.sh 的 `CFLAGS=-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`（**不复用** `build-rf`/`build-emp`） | 约定；见 §12.1 |

> 树在 15:32 后仍被 runtime-facts 修改（15:32→15:38 md5 已变）⇒ 阶段二必须以当时的 md5/行数重新钉基线；本表数字只对 15:38:17 快照成立。

### §0.2 原型数字的效力边界（必须先钉死）

`docs/analysis/P0-PROTOTYPE.md:3` 与 §7 局限 1（`:204`）：全部实验是**生成 C 上的文本补丁**，不改 `src/`；`:205` 自述 B 的计数档安全性**只做了静态论证、没跑计数档全量断言**。⇒ 原型数字只能当「方向性预期」，落地后**必须重测**；本审计不把原型数字当证据，只当待验证的对照值。

### §0.3 方法（阶段二）

1. 自建快照：`git archive HEAD 85b8871b` + 当前工作树 patch → `/tmp/p0impl-repo`，在其中 `make BUILDDIR=build-da -j24`（`-Wall -Wextra -Wpedantic`），**不复用** `build-rf`/`build-lead5`；记录 `build-da/extc` md5 与源码 md5。
2. 先静态（§1/§2），后跑（§3/§4）；每跑一条把命令、退出码、原始输出路径写进本文件。
3. 判据只认磁盘日志；「我独立跑的」与「我只核对的」分开列（§7）。

## §1 P（配额按字节）审计清单

源码锚点（v5 行号，落地后会漂移，以函数名+语句为准）：

| 位置 | 现内容 | 期望改动 |
|---|---|---|
| `reclaim.c:1298` | `else extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET * (pressure ? MUL : 1));` | `budget = max(原值, n>>QUOTA_SHIFT_SWEEP)`，且 `min(budget, QUOTA_MAX_WORDS)` |
| `reclaim.c:1302` | `if (r->msp != 0 \|\| !r->seedsDone \|\| r->scanObj) extc_rc_markAdvance(r);` | 先 1 次，再补 `n>>QUOTA_SHIFT_MARK` 批（≤ `QUOTA_MAX_BATCHES`），循环条件同原 if |
| `reclaim.c:1307` | `if (r->seedsDone && r->msp == 0 && !r->scanObj) extc_rc_finish(r);` | 循环结束后仍须执行一次 |

- **P1 实现位置/形式**：两处都在 `extc_rc_alloc` 的 `sweeping`/`marking` 分支内（`1294-1309`）；确认没有第三处（`3416/3461/3503` 的 sweepStep 是 collect/step 入口，**不应该**被配额改）。
- **P2 量纲**：`n` 必须是**本次请求字节数**（`extc_rc_alloc(r,n,td)` 的 n），不是 slot stride `ss`、不是 `nslots`。sweep budget 的单位是 **words**：`n>>6` = n/64 words = payload 的 1/8（64B/word 口径需与 `extc_rc_sweepStep` 内部单位核对）。注释必须写清「字节→word 的换算与为何是 1/8」，否则读者会以为 1:1。
- **P3 常量**：`QUOTA_MAX_WORDS=(1<<18)`、`QUOTA_MAX_BATCHES=(1<<10)`、`QUOTA_SHIFT_SWEEP=6`、`QUOTA_SHIFT_MARK=10` 全部 `#ifndef` 保护；检查是否有 `#error` 守卫（shift 越界、max=0）。缺 `#error` 时记 C 级。
- **P4 极端 n**：n=0（移位后 0，退化成原预算）；n=1..63（0，退化）；n 超大（`SIZE_MAX` 附近，`n>>6` 仍可能极大 ⇒ 必须靠 cap 截断，不能只靠移位）；n 超 slab（`slotSize(n) > slab` ⇒ big slab，payload 单独 malloc；此时 n 仍应参与配额）。确认 `max`/`min` 的求值顺序不溢出（`n>>6` 不溢出；`max(8, n>>6)` 后 `min(...,1<<18)` 安全）。
- **P5 `P_step` 上界表达式 + 实测**（本轮**最重要**）：单次分配最坏 GC 工作 = `QUOTA_MAX_WORDS` 个 word 的 sweep + `QUOTA_MAX_BATCHES` 个 mark 批。代入现有成本（`CALIBRATION.md`：sweep `C_scan≈3.4 ns/word`（65536 段外推）~9.5–12 ns/word（8192 段实测）；mark `17.49 ns/obj`，每批最多 `MARK_BATCH_MAX=256` pops）：
  - sweep 侧：`2^18 × 3.4ns ≈ 0.89 ms`（若按 9.5–12ns/word 则 2.5–3.1 ms）；
  - mark 侧：`2^10 × 256 × 17.49ns ≈ 4.59 ms`。
  ⇒ **预测上限 ms 级单次分配停顿**。这直接冲击「停顿有界（5µs/200µs 门禁）」的叙事：必须给出**新的停顿上界表达式**并说明它是「分配变慢」而不是「STW」，且与 `docs/analysis/MEMORY-MODEL-CURRENT.md` §19 的 `P_max` 公式一致。实测：`pd_bigobj` 的 `[step]`/`[pause]` max、四形状 max、`[histo]` 尾部。若实测 < 预测一个量级，检查是否 clamp 常量没生效或 pressure 路径被提前 return。
- **P6 是否仍从不拒绝分配**：grep 新增分支不得出现 `oops`/`return 0`；`QUOTA_MAX` 只夹 GC 工作量。分配失败路径（slabNew 的 oops）行为不得变化。
- **P7 sweep 侧与 pressure 的复合**：`max(原值, n>>6)` 中「原值」必须**保留 pressure 乘子**（`SWEEP_BUDGET*MUL`），且 cap 在 `max` **之后**；否则大对象在 pressure=0 时被压回原预算、在 pressure=1 时又可能超 cap。
- **P8 mark 侧循环语义**：额外批次必须每轮重判 `msp!=0 || !seedsDone || scanObj`；中途就绪即停；`finish` 仍只在循环后调用一次；**不得**先 finish 再 advance（会清掉本轮 mark 位后又扫）。若实现为 `for (k=n>>10; k; k--) { if (!cond) break; markAdvance(); }`，核对 `markAdvance` 自己是否也可能促成 `seedsDone`（会，见 `markStep`）⇒ 循环条件必须每轮重算。
- **P9 门禁脚本卫生**：新增早退带同行 `skip:...` 标记并登记 `docs/design/RECLAMATION.md` 表；`python3 tools/check_skip_table.py`、`python3 tools/check_no_history.py --gate` 必须 ok（v5 基线先各跑一次记录）。
- **P10 交互**：`noSweep` 早退（`1295-1296`）优先于配额；`sweeping` 与 `marking` 互斥由既有状态机保证；确认配额不改变 `extc_rc_why` 归因（§19 计量）。

## §2 B（惰性 freelist）审计清单

### §2.1 必须同步的全部 `freelist` 代码点（v5 全仓仅 8 处）

| 行 | 语句 | 处置 |
|---|---|---|
| `844` | `s->freelist = -1;`（slabNew） | 同处加 `s->nfree = 0;` |
| `1316` | `if (!s \|\| (s->freelist < 0 && s->bump >= s->nslots))` | 满判据改 `(s->bump >= s->nslots && s->nfree == 0)` |
| `1322` | `while (s && s->freelist < 0 && s->bump >= s->nslots)` | **同形改**（漏改 = 原型 v2 的踩坑：class 链跳过一个「有洞但 bump 满」的 slab） |
| `1336-1338` | 弹出：`i = freelist; freelist = tid[i]-1;` | 加 `s->nfree--;` |
| `1476-1477` | `freeSlotLink`（freeSlot 用） | **保留**（任务 3），另在 freeSlot 里 `nfree++` |
| `1523-1524` | `freeSlotWord` 逐槽链写循环 | **删除**，改 `nfree += popcount(dead)` |
| `3048` | sweepElide bulk reset `freelist=-1` | 同处加 `nfree = 0` |

- **B1 布局**：`uint32_t nfree;` 加在 `ExtcRcSlab`（`216-266`）；确认无 padding 引发的重复 memset/拷贝；slab 从不 memcpy/序列化。
- **B2 满判据三处一致**（`1316`、`1322`、以及重建后的 `slabNew` 判据）——**这是最容易漏的一处**。
- **B3 取槽顺序**：freelist 弹出 → bump → 重建；核「弹出后 `nfree--`」与「重建后 `nfree` 重算」不会双计。
- **B4 重建分支**：仅 `freelist == -1` 时进入；按 `valid & ~occ[w]` 扫全部 word（末 word 用 `nslots - w*64` 掩码）；重建本身走 `freeSlotLink` 还是直接写 `tid[]`？两种都可，但**必须**与 `nfree` 一致（重建把洞全部串起来后，`nfree` 应等于链长；建议重建后按实际链长覆写 `nfree`，而不是自增）；重建为空（位图无洞）⇒ `nfree = 0` 并 `slabNew`。
- **B5 不变式**（写进注释 + debug 断言）：任一时刻 ① 链无重复；② 链长 ≤ `nfree`；③ `nfree == 链长 + 未入链洞数`；④ 重建只在 `freelist==-1`；⑤ elide/新建后 `nfree==0`。建议借鉴 `EXTC_RC_SHADE_GUARD` 做成可选自检（`-DEXTC_RC_FREE_GUARD=1`），并在两档各跑一遍。
- **B6 `freeSlotWord` 重复计数**（**最高风险**）：计数档下 `freeSlot` 已把槽链入并 `nfree++`；随后 sweep 的 `freeSlotWord` 对同一 slab 再 +1 ⇒ `nfree` 虚高。核实现是否用 `popcount(dead & occ)`（只数仍占用的槽）而不是 `popcount(dead)`。若 `dead` 本身来自 `~occ` 的旧快照，需确认快照时点。
- **B7 `freeSlot`（单槽）**：任务要求「仍链入 + `nfree++`」；确认级联（`freeSlotCascade`→`freeSlot`）、`releaseOld`、定向释放都走这一条；sweep 中不调 `freeSlot`（只用 `freeSlotWord`）。
- **B8 uint32 下溢**：`nfree--` 只在弹出时；若链长 > `nfree`（双重链接 bug）会下溢成 4e9 ⇒ 判据永假 ⇒ 每个 slab 都被当「有洞」进重建。检查是否有饱和/断言；实测一个「分配-释放-再分配」压力形状看 RSS 是否线性涨。
- **B9 slab 内洞被重建链服务后一致性**：分配置 `occ` 位并 `nfree--`；断言 `occ` 与链/洞计数匹配；单槽用例（同槽反复分配释放）验证「slab 数不随轮数增长」（task-22 验收 b）。
- **B10 elide 后状态**：`sweepElide`（`3021-3060`）bulk reset 把整块变回「从未发放」（`bump=0/freelist=-1/nfree=0`），且 `occ/mark` memset 清零；核对 `r->bytes -= live*ss` 与 `nfree` 无关。
- **B11 big slab**（`nslots=1`，`big=1`）：`bump>=1 && nfree==0` 才算满；`freeSlot` 对 big 会 `classPush`（`1498`），确认 big 的 `data` 单独 malloc 不受影响。
- **B12 `regionFree`/region 复用**（`2358` 起）：整区释放不读 `nfree`；确认 region drop 后没有 slab 被复用（slab 随 region free）。
- **B13 P1-7 `sweepElide` 收益交叉**：`SEED_PUSHES/SEED_WORDS` 与 B 无关，但 elide 命中率会因 B 的「洞计数」变化？确认 elide 判据只看 `occ/mark/rsbit`，不看 freelist。
- **B14 与 `allocHot` 交互**：`allocHot[32]` 缓存的 slab 若满（bump 满 + nfree=0），`1316` 条件必须能踢走它（同形判据）；否则会反复命中同一个满 slab。

## §3 语义回归清单（阶段二执行，命令写死）

| # | 项 | 命令/判据 |
|---|---|---|
| R1 | 四形状 stdout 逐字节 | `build-da/extc` 与 v5 基线各生成一次，`cmp` 生成的 .c 与运行 stdout；promote/sizeratio/cycleratio/chase |
| R2 | 四形状墙钟/RSS ±3% | 同轮交错 5 reps，median；`/tmp/v2/v2run --pin 6 --out`（原型口径）或 bench/gc/v2 现有驱动，记录原始输出 |
| R3 | `tests/reclaim/run.sh` 默认档 + `EXTC_RC_NOCOUNT=0` | 先记录 v5 基线（自建 build-da，不抄别人的 169/218），落地后同口径对比 |
| R4 | ASan/UBSan 两档 | `-fsanitize=address,undefined`；判据 exit 0 且无 sanitizer 报告 |
| R5 | `EXTC_RC_ALLOC_INLINE=0` | 编译产物可跑、四形状 stdout 不变 |
| R6 | B 专项新断言 | slab 数不随轮数增长；elide 后 `nfree==0`；同一槽只入链一次（debug guard） |
| R7 | 既有门禁不回归 | G8/G9/G12/G13/G14/G4/G17/P0-3（沿用我在 `AUDIT-GC-P0.md` §15 的脱敏清单与判据） |

## §4 新门禁 G18/G20 的可审计性

### §4.0 状态修正（阶段一注记）

- 本节 §4.1 在 15:29 写时 `run.sh` 尚无 g18 子命令；**15:30 已落地**（`tests/gc-p0/run.sh` mtime 15:30）：G18 在 `run.sh:242-259`，判定在 `summarize.py:376-399` 与 `g18.py:126-160`；G20 在 `run.sh:262-291` + `g20.py`（18,021 B，15:30）。⇒ 阶段一「门禁未实现」的 C 类结论作废，以下按落地版逐行审计。
- 仍缺 after 数据：`tests/gc-p0/out/after/timemax.txt` 无 `pd_bigobj` 行、无 `g18.json`（after 全套跑在 G18 落地前）⇒ G18 的 after 锚点必须阶段二自跑。

### §4.1 G18（大对象内存有界）：落地版逐行审计

- 形状：`tests/gc-p0/src/pd_bigobj.extc`（8 MiB bool 数组实为 `[1048576]?mut ref leaf`，活集恒 1 个 big + 64 leaf，每轮 `w.cur=null`）。60/200 两档。
- **判据实际实现**：`run.sh:253` `timemax pd_bigobj 60|200`（`run.sh:96-104` 用 `/usr/bin/time -f '%M %e'` 取 **ru_maxrss**）→ `summarize.py:378-386` 读 p60/p200 → `summarize.py:399` `p200 <= 26*1024 AND stdout逐字 AND exit全0`；`g18.py:145-160` 同判据（`BOUND_MIB=G18_BOUND_MIB,26`）。
- **G18-1（B，口径错）**：`run.sh:242-243` 与 `summarize.py:377` 写「200 轮 VmHWM ≤26 MiB」，但判决数字是 `/usr/bin/time %M`（ru_maxrss），不是 VmHWM 采样。ru_maxrss 与 VmHWM 通常同值，但口径必须固定；建议改文案为「ru_maxrss（/usr/bin/time -f %M）」并把 `run_pause` 采的 VmHWM/CSV 标为旁证。
- **G18-2（A，数字无推导）**：26 MiB 与本形状的 live 不自洽。`tests/gc-p0/out/after/log/pd_pause_r1.err:17` `[work] ... M_max=25165824 thr_max=23068672 ...`，而 `M_max=3·live` ⇒ **live=8,388,608 B=8 MiB**；故公式 `3·live+8 MiB` = **32 MiB**，不是 26 MiB。探针注释 `tests/gc-p0/src/pd_bigobj.extc:8-10` 自称「Lead 的 ≈26 MiB 按 live≈6 MiB 计」，与实测 live=8 MiB 不符；原型实测 26,072 kB ≈ `M_max+2 MiB`（M_max=24 MiB+元数据），说明 26 MiB 是「经验更严上界」而非公式结果。
- **§4.1.1 双算法核算（Lead m02709 第 3 点要求）**：
  - 算法甲（源码可复算，推荐主判据）：`peak(200) ≤ 3·live + EXTC_RC_SLAB_SIZE`，live 由 `[work] M_max/3` 反读（本形状 8 MiB）⇒ **32 MiB**；配合 **规模无关判据** `peak(200) ≤ peak(60)×1.05`（summarize.py:400 已打印 ratio，但未入判据）。
  - 算法乙（绝对 26 MiB）：`peak(200) ≤ 26 MiB`；只能作「更严的经验硬上界」，必须在文档注明出处（Lead m03207 经验值）与它比甲更严（32→26 MiB）这一事实，否则无法审计。
  - 结论：**门禁主判据用甲（含 ratio 项）更可审计**；乙保留为 `G18_BOUND_MIB` 覆盖项即可（现状 `summarize.py:387` 已可覆盖）。
- **G18-3（C，承诺未实现）**：探针注释 `pd_bigobj.extc:10` 称「两种算法都打印」，但 `summarize.py:387` 只算 26 MiB；`g18.py:95-108` 的 `collect_one` 不解析 `[work]` 的 `M_max`/live ⇒ 落盘 JSON 里没有算甲所需的 live。建议 `g18.py` 增 `work` 字段（`M_max`、`thr_max`）并打印甲式。
- **G18-4（B，两进程两口径）**：判决 RSS 来自 `timemax`（无 PAUSE），逐字 stdout 来自 `run_pause`（`PAUSE=1 STEP_CPU=1`，`run.sh:256-257`）；两者不是同一次运行。可接受但必须写明；`run_pause` 的 VmHWM 数字未被判，建议至少打印差值。
- **G18-5（B，停顿判据过松）**：停顿只用「软」约束 `单步 max ≤1000 µs`（`summarize.py:419-425`、`g18.py:150-155`），且是单次 max。P 落地后应按 P_step 闭式（§6.1）判 p99.9/中位，否则 P 的回退（如每步 ~98 µs × 128 步）不会翻红。
- **G18-6（C，参考产物）**：期望 stdout 硬编码在 `summarize.py:389` 与 `g18.py:100`（`D n=%d made=%d s=0`），不是入库参考产物；语义上正确（末轮 `w.cur=null` ⇒ s=0），但源若改轮数/形状，判据会静默失配。建议注明来源并加一条「参考产物」文件。
- **G18-7（缺证据）**：after 全套（`tests/gc-p0/out/after`）没有 `pd_bigobj` 的 timemax/g18.json ⇒ 「after 26,072 kB」类数字目前无落地证据，阶段二重测。

### §4.2 G20（B 的停顿收益）

- 判据（task-23）：四形状 `EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，`[step]` 总时长与 max、`[histo]` 15 桶；promote 步 max ≤60 µs；GC 步总时长较 before 降 ≥10%；sizeratio ≥40%；cycleratio 不回归（原型 −12.5%/−52%/−8%，max 160.4→57.5、51.8→9.5、39.0→16.9 µs，`P0-PROTOTYPE.md:49-53`）；四形状墙钟/RSS ±3%。
- 审计点：① 「较 before」的 before 是哪份产物（build-lead5 自述、源码 md5 溯源）？必须同源码、同 CFLAGS、同 run 脚本；② `[step]` 是 cpu 还是 wall（`EXTC_RC_STEP_CPU=1` 才是 cpu；G1 的教训：86.2ms 被误标墙钟）；③ promote max ≤60 µs 与实测 57.5 µs 只差 4%，判据过紧会在机器抖动下翻红 ⇒ 建议改 p99.9 或 ≤80 µs 并注口径；④ 抽 3 个数重算（total/max 各形状）；⑤ 确认 G20 不会把 P 的收益算进 B（小对象形状 `n>>6≈0` ⇒ P 中性，可接受；但必须在报告里写明「G20 主要测 B」）。
- **G20-1（落地核对，已实现）**：判据逐条对得上——promote `step_total ≤ before×0.90`、sizeratio `≤×0.60`、cycleratio `≤×1.03`、promote `step_max_us` 的 **跨 rep 中位** `≤60`、四形状 wall/RSS 中位 ±3%（`g20.py:268-297`）；另有 stdout sha256 一致判据（`:297-302`，文件头判据清单漏写，属文档小缺口）。`before/after` 用 median across reps（`:154`、`:230-236`），`step_max_us_worst` 单独记录（`:190`）✓。
- **G20-2（B，对照未绑定）**：`g20.py` 的 JSON 记录了 `extc_md5/cflags/pin/reps`（`:186-190`），`compare` 只把它们**打印**（`:222-223`）不校验；before=build-lead5、after=build-rf 若 CFLAGS 不同会静默比错。建议 compare 加 cflags/pin 相等校验（不等即 RED/拒跑）。
- **G20-3（B，噪声判据不自洽）**：`g20.py:11-17` 用「WSL2 时钟噪声地板 78–181 µs」解释为何不用单次 max，但 `MEASUREMENTS-GC-P0.md:58` 说 **cpu 口径也不是零噪声（cpu max 34–181 µs）**；60 µs 判据与噪声同量级、原型 57.5 µs 仅差 4%。建议改相对台阶判据：`promote step_max median ≤ before×0.5`（160.4→57.5 为 0.36×，仍有余量），绝对值 60 仅作参考。
- **G20-4（C，chase 无 GC 判据）**：chase 形状只参与 wall/RSS ±3% 与 stdout，不判 `[step]`（`:268-282` 只覆盖 promote/sizeratio/cycleratio）；这是设计选择，但报告须写明「chase 只作回归护栏」。
- **G20-5（缺证据）**：`tests/gc-p0/out/` 下尚无 `p0-before`/`p0-after` 的 g20.json；MEASUREMENTS-P0-IMPL.md 若已出数，阶段二用 `g20.py compare` 独立复算并抽 3 个数。

## §5 文档一致性

- **D1 `P0-PROTOTYPE.md` 原型 → 落地**：§1.3/§4.3/§4.4 全部数字逐项对照落地实测；差异 >10% 要解释（文本补丁 vs 源码实现、clamp 常量不同、编译 flag 不同）。
- **D2 `docs/analysis/P0-IMPL.md`（runtime-facts 交付）**：核对机制描述、常量表、diff 摘要、§1 不变式论证、计数档论证、验收数字、复现命令、已知局限是否与源码一致；抽 5 处源码断言逐行核。
- **D3 `GC-ROOFLINE.md` §6 P0-12/P1-9 状态行**：能否从「计划/原型」改「已落地」——判据必须是本文件 §3/§4 全绿 + 常量与公式更新（P 改变 `P_step` 上界、B 改变 sweep 步成本），不能只凭原型数字。给出「改/不改」的明确建议与所需证据。
- **D4 `MEMORY-MODEL-CURRENT.md` §18**：P 落地后 `M ≤ M_max + 缓冲` 的「前提：回收跟得上分配字节速率」从**经验前提**变**结构保证**（配额 ∝ 字节），但前提仍是「mark 每字节的工作量有界」——核对 §18/§19 是否要加「配额截断 ⇒ 单步停顿上界」的新公式。
- **D5 `docs/design/RECLAMATION.md`**：新常量、B 的 `nfree` 状态、skip 标记表新增条目是否登记。

## §6 预置风险（阶段一静态推断，全部待阶段二证实/否证）

1. **P 的单步停顿上界须按 word 计价并给闭式**（Lead m02709 已采纳）：sweep `min(n>>6, 8192)` words、mark 累计 8192 words。逐项核算：①sweep 的 `n` 是 slab 槽数，bitmap words=ceil(n/64)；常规 slab ≤1 MiB、slotSize≥16 ⇒ nslots≤65536 ⇒ **W_s≤1024 words**（8192 上限对本仓 slab 基本不 binding），按 c_scan≈9.5–12 ns/word ⇒ **≤~12 µs/步**；②mark 的 8192 words 累计 ⇒ **≈78–98 µs/步**（c_scan 实测口径），这是 P_step 的主项；③**量纲陷阱（A 类风险）**：nslots=1 的 huge slab（如 G18 的 8 MiB big）`n>>6=0` ⇒ 永远不推进，必须 `W_s=max(1, min(ceil(nslots/64), 8192))`；若落地版仍用「批次数」作 mark 配额 ⇒ A 类上报。⇒ P_step 闭式主项 ≈0.1 ms，必须与 5 µs 门禁分通道列进 §19。
2. **满判据两处必须同形改**（`1316`/`1322`）：原型 v2 就栽在这（`P0-PROTOTYPE.md:35`）。
3. **`nfree` 与链长解耦**（B4/B6）：重建后必须按链长重算而非自增；`freeSlotWord` 只数 `dead & occ`。
4. **计数档 + sweep 交叉重复计数**（B6）：`freeSlot` 已链入的槽若再被 `freeSlotWord` 计一次，`nfree` 虚高 ⇒ 满判据失真 ⇒ RSS 涨；这是原型未覆盖的路径（`P0-PROTOTYPE.md:205` 自认）。
5. **G18 的停顿列已补但判据过松**：`run.sh:256-257` 收 `[histo]/[step]/[top]`，`summarize.py:419-425` 只在单次 max ≤1000 µs 上给「软」判定 ⇒ P 的 ~0.1 ms/步回退不会翻红。应改按 §6.1 闭式判 p99.9/中位。
6. **G20 只测小对象**：P 在小形状近似中性（`n>>6≈0`），G20 的收益主体是 B；报告须写明「G20 不测 P」，P 的代价由 G18 停顿列与 §6.1 闭式承担。
7. **`P_step` 上界表达式缺失**：交付物 (2) 需要闭式，建议：`P_step ≤ c_s·W_s + c_m·W_m + c_pop·pops`，`W_s=min(max(1,ceil(n/64)),8192)`、`W_m=8192`（累计）、`c_s≈9.5–12 ns/word`、`c_m` 同量级 ⇒ 上界 ≈0.1–0.2 ms/步；落地后作为 §19 新公式落盘，并在 §19 表中与 5 µs 门禁分列（不同形状/不同通道）。
8. **B 的 `nfree` 覆写点**：Lead 要求「重建后用实际链长覆写 `nfree`」⇒ 审计必须找到该赋值并验证 ①重建函数出口处 `nfree == 链长`、②重建末 word 用 `nslots - w*64` 掩码（否则 `~occ` 越界位会链出 nslots 之外的槽 ⇒ `tid[]` OOB/`uint32` 下溢）。
9. **满判据必须三处同形**：`alloc` 的 `freelist<0 && bump>=nslots`、class 链跳过循环、`allocHot` 缓存判空；任何一处漏改 ⇒ bump 满但有洞的 slab 被跳过（浪费洞，非正确性），但若缓存侧漏改会出现「有洞仍判满」⇒ P/B 收益归零。

## §7 阶段二执行顺序与记录模板

1. 快照 + 构建（记录源码 md5、`build-da/extc` md5、编译警告数）。
2. 静态核对 §1/§2 清单，逐项写「符合/偏离/不适用 + 行号」。
3. `tests/reclaim` 两档 + ASan/UBSan（先证明正确性）。
4. 四形状回归（R1/R2）+ G20。
5. G18（60/200 轮）+ P 停顿实测（`[pause]`/`[step]`/`[histo]`）。
6. 既有门禁抽跑（G8/G9/G12/G13/G14 + G4 之一）。
7. 文档 D1–D5 核对，收尾：A/B/C/D 分类统计 + 最严重 3 条 + 「独立跑 vs 只核对」分界 + 给 Lead ≤40 行摘要。

每条的记录格式：`| 编号 | 断言/判据 | 我跑的命令（路径） | 退出码/原始输出 | 判定 |`。

## §8 尚未做的事（明确边界）

- 未跑任何程序/门禁（P0 未落地，Lead 未发 GO；且阶段一约定）。
- 未核对 `build-rf`（runtime-facts 的产物）——它可能先于源码改动存在，**不可**当落地证据；阶段二只用自建 `build-da`。
- 未审计 `src/back/codegen.c`（本轮 task-22 明确「不要碰 codegen.c」，无编译侧改动）。

## §9 落地版静态复核（P/B，15:32–15:38 落地；快照 md5 `533fd3d6…`）

> **已被 §10 部分取代（15:41:02 快照 `abef7e34…`）**：P 的配额语义（sweep 改按游标 slab 的槽数、mark 改 `n>>3` 且默认上限 `1<<20`）、D1/D2 状态（**均已修**）、§9.6 的两处注释（**均已修**）都以 §10 为准。§9 保留为 15:32–15:38 时点的审计记录，B（§9.7）不受影响。

### §9.1 结论速览

- **P（配额按字节）**：落地形态与 Lead m02709 的要求一致（word 计价、两处配额、cap 常量 `#ifndef`、分配语义不变、pressure 乘子保留）。**无 A 类机制偏离**；有 **1 条 A 类注释错**（§9.6）、**1 条 D 类真 bug**（§9.4）、**1 条 D 类并发缺陷**（§9.5）。
- **B（惰性 freelist）**：三处满判据同形、重建末 word 掩码、`nfree = chained` 覆写、hot/elide/新建清链清计数 **全部符合**；静态不变式成立（§9.7）。**0 条 D**；2 条 C（无 FREE_GUARD 自检、无专项压力证据）、1 条 B（注释未覆盖 sweep×releaseOld 交叉）。
- **P_step**：闭式与数值见 §9.3。主项 **0.19–0.26 ms**（8 MiB 分配，无续扫）；大对象续扫同期 **0.27–0.36 ms**（默认 `SCAN_CHUNK=8192` words = 64 KiB 一段，实测 78–98 µs；65536 是 `-D` 变体，164–269 µs）。⇒ 必须与 5 µs 门禁**分通道**进 §19；G18 的软约束「单步 max ≤1000 µs」太松（P 回退到 0.5 ms 不会翻红）。

### §9.2 P 逐项核对（清单编号沿用 §1）

| # | 落地证据（快照行号） | 判定 |
|---|---|---|
| P1 位置 | sweep 配额在 `extc_rc_alloc` 的 `if (r->sweeping)` 分支内 `1320-1327`；mark 配额在同一函数的 `if (r->marking)` 分支内 `1333-1337`。其余 5 处 `sweepStep` 调用（`3237` collect 内 drain、`3489` step、`3534`/`3576` `skip:sweeping-resume`）**未被配额改** ✓ | 符合 |
| P2 量纲 | 注释 `977-981` 逐字：「the caps are the pause bound」「The unit is **bytes of the request**」「This is the structural premise of the memory bound M <= M_max + buffer」。`n` 是 `extc_rc_alloc(r, n, td)` 的请求字节；sweep 单位 word（`_q = n >> 6`，`1324`），mark 单位 [work]（`_quota = n >> 10`，`1333`）✓ | 符合 |
| P3 常量 | `982-995`：`QUOTA_SHIFT_SWEEP 6`、`QUOTA_SHIFT_MARK 10`、`QUOTA_MAX_WORDS 8192`、`QUOTA_MAX_MARK 8192`，四个都 `#ifndef` 保护 ✓；**无 `#error` 守卫**（shift 越界/上限为 0 未拦） | 符合 / C |
| P4 极端 n | `n<64` ⇒ `_q=0`（只剩基预算 `8·pm` words）；`n=512 KiB` ⇒ `_q=8192` 恰好顶到 cap；`n=8 MiB` ⇒ `n>>6=131072` 被 cap 截到 8192 ✓；`(uint32_t)(n>>6)` 在 `n≥2^38`（256 GiB）时先截断再 cap，但该请求在 `slotSize`/`slabNew` 前就会失败 ⇒ 实际不可达（记 B 备注） | 符合 |
| P5 P_step | 见 §9.3（闭式 + 实测口径）；实测待阶段二 | 闭式已给 |
| P6 从不拒绝分配 | 新增分支无 `oops`/`return 0`；`sweepStep`/`markAdvance` 的返回值只用于配额累计，不参与分配成败 ✓ | 符合 |
| P7 pressure 复合 | `1322-1323` 先 `_b = EXTC_RC_SWEEP_BUDGET * (pressure ? MUL : 1)`，`1326` 再 `_b += _q`，**配额另加而非替换**（比清单要求的 `max` 更强）；cap 只作用在 `_q` 上，基预算的 pressure 乘子保留 ✓ | 符合（更强） |
| P8 mark 循环语义 | `1337` 每轮重算 `(r->msp != 0 || !r->seedsDone || r->scanObj)`；`1344` 的 finish 在循环外、循环后只调一次；未先 finish 再 advance ✓ | 符合 |
| P9 门禁卫生 | 新增早退仅沿用既有的 `skip:mark-tick`（`1274`），配额路径没有新增 skip 标记 ⇒ `check_skip_table.py` 无需新登记（阶段二仍跑一遍） | 符合 |
| P10 交互 | `noSweep` 在 sweep 分支先判（`1315-1316`）并整段跳过配额 ✓；`why`：sweep 配额路径仍是 `alloc`（`1315`），mark 路径由 `markAdvance` 写 `advance`（`1276`）⇒ `[worst]` 归因不变 ✓ | 符合 |

### §9.3 P_step 闭式（word 计价；Lead m02709 第 1 点要求）

定义：`n` = 本次请求字节，`pm` = pressure 乘子（1 或 4），`c_s` = sweep word 成本，`c_m` = mark [work] 单位成本，`c_scan` = 大对象段扫描 word 成本。

```
W_s = 8·pm + min(n >> 6, 8192)                    # sweep words
W_m = min(n >> 10, 8192)                          # mark [work] units
b   = min((MARK_EVERY·MARK_STEP + debt)·pm, 256·pm) = 256·pm   # 单次 advance 内部预算
tail = min(SCAN_CHUNK, remaining) words，默认 SCAN_CHUNK=8192（reclaim.c:2640）；仅当进入该步时
       scanObj 挂起才付一整段（一次 advance 付一段，不可再分）
P_step ≤ c_s·W_s + c_m·(W_m + b) + c_scan·tail·[scanObj 挂起] + c_f
```

代入实测口径（`c_s=c_scan=9.5–12 ns/word`（8192 段实测；65536 段为 164–269 µs/段）、`c_m≈17.5 ns/unit` 的保守上界）：

| 形状 | W_s | W_m | P_step 上界 |
|---|---|---|---|
| 16 B 小对象 | 8–32 | 0 | ≤ 0.4 µs |
| 512 KiB | 8200 | 512 | **78–98 µs** |
| 8 MiB（big slab，无续扫） | 8200 | 8192 + 1024 | **0.19–0.26 ms** |
| 8 MiB + 大对象续扫同期 | 8200 | 同上 + 一段 | **0.27–0.36 ms**（段项 78–98 µs @ SCAN_CHUNK=8192） |

结论：①P 把「分配触发的 GC 工作」从常数抬到**与请求字节成正比**（这是内存界的前提），代价是单次分配停顿 **0.1–0.5 ms**，与 5 µs 门禁/200 µs 判据不相容，必须单列通道；②闭式里的**主项是配额本身**，`b` 只是小尾巴；③`tail` 项证明「配额不构成 mark 段扫描的界」——大对象续扫仍由 `SCAN_CHUNK` 约束；源码注释 `2629-2630` 逐字：“Step bound model: a resumed segment walks min(EXTC_RC_SCAN_CHUNK, remaining) pointer words, so P_step <= C_scan * EXTC_RC_SCAN_CHUNK + fixed step cost”，与 §19.1 同源，配额不能替代它。
### §9.4 D1（真 bug，P0-P 引入）：`markAdvance` 的 `return` 插在 `wbDebt=0`/`markDone=1` 之前 ⇒ 两行不可达

- **证据**：`reclaim.c:1293` `return (size_t)extc_rc_stepWork;`，紧跟 `1294` `r->wbDebt = 0;`、`1295` `if (r->seedsDone && !r->msp && !r->scanObj) r->markDone = 1;`，然后 `}`。HEAD v5 中这两行是函数尾部（:1036-1037，见 `/tmp/v5chk`）。GCC 默认不启用 `-Wunreachable-code`，所以 `make` 的 0 warning 掩盖了它。
- **后果 ①（停顿/预算失真，影响对比基线）**：alloc 驱动的标记路径是本轮唯一**不经过** `extc_rc_step`/`collect` 的标记入口，它也永不清 `wbDebt` ⇒ 本轮内 `wbDebt` 只增不减。`extc_rc_stepBudget`（`1265-1268`）的 assist 项 `d = min(wbDebt, 64)` 随后恒为 64 ⇒ 每个 `step()` 的预算从 `8+d0` 变成 **72（约 9 倍）**；`markAdvance` 里的 `_debt = min(wbDebt, ASSIST_CAP=512)` 恒 512。
- **后果 ②（死字段）**：`markDone` 全仓**无读者**（`grep -rn markDone src/` 仅 `1295` 写 1、`2412`/`3480` 写 0，加两处注释 290/318）⇒ 今天只是死字段；但注释把它列为「stack empty 即完成」的检查之一，修 D1 时应一并改注释或删字段，否则后人会以为它在生效。
- **后果 ③（计量）**：`extc_rc_stepAssist`（`1114`/`1285`/`1267`）恒 512 ⇒ `[wbudget] ... assist_max=`（`3430`）在 `EXTC_RC_WORK_BUDGET>0` 的对照运行里被污染，不能当证据。
- **附带（预存、非 P0-P 引入，但审计必须记录）**：`markAdvance` 的内部批 `b = (MARK_EVERY·MARK_STEP + debt)·pm` 下限已是 `64·8=512`，而 cap 是 `MARK_BATCH_MAX·pm=256·pm` ⇒ **debt 助推对 `b` 永远无效**（先被 cap 压回）；且 `markStep` 的 `_hot = budget <= MARK_BATCH_MAX`（`2751`）在 `b≥512` 时恒假 ⇒ 所有 alloc-advance 步都被归入 **[ubwork]「无界」桶**，`[work]` 只统计 `step()`（预算 8+d）。这与文档「批量 mark 步是有限热路径」的叙述口径不同，阶段二读量具时必须写明。
- **修法（一行）**：`size_t _w = (size_t)extc_rc_stepWork; r->wbDebt = 0; if (r->seedsDone && !r->msp && !r->scanObj) r->markDone = 1; return _w;`（或把 `return` 移到 `1295` 之后）。
- **验收判据**：①`EXTC_RC_WORK_BUDGET=1` 下 `[wbudget] assist_max` 不再是常数 512；②四形状 `[step]` 预算统计回到 `8+d` 量级；③`tests/reclaim` 两档仍全绿（D1 修法不改变可达性/回收量，只改 pacing 与计量）。
- **严重度 D**（不改可达性/正确性，但使「debt 已还」机制与停顿计量整体失真；且是 P0-P 直接引入，不是历史遗留）。

### §9.5 D2（并发，P0-P 引入）：配额计量用文件级 `static extc_rc_stepWork`

- **证据**：`reclaim.c:1115` `static long long extc_rc_stepWork;`（文件级，不是 `ExtcRcRegion` 字段）；`2925` 在 `markStep` 里写、`1293` 在 `markAdvance` 里读、`1337` 在配额循环里读。
- **问题**：`extc_rc_alloc` 的 region 是每线程各自的（`parTls`/par 路径），但这个计数器全局共享 ⇒ 多线程下是数据竞争（C11 UB）。实践后果是配额抖动（读到别的 region 的大值 ⇒ 本轮少做/多做），不影响可达性，但「结构性前提 M ≤ M_max + buffer」的证明依赖配额计量正确，竞争下它不再是严格保证。
- **修法**：`markStep` 返回本次 work（它已算好 `pops+words+seg`，只需 `return` 而非写全局），`markAdvance` 返回该值，`alloc` 的 `_used` 继续用返回值累计（现实现已经是 `_used += extc_rc_markAdvance(r)` ✓）；全局变量只保留给 `EXTC_RC_PAUSE` 诊断（或改 `_Thread_local`）。
- **严重度 D**（并发语义缺陷；单线程无影响）。

### §9.6 A 类：两处代码注释仍按旧的 `3.4 ns/word` 口径

| 位置 | 现文 | 错在哪 | 建议 |
|---|---|---|---|
| `reclaim.c:989` | `#define EXTC_RC_QUOTA_MAX_WORDS 8192  /* hard cap ... (~50 us at the calibrated 3.4 ns/word) */` | ①`8192×3.4=27.9 µs ≠ 50 µs`；②`3.4 ns/word` 是 **65536 段**的外推值，8192 段实测 **9.5–12 ns/word** ⇒ 真实值 **78–98 µs**（与 §19/A4 的修正口径一致） | 删掉括号里的 `~50 us ...`，改「8192 words ≈ 78–98 µs（8192 段实测 9.5–12 ns/word；3.4 是 65536 段外推）」 |
| `reclaim.c:2629-2630` | 注释仍写 `Empirics reports C_scan ~ 3.4 ns/word on the 200x8 MiB pd shape`（默认 chunk 已是 8192，见 `2640`） | 同上：3.4 只在 65536 段成立 | 保留但注明「3.4 是 65536 段外推；默认 8192 段实测 9.5–12 ns/word ⇒ 78–98 µs/段」，或直接改引 78–98 µs/段 |

> 这两行不改 ⇒ §19 正文（已按 A4 修正）与代码注释自相矛盾，门禁与文档都不能再把注释当口径来源。
### §9.7 B 落地核对：`nfree` 配平表（12 处，全仓穷举）

| 行（快照） | 语句 | 作用 |
|---|---|---|
| `250` | 字段声明 | `uint32_t nfree;`（注释：hole count，洞不一定在链上） |
| `850` | slabNew | `s->nfree = 0;`（同时 `s->freelist = -1`、`bump = 0`） |
| `1352` | alloc 入口满判据 | `if (!s \|\| (s->bump >= s->nslots && s->nfree == 0))` |
| `1357` | class 链跳过循环 | 同形判据（`while (s && bump>=nslots && nfree==0)`） |
| `1374` | freelist 弹出 | `s->nfree--;` |
| `1396` | 重建链 | `s->nfree = (uint32_t)chained;`（按实际链长覆写，Lead 要求） |
| `1400` | 重建后首次弹出 | `s->nfree--;` |
| `1402` | 位图陈旧（链空） | `s->nfree = 0;` 再走 slabNew |
| `1562` | freeSlot（hot 路径） | `s->nfree++;`（同处已 `freeSlotLink` 入链） |
| `1611` | freeSlotWord（sweep） | `s->nfree += (uint32_t)n;`（`n = popcount(bits)`，**不入链**） |
| `3121` | sweepElide bulk reset | `s->nfree = 0;`（同时 `bump=0`、`freelist=-1`、occ/mark memset） |
| `1608-1610` | 注释 | Lead 要求的两条不双计论证已写入源码 |

**静态不变式（逐条核过）**：

1. **不双计**：`freeSlot` 释放的槽 `occ` 已为 0 ⇒ 同轮 `sweep` 的 `dead = occ & ~mark` 不可能再含它；已入链的槽同理不会被重建再链（重建只在 `freelist == -1`、即链空时发生）⇒ Lead 的两条论证**成立**，且已写进注释 `1608-1610`。
2. **末 word 掩码**：`1385-1388` 的 `valid = (w+1==nwords) ? ((nslots - w*64 >= 64) ? ~0 : ((1<<(nslots-w*64))-1)) : ~0`；`h = valid & ~s->occ[w]` ⇒ `~occ` 的越界位被掩掉，`tid[]` 不会越界链出 `nslots` 之外的槽 ✓。
3. **`nfree--` 不下溢**：所有 `--`（`1374`/`1400`）都在 `freelist >= 0` 的分支内，而链非空 ⇔ 链上至少有 1 个洞；重建后 `nfree=chained` 且 `freelist>=0 ⇔ chained>0`；`freeSlot` 同时 `++` 链与计数；`slabNew`/`elide` 同时清零两者 ⇒ 不变式「链长 ≤ nfree，且链非空 ⇒ nfree>0」成立 ⇒ `uint32` 下溢不可达。
4. **满判据三处同形**：`1352`（alloc 入口/allocHot）、`1357`（class 链）、`1402`（重建失败后）全是 `bump>=nslots && nfree==0` ✓ ⇒「bump 满但有洞」的 slab 不会被误跳过，也不会被误判满。
5. **big slab**（`nslots=1`、`big=1`）：`slabNew` 置 `bump=0,nfree=0`；首个槽发放后 `bump=1=nslots, nfree=0` ⇒ 判满；`freeSlot` 对 big 无特殊分支（`1562`/`1570` 与普通槽同路径，`classPush` 走既有链）✓。

**残留（C）**：没有 `-DEXTC_RC_FREE_GUARD` 一类的自检 ⇒ 链环/重复链只能靠 ASan 与压力形状间接发现。建议阶段二在 `/tmp` 私有副本加一次性断言（重建时顺链数长度必须 == `chained`；分配弹出的槽 `occ` 必须为 0），**不改仓库**。

**残留（B，预存风险被 B 放大）**：若计数档 double-dec 导致 `freeSlot` 对同一槽二次调用，会形成链环/重复链 ⇒ `nfree` 漂移（重建时以位图自愈），但链环会让 `freelist` 弹出循环成死循环。改造前（B 之前）同一缺陷表现为「静默的重复入链 + 内存浪费」，B 之后可能变成 **hang**；应记为已知风险并在阶段二用 ASan/超时形状验证。

### §9.8 阶段二仍需跑（P/B 专项，等 GO）

1. **D1 回归**：`EXTC_RC_WORK_BUDGET=1` 下 `[wbudget] assist_max` 不再恒定；四形状 `[step]` 预算回到 `8+d`。
2. **P 停顿实测**：`pd_bigobj` 60/200 轮，采 `[step]`/`[worst]`/`[pause]`/`[histo]`，与 §9.3 闭式对照（判据：实测 max ≤ 闭式 + 20%；并确认 `n>>6` cap 实际生效：512 KiB 与 8 MiB 两点的 sweep 时长接近而非 16 倍差）。
3. **B 压力形状**：同一槽反复 alloc/free 200k 轮，判 slab 数与 maxRSS 平台化；计数档（`EXTC_RC_NOCOUNT=0`）+ `SWEEPFREE=1` 交叉各跑一次（这是原型未覆盖的路径）。
4. **正确性**：`tests/reclaim/run.sh` 默认档 + `EXTC_RC_NOCOUNT=0`（记录 v5 基线，自建 `build-da` 不抄别人数字）；ASan/UBSan 两档；`EXTC_RC_ALLOC_INLINE=0`。
5. **门禁**：G18/G20 按 §4 的判据跑；既有 G8/G9/G12/G13/G14/G4 抽跑。
6. **基线纪律**：树仍在动（15:32→15:38 md5 已变）⇒ 开跑前重取 md5/行数并在本文件记录；所有数字必须能追到 `/tmp` 日志路径。




## §10 落地版复核 r3（快照 15:41:02，md5 `abef7e345e70689859a5d81428265193`，3750 行）

> 权威口径：本节取代 §9.1/§9.2/§9.3/§9.4/§9.5/§9.6 中与 P 有关的内容。§9.7 的 B 表仍成立（r3 下抽查行号未漂移：`1357`/`1374`/`1396`/`1562`/`1611` 一致）。

### §10.1 D1/D2 均已修（复核）

| 项 | r3 证据 | 判定 |
|---|---|---|
| D1（`return` 截断） | `1301` `r->wbDebt = 0;`、`1302` `if (r->seedsDone && !r->msp && !r->scanObj) r->markDone = 1;`、`1303` `return (size_t)(r->markWork - _w0);` ⇒ `return` 已移到两行**之后** | **已修** |
| D2（文件级 `stepWork`） | `grep -n stepWork src/back/reclaim.c` **零命中**；注释 `291` 已写「the byte quota reads a per-region delta, not a file-level static」；`markAdvance` 自算 delta 返回（`1303`），alloc 用返回值累计（`1347`） | **已修** |
| D1 余项（`markDone`） | 仍**无读者**（全仓仅 `1302` 写 1、`3487`/`2421` 写 0）⇒ 死字段 | C（建议删字段或注明预留） |
| §9.6 两处 3.4 ns/word 注释 | `995` 已改「8192 words is 78-98 us at the measured 9.5-12 ns/word」；`2633-2635` 已改「the 3.4 ns/word figure is a 65536-word-slice extrapolation … must not be rescaled」 | **已修** |

### §10.2 P 语义在 r3 已变：两处配额的定义与单位

| 通道 | r2（§9 口径） | r3 实际 | 证据 |
|---|---|---|---|
| sweep | `W_s = 8·pm + min(n>>6, 8192)` words，按**请求字节** | `W_s = 8·pm + max(1, min(ceil(swSlab->nslots/64), 8192))` words，按**游标 slab 的槽数**；`_b += _q; sweepStep(r,_b)` | `1329-1336`：`_q = _cs ? ((uint32_t)(_cs->nslots + 63u) >> 6) : 1u`；cap `1335` |
| mark | `min(n>>10, 8192)` units | `min(n>>3, 1<<20)` units；单位 = popped slot **或** 被扫的 word（含续扫 segment 的 pointer word） | `992`（`QUOTA_SHIFT_MARK 3`）、`998`（`QUOTA_MAX_MARK (1 << 20)`）、`1343-1345` |
| 常量表 | `QUOTA_SHIFT_SWEEP 6` 存在 | **已删除**；只剩 `QUOTA_SHIFT_MARK`/`QUOTA_MAX_WORDS`/`QUOTA_MAX_MARK`（均 `#ifndef`） | `992-1000` |
| 注释 | `977-981` 三句（caps=停顿界 / unit=请求字节 / 结构前提） | `976-990` 已重写，含新闭式与 `QUOTA_MAX_MARK * ~17.5 ns ~= 18 ms` 的 **UNTESTED risk 自述** | `976-990` |

### §10.3 重新核算 P_step（r3 默认值）

成本常量取 `w_1 = 64·τ_s ≈ 29.31 ns/word`（τ_s=0.458 ns/slot，QUOTA-MODEL §2）、`c_u ∈ [9.5–12 ns/word（8192 段实测）, 17.49 ns/popped slot]`。

**通道 A（sweep，单位 word）**：`W_s = 8·pm + min(ceil(nslots/64), 8192)`。
合法 slab 上限：`EXTC_RC_SLAB_SIZE = 2^20 B`（`326`）、最小 slot stride 8 B ⇒ `nslots ≤ 131072` ⇒ `ceil(nslots/64) ≤ 2048` ⇒ **`QUOTA_MAX_WORDS=8192` 永远不可能生效**（要触到它需 nslots ≥ 524288，即 ss<8，不存在）。故 `W_s ≤ 32+2048 = 2080 words ≈ 61 µs`；典型 64 KiB slab / 16 B 槽 ⇒ 64 words ≈ 1.9 µs。

**通道 B（mark，单位 [work]）**：`W_m = min(n>>3, 2^20)`。
**同一次分配内两通道相加**（alloc 先走 `1329-1336` 的 sweep 分支，再走 `1341-1348` 的 marking 分支），故

```
P_step(n) ≤ w_1·W_s + c_u·W_m + c_a
```

其中 `c_a` 覆盖「配额只在 advance 返回后比较」造成的**一个 advance 超调**（≤ b·c_u + 一段）；注释 `988` 写的 `fixed + max(W_s words, W_m units)` 把两通道取大、且混用 words/units，是**下界**而不是上界。

**dense 指针数组（n 字节 payload ⇒ n/8 个指针 word；每个续扫 word 记 1 unit，见 `2935` 的 `pops + words + seg`）**：`W_m = min(n/8, 2^20)`，而扫完整个对象需要 n/8 units ⇒ **n ≤ 8 MiB 时一次分配就把整个大对象扫完**；段数 = `ceil(n/65536)`。

| 形状 | `W_s` | `W_m` | 段数 | P_step（按 9.5–12 ns/word） |
|---|---|---|---|---|
| 16 B 小对象 | 8–32 words | 2 | 0 | ≤ 1 µs |
| 512 KiB 非密集（描述符扫描/极少 ref 字段） | ≤ 64 words | 小 | 0 | 与锚点 78–98 µs 同量级 |
| **512 KiB dense** | ≤ 64 | 65,536 | 8 | **0.62–0.78 ms** |
| **8 MiB dense** | ≤ 2,048 | 1,048,576（恰好顶 cap） | 128 | **10.0–12.5 ms**（按 17.5 ns/unit = 18 ms，与 `989` 自述一致） |
| 8 MiB dense，pause-first `-DCAP=8192` | ≤ 2,048 | 8,192 | 1 | **78–98 µs**（峰值 34,328 kB，`999` 自述） |
| 64 MiB dense | ≤ 2,048 | 2^20（截断） | 128/次分配 | 10–12.5 ms/步，8 步扫完 |

**结论 D3（当前最严重的未闭合项）**：默认 `QUOTA_MAX_MARK = 1<<20` 是 **memory-first** 取向（注释 `998` 明说「一个 8 MiB 指针密集大对象在一次分配里扫完」），代价是该次分配的单步停顿 **10–12.5 ms**（实测外推；理论 18 ms）。它同时破 5 µs 门禁（2000×）与 G18 软约束 1000 µs（10×）。`989` 已诚实标注 UNTESTED，但**未测不等于不存在**：这是契约项，必须进 §19 的停顿通道表，并由新门禁（dense 形状）覆盖；否则「停顿有界」不成立。
两个可选契约：(a) **pause-first**：`CAP=8192` ⇒ 单步 ≤ 一段 78–98 µs，代价峰值 34,328 kB；(b) **memory-first**：接受 ~10–18 ms 通道，峰值 26,264 kB。选哪个是产品决定，但默认值 1<<20 与文档「停顿有界」目前不一致。
**锚点缺形状（A）**：`987` 的 512 KiB 78–98 µs / 8 MiB 0.22–0.26 ms / 带续扫 0.35–0.5 ms **均未标对象形状**；这些值只可能来自非指针密集形状，同请求的 dense 形状差 8–25× ⇒ 必须标形状，且 `pd_bigobj` 不能当作 dense 通道的证据。

> **§10.8 修正**：本节 "nslots ≤ 131072 ⇒ ≤2048 words" 的上界偏松。`extc_rc_slotSize` 下限是 16 B（`697-701`），故合法 slab 的 `nslots < 4096`、配额增量 `≤64 words`；`QUOTA_MAX_WORDS=8192` 永不生效的结论更强。dense 通道（`c_u·W_m`）数值不变。D3 的选择已由 Lead m02954 定为 memory-first + deadline，见 §10.7。

### §10.4 r3 下的 A 类（注释/公式）

| 位置 | 现文 | 问题 | 建议 |
|---|---|---|---|
| `983` | 「1 unit = 1 popped slot or 1 scanned bitmap word」 | 漏了续扫 pointer word：`2935` 记的是 `pops + words + seg`，seg 每 word 记 1 unit | 改为「popped slot / seed bitmap word / 续扫 segment 的 pointer word，各 1 unit」 |
| `988` | `P_step <= fixed + max(W_s words, W_m units)` | ①混单位不能取 max；②两通道在**同一次分配**内相加（`1329`→`1341`）；③未含配额超调 | 改成 `P_step ≤ w_1·W_s + c_u·W_m + c_a`，并写明 c_u 两端 |
| `995` | `QUOTA_MAX_WORDS 8192 /* words needed to finish the slab */` | 合法 slab 至多 2048 words ⇒ cap **永不生效**（死参数） | 注明「纯防御」或降到 2048 并写出推导 |
| `987` | 三个实测锚点未标形状 | dense 形状差 8–25× | 每个数字标形状 |

### §10.5 r3 状态汇总（与 §9 的差异）

| 项 | 状态 |
|---|---|
| P1 位置 / P6 不拒绝分配 / P7 pressure 复合 / P8 finish 在循环外 / P9 skip 卫生 / P10 `why` 归因 | **仍成立**（r3 行号 `1329-1336`、`1341-1355`） |
| D1 `markAdvance` 截断 | **已修**（`1301-1303`） |
| D2 文件级 `stepWork` | **已修**（零命中，改 per-region delta） |
| §9.6 两处 3.4 ns/word 注释 | **已修**（`995`、`2633-2635`） |
| §9.7 B 配平表/不变式 | **仍成立**（行号未漂） |
| §9.3 的 P_step 数值 | **被 §10.3 取代**（配额语义已变） |
| 新增 D3 / A×4 | D3 由 Lead m02954 裁决为 **memory-first + deadline 250 µs**（§10.7）；A×4 已由 Lead 确认交 runtime-facts 修（§10.4） |
| deadline 实现 | **尚未落地**（15:42:57 树 `grep DEADLINE|deadline` 零命中）；DL1–DL8 审计清单见 §10.9 |

### §10.6 阶段二必须补（在 §9.8 基础上）

1. **dense 指针数组停顿通道**：512 KiB 与 8 MiB 两点，形状必须「分配后存活、mark 轮的保守分段扫描会走到」（如 `new [N]ref T` 或元素为 ref 的数组），采 `[step]`/`[worst] seg=`/`[pause]`；先按 §10.3 的 0.62–0.78 ms / 10–12.5 ms 判，再按契约收口。
2. **CAP 对照（Lead m02709 第 1 点的三列表）**：`-DEXTC_RC_QUOTA_MAX_MARK=8192` 重建一次，确认单步回落到 ≤ 一段（78–98 µs）、峰值升到 ~34 MB ⇒ 填「CAP vs 峰值 vs pd 停顿」。
3. **复核 `w_1`**：§10.3 沿用 QUOTA-MODEL §2 的 29.31 ns/word；若 `[work] sw_words_max` 与实测不符，按实测替换后再算通道 A。
4. 基线纪律：r3 树仍在动（15:41:02），阶段二开跑前**重取 md5/行数**并记入本文件。

### §10.7 Lead m02954 裁决（memory-first + 时间片兜底）与阶段二判据

**契约**：保留 `QUOTA_MAX_MARK = 1<<20`（`998`，memory-first：一次分配扫完 8 MiB dense 大对象）+ 新增 `EXTC_RC_QUOTA_DEADLINE_US` 默认 **250 µs**；mark 配额循环每**粗粒度**检查一次耗时，超时 break 并计数 `[quota] deadline_hits`。理由：`1<<20` 的 dense 通道实测外推 10–18 ms（§10.3），而 `CAP=8192` 的峰值 34,328 kB（`999` 自述）会让 G18 主判据红；用户明确内存优先，故用 deadline 把时间封顶在 `~250 µs + 一个 advance`，而不是缩小 CAP。

阶段二 P_step 通道表（Lead 指定，我的判据）：

| 通道 | 形状 | 判据 | 备注 |
|---|---|---|---|
| ① 常规 | n ≤ 64 KiB（`BIG_THRESHOLD=64<<10`） | `[step]` max ≤ 5 µs | 沿用既有 5 µs 门禁；n>64 KiB 的插桩未分桶，① 与②混在 `(64k,...]` 行 ⇒ 该行只能作混合通道可判定界（g18.py:192 起、:321） |
| ② 大分配稀疏 | `pd_bigobj`（8 MiB，非指针密集） | 实测 ~107 µs 量级（Lead 给定） | 记录即可，不作硬门禁 |
| ③ 大分配 dense | `pd_pointer_dense`（8 MiB、1,048,576 个非空指针 word） | `[step]` max ≤ **deadline + one seg**（250 µs + ≤98 µs ⇒ 期望 ≤ ~350 µs），且 `deadline_hits > 0` 证明兜底真的生效 | 若 hits == 0 且 max 仍 ~10 ms ⇒ deadline 未落地/检查点过粗，按 A 报；若 hits > 0 但 max > deadline+one seg+检查余量 ⇒ 超调未计入 |

**闭式必须写成相加形式**（§10.4 已确认的 A 类，runtime-facts 改）：

```
P_step(n) ≤ w_1·W_s + c_u·W_m + c_a + c_d
```

其中 `W_s = 8·pm + max(1, min(ceil(cursor_slab->nslots/64), QUOTA_MAX_WORDS))`、`W_m = min(n>>3, QUOTA_MAX_MARK)`；deadline 生效后 `c_u·W_m` 被 `250 µs` 截断，故 dense 通道实际是 `c_u·W_m → min(c_u·W_m, deadline + 一个 advance)`；`c_d` 是粗粒度检查点间隔内未检查的那部分工作量（≤ 一个 advance），`c_a` 是 advance 本身的超调。

**G18 附加要求**（主判据不变：peak200 ≤ 3L+2·SLAB 且 peak200 ≤ peak60×1.05）：

1. 报告必须同时给 `deadline_hits` 与 `[step]` max；并说明「内存界成立的前提是 hits **不持续增长**」——即 hits 是 per-round/每轮清零的一次性事件，而不是随轮数累积、把每轮停顿都推到 250 µs（后者会让 G18/G20 的吞吐与停顿口径同时失真）。
2. 若 hits 是累计计数器，需给出 `hits/round` 趋势（200 轮内应趋平）；若 200 轮后仍线性增长，说明每轮都在扫同一个 dense 对象 ⇒ 标记没有推进/对象不可达却未被清，按 D 报。
3. 「CAP vs 峰值 vs pd 停顿」三列表由 runtime-facts 跑并出表，我阶段二独立复核**至少 2 个数**（建议：peak200 与 [step] max）。

**我阶段二的静态检查项**（不依赖跑程序）：`grep -n "QUOTA_DEADLINE" src/back/reclaim.c` 应有 `#ifndef` 定义 + goal 循环内检查 + `[quota]` 输出三处；核对注释与实现是否一致——尤其 §10.8 证明的「`QUOTA_MAX_WORDS` 永不生效」是否仍被注释当作真实上界。

### §10.8 `QUOTA_MAX_WORDS` 永不生效：精确核算（修正 §10.3 的 nslots 上界）

§10.3 用「最小 slot stride 8 B ⇒ nslots ≤ 131072 ⇒ ≤2048 words」估算，偏松；`extc_rc_slotSize` 的真实下限是 **16 B**：

- `reclaim.c:697-701`：`size_t s = (n + 7) & ~(size_t)7; ... return s > 16 ? s : 16;` ⇒ 任何槽的 stride `ss ≥ 16`。
- `slabNew`：`chunk = EXTC_RC_SLAB_CHUNK_MIN = 64 KiB`（`331`）；`while (chunk < EXTC_RC_SLAB_SIZE && chunk / slotSize < EXTC_RC_SLAB_SLOTS_MIN) chunk <<= 1;`（`793-794`，`SLOTS_MIN = 1024`，`332`）；`cap = (chunk - EXTC_RC_SLAB_HDR) / slotSize`（`795`）；`HDR = (sizeof(ExtcRcSlab)+63) & ~63`（`327`）。
- `ss = 16` 时 `64 KiB / 16 = 4096 ≥ 1024` ⇒ **chunk 不倍增**，`nslots = cap = (65536 − HDR)/16 < 4096` ⇒ `ceil(nslots/64) ≤ 64` words。
- `ss > 64` 时按 1024 槽规则倍增，但 `cap ≈ chunk/ss ≤ 2048`（`ss ≈ 1 MiB/1024` 后 ≤ 1024）；huge block（`big=1`，`cap<2`）时 `nslots = 1`。
- 故 sweep 配额增量 `≤ 64 words`，`QUOTA_MAX_WORDS = 8192`（`996`）**要 128 倍才触到，永远不生效**；`996` 注释的「8192 words is 78–98 µs」是**不可达上界**，真实上界是 64 words ≈ 1.9 µs（按 29.31 ns/word）。实测 `[work]` 里 `sw_words_max` 阶段二应复核是否 ≤ 64 量级。
- 连带影响：`s->nslots > EXTC_RC_REUSE_MAX_SLOT (0xFFFE)` 的 oops（`801-818`）同样**永不触发**（nslots < 4096，big 时 1），是纯防御/死代码；`§10.3` 表与 §9.5 里「W_s ≤ 2048 words ≈ 61 µs」应统一改为「`W_s ≤ 8·pm + 64`；因 `pm ≤ 16`（`EXTC_RC_SWEEP_PRESSURE_MUL` 上限）⇒ **≤192 words ≈ 5.6 µs**，默认 `pm=4` 时 ≤96 words ≈ 2.8 µs（§10.11 G-2 同口径）」。dense 通道的数值主体（`c_u·W_m`）不受影响。

### §10.9 deadline 实现审计清单（清单；逐条核对结果见 §10.10）

| 编号 | 检查点 | 失败判定 |
|---|---|---|
| DL1 | 定义：`#ifndef EXTC_RC_QUOTA_DEADLINE_US` + 默认 250，且 0 有明确语义（关闭 vs 立即超时） | 硬编码、不可覆盖、0 语义含糊 ⇒ A |
| DL2 | 时钟源：wall 还是 cpu？是否复用 `chanClock` 的 `STEP_CPU` 分支（`1056-1059`、`3198-3218`）；`[step]`/`[quota]` 的时钟口径必须标注 | 用 wall 而报告按 cpu 解释（或反之）⇒ A；未标注 ⇒ C |
| DL3 | 检查点粒度：必须**每 advance 之后**或每 K 次 advance 检查（K 粗粒度），且检查本身的 clock 调用成本计入 `c_a` | 每个 mark unit 都 check ⇒ 吞吐回归（B/A）；从不 check ⇒ D |
| DL4 | break 语义：超时 break 后仍要执行 finish（`finish` 在 while 外，`1349` 附近），不得 `return` 截断（D1 回归）；`msp`/`scanObj` 状态保留给后续分配 | 截断（丢 wbDebt/丢 finish）⇒ D1 回归，A |
| DL5 | 计数：`[quota]` 行含 `deadline_hits`；hits 是累计还是 per-region/每轮；hits 不得改变 `markAdvance` 的 work 返回值；summarize.py/g18.py 的解析与该行格式一致 | 计数缺失/重复计数/解析错位 ⇒ C |
| DL6 | 叠加界：实测 `[step]` max 与同轮 `deadline_hits` 同给；dense 期望 ≤ 250 µs + 一个 advance（≈98 µs）+ 检查余量；若 max ≈ 10 ms 而 hits>0 ⇒ 超调未计入 | 超调 > 一个 advance ⇒ A |
| DL7 | 内存界前提：hits 若累计，G18 需给 hits/round 趋势与「不持续增长」证据；200 轮后仍线性增长 ⇒ 每轮重扫同一对象，标记可能未推进 ⇒ D | 无趋势证据 ⇒ C |
| DL8 | 吞吐：deadline 只封顶时间、不减少总工作量；慢机上 8 MiB dense 需 ~128 个带 deadline 的分配步 ⇒ G20 的总时长/吞吐门禁是否回归，阶段二须实测 | 未测 ⇒ C |

> 口径提醒（阶段二实测时）：③ 的 `[step]` max 若采 `EXTC_RC_STEP_CPU=1`，报告须同一口径给 cpu 与 wall（G18 已用 cpu 口径跑 `run_pause`）；deadline 若用 wall 时钟而 step 用 cpu 记录，则「max ≤ deadline + seg」必须换算到同一时钟再判。

### §10.10 deadline 首次落地的逐条静态审计（快照 15:52:59，md5 `f7f7011e…`，3794 行；**已被 §10.12 复audit 取代，保留作历史**）

> 口径：本节只读源码，不跑程序。快照取 mtime 15:52:59（`reclaim.c` md5 `f7f7011e57ff9c9e31935d9e3dcf1491`，3794 行；`build-rf/extc` md5 `bffd9c9fafc2ff47e82150b0f27f7d5d`）。**树在审计期间变了三版**，核对结果只对该快照成立；GO 前若再变，须按同一清单重核。
>
> 移动靶记录（同一文件三次快照）：①15:48:48 `ad32f8ab…`/3788 行，首版落地，默认 **250**；②15:50:54 `f2aa4b27…`/3798 行，出现 `r->markCarry`（超时未花的配额跨分配结转、上限 8×MAX_MARK、新轮清零），但同刻读到的循环里 `markCarry` 赋值与 1382-1383 的**重复 deadline 检查**并存；③15:52:59 `f7f7011e…`/3794 行，`markCarry` 整段被移除（结构体字段、分配循环、markBegin 清零全部消失），默认改成 **800**，重复检查仍在。

**三个直接观察（先报给 Lead）**

1. **默认值与裁决不一致**：Lead m02954 定的是 `EXTC_RC_QUOTA_DEADLINE_US` 默认 **250**；当前源码默认 **800**（`reclaim.c:1021-1023`，注释 1017-1020 给的理由是 `pd_bigobj` 200 轮实测：250 会 hit 99-143 次且峰值 34,524 kB，800 只 hit 0-1 次且峰值 26,460 kB）。阶段二判据必须按 800 重写，否则「max ≤ deadline + one seg」的分母全错；250 只作为 `-D` 的 pause-first 变体。
2. **`markCarry`（信用结转）已被回退**：m02954 没要求它，属 runtime-facts 实验痕迹；当前树没有「超时未花配额跨分配保留」，超时即丢信用。若 800 的实测理由依赖「截断 delay sweep」而非结转，则回退无害；但 §19 若写「信用不丢弃」需删。
3. **1382-1383 残留重复检查**（B，编辑残留）：`if (pauseNow()-_dl0 >= D) {`（1382）体内只有一行同样的 `if (pauseNow()-_dl0 >= D) { hits++; break; }`（1383）。语义与单层等价（超时时多打一次 `clock_gettime`），但显然是改 250→800 时留下的半行。建议删内层、保留外层块，或反之。

**DL1–DL8 逐条判定**

| 编号 | 判定 | 证据与说明 |
|---|---|---|
| DL1 定义/默认/0=off | **✓** | `1017-1023`：`#ifndef EXTC_RC_QUOTA_DEADLINE_US` / `#define … 800`；`#if EXTC_RC_QUOTA_DEADLINE_US > 0` 两处（`1368`、`1376`）⇒ 0 时整段编译掉，可 `-D` 覆盖。⚠️ 默认值与 m02954 的 250 不符（见上）。 |
| DL2 时钟源/口径 | **C** | deadline 固定用 `extc_rc_pauseNow()`（`3387-3396`：`CLOCK_MONOTONIC` 纳秒；无该宏时退回 `clock()*1e9/CLOCKS_PER_SEC`，即 CPU 微秒）。它**不随** `STEP_CPU` 切换，而 `[step]` 走 `chanClock`（`1183-1184`：`STEP_CPU` 时 cpu）⇒ 报告必须写「deadline 是 **wall** 上界；cpu 口径的 `[step]` max 只会 ≤ 它」，判 `max ≤ deadline+seg` 前先换算同一时钟（抢占多时 wall − cpu 是纯外部时间）。 |
| DL3 检查点粒度 | **A** | 检查按 **work unit** 计（`1379` `_dlTick >= 8192`），而 unit 只含 pops+seed words+seg（`2977` `markWork += pops + words + seg`）。**描述符字段扫描不计、也不分段**：`2759` `for (; _op < _oend; ++_op)` 一次 pop 可扫 `t->nptr` 个指针字；`nptr` 最多 4096（`codegen.c:3872-3873`：`offs.len > 4096` 才转 conservative）。一个 advance 的 pop 预算上限 = `MARK_BATCH_MAX×pm = 256×16 = 4096` ⇒ 单次 advance 最多可含 ~1.7×10⁷ 次字段扫描，而 `_dlTick` 只 +≤4096 ⇒ 检查点可滞后几十~几百 ms，**deadline 在这种形状上不成立**。建议：(a) 描述符字段每 8 个记 1 unit（与 seed 扫描同权）；(b) `quota≥4096` 时每次 advance 后直接查时钟（大配额是冷路径，clock_gettime≈25 ns）；(c) §19 闭式加 `c_f·F_max·b`（`F_max=4096`）并注「该形状未实测」；(d) 或 `nptr>64` 的对象也走 conservative 分段。注意此形状**不会**让 `hits` 增长（unit 不涨）⇒ G18 的 hits 趋势也看不见它（DL7）。 |
| DL4 break 语义 | **✓** | `break` 只在 while 内（`1383`）；break 后仍执行循环外的 finish guard（`1398` `if (seedsDone && msp==0 && !scanObj) finish`）⇒ 无 D1 式截断。断点在 advance 边界，markStep 内部已把 batch 中未扫项回推（`2870-2880`）⇒ 不丢可达对象。 |
| DL5 计数/可审计性 | **C** | `hits/checks` 是进程级 `static long long`（`928`），且只在二进制退出/诊断 dump 打印：`2033` `[quota] deadline_us=%d hits=%lld`（STATS 路径）、`3452` `[quota] deadline_us=%d max_mark=%d shift=%d checks=%lld hits=%lld`（PAUSE 路径）。**单次终值判不了「hits 不持续增长」**：G18 需按轮采样 Δhits（例如 run.sh 每 N 轮读一次 stderr，或 runtime-facts 加 `EXTC_RC_QUOTA_TRACE` 每次 hit 打印 step_n）⇒ 阶段二 auditability 项。 |
| DL6 叠加界 | **A** | 首次 advance 在时钟起点**之前**：`1367` `_used = extc_rc_markAdvance(r);` 先执行，`1370` 才取 `_dl0` ⇒ 这一次 advance 的耗时既不受 deadline 约束、也不进 `_dlTick`。conservative dense 形状上界因此是 **`D + 2×seg`**（800 + 2×(78–98) ≈ **0.96–1.0 ms**），而注释 `1014`「deadline + one segment」与 `1019`「~0.9 ms」都少算一段。建议把 `_dl0` 移到首 advance 之前，并把首 advance 的 `_used` 折进 `_dlTick`。 |
| DL7 内存界前提 | **C/A** | 「hits 不增长 ⇒ 内存界成立」有两个缺口：(a) 无 per-round 视图（见 DL5）；(b) DL3 的描述符形状可**既不触发 hits 也超 deadline**，而 G18 只看 hits/peak 会把它当绿。阶段二应把「单步 wall 时间」直接纳入判据（PAUSE 的 `[top]`/`[histo]`），并注明该推理只覆盖「分段可探的」形状。 |
| DL8 吞吐 | **C** | 未测（等 after 数据）。静态上：小配额（`_quota<4096`，即 n<32 KiB）不取时钟（`1370` 条件）⇒ 热路径无时钟成本；大配额每 8192 units 一次 `clock_gettime`（vDSO ≈25 ns），相对 8192 units 的 78–143 µs 可忽略；deadline 不减少总工作量，只把一次分配摊成多次（下一分配继续），G20 的 promote `[step]`/总时长须 after 实测。 |

**pd_pointer_dense 的静态定性（判据三行里的最有利形状，不能外推）**

生成 C 证据：`tests/gc-p0/out/p0-before/c/pd_pointer_dense.c:3826` —
`dense * b = ((dense *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(dense), ((const ExtcRcType *)-1)));`
即 dense 的 descriptor 是 **conservative**。原因：`struct dense { a: [1048576]?mut ref leaf }` 有 1,048,576 个 ref 字段，超过 4096 上限（`codegen.c:3872-3873`）⇒ 走 `2734-2737` 的 conservative 分支，`words > SCAN_CHUNK` 时记 cursor 并 `_split=1`，由 `markResume`（`2789-2794`）按段走 ⇒ 每个 advance 产生 ≈8192 units（seg）⇒ 检查点每 advance 生效。**结论**：dense 的 `max ≤ D + 2×seg` 成立（DL6 修正后），但「任意大对象都 ≤ D+seg」不成立——`≤4096` 个 ref 字段的描述符对象反而不分段（DL3）。阶段二若只跑 pd_pointer_dense 会高估 deadline 的普适性，建议补一个 `[4000]?mut ref` 的 descriptor-dense 形状做观察（只报观测）。

### §10.11 G18 门禁脚本侧的静态审计（量具可审计性；只读，未跑）

> 对象：`tests/gc-p0/g18.py`（15:41，26 KB）、`tests/gc-p0/summarize.py`（15:42）、`tests/gc-p0/pause_parse.py`（`[quota]` 解析 :153-164）。判据公式侧只读，不改。

| 编号 | 判定 | 证据 | 说明 / 建议 |
|---|---|---|---|
| G-1 deadline 常数过期 | **A** | `g18.py:58` `QUOTA_DEADLINE_US = 250`；`:92` `ceiling = QUOTA_DEADLINE_US + batch_us` ⇒ 天花板 **393.4 µs** | 源码默认已改 **800**（`reclaim.c:1021-1023`）⇒ 真实天花板 ≈ `800+2×143.4 ≈ 1087 µs`。当前常量会让 `ch2_verdict` 把 512 µs 桶的 p99.9 判 RED（假红）。**修法**：从 `[quota] deadline_us=`（parser 已抽取，`pause_parse.py:153-164`）读实际值，并改成 `deadline + 2×batch_us`（DL6：首个 advance 在时钟起点之前）。 |
| G-2 W_s 用永不生效的 cap | **A** | `g18.py:81-86`：未观测 nslots 时 `q=QUOTA_MAX_WORDS=8192` ⇒ `W_s=8*16+8192=8320` words ⇒ `t_sweep≈79–100 µs` | 由 §10.8，`q=ceil(nslots/64)≤64`（slotSize 下限 16 B ⇒ nslots<4096），故实际 `W_s ≤ 8·16+64 = 192` words（≈1.8–2.3 µs）。闭式把清扫项高估 ~43×；docstring 已声明「未观测时不能当实际形状」，但 `summarize.py:488-494` 仍把它印成「标称闭式」⇒ 建议在输出里显式标 `W_s_cap_unreachable`，或直接用 192 作为上界（仍是上界，更紧）。 |
| G-3 c_a 是拟合残差 | **C** | `g18.py:53` `C_A_NS = (0.0, 18000.0)`，注释「使闭式落在 0.22~0.26 ms」 | 「为落在目标区间而调」的残差不可伪证；建议置 0 并允许闭式上界变大（0.22→0.24 ms 区间），或把 c_a 拆成具名项（首 advance 未计时 ≈ 1 段、描述符字段 `c_f·F_max·b`，见 DL3）。审计时若闭式仅靠 c_a 吻合，按 C 记。 |
| G-4 before/after 闭式不可比 | **C** | `out/p0-before/g18.json:23` `W_m=1048576 units, b=4096 pops`（旧 `n>>3` 语义，无 deadline）；after 用 `W_m=8192`（`g18.py:57` docstring 引 m03564） | 同一 `pstep_closed_form` 字段在两 tag 下语义不同 ⇒ G20/P0 报告只能比 RSS/stdout/`step_max`，**不能**比闭式。建议 json 里加 `model_version`。 |
| G-5 `[pstep]` 无发射器 | **C** | 全仓 `grep -rn "pstep" src/` = **0**；`g18.py:132-133` `_pstep_lines` 只收 `startswith("[pstep]")` | 所有 `pstep_lines` 必为空（`out/p0-before/g18.json:125/228/345` 已证实 `[]`）。后果：无法按「sweep vs mark vs 首 advance」拆分单步耗时 ⇒ DL3/DL6 的实测归因缺通道。建议加一行 `[pstep]` 或在 `[top]` 带 `w_s/w_m` 字段。 |
| G-6 26 MiB 与 3L+2SLAB 的一致性 | **✓** | `g18.py:45-46` `EMP_BOUND_MIB=26.0`「= 3L + 2 MiB（L=8 MiB）」；`summarize.py:409-412` `bound_kb = 3*live_kb + 2*slab_kb`（slab=1024 kB）；`G18_LIVE_MIB` 兜底 8 | 我此前记的「26 MiB vs 3L+8=32 MiB」是笔误：正确式是 **3·live + 2·SLAB**，L=8 MiB ⇒ 3×8+2 = 26 MiB，两个数字同源、无矛盾。live 从 `[work] M_max/3` 反读（`g18.py:262-270`），读不到才用 8 MiB 兜底并标来源。 |
| G-7 ①通道 5 µs 判据不可判区 | **C** | `g18.py:210-218` `ch1_verdict`：`gt8>0 ⇒ RED`；`gt4==0 ⇒ PASS`；否则 N/A | 4–8 µs 桶非空时永远 N/A ⇒ 「≤5 µs」在 histo 上不可证（只能证 ≤4 或 >8）。阶段二须用 n 分桶插桩（或接受 N/A 并说明边界），不能把 N/A 记 PASS。 |
| G-8 deadline_hits 已接入 | **✓** | `pause_parse.py:153-164` 抽 `[quota]` 的 `deadline_us/hits/checks/max_mark`；`g18.py:272-273/308-309/375/420-422` 打印 `deadline_hits` 与 `step_max_us` | Lead m02954 要求的「报告同时给 hits 与 [step] max」结构上已满足；但仍是进程级终值（DL5），判「不持续增长」需按轮采样。 |

**对阶段二判据的净影响**：G18 主判据（RSS/peak 与 stdout）不受 G-1..G-8 影响；受影响的是「p99.9 ≤ deadline 天花板」与「闭式对照」两个观察通道 —— 按 G-1 修常数、按 G-2 换紧上界后重跑，否则 800 默认下的 PASS/RED 不可解释。

### §10.12 快照 15:56:15 复audit（DL3/DL6/DL5 修正已落地；新发现 1 A + 1 B + 3 C；**N-1..N-4 已被 §10.13 收口**）

> 口径：`src/back/reclaim.c` md5 `ed34e2a496ece97e528ef88a43eafd48`、**3800 行**、`build-rf/extc` 15:56:15。§10.10/§10.11 的判定针对 f7f7011e…（3794 行）快照，以下按新快照逐条重判。

**我的 A 类已修（逐条验证）**

- **DL3 ✓ 已修**：配额循环改为**每次 advance 之后都读时钟**（`1376-1386`：`_used += markAdvance(r);` 后紧跟 `if (_dl0 >= 0) { checks++; if (pauseNow()-_dl0 >= DEADLINE*1000) { hits++; break; } }`），不再按 8192 work unit 打点；注释 `1367-1372` 逐字承认「a descriptor field scan (one pop can walk up to nptr fields) does not show up in the work units, so a work-unit tick would miss it」——与本审计独立结论一致。`_dl0` 移到循环之前（`1374`），首 advance 的耗时计入 elapsed。
- **DL6 ✓ 已修（措辞）**：注释 `1369-1372` 现写「Bound: **D + 2 segments**（the first advance and the in-flight advance that trips the deadline）」；`1394-1400` 的 while 结构与之相符（断点只在 advance 边界）。⚠️ 但同一注释块 `1019` 的默认值理由仍写「One step is therefore bounded at 800 us + **one** segment (~0.9 ms)」⇒ 与 D+2 segments（≈0.96–1.0 ms）不一致，建议统一成 D+2（仍 <1 ms，结论不变）。
- **DL5 部分修**：新增 per-round 计数 `extc_rc_quotaHitsRound/quotaChecksRound`（`928`）；`sweepBegin` 在 `extc_rc_pauseOn>0` 时打印并清零（`3195-3203`，注释「G18 判据是 hits do not keep growing，process total 看不出来」）；终值行 `2034` 加 `hits_round`、诊断行 `3458` 加 `hits_round`。
- **重复检查已删**（上快照 1382-1383）；单检查在 quota 判据之前；`markCarry` 仍未出现。

**新发现**

| 编号 | 判定 | 证据 | 说明 / 建议 |
|---|---|---|---|
| N-1 `QUOTA_MAX_MARK` 是死上限 | **A** | 定义 `1008`；打印 `3458`；注释 `984/987/1008` 声称 `quota = min(request_bytes >> SHIFT_MARK, QUOTA_MAX_MARK)`；但实际 `1362` 只有 `_quota = (size_t)(n >> EXTC_RC_QUOTA_SHIFT_MARK);`，**全文件无钳位** | `SHIFT_MARK=3` ⇒ 对 8 MiB 恰好 1<<20（等于 cap）；对 **>8 MiB** 的指针密集分配 quota 线性增长，只被 800 µs deadline 拦住 ⇒ 「memory-first 允许一次扫完 8 MiB」的描述只在 n≤8 MiB 成立。deadline 仍在，所以停顿有界；但注释与闭式（`W_m=min(n>>3,1<<20)`）在 n>8 MiB 时不成立。**修法**：`_quota = min(_quota, QUOTA_MAX_MARK)`，或把注释/闭式改为「cap 只对 n≤8 MiB 生效/无 cap，靠 deadline」。 |
| N-2 `[quota]` 解析键位冲突 | **C** | `pause_parse.py:153-164` 对每条 `[quota]` 行按 `k=v` 覆盖写入同一 dict；行序：`deadline_us=.. hits=<total> hits_round=<r>`（`2034`/`3458`）之后 `round hits=<r2> checks=<c>`（`3201`）⇒ `quota["hits"]` 最终是**末轮** hits，total 丢失 | `g18.py:309` 把 `(pr.quota).hits` 当 `deadline_hits` 打印，报告若引用会与「total」混淆。**修法**：round 行改成不冲突的键（如 `round_hits=`），或 parser 对 round 行不覆盖 `hits`。 |
| N-3 `QUOTA_MAX_WORDS` 注释仍按 131072 槽估界 | **C** | `1005`「For a legal slab (nslots <= 131072) this cap never binds: ceil(nslots/64) <= 2048」 | 与 §10.8 一致方向（cap 不生效），但 nslots 的真实上界是 <4096（slotSize≥16）⇒ 可把注释收紧到 ≤64 words；cap 保持为防御即可。 |
| N-4 deadline 时钟仍固定 wall | **C**（保留） | `3394-3402` `extc_rc_pauseNow` 未变（CLOCK_MONOTONIC；无宏退回 CPU `clock()`） | DL2 仍成立：`STEP_CPU=1` 时 `[step]` 记 cpu，deadline 是 wall ⇒ 判「max ≤ D+2seg」须同口径（wall 口径下 deadline 严格成立；cpu 口径只会更小）。 |

**净结论**：lead 关心的两个形式化界项（DL3 描述符字段、DL6 首 advance）已在源码层修好并写进注释；剩余 N-1 是「cap 声明与实现不一致」的 A 类，影响 §19/§17.2 对 `QUOTA_MAX_MARK` 的表述（不是停顿破界，因为 deadline 兜底）。N-2/N-3 是量具/注释级；DL2 需在报告口径里持续标注。G18 的 `g18.py` 常数（`QUOTA_DEADLINE_US=250`）仍待按 §10.11 G-1 更新。

### §10.13 冻结快照 `7c2a9ce` 复核（N-1..N-4 收口；纯静态）

> 口径：`src/back/reclaim.c` md5 **7c2a9cedd7edd54de0a378d8a507dcbe**（3801 行）、`build-rf/extc` **7019342b3e5ccddbe67cbdc96eae2395**、`tests/reclaim/driver.c` 83e167d761705268dbda3f89895ea574、`docs/analysis/P0-IMPL.md` c5a715ed…（233 行）。md5/行数已自行复核一致。

| 编号 | 判定 | 证据（冻结行号） | 说明 |
|---|---|---|---|
| N-1 钳位 | **✓ 已修** | `1363-1366`：`_quota = n >> EXTC_RC_QUOTA_SHIFT_MARK;` 紧跟 `if (_quota > (size_t)EXTC_RC_QUOTA_MAX_MARK) { _quota = (size_t)EXTC_RC_QUOTA_MAX_MARK; }` | 语义现在是真 `min(n>>3, 1<<20)`：n≤8 MiB 时按字节线性；n>8 MiB 截到 1<<20（memory-first「一次分配最多扫 8 MiB 当量」），剩余交给后续分配/deadline。P0-IMPL §1.2/:60 的表述与代码一致。 |
| N-2 键冲突 | **✓ 已修** | 三条打印互不覆盖：`:3201` `[quota] round_hits=%lld round_checks=%lld`；`:2034` `[quota] deadline_us=… hits_total=%lld hits_round=%lld`；`:3458` `[quota] deadline_us=… checks=%lld hits=%lld hits_round=%lld` | 键集：`hits_total`（累计，仅 stats 行）、`hits`（累计，仅诊断行）、`round_hits/round_checks`（每轮，sweepBegin 打印后清零，`:3197-3203`）、`hits_round`（自上一轮以来的 hit 数，两处终值行都有，退出时=末轮值）。**残留**：`pause_parse.py` 的通用解析会把每轮 `round_hits` 行也收进 `rec["quota_lines"]`（无害），但 `g18.py:309` 仍读 `.get("hits")` —— 只有诊断行存在时才有值（PAUSE 路径），stats-only 路径为 `None`；阶段二报告请统一读 `hits_total`，趋势读 `round_hits` 序列（C，门禁消费侧待更新）。 |
| N-3 注释界 | **✓ 方向已修，界仍偏松** | `:1005`「For a legal slab (at most 65536 slots: 1 MiB / the 16-byte minimum slot) this cap never binds: ceil(nslots/64) <= 1024」 | 结论（`QUOTA_MAX_WORDS=8192` 永不生效）与我 §10.8 一致。但 65536 槽是「1 MiB 全用 16 B 槽」的假设，而 `slabNew` 的倍增规则（chunk 到 ≥1024 槽即停）使 ss=16 时 nslots≈4088、大 ss 时 ≤1024 ⇒ 真实上界 `ceil(nslots/64) ≤ 64`，比注释的 1024 小 16×。注释仍是安全上界，可留；若追求精确可写 `<= 64`。 |
| 1019 措辞 | **✓ 已统一** | `:994`、`:1014`、`:1019`、`:1371` 全部写 `D + 2 segments`；`:1019` 明确「800 us + 2 segments (about 0.96-1.0 ms), under the 1 ms histogram edge」 | DL6 的措辞矛盾已消除；`D+2seg ≤ 800+2×98 = 996 µs < 1 ms`，与 over1ms=0 的判据自洽。 |
| N-4 时钟口径 | **记录（未变）** | `:3394-3402` `extc_rc_pauseNow`：`CLOCK_MONOTONIC` 纳秒；无宏退回 `clock()`（CPU 微秒）；deadline 固定用它 | deadline 是 **wall** 上限；`[step]` 在 `STEP_CPU=1` 时是线程 CPU。判「max ≤ D+2seg」须注明口径：wall 口径严格成立，cpu 口径只会更小（并发抢占时 wall 更大）。阶段二报告两口径都给。 |

**与 P0-IMPL.md 的一处交叉核对（C，文档内部矛盾）**：P0-IMPL §6.2 说 dense 的 `checks=0` 是因为「它的分配都小于 4096 units（32 KiB 请求字节以下）」，但 `pd_pointer_dense` 每轮就是一次 8 MiB 分配（`8 MiB >> 3 = 1<<20 ≥ 4096`），该解释自相矛盾。静态可判：`checks=0` 只能说明**进入 mark 配额分支时 `_dl0` 未被取**或**根本没进该分支**（`1367` 的 `if (r->marking) … if (msp||!seedsDone||scanObj)` 不成立），即 dense 的慢尾（199.6–369.3 µs）来自 `[step]`/collect 的分段扫描通道，**不是**配额循环。后果：deadline 不能给 collect 路径封顶；阶段二若要用 dense 证明「deadline 兜底生效」，需要另造「分配时正在标记且 `n ≥ 32 KiB`」的形状（见 §12.4 的探针建议），不能沿用 `hits>0` 作为 dense 的判据。另 §5 表 `deadline=250` 峰值 34,712 kB 与源码注释 `:1017-1018` 的 34,524 kB 差 188 kB（不同 rep，非矛盾，引用时注明）。

## §11 P0-B（惰性 freelist / hole count）静态审计（r3 树，未跑程序）

> 口径：`src/back/reclaim.c` md5 `abef7e345e70689859a5d81428265193`（3750 行，15:41:02）。本节只做静态推导；链长 vs `nfree` 的运行时一致性、`uint32` 下溢检测需要阶段二插桩（见 §11.3）。

### §11.1 数据模型与全部维护点

`nfree` = 「occ=0 但**不一定**在复用链上的洞」计数；链上只放**热路径释放**（`freeSlot`）的槽；sweep 只把洞计入 `nfree`、不逐个链入，等 bump 耗尽后**一次性**从 occ 位图重建链（`1388-1414`）。

| 位置 | 动作 | 证据 |
|---|---|---|
| `slabNew` | `freelist=-1; bump=0; nfree=0` | `849-851` |
| `freeSlotLink` | 头插：`tid[i]=freelist+1; freelist=i`（不改 nfree） | `1552-1557` |
| `freeSlot` | `occ/rsbit` 清位、`hold/rscnt` 清、**`nfree++`**、`bytes-=ss`、`!avail ⇒ classPush` | `1559-1580` |
| `freeSlotWord`（sweep 批量） | 每 word 清位一次、**`nfree += popcount(dead)`**、不链入、`bytes-=n*ss`、`!avail ⇒ classPush` | `1586-1628` |
| `alloc` 满判据 | `bump >= nslots && nfree == 0` 才算满；class 链线性跳过同形 | `1363-1373` |
| `alloc` 取槽 | ① `freelist>=0` ⇒ 弹链、`nfree--`；② `bump<nslots` ⇒ bump；③ 否则重建链、`nfree=chained`、弹一个、`nfree--` | `1382-1414` |
| `alloc` 重建失败 | 位图显示真满 ⇒ `nfree=0`，新建 slab（旧块仍 avail=1，懒弹出） | `1413-1421` |
| `sweepElide`（整片垃圾） | `bump=0; freelist=-1; nfree=0`，清 occ/mark，`!avail ⇒ classPush` | `3131-3135` |
| `releaseOld` | 走 `freeSlot`，无重复扣账（`releaseOld` 自身不减 `bytes/freedSlots`，只由 freeSlot 扣） | `3021`、`2966-3056` |
| `alloc` 新槽初始化 | `occ` 置位、`tid=typeIdx`、`hold=0`、`bytes+=ss` | `1424-1438` |

### §11.2 不变式逐条核（结论：静态成立）

| 编号 | 不变式 | 静态论证 | 判定 |
|---|---|---|---|
| B-I1 | 链是洞的子集、无重复 | 链项只来自 ① `freeSlot`（occ 刚清 0）或 ② 重建（`~occ & valid`，空链时一次遍历）；重建条件 `freelist < 0`（`1382` 的 if 与 `1386` 的 else-if 都不满足才进 `1388`）⇒ 链空时重建 ⇒ 无重复 | **成立** |
| B-I2 | 重建只在链空时发生 | 进入重建的唯一路径是 `freelist<0 && bump>=nslots`；`1382` 保证 `freelist<0` | **成立** |
| B-I3 | 重建不漏 issued 槽 | 重建分支要求 `bump>=nslots`（`1386` 的 `else if (bump<nslots)` 已排除），故 `[0,nslots)` 全部 issued；不需要 `idx<bump` 过滤 | **成立**（注意：若未来允许 bump 中途 elide/回退，必须补 `idx<bump` 掩码） |
| B-I4 | 重建后 `nfree` == 链长 | 重建前链空（B-I2），`chained` 逐槽 ++，随后 `nfree=(uint32_t)chained` 再弹一个 `nfree--`；链长 = chained-1 = nfree | **成立** |
| B-I5 | `nfree` 不双计 | 热路径已释放槽 occ=0，sweep 的 `dead=occ&~mark` 不会再次包含；重建用 `~occ` 与增量计数无关，且整表覆写 | **成立**（注释 `1619-1621` 与实现一致） |
| B-I6 | 无 `uint32` 下溢 | `nfree--` 只在 `freelist>=0` 时执行，而链非空 ⇒ 至少一个 `freeSlot`/`chained` 计入 ⇒ `nfree>=1`；唯一潜在破口是不变式被破坏（双重释放），此时 `nfree` 下溢成巨值，但下一次 bump 耗尽的重建会整表覆写 `nfree` ⇒ **自愈**，不会永久卡住 | **成立（自愈）**；建议阶段二在 `EXTC_RC_SHADE_GUARD` 式诊断下加 `if (nfree==0) oops` |
| B-I7 | bump 路径不误碰洞计数 | `else if (bump<nslots)` 分支不动 `nfree`；洞在 bump 耗尽后由重建收编 | **成立** |
| B-I8 | elide 后状态 | `bump=0/freelist=-1/nfree=0` + occ/mark 清；旧 `tid/hold` 残留由 alloc 逐槽覆写（`1435-1437`）⇒ 不影响 | **成立** |
| B-I9 | 满判据与 class 链一致 | 满 = `bump>=nslots && nfree==0`；链上块可能已满（懒弹出），跳循环用同一判据弹出（`1368-1372`） | **成立（懒弹出会多走 O(链长) 次，属设计取舍）** |

### §11.3 静态无法闭合、留给阶段二的点

1. **链长 = nfree 的运行时抽检**：需要一个只读诊断（`EXTC_RC_STATS` 行或临时 `assert`）在 `alloc` 弹链后遍历链数一次，比对 `nfree`；建议在 `pd_bigobj` 与 `tests/reclaim` 各跑一遍。
2. **双重释放检测**：`freeSlot` 不做 `occ` 校验（性能取向）；若上游计数出现 `hold` 归零两次，链会出现自环（`tid[i]=i+1` ⇒ 弹链死循环）。阶段二用 ASan/计数档压测确认无此路径。
3. **B 的吞吐代价**：满判据多了 `nfree` 读 + 比较、class 链多一层懒弹出；重建是 O(nslots/64) 次位图遍历（每块每次访问至多一次）。这与 G20 的 `promote [step] total / sizeratio / cycleratio` 判据直接相关，阶段二须用 after 数字判回归。
4. **`nfree` 是否为「洞」而非「可用槽」**：重建失败时 `nfree=0` 而位图确实全满，二者一致；但若位图与 `nfree` 不一致（如 I6 的双重释放），重建会把 `nfree` 拉回真值，**洞计数本身不会造成永久泄漏**——这也是 B 比「只增不减的洞计数」更安全的理由。



## §12 阶段二待跑清单（§9–§11 收敛；冻结口径 `7c2a9ce`）

> 本节把 §9/§10/§10.7–§10.13/§11.3 的开放项收敛成「GO 后照做即可」的清单。冻结三件套：`reclaim.c` **7c2a9cedd7edd54de0a378d8a507dcbe**（3801 行）、`build-rf/extc` **7019342b3e5ccddbe67cbdc96eae2395**、`tests/reclaim/driver.c` **83e167d761705268dbda3f89895ea574**；另有 `docs/analysis/P0-IMPL.md` c5a715ed…（233 行）作为**被审对象**（其数字我只核对、不独立重跑）。

### §12.1 开跑前：冻结核对 + 自建构建（先做，失败即停）

```bash
cd /home/alphayang/extC_Compiler
md5sum src/back/reclaim.c src/back/codegen.c src/check/check_top.c src/check/check_stmt.c \
       tests/reclaim/driver.c build-rf/extc
# 期望与 §12 开头四值逐字相同；任一不符 ⇒ 树又动了，先报 Lead 再继续
mkdir -p /tmp/gc-da
make BUILDDIR=build-da -j24 2>&1 | tee /tmp/gc-da/make.log   # 默认 CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -O1 -g
grep -ci "warning" /tmp/gc-da/make.log                        # 期望 0
md5sum build-da/extc                                          # 记录（不要求等于 build-rf：不同 BUILDDIR/时间）
```

判据：源码 md5 四项一致；`make` exit 0 且 warning=0；`build-da/extc` 可执行。**不复用** `build-rf`/`build-lead5`/`build-emp`。

### §12.2 逐 gate 命令表（全部用 `build-da/extc`，输出统一进 `tests/gc-p0/out/p0-after-da/`）

| # | gate | 命令 | 判据（谁判） | 产物 |
|---|---|---|---|---|
| 1 | tests/reclaim 默认档 | `bash tests/reclaim/run.sh` | exit 0、FAIL=0（脚本自带 ASan/UBSan、`ALLOC_INLINE=0`、chunk=1/1e9 子测） | 终端 ok 计数；P0-IMPL 期望 181/0 |
| 2 | tests/reclaim 计数档 | `EXTC_RC_NOCOUNT=0 bash tests/reclaim/run.sh` | exit 0、FAIL=0（P0-IMPL 期望 237/0） | 同上 |
| 3 | gate.sh（Ir 棘轮） | `EXTC=./build-da/extc bash tests/reclaim-perf/gate.sh` | `LIMIT=88029115`（gate.sh:125），实测 Ir ≤ LIMIT 且退出 0 | callgrind 输出 + Ir |
| 4 | 跳表/历史表自检 | `python3 tools/check_skip_table.py`；`python3 tools/check_no_history.py --gate` | 均 exit 0 | 终端 |
| 5 | G18 主判据 | `EXTC=./build-da/extc OUT=tests/gc-p0/out/p0-after-da tests/gc-p0/run.sh g18` | `peak200 ≤ 3·live+2·SLAB`（live 从 `[work] M_max/3` 反读）且 `peak200 ≤ peak60×1.05`；stdout 逐字 `D n=200 made=200 s=0`；exit 0（summarize/g18 判） | `timemax.txt`、`log/pd_bigobj_pause_r*.err`、`g18.json` |
| 6 | G18 观察列（我手动） | 同上（g18 子命令已含 dense） | `[quota] hits_total`、`round_hits` 序列不增长；`[step]` max/over1ms；dense 只观测 `PD n=200 made=200 s=1048575` | 同 5 |
| 7 | G18-ASan | `cc -O1 -g -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer tests/gc-p0/out/p0-after-da/c/pd_bigobj.c -o /tmp/gc-da/pd_bigobj_asan`；`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 timeout 300 /tmp/gc-da/pd_bigobj_asan 200`；dense 同法 | rc=0、stderr 无 sanitizer 报告、stdout 逐字 | /tmp/gc-da/*_asan.* |
| 8 | G20（P0-B 停顿收益） | `EXTC=./build-da/extc OUT=tests/gc-p0/out/p0-after-da REPS=5 tests/gc-p0/run.sh g20` | promote `[step]` total ≤−10%、sizeratio ≤−40%、cycleratio ≤+3%、promote step max 跨 rep 中位 ≤60 µs；四形状 wall/RSS ±3%、stdout sha256 相同（P0-IMPL §6.4 已给 before/after 值） | `g20.json` |
| 9 | G20 对照 | `BEFORE_G20=tests/gc-p0/out/p0-before/g20.json AFTER_G20=tests/gc-p0/out/p0-after-da/g20.json tests/gc-p0/run.sh g20cmp` | 同 8；before 必须是 **build-lead5** 产物 | 终端 |
| 10 | （可选）G1 cpu 口径 | `EXTC_RC_STEP_CPU=1 EXTC=./build-da/extc OUT=tests/gc-p0/out/p0-after-da tests/gc-p0/run.sh g1cpu` | over1ms=0；② 下界>1e-4 才 RED；③ p99.9 ≤50 µs（缺直方图则 N/A） | `pauses.jsonl` |
| 11 | （可选）G4 正式 | `EXTC=./build-da/extc OUT=tests/gc-p0/out/p0-after-da tests/gc-p0/run.sh stress` | VmHWM ≤ M_max+8 MiB、稳态差 ≤2%、A<B<C | `g4_stress.json` |

**串行纪律**（Lead m03189/m03358）：5–9 计时类必须独占机器；1–4 可以串行但不与计时类并发。`OUT` 固定 `p0-after-da`，**不要**写进 `p0-after`（那是 build-rf 的正式 after，避免两套数字混在同目录）。

### §12.3 我要独立重算的数字（≥3，不接受摘要值）

1. **G18 上界**：从 `log/pd_bigobj_pause_r2.err` 的 `[work] M_max=` 取 live=`M_max/3`，手算 `3·live+2·1024 kB`，与 `timemax.txt` 的 200 轮 ru_maxrss 比；同时用 60 轮比 `peak200 ≤ peak60×1.05`（两项都要自己算，不用 g18.json 结论）。
2. **G20 三形状 `[step]` total**：从 `g20.json` 原始 before/after 的 `step_total` 重算 promote/sizeratio/cycleratio 百分比，核对 P0-IMPL §6.4 的 −10.34%/−40%/−0.09% 与 `sizeratio +10.34% 墙钟` 是否同源（一个是 `[step]`，一个是墙钟，别混）。
3. **gate.sh Ir**：自己从 callgrind 输出取 total Ir，与 `LIMIT=88029115` 比；顺带核对 P0-IMPL 的 86,618,661 是否落在同一 build。
4. **deadline hits 趋势**：解析全部 `[quota] round_hits=` 行，判「后 1/3 轮的最大值 ≤ 前 1/3 轮最大值」（g18.py 目前不做，我手动做）。

### §12.4 判据修正建议（交给 Lead 拍板，不改门禁脚本）

1. **dense 的 `hits>0` 判据不成立**（§10.13 交叉核对）：`pd_pointer_dense` 实测 `checks=0`，其慢尾走 `[step]`/collect 分段通道而非配额循环（8 MiB 分配在 `r->marking` 为假时不进配额分支）。⇒ dense 行改为「`[step]` max ≤ 1 ms 且 over1ms=0；hits 不作判据」。
2. **另设「deadline 兜底生效」探针**（小代价、可静态预期 `hits>0`）：对 `pd_bigobj` 加 `-DEXTC_RC_QUOTA_DEADLINE_US=50` 编译（它确实进配额循环：P0-IMPL 实测 `checks=836096`），200 轮看 `[quota] hits_total>0` 且 stdout 不变；再加 `-DEXTC_RC_QUOTA_MAX_MARK=8192` 作 pause-first 变体对照。
3. **g18.py 常数**（§10.11 G-1/G-2）：`QUOTA_DEADLINE_US` 改从 `[quota] deadline_us=` 读；天花板改 `deadline+2×batch_us`；`W_s` 用 `8·pm+64 ≤192` words（或实测 `G18_SLAB_NSLOTS`）。
4. **g18.py 的 `deadline_hits`**（§10.13 N-2）：改读 `hits_total`，趋势读 `round_hits` 序列；`hits` 键只在诊断行存在。

### §12.5 执行顺序与失败处理

顺序：§12.1 → 1→2（源码级，最快）→ 3→4 → 5/6（计时，独占）→ 7 → 8/9（计时，独占）→ 10/11（按 Lead 需要）。任一 RED：只记录原始日志路径 + 三行复现命令 + 当时 md5/OUT，**不重跑求绿**（噪声地板已知：WSL2 max 23.8–171.2 µs，见 run.sh 头部注释）；只有「可复现的同样 RED」才升级为结论。所有数字进 `/tmp/gc-da/`，报告只写路径与口径。

### §12.6 独立性分界（写进终稿）

- **独立跑**：§12.1/§12.2 的 1–9、§12.3 的 1–4、以及 §10.13 的静态判定（N-1..N-4）。
- **只核对不重跑**：build-rf 的正式 after（`p0-after`）、P0-IMPL 的 §5/§6 数字、MEASUREMENTS/GC-ROOFLINE 的历史数字；差异以「口径/构建不同」而非「错误」表述，除非能复现。


## §13 阶段二：独立跑门禁（build-da，2026-11-09 16:19–16:45）

### §13.0 冻结口径复核（GO 声明 vs 实测，发现 2 处冲突）

| 项 | Lead m03238 GO 声明 | 实测（本报告依据） | 结论 |
|---|---|---|---|
| `src/back/reclaim.c` | `7c2a9ced…`、3801 行 | **`7c2a9cedd7edd54de0a378d8a507dcbe`、3801 行** | ✓ |
| `src/back/codegen.c` | `cf288544…`、15994 行 | **`d339382889b87c0dd18b357c69e2e779`、16017 行** | **GO 引用了 v4 旧口径**（MEASUREMENTS-P0-IMPL:9 已自注更正为 d33938，但 GO 文本未同步） |
| `tests/reclaim/driver.c` | `83e167d7…` | `83e167d761705268dbda3f89895ea574` | ✓ |
| `docs/analysis/P0-IMPL.md` | `c5a715ed…`、233 行 | 同 | ✓ |
| `MEASUREMENTS-P0-IMPL.md` | `cc7cd35e…`、369 行 | **`97c228e653e72b36688a18e36cc907e4`、384 行、mtime 16:24:20** | **GO 后 empirics 又改过**（§7-12 自证 369→384）；引用以 384 行为准 |
| `release/GC-ROOFLINE.md` | 「我马上会改 §7」 | 已是 **v2.0 合并版 915 行**（mtime 16:24:37），§6.6/§7 在仓库内 | 按仓库内版本核对 |
| `build-rf/extc` | `7019342b…` | `p0-after/g20.json` 的 `extc_md5=7019342b…` 互证 | ✓ |
| `build-lead5/extc`（before） | — | `/tmp/empirics/ab-full/ab.json` 的 `a.md5=9bef5488…` 互证 | ✓ |

### §13.1 构建（独立，不复用任何他人生成物）

`rm -rf build-da && make BUILDDIR=build-da -j24`：**exit 0，warning 0**；`build-da/extc` md5 **`21051bbf50b4097bb1bee7e14ff2143e`**（2,999,696 B，16:19:23）。Makefile 默认 flags `-std=c11 -Wall -Wextra -Wpedantic -O1 -g`；生成 C 用 run.sh 的 `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`。`OUT=tests/gc-p0/out/p0-after-da`（**未写** `p0-after`）。

### §13.2 逐 gate 结果（本机实测；只有第 16 行是「只核对未跑」）

| # | gate | 命令/口径 | 结果 | 判定 |
|---|---|---|---|---|
| 1 | `tests/reclaim` 默认档 | `EXTC=./build-da/extc bash tests/reclaim/run.sh` | 主断言 **通过 181 失败 0**；ASan/UBSan 181/0；`ALLOC_INLINE=0` 181/0；chunk=1 10/0；chunk=1e9 红自测有牙 | PASS |
| 2 | `tests/reclaim` 计数档 | 同上 + `EXTC_RC_NOCOUNT=0` | 主断言 **通过 237 失败 0**（`/tmp/gc-da/reclaim-nc0-da.log:243`）；ASan 237/0；不内联 237/0；chunk=1 10/0 | PASS |
| 3 | `gate.sh` Ir 棘轮 | `EXTC=./build-da/extc LIMIT=88029115` | **86,618,918 Ir** ≤ 88,029,115（= 基线 82,037,532 的 +5.59%）；slotindex 域内穷举 1,433,436 组 0 不符 | PASS |
| 4 | 跳表/历史表 | `check_skip_table.py`；`check_no_history.py --gate` | 62 类、未标记早退 0；存量 0 条；均 rc=0 | PASS |
| 5 | **G18 主判据** | `run.sh g18`（`p0-after-da`） | 60/200 轮 `ru_maxrss` **26,264 / 26,264 kB**；live 8,192 kB；上界 **26,624 kB**；ratio **1.000×**；stdout 两档 `D n=… made=… s=0` 逐字；exit `[0,0]` | **PASS** |
| 6 | G18 观察列 | 同上 + `STEP_CPU=1` | step_n 7,734/26,249；total 61,467.1/184,943.4 µs；max 246.9/296.4 µs；collects 59/199；`hits 0/0`、checks 244,736/836,096；p99.9 桶 128/64 µs；`[histo]` b1ms 及以上全 0；`[top]` 全是 `mark/advance seg=8192 pops=1`；闭式 W_s=192 words、W_m=8192 units ⇒ 标称 145.2–145.7 µs、天花板 1086.7 µs | 观察 PASS；与 doc 的 48,320.7/165,611.9/216.9/185.3、hits 0/1 有 10–60% 跨构建差（C1） |
| 7 | dense 观察 | 同上 | `ru_maxrss` **111,900 kB**；step_n 737,759；total 1,118,006.6 µs；max 304.7 µs；p99.9 64 µs；over1ms 0；hits 0/**checks 0**；`[chan] regionfree` n=1 max **6,176.3 µs**（不在 `[step]`） | 不参与判定；见 A1 |
| 8 | G18-ASan | `cc -O1 -g -fsanitize=address` 手工编 `p0-after-da/c/pd_bigobj.c` 跑 200 轮 | rc=0、stdout 逐字、无 ASan 报告；44,808 kB / 0.43 s（ASan shadow，不按 26 MiB 判） | PASS |
| 9 | **G20** | `REPS=5`、pin6、`p0-after-da` | 见 §13.3 | **RED（1 硬项 + 3 对称项）** |
| 10 | G20cmp（官 before） | `BEFORE_G20=p0-before/g20.json` | 同 §13.3；before 夹具与 doc §4.1 逐位一致（48.323/6,684/18,406.9/89.5 等） | RED（同上） |
| 11 | G1 cpu 口径 | `STEP_CPU=1 run.sh g1cpu` | ① over1ms：cyc/pc/pb/pd **全 0** ⇒ PASS；② pb `[2.31e-4,7.56e-4]`、pd `[4.03e-4,2.14e-3]` ⇒ **RED**（cyc/pc 跨阈 N/A）；③ p99.9：cyc 16、pc 16、pb 32、pd 64(N/A) | ①PASS ②RED ③部分 |
| 12 | G4-Stress | `run.sh stress` | A **9,104** / B 41,820 / C 82,956 kB；A ≤ 15,223 kB；末 8 轮恒 9,104、相邻差 0.00%；A<B<C；M_max A=7,200,144 B、C=1,099,511,627,776 B(=2^40)；pressure_steps 98/98/0 | PASS |
| 13 | `gate-release.sh` | `EXTC=./build-da/extc` | **4/4**：A sweeps 1/1、B 1.00×、C 默认与关档 stdout 逐字相同、D releases **19/79**、release_slots **4,883/20,303** | PASS（与 GC-ROOFLINE §6.6 末行逐字一致） |
| 14 | 独立 A/B（P/B 净效应） | `/tmp/gc-da/ab-full`：A=build-lead5(idle)、B=build-da，9 reps pin6 | promote wall 45.408→42.819（−5.70%）、Ir 804,669,717→820,354,890（**+1.95%，与 doc 逐位相同**）、rss −5.69%；sizeratio wall 22.46→25.006（**+11.34%**，doc +9.31%）、Ir 229,478,462→243,258,295（**+6.00%，同 doc**） | 方向确认；墙钟幅度 9.3–11.3% 有噪声 |
| 15 | quota0 归因 | 同上 + `-DQUOTA_MAX_MARK=0 -DQUOTA_DEADLINE_US=0` | sizeratio wall 24.318→23.343（**−4.01%**，doc −7.34%）、Ir +0.96%（同 doc）、qHits 0/0 | Ir 归因确定；**墙钟应写区间 4.0–7.3 pp** |
| 16 | G2/G3/G6/G7/G10/G11/G17 | 未独立跑 | 只核对（§13.5） | N/A |

### §13.3 G20 独立结果（官 after=p0-after 5 reps vs 我 build-da 5 reps）

| 形状 | 指标 | before（p0-before） | 官 after（p0-after） | 我 after（p0-after-da） | 相对判据 |
|---|---|---|---:|---:|---|
| promote | wall_ms | 48.323 | 42.920（−11.18%） | 42.552（−11.94%） | −11% 越 ±3%（改善方向） |
| promote | rss_kb | 6,684 | 6,304（−5.69%） | 6,304（−5.69%） | 越 ±3%（改善方向） |
| promote | step_total_us | 18,406.9 | 14,692.6（−20.18%） | 14,543.7（−20.99%） | ≤−10% **PASS** |
| promote | step_max 中位（reps） | 89.5 | **63.8**（37.6/43.7/75.4/63.8/181.6） | **67.9**（36.0/95.6/67.9/57.2/277.6） | ≤60 **RED**（判据争议 A2 §13.6） |
| sizeratio | wall_ms | 24.148 | 24.639（+2.03%） | 24.938（**+3.27%**） | 官未越 3%、我略越 |
| sizeratio | rss_kb | 3,436 | 3,376（−1.75%） | 3,440（+0.12%） | 官 PASS/我 PASS |
| sizeratio | step_total_us | 2,606.8 | 1,136.5（−56.40%） | 1,140.1（−56.26%） | ≤−40% **PASS** |
| chase | wall_ms / step_total | 149.170 / 5,211.4 | 146.47（−1.81%）/ 5,046.5（−3.16%） | 147.131（−1.37%）/ 5,133.0（−1.50%） | PASS |
| cycleratio | wall_ms / step_total | 13.035 / 413.7 | 13.15（+0.88%）/ 369.2（−10.76%） | 13.236（+1.54%）/ 371.1（−10.30%） | PASS |

`[histo]` 聚合（我从 `pauses.jsonl` 重新分桶，5 reps 合并）：promote before n=106,440、p99.9 桶 16 µs、>64 µs **12 步=0.0113%**；after n=55,440、p99.9 桶 **32 µs**、>64 µs **7 步=0.0126%**。sizeratio before 0.0098% → after 0.0333%（1.2 样本/rep，噪声）；chase 0.143%→0.165%；cycleratio 0/0。⇒ **max 判据由 1–2 个样本支配，p99.9 与占比判据稳定**（与 GC-ROOFLINE §8.1 R6「max 不可作契约」自洽）。

### §13.4 独立重算的 4 个数（Lead §12.3）

1. **G18 上界**：`[work] M_max=25,165,872 B ⇒ live=8,388,624 B=8,192 kB`；`3·8192+2·1,024=26,624 kB`；peak 26,264 ⇒ 余量 360 kB；peak60=peak200=26,264。**25,167,920 kB 是旧 1024× buggy 打印**（见 A2），正确值就是 26,624。
2. **G20 百分比**：全部用 before/after 的 `median` 重算，结果见 §13.3；step_total 三判据 PASS（−20.2%/−56.4%/−10.8%）。
3. **gate.sh Ir**：86,618,918（limit 88,029,115）；与 P0-IMPL 86,618,661 差 **+257 Ir（+0.0003%）**，同源（同源码、同 CFLAGS；差在 callgrind/路径计数，不影响判定）。
4. **deadline hits 趋势**：60 轮 `round_hits=0` 全 0、200 轮全 0；`checks` 244,736/836,096 均匀（每轮 4,224）；**无增长趋势**。dense 的 `checks=0` 机制另见 A1。

### §13.5 独立跑 vs 只核对（终稿分界）

- **独立跑（本机、build-da）**：§13.2 的 1–15；dense；`[histo]` 重分桶；A/B 与 quota0；`gate-release.sh`；G18-ASan。
- **只核对（用他人产物/文档，仅查内部一致与可追溯）**：build-rf 的官方 after（`p0-after`）、MEASUREMENTS-P0-IMPL §3.2/§4/§5、GC-ROOFLINE §6.6/§7、P0-IMPL §5/§6、G2/G3/G6/G7/G10/G11/G17（本轮未跑）。
- **已逐位核对 MEASUREMENTS §3.2/§4.2 与官方 `p0-after` 的 `g18.json`/`g20.json`**：60/200 轮 26,264/26,264、7,734/26,116、48,320.7/165,611.9、216.9/185.3、collects 59/198、hits 0/1；g20 四形状 `42.92/6304/14692.6/63.8` 等全部一致 ⇒ **doc 不是抄错，差异纯跨构建**（我 build-da 的同项为 61,467.1/184,943.4/246.9/296.4）。
- **未做**：不改门禁脚本、不改源码/tests/其他 docs、不 commit。

### §13.6 发现（只报不改；A=错误断言，B=口径偏移，C=缺证据，D=真 bug）

**A1（A）** `P0-IMPL.md:175`（及 §6.2 的 :173 表）把 dense 的 `checks=0` 解释为「分配都小于 4096 units（32 KiB 以下）」。**8 MiB dense 分配 = 1,048,576 units，远大于 4,096 ⇒ 解释不成立**。探针实证（/tmp/probe：在生成 C 的 `extc_rc_alloc` 入口对 n≥32768 打状态，跑 5 轮）：每次 8 MiB 分配都是 `marking=0 msp=0 seeds=1 scan=0 sweeping=0`，配额循环（`reclaim.c:1365-1390` 的 `if (r->marking)` 分支）根本不进；而 1M 次 leaf 分配 n=8 ⇒ `_quota=1 unit` ⇒ `_dl0 = _quota >= 4096 ? pauseNow() : -1` 使 deadline 检查与 `checks++` 都不发生。**正确机制**：该形状的大分配总发生在无在飞标记的轮次（`marking=0`），慢尾来自 `maybeCollect` 里 `markAdvance` 的 `markResume` 分段（`[top]` `pops=1 seg=8192`，246.9/296.4 µs），不是 P/deadline 通道。结论句「与配额通道无关」成立，**理由需改写**；`checks` 的语义是「配额循环里 ≥4096 units 的采样次数」，不是循环进入次数，doc 不要把它当 fan-out 证据。
**A2（A）** G18 上界的旧值仍在**未被重生成的 summary 产物**里：`tests/gc-p0/out/p0-after/summary.txt:51` 写 `live = 8388624 kB…上界 = 25167920 kB（⇒ PASS）`，`p0-before/summary.txt:51` 同形；而两处 `g18.json`（`rounds[].live_bytes=8,388,624`、`live_kb=8,192`、ts 16:07:01Z）已修，summary 的 mtime 16:05:53 早于 json ⇒ **没有重跑 summarize.py**。下游若引用 25,167,920 kB 会得到 1024× 错。修法：重跑 summarize（或删旧 summary/加注）。MEASUREMENTS §7-12 只说 json 重生成、没说 summary 未重生成。
**A3（A）** `GC-ROOFLINE.md:633`（G4 行）`A=9148 kB ≤ 7200+8192=15392 kB`：`M_max=7,200,144 B = 7,031 kB`，正确上界 **15,223 kB**（MEASUREMENTS-P0-IMPL §5、MEASUREMENTS-GC-P0:261 都用 15,223）——这里又把字节当 kB 了 1,024 倍。改法：`7,031+8,192=15,223`；顺带 `9148` vs 我 9,104、doc §5 9,104 也不一致（跨构建，标单次即可）。

**B1（B）** GO 的口径两处过期：`codegen.c`（`cf288544/15994` → 实际 `d3393828/16017`）、`MEASUREMENTS-P0-IMPL`（`cc7cd35e/369` → 实际 `97c228e6/384`，GO 后仍被改）。建议 Lead 的正式口径表以实测 md5/行数为准，并在 `GC-ROOFLINE.md:600` 同步（它现在写「AUDIT-P0-IMPL.md（628 行）」，本 §13 后会过期）。
**B2（B）** `P0-IMPL.md:173-175` 与 MEASUREMENTS §3.3/§7-9 的 dense 数字是 build-rf 单次（`max 199.6–369.3`、rss 112,280–112,284、checks=0），我 build-da 的 `max 304.7`、rss 111,900、`over1ms 0`——同构不同值，引用时须带构建；不构成错误。

**C1（C）** G18 after 绝对计时跨构建不可复现：doc `48,320.7 / 165,611.9 / 216.9 / 185.3`、hits 0/1、collects 59/198 vs 我 `61,467.1 / 184,943.4 / 246.9 / 296.4`、hits 0/0、collects 59/199（+27%/+11.7%/+14%/+60%）。但**内存结论完全一致**（26,264、ratio 1.000、26,624 上界、stdout、over1ms=0）、`step_n ×61` 与 `total ×33` 方向一致 ⇒ doc 的数应标「本机单次观测」。
**C2（C）** G1② 区间不稳定：doc pb `[1.39e-4,4.23e-4]`（MEASUREMENTS after 3.31e-4）；我 `[2.31e-4,7.56e-4]`；两者都 RED。`GC-ROOFLINE.md:628` 引用的「doc-audit 独立构建 [1.20e-4,5.43e-4]」是 v4 旧跑，建议改为「下界>阈跨构建稳定，区间随构建变」。
**C3（C）** G3b：doc pb regionFree cpu 86.2 ms / doc-audit 99.0 ms；我 `[chan]` pb `n=3 max 118,517.0 µs total 281,211.3 µs`（更高 1.2–1.4×），仍 RED；pd 我 1,152.9 µs vs doc 27.5 ms——**形状/N 口径不同，未复现，存疑**。建议门禁把 regionFree 的 N/形状/构建写进日志。
**C4（C）** `GC-ROOFLINE.md:636`（G7 行）数字三源冲突且不可追溯：3.17 MB 实为 MEASUREMENTS-GC-P0 的 **pc** 值（3,244 kB）；同句 `cyc 20M/80M = 6092/5888 kB，比 1.000×` —— 6,092/5,888=1.035 不是 1.000，且与 MEASUREMENTS-P0-IMPL §5 的 `cyc 4,984/4,988 = 1.001×`、旧 doc 的 `6,048/6,048` 都不同。我未独立跑 G7 ⇒ 只核对。建议重跑后写 N/构建，或标「未复测」。
**C5（C）** P0-IMPL §5 三列表与我的 run 差（pd_bigobj 200 轮 26,636–26,648 vs 26,264 kB、hits 0–1 vs 0）都在界内，属单次/跨构建差；引用时标单次。

**D：无**。P/B 未引出内存安全 bug：tests/reclaim 两档（181/0、237/0）、ASan/UBSan、G18-ASan、gate-release 4/4 全绿。

### §13.7 「G18 PASS / G20 RED」是否成立

- **G18 PASS：成立**。三要素独立复核：peak 26,264 ≤ 26,624（上界从 `M_max/3` 手算）、peak60=peak200（1.000×）、stdout/exit 逐字；ASan 干净。caveat：doc 的 step_n/step_total/max 是单次观测（C1），引用时带构建。
- **G20 RED：按现行脚本成立，但只有 1 项是真回归**。doc 的「1 硬项 + 2 改善方向」在我这轮是「1 硬项 + 3 对称项」（sizeratio wall +3.27% 也越 3%）。拆开看：
  1. promote `step_max` 中位 63.8/67.9 > 60：after 的 p99.9 桶 32 µs、>64 µs 占比 0.0126% ≈ before 0.0113%，max 由 5 rep 中 1 个 277.6 µs 支配 ⇒ **不是性能回归，是 max 判据不合 R6**；改判「p99.9 桶 ≤64 µs 且 >64 µs 占比 ≤0.05%」则 PASS。
  2. promote wall −11.2%/−11.9%、rss −5.7%：**改善被对称 ±3% 判红**；改单侧（只判 ≤+3%）则 PASS。
  3. sizeratio wall +2.03%（官）/ +3.27%（我）：唯一真回归方向；Ir +6.00%（两次独立逐位一致）+ quota0 收回 4.0–7.3 pp ⇒ 归因 P 的每分配配额记账；幅度写区间，不写「7.34 个百分点」点值。
  ⇒ 建议：改 G20 判据（p99.9/>64 µs 占比 + 单侧 wall/RSS）→ promote 转 PASS；**G20 汇总是否仍 RED 只取决于 sizeratio wall（判据改后 +2.03% 官值其实 PASS，我 +3.27% 临界）**，应在 doc 里写「脚本 RED，其中 2/3 项为判据设计问题，真回归仅 sizeratio 墙钟 2–11%」。

### §13.8 必须改的清单（按优先级）

1. **G20 判据**（`g20.py:284-300`）：max 改 p99.9/>64 µs 占比，wall/RSS 改单侧；否则每次构建的 max 抖动都会造 RED（Lead 已倾向此改法，我的数据支持）。
2. **重生成/删除 `p0-after|p0-before/summary.txt`**（A2），或加「本文件上界为旧 bug，勿引」警示。
3. **P0-IMPL.md:173-175 dense 解释改写**（A1），附探针机制；`checks` 语义写清。
4. **GC-ROOFLINE.md:633 G4 上界改 15,223**（A3）。
5. **GC-ROOFLINE.md:636 G7 行**：重跑或标「未复测」并统一 3.17 MB=pc、cyc 比值口径（C4）。
6. **口径同步**：GO/§6.6 的 codegen md5+行数、MEASUREMENTS 行数、报告自身 628→730+ 行引用（B1）。
7. **G1②/G3b 引用口径**：标「跨构建区间/单次」，regionFree 记 N/形状（C2/C3）。
