# `tests/reclaim-gaps/` —— **现在还有哪些情况回收不掉**（实测矩阵）

对象：分支 `main` 的当前树（每修掉一条缺口就复跑本矩阵刷新，逐轮记录见 `docs/changelog/`）。
方法：为每一维组合造小形状（≤60 行），**逐条实测**：开关开（`EXTC_RECLAIM=1`）vs 关
（`EXTC_RECLAIM=0`），量峰值（`/usr/bin/time -v` 的 Maximum resident set size）、退出码、
stdout、以及 `tools/rc-pause-probe.py` 观察到的**清扫次数**。

**没有推理填空**：下面每一行要么有实测数字，要么明确写「未实测」。
本目录最初只新增文件；之后每修掉一条缺口，`src/` 与 `docs/` 会跟着改（逐轮见 `docs/changelog/`），`out/` 由 `gate.py` / `run-gaps.sh` 重跑刷新。

```sh
tests/reclaim-gaps/run-gaps.sh                 # 全矩阵 + 规模判据（永远 exit 0，打印结果）
tests/reclaim-gaps/run-gaps.sh home_cycle_r1_n40000   # 只跑指定条目
python3 tests/reclaim-gaps/audit-static.py --selftest # ② 检测器有牙的自检
python3 tests/reclaim-gaps/audit-static.py            # 全语料静态审计（①/② 两个计数）
python3 tests/reclaim-gaps/audit-static.py --gate     # 红计数非 0 ⇒ exit 1（建议接进 check.sh）
```

原始输出：`out/matrix.tsv`（41 行明细）、`out/scale.tsv`（20 行 N vs 4N 判定）、
`out/raw.txt`（逐形状 stdout/rc/ASan）、`out/audit-static.tsv` + `out/audit-static-summary.txt`、
每个形状的开档生成物 `out/<形状>.on.c`（跑 `run-gaps.sh` 现场写、**不入库** —— 要看就自己跑一遍；
`check.sh` 的 `check-tree` 盯着这条）。

---

## 0. 一句话结论

**20 个可运行形状里，5 条收不掉**（涉及 5 个形状族；N=160000→640000 峰值比 2.99×~3.61×，判据 < 1.5）。
其中 **5 条都比「整块关掉回收」还差**（on 档峰值 − off 档峰值 ≈ +1.5~+2.7 MB @ N=640000）——
那是「花了回收的钱没拿到货」。收不掉的根因是 G1/G2/G5/G7；G6（视图/arena 数组里的槽）**已修**（见 §G6a）、
G3（`match` 载荷）**已修**（见 §G3）、G4（`parTls` 线程本地区域）**已修**（见 §G4）；外加**已修**的 G8：

| # | 根因 | 形状 | 严重度 | 能否补 |
|---|---|---|---|---|
| G1 | **拥有者只调用一次** ⇒ 欠收标志一辈子没被销账，家的房间里的环垃圾等到帧销毁 | `home_cycle`（rounds=1） | **高**（比关档差 +2.7 MB）| 需新机制（帧销毁前销账 / 根栈），非判据能补 |
| G2 | **协程帧 / `ext` 帧**（`rcRootsUnreliable`）⇒ 一个触发都不发 | `coro_frame_cycle`、`ext_frame_cycle` | **高**（+2.7/+2.6 MB）| 需设计（挂起帧的根表） |
| G3 | **`match` 绑定引用载荷** ⇒ 整值存储让目标区域退出清扫、根表枚举不出枚举负载 | `match_payload` | **已修**（1.00×，比关档差消失）| 两件事合起来认领完整：根表按所有变体展开负载叶子 + `storeStructRooted` 只记引用（见 §G3）|
| G4 | **`parTls`（TLS worker 房间）** ⇒ 既不触发也不欠收 | `par_tls_cycle` | **已修**（1.13×，比关档差消失）| 按运行期深度认领归属：入口帧物化根表当场扫、内层帧只记欠收，区域随 arena 同寿丢弃（见 §G4）|
| G5 | **池容器里的引用** ⇒ `storeRefUnknown`（目标区域 `noSweep`）+ 无触发 | `pool_ref_slot` | **高**（+2.6 MB）| 同 G6 |
| G6 | **视图/arena 数组里的槽**（视图元素 `v[0].kids[0]`）⇒ 旧行为 `noSweep`（目标区域退出清扫）| `slot_elem_unknown` | **已修**（1.00×，比关档差消失）| 反查失手 ⇒ 把**值钉成根**（`pinAdd`），`markBegin` 按区域播种、`regionFree` 按区域清死指针，表有界（§G6a）。**基表达式带调用已修**：槽地址反查宿主（1.03×）|
| G7 | **dyn 载荷里的引用** ⇒ dyn 池不回收（已知限制，回收推到 place 退出）| `dyn_payload` | 中（+1.5 MB）| `DYN.md` 已记为已知限制 |
| G8 | **可能的环被区域划分切开** ⇒ 两只区域互相钉住 + 自学习退场 | `ring_per_round` | **高**（修前 3.90×、比关档差 160 MB）| **已修**（路线 C：候选区域合并，见 §G8）|

