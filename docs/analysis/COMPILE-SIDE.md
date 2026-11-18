# 编译侧内存决策事实清单（COMPILE-SIDE）

> 审计对象：/home/alphayang/extC_Compiler（extC 编译器，C11 后端 + 内嵌运行时）。
> 判定口径：**只以源码与生成的 .c 为证据**；docs/** 与 tests 里的注释一律当"待验证断言"，与源码冲突时以源码为准并在第 11 节登记。
> 所有行号来自本次审计的实际 read/grep；生成物行号随标准库/开关变化，**重生成后需重定位**（命令见第 12 节）。
> 标注约定：**【默认】**＝不开任何开关就走到的路径；**【开关】**＝仅在该环境变量下生效。

---

## 0. 一句话结论

一个函数的内存行为在**编译期**就基本定死：每个函数栈上一组 extc_arena __extc_a[N]（每个词法块层一个，N 编译期可得）；每个分配点的 arena 层号由检查器（逃逸/提升 + 层级求解器）静态算出（0=出帧 ⇒ home arena，k>=1 ⇒ 第 k 层块 arena）；块/循环体退出时按层 release，函数退出时 destroy；只有当**单元里存在"受治理分配点"**且该单元**未被证明是 idle/acyclic** 时，才额外发射一套"位图 slab + 持有计数 + 区域清扫"的 rc 运行时，并在每个 rc 分配点前插入"这个 region 该不该收"的触发点。地址从不移动：arena 是 bump，rc 是 slab slot。

---

## 1. 词法层级编号：level 0/1/2 各是什么

### 1.1 语义（检查器一侧的权威注释）

- src/check/check_lvl.c:11-15（该文件整体在 EXTC_DBG_LVL 下只做 instrumentation，不改生成物）：
  逐字：**"A level is the depth of the place that owns the bytes: 0 means the storage outlives the frame (a global, the caller's home arena), 1 is the function body, larger numbers are deeper blocks, and a while or for body is one deeper than the block around it. Smaller therefore means longer lived."**
- 同一套数字在 arena 参数上用哨兵 -1 表示"出帧"：src/ast/ast.h:183 逐字 **#define ARENA_HOME (-1)**；src/plan/plan.h:1647 逐字注释 **"ARENA_HOME sentinel means 'use the home arena the caller passed'; a larger number means a shorter lifetime."**
- 检查器把 ARENA_HOME/非正 level 统一折算成深度 0：src/check/check_internal.h:290-299（arenaDepthOf）。
- 代码生成侧同一约定：src/back/codegen.c:241-247 逐字 **"Arenas are refined per block -- __extc_a[k] is the arena of the block at level k; k is the block depth, known at compile time, the same lexical depth the checker uses when it assigns reference depths."**

所以：**level 1 = 函数体块；level 2 = 函数体里第一层内嵌块 / 循环体 / 条件体；level 3 = 再深一层**。level 0 与 ARENA_HOME 都不是块层，不参与 __extc_a 下标。

### 1.2 每层 arena 数组长度 N 由什么决定

src/back/codegen.c:9646-9652（genFunc）逐字：
    int maxLv = 1 + blkMaxOfBlock(f->body);
    g->levelCap = maxLv + 2;
    ...
    if (!g->noArena) cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);

即 **数组长度 = 1 + (1 + 函数体最大块深度) = 最大层号 + 1**，下标 0 恒不用。层数在 src/back/codegen.c:6495-6562（blkMaxLevel）算出，规则逐字要点：
    case ST_BLOCK:  return 1 + blkMaxOfBlock(s);
    case ST_IF:     a = 1 + blkMaxOfBlock(thenBody); b = elseBody ? 1 + (...) : 0;  return max(a,b);
    case ST_WHILE:  return (planCondAllocs(s) ? 2 : 1) + blkMaxOfBlock(body);
    case ST_DOMAIN: return 1 + blkMaxOfBlock(body);
    case ST_MATCH:  每个 arm 1 + blkMaxOfBlock(body)，取 max;

**样本对照**：/tmp/cf/cyclic_shape.c:3275 逐字 extc_arena __extc_a[4] = {0};（main 体 1 + while 2 + while 内 if 3 ⇒ maxLv=3，长度 4）。

**noArena（连数组都不声明）**：src/back/codegen.c:9624-9685 段内逐字 g->noArena = !planMayUseArena(f) && !(owNow.len > 0 || owcNow.len > 0); 另有 g->coroFunc ⇒ noArena=true（step 不拥有 arena 数组）、mainArgs ⇒ false、isEntry && planUsesHome(f) ⇒ false、isEntry && needPool ⇒ false。语义见 src/back/codegen.c:271-274 逐字：函数不往自己块 arena 放东西 ⇒ 既不声明 __extc_a[N] 也不发 release。

### 1.3 能否观察

| 开关/选项 | 定义位置 | 行为 |
|---|---|---|
| --dump-effects（= EXTC_DUMP_EFFECTS=1） | src/main.c:438、src/main.c:861-865；读点 src/check/check_top.c:1255/2604/3398/3403 | 打印每函数逃逸集与 effect summary |
| EXTC_DBG_ARENA=1 | src/main.c:459；实现 src/back/codegen.c:5954-5962（arenaDriftCheck） | **只在两处不一致时打印**：level==0（检查器漏站点）或 level 比当前块更深；**一致时无输出、且不改生成物**。另有 EXTC_DBG_ARENA_VERBOSE=1 逐点打印 |
| EXTC_DUMP_LVL=1 | src/check/check_top.c:9525-9555/9623-9624 | 打印层级求解器收敛轮数与落位统计 |

本轮实测（2026-11-09，用 build/extc）：
- EXTC_DBG_ARENA=1 编 tests/reclaim-shapes/cyclic_shape.extc：**无输出**（= checker/codegen 一致），与 src/back/codegen.c:5940-5945 的注释一致（"changes no output"）。
- EXTC_DUMP_LVL=1 编 tests/arena/promote.extc：
  [lvl] 175 level facts, converged after 32 round(s)
  [lvl] decided: 0 sites in the home arena, 0 kept at block level
  （"converged after 32" 正好命中 src/check/check_top.c:9525-9555 的 **≤32 轮**重放上限 ⇒ 见第 11 节）
- EXTC_DUMP_EFFECTS=1 编同一文件，可见：
  [escapes] next                   { old self x rot }
  [effects] next                   toParam[Addr=0x0 Cont=0x0 Other=0x0] toHome[Addr=0x0 Cont=0x0] localAddr=0 fresh=0 callees=0
  格式源码：src/check/check_top.c:2604-2610（[escapes]）与 src/check/check_top.c:3403-3407（[effects]）。

---

## 2. 哪些构造会分配、分配到哪一层

分配层号一律来自检查器：**ar = arenaRefAt(g, planArenaLevel(e))**；ARENA_HOME 渲染成 (*__extc_home)，k>=1 渲染成 &__extc_a[k]（src/back/codegen.c:5772-5781 逐字 if (arenaArg == ARENA_HOME) return "__extc_home"; ... return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);）。

### 2.1 new T（单值）
src/back/codegen.c:4605-4639（exNew）。两种形态：
- 【默认/无 rc】逐字：
      ((T *)extc_arena_alloc(&<ar>, (int64_t)sizeof(T), "path", line))
  样本：/tmp/cp-loop.c:255（tests/asan/promote-loop.extc 的 while 内 new node，落在 __extc_a[1]）
      node * cell = ((node *)extc_arena_alloc(&__extc_a[1], (int64_t)sizeof(node), "tests/asan/promote-loop.extc", 12));
- 【默认/有 rc】逐字形如（src/back/codegen.c:4625-4639）：
      ((T *)extc_rc_alloc(<region>, (int64_t)sizeof(T), <td>))
  样本：/tmp/cf/cyclic_shape.c:3281
      dnode * head = ((dnode *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(dnode), &__extc_rct0));
  td = 类型描述符（引用叶子集合，src/back/codegen.c:3619-3626 的 RcScan 三态：NO_REFS/LEAF_REFS/CONSERVATIVE）。

### 2.2 new T[n]（数组）
src/back/codegen.c:4641-4678。**只有 planArenaLevel(e)==1（函数体层）才走 rc region**；其它层（内层块 / ARENA_HOME）照旧走 arena。返回的是视图：
    (slice_T){ .data = (T *)<core>, .len = n }
td：rcScanType(g,w,0) != RC_NO_REFS ⇒ "((const ExtcRcType *)-1)"（保守扫每字），否则 "(const ExtcRcType *)0"。

### 2.3 alloc<T>(n)
src/back/codegen.c:4680-4790（exGencall 的 alloc 分支）：**层号直接用检查器给的 planArenaLevel(e)**；有 rc 时同 td 机制，level 可 >1。
- 无 rc 样本：/tmp/cp-p.c:253（tests/arena/promote.extc:13 的循环内 alloc，落在循环自己的层 2）
      int64_t * p = (int64_t *)extc_arena_alloc(&__extc_a[2], (int64_t)(250000) * (int64_t)sizeof(int64_t), "tests/arena/promote.extc", 13);
- 有 rc 且提升到 home 的样本：/tmp/cf2/A_field_root_lowered.c:2846
      int32_t * x = (int32_t *)extc_rc_alloc(extc_rc_regionOf((void *)&((*__extc_home)), -1), (int64_t)(1) * (int64_t)sizeof(int32_t), (const ExtcRcType *)0);

### 2.4 字符串 / 字面量
**不分配、不进 arena、不是 rc 槽**。src/back/codegen.c:5060-5066 逐字：
    case EX_STR: return arenaPrintf(g->arena, "(%s){ .data = (uint8_t *)"%s", .len = sizeof("%s") - 1 }", cType(g,e->type), e->u.str.text, e->u.str.text);
注释：字面量在**只读内存**里，长度用 sizeof-1 让 C 处理转义。样本：/tmp/cf/cyclic_shape.c:3394 行附近 (slice_u8){ .data = (uint8_t *)"fwd=", .len = sizeof("fwd=") - 1 }。

### 2.5 容器（vector / hashMap / string 扩容 / dyn）

**（v38 起本节重写：池基质退场）** 容器不占 arena、也不占 rc slab，但也不再从 pool 拿块：

- `vector<T>` / `string` 的载荷走 `buf<T>` 的地址预留（`extc_buf_reserve` / `extc_buf_grow` / `extc_buf_release`，`src/back/bufrt.c`）；`string` 的 ≤16 字节走内联 SSO。
- `hashMap` / `map` / `set` / `linMap` / `linSet` 的存储是容器自有的平坦块（`block<T>`），容量固定、任意下标读写。
- `dyn` 的载荷落在构造点的**命名 place** 里：发射物把 place 号当第 4 实参传给 `extc_dyn_put`（`src/back/places.c:235`），载荷 `extc_place_alloc(zi, size)` bump 进该 place 的 chunk 链；句柄 `{ zi, slot, gen, zgen }` 靠两次世代比较判陈旧。
- 历史对照：v38 之前容器块由 `src/back/pools.c`（126 行）的池块链提供（`extc_pool_new_at` / `extc_pool_take_raw`），该文件与整套 `extc_pool_*` 对象运行期已删除。

### 2.6 闭包
**未验证**：本轮在 src/back/codegen.c 里 grep "closure" 只命中"迭代到不动点的闭包（closure of the demand set）"这类术语，未定位到闭包的运行时表示与分配路径 ⇒ 见第 11 节。

### 2.7 coroutine 帧
每任务一个**独立 arena**：src/back/coroutine.c:57 逐字 static extc_arena *extc_taskArena;；分配门 src/back/coroutine.c:124-130：
    /* A task's own place as an allocator: the frame of a boxed coroutine lives here, and so does anything the coroutine allocates with new (slice C's promotion). */
    void *extc_task_alloc(int64_t id, int64_t n, const char *path, int line) { ... return extc_arena_alloc(&extc_taskArena[id], n, path, line); }
