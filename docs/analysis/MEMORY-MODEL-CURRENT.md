# extC 现有内存管理模型（源码级技术文档）

- 仓库：`/home/alphayang/extC_Compiler`，`git HEAD = 85b8871b`，编译器 `build/extc` 报告 `0.0.1-alpha`。
- 写作时间：2026-11-09。
- 配套文件：`docs/analysis/RUNTIME.md`（运行时事实表，716 行）、`docs/analysis/COMPILE-SIDE.md`（编译侧事实表，455 行）、`docs/analysis/MEASUREMENTS.md`（本机实测）、`docs/analysis/DOC-AUDIT.md`（仓库文档 vs 源码对账）、`docs/analysis/CALIBRATION.md`（模型常量标定）、`docs/analysis/MEASUREMENTS-GC-P0.md`（阶段 0–2 门禁 before/after）、`docs/analysis/MEASUREMENTS-V2-AFTER.md`（bench/gc/v2 外部基准与 memory-first 探针）、`docs/analysis/AUDIT-GC-P0.md`（独立复核）。
- 本文件是"**现状**"文档；优化方案另见 `docs/analysis/GC-ROOFLINE.md`。

---

## §0 证据约定与复现

本文档只把两类东西当证据：

1. **编译器源码**：`src/` 中直接产生行为的 C 代码与其注释。
2. **extc 生成的 C**：`docs/analysis/evidence/*.c`。它们由本工作树的 `build/extc` 现场编译得到：

   ```
   ./build/extc -o docs/analysis/evidence/example-new.c examples/new.extc
   ./build/extc -o docs/analysis/evidence/example-alloc-in-block.c examples/alloc-in-block.extc
   ```

   两条命令只做"发射 C"，不运行程序。产物与仓库里已有的 `build/new.c`、`build/alloc-in-block.c` **逐字节相同，唯一差别是 `#line` 指令里的文件名为相对路径 vs 绝对路径**（可自行 `diff` 验证）。

**行号有两套，必须分清：**

- `src/back/reclaim.c` 等运行时文件里的**行号 = 生成器源码行**。这些文件用 `bufPuts(...)` 把 C 文本按行写出，所以"源码第 N 行"与"产物中对应那段文本"是同一份内容。
- `docs/analysis/evidence/*.c` 里的行号 = **最终 C 文件的行**。本文档在引用产物时，形如 `evidence/example-new.c:2993`。

引用格式：`文件:行号`，行号指该行或该行起始处；区间用 `a-b`。凡是本文件写"**默认不生效/未接线**"的地方，都是我在工作树里 grep 过发射器（`codegen.c` 里是否有对应调用）的结论。
> 外部量测/考古文档在少数地方用裸文件名引用（`MARK-MODEL.md`、`SWEEP-CENSUS.md`、`TAXMAP.md`、`DIGRAPH.md`、`FINDINGS.md`、`SWEEP-SLICE-RECON.md`），**它们都在 `tests/reclaim-perf/` 下**，行号指该目录中的文件。
> **行号口径（FREEZE v5 = 交付树）**：`src/back/codegen.c` = `d339382889b87c0dd18b357c69e2e779`（16 017 行）、`src/back/reclaim.c` = `9af9ca9b8f6ad939a577f99404ddf319`（3 667 行）、`tests/reclaim/driver.c` = `c4ccf26a4d202fd92e82e0a7663a9b2c`；`git HEAD = 85b8871b` + 未提交工作树补丁。构建口径 `build-lead5/extc` = `9bef54886f49d174c01a0447be9ad3fc`（同源不同 BUILDDIR 重建时 extc md5 会因 `-g` 段嵌入的构建目录路径而不同 ⇒ 溯源以源码 md5 为准）。相对上一版 FREEZE v4，P1-7（slab 级省略）在 `reclaim.c` 插入 60 行、P1-8（事件档 `initRef`）在 `codegen.c` 插入 23 行；本文所有行号已按 v5 重算，并逐条用符号定义行校验。
> **行号口径（v6 = 本轮 P0 落地）**：`src/back/reclaim.c` = `7c2a9cedd7edd54de0a378d8a507dcbe`（3 801 行）、`build-rf/extc` = `7019342b3e5ccddbe67cbdc96eae2395`。本版只有 §18.6、§19.1、§21、§22 引用了 v6 的新增段落（回收配额 / 惰性 freelist），其余章节的行号仍是 v5 口径 ⇒ **v5 与 v6 行号不可混用**。
> **行号口径（v7 = P1 一轮：chunk/B/regionFree 队列）**：`src/back/reclaim.c` = `e17080d1c80513b27ca06f2bc6224b0f`（4 031 行）、`src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16 017 行，与 v6 相同）（v7 测量输入为 `d339382889b87c0dd18b357c69e2e779`/16 017 行；提交树只把 `typeHasRefSlots` 的前向声明移到 `genViewCopier` 的说明块之前以过 `check-doxygen`，行数与代码不变）、`tests/reclaim/driver.c` = `7b9ac61a55c140640a5964ab74a8780e`（1 523 行）、`build-p1/extc` = `b9fd57843f2c734c16756f6850ace9c5`（我自建 `build-lead7/extc` = `a7f2a873636aa2f69eaa678691cded0e`，同源只差 `-g` 路径）。相对 v6，`reclaim.c` 插入 free-queue（约 +190 行）与 elide 卷计数器（nLive/nMark）⇒ **v6 与 v7 行号不可混用**；更早的 3 572 行版一律作废。
> **行号口径（v8 = G3b closing queue，当前交付树）**：`src/back/reclaim.c` = `37c92f0b45180dfd65152801dfe48dfc`（4 128 行）、`src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16 017 行，与 v7 相同）、`tests/reclaim/driver.c` = `a9ac2cf4e2f8f184ef9cb524232ac5d6`（1 543 行）、`build/extc` = `58402a50b8d2f05d5a11f49835c15835`。相对 v7，`reclaim.c` 插入 region 级 closing queue（+92 行）⇒ **v7 与 v8 行号不可混用**；本文所有行号已按 v8 重算（v7→v8 用插入表逐段映射，再逐条按符号定义行复核）。更早的 4 031 行版 = v7，仅作历史快照。
> **行号口径（v10 = 分配准入按请求字节，当前工作树）**：`src/back/reclaim.c` = `ff267f16daf6b387d8735158a6d415d7`（4 165 行）、`src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16 017 行，v7 起未改）、`tests/reclaim/driver.c` = `e0509b5182033b80a0d2fc33e923e96f`（1 593 行）、`build/extc` = `9bd5588f8441423dcc93d71d198cb1f0`。**v9（4 156 行）与 v10 都没有做全文重锚**：本版只把本轮编辑到的段（§17.1 的 `M_MAX` 行、§18.2、§18.5、§22）更新到 v10，其余章节的行号仍是 v5–v8 口径 ⇒ **跨版本行号不可混用，核对一律按符号 grep**（`reclaim.c` 的权威索引在 `GC-ROOFLINE.md` 附录 D，已按 v10 重锚）。  **行号口径（v11 = 准入只绑显式墙，当前工作树）**：`src/back/reclaim.c` = `ab21781d29c7ee213afbaafc85b0cc1c`（4 169 行）、`tests/reclaim/driver.c` = `6e1bcec5e3320feb780b9567ed9bca4f`（1 620 行）、`build/extc` = `6f1213ea100acac6c12b61cb262776e7`；v10（4 165 行 / `ff267f16`）是**中间口径**（dense 峰值回归已修），其峰值数字作废。本版把 §16.1、§17.1、§18.2、§18.5、§22 更新到 v11，其余仍是 v5–v8 口径 ⇒ 跨版本行号不可混用，核对按符号 grep。
> **行号口径（v13 = 清零跳表 + 块整块发槽 + 512 KiB 块，当前工作树）**：`src/back/reclaim.c` = `7151a43181f69ab865607f61ad2afad7`（4 212 行）、`src/back/codegen.c` = `2f560e2e6deef8c4927db4611344aecb`（16 017 行，v7 起未改）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行）、`build/extc` = `8af2a5e85324a63720705475d781dbab`。相对 v11，`reclaim.c` 改了三处行为（`extc_rc_zeroSmall` 清零跳表、`EXTC_RC_SLAB_CHUNK_MIN = EXTC_RC_SLAB_SIZE` 块整块发给槽、`EXTC_RC_SLAB_BITS` 17 → 19）并新增 `EXTC_RC_SWEEP_FINISH_MAX`（64 word）夹住分配路径的 sweep 收尾信用 ⇒ 行号位移集中在常量段（+3 ~ +4）与 slabNew/alloc 发射段（+2）。**§5.2、§5.4、§17.2、§18 与块几何相关的行号已按 v13 重锚，其余章节仍是 v5–v8 口径**；机制与四形状实测见 [`P1-ZEROINIT-FILL.md`](P1-ZEROINIT-FILL.md)。
> **行号口径（v15 = `typeNeedsAnyDup` 在剥引用前判 `?ref`，当前工作树）**：`src/back/codegen.c` = `41c26996d3f608855cc41c72f2f0a996`（16 065 行，v7 起的 16 017 行版本作废）、`stdlib/std/json.extc` = `fc33b94db7b01f61ad367fff768edfbb`（1 186 行）、`src/back/reclaim.c` = `7151a43181f69ab865607f61ad2afad7`（4 212 行，同 v13）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，同 v13）、`build/extc` = `f8f037a7b4a457da114b7918da796975`。相对 v14，`codegen.c` 只在 `typeNeedsAnyDup`（原 :5308）一处改动：把「引用判断」放到 `ttBase` 之前（`?ref node` 字段不再被剥成 `node` 递归），+6 行 ⇒ **v14 与 v15 的 codegen.c 行号在 :5308 之后不可混用**；本文所有 `codegen.c` 行号已按 v15 重算（换算表：n<5308 ⇒ n；n≥5308 ⇒ n+6），并逐条按内容校验（旧行文本 == 新行文本）。`reclaim.c` 未改 ⇒ 其行号仍是 v13 口径。
> **行号口径（v16 = `par` 运行期改常驻池，当前工作树）**：`src/back/codegen.c` = `cc01b5f4216ea5c8d9fde77c47c41f24`（16 212 行）、`src/back/reclaim.c` = `7151a43181f69ab865607f61ad2afad7`（4 212 行，同 v13）、`stdlib/std/json.extc` = `fc33b94db7b01f61ad367fff768edfbb`（1 186 行，同 v15）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，同 v13）、`build/extc` = `4f547333175d8583103543aac0ebf73a`。相对 v15，`codegen.c` 只重写 `emitParallelRuntime`（原 :15019-15180）的发射文本并加入常驻线程池（+147 行，净 +191/-44）⇒ **换算表：n < 15019 ⇒ n；n ≥ 15181 ⇒ n + 147；15019–15180 这段被整体重写，旧行号作废、按符号 grep**。本文与该窗口相关的引用（§15.2、§22 的 `codegen.c:15984-15992`）已按 v16 重算，其余章节未受影响。
> **行号口径（v17 = 量具精确计数，当前工作树）**：`src/back/reclaim.c` = `39c93ca4fa0718920844e91147695589`（4 269 行）、`src/back/codegen.c` = `cc01b5f4216ea5c8d9fde77c47c41f24`（16 212 行，同 v16）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，同 v13）、`build/extc` = `722e5ff1247c9bd4204e7f24ed01ff84`。相对 v16，`reclaim.c` 只改量具（`[histo]`/`[segsteps]` 新增 `gt10us`/`gt50us`/`gt64us` 三个精确超阈计数、`[chan]` 的 `collect` 格改名为 `trigger-step`），+10 行 ⇒ **换算表：n ≤ 1176 ⇒ n；1177–1179 ⇒ n+1；1180–1275 ⇒ n+2；1276–3301 ⇒ n+5；3302–3832 ⇒ n+8；3833–3836 ⇒ n+9；n ≥ 3837 ⇒ n+10**。本文所有 `reclaim.c` 行号已按 v17 重算，并逐条按内容校验（旧行文本 == 新行文本）。
> **行号口径（v18 = R2 冷档，当前工作树，2026-11-13）**：`src/back/reclaim.c` = `8f49f0718d788cd1202d348945260ab3`（**4,282 行**）、`src/back/codegen.c` = `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行，同 v16/v17）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，同 v13）、`build/extc` = `9ceb7690c3ae62d12921b7f0a9bf3757`。相对 v17（4,269 行）只插入冷档 13 行（11 行常量块 + 1 行计数器 + 1 行 clamp），`[work]` 的 `cold_steps=` 是原地改 ⇒ **换算表：n < 1153 ⇒ n；1153–1177 ⇒ n+11；1178–1417 ⇒ n+12；n ≥ 1418 ⇒ n+13**。本文的 `reclaim.c` 行号**仍按 v17 保留**（不机械平移：历史上机械平移曾把 `thrUpd` 指到 `markResume`）；本轮新增内容一律用 **v18 绝对行号**并标 `(v18)`，跨版本核对按符号 grep。机制/实测/判据见 §19.6 与 `GC-ROOFLINE.md` §6.8。

> **行号口径（v24 = 自动路径块级归还，当前工作树，2026-12-01）**：src/back/reclaim.c = `711d2b02ade155be40b52508cf784397`（**4,506 行**）、src/back/codegen.c = `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行，未改）、tests/reclaim/driver.c = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、build/extc = `effe8fe2f4330ea878347d4c6c54b2a1`。相对 v23：reclaim.c 净 −1 行（删 `shrinkDue` 字段与三处使用 −5、`capBytes` 记账去 `#if` −2、新宏与 slack 早退 +5、调换调用点 +3）。**v23→v24 的 11 个 hunk、分段换算表与绝对锚点见 `docs/analysis/REFS-V24.md`**；本文件与 GC-ROOFLINE.md 的旧行号仍按 v17/v18 保留（禁止机械平移），v21 起新增内容用当日绝对行号并标版本。机制/实测/判据：§19.7（v21 量具）、§19.8（v22/v24 块级归还）、§19.9（v23 页级归还）、§19.10（v24 自动路径）与 GC-ROOFLINE.md §6.9–§6.12。
> **行号口径（v25 = R7 停顿契约，当前工作树，2026-11-14）**：`src/back/reclaim.c` = `0b91d96ee08ff5e2589136ca313aee4f`（**4,658 行**）、`src/back/codegen.c` = `3ae6e499b6a2d64668ff1daf2eed26a2`（16,222 行；描述符长度上限 4096 → **32**）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `11d39dee4a6481b9e003591ac4e57cc9`。相对 v24（4,506 行）：新增 `[rt:hugefree]` 发射块（858–940，84 行）、步内扫描额度与标记栈软上限、pop/push 高水位计数与 `[wbudget]` 三个新字段、冷档 128 → 32、free 队列 40/50 µs。**v24→v25 的 37 个 hunk、分段换算表与绝对锚点见 `docs/analysis/REFS-V25.md`**；本文件旧行号仍按 v17/v18 保留（禁止机械平移），v21 起新增内容用当日绝对行号并标版本。机制/实测/判据：§19.11 与 `GC-ROOFLINE.md` §6.13。
> **行号口径（v28 = R-A 路径选择，当前工作树，2026-12-05）**：`src/back/reclaim.c` = `7a157eb5d30bb3b71b67c41eea1c6f07`（**4,664 行**，v25 起 +6）、`src/back/codegen.c` = `3ae6e499b6a2d64668ff1daf2eed26a2`（16,222 行，未改）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `e742e423dd5b901cf0405f5beb3a4cb7`。相对 v25 只有两处**纯插入**（`+3` 行在宏区 3339–3341、`+3` 行在浅栈判定 3623–3625）⇒ **v25 与 v28 行号不可混用**；分段换算与绝对锚点见 `docs/analysis/REFS-V28.md`，核对一律按符号 grep，禁止机械平移。
> **行号口径（v29 = R-W 位图登记，当前工作树，2026-12-09）**：`src/back/reclaim.c` = `845c57e2b230202efcd04639e07664d9`（**4,738 行**，v28 起 +74）、`src/back/codegen.c` = `3ae6e499b6a2d64668ff1daf2eed26a2`（16,222 行，未改）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `b5ad0e7cdcb7abbf0cd8f8f18b113570`。相对 v28 新增 74 行：宏区 +21（`EXTC_RC_WMARK` / `EXTC_RC_WMARK_MIN_BYTES` 与长注释）、`ExtcRcSlab` 尾部 +4、`ExtcRcRegion` +2、`slabNew` 位图分配 +9、`extc_rc_wmarkSlot` +18、`unlinkChildren` 位图分支 +19、`rsIncSlab` 三参签名与四个调用点 +3、销毁/复位四处 +8（合计 74）⇒ **v28 与 v29 行号不可混用**；分段换算（旧 n → 新 n + {0(≤220) / 21(221–268) / 25(269–324) / 27(325–977) / 36(978–1931) / 52–53(1932–1959) / 72–74(1960–4664)}）、17 个 hunk 与绝对锚点见 `docs/analysis/REFS-V29.md`，核对按符号 grep，禁止机械平移。
> **行号口径（v30 = idle 判据的第四判据 (d)：不丢内存，当前工作树）**：`src/back/codegen.c` = `ec12011cc5bbff7100072bb465d8f234`（**16,346 行**，v29 的 16,222 行版本作废，净 +124）、`src/back/reclaim.c` = `845c57e2b230202efcd04639e07664d9`（4,738 行，同 v29）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，同 v29）、`build/extc` = `f5ed0d066b084f7f13024b9f4f14950c`。相对 v29，`codegen.c` 的改动全在回收判定一侧（新字段 `rcMemGrowth`、四个新 helper、`scanUnitReclaim` 的 idle 分支、档位闸门）⇒ **换算表：1–473 同号；474–475 段被替代；476–14304 ⇒ +16；14305–14355 ⇒ +77；14356–14402 ⇒ +104；14403–14414 ⇒ +106；14415–14429 ⇒ +107；14432–14458 ⇒ +119；14459–16145 ⇒ +122；16147–16222 ⇒ +124；被替代段（474、14403–14404、14430–14431、14459、16146）按符号 grep**；`reclaim.c` 未改，§5/§17/§18/§19 的运行期行号仍按 v29。机制与实测见 §19.14。
> **行号口径（v31 = 巨块归还与复用缓存，当前工作树，2026-12-12）**：`src/back/reclaim.c` = `29a6372eb0fac935f215847df35ccf41`（**4,780 行**，v30 的 4,738 行 +42）、`src/back/codegen.c` = `ec12011cc5bbff7100072bb465d8f234`（16,346 行，同 v30）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `f82ee508c6fe7233f228fc0decc00b9c`。相对 v30 的 12 个 hunk 与分段换算（0 ≤287 / +3 288–883 / +5 884–894 / +7 895–984 / +17 985–1048 / +18 1049–2070 / +20 2071 / +34 2073–2086 / +38 2088–2093 / +42 ≥2094；被替代段 894、1465、1468、2070、2072、2087、2092–2093、4368 按符号 grep）与 22 条绝对锚点见 `docs/analysis/REFS-V31.md`；本文件旧行号仍按 v17/v18 保留（禁止机械平移），v21 起新增内容用当日绝对行号并标版本。机制/实测/判据：§19.15 与 `GC-ROOFLINE.md` §6.20。
> **文件形态（E1 起）**：上面这 4,780 行发射文本现以 `src/back/reclaim.c` 的数组头 + `src/back/rt/*.inc` 24 个分节文件存放，`tools/source_split.py` 渲染出的文本与拆分前的 `reclaim.c` **逐字节相同**（`tools/emitted_identity.py --check` 430 行为证），所以本文与 `GC-ROOFLINE.md`/`REFS-V31.md` 里的行号引用继续指向同一行。
> **行号口径（E3 = codegen.c 按主题分节，当前工作树）**：`src/back/codegen.c` 现为 **75 行**（39 行文件头 + 14 行 `cg-split:begin`/`cg-split:end` 说明块 + 22 条 `#include "cg/*.inc"`；说明块渲染时整块丢掉）。22 个分节文件 `src/back/cg/*.inc` 合计 16,368 行，渲染文本 md5 仍是 `02acf0b0ae84ece59fb2b70d604ddda7`（逐字节未变）⇒ 本文的 codegen.c 行号继续指向同一行正文，只是那一行的物理位置在某个 `src/back/cg/*.inc` 里。`build/extc` 的 md5 由 `24ce5d0a…` 变为 `eaa22babe131cfc6360fac7eb51a7b0c`，原因是 DWARF 记录分节文件名与节内行号，生成 C 逐字不变（`tools/emitted_identity.py --check` 430 行）⇒ **源码指纹取渲染文本 md5，不取二进制 md5**。**旧行号 → 分节文件 + 节内行号的对照表见 `docs/analysis/REFS-E3.md`**（22 行分节表、六个抽样核对、两个常用坐标：`struct RcPlan` 在 `cg/20-primitive-block.inc`、`rcPlanDecide` 在 `cg/210-criterion-d.inc`）。
> **行号口径（E2 = 回收计划收成一个值，当前工作树）**：`src/back/codegen.c` = `02acf0b0ae84ece59fb2b70d604ddda7`（**16,368 行**，v31 的 16,346 行版本作废，净 +22）、`src/back/reclaim.c` = `75ecc57760aa8c8d92bef3f4bd452529`（**241 行**：E1 起是数组头 + 24 条 `#include "rt/*.inc"`；渲染文本仍是 4,780 行、md5 仍是 `29a6372e…`）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `24ce5d0ab3e817bc5f8ddedee6773b84`。E2 只有两类改动：七个决策字段从 `CG` 搬进 `struct RcPlan`（读写点改 `g->rc.*`）、`scanUnitReclaim` 改名 `rcPlanDecide`；生成 C 逐字不变（`tools/emitted_identity.py --check` 430 行）。**codegen.c 的 87 个变化块与分段换算见 `docs/analysis/REFS-E2.md`**（禁止单一 offset 平移），reclaim.c 的行号引用继续按 v31 / 渲染文本。
> **行号口径（v33 = 容量归还收敛成一个水位，当前工作树）**：`src/back/reclaim.c` = `a8706acf8b9b144399e46fa2c73ad000`（**241 行**：E1 起的数组头 + 24 条 `#include "rt/*.inc"`；渲染文本 **4,780 行**、356,734 字符，`python3 tools/source_split.py --check` ok）、`src/back/codegen.c` = `6ba135e22e86ec89d5cd8aa416a994bc`（75 行，未改）、`tests/reclaim/driver.c` = `28dd4009ac7207f47e581a697c0d421f`（1,637 行，未改）、`build/extc` = `b5a05be1ce5780d34a8f50b4e322322a`。相对 v31 渲染行的变化全部落在容量归还一节：删 `52-slab-release.inc` + `53-page-release.inc`，新增 `52-capacity-release.inc`（surplus / pageWorth / pageTrimSlab / slabUnlink / returnStep）与 `53-slot-release.inc`；`ExtcRcSlab` 删 `deadA`/`deadN`/`allocN`/`pgAllocN` 四字段；分配热路径删 `s->allocN++`；`[pages]` 后新增 `[return]` 一行。**v31→v33 的 hunk、分段换算与绝对锚点见 `docs/analysis/REFS-V33.md`**；本文旧行号仍按 v17/v18 保留（禁止机械平移），v21 起新增内容用当日绝对行号并标版本。机制/实测/判据：§19.16 与 `GC-ROOFLINE.md` §6.21。
> **行号口径（v35 = R2b 巨大块载荷的宿主解析，当前工作树）**：`src/back/reclaim.c` = `b05bbde438d39f3254dc5ea5e8b8d220`（**242 行**：E1 起的数组头 + 25 条 `#include "rt/*.inc"`；渲染文本 **5,050 行**、374,981 字符、`1dcea37a8ea7858a350035507b7364b1`）、`src/back/codegen.c` = `6ba135e22e86ec89d5cd8aa416a994bc`（75 行，渲染 16,429 行 / `aaaaf36f73ff859044acb222934ce9c8`，本轮未改）、`tests/reclaim/driver.c` = `b82c9c83270d859ed51418e2ecb96d05`（1,655 行）、`build/extc` = `82e1b3e2237a3fb961b7f80e1b647224`；v34→v35 的逐段换算见 `docs/analysis/REFS-V35.md`。
> **行号口径（v36 = R3 载荷预留与原地长，当前工作树）**：`src/back/reclaim.c` = `b05bbde438d39f3254dc5ea5e8b8d220`（**242 行**：E1 起的数组头 + 25 条 `#include "rt/*.inc"`；渲染文本 **5,123 行**、380,449 字符、`b8d11ef09d0eeb1cec19d65192d29594`）、`src/back/codegen.c` = `6ba135e22e86ec89d5cd8aa416a994bc`（75 行，渲染 16,429 行 / `aaaaf36f73ff859044acb222934ce9c8`，本轮未改）、`tests/reclaim/driver.c` = `a10fc463149af86ee86f8667fdf65a7f`（**1,706 行**）、`build/extc` = `30bbfdc0e15f867d8f7a5e4353648e95`；v35→v36 的 7 个 hunk 与逐段换算见 `docs/analysis/REFS-V36.md`（渲染文本净 +73 行：`ExtcRcSlab.bigMax` +3、slack 宏与 `bigPayloadAlloc` 签名 +24、载荷分配体 +15、两处 `reserved` 回退 +2、`extc_rc_bigGrow` +34、`slabNew` 调用点 ±0）。
> **行号口径（v38 = 池运行期退场，当前工作树，2026-12-02）**：`src/back/pools.c`（126 行）与 `src/back/pools.h` 已删除，新增 `src/back/places.c`（315 行，md5 `7a618e8bb59338547c712c7d66734215`）与 `src/back/places.h`（16 行）；生成物里的 place 槽表与 `dyn` 对象表由它的 `PLACE_RT[]`（:34）与 `PLACE_DYN_RT[]`（:180）逐行发出。发射符号改名：`extc_pool_zoneEnter`→`extc_place_enter`、`extc_pool_zoneLeaveTo`→`extc_place_leaveTo`、`extc_pool_zoneNewRoot/zoneFreeRoot/zoneOuter/zoneDepth`→`extc_place_newRoot/freeRoot/outer/depth`、全局 `extc_zoneTop`→`extc_placeTop`；`dyn` 句柄字段 `{pid,slot,gen,pgen}`→`{zi,slot,gen,zgen}`，载荷不再寄居对象表池而直接 bump 在命名 place 的 chunk 链里。本工作树当前值：`src/back/reclaim.c` = `15cd7c83bfcd55b8dee6689c13165f30`（255 行 shell、26 分节、渲染 5,234 行），**本条目未改它**；`src/back/codegen.c` = `b3052c5a943e782a2861f05b47e87bf3`（76 行 shell、22 分节、渲染 17,298 行）；`src/back/coroutine.c` = `192e361082285aaf25a01f822a2fcc53`（554 行）。**本条目之前的版本条目不重锚**（本文件的规矩：不机械平移），涉及被改文件的旧引用按符号 grep；§13 与 §15.1 已按 v38 重写。
> **行号口径（v39 = 池原语族与池构造器名表退场，当前工作树，2026-12-03）**：`poolSlice` / `poolSliceRaw` / `poolResize` / `poolResizeRaw` / `poolGive` 与 `isPoolCtorName` / `poolCtorNeedsZone` 已从编译器删除，名字表只剩 `isCopyIntoName`（`copyInto`）；`src/back/cg/*.inc` 改动后重渲染，`src/back/codegen.c` = `b3052c5a943e782a2861f05b47e87bf3`（76 行 shell、22 分节、渲染 **17,235** 行，v38 是 17,298）；`src/back/reclaim.c` = `15cd7c83bfcd55b8dee6689c13165f30`（5,234 行，**本轮未改**）；`src/back/places.c` = `7a618e8bb59338547c712c7d66734215`（315 行，**本轮未改**）。检查器：`src/check/check_top.c` = `fec2ea3fd067196d1f28f84bf4e4e889`（10,998 行）、`src/check/check_expr.c` = `82aa6c3452a902a39974a56473581dd5`（5,874 行）、`src/check/check_lvl.c` = `fe9c59e20ad44c1c251da83f7a626800`；新增公开 API `bool typeOwnsStorage(TypeTable *tt, Type *t)` 在 `src/check/check_lookup.c`（1,091 行，`cc27338ff144d85c66a87410798cc214`）。本工作树 `build/extc` = `fc6f9c6d29db3900a56936046217e28c`。**本条目之前的版本条目不重锚**；涉及被删分支的旧引用按符号 grep。§19.23 是本轮新增。
> **行号口径（v40 = @place/@poolObject 语法面退场，当前工作树，2026-12-04）**：`@place` / `@poolObject` 注解与 `StructDef.poolObject` 字段已从语言删除（写了报 unknown top-level annotation），四条消费分支一并收起：parser 的解析与三条报错文案、`check_stmt.c` 的 `ckStorageCanMove`（现在只看 `typeHasDrop`）、`check_escape.c` 的两处 by-value 深度放宽、`check_top.c` 的 `calleeMakesPool` 声明捷径。本轮未改运行期：`src/back/reclaim.c` = `15cd7c83bfcd55b8dee6689c13165f30`（5,234 行）、`src/back/places.c` = `7a618e8bb59338547c712c7d66734215`（315 行）、`src/back/codegen.c` = `b3052c5a943e782a2861f05b47e87bf3`（76 行 shell、渲染 17,235 行）。检查器新指纹：`src/front/parser.c` = `284727a7d9b61c06a65d2f8f66749e5b`（5,801 行）、`src/ast/ast.h` = `1f92daadb44d23c12b534324ec76a272`、`src/check/check_stmt.c` = `c0b37b10470ea07115dcaf23172d4386`（5,727 行）、`src/check/check_escape.c` = `a17559fb68fd66b6cb77089772ea5ea5`（3,847 行）、`src/check/check_top.c` = `0e1cb36916bfbff382e347129c987c32`（10,993 行）、`src/check/check_internal.h` = `7360f0bc8d27d6fd88d91c36ebd20157`（3,009 行）；本工作树 `build/extc` = `8f34f2b5c1f49cac60533ad590c322b2`。**本条目之前的版本条目不重锚**；涉及被删分支的旧引用按符号 grep。§19.24 是本轮新增。
> **行号口径（v41 = hashMap 控制字节按机器字打包，当前工作树，2026-12-07）**：`stdlib/std/data/hashMap.extc` 从 453 行改成 **561 行**，md5 = `0042290b13118d43e61dd80dfe29702a`。只改标准库，未改编译器：`src/back/reclaim.c` = `15cd7c83bfcd55b8dee6689c13165f30`（5,234 行）、`src/back/places.c` = `7a618e8bb59338547c712c7d66734215`（315 行）、`src/back/codegen.c` = `b3052c5a943e782a2861f05b47e87bf3`（76 行 shell、渲染 17,235 行）、`build/extc` = `8f34f2b5c1f49cac60533ad590c322b2`（同 v40）。存储形态变化：`tag` 从 `block<u8>`（一桶一字节）变成 `block<u64>`（一字节一桶、按机器字打包），控制字节从 `((h >> 20) & 127) + 1` 变成 `((h >> 20) & 127) + 128`（最高位恒 1），空桶仍是 0；探测定位于 `probeRange` / `slotRange` / `findH` 且全部 `@inline`。**本条目之前的版本条目不重锚**；§19.25 是本轮新增。


---

## §1 一句话结论

extC 的内存管理**不是"一个 GC"**，而是：

> **编译器在静态分析阶段把每个分配点指派给五种存储基质之一**；绝大多数对象因此在编译期就拿到确定的、作用域式的生命周期。运行期回收器只治理其中一种——"逃逸到函数体层（level==1）或更浅层"的**引用对象**——它是**非移动、非 STW、增量推进的标记 + 步进清扫**。默认构建下它有三个工作档（§1.5）：**事件档**（静态证明无环，计数开、不做 sweep）、**清扫档**（含环或未证明无环，计数层在发射期被整段删掉）、**无回收档**（整 unit 无回收工作，连回收运行期都不发射）。

由此得到两条现状结论（也是下一步优化的出发点）：

- **内存**：作用域基质（帧 arena / coroutine arena / plate）的内存**有界**（上界由作用域内分配量给出）；rc region 的内存**没有常量上界**，上界是 `Σ`（每个存活 region 的 slab 峰值），是"分配速率 × 触发阈值 × 环堆积量"的函数；place 载荷区（`dyn` 的值）随 place 内的分配量增长，place 结束整条 chunk 链归还（上界 = 该 place 内累计分配字节 + 块余量，< 2× 字节 + 256 KiB）。
- **停顿**：生产路径上的回收工作已被切碎（每步 256 单位标记、8 word 清扫、`releaseOld` 2048 步预算），但“单位”不是时间：1 单位可能要读 `nptr` 个指针（`nptr` 无上限），1 次清扫步最多释放 512 个槽，大对象一段 8192 words。v8 起 `regionFree` 也入界：同步段 O(1)（置 `dead` + 挂 closing queue），逐 slab 的 `unlinkChildren`/`ownDel`/`free` 移到预算内排空（§19.5）⇒ 生产路径上不再有随 region 规模增长的同步释放；剩下的界由段成本（`c_c·8192`）与配额 deadline（800 µs）给出。

### 表 1-1 五种存储基质

| # | 基质 | 分配点（编译期判定） | 分配实现 | 生命周期终点 | 回收方式 | 内存上界 |
|---|---|---|---|---|---|---|
| 1 | **帧 arena**（bump） | `new`/`alloc` 落在 level≥2 的块内，或 home 层 | `extc_arena_alloc`（`codegen.c:12442`） | 块结束 / 帧返回 | `extc_arena_release` / `extc_arena_destroy`，整块丢弃，无逐对象成本 | **有界**：= 该作用域内累计分配字节（+ 对齐/块头），释放后归零；循环体每轮进门/出门各 release 一次 |
| 2 | **rc region**（slab + 标记清扫） | `new T`、定长 `new [N]T`、`alloc<T>(n)`、单值 `@overwrite`（`needReclaim` 为真时一律 rc，与层号无关，`codegen.c:4686/4716`）；`alloc<T>(n)` 与 `new T[n]` 的 data 区仅层号 == 1（`:4721-4736、4831-4843`） | `extc_rc_alloc`（`reclaim.c:1489`） | 不可达被标记清扫判定 | 增量标记 + 步进清扫，槽进 freelist 复用；slab **永不归还** | **无常量上界**：Σ(存活 region 的 slab 峰值) |
| 3 | **place 载荷区**（`dyn` 的值） | `dyn Trait(x)` 的载荷；用户可见容器已全部离开（`vector`/`string` → 缓冲区基质，`hashMap`/`map`/`set`/`linMap`/`linSet` → `block<T>`，见 §19.20/§19.21/§19.22） |
| 4 | **coroutine / parallel worker arena** | 每个任务的帧 | `extc_arena_*`（每任务独立） | 任务结束 | 确定性 destroy | **有界（每任务）** |
| 5 | **plate**（`std.mem`） | 程序显式请求 | `mmap(PROT_NONE)+mprotect`（`stdlib/std/mem.extc`） | `close` | `munmap`；永不搬家 | **有界**（用户 view 的长度） |

> 注意"五种基质"里只有第 2 种有运行期回收器；其余四种都是**作用域式**的：谁分配、谁在作用域结束时整块丢弃。这是 extC 内存模型最重要的结构事实。


### §1.5 默认构建不是一档：三档与判定链

上面说的"标记 + 步进清扫"是运行期**机制**；"默认构建到底发射哪一档"由编译器对每个 translation unit 的**静态分析结果**决定，共三档。这是仓库现有设计文档全部漏写的地方（对账见 §20）。

| 档 | 判定（每个 unit 各判一次） | 发射的回收运行期 | 产物决定性行 | 分配点实现 | 谁来收 |
|---|---|---|---|---|---|
| **0 无回收档** | `unitReclaimIdle` 为真：整 unit 的受治理分配点都没有需要回收的工作 | **不发射任何 rc 运行期** | 产物 `extc_rc_` 记号数 = 0 | `extc_arena_alloc`（作用域式） | 作用域结束整块丢弃 |
| **1 事件档** | 档 0 为假，且 `unitAllAcyclic` 为真：可静态证明无环、引用只存活在被计数槽中 | 计数层 + 定向释放；**不做 sweep** | 第 13 行 `#define EXTC_RC_NOCOUNT 0`、第 15 行 `#define EXTC_RC_SWEEP_FREE 1` | `extc_rc_alloc` | `hold` 归零当场 `freeSlot`；trial deletion |
| **2 清扫档** | 以上都不成立（含环，或分析无法证明无环） | 增量标记 + 步进清扫；计数层整段编译掉 | 第 13 行 `... NOCOUNT 1`、第 15 行 `... SWEEP_FREE 0` | `extc_rc_alloc` | 标记清扫（sweep） |

判定链源码（都在 `src/back/codegen.c`）：

1. `:15984` `scanUnitReclaim(&g, m);` 先做 unit 级回收分析；若 `unitReclaimIdle` 成立，`:14419-14421` 把 `g.needReclaim = false` ⇒ 之后任何档都不发射回收运行期（`unitReclaimIdle` 本体 `:14319-14343`，判据 `planFnReclaimIdle` 合取每个有 body 的函数、每个 used 泛型实例方法、全局初始化器）。
2. 紧接着 `:15989-15992` 逐字：

```c
if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) {
  g.rcSweepFree = 1;
  g.rcNoCount = 0;
}
```

即**事件档的自动升级**：只要用户没有显式要求 `EXTC_RC_SWEEPFREE=0`（那会把 `rcSweepFree` 置 `-1`，封死自动升级），且能证明整 unit 无环，就自动改成"计数开 + 不清扫"。注释给的理由逐字是：整 unit 被证明 acyclic 且引用只存活在被计数槽里 ⇒ 不可达对象在 `hold` 归零时当场释放 ⇒ sweep 无事可做。
3. 否则落回编译默值 `:14105` `g->rcNoCount = 1;`（注释："Default 1 = no counting, reclaimed by sweeping alone"）。
4. `unitAllAcyclic` 本体 `:14358-14387`：合取每个有 body 的 `planFnAcyclic(f)`、每个 used 泛型实例方法、全局初始化器（`gd->init && !rcAllocInExpr(NULL, gd->init)` 就返回 false）；**无 body 的 extern 不参与**（否则任何调用 C extern 的程序都进不了事件档）。
5. `planFnAcyclic` 的判据不止「无环」，是 4 条合取（`src/check/check_top.c:6166-6205` 注释逐字，v1）：(i) 写进 ref 字段的值必须是本帧新鲜分配（直接 `new`/`alloc`，或来自 acyclic 且 retFresh 的被调函数）；(ii) 或 null；(iii) 值的层号**严格大于**宿主层号；(iv) 每个能丢弃引用的位置必须是被计数的 rc 槽（无 ref 局部量赋值、无涉及引用的 opaque 调用）。⇒ 档 1 的实际适用域是「单层 churn」，不是整个无环堆（父→子的链式形状自动退回档 2，判据 (iii)）。

三档的可观测量（复现命令见 §0）：

- 档 0：不逃逸的循环分配程序（例如 `/tmp/empirics/probes/pa_arena_nonescape.extc`，源码来自 MEASUREMENTS 表 A），产物 `grep -c extc_rc_` = 0；实测 2,000,000 轮 1.5 MB 峰值 RSS 恒定、零停顿。
- 档 1：`tests/reclaim-acyclic/acyclic_heapref.extc` → 第 13 行 `#define EXTC_RC_NOCOUNT 0`、第 15 行 `#define EXTC_RC_SWEEP_FREE 1`，8 处 `extc_rc_alloc`、7 处 `inc/dec` 调用、4 处 `shouldSweep`（永假）。
- 档 2：`tests/reclaim-shapes/cyclic_shape.extc` → 第 13 行 `1`、第 15 行 `0`，12 处 `extc_rc_alloc`。

