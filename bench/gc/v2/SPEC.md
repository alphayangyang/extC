# v2 形状规格（GC 大横评：工程友好写法）

给「GC 大横评」用的四个**新形状**。参与语言：extC / Go / Java / Rust / Node / C++。
每个形状 × 每种语言都必须产出**逐字节相同**的 stdout（这是对齐判据，不一样就是移植错了）。

## 0. 写法要求（用户口径）

- **平时自然怎么写就怎么写**：Go 用指针、Java 用对象、Rust 用 `Box`/`Rc<RefCell<...>>`、Node 用普通对象、C++ 用 `shared_ptr`/`unique_ptr`。
  **不要**手工模拟 arena、不要手写 freelist、不要为了迁就 extC 改变数据结构。
- **所有语言都不用库容器**（map / dict / HashMap / HashSet / BTreeMap / VecDeque / ArrayList / std::map…）。
  定长数组、链表、对象引用是允许的；Rust 的 `Vec`、Go 的 slice 当**定长数组**用（只按下标读写、不 push）可以。
- extC 侧硬约束：不 `use std::stl`。现有六档形状本来就只 `use std::io`。
- 不许把分配优化掉（`bench/gc/SHAPES.md` 的坑 1：`-O2` 能把 malloc+free 整对对消）：
  所以每个形状都保留了「尾端读回」量（G1 的 `tailv`、G3 的 `tailv`）并把值打进 stdout。
  若某语言编译器仍然把分配消掉（`objdump -d | grep -c 'call.*malloc'` 为 0，或峰值 RSS 不随规模增长），
  就用该语言最自然的手段（`black_box`、把最近一次的头存进全局/静态槽）保住它，并在报告里说明。

## 1. 公共约定

- 随机源 = 一个全局 `state`：`nxt()` 即 `state = (state * 1103515245 + 12345) & 2147483647`，返回新 `state`。
  上电时 `state = seed & 2147483647`。**这个乘法是 62 位中间量**。
- `pick(k) = (nxt() / 1024) % k`（整数除法向下取整）。小范围抽取用 `pick`，避免 LCG 低位退化。
- 累加器掩码 `& 4611686018427387903`（2^62−1）。本规格里除 LCG 中间量外所有数值都 < 2^53（已逐个核过），
  所以 **Node 快速臂**（`JS_MODE=split`）用 Number 即可：把 LCG 的乘拆成高低 16 位两次乘，
  照抄 `bench/gc/src/digraph.js` 里已有的 split 写法；另出一个 `JS_MODE=bigint` 臂（BigInt 全程）作对照。
- argv：`argv[1]=N`、`argv[2]=seed`（默认 20261013）；G3 另有 `argv[3]=W`、G4 另有 `argv[3]=f`。
  缺参数用默认值。参数解析只认十进制数字（照抄现有形状的 `parseI64`：逐字符、遇非数字停）。
- stdout **恰好两行**，字段用单空格分隔、`key=value`，每行（含第二行）末尾有一个换行符。stderr 不许有输出。
- 整数按 64 位有符号运算（extC `i64` / Go `int64` / Java `long` / Rust `i64` / C++ `int64_t` / Node Number∪BigInt）。
- LCG 调用顺序就是语义的一部分：**按下面伪代码里出现的顺序**逐个调用，多一次少一次输出就不同。

## 2. 四个形状

### G1 promote —— 分代假设：9 个短命 + 每 10 轮 1 个提升到长命表

常量：`TABLE=65536`、`SHORT=9`、`EVERY=10`、`WALKCAP=8`。默认 `N=500000`。
数据结构：节点 `{ k: i64, v: i64, next: 可空节点引用 }`；长命表 `{ b: [65536] 可空节点引用 }`（全空）。

```text
state = seed & 2147483647
sum = walksum = live = tailv = 0
for i in 0 .. N-1:
    head = null
    for j in 0 .. SHORT-1:
        s = 新 node; s.k = i*SHORT + j; s.v = nxt() % 1000; s.next = head; head = s
        sum = (sum + s.v) & MASK
    tailv = head.v                      # 短命链确实存在（防消解、也让结果可观测）
    if i % EVERY == 0:
        p = 新 node; p.k = i; p.v = nxt() % 1000
        b = i % TABLE; p.next = table.b[b]; table.b[b] = p; live += 1
    b2 = (i * 40503) % TABLE
    c = table.b[b2]; cnt = 0
    while c != null and cnt < WALKCAP:
        walksum = (walksum + c.v) & MASK; c = c.next; cnt += 1
输出：
  rounds=<N> promoted=<live> walksum=<walksum> sum=<sum> tailv=<tailv>
  table=65536 short=9 every=10 walkcap=8
```