任务结束 src/back/coroutine.c:145 逐字 extc_arena_destroy(&extc_taskArena[id]);（同段注释解释：任务不会再跑，spare 块会永远占着）。样本 /tmp/cf/coro.c:1539。
任务表本身是 realloc 出来的（src/back/coroutine.c:108-113），不是 arena 分配。

### 2.8 C 互操作内存
- 外部声明/未检查函数体：【默认】g->uncheckedBody 关闭借用/转换检查，但不改变分配路径（src/back/codegen.c:9622-9623、1959、3378）。
- C 内存的视图：src/back/plate.c:35-38 的 extc_viewOf(uint8_t *p, int64_t n) 只生成 slice_u8 v = { p, n }（**零检查、不分配**）。
- C 侧 malloc 出来的内存归谁、【未验证】。

---

## 3. 逃逸判定（"逃逸"= 引用活得比被指对象久）

### 3.1 规则本体
src/check/check_escape.c:1-8 文件头逐字：**"The pass is driven by the one reference rule and by the per-expression depth recorded for every value. Depth 0 means the value lives outside this frame (a parameter, a global, static storage); every nested block adds 1, so a smaller depth means a longer life."**
同一文件 :33-43 的三条义务 + 一条明确不做：
    1. A returned reference or view must point at a parameter or at static data (depth 0).
    2. A local variable may not be initialized with a reference to something deeper than the variable itself.
    3. A field or element store (o.f = v) may not store a reference to something deeper than the target.
    4. Passing a reference into a function that stores it needs interprocedural analysis and is checked at the call site instead.
⇒ 逃逸判定 = **比较整数深度**：把值"发布"到 level at，若值当前深度更深就必须提升（promote）到 at。所以"返回 / 赋给外层变量 / 存入堆对象 / 闭包捕获 / 全局"全都归约为"发布点 at 更小"。

### 3.2 记录与提升的固定顺序
src/check/check_escape.c:1487-1512（noteStore）逐字注释：**"The order cannot change: recordStore has to run before promoteInto so the solver sees the requirement first, and the field table has to run after promotion because promoting the field table reads it."** 四步：recordStore → promoteInto → markCallHomeIfEscaping → 字段表/整值表。
recordStore 本身逐字（src/check/check_escape.c:1451-1483）：**"Record one publication: a value is put at level at. Nothing is decided here."** 记录进 c->stores，at 为 0 表示"必须活过本帧"。

### 3.3 提升目标的选取
- 目的深度 0 ⇒ 目标就是 ARENA_HOME：src/check/check_escape.c:1781-1802 段逐字 **"目的 depth 0 = caller's level = home arena（用 ARENA_HOME 表示）"**，代码 if (planArenaLevel(val) == ARENA_HOME) return true; int target = (at == 0) ? ARENA_HOME : at; if (planArenaLevel(val) > target) planSetArenaLevel(val, target);
- @return：src/check/check_escape.c:2642-2646（返回 ARENA_HOME 或块层，if (a == 0) return ARENA_HOME;）。
- home 是"粘性"的：src/check/check_escape.c:2792-2803（if (w == ARENA_HOME) { home = true; break; }，一旦 home 就不许改回块层）。
- 字段级提升：src/check/check_escape.c:1373-1445（promoteFieldsAt：只提升有 src 的槽；超限保守返回 true；want = min(slot depth, 最浅 level)）。

### 3.4 保守回退
- 走到路径预算上限即保守放弃并报诊断：src/check/check_escape.c:27 逐字 **#define EXTC_PROMOTE_MAX_HOPS 512**；:218-225 超预算时 EXTC_DBG_FALLBACK("symMinPointee: hop budget exhausted") 并返回 0（= 出帧，最保守）。
- 逃逸集计算：src/check/check_top.c:2590-2610（computeEscapesMode）。逐字：非 union 模式 for (int round = 0; round < 32; round++) if (!markNamesInStmt(c, f, f->body)) break;；union 模式 < 64 轮，注释 "unionMode: keep the names already collected"。
- 层级求解器也是**重放到不动点、上限 32 轮**：src/check/check_top.c:9525-9555（EXTC_DUMP_LVL 打印 converged after %d round(s)）。

### 3.5 循环内分配：不逃逸 vs 逃逸出循环
同一份语义规则直接体现在产物里：
- **不逃逸**（tests/arena/promote.extc:12-16 的 var p = alloc<i64>(250000) 只在循环内用）⇒ 分配点留在循环体自己的层 2，每轮 release：/tmp/cp-p.c:253（见 2.3）。
- **逃逸**（tests/asan/promote-loop.extc:12 的 var cell = new node; cell.next = head; head = cell，cell 被存进 head 且 head 活到函数结束）⇒ 分配点被提升到函数体层 1，即使代码写在 while 里：/tmp/cp-loop.c:255（见 2.1）。
  即"分配在循环内"不决定生命周期，"值被发布到哪一层"才决定。

---

## 4. 逃逸到第 k 层时发生什么（生成 C 逐字）

**静态选层 → 直接在那一层 arena/region 里分配 → 地址永不移动**。没有"检查是否逃逸"的运行时代码，也没有 mover。

### 4.1 选层发生在检查器的 level pass（覆盖前端给出的初值）
src/check/check_top.c:9563-9622 段逐字规则：
    if (anMinAt(site) == 0) want = ARENA_HOME;
    else if (anMinAt(site) >= 1) want = anMinAt(site);
    else want = anLexicalLevel(site) >= 1 ? anLexicalLevel(site) : 1;
    if (变化) { planSetArenaLevel(site, want); anSetRefDepth(site, arenaDepthOf(want)); }
注释逐字：anMinAt==0 ⇒ **must outlive this frame ⇒ home arena**；否则块 arena k（循环内即每轮回收）；未受约束用自己词法层。
落位前的求解：src/check/check_top.c:9490-9523（overflow ⇒ ARENA_HOME；否则 want = (nd <= 0) ? ARENA_HOME : nd，与已有 cur 取更长寿一侧 ⇒ "只许往长活方向走"）。

### 4.2 第 k 层（k>=1）的 arena 分配
样本 /tmp/cp-p.c:270（函数体层）与 :253（循环层）：
    node * cell = ((node *)extc_arena_alloc(&__extc_a[1], (int64_t)sizeof(node), "tests/arena/promote.extc", 22));
    int64_t * p = (int64_t *)extc_arena_alloc(&__extc_a[2], (int64_t)(250000) * (int64_t)sizeof(int64_t), "tests/arena/promote.extc", 13);
arena 本体：extc_ablock 单链表 bump；块 cap 倍增（下限 64）：
    int64_t cap = n > 64 ? n : 64;  if (a->top && a->top->cap < (1 << 20) && cap < a->top->cap * 2) cap = a->top->cap * 2;
越界即 trap 并退出：**"trap: out of arena memory (this allocation wanted %lld bytes)"** + extc_die(1)（/tmp/cp-loop.c:97-142 的 extc_arena_alloc）。
release 保留一个 <= EXTC_ARENA_SPARE_MAX (1<<20) 的 spare 块，其余 free（/tmp/cp-loop.c:61-76）。

### 4.3 第 k 层（含 home）的 rc 分配
样本 /tmp/cf/cyclic_shape.c:3278/3281（块层）：
    ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&(__extc_a[1]), 1);
    dnode * head = ((dnode *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(dnode), &__extc_rct0));
样本 /tmp/cf2/owed_home.c:2824（home，rid 传 -1）：
    ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&((*__extc_home)), -1);
⇒ region 的 **key 是 arena 地址**，rid 只存进结构、运行期无读者（src/back/codegen.c:5796-5800 注释逐字："rid is only stored into the struct by regionNew and has no reader"）。因此 region 寿命与那只 arena **完全一致**（src/back/codegen.c:12408-12414 注释逐字 "the region's lifetime coincides exactly with that arena's"）。

---

## 5. 局部释放：写屏障的分派（问题 5）

### 5.1 分派点
src/back/codegen.c:8631-8779（赋值语句）是唯一分派处，顺序：refStore = g->needReclaim && !s->u.assign.op && cgIsRefType(target)；host 由 cgSlotHost 判成 RC_HOST_OBJ / RC_HOST_LOCAL / RC_HOST_UNKNOWN；然后：
1) hostUnknown ⇒ extc_rc_storeRefSlot(slot, v)（发在 :8715）
2) 命中 rcFreshHostRegion(hostBase) ⇒ no-count 且 vlvl==hlvl 用 extc_rc_storeRefSameRegion(...)，否则 extc_rc_storeRefOwnedReg(slot, v, hreg)
3) hostBase==NULL 且 target 是 EX_IDENT ⇒ 先试 rcEmitStoreRelease（点修复定向释放），不行再
4) rcLocalStore && rcNoCount && hostBase==NULL ⇒ extc_rc_storeRefLocal(slot, v)
5) 兜底 ⇒ extc_rc_storeRefOwned(slot, v, owner)
数据字段（非引用类型）**一个字都不发**（/tmp/cf/cyclic_shape.c:3285 head->val = 0; 就是裸赋值）。

