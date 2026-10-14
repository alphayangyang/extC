# `tests/reclaim-acyclic/` —— 「程序无环 ⇒ 事件档（计数、不扫）」的**动态判据**

配套机制文档：[`docs/RECLAMATION.md`](../../docs/RECLAMATION.md)（跳过表 `skip:sweepfree`）。
上游事实：[`prototype-reclaim/README.md`](../../prototype-reclaim/README.md) 的 `ev` 档
（0 次清扫 / 0 停顿 / 内存 12 KiB）；对照形状：[`tests/reclaim-shapes/`](../reclaim-shapes/README.md)。

## 这个目录的语义

**这是门禁，不是证据脚本。** 它钉的是改动方向的两侧：

> 程序被判定**无环**时，用计数（事件路径）并让 `shouldSweep` 恒 0 ⇒ **清扫次数必须为 0**。
> 判错的**危险方向**是反的：有环区域的**环**垃圾永远收不掉 ⇒ **峰值 RSS 涨**（不是悬垂）。

| 文件 | 是什么 |
|---|---|
| `sweeps.sh` | **量具**：一个 `.extc` 的「清扫次数 / 峰值 RSS / stdout / 退出码」。不做断言。 |
| `gate-acyclic.sh` | **门禁**（`check.sh` 的 `gate-acyclic` 项）：7 格判据，见下。 |
| `acyclic_heapref.extc` | 成功判据的形状：无环 + **引用住在堆槽里**。 |
| `blind_localhold.extc` | 事件路径的**能力**盲点形状：引用**只住在局部量里**。②判它 idle ⇒ 默认档整段不发射 rc（④ 已不红，界由 `gate-reclaim-idle` 看着）。 |
| `known-red.txt` | 已知红基线（棘轮）：预期红的格必须在里面；已不红的条目会让门禁变红。 |

## 档的定义（只有三个开关，都是现成的）

| 档 | 开关（编译期环境变量） | 语义 |
|---|---|---|
| 默认档 | 不设 | 不计数 + 区域清扫（今天的行为） |
| **事件档** | `EXTC_RC_SWEEPFREE=1` | 打开计数 + `shouldSweep` 恒 0（"完全不扫、只靠计数归零当场回收"） |
| 无回收基线 | `EXTC_RECLAIM=0` | stdout 的语义参照 |

`EXTC_RC_TIME=1` 只用于**量具**：让生成物在退出时把每个回收组件的**调用次数**打到 stderr
（`[rccount] collect calls=N`）。`sweeps.sh` 里"计数那一趟"和"纯跑那一趟"是两次独立编译，
所以 RSS/stdout 不会被计数代码污染。

`EXTC_RC_SWEEPFREE` 是 Lead 在 `src/back/codegen.c` 接的线（默认关、关掉时产物逐字不变）。
**本门禁只认这三个开关与 RSS/清扫次数，不依赖 `fnAcyclic`** —— 分析侧（另一个成员）落地后，
只需要把门禁①的"输入"从 `EXTC_RC_SWEEPFREE=1` 换成"编译器自己判无环"，判据本身一个字不动。

## 七格判据

### ① 成功判据（无环 + 事件档）—— `acyclic_heapref.extc N=200000`

* **①a** 事件档清扫次数 **== 0**（并且必须真的量到了 `[rccount]` 行："没量到"≠"0"）；
* **①b** stdout 与 baseline（`EXTC_RECLAIM=0`）**逐字相同**（sha256 比对整份 stdout）；
* **①c** 事件档峰值 RSS **≤ 同形状默认档 RSS × 1.5**。

形状为什么是"引用住在**堆槽**里"：现有事件路径只数「rc 槽 → 对象」这条边。每轮
`head.next = new node` 把新对象存进堆字段、覆盖上一个 ⇒ 上一个最后一个持有者消失 ⇒
计数归零当场释放 ⇒ 不扫也不涨。**不要拿 churn/rebuild 型（引用在局部量里）当成功样例**，
那是第④格的红。

### ② 上界判据（有环形状不得被判成事件档）—— `cyclic_pressure.extc N=800000`

* **②a** 默认档清扫次数 **> 0**（判据不许瞎：有环形状必须还走清扫档）；
* **②b** 旁证**开关非空转**：同一形状默认档 collect=28 → 事件档 collect=0。

### ③ 红证明（把事件档强制施加到有环形状上 ⇒ ①的三条必须 FAIL）

把①的三条原样施加到 `cyclic_pressure` 的**强制事件档**上：清扫那一半会"通过"（正是误判的
形态）、stdout 一致，但 **RSS 那一半必须 FAIL**（实测 `64.79 MB / 7.23 MB = 8.96×`）。
若它反而通过 ⇒ 这条判据抓不住误判 ⇒ 本门禁自己红。