### G2 sizeratio —— 4 个尺寸类（32/64/96/128 字节）的对象池

常量：4 个类 `c0..c3`，大小 32/64/96/128 字节（`k: i64` 加 `p: [3|7|11|15]i64`）；
每类一张 1024 槽的长命表；每 8 轮里有 1 轮把对象存进**该类**的表（顶掉同槽旧对象）。默认 `N=2000000`。

```text
state = seed & 2147483647
sum = kept = livebytes = 0
for i in 0 .. N-1:
    cls = pick(4)
    创建该类的对象 o; o.k = i
    sum = (sum + o.k) & MASK
    if i % 8 == 0:
        b = (i / 8) % 1024
        old = 该类表[b]
        if old != null: livebytes -= 大小(cls)      else: kept += 1
        该类表[b] = o; livebytes += 大小(cls)
输出：
  ops=<N> kept=<kept> livebytes=<livebytes> sum=<sum>
  classes=4 sizes=32..128 slots=1024
```

### G3 chase —— 大活集指针追逐 + churn

常量：`CAP=1048576`、出度 4、`CHURN_EVERY=256`、`CHURN=4`。
默认 `M=1048576`（必须 ≤ CAP）、`W=4000000`。
数据结构：节点 `{ k: i64, v: i64, o: [4] 可空节点引用 }`；图 `{ n: [CAP] 可空节点引用 }`（全空）。
**两趟**：先建 M 个节点，再连边（建边时从图里取目标，天然给 GC 一个 4M 条边的活图）。

```text
state = seed & 2147483647
for i in 0 .. M-1: x = 新 node; x.k = i; x.v = nxt() % 1000; graph.n[i] = x
for i in 0 .. M-1: src = graph.n[i]
    for j in 0 .. 3: t = nxt() % M; src.o[j] = graph.n[t]
cur = graph.n[0]; sum = churn = tailv = 0; step = 0
while step < W:
    sum = (sum + cur.v) & MASK
    r = pick(4); nx = cur.o[r]; if nx != null: cur = nx
    if step % CHURN_EVERY == 0:
        h = null
        for j in 0 .. CHURN-1: s = 新 node; s.k = step; s.v = nxt() % 1000; s.o[0] = h; h = s
        tailv = h.v; churn += CHURN
    step += 1
输出：
  nodes=<M> edges=<M*4> walk=<W> churn=<churn> sum=<sum> tailv=<tailv>
  outdeg=4 cap=1048576 churn_every=256 churn=4
```

### G4 cycleratio —— 环垃圾占比 f

常量：窗口 256 槽；每组 `k = 3 + pick(4)` 个节点，以 `pick(100) < f` 的概率闭成环。
默认 `G=400000`、`f=100`（全成环；`f=0` 全成链）。窗口满后每组顶掉一组旧组：
**成环的那组被顶掉后是不可达环**（引用计数收不掉，追踪式 GC 照样能收）——这就是形状要暴露的差异。

```text
state = seed & 2147483647
sum = made = evicted = dropnodes = rings = 0
win = [256] 槽，每槽存 (头节点, 该组节点数)
for i in 0 .. G-1:
    k = 3 + pick(4); isring = pick(100) < f
    first = prev = null
    for j in 0 .. k-1:
        s = 新 node; s.k = i; s.v = nxt() % 1000; s.next = null
        if first == null: first = s
        if prev != null:  prev.next = s
        prev = s
        sum = (sum + s.v) & MASK
    if isring: rings += 1; prev.next = first
    made += k
    b = i % 256
    if win[b] 非空: evicted += 1; dropnodes += win_k[b]
    win[b] = first; win_k[b] = k
输出：
  groups=<G> made=<made> evicted=<evicted> dropnodes=<dropnodes> sum=<sum>
  f=<f> window=256 kmin=3 kmax=6 rings=<rings>
```

## 3. 对齐参考输出（extC 参考实现产出，必须逐字节相同）

```text
promote 100000 20261013
rounds=100000 promoted=10000 walksum=3801746 sum=449571904 tailv=333
table=65536 short=9 every=10 walkcap=8

sizeratio 200000 20261013
ops=200000 kept=1024 livebytes=81920 sum=19999900000
classes=4 sizes=32..128 slots=1024

chase 65536 20261013 200000
nodes=65536 edges=262144 walk=200000 churn=3128 sum=99695411 tailv=634
outdeg=4 cap=1048576 churn_every=256 churn=4

cycleratio 200000 20261013 100
groups=200000 made=910721 evicted=199744 dropnodes=909559 sum=454952238
f=100 window=256 kmin=3 kmax=6 rings=200000

cycleratio 200000 20261013 50
groups=200000 made=910721 evicted=199744 dropnodes=909559 sum=454952238
f=50 window=256 kmin=3 kmax=6 rings=100382

cycleratio 200000 20261013 0
groups=200000 made=910721 evicted=199744 dropnodes=909559 sum=454952238
f=0 window=256 kmin=3 kmax=6 rings=0
```

