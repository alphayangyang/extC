# `tests/reclaim-gaps/` —— **现在还有哪些情况回收不掉**（实测矩阵）

对象：分支 `main`，HEAD `c66e9f4`（含 `19fd567` 的 C 洞收紧 + 本帧/欠收子集 A+B）。
方法：为每一维组合造小形状（≤60 行），**逐条实测**：开关开（`EXTC_RECLAIM=1`）vs 关
（`EXTC_RECLAIM=0`），量峰值（`/usr/bin/time -v` 的 Maximum resident set size）、退出码、
stdout、以及 `tools/rc-pause-probe.py` 观察到的**清扫次数**。

**没有推理填空**：下面每一行要么有实测数字，要么明确写「未实测」。
本目录只新增文件；`src/`、`docs/`、`tools/` 既有文件、`tests/` 既有文件**一个字节没改**。

```sh
tests/reclaim-gaps/run-gaps.sh                 # 全矩阵 + 规模判据（永远 exit 0，打印结果）
tests/reclaim-gaps/run-gaps.sh home_cycle_r1_n40000   # 只跑指定条目
python3 tests/reclaim-gaps/audit-static.py --selftest # ② 检测器有牙的自检
python3 tests/reclaim-gaps/audit-static.py            # 全语料静态审计（①/② 两个计数）
python3 tests/reclaim-gaps/audit-static.py --gate     # 红计数非 0 ⇒ exit 1（建议接进 check.sh）
```

原始输出：`out/matrix.tsv`（39 行明细）、`out/scale.tsv`（19 行 N vs 4N 判定）、
`out/raw.txt`（逐形状 stdout/rc/ASan）、`out/audit-static.tsv` + `out/audit-static-summary.txt`、
每个形状开档生成物 `out/<形状>.on.c`。

---

## 0. 一句话结论

**19 个可运行形状里，9 条收不掉**（涉及 8 个形状族；N=160000→640000 峰值比 2.65×~3.38×，判据 < 1.5）。
其中 **7 条比「整块关掉回收」还差**（on 档峰值 − off 档峰值 ≈ +3.6~+6.6 MB @ N=640000）——
那是「花了回收的钱没拿到货」。收不掉的 7 个根因（G1–G7）：

| # | 根因 | 形状 | 严重度 | 能否补 |
|---|---|---|---|---|
| G1 | **拥有者只调用一次** ⇒ 欠收标志一辈子没被销账，家的房间里的环垃圾等到帧销毁 | `home_cycle`（rounds=1） | **高**（比关档差 +6.6 MB）| 需新机制（帧销毁前销账 / 根栈），非判据能补 |
| G2 | **协程帧 / `ext` 帧**（`rcRootsUnreliable`）⇒ 一个触发都不发 | `coro_frame_cycle`、`ext_frame_cycle` | **高**（+6.5/+6.6 MB）| 需设计（挂起帧的根表） |
| G3 | **`match` 绑定引用载荷** ⇒ 整函数根不可靠 ⇒ 触发全关 | `match_payload` | **高**（+6.6 MB）| 可收窄：只让"枚举不出来的那个变量"不可靠，而不是整个函数 |
| G4 | **`parTls`（TLS worker 房间）** ⇒ 既不触发也不欠收 | `par_tls_cycle` | **高**（+6.5 MB）| 需设计（TLS 区域归属）；本轮**首次**给出正向并行程序 |
| G5 | **池容器里的引用** ⇒ `storeRefUnknown`（目标区域 `noSweep`）+ 无触发 | `pool_ref_slot` | **高**（+6.6 MB）| 同 G6 |
| G6 | **视图元素 / 基表达式带调用**的槽 ⇒ `noSweep`（目标区域退出清扫）| `slot_elem_unknown`、`base_call_unknown` | 中（2.7×，但比关档好）| 保守路径是有意的；要补得把宿主追出来 |
| G7 | **dyn 载荷里的引用** ⇒ dyn 池不回收（已知限制，回收推到 place 退出）| `dyn_payload` | 中（+3.6 MB）| `DYN.md` 已记为已知限制 |

