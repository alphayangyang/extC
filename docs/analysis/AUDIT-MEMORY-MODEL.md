# 审计报告：docs/analysis/MEMORY-MODEL-CURRENT.md 源码逐条核对

- **审计对象**：docs/analysis/MEMORY-MODEL-CURRENT.md（912 行，§0–§22；Lead 记为 913 行，实为 912）。
- **基线**：git HEAD 85b8871b，工作树无未提交改动（本报告为唯一新增文件）。
- **方法**：对文档里每一条可核验断言（file:line 引用、常量数值、机制描述、实测数字）都打开对应源码/产物核对：①行号是否落在该断言所指内容上；②转述语义是否与代码逐字一致；③常量值、默认值、档位是否对得上；④§17.2 全部常量重读；⑤§18/§19 实测数字逐条对 docs/analysis/MEASUREMENTS.md；⑥§20 的 10 条对 docs/analysis/DOC-AUDIT.md 与 tests/reclaim-perf/gate.sh。
- **未做**：本次没有编译/运行程序，也没有复验 §0 声称的“evidence 与 build 产物逐字节相同”（列为 C9/C10）；涉及“实测”的判定只做文本对账，不重跑。
- **判定口径**：A=断言与源码相反或算术错误（必须改）；B=引用行号/位置错位；C=缺证据或表述含糊；D=与仓库其它文档口径冲突。

## 结论统计

| 类别 | 条数 | 性质 |
|---|---|---|
| A 错误断言 | **12** | 语义/算术错误，必须改 |
| B 行号/引用偏移 | **24** | 证据指向错误位置，逐条给正确行号 |
| C 缺证据/含糊 | **11** | 需补路径、补限定或补证明 |
| D 与其他文档冲突 | **3** | 需统一口径 |

**一句话结论**：文档的骨架、五基质分类、三档判定链、§17.2 常量表、§19.2 停顿数字全部经得起源码核对；错误集中在三类——绝对化措辞（“恒为/永远/从不”）、档位限定缺失（“默认 NOCOUNT=1”未说哪一档）、证据行号漂移。另有 2 处源码级语义缺口（根表 >4096 会连定向释放一起关掉；extc_task_rootPush 无发射器）比文档措辞更值得单独处理。

---

## A. 错误断言（12 条，按严重度排序）

### A1 §18.2 pd 内存上界的算术错误
- **原文**（:800）：“pd 145.7 MB = 200 个 8 MiB payload + 元数据。”
- **判定**：算术错误。200 × 8 MiB = 1600 MiB = 1.6 GB，与 145.7 MB 差约 11 倍。
- **证据**：docs/analysis/MEASUREMENTS.md:103 原文“pd | 200 × 8 MiB 大对象 | 0.30/0.30 | 667 大对象/s | 145.7 MB”；:140 只写“每个 big 8 MiB payload；200 个 → 145.7 MB 峰值”。145.7 MB 是 200 次先后分配的峰值，同时驻留约 18 个 payload（145.7/8 ≈ 18）。
- **建议改为**：“pd 是 200 次 8 MiB 大对象的先后分配，峰值 145.7 MB ≈ 同时驻留 18 个 payload；145.7 MB 与 200×8 MiB 无关。”

### A2 §6.2 shadeOld 的生效档位写反了
- **原文**（:298）：“shadeOld（reclaim.c:1468-1475）**只在 NOCOUNT=0**（有计数层）时用。”
- **判定**：方向相反。reclaim.c:1463-1467 注释逐字 “Delete barrier (no-count tier only)”；:1468-1475 实际代码是 #if EXTC_RC_NOCOUNT 分支才 if (!extc_rc_wb || !slot) return; extc_rc_shade(*slot);，#else 是 (void)slot;。即 NOCOUNT=1（无计数/清扫档）才 shade 旧值；计数档是 no-op。:1478-1484 的 initRef 里 #if !EXTC_RC_NOCOUNT shade(v) 同理（:1479-1480 注释 “no-count tier: local slots carry no barrier” 与 #if 条件自相矛盾，属源码注释陈旧，以 #if 为准）。
- **建议改为**：“shadeOld 只在无计数档（NOCOUNT=1、清扫档）生效：覆盖前 shade 旧值；计数档下它是 (void)slot 空操作（旧值死亡由 dec 处理）。”

