# `tests/reclaim-perf/` —— 回收运行期"慢在哪"的归因与优化账

配套：[`tests/reclaim-shapes/README.md`](../reclaim-shapes/README.md)（行为基线：回收对不对）、
[`docs/RECLAMATION.md`](../../docs/RECLAMATION.md)（机制与待做清单）、
[`prototype-reclaim/stress.c`](../../prototype-reclaim/stress.c)（C 原型，对照的基准侧）。

**这个目录只做归因，不做门禁**（同 `reclaim-shapes` 的口径：证据，不是判据；不接进 `check.sh`）。
所有脚本只读形状源、只改 `/tmp` 里**生成物**的副本，`src/` 一个字节都不动。

---

## 0. 结论先行

在 `tests/reclaim-shapes/cyclic_pressure.extc`（有环 + 大压力，N=800000，与 C 原型同参）上：

| # | 成分 | 指令数 | 占开关开那一跑 | 测法（都是实测） |
|---|---|---|---|---|
| **1** | **根表物化**（每个分配点当场建 ~1412 项复合字面量） | **2.473 G** | **56.1 %** | Δ(`on` − `guardfirst`)；Δ(`on` − `late`) 校核 |
| 2 | `owns` 守卫（`ownSlab` + `ownLower`） | 1.016 G | 23.0 % | 逐函数自身指令数 |
| 2a | ├ 其中**二分** `ownLower` | 0.525 G | 11.9 % | 逐函数；Δ(`on` − `guardfast`) = 0.121 G（N=2e5）校核 |
| 3 | 计数（`holdUpd` + `storeRefOwned` + `regionOf*` + `initRef`） | 0.473 G | 10.7 % | 逐函数；`nohold` 消融给上界 17 %（N=2e5，会改内存行为） |
| 4 | 标记 + 清扫 | 0.121 G | 2.7 % | Δ(`on` − `nosweep`，清扫永不触发) |
| 5 | 分配 / 释放 / 摘边 | 0.159 G | 3.6 % | `alloc` + `unlinkChildren` + `freeSlot` + `memset` |
| 6 | 其它（`maybeCollect` 非清扫部分、`markPush`、libc、程序自身逻辑） | 0.167 G | 3.8 % | 残差 |
| — | **合计** | 4.408 G | 100 % | |
| 参考 | 程序自身（开关关，全进程） | 0.116 G | 2.6 % | `off` 一跑的总量 |

> 第 1、4 行是**差分**口径，自带它们那部分守卫/libc 份额 ⇒ 六行**不是严格互斥**，
> 这是近似归因、不是恒等式（N=2e5 上残差 0.1 %，N=8e5 上 3.8 %；见 §3.2 末尾）。

**一句话**：慢的不是回收算法（标记+清扫 2.7 %），是**每次分配都当场把整帧的引用槽摊成一张
根表**（56 %）和**每次指针赋值都要问一句"这指针是 rc 槽吗"**（23 %）。前者可以近乎零成本地
拿掉（把物化推迟到真要扫的那一刻），后者要靠登记表查法与静态化分两步吃。

> **落地状态**：第 1 条（入口守卫）**已经落进 `src/back/codegen.c`**，判据 `gate.sh` 先红后绿，
> N=2e5 开关开实测 **−56.5 %**。本文 §1–§4 的所有数字都是**落地前**的归因（绑在 `build/extc`
> md5 `5e51d77f…` 上）；落地后的数字与复跑见 §0 第 1 条、§5.1 第 1 行、§7.4 的 `gate.sh` 与 §8 末条。
>
> 第 2 条（`owns` 登记表查询 O(1) 化）**也已经落地**（`src/back/reclaim.c` 的运行期文本），
> 落地后把 `gate.sh` 的线收紧到 **425 000 000**：N=2e5 **404 358 444 Ir**（相对入口守卫那一版 −20.8 %）；
> 真实形状 `bt` **1120 → 640 ms**、`rebuild` **420 → 360 ms**。§5.1 第 2 行与 §9 已按落地后的状态更新。
>
> 第三刀（**赋值路径上每个指针只查一次登记表**）也落了：`storeRefOwned` 复用 slab、记账走
> `*Slab` 变体、命中路径去冗余比对 ⇒ `gate.sh` 的线再收到 **356 000 000**，N=2e5
> **339 186 860 Ir**（相对第二刀 −16.1 %）。真实形状 `rebuild` 360 → **320 ms**；
> **`bt` 的墙钟 610 → 610 ms 没动**（Ir 只 −3.8 %）—— 见 §9 的说明。
>
> 第六刀（**`rsIncN==0` 时跳过「撤跨区边」的走查**）：`cyclic_pressure` **279 224 856 Ir**
>（线收到 **294 000 000**）；`bt` −5.3 %、`rebuild` −10.0 %；`bt` 的税 **1.910×**。
>
> 第四刀（**热路径小助手一律 `static inline`**）也落了：`slabOf`/`slotOf`/`bit`/`word`/`ownSlab`
> ⇒ `gate.sh` 的线收到 **324 000 000**，N=2e5 **308 485 778 Ir**（相对第三刀 −9.1 %）。
> 三个形状的 Ir 一起降（`bt` −10.7 %、`rebuild` −9.9 %），但**墙钟几乎不动**
> （`bt` best-of-9 610 → 570 ms 而均值持平，`rebuild` 330 → 330 ms）。
> 原因见 §10 的消融：**墙钟确实由指令决定，但不由"我们前四刀减掉的那些指令"决定** ——
> `bt` 的墙钟差几乎全在 `initRef` 里那一行计数上；把回收入口的总 Ir 降 9 % 而没碰到它，
> 墙钟自然不动。（早先这里写过"这三个形状是内存受限"，那是从"Ir 降了墙钟没动"推出来的，
> **已被消融推翻**：见 §10 的更正。）

**1412 项根表**从哪来：`world` 这个帧内结构体的**全部 `ref` 槽**——`cache[1024]` + `tree[256]`
+ `bkHead[64]` + `bkTail[64]` + `last`/`idxHead`/`retire` = **1411**，两个站点再加当轮新分配的
局部 `p` ⇒ **1412**。编译器是**保守**发的：不管那个槽此刻有没有值，整条数组都进根表。

**"只装此刻活着的根"救不了**：稳态下根表**本来就是满的**（N=8e5 时站点 0/2 平均
1354/1412 = **95.9 %** 非空；永久死槽只占物化总量的 **2.3 %**）。
所以正确的做法是**别在分配点建表**，而不是"建小一点的表"。

**下一步动作（按 收益/成本 排）**：

1. ~~**分配点入口守卫**（`if (shouldSweep(r))` 再建表）~~ —— **已落地**（`src/back/codegen.c`
   `rcEmitTrigger`），收益 **56.5 %**（N=2e5 实测 1 169 388 408 → 508 581 405 Ir），
   判据 [`gate.sh`](gate.sh)（改动前红、改动后绿）。§5.1 及 §8 的对应条目已按落地后的状态更新。

2. ~~**`owns` 登记表查询 O(1) 化**（直接映射 + 校验，碰撞回落二分）~~ —— **已落地**（−20.8 %）；
   其后又落了**第三刀**（赋值路径上每个指针只查一次登记表），两刀合计 **−33.6 %**（相对第一刀那一版）。
   细节与真实形状的数字见 §0 落地状态与 §9。下一档是第 3 条，但上界要按当前剖析重算。
3. **`owns` 守卫静态化**（能证明是 rc 槽的赋值直接发计数）—— 收益上界 **23 %**，前端要传"来源是 rc 槽"的信息，风险中（判错即 SEGV）。
4. **计数按编译期收窄** —— 收益 ≤ **10.7 %**，成本中高，红判据在 [`tests/reclaim-elide/`](../reclaim-elide/)。
5. **根表去重/共享** —— 单独做**指令收益为 0**，只省生成物体积（根表占生成物 **64 %**，121.5 KB → ~40 KB）。
6. **"只装活着的根"** —— ≤ **4 %**，且要付遍历成本才判得出谁活着。**不建议为此做活跃性分析**。
7. **槽地址延迟绑定**（`late`）—— 收益 55.9 %，但**与第 1 条完全重合**且成本高得多；只有将来做
   异步/批量清扫时才值得。

---

## 1. 测量口径（数字绑在哪、怎么复跑）

一手跑法：

```bash
cd <repo>
tests/reclaim-perf/run.sh          # 默认 N=200000；N=800000 时约 10 分钟量级
```

`run.sh` 自己会：拷 `build/extc` 到临时目录固定住 → 生成/编译全部变体 → 校验输出逐字一致 →
逐个跑 callgrind → 打印 ⑤⑥⑦⑧ 四张表。下面是写这份 README 时用的那一跑的口径。

**编译器状态（重要）**：`src/back/` 有**别人正在改的未提交改动**（登记缺口修复）。
所有数字绑在这一份固定住的构建产物上：

```
HEAD          = 1b62eb8（reclaim-default）
build/extc    = md5 5e51d77fb9a60864ba1fe9197561c78b   ← 已 cp 到 /tmp 固定，中途不重建
git status    = M src/back/codegen.c, M src/back/reclaim.c, M src/back/reclaim.h
```

> ⚠️ 因此本目录的数字与 [`reclaim-shapes/README.md`](../reclaim-shapes/README.md)
> 「差距与原因」一节里的 **3.805 G / 0.621 G** **不是同一份构建，也不是同一个编译档**。
> 这一点已经查清，见 §6（旧的两个数都是 `-O2` 档；工作树的 `src/back` 未提交改动另加 6.8 %）。

