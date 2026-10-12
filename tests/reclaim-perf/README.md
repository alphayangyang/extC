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

**1412 项根表**从哪来：`world` 这个帧内结构体的**全部 `ref` 槽**——`cache[1024]` + `tree[256]`
+ `bkHead[64]` + `bkTail[64]` + `last`/`idxHead`/`retire` = **1411**，两个站点再加当轮新分配的
局部 `p` ⇒ **1412**。编译器是**保守**发的：不管那个槽此刻有没有值，整条数组都进根表。

**"只装此刻活着的根"救不了**：稳态下根表**本来就是满的**（N=8e5 时站点 0/2 平均
1354/1412 = **95.9 %** 非空；永久死槽只占物化总量的 **2.3 %**）。
所以正确的做法是**别在分配点建表**，而不是"建小一点的表"。

**下一步动作（按 收益/成本 排）**：

1. **分配点入口守卫**（`if (shouldSweep(r))` 再建表）—— 收益 **56 %**，改动约 10 行 codegen，风险≈0。
2. **`owns` 登记表查询 O(1) 化**（直接映射 + 校验，碰撞回落二分）—— 收益 **~10 %**，改动在运行时，风险低。
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
| **1** | **分配点入口守卫** | **56.1 %**（N=8e5 实测） | **极低**：codegen 把 `extc_rc_maybeCollect(r, (void*[]){…}, n)` 包成 `{ ExtcRcRegion *t = r; if (extc_rc_shouldSweep(t)) extc_rc_maybeCollect(t, (void*[]){…}, n); }` | **≈0**：`maybeCollect` 第一句就是同一判据 ⇒ 语义逐字等价 | `tests/reclaim-shapes/run-switch.sh`（9 条全绿）+ `run-evidence.sh`（清扫确实上场） |
| **2** | **登记表查询 O(1) 化** | **10.3 %**（N=2e5）；`ownLower` 自身 11.9 % 是上界 | 低：运行时加一张直接映射表，`ownAdd`/`ownDel` 维护，命中即用、未命中回落二分 | **低**：回落路径保证语义不变（`guardfast` 原型已验证输出逐字一致） | `tests/reclaim-shapes/run-switch.sh` + 4 个 SEGV 用例 |
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
| `ablate.py` | 单项消融/原型：`guardfast`（登记表 O(1)）/ `guardfirst`（入口守卫）/ `nohold`（拆计数，上界）/ `noreg`、`noreg0`（拆跨区登记）/ `padN`（根表补空到 N 项） | `nohold`/`noreg` 会改健全性或内存行为，**只能当上界**；`guardfast`/`guardfirst`/`noreg0`/`padN` 语义不变（可验证输出逐字一致） |
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
* **`guardfirst` 没有在真实 codegen 里落地过**：本目录全是"改 `/tmp` 生成物副本"的原型
  （`src/` 一个字节没动）。把它落进 `src/back/codegen.c` 之后的实际数字（尤其
  `tests/reclaim-shapes/run-switch.sh` 的 9 条门禁）**未验证**。
