# DOC-AUDIT：文档断言 vs 源码对账

> 审计人：doc-audit（task-4）。基线：git HEAD 85b8871b，工作树无未提交改动；build/extc 构建于 2026-11-08 16:19。
> 真相口径：src/ 编译器源码 + build/ 下生成 C。docs/** 一律视为“待验证断言”。
> 行号口径：src/back/reclaim.c 的运行时是 C 字符串字面量，文中行号指编译器源码里该字符串所在行，不等于生成 C 的行号。
> 本文只写 docs/analysis/DOC-AUDIT.md，未改源码、tests、其他文档。

## 0. 结论速览

1. 默认路径不是单一档，而是两档：未设开关时，编译器对每个 unit 跑 unitAllAcyclic（codegen.c:14001）；证明无环则自动进“事件档”（rcNoCount=0、rcSweepFree=1，codegen.c:15604-15612），此时计数开着、清扫关着；含环或未证明无环才落回“无计数 + 区域清扫”（codegen.c:13770）。设计文档与 usage 文档全部只描述后者，这是最大缺口（见 4-G1）。
2. RECLAMATION 的“四层机制”只有“本地整片释放/提权”无条件在默认路径上；计数归零只在事件档（含自动事件档）；区域清扫在含环档。TODO.md:32 的“四层机制……在默认路径上”是错误断言。
3. RECLAMATION:11 与 :42 的“不开写屏障、不做原子操作、无 STW”三条都与源码冲突，且与该文自己的 §三（mark-both 插入/删除屏障、registry 分代发布与自旋锁）自相矛盾：codegen.c:8709-8768 每次引用赋值都发射 storeRef 钩子；reclaim.c:383-401 有 __atomic 自旋锁与按代发布；reclaim.c:921-929 有原子参与者计数；reclaim.c:420/2063/2089 有 _Thread_local。准确说法是：没有卡片/分代式写屏障，没有并发 GC 线程与全堆 STW；但有赋值钩子、有原子发布与 TLS，且 releaseOld/regionFree 有 O(活集/区域) 的同步工作（RECLAMATION:771 自认 releaseOld 的标记预算为无穷）。
4. 文档数字是三套互不一致的历史口径：同一 800 万 i64 实验写作 132.8 MB/70 ms（MEMORY-MODEL:43）、129.5 MB/57 ms（changelog/2026-11.md:1537-1540）、126 MB vs 62 MB（ALLOCATORS:117）、129 MB→70 MB（ALLOCATORS:170）、64 MB（MEMORY-MODEL:121）；churn 实验写作 11.8/2.6（MEMORY-MODEL:46,64）、2.4/103.8（ALLOCATORS:54-55）、11.86/2.4（changelog/2026-11.md:1449,1537-1540）。除 86 174 829 Ir（RECLAMATION:830 与 tests/reclaim-perf/gate.sh:88 对得上）外，本文数字均未在本次审计中重跑。
5. ALLOCATORS.md 同页自相矛盾（L3 “当前口径” vs L5 “plan，Nothing here is implemented yet”），且 L49 的 extc_pool_zoneReset 在全仓不存在（仅该文档出现）。它自述“计划”的 zone::alloc/epoch/reset 也没有实现（stdlib/std/ 下无 zone.extc，grep zone:: 无实现）。
6. DESTRUCTORS.md:57 “程序自己 raise 的 trap 会跑钩子”是错误断言：ST_TRAP 只发 extc_trapMsg+extc_die，不发射任何 drop（codegen.c:8496-8525，实测见附录 B）。反过来 L9-10 “program exit for globals 未做”已过时：main 序言注册 atexit(extc_drop_globals)（codegen.c:9810-9813、13543-13555），全局 @drop 在 trap/exit 时确实跑。
7. POOL-LEVELS.md 内部矛盾：L58 说 O2 是 open gap G2，L91 说 G2 closed（日期写成 2026-11-82，非法）；L75 的 G1 closed 日期 2026-11-81 同样是笔误。L135 “整池粒度的回收（设计，未实现）”与 L166 “Whole-pool collection…已可用”并存：实际已实现的是“按调用者契约整池释放”（pools.c:53 的 zoneFreeRoot + coroutine.c:78-95），而基于根链的可达性整池回收确实未实现；且 extc_task_rootPush 无任何 emitter 调用（coroutine.c:67-95），所以 L166 的 “freeing what no handle reaches” 字面不成立。
8. ARRAY.md 标题写“当前口径”，正文 L5 又说“属于有什么待做”；实际只有 rank-1 降低落地（check_stmt.c:2815 arrayAssignLower），array<T>/view1/view2/array2 在 src/ 中不存在（codegen.c:10357 注释直言 array<point> do not exist yet）。
9. usage/reclamation.md:142 “线程还没做”与 PARALLEL.md:136-149（par 已实现，运行时起 pthread、原子领块、线程本地 arena，codegen.c:14693/14752/14796-14839）冲突；准确说法是“用户可见的 spawn/join 与跨线程回收规则未做，par 内部线程已做”。
10. 最大文档缺口：自动事件档（unitAllAcyclic + unitReclaimIdle 的编译器自判）没有任何设计文档描述；RECLAMATION:916 与 tests/reclaim-acyclic/gate-acyclic.sh:2,5,35 仍把它写成“默认关的诊断档”。

### 0.1 默认档速查（实测 + 源码，Lead 可直接放进技术文档）

| 条件（环境变量未设时由分析决定） | EXTC_RC_NOCOUNT | EXTC_RC_SWEEP_FREE | 实际生效机制 |
|---|---|---|---|
| 未设，unit 被 unitAllAcyclic 证明无环 | 0 | 1 | 计数归零当场回收；shouldSweep 恒 0，不扫 |
| 未设，含环或未证明无环 | 1 | 0 | 无计数；区域清扫（mark-both + sweep） |
| EXTC_RC_SWEEPFREE=1（强制事件档，红证明用） | 0 | 1 | 同第一行，但强制 |
| EXTC_RC_SWEEPFREE=0（强制关事件档） | 1 | 0 | 同第二行，但强制 |
| EXTC_RECLAIM=0 | 不发射运行时 | 不发射运行时 | new 走 extc_arena_* 纯 bump；不是纯 GC 税对照 |

证据：codegen.c:13762-13770（默认值）、13796-13813（三态：未设=0 由分析定；环境值 0→-1 强制关；1→1 强制开）、15604-15612（自动接线）、14001（unitAllAcyclic）；reclaim.c:2914/2916（发射宏）、2652（SWEEP_FREE 下 shouldSweep 恒 0）；生成 C 实测见附录 B。

## 1. 一致性总表

| 文档 | 行数 | 审计条数 | 准确 | 过时 | 错误 | 计划未实现 | 含糊 | 未验证 | 主要偏差 |
|---|---|---|---|---|---|---|---|---|---|
| docs/design/MEMORY-MODEL.md | 129 | 15 | 2 | 4 | 3 | 0 | 1 | 5 | 缺 rc slab 层；“没有 GC/没有写屏障/没有共享堆”三处定性错误；数字过时 |
| docs/design/RECLAMATION.md | 1004 | 18 | 8 | 3 | 3 | 0 | 3 | 1 | 无写屏障/无原子/无 STW 三连错；四层默认路径含糊；缺自动事件档 |
| docs/design/ALLOCATORS.md | 200 | 10 | 3 | 2 | 2 | 2 | 1 | 0 | 状态自相矛盾；zoneReset 不存在；zone:: API 未实现 |
| docs/design/POOL-LEVELS.md | 281 | 9 | 3 | 0 | 4 | 1 | 1 | 0 | G2 open/closed 矛盾；日期笔误；整池回收“未实现/已可用”并存 |
| docs/design/DESTRUCTORS.md | 164 | 11 | 3 | 4 | 1 | 1 | 2 | 0 | trap drop 断言错误；globals 退出 drop 已实现；tests 清单过时 |
| docs/usage/reclamation.md | 188 | 9 | 3 | 3 | 0 | 0 | 1 | 2 | new 经 slab；默认档缺事件档；“线程还没做”过时 |
| docs/design/ARRAY.md（内存部分） | 204 | 6 | 1 | 0 | 1 | 4 | 0 | 0 | “当前口径”与“待做”矛盾；array<T>/view2 未实现 |
| docs/design/PARALLEL.md | 189 | 4 | 3 | 0 | 0 | 1 | 0 | 0 | 与 usage 的“线程还没做”冲突 |
| docs/plans/TODO.md | 276 | 8 | 3 | 0 | 2 | 1 | 0 | 2 | 四层默认路径错误；非法日期；删计数层影响面未验证 |
| 合计 | — | 90 | 29 | 16 | 16 | 10 | 9 | 10 | 见第 3 节 |

## 2. 逐条审计表

### 2.1 docs/design/MEMORY-MODEL.md（15 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| MEM-1 | :5 | extC 没有 free，也没有垃圾回收器 | 错误 | codegen.c:15609-15612；reclaim.c:2531（sweep 段）、2914/2916 | 改为“没有用户可见的 free；有一个按区域、增量、可按 unit 整段不发射的标记-清扫回收器；默认随无环证明在计数档与清扫档之间切换” |
| MEM-2 | :13 | 帧……分配只是挪指针 | 过时 | codegen.c:4618（new T 在 needReclaim 时走 rcAllocCore）、4633（仅 !needReclaim 才 extc_arena_alloc）、4653/4670（new T[n] 仅 level==1 走 rc 区） | 补“回收区 slab”一类；说明 new T 默认从 rc slab 出、new T[n] 仅函数体层走 rc，内层块仍是 arena bump |
| MEM-3 | :14 | 板块……容器换缓冲/缩小时当场归还 | 准确 | pools.c:56-63（block chain malloc/free），pools.c:58（pool reset 语义） | 补一句“整池归还给 malloc，运行时不 munmap，RSS 不保证立刻下降” |
| MEM-4 | :15 | 对象表……槽可回收；回收后旧句柄读到的是没有 | 准确 | pools.c:58（APPEND-ONLY + tombstone）、pools.c:119-123（dyn slot 的 gen/pgen 与 extc_dynSweep） | 补“池记录表/槽表本身是进程级 realloc 数组，不随 place 归还” |
| MEM-5 | :19 | 不记账、不扫描、退出不遍历 | 含糊 | codegen.c:12130（extc_arena_release 逐块 free）、12132（destroy 再 free spare） | 改成“不记账、不逐对象扫描；退出时按块链整块释放，仍要遍历块链，代价 O(块数)” |
| MEM-6 | :43-44 | 132.8 MB / 70 ms；vector 64 MB / 28 ms | 过时 | changelog/2026-11.md:1537-1540（帧里翻倍 129.5 MB / 57 ms）、:1426（132.8 MB 是另一种形状）；docs/design/TYPE-MODEL.md:124-125（varArray 130 MB、buf 64 MB） | 同一实验选一个口径重测并注明日期/档位；不要把 132.8 与 129.5 混用 |
| MEM-7 | :46,64 | 板块 2.5 MB；帧里换缓冲 11.8 MB；循环体 2.6 MB；关回收 103.8 MB | 过时 | changelog/2026-11.md:1449（103.8→2.4）、:1506、:1537-1540（循环体 2.4）；ALLOCATORS:54-55（2.4） | 用当前构建重测；明确“默认档/事件档/关回收”与“变量寿命是否跨循环” |
| MEM-8 | :71 | snapshot 拷贝二十万轮 2.6 MB | 未验证 | 未在本次审计复现，源码注释/changelog 未找到同口径 | 标历史实测并给出处，或删除 |
| MEM-9 | :77-78 | 两百万次新位置 1.8 MB；长命位置一百万次 82 MB | 未验证 | 同上 | 同上 |
| MEM-10 | :94 | 没有经典意义上的碎片，因为没有共享大堆 | 错误 | reclaim.c:790-792（slab calloc/malloc）、2004-2007（regionFree 逐个 free 还给 malloc）、pools.c:50-63（全局池表 realloc）、reclaim.c:383-401（全局 registry） | 改为“有共享的 C 堆，但按尺寸类/区域批量申请与整批归还，对象地址固定因此不需要 compaction；malloc 层碎片仍可能存在” |
| MEM-11 | :99-100 | vector 1.5x、hashMap 墓碑、对象表槽不缩 | 未验证 | 未逐项核对 stdlib 增长因子；本审计只确认对象表复用存在（pools.c:58） | 逐项给出 stdlib 文件:行，或标为设计目标 |
| MEM-12 | :102 | 稳态 2.5 / 1.5 / 5.6 MB | 未验证 | 无当前构建复现 | 标历史实测或删除 |
| MEM-13 | :108 | 好处是分配免费、没有写屏障、没有停顿 | 错误 | codegen.c:8709-8768（每次引用赋值发射 storeRef*）；reclaim.c:1495-1588（storeRef 家族）、195（mark-both 插入+删除屏障） | 改成“分配多半是 bump/slab 快路径；没有卡片/分代写屏障，但有赋值钩子（跨区登记+旧值着色）；没有并发 GC 线程与全堆 STW，但有增量清扫小步与释放点同步工作” |
| MEM-14 | :121 | @poolObject 公开；手写 800 万 i64 峰值 64 MB | 过时 | parser.c:861-872（@place 与 @poolObject 都接受，后者是旧拼写） | 改称 @place；64 MB 数字与 :43 的 132.8 冲突，需重测统一 |
| MEM-15 | :123 | 没有世代守卫……不开 ASan 时打印完全正常 | 未验证 | 未做该实验 | 标未验证或补一个可复现实验 |

### 2.2 docs/design/RECLAMATION.md（18 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| REC-1 | :3 | 状态：当前口径 | 过时 | 见 REC-10/REC-17；自动事件档未写 | 保持“当前口径”可以，但必须先补 0.1 的默认档表 |
| REC-2 | :11 | 不开写屏障 | 错误 | codegen.c:8709-8768；reclaim.c:1495/1513/1520/1588/1791；与本文件 :195“插入+删除两条屏障”自相矛盾 | 改为“没有卡片/分代式写屏障；有引用赋值的 storeRef 钩子与 mark-both 的插入/删除着色” |
| REC-3 | :11 | 不做原子操作 | 错误 | reclaim.c:383-401（regLock 自旋锁、__atomic_store_n、regSeq）、921-929（extc_rc_wbAdd 原子加减）、420/2063/2089（_Thread_local）；codegen.c:14752（par 原子领块） | 改为“单线程 mutator 语义；运行时含为并行预留的原子发布、自旋锁与 TLS 记账” |
| REC-4 | :11 | 不搬对象 | 准确 | codegen.c:4604（arenaRefAt(planArenaLevel(e))）、4618/4628（直接在目标层 rc slab 分配） | 保留；可补一句“对象地址从分配起不变” |
| REC-5 | :33 | 提权：把那一份存储挪到外层区域；一次指针操作，地址不变 | 含糊 | 同上；编译器在分配点按静态层级直接分配（planArenaLevel），不存在先分配再搬的动作 | 改为“分配点直接落在静态提升层；不发生复制或搬移，地址从一开始就固定” |
| REC-6 | :30-39 | 四层……前三层处理掉绝大多数 | 含糊 | codegen.c:13770（默认无计数）、13806-13813、15609-15612；见 0.1 表 | 按档拆写：“本地/提权无条件；计数归零仅在事件档；清扫仅在含环档” |
| REC-7 | :41 | 任一区域驻留垃圾不超过存活量+阈值 | 含糊 | reclaim.c:143-151（SCALE_MAX=1<<40、SCALE_MUL=4）、2601-2604（空扫乘 4）、2637-2641（thr=max(2*live,floor)*scale） | 写明公式与上限；指出 scale 上限极大，该界在极端情况下退化为“可涨到极大” |
| REC-8 | :42 | 停顿只与被扫区域有关；无 STW | 错误 | 本文件 :771 自认 releaseOld 标记预算为无穷 O(活集)；reclaim.c:2519（storeRefOwnedRelease）、1970-2007（regionFree 同步释放整个区域） | 改为“无并发 GC 线程、无全堆 STW；单次同步工作可能为 O(被归还区域) 或 O(活集)（定点释放）”；把 releaseOld 列为已知无界项 |
| REC-9 | :56-66 | slab 只增不减；唯一块级归还=区域整个归还 | 准确 | reclaim.c:316-322（SLAB_SIZE/CHUNK_MIN/SLOTS_MIN）、779-784（64KiB 倍增到 1024 槽或 1MiB）、2004-2007（regionFree 归还 malloc） | 补“归还 malloc 但无 munmap，RSS 是水位的，不保证下降” |
| REC-10 | :164-170 | 二、计数归零（默认关闭；EXTC_RC_NOCOUNT=0 打开）……默认只靠区域清扫 | 过时 | codegen.c:15604-15612（无环 unit 自动 rcNoCount=0）；13770 | 先给 0.1 表，再写“计数档的细节”；否则读者会以为计数永远不在默认路径 |
| REC-11 | :195-204 | 增量标记用插入+删除两条屏障（mark-both） | 准确 | reclaim.c:1454（shade）、1471/1480/1497/1516；codegen.c:8709-8768；tools/check_mark_both.py 存在 | 保留 |
| REC-12 | :205-221 | shade cache 2048 格，2^11 代，回绕 memset 16KiB | 准确 | reclaim.c:1395-1407（2048、mask 2047、extc_rc_shadeNextGen） | 保留 |
| REC-13 | :264-271 | 没有环的程序永远不会进入清扫 | 准确 | reclaim.c:2652（EXTC_RC_SWEEP_FREE 下 shouldSweep 恒 0）；codegen.c:15609 | 补“含自动事件档与手工强制两路” |
| REC-14 | :483-487 | EXTC_RECLAIM=1 只是允许；=0 连分配器换成纯 bump | 准确 | codegen.c:12395（发射条件）、4618/4633、14062（needReclaim 可被静态 idle 证明关掉） | 保留；可补“整 unit 被证明 idle 时回收运行期根本不发射” |
| REC-15 | :830 | 指令数棘轮 86 174 829 Ir | 准确 | tests/reclaim-perf/gate.sh:88（阈值 90 483 570 = ×1.05）、changelog/2026-10.md:1745-1751 | 保留 |
| REC-16 | :850 | 量具 PAUSE_DIST 恒 n=0，不能断言 0 次停顿 | 准确 | 该文自述；:564 同 | 保留 |
| REC-17 | :916 | EXTC_RC_SWEEPFREE=1（事件档）……诊断档：实测它在语料里没有一个能赢的形状 | 过时 | codegen.c:13806-13813、15609-15612（未设时自动开）；tests/reclaim-acyclic/gate-acyclic.sh:2,5,35 仍写“默认关/分析侧落地后换输入” | 改为“默认由 unitAllAcyclic 决定；=1 强制，=0 强制关；gate 头注释需同步更新” |
| REC-18 | :1002-1004 | 单测两档：默认无计数 128 条、计数档 171 条 | 未验证 | 本次未跑 tests/reclaim/driver.c | 重跑后给命令与输出，或标历史 |

### 2.3 docs/design/ALLOCATORS.md（10 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| ALLOC-1 | :3,5 | 状态：当前口径……Status: plan. Nothing here is implemented yet. | 错误 | 同页矛盾；L154-156 与 L188-200 又描述已落地的 borrow 守卫与 load-bearing shortcut | 拆成两节：现状（pools.c/@place/dyn/plate）与计划（zone API）；删除“当前口径”或“Nothing implemented”之一 |
| ALLOC-2 | :9 | A zone is the compiler只有 allocator-facing concept | 计划未实现 | 实际存在 pools.c:48-104 的 pool/zone 运行时、extc_pool_*、@place；codegen.c:6004 用 calleeCreatesPool | 标为“目标模型”；现状描述改到 pools.c 的 pool/zone 运行时 |
| ALLOC-3 | :21-22 | 若三件事存在，@place、poolSlice、poolResize、std::sys::pool 与 pools.c block-chain 全冗余 | 计划未实现 | stdlib/std/ 无 zone.extc；仓库无 zone::alloc/epoch/reset 实现；poolSlice/poolSliceRaw 仍出现在 bench（bench/zero/probe_raw.extc:10） | 明确“这是迁移完成后的判断，今天不成立”；删除“全冗余”现在时 |
| ALLOC-4 | :31-32 | mem::plate 4GiB/1MiB；block chain 是 malloc 单链 | 准确 | stdlib/std/mem.extc:73（DEFAULT_RESERVE=4294967296）、:92-95（mmap）、:112-128（mprotect 1MiB chunk）；pools.c:61-63 | 保留 |
| ALLOC-5 | :33 | privileged heap emission：extc_viewOf / extc_memCopy | 含糊 | src/back/plate.c 仅 74 行：extc_viewOf 约 :24-39、extc_memCopy :53-74；是转门 helper，不是分配器 | 改称“为语言拼不出的代码发射的 helper，不拥有堆” |
| ALLOC-6 | :49 | extc_pool_zoneEnter / extc_pool_zoneLeaveTo / extc_pool_zoneReset already give…… | 错误 | 全仓 grep extc_pool_zoneReset 只命中该文档；实现有 zoneEnter/zoneLeaveTo/zoneNewRoot/zoneFreeRoot/zoneOuter/zoneDepth（pools.c:53）、extc_pool_reset(rid)（pools.c:58，语义为保留池+槽复用+generation++） | 删 zoneReset；改列实际符号与语义 |
| ALLOC-7 | :52-56 | new inside a loop accumulates……peaks at 103.8 MB；loop body 2.4 MB | 过时 | changelog/2026-11.md:1449/1506；MEMORY-MODEL:46/64 用 11.8/2.6；条件与档位不同 | 同一实验统一一处数字并注明档位/变量寿命 |
| ALLOC-8 | :117,170 | vector 62 MB vs varArray 126 MB；129 MB→≈70 MB（prototype） | 过时 | MEMORY-MODEL:43-44（132.8/64）、:121（64）；TYPE-MODEL:124-125（130/64）；changelog:1537-1540（129.5/57） | 重测；把“prototype 数字”与“当前构建数字”分开 |
| ALLOC-9 | :154-167 | 已经能跑的那一半（2026-11-04）：buf::borrow @noCopy+@drop 守卫、trap frozen by a live borrow、tests/pin | 准确 | tests/pin 下 borrow_freezes/borrow_release/vector_frozen/string_frozen 等存在；stdlib/std/data/buf.extc:89 有该 trap 文案 | 保留；补“编译器静态检查只覆盖同函数体”的边界 |
| ALLOC-10 | :186-200 | 两条 shortcut 今天是 load-bearing：calleeMakesPool / calleeCreatesPool | 准确 | check_top.c:1906（calleeMakesPool）、10550（calleeCreatesPool）、codegen.c:6004；ast.h:1483-1489 记“Known divergence, not yet fixed” | 保留，并引用 ast.h 的分歧：checker 判据比 funcTakesHomeZone 宽，可能提权却没发隐藏实参 |

### 2.4 docs/design/POOL-LEVELS.md（9 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| POOL-1 | :3 | 状态：当前口径 | 含糊 | 同文 :135“设计，未实现”与 :166“已可用”并存；规则 S/R 与 witness 已实现，整池可达性回收未实现 | 分三块标注：已实现（规则/见证/契约式整池释放）、部分实现（规则 R 三类 carrier）、未实现（根链整池标记） |
| POOL-2 | :58 vs :91 | O2……(open gap G2) vs G2 — closed | 错误 | 同文两处矛盾；G2 的别名/读侧证据未在源码中复核 | 二选一：若 L91 为准，删 L58 的 open；并给出关闭的判据/测试 |
| POOL-3 | :75,91 | G1 closed (2026-11-81)；G2 closed (2026-11-82) | 错误 | 日期 2026-11-81/82 不存在（月份 81/82） | 改为真实日期 |
| POOL-4 | :105-113 | No accepted program reads freed pool storage……Both named gaps are closed | 错误 | 与 :58 冲突；源码侧只能确认 witness/测试存在，不能从 witness 推出 soundness | 与 POOL-2 同步改；把“Both closed”改为“按现有 witness 未观察到差异” |
| POOL-5 | :135-164 | 整池粒度的回收（设计，未实现）：根链 EXTC_ROOT_TASK/POOL、经 parent/firstChild 标记、trigger 挂 maybeCollect/pool_take | 计划未实现 | reclaim.c:2051（no emitter pushes onto the chain yet）、2064（extc_rc_framePush 标 unused）；但 pools.c:53 的 zoneFreeRoot 与 coroutine.c:78-95 的 collect 是简化版整池释放 | 区分“契约式整池释放已实现”与“根链标记式自动回收未实现” |
| POOL-6 | :166 | Whole-pool collection: freeing what no handle reaches | 错误 | coroutine.c:67-95：根表 extc_task_rootPush 只有定义，无 emitter 调用；collect 只看 zone 表 depth==0，不做可达性计算 | 改为“按调用者契约释放 depth==0 的 task zone；不做可达性判断” |
| POOL-7 | :196-200 | 契约是调用者的……Making the call explicit keeps the failure mode a documented assertion | 准确 | coroutine.c:78-95 与 stdlib/std/sys/pool.extc:34-44 的契约一致 | 保留；补“根注册 API 未接线” |
| POOL-8 | :120,128 | 1,500 个 .extc 真实差异 0 条；基线空文件 | 准确 | tools/check_lvl_witness.py 与 tools/lvl-witness-baseline.txt（0 行）；本次实测语料 1501 个 | 保留；把“1,500”改成“约 1,500（当前 1501）” |
| POOL-9 | :202-221 | 仍 per-site 的 promoteInto2 手写 switch vs check_lvl 不动点 | 准确 | check_escape.c:1237（lowerPoolSite）、1738（promoteInto2）；check_lvl.c 存在且 EXTC_DBG_LVL 门控 | 保留 |

### 2.5 docs/design/DESTRUCTORS.md（11 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| DEST-1 | :5-10 | Step S1/S2 in……Not in yet: generators' frames, drops on the trap path, and program exit for globals | 过时 | globals 已实现：codegen.c:9813 atexit(extc_drop_globals)、13543-13555 emitGlobalDrops、tests/drop/global_exit.extc；trap 路径块内 drop 确实未实现 | 删除 program exit for globals；保留 generators frames 与 trap path |
| DEST-2 | :57 | Hooks do run for a trap that the program raises (trap(...) statement) | 错误 | codegen.c:8496-8525 的 ST_TRAP 只发 extc_trapMsg + extc_die(1)，无任何 drop 发射；实测附录 B：块内 @drop 不跑 | 改为“trap 路径不跑块内钩子；只有全局 @drop 经 atexit 会跑”（与 :59 分开写清） |
| DEST-3 | :59 | 全局 @drop 在 process exit；traps/exit() 也走它 | 准确 | codegen.c:9810-9813（先注册 cout_flush，后注册 drop_globals，atexit LIFO 先钩子后 flush）、13543-13555；实测 trapdrop2 输出 drop 9 | 保留 |
| DEST-4 | :61-63 | 字段随类型死，先 body 再字段逆序 | 准确 | genDropValue codegen.c:6655；tests/drop/fields.extc；L152-154 的“field order 仍缺”应删 | 保留，并删掉 L152-154 的过时项 |
| DEST-5 | :93 | Not in yet: drop on a field | 准确 | 源码未见 drop self.inner 实现；拒绝表 L99-108 仍在 | 保留 |
| DEST-6 | :106 | frame drop list not implemented yet (stage 2) | 含糊 | 与 L155-156 Stage 3 的编号冲突；yield 相关检查未在源码中找到 | 统一 stage 编号，明确“跨 yield 拒绝是否已实现/仅计划” |
| DEST-7 | :120 | Trap-path drops: a trap records the enclosing blocks' drop lists | 计划未实现 | 与 DEST-2 同；ST_TRAP 无此逻辑 | 标“未实现（计划）” |
| DEST-8 | :131 | Runtime: none. A destructor is a plain C function call | 含糊 | 钩子本身是普通调用，但全局退出依赖 atexit(extc_drop_globals) 运行期胶水（codegen.c:9813、13543）；trap 路径还依赖 extc_die/__extc_dying（codegen.c:12088-12092） | 改为“钩子体是普通 C 调用；全局退出与 trap 退出由运行期 atexit/die 胶水负责” |
| DEST-9 | :133-146 | Tests (tests/destructor/) 12 项，含 trap_path.extc | 过时 | tests/destructor/ 目录不存在；实际 tests/drop/ 13 个 .extc + run.sh，errors/ 6 个反例；无 trap_path.extc | 重写测试清单为 tests/drop 的真实文件 |
| DEST-10 | :150 | Stage 1 (landed, tests/drop/ 7 cases) | 过时 | tests/drop/ 现有 13 个 .extc（含 early_return、break_continue、fields、global_exit 等）+ 6 反例 | 改为现数 |
| DEST-11 | :152-154 | Still open in S1: break/continue、program end、trap path、field order、borrow census | 过时 | break/continue：codegen.c:8891（genDropBlocksDownTo 在 cgReleaseLevel 前）+ tests/drop/break_continue.extc；program end：global_exit 测试；field order：tests/drop/fields.extc；trap path 仍缺 | 只保留 trap path（与 borrow census，若仍缺则保留） |

### 2.6 docs/usage/reclamation.md（9 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| USE-1 | :3 | 没有 free，也没有那种停下来把整个堆扫一遍的垃圾回收 | 准确 | reclaim.c:2531（按词增量扫描）、888-889（SWEEP_BUDGET 8）；无全堆 STW 路径 | 保留；可补“有按区域的增量 mark-sweep，默认随无环证明切换” |
| USE-2 | :13 | new 从当前块的这片内存里往下发 | 过时 | codegen.c:4618/4633/4653/4670（默认回收开时 new T 走 rc slab；new T[n] 仅 level==1 走 rc） | 改为“非回收档是 arena bump；默认档 new 从回收区 slab 分配，内层 new T[n] 仍走 arena” |
| USE-3 | :87 | 峰值 2.8 MB、墙钟中位 5.0 ms（2055 片，最慢 9.6 µs）；关回收 17.0 MB / 6.6 ms | 未验证 | 本次未跑 bench/gc/v2/run.sh；changelog/2026-11.md:2038 有停顿中位 0.70–1.40 µs / p99≤2 µs 的相近记录 | 重跑后更新；标注构建日期与机器 |
| USE-4 | :100 | 前三层处理掉绝大多数 | 含糊 | 同 REC-6；默认档计数关 | 按 0.1 表分档说明 |
| USE-5 | :102 | 默认口径：不做持有者计数 | 过时 | codegen.c:15609-15612（无环 unit 自动开计数） | 加一句“例外：unit 被证明无环时自动进事件档（计数开、清扫关）” |
| USE-6 | :123 | 没有环的程序永远不会进入清扫 | 准确 | reclaim.c:2652 + codegen.c:15609（事件档 shouldSweep=0） | 保留 |
| USE-7 | :132 | 单次停顿中位 0.70–1.40 µs、p99≤2 µs……2055 片没有一片超过 16 µs | 未验证 | 未重跑；且 RECLAMATION:564/850 自认 PAUSE_DIST 恒 n=0，需说明该数字来自 [step]/[pause] 而非 PAUSE_DIST | 注明量具与构建；不要用 PAUSE_DIST 口径作证 |
| USE-8 | :142 | 线程还没做：区域与根表的账都是按单线程算的 | 过时 | par 运行时每次调用起 pthread（codegen.c:14796-14826）、线程本地 arena（:14693）、原子领块（:14752）；PARALLEL.md:136-149 同 | 改为“用户可见的 spawn/join 与跨线程区域/根表记账未做；par 的内部线程已实现” |
| USE-9 | :166 | 回收关掉同时把分配器换成纯 bump，不是纯 GC 税对照 | 准确 | codegen.c:4633（!needReclaim 即 extc_arena_alloc）；RECLAMATION:483-487 一致 | 保留 |

### 2.7 docs/design/ARRAY.md（内存相关，6 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| ARR-1 | :3 vs :5 | 状态：当前口径 vs 属于有什么待做那一类 | 错误 | 同文自相矛盾；实际只落地 rank-1 子集（:70-73 自述） | 把标题改成“目标模型 + 已落地子集” |
| ARR-2 | :9-11 | 数组只保留 owner(array/array2) 与 view(view1/view2)；不分配临时数组、不引入运行时 | 计划未实现 | src/ 无 array2/view1/view2 关键字；只有 rank-1 降低（check_stmt.c:2815）；codegen.c:10357 注释 array<point> that do not exist yet | 标“设计目标”；实现状态只写 rank-1 降低 |
| ARR-3 | :24 | array<T>：运行期长度，唯一主人，arena 分配 | 计划未实现 | 仓库无 array<T> 类型；TY_ARRAY 只表示定长 [N]T（codegen.c:918 附近）；grep array< 仅注释 | 标未实现；说明今天用 slice<T>/[N]T |
| ARR-4 | :70-73 | 已落地的是 rank-1 子集（check_stmt.c 的 arrayAssignLower） | 准确 | check_stmt.c:2815（arrayAssignLower）、:3847（调用） | 保留 |
| ARR-5 | :85-90 | 任何整数组表达式不分配堆内存……生成 C 只多 double 循环 | 计划未实现 | 契约只对 rank-1 子集成立（check_stmt.c:2815）；比较/%/ndview 多下标未做（:70-73 自述） | 标“契约目标的已实现部分” |
| ARR-6 | :101-112 | 分阶段落地：view2 内建、整数组表达式、三维 | 计划未实现 | 无对应源码 | 标计划；给依赖与判据 |

### 2.8 docs/design/PARALLEL.md（4 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| PAR-1 | :15 | 可变数据有唯一主人，能共享的只有只读。引用不过线程 | 准确 | 检查器限制集见 :54-87；par 实现 codegen.c:14661-14840 | 保留 |
| PAR-2 | :47-48 | par/ext 两动词，不引入用户可见线程/锁/原子 | 准确 | 用户侧只有 par 语法；运行期内部用 pthread/原子（codegen.c:14752） | 保留，补“内部实现用了原子，语言层不暴露” |
| PAR-3 | :136-149 | 状态：par 起线程、原子计数器动态领块、线程本地 arena、EXTC_PAR_THREADS | 准确 | codegen.c:14693（_Thread_local extc_tls_arena）、14752（__atomic_fetch_add）、14796-14826（pthread_create/join）、14832-14839（默认线程数） | 保留 |
| PAR-4 | :153-182 | 线程与回收（暂缓，规则已定）：spawn/借出块冻结/池化线程 arena | 计划未实现 | 无用户可见 spawn/join；无 arena users 记账；PARALLEL 自述暂缓 | 保留；与 usage:142 的“线程还没做”交叉引用，避免读者误解 |

### 2.9 docs/plans/TODO.md（8 条）

| 编号 | 文档:行 | 原文摘录（节选） | 判定 | 源码证据 | 建议修正 |
|---|---|---|---|---|---|
| TODO-1 | :11-15 | 计数层（推迟，用户决定 2026-11-05）……删掉计数档会影响事件档与编译器自判无环⇒开计数+不扫两条路径 | 准确 | 与 codegen.c:15609-15612 一致；是计划陈述 | 保留；可在 RECLAMATION 加“计划删除计数层”的指针 |
| TODO-2 | :14 | 净删 103 行，生成 C 逐字节不变 | 未验证 | 未按 TRIM.md 执行删除，也未比对生成 C | 标未验证；删除前应先跑逐字节 diff |
| TODO-3 | :32 | 四层机制（本地/提权/计数归零/区域清扫）在默认路径上 | 错误 | codegen.c:13770（默认无计数）、13806-13813、15609-15612 | 改为“本地/提权恒在；计数归零仅在事件档（含自动事件档）；清扫仅在含环档” |
| TODO-4 | :65 | 打开计数比清扫贵 +100%~+455%；计数价仍是清扫侧 2.5–17 倍 | 未验证 | 来源 TAXMAP 第 9 节；本次未跑 tests/reclaim-perf | 标历史实测/重跑后更新 |
| TODO-5 | :137 | 选定的档会出现 PAUSE_DIST n=0，同一次运行 [pause]/[step] 计数不为 0 | 准确 | 与 RECLAMATION:564/850 一致 | 保留 |
| TODO-6 | :232 | 栈帧协程……（2026-11-76） | 错误 | 2026-11-76 不是合法日期（日 76） | 改真实日期 |
| TODO-7 | :240 | 协程任务整池粒度进回收方向：ExtcPool 链、framePush 根链、zoneNewRoot/zoneFreeRoot | 计划未实现/部分 | zoneNewRoot/zoneFreeRoot 已有（pools.c:53）；framePush 根链未接线（reclaim.c:2051/2064） | 标“已实现简化契约版；根链自动标记未实现” |
| TODO-8 | :256 | syspool::collect 已交付；逃逸句柄 after=1→0 | 准确 | stdlib/std/sys/pool.extc:38-44、coroutine.c:78-95；限制是契约式、无根注册 | 保留，补契约限制 |

## 3. 必须修正的 10 条（按严重度排序）

1. 默认档写错（影响全部技术文档的结论）。所有文档只写“默认不计数、只靠清扫”，漏掉“unit 被证明无环时自动开计数+关清扫”。
   证据：codegen.c:13770、13806-13813、14001、15604-15612；实测附录 B。
   改法：把 0.1 的表原样放进 MEMORY-MODEL、RECLAMATION §二开头、usage §三开头；所有“默认路径”句子按档限定。

2. “无写屏障/无原子/无 STW”三个定性错误（RECLAMATION:11,42；MEMORY-MODEL:108；usage 隐含）。
   证据：codegen.c:8709-8768；reclaim.c:383-401、921-929、420/2063/2089、195-204；RECLAMATION:771。
   改法：统一替换为“无卡片/分代写屏障；有赋值钩子与 mark-both 着色；单线程 mutator，但有原子发布/自旋锁/TLS；无并发 GC 线程与全堆 STW，但有 O(区域/活集) 的同步释放点”。

3. MEMORY-MODEL 的“三种存储”漏掉回收区 slab，且对 new 的描述过时（:13,:15）。
   证据：codegen.c:4618/4633/4653/4670；reclaim.c:1055（extc_rc_alloc）、1970-2007。
   改法：改为四类：帧 arena（bump）、回收区 slab（默认 new T）、容器板块（pool block chain）、dyn 对象表；并写清 new T 与 new T[n] 的分配差异。

4. “内存有界/无碎片”的表述不严谨（MEMORY-MODEL:94；RECLAMATION:41-42）。
   证据：slab 从 malloc 来、regionFree 只还 malloc 不 munmap（reclaim.c:2004-2007 无 munmap）；registry 旧代永不 free（reclaim.c:385-386,445-446）；阈值可乘到 SCALE_MAX=1<<40（reclaim.c:143-151）。
   改法：把“有界”限定为“每个区域驻留垃圾 ≤ 阈值（阈值= max(2*live,floor)*scale，scale 上限 2^40）”；把“无碎片”改为“对象不移动、无需 compaction；malloc 层碎片与 RSS 水位仍存在”。

5. 数字三套口径未统一（MEMORY-MODEL:43-46,64,71,77-78,102；ALLOCATORS:52-56,117,170；usage:87,132）。
   证据：changelog/2026-11.md:1412/1426/1449/1506/1537-1540/2038；TYPE-MODEL:124-125；gate.sh:88。
   改法：建立“数字-构建日期-档位-量具”四列的唯一表；未重跑的标历史；删除或标注互相冲突的副本。

6. ALLOCATORS 状态与 zoneReset 错误（:3,:5,:49）。
   证据：同页矛盾；extc_pool_zoneReset 全仓零命中；实际 zone 符号 pools.c:53。
   改法：状态拆“现状/计划”；删除 zoneReset 或换成实际符号（zoneEnter/zoneLeaveTo/zoneNewRoot/zoneFreeRoot/zoneOuter/zoneDepth + extc_pool_reset(rid)）。

7. DESTRUCTORS 的 trap 断言错误、globals 断言过时（:57,:9-10）。
   证据：ST_TRAP codegen.c:8496-8525 无 drop；atexit(extc_drop_globals) codegen.c:9810-9813、13543-13555；实测附录 B。
   改法：“trap 不跑块内钩子；全局 @drop 经 atexit 会跑”；更新 S1/S2 状态与 tests/drop 清单。

8. POOL-LEVELS 的 G2 矛盾、日期笔误、整池回收表述（:58,:75,:91,:135,:166）。
   证据：同文矛盾；coroutine.c:67-95 根表无 emitter 调用；pools.c:53 zoneFreeRoot 已实现。
   改法：G2 只留一个状态；日期改真；拆“契约式整池释放（已实现）”与“根链标记整池回收（未实现）”；L166 标题改为“按调用者契约整池释放”。

9. usage 的“线程还没做”与 new 描述过时（:13,:142）。
   证据：codegen.c:14693/14752/14796-14839；PARALLEL.md:136-149。
   改法：“用户可见 spawn/join 与跨线程回收未做；par 内部线程已做；new 在默认档走 rc slab”。

10. ARRAY 的“当前口径”与 array<T> 未实现（:3,:5,:24）。
    证据：check_stmt.c:2815 只 rank-1；src/ 无 array<T>/view1/view2；codegen.c:10357 注释。
    改法：标题改“目标 + 已落地 rank-1 子集”；array<T>/view2 标计划。

## 4. 文档未提及但源码存在的机制（文档缺口）

| 编号 | 机制 | 源码证据 | 应该写进哪份文档 |
|---|---|---|---|
| G1 | 自动事件档与无环/空闲分析：unitAllAcyclic、unitReclaimIdle、planFnAcyclic，决定计数/清扫档与回收运行期是否发射 | codegen.c:13962-14031、14062、15604-15612；RECLAMATION 未提 | RECLAMATION §二/§五；MEMORY-MODEL 总览 |
| G2 | storeRef 钩子家族与派发条件：OwnedReg / SameRegion / Local / Owned / Slot / initRef / initRefNoHold；以及 mark-both 的 shade/shadeOld | codegen.c:8709-8768；reclaim.c:1478-1588、1454-1520 | RECLAMATION 需要把“赋值钩子”单列一节；MEMORY-MODEL 现状 |
| G3 | 回收区 slab 分配器：extc_rc_alloc、尺寸类、槽位/位图/释放、regionFree；new 与 region 的绑定 | reclaim.c:1055、369-1000、1970-2007；codegen.c:4618-4670 | MEMORY-MODEL 的存储分类；ALLOCATORS 现状 |
| G4 | registry 分代发布：自旋锁、__atomic_store_n、regSeq 重试、旧代永不 free、旧代总量约 2 倍 | reclaim.c:383-401、444-446、535-560 | RECLAMATION 并行一节；技术文档的“有界”说明 |
| G5 | dyn 对象表的真实实现：extc_dyn_put 经 extc_pool_new_at(-1,zi) 建 kind=1 池、slot(pid,slot,gen,pgen)、tombstone 式 extc_dynSweep | pools.c:58（table 契约）、pools.c:119-125（dyn 运行时、extc_dynSweep） | MEMORY-MODEL 对象表一节 |
| G6 | pool/zone 运行时：ExtcZone 栈、zoneEnter/LeaveTo/NewRoot/FreeRoot/Depth、pool 种类（container/object table）、block chain release | pools.c:48-104（尤其 :53、:56、:58、:61-63） | ALLOCATORS 现状；POOL-LEVELS 已实现部分 |
| G7 | @drop 全局退出胶水与 trap 退出路径：atexit(extc_cout_flush) 先注册、atexit(extc_drop_globals) 后注册（LIFO 先钩子后 flush）；extc_die 只跑 __extc_dying | codegen.c:9810-9813、13543-13555、12088-12092 | DESTRUCTORS 运行期一节 |
| G8 | par 运行时机制：每次调用 pthread_create/join、原子领块、_Thread_local arena、worker 结束 regionDrop+arena_destroy、默认线程数与 EXTC_PAR_THREADS | codegen.c:14693、14752、14796-14839；stdlib/std/parallel.extc:27-35 | PARALLEL 状态段；usage 的线程说明 |
| G9 | 整 unit 静态 idle / EXTC_RECLAIM=1 只是允许：needReclaim=false、idle 证明成立时回收运行期不发射 | codegen.c:14062、14081-14112；RECLAMATION:483-487 只是暗示 | RECLAMATION 开关一节；技术文档的“零税”说明 |
| G10 | arena 的块管理细节：单链表、spare、倍增上限 1MiB、release 遍历、destroy、越界 trap | codegen.c:12119-12153；MEMORY-MODEL 只说“栈” | MEMORY-MODEL 帧一节 |
| G11 | trap 的唯一出口：extc_trapMsg→extc_die(1)→exit(code)；__extc_dying 只用于 raw 终端复位类回调 | codegen.c:12088-12092 | DESTRUCTORS/技术文档的异常路径 |
| G12 | 池/表全局数组的 realloc 增长与 free list（extc_pools、extc_dynSlots、extc_poolKind）；池记录 sizeof==104 | pools.c:50-58、119-125 | MEMORY-MODEL 对象表；技术文档的元数据成本 |

## 附录 A：未复现/存疑清单

| 条目 | 出处 | 状态 |
|---|---|---|
| 132.8 MB / 70 ms；64 MB / 28 ms | MEMORY-MODEL:43-44 | 未在当前构建重跑；与 changelog:1537-1540 的 129.5/57 冲突 |
| 11.8 / 2.6 / 103.8 MB churn | MEMORY-MODEL:46,64 | 未重跑；与 changelog:1449 的 2.4、ALLOCATORS:54-55 的 2.4 冲突 |
| snapshot 2.6 MB；dyn 1.8/82 MB；稳态 2.5/1.5/5.6 MB | MEMORY-MODEL:71,77-78,102 | 未找到同口径来源，未复现 |
| 2.8 MB / 5.0 ms / 2055 片 / 9.6 µs；17.0 MB / 6.6 ms | usage:87 | 未跑 bench/gc/v2；changelog:2038 有相近停顿数据 |
| 停顿中位 0.70–1.40 µs、p99≤2 µs、最慢 115 µs | usage:132 | 未重跑；PAUSE_DIST 恒 n=0（RECLAMATION:564/850），需注明量具 |
| +100%~+455%；2.5–17 倍 | TODO:65 | 未跑 TAXMAP/gate；标历史 |
| 62/126 MB；129→70 MB | ALLOCATORS:117,170 | 未复现；与 MEMORY/TYPE-MODEL 冲突 |
| 净删 103 行、生成 C 逐字节不变 | TODO:14 | 未执行删除，未做逐字节 diff |
| 单测 128/171 条 | RECLAMATION:1002-1004 | 未跑 driver |
| 800 万 i64 手写 64 MB 与 vector 相同 | MEMORY-MODEL:121 | 未复现 |
| tests/drop 的 7 cases 说法 | DESTRUCTORS:150 | 实际 tests/drop/ 13 个 .extc + errors/ 6 个反例（本次 ls 核实） |
| gate-mark-both 独立脚本 | RECLAMATION:204/951 | 未在 tools/ 找到同名脚本，判据可能在 gate.sh/inject.py（未核） |

## 附录 B：本次可复现实验

环境：build/extc 构建于 2026-11-08 16:19；git HEAD 85b8871b。

1. 默认档（无环 unit 自动事件档）：
   ./build/extc -o /tmp/a1.c tests/reclaim-acyclic/acyclic_heapref.extc
   结果：/tmp/a1.c 第 13 行 #define EXTC_RC_NOCOUNT 0；第 15 行 #define EXTC_RC_SWEEP_FREE 1；共 8 处 extc_rc_alloc。
2. 默认档（含环 unit 无计数+清扫）：
   ./build/extc -o /tmp/a2.c tests/reclaim-shapes/cyclic_shape.extc
   结果：#define EXTC_RC_NOCOUNT 1；#define EXTC_RC_SWEEP_FREE 0；共 12 处 extc_rc_alloc。
3. trap 路径的块内 @drop（/tmp/trapdrop1.extc：块内 var a: res 带 @drop，随后 trap("boom")）：
   结果：stdout 只有 before；无 drop 1；rc=1；stderr 为 “trapdrop1.extc:9: trap: boom”。
4. trap 路径的全局 @drop（/tmp/trapdrop2.extc：全局 var g = res{id:9}，随后 trap）：
   结果：stdout 为 body、drop 9；rc=1。
5. build/ 生成 C 抽样（历史统计，非本次重跑）：294 个 .c 中，NOCOUNT=1 有 15 个、NOCOUNT=0 有 0 个、无该宏 279 个；SWEEP_FREE=1 有 0 个、=0 有 15 个。说明 build 语料没有自动事件档实例（多为含环/容器形状），事件档结论来自源码与 acyclic 测试。

## 附录 C：审计边界与提醒

1. 所有 bench/横评数字均未在当前构建重跑；本文只做“出处与冲突”审计，未验证性能结论。
2. src/back/reclaim.c 的运行时是 C 字符串字面量；引用其行号时一定要说明这是编译器源码行，不是生成 C 的行号。
3. 用户提示中的 src/back/plan.c 不存在；pool site 逻辑实际在 src/check/check_escape.c（lowerPoolSite:1237、promoteInto2:1738）、src/check/check_top.c、src/check/check_expr.c。
4. POOL-LEVELS 的 witness 统计（1,500 个 .extc、0 差异）本次用 tools/check_lvl_witness.py 的语料口径复核到 1501 个文件、基线 0 行；未重跑 compare。
5. docs/analysis/RUNTIME.md 与 COMPILE-SIDE.md 是同一任务中其他成员的产物，本文未把它们当作证据。
6. 本文对“准确”的判定以“默认配置下成立或与源码字面一致”为准；凡依赖未重跑数字的，一律标“未验证”而不是“准确”。