**静态审计**：① 区域没有回收者的程序 = **13**（今天必然非 0 ✓）；② 未记账的引用槽写入 = **0**
（与任务预期相反，见 §4——检测器有牙，但今天这类直接字段写入确实一条不剩）。

---

## 1. 矩阵总表（每一行都有实测数字）

口径：峰值 = `/usr/bin/time -v` 的 Maximum resident set size，量**生成物二进制**（KB/1024 = MB）。
「on 160k→640k」= `EXTC_RECLAIM=1` 两跑；「off」= `EXTC_RECLAIM=0` 两跑。
「sweeps」= 探针观察到的 `extc_rc_collect` 次数（160k / 640k）。
**「收得掉」= on 档 160k→640k 峰值比 < 1.5**。

| # | 形状（shapes/）| A 房间 | B 槽 | C 结构 | D 谁触发 | 收得掉? | on 160k→640k | off 160k→640k | sweeps 160k/640k | 比关档差? | 红判据 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `home_cycle` rounds=1 | 调用者家房间 | 普通字段 | 自环 | **拥有者只调一次** | ✗ **2.94×** | 6.11→**17.98** | 3.86→11.41 | **0 / 0** | **是 +6.6** | 无（建议①l）|
| 2 | `home_cycle` rounds=64/256 | 调用者家房间 | 普通字段 | 自环 | 拥有者反复调 | ✓ 1.00× | 4.98→4.98 | 40.79→158.98 | 31 / 127 | 否 | 现成 ①k 同机制 |
| 3 | `own_room_cycle` | 本帧房间 | 普通字段 | 自环 | 本帧自己 | ✓ 1.01× | 4.98→5.04 | 3.92→11.36 | 2 / 9 | 否 | 现成 ①/①b 同类 |
| 4 | `cross_scope_cycle` | 本帧房间 | 普通字段 | **跨作用域圈** | 本帧自己 | ✓ 0.99× | 5.04→4.98 | 6.29→21.11 | 4 / 19 | 否 | 现成 ① 同类 |
| 5 | `slot_elem_known` | 本帧房间 | **下标槽 `s.kids[0]`** | 圈 | 本帧自己 | ✓ 1.00× | 12.48→12.48 | 16.29→60.29 | 6 / 28 | 否 | 现成 ①d |
| 6 | `slot_elem_unknown` | 本帧房间 | **视图元素 `v[0].kids[0]`** | 圈 | 本帧自己（区域 `noSweep`）| ✗ **2.70×** | 12.29→**33.16** | 16.23→60.29 | 4 / 19 | 否（比 off 好）| 现成 ①d′ 只查结构（建议①m）|
| 7 | `base_call_unknown` | 本帧房间 | **`(*id(s)).kids[0]`** | 圈 | 本帧自己（区域 `noSweep`）| ✗ **2.69×** | 12.29→**33.11** | 16.23→60.29 | 4 / 19 | 否（比 off 好）| 同上（建议①m）|
| 8 | `struct_copy_slot` | 本帧房间 | **整值存储 `s.w=wrap{…}`** | 圈 | 本帧自己 | ✓ 1.00× | 8.54→8.54 | 13.79→50.54 | 11 / 51 | 否 | 现成 ①f |
| 9 | `struct_copy_ret_home` | 调用者家房间 | **按值返回**落具名槽 | 自环 | 拥有者反复调 | ✓ 0.99× | 5.04→4.98 | 1.48→1.42 | 2 / 9 | 否 | 无（建议①s）|
| 10 | `carray_slot` | 本帧房间 | **C 定长数组 `arr[i%8]`** | 自环 | 本帧自己 | ✓ 1.00× | 4.98→4.98 | 3.86→11.36 | 2 / 9 | 否 | 无（建议①s）|
| 11 | `owcell_garbage` | **`@overwrite` 格子** | 普通字段 | 自环 | 拥有者反复调 | ✓ 1.00× | 4.98→4.98 | 3.92→11.36 | 2 / 9 | 否 | 无（建议①s）|
| 12 | `coro_frame_cycle` | **协程帧** | 普通字段 | 自环 | **无（不触发）** | ✗ **2.97×** | 5.96→**17.71** | 3.65→11.21 | **0 / 0** | **是 +6.5** | 无（建议①n）|
| 13 | `ext_frame_cycle` | **`ext` spawn 帧** | 普通字段 | 自环 | **无（不触发）** | ✗ **3.00×** | 5.90→**17.71** | 3.65→11.15 | **0 / 0** | **是 +6.6** | 无（建议①n）|
| 14 | `match_payload` | 本帧房间 | **枚举载荷绑定** | 自环 | **无（整函数根不可靠）** | ✗ **2.93×** | 6.11→**17.92** | 3.92→11.36 | **0 / 0** | **是 +6.6** | 无（建议①o）|
| 15 | `par_tls_cycle` | **TLS worker 房间** | 普通字段 | 自环 | **无（parTls 不触发/不欠收）** | ✗ **2.65×** | 8.60→**22.79** | 5.17→16.29 | **0 / 0** | **是 +6.5** | 无（建议①p）|
| 16 | `pool_ref_slot` | **池缓冲（视图）** | `pool<mut ref node>` 的槽 | 自环 | **无（`noSweep`+无触发）** | ✗ **2.94×** | 6.09→**17.92** | 3.86→11.36 | **0 / 0** | **是 +6.6** | 无（建议①q）|
| 17 | `dyn_payload` | **dyn 池** | dyn 载荷里的引用 | 自环 | 本帧自己（dyn 池不回收）| ✗ **3.38×** | 24.35→**82.29** | 20.73→78.67 | 2 / 9 | **是 +3.6** | 无（建议①r）|
| 18 | `byval_temp` | 调用者家房间 | **按值返回纯临时量** | 自环 | 拥有者反复调 | ✓ 0.99× | 5.04→4.98 | 3.86→11.41 | 2 / 9 | 否 | 无（建议①s）|
| 19 | `closure_chain` | 本帧房间 | **嵌套闭包环境** | 自环 | 本帧自己 | ✓ 0.99× | 5.04→4.98 | 1.48→1.42 | 2 / 9 | 否 | 无（建议①s）|
| 20 | `global_slot`（G9）| 全局量 | — | — | — | **编译期拒** | — | — | — | — | 前端已有 |