**静态审计**：① 区域没有回收者的程序 = **12**（今天必然非 0 ✓）；② 未记账的引用槽写入 = **0**
（与任务预期相反，见 §4——检测器有牙，但今天这类直接字段写入确实一条不剩）。

---

## 1. 矩阵总表（每一行都有实测数字）

口径：峰值 = `/usr/bin/time -v` 的 Maximum resident set size，量**生成物二进制**（KB/1024 = MB）。
「on 160k→640k」= `EXTC_RECLAIM=1` 两跑；「off」= `EXTC_RECLAIM=0` 两跑。
「sweeps」= 探针观察到的 `extc_rc_collect` 次数（160k / 640k）。
**「收得掉」= on 档 160k→640k 峰值比 < 1.5**。

| # | 形状（shapes/）| A 房间 | B 槽 | C 结构 | D 谁触发 | 收得掉? | on 160k→640k | off 160k→640k | sweeps 160k/640k | 比关档差? | 红判据 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `home_cycle` rounds=1 | 调用者家房间 | 普通字段 | 自环 | **拥有者只调一次** | ✗ **3.04×** | 4.61→**13.98** | 3.92→11.36 | **0 / 0** | **是 +2.6** | 无（建议①l） |
| 2 | `home_cycle` rounds=64/256 | 调用者家房间 | 普通字段 | 自环 | 拥有者反复调 | ✓ 0.98× | 3.17→3.11 | 40.79→158.92 | 31 / 127 | 否 | 现成 ①k 同机制 |
| 3 | `own_room_cycle` | 本帧房间 | 普通字段 | 自环 | 本帧自己 | ✓ 1.00× | 2.92→2.92 | 3.92→11.36 | 2 / 9 | 否 | 现成 ①/①b 同类 |
| 4 | `cross_scope_cycle` | 本帧房间 | 普通字段 | **跨作用域圈** | 本帧自己 | ✓ 0.98× | 2.98→2.92 | 6.36→21.11 | 4 / 19 | 否 | 现成 ① 同类 |
| 5 | `slot_elem_known` | 本帧房间 | **下标槽 `s.kids[0]`** | 圈 | 本帧自己 | ✓ 1.04× | 5.73→5.98 | 16.23→60.29 | 16 / 68 | 否 | 现成 ①d |
| 6 | `slot_elem_unknown` | 本帧房间 | **视图元素 `v[0].kids[0]`** | 圈 | 本帧自己（值被钉住） | ✓ 1.00× | 4.04→4.04 | 16.29→60.29 | 18 / 77 | 否 | 无（**已修**：反查失手 ⇒ 值钉成根；见 §G6a） |
| 7 | `base_call_unknown` | 本帧房间 | **`(*id(s)).kids[0]`** | 圈 | 本帧自己（槽地址反查命中） | ✓ 1.03× | 5.73→5.92 | 16.23→60.36 | 16 / 68 | 否 | 无（**已修**：槽地址反查宿主；见 §G6） |
| 8 | `struct_copy_slot` | 本帧房间 | **整值存储 `s.w=wrap{…}`** | 圈 | 本帧自己 | ✓ 0.99× | 4.67→4.61 | 13.85→50.60 | 19 / 79 | 否 | 现成 ①f |
| 9 | `struct_copy_ret_home` | 调用者家房间 | **按值返回**落具名槽 | 自环 | 拥有者反复调 | ✓ 1.00× | 1.42→1.42 | 1.42→1.42 | - / - | 否 | 无（建议①s） |
| 10 | `carray_slot` | 本帧房间 | **C 定长数组 `arr[i%8]`** | 自环 | 本帧自己 | ✓ 0.98× | 2.98→2.92 | 3.86→11.36 | 2 / 9 | 否 | 无（建议①s） |
| 11 | `owcell_garbage` | **`@overwrite` 格子** | 普通字段 | 自环 | 拥有者反复调 | ✓ 1.00× | 2.92→2.92 | 3.92→11.41 | 2 / 9 | 否 | 无（建议①s） |
| 12 | `coro_frame_cycle` | **协程帧** | 普通字段 | 自环 | **无（不触发）** | ✗ **3.10×** | 4.46→**13.84** | 3.65→11.15 | **0 / 0** | **是 +2.6** | 无（建议①n） |
| 13 | `ext_frame_cycle` | **`ext` spawn 帧** | 普通字段 | 自环 | **无（不触发）** | ✗ **3.13×** | 4.40→**13.78** | 3.65→11.21 | **0 / 0** | **是 +2.6** | 无（建议①n） |
| 14 | `match_payload` | 本帧房间 | **枚举载荷绑定** | 自环 | 本帧自己 | ✓ 0.98× | 2.98→2.92 | 3.86→11.36 | 2 / 9 | 否 | 无（**已修**：§G3） |
| 15 | `par_tls_cycle` | **TLS worker 房间** | 普通字段 | 自环 | 本帧自己（TLS 拥有者帧触发） | ✓ 1.13× | 4.23→4.79 | 3.86→11.79 | 2 / 8 | 否 | 无（**已修**：TLS 区域按运行期深度认领；见 §G4） |
| 16 | `pool_ref_slot` | **池缓冲（视图）** | `pool<mut ref node>` 的槽 | 自环 | **无（`noSweep`+无触发）** | ✗ **2.99×** | 4.67→**13.98** | 3.86→11.41 | **0 / 0** | **是 +2.6** | 无（建议①q） |
| 17 | `dyn_payload` | **dyn 池** | dyn 载荷里的引用 | 自环 | 本帧自己（dyn 池不回收） | ✗ **3.61×** | 22.23→**80.23** | 20.79→78.66 | 2 / 9 | **是 +1.5** | 无（建议①r） |
| 18 | `byval_temp` | 调用者家房间 | **按值返回纯临时量** | 自环 | 拥有者反复调 | ✓ 0.98× | 2.79→2.73 | 3.86→11.42 | 2 / 9 | 否 | 无（建议①s） |
| 19 | `closure_chain` | 本帧房间 | **嵌套闭包环境** | 自环 | 本帧自己 | ✓ 1.04× | 1.42→1.48 | 1.42→1.48 | - / - | 否 | 无（建议①s） |
| 20 | `global_slot`（G9）| 全局量 | — | — | — | **编译期拒** | — | — | — | — | 前端已有 |
| 21 | `ring_per_round`（G8）| 外层循环体（region 1）| 普通字段 | **跨区闭环** | 本帧自己（环被切开 ⇒ 两区互相钉住）| ✓ **1.00×**（修前 3.90× ✗）| 10.61→10.61 | 3.08→3.45 | 13 / 55 | 修前 **+160.7** | §G8 |

