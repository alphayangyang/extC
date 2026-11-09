# 阶段 0–2 GC patch 独立复核（第一段 + 冻结树静态复核 + 第二段 FREEZE v4 独立实测）

- 审计人：doc-audit（teammate）
- 审计对象：**冻结树** base commit `85b8871bc345dd33f51c9f15f9d7a25d69ecc3c0` + 冻结 patch（`src/back/codegen.c`、`src/back/reclaim.c`、`src/check/check_stmt.c`、`src/check/check_top.c`），md5 与行数见 §0.1。
- 状态：**第一段 + 冻结静态复核 + 第二段 FREEZE v4 独立复核（已完成，见 §18）**。08:48 之前的实测均来自 pre-freeze 快照 `/tmp/currepo2` / `/tmp/currepo3`，正文已逐处标注；第二段（门禁 G8/G9/G4/G1、ASAN、tier 四档矩阵、P0-7b/LEAK-1/G14、正式 RSS/停顿）已于 `AUDIT GO` 后完成，见 **§18**（FREEZE v4 独立实测）。
- 原则：只报不改。不修改 `src/`、`tests/`、其他 `docs/`；本文件是唯一写入。

## §0 环境、快照与证据约定

| 快照 | 来源 | 用途 | 一致性 |
|---|---|---|---|
| `/tmp/currepo2` | 08:31 工作树 `tar --exclude=.git --exclude='build*'` + `make -j8`（exit 0） | 本报告所有实测量（tier 矩阵、回退 diff、LEAK-1、UAF 复现、poolResize） | **一致快照**，可复现 |
| `/tmp/currepo3` | 08:36 工作树同法快照 + `make -j8`（exit 0） | 只用于证明“当前树半落地” | 半落地（见 §9.2） |
| `/tmp/headrepo` | `git archive 85b8871b` + `make -j8` | pre-existing 对照 | 干净 HEAD |

行号约定：`src/back/reclaim.c`、`src/back/pools.c`、`src/back/coroutine.c` 是**生成器源码**（C 字符串），文中的 `reclaim.c:NNNN` 指生成器源码行，不是生成产物行。`docs/analysis` 里其他报告若引用“生成 C 行号”，与本文件不可直接比较。

### 0.1 冻结口径（2026-11-09 08:48 初始 / 09:11 FREEZE v3；源码不再改动）

| 项 | 值 |
|---|---|
| base commit | `85b8871bc345dd33f51c9f15f9d7a25d69ecc3c0` |
| 改动文件 md5（**FREEZE v3，当前唯一有效**） | `src/back/codegen.c cf2885440981b5c9d154927fb61441b9`、`src/back/reclaim.c 6a69643ae0a852f4790cbeeb17f2ee5d`、`src/check/check_top.c b09e25db062b7f85ddd82805a8196e39`、`src/check/check_stmt.c e732bcbdd97e222c63ee52688db3220e` |
| 改动文件 md5（**v2，已 superseded**） | `src/back/reclaim.c 4f927b4f31b037ed86d6ea25308df616`（v2；其余三文件与 v3 相同） |
| 冻结编译器 | v3 `build-frz/extc` md5 `59171671ab71d5d6583c17339f321149`（v3）；v2 `f988867b269c6b7cf27566a77cddf797`（08:53:53）已 superseded |
| 行数（现行 v3） | `src/back/reclaim.c` **3577**（v2: 3572）、`src/back/codegen.c` 15994、`src/check/check_top.c` 10761、`src/check/check_stmt.c` 5651 |

> **口径（Lead `m02261` 裁决，2026-11-09 09:11）**：**FREEZE v3 唯一有效**。v3 只改 `src/back/reclaim.c`（`slabNew` 尾部加 `if (r->sweeping) s->swEpoch = r->swId;`，修 sweep 期新建 slab 的黑位残留），`reclaim.c` 由 3572 → **3577** 行，md5 变为 `6a69643a…`；`codegen.c`/`check_top.c`/`check_stmt.c` 与 v2 相同。⇒ **v3 下 `reclaim.c` 行号 ≥845 一律 +5，<845 不变**（§14.9.3 有映射表）。`m01999` 的 v2 口径、`m01841` 的 v1 口径均 **superseded**。**§18 起由 FREEZE v4 取代**（`reclaim.c ff42799b…`/3607 行、`driver.c 9aae9cb5…`/1216 行、`build-rf/extc 9fd0dcd8…`）；§14.9 的 v3 `+5` 映射随之作废。
>
> 本文件中 `reclaim.c` 行号分两段：**§0–§13（实测依据 pre-freeze 快照树）与 §14.1–14.8 为 v2**（§14 开头已标 superseded），**§14.9 起为 v3**。代码内容逐点核过（`storeStructCopy`、`pinDropRegion`、`releaseDead`、`thrUpd`、`roundOk`、`cgTargetIsParamSlot`、`TIER` 全部逐字命中）；未见语义回退。

**已做（§18）**：门禁 G8/G9/G4/G1、ASAN、`tests/reclaim-perf/gate.sh`、`gate-release.sh`、其余 `gate-*.sh`、冻结后正式重测（FREEZE v4）。**未做**：G10/G11 的独立重算（只核对文档）。故本文件结论分四档：**实测（pre-freeze 快照）**、**冻结静态复核**、**只读源码**、**存疑/未验证**。

## §1 结论摘要

| # | 检查项 | 结论 | 证据 |
|---|---|---|---|
| Q1 | 计数不变式（+1 唯一入口、-1 唯一下降点、NoHold 是否绝迹计数档） | **OK**，冻结树行号 §2；批量搬移旁路已由 `storeStructCopy` 补上 | §2 |
| Q2 | 级联释放判据（occ/hold/SAT/跨 region/预算/realloc 失败） | **OK**，失败方向全是“晚回收”；新增 release 信用结算（`releaseDead:2959`）解释 gate-release D 红 | §3 |
| Q3 | pin 与级联（pinAdd 不改 hold、默认档可达性） | **OK**；**“pin 默认不可达”不成立**（`TY_REF` 变量经判据 iv 被当堆槽）；冻结树 `pinDropRegion` 缩小泄漏面 | §4 |
| Q4 | 任务根 push/pop 配平（正常/return/break/continue/try/trap/协程） | **配平 OK**（break/continue 修复已复核）；5 条语义缺口（§5.4） | §5 |
| Q5 | `EXTC_TASK_ROOTS=0` / `EXTC_RC_SWEEPFREE=0` 是否逐字节回退 | **部分**：只对“未升级”程序 byte-identical；对自动事件档程序 `=0` 翻档，属预期 | §6 |
| Q6 | `EXTC_RC_TIER` 组合表 | 冻结树四档（0 不发射/1 事件/2 清扫/3 计数+sweep）；第一段 currepo2 的两档标签错已修 | §7 |
| P0-7 | 参数槽覆盖修复 | **通过**（insert-only；`codegen.c:8943-8967`）；no-count 档不受影响 | §9.1 |
| P0-7b | 批量搬移会计（copyInto） | **冻结树已落地并静态复核通过**（`storeStructCopy`，`reclaim.c:2153-2179`）；待 `AUDIT GO` 后按 G14 正式复测 | §9.2 |
| LEAK-1 | copyInto 会计把目标 region 永久 `noSweep` | **静态已修**（可证 base region 时不置 `noSweep`）；残留风险=dst 在栈/pool view 时仍回落 `storeStructCons` ⇒ `noSweep`，待 GO 构造最小例 | §9.3 |
| L10-a | pool 槽里写引用 ⇒ 永久 pin ⇒ 无界泄漏 | **实测确认，无 UAF**；`pinDropRegion`（`reclaim.c:2043-2053`，`regionFree:2358` 调用）使“对象所在 region 每轮死亡”的形状不再泄漏；按 Lead 决定记已知限制（G15 XFAIL） | §9.4 |
| **M_MAX** | **全文新增：内存墙契约 `EXTC_RC_M_MAX`** | 触发线被夹到 `M_max - live/4`；未设置时隐式 `3×live`；≥7/8 墙时 4× 步内工作（`PRESSURE_MUL`）；`live==0` 首轮前无 cap | §10 / §13 |
| **DOC** | 对 `docs/analysis/MEMORY-MODEL-CURRENT.md` §18.5/§19 的静态复核（Lead `m01999` 追加） | 骨架/§17.2 默认值/§19.2 数字正确；**A 类 7 条**：`≤22µs` 算式错、`36µs` 低估 4–5×、`markResume` 盲区已修、`pressure` 漏判据、`thr` 公式不可求值、行号整体漂移、墙示例数字不可追溯 | §14 |

## §2 Q1：级联计数的 +1/-1 不变式

### 2.1 +1 的入口（计数档，冻结树行号）

| 入口 | 位置 | 语义 |
|---|---|---|
| `extc_rc_initRef(slot,v)` | `reclaim.c:1796-1802` | `*slot=v; inc(v);`，不读旧值（声明初始化） |
| `extc_rc_storeRef(slot,v)` | `reclaim.c:1813-1821` | shade → 读 old → shadeOld → 写 → `inc(v)` → `dec(old)`（1815-1820） |
| `extc_rc_storeRefOwnedReg` | `reclaim.c:1838-1877`，计数行 `1871-1872` | `if (sv) holdUpdSlab(sv,v,+1); if (sold) holdUpdSlab(sold,old,-1);` |
| `extc_rc_storeRefOwned` | `reclaim.c:1906-1947`，计数行**精确 `1941-1942`**（`grep -n` 确认） | `if (sv) holdUpdSlab(sv,v,+1); if (sold) holdUpdSlab(sold,old,-1);` |
| `extc_rc_storeRefSlot` | `reclaim.c:2109-2116` | slot 地址可归到 rc slab ⇒ 转 `storeRefOwnedReg`；否则 `storeRefUnknown` |
| `extc_rc_storeStructCons` | `reclaim.c:2124-2141` | **只 inc，不 dec**（`hostKnown=0` 时置 `rv->noSweep=1`，行 2139） |
| `extc_rc_storeStructRooted` | `reclaim.c:2192-2201` | **只 inc**，不置 `noSweep` |
| `extc_rc_storeStructCopy`（新） | `reclaim.c:2153-2179` | 批量搬移：base 可证宿主 region ⇒ 逐引用 word `rsInc`（跨区时另记 `foreign`）+ `inc`，**不置 noSweep**；不可证 ⇒ 回落 `storeStructCons(dst,size,0,0)`（2161，保守） |

`initRefNoHold`（`reclaim.c:1805-1810`）只写槽不 inc。其发射点被 `planRcNoDec` 门控：`codegen.c:7853` 只有“本函数动态范围无 dec”才用 NoHold，注释明确“NoHold 会让 cascade 的 -1 缺一次 +1”。**计数档里 NoHold 绝迹**（纯读推导，未逐函数跑覆盖；间接实测：§8 的强制计数档程序全部通过）。

### 2.2 -1 的下降点（全域唯一）

- cascade 子对象递减：`reclaim.c:1615-1617`
  ```
  if (chs->hold[chi] == 0 || chs->hold[chi] == EXTC_RC_HOLD_SAT) continue;
  chs->hold[chi]--;
  if (chs->hold[chi] == 0) (void)extc_rc_cascadePush(r, chs, chi);
  ```
- `extc_rc_holdUpdSlab`（`reclaim.c:1649-1676`，另有 NOCOUNT 桩 `1638-1646`）与 `extc_rc_holdUpd`（`1663-1676` 附近）：先 `occ` 位判活（1651/1666），`delta<0` 时 `hold==0||SAT` 直接 return（1655-1658 / 1667-1672），否则 `--` 归零即 `freeSlotCascade`（1663）。
- `inc/dec` 只是 `holdUpd(p,±1)` 的薄包装（`1678-1679`）。**没有第三条 -1 路径**（`grep hold\[.*\]--` 只命中 1616）。

结论：**每个 -1 都有唯一对应的 +1 站点**（`storeRef*` 先 inc 新值、再 dec 旧值；cascade 的 -1 作用于“子对象被父对象释放时撤掉父对象曾加的 +1”）。冻结树里批量搬移旁路已由 `storeStructCopy` 补上（§9.2），不再有例外。

### 2.3 旁路审计面

| 旁路 | 位置 | 现状 |
|---|---|---|
| `copyInto` 的 `memmove` | `codegen.c:1297` + 会计行注释 `1298-1316` + 发射 `1317-1319` | **冻结树已修**：改发 `extc_rc_storeStructCopy`（§9.2） |
| `poolResize`/`poolResizeRaw` | `codegen.c:4807-4816` 只发 `extc_pool_resize`（复合字面量单次 store） | **不需要每元素 inc**（搬迁只有一份指针；且无 dec），Lead 分析与本人最小例一致（§9.4） |
| `extc_any_box` 的 memcpy | `reclaim.c` 生成文本 `~15171/15175`（旧口径，冻结树未重定位） | **未验证**：含引用 payload 是否计数未测，列第二段候选审计面 |
| 参数槽重赋值 | `codegen.c:8943-8967`（判定函数 `8691`） | P0-7 已修为 insert-only（§9.1） |

> 注：`storeStructCopy` 的“不可证”分支仍置 `noSweep`（`storeStructCons:2139`），所以旁路会计的**守恒性**在“dst 为栈/pool view”时仍不成立——这是 §9.3 的残留风险，不是计数不变式的反例（那个方向只会多保留，不会缺 -1）。

## §3 Q2：级联释放的判据

1. **只释放活对象**：`holdUpdSlab` 先判 `occ`（`reclaim.c:1651`，注释 `/* free slot: not a live object */`），释放后 `freeSlot` 清 occ ⇒ 环只被访问一次（`cascade` 主循环约 `1546-1581`）。
2. **SAT 粘住**：`delta>0` 时到 `EXTC_RC_HOLD_SAT=65535` 不再加（`154`、`1655-1658`）；`delta<0` 时 SAT 值不递减（`1667-1672`）。饱和对象永不 cascade，只等 sweep——安全方向。
3. **realloc 失败**：`cascadePush`（`1567`）返回 0（注释“子对象保留计数，等 sweep”）；`freeSlotCascade`（`1581`）在 push 失败时**直接 `freeSlot`**——父对象已被判定死亡，子对象计数滞留在 sweep 回收，不早释放。
4. **无 C 递归**：cascade 用 region 拥有的 `cstk`（`intptr` 对存 slab+slot），深度不消耗 C 栈。
5. **无描述符的保守槽**：`EXTC_RC_CONSERVATIVE` 下不遍历子对象，子计数滞留 ⇒ 等 sweep。
6. **定向释放 `extc_rc_releaseOld`**（`reclaim.c:2864-2932`）：
   - 阶段 1 从“其他根 + 跨区种子”标记（与 collect 同一 seed 集），**未排空则整次放弃**（`budget==0` break，注释“unmarked 不证明死亡”）；放弃时 `thr=0; roundVisitedBytes=0;`（`2887-2888`）。
   - 阶段 2 从 old 下降，遇 marked 即停（“另一个根还指着它”）；下坡预算耗尽同样 `thr=0; roundVisitedBytes=0;`（`2924-2925`）。
   - 结束 `extc_rc_shadeNextGen()`（`2928`）+ 逐 slab `memset(mark)`（`2930`）——符合“mark 位只能由 sweep 清”的约定。
   - **单一预算** `EXTC_RC_RELEASE_BUDGET`（`975`，注释 `969-974`，默认 2048，~17.5ns/槽、最坏 <50µs），阶段 1/2 共用。
7. **调用点三重 guard** `extc_rc_releaseDead`（`2950-2960`）：`home->noSweep || home->owe` 直接 return；`s->region != home` 跳过；`budgetOk(home)` 失败跳过。`budgetOk`（`2296-2301`）=`allocBytes - visitedBytes >= max(live, floor)` ⇒ 每次 O(live) 遍历由“上次遍历后的新分配”付费，防 O(N²)。
   - **新增结算（契约变更）**：`releaseOld` 返回后立即 `home->visitedBytes = home->allocBytes;`（`2959`）⇒ 一次定向释放把“本轮已花费的扫描”记入信用账。这就是 `gate-release.sh` D 项（release_slots>0）在 `M_MAX` 夹紧后变 0/0 的静态原因：清扫更频繁 ⇒ 每轮把 visitedBytes 记到 allocBytes ⇒ 信用被吃光 ⇒ `budgetOk` 失败 ⇒ 定向释放不再触发。语义上仍是“优化不是必需”，不构成 soundness 问题（见 §13）。
8. **释放后不二次结算**：`storeRefOwnedRelease`（`2963-2969`）先 `releaseDead` 再 `storeRefOwned`；旧槽已 free ⇒ `rsDecSlab/holdUpdSlab` 在 occ 行返回。

补充：**release 信用与 round 信用已分离**。`budgetOk`（`2296-2301`）只看 release；round 起始用 `roundOk`（`3151-3157`，注释在 `3148-3150`）：`need = max(live,floor); if (pressure(r)) need /= EXTC_RC_SWEEP_PRESSURE_MUL; return allocBytes - roundVisitedBytes >= need;`——注释言明两者若共用一个账，release 会饿死 round start。