`matrix.tsv` 另有 N=40000/160000 的 39 行明细（含 on 档生成物的
`maybeCollect`/`owe`/`checkOwed`/`storeRefUnknown`/`storeRefOwned`/`storeStructCons`/`initRef`
调用点数）。所有 38 个 ASan/UBSan 跑都是 `rc=0`、无 `SEGV/UAF/runtime error`。
**语义**：19 个可运行形状开档/关档 stdout 与退出码**逐字一致**（`sem=一致`）。

> 「比关档差」= on 档峰值比同一形状 `EXTC_RECLAIM=0` 还大 ≥ 0.5 MB——第 1、12、13、14、15、16、17 条命中。
> 注意 6/7 条（`noSweep`）**不是**比关档差（off 更差），但 on 档仍随规模涨：收了一部分，收不干净。

---

## 2. 逐类证据

### 2.0 十条嫌疑的确认 / 推翻一览

| 嫌疑（任务原文）| 判定 | 凭据（§）|
|---|---|---|
| 1 拥有者只调一次、被调者在它房间里攒大垃圾 | **确认** | G1：`home_cycle` rounds=1，2.94×、比关档差 +6.6 MB |
| 2 被调者自己是长循环，调用期间没人清 | **确认** | G1 同一形状（`build` 的 while 就是长循环；`sweeps=0`）|
| 3 判不出宿主的引用槽（`a[i]` / 基表达式带调用 / 视图）| **确认（能力损失、非悬垂）** | G6：`slot_elem_unknown` 2.70×、`base_call_unknown` 2.69×（对照 known = 1.00×）|
| 4 按值传递/返回的临时量携带引用 | **推翻（本形状下收得掉）** | 第 9 行 `struct_copy_ret_home` 0.99×、第 18 行 `byval_temp` 0.99× |
| 5 协程帧 | **确认** | G2：`coro_frame_cycle` 2.97×、`sweeps=0`、比关档差 +6.5 MB |
| 6 `ext`（spawn）帧 | **确认** | G2：`ext_frame_cycle` 3.00×、`sweeps=0`、比关档差 +6.6 MB |
| 7 TLS / 并行（`parTls`）| **确认**（此前「未实测」；本轮新写正向程序）| G4：`par_tls_cycle` 2.65×、`sweeps=0`、比关档差 +6.5 MB |
| 8 池/板容器里的引用、`match` 绑定、`dyn` 载荷 | **确认（池 / match / dyn）+ 板未实测** | G5 池 2.94×、G3 match 2.93×、G7 dyn 3.38×；板只存字节 ⇒ §6 |
| 9 `@overwrite` 格子 | **推翻（本形状下收得掉）** | 第 11 行 `owcell_garbage` 1.00× |
| 10 全局量里的引用 / C 层数组里的引用 | **全局：前端拒；C 数组：推翻（收得掉）** | §3（编译期拒）、第 10 行 `carray_slot` 1.00× |