`matrix.tsv` 另有 N=40000/160000 的 41 行明细（含 on 档生成物的
`maybeCollect`/`owe`/`checkOwed`/`storeRefUnknown`/`storeRefOwned`/`storeStructCons`/`initRef`
调用点数）。所有 39 个 ASan/UBSan 跑都是 `rc=0`、无 `SEGV/UAF/runtime error`。
**语义**：19 个可运行形状开档/关档 stdout 与退出码**逐字一致**（`sem=一致`）。

> 「比关档差」= on 档峰值比同一形状 `EXTC_RECLAIM=0` 还大 ≥ 0.5 MB——第 1、12、13、16、17 条命中（5 条）。
> 第 6 条（视图元素）**已修**：1.00×、sweeps 18 / 77、比关档差消失，见 §G6a。
> 第 7 条（基表达式带调用的槽）**已修**：1.03×、sweeps 16 / 68，与第 5 条（同形状、只换宿主可证性）1.04× 同档。
> 第 14 条（`match` 绑定引用载荷）**已修**：0.98×、sweeps 2 / 9，见 §G3。
> 第 15 条（TLS worker 房间）**已修**：1.13×、sweeps 2 / 8、比关档差消失，见 §G4。

---

## 2. 逐类证据

> **数字口径**：§2 各节的绝对峰值/比值来自 2026-10-13 的首轮缺口普查；之后回收器改过清扫频率与
> 退场倍数，绝对峰值整体下移（结论不变）。§0/§1 的矩阵与「收得掉?」列已按最新复跑刷新，
> 与 `out/scale.tsv` 同一批数字。

### 2.0 十条嫌疑的确认 / 推翻一览

| 嫌疑（任务原文）| 判定 | 凭据（§）|
|---|---|---|
| 1 拥有者只调一次、被调者在它房间里攒大垃圾 | **确认** | G1：`home_cycle` rounds=1，3.04×、比关档差 +2.6 MB |
| 2 被调者自己是长循环，调用期间没人清 | **确认** | G1 同一形状（`build` 的 while 就是长循环；`sweeps=0`）|
| 3 判不出宿主的引用槽（`a[i]` / 基表达式带调用 / 视图）| **确认（能力损失、非悬垂）→ 两半都已修** | G6：`base_call_unknown` 已修 1.03×（槽地址反查宿主）；G6a：`slot_elem_unknown` 已修 1.00×（反查失手 ⇒ 值钉成根，对照 known = 1.04×）|
| 4 按值传递/返回的临时量携带引用 | **推翻（本形状下收得掉）** | 第 9 行 `struct_copy_ret_home` 1.00×、第 18 行 `byval_temp` 0.98× |
| 5 协程帧 | **确认** | G2：`coro_frame_cycle` 3.10×、`sweeps=0`、比关档差 +2.7 MB |
| 6 `ext`（spawn）帧 | **确认** | G2：`ext_frame_cycle` 3.13×、`sweeps=0`、比关档差 +2.6 MB |
| 7 TLS / 并行（`parTls`）| **确认 → 已修**（首轮新写正向程序实测；按运行期深度认领归属后收得掉）| G4：`par_tls_cycle` 已修 1.13×、`sweeps` 2 / 8、比关档差消失 |
| 8 池/板容器里的引用、`match` 绑定、`dyn` 载荷 | **确认（池 / dyn）+ `match` 已修 + 板未实测** | G5 池 2.99×、G3 match 已修 1.00×、G7 dyn 3.61×；板只存字节 ⇒ §6 |
| 9 `@overwrite` 格子 | **推翻（本形状下收得掉）** | 第 11 行 `owcell_garbage` 1.00× |
| 10 全局量里的引用 / C 层数组里的引用 | **全局：前端拒；C 数组：推翻（收得掉）** | §3（编译期拒）、第 10 行 `carray_slot` 0.98× |