**形状与规模**：`tests/reclaim-shapes/cyclic_pressure.extc`，N=800000（与原型同参），
另有 N=20000/200000 用于小规模与斜率。**C 原型**对照跑
`prototype-reclaim/stress.c` 的参数 `N 1024 64 20000 48 8`（缓存 1024 / 每 64 轮 / 索引 2 万 / 节点 48 字节 / 分片 8），
与 extC 形状的旋钮对齐。

**编译**：

```bash
EXTC_RECLAIM=1 <pinned>/extc -I <repo>/stdlib tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/on.c
cc -std=c11 -fwrapv -O1 -o /tmp/on /tmp/on.c
```

> 注：`extc` 按**自身所在目录**找 stdlib，把二进制拷到别处固定时必须显式 `-I <repo>/stdlib`，
> 否则 `use std::io` 找不到（报 `cannot find module std::io`）。`run.sh` 已经这么做了。

**指令数**：`valgrind --tool=callgrind --callgrind-out-file=...` + `callgrind_annotate --auto=no`。
取 callgrind 输出文件的 `summary:` 行（总 Ir）与 annotate 的函数表（自身 Ir）。

---

## 2. "1410 项根表"是怎么来的

### 2.1 哪些槽算根

生成物里那个热点长这样（本跑里是 `on.c` 第 1409 行，**单行 40513 字符**；行号随生成物变）：

```c
extc_rc_maybeCollect(extc_rc_regionOf((void *)&(__extc_a[1]), 1), (void *[]){
    (void *)(w.bkHead.data[0]), (void *)(w.bkHead.data[1]), … ,
    (void *)(w.tree.data[255]), (void *)(w.last), (void *)(w.idxHead), (void *)(w.retire) }, 1411);
```

`w` 是 `main` 的**栈上局部结构体**（同一份生成物里的 `world w = (world){ … }`）。
根表 = 它身上**每一个 `ref` 槽**：

| 来源（`cyclic_pressure.extc` 的 `struct world`） | 槽数 |
|---|---|
| `cache: [1024]?mut ref node` | 1024 |
| `tree: [256]?mut ref node` | 256 |
| `bkHead: [64]?mut ref node` | 64 |
| `bkTail: [64]?mut ref node` | 64 |
| `last` / `idxHead` / `retire`（三个标量 `ref`） | 3 |
| **小计** | **1411** |
| 两个站点再加当轮新分配的局部（`p` / `t`） | +1 = **1412** |

**三个站点**（`main` 里三处分配点）：`1411` / `1412` / `1412` 项，调用次数分别是
`N` / `20000`（索引构建，定长）/ `N/16`。

**为什么这么多**：整条定长数组都进根表，**不区分槽此刻有没有值**；而 `cache[1024]` 与
`tree[256]` 是业务形状自己要求的定长槽（原型里也是 1024 / 8192）。所以这个数是**形状的
性质 + 保守发射**两件事叠出来的，不是编译器把根表发大了。

**为什么每个分配点都要重发**：`maybeCollect` 的根表是**调用实参**，C 语义要求调用前
把复合字面量整个建出来；而清扫触发是运行期判据（内存压力），编译期不知道这次要不要扫。
于是 87 万次分配 → 87 万次建表。

### 2.2 多少是死的

探针（`rootprobe.py` 只读插桩，只加计数不改判据）逐站点统计。**原始输出**：

```bash
python3 tests/reclaim-perf/rootprobe.py /tmp/on.c /tmp/probe.c
cc -std=c11 -fwrapv -O1 -o /tmp/probe /tmp/probe.c
/tmp/probe 800000 2>&1 >/dev/null | grep extc_probe
```

```
extc_probe root site=0 calls=800000 nslots=1411 ever_nonnull=1411 dead_slots=0 avg_nonnull=1354.2 max_nonnull=1411 last_nonnull=1411 slots_materialized=1128800000
extc_probe root site=1 calls=20000  nslots=1412 ever_nonnull=701  dead_slots=711 avg_nonnull=504.5  max_nonnull=701  last_nonnull=701  slots_materialized=28240000
extc_probe root site=2 calls=50000  nslots=1412 ever_nonnull=1412 dead_slots=0   avg_nonnull=1355.2 max_nonnull=1412 last_nonnull=1412 slots_materialized=70600000
extc_probe totals root_calls=870000 slots_materialized=1227640000 sweeps=28 collect_calls=28 ownSlab_calls=19692486 ownSlab_null=4751809 ownSlab_miss=1287892 ownSlab_rej_off=8060 ownSlab_rej_slot=0 ownSlab_rej_data=0 ownSlab_hit=13644725 ownLower_calls=14940687 owns_calls=0 owns_false=0 ownN_max=5
```

读数（**实测**）：

* **87 万次分配点、共物化 12.2764 亿个槽** —— 与 `reclaim-shapes/README.md` 里
  「87 万次分配 × ~1410 个槽 ≈ 12 亿次装载/存储」逐字对上。
* **永久死槽（整跑从未非空的槽）：只有站点 1 的 711/1412**，因为站点 1 在**索引构建阶段**
  执行（`cache`/`tree` 还没填）。按物化量加权只占 `28.24 M / 1227.64 M = 2.3 %`。
* **稳态瞬间非空比例**：站点 0/2 是 `1354/1412 = 95.9 %`、`1355/1412 = 95.9 %`。
  `cache` 只写不清（`cachePut` 只赋值），所以填满后一直活着。
* 短跑时死槽多（N=20000：三个站点都是 711 死槽、`avg_nonnull ≈ 504`），
  **但这个比例随轮数衰减**——结论要按目标规模说。

对应的 N=20000 / N=200000 原始输出见 §7「原始输出」。

---

## 3. 精确归因

### 3.1 总表（N=800000，`on` = 4 408 392 512 Ir）

`callgrind_annotate` 的**函数自身**指令数（`???` = 生成物里的 static 函数）：

```
  2,523,564,402 (57.24%)  main
    524,545,989 (11.90%)  extc_rc_ownLower
    491,239,797 (11.14%)  extc_rc_ownSlab
    250,571,631 ( 5.68%)  extc_rc_holdUpd
    163,394,255 ( 3.71%)  extc_rc_maybeCollect      ← collect/sweep/mark 被内联进来了
    131,417,276 ( 2.98%)  extc_rc_storeRefOwned
     57,103,064 ( 1.30%)  extc_rc_alloc
     56,390,646 ( 1.28%)  extc_rc_unlinkChildren
     41,760,017 ( 0.95%)  extc_rc_regionOf
     38,897,044 ( 0.88%)  extc_rc_regionOfOwn
```

`main` 自身 2.5236 G 与旧 README 的「main 自身 2.523 G」**逐位相同** ⇒ 根表那段代码路径
从头到尾没变（§6）。

### 3.2 逐项：怎么量的、得到多少

**（1）根表物化 = 56.1 %** —— 两条独立的差分，互为校核：

| 变体 | 是什么 | N=800000 | N=200000 |
|---|---|---|---|
| `on` | 基线 | 4 408 392 512 | 1 169 388 375 |
| `guardfirst` | 调用点包一层 `if (extc_rc_shouldSweep(r))`，不到压力线**根本不建表** | 1 935 792 900 | 508 581 342 |
| `late` | 每站点建一张**槽地址**表（只一次），标记时才解引用 | — | 515 302 680 |
| **Δ root** | `on` − `guardfirst` | **2 472 599 612 = 56.1 %** | 660 807 033 = 56.5 % |
| 校核 | `on` − `late` | — | 654 085 695 = 55.9 % |

**每个 (槽 × 分配点) 值 2.014 条指令**（两个规模上一致到小数点后两位）：

* N=800000：`2 472 599 612 / 1 227 640 000 = 2.014`
* N=200000：`660 807 033 / 328 090 000 = 2.014`

即"从 `w` 里读一个槽 + 往临时数组里写一个槽"——**GCC 已经向量化得不错**，但次数太多。

**（2）`owns` 守卫 = 23.0 %** —— `ownSlab` 491.2 M + `ownLower` 524.5 M = **1 015.8 M**。
按调用次数摊：`1 015 785 786 / 19 692 486 = 51.6` 条指令**每次守卫查询**。

* **不是深度问题**：探针 `ownN_max=5` —— 登记表只有 **5 个 slab**，二分 ~3 轮。
  代价全在"每次指针赋值都要走一遍"。
* **二分单卖价**：`guardfast`（直接映射命中就跳过二分、未命中回落）在 N=200000 上省下
  **120 694 101 Ir = 10.3 % of on**（等于 `ownLower` 自身 132.8 M 的 91 %）。
* 守卫命中的分类（N=800000，五类相加 = `ownSlab_calls`，闭合）：
  `null 4 751 809 + miss 1 287 892 + rej_off 8 060 + rej_slot 0 + rej_data 0 + hit 13 644 725 = 19 692 486`。
  `rej_off=8060` 就是"指针不是 rc 槽"的真实命中（比如 `w` 这个 arena 指针、以及
  `unlinkChildren` 顺着字段读到的外来指针）——**守卫不能拆**，见 §4.3。