结论：**判据实现与注释一致，所有失败方向是“少回收/晚回收”**。未验证：`releaseOld` 的 msp/scanObj 与 `SCAN_CHUNK`（`2544`，默认 8192）计费在正式重测时是否仍守恒（第二段）。

## §4 Q3：pin 与级联

- `extc_rc_pinAdd`（`reclaim.c:2006-2026`）：只操作 pin 表（`pins[h].n++` / 插入；`pins[h].rg = ps ? ps->region : 0` 在 `2021`），**不碰 `hold`**；`!v || !owns(v)` 返回 1（无需 pin）。
- `extc_rc_pinSub`（`2028-2041`）：`n--` 或 erase，**不碰 `hold`**。
- `extc_rc_pinSeed`（`2060-2068`，noinline）：把本 region 的 pin 值作为 mark 根；`pinN==0` 快路径。
- 唯一 callsite：`extc_rc_storeRefUnknown`（`2072-2082`）：
  ```
  if (old) extc_rc_pinSub(old);
  if (!extc_rc_pinAdd(v)) { ExtcRcRegion *r = extc_rc_regionOfOwn(v); if (r) r->noSweep = 1; }
  extc_rc_storeRef(slot, v);
  ```
  ⇒ 必经 `storeRef` 的 inc（计数档）；**pin 失败才 noSweep**。
- **`pinDropRegion`（冻结树新增，`2043-2053`）**：若 `pinN==0` 直接返回；否则加锁扫 pin 表，`pins[i].rg == r` 的项 erase。`regionFree`（`2339` 起）在 abort 后调用它（`2358`，注释“region is dying: objects of it in the pin table are not needed”）⇒ **被 pin 对象所在 region 死亡时，陈旧的 pin 表项会被清掉**。这缩小了 L10-a 的泄漏面：只有“被 pin 对象所在 region 活过 give”的形状才泄漏（pool 例里 `new node` 落在主帧 region，故仍泄漏，§9.4）。
- **“默认档不可达”不成立**：判据 (iv) 的宿主判定 `acBaseIsHeap`（`check_top.c:6468-6486`）对 `EX_IDENT` 只看静态类型 `TY_REF` 就判“堆”；`var q: mut ref item = ref s[0]; q.r = ...` 因此通过 (iv)，默认自动升级事件档（实测 `EXTC_DBG_ACYCLIC=1` 显示 `main fn=1 reason=ok`、产物 `EXTC_RC_TIER 1`），产物里 `storeRefUnknown` 被调用。tier1 无 sweep ⇒ pin 表只占内存（覆盖时 pinSub 删除），界 = 同时存活被 pin 槽数。
- 结论：**pin 不参与级联计数，失败方向是保守（noSweep/少回收）**；`pinDropRegion` 把“region 死亡”作为 pin 表的兜底清理点。剩余风险是 §9.4 的 L10-a：region 存活但 pool 块被 give ⇒ 陈旧 pin 成为永久根。

## §5 Q4：任务根登记 push/pop 配平

### 5.1 发射点（4 处）

| 位置 | 形状 | 条件 |
|---|---|---|
| `codegen.c:3037` | `&__extc_dtidN`（domain 任务） | `exExt` 带 zone |
| `codegen.c:7572` | `&h.task`（boxed handle） | 非全局 |
| `codegen.c:7591` | `&__extc_czidN`（spawn 局部） | `!g->coroFunc` |
| `codegen.c:7903` | `&nm.task`（ST_VAR 局部 coroutine handle） | `isGlobal` 跳过 |

命名：`g->taskRootSeq=0`（`9855`），`trk=seq++`（`6220`，push 定义 `6218`）⇒ **per-function 唯一**，跨块重名不会冲突。

### 5.2 pop 路径（修复后）

- `cgTaskRootPopAt(CG*, int lvl, bool atBlockEnd, bool clear)`（`codegen.c:6241-6262`）：
  - `domBodyLevel==lvl && atBlockEnd` ⇒ 延迟到 `ST_DOMAIN` 后（判断 `6243`；延迟 pop 发射 `8801`）；
  - **只有 `clear` 才清 `v->len=0`**（`6254`），注释明说 break/continue 的 release 不许清表。
- `cgTaskRootPopsForReturn`（`6264-6276`）：只发 pop、不清表。
- `cgReleaseLevel` 调 `cgTaskRootPopAt(g, lvl, g->atBlockEnd, g->atBlockEnd)`（`6297` / `6303`）⇒ 真块尾才 clear。
- 正常 return：`cgReturn`（`5627` / `5644`）在 `goto __extc_ret;` 前 `genReturnDrops` + `cgTaskRootPopsForReturn`。
- `?` 失败路径：非 main 分支 `cgReturn(g, genTryFail(&ti))`（`5706`）⇒ 同样发 pops；main 的 `?` 走 `extc_die(1)`，进程退出无需 pop。
- trap：进程退出，无需 pop。
- 协程 step：**不 push**（`7591` 条件 `!g->coroFunc`）。
- probe 路径：`6976` 发一次 `cgTaskRootPopAt(..., true, true)`。

实测复核（currepo2）：`/tmp/t4.extc`（while 体内 `var c = counter(2)` 需 zone + `if j==1 { break }`）产物有 push、break 路径 pop、块尾 pop，**配平**（修复前块尾 pop 缺失）。

### 5.3 运行期

- `extc_taskRoots[64]`、表满 `rootPush` 返回 0 且**静默不登记**（`coroutine.c:69-74`）；`rootPop` 盲减。
- `extc_task_collect`（`78-96`）：遍历 zone，`depth==0` 且无登记槽命中 ⇒ `extc_task_end` 该 zone 所有 task + `extc_pool_zoneFreeRoot(zi)`。读的是 `*extc_taskRoots[i]`（**裸栈指针**）。

### 5.4 语义缺口（不是本轮 patch 的配平 bug，需文档/后续处理）

1. **全局 coroutine handle 不登记**（`codegen.c:7903` 注释“Globals are skipped (static storage, no scope exit to pop at)”）⇒ 只有全局 handle 的活任务会被 `collect()` 判不可达并回收（含仍在运行的任务）。
2. **容器/聚合/struct 字段里的 handle 不登记** = 调用者契约（`codegen.c:7575-7583` / `7897-7903`；`stdlib/std/sys/pool.extc:38-46` collect 注释“调用者的一句断言”）。
3. **表满 64 槽静默降级**：第 65 个 root 起不登记 ⇒ 只能靠调用者不 collect，否则误杀活任务。无 warning。
4. **handle 变量重赋值不 end 旧任务**：全仓无“赋值时 `extc_task_end`”发射（`6113/6151/9591` 只有块尾/显式路径）⇒ 旧任务只有在 `collect()` 或块尾再次被驱动时才 end，否则泄漏（与 L10 同族）。
5. **pop 失配时 `collect` 读已死栈槽**：`coroutine.c:84` 的 `*extc_taskRoots[i]` 无存活期检查；静态配平是唯一保证（现实为保守多留/UB 风险）。

## §6 Q5：回退开关是否 byte-identical

环境名与产物宏不同：env `EXTC_RC_SWEEPFREE`、产物宏 `EXTC_RC_SWEEP_FREE`；env `EXTC_RC_NOCOUNT`、产物宏 `EXTC_RC_NOCOUNT`。冻结树开关定义：`codegen.c:14057-14064`（RECLAIM，getenv 在 `14062`）、`14088-14095`（TASK_ROOTS，getenv `14093`）、`14096-14104`（NOCOUNT，默认 `g->rcNoCount = 1` 在 `14071`）、`14105-14123`（SWEEPFREE 三态：getenv 在 `14119`；`unset`⇒0 交分析、`"0"`⇒-1 强制关、其它⇒1；`14122` `if (g->rcSweepFree == 1) g->rcNoCount = 0;`）。

| 对照 | 结果 | 判定 |
|---|---|---|
| 清扫档程序（`uaf_param_overwrite`）默认 vs `EXTC_RC_SWEEPFREE=0` | `cmp` **完全一致** | 未升级时该开关是 **no-op**（符合“=0 只是禁止升级”） |
| 事件档程序（`acyclic_heapref`）默认 vs `EXTC_RC_SWEEPFREE=0` | 223 行差异，翻成 `TIER 2 nc1 sf0` | **不是 no-op**：=0 强制退回清扫档，属预期（A/B 需要），但不能声称“逐字节回退” |
| `EXTC_TASK_ROOTS=0` vs 默认（`tests/zone/collect_with_live_task.extc`） | 只删 3 行：`int64_t __extc_trk0 = extc_task_rootPush(&h.task);` + 两条 `if (__extc_trk0) extc_task_rootPop();` | 行为等价于无登记路径；**≠ HEAD 整文件**（新运行期文本恒在） |

补充：生成 C 里 return 路径有一条 pop 落在 `goto __extc_ret;` 之后（不可达死代码）。**不是双弹**，只是产物美观问题，建议 compile-facts 顺手删（低优先）。

结论：回退开关的“逐字节”只能按**同一编译器、同一程序**比较；`SWEEPFREE=0` 对自动事件档程序是**语义性关闭**，文档必须写清，否则会被当成 bug 或当成可回退证明。

## §7 Q6：`EXTC_RC_TIER` 组合表

自动化判据（冻结树 `codegen.c:15918-15921`，`if` 在 `15918`、body `15919-15920`；`unitAllAcyclic` 定义在 `14309`）：
```
if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) {
    g.rcSweepFree = 1;
    g.rcNoCount = 0;
}
```
优先级：显式 `EXTC_RC_SWEEPFREE=1` 也强制 `rcNoCount=0`（`14125` 附近）；显式 `=0` 设 `-1`，**跳过**自动升级（条件 `!= -1`）。

currepo2 实测（注意：currepo2 的 tier 发射是两档逻辑，`NC0+SF0` 会写 `TIER 2` 但计数其实是开的）：

| 程序 | nc | sf | TIER | nc 宏 | sf 宏 |
|---|---|---|---|---|---|
| 非无环 | U | U | 2 | 1 | 0 |
| 非无环 | U/0 | 0 | 2 | 1 | 0 |
| 非无环 | 任意 | 1 | 1 | 0 | 1 |
| 无环(自动) | U | U | 1 | 0 | 1 |
| 无环(自动) | U | 0 | 2 | 1 | 0（强制关同时跳过升级） |
| 无环(自动) | 0 | U | 1 | 0 | 1 |
| 无环(自动) | 0 | 0 | 2 | **0** | 0（currepo2 标签错：计数开却写“counting off”） |
| 无环(自动) | 0/1 | 1 | 1 | 0 | 1 |
| 无受管分配（hello） | 任意 | 任意 | 无 TIER 行 | — | — |

**冻结树已修**：`codegen.c:12623-12631` 改为四档（`12625-12628` 判定：`nc==0&&sf==1`⇒1；`nc==1&&sf!=1`⇒2；`nc==0&&sf!=1`⇒3；否则 2；`bufPrintf #define EXTC_RC_TIER` 在 `12629-12631`）。⇒ `NC0+SF0` 正确写 `TIER 3`（=counting+sweep）；第一段 currepo2 的两档标签错**只作为历史**，最终文档以四档为准。第二段用冻结树重跑本表，逐行确认。

## §8 UAF 复现（BUG-A / BUG-B）与 HEAD 步骤

### 8.1 文件与判据

| 文件 | 形状 | 期望输出 | 计数档修复前 |
|---|---|---|---|
| `tests/gc-p0/src/uaf_param_overwrite.extc` | 参数 `p = new node` 覆盖 | `alias check b.v=999` | `alias check b.v=111`（a、b 同槽） |
| `tests/gc-p0/src/uaf_bulk_copy.extc` | 经 `ref s[0]` 写 ref 字段 + `copyInto` | `d0r=7` | `d0r=555` |
| `tests/gc-p0/src/uaf_bulk_copy_default.extc` | 同上，另证默认档可达 | default 与 `NOCOUNT=0` 都是 `d0.v=7` | 两者都 `d0.v=555` |

运行（`/tmp/currepo2` 或 `/tmp/currepo3` 均可，后者 copyInto 半落地除外）：

```
./build/extc build tests/gc-p0/src/uaf_param_overwrite.extc -o /tmp/t && /tmp/t
EXTC_RC_NOCOUNT=0 ./build/extc build tests/gc-p0/src/uaf_param_overwrite.extc -o /tmp/t && /tmp/t
```
强制事件档：`EXTC_RC_SWEEPFREE=1`（等价于 `NOCOUNT=0` 且关 sweep）；**不要**用 `EXTC_RC_SWEEP_FREE` 作为环境名（那是产物宏名，env 不认）。

HEAD pre-existing 对照：

```
rm -rf /tmp/headrepo && mkdir -p /tmp/headrepo
cd /home/alphayang/extC_Compiler && git archive 85b8871b | tar -x -C /tmp/headrepo
cd /tmp/headrepo && make -j8
EXTC_RC_NOCOUNT=0 ./build/extc build tests/gc-p0/src/uaf_param_overwrite.extc -o /tmp/h && /tmp/h
```

已实测：HEAD 上三例同样复现（`b.v=111` / `d0r=555` / `d0.v=555`）⇒ **pre-existing，不是本轮 patch 引入**。

### 8.2 可达性判定（Lead 要求的“严重性定级”）

- **BUG-A（参数槽覆盖）**：判据 (iv) 原文（`check_top.c:6181-6186`）：
  > “every place that can **drop a reference** is a **counted rc slot**: the function makes no non-declaration-initializing assignment to a reference-typed local or parameter …”
  
  参数重赋值正是“non-declaration-initializing assignment to a reference-typed parameter” ⇒ `noteAcyclicStore` 置 `ivBad` ⇒ `unitAllAcyclic` 不成立 ⇒ **默认档不会自动进事件档**，BUG-A **仅在显式 `NOCOUNT=0`/`SWEEPFREE=1` 时可达** ⇒ 严重度：计数档 soundness（非默认）。实测一致：`uaf_param_overwrite` 默认档为 T2，输出正确。
- **BUG-B（copyInto 批量搬移）**：写路径是 `ref s[0]` 后 `q.r = ...`；`acBaseIsHeap` 对 `EX_IDENT` 仅看 `TY_REF`（`6468-6486`）⇒ 该写入被当“堆槽”，(iv) 不拦截 ⇒ **默认档会自动进事件档**。实测（修复前 HEAD 与 currepo2 默认产物）：`EXTC_DBG_ACYCLIC=1` 显示 `main fn=1 reason=ok`、`EXTC_RC_TIER 1`，输出 555 ⇒ **默认路径可达，S0 级**。修复后分析侧 veto（`check_top.c:7806-7827`：`copyInto/poolResize/poolResizeRaw` + 元素含引用/提及参数 ⇒ `ivBad`）使该 unit 退回 T2，输出 7 ⇒ 默认档回到“保守但正确”。

## §9 P0-7 / P0-7b / LEAK-1 / L10-a

### 9.1 P0-7 参数槽覆盖修复：**通过**

- 判定函数：`codegen.c:8691` `cgTargetIsParamSlot(CG *g, Expr *e)`，按 C 名与参数表精确匹配（协程帧字段拼写作 `f->x` 不会误判，局部遮蔽会被 mangle 成 `a__2`）。
- 发射：`codegen.c:8943-8967`（`initRef` 在 `8959`；判定分支 `if (!g->rcNoCount && cgTargetIsParamSlot(g, s->u.assign.target))`），计数档且目标是参数槽时只发
  ```
  extc_rc_initRef((void **)&(p), (void *)(...new...));
  ```
  不再走 `storeRefOwned` 的 `if (sold) holdUpdSlab(sold, old, -1)`；注释自称“insert-only … At worst one reference is reclaimed later (the conservative side of soundness)”。
- 实测（pre-freeze）：`EXTC_RC_NOCOUNT=0` 产物里 clobber 体是 `extc_rc_initRef((void **)&(p), ...)`，无 `-1` 分支；三份 UAF 测试在 4 种构建（默认/NC0/NC0+SF1/SF1）全部输出期望值。**冻结树静态复核：分支与行号原样保留**（`8943-8967`），待 GO 在冻结编译器上重跑四档矩阵。
- 代价（实测）：无逃逸时无泄漏（新对象随 callee arena 释放，200k 轮 RSS 平台化）；**有逃逸且强制 `SWEEPFREE=1`（T1，无 sweep）时 ~20B/轮线性增长**（50k→200k→800k：2.6→5.9→19.2MB）。因判据 (iv) 拒绝参数重赋值 ⇒ 自动 T1 不可达，T3 的 sweep 兜住 ⇒ 可接受。

### 9.2 P0-7b 批量搬移会计：**冻结树已落地，静态复核通过（待 GO 复测）**

- 发射：`codegen.c:1297` 仍先 `memmove`（保留重叠语义），注释 `1298-1316` 说明会计语义，发射 `1317-1319`：
  ```
  if (g->needReclaim && typeHasRefSlots(g, elem))
      cgLine(g, "for (int64_t __extc_bk = 0; __extc_bk < n; __extc_bk++)"
                " extc_rc_storeStructCopy((void *)&d.data[__extc_bk], sizeof(%s), (void *)d.data);",
             cType(g, elem));
  ```
