# 语料扩容：真实程序与对抗程序（tests/gc-torture）

本文记录 2026-12 语料扩容的**形状、判据与实测**。口径：本树 HEAD 的 build/extc（v31 发射文本，src/back/reclaim.c md5 29a6372eb0fac935f215847df35ccf41 / 4,780 行；src/back/codegen.c ec12011cc5bbff7100072bb465d8f234 / 16,346 行；build/extc f82ee508c6fe7233f228fc0decc00b9c），一台机器（Intel Core Ultra 9 275HX、WSL2、taskset -c 6）。

## 1 为什么要扩

既有语料 tests/ 有 1,396 个 .extc，但**总共 30,916 行，平均 22 行/个**：它们是按机制造的微形状（每个验证一条不变量），不是程序。census（paper/data/census.tsv）给出静态画像：

| 类别 | 程序数 | 占比 |
|---|---|---|
| 独立编译通过 | 794 | 100% |
| 不发射任何回收代码（T0） | 544 | 68.5% |
| 清扫档（T2） | 248 | 31.2% |
| 计数档（T1） | 2 | 0.3% |

结论：**承重的是 248 个 22 行形状**。真实程序只有 bench/app 三个（log/route/session，共 227 行）与 bench/crazy（JSON）。缺的是两件事：

1. **真实程序**：长生命周期数据结构（索引、图、CSR、哈希表）、多阶段活集、跨函数的分配/释放，用来回答「GC 在真程序里占多少、容量能不能收回」；
2. **对抗程序**：每个只针对运行期的一条脆弱路径（巨对象、深环、句柄失效、跨 yield 存储、copyInto 覆盖引用、稀疏活集钉住容量……），用来把「契约」从口号变成会失败的用例。

## 2 目录与运行

    tests/gc-torture/
      src/            *.extc（t01..t13 对抗程序 + datalog_lite 真程序）
      expected/       *.txt（golden：逐字 stdout）
      gen_graph.py    确定性图生成器（同一参数输出逐字相同）
      sample.py       RSS 采样器（2 ms 间隔，报告末态/峰值/四分之一后的增长）
      run.sh          all | quick | asan | ladder | one <name>

构建产物落在 OUT（默认 /tmp/gt-run），不入库。

## 3 判据（三条硬判据 + 一条归档）

1. **精确 stdout**：与 expected/<name>.txt 逐字相同（同输入 ⇒ 同输出）。as/UB 变异、句柄失效、别名复活都会改这一行；
2. **峰值 RSS 上界**：/usr/bin/time -f %M，每案一个上限（表 3）。它检验的是**容量归还**：程序把活集丢掉之后，堆不能停在历史峰值；
3. **停顿通道**（-DEXTC_RC_PAUSE=1 再跑一遍）：[histo] over1ms=0；[wbudget] pop_max<=512、push_max<=8448、mscap_max<=16384（三个编译期常量，见 GC-ROOFLINE §6.13）；
4. **xfail**：已归档的开放 RED。判据仍按 1~3 计算并打印观测值，但不算通过、也不算失败，原因写在 CASES 表里，修复后必须删掉该行的 xfail 字段（棘轮：只允许把 xfail 改成严格，不允许新增）。

另外两档：asan（-fsanitize=address,undefined，5 案，grep 不得出现 AddressSanitizer/runtime error:/LeakSanitizer）与 ladder（同一形状两档规模，只判 RSS 与停顿通道，不判 golden——两档参数不同，stdout 必然不同）。

## 4 清单与实测

表 4（MODE=all，2026-12 实测，OUT=/tmp/gt2）：