> **档 1 的实测锚点**（`docs/analysis/MEASUREMENTS-TIER1.md` §3，自动升级确认生效）：leaf 16 B×200 M ⇒ 产物第 13 行 `NOCOUNT 0`、第 15 行 `SWEEP_FREE 1`，7.55 ns/op、峰值 1 MB、`freeSlot` 199,999,999、`slabNew` 1；同形状档 2 为 3.02 ns/op（档 1 慢 2.50×）。456 B 两者打平（0.101 s）。32 KB（活集 32 MB）档 1 0.051 s / 33 MB vs 档 2 0.151 s / 226 MB（7.1× 活集，78% 墙钟是缺页）。**强制档 1 于父→子形状会漏整棵子树**（45.4 MB vs 3.0 MB），所以放宽判据前必须先补 `freeSlot` 的子引用级联。

用户开关可以强制覆盖（会产生三档之外的组合）：

| 开关 | 效果 |
|---|---|
| `EXTC_RECLAIM=0` | 强制档 0 形态：分配点改纯 bump（`codegen.c:4700-4702` else 分支），不发射运行时文本 |
| `EXTC_RC_SWEEPFREE=0` | `rcSweepFree=-1`，**禁止**事件档自动升级 |
| `EXTC_RC_SWEEPFREE=1` | 强制"计数开、不清扫"（`codegen.c:14152-14157`，同时把 `rcNoCount=0`；三态解析 `:14139-14157`） |
| `EXTC_RC_NOCOUNT=0` | 只强制计数层存在；清扫仍由 `rcSweepFree` 决定 ⇒ 组合"计数 + 清扫"（性能对照档，MEASUREMENTS 表 A2） |
| `EXTC_RC_NOCOUNT=1` | 关计数；若同时 `SWEEPFREE=1`，则 `13810-13813` 的后写覆盖前者 |

> **判定失败的方向**：所有近似都朝"少收 ⇒ 内存涨"，从不朝"早收 ⇒ 悬垂"（`unitAllAcyclic` 只允许被证明成立的 unit 升级；证明失败退回档 2）。
> **后文约定**：凡说"默认"指**档 2 的默认参数**；说"事件档"指档 1。仓库旧文档（`docs/design/RECLAMATION.md`、`docs/design/MEMORY-MODEL.md` 等）只描述了档 2，因此它们"默认无计数"的断言只对档 2 成立（§20）。
> **代价**：档 1 用计数换掉 sweep。已实测的是**计数层**的代价，即 `EXTC_RC_NOCOUNT=0` 的"计数 + 清扫"对照档：墙钟 +76%~+301%（MEASUREMENTS 表 A2）。**纯事件档**已实测（`docs/analysis/MEASUREMENTS-TIER1.md` §3）：16 B 上比档 2 慢 2.50×（7.55 vs 3.02 ns/op）、456 B 打平、32 KB 上反而快 3× 且峰值 1.03× 活集（档 2 为 7.1×）；自动档只在「单层 churn」生效（chain2 因判据 (iii) 退回档 2），强制档 1 会漏子树（45.4 MB vs 3.0 MB）。档 0 代价 ≈ 0。

---

## §2 语言表面：一条语句落在哪种基质

| 语言形态 | 层号判定 | 产物形态 | 基质 |
|---|---|---|---|
| `new T`（单值） | 恒为 rc（"single object is an rc slot"，`codegen.c:14263`） | `extc_rc_alloc(r, sizeof(T), &desc)` | rc |
| `new [N]T`（定长） | 同上 | `extc_rc_alloc(...)` | rc |
| `new T[n]`（**无长度**，n 运行期决定） | 永远是"视图"（slice），不是引用槽 | `extc_arena_alloc(&__extc_a[k], n*sizeof(T), ...)`，再包成 `slice` | arena（**层号 == 1 时 data 区改走 `extc_rc_alloc`**，见下注②） |
| `alloc<T>(n)` | 层号由检查器 `planArenaLevel` 给出；== 1 时进 rc | level==1：`extc_rc_alloc(...)`；否则 arena | rc 或 arena |
| 单值 `@overwrite` 单元 | 单元总在当前帧；初始化器就是 `new T` | `extc_rc_alloc(...)` | rc |
| `dyn Trait(x)` 的载荷表 | — | `extc_place_alloc` / `extc_dyn*` | place 载荷区 |
| 字符串字面量 | — | 静态 `char[]`（只读），不分配 | rodata |
| coroutine 帧 | — | 每任务 `extc_arena`（`task->arena<k>`） | coro arena |
| `std.mem` 的 plate | — | `mmap` 保留 + `mprotect` commit | plate |

> **三个必须按源码读的前提（AUDIT A3/A4/A5）**：① 表 1-1 第 2 行与上表的 rc 判定都以 `needReclaim` 为真为前提；档 0 / `EXTC_RECLAIM=0` 时改走 `extc_arena_alloc`（`codegen.c:4700-4702`）。② `new T[n]` 的值是 slice 视图，但层号 == 1 时其 **data 区**是 rc 槽（`4716-4731`），不是「永远是 arena」。③ `alloc<T>(n)` 的层号由检查器 `planArenaLevel` 决定，只有 == 1 才进 rc。

**产物证据**（`docs/analysis/evidence/example-new.c`，源 `examples/new.extc`）：

```c
2987:    extc_arena __extc_a[3] = {0};
2989:    ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&(__extc_a[1]), 1);
2990:    ExtcRcRegion *__extc_rcr1 = extc_rc_regionOf((void *)&(__extc_a[2]), 2);
2992:    { if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, (void *const *)0, 0); }
2993:    node * a = ((node *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(node), &__extc_rct0));
2998:    { if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, (void *[]){ (void *)(a) }, 1); }
2999:    node * b = ((node *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(node), &__extc_rct0));
3014:    slice_i32 buf = (slice_i32){ .data = (int32_t *)extc_rc_alloc(__extc_rcr0, (int64_t)(8) * (int64_t)sizeof(int32_t), (const ExtcRcType *)0), .len = (int64_t)(8) };
3026:    array_3_i32 * arr = ((array_3_i32 *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(array_3_i32), (const ExtcRcType *)0));
3041:        { if (extc_rc_shouldSweep(__extc_rcr1)) extc_rc_maybeCollect(__extc_rcr1, (void *[]){ (void *)(a), (void *)(b), (void *)(buf.data), (void *)(arr) }, 4); }
3042:        node * cell = ((node *)extc_rc_alloc(__extc_rcr1, (int64_t)sizeof(node), &__extc_rct0));
3056:    extc_rc_regionDrop((void *)&__extc_a[1]);
3057:    extc_arena_destroy(&__extc_a[1]);
3058:    extc_rc_regionDrop((void *)&__extc_a[2]);
3059:    extc_arena_destroy(&__extc_a[2]);
```

可以逐字读出四件事：(a) 每个用到的 level 在**序言**就 `regionOf` 惰性登记；(b) **每次受治理分配之前**先发一次"压力检查 + 就地根表"；(c) 根表是**分配点当前静态已知的活槽**组成的复合字面量（`(void *[]){...}`），随位置变化，不是全帧根集；(d) 帧尾是 `regionDrop` + `arena_destroy`，逐层成对。

---

## §3 静态层级系统（一切的基础）

### 3.1 level 的语义

权威定义在 `src/check/check_lvl.c:11-15`（逐字）：

> "A level is the depth of the place that owns the bytes: `0` means the storage outlives the frame (a global, the caller's home arena), `1` is the function body, larger numbers are deeper blocks… Smaller therefore means longer lived."

- level **0** = home 层（比本帧活得久：全局或调用者给的 home arena），在内部用 `ARENA_HOME = -1` 表示。
- level **1** = 函数体。
- level **2,3,…** = 逐层嵌套块；`while`/`for` 体比外层块**深一层**。

### 3.2 数组布局

- `maxLv = 1 + blkMaxOfBlock(f->body)`（`codegen.c:9908`），即块的最大深度 + 1。
- 序言里发 `extc_arena __extc_a[maxLv + 1] = {0};`（`codegen.c:9965`；产物 `evidence/example-new.c:2987`）：下标 0 是 home 槽，**下标 1..maxLv 对应函数体与各块层**。
- 生成器内部表（`zoneMark`/`loopLevel`/`loopRootBase`/`coroTaskAt`/`ownedCoro`）按 `levelCap = maxLv + 2` 分配（`codegen.c:9911-9922`），比 `__extc_a` 多一格用于越界防护。
- 若函数体里没有任何 `new`（且不调用带 home arena 的函数），**连数组都不发射**（`planMayUseArena` 判定；`codegen.c:9945-9949（`g->noArena = !planMayUseArena(f) && …`；9912-9916 是注释）` 注释）。

### 3.3 分配点归属由检查器静态决定，地址永不变

- 每个分配表达式的 `planArenaLevel` 由检查器给出：前端先给初值，**逃逸分析把"发布到更浅层"的值提升到该更浅层**（`src/check/check_escape.c:1-8, 33-43`），最后 `check_top.c:9563-9622` 用静态逃逸层**覆盖**前端初值（`minAt == 0` ⇒ `ARENA_HOME`）。
- 逃逸提升有 hop 预算 **512**（`check_escape.c`），home 有**粘性**规则（`check_escape.c:2792-2803`）。
- 地址 = 该层 arena 的 bump 指针，**对象在生命周期内不移动**；rc region 的槽地址同样固定（slab 基址 + 槽号 × stride，见 §5）。

### 3.4 home 与根表

- 每帧有 `__extc_home`：调用者选择的最长寿 arena；`main` 的 home 就是它自己的 `&__extc_a[1]`（`codegen.c:5966-5986, 10079, 3637-3638` 一带的注释与实现）。
- 静态根表由编译器在**分配点**就地拼出（§7），长度上限 **4096**：超过则不发射触发点，并把 region 标成 `rcRootsUnreliable`（`codegen.c:4140-4165、4191-4200、4352-4387`）。

---

## §4 帧 arena（bump 分配器）

### 4.1 结构

生成物里的两个类型（`codegen.c:12410-12416` 一带的 `PRIM_VAR`）：

```c
typedef struct extc_ablock { struct extc_ablock *prev; int fresh; int64_t cap, used; char data[1]; } extc_ablock;
typedef struct extc_arena { extc_ablock *top; extc_ablock *spare; } extc_arena;
```

- 单向块链 `top`（先入后出），每块 `calloc` 得到 ⇒ 数据全域为 0。
- `fresh = 1` 表示"这块从没被写过"；从 `spare` 复用的块 `fresh = 0`。
- `spare`：`extc_arena_release` 回收时**保留一块**（cap ≤ `EXTC_ARENA_SPARE_MAX = 1 MiB`，`codegen.c:12417-12418（宏 `EXTC_ARENA_SPARE_MAX (1 << 20)`）+ 12410（`extc_arena_release` 的保留逻辑）`）留作下次复用，其余 `free`；`extc_arena_destroy` 连 spare 一起 `free`。

### 4.2 分配算法（逐字对应 `codegen.c:12442`）

1. `n <= 0` ⇒ `n = 1`；`n = (n + 7) & ~7`（**8 字节对齐**）。
2. 若 `top == NULL` 且有 `spare`：把 spare 扶正为 top，`used = 0`（只在 arena 空时采用 spare —— 一次指针判空，热路径便宜）。
3. 若 `top` 为空或剩余 `cap - used < n`：新开块，`cap = max(n, 64)`；若已有 top 且 `top->cap < 1 MiB` 且 `cap < 2*top->cap`，则 `cap = 2*top->cap`（**倍增，但顶到 1 MiB 就停止倍增**）；`calloc(1, sizeof(extc_ablock) + cap)`，失败即 **trap "out of arena memory"**（唯一的 arena trap）。
4. 返回 `top->data + top->used`，`used += n`；**仅当 `!fresh` 时 `memset(p, 0, n)`**（新块由 `calloc` 保证全零，只清"会被读到"的那部分）。

> **关于"清零承诺"**：SPEC §0.6 承诺"读一个生命周期已结束的字节读到的是已初始化字节"。实现靠两条：新块 `calloc` 全零；spare 复用块在交出前 `memset`。这也是"32 MiB 的 `new` 只为触碰到的部分付页"的原因（``codegen.c:12442` 的 `extc_arena_alloc` 发射文本（注释位于该 prim 内）` 注释）。

### 4.3 释放点

- **块级**：块**进入**由 `genBlockBody` 直接 emit `cgArenaRelease`（`codegen.c:6972-6992`，6964/6981），**离开**才走 `cgReleaseLevel`（`codegen.c:6308-6322`）→ `cgArenaRelease`（6362-6371）；两处各发一次 `extc_arena_release(&__extc_a[lvl])`。产物 `evidence/example-alloc-in-block.c:245/247/252/258-259（进入 release 245、alloc 247（值写 249）、离开 release 252、destroy 258-259）`：

  ```c
  247:        extc_arena_release(&__extc_a[2]);   /* 块进入：丢掉上一轮（循环上一趟）的内容 */
  249:        int32_t * q = (int32_t *)extc_arena_alloc(&__extc_a[2], (int64_t)(n) * (int64_t)sizeof(int32_t), "...", 16);
  268:        extc_arena_release(&__extc_a[2]);   /* 块离开 */
  ```

  进入时释放是**循环复用**的关键：`while` 体的 arena 在每一趟开头被清空，所以"循环里 `new` 不会涨内存"（`examples/new.extc` 注释与产物一致）。
- **帧级**：函数返回路径上 `extc_arena_destroy(&__extc_a[1..maxLv])`（`evidence/example-alloc-in-block.c:269-270`，`evidence/example-new.c:3057,3059`）。

### 4.4 上界（结论）

单个作用域 arena 的内存 = `Σ` 该作用域内所有块的 cap，且每块 cap ≤ max(请求, 前块的两倍, 64)（1 MiB 之前）/ = 请求（1 MiB 之后）。因此：

- **每块级 arena**：上界 ≈ 该块一趟内的累计分配字节 × (1 + 常数)，且**在块结束时归零**（spare 最多留 1 MiB）。
- **帧 arena（level 1..maxLv）**：上界 ≈ 该帧所有块的最大累计使用；循环不会累积（每轮 release）。
- **特例**：若某帧在其**函数体层（level 1）**持续分配而对象又不可达，这些分配**不在 arena**（level==1 的 `new` 走 rc，见 §2），所以"arena 无界增长"只可能来自 level≥2 的块内分配在同一趟内无界地累加（例如一个 `while` 体的一个块里做 N 次分配而不出块）。

---

## §5 rc region：身份、slab 几何、分配快路径

### 5.1 region 的身份与惰性 claim

- 运行期有一张 **512 项**的静态注册表：`EXTC_RC_REG_MAX 512`，`key[]` / `val[]` 两个数组（`codegen.c:12738-12742`）。
- 键 = **arena 的地址**（`&__extc_a[k]`）。序言里每帧对每个用到的 level 调一次 `extc_rc_regionOf(&__extc_a[k], k)`（产物 `evidence/example-new.c:2989-2990`）：**线性扫表**，命中即返回；未命中则 `extc_rc_regionNew(rid)` 建一个并登记（表满则**不登记** —— 该 region 之后不会被 drop，等价泄漏，且"宁可漏不可早放"）。
- 分片（`EXTC_RC_SHARDS>1`）时一个逻辑 region 按分配点 `rcSite++ % N` 切成 N 个运行期 region，键 = `地址 + k`，**必须逐个 drop**（`codegen.c:7512-7513` 为逐字注释：key = address + k … every shard must be dropped individually；函数 7511、逐 shard drop 循环 `7527-7528`；12736-12767 是 regionOf/regionDrop 函数体）。
- **drop 只在帧销毁**（`cgReclaimDropArena`，`codegen.c:7522-7540（函数体）、7504-7505（分片循环）`；产物 `evidence/example-new.c:3056,3058`）：`extc_rc_regionDrop(key)` → 查表 → `extc_rc_regionFree`。块级 release **不** drop region，否则循环体每轮会新建一块 slab（注释明确）。

### 5.2 slab 几何（`reclaim.c:841-915`）

| 项 | 值/规则 | 出处 |
|---|---|---|
| slab 对齐与总尺寸 | `EXTC_RC_SLAB_BITS = 19` ⇒ `EXTC_RC_SLAB_SIZE = 512 KiB`，`aligned_alloc(512 KiB, 512 KiB)`（v12 把栅格从 1 MiB 降到 128 KiB，见 [`P1-SLAB-QUANTUM.md`](P1-SLAB-QUANTUM.md)；v13 提到 512 KiB，见 [`P1-ZEROINIT-FILL.md`](P1-ZEROINIT-FILL.md)） | `reclaim.c:138/339/862`（`aligned_alloc` 在 862；893 是 `ttab[1] = EXTC_RC_CONSERVATIVE`） |
| 头部 | `EXTC_RC_SLAB_HDR = align64(sizeof(ExtcRcSlab))` | `reclaim.c:339` |
| 槽步长 `ss` | 请求长度向上取 8 的倍数，**非 2 的幂** | `reclaim.c:1575（`slotSize` 调用；1293 是 `if (r->sweeping)`）`（分配路径） |
| 块初始尺寸 | `chunk = EXTC_RC_SLAB_CHUNK_MIN = EXTC_RC_SLAB_SIZE`（512 KiB，v13 起**整块发给同一尺寸类**，块尾没有洞）；倍增循环 `while (chunk < SLAB_SIZE && chunk/ss < SLOTS_MIN)` 只在把 `SLAB_BITS` / `SLOTS_MIN` 调小到块装不下 1024 槽时才生效 | `reclaim.c:345-346,845-850` |
| 槽数 | `cap = (chunk - HDR) / ss`；`cap < 2` ⇒ **huge block**：头部照建，payload 单独 `malloc`，`nslots = 1` | `reclaim.c:851-867` |
| 位图 | `occ`、`mark`、`rsbit` 各 `nwords = (nslots+63)/64` 个 `uint64_t`，**calloc**（要整片清） | `reclaim.c:879-881` |
| 每槽元数据 | `tid`（2 B，类型号）总是 `malloc`；`hold`（2 B 计数）**仅 NOCOUNT=0 时分配**；`rscnt` 懒惰建 | `reclaim.c:883-896（hold 注释/malloc 823-825、tid 827、ttab 828-831；rscnt 懒建 1401-1407）` |
| 类型表 | `ttab` 容量 8，`ttab[1] = EXTC_RC_CONSERVATIVE`（号 1 固定是保守描述符） | `reclaim.c:887-891（828 ttabCap=8、829 ttabN=2、830 calloc、831 CONSERVATIVE；833-837 是失败守卫）` |
| 复用链 | `freelist` 初始 -1；**不预链全块**，"未发放的槽走 bump，只有被释放的槽进 freelist"，链值存在 `tid[]`（槽号+1）而不是 payload | `reclaim.c:904-906（`freelist = -1`、`bump = 0`；857-886 已属 classBucket/classPush）` |
| 槽号上限 | `EXTC_RC_REUSE_MAX_SLOT = 0xFFFE`，超了 `oops`（复用链编码上限） | `reclaim.c:349（常量）+ 870（守卫）` |

**为什么"块对齐"重要**：从指针反查 slab 只需一次位与（`EXTC_RC_SLAB_DATA`/`SlabOf`，`reclaim.c:353/361`；`regionOfOwn` 见产物 `evidence/example-new.c:1034`）。对齐单位就是 `EXTC_RC_SLAB_SIZE`（v12 起 128 KiB），所以"一个对齐块 = 一个 slab"是**承重语义**：只能整体缩小栅格，不能把一个大块切成多块（否则反查要做除法或查表）。块**整块**发给一个尺寸类（v13）解决的是另一件事：chunk 小于块时，48 B 槽的类只发 1 364 个槽（85 KiB）就停，一块 512 KiB 里 83% 是洞，分配要跨 ~4 倍地址空间；整块发槽之后每块只有块尾不足一个槽的残渣。代价见 §19 的步长口径：一次分配能资助的 sweep 收尾信用不再被块几何自动限住，必须用 `EXTC_RC_SWEEP_FINISH_MAX` 显式夹住（`reclaim.c:1069`）。

### 5.3 大小类表

- `ExtcRcClass { ss, head, next }`，32 个桶（`EXTC_RC_CLS_BUCKETS 32`，`reclaim.c:213`）。
- 桶号 = `((ss >> 3) * 0x9E3779B97F4A7C15) >> 59`（Knuth 乘取高 5 位；低比特对小键分布差，不用）。
- `head` 链 = "**可能还有空槽**的 slab"；满块**懒弹出**（分配时发现满才处理）。这是为了避免"线性扫 region 的 slab 链找空位"退化成 O(活 slab 数)——注释记录了 3200 万次分配 / ≈8000 块时退化成二次方的历史。

### 5.4 分配快路径（`reclaim.c:1489-1680`，产物 `evidence/example-new.c:945` 起）

顺序（每一步都是逐字的）：

1. `extc_rc_notePart(r)`：登记"本 region 参与过 rc"（只有**一个** region 参与时，赋值路径可跳过跨区查找）。
2. 若本 region 正在清扫：`extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET)` —— **先还清扫债**。
3. 否则若正在标记：`extc_rc_markAdvance(r)` —— **推进标记**（§7）。
4. 取大小类：`classSlot(r, ss)` → 命中则从 `freelist` 取一个槽（复用）；否则 `bump` 取未发放槽；都没有 → `slabNew(r, ss)`（冷路径：512 KiB `aligned_alloc` + 3 个位图 `calloc` + `tid` `malloc`）。
5. `extc_rc_zeroSmall(p, ss)`：**整个槽**清零（新对象的字段默认值）。实现是 `switch (n >> 3)` 的 16→1 fallthrough 跳表、逐 8 字节写 0（`reclaim.c:1507-1529`），`default` 才回退 `memset`；不走 libc 是因为 fortify 会把定长 `memset` 变成 `__memset_chk` 调用（sizeratio 形状上占 11.70% 的 Ir，实测见 `P1-ZEROINIT-FILL.md`）。
6. `occ |= bit`；若正在标记，`mark |= bit`（只对**清扫游标尚未到达**的位置补黑，见 §9.3）。
7. `tid[i] = typeIdx`（类型号；0 = 无描述符）。
8. `bytes += ss; allocBytes += ss`。

**分配成本**：热路径是 O(1)（几次位运算 + freelist 弹一个），但清零是 O(ss)（每个对象都付，实测 ≈1.9 ns/次 on 16 B 槽的跳表路径 vs ≈7 ns on 库调用）；冷路径是新 slab（实测 ~65 µs 的尖峰来源，见 §19.4）。**没有任何"分配失败"路径**：OOM 一律 `extc_rc_oops` → 打印后 `extc_die(1)`。

---


## §6 引用槽的写屏障（什么时候动手、动哪只手）

### 6.1 运行期入口

产物里的引用写函数（源 `src/back/reclaim.c`；产物里同名函数见 `docs/analysis/evidence/example-new.c`）：

| 入口 | 源码行 | 何时由编译器选中 | 做了什么 |
|---|---|---|---|
| `extc_rc_initRef` | `reclaim.c:2225-2232` | 新槽**首次**赋值 | 目标计数 +1（无旧值）；无跨区登记（新槽没有旧边） |
| `extc_rc_initRefNoHold` | `reclaim.c:2234-2240` | 检查器证明该槽"没有任何 dec"（`planRcNoDec`） | 不增计数，只写值 |
| `extc_rc_storeRef` | `reclaim.c:2242-2258` | 一般引用赋值 | `shade(v)`、`shadeOld(slot)`、写、`inc(v)`、`dec(old)`；**NOCOUNT=1 时 inc/dec 是空函数** ⇒ 净效果 = 屏障 + 裸写 |
| `extc_rc_storeRefOwned` | `reclaim.c:2336-2392` | 编译器能证明**宿主 region** | 同上 + 跨区时 `rsIncSlab` 并置 `host->foreign=1`；旧值只在 `host->foreign` 时才查（foreign 单调置位、从不清） |
| `extc_rc_storeRefOwnedReg` | `reclaim.c:2267-2313` | 已知宿主 region 且已查表 | 同上 |
| `extc_rc_storeRefUnknown` | `reclaim.c:2502-2512` | 源不可证（仅由 `storeRefSlot` 兜底可达） | `pinAdd(v)`；**失败（OOM）⇒ 目标 region `noSweep=1`**，再 `storeRef` |
| `extc_rc_storeRefSlot` | `reclaim.c:2539-2552` | 不知道槽属于哪个 region | 先 `slabOfInterior` 反查宿主 slab：查到 → OwnedReg；查不到 → Unknown |
| `extc_rc_storeStructCons` | `reclaim.c:2554-2572` | 整值 struct 赋值、叶子不可枚举 | 逐 word 当候选引用**只增不减** + 登记跨区边；`hostKnown==0` ⇒ 目标 region `noSweep=1` |
| `extc_rc_storeStructRooted` | `reclaim.c:2622-2630` | 整值赋值，但根表已按所有 variant 静态展开 | 逐 word 只增；**不**退出 sweep、**不**登记跨区边 |

编译器侧如何在这些入口之间**静态分派**有 8 种形态（`codegen.c:8914-9084`，逐条见 `docs/analysis/COMPILE-SIDE.md` §7）。关键事实：`storeRefUnknown` **没有直接发射点**，只能由 `storeRefSlot` 的运行期回退到达 —— 这决定了 `noSweep`（永久退出清扫）在正常形状下不会触发。

### 6.2 屏障的实际成本：只在标记在飞时付

- `extc_rc_shade` 快路径（`reclaim.c:2201-2214`）：`if (!extc_rc_wb || !v) return;` —— **没有标记在飞时两条测试就出门**。
- 慢路径 `shadeSlow`（`noinline`，`reclaim.c:2182-2200`）顺序：`!s || !s->region || (partN>1 && !region->marking)` ⇒ 返回；`mark` 位已置 ⇒ `shadeRemember` 后返回（多数调用在此退出）；未 `occ` ⇒ 返回；否则置 `mark`、`remember`、`region->wbDebt++`、`markPushForce`。
- `shade` 缓存（`reclaim.c:2140-2181`）：**2048 格 × 8 B = 16 KB 静态**；索引 `((addr>>3) + gen) & 2047`，代际旋进索引 ⇒ 同格同指针必属同代 ⇒ 命中精确；2048 代绕回前整表 `memset`。注释自称是 hint 不是 cache，冲突破坏无所谓；注释给出命中率 ≈ **72%**。
- `shadeOld`（`reclaim.c:2215-2222`）**只在 NOCOUNT=1（无计数档，即默认清扫档）时生效**：函数体由 `#if EXTC_RC_NOCOUNT` 包住，计数档下是空操作；作用是覆盖前 shade 旧值，防止"从没扫过的对象读到白对象、该边又被堆上删除"的病态漏标。

---

## §7 触发与推进：谁在什么时候做多少活

### 7.1 三处受治理分配点（逐字定义）

`codegen.c:14233-14251` 的注释给出全部三处，且只有这三处（原文节选）：

```
 *   - new T (single value or fixed-size new [N]T) -- the branch of exNew without a length;
 *   - alloc<T>(n) -- the arena branch of exGencall;
 *   - a **single-value** @overwrite cell -- that branch also goes through new T…
 *
 * A new T[n] without a length stays in the arena (a view is not a reference slot), and so does a
 * variable-length @overwrite cell, so neither counts.
```

扫描对象 = **即将发射的函数体** + 泛型实例方法体 + 全局初始化器（"appears in the program" 指出现在产物里；死函数里的 `new` 不带运行期）。判据函数 `rcAllocInExpr`（`codegen.c:14259-14273`）逐字：

```c
if (e->kind == EX_NEW) {
    if (!e->u.new_.count) return false;   /* a single object is an rc slot */
    return planArenaLevel(e) != 1;
}
if (e->kind == EX_GENCALL)
    return !(e->u.gencall.name && strcmp(e->u.gencall.name, "alloc") == 0);
```

即：单值 `new` **无条件**是 rc 槽；带长度的 `new` 仅当层号 == 1 才是 rc 槽；`alloc` 无条件算。

### 7.2 触发点的产物形态

产物（`evidence/example-new.c:2992/2998/3013/3025/3041`）：

```c
{ if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, (void *[]){ (void *)(a), (void *)(b) }, 2); }
node * b = ((node *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(node), &__extc_rct0));
```

三点必须看清：

1. 触发发生在**分配之前**，被分配的槽**还不在根表**里（所以它不可能是根）。
2. 根表是**该位置静态已知的活槽**（复合字面量），不是全帧根集；这就是"根表就地拼表"（`codegen.c:4006-4055`）。
3. 若该位置根表长度会超 **4096**，编译器**完全不发射触发点**，并把 region 标成 `rcRootsUnreliable`（`codegen.c:4140-4165、4191-4200、4352-4387`）⇒ 该 region 不再开新轮；并且 `codegen.c:7261` 连**定向释放也一并禁用**（`rcRootsUnreliable` 是函数/单元级标志，不是「该 region」级）⇒ 只剩帧销毁一条路。

### 7.3 触发判据（逐字公式）

`extc_rc_thrUpd`（`reclaim.c:3650-3699`）：

```c
int64_t scale = r->scale > 0 ? r->scale : 1;
int64_t thr = r->live * 2;
if (thr < r->floor) thr = r->floor;         /* floor 默认 1 MiB */
if (scale > 1 && thr > INT64_MAX / scale) { r->thr = 0; return; }
r->thr = thr * scale;
```

`extc_rc_shouldSweep`（`reclaim.c:3711-3721`）：

```c
if (r->noSweep) return 0;                   /* 计数层专用 region：默认配置下永不回收 */
#if EXTC_RC_SWEEP_FREE
    return 0;                               /* 整个程序被证明无环：不需要清扫 */
#else
    return r->bytes > r->thr && extc_rc_roundOk(r);
#endif
```

`extc_rc_roundOk`（`reclaim.c:3703-3710`）：`allocBytes - roundVisitedBytes >= max(live, floor)` —— **轮信用**（与定向释放的信用是两个独立计数器，共用会互相饿死）。

`scale` 的自适应（`reclaim.c:3613-3618`）：一轮扫完**什么也没释放** ⇒ `scale = (scale>0?scale:1) * EXTC_RC_SCALE_MUL(4)`（clamp `EXTC_RC_SCALE_MAX = 2^40`）；**释放了东西** ⇒ `scale = 0`（下一轮 thr 回到 `max(2×live, 1 MiB)`）。注释的判据：**"扫不到东西"说明这里没有环**，于是把触发线推远，避免"活集大、垃圾少"的负载每轮都做整堆标记。

### 7.4 "推进"有两个入口、单步预算不同

| 入口 | 源码 | 触发条件 | 单步标记预算 | 单步清扫 |
|---|---|---|---|---|
| 触发点 `extc_rc_step` | `reclaim.c:3938-3945`（由 `maybeCollect` 在轮在飞时调） | 每次受治理分配前 | `MARK_STEP(8) + min(wbDebt, 64)` ∈ **[8, 72]** 单位 | `sweepStep(SWEEP_BUDGET=8)` 若本轮已进入清扫 |
| 分配点 `markAdvance` | `reclaim.c:1380-1404`，由 `extc_rc_alloc` 调 | 每 `MARK_EVERY = 64` 次分配 | `min((64*8 + min(wbDebt,512)) × _pm, MARK_BATCH_MAX × _pm)`，`_pm` = 压力 ? `SWEEP_PRESSURE_MUL` : 1（`:1363`）⇒ 非压力步**恒 256**、压力步**恒 1024** 单位（`:1369/1368`） | 同上（清扫债优先，见 §5.4） |

`markAdvance` 的窗口增益在默认参数下**永不生效**：乘压力乘子后基数 `64*8*_pm = 512*_pm` 仍不小于 clamp 值 `MARK_BATCH_MAX*_pm = 256*_pm`，clamp 之后恒等于 `256*_pm`（`reclaim.c:1390/1367/1368`）——非压力步 256、压力步 1024；只有用 `-D` 把 `MARK_EVERY`/`MARK_STEP` 调小才会让 `EXTC_RC_MARK_WINDOW = 2 MiB` 的 ×8 生效。

`EXTC_RC_WORK_BUDGET`（默认 **0 = 关**）：关时三条预算（walk 256 / seed words 512 / seed pushes 256）**独立且在同一步内叠加**；开时四者共享一个总数 `wleft`，单步总工作量按构造 ≤ W。

### 7.5 谁有资格开启一轮：只扫"本帧拥有"的 region

- `rcFrameOwnsArena`（`codegen.c:3929-3965`）：只有能证明"本帧是该 region 的唯一扫者"才发触发点；否则发 `extc_rc_owe`（记一笔欠账）。
- 欠账由**属主帧**在它下一个触发点 `checkOwed` 结算（`codegen.c:4148-4161/4078-4098`）。
- `tlsDepth`（`reclaim.c:2932-2940`）：worker 入口帧（depth==0，唯一调用者是 C 运行时）可以清扫（外层没有 extC 活帧）；内层帧只能记 owe。

---

## §8 标记

### 8.1 轮的开始：`markBegin`（`reclaim.c:2941-3011`）

`msp = 0` → `scanObj = 0 / scanOff = 0`（上轮大对象分段游标不延续）→ 逐 `roots` push（**非空且 owns**；表外指针跳过，否则按位与会读无关内存）→ `pinSeed(r)` → `seedSlab = r->slabs, seedWord = 0, seedBits = 0` → `seedsDone = (slabs == NULL) || !seedAny`。

**根的四个来源，默认只有一个真的被读：**

| 来源 | 默认 | 说明 |
|---|---|---|
| 静态根表 `roots[]`（编译器生成、以参数传入） | **是，唯一实际种子** | 产物 `evidence/example-new.c:2998` 的复合字面量 |
| `pin` 表 | 否（只有"宿主不可追踪但值是 rc 对象"的路径） | 进程级开放寻址哈希，见 §11.2 |
| 帧根链 `[rt:chain]` | **未接线** | 结构体在 `reclaim.c:2900-2931` 存在，但 `codegen.c:12711-12716` 注释逐字："没有发射器往链里 push"，`frameRoots` 恒传 0，整段按 tier 不发射 |
| bump 区 | **未接线** | `reclaim.c:2679-2703` 有 `extc_rc_bump/bumpReset`，`codegen.c` 无任何引用 |

### 8.2 黑化：`MARK_SCAN` + `shade` 缓存

- `mark` 位图：置位 = 黑 = 可达。`MARK_SCAN`（`reclaim.c:3030-3098`）先判 `mark` 去重，再判 `occ`。
- 对象寻址：从 payload 指针 `>> SLAB_BITS` 得 slab，`(p - data) / ss`（`rcp` 倒数乘）得槽号。
- `shade` 缓存见 §6.2：2048 格、代际旋转、命中精确、≈72% 命中率。
- `markPushForce`：`shadeSlow` 在缓存未命中时把指针压栈；`markPush` 的 `realloc` 发生在 `markStep` 内（这是"单步可能远超预算"的失败模式之一，`reclaim.c` 注释把 `mscap` 列为该失败模式的判据）。

### 8.3 遍历顺序与批量

- 栈深 ≤ `EXTC_RC_MARK_SHALLOW = 64` 时走"浅栈直通"：逐个 pop **立即 scan**（`reclaim.c:3197/2764`），避免为小图付批量化开销。
- 否则先 pop 至多 `EXTC_RC_MARK_BATCH_POP = 200` 个对象（一起 prefetch），再逐个 scan（打断 LIFO 串行指针链）。注释明确：**批宽会改变遍历顺序 ⇒ 步边界、collect 时刻、峰值 RSS 都会移动**。
- `POP > BATCH_MAX` 直接 `#error`（`reclaim.c:1147`）。

### 8.4 精确 tracing vs 保守

| 类别 | 描述符 | 行为 |
|---|---|---|
| 精确 | `ExtcRcType{nptr, off[]}`，槽内 `tid` 2 B 指向 slab 的类型表 `ttab` | 逐字段读指针，只走 region 内的槽 |
| 保守 | `EXTC_RC_CONSERVATIVE = (const ExtcRcType*)-1`，固定为号 1 | **唯一场景**：`alloc<T>(n)` 且 T 含引用（元素 1..n-1 的偏移随运行期计数变，静态描述符表达不了）⇒ 整槽按 word 当候选引用扫，**只会多留不会漏** |
| 无描述符 | `tid == 0` | 数据对象，直接跳过（一个 word 都不读） |

- 指向 region **外**的对象：标记在该对象处**停止，且不写它的 mark 位**（别的 region 的黑化会让它下次 sweep 跳过并释放其子）。
- 大对象/保守对象：words > `EXTC_RC_SCAN_CHUNK = 8192`（64 KiB/段）时不进标记栈，只记 `scanObj/scanOff`，分段续扫（`reclaim.c:3058-3060`）。

### 8.5 两个"没有"

| 问题 | 答案 | 证据 |
|---|---|---|
| 是否移动对象 | **否**。无压缩、无 `memmove`；地址 = slab 基址 + 槽号 × stride，region 生命期内恒定；`markPush` 存裸指针、`pin` 表以值指针为键 | 全文件无搬移逻辑；地址只由 `rcp` 公式给出；slab 永不重定位（§5.2） |
| 是否 STW | **否**。标记与 mutator 在同一线程内交错推进（分配点 `markAdvance`、触发点 `step`），用写屏障维持三色不变式；没有任何暂停其他线程的代码 | §6 屏障 + §7 推进 |

**但“非 STW”不等于“停顿有界”** —— 见 §19：v8 起 `regionFree` 的同步段是 O(1)（closing queue，§19.5），`releaseOld` 有 2048 步预算；残余的界来自**段级扫描成本**（§19.4 的 `c_c·w`）与配额 deadline。诊断接口 `settle/collect` 在生产路径上无调用者。

---

## §9 清扫

### 9.1 状态机

| 阶段 | 源码 | 动作 |
|---|---|---|
| 开始 | `reclaim.c:3531-3544 sweepBegin` | `shadeNextGen()`（清 shade 缓存代）；`sweeping=1`；`swId++`；`swSlab=r->slabs`；`swWord=0`；`swFreed=0` |
| 扫一个 word | `reclaim.c:3546-3557 sweepWord` | `dead = occ[w] & ~mark[w]`；非 0 ⇒ `freeSlotWord`（批量头插 freelist + 每词一次记账）累加 `swFreed`；`mark[w]=0`（**只清当前 word**）；`++swWord` 到头则 `s->swEpoch=swId`、`swSlab=s->next`、`swWord=0` |
| 推进 | `reclaim.c:3561-3623 sweepStep` | `while (left>0 && swSlab)` 每个 word 花 1 单位；扫完（`swSlab==NULL`）才结算 |
| 结算 | `reclaim.c:3613-3618` | `sweeping=0`；`swFreed==0` ⇒ `scale*=4`（clamp）否则 `scale=0`；`thrUpd`；`sweeps++`；返回 1 |

### 9.2 步长与成本