- **运行期原语已在冻结树落地**：`reclaim.c:2153-2179` `static void extc_rc_storeStructCopy(void *dst, size_t size, void *base)`：
  `bs = slabOfInterior(base)` ⇒ 宿主 region 可证时逐引用 word `shade → rsInc(跨区则另记 foreign) → inc` 且**不置 noSweep**（`2175`/`2177`）；不可证时 `storeStructCons(dst,size,(void*)0,0)`（`2161`）。注释 `2143-2152` 明说 base 在 rc 外（stack/pool view）回落的是保守方向。
- **第一段的历史中间态（已结束，仅存档）**：08:36 树上以 `/tmp/currepo3` 编译 `/tmp/l1k.extc`（copyInto 含引用元素）报
  ```
  build/l1k.c:3531:61: error: implicit declaration of function 'extc_rc_storeStructCopy';
  did you mean 'extc_rc_storeStructCons'? [-Wimplicit-function-declaration]
  ```
  ⇒ 当时产物不可链接。冻结树已不存在该符号缺失（`reclaim.c:2153` 有定义），第二段以冻结编译器复测链接与运行。
- 分析侧配套：`check_top.c:7806-7827` 的 veto 文案“moves a block of elements containing references; the acyclic analysis cannot see the byte move”，使这类 unit 默认退回 T2（保守方向）。
- **测试注释与当前行为不一致（D 类，建议 compile-facts 顺手改）**：`tests/gc-p0/src/uaf_bulk_copy.extc` 头注释仍写“默认档会自动进事件档（TIER 1）”；实际（currepo2/currepo3 实测）默认档因 veto 是 T2。`uaf_bulk_copy_default.extc` 的“默认产物自动进事件档”同理只在修复前成立。判据本身（输出 7）不变。

### 9.3 LEAK-1：copyInto 会计把目标 region 永久 `noSweep`（**静态已修，残留风险在 dst 为栈/pool view**）

- 旧机制（currepo2 实测口径）：会计行把 `owner=(void*)0, hostKnown=0` 传给 `extc_rc_storeStructCons`；冻结树对应行 `reclaim.c:2139` `else if (rv) rv->noSweep = 1;`；`noSweep` 只在 regionFree 随 region 消失，**全仓无清零点** ⇒ 目标对象所在 region 永久退出清扫。
- currepo2 实测（三档 T1/T2/T3 全部线性）：
  - `/tmp/leak1_8.tpl`（8B payload，200k/800k 轮）RSS：5.52MB/17.62MB（≈20B/轮）
  - `/tmp/leak1_64.tpl`（64B payload）：16.08MB/60.05MB（≈73B/轮 ≈ payload+~10B 槽开销）
  - 不调用 copyInto 的对照：2.64MB/2.83MB 平台化
- **冻结树修法（静态）**：`storeStructCopy`（`reclaim.c:2153-2179`）在 base 可证宿主 region 时**不再走 `noSweep` 分支**，而逐引用 word 记 `rsbit`+`inc` ⇒ G14 的形状（dst 也是 `new item[N]` 得到的堆 slice）静态应满足“800k ≤ 1.2× 50k”。
- **残留风险（需 GO 后构造最小例定级）**：dst 为**栈 view 或 pool view** 时，base 不在任何 rc slab 数据区 ⇒ 回落 `storeStructCons(dst,size,0,0)`（`2161`）⇒ 目标 region 仍被置 `noSweep`。若该形状可默认/计数档到达，则 LEAK-1 只修了一半（属 P0-7b 未闭合，不是新缺陷）。
- 判定更新：LEAK-1 从「修复中」改「**静态已修，待 G14 固化**」；G14 判据不变（copyInto 800k 轮 maxRSS ≤1.2× 50k 轮）；同时报残留风险的 RED 与否。

### 9.4 L10-a：pool 槽里的引用赋值 ⇒ 永久 pin ⇒ 无界泄漏（已知限制，本轮不修）

- 最小例 `/tmp/poolres.extc`：`var s: mut slice<item> = poolSlice<item>(rid,2); s[0].r = new node; s = poolResize<item>(rid,s,4); s[0].r = new node; ...; poolGive<item>(rid, s)`，循环 200k 轮。
- 结果：**无 UAF、无错误输出**；50k→800k 轮 maxRSS：T2 12.4MB→186.0MB（≈232B/轮）、T1 11.9MB→171.7MB（≈213B/轮）⇒ 三档（T1/T2/T3）全不回收。
- 机制（逐行，冻结树行号）：
  1. `s[0].r = new node` 的槽地址在 pool 缓冲里 ⇒ 生成 `extc_rc_storeRefSlot`（`reclaim.c:2109-2116`）；
  2. `extc_rc_slabOfInterior((void*)slot)` 对 pool 缓冲返回空（不在任何 rc slab 的数据区）⇒ 回落 `extc_rc_storeRefUnknown`（`2072-2082`）；
  3. `extc_rc_pinAdd(v)` 把对象登记为 **mark 根**，并记下其 region（`pins[h].rg`，`2006-2026`，`2021`）；
  4. `extc_pool_give`（`src/back/pools.c` 生成文本）只 `free(b)`、减字节账，**不调用任何 rc 通知 / `pinSub`** ⇒ pin 表项与其根语义保留；
  5. `pinSeed`（`2060-2068`）把它当根 ⇒ **T1/T2/T3 都不回收**（T2 的 sweep 也扫不掉）。
- **冻结树新增的收窄**：`extc_rc_pinDropRegion`（`2043-2053`）在 `regionFree`（`2339` 起，调用点 `2358`）死亡 region 时清掉 `pins[i].rg==r` 的项 ⇒ 若被 pin 对象落在**每轮都会死的 callee region**（例如 give 前已返回的函数帧 region），pin 不再永久泄漏。**仍泄漏的形状**：被 pin 对象落在**活过 give 的 region**（本最小例里 `new node` 在主帧 region）⇒ L10-a 成立，只是面变窄。
- 严重度：**无界泄漏，不是 soundness**（没有提前释放）。按 Lead 决定记为 **L10-a**，本轮不修；报告标注 **预期 RED · 已记录**（冻结树里由 empirics 做成 G15 XFAIL）。
- 修复方向建议（**P0-8，本轮不做**）：给 pin 表项加“来源区间”标签（写屏障已知宿主 address range 时打标；`poolGive`/`zoneFree` 时 `pinDropRange`），或改池级 epoch（每次 pool 生命周期一代，give 时整代撤销）。**判据：同形状 800k 轮 RSS ≤ 1.2× 50k 轮。**
- 边界：object-table 池（`extc_poolKind!=0`）的 resize/give 直接 trap（`pools.c` 文本），所以该形状只对“块池 + 元素带引用”成立。

## §10 清单外改动（不在任务书 P0 列表、但工作树相对 HEAD 存在）

| 改动 | 位置（冻结树） | 说明 |
|---|---|---|
| **`EXTC_RC_M_MAX` 内存墙** | 注释 `reclaim.c:892-905`；变量/arm/`mmaxEff` `908-941`；触发线夹紧 `thrUpd` `3102-3145` | **本轮新契约**：显式 >0 用该值，否则隐式 `3×live`（`live==0` 时无 cap）；`thr` 被夹到 `M_max - live/4`（低于 floor 时用 floor + 一次性 stderr `[mmax] memory wall unreachable`）；见 §13 |
| **`pressure()` + `EXTC_RC_SWEEP_PRESSURE_MUL`** | `reclaim.c:945-960`、`977-988`（默认 4，范围 1..16，越界 `#error`） | 压力判据 `bytes>=m-m/8` 或 `thr>0 && bytes>thr*4`；压力下每步做 4× 工作（sweep words / mark objects 有硬上限），用工作换频率 |
| **`roundOk`（round 信用）** | `reclaim.c:3151-3157` | 与 `budgetOk` 分离；`need=max(live,floor)`，压力下 `/= PRESSURE_MUL`（注释 `3148-3150`：共用一个账会饿死 round start） |
| **`releaseDead` 信用结算** | `reclaim.c:2950-2960`（结算行 `2959`） | 定向释放后 `visitedBytes=allocBytes` ⇒ 解释 gate-release D 0/0（§3.7、§13） |
| `EXTC_RC_RELEASE_BUDGET` 默认 2048 | `reclaim.c:975`（注释 `969-974`） | 定向释放单一预算 |
| `EXTC_RC_SCAN_CHUNK` 默认 8192 | `reclaim.c:2544`（旧文档口径 65536 已过时） | TODO 注释 `2539-2542`：65536 时 collect 3.6×、pd step 时间 6.4×；**未验证**是否应为默认 |
| `EXTC_RC_REG_FULL_POLICY=1`、`EXTC_RC_REG_MAX=512` | `codegen.c:12699-12723` | 表满一次性 stderr 警告；“不进入新 region、永不 drop”，泄漏优先于早释放 |
| `EXTC_RC_TIER` 宏 | `codegen.c:12623-12631` | 四档（含 tier 3） |
| **`extc_rc_storeStructCopy`（新原语）** | `reclaim.c:2153-2179` | 冻结树**已落**（第一段时“待落”，已过时） |
| **`extc_rc_pinDropRegion`** | `reclaim.c:2043-2053`，`regionFree:2358` 调用 | L10-a 的收窄（§9.4） |
| 任务根登记 `EXTC_TASK_ROOTS` | `codegen.c:14088-14093`、`6218-6276` | P0-6；默认开 |
| 级联 `cascadePush/freeSlotCascade/holdUpdSlab` | `reclaim.c:1567/1581/1649-1676` | P0-5 |

## §11 未验证 / 存疑（第二段清单）

1. **P0-7b 残留形状**：`storeStructCopy` 已落，但“dst 为栈/pool view ⇒ 回落 `storeStructCons` ⇒ `noSweep`”分支是否可默认/计数档到达，未构造最小例；G14 只覆盖“dst 在 rc 存储内”。
2. **`M_MAX` 的运行时行为**：静止墙、`floor` 兜底 warning、隐式 `3×live`、压力乘子 4× 的实际内存上界与停顿代价**全部未实测**（只有静态阅读）；第二段用 after 套件核 `mmax` 统计行。
3. **`extc_any_box` 的 memcpy**（`reclaim.c` 生成文本 ~15171/15175，冻结树未重定位）：是否把含引用 payload 复制成第二份指针而不计数。未构造最小例。
4. **SCAN_CHUNK=8192 vs 65536**：`2539-2542` 的 TODO 是性能/停顿权衡，未验证默认 8192 是否最优；报告不据此改文档。
5. **任务根表满 64 的静默降级**：是否应 warning / 拒绝 collect（见 §5.4-3）。
6. **`collect()` 对全局 coroutine handle**：`extc_task_collect` 会把仍活的 depth==0 zone 判不可达（`coroutine.c:78-96`），仅当有非全局登记槽时幸免。是否属已知设计（调用者契约）待 Lead 确认。
7. **门禁/ASAN 全部未跑**：本报告不含 G8/G9/G4/G1 数字；第二段执行。
8. **pop 失配时读死栈槽 UB**（`coroutine.c:84`）：静态配平已复核，动态失配路径未构造。
9. **`releaseDead:2959` 的语义**：`visitedBytes=allocBytes` 是“信用结算”还是“有意抑制定向释放”，需 Lead/作者确认设计意图（本报告只证明它能解释 D 红）。

## §12 冻结后待做（第二段）

0. ~~静态复核冻结树~~：**已完成**（行号已按冻结树重定位，md5 复核一致；未跑程序，理由见 §0.1）。
1. 用冻结树重新 `tar` 构建 `/tmp/frzrepo`（**不复用 currepo3**），零 warning 复核。
2. `EXTC_RC_TIER` 四档矩阵重跑（§7 表），确认 `NC0+SF0 ⇒ TIER 3`。
3. P0-7b 正式复核：`grep` 产物确认 copyInto 会计调用 `storeStructCopy`、参数槽无 `storeRefOwned` 的 `-1`；运行三份 UAF 测试四档全绿。
4. LEAK-1/G14 复测；L10-a/G15 XFAIL 复测（预期 RED）；新增“dst 为栈/pool view”的 LEAK-1 残留形状最小例（§11-1）。
5. `M_MAX`/压力乘子的内存上界与停顿实测（§11-2）。
6. 门禁 G8/G9/G4/G1 + ASAN（至少 G8/G9 一次）；**先等 Lead 的 `AUDIT GO`**（empirics 正跑 after 全套，避免污染 max/RSS）。
7. 把本文件 §1 表格的“待复测”改为终值，补“门禁是否绿”。

## §13 FREEZE 契约变更与红项判定（126/2 与 2/4）

Lead 已证因果（`m01841`），以下只记判定口径，供最终交付物引用。

### 13.1 `tests/reclaim/run.sh` 126/2

- **现状（先读这条）**：`tests/reclaim/driver.c`（08:46:58 改写后）**已无 `:838/:839` 两条断言**；旧文本只作注释保留在 `driver.c:879`。新契约是 `driver.c:893`（`s2 <= s1 + 8`）、`895`（默认档 `s2 >= s1 + 3`）、`897-898`（`p ≤ m + EXTC_RC_SLAB_SIZE`）与 `wallPeak` 的 `:872`（`peak ≤ mmax + SLAB_SIZE`）。⇒ 126/2 是**断言改写之前**的运行记录；冻结树 v2 上重跑应以新断言为准（待 `AUDIT GO`），详见 §14.2。
- 直接原因：测试用 `EXTC_RC_M_MAX=1099511627776`（1 TiB，等效关闭内存墙夹紧）⇒ 夹紧解除后清扫次数随规模增长，128/0。
- **这是契约变更，不是回归**：`t_learning` 的新契约（`driver.c` 约 `862-899` 注释）：
  1. 峰值驻留 ≤ `M_max` + 一块 slab（`peak ≤ mmax + EXTC_RC_SLAB_SIZE`）；
  2. 清扫次数随规模**对数**增长（100× 只多 ≤8 次，底数≈2.75），**非常数**。
- 两档都要绿：默认档 / `M_MAX=1TiB`（关墙）档，分别覆盖“被动对数”与“墙驱动”两种形态。
- 建议改法：断言改写已落地，无需再改 driver.c；只需同步 `run.sh` 里按旧断言写死的期望计数（若仍存在），并补 §14.2 建议的“双配置 peak 对比”。

### 13.2 `tests/reclaim-gaps/gate-release.sh` 2/4

- 红 A：`清扫次数 ≤ 1`——同属旧契约（实测 10/42），应按 §13.1 的“不随 N 增长/对数”改判或退役。
- 红 D：`release_slots > 0` 变 `0/0`。静态原因：`M_MAX` 夹紧 ⇒ sweep 更频繁 ⇒ 每轮 `releaseDead` 把 `visitedBytes` 记成 `allocBytes`（`reclaim.c:2959`）⇒ `budgetOk`（`2296-2301`）不再给信用 ⇒ 定向释放不触发。`storeRefOwnedRelease` 仍在产物里（ring_per_round 生成物 :3660），只是不再命中。
- 判定：**定向释放是优化，不是必需**；信用不足时不触发是允许的。建议 D 改为“允许 0，但用一个能触发它的形状保留覆盖”（例如降低同时存活量/放大 `EXTC_RC_RELEASE_BUDGET`），而不是要求每次都 >0。
- 备注：A/D 都不构成 soundness 问题；**不要**为了变绿去改 `reclaim.c` 的 `M_MAX` 夹紧或 `visitedBytes` 结算（那会破坏内存上界契约）。

## §14 对 MEMORY-MODEL-CURRENT.md §18.5 / §19 的静态复核（Lead `m01999` 追加项）

> 行号口径：§14.1–14.8 中的 `reclaim.c` 行号是 **FREEZE v2**（md5 `4f927b4f…`，3572 行）；v3（md5 `6a69643a…`，3577 行）在 845-849 插入 5 行 ⇒ **≥845 的行号一律 +5**，映射见 §14.9.3。`codegen.c` / `check_top.c` / `check_stmt.c` 未变，引用无需重指。

对象：`docs/analysis/MEMORY-MODEL-CURRENT.md`（2026-11-09 08:56:41，977 行）。方法：逐条打开源码核对语义、常量、行号；只读，不改文档。结论分四档：**A 类 = 错误/自相矛盾（必须改）**、**B 类 = 表述含糊或过时**、**C 类 = 缺证据/不可追溯**、**D 类 = 与其他文档/自身口径冲突**。

统计：A 7 条、B 5 条、C 3 条、D 2 条。**骨架（五基质、三档判定链、§17.2 全部默认值、§19.2 pd 分段与标记步数字）核对无误**；错误集中在三类：公式不完整/绝对化、行号整体漂移、数字不可追溯。

### 14.1 §18.5 内存墙契约（`reclaim.c:892-903` 注释 + `thrUpd:3102-3145` + `pressure:944-956`）

