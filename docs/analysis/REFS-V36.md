# REFS-V36：v35 → v36 的行号映射（R3：载荷预留与原地长）

> 用途：本树的行号引用只有一个有效口径。**v36 之前**的行号（v17/v18/v21 等历史口径）一律按各自的 REFS 表换算；**v36 之后**新增内容用「v36 绝对行号 + 版本标记」。禁止把旧行号按单一 offset 机械平移（历史上机械平移曾把 `thrUpd` 指到 `markResume`）。核对一律按符号 grep，再回表确认。

## 0. 口径

| 项 | v35（HEAD 38c05208） | v36（本轮，R3） |
| --- | --- | --- |
| `src/back/reclaim.c`（物理） | `b05bbde438d39f3254dc5ea5e8b8d220` / **242 行** | 同左（**物理文件未改**：它是数组头 + 25 条 `#include "rt/*.inc"`） |
| 渲染文本（`tools/source_split.py` 拼出的发射文本） | **5,050 行** / 374,981 字符 / `1dcea37a8ea7858a350035507b7364b1` | **5,123 行** / 380,449 字符 / `b8d11ef09d0eeb1cec19d65192d29594`（净 **+73** 行） |
| `src/back/codegen.c` | `6ba135e22e86ec89d5cd8aa416a994bc` / 75 行（渲染 16,429 行 / `aaaaf36f…`） | 同左（本轮未改） |
| `tests/reclaim/driver.c` | `b82c9c83270d859ed51418e2ecb96d05` / 1,655 行 | `a10fc463149af86ee86f8667fdf65a7f` / **1,706 行**（+51/−0） |
| `build/extc` | `82e1b3e2237a3fb961b7f80e1b647224` | `30bbfdc0e15f867d8f7a5e4353648e95` |

净 +73 行的来源：`ExtcRcSlab.bigMax` +3；slack 注释与三个宏 + 新签名 +21；载荷分配体（`max` 计算 + carve + `reserved`）+13；两处 `reserved` 回退 +2；`extc_rc_bigGrow` +34；`slabNew` 调用点 ±0（一行替换）。

## 1. 改动清单（按文件）

| 文件 | 增/删 | 内容 |
| --- | --- | --- |
| `src/back/rt/21-wmark.inc` | +3 | `struct ExtcRcSlab` 在 `rcp` 之后加 `size_t bigMax;`（`[rt:bigslack]`，巨大块可原地长到的上限；普通块 0） |
| `src/back/rt/33-hugefree.inc` | +74/−4 | `[rt:bigslack]` 注释块；`EXTC_RC_EXT_SLACK` / `_SLACK_MIN` / `_SLACK_MAX`；`extc_rc_bigPayloadAlloc(n, reserved)` 新签名与 `max` 计算；`extc_rc_bigGrow`；`slabNew` 调用点传 `&s->bigMax` |
| `tests/reclaim/driver.c` | +51 | include 前 `#define EXTC_RC_EXT_SLACK 16`；`t_bigslack()` 13 条断言；main 里在 `t_admissionReserve` 之后调用 |
| `docs/analysis/GC-ROOFLINE.md` | +87 | 新 §6.24（七小节 + 三张表 + 复现） |
| `docs/analysis/MEMORY-MODEL-CURRENT.md` | +50 | 头部 v36 行号口径 bullet；新 §19.19（五小节）；§22 第 12 条 |
| `docs/design/RECLAMATION.md` | +18 | 新「三点十一」（三句契约 + 判据） |

## 2. 渲染文本的分段换算（v35 n → v36）

| v35 行段 | v36 | 说明 |
| --- | --- | --- |
| 1–268 | 同号（+0） | |
| 269–1172 | **+3** | `bigMax` 三行插在 `rcp` 之后 |
| 1173 | **被替换 ⇒ 1176–1197** | 旧 `bigRound` 一行之后插入 slack 注释与三个宏、并改写 `bigPayloadAlloc` 签名 |
| 1174–1175 | **+24** | |
| 1176–1177 | **被替换 ⇒ 1200–1214** | `max` 计算与 carve 调用 |
| 1178 | **+37**（→1215） | |
| 1179–1182 | **+38** | 1216 是 `reserved = m` 回退（纯插入） |
| 1183–1198 | **+39** | 1221 是 `reserved = n` 回退（纯插入） |
| 1199–1254 | **+73** | 1238–1271 是 `extc_rc_bigGrow`（纯插入 34 行） |
| 1255 | **被替换 ⇒ 1328** | `slabNew` 调用点加 `&s->bigMax` |
| 1256–5050 | **+73** | |

自检：`5050 + 73 = 5123` ✓。分段边界都取在整行上，没有跨半行的插入。

## 3. v36 绝对锚点（渲染文本行号）

| 符号 | 行 |
| --- | --- |
| `ExtcRcSlab.bigMax` 字段（`[rt:bigslack]`） | 269–271 |
| `[rt:bigslack]` 机制注释块 | 1176–1187 |
| `EXTC_RC_EXT_SLACK` | 1188–1190 |
| `EXTC_RC_EXT_SLACK_MIN` | 1191–1193 |
| `EXTC_RC_EXT_SLACK_MAX` | 1194–1196 |
| `extc_rc_bigPayloadAlloc` 签名 | 1197 |
| `max` 计算（`#if EXTC_RC_EXT_SLACK > 0`） | 1201–1208 |
| carve 调用 `extc_rc_extAlloc(m, max, &com)` | 1209 |
| `reserved = m`（自身映射回退） | 1216 |
| `reserved = n`（`EXTC_RC_BIG_DROP_OK == 0` 回退） | 1221 |
| `extc_rc_bigGrow` 注释 | 1239–1245 |
| `extc_rc_bigGrow` 函数 | 1246–1271 |
| `skip:big-grow-notbig` | 1247 |
| `skip:big-grow-nogrow` | 1249 |
| `skip:big-grow-exhausted` | 1250 |
| `skip:big-grow-nomem` | 1256 |
| `extc_rc_bigTrimStep` | 1273 |
| `slabNew` 的 `bigPayloadAlloc(slotSize, &s->bigMax)` | 1328 |

## 4. 使用规则

1. **禁止单一 offset 平移**：本表的偏移量在 0、+3、+24、+37、+38、+39、+73 之间跳变，按行段查。
2. v35 之前的引用先查各自的 REFS（v31/v33/v34/v35），再套本表；v36 之后的新引用直接用绝对行号并标版本。
3. `tools/emitted_identity.py` 的基线在本轮重冻结（运行期文本确实变了）：`--check` 报 changed 的行必须全部落在容量归还/载荷预留两节内，否则停止提交。
4. 生成 C 的行号不是本表的口径（它由发射文本 + 程序的 codegen 共同决定）；要核对生成 C 请直接对生成文件 `grep -n`。
5. `src/back/reclaim.c` 的物理行号（242 行）与渲染文本行号**不是一回事**：`include` 列表的顺序决定渲染顺序，物理行号只在改数组头时才有意义。
