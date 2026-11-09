# MODEL-UPDATE-P1.md —— P1 落地后对 QUOTA-MODEL / MEMORY-FIRST-MODEL / roofline.py 的模型修正

本文件只做一件事：逐节说明 P1（P 字节配额 + B 惰性 freelist + regionFree 两阶段 + slab 卷计数器）**实测落地后**，
哪些旧结论被推翻、哪些数字要换、哪些缺失的形式化要补上（regionFree 通道、markResume 快路径）。
不改 src/、tests/；GC-ROOFLINE.md 与 MEMORY-MODEL-CURRENT.md 的同步由 Lead 做。

**冻结口径 v7（溯源只认源码 md5）**

| 对象 | md5 / 规模 |
|---|---|
| src/back/reclaim.c | e17080d1c80513b27ca06f2bc6224b0f，4031 行 |
| src/back/codegen.c | d339382889b87c0dd18b357c69e2e779，16017 行 |
| tests/reclaim/driver.c | 7b9ac61a55c140640a5964ab74a8780e，1523 行 |
| 锚点构建 | build-p1/extc b9fd57843f2c734c16756f6850ace9c5（Lead 自建 build-lead7 同源，仅差 -g） |

证据等级沿用前作：[实测] 有构建/量具；[源码] 只认行号；[模型] 由实测常量推出的公式；[估算] 未标定；[原型] 生成 C 补丁。
本文件**不含新跑的计时**（empirics 正在跑 task-29 after 全套，为避免污染数字，本轮只做推导与文档同步）；
凡引自 P1-RQ3-CHUNK.md / P1-QUOTA-FASTPATH.md / P1-REGIONFREE-BATCH.md / GC-ROOFLINE.md §6.7 的数字均标 [实测]。

## 0 结论