### G1 拥有者只调用一次（嫌疑 1 + 2）——**新洞，比关档差**

形状 `home_cycle.extc`：`build` 在**调用者的家房间**里建自环链（`p.next = p` + `keep` 覆盖），
`build` 自己就是长循环。生成物里 `build` 的分配点**一个触发都不发**，只发
`extc_rc_owe(__extc_rcw)`（欠收）；`main` 只在**下一次把家 arena 传给被调者之前**
`extc_rc_checkOwed(__extc_rcc, …)` 销账。

| rounds × per | on 峰值 | off 峰值 | sweeps | 读法 |
|---|---|---|---|---|
| 1 × 40000 | 2.92 MB | 1.98 MB | **0** | 640 KB 垃圾还没过 1 MiB 压力线 |
| 1 × 160000 | 6.11 MB | 3.86 MB | **0** | 欠收标志记了（生成物 `owe=2`），但**没有下一次调用** ⇒ 不销账 |
| 1 × 640000 | **17.98 MB** | 11.41 MB | **0** | 峰值随规模 2.94×，比关档**差 6.6 MB** |
| 64 × 40000 | 4.98 MB | 40.79 MB | 31 | 拥有者反复调 ⇒ 每轮销账，有界 |
| 256 × 40000 | 4.98 MB | 158.98 MB | 127 | 界与调用次数解耦 |

⇒ 子集 B 的健全性不变量（“销账时不能有活跃被调者子树”）保住了，但**销账点只在调用前**：
拥有者此后再不发起调用，这笔账永远挂着，区域等到帧销毁（`main` = 进程结束）。
这正是 `tests/reclaim-shapes/README.md` §「代价与后续」自己标出来的那条损失，
本轮第一次把它量成「比关档差」。

### G2 协程帧 / `ext` 帧（嫌疑 5 + 6）——**新洞，比关档差**

`coro_frame_cycle.extc`（`yield` 长循环）与 `ext_frame_cycle.extc`（`domain::single().run { ext task(n) }`）：
两个函数都因为“帧挂起后不在 C 栈上”被置 `rcRootsUnreliable` ⇒ **一个触发都不发、也不欠收**
（生成物里 `maybeCollect(__extc_rcg…` = 0、`owe(` = 0，`sweeps = 0`）。
`keep = p` 把自环节点提到帧的函数级区域，只有帧销毁才还。

on 5.96→17.71（2.97×）、off 3.65→11.21；`ext` 5.90→17.71（3.00×）。
ASan 干净（`rc=0`）——纯上界问题，不是悬垂。

### G3 `match` 绑定引用载荷（嫌疑 8）——**新洞，比关档差**