- `EXTC_RC_SWEEP_BUDGET = 8` ⇒ 每次 `sweepStep` 最多扫 **8 个 64-bit word = 512 个槽**（`reclaim.c:1034`）。
- `freeSlotWord` 是**整词批量**（`reclaim.c:1896-1935`）：word 里每个 dead 槽头插进 `freelist`，链的下一个槽号写在 `tid[]`（槽号+1，0 = 尾），不写 payload（避免 RFO 整个 cache line）。
- **成本 ∝ 槽数，不是常数**：一个 8-word 步可能释放 512 个槽，每个槽一次链插入 + 记账。仓库自测（`tests/reclaim-perf/SWEEP-CENSUS.md`）每槽有效 **0.61–0.89 ns/槽** ⇒ 512 槽约 0.3–0.45 µs；但**大槽（huge block）与 `unlinkChildren` 的字段遍历不在这个账里**。

### 9.3 sweep 与 mark 位图的耦合（三条后果）

1. `mark` 位图按 word 顺序被清 ⇒ **标记必须从干净的 mark 位图开始**；清扫未完成时触发点只续扫、**不新开标记**（`reclaim.c:3457-3460` 注释）。
2. **清扫期间分配的槽必须在分配点补黑**（`reclaim.c:1668-1672`），且**只标清扫游标尚未到达的位置**（已扫过的位置标了会留下无人清的陈旧位）。判据是 `s->swEpoch` 与 `swId`。
3. 定向释放自己负责清整片 `mark`（`reclaim.c:3400-3402`），理由同上。`freeSlot` 故意**不清** `mark`（free 槽不参与本轮标记，下一轮扫到该 word 时顺手清）。

### 9.4 slab 永不归还

- 全文件 **无 `munmap` / 无 `madvise` / 无缩容**（grep 实证）。`sweep` 只把槽放回复用链；整块空闲时"所有槽都在 freelist 上"，slab 仍挂在 `region->slabs` 与大小类链上。
- slab 的**唯一**释放路径是 closing queue 的排空：`extc_rc_regionFree`（帧销毁，`reclaim.c:2769-2862`）只置 `r->dead` 并挂队；`extc_rc_cqDrainCore`（`reclaim.c:1811-1854`）逐 slab 做 `rsIncN≠0` 时的 `unlinkChildren`、`ownDel`、压入 free queue；最后由 `extc_rc_fqDrainCore` `free` 位图、`tid/ttab/thash`、huge 的 payload、slab 本身。
- 结论：**region 的 slab 峰值 = 该 region 生命期内的峰值**，且峰值由分配速率与触发阈值决定 —— 这是"内存没有常量上界"的直接来源（§18）。

---


### 9.5 slab 级省略：整块全死时一次决策（P1-7，仅默认无计数档）

| 项 | 内容 |
|---|---|
| 位置 | `reclaim.c:3474-3524 extc_rc_sweepElide`（包在 `#if EXTC_RC_NOCOUNT` 内；计数档是 stub，`reclaim.c:3525-3529`） |
| 调用点 | `reclaim.c:3571`：`sweepStep` 在 slab 边界（`swWord==0`）先试；命中则 `_left--`、`_trans++`、`continue`（一个预算单位换掉整块） |
| 判据 | ① 有当前 slab（`!s` ⇒ 返回 0）② `!s->big`（huge block 不参与）③ `!r->foreign` ④ 逐 word 要求 `!(occ[w] & mark[w])`（任何可达位 ⇒ 放弃）且 `!s->rsbit[w]`（任何跨区登记 ⇒ 放弃）⑤ 计数档直接返回 0 |
| 命中动作 | 记账 `bytes -= live*ss`、`freedSlots += live`、`swFreed += live`（`live` = 全块 occ 的 popcount）；`bump=0`、`freelist=-1`；`memset(occ)` / `memset(mark)`；`!s->avail` 则 `classPush`；`s->swEpoch = swId`；游标推进到 `s->next` |
| 成本 | 判定是 O(nwords) 一次过：512 KiB slab × 2 位图，16 B 槽 ⇒ ≤512 word（v12 的 128 KiB 栅格是 ≤128 word、≈0.3–0.4 µs ⇒ 同比例约为其 4 倍）；1 MiB 栅格时代实测 cycleratio 单步 max ≤9.9 µs、步数 1,221 → 255 |
| 与 §9.3 不变式的关系 | 判据要求 `mark` 全 0 ⇒ 不会把「清扫期间分配补黑」的槽判死（那种槽 `occ & mark ≠ 0`）；`rsbit` 全 0 ⇒ 不会有跨区入边指向被重置的槽 |
| 实测收益 | cycleratio（`bench/gc/v2` bench_args）Ir 274,116,151 → 264,298,098（**−3.56%**），切片数 −4.8×，`[histo]` 的 b32us 桶 2 → 0；`[sweepelide]` 实测 `bulk_reset=651 bulk_slots=1,739,978 swept=105`，与独立探针的 `full_dead=651 / full_dead_slots=1,739,978` 逐位一致 |
| 限制 | `foreign` 一旦置位，该 region 永久放弃本优化；`rsIncN != 0`、`big`、计数档都不参与；槽很大时（整块槽数很少）判定本身可能比逐槽扫更贵 |

这一小节给出的结论比补丁本身重要：**清扫的「快速路径」只能建立在「整块死」之上**。promote 一类的形状（长短命对象在 slab 内交错，实测 879 次 slab 访问里 **0 次全死**）拿不到任何收益，其清扫成本必然 ∝ 死槽数 ⇒ 想降 promote 的清扫税必须让「释放单个死槽」更便宜，而不是跳过扫描（§18.3-1 的上界与 §19.4 的 `c_f` 项对应这一条）。

## §10 定向释放：trial deletion（唯一"提前释放"的路径）

### 10.1 什么时候被发射

`storeRefOwnedRelease` 由 `codegen.c:7218-7292` 的注释定义（发射在 `codegen.c:7290`），四个 criterion **全部**成立才发射：

1. 赋值在**循环体内**；
2. 目标槽是**本帧的引用槽**，且声明在**所有已注册循环之外**（每轮都是同一个槽）；
3. 写入值是**本帧 fresh 分配、从未重绑定、且其 region 由本帧拥有**；
4. 该对象**已被写进某个堆对象的引用字段**（即在静态根表之外还有别的边）。

禁用条件（任一成立就不发射）：`rcSweepFree==1`、`!needReclaim`、`rcRootsUnreliable`、`coroFunc`、`rcNShard>1`、根表颗数 > 4096。

产物样本形态（`/tmp/cf2/struct_copy.c:3408`）：

```c
extc_rc_storeRefOwnedRelease((void **)&(keeper), (void *)(t), (void *)0, __extc_rcr1,
                             (void *[]){ (void *)(pad), (void *)(s), (void *)(t) }, 3);
```

### 10.2 运行期入口与三道闸

| 步骤 | 源码 | 动作 |
|---|---|---|
| 入口 | `reclaim.c:3437-3446` | `shade(v)` → `releaseDead(home, *slot, roots, nroots)` → `storeRefOwned(slot, v, owner)` |
| 闸 1 | `reclaim.c:3426` | `home->noSweep \|\| home->owe` ⇒ **直接返回**（根不完整，不准证明死亡） |
| 闸 2 | `reclaim.c:3428-3430` | 旧值必须 `ownSlab(old)` 且 `s->region == home`，否则返回（旧值不在本 region，保守跳过） |
| 闸 3 | `reclaim.c:3431` | `!budgetOk(home)` ⇒ 返回；否则 `releaseOld` 后 `home->visitedBytes = home->allocBytes` |
| 信用 | `reclaim.c:2726-2738` | `budgetOk: allocBytes - visitedBytes >= max(live, floor)` —— **每花一次 O(live) 遍历，先要攒够 max(live, floor) 字节的分配** |

### 10.3 主体（`reclaim.c:3342-3398`）

1. `extc_rc_mark(r, roots, nroots)`：与 `collect` 用**同一 seed 集**先把所有根可达物标黑。
2. `push(old)`。
3. 循环 `pop`，判据（`2463-2483`）逐条：不在本 region / 非槽首 / 未 `occ` ⇒ **停**（区域外当活物）；**`mark` 位已置 ⇒ 停**（还有别的根或种子指向它，且它的子树已在第 1 步标黑）；否则**先按描述符 push children、再 `freeSlot`**（描述符与字段必须还完整 —— `freeSlot` 会覆写 payload 头）。
4. 收尾（`2485-2487`）：`shadeNextGen()`，并对每个 slab `memset(mark, 0, nwords)`（`mark` 位只有 sweep 会清，这里不清会留给下一轮当活物）。
5. 统计（`2488-2489`）：`relN++`；`relSlots += freed`。

**安全性论证**（`reclaim.c:3342-3345` 逐字要点）：释放集是"同一时刻、同一 seed 集下 `collect` 会释放的集合"的**子集**（轻量级 trial deletion）；判据只决定"这一趟值不值"；判多只可能来自不完整的 roots/seeds，而那是 `collect` 的前置条件本身，由 `noSweep/owe`/帧所有权守着。

**谁有资格证明子树死亡**：不是计数。而是同时满足——覆盖发生在带 home 的槽上 + 该帧静态根表对 home 完整（无 noSweep/owe）+ 旧值确在本 region + 已攒够一次 O(live) 遍历的信用 + 旧值在同一 seed 集下不可达。

### 10.4 成本：**O(子树)，无界**

`releaseOld` 是"标记一遍 + 逐个 freeSlot"，成本 ∝ 被释放子树的槽数（且每个槽 freeSlot 还要 `unlinkChildren` 撤销跨区边）。它**不是**被切碎的增量工作：一次调用在一处完成全部标记与全部槽释放。信用只限制**频率**（攒够 max(live,floor) 字节分配才准做一次），不限制**单次时长**。这就是 §19 里"停顿无界"的两个来源之一。

它与计数层的关系：计数层（NOCOUNT=0）另有一条**独立**的"hold 减到 0 当场 freeSlot"路径（`reclaim.c:2077-2109`），两者互不替代。

---

## §11 跨区指针、pin、noSweep

### 11.1 跨区边：`rsbit` 是缓存，`rscnt` 是精确计数

| 机制 | 源码 | 语义 |
|---|---|---|
| `rsbit` | 位图 `reclaim.c:221-227`；维护点 `1415/1426/1490` | "该目标槽有**活**跨区入边"的缓存；0→1 置位、1→0 清位；**饱和值粘住永不减** |
| `rscnt` | 同上；**懒惰** calloc（`1166-1172`） | 每槽精确入边数（`uint16` 饱和 65535）；从没跨过区的 slab 不建表（省 2 B/槽） |
| 边的三处转移 | `reclaim.c:1687-1691`（3 条逐字）；实现 `1657-1725` | (1) `storeRefOwned` 一次赋值替换一条边（旧减新增，仅跨区）；(2) `freeSlot` 释放对象时丢掉它持有的跨区边；(3) region 被丢弃时按**仍活**对象批量减（v8 起发生在 closing queue 排空里，`reclaim.c:1826-1833`） |
| 撤销时机 | `reclaim.c:1772-1856 unlinkChildren` | 槽"即将不再是对象"前撤销其出边：字段与描述符必须还完整 ⇒ 所有调用点在写 free 链与清描述符**之前** |
| 为什么不是只置不清 | `reclaim.c:1691` 注释 | 只置不清会让种子涨到每个对象；现在的登记位数 = 当前活跨区边数，与轮数解耦 |
| 高水位 | `reclaim.c:1696` `rsIncN/rsDecN/rsHwm` | `rs_live = inc-dec` = 当前活跨区边数；判据看 `rs_hwm` |
| 运行期守卫 | `reclaim.c:1713-1720` `EXTC_RC_FOREIGN_GUARD`（默认 0） | 在"因本 region 无外来指针而跳过旧值查找"的点重算旧值 region 并计数；出现未登记的外部旧值则 `bad>0` |

**种子=跨区入边**：`markBegin` 里 `seedSlab/seedWord/seedBits` 就是把本 region 每个 `rsbit` 置位的槽当种子 push。`seedsDone = (slabs==NULL) || !seedAny` ⇒ 从没被跨区指过的 region，整轮**跳过种子扫描**。

### 11.2 pin：把"值本身"钉成根

| 机制 | 源码 | 语义 |
|---|---|---|
| 动机 | `reclaim.c:2378-2391` 注释 | 宿主（view 元素 / pool buffer 槽）编译期不可追踪，但值本身是 rc 对象。老办法把**整个目标 region** 置 `noSweep`；新办法把**值本身**钉成根 |
| 结构 | `reclaim.c:2393-2456` | 进程级开放寻址哈希（键=值指针，load ≤ 1/2，删除用 Knuth 后移），每项存 `v/rg/n`（`rg` 登记时用 `ownSlab` 算一次，轮起/region 死只比 region 指针） |
| 方向 | `reclaim.c:2389` | 只有 pin **不可能**（分配失败）才退回 `noSweep` —— 宁少收，不可错杀 |
| 线程 | `reclaim.c:2391` | pin 表**进程级**（region 可能被别的 worker sweep），insert/remove/轮起加锁；**热路径永不碰** |
| `pinAdd` | `reclaim.c:2436-2457` | `!v \|\| !owns(v)` ⇒ 返回 1（不是活槽）；加锁；负载 > 1/2 ⇒ `pinGrow`（失败返回 0）；已存在则 `n++`；否则记 `v/rg/n=1` |
| `pinSub` | `reclaim.c:2458-2471` | `n--` 或擦除（Knuth 后移） |
| `pinDropRegion` | `reclaim.c:2473-2488` | region 死时删掉它名下所有 pin（由 `regionFree` 调） |
| `pinSeed` | `reclaim.c:2490-2500`（`noinline`） | 把本 region 的 pinned 值 push 进标记栈；`pinN==0` 时一次原子读即返回（最常见情况） |

### 11.3 noSweep：永久退出 sweep

| 触发条件 | 源码 | 后果 |
|---|---|---|
| `storeRefUnknown` 的 `pinAdd` 失败 | `reclaim.c:2502-2512`（`2376-2378` 置位） | 目标 region `noSweep=1` |
| `storeStructCons` 且 `hostKnown==0` | `reclaim.c:2554-2572`（`2438` 置位） | 同上 |
| 帧根链存在 bad 帧 | `reclaim.c:2918-2930 chainBad` | 调用方拒绝 sweep（同一决策） |
| **清除条件** | grep 实证：全文件只有 `1761/1821` 两处置位，**没有任何一处清零**（除 region 整体 `regionFree` 被销毁） | 该 region 在生命周期内**永久**不再 sweep |

`noSweep` 的实际效果（逐点）：

- `shouldSweep` 直接返回 0（`reclaim.c:3712`，注释 "reclaimed by counting only"）⇒ 永不触发新轮。
- 分配点若发现 `sweeping` 在飞 ⇒ **当场作废**、一个字节不再扫（`1063`）。
- `maybeCollect` 的 sweeping 分支 ⇒ 作废并返回 -1（`2785`）；`checkOwed` 同（`2825`）。
- `settle` 直接放弃（`2621`）；定向释放的闸 1 拒绝（`2509`）。
- **档 2（默认清扫档）NOCOUNT=1 时计数层整体编译掉** ⇒ noSweep 的 region **永远不会回收**：上界退化为"所有曾走 `storeRefUnknown` / `storeStructCons(hostKnown=0)` 的 region 的 `allocBytes` 总和"，且与触发阈值无关。
- 默认代码里触发有多常见：`storeRefUnknown` 只由 `storeRefSlot` 兜底调用，能证明宿主时走 `OwnedReg` ⇒ 正常形状不触发；"槽指针来自 C/库代码"的形状会触发（**未量化**）。

---

## §12 计数层（仅 `EXTC_RC_NOCOUNT=0` 时存在）

### 12.1 默认（清扫档）：整层被编译掉

`reclaim.c:2065-2073` 是 `[rt:nocount]` 段的开关：`EXTC_RC_NOCOUNT=1` 时 `holdUpdSlab/holdUpd/inc/dec` 都是空函数（`1935-1942`），而且发射器在发射期把整个 `#if` 分支折叠掉（`rtCfgIf 67-88`）——产物里连空函数都没有。

注意：本节三点只描述**档 2（清扫档）**。事件档的默认值是 `EXTC_RC_NOCOUNT=0` / `EXTC_RC_SWEEP_FREE=1`（自动升级见 §1.5），产物第 13/15 行随之为 0/1，计数入口真实存在。

清扫档的默认值链路（三点，均已实测）：

1. `codegen.c:14105` `g->rcNoCount = 1`（注释："Default 1 = no counting, reclaimed by sweeping alone"）。
2. 发射器 `reclaim.c:4099` 写出 `#define EXTC_RC_NOCOUNT 1`（=0 时写 0）。
3. 运行时 `reclaim.c:2065` `#if EXTC_RC_NOCOUNT` 把计数入口变成空操作。

反向开关：`EXTC_RC_NOCOUNT=0` ⇒ `codegen.c:14135-14138` 置 `rcNoCount=0`；`EXTC_RC_SWEEPFREE=1` 也强制 `rcNoCount=0`（`codegen.c:14152-14157`）。

### 12.2 计数语义（NOCOUNT=0）

| 机制 | 源码 | 语义 |
|---|---|---|
| 每槽 `hold` | `ExtcRcSlab.hold`（`uint16`/槽）；**只有 !NOCOUNT 才分配**：`slabNew 822-825` | 精确 holder 数，饱和 `EXTC_RC_HOLD_SAT=65535` |
| `holdUpd` 五步 | `reclaim.c:2091-2105` | ① `ownSlab(p)`，非本运行时槽 ⇒ 返回 ② `occ` 未置位 ⇒ 返回（free 槽不是活对象）③ `delta>0`：`hold!=SAT` 则 ++，**到顶粘住** ④ `delta<0`：`hold==0 \|\| hold==SAT` 直接返回（"饱和值永不再减"）⑤ `--` 后到 0 ⇒ **当场 `freeSlot`**（早收） |
| `holdUpdSlab` | `reclaim.c:2077-2089` | 已解析 slab 的版本，判据逐条相同（省一次 `ownSlab`） |
| `inc/dec` | `reclaim.c:2106-2107` | 只是 `holdUpd(p,+1)` / `holdUpd(p,-1)` 的别名 |

"三阶段饱和"（义务 3）：`hold == 0`（无入边）→ `1..65534`（精确）→ `== 65535`（**计数未知**，粘住永不减）。唯一目的：绝不把"未知"当成 0 而提前释放；这类槽只能交给 sweep。

### 12.3 计数只在"指针赋值"上动

`reclaim.c:2105` 逐字："Counting hooks only pointer assignments: data-field stores do not go through these two."

| 入口 | 源码 | 对计数的作用 |
|---|---|---|
| `initRef`（新槽首次赋值） | `1478-1484` | 写值 + `inc(v)`；**从不读旧值**（义务 1） |
| `initRefNoHold` | `1487-1492` | 只写不增；调用点由 `planRcNoDec` 选出（`codegen.c:7887-7890`），其动态范围内没有 dec，故不增不会失衡 |
| `storeRef` | `1495-1503` | `old=*slot`；写；`inc(v)`；`dec(old)` |
| `storeRefOwnedReg/Owned` | `1520-1559 / 1588-1629` | NOCOUNT=0 分支：`old/sold/sv` 三次 `ownSlab` + 跨区登记 + `holdUpdSlab(±1)`（`1541-1557`） |
| 定向释放后的旧槽 | `2504-2505` 注释 | 旧槽已 free，随后 `storeRefOwned` 的 `rsDecSlab/holdUpdSlab` 都会在 `occ` 行返回 ⇒ 不会二次结算 |

数据字段赋值不走 `inc/dec`（附加准则 `reclaim.c:123-126`）——所以计数**不跟踪** `memset`、结构体逐字节复制、外部 C 代码写入的引用；这类引用只能靠静态根表 + 跨区登记 + pin 覆盖。

### 12.4 与 sweep 的分工、以及计数不准的后果

| 情况 | 结论 | 证据 |
|---|---|---|
| 分工 | 标记/清扫**完全不依赖计数**；计数的唯一角色是"减到 0 立即回收" | `reclaim.c:2059-2064` 逐字 |
| 环 | 环内互相持有 ⇒ `hold` 不归零 ⇒ 早收永不发生 ⇒ 交给 sweep（设计如此） | 义务 3 后的边界注释 128-130 |
| 计数偏高 | 只会**晚收**；该 region 仍能 sweep 则无泄漏，只是峰值变高 | 同上 |
| 计数饱和 | 该槽永不早收 ⇒ 交给 sweep | 1351/1354 |
| 计数偏低 | 会**提前释放** = unsound。防线：`occ` 检查（1333/1349）、义务 1（新槽不读旧值）、`planRcNoDec` 判据 | 1333/1349、108-109 |
| region 是 noSweep / owe 长期未结 | 计数是**唯一**回收途径；计数偏高或成环 ⇒ **永久泄漏** | 2651、§11.3 |
| 代价 | `docs/plans/TODO.md` 称"计数层比清扫贵 +100%~+455%"，**本次未复核** | — |

TICK 调用点（默认 `EXTC_RC_TIME` 关时全部编译成 `do{}while(0)`，`reclaim.c:158-162`）：`alloc(0) 1290`、`storeRefOwnedReg(1) 1860`、`storeRefOwned(2) 1927`、`collect(3) 3442`、`slabNew(4) 783`、`freeSlot(5) 1481`、`regionNew(6) 2295`。

---

## §13 place 载荷区（`dyn` 的值住在 place 的 chunk 链里，`src/back/places.c` 发射）

- 由 `src/back/places.c`（315 行，md5 `7a618e8bb59338547c712c7d66734215`）**整段作为发射文本**写出：`PLACE_RT[]`（:34）是 place 槽表与 chunk 分配器，`PLACE_DYN_RT[]`（:180）是 `dyn` 对象表；两个入口 `placesEmitRuntime`（:295）/ `placesEmitDynRuntime`（:311）。底层只有 `malloc/realloc/free`。
- 一个 place = 槽表里的一个槽（`ExtcPlace{gen,parent,depth,live,chunks}`）+ 一条 **chunk 链**（`ExtcPlaceChunk{next,cap,used}`）。chunk 首块 4 KiB（`EXTC_PLACE_CHUNK_MIN`，:63），之后每块翻倍直到 256 KiB 并停在那里（`EXTC_PLACE_CHUNK_MAX`，:66），单次请求超过 256 KiB 时按请求字节单独给一块。`extc_place_alloc`（:150）按 16 字节对齐 bump；chunk 用 `calloc` ⇒ 新空间天然为零，语言「新存储读作 0」的语义不需要 memset。
- 槽表与栈动作：全局 `extc_places/extc_placeCap/extc_placeN/extc_placeFree/extc_placeTop`；槽从 free list（穿在 `parent`）复用，**每次出生 `gen++`** ⇒ 旧句柄的世代比较必然失败。`extc_place_enter`（:106）压栈并返回槽号作为 mark；`extc_place_leaveTo(mark)`（:117）沿 `parent` 链弹到 mark（含 mark），每个弹掉的 place 整条 chunk 链 `free`；`extc_place_newRoot`/`freeRoot`（:129/:130）给任务一个不在块栈上的 place；`extc_place_outer`/`depth`（:132/:134）、`live`/`gen`（:135/:139）、`count`/`isRoot`（:143/:144）。
- `dyn` 的值：句柄 `struct ExtcDynHandleS { zi, slot, gen, zgen }`（四个整数，拷贝不碰任何计数器 —— `dyn` 按设计到处被拷贝，这就是载荷不住 rc 槽的原因）；槽 `ExtcDynSlot{zi,zgen,gen,next,addr,vt}`。`extc_dyn_put`（:235）先验命名 place 还活着（否则 trap `a dyn value needs a live place to live in`），载荷直接 `extc_place_alloc(zi,size)` 后 memcpy；取用时 `extc_dyn_slot`（:273）依次校验 slot 范围、槽未回收、slot 号相同、槽世代、place 活、place 世代 —— 全部是**比较**，从不读已释放存储，失败 trap `stale dyn value`。
- 死槽回收：`extc_dynSweep`只在槽表满且 free list 为空时运行（:245）⇒ 从没堆出死句柄的程序不付清扫代价；判死条件 = place 不活，或 place 槽被重新发出（`zgen` 不等）。
- 回收方式与内存性质：place 结束（块退出 / 任务结束）时整条 chunk 链 `free`；**place 内部**死掉的 `dyn` 载荷不会单独回收（槽表只回收元数据，载荷字节留在 chunk 里直到 place 结束）。上界 = 该 place 内累计分配字节 + 每块余量（chunk 翻倍 ⇒ 余量 < 累计字节 + 256 KiB），place 结束归零；**有界性由作用域（place/task/frame 结束）给出，不由 GC 给出**。用户可见的池基质（`std::data::pool`、`std::sys::pool`、`std::data::place` 与 `@place` 容器）已退场，运行期只剩 `dyn` 载荷与 place 栈；`@place` 注解本身也在 v40 删除（写了报未知注解），容器存储由 `buf<T>` 地址预留与 `block<T>` 平坦块自己持有。

---

## §14 plate（`std.mem` 的地址保留层）

- `src/back/plate.c`（74 行）**不分配**，只发两个门：`extc_viewOf(uint8_t *p, int64_t n)` 造 `slice_u8 v = { p, n }`（`:35-38`，**零检查**）、`extc_memCopy`（`:69-73`，`memcpy`）。
- 真正的实现在**语言层库** `stdlib/std/mem.extc`（+ `stdlib/std/sys/heap.extc` 的 `mmap/mprotect/munmap` 外部声明）：`open` 一次 `mmap` 保留地址空间（`PROT_NONE + MAP_NORESERVE`，不占物理页）；`alloc` 把要用页 `mprotect` 成 RW（按 `CHUNK` 粒度，不是每页一次）；`close` 是 `munmap`。"一次保留、按需 commit、永不搬家"。
- 视图**不是根、不是 rc 槽**（`codegen.c:3664-3667` 注释：视图 data 指向 arena/plate/C 存储）。
- 内存性质：**有界**（= 每个板的 `reserved` 长度，用户显式给定）；生命周期由用户的 `close` 决定。

---

## §15 coroutine / parallel：每任务/每 worker 一个 arena

### 15.1 coroutine（`src/back/coroutine.c`，552 行）

- 任务表 = `realloc` 增长（`:106-114`），**不是** arena 分配。
- **每任务一个独立 arena**：`:57` `static extc_arena *extc_taskArena;`；分配门 `:124-131`（`extc_task_alloc ⇒ extc_arena_alloc`），注释逐字："A task's own place as an allocator: the frame of a boxed coroutine lives here, and so does anything the coroutine allocates with new (slice C's promotion)."
- 任务结束 `:145` `extc_arena_destroy(&extc_taskArena[id])`（**destroy 而不是 release**；注释：任务不会再跑，spare 块会永远占着）。
- 任务根 place：`extc_taskRoots[64]` + `extc_taskRootN`（`src/back/coroutine.c` 的静态数组）；`extc_task_collect` 只扫 `extc_place_isRoot(zi)` 为真的槽（= 活着的、`depth == 0` 的 place；`extc_place_newRoot` 只由 `extc_task_begin` 调用，`src/back/coroutine.c` 与 `src/back/places.c`），按「已登记根可达」判活，否则 `extc_task_end` + `extc_place_freeRoot`（``src/back/coroutine.c``）。语言表面：`syspool::collect()` 已随池模块删除；编译器在块退出处自己发 `extc_task_end`，逃出该证明的句柄由 `extc_task_collect` 在运行期按已登记根判活。
- 编译侧：`g->coroFunc ⇒ noArena=true`（`codegen.c:9945/9915`（判据/置位；`6275-6311` 是 `cgReleaseLevel` 注释与函数体、`9920-9931` 是 @overwrite 站点收集）；step 不拥有 arena 数组）；coroutine 帧**不走 rc**，且 coroutine 帧会让 region `rcRootsUnreliable`（`codegen.c:249-252（注释）、7600/7645（协程帧真实置位）；4371-4387 是 `rcRootLeaves` 的 4096 溢出点`）。

### 15.2 parallel worker（`codegen.c:15064-15345`）

- 每 worker 一个 `extc_arena`（`extc_par_open` 为每个 worker 建一个；`extc_par_region` 时来自 par region）。**region 模式下 worker 自己不销毁**（`codegen.c:15099` 注释），统一由 `par_close` 做（`15078-15089`）。
- worker 线程序言保存并把 `extc_rc_tlsDepth` 清 0（`codegen.c:15116-15117`），使 worker 入口帧成为 **owner**；每块调用后恢复（`15122`）。
- TLS owner 判据在触发点做运行期三分支（`codegen.c:4288-4298`）：`if (__extc_tlsOwner)` 触发 `else` 记 owe。

### 15.3 线程模型（准确表述）

| 有保护 | 无保护 |
|---|---|
| slab 登记表（自旋锁 `reclaim.c:412-425` + 按代发布 + **旧代永不释放**；读者无锁探测，`ownFind` miss 后读 `extc_rc_regSeq`，变了重试，32 次后加锁复查 545-561） | `region->bytes/allocBytes/live/thr/scale/marking/sweeping/bump/freelist/slabs/seedsDone/wbDebt` |
| `_Thread_local ownHot`（598-628）、`ownMap[4096]`（412-413，写侧锁下） | `slab->occ/mark/rsbit/hold/rscnt/tid/freelist/bump` |
| pin 表（进程级自旋锁 1649） | shade 缓存（`wb` 之外） |
| `extc_rc_wb`（原子 921/928-930）、`extc_rc_regSeq`（原子） | — |

**准确表述：每个 region 单线程独占时安全；共享 region（把同一 home arena 传给多个 worker）不安全，且运行时没有检测。**

已知（未验证的）隐患：

| 隐患 | 位置 | 说明 |
|---|---|---|
| region 表无锁 | `codegen.c:12738-12778`（`extc_rc_regKey/regVal/regN` 是普通 `static` 数组） | 两个 worker 同时首次分配（`regionOf` miss ⇒ `regionNew` + 写表）或同时帧结束（`regionDrop`）会数据竞争；当前并行运行时每个 worker 用自己的 arena/region，但首次 `regionNew` 仍写同一张全局表 ⇒ **理论上有竞争窗口**；已核实：`extc_par_open`（`codegen.c:15063-15072`）只 calloc 结构体 + arena 数组 + arena_init，**不预建 region**；region 由 worker 首次分配惰性 `regionOf` 建立（`12736-12757`：miss ⇒ `regionNew` + 登记；`15082`：worker 只设 `extc_tls_arena`，不预建 region），而注册表无锁 ⇒ 竞争窗口**真实存在** |
| noSweep/owe 跨线程写 | `reclaim.c:2507-2509/2438`（`noSweep` 仅此两处写）、`:3862`（owe；`3858` 只读） | 若线程 A 向线程 B 拥有的 region 写 `storeRefUnknown/storeStructCons`，会无锁写 `noSweep` |
| pin 表项 `rg` 缓存 | `reclaim.c:2436-2457` | 加锁维护；`rg` 指向的 region 可能已被别的线程 `regionFree`（`pinDropRegion` 在 `regionFree` 里调，加锁）⇒ 有处理 |

---

## §16 诊断接口、死代码、已实现但未接线

### 16.1 诊断（仅显式开启时存在）

| 开关 | 源码 | 输出 |
|---|---|---|
| `EXTC_RC_PAUSE=1` | `reclaim.c:3795-3906`（`pauseDump`）、3864-3883（`pauseArm`：getenv 3878、SLOWSTEP 阈值 3879、atexit 3881） | 打印 `[pause]/[step]/[work]/[wbudget]/[worst]/[ubwork]/[guard]` 停顿统计 |
| `EXTC_RC_SLOWSTEP=<us>` | `reclaim.c:1216/3879` 只设阈值，超阈值在 `3260/3581` 打印 `[slow]` | **不是人为加延迟**；是「慢步告警阈值」 |
| `EXTC_RC_STATS` | `reclaim.c:2319-2334`（`statsArm` 2287） | 退出时打印 `rs_inc/rs_dec/rs_live/rs_hwm/releases/release_slots` |
| `EXTC_RC_TIME`（`getenv != NULL` 即开，**设成 "0" 也开**） | `codegen.c:12714`；运行时 156-184 | TICK 分组件计时 + `[rt:counters]` 段 |

**"能全量回收"的接口存在但不是生产路径**：`extc_rc_collect`（`reclaim.c:3988-3997`，标记到底再 settle）、`extc_rc_sweep`（`3627-3631`，`sweepBegin+settle`）、`extc_rc_settle`（`3614-3625`）。**它们的调用者是查询/诊断函数，`codegen.c` 不发射它们**。

### 16.2 已实现、无发射器（死代码）

| 符号 | 源码 | 状态 |
|---|---|---|
| `extc_rc_collect` | 2766 | 无发射器调用 |
| `extc_rc_sweep` | 2625 | 无发射器调用 |
| `extc_rc_settle` | 2613 | 只被查询函数与 `extc_rc_sweep` 调 |
| `extc_rc_bump / bumpReset` | 1891 | `codegen.c` 无任何引用（bump 不置 `occ`，见 §18 增长条件 9） |
| 查询函数 `bytes/live/scale/sweeps/freedSlots/slabCount` | — | 无调用方 |
| `extc_rc_garbageHint` | — | **例外**：仅多 shard 模式下 `codegen.c:4242` 调用 |

产物级实证：`[rt:counters]` 只在 timeOn 时发射；`[rt:chain]` 因 `frameRoots` 恒传 0 而整段不发射；`[rt:types]/[rt:helpers]/[rt:tlsdepth]/[rt:release]/[rt:owe]` 五段与源码字符串表**字节完全相等**（§0 的复现命令，`tests/reclaim-shapes/cyclic_shape.extc` 产物 3417 行）。

---

## §17 默认参数总表（"默认路径"的完整定义）

### 17.1 环境变量（改变"发射什么"）

下表是**档 2（清扫档）**的默认值。档 1（事件档）由分析把 `rcNoCount` 覆盖成 0、`rcSweepFree` 覆盖成 1；档 0（无回收档）整块不发射（§1.5）。

| 开关 | 默认 | 读取位置 | 作用 |
|---|---|---|---|
| `EXTC_RECLAIM` | 开（未设=开） | `codegen.c:14096-14098` | =0 关闭整个回收：分配点改走 `extc_arena_alloc`（`4695-4697` 的 else 分支），不发射运行时文本。**不是纯 GC 税对照**（连分配器都换成纯 bump） |
| `EXTC_RC_NOCOUNT` | **1（清扫档）/ 0（事件档）** | `codegen.c:14105`（默认 =1）、`14126`（=0 覆盖）；自动升级判定 `15936-15944` | =1 ⇒ 计数层整段编译掉（清扫档）；=0 ⇒ 恢复 hold 计数 + 归零即收（事件档，或 `EXTC_RC_NOCOUNT=0` 对照档） |
| `EXTC_RC_SWEEPFREE` | 三态（未设由 `unitAllAcyclic` 决定） | `codegen.c:14150-14157（getenv 14142）`；`shouldSweep 3239-3249` | 开 = 整程序证明无环 ⇒ 不做 sweep |
| `EXTC_RC_LOCALSTORE` | 1 | `codegen.c:14113-14121（getenv 14108）` | =0 关闭 no-count 层的局部槽 write-only store（`extc_rc_storeRefLocal 3662-3665`） |
| `EXTC_RC_SHARDS` | 1，上限 64 | `codegen.c:14107-14112` | 分片数 N；floor 按 N 除（`4216-4239`），每次触发只收垃圾最多的 shard |
| `EXTC_RC_TIME` | 无（`getenv!=NULL` 即开） | `codegen.c:12714` | 计时段 + 计数器段 |
| `EXTC_RC_M_MAX` | 未设或 0 = 隐式墙 `3·live`（负数按 0） | 运行期 `reclaim.c:990-1012`（`mmaxArm`） | 内存墙（字节）：把触发线夹到 `M_max − max(64·L/b, R_max)`（取 b=256 ⇒ `live/4`；`R_max = region.maxReq` = 见过的最大的单次请求，v10 起；**v11 收窄**：`R_max` 只在显式 `M_max` 下参与预留，隐式墙仍是 `−live/4`，见 §18.5） |
| `EXTC_RC_SWEEP_PRESSURE_MUL` | 4（1..16，越界 `#error`） | `reclaim.c:1120` | 近墙（`bytes ≥ 7/8·M_max`）时每步预算乘该值：**乘预算、不缩触发间隔**。`markAdvance` 的 walk 批上限随之从 256 变 1024（`reclaim.c:1394`）、sweep 步长与配额同乘 |
| `EXTC_RC_STEP_CPU` | 未设 = 墙钟 | `reclaim.c:1174` | =1 时 `[histo]/[top]/[chan]/[worst]` 用线程 CPU 时钟计时（滤掉调度噪声） |
| `EXTC_TASK_ROOTS` | 开 | `codegen.c:618、14111-14118（getenv 14116）` | =0 恢复上一版产物（不发射 `extc_task_rootPush`）；本轮修 P0-6 |

### 17.2 编译期常量（全部 `#ifndef`，可 `-D` 覆盖）