| 用例 | 参数 | 峰值 RSS | 墙钟 | over1ms | 观察 |
|---|---|---|---|---|---|
| t01_sparse_pin | 1000000 200 | 49,264 kB | 45 ms | 0 | 400 万结点双向链建满（活集 ≈207 MB）后断头，稀疏留 63 个 keeper；容量必须回落 |
| t02_fanout | 40 | 25,756 kB | 120 ms | 0 | 2 MiB 宽对象（262,144 个引用域，超 SCAN_CHUNK 8192 词）⇒ 分段扫描 |
| t03_deepring | 1000000 12 | 80,972 kB | 148 ms | 0 | 100 万结点双向环，每轮整环换根 ⇒ 整环不可达 |
| t04_bigchurn | 120 | 9,764 kB | 32 ms | 0 | 每轮 8 MiB 巨块 + 8 叶子，填完立刻丢根；v29 起撤销按 64 B 组位图走，见 §5.2 |
| t05_copyrefs | 800000 | 3,564 kB | 10 ms | 0 | copyInto 拷含引用元素（G14 路径），每 4096 轮散留 keeper |
| t06_handles | 8 200000 | 5,508 kB | 9 ms | 0 | pool 句柄：remove 后重读必须给 none（陈旧句柄不 trap），轮末 clear 让容量平台化 |
| t07_tasks | 20000 | 2,244 kB | 5 ms | 0 | 协程内建 varArray 跨 yield；偶数号任务取第一个值就 break（任务 arena 整块归还）；判据 extc_pool_live after==before |
| t08_mixed_sizes | 400 20000 | 3,056 kB | 63 ms | 0 | 8 个 size class（16/32/48/64/96/128/192/384 B）同时 churn |
| t09_cycle_pressure | 200000 20 | 14,892 kB | 123 ms | 0 | 20 万结点长链活集 + 每轮 80 万个互指 2-环（不可达但互指，只能清扫） |
| t10_alias_slots | —  | 1,864 kB | 4 ms | 0 | 参数槽覆盖后别名不复活、pool 句柄 stale 必须 none、copyInto 覆盖引用 |
| t12_overwrite_arena | 20 64 | 199,264 kB | 90 ms | 0 | 函数级缓冲在循环里被**同尺寸**替换（idle 判据 (d) 的形状）；峰值 = live + 两代垃圾 |
| t13_growth_copy | 20 8 | 194,352 kB | 224 ms | 0 | 函数级缓冲在循环里**拷贝增长**（1.1×，尺寸每轮都变）；峰值 = 闭包活集的常数倍，与轮数无关，见 §5.4 |
| datalog_lite | 200000 边/32 查询/50000 结点 | 17,456 kB | 396 ms | — | 506 行真程序：magic-set BFS + 双向 CSR 度分布 + 并集/交集 + 覆盖度，四阶段活集依次变大 |
| datalog_lite_small | 2000 边/4 查询/2000 结点 | 1,868 kB | 4 ms | — | 同上小档 |

ladder 档（只判 RSS/停顿）：

| 用例 | 小档 | 峰值 | 大档 | 峰值 |
|---|---|---|---|---|
| t01_sparse_pin | 250000 100 | 14,132 kB | 1000000 400 | 52,140 kB |
| t03_deepring | 250000 4 | 21,280 kB | 1000000 12 | 80,856 kB |
| t09_cycle_pressure | 50000 8 | 5,104 kB | 200000 20 | 14,892 kB |
| datalog_lite | 50000 边 | 6,948 kB | 200000 边 | 17,460 kB |

读法：峰值随**活集**走（t01 的活集是 4M×24 B 结点，t03 是 24 B/结点的环），不是随轮数走；轮数翻倍不改峰值（t08 100 轮 3,056 kB vs 400 轮 3,052 kB）。

## 5 本轮发现

### 5.1 t01：容量归还确实发生，但峰值由活集决定

同一程序在 1600/6400 轮下末态 RSS 是 97,436 / 22,856 kB（峰值 207,892 kB）——**向下收敛**，说明 slab/页归还路径在真实分配序列上生效。峰值本身是 4M 结点的活集，不是泄漏。

### 5.2 t04：清扫释放巨对象时，一步的工作量正比于它的引用域个数（v29 已闭合）

现象（可复现，v28 构建）：[step] n=238，其中 1~5 步 >1 ms，最长 1,139–1,529 µs；[top] rank=1 恒为 kind=sweep why=finish，freed=1、words=1。

定位（在生成 C 上逐层加时，未改源）：

    extc_rc_sweepWord            ← 一次调用 0.27–1.45 ms
      extc_rc_freeSlotWord(s, w, dead)
        if (extc_rc_rsIncN) extc_rc_unlinkChildren(s, base + b);   ← 占其中 ≈99.9%
          for (w = 0; w < ss/8; w++) { void *c = ((void**)p)[w]; ... }   ← 保守描述符：8,388,616/8 = 1,048,576 个域

机制：t04 的对象是 8 MiB 的「结构体内联 1M 个可空引用」的巨块，描述符因域数超上限而退回 EXTC_RC_CONSERVATIVE；进程里一旦出现跨区边（本例 rs_inc=960、rs_dec=952、rs_live=8），释放任何槽都要先把它指向别区的引用逐个撤销计数；对这样一个槽，撤销就是一整圈 1M 个域的遍历。t01 的 rs_inc=0，同一个 freeSlotWord 走 rsIncN==0 的短路，所以不出现。