### 5.2 各形态的发射条件 + 真实样本

| 发射形态 | 触发条件（源码位置） | 生成物样本（逐字） |
|---|---|---|
| extc_rc_initRef | 引用类型局部/全局"声明即初始化"（src/back/codegen.c:7257-7323 的 init 分支：只写+加，**不读旧值**） | /tmp/cf/cyclic_shape.c:3282 extc_rc_initRef((void **)&head, (void *)head); |
| extc_rc_storeRefLocal | 【默认，rcLocalStore=1】no-count tier 且 host 证不出（hostBase==NULL）、目标是本帧引用槽（开关 src/back/codegen.c:13789-13796；注释 :8730-8754：no-count 下这两次 ownSlab 纯属浪费） | /tmp/cf/cyclic_shape.c:3301 extc_rc_storeRefLocal((void **)&(prev), (void *)(n)); |
| extc_rc_storeRefOwnedReg | host 可证，且 host 是**本帧 fresh 局部**（rcFreshName / rcFreshHostRegion，src/back/codegen.c:6997-7036、8620-8768），region 指针可静态给（省运行时反查） | /tmp/cf2/acyclic_heapref.c:3356 extc_rc_storeRefOwnedReg((void **)&(head->next), (void *)(((node *)0)), __extc_rcr0);（null 存储也走这条） |
| extc_rc_storeRefSameRegion | 同上且 no-count 且值层==host 层（跨区不可能，省掉两次 ownSlab） | /tmp/cf2/struct_copy.c:3406 extc_rc_storeRefSameRegion((void **)&(t->next), (void *)(t)); |
| extc_rc_storeRefOwned | host 可证但非 fresh（含形参/全局宿主） | /tmp/cf/cyclic_shape.c:3256 extc_rc_storeRefOwned((void **)&(p->next), (void *)(q), (void *)(p)); |
| extc_rc_storeRefSlot | host **证不出**（基表达式带调用、视图元素等；src/back/codegen.c:8720） | /tmp/cf2/unknown_host.c:3236 extc_rc_storeRefSlot((void **)&((*(id(a))).next), (void *)(a)); |
| extc_rc_storeRefUnknown | 由 **storeRefSlot 在运行期回退**才可达：槽不在任何 rc slab 里（视图数据 / C 或 pool 缓冲）。逐字（/tmp/cf/cyclic_shape.c:1591-1597）：if (ss && ss->region) { extc_rc_storeRefOwnedReg(slot, v, ss->region); return; } extc_rc_storeRefUnknown(slot, v); |
| extc_rc_storeStructCons | 整块赋值（结构体/定长数组）且引用叶子**无法枚举**（RC_CONSERVATIVE）：只加不减 + 注册跨区边；host 证不出则让目标 region 退出清扫（src/back/codegen.c:7717、7246-7312 注释） | 本次样本未观测到发射（见第 11 节） |
| 逐叶屏障 + 整块 *d = s | 整块赋值且叶子可枚举：拆成每叶一次屏障，再做整块拷贝（src/back/codegen.c:7257-7323） | /tmp/cf2/struct_copy.c:3396-3398：extc_rc_storeRefOwned((void **)&((*__extc_svd0).n), (void *)(__extc_svv0.n), (void *)(s)); *__extc_svd0 = __extc_svv0; |
| extc_rc_storeRefOwnedRelease | **点修复**定向释放旧值，四个 criterion 全须成立（src/back/codegen.c:7056-7135）：(1) 赋值在循环体内；(2) 目标槽是本帧引用槽且声明在所有已注册循环之外；(3) 写入值是本帧 fresh 分配、从未重绑定、其 region 由本帧拥有；(4) 该对象已被写进某个堆对象的引用字段。禁用：rcSweepFree==1、!needReclaim、rcRootsUnreliable、coroFunc、rcNShard>1、根表>4096 | /tmp/cf2/struct_copy.c:3408 extc_rc_storeRefOwnedRelease((void **)&(keeper), (void *)(t), (void *)0, __extc_rcr1, (void *[]){ (void *)(pad), (void *)(s), (void *)(t) }, 3); |

### 5.3 rcLocalStore 的作用与默认
- 默认值：src/back/codegen.c:13793-13796 g->rcLocalStore = 1; EXTC_RC_LOCALSTORE=0 时置 0；注释（:13789-13792）逐字：**【默认开】=0 是 kill switch，只影响 no-count tier**；=0 能逐字节恢复"翻档前"的产物（src/main.c 的 --help 段同样如此自述）。
- 作用：本地/参数槽的引用赋值只做"置值+加新值"，**跳过 ownSlab 反查与旧值减**（no-count 档里计数本来不用，那两次查纯属浪费）；旧值保护交给 sweep/region 机制。

### 5.4 哪些写入被证明"旧值死亡"（可省旧值处理）
只有 extc_rc_storeRefOwnedRelease 那一档：criterion (1)+(4) 共同证明"旧值只从这一格可达"（循环里每轮覆盖、且对象被链进堆结构后再无人指向旧值），于是可以在赋值点直接把旧值归还 slab。其余所有档都**必须**读旧值/保守处理；storeRefSlot 的 storeRefUnknown 回退更进一步：把整个 region 标成 noSweep（宁可少收）。

---

## 6. region 的生命周期与收集触发（问题 6）

### 6.1 谁创建 region：**不预先创建，第一个用户现场 claim**
region 由 arena 地址惰性登记：src/back/codegen.c:12425-12454 把下面这段直接放进生成的 C（逐字）：
    #define EXTC_RC_REG_MAX 512
    static void *extc_rc_regKey[EXTC_RC_REG_MAX];
    static ExtcRcRegion *extc_rc_regVal[EXTC_RC_REG_MAX];
    static size_t extc_rc_regN;
    static inline ExtcRcRegion *extc_rc_regionOf(void *key, int32_t rid) {
      for (size_t i = 0; i < extc_rc_regN; i++) if (extc_rc_regKey[i] == key) return extc_rc_regVal[i];
      ExtcRcRegion *r = extc_rc_regionNew(rid);
      if (extc_rc_regN < EXTC_RC_REG_MAX) { extc_rc_regKey[extc_rc_regN] = key; extc_rc_regVal[extc_rc_regN] = r; extc_rc_regN++; }
      return r;
    }
配套 extc_rc_regionDrop(void *key)（regionFree + 末元素填补 + regN--）。表满时**不再登记**（src/back/codegen.c:12416-12418 注释逐字：**"leaking a region is preferred over freeing it early"**）。
⇒ 粒度为 **arena 地址**：每个函数的每一层块 arena 一个 region（若该层真被 rc 用到），home arena 在**拥有它的帧**里 claim；coroutine 任务 arena 由任务自身持有时在任务里 claim。

### 6.2 每个函数入口做什么（没有"root table 构造"，根表是就地物化的）
- 帧入口：src/back/codegen.c:9646-9652 发 extc_arena __extc_a[N] = {0};；src/back/codegen.c:5782-5813 段把 rc 用到的 region 指针在**函数体顶部**预声明（rcPredeclare 先扫 body 的 EX_NEW），最多 40 个：ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&(__extc_a[1]), 1);
- 根**不是**一张入口时采样的表：每个触发点用当前作用域活引用槽的 C 表达式**当场拼一个复合字面量**（src/back/codegen.c:3941-3992 rcRootTableText）：(void *[]){ (void *)(cur), (void *)(keep) }；n==0 时发 (void *const *)0（运行时只看 nroots）。根内容 = 包围块里还活着的本地 + 本函数参数 + 全局（src/back/codegen.c:3545-3617 注释）。
- 每进入一层块，根表游标被截断记录：src/back/codegen.c:6744-6747 *(size_t *)vecPush(&g->rcRootMarks) = g->rcRoots.len，块退出时从游标恢复（"块里声明的槽在块外不存在"）。

### 6.3 何时调用 maybeCollect、参数是什么
唯一的发射点是 rcEmitTriggerNow（src/back/codegen.c:4101-4186），由 rcAllocCore 在**每个 rc 分配点之前**发出（src/back/codegen.c:4238-4278）。单分片逐字：
    int n = rcGlobalRoots.len + rcRoots.len;
    if (n > 4096) { rcRootsUnreliable = true; return; }        /* 硬性禁用：根表太大 */
    ... "if (extc_rc_shouldSweep(rg)) extc_rc_maybeCollect(rg, <roots>, n);"
参数 = (region 指针, 根数组, 根数)；语义（src/back/reclaim.c:2783-2806）：先推进在飞的轮次，sweeping 则走一步 sweep；否则若不该 sweep（未到 floor / 轮次未就绪）直接 return -1（跳过），否则 arm 计时并 step 一次。
多分片：遍历 N 个 shard，extc_rc_regionFloor(rg, (EXTC_RC_FLOOR) / N)（**floor 按 shard 均分**，src/back/codegen.c:4158-4177 注释：否则 N 个 shard 各守整个 floor，总界变 N 倍、停顿旋钮会副作用改变内存界），取 extc_rc_garbageHint 最大的 shard（bytes-live 最多垃圾），**一次只收一个 shard**。

### 6.4 三个"只能不 collect"的边界（决定了"谁有资格收"）
1. **不是本帧拥有的 region 一律不发触发点**：src/back/codegen.c:3842-3870 rcFrameOwnsArena 逐字 if (g->parTls) return false; if (lvl == ARENA_HOME) return g->inMain; return true;。理由（src/back/codegen.c:3545-3617 注释，C hole）：静态根表是**词法的**，看不见调用栈上其它帧的活引用；home/thread-local arena 从外层递下来时，内层触发就会把活子树当垃圾（实测 nroots=0，被释放的 65537 个 slot 每一个 hold>0）。
2. **不能证明根表完整就完全不发触发点**：rcRootsUnreliable（enum payload、泛型 enum、coroutine 帧、根数 > 4096；src/back/codegen.c:522-536、4287-4324）。
3. **本帧拥有但当前有活跃被调子树**：交给 owed（next 小节）。

