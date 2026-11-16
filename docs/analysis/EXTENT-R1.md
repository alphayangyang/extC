# R1：extent 的三个未知量（VA 预算 / THP×归还 / slack 策略）

> 口径：本轮无 `src/` 改动，只做探针。`taskset -c 6`，`cc -O2 -std=c11`（C++ 臂 `c++ -O2 -std=c++17`），
> 形状 = 8,010,895 个 `i64`（64 MiB 活集）按 1.1× 增长，与 `grow11` / `t13_growth_copy` 同族。
> 探针：`tools/probe/vaarena.c`、`tools/probe/thpdrop.c`、`tools/probe/slackgrow.c`、`tools/probe/vectorbench.cpp`。

## 0. 三个问题

1. **VA 预算**：extent 的 slack 只吃虚拟地址。每 region 用一个多大粒度的子系统分配？碎片能不能毁掉大 extent 的连续性？
2. **THP × 归还**：`MADV_DONTNEED` 会拆掉 2 MiB 页；归还-复用如果是常态（`t12` 就是），THP 的收益还剩多少？
3. **slack 策略**：预留多少才能让增长变成 0 次拷贝？

## 1. 结论先写（四条，后面是数据）

1. **carve 比 mmap 便宜 270 倍**：共享预留里用 2 MiB 量子切分，切一个 extent **7 ns**；每 extent 一次 `mmap` 是 **1,897 ns**。10,000 个 extent 占 **58.6 GiB VA、2.3 MB RSS**（VA 不是内存，这条实测成立）。代价是**预算**：64 GiB 的 arena 装 10,923 个就满了（`fail=89,077`），所以要有 per-region 的 VA 预算 + 「切不动就补一块」的 refill。
2. **碎片被 2 MiB 量子和 refill 管住**：8 GiB arena 填满 4,000 个 extent、随机释放一半后，最大可分配块仍有 **128 MiB**（order 6），4 量子的请求照常成功；真切不动时 refill 一次 `mmap` 就恢复。
3. **THP 与 `MADV_DONTNEED` 不冲突**：丢弃后重新触页，`AnonHugePages` **回到满额**（64 MiB → 65,536 kB、256 MiB → 262,144 kB）。而重触成本 **99 ns/KB vs 4 KiB 页 260–279 ns/KB（2.7×）**，丢弃本身 **2.7 ns/KB vs 19 ns/KB（7×）** ⇒ 归还 pass 在有 THP 的 extent 上更便宜，S1 标定的 `c_cap = (13 + 233·p) ns/KB` 继续是保守上界。
4. **slack 本身不消拷贝，只有「按终值预留」才消**：`slack4`（max = 4×当前容量）在 64 MiB 形状上仍拷贝 **6 次 / 49.9 MB**；固定预留 1 GiB 的臂是 **0 次拷贝 / 20.2–21.0 ms**。所以策略必须是「一次留够 VA」，而不是「长到头再换一块大的」。

## 2. VA 子系统：carve 7 ns，mmap 1,897 ns

| 方式 | 10,000 个 extent | 每 extent | VA 占用 | RSS | VMA |
|---|---|---|---|---|---|
| 共享预留里 buddy 切分（2 MiB 量子） | 0.075 ms | **7 ns** | 58.6 GiB | 2,284 kB | 26 |
| 每 extent 一次 `mmap(MAP_NORESERVE)` | 18.97 ms | 1,897 ns | 58.6 GiB | 42,128 kB | 25 |

（`mmap` 臂的 RSS 42 MB 是它自带的 10,000 次首触，不是映射本身的成本。）
预算与碎片：

| 实验 | 结果 |
|---|---|
| 64 GiB arena，要 100,000 个 extent | 成功 **10,923** 个后耗尽（fail 89,077）⇒ 每个 extent 平均要 6 MiB VA |
| 8 GiB arena 填 4,000 个 extent、释放一半 | 最大可分配 **128 MiB**；4 量子（8 MiB）请求仍 ok |