`match_payload.extc`：main 本体分配（`maybeCollect` = 2，生成物里有触发），
但只要函数里出现 `match m { some(q) => … }` 绑定枚举载荷，整函数的 `rcRootsUnreliable` 就被置上
（`src/back/codegen.c:5487`：绑定类型的 `rcScanType != RC_NO_REFS` ⇒ 本函数触发一个都不发）
⇒ 触发被丢掉：`sweeps = 0`、峰值 6.11→17.92（2.93×）。

生成物证据：同形状把 `match` 去掉（`own_room_cycle`）`sweeps = 2/9`、峰值 4.98→5.04（1.01×）。
⇒ **关掉的是整个函数的触发，而不是“枚举不出来的那个变量”**——这是可收窄的点。

### G4 `parTls` / TLS worker 房间（嫌疑 7）——**首次实测**

`tests/reclaim-rootstack/RECON.md` 说「全语料没有能编译的正向 `parallel::run` 程序，TLS 一档未实测」。
本轮**新写了最小正向程序** `par_tls_cycle.extc`：

```extc
fn w(id: i64, lo: i64, hi: i64, out: mut slice<i64>) -> i32 { … keep = p … }
fn main(args) { … parallel::run(w, out, n, i64(2)) }
```

生成物里 `extc_tls_arena` 出现、`extc_arena_alloc(&(*extc_tls_arena), …)` 就是 worker 的 `new`；
`rcFrameOwnsArena` 与 `rcEmitOwe` 对 `parTls` 都直接返回 ⇒ **`maybeCollect`=0、`owe`=0、`sweeps`=0**。
on 8.60→22.79（2.65×）、off 5.17→16.29，**比关档差 +6.5 MB**。
（threads=2；更多线程未测。）

### G5 池容器里的引用（嫌疑 8）——**新洞，比关档差**

`pool_ref_slot.extc`：`pool<mut ref node>::withCap(8)` 的 dense 缓冲是一条引用槽数组。
生成物里 `p.insert(nd)` 内部对缓冲槽发 `extc_rc_storeRefUnknown`（池缓冲是视图 ⇒ 宿主追不到）
⇒ **目标区域 `noSweep`**；同时 `new node` 的分配点**一个触发都不发**（`var p = pool<…>` 的声明走
`rcRootDecl`，类型扫描回 `RC_CONSERVATIVE` ⇒ 整函数 `rcRootsUnreliable`，`src/back/codegen.c:3463`）
⇒ `sweeps = 0`、峰值 6.09→17.92（2.94×）、比关档差 +6.6 MB。

### G6 视图元素 / 基表达式带调用（嫌疑 3）——**保守路径确实丢回收能力**

- `slot_elem_known`（`s.kids[0] = keeper`，宿主 = `s`）：生成物逐叶 `storeRefOwned`，扫得掉，
  12.48→12.48（1.00×），off 16.29→60.29。
- `slot_elem_unknown`（视图元素 `v[0].kids[0] = keeper`）：生成物
  `extc_rc_storeRefUnknown((void **)&((*slice_node_index(v,0,…)).kids.data[…]), (void*)keeper)`
  ⇒ 运行期把 **keeper 所在区域** `noSweep`。
- `base_call_unknown`（`(*id(s)).kids[0] = keeper`）：同一档。

两条的 on 档 12.29→33.16 / 12.29→33.11（≈2.7×），**比关档（60.29）好，但界仍与规模有关**。
`srUnknown=1`、`sweeps` 的 4/19 次收的是**另一个区域**（region 2，`s` 的循环体房间），
目标区域（region 1）从此一次都不扫——差分对照就是第 5 行（同形状、只换宿主可证性）= 1.00×。

### G7 dyn 载荷（嫌疑 8）——已知限制

`dyn_payload.extc`：`dyn Tag(box { v, n: nd })` 的载荷按值拷进 dyn 池。on 24.35→82.29（3.38×）、
off 20.73→78.67；`sweeps` 2/9 但 dyn 池本身不回收（`DYN.md` 记为已知限制：回收推到 place 退出）。
on 仍比 off 差 +3.6 MB。

### 绿（对照，说明矩阵不是「处处红」）