### 6.5 owe / owed 借贷机制（内层向属主借一次收集）
- 内层（region 属主是外层帧，或 TLS）不 sweep，只记一笔：src/back/codegen.c:4020-4047 rcEmitOwe 单分片逐字 "if (extc_rc_shouldSweep(rr)) extc_rc_owe(rr);"（parTls 时直接 return，TLS 归属由运行时 RC_OWN_TLS 分支处理）；生成物样本 /tmp/cf2/owed_home.c:2826-2829（if (__extc_homeOwn) {...maybeCollect...} else {...extc_rc_owe(rr);...}）。
- 属主在**下一次调用之前**结算：src/back/codegen.c:4075-4099 rcEmitOwedCheck 单分片逐字 "if (extc_rc_owed(rr)) extc_rc_checkOwed(rr, <roots>, n);"；调用点插入位置在 cgImplicitArgs（src/back/codegen.c:2471-2484，注释逐字：上一批被调已返回、下一批尚未进入，此刻本帧静态根表是完整 seed，故**不需要**帧级 roots-unreliable 标记）。样本 /tmp/cf2/owed_home.c:2894。
- 可靠性不变量与界：src/back/reclaim.c:2808-2820 注释逐字 **"if the owner frame never calls again the debt stays forever and the region waits until the frame is destroyed -- a relaxed upper bound, not a dangling pointer."**；运行时实现 src/back/reclaim.c:2821-2854（owe=1；owed；checkOwed：sweeping ⇒ 走一步 / !owe ⇒ -1 / noSweep ⇒ 清 owe 返回 -1 / marking ⇒ 清 owe + markAdvance / 轮次未就绪 ⇒ -1 / 否则清 owe 并 step 一次）。
- 代价（源码自述，仍是当前界的一部分）：只往 home arena 分配且属主之后不再调用的形状仍等帧销毁；属主在循环里反复调被调的形状峰值仍 10.86–10.92 MB（src/back/codegen.c 同段注释）。

### 6.6 何时释放 region：**只在帧销毁**
src/back/codegen.c:7325-7346 cgReclaimDropArena 逐字要点：只在**帧销毁**发 extc_rc_regionDrop；块 release 不 drop（否则循环体每轮新建 1 MiB slab）；分片时每个 shard 各自 drop；rcArenaUsed 位图没碰过的 level 跳过。样本 /tmp/cf2/owed_home.c:2905-2907：
    extc_rc_regionDrop((void *)&__extc_a[1]);
    extc_arena_destroy(&__extc_a[1]);
    extc_arena_destroy(&__extc_a[2]);

### 6.7 EXTC_RC_SHARDS 如何切分
- 语义（src/back/codegen.c:476-497 注释逐字要点）：一个逻辑 region（arena 地址 claim）切成 N 个运行时 region；每 shard 自己的 slab 链、跨区注册位图、mark 栈 ⇒ 可单独 sweep，停顿 ∝ 被收 shard 的可达集。
- **按分配点分片号，不按活跃集**：rcAllocCore 里 int k = g->rcSite++ % nsh;，region 号 = lvl*nsh + k（src/back/codegen.c:4238-4278 逐字 "extc_rc_alloc(extc_rc_regionOf((void *)((char *)(base) + k), lvl*nsh + k), bytes, td)"）。理由（同段注释）：一个 new 点产生的所有东西（一条链、一个环、一代树节点）永远同 shard，链/环保持完整；按活跃集分会撕裂链环，跨 shard 注册位一旦置位，环状垃圾永远当 seed、界随轮数增长。
- 上限 64：src/back/codegen.c:13783-13787（v > 64 ? 64 : v；注释：再大则每 shard 至少一个 1 MiB slab 的基线压过停顿收益）。
- 有跨站点环就**拒绝分片**：src/back/codegen.c:14057-14124 段（[shard] unit has a cycle across sites (fn=%s); refusing to shard ⇒ rcNShard=1）。
- N==1 时字节相同（src/back/codegen.c:476-497）。

---

## 7. 开关矩阵（读取位置 / 默认值 / 对发射代码的影响）

全部开关都在 **编译期** 读环境变量；运行期只剩少数（FLOOR/SWEEP_BUDGET/MARK_EVERY/WORK_BUDGET/PAUSE/TIME）会再读一次（见 §8）。

| 开关 | 读取位置（默认） | 对生成 C 的具体影响 |
|---|---|---|
| EXTC_RECLAIM | src/back/codegen.c:13772-13774（**【默认开】**；=0 关） | =0 ⇒ 不发射整套 rc 运行时，分配退化为纯 bump 的 extc_arena_*；=1 ⇒ rcForced，跳过"有没有受治理分配点"的检测**强制**发射（=1 是许可不是强制；idle 证明仍生效） |
| （无开关）受治理分配点检测 | src/back/codegen.c:14057-14124（scanUnitReclaim） | ① !needReclaim ⇒ return；② unitReclaimIdle ⇒ needReclaim=false（注释：这是**证明**不是优化开关）；③ 跨站点环 ⇒ rcNShard=1；④ rcForced ⇒ return；⑤ 无 new T / alloc<T>(n) / @overwrite 单值 cell ⇒ needReclaim=false |
| （无开关）idle/acyclic 判据 | src/back/codegen.c:13949-14042 | unitReclaimIdle 三条：每个堆 ref 字段每个对象至多写一次 / ref 类型局部与参数从不被重新赋值 / 无丢引用操作（容器删除替换、@overwrite 复用、free 类内建、接收引用的 opaque callee）；一个点不满足 ⇒ 整单元判 false。unitAllAcyclic 为 event tier 判据 |
| EXTC_RC_NOCOUNT | src/back/codegen.c:498、13769-13795（**【默认 1 = 不计数】**）；运行期文本 src/back/reclaim.c:2919 发 #define EXTC_RC_NOCOUNT 1，:1317 段 [rt:nocount] 把计数入口编译成 no-op | 计数入口变 no-op，回收只靠 sweep；=0 恢复计数（实测计数层比清扫贵 +100%~+455%，docs/plans/TODO.md） |
| EXTC_RC_LOCALSTORE | src/back/codegen.c:501、13778-13785（**默认 1**） | =0 关掉 extc_rc_storeRefLocal 发射，回到每次本地槽存储都反查 region 的旧形态（no-count tier 下纯浪费）；=0 可逐字节还原"翻档前"产物 |
| EXTC_RC_SHARDS | src/back/codegen.c:476、7093、13764-13776（**默认 1**，上限 64） | N>1 ⇒ 每分配点按 rcSite++%N 选 shard、region 号 lvl*N+k、floor 均分、每触发点只收一个 shard；N==1 字节相同 |
| EXTC_RC_SWEEPFREE | src/back/codegen.c:505、13809-13814（默认 0；=1 时并强制 rcNoCount=0） | =1 ⇒ sweep 直接 free slab/整 region（绕过回收位图），且禁用点修复释放 |
| EXTC_RC_FLOOR | 运行期 src/back/reclaim.c:140-141（**默认 1<<20 = 1 MiB**）；发射点 src/back/codegen.c:4158-4177 | sweep 启动门槛（bytes 未达 floor 不收）；多分片时每 shard 收 floor/N |
| EXTC_RC_SWEEP_BUDGET | 运行期 src/back/reclaim.c:888-890（**默认 8 words**） | 一步 sweep 的词数上限（word = 64 slot） |
| EXTC_RC_MARK_EVERY | 运行期 src/back/reclaim.c:901-903（**默认 64**） | 每多少次分配推进一次标记 |
| EXTC_RC_WORK_BUDGET | 运行期 src/back/reclaim.c:1001-1013（**默认 0 = off**） | >0 ⇒ walk/scan/push/assist 共用总预算 W，"一步的总工作量按构造 ≤ W" |
| EXTC_RC_TIME | 运行期 src/back/reclaim.c:62；发射开关 src/back/codegen.c:12402-12407（默认关） | 每个收集步计时并按桶统计（instrument，$ 只改运行时文本、不改分配路径） |
| EXTC_RC_PAUSE | 运行期 src/back/reclaim.c:2721-2728（默认关） | 记录单步停顿直方图并以 EXTC_RC_SLOWSTEP 阈值告警，atexit 打印（instrument） |
| EXTC_ARENA_FILL | 见 --help；只填非零 arena 分配 | 不改变字节内容（调试用） |
| EXTC_DBG_ARENA / _VERBOSE | src/back/codegen.c:5954-5962 | 只诊断，不改产物（见 §1.3） |
| EXTC_DBG_RCCOST | src/back/codegen.c:3896-3939（dbgRcCostReport） | 编译期打印每站点分配/屏障计数表：表头 lvl/ref/refInLoop/alloc/allocInLoop/rootMax，rootMax = 触发点根表槽数最大值（= 标记启动规模） |
| EXTC_NO_LEVELPASS | src/main.c 自述 **"CHANGES THE OUTPUT"**（414 个 golden 里 2 个的 arena 放置会变） | 跳过 arena level pass，用前端初值 ⇒ 仅用于定位问题 |
| 帧根链 | src/back/codegen.c:12402-12407（末参数传 0） | **[rt:chain] 是死代码**：注释逐字 "The frame root chain ... has no emitter pushing into the chain yet, so pushing code would be all dead => passing 0 emits nothing." |

---

## 8. 编译器插入的停顿分散机制（问题 8）

### 8.1 机制的形状：**没有独立的"轮询/让步"循环**，停顿被切成"分配点里的一小步"
编译器只做两件事：
1. **在 rc 分配点前**发一个触发点（§6.3），由 extc_rc_maybeCollect 决定"是否轮到收集"；一次 maybeCollect 最多走**一步**（markStep 或 sweepStep），不是一整轮。
2. rc 分配本身顺带推进在飞轮次：src/back/reclaim.c:1055-1073（extc_rc_alloc 开头）逐字：
       EXTC_RC_TICK(0); extc_rc_notePart(r);
       if (r->sweeping) { extc_rc_why = "alloc"; if (r->noSweep) r->sweeping = 0; else extc_rc_sweepStep(r, EXTC_RC_SWEEP_BUDGET); }
       if (r->marking) { if (r->msp != 0 || !r->seedsDone || r->scanObj) extc_rc_markAdvance(r); if (r->seedsDone && r->msp == 0 && !r->scanObj) extc_rc_finish(r); }
   即每次分配：若在 sweep 就推 8 words，若在 mark 就按节流推一段标记。