### A3 §2 表：new T[n] “永远是 arena”不成立
- **原文**（:112）：“new T[n]（**无长度**，n 运行期决定）| 永远是‘视图’（slice）… 产物 extc_arena_alloc(&__extc_a[k], n*sizeof(T), ...)，基质 arena。”
- **判定**：错误（且与 :308 自相矛盾）。src/back/codegen.c:4653-4667 是 if (g->needReclaim && planArenaLevel(e) == 1) { data 走 rcAllocCore，值仍是 (slice){.data=…,.len=…} 视图 } else { extc_arena_alloc(...) }。层号 == 1 时 data 是 rc 槽；只有 level != 1 才在 4669-4672 走 extc_arena_alloc。文档 :308 自己也写“new T[n] 只在函数体层是 rc 槽”。
- **建议改为**：“new T[n] 的值恒为 slice 视图；**data** 在层号 == 1（needReclaim 打开）时是 rc 槽，其他层号是 arena 块。基质按 data 的落点标注。”

### A4 §1 表 1-1 第 2 行把两种层号规则并成一句
- **原文**（:50）：“| 2 | rc region | new T、定长 new [N]T、alloc<T>(n)、单值 @overwrite，且层号 == 1 或更浅 | extc_rc_alloc | …”
- **判定**：错误。codegen.c:4604-4634 对无 count 的 new T / 定长 new [N]T 的判据只有 if (g->needReclaim)，与层级无关（4618 起 rcAllocCore）；只有带 count 的 new T[n]（4653）和 alloc<T>(n)（rcAllocInExpr 13910）才受 planArenaLevel(e)==1 限制。
- **建议改为**：拆两条——“单值 new T / 定长 new [N]T / @overwrite 初始化器：needReclaim 时恒为 rc 槽”；“new T[n] / alloc<T>(n)：layer==1 才是 rc 槽，否则 arena”。

### A5 §2 表“恒为 rc”缺少 needReclaim 限定（档 0 / EXTC_RECLAIM=0 时是 arena）
- **原文**（:110-111）：“new T（单值）| 恒为 rc（‘single object is an rc slot’，codegen.c:13907）| extc_rc_alloc(...) | rc”；“new [N]T（定长）| 同上”。
- **判定**：错误（未限定档位/开关）。needReclaim=false（档 0，或 EXTC_RECLAIM=0）时 codegen.c:4632-4634 走 extc_arena_alloc；文档 :64（档 0 行）自己也写“分配点实现 extc_arena_alloc”。
- **建议改为**：“needReclaim 打开（档 1/2）时恒为 rc 槽；档 0 / EXTC_RECLAIM=0 时走 extc_arena_alloc。”并把 :13907 改为 :13906（见 B15）。

### A6 §7.2 根表 > 4096 的后果写重了（定向释放也被一并禁用）
- **原文**（:343）：“3. 若该位置根表长度会超 **4096**，编译器**完全不发射触发点**，并把 region 标成 rcRootsUnreliable（codegen.c:4096-4181）⇒ 该 region 不再开新轮（只能靠定向释放/帧销毁）。”
- **判定**：后半句错误。codegen.c:4127-4128 是 if (n > 4096) { g->rcRootsUnreliable = true; return; }；而 codegen.c:7092（定向释放发射判据）是 if (!g->needReclaim || g->rcRootsUnreliable || g->coroFunc) return false; ⇒ 定向释放也一起被禁用，只剩帧销毁（regionDrop/regionFree）。另外该标志是 g 的函数/单元级字段（codegen.c:572），不是“该 region”；置位点还有 4545 / 7408 / 7449 / 8450 / 9845（协程帧等）。
- **建议改为**：“根表 > 4096 时置 g->rcRootsUnreliable：该函数（单元）不再发射触发点，定向释放也一并禁用（codegen.c:7092），该 region 只能等帧销毁回收。”

