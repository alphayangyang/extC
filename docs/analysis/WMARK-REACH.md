# wmark 的覆盖面：大槽类型与跨区边在语料里的常见度

**口径。** v29 运行期（`src/back/reclaim.c` = `845c57e2b230202efcd04639e07664d9` / 4,738 行、`build/extc` = `b5ad0e7cdcb7abbf0cd8f8f18b113570`、`src/back/codegen.c` = `3ae6e499b6a2d64668ff1daf2eed26a2` / 16,222）。本文件回答一个问题：v29 的 64 B 组位图登记（`[rt:wmark]`）服务的是哪种程序的哪种形状，这种形状在本仓语料里出现得多不多。

三件事必须分开，它们的机制、量具与判据都不同：

1. **大槽类型** —— 类型描述符超过 32 项 ⇒ 分配点写 `EXTC_RC_CONSERVATIVE`（`src/back/codegen.c:3888`）。代价在**标记**侧：每一步按 `EXTC_RC_SCAN_CHUNK` 词续扫，是有界的。
2. **跨区边** —— `extc_rc_rsIncN > 0`。这是 `extc_rc_unlinkChildren` 唯一的入口（四个调用点全在 `if (extc_rc_rsIncN)` 之下）。代价在**撤销**侧：一个死槽的出边要一次走完，v29 之前按 `ss/8` 计。
3. **稠密跨区写** —— 对象的每个 64 B 组都被写过跨区指针，位图回答不出「哪几组可能有事」，`W_unlink` 的右端 `8·m` 退化回左端 `ss/8`。

wmark 的收益只在 1×2×3 的交集上：位图把「一步扫 `ss/8` 个域」换成「一步扫 `8·m` 个域」，`m` 是置位的 64 B 组数。所以「常见吗」要按这三层分别回答。

## 1. 结论表

| 层 | 方法 | 结果 |
| --- | --- | --- |
| 大槽类型（>32 域） | 静态：全语料逐个编译，读编译器发射的描述符 | **18 个程序 / 21 个类型**（1,530 个 .extc 中的 1.2%），全部是基准与压测形状；**真实应用 0** |
| 跨区边（`rsIncN > 0`） | 动态：9 个真实/基准程序 + 527 个可跑测试程序 | 9 个真实/基准程序**全 0**；测试语料 **6 / 527 = 1.1%** |
| 稠密跨区写（`m → ss/4096`） | 动态：t04 的位图命中数与组数之比 | **从未出现**；t04 的密度是 **0.73%**（960 条边 / 131,072 组） |

一句话：**大槽在基准里是常态、在真实应用里为 0；跨区边只在 6 个测试程序里出现；二者同时成立只有 t04；稠密侧没有样本。** 这是语料的性质，不是语言的不变量 —— 第 4 节的尺寸扫描证明跨区边不需要大槽（`[32]?mut ref` 就能产生 320 条）。

## 2. 静态：大槽类型

**机制。** `rcEmitTypeDesc` 把类型的引用偏移收集进 `Vec offs`（`rcCollectOffsets`，收集上限 4096），项数 >32 时返回 `((const ExtcRcType *)-1)` ⇒ 运行期走保守扫描。所以「大槽」是**发射出来的描述符**的属性，不是源码里有没有 `[N]?mut ref` 的属性：字段数是收集到的引用偏移总数，嵌套结构体会被摊平。

**方法。** 临时在 `src/back/codegen.c` 的 `if (offs.len > 32)` 上一行插入一行报告（不留在树里）：

```text
if (getenv("EXTC_FANOUT_REPORT")) fprintf(stderr, "[fanout] n=%zu\n", offs.len);
```

用 `make -j8 BUILDDIR=build-fan build-fan/extc` 构建（`BUILDDIR` 必须是树内相对路径：Makefile 的 `$(EMBED)` 调用带 `./` 前缀，绝对路径会 `No such file or directory`），然后对 `find tests bench -name "*.extc"` 的每个文件跑 `EXTC_FANOUT_REPORT=1 build-fan/extc f -o /tmp/out.c`，只收 stderr 的 `[fanout]` 行。

**结果（1,530 个文件、405 个描述符）。**

* 230 个程序至少发射一个描述符；
* **18 个程序含 >32 域的类型，共 21 个**；项数分布：`64 ×3、256 ×3、1024 ×2、2048 ×3、4096 ×1、4097 ×9` —— `4097` 是「收集上限 4096，再加一项」的哨兵值，代表字段数超过 4096 的类型；
* 全部 18 个都是基准或压测形状，按目录：`bench/gc/v2` 4（chase 的 `graph.n: [1048576]?mut ref node` ⇒ 4097、promote 的 `table.b: [65536]…` ⇒ 4097、sizeratio 的 4×1024 ⇒ 4096、cycleratio 的 `[256]?mut ref` ⇒ 256）、`bench/gc/src` 与 `bench/gc/stamped` 5（spectrum_p4 / human_a 的 `[2048]?mut ref`、spectrum_p5 的 `[1024]` 与 `[256]`，各含 stamped 副本）、`tests/gc-torture` 2（t02 的 `[262144]`、t04 的 `[1048576]` ⇒ 4097）、`tests/gc-p0` 3（lat_randchain 两个 4097、pd_bigobj 4097、pd_pointer_dense 4097）、`tests/reclaim-perf` 4（scanchunk 4097，rg_evict_cache / rg_insert_hash / rg_walk_buckets 各 64）。