### 8.2 参数（全部是生成的 C 里的 #ifndef，运行期可用环境变量覆盖）
| 常量 | 定义（src/back/reclaim.c） | 默认 | 含义 |
|---|---|---|---|
| EXTC_RC_SWEEP_BUDGET | 888-890 | 8 | 一步 sweep 8 words = 512 slots（注释逐字 "Each step sweeps at most 8 words (64 slots/word)"） |
| EXTC_RC_SEED_PUSHES | 895-897 | 256 | 一步最多推入多少 seed slot |
| EXTC_RC_SEED_WORDS | 898-900 | 512 | 一步最多扫多少 rsbit 字 |
| EXTC_RC_MARK_EVERY | 901-903 | 64 | 每 64 次分配才推进一次标记（extc_rc_markAdvance 里 if (--r->markTick > 0) return; /* skip:mark-tick */） |
| EXTC_RC_MARK_STEP | 907-909 | 8 | 每次推进的基数（每分配点至少推进这么多对象） |
| EXTC_RC_MARK_WINDOW | 904-906 | 2<<20 | 分配字节相对 markStartAlloc 漂移超过 2 MiB ⇒ 预算 ×8（追赶） |
| EXTC_RC_MARK_BATCH_MAX | 910-912 | 256 | 单次 markAdvance 的预算上限 |
| EXTC_RC_MARK_BATCH_POP | 913-917 | 200 | 一次弹 200 个对象一起预取（注释：batch 宽度同时**决定遍历顺序**，步边界随之移动 ⇒ 收集时刻与峰值 RSS 都会变） |
| EXTC_RC_ASSIST_CAP | 998-1000 | 512 | 单步 assist 上限（债务再大也不许一步变成整轮 mark） |
| EXTC_RC_WORK_BUDGET | 1001-1013 | 0（off） | >0 ⇒ 三步共用总预算，单步总工作量**按构造 ≤ W** |
| EXTC_RC_ALLOC_INLINE | 1047-1055 | 1 | 分配体 always_inline（每分配点约 2.7 KB 机器码）；=0 改 noinline |

步预算计算（src/back/reclaim.c:1015-1019）：size_t d = r->wbDebt > 64 ? 64 : r->wbDebt; return EXTC_RC_MARK_STEP + d;
markAdvance 预算（src/back/reclaim.c:1023-1038）：b = EXTC_RC_MARK_EVERY * EXTC_RC_MARK_STEP + _debt; 漂移超窗 ⇒ b *= 8; b = min(b, BATCH_MAX); 若 WORK_BUDGET>0 则 b = min(b, W)。

### 8.3 是否"有界"取决于入口，编译器的默认路径只走有界入口
src/back/reclaim.c:2750-2806 四个入口：
- extc_rc_step：markStep(stepBudget) + finish（热路径，**只前进一步**）
- extc_rc_maybeCollect：触发点用；sweeping 则 sweepStep(SWEEP_BUDGET)，否则 if (!r->marking && (!extc_rc_shouldSweep(r) || !extc_rc_roundOk(r))) return -1; 再 step 一次
- extc_rc_begin：置 marking、markTick、markStartAlloc、wbAdd(1)、markBegin
- extc_rc_collect：**无界**——while (!seedsDone || msp || scanObj) markStep(r, 0xffffffffu) 一轮做完；这是"契约入口"（程序主动要求一次收集），**不是**编译器发出的触发点。

⇒ 关键结论：默认发射的每个触发点 = maybeCollect = **一步有界**；收集一轮要跨越"若干次分配点"才走完；因此单次停顿的上界由 MARK_BATCH_MAX/WORK_BUDGET 与 SWEEP_BUDGET 决定，而**一轮的总时长**由活跃堆规模决定（这正是后续优化方案要形式化的部分）。

---

## 9. 后端自带分配器与框架的关系（问题 9）

| 文件 | 提供什么 | 分配源 | 与 arena / rc 的关系 |
|---|---|---|---|
| src/back/reclaim.c（2889 行运行时文本，按 [rt:name] 分段：head/types/helpers/slab/budget/xregion/nocount/shade-cache/xentry/pin/chain/tlsdepth/mark/release/sweep/owe/counters） | 位图 slab 分配器 + 持有计数 + 区域标记/清扫 + owe/owed | 自己 malloc slab（生成物 /tmp/cf/cyclic_shape.c:107-163 ExtcRcSlab/ExtcRcClass 字段 big/rcp/avail/clsBkt/ownMap） | **rc 只治理** new T / new [N]T（仅 level 1）/ alloc<T>(n) / @overwrite 单值 cell。region 由 arena 地址 claim，**寿命 = 那只 arena** |
| src/back/codegen.c:9646-9652 + 运行期 arena 文本（[rt:head]） | extc_arena bump 分配、release（保留 1 个 spare block，上限 EXTC_ARENA_SPARE_MAX = 1<<20）、destroy、越界 trap | 每块 calloc（cap 倍增，下限 64） | 帧局部对象；**不是** pool，也**不**参与 rc（除上面四类治理点的额外记录） |
| src/back/places.c（315 行） | place 载荷区：place 槽表（extc_place_enter/leaveTo/newRoot/freeRoot/alloc）+ 每槽一条 malloc chunk 链 + dyn 槽表（extc_dyn_put / extc_dyn_slot，四次比较判陈旧：zi / zgen / gen / slot）；src/back/pools.c 与 extc_pool_* 已删除（v38） | 全 malloc/calloc/free | 尾注释（src/back/places.c:19-24）自述「A place is a scope of storage, not a value」；dyn 载荷是它的唯一消费者 |
| src/back/plate.c（74 行） | 只发两个门：extc_viewOf(uint8_t *p, int64_t n) 造 slice_u8（:35-38，零检查）、extc_memCopy（:69-73，memcpy） | **不分配** | 视图不是根、不是 rc 槽（src/back/codegen.c:3545-3617 注释：视图 data 指向 arena/plate/C 存储） |
| src/back/memfind.c（126 行） | extc_memFind（needleLen<=32 走 memchr+memcmp，否则 memmem）、extc_memEq（memcmp==0） | **不分配** | 只读查找 |
| src/back/coroutine.c（552 行） | 任务表（realloc 增长）+ **每任务一个 extc_arena**（:57 static extc_arena *extc_taskArena;）+ 任务根 place 注册（extc_taskRoots[64]） | 帧 = 任务 arena bump（:124-130 extc_task_alloc ⇒ extc_arena_alloc）；表 = realloc | 任务帧**不走 rc**：任务自己的 arena 一次 release/destroy（:145 extc_arena_destroy(&extc_taskArena[id]) 注释：任务不会再跑，spare 块会永远占着）；extc_task_collect 按根 place 可达性整任务回收 |

**一句话**：arena 管"帧语法块"、rc 管"逃逸到块层以上的引用图对象"、pool 管"容器数据块"、coroutine 管"任务帧"——四套独立分配器，唯一的联系是 rc 的 region **以 arena 地址为身份**，因此 rc 对象的寿命被绑定在 arena 的释放点上。

---

## 10. 生成代码的内存模型"形状"（问题 10）

一个典型函数的时间线（用 /tmp/cf2/owed_home.c 的真实片段串起来；源 tests/reclaim-shapes/owed_home.extc）：

**① 帧入口（零分配）**
    static node * build(int64_t n, extc_arena *__extc_home, EXTC_UNUSED int32_t __extc_homeOwn) {
      extc_arena __extc_a[3] = {0};
      ExtcRcRegion *__extc_rcr0 = extc_rc_regionOf((void *)&((*__extc_home)), -1);
（owed_home.c:2821-2824）两块栈上状态：arena 数组、region 指针；region 可能就地新建（首次 claim），此后复用。

**② 分配点前：触发或欠账（有界一步）**
    if (__extc_homeOwn) { if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_maybeCollect(__extc_rcr0, (void *const *)0, 0); }
    else { if (extc_rc_shouldSweep(__extc_rcr0)) extc_rc_owe(__extc_rcr0); }
（owed_home.c:2826-2829）arena 属本帧 ⇒ 真的收；属调用者 ⇒ 只记一笔欠账。

**③ 分配与写入**
    node * p = extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(node), &__extc_rct0);
    extc_rc_storeRefOwnedReg((void **)&(p->next), (void *)(cur), __extc_rcr0);
（owed_home.c:2840-2850）地址不动：p 是 slab slot 的地址；写引用字段 = 注册一条边（owner region 静态已知）。

**④ 帧出口（唯一释放点）**
    extc_arena_destroy(&__extc_a[1]);  extc_arena_destroy(&__extc_a[2]);
（owed_home.c:2860-2861）本帧自有的块层 arena 连 spare 一起 free；rc region 在**拥有它的帧**里 drop：
    extc_rc_regionDrop((void *)&__extc_a[1]);
    extc_arena_destroy(&__extc_a[1]);
（owed_home.c:2905-2907，main 的帧尾）

**⑤ 调用者侧：先结算欠账，再传 home**
    { ExtcRcRegion *__extc_rcc = extc_rc_regionOf((void *)(&__extc_a[1]), 1);
      if (extc_rc_owed(__extc_rcc)) extc_rc_checkOwed(__extc_rcc, (void *[]){ (void *)(keep) }, 1); }
    extc_rc_storeRefLocal((void **)&(keep), (void *)(build(20000, &__extc_a[1], 0)));
（owed_home.c:2894-2895）**"上一批被调已返回、下一批未进入"** 的位置结算；home 指针 + homeOwn 标志一起显式传下去。

**⑥ 循环体：每轮一个 release（内存按"一轮"有界，与迭代次数无关）**
    extc_arena_release(&__extc_a[1]);        /* 循环体入口清层（owed_home.c:2885 附近）*/
（src/back/codegen.c:6720-6733 注释逐字：a loop body is a block too, so each iteration clears once -- the memory a loop uses is therefore bounded by one iteration, not by the iteration count.）

**⑦ 块/条件层的分配（无 rc 的纯 bump 形态，来自 /tmp/cp-p.c）**
    while (...) { int64_t * p = (int64_t *)extc_arena_alloc(&__extc_a[2], (int64_t)(250000) * (int64_t)sizeof(int64_t), "tests/arena/promote.extc", 13);
                  ... 出块时 extc_arena_release(&__extc_a[2]); }

**一句话时间线**：进函数 ⇒ 栈上 { arena 数组, region 指针 } 就位（无堆动作）⇒ 每个分配点"先问一句该不该收（有界一步）+ 在该层的 bump/slab 里原地取地址"⇒ 引用写入用编译期选定的最便宜屏障登记边 ⇒ 出块 release 清一层、出循环每轮清一次、出帧 destroy+regionDrop ⇒ 期间收集只由分配点驱动、一轮跨多个分配点走完，且**从不移动对象**。

---

## 11. 未验证项 / 存疑（不要当结论引用）

