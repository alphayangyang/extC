# REFS-CODEGEN-V4 — 两份交付文档中 codegen.c 引用的语义重建

FREEZE v4 口径：src/back/codegen.c md5 cf2885440981b5c9d154927fb61441b9、15994 行；src/back/reclaim.c md5 ff42799bfa62c2c9b0ac5b238e7d05e9、3607 行。本文所有 v4 行号均指这两个文件/版本。

方法（语义对齐，不偏移）：逐条读文档原句 → 在 v4 源码里找同一语义的函数/注释/发射点 → 用 sed 摘录做证据。**不要**用"旧行号 + 差值"批量重指：v4 在 9890-10150、12376-12745、14040-14130、15013-15135 都有大段插入/重排，偏移量逐段不同。
置信度：high = 摘录直接命中同一语义；medium = 语义等价但 v4 合并/重排了载体（注释换了位置、一个函数拆成两段）；low = 只能推断。stale = 该语义在 v4 已不存在或被取代。

文件：docs/analysis/MEMORY-MODEL-CURRENT.md（980 行版）、docs/analysis/GC-ROOFLINE.md（803 行版）。

## 1. MEMORY-MODEL-CURRENT.md

| 文档行 | 旧引用 | 原文引用（截断） | 语义指向 | v4 正确行号 | 依据（sed 摘录 ≤80 字符） | 置信度 |
|---|---|---|---|---|---|---|
| 49, 196, 203 | codegen.c:12151 | 表 1-1 帧 arena 的 extc_arena_alloc | arena bump 分配器本体（发射文本） | 12408 | { PRIM_FUNC, "extc_arena_alloc", ... } | high |
| 50, 926 | codegen.c:4618 | needReclaim 为真时一律 rc | 单值 new T 的 rc 分支入口 | 4681 | if (g->needReclaim) {（exNew 单值分支内） | high |
| 50 | codegen.c:4633 | 定长 new [N]T 也 rc | new T[n] 的 rc 分支判据（仅 level==1） | 4716 | if (g->needReclaim && planArenaLevel(e) == 1) { | high |
| 50, 123, 926 | codegen.c:4653-4667 | alloc<T>(n)/new T[n] 的 data 仅层 1 | 数组 rc 分支 + alloc<T>(n) rc 分支 | 4716-4731；4831-4843 | 4716 planArenaLevel(e) == 1；4831 if (g->needReclaim) { | high |
| 97 | codegen.c:4622-4640 | RECLAIM=0 分配点改纯 bump | 无回收时的 arena 回退（单值/数组各一处） | 4695-4697；4732-4735 | "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), ..." | high |
| 99, 591, 729 | codegen.c:13810-13813 | SWEEPFREE=1 同时把 rcNoCount=0 | 强制事件档的三态解析 | 14115-14123 | if (sf && *sf) g->rcSweepFree = (strcmp(sf, "0") == 0) ? -1 : 1; | high |
| 99 | codegen.c:13796-13813 | 三态解析 | 同上（含注释与赋值） | 14105-14123 | /* Three states: unset = decided by analysis ... */ | high |
| 113, 322 | codegen.c:13906 | single object is an rc slot | rcAllocInExpr 的单值判据 + 注释 | 14215 | if (!e->u.new_.count) return false;   /* a single object is an rc slot */ | high |
| 123 | codegen.c:4632-4634 | 档 0 改走 extc_arena_alloc | 同 97 行：单值无回收回退 | 4695-4697 | "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), ..." | high |
| 163 | codegen.c:9619 | maxLv = 1 + blkMaxOfBlock | 帧层级数 | 9874 | int maxLv = 1 + blkMaxOfBlock(f->body); | high |
| 164 | codegen.c:9674 | extc_arena __extc_a[maxLv+1] | 帧 arena 数组声明 | 9931 | cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1); | high |
| 165 | codegen.c:9622-9631 | levelCap = maxLv + 2 的生成器表 | 逐层表（含 taskRootAt）分配 | 9877-9888 | g->levelCap = maxLv + 2; ... vecInit(&g->taskRootAt[lv], ...); | high |
| 166 | codegen.c:9654-9658 | 无 new 则 noArena | noArena 判据与协程覆盖 | 9911-9915 | g->noArena = !planMayUseArena(f) && !(...); if (g->coroFunc) g->noArena = true; | high |
| 166 | codegen.c:9636-9641 | planMayUseArena 注释 | 不发射数组的理由注释 | 9889-9893 | * functions in real programs are pure computation. cgReleaseLevel skips the | medium |
| 176 | codegen.c:579, 3487, 3587 | 每帧 __extc_home；main 的 home 是自己的 a[1] | arena 拼写函数与 main 的 home 赋值 | 5955-5975；10056；3637-3638 | if (level == ARENA_HOME) return "(*__extc_home)";；extc_arena *__extc_home = &__extc_a[1]; | medium |
| 177, 348 | codegen.c:4096-4181 | 根表 >4096 不发射触发点并标 rcRootsUnreliable | >4096 检查（含 trigger 跳过与 rcRootLeaves 溢出） | 4135-4160；4191-4200；4352-4387 | if (g->rcGlobalRoots.len + g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; return; } | high |
| 185 | codegen.c:12119 | 生成物里的 PRIM_VAR 类型 | extc_ablock / extc_arena / ExtcAny 定义 | 12376-12382 | { PRIM_VAR, "extc_ablock", "typedef struct extc_ablock ... | high |
| 194 | codegen.c:12126-12128 | EXTC_ARENA_SPARE_MAX = 1 MiB | spare 上限宏 | 12383-12384 | { PRIM_MACRO, NULL, "#define EXTC_ARENA_SPARE_MAX (1 << 20)\n" } | high |
| 194 | codegen.c:12130 | release 时保留一块 spare | extc_arena_release 本体 | 12387 | { PRIM_FUNC, "extc_arena_release", ... if (!a->spare && a->top->cap <= EXTC_ARENA_SPARE_MAX) | high |
| 196, 203 | codegen.c:12151-12205 | 分配算法（倍增/floor 64/calloc/memset） | extc_arena_alloc 全文（单个 prim 字符串） | 12408 | int64_t cap = n > 64 ? n : 64; ... if (!a->top->fresh) memset(p, 0, (size_t)n); | high |
| 207 | codegen.c:6218 | cgReleaseLevel 在块进入/离开各发一次 release | cgReleaseLevel + cgArenaRelease + 进入点 | 6297-6311；6362-6371；6964, 6978-6981 | cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl); | high |
| 232 | codegen.c:12414-12420 | region 注册表 512 项 | REG_MAX 与静态表 | 12704-12708 | #define EXTC_RC_REG_MAX 512 / static void *extc_rc_regKey[...] | high |
| 234 | codegen.c:7317-7321 | 键 = 地址 + k，必须逐个 drop | 分片 drop 循环（键为基址+k） | 7494-7505 | for (int k = 0; k < g->rcNShard; k++) cgLine(... regionDrop((char *)&%s + %d) | high |
| 234, 685 | codegen.c:12424-12443 | regionOf/regionDrop 函数体 | 运行期查表/登记/搬尾 | 12713-12744 | for (size_t i = 0; i < extc_rc_regN; i++) if (extc_rc_regKey[i] == key) ... | high |
| 235 | codegen.c:7330-7348 | drop 只在帧销毁（cgReclaimDropArena） | cgReclaimDropArena 本体 | 7488-7506 | static void cgReclaimDropArena(CG *g, int lvl) { ... extc_rc_regionDrop((void *)&__extc_a[%d]) | high |
| 235 | codegen.c:7346-7347 | 分片循环 | 同一函数的 shard 循环 | 7504-7505 | for (int k = 0; k < g->rcNShard; k++) cgLine(...) | high |
| 296 | codegen.c:8620-8768 | 8 种屏障形态静态分派 | store 分派主体（refStore/valueStore 到兜底） | 8880-9050 | const bool refStore = g->needReclaim && !s->u.assign.op && cgIsRefType(...) | high |
| 311 | codegen.c:13876-13894 | 只有三处受治理创建点 | 三创建点块注释 | 14185-14203 | Reclamation governs heap storage at exactly three creation points | high |
| 322 | codegen.c:13906-13918 | rcAllocInExpr 逐字 | rcAllocInExpr 本体 | 14211-14225 | static bool rcAllocInExpr(void *ctx, Expr *e) { ... | high |
| 347 | codegen.c:3936-3987 | 根表就地拼表 | rcRootTableText 的文档与实现 | 4001-4050 | The table is just the **reference-slot expressions** ... (void *[]){ ... } | high |
| 348 | codegen.c:7092 | rcRootsUnreliable 连定向释放也禁用 | 定向释放的前置拒绝 | 7250 | if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false; | high |
| 390 | codegen.c:3837-3865 | rcFrameOwnsArena 才发触发点 | rcFrameOwnsArena 本体 | 3924-3960 | static bool rcFrameOwnsArena(CG *g, int lvl) { | high |
| 391 | codegen.c:4019, 4080 | 属主帧在 checkOwed 结算欠账 | checkOwed 发射 + owe 记账 | 4143-4156；4078-4098 | if (%sextc_rc_owed(%s)) extc_rc_checkOwed(%s, %s, %zu); | high |
| 408 | codegen.c:12392-12396 | [rt:chain] 无发射器，frameRoots 恒传 0 | 发射 prologue 处的 0 与注释 | 12677-12682 | The frame root chain ... has no emitter ... passing 0 emits nothing. | high |
| 482 | codegen.c:7045-7124 | storeRefOwnedRelease 的四条 criterion | 定向释放函数的判据（rcEmitStoreRelease） | 7207-7281 | if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false; ... 4 条 | high |
| 482 | codegen.c:7121 | 发射点 | storeRefOwnedRelease 的 cgLine | 7279 | cgLine(g, "extc_rc_storeRefOwnedRelease((void **)&(%s), ...", ...) | high |
| 587, 728 | codegen.c:13770 | rcNoCount=1（Default 1 = no counting） | 无计数档默认值 | 14071 | g->rcNoCount = 1; | high |
| 591 | codegen.c:13792-13795 | NOCOUNT=0 恢复计数 | 环境变量解析 | 14101-14104 | if (nc && *nc && strcmp(nc, "0") == 0) g->rcNoCount = 0; | high |
| 611 | codegen.c:7676, 7678 | planRcNoDec 选 initRefNoHold | 初始化屏障的 NoHold/计数分支 | 7853-7856 | if (planRcNoDec(g->curFunc) && g->rcNoCount) ... extc_rc_initRefNoHold | high |
| 649 | codegen.c:3540-3612 | 视图不是根、不是 rc 槽 | 视图非根注释 | 3659-3662 | A view (slice<T> / mut slice<T>) is not a root | high |
| 662 | codegen.c:9654, 9658 | coroFunc 强制 noArena | noArena 判据与协程覆盖 | 9911, 9915 | if (g->coroFunc) g->noArena = true; | high |
| 662 | codegen.c:9635-9653 | cgReleaseLevel 注释与 @overwrite 站点收集 | cgReleaseLevel + collectOwSites | 6297-6311；9897-9911 | cgCoroTaskEndAt(g, lvl, g->atBlockEnd); ... cgZoneLeave(g, lvl); | medium |
| 662 | codegen.c:517-531 | coroFunc/noArena 与 rcRootsUnreliable 注释 | 结构体字段注释 + 真实覆盖逻辑 | 249-252；9912-9915 | /* Coroutine lowering. ... */ const FuncDef *coroFunc; | medium |
| 662 | codegen.c:7408, 7449 | 协程帧真实置 rcRootsUnreliable | 两处置位（boxed handle / 栈帧 spawn） | 7577, 7622 | g->rcRootsUnreliable = true; | medium |
| 662 | codegen.c:4287-4324 | rcRootLeaves 的 4096 溢出点 | 根叶子枚举的溢出分支 | 4371-4387 | if (g->rcRoots.len > 4096) { g->rcRootsUnreliable = true; | medium |
| 664 | codegen.c:14660-14790 | parallel worker 运行期 | 并行运行期文本（par_open/worker/run） | 15013-15135 | EXTC_UNUSED static extc_par_region *extc_par_open(int64_t workers) { | high |
| 666 | codegen.c:14743-14745 | region 模式下 worker 自己不销毁 | worker 尾部的 regionDrop 位置注释 | 15093-15097 | extc_rc_regionDrop((void *)&local); /* the reclamation region lives as long as this arena */ | high |
| 666 | codegen.c:14718-14733 | 统一由 par_close 做 | extc_par_close 本体 | 15027-15042 | EXTC_UNUSED static void extc_par_close(extc_par_region *r) { | high |
| 667 | codegen.c:14758-14766 | worker 保存并清零 tlsDepth | worker 进入时保存/清零 | 15067-15075 | int32_t __extc_depth = extc_rc_tlsDepth; extc_rc_tlsDepth = 0; | high |
| 667 | codegen.c:14770-14772 | 尾声恢复 | worker 调用后恢复 | 15079-15081 | extc_rc_tlsDepth = __extc_depth; | high |
| 668 | codegen.c:4222-4230 | TLS owner 运行期三分支 | RC_OWN_TLS 的 if/else 发射 | 4283-4293 | pfLine(g, "if (__extc_tlsOwner) {"); ... rcEmitOweNow(g, basePtr, lvl); | high |
| 685 | codegen.c:14706-14714 | par_open 只 calloc 结构体+arenas | extc_par_open 本体 | 15015-15024 | extc_par_region *r = calloc(1, sizeof *r + (size_t)workers * sizeof(extc_arena)); | high |
| 685 | codegen.c:12423-12433 | region 惰性建立（regionOf miss） | regionOf 内 regionNew 分支 | 12713-12734 | ExtcRcRegion *r = extc_rc_regionNew(rid); if (extc_rc_regN < EXTC_RC_REG_MAX) ... | high |
| 700, 732 | codegen.c:12395 | EXTC_RC_TIME（getenv != NULL 即开） | 运行时文本的 timing 参数 | 12680 | reclaimEmitRuntime(..., getenv("EXTC_RC_TIME") != NULL, ...) | high |
| 713 | codegen.c:4174 | garbageHint 仅多 shard 模式调用 | 分片触发点的 garbageHint | 4237 | int64_t %s = extc_rc_garbageHint(%s); | high |
| 727 | codegen.c:13761-13763 | EXTC_RECLAIM 开/关 | 环境变量解析与 needReclaim 初值 | 14057-14065 | g->needReclaim = !(rcEnv && *rcEnv && strcmp(rcEnv, "0") == 0); | high |
| 728, 924 | codegen.c:15604-15612 | 三态与自动升级 | 自动升级判据（unitAllAcyclic） | 15913-15921 | if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) { rcSweepFree = 1; rcNoCount = 0; } | high |
| 730 | codegen.c:13778-13786 | EXTC_RC_LOCALSTORE kill switch | 环境变量解析 | 14079-14090 | const char *ls = getenv("EXTC_RC_LOCALSTORE"); | high |
| 731 | codegen.c:13764-13777 | EXTC_RC_SHARDS 上限 64 | 分片数解析 | 14065-14078 | if (v > 1) g->rcNShard = (int)(v > 64 ? 64 : v); | high |
| 731, 953 | codegen.c:4172, 4153-4172 | floor 按 N 除 | 每 shard floor 除法（注释+发射） | 4216-4239 | extc_rc_regionFloor(%s, (EXTC_RC_FLOOR) / %d); | high |
| 736 | codegen.c:613 | EXTC_TASK_ROOTS 字段 | 字段注释 + 默认赋值 | 613；14091 | g->taskRoots = 1;（env 覆盖在 14088-14094） | high |
| 774, 824 | codegen.c:12704 | EXTC_RC_REG_MAX = 512 | 宏定义 | 12704 | #define EXTC_RC_REG_MAX 512 | high |
| 775 | codegen.c:12384 | EXTC_ARENA_SPARE_MAX = 1 MiB | 宏定义 | 12384 | #define EXTC_ARENA_SPARE_MAX (1 << 20) | high |
| 924 | codegen.c:13962-14031 | 环境开关总段 | 开关解析段整体 | 14040-14125 | g->rcNShard = 1; ... EXTC_DBG_RCCOST | high |
| 925 | codegen.c:8709-8768 | 每次引用赋值都发 storeRef* | store 分派段 | 8929-9050 | if (hostUnknown) { ... extc_rc_storeRefSlot ... | high |
| 926 | codegen.c:4670 | new T 默认走 extc_rc_alloc | 单值 rc 分配的 core 调用 | 4691-4692 | const char *core = rcAllocCore(g, base, lvl, rcOwnKind(g, lvl), ...); | high |
| 928 | codegen.c:8496-8525 | ST_TRAP 不发 drop | trap 发射分支 | 8717-8745 | case ST_TRAP: ... extc_trapMsg(...) | high |
| 929 | codegen.c:9810-9813 | atexit(extc_drop_globals) | atexit 发射 | 10065-10070 | if (isMain && g->needGlobalDrops && !g->needModuleTramp) cgLine(g, "atexit(extc_drop_globals);"); | high |
| 929 | codegen.c:13543-13555 | extc_drop_globals 本体 | emitGlobalDrops | 13844-13852 | static void extc_drop_globals(void) { ... genDropValue ... | high |
| 941 | codegen.c:8948 | P0-7 边界注释 | 参数槽 insert-only 分支注释 | 8958 | if (!g->rcNoCount && cgTargetIsParamSlot(g, s->u.assign.target)) { | high |
| 936 | codegen.c 零调用（无行号） | rootPush/rootPop 全仓无发射器 | **已被 P0-6 取代**：现在有 4 个发射点 | 3037；7572；7591；7903 | cgTaskRootPush(g, "&%s.task")；cgTaskRootPush(g, "&__extc_czid%d") | stale |

### 1.1 关于第 936 行的 stale 说明（重要）

文档仍写着 extc_task_rootPush/rootPop 在 codegen.c 零调用、extc_taskRootN 恒 0、task_collect 的可达性保护完全失效。P0-6 落地后**前半段已不成立**：codegen.c 现在有四处登记发射（3037 exExt、7572 boxed handle、7591 栈帧 spawn、7903 handle 拷贝），并配有 break/continue/return/块尾的配平弹出；EXTC_TASK_ROOTS=0 可关。仍然成立的只有后半段：**extc_task_collect 在整个 src/ 仍无调用者**（coroutine.c 内定义，codegen 不发射），所以"登记"目前不改变运行期行为，只是为将来整池 collect 接线。

## 2. GC-ROOFLINE.md

| 文档行 | 旧引用 | 原文引用（截断） | 语义指向 | v4 正确行号 | 依据（sed 摘录 ≤80 字符） | 置信度 |
|---|---|---|---|---|---|---|
| 58 | 15913-15921 | 档位触发条件 | 自动升级判据 | 15913-15921（不变） | if (g.rcSweepFree != -1 && g.needReclaim && unitAllAcyclic(&g, m)) | high |
| 60 | 14271-14309, 14371-14373 | 档 0：unitReclaimIdle | 回收空闲判定与 needReclaim=false | 14271-14309, 14371-14373（不变） | static bool unitReclaimIdle(...)；needReclaim = false | high |
| 61 | 15607-15612 | 档 1：SWEEPFREE=1 或自动升级 | 同一升级判据（旧偏移） | 15913-15921 | rcSweepFree = 1; rcNoCount = 0; | high |
| 62 | 14071 | 档 2 默认 rcNoCount=1 | 默认值 | 14071（不变） | g->rcNoCount = 1; | high |
| 64 | 14310-14354 | 自动升级四条判据 | unitAllAcyclic | 14310-14354（不变） | static bool unitAllAcyclic(...) | high |
| 73 | 12383-12395 | arena spare 保留 | 宏 + release + destroy | 12383-12390 | extc_arena_release / extc_arena_destroy | high |
| 85 | 12699-12745 | region 注册表满则泄漏 | REG 文本段 | 12699-12745（不变） | EXTC_RC_REG_FULL_POLICY 1 ... | high |
| 434 | 12629 | 产物头 EXTC_RC_TIER | TIER 发射 | 12629（不变） | bufPrintf(out, "#define EXTC_RC_TIER %d ... | high |
| 437, 468 | 1266-1284 | genViewCopier 批量拷贝 | 函数整体（v4 变长，行号漂移） | 1281-1325；发射 1313-1318 | if (n > 0) memmove / extc_rc_storeStructCopy(...) | high |
| 467 | 8958, 10106, 10113 | 参数槽 insert-only 与参数根 | 三处均未移动 | 8958, 10106, 10113（不变） | cgTargetIsParamSlot ... rcRootDeclImpl(..., true) | high |
| 472 | 6179-6230 | P0-6 任务根登记 | cgTaskRootPush/Pop 等 | 6179-6311 | static void cgTaskRootPush(CG *g, const char *slotExpr) | high |
| 472 | 7526, 7545, 7857 | P0-6 发射点（旧） | 三个真实发射点（v4 另有 handle 拷贝点） | 7572, 7591, 7903 | cgTaskRootPush(g, ...) | high |
| 759 | 14211 | 分配点 rc 判定 | rcAllocInExpr | 14211（不变） | static bool rcAllocInExpr(void *ctx, Expr *e) | high |
| 760 | 14310 | acyclic 单元判定 | unitAllAcyclic | 14310（不变） | static bool unitAllAcyclic(...) | high |
| 761 | 14271 | 回收空闲单元判定 | unitReclaimIdle | 14271（不变） | static bool unitReclaimIdle(...) | high |
| 766 | 8691 | 参数槽识别 | cgTargetIsParamSlot | 8691（不变） | static bool cgTargetIsParamSlot(CG *g, Expr *t) | high |
| 767 | 1318 | bulk copy 记账发射 | storeStructCopy 发射行 | 1318（不变） | extc_rc_storeStructCopy((void *)&d.data[__extc_bk], ...) | high |
| 768 | 12629 | TIER 宏发射 | 同 434 | 12629（不变） | bufPrintf(out, "#define EXTC_RC_TIER %d ... | high |
| 769 | 12704 | region 表满文本 | REG_MAX | 12704（不变） | #define EXTC_RC_REG_MAX 512 | high |
| 770 | 6218 | 任务根 push 发射 | cgTaskRootPush 定义行 | 6218（不变） | static void cgTaskRootPush(CG *g, const char *slotExpr) | high |
| 771 | 15918 | 档位自动升级判据 | 升级块内 | 15918（不变） | g.rcSweepFree = 1; g.rcNoCount = 0; | high |
| 775 | 12713 | region 身份表 | regionOf | 12713（不变） | static inline ExtcRcRegion *extc_rc_regionOf(void *key, int32_t rid) | high |

结论：GC-ROOFLINE.md 的 20 处引用绝大多数本来就是 v4 行号（它是在 v4 上写的）；只有第 61 行（15607-15612）需要改成 15913-15921，第 437/468 行（1266-1284）要跟着 genViewCopier 的新位置改成 1281-1325，第 472 行的三个发射点要改成 7572/7591/7903（并补 3037）。

## 3. 复核命令

    sed -n '4681p;4716p;4831p' src/back/codegen.c
    sed -n '9874p;9931p;9911p;9915p' src/back/codegen.c
    sed -n '12408p' src/back/codegen.c | cut -c1-120
    sed -n '12699,12745p' src/back/codegen.c | head -6
    sed -n '14057,14123p' src/back/codegen.c
    sed -n '15913,15921p' src/back/codegen.c
    md5sum src/back/codegen.c src/back/reclaim.c

## 4. 存疑（medium/low 项汇总）

- 176 行：旧的 579/3487/3587 是分散的 __extc_home 注释；v4 的权威载体是 5955-5975（arenaRefAt）、10056（main 的赋值）与 3637-3638（owner 论证），一一对应关系无法从 v1 文本复原，只能按语义给三点。
- 662 行：517-531 的 coroFunc/noArena 注释在 v4 结构体里已压缩成 249-252，真正的强制逻辑在 9911-9915；该行是多引用复合，建议文档改写时只保留 v4 三处。
- 662 行：4287-4324 的"rcRootLeaves 4096 溢出"在 v4 的落点按语义是 4371-4387，但 v1 那段可能还含别的注释，故 medium。
- 234 行：7317-7321 的"键 = 地址 + k"注释在 v4 同时出现在 4090-4097（owe 分片）与 7494-7505（drop 分片），文档原句指向 drop，故取后者。
- 936 行已标 stale：不要让文档继续声称"codegen 零调用 rootPush"；但也不要写成"任务根保护已完备"——task_collect 仍无调用者。