| # | 文档断言（doc 行） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|
| A1 | `M_max = EXTC_RC_M_MAX`（未设/0 ⇒ 隐式 3·live；负数按 0），锚点 `reclaim.c:925` 的 mmaxArm（845 行） | 语义准确，锚点错位 | `extc_rc_mmaxArm` 定义 `reclaim.c:921-930`（925 行只是函数体内一行）；隐式 3·live 在 `extc_rc_mmaxEff:935-941`（`return r->live > 0 ? r->live*3 : 0;`，首轮前无 cap） | 锚点改 `reclaim.c:921-930`；补“`r->live` 是上一轮清扫后的存活字节（`reclaim.c:3310`）” |
| A2 | `thr = min( max(2·live, floor)·scale , M_max − 64·L/b )`（846 行） | **不可求值**：丢 `b=256`、`L` 未定义 | `reclaim.c:3113` 注释逐字 “the trigger line is also capped at M_max - 64*L/b (b = 256 => L/4)”；代码 `reclaim.c:3121` `int64_t _cap = _wall - r->live / 4;`；`r->live` 单位是字节（3310） | 写成 `thr = min(max(2·live, floor)·scale, M_max − live/4)`，括注源码记法 `M_max − 64·L/b, b=256` |
| A3 | `pressure = (bytes ≥ 7/8·M_max)`（847 行） | **漏一条判据** | `reclaim.c:944-956`：`if (m <= 0) return 0; if (bytes >= m - m/8) return 1; if (r->thr > 0 && r->thr < INT64_MAX/4 && bytes > r->thr*4) return 1;` ⇒ 即使 `M_max` 很大，`bytes > 4·thr` 也判近墙 | 补第二判据 `bytes > 4·thr`（`thr` 未溢出时）；否则读者以为 pressure 只在墙边出现 |
| B1 | “承诺：峰值驻留 ≤ `M_max + 缓冲`”（851 行） | 表述过强（经验界，非硬界） | 全仓 `extc_rc_mmax*` 只出现在 arm/eff/thrUpd/pressure/诊断（`reclaim.c:907,916,921-941,949,3117-3144,3256-3260`），**分配路径无拒绝/阻塞**；越墙靠 `PRESSURE_MUL` 加速清扫追上 | 改为“触发线契约：`thr ≤ M_max − live/4`、`≥7/8·M_max` 时每步预算 ×4；峰值实际落在 `M_max` 附近由 `wallPeak` 实测（`driver.c:872`），**不是**分配器硬上限”；“缓冲 = roundOk 信用”缺推导，应标“推导/未证明” |
| B2 | “代价：夹紧 thr 使 sweep 次数从小常数变成 O(log N)；活集要涨 ~2.75× 才再触发”，称引自 `reclaim.c:925` 上方注释（852 行） | 方向准确，锚点错位、注释内引用过期 | 该段注释在 `reclaim.c:892-903`（“an unbounded trigger threshold keeps the sweep count a small constant …, but pays with unbounded resident memory”）；2.75 的推导同段（`M_max-L/4 = 2.75L`）；`thrUpd` 自身注释 `3103-3115` | 锚点改 `892-903`；另 `reclaim.c:896` 引 `tests/reclaim/driver.c:838-839` 已失效（见 14.2），源码注释需同步（低危） |
| C1 | `[work] M_max=103125744 thr=35546880 pressure=1 mult=4`、`RSS 93080 KB`、`pressure_steps=1209`、`collects=13`、旧版 45.8 MB（853 行） | **不可追溯 + 字段语义未注** | 这些数字在 `MEASUREMENTS.md`/`MEASUREMENTS-GC-P0.md`/`MEASUREMENTS-TIER1.md` 均无记录（`grep 93080/103125744/pressure_steps` 只命中本文档）；`[work]` 的 `M_max=` 取 `extc_rc_mmaxUsed`（`reclaim.c:3258`，历史最大隐式墙），`thr=` 取 `extc_rc_thrNow`（最后一次 thrUpd），另有两字段 `M_max_now` / `M_max_implicit`（`3260`） | 补命令与原始 stderr 到 MEASUREMENTS-GC-P0；注明“`M_max` 是历史最大隐式墙（`M_max_implicit=1`），`thr` 是最后一次 thrUpd 的值”；否则“峰值落在 98.4 MiB 墙下”无法判定是当刻墙还是历史最大墙 |
| E1 | 墙不可达警告 `reclaim.c:3127`（854 行） | 准确 | `reclaim.c:3121-3127`：`_cap = _wall - r->live/4; if (_cap < floor) { _cap = floor; … fprintf(stderr, "[mmax] memory wall unreachable: M_max=%lld floor=%lld; using floor") }` | 无需改 |

### 14.2 driver.c 契约断言与“互斥性”论证

现状（`tests/reclaim/driver.c`，08:46:58，md5 `baaea9c2497e1fd39dbed3b227386671`）：
- 旧两条断言已不存在，旧文本仅作注释保留在 `driver.c:879`（“旧的「规模涨 100 倍…」与内存有界互斥”）。
- 新契约：`driver.c:893` `check(s2 <= s1 + 8, …100× 只多 ≤8 次…)`；`895` 仅当 `m2>0 && m2 < 1<<40` 时 `check(s2 >= s1 + 3, …)`；`897-898` `check(p1/p2 <= m1/m2 + EXTC_RC_SLAB_SIZE, …)`；`wallPeak`（`836-874`）在垃圾形状上 `check(peak <= mmax + EXTC_RC_SLAB_SIZE)`（`872`）。

**互斥性判定：方向成立，但不是逻辑互斥，而是“该自学习机制下的取舍”。**
- 成立的一半：阈值 `max(2·live,floor)·scale` 随空扫放大 ⇒ 清扫次数近似常数、`live+garbage` 无界（`reclaim.c:896-899` 与 `driver.c:879-886` 表述一致）；夹紧到 `2.75L` ⇒ 峰值有界、次数 `O(log_2.75 N)`（`driver.c:883` 实测默认档 4→9、关墙档 2→3）。
- 不成立的一半：理论上可另设机制同时满足“峰值有界 + 次数常数”（固定阈值 + 更细粒度回收），所以“互斥”须限定为“在自学习阈值机制下”；文档/注释宜写“取舍/tension”，不写无条件互斥。
- 门禁判别力弱化（建议补）：
  1. `t_learning` 两条 peak 断言近乎恒真（全存活形状 `peak ≈ live`，而 `m = mmaxEff = 3·live` ⇒ `peak ≤ 3live+slab` 必真）；真正判别墙的是 `wallPeak`。
  2. `wallPeak` 只断言上界，且 `mmax` 取自 `extc_rc_mmaxEff(r)`（`868`）——若墙在实现上退化为“永远很大”，断言同样恒真。建议加下界或双配置对比：默认档 `peak ≤ M_max+slab`，显式 `EXTC_RC_M_MAX=64MiB` 档要求 `peak` 明显小于关墙档（如 `peak(M_MAX=64MiB) < peak(默认)/2`），并打印两档 peak。
  3. 关墙档（`M_MAX=2^40`）下 `895` 的 log 增长断言被 guard 跳过、peak 断言因 `mmax=1TiB` 恒真 ⇒ “两档都必须绿”实际只有默认档有判别力；关墙档应补“次数不随规模线性增长”（如 `s2 <= 2*s1`）或明确该档只守“不误杀”。
  4. `driver.c:881-882` 注释写 `M_max-64*L/b = 2.75*L`（b=256）——公式对，但同样未写 `L` 是字节；与 A2 一并修。

### 14.3 §17.2 / §19.1 行号漂移（值全对，行号须整体重指）

§17.2 表（doc `740-775`）逐行映射 → v2 精确行号（`reclaim.c`，生成器源码行）：742→138；743→141；744→144；745→151；746→154；747→**321**；748→**322**；749→**326**；750→**327**；751→**330**；752→**352**；753→211 ✓；754→**417**；755→**707**；756→**965**；757→**1000**；758→**1003**；759→**1006**；760→**1009**；761→**1012**；762→**1015**；763→**1018**；764→**1207**；765→975 ✓；766→985 ✓；767→1554 ✓；768→**1220**；769→**1263**；770→**1375**；771→**1731**；772→**2517**；773→2544 ✓；774→`codegen.c:12415`→**12704**；775→`codegen.c:12126`→**12384**。

§19.1 表行：标记推进（863 行）源码列 `896/899/911/914/999/908` ⇒ **1000/1003/1015/1018/1207/1012**；清扫（865）`889` ⇒ **965**；`releaseOld`（866）函数体 `2864` ⇒ 定义在 **2859**（注释 2849-2858，函数体 2859-2935）；`regionFree`（867）上界注释 `2336` ⇒ **2331**（整段 2328-2347），探针 `3236` ✓、`abort 2318` ✓。

### 14.4 §19 数值与措辞错误

| # | 文档断言（doc 行） | 判定 | 源码/实测证据 | 建议修正 |
|---|---|---|---|---|
| A4 | 大对象扫描行：`P_step ≤ C_scan·SCAN_CHUNK`（`C_scan≈3.4 ns/word ⇒ ≤22 µs 量级`）（864 行） | **算式错 + 漏固定项 + 常量跨段长外推** | 源码注释 `reclaim.c:2533-2536` 原文含 `+ fixed step cost`，称 “C_scan ~ 3.4 ns/word on the 200x8 MiB pd shape”；`3.4ns×8192 = 27.9µs ≠ 22µs`；实测 8192 段 max 78–98µs（MEASUREMENTS-GC-P0 §8.2），换算 9.5–12 ns/word ⇒ 3.4 是从 65536 段（164–269µs/65536 ⇒ 2.5–4.1 ns/word）推得，不可外推到 8192 | 改为 `P_step ≤ C_scan(8192)·8192 + fixed ≈ 78–98 µs`（C_scan 段长相关；删 “≤22µs”） |
| A5 | `releaseOld` 界 `≤2048×τ_t≈36µs`（866、880、888 三处；§17.2 765 行亦称 ≈36µs） | **低估约 4–5×** | `reclaim.c:2879-2882`：`uint32_t charge = (uint32_t)step; if (r->scanObj) charge += EXTC_RC_SCAN_CHUNK/8;` ⇒ 续扫段只按 `8192/8=1024` 单位记账（×17.49ns≈17.9µs），实测该段 78–98µs ⇒ 每单位实际 ~4.4×τ_t；预算 2048 ⇒ 可含 2 段 | 改“≤2048 计费单位；普通 walk 单位 τ_t≈17.5ns，续扫段按 1024 单位/段记账而实际 78–98µs ⇒ 最坏约 2 段 + pop ≈ **0.1–0.2 ms**；36µs 仅无大对象续扫时成立” |
| A6 | “计量盲区：`extc_rc_markResume` 续扫只 `pops++`、不累加 words ⇒ `[work]/[worst]` 结构性看不见分段扫描”（882 行） | **过时（v2 已修）** | `extc_rc_markResume` 定义 `reclaim.c:2636`，注释 `2640-2642` “Returns the number of pointer words walked, so a resumed segment shows up as work (`seg` in `[top]/[worst]`, `seg_max` in `[work]`)”；`markStep` `2666-2669` `seg = extc_rc_markResume(r); pops++; budget = 0;`；`[work]` printf 含 `seg_max`（`3250`），`[ubwork]` 亦含（`3278`） | 改“已计量：`seg`/`seg_max` 上报续扫 words，可单次运行校验 `P_step ≤ C_scan·SCAN_CHUNK`”；`[top]` 的 `kind=mark` 不再是唯一证据 |
| A7 | 同一行“规模无关性”在 §19.2 表内**重复两次**（879 与 883，文本完全相同） | 复制错误 | 879、883 两行逐字相同 | 删一行 |
| B3 | “`[pause]`（一次 collect）”（874 行标题列） | **语义错**：`[pause]` 量的是 `maybeCollect` 包 `extc_rc_step` 的整步墙钟（`collect()` 共用插桩但无调用者） | `reclaim.c:3372-3379`（maybeCollect 内 `_p0/_p1`、`pauseN++`、`chanNote(2,…)`）；`3422-3429`（collect 内同形；`3346` 定义带 `__attribute__((unused))`）；`1161` 注释 “number of collect triggers”；`MEASUREMENTS.md` §6.3 已给同一结论（`PROBE step_*` 与 `[pause]` 一致） | 改“`[pause]`（一次 maybeCollect→step 的整步墙钟；`collects` = 触发次数，不是 `collect()` 调用数）”；“100M 次分配后 collect max”改“step 入口 max” |
| B4 | `collect/settle` 通道引 “MEASUREMENTS §10.1”（889 行） | 引用不存在 | `MEASUREMENTS.md` §10 是列表，第 1 条才是该结论（`## 10. 未能测量 / 存疑`），无 10.1 小节 | 改 “MEASUREMENTS §10 第 1 条” |
| B5 | “关分段时 1010–1044 µs/段”（864 行）与 “旧 65536 时 55–100 µs/段”（910 行） | 两数来自不同构建，未标 | 1010–1044 是 `-DEXTC_RC_SCAN_CHUNK=1048576`（8 MiB 一段扫完）的旧数（MEASUREMENTS.md §6.3）；55–100 是 pd 形状 65536 段（mean 44.4–48.9、max 164–269，故 55–100 是典型段而非尾） | 各注构建与统计口径（`-DSCAN_CHUNK=1048576` / 65536 段 / 本轮 8192 段） |
| C2 | `regionFree` 实测 pd 18.6 ms/max_frees=1331/max_slabs=133、pb 100M 167.7 ms/max_frees=220377（881、887 行） | 公式准确，**数字不可追溯** | 上界公式与代码一致（注释 `reclaim.c:2328-2347`；计数 `2363` `_frees += 9 + (s->big ? 1 : 0)`、`2389` 每 class `_frees++`、`2395` 计数档 cstk `_frees++`、`2398` `_frees += 2 /* mstk + region */` ⇒ `10·#slabs + #classes + 3`）；但 18.6/167.7/1331/220377 只出现在本文档，`tests/gc-p0/out` 无对应 stderr | 补 MEASUREMENTS-GC-P0 一节（命令 + `[chan] regionfree` 原文）；算术自洽（`10*133+1=1331`）只证明记录一致，不能替代来源 |
| D1 | §19.2 `[pause]` pc 4577 轮 max 16–68 µs（874 行）与 §19.3-3 “`collect` 无调用者”（889 行）并列 | 读者会误判矛盾（“collect 不跑，何来 4577 次 collect”） | 874 行 `collects` 实为触发次数（`1161`），插桩在 maybeCollect（`3372-3379`）；`MEASUREMENTS.md` §6.3 已澄清两者都量 `extc_rc_step` 入口 | 874 行改标签（同 B3）；§19.3-3 补“`[pause]`/`[step]` 的 n 都来自 `extc_rc_step`，与 `collect()` 无关” |
| D2 | §17.2 / §19.1 行号（见 14.3） | 与冻结树不一致 | v2 行号见 14.3 | 机械重指（值不动） |

### 14.5 需 Lead 立刻修的 A 类（按严重度）

1. **A4**（864 行）：`≤22 µs` 算式错、漏 `fixed step cost`，3.4ns/word 跨段长外推 ⇒ 形式化停顿界的 `c_c·w` 项低估 ~3×。
2. **A5**（866/880/888/765 四处）：`36 µs` 把续扫段的 8× 计费折扣当成真实成本 ⇒ 低估 4–5×；形式上界须分“计费单位”与“真实单位”。
3. **A6**（882 行）：`markResume` 盲区已被 v2 修掉（`seg_max` 已上报）⇒ 不删会误导第二段实测方案（去建不必要的判据）。
4. **A3**（847 行）：`pressure` 漏 `bytes > 4·thr` 第二条判据 ⇒ “近墙”语义不完整，影响“墙何时介入”的模型。
5. **A2**（846 行）：`64·L/b` 缺 `b=256`、`L` 未定义 ⇒ 公式不可求值（P0-4 契约的可校验性依赖它）。
6. **A1 + A7 + D2**：`mmaxArm` 锚点、重复行、§17.2/§19.1 行号整体重指（机械但必须，否则后续引用继续漂移）。
7. **C1**（853 行）：墙示例数字不可追溯、`M_max`/`thr` 字段语义未注 ⇒ 不能作为“实测契约成立”的证据；补日志或降级为“待复测”。

补充：`driver.c` 契约断言的判别力问题（14.2 三条）不阻塞文档交付，但建议在 `AUDIT GO` 的 G9 里补“双配置 peak 对比”，否则“墙生效”在门禁上不可证。

### 14.6 Lead 按 A 类修订后的复核（`m02183`，974 行版；只读，未跑程序）

**已修，逐项核过**：A2 846（`M_max − live/4` + 3113/3121 注）、A3 847（两条判据 + 944-956）、A4 864（`P_step ≤ C_scan(8192)×8192 + 固定步成本` + 2533-2536；78–98 µs、9.5–12 ns/word；`-DEXTC_RC_SCAN_CHUNK=1048576` 的 1010–1044 µs 标为“关分段构建”；22 µs 断言已删）、A5（7 处 36 µs 全部限定“无续扫/续扫最坏 0.1–0.2 ms”：765/866/880/887/955/960/970；函数体锚点 2859 ✓；2879-2882 依据 ✓）、A6 882（“续扫计量(v2 已修)” + 2636-2642/2666-2669/3250/3278 ✓）、A7（863 的 1000/1003/1015/1018/1207/1012、865 的 965、867 的 2331 ✓；§19.2 重复行已删）、B3 874（`[pause]` = 一次 maybeCollect→step 整步墙钟、collects=触发次数 ✓）、B4 888（MEASUREMENTS §10 第 1 条 ✓）、标记步误差 877（−2%/+8%/−27% ✓）、C1 853 口径注已加（含 `mmaxUsed`/`thrNow`/`M_max_now`/`M_max_implicit`）✓。

