# bench/containers —— 容器存储基质的 A/B（池板块 vs `buf<T>` 预留 vs C++ vs Rust）

> **自动生成**：`bash bench/containers/run.sh --reps 9` 写 `RESULTS.tsv`，`RESULTS.md` 由这张表
> 抄数；改判据先改 `run.sh` 顶部的注释与这里的方法节。

## 这条 bench 要回答什么

R4c 要把 `vector` / `string` / `hashMap` 从**池板块**搬到 `buf<T>`（一片自己预留的地址空间，
预留内增长零拷贝），然后删掉池与运行期池。删之前必须先量：**换基质换掉了什么、加回了什么**。
所以四臂编译**同一份 `.extc` 源**：

| 臂 | 编译器 | 标准库 | 存储 |
|---|---|---|---|
| `old` | `git worktree` 检出 `f910d14d` 自建 | 该树的 `stdlib`（`EXTC_STD`） | 池板块（`poolSliceRaw` / `poolResizeRaw`） |
| `new` | 当前树的 `./build/extc` | 当前树的 `stdlib` | `buf<T>` 的 2 MiB 量子预留 |
| `cpp` | `g++ -O2 -std=c++17` | libstdc++ | `std::vector` |
| `rs` | `rustc -O` | Rust std | `Vec` / `String` / `HashMap` / `HashSet` / `BTreeMap` / `BTreeSet` |

## 形状

| 形状 | 做什么 | 考的是什么 |
|---|---|---|
| V1 | 从容量 4 push 到 8e6 个 `i64`，再求和 | 增长：池版本每次换板块、旧代变垃圾；buf 版本在预留内推长度 |
| V2 | 装满 2e6 后连做 5 次克隆 | 拷贝：`clone` 的开销（池版本从池拿新板块逐元素拷，buf 版本一次预留到位） |
| V3 | 装满 8e6 → `clear()` → 再装满一次 | 容量复用：`clear` 之后该不该重新长 |
| V4 | 20 万个小容器（容量 8、装 8 个 `i64`）生死循环 | 小对象周转：池版本的主场（板块在池里复用、只还世代号） |
| S1 | 1000 万次「零值构造 + 写 15 字节 + 读长度」 | 短串 SSO 路径：两代存储都不该为它分配 |
| S2 | 16 条 4 MiB 长串，每条 64 字节块 append 65536 次 | 长串增长（池按 1.5 倍原地长）与**峰值 RSS** |
| S3 | 1 条 8 MiB 串 clone 20 次 | 深拷贝：clone 是 O(n)（新缓冲区 + 搬 160 MiB） |
| H1 | 4e6 条 i64 键，`withCap(N)` 之后插入并全量命中查询 | hashMap 稳态：探测、命中、稠密值列 |
| H2 | 同样的 4e6 条键，从 `withCap(8)` 长起来 | 增长：重建时旧列与新列同存（峰值 RSS） |
| H3 | 8e6 次插入/查询/删除混在 1e6 个键上 | 墓碑与 swap-remove：删一次要搬值、修反查列 |
| H4 | 2e6 条之后两遍全表遍历 | 遍历：稠密值列连续、桶列稀疏 |
| H5 | 4e6 条之后 8e6 次不命中查询 | 未命中路径：探测到 EMPTY |
| H6 | 只做 4e6 次插入（C++ 侧是 `h_flat p1`） | 把 H1 拆成插入侧与查询侧 |
| HS1 | 4e6 条 i64 键进 `hashSetI64`，`withCap(N)` 之后插入并全量查询 | 集合稳态：探测、命中 |
| HS2 | 1e6 个键上 8e6 次插入/删除/查询 | 集合的墓碑与 swap-remove |
| LM1 | 1024 条 i64 键的 `linMap`，100 万次线性查找 | 线性查找：`find` 逐项 `==` 扫描，规模就是定义 |
| LM2 | 1024 键上 100 万次写入/删除/读取 | `linMap` 的覆盖写与 swap-remove |
| LS1 | 1024 个成员的 `linSet`，100 万次成员查询 | 线性集合的成员检查 |
| M1 | 有序 `map` 插入 1e6 条 i64 键，再全量查询 | B+ 树稳态：下降、节点内二分、叶内插入 |
| M2 | 有序 `map` 在 30 万键上 2e6 次写入/删除/读取 | 分裂、借用、合并路径 |
| ST1 | 有序 `set` 插入 1e6 条、全量查询、再 `at(i)` 走一遍有序序 | 有序遍历：`at(i)` 是 B+ 树的 rank 下降 |

