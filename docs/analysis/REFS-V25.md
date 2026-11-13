# REFS-V25 · v24 → v25 行号映射、变化块与绝对锚点

本文件是 v25（2026-11-14 工作树，R7 轮：**单步工作量由编译期常量封顶**）的行号口径。它只做一件事：让旧文档里按 v17/v18/v21/v22/v24 写下的 1,600 余处行号，能够被**按符号核对后换算**到 v25，而不是被机械平移。

## 0. 为什么禁止单一 offset 平移

`src/back/reclaim.c` 在 v24 → v25 之间是净 +152 行，但整文件**不存在**一个可用的全局偏移：第 340 行之前偏移为 0，最大的单点插入是第 858–941 行（84 行：83 行 `[rt:hugefree]` 发射块 + 1 行分节空行），末尾偏移停在 +152。任何『旧行号 + 常数』的机械重指，在 858 行附近都会指错一整段。R7 轮之前的一次机械平移曾把 `thrUpd` 指到 `markResume`，因此本目录的规则是：**旧引用不逐条重指，按符号检索 + 本文件的分段表换算**。

## 1. 指纹（本文件写定时的构建）

| 文件 | md5 | 行数 | 相对 v24 |
|---|---|---|---|
| `src/back/reclaim.c` | `0b91d96ee08ff5e2589136ca313aee4f` | **4,658** | +152 |
| `src/back/codegen.c` | `3ae6e499b6a2d64668ff1daf2eed26a2` | **16,222** | +10 |
| `tests/reclaim/driver.c` | `28dd4009ac7207f47e581a697c0d421f` | 1,637 | 0（未改） |
| `build/extc` | `11d39dee4a6481b9e003591ac4e57cc9` | — | — |

v24 基点（`git` HEAD = `ef712794`）：`src/back/reclaim.c` = `711d2b02ade155be40b52508cf784397`（4,506 行）、`src/back/codegen.c` = `cc01b5f4216ea5c8d9fde77c47c41f24`（16,212 行）。

溯源只认**源码 md5**：`build/extc` 的 md5 随 BUILDDIR 变化（同源码不同 BUILDDIR 会得到不同的 extc md5），不要用它判『是不是同一棵树』。

> 本文件曾在 4,657 行的中间构建上写过一版 37 块表，**已作废**：最终构建多出第 28 块（种子 push 记账从『每槽一次自增』改成『种子阶段结束时一次快照』），净行数从 +151 变成 +152，且 [work] 的 `push_max` 恢复为**种子阶段 push 数**的语义（`gate-work` 的 `SEED_PUSHES + 63` 界因此原样成立；整步 push 总量在 [wbudget] 的 `push_max`）。下表一律按最终构建重算。

## 2. 变化块（reclaim.c，共 38 处）

行号格式为『旧 v24 位置 → 新 v25 位置』；插入块用『旧 v24 的插入点』。