**这条路径在基准里是常态，不是边角。** `bench/gc/v2/src/chase.extc` 生成的 C 里，整张图对象就是以保守描述符分配的：

```text
graph * g = ((graph *)extc_rc_alloc(__extc_rcr0, (int64_t)sizeof(graph), ((const ExtcRcType *)-1)));
```

所以四个 gcbench 形状全部命中大槽分支；它们没有跨区边（第 3 节），于是保守扫描只发生在**标记**侧（每步 ≤ `EXTC_RC_SCAN_CHUNK` 词，有界），撤销侧那一整段代码不执行。

**真实应用是 0。** `bench/app/log.extc`、`bench/app/route.extc`、`bench/app/session.extc`、`tests/gc-torture/src/datalog_lite.extc` 生成的 C 里 `extc_rc_` 出现 **0 次**（纯 arena，T0）：连描述符都不发射，大槽、跨区边、位图都无从谈起。`alloc<T>(n)` 也不是来源：语料里带引用元素的 `alloc<…ref>` 出现 0 次，`vector<T>` 走 pool（`stdlib/std/data/vector.extc` 的 `syspool::extc_pool_new` + `poolSliceRaw`），不在 rc 区。

## 3. 动态：跨区边

**机制。** `extc_rc_rsIncN` 统计「目标槽所在的 slab 与源槽不在同一个 region」的登记次数；`extc_rc_unlinkChildren` 的四个调用点（`src/back/reclaim.c` 的 `2094 / 2261 / 2294 / 2334`）全在 `if (extc_rc_rsIncN)` 之下，发射文本里那句注释直说「默认构建里 `rsIncN` 恒为 0，整段不执行」。统计用运行期环境变量 `EXTC_RC_STATS=1`（`atexit` 时读 `getenv` 并打印 `extc_rc_stats rs_inc=… rs_dec=… rs_live=…`）；编译期 `-D` 不是开关。

**真实程序与跨语言基准（9 个，`taskset -c 6`）。**

| 程序 | `rs_inc` | 说明 |
| --- | --- | --- |
| `bench/app/log.extc` / `route.extc` / `session.extc` | 不发射回收代码 | 纯 arena |
| `bench/crazy`（44 MB JSON） | 0 | 语料里最大的真实负载 |
| `bench/gc/src/digraph.extc` 250k | 0 | 跨语言基准 |
| `bench/gc/v2/src/chase.extc` 1M | 0 | 跨语言基准（活集 66 MB） |
| `bench/gc/v2/src/promote.extc` 500k | 0 | 跨语言基准 |
| `bench/gc/v2/src/sizeratio.extc` 2M | 0 | 跨语言基准 |
| `bench/gc/v2/src/cycleratio.extc` 400k | 0 | 跨语言基准 |
| `tests/gc-torture/src/t02_fanout.extc` 40 | 0 | 2 MiB 宽对象 = 262,144 个引用域 |
| `tests/gc-torture/src/t04_bigchurn.extc` 120 | **960 / 952 / 8** | 唯一非零 |

**测试语料（527 个能编能跑的程序）。**

| 类别 | 数量 | 占比 |
| --- | --- | --- |
| 纯 arena（不发射回收代码） | 237 | 45.0% |
| 发射回收代码但 `rs_inc = 0` | 190 | 36.1% |
| **有跨区边** | **6** | **1.1%** |
| 编译失败（负向测试/多模块） | 87 | — |
| 没有 `[stats]` 行 | 7 | — |

扫描范围：`tests/reclaim-shapes tests/reclaim-gaps/shapes tests/gc-p0/src tests/gc-torture/src tests/escape-fuzz/cases tests/stl tests/pool tests/ops tests/impl tests/par tests/arena-promoted tests/coro tests/io tests/lambda tests/pin`；每个文件 `./build/extc f -o s.c` 再 `cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L`，运行时 `EXTC_RC_STATS=1 timeout 10 taskset -c 6`。

6 个非零项：`tests/gc-p0/src/leak_copy_rc_churn.extc` **50,000 / 0**（L10-c 归档的已知限制：`copyInto` 覆盖 `dst[0].r` 时旧目标的跨区入边不撤销 ⇒ 每轮留下一个活种子、线性泄漏，XFAIL，修复方向 P0-10；与位图无关，见第 6 节）、`tests/gc-torture/src/t04_bigchurn.extc` 960/952、`tests/reclaim-shapes/cross_region.extc` 8、`tests/reclaim-shapes/cross_region_elem.extc` 8、`tests/reclaim-shapes/struct_copy.extc` 8、`tests/arena-promoted/D1_assoc_publish_home.extc` 1。

## 4. 跨区边从哪里来

**判定条件与大小无关。** t04 生成的 C 里两个 region 的键是写死的：