### A7 §16.1 EXTC_RC_SLOWSTEP 不是“人为加延迟”
- **原文**（:693）：“EXTC_RC_SLOWSTEP=<us> | reclaim.c:2719 | 人为加 us/步，放大停顿便于观测。”
- **判定**：错误。reclaim.c:2719-2720 只是 extc_rc_slowUs = (_ss && *_ss) ? (long long)(atof(_ss) * 1000.0) : -1; 即慢步阈值；:2389-2390（mark）与 :2580-2581（sweep）在超过阈值时打印 [slow] step_n=… dur_us=…，不注入延迟（MEASUREMENTS.md:177/251 的用法也是阈值扫描）。
- **建议改为**：“慢步阈值；单步超过该值（µs）时打印 [slow] 行。仅观测，不改变时序。”

### A8 §13 “generation（四代校验）”无出处
- **原文**（:634）：“容器句柄带 **generation**（四代校验）使陈旧句柄失效。”
- **判定**：错误。src/back/pools.c:5-7 的句柄是 { int64_t index; int64_t generation; }，只有 1 个 generation；ExtcPool 里也只有 1 个 generation 字段（pools.c:50 附近）；只有 dyn 句柄是 {pid, slot, gen, pgen}（pools.c:118 附近，两个 generation：槽代 + 池代）。全仓 grep 无“四代”依据。
- **建议改为**：“容器句柄带 generation，失配即使陈旧句柄失效；dyn 句柄另有槽 gen 与池 pgen 两道校验。”

### A9 §15.2/§18.1 “worker 结束 destroy + regionDrop”与 region 模式代码相反
- **原文**（:661）：“每 worker 一个 extc_arena（…；extc_par_region 时来自 par region）；worker 结束 destroy + regionDrop。”（:781 §18.1 表同：“每任务 = 任务内分配峰值（worker 结束 destroy）”）
- **判定**：错误（region 模式）。codegen.c:14743-14745 是 void *r = j->region; if (r) extc_tls_arena = &((extc_par_region *)r)->arenas[a->id];，注释 14744 逐字“the parent reads it after the join, so it is not destroyed here”。region 模式下 worker 不 destroy、不 regionDrop；统一由 extc_par_close（14718-14733，regionDrop 14726-14728、arena_destroy 14730）在 join 之后做。非 region 模式才是 worker 自己的 extc_arena local（14741/14750 起，worker 自己 destroy）。
- **建议改为**：“region 模式：arena 来自 extc_par_region，worker 不销毁，extc_par_close 在 join 后统一 destroy + regionDrop；非 region 模式：每 worker 一个本地 arena，worker 结束自己 destroy。”

### A10 §15.3 表：reclaim.c:2816 不是 noSweep 的写点
- **原文**（:681）：“| noSweep/owe 跨线程写 | reclaim.c:1761/1821、2816 | 若线程 A 向线程 B 拥有的 region 写 storeRefUnknown/storeStructCons，会无锁写 noSweep |”
- **判定**：2816 错。reclaim.c:2816 是 static void extc_rc_owe(ExtcRcRegion *r) { r->owe = 1; }（写 owe，不是 noSweep）；:2825 是 if (r->noSweep) { r->sweeping = 0; return -1; }（读 noSweep、写 sweeping）。noSweep 的写点只有 :1761（pin 表满）与 :1821（宿主未知的保守 store）。
- **建议改为**：“noSweep 写点只有 reclaim.c:1761/1821；owe 写点 :2816；:2825 只读 noSweep（写 sweeping）。”

### A11 §17.2/§18.3 “SCALE_MAX ⇒ 停止回收”在 thr 溢出时反了方向
- **原文**（:735）：“EXTC_RC_SCALE_MAX | 2^40 | 144 | 阈值上限 ⇒ 事实上等于‘不再回收’”；:807（§18.3 第 4 条）：“scale 可乘到 2^40 … 连续空扫后阈值事实上无穷 ⇒ 等价于停止回收。”
- **判定**：只在 live 小时成立。reclaim.c:2600-2602 确实把 scale clamp 到 SCALE_MAX；但 thrUpd（:2636-2642）是 int64_t thr = r->live * 2; if (thr < r->floor) thr = r->floor; if (scale > 1 && thr > INT64_MAX / scale) { r->thr = 0; return; } r->thr = thr * scale;。:2634-2635 注释逐字：“The overflow case is expressed as thr = 0 (= sweep as soon as anything was allocated)”。即 live > INT64_MAX/(2·2^40) ≈ 4.19e6 个对象时 thr=0 = 每次分配都扫，与“停止回收”相反。
- **建议改为**：“live 较小时 SCALE_MAX 使阈值事实上无限（≈停止回收）；live 大到 thr 溢出时 thr=0（每次分配都扫），是反方向的退化。”