> 这条红证明在**门禁内**用 `EXTC_RC_SWEEPFREE=1` 强制；**同一件事**在编译器层由
> `tools/inject.py acyclic-always` 强制（把判定写成恒真），那时是第②格变红。两条独立路径。

### ④ 盲点棘轮（`blind_localhold` 已不红）—— `blind_localhold.extc N=2000000`

现有事件路径**只数「rc 槽 → 对象」这条边**：引用只住在局部量里的对象 `hold` 恒 0 ⇒
**永不触发释放** ⇒ 事件档下内存随轮数涨（历史上实测 `48.86 MB / 4.86 MB = 10.06×`）。

**这一格现在已不红，条目已从 `known-red.txt` 删掉**（棘轮只许缩）：②（"这个区域不需要回收"
的静态证明）把 `blind_localhold` 判成 idle ⇒ 默认档**整段不发射 rc 运行期** ⇒ 没有"事件档"
可比。它的界不再靠这一格看，而是 `gate-reclaim-idle` 的 N vs 4N 峰值界（实测 rc 关掉
**1452 KB 平**，比开档 4.86 MB 更低）。

**这一点仍是"能力"上的盲点**：`EXTC_DBG_ACYCLIC=1` 把 `blind_localhold` 判成 **fn=0**
（理由 `iv:`——嵌套块里的引用型局部量），所以事件路径本身仍然覆盖不到"引用只住在局部量里"
的对象；②绕开它是靠"根本不发运行期"。真实语料那一档（`churn` / `rebuild`）仍在基线里。

真实语料对照（默认**不跑**，`GATE_ACYCLIC_HEAVY=1` 才跑；都进 `known-red.txt`）：

| 形状 | 默认档 | 事件档 | 比值 |
|---|---|---|---|
| `bench/gc/src/churn.extc` | 5 100 KB / collect 488 | **831 852 KB** / collect 0 | 163× |
| `bench/gc/src/rebuild.extc` | 5 040 KB / collect 307 | ~521 MB | ~101× |

### ⑤ 端到端（真实路径，**绿**）—— `tests/reclaim-shapes/hole_c/hole_c.extc`

①③④ 都用 `EXTC_RC_SWEEPFREE=1` **人工选档**，测的是"事件档运行期对不对"；这一格**不设任何
变量**，测"分析（`planFnAcyclic`）+ codegen 接线（`unitAllAcyclic`）有没有真的把被判无环的程序
切过去"。没有它，接线整条断掉也看不出来（①照样绿）。