| 常量 | 默认 | 行 | 影响 |
|---|---|---|---|
| `EXTC_RC_SLAB_BITS` | 19（512 KiB，v13） | 138 | slab 是内存归还的最小单位（**永不归还**）；同时是**对齐**单位（反查靠位与） |
| `EXTC_RC_FLOOR` | 1 MiB | 143 | 压力地板；分片时 ÷ N；也是 `roundOk/budgetOk` 的下限 |
| `EXTC_RC_SCALE_MAX` | 2^40 | 146 | 阈值上限 ⇒ 事实上等于"不再回收" |
| `EXTC_RC_SCALE_MUL` | 4 | 153 | 连续空扫一次乘 4 |
| `EXTC_RC_HOLD_SAT` | 65535 | 156 | 计数饱和值 |
| `EXTC_RC_SLAB_SIZE` | `1 << EXTC_RC_SLAB_BITS`（512 KiB） | 339 | slab 大小 = 对齐单位 |
| `EXTC_RC_SLAB_HDR` | `(sizeof(ExtcRcSlab)+63)&~63` | 340 | slab 头 64 对齐 |
| `EXTC_RC_SLAB_CHUNK_MIN` | `EXTC_RC_SLAB_SIZE`（512 KiB，v13：块整块发给槽） | 345 | 每次开块发出去的容量下限（= 整块） |
| `EXTC_RC_SLAB_SLOTS_MIN` | 1024 | 346 | 每 slab 最少槽数 |
| `EXTC_RC_REUSE_MAX_SLOT` | 0xFFFE | 349 | 复用链 next 编码上限（`tid` 里存 slot+1） |
| `EXTC_RC_RCP_SHIFT` | 42 | 371 | 倒数乘移位宽度（槽号 O(1)） |
| `EXTC_RC_CLS_BUCKETS` | 32 | 213 | 大小类哈希桶数 |
| `EXTC_RC_OWNMAP_BITS` | 12（4096 项） | 436 | owns 直接映射缓存 |
| `EXTC_RC_TTAB_LINEAR_MAX` | 8 | 727 | slab 内类型表线性扫上限 |
| `EXTC_RC_SWEEP_BUDGET` | 8 word | 1037 | 每次 `sweepStep` 的 word 数（512 槽） |
| `EXTC_RC_SEED_PUSHES` | 256 | 1135 | 每步种子 push 上限 |
| `EXTC_RC_SEED_WORDS` | 512 | 1138 | 每步种子 word 上限 |
| `EXTC_RC_MARK_EVERY` | 64 | 1141 | 分配点每 64 次分配推进标记 |
| `EXTC_RC_MARK_WINDOW` | 2 MiB | 1144 | 漂移窗口（默认参数下 ×8 不生效） |
| `EXTC_RC_MARK_STEP` | 8 | 1147 | `stepBudget` 基数 ∈ [8,72] |
| `EXTC_RC_MARK_BATCH_MAX` | 256 | 1150 | walk 批**基数**上限；压力档夹到 `256 × SWEEP_PRESSURE_MUL`（默认 4 ⇒ **1024**，`reclaim.c:1394`），`[work] walk_cap` 报运行期实际值 |
| `EXTC_RC_MARK_BATCH_POP` | 200 | 1153 | 批量 pop 上限 |
| `EXTC_RC_MARK_COLD_BYTES` | 16 MiB（R2） | `reclaim.c:1154`（v18） | 冷档的活集门槛：`r->live` 超过它，一次 pop 就不再是缓存命中（见 §19.6） |
| `EXTC_RC_MARK_COLD_BATCH` | **32**（v25；R2 时为 128，`#error` 约束 ≤ `MARK_BATCH_MAX`，`reclaim.c:1270-1272` v25） | `reclaim.c:1158`（v18） | 冷档的每步 pop 上限；只收紧、不放宽（`walk_max ≤ walk_cap` 仍成立） |
| `EXTC_RC_ASSIST_CAP` | 512 | 1361 | assist 单步上限 |
| `EXTC_RC_RELEASE_BUDGET` | 2048 步 | 1110 | `releaseOld` 两相共用的步数预算（无续扫时 ≈36 µs；续扫段按 `SCAN_CHUNK/8` 计费 ⇒ 最坏 0.1–0.2 ms） |
| `EXTC_RC_SWEEP_PRESSURE_MUL` | 4（1..16） | 1120 | 近墙时每步预算乘子（乘预算、不缩触发间隔）；`markAdvance` 的 walk 批上限 256→**1024**，`[work] walk_cap` 随之为 1024 |
| `EXTC_RC_CASCADE_DEBUG` | 0 | 1987 | **编译期宏**（`-D`）：计数档级联释放的统计与不变式检查 |
| `EXTC_RC_WORK_BUDGET` | 0（关） | 1374 | >0 时三份预算合并为**一个总数**（停顿的形式化上界开关） |
| `EXTC_RC_ALLOC_INLINE` | 1 | 1419 | alloc 强制内联到每个分配点 |
| `EXTC_RC_FOREIGN_GUARD` | 0 | 1718 | 跨区旧值运行期守卫（仅调试） |
| `EXTC_RC_SHADE_GUARD` | 0 | 2164 | shade 缓存正确性守卫（仅调试） |
| `EXTC_RC_MARK_SHALLOW` | 64 | 2979 | 浅栈直通深度 |
| `EXTC_RC_SCAN_CHUNK` | **8192** word（本轮由 65536 下调） | 3016 | 大对象分段段长（64 KiB/段）；单步扫描成本的主项（§19.1、§19.2） |
| `EXTC_RC_QUOTA_SHIFT_MARK` | 3 | 1062 | mark 配额 = n >> 3（每 8 请求字节 1 个工作单位；P0-P） |
| `EXTC_RC_QUOTA_MAX_WORDS` | 8192 | 1065 | sweep 配额上限（合法 slab 至多 512 words ⇒ 实际不生效，只防异常 nslots） |
| `EXTC_RC_SWEEP_FINISH_MAX` | **64 word** | 1069 | 分配路径的 sweep **收尾信用**上限：一次分配最多资助清算游标块 64 word（1024 个 16 B 槽）。不夹这一下，512 KiB 块 / 16 B 槽 = 512 word ⇒ 一步可释放 32 766 个槽（cross 形状实测单步 194 µs） |
| `EXTC_RC_QUOTA_MAX_MARK` | 1<<20 | 1075 | mark 配额上限（reclaim.c:1543 才真正钳位）；调小换停顿、但 pd_bigobj 峰值会破 G18 |
| `EXTC_RC_QUOTA_DEADLINE_US` | 800 | 1089 | 配额循环的物理时间封顶（0=关）；判定点在 reclaim.c:1554-1564 |
| `EXTC_RC_LAZY_FREE` | **0（默认关闭）** | 1095 | P0-B 惰性复用链重建（sweep 只清 occ、重建推迟到复用）；实测收益不可复现且 Ir 更差 ⇒ **只作旋钮** |
| `EXTC_RC_ELIDE_VERIFY` | 0 | 1100 | **编译期宏**（`-D`）：slab 级省略命中前重扫 occ/mark/rsbit 并校验 `nLive`，不一致则拒绝重置（`extc_rc_elideMismatch`） |
| `EXTC_RC_FREE_DEFER` | 1 | 815 | 延迟释放总开关：1 = region 丢弃只挂 closing queue（O(1)），逐 slab 的撤销/摘登记/入 free queue 在有预算的排空里做；0 = 同步逐 slab 释放（对照/调试） |
| `EXTC_RC_CQ_MAX_REGIONS` | 32 | reclaim.c:829 | closing queue 的 region 积压上限；超过时 `regionFree` 就地做**一次**有界排空（8 slab / 100 µs），积压与 RSS 因此有上界 |
| `EXTC_RC_FREE_BUDGET_SLABS` | 8 | reclaim.c:817 | 队列排空每趟最多处理的 slab 数 |
| `EXTC_RC_FREE_DEADLINE_US` | 100 | reclaim.c:820 | 队列排空每趟的时间上限（与 slab 数取先到者） |
| `EXTC_RC_FREE_QUEUE_BYTES` | 8 MiB | reclaim.c:823 | 队列字节上限；超过则改用硬截止排空 |
| `EXTC_RC_FREE_HARD_DEADLINE_US` | 1000 | reclaim.c:826 | 强制排空的时间上限（即 G3b 的 1 ms 判据） |
| `EXTC_RC_REG_MAX` | 512 | `codegen.c:12738` | region 表容量；满则新 region **不登记 ⇒ 永不 regionFree** |
| `EXTC_ARENA_SPARE_MAX` | 1 MiB | `codegen.c:12418` | arena release 时保留的 spare block 上限 |

`EXTC_RC_PAUSE` / `EXTC_RC_SLOWSTEP` / `EXTC_RC_STATS` 见 §16.1。

### 17.3 产物内观测宏（不是开关，不能 `-D` 覆盖）

| 宏 | 取值 | 行 | 说明 |
|---|---|---|---|
| `EXTC_RC_TIER` | 1 / 2 / 3（0 档不发射该行） | `codegen.c:12663` | 产物头里的档位标签：1 = 事件档（计数开、清扫关）、2 = 清扫档（计数关、清扫开，默认）、3 = 计数+清扫档（`EXTC_RC_NOCOUNT=0` 或 `EXTC_RC_SWEEPFREE=0` 强制）；只**描述**产物档位、不控制任何行为；无 `getenv`、无 `#ifndef` ⇒ `-D` 覆盖无效 |

---

---


## §18 内存上界：逐基质公式、实测、以及"名义上界"与真实漏洞

### 18.1 逐基质上界（可证）

| 基质 | 上界 | 依据 | 实测 |
|---|---|---|---|
| 帧 arena | `M_arena(f)` ≤ Σ（该函数每个块单轮内累计分配字节 + 8 字节对齐/块头）+ 至多 1 个 spare（≤ `EXTC_ARENA_SPARE_MAX` = 1 MiB） | 每次进入/离开块各 `release` 一次（§4），帧尾 `arena_destroy` | 不逃逸 2,000,000 轮：峰值 RSS 1.5 MB 恒定 |
| rc region | `M_rc(t)` = Σ`_{r 存活}` `S_r(t)`；`S_r(t)` ≤ 该 region **历史同时驻留 slab 字节和**（slab 永不还 OS，除非 `regionFree`） | §5、§9；`reclaim.c:2769-2862` | pb ≈ 20 B/节点、≈1.3 GB/s、100M 节点 1953.7 MB；pg2 丢弃 ≈120 MB 活集后 RSS 不降；pd 200×8 MiB 145.7 MB |
| place 载荷区 | 该 place 内累计分配字节 + 块余量（chunk 从 4 KiB 逐块翻倍到 256 KiB 后保持；余量 < 累计字节 + 256 KiB） | §13 | 未单独标定 |
| coroutine/parallel arena | 每任务 = 任务内分配峰值（worker 结束 destroy） | §15 | 未单独标定 |
| plate | 用户 view 长度（`mmap` 保留，不搬家） | §14 | 未单独标定 |

**最重要的一句**：只有作用域基质有"常量形态"的上界（由程序在该作用域内的分配量决定）；rc region 的上界是**逃逸活集 + 垃圾缓冲**的函数，不是常量。

### 18.2 触发线推出的稳态垃圾上界

触发判据（`src/back/reclaim.c:3751-3759`，`extc_rc_shouldSweep`）：`shouldSweep = bytes > thr && roundOk`；`thr = max(2·live, floor) · scale`（`:3643-3701` 的 `extc_rc_thrUpd`，夹取在 `:3660-3686`）；`roundOk = allocBytes - roundVisitedBytes >= max(live, floor)`（信用制，`:3705-3711` 的 `extc_rc_roundOk`，近墙时 `need /= SWEEP_PRESSURE_MUL`）。

由此（`scale = 1` 时）：

- 触发前一刻 `M = L + G ≲ max(2L, floor)`；
- 一轮清扫若能收干净，`G ≲ max(L, floor)`；
- 再加上 slab 粒度取整（每 region 至少一个 slab，v13 起 512 KiB）与大对象 payload（huge block 不在 slab 内），得到

本轮新增**内存墙**（P0-4）：触发线被夹到墙下，`thr = min( max(2·live, floor)·scale , M_max − res )`（`res` 定义见 §18.5），`M_max` 未设时隐式 `= 3·live`。⇒ 垃圾缓冲不再由自由参数决定，而由契约决定，见 §18.5。**v10 起**墙帽改成 `M_max − max(64·L/b, R_max)`（`R_max = maxReq`）⇒ 显式墙下触发线之上一律放得下一笔最大请求。**v11 修正**：隐式墙下无条件预留会让 `pd_pointer_dense` 峰值从 112,092 涨到 142,620 kB（+27.3%，自学习倍数养到 64），故预留改为**只绑显式墙**（`P1-ADMISSION.md` §3）；隐式墙回到 v9 公式，dense 逐 kB 相同。

校准形式（本轮实测，替代下面这个「粗暴界」作为对外承诺）：`M_peak = L·(1 + κ_M·r')`，`κ_M∈[0.772,1.010]`，保守取 1 即 `3L`（`CALIBRATION.md` §4；digraph 1M 实测 6.44 MB vs 上界 7.00 MB）。粗暴界如下，仅用于解释 floor 主导的小活集形状：

```text
M ≲ thr + max(L, floor) + slabPadding(512 KiB 粒度) + hugePayload    (thr = max(2L, floor)·scale)
// roundOk 的信用允许 bytes 超过 thr 最多 max(live, floor) 字节后才触发 ⇒
// 上界用 thr + max(L, floor)，而不是 thr；这解释了 1 MiB 栅格时代 pe 的 3.0 MB ≈ 1 MiB thr + 1 MiB 信用 + 取整
```

实测核验（1 MiB 栅格口径）：pe（活集 = 1 个对象，floor = 1 MiB）峰值 3.0 MB ≈ 3 个 1 MiB slab —— 与"`2L + floor` 再按 1 MiB 取整"同量级（差的是触发滞后与 slab 粒度）；v12 栅格 128 KiB 时取整项 ≤128 KiB/region、v13 512 KiB 时 ≤512 KiB/region（仍远小于 floor 主导项）；pc（环垃圾持续产生）恒 2.9 MB；pd 峰值 145.7 MB 来自 200 次 8 MiB 分配先后发生、同时驻留 ≈18 个 payload（200×8 MiB = 1600 MiB 是累计量，不是峰值）+ 元数据。

### 18.3 真正的无界条件（一条都不能漏）

1. **垃圾产生速率 > 清扫带宽**：sweep 活跃时每个分配点调用 `sweepStep(r, 8×pm + min(ceil(nslots/64), EXTC_RC_SWEEP_FINISH_MAX=64))` = 默认至多 72 word（pressure 档 96）= 4 608 个 16 B 槽。只要单次分配不产生超过这个数的待清槽，就收得住（实测 pc 2.9 MB 恒定）；关掉回收（`EXTC_RECLAIM=0`）则 pc 线性涨到 4615 MB（+1.9 GB/s）。
2. **`noSweep` / `owe` 永久置位**：`noSweep` 只在 `reclaim.c:2507-2509`（pin 表满）与 `:2443`（宿主未知的保守 store）置位，**没有任何清零路径**（§11.3）；`owe` 属主不结算则长期保留。档 2 下该 region 永不回收。
3. **region 表容量**：`EXTC_RC_REG_MAX = 512`（`codegen.c:12738`），表满后新 region **不登记** ⇒ 永不 `regionFree` ⇒ 该 region 的全部 slab 永不归还 OS（"宁漏不早放"）。
4. **阈值上限**：`scale` 可乘到 `EXTC_RC_SCALE_MAX = 2^40`（`reclaim.c:146`）⇒ 连续空扫后阈值事实上无穷 ⇒ 等价于停止回收。**例外**：`thr` 乘法溢出时被置 0（`reclaim.c:3657`）⇒ 反而每次分配都扫。
5. **活集本身线性增长**（pb）：这不是 GC 的错，内存上界就是活集；GC 只能保证"垃圾不额外膨胀"。
6. **空扫把上界整体乘 scale**：`scale`（reclaim.c:3613-3618；空扫 ×4、上限 2^40）不只决定"多久扫一次"，它把 thr 直接放大 ⇒ §18.2 的上界必须写成 `scale·thr`。实测 bf 形状（32 KB 槽、活集 32 MB）：T2 峰值 226 MB = 活集 × 7.1，其中 thr 学到 4 倍（4×2L 已大于活集），代价是 78% 墙钟落在缺页（MEASUREMENTS-TIER1.md §3 B3）。这是当前**大槽形状**的真实主导项，也是 §18.4 所说"RSS=历史峰值"的放大器。

### 18.4 结论

- 作用域基质（arena / coro / parallel / plate / place 载荷区）：**有界**，上界由作用域内分配量给出，代价 ≈ 0（实测 pa 0 记号、1.5 MB 平坦）。
- rc region：**有上界但非常量**。本轮校准（`docs/analysis/CALIBRATION.md` §4）把 §18.2 的粗糙界收紧为一条可外推的定律：
  `M_peak = L + κ_M·H = L·(1+κ_M·r')`，实测 `κ_M = 0.772 / 0.825 / 1.010`（1M / 250k / p4）；保守取 `κ_M = 1` ⇒ **`M ≤ (1+r')·L`**，默认 `r' = max(2L,floor)/L ≈ 2` 时即 **3L**（digraph 1M：上界 7.00 MB，实测 6.44 MB，−8%）。再叠加 §18.2 的 `max(L, floor)` 信用、slab 取整与 huge payload 尾项。
- **本轮把这条界做成可配置的经验上界（不是可强制的硬契约）**：运行时只把它用作触发线夹取与加压清扫（分配路径不拒绝分配，超墙唯一动作是扫得更凶），因此承诺写成「`M_peak ≤ M_max + 缓冲`（经验上界，§18.5 的口径注）」；默认 `M_max = 3·live`，可用 `EXTC_RC_M_MAX` 显式指定；代价是 sweep 次数 O(log N)（§18.5）。
- 且 **RSS = 历史峰值**（region 存活期不还 OS，`reclaim.c:2769` 只在 drop 时 free）。
- 需要在优化方案里处理的三件事：把 RSS 还回去（§18.3-2/3 的泄漏）、把垃圾缓冲系数压下来（触发策略）、把 `noSweep` 的出路补上。

---

### 18.5 内存墙契约（本轮引入：P0-4 + P1-1 + P1-2）

§18.2 只说明垃圾缓冲由触发线决定，而触发线本身是自由参数 ⇒ 峰值没有对外可承诺的界。本轮把它改成契约：

```text
M_max    = EXTC_RC_M_MAX   (未设/0 ⇒ 隐式 3·live；负数按 0)        // reclaim.c:990-1012 的 mmaxArm
thr      = min( max(2·live, floor)·scale , M_max − res )             // thrUpd 内夹取（reclaim.c:3681-3739；夹取 :3660-3686）
res      = 显式墙 ? min( max(live/4, maxReq) , live ) : live/4       // v10 引入、v11 收窄：源码写法 64·L/b，取 b=256 ⇒ live/4；maxReq = 见过的最大的单次请求；隐式墙下不预留（否则 dense +27.3%）
pressure = (bytes ≥ 7/8·M_max) || (thr>0 && bytes > 4·thr)          // 近墙判据（两条，reclaim.c:1016-1028）
近墙时每步预算 × EXTC_RC_SWEEP_PRESSURE_MUL                          // 默认 4（reclaim.c:1110）
```

- **承诺（经验上界，不是可强制的硬契约）**：峰值驻留实测落在 `M_max + 缓冲` 之内；缓冲 = `max(L, floor)` 的 roundOk 信用 + slab 取整（≤ 块大小：v13 起 512 KiB/slab）+ huge payload 尾项。默认 `M_max = 3·live` ⇒ 与 §18.4 的 `(1+r')·L` 同量级。**注意：分配路径并不拒绝分配**（`M_max` 只用于 arm / 有效墙 `eff` / `thrUpd` 夹取 / pressure 判据 / 诊断），超墙时唯一的动作是加压清扫 ⇒ 该界是经验上界，越界意味着清扫跟不上分配。
- **代价（必须一并承诺）**：夹紧 thr 使 sweep 次数从小常数变成 **O(log N)** —— 一轮最多退休一个活集，活集要涨 ~2.75× 才再触发（`reclaim.c:952-972` 上方注释逐字：an unbounded trigger threshold keeps the sweep count a small constant ... pays with unbounded resident memory）。`tests/reclaim/driver.c` 的两条旧断言因此改写成本节的契约；`EXTC_RC_M_MAX=2^40` 等价于关闭夹紧（run.sh 128/0 vs 默认 126/2）。
- **实测（本轮 after，默认档，`build-rf/extc` = `9fd0dcd8…`）**：10×400k 环形状（`tests/gc-p0/src/p03_ring_release.extc`，rounds=10 × nodes=400000）**VmHWM median 18,164 kB（17.7 MiB）**（3 reps；`/usr/bin/time` 单 rep 复测 18,008 kB），HEAD 同形状 41,616 kB；对照：同形状 `EXTC_RC_M_MAX=32MiB` 42,392 kB、`=64MiB` 47,768 kB、`=1TiB`（关墙）47,768 kB ⇒ **墙确实在约束峰值，且 after 比 HEAD 更低**。该形状 `[work]`：`M_max=4101120`（隐式 3·live）、`thr=2715120`、`pressure_steps=8787`、`scale_max=1`。⚠️ 早前手记的 **93,080 KB（90.9 MiB）/ `pressure_steps=1209` / `collects=13` / `[histo]` n=21833** 来自 interim 构建的单次观测、日志未归档，**v4 复测不复现**（同形状关墙也只到 47.8 MB），已作废，不要再引用。
- **墙不可达警告**：若 `M_max` 小到连 floor 都放不下（`max(2L,floor)` 压不到墙下），运行期打一次 `[mmax] memory wall unreachable: M_max=… floor=…; using floor`（`reclaim.c:3714-3718`）并退回 floor ⇒ 此时契约退化为 floor 主导，必须让用户看见，不能静默。

### 18.6 前提的修正：回收配额按分配字节计（P0-P，已落地）

§18.2–§18.5 的界都隐含一个前提：**回收工作量跟得上分配字节速率**。`tests/gc-p0/src/pd_bigobj.extc` 是它的直接反例——每轮 `new big`（100 万个可空指针字段 = 8 MiB）后立刻丢弃，活集恒为 1 个对象（8 MiB）。改造前默认档实测：

| 轮数 | 峰值 `ru_maxrss` | 墙钟 | `[pause] collects` | `step_n` |
|---|---|---|---|---|
| 60 | 493,400 kB | — | 1 | 121 |
| 200 | 1,067,284 kB | 0.78 s | — | 428 |

内存按累计分配量线性增长（200 轮 = 60 轮的 **2.163×**）。根因不是清扫慢，而是**量纲不匹配**：标记/清扫的推进量由「分配事件数」给配额（每次分配 8 个 sweep word、256 pops 或一个 `SCAN_CHUNK` 扫描窗），而真实工作量由字节数决定（该形状每个对象 100 万个指针字段）⇒ 分配事件太少、一轮标记都跑不完、一个槽都没释放。

**已落地（P0-P；宏 `reclaim.c:1050-1080`、实现 `reclaim.c:1540-1573`）**：配额改为按分配字节给——

- mark：`Q_mark(n) = min(n >> 3, EXTC_RC_QUOTA_MAX_MARK = 1<<20)` 个 work unit（1 unit = 1 次 pop 或 1 个扫描字；语义是「每 8 请求字节允许 1 个回收工作单位」）；
- sweep：原预算（含 pressure 乘子）再加 `max(1, min(ceil(nslots_sw/64), EXTC_RC_QUOTA_MAX_WORDS = 8192))` words；
- 时间片兜底：`EXTC_RC_QUOTA_DEADLINE_US`（默认 **800**）——配额循环每次 advance 后读一次时钟，超时就 break 并计数（`[quota] hits=`）。语义分工：**配额给「按字节的回收信用」，deadline 给「单步物理上限」**。

改造后实测（`build-rf/extc 7019342b…`，见 `MEASUREMENTS-P0-IMPL.md` §3）：**200 轮 = 60 轮 = 26,264 kB（1.000×）**，上界 `3·L + 2·SLAB = 26,624 kB`，余量 360 kB；`deadline_hits` 0–1；大分配通道 p99.9 = 64 µs、over-1ms = 0。代价如实记：同形状 `step_n` 428→26,116（×61）、GC 步总时长 5,018→165,612 µs（×33）、`step_max` 66.0→185.3 µs——**P 是用停顿换内存**。

⇒ 修正后的结论：配额按字节计之后，`M ≤ M_max + 缓冲` **对分配字节速率成立**（不再要求「分配事件足够多、单对象不大」），代价是超墙时分配同步变慢。同批落地的惰性 freelist（P0-B）改的是清扫停顿而非内存界：GC 步总时长 promote −20.2%、sizeratio −56.4%、cycleratio −10.8%；代价是 sizeratio 墙钟 **+9.31%**（Ir +6.00%，其中 7.34 个百分点归因于 P 的每分配配额记账），已登记为 `GC-ROOFLINE.md` §8.2-L14（下一轮按形状条件启用）。

---
## §19 停顿上界：预算常量给出的界、实测、以及三个通道的现状（v8：1 闭合、1 有预算、1 结构性 0）

### 19.1 单位预算（结构性，全部是编译期常量）

| 通道 | 单步预算 | 源码 | 界的形式 |
|---|---|---|---|
| 标记推进（分配点触发） | 种子 ≤ `SEED_PUSHES 256` pushes / `SEED_WORDS 512` words；walk 批 ≤ `MARK_BATCH_MAX 256` pops（压力档 ≤ `256 × SWEEP_PRESSURE_MUL` = 1024，`reclaim.c:1384/1368`）；批量 pop ≤ `MARK_BATCH_POP 200`；assist ≤ `ASSIST_CAP 512`；基步 `MARK_STEP 8` | `reclaim.c:1125/1107/1119/1122/1326/1116` | **单位数有界，单位成本无界**：1 walk 单位 = 沿一个对象读 `nptr` 个指针，`nptr` 由类型描述符决定 |
| 大对象扫描 | 每段 `SCAN_CHUNK` **8192** words = 64 KiB，超出记 cursor 续扫 | `reclaim.c:3007-3008`、`extc_rc_markResume`（v9 起行号未重锚，按符号定位：`grep -n 'extc_rc_markResume' src/back/reclaim.c`）；v9 起每 512 词查一次 `CLOCK_MONOTONIC`、超 `EXTC_RC_SEG_DEADLINE_US`(40) 提前收工，返回值改为**实际走过词数**（`P1-SEG-TAIL.md` §1） | 单步上界由**段长**而非对象大小决定：`P_step ≤ C_scan(8192)×8192 + 固定步成本`（源码注释 reclaim.c:2990-2991 明确含 fixed step cost）。8192 words=64 KiB 段的 **max** 实测 **78–98 µs**（含冷页/抢占的 max 族；task-32 同段典型 4–16 µs、p50 4–8 µs、0.75–1.56 ns/word）（折合 ≈9.5–12 ns/word；旧的 3.4 ns/word 是 65536 段的实测折算（区间 2.5–4.1 ns/word），不可外推到 8192 段）；关分段构建（`-DEXTC_RC_SCAN_CHUNK=1048576`）1010–1044 µs/段 |
| 清扫 | 每次 `sweepStep` `SWEEP_BUDGET 8` words = 512 槽 | `reclaim.c:1034` | 有界（槽数） |
| 回收配额（P；分配点触发的第二通道） | `Q_mark(n)=min(n>>3, 1<<20)` unit、`Q_sweep = 原预算 + max(1, min(ceil(nslots/64), 8192))` words；**物理上限** `EXTC_RC_QUOTA_DEADLINE_US 800`（每次 advance 后检查） | `reclaim.c:1050-1080`、`:1480-1513`、deadline 判定 `:1485-1504` | `P_step ≤ D + 2 segments ≈ 0.96–1.0 ms`（D=800 µs，首 advance 前移、再加一段续扫）。**注意**：描述符字段扫描（一次 pop 可扫 `nptr ≤ 4096` 个字段）不进 work unit ⇒ 对它有界性靠 deadline，不靠 unit 计数 |
| 定向释放 `releaseOld` | **有预算**：`EXTC_RC_RELEASE_BUDGET 2048` 步，两相共用；phase1 预算耗尽 ⇒ 整次放弃（`thr=0`、`roundVisitedBytes=0`、清 region mark 位），phase2 中断 ⇒ 已释放保留 | `reclaim.c:1100`、函数体 `reclaim.c:3336-3407`（放弃语义在函数头注释） | 无续扫时 ≤2048 × τ_t ≈ **36 µs**；**续扫段只按 `SCAN_CHUNK/8 = 1024` 计费单位，而一段实测 78–98 µs ⇒ 最坏约 0.1–0.2 ms**（reclaim.c:3347-3360）。预算耗尽只少收不多收 |
| `regionFree` | **O(1) 同步段 + closing queue 排空**（v8）：同步段只做 `extc_rc_abort`、`pinDropRegion`、`r->dead=1`、挂本线程 closing queue；逐 slab 的 R3 守卫 / `unlinkChildren` / `ownDel` / 压 free queue 由 `extc_rc_cqDrainCore` 在预算内做（8 slab 或 100 µs / 趟；积压 > `EXTC_RC_CQ_MAX_REGIONS`(32) 时 `regionFree` 内做一次） | 实现 `reclaim.c:2769-2862`（drop）、`:1776-1819`（排空）、`extc_rc_owns` 的 `dead` 判定 `:698`；探针 `[regionfree]`（`sync_us`/`queue_us`）、`[freequeue]`、`[closing]` | `free` 次数上界仍在（≤ `10·#slab + #classes + 3`），但**同步段不再随 #slab 增长**；v8 实测 pd 0.4 µs、dense 1.6 µs、pb 100M 1.7 µs |
| 计数层 `inc/dec` | O(1) | §12.2 | 有界 |

### 19.2 实测（-O2，默认档 = 档 2，MEASUREMENTS §6）

| 观测量 | 实测 |
|---|---|
| `[pause]`（一次 `maybeCollect`→`extc_rc_step` 的整步墙钟；`collects` 是触发次数） | pc 4577 轮：max **16–68 µs**、均值 ≈1.0 µs、占墙钟 0.6%；pe max 29 µs；pd 5–6 µs；pb 4.0 µs |
| `[step]`（切片） | pc n=425,661：p99 ≈2 µs、>5 µs 仅 0.07%、tail **86–314 µs**；pb tail 118–179 µs；`over1ms=0` |
| 大对象（pd = 200×8 MiB，**P0 轮**强制 chunk 对照，产物集 `build-emp`） | `SCAN_CHUNK=8192`：`[step]` n=510、max **78–98 µs**、>64 µs 仅 1–2 段、total 4504–4508 µs、mean 8.8–8.9 µs；同构建对照 `SCAN_CHUNK=65536`：n=643、max 164–269 µs、>64 µs 346–352 段、total 28572–31453 µs、mean 44.4–48.9 µs ⇒ 尾巴由段长主导（max −59%~−64%、total −84%；mean 含全部步）。**v7 交付口径另记**：pd_bigobj 200 单 rep `[step]` n=26,116 / total 144,038.6 µs / max 113.4（§22-5） |
| 标记步 p99（本轮标定） | 3 rep 分别 **4.36 / 4.79 / 6.42 µs**（p50 2.42）；模型 `P(b)=b·τ_t+8·64·τ_s = 4.71 µs`（b=256）：对 p99 中位 4.79 µs 误差 **−2%**，对 4.36 高 8%、对 6.42 低 27%；b=224 ⇒ 4.15 µs | `CALIBRATION.md` §7 |
| 计时噪声地板（本机） | 纯空转循环 200k 次：max **98–171 µs**、>64 µs 有 2–13 次 ⇒ 本机 max 不可作门禁，必须用 p99 | 本轮 `tests/gc-p0/noise_floor.c` |
| 规模无关性 | 100M 次分配之后，`[pause]` 整步墙钟（16–68 µs）与 `[step]` 切片预算都不随堆增长 ⇒ **停顿由预算常量决定，不由堆大小决定**；注意 `extc_rc_collect` 无生产调用者（§19.3-3 结构性 0），16–68 µs 是 `[pause]` 口径、不是 collect 口径 |
| `releaseOld` 有界（本轮 after） | 2048 步预算生效：无续扫时单次 ≤ **36 µs**（τ_t=17.49 ns × 2048）、含大对象续扫时最坏 **0.1–0.2 ms**（每段计 1024 单位，reclaim.c:3347-3360）；代价是少收，残留垃圾交给 sweep，因此该形状的内存靠 §18.5 的 `M_max` 契约兜底 |
| `regionFree`（v8，closing queue） | pd_bigobj 200：`[chan]` **0.4 µs**（max 0.9；v7 493.2/711.4）；`pd_pointer_dense` **1.6 µs**（max 2.5；v7 1,365.5/1,370.7）；pb_escape_grow **100M 1.7 µs**（v7 13.2–14.8 ms）；cyc/pc/pd ≤1.7 µs ⇒ **G3b 三通道闭合**（判据 ≤1 ms、目标 ≤0.2 ms）。`[regionfree] max_frees=0 max_slabs=0`、`[closing] slabs_moved=0`：这些形状只在退出前 drop 一次，队列未排空就随进程结束（OS 回收）；有界排空归零由 driver `t_regionFreeDefer*` 与 `extc_rc_freeQueueDrainAll` 证明 | `P1-CLOSING-QUEUE.md`、`MEASUREMENTS-P1.md` §3（v7 旧数）、`P1-REGIONFREE-BATCH.md` |
| 续扫计量（v2 已修） | `extc_rc_markResume` 现在**返回走过的 word 数**（`reclaim.c:3100-3116`，return 在 `:2988`），`markStep` 在 `:3003` 记 `seg`，`[work] seg_max` / `[ubwork] … seg_max` 上报（`:3694` / `:3722`）⇒ 续扫工作量可见。但一步的**墙钟**仍是一整段的固定成本（pd 实测 78–98 µs；源码注释 `:2862-2863` 给出的模型是 `P_step ≤ C_scan·SCAN_CHUNK`，热 9 µs / 冷 ~28 µs 量级）⇒ 判尾巴用 `[histo]/[top]` |

### 19.3 三个通道的现状（关键，必须写进结论；v8 起 regionFree 已闭合）

1. **`regionFree`（帧销毁）——v8 已闭合**：同步段 = `abort` + `pinDropRegion` + 置 `dead` + 挂 closing queue，与 region 的 slab 数无关（O(1)）；逐 slab 的 `unlinkChildren`/`ownDel`/`free` 移到 closing queue 的**有预算排空**（8 slab / 100 µs，`reclaim.c:1811-1854`），积压上限 `EXTC_RC_CQ_MAX_REGIONS=32` 保证 drop 内也最多一次有界排空（`:2769-2770`）。语义靠 `dead` 保持：`owns()` 对 dead region 立刻答 0（`:698`）⇒ 不登记、不标记、不复活；内存在排空到它之前保持有效。代价是跨区出边撤销推迟 ⇒ 目标对象暂时被**保守保留**（安全方向）。实测 pd 493 µs→0.4 µs、dense 1,365 µs→1.6 µs、pb 13.2 ms→1.7 µs（§19.5）。**诚实限制**：驱动形状只在退出前 drop 一次，队列不被后续分配点排空（`slabs_moved=0`），长跑/反复 drop 形状未构造。
2. **`releaseOld`（定向释放）**：本轮已加步数预算 ⇒ 无续扫时单次 ≤2048 步 ≈ **36 µs**、含续扫最坏 **0.1–0.2 ms**（§19.1）。预算耗尽的语义是只少收（只清标记、不早放），残留由 sweep 兜底 ⇒ 它的内存靠 §18.5 的 `M_max` 契约，而不是靠无界地一次收完。
3. **`collect` / `settle` 通道**：生产路径无调用者——`extc_rc_collect` 在生成物里是 `__attribute__((unused))` 且无调用点（MEASUREMENTS §10 第 1 条），`PAUSE_DIST` 的 0 是**结构性 0**，不是"没有停顿"；只看 `max_pause_us` 的门禁在本 revision 上是盲的。

⇒ 现状的准确表述是：**"分配点推进通道的停顿有界"**，不是"停顿有界"。

### 19.4 形式化的停顿界

```text
P_max  ≤  c_m·b  +  c_s·s  +  c_c·w  +  c_f·f  +  c_r·n  +  ε
```

| 符号 | 含义 | 现状数值 | 依据 |
|---|---|---|---|
| `b` | 单步 walk 单位数 | 非压力步 ≤256 pops；压力步 ≤`256 × SWEEP_PRESSURE_MUL` = **1024** / 基步 8 | `MARK_BATCH_MAX` / `MARK_STEP` / `reclaim.c:1394` |
| `c_m` | walk 单位成本 | **17.49 ns/obj**（τ_t，本轮标定；旧值 14.4）。该项 `c_m·b` = **4.48 µs**（b=256）；压力档 b=1024 时 ≈**18.1 µs**（压力步只在近墙出现，且仍 < G1② 的 50 µs、G20 的 64 µs 门限）；基步 b=8 时 ≈0.14 µs | `CALIBRATION.md` §7 E5；250k 形状 16.10、p4 6.8 |
| `s` | 种子 word/push 数 | ≤512 word / ≤256 push | `SEED_WORDS` / `SEED_PUSHES` |
| `w` | 每段扫描 word 数 | **8192**（64 KiB，本轮 P0-1） | `SCAN_CHUNK` reclaim.c:3007 |
| `c_c` | 每 word 扫描成本 | **暖 0.97 / 1.60 ns/word**（pd / dense，p50 精确分位，v9）⇒ 单项 `c_c·w` = **7.9 / 13.1 µs**；**max 族** 9.5–12 ns/word（78–98 µs/段）只在冷页/抢占下出现 | v9 修正（`P1-SEG-TAIL.md` §2/§5；旧值是把 p99.9 尾巴当成本）；§19.2 的 8.8–8.9 µs 是全部 step 的均值 `total/n`，不是 `c_c` |
| `c_s` | 每 seed word 的成本（种子按候选指针逐 word 检查） | 未单独标定；与 `c_c` 同属一段扫描代码、量级相近 ⇒ 该项上界 ≈512×12 ns ≈ **6 µs**（实际种子常远少于 512 word） | `SEED_WORDS`=512；`SEED_PUSHES`=256；`reclaim.c` 种子/扫描同一循环 |
| `f` | 单次释放槽数 | 清扫 ≤512；`releaseOld` ≤2048 步（本轮 P0-3） | `SWEEP_BUDGET` / `EXTC_RC_RELEASE_BUDGET` reclaim.c:1100 |
| `n` | 一趟排空处理的 slab 数（region 规模不再进入单步停顿，v8） | ≤ `EXTC_RC_FREE_BUDGET_SLABS` = 8 | `reclaim.c:1815`（budget break） |
| `c_r` | 每 slab 的**区域级延迟释放**成本（`unlinkChildren` + `ownDel` + 压 queue） | ≈560 ns/slab（v7 同步段 `ownDel` 实测折算；8 slab ⇒ ≈4.5 µs） | `reclaim.c:1826-1838` |
| `c_f` | 每槽释放成本（清扫/定向释放的批量路径 `freeSlotWord` / `freeSlot`） | 未标定；参考量级 ≈33 Ir/次（≈10 ns/槽 @3 GHz） | `reclaim.c` 的 `freeSlot` 路径；TAXMAP.md:388 |


**配额通道（P0-P，§18.6）**：分配点触发的回收还有第二条通道——`Q_mark(n)=min(n>>3, 1<<20)` 个 work unit 与 sweep 配额（`按请求字节给`）。它的物理上限是 `EXTC_RC_QUOTA_DEADLINE_US`（默认 **800 µs**，每次 advance 后检查、超时 break）⇒ 单步上界 `P_step ≤ 800 µs + 2 segments ≈ 0.96–1.0 ms`（首 advance 前移、再加一段续扫）；`pops` 与描述符字段（一次 pop 可扫 `nptr ≤ 4096` 个字段）都不计入 unit ⇒ 对它们只能靠 deadline，不能靠 unit 计数。这是 §18.6 换内存的价钱：`pd_bigobj` after 实测单步 max 185.3 µs、`step_max` 中位 66→185 µs，`deadline_hits` 0–1。

**口径（必读）**：该式是**全通道保守和** ⇒ 只要本步含一次大对象扫描段，`P_max` ≥ `c_c·w` ≈ 78–98 µs（max 族；典型段 4–16 µs，≈0.1 ms 只是上界），与「5 µs 停顿」不相容。**5 µs 门禁只约束不含大对象续扫、且不含压力档的 mark/step 单步**（`CALIBRATION.md`:277-278/334：p99 中位 ≤5 µs、模型 4.71 µs @b=256）；压力档 b=1024 的步模型约 **18.1 µs**（`reclaim.c:1394`），只在近墙出现（实测 p5 `walk_max=1024`、`pressure` 步 n=1726，gate-work 判 `walk_max ≤ walk_cap` 且 `walk_gtcap=0`）；两者不可混用：一个是设计上界之和，一个是实测的常态单步。

**R2 修正（冷档，2026-11-13，见 §19.6）**：上式里的 `b` 不再是全局常量，而是 `b_eff`：暖档 `min(b, 1024)`、冷档（`r->live > EXTC_RC_MARK_COLD_BYTES = 16 MiB`）`min(b, 32)`（v25；R2 时为 128）。所以 `P_step ≤ b_eff·τ_t + 234.5 ns`：暖形状不变，巨活集形状的单步界被**收紧**（不是放宽）。

