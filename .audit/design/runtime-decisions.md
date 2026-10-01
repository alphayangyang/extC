# 运行期线 · 审计后的设计裁定（pool / heap / coroutine / concurrency / C-ABI / module / IO）

> **单元**：`docs/topics/` 的 `POOLS.md` · `POOL-SOUNDNESS.md` · `HEAP.md` · `CONCURRENCY.md` · `C-ABI.md` ·
> `MODULES.md` · `IO.md` · `INLINE-C.md` · `PLUGINS.md` · `LIBS.md`（10 份，共 5129 行，逐份读完）。
> **代码基准**：`HEAD = 95e7526`（2026-09-30 01:26「修复编译器正确性与内存安全缺口」），
> 工作树干净（只有 `.audit/` 与审计报告未跟踪）；`95e7526` 只修了 `pools.c` 一处 zone 初始化 + 若干检查器条目，
> 下面标"缺口"的条目在 HEAD 上**仍然成立**。
> **证据基准**：`.audit/findings-index.tsv`（173 条）与 `docs/reviews/COMPILER-AUDIT-2026-09-30.md`（22 条 P0）。
> **状态取值**：`已定案` · `提案` · `缺口` · `与代码不符`（后两类都给出两边位置）。
> **一行 = 一条决策**；出处写文档 `文件:行`；实现写 `src/文件:行`（池/协程/事件层运行期是 `bufPuts` 里的**发射文本**，
> 行号给那一条 `bufPuts`）。凡引审计条目，写 `审计 <file>:<line>`（可回 `.audit/findings/*.json` 核对）。

---

## 1. 地方（place）/ zone / arena：所有权与释放顺序

- **R-01 三层命名与释放点** — arena（栈式、整块还）/ pool（等大元素、槽位复用、世代 handle、可显式释放）/ zone（一个地方里的一组 pool）三层各有准确语义，机制名为 pool 不叫 region。｜出处 `docs/topics/POOLS.md:13`｜状态：已定案｜实现：命名不是代码实体，落点在 `src/pools.c:37` 起的运行期文本头注释。
- **R-02 模型里没有"帧"** — 组织单位是"一个地方"（作用域），不是函数帧；arena 是**寿命**来源，不是唯一内存来源。｜出处 `docs/topics/POOLS.md:69`｜状态：已定案｜实现：`extc_pool_zoneEnter`/`extc_pool_zoneLeaveTo`（`src/pools.c:41`/`:48`）。
- **R-03 pool 持有自己的板块（plate）** — 数据 / free list / 嵌套池各占自己的块，池把它们连成旁链；arena 只拥有它们的**寿命**。｜出处 `docs/topics/POOLS.md:76`｜状态：已定案｜实现：`ExtcPool.blocks` + `extc_pool_take/_give/_resize`（`src/pools.c:39`,`:51`–`:53`）。
- **R-04 zone 是虚概念、独立旁链** — 不嵌在 arena 的栈里，退出时枚举 zone 自己的链（否则块回退会把它连根拔掉）。｜出处 `docs/topics/POOLS.md:84`｜状态：已定案｜实现：`ExtcZone.firstPool` + `zoneNext`（`src/pools.c:39`,`:46`）。
- **R-05 zone → pool 树 → pool 三层索引** — 一个地方可有多个 pool；zone 只是索引，链式还是数组随实现。｜出处 `docs/topics/POOLS.md:88`｜状态：已定案｜实现：`extc_pool_new_at`（`src/pools.c:45`）。
- **R-06 两条链各挂各的** — **寿命链**（`zoneNext` 进 zone 根链，地方退出整批回收）+ **逻辑链**（`nextSibling` 进父池子链，`drop(父)` 递归带走子树）；有父的池**两条都挂**（`62737da` 修的旧二选一缺陷）。｜出处 `docs/topics/POOLS.md:530`｜状态：已定案｜实现：`extc_pool_new_at` / `extc_pool_drop` / `extc_pool_unlinkZone`（`src/pools.c:45`–`:46`,`:42`）。
- **R-07 释放顺序 = 栈式 mark，不逐出口写** — 退出表达成"回到进入时的 mark"，`return`/`break`/`continue` 由结构覆盖。｜出处 `docs/topics/POOLS.md:510`｜状态：已定案｜实现：`extc_pool_zoneLeaveTo(mark)`（`src/pools.c:48`）+ codegen 每个释放点一行钩子。
- **R-08 zone 下标 = 栈深度、循环体每轮复用同一下标** — 这正是染色能起作用的前提。｜出处 `docs/topics/POOLS.md:392`｜状态：已定案｜实现：`extc_pool_zoneEnter` 里 `id = extc_zoneTop + 1`（`src/pools.c:41`）。
- **R-09 不许缓存 zone 指针，只能缓下标** — zone 数组是按需翻倍的堆数组（`realloc` 换基址），池记的是下标。｜出处 `docs/topics/POOLS.md:386`｜状态：已定案｜实现：`realloc`（`src/pools.c:41`）、`r->zone = zi`（`:45`）。
- **R-10 提权（needsHome）** — 分配点落在"调用者选的那个地方"；池与 arena 走同一条链（`extc_pool_new_at(parent, __extc_home_zone)`）。｜出处 `docs/topics/POOLS.md:750`｜状态：已定案｜实现：`codegen.c:1744`（签名补 `__extc_home_zone`）、`:1775`、`:3213`、`:2444`（池站点）。
- **R-11 arena 释放保留一块 spare** — `extc_arena_release` 整链还、**保留一块**（`cap ≤ EXTC_ARENA_SPARE_MAX`，1 MB）给下一次用；只有 `extc_arena_destroy` 才 free 掉 spare。｜出处 `docs/topics/POOLS.md:211`｜状态：已定案｜实现：codegen arena 运行期（审计 `src/coroutine.c:83` 引 `codegen.c:7247-7262`）。
- **R-12 长命层里的分配是永久的** — arena 只在分配点那一层的块退出时回收；长命容器必须显式 `release`/`clear`，方案保证"不漏"不保证"自动收缩"。｜出处 `docs/topics/POOLS.md:62`,`:287`｜状态：已定案｜实现：arena 语义本身（`src/pools.c:19` 注释 + codegen 块释放）。
- **R-13 "累积"是 arena 语义的直接推论** — 长命 place 里反复 `new` 只增不减（回收粒度是块）；HTTP keep-alive 实测每连接 9,110 B，改成循环内复用/全局瞬时缓冲后 1,623 B。｜出处 `docs/topics/CONCURRENCY.md:221`,`:223`｜状态：已定案（已实测）｜实现：无机制，靠用法纪律。
- **R-14 地方边界由"分配过 / 建过池"识别** — `stmtHasNew ‖ stmtMakesPool`；协程里 `placeBoundaryDepth` 恒 0（池落进任务 place，不再算边界）。｜出处 `docs/topics/CONCURRENCY.md:512`,`:695`｜状态：已定案（代码已放宽）｜实现：`check_stmt.c`/`check_top.c` 的 placeBoundary 判据。
- **R-15 自定义池容器的释放顺序是模型的结果** — `release` 必须**先收视图、容量归零，再 reset/drop**；反过来写就是悬垂窗口。｜出处 `docs/topics/POOLS.md:731`,`:785`｜状态：已定案｜实现：`stdlib/stl/*.extc` 五个容器的 `release`；模板 `tests/stl/custom_pool.extc`。
- **R-16 池树"不全"这件事如实记账** — 用户在容器外先建好再放进去的容器（`vector<vector<T>>` 内层）今天没有语言层上下文可传父 ⇒ 保持独立根池、寿命按自己的地方走。｜出处 `docs/topics/POOLS.md:556`｜状态：已定案（已知不完整）｜实现：`stl/pool.extc::withParent`（`stdlib/stl/pool.extc:147`）。
- **R-17 协程任务 place 是"懒"建的** — 只有协程需要自己那块存储（建池）时才有任务；任务表坐在池的 zone 原语上，所以生成任务表时要把池运行期也标成"需要"。｜出处 `docs/topics/CONCURRENCY.md:571`｜状态：已定案｜实现：`extc_task_begin`（`src/coroutine.c:44`）+ `genCoroDecls` 的 needPool 联动（`src/codegen.c:8255`）。
- **R-18 LIFO 是 zone 栈的隐含前提，但任务的存活顺序不是 LIFO** — 见 C-24/C-26；文档只写了"任务跑完 ⇒ zoneLeaveTo 一次性回收"，没有写这个前提。｜出处 `docs/topics/CONCURRENCY.md:551`,`:803`｜状态：与代码不符｜实现：`extc_task_end` 无条件 `extc_pool_zoneLeaveTo(extc_taskZone[id])`（`src/coroutine.c:84`）——审计 P0-20 实测 ASan UAF。

---

## 2. 池：模型、两副面、边界

- **Q-01 pool = 基础容器，STL = 纯逻辑** — 池只负责存储/句柄/查询/压缩/重建；容器把**句柄当数组下标**用（OI 的 `nxt[i]` 写法，区别只在这块数组是动态的）。｜出处 `docs/topics/POOLS.md:105`｜状态：已定案｜实现：`stdlib/stl/pool.extc`（`insert/get/set/remove/atDense/handleAtDense/gather/toSlice`）。
- **Q-02 池有两副面 + 一副记录** — 槽面发稳定身份（map/pool 自己），块面发可增长可归还的连续内存（vector/string 缓冲、节点视图指向的扁平存储）；两面共用同一条池记录、同一条寿命、同一次 `release`。｜出处 `docs/topics/POOLS.md:177`｜状态：已定案｜实现：`poolSlice<T>` / `poolResize<T>` / `poolGive<T>` 生成器内建（`docs/topics/POOLS.md:742` 表；`src/pools.c:51`–`:53`）。
- **Q-03 裁决一：vector/string 也走 pool，没有例外** — 判据不是"有没有句柄"，而是"扩容后旧块能不能立刻还回去"（实测 1e6 i64：arena 18,028 KB → 板块 9,708 KB）。｜出处 `docs/topics/POOLS.md:142`｜状态：已定案｜实现：`stdlib/stl/vector.extc`/`string.extc` 的缓冲区走 `poolSlice/poolResize`。
- **Q-04 裁决三：`varArray` 弃置、底座不替换** — 35 文件 / 66 处不迁移，新代码一律 `vector`。｜出处 `docs/topics/POOLS.md:153`｜状态：已定案｜实现：无迁移动作（`stdlib/prelude.extc` 里仍留 `varArray` 供 §13 用途）。
- **Q-05 块不进池** — 块是变长、无身份、只需 LIFO 的 push/pop ⇒ 走链（无句柄无世代）；给块发句柄是把 bump 的 O(1) 换成"摊销 O(1) 加元数据"，还会让池自己反复 resize。｜出处 `docs/topics/POOLS.md:182`｜状态：已定案｜实现：`ExtcBlock` 链表（`src/pools.c:39`），块分配器活在运行期而不是池里。
- **Q-06 两个提案被否掉** — 「arena 全部改成 pool」（实测 0.5ns vs 26ns、占用 2.9 倍，且语言层没有 free）与「块进池登记」。｜出处 `docs/topics/POOLS.md:204`｜状态：已定案（否）｜实现：无。
- **Q-07 池随容器创建，不能单独建空池** — "至少目前是这样，未来可以"。｜出处 `docs/topics/POOLS.md:219`｜状态：已定案（暂时）｜实现：`extc_pool_new` 只由容器构造器/`@poolObject` 调用（`stdlib/std/sys/pool.extc` 是库作者面）。
- **Q-08 元素等大 ⇒ 槽位复用成功率极高** — 基本不留碎片。｜出处 `docs/topics/POOLS.md:224`｜状态：已定案｜实现：`stl/pool.extc` 的槽/空洞链（`stdlib/stl/pool.extc:244` `remove`）。
- **Q-09 嵌套只是逻辑父子** — 实现上统一在一个 zone/arena 寿命下，清空时遍历树。｜出处 `docs/topics/POOLS.md:229`｜状态：已定案｜实现：`firstChild/nextSibling`（`src/pools.c:42`）。
- **Q-10 接口用词：`release`（不叫 close）· `clear` · `shrink`（不叫 compact）· 扩容像 vector（翻倍/1.5 倍，dense 保持连续）**。｜出处 `docs/topics/POOLS.md:27`｜状态：已定案｜实现：`stdlib/stl/pool.extc:293`（release）`:375`（clear）`:334`（shrink）`:456`（grow）。
- **Q-11 用户面 API 名在两处文档里不一致** — §4 写 `own/free/compact`，§12.1 与实现是 `insert/remove/shrink/release`；代码里**没有** `own/free/compact`。｜出处 `docs/topics/POOLS.md:264` 对 `docs/topics/POOLS.md:684`｜状态：与代码不符（§4 是旧稿残留）｜实现：`stdlib/stl/pool.extc` 的公开面。
- **Q-12 失效是值，不是崩溃** — 世代对不上 ⇒ `failure(staleHandle)` 带位置；越界、用已释放的池 ⇒ trap 带位置。｜出处 `docs/topics/POOLS.md:277`｜状态：已定案｜实现：`pStale`/`pPoison` 守卫（`stdlib/stl/pool.extc:280`–`:284`）+ `extc_pool_generation`（`src/pools.c:47`）。
- **Q-13 引用不许出池（出/入两半）** — 禁止把指向板块内部的视图交出去（`asSlice`/`denseView` 必须**拷到目的地所在的地方**）；禁止元素类型携带引用（`vector<ref T>` 一律不许，交叉引用用句柄）。｜出处 `docs/topics/POOLS.md:288`｜状态：已定案｜实现：库侧 `toSlice` 拷贝（`stdlib/stl/vector.extc`），检查器侧 `ttContainsRef` 判据。
- **Q-14 整个容器值可以借出去** — `ref vector<i32>` / `mut ref vector<i32>` 都允许（容器值自身是 arena 上的普通值）。｜出处 `docs/topics/POOLS.md:291`｜状态：已定案｜实现：容器方法的 `self: mut ref` 形状。
- **Q-15 `varArray<T>` 是唯一能装引用的容器** — 因为元素地址永不移动/永不复用；池底容器会压实/复用/翻纪元，所以池里的引用是雷。留它的理由不是性能（四个偏它的形状一个都没赢）。｜出处 `docs/topics/POOLS.md:811`｜状态：已定案｜实现：`tests/stl/vararray_ref.extc`。
- **Q-16 浅拷贝共享板块要警告** — `var b = a` 复制的是切片的指针（两个名字一块板块）；`clone()` 是显式 O(n) 独立副本；编译器在无标注绑定与"返回已有的值"两处**警告**（不是错误）。｜出处 `docs/topics/POOLS.md:467`｜状态：已定案｜实现：`Sym.line > c->curFunc->line` 判据 + `tests/stl/clone_warn.extc`。
- **Q-17 并发落地前的两条前置不变** — 数组作为**不可变包一次原子发布** · **先写载荷再发布状态**；注册表的进程级 `static` 到时候改 `_Thread_local`。｜出处 `docs/topics/POOLS.md:321`｜状态：与代码不符（三点）｜实现：`extc_pools/extc_poolKind/extc_zones/extc_dynSlots` 今天都是进程级 `static`（`src/pools.c:39`–`:40`,`:118`），没有任何 `_Thread_local`；且 321–324 行**文本已损坏**（重复一句 + 一句从"数据共享」"中间开始）——见 §14 含糊处。

---

## 3. 池：对象表 vs 容器池 · kind · 槽位复用 · resize/give 契约

