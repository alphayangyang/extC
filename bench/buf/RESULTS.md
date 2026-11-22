# bench/buf —— 结果（R4a）

> 判据与形状定义在 [`SPEC.md`](SPEC.md)；本表由 `REPS=7 bash bench/buf/run.sh` 生成，
> 原始数据 [`RESULTS.tsv`](RESULTS.tsv)。机器：本机，`taskset -c 6`，`cc`/`g++ -O2`。

## 1 结论先行

| 形状 | buf | vector | vector+reserve | buf 对 vector | 峰值 RSS（buf / vec） |
|---|---|---|---|---|---|
| S1 增长到 8e6 i64 | **0.022 s** | 0.063 s | 0.034 s | 2.86× 快（对预留臂 1.55×） | 65,456 / 69,224 KB |
| S2 100 万短命 buf | **0.039 s** | 0.084 s | — | 2.15× 快 | **1,868 / 4,308 KB** |
| S3 装满→还页→长回 | **0.029 s** | 0.085 s | — | 2.93× 快 | 65,456 / 69,164 KB |
| S4 24 B 结构体 2e6 | **0.016 s** | 0.046 s | — | 2.88× 快 | 49,080 / 52,824 KB |
| S5 顺序扫 + 随机扫 | **0.083 s** | 0.117 s | — | （不用于排名，见 §5） | 128,940 / 128,936 KB |

五条形状 buf 全赢；**最该比较的是 S1 的「对预留臂」列**：`vector::reserve` 是 C++ 侧的最优解
（一次留够、之后零重分配），buf 仍然快 1.50×，且搬家次数 2 对 1 却总时间更短。

## 2 增长为什么快（S1 / S4）

`moves` 是 `buf::relocations()`，即真正搬过家的次数：

| 臂 | S1 moves | S4 moves | 原因 |
|---|---|---|---|
| buf | **2** | **2** | 预留 2 MiB 起，撞到预留上限才按 16 倍长搬一次；中间的增长只在预留里推 `cap` |
| vector | 24 | 22 | 几何翻倍，每一代都整段重分配 + 拷贝 |
| vector+reserve | 1 | — | 一次 `reserve` 到终值，之后零重分配（C++ 侧最优） |

S1 里 vector 的 24 次翻倍对应搬运总量 ≈ 2 × 64 MiB = 128 MiB，按本机顺序拷贝约 14 ms，
其余是 24 次 `malloc`（32 KB → 64 MiB）与首写缺页。buf 的两次搬家总量 ≈ 3 MiB。

RSS 差 3.7 MB 不是「buf 省内存」，而是 `std::vector` 的容量只增不减：最后一次 64 MiB 的
旧 buffer 在换代瞬间还活着（`shrink_to_fit` 没被调用），buf 在 `moveTo` 里是**拷一段丢一段**
（每 2 MiB 就把已拷过的源页 `madvise` 掉），所以瞬时峰值是「一份 + 一块」而不是「旧代 + 新代」。

## 3 S3：还了页再长回去，并不贵

这条形状把「容量能还」的账单独量出来：装满 8e6（64 MiB）→ `shrink(4096)` 把 64 MiB 的页
还给操作系统（地址不变）→ 再长回 8e6。程序内计时（`std::loop::now`，三轮一致）：

| 阶段 | 耗时 | 说明 |
|---|---|---|
| 第一次装满 | 12–24 ms | 含两次搬家与首写缺页 |
| `shrink(4096)` | < 1 ms | `madvise(MADV_DONTNEED)`，本机 ≈13 ns/KB ⇒ 64 MiB ≈ 0.9 ms |
| 再长回 8e6 | **4 ms** | 64 MiB 重写 = 16 GB/s，即 ≈0.5 ns/元素 |

**结论：在这条分配路径上「丢页→再写回」几乎不贵**（大页重新缺页），所以 S3 的 buf 总时间
28 ms ≈ 一次填充 + 一次重写。不要把 4 ms 外推到 4 KiB 页的路径：`../extc-docs/devlog/analysis/RESERVE-GROWTH.md`
里 130–300 ns/KB 的重触代价是另一条路径（`madvise` 后按 4 KiB 页重新缺页）的实测。

对照臂 `std::vector` 侧没有「还容量」这个动作，`shrink_to_fit()` 走的是重新分配（`moves=12`），
它把「shrink 之后再长」理解成继续翻倍，所以这 85 ms 里有一半是它自己不愿意省的：
S3 的意图是量「归还」的价，不是证明 vector 笨。

## 4 短命复用为什么快（S2）