**杠杆顺序（按对 P 的贡献）**：大分配上的配额通道 `D + 2 segments`（≈0.96–1.0 ms 天花板，只有 `pd_bigobj`/dense 类形状会碰到；由 `EXTC_RC_QUOTA_DEADLINE_US` 直接决定）> `c_c·w`（SCAN_CHUNK：8192 段 78–98 µs/段；旧 65536 段 max 164–269 µs）> `c_r·b_cq`（closing queue 每趟 8 slab ≈4.5 µs）> `c_f·f`（releaseOld 预算）> `c_m·b`（walk，≈**4.5 µs** @b=256、压力档 b=1024 时 ≈**18.1 µs**；基步 8 时 ≈0.14 µs）。这就是优化方案里"先降 P"的次序依据。

---

### 19.5 v8 轮：`regionFree` 的 closing queue（G3b 闭合）

机制（`reclaim.c`，md5 `37c92f0b…`，4 128 行）：`extc_rc_regionFree`（`reclaim.c:2769-2862`）在 FREE_DEFER 档下变成 **O(1)**：`abort` → `pinDropRegion` → `r->dead = 1`（`:2762`）→ 尾插本线程 closing queue（`cqNext`，`:2763-2766`）→ 若 `cqN > EXTC_RC_CQ_MAX_REGIONS`（默认 32，`reclaim.c:829`）就地做**一次**有界排空（`:2769-2770`）。逐 slab 的工作全部移入 `extc_rc_cqDrainCore`（`reclaim.c:1811-1854`）：取 region 链头一块 slab → R3 守卫（`part==0` 时扫 occ，`:1787-1790`）→ `rsIncN≠0` 时逐占用槽 `unlinkChildren`（`:1791-1798`）→ `ownDel`（`:1799`）→ O(1) 压入 free queue（`extc_rc_fqPushOne`，`reclaim.c:1798-1809`）；region 链空时 free 类桶/`mstk`/`cstk`/region 本身（`:1804-1816`）。排空预算与 free queue 共用 **8 slab / 100 µs**，排在 `extc_rc_fqDrainAuto` 最前面（`reclaim.c:1465-1468`；调用点仍是 `slabNew` 开凿前与 `settle`），全量入口 `extc_rc_freeQueueDrainAll` 先清 closing queue（`reclaim.c:1479-1480`）。

语义不变的关键：`extc_rc_owns` 在 `reclaim.c:696` 判 `s->region->dead` ⇒ region 一经丢弃，其槽**立刻**不算活（不可登记、不可标记、不可计数），所以“drop 后旧对象不再活”“写旧槽不复活对象”在延迟释放下仍成立；slab 内存在排空到它之前保持有效，dead region 的 `mstk` 也在排空前仍分配着。代价是**保守保留**：跨区出边撤销被推迟 ⇒ 目标对象暂时不会因入边归零而回收（安全方向），最多到该 region 排空为止。量具：`[regionfree] sync_us= queue_us= max_frees= max_slabs=`、`[freequeue] …`、新增 `[closing] regions_pending= slabs_moved= cap_regions=`（`reclaim.c:3804-3806`，仅 `EXTC_RC_PAUSE=1`）。

判定（v8，cpu 口径，`EXTC_RC_PAUSE=1 EXTC_RC_STEP_CPU=1`）：`[chan] regionfree` **pd_bigobj 493.2 µs → 0.4 µs**（max 0.9）、**pd_pointer_dense 1,365.5–1,849 µs → 1.6 µs**（max 2.5）、**pb_escape_grow 100M 13.2–14.8 ms → 1.7 µs** ⇒ **G3b 三通道全部闭合**（判据 ≤1 ms、目标 ≤0.2 ms）。G18 不变：pd_bigobj 60 轮 = 200 轮 = **26,076 kB**（上界 26,624）、dense 112,092 kB 与 v7 逐 kB 相同 ⇒ closing queue 没有抬 RSS（积压上限 32 region，按分配点配速）。**诚实限制**：这些形状只在进程退出前 drop 一次 ⇒ 队列还没被后续开凿排空（`[closing] slabs_moved=0`、`[freequeue] left_slabs=0`，随进程结束由 OS 回收）；“有界排空归零”由 `tests/reclaim/driver.c` 的 `t_regionFreeDefer*`（最多 64 个新 region 的首分配点驱动，断言 `fqSlabs==0 && cqN==0`）与 `extc_rc_freeQueueDrainAll` 证明。长跑、反复 drop 且继续分配的形状仍未构造（region 寿命 = 帧 arena 寿命，普通程序 n=1）。单步 p99.9 的 64 µs 桶尾巴属 (b) 续扫通道（§19.5.3 归因），与本条无关。


#### 19.5.1 `G1②` 的量具改成三通道（否则判据不可审计）

v7 新增 `[segsteps] n= pops_tot= max_us= b1us…bge64ms=`（15 桶桶界与 `[histo]` 相同；段步 = `seg>0` 的分段续扫步）。于是判据拆成三通道：(a) **常规步**（`seg=0`）＝ `[histo]` 逐桶减去 `[segsteps]`（负值或 `n_seg>n_histo` ⇒ ERROR）；(b) **续扫段**（`seg>0`）＝ `[segsteps]`，判 `p99.9 ≤ min(1.5·c_seg(shape)·chunk + 1 µs, 64 µs)` 且 `over-1ms=0`；(c) `regionfree`/`release`/`collect`/`account` 取 `[chan]`。warm 形状 (a) 判 `>50 µs 占比 ≤1e-4` + `p99.9 ≤10 µs`；cold-dense（`pd_pointer_dense`）**不参与 5 µs 门禁**，只判 `τ_t=步时/pops ≤2 µs`、`p99.9 ≤1 ms`、`over-1ms=0`。`max` 一律只作 OBS（WSL2 时钟尖峰 + 58–871 µs 的段步尾巴机制未定位），`n<1e5` ⇒ N/A。

量具实现：声明 `reclaim.c:1168`、记录 `:3126-3127`、打印 `:3642-3644`；empirics 侧 `tests/gc-p0/g1c.py`（自检 S1–S22）+ `summarize.py` 四类判据段 + `run.sh g1c` 子命令；`c_seg` 用 `[segsteps]` p99.9 ÷ chunk 标定（**不用** `[top]` 的冷整段 278–424 µs）——**v9 更正：该式是把尾巴拟合尾巴（阈值 = 1.5×自身），不具判别力**，改造方案见 `P1-SEG-TAIL.md` §6，落 `tests/gc-p0/bseg-cal.tsv`（9 列、3 reps 中位、单独跑、src_md5 不一致必须重标）。
**`c_seg` 标定存疑（量具项）**：v7 标定得 pd_bigobj **7.087 ns/word**、dense **11.077 ns/word**，比 v6 参考值（4.16 / 6.885）高 **61–70%**；标定只有 3 reps 且与 after 计时同机（load≈1.4）。当前 `B_seg = 1.5·c_seg·chunk + 1 µs` 取的是**宽**的一侧，因此判成 RED 的方向保守（不会把超标判成通过）；下一轮把 c_seg 标定放到空闲机 + ≥9 reps。


#### 19.5.2 吞吐代价（相对 HEAD，v7 构建）

同轮交错 9 reps、pin 6、奇偶换序（`/tmp/v2/ab7.sh`，HEAD=`build-base/extc` `829e55a7…`、v7=`build-lead7/extc` `a7f2a873…`）：promote **+5.4%**、sizeratio **+5.0%**、chase +0.4%、cycleratio +0.7%；RSS 比 0.996/0.994/**1.035**/0.990；`EXTC_RECLAIM=0` 四臂 0.961–1.017 ⇒ 代价在回收路径，不在生成代码。callgrind（align 小 N）：promote 115,211,310 → **129,126,214（+12.1%）**、sizeratio 22,812,974 → 23,355,894（+2.4%）；promote 归因：v4 内存墙/压力 +8.2%、quota 路径 +2.5%、elide 卷计数器 +0.95%，`EXTC_RC_FREE_DEFER=0` 无差。`EXTC_RC_LAZY_FREE=1` 的 A/B 收益不可复现（promote 0.996、sizeratio 1.033）⇒ 默认关正确。


#### 19.5.3 超长步（>50 µs）的尾部归因（task-32，生成 C 上的 rdtsc 探针）

`P1-STEP-TAIL.md`（99 行）在不改 `src/` 的前提下，把 `markResume` 的 8192-word 段切成 16 个 512-word（4 KiB）子块分别计时，6 形状 × 3 reps。结论：

- segment 步 100% 的时间在 `resume`（逐字循环），且 **96–99% 集中在单个 4 KiB 子块**、邻块只有 0.1–0.7 µs ⇒ 一次连续停顿，而不是均匀的每字变慢。
- 三类机制：① **环境抢占**为主（`minflt=0`；无 GC 的纯遍历对照 `control.c`/`control2.c` 在同一核复现 90–512 µs 与 870–2345 µs，rate 2.0–2.3e-5/子块）；② `chase` 的**对象冷页缺页**（`minflt=16–49`、100–143 µs ≈ 16 页 × 4–9 µs）；③ `promote`/`pd_dense` 的 **256-pop 批真实工作**（0.6–1.9 µs/pop、60–230 µs）。
- **方法学结论（影响本文件所有 cpu 口径）**：本机 `CLOCK_THREAD_CPUTIME_ID` 会把被抢占时间计入 CPU——`control2` 里 stall 的 cpu 时间占 tick 的 **70–80%** ⇒ `cpu_us ≈ dur_us` **不能**作为「这是 GC 工作」的证据。因此 §19.5.1 的 `(a)(b)` 判据只用 p99.9 与 `over-1ms=0`，`max` 只作诊断。
- 对 chunk 的含义：这条尾部压不低 chunk（task-26 已从另一侧证明）；`chunk=8192` 的一段典型成本是 **4–16 µs**（实测每段 6.16 µs(pd)/12.76 µs(dense) = 0.75/1.56 ns per word，固定成本主导；p50 4–8 µs、p99.9 58.06 µs）⇒ 5 µs 预算 < 一段，只有「按字数预算在段内停下、下次续扫」才可达（E30 修订，见 MODEL-UPDATE-P1.md）。

---
### 19.6 v18 轮：标记步的冷档（R2；把巨活集形状的标记步压回契约）

**问题（R1 判据可判化后暴露的唯一真红）**：`pb_escape_grow`（活集长到 GB 级、标记栈几乎总是浅的逃逸增长形状）的**标记步** `>50 µs` 占比 2.56e-4（阈 1e-4）、p99.9 16–32 µs（阈 10 µs）。

**被否掉的做法**：给标记步加**时间**上限（`EXTC_RC_MARK_DEADLINE_US`）。`pb` 20M 上步数 19,531 → **725,207**、整机 **7.7×** 慢、`gate.sh` Ir 破线（89.2M > 88,029,115）。根因：`pb` 的标记栈几乎总是 `sp=1`，走的是 `EXTC_RC_MARK_SHALLOW=64` 的浅栈快路；armed 后禁掉快路 ⇒ 每 pop 从 ~2 ns 变 ~130 ns；且每批一次 `CLOCK_THREAD_CPUTIME_ID` syscall 本机 **122.7 ns**，与 128 pop 的工作同量级。⇒ 时间闸门不能进标记热路径。

**采纳的机制**：按活集分两档封顶 `markAdvance` 的 `b`。常量 `EXTC_RC_MARK_COLD_BYTES (16 << 20)`（v18 `reclaim.c:1154`）与 `EXTC_RC_MARK_COLD_BATCH 128`（`:1158`，另有 `#error` 约束它 ≤ `MARK_BATCH_MAX`，`:1161-1163`；**v25 起该常量收到 32**，见 §19.11）；clamp 在 `extc_rc_markAdvance` 的 window clamp（v18 `:1429`）**之后**（`:1430`）：`if (r->live > (int64_t)EXTC_RC_MARK_COLD_BYTES && b > 128) { b = 128; extc_rc_markColdN++; }`。计数在 `:1189`，`[work]` 行尾新增 `cold_steps=`（`:3927`，实参 `:3939`）。

**为什么门槛是 16 MiB（形状实测，不是按基准名开洞）**：冷档只在巨活集形状触发，其余 `cold_steps=0` —— promote `thr_max=6,068,128`、cycleratio `2,735,120`、chase `thr_max=63,438,892` 但 `r->live` 仍 <16 MiB、pb `thr_max=1,247,201,956`（`cold_steps=331,260`）。门槛是「活集字节数」这一个可解释的量：超过它，一次 pop 就是一次访存而不是缓存命中，`τ_t` 显著大于 17.49 ns/obj。

**判据方向**：冷档只让 `b` 更小 ⇒ `walk_max ≤ walk_cap` 仍逐字成立、`P_step` 上界收紧。`[work]` 的 `walk_max` 与 `walk_gtcap=0` 的既有断言不变（`tools/check_mark_batch.py` 新增 6 条静态断言 + 4 条红自测，`--selftest` 31 条全过）。

| 量（门禁口径 `EXTC_RC_PAUSE=1`，wall 时钟） | R1 | R2 | 判定 |
|---|---:|---:|---|
| pb 50M 常规步 p99.9 | RED | `gt10us=89 / n=343,296` = **2.6e-4**（判据 `gt10×1000 ≤ n`） | **PASS** |
| pb 50M `>50 µs` 占比 | 2.56e-4 | `gt50us=10` = **2.9e-5** | **PASS（≤1e-4）** |
| pb 50M 其它 | — | `max_us=85.8`、`over1ms=0`、`cold_steps=331,260`、wall 0.88 s（+2.3%）、RSS 911,964 kB 不变 | ① over-1ms=0 |
| pb 20M | — | `n=115,636`、`gt10=56`、`gt50=6`、wall 0.33 s | 同上 |
| 四形状 A/B（冷档开/关，5 轮交错 min ms） | 1.000 | promote 0.973 / sizeratio 0.981 / chase 1.007 / cycleratio 0.982；RSS 逐 kB 相同 | 无回归 |
| `gate.sh` Ir | 87,009,872 | **87,649,685 ≤ 88,029,115**（+0.74%） | PASS |
| G18（pd_bigobj 200 轮） | 26,072 kB | **26,072 kB ≤ 26,624** | PASS |

**为什么不全档降 `b`**：`b=128` ⇒ promote +4.1% / sizeratio +5.4%；`b=64` ⇒ +7.0% / +5.8%；`b=32` ⇒ +13.0% / +7.6%。这些形状根本不进冷档，全局降预算是用它们的墙钟买它们不需要的界。

**仍未闭合（显式移交下一轮，不算已绿）**：① cyc 的 `(a)` 仍 RED（`n=469,458`、`>50us=1.11e-04`、p99.9 RED；该形状 `cold_steps=0` ⇒ 与本轮无关，属噪声地板）；② pc 的 `(a)` 是 **N/A**（`n=41,184 < 1e5`），必须把 n 抬过 1e5 才能判；③ `tests/gc-p0/bseg-cal.tsv` 的 `c_seg` 标定仍是 v9 口径（门禁已告警必须重标），(b) 通道对 cyc/pb/pc/pd 记 N/A；④ `τ_t^cold` 未单独标定，模型仍用单一 17.49 ns/obj 给上界。
### 19.7 v21 轮（R3）：把停顿判据变成可判的（阈值不动，也不加时限）

R2 之后停顿契约在**形式**上是两项（§19.4 的 pop 预算与配额窗口），但门禁有五个用例因为某个通道 `n=0` 或标定陈旧而整体记 N/A。R3 的目标只有一个：让每条判据都能判，**阈值一个字不改**；能绿的绿、该红的红。

**(1) 量具：每步取 `min(线程 CPU, 墙钟)`。** 两个读数都是真值的上界（线程 CPU 时间不可能超过墙钟），各有一个方向的偏差（墙钟含宿主抢占、CPU 钟含 syscall 开销），取小者是最紧的可靠上界。发射为 `extc_rc_stepSpan(c0, t0)`（v21 `reclaim.c:1316-1320`），`markStep`/`sweepStep` 的步首尾各一处；`EXTC_RC_STEP_CPU` 没开时退回墙钟。这一步把 `pb` 20M 的 `max` 从 189.3 µs 压到 62.6 µs、`gt64` 从 4 到 0（`cpu > wall` 的记账工件）。

**(2) 量具：环境地板协议。** 宿主停顿按时间到达，落进哪一步就把哪一步推过 10/50 µs，于是被计成一条「GC 慢步」。对照实验（`tests/gc-p0/noise_floor.c` 的纯整数循环，无分配无 GC）：200M 迭代里 >6.5 µs 的停顿 740–958 次、单次最长约 186 µs ⇒ 地板客观存在。协议：每个 rep 在 `sample.py` 前后各跑 0.3 s 探针，取 `min(pre, post)` 速率 × 该 rep 墙钟 = 该 run 的停顿次数地板；只对 `>10 µs`/`>50 µs` 两条计数判据扣减，阈值不变、`over1ms` 不扣、缺探针不扣（退回原始口径）、扣减量在报告里显式打印。`pb`/`pc` 因此从 N/A/RED 变为 PASS；`cyc` 的 `>50 µs` 与本机地板同量级（原始 8.73e-05，扣 60 条），所以该通道在本机**可检出回归的幅度约 ≥2×**，已写进 GC-ROOFLINE §8 风险。

**(3) 判据：结构性 vacuous 必须有证据。** 三处 `n=0` 被查清是**必然为 0**（不是样本不足）：`(b)` 段通道只在 `words > EXTC_RC_SCAN_CHUNK` 时可达，所以只要 `[work] maxreq ≤ SCAN_CHUNK×8 = 65,536 B` 且 `n_seg=0` ⇒ vacuous PASS（`cyc 40 B`/`pb 16 B`/`pc 24 B`）；`(c)` release 通道要求生成物里存在 `extc_rc_storeRefOwnedRelease` 调用点，四个形状与 digraph 的生成 C 里都是 **0 处** ⇒ vacuous PASS，机制另由探针 `tests/gc-p0/src/rel_overwrite.extc` 证明（`release n=90`、max 7.5 µs）；`pd_bigobj` 的 `(a)` 因插桩不按 n 归因 ⇒ 记 `structural_na`，不进用例聚合但仍逐条打印。缺 `maxreq`、缺 `relsite` 证据时仍然 N/A —— 不许无证据变绿。

**(4) 契约：走步循环里不许读时钟。** R3 中途试过一个反面方案：给 mark 步加「时间闸门」（`EXTC_RC_MARK_DRAIN_DEADLINE_US=6`，每 32 个 pop 查一次，浅栈与批量两条路径都在循环体开头检查），它确实把 `cyc` 的 `gt10` 从 191 降到 132/136/128。但 `./check.sh nobench` 五个形状 FAIL：`tests/reclaim-perf/gate-work.py:174` 要求三次跑的 `[work]` 行逐字一致，而按时间切步的切点取决于调度（p5 三跑 `n=2684/2683/2687`、`walk_gt16=2658/2657/2660`）。既然墙钟/`[step]` 已被证明不可靠、工作量界是**唯一**确定的窗口判据（`gate-work.py:1-15`），就只能删掉这个机制，而不是放宽判据。所以步时上界今天只有一条：

```
P_step  ≤  b_eff · τ_t          b_eff = min(b, 1024)  … 活集 ≤ 16 MiB（暖档，§19.6）
                                b_eff = min(b, 128)   … 活集 > 16 MiB（冷档）
```

时钟只允许出现在步首尾与 `[slow]` 诊断里；这条规则由 `tools/check_mark_batch.py` 的两条结构断言（`batch walk reads no clock`、`shallow drain reads no clock`）加注入自测守着。代价是 `τ_pop` 必须自己有界，而扇出型形状今天没有（见 (5)）。

**(5) 门禁判决（v21，cpu 口径）**：`cyc`/`pb`/`pc`/`pd_bigobj`/`rel` PASS，`pd_bigobj60` 的 `(b)` N/A（`n_seg=7,488 < 1e5`），`pd_pointer_dense` 的 `(a)` **RED**。R1 登记的两条真红已经清掉：`pb` 的 `(a)` 落到 **5.63e-05**（`n=1,029,888`）、`pc` 的 `(a)` 从 N/A（`n=41,184`）变成可判且 **8.09e-06**（`n=123,588`）；`cyc` 的 `(a)` 净 `count(>10 µs)=252 ≤ 469,458`。新浮出的 RED 是 `pd_pointer_dense`：`tau_t=5.434 µs/pop`（`[top] cpu_us=695.6 pops=128`），而判据是 `≤2.0 µs`（`over1ms` 反而过了）。根因是该形状 8 MiB 里 1M 个非空指针 ⇒ 一次 pop push 上万个孩子、标记栈推到 `msp≈830k`（`mscap=1,048,576`），`realloc` 扩容 + 冷页缺页正好落在一步里 ⇒ **`b·τ_t` 在「每个 pop 的成本有界」时才有意义**，扇出型形状的一次 pop 成本正比于扇出。这是契约的真实漏洞（不是量具问题：on/off 两档构建都出现 271–361 µs 步）。R4 修法：把 push 计入步预算（`charge = pops + pushes` 或按 `SCAN_CHUNK` 迁入字数配额），标记栈改块链；`[work]` 里 `push_max=0` 就是「push 未计入」的直接证据。

**(6) 与模型的对应**：`M_max`/冷档/配额窗口的公式与常量一条没动（§19.4/§19.6 仍然成立），roofline 的 `λ(r)` 与 `M = L + κ_M H` 不需要重标。可标定量只有 `c_seg`：v21 重标为 **`pd_bigobj 3.841 ns/word`**、**`pd_pointer_dense 5.997 ns/word`** ⇒ `B_seg` 从 64.36/78.65 µs 降到 **48.20/74.69 µs**（`tests/gc-p0/bseg-cal.tsv` 绑定源码 md5，换 build 必须重标）。Ir 门禁回到 v18 量级（**87,625,199 ≤ 88,029,115**），因为时间闸门删掉后走步循环里不再读钟（`GC-ROOFLINE.md` §6.9.7）。

### 19.8 容量墙与 slab 归还（v22 新增）

§18 的内存定律把堆写成 M = L + κ_M·H，§19 的停顿界把一步写成 b·τ_t + c_fix。两条界都默认「容量（已映射的 slab 字节）跟着活集走」。在没有写屏障、也不搬移对象的前提下，这一点并不自动成立：

- 清扫的最小工作量是 Θ(容量)：每个占用槽每轮都要「清本轮 mark 位 + occ∧¬mark 判定」，而清位是 Θ(容量)。整块 slab 被跳过只在`s->nMark != 0`为假时成立（src/back/reclaim.c:3629），所以一块 512 KiB 的 slab 里只要留 1 个长命对象，它每轮都要逐词扫完。

- 容量是高水位：slab 只在 region 析构时整块归还。峰值过去以后容量不降，于是「每个长命对象最多钉住一块 slab」这一项不在 §18 的 κ_M 里，却是稀疏活集上最大的一项。

实测（48 B 槽，稀疏活集 = 每 MB 一个 root，连续三轮 collect）：16 / 64 / 256 MB 区在第 2、3 轮零可回收字节，仍花 5.7 / 45.8 / 239.8 µs 与 5.1 / 22.4 / 103.1 µs（≈0.7 ns/KB）。即工作量 ∝ 容量、与垃圾量无关。探针与机制锚点在 docs/analysis/T2-CAPACITY-AND-T1-WIDENING.md。

**机制（v24，冷路径，无写屏障）。** `extc_rc_slabShrink(r)`（src/back/reclaim.c:1898）在一轮已收尾、清扫游标已清空的时刻（`extc_rc_sweepStep` 收尾分支，3915；见 §19.10）沿 region 链访问至多 `EXTC_RC_SHRINK_VISIT_MAX=256` 块，把同时满足「`nLive==0`」「非 big」「非解析档 bump slab」「本区无跨区入边位（rsbit 全零或 `rsIncN==0`）」的 slab 从 region 链和 size-class 链上 O(1) 摘掉（新增 `prev` / `availPrev` 两个反向指针），入 free 队列，每次至多 `EXTC_RC_SHRINK_REL_MAX=16` 块；真正的 munmap 由既有 `extc_rc_fqDrainCore`（1501；budget `EXTC_RC_FREE_BUDGET_SLABS=8`、`EXTC_RC_FREE_QUEUE_BYTES=8 MiB` 硬闸）执行，所以不新增一条「按容量走」的停顿界。`EXTC_RC_SLAB_KEEP`（默认 0）可留少数空 slab 复用；累计归还块数记在 `extc_rc_slabRel`。

调用点必须在清扫游标清空之后：若在标记完成时就摘块，游标可能仍指向那块 slab，一旦放掉，游标就悬空（探针在 `extc_rc_sweepElide` 里段错误）。`extc_rc_sweepStep` 的收尾分支是这个前提唯一成立的地方，摘链固定在那里（3915）。

**实测（探针 /tmp/p4/probe4.c：64 MiB 峰值 → 留 16 个存活对象 → 每轮 1000 次短命分配）。** 基线 slabs 129 → 129、RSS 82.9 MB 恒定；v22 slabs 129 → 1（8 轮，16 块/轮）、RSS 82.9 → 16.8 MB（其中 11 MB 是探针自己的指针数组，运行期实际 ≈5 MB）。四形状交错 A/B（9 reps，min）：promote 38.86→39.23 ms（+1.0%）、sizeratio 15.40→14.32（−7.0%）、chase 119.22→117.08（−1.8%）、cycleratio 14.35→14.01（−2.4%）；RSS 逐形状不变（5,724 / 2,672 / 67,740 / 2,660 kB）。

**残留（未闭合）。** 稀疏模式不受影响：每块 slab 恰好 1 个长命对象时 slabs 仍是 33 / 129 / 513，第 2、3 轮仍 ≈0.7 ns/KB —— 归还的粒度是 slab，一个存活对象仍钉住 512 KiB。要把这一项降到 O(活集)、只有三条路：①细粒度归还（页级占用计数 + MADV_DONTNEED，常数 = 4 KiB / 槽宽）；②搬移式压实（κ→1，但停顿 ∝ 活集且需要转发指针）；③写屏障（用户已否决，m16773）。本文不宣称稀疏活集已界住。


### 19.9 页级归还：容量成本从块降到页（v23 新增）

§19.8 把「峰值之后 slab 永久钉在堆上」这一项消掉，粒度是 512 KiB；稀疏活集里每块留 1 个长命对象，容量项仍是 Θ(容量)。本轮把归还单位降到**页**：`MADV_DONTNEED` 丢物理页，不搬移、不加写屏障、全部在冷路径。

**布局前提（决定了能不能还页）。** 普通 slab 是 `aligned_alloc(EXTC_RC_SLAB_SIZE, EXTC_RC_SLAB_SIZE)`（默认 512 KiB、页对齐）；块头 `EXTC_RC_SLAB_HDR = (sizeof(ExtcRcSlab)+63)&~63` 只有几百字节、**不在页边界上**，所以页 0 含块头、永不归还。元数据（`occ`/`mark`/`rsbit`/`hold`/`tid`/`ttab`）全部在块外单独分配 ⇒ 还页不碰它们；freelist 的下一槽号存在 `s->tid[i]` 而不是载荷里 ⇒ 还页不破坏链表；分配路径每个槽都走 `extc_rc_zeroSmall` ⇒ 还过的页重新分配时语义安全（语言承诺「新分配读作 0」）。

**机制。** `extc_rc_pageTrimStep`（src/back/reclaim.c:2037）由 `extc_rc_sweepStep` 收尾调用（3917），每次至多访问 16 块、还 256 KiB。三层闸门：①**slack**（区域级）`capBytes − bytes < 8 MiB` 不还 —— 差得少时丢页/缺页抖动赢，去掉它 sizeratio +56%、cycleratio +19%；②**quiet**（块级）自上次 pass 起分配数 ≤64 才还，busy 块跳过并把时钟重置为当前 `allocN`；③**稠密** `nLive × 8 > nslots` 跳过。块内按 occ 位图标活页（槽可跨页 ⇒ 同时标首页与尾页），其余连续空页**合并成一个 run** 再 `madvise`。`pgNext` 续扫被预算截断的 pass、`pgLive` 记录上次 pass 的 `nLive` ⇒ 幂等。计数器 `extc_rc_pageRel`（字节）/`extc_rc_pageTrimN`（块 pass 次数），`[pages]` 行打在 `[work]` 之前。

**触发路径与收敛速率。** 调用点是 `extc_rc_sweepStep` 的收尾分支（3917），`sweepStep` 同时在 mutator 自动路径上（4239 / 4301）⇒ 不调 `extc_rc_collect` 的程序也会还页；块级归还与页归还同处这一个收尾分支（3915 / 3917），两者都在自动路径上（§19.10）。速率有界：每次收尾 ≤256 KiB ⇒ 从峰值收敛到界需要 ⌈(C − live)/256 KiB⌉ 轮。探针 `/tmp/pg/pages5.c`（64 MB 峰值、每 MB 1 root、140 万次分配、只调 `extc_rc_maybeCollect`）：RSS 43,636 → 36,404 kB、还页 7.25 MB、29 次块 pass、slabs 64 不变。
**容量界（本轮口径）。**

    C ≤ live + 4 KiB × #page(含活对象) + M_slab × #slab(含至少一个活对象) + 8 MiB
    M_slab ≈ 69 kB/块    （实测，计数关：tid 2 B/槽 + occ/mark/rsbit 三张位图 + 分配器取整与固定页）

`M_slab` 是页归还**消不掉**的一项：它按「含活对象的块数」计，所以 1 个长命对象仍钉住 ≈69 kB 元数据（探针 `/tmp/pg/pages4.c` 实测：32 / 64 / 128 块、每块 1 个长命对象 ⇒ 每块残差 69.8 / 69.1 / 68.7 kB，还页 15.62 / 31.25 / 62.50 MB；计数档再加 `hold` 2 B/槽 ≈ 21.8 kB）。8 MiB 是 slack 闸门的常数。两项都必须显式写在界里，不能假装它们是 0。

**实测。** 探针 `/tmp/pg/pages1.c`（48 B 槽、每 MB 一个 root、64 MB 区、400 轮 collect）：slabs 129 → 64、RSS 52,304 → **20,280 kB**、还页 31.27 MB（188 次块 pass）、收敛后单步 16.2–16.5 µs 与关掉机制相同、400 轮 collect 总 11.8 ms。16 MB 区（容量恰好 8 MiB）被 slack 闸门挡下（RSS 14,400 kB、还 0.25 MB），这是设计的取舍不是缺陷。四形状交错 A/B（9 reps）：promote 1.006 / sizeratio 1.007 / chase 0.994 / cycleratio 1.021，RSS 逐形状不变，`[pages] blocks=0`。

**两条踩出来的硬约束（都在源码注释里）。** ①**热路径只能加一个 `s->allocN++`**：先做的「每分配清已还页位图」让四形状慢 4.4%–5.5%，删掉后靠 `pgLive`+quiet 保证幂等，收敛结果逐 kB 相同。②**不能 include `<sys/mman.h>`**：它声明 `mmap(void*)`，与本语言给 plate 的 `uint8_t *mmap(...)` 冲突，README 的 par 示例编不过；运行期自带 `extern int madvise(void *, size_t, int)` 与 `#ifndef MADV_DONTNEED / #define MADV_DONTNEED 4`。

**仍未闭合**：搬移式压实（停顿 ∝ 活集 + 需转发指针）才能把 `M_slab` 与 slack 两项一起消掉；写屏障路线按用户决定不做。
### 19.10 自动路径上的块级归还（v24 新增）

§19.8 的块级归还有一条限制写在机制段里：它挂在 `extc_rc_settle` 上 ⇒ 只属显式路径。于是「从不调 `extc_rc_collect`」的程序，即使死块很多，容量也只能靠 §19.9 的页归还吃掉块内空页，块本身（512 KiB 映射 + ≈69 kB 元数据）一直钉着。本轮把块级归还移到**轮末**：与页归还同一个收尾分支，因此同一条自动路径（`extc_rc_maybeCollect` → `extc_rc_sweepStep`）上就会发生。

**机制（v24 口径）。** `extc_rc_sweepStep` 的收尾分支（src/back/reclaim.c:3914–3918）依次：`extc_rc_slabShrink(r)`（3915）→ `extc_rc_fqDrainAuto()`（3916）→ `extc_rc_pageTrimStep(...)`（3917），三件事都在 pause 计时块之内 ⇒ 成本记进 `[step]`/`[histo]` 而不是隐身。`extc_rc_slabShrink`（1898）新增 slack 闸门：

    if (r->capBytes - (int64_t)r->bytes < (int64_t)EXTC_RC_SHRINK_MIN_SLACK) return 0;   /* 1900，skip:shrink-slack */

`EXTC_RC_SHRINK_MIN_SLACK = 8 MiB`（1894–1895）；理由同页归还的 slack（§19.9）：无闸门时丢块/再 carve 的抖动赢（页级实测 sizeratio +56%、cycleratio +19%），代价是容量界抬一个常数。配套：`capBytes` 的累加从 `#if EXTC_RC_PAGE_TRIM` 里移出、改成无条件（885），否则 `-DEXTC_RC_PAGE_TRIM=0` 时 `capBytes` 恒 0、新闸门永远成立，块级归还被静默关掉。

**为什么摘块必须发生在轮末（结构性约束，不是时序偏好）。** 清扫游标可能停在链上任意一块；若在「标记完成」就摘块，游标会指向已 munmap 的块（探针在 `extc_rc_sweepElide` 里段错误）。轮末的前提是游标已清空，因此摘链只发生在一个安全的时刻：`extc_rc_slabShrink` 的调用点固定在 `extc_rc_sweepStep` 的收尾分支。

**容量界（v24 口径）。**

    C ≤ live + 4 KiB × #page(含活对象) + M_slab × #slab(含至少一个活对象) + 8 MiB

与 §19.9 的唯一区别是 **8 MiB 现在是两道闸门共用的常数**（页级 `EXTC_RC_PAGE_TRIM_MIN_SLACK`、块级 `EXTC_RC_SHRINK_MIN_SLACK`），并且两个常数项都不依赖历史峰值。收敛速率：块级 ⌈n/16⌉ 轮、页级 ⌈(C − live)/256 KiB⌉ 轮；单轮代价封顶在 `EXTC_RC_SHRINK_VISIT_MAX=256` 次访问、`EXTC_RC_SHRINK_REL_MAX=16` 块入队、free 队列的 `EXTC_RC_FREE_BUDGET_SLABS` / `EXTC_RC_FREE_QUEUE_BYTES`（>8 MiB 时走 `EXTC_RC_FREE_HARD_DEADLINE_US=1000` 硬截止档，属既有预算）。

**实测。** 探针 `/tmp/pg/pages6.c`（64 MB 峰值、零 root、之后 140 万次分配、**只**调 `extc_rc_maybeCollect`）：base（v23）= 64 slabs / `slabRel=0` / RSS 43,788 kB ⇒ v24 = **16 slabs / `slabRel=48` / RSS 18,832 kB**，残差 = 8 MiB slack 余量。四形状交错 A/B（15 reps，min ms）：promote 1.013 / sizeratio 0.970 / chase 1.015 / cycleratio 1.002，RSS 逐形状不变（±4–64 kB）；指令数 87,871,190 ≤ 88,029,115（v23 为 87,871,102，+88）。

**停顿。** 同轮交错 base/pat 双臂（pb 50M、cyc 20M，各 5 rep，wall、`taskset -c 6`）：`gt50us` 中位 30 vs 31（pb）、21 vs 23（cyc），`[histo] n` 逐 rep 逐字相同 ⇒ 分布不可区分；full gate（REPS=3）判 pb/cyc/pc 的 G1②(a) PASS，唯一 RED 是既有漏 `pd_pointer_dense`（R4 fanout）。偶发的毫秒级单步在 base 臂同样出现（wall 口径含整机抢占）。

**残余。** ①`M_slab ≈ 69 kB`（含活对象的块，元数据）与 ②8 MiB 的 slack 常数 ③free 队列的 1000 µs 硬截止档在自动路径上出现得更频繁 —— 三者都不随活集增长，但都是加法常数；要消掉只能搬移式压实（停顿 ∝ 活集、需转发指针），写屏障已否决（m16773）。

### 19.11 停顿契约的机制化：四个结构性无界孔 + 无钟工作量界（v25 新增）

§19.8–§19.10 解决的是**容量**项；停顿侧还剩一条用户级要求没被机制回答：「任何形状都不许出现毫秒级单步」。本轮把「[step] 测得的 max」与「一步能做多少工作」分开：前者在本宿主上不可判（§19.11.1），后者由编译期常量封顶（§19.11.2），并把界写进 `[wbudget]` 与门禁 G1④（§19.11.3）。

#### 19.11.1 为什么「测得的 max」不能当判据

探针 `/tmp/ctl/ctl.c`：纯自旋（每迭代两次乘加、共 20 万次迭代，单轮约 59.5 ms），每轮用 `CLOCK_MONOTONIC` 与 `CLOCK_THREAD_CPUTIME_ID` 各测一次，`taskset -c 6`，4 次：wall max **87.7 / 123.7 / 102.2 / 105.7 µs**，cpu max **88.8 / 140.8 / 235.7 / 108.3 µs**，`over1ms=0`。单次迭代约 70 ns 的工作，宿主能注入 88–236 µs 的「停顿」；且出现 **cpu > wall** ⇒ `CLOCK_THREAD_CPUTIME_ID` 的记账把被抢占时间也算进来。因此「所有形状 `[step] max ≤ 50 µs`」在本宿主上结构性不可达，与回收器无关。旁证：同一形状的**同类步**（`pops=128`、`pushes=0`、`seg=0`、`marked=128`）耗时在 8–152 µs 之间（15 倍），跨核（`-c 6/10/14/20/22`）max 129–219 µs 无系统性差异。

处置（判据方向只收紧）：`max` 与 `[top]` **只作 OBS**；`over1ms=0`、`>50 µs` 占比、p99.9 三组既有判据一条不动；**新增**三条 clock-free 上界（19.11.3）。

#### 19.11.2 四个结构性无界孔与处置（v25 机制）

| 孔 | 机制（v24 及以前） | 为什么无界 | 处置（v25） | 一步的界 |
|---|---|---|---|---|
| H1 扇出 push | 描述符扫描只受「对象有多少引用字段」约束 | 一个 pop 可 push 任意多个 child（codegen 描述符上限 4096 字段） | `rcEmitTypeDesc` 上限 4096 → **32**（`codegen.c:3888`，超过即退回保守扫描）+ 批级 push 记账 `EXTC_RC_MARK_PUSH_STEP=2048`（`reclaim.c:1259`、守卫 `:3665-3666`） | push ≤ 2048 + 200×32 = **8448** |
| H2 内联保守扫描 | 对象词数 ≤ `SCAN_CHUNK` 时在 pop 内联扫完，不记账 | 一步 128 pop × 每 pop 8192 词 | 步内扫描额度 `_scanLeft`（`:3543`，扣减 `:3445`），超额度改记游标续扫（`:3437-3442`） | 内联词数 ≤ **8192** |
| H3 标记栈无软上限 | 栈深只受堆形状约束，`realloc` 加倍 | pd 实测 `msp=804,352`、`mscap=1,048,576`（8 MB）⇒ 下一次增长要拷贝 8 MB | `EXTC_RC_MARK_STACK_SOFT=8192`（`:1274`，续扫条件 `:3551`） | 栈高水位 ≤ **16384** 条（≤128 KiB 拷贝） |
| H4 free 队列 1 ms 硬帽 | `fqBytes > 8 MiB` 时 `fqDrainCore(0, 1000)` | 该档出现在自动路径（`slabNew` 前、sweep 收尾） | `EXTC_RC_FREE_DEADLINE_US` 100 → **40**（`:833`）、`EXTC_RC_FREE_HARD_DEADLINE_US` 1000 → **50**（`:841`） | 一次排空 ≤ **50 µs** + 一块 slab |
| H5 巨块 munmap | 死巨块在 free 队列里一次 `free()` | 8 MiB munmap 实测 118 µs、32 MiB 553–901 µs，不可分 | `[rt:hugefree]`（`:858-940`）：载荷按 `EXTC_RC_BIG_DROP_BYTES=512 KiB`/步 `madvise(MADV_DONTNEED)`，最后再 `free` | 一步丢页 ≤ **512 KiB**（≈6.7 µs） |