- **K-01 kind 存在侧表、不进 `ExtcPool`** — 0 = 容器板块、1 = 对象表；理由是 `ExtcPool` 已 88 字节、每次访问都要碰（实测多一字段 = 每触一次多一条访存）。｜出处 `src/pools.c:40`（运行期注释；文档侧 `docs/topics/POOL-SOUNDNESS.md:16`）｜状态：已定案｜实现：`extc_poolKind` 字节数组 + `extc_poolKindCap`（`src/pools.c:40`）。
- **K-02 对象表 = 同一底座、更严契约（dyn 的池）** — 只追加：块永不替换/归还（否则元素句柄会静默指向别处）；删除 = 墓碑；`give`/`resize`/`resize_raw` 在对象表模式下**直接 trap**。｜出处 `docs/topics/POOL-SOUNDNESS.md:240`｜状态：已定案｜实现：`extc_pool_new_table` 置 kind=1（`src/pools.c:47`）、三处守卫（`src/pools.c:52`,`:53`,`:69`）。
- **K-03 池的"模式"决定 INV-A 是否成立** — 容器池允许换块（句柄按槽号而非块地址，块搬家不失效），对象表池焊死换块。｜出处 `docs/topics/POOL-SOUNDNESS.md:32`,`:240`｜状态：已定案｜实现：同 K-02。
- **K-04 缺口：槽位回收不清 `extc_poolKind`** — `extc_pool_freeSelf` 只重置 `live/parent/firstChild/freeNext`，kind 留在 1 ⇒ 容器池复用到该槽后继承"对象表"模式 ⇒ 合法程序运行期假 trap（abort）。｜出处 审计 `src/pools.c:46`（`.audit/findings-index.tsv`）｜状态：缺口｜实现：`extc_pool_freeSelf`（`src/pools.c:41`）缺 `extc_poolKind[rid] = 0`。
- **K-05 `resize` 契约：原地长大优先，新增段清零** — 用 `take+copy+give` 时新旧块同时活（实测峰值 1.9 倍活跃量）；`resize` 在原地长（`realloc`）并只清新增段。｜出处 `src/pools.c:53`（运行期注释）｜状态：已定案｜实现：`extc_pool_resize`（`src/pools.c:53`）。
- **K-06 `give` 契约：按地址在**本池**的链上找，找到才 free 并返回 1，否则返回 0** — 块是池自己的，不借不共享。｜出处 `src/pools.c:52`｜状态：已定案｜实现：`extc_pool_give`（`src/pools.c:52`）。
- **K-07 缺口：非正字节数静默钳成 1** — `take`/`take_raw`/`resize`/`resize_raw` 全都有 `if (bytes <= 0) bytes = 1;`；当 `n * sizeof(T)` 溢出成非正时，调用方拿到 1 字节板块而切片 `.len` 仍是 n ⇒ 堆越界写。｜出处 审计 `src/pools.c:51`｜状态：缺口｜实现：四处钳位（`src/pools.c:51`,`:52`,`:53`,`:69`）。
- **K-08 缺口：块粒度陈旧检查仍缺** — `give`/`resize` 都不动 `generation`，文档 T3 自认"部分修复、块粒度仍缺"；今天靠"对象表模式禁掉 give/resize"焊住 dyn 那条路，容器池按设计允许。｜出处 `docs/topics/POOL-SOUNDNESS.md:32`,`:80`｜状态：缺口（文档自认）｜实现：`src/pools.c:52`,`:53` 无 `generation++`。
- **K-09 `take_raw` 不清零，且只给"马上整片重写"的块** — 用户可见内存不走这条路（`new T[n]` 的"新分配读作 0"是语言承诺）；毒化金丝雀 `EXTC_POISON_RAW` 把 raw 块填 0xAA，任何"先读后写"当场现形。｜出处 `src/pools.c:52`｜状态：已定案｜实现：`extc_pool_take_raw`/`resize_raw` + `tests/pool/run.sh` 毒化判据。
- **K-10 池槽表扩容时必须把新槽挂上 free list** — 否则每建一个活池就把表翻一倍（20 个活池 = 33,554,432 槽 ≈ 3 GB，30 个 OOM）；SSO 之后每个 >16 字节的串一只池，必然撞上 ⇒ 已修（20 池 ⇒ cap=64）。｜出处 `docs/topics/POOLS.md:876`｜状态：已定案（已修）｜实现：`extc_pool_new_at` 扩容循环（`src/pools.c:46`）。
- **K-11 池级失效两条：`reset` 换代（粗但 sound）+ `drop` 置 `live=0`** — `generation++` 今天在 `new_at`（槽被新池占用）与 `reset` 两处。｜出处 `docs/topics/POOL-SOUNDNESS.md:15`,`:238`｜状态：已定案｜实现：`src/pools.c:45`（`new_at` 的 `r->generation++`）、`:47`（`reset`）。
- **K-12 `reset` 的语义是"保留池、槽可复用"，不还内存** — 嵌套树被 drop，但内存留在原地（地方退出才还）⇒ 便宜。｜出处 `src/pools.c:47`｜状态：已定案｜实现：`extc_pool_reset`（`src/pools.c:47`）。
- **K-13 对象表模式下的 `_raw` 同样拒绝搬家** — "清不清零"与"能不能搬家"正交，`resize_raw` 也带对象表守卫。｜出处 `src/pools.c:65`,`:69`｜状态：已定案｜实现：`extc_pool_resize_raw`（`src/pools.c:69`）。
- **K-14 线程下槽表/自由链是共享可变状态** — 规则 7：每线程一份池，或把"取槽 + 校验 gen/pgen"做成原子；跨线程只传值拷贝（句柄要么不跨线程，要么配原子校验）。｜出处 `docs/topics/CONCURRENCY.md:1090`｜状态：缺口（待实现）｜实现：判据 J3（`tools/check_concurrency_guards.py`）只把"元数据只经 `pools.c`"锁住，`_Atomic` 未做。

---

## 4. 池：染色 / 失效粒度 / 重建 / 共享拷贝

- **E-01 两层失效，各管一段** — 池级 `pool.epoch` 对 `zones[pool.zone].color`（整个 place 死了，零走查零重写）；槽级 `handle.gen` 对 `slot.gen`（epoch 内部复用，不能拿掉）。只记"每个元素一个 color"或只记 gen 都不够。｜出处 `docs/topics/POOLS.md:369`｜状态：已定案｜实现：`ExtcZone.color` + `ExtcPool.color`（`src/pools.c:39`,`:41`）。
- **E-02 色从哪来** — 池创建时抄下出生色；校验是"我记的色对当前 zone 的色"，池随时能回到自己那一格 zone（`r->zone`）。｜出处 `docs/topics/POOLS.md:382`｜状态：已定案｜实现：`zoneEnter` 翻色（`src/pools.c:41`）、`new_at` 抄色（`:45`）、`extc_pool_rowColor`/`birthColor`（`:52`）。
- **E-03 零成本刷新 = 全是 freeslot** — 僵尸池不需要逐槽处理：`n=0`、`free=0`、空洞链头 `-1`，再抄当前色，cap 个槽一个字节都不写。｜出处 `docs/topics/POOLS.md:396`｜状态：已定案｜实现：`stl/pool.extc::clear`（`stdlib/stl/pool.extc:375`，`epoch+1 / n=0 / cursor=0 / holeHead=-1` 四行）。
- **E-04 色宽 3 位** — 位宽只是**检测强度**（命中 1/8），不参与正确性；取 3 位是因为全新页的垃圾值可能偶然命中色（1 位命中 1/2）。｜出处 `docs/topics/POOLS.md:406`,`:518`｜状态：已定案｜实现：`EXTC_ZONE_COLOR_BITS 3`（`src/pools.c:41`），`color==0` 跳回 1。
- **E-05 色只否决，不确认** — "不等 ⇒ 一定死"可靠；"相等 ⇒ 活"不可靠（垃圾命中、位宽绕回）。活性权威永远是 `n`、空洞链、`gen`。｜出处 `docs/topics/POOLS.md:500`,`:577`｜状态：已定案｜实现：`src/pools.c:52` 注释与 `extc_pool_rowColor` 的用法纪律。
- **E-06 染色已 sound 落地，但库侧零调用点；性能账按实测重排** — 分片不划算（10ns/池里只占 2~3ns）、真收缩今天已成立；色是"整个地方死了"的显式语义，不是性能杠杆。｜出处 `docs/topics/POOLS.md:412`｜状态：与代码不符（§7.3/§7.4 承诺"翻色 + 注册表游标归零 = O(1)"，实测段自己承认**两件今天都还没落**：`zoneEnter` 除翻色外仍重置 `firstPool`、`zoneLeaveTo` 仍是逐个 drop 走链；本文 §7.4.1 的实测结论与 §7.4 的承诺在同一份文档里并存）。｜实现：`src/pools.c:41`（翻色已做）、`:48`（逐个 drop，未做分片/游标归零）。
- **E-07 真正的杠杆是"哪些东西可以不清零"** — 池簿记表（`vals/slots/ent`）走 `poolSliceRaw` ⇒ "每轮建 cap=10 万 的池"2000 轮 1.09s → 0.012s（约 90 倍）。｜出处 `docs/topics/POOLS.md:431`｜状态：已定案（已落地）｜实现：`poolSliceRaw`/`poolResizeRaw`（`src/pools.c:52`,`:69`）。
- **E-08 保留池已回滚** — 收益（62 KB 块 3.5 倍）配不上机制量（分档/预算/单块上限/退出结账/"块头来源标记每条路径写对"新坑）；`mmap` 拿零页也否掉（比 memset 慢 4~46 倍）。机制不在树里。｜出处 `docs/topics/POOLS.md:437`｜状态：已定案（回滚）｜实现：`grep keep_bytes src/ stdlib/` 无命中（已核对）。
- **E-09 世代/句柄空间的防御阶梯** — 毒化槽（O(1)）→ 换茬戳（纪元+1，O(1)）→ 响亮拒绝 → `rebuild`/`compact`（最后一招，O(n)）；重建会让已发出的视图失效。｜出处 `docs/topics/POOLS.md:303`｜状态：已定案｜实现：`stl/pool.extc` 的 `shrink`（`:334`）与拒绝路径。
- **E-10 重建时机** — 单线程随时；多线程按 redis 架构：发现该重建就挡住后续操作，等在飞的做完（期间撞上 ABA 就地毒化），再重建；代价 STW、频率极低。｜出处 `docs/topics/POOLS.md:315`｜状态：提案（并发落地前的前置未实现）｜实现：无。
- **E-11 频率口径与代码不符** — 文档算的是"计数器放 48 位 ⇒ 约 32 天一次"；代码是 `HANDLE_EPOCH_BITS = 32`，且 `gen` 也只用 32 位（`pPack = gen << 32 | low`）⇒ 按文档自己的速率口径约 43 秒一次。｜出处 `docs/topics/POOLS.md:318` 对 `stdlib/stl/pool.extc:55`,`:120`｜状态：与代码不符｜实现：`HANDLE_EPOCH_BITS`（`stdlib/stl/pool.extc:55`）、`pPack`（`:120`）。
- **E-12 重建 = 干净启动 = ABA 的解法** — 重建把整池回到纪元 0/世代 0，空间从零滚动 ⇒ 不存在"被旧数据淹没"；不需要纪元参与校验（手柄可以更小）。前提：唯一长期持有下标/手柄的地方就是池自己。｜出处 `docs/topics/POOLS.md:310`｜状态：已定案｜实现：`shrink` 重建活槽与空洞链（`stdlib/stl/pool.extc:334`）。
- **E-13 dyn 三条不变量已落地** — 对象表池（`extc_pool_new_table`）+ 派发键存在**槽**里 + 派发前校验 `gen`/`pgen`；卸载/重置后调用 = trap 带位置，跨实现槽复用校验先失败。｜出处 `docs/topics/POOL-SOUNDNESS.md:195`｜状态：已定案｜实现：`extc_dyn_put`/`extc_dyn_slot`/`extc_dynSweep`（`src/pools.c:118`）。
- **E-14 缺口：`extc_dynPoolOf` 只按 zone 下标记 pid** — 判据是 `kind != 1 || generation == 0`，不校验该 pid 是否仍属于**这个** place ⇒ 一个 place 会把 dyn 值生进"别的 place 还活着的对象表"里（地方寿命失效）。｜出处 审计 `src/pools.c:118`｜状态：缺口｜实现：`extc_dyn_put` 的 `pid = extc_dynPoolOf[zi]` 分支（`src/pools.c:118`）。
- **E-15 三个动词的落点** — `clear` O(1)（不碰槽数组）· `shrink` O(cap)（降高水位、重建活槽与空洞链）· `release` O(1)（真还池记录）。｜出处 `docs/topics/POOLS.md:492`｜状态：已定案｜实现：`stdlib/stl/pool.extc:375`,`:334`,`:293`。
- **E-16 位宽不够就 rebuild 是错的顺序** — 必须走 E-09 前三级；位宽只影响检测强度。｜出处 `docs/topics/POOLS.md:589`｜状态：已定案｜实现：策略层，无代码强制。

---

## 5. Heap（边界层 / 板）

- **H-01 Heap 是三层分工里的第三层** — 给外来代码（dlopen 模块 / C ABI 另一侧）：一块**连续板**、槽里全是指针、寿命 open…close（Arena 兜底）、进门只查区间（起点+长度）、**没有世代**。少检验是故意的。｜出处 `docs/topics/HEAP.md:23`｜状态：提案（设计稿；板本体已落地，dlopen 接口表部分落地）｜实现：`stdlib/std/heap.extc` + `src/plate.c`。
- **H-02 核心不变量：只传值，指针只指向板内** — 宿主的地址永远不出界；模块能碰的内存全是它自己申请、由宿主发出去的。｜出处 `docs/topics/HEAP.md:34`｜状态：已定案（实现路线）｜实现：`plate::holds`/`holdsRange`（`stdlib/std/heap.extc`）+ `extc_viewOf`（`src/plate.c:8`）。
- **H-03 借出即入板；可写借出是默认，只读借出是可选硬保证** — C 的常态是 in/out 参数；危险是"留住"而不是"写"，由"借出即入板 + close 即 munmap"兜住；只读用 `mprotect(PROT_READ)`，由内核保证（写=缺页）。｜出处 `docs/topics/HEAP.md:41`,`:50`｜状态：已定案（只读借出已落地）｜实现：`loanRO`/`loanRW`（`stdlib/std/heap.extc`；`tests/heap/loan.extc`、`loanwrite.extc` rc=139）。
- **H-04 连续板是结构不是实现细节** — 校验一次比较 · `close` 即 `munmap`（旧指针硬缺页）· 自主分配+整体回收 · 内部用偏移外部给指针；增长靠预留（mmap 保留 + 按需 commit），不搬移；相对地址不行（模块是宿主编译的 C，`p->x` 就是绝对地址）。｜出处 `docs/topics/HEAP.md:72`｜状态：已定案｜实现：`tests/heap/`（reserve=4096MB、rss_delta=4、CHUNK=1MiB）。
- **H-05 "假嵌入"：模块只用宿主一张表** — 表头字段是 `size`+`ctx`（老模块配新宿主 ⇒ 干净拒绝）；不靠符号 interposition；模块不自己跑，每次进入都是宿主发起的 ⇒ 参数是宿主选的、没有环境权限。｜出处 `docs/topics/HEAP.md:91`｜状态：已定案（表已能编能跑）｜实现：`tests/cabi/heapmain.extc` + `tests/cabi/platetable.{extc,c}`（`init=16 holds=true outside=rejected`）。
- **H-06 板的寿命：显式 close + Arena 兜底** — `close` 就像 pool 的显式 `reset`；忘了关由 open 它的那只 Arena 兜底。这条必须定死，否则"少检验"退化成"不知道什么时候没的"。板内槽不需要世代（`close` 把 use-after-free 变成一次缺页）。｜出处 `docs/topics/HEAP.md:144`｜状态：**缺口**：```close``` 已落地、Arena 兜底**没做**（今天忘了关只是漏到进程结束；`owned` 那条线也没做）。｜实现：`tests/heap/afterclose.extc`（rc=139）；兜底无。
- **H-07 不对称规则（这套设计最要紧的一半）** — 宿主内存寿命 ≥ 模块代码寿命 ⇒ **模块可以持有宿主（板内）指针；宿主不可以持有模块指针**（数据或函数都不行）。｜出处 `docs/topics/HEAP.md:174`｜状态：已定案｜实现：结构约定 + `tests/hostile/` 判据。
- **H-08 注册一律给世代句柄** — 模块在宿主表里注册的东西不给裸指针，给 `{slot, gen}`；`dlclose` 时把属于该模块的槽世代加一，旧句柄当场 trap；卸载四步协议（停发新活 → `deinit` → 作废世代 → `dlclose`）+ "在模块里跑着的调用计数"。｜出处 `docs/topics/HEAP.md:179`｜状态：提案（未落地）｜实现：无（`@export`/fn 已有，句柄注册表未做）。
- **H-09 跨界视图必须指向 Heap 内部** — 给视图加"在 Heap 里"的标记位（与 `Thread=` 一个路数），边界处查这个位。｜出处 `docs/topics/HEAP.md:46`｜状态：提案（今天用 `holds` 区间检查代替，标记位未做）｜实现：`plate::holds`（一次比较）。
- **H-10 一只板属于一个模块，板间不共享** — 共享等于两个外来方互相别名，插件之间也会打架。｜出处 `docs/topics/HEAP.md:310`｜状态：已定案（不做）｜实现：无。
- **H-11 队列不是原语；跨线程才需要；现在不做** — 单线程会合 `k = yield e` 已够；跨线程队列 = 一池消息槽 + 一个单调序号 + 每槽就绪标志（release 写/acquire 读），没有新结构；载荷不进区域 arena（长命区域里反复分配只增不减），大块走池句柄。｜出处 `docs/topics/HEAP.md:262`｜状态：提案 + **缺口**（"现在不做"；前置工作是池的线程安全——空闲链与世代号目前是单线程写法）｜实现：无。
- **H-12 消息线与 Heap 共用同一层共享分配器** — 原子 bump（或每线程先领整块）、按区域释放、"堆视图"标记位、只读共享的可见性由邮箱 release/acquire 给。｜出处 `docs/topics/HEAP.md:249`｜状态：已定案（部分落地：区域 arena 已落地；"原子 bump 不需要"是实测结论——每 worker 一只自己的 arena，bump 路径无原子；邮箱未做）｜实现：`extc_par_region`（`src/codegen.c:8331`）。
- **H-13 区域共享 arena 已落地** — `extc_par_run` `calloc` 区域；每 worker 一只 `extc_arena`；worker 里的 `new` 落进 `region->arenas[id]`（`extc_tls_arena` 指过去）；**worker 退出时不销毁**，父线程 join 后直接读不拷贝；`extc_par_close` 把每只 worker arena 的块链还回去再 `free`。｜出处 `docs/topics/HEAP.md:198`｜状态：已定案（已落地）｜实现：`src/codegen.c:8328`–`:8348`、`:8360`+（`extc_par_open/close`）。
- **H-14 只在需要时开** — 调用点按 worker 是否会 `new`（`usesHome`）传 `withRegion`；不开时退回线程本地 arena 并由 worker 自己销毁（既有行为一字不改）。｜出处 `docs/topics/HEAP.md:207`｜状态：已定案｜实现：`g->needParRegion`（`src/codegen.c:2315`）、`#define EXTC_PAR_REGION 1`（`:8309`）。
- **H-15 不维护两份运行期文本** — 一份文本 + 一个 `#ifdef EXTC_PAR_REGION`；不用并行的程序里那半套不参与编译（不是死代码、不进二进制）；逐字节不变不是目标，可维护/性能/安全不损失才是。｜出处 `docs/topics/HEAP.md:228`｜状态：已定案｜实现：`src/codegen.c:8309`–`:8310`。
- **H-16 威胁模型：防错不防敌** — 十种攻击实测：板外指针被 `holds` 拒、越界写踩不到板外、close 后写硬缺页；但"库少符号走 `!` 直接调"（rc=139）、食言留指针、munmap 同址重映射、改宿主表槽都打得成。能力模型，不是沙箱。｜出处 `docs/topics/HEAP.md:137` + `docs/topics/C-ABI.md:677`｜状态：已定案｜实现：`tests/hostile/run.sh`。
- **H-17 待作者拍的两问** — `deinit` 强制还是可选（建议强制，理由是线程静默）；Heap 与 arena 的转换在哪一侧做（宿主 `copyOut` 直接落到调用者的 arena 层号？建议是）。｜出处 `docs/topics/HEAP.md:314`｜状态：提案（未拍）｜实现：无。
- **H-18 HEAP.md §8.4 已过时** — 它写"§2 那张表整张都是函数指针，而 `docs/SPEC.md` §7.3 写着没有函数指针……这是唯一一处不改语言就写不出这页文档"；`fn(A)->R` 已在 `C-ABI.md:160` 落地（含表槽 effects），指针过界也已放开。｜出处 `docs/topics/HEAP.md:321` 对 `docs/topics/C-ABI.md:160`｜状态：与代码不符（文档低估）｜实现：`TY_FN`（`src/ast.h`）、`src/codegen.c` 的 typedef 发射（`C-ABI.md:163`）。

