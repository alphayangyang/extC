# REFS-V7-BATCH2：两份文档 reclaim.c 引用的全量复核（batch2 补丁）

> 本文件只记录复核结论与补丁清单；**没有修改** `docs/analysis/MEMORY-MODEL-CURRENT.md` 与 `docs/analysis/GC-ROOFLINE.md`。

## 0 冻结口径与快照

| 对象 | md5 | 行数 |
|---|---|---|
| `src/back/reclaim.c`（v7 冻结） | `e17080d1c80513b27ca06f2bc6224b0f` | 4031 |
| `docs/analysis/MEMORY-MODEL-CURRENT.md` | `c2441fe16d303a11d8300decc5d8bb27` | 1067 |
| `docs/analysis/GC-ROOFLINE.md` | `3353f67d435ddd52dfb05e0e90ae4a35` | 1016 |

产物：`/tmp/refs_v7_batch2.tsv`（38 行 / 25 个文档行，md5 `5680318ed7340b08c390e1a804c54ded`）、`/tmp/refs_v7_batch2_check.py`、`/tmp/refs_v7_batch2_relocate.py`、本文件。

**两份文档都在被并行编辑**（本轮复核期间 MEMORY 1065 → 1067 行、GC 986 → 1016 行；§6.7 表整体后移，两条 prose 引用从 957/975 移到 GC:987、GC:1005）。tsv 的行号是最后一次 relocation 的时刻值（见下）；apply 前请先跑 checker，若 miss>0 用 relocator 重新定位并重写 tsv（§7）。

## 1 方法

1. 正则抽出全部 prefixed 引用 `reclaim.c:(数字)(-(数字))?`：196 条，分布在 180 个文档行（MEMORY 152、GC 44）。
2. **按语义定位，不做机械平移**：每条按「文档里这句话想说明的机制」回到 v7 源码找函数/语句边界（反例：`reclaim.c:1366` 是配额钳位语句，不是 `thrUpd`；`reclaim.c:2883` 是 releaseOld 函数，不是 markResume）。
3. 另外收进三类不属于 prefixed 的引用：反引号内的省前缀区间（`:3493`、`1597-1598`）、复合引用（`reclaim.c:2376-2378/2158`）、§6.7 表里两个没加反引号的 prose 数字（`1316-1341`、`1657-1659`）。
4. 每条 `new_ref` 都用 `sed -n` 取首行核对语义落在函数/语句边界；tsv 以 `doc + line` 为主键，`old_ref` 必须在那一行**恰好出现一次**（checker 强制）。

## 2 统计

| 项 | 数量 |
|---|---|
| prefixed reclaim.c 引用（总） | 196 条 / 180 行 |
| 其中已经是 v7（delta=0） | 186 条 |
| 其中仍是旧行号（batch2 修复） | 10 条 |
| batch2 额外修复（bare / prose / 复合） | 28 条 |
| **batch2 总行数 / 覆盖文档行** | **38 行 / 25 行** |
| 复核后残留旧 prefixed 引用 | 0 |

置信度：38/38 条 high（全部用源码首行复核语义），无 medium/low。

## 3 batch2 补丁表（doc + old_ref；行号见 tsv）