### G1 拥有者只调用一次（嫌疑 1 + 2）——**新洞，比关档差**

形状 `home_cycle.extc`：`build` 在**调用者的家房间**里建自环链（`p.next = p` + `keep` 覆盖），
`build` 自己就是长循环。生成物里 `build` 的分配点**一个触发都不发**，只发
`extc_rc_owe(__extc_…)`（欠收；局部量名随发射形态变，按 `__extc_` 前缀认）；`main` 只在**下一次把家 arena 传给被调者之前**
`extc_rc_checkOwed(__extc_…, …)` 销账。

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
（生成物里 `maybeCollect(__extc_…` = 0、`owe(` = 0，`sweeps = 0`）。
`keep = p` 把自环节点提到帧的函数级区域，只有帧销毁才还。

on 5.96→17.71（2.97×）、off 3.65→11.21；`ext` 5.90→17.71（3.00×）。
ASan 干净（`rc=0`）——纯上界问题，不是悬垂。

### G3 `match` 绑定引用载荷（嫌疑 8）——**已修（1.00×）**

`match_payload.extc`：`var m: maybe = maybe.some(keep)` 这类**整值声明**里的引用藏在判联体负载里 ——
C 的 `union` 让「哪个负载是活的」只有运行期 tag 知道，编译器枚举不出叶子，旧行为走
`storeStructCons(..., 0, 0)` ⇒ **目标区域退出清扫**（`noSweep`，只靠计数回收）⇒ 界随规模涨。
两件事合起来认领完整：

* 根表按**所有**枚举变体把负载引用叶子静态展开成根（`rcRootLeaves` 的 `TY_ENUM` 分支，
  `<base>.u.<variant>._<k>`；读不活跃变体的垃圾字由 `markBegin` 里的 `extc_rc_owns` 过滤 ⇒ 只多留）；
* 存储侧发 `extc_rc_storeStructRooted`（逐字着色 + 只对 rc 槽 `inc`，**不**置 `noSweep`、不登记跨区边）。

⇒ on 2.92→2.92（**1.00×**）、sweeps 2 / 9、比关档差消失；off 3.86→11.42；stdout 与关档逐字节相同。
可枚举不出来的（泛型实参带引用的枚举实例、认不出来的 kind）仍走保守路径（多留方向）。
牙：`tests/reclaim/driver.c` 的 `t_storeStructRooted`（默认 7 / 计数档 8 条确定性断言）+ 注入
`struct-shade-nop`（拿掉逐叶着色 ⇒ 红在「增量标记期间叶子当场标黑」）。
### G4 `parTls` / TLS worker 房间（嫌疑 7）——**已修（1.13×）**

`tests/reclaim-rootstack/RECON.md` 说「全语料没有能编译的正向 `parallel::run` 程序，TLS 一档未实测」。
首轮缺口普查新写了最小正向程序 `par_tls_cycle.extc`，本轮把它修掉：

```extc
fn w(id: i64, lo: i64, hi: i64, out: mut slice<u8>) -> i32 { … keep = p … }
fn main(args) { … parallel::run(w, out, n, i64(2)) }
```

生成物里 `extc_tls_arena` 出现、`extc_arena_alloc(&(*extc_tls_arena), …)` 就是 worker 的 `new`。
根因：`rcFrameOwnsArena` 与 `rcEmitOwe` 对 `parTls` 都直接返回 ⇒ **`maybeCollect`=0、`owe`=0、`sweeps`=0**，
线程本地区域谁都不认领。修法是「按运行期深度认领归属」：

* 运行期加 `static _Thread_local int32_t extc_rc_tlsDepth;`（恒发，不碰 parallel 的程序只有声明）；
* 每个「`parTls` + 需要回收 + 有 arena + 不是协程」的函数序言
  `int __extc_tlsOwner = (extc_rc_tlsDepth++ == 0);`、尾部 `extc_rc_tlsDepth--;` 配对 ——
  静态判据分不出「worker 入口帧」与「内层帧」，只有深度知道谁是这一段调用链的拥有者；
* 触发点按三态发：拥有者 ⇒ 物化根表、当场 `maybeCollect`；内层帧 ⇒ 只 `extc_rc_owe`；
* 线程本地区域随 arena 同寿丢弃两处：`extc_par_close` 里每个 worker arena 前
  `extc_rc_regionDrop(&r->arenas[i])`、worker 兜底销毁局部 arena 前 `extc_rc_regionDrop(&local)`；