| # | v24 | v25 | 内容 |
|---|---|---|---|
| 1 | 341 前 | 341 | 新增 `int32_t scanDesc`（游标种类：0 = 保守词索引，1 = 描述符字段索引） |
| 2 | 832 | 833–835 | `EXTC_RC_FREE_DEADLINE_US` 100 → **40**（+2 行注释：一条 drain pass 本身就是一个停顿） |
| 3 | 838 | 841–843 | `EXTC_RC_FREE_HARD_DEADLINE_US` 1000 → **50**（+2 行注释：sweep 收尾会调它，一次 pass ≤ 50 µs + 一块） |
| 4 | 852 后 | 858–941 | 新增 `[rt:hugefree]` 整块（83 行 + 1 行分节空行）：`EXTC_RC_BIG_DROP_BYTES`、`madvise` 前向声明、`ExtcRcBigDrop` 记录链、`bigPayloadAlloc`/`bigPayloadRelease`/`bigTrimStep` |
| 5 | 869 | 958 | `slabNew` 的巨块载荷 `malloc(slotSize)` → `extc_rc_bigPayloadAlloc(slotSize)` |
| 6 | 1165 后 | 1255–1260 | 新增 `EXTC_RC_MARK_STEP_POP_MAX 512` 与 `EXTC_RC_MARK_PUSH_STEP 2048`（两对 ifndef/define/endif） |
| 7 | 1171–1172 | 1266–1268 | `EXTC_RC_MARK_COLD_BATCH` 128 → **32**（注释改写为冷 pop ≈ 1 µs 的预算论证） |
| 8 | 1175 后 | 1272–1277 | 新增 `EXTC_RC_MARK_STACK_SOFT 8192`（标记栈软上限） |
| 9 | 1272 后 | 1375–1383 | 新增 `extc_rc_wConsMax` 与 `extc_rc_popMax`/`pushMax`/`popsTot` 声明 + 注释（无钟量具） |
| 10 | 1274 | 1385 | `extc_rc_workNote` 签名加第 5 参 `size_t cons` |
| 11 | 1279 后 | 1391 | `workNote` 内更新 `extc_rc_wConsMax` |
| 12 | 1523 | 1635 | `fqDrainCore` 的 `free(s->data)` → `extc_rc_bigPayloadRelease(s->data, s->ss)` |
| 13 | 1534 后 | 1647 | `fqDrainAuto` 新增 `bigTrimStep(EXTC_RC_BIG_DROP_BYTES)`（每步 ≤ 512 KiB） |
| 14 | 1546 后 | 1660 | `freeQueueDrainAll` 新增 `bigTrimStep(1 << 30)`（唯一无界丢页，只在退出路径） |
| 15 | 3080 | 3194 | region 拆除路径的巨块 release |
| 16 | 3192 | 3306 | 轮开始清游标时补 `r->scanDesc = 0` |
| 17 | 3323–3324 | 3437–3442 | 保守扫描的额度守卫（`if (words > _scanLeft)` + 游标 + `_split`，+4 行注释；判据行在 3441） |
| 18 | 3326 后 | 3445 | `_scanLeft -= words`（宏体行，行尾是三重续行） |
| 19 | 3366–3370 | 3485–3490 | `markResume` 注释重写（5 → 6 行：双游标、不 pop、walked 计数） |
| 20 | 3373 | 3493–3509 | `markResume` 顶部新增：`s->region` 校验补 `scanDesc = 0`、`walked` 上移、游标取样 + `if (r->scanDesc)` 描述符续扫分支 |
| 21 | 3378 | — | 删除旧的 `size_t walked = 0;`（已上移到 3494） |
| 22 | 3388 | 3523–3525 | 保守词循环拆成 3 行（`EXTC_RC_MARK_CHILD` 单独一行） |
| 23 | 3390 | 3527 | 注释 time-capped segment → capped segment |
| 24 | 3393 | 3530 | `return en - st;` → `return walked;` |
| 25 | 3400 后 | 3538–3539 | markStep 新增 POP 硬上限钳制（1 行注释 + `budget = min(budget, MARK_STEP_POP_MAX)`） |
| 26 | 3403 | 3542–3545 | `pushes` 声明加 `seedPushes`（种子 push 单独计数）+ `_scanLeft` 声明（3 行含注释） |
| 27 | 3407–3408 | 3549–3552 | 续扫条件改为『栈深不超过软上限才把步给游标』（+2 行注释 + 缩进破格的 `seg = extc_rc_markResume(r);`） |
| 28 | 3462 后 | 3607 | 种子阶段结束处新增 `seedPushes = pushes;`（一次快照取代逐槽自增：[work] 的 push_max 恢复为种子界） |
| 29 | 3478 后 | 3624 | 浅栈路径新增 `size_t _spS = r->msp;` |
| 30 | 3490 后 | 3637 | 浅栈路径新增 `if (r->msp > _spS) pushes += ...` |
| 31 | 3517 后 | 3665–3666 | 批路新增 pushes 记账 + `if (pushes > MARK_PUSH_STEP) _split = 1` |
| 32 | 3588 | 3737 | `workNote` 调用改为传 `seedPushes`，并加第 5 实参 `SCAN_CHUNK - _scanLeft` |
| 33 | 3591 后 | 3741–3743 | 新增 pop/push 高水位与 `pops_tot` 累加（3 行，无钟） |
| 34 | 3645 | 3797 | abandon 路径补 `r->scanDesc = 0` |
| 35 | 4151 | 4303 | `[work]` 格式串尾部加 `cons_max=%lld` |
| 36 | 4163 | 4315 | `[work]` 实参加 `extc_rc_wConsMax` |
| 37 | 4168 | 4320 | `[wbudget]` 格式串尾部加 `pop_max`/`push_max`/`pops_tot` |
| 38 | 4170 | 4322 | `[wbudget]` 实参加三枚计数器 |