### A12 §11.3 未限定档位的“默认 NOCOUNT=1”
- **原文**（:567）：“- **默认 NOCOUNT=1 时计数层整体编译掉** ⇒ noSweep 的 region **永远不会回收** …”
- **判定**：违反本文档自己的约定（:101：“凡说‘默认’指**档 2 的默认参数**”），且容易误读为全局默认。事件档（档 1）是 NOCOUNT=0，noSweep region 由计数路径在 hold 归零时 freeSlot 回收（reclaim.c:1345-1357）。
- **建议改为**：“**档 2（NOCOUNT=1）** 下计数层被编译掉 ⇒ 该 region 永不回收；事件档（档 1）由计数路径在 hold 归零时回收。”

---

## B. 行号/引用偏移（24 处）

| # | doc 位置 | 文档引用 | 实际位置 | 说明 |
|---|---|---|---|---|
| B1 | :161 §3.2 | planMayUseArena 判据 codegen.c:9636-9641 | 调用在 9654：g->noArena = !planMayUseArena(f) && !(owNow.len > 0 或 owcNow.len > 0)； | 9636-9641 是注释；9658 协程强制 noArena=true |
| B2 | :189 §4.1 | spare 上限 codegen.c:12180 | 宏 12126-12128（EXTC_ARENA_SPARE_MAX (1 << 20)）；保留逻辑在 12130 的 extc_arena_release 文本里 | 12180 在另一个 prim 内 |
| B3 | :198 §4.2 | “32 MiB new 只为触碰部分付页”注释 codegen.c:12166-12178 | 同一注释在 12151 的 extc_arena_alloc 发射文本里 | 12166-12178 是别的 prim 行 |
| B4 | :202/:205-207/:211 §4.3 | evidence/example-alloc-in-block.c:247,268；代码块“247 release/249 alloc/268 release/269-270 destroy” | 实际 245 release（块进入）/247 alloc（(*(q)) = (n*2) 在 249）/252 release（块离开）/258-259 destroy(&a[1],&a[2]) | 整个代码块行号错位，按实际重写 |
| B5 | :229 §5.1 | 分片“键=地址+k，必须逐个 drop”注释 codegen.c:12424-12443 | 注释在 7317-7321（7319-7321 逐字 “key = address + k … every shard must be dropped individually”） | 12424-12443 是 regionOf/regionDrop 函数体 |
| B6 | :230 §5.1 | cgReclaimDropArena codegen.c:7314-7345 | 函数体 7330-7348；分片循环 7346-7347 | 7314-7325 是注释 |
| B7 | :236 §5.2 | slab 行 reclaim.c:138,316,826 | aligned_alloc 在 796；826 是 ttab[1] = EXTC_RC_CONSERVATIVE; | 826→796 |
| B8 | :238 §5.2 | 槽步长 reclaim.c:1059（分配路径） | slotSize 调用在 1074；1059 是 if (r->sweeping) | — |
| B9 | :242 §5.2 | 每槽元数据 reclaim.c:817-829 | hold 注释 817-821、hold malloc 819-821（仅 #if !EXTC_RC_NOCOUNT）、tid 822、ttab 823-826；rscnt 懒建在 1166-1172 | 范围不覆盖所述内容 |
| B10 | :243 §5.2 | ttab reclaim.c:830-833 | 823-826（823 ttabCap=8、824 ttabN=2、825 calloc、826 ttab[1]=CONSERVATIVE） | 830-833 是失败守卫 |
| B11 | :244 §5.2 | freelist reclaim.c:838-850 | 初始化 839 freelist = -1;、840 bump = 0; | 845-849 已是 classBucket |
| B12 | :245 §5.2 | 槽号上限 reclaim.c:325,825 | 常量 325 + 守卫 807-808 | 825 是 ttab calloc |
| B13 | :251 §5.3 | EXTC_RC_CLS_BUCKETS 32，reclaim.c:212 | reclaim.c:211：EXTC_RC_CLS_BUCKETS 32u | 与文档 :744 自己写的 211 冲突 |
| B14 | :255-266 §5.4 | 分配八步 | ①步骤 2/3 的“否则”不成立：1059-1065（sweeping）与 1066-1073（marking）是两个独立 if；②步骤表漏 allocHot 热缓存（1076-1099，命中直接 s=*hot，classSlot 只在无效/满时走）；③步骤 6 把 marking 置黑（1113）与 sweeping 补黑（1116-1118，条件 r->sweeping && r->swEpoch != r->swId && !already）混成一句；④步骤 4 冷路径还漏了 ttab calloc 与条件 hold malloc | 建议按 reclaim.c:1055-1126 重写步骤表 |
| B15 | :110 §2 | codegen.c:13907 引 “a single object is an rc slot” | 注释在 13906；13907-13909 是 “block of them…only where exNew puts it there” | — |
| B16 | :509 §10.4 | 统计 2490-2491 | 2488-2489（relN++；relSlots += freed） | — |
| B17 | :609 §12.3 | “定向释放后的旧槽 2449-2452 注释” | 该注释在 2504-2505；2449-2450 是 mark 清理理由 | — |
| B18 | :657 §15.1 | 协程帧 rcRootsUnreliable codegen.c:517-531、4287-4324 | 517-531 是注释（正确）；协程帧真实置位在 7408/7449；4287-4324 是 rcRootLeaves 的 4096 溢出点 | 引用段与断言不对应 |
| B19 | :657 §15.1 | coroFunc ⇒ noArena=true codegen.c:9635-9674 | 判据 9654、置位 9658；9635-9653 是 cgReleaseLevel 注释与 @overwrite 站点收集 | — |
| B20 | :692 §16.1 | EXTC_RC_PAUSE=1 输出 reclaim.c:2715-2722 | 输出实体 pauseDump 在 2678-2714（[pause]/[step]/[work]/[wbudget]/[worst]/[ubwork]/[guard]）；2715-2723 是 pauseArm（getenv 2717、SLOWSTEP 2719-2720、atexit 2722） | 应拆成两处引用 |
| B21 | :779 :828 :842 :877 | regionFree reclaim.c:1970-2008 | 函数实体 1970-2019（2008 是 huge data 的 free，之后还有 free(tab) 等收尾） | 四处同错 |
| B22 | :880 §20-7 | “唯一可核：86,174,829 → 82,037,532 Ir；tests/reclaim-perf/gate.sh:88；MEASUREMENTS §9” | 82,037,532 在 MEASUREMENTS.md:225（§7 门禁表，判据 ≤ LIMIT 88,029,115），§9 无 Ir 对；gate.sh:88 原文是“本形状 **86 174 849 Ir** ⇒ 线收到 **90 483 570**（= gate 自量 86 174 829 × 1.05）” | 两个数出自不同量具/不同构建，不是一对前/后值，不宜读成“降低 4.8%” |
| B23 | :886 §20-10 | “pool-site 判定在 check_escape.c:1237/1738、check_top.c” | 全路径：src/check/check_escape.c（3682 行；lowerPoolSite:1237、promoteInto2:1738）、src/check/check_top.c（10736 行；calleeMakesPool:1906、makesPool 闭包 9782-9880） | 目录不是 src/back |
| B24 | :653-654 §15.1 | 任务表 realloc :108-113；分配门 :124-130 | realloc 实际 106-108（zone/live/arena 三个）+ 111-114 初始化；分配门 124-131 | 各差 1-2 行 |