**（3）计数 = 10.7 %** —— `holdUpd` 250.6 M + `storeRefOwned` 131.4 M + `regionOf` 41.8 M
+ `regionOfOwn` 38.9 M + `initRef` 10.0 M = **472.7 M**（N=800000；N=200000 时 125.7 M = 10.7 %）。
`nohold` 消融（`holdUpd` 直接返回）在 N=200000 上给的是 **199.1 M = 17.0 %**，
但它同时取消了"归零当场释放"⇒ 内存行为变了 ⇒ **只能当上界**读。

**（4）标记 + 清扫 = 2.7 %** —— `on` − `nosweep`（`-DEXTC_RC_FLOOR=1 TiB`，清扫一次不触发）
= `4 408 392 512 − 4 287 888 118 = 120 504 394`。
（N=200000：75 787 518 = 6.5 %；28 次清扫 vs 87 万次分配，本来就该是零头。）

**（5）空 owner 的登记 = 0.09 %** —— `noreg0`（`storeRefOwned(..., (void *)(0))` → `storeRef`）
只省 1 086 254 Ir（N=200000）。6 个静态站点，白付 `regionOfOwn(0)`，量级可忽略。

**（6）回收总开销 = 97.4 %** —— `on` − `off` = `1 169 388 375 − 30 341 339 = 1 139 047 036`
（N=200000）。程序自身只占 2.6 %。

**归因的闭合程度**（不是恒等式，是近似归因）：

* N=200000：660.8 + 264.0 + 125.7 + 75.8 + 41.9 = **1 168.1 M** vs `on` 1 169.4 M，残差 1.3 M（0.1 %）。
* N=800000：2 473.0 + 1 015.8 + 472.7 + 120.5 + 159.5 = **4 241.0 M** vs `on` 4 408.4 M，残差 167.4 M（3.8 %）。

残差 = 没分桶的 `maybeCollect` 非清扫部分、`markPush`、libc（`calloc`/`memmove`）与程序自身逻辑。
第 1、4 行是**差分**口径，自带它们的守卫/libc 份额，所以六块**不是严格互斥**
（残差随规模变化，正是因为差分项与自身项在交叠处的抵消程度不同）。
**要精确到恒等式需要按调用者拆 `ownSlab`/`ownLower` 的指令数，本轮没做**（见 §8）。

---

## 4. 只差根表尺寸的对照实验

### 4.1 加空根（`padN`）：语义逐字不变，只改表尺寸

`ablate.py padN` 把每个调用点的根表**用 `(void *)(0)` 补齐**到 N 项（原根一个不少）。
标记会跳过空根 ⇒ **语义完全不变**，可验证：

```bash
python3 tests/reclaim-perf/ablate.py pad2824 /tmp/on.c /tmp/pad2824.c
cc -std=c11 -fwrapv -O1 -o /tmp/pad2824 /tmp/pad2824.c
/tmp/on 20000 | md5sum ; /tmp/pad2824 20000 | md5sum     # 两个 md5 相同
```

**实测（N=200000）**：

| 变体 | 根表项数 | Ir | 说明 |
|---|---|---|---|
| `on` | 1411/1412/1412 | 1 169 388 375 | 基线 |
| `pad2824` | 2824 | 1 497 946 230 | Δ = +328 557 855 |
| `pad5648` | 5648 | 10 349 294 325 | **不可用，见 4.2** |
| `pad11296` | 11296 | 22 186 468 011 | **不可用，见 4.2** |

`pad2824` 多出的槽数 = `(200000 + 20000 + 12500) × (2824 − 1412) = 328 290 000`，

> **斜率 = 328 557 855 / 328 290 000 = 1.0008 条指令 / (槽 × 分配点)**

`pad2824` 那一档是**干净的**：它只增加了"把常量空指针写进临时数组"这一件事。
而真实根项要多一次"从 `w` 读出来"，所以真实单价是 **~2.01**（§3.2），与"读+写"吻合。

### 4.2 一个必须说明的假象：`pad5648` 起 GCC 换了策略

`pad5648` 从 1.50 G 跳到 10.35 G，**不是线性外推失灵**，是 GCC 在数组变大后决定
"先整片清零、再填真实项"：

```
9,044,103,700 (87.39%)  __memset_avx2_unaligned_erms
  812,645,248 ( 7.85%)  main
  132,757,213 ( 1.28%)  extc_rc_ownLower
```

（`pad2824` 那一跑没有这一行；`memset` 只有 `extc_rc_alloc` 的每槽清零 3.5 M。）
所以 **`pad5648`/`pad11296` 不是"只差表尺寸"，还差了一段 memset 策略**，
不能用它们外推。结论只用 `on → pad2824` 这一对干净的对照 + `guardfirst`/`late` 两条差分。

### 4.3 反面对照：把守卫整个拆掉，本形状直接段错误

`ablate.py` 里**没有**"假定所有指针都是 slab 指针"那一档，因为它在这里跑不起来：

```
Program received signal SIGSEGV
#0 extc_rc_regionOfOwn   #1 extc_rc_unlinkChildren   #2 extc_rc_regionDrop   #3 main
```

`w`（world）是 **arena** 指针，会作为 `owner` 进 `storeRefOwned`；`unlinkChildren` 也会顺着
字段读到外来指针。⇒ **`owns` 守卫不是可选项**，这是 `tests/reclaim-shapes/README.md`
里那 4 个 SEGV 用例在"压力形状"上的同一个洞。

### 4.4 延迟绑定原型（`late`）与入口守卫（`guardfirst`）等价

`late`（`latemark.py`）= 每站点建一张 **槽地址表**（`&(w.cache.data[k])`），标记那一刻才
`*(void **)slot` 解引用。它把"每个分配点 O(根项数)"换成"建一次 O(根项数)"，
且**不改健全性**（解引用拿到的是同一时刻的值——`maybeCollect` 是同步调用）。

| N=200000 | Ir | vs `on` |
|---|---|---|
| `on` | 1 169 388 375 | — |
| `guardfirst`（入口守卫） | 508 581 342 | −56.5 % |
| `late`（槽地址延迟绑定） | 515 302 680 | −55.9 % |

**两者重合**（差 1.3 %：`late` 在清扫那一刻仍要遍历 1412 项解引用，`guardfirst` 也要建同样的表，
差别只是那 8 次清扫）。⇒ 要拿这 56 %，**入口守卫就够了**，不必付 `late` 的复杂度。

---

## 5. 候选优化：收益 / 成本 / 风险

### 5.1 按 收益÷成本 排的下一步

| 序 | 动作 | 实测收益 | 实现成本 | 风险 | 判据/回归 |
|---|---|---|---|---|---|
| **1** | **分配点入口守卫**（**已落地**） | **56.1 %**（N=8e5 实测）；落地后 N=2e5 **−56.5 %** | **极低**：codegen 把 `extc_rc_maybeCollect(r, (void*[]){…}, n)` 包成 `{ ExtcRcRegion *t = extc_rc_regionOf(…); if (extc_rc_shouldSweep(t)) extc_rc_maybeCollect(t, (void*[]){…}, n); }` | **≈0**：`maybeCollect` 第一句就是同一判据（同一份 `extc_rc_shouldSweep`）⇒ 语义逐字等价 | [`gate.sh`](gate.sh)（先红后绿）+ `tests/reclaim-shapes/run-switch.sh`（17 条全绿，关掉仍红）+ `run-evidence.sh`（清扫确实上场） |
| **2** | **登记表查询 O(1) 化**（**已落地**）＋**第三刀：赋值路径每个指针只查一次**＋**第四刀：热路径 `static inline`** | N=2e5 **510 475 244 → 404 358 444 → 339 186 860 → 308 485 778 Ir**；真实形状 `rebuild` 420 → **330 ms**、`bt` 1120 → **570–610 ms**（后两刀的墙钟与 Ir 脱钩：三个形状内存受限） | 低：直接映射命中缓存 + `*Slab` 记账变体 + `inline`（**不给 `always_inline`**，强制内联在 `rebuild` 上反而 +0.5 %） | **低**：判据与回落路径一字不改（原型与落地后输出都逐字一致；运行时 70 条断言 + ASan 干净） | [`gate.sh`](gate.sh)（现行线 324 000 000 = 第四刀实测 × 1.05）+ `run-switch.sh` 30/0 + `hole_c` 7/0 |
| **3** | **`owns` 守卫静态化** | 上界 **23.0 %**（守卫总量）；实际取决于能证明多少赋值 | 中高：前端把"这个引用值来自 rc 槽"的来源信息传到 codegen，能证明的直接发 `inc`/`dec`，不发 `ownSlab` | **中**：判错就是 SEGV/漏收 —— 正是 `tests/heap/loan`、`tests/heap/plate`、`tests/cabi/heapmain`、`tests/coro/iterator_protocol` 钉的那条 | 同上 4 个用例 + `tests/reclaim-shapes/run-switch.sh` |
| **4** | **计数按编译期收窄**（`RECLAMATION.md` 五节末条） | ≤ **10.7 %**（计数自身）；`nohold` 上界 17 % | 中高：编译期唯一性/新鲜度证明 | 中：`tests/reclaim-elide/` 既有绿判据与**红判据**都必须不动 | `tests/reclaim-elide/run.sh` |
| 5 | 根表**去重/共享** | 指令 **0 %**；生成物体积 **−64 %**（121.5 KB → ~40 KB） | 低（codegen 去重） | 低 | 生成物 diff |
| 6 | **只装此刻活着的根** | ≤ **4 %**（N=8e5；因为 95.9 % 本来就非空） | 中：得先遍历一遍判断谁非空 —— **省不下还要先付** | 低 | — |
| 7 | **槽地址延迟绑定**（`late`） | 55.9 %，但**与第 1 条重合** | 中高：多一套 `markSlots`/`collectSlots` + 站点 static 表（首次 malloc） | 中：把"分配点那一瞬的值"改成"清扫那一瞬的槽值"；同步调用下等价，异步/批量清扫下**不等价**；另有线程安全与表释放 | 同 1 |