H5 的依据是实测的物理常数（`/tmp/ctl/unmap2.c`，`taskset -c 6`）：munmap 代价跟**驻留页**走，不跟虚拟大小走 —— 1 MiB touch 后 13.6–20.9 µs、8 MiB 116.6–119.6 µs、32 MiB 552.9–901.4 µs；8 MiB **未 touch** 仅 0.3 µs；8 MiB touch 后先 `madvise(DONTNEED)` 再 munmap 只剩 **2.0–2.4 µs**；512 KiB touch 后 6.5–6.9 µs。

另一条硬顶：压力档会把一步的 pop 预算乘性上调，`EXTC_RC_MARK_STEP_POP_MAX=512`（`:1256`、clamp `:3539`）是它的上限；冷档仍是 32（`:1266`，`EXTC_RC_MARK_COLD_BATCH`）。

#### 19.11.3 无钟工作量界（本轮新增的正面判据）

`[wbudget]`（`:4320`）在 v25 多出三个**纯计数**字段，全部是「一步做了多少工作」的高水位，更新点在 `:3741-3743`，**不读钟**：

    [wbudget] W=0 tot_max=512 assist_max=1 mscap_max=16384 msp_max=16384 pop_max=512 push_max=0 pops_tot=931910182

门禁侧（`tests/gc-p0/g1c.py --require-workbound`，`run.sh` 已带该开关）新增 G1④：`pop_max ≤ 512`、`push_max ≤ 8448`、`mscap_max ≤ 16384`；缺字段记 N/A 且 N/A 进用例聚合（不静默变绿）。红自测 S34（三个字段各一条 RED）与变异测试（把聚合处的 `vs.append("RED")` 改成不可能命中 ⇒ S34 立即变红）都已跑过。

同时把 cold-dense 的 tau 判据从「`[top]` 最慢步的 步时/pop」改成**聚合口径** `tau_agg = 总步时 / 总 pop`（`PER_POP_US=2.0` 一个字没改），旧口径降为 OBS：因为最慢步一旦被宿主 steal 打中就是几百 µs 的假值。红自测 S35（聚合 4.6 ns/pop ⇒ PASS；2.3 µs/pop ⇒ RED）。

#### 19.11.4 停顿契约（v25 口径）

    P_step ≤ C_pop·W_pops + C_push·W_pushes + C_scan·W_words + 固定开销
    W_pops ≤ 512（冷档 32）      W_pushes ≤ 8448      W_words ≤ 8192（内联与段扫同额）
    段扫另受 EXTC_RC_SEG_DEADLINE_US = 40 µs 约束（每 512 词查钟）
    free 排空 ≤ 50 µs + 一块 slab；巨块丢页 ≤ 512 KiB/步；标记栈 ≤ 16384 条

四条合起来：**单步工作量与形状无关**。这是「任何形状都不出现毫秒级单步」的机制层答案；「测得的 max」由 19.11.1 证明不可判，只作 OBS。

#### 19.11.5 实测（v25，`build/extc` = `11d39dee…`，`taskset -c 6`）

`pd_pointer_dense` 200 轮、`EXTC_RC_PAUSE=1` + `EXTC_RC_STEP_CPU=1`，3 rep：

| 量 | v24 | v25 |
|---|---|---|
| `[step]` n | 1.5 M | 5.82–5.85 M（冷档 128→32 ⇒ 步数 ×4） |
| `[step]` `over64` | 158 | 64–79（≈1.1e-5） |
| `[step]` `over1ms` | 0 | **0** |
| `[segsteps]` max | 160.7 µs | 144–223 µs（绝大多数 8–16 µs） |
| `[wbudget]` `mscap_max` | 1,048,576 | **16,384** |
| `[top]` 前 8 名 | 含 `sweep`（巨块 munmap）239 µs | **全部 `kind=mark`**（pops=32 或段扫）⇒ 巨块 munmap 已离开慢步榜 |

真实冷 pop 成本：pd 的总步时 / 总 pop = **4.6 ns/pop**；旧口径 `tau_t` 的 5.756–17.325 µs/pop 是「地板 ÷ 32 pop」的伪指标。

门禁与吞吐（同一构建）：`tests/reclaim/run.sh` 216/0；`tests/reclaim-perf/gate.sh` **87,886,765 ≤ 88,029,115**；`tests/gc-p0/run.sh all` EXIT=0，G1② 七用例中 cyc/pb/pc/pd_bigobj/pd_pointer_dense/rel PASS、`pd_bigobj60` N/A（`n_seg=7,358 < 1e5`，轮数由 G18 的定义决定）；G1④ 全 PASS；四形状交错 A/B（7 reps，顺序执行）promote 0.982 / sizeratio 0.998 / chase 0.998 / cycleratio 0.977，RSS 略降。

#### 19.11.6 残余

①`push_max` 只覆盖批路：保守内联与段扫路径的 push 由扫描额度（≤8192 词/步）间接封顶，未逐条计数，故 `push_max` 是下界而非全量。②冷档 32 让 `pd_pointer_dense` 步数 ×4、总步时 +5%，这是「冷 pop 约 1 µs」的必然代价。③`bseg-cal.tsv` 的 `c_seg` 标定仍是 v9 口径（`src_md5` 陈旧，每跑 gate 都打印重标定提示）。④`M_slab ≈ 69 kB` 与 8 MiB slack 两条容量常数照旧（§19.9–§19.10）。

### 19.12 标记路径由屋顶选：冷档批量、暖档浅栈（v28 新增）

#### 19.12.1 为什么它是「停顿契约」的一部分

§19.6 的冷档把每步 `b` 压到 32，是为了让巨活集形状的**单步**回到契约内。但同一份生成 C 上还有第二个旋钮：一步里 pop 出来的对象**怎么扫**——浅栈快路（一次 pop 一个，串行指针链）还是批量路径（先攒一批再扫，让 payload 的 miss 重叠）。R-C 的延迟屋顶实测（`GC-ROOFLINE.md` §6.16.3）把这件事定量了：166 MB 随机链、**同一活集**、callgrind 三口径（Ir / D1 / LLd）逐位相同，

- 浅栈：0.88–0.93 s，`τ_pop` 86–91 ns，`m=1`（一次只有一个 miss 在飞）；
- 批量：0.37 s，`τ_pop` 20.5 ns，`m≈4.8`（一批 miss 同时在飞）。

指令数一样，差的是 miss 能不能重叠。于是「走哪条路」决定式 §19.4 里 `P_step ≤ b_eff·τ_t + c_fix` 的 `τ_t`，四倍差距；它和 `b` 一样属于停顿契约，而不是实现细节。

#### 19.12.2 机制（v28，`src/back/reclaim.c` 两处纯插入共 6 行）

| 位置 | 行（v28）| 内容 |
|---|---|---|
| 宏区 | 3339–3341 | `EXTC_RC_MARK_SHALLOW_WARM_ONLY`，默认 1；`#endif` 后接既有 `EXTC_RC_MARK_COLD_BYTES`（16 MiB）/`EXTC_RC_MARK_COLD_BATCH`（32）|
| `extc_rc_markStep` 浅栈判定 | 3623–3625 | `#if EXTC_RC_MARK_SHALLOW_WARM_ONLY` / `&& (int64_t)r->live <= (int64_t)EXTC_RC_MARK_COLD_BYTES` / `#endif` |

三条性质：

1. **不新开常数**：阈值就是冷档那一个旋钮 `EXTC_RC_MARK_COLD_BYTES`（16 MiB，v25 起）。一个活集只有一个「冷不冷」的答案，路径与每步预算必须用同一个答案。
2. **`r->live` 是确定的**：sweep 收尾刷新，恒等于上一轮标记到的字节数 ⇒ 路径选择不影响 `[work]` 的三跑逐字确定性（`gate-reclaim-work` 的断言），也不读钟。
3. **0 值退回 v27 行为**：`-DEXTC_RC_MARK_SHALLOW_WARM_ONLY=0` 用于 A/B 与大缓存机器移植（`GC-ROOFLINE.md` §2.5 四步）。

#### 19.12.3 契约（形状）

| 项 | 式 | 判据（无钟计数）|
|---|---|---|
| 单步 pop 上界 | `pops_step ≤ b_eff ≤ 512` | `[wbudget] pop_max ≤ 512`（LAT 实测 32）|
| 单步 push 上界 | `pushes_step ≤ 8,448` | `[wbudget] push_max ≤ 8,448`（LAT 实测 32；浅栈路径为 0）|
| 标记栈高水位 | `msp_max ≤ 16,384` | `[wbudget] mscap_max` |
| 单步时间 | `P_step ≤ b_eff·τ_pop(W) + c_fix`，`W` = 活集 | 均值 + 尾部率（LAT 通道）；`W > LLC_eff` 时 `τ_pop` 取批量值 |

`b_eff` 与路径都由 `W = r->live` 决定：`W ≤ 16 MiB` ⇒ 暖档 `min(b,1024)` + 浅栈；`W > 16 MiB` ⇒ 冷档 `min(b,32)` + 批量。两条都是「同一个上界下的单价」，不是放宽。

#### 19.12.4 实测（v28，`build/extc` = `e742e423…`，`taskset -c 6`）

| 量 | v27（HEAD）| v28 |
|---|---|---|
| LAT 墙钟 min / 中位（166 MB 活集，rounds=4）| 948 / 974 ms | **364 / 374 ms**（2.60×）|
| LAT `[step] total_us`（5 reps）| 692,623–785,328 | **169,753–213,724** |
| LAT Σ步时 / pop | 87–98 ns | **21.3–26.8 ns** |
| LAT 均值步时（`b=32`）| 2.93 µs | **0.74 µs** |
| LAT `push_max` / `pop_max` / `mscap_max` | 0 / 32 / 16,384 | 32 / 32 / 16,384 |
| LAT RSS | 311,972 kB | 311,972 kB（不变）|
| 暖档四形状（min 比 base/v28）| — | promote 0.974、sizeratio 1.000、chase 1.000、cycleratio 1.000（11 reps 交错）|
| 暖档四形状 RSS | — | 逐形状相同（5,536 / 2,656 / 65,444 / 2,660 kB）|

暖档形状的生成 C 只多 8 行预处理文本（`diff` 全列见 `GC-ROOFLINE.md` §6.17.2），机器码 `.text` +32 B 逐形状相同 ⇒ 「不退化」是结构性的，不是测不出来。

#### 19.12.5 边界（三条）

1. **阈值即冷档阈值，只对本机 `LLC_eff`（8–12 MB）成立**：16 MiB 门槛在 LLC 更大的机器上会把「暖但 20–30 MB」的活集判成冷档 ⇒ 仍正确，但白付批量固定成本（指令数相同，损失只在该项常数里）。移植按 `GC-ROOFLINE.md` §2.5 四步重标，`-D...=0` 是退路。
2. **`r->live` 是上一轮的标记量**：堆刚长大后的第一轮仍按旧值选路径，第二轮起正确；与冷档 clamp 的语义完全一致，不是新偏差。
3. **它不改 Ir、不改 RSS、不改停顿上界**：改的是同一上界下每 pop 的单价。所以 `λ(r)` 不因这一轮下降 —— 下降的是冷档形状的墙钟，以及「路径退化把 `τ_pop` 抬到 300 ns、`b=32` 破 8 µs」的那个风险（`GC-ROOFLINE.md` §6.16.7 第 1 条）。
### 19.13 大槽的跨区出边用 64 B 组位图登记（v29 新增）

#### 19.13.1 为什么它是「停顿契约」的一部分

§19.11 把标记侧的可续扫与每步常量上界写成了无钟计数；释放侧还剩一处**不可续、且与对象大小线性**的工作：一个对象的跨区出边在它死的时候必须逐条撤销。对没有描述符的类型（`EXTC_RC_CONSERVATIVE`，`src/back/codegen.c:3888` 的 `if (offs.len > 32) return "((const ExtcRcType *)-1)";`）只能按 `ss/sizeof(void*)` 扫完整块，于是单步工作量是 `Θ(ss/8)` —— **对象多大，停顿就多大**，而 `ss` 由类型定义决定（`[N]?mut ref T` 的 N 写在程序里）。`tests/gc-torture/src/t04_bigchurn.extc`（8 MiB 槽、1,048,576 个引用域、其中 960 个曾非空）实测该步 **1182–1530 µs**。

两条路被排除：**摊到多步**用滞留换停顿（原型已否证，RSS 3.4 MB → 269 MB，见 `UNLINK-DEFERRAL.md`）；**给源槽加标记**＝新增写屏障（用户 m16773 排除）。剩下的第三条：在**已经存在**的跨区登记分支上顺手记一位 —— 不在指针写点加判断，只记「这 64 B 里有一条跨区出边」。

#### 19.13.2 机制（v29，`src/back/reclaim.c` 唯一改动文件，4,738 行 / `845c57e2…`）

| 件 | 位置（v29 绝对行号）| 内容 |
|---|---|---|
| 开关 | 221–241 | `EXTC_RC_WMARK`（默认 1；0 = 位图前行为，供 A/B）、`EXTC_RC_WMARK_MIN_BYTES 4096` |
| 槽位图 | 290–293 | `ExtcRcSlab` 尾部加 `uint64_t *wmark; uint32_t wmW;` |
| 区标志 | 350–351 | `ExtcRcRegion` 加 `int32_t wmUnsafe;`（整区退回全扫描，单调置位）|
| 分配 | 1005–1013 | `slabNew`：`ss ≥ 4096` 时 `wmW = ceil(ss/4096)`、`wmark = calloc(nslots*wmW)` |
| 登记 | 1972（助手）、1987–1988（入口）| `extc_rc_wmarkSlot(slot)` 由 `extc_rc_rsIncSlab(s, t, srcSlot)` 首行调用；四个调用点在 2718 / 2729 / 2785 / 2799 |
| 扫描 | 2008 起，位图分支 2013–2031 | `unlinkChildren` 的 CONSERVATIVE 分支先走位图（读置位组的 8 个词、读后清零、命中跨区即 `rsDecSlab`）|
| 销毁/复位 | 1669、3264、3919–3920、4031 | 两处 `free(s->wmark)`；elide 整块复位与 bulk reset 各 `memset` 位图 |

三条几何事实：

1. **一位管 64 B**（8 个引用词），每槽 `wmW = ceil(ss/4096)` 个 64 位词；每块 slab 的位图恒 `nslots × wmW` 词 ⇒ 512 KiB 块约 128 词 = **1 KiB**（与步长无关），8 MiB 单槽块 16 KiB（0.2%）。
2. **热路径只多一次寄存器传参**：位图集写在**体外**函数 `extc_rc_rsIncSlab` 里（原函数加第三参）。原因不是美学：把同样逻辑内联进 `extc_rc_storeRefOwned`（`always_inline`）会让**从不执行**的代码产生 +435k Ir 的寄存器分配噪声（12 个变体标定，`GC-ROOFLINE.md` §6.18.3），Ir 棘轮破线；三参形式回到 87,914,793 ≤ 88,029,115。
3. **位图是超集**：它只回答「这 8 个词里可能有一条非空跨区引用」，真值在释放时从内存重读 ⇒ 覆写/清空引用**不需要**维护位图，所以这不是写屏障，只是既有登记分支上的一个位或。

兜底：整值拷贝两条路径（`storeStructCons` 的 `hostKnown` 分支、`storeStructCopy`）没有源槽可指 ⇒ 置 `r->wmUnsafe = 1`，该区随后所有 CONSERVATIVE 槽退回全扫描（正确性不变，性能回落到位图前）。

#### 19.13.3 契约（形状）

    单步内单个死槽的撤销工作量  W_unlink(ss, m) ≤ min( ss/8 , 8·m )  个引用域

其中 `m` 是该槽本轮置位的 64 B 组数（`0 ≤ m ≤ ss/4096`）。判据仍是无钟计数：`[wbudget]` 的 `pop_max ≤ 512` / `push_max ≤ 8,448` / `mscap_max ≤ 16,384` 不动，另加 torture 的 `over1ms = 0`。

**诚实声明**：它只闭合**稀疏**侧。若一个对象的每个 64 B 组都写过跨区指针（`m = ss/4096`），上界回到 `ss/8`（仍是 ≈1.2 ms / 8 MiB 槽）；稠密侧需要「按对象域数计费/声明」的契约路线，本轮不做。语料定位：全语料 1,396 个程序里只有 10 个类型的描述符超 32 项（3 个 n=64、7 个 n=4097），真正超过 1M 域的只有 t04 这个压测程序。

#### 19.13.4 实测（v29，`build/extc` = `b5ad0e7c…`，`taskset -c 6`）

| 量 | wmark=0（位图前）| wmark=1（v29）|
|---|---|---|
| t04 `[step] max_us`（3 rep）| 1106.4 / 871.6 / 671.6 | **4.7 / 11.5 / 16.7** |
| t04 over1ms | 1 / 0 / 0 | **0** |
| t04 `[step] total_us` | 43,126 | 170–183 |
| t04 `rs_inc / rs_dec / rs_live` | 960 / 952 / 8 | 960 / 952 / 8（逐字相同）|
| 四形状 A/B（15 reps，min）| — | promote 0.961、sizeratio 0.991、chase 0.966、cycleratio 0.991 |
| RSS（四形状）| — | 逐形状不变 |
| `tests/reclaim-perf/gate.sh` Ir | 87,695,414（HEAD 基线）| **87,914,793 ≤ 88,029,115** |

门禁：`tests/gc-torture/run.sh all` 12 通过 / xfail 0 / 失败 0（t04 的 `xfail` 字段已删，棘轮只许变严）；`asan` 5/5；`tests/reclaim/run.sh` 216/0；`tests/gc-p0/run.sh all` EXIT=0（G18 PASS 26,272 / 26,268 ≤ 26,624、G4-Stress PASS）。

#### 19.13.5 边界（三条）

1. **位图内存按块付费，不是按容量付费**：`nslots ∝ 1/ss`、`wmW ∝ ss` ⇒ 每块约 1 KiB，不改变 §19.9/§19.10 的容量界。
2. **`wmUnsafe` 单调且无清零路径**：语义安全（退回全扫描），但意思是「用过整值搬运的区拿不到这一轮的好处」；当前语料触发面小（`pd_*`、`t05_copyrefs` 是刻意构造），换真实程序前要重新 census。
3. **Ir 余量只剩 114,322**（88,029,115 − 87,914,793）：任何新增热路径代码都要先算这笔账；本轮为了不破线，把逻辑放在既有体外函数里而不是内联体里。

### 19.14 idle 证明的第四判据 (d)：不丢**内存**（v30 新增）

§18 的 idle 证明（判据 (a)(b)(c)）与 §19.13 的位图登记都只回答一个问题：**这个单元会不会丢活引用**。
三条判据全部是引用判据 —— 字段写几次、ref 局部量是否被重新赋值、有没有丢引用的操作。一个只玩
`slice<u8>` / `[N]u8` 的程序一条 `ref` 都没有，(a)(b)(c) 因此**空洞成立**，整单元判 idle、`needReclaim = false`，
生成物里一个 `extc_rc_alloc` 都没有，所有 `new T[n]` 走帧 arena 的 bump 分配。

在没有覆盖写的时候这是对的：块作用域里的数组在块末由 `extc_arena_release` 整片归还（探针 `loop1.extc`：
0 处 `extc_rc_alloc`、4 处 `extc_arena_release`、LiveSet 恒定 64 MiB、峰值 61–66 MB）。但它漏掉了另一种形状：
**函数级 `var b = new T[n]` 在循环里被重新赋值**。此时旧块的 `data` 字被直接覆盖，没有任何钩子；帧 arena 要到
函数返回才归还 ⇒ 峰值 = 轮数 × 每轮字节，与活集无关（探针 `loop2.extc`：20 轮 × 64 MiB，活集 64 MiB，
v29 实测峰值 **1,312,556 kB**，生成物 0 处 `extc_rc_alloc`）。

#### (d) 的判据

**(d) 单元里不存在「函数体帧级别的数组在循环体内被新建」。** 命中条件三条合取（`rcMemExprProbe`，codegen.c:14348）：
表达式是 `EX_NEW`、带 `count`（`new T[n]` 形式）、当前循环深度 `depth > 0`、且 `planArenaLevel(e) == 1`
（存储落在函数体帧 arena —— 也就是「没有更早的释放点」的那一级）。语句走查用 `rcMemStmtProbe`（codegen.c:14361）
维护循环深度（`for`/`par` 在 AST 里已脱糖成 `while`），`rcBodyGrowsMemory`（14375）对一具体函数体求值，
`unitGrowsMemory`（14442）对整单元的具函数与 generic 方法体合取。

判定位置在 `scanUnitReclaim` 的 idle 分支 (2b)：`if (unitReclaimIdle(g, m)) { if (!unitGrowsMemory(g, m)) { needReclaim = false; return; }
g->rcMemGrowth = true; ... }`（codegen.c:14543 与 14547）。即：**idle 证明的两半从「不丢活引用」扩成「不丢活引用、也不丢活内存」**，
两半都成立才允许整段不发射运行期。

#### 为什么这类单元必须留在清扫档，不能升 T1

单元一旦判非 idle，档位联动会在「无环证明成立」时把 `rcSweepFree` 置 1（T1 事件档：计数开、清扫关）。
T1 档的内存界靠**引用计数把垃圾变成 0**，它数的是引用，不数字节：字节数组没有 holder，没有计数可开。
所以 v30 在档位闸门上加了一条否决（codegen.c:16268–16270）：
`if (g.rcSweepFree != -1 && g.needReclaim && !g.rcMemGrowth && unitAllAcyclic(&g, m))` —— `rcMemGrowth` 的单元
永远拿不到 T1，只能停在 T2 清扫档；只有清扫档能收掉「被覆盖的帧级数组」。

#### 语义修正：`EXTC_RECLAIM=1` 与 idle 证明的顺序

`EXTC_RECLAIM=1`（开档）的用途是「同一份语义、强制发射运行期」，供门禁对照。它的实现是 `if (g->rcForced) return;`
绕过最后那道「没有受治理分配点就不发射」的剪枝。**它不是「跳过整趟 pass」**：idle 证明仍然先跑，
`rcForced` 只作用于剪枝。v30 曾把这条早退提到 pass 开头，结果是 bt / rebuild / `hole_c` / elide 四个门禁
的「开档」产物多出发射运行期 —— Ir 棘轮（bt 3,152,725 > 2,950,000、rebuild 18,440,147 > 15,790,000）、
税线（bt 1.113× > 1.050×）、`hole_c` 的「家 arena 不许发清扫触发」、elide 的计数门禁同时变红。
现位置在 `for (... rcBodyHasGovernedAlloc ...)` 之前（codegen.c:14578），与 v29 一致。

#### 实测

| 形状 | v29 峰值 | v30 峰值 | 生成物 `extc_rc_alloc` | 档位 | 墙钟 |
|---|---|---|---|---|---|
| `/tmp/av/loop2.extc`（20 × 64 MiB 覆盖写，活集 64 MiB） | 1,312,556 kB | **199,464 kB** | 0 → 11 | T0 → T2 | 0.57 s → 0.09 s |
| `tests/gc-torture/src/t12_overwrite_arena.extc`（同形状，52 行） | 1,313,212 kB | **199,264 kB** | 0 → 11 | T0 → T2 | 0.57 s → 0.093 s |
| `/tmp/av/loop1.extc`（块作用域内新建，每轮块末释放） | 61,556 kB | 61,556 kB | 0 → 0 | T0 | 不变 |

t12 带 `-DEXTC_RC_PAUSE=1 -DEXTC_RC_STATS=1` 的运行期读数：`thr=134,217,728`（= 2L）、`M_max=201,326,592`（= 3L）、
`[chan] trigger-step n=10 max_us=25.0 total_us=118.9`、`[work] n=10 walk_max=1`、`maxrss=199,648 kB`。
峰值 = live(64 MiB) + 两代垃圾(2 × 64 MiB) + ~7 MB 元数据 = 199 MB，**与轮数无关** —— 这就是 §19.4 的
`M ≤ (1 + κ_M·r)L` 在该形状上第一次成立。

#### 边界（写清楚）

1. **只覆盖「覆盖写」**：块作用域数组不需要 (d)（块末已归还，实测峰值平）；「每轮 `new` 并存进容器/长寿命槽」
   的累积型本来就被 (c) 判否（临时量/容器替换），不走这条新判据。
2. **(d) 是「有没有可能丢内存」的充分条件里的保守一侧**：命中即发射运行期，不判断实际是否真的泄漏。
   代价是全语料 1,641 个源文件里只翻转 3 行（见 §19.14 末表），且三行的墙钟/RSS 都在噪声内。
3. **idle 指纹基线只能往严的方向改**：`tools/idle-fingerprint-baseline.tsv` 的 `same` 列从 YES 变 NO 要写台账
   （`tests/reclaim-idle/README.md` 台账节），反向（NO → YES）视为放宽，需单独审。

### 19.15 巨块载荷的归还与「复用缓存」的容量预算（v31 新增）

**为什么单列一节。** §19.10–§19.14 解决的是「块什么时候离开链表」，但有一类块从来没有资格离开：**巨块**（`cap < 2` ⇒ 一个 slab 只有一个槽，槽的载荷由 `extc_rc_bigPayloadAlloc` 单独映射）。v30 之前 `extc_rc_slabShrink` 与 `extc_rc_pageTrimStep` 都以 `if (s->big) ...` 跳过巨块，而巨块载荷的释放点只有三个（free 队列排空、region 析构、进程退出）⇒ 一个「拷贝增长」形状（旧代不可达、新代更大）的峰值等于**各代之和**，与活集无关。

**实测（这一节的全部数字都在本机 core 6、`/usr/bin/time -f %M`、3 次取中位）**：

| 形状 | 活集 | v30（HEAD `853e46d0` 树内构建） | v31 |
|---|---|---|---|
| `grow11`（函数级 `slice<i64>` 在循环里被 1.1× 增长替换，8M 元素） | 63.4 MB | **749,980 kB / 0.32–0.44 s**（11.7×，等于累计分配量） | **240,716–241,084 kB / 0.34–0.38 s**（3.8×） |
| `grow11mr`（同上形状重复 10 / 20 / 40 轮） | 16 MB | 不适用（无量） | 126,184 / 122,916 / 130,744 kB ⇒ 轮数无关 |
| `growround_slice`（20 轮，每轮 1.5× 从 1,024 长到 1,048,576 个 `i64`） | 8 MB | 37,296 kB / 0.09 s / 12,307 缺页 | 37,300–37,308 kB / 0.08–0.09 s / 12,310 缺页（= 不归还的基线） |
| `t12_overwrite_arena`（20 轮 × 64 MiB 同尺寸覆盖） | 64 MB | 199,264 kB / 0.08 s | 199,264 kB / 0.09 s |
| `rcslice_ns`（单次 1.5× 增长到 10.2M 元素） | 82 MB | 241,572 kB | 242,224 kB（不变） |

两件事一起看：拷贝增长形状的峰值从「各代之和」降到「约一代的量」（相对最终活集 3.8×），而**同尺寸反复分配的形状逐 kB 不动** —— 后者是复用缓存的功劳，前者是归还的功劳，缺一个就会在另一侧塌。

**机制三项（全在发射文本 `RECLAIM_RT[]` 内）**

1. **空巨块可以离开链表**：`extc_rc_slabShrink` 的跳过条件从 `if (s->big || s->nLive != 0 || s == r->bumpSlab)` 收成 `if (s->nLive != 0 || s == r->bumpSlab)` ⇒ 巨块与普通块同权；载荷进 `ExtcRcBigDrop` 链，由 `extc_rc_bigTrimStep(budget)` 逐段 `madvise(MADV_DONTNEED)`（v25 的 `[rt:hugefree]`）。
2. **丢弃速率绑在「让堆变大的那次分配」上**：`extc_rc_slabNew` 的巨块分支在 `extc_rc_bigPayloadAlloc(slotSize)` 之前调 `extc_rc_bigTrimStep(slotSize)` —— 一次分配最多丢「本次请求大小」的死载荷，于是**丢弃速率 ≥ 分配速率**，且这份页工作记在造成增长的分配头上；新增 `[chan] bigdrop` 通道单独计量（`max_us / total_us / work_max` 的 work 单位是丢弃字节）。
3. **复用缓存带宽限与容量预算**：`deadA`（上次见到空时的 `allocN + 1`，0 = 仍活）与 `deadN`（连续空了几次闭包）—— 一块空巨块**只要还在被复用就继续留**，之后再多宽限 `EXTC_RC_SHRINK_GRACE`（默认 1）次闭包，之后视为余量归还；同时 `EXTC_RC_SHRINK_CAP_LIVE`（默认 3）给出容量闸门：容量还在活集的 3 倍以内时一个块都不摘（`skip:shrink-budget`）。0 关掉闸门用于 A/B。

**为什么需要「宽限」而不是「一次没复用就还」**：`growround_slice` 的同一尺寸块在**下一次闭包之后**才回来复用，一次性判据会提前释放它 ⇒ 每轮 munmap 512 KiB 再重新 carve+触页（实测 0.15 s / 45,609 缺页 vs 0.08 s / 12,310 缺页）；宽限一次闭包即可把复用接上。反过来，没有上限的宽限会让拷贝增长形状把「每一代」都当成待复用块留住（`grow11` 240 MB 就是宽限 1 次的代价；宽限 0 可回到 189 MB，但 `growround_slice` 退回 0.17 s）。

**契约（写进 §18.5 的容量项）**

- 容量：`C ≤ C_cap + 8 MiB`，其中 `C_cap = max(3·live_at_closing, live_at_closing + 单次请求)`，`live_at_closing` 是**闭包时刻**的活集字节（瞬时量，不是峰值）；被归还的块当轮进 free 队列，`munmap` 由既有预算 `EXTC_RC_FREE_BUDGET_SLABS` / `EXTC_RC_FREE_QUEUE_BYTES` 摊开。
- 丢弃：一次巨块分配最多丢「本次请求字节」的死载荷，耗时 ∝ 请求字节（实测 47 MB ⇒ 1,390 µs，≈30 µs/MB）；这是**请求的页工作**，同一请求首次触页本身 ≈256 µs/MB（1 µs/4 KiB 页），所以丢弃只占该分配的 ≤12%。它记在 `[chan] bigdrop`，不进 `[step]` 直方图（那条路径本来就不在步里）。
- 这套界是**块级常数**，与历史峰值无关；`grow11mr` 10/20/40 轮 126/123/131 MB 就是「与轮数无关」的实测。

**用例化**：`tests/gc-torture/src/t13_growth_copy.extc`（20 轮 1.1× 拷贝增长、起始 8 MiB）是本节的回归守护 —— 同一份源在 v30 编译器下峰值 **526,816 kB 判 RED**、v31 下 **194,352 kB 通过**（阈值 260,000 kB，见 `CORPUS-TORTURE.md` §5.4）。

**边界（不许读成更强的结论）**

1. 3.8× 不是 3×：`grow11` 的 240 MB 相对**闭包时活集**仍在 3 倍预算内，但相对**最终活集**是 3.8× —— 拷贝增长的某一时刻活集是两代之和，闸门按那一刻的 `bytes` 判。要把最终比压到 ≤3× 必须关掉宽限，代价是 churn 形状每轮重触页。
2. `[chan] bigdrop` 的 1,390 µs 是**真实暂停**，只是不在 `[step]` 直方图里（分配路径不计步）。它正比于请求字节：这是「巨块分配的代价是请求的函数」的一致延伸，不是新增的无界项；但如果用户要求任何形状都不出现 ms 级暂停，则必须把丢弃速率从「请求大小」降下来，代价是容量重新跟随各代之和（同一份生成 C 上 `-DEXTC_RC_BIG_DROP_CAP=4 MiB` 实测 578,444 kB）。
3. `t12_overwrite_arena` 的 0.09 s 与 v30 的 0.08 s 差一格时间分辨率；`growround_slice` 的 0.08–0.09 s 同样。逐 kB 的 RSS 相等是硬结论，墙钟差一格不当作回归。

**复现**：`/tmp/s4/` 下 `grow11.extc`（1.1× 帧级增长）、`grow11mr.extc`（多轮）、`growround_slice.extc`（1.5× 多轮）、`tests/gc-torture/src/t12_overwrite_arena.extc`；编译器 v31 = `src/back/reclaim.c` = `29a6372eb0fac935f215847df35ccf41`（4,780 行）、`build/extc` = `f82ee508c6fe7233f228fc0decc00b9c`。运行期统计要 `-DEXTC_RC_PAUSE=1 -DEXTC_RC_STATS=1` 编译并在运行期设 `EXTC_RC_PAUSE=1`。

### 19.16 容量归还收敛成一个水位（v33 新增）

**v31 的形态（四个旋钮，互相串联）**：容量归还由四段机制拼成 —— 块级摘链（`extc_rc_slabShrink`）、页级归还（`extc_rc_pageTrimStep`）、复用宽限（`deadA`/`deadN` 按 `allocN` 推断「这块还热吗」）、容量比例闸门（`EXTC_RC_SHRINK_CAP_LIVE` 把 `capBytes` 与 `bytes · 3` 比）。四段各自带判据与旋钮，且**串联生效**：2×2 探针（`CAP_LIVE ∈ {3, 0}` × `GRACE ∈ {1, 0}`）显示拷贝增长形状 `grow11` 只有同时放松两道闸门才从 240 MB 掉到 189 MB，任一道在就停在 240 MB；把闸门里的尺子从 `bytes` 换成 `live` 完全无效（240,708–240,716 kB）。结论：**峰值当时由两个比例判据决定，不是由机制决定**。

**机制（一个水位 + 一个 pass）**

```c
static int extc_rc_surplus(ExtcRcRegion *r) {
    return EXTC_RC_CAP_MULT > 0
        && r->capBytes > r->bytes * (int64_t)EXTC_RC_CAP_MULT + (int64_t)EXTC_RC_RETURN_FLOOR;
}
```

一句话：**刻出来的容量比活集超出一个倍数再加一个固定存量，就是过剩**。默认 `EXTC_RC_CAP_MULT = 2`（一代余量：一次 1.1× 拷贝增长从 `L` 到 1.1·L 需要 1.1·L 的容量，2·L 覆盖得了）、`EXTC_RC_RETURN_FLOOR = 16 MiB`（复用存量：给「活集很小、每轮新建大块」的形状留常驻余量，否则每轮归还都要重新缺页）。

`extc_rc_returnStep(r)` 在清扫轮收尾（与 `extc_rc_fqDrainAuto` 同一处收尾分支，pause 计时块内）跑一次，单游标 `r->trimCur` round-robin，三类动作：

1. **死块**（`nLive == 0` 且 `rsbit` 无位）⇒ O(1) 摘链 + 入 free 队列。不再有复用宽限判据。
2. **活块但页值得还**（`!s->big`、`s->pgLive != nLive` 即上次 pass 之后活槽数变过、且 `extc_rc_pageWorth(s)`）⇒ `extc_rc_pageTrimSlab` 对连续空页 run 做 `MADV_DONTNEED`。
3. 每摘一块都重新验水位，够了就停。

`extc_rc_pageWorth` 是算术充分条件，替掉 `PAGE_TRIM_DENSE`/`PAGE_TRIM_ACTIVE` 两个猜测：`per = 1 + (ss >> 12)` 表示一个活槽最多能钉住几页，`nLive · per < nslots · ss / 4096` 时才值得走位图。预算三个常数：`EXTC_RC_RETURN_VISIT_MAX = 256` 块、`EXTC_RC_RETURN_SLAB_MAX = 16` 次摘链、`EXTC_RC_RETURN_BYTES = 256 KiB` 页字节 —— 与 §19.11 的「一步工作量由编译期常量封顶」同一口径。

**删掉的十三个名字**：`EXTC_RC_SLAB_KEEP`、`EXTC_RC_SLAB_KEEP_BIG`、`EXTC_RC_SHRINK_GRACE`、`EXTC_RC_SHRINK_CAP_LIVE`、`EXTC_RC_SHRINK_VISIT_MAX`、`EXTC_RC_SHRINK_REL_MAX`、`EXTC_RC_SHRINK_MIN_SLACK`、`EXTC_RC_PAGE_TRIM`、`EXTC_RC_PAGE_TRIM_BYTES`、`EXTC_RC_PAGE_TRIM_VISIT_MAX`、`EXTC_RC_PAGE_TRIM_MIN_SLACK`、`EXTC_RC_PAGE_TRIM_DENSE`、`EXTC_RC_PAGE_TRIM_ACTIVE`；结构字段 `deadA`、`deadN`、`allocN`、`pgAllocN` 一并删除（保留 `pgLive`、`pgNext`），分配热路径上的 `s->allocN++` 也删掉 ⇒ **Ir 反而下降**（87,911,848 → 87,678,211）。

**契约**

- 内存：`C ≤ CAP_MULT · L + RETURN_FLOOR + 4 KiB · #page(含活对象) + M_slab · #slab(含活对象)`（`M_slab ≈ 69 kB`，见 §19.9）。三项都不依赖历史峰值：`L` 是 `bytes` 的即时值，后两项是当时还活着的块/页数。
- 停顿：一个 pass ≤ 256 次块访问 + 16 次摘链 + 256 KiB 的 `MADV_DONTNEED`（整段 ≈ 13 ns/KB ⇒ 256 KiB ≈ 3.4 µs）；`grow11` 实测 `[step] n=104 max_us=3.1 over1ms=0`。
- 吞吐：**还回去的容量在下次分配时按首次触页计费**（本机 ≈233 ns/KB），不是回归而是内存界的价码；见下面的实测与代价。

**实测（base = `90c876f1`（档一 = v32 提交，容量行为与 v31 相同），`/usr/bin/time -f %M`，core 6，3 次中位）**

| 形状 | 活集 | base | v33 | 读法 |
|---|---|---|---|---|
| `grow11`（帧级 1.1× 增长到 8M `i64`） | 63.4 MB | 241,096 kB / 0.35 s | **189,660 kB / 0.35 s** | 3.8× → 3.0× 活集；`FLOOR=0` 时 183,536 kB（但 `t04` 退化） |
| `grow11mr`（同形状 40 轮） | 16 MB | 108,244 kB / 3.27 s | **104,676 kB / 3.16 s** | 与轮数无关 |
| `t13_growth_copy`（20 轮 1.1×） | 49 MB | 194,732 kB / 0.21 s | **148,664 kB / 0.23 s** | 门禁阈值 260,000 kB |
| `t12_overwrite_arena`（20 × 64 MiB 覆盖） | 64 MB | 199,648 kB / 0.09 s | 199,548 kB / **0.29 s** | 峰值不变，墙钟 +0.20 s（代价，见下） |
| `t04_bigchurn`（120 轮 8 MiB 巨块） | 0 | 10,148 kB / 0.03 s | **9,764 kB / 0.02 s** | `FLOOR=0` 时退化到 0.42 s / 246,653 次缺页 ⇒ `FLOOR` 的由来 |