1. **闭包**：未在 src/back/codegen.c 里定位闭包的运行时表示（grep "closure" 只命中"求不动点的闭包"这类术语）⇒ 闭包捕获是否分配、分配到哪层、是否算逃逸，全部**未验证**。
2. **tests/reclaim-shapes/unknown_host.extc 的头注释** 断言该形状会发射 extc_rc_storeRefUnknown，但实测产物只发 extc_rc_storeRefSlot（/tmp/cf2/unknown_host.c:3236），storeRefUnknown 仅由 storeRefSlot 在运行期回退可达。⇒ doc/tests 与产物**不一致**，以产物为准。
3. **tests/asan/promote-loop.extc 为何完全不发射 rc**：该例确有 new node 且 head=cell 重赋值、head 活到函数结束，但产物 0 个 rc 符号（/tmp/cp-loop.c）。推测是 unitReclaimIdle 的判据把它判成 idle，但**未逐条核对** check_top.c idle 判据 ⇒ 存疑。
4. **"[lvl] 175 level facts, converged after 32 round(s)"**：32 恰是 check_top.c:9525-9555 的重放上限，无法区分"真收敛"与"撞上限被截断"（代码里未见截断告警）⇒ 待核。
5. **EXTC_RC_SHARDS>1 未实测**：分片逻辑全部来自读码（codegen.c:476-497/4096-4181/13764-13776），未生成并比较 N=2/4 的产物与行为。
6. **对照产物未生成**：EXTC_RECLAIM=0、EXTC_RC_NOCOUNT=0、EXTC_RC_LOCALSTORE=0、EXTC_RC_SWEEPFREE=1 的逐字节 diff 均未做（--help 的"=0 逐字节还原"只是自述）。
7. **extc_rc_storeStructCons 未观测到发射**（唯一发射点 codegen.c:7717）；触发它需要 RC_CONSERVATIVE 的整块赋值样本。
8. **类型描述符生成规则**（rcScanType / rcEmitTypeDesc，codegen.c:3619-3626 附近）未逐行核；td 为 -1 表示"保守扫每字"这一约定来自注释与产物。
9. **4096 根表上限的口径**：rcEmitTriggerNow 用 n = globals.len + locals.len（codegen.c:4101-4186），rcRootLeaves 用 rcRoots.len > 4096（codegen.c:4292-4329）⇒ 两处口径不同，需对齐。
10. **C 互操作内存的所有权**：外部声明/未检查函数体、extc_viewOf 之后的 C 内存由谁负责、是否可能进入 rc 图——未验证。
11. **coroutine 内 new 的归属**：任务 arena 是 bump（coroutine.c:124-130），但 needReclaim 单元里任务内 new 是否走 rc、region 归谁，未实测（/tmp/cf/coro.c 只确认任务表与 taskArena）。
12. **arena block cap 倍增与 spare 上限**来自生成物运行时文本（/tmp/cp-loop.c:97-142、61-76），需与 src/back/reclaim.c 的 [rt:head] 段逐字对齐后再作为定稿依据（本轮按"生成物 = 真相"采信，但未与源码段 diff）。
13. **@overwrite 的独立路径**（planSetOwSites / planSetOwLocal，check_top.c:9734-9759；codegen 侧 owNow/owcNow）只读了入口，未展开其分配/释放语义。

---

## 12. 复现命令

    cd /home/alphayang/extC_Compiler
    mkdir -p /tmp/cf /tmp/cf2

    # 无 rc 的最小样本（arena 层）
    ./build/extc tests/arena/promote.extc              -o /tmp/cp-p.c
    ./build/extc tests/asan/promote-loop.extc          -o /tmp/cp-loop.c

    # 有 rc 的样本（环 / 跨区 / home 欠账 / 分片 / 屏障分派）
    for f in tests/reclaim-shapes/*.extc tests/reclaim-acyclic/*.extc; do
      b=$(basename "$f" .extc); ./build/extc "$f" -o "/tmp/cf2/$b.c" 2>/tmp/cf2/$b.err || echo "FAIL $b (neg case?)";
    done

    # 统计发射了哪些机制
    grep -c "extc_rc_"        /tmp/cf2/cyclic_shape.c
    grep -n "maybeCollect\|extc_rc_owe\|extc_rc_regionOf\|regionDrop" /tmp/cf2/owed_home.c
    grep -n "storeRefLocal\|storeRefOwnedReg\|storeRefOwnedRelease\|storeRefSlot\|storeRefSameRegion" /tmp/cf2/struct_copy.c

    # 诊断开关
    EXTC_DUMP_LVL=1      ./build/extc tests/arena/promote.extc -o /tmp/x.c
    EXTC_DUMP_EFFECTS=1  ./build/extc tests/arena/promote.extc -o /tmp/x.c
    EXTC_DBG_ARENA=1 EXTC_DBG_ARENA_VERBOSE=1 ./build/extc tests/reclaim-shapes/cyclic_shape.extc -o /tmp/x.c
    EXTC_DBG_RCCOST=1    ./build/extc tests/reclaim-shapes/cyclic_shape.extc -o /tmp/x.c
    EXTC_RC_SHARDS=4     ./build/extc tests/reclaim-shapes/cyclic_shape.extc -o /tmp/x4.c

    # 运行期调参（改的是生成的 C 里的 #ifndef 默认值）
    EXTC_RC_FLOOR=262144 EXTC_RC_SWEEP_BUDGET=4 EXTC_RC_MARK_EVERY=32 \
    EXTC_RC_WORK_BUDGET=256 ./a.out

---

## 13. 编译侧改动记录（§1–§12 取证之后落地的修复）

本节记录本文档首次取证之后、在 src/back/codegen.c 与 src/check/check_top.c 上落地的修复。§1–§12 的模型事实仍然成立；与之冲突的个别行（§5 的 initRefNoHold 条件、§7 的开关副作用）以本节为准。行号均为当前工作树（2026-11-09，HEAD 85b8871b + 未提交改动）。

### 13.1 P0-6 任务根登记与配平

运行期在 src/back/coroutine.c 的 coroutineEmitRuntime 文本里：static int64_t *extc_taskRoots[64];（:68）、extc_task_rootPush 满表返回 0（:70-75）、extc_task_rootPop 是盲弹（只 rootN > 0 时自减）。extc_task_collect 在 src/ 中没有任何调用者（grep 只命中自身定义与前置声明）⇒ 当前登记只延长任务寿命，不改变既有行为。

发射侧（src/back/codegen.c）：

    static void cgTaskRootPush(CG *g, const char *slotExpr) {
        if (!g->taskRoots || !g->taskRootAt || g->blkLevel < 0 || g->blkLevel >= g->levelCap) return;
        int trk = g->taskRootSeq++;
        cgLine(g, "int64_t __extc_trk%d = extc_task_rootPush(%s);", trk, slotExpr);
        *(int *)vecPush(&g->taskRootAt[g->blkLevel]) = trk;
    }

（:6226-6231；docstring :6203-6225 说明登记哪四类槽：d.run 体里的 ext 帧、栈帧 spawn、局部 boxed handle 的 .task、拷贝到另一局部时在该变量的声明处；聚合体/容器/结构体字段不登记，方向只多保活。）弹出用守卫局部量配对：if (__extc_trk%d) extc_task_rootPop();（:6249 起；表满时 push 返回 0，不能盲弹偷别人的槽）。

登记点：cgTaskRootPush 于 :3039（exExt 的 &__extc_dtid%d）、:7580（boxed handle 的 &%s.task）、:7599（栈帧 spawn 的 &__extc_czid%d，if (!g->coroFunc)）、:7911（handle 拷贝到另一局部时该局部的 .task）。弹出点：cgReturn 两条返回路径 :5635/:5652（cgTaskRootPopsForReturn，不清表）、cgReleaseLevel :6311（传 g->atBlockEnd）、块尾 probe :6984（true,true）、d.run 驱动之后 :8809（bodyLevel，false,true，ST_DOMAIN）。

clear 语义（break/continue 真 bug 的修复）：只有普通块尾清表，早退路径只发弹出、保留清单，因为块自身的 release 文本对 fall-through 路径仍然有效——if (clear) v->len = 0;（:6165，cgCoroTaskEndAt；:6200 cgCoroOwnedEndAt；:6249 起 cgTaskRootPopAt）。EXTC_TASK_ROOTS=0 是编译期 kill switch（默认 1，:13981 附近）。验收：tests/zone/task_root_break.extc --run 打印 live=0；无 break 对照 tests/zone/task_root_break_nobreak.extc 同样 live=0；EXTC_TASK_ROOTS=0 时两例产物只少 trk 行。

### 13.2 P0-5 initRefNoHold 只在无计数档发射

    if (planRcNoDec(g->curFunc) && g->rcNoCount)
        cgLine(g, "extc_rc_initRefNoHold((void **)&%s, (void *)%s);", nm, nm);
    else
        cgLine(g, "extc_rc_initRef((void **)&%s, (void *)%s);", nm, nm);

（:7861 起。）计数档的级联会在槽对象死亡时按槽减计数，NoHold 只写不加会让该次减计数没有对应的加计数 ⇒ 目标提前归零/释放；因此只有无计数档（NoHold 等价于普通写）保留该优化。

### 13.3 P1-3 REG_MAX 满表可观测

运行时文本（:12699 起）现在带可 grep 的宏与一次性告警开关：

    /* EXTC_RC_REG_FULL_POLICY 1: when the table is full, a new region is not entered and
     * is never dropped -- leaking a region beats freeing it early (see the note above).
     * extc_rc_regWarned makes the diagnostic one-shot. */
    #define EXTC_RC_REG_FULL_POLICY 1
    #define EXTC_RC_REG_MAX 512
    static void *extc_rc_regKey[EXTC_RC_REG_MAX];
    static int extc_rc_regWarned;

语义不变：表满时 regionOf 仍是不登记、不释放（宁漏不早放）。告警受编译期开关保护（生成物里的 #if EXTC_RC_TIME），默认编译产物与该诊断默认静默；EXTC_RC_TIME=1 ./extc ... 编出的程序在第一次满表时打印一行 extc: rc region table full (EXTC_RC_REG_MAX=512); later regions are never dropped。

### 13.4 P1-4 产物档位 EXTC_RC_TIER（0/1/2/3）

