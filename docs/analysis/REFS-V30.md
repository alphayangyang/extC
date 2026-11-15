# REFS-V30：v29 → v30 源码行号映射与绝对锚点

本文件把文档里引用的源码行号从 v29 口径换到 v30 口径。**v30 只改 `src/back/codegen.c` 一个文件**，
`src/back/reclaim.c`、`tests/reclaim/driver.c`、`src/main.c` 的语义行号未变（`src/main.c` 只在 debug 开关表里插了
`EXTC_DBG_IDLEMEM` 三行；`docs/usage/flags.md` 由 `tools/gen_flags.py` 重生成）。

## 1. 指纹

| 文件 | v29（HEAD `853e46d0`） | v30（本轮） |
|---|---|---|
| `src/back/codegen.c` | `3ae6e499b6a2d64668ff1daf2eed26a2` / 16,222 行 | `ec12011cc5bbff7100072bb465d8f234` / **16,346 行**（+124） |
| `src/back/reclaim.c` | `845c57e2b230202efcd04639e07664d9` / 4,738 行 | 同左（未改） |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` / 1,637 行 | 同左（未改） |
| `build/extc` | `b5ad0e7cdcb7abbf0cd8f8f18b113570`（v29 提交树） | `f5ed0d066b084f7f13024b9f4f14950c` |

基线方法：`git worktree add -f /tmp/d6/wt HEAD` 后在树内 `make build/extc -j8` ⇒ `/tmp/d6/wt/build/extc` =
`6447a11da44c564569f90f21b688103d`、其 `codegen.c` md5 = `3ae6e499…`（确认是 HEAD 即 v29）。
注意：不同 `BUILDDIR` 下编出的 `extc` md5 不同，溯源只认源码 md5。

## 2. 变化块（`git diff -U0` 逐 hunk，10 块）

| # | v29 行 | v30 行 | 性质 | 内容 |
|---|---|---|---|---|
| 1 | 474–475 | 474–477 | 替代（2→4） | `rcForced` 字段的注释改写（现在时），字段仍在 477 |
| 2 | 475 后 | 478–491 | **新增 14 行** | `rcMemGrowth` 字段 + criterion (d) 说明注释 |
| 3 | 14304 后 | 14321–14381 | **新增 61 行** | `RcMemProbeCtx`、`rcMemExprProbe`、`rcMemStmtProbe`、`rcBodyGrowsMemory` |
| 4 | 14356 后 | 14433–14459 | **新增 27 行** | `unitGrowsMemory` 与它的调用说明 |
| 5 | 14403–14404 | 14507–14510 | 替代（2→4） | `scanUnitReclaim` 顶部注释（`EXTC_RECLAIM=1` 的真实语义） |
| 6 | 14414 后 | 14521 | **新增 1 行** | `scanUnitReclaim` 的局部变量/空行 |
| 7 | 14430–14431 | 14537–14550 | 替代（2→14） | idle 分支 (2b)：`unitGrowsMemory` 判定 + `[idlemem]` 打印 |
| 8 | 14459 | 14578–14580 | 替代（1→3） | `if (g->rcForced) return;` 复位 + 三行注释 |
| 9 | 16146 | 16268–16270 | 替代（1→3） | 档位闸门加 `!g.rcMemGrowth` |

（第 10 块是纯位移，不单独列。）

## 3. 分段换算表（v29 n → v30）

| v29 行范围 | v30 行范围 | offset |
|---|---|---|
| 1–473 | 1–473 | 0 |
| 474 | 474–476 | 替代 |
| 475 | 477 | +2 |
| （插入） | 478–491 | — |
| 476–14304 | 492–14320 | **+16** |
| （插入） | 14321–14381 | — |
| 14305–14355 | 14382–14432 | **+77** |
| （插入） | 14433–14459 | — |
| 14356–14402 | 14460–14506 | **+104** |
| 14403–14404 | 14507–14510 | 替代（2→4） |
| 14405–14414 | 14511–14520 | **+106** |
| （插入） | 14521 | — |
| 14415–14429 | 14522–14536 | **+107** |
| 14430–14431 | 14537–14550 | 替代（2→14） |
| 14432–14458 | 14551–14577 | **+119** |
| 14459 | 14578–14580 | 替代（1→3） |
| 14459–16145 | 14581–16267 | **+122** |
| 16146 | 16268–16270 | 替代（1→3） |
| 16147–16222 | 16271–16346 | **+124** |

**规则（与 REFS-V23/V24/V28/V29 一致）**：禁止用单一 offset 平移旧引用；先查本表所在分段；落在「替代」行的旧行号
作废，按符号 grep 现文本。文档里 1,600 余处旧引用不逐条重指，只在用到的那一条上按本表换算并写入新文。

## 4. v30 绝对锚点（直接 grep 用）

| 符号 | 行 | 说明 |
|---|---|---|
| `bool rcMemGrowth;` | 491 | idle 第四判据的结论字段 |
| criterion (d) 注释 | 14324 | 判据的语义与「为什么不许升 T1」 |
| `typedef struct { int depth; } RcMemProbeCtx;` | 14340 | 循环深度上下文 |
| `static bool rcMemExprProbe(void *ctx, Expr *e)` | 14348 | 命中：`EX_NEW` + count + depth>0 + `planArenaLevel==1` |
| `static bool rcMemStmtProbe` | 14361 | `ST_WHILE` 时 depth++/-- |
| `static bool rcBodyGrowsMemory(Stmt *body)` | 14375 | 一具函数体 |
| `static int unitGrowsMemory(CG *g, Module *m)` | 14442 | 整单元（`g->funcs` 14446、generic 方法体 14453） |
| `if (!unitGrowsMemory(g, m))` | 14543 | idle 判定短路点 |
| `g->rcMemGrowth = true;` | 14547 | 标记该单元 |
| `dbgOn("EXTC_DBG_IDLEMEM")` 打印 | 14548 | `[idlemem] …` 量具 |
| `if (g->rcForced) return;` | 14578 | 开档只绕最后剪枝（在 `rcBodyHasGovernedAlloc` 循环之前） |
| 档位闸门 | 16268–16270 | `… && !g.rcMemGrowth && unitAllAcyclic(&g, m)` |

## 5. 生成物证据（v30 编译器）

- `tests/gc-torture/src/t12_overwrite_arena.extc` ⇒ 生成 C 含 11 处 `extc_rc_alloc`、`#define EXTC_RC_TIER 2`；
  v29 编译器同源 ⇒ 0 处、无 `EXTC_RC_TIER`。
- `/tmp/av/loop1.extc`（块作用域内新建）⇒ v29/v30 都是 0 处 `extc_rc_alloc`、4 处 `extc_arena_release`（(d) 不误伤）。

## 6. 一句话

v30 把「整段不发射运行期」的证明从「不丢活引用」扩成「不丢活引用、也不丢活内存」；代码只动 codegen 的判定侧，
运行期（`reclaim.c`）零改动，所以 §5/§17/§18/§19 里所有 `reclaim.c` 行号继续有效。
