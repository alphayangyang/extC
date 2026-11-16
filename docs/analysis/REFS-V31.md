# REFS-V31：v30 → v31 源码行号映射与绝对锚点

本文件把文档里引用的源码行号从 v30 口径换到 v31 口径。**v31 只改 `src/back/reclaim.c` 一个文件**，而且全部改动都落在它的发射文本 `RECLAIM_RT[]`（生成 C 的那份字符串数组）里，`src/back/codegen.c`、`tests/reclaim/driver.c` 语义行号未变。

## 1. 指纹

| 文件 | v30（HEAD `853e46d0`） | v31（本轮工作树） |
|---|---|---|
| `src/back/reclaim.c` | `845c57e2b230202efcd04639e07664d9` / 4,738 行 | `29a6372eb0fac935f215847df35ccf41` / **4,780 行**（+42） |
| `src/back/codegen.c` | `ec12011cc5bbff7100072bb465d8f234` / 16,346 行 | 同左（未改） |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` / 1,637 行 | 同左（未改） |
| `build/extc` | `f5ed0d066b084f7f13024b9f4f14950c` | `f82ee508c6fe7233f228fc0decc00b9c` |

基线方法：`git worktree add -f /tmp/s4/basewt HEAD` 后在树内 `make BUILDDIR=build-b build-b/extc -j8` ⇒ `/tmp/s4/basewt/build-b/extc` =
`a70616b370305b203940eb7361fb0dda`（同一源码在不同 `BUILDDIR` 下编出的 `extc` md5 不同，溯源只认源码 md5；在 /tmp 下运行它需要 `EXTC_STD=/tmp/s4/basewt/stdlib`）。
v31 的源码有 12 个 hunk（见 §2），`build/extc` 由 `make build/extc` 从干净对象重建；同一份源码重建两次得到**同一 md5**（`f82ee508…`）⇒ 该指纹可复现。

## 2. 变化块（`git diff -U0` 逐 hunk，12 块）

| # | v30 行 | v31 行 | 性质 | 内容 |
|---|---|---|---|---|
| 1 | 287 后 | 288–290 | **新增 3 行** | `ExtcRcSlab` 加 `uint32_t deadA;` / `uint8_t deadN;` 两个字段（死巨块复用宽限的年龄） |
| 2 | 883 后 | 887–888 | **新增 2 行** | `extc_rc_chanClock` / `extc_rc_chanNote` 的前置声明（chan 钩子定义在 1469/1500，晚于 slabNew） |
| 3 | 894 | 899–901 | 替代（1→3） | `[rt:hugefree]` 契约注释：丢弃与请求同阶、记入 `[chan] bigdrop` |
| 4 | 984 后 | 992–1001 | **新增 10 行** | `slabNew` 的巨块分支：申请载荷**之前**先按本次请求字节 `extc_rc_bigTrimStep(slotSize)` 丢死载荷，并记 chan |
| 5 | 1048 后 | 1066 | **新增 1 行** | `slabNew` 里 `s->deadA = 0; s->deadN = 0;` 初始化 |
| 6 | 1465 | 1483 | 替代（1→1） | `EXTC_RC_CHAN_N` 3 → **4** |
| 7 | 1468 | 1486 | 替代（1→1） | chan 的 work 单位注释加 dropped bytes |
| 8 | 2070 | 2088–2090 | 替代（1→3） | `extc_rc_slabShrink` 安全注释：巨块与普通块同权，空巨块载荷进 drop 链 |
| 9 | 2072 | 2092–2106 | 替代（1→15） | 宏块改写：`EXTC_RC_SLAB_KEEP 0`、`EXTC_RC_SLAB_KEEP_BIG 0x7fffffff`、`EXTC_RC_SHRINK_GRACE 1`、`EXTC_RC_SHRINK_CAP_LIVE 3` 及注释 |
| 10 | 2087 | 2121–2125 | 替代（1→5） | `skip:shrink-budget` 早退（容量闸门）+ `keep/keepBig` 局部量 |
| 11 | 2092–2093 | 2130–2135 | 替代（2→6） | 宽限分支：alive 清年龄、`deadA != allocN+1` 判「没被复用」、`deadN` 数宽限次数 |
| 12 | 4368 | 4410 | 替代（1→1） | `[chan]` 名表加 `"bigdrop"` |

## 3. 分段换算表（v30 n → v31）

| v30 行范围 | open 端映射 | offset |
|---|---|---|
| 1–287 | 同号 | 0 |
| 288–883 | n + 3 | +3 |
| 884–893 | n + 5 | +5 |
| 894 | 899–901（替代） | — |
| 895–984 | n + 7 | +7 |
| 985–1048 | n + 17 | +17 |
| 1049–1464 | n + 18 | +18 |
| 1465 | 1483（替代） | — |
| 1466–1467 | n + 18 | +18 |
| 1468 | 1486（替代） | — |
| 1469–2069 | n + 18 | +18 |
| 2070 | 2088–2090（替代） | — |
| 2071 | n + 20 | +20 |
| 2072 | 2092–2106（替代） | — |
| 2073–2086 | n + 34 | +34 |
| 2087 | 2121–2125（替代） | — |
| 2088–2091 | n + 38 | +38 |
| 2092–2093 | 2130–2135（替代） | — |
| 2094–4367 | n + 42 | +42 |
| 4368 | 4410（替代） | — |
| 4369–4738 | n + 42 | +42 |

核对样例（旧 → 新）：287→287、288→291、883→886、884→889、894→899、895→902、984→991、985→1002、1048→1065、1049→1067、1465→1483、1468→1486、2070→2088、2071→2091、2072→2092、2073→2107、2086→2120、2087→2121、2088→2126、2092→2130、2094→2136、4368→4410、4738→4780。

**规则（与 REFS-V23/V24/V28/V29/V30 一致）**：禁止用单一 offset 平移旧引用；先查本表所在分段；落在「替代」行的旧行号
要看新范围的语义，不要直接 +43。`docs/analysis/GC-ROOFLINE.md` 与 `docs/analysis/MEMORY-MODEL-CURRENT.md` 里已有的
上千处旧引用**不逐条重指**，只有本轮新增内容用当日绝对行号并标版本（v31）。

## 4. v31 绝对锚点（`src/back/reclaim.c`，本轮工作树，已逐条 grep 核过）

| 行 | 内容 |
|---|---|
| 288 | `uint32_t deadA;` —— allocN at the closing that last saw this block empty；0 = alive at that closing |
| 289 | `uint8_t deadN;` —— empty closings in a row with no reuse since；bounded by `EXTC_RC_SHRINK_GRACE` |
| 887–888 | `extc_rc_chanClock` / `extc_rc_chanNote` 前置声明 |
| 900 | `[rt:hugefree]` 契约注释：一口丢弃是 proportional to the request … reported as the `[chan] bigdrop` |
| 996–999 | `slabNew` 巨块分支的四行：`_bd0 = extc_rc_chanClock()`、`_bdw = extc_rc_bdBytes`、`extc_rc_bigTrimStep(slotSize)`、`extc_rc_chanNote(3, …)` |
| 1002 | `s->data = extc_rc_bigPayloadAlloc(slotSize);`（丢弃发生在它**之前**） |
| 1066 | `s->deadA = 0; s->deadN = 0;` 初始化 |
| 1483 | `#define EXTC_RC_CHAN_N 4` |
| 1500 | `extc_rc_chanNote` 定义 |
| 2092 | `#define EXTC_RC_SLAB_KEEP 0` |
| 2094–2095 | `EXTC_RC_SLAB_KEEP_BIG 0x7fffffff`（计数不是界，界是宽限判据 + 容量闸门） |
| 2100 | `#define EXTC_RC_SHRINK_GRACE 1` |
| 2104–2105 | `EXTC_RC_SHRINK_CAP_LIVE 3` |
| 2121 | `if (EXTC_RC_SHRINK_CAP_LIVE > 0 && r->capBytes <= r->bytes * …) return 0;   /* skip:shrink-budget */` |
| 2122 | `long long keep = EXTC_RC_SLAB_KEEP, keepBig = EXTC_RC_SLAB_KEEP_BIG, rel = 0, seen = 0;` |
| 2130 | alive 分支：`if (s->nLive != 0 \|\| s == r->bumpSlab) { s->deadA = 0; s->deadN = 0; s = n; continue; }` |
| 2132 | 第一次见到空 / 期间被复用：`if (s->deadA == 0 \|\| s->deadA != s->allocN + 1) { s->deadA = s->allocN + 1; s->deadN = 0; keepBig--; … }` |
| 2133 | 宽限计数：`if (s->deadN < (uint8_t)EXTC_RC_SHRINK_GRACE) { s->deadN++; keepBig--; … }` |
| 4410 | `static const char *const _chn[EXTC_RC_CHAN_N] = { "regionfree", "release", "trigger-step", "bigdrop" };` |

## 5. 生成 C 侧的核对方法

发射文本改动一律用「生成 C 再读」来核，不要只读 `reclaim.c` 的字符串数组（预处理指令写成裸行会失效，历史上踩过两次）：

```
./build/extc tests/gc-torture/src/t12_overwrite_arena.extc -o /tmp/v31/t12.c
grep -n 'bigdrop\|deadA\|deadN\|SHRINK_CAP_LIVE\|BIG_DROP_CAP' /tmp/v31/t12.c | head
cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L /tmp/v31/t12.c -o /tmp/v31/t12
EXTC_RC_PAUSE=1 taskset -c 6 /tmp/v31/t12 20     # 看 [chan] bigdrop 行
```

注意 `[chan]` / `[step]` / `[work]` 等行由 `EXTC_RC_PAUSE=1` **环境变量**打开（`extc_rc_pauseOn` 读 env），不只是编译宏。