* worker 调回调前后保存 / 清零 / 恢复深度 —— 同线内联跑时（线程起不出来、`threads<=1`）入口帧才一定是拥有者。

⇒ on 4.23→4.79（**1.13×**）、sweeps 2 / 9、比关档差消失；off 3.86→11.79；stdout 与关档逐字节相同。
下界现在是 main 里那只 `new u8[n]` 输出视图（**回收本身已经平坦**）；原形状用 `slice<i64>` 输出时会多出
8n 字节底噪、比值下界 1.68× 测不出回收，所以把输出视图改成 `u8`（语义不变，`rc` 照样打印）。

牙：结构门禁 `gate-par-tls-owner`（`tools/check_par_tls_owner.py`，跑真实发射：正向并行形状 + 普通形状，
断言声明恒发 / 序言尾声配对 / 三态 / worker 清零恢复顺序 / 两处区域丢弃，且普通形状一个字节都不多发）
+ 注入 `par-tls-depth-noreset`（拿掉 worker 的清零 ⇒ 同线内联跑时入口帧当成内层帧、区域整片收不掉）。
9 形状 `-O2` callgrind Ir 逐位不变（不碰 parallel 的帧零税）。

### G5 池容器里的引用（嫌疑 8）——**新洞，比关档差**

`pool_ref_slot.extc`：`pool<mut ref node>::withCap(8)` 的 dense 缓冲是一条引用槽数组。
生成物里 `p.insert(nd)` 内部对缓冲槽发 `extc_rc_storeRefUnknown`（池缓冲是视图 ⇒ 宿主追不到）
⇒ **目标区域 `noSweep`**；同时 `new node` 的分配点**一个触发都不发**（`var p = pool<…>` 的声明走
`rcRootDecl`，类型扫描回 `RC_CONSERVATIVE` ⇒ 整函数 `rcRootsUnreliable`，`src/back/codegen.c:3463`）
⇒ `sweeps = 0`、峰值 6.09→17.92（2.94×）、比关档差 +6.6 MB。

### G6 视图元素 / 基表达式带调用（嫌疑 3）——**两半都已修**

- `slot_elem_known`（`s.kids[0] = keeper`，宿主 = `s`）：生成物逐叶 `storeRefOwned`，扫得掉，
  on 5.73→5.98（1.04×）、sweeps 16 / 68；off 16.23→60.29。
- `base_call_unknown`（`(*id(s)).kids[0] = keeper`）：**已修**。槽地址落在 `s` 的数据区里 ⇒
  `extc_rc_storeRefSlot` 按**槽地址**反查宿主 slab（`extc_rc_slabOfInterior`：ownHot/ownMap/ownTab 按
  slab 基址查，校验偏移 < `nslots * ss`）⇒ 走 `storeRefOwnedReg`，不再 `noSweep`。
  on 5.73→5.92（**1.03×**）、sweeps 16 / 68；off 16.23→60.29。与第 5 行同档 ⇒ 差分闭合。

### G6a 视图元素（嫌疑 3 的另一半）——**已修（1.00×）**

- 形状 `slot_elem_unknown`：视图元素 `v[0].kids[0] = keeper`，生成物
  `extc_rc_storeRefSlot((void **)&((*slice_node_index(v,0,…)).kids.data[…]), (void*)keeper)`。
  槽确实落在某个对象的数据区里，但宿主对象是 `v` 指向的元素，而 `v` 来自 `new node[4]` ——
  那块数组走 `extc_arena_alloc`（**不是 rc 槽**）⇒ 槽地址反查必然失手。
- 旧行为：反查失手 ⇒ `extc_rc_storeRefUnknown` ⇒ 目标区域 `noSweep`（永久退出清扫）
  ⇒ on 20.04→71.92（3.59×）、sweeps 4 / 19，比关档（60.29）还差 **+11.6 MB**。
- 修法（把值钉成根）：反查失手时不再让区域退出清扫，改把**值**登记进一张进程级「钉住表」
  （`extc_rc_pinAdd(v)`：键 = 指针、条目记 `ownSlab(v)->region`），起轮时 `extc_rc_pinSeed(r)`
  把本区域钉住的值压栈当种子 ⇒ 它照常被扫到、照常被标，别的垃圾照常收；覆盖旧值时
  `extc_rc_pinSub(old)` 撤销旧边；区域死时 `extc_rc_pinDropRegion(r)` 清死指针。
- 表是**开放寻址哈希**（装载 ≤ 1/2、乘法散列、Knuth 回移删除）：线性扫版本实测 O(n²)
  （5 万 0.26 s → 20 万 5.23 s），改哈希后 5 万 0.10 s / 20 万 0.10 s；`gate-pin-scale` 钉住
  「每钉一个的成本不随表长涨」（4000 项 285.4 Ir、16000 项 235.0 Ir、比 0.82；注入线性预扫立刻红）。