| # | 结论 | 关键数值 | 等级 |
|---|---|---|---|
| C1 | **SCAN_CHUNK 8192→128 的门禁前提被否决**：chunk 不是尾巴杠杆，是「每步固定成本摊销的量子」 | 段步 p99.9 1.65/1.84/34.08 us（chunk 128/256/8192）；>10 us 尾部（max 58-871 us）在所有 chunk 上都是 1e-4~1e-3 比例且**不随 chunk 缩小** | [实测] |
| C2 | **单独降 chunk 是用内存换微秒** | pd_bigobj 200 轮峰值 26.6 MB（8192+dl800）→ 215-240 MB（128+dl800）；每 deadline 窗口可扫字数 192,300 vs 62,060（3.1x 差），实测峰值比 8.1x（含 feedback 放大） | [实测]+[模型] |
| C3 | **真正的停顿前提是 markResume 快路径**：一步内连续续扫多段、按 pointer word 计费，把「每步字数 W」与「段长 chunk」解耦 | 5 us 内 W*=961 字（pd）/657 字（dense）；段效率 E(W*)=77.3%/89.7%，而 chunk=128 单段只有 31.2%/62.8% | [模型] |
| C4 | **配额记账不是成本**，sizeratio 的死因是 B 的复用链重建 | 2M 分配只 174 次 markAdvance、deadline checks=0、两个 -D 变体 Ir 差 <0.1%；B 重建 2,020 次/1.89M 洞 ≈ 19.5M Ir | [实测] |
| C5 | **B 已默认关**（EXTC_RC_LAZY_FREE=0），墙钟收益两方测量方向相反 | runtime-facts 13 reps +4.63% / Lead 9 reps −4.3% | [实测] |
| C6 | **L1b（全死 slab 整块重置）已落地，但它是指停顿杠杆而非吞吐杠杆** | Ir 273.19M→264.3M（−3.1%）、墙钟 15.51→15.38 ms（−0.84%）、切片 1221→255（−79%）；原预测 −6~9.5% 墙钟不成立 | [实测] |
| C7 | **regionFree 通道闭合了 pd 类，没闭合 pb 类**：同步段 O(#slabs) 仍在暂停里 | pd 663 us（sync 0.5 + queue 662.5）；dense 1,708 us（单块 8 MiB munmap 不可切）；pb 14.2 ms（sync 13.2 ms / 24,510 slab ≈ 561 ns/slab） | [实测] |
| C8 | **P 的表述必须通道化**，不能再给一个混口径的 max | 见 §3.1 表 | [实测口径] |

## 1 旧 → 新 替换表

| # | 旧断言（位置） | 新事实 / 新数值 | 依据 |
|---|---|---|---|
| R1 | 「SCAN_CHUNK 8192→128 是 5 us 门禁的**硬前提**」（QUOTA-MODEL.md:15 C4、:16 C5） | **作废**。chunk 保持 8192 + EXTC_RC_QUOTA_DEADLINE_US=800 即采纳档（A 档）：段步 p99.9 34.08 us，但 5 us 门禁不覆盖 seg 通道 | P1-RQ3-CHUNK.md §10；reclaim.c:2867-2871 |
| R2 | 「推荐 chunk=128 字 + k_m=10 + b=192」（QUOTA-MODEL.md:16、:139） | **作废**。该组合把内存从 26.6 MB 推到 215-240 MB（C 档）；B'（128+dl0）chase 墙钟 +6.2% | P1-RQ3-CHUNK.md §10 表 C/C' |
| R3 | 「R-Q3 降 chunk ≤256、R-Q4 降 b ≤205」（QUOTA-MODEL.md:206-207） | **改写**为 R-Q3'：不降 chunk、不降 b（保持 8192/256）；降 chunk 的前置是 markResume 快路径 | 本文件 §3.3、§8 |
| R4 | 「配额记账有成本 / sizeratio 回归归因配额」（QUOTA-MODEL.md §5 归因） | **作废**。记账 Ir 差 <0.1%；真因是 B 复用链重建 19.5M Ir，B 默认 0 | P1-QUOTA-FASTPATH.md §0 |
| R5 | 「B 的 s_link 可预测墙钟降幅」（QUOTA-MODEL.md §5.2-5.4、C6） | **降级为不可用**：预测 max 比 0.459/0.207/0.897 vs 实测 0.358/0.183/0.433；且墙钟方向两方相反 | P1-QUOTA-FASTPATH.md §0；QUOTA-MODEL.md §5.3 |
| R6 | 「L1b 可达 −6~9.5% 墙钟」（MEMORY-FIRST-MODEL.md:12 C2、:150、:196） | **换数**：−0.84% 墙钟 / −3.1% Ir / 切片 −79%；L1b 记入「停顿与尾部分布」 | P1-QUOTA-FASTPATH.md §8；Lead m01483 事实 4 |
| R7 | 「E21（L1b 原型）为 P0 待做」（MEMORY-FIRST-MODEL.md:398） | **已完成**（slab 上 nLive/nMark + O(1) 重置 + verify 模式）；E21 关闭 | P1-QUOTA-FASTPATH.md §0、§4 |
| R8 | 「5% 税靠 L1b+L2 在 r=2.83 可达 3.65%」（MEMORY-FIRST-MODEL.md:327） | **保留为 Ir 口径算例**：L1b 墙钟只兑现 0.84%，该结论在墙钟口径不成立 | 本文件 §3.6 |
| R9 | 「P_step 单值 ≤5 us」、panel (h) 的单条 max 曲线（roofline.py v1.3） | **改成通道化**：P = max(通道界)，各通道判据见 §3.1；roofline.py v1.4 panel (i)(j) | 本文件 §3.1、§4 |
| R10 | 「regionFree 不在 P 模型里」 | **补通道**：P_rf = sync(#slabs) + 排空预算（100 us 常态 / 1000 us 强制）；pb 类 sync 项未闭合 | P1-REGIONFREE-BATCH.md；reclaim.c:798-811、:2659-2705 |
| R11 | 「首字节/缺页是停顿主因」的旧疑点 | 段步 max 尾部 58-871 us 的 minflt=0/first=0/trans=0，机制仍未定位（不是 chunk、不是缺页） | P1-RQ3-CHUNK.md §11；reclaim.c:2867-2868 |

## 2 被推翻/改写结论逐条（附反例）

**O1（推翻）chunk 是 5 us 门禁的硬前提 ⇒ 否。**
反例一：chunk=128 段步 p99.9 = 1.65 us 已进 5 us，但 pd_bigobj 峰值 215-240 MB；
反例二：chunk=8192 段步 p99.9 = 34.08 us 超 5 us，但峰值 26.6 MB、四形状墙钟 ≤+2.1%；
反例三：>10 us 尾部在 128/256/512/8192 上都是 1e-4~1e-3 比例且**不随 chunk 缩小**，故它不是段长效应。
机制线索（源码自述，reclaim.c:2871）：默认低于 8192 需要一个先把固定每步成本摊到多段上的 resume；
而现在的 resume 是**一步一段**（reclaim.c:2997-3002：resume 之后 budget=0）。

**O2（推翻）配额记账有成本 ⇒ 否。** 2M 次分配的 sizeratio 只走 174 次 markAdvance、deadline 检查 0 次
（reclaim.c:1485 的 `_quota >= 4096` 门槛让只有大配额才读钟）；-DEXTC_RC_QUOTA_MAX_MARK=1 与 -DEXTC_RC_QUOTA_DEADLINE_US=0 两变体 Ir 差 <0.1%。
sizeratio 的 Ir 回归来自 B：2,020 次重建 / 1.89M 洞，alloc 体 166.4M→211.7M Ir。

**O3（推翻）B 有正收益 ⇒ 不可复现，默认关。** 默认 EXTC_RC_LAZY_FREE 0（reclaim.c:1060-1065 注释逐字给出 19.5M Ir 与噪声内的墙钟）。
B 只留作编译期旋钮；任何以 B 为基础的墙钟预测不得进入决策表。

**O4（改写）L1b 是吞吐杠杆 ⇒ 它是停顿杠杆。** 实测 Ir −3.1%、墙钟 −0.84%、扫掠切片 1221→255。
解释：cycleratio 的 sweep 段 Ir 只占 3.2%，墙钟还含缺页/启动等非 Ir 成分；切片数 −79% 才是可用收益（尾部分布收窄）。
⇒ MEMORY-FIRST 的 L1 表要把「Ir 收益」与「墙钟收益」并列成两列，R-MF1 判据改为看切片数/尾部而不是墙钟百分比。

**O5（补充）regionFree 进 P 模型但只闭合一半**，见 §3.4。

**O6（新增）卷计数器把省略判定变成 O(1)**（reclaim.c:250-252 定义 nLive/nMark、:3343-3346 判据），代价 +0.95% Ir；
verify 模式抓到真 bug：force 条目在 MARK_SCAN 里二次计 nMark（nMark=3/pop=2），修法见 reclaim.c:2919（shadeSlow 计一次、MARK_SCAN 只对非 force 计数）。

## 3 新形式化

### 3.1 P_step 的通道分解（替代单一 5 us 界）

一次分配或一次结算触发的暂停来自若干**互不同源**的通道，取最大值：

    P_step = max( P_adv , P_seg , P_sweepQ , P_rf , P_rel ) + eps

| 通道 | 触发点 | 上界式 | 数值（pd 口径） | 源码行 |
|---|---|---|---|---|
| P_adv 推进 | 每 MARK_EVERY=64 次分配，或配额循环内 | min( W_m·c_u , D + 2·(c_fix + c_scan·chunk) )；W_m = min(n>>3, 2^20) unit，c_u ≈ 17.5 ns/unit | D=800 us ⇒ ≤ 800 + 2×34 ≈ 868 us | reclaim.c:1020-1058、:1477-1502 |
| P_seg 续扫段 | 有待续扫大对象（scanObj 非空） | c_fix + c_scan·chunk | 1.65 / 1.84 / 34.08 us（128/256/8192） | reclaim.c:2967-2984、:2997-3002 |
| P_sweepQ 清扫配额 | 每次分配（清扫进行中） | w1·W_s，W_s = 8·pm + min(ceil(nslots/64), 8192)；合法 slab ⇒ ≤1032 word | ≤ ~30 us（w1≈29.3 ns/word） | reclaim.c:1018-1022、:1465-1471 |
| P_rf 区域释放 | 帧/region 销毁，或 slabNew 处排空 | sync(#slabs) + 排空预算 | 见 §3.4 | reclaim.c:2659-2705、:822、:3500 |
| P_rel 定向释放 | releaseOld | ≤ RELEASE_BUDGET 2048 + 续扫计费 1024 unit | 3,072 unit | reclaim.c:1078-1079、:3211-3218 |

判据按通道给（与 GC-ROOFLINE §6.7.4 同口径）：

| 档 | 判据 | 覆盖的通道 |
|---|---|---|
| warm（默认四形状） | >50 us 的步占比 ≤1e-4 且 p99.9 ≤10 us | P_adv（小配额）+ P_sweepQ |
| seg（续扫段） | p99.9 ≤ min(1.5·c_seg·chunk + 1 us, 64 us)，over1ms=0，max 只作 OBS | P_seg |
| cold-dense（PD / 大对象） | τ_t ≤2 us、p99.9 ≤1 ms、over1ms=0 | P_adv 的 800 us 档 |
| regionFree | 常态 ≤100 us；队列 >8 MiB 切 ≤1000 us；sync 项单列 | P_rf |

旧的单一「P_step ≤ 5 us」只对 warm 档成立；对 seg / cold-dense 档它既不成立也不是目标。
这一条同时解释了 GC-ROOFLINE 里 8192 段的 p99.9 34-87 us 与 5 us 门禁的「矛盾」：两者不在同一通道。

### 3.2 seg 通道：两点拟合与残差

以 [segsteps] dl800 的 p99.9 拟合 P_seg(chunk) = c_fix + c_scan·chunk（单位 us / ns per word）：

| 形状 | c_fix | c_scan | 128 | 256 | 512 | 8192 | 拟合残差 |
|---|---|---|---|---|---|---|---|
| pd（200×8 MiB） | 1.14 us | 4.02 ns/word | 1.65 | 1.84 | 8.19 | 34.08 | 256 +17%、512 +157% |
| dense（200×） | 0.52 us | 6.82 ns/word | 1.39 | 1.98 | — | 56.40 | 256 +14% |

拟合用两端点（128、8192）。四点最小二乘给 pd c_fix=2.70 us / c_scan=3.85 ns/word，且 512 点残差 −3.5 us；
512 点折合 16 ns/word，高于 9.5-12 ns/word 的锚点区间（reclaim.c:2858）⇒ 建议把 P_seg 当**2 倍保守界**使用（拟合值 ×2），
并把 512 点列为 U2（p99.9 自身波动 >2x）。c_scan 的 pd 值 4.02 与 8192 段实测 34.08/8192=4.16 一致，可信度高于 c_fix。

### 3.3 markResume 快路径（本轮要补的核心形式化）

**现状（源码逐字）**：一步之内若存在续扫游标，本步只走一段并把预算清零：

    reclaim.c:2997-3002
    if (r->scanObj) {
        seg = extc_rc_markResume(r);
        pops++;
        budget = 0;
        _b0 = 0;
    }

段长就是 EXTC_RC_SCAN_CHUNK（reclaim.c:2978 `size_t en = st + (size_t)EXTC_RC_SCAN_CHUNK;`），
⇒ **每步固定成本 c_fix 只被 chunk 个字摊薄**，段效率

    E(chunk) = chunk·c_scan / (c_fix + chunk·c_scan)

| chunk | 128 | 256 | 512 | 8192 | 快路径 W=961 |
|---|---|---|---|---|---|
| E, pd | 31.2% | 47.6% | 64.5% | 96.7% | 77.3% |
| E, dense | 62.8% | 77.2% | 87.1% | 99.1% | 89.7% |

**快路径定义**：允许一个 markStep 在字数预算内**连续续扫多段**（每次 resume 后不清预算，按走过的 pointer word 计费），
即把「一步扫描字数 W」与「段长 chunk」解耦。此时

    P_seg(W) = c_fix + c_scan·W
    W* = floor((P_max - c_fix) / c_scan)
    T(W) = W / (c_fix + c_scan·W)   [word/s]

| 量 | pd | dense | 含义 |
|---|---|---|---|
| W*(P_max=5 us) | 961 word | 657 word | 一步最多走的字数，与 chunk 无关 |
| T(W*) | 192 word/us | 131 word/us | 每步有效吞吐（理想上界 1/c_scan = 249 / 147） |
| E(W*) | 77.3% | 89.7% | 相对理想值的损失 = 23% / 10% |
| a 上限（指针密集，1 word/8 B） | 1.54 GB/s | 1.05 GB/s | 由 a/8 ≤ T 得 |
| a 上限（σ=0.35 典型） | 4.4 GB/s | 3.0 GB/s | 由 a·σ/8 ≤ T 得（σ 待 E31 标定） |

对比与解释：不进快路径时 chunk=128 的 E=31.2% ⇒ 同样的 5 us 只换 128 字/步，
deadline 窗口内可扫字数 62,060（128）vs 192,300（8192）⇒ **3.1x**；这与 pd_bigobj 峰值 26.6→215 MB（8.1x）的差距
来自「deadline 到期丢弃剩余信用」的反馈放大（P1-RQ3 §7 机制段），故 3.1x 应读作**下界**，不能读作精确预测。

**可执行结论**：R-Q3' 不是「降 chunk」，而是「先做 resume 快路径，再谈 chunk」；快路径落地前 chunk 必须留 8192。
快路径的验收判据写成一个等式：在 P_max=5 us 下 W ≥ 900 word 且每轮步数 ≤ 8192 档的 1.2x（E30）。

### 3.4 regionFree 通道（P_rf）

新结构（reclaim.c:798-811 常量、:2659-2705 同步段与入队、:822 与 :3500 两个排空点）：
同步段 = abort + pinDropRegion + 逐 slab 摘链 ownDel（O(#slabs)，**不可切**），随后整链入 thread-local 队列；
排空点：常态 8 slab 或 100 us，队列 >8 MiB 时切 1000 us 硬截止（forced++）。

    P_rf = c_sync(S) + min(B_drain, 队列字节/释放带宽)
    B_drain = 100 us（常态）  /  1000 us（强制，队列 >8 MiB）

| 形状 | S（slab） | sync | queue | 合计 | 判定 |
|---|---|---|---|---|---|
| pd_bigobj 200 | ≤4 | 0.5 us | 662.5 us（另一次 rep 742.4） | **663 us** | PASS（≤1 ms） |
| dense（G18） | 1（8 MiB 块） | — | 1,708.4 us | **1,708 us** | 登记：单个大 munmap 不可切 |
| pb_escape_grow 100M | 24,510 | 13,755.6 us | 1,005.0 us | **14,762 us** | RED 未闭合 |

c_sync 不是常数：pb 24,510 slab ⇒ 561 ns/slab；pd 4 slab ⇒ ≤125 ns/slab（差在 unlinkChildren 的「占用槽×出边」成本，且只有 rsIncN≠0 才走）。
按 561 ns/slab 反解「1 ms 契约」的 slab 上限：**S ≤ 1,782**。超出即需 region 级 closing 队列
（同步段 O(1) 挂链 + closing 标志让 owns() 拒绝认领，ownDel 移入有预算排空；代价是排队期间元数据仍活）。
⇒ 契约写法：regionFree 常态 ≤100 us、强制 ≤1,000 us；**sync 段只承诺 S ≤1,782 slab**，更大的 region 登记为契约外。

### 3.5 内存界：quota pacing 是结构前提，sweep 配额是新通道

源码逐字（reclaim.c:1015-1017）：配额节奏 is the structural premise of the memory bound M <= M_max + buffer。
故内存真界的形式不变，但**成立理由**换成配额：

    M_peak <= M_max + max(L,floor) + (c_t/c* - 0.25)·L + n_max = M_max + max(L,floor) + 0.686·L + n_max

实测支持：pd_bigobj 200 在 EXTC_RC_QUOTA_DEADLINE_US=800 下峰值 26,456 kB（改前 26,460 kB），[quota] 命中 0-1 次。
反面：deadline 调小到 250 us 时命中 99-143 次、峰值 34,524 kB（reclaim.c:1053-1056 注释）⇒ **deadline 是内存与停顿的交换旋钮**，不是纯收益项。
sweep 配额通道的界：W_s ≤ 8·pm + 1,032 word（合法 slab），故 P_sweepQ ≈ 30 us 量级，且不随分配字节数增长。

### 3.6 elide 通道：把吞吐收益重新分类为停顿收益

卷计数器（slab 上 nLive/nMark，reclaim.c:250-252 定义、:1601-1602 alloc +1、:1745-1746 free −1、:2063 shade +1）
让「整块全死」判定 O(1)（:3343-3346 判 nMark==0，死槽数取 nLive），整块重置见 :3377-3378；verify 模式 :3358-3368。
实测（P1-QUOTA-FASTPATH §8, cycleratio）：V0 273,188,831 Ir → V1（关 elide）284,594,252 ⇒ elide 省 11.40M；roll-up 花 2.67M；净 −8.73M（−3.1%）。
墙钟 15.51→15.38 ms（−0.84%）；扫掠切片 1221→255。
⇒ 在 roofline 平面（λ, a）上它几乎不动（Δλ ≈ −0.003 ns/B 量级，落在噪声内），但在停顿分布上合并掉 79% 的切片
⇒ 应写进「停顿有界」一栏，不写进吞吐栏。

## 4 roofline.py v1.4 的变更

| 项 | v1.3 | v1.4 |
|---|---|---|
| 版本与口径 | v1.3，BUILD=HEAD 85b8871b，锚点 build-th 72c05dff | v1.4，docstring 标 v7 冻结 md5 与「锚点来自 build-p1/extc b9fd5784」 |
| 图 | 4x2（panel a-h） | 5x2（新增 (i) seg 通道、(j) regionFree 通道） |
| 表 | A-I 段 | 新增 P 段「P1 修正（旧→新）」与 J 段「G-P1 门禁」 |
| P 常量 | SCAN_NS_W=11 ns/word 单一常数 | 拆成 C_SCAN_PD=4.02 / C_FIX_PD=1.14 us / C_SCAN_DENSE=6.82 / C_FIX_DENSE=0.52 us，并带实测 chunk 表 |
| P 模型 | P_quota(n) 单式，推荐 chunk=128 | P_seg(chunk) 拟合 + 快路径 T(W*)/W*/a_max；chunk=128 组合标作「已否」 |
| 新增常量 | — | Q_MAX_WORDS=8192、Q_DEADLINE=800 us、Q_MAX_MARK=2^20、Q_SHIFT=3、RELEASE_BUDGET=2048、FREE_* 四常量 |
| 新增数据 | — | RF 三形状实测、吞吐代价 1.054/1.050/1.004/1.007、elide 三数、callgrind promote 115,211,310→129,126,214 |

产物（本轮实跑，`python3 docs/analysis/model/roofline.py` 退出码 0，无网络；未跑任何计时程序）：

| 文件 | 规模 |
|---|---|
| docs/analysis/model/roofline.png | 1,270,189 B，2340x3660（5x2 十联图） |
| docs/analysis/model/roofline-table.md | 24,130 B，128 行（A-I + P + J 段） |

脚本 §P1 自检的关键输出（与本文数字一致，可作回归判据）：

    seg 拟合：pd c_fix=1.14 us / c_scan=4.02 ns/word；dense 0.52 us / 6.82 ns/word（倍数 2.0）
    chunk=  128 word: pd 实测 1.65 us / 模型 1.65 / 保守 3.31；dense 1.39
    chunk=  256 word: pd 实测 1.84 us / 模型 2.17 / 保守 4.34；dense 1.98
    chunk=  512 word: pd 实测 8.19 us / 模型 3.20 / 保守 6.40（离群）
    chunk= 8192 word: pd 实测 34.08 us / 模型 34.07 / 保守 68.14；dense 56.40
    段效率 E(chunk)：pd 31.1/47.4/64.4/96.7%；dense 62.7/77.1/99.1%
    快路径：W*(5us)=960 字（pd，E=77.2%）/ 657 字（dense，E=89.6%）；T=192/131 word/us
    keep-up a <= 1.54 GB/s（sigma=1）/ 4.39（sigma=0.35）
    deadline=800 us 窗口可扫字数：128->61889  256->94416  512->128070  8192->192346（3.1x）
    regionFree pd_bigobj200 S=4 sync=0.5 queue=662.5 => 663.0 us / 单步上界 102.2 us
    regionFree dense_G18 S=1 queue=1708.4 => 1708.4 us / 单步上界 100.6 us
    regionFree pb_escape100M S=24510 sync=13755.6 queue=1005.0 => 14760.6 us / 单步上界 13850.1 us
    1 ms 契约的 slab 上限（561 ns/slab）= 1782 slab
    elide：Ir 273.19M -> 264.46M（净省 8.73M，约 -3.2%），墙钟 15.51 -> 15.38 ms（-0.84%），切片 1221 -> 255

图中两处显式的「作废标记」：panel (h) 标题与图例已标「v1.3 口径 / chunk 结论已否，见 (i)」；表 H 段「SCAN_CHUNK 与 b 的联合约束」与 I 段「G-Q2 停顿」两行加前缀【已作废，见 P/J 段】。
## 5 标定清单（新增）

| 编号 | 要标什么 | 怎么测 | 判据 | 优先级 |
|---|---|---|---|---|
| E29 | seg 通道 c_fix/c_scan 的跨形状值 | [segsteps] 在 pd/dense/大数组/chase 四形状 × chunk{128,256,512,1024,8192} × 3 reps，per-shape 拟合 | 残差 ≤20%（当前 pd 512 点 157% 须解释） | P0 |
| E30 | markResume 快路径原型 | 允许一步内连续续扫（budget 不清零、按 word 计费），测 p99.9 与每轮步数 | 5 us 内 W ≥900 word，步数 ≤8192 档的 1.2x | P0 |
| E31 | warm 形状的 σ 与 a 联合 | calprof 记 L/H 与 alloc/wall | 用 a·σ/8 ≤ T(W*) 检验 keep-up | P1 |
| E32 | c_sync(#slabs) 多点拟合 | 构造 16/256/4,096/24,510 slab 的 region 逐帧销毁 | 偏差 ≤25%，定 1 ms 契约的 S 上限 | P1 |
| E33 | elide 的停顿收益 | [segsteps]/[step] 的切片数与 p99.9，开/关 elide 对照 | 切片 −70% 以上且 p99.9 不升 | P1 |

## 6 存疑

| # | 存疑 | 现状 |
|---|---|---|
| U1 | 段步 max 尾部 58-871 us（最高 1,596 us）机制未定位 | minflt=0/first=0/trans=0 全排除；不是 chunk；疑 THP/缓存冷读，未做 perf 采样（P1-RQ3 §11） |
| U2 | pd 的 512 字点 8.19 us 明显偏离拟合 | 只能标为 p99.9 自身波动 >2x；E29 必须复测 |
| U3 | c_sync 只有两点（pd 4 slab、pb 24,510 slab） | 中间段未知；S ≤1,782 是线性外推 |
| U4 | 快路径 a 上限里的 σ | σ=0.35 来自 digraph 1M；四形状各自 σ 未测（E31） |
| U5 | pd_rf 两次 rep 663 / 743.5 us 差 12% | 模型取 663，需 3 reps 定 p99 |
| U6 | 吞吐代价 1.054/1.050/1.004/1.007 是相对 HEAD 的墙钟比 | 与 callgrind promote +12.1% 不同源（Ir vs 墙钟），不可互换算 |
| U7 | L1b 的 −0.84% 墙钟只在 cycleratio 一个形状上测到 | 其它形状 full_dead≈0 本不适用；不得外推 |

## 7 下一轮顺序建议（一句）

先把 **E30（markResume 快路径）** 做出来再碰 chunk——它是唯一能同时买到「chunk 小、步数不涨、内存不爆」的改动，其余 P1 项都已在实测里拿到确定答案。

## 8 决策规则增量（并入 R-Q 表）

| 编号 | 规则 | 数值依据 |
|---|---|---|
| R-Q3' | 不降 SCAN_CHUNK（保持 8192）、不降 b（保持 256）；降 chunk 的前置是快路径 | §2 O1、§3.3 |
| R-Q10 | P 按通道判，不用单一 max；warm / seg / cold-dense 三档各自判据 | §3.1 |
| R-Q11 | regionFree 契约写「常态 ≤100 us / 强制 ≤1,000 us，sync 段只承诺 S ≤1,782 slab」；更大 region 登记未闭合 | §3.4 |
| R-Q12 | 不为「省配额记账」做任何特化（记账不是成本） | §2 O2 |
| R-Q13 | B（LAZY_FREE）保持 0；不得以 B 的预测进入决策 | §2 O3 |
| R-Q14 | elide 的收益记入「停顿/切片数」，不记入吞吐 | §3.6 |

## 9 复现

    md5sum src/back/reclaim.c src/back/codegen.c tests/reclaim/driver.c build-p1/extc    # 对照 §0 表
    python3 docs/analysis/model/roofline.py                                             # 退出码 0，重生成 png/table

数据来源：docs/analysis/P1-RQ3-CHUNK.md、P1-QUOTA-FASTPATH.md、P1-REGIONFREE-BATCH.md、GC-ROOFLINE.md §6.7（均由 Lead/队友实测）。

