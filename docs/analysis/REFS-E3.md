# REFS-E3: codegen.c 的分节坐标（E3 之前 → 之后）

本文件记录 E3（把 `src/back/codegen.c` 的 16,368 行按主题切到 `src/back/cg/*.inc`）
之后旧行号该往哪里找。它与 `REFS-E2.md` 分层：E2 管「E2 改动带来的 ±22 行」这一层偏移，
E3 管「同一个行号现在落在哪个分节文件、节内第几行」这一层坐标。两层叠加时先按 E2 的 87 个变化块
落到 E2 之后的行号，再查本文的表。

## 0. 口径

| 时点 | `src/back/codegen.c` | 渲染文本（编译器实际读到的 16,368 行） | `build/extc` |
|---|---|---|---|
| E3 之前（提交 `636323cb`，E2 收口） | 16,368 行、md5 `02acf0b0ae84ece59fb2b70d604ddda7` | 同左（文件就是正文） | `24ce5d0ab3e817bc5f8ddedee6773b84` |
| E3 之后（当前树） | **75 行**、md5 `6ba135e22e86ec89d5cd8aa416a994bc` | **16,368 行、md5 `02acf0b0ae84ece59fb2b70d604ddda7`（逐字节未变）** | `eaa22babe131cfc6360fac7eb51a7b0c` |

三个读法：

1. **源码口径以渲染文本为准。** `tools/source_split.py` 把 `src/back/codegen.c` 的 22 条
   `#include` 指向的 `cg/*.inc` 就地展开，得到 16,368 行，与 E3 之前逐字节相同 ⇒ 文档里
   已有的 codegen.c 行号**仍指同一行正文**，只是那一行现在住在某个 `.inc` 里。
2. **二进制 md5 变了不是回归。** `build/extc` 从 `24ce5d0a…` 变成 `eaa22bab…`：
   DWARF 记录分节文件名与节内行号，同一函数的目标码没有变。更强的证据是生成 C 逐字不变：
   `python3 tools/emitted_identity.py --check` 报 430 行基线与基线逐字节相同。因此**不要用
   `build/extc` 的 md5 当源码指纹**；要指纹就取渲染文本的 md5。
3. **新写内容一律用符号锚点。** 严格集文档已是 0 个数字锚点，`check-doc-anchors` 只许棘轮单向。

## 1. 分节表（旧绝对行 → 分节文件 + 节内偏移）

切点是正文里**既有的 banner 注释行**（`/* ---- … */` 与两条 `/* ==== … */`），
没有移动任何一行；节内第 1 行就是旧行号「起点」那一行。`codegen.c` 的 39 行文件头（`@file` 注释与
全部 `#include`）不进任何分节，直接留在根文件里。

| 旧行号区间 | 分节文件 | 行数 | 主题 |
|---|---|---|---|
| 40–64 | `src/back/cg/00-type-mapping.inc` | 25 | type mapping |
| 65–139 | `src/back/cg/10-state.inc` | 75 | state |
| 140–1019 | `src/back/cg/20-primitive-block.inc` | 880 | runtime primitive block |
| 1020–1415 | `src/back/cg/30-expressions.inc` | 396 | expressions |
| 1416–1617 | `src/back/cg/40-type-descriptors.inc` | 202 | type descriptors |
| 1618–1815 | `src/back/cg/50-descriptors-on-demand.inc` | 198 | descriptors on demand |
| 1816–2457 | `src/back/cg/60-structural-eq.inc` | 642 | structural equality |
| 2458–3633 | `src/back/cg/70-trailing-args.inc` | 1,176 | implicit trailing arguments, one owner |
| 3634–5013 | `src/back/cg/80-roots-and-triggers.inc` | 1,380 | static root table and trigger |
| 5014–5483 | `src/back/cg/90-lambda-fnptr.inc` | 470 | lambda function pointers |
| 5484–5496 | `src/back/cg/100-debug-printing.inc` | 13 | debug printing |
| 5497–5564 | `src/back/cg/110-statements.inc` | 68 | statements |
| 5565–7130 | `src/back/cg/120-question-expansion.inc` | 1,566 | the ? expansion |
| 7131–7988 | `src/back/cg/130-local-host-region.inc` | 858 | local objects, static host region |
| 7989–9273 | `src/back/cg/140-view-subscript.inc` | 1,285 | view-subscript loop entry |
| 9274–10343 | `src/back/cg/150-top-level.inc` | 1,070 | top level |
| 10344–10689 | `src/back/cg/160-generic-instances.inc` | 346 | generic instances |
| 10690–11179 | `src/back/cg/170-struct-order.inc` | 490 | struct definition order |
| 11180–13551 | `src/back/cg/180-unreferenced-defs.inc` | 2,372 | unreferenced definitions |
| 13552–14280 | `src/back/cg/190-lambda-envs.inc` | 729 | lambda environments |
| 14281–14342 | `src/back/cg/200-region-reclaim.inc` | 62 | region reclamation, whether this pass emits |
| 14343–16368 | `src/back/cg/210-criterion-d.inc` | 2,026 | region reclamation, criterion (d) |