| 文档 | old_ref | new_ref | 语义 / 依据 |
|---|---|---|---|
| MEMORY | `reclaim.c:782-853` | `reclaim.c:820-894` | §5.2 heading；slabNew 全函数 |
| MEMORY | `reclaim.c:1289-1361` | `reclaim.c:1455-1615` | §5.4 heading；alloc 全函数 |
| MEMORY | `reclaim.c:2883-2954` | `reclaim.c:3206-3262` | §10.3 heading；releaseOld 主体 |
| MEMORY | ``1408-1469`` | ``1657-1725`` | xregion 三处转移的实现（rsAlloc..unlinkChildren）；同行 prefixed 1622-1626 正确 |
| MEMORY | ``2096-2098`` | ``2376-2378`` | pinAdd 失败置 noSweep（storeRefUnknown） |
| MEMORY | ``2158`` | ``2438`` | storeStructCons hostKnown==0 置 noSweep |
| MEMORY | `（`1657-1664`）` | `（`1935-1942`）` | NOCOUNT=1 四条空桩 |
| MEMORY | `reclaim.c:2376-2378/2158` | `reclaim.c:2376-2378/2438` | 复合引用：noSweep 两处写点 2376-2378 正确、2158→2438 |
| MEMORY | ``:3493`` | ``:3857`` | owe 置位（extc_rc_owe） |
| MEMORY | ``3494`` | ``3858`` | owe 只读（extc_rc_owed） |
| MEMORY | ``3167`` | ``3503-3507`` | extc_rc_sweep；同行 3805-3814 collect 正确 |
| MEMORY | ``3100-3110`` | ``3490-3501`` | extc_rc_settle |
| MEMORY | `reclaim.c:1366` | `reclaim.c:1478` | QUOTA_MAX_MARK 真正钳位行；同行 1044 是 #define 正确 |
| MEMORY | ``:3178`` | ``:3514-3563`` | extc_rc_thrUpd 定义与 M_max 夹紧 |
| MEMORY | ``:3231`` | ``:3567-3574`` | extc_rc_roundOk |
| MEMORY | ``:2158`` | ``:2438`` | storeStructCons 置 noSweep |
| MEMORY | `reclaim.c:1365-1390` | `reclaim.c:1475-1508` | P0-P 实现块；同行 1029-1059 宏块正确 |
| MEMORY | ``:1365-1390`` | ``:1475-1508`` | 配额实现块 |
| MEMORY | ``:1379-1383`` | ``:1480-1499`` | deadline 判定块（#if 起） |
| MEMORY | `reclaim.c:2883` | `reclaim.c:3200-3271` | releaseOld 函数；同行 1079/3211-3224 正确 |
| MEMORY | `:2671` | ``:2983`` | markResume 的 return en - st；同行 2967-2983 正确 |
| MEMORY | ``:2686`` | ``:2998`` | markStep 记 seg |
| MEMORY | ``:3342`` | ``:3689`` | [work] 行 |
| MEMORY | ``:3371`` | ``:3717`` | [ubwork] 行 |
| MEMORY | ``:2552-2553`` | ``:2857-2858`` | SCAN_CHUNK 的 P_step 模型注释 |
| MEMORY | ``:2941-2943`` | ``:3121-3122`` | seg 记录块；同行 1147 声明正确 |
| MEMORY | ``:3445-3447`` | ``:3637-3639`` | [segsteps] 打印 |
| GC | `:1548` | `:1826` | [rt:cascade] 段头 |
| GC | `reclaim.c:1365-1390` | `reclaim.c:1475-1508` | P0-12 实现块；同行 1029-1059 正确 |
| GC | `reclaim.c:1597-1598` | `reclaim.c:1730-1731` | freeSlotLink 热路径头插两行；同行 256/1553-1571 正确 |
| GC | `reclaim.c:1661-1664` | `reclaim.c:1798-1802` | freeSlotWord 惰性分支 nfree 记账（LAZY_FREE=1 路径；默认 0 在 1804-1816 逐槽链入） |
| GC | `reclaim.c:1365-1390` | `reclaim.c:1475-1508` | 配额实现块；同行 1029-1059 正确 |
| GC | ``:1366`` | ``:1478`` | 钳位行 |
| GC | ``:1597-1598`` | ``:1730-1731`` | freeSlot 仍头插；同行 1553-1571 正确 |
| GC | ``:2941-2943`` | ``:3121-3122`` | seg 记录块；同行 1147 正确 |
| GC | ``:3445-3447`` | ``:3637-3639`` | [segsteps] 打印 |
| GC | `1316-1341` | `1588-1594` | prose 数字（非 backtick）：alloc 非 lazy 路径复用/发号 |
| GC | `1657-1659` | `1934-1942` | prose 数字（非 backtick）：NOCOUNT=1 空桩；同行 prefixed 1960 正确 |

## 4 你列的 10 条未命中逐条交代

| # | 旧引用（你列的） | 现在位置 | 状态 |
|---|---|---|---|
| 1 | `## 5.2 slab 几何（reclaim.c:782-853）` | MEMORY:240 | 改为 `reclaim.c:820-894`（slabNew 全函数） |
| 2 | `### 5.4 分配快路径（reclaim.c:1289-1361…）` | MEMORY:263 | 改为 `reclaim.c:1455-1615`（alloc 全函数） |
| 3 | `### 10.3 主体（reclaim.c:2883-2954）` | MEMORY:526 | 改为 `reclaim.c:3206-3262`（releaseOld 主体） |
| 4 | `reclaim.c:1366`（钳位） | MEMORY:794 | 改为 `reclaim.c:1478` |
| 5 | `reclaim.c:1365-1390`（配额实现块） | MEMORY:896、GC:446、GC:603 | 改为 `reclaim.c:1475-1508`（3 处） |
| 6 | `reclaim.c:2883`（releaseOld） | MEMORY:917 | 改为 `reclaim.c:3200-3271` |
| 7 | `reclaim.c:1597-1598`（freelist 热路径头插） | GC:447 | 改为 `reclaim.c:1730-1731`（batch1 用函数范围 1727-1733，本轮收紧到头插两行） |
| 8 | `reclaim.c:1661-1664`（sweep 只记账） | GC:447 | 改为 `reclaim.c:1798-1802`（B 档 `nfree += n`；默认 `LAZY_FREE=0` 的产品路径在 1804-1816 逐槽链入，见 §6） |
| 9 | `reclaim.c:979-1022` / `:1000-1022`（配额宏块） | 当前文档 0 命中 | 文本已不含这两个数字；v7 宏块是 `1029-1059`，文档现在引用的就是它（正确） |
| 10 | `reclaim.c:250`（nfree 字段） | GC:447 已是 `reclaim.c:256` | batch1 已生效，本轮无需再改 |