100 万个 512 B 的 buf 依次 new/fill/drop。buf 侧 39 ms ≈ 39 ns/次：buddy 的 `carve`/`give` 是 O(1)
（每量子 1 bit 的「块起点」位 + 双向自由链，见 `src/back/bufrt.c` 首部注释），整个 arena 只做一次
64 GiB 的地址预留，热路径不产生任何 `mmap`/`munmap`（`strace -c` 全程 9 次 `mmap`）。
vector 侧 84 ms 是 100 万次 `malloc`/`free` 往返；RSS 4,308 KB 是 glibc 保留的 arena，buf 的 1,868 KB
是真正碰过的页（512 B × 100 万 = 512 MB 的吞吐只映射了几 MB）。

**这条形状是设计缺陷的回归门**：第一版 buddy 在 `carve` 的 split 里给整块涂 interior 标记、在 `give`
里线性扫自由链，同样这条形状要 **0.76 s**（每次分配约 65,536 次写）。改成现在这样之后 39 ms。
　同一段代码在这一轮又被砍掉一半（76 → 39 ms）：回收量子缓存（`EXTC_BUF_MAG_KEEP`）让 512 B 的
缓冲区归还时直接进免费链、不再逐层合并，`span` 快路与「锁只在有 `par` 时发射」各再省一点。三个
台阶在 20 万小容器那条形状上的毫秒数与机理记在
[`../extc-docs/design/BUF-INVARIANT.md`](../../../extc-docs/design/BUF-INVARIANT.md) §11。
改 `src/back/bufrt.c` 的分配器必须让 S2 留在 0.1 s 量级。

## 5 S5：只证明布局是连续的

两臂的 `seq` 与 `rnd` 逐位相同（`seq=383999976000000`、`rnd=383738819136000`），这证明 `buf` 的视图
就是 `base + i*sizeof(i64)`，没有间接层 —— 这是 R4b 之前必须钉住的性质。
时间列（86 vs 116 ms）**不用于给「连续布局更快」下结论**：两臂的扫描代码形状不同（extC 是
`for x in v` 对 slice，C++ 是 range-for + 下标），构建循环也不同（`push` vs `push_back`）。
要量扫描速度该用 `bench/stream` 那种同源生成、只差存储层的卡具。

## 6 一条必须记住的测量陷阱（也是实现要求）：`push` 必须 `@inline`

`buf` 的热成员（`push`/`pop`/`len`/`capacity`/`view`/`viewMut`/`clear`/`relocations`/`reservedBytes`）
在 `stdlib/std/data/buf.extc` 里都标了 `@inline`。这不是风格，是**数字的前提**：

同一份 8e6 次 `push` 的循环，不标 `@inline` 时实测 8e6 次要 86 ms，标了之后 4 ms（同一个 `main`）。
原因是发射出来的每个函数都带一个 `extc_arena __extc_a[3]` 的前置与两次 `extc_arena_destroy` 收尾
（本函数其实一次都不分配），这让 C 编译器不肯把 `push` 内联进调用者的循环；不内联时，
`b` 是否留在寄存器里取决于它在这段代码之后是否还被读 —— 只是多打印两个字段就能从 2.6 ns/push
变成 10 ns/push（本卡具开发时实际踩到：`println(..., b.len(), b.capacity(), b.relocations())`
让 S3 变成 154 ms，删掉两个字段又回到 40 ms）。内联之后这个依赖消失，两种写法都是 4–5 ms。

**因此**：(a) 这个卡具画不出「不标 `@inline` 的 buf」那种更慢的世界，因为库里的标注是契约的一部分；
(b) 后续如果有人给 `buf` 加成员，热路径上的新成员也要标 `@inline`，否则 S1/S3/S4 会静默回退；
(c) 真正的根因在发射器：对「一次都不分配」的函数不必发射 arena 前置，这条记在
`../extc-docs/design/BUF-INVARIANT.md` 的「未做」里。

## 7 复现与口径

```
make -j8
REPS=7 bash bench/buf/run.sh      # 写 bench/buf/RESULTS.tsv；校验和不一致即红
```

- 时间是交错多轮的**最小值**（`bash time %3R`，10 µs 分辨率）；RSS 是 `ru_maxrss` 单跑。
- 交错的含义：`run.sh` 按 `REPS` 外层循环逐轮跑完一个形状的所有臂，再取各自最小值，
  所以 A/B 在线性时间上互相交错，热漂移对两臂同向。
- 数字只在本机有意义；换机器请重跑并覆盖本表。

## 8 局限

1. 单核单线程；`buf` 的 arena 是进程级单锁（`extc_buf_arena_lock`），多线程形状未测。
2. 只对 C++；Go/Rust 的容器不在本卡具（见 `bench/realsvc`、`bench/gc`）。
3. 不测回收器：`buf` 是 T0 基质（程序不向回收器要内存），`EXTC_RC_*` 档对它无效。
4. S3 的相位拆分只有程序内计时（`loop::now`）三轮一致，没有逐页计数器；要精确到页需要
   `perf stat -e minor-faults` 这类工具，本机不可用（无 supported events）。