bench 规模参考（`run.sh bench` 用；对齐也可用）：
`promote 500000` 出 `rounds=500000 promoted=50000 walksum=95299501 sum=2247633312 tailv=661` ·
`sizeratio 2000000` 出 `ops=2000000 kept=1024 livebytes=81920 sum=1999999000000` ·
`chase 1048576 20261013 4000000` 出 `nodes=1048576 edges=4194304 walk=4000000 churn=62500 sum=1987545638 tailv=990` ·
`cycleratio 400000 20261013 100` 出 `groups=400000 made=1821441 evicted=399744 dropnodes=1820279 sum=909881214`。

> `kept=1024`（而不是 4096）是 LCG 位结构的产物：每个槽在整轮里只见过同一个尺寸类。所有语言照出即可，不用「修」。

## 4. 各语言的文件名 / 构建 / 运行

源码一律放 `bench/gc/v2/src/<形状>.<后缀>`，形状名 `promote` / `sizeratio` / `chase` / `cycleratio`：
`.extc`（已有，参考实现）、`.go`、`.java`、`.rs`、`.js`、`.cpp`。

| 语言 | 构建 | 运行 |
|---|---|---|
| extC | `build/extc -w --no-line-map -I stdlib src/x.extc -o b/x.c` 加 `cc -O2 -D_POSIX_C_SOURCE=200809L` | `b/x.extc <args>` |
| Go | `go build -o b/x.go src/x.go` | `GOMAXPROCS=1 b/x.go <args>`；另跑 `GOGC=off` 臂 |
| Java | `javac -d b/classes src/x.java`（`class x`，**类名=文件主干**，与 `src/digraph.java` 同规矩） | `java -cp b/classes x <args>` |
| Rust | `rustc -O --edition 2021 src/x.rs -o b/x.rs`；另 `--cfg careful` 出 `b/x.rs_careful` | `b/x.rs <args>` |
| Node | 无（`node --check src/x.js`）；`JS_MODE=split` 与 `bigint` 切换 | `node src/x.js <args>` |
| C++ | `g++ -O2 -std=c++17 -D<大写形状>_NAIVE src/x.cpp -o b/x.cpp`；另 `-D<大写形状>_CAREFUL` 出 `b/x.cpp_careful` | `b/x.cpp <args>` |

- Rust `--cfg careful` / C++ `_CAREFUL`：**只有 G4 语义不同**（naive 用会成环的强引用，careful 在顶掉前断环或用 `Weak`）；
  别的形状两臂源码相同（但也要能编出来并给出同样的 stdout）。
- Go 的 `GOGC=off` 臂与 Node 的 `bigint` 臂都是**对照臂**，必须同样逐字节正确。

## 5. 验收

```bash
bash bench/gc/v2/run.sh align                     # 所有语言 × 四形状：stdout 与 extC 逐字节相同
bash bench/gc/v2/run.sh align --lang go           # 只查自己那一种
bash bench/gc/v2/run.sh build                     # 只构建、不跑
```

## 6. 出成绩单（跑表 → 表/图/停顿表）

`run.sh bench` 的一切都在 `bench/gc/v2/shim.c` 垫片里跑（`wait4` 内核值：`ms/rss_kb/utime_ms/stime_ms/minflt/majflt/nvcsw/nivcsw`），
stdout 仍与 extC 参考逐字节比对，任何一臂不同就 `BAD` 且退出码非 0。给 `TSV=<路径>` 就落原始表。

```bash
B=/tmp/v2matrix/build TSV=/tmp/v2matrix/matrix.tsv REPS=5 bash bench/gc/v2/run.sh bench
B=/tmp/v2matrix/build bash bench/gc/v2/pause.sh bench/gc/v2/pause.tsv   # extC 侧 [step]/[pause]
python3 bench/gc/v2/report.py bench/gc/v2/matrix.tsv bench/gc/v2/REPORT.md bench/gc/v2/matrix.png bench/gc/v2/pause.tsv
```

臂清单（11 个）：`extc:default` / `extc:off`（编译期 `EXTC_RECLAIM=0`，**产物不同**，不是「纯 GC 税」对照，见 `docs/TODO.md`）·
`go:default` / `go:off` · `java:default` · `rust:default` / `rust:careful` · `node:split` / `node:bigint` · `cpp:default` / `cpp:careful`。
提交在库里的成绩单是 `REPORT.md` + `matrix.tsv` + `pause.tsv` + `matrix.png`。