---

## 6. 协程：帧（frame）与句柄（handle）的表示

- **C-01 前提是**无栈**（这一节不成立则整条线作废）** — 状态 = 编译器生成的 frame 结构体 + PC 整数（"控制流状态 = 数据"字面成立）；有栈 = 一整条机器栈，等于偷偷引入第二种内存模型。｜出处 `docs/topics/CONCURRENCY.md:35`｜状态：已定案｜实现：`genCoroFunc` 的 `switch(f->pc)` 状态机（`docs/topics/CONCURRENCY.md:514`）。
- **C-02 帧是一个普通值** — 合成结构体，字段 = `pc` + `ret` +（按需的 `zone`）+ 跨挂起点存活的局部；**参数也进帧**（恢复时调用者没有实参可传）。｜出处 `docs/topics/CONCURRENCY.md:451`,`:514`｜状态：已定案｜实现：`FuncDef.coroFrame` + `coroFrameLay`（`src/check_top.c`），codegen 帧字段重定向在 `EX_IDENT` 唯一一处。
- **C-03 帧的规则：跨挂起点的**值**按值进帧；被取视图且视图跨挂起点 ⇒ 编译期拒绝（给两条改法）；不跨挂起点的局部不进帧（零代价）。** — 作者口径：用户没写 `new` 就不进 arena，与其他程序同一套标准，无隐式分配。｜出处 `docs/topics/CONCURRENCY.md:451`｜状态：已定案｜实现：规则 ② 的活跃性分析 + 拒收判据 `tests/errors/coro_view_lives_across_yield.extc`。
- **C-04 引用三档** — 共享（借用 + 提权，提的是用户自己 `new` 的站点）/ 独立副本（`.clone()`，落当前地方 ⇒ 协程里就是任务 arena）/ 说不清 ⇒ 编译期拒绝并给两条改法；**不做隐式深拷贝**。｜出处 `docs/topics/CONCURRENCY.md:460`｜状态：已定案｜实现：`check_escape.c:2114`/`:2155` 的措辞族 + `tests/coro/coro_pool.extc`。
- **C-05 帧里的局部再被取视图** — 帧是可复制的值 ⇒ 指进帧内部的视图在帧被搬走后悬 ⇒ 与"指向局部"同类处理。｜出处 `docs/topics/CONCURRENCY.md:458`｜状态：已定案｜实现：同一套逃逸深度规则。
- **C-06 统一句柄：表面只有一个 `coroutine<T>`** — 表示由逃逸决定：不外逃 ⇒ 帧留在调用者栈上（零分配）；外逃（容器/字段/返回值/传给可能存指针的被调者）⇒ 装箱到任务 place。用户看不见 `worker$frame` 这种东西。｜出处 `docs/topics/CONCURRENCY.md:573`｜状态：已定案（已落地，判据 13/13）｜实现：`coroBoxed`（`src/codegen.c:6777`）、`needCoroHandle`（`:6779`）。
- **C-07 句柄的 C 表示是 24 字节 `{frame, kind, task}`** — `task` 是安全凭据；`kind` 每协程一个编译期编号，`next`/`value` 按 kind 分派（静态结构 ⇒ 跳转表，不用函数指针）；与池句柄 `{pid,slot,gen}` 是同一个套路。｜出处 `docs/topics/CONCURRENCY.md:597`,`:649`｜状态：已定案｜实现：`struct ExtcCoroS`（`src/codegen.c:7075`）、`coroKind`（`:6778`）。
- **C-08 四条句柄安全规则** — ① 不许伪造（结构字面量解析不了；帧值当句柄是干净类型错误）② 过期要 trap（`extc_task_alive` 验活 ⇒ `exit(70)` 带源码位置）③ 没有零值（不带初始化的声明拒绝）④ 一个逻辑驱动者（复制内存安全，单驱动者是**约定**）。｜出处 `docs/topics/CONCURRENCY.md:634`｜状态：已定案（④ 是约定）｜实现：`extc_task_alive`（`src/coroutine.c:88`）、`tests/coro/coro_copy*.extc`。
- **C-09 装箱点只有：带标注的声明 / 实参 / 字段 / 元素** — 帧变量 → 句柄变量是干净的编译错误（那等于复制一个已经跑过的状态机）；`return` 一个句柄按设计不存在（`-> coroutine<T>` 是"这个函数体是协程"）。｜出处 `docs/topics/CONCURRENCY.md:671`｜状态：已定案｜实现：`tests/coro/coro_boxing.extc`、`tests/errors/coro_forge_frame_var.extc`。
- **C-10 `coroutine<T>` 与 `coroutine<A,B>` 都是协程** — 一参数是"请求与应答同型"的简写；`send` 收 A、`value` 给 B。｜出处 `docs/topics/CONCURRENCY.md:713`（说 `coroutine<A,B>` 没做）对 `src/check_top.c:3905`,`:4147`｜状态：**与代码不符**（文档说欠账，代码已支持）｜实现：`check_top.c:3905`–`:3919`、`:4145`–`:4150`；判据 `tests/coro/coro_shorthand.extc`（`coroutine<i64>` 与 `coroutine<i64,i64>` 同结果，退出码 17）。
- **C-11 帧里 arena 按块层级分** — 帧持 `extc_arena arena1..arenaN`，`arenaRefAt` 在 step 里返回 `f->arenaK` ⇒ 存储活过挂起、块退仍 `extc_arena_release(&f->arenaK)`；跑完（`pc=-1` 早退与落到体尾两条路）统一 `extc_arena_destroy` 各层。不是"整个帧一个 arena"（长任务会一路涨）。｜出处 `docs/topics/CONCURRENCY.md:686`｜状态：已定案（已落地）｜实现：`tests/coro/coro_new.extc`、`coro_alloc_loop.extc`（20000 轮 1.28 GB 分配量、常驻 ≤32 MB）。
- **C-12 规则 ② 在协程里的一种放开** — 初始化式**直接就是 `new`**（存储在本协程帧的 arena 里）的引用可以跨挂起；参数、指向调用者的 `ref`、外面递进来的视图仍拒。｜出处 `docs/topics/CONCURRENCY.md:692`｜状态：已定案｜实现：同活跃性分析。
- **C-13 缺口：泛型协程装箱后从不 `extc_task_end`** — `coroNeedsZone`/`$next` 对实例跳过 ⇒ 任务 arena 与 zone **每次创建泄漏**（实测 30 万次 57 MB，2000 次 1.8 MB）。｜出处 审计 `src/codegen.c:4521`（P1-13）｜状态：缺口｜实现：泛型实例的协程发射路径。
- **C-14 缺口：协程声明为方法时帧结构从未定义** — checker 建的结构体叫 `count$frame`，codegen 用 `cFuncName` 得 `tick_count$frame` ⇒ 生成物编不过（`extc` 仍退出 0）。｜出处 审计 `src/codegen.c:4680`,`:7708`（P1-8）｜状态：缺口｜实现：帧结构命名两处实现分叉。
- **C-15 缺口：没有 `yield` 的协程帧没有成员** — `coroFrameLay` 提前返回 ⇒ 帧结构没有 `pc`/`arenaN`/参数成员，生成物 `'struct c$frame' has no member named 'pc'`。｜出处 审计 `src/check_top.c:6750`（P1-22）｜状态：缺口｜实现：`coroFrameLay`。
- **C-16 缺口：句柄辅助函数把 `cType(yieldType)` 拼进标识符** — `ref` 产出类型给 `extc_coro_value_int64_t *`，调用点被解析成乘法。｜出处 审计 `src/codegen.c:4538`（P1-10）｜状态：缺口｜实现：handle helper 命名。
- **C-17 缺口：泛型协程实例帧漏 `var x = yield e` 绑定** — `coroBinds` 按模板函数指针匹配，实例漏掉 ⇒ 生成物编不过。｜出处 审计 `src/check_top.c:6850`（P1）｜状态：缺口｜实现：`coroBinds`。
- **C-18 缺口：递归协程的 rec guard 只进不出** — 协程分支提前 return 从不配对 `cgRecLeave` ⇒ 顺序创建 10 万个协程后被假的 `recursion too deep` 杀掉（真实深度 ≤2）。｜出处 审计 `src/codegen.c:4901`（P1-12）｜状态：缺口｜实现：`genCoroFunc` 的 `extc_rec_enter` 路径。
- **C-19 缺口：`coroutine<T>` 只在数据位出现时 typedef 不发射** — 结构体字段 / 数组元素 / 泛型实参里出现 `coroutine<T>` ⇒ 发出 `extc_coro` 却从未发 typedef，生成物编不过。｜出处 审计 `src/codegen.c:6779`（P1）｜状态：缺口｜实现：句柄 typedef 的按需判据。
- **C-20 缺口：`coroutine<T>` 的零值发射成 `(extc_coro){ .unit = false }`** — 真实句柄类型没有 `unit` 字段。｜出处 审计 `src/codegen.c:1574`（P1）｜状态：缺口｜实现：`zeroValue` 对泛型实例的处理。
- **C-21 缺口：聚合里的 `coroutine<T>`/`dyn` 句柄不算"没有零值"** — 零句柄驱动 NULL 帧（SEGV）。｜出处 审计 `src/check_lookup.c:671`（P1）｜状态：缺口｜实现：`typeLacksZeroValue` 的聚合遍历。
- **C-22 帧可自由移动/复制** — 调度器挪帧、池化帧都要这个性质；帧里只放字节缓冲+偏移的设计下"引用跨暂停"根本不会出现。｜出处 `docs/topics/CONCURRENCY.md:278`｜状态：已定案（设计意图）｜实现：统一句柄落地后帧是任务 arena 上的独立值。

---

## 7. 协程：任务 place 的建立、生命周期与清理

- **C-23 任务 = 地方根（作者拍板 B）** — `spawn`（= 调用）建立**任务 place**（懒：第一次分配才建），帧里存**它自己的 zone id**；恢复点**只读字段、不重推导**。｜出处 `docs/topics/CONCURRENCY.md:443`｜状态：已定案｜实现：`extc_task_begin`/`extc_task_zone`（`src/coroutine.c:44`,`:75`）+ 帧的 `zone` 字段。
- **C-24 任务由调度器/任务表拥有** — 跑完或显式 drop 是**同一次释放**（`extc_task_end`）；调度器能枚举还活着的任务（`extc_task_live`）。不采用"帧的作用域 = 任务寿命"（那会让帧不可复制，毁掉"帧是普通值"）。｜出处 `docs/topics/CONCURRENCY.md:445`,`:803`｜状态：已定案｜实现：`extc_task_end`/`extc_task_live`（`src/coroutine.c:80`,`:92`）。
- **C-25 任务自己的 arena** — 箱装协程的帧住这里，协程自己 `new` 的东西也住这里（切片 C 的提权）；与块 arena / 池 plate 同一个形状（一整批释放）；任务结束时 `extc_arena_release` + `zoneLeaveTo` 一起放。｜出处 `docs/topics/CONCURRENCY.md:555`｜状态：已定案｜实现：`extc_task_alloc`（`src/coroutine.c:67`）、`extc_task_end`（`:80`）。
- **C-26 恢复点只读帧字段（规则 1 收紧）** — `$next` 先 `extc_zoneTop = f->zone`，跑完恢复；`$step` 体内不许出现 `extc_zoneTop`/`__extc_home_zone`。｜出处 `docs/topics/CONCURRENCY.md:546`,`:561`｜状态：已定案（判据已就位）｜实现：`tools/check_concurrency_guards.py` 的 J1 + 反证验证。
- **C-27 缺口（P0）：任务按 LIFO 释放 zone，但任务不按 LIFO 死亡** — `extc_task_end` 无条件 `extc_pool_zoneLeaveTo(extc_taskZone[id])` ⇒ 弹掉栈上更高、**仍然存活**的任务 zone，那些任务的池块被释放 ⇒ ASan UAF。修法建议：按任务自己的 mark 精确释放，或每任务独立 arena 结束时 `extc_arena_destroy`，不动全局 zone 栈。｜出处 审计 P0-20 `src/coroutine.c:84`｜状态：缺口｜实现：`extc_task_end`（`src/coroutine.c:84`）+ 装箱 spawn 不还原 `zoneTop`（`src/codegen.c:3805`–`:3826`）+ `$next` 先恢复再调 end（`:4612`–`:4613`）。
- **C-28 缺口（镜像）：非装箱（栈帧驱动）路径的 place 永不回收** — spawn 在任务结束前就把 `extc_zoneTop` 还原 ⇒ `extc_pool_zoneLeaveTo(f->zone)` 是 no-op；实测 5 万次 spawn 263 MB（P1-14），审计 P0-20 另测 20000 个结束任务 RSS 85.9 MB（对照 1.4 MB）。｜出处 审计 `src/codegen.c:3841` / P0-20｜状态：缺口（已知泄漏）｜实现：非装箱 spawn 的 zone 保存/还原（`src/codegen.c:3832`–`:3864`）。
- **C-29 缺口：装箱 spawn 不保存/还原调用方的 `extc_zoneTop`** — 与 C-27 是同一行的两个方向（这里是把**调用方**的地方弹掉）：spawn 之后运行期 `zoneTop` 停在任务 zone，之后创建的 dyn 值落进任务 place，任务一结束就 stale trap。｜出处 审计 `src/coroutine.c:84`（P1）｜状态：缺口｜实现：`src/codegen.c:3805`–`:3820`。
- **C-30 缺口：任务 arena 的 spare 块永久泄漏** — `extc_task_end` 用 `extc_arena_release`（保留一块 spare），而任务 id 单调递增、槽永不复用 ⇒ 那个 spare 既不可达又永不归还（30 万任务 +32MB；`-DEXTC_ARENA_SPARE_MAX=0` 回落到 12.5 MB）。帧自己的块 arena 走 `extc_arena_destroy`，任务 arena 是唯一用 release 的地方。｜出处 审计 `src/coroutine.c:83`（P2）｜状态：缺口｜实现：`extc_task_end`（`src/coroutine.c:83`）。
- **C-31 缺口：`extc_task_zone` 不查 liveness** — 只查上界 ⇒ 对从未用过的 id 返回未初始化内存（ASan 下 0xBEBE…），对死任务返回旧 zone；同表另外三个入口（`alloc`/`end`/`alive`）都先查 `extc_taskLive`。｜出处 审计 `src/coroutine.c:76`（P3）｜状态：缺口｜实现：`extc_task_zone`（`src/coroutine.c:75`）。
- **C-32 已丢弃、从未跑完的协程会漏掉帧 arena 里剩下的块（有界，记在案）**。｜出处 `docs/topics/CONCURRENCY.md:693`｜状态：缺口（已知有界）｜实现：`coro_new` 的清理路径。
- **C-33 任务预算/上限没做** — 防止长任务把内存吃光的"有界 + 大声 trap"那一半。｜出处 `docs/topics/CONCURRENCY.md:715`｜状态：缺口｜实现：无。
- **C-34 统一句柄的四条规则在测试里钉住；`value()` 的 helper 曾漏验活（已修）** — 读已回收的帧曾是 use-after-free；现在驱动与 `value()` 都必须大声 trap（退出码 70 + 源码位置）。｜出处 `docs/topics/CONCURRENCY.md:681`｜状态：已定案（已修）｜实现：`extc_task_alive` 在两条 helper 上；`tests/coro/coro_value_expired.extc`。

---

## 8. 协程：驱动协议 / `ext` / 调度域