**§17.2 表**：实际 34 行（Lead 称 36 行），逐行对 `reclaim.c` v2 核过全部准确：138/141/144/151/154/321/322/326/327/330/352/211/417/707/965/1000/1003/1006/1009/1012/1015/1018/1207/975/985/1554/1220/1263/1375/1731/2517/2544 + `codegen.c` 12704/12384。旧行号（889/902/896/899/908/911/914/999/1048/1159/1413/2131/2147）已不再出现。

**仍须修（A 类残留 1 条 + 锚点残留 1 条 + B 类 2 条）**：

| # | 位置 | 问题 | 建议修正 |
|---|---|---|---|
| A8 | 904 行 `c_c` 行、909 行杠杆顺序 | **自相矛盾且低估一个量级**：公式是 `c_c·w`（`w=8192` words），却给“典型 8.8–8.9 µs”⇒ 隐含 `c_c≈1.07 ns/word`，同格又写 `C_scan≈3.4 ns/word`；8.8 µs 实为 §19.2 的 total/n 均值（4504–4508 µs / 510 步），**不是单段成本**（单段 78–98 µs）。909 行同错，且“旧 65536 时 55–100 µs/段”与 876 行本轮 65536 的 max 164–269 µs、mean 44.4–48.9 µs 对不上，无法由 after 复现（疑旧版本数） | 904 改：`c_c` = 每 64 KiB 段 **78–98 µs**（≈9.5–12 ns/word），并注“§19.2 的 8.8–8.9 µs 是全部 step 的均值，不可代入 `c_c·w`”；909 改“8192 段 78–98 µs/段（旧 65536 段 max 164–269 µs）”，删 55–100 或标旧版本不可复现 |
| A1/A7 残留 | 845 行、852 行 | 845 仍写“`reclaim.c:925` 的 mmaxArm”（应 921-930，925 只是函数体内一行）；852 仍写“`reclaim.c:925` 上方注释”（应 892-903） | 同 A1：锚点改 `reclaim.c:921-930` / `reclaim.c:892-903` |
| B 残留 | 879 行“规模无关性” | 仍以“`collect max` 仍是 16–68 µs”为证；本轮已定 `extc_rc_collect` 无生产调用者（§19.3-3，结构性 0），该证据链无效。16–68 µs 其实是 `[pause]` 整步墙钟 | 改为“100M 次分配后 `[pause]`/`[step]` 的 max 不变（16–68 µs 整步墙钟）⇒ 停顿由预算常量决定” |
| B1 未处理 | 851 行“承诺” | 仍写成硬契约“峰值驻留 ≤ `M_max + 缓冲`”，但分配路径不拒绝（全仓 `mmax` 只用于 arm/eff/thrUpd/pressure/诊断）⇒ 只是经验界 | 降级措辞：“经验上界；超墙不能拒绝分配，只能加压清扫（分配到挂之前）”；或明确“契约 = 触发策略承诺，非分配准入” |

结论：8 项修订中 7 项完全落地、A4 只改到 §19.1 未同步 §19.4（形式化界表格的核心参数，必须改）；其余为锚点与证据链残留。§17.2 表可视为已修复。

### 14.7 第二次复核（Lead `m02220`，975 行版；只读，未跑程序）

**已修，逐项核过**：A8(a) 904 行 c_c 改“每 64 KiB 段 78–98 µs（≈9.5–12 ns/word）”并注均值不可代入 ✓；A8(b) 909 行改“8192 段 78–98 µs/段；旧 65536 段 max 164–269 µs”，55–100 µs 已删 ✓；锚点 733/845 改 `reclaim.c:921-930`、852 改 `reclaim.c:892-903` ✓，全文 `grep 'reclaim.c:925'` = 0 ✓；879 行改 `[pause]`/`[step]` 口径 + 明写 `extc_rc_collect` 无生产调用者 ✓；851 行降级为“经验上界、分配不拒绝、超墙只加压清扫” ✓；§17.2 行数与 34 行口径一致 ✓。

**新发现 A9（A 类，形式化界量纲不一致）**：895 行 `P_max ≤ … + c_c·w + …`，903 行 `w = 8192` words，而 904 行 `c_c` 定义为“每 64 KiB 段的成本 78–98 µs” ⇒ `c_c·w = 8192 × 78–98 µs ≈ 640–800 ms`，量纲/数量级都不成立（改前 c_c=3.4 ns/word 时该项 ≈27.9 µs 才与公式相容）。建议二选一，推荐 (a)：
- (a) 把 `c_c` 定义改回**每 word 成本 9.5–12 ns/word**（“由 78–98 µs/64 KiB 段实测折算”），则 `c_c·w = 78–98 µs`，公式不改；904 行“均值不可代入”注释保留但改成“8.8–8.9 µs 是全部 step 的均值，不是 `c_c`”；
- (b) 保持 `c_c` = 每段成本，把公式项改成 `c_c(段)`（不乘 `w`），并在表中注明 `w` 只用于说明段长。

**B 类残留（可选）**：733 行仍写“把触发线夹到 `M_max − 64·L/b`”，未注 `b=256 ⇒ live/4`（§18.5 有注）⇒ 建议补一句保持一致。864 行的“3.4 ns/word 是从 65536 段外推出来的”保留可接受；严格说 3.4 是 65536 段实测折算（2.5–4.1 ns/word 区间内），改“由 65536 段实测折算”更准确，非必须。

结论：A8/锚点/B 残留全部关闭；但修复过程在 §19.4 引入 `c_c·w` 量纲错误（A9），这是形式化停顿界的核心公式项，必须在冻结交付前改掉。

### 14.8 第三次复核（Lead `m02236`，(a) 方案落地后；只读，未跑程序）

**A9 已修且量纲自洽（逐项算过）**：904 行 `c_c = 9.5–12 ns/word`，与 903 行 `w=8192` ⇒ `c_c·w = 9.5–12 ns × 8192 = 77.8–98.3 µs` ✓（与 §19.2 实测 78–98 µs 一致）；`c_m·b = 17.49 ns × 256 = 4.48 µs`；公式其余项 `c_s·s`、`c_f·f`、`c_r·n` 若按 µs 计则同量纲 ✓；895 行公式无需改，904 的“均值不是 `c_c`”注正确 ✓。733 行补 `b=256 ⇒ live/4` ✓；864 行“3.4 ns/word = 65536 段实测折算（2.5–4.1 ns/word），不可外推到 8192 段” ✓。

**与 5 µs 门禁的关系（必须写明，否则误读）**：公式是**全通道保守和** ⇒ `P_max ≥ c_c·w = 78–98 µs`，与 5 µs 门禁**不相容**；5 µs 门禁（`CALIBRATION.md:277-278`、:334“p99 中位 ≤5 µs、模型 4.71 µs”）只覆盖**不含大对象续扫的分配点单步**（mark/step 通道）。`GC-ROOFLINE.md:20` 把“P ≤ 求和式”与“5 µs 门禁”并列，读者会误以为 5 µs 对全通道可达。建议 §19.4 补一句：“`P_max` 是全通道上界之和（≈0.1 ms，由 `c_c·w` 主导）；5 µs 门禁只约束 mark/step 通道的单步；两者不可混用。”

**残留 C（符号表缺定义）**：895 行公式用到 `c_s`、`c_f`，但 897-907 的符号表只定义 `c_m / c_c / c_r`（`c_r` 是本轮补的）⇒ `c_s`（种子扫描每 word 成本）、`c_f`（每槽释放成本）无定义，公式不可求值。注意 `c_f` 与 `c_r` 都是“每槽释放成本”、语义重叠（`c_f·f` 用于 sweep/releaseOld，`c_r·n` 用于 regionFree），建议在表里区分或合并。

**残留 B（数值自相矛盾）**：909 行“`c_m·b`（walk，已不是主项，≈1 µs）”与自身数据不符：17.49 ns × 256 = **4.48 µs**，且 §19.2 877 行实测 mark 步 p99 **4.36–6.42 µs**。应改“≈4.5 µs（b=256；基步 8 时 ≈0.14 µs）”，或删“≈1 µs”。

**GC-ROOFLINE v2 三处改动未见于我可见的文件**：`docs/analysis/GC-ROOFLINE.md` 仍是 v1.0（mtime 07:34:34），旧数仍在：112 行“step ≤115 µs”；20/245/396/427 行“55–100 µs/段”；396 行还写“SCAN_CHUNK 65536（reclaim.c:2146-2147）”“8192 让长对象段从 55–100 µs 降到 ~7–12 µs”；245 行“τ_t=14.4 ns ⇒ b=256 对应 P=3.69 µs”。其中“~7–12 µs”与实测 78–98 µs/段 冲突约 8×，会直接影响优化方案 P0-1 的收益预期，建议按实测重算。若 v2 是另一份分片文件，请给路径再核。

结论：(a) 修复正确、公式量纲已自洽；但 §19.4 必须补“全通道和 vs 5 µs 门禁”的口径句、补 `c_s`/`c_f` 定义、修 909 行“≈1 µs”；GC-ROOFLINE 的三处旧数需确认落在哪份文件。

---

### 14.9 第四次复核：A10/C/B 关闭 + FREEZE v3 行号漂移（Lead `m02261`；只读，未跑程序）

**口径**：本节起 `reclaim.c` 行号一律 **v3**（md5 `6a69643ae0a852f4790cbeeb17f2ee5d`，3577 行；v2 `4f927b4f…`/3572 已 superseded）。`codegen.c` `cf2885440981b5c9d154927fb61441b9`（15994 行）、`check_top.c` `b09e25db…`、`check_stmt.c` `e732bcbd…`、`build-frz/extc` `59171671ab71d5d6583c17339f321149` 与 Lead 通告一致（md5sum 已核）。

#### 14.9.1 A10 / C / B 三项修订：全部通过

| 项 | 核验结果 | 证据 |
|---|---|---|
| A10 口径句 | 位置正确（符号表 `897-907` **之后**、杠杆顺序**之前**）；文本逐字含“全通道保守和 ⇒ P_max ≥ c_c·w ≈ 78–98 µs（≈0.1 ms），与 5 µs 不相容；5 µs 门禁只约束不含大对象续扫的 mark/step 单步（`CALIBRATION.md:277-278/334`）”；“两者不可混用”已写 | 文档 897 之前段/912 附近（只读核） |
| 符号表补 `c_s`/`c_f`、改 `c_r` | `c_m` 行＝17.49 ns/obj + 算式 `c_m·b`=4.48 µs（b=256）/0.14 µs（b=8）；新增 `c_s`（未标定，上界 ≈512×12 ns≈6 µs）、`c_f`（未标定，≈33 Ir/次≈10 ns/槽）；`c_r` 改“每槽的**区域级**释放成本” ⇒ `c_f` 与 `c_r` 语义不再重叠 | 文档符号表（`c_r` 依据 `reclaim.c:2331` 现应 `2336`） |
| 杠杆顺序 | `c_c·w > c_r·n > c_f·f > c_m·b`（≈4.5 µs @b=256；基步 8 ≈0.14 µs）✓；原文“≈1 µs”已删（`grep '≈1 µs'` = 0） | 文档 §19.4 |
| 行数/格式 | 980 行；围栏 24 个（偶数）；28 处“奇数反引号行”全部是围栏行（含 `17/20/209/213` 缩进围栏）⇒ inline code 配对完整，Lead 的“0 处奇数反引号”对 inline 成立 | `awk` 统计 |

**A10/C/B 三项无残留。** 公式项与符号一一对应（`c_m·b`、`c_s·s`、`c_c·w`、`c_f·f`、`c_r·n`），量纲自洽：`c_c·w`=9.5–12 ns×8192=77.8–98.3 µs、`c_m·b`=4.48 µs。

#### 14.9.2 FREEZE v3 修复（sweep 期新建 slab 黑位残留）：静态成立，动态待 runtime-facts

Lead 给的机制：sweep 进行中新建 slab 被 prepend 到 cursor 之后（本轮不会被扫），但 alloc 的“扫掠中分配即标黑”仍给它标黑 ⇒ 该 slab 的黑位在本轮无人清（扫不到）⇒ 下一轮 mark 的 dedupe 见黑即跳过整棵可达子树 ⇒ sweep 误释放活对象（pb 100M 挂死）。

代码逐处核过（v3）：

| 位置 | 内容（逐字） | 作用 |
|---|---|---|
| `reclaim.c:845-849` | 4 行注释（新建 slab 在 sweep 中创建时直接标记为本代已扫） | 说明改法 |
| `reclaim.c:850` | `if (r->sweeping) s->swEpoch = r->swId;` | **修复点**：新 slab 直接“视为本代已扫完” |
| `reclaim.c:851` | `extc_rc_ownAdd(s);` | 原路径不变 |
| `reclaim.c:1337-1340` | `if (r->sweeping && s->swEpoch != r->swId && (s != r->swSlab \|\| wi >= r->swWord)) s->mark[wi] \|= bi;` | alloc 标黑规则：本代已扫的 slab（含新建的）**不再标黑** ⇒ 黑位不会残留 |
| `reclaim.c:3014` | `if (++r->swWord >= s->nwords) { s->swEpoch = r->swId; r->swSlab = s->next; r->swWord = 0; }` | 扫完一块即把该 slab 记为“本代已扫” |
| `reclaim.c:2326-2331` | `regionFree` 清 `sweeping`/`marking` 状态 | 区域释放后回初始态 |

判定：与不变式“**sweep 结束时 mark 位必须全 0**”方向一致（新 slab 从不标黑；已扫 slab 完成时置 `swEpoch` 后不再标黑；sweep 结束清 `sweeping`）。静态无矛盾，但**全局不变式仍须 runtime-facts 动态验证**（含专用回归用例），并复跑 G16（before 挂死 >10 s / after 3.06 s、20.49 B/node、2,000,404 kB）。
待核（不阻塞）：`swEpoch` 在 slab 复用/reuse 链路径的初值与更新（我仅核 `slabNew` 主路径）；若复用路径漏置，可能出现反向“该标黑未标黑”。

#### 14.9.3 v3 行号漂移：规则、统计与关键锚点

- **规则**：插入在 `reclaim.c:845-849`（5 行）⇒ 行号 **≥845 全部 +5**，**<845 不变**。
- **统计**（文档 `MEMORY-MODEL-CURRENT.md`）：共 132 处 `reclaim.c` 行号引用，其中 **115 处**含 ≥845 的行号需 +5；17 处全 <845 不变（如 `138/141/144/151/154/211/316-352/417/707`）。
- **关键锚点映射**（v2 → v3）：

| v2 | v3 | 内容 |
|---|---|---|
| 892-903 | **897-908** | Memory-wall 契约注释（含 `driver.c:838-839` 旧引用） |
| 918-920 | 923-925 | 墙读取辅助 |
| 921-930 | **926-935** | `extc_rc_mmaxArm`（函数定义 926；`925` 仅为体内一行） |
| 935-941 | **940-946** | `extc_rc_mmaxEff` |
| 944-956 | **949-961** | `extc_rc_pressure`（两条判据） |
| 965 | **970** | `EXTC_RC_SWEEP_BUDGET` |
| 975 | **980** | `EXTC_RC_RELEASE_BUDGET` |
| 985 | **990** | `EXTC_RC_SWEEP_PRESSURE_MUL` |
| 1000 / 1003 | **1005 / 1008** | `SEED_PUSHES` / `SEED_WORDS` |
| 1006 / 1009 | **1011 / 1014** | `MARK_EVERY` / `MARK_WINDOW` |
| 1012 / 1015 / 1018 | **1017 / 1020 / 1023** | `MARK_STEP` / `MARK_BATCH_MAX` / `MARK_BATCH_POP` |
| 1207 | **1212** | `ASSIST_CAP` |
| 1220 | **1225** | `WORK_BUDGET` |
| 1263 | **1268** | `ALLOC_INLINE` |
| 1375 | **1380** | `FOREIGN_GUARD` |
| 1731 | **1736** | `SHADE_GUARD` |
| 2318 | **2323** | `abort`（regionFree 越界） |
| 2331 | **2336** | `[regionfree]` 上界注释（`10·#slabs+#classes+3`） |
| 2517 | **2522** | `MARK_SHALLOW` |
| 2533-2536 | **2538-2541** | 大对象步上界注释（`+ fixed step cost`） |
| 2544 | **2549** | `SCAN_CHUNK`（8192） |
| 2636 | **2641** | `extc_rc_markResume` |
| 2666-2669 | **2671-2674** | `markStep` 的 `seg` 记账 |
| 2859 | **2864** | `releaseOld` 头注释（函数定义 **2869**） |
| 2879-2882 | **2884-2887** | `releaseOld` 的 `charge`（续扫按 1024 单位记账） |
| 3102-3145 | **3107-3150** | `thrUpd`（`3113`→**3118**、`3121`→**3126**、`3123-3127`→**3128-3132**） |
| 3236 | **3241** | `[regionfree]` 探针 |
| 3250 | **3255** | `[work]`（含 `seg_max`） |
| 3278 | **3283** | `[ubwork]` |