四形状交错 A/B（干净 15 reps、min、与任何其它计时任务不并发；`base` = `90c876f1` 树内重建）：`promote` **0.982** / `sizeratio` **1.008** / `chase` **1.012** / `cycleratio` **0.998**，中位 0.990 / 1.024 / 0.993 / 1.004 —— min 与 median 的符号逐形状翻转 ⇒ 噪声带；RSS 5,676 / 2,664 / 65,452 / 2,796 kB 对 base 的 5,672 / 2,792 / 65,448 / 2,796 kB（±128 kB）。更早一轮 11 reps 给出 0.999 / 1.030 / 1.041 / 1.005，同轮三臂（`CAP_MULT=0` 关档 / `RETURN_BYTES=0` 只摘块 / 默认）把差摊开：`sizeratio` 14.69 / 14.31 / 14.65、`chase` 114.04 / 114.39 / 115.50 ⇒ 开档与关档之间 ≤ 1.3%，判为**噪声带内**（见 §6.21.6）。

**代价：还回去的容量要重新缺页**

`t12_overwrite_arena` 是这条代价的标本：峰值逐 kB 不变，墙钟 0.09 → 0.29 s。它每轮丢掉 64 MiB、下一轮又要同样一块，于是 ≈1.28 GB 容量被还给 OS、再以 156 ns/KB 重新缺页 —— 与价目表（归还 13 ns/KB、再缺页 233 ns/KB、清扫同一段 0.5–0.9 ns/KB）预测的量一致。两个极端：`CAP_MULT = 3` 时 `t12` 回到 0.10 s 但 `grow11` 231 MB 破 3·L 线；`CAP_MULT = 0` 关档时 `t12` 0.09 s 而 `grow11` 保持 241 MB ⇒ **这是同一个旋钮的两端，没有第三种选择**。契约里因此写明：默认档承诺内存界，吞吐可以换但不是默认。

**边界**

1. `sizeratio` 与 `chase` 的 +3~4% 只出现在 11 rep 那一轮，同轮三臂差 ≤1.3%，判为噪声带；15 rep 复测在 `GC-ROOFLINE.md` §6.21.7 补。
2. 稠密侧（每 64 B 组都写过跨区指针，§19.13 的右端 `8·m`）与 L10-a（pool 槽 pin 泄漏，已随 R4b 槽地址键与池基质退场关闭，§19.21）不在本节范围。
3. `EXTC_RC_RETURN_FLOOR` 的 16 MiB 是本机取值：它由「最小要被救的巨块是 8 MiB」推出（9 MiB 已足够，取一倍余量），换语言/换尺寸档要重算。

**复现**：v33 = `src/back/reclaim.c` = `a8706acf8b9b144399e46fa2c73ad000`（241 行，渲染文本 4,780 行）、`build/extc` = `b5a05be1ce5780d34a8f50b4e322322a`；探针 `/tmp/s4/grow11.extc`、`/tmp/s4/g11mr_40.extc`、`tests/gc-torture/src/t12_overwrite_arena.extc`、`tests/gc-torture/src/t13_growth_copy.extc`、`tests/gc-torture/src/t04_bigchurn.extc`；运行期统计要 `-DEXTC_RC_PAUSE=1` 编译并设 `EXTC_RC_PAUSE=1`。
### 19.17 巨块载荷的基质：extent（保留的 VA 域，v34 新增）

这是 R2a（`docs/analysis/EXTENT-R1.md` 的结论落地）在这一份文档里的位置：它不新增对外可见的第六种基质，而是把**巨块槽的载荷从哪里来**从「一次 `aligned_alloc`」换成「从一个预留的 VA arena 里 carve 一个 2 MiB 对齐的区间」。对外的对象放置规则、槽大小分类、可达性判定、三档策略全部不变。

#### 19.17.1 在这一份模型里的位置

§2 的五个存储基质里，巨块槽此前不是基质，是 rc region 的一个特例：`slabNew` 见到 `cap < 2` 就建一个只含一槽的 slab，载荷由 `extc_rc_bigPayloadAlloc` 用 `aligned_alloc(4096, round4k(n))` 拿一次映射，由 `extc_rc_bigTrimStep` 按 `EXTC_RC_BIG_DROP_BYTES` 分片 `MADV_DONTNEED`，整条丢完后 `free`。所以它虽然挂在 region 上，**地址与页表是一次性的**：释放之后地址还给不了别人，下一个载荷再付一次 1,897 ns 的映射（R1 实测）。

v34 起这层有了自己的基质，但只服务同一个特例：

| 项 | 内容 |
| --- | --- |
| 单位 | **extent** = 2 MiB 对齐的 VA 区间（量子 `EXTC_RC_EXT_QSHIFT` = 21） |
| 容量 | 每个 arena `EXTC_RC_EXT_ORD` = 15 个 order（32,768 量子 = 64 GiB VA），最多 8 个 arena（512 GiB VA，懒创建） |
| 分配器 | buddy；每量子一个字节 `ordQ`（bit 7 = 空闲块首，低 7 位 order，`0xFF` = 块内部），空闲链 `nextQ` |
| 预留方式 | `mmap(NULL, span, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE)`，起点对齐到 2 MiB |
| 大页 | 只对 carve 出来且 ≥ `EXTC_RC_EXT_HUGE_MIN`（4 MiB）的区间 `madvise(MADV_HUGEPAGE)` |
| 入口 | `extc_rc_extAlloc(com, max, &outCom)` / `extc_rc_extRelease(p)`，包一把专用自旋锁 |
| 归还 | `extRelease` 把 VA 按 buddy 合并回空闲链；页已经由丢页链 `MADV_DONTNEED` 掉了 |
| 探针 | `[extent] arenas= va_mb= res_mb= calls= fails=`（`[return]` 之后一行） |
| 关掉 | `-DEXTC_RC_EXT=0`（或平台无 `mmap`）⇒ 载荷路径改走 `aligned_alloc`，两个入口恒返回 0/NULL |

#### 19.17.2 生命周期归谁管（这一层不改变所有权）

三件事仍然分层，没有互相越界：

1. **槽何时空出来**由 rc region 决定：`occ` 位图 + 标记/清扫（§5、§6）判定这个巨块槽不可达，`freeSlot` 清 occ 并把它压回 size-class 空闲链。
2. **载荷的页何时丢**由丢页链决定：载荷进 `extc_rc_bdHead` 链，`extc_rc_bigTrimStep` 每步丢 ≤ `EXTC_RC_BIG_DROP_BYTES`，这就是 §19.15 的 `[chan] bigdrop` 通道。
3. **载荷的 VA 何时回**由 extent 层决定：整条丢完的分支把区间还回 buddy（`extc_rc_extRelease`），下次同量级的载荷 7 ns 拿到同一个区间。

所以 extent 层不参与「对象是否活着」的判定，只回答「这块地址现在归谁」。这也是 R2b 的边界：在把 extent 登记进 region 账本之前，预留区里的内存对回收器仍是外来内存（`extc_rc_ownSlab` / `extc_rc_slabOfInterior` 找不到 ⇒ `storeRefUnknown` ⇒ pin 表），巨块槽之所以不需要这条，是它天然不会被别人的引用指进载荷内部（`extc_rc_ownBig` 只认载荷首字节）。

#### 19.17.3 实测

三臂交错（同机 core 6，5 rep 取 min；wall 秒 / maxrss kB）：

| 臂 | t04_bigchurn | t12_overwrite_arena | t13_growth_copy |
| --- | --- | --- | --- |
| `EXTC_RC_EXT=0` | 0.020 / 9,760 | 0.270 / 199,548 | 0.210 / 148,660 |
| `EXT=1, HUGE=0` | 0.020 / 9,956 | 0.260 / 199,684 | 0.210 / 156,352 |
| `EXT=1`（默认） | 0.020 / 12,024 | 0.080 / 200,584 | 0.060 / 164,624 |

结论分两句，不能混：

- **carve 便宜在墙钟上看不见**（第二臂 ≈ 第一臂）：这三个形状的时间在写页上，不在分配上。
- **大页是 2.5–3.5× 的全部来源**（第三臂）：首触一个 2 MiB 页代替 512 次 4 KiB 缺页，丢弃也按大页（2.7 vs 19 ns/KB），重触 99 vs 260–279 ns/KB（`tools/probe/thpdrop.c`）。

四形状（暖档主力）15 rep 交错 A/B：promote 0.998/1.015、sizeratio 1.015/0.993、**chase 0.956/0.973**（其 `[1048576]?mut ref` 载荷吃到 4 MiB 门槛）、cycleratio 0.997/0.992 ⇒ 除 chase 变快 3–4% 外都在 ±2% 噪声带内。RSS：大页选择化之后 5,872 / 2,668 / 65,672 / 2,796 kB，逐形状与 v33 相差 ≤ 128 kB。

#### 19.17.4 边界

1. **取整在 VA 上，不在 RSS 上**：carve 把请求向上取整到 2 的幂个量子（`区间 = max(2 MiB, 2 × com)`；实测 8.1 MiB → 16 MiB），但页只按被写的部分驻留 ⇒ 取整不改变 `M`，它改变的是 VA 预算（com < 2 MiB 时区间固定 2 MiB，最多 8×）。真正进 `M` 的页级放大是**大页尾页**：每个被触区间最多多驻留一个 2 MiB 页，记 `+ P_hp`（`P_hp ≤ 2 MiB × #被触区间`；t04 的 +2.3 MB 就是一次尾页，t13 的 +16 MB ≈ 8 个区间的尾页）。
2. **稀疏触页的放大**：t04 +2.3 MB 是「8 MiB 载荷只碰几页、但每碰拉一个 2 MiB 页」。R4 把容器存储也换成 extent 时这一项会从巨块扩到所有 ≥4 MiB 的容器。
3. **不是运行期开关**：`EXTC_RC_EXT` 是生成 C 的编译期常量（与其余 `EXTC_RC_*` 一致），`-DEXTC_RC_EXT=0` 得到一个「载荷走 `aligned_alloc`」的对照臂。
4. **overcommit 与 Windows**：`MAP_NORESERVE` 依赖 overcommit 允许；没有 `mmap` 的平台整节退化成 stub。这两条都由 `EXTC_RC_EXT_OK` 一处收口。

### 19.18 巨大块载荷的宿主解析（v35 新增，R2b）

**为什么单列**：`§19.17` 把巨块载荷的**基质**换成了 extent，但回收器对地址的**解释能力**没有跟着换——这属于「停顿/正确性契约」而不是「内存界」，所以放在本节。

**缺口（v34 口径）**：宿主反查只有一级——按 512 KiB 掩码回退到块首查登记表。巨大块的载荷是 carve 出来的区间，不对齐任何已登记块首，于是载荷**内部**（偏移 ≥ 512 KiB 的引用域）：

| 后果 | 通道 | 实测 |
| --- | --- | --- |
| 赋值走 `storeRefUnknown` ⇒ 值进 `pin` 表 | 内存（每存储 ≈17.5 B 元数据，只增不减）| `probe0` 200k/800k 轮 RSS 30,600 / 41,116 kB |
| `wmarkSlot` 置不到位 ⇒ 释放时保守分支不撤边 | 正确性（计数档永久泄漏）| `probe1`：`rs_inc=960 rs_dec=0 rs_live=960` |

**机制（v35）**：arena 在 `ordQ` 旁加等长归属表 `ownQ`（2 MiB 量子 → `ExtcRcSlab *`，256 KiB/arena，惰性 `calloc`）。`slabNew` 的 big 分支绑定、`extGive` 清量子、`extLookup` 无锁读；`slabOfInterior` 与 `wmarkSlot` 共用这级反查。

**契约**：`p ∈ [data, data + ss)` ⇒ 返回该块；容量外/已回收/表分配失败 ⇒ 返回空并回落保守方向。成本 `O(8)` 次比较 + 一次原子读；元数据 ≤2 MiB/进程。**内存界不变**（簿记不是容量）。

**实测**：`probe0` `[pin] n=0`、RSS 24,084 / 35,028 kB、2M/5M 轮 54,416 / 54,612 kB 平台；`probe1` `rs_dec=952`、`rs_live=8`、`[pin] n=0`、步 max 5.3 → 3.4 µs；`tests/reclaim` **221/0**（新增 5 条断言，ASan/UBSan 干净）；暖档四形状 A/B 0.979 / 1.030 / 1.004 / 1.027（噪声带）。

**边界**：①`pin` 表对**视图数据**的泄漏不在此列（视图槽不在任何 extent 里；池块已退场，L10-a 由 R4b 的槽地址键与区间撤销关闭、G15 升为硬判据）；②`EXTC_RC_EXT=0` 或无 mmap 平台下两个入口是 stub，退回位图前的保守路径（慢但不误释放）；③绑定发生在 `slabNew`，不在分配热路径；④R3/R4 会让更多地址落进 extent，因此本节是它们的前置条件。

**指纹（v35）**：渲染文本 5,050 行 / md5 `1dcea37a8ea7858a350035507b7364b1`；物理 `src/back/reclaim.c` 242 行；`tests/reclaim/driver.c` 1,655 行 / md5 `b82c9c83270d859ed51418e2ecb96d05`；行号换算见 `REFS-V35.md`。
### 19.19 载荷先留地址、再原地长（v36 新增，R3）

#### 19.19.1 这一份模型里的位置

§19.17 换掉了巨块载荷的**底盘**（预留 VA 域），§19.18 让它**可解析**。这一节补的是同一栏的第三个缺口：载荷的**容量**曾经只能靠拷贝长出来，而拷贝的成本与对象同阶。R3 把「以后可能长到多大」也记进 slab（`bigMax`），于是一次增长只有触页成本。

这一层**不改变所有权**：载荷仍然属于它所在的 region，随 slab 的释放整段还给 arena；`bigMax` 只是「这块载荷的地址最多能长到哪里」的记录。

#### 19.19.2 机制

| 位置 | 内容 |
| --- | --- |
| `ExtcRcSlab.bigMax` | 巨大块专用：载荷可原地长到的上限（字节）；普通块为 0（容量就是块本身） |
| `extc_rc_bigPayloadAlloc(n, &reserved)` | `m = bigRound(n)`；`EXTC_RC_EXT_SLACK > 0` 时 `max = clamp(m·SLACK, SLACK_MIN(2 MiB), SLACK_MAX(1 GiB))`，否则 `max = m`；`extc_rc_extAlloc(m, max, &com)` **只提交 `com` 页、只预留 `max` 地址**；carve 失败退回 `aligned_alloc` 且 `reserved = m`（自己的映射没有预留） |
| `extc_rc_bigGrow(s, n)` | `want = bigRound(n)`；`want ≤ ss` ⇒ 返回现有容量；`want > bigMax` ⇒ 0（调用方搬）；否则组位图按新步长重建、`ss`/`rcp` 换步长、`region->bytes/allocBytes/capBytes` 各 += `want − ss`；新窗口不清零（内核首次读给零页） |
| `slabNew` | 把 `reserved` 写进 `s->bigMax`；此后归还仍按 `bigRound(n)`（= 已提交 com）记 drop 链 |

#### 19.19.3 契约

- **预留上界**：`bigMax = clamp(bigRound(n)·SLACK, 2 MiB, 1 GiB)`；生产默认 `EXTC_RC_EXT_SLACK = 0`（等于 R3 之前的行为），驱动测试用 16 打开。
- **增长成本**：预留之内 O(1) 账本写（+ 组位图可能一次 `realloc`），零拷贝、零前置清页；新窗口的页由调用方第一次写时按页付钱。
- **拒绝是诚实的**：`want > bigMax` ⇒ 返回 0，调用方搬到新对象，旧代由清扫回收；容量界不变。
- **地址稳定性**：预留之内增长不搬 data（`EXTC_RC_SLAB_DATA(s)` 不变）⇒ 已发出的元素地址仍有效（R4 的 `buf` 不变量要用）。
- **归还按 used**：drop 链记 `bigRound(n)`，VA 尾巴整段还给 arena；一步丢弃量仍由 `extc_rc_bigTrimStep(budget)` 封顶 —— 容量界与停顿界各由原来的那个量给出，R3 没有新增旋钮（只有一个 slack 倍数，默认关）。

#### 19.19.4 实测

探针 `slackgrow2.c`（通过发射出来的运行期文本，8 MiB 载荷按 1.1× 长到 64 MiB，core 6）：

| 臂 | bigMax | 步数 | 增长墙钟 | minflt | RSS HWM |
| --- | --- | --- | --- | --- | --- |
| slack = 16，原地长 | 128 MiB | 22 | **15.6–20.2 ms** | **61** | 67,892–67,896 kB |
| slack = 16，只预留 | 128 MiB | 0 | 0.04–0.05 ms | **2** | **10,416–10,420 kB** |
| slack = 0 | 8 MiB | — | 第一步拒绝 | — | — |

对照同形状（8 MiB 起始、同会话）的拷贝增长（`tools/probe/reserveproto.c`）：**375.7–394.5 ms** / 峰值 130.8 MB ⇒ 原地长快 **17–25 倍**，且峰值只有 1.06× 活集。第二行说明预留只花地址空间：`extLive = 128 MiB` 是 VA，RSS 只有 10.4 MB。

驱动新增 **13 条断言**（`tests/reclaim` 221 → **234/0**，ASan/UBSan 与不内联档全绿）：容量对账、基址不变、已写字节保值、新窗口读作零、账本三项同增、可长到 `bigMax`、超限拒绝且步长不变、归还后 `extLive` 回到分配前。

#### 19.19.5 边界

- **slack 是赌终值的**：16× 起步意味着「不会长过初始的 16 倍」，超过就搬一次（并以 `2×max` 重新预留）。R1 的 slack-4× 实验说明留得不够就是白留。
- **默认关**：生成文本里还没有让载荷变长的东西（增长动词/容器存储是 R4），开着只会白占 VA 与大页尾页。R4 一旦开始用 `extc_rc_bigGrow`，这个开关必须默认打开。
- **搬家的瞬时峰值**：预留用尽那一次，新旧两代同时活着（幂等上界仍是 `2L` 再加那一刻的旧代）。R3 只降低搬家的频率，不消除它。
- **大页尾页放大**：≤2 MiB / 被触区间（与预留大小无关）。

机制与价格的完整表格见 [`GC-ROOFLINE.md`](GC-ROOFLINE.md) §6.24。

### 19.20 第五种存储基质：`buf<T>` 的预留（v37 新增，R4a）

#### 19.20.1 这一份模型里的位置

§1 数过的存储基质是五种（帧 arena / rc region / place 载荷区 / 协程任务 arena / plate）。`buf<T>` 是第六种，也是最简单的一种：**容器自己拥有一片预留的地址域**，不经过 rc 的槽、不经过 place 的载荷区，回收器完全看不见它。它的界不在 §18 的公式里，而在「一片预留」这三个字里：`reserved` 是这片预留的实际大小（量子取整），`live` 是其中用到的元素，两者都由容器自己的账本给出，可读、可测、可断言。

为什么值得单独记：它把「容量」从回收器手里拿走。§19.16 之后回收器归还容量只做一件事 —— 判断 `capBytes − bytes` 超没超水位；而一个 `buf` 的容量是它**自己**的预留，回收器连问都不用问。于是「容器会增长」不再是回收器的负载，而是容器的一次指针推进。

#### 19.20.2 机制

| 位置 | 内容 |
| --- | --- |
| 进程级 VA 竞技场 | `EXTC_BUF_ARENAS = 8` 个、每个 `64 GiB`（`QSHIFT = 21`，量子 2 MiB，`ORD = 15`），`mmap` 一次 `PROT_READ|PROT_WRITE`、懒创建；按 2 MiB 对齐发放，`carve` 实测 **7 ns**（对照一次 `mmap` 1,897 ns） |
| buddy 分配链 | 每量子 1 bit 起点表 + 字节序表 + 双向自由链（`flPush`/`flPop`）：切块只写起点位、合并 O(1) 摘链；第一版涂 interior 标记并线性扫链，10 万次短命 `buf` 要 **0.76 s** |
| `extc_buf_reserve(bytes)` | 抬到 4 KiB、页取整，从竞技场 carve；竞技场拼不出连续块时退回独立 `mmap`。返回**基址**（对齐到 2 MiB） |
| `extc_buf_release(at, bytes)` | 还给竞技场（不进内核）：容量回到空闲表，页留在大页里 ⇒ 短命 `buf` 的稳态 RSS 是**常数**（100 万次 1,868 kB） |
| `extc_buf_discard(at, bytes)` | `MADV_DONTNEED`：只用于 `shrink` 与搬家时「拷一段丢一段」，每 2 MiB 一块、向下取整到页 |
| `extc_buf_span(at)` | 这块预留**实际**多大（量子取整）；账本必须报它，不能报请求字节（512 B 的请求实际握 2 MiB） |
| `extc_buf_live()` | 进程当前预留总量（`buf::bufLive()`），`buf_fuzz` 用它做账本守恒 |
| 视图两个发射原语 | `extc_sizeOf<T>()`、`extc_viewOfT<T>(p, n)` —— 语言没有 `sizeof` 与指针算术，类型化视图必须由发射器给 |

不变量（`docs/design/BUF-INVARIANT.md` §1）是**一片预留 + 一条地址公式**：`len ≤ cap ≤ max`，元素地址恒为 `base + i·sizeof(T)`；预留之内增长只推 `cap`（地址不动、零拷贝），只有预留用尽才搬家（`epoch + 1`，旧视图失效）。四个动作 `reserve / grow / shrink / release` 都作用在这一片地址上：

- `shrink(k)`：地址不动，把 `[k, cap)` 的页丢掉，`cap = k`；
- `release()`：整片还给竞技场（页不还内核），`@drop` 再发一次是空操作；
- 搬家用「拷一段、丢一段」：每 2 MiB 拷完就把源那一段 `discard` ⇒ 瞬时峰值是**一份 + 一块**，不是旧代 + 新代。

#### 19.20.3 契约

- **内存界**：一个 `buf` 的预留是 `span` 字节（量子取整，最小 2 MiB）；进程预留总量 = Σ `span`，是**地址空间**而不是常驻集。常驻集上界 = 已写的页（大页尾页放大 ≤ 2 MiB / 被触区间）。`release` 只把容量还给竞技场；要还内存给内核是 `shrink` / `discard`。
- **停顿界**：`carve` 在锁内是 O(ORD) = O(1)（15 次位运算），不进回收器的任何预算；`buf` 完全不出现在回收器的路径上 —— **只用 `buf` 的程序连回收运行期都不发射**（`#if defined(EXTC_RC_TIER)` 之外它编成空操作，T0 程序不会被拖进回收器）。
- **拷贝是一次真正的 O(len) 动作，但它在 mutator 侧**：`reserve` 在预留之内不拷；预留用尽那一次搬家是 O(len) 字节复制（与 `std::vector` 的 realloc 同一个量级），它不进任何停顿预算，也不改变回收器的界。契约说的是「回收器的每一步有界」，不是「程序不会做长事」。
- **没有静默的失败**：`maxCapacity` 可读；`reserve(n)` 超上限（1 GiB 预留、117 万元素级的默认上限）返回失败而不 trap。

#### 19.20.4 实测（`bench/buf/run.sh`，REPS = 7，min-of-7，`taskset -c 6`，同形状校验和逐字相同）

| 形状 | `buf<T>` | `std::vector` | `vector` + `reserve` | 常驻集（buf / vec） |
| --- | --- | --- | --- | --- |
| S1 从 4 长到 8e6 个 `i64` | **0.020 s**，搬家 2 次 | 0.059 s，24 次 | 0.030 s，1 次 | 65,452 / 69,196 kB |
| S2 100 万次短命 `buf`（512 B） | **0.076 s** | 0.096 s | — | **1,868** / 4,288 kB |
| S3 装满 → `shrink(4096)` → 长回 | **0.028 s** | 0.085 s（`shrink_to_fit` 走重新分配，12 次） | — | 65,452 / 69,192 kB |
| S4 24 B 结构体 2e6 个 | **0.017 s**，搬家 2 次 | 0.049 s，22 次 | — | 49,076 / 52,968 kB |
| S5 16e6 `i64` 顺序 + 随机索引扫 | 0.086 s | 0.116 s | — | 128,940 / 128,904 kB |

S1 的快不是「拷贝更便宜」，是**不拷**：`buf` 一次 `reserve` 拿够地址，此后 8e6 次 `push` 只推 `cap`；`vector` 的 24 次搬家是 24 次真拷贝。S3 里 `buf` 只付「丢页 + 再缺页」的价（`shrink` < 1 ms，长回 4 ms ≈ 16 GB/s，大页路径下 0.5 ns/元素），`vector` 的 `shrink_to_fit` 则重新分配。S2 的常驻集差一半是「还容量不还页 + 拷一段丢一段」的直接后果。

账本口径的判据（`tests/stl`，失败 0）：`buf_fuzz` 3,000 轮、三个数量级、非 LIFO 释放顺序，每轮核 `buf::bufLive() − base == Σ reservedBytes()` ⇒ `rounds=3000 bad=0 worst=0 leak=0`；`buf_huge` 断言 1 GiB 预留只手写 10 个元素时 `max=134217728 reserved=1073741824 live=1073741824` 而 RSS 4,016 kB。

#### 19.20.5 两条被撞出来的边界

- **`release` 不把存储清零**：块回到竞技场时页还在、字节还是上一轮的。于是「往复用的存储里写引用」这件事**不能读槽里的旧值**（§19.21）—— 这一条把一个原本以为是「覆盖写语义」的问题变成了「表的键必须取槽地址」的问题。
- **发射器的 arena 前置会挡住内联**：每个函数前都发 `extc_arena __extc_a[3] = {0};` 与两次 `extc_arena_destroy`，即使该函数一次都不分配（`buf::push` 就是），C 编译器因此拒绝把它内联进调用者的循环 —— 同一个循环 2.6 ns/push 变 10 ns/push（S3 因此假红 1.77×）。修法：库的热成员加 `@inline`（10 处）。这是**发射器的债**，记在 `BUF-INVARIANT.md` §8。

完整语义与可运行示例见 `docs/design/BUF-INVARIANT.md`，实现与价目见 [`GC-ROOFLINE.md`](GC-ROOFLINE.md) §6.25。
### 19.21 引用写进非 rc 存储：外部边表（v37 新增，R4b）

#### 19.21.1 这一份模型里的位置

§18 的每条公式都假设「引用在 rc 槽里」：槽有宿主 region、有占用位、有入度。但语言允许把引用写进回收器不认识的地方 —— 桩上局部量、`new T[n]` 的视图、place 载荷区（`dyn` 的值）、`buf<T>` 的预留、`C` 缓冲区。这些槽没有寿命可跟踪，**可这条边还在**：值仍然是本运行期管的活对象，只要还有人指着它，它就不能被收。

§19.13 的 64 B 组位图解决的是「大槽内部的引用域」，入口仍然绑在 slab + slot 上。这一节解决最后一段：**槽根本不在任何 slab 里**。

#### 19.21.2 机制：把边记在槽上，而不是把值钉成根

| 位置 | 内容 |
| --- | --- |
| 表结构 | 开放寻址哈希，条目 `{ void **slot; void *v; ExtcRcRegion *rg; }` = **24 B**；键 = **槽地址**，每外部槽一条；散列 `(((uintptr_t)slot >> 3) * 0x9E3779B97F4A7C15ull) >> 32`，装载 ≤ 1/2 翻倍重散，删除走 Knuth 回移 |
| `extc_rc_pinPut(slot, v, &old)` | 登记或改写这个槽的边；`oldOut` 给出**表里原本的值**；`v` 不是本运行期的活槽（外来指针 / 0）⇒ 把这个槽的条目**清掉** |
| `extc_rc_storeRefUnknown(slot, v)` | `shade(v)` → `pinPut` → `*slot = v` → `if (old) { shade(old); dec(old); }` → `inc(v)`；只在表扩容失败（`calloc` 失败）时给目标区域打 `noSweep` |
| `extc_rc_pinSeed(r)` | 起轮：遍历表，`rg == r` 的条目 `markPush` 成种子（`noinline`：内联会让 owe 形状每分配 +2 Ir） |
| `extc_rc_pinDropRange(lo, hi)` | 结构动作：槽地址落在 `[lo, hi)` 的条目全部 `dec` + 删除；一次表扫描，不是每元素一次 |
| `extc_rc_pinDropRegion(r)` | `regionFree` 时清掉属于该区域的死指针（对象已死，留着只会让标记走废值） |
| `extc_rc_storeStructCons` | 保守整值拷贝不再把整区置 `noSweep`，改成**逐字** `pinPut`（栈地址 / 池块地址的条目随槽被重写而改正） |
| buf 侧的桥 | `extc_buf_releaseRefs(at, esz, from, to)` 在 `#if defined(EXTC_RC_TIER)` 里调 `pinDropRange`，否则编成空操作；`buf.extc` 的私有成员 `dropRefs(from, to)` 是 `shrink` / `clear` / `release` / 搬家四个结构动作唯一的调用点 |

两个决定是有界性的全部来源：

1. **键取槽址、不取值。** 同一个槽被反复写只改一条；槽被复用不会留下上一位主人的条目。值键的表只能在「同槽覆盖」时撤边，而 `shrink` / `release` 丢掉的那几个元素没有任何槽能撤 —— 条目随轮数涨（这正是 L10-a/G15 的机制）。
2. **旧值只从表里取，绝不读槽里的字节。** `buf` 的存储从竞技场复用回来是**不清零**的（§19.20.5），槽里可能是上一轮的陈旧指针；按陈旧字 `dec` 会撤掉别人还持有的边 —— 那是提前回收，方向错。没有条目就什么都不撤。

#### 19.21.3 契约

- **内存界**：表大小 ≤ `24 B × 外部槽条目数`，条目数 = 「在容器/桩上写过的不同槽地址」减去已撤销的。界里**没有轮数**：同一槽写一百万次还是一条。撤销之后条目立刻消失（`pinDropRange`），表会收缩。
- **停顿界**：登记/撤销一条摊 O(1)（负载因子 ≤ 1/2）；`pinDropRange` 是**一次表扫描**，但它发生在 mutator 的结构动作里（`release` / `shrink` / `clear`），不进停顿预算。起轮时的 `pinSeed` 在回收器里，代价 O(表容量)，这一项**还没有标定**（见 19.21.5）。
- **语义**：`pop()` **不撤边** —— 取出的元素由调用方持有，那条边仍然成立；该槽下一次被写时表项自动改正。这是刻意的取舍，写在 `BUF-INVARIANT.md` §7。
- **T0 不付钱**：只用 `buf<i64>` 的程序一条都不进表（不含引用元素时 `extc_sizeOf` 之外零成本），也没有回收运行期。

#### 19.21.4 实测

形状 `tests/gc-p0/src/leak_buf_ref.extc`（G21，硬判据 ≤ 1.20×）：每轮 `buf<mut ref node>::withCap(4)`、push 四个 `new node`、`shrink(2)`、再 push 一个、`release()`。

| 臂 | 50k 轮 peak RSS | 800k 轮 peak RSS | 比 | 判决 |
| --- | --- | --- | --- | --- |
| 本轮（槽址键 + 区间撤销） | 3,764 kB | 3,764 kB | **1.000×** | PASS |
| 基线（值键 pin 表，无撤销） | 24,144 kB | **363,008 kB** | **15.035×** | RED |
| 对照臂：生成 C 里让 `pinDropRange` 立刻返回 | 3,764 kB | 3,764 kB | **1.000×** | 平 |

第三行是机制归因，也是这一节最重要的一条负结果：**上界来自键的选择，不来自区间撤销**。把撤销整个关掉 R21 仍然平 —— 因为同一槽写一百万次还是一条。区间撤销买的是**及时性**：`release` 之后条目立刻消失、表收缩、那些对象下一轮就能收（`pin` 立即回落），而不是「上界更小」。写文档、写论文都不能把撤销说成有界性的来源。

单测 `tests/reclaim`（242/0）钉住更细的语义：`t_pin`（不是活槽不登记 / 两条槽指着同值 ⇒ 两条条目 / 撤一条仍在 / 撤完可收 / 覆盖撤旧边 / 区域死时清死指针且别的区域不受牵连）、`t_pinRange`（区间内全撤 / 区间外不动 / 表长正好减槽数 / 撤销后同槽重新登记不凭空减计数）、`t_noSweep`（保守整值存储逐字登记、区域**照常清扫**）。跳过表 95 类标记、未标记早退 0 处。

缓冲区容器（`varArray<mut ref T>` / `buf<mut ref T>`，G21 形状）与 `dyn` 载荷一样反查不到宿主，落进的也是同一条 `extc_rc_storeRefUnknown`。键换成槽地址之后，槽复用、地址不变 ⇒ 表项被改正、不随轮数长：50k 与 800k 两档的峰值都是 **1.000×**（G21，3,764 kB）。G15 那个池块形状随池基质退场删除，覆盖由 G21 继承。
#### 19.21.5 边界与还欠的账

- **区域外存储有界、但撤销要看谁负责**：有界来自「键取槽地址」，不是来自区间撤销 —— 条目要到该槽被重写、区间撤销或区域死时才消失。容器现在的结构动作都接上了 `dropRefs`（`buf` 的 `release` / `shrink` / `clear` / `moveTo`，`block` 的 `dropRefs`），`pop` 不撤（元素由调用方持有）。
- **`pinSeed` 的 O(表容量) 未标定**：表大时起轮那一下是回收器里的真实开销，它目前不在 `P_step` 的任何一项里。G21 的表只有 16 条，看不出这一项；要一条「几十万个外部槽」的形状才量得出来。
- **`pop` 不撤边**：丢掉的槽位仍留一条条目，直到该槽被重写或区间撤销。上界因此是「峰值同时在用的槽集合」而不是「当前活元素数」—— 方向仍是安全（多留），但要在 `BUF-INVARIANT.md` 里说清。
- **多线程**：表有锁（`pinLockAcq`），登记与撤销都在锁内；种子扫描也持锁。分片（per-worker 表）是 §S 里并发那一栏的事，不在本节。

机制与价格表见 [`GC-ROOFLINE.md`](GC-ROOFLINE.md) §6.26；跳过表的每一行在 `docs/design/RECLAMATION.md`。
### 19.22 池运行期退场：place 载荷区（v38 新增）

- 运行期从 `pools.c`（126 行）换成 `places.c`（315 行）：place 槽表 + 每槽一条 `malloc` chunk 链 + `dyn` 槽表；生成物里池对象符号（`extc_pool_zone*` / `extc_pool_new*` / `take` / `give` / `resize` / `kind` / `generation` / `drop` / `reset`）归零（`par` 的线程池符号是另一套，不在此列）。
- place 栈动作 O(1) 均摊：`extc_place_enter` 取一个槽（free list + 摊还扩容）、`extc_place_leaveTo(mark)` 沿 `parent` 链弹栈；每弹掉一个 place 释放它自己的整条 chunk 链。**这是本轮唯一一条工作量与历史分配量成比例的路径**：工作量 ∝ 该 place 的 chunk 数（≈ 累计分配字节 / 末块大小），不 ∝ 对象数、不 ∝ 槽数；而且它发生在**作用域出口**（块退出 / 任务结束），与「出口时堆上还有多少活对象」无关 —— 它不属于回收器步。
- `extc_place_alloc` 单次 O(1)：16 字节对齐 bump；chunk 用尽时一次 `calloc`（请求 ≤ 256 KiB 时取当前块大小的 2 倍，封顶 256 KiB；请求更大时按请求字节单独给一块）⇒ 单个大对象的分配仍是停顿的界（与 §19.11 的巨块契约同一条）。
- `dyn` 侧：`extc_dyn_put`（:235）O(1)，只有「free list 空且槽表满」时才跑一次 `extc_dynSweep`（O(槽数)，:245）；取用 `extc_dyn_slot`（:273）是六次比较 O(1)，从不读已释放存储。
- 门禁：`tests/dyn/run.sh` 27/0、`tools/check_dyn_place.py --selftest` 9 条注入全红过、`tools/check_concurrency_guards.py` J1/J2/J3 通过、`./tests/run.sh` 377/0。
- 存量契约不变：§19.11 的四个结构性界与 §19.12 的冷档都不动；place 层没有新增计时器、没有新增按墙钟切的工作量。
### 19.23 池原语族退场与跨挂起的类型级判据（v39 新增）

- **删掉的编译器特例**：`poolSlice` / `poolSliceRaw` / `poolResize` / `poolResizeRaw` / `poolGive` 的名字表（原 `src/types/typelayer.h` 的 `isPoolPrimitiveName`）、检查器分支（原 `checkPoolPrim`）与 `alloc<T>`/池原语共用的发射分支都已删除；`copyInto` 留下，收成 `isCopyIntoName`（名字表只剩这一项）。现在手写那五个名字在**检查期**就报未声明的泛型调用，不会生成引用已删运行期的 C。
- **两条陈旧推断**：①`calleeCreatesPool` 对未解析 callee 原本保守返回 true；池运行期退场后，未解析 callee 只剩 `extern!` C helper、builtin 原语、类型参数上的关联函数，没有一种能建 place ⇒ 改成返回 false。②`exprMakesPool` 的 `EX_NEW` 分支（`planNewAsBuffer`）删除 —— 那块存储是 buf 预留，不是 place 站点。后果：容器/字符串程序不再被拖进 place 运行期（`tests/stl/buf.extc` 17→0 处 `extc_place_`、`bench/string_vs_cpp/s.extc` 67→0、生成物 444,575→433,046 B）。**place 站点只剩两类**：`dyn` 值（`EX_DYN` 自答 true）与任务表（独立扫描）；语料 1,676 个程序里发射 `extc_place_enter` 的 59 个全部属于这两类。
- **跨 `yield` 的豁免表**（`coroCheckDeferred`）原先把「能不能活过挂起」交给 `exprMakesPool(init)` 推断 —— 容器跨挂起之所以合法，靠的正是上面那条陈旧推断碰巧为真。现在换成类型级判据 `bool typeOwnsStorage(TypeTable *tt, Type *t)`（`src/check/check_lookup.c`）：`@buffer` 类型（`buf<T>`）拥有自己的 extent ⇒ true；其余按字段递归（`vector<T>` 经 `b: buf<T>` 命中）；`TY_REF` / `TY_DYN` / `TY_PARAM` / 视图类型一律 false；`TY_ARRAY` 递归、`TY_ENUM` 对全部 payload 递归；深度超限保守 false。
- **能力边界（方向收紧，必须记住）**：`string`（字段是 `?mut slice<u8>`）、`block<T>` 基容器（`hashMap` / `map` / `set` / `linMap` / `linSet`）、`varArray<T>` 不再允许跨挂起 —— 它们的存储其实自有，但类型层表达不出（`?slice` 是有条件拥有，`block<T>` 未标 `@buffer`）。要放开需给它们类型级「拥有存储」标记，属后续工作。
- **协程不再需要 place 时的形态**：`planCoroNeedsZone(cf) = planMakesPool(cf) || planCoroBoxed(cf)`；无 place ⇒ 发射 `$step`（帧是调用者栈上的普通 struct，不登记任务），有 place ⇒ `$next` + `extc_task_alloc`。因此 `tests/coro/coro_tasks.extc` 与 `examples/task-place-release.extc`改为在协程体里持一个 `dyn Tag` 载荷，判据数字（退出码 10 / `a=3 b=4 live=0`）不变。
- **门禁**：`./tests/run.sh` 377/0、`tests/coro` 37/0、`tests/dyn` 27/0、`tests/stl`/`tests/zone`/`tests/dense`/`tests/warnings` 全绿；`reclaim-perf/gate.sh` 87,678,563 Ir（与本轮之前逐字同值 ⇒ 回收路径未动）；`emitted_identity --set core` 110 行 changed（全部是原先被 place 拖入的程序）重冻结后逐字节相同；`tools/emitted_identity.py` / `check_no_history` / `check_layering` / `source_split --check` 全绿。
- 存量契约不变：place 载荷区的时间/空间界见 §19.22；回收器的四个结构性界（§19.11）与冷/暖档（§19.12）本轮未被触碰。
### 19.24 `@place` / `@poolObject` 语法面退场（v40 新增）