### 5.2 逐条说清"为什么"

**#1 入口守卫（首推）**：代码里 `maybeCollect` 的唯一行为就是
```c
if (!extc_rc_shouldSweep(r)) return -1;
return (int)extc_rc_collect(r, roots, nroots);
```
把判据提到调用点前面，是**纯粹的求值顺序改动**：87 万次调用里只有 28 次需要建表。
`guardfirst` 原型就是这件事的机器证明（生成物副本上做的，`src/` 没动）。

**#5 去重单独做没有运行期收益**：去重能消掉重复的**表内容**，但每个分配点**仍然要
把那 O(根项数) 个槽从 `w` 读出来写进临时数组**——这正是被计入 56 % 的那部分。
真正让"表"不再重复付的是 #1（根本不建）或 #7（只建一次）。
"共享"若指"多个站点共用一张 static 表"，那就已经是 #7 了。

**#6 为什么不建议**：直觉是"1412 项里大部分是空的，只装非空的就行"。实测把这个直觉否掉了：

| 规模 | 站点 0 平均非空 / 总槽 | 永久死槽占比（按物化量） |
|---|---|---|
| N=20 000 | 503.4 / 1411 = 35.7 % | ≈ 50 %（三个站点都是 711/1412 死槽） |
| N=200 000 | 1226.5 / 1411 = 86.9 % | 28.24 M / 328.09 M = **8.6 %** |
| N=800 000 | 1354.2 / 1411 = 95.9 % | 28.24 M / 1227.64 M = **2.3 %** |

`cache` 只写不清 ⇒ 填满后一直非空；`tree` 同理。**运行越久越没用**，
而且"判定谁非空"本身要遍历一次（就是把省下的又付回去，只是少了写）。

**#3 与 #2 的关系**：#2 只是把守卫的**查表**变快（每次 51.6 Ir 里的二分部分）；
#3 是把**整个守卫**从能证明的站点上删掉（连 `ownSlab` 都不用进）。两者可以叠加，
但 #2 便宜得多、先做。

**#7 与 #1 的关系**：两者收益重合，**不要同时做**。#1 是 10 行 codegen；
#7 要动运行期接口 + 站点 static 状态。除非将来把清扫改成异步/批量（那时 #1 的
"当场建表"本来就不成立），否则 #7 没有位置。

---

## 6. 与旧数字的差异（`reclaim-shapes/README.md` 的 3.805 G / 0.621 G）—— **已查清**

为了把这件事钉死，用 `git archive HEAD | tar -x -C /tmp/pristine && make -j12`
在 `/tmp` 重建了一份**纯 HEAD** 的 `extc`（**没动工作树、没动 `.git`、没碰 `build/`**），
四种组合都跑了 N=800000：

| 构建 | `-O1`（任务规定的编译档） | `-O2`（`prototype-reclaim/run.sh` 的档） |
|---|---|---|
| **pristine HEAD** `1b62eb8`（`git archive` 重建） | 4 129 038 253 | **3 874 977 128** |
| **工作树**（HEAD + 未提交 `src/back` 改动，md5 `5e51d77f`） | **4 408 392 512** | 4 120 476 037 |
| 旧 README 记的 extC | — | **3 805 000 000（3.805 G）** |
| C 原型 `stress.c` 分片=8 | 708 448 343 | **620 902 238（= 0.621 G，逐位吻合）** |

三条结论：

1. **旧 README 的两个数都是 `-O2` 档。** 原型侧 `-O2` 复现到 `620 902 238`，与旧记录的
   0.621 G **逐位一致**；extC 侧 `pristine -O2 = 3.875 G` 距 3.805 G 只差 1.8 %
   （旧测量应来自略早的提交）。⇒ 旧 README 的 **6.1×** 与本跑的 **6.22×**（`-O1` 对 `-O1`）
   是同一个结论，只是绝对值绑在不同编译档上。**引用绝对值时必须写清哪一档**
   （同一份代码 `-O1` vs `-O2` 差 **6.5 %**）。
2. **未提交的 `src/back` 改动把 `-O1` 从 4.129 G 抬到 4.408 G（+6.8 %），机制已定位：**
   pristine 的生成物在 `pushFront` / `retireChain` / `cachePut` 等处发的是
   `extc_rc_storeRefOwned(…, (void *)(0))`（**owner 是空**），工作树发的是真实 owner
   （`(void *)(a)` / `(void *)(w)`）。owner 非空 ⇒ `storeRefOwned` 真的去做跨区登记
   （`regionOfOwn` + `rsInc`/`rsDec`），守卫调用随之变多：

   | 函数自身 Ir @800k | pristine HEAD | 工作树 | 差 |
   |---|---|---|---|
   | `main` | 2 523 564 420 | 2 523 564 402 | **−18（就是同一段）** |
   | `extc_rc_ownSlab` | 389 555 957 | 491 239 797 | +101.7 M |
   | `extc_rc_ownLower` | 384 319 729 | 524 545 989 | +140.2 M |
   | `extc_rc_regionOfOwn` | 23 376 860 | 38 897 044 | +15.5 M |
   | `extc_rc_storeRefOwned` | 111 303 264 | 131 417 276 | +20.1 M |
   | `extc_rc_rsDec` | 0 | 28 210 | +28 K |
   | **总计** | 4 129 038 253 | 4 408 392 512 | **+279.4 M** |

   这正是 HEAD 提交信息「**步15：跨区登记接进 codegen + 来源侧记账撤销**」在做的事，
   **是健全性修复（把之前漏掉的跨区登记补上），不要当成性能回归回退。**
3. **主因的结论与这些差异无关**：`main` 自身在四种组合里都是 **2.523–2.527 G**，
   对应根表物化 **≈2.47 G**，占 `-O1` 工作树 **56.1 %** / `-O1` pristine **57.2 %** /
   `-O2` 工作树 **60.0 %**。「根表物化是主因」**与编译档、与这次 `src/back` 改动都无关**。

（pristine 与 `-O2` 那几跑的原始 `summary:` 行已并入 §7.2 的表；复跑命令见 §7.3。）

---

## 7. 原始输出

### 7.1 探针（根表形状 + 守卫分类）

```bash
python3 tests/reclaim-perf/rootprobe.py /tmp/on.c /tmp/probe.c && cc -std=c11 -fwrapv -O1 -o /tmp/probe /tmp/probe.c
for n in 20000 200000 800000; do echo "### N=$n"; /tmp/probe $n 2>&1 | grep extc_probe; done
```

```
### N=20000
extc_probe root site=0 calls=20000 nslots=1411 ever_nonnull=700 dead_slots=711 avg_nonnull=503.4 max_nonnull=700 last_nonnull=700 slots_materialized=28220000
extc_probe root site=1 calls=20000 nslots=1412 ever_nonnull=701 dead_slots=711 avg_nonnull=504.5 max_nonnull=701 last_nonnull=701 slots_materialized=28240000
extc_probe root site=2 calls=1250  nslots=1412 ever_nonnull=701 dead_slots=711 avg_nonnull=504.2 max_nonnull=701 last_nonnull=701 slots_materialized=1765000
extc_probe totals root_calls=41250 slots_materialized=58225000 sweeps=1 collect_calls=1 ownSlab_calls=854003 ownSlab_null=222199 ownSlab_miss=51581 ownSlab_rej_off=7916 ownSlab_rej_slot=0 ownSlab_rej_data=0 ownSlab_hit=572307 ownLower_calls=631810 owns_calls=0 owns_false=0 ownN_max=3
### N=200000
extc_probe root site=0 calls=200000 nslots=1411 ever_nonnull=1411 dead_slots=0 avg_nonnull=1226.5 max_nonnull=1411 last_nonnull=1411 slots_materialized=282200000
extc_probe root site=1 calls=20000  nslots=1412 ever_nonnull=701  dead_slots=711 avg_nonnull=504.5 max_nonnull=701 last_nonnull=701 slots_materialized=28240000
extc_probe root site=2 calls=12500  nslots=1412 ever_nonnull=1412 dead_slots=0 avg_nonnull=1227.5 max_nonnull=1412 last_nonnull=1412 slots_materialized=17650000
extc_probe totals root_calls=232500 slots_materialized=328090000 sweeps=8 collect_calls=8 ownSlab_calls=5257141 ownSlab_null=1267612 ownSlab_miss=337021 ownSlab_rej_off=8176 ownSlab_rej_slot=0 ownSlab_rej_data=0 ownSlab_hit=3644332 ownLower_calls=3989539 owns_calls=0 owns_false=0 ownN_max=5
### N=800000
extc_probe root site=0 calls=800000 nslots=1411 ever_nonnull=1411 dead_slots=0 avg_nonnull=1354.2 max_nonnull=1411 last_nonnull=1411 slots_materialized=1128800000
extc_probe root site=1 calls=20000  nslots=1412 ever_nonnull=701  dead_slots=711 avg_nonnull=504.5 max_nonnull=701 last_nonnull=701 slots_materialized=28240000
extc_probe root site=2 calls=50000  nslots=1412 ever_nonnull=1412 dead_slots=0 avg_nonnull=1355.2 max_nonnull=1412 last_nonnull=1412 slots_materialized=70600000
extc_probe totals root_calls=870000 slots_materialized=1227640000 sweeps=28 collect_calls=28 ownSlab_calls=19692486 ownSlab_null=4751809 ownSlab_miss=1287892 ownSlab_rej_off=8060 ownSlab_rej_slot=0 ownSlab_rej_data=0 ownSlab_hit=13644725 ownLower_calls=14940687 owns_calls=0 owns_false=0 ownN_max=5
```