emitPreamble 内（:12610-12637）：只在 g->needReclaim 为真时发一行；EXTC_RECLAIM=0 把整个回收运行期删掉，也就没有标记（档 0 以“无标记”表示，且不能给不存在的运行期发宏）。映射逐字：

    int tier;
    if (g->rcNoCount == 0 && g->rcSweepFree == 1)        tier = 1;
    else if (g->rcNoCount == 1 && g->rcSweepFree != 1)   tier = 2;
    else if (g->rcNoCount == 0 && g->rcSweepFree != 1)   tier = 3;
    else                                                 tier = 2;   /* not reachable; keep the sweep label */
    bufPrintf(out, "#define EXTC_RC_TIER %d   /* %s */\n", tier,
              tier == 1 ? "event tier: holder counting on, region sweeping off"
              : tier == 2 ? "sweep tier: holder counting off, region sweeping on"
              : tier == 3 ? "counted+sweep tier: both on (NOCOUNT=0 forced)"
                          : "sweep tier: region sweeping on");

0 = 无回收运行期（needReclaim 假）；1 = 事件档（计数开、清扫关）；2 = 清扫档（计数关、清扫开，默认）；3 = 计数+清扫（NOCOUNT=0 强制，或 SWEEP_FREE=0 禁止事件档）。最后一个 else 是 noCount==1 且 sweepFree==1 的组合，当前不可达（EXTC_RC_SWEEPFREE=1 在 :14013 附近强制把 rcNoCount 置 0；自动升级同样置 0），保留文案只为穷举；可达状态下注释与随后生成的 EXTC_RC_NOCOUNT/EXTC_RC_SWEEP_FREE 一致。实测见 §14 矩阵。

### 13.5 P0-7 参数槽覆盖（计数档 insert-only store）

赋值目标若是当前函数的参数槽，计数档改发只插入的 initRef，不递减旧值：

    if (!g->rcNoCount && cgTargetIsParamSlot(g, s->u.assign.target)) {
        cgLine(g, "extc_rc_initRef((void **)&(%s), (void *)(%s));", tgt, val);
        return;
    }

（src/back/codegen.c:8961-8964。）判定器 :8699-8715 cgTargetIsParamSlot：目标必须是 EX_IDENT，C 名取 planCName(t) ?: t->u.ident.name；被遮蔽的局部在 checker 里改名（a__2），所以与参数 cname/name 重名者只能是该参数。理由（注释 :8951-8960）：参数槽在入口只登记为 root、从不 initRef，旧值没有自身计数；普通 store 的 -1 会提前释放调用方别名。insert-only 最多让一个引用晚回收。无计数档不走此路径（那里普通 store 是 storeRefLocal 纯写，已安全且输出必须逐字节不变）。验收：tests/gc-p0/src/uaf_param_overwrite.extc 在默认档与 EXTC_RC_NOCOUNT=0 下都打印 alias check b.v=999（修复前计数档为 111）。边界（Lead 确认，不改代码）：insert-only 只在**强制事件档**（TIER 1：计数开、清扫关）线性泄漏，量级约 20 B/轮（每轮一个新值、旧值不再回收）；默认清扫档 T2 与 T3 由 region 清扫兜底，循环结束后随 region 一起回收，不构成无界增长。

### 13.6 P0-7b 批量字节搬移（copyInto）的引用记账 + (iv) 保守退回清扫档

生成器（genViewCopier，src/back/codegen.c:1284 前向声明、:1311-1326）：memmove 之后再对目的地逐元素记账：

    if (g->needReclaim && typeHasRefSlots(g, elem))
        cgLine(g, "for (int64_t __extc_bk = 0; __extc_bk < n; __extc_bk++)"
                  " extc_rc_storeStructCopy((void *)&d.data[__extc_bk], sizeof(%s), (void *)d.data);",
               cType(g, elem));

先 memmove 保持重叠语义（string::removeFront 会自重叠搬移），再记账；typeHasRefSlots = rcScanType(g, t, 0) != RC_NO_REFS（:3775），纯数据元素类型没有该循环（i64 copyInto 的 HEAD 产物逐字节 0 行差异）。不要换成 storeRefSlot：memmove 已经把旧值覆盖成新值，再减旧值会净欠一份计数。

记账入口是运行期新函数 extc_rc_storeStructCopy(dst, size, base)（契约由 runtime-facts 在 src/back/reclaim.c 实现）：用基址 d.data 经 extc_rc_slabOfInterior 证明宿主 region；证明成功时逐引用 word 做 shade / extc_rc_regionOfOwn 跨区 rsInc（并置 r->foreign=1）/ inc，**不**把目标 region 逐出清扫；只有基址不在 rc 存储（栈或 pool 视图）才退回老方向 extc_rc_storeStructCons(dst, size, (void *)0, 0)（noSweep，保守）。实测依据：main 里 new item[n] 生成的正是 extc_rc_alloc（见 /tmp/cf-ub.c:3523、3534 "extc_rc_alloc(__extc_rcr0, ...)"），基址 d.data 是 slab 内部指针，slabOfInterior 可解。

历史（BUG-C，已修）：第一版发的是 extc_rc_storeStructCons(..., (void *)0, 0)，hostKnown=0 对每个被引用对象执行 rv->noSweep = 1，而 noSweep 没有清除点 ⇒ 该 region 永久退出清扫、inc-only 计数永不配对：默认档 TIER 2 在循环 copyInto 下线性泄漏（doc-audit 实测 50k 轮 maxRSS 2640 KB、200k 轮 5520 KB、800k 轮 17616 KB；对照不 copy 的平台化 2640→2832 KB）。

分析侧（`src/check/check_top.c` 的 `acScanExpr`，`case EX_GENCALL`）：`copyInto` 的类型实参首位若 mentionsParam 或 typeHoldsCountedRef，则把本次 unit 的 (iv) 判为假（ivBad、ivWhy 给出原因），程序不被拒绝，unit 退回清扫档（TIER 2）；`copyInto<i64>` 仍过。（v39：`poolResize` / `poolResizeRaw` 两个名字随池原语族删除，判据只剩 `isCopyIntoName`。）理由写在注释里：判据 (iv) 的前提“每个引用的掉落都经过计数槽”看不见一次 memmove 的字节复制。(iv) 原文见 check_top.c:6166-6210。

验收（build-cf/extc）：lead 形状 /tmp/cf-bulk2.extc 默认档现在 TIER 2（修复前 TIER 1）、输出 d0.v=7（修复前 555）；EXTC_RC_NOCOUNT=0 强制计数档 TIER 3、输出 d0.v=7；EXTC_RC_SWEEPFREE=1 强制事件档 TIER 1 也输出 d0.v=7。tests/gc-p0/src/uaf_bulk_copy.extc 默认档 TIER 2、计数档 TIER 3，两档打印同一值 7；ASAN 无报告。

### 13.7 副作用、既有红项与文档一致性

- 隔离基线（HEAD 源码 + 只覆盖 codegen.c / check_top.c / check_stmt.c，/tmp/mine2-src/build-mine2/extc）对 HEAD 产物 diff：promote.extc 0 行；acyclic/cyclic 默认档各 17 行（TIER 行 + P1-3 的 REG_FULL_POLICY 文本）；cyclic + EXTC_RECLAIM=0 0 行；copyInto 纯数据形状 0 行；引用元素 bulk 形状 135 行（档位回退 + 记账循环）；参数槽计数档 19 行。除上述预期文本外无其他污染。
- 集成树（build-cf，含其他 teammate 在途的 src/back/reclaim.c 改动）当前 tests/reclaim/run.sh 与 tests/coro 红：reclaim 运行期文本里的诊断 fprintf 参数与格式串不匹配，-Werror=format 失败（如 reclaim.c:2652 的 [slow] 行）；不是本节改动引起，等 reclaim.c 收尾后复跑。隔离基线上 coro 37/37、zone 全过、reclaim-shapes exit 0。
- G13 判据已对齐（empirics 改用例打标签）：tests/gc-p0/src/uaf_bulk_copy.extc 现在输出 d0r=7，run.sh 判据也是 d0r=7。实测 run.sh g12（EXTC=build-cf/extc）：G12 强制计数档 T3 PASS 'alias check b.v=999'（修复前 111）、G13 默认档 T2 PASS 'd0r=7'（修复前 555）、G13 强制计数档 T3 PASS。
- tests/gc-p0/summarize.py 的档位文案仍是“TIER 宏只分 1/2”，是四值改动前的旧叙述；它用 NOCOUNT/SWEEP_FREE 反推档位，判据本身不受影响，但报告里 T3 的括号说明需更新。
- promote-place 行在 tests/reclaim-shapes/run.sh 为红；HEAD 与隔离基线两处都红 ⇒ pre-existing，非本次改动。
- tests/arena-promoted 的 D1_assoc_publish_home 在 HEAD 原版即 heap-use-after-free（tests/arena-promoted/D1_assoc_publish_home.extc:40）⇒ pre-existing。

---

## 14. 验收命令与实测输出

    cd /home/alphayang/extC_Compiler
    make BUILDDIR=build-cf -j 8              # 0 warning

    # 档位矩阵
    for f in tests/reclaim-acyclic/acyclic_heapref.extc tests/reclaim-shapes/cyclic_shape.extc; do
      for e in "" EXTC_RC_NOCOUNT=0 EXTC_RECLAIM=0; do
        env $e ./build-cf/extc -w --no-line-map $f -o /tmp/t.c
        grep -hoE '#define EXTC_RC_(TIER [0-9]+|NOCOUNT [01]|SWEEP_FREE [01])' /tmp/t.c
      done
    done

实测：acyclic 默认 TIER 1/NOCOUNT 0/SWEEP_FREE 1（EXTC_RC_NOCOUNT=0 仍 TIER 1，因为升级已给同一状态）；cyclic 默认 TIER 2/NOCOUNT 1/SWEEP_FREE 0；cyclic + NOCOUNT=0 TIER 3/NOCOUNT 0/SWEEP_FREE 0；两者 + EXTC_RECLAIM=0 均无任何 TIER 行；promote.extc（无 rc）无 TIER 行。

    # P0-7b / P0-7 两档运行（gcc -O0 -fsanitize=address 编译）
    ./build-cf/extc -w --no-line-map /tmp/cf-bulk2.extc -o /tmp/b.c
    gcc -O0 -w -fsanitize=address /tmp/b.c -o /tmp/b && /tmp/b
    EXTC_RC_NOCOUNT=0 ./build-cf/extc -w --no-line-map tests/gc-p0/src/uaf_param_overwrite.extc -o /tmp/p.c
    gcc -O0 -w -fsanitize=address /tmp/p.c -o /tmp/p && /tmp/p