- **结论**：文档 §17.2（34 行常量表）/§18.5/§19 的**值全对、行号全为 v2**，交付前需机械重指（≥845 +5）；建议改后以 v3 锚点集合 `grep` 复查，保证 0 处 v2 锚点。§14.1–14.8 的 `reclaim.c` 引用均按 v2 阅读，已在 §14 开头标注 superseded。

#### 14.9.4 分片抽查（`/tmp/gc2-part*.md`，**非仓库交付物**，仅口头结论）

- **part2 §3.5 停顿定律**：`P(b) = b·τ_t + W_s·64·τ_s = b·17.49 ns + 234.5 ns`；表 b=256 ⇒ 4.71 µs（贴 5 µs 门禁）、b=224 ⇒ 4.15 µs（建议值，17% 余量）、b=128 ⇒ 2.47 µs；`b_max = ⌊(5 µs − 0.2345 µs)/17.49 ns⌋ = 272 → 256` 截断 ✓；τ_t 已全部改 17.49 ✓。
  - **问题 1（口径混用，需改）**：“实测 p99 4.36/4.79/6.42 µs，模型 4.71 ⇒ 误差 −2%～+36%”由两种口径拼接。按 `(模型−实测)/实测` 应为 **−27% / +8% / −2%**（与 `MEMORY-MODEL-CURRENT:877` 一致）；按 `(实测−模型)/模型` 为 **+36% / −8% / +2%**。建议统一为前者并把三个误差都写出。
  - §3.6 `r_min(256)=1.690`、实测 `min r'=2.425`、裕度 1.44×；§3.7 可行带 `M_max ≥ L(1+κ_M(1+gK/b_max))`、digraph 1M 1.53× 裕度；§3.8 闭式 `b* = min(256, ⌊(P_max − W_s·64·τ_s)/τ_t⌋)` —— 结构无源码冲突（数字待 after 复测）。
- **part5 §7 门禁表**：G16 已收录本次修复（after 3.06 s/20.49 B/node PASS；before N≥1.5M 挂死 >10 s）；§8.2 L12 记录修复；L7（36 µs/0.1–0.2 ms）、L8（旧 65536 段 max 164–269 µs）、R7（78–98 µs / 9.5–12 ns）、R8（65536: collects 3.6×=25 vs 7、总 step 时间 6.4×，机制未定位）已按 v3 实测改；§8.3 边界表述正确（“单步 cpu 口径 p99.9 有界，不是任意 STW ≤50 µs、也不是每秒工作量有界”）。
  - **问题 2（待核）**：G1② before 列写“wall 口径 pd 341/643 = 0.53”，判据却是 **cpu 口径 >50 µs 占比 ≤1e-4**；与 `MEASUREMENTS` 的 >64 µs 346–352/643 阈值不同（>50 µs vs >64 µs），需写明阈值并统一口径，否则 before/after 不可比。
  - **问题 3（措辞）**：§8.2 L10 写“已修（逐元素拷贝；纯数据元素仍 memmove）”。实现是 `extc_rc_storeStructCopy(dst, size, base)`：按引用字 `rsInc` 并置 foreign（不置 noSweep），**不是逐元素深拷贝**；纯数据元素仍整体 `memmove`。建议改“引用字逐字 rsInc 记账后整体 memcpy/memmove；纯数据元素仍 memmove”。
- 未核：part5 §7 的 after 数值（G1/G2/G3/G4/G5/G8…）尚未重跑，`AUDIT GO` 后由第二段复测；本节只核结构/公式/口径。

#### 14.9.5 结论

1. **文档侧**：A10/C/B 三项全部关闭；剩余唯一系统性问题＝**v3 行号整体 +5（115 处）**，属机械重指，不阻塞内容判定。
2. **代码侧**：v3 修复静态成立且与“sweep 结束 mark 全 0”一致；等 runtime-facts 动态验证与 G16 回归。
3. **分片侧**：两处口径问题（p99 误差区间、G1② before 阈值）+ 一处措辞（L10），等合并后终核。
4. 仍未 claim task-14（`blocked_by task-12`，`AUDIT GO` 未发）；本节不含新实测。

---

---

## 十五、第二段门禁执行清单（静态准备，**尚未执行**）

> 本节按 Lead m02325 (a) 的要求预排第二段待跑项；**全部为计划，不含新实测**。冻结口径为 **v3**（`build-frz/extc` md5 `59171671ab71d5d6583c17339f321149`）；Lead 通告将再冻 v4（runtime-facts 正在改 reclaim.c 的 cpu 时钟兜底与 `[histo]` 桶加密），届时以 Lead 给出的 **md5 + 日志目录** 为准，命令模板不变。
>
> **脱敏约定（Lead m02371）**：§15–§17 不引用 `reclaim.c` 行号；机制与探针一律写「函数名 + 语句特征」（如 `extc_rc_pressure` 的两条判据、`slabNew` 尾部 `if (r->sweeping) s->swEpoch = r->swId`）。`codegen.c` 行号为 v3 冻结值（v4 的改动范围是 reclaim.c），若 v4 触及 codegen 再统一映射。

### 15.1 通用执行口径（所有门禁共用）

| 项 | 取值 | 依据 |
|---|---|---|
| 编译器 | `EXTC=./build-frz/extc`（或 Lead 指定帧） | 本报告 §0.1 |
| 编译标志 | `CC` 环境变量；`CFLAGS` 必带 `-D_POSIX_C_SOURCE=200809L` | `tests/gc-p0/run.sh`：缺此宏则 glibc 不暴露 `CLOCK_THREAD_CPUTIME_ID`，cpu 口径全 0（empirics 已因此作废旧 after G1） |
| 输出目录 | `OUT=tests/gc-p0/out/after` | Lead m02183 指定 |
| 重复次数 | `REPS=3`（默认） | run.sh |
| 采样间隔 | `INTERVAL=2ms`（默认） | run.sh |
| 档位自证 | 每次构建后 `grep '^#define EXTC_RC_TIER'` 产物 | 产物头 define 是唯一可信档位证据；**宏缺失 = T0**（`codegen.c:12612-12631` 仅在 needReclaim 时发射） |
| 主入口 | `EXTC=... OUT=... REPS=3 tests/gc-p0/run.sh all` | run.sh 子命令 `all/noise/g8/g9/g12/g14/g15/g16/p03/g4/g1/g1cpu` |

### 15.2 档位矩阵（构造方式 + 验证 + 已知数值）

四档均由 `g->needReclaim` 与两开关决定；`codegen.c:14071` 默认 `rcNoCount=1`、`:14119-14122` `SWEEPFREE` 三态、`:15914-15920` 自动事件档（`rcSweepFree!=-1 && needReclaim && unitAllAcyclic`）。

| 档 | 名称 | 构造（编译期 env） | 判定条件 | 产物头验证 | 已知数值/备注 |
|---|---|---|---|---|---|
| **T0** | 无回收 | `EXTC_RECLAIM=0`，或程序被判 reclaim-idle（`codegen.c:13962-13986/14062-14065`） | 不发射 TIER | `grep` 无命中 | 分配器退回纯 bump `extc_arena_*`；**不是纯 GC 税对照**（旧文档坑，§2） |
| **T1** | 事件档（引用计数） | `EXTC_RC_SWEEPFREE=1`（强制，含环程序也可）；等价 `NOCOUNT=0 + SWEEPFREE=1` | `rcNoCount==0 && rcSweepFree==1` | `EXTC_RC_TIER 1` | 无 sweep，环永不回收；参数槽覆盖 UAF（BUG-A）仅此档可达（§8） |
| **T2** | 清扫档（默认） | 不设开关 + **含环/未证明无环**的形状（如 `tests/reclaim-shapes/cyclic_pressure.extc`、`pc_cycle_churn.extc`） | `rcNoCount==1 && rcSweepFree!=1` | `EXTC_RC_TIER 2` | 默认档；`unitAllAcyclic` 为真时会自动升 T1（`codegen.c:15914-15920`），故必须用含环形状构造 |
| **T3** | 计数 + 清扫 | `EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0` | `rcNoCount==0 && rcSweepFree!=1` | `EXTC_RC_TIER 3` | 计数档完整语义；G12/G13 的“计数档”即此档 |

- 四个 `else` 分支落到 tier=2（`codegen.c:12612-12631`），不可达；`SWEEPFREE` 未设或写其它值 ⇒ `rcSweepFree=0`。
- `EXTC_RC_NOCOUNT` 是**编译期**开关（`main.c:494` 注释 “CHANGES THE OUTPUT”），运行期设置 env 无效。

### 15.3 门禁表（命令 / 量具 / 判据 / 期望 / 失败症状）

**锚点脱敏**：本表只用探针名与函数名——`[step]`/`[histo]` = 单步计时与直方桶（step 记账处）、`[chan]` = 每通道计时、`[work]` = 工作量汇总、`[regionfree]` = `extc_rc_regionFree` 统计、`[mmax]` = `extc_rc_thrUpd` 内的墙不可达警告；**全表不含 `reclaim.c` 行号**，故 v4 漂移不会使表失效。

| 门禁 | 命令 | 量具 | 判据 | after 期望 | 失败症状 |
|---|---|---|---|---|---|
| **G1①** | `EXTC_RC_PAUSE=1 ... run.sh g1cpu` | `[step]`/`[histo]` 的 `over1ms` | over-1ms 步数 = 0 | 0 | 任一 rep >0 ⇒ RED |
| **G1②** | 同上 | `over64/n`（下界）、`over16/n`（上界） | cpu 口径 >50 µs 步占比 ≤1e-4 | 下界 ≤1e-4 | 下界 >1e-4 ⇒ RED；跨阈 ⇒ N/A（不得记 PASS） |
| **G1③** | 同上，需 `[histo]` | p99.9 | p99.9 ≤50 µs 且 n≥1e5 | 待测（v4 已加桶） | 量具缺失 ⇒ N/A，**不得用 max 代替** |
| **G2** | `run.sh all`（chan 分布） | `[chan]`/`[worst]` | collect/account 单次 ≤200 µs（整轮排空例外需标注） | before 1.08–1.45 ms（整轮） | 无自动判定 ⇒ 人工 |
| **G3** | 同上 | `[regionfree]` | 单次 free 次数 ≤上界公式且只在帧销毁/退出 | 见 §19.1 表 | 探针缺失 ⇒ N/A |
| **G5** | `EXTC=./build-frz/extc tests/reclaim-perf/gate.sh` | callgrind 总 Ir | ≤`LIMIT=88,029,115`（gate 自量 83,837,253×1.05） | ≤limit | 超限 ⇒ RED；缺 valgrind ⇒ 只跑一半、exit0，需在报告中显式记 skip |
| **G6** | `run.sh g4`（pb 100M） | `/usr/bin/time -v` VmHWM/N | 20 B/node ±10% | 20.49 | 越界 ⇒ RED |
| **G7** | `run.sh g4`（pc 100M） | VmHWM | 稳态 ≤4 MB 且与轮次无关 | 2.85/2.85 MiB | 单调涨 ⇒ RED |
| **G8** | `run.sh g8`（N=2e6，两档 + ASAN） | VmHWM(t1)/VmHWM(t2) | ≤1.1× 且 ASAN rc=0 无报告 | before 15.38× ⇒ after 0.53× | ratio 超标或 ASAN 报告 ⇒ RED |
| **G9** | `run.sh g9`（collect_with_live_task，ASAN） | stdout 全文 + exit | live0=1 freed=0 live1=1 / got=20 steps=5 / tag=777，exit 0 | 全匹配 | 旧构建 trap exit70 ⇒ RED |
| **G10** | 手工：VmHWM vs `[work]` 的 L | 2 个量具 | M_peak ≤3·L_peak | before 2.87× | 无自动判定 |
| **G11** | 手工：`[work]` + CALIBRATION §3 | 峰值轮 r' / r_min | r' ≥1.4×r_min | before 1.44×（薄） | 人工判定，需标注裕度 |
| **G12** | `run.sh g12`（uaf_param_overwrite，T3） | stdout | `alias check b.v=999` | 999 | 111 ⇒ UAF 回归（RED） |
| **G13** | 同 g12 模板（uaf_bulk_copy，**两档都判**） | stdout | `d0r=7` | 7 | 555 ⇒ UAF 回归（RED） |
| **G14** | `run.sh g14`（leak_copy_refs，50k/100k/800k/800k ctl） | `/usr/bin/time -f '%M %e'` | maxRSS(800k copy) ≤1.02×maxRSS(800k ctl) 且 ≤4 MB | 待测 | 线性涨 ⇒ RED；退出码必须自证（防早退假 PASS） |
| **G15** | `run.sh g15`（leak_pool_pin，50k/800k） | maxRSS | RSS(800k) ≤1.20×RSS(50k) | **预期 XFAIL**（已知 L10-a） | 非阻塞；报告标 “RED·已记录” |
| **G16** | `run.sh g16`（regress_sweep_black，N=2e6，timeout 25s） | stdout + wall | exit0、timed_out=false、cnt=2000000、sum=1999999000000、wall<5s | after 3.06 s / 20.49 B/node | 挂死 >10 s ⇒ RED；N 不得 <1.5M（旧构建 1.0–1.3M 即“正常”，是假象） |
| **P0-3** | `run.sh p03`（10×400k ring） | 仅记录 | 不判 | — | 无判定 |

**已知量具缺口（必须在报告中如实标注，否则等于虚报 PASS）**：
1. `G4`（“注入 4× 循环垃圾，RSS ≤ M_max+8 MiB 且不随轮次单调涨”）在 part5 §7 指向 `tests/gc-p0/stress.py`；本准备时该文件**尚不存在**，Lead m02371 已让 empirics 补写（三档 A/B/C + 每轮采样 + `[work]`/`[mmax]` 证据）。⇒ 第二段以该文件届时是否存在为准：在则跑并给证据行，缺则记 “N/A（量具未落地）”，**不得**用 `run.sh g4` 的 G6/G7 结果冒充 G4 PASS。
2. `G2/G3/G10/G11` 无自动判定，需从 stderr 探针人工取数并写口径。
3. `G12/G13` 在 `run.sh` 中只见 `g12` 子命令；Lead m02371 已让 empirics 确认/补 `g13` 子命令并给可复制命令。执行前以 `run.sh` 源码为准；若仍未单列，用同一 build/cc 模板手工构造（`EXTC_RC_NOCOUNT=0 EXTC_RC_SWEEPFREE=0` 与默认两档）。

### 15.4 执行顺序与污染控制

1. 先跑 `grep '^#define EXTC_RC_TIER'` 抽样自证四档构造正确（T0 无宏、T1/T2/T3 = 1/2/3）。
2. **串行**跑 `run.sh all`（G1 计时与 RSS 对并发敏感；Lead 明确要求避免并发污染）。
3. 再跑 `gate.sh`（G5，callgrind 慢，单独跑）。
4. 再跑 `gate-release.sh`（A/B/C/D 四判据；冻结 v3 已知 A/D 红属契约变更，需按新契约改判，见 §13.1）。
5. ASAN 两侧（G8/G9）用 `-O1 -g -fsanitize=address` + `ASAN_OPTIONS=detect_leaks=0`，与计时构建分开目录。
6. 全程记录 `md5sum build-frz/extc` 与 `git rev-parse HEAD`，写入证据头。

---

## 十六、(b) §18.5 内存墙契约与 §19.1–§19.3 措辞核对（只读）

**脱敏说明**：本节的 `reclaim.c` 一律改写成「函数名 + 语句特征」，不再给行号（v4 会整体漂移）；仅当需要指出 doc 原文锚点时才保留 doc 行号（§18.x 等）。

**基准表述**：§18.5:851 已明确 “承诺（经验上界，不是可强制的硬契约）…… 分配路径并不拒绝分配（M_max 只用于 arm/eff/thrUpd 夹取/pressure/诊断），超墙唯一动作是加压清扫”。核对目标是：全文其余处不得再出现可被读成“硬保证”的措辞，且不得与“分配不拒绝”互斥。