- **C-35 语法定稿** — `fn f(n: i64) -> coroutine<i64>` 体内可 `yield`；`let c = f(args)` **就是 spawn**（帧生在这个调用点）；驱动走已落地的迭代器协议：`for x in c` 或 `while c.next() { c.value() }`。｜出处 `docs/topics/CONCURRENCY.md:427`｜状态：已定案｜实现：切片 B2a/B2b/B2d（`docs/topics/CONCURRENCY.md:515`–`:526`）。
- **C-36 不做 `spawn`/`resume` 关键字** — 组合用 `for x in inner { yield x }`（= `yield from`）。｜出处 `docs/topics/CONCURRENCY.md:441`｜状态：已定案｜实现：无这两个关键字。
- **C-37 `yield` 是语句（v1 不是表达式）；`send` 收值** — 帧多一个 `in` 槽；两种写法（`var got: T = yield e` / `got = yield e`）都收；帧侧 `send(self: mut ref F, v: T) -> bool`、句柄侧 `send` 走 helper（先验活再写 in 再 next）。｜出处 `docs/topics/CONCURRENCY.md:587`｜状态：已定案（已落地，判据 `coro_send` 17）｜实现：`src/check_top.c:4001`（send 的 A）。
- **C-38 目标形式 vs A1 的保守形式** — 目标：`yield` 不许落在"活着时会回收、挂起点之后还要用"的块里，该回收的东西要么不跨挂起点、要么先提到任务自己的地方；A1 保守形式按"体顶层/嵌套块"分别拒或放行。｜出处 `docs/topics/CONCURRENCY.md:478`｜状态：已定案（C 切片后按目标形式放宽）｜实现：`placeBoundaryDepth`（协程里恒为 0，`ST_YIELD` 检查保留为不变式）。
- **C-39 `ext` 关键字 + 词法调度域（定案）** — 语法糖关键字就叫 `ext`（与 Go 的 `go` 同构）；域由**库 + 词法块**定义，`ext` 属于**最近**的域；块结束 ⇒ 子任务必须都结束（结构化）；不用 Go 式隐式全局域（编译期报错而不是运行期炸）；域可显式往下传。｜出处 `docs/topics/CONCURRENCY.md:611`｜状态：已定案（**与代码不符**：文档"欠账 6"说"`ext` + 调度域没实现，一行代码都没有"，实际解析器/检查器/运行期/判据都在）｜实现：`parseUnary` 的 `EX_EXT`（`src/parser.c:2732`）、`ST_DOMAIN`、`src/domain.c:19`–`:60`、`tests/ext/domain_*.extc`。
- **C-40 域是一个普通结构：任务 = (frame, step)** — `extc_dom_new/add/run/free`；`extc_dom_run` 把每个任务 step 到完成 ⇒ 块的结构化由它保证。｜出处 `src/domain.c:1`（代码头注释，对应 `docs/topics/CONCURRENCY.md:614`）｜状态：已定案（已落地）｜实现：`src/domain.c:19`（`domainEmitRuntime`），按需发射 `needDomain`（`src/codegen.c:2150`,`:2273`,`:3780`）。
- **C-41 撤回 `with place { }`** — 作者口径"太丑，写着麻烦"；普通函数里块本来就是那个机制，协程里缺的只是"块的 arena 归帧所有" ⇒ 正解是帧携带块级 arena，不需要新语法。｜出处 `docs/topics/CONCURRENCY.md:625`｜状态：已定案（撤回）｜实现：无 `with place` 语法；帧 arena 见 C-11。
- **C-42 缺口（P0）：`d.run { }` 不计入块层级** — 生成物 `__extc_a[]` 少一格 ⇒ 栈越界（ASan 确认）；`blkMaxLevel` 必须认 `ST_DOMAIN`。｜出处 审计 P0-11 `src/codegen.c:3642`｜状态：缺口｜实现：`blkMaxLevel`/`genBlockBody` 两处实现（§0.2-3 的"同一张表两份实现"样本）。
- **C-43 缺口：`countOwSites` 不下探 `ST_DOMAIN`** — codegen 的 `collectOwSites` 下探 ⇒ `d.run` 里的 `@overwrite` 让 `needOw` 为假、`extc_owcell` 类型从不生成，但 cell 照发 ⇒ 生成物编不过。｜出处 审计 `src/check_top.c:857`（P1-18）｜状态：缺口｜实现：`countOwSites`。
- **C-44 缺口：`ext` 目标未启动时 `$step` 体丢失** — 保留 `$next` 包装却没有 `$step` 体 ⇒ `-Wundefined-internal`，-O0 下硬链接错误。｜出处 审计 `src/codegen.c:4608`（P1）｜状态：缺口｜实现：协程发射的可达性剪枝。
- **C-45 缺口：echo/AF_UNIX/TCP_NODELAY 的三条实测教训** — AF_UNIX 不设 Nagle（设了本来失败）；echo server 已能跑（`tests/coro/echo_server.extc`，判据 `coro_echo.extc` 退出码 50）；`pump` 的 level-triggered 陷阱（一轮只 accept 一个 ⇒ 0 超时排空会死转，修法"遇 listener 停止排空 + 上限 64"）。｜出处 `docs/topics/CONCURRENCY.md:728`,`:782`,`:789`｜状态：已定案（已修/已落地）｜实现：`stdlib/std/coro/scheduler.extc`。
- **C-46 缺口：事件循环"等"的用法纪律** — pump 只处理一个事件 ⇒ 单线程里"等"只能靠 `pump`，数据要用非阻塞 fd 收；否则永远停住。｜出处 `docs/topics/CONCURRENCY.md:710`｜状态：已定案（写进手册）｜实现：`tests/coro/coro_accept.extc`。
- **C-47 缺口：请求只能表达"等可读"** — v1 的 yield 槽：正数 = 等该 fd 可读，负数 = 立刻再推，**没有"等可写"**；正式出路是请求类型 / `coroutine<A,B>`。｜出处 `docs/topics/CONCURRENCY.md:721`｜状态：缺口｜实现：调度器请求编码。
- **C-48 缺口：trampoline 未做** — `-fno-inline` 下每层 ~1.3 ns 的真实成本已量，机制未做。｜出处 `docs/topics/CONCURRENCY.md:714`｜状态：缺口｜实现：无。
- **C-49 缺口：未使用的局部量留下未用变量** — 生成物报 `-Werror=unused-variable`，破坏"生成物零告警"合同（踩到两次）；要么检查器报"未使用的局部量"，要么 codegen 声明后补 `(void)x;`。｜出处 `docs/topics/CONCURRENCY.md:725`｜状态：缺口｜实现：codegen 局部声明。
- **C-50 数值边界缺口（生成到用户程序里的 UB）** — `parallel::run` threads=0 除零 SIGFPE（`codegen.c:8402`，P1-23）；区域大小按未截断线程数算可回绕成堆溢出（`codegen.c:8342`，P2）。｜出处 审计 `src/codegen.c:8402`,`:8342`｜状态：缺口｜实现：并行运行期发射文本。

---

## 9. 线程模型：共享状态 / `effects Thread=` / worker 白名单 / TLS arena

- **T-01 不加并发运行时，只加编译期变换 + 四个 syscall 包装** — 层 1 编译器（状态机+类型）、层 2 `std::sys::*`（epoll/thread 原语）、层 3 stdlib（调度器/parallel）、层 4 用户；**没有第 5 层**（没有绿色线程、executor 抽象、隐藏的运行时线程）。｜出处 `docs/topics/CONCURRENCY.md:20`｜状态：已定案｜实现：调度器是纯 extC 库（`stdlib/std/coro/scheduler.extc`）。
- **T-02 明确不做清单** — 有栈协程 / `async`/`await` 函数着色 / 运行时拥有的调度器 / 共享可变状态+隐式同步 / 绿色线程抢占 / 协程池与 executor 抽象 / 第一版跨线程协程迁移 / 结构化并发的语法。｜出处 `docs/topics/CONCURRENCY.md:975`｜状态：已定案（不做）｜实现：无。
- **T-03 "worker 不碰共享数据"收紧成"只碰**不可变**共享数据"** — **不可变可共享（`ref slice<A>`）· 可变必转移（移动）**；无数据竞争是结构保证，不是纪律。｜出处 `docs/topics/CONCURRENCY.md:904`｜状态：已定案｜实现：`check_expr.c` 的 worker 检查（`parBodyProblem`）。
- **T-04 共享点诚实数是三处** — channel · arena 池 · 每线程的分配器（跨线程给块需要显式内存序或一把锁）。｜出处 `docs/topics/CONCURRENCY.md:922`｜状态：已定案（记账）｜实现：`extc_tls_arena`（`src/codegen.c:8326`）。
- **T-05 arena 池必须**有界** + 背压** — 生产者比消费者快 ⇒ 块在 channel 里排队 ⇒ 池里的 arena 全被占住 ⇒ 内存涨；不能只靠"低频加锁"。｜出处 `docs/topics/CONCURRENCY.md:914`｜状态：已定案（通道未做）｜实现：无。
- **T-06 并行 = 分 → 并行处理 → 合并** — 唯一限制不是"不能原地修改"，而是"原地修改必须发生在同一个 worker 自己的 arena 里"；跨 worker 合并走分-合。｜出处 `docs/topics/CONCURRENCY.md:925`｜状态：已定案｜实现：`extc_par_run` + 分段借用判据。
- **T-07 规则 1：任务/线程是"地方根"** — place 在创建时钉死、随帧存下来；恢复点禁止重推导。｜出处 `docs/topics/CONCURRENCY.md:1072`｜状态：已定案（判据 J1 已就位）｜实现：`extc_task_zone` + 帧 `zone` 字段（`src/coroutine.c:75`）、J1（`tools/check_concurrency_guards.py`；`FALLBACK_BUDGET = 2` 计数棘轮只许少不许增）。
- **T-08 规则 2：跨任务/跨挂起点只传值或池句柄** — 引用必须提到 home/任务层，指不动的东西（C 栈局部）直接拒绝。｜出处 `docs/topics/CONCURRENCY.md:1073`｜状态：已定案（洞已修：族 E `dyn` 载荷带引用曾实测 `stack-use-after-scope`，现在载荷算本值的一部分，E1 已毕业进 `tests/errors/`）。
- **T-09 规则 3：禁止在地方边界内挂起**（或把边界提升到任务级） — 协程里 `placeBoundaryDepth` 恒 0（切片 C 之后池落进任务 place）；`ST_YIELD` 检查保留为"挂起点不得落在一块活不过它的存储里"这个不变式本身。｜出处 `docs/topics/CONCURRENCY.md:1074`,`:695`｜状态：已定案（已放宽）｜实现：`check_stmt.c` + `tests/coro/coro_pool.extc`（原反例 `yield_inside_place_boundary.extc` 已删除）。
- **T-10 规则 4：线程下 arena 元数据无竞争** — 每线程 arena + effects 要能表达"别的线程可能观察到"；判据：生成物断言池槽访问只经 `extc_pool_*` 一处（便于将来加 `_Atomic`）。｜出处 `docs/topics/CONCURRENCY.md:1075`｜状态：缺口（待实现；J3 前身已就位）｜实现：`tools/check_concurrency_guards.py` J3。
- **T-11 规则 5：池的"地方"同样按规则 1 钉死** — `makesPool`/`zoneLevel`/`extc_pool_new_at` 决定池生在哪个 place；协程下就是任务，池与 plate 随任务亡。判据：每个 `extc_pool_new` 都被改写成 `_at`，且实参来自帧字段。｜出处 `docs/topics/CONCURRENCY.md:1088`｜状态：缺口（待实现判据）｜实现：`extc_pool_new_at`（`src/pools.c:45`）。
- **T-12 规则 6：不需要拍"句柄 vs 池撑到最后"** — 既有机制已经在做后者（静态版）：`dyn` 是天然跨任务通道（池自带 plate），"句柄要活多久，池就提到哪一层 place"由逃逸分析 + zone 提权完成。｜出处 `docs/topics/CONCURRENCY.md:1089`｜状态：已定案（已由既有机制实现；判据 `dyn_promoted_return`/`dyn_promoted_two_hops`）。
- **T-13 规则 7：线程下槽表/自由链是共享可变状态** — 每线程一份池，或把"取槽 + 校验 gen/pgen"做成原子；**跨线程只传值拷贝**。｜出处 `docs/topics/CONCURRENCY.md:1090`｜状态：缺口（判据 J3 已就位，原子未做）｜实现：`src/pools.c`。
- **T-14 `effects Thread=` 的语义** — 单事实、不是逐参掩码：`0` = 这次调用**不碰跨线程共享的状态**（模块级 `var`、运行期缓冲、输出流），`1` = 碰；只许 0/1。｜出处 `src/parser.c:373`–`:382` + `src/ast.h:824`｜状态：已定案｜实现：`parseEffectsClause`（`src/parser.c:342`）、`check_expr.c:108`。
- **T-15 `Thread=` 的缺省与代码不符** — 头文件注释与设计写"缺省保守 = 1（假定碰共享状态）"，但 `extThreadMask = 1` 只在**写了 `effects` 子句**时设置；完全没写 effects 的 `extern!` 保持 0 ⇒ worker 白名单（`check_expr.c:108` 只在 `!= 0` 时拒）放行它 ⇒ 并行 worker 里可静默调用碰共享状态的 C 函数（审计 TSan 实测 data race）。｜出处 `docs/topics/CONCURRENCY.md:1075` 对 `src/parser.c:342`,`:345`、`src/ast.h:824`、`src/check_expr.c:108`｜状态：与代码不符 + 缺口｜实现：`parseEffectsClause` 与 worker 检查。
- **T-16 `Thread=` 写在**字段**上暂时报错** — "还没有用"：读它的那条检查（worker 里允许调谁）是冲着已解析的调用去的，运行期填的表槽不是；记下来却没人读，比拒绝更坏。｜出处 `docs/topics/C-ABI.md:400`｜状态：已定案（暂时拒绝）｜实现：`parser.c:947`–`:956`（`sawThread` → error）。
- **T-17 worker 白名单规则** — 只碰自己的参数与局部（模块级 `var` 自然落在这一档，不用另做全局分析）；调用只能是签过 `Thread=0` 的 extern，或另一个同样通过这一关的顶层函数；递归深度上限 16，超限按不安全处理。｜出处 `src/check_expr.c:127`–`:151`｜状态：已定案｜实现：`parBodyProblem`（`src/check_expr.c:132`）。
- **T-18 缺口：白名单不识别 `EX_GENCALL`** — 池原语（`poolSlice`/`poolResize`/`poolGive`）在 worker 里不经任何检查。｜出处 审计 `src/check_expr.c:100`（P2）｜状态：缺口｜实现：`parBodyExpr`（`src/check_expr.c:100`）。
- **T-19 缺口：`poolResize`/`poolGive` 不要求视图是 `mut`** — 只读 `slice<T>` 被洗成可写 `mut slice<T>`。｜出处 审计 `src/check_expr.c:586`（P2）｜状态：缺口｜实现：这两个内建的检查。
- **T-20 缺口（P0）：TLS arena 的适用条件被建模错了** — `parBodyProblem` 把 `parTlsArena` 打在**整条 worker 调用链**的函数上（函数级标志），codegen 让这些函数体里所有分配走 `(*extc_tls_arena)`；而该指针只在 worker 线程里被赋值，主线程恒为 NULL ⇒ 主线程调用同一 helper 必崩（ASan SEGV in `extc_arena_alloc`）。修法方向：TLS arena 只能用于"确定在 worker 里执行"的函数体（调用图分析区分入口），或生成 `extc_tls_arena ? *extc_tls_arena : 当前 arena` 的兜底。｜出处 审计 P0-21 `src/check_expr.c:149`｜状态：缺口｜实现：`parBodyProblem` 的函数级标记 + `arenaRefAt` 的 TLS 分支。
- **T-21 TLS arena 的形状** — `_Thread_local extc_arena *extc_tls_arena = NULL`（worker 内 `new` 走它；NULL 时检查器要能响亮说话）；只有 `needPar` 才发射。｜出处 `src/codegen.c:8324`–`:8326`｜状态：已定案｜实现：`g.needPar`（`src/codegen.c:8310`）。
- **T-22 区域共享 arena 的形状** — `_Thread_local extc_par_region *extc_tls_region`；`extc_par_open` 一次 `calloc`（区域 + workers 只 arena）；每 worker 一只自己的 arena，bump 路径无原子（竞争在"谁拥有哪只"这层消掉）；父线程 join 后直接读，不拷贝；区域的寿命 = 本线程下一次并行运行（那一刻关闭）。｜出处 `docs/topics/HEAP.md:198`,`:225`｜状态：已定案｜实现：`src/codegen.c:8328`–`:8360`。
- **T-23 并行调度模型的证据** — 矩阵乘 n=768 f32：1 线程 308.5ms（旧 312.2）、8 线程 45.7ms（旧 45.2），加速比 6.76x（旧 6.90x），交错 A/B 判定无回归；"先量到 5.91x 是噪声"的教训记在案。｜出处 `docs/topics/HEAP.md:212`｜状态：已定案（实测）｜实现：`bench/` 与 `parrun.py`。
- **T-24 两条路别互相替代** — 规则形状的结果继续用分段借用（`parallel::run`，8 线程 6.81x，不需要消息）；不规则/流式走消息（按 worker_id 规范化合并、浮点固定合并树）。验收一：不用 send/recv 的程序产物逐字节相同且仍 6.81x；验收二：用了 send 的程序与串行结果逐元素相同且与线程数无关。｜出处 `docs/topics/HEAP.md:292`｜状态：已定案（消息线未落地；`coroutine<A,B>` 前置已具备）｜实现：`extc_par_run`。
- **T-25 单元测试工具** — `tools/parrun.py`（并行验收脚本）与 `tools/check_concurrency_guards.py`（J1/J2/J3）是这条线的常设闸门；`check.sh` 里跑。｜出处 `docs/topics/CONCURRENCY.md:1102`｜状态：已定案｜实现：两个脚本 + `tests/ext/`。