## 5 delta=0 抽查（未进 tsv 的引用仍然正确）

| 文档 | 引用 | v7 源码首行（截断 95 字） |
|---|---|---|
| MEMORY | `reclaim.c:1455` | "__attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(ExtcRcRegion *r, size_t n, const ExtcRc |
| MEMORY | `reclaim.c:138` | "#define EXTC_RC_SLAB_BITS 20\n", |
| MEMORY | `reclaim.c:1350-1373` | "__attribute__((unused)) static inline __attribute__((always_inline)) size_t extc_rc_markAdvance(ExtcRcRegion  |
| MEMORY | `reclaim.c:3338-3388` | "__attribute__((unused)) static int extc_rc_sweepElide(ExtcRcRegion *r) {\n", |
| MEMORY | `reclaim.c:1069` | "#define EXTC_RC_ELIDE_VERIFY 0\n", |
| MEMORY | `reclaim.c:144` | "#define EXTC_RC_SCALE_MAX ((int64_t)1 << 40)\n", |
| MEMORY | `reclaim.c:2874-2875` | "#ifndef EXTC_RC_SCAN_CHUNK\n", |
| GC | `reclaim.c:1029-1059` | " * EXTC_RC_QUOTA_DEADLINE_US adds a physical ceiling to the mark quota loop: the quota is the byte-earned rec |
| GC | `reclaim.c:1399` | "__attribute__((unused)) static int64_t extc_rc_fqPushChain(ExtcRcSlab *h) {\n", |
| GC | `reclaim.c:1147` | "static long long extc_rc_segN, extc_rc_segPops, extc_rc_segMax, extc_rc_segB[EXTC_RC_STEP_NB];   /* resumed-s |
| GC | `reclaim.c:3425` | "__attribute__((unused)) static int extc_rc_sweepStep(ExtcRcRegion *r, uint32_t budget) {\n", |

另：196 条 prefixed 中 186 条 delta=0，上表 11 条为抽样复述；token 审计（把文档行的 `extc_rc_*`/`EXTC_RC_*` 与目标区间比对）只在 11 个「一行多引用」的行上报错，均为良性（token 属于同行另一条引用）。

## 6 仍未解决 / 需人工确认

1. **范围**：只审 `reclaim.c`；`codegen.c`、`coroutine.c`、`pools.c`、`plate.c` 与 `evidence/example-new.c` 的引用未审。
2. **GC:447 的描述性风险**：那句「sweep 只记账」描述的是 **B 档（`EXTC_RC_LAZY_FREE=1`）**，而冻结默认是 `LAZY_FREE=0`（sweep 仍逐槽链入，`1804-1816`）。行号按 B 语义给（`1798-1802`）；建议句中加「(B 档)」，否则默认行为读起来是错的。
3. **区间粒度**：`new_ref` 是包含式函数/语句区间；指向函数内子块时可能仍有 1-2 行误差（batch1 §6 已按块级核对，本次未逐行重审每个子块）。
4. **文档仍在被改**：GC 本轮 986 → 1016 行、MEMORY 1065 → 1067 行；tsv 的行号由 `/tmp/refs_v7_batch2_relocate.py` 在最后一次运行时写入，apply 前重跑 relocator + checker 即可对齐当前文本。
5. **prose 常量数字**未系统审计（如「`nfree` 12 处写点」「+0.95% Ir」「−3.56%」），它们不是行号引用。
6. **两条 prose 行号**（GC §6.7 的 985/1003）没有反引号，靠表格上下文定位；若重写该表需重新核对。

## 7 复现、自检与漂移重定位

```sh
cd /home/alphayang/extC_Compiler
python3 -I /tmp/refs_v7_batch2_check.py    # rows=38 lines=25 miss=0 才可套用
python3 -I /tmp/refs_v7_batch2_relocate.py # 文档被再编辑后：按 old_ref 重新定位，输出当前行号
md5sum docs/analysis/MEMORY-MODEL-CURRENT.md docs/analysis/GC-ROOFLINE.md /tmp/refs_v7_batch2.tsv
```

checker 对每条做三件事：`sed` 取该文档行、断言 `old_ref` 出现次数 == 1、断言 `new_ref` 落在 `1..`；`miss` 计入输出并以 exit 2 退出。relocator 用同样的 old_ref 唯一性规则在全文重新搜索，输出 `doc:current_line -> old_ref -> new_ref`（找不到唯一行时打印 AMBIG/MISS，不改文件）。