V4 是刻意选的**反向形状**：它是旧基质最便宜、新基质最贵的一格，用来量出「预留 + 归还」的
固定成本。

## 方法

- **等价工作**：四臂打印同一行 `n=<n> sum=<sum>`，逐字相同才算数（不同直接报失败）。
- **计时**：`taskset -c 6` 固定核；每臂每轮跑一次，**交错**（同一轮里 old → new → cpp → rs），取 `REPS`
  轮的最小值；单位毫秒（bash `time` 的 `%3R`，10 µs 分辨率）。前端只吐 C，编译时间不进窗口。
- **RSS**：`/usr/bin/time -f %M` 的 `ru_maxrss`，每臂单独一次运行（输出确定，一次即可）。
- **源码同一份**：`.extc` 文件对各臂完全相同，只有编译器与 `EXTC_STD` 不同；`cpp` 臂是逐行对应的
  C++ 写法（同样的循环、同样的校验和公式）。

## 复现

```sh
# 旧臂的编译器：池版本最后一个提交（vector 迁移之前）
git worktree add -f /tmp/ct/wt-old f910d14d
make -C /tmp/ct/wt-old build/extc -j8
bash bench/containers/run.sh --reps 9 --old /tmp/ct/wt-old
```

缺旧编译器时脚本直接报错退出，不会静默跳过一臂。
## 判据与当前账

三条判据，任一不满足就不许删池（池的廉价复用必须由机制补回来，不能靠「新基质通常更快」硬切换）：

1. **等价工作**：四臂 `n`/`sum` 逐字相同。
2. **墙钟**：`new ≤ old × 1.10`（每个形状）。超了说明这一格由池提供的机制缺失，先补机制再迁移。
3. **RSS**：`new ≤ old + ε`，ε 按形状取 8 MiB；并记录偏差百分比，写进下面的表。

当前账（`RESULTS.tsv`，`--reps 11`，min-of-11，`taskset -c 6`）：

| 形状 | old | new | cpp | new/old | 峰值 RSS old → new |
|---|---|---|---|---|---|
| V1 增长 | 28 ms | 19 ms | 54 ms | 0.68× | 64,252 → 65,552 kB（+2.0%） |
| V2 克隆 | 24 ms | 15 ms | 32 ms | 0.63× | 32,852 → 34,840 kB（+6.1%） |
| V3 复用 | 51 ms | 30 ms | 67 ms | 0.59× | 64,256 → 65,552 kB（+2.0%） |
| V4 小容器 churn | 4 ms | 4 ms | 7 ms | 1.00× | 1,868 → 1,868 kB（0%） |
| S1 短串 SSO | 34 ms | 33 ms | 32 ms | 0.97× | 1,868 → 1,868 kB（0%） |
| S2 长串增长 | 24 ms | 9 ms | 37 ms | 0.38× | 67,328 → 12,016 kB（−82%） |
| S3 深拷贝 | 18 ms | 12 ms | 23 ms | 0.67× | 18,136 → 24,308 kB（+6.0 MiB） |

七格全过。`vector` 与 `string` 都已在缓冲区基质上，`hashMap` 的元素所有权已接、存储还在池上。