- **删掉的东西**：`@place` 与旧拼写 `@poolObject` 两种注解、`StructDef.poolObject` 字段（`src/ast/ast.h`）、parser 的解析分支与三条带注解名的报错文案（顶层未知注解清单、impl / trait 的注解适用条件、enum 上的 `@place` 报错）。现在写 `@place` 报 `unknown top-level annotation`，与任何未实现注解同路。
- **视图悬垂门的判据收成一条**：`ckStorageCanMove`（`src/check/check_stmt.c`）原来是 `sd->poolObject || typeHasDrop(c, t, 0)`，现在只有 `typeHasDrop`。这没有放宽：语料里 `vector<T>`（字段 `b: buf<T>`）、`string`、`buf<T>`、`block<T>` 与 `map` / `hashMap` 都带 `@drop` 或经字段递归命中它；删掉的只是那条再没有声明者的捷径。
- **两处 by-value 深度放宽删除**（`src/check/check_escape.c`）：`borrowArgDepth` 里「接收者是 `@place` 类型 ⇒ 元素寿命等于 slot 深度」的早返回，与 `targetDepthPure` 的 `EX_METHOD` 分支里同样的 max 提升。留下的规则是字段表记录的槽深度（方向保守：取更大的深度）。`src/check/check_top.c` 的 `calleeMakesPool` 也删掉「`@poolObject` 声明胜过推断」那一句，只剩 `planMakesPool(f)` 与 `makesPoolAny` 的关联函数放宽。
- **语料零用法**：删除前在 `tests/`、`examples/`、`stdlib/`、`bench/` 里 grep 两个注解名只命中注释（`tests/zone/grow_view.extc` 头注释与 `bench/app` 的三处说明），没有程序被改写；`tests/zone/grow_view.extc` 的判据（扩容后同层取视图仍是 100）继续成立，因为该门的判据现在是 `varArray` 的 `@drop`（经 `buf<T>` 字段命中）。
- **能力不变**：跨挂起的豁免仍是 §19.23 的 `typeOwnsStorage`；place 站点仍只有 `dyn` 值与任务表；生成 C 的 place 计数（`tests/dyn/dyn_stored_call.extc` 19 处、`bench/app/route.extc` 0 处）与 v39 相同。
- **门禁**：`./tests/run.sh` 377/0、`tests/coro` 37/0、`tests/dyn` 27/0，`tests/stl` / `tests/zone` / `tests/dense` / `tests/warnings`（192 正例零警告）全绿；`tests/reclaim`、`tests/reclaim-perf/gate.sh`（87,678,563 Ir ≤ 88,029,115，与 v39 逐字同值 ⇒ 回收路径未动）、`tests/reclaim-gaps/gate-release.sh`、`tests/gc-p0/run.sh all`、`tests/gc-torture` 全绿；`tools/emitted_identity.py --check --set core` 376 行逐字节相同（没有程序用注解 ⇒ 生成 C 不变，无需重冻结）；`python3 tools/source_split.py --check`、`tools/check_dyn_place.py --selftest`、`tools/check_concurrency_guards.py --list`、`tools/check_no_history.py`（新增 0）全绿。

### 19.25 hashMap 控制字节按字打包（v41 新增）

**为什么**：逐字节控制字节列在 17/20 装载率下，一次不命中查询的期望探测数是 22.7，J1（h1 峰值 ≤85 MB）与 J2（h5 ≤1.3× 手写 flat 表）在同一条权衡的两端：α=0.82 时峰值 85,880 kB 贴边但 h5 仍 1.71×。这一节记录把一次探测覆盖 8 个桶之后的形态与代价。

- **布局**：`tag: block<u64>`，字数 `(cap + 7) / 8`，桶 `j` 是第 `j / 8` 字的第 `j % 8` 个字节（按位序定义，与端序无关）。`cap` 不必是 8 的倍数：头字用 `m = ones << u64(off * 8)`、尾字再 `m &= ones >> u64((8 - (hi - base)) * 8)` 把区间外的字节裁掉。
- **两个一次减法**：空桶检测 `emp = (x ^ ones) & 0x8080...80`（控制字节最高位恒 1、空桶是 0，所以 `x ^ ones` 只在空桶字节的最高位置 1）；候选检测 `zeroBytes(x ^ bc)`（`bc` 是同值字节广播）。各自 `ctz` 后 `>> 3` 得到字节偏移，谁在前谁就是答案。
- **`@inline` 不是优化而是前提**：extC 的 `@inline` 是源码级内联；`probeRange` / `slotRange` / `findH` / `find` / `slotFor` 任一不开，h5 就回到 0.65×（函数调用与跨函数传参吃掉字级扫描的收益）。
- **两次否证**：①「先按区间掩码/移位再取最低置位」会因区间头之外的低位借位把高位非零字节误报成零字节 ⇒ 空桶判错、覆盖活键（正确性错，不是性能问题）；②保留头尾逐桶路径时每步要一次有符号除法（`i / 8`），比字级扫描还贵，per-op 指令数没有下降。
- **放弃的一条**：让 `probeRange` 顺带返回第一个空桶号供 `put` 合成一次探测。h6 0.81×、h4 1.10×，但 h3_churn 出现重复键（`n` 多 1），8M 次操作约漏判一次「键已存在」，根因未定位（疑在 `-1 - 桶号` 与 `-2` 的解码边缘）⇒ `put` 保持两次探测。
- **实测（同轮 `run-flat.sh --reps 7`，交错 min-of-7）**：h5 0.363 s / flat 0.318 = 1.14×、h1 0.315 / 0.250 = 1.26×、h6 0.108 / 0.151 = 0.72×，峰值 81,780 kB 不变 ⇒ **J1 与 J2 同时 PASS**（逐字节时代分别是 PASS / 2.17× FAIL）。
- **代价**：相对逐字节版本 h2 +6%（每次 `put` 在增长路径上多一遍字级探测）、h4 +16%（`liveAt` 没被 GCC 内联，callgrind 里成了独立函数占 103.5M Ir）；绝对量 +21 ms / +8 ms，换 h5 的 −240 ms。四臂表与归因见 `bench/containers/RESULTS-4WAY.md` §4.3 与 `docs/design/HASHMAP.md` §9。
## §20 与仓库既有文档的不一致清单（口径必须按本节改）

完整 90 条对账在 `docs/analysis/DOC-AUDIT.md`；这里只列"会误导读者/会误导优化决策"的 10 条。

| # | 旧文档断言 | 实际（源码/实测） | 出处 |
|---|---|---|---|
| 1 | 默认 = "无计数 + 区域清扫" 单档 | 三档（§1.5）：无回收档 / 事件档 / 清扫档 | `codegen.c:16131-16139`、`:14077-14159` |
| 2 | "无写屏障 / 无原子 / 无 STW" | 每次引用赋值都发射 `storeRef*`（§6）；运行时含自旋锁与 `__atomic`；"无 STW"成立 | `codegen.c:8914-9084`；`reclaim.c:412-425, 1985-1991, 926-938` |
| 3 | "三种存储"（漏回收区 slab） | 五种基质（§1 表 1-1）；`new T` 默认走 `extc_rc_alloc` | `codegen.c:4686/4716`、`:4721/4691-4692` |
| 4 | "无共享大堆 / 无碎片" | slab 来自 `malloc`，`regionFree` 只 `free` 不 `munmap`；registry 旧代永不 free | `reclaim.c:2769-2862, 390-393, 450-451` |
| 5 | "trap 会跑 `drop` 钩子" | `ST_TRAP` 不发 `drop`（块内 `drop` 实测不跑） | `codegen.c:8751-8779` |
| 6 | "globals 退出 drop 未做" | 已有 `atexit(extc_drop_globals)` | `codegen.c:10099-10104, 13867-13875` |
| 7 | 性能数字多口径（132.8/129.5/126/129 → 70/64 MB 等） | 唯一可核：`86,174,829 → 82,037,532 Ir`；其余本次未复跑 | `tests/reclaim-perf/gate.sh:88`；MEASUREMENTS §9 |
| 8 | 计数层 "+100%~+455%" | 本机实测 **+76%…+301%**（pb 1.76–1.81×、pe 2.14×、pc 3.89–4.01×） | MEASUREMENTS 表 A2 |
| 9 | "分片降低停顿"（计划/未验证） | 单线程环负载：×10 时间、RSS ×2120、完全不回收 | MEASUREMENTS 表 A2 |
| 10 | `src/back/plan.c` 存在 | 不存在；pool-site 判定在 `src/check/check_escape.c`（`lowerPoolSite:1237`、`promoteInto2:1738`）、`src/check/check_top.c`（`calleeMakesPool:1906`、`makesPool` 闭包 9782-9880） | DOC-AUDIT §2 |
| 11 | 门禁读数 = 回收次数（`sweeps.sh` 数 `[rccount] collect`） | 增量路径走 `extc_rc_step`，`collect` 恒 0；`gate-acyclic` 12 通过 / 3 失败；且无停顿直方图 ⇒ p50/p99 结构性不可测 | `tests/reclaim-acyclic/sweeps.sh`；MEAS-T1 §7.2/§9.2 |
| 12 | 「事件档（计数）一个形状都不能赢」 | 只对小对象成立：16 B +150%、456 B 打平、32 KB −66%；且档 1 只对「无出边的死对象」正确（chain2 强制档 1 漏 2M 子结点） | MEAS-T1 §3/§5.3 |
| 13 | —（源码 bug，非文档；**P0-6 已修**） | 修复前：`extc_task_rootPush`/`rootPop`（`coroutine.c:69`、`:74`）全仓无发射器 ⇒ `extc_taskRootN` 恒 0 ⇒ `extc_task_collect` 的“按已登记根可达”判活恒为否，任何一次 `syspool::collect()` 都会把所有 `depth==0`（= 全部任务根）的活任务判不可达并 `extc_task_end` + `zoneFreeRoot`。**修复后（FREEZE v4）**：编译器对存储为普通局部变量的 boxed handle 发射 push/pop——4 个发射点 `codegen.c:3042`（`d.run` 域任务 id）/`:7606`（局部 boxed handle）/`:7625`（栈帧 spawn 的 arena id）/`:7937`（拷贝到另一局部），pop 在块尾/return 配平（`cgTaskRootPopAt` `:6252`、`cgTaskRootPopsForReturn` `:6275`；调用点 `:5638`/`:5655`/`:6987`/`:8835`）。登记只覆盖“槽就是最后属主”的情形，句柄被塞进聚合体/容器时不登记（只漏放，不早放）。**残余风险**：`extc_task_collect` 在 `src/` 内仍无调用者（唯一入口是 `stdlib/std/sys/pool.extc:38` 的 `extern extc_task_collect()` 与 `:42-44` 的 `collect()` 包装），所以“调用者断言此刻没有可驱动句柄”的契约仍由程序作者承担；G9（`tests/zone/collect_with_live_task.extc`）是回归守护 | Lead 复核 + P0-6 + G9 |
| 14 | 断言 sweep 次数与规模无关（小常数） | 内存墙契约下改为 **O(log N)**；判据改为峰值 ≤ `M_max` + 缓冲 | `tests/reclaim/driver.c`（本轮改写）、§18.5 |
| 15 | `gate-release` A: sweeps≤1 / D: release_slots>0 | v5 曾 2/4：夹紧后每轮 sweep 把 `visitedBytes` 推到 `allocBytes` ⇒ `budgetOk` 信用被吃光；**v6 的 P 配额（§18.6）改变了信用节奏，已恢复 4/4**（Lead 用 `build-lead6` 实测：A 1/1、B 1.00×、C 逐字相同、D releases 19/79） | `tests/reclaim-gaps/gate-release.sh` |
| 16 | —（**已关闭**：R4b 槽地址键 + 池基质退场） | 旧缺陷：pool 槽里的引用赋值走 `storeRefSlot→storeRefUnknown→pinAdd` ⇒ 永久 pin，`pool_give` 不撤 pin ⇒ 三档都不回收（T2 50k=12.4 MB→800k=186.0 MB ≈232 B/轮，T1 11.9→171.7 MB）。修法：pin 表改槽地址键 + `extc_rc_pinDropRange`（§19.21），pool 基质退场（§19.22）；G15 由 XFAIL 升为硬判据（1.000× 通过） | `MEASUREMENTS-GC-P0.md`、§19.21、§19.22 |
| 17 | —（L10-b / L10-c） | L10-b：目的 view 由 arena 分配 ⇒ `storeRefSlot` 回退 `noSweep` 永久（删回退后 800k 恒 2832 kB）⇒ P0-9；L10-c：rc 基址但目标反复覆盖 ⇒ `rsInc` 边不撤销（6660→93544 kB）⇒ P0-10 | `MEASUREMENTS-GC-P0.md` |
| 18 | —（P0-7 / P0-7b 的边界） | 参数槽被覆盖时只 `+1` 不减（insert-only），**只在强制事件档 + 逃逸**时线性泄漏 ~20 B/轮；默认 T2/T3 由 sweep 兜住（边界注释 `codegen.c:8992`）。bulk copy 已改为按引用 word 记账（P0-7b，`reclaim.c` 新增 `extc_rc_storeStructCopy`） | MEAS-GC-P0 的 G12/G13 |
| 19 | —（P1 轮：缩小 chunk 不是停顿尾部的杠杆） | 段步 `max` 尾部 **58–871 µs 与 `SCAN_CHUNK` 无关**（128/256/8192 都有）；缩小 chunk 的代价是内存：pd_bigobj 200 轮峰值 8192→**26.6 MB**、512→84.6、256→126–134、128→215–240 MB（配额窗口花不完 ⇒ 标记落后）⇒ **保持 8192 + deadline 800** | docs/analysis/P1-RQ3-CHUNK.md、§19.5 |
| 20 | —（P1 轮 `regionFree` 两阶段；v8 closing queue） | pd_bigobj 200 的 `[chan] regionfree` 18.6–22.7 ms → 493.2 µs → **0.4 µs**；dense **1,365.5 µs → 1.6 µs**；**pb_escape_grow 100M 13.2–14.8 ms → 1.7 µs** ⇒ G3b 三通道闭合 | §19.5；`P1-CLOSING-QUEUE.md`；`MEASUREMENTS-P1.md` §3 |
| 22 | —（P1 轮：尾部归因） | 58–871 µs 的超长步不是 GC 工作量：① 环境抢占（对照复现 870–2345 µs，且 cpu 计入 70–80%）、② `chase` 冷页缺页（`minflt` 16–49）、③ 256-pop 真工作（0.6–1.9 µs/pop）；`chunk` 压不低该尾部；`chunk=8192` 一段典型 4–16 µs ⇒ 5 µs 只能靠按字数在段内停 | §19.5.3；`P1-STEP-TAIL.md` |
| 21 | —（P1 轮：惰性 freelist 与卷计数器） | P0-B 惰性 freelist **默认关**（`LAZY_FREE=0`：收益不可复现——promote 0.996、sizeratio 1.033，且 Ir 更差）；slab 级省略改为 **O(1) 卷计数器**（`nLive`/`nMark`：cycleratio Ir −3.1%、墙钟 15.51→15.38 ms、切片 1 221→255）；`EXTC_RC_ELIDE_VERIFY=1` 自检（默认档/pd/cycleratio/bench v2 四形状 `markbegin-mismatch` 全 0）抓到 force 条目 `nMark` 双计真 bug 并修 | `reclaim.c:1085`、`:1074`；§19.5.2 |
| 21 | —（P1 轮：惰性 freelist 与卷计数器） | P0-B 惰性 freelist **默认关**（`LAZY_FREE=0`：收益不可复现——promote 0.996、sizeratio 1.033，且 Ir 更差）；slab 级省略改为 **O(1) 卷计数器**（`nLive`/`nMark`：cycleratio Ir −3.1%、墙钟 15.51→15.38 ms、切片 1 221→255）；`EXTC_RC_ELIDE_VERIFY=1` 自检（默认档/pd/cycleratio/bench v2 四形状 `markbegin-mismatch` 全 0）抓到 force 条目 `nMark` 双计真 bug 并修 | `reclaim.c:1085`、`:1074`；§19.5.2 |


**数字引用规范（建议此后所有性能数字都用四列）**：数值 | 构建（HEAD + `build/extc` md5） | 档位（0/1/2 + 全部开关） | 量具（VmHWM / `[pause]` / `[step]` / Ir）。本次 MEASUREMENTS 的 header 就是按这个格式写的。

---

## §21 优化旋钮：现状值，以及每个旋钮影响哪条界

| 旋钮 | 现值 | 影响的界 | 备注 |
|---|---|---|---|
| 触发倍率 `r`（`thr = max(r·L, floor)·scale`，现 `r=2`） | 2 | §18.2 内存界、§19 触发频率 | 优化方案的核心决策变量 |
| `EXTC_RC_FLOOR` | 1 MiB | §18.2 下限、§19 触发频率 | 分片时 ÷N（`codegen.c:4221-4244`）；降档实测：**256 KiB** ⇒ sizeratio RSS −35%（3,244→2,088 kB）/墙钟 +15%、cycleratio RSS −32% / 墙钟 +4%，promote 无关（活集 > floor）；64 KiB 只多省 0.2–0.4 MB 但墙钟 +35–38% ⇒ 256 KiB 是拐点（`MEASUREMENTS-V2-AFTER.md` §12.3） |
| `scale` 初值（0=按 1） / `SCALE_MUL 4` / `SCALE_MAX 2^40` | 见 §17.2 | §18.3-4 无界条件 | "空扫即放弃"策略 |
| walk 批预算 `MARK_BATCH_MAX 256`（压力档 ×4 ⇒ 1024）/ 基步 8 / assist 512 | 见 §17.2 | §19.4 | 已不是 P 的主项；压力档上界只近墙出现 |
| 清扫 `SWEEP_BUDGET 8` words | 8 | §18.3-1 清扫带宽、§19.4 | 每分配最多一步 |
| `SCAN_CHUNK` words | **8192**（64 KiB，由 65536 下调） | §19.4 **主导项** | 调小直接降 P：pd 平台 max 164–269 µs → **78–98 µs**（§19.2）；**但 P1 轮证明不能再小**：128/256 换来的段步 p99.9 1.65/1.84 µs 要付出 pd 峰值 215–240/126–134 MB 的代价（§19.5） |
| 触发线夹紧 `EXTC_RC_M_MAX`（隐式 3·live） | 未设 | §18.5 内存界、§19 触发频率 | **新增**：本轮契约，直接决定峰值；显式给小值 ⇒ 峰值降、sweep 次数升 |
| `EXTC_RC_SWEEP_PRESSURE_MUL` | 4（1..16） | §19.4、§18.5 | **新增**：近墙时乘每步预算（乘工作量，不是乘触发频率）；`markAdvance` 的 walk 批上限随之 256→1024（`reclaim.c:1394`），`[work] walk_cap` 报运行期实际值 |
| `EXTC_RC_RELEASE_BUDGET` | 2048 步 | §19.1、§19.3-2 | **新增**：`releaseOld` 的步数预算（无续扫 ≈36 µs，续扫最坏 0.1–0.2 ms） |
| `EXTC_RC_QUOTA_MAX_MARK` / `_SHIFT_MARK` | 1<<20 / 3 | §19.1 停顿（P 通道）、§18.6 内存 | **本轮新增**（P0-P）：mark 配额 = `min(n>>3, cap)`；cap 调小换停顿，但 8192 档 pd_bigobj 峰值 34,328 kB 会破 G18 ⇒ memory-first 取 1<<20 |
| `EXTC_RC_QUOTA_MAX_WORDS` | 8192 | §19.1 | **本轮新增**：sweep 配额上限；合法 slab 至多约 2048 words ⇒ 实际不生效（只防异常 nslots） |
| `EXTC_RC_QUOTA_DEADLINE_US` | 800 | §19.1 停顿 | **本轮新增**：配额循环的物理时间封顶（0=关）；关掉后 dense 形状单步可达 10–18 ms |
| `EXTC_RC_SHARDS` | 1（上限 64） | 吞吐/收敛 | 单线程灾难，默认必须保持 1（§20-9） |
| 计数档位选择（§1.5） | 自动（按 unit：无环 ⇒ 档 1，否则档 2） | 吞吐与停顿通道；大槽形状上档 1 更快更省 | 实测：16 B 上档 1 慢 2.5×、32 KB 上快 3×（MEAS-T1 §3） |
| `EXTC_RC_REG_MAX` | 512 | §18.3-3 泄漏 | 需要淘汰/增长策略 |
| `EXTC_ARENA_SPARE_MAX` | 1 MiB | §18.1 arena 上界与归还抖动 | 只影响作用域基质 |
| `releaseOld` 预算 | 2048 步（本轮新增） | §19.1 / §19.3-2 | 已入界（无续扫 ≈36 µs，续扫最坏 0.1–0.2 ms）；下一档 512 步（无续扫 ≈9 µs） |
| slab 级省略（P1-7，默认开） | 整块全死 ⇒ 一次决策 | 不改任何界，只降清扫 CPU | 判据 O(nwords)；对 cycleratio 类形状有效（Ir −3.56%），promote 实测 0 次全死 ⇒ 无收益；计数档/大块/跨区不参与（§9.5） |
| `EXTC_RC_FREE_DEFER` / `_BUDGET_SLABS` / `_DEADLINE_US` / `_QUEUE_BYTES` / `_HARD_DEADLINE_US` / `_CQ_MAX_REGIONS` | 1 / 8 / 100 / 8 MiB / 1000 / 32 | §19.5 停顿（closing queue + free queue） | v8：`regionFree` 同步段 O(1)，逐 slab 工作进 closing queue；积压 >32 个 region 时 drop 内做一次有界排空 ⇒ pd 0.4 µs / dense 1.6 µs / pb 1.7 µs（G3b 闭合） |
| `EXTC_RC_LAZY_FREE` / `EXTC_RC_ELIDE_VERIFY` | 0 / 0 | §19.5.2 吞吐 | **本轮新增**：B 惰性 freelist 只作旋钮（默认关）；`ELIDE_VERIFY` 是编译期自检 |

---

## §22 现状一句话（交付摘要）

extC 的内存管理是**编译期分档的**：绝大多数分配被静态指派到作用域式基质（帧 arena / 任务 arena / plate / place 载荷区），内存有界、回收成本 ≈0；只有"逃逸到 level 1 或更浅"的引用对象进入三种档之一的运行期回收器（无回收 / 计数 + 定向释放 / 标记 + 步进清扫）。本轮（阶段 0–2）改造后，三件事变了：

1. **内存有了对外契约**：`M_peak ≤ M_max + 缓冲`，`M_max` 默认隐式 `3·live`、可用 `EXTC_RC_M_MAX` 显式指定 ⇒ 峰值可预言（§18.5）；代价是 sweep 次数变为 O(log N)。**v10 补齐了契约里漏掉的一项**：墙帽从 `M_max − live/4` 改成 `M_max − max(live/4, maxReq)`，使 `thr + maxReq ≤ M_max` 恒成立（任何单个请求都装得进墙内；`P1-ADMISSION.md`）——这一项是**结构性**的，实测峰值不变（`pd_bigobj` 仍 26,076 kB），只是 `thr_max` 从 22 MiB 收到 16 MiB。**v11 更正**：v10 的无条件预留让 dense 峰值 +27.3%（112,092 → 142,620 kB），现改为**只在显式墙下预留** ⇒ 默认路径与 v9 逐 kB 相同（`P1-ADMISSION.md` §3）。RSS 仍等于历史峰值（slab 不还 OS），这是下一个阶段要解决的事（归还/madvise）。
2. **三条硬契约通道全部入界（v8）**：`releaseOld` 无续扫 ≤2048 步 ≈36 µs（续扫最坏 0.1–0.2 ms）、大对象分段 8192 words/段（pd max 78–98 µs）、`regionFree` 同步段 O(1)（0.4–1.7 µs，`P1-CLOSING-QUEUE.md`）⇒ G3b 三形状（pd/dense/pb 100M）全部 PASS，G2/G3a 亦 PASS。**仍未达标的两项必须一起写**：G1②(a) 常规步 `>50 µs` 占比（warm 形状 4.25e-4–1.21e-3 > 1e-4）与 (b) 续扫段 p99.9（58.53/99.20 µs，`bseg-cal.tsv`，`[segsteps]` 桶界落 >64 µs 桶）；(b) 的尾部分解见 §19.5.3（环境抢占 + 冷页 + 256-pop 真工作），(a) 是预存问题、非本轮引入。**v9 更正（`P1-SEG-TAIL.md`）**：这两项的数值在本机主要由**宿主噪声地板**决定（配对噪声探针 p99.9 = 22–26 µs、>50 µs 事件率 133/s，与 GC 的 ≈200/s 同量级）；可判的是 p99 = 20.8/33 µs（≤50 µs 达成）与 over-1ms = 0，p99.9 50 µs 目标在 chunk=8192 下不可达。
3. **两处 UAF 已按保守方向修好**（P0-7 参数槽 insert-only、P0-7b bulk copy 逐引用记账），并登记了三个仍不回收的形状（L10-a/b/c ⇒ P0-8/9/10）。

4. **memory-first 一轮（用户 m03301：不抬 r / 不抬 `M_max`）做了两件小事，并否掉了三条看起来有希望的路**：全死 slab 批量重置（cycleratio Ir −3.56%、切片数 −4.8×，§9.5）与事件档 `initRef` 只发 `inc`（−1.09% 指令，其余档产物逐字节不变）；否掉的是 THP（chase 171.4→171.0 ms 无差异）、sweep 切片粒度（总 GC 时间不变）、promote 上的清扫省略（探针证明 0 次全死）。诚实的天花板：promote −15%、sizeratio −9%、cycleratio −6~10%、chase −2%；唯一 >10% 的单步收益是抬 r（2→4，实测 −17%），已被 memory-first 约束禁用。
5. **回收配额改为按分配字节计（P0-P + P0-B，§18.6）**：`pd_bigobj` 200 轮从 1,067,284 kB（60 轮的 2.163×、不可控）变为 **26,072 kB 平台（1.000×，上界 26,624 kB、余量 552）**，代价是同形状 GC 步总时长 ×28.7（144,038.6 µs）、`step_max` 66→113.4 µs；惰性 freelist（B）在 v7 轮**已被默认关闭**（`EXTC_RC_LAZY_FREE 0`）：它把清扫步总时长降 promote −20.2% / sizeratio −56.4% / cycleratio −10.8%，但墙钟收益只有 promote 可复现（交错 A/B 0.998→0.972 utime），sizeratio 反而 +5.1% ⇒ 登记 L14。

6. **P1 一轮（四项）三否定一落地；v8 闭合 G3b**：chunk 保持 **8192**（缩小只能拿内存换步时，见 §19.5）；惰性 freelist **默认关**；`regionFree` 两阶段在 v8 升级为 **closing queue**（同步段 O(1)、三形状 0.4–1.7 µs，§19.5）；`G1②` 改三通道量具（`[segsteps]` + `[chan]`，`max` 只作 OBS）。相对 v5 的同机交错 A/B（9 reps、utime）：promote **0.972**、chase **0.973**、cycleratio **1.019**、sizeratio **1.051（未决）**；相对 HEAD 的累计代价 promote +5.4%、sizeratio +5.0%、chase/cycleratio ≤+0.7%（§19.5.2、GC-ROOFLINE §6.7.6）。

7. **R1 + R2（量具可判化 + 标记步冷档，§19.6）**：R1 把 G1② 从桶界改为**精确超阈计数**（`[histo]`/`[segsteps]` 的 `gt10us/gt50us/gt64us`，`[chan]` 的 `collect` 改名 `trigger-step`），使判据可判；R2 用**按活集分档的每步 pop 上限**（暖 `min(b, 1024)` / 冷 `min(b, 128)`，门槛 `EXTC_RC_MARK_COLD_BYTES = 16 MiB`）把 `pb_escape_grow` 的标记步压回契约：p99.9 判据 **2.6e-4 PASS**、`>50 µs` **2.9e-5 PASS**（R1 为 RED / 2.56e-4），四形状 A/B **0.973–1.007**、`gate.sh` Ir **87,649,685 ≤ 88,029,115**、G18 **26,072 kB** 不变；`walk_max ≤ walk_cap` 仍逐字成立（冷档只收紧）。仍未闭合：cyc 的 `(a)` 1.11e-4（噪声地板）、pc 的 `(a)` **N/A（n=41,184 < 1e5）**、`bseg-cal.tsv` 的 `c_seg` 标定仍是 v9 口径。

8. **R-W（v29）：大槽的跨区出边用 64 B 组位图登记（§19.13）**：`W_unlink(ss, m) ≤ min(ss/8, 8·m)`，t04 的 `[step] max_us` **1106.4 / 871.6 / 671.6 → 4.7 / 11.5 / 16.7**、`over1ms` → **0**，`rs_inc / rs_dec / rs_live` 960 / 952 / 8 逐字不变，四形状 A/B 0.961 / 0.991 / 0.966 / 0.991、RSS 不变，`gate.sh` Ir **87,914,793 ≤ 88,029,115**（余量 114,322）。登记点在既有体外函数 `extc_rc_rsIncSlab` 里而不是内联体里 —— 内联体的**未执行**代码会让形状函数重新分配寄存器、+435k Ir 破线（12 变体标定，`GC-ROOFLINE.md` §6.18.3）。**诚实的边界**：只闭合稀疏侧，每个 64 B 组都有跨区出边的大槽上界仍是 `ss/8`。

9. **S1（v33）：容量归还收敛成一个水位（§19.16）**：删掉 `slabShrink` / `pageTrim` / `SHRINK_GRACE` / `SHRINK_CAP_LIVE` 四个旋钮（连同 `SLAB_KEEP`、`SLAB_KEEP_BIG`、`SHRINK_*`、`PAGE_TRIM_*` 共十三个名字与 `deadA`/`deadN`/`allocN`/`pgAllocN` 四个字段），换成一个不等式 `capBytes > CAP_MULT·bytes + RETURN_FLOOR`（默认 2 倍 + 16 MiB）与一个 `extc_rc_returnStep` pass（≤256 块访问 / ≤16 次摘链 / ≤256 KiB 页）。`grow11` 峰值 241,096 → **189,660 kB**（3.8× → 3.0× 活集）、`t13` 194,732 → **148,664 kB**、`grow11mr40` 108,244 → 104,676 kB、`t04` 保持 9,764 kB / 0.02 s；代价是 `t12`（每轮 64 MiB 全换新）墙钟 0.09 → 0.29 s（还回去的容量要重新缺页，233 ns/KB）；四形状干净 A/B **0.982 / 1.008 / 1.012 / 0.998**（min，15 reps；median 0.990 / 1.024 / 0.993 / 1.004，符号翻转 ⇒ 噪声带，同轮三臂开档与关档差 ≤1.3%），`gate.sh` Ir **87,678,211 ≤ 88,029,115**（删热路径 `allocN++` 使 Ir 下降）。

10. **R2a（v34）：巨块载荷从一个预留的 VA arena 里 carve（§19.17）**：buddy 2 MiB 量子、`MAP_NORESERVE`、选择性大页（≥ 4 MiB 才 advise）；t12 0.270 → **0.080 s**、t13 0.210 → **0.060 s**，四形状 RSS 不变（chase 快 3–4%），代价是 t04 +2.3 MB / t13 +16 MB 的大页尾页放大（≤2 MiB/被触区间；区间取整只落在 VA 上）；Ir 87,678,535，生成 C 只有运行期段内的 4 个 hunk。

11. **R2b（v35）：巨大块载荷内部的地址能命名宿主（§19.18）**：arena 增加归属表 `ownQ`（2 MiB 量子 → slab，32,768×8 B = 256 KiB/arena，惰性分配），`slabNew` 的 big 分支绑定、`extGive` 回收块时清空；`slabOfInterior` 与 `wmarkSlot` 各加一级区间反查 ⇒ 载荷内部写跨区指针时不再进 `pin` 表、位图能记到组、释放时边能撤销。探针：`[pin] n=0`（修复前 800k 次存储涨 17.5 B/次）、`rs_inc=960 rs_dec=952 rs_live=8`（修复前 rs_dec=0 rs_live=960）；`tests/reclaim` 221/0；暖档四形状 0.979 / 1.030 / 1.004 / 1.027（两个形状没有 ≥2 MiB 载荷 ⇒ 只付一次原子读）。

12. **R3（v36）：载荷先留地址、再原地长（§19.19）**：`ExtcRcSlab.bigMax` 记住「载荷的地址最多能长到哪里」，`extc_rc_bigPayloadAlloc` 按 `clamp(bigRound(n)·16, 2 MiB, 1 GiB)` 预留、只提交 `bigRound(n)` 页，`extc_rc_bigGrow` 在预留之内换 `ss`/`rcp` 并把 `bytes/allocBytes/capBytes` 各加一个增量（组位图按新步长重建），新窗口不清零（内核首次读给零页）。实测 8 MiB→64 MiB 22 步 **15.6–20.2 ms**、缺页 **61** 次、RSS HWM 67.9 MB、`data_same=1`，对照同形状同会话的拷贝增长 375.7–394.5 ms / 峰值 130.8 MB ⇒ **快 17–25 倍**；只预留不碰时 RSS 10.4 MB（预留是 VA 不是内存）。生产默认 `EXTC_RC_EXT_SLACK = 0`（今天生成文本里没有消费者，R4 的容器/增长动词会打开它）；驱动用 16 编译 ⇒ 13 条断言（`tests/reclaim` **234/0**）。拒绝语义：`want > bigMax` 返回 0，调用方搬一次，容量界与停顿界都不变。
13. **R4a（v37）：可增长数组的存储成了自己的一片预留（§19.20）**：新增第五种存储基质 `buf<T>` —— 进程级 8×64 GiB 的 VA 竞技场（懒创建、`MAP_NORESERVE`）、2 MiB 量子的 O(1) buddy（双向自由链 + 起点位图）、`≥ 4 MiB` 才 `MADV_HUGEPAGE`，carve **7 ns** 对照一次 `mmap` **1,897 ns**；四格运行期入口 `extc_buf_reserve` / `extc_buf_release` / `extc_buf_discard` / `extc_buf_span`，两个发射原语 `extc_sizeOf<T>()` 与 `extc_viewOfT<T>`。不变量：`len ≤ cap ≤ max`、元素地址恒为 `base + i·sizeof(T)`；增长在预留内**零拷贝**、搬家「拷一段丢一段」（峰值 = 一份 + 2 MiB）。`bench/buf` 五形状（REPS=7，min）：S1 8e6 个 `i64` **0.020 s / 2 次搬家 / 65,452 kB** vs `std::vector` 0.059 s / 24 次 / 69,196 kB vs `vector+reserve` 0.030 s / 1 次 / 66,308 kB；S2 一百万短命 buf 0.076 vs 0.096 s；S3 装满→`shrink(4096)`→长回 **0.028 vs 0.085 s**（还 64 MiB 页 <1 ms、长回 4 ms）；S4 0.017 vs 0.049 s；S5 布局连续（不作排名）。三条撞出来的边界：buddy 第一版线性扫 + 涂 interior ⇒ 每次分配 65,536 次写、S2 **0.76 s**；发射器给每个函数发 `extc_arena` 前置挡住内联 ⇒ 同一循环 **2.6 → 10 ns/push**（指令数只差 10%），10 个热成员 `@inline` 后 76 → 4 ms；`reservedBytes()` 报请求值而非实际 carve 块 ⇒ `buf_fuzz` 抓到 `bad=5692 / worst=33,552,768`，改报 `extc_buf_span()` 后 `bad=0 / worst=0 / leak=0`。`tests/stl` 失败 0（含 ASan），只用 `buf` 的程序仍是 T0（不发射回收运行期）。
14. **R4b（v37）：引用写进非 rc 存储 —— 外部边表（§19.21）**：把一直存在的 `pin` 表从「值键 + 计数」换成「**槽地址键**，一槽一条 `{slot, v, rg}`（24 B）」，并新增区间撤销 `extc_rc_pinDropRange(lo, hi)`（`buf` 的 `shrink` / `clear` / `release` / 搬家经 `extc_buf_releaseRefs` 调用）。两条可证的理由：①**旧值只能从表里取**——存储会被还回去再 carve（`buf` 的 `release` 不清零），槽里的字节可能是上一位主人的陈旧指针，按它 `dec` 撤掉的是别人的边（UAF 方向）；②结构动作要**按地址区间**精确撤边。同时修掉 `storeStructCons` 在宿主未知时置 `noSweep` 的整区退出（改成逐字登记）。新门禁 **G21**（`buf<mut ref node>` 每轮 withCap/push/shrink/push/release，n=50k vs 800k）：本轮 **3,764 / 3,764 kB = 1.000× PASS**，基线（HEAD 8faab139 值键）**24,144 → 363,008 kB = 15.035× RED**，**对照臂**（把 `pinDropRange` 改成立刻返回）仍 **1.000×** ⇒ **上界来自键的选择、撤销买的是及时性**，两者不能混说。驱动 `t_pin` / `t_pinRange` / `t_storeStructRooted`（清扫档与计数档、ASan、不内联档）⇒ `tests/reclaim` **242/0**。契约：`M ≤ live + 24 B × 峰值外部槽数`（无轮数项）、结构动作 O(区间条目 + 表扫描) 在 mutator 侧、`buf<i64>` 零成本、`pop` 不撤边、`pinSeed` O(表容量) 未标定。边界：池块（G15）仍是 XFAIL，R4c 把池存储换成 `buf` 后同一张表覆盖它。
一句话没变：作用域基质（arena/place/coro/plate）从来是有界的；rc region 的有界性**现在由契约给出、由标定支撑**。门禁 G1–G16 与 after 实测见 `docs/analysis/GC-ROOFLINE.md` §6–§7。

下一步的优化方案与形式化模型（内存有界、停顿有界、类 Roofline 的 GC 策略平衡）见同目录 `docs/analysis/GC-ROOFLINE.md`。

**独立复核（doc-audit，`AUDIT-GC-P0.md` §18）**：自建 `build-da/extc`（md5 `b7cef25f…`）重跑 after 全套 ⇒ **内存侧全绿**（G4-Stress 9144≤15223 kB、末 6 段差 0.00%、G8 0.478×、G14 1.000×、G16 完成、G17 逐 kB 一致）；**停顿侧稳健红降为 2 条**：G1②（pb >50 µs 占比下界 1.20e-4 > 1e-4，与我方 1.39e-4 同向）与 G3b（`regionFree` 时间未界）；G1① 的 2540 µs 与 G2 的 215.3 µs 在 3+20+10 次独立复跑中均不复现（分别 0/13 与 PASS/13），已改判为「单次奇点」与「旧判据过敏（改 p99.9 后 PASS）」。吞吐绿（G5 86,316,991 Ir，余量 1.9%）。