---

## C. 缺证据 / 表述含糊（11 条）

1. **:857 §19.4**：c_m = 14.4 ns/obj（tau_t）引 MARK-MODEL.md:71-73 未给路径。:30 已声明裸文件名都在 tests/reclaim-perf/ 下，故可解析（该文件 :71-73 确有 250k 13.1 / 1M 14.4 ns/对象），但建议表内写全 tests/reclaim-perf/MARK-MODEL.md:71-73，否则读者无法直接打开。
2. **:843 §19.3-2**：“theory 草案 §4 记录”应写 docs/analysis/GC-MODEL-DRAFT.md §4.2（:273，源头 docs/changelog/2026-10.md:4235）。
3. **:102 §1.5**：引 docs/analysis/MEASUREMENTS-TIER1.md，该文件不存在（docs/analysis/ 只有 COMPILE-SIDE / DOC-AUDIT / GC-MODEL-DRAFT / GC-ROOFLINE / MEASUREMENTS / MEMORY-MODEL-CURRENT / RUNTIME / evidence / model）。应标“未实测”或补做。
4. **:680 §15.3**：表内自标“是否真并发取决于 extc_par_open 是否预建 region（**未验证**）”。已核实：extc_par_open（codegen.c:14706-14714）只 calloc 结构体 + arena 数组 + arena_init，不建 region；region 由 worker 首次分配惰性 regionOf 建立（12423-12433、14743-14745），而 region 注册表无锁 ⇒ 多 worker 各自建 region 的竞争窗口真实存在。建议把“未验证”改为已确认结论。
5. **:851 §19.4**：公式 P_max ≤ c_m·b + c_s·s + c_c·w + c_f·f + c_r·n + ε 里 c_r、n 在符号表（:854-863）中未定义，但 :864 的杠杆排序又用到 c_r·n。补定义（c_r=每槽释放成本，n=region 槽数）或删项。
6. **:797-800 §18.2**：上界未计 roundOk 信用。shouldSweep = r->bytes > r->thr && extc_rc_roundOk(r)（2650-2658），而 roundOk = allocBytes - roundVisitedBytes >= max(live, floor)（2646-2649）⇒ 一轮之后 M 可再涨 max(L, floor)。建议写成 M ≲ thr + max(L, floor) + slabPadding——这样恰好能解释 pe 3.0 MB ≈ 1 MiB 阈值 + 1 MiB 信用 + slab 粒度。
7. **:900 §21**：“计数档位选择（§1.5）| **清扫档**”。默认是按 unit 自动（无环 unit ⇒ 事件档），不是固定清扫档。改“自动（档 1/档 2）”。
8. **:379、:751**：“EXTC_RC_MARK_WINDOW 的 ×8 在默认参数下永不生效”未给完整证明。markAdvance（1023-1038）确有 if (r->allocBytes - r->markStartAlloc > EXTC_RC_MARK_WINDOW) b *= 8; 且随后被 MARK_BATCH_MAX clamp 到 256；:379 的论证只用了基数 512 > 256，但长标记轮（大活集）确实可以超 2 MiB 窗口，此时 ×8 发生在 clamp 之前、结论仍是 256——结论对，但理由不完整；:751 的“不生效”应写清是“对批大小无影响”。
9. **:22 §0**：“产物与 build/new.c、build/alloc-in-block.c **逐字节相同，唯一差别是 #line …**（可自行 diff 验证）”——无 md5/diff 留证，本次未复验。建议补一条 diff 记录或标“未验证”。
10. **:710 §16.2**：“[rt:types]/[rt:helpers]/[rt:tlsdepth]/[rt:release]/[rt:owe] 五段与源码字符串表**字节完全相等**”——同样未留证（同上）。
11. **:219 §4.4**：“level==1 的 new 走 rc，见 §2”与 :112（§2 表“new T[n] 永远 arena”）自相矛盾，是 A3 的根源；修 A3 时同步改此处措辞。

