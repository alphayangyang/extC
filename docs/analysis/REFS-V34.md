# REFS-V34：源文件行号换算表（v33 → v34）

本表服务两件事：①任何写 `源文件:行号` 的地方，若数字来自 v33 或更早，必须经本表换算；②v34 新增/改动的行给出绝对行号，后续引用直接抄这里。**禁止用单一 offset 平移**（本轮就有一条 −1 与三条 +N 的分段）。

## §0 口径

| 文件 | v33（HEAD 90c876f1，R1 提交前） | v34（本轮工作树） |
| --- | --- | --- |
| `src/back/reclaim.c`（物理） | `75ecc57760aa8c8d92bef3f4bd452529` / 241 行 | `b05bbde438d39f3254dc5ea5e8b8d220` / **242 行**（25 条 `#include "rt/*.inc"`） |
| 渲染文本（`tools/source_split.py` 读出） | `2ad2ae580109578b77d6f096214b19f1` / 4,780 行 / 356,735 字符 | `837071b7b5d835af0cec34e72065e7c5` / **4,997 行** / 370,381 字符 |
| `src/back/codegen.c` | `6ba135e22e86ec89d5cd8aa416a994bc` / 75 行（渲染 16,429 行 / `aaaaf36f73ff859044acb222934ce9c8`） | 未改 |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` / 1,637 行 | 未改 |
| `build/extc` | `b5a05be1ce5780d34a8f50b4e322322a` | **`9bcf0e917446b75e04e3cdb928ea3232`** |
| base 臂（A/B 对照） | — | `git worktree add -f /tmp/r2/wt 774d5f72` → `build/extc` = `4dc2a04fde98de81ebd56b257930179c`；跑生成物须 `EXTC_STD=<wt>/stdlib` |

注意：`build/extc` 的 md5 依赖 `BUILDDIR`，同一源码在不同 `BUILDDIR` 下 md5 不同；溯源只认**源文件与渲染文本**的 md5，不认二进制。

## §1 本轮改动清单

| 位置 | 动作 | 渲染文本影响 |
| --- | --- | --- |
| `src/back/rt/32-extent.inc`（新文件，214 行 / `c632abca276ce60c5e20800a648f3c6d`） | 新增 `[rt:extent]` 整节：buddy VA arena、选择性大页、两个入口 | **+213 行** |
| `src/back/reclaim.c` | include 列表插一行 `#include "rt/32-extent.inc"`（在 `32-deferred.inc` 与 `33-hugefree.inc` 之间） | 0（物理 +1 行） |
| `src/back/rt/33-hugefree.inc`（617 行 / `127e16e52a94b33b65e026bc2a55f97d`） | ①`extc_rc_bigPayloadAlloc` 先 `extc_rc_extAlloc`，失败改走 `aligned_alloc`（渲染 1127–1131）；②`extc_rc_bigTrimStep` 末端 `free` 改 `extRelease` 或 `free`（渲染 1166） | **+1 行**（两处） |
| `src/back/rt/62-sweep.inc`（588 行 / `68101667c4cf0c18f73e604b7686a310`） | `[return]` 后新增 `[extent]` 探针（`#if EXTC_RC_EXT_OK` 守卫） | **+3 行** |
| `docs/design/RECLAMATION.md` | 跳过表新增 `skip:ext-no-mmap` / `skip:ext-not-base` / `skip:ext-not-block` | 文档，不影响渲染文本 |

净变化：4,780 − 4,780 = **+217 行**（213 + 1 + 3；其中 33-hugefree 的 +1 记在替换里，见 §2）。

## §2 分段换算表（v33 行号 → v34 行号）

由 `difflib.SequenceMatcher(autojunk=False)` 对齐两份渲染文本得到，逐段可直接抄：

| v33 行号 | v34 行号 | offset | 说明 |
| --- | --- | --- | --- |
| 1 – 882 | 1 – 882 | +0 | 头、计数器、类型、wmark、helper、slab、延迟释放 |
| （无） | **883 – 1095** | — | **新增 `[rt:extent]` 整节**（213 行） |
| 883 – 915 | 1096 – 1128 | +213 | `[rt:hugefree]` 头 + 常量 + 丢页结构 |
| 916 – 917 | 1129 – 1131 | +213→+214 | `bigPayloadAlloc` 的两行被替换成三行（carve、失败改走、失败报错） |
| 918 – 951 | 1132 – 1165 | +214 | `bigPayloadAlloc` 其余 + `bigPayloadRelease` + `bigTrimStep` 前段 |
| 952 | 1166 | +214 | `bigTrimStep` 末端：`free(d)` → `{ int kept = extc_rc_extRelease(...); if (!kept) free(d); }`（行数不变） |
| 953 – 4424 | 1167 – 4638 | +214 | 其余 21 节（pause / budget / xregion / closing / capacity-release / slot-release / cascade / nocount / shade / xentry / chain / tlsdepth / mark / release / sweep 的前段） |
| （无） | **4639 – 4641** | — | **新增 `[extent]` 探针三行**（`#if` / `fprintf` / `#endif`） |
| 4425 – 4781 | 4642 – 4998 | +217 | `[work]` 行起的尾部 |

