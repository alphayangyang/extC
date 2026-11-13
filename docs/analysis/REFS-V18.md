# REFS-V18：src/back/reclaim.c 行号 v17 → v18 映射（R2 冷档）

本表是 R2 轮的交付物之一：记录 `src/back/reclaim.c` 从 v17（R1 冻结，HEAD `762abb79`）到 v18（R2 冷档）的**全部**行号变动，以及 `docs/analysis/` 下两份分析文档里既存 `reclaim.c:NNNN` 引用该怎么换算。

与 `REFS-V4.md` / `REFS-V7.md` 的区别，必须写在最前面：那两张表是**逐条语义重指**（因为 v4→v5→v7 有删除、移动、重写，机械偏移会把引用指到别的函数上，历史上确实发生过 `thrUpd` 被指到 `markResume`）。**v17→v18 是纯插入**：用 `diff -u` 可以证明只有 3 处插入（共 13 行）+ 2 处原地修改（`[work]` 的格式串与它的实参行），没有删除、没有移动、没有函数体改写 ⇒ 行号映射在这里是**可证明的恒等变换**，不需要逐条重指。

## 0 冻结口径

| 文件 | md5 | 行数 | 说明 |
|---|---:|---:|---|
| `src/back/reclaim.c` | `8f49f0718d788cd1202d348945260ab3` | 4,282 | **v18 权威源码**（R2 冷档）|
| `src/back/reclaim.c`（v17） | `39c93ca4fa0718920844e91147695589` | 4,269 | R1 冻结版，`git show 762abb79:src/back/reclaim.c` 可取 |
| `src/back/codegen.c` | `cc01b5f4216ea5c8d9fde77c47c41f24` | 16,212 | 本表不涉及（v16 起未改）|
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` | 1,637 | 本表不涉及（v13 起未改）|
| `build/extc` | `9ceb7690c3ae62d12921b7f0a9bf3757` | — | 本轮门禁实测用的编译器（同源码不同 `BUILDDIR` 的 md5 不同，**溯源只认源码 md5**）|
| `docs/analysis/MEMORY-MODEL-CURRENT.md` | `69368e825376a3cbf4e156d5070a93a6` | 1,129 | 快照：本文写完后若文档再改，行号列会漂移，但「原文引用」列仍可按文本搜到 |
| `docs/analysis/GC-ROOFLINE.md` | `7e9df7ec045513f44daac6f3bdea46fe` | 1,226 | 同上 |

## 1 取证：v17 → v18 的 diff 全貌

```
git show 762abb79:src/back/reclaim.c > /tmp/r2c/v17.c
diff -u /tmp/r2c/v17.c src/back/reclaim.c
```

输出只有 5 个 hunk，逐字要点：

| hunk | v17 位置 | 形态 | 内容 |
|---|---|---|---|
| 1 | `@@ -1150,6` | **插入 11 行** | 在 `#define EXTC_RC_MARK_BATCH_MAX 256` 的 `#endif` 之后插入 `EXTC_RC_MARK_COLD_BYTES` / `EXTC_RC_MARK_COLD_BATCH` 两个常量块与一条 `#error` 约束 |
| 2 | `@@ -1175,6` | **插入 1 行** | 在 `extc_rc_stepGt10/50/64` 声明之后插入 `static long long extc_rc_markColdN;` |
| 3 | `@@ -1415,6` | **插入 1 行** | 在 `extc_rc_markAdvance` 的 window clamp（`if (b > MARK_BATCH_MAX * _pm) …`）之后插入冷档 clamp |
| 4 | `@@ -3911,7` | 原地改 1 行 | `[work]` 的 `fprintf` 格式串尾部加 ` cold_steps=%lld` |
| 5 | `@@ -3923,7` | 原地改 1 行 | 同一条 `fprintf` 的实参表尾部加 `, extc_rc_markColdN` |

**没有任何删除行、没有移动行**（diff 里 `-` 开头的只有 hunk 4/5 那一对被 `+` 替换的同位行，行数不变）。这是本表可以按段换算的唯一理由。

## 2 映射表（v17 → v18）