| 位置 | 原文要点 | 判定 | 依据/建议 |
|---|---|---|---|
| §18.5:845 | M_max=EXTC_RC_M_MAX；未设/0=隐式 3×live | **准确** | `extc_rc_mmaxArm`（M_max 未设或 0 ⇒ 0、负值按 0）+ `extc_rc_mmaxEff`（未设墙 ⇒ `r->live>0 ? live*3 : 0`，首轮前无 cap） |
| §18.5:846 | thr=min(max(2·live,floor)·scale, M_max−live/4) | **准确**（已补 b=256） | `extc_rc_thrUpd`：`_cap = _wall − r->live/4`（注释写法 `M_max − 64*L/b`，b=256 ⇒ `L/4`） |
| §18.5:847 | pressure 两条：bytes≥7/8·M_max 或 bytes>4·thr | **准确** | `extc_rc_pressure`：`m<=0 ⇒ 0`；`bytes >= m − m/8 ⇒ 1`；`thr>0 && thr<INT64_MAX/4 && bytes > thr*4 ⇒ 1` |
| §18.5:848 | 近墙预算 ×SWEEP_PRESSURE_MUL=4 | **准确** | `EXTC_RC_SWEEP_PRESSURE_MUL` 宏默认 4（`#error` 限制 1..16），用于 `sweepStep` 预算与 mark 批上限 |
| §18.5:851 | 经验上界、不拒绝分配、超墙只加压 | **准确且为基准句** | 与 `reclaim.c` 分配路径无拒绝一致 |
| §18.5:852 | O(log N) 代价、driver 断言改写、M_MAX=2^40 等价关墙 | **准确** | `extc_rc_mmaxArm` 上方的 memory-wall contract 注释段（自述与旧自学习契约互斥、`thr<=2.75*live ⇒ 清扫 O(log N)`）；run.sh 128/0 vs 126/2 |
| §18.5:853 | after 数字（M_max=103125744/thr=35546880/93080KB/pressure_steps=1209/collects=13） | **不可追溯（C 类）** | 已加口径注“单次手工观测/日志未归档/待复测” ⇒ 可接受但**必须**在第二段用 after 日志替换或复测确认；字段语义（M_max=mmaxUsed、thr=thrNow、另有 now/implicit）正确 |
| §18.5:854 | 墙不可达警告 `[mmax] … using floor` | **准确** | `extc_rc_thrUpd` 内一次性 stderr：`[mmax] memory wall unreachable: M_max=%lld floor=%lld; using floor` |
| §18.4:834 | “本轮起这条界是**契约**而不是实测巧合：M_peak ≤ M_max + 缓冲” | **已修（m02371），复核通过** | doc:834 现为“本轮把这条界做成可配置的经验上界（不是可强制的硬契约）……分配路径不拒绝分配，超墙唯一动作是扫得更凶”；若再出现无修饰的“契约”即回归 |
| §18.3:824 | “REG_MAX=512（codegen.c:12415）” | **已修（m02371），复核通过** | doc:824 与 §17.2:774 现均为 `codegen.c:12704`（实际值），不再内部矛盾 |
| §18.3:823 | noSweep 只在两处置位且无清零路径 | **结构仍准确**（P0-7b 修复后 storeStructCopy 不再置 noSweep，故该断言只会更成立） | 置位点：pin 表满分支、宿主未知的保守 store 分支（`extc_rc_storeStructCons`）；LEAK-1 修复动机即此；建议加一句“P0-7b 后新增写路径不置 noSweep” |
| §19.1:867 / §19.2:881 | regionFree 18.6 ms / 167.7 ms / max_frees 1331 / 220377 | **口径已加注（m02371），复核通过** | MEASUREMENTS 无对应 stderr；公式与 `extc_rc_regionFree` 一致（注释上界 `10·#slab + #classes + 3`；计费行 `_frees += 9 + (big?1:0)`、每 class 一次、cstk 一次、`+=2`）⇒ 已按 Lead 修订标“单次观测、待 after 复测”，第二段以 after 的 `[regionfree]` 行替换 |
| §19.1:863/864/865/866、§19.2:874–882、§19.3 | 预算/锚点/口径 | **内容准确，锚点为符号名（已脱敏）** | 本文档已改为「函数名+语句特征」（如 `sweepStep` 的 `SWEEP_BUDGET*(pressure?MUL:1)`、`markStep` 的 `seg` 记账、`releaseOld` 的 1024 计费单位）；doc 侧待 v4 映射后机械重指 |
| §19.3 | 三条通道现状结论（regionFree 同步长停顿；releaseOld 只少收；collect/settle 无生产调用者、PAUSE_DIST 结构性 0） | **准确** | 与 §19.2、MEASUREMENTS §10 一致 |

**结论（b）**：§18.5 主体与“分配路径不拒绝分配”的降级表述**一致**；上轮 3 处必修（834 “契约”、824 行号、867/881 regionFree 口径）**均已按 m02371 修好并复核通过**（doc:834/824/867/881 逐句读过）。剩余仅机械项：doc 侧 §18.5/§19.1/§19.3 的 `reclaim.c` 锚点仍是 v2 行号（如 doc:845/846/847/848/854/867/880/886-887），等 v4 正式映射；本报告对应处已脱敏为符号名，映射落定即可关闭。

---

## 十七、(c) 第二段审计结论模板（预写，待填）

### 17.1 每门禁一行的判定模板

填表规则：**PASS** = 全部 REPS 满足判据 **且** ASAN 干净（若适用）；**RED** = 任一 rep 违反；**XFAIL** = 已知限制（G15/L10-a）且形状未变，需在报告中保留“预期 RED·已记录”；**N/A** = 量具缺失或口径不成立——**N/A 不得写成 PASS**；**BLOCKED** = 依赖 Lead 尚未给出的 v4 帧/日志目录。