读法：共享 arena 的 per-extent 成本可以忽略，但**必须**配一个 VA 预算（每 region 多少 GiB）和「切不动就 `mmap` 补一块 1 GiB 对齐段」的 refill；否则大数组 Creator 一多就把 arena 耗光。

## 3. THP × 归还：大页丢得掉、也回得来

| 映射 | 规模 | 首次触页 ms | `MADV_DONTNEED` ms | 重触 ms | `AnonHugePages`（重触后） |
|---|---|---|---|---|---|
| 4 KiB | 64 MiB | 17.19 | 1.13–1.77 | 17.67–20.94 | 0 |
| THP | 64 MiB | 19.59 | **0.10–0.23** | **5.81–6.92** | 65,536 kB（满额） |
| 4 KiB | 256 MiB | 73.30 | 4.93–5.12 | 66.51–71.38 | 0 |
| THP | 256 MiB | 59.91 | **0.70–0.71** | **24.63–25.37** | 262,144 kB（满额） |

折算成价目：重触 256 MiB 是 **99 ns/KB**（THP）vs 260–279 ns/KB（4 KiB）；丢弃是 **2.7 ns/KB** vs 19 ns/KB。
⇒ S1 的归还常数**不用改**（它已经是保守上界），但「归还-复用」在 THP 后从 246 ns/KB 降到约 102 ns/KB；对 `t12` 那种「刚还完就复用」的形状，THP 把 S1 引入的代价也砍掉一半以上。

## 4. 增长：和 `std::vector` 正面对比

同一形状（8,010,895 个 `i64`、64 MiB 活集），所有臂的校验和逐字相同：

| 臂 | 增长 ms | 拷贝次数 | 拷贝量 MB | 峰值 RSS kB | 备注 |
|---|---|---|---|---|---|
| `std::vector` push_back（2× 增长，惯用法） | 48.3–49.5 | 23 | 64.0 | 69,348–69,412 | 终容量 8,388,608 |
| `std::vector` reserve(8M)（**必须预先知道 N**） | 21.9–22.1 | 0 | 0 | 65,952–66,108 | 终容量 8,000,000 |
| `std::vector` 1.1× 应用侧 resize（与本节形状同因子） | 73.1–79.7 | 14 | 96.8 | 102,452–102,556 | 终容量 12,037,404（容量就浪费 1.5×） |
| 本方案 copy（2× 搬，`mmap`+`memcpy`） | 44.2–45.7 | 11 | 58.8 | 65,692–65,696 | 对照组 |
| 本方案 slack2（max = 2× 容量） | 44.8–45.5 | 11 | 58.8 | 65,692–65,696 | **和 copy 一样** |
| 本方案 slack4（max = 4× 容量） | 40.2–40.6 | 6 | 49.9 | 79,212 | 仍要搬 |
| 本方案 **big**（一次预留 1 GiB VA） | 20.2–21.0 | **0** | **0** | **64,052** | 与 vector::reserve 同价，但**不需要知道 N** |
| 本方案 **bighuge**（同上 + `MADV_HUGEPAGE`） | **7.08–7.23** | **0** | **0** | 65,040 | 一次 THP 回退时 17.86 |

公平的读法：

- `std::vector::reserve` 是 C++ 能做到的最好，但它要求**你在增长之前就知道终值**；`push_back` 不知道，代价是 23 次 realloc、48 ms、69 MB。
- 我们的 0 拷贝臂（20.2 ms）与 `reserve` 同价（21.9 ms），但**不需要知道 N**；加 THP 之后 7.1 ms 是 C++ 两种写法都到不了的（3.1× 快于 reserve、6.8× 快于 push_back），因为 `std::vector` 没法让地址不变地长。
- 「1.1× 应用侧 resize」这一行就是 C++ 用户今天被迫接受的现实：73–80 ms、102 MB、容量浪费 1.5× —— 也是「老是拷贝」这句话的价目表。