- 实测：on **4.04→4.04（1.00×）**、sweeps **18 / 77**；off 16.29→60.29；比关档差消失。
- 边界：容器宿主仍枚举不出（视图基址来自 arena）⇒ 「槽被覆盖」时我们知道旧值，但不知道还有哪些别的槽
  指着它，覆盖之外的撤销做不到，只能等区域死；`pinAdd` 扩容失败（OOM）才回落 `noSweep`。

### G7 dyn 载荷（嫌疑 8）——已知限制

`dyn_payload.extc`：`dyn Tag(box { v, n: nd })` 的载荷按值拷进 dyn 池。on 24.35→82.29（3.38×）、
off 20.73→78.67；`sweeps` 2/9 但 dyn 池本身不回收（`DYN.md` 记为已知限制：回收推到 place 退出）。
on 仍比 off 差 +3.6 MB。

### G8 可能的环被**区域划分切开**（新根因；**已修**，路线 C）—— §G8

形状 `shapes/ring_per_round.extc`：每轮建一条 **256 结点的闭环**，环头存进长命槽 `keep`；轮数来自
argv（`ring*` 这一族用**轮数**当规模，不是结点数；每轮垃圾 ≈ 1 MiB）。它和前面 5 条的成因都不同：

* 触发**发生了**、清扫也**在跑**（on 档 `sweeps` 13 / 55），但每次收 0 个槽；
* 生成物里环横跨**两只区域**：`head` 在外层循环体（`__extc_rcr0`），256 个 `t` 在内层循环体
  （`__extc_rcr1`）；`(*cur).next = head` 是一条 `r1 → r0` 的跨区边，`head.next = t` 是一条
  `r0 → r1` 的跨区边 —— **两只区域互相钉住**；
* 跨区种子按来源侧记账撤销（`freeSlot` → `unlinkChildren`），而环里的槽**永不释放**
  ⇒ 鸡生蛋：两边的标记都能从种子走到整整一环，`freedSlots = 0` ⇒ 自学习把 `scale` 翻倍、
  阈值涨到 `EXTC_RC_SCALE_MAX`（17 GB）⇒ 该区域**彻底不扫**。

这就是 `docs/RECLAMATION.md:128-130` 早就写下的那条（"把一条链或一个环强行切开，跨片登记位就会
成为环垃圾的种子……界随轮数增长"）：**区域标记精确 ⟺ 划分不切开任何 SCC**。

| 消融（生成物副本）| 250 轮 | 1000 轮 | 规模律 | 健全？ |
|---|---|---|---|---|
| 今天默认 `EXTC_RECLAIM=1`（修前）| 510 MB | 2036 MB | **3.99× 线性 ✗** | — |
| `EXTC_RECLAIM=0`（对照）| 4.1 MB | 7.1 MB | 1.76×（绝对值平）✓ | ✓ |
| 关掉 `rsInc`/`rsIncSlab` 登记 | 5.4 MB | 5.5 MB | 平 ✓ | **故意不健全 ✗**（种子不全）|
| **把两只区域并成一只** | 10.6 MB | 10.6 MB | 平 ✓ | ✓ |

**修法（路线 C）**：*可能闭环*的写 + 宿主可能有**多只区域** ⇒ 把这些候选区域**合并**到最浅的那一只。
判据现成，不用新造：`fnAcyclic` 的扫描把这种写分类成 `SR_BAD`
（"既不是空、也不是新分配、也不是严格更年轻的层"，`EXTC_DBG_ACYCLIC=1` 逐字可复现）。

实现（三段，`src/check/check_escape.c` + `check_stmt.c` + `check_top.c`）：

1. **记录**（`noteCycleMerge`，检查走查里）：`SR_BAD` 且宿主经具名引用到达（`(*cur).f = v` / `p.f = v`）
   时记下 `(宿主绑定, 值)`；同一个站点多趟只记一次。
2. **收候选站点**（同一处，作用域还开着）：只走宿主绑定**自己**可能指向谁的三条来源
   （`origin` / `initSrc` / `heldSrc`）。**不走字段表** —— 那记的是"写进宿主**字段**里的值"
   （从宿主**可达**的东西），当候选会把 `cross_region.extc` 的跨区边也误并掉。
3. **合并**（`applyCycleMerges`，全局电平求解之后、placement 之前）：候选站点落进**不止一只**
   区域时，把它们与值一起并到**最浅**的那一层（`anMinAt`）。只往更长寿的一侧挪（多合并只多留），
   候选里出现家区域就**不动**（`needsHome` 已定，不许在后段改）。
   入口：`check_top.c` 的 `applyCycleMerges(&c)`（求解之后、placement 之前）；
   `applyCycleMerges` 在 `EXTC_RECLAIM=0` 时直接返回 —— 没有回收运行期就没有区域划分，
   关档产物逐字不变（关档是整套对照的基线）。

**实测（2026-10-13，本轮）**：

