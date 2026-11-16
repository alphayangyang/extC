# REFS-E2：src/back/codegen.c 的行号换算（RcPlan）

> 本文件的表由 git diff -U0 生成，不是手写：每个变化块的旧行段、新行段、类型、所在函数（或匿名作用域）与首个新增行。

## 口径

* 旧树 = v31（HEAD 5fd00730 之前的工作树，src/back/codegen.c 16,346 行、md5 ec12011cc5bbff7100072bb465d8f234）。
* 新树 = E2 之后（本文档同批提交）：src/back/codegen.c 16,368 行、md5 02acf0b0ae84ece59fb2b70d604ddda7；build/extc 仍 24ce5d0ab3e817bc5f8ddedee6773b84（注释与工具改动不改二进制）。生成 C 逐字不变是验收：python3 tools/emitted_identity.py --check 覆盖 215 个源 × 2 档、430 行基线。
* 变化只有两类：七个决策字段从 CG 搬进 struct RcPlan（读写点改成 g->rc.*），决策入口 scanUnitReclaim 改名 rcPlanDecide；另有三处旧名注释改成新名（纯注释，不影响发射）。

## 改名表（按符号 grep，不按行号）

| 旧 | 新 |
| --- | --- |
| g->needReclaim | g->rc.reclaim |
| g->rcForced | g->rc.forced |
| g->rcMemGrowth | g->rc.memGrowth |
| g->rcNShard | g->rc.nShard |
| g->rcNoCount | g->rc.noCount |
| g->rcSweepFree | g->rc.sweepFree |
| g->rcRootsUnreliable | g->rc.rootsUnreliable |
| scanUnitReclaim | rcPlanDecide |
| g.needReclaim / g.rcSweepFree / g.rcNoCount / g.rcMemGrowth（generateC 的档位闸门） | g.rc.reclaim / g.rc.sweepFree / g.rc.noCount / g.rc.memGrowth |

## 变化块（87 个；净 +22 行 ⇒ 新文件 16368 行）