（`ownSlab` 五类互斥计数相加 = `ownSlab_calls`，闭合：例如 N=800000 时
`4 751 809 + 1 287 892 + 8 060 + 0 + 0 + 13 644 725 = 19 692 486`。）

### 7.2 callgrind 总表

```bash
# 变体（/tmp/on、/tmp/guardfirst、…）的生成命令见 run.sh 的 ①②；这里只列测量本身。
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.on ./on 800000 >/dev/null 2>&1
grep '^summary:' /tmp/cg.on                    # summary: 4408392512
callgrind_annotate --auto=no /tmp/cg.on | head -12   # 每函数自身 Ir（§3.1 那张表）
rm -f /tmp/cg.on
```

**N=200000**：

| 变体 | Ir |
|---|---|
| `on` | 1 169 388 375 |
| `nosweep` | 1 093 600 857 |
| `off` | 30 341 339 |
| `guardfast` | 1 048 694 274 |
| `guardfirst` | 508 581 342 |
| `nohold` | 970 328 477 |
| `noreg0` | 1 168 302 121 |
| `late` | 515 302 680 |
| `pad2824` | 1 497 946 230 |
| `pad5648` | 10 349 294 325 |
| `pad11296` | 22 186 468 011 |
| `proto_O1` | 173 527 820 |
| `proto_O2` | 152 727 752 |

**N=800000**：

| 变体 | Ir | 每轮 |
|---|---|---|
| `on`（工作树 `-O1`） | 4 408 392 512 | 5 510 |
| `nosweep` | 4 287 888 118 | 5 360 |
| `off` | 116 165 063 | 145 |
| `guardfirst` | 1 935 792 900 | 2 420 |
| `on`（工作树 `-O2`） | 4 120 476 037 | 5 151 |
| `on`（pristine HEAD `-O1`） | 4 129 038 253 | 5 161 |
| `on`（pristine HEAD `-O2`） | 3 874 977 128 | 4 844 |
| `proto_O1` | 708 448 343 | 886 |
| `proto_O2` | 620 902 238 | 776 |

每轮指令数：extC 开关开（工作树 `-O1`）**5 510** vs 原型 `-O1` **886** = **6.22×**；
旧 README 的 **6.1×** 用的是 `-O2` 档（pristine `-O2` 3.875 G ÷ 原型 `-O2` 0.621 G = **6.24×**）。
**只有同档比才有意义**（混档会差 7 %，见 §6）。

### 7.3 pristine HEAD 对照怎么复跑（不碰工作树、不碰 `.git`）

```bash
rm -rf /tmp/pristine && mkdir -p /tmp/pristine
git archive HEAD | tar -x -C /tmp/pristine          # 只读 HEAD，不写 .git / build /
( cd /tmp/pristine && make -j12 )                   # 得到 /tmp/pristine/build/extc
cd /tmp/pristine
EXTC_RECLAIM=1 ./build/extc tests/reclaim-shapes/cyclic_pressure.extc -o /tmp/pristine.c
cc -std=c11 -fwrapv -O1 -o /tmp/pristine         /tmp/pristine.c
cc -std=c11 -fwrapv -O2 -o /tmp/pristine_O2      /tmp/pristine.c
valgrind --tool=callgrind --callgrind-out-file=/tmp/pr.cg /tmp/pristine    800000
valgrind --tool=callgrind --callgrind-out-file=/tmp/pr2.cg /tmp/pristine_O2 800000
grep '^summary:' /tmp/pr.cg /tmp/pr2.cg
```

「owner 从空变真实」这一条用的是生成物 diff（`sed 's/[0-9]\+//g'` 抹掉数字后对比）：

```bash
diff <(sed 's/[0-9]\+//g' /tmp/pristine.c) <(sed 's/[0-9]\+//g' /tmp/on.c) | head -40
# <  extc_rc_storeRefOwned((void **)&((*(a)).next), (void *)(b), (void *)());
# >  extc_rc_storeRefOwned((void **)&((*(a)).next), (void *)(b), (void *)(a));
```

### 7.4 四个脚本的定位

| 文件 | 干什么 | 备注 |
|---|---|---|
| `rootprobe.py` | 只读插桩：每个站点的根表**槽数 / 曾非空数 / 永久死槽数 / 平均非空数**，站点调用次数；`ownSlab` 的五类互斥分类、`ownLower` 次数、登记表最大条目数 | 只加计数，不改任何判据；结果走 stderr，前缀 `extc_probe` |
| `latemark.py` | **槽地址延迟绑定**原型：把"每分配点物化值表"换成"每站点建一次槽地址表、清扫时解引用" | 只改 `/tmp` 生成物副本 |
| `../inject.py`（`tools/`）| **注入验牙**：`--list` 列出注入（`thr-stale` / `rsinc-drop` / `sweep-never` / `overregister` / `perf-lookup` / **`count-noinc`**（只打计数档））；每条会改源码 → 重编 → 跑期望判据（**必须红**）→ 还原 → 再跑（**必须绿**），且默认在硬链接副本树里跑、收尾核对原树未被改动 | 手工注入有**忘记还原**的隐患；用这个工具代替 |
| `ablate.py` | 单项消融/原型：`guardfast`（登记表 O(1)）/ `guardfirst`（入口守卫）/ `nohold`（拆计数，上界）/ `noreg`、`noreg0`（拆跨区登记）/ `padN`（根表补空到 N 项） | `nohold`/`noreg` 会改健全性或内存行为，**只能当上界**；`guardfast`/`guardfirst`/`noreg0`/`padN` 语义不变（可验证输出逐字一致）。入口守卫**已在 codegen 落地** ⇒ `guardfirst` 对新生成物是 **no-op**（`already guarded`，该变体 == `on`） |
| `gate.sh` | **性能判据 + 槽号索引判据**：①开关开的 `cyclic_pressure` 固定 N=200000，callgrind 总指令数 ≤ 现行线（**线在脚本头部，每落地一次就往下拧一次**）；②`slotindex.c` 把发射出来的 `extc_rc_rcpOf`/`extc_rc_slotOf` 在 16..1 MiB 的每个 8 倍步长 × 全部合法槽号上做**域内穷举**（槽步长按 8 字节取整 ⇒ 索引是乘倒数右移，算错一格就会漏认合法槽 ⇒ 悬垂） | 性能线落地过一次就收紧一次（现行线见脚本头）；索引判据有牙：注入 `EXTC_RC_RCP_SHIFT` 42 → 19 ⇒ `slotindex: FAIL`（140384 处不符，exit 1）。用法：`tests/reclaim-perf/gate.sh` |
| `shape-baselines.tsv` | **按工具链**记 `gate-shapes.sh` 的指令数基线：键 = `cc -dumpfullversion` + `glibc-<版本>`，线 = 该键实测 × 1.05 | 换 gcc / glibc 会改编译决定与库实现（glibc 的 IFUNC 还按 CPU 派发 `memcpy`）⇒ 绝对线只在同一条工具链上有意义；表里没有的键**跳过并打出实测值**，新工具链第一次跑就把实测值抄成一行 |
| `run.sh` | 一键复跑上面全部：固定编译器 → 生成/编译 → 语义校验 → callgrind → 打印总表与差值 | `N=`、`SMALL=`、`SKIP_PROTO=1` 可调 |

---

## 8. 未查清

* **`ownSlab` 每次 51.6 Ir 的构成**：只知道登记表只有 5 个 slab（二分 ~3 轮），
  没逐条看反汇编数清 prologue / 调用开销 / 各判据各占多少。⇒ 这决定 #2（O(1) 化）之外
  还有多少可以靠"少判两条"拿回来。
* **`src/back` 那 +6.8 % 里"必要的健全性成本"与"可以省的部分"各占多少**：§6 只证明了
  它是"owner 由空变真实 ⇒ 真的去做跨区登记"带来的，**没有**判断这些登记是否每一条都必须发
  （例如同区可证时本来就不必登记 —— `ablate.py noreg0` 那一档量的是"owner 为空"的那 6 个点，
  量不到"owner 非空但同区"的点）。
* **真实业务形状上的外推**：本结论建立在一个形状（`cyclic_pressure`）上。
  "每个分配点物化全量根表"的严重程度 ∝ **根项数 × 分配密度 / 清扫次数**；
  根项少或清扫频繁的形状上，56 % 会显著缩水。**没有**在别的形状上复测。
* **`guardfirst` 在"清扫频繁"形状上的收益**：本形状 87 万次分配对 28 次清扫（比 ≈31000:1）。
  若某个形状清扫很频繁，入口守卫的收益会下降——**没有**构造这样的对照。
* ~~**`guardfirst` 没有在真实 codegen 里落地过**~~ —— **已落地并验证**（`src/back/codegen.c`
  `rcEmitTrigger`，判据 `gate.sh`）：N=200000 上 1 169 388 408 → **508 581 405 Ir（−56.5%）**，
  落地后的 508 581 405 与原型 `guardfirst` 的 508 581 342 相差 63 条（测量噪声）；清扫次数 / `EXTC_RC_STATS`
  / N vs 4N 峰值比 / 全部 `owns` 探针计数**逐位不变**，默认（开关关）产物对 693 份可编译
  `.extc` **逐字节 diff=0**。