已关闭（v29，提交 9bd1cccd）：在既有的跨区登记分支上顺手记一位「哪个 64 B 组写过跨区指针」（`src/back/reclaim.c` 的 `extc_rc_wmarkSlot`），释放时只读置位组的 8 个词。契约变成 `W_unlink(ss, m) ≤ min(ss/8, 8·m)`，m = 置位组数 —— 上界不再由对象大小单独决定，而由真实跨区出边的分布决定。已否证的另一条路（把 walk 摊到多步）见 `docs/analysis/UNLINK-DEFERRAL.md`：停顿上界与滞留上界是同一旋钮两端。

判据（同一份生成 C 只差 `-DEXTC_RC_WMARK=0/1`，120 轮、3 rep）：`[step] max_us` 1106.4 / 871.6 / 671.6 → **4.7 / 11.5 / 16.7**，over1ms 1 / 0 / 0 → **0**，`[step] total_us` 43,126 → 170–183；`rs_inc / rs_dec / rs_live` 960 / 952 / 8 逐字不变；本表 RSS 9,764 kB、墙钟 32 ms。撤销侧不再是无界项，剩下的「稠密侧」没闭合（每个 64 B 组都写过跨区指针时上界回到 ss/8），常见度取证见 `docs/analysis/WMARK-REACH.md`。

复现命令（约 5 秒）：

    ./build/extc tests/gc-torture/src/t04_bigchurn.extc -o /tmp/t04.c
    cc -O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L -DEXTC_RC_PAUSE=1 -o /tmp/t04 /tmp/t04.c
    EXTC_RC_PAUSE=1 taskset -c 6 /tmp/t04 120 2>&1 >/dev/null | grep "\[step\]"

### 5.3 t09：偶发 over1ms 属环境地板

首次全量跑出现 1 次 over1ms；同一二进制连跑 5 次全是 0（max_us 69.2 / 91.2 / 123.8 / 221.2 / 306.8，over256=0/0/0/0/1）。与 R3 记录的环境地板（纯自旋程序 wall_max 88–236 µs）同源：这台机器的抢占会让极少数步跨过 1 ms。判据保持严格（不放宽 over1ms=0），观测上把它记为环境项。

### 5.4 t13：拷贝增长的峰值曾经等于各代之和（v31 已闭合）

形状：函数级 `var b: mut slice<u8>` 在循环里被替换成 `new u8[cap]`，`cap` 每轮乘 1.1（起始 8 MiB、20 轮 ⇒ 末代 53 MiB），每代触页后只留校验和。

v30 的读数：`[step]` 的收集确实在跑（criterion (d) 已让它进清扫档），但截峰值 **526,816 kB** —— 巨块载荷从不归还（`extc_rc_freeSlot` 只清 occ，`slabShrink`/`pageTrimStep` 都 `if (s->big) continue`），所以容量 = 各代之和。v31 的读数：**194,352 kB / 224 ms / over1ms=0**（= 闭包活集 53 MB 的 3.6 倍，与轮数无关）。

判据：峰值 RSS ≤ 260,000 kB —— **v30 红（526,816 > 260,000）、v31 绿（194,352）**，这是本用例先红后绿的证据；机制见 `MEMORY-MODEL-CURRENT.md` §19.15 与 `GC-ROOFLINE.md` §6.20。

## 6 还没覆盖的（下一步）

1. **并发/协程真程序**：t07 只覆盖任务 arena 的归还；par worker 的每 worker 一区、跨区注册、任务根登记仍只有微形状；
2. **与 C 库/系统交互的程序**：plate（mmap）路径在语料里只被 stdlib 内部用到；
3. **规模更大的真程序**：datalog_lite 是 506 行单文件；要检验「程序变大时 68.5% 这个比例怎么变」，需要 2–5k 行的多文件程序；
4. **稠密侧与 L10-c**：t04 的 xfail 已删（v29，提交 9bd1cccd）；仍缺 ① 一个「每个 64 B 组都写跨区指针」的用例，用来测 `W_unlink` 的右端；② `tests/gc-p0/src/leak_copy_rc_churn.extc` 的 L10-c 泄漏（覆盖已登记入边时不撤销）。常见度取证见 `docs/analysis/WMARK-REACH.md`。

## 7 复现

    make build/extc -j8
    ./tests/gc-torture/run.sh all      # 14 案，期望：通过 14，xfail 0，失败 0
    ./tests/gc-torture/run.sh asan     # 5 案
    ./tests/gc-torture/run.sh ladder   # 8 次运行
    ./tests/gc-torture/run.sh quick    # 3 案（编辑循环）