---

## D. 与仓库其它文档的口径冲突（3 条）

1. **“没有自动回收不可达容器” vs POOL-LEVELS 的整池回收**。:635 §13：“drop/release（整 pool 释放）、place 结束、zoneLeaveTo 回卷；**没有**任何‘自动回收不可达容器’的机制。”
   docs/design/POOL-LEVELS.md:166-200 称“Whole-pool collection 已可用 syspool::collect()”。源码支持后者：stdlib/std/sys/pool.extc:38-43 的 collect() 调 extc_task_collect()；src/back/coroutine.c:78-95 遍历 depth == 0 的 zone、按 extc_taskRoots 可达性判断后 extc_task_end + extc_pool_zoneFreeRoot。
   **统一口径**：“自动（无显式调用）回收不可达容器：没有；**显式**整池/zone 粒度回收：有（syspool::collect() → extc_task_collect），容器/槽粒度仍没有。”文档 §13 应补这一句，否则读者会低估现有能力。

2. **extc_task_rootPush 无发射器——文档 §13/§15 均未提，且这是源码级语义缺口**。coroutine.c:69-74 定义 extc_task_rootPush/rootPop，extc_task_collect（:78-95）依赖 extc_taskRootN 判可达；但 codegen.c 只在 :2986/3220/7385/7416 发射 extc_task_begin/zone/alloc、:6087/6121/9432 发射 extc_task_end，从不发射 extc_task_rootPush（grep -n 'extc_task_' src/back/codegen.c 只有上述调用，无 rootPush）。后果：extc_taskRootN 恒为 0 ⇒ 任何一次 syspool::collect() 都会把所有 depth == 0 的 zone（即所有还活着的任务）都判为不可达并 extc_task_end + zoneFreeRoot。:62-63 的注释自己说“the emitter registers the handles it can see”——发射器这一半没有落地。
   → 文档 §15.1 必须写明“根的登记侧未接线”，并建议 Lead 把这条单列为潜在 bug（不是文档措辞问题）。