反查（v34 → v33）：≤882 同号；883–1095 是新行；1096–1128 减 213；1129–1131 映射到 v33 916–917；1132–1165 减 214；1166 → v33 952；1167–4638 减 214；4639–4641 是新行；≥4642 减 217。

## §3 v34 绝对锚点（本轮新增或改动）

全部相对**渲染文本**（`python3 tools/source_split.py --read src/back/reclaim.c`）：

| 内容 | 行号 |
| --- | --- |
| `[rt:extent]` 分节头（注释块起） | 883 |
| 注释块（为什么 carve 而不是映射、ordQ 编码、buddy 规则） | 883 – 907 |
| `EXTC_RC_EXT` / `EXTC_RC_EXT_QSHIFT` / `EXTC_RC_EXT_ORD` | 908 – 916 |
| `EXTC_RC_EXT_HUGE` / `EXTC_RC_EXT_HUGE_MIN` / `EXTC_RC_EXT_ARENAS` | 917 – 926 |
| `EXTC_RC_EXT_OK` 平台门 | 927 – 929 |
| `extern uint8_t *mmap(...)` 声明（必须与 stdlib 的 plates 一致） | 952 |
| `extc_rc_extMmap(size_t)` 包装 | 953 – 955 |
| `typedef struct ExtcRcExtArena` | 957 – 962 |
| arena 数组与计数器（`extc_rc_extArena` / `extc_rc_extNArena`） | 963 – 965 |
| `extc_rc_extArenaNew(void)`（mmap + 对齐 + 位数组） | 967 – 991 |
| `extc_rc_extCarve(com, max, outCom)`（buddy 分配 + 选择性大页） | 993 – 1034 |
| `extc_rc_extGive(p)`（量子校验 + buddy 合并） | 1036 – 1066 |
| `extc_rc_extLockAcq` / `extc_rc_extLockRel` | 1069 – 1070 |
| `extc_rc_extAlloc` / `extc_rc_extRelease`（对外两个入口） | 1071 – 1082 |
| `#else`（`EXTC_RC_EXT_OK == 0`）的零计数 stub | 1083 – 1095 |
| `[rt:hugefree]` 分节头 | 1096 |
| `extc_rc_bigPayloadAlloc`（carve 优先） | 1127 – 1136 |
| `extc_rc_bigTrimStep`（末端 `extRelease`） | 1154 – 1170 |
| `[extent]` 探针 | 4639 – 4641 |

`src/back/reclaim.c` 物理行：242 行，include 列表里 `32-extent.inc` 位于 `32-deferred.inc` 之后、`33-hugefree.inc` 之前。

## §4 使用规则

1. **禁止单一 offset 平移**：本轮有四段不同 offset（+0 / +213 / +214 / +217）与一个插入段。
2. 引用**物理行**（`src/back/reclaim.c:行`）只在指「include 列表」时用；任何运行期逻辑一律引渲染文本行号，并注明「渲染文本」。
3. `tools/doc_anchors.py` 的 STRICT 面（`docs/design/RECLAMATION.md`）不许带数字锚点；RATCHET 面（本目录下两份分析文档）新增数字锚点会被棘轮判红 ⇒ 新写内容一律用符号定位（函数名、宏名、分节头），数字只出现在 REFS-*.md 与 changelog 里。
4. `tools/emitted-identity-baseline.tsv` 本轮用 `--write --set core --why "R2: the runtime text gains the extent substrate section ([rt:extent]) and the payload path carves from it; every generated file that emits the runtime changes by that insertion alone"` 重冻结；下一个 v35 若仍只改运行期文本，仍允许整段位移，但**程序自身文本的变化必须单独解释**。
5. v31 → 当前的行号换算仍走 `docs/analysis/REFS-V31.md`（v29→v31）与本表（v33→v34）串联，中间还缺 v31→v33 的一段：`docs/analysis/REFS-V33.md` 已给 v32→v33；v31→v32 的净变化见 `docs/analysis/REFS-V31.md` 末尾的说明与 v32 提交记录。