净变化：4,506 → **4,658** 行（+152）。第 28 块是最终构建相对中间构建唯一的新增块。

## 3. 分段换算表（39 段相等块）

用法：v24 行号落在左列区间 → 加 offset 得 v25 行号；**落在两个区间之间的行属于被改写/删除的行**，必须按第 2 节查。

| v24 | v25 | offset |
|---|---|---|
| 1–340 | 1–340 | +0 |
| 341–831 | 342–832 | +1 |
| 833–837 | 836–840 | +3 |
| 839–852 | 844–857 | +5 |
| 853–868 | 942–957 | +89 |
| 870–1165 | 959–1254 | +89 |
| 1166–1170 | 1261–1265 | +95 |
| 1173–1175 | 1269–1271 | +96 |
| 1176–1272 | 1278–1374 | +102 |
| 1273–1273 | 1384–1384 | +111 |
| 1275–1279 | 1386–1390 | +111 |
| 1280–1522 | 1392–1634 | +112 |
| 1524–1534 | 1636–1646 | +112 |
| 1535–1546 | 1648–1659 | +113 |
| 1547–3079 | 1661–3193 | +114 |
| 3081–3191 | 3195–3305 | +114 |
| 3193–3322 | 3307–3436 | +114 |
| 3325–3326 | 3443–3444 | +118 |
| 3327–3365 | 3446–3484 | +119 |
| 3371–3372 | 3491–3492 | +120 |
| 3374–3377 | 3510–3513 | +136 |
| 3379–3387 | 3514–3522 | +135 |
| 3389–3389 | 3526–3526 | +137 |
| 3391–3392 | 3528–3529 | +137 |
| 3394–3400 | 3531–3537 | +137 |
| 3401–3402 | 3540–3541 | +139 |
| 3404–3406 | 3546–3548 | +142 |
| 3409–3462 | 3553–3606 | +144 |
| 3463–3478 | 3608–3623 | +145 |
| 3479–3490 | 3625–3636 | +146 |
| 3491–3517 | 3638–3664 | +147 |
| 3518–3587 | 3667–3736 | +149 |
| 3589–3591 | 3738–3740 | +149 |
| 3592–3644 | 3744–3796 | +152 |
| 3646–4150 | 3798–4302 | +152 |
| 4152–4162 | 4304–4314 | +152 |
| 4164–4167 | 4316–4319 | +152 |
| 4169–4169 | 4321–4321 | +152 |
| 4171–4507 | 4323–4659 | +152 |

（末行 4507/4659 是文件末尾空行计数，实际行数为 4,506/4,658。）

## 4. v25 绝对锚点（全部用源文本核验过）

每一条都在写定前用 `grep` 对照过源文件，行号即 v25 的物理行号；引用新机制时**只用这一节**，不再换算。