| v17 行区间 | v18 行区间 | delta | 依据（边界锚点原文）|
|---|---|---:|---|
| `n ≤ 1152` | 同 `n` | 0 | v17:1152 `#endif`（紧随 `MARK_BATCH_MAX` 的定义）在 v18 仍是 :1152 |
| `1153 – 1177` | `n + 11` | +11 | v17:1153 `#ifndef EXTC_RC_MARK_BATCH_POP` → v18:1164（同一行文本）；v17:1177 `static long long extc_rc_stepGt10, extc_rc_stepGt50, extc_rc_stepGt64;` → v18:1188 |
| `1178 – 1417` | `n + 12` | +12 | v17:1178 `#define EXTC_RC_STEP_NB 15` → v18:1190；v17:1417 `if (b > (size_t)EXTC_RC_MARK_BATCH_MAX * (size_t)_pm) b = …` → v18:1429（冷档 clamp 插在它后面，:1430）|
| `n ≥ 1418` | `n + 13` | +13 | v17:1418 是紧跟 window clamp 的那一空行/下一句 → v18:1431；尾部 `[work]` 原地改的 v17:3914 → v18:3927、v17:3926 → v18:3939 与 +13 一致 |

锚点已逐条核对（`sed -n` 取 v18 实际行文本，与 v17 同位置文本逐字相同或按上表的唯一改动变化）⇒ **没有一处使用偏移量盲推**。

## 3 v18 新增行清单（绝对行号，文档里引用新机制一律用这一列）

| v18 行 | 内容 | 被谁引用 |
|---:|---|---|
| 1153 | `#ifndef EXTC_RC_MARK_COLD_BYTES` | `MEMORY-MODEL-CURRENT.md` §17.2（宏表）、`RECLAMATION.md` 契约段 |
| 1154 | `#define EXTC_RC_MARK_COLD_BYTES (16 << 20)` —— 冷档活集门槛 | 同上 |
| 1155 | 续行注释：过去它就是一次访存 | — |
| 1156 | `#endif` | — |
| 1157 | `#ifndef EXTC_RC_MARK_COLD_BATCH` | §17.2 |
| 1158 | `#define EXTC_RC_MARK_COLD_BATCH 128` —— 冷档每步 pop 上限 | §17.2、`GC-ROOFLINE.md` §6.8.2 |
| 1159 | 续行注释：标定形状活集 450 MB–1.25 GB，一次 pop 约 5× 暖走 | — |
| 1160 | `#endif` | — |
| 1161-1163 | `#if COLD_BATCH > MARK_BATCH_MAX` / `#error "EXTC_RC_MARK_COLD_BATCH exceeds EXTC_RC_MARK_BATCH_MAX"` / `#endif` | 契约「只收紧」的编译期证据 |
| 1189 | `static long long extc_rc_markColdN;` | `[work] cold_steps=` 的来源 |
| 1430 | `if (r->live > (int64_t)EXTC_RC_MARK_COLD_BYTES && b > (size_t)EXTC_RC_MARK_COLD_BATCH) { b = …; extc_rc_markColdN++; }` | `GC-ROOFLINE.md` §6.8.2 的 clamp 原文 |
| 3927 | `[work]` 格式串（尾部 `cold_steps=%lld`）| `RECLAMATION.md` 判据段 |
| 3939 | `[work]` 实参表（尾部 `, extc_rc_markColdN`）| 同上 |

配套量具（不在 `reclaim.c` 里，无行号问题）：`tools/check_mark_batch.py` 新增 6 条静态断言（`cold tier defined` / `cold tier clamp` / `cold tier inside advance` / `cold tier only lowers` / `cold tier reported`）+ 4 条红自测 token，`--selftest` 共 **31 条**全过。

## 4 使用规则（下一轮的人必须照做）

1. `docs/analysis/MEMORY-MODEL-CURRENT.md` 与 `docs/analysis/GC-ROOFLINE.md` 里既存的 **1,657 处** `reclaim.c:NNNN` 仍按 **v17** 写，本文**不逐条重指**（纯插入 ⇒ 按 §2 换算即等价，逐条重指只是把 1,657 次机械操作的风险再买一遍）。新增内容一律写 **v18 绝对行号**并标 `(v18)`。
2. 换算前先 `grep` 一遍符号（例：`grep -n "extc_rc_markAdvance" src/back/reclaim.c`）。若符号所在行与 §2 换算不符 ⇒ **源码已经改过，本文过期**，必须重做 REFS，不许拿旧表换算。
3. 下一轮一旦发生非插入改动（删行/移动/函数体改写），本文的「恒等映射」前提失效 ⇒ 重做一张 REFS-V19.md，回到 `REFS-V7.md` 的逐条语义重指方法。

## 5 已知遗留（与本轮无关，但会被行号问题撞上）

- `tests/gc-p0/bseg-cal.tsv` 的 `src_md5` 仍是 v9 口径（`reclaim.c=3de56554…`），门禁每次告警「必须重标定」；(b) 通道因此对 cyc/pb/pc/pd 记 N/A。属下一轮量具协议项。
- `τ_t^cold` 未单独标定：模型仍用单一 17.49 ns/obj 给上界（冷档真实每 pop 成本更高），所以 §6.8 的 `P_step` 上界是保守的。