合计 16,329 行；加上根文件里不进分节的 39 行文件头 = 16,368 行。任何旧行号 n 的换算：

    file = 第一个满足 n <= 区间上界 的分节
    line = n - 区间起点 + 1

抽样逐字核对（节内该行 == 旧文件该行）：`40` → `cg/00-type-mapping.inc:1`；
`3888` → `cg/80-roots-and-triggers.inc:255`；`4696` → `cg/80-roots-and-triggers.inc:1063`；
`12442` → `cg/180-unreferenced-defs.inc:1263`；`14541` → `cg/210-criterion-d.inc:199`；
`16368` → `cg/210-criterion-d.inc:2026`（文件末行）。

两个常用坐标：`typedef struct RcPlan` 在 `src/back/cg/20-primitive-block.inc:59`（旧行 198）；
决策入口 `static void rcPlanDecide(CG *g, Module *m)` 在 `src/back/cg/210-criterion-d.inc:199`
（旧行 14,541）。

## 2. 使用规则

1. 旧行号 n：按表落到「文件 + 节内行号」；**不要**把它当新 `codegen.c` 的行号（该文件只有 75 行）。
2. 首选做法是按符号定位：`python3 tools/doc_anchors.py --map src/back/codegen.c:14541` 按渲染文本解析，
   输出 `codegen.c:rcPlanDecide`；进分节文件时再按函数名 grep。
3. 节内行号会随该节编辑而漂 ⇒ 引用函数名，不引用节内行号。
4. 不要混写「渲染文本行号」与「分节文件行号」；要写数字就写明是哪个坐标，并附符号。

## 3. 工具接线（本轮改名与新增）

* `tools/reclaim_source.py` → **`tools/source_split.py`**：`SPLITS` 登记两棵分节树
  （`src/back/reclaim.c` → `src/back/rt/`，`src/back/codegen.c` → `src/back/cg/`）；
  `files()/read()/text()/lines_with_owner()/write()` 对任意分节根生效，非分节文件原样读写；
  新增 `read_path(path)`（工具手里是 `pathlib.Path` 时用）；仓库外临时文件原样读，供 selftest。
* `write()` 仍按「渲染文本 diff → 唯一 owning 行 → 写回该 `.inc`」；一个补丁跨两节会被拒绝
  （`inject.py` 的多行补丁都在同一节内，实测通过）。
* `tools/cscan.py` 新增 `read(path)`：分节根走渲染，其余原样读 —— 凡是「读源码做结构断言」
  的工具都改走它，否则读到 75 行 `#include` 列表然后报 ok（这类静默失明是本轮真正修掉的隐患）。
* 新闸门 `check-source-split`（`core` 组）：`python3 tools/source_split.py --check` 断言五件事 ——
  每条 `#include` 都有文件、没有空分节、分节以换行收尾、`*-split:begin`/`*-split:end` 各一处且顺序正确、
  渲染文本里没有残留的未展开 `#include`。

## 4. 取证

* `python3 tools/source_split.py --check`：`reclaim.c` 24 节 / 4,780 行 / 357,316 字符；
  `codegen.c` 22 节 / 16,368 行 / 874,897 字符。
* `python3 tools/emitted_identity.py --check`：430 行基线逐字节相同（编译器产物未变）。
* 渲染文本与 E3 之前的文件逐字节相同（difflib 0 处差异）⇒ 旧行号引用继续有效。
* `make build/extc -j8` 成功；`-MMD` 已跟踪 22 个 `src/back/cg/*.inc`（改任一节会重编 `codegen.o`）。

## 5. 未做（有意）

* **不拆多翻译单元。** 真拆 TU 要把数百个 `static` 导出到新头文件，风险与收益不成比例；
  本轮要的是「按主题能打开一个文件」，不是链接期边界。`cg_internal.h` 因此仍不需要。
* **不重排分节边界、不重命名函数。** 切点取正文既有 banner，节名只描述主题。
* **分节文件不是独立可编译单元**：没有自己的 `#include`，只能经 `codegen.c` 的渲染读。