V2 的 +6.1% 与 S3 的 +6.0 MiB 是同一笔语义差：池版本的克隆与原件共享一块板块，新版本的每个副本
各自拥有预留 —— 这份 RSS 换的是「副本可以独立增长、独立释放」。S2 是反方向：16 条顺序构造、活集
恒为一条 4 MiB，old 臂的峰值随构造过的条数线性涨（k=1/2/4/8/16 ⇒ 5.8/9.9/18.2/34.5/67.3 MB，斜率
≈ 一整个串），new 臂与 k 无关（12.0 MB）；C++ 臂同一形状 11.7 MB。

V4 是小对象周转最贵的一格：预留 + 归还的固定成本全落在这里。要靠三个机制才与池版本打平 ——
2 MiB 量子的回收缓存、`span` 的单线程快路、`EXTC_BUF_MT` 按需发射，再加归还路径的「一把锁、
一次查类」。机理与台阶记在 `../extc-docs/design/BUF-INVARIANT.md` §11 与 §15.2，结构由 `gate-buf-mt` 守着。
规则与迁移顺序见 `../extc-docs/design/CONTAINER-OWNERSHIP.md`。

## hashMap（H1–H6）：为什么要第四个臂

`unordered_map` 是链式节点表（每元素一次分配、查找沿指针追），拿它当「C++ 的上界」会把 extC 的
优势说大——而 extC 的 `hashMap` 本身就是开放寻址。所以 H 形状有两个 C++ 臂：`cpp` 用
`std::unordered_map`，`flat` 用 `h_flat.cpp`（同一类算法：2 的幂容量、线性探测、墓碑、0.5 装载率，
键与值内联在桶里，没有 extC 为「跨扩容稳定句柄」保留的稠密值列与反查列）。

`run.sh --shapes h1,...,h6` 出 old/new/cpp/rs 四臂（H6 的 C++ 对照是 `h_flat` 的插入侧）；
`run-flat.sh` 出 extC/cpp/flat 三方（H6 对应 `h_flat p1`）。两张表与归因写在 `RESULTS-hash.md`。

结论（`RESULTS-hash.tsv` 与 `RESULTS-hash-flat.tsv`，`--reps 7`，min-of-7）：

- 对 `unordered_map`：H1 0.41×、H2 0.29×、H4 0.14×、H5 0.41×（即快 2.5–7.0×），H3 反向 1.21×；
- 对同算法手写表：H6（插入侧）1.10×，H1 1.68×、H5 1.71×、H3 2.56×，H4 1.02×（遍历持平）；
- 差距归因**指令数**（callgrind：H1 190.3 vs 49.1 指令/操作 = 3.9×；H5 174.5 vs 49.4 = 3.5×），
  不是内存流量（末级缺失 1.00× / 1.02×）；`run-cg.sh` 可复现。
- 换基质（池 → `block`+`slots`）在这一组形状上速度中性（0.70×…1.06×），RSS 结构性变高
  （+19%…+81%；H2 重建瞬间两代同存 +44%）；它买到的是所有权与可删池，不是性能。


## 四方对照（收口重跑，2026-12）

池退场之后（v38–v40）重跑一次：每个形状四臂编译**同一份** `.extc` 源，
old = 池底最后一个提交 `f910d14d`（vector 迁移前），new = 当前树。

| 臂 | 编译器 | 标准库 | 存储 |
|---|---|---|---|
| `old` | 旧树自建（`git worktree` `f910d14d`） | 该树的 `stdlib` | 池板块 |
| `new` | 当前树 `./build/extc` | 当前树 `stdlib` | `buf<T>` 地址预留 / `block<T>` 平坦块 |
| `cpp` | `g++ -O2 -std=c++17` | libstdc++ | 对应标准容器 |
| `rs` | `rustc -O` | Rust std | 对应标准容器 |

判据不变：各臂输出逐字相同、min-of-N、峰值 RSS 一并记录；`new` 比 `old` 慢超过 10% 的形状要写清是哪一条机制缺了。
表在 `RESULTS-4WAY.tsv`（`--reps 5 --tsv RESULTS-4WAY.tsv`），解读在 `RESULTS-4WAY.md`。
varArray 的五臂对照（old / varArray / `stl::vector` / C++ / Rust）在 `bench/vararray`。