`own_room_cycle` 1.01×、`cross_scope_cycle` 0.99×、`slot_elem_known` 1.00×、
`struct_copy_slot` 1.00×、`struct_copy_ret_home` 0.99×、`carray_slot` 1.00×、`owcell_garbage` 1.00×、
`byval_temp` 0.99×、`closure_chain` 0.99×、`home_cycle` rounds≥64 1.00×。

⇒ 嫌疑 4（按值传递/返回的临时量）在本轮形状下**推翻**：`byval_temp` 与 `struct_copy_ret_home`
都收得掉（`checkOwed` 在每次调用前销账）。嫌疑 9（`@overwrite` 格子）与嫌疑 10 的「C 层数组」
也收得掉；嫌疑 10 的「全局量里的引用」在**前端就被拒**（见 §3）。

---

## 3. 编译期拒：全局量里的引用（嫌疑 10 的前半）

四种写法全部在 `extc` 阶段被拒（`shapes/global_slot.extc` 保留为反例）：

```
var g: ?mut ref node            → cannot zero-initialize the global `g`: it contains a reference
var g: ?ref node = null         → `null` here does not know which `?ref T` it is
struct holder { f: ?mut ref node } + var h: holder → 同上（contains a reference）
var arr: [4]?mut ref node       → 同上（contains a reference）
```

⇒ 这一档**没有运行期形状可测**：不是「回收不掉」，是「前端不许有」。codegen 里的
`rcGlobalRoots` 路径在本语料里因此碰不到（审计的 `no-alloc` 档里含全部含全局引用的程序）。

---

## 4. 静态审计（两个红计数）

脚本 `audit-static.py`：对全语料（`examples/ tests/ bench/ stdlib/ tools/escape-corpus/`）每个能编译的
`.extc` 用 `EXTC_RECLAIM=1` 生成 C（保留 `#line`），扫生成物。

### ① 区域没有回收者

分类：`ok-trigger`（有触发）/ `owe-settled`（欠收 + 销账）/ `owe-no-settle`（只欠收没销账）/
`silent-both`（不触发也不欠收）/ `no-alloc`（没有回收管辖的分配点）。
**红计数① = `silent-both` + `owe-no-settle`。**

实测（1266 个 `.extc`；编译成功 862，失败 404）：

| 类 | 程序数 |
|---|---|
| ok-trigger | 114 |
| owe-settled | 116 |
| owe-no-settle | 0 |
| **silent-both** | **13** |
| no-alloc | 619 |
| **红计数①** | **13** |

13 个程序（= 今天必然非 0 的那一半）：

```
tests/coro/coro_new.extc                                  alloc=1 trig=0 owe=0 check=0
tests/escape-fuzz/cases/m_home_cont_after.extc            alloc=1 trig=0 owe=0 check=1 srUnknown=2
tests/escape-fuzz/cases/m_home_cont_before.extc           同上
tests/escape-fuzz/cases/m_main_cont_after.extc            同上
tests/escape-fuzz/cases/m_main_cont_before.extc           同上
tests/escape-fuzz/cases/m_nohome_cont_after.extc          同上
tests/escape-fuzz/cases/m_nohome_cont_before.extc         同上
tests/reclaim-gaps/shapes/coro_frame_cycle.extc           alloc=2 trig=0 owe=0 check=0
tests/reclaim-gaps/shapes/ext_frame_cycle.extc            alloc=2 trig=0 owe=0 check=0
tests/reclaim-gaps/shapes/par_tls_cycle.extc              alloc=2 trig=0 owe=0 check=0
tests/reclaim-gaps/shapes/pool_ref_slot.extc              alloc=1 trig=0 owe=0 check=0 srUnknown=2
tools/escape-corpus/P2-6_container_uaf.extc               alloc=1 trig=0 owe=0 check=1 srUnknown=2
tools/escape-corpus/P2-7_container_before_uaf.extc        同上
```

### ② 引用槽写入没有对应记账调用