```text
huge * a = … extc_rc_regionOf((void *)&(__extc_a[2]), 2);      /* 循环内 new 的宿主 */
leaf * l = … extc_rc_regionOf((void *)&((*__extc_home)), -1);  /* 被调函数里 new 的元素 */
```

`main` 传进 `touch(huge *b, …)` 的 `__extc_home` 指向 `__extc_a[1]`，而 `huge` 自己在 `__extc_a[2]` —— 两个键不同，于是 `b.a[i] = p` 每写一次都是一条跨区边。

**反例（同一形态、宿主提到循环外）。** 探针 `/tmp/common/probe_helper.extc`：`struct tbl { head: ?mut ref node  n: i64 }`，`fn insert(t: mut ref tbl, v: i64)` 里 `new node` 存进 `t.head`，`main` 在循环外 `var t: mut ref tbl = new tbl` 再 insert 两万次 ⇒ `t` 走 `regionOf((void *)&(__extc_a[1]), 1)`，与 `node` 的 home 区同键，`rs_inc = 0`。

**尺寸扫描（`/tmp/common/sweep.sh`）。** 生成的形状：`struct holder { a: [N]?mut ref leaf  tag: i64 }`，`fn touch(h: mut ref holder, r: i64)` 里 8 次 `new leaf` 存进 `h.a[i]`，`main` 40 轮循环内各 `new holder` 一次。

| `N` | 32 | 64 | 1024 | 8192 | 65,536 | 262,144 | 1,048,576 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `rs_inc` | 320 | 320 | 320 | 320 | 320 | 320 | 320 |
| `rs_dec` | 0 | 0 | 0 | 256 | 304 | 312 | 312 |

`rs_inc` 恒为 320 = 40 轮 × 8 次写入，与 `N` 无关 ⇒ **跨区边不需要大槽**，`[32]?mut ref` 就够。语料里它稀有，是因为基准写法把宿主放在函数级 arena（同键）、真实应用根本不进 rc 区（T0）。

## 5. 位图密度与覆盖缺口

* t04 置位的 64 B 组数 = 960 /（`ss/64` = 131,072）= **0.73%**；「每个组都置位」的稠密样本 **0 个**，所以契约 `W_unlink(ss, m) ≤ min(ss/8, 8·m)` 的右端（`8·m`，`m → ss/4096`）**没有被任何用例验证**。
* 位图只在 `ss ≥ EXTC_RC_WMARK_MIN_BYTES`（4096）时分配；更小的槽走 `ss/8 ≤ 512` 词 ≈0.4 µs，本来就有界。
* 位图是超集（真值在释放时从内存重读），所以它不是写屏障、热路径只多一次寄存器传参；代价是每块 `nslots × wmW` 词（512 KiB 块 ≈1 KiB，8 MiB 单槽块 16 KiB）。

## 6. 结论与下一步

* v29 的收益面在语料里是 **1 个程序**（t04：1.1 ms → 5–17 µs），换来的是一条**契约**：一步内撤销一个死槽的工作量从「对象多大就多大」变成「按置位组数计」，而普通对象零成本（不建位图、分配路径不变）。
* 「大槽」本身在基准里是常态（四个 gcbench 形状全命中，chase 的整张图就是保守分配），但它的代价在标记侧、已经按 `SCAN_CHUNK` 有界；wmark 补的是撤销侧那条一次走完的路径。
* 「跨区边」稀有是语料的性质：换真实程序前重新 census，一条命令即可（`EXTC_RC_STATS=1`）。
* 两个开放项：①**稠密侧**没有用例 —— 需要一个「每个 64 B 组都写跨区指针」的 torture 用例，让 `min` 的右端第一次被测到（预期停在 `ss/8`，那时 A 路线＝契约显式化、C 路线＝引用数组落成槽才有意义）；②**L10-c** 在当前构建仍在泄漏（`rs_inc=50,000 / rs_dec=0`），与位图无关，是覆盖已登记入边时不撤销的既有缺口。

## 7. 复现

**静态普查。** 写一个 `/tmp/fan/patch.py`：读 `src/back/codegen.c`，在唯一的 `if (offs.len > 32) return …` 行前插入第 2 节那行 `fprintf`，写回文件；然后

```sh
make -j8 BUILDDIR=build-fan build-fan/extc        # 必须是树内相对路径
for f in $(find tests bench -name "*.extc"); do
  EXTC_FANOUT_REPORT=1 build-fan/extc "$f" -o /tmp/o.c 2>&1 >/dev/null \
    | grep "^\[fanout\]" | sed "s/\[fanout\] n=//"
done
git checkout -- src/back/codegen.c && rm -rf build-fan
```

**动态统计。** `cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L gen.c -o gen`，然后 `EXTC_RC_STATS=1 taskset -c 6 timeout 10 ./gen`，读 stderr 的 `extc_rc_stats rs_inc=…` 行。

**尺寸扫描。** 按第 4 节的形状生成 `N ∈ {32, 64, 1024, 8192, 65536, 262144, 1048576}` 七份 `.extc`，各自统计一次。