实测：bulk2 默认 d0.v=7、计数档 d0.v=7、强制事件档 d0.v=7，rc=0；param 两档 alias check b.v=999、ASAN 无报告；task_root_break --run live=0。

剩余存疑/未验证（补 §11）：

1. extc_task_collect 仍无调用者（src/back/coroutine.c 内只有定义与前置声明）⇒ P0-6 目前只是为将来的整池 collect 接线，登记不改变任何现有运行期行为。
2. (iv) 对 poolResize/poolResizeRaw 的退回是否必要未单独证明（v39：这两个名字连同名表一起删除，现存 `copyInto` 仍在走 (iv)）：pools.c 的 resize 是 realloc 搬移（同一对象集合，按对象计数不变），不像 copyInto 那样在目的地制造第二个指针；这里的处理方向是保守（只影响档位，不拒绝程序），是否可从 (iv) 收紧另行验证。
3. 引用元素 copyInto 的逐元素 storeStructCons 在强制事件档（EXTC_RC_SWEEPFREE=1，TIER 1）下已验证输出正确且 ASAN 干净，但没有在“事件档 + 大量 churn”的压力形状下测过暂停/内存界。
4. TIER 标记未进入任何 golden 判据（tools/golden.sh 只把逐字节差异当观察项、判据是合法 C），所以它的稳定性靠本节矩阵而非门禁。

### 14.1 G14 泄漏门禁（P0-7b，官方用例）

    ./build-cf/extc tests/gc-p0/src/leak_copy_refs.extc -o /tmp/g14.c
    gcc -O2 -std=c11 -fwrapv -o /tmp/g14 /tmp/g14.c
    /usr/bin/time -f '%M' /tmp/g14 50000
    /usr/bin/time -f '%M' /tmp/g14 800000
    EXTC=$PWD/build-cf/extc OUT=/tmp/gc-cf2 REPS=1 tests/gc-p0/run.sh g14

实测（build-cf/extc，默认档，产物 #define EXTC_RC_TIER 2；判据 ratio = maxRSS(800k)/maxRSS(50k) ≤ 1.20）：

- 50000 轮：2648 KB（5 次重复全 2648）；800000 轮：3220–3224 KB（5 次）⇒ ratio = 1.216–1.218，官方 harness 判 RED <=1.20x（原文 "50k maxRSS = 2648 kB / 800k maxRSS = 3224 kB  ratio = 1.218x  exit=[0, 0]  [RED <=1.20x]"；同次记录还打出"记账自证：产物 g14.c 中 extc_rc_storeStruct* 出现 2 次"）。
- RSS 曲线（同一二进制，逐档单次）：50k 2648 / 100k 3224 / 200k 3224 / 400k 3224 / 800k 3224 / 1.6M 3224 / 3.2M 3224 / 6.4M 3224 KB ⇒ 100k 起完全平台，内存有界；2,000,000 轮另测 3032 KB（低于 800k 峰值，属采样/分配器相位波动）。
- 归因对照：把 copyInto 那一行删掉、其余不变（sum 改读 src[0].r.v）的对照程序 /tmp/g14_ctl.extc，同编译器同档位：50k 2648 / 100k 3224 / 200k 3224 / 400k 3224 / 800k 3224 / 1.6M 3224 / 3.2M 3224 KB —— 与含 copyInto 的曲线逐点相同。⇒ 1.218× 的超阈来自该形状本身在 50k→100k 之间一次性抬升约 576 KB 的堆高水位（arena/region 首次扩容与 free-slab 保留），不是 P0-7b 记账造成的增长；P0-7b 把 copyInto 形状从修复前的 6.68×（2636→17616 KB）拉回到与"完全不做 copyInto"的形状逐点一致。
- 结论：门禁口径对 50k 基线的一次性开销敏感；按 ≤1.20× 死判，同一台上任何同形状（含无 copyInto 对照）都会红。**Lead 2026-11-09 裁决采用判据 (b)**：「maxRSS(800k) ≤ 1.02 × maxRSS(对照：同程序不做 copyInto，或 100k 起基线) 且 maxRSS(800k) ≤ 4 MB」，同时照旧报 50k / 100k / 800k 三个点与 ratio。按该判据：对照 800k=3224 KB、比值 3224/3224 = 1.000×、绝对 3224 KB ≤ 4 MB ⇒ PASS。run.sh/用例由 empirics 改，本节只留命令与数字。

### 14.2 L10-b / L10-c：copyInto 记账的两处漏账（2026-11-16 已修，各带一个硬门禁）

**现状：L10-b 与 L10-c 都已关闭**（同一提交带 `tests/gc-p0/l10_check.py` 与 `run.sh` 的 L10 段）。L10-a（pool 槽里的引用赋值 ⇒ 永久 pin）已由外部边表按槽地址登记兜住（G15 由 XFAIL 提升为硬判据），但池的 free 仍不显式撤边，撤销一侧随 R4c 迁 `buf` 收口，见本节末尾。

读者注意：P0-7b（2026-11-09）只修复了"目标 view 在 rc 存储且目标槽不被反复覆盖"的形状（uaf_bulk_copy 与 G14 reference）；下面两条是它没覆盖的两类，2026-11-16 才关掉。历史现象与根因保留在两小节里，因为它们是这两个回归用例的由来。

#### L10-b：目的 view 由 arena 分配 ⇒ fallback 的 noSweep 永久 pin

- 形状构造（/tmp/leak_copy.extc，doc-audit）：循环体内 var s = new item[2]; var q = ref s[0]; q.r = new node; var d = new item[2]; copyInto<item>(d, s, 1)——s/d 都落在循环体 arena（生成物 extc_arena_alloc(&__extc_a[2], ...)，/tmp/new-leak_copy-800000.c:3662、3672），因此 d.data 基址不在 rc 存储。
- 机理：extc_rc_storeStructCopy 里 slabOfInterior(base) 失败 ⇒ 退回 extc_rc_storeStructCons(dst, size, (void *)0, 0)；hostKnown=0 对每个被引用对象执行 rv->noSweep = 1（src/back/reclaim.c 的 storeStructCons 文本），而 noSweep 没有清除点 ⇒ 该 region 永久退出清扫 ⇒ 线性积累。
- 实测：50k 2640 / 200k 5520 / 800k 17616 KB（与修复前完全相同，新入口未改善）；把生成物里该 fallback 改成 no-op 后 800k = 2832 KB 平坦、ASAN 干净、输出 acc=5600000 正确 ⇒ 根因确为 noSweep pin。判据（沿用 G14 旧口径 maxRSS(800k) ≤ 1.2 × maxRSS(50k)）：当前 6.67× ⇒ RED。
- 修复方向 P0-9：给 noSweep 加清除点（region 生命周期/世代），或编译侧要求 copyInto 目的 view 必须是 rc 基址（否则拒绝或静态告警）。
- **2026-11-16 落地（第二条的推广形态）**：只要元素类型含引用（`rcScanType(g, w, 0) != RC_NO_REFS`），`new T[n]` 一律放进 rc 区，不再看 `planArenaLevel`，描述符取保守值。理由写进 src/back/cg/80-roots-and-triggers.inc：arena 槽不是 rc 槽，`storeRefSlot` 找不到宿主就退回 `storeRefUnknown` ⇒ pin 表；而 arena 复用时会 memset，pin 撤不回来。实测 50k/200k/800k 的 RSS **3,616/3,616/3,616 KB**（线性泄漏消失），rs_inc=rs_dec=0。
- 判据：`l10_check.py` 的 L10-b 段（maxRSS(800k) ≤ 1.25 × maxRSS(50k)，并核对 stdout 的 `L10B` 行）。

#### L10-c：rc 基址但目标槽被反复覆盖 ⇒ rsInc 边只增不减

- 形状构造（/tmp/cf-leak）：d 在函数体 new item[1]（rc 基址），源 s 在循环体 arena；每轮 s[0].r = new node; copyInto<item>(d, s, 1)，同一目标槽 d[0].r 被反复覆盖。
- 机理：storeStructCopy 对每个新值做 rsInc（跨区边）并置 r->foreign=1；memmove 已把旧值覆盖掉，但**旧边不撤销** ⇒ 旧 node 永远有跨区入边、不可回收。
- 实测：50k 6660 KB → 800k 93544 KB（旧实现 85592 KB）⇒ 不有界；判据 800k ≤ 1.2 × 50k 为 RED。
- 修复方向 P0-10：边记账改成"按槽位去重 + 覆盖时撤销旧边"（src/back/reclaim.c 的 unlinkChildren 已有按边撤销机制，可复用），或把 copyInto 的记账从 inc-only 改成"先 dec 旧槽、再 inc 新槽"。
- **2026-11-16 落地（第二条）**：`genViewCopier` 在 memmove **之前**发射一趟只读 pre-pass，对目标区每个「可枚举引用叶子」调新入口 `extc_rc_decOldRef`（src/back/rt/61-release.inc）：旧值不是本运行期的槽就跳过、同区跳过、跨区则 `rsDecSlab`（计数档再 `holdUpdSlab(-1)`）。顺序固定 dec → memmove → 只增记账，所以旧边在值消失之前撤销。
- 实测：50k 6,724 → **3,616 KB**、200k 23,248 → 3,616、800k 92,820 → 3,616（平）；`rs_inc=n、rs_dec=n-1、rs_live=1`（剩下的那条属于仍活的 `d` 槽）。
- 判据：L10-c 段（maxRSS(800k) ≤ 1.25 × maxRSS(50k)、`rs_live ≤ 8`、stdout 的 `L10C n=… sum=…` 两条）。
- 已知边界：元素类型是保守描述符（`EXTC_RC_CONSERVATIVE`）时没有 pre-pass（叶子不可枚举），这类元素仍是 inc-only；语料里没有这种形状（17 个大槽类型都是内联数组，叶子可枚举）。

#### 与 L10-a 的关系

L10-a（pool 槽里的引用赋值 ⇒ 永久 pin，即 tests/gc-p0 G15 的形状）与 L10-b/L10-c 是同一族"记账只增不减 / 永久 pin"问题：L10-a 属运行期 pool 路径，L10-b/L10-c 属 copyInto 记账路径（编译侧发射 + 运行期入口）。三者都不阻塞本轮交付；但任何"copyInto 已修好"的结论都必须限定在 G14 reference 形状（buf/src 都在函数体 ⇒ rc 基址、目标槽固定）。