用生成物自带的 `ExtcRcType` 偏移表（`__extc_rcoN[]` 的 `offsetof(T, f)` ⇒ 哪些字段是引用叶子）
+ `struct T { … }` 定义（字段 → C 类型）做**类型感知**检测：LHS 字段链逐段解析，
只有**所有可能类型**都落到 `T *`（T 是回收管辖的结构）才算候选。宁可漏报，不虚报。

**实测：② = 0**（862 个程序里一条直接字段写入都没有漏记账）。
这与任务预期的「今天必然非 0」**相反**，所以把「判据有牙」的证据一并给出：

```
$ python3 tests/reclaim-gaps/audit-static.py --selftest
selftest: 干净生成物候选 = 0，注入一条裸写入后候选 = 1 ⇒ ok（判据有牙）
```

（自检把 `slot_elem_known` 生成物里的一条 `extc_rc_storeRefOwned((void **)&(s->kids.data[0]) …)`
换成 `s->kids.data[0] = keeper;`，检测器必须报出来。）

> 换句话说：**直接字段写入这一轴今天干净**（`a7e2642`/⑧⑨⑪ 那一批修完的结果），
> 漏的是「压根不走引用槽记账路径」的那几档（整值/临时量/池/dyn/不可靠函数）——
> 它们不表现为裸字段赋值，② 这种文本判据天生抓不到。要抓得做值级分析，那是另一轮。

**②′（补充计数，本轮新增）**：走**保守/退出清扫**路径的程序 = **14**
（`storeRefUnknown` 12 个、`storeStructCons(hostKnown=0)` 11 个，并集 14）。这不是 bug 计数，
是「编译器主动放弃了这个区域的清扫」的规模，与 §1 第 6/7/16 条直接对应。

---

## 5. 建议的判据（**补丁片段，未落地**）

### 5.1 接进 `tests/reclaim-shapes/run-switch.sh`（形如 ①l…①s）

```sh
# ── 建议追加（未落地）────────────────────────────────────────────────────────
# 形状都在 tests/reclaim-gaps/shapes/；判据一律：开档 N vs 4N 峰值比 < 1.5，
# 且「清扫次数 > 0」（用 tools/rc-pause-probe.py，与 ①k 同一把尺子）。
#  ①l home_cycle  rounds=1   1:160000 / 1:640000   今天 2.94×（红）—— 拥有者只调一次
#  ①m slot_elem_unknown / base_call_unknown        今天 2.70× / 2.69×（红）—— noSweep
#     对照 slot_elem_known 必须 1.00×（绿）⇒ 差分闭合
#  ①n coro_frame_cycle / ext_frame_cycle           今天 2.97× / 3.00×，sweeps=0（红）
#  ①o match_payload                                今天 2.93×，sweeps=0（红）—— 根不可靠
#  ①p par_tls_cycle                                今天 2.65×，sweeps=0（红）—— TLS
#  ①q pool_ref_slot                                今天 2.94×，sweeps=0（红）—— 池里的引用
#  ①r dyn_payload                                  今天 3.38×（红）—— dyn 池（已知限制）
#  ①s（护栏，防回归）struct_copy_ret_home / carray_slot / owcell_garbage /
#     byval_temp / closure_chain 今天都 ≤ 1.01×（绿）；加上它们是为了
#     一旦有人动记账路径，这几条会先红。
# 直接复用 run-gaps.sh 的规模段即可：tests/reclaim-gaps/run-gaps.sh 打印判定，
# 若接进门禁，把 out/scale.tsv 的「判定」列当断言源。
```

### 5.2 接进 `check.sh`（静态审计当红判据）

```sh
# ── 建议追加（未落地）────────────────────────────────────────────────────────
# 红判据（今天就会红，exit 1）：
#   python3 tests/reclaim-gaps/audit-static.py --gate || exit 1
#   --gate：红计数①(区域没有回收者)>0 或 ②(未记账的引用槽写入)>0 ⇒ exit 1。
#   今天 ① = 13（必然非 0）；② = 0（检测器有牙，见 --selftest）。
# 只想先观察、不把 check.sh 弄红：
#   python3 tests/reclaim-gaps/audit-static.py >/dev/null || true
# 或者只用 ①（今天的红）：
#   python3 tests/reclaim-gaps/audit-static.py | grep -q '红计数①.*= 0' || exit 1
```