| # | 旧行段 | 新行段 | 类型 | 旧→新行数 | 所在函数 / 上下文 | 首个新增行 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 184 后 | 185–262 | 新增 | 0→78 | typedef struct { | /** |
| 2 | 460–516 | （删除） | 删除 | 57→0 | typedef struct { |  |
| 3 | 521–522 | （删除） | 删除 | 2→0 | typedef struct { |  |
| 4 | 538 | 557 | 替代 | 1→1 | typedef struct { | /* Compile-time state of the reclamation runtime (meaningful only when |
| 5 | 547 | 566 | 替代 | 1→1 | typedef struct { | * `rc.rootsUnreliable`: this function holds reference stores that cann |
| 6 | 591 后 | 611–614 | 新增 | 0→4 | typedef struct { | /* The reclamation plan of this unit: whether a runtime is emitted at  |
| 7 | 593 | （删除） | 删除 | 1→0 | typedef struct { |  |
| 8 | 1337 | 1359 | 替代 | 1→1 | static void genViewCopier(CG *g, Type *inst, b | if (g->rc.reclaim && typeHasRefSlots(g, elem)) |
| 9 | 2491–2492 | 2513–2514 | 替代 | 2→2 | static const char *homeOwnArg(CG *g, const Exp | if (!g->rc.reclaim \|\| g->parTls \|\| g->coroFunc) return "0"; |
| 10 | 2545–2546 | 2567–2568 | 替代 | 2→2 | static void cgImplicitArgs(CG *g, Buf *b, cons | * `rc.reclaim`. */ |
| 11 | 3631 | 3653 | 替代 | 1→1 | static const char *exConv(CG *g, Expr *e) { | *  - **Emit no trigger at all while the root table is incomplete** (`r |
| 12 | 3882 | 3904 | 替代 | 1→1 | static void rcCollectOffsets(CG *g, Type *t, c | * @param g  The generator; returns an empty descriptor while `rc.recla |
| 13 | 3887 | 3909 | 替代 | 1→1 | static const char *rcEmitTypeDesc(CG *g, Type  | if (!g->rc.reclaim \|\| g->prefixBlk == 0) return "(const ExtcRcType * |
| 14 | 4012 | 4034 | 替代 | 1→1 | static void dbgRcCostReport(CG *g) { | g->rc.reclaim, g->rc.noCount, g->rc.sweepFree, g->rc.nShard > 0 ? g->r |
| 15 | 4110 | 4132 | 替代 | 1→1 | static void rcEmitOweNow(CG *g, const char *ba | if (g->rc.nShard <= 1) { |
| 16 | 4128 | 4150 | 替代 | 1→1 | static void rcEmitOweNow(CG *g, const char *ba | g->rc.nShard, basePtr, lvl * g->rc.nShard); |
| 17 | 4165–4166 | 4187–4188 | 替代 | 2→2 | static void rcEmitOwedCheck(CG *g, const char  | if (!g->rc.reclaim \|\| g->rc.rootsUnreliable \|\| g->prefixBlk == 0)  |
| 18 | 4171 | 4193 | 替代 | 1→1 | static void rcEmitOwedCheck(CG *g, const char  | if (g->rc.nShard <= 1) { |
| 19 | 4187 | 4209 | 替代 | 1→1 | static void rcEmitOwedCheck(CG *g, const char  | g->rc.nShard, basePtr, lvl * g->rc.nShard, ownCond, bufCstr(&b), n); |
| 20 | 4222 | 4244 | 替代 | 1→1 | static void rcEmitTriggerNow(CG *g, const char | if (n > 4096) { g->rc.rootsUnreliable = true; return; } |
| 21 | 4227 | 4249 | 替代 | 1→1 | static void rcEmitTriggerNow(CG *g, const char | if (g->rc.nShard <= 1) { |
| 22 | 4271–4272 | 4293–4294 | 替代 | 2→2 | static void rcEmitTriggerNow(CG *g, const char | rb, rbg, ck, ck, g->rc.nShard, ck, rg, basePtr, ck, lvl * g->rc.nShard |
| 23 | 4292 | 4314 | 替代 | 1→1 | static void rcEmitTrigger(CG *g, const char *b | if (!g->rc.reclaim \|\| g->rc.rootsUnreliable \|\| g->prefixBlk == 0)  |
| 24 | 4354 | 4376 | 替代 | 1→1 | static const char *rcAllocCore(CG *g, const ch | int nsh = g->rc.nShard > 0 ? g->rc.nShard : 1; |
| 25 | 4383 | 4405 | 替代 | 1→1 | static void rcRootLeaves(CG *g, Type *t, const | if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; } |
| 26 | 4402 | 4424 | 替代 | 1→1 | static void rcRootLeaves(CG *g, Type *t, const | if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; } |
| 27 | 4418 | 4440 | 替代 | 1→1 | static void rcRootLeaves(CG *g, Type *t, const | if (g->rcRoots.len > 4096) { g->rc.rootsUnreliable = true; return; } |
| 28 | 4620 | 4642 | 替代 | 1→1 | static void rcRootDeclImpl(CG *g, Type *t, con | if (!g->rc.reclaim \|\| !t) return; |
| 29 | 4639 | 4661 | 替代 | 1→1 | static void rcRootDeclImpl(CG *g, Type *t, con | g->rc.rootsUnreliable = true; |
| 30 | 4712 | 4734 | 替代 | 1→1 | static const char *exNew(CG *g, Expr *e) { | if (g->rc.reclaim) { |
| 31 | 4747 | 4769 | 替代 | 1→1 | static const char *exNew(CG *g, Expr *e) { | if (g->rc.reclaim && planArenaLevel(e) == 1) { |
| 32 | 4862 | 4884 | 替代 | 1→1 | static const char *exGencall(CG *g, Expr *e) { | if (g->rc.reclaim) { |
| 33 | 5938 | 5960 | 替代 | 1→1 | static void rcPredeclare(CG *g, Stmt *body) { | if (!g->rc.reclaim \|\| !body) return; |
| 34 | 6427 | 6449 | 替代 | 1→1 | static void cgCoroArenaDestroy(CG *g) { | if (g->rc.reclaim) |
| 35 | 7185 | 7207 | 替代 | 1→1 | static void rcNoteFreshLocal(CG *g, Stmt *s) { | if (!g->rc.reclaim \|\| g->coroFunc \|\| g->rc.nShard > 1) return; |
| 36 | 7286–7287 | 7308–7309 | 替代 | 2→2 | static bool rcEmitStoreRelease(CG *g, const ch | if (g->rc.sweepFree == 1) return false; |
| 37 | 7294 | 7316 | 替代 | 1→1 | static bool rcEmitStoreRelease(CG *g, const ch | if (g->rc.nShard > 1) return false; |
| 38 | 7408 | 7430 | 替代 | 1→1 | static bool rcLinkStmt(void *ctx, Stmt *s) { | if (tgt && ve && !s->u.assign.op && ve->kind == EX_IDENT && g->rc.recl |
| 39 | 7436 | 7458 | 替代 | 1→1 | static void rcScanLinked(CG *g, Stmt *body) { | if (!body \|\| !g->rc.reclaim) return; |
| 40 | 7457 | 7479 | 替代 | 1→1 | static void cgInitRefStore(CG *g, const char * | if (g->rc.sweepFree == 1 && !g->rc.noCount) { |
| 41 | 7549 | 7571 | 替代 | 1→1 | static void cgReclaimDropArena(CG *g, int lvl) | if (!g->rc.reclaim) return; |
| 42 | 7554 | 7576 | 替代 | 1→1 | static void cgReclaimDropArena(CG *g, int lvl) | if (g->rc.nShard <= 1) { |
| 43 | 7564 | 7586 | 替代 | 1→1 | static void cgReclaimDropArena(CG *g, int lvl) | for (int k = 0; k < g->rc.nShard; k++) |
| 44 | 7637 | 7659 | 替代 | 1→1 | if (planCName(s) && s->u.var.init && s->u.var. | if (g->rc.reclaim) g->rc.rootsUnreliable = true; |
| 45 | 7682 | 7704 | 替代 | 1→1 | if (planCName(s) && s->u.var.init && s->u.var. | if (g->rc.reclaim) g->rc.rootsUnreliable = true; |
| 46 | 7701 | 7723 | 替代 | 1→1 | if (g->coroFunc && planCName(s)) { | if (g->rc.reclaim && cgIsRefType(g, s->type)) |
| 47 | 7773 | 7795 | 替代 | 1→1 | if (s->u.var.overwrite) { | if (g->rc.reclaim) { |
| 48 | 7906 | 7928 | 替代 | 1→1 | localDef(g, lb, nm, 1); | if (g->rc.reclaim && !s->u.var.isGlobal && cgIsRefType(g, s->type)) { |
| 49 | 7913 | 7935 | 替代 | 1→1 | if (g->needReclaim && !s->u.var.isGlobal && cg | if (planRcNoDec(g->curFunc) && g->rc.noCount) |
| 50 | 7917 | 7939 | 替代 | 1→1 | if (g->needReclaim && !s->u.var.isGlobal && cg | } else if (g->rc.reclaim && !s->u.var.isGlobal && s->u.var.init) { |
| 51 | 8697–8698 | 8719–8720 | 替代 | 2→2 | for (size_t i = 0; i < s->u.match.arms.len; i+ | if (g->rc.reclaim && bt && rcScanType(g, bt, 0) != RC_NO_REFS) |
| 52 | 8934 | 8956 | 替代 | 1→1 | static void genStmtInner(CG *g, Stmt *s) { | const bool refStore = g->rc.reclaim && !s->u.assign.op |
| 53 | 8946 | 8968 | 替代 | 1→1 | static void genStmtInner(CG *g, Stmt *s) { | const RcScan valScan = (g->rc.reclaim && !s->u.assign.op && !refStore  |
| 54 | 9018 | 9040 | 替代 | 1→1 | static void genStmtInner(CG *g, Stmt *s) { | if (!g->rc.noCount && cgTargetIsParamSlot(g, s->u.assign.target)) { |
| 55 | 9052 | 9074 | 替代 | 1→1 | static void genStmtInner(CG *g, Stmt *s) { | if (g->rc.noCount && vlvl != -2 && vlvl == hlvl) |
| 56 | 9074 | 9096 | 替代 | 1→1 | static void genStmtInner(CG *g, Stmt *s) { | if (g->rcLocalStore && g->rc.noCount && hostBase == NULL) { |
| 57 | 10147 | 10169 | 替代 | 1→1 | static void genFuncInner(CG *g, FuncDef *f) { | bool tlsOwnerFrame = g->rc.reclaim && g->parTls && !g->noArena && !g-> |
| 58 | 10162–10163 | 10184–10185 | 替代 | 2→2 | static void genFuncInner(CG *g, FuncDef *f) { | g->rc.rootsUnreliable = g->rc.reclaim && g->rcGlobalsUnreliable; |
| 59 | 10168 | 10190 | 替代 | 1→1 | static void genFuncInner(CG *g, FuncDef *f) { | } else if (g->rc.reclaim) { |
| 60 | 10606 | 10628 | 替代 | 1→1 | static void genParTramp(CG *g, FuncDef *f) { | if (g->rc.reclaim) bufPuts(&args, ", 0"); |
| 61 | 12683 | 12705 | 替代 | 1→1 | static void emitPreamble(CG *g, Buf *out, Modu | if (g->rc.reclaim) { |
| 62 | 12685–12687 | 12707–12709 | 替代 | 3→3 | static void emitPreamble(CG *g, Buf *out, Modu | if (g->rc.noCount == 0 && g->rc.sweepFree == 1)        tier = 1; |
| 63 | 12734–12736 | 12756–12758 | 替代 | 3→3 | static void emitRuntimePrologue(CG *g, Buf *ou | * only when the program has a governed allocation point (`g->rc.reclai |
| 64 | 12740–12741 | 12762–12763 | 替代 | 2→2 | static void emitRuntimePrologue(CG *g, Buf *ou | reclaimEmitRuntime(g->arena, out, g->rc.noCount, getenv("EXTC_RC_TIME" |
| 65 | 13435 | 13457 | 替代 | 1→1 | static void emitGlobals(CG *g, Module *m) { | if (g->rc.reclaim && gd->ann) { |
| 66 | 14120 | 14142 | 替代 | 1→1 | static void scanUnitNeeds(CG *g, Module *m) { | * that really get emitted, `rcPlanDecide` narrows it to "is there a re |
| 67 | 14123–14124 | 14145–14146 | 替代 | 2→2 | static void scanUnitNeeds(CG *g, Module *m) { | g->rc.forced = rcEnv && *rcEnv && strcmp(rcEnv, "0") != 0; |
| 68 | 14129 | 14151 | 替代 | 1→1 | static void scanUnitNeeds(CG *g, Module *m) { | g->rc.nShard = 1; |
| 69 | 14131 | 14153 | 替代 | 1→1 | static void scanUnitNeeds(CG *g, Module *m) { | g->rc.noCount = 1; |
| 70 | 14136 | 14158 | 替代 | 1→1 | static void scanUnitNeeds(CG *g, Module *m) { | if (v > 1) g->rc.nShard = (int)(v > 64 ? 64 : v); |
| 71 | 14163 | 14185 | 替代 | 1→1 | static void scanUnitNeeds(CG *g, Module *m) { | if (nc && *nc && strcmp(nc, "0") == 0) g->rc.noCount = 0; |
| 72 | 14180–14182 | 14202–14204 | 替代 | 3→3 | static void scanUnitNeeds(CG *g, Module *m) { | g->rc.sweepFree = 0; |
| 73 | 14519–14520 | 14541–14542 | 替代 | 2→2 | static int unitAllAcyclic(CG *g, Module *m) { | static void rcPlanDecide(CG *g, Module *m) { |
| 74 | 14544 | 14566 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | g->rc.reclaim = false; |
| 75 | 14547 | 14569 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | g->rc.memGrowth = true; |
| 76 | 14560 | 14582 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | for (size_t i = 0; i < g->funcs.len && g->rc.nShard > 1; i++) { |
| 77 | 14567 | 14589 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | g->rc.nShard = 1; |
| 78 | 14570 | 14592 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | for (size_t i = 0; i < g->insts.len && g->rc.nShard > 1; i++) { |
| 79 | 14575 | 14597 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | if (md && planUsed(md) && planFnCrossSiteCycle(md)) { g->rc.nShard = 1 |
| 80 | 14581 | 14603 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | if (g->rc.forced) return; |
| 81 | 14601 | 14623 | 替代 | 1→1 | static void scanUnitReclaim(CG *g, Module *m)  | g->rc.reclaim = false; |
| 82 | 15213 | 15235 | 替代 | 1→1 | static void emitParallelRuntime(CG *g, Buf *ou | if (g->rc.reclaim) |
| 83 | 15241 | 15263 | 替代 | 1→1 | static void emitParallelRuntime(CG *g, Buf *ou | if (g->rc.reclaim) |
| 84 | 15252 | 15274 | 替代 | 1→1 | static void emitParallelRuntime(CG *g, Buf *ou | if (g->rc.reclaim) |
| 85 | 15265 | 15287 | 替代 | 1→1 | static void emitParallelRuntime(CG *g, Buf *ou | if (g->rc.reclaim) |
| 86 | 16263 | 16285 | 替代 | 1→1 | bool generateC(Ctx *ctx, Arena *arena, TypeTab | rcPlanDecide(&g, m); |
| 87 | 16270–16272 | 16292–16294 | 替代 | 3→3 | bool generateC(Ctx *ctx, Arena *arena, TypeTab | if (g.rc.sweepFree != -1 && g.rc.reclaim && !g.rc.memGrowth && unitAll |

## 使用规则

1. **按符号 grep，再按本表定位。** 文档里引用的 codegen.c:NNNN 若指向上面某个变化块，先找符号（函数名/宏/字段），再用新行号；落在未变化的段里则按累计净变化平移。
2. **禁止单一 offset 平移。** 87 个变化块把偏移切成 88 段，任何“加 N 行”的算法都会在下一个变化块前错位。
3. **行号只是定位器。** 本轮的验收是 emitted_identity --check 逐字不变，所以每个旧行号背后的 C 文本在生成物里仍逐字节相同；换算只影响“在源码里怎么找”。
4. **新写文档一律用符号锚点**（tools/doc_anchors.py 的 --check 与棘轮守着），行号只出现在本表与版本快照里。