## 5. 由这三项定下的设计参数

```
extent = 从 region 的 VA arena 切出的一段 2 MiB 对齐区间
  VA arena：buddy，量子 2 MiB；预算用满时 mmap 再补一段 1 GiB（MAP_NORESERVE）
  slack：max = clamp(16 × 请求, 2 MiB, 1 GiB)；超出 max 时整块搬到 2 × max（摊还，拷贝量 ≈ 2 × 终值）
  提交：写到哪算到哪（缺页零填），增长 = bump，0 系统调用
  大页：整段 MADV_HUGEPAGE（THP 在时首触/重触都快 2.7–3×，不在时也成立）
  归还：MADV_DONTNEED 后把 VA 还回 buddy（2.7 ns/KB 丢弃、99 ns/KB 重触）
```

VA 预算的算术：16× slack ⇒ 每 GiB 活的大数组占 16 GiB VA；47 位用户空间 128 TiB ⇒ 约 8,000 GiB 活大数组，远超我们关心的规模。per-region 预算取 256 GiB 并允许 refill 即可。

## 6. 对 R2–R4 的修订（下一步）

| 步 | 修订后的内容 |
|---|---|
| R2 账本 | extent 登记进 region 账本（提交字节 O(1)）；归还 pass 从 slab/页改成 extent 级 ⇒ 同时消 pin（L10-a/G15） |
| R3 巨块 | 巨块载荷 = 一个 16× slack 的 extent；删 `bigPayloadAlloc`/`bigTrimStep`/`[chan] bigdrop` |
| R4 容器 | 容器存储 = 可增长 extent（`mem::grow` 有 slack 就原地长）；删 stdlib 池 570 行 |
| R5 词汇 | 一套容量 API、旋钮 ≤12 |

验收目标（R4 之后）：与本节 `big` / `bighuge` 臂同阶 —— 增长 ≤30 ms（无 THP）、≤10 ms（有 THP），峰值 ≤1.1× 活集，对 `std::vector::push_back` 至少 1.6×、对 `reserve` 不劣。

## 7. 复现

```
cc -O2 -std=c11 -o /tmp/r1/vaarena tools/probe/vaarena.c
cc -O2 -std=c11 -o /tmp/r1/thpdrop tools/probe/thpdrop.c
cc -O2 -std=c11 -o /tmp/r1/slackgrow tools/probe/slackgrow.c
c++ -O2 -std=c++17 -o /tmp/r1/vectorbench tools/probe/vectorbench.cpp
taskset -c 6 /tmp/r1/vaarena shared 10000 2      # 7 ns/extent, 58.6 GiB VA, 2.3 MB RSS
taskset -c 6 /tmp/r1/vaarena mmap 10000 2        # 1,897 ns/extent
taskset -c 6 /tmp/r1/vaarena frag 4000           # 最大可分配 128 MiB
taskset -c 6 /tmp/r1/thpdrop huge 256 2          # drop 0.70 ms / retouch 24.6 ms
taskset -c 6 /tmp/r1/slackgrow big               # 20.2 ms, 0 copies
taskset -c 6 /tmp/r1/slackgrow bighuge           # 7.1 ms, 0 copies
taskset -c 6 /tmp/r1/vectorbench pushback        # 48.3 ms, 23 reallocs
taskset -c 6 /tmp/r1/vectorbench reserve         # 21.9 ms, 0 reallocs
taskset -c 6 /tmp/r1/vectorbench grow11          # 73.1 ms, 14 reallocs
```

> 未做：①没有把 VA arena 做成 per-region 的生产代码（本轮只有探针）；②没有测多线程/每 worker 一个 region 的 VA 预算；③没有测「slack 用尽后整块搬」的摊还形态（只有固定 1 GiB 的 0 拷贝臂）；④Windows 侧未复核（无 `madvise`，走 4 KiB）；⑤没有量 `MADV_DONTNEED` 之后 `khugepaged` 的后台成本。