| | 20 轮 | 80 轮（4×）| 判据 < 1.5× |
|---|---|---|---|
| on（修前）| 42.11 MB | 164.17 MB | **3.90× ✗** |
| on（修后）| 10.61 MB | 10.61 MB | **1.00× ✓** |
| off（修前/修后**逐字相同**）| 3.08 MB | 3.45 MB | 1.12×（对照不瞎 ✓）|

on 档清扫次数从 3/3（收了 0 个）变成 **13/55**（真的在收）；stdout 与关档逐字一致；ASan 0。

**判据与棘轮**：`run-gaps.sh` 的 SCALE 段加一行 `ring_per_round 20 80`，`gate.py` 的已知坏基线
`gate-known-bad.txt` 修前登记 `ring_per_round`（今天必然红），**修后已删掉该条**（棘轮只许缩）。
**红向注入（去掉修法）** —— 手动形态（`tools/inject.py` 不在本轮的写范围里）：删掉
`src/check/check_stmt.c` 里 `if (kind == SR_BAD) { … noteCycleMerge … }` 那一段，重编，跑判据：

```sh
OUTDIR=/tmp/gaps-inject bash tests/reclaim-gaps/run-gaps.sh
python3 tests/reclaim-gaps/gate.py --scale /tmp/gaps-inject/scale.tsv --raw /tmp/gaps-inject/raw.txt
```

原样输出（2026-10-13；注入点就是上面那一段）：

```text
ring_per_round	42.11	164.17	3.90	3.14	3.51	3	3	**收不掉(随规模涨)**
[reclaim-gaps] sanitizer：0 条失败，与基线一致（0 条）
[reclaim-gaps] 新出现的缺口 1 条（收不掉的形状变多了）：ring_per_round
gate exit=1
```

还原后 `build/extc` 的 md5 与注入前**逐字相同**（`0a0fb29dc54ff8c97e7a293f7dd14aa9`），
`gate.py` 复绿（`收不掉 9 条，与当时的基线一致`）。

**这条路没覆盖的**（说清楚，别把"没测到"读成"没问题"）：

* 候选集只走 `origin`/`initSrc`/`heldSrc`：一个绑定被**多次改指**时只记得住当前值与声明来源，
  更早的目标站点收不到 ⇒ 那一档不合并（方向安全：保留原状，不会放掉东西）。
* 候选里出现**家区域**时整条记录跳过（`needsHome` 在合并之前已关死）；这类形状的可能环仍可能
  被切开，界仍随规模涨 —— 未实测具体形状。
* **值**那边只调一次 `promoteInto(value, m)`；值若是调用结果（`SR_CALL` 不触发本路径）或跨函数
  来源，提不动时保持原状（安全方向）。
* 只在**单线程 `main`** 上实测；分片（`EXTC_RC_SHARDS`）、TLS/coroutine 形状未测。

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

实测（1329 个 `.extc`；编译成功 916，失败 413）：

| 类 | 程序数 |
|---|---|
| ok-trigger | 121 |
| owe-settled | 109 |
| owe-no-settle | 0 |
| **silent-both** | **12** |
| no-alloc | 620 |
| **红计数①** | **12** |

12 个程序（= 今天必然非 0 的那一半）：

```
tests/coro/coro_new.extc                                alloc=1 trig=0 owe=0 check=0 srUnknown=1
tests/escape-fuzz/cases/m_home_cont_after.extc          alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/escape-fuzz/cases/m_home_cont_before.extc         alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/escape-fuzz/cases/m_main_cont_after.extc          alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/escape-fuzz/cases/m_main_cont_before.extc         alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/escape-fuzz/cases/m_nohome_cont_after.extc        alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/escape-fuzz/cases/m_nohome_cont_before.extc       alloc=1 trig=0 owe=0 check=1 srUnknown=1
tests/reclaim-gaps/shapes/coro_frame_cycle.extc         alloc=2 trig=0 owe=0 check=0 srUnknown=1
tests/reclaim-gaps/shapes/ext_frame_cycle.extc          alloc=2 trig=0 owe=0 check=0 srUnknown=1
tests/reclaim-gaps/shapes/pool_ref_slot.extc            alloc=1 trig=0 owe=0 check=0 srUnknown=1
tools/escape-corpus/P2-6_container_uaf.extc             alloc=1 trig=0 owe=0 check=1 srUnknown=1
tools/escape-corpus/P2-7_container_before_uaf.extc      alloc=1 trig=0 owe=0 check=1 srUnknown=1
```

### ② 引用槽写入没有对应记账调用

用生成物自带的 `ExtcRcType` 偏移表（`__extc_rcoN[]` 的 `offsetof(T, f)` ⇒ 哪些字段是引用叶子）
+ `struct T { … }` 定义（字段 → C 类型）做**类型感知**检测：LHS 字段链逐段解析，
只有**所有可能类型**都落到 `T *`（T 是回收管辖的结构）才算候选。宁可漏报，不虚报。

**实测：② = 0**（916 个程序里一条直接字段写入都没有漏记账）。
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