## 9. 真实形状上的外推：`bt` / `rebuild` 走回收档之后

§8 的两条未查清在这里补上：`ownSlab` 的大小，以及"别的形状上会怎样"。用横评自己的量法
（`cc -O3 -march=native -fwrapv` + `bench/bigmatrix/in/<形状>.txt`，best of 3，`/usr/bin/time -v` 取峰值）：

| 形状 | 回收档 | 关闭档（`EXTC_RECLAIM=0`）| 倍率 |
|---|---|---|---|
| `bt`（二叉树，逐结点 `new`）| 1129 ms / 21228 KB | 240 ms / 9880 KB | **4.7×** |
| `rebuild`（分配与回收）| 427 ms / 5292 KB | 45 ms / 1900 KB | **9.5×** |

关闭档的 `bt` 240 ms 与 `RESULTS.md` 里"bt 231 ms"对得上 ⇒ 那张横评表量的是**回收默认开之前**的编译器。

**先排掉清扫**：`tools/rc-pause-probe.py` 插桩后 `bt` 报
`sweeps=1 max_pause_us=910.2 max_mark_us=1.8 max_sweep_us=908.3 total_pause_us=910.2`
⇒ 清扫总共 **0.9 ms / 1129 ms = 0.08 %**，不是它。

**再定位**：`callgrind` 跑 `in-small/bt.txt`（`--callgrind-out-file`），指令占比

| 函数 | 占比 |
|---|---|
| **`extc_rc_ownSlab`**（判断"这个指针是不是回收槽"的守卫）| **45.46 %** |
| `check'2`（程序自己的逻辑）| 36.96 % |
| `bottomUp` / `bottomUp'2`（程序逻辑）| 6.33 % / 2.99 % |
| `extc_rc_regionDrop` / `storeRefOwned` / `regionOf` / `unlinkChildren` | 2.26 / 1.49 / 1.32 / 1.00 % |

⇒ **开销在 `owns` 守卫上**，不在计数（`storeRefOwned` 1.5 %）也不在清扫（0.08 %）。
指令比只有约 2.2×，而**墙钟**是 4.7× —— 差在守卫的数据结构（登记表有序 + 二分）**cache 不友好**。

**两条路的状态**：

1. **加速守卫本身**（**已落地，三刀**）：
   · 第二刀：登记表查询 O(1) 化 —— 一张直接映射的命中缓存（命中即取，未命中/碰撞回落二分
     ⇒ 结果与只有二分时逐字相同），在 `src/back/reclaim.c` 的运行期文本里。
     N=2e5 `cyclic_pressure` **510 475 244 → 404 358 444 Ir（−20.8 %）**。
   · 第三刀：**赋值路径上每个指针只查一次登记表** —— `storeRefOwned` 把 owner/旧值/新值
     各查一次（原来连同 `rsInc`/`rsDec`/`inc`/`dec` 里的查询最多 7 次），记账走
     `holdUpdSlab`/`rsIncSlab`/`rsDecSlab`；命中路径再省掉一次冗余比对。
     再降到 **339 186 860 Ir**（相对第二刀 −16.1 %）。
   · 第四刀：**热路径小助手一律 `static inline`**（`slabOf`/`slotOf`/`bit`/`word`/`ownSlab`）
     → **308 485 778 Ir**（相对第三刀 −9.1 %）。量过五组变体：收益全部来自 `ownSlab`；
     **`always_inline` 在 `rebuild` 上反而 +0.5 %**（代码变大），只给 `inline` 才三个形状一起降。
   · 负结果（如实记）：`unlinkChildren` 里同样的"复用查询"反而 **+1.7 M Ir** ⇒ 没做。
   · 真实形状（同档 `-O3 -march=native`，输入 `bench/bigmatrix/in/*.txt`；关档 230 / 40 ms）：
     `bt` 1120 → **570–610 ms**、`rebuild` 420 → **330 ms**，输出都逐字一致。
     **墙钟与"入口总 Ir"脱钩**：第四刀把三个形状的 Ir 又降了 9~11 %，`bt` 的 best-of-9 只从
     610 到 570 ms 而**均值持平**，`rebuild` 330 → 330 ms。**原因在 §10 的消融里**：
     墙钟由指令决定（不是 cache 缺失），但由**计数那一支**决定 —— 前四刀减的入口 Ir
     没碰到 `initRef` 里的那行计数，所以墙钟不动。
   · 第五刀（**跨过程"本帧不会销账"**，见 `tests/reclaim-unified/DESIGN.md` 步 0.5 路线 A）：
     纯读函数里 `var p = t` 的 `+1` 改发 `extc_rc_initRefNoHold` ⇒ `bt` 的税
     **4.637× → 2.040×**（in-small）、**4.124× → 1.075×**（全输入）；`rebuild` 不受影响。
     线：`gate-tax.sh` 4870 → **2150**、`gate-shapes.sh` 的 bt 11 090 000 → **5 150 000**。
   · 第六刀（**`rsIncN==0` 跳过撤边走查**）：跨区登记计数为 0 ⇒ 无物可撤 ⇒ 两处走查整段跳过。
     `cyclic_pressure` 308 485 778 → **279 224 856**，`bt` 4 870 353 → **4 611 436**，
     `rebuild` 140 491 750 → **126 466 892**；`bt` 的税 2.040× → **1.910×**。
   · **目标口径的现状**（第八刀后实测）：`bt` 全输入（`16 18 500`）**税 1.035×、占比 3.4 %**
     （跨过程 `rcNoDec` 之前是 4.124×）；`12 14 200` 1.088×/8.1 %；in-small 1.711×/41.6 %。
     消融定价：把持有者计数**全省**只值 5.1 % ⇒ 残余 87 % 是分配器结构（Tier 3 才是下一档）。
   · **口径**（第七刀只动判据、没动代码）：`gate-tax.sh` 现在同时判**两个口径** ——
     `in-small`（分配占主导，1.910×）与 `12 14 200`（**遍历占主导**，1.148×）。
     用户的"不需要回收 ⇒ 占比接近 0"要读后者；`14 16 300` 1.119×、全输入（第六刀前）1.075×，
     趋势是遍历/分配比越大越趋近 1.0。
   · **负结果**：给 `extc_rc_regionOf` 加 64 格直接映射缓存**反而更慢**（bt 1.910×→1.920×，
     `cyclic_pressure` +465 k Ir）—— 登记表只有几个条目、线性扫本来就便宜。已回退。
   · 第七刀（**删掉没人用的 arena 上那次 `regionDrop`**，codegen 死调用消除）：`CG` 记下本函数
     哪些 arena 层可能承载 rc 对象（分配表达式 / home 实参 / `main` 的家 arena），帧销毁只为这些层发丢弃。
     `bt` 税 1.910× → **1.762×**、mid 口径 1.148× → **1.103×**；`rebuild`/`cyclic_pressure` 不受影响。
   · 第八刀（**`regionOf` 结果做成函数级局部量**）：区域在帧内是不变量，线性扫登记只做一次。
     `bt` 税 1.762× → **1.711×**、mid 口径 1.103× → **1.088×**；`rebuild` −8.5 %。
     协程函数不 hoist（恢复状态机可跳进 body 中间）。
   · 判据三条闸门：`gate.sh`（合成形状 `cyclic_pressure`，线收到 **324 000 000**）＋
     `gate-shapes.sh`（**真实形状** `bt` / `rebuild` 的小输入，线**按工具链**记在
     `shape-baselines.tsv`（现行：参考机 2 950 000 / 15 790 000，CI 的 gcc 13.3.0 3 170 000 / 17 740 000），
     并要求开档/关档输出逐字一致；该工具链不在表里就打实测值 + `  --   跳过：`，不当通过）
     ＋ `gate-tax.sh`（**回收税**：`bt` 开档/关档指令数比 ≤ 现行线，
     棘轮只许往下拧 —— 目标值 **1.00×**）。任一红都说明收益被付回或目标退步。
2. **静态化**（**未做**，上界要重算）：编译期能证明"这个指针是回收槽"的赋值直接发计数，
   只在证不出时走运行期守卫（§5 建议里的第 ③ 条）。原来的"上界 23 %"是相对**每次赋值查
   5～7 次**的守卫算的，第二、三刀之后这个上界会明显缩水 —— 动手前先按当前剖析重估。
   §10 的 `tinymap` 消融给了个弱信号：守卫那条路如今只值 ~2 %（`bt` 606 vs 592 ms，噪声内），
   **优先级低于计数**；既然墙钟是"指令受限但受限在计数那一支"，下一档就该动计数，不是继续
   削守卫、更不是去优化 cache（元数据只占 `bt` 缺失的 12 %，`ownMap` 只值 4 547 次缺失）。

**复现**：本节的命令都是 `build/extc` + `cc -O3 -march=native -fwrapv` + 上表输入，
`EXTC_RECLAIM=0` 取关闭档；`callgrind` 与探针用法见本文件第 1 节与 `tools/rc-pause-probe.py` 的文件头。

---

## 10. `bt` 的回收开销落在**局部引用的计数**上（不是 `storeRefOwned`）