---

## 6. 未实测 / 没把握（诚实清单）

* **板（`heap::plate`）**：`plate.store` 只接 `slice<u8>`（字节），语言层没有「板里的 typed 引用槽」
  这一形状 ⇒ **未实测**（不是「收得掉」，是「测不出」）。池测的是 `pool<mut ref node>`。
* **TLS 只测了 threads=2**；更多线程、worker 里更深调用链、`out` 之外的共享视图都没测。
* **`dyn` 只测了「载荷里带一个自环节点」一种**；dyn 池的回收是已知限制，没有做「dyn 池能不能收」的
  独立判据。
* **`pool_ref_slot` 的 insert/remove 组合只测了一种**（插入后立刻删除）；长时间驻留池里的引用未测。
* **`match` 只测了 `some(?mut ref node)` 一种载荷**；其他枚举/泛型实参带引用的形状未测。
* **协程只测了「帧级 keep 攒自环」**；挂起期间跨 `yield` 持有引用、`ext` 的 `out` 视图等未测。
* **静态审计 ②** 只覆盖「直接字段链写入」；整值/临时量/池/dyn 那一档抓不到（见 §4），
  所以 ② = 0 **不等于**「引用记账完备」。
* **`owe-no-settle` = 0** 是 862 个程序的实测；语料里 `owe` 与 `checkOwed` 几乎成对出现，
  但不排除手工写的形状（例如 `main` 只调一次的 `hole_c`）在别的口径下会落进这一类。
* ASan 的 LeakSanitizer 对域运行时（`extc_dom_new`/`extc_dom_add` 152 B）有既有噪声，
  已置 `ASAN_OPTIONS=detect_leaks=0`；本条不是本轮发现，`tests/ext/domain_runs.extc` 基线同样报。
* **没有跑完整测试套件**（按要求）。

---

## 7. 方法与口径

* 编译：`EXTC_RECLAIM=1 ./build/extc -w --no-line-map <src> -o <c>`；`cc -std=c11 -fwrapv -O1`。
  关档：同一源码换 `EXTC_RECLAIM=0`（`unset` 等于默认打开，不能用）。
* 峰值：`/usr/bin/time -v <生成物二进制>` 的 `Maximum resident set size`（KB→MB 除以 1024）。
* 清扫次数：`python3 tools/rc-pause-probe.py <生成物.c> > probed.c`，跑 `probed` 读
  `PROBE sweeps=N`。**判据只用「sweeps」**；生成物里有没有 `maybeCollect` 是另一列（弱证据）。
* 规模判据取 **160000 → 640000（4×）**，不取 40000→160000：后者的 40000 常常还在 1 MiB
  压力下限（`EXTC_RC_FLOOR`）以下，比的是进程底噪而不是界。
* 语义：开档/关档 stdout + 退出码逐字比（38/38 个可编译 spec 一致）。
* 正确性：38 个 ASan/UBSan 跑全 `rc=0`，无 `SEGV/UAF/runtime error`。
* 静态审计：`-w`（**不带** `--no-line-map`，要 `#line` 划出用户代码段）。
* 本目录**只新增文件**；不改 `run-switch.sh` / `check.sh`（§5 只是建议片段）。

## 8. 文件

| 文件 | 作用 |
|---|---|
| `README.md` | 本文件（矩阵 + 结论 + 建议判据）|
| `run-gaps.sh` | 一键复跑：矩阵（39 行）+ 规模判据（19 行），**exit 0** |
| `audit-static.py` | 全语料静态审计 ①②，`--gate` / `--selftest` |
| `shapes/*.extc` | 19 个形状（+1 个编译期反例），每个 ≤ 60 行 |
| `out/matrix.tsv` `out/scale.tsv` `out/raw.txt` | 原始数字 |
| `out/audit-static.tsv` `out/audit-static-summary.txt` | 静态审计原始输出 |
| `out/<形状>.on.c` | 开档生成物（判据的文本证据）|