**②′（补充计数）**：走 `storeRefUnknown`（目标区域运行期 `noSweep`）的程序 = **242**、
走 `storeStructCons(..., 0)` 保守分支的程序 = **10**（并集 242；机器数自 `out/audit-static.tsv` 的 `srUnknown`/`srCons0` 列）。这不是 bug 计数，
是「编译器主动放弃了这个区域的清扫」的规模，与 §1 第 16 条直接对应（第 6 条已改成值钉成根、
第 7 条已改成槽地址反查、第 14 条已修：`storeStructRooted` 取代了那条保守存储）。

---

## 5. 建议的判据（**补丁片段，未落地**）

### 5.1 接进 `tests/reclaim-shapes/run-switch.sh`（形如 ①l…①s）

```sh
# ── 建议追加（未落地）────────────────────────────────────────────────────────
# 形状都在 tests/reclaim-gaps/shapes/；判据一律：开档 N vs 4N 峰值比 < 1.5，
# 且「清扫次数 > 0」（用 tools/rc-pause-probe.py，与 ①k 同一把尺子）。
#  ①l home_cycle  rounds=1   1:160000 / 1:640000   今天 3.04×（红）—— 拥有者只调一次
#  ①m slot_elem_unknown                            已修 1.00×（绿）—— 反查失手 ⇒ 值钉成根
#     base_call_unknown 已修 1.03×（绿）—— 槽地址反查宿主
#     对照 slot_elem_known 必须 1.04×（绿）⇒ 差分闭合
#  ①n coro_frame_cycle / ext_frame_cycle           今天 3.10× / 3.13×，sweeps=0（红）
#  ①o match_payload                                已修 1.00×（绿）—— 枚举负载整值存储 + 按变体展开根
#  ①p par_tls_cycle                                已修 1.13×（绿）—— TLS 区域按运行期深度认领
#  ①q pool_ref_slot                                今天 2.99×，sweeps=0（红）—— 池里的引用
#  ①r dyn_payload                                  今天 3.61×（红）—— dyn 池（已知限制）
#  ①s（护栏，防回归）struct_copy_ret_home / carray_slot / owcell_garbage /
#     byval_temp / closure_chain 今天都 ≤ 1.05×（绿）；加上它们是为了
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
* **`owe-no-settle` = 0** 是 916 个程序的实测；语料里 `owe` 与 `checkOwed` 几乎成对出现，
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
* 语义：开档/关档 stdout + 退出码逐字比（39/39 个可编译 spec 一致）。
* 正确性：39 个 ASan/UBSan 跑全 `rc=0`，无 `SEGV/UAF/runtime error`。
* 静态审计：`-w`（**不带** `--no-line-map`，要 `#line` 划出用户代码段）。
* 本目录不改 `run-switch.sh` / `check.sh`（§5 只是建议片段）；形状本身只在「口径站不住、测不出回收」时才改
  （`par_tls_cycle` 的输出视图从 `slice<i64>` 换成 `slice<u8>`，见 §G4）。

## 8. 文件

| 文件 | 作用 |
|---|---|
| `README.md` | 本文件（矩阵 + 结论 + 建议判据）|
| `run-gaps.sh` | 一键复跑：矩阵（41 行）+ 规模判据（20 行）+ 每形状 ASan 结论，**恒 exit 0** |
| `gate.py` | **棘轮闸门**：① 每个形状的 `asan=<非 0>`；② 规模判据里"收不掉"的集合。两者都"多一条红、少一条也红" |
| `gate-known-bad.txt` | ① 的基线（当前 5 条收不掉的形状）|
| `asan-known-bad.txt` | ② 的基线（当前**没有已知坏**：所有 ASan/UBSan 跑都是 rc=0）|
| `audit-static.py` | 全语料静态审计 ①②，`--gate` / `--selftest` / `--baseline`（棘轮）|
| `ledger-gate.py` | 把静态审计按固定口径跑成闸门（`gate-reclaim-ledger` 的入口）|
| `audit-known-bad.txt` | 静态审计棘轮的基线（当前 12 条没有回收者的程序）|
| `shapes/*.extc` | 20 个形状（+1 个编译期反例），每个 ≤ 60 行 |
| `out/matrix.tsv` `out/scale.tsv` `out/raw.txt` | 原始数字 |
| `out/audit-static.tsv` `out/audit-static-summary.txt` | 静态审计原始输出 |
| `out/<形状>.on.c` | 开档生成物：`run-gaps.sh` 现场写，**不入库**（要看就自己跑一遍）|

> `gate.py` 跑矩阵时用 `OUTDIR=<临时目录>`，**不覆盖**上面那些入库的 `out/*`（想刷原始数字就直接跑
> `run-gaps.sh`）。手动复用现成结果：`gate.py --scale out/scale.tsv`（同目录的 `raw.txt` 会给
> sanitizer 那一段用）。
>
> 入库的是**记录**（`matrix.tsv` / `scale.tsv` / `raw.txt` / `audit-static*.tsv`），不入库的是**转储**
> （每个形状的 `.on.c`）：后者跑一遍就被重写，留着只会让每次复跑都欠一笔 `git checkout --`。