§9 的剖析（第四刀之前）说 `bt` 的开销在 `owns` 守卫。守卫单次变便宜之后 **`bt` 的墙钟没动**
（610 → 610 ms，最佳 5 次交错），所以直接**数调用**（把 `/tmp` 生成物副本加上计数器）：

`bench/bigmatrix/in-small/bt.txt`（输入 `8 10 50`）：

| 入口 | 调用次数 |
|---|---|
| `extc_rc_ownSlab` | **115 461**（命中 115 461 / 回落 0）|
| `extc_rc_initRef` | **106 377** |
| `extc_rc_holdUpd` | 106 377（**全部**来自 `initRef`）|
| `extc_rc_storeRefOwned` | 3 028 |
| `extc_rc_regionOfOwn` | 3 028 |
| `extc_rc_alloc` | 3 034 |
| `owns` / `mark` / `maybeCollect` | 0 |

⇒ **92 % 的 `ownSlab` 调用来自 `initRef`**；而这个形状里 `initRef` 几乎全是 `check()` 里的
`var p = t` —— **把一个已有的引用拷进一个局部绑定**。全输入（`in/bt.txt`，`16 18 500`）上
同一支量到 **265 551 145** 次 `ownSlab`（命中 265 551 085、回落 60、`ownLower` 84 次）。

**为什么这些计数只增不减**：`src/back/codegen.c` 只在两处发计数 ——
`extc_rc_initRef`（`:5333` 局部/全局引用绑定、`:5025` 整值存储的引用叶子）与
`extc_rc_storeRefOwned` / `storeRefUnknown`（赋值）。**块退出与函数返回都不发 `dec`**：
一个局部绑定唯一的"销账"机会是**它自己被重新赋值**（那时发的是 `storeRefOwned`，
`dec` 的是旧值）。`check()` 里的 `p` 从不重新赋值 ⇒ 那 10 万次 `inc` 一笔也没销掉。

**复现（只改 `/tmp` 副本）**：给 `extc_rc_ownSlab` / `initRef` / `storeRefOwned` / `holdUpd` /
`alloc` / `owns` / `mark` / `maybeCollect` 各插一个 `__g_*++`，挂 `atexit` 打印；
计数变量**声明要放在 `extc_rc_ownTab` 声明之后**（`ownLower` 定义在它前面，`ownLower` 也要用计数器，
得先有声明）。输入用 `bench/bigmatrix/in[-small]/bt.txt`。

**这条把"计数按编译期收窄"的优先级抬上来了**（`docs/RECLAMATION.md` 第五节末条）：
在**读式拷贝多、写指针少**的遍历形状上，计数是当下最大的一笔；
它的三类"该省"判据已经全红在 `tests/reclaim-elide/`（`gate-reclaim-elide` 盯着，见那个目录）。

### 10.1 墙钟归因：**是指令，而且是计数那一支的指令**（消融实测）

在 `/tmp` 的生成物副本上做**消融**（改的是副本、语义已变 ⇒ 只能当**上界**读），
交错 best-of-9 量墙钟，并用 `valgrind --tool=cachegrind --cache-sim=yes` 取 D1/LL：

| 变体 | `bt` 墙钟 | `bt` Ir | `bt` D1mr |
|---|---|---|---|
| 开档 | 592–622 ms | 16.63 G | 74.94 M |
| 关档 `EXTC_RECLAIM=0` | 244–248 ms | 4.03 G | 65.68 M |
| **删掉 `initRef` 里那一行计数**（不安全）| **251 ms** | **4.30 G** | **66.18 M** |
| `ownMap` 缩到 4 KB | 606 ms | — | — |

⇒ ① 删一行计数：**指令 −74 %、墙钟 −57 %**，而 **D1 缺失只降 12 %**（LL 几乎不动）；
多出来的缺失对应约 1 350 条指令/次 ⇒ 缺失最多值 ~24 ms，差是 341 ms。**墙钟由指令决定。**
② 既然"减 9 % 入口 Ir"不动墙钟、而"删掉计数那一支"就收回全部差距 ⇒
**前四刀减在了不是墙钟主项的地方**；下一步要动的是计数本身。
③ `ownMap`（32 KB 直接映射）在 `bt` 上被查了 2.66 亿次却只值 **4 547** 次 D1 缺失，
`rebuild` 1.6 亿次 / **464** 次；回收元数据整体只占 `bt` 全部缺失的 12.3 %，
每 MiB slab 的元数据 ~0.23 MB 装得进 D1 ⇒ **不是缺失问题，是指令问题**。
④ `rebuild` 的回收侧是**广谱**的（`alloc` 与计数各 20 M 次调用）：全部计数消融只值
33 ms / 294 ms，它剩下的账（每槽清零、位图维护、20 M 次 slab 分配）还没单独量。

**方法学坑**：valgrind 3.26 的 `cachegrind` 默认 `--cache-sim=no`，不给这个开关的输出里
**只有 `Ir` 事件**、没有任何 D1/LL 列 —— 读到"缺失 0"要先怀疑没开开关。

---

## 11. 槽元数据瘦身（`task-19`）：每槽字节数与命名形状前后对照

两处改动都在 `src/back/reclaim.c` 的**发射文本**里（逐项独立可回退）：

1. **每槽类型描述符指针 → 16 位类型号**：`ExtcRcSlab.type`（`const ExtcRcType **`，8 B/槽）换成
   `tid`（`uint16_t`，2 B/槽）+ 每 slab 一张**小类型表** `ttab`（描述符按**指针相等**去重；号 0 =
   无描述符、1 = `EXTC_RC_CONSERVATIVE`、≥2 = 表下标）。表满（6.5 万个不同描述符挤进同一块 slab）
   当场 trap：静默退回 0 会让标记漏走这个对象的引用字段 ⇒ 提前释放（不安全方向）。
2. **槽步长从"2 的幂取整"改成"8 字节取整"**（下限 16 = 空闲链槽头）：80 字节的对象不再占
   128 字节的槽，24 字节的对象也不再多占 8。8 是够用的粒度，因为语言里最大自然对齐就是 8
   （判据 `tools/check_slot_unit.py`）。代价是
   "由对象指针找槽号"从一次移位变成一次**乘倒数右移**（`rcp = 2^42/ss + 1`，精确性判据见 §11.4）。
   "放不下两个"的阈值随之变成 `slotSize > (1 MiB - 头)/2`（超大块语义不变）。

### 11.1 每 slot 的元数据（`digraph` @1M，默认无计数档）

量具：在 `/tmp` 的**生成物副本**上给 `slabNew` / `alloc` / `freeSlot` 插计数并挂 `atexit` 打印
（工作树零改动）。口径是"全过程的 slab 普查"与"峰值同时存活"。

| 版本 | occ+mark+rsbit | 跨区入边计数 | 类型 | **合计 B/slot** | 尺寸档 | slab 数 | 峰值存活槽容量 |
|---|---|---|---|---|---|---|---|
| 改动前 | 0.375 | 2 | 8（指针）| **10.375** | 32 / 64 / 128 | 3 / 1 / 5 | 6 765 984 B |
| 只换类型号 | 0.375 | 2 | 2（号）| **4.375** | 32 / 64 / 128 | 3 / 1 / 5 | 6 765 984 B |
| 两项都做 | 0.375 | 2 | 2（号）| **4.375** | 32 / 48 / 80 | 3 / 1 / 3 | 5 139 536 B |

程序的普查行（改动前 / 两项都做，`stderr`）：

```text
# 改动前
[census] shift= 5 slotsize=      32 slabs=    3 slots=    98292 slotbytes=     3145344 meta/slot=10.375 peaklive=    65551
[census] shift= 6 slotsize=      64 slabs=    1 slots=    16382 slotbytes=     1048448 meta/slot=10.375 peaklive=     7299
[census] shift= 7 slotsize=     128 slabs=    5 slots=    40955 slotbytes=     5242240 meta/slot=10.375 peaklive=    32822
[census] TOTAL slots=155629 meta=1614658 slotbytes=9436032 meta/slot=10.375 allocs=3223802 bumps=0 peak_livebytes=6765984
# 两项都做
[census] ss=    32 slabs=   3 slots=   98286 slotbytes=    3145152 meta/slot= 4.376 peak_live=   66403
[census] ss=    48 slabs=   1 slots=   21841 slotbytes=    1048368 meta/slot= 4.382 peak_live=    7410
[census] ss=    80 slabs=   3 slots=   39312 slotbytes=    3144960 meta/slot= 4.379 peak_live=   33237
[census] TOTAL slots=159439 meta=697972 slotbytes=7338480 meta/slot=4.378 allocs=3223802 bumps=0 peak_livebytes=5139536
```

⇒ 元数据 **1 614 658 → 697 972 B（−57 %）**；槽容量 **9 436 032 → 7 338 480 B（−2.1 MB）**。

### 11.2 最终对照（每个形状每个编译器 **10 次**；min/中位/max；pin `build/extc` md5 `180e74b9…`）