---

## 10. C-ABI：fn / extern! / @export / @frozen / 板边界 / 指针抹除

- **A-01 三条线（这一页的全部结论）** — ① 跨界只有**裸函数指针**（`fn(A)->R` 是代码指针、**没有环境**，要状态就显式传 `ctx`）② 方向定死：**模块 → 宿主免费**（宿主代码与内存比模块活得久），**宿主 → 模块只经世代句柄**（宿主永不持有模块的裸代码指针）③ 闭集分发仍用枚举 + `match`，`fn` 只为"被调方编译期未知"存在。｜出处 `docs/topics/C-ABI.md:19`｜状态：已定案｜实现：`TY_FN` + `ttCrossesC`。
- **A-02 `fn` 的 ABI 就是平台 C 的函数指针 ABI** — 参数/返回值/结构体按值全按平台 C ABI（含 `f32/f64`、小整数提升、结构体返回的几种传递方式）；产物本来就 `-std=c11 -fwrapv`；代价是跨界的结构体布局必须与 C 一致（下一格，`@frozen`）。｜出处 `docs/topics/C-ABI.md:47`｜状态：已定案｜实现：`cType(TY_FN)` 発 typedef（`docs/topics/C-ABI.md:163`）。
- **A-03 `fn` 是普通值类型** — 可出现参数/返回值/结构体字段/泛型实参/数组元素/`slice<T>` 的元素；**没有空值**（要可空用 `option<fn…>`）；**不带环境**（带捕获的闭包不能变成 `fn`；捕获为零的 lambda 可以，零成本）；不承诺可调用性的副作用信息（外来目标按最保守记）。｜出处 `docs/topics/C-ABI.md:33`｜状态：已定案｜实现：`tests/ops/fnptr.extc`（值/参数/返回值/字段/嵌套签名/extern 符号当值）。
- **A-04 四个当场修的坑（合法但错）** — 共享走查器故意不看 call 的 callee（`callViaFn` 才当子节点）；`o.f(x)` 在 parser 眼里是方法调用（用 `peekFieldType` 偷看字段类型后原地改写）；零值 `typeLacksZeroValue(TY_FN)=true` 且 codegen 零值发成不存在的标识符（让 gcc 点名）；"函数地址能否当 `fn` 值"在检查期答不了（隐藏参数是可传递闭包），由 codegen 在标志终局处拒绝。｜出处 `docs/topics/C-ABI.md:171`｜状态：已定案（已修）｜实现：`ast.c`/`check_top.c`/`codegen.c` 对应分派。
- **A-05 指针过界放宽为"一个机器字"** — 标量 · 指向**任何东西**的指针（`ref T`/`mut ref T`/`?ref T`，含 `ref void`）· 函数指针；**不是**一个机器字的一律不过：`slice<T>`（C 那边两个参数）、结构体/带载荷枚举**按值**（布局没冻结）、`void` 按值。｜出处 `docs/topics/C-ABI.md:214`｜状态：已定案｜实现：`ttCrossesC`（`src/check_top.c`）。
- **A-06 `ref void` 看不进去** — `*p` 编译期报错（"`void` has no size"），`p[i]` 由既有索引检查挡住；能做的只有传回给 C、放进 `?ref void` 判空、当参数转手。｜出处 `docs/topics/C-ABI.md:223`｜状态：已定案｜实现：`EX_DEREF` 检查。
- **A-07 借出可写内存的义务变大（四条论证）** — 指针没有布局问题；extC 能对新类型做的操作一件都没新增；寿命与权限一个字没改（`checkCallRefArgs` 与指向类型无关）；真正变大的是**义务**：C 可从我们内存里读出一个指针并留着（`Cont` 管），也可往我们内存里写一个它自己的指针。｜出处 `docs/topics/C-ABI.md:230`｜状态：已定案｜实现：`checkCallRefArgs`（`src/check_top.c`）。
- **A-08 写回方向没有掩码** — 挡"C 往我们的 `?ref` 字段写它自己的指针"只有三条**声明处**选择：(a) 只读借出（写 `ref T` 不写 `mut ref T`）(b) 大块输入用只读映射（`mprotect`，实测 ≈0.7 MiB 盈亏平衡）(c) 签字里写明不写回指针；要不要加第四个掩码（比如 `Wr`）等真有库需要再拍，不先发明。｜出处 `docs/topics/C-ABI.md:250`｜状态：提案（未拍）｜实现：无 `Wr`。
- **A-09 `@frozen` = 布局的签字** — 这个类型的字节布局 = C 为这份字段表排出来的布局（声明顺序、C 的对齐与填充、里面没有别的东西）；作者那一半往里放什么随他（`slice<T>`/枚举/泛型参数都行，代价是 C 照抄发射的形状）；切片不因为 `@frozen` 就变成一个参数。｜出处 `docs/topics/C-ABI.md:268`｜状态：已定案｜实现：`StructDef.frozen` + `src/codegen.c:5282`–`:5307`。
- **A-10 `@frozen` 编译器那一半是硬的** — 永不重排字段、永不加隐藏字段；产物里每个 `@frozen` 类型两条**平台无关**的 `_Static_assert`（顺序 + 尺寸，数字全由 C 自己算 `offsetof/sizeof/_Alignof`，不写死）。门票的粒度：`ttCrossesC` 里一行，按值过界只认 `sdef->frozen`。｜出处 `docs/topics/C-ABI.md:284`｜状态：已定案｜实现：`src/codegen.c:5295`–`:5307`；判据 `tests/frozen/` 四正四反 + 两条有牙（调换字段必须响）。
- **A-11 `@export`：把符号交出去** — `extern!` 是"我调 C"，`@export` 是"C 调我"；产物里是**外部链接**的 C 符号，名字就是源码里写的那个。四条硬规矩：签名每个参数/返回值是"C 能写的一个机器字或 `@frozen`"（检查器，同一个 `ttCrossesC`）；不能有隐藏参数（codegen，可传递闭包）；名字不能需要 C 改名、不能与另一个导出重名（codegen）；泛型/方法/协程/`extern!` 不能导出（语法层）。导出不能被可达性剪枝剪掉。｜出处 `docs/topics/C-ABI.md:350`｜状态：已定案（已完成）｜实现：`check_top.c:5503`–`:5526`、`codegen.c:590`/`:4687`/`:6729`–`:6743`；判据 `tests/cabi/`（nm -D 查到 `T triple/T mid/T makePair`）。
- **A-12 表槽的 `effects` 签字** — 签名**在槽上、不在类型上**；从槽里直接调带着签名，拷贝到局部就丢了（按最保守算，反例 `errors/slot_copied_out.extc`）；调用点把掩码交给 `checkCallRefArgs` 的 **extern 那条路**（合成一份声明），所以"签字的槽"与"签字的 extern!"不可能漂。顺带补掉一个真洞：从前经 `fn` 值调用走非 extern 保守路径，界是"被调方拿到的那只 arena 活多久"，而经指针调用的被调方根本没拿到 arena ⇒ 帧内指针能被交出去。｜出处 `docs/topics/C-ABI.md:374`｜状态：已定案（已完成）｜实现：`parseEffectsClause` 在字段上（`src/parser.c:928`）+ 调用点合成声明。
- **A-13 `effects Ret=0`** — "我返回的引用/视图不指向调用方的任何帧"（`Addr` 的返回侧）；记到 `Sym.outOfFrame`，视图绑定的深度取"它指向的存储"；只准 `Ret`/`Thread` 写在有体函数上（`Addr`/`Cont` 由身体算，写了报错）；**赋值即收回**签字信任。｜出处 `docs/topics/C-ABI.md:610`｜状态：已定案（已落地）｜实现：`parser.c:360`–`:372`、`Sym.outOfFrame`、判据 `tests/cabi/outframe.extc`。
- **A-14 指针抹除只开一个方向** — `u64(p)`/`i64(p)`：指针 → 整数（区间检查要它）；**整数 → 引用故意不开**（那是"把数字当指针用"，另一个决定）⇒ 指针能被验证、不能被伪造；`u32(p)` 这种窄目标仍被拒（截断）。｜出处 `docs/topics/C-ABI.md:425` + `docs/topics/HEAP.md:130`｜状态：已定案｜实现：转换检查 + `tests/cabi/` 判据。
- **A-15 视图造不出来的那条缝：`extc_viewOf(p,n)`** — 泛型实例没有结构体字面量，`ref void → ref T` 又故意不开 ⇒ 由**编译器**给三行 C（`@builtin`，`src/plate.c:8`）；它是"指针 + 长度 ⇒ 视图"的**唯一入口，不做任何检查**：谁调用谁负责（板用 `holds`，C 边界用声明处的签字）。`extc_memCopy` 同理（`memcpy` 的签名里有 `const void*`/`size_t`，手写声明会与 `<string.h>` 撞），**按声明触发**（一条声明一个标志）——加进 `extern!("extc-mem")` 那块会让 21 个产物逐字节变化，golden 当场抓住。｜出处 `docs/topics/C-ABI.md:461`｜状态：已定案｜实现：`src/plate.c:8`（`plateEmitViewOf`）、`:24`（`plateEmitMemCopy`）、`src/codegen.c:6820`/`:6843`/`:8437`。
- **A-16 缺口：`extc_memCopy` 用 `memcpy` 做板的整块拷贝** — `copyIn/copyOut` 允许源与目标都是板内视图 ⇒ 重叠时 -O2 静默拷错（该用 `memmove`）。｜出处 审计 `src/plate.c:39`（P2）｜状态：缺口｜实现：`plateEmitMemCopy`（`src/plate.c:24`）。
- **A-17 谁负责释放（extC 侧没有 free）** — C 给 extC 的内存按**定案 79**：显式释放，编译器只证明能证明的泄漏（`close` 协议那条判据）；`extern!` 的 `owned` 声明**未实现**，写了直接报错（"Memory returned by C has to be released, and extC has no free… 计划是 resource value owned by the frame，还没做"）。｜出处 `docs/topics/LIBS.md:169`｜状态：**缺口**：`owned` 仍是"planned"；今天只许声明"往调用者已拥有的内存里写"的 C 函数。｜实现：`parseExtern` 的 `owned` 分支（`src/parser.c:440`–`:447`）。
- **A-18 谁负责释放（板的模型）** — 板是宿主的，模块经表申请**板内**内存；宿主 `close` 一次 `munmap` 整块还；模块不需要配合关板（关不关是宿主的事）；板内槽不需要世代（use-after-free 变硬缺页）。｜出处 `docs/topics/HEAP.md:149`｜状态：已定案（已落地）｜实现：`stdlib/std/heap.extc`、`tests/heap/`。
- **A-19 `extern!` 未签字 ⇒ 最保守** — 声明漏了 `effects` ⇒ 退回"可能留住一切"（安全但几乎不可用：连普通局部都传不进去）；签了但说错 ⇒ 那是**你签的字**（与 `p!` 同一族，P′ 允许）。｜出处 `docs/topics/LIBS.md:157`｜状态：已定案｜实现：`checkCallRefArgs` + `ttCrossesC`。
- **A-20 生成物里没有 `restrict`** — `grep restrict src/*.c` 只命中 `base.c` 的 C 关键字表（给标识符重命名用）；若有，C 存下指针再写就会与 extC 的读构成 UB —— 指针放宽之后这个前提值得记住。｜出处 `docs/topics/C-ABI.md:258`｜状态：已定案（已核对）｜实现：无 `restrict` 发射。
- **A-21 缺口：`ttCrossesC` 对含类型参数的签名跳过、实例化后不复检** — 审计报告 §10.4 自认的已知缺口（`types.c:417`）；`fn` 泛型体里的 `fn(T)->T` 同理（没有程序能构造出越界实例，唯一来源是模板自己）。｜出处 `docs/topics/C-ABI.md:204` + 审计报告 §10.4｜状态：缺口｜实现：`ttCrossesC`。
- **A-22 缺口：`Addr=1` 的槽包一层之后要求退化** — 直接经槽调用会正确处理，包一层之后那层要求退化成"活得比我的某个 ref 实参久"；总结的词汇表里没有"存到 C 自己的内存里（永久）"这一档（`addrMask`/`contMask`/`homeAddrMask` 说的都是"存进调用方给的某处"）。要补就得给总结加一档（例如 `foreignMask`），是独立的一步。｜出处 `docs/topics/C-ABI.md:604`｜状态：缺口｜实现：`collectEffectsExpr`（`src/check_top.c`）。
- **A-23 `extern!("libcairo")` 会链接、不会 dlopen** — 声明写下去链接器就找那些符号；dl 那条路上函数只是**地址**，所以 cairo 零 `extern!`、全是 `dlsym`。地址必须有签名，而签名在槽上 ⇒ 表要传下去、不能把函数指针拷出来。｜出处 `docs/topics/C-ABI.md:508`｜状态：已定案（实测）｜实现：`tests/real-lib/`。
- **A-24 门函数不能带隐藏 arena 参数** — 两个被逼出来的库 API：`plate::allocPtr(n)`（门要裸指针，而"先 alloc 拿视图再取 .data"会被逃逸分析挡住）与 `plate::holdsRange(p,n)`（"先 extc_viewOf 再 holdsView"会让门函数带上隐藏 arena 参数，地址就不是 `fn` 值）。｜出处 `docs/topics/C-ABI.md:554`｜状态：已定案（已落地）｜实现：`stdlib/std/heap.extc`。
- **A-25 回调三个方向分开看** — 宿主 → 模块安全（指针来自已加载模块）；模块 → 宿主**免费安全**（宿主代码/内存比模块活得久）；模块注册回调给宿主需要机制（禁裸指针，只给 `{slot,gen}` 世代句柄）。回调不需要是闭包，要状态就显式 `ctx`。｜出处 `docs/topics/C-ABI.md:69`｜状态：已定案｜实现：`tests/cabi/`（表 + 回调 + 区间检查）。
- **A-26 实施顺序四步** — ①类型+解析+检查+下降（不动 prelude，判据 golden 414 逐字节不变）②跨界的结构体布局（`@frozen`）③`std::dl`（open/close/sym）④Heap 板进运行期。四步都已完成或部分完成。｜出处 `docs/topics/C-ABI.md:117`｜状态：已定案（①–④ 均已落地，见 §9.6–§9.12）｜实现：`tests/cabi/`、`tests/heap/`、`stdlib/std/dl.extc`。
- **A-27 待作者拍的六问** — `fn` 是类型关键字还是别写法（建议沿用 lambda 拼法）；零捕获 lambda 自动转 `fn`（建议允许）；`fn` 做泛型参数（建议允许）；`fn` 跨模块自由传递（建议能）；`extern!` 降级为"只给自链接库"（建议是）；可空写法（建议 `option<fn…>`）。｜出处 `docs/topics/C-ABI.md:106`｜状态：提案（未拍；其中多数已按建议落地）｜实现：`parser.c` 类型位置接受 `fn`。
- **A-28 分析精度两条"复用已有框架"的修复（定案 97）** — `match` payload 拷贝要做收窄（复用 `declare` 的契约：谁引入新绑定点，谁就问"我知道初始化器吗"）；目的地深度的代理只用**能装引用的东西**（`typeContainsRef` 过滤，`ref u8` 不是目的地）。｜出处 `docs/topics/C-ABI.md:707`｜状态：已定案（已修）｜实现：`declare` 的收窄契约 + `typeContainsRef`；判据 `tests/cabi/checked_path.extc`。
- **A-29 缺口：经 `fn` 值调用的效果摘要在总结里是 `effUnknown`** — 经**签字的槽**调用已并进所在函数的总结（§9.14 修），但槽没签或值只是局部拷贝 ⇒ 仍 `effUnknown`（总结不完整、保守）。｜出处 `docs/topics/C-ABI.md:591`｜状态：已定案（保守面保留）｜实现：`collectEffectsExpr`。

---

## 11. 模块：可见性 / mangle / 加载顺序 / 跨模块泛型