3. **§20-7 的 gate 数字引用**（见 B22）：:880 把 86,174,829（gate 自量）与 82,037,532（MEASUREMENTS.md:225 的现行实测）当作一对“可核”数字，并引 MEASUREMENTS §9（§9 无 Ir 对）。与 MEASUREMENTS.md 的门禁口径（LIMIT = 88,029,115）和 DOC-AUDIT.md 的表述不一致，需按 B22 重写为：“现行门禁实测 82,037,532 Ir ≤ 线 88,029,115（MEASUREMENTS.md:225）；gate.sh:88 的 86 174 849 是形状自量、86 174 829 是 gate 自量，两者非同一件事。”

---

## E. 已核实无问题（抽样，供 Lead 放心使用）

- **§1.5 三档表 5 行全部准确**：codegen.c:15604-15612（三态自动升级）、13761-13763（EXTC_RECLAIM 默认开）、13768-13770（NOCOUNT=1）、13806-13813（SWEEPFREE 三态，=1 强制 rcNoCount=0）、13962-14031（unitReclaimIdle/unitAllAcyclic）、14062-14065（idle ⇒ needReclaim=false）。
- **§17.2 常量表逐值核对，全部正确**：reclaim.c :138 SLAB_BITS 20、:141 FLOOR 1 MiB、:144 SCALE_MAX 1<<40、:151 SCALE_MUL 4、:154 HOLD_SAT 65535、:211 CLS_BUCKETS 32、:316 SLAB_SIZE、:317 HDR、:321 CHUNK_MIN 64 KiB、:322 SLOTS_MIN 1024、:325 REUSE_MAX_SLOT 0xFFFE、:347 RCP_SHIFT 42、:412 OWNMAP_BITS 12、:702 TTAB_LINEAR_MAX 8、:889 SWEEP_BUDGET 8、:896 SEED_PUSHES 256、:899 SEED_WORDS 512、:902 MARK_EVERY 64、:905 MARK_WINDOW 2 MiB、:908 MARK_STEP 8、:911 BATCH_MAX 256、:914 BATCH_POP 200、:999 ASSIST_CAP 512、:1012 WORK_BUDGET 0、:1048 ALLOC_INLINE 1、:1159 FOREIGN_GUARD 0、:1413 SHADE_GUARD 0、:2131 MARK_SHALLOW 64、:2147 SCAN_CHUNK 65536；codegen.c :12415 REG_MAX 512、:12126-12128 SPARE_MAX 1 MiB。
- **§4.2 arena 四步算法**与 codegen.c:12151 的 extc_arena_alloc 发射文本逐字一致（n<=0→1、8 对齐、扶正 spare、cap=max(n,64) 倍增顶 1 MiB、calloc 失败 trap、仅 !fresh memset）；12119-12120 结构体逐字一致。
- **§5.1 region 机制**：codegen.c:12414-12443（REG_MAX 512、惰性 regionOf、表满不登记、drop 查表 + 尾部交换）逐字一致；7319-7321 分片注释一致。
- **§5.3 桶号公式** ((ss>>3)*0x9E3779B97F4A7C15)>>59（reclaim.c:848）一致。
- **§6.1 八个屏障入口行号全部正确**：initRef 1478-1484、initRefNoHold 1487-1492、storeRef 1495-1503、storeRefOwnedReg 1520-1559、storeRefOwned 1588-1629、storeRefUnknown 1754-1764、storeRefSlot 1791-1796、storeStructCons 1806-1824、storeStructRooted 1834-1842；NOCOUNT=1 时 inc/dec 是空函数（1312-1327）正确。
- **§7.1/§7.3/§7.4/§7.5**：rcAllocInExpr（13902-13916）、thrUpd/roundOk/shouldSweep（2636-2664）、stepBudget（1015-1018）、markAdvance（1023-1038，恒 256）、rcFrameOwnsArena（3861-3865）、tlsDepth（2081-2089）全部一致。
- **§8 标记 / §9 清扫 / §10 定向释放 / §11 rsbit+pin / §12 计数层**的主体逐字一致（除 A2、B16-B18、A10 所列）。
- **§14 plate 全部准确**：src/back/plate.c:35-38（extc_viewOf 无检查）、:69-73（extc_memCopy）；stdlib/std/mem.extc:52-56/72/92-94/115-122/306-309（保留 + 按 CHUNK commit、close=munmap）。
- **§15.2 parallel**：extc_par_open 14706-14714、extc_par_close 14718-14733、TLS owner 三分支 4222-4230、tlsDepth 清/恢复 14758-14772 一致（除 A9 的 destroy 归属）。
- **§16 诊断**：pauseDump 标签、TICK 8 组件、EXTC_RC_TIME、collect/sweep/settle/bump 无发射器（grep 确认）、garbageHint 例外 4174 一致。
- **§18.1/§18.3**：regionFree 只 free 不 munmap（1970-2019，全文件 grep munmap/madvise 0 命中）、budgetOk（1938-1941）、noSweep 只有 1761/1821 两处置位且无清零路径——全部成立（除 A11 的溢出反例与 C6 的信用项）。
- **§19.2 停顿数字与 MEASUREMENTS.md 逐条一致**：pc 4577 次 collect、pause max 16-68 µs、mean≈1.0 µs、占墙钟 0.59%；step n=425,661、p99≈2 µs、>5 µs 占 0.07%、tail 86-314 µs、over1ms=0；pd collect 5-6 µs、step tail 112-256/355/643 µs；大对象默认分段 collect 5-8 µs / step ≤115 µs vs 关分段 1010-1044 µs、over1ms=1；100M 循环后 collect max 仍 16-68 µs（“规模无关性”成立）。
- **§20 的 1/2/3/5/6/8/9/10 条**与 DOC-AUDIT.md 结论一致（第 4、7 条见 B21/B22）；src/back/plan.c 确实不存在。

---

## F. 局限

- 本次未运行任何程序：§18/§19 中标注为“实测”的数字只与 MEASUREMENTS.md 做文本对账，未重新编译复跑；MEASUREMENTS.md 自身已声明测量窗口有并发负载干扰（其 §10.7）。
- 未复验 §0 的“evidence 与 build 产物逐字节相同”（C9/C10），也未复验 MEASUREMENTS 里的 RSS/Ir 原始 JSON；这两项如需 100% 担保，建议由 Lead 跑一次 diff + 门禁。
- 本报告只报不改；docs/analysis/MEMORY-MODEL-CURRENT.md 未做任何修改。