```text
形状                                                 档           base min/med/max KB      t19d min/med/max KB      Δ中位  stdout
bench/gc/src/digraph.extc（@1M）                    named         11820/11820/11884           8748/8748/8812   -26.0%    same rc0
bench/gc/src/digraph.extc（@250k）                  named            7980/7980/8044           3180/3180/3244   -60.2%    same rc0
bench/gc/src/churn.extc                            named            1644/1644/1704           1644/1644/1708    +0.0%    same rc0
bench/gc/src/rebuild.extc                          named            1644/1644/1708           1644/1644/1708    +0.0%    same rc0
bench/bigmatrix/src/bt.extc                        named            9880/9880/9940           9880/9880/9948    +0.0%    same rc0
bench/bigmatrix/src/rebuild.extc                   named            1836/1836/1900           1836/1836/1900    +0.0%    same rc0
tests/reclaim-shapes/persist_nodes.extc            corpus        36972/36972/37036        32172/32172/32236   -13.0%    same rc0
tests/reclaim-gaps/shapes/base_call_unknown.extc   corpus        27180/27240/27244        24492/24524/24556   -10.0%    same rc0
tests/reclaim-gaps/shapes/slot_elem_unknown.extc   corpus        27180/27180/27244        24492/24492/24556    -9.9%    same rc0
tests/reclaim-gaps/shapes/dyn_payload.extc         corpus        26604/26604/26668        25836/25866/25900    -2.8%    same rc0
tests/reclaim-shapes/cross_region.extc             corpus        10668/10668/10732           8748/8748/8812   -18.0%    same rc0
tests/reclaim-shapes/cyclic_pressure.extc          corpus           7404/7404/7468           5676/5772/5928   -22.0%    same rc0
tests/reclaim-gaps/shapes/ring_per_round.extc      corpus           6636/6636/6700           4524/4524/4588   -31.8%    same rc0
tests/reclaim-shapes/cond_save.extc                small            1452/1452/1516           1440/1452/1516    +0.0%    same rc0
tests/reclaim-shapes/unknown_host.extc             small            1452/1452/1516           1452/1452/1516    +0.0%    same rc0
tests/reclaim-shapes/bounded_churn.extc            small            1452/1452/1516           1452/1452/1516    +0.0%    same rc0
tests/reclaim-gaps/shapes/closure_chain.extc       small            1440/1482/1516           1452/1452/1516    -2.0%    same rc0
```

**噪声地板**：小形状（1.4–1.9 MB）的 min/中位/max 在同一二进制上就横跨 **±15 页（≈60 KB）**
（glibc arena / THP / ASLR），所以这一档只声称"**不显著变差**"，改善只对 **≥ 数 MB** 的形状声称。
`bt`（②已判"不需要回收"⇒ 不发射 rc）**产物与关档逐字节相同**，它的 +0.0% 就是量具地板。

`digraph` @1M 的墙钟（10 次交错，`bench/gc/build/digraph_run --pin 0`）：

| | min | 中位 | max | RSS |
|---|---|---|---|---|
| 改动前 | 524.4 ms | 535.5 ms | 547.0 ms | 11 820–11 884 KB |
| 两项都做 | 480.2 ms | 487.4 ms | 499.0 ms | 8 748–8 812 KB |

两边 stdout 逐字相同（`nodes=14077 edges=28166 …`）⇒ 瘦身没有用语义换内存。

### 11.3 一个量出来的坑：`calloc` 的急切清零把 RSS 抬起来

只换类型号那一版在 46 个语料形状里让 **8 个小形状**的峰值涨了 300~450 KB（`cond_save`
1452→1836）。根因不是"数组变大"（其实是变小），而是 **glibc 的 mmap/brk 选择变了**：旧版每槽
`type` 8 B（16 B 槽时 512 KB）走 **mmap**（内核零页，只有写到的页常驻）；新版两个 2 B 数组
（各 128 KB）落在**堆**上，`calloc` 整片急切清零 ⇒ 多 ~256 KB 常驻。修法：三张位图（会被整片
顺序遍历）保持 `calloc`；**每槽那两个 16 位数组改 `malloc` + 在分配那一刻按槽清**。

那条修法引入一条**不变量**："只有占用槽才会被读 `hold[i]`/`rscnt[i]`/`tid[i]`"。逐读点审计：

| 读点 | 守卫 |
|---|---|
| `extc_rc_rsInc` / `rsDec` / `rsIncSlab` / `rsDecSlab` | 每处先 `if (!(s->occ[word] & bit)) return;`（`skip:occ-free`）|
| `extc_rc_rsCount` / `extc_rc_hold` | 同上 |
| `extc_rc_holdUpdSlab` / `holdUpd` | 同上 |
| `extc_rc_typeOf`（读 `tid`）| 三个调用点：`mark` / `releaseOld` 都在 `occ` 位判之后；`unlinkChildren` 只被 `freeSlot` 与"按 `occ` 位遍历"的 `regionFree` 调用 |

判据：`plain` / `MALLOC_PERTURB_=165` / `EXTC_ARENA_FILL=165` 三档跑 41 个回收语料形状
（N=200000），比对退出码 + stdout md5 ⇒ **不一致 0、崩溃 0**；全量
`tests/perturb/census.py --check --arms plain,42,165,afill`（1213 源 × 4 档）崩的源 0。

### 11.4 判据与牙

* **槽号索引（`slotindex.c`，挂 `gate.sh`）**：`#include` **发射出来的**运行期文本，用里面的
  `extc_rc_rcpOf` / `extc_rc_slotOf` 对 16..1 MiB 的每个 8 倍步长 × 全部合法槽号做域内穷举，
  另加"槽内偏移/ slab 头里的指针必须被拒"。实测 **1 433 436 组 0 处不符、1 433 499 处被拒**。
  牙：注入 `EXTC_RC_RCP_SHIFT` 42 → 19 ⇒ **140 384 处不符 + `slotindex: FAIL`，exit 1**。
* **性能线棘轮（`gate.sh`）**：`cyclic_pressure` N=200000 的 Ir
  **160 049 784（改动前）→ 165 100 449（只换类型号，+3.2 %）→ 154 214 566（两项都做，−3.6 %）**
  ⇒ 线从 168 050 000 收到 **161 930 000**（= 实测 × 1.05，只许往下）。

### 11.5 `gate-shard-cycles` 判据去腐

旧 ① 判"`digraph` 的 4N/N < 1.5×"。它隐含依赖**每槽元数据的常数地板**：改动前 250k 的 7 980 KB
里一大块是那个地板，把比值压在 1.49（贴着线、没有余量）；瘦身把地板拿掉以后，比值变成 2.70 ——
**两边绝对峰值都更好了**（−59 % / −26 %，开档/关档压缩比 0.228/0.087 → 0.093/0.064），红的只是
一个**在量"地板还在不在"**的代理量。改后的 ① 与**同 N 的关档基准**比，把常数项消掉：

| 判据 | 改动前 pin0 | 改动后 t19d | 注入 `sweep-never` |
|---|---|---|---|
| ①a `peak_on ≤ 0.5 × peak_off`（N 与 4N）| 0.228 / 0.087 ✓ | 0.092 / 0.065 ✓ | **1.232 / 1.233 ⇒ FAIL** |
| ①b `on_ratio < off_ratio` | 1.49 < 3.88 ✓ | 2.72 < 3.88 ✓ | **3.88 = 3.88 ⇒ FAIL** |
| ①c `SHARDS=4` 的 4N ≤ 2× 默认档 | ✓ | ✓ | **✓（仍绿）** |

①c 在注入下仍绿，说明三格互不掩盖；②（`shard_bands` 的停顿收益）保留。新判据仍然覆盖旧判据
原本保护的那件事：分片把环钉住时 `SHARDS=4` 会 ≈ 关档 ⇒ ①a 必红。

### 11.6 停顿与分项计数（`rc-pause-probe.py` + `EXTC_RC_TIME=1`）

`EXTC_RC_TIME=1` 的分项计数（`digraph` @1M，改动前 / 两项都做）：
`alloc` 3 223 802 / 3 223 802 · `storeReg` 7 670 189 / 7 670 189 · `storeOwned` 219 087 400 /
219 087 400 · `freeSlot` 3 166 259 / 3 166 259 · `regionNew` 1 / 1 · `slabNew` **9 / 7** ·
`collect` **146 / 156**。除了 `collect` 与 `slabNew`，其余逐字相同（语义没动）。

`tools/rc-pause-probe.py` 的 `PROBE` 行（3 次重跑的中位）：

| 形状 | 清扫次数 | 最大单次停顿 | 停顿合计 |
|---|---|---|---|
| `digraph` @250k | **67 → 50** ✓ | 623 → 514 µs ✓ | 8 987 → 8 416 µs ✓ |
| `cyclic_pressure` @800k | **28 → 25** ✓ | 891 → 646 µs ✓ | 14 153 → 12 608 µs ✓ |
| `digraph` @1M | **146 → 156**（+6.8 %）| 1 365 → 1 687 µs（区间重叠：1 269–1 723 vs 1 192–1 757）| 55 732 → 56 642 µs（+1.6 %）|

**`digraph` @1M 的清扫次数涨了 10 次**，而**释放槽总数逐字相同（3 166 259）**、156 次里 155 次
是真收到东西（改动前 146 次里 142 次）⇒ 同样的回收工作量被切成了更多批。机制：`bytes` / `live`
的记账单位换成了 8 字节取整的槽步长（改动前是 2 的幂步长），而触发式
`thr = max(2·live, floor)·scale` 里的 `floor` 是**绝对字节常数**，于是触发轨迹相对分配批次发生
相位移动。方向不系统：@250k 与 `cyclic_pressure` 都是次数更少、停顿更小，只有 @1M 这一档次数多
10 次、停顿合计 +1.6 %（最大停顿的 3 次区间重叠）。触发阈值属于**策略**（换它就是在换内存 ↔ 吞吐），
所以这里只报数，不顺手调常数。