- **M-01 模块 = 一个文件；一个 `use` 就够；没有 .h、没有重复声明、没有构建系统** — 语义导入（不是文本包含）；默认公开 + `@private`（要藏才写，不雷霆展开）；禁环；`-I <dir>` 搜索。｜出处 `docs/topics/MODULES.md:96`,`:198`｜状态：已定案（方案 A v1 已落地，定案 70）｜实现：`src/modules.c` 的 `loadModules`（`:1917`）。
- **M-02 单 TU + static 是刻意的** — 所有非 `main` 函数加 `static`（PLAN #37 换向量化 3.2×）；`export`/`@export` 才丢 `static`。库只需要"名字 + 可见性 + 摘要"，真·多 TU 会破坏那个优化。｜出处 `docs/topics/MODULES.md:83` + `docs/topics/LIBS.md:94`｜状态：已定案｜实现：`codegen.c:4690`/`:5125` 的 linkage 选择。
- **M-03 装载五步** — ①找文件（`use a::b` ⇒ `<项目根>/a/b.extc`，再 `-I`，再 `$EXTC_STD`；找不到印出找过的地方）②递归 + 查环（"正在装载"又被打到 ⇒ `import cycle`；同一文件只装一次）③拓扑序合并（**后序入表**，依赖先合进主 Module，与 prelude 同一条路）④**检查之前**解析限定名（`io::readLine` ⇒ `EX_CALL(readLine)`；`io::STDIN` ⇒ `EX_IDENT`；`io::File` ⇒ `File`）⑤可见性（`@private` 在改写这一步挡；不带限定地引用别模块的名字在检查器解析处挡）。｜出处 `docs/topics/MODULES.md:473`｜状态：已定案（已落地）｜实现：`loadUnit`/`mergeUnit`/`rwExpr`/`rwExprName`/`unitExports`（`src/modules.c`）。
- **M-04 加载顺序 = 后序入表 = 拓扑序；root 文件最后合并且走**同一个** `mergeUnit`** — 依赖先合入；root 的名字从不 mangle（`modName == NULL` 让 `mangleUnitDecls` 立即返回）。｜出处 `src/modules.c:1742`（`loadUnit` 注释）、`:1989`–`:1996`、`:2067`–`:2085`｜状态：已定案｜实现：`loadModules` 的 `L.order` 合并循环与 root merge。
- **M-05 环 = 编译错误** — 两个模块互相依赖时各自的类型都需要对方才能检查；消息建议"把共享部分放进第三个模块"。｜出处 `docs/topics/MODULES.md:491`｜状态：已定案｜实现：`loadUnit` 的状态机（`src/modules.c:1774`+）。
- **M-06 文件解析顺序与 in-code 注释不符** — `findModFile` 实际顺序：`rootDir`（入口文件所在目录）→ `searchDirs`（`-I`）→ `stdDir`（`$EXTC_STD` 或 `<extc>/../stdlib`），三者都不看"导入者文件所在目录"；而 `modules.c` 头部注释说 `use` 解析 "next to the importing file"。｜出处 `docs/topics/MODULES.md:477` 对 `src/modules.c:12`、实现 `src/modules.c:396`–`:414`｜状态：与代码不符（**src 注释**过时；MODULES.md 的写法与代码一致）｜实现：`findModFile`（`src/modules.c:396`）。
- **M-07 可见性有三种做法，extC 选"默认公开 + `@private` 逐条收紧"** — 与 `mut`/`ref` 一样"写了才给"的反面：不写就是公开（作者口径"不雷霆展开"）。｜出处 `docs/topics/MODULES.md:165`（Q3）｜状态：已定案｜实现：`parser.c` 的 `isPrivate`（`:488`–`:511`,`:625`,`:706`,`:733`,`:775`,`:805`,`:893`,`:928`,`:1231`）。
- **M-08 `@private` 的四个检查点** — 限定类型名（`modules.c:744`/`:819`/`:1089`）、限定函数/全局（`:1020`）、字段（`check_expr.c:1640`）、方法（`check_expr.c:3385`）；裸名引用别模块的名字由 `openLookup` 挡（`modules.c:1545`）。｜出处 `docs/topics/MODULES.md:481`｜状态：已定案（部分）｜实现：上述位置。
- **M-09 缺口：`@private` 对**关联函数**无效** — `mod::Type::secret()` 任何模块可调（字段/方法/运算符都挡了，关联函数没挡）。｜出处 审计 `src/check_expr.c:1975`（P2 UNSOUND）｜状态：缺口｜实现：`EX_ASSOC` 的解析路径。
- **M-10 缺口：`@private` 类型不设防（alias 表泄漏）** — alias 表收录了私有声明的返回票据 ⇒ 裸名（含传递 import）就能使用别人的私有 struct/enum。｜出处 审计 `src/modules.c:2005`（P1）｜状态：缺口｜实现：`loadModules` 的 alias 收集（`src/modules.c:2000`–`:2019`）。
- **M-11 缺口：`openLookup` 被"同名 @private"压掉公开符号** — 遇到"某个被打开模块里是 `@private` 的同名声明"就报错返回，另一个模块公开导出的同名符号被吞（误拒合法程序）。｜出处 审计 `src/modules.c:1557`（P2 FALSE-REJECT）｜状态：缺口｜实现：`openLookup`。
- **M-12 mangle：每条顶层声明改写成模块内唯一的内部名** — `pair` → `liba$pair`（`$` 在 extC 标识符里不合法 ⇒ 不撞用户名字）；**一条不变式：`extern!` 的名字是 ABI，绝不能被 mangle**（`extern!("libc") fn read(…)` 里的 `read` 就是链接器要找的符号）。｜出处 `docs/topics/MODULES.md:523`｜状态：已定案｜实现：`mangleName`（`src/modules.c:282`）、`externKeepsName`（`:305`）、`mangleUnitDecls`（`:1318`）。
- **M-13 内部名 vs 显示名必须分家** — `name` = `io$reader`（codegen 必须唯一、查表、比较）；`srcName` = `io::reader`（诊断 / `typeStr` / 调试开关）；唯一的例外是 `EXTC_DBG_M`。泛型实例名与模块前缀的顺序：**前缀在最外**（`pair::pair<i32>` → `pair$pair_i32`）。｜出处 `docs/topics/MODULES.md:506`｜状态：已定案｜实现：`FuncDef.name`/`srcName`；`tests/modules/run.sh` 有一条自动判据。
- **M-14 别名：全名是权利、别名是方便** — 表达式位置支持任意层限定名（`std::sys::io::write(…)`）；`use std::sys::io as sysio` 只换 `shortName`，`path` 永远是用户写的全名（`path` 是"全名对全名"匹配 `use` 表的依据）；`use` 不重新导出。｜出处 `docs/topics/MODULES.md:530`｜状态：已定案（已落地）｜实现：`src/modules.c` 的 `importedAsPath`/`importedAs`；判据 `tests/qname/`。
- **M-15 缺口：跨模块泛型的**类型实参**没改写全** — `rwType` 只下探 `TY_REF` 与 `targs`；数组 `[N]m::T` 与函数类型 `fn(m::T)->R` 里的模块限定类型名永远不被改写（误拒合法程序）；诊断还全部用 `ctxError(ctx, 0, …)` ⇒ 整条类型改写路径的错误没有行号。｜出处 审计 `src/modules.c:759`（P1 + P2 `:820`）｜状态：缺口｜实现：`rwType`（`src/modules.c:759`）。
- **M-16 缺口：`EX_GENCALL` 的 callee 名从不改写** — 模块里的泛型函数一旦写显式类型实参就再也调不到（误拒 + 误导诊断）。｜出处 审计 `src/modules.c:1209`（P1）｜状态：缺口｜实现：`rwExpr` 缺 `EX_GENCALL` 分支（`src/modules.c:1132`）。
- **M-17 缺口：`isStdlibFile` 用裸前缀比较路径** — 以 `stdDir` 开头的**任意目录**都被当成标准库 ⇒ `@builtin` / 特权模块闸门被绕过。｜出处 审计 `src/modules.c:1769`（P2 UNSOUND）｜状态：缺口｜实现：`isStdlibFile`（`src/modules.c:1769`）。
- **M-18 缺口：短名冲突检查只看单个 unit 的 use 列表** — 跨文件的 `x::util` / `y::util` 漏检，两个模块共用 `util$` 前缀后互相绑错类型，报错却指向无辜代码和根文件行。｜出处 审计 `src/modules.c:1865`（P2）｜状态：缺口｜实现：`loadModules` 的短名冲突循环（`src/modules.c:1971`–`:1984`，只查 root 的 use）。
- **M-19 特权模块与 `@builtin` 只许标准库** — `PRIVILEGED_MODULES = { "std::sys::heap" }`（`std::sys::heap` 能把任意 (指针,长度) 变成视图、能 munmap/mmap ⇒ 一个 import 绕过整个语言设计）；`@builtin` 声明在入口文件里直接报错（入口不走 `loadUnit`，这条规则单独再查一遍）。｜出处 `src/modules.c:1747`–`:1765`,`:1949`–`:1962`｜状态：已定案｜实现：`isPrivilegedImport`、`loadUnit`/`loadModules` 的 builtin 闸门。
- **M-20 模块里不能有 `main`** — `main` 必须活在入口文件；每个声明带 `ctx`+`modName`，模块文件的报错走它自己那份 Ctx（"指对文件是硬要求"）。｜出处 `docs/topics/MODULES.md:486`,`:503`｜状态：已定案｜实现：`mergeUnit` 给每个声明挂 `ctx`/`modName`（`src/modules.c:1646`+）。
- **M-21 缺口/未做：类型名的"必须限定"还没挡严** — 函数/全局已经挡严，类型名没有；`use` 的 per-module 可见性传递也没做（`use` 不重新导出）。｜出处 `docs/topics/MODULES.md:529`,`:542`｜状态：缺口｜实现：`unitExports`（`src/modules.c:1417`）覆盖函数/全局/struct/enum，但类型名的裸用仍有漏。
- **M-22 增量编译/二进制分发排在后面** — 按实例增量编译技术上成立（实例驻留表/确定性 mangle/`FuncDef.used`），但实测单态化不是瓶颈（生成 C 38k 行里泛型实例只占 11.5%；gcc 占端到端 96%）⇒ 先做 `--no-line-map`/瘦身/ccache 式缓存，按实例 `.o` 最后。｜出处 `docs/topics/MODULES.md:379`,`:398`,`:414`｜状态：已定案（顺序）｜实现：`tools/golden.sh`、`bench/compile`。
- **M-23 一句话：模块系统不碰借用/逃逸检查一行，它只管"谁看得见谁"** — 检查器看到的还是**一张平表**（所以 golden 88 个单文件程序逐字节相同）。｜出处 `docs/topics/MODULES.md:160`,`:471`｜状态：已定案｜实现：`mergeUnit` 只做改名与合并。

---

## 12. IO / 事件层：行为契约 + 按需发射运行期文本的规则

### 12.1 IO 行为契约

- **I-01 读 = 整块读 + 内存里切；写 = 内存里拼 + 整块写** — 编译器只给 4 个裸原语（`i64` 进 `i64` 出），方便的那层全是 extC 写的；设计目标一句话：**让方便的那个就是快的那个**。｜出处 `docs/topics/IO.md:15`,`:77`｜状态：已定案｜实现：`stdlib/std/sys/io.extc`（extern! 签字）+ `stdlib/std/io.extc`。
- **I-02 原语层：`i64` 进 `i64` 出，不许读 `errno`** — `extc_read/write/open/close`；错误码走返回值（`-errno`），0 = EOF；不用 `FILE*`（隐藏的堆分配 + 隐藏缓冲 + 隐藏错误位置，正是 P 要禁的形态）。｜出处 `docs/topics/IO.md:152`｜状态：已定案｜实现：`stdlib/std/sys/io.extc`；`<errno.h>` 在 BOOTSTRAP §4.3 已被否。
- **I-03 长度用 `i64`（定案 69）** — 索引和长度必须同类型（`slice.len: i64`、`extc_checkedIndex(int64_t,…)`）；"非负"不是类型该表达的（`result<i64,ioError>` 已保证）；`u64` 在 extC 里是"按无符号算"而不是"非负的数"；顺带消掉 signed/unsigned 混算那类 C 经典 bug。原语层无论如何必须 signed。｜出处 `docs/topics/IO.md:86`,`:112`｜状态：已定案｜实现：`slice<T>` 的 `len: i64`（`stdlib/prelude.extc`）。
- **I-04 不提供逐字节 `readLine`；`reader` 是 64KB 块缓冲** — 逐字节实测 45× 慢（12MB/100 万行：1.444s 对 0.032s，差距全在 sys 时间）；块缓冲**隐式给**（`readerOf(fd)` 内部 `new u8[65536]`，落调用点 arena、跟 reader 同寿），行缓冲**调用者给**（拷进 `out`，"总比拷贝好"）；行是拷贝出去的 ⇒ 不存在"这个视图下次调用就失效"的隐藏约定。｜出处 `docs/topics/IO.md:71`,`:289`,`:720`｜状态：已定案（定案 74，已落地）｜实现：`stdlib/std/io.extc` 的 `reader`/`readerOf`。
- **I-05 CRLF 三条口径（定案 84）** — ① `nextLine` 剥掉行尾一个 `\r`（行 = `\n` 或 `\r\n`；EOF 前最后一个 `\r` 也剥）② `nextLineRaw` 原样（两个名字两种语义）③ 孤立的 `\r` 不是终止符，且不做边界翻译。`nextToken`/`nextInt`/`skipSpace` 走 `isSpaceByte`（含 `\r`）⇒ 本来就免疫。｜出处 `docs/topics/IO.md:234`｜状态：已定案（行为变更，实测影响面为零）｜实现：`tests/io/crlf.extc`（`od` 逐字节断言）。
- **I-06 输出只发 `\n`，要 CRLF 就显式写** — 平台翻译（Java/C#/CRT 文本模式）被否；`print`/`println` 只发 `\n`，`fs::ofstream.put` 原样写；`\r`（及 `\v`/`\f`/`\0`）转义今天就能用。将来原生 Windows 要严格 LF 得开 `O_BINARY`/`_setmode`。｜出处 `docs/topics/IO.md:248`｜状态：已定案｜实现：`tests/io/crlf.extc` 的 `41 0d 0a 42 0a` 判据。
- **I-07 文件归属 = 程序拥有（定案 79）** — 编译器不记账、块退不隐式关闭、arena 一个字节都不参与；生成代码里没有 fd 表、没有释放行。三条理由：块退没有失败通道（写型 `close(2)` 是延迟写错误唯一报告点）；fd 是稀缺 OS 资源；隐式+显式两套机制要养一套幂等记账。｜出处 `docs/topics/IO.md:365`｜状态：已定案｜实现：`stdlib/std/fs.extc`。
- **I-08 `close()` 是唯一机制、且幂等** — 句柄自带 open 标志，第二次 `close()` 直接返回、不碰 `close(2)`（否则关掉的是陌生人的描述符）；写型没有单独 `commit()`（`close(2)` 就是"还有话要说"的调用）；关闭后使用 ⇒ `failure(closed(fd))` 带位置（不是 trap、不是静默）。｜出处 `docs/topics/IO.md:390`｜状态：已定案｜实现：`tests/fs/close-twice.extc`（a 关两次后 b 拿到同一个号、b 仍能写）。
- **I-09 忘关不静默：能被证明的泄漏是一条警告** — "局部句柄、全程没有 `close`" ⇒ 警告（**不是错误**，留到进程结束合法，`-w` 能关）；函数里有 `close`（哪条路径都算）⇒ 静默；句柄交出去/`return`/存进字段或数组 ⇒ 静默（证明不了，那一半归运行时的 fd 漂移判据）。检查器认的协议是**库自己声明的**：struct 声明了 `close` 方法就是资源类型（编译器里不出现库名）。｜出处 `docs/topics/IO.md:401`｜状态：已定案（已落地）｜实现：`check_top.c:3585`（`opened and never closed` warn）。
- **I-10 自动释放的资格判据（留给将来）** — 同时满足三条才配自动释放：①释放不会失败（没有信息可报）②释放没有顺序/协议语义 ③只由标准库少数原语获取；今天这三条**没有任何资源在用**（fd 因①对写型不成立而显式）。｜出处 `docs/topics/IO.md:440`｜状态：已定案（判据留存）｜实现：无。
- **I-11 `print`/`println` 不可失败、不缓冲（一条一次 `write(2)`）** — 终端场景正是要这样；要刷量就内存拼完一次 `writeAll`；不搞"看不见的输出缓冲"（那会偷偷改变"什么时候能看见输出"）。｜出处 `docs/topics/IO.md:446`｜状态：已定案（部分被定案 90 取代，见 I-13）｜实现：`println` 走内建打印运行期（`needRuntime`）。
- **I-12 流式 `cout << x` / `cin >> x`（定案 85）** — 三条形状决定：句柄只装 `fd`（`<<` 必须返回 `ostream`，而 `-> ostream` 里 self 是引用 ⇒ 造新薄句柄，拷贝无害、链式成立）；输出缓冲交给 C 的 stdio（`flush()` = `fflush(NULL)`，退出自动冲刷、与 `println` 顺序天然一致）；输入缓冲是全局定长数组（token 可能停在块中间 ⇒ 位置必须活得比一次 `>>` 长；含引用的 `mut slice<u8>` 不能当全局），解析复用 `reader`。｜出处 `docs/topics/IO.md:464`,`:491`｜状态：已定案（已落地）｜实现：`stdlib/std/io.extc`（`cin/cout/endl/inBad`）。
- **I-13 两个 `>>` 语义决定** — 读到东西之前先问 `eof()`（`nextInt` 在输入结束时返回 `success(0)`，与"读到的就是 0"同形）；**读不到 ⇒ 目标保持原值 + `inBad()` 为真**（不是静默写 0、也不是 trap；`a >> b` 站在 `a` 的位置上，`>>` 只能返回流，报错没有别的出口；trap 会让"读到 EOF 为止"的循环写不出来）。｜出处 `docs/topics/IO.md:499`｜状态：已定案｜实现：`tests/io/stream.extc` 四格判据。
- **I-14 文件也走同一套（`fin >> n >> line >> b`）** — 状态在流自己身上 ⇒ `ifstream` 是 `@noCopy`，`>>` 返回 `mut ref ifstream`（定案 88：链式靠"返回的是同一个对象"）；错误用标志 `bad()` 而不是返回类型（两层分工：`>>` 顺手时用、`result` 认真时用）；`fin.reader()` 交出的是**内部那个** reader ⇒ 与 `>>` 混用不会跳字节。｜出处 `docs/topics/IO.md:513`｜状态：已定案（已落地）｜实现：`stdlib/std/fs.extc`、`stdlib/stl/stringio.extc`。
- **I-15 `main` 里的 `?` 直接 trap（定案 89）** — `?` 的语义是"把失败交给外层"，`main` 的外层就是进程边界 ⇒ 交出去的方式是说清楚然后死（带源码位置 + 退出码 1）；失败消息的载荷走描述符打印；**只对 `main` 生效**，别的函数仍必须声明 `result` 才能用 `?`。｜出处 `docs/topics/IO.md:532`｜状态：已定案（已落地）｜实现：`tests/io/main-question.extc` + `tests/traps/main_question.extc`。
- **I-16 五个流与 `cerr` 的区分** — `cin`/`cout`/`cerr`/`fin`/`fout`；`print`/`println` 打 **stdout**（`prog > out.txt` 能抓到，`2>/dev/null` 杀不掉），`cerr <<` 才是 stderr；`trap:` 那类消息也走 stderr；`cerr` 写之前先 flush stdout ⇒ 顺序不插队；整数格式化三个流共用 `io::fmtI64` 一份。｜出处 `docs/topics/IO.md:570`｜状态：已定案（已落地；`f64` 的 `<<` 待格式化层搬进库）｜实现：`stdlib/std/io.extc`。
- **I-17 程序级 `fin`/`fout` 与"静态段打开"被否** — 形状与控制台流同一招（模块全局定长数组 + 薄句柄 + 每次 `>>` 借本地 reader 读写 len/pos），所以**不需要任何语言规则**；不做静态段打开（要引入一趟运行期初始化 ⇒ 失败发生在 `main` 之前没人接住、没有析构 ⇒ 谁关、跨模块顺序要立新规矩；全局今天连"持有一个引用"都做不到）。｜出处 `docs/topics/IO.md:597`,`:641`｜状态：已定案（已落地）｜实现：`stdlib/std/fs.extc` 的 `openIn/openOut/closeIn/closeOut`。
- **I-18 控制台输出是缓冲的（定案 90）** — `cout << ...` 写进一个 **256 KB** 缓冲（生成器提供，`main` 返回前收尾），不再是"一条操作数一次 printf"：实测 3407 → 203 ms（10M 行 / 224 MB）；**顺序契约**：`cout` · `io::writeBytes` · `cerr` **共用一个缓冲** ⇒ 三者不可能乱序，要立刻出去就 `io::flushCout()`；已弃置的 `print`/`println` 走 stdio（另一支缓冲）⇒ 与上面三条混用不保证顺序。｜出处 `docs/topics/IO.md:630`｜状态：已定案（已落地）｜实现：`extc_cout_buf[1 << 18]` + `extc_cout_put/flush`（`src/codegen.c:6890`–`:6929`）；`cerr` 先 `flushCout()`（`stdlib/std/io.extc:874`）。
- **I-19 `f64` 的库层格式化还没搬** — `cout << 3.5` 今天仍借内建 `print`（所以它之前会先冲刷一次）；`>>` 今天只有 `i64`（reader 只提供 `nextInt`），`f64`/字符串/整行还要各自的解析；要写任意 fd 也得等格式化层搬进库（那正是 `print` 退役的前提）。｜出处 `docs/topics/IO.md:639`,`:657`,`:659`｜状态：缺口｜实现：`extc_cout_f64` helper（`src/codegen.c:6940`）。
- **I-20 格式化输入：A（格式=函数名，类型驱动）今天、B（编译期格式串）以后** — `nextInt/nextFloat/nextWord/line/nextChar/nextBool` 就是"格式化输入"，只不过格式不是字符串而是函数名（编译期知道类型、运行时零解析）；B 需要"编译期读一个字符串字面量"的能力（现在没有），但它不影响 A 的形状（A 就是 B 的展开结果）。`scan(mut a, mut b, …)` 要，但走**真变参**、现在不写。｜出处 `docs/topics/IO.md:322`,`:336`,`:732`｜状态：已定案（A 落地，B 提案）｜实现：`stdlib/std/io.extc` 的 `nextInt` 一族。
- **I-21 argv** — `fn main(args: slice<slice<u8>>) -> i32`：`args.len` **含程序名**（与 C 一致）、字节**不拷贝**（视图直接指向操作系统的实参块，比任何 arena 都活得久）、视图数组建在 **main 自己的帧 arena**（`&__extc_a[1]`）；形状写错是编译期 extC 报错（三条）；不做 `args()` 内建（它返回不了：数组得放进调用者帧 arena ⇒ 深度 1）。｜出处 `docs/topics/IO.md:661`｜状态：已定案（已落地）｜实现：`tests/argv/`。
- **I-22 open 读/写型分开（定案 77）** — `fs::openRead(p) -> ifstream` · `openWrite(p)`（截断）· `openAppend(p)`；读型/写型是两个 struct ⇒ 把写型当读型用是编译期错误；`O_*` 与 POSIX 数只许出现在 `std::sys::io`。｜出处 `docs/topics/IO.md:689`｜状态：已定案（已落地，实测在 IO-1/IO-2）｜实现：`stdlib/std/fs.extc`、`tests/fs-shape/`。
- **I-23 `reader` 是一个普通 struct + 一大堆方法**，底下全部调同一套 `rawRead`；`readAll` + 游标是"榨性能专用"（日常是 `reader`）；IO-2 三件（`f.reader()` · `proc::exit` · termios raw + trap 路径还原终端）已落地。｜出处 `docs/topics/IO.md:702`,`:727`,`:730`｜状态：已定案（已落地）｜实现：`stdlib/std/io.extc`、`stdlib/std/term.extc`、`stdlib/std/proc.extc`。
- **I-24 未决问题** — 原语的名字（`rawRead`/`readRaw`/`sysRead`/`read`）；`readAll` 读满之后剩的怎么办（本文选"返回读到的字节数、再调一次"，`readExactly` 要不要）；格式化输入 B 的时机；`{ }` 块要不要变成释放点（要 arena 支持 mark/release）；`scan` 的真变参。｜出处 `docs/topics/IO.md:711`｜状态：提案（未拍）｜实现：无。