判据：**a**/**b** 是**两种等价证据**（`EXTC_RECLAIM=1` 是"允许回收 / 不强制发射"）——
或默认编译 collect == 0（且 `[rccount]` 量到了、生成物里 `#define EXTC_RC_SWEEP_FREE 1`），
或**②生效**：默认档用户区 **0 个 rc 调用点** ⇒ 整段不发射回收运行期（这一支下"清扫次数不可信"
显式跳过：没有运行期就没有清扫可谈）；**c** stdout 与 `EXTC_RECLAIM=0` 基线逐字相同；
**d** 真实路径 RSS ≤ `EXTC_RC_SWEEPFREE=1` 强制档 ×1.5（同一档的两个入口应当等价）。

实测：`hole_c` 走**②那一支**（用户区 0 个 rc 调用点、产物与关档逐字节相同）、stdout 与基线一致、
RSS 3.67 MB / 强制档 3.67 MB（1.00×）。`⑥` 的 `acyclic_heapref` 走另一支，所以两条分支都被活着
证明（`SWEEP_FREE=1`、collect=0）。

### ⑥ 端到端（真实路径，**绿**）—— `acyclic_heapref.extc N=200000`

同 ⑤ 的四条判据。这个形状用 `io` 打印 ⇒ 会拉出 `extc_cout_put/flush` 的 **extern 声明**。
**曾经的 bug（已修）**：`collectEmitted` 会把无体的 extern 声明也推进 `g->funcs`，而
`planFnAcyclic` 对"没有函数体"者恒 false ⇒ `unitAllAcyclic` 把整个单元否掉（现象：程序级诊断
`all=1`，产物却是 `SWEEP_FREE=0`）。**修法**（Lead，`src/back/codegen.c`）：`unitAllAcyclic`
跳过 `!f->body` 的函数 —— extern 没有体、不会去写我们的引用槽，它拿到的引用受调用点
逃逸/借用约束 ⇒ 无法让区域内对象变成不可达。承重假设写在代码注释里，判错方向是"该扫却没扫
⇒ 内存涨"，由 ⑦ 的安全格看住"别放宽过头"。

实测：`SWEEP_FREE=1`、collect=0、stdout 与关回收基线逐字相同、RSS 1.42 MB（强制档同值 1.42 MB）。

### ⑦ 安全格（真实路径）：**该拒的必须拒** —— `SWEEP_FREE=0` 且 `collect > 0`

修法放宽了 extern，必须由这一格看住"别放宽过头"。两个形状、两种"看起来像无环但必须拒"的成因：

| 形状 | 规模 | 为什么必须拒 | 实测 |
|---|---|---|---|
| `cyclic_pressure.extc` | N=800000 | 真的有环（退役链 next/prev 互指） | `SWEEP_FREE=0`、collect=28 |
| `scale_growth.extc` | N=800000 | `p.next = saved`：值不比宿主年轻（回边） | `SWEEP_FREE=0`、collect=12 |

**`blind_localhold` 已从这一格摘掉**：②（"这个区域不需要回收"的静态证明）判它 idle ⇒ 默认档
整段不发射 rc 运行期 ⇒ 既没有 `SWEEP_FREE=0` 也没有 `collect > 0` 可断言。它不是"漏拒"，而是
**不再需要拒** —— 它的界由 `gate-reclaim-idle` 的 N vs 4N 峰值界看着（rc 关掉 1452 KB 平）。

**为什么没有 `bench/app/session.extc`**（Lead 点名的第三个形状）：它默认**根本不发回收运行期** ——
生成物里 `extc_rc_` 出现 0 次、`EXTX_RC_TIME=1` 下 `[rccount]` 一行都没有（即使
`EXTC_RECLAIM=1` 强制打开，`extc_rc_alloc(` 也只有定义没有调用点）⇒ 没有"`collect > 0`"可断言，
拿它当安全格会得到一个空转的格子。改用了 `scale_growth`（有真分配、真实路径下确实被拒）。

## 怎么跑 / 复现

```bash
bash tests/reclaim-acyclic/gate-acyclic.sh                     # 7 格（≈13 s）
GATE_ACYCLIC_HEAVY=1 bash tests/reclaim-acyclic/gate-acyclic.sh  # 另跑 churn/rebuild（≈+20 s、峰值 ~812 MB）
./check.sh --only gate-acyclic                                 # 走电池（已登记，见下）
bash tests/reclaim-acyclic/sweeps.sh tests/reclaim-acyclic/acyclic_heapref.extc 200000
EXTC_RC_SWEEPFREE=1 bash tests/reclaim-acyclic/sweeps.sh tests/reclaim-acyclic/acyclic_heapref.extc 200000
python3 tools/inject.py acyclic-always                         # 注入验证（红 + 自动还原 + 复绿）
```

注入 `acyclic-always` 的实测输出（原样，2026-10-13）：

```text
== 注入 acyclic-always：…（期望 gate-acyclic 红）==
   打补丁 src/back/codegen.c
   ok：注入后 gate-acyclic 变红（FAIL ②a 有环形状默认档清扫次数 = 0，要求 > 0 —— **它被判成事件档了**（判据瞎了））
   还原：逐字相同 ✓
   ok：还原后 gate-acyclic 复绿 ✓
   原树核对：未被改动 ✓
```

同一件事在门禁内的等价代理（不重编编译器）：`EXTC_RC_SWEEPFREE=1 bash
tests/reclaim-acyclic/gate-acyclic.sh` ⇒ ②a 原样红、门禁 exit 1。

可调旋钮（都有默认；判据口径不许放宽，只许调规模）：`N_ACY` `N_CYC` `N_BLIND`
`MIN_BLOWUP`（默认 5）`BOUND`（默认 1.5）。

### 挂进 `check.sh`（**已登记**）

`tools/check.py` 里的项（Lead 加，2026-10-13）：

* **项名**：`gate-acyclic`，分组 `gate`
* **命令**：`bash tests/reclaim-acyclic/gate-acyclic.sh`
* **tier**：`nobench`（`bench=False`；含故意打爆内存的红证明格，不挂 `smoke`）
* `ok_msg` 认末行 `通过 N，失败 M`；`bad_dump` 取尾 14 行

登记后 `python3 tools/check_gate_wiring.py` 转绿（登记前它正是因为本脚本"不在电池可达闭包里且
没自报 `# manual:`"而红）。实测 `./check.sh --only gate-acyclic` ⇒ `通过 18，失败 0`（≈13 s）。

### 规模怎么定的（实测，别照抄"小 N"直觉）

红证明的比值**随 N 涨**（默认档被清扫压平在 ~7.4 MB，事件档随轮数线性涨）：

| `cyclic_pressure` N | 默认档 RSS | 事件档 RSS | 比值 |
|---|---|---|---|
| 50 000 | 6 444 KB | 6 636 KB | **1.03×**（太小，红证明不成立） |
| 200 000 | 7 404 KB | 18 348 KB | 2.48× |
| 400 000 | 7 468 KB | 34 284 KB | 4.59× |
| **800 000** | **7 468 KB** | **66 348 KB** | **8.89×** ✓（默认） |

所以 `N_CYC=800000`、阈值 `MIN_BLOWUP=5`：在任何机器上比值都足够大，峰值 ~66 MB 不会打爆 CI。
盲点格同理，取 `N_BLIND=2000000`（~49 MB、10.06×）。

## 与接线的接口（现状：分析侧 + codegen 接线都已落地）

* 门禁①的输入 = `EXTC_RC_SWEEPFREE=1`（人工选定事件档）；
* **现状**：`planFnAcyclic`（`src/plan/plan.c`）与 `EXTC_DBG_ACYCLIC=1` 已落地；
  `src/back/codegen.c` 接了 `unitAllAcyclic(g, m)`（**跳过无函数体的 extern**，见 ⑥；其余每个
  被用函数 `planFnAcyclic` + 全局初始化式没有回收管辖分配 ⇒ 保守否），在 `scanUnitReclaim`
  之后置 `rcSweepFree=1; rcNoCount=0`。`EXTC_RC_SWEEPFREE=1` 仍是**强制覆盖**，①③④继续用它。
* **真实路径两端都通了**：⑤ `hole_c` 与 ⑥ `acyclic_heapref` 都 `SWEEP_FREE=1` / collect 0 /
  stdout 与关回收基线一致；⑦ 三个"该拒"的形状仍 `SWEEP_FREE=0` / collect > 0（看住 extern 放宽
  别过头）。接线的历史 bug（extern 无体被算进 `g->funcs`）已在 ⑥ 一节记录。
* `tools/inject.py acyclic-always` 打在**分析侧**：`src/plan/plan.c` 的 `planFnAcyclic`
  声明+返回两行（全文件唯一），强制 `return true`。这样走的是**整条链**（`unitAllAcyclic`
  读它）⇒ 默认档下有环形状也被判成事件档 ⇒ ②a 与 ⑦ 一起变红。实测输出见上面「复现」一节。
* `tools/inject.py acyclic-always` 打在**分析侧**：`src/plan/plan.c` 的 `planFnAcyclic`
  声明+返回两行（全文件唯一），强制 `return true`。这样走的是**整条链**（`unitAllAcyclic`
  读它）⇒ 默认档下有环形状也被判成事件档 ⇒ ②a 变红。实测输出见上面「复现」一节。

## 未覆盖（说清楚，别把"没测到"读成"没问题"）

* **只覆盖了 5 个形状**（`acyclic_heapref` / `hole_c` / `cyclic_pressure` / `blind_localhold` / `scale_growth`），
  外加可选的真实语料 `churn` / `rebuild`。语料里"无环但引用在堆槽"的其他形态没有逐个跑。
* **没有单独验证真实的无环判定**：①③④三格的输入仍是 `EXTC_RC_SWEEPFREE=1`（人工选定事件档），
  不依赖分析侧；②格量**默认档**（有环形状必须 collect>0）；⑤⑥量**默认档下的真实路径**
  （`⑤` 走"②生效 ⇒ 整段不发射 rc"，`⑥` 走"切进事件档"—— 两条分支都活着）；⑦量**默认档下
  该拒的两个形状**（都仍被拒）。`fnAcyclic` 的
  **精确度**（哪些程序判无环、理由）由 `task-2` 的覆盖表负责，本目录不测。
  旁证（**由 analysis-acyclic 实测，不是我复跑的**）：`acyclic_heapref.extc` ⇒ `main fn=1
  reason=ok`；`cyclic_pressure.extc` ⇒ `fn=0 reason=iv:`；`blind_localhold.extc` ⇒
  `fn=0 reason=iv:`（嵌套块里的引用型局部量 —— 就是第④格那条盲点的成因）。行号会随形状
  注释漂，这里不抄。
* **没有测根引用计数落地后的行为**：`blind_localhold` 已因②转绿（不再发运行期），但"事件档
  本身对所有无环程序有界"这件事没有被形状证明（只有 `hole_c` / `acyclic_heapref` 两个正向
  样例 + `gate-reclaim-idle` 的界）。
* **⑦ 只覆盖 2 个"该拒"的形状**：没有覆盖"泛型实例方法体里写引用槽"（`session` 那条线——
  它默认连回收运行期都不发，见 ⑦）；extern 放宽的承重假设（"extern 拿到的引用受调用点
  逃逸/借用约束"）没有被形状直接检验，只被 ⑦ 的"别放宽过头"间接看住。
* **多线程 / 协程 / FFI 形状**没测：本目录只有单线程 `main`。
* **`EXTC_RC_SWEEPFREE` 与分片（`EXTC_RC_SHARDS`）、无计数档（`EXTC_RC_NOCOUNT`）的交互**没测。
* 比值阈值（`MIN_BLOWUP=5`、`BOUND=1.5`）来自本机当天的实测；换机器只需保证**比值**量级不变
  （绝对值已全部避开）。