| # | 门禁 | 判定 | 证据文件 | 我该看哪一行 | 备注 |
|---|---|---|---|---|---|
| 1 | G1① over-1ms | PASS/RED | `tests/gc-p0/out/after/pauses.jsonl` + `summary.txt` | `over1ms` 列；每 rep 一行 | 判据 0 |
| 2 | G1② >50 µs 占比 | PASS/RED/N/A | 同上 | `over64/n`（下界）与 `over16/n`（上界）两列 | cpu 口径；阈值 1e-4 |
| 3 | G1③ p99.9 | PASS/RED/N/A | `pauses.jsonl` 的 `[histo]` 桶 | `p999_us`（若存在） | 无桶 ⇒ N/A |
| 4 | G5 Ir | PASS/RED/SKIP | `tests/reclaim-perf/out*/callgrind.out` + gate stdout | gate 打印的 `total Ir` 与 `LIMIT` 行 | skip 必须显式记 |
| 5 | G6 pb B/node | PASS/RED | `.../after/timemax.txt` | 第二列 `%M` 与 N 的比值 | 20±10% |
| 6 | G7 pc 稳态 | PASS/RED | 同上（pc 行） | maxRSS 与轮次无关 | ≤4 MB |
| 7 | G8 强制档1 峰值 | PASS/RED | `.../after/timemax.txt`（两档）+ ASAN 日志 | `g8_t1`/`g8_t2` 的 VmHWM 比 | ≤1.1× |
| 8 | G9 活任务 collect | PASS/RED | `.../after/runs.jsonl` + ASAN 日志 | `g9` 的 stdout 全文与 exit | 三行文本全匹配 |
| 9 | G10 M_peak/L | PASS/RED | VmHWM + 产物 `[work]` stderr | `L` 字段与 `%M` | 判据 3× |
| 10 | G11 峰值轮 | PASS/RED | `[work]` 的 `scale_max` 与 CALIBRATION §3 | `r'`/`r_min` | ≥1.4× |
| 11 | G12 参数槽 | PASS/RED | `.../after/runs.jsonl`（T3 构建） | `alias check b.v=` 值 | 999 |
| 12 | G13 批量拷贝 | PASS/RED | 同上（两档） | `d0r=` 值 | 7 |
| 13 | G14 copyInto 泄漏 | PASS/RED | `timemax.txt`（800k copy/ctl） | 两行 `%M` 比 | ≤1.02× 且 ≤4 MB |
| 14 | G15 pool pin | XFAIL | `timemax.txt`（50k/800k） | `%M` 比 | 预期 RED·L10-a |
| 15 | G16 扫掠期新 slab | PASS/RED | `runs.jsonl` + `timemax.txt` | `timed_out`、`cnt=`、`sum=`、wall | N=2e6 |
| 16 | G4 注入压力 | PASS/RED/**N/A（量具缺失时）** | `stress.py` 输出 + `[work]`/`[mmax]` stderr | 每轮采样列与 `RSS` 列 | 量具由 empirics 补写中；缺则 N/A，不得 PASS |
| 17 | G2/G3 | 人工 | `[chan]`/`[worst]`/`[regionfree]` stderr | 见 §15.3 | 无自动判定 |
| 18 | tier 四档自证 | PASS/RED | 各产物头 | `grep '^#define EXTC_RC_TIER'` | T0 无宏 |

### 17.2 报告 §18 的收尾骨架

- **18.x 冻结与构建**：HEAD、v4 md5、`build-frz/extc` md5、编译器版本、CFLAGS（含 `-D_POSIX_C_SOURCE=200809L`）。
- **18.y 门禁结果**：上表原样；每条附“判据/实测/证据行号”。
- **18.z 结论三分**：①内存有界（M_peak ≤ M_max+8 MiB 的实测支撑；不宣称 RSS 恒定）；②停顿有界（仅“分配点推进通道”的单步 cpu 口径；regionFree/releaseOld 的同步长停顿要单独列出；不得写成“任意 STW ≤50 µs”）；③吞吐（摊销数字，标注各形状）。三条都必须带“合成形状、非通用证明”的边界句（part5 §8.3）。
- **18.w 未完成项**：G4 量具缺失、G2/G3/G10/G11 人工口径、L10-a（G15 XFAIL）、PAUSE_DIST 的 n=0 结构性事实（若 v4 未修）。

---

*（第一段 + 冻结树静态复核结束；§15–§17 为第二段**静态准备**（无实测）。运行实测仍只覆盖 pre-freeze 快照 `/tmp/currepo2`/`/tmp/currepo3`；冻结树上的门禁/停顿/RSS 结论以第二段正式重测为准。§14.1–14.8 为 **v2** 树上的只读静态复核（reclaim.c 行号 v2），§14.9 起为 **v3**；§15–§17 不含新实测。**§18 起为 FREEZE v4 第二段独立实测（已完成，v4 口径见 §18.1）；§0.1 的 v3 口径与 §14.9 的 v3 映射据此作废**。）*

## 十八、FREEZE v4 独立复核（第二段实测，2026-11-09 AUDIT GO）

### 18.1 口径、构建与命令

| 项 | v4 值（Lead `m02396` 裁决，唯一有效） |
|---|---|
| base commit | `85b8871bc345dd33f51c9f15f9d7a25d69ecc3c0` + 未提交工作树 patch |
| 源码 md5（`md5sum` 逐文件复核一致） | `src/back/codegen.c cf2885440981b5c9d154927fb61441b9`（15994 行）、`src/back/reclaim.c ff42799bfa62c2c9b0ac5b238e7d05e9`（3607 行）、`src/check/check_top.c b09e25db062b7f85ddd82805a8196e39`、`src/check/check_stmt.c e732bcbdd97e222c63ee52688db3220e`、`tests/reclaim/driver.c 9aae9cb59ff28ef4674c7c1690702d8e`（1216 行） |
| 参考编译器（empirics after 口径） | `build-rf/extc` md5 `9fd0dcd861374668be3c74c342beac6f` |
| 我的隔离编译器 | `build-da/extc` md5 `b7cef25ffd2917121122f5d5db822537`；`make BUILDDIR=build-da -j24` exit 0、**0 warning** |
| CFLAGS（生成物编译） | `-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`；缺 `_POSIX_C_SOURCE` 时 `CLOCK_THREAD_CPUTIME_ID` 不可用 ⇒ `[step]`/`[chan]` 静默为 0（MEASUREMENTS §8 已记录） |

- **取代关系**：§0.1 的 FREEZE v3（`reclaim.c 6a69643a…`/3577 行）与 §14.9 的 **v3 行号 +5 映射**自本节起作废；v4 在 v3 基础上 `reclaim.c` 再 +30 行（3577→3607），`driver.c` 改到 1216 行。本节行号一律 v4，且尽量写「符号名 + 行号」，便于后续再漂移。
- **同源不同 BUILDDIR 的 `extc` md5 不同**（`-g` 段嵌构建目录路径）⇒ 溯源只认源码 md5；本节所有数字出自 `build-da`，未复用他人产物。
- 全部命令（可复现）：
  - `EXTC=./build-da/extc OUT=/tmp/gc-da/after REPS=3 tests/gc-p0/run.sh all` → EXIT 0
  - 同一 `OUT` 追加 `EXTC_RC_STEP_CPU=1 … tests/gc-p0/run.sh g1cpu` → EXIT 0
  - `python3 tests/gc-p0/summarize.py /tmp/gc-da/after > /tmp/gc-da/summary2.txt`（140 行）
  - 定向复现：`/tmp/gc-da/cyc20.sh`（build-da 编 `tests/reclaim-shapes/cyclic_pressure.extc`，20 reps，`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`，N=20,000,000）；`/tmp/gc-da/cycchan.sh`（同形状 10 reps，取每次 `[chan] name=collect`）
  - 既有门禁：`tests/reclaim/run.sh`；`EXTC=./build-da/extc tests/reclaim-perf/gate.sh`；`EXTC=./build-da/extc tests/reclaim-gaps/gate-release.sh`；其余 9 个 `tests/reclaim-perf/gate-*.sh`（逐个传 EXTC）；`python3 tools/check_skip_table.py`；`python3 tools/check_no_history.py --gate`
- 证据：`/tmp/gc-da/{run-all.log,run-g1cpu.log,summary2.txt,gate-ir.log,gate-release.log,reclaim-run.log,<9 个 gate 日志>,cyc20/,cyc20.sh,cycchan.sh}`；`/tmp/gc-da/after/{pauses.jsonl,runs.jsonl,summary.txt,g4_stress.json,log/,c/,csv/}`。
- 判定规则：**PASS** = 全部 REPS 满足；**RED** = 任一 rep 违反；**N/A** = 量具缺失/口径不成立（不得写成 PASS）；**XFAIL** = 已知限制（G15/L10-a）。「与 empirics 一致」= 同向且差 ≤1 个桶 / ≤4 kB / ≤5%（个别处注明）。

### 18.2 逐门禁表（v4，全部用 `build-da` 独立跑）

| 门禁 | 判据 | 我的实测（v4） | 判定 | 与 empirics / 文档 |
|---|---|---|---|---|
| G8 | 档1 峰值 ≤1.1×档2 | T1 1572 / T2 3288 kB ⇒ **0.478×** | PASS | 一致（doc 0.479×、1572/3284，Δ4 kB） |
| G8-ASAN | 两侧干净 | T1/T2 `rc=0`、ASan 报告 0 条 | PASS | 一致 |
| G9 | 活任务 collect 后仍可驱动 | 3/3：`live0=1 freed=0 live1=1` / `tag=777` / `got=20 steps=5`，exit 0 | PASS | 一致 |
| G9-ASAN | 干净 | `rc=0`、0 条报告 | PASS | 一致 |
| G12 | 强制 T3 输出 `b.v=999` | T3 `alias check b.v=999` 3/3 exit 0（修前 111）；默认 T2 也 999（观测，PASS-by-design） | PASS | 一致 |
| G13 | 默认档 + T3 都 `d0r=7` | 默认 T2 `d0r=7`、T3 `d0r=7`，3/3 exit 0（修前 555） | PASS | 一致（S0 形状默认档已不可命中） |
| G14 | 800k ≤1.02×对照 且 ≤4 MB | 50k 2652 / 100k 3228 / 800k 3228 / ctl 3228 kB；对照比 **1.000×**；产物 `storeStruct*` 2 处 | PASS | 一致 |
| G15 | 800k ≤1.20×50k（XFAIL） | 12660 → 189672 kB ⇒ **14.982×** | XFAIL | 一致（doc 14.985×） |
| G16 | N=2M 三 reps 完成、输出正确、<5 s | exit=[0,0,0]、timed_out=[F,F,F]、**wall_max=0.044 s**、cnt/sum 正确 | PASS | 一致（doc 0.055 s） |
| G4-Stress | ①peak ≤ M_max+8 MiB ②末段差 ≤2% ③A<B<C | ①**9144 ≤ 15223 kB** ②末 6 段差 **0.00%**（R93–R100 均 9148）③9144<41704<93428 | PASS | 一致（doc A 9148，Δ4 kB） |
| G4-a | pb 20 B/node ±10% | **20.49 B/node**（pb 100M：2,000,408 kB） | PASS | 一致 |
| G4-b | pc 峰值 ≤4 MB 且与轮次无关 | VmHWM **3244 kB**（`/usr/bin/time` 3032 kB） | PASS | 一致（3.17 MB） |
| G4-c（旧代理） | cyc N vs 4N <1.5 | 6048 / 6044 kB ⇒ **0.999×** | PASS（代理） | 一致 |
| G4-M_max | 报告实际 M_max | stress A 声明 `M_max=7200144 B`；peak/M_max=**1.30×** | 可判 | 一致 |
| G4-pressure | `pressure=1` 次数 | 合计 **18**（cyc 1/pc 0/pb 1/pd 1）；stress A `pressure_steps=33` | 观察 | 一致 |
| G17 | 双配置峰值（观察） | A 22240 / D 25696 / B 70240 / C 70236 kB | N/A（观察） | 逐 kB 一致 |
| P0-3 | 10×400k 环峰值（观察） | **18,164 kB**（3 reps median） | N/A（观察） | 完全一致 |
| G5 | Ir ≤88,029,115 | **86,316,991 Ir**（gate 打印「7.4% of 改动前」） | PASS | 完全一致 |
| G6 | pb 20 B/node | 与 G4-a 同一实测（20.49） | PASS | 一致 |
| G7 | pc ≤4 MiB 且与轮次无关 | 与 G4-b 同一实测（3244 kB）；cyc 20M/80M=6048/6044 ⇒ 0.999× | PASS | 一致 |
| G1① | over-1ms 步数 = 0 | **cpu 全 0**：cyc 0/537750、pc 0/164736、pb 0/409035、pd 0/1296；**wall cyc 0 但 pb 1/409035（4131.3 µs）** | **PASS（cpu）/ RED（wall，单次尖峰）** | **不一致**：doc RED = cyc cpu 1/537750（2540 µs） |
| G1② | >50 µs 占比 ≤1e-4（cpu） | cyc [5.95e-5,2.79e-4] N/A；pc [8.50e-5,2.12e-4] N/A；**pb [1.20e-4,5.43e-4] RED**（over64=49/409035）；pd n=1296<1e5 N/A | **RED（pb）** | 一致（doc pb 下界 1.39e-4）；三例区间不同（B1） |
| G1③ | p99.9 ≤50 µs（cpu） | cyc ≤32、pc ≤16、pb ≤32、pd 桶界 32–64 ⇒ 部分 PASS + pd N/A | 部分 PASS / pd N/A | 一致（doc cyc 16 vs 我 32） |
| G2 | collect/account 单次 ≤200 µs | **cpu**：cyc collect max **91.2 µs**（n=2202）、pb 5.8、pc 38.6、pd 10.3；另 10 独立 reps cyc collect max 16.6–51.0 µs | **PASS（我）/ 不可复现** | **不一致**：doc RED 215.3 µs |
| G3 | frees ≤ 10·slabs+classes+3，且只在帧销毁/退出 | `[regionfree]` n=1/进程：cyc 517≤573（57 slabs）、pc 156≤173（17）、pb 110190≤122433（12243）、pd 1331≤1333（133）⇒ 计数全过；时间：pb **cpu 99.0 ms / wall 94.3 ms**、pd 29.1/29.1、cyc 0.67/0.61、pc 0.22/0.15 ms | 计数 PASS / **时间未界** | 量级一致；**doc 把 86.2/27.5 标成「墙钟」，实为 cpu**（A1） |
| G10 | M_peak ≤3·L_peak | **只核对**（doc digraph 1M：6.44 MB/2.333 MB=2.76×），未独立算 L | N/A（未独立） | 未复核 |
| G11 | r' ≥1.4×r_min | **只核对**（doc 1.44×；b=224 时 1.36×），未独立算 r' | N/A（未独立） | 未复核 |

既有门禁（全部 `EXTC=./build-da/extc`）：

| 门禁 | 结果 | 判定 | 备注 |
|---|---|---|---|
| `tests/reclaim/run.sh` | 通过 **150**、失败 0；ASan/UBSan 干净；chunk=1 全绿（10/0）；chunk=1e9 红在分段断言 | PASS | 与期望 150/0 一致；「有牙」自测成立 |
| `tests/reclaim-perf/gate.sh` | 总指令数 **86,316,991 Ir**（限 88,029,115） | PASS | 与 GC-ROOFLINE G5 完全一致 |
| `tests/reclaim-gaps/gate-release.sh` | **2/4**：A FAIL（sweeps 10/42）、B ok（1.00×<1.5；N=20/80 RSS 6.52/6.52 MB，关档 3.10/3.47 MB）、C ok（语义逐字同）、D FAIL（releases 0/0，槽 0/0） | 预期内 RED×2 | 与契约变更说明一致；机制核实见下 |
| 其余 9 个 `gate-*.sh` | 全 EXIT 0：rt-defaults（1/0）、rc-idle-corpus（17 形状无 rc）、idle-bounded（7 形状判 idle）、shard-cycles（10 通过）、tax（bt-mid ≤1.050×）、slab-scale（0.990≤1.15；4.00×）、pin-scale（0.825≤1.15；4.00×）、reclaim-tables（两条 ok）、mark-locality（Ir 149,284,068；D1mr 655,830≤700,000；DLmr 119,542≤190,000） | PASS | 逐脚本 exit 0 |
| `tools/check_skip_table.py` / `check_no_history.py --gate` | 前者 56 类标记、未标记早退 0；后者历史词存量 0 | PASS | — |

**D 失效的机制核实（不是发射缺失）**：`build-da` 默认档编 `ring_per_round`，产物里 `extc_rc_storeRefOwnedRelease` **出现 4 处**，但运行 `releases=0 release_slots=0` ⇒ 发射仍在、信用不足不触发（夹紧后 sweep 更频繁，`visitedBytes` 记入 `allocBytes` 吃掉 `budgetOk`）。与「A/D 属夹紧后的旧契约」判断一致；建议保留一个能触发它的形状，而不是直接退役 D。

**噪声地板（同一 build-da，3 reps）**：`noise_floor` wall max **207.7 µs**、over50 9/200000、p99.9 6.8 µs；cpu max **207.0 µs**、over50 5/200000、p99.9 0.9 µs。⇒ 任何 max 类数字都在噪声量级附近。

### 18.3 A/B/C/D/E 分类增量问题（A 2 / B 5 / C 4 / D 2 / E 23）

**A 类（错断言/结论，2 条）**

| # | 位置 | 原文/断言 | 判定 | 证据 | 建议修正 |
|---|---|---|---|---|---|
| A1 | `docs/analysis/GC-ROOFLINE.md:547` | 「但**墙钟** pb **86.2 ms**、pd **27.5 ms** ⇒ 时间上仍是长停顿」 | **口径错**：86.2/27.5 是 **cpu** 口径 | `tests/gc-p0/out/after/pauses.jsonl`：pb regionfree cpu 81509/81894/86185 µs、wall 74768/82396/79516；pd cpu 26270/27475/27407、wall 21958/25450/25786；我用 build-da 复测：pb cpu 99020.6 / wall 94319.6、pd 29106.9/29125.7、cyc 671.5/613.6、pc 216.8/154.4 µs | 改为「cpu pb 86.2 ms / pd 27.5 ms（wall 74.8–82.4 / 22.0–25.8 ms；n=3）；独立复测 cpu 99.0 / 29.1 ms」 |
| A2 | `docs/analysis/GC-ROOFLINE.md:546`（与 `:635` 汇总） | 「G2 … **RED（临界）**：cyc collect max 215.3 µs」；`:635`「G2 超 7.7%」 | **不可审计**：用单次 max 判 200 µs，与同文 §8.1 R6「任何 max 类数字不可作契约」矛盾；且 215.3 不可复现 | 我 3 reps：cyc collect cpu max **91.2 µs**；另 10 独立 reps：16.6–51.0 µs；empirics 自身直方图里 cyc collect 落 128–256 µs 桶的样本只有 3 个（两口径合并 n=4404），p99.9 两侧都 ≤64 µs | 判据改 **p99.9 ≤200 µs**（或 max≤200 但注明「单次尖峰、复跑 N 次不复现」）；`:635` 的「超 7.7%」同步改 |

**B 类（数字/锚点偏差，5 条）**：B1 G1② 三例区间（doc cyc [7.25e-5,2.34e-4] / pc [3.64e-5,1.46e-4] / pb [1.39e-4,4.23e-4]；我 [5.95e-5,2.79e-4] / [8.50e-5,2.12e-4] / [1.20e-4,5.43e-4]，结论同）；B2 G1③ cyc p99.9 doc 16 µs、我 32 µs（桶上界，PASS 不变）；B3 `:547` 上界「10·#slabs+#classes+3（最大 pb 110190 ≤ **122430**）」：12243 slabs ⇒ 10·slabs=122430，加 `#classes+3` 后上界应 **≥122433**（写成 122430 偏保守，不影响判决）；B4 同量 ≤4 kB 漂移（G8 T2 3284 vs 3288；G4-c A 9148 vs 9144）；B5 G2 其余三例（doc pb 14.3 / pc 34.8 / pd 21.6 vs 我 cpu 5.8 / 38.6 / 10.3 µs）与 G16 wall（0.055 vs 0.044 s）。

**C 类（不可复现/量具不稳，4 条）**

| # | 现象 | 证据 | 判定 |
|---|---|---|---|
| C1 | G1① 的 2540 µs 单步**不复现** | 我 3 reps cpu：四例 over1ms **全 0**；20 reps：`[histo]` n=179250/rep，b1ms..bge64ms 全 0，`[pause]` max 20.8–107.0 µs，`[top]` rank1 158.8–303.2 µs（最高一次 mark/advance 或 sweep/alloc） | 与 empirics §8.8「0/20」一致；文档也已注「16 次复跑不复现」 |
| C2 | G2 的 215.3 µs **不复现** | 见 A2；empirics 3/4404 样本 >128 µs，我 10 reps 全 ≤51 µs | 判据对尖峰过敏，不是稳定回归 |
| C3 | **量具聚合 bug**：`tests/gc-p0/summarize.py:576` 产出 `RED(下界>阈)`，`:625` 只判 `v == "RED"` ⇒ 聚合层漏掉 pb 的 RED；我 `summary2.txt` 打印「G1② 汇总：N/A（结构性不可判）」而同一文件 pb 行是 RED；`GC-ROOFLINE.md:546` 手写 RED(pb) 与工具输出不一致 | 原文行：`pb clock=cpu … ⇒ [①PASS ②RED(下界>阈) ③PASS]`，随后 `⇒ G1② 汇总（cpu 口径）：N/A（结构性不可判…）` | 一行修：`:625` 改 `v.startswith("RED")`；否则任何引用 `summary.txt` 的 after 结论都会自相矛盾 |
| C4 | **源码注释过时**：`src/back/reclaim.c:1130-1131`「Channels are timed with the wall clock, like `[pause]`」，但 `:1137 extc_rc_chanClock()` 在 `EXTC_RC_STEP_CPU` 且 cpuClock 可用时返回 `extc_rc_cpuNow()`（`:1056-1059`、`:3198-3218` 用 `CLOCK_THREAD_CPUTIME_ID`；`:3225` 注释才是对的） | 生成物 `[clock] step=cpu pause=wall chan=cpu` 自证 | 注释改「chan 跟 step 口径（STEP_CPU 时 cpu）」；文档侧写清 `[chan]` 口径规则 |

**D 类（文档口径冲突/过时，2 条）**：D1 `MEASUREMENTS-GC-P0.md` **没有 G2/G3 判据行**（grep 无 `215`/`86.2`/`collect max`），v4 after 的 `[chan]` 判据只在 `GC-ROOFLINE.md:546-547` 一处 ⇒ 建议 MEASUREMENTS §8 补 G2/G3（含 clock 口径与 `[regionfree]` 原值），否则「唯一有效口径」的原始 stderr 索引不覆盖这两条；D2 `GC-ROOFLINE.md:635` 判定汇总把 G1① 列 RED，而 `:540` 同文已注「16 次复跑不复现」、我另 20 reps 也 0 ⇒ 结论行与证据行口径不一致，建议 RED 后缀「单次奇点；复跑 16+20+10 次全 0」。

**E 类（通过复核，23 条）**：E1 G8 0.478×；E2 G8-ASAN；E3 G9 3/3；E4 G9-ASAN；E5 G12 T3 999；E6 G13 两档 7；E7 G14 1.000×/3228；E8 G15 14.982（XFAIL 一致）；E9 G16 0.044 s；E10 G4-Stress 三判据；E11 G4-a 20.49；E12 G4-b 3244 kB；E13 G4-c 代理 0.999×；E14 G4-M_max 1.30×；E15 G4-pressure 18/A 33；E16 G17 逐 kB；E17 P0-3 18,164；E18 G5 86,316,991；E19 G6/G7；E20 `tests/reclaim` 150/0 + 有牙自测；E21 gate-release B/C 与 A/D 旧契约定性；E22 其余 9 gate + 2 工具 exit 0；E23 噪声地板数字。

### 18.4 最严重 3 条（按对交付物的影响排序）

1. **G2 的「RED」不可审计（A2+C2）**：判据以 2202 个样本的单次 max 判 200 µs，我 13 次独立运行（3+10）全 PASS，empirics 也只有 3/4404 样本越 128 µs、p99.9 两侧 ≤64 µs。同时这与同文 R6「max 类数字不可作契约」直接冲突。**动作**：判据改 p99.9（或 max + 「单次尖峰/复跑记录」后缀），并同步 `:635` 的「G2 超 7.7%」。G1②(pb) 与 G3 的 RED 不受影响（见 18.6）。
2. **G3 口径误标，且长停顿是真实缺口（A1）**：86.2/27.5 ms 被写成「墙钟」，实为 cpu；无论口径，pb 帧销毁的 `regionFree` 是 **90–99 ms** 级同步长停顿（我独立复现），这是「停顿有界」契约最大的未闭合项，与 §8.3-2「`regionFree` 仍是同步长操作」一致。**动作**：改口径与数字；把「时间未界」从 G3 的 PASS(计数) 行里拆出来单列为独立 RED。
3. **G1① 的 RED 与「复跑全 0」并存（C1+D2）**：我 3 reps cpu 全 0、20 reps 全 0，wall cyc 也 0；但同一轮 wall 口径下 pb 出现 **1 步 4131.3 µs**（cpu max 238.4 µs）⇒ 毫秒级墙钟尖峰确实存在。**动作**：① 明确 ① 只认 cpu 口径（wall 仅观察）；② 若保留「不剔除」裁决，结论行必须写「单次奇点/复跑 0」，否则 after 的「停顿门禁仍未转绿」会被误读为 G1① 稳定失败。

（第 4 条：C3 的 `summarize.py:625` 一行 bug——低风险，但会让 summary 与手写结论长期不一致，建议随本轮修掉。）

### 18.5 「我独立跑的」与「我只核对的」分界

- **独立跑（build-da + /tmp/gc-da）**：G4-Stress、G4-a/b/c/M_max/pressure、G5、G6、G7、G8、G8-ASAN、G9、G9-ASAN、G12、G13、G14、G15、G16、G17、P0-3、G1 的 wall+cpu 两遍、G2（`[chan]`）、G3（`[regionfree]` 探针 + 时间）、`tests/reclaim/run.sh`、`gate.sh`、`gate-release.sh`、其余 9 个 `gate-*.sh`、两个 tools、`noise_floor`，以及 20 reps（cyc 2540 µs 复现尝试）与 10 reps（cyc collect 上限）两次定向复跑。
- **只核对（未独立重算）**：G10（M_peak/L，未独立取 L）、G11（r'/r_min，未独立读 `[work]` 的 scale_max）；MEASUREMENTS §7–§13 的 interim/校准数字（如 93,080 kB 的来历：`MEASUREMENTS:429` 已说明旧 interim 构建、日志未归档，`MEMORY-MODEL-CURRENT.md:853` 已改为 18,164 kB 并明写 93,080/pressure_steps=1209/collects=13 作废；`grep 93080` 只命中 `MEASUREMENTS:429`、`MEMORY-MODEL-CURRENT:853`、本报告 §14/§15 旧行）；GC-ROOFLINE v2 的模型参数（`c_scan`、`c_m`、`c_f`、`c_r`、`κ_M`、`τ_t` 等）未重标定。
- **原始数据核对（读 empirics jsonl，不算我跑）**：G2/G3 的 215.3/86.2 ms 等取 `tests/gc-p0/out/after/pauses.jsonl`（与其 `summary.txt` 一致），仅用于判定「是否与文档一致」，不充当我的实测。

### 18.6 对 after 结论的净影响（我的独立复核口径）

- **内存有界：全绿**。G4-Stress PASS（9144 ≤ 15223 kB，末 6 段 0.00%）、G17 A<D<B≈C、P0-3 18,164 kB、G8 0.478×、G14 1.000×、G16 完成。唯一「已知权衡」是 P0-3 的 17.7 MB（相对 HEAD 45.8 MB 是改进，不是回归）。
- **停顿有界：稳健红 2 条**，不是 3 条：**G1②（pb 下界 1.20e-4 > 1e-4）** 与 **G3 `regionFree` 时间未界（pb cpu 99.0 ms）**。G1① 的 2540 µs 与 G2 的 215.3 µs 在我这里都不复现（分别 0/13 与 PASS/13），只能作「单次奇点/临界尖峰」记录，不能当稳定 RED。
- **吞吐：绿**。G5 86,316,991 Ir ≤88,029,115（余量 1.9%）；G6/G7 与文档一致。

### 18.7 未闭合项（转给 Lead / empirics）

1. `summarize.py:625` 的聚合 bug（一行）；修后重生成 `summary.txt`。
2. G2 判据改 p99.9 或加尖峰规则；G3 拆成「计数 PASS / 时间 RED」两判据。
3. G3 的 86.2/27.5 ms 口径修正 + 我复测值（cpu 99.0/29.1、wall 94.3/29.1 ms）入文档。
4. G1① 的时钟口径与「奇点是否剔除」需在 MEASUREMENTS / GC-ROOFLINE 统一写法。
5. G10/G11 未独立重算（若 Lead 要，可补读 `[work]` 原始 stderr 的脚本，不改 `tests/`）。
6. `reclaim.c:1130-1131` 注释（冻结期未动，记入下一轮）。