### 12.2 按需发射运行期文本：哪个标志/前缀门控哪段文本

- **I-25 总规则** — 每段运行期文本由一个"声明或名字前缀"推导出的 `need*` 标志门控；不需要它的程序**一个字节都不发**（判据是 `tools/golden.sh` 414/414 逐字节不变）；标志的判定必须在 preamble 之前跑（`_POSIX_C_SOURCE`/`_GNU_SOURCE` 只对第一个 include 之前有效）。｜出处 `docs/topics/HEAP.md:228` + `docs/topics/C-ABI.md:472`｜状态：已定案｜实现：`src/codegen.c:6769`–`:6852`（一次性声明扫描）、`:7041`,`:7045`（特性宏）、`:8427`–`:8468`（发射顺序）。
- **I-26 事件层门控** — `used` 的 extern 名以 `extc_epoll_`/`extc_sock_`/`extc_tcp_` 开头 ⇒ `needEvent` ⇒ `eventEmitRuntime`。｜出处 `src/codegen.c:6782`–`:6788`、`:8440`｜状态：**与代码不符/缺口**：前缀表**漏了 `extc_unix_`** ⇒ 只用 `extc_unix_listen/connect` 的程序 `needEvent` 为假、整块事件层（连同这两个定义）不进产物 ⇒ 链接失败；而 `--check-c` 只做 `-fsyntax-only`，编译器自己的闸门报通过。｜实现：`eventEmitRuntime`（`src/coroutine.c:106`）按名字前缀判定（`src/codegen.c:6785`）。
- **I-27 DNS 门控与 `<arpa/inet.h>` 缺口** — 出现 `extc_dns_lookup` **声明** ⇒ `needDns` ⇒ `dnsEmitRuntime`；`_POSIX_C_SOURCE 200809L` 由 `needTime || needDns` 发（`clock_gettime/nanosleep/getaddrinfo` 在严格 c11 下不可见）。｜出处 `src/codegen.c:6831`,`:7041`、`:8433`｜状态：**与代码不符/缺口**：`eventEmitRuntime` 里用了 `inet_pton`，而 `<arpa/inet.h>` 只在 DNS 块里 include ⇒ 只声明 `extc_tcp_connect_to` 的程序（`needEvent` 真、`needDns` 假）生成 C 编不过（gcc≥14 把隐式声明当错误）。｜实现：`eventEmitRuntime` 的 include 表（`src/coroutine.c:111`–`:117`）与 `dnsEmitRuntime`（`:328`）。
- **I-28 文件层门控** — extern 名以 `extc_file_`/`extc_sendfile`/`extc_http_` 开头 ⇒ `needHttpFile` ⇒ `fileLayerEmitRuntime`，并触发 `_GNU_SOURCE 1`（`strptime`/`timegm`/`gmtime_r` 只有 `_GNU_SOURCE` 下可见，`timegm` 连 POSIX 都不是）。｜出处 `src/codegen.c:6828`–`:6830`,`:7045`、`:8432`｜状态：已定案（已落地）｜实现：`fileLayerEmitRuntime`（`src/coroutine.c:362`）。
- **I-29 时间门控** — `externLib == "extc-time"` ⇒ `needTime` ⇒ `timeEmitRuntime`（时钟与 sleep）。｜出处 `src/codegen.c:6824`,`:8439`｜状态：已定案｜实现：`timeEmitRuntime`（`src/time.c:13`）。
- **I-30 字节搜索门控** — `externLib == "extc-mem"` ⇒ `needMemFind`（`std::sys::mem` 是唯一声明这些的模块）；`_GNU_SOURCE` 块也读这个标志。｜出处 `src/codegen.c:6814`–`:6818`,`:8436`｜状态：已定案｜实现：`memfindEmitRuntime`（`src/memfind.c:23`）。
- **I-31 板门控** — `externLib == "extc-heap"` + 名字分派：`extc_memCopy` ⇒ `needPlateCopy`，其余 ⇒ `needPlateView`；`@builtin` 的 `extc_viewOf` 也置 `needPlateView`（它没有 `externLib`，上面那条按 externLib 的扫描看不见它）。｜出处 `src/codegen.c:6819`–`:6823`,`:6841`–`:6843`,`:8437`–`:8438`｜状态：已定案｜实现：`plateEmitViewOf`/`plateEmitMemCopy`（`src/plate.c:8`,`:24`）。
- **I-32 池门控（两条来源）** — ① extern 名以 `extc_pool_` 开头 ② 检查器早算好的 `makesPool`（含方法；只看声明是不够的：声明在库模块里而 `m` 是入口模块 ⇒ main 的 prologue 那一刻 `needPool` 还是假 ⇒ 帧的 zone 记号不发、提权无法表达）。`makesPool` 是可传递最小不动点。｜出处 `src/codegen.c:6812`,`:6833`–`:6851`,`:8431`｜状态：已定案｜实现：`poolsEmitRuntime`（`src/pools.c:37`）。
- **I-33 控制台门控** — 声明了 `extc_cout_put` ⇒ `needCout`；用到 `extc_cout_f64` ⇒ `needCoutF64`（`std::io` 的 `<<(f64)` 是它唯一用户，不打印浮点的程序不该背这个格式化器）。｜出处 `src/codegen.c:6813`,`:2449`,`:8429`–`:8430`｜状态：已定案｜实现：`g.rtCout`/`g.rtCoutF64`（`src/codegen.c:6890`,`:6940`）。
- **I-34 打印与 trap 门控** — `needRuntime`：内建打印（`println`/`extc_print`）与 `extc_trapMsg` 各自置位 ⇒ 发 `g.rt`（+ `g.rtPrint`）；`g.rtEq` 由 `eqNeed` 决定（打印或比较谁先要就发）。｜出处 `src/codegen.c:1705`,`:3148`,`:3742`,`:8427`–`:8428`,`:8468`｜状态：已定案｜实现：`genPrint`/trap 发射路径。
- **I-35 并行门控（两个标志分开）** — `needPar` ⇒ 线程运行期（或 `EXTC_DBG_PAR=1` 调试构建）；`needParRegion`（有 worker 会 `new`）⇒ 额外发 `#define EXTC_PAR_REGION 1`；区域那半套整段被 `#ifdef` 包着（不是死代码、不进二进制）。｜出处 `src/codegen.c:8222`,`:8309`–`:8310`｜状态：已定案｜实现：`extc_par_run` 运行期文本。
- **I-36 域门控** — `needDomain` ⇒ 先为每个 `isExtTarget` 协程发帧 typedef + `$step`/`$next` 原型 + `$domstep` 适配器，再发域运行期（`domainEmitRuntime`）；**适配器必须发在前言**（调用点位于函数体内，而协程段最后才追加）。｜出处 `src/codegen.c:2150`,`:2273`,`:3780`,`:8441`–`:8459`｜状态：已定案｜实现：`domainEmitRuntime`（`src/domain.c:19`）。
- **I-37 协程门控** — `coroBoxed` ⇒ `needCoroHandle` ⇒ 句柄 typedef 提前到 preamble（原型与 `vector<coroutine<T>>` 的实例结构体会先提到它）；任务表 + 池运行期在最终装配时 splice 进 `coroDefA` 位置（顺序：句柄定义 → 池运行期（`poolDone` 去重）→ 任务表），并把 `needPool` 置真。｜出处 `src/codegen.c:6777`–`6779`,`:7072`–`:7076`,`:8245`–`:8274`｜状态：已定案｜实现：`genCoroHandleDecls`/`coroutineEmitRuntime`（`src/coroutine.c:21`）。
- **I-38 dyn 门控** — `m->usesDyn` ⇒ `ExtcDynHandle` typedef（preamble）+ `poolsEmitDynRuntime`（单独一段，让只用池的程序保持逐字节不变；dyn 程序建值时本来就置 `makesPool`）。｜出处 `src/codegen.c:7064`–`:7069`,`:8460`–`:8461`｜状态：已定案｜实现：`poolsEmitDynRuntime`（`src/pools.c:110`）。
- **I-39 原始终端门控** — 声明 `extc_raw_enter` ⇒ `needRawTerm` ⇒ 原始终端块（`tcsetattr` 用与库相同的原型声明，`struct termios` 布局从不被命名）。｜出处 `src/codegen.c:6809`,`:8462`–`:8466`｜状态：已定案｜实现：`g.rtRaw`。
- **I-40 事件层内部约定（按需发射后仍要守）** — 一次返回一个 tag、FIFO 环形 `pending[64]`（从前是栈 ⇒ 后到先服务 ⇒ 秒级尾延迟）、满了丢（注册全是 level-triggered 会重报）、`-100` = 队列空（tag ≥ -1）。｜出处 `src/coroutine.c:129`–`:140`｜状态：已定案｜实现：`extc_epoll_wait`（`src/coroutine.c:137`）。
- **I-41 缺口（审计 P2）：`pending` 是函数级 `static`，不按 epoll 实例区分** — 一次 wait 多出来的事件会在下一次对**另一个** epoll 实例的 wait 里被还出去 ⇒ 跨实例投递别人的 tag（调度器按 tag 直接派发 ⇒ resume 了另一个调度器同号的行）。注册全是 level-triggered，丢事件本身安全，错的是不认实例。｜出处 审计 `src/coroutine.c:137`（P2）｜状态：缺口｜实现：`extc_epoll_wait`。
- **I-42 缺口（审计 P2）：unix/tcp listen/connect 失败路径不关 fd** — `socket()` 已拿到 fd，之后 `len` 非法/`connect`/`bind`/`listen` 失败都直接 `return -1`；调用方看到"失败 -1"自然重试，每次漏一个 fd，最终 EMFILE。对照 `extc_tcp_connect_to` 的 connect 失败路径**是**关的 ⇒ 漏写而非约定。｜出处 审计 `src/coroutine.c:203`（P2）｜状态：缺口｜实现：三个 listen 路径 + `extc_unix_connect`。
- **I-43 缺口（审计 P2）：`extc_http_date` 固定 64 字节上限写调用方缓冲区** — extC 侧声明 `out: ref u8` 不带长度，容量既没传进来也没写进契约；调用方给小于 30 字节的对象就越界写（实测返回 29 且窗口后被写）；同文件其它 shim（sock_read/write、sendfile、dns_lookup）都带 n/outcap。｜出处 审计 `src/coroutine.c:403`（P2）｜状态：缺口｜实现：`fileLayerEmitRuntime`。
- **I-44 缺口：`--check-c` 只做语法检查，且自己会漏** — 它只跑 `-fsyntax-only`（`src/main.c:565`），所以 I-26 那类"声明有、定义无"的链接期错误编译器闸门报通过。｜出处 审计 coro 条目 + `docs/reviews/COMPILER-AUDIT-2026-09-30.md` §3.3｜状态：缺口｜实现：`main.c` 的 `--check-c`。
- **I-45 缺口（P0-22）：按需剪枝的 `dropRuntimeDefs` 有死循环** — `if (!defs) continue;` 跳过了循环末尾唯一的推进语句（`ln = eol + 1`），而上一轮剪枝会把运行期文本切成截断的 `#define EXTC_ZON` 片段，于是同一行被扫 900 万次 ⇒ `stdlib/stl/hashSet.extc`（52 行）作根文件时 100% CPU 永不返回、无诊断；`use stl::hashSet` 的程序不触发。修法一行：`if (!defs) { if (!eol) break; ln = eol + 1; continue; }`，同族两个扫描循环（`:5709`,`:5979`）一并审计。｜出处 审计 P0-22 `src/codegen.c:5798`｜状态：缺口｜实现：`dropRuntimeDefs`（`src/codegen.c:5798`）。