| 行 | 锚点 |
|---|---|
| 341 | `int32_t scanDesc;` 字段（`struct ExtcRcRegion`，紧跟 `scanOff`） |
| 833 | `#define EXTC_RC_FREE_DEADLINE_US 40` |
| 841 | `#define EXTC_RC_FREE_HARD_DEADLINE_US 50` |
| 858–940 | `[rt:hugefree]` 发射块（首行注释 / 末行 `}`；941 是分节空行） |
| 869 | `#define EXTC_RC_BIG_DROP_BYTES (512 * 1024)` |
| 887 | `extc_rc_bigPayloadAlloc(size_t n)` 定义 |
| 899 | `extc_rc_bigPayloadRelease` 定义 |
| 913 | `extc_rc_bigTrimStep(size_t budget)` 定义 |
| 958 | `slabNew` 巨块载荷改 `bigPayloadAlloc` |
| 1256 | `EXTC_RC_MARK_STEP_POP_MAX 512` |
| 1259 | `EXTC_RC_MARK_PUSH_STEP 2048` |
| 1266 | `EXTC_RC_MARK_COLD_BATCH 32` |
| 1274 | `EXTC_RC_MARK_STACK_SOFT 8192` |
| 1298 | `extc_rc_markStep` 前向声明（定义在 3532） |
| 1378 | `static long long extc_rc_wConsMax;` |
| 1383 | `static long long extc_rc_popMax, extc_rc_pushMax, extc_rc_popsTot;` |
| 1385 | `extc_rc_workNote(..., size_t cons)` 定义 |
| 1391 | `wConsMax` 更新 |
| 1572 | 冷档 clamp（`extc_rc_markColdN++` 处） |
| 1635 | `fqDrainCore` 巨块 release |
| 1647 | `fqDrainAuto` 的 `bigTrimStep(EXTC_RC_BIG_DROP_BYTES)` |
| 1660 | 退出路径无界丢页 `bigTrimStep(1 << 30)` |
| 3194 | region 拆除路径巨块 release（与 1635 是同一行的两处调用） |
| 3306 | 轮开始清游标（含 `scanDesc = 0`） |
| 3437–3442 | 保守扫描额度守卫（3437 注释首行，**3441 = `if (words > _scanLeft)` 判据行**，3442 写游标 + `scanDesc = 0`） |
| 3445 | `_scanLeft -= words;` |
| 3484 | `extc_rc_markResume` 定义 |
| 3498 | `if (r->scanDesc)` 描述符续扫分支 |
| 3532 | `extc_rc_markStep` 定义 |
| 3539 | POP 硬上限钳制 |
| 3543 | `size_t _scanLeft = (size_t)EXTC_RC_SCAN_CHUNK;` |
| 3551–3552 | 续扫软上限条件（`if (r->scanObj && r->msp <= EXTC_RC_MARK_STACK_SOFT)`）与 `seg = extc_rc_markResume(r);` |
| 3607 | `seedPushes = pushes;`（种子阶段结束的快照：`[work]` 的 push 界就是它） |
| 3624 | `size_t _spS = r->msp;`（浅栈路径） |
| 3637 | 浅栈 pushes 记账 |
| 3665–3666 | 批路 pushes 记账与 `if (pushes > EXTC_RC_MARK_PUSH_STEP) _split = 1` |
| 3737 | `workNote` 调用（传 `seedPushes` + `SCAN_CHUNK - _scanLeft`） |
| 3741–3742 | pop / push 高水位（`extc_rc_popMax` / `extc_rc_pushMax`），3743 是 `popsTot` 累加 |
| 3797 | abandon 路径清 `scanDesc` |
| 4303 | `[work]` 打印（尾部 `cons_max`） |
| 4320 | `[wbudget]` 打印（尾部 `pop_max`/`push_max`/`pops_tot`） |

`src/back/codegen.c` 的锚点：**3888** = `if (offs.len > 32) return "((const ExtcRcType *)-1)";`（描述符扇出上限），**3619–3621** = 文件头对该上限的说明。

## 5. codegen.c 换算（2 处）

- v24 1–3618 → v25 同号（offset +0）。
- v24 3619–3620 → v25 3619–3621（注释 4096 → 32 语义）。
- v24 3621–3877 → v25 3622–3878（offset +1）。
- v24 3878 → v25 3879–3888（上限改 32 + 9 行注释）。
- v24 3879–16212 → v25 3889–16222（offset +10）。

## 6. 使用规则

1. 引用 v25 的机制：直接用第 4 节的绝对行号，并在文字里带符号名（`extc_rc_bigTrimStep`、`EXTC_RC_MARK_PUSH_STEP` 等），便于源码变动后重新定位。
2. 引用 v24 及更早的内容：先用第 3 节换算，再 `grep` 符号确认；换算结果与符号不符时，以符号为准并在文档里记一行偏差原因。
3. 换算不等于重指：既有文档里的旧行号**保持原样**（它们标着 v17/v18/v21/v22 口径），新写的内容只用当日绝对行号并标注版本。
4. 任何再改动 `src/back/reclaim.c` 的轮次，必须新建 `REFS-V26.md`（或更高），不得在本文件的表上叠加偏移。