---

## 13. 插件 / 库分发 / 内联 C

- **L-01 插件四档：①进程内选表（`varArray<dyn Trait>`）②同进程+宿主池+只走拷贝+沙箱分配 ③同进程裸 dlopen ④独立进程+行协议** — 档①已落地（默认）；③"事实上可以不做"（收益全被②覆盖、风险大得多，`@export` 只服务③ ⇒ 跟着推迟）；④不需要 `@export`（插件是独立可执行文件，`main(args)` 读协议今天就能写）。｜出处 `docs/topics/PLUGINS.md:17`｜状态：已定案（暂缓实施，讨论归档）｜实现：档①注册表 = `varArray<dyn Trait>`（`tests/` 与 `DYN.md`）。
- **L-02 `dlopen` 不是"插件"，是延迟链接** — 加载完那一刻那段代码就是你程序的一部分；分界是二元的（要么它是你，要么它在另一个进程里），中间没有"只读不能写/信任一半"这种档（in-process 想要那种保证现实里只有 WASM 那类线性内存沙箱，而那就不是 C ABI 了）。｜出处 `docs/topics/PLUGINS.md:13`｜状态：已定案｜实现：无（认知口径）。
- **L-03 术语：第②档叫 extension（扩展）不叫 plugin；第④档才配叫 plugin**。｜出处 `docs/topics/PLUGINS.md:30`｜状态：已定案｜实现：无。
- **L-04 第②档设计（若将来做）** — 池由宿主拥有（`extc_plugin_pool_create(limit)` → `poolId`；插件里所有分配改成调用宿主提供的符号；卸载 = pool reset + drop ⇒ 一次全收零泄漏）；ABI 只走一个邮箱（全程零指针）；返回值一律夹紧（长度/偏移对邮箱大小检查）；世代句柄（插件残留句柄卸载后立刻失效）；trap 不许跨边界（extC 的 trap 是 `fputs + exit(70)` 硬退整个进程 ⇒ 插件必须编一个"边界模式"把 trap 收敛成返回码）。｜出处 `docs/topics/PLUGINS.md:64`｜状态：提案（未落地）｜实现：无。
- **L-05 三条工业纪律** — 谁分配谁释放 · panic/trap 不许跨边界 · 不卸载（`dlclose` 会让悬垂的 vtable/函数指针留在宿主里）。｜出处 `docs/topics/PLUGINS.md:75`｜状态：已定案（纪律）｜实现：无。
- **L-06 危险清单与实测** — 地址空间共享 ⇒ 插件能算地址、调 mmap/sbrk；`malloc` 符号插入"全有或全无"（libc 内部有自己的 malloc ⇒ 插件 strdup 的块被宿主 free ⇒ 崩）；崩溃无解；两个运行期副本（插件自带池表/zone ⇒ 跨界的"共享状态"必须重新定义，要自包含得 `-fvisibility=hidden` + `-Wl,-Bsymbolic`）；平台差异。实测：socketpair 往返 21.8µs、共享内存+eventfd 23.3µs（并不更快，钱花在两次唤醒）、进程内直接调用 1.48ns、间接调用 1.62ns；分界线 ≈4.5k 往返/秒 = 10% 一个核。｜出处 `docs/topics/PLUGINS.md:35`,`:48`｜状态：已定案（记账）｜实现：无。
- **L-07 什么时候回来做** — ①"不重新编译就要换实现"的真实需求 ②且该扩展在热路径上、④的往返成本超过 10% 一个核 ③且实现由自己人写（或接受"插件能改我内存"）；三条同时成立才做②/③。｜出处 `docs/topics/PLUGINS.md:126`｜状态：已定案（触发条件）｜实现：无。
- **L-08 DYN 侧为卸载留的三条不变量（现在就要守）** — ①表指针只许出现在**槽**里（不进句柄/载荷/任何 ABI 可见布局）②键是**稳定的 mangled 名**（`extc_vt$Trait$Type`，不依赖插入序）③有效性**只看槽/池世代**、不看"哪个模块装的"；统一 struct 的字段顺序 = 声明顺序 ⇒ 给 trait 加方法会改布局 ⇒ 开放注册必须"追加式 + 版本号"。｜出处 `docs/topics/PLUGINS.md:80`｜状态：已定案（留门不出手）｜实现：`DYN.md` §10 的接缝（`src/pools.c:118` 的 dyn 运行期）。
- **L-09 库的四种分发模型与建议顺序** — ①源码库（编译时合进同一个 TU）②二进制库+接口文件 ③extC 库给 C 用户（生成 `.h`）④消费 C 库（extern 声明）；顺序：①先做（= `MODULES.md` 方案 A）→ ④紧接着（IO 需要）→ ③顺手（几乎白送）→ ②最后（构建系统的活）。L1 模块 / L2 `extern!`+信任声明（定案 72）/ L3 `std.io`（定案 73）已落地；L4 给 C 的 `.h`、L5 二进制分发待做。｜出处 `docs/topics/LIBS.md:111`,`:195`｜状态：已定案（L1–L3 落地，L4/L5 未做）｜实现：`src/modules.c`、`stdlib/std/io.extc`。
- **L-10 extC 库 API 天生"按区域参数化"** — 库要产出数据就问调用者要区域（A3 的"家"机制）⇒ "谁负责释放"在类型层面就不存在（没有 free/GC/drop）；代价是库的内存行为是它签名的一部分，库文档必须写清"我一共要多少、什么时候要"。｜出处 `docs/topics/LIBS.md:127`｜状态：已定案｜实现：needsHome/提权（`src/codegen.c:1744`）。
- **L-11 接口必须带效果摘要** — `Addr(j)` 存了 `&arg_j` / `Cont(j)` 存了从 `arg_j` 读到的指针；摘要不完整（递归/解析不出来的调用）⇒ 调用点保守拒（"对所有含引用的实参按最坏情况查"）；源码库也能用（摘要从源码算）。落点：方案 B 的接口文件里导出函数要带 `Addr/Cont/homeAddr/homeCont/otherMask + 是否完整`（`--dump-effects` 打印的就是这些）。｜出处 `docs/topics/LIBS.md:142`｜状态：已定案（源码库已实现，接口文件未做）｜实现：`computeEffectsTransitive`（`src/check_top.c`）。
- **L-12 `extern!` 的信任声明最小形状 = `effects` + `owned`（LQ3）** — 漏写 `effects` ⇒ 最保守（安全但难用）；`owned` 管"谁释放"。｜出处 `docs/topics/LIBS.md:216`｜状态：**缺口**：`effects` 已落地，`owned` 未实现且写了报错（见 A-17）。｜实现：`parseExtern`（`src/parser.c:399`）。
- **L-13 C ↔ extC 的类型/所有权映射** — `slice<u8>` = `const char*` + `size_t` 两个参数；`mut slice<u8>` 是出参缓冲（IO 的主力形状）；`?ref T`/`ref T`/`mut ref T` 零成本（C 那边没有只读概念 ⇒ 只读承诺过不了边界）；**`varArray<T>` 不可传**（带 `home: ref arena` ⇒ 泄露内部，要传就 `asSlice()`）；extC struct 要过 C 边界必须冻结布局；trap ↔ `abort()`；arena 内存 ↔ `malloc` 块（方向 1 完全安全；方向 2 按定案 79 显式释放）。｜出处 `docs/topics/LIBS.md:178`｜状态：已定案（映射表；`@frozen` 就是那份"冻结"）｜实现：`ttCrossesC`（`src/check_top.c`）。
- **L-14 给 C 的 `.h` 一旦发布就冻结（LQ1/LQ4）** — 建议先只给 extC、给 C 是永久承诺等接口稳了再签；`@c` 布局"等真要给 C 用再说"（冻结是单向门）。｜出处 `docs/topics/LIBS.md:202`,`:218`｜状态：已定案（等待）｜实现：`@export` 已给符号（`C-ABI.md:350`），头文件生成未做。
- **L-15 `inline C!` 是签字族的第四条出口** — 与 `!`/`extern!`+effects/`subView` 同族："这段 C 的后果见下面的申报"；语言负责把声明显式化、可检索、可评审。｜出处 `docs/topics/INLINE-C.md:13`｜状态：提案（**归档，作者未想清楚，不做**）｜实现：无。
- **L-16 `inline C!` 签字要申报的四类后果** — ①收到的引用是否会被存到活得更久的地方（`Addr=`）②是否可能不返回（长跳转/exit，`Cont=`）③是否分配、分配之物归谁（需新词汇，影响 home 机制与物化位置）④是否会让池里的东西失效（`give`/`resize`/换代）——**第 4 条是它与池/dyn 那条线的交汇点**：若沙箱禁止块触碰 extC 管理的存储，则第 4 条按构造恒为否，签字退化为"只申报进出数据的形状"。｜出处 `docs/topics/INLINE-C.md:37`｜状态：提案（未定案）｜实现：无。
- **L-17 `inline C!` 两层分工与沙箱** — 签字保证诚实、沙箱保证后果可控；不加沙箱时块里的 UB 会伤到 extC 的不变量（语言承诺作废），加沙箱后只伤自己、垃圾落回 `result<T,E>` 值域。｜出处 `docs/topics/INLINE-C.md:28`｜状态：已定案（分工口径）｜实现：见 `BOOTSTRAP.md` §4.4（本文不重复）。
- **L-18 `inline C!` 待定七问** — 拼写与位置（语句位还是表达式位）；块内如何称呼 extC 变量；能否触碰编译器运行期全局（默认应当不能，否则 TCB 一次性扩到整个运行期）；`#include` 处理；`#line` 保持；沙箱档位；首版是否只做签字（不做沙箱时爆炸半径是整个程序，必须写明）。｜出处 `docs/topics/INLINE-C.md:51`｜状态：提案（未定）｜实现：无。
- **L-19 `inline C!` 与运行期文本的远期关系** — "运行期自身用 prelude 的 extC + `inline C!` 块写"是一条可能的路，可逐步摆脱"一大段字符串"的形态；这是远期方向，不构成近期理由（近期仍以独立重构处理）。｜出处 `docs/topics/INLINE-C.md:64`｜状态：提案（远期）｜实现：运行期目前是 `bufPuts` 字符串（`src/pools.c:37`、`src/coroutine.c:21`、`src/domain.c:19` 等）。

---

## 14. 汇总：含糊、矛盾、"文档说有但代码里没有"

### 14.1 最严重的三个含糊处

1. **任务 place 的释放假设 LIFO，但任务按结束顺序死亡**（`docs/topics/CONCURRENCY.md:551`,`:803` 只写"跑完一次性 `zoneLeaveTo`"，没写前提；代码 `src/coroutine.c:84` 无条件按 mark 弹栈）。后果是双向的：装箱路径弹掉仍存活任务的 zone（P0-20 ASan UAF），非装箱路径一个都不弹（place 永久泄漏，5 万次 263 MB）。**这是本单元里唯一同时有"设计缺口 + 实测 UAF"的地方**，必须先裁定"任务的 zone 是否还留在全局栈上"（建议按任务自己的 mark 精确释放或每任务独立 arena + `destroy`）。
2. **`effects Thread=` 的缺省在两处说法相反**：`src/ast.h:824` 与设计口径说"缺省保守 = 1（假定碰共享状态）"，实现只在**写了 effects 子句**时才置 1（`src/parser.c:342`,`:345`），没写 effects 的 `extern!` 保持 0 ⇒ worker 白名单（`src/check_expr.c:108`）放行它（审计 TSan 实测 data race）。缺省值是安全边界，不能靠"大概"。
3. **按需发射的触发名单是名字前缀的散列表，没有单一来源**：`extc_unix_` 漏了（链接失败，`--check-c` 报通过，`src/codegen.c:6785`）、`inet_pton` 的 include 挂在 DNS 块（`src/coroutine.c:230`）、`extc_pool_`/`extc_file_`/`extc_http_`/`extc_cout_` 各写各的前缀；`dropRuntimeDefs` 还会把文本切坏并死循环（P0-22，`src/codegen.c:5798`）。"一个声明一个标志"是纪律，但没有机制保证前缀表与运行期定义同步。

### 14.2 文档说有、代码里没有（或反过来）

| # | 文档说 | 代码 | 状态 |
|---|---|---|---|
| 1 | `effects Thread=` 缺省 1（碰共享状态）｜`src/ast.h:824` | 只有写了 effects 才置 1；无 effects 的 extern 是 0 | 与代码不符（缺口） |
| 2 | 池句柄计数器"放 48 位"约 32 天绕回｜`docs/topics/POOLS.md:318` | `HANDLE_EPOCH_BITS=32`、gen 也只有 32 位｜`stdlib/stl/pool.extc:55`,`:120` | 与代码不符 |
| 3 | 用户面 `p.own/free/compact`｜`docs/topics/POOLS.md:264` | 真实面是 `insert/remove/shrink/release`｜`stdlib/stl/pool.extc` | 与代码不符（文档内部也自相矛盾） |
| 4 | 并发前置"数组原子发布 + 注册表改 `_Thread_local`"｜`docs/topics/POOLS.md:321` | 全部仍是进程级 `static`；且该段文本损坏 | 缺口 |
| 5 | `@poolObject` 的隐藏"家 zone"参数 | 已实现（`src/codegen.c:1744`/`:1775`/`:2444`） | 文档与代码一致 |
| 6 | `grep keep_bytes src/ stdlib/` 无命中（保留池不在树里） | 与 §7.4.2 的"已回滚"一致 | 一致 |
| 7 | `ext` + 调度域"一行代码都没有"｜`docs/topics/CONCURRENCY.md:712` | 解析器 `EX_EXT`、`src/domain.c`、`tests/ext/` 都在 | 与代码不符（文档低估） |
| 8 | `coroutine<A,B>` 没做｜`docs/topics/CONCURRENCY.md:713` | 已支持两参数（`src/check_top.c:3905`,`:4147`；`tests/coro/coro_shorthand.extc`） | 与代码不符（文档低估） |
| 9 | `fn` 是"唯一一处不改语言写不出这页文档"｜`docs/topics/HEAP.md:321` | `fn` 已落地（`docs/topics/C-ABI.md:160`） | 与代码不符（文档低估） |
| 10 | `@export` 随第③档推迟｜`docs/topics/PLUGINS.md:26` | `@export` 已落地（`docs/topics/C-ABI.md:350`） | 与代码不符（文档低估） |
| 11 | 板"Arena 兜底" | 只有 `close`，忘了关漏到进程结束｜`docs/topics/HEAP.md:157` 自认 | 缺口（文档自认） |
| 12 | `extern!` 的 `owned`（LQ3 最小形状之一） | 写了直接报错"not implemented"｜`src/parser.c:440` | 缺口（文档自认） |
| 13 | 跨界的结构体布局 / 给 C 的 `.h` | `@frozen` 已给按值门票；`.h` 生成未做｜`docs/topics/LIBS.md:202` | 缺口（文档自认） |
| 14 | `src/modules.c:12` 注释说 `use` 解析 "next to the importing file" | 实际只看 rootDir/-I/stdDir｜`src/modules.c:396` | 与代码不符（src 注释过时；MODULES.md 正确） |
| 15 | 池"对象表模式下 give/resize 直接 trap"⇒ 池档 sound｜`docs/topics/POOL-SOUNDNESS.md:240` | 槽位回收不清 `extc_poolKind` ⇒ 容器池可能继承对象表模式假 trap | 缺口（`src/pools.c:41`） |
| 16 | `Hostile` 威胁模型"防错不防敌" | 实测与文档一致（十种攻击）｜`docs/topics/C-ABI.md:677` | 一致 |

### 14.3 条目统计

- 正文条目：**R 18 · Q 17 · K 14 · E 16 · H 18 · C 50 · T 25 · A 29 · M 23 · I 45 · L 19 = 274 条**（按节内编号逐条可数；跨节重复只记一次）。
- 状态分布（机器按条目首行"状态："字段统计）：**已定案 194 · 缺口 54 · 与代码不符 11 · 提案 14 · 部分落地 1**。

### 14.4 建议的裁定顺序（与审计报告 §8 对齐）

1. **先裁定 C-27/C-28/C-29（任务 place 的 zone 归属）**——它同时是 UAF 与泄漏，且决定"帧能不能跨函数传"（切片 C 的承诺）。
2. **T-15/`Thread=` 缺省 + T-20/TLS arena 适用条件**——并行线的安全边界，两条都会静默产出错程序。
3. **I-26/I-27/I-45（按需发射的名单与剪枝）**——影响所有"产物能不能编过"的判据；前缀表要收成一处数据 + 闸门。
4. **K-04/K-07（kind 未清 + 字节数钳位）与 M-09/M-10（@private 两个洞）**——局部修法、影响面明确。
5. 其余 P1/P2 按审计报告批次走；E-11（位宽口径）与 Q-11/Doc 陈旧的修订可以随文档轮一并做。
