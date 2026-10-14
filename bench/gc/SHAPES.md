# bench/gc —— 7 个形状的规格（写实现前先定死，避免"各自写各的"）

每个形状的判据：**参与该形状的语言的输出必须逐字节一致**；不一样就说明形状没对齐，数字不算数 ✗
（形状 1–6 = 5 家：C/Rust/Go/Java/extC；形状 7 = 4 家：extC / 现代 C++ / Rust / Go）

| # | 名字 | 形状 | 量什么 | GC 敏感点 |
|---|---|---|---|---|
| 1 | `churn` | R 次分配；每 8 次 free 一个旧节点；尾端读回求和 | **纯分配吞吐** | bump 指针 vs 每对象元数据 |
| 2 | `rebuild` | 20000 轮 × 每轮 1000 节点链，建→遍历→**全丢** | **回收压力** | arena 出块即还 vs 追踪回收 |
| 3 | `list` | 200 万节点链 + 遍历求和 | **指针追逐** | 缓存不友好（GC 语言的经典痛点） |
| 4 | `tree` | 不规则大小节点树（带 i32 载荷），建 + 前序求和 | **不规则尺寸分配** | 大小类/分代会露出来 |
| 5 | `queries` | stdin 读 n 个数 + q 个查询，输出 q 行 | **IO + 结构混合** | 分配 + 解析 + 输出 |
| 6 | `hash` | 开放寻址哈希：30 万次插入 + 30 万次查询 | **随机访问 + 分配** | 写屏障/分配混合 |
| 7 | `digraph` | **错综复杂的有向图**：每轮建若干"环组"（k=3..6 结点首尾相接成**真环**）+ 弦边 + 命名回指；再改边、**丢整分量**（制造不可达的环状垃圾）、遍历存活部分 | **环状垃圾的回收** | 引用计数（`shared_ptr`/`Rc`）**收不掉环** vs 追踪 GC / 区域清扫 |

### 形状 7（`digraph`）的规格（写死，四家同）

- 入参：`argv[1]=N`（默认 200000）、`argv[2]=seed`（默认 20261013）；**不看 stdin**；无浮点。
- 结构：结点 = `id/gid/payload` + 变长出边表（头插链）+ 祖先链 `ord` + 命名回指 `back`；
  根集 = 双向链 handle 表（每分量一个）；全局枢纽集 = 一个 8 结点永久存活的组。
- 每轮四步：(a) 建 G 个环组（闭合边 = 回溯边，计入 `backedges`），弦边目标 60% 组内更老 /
  20% 组内更新 / 10% 全局枢纽 / 10% 长程（**可跨组**）；(b) 改 M 条边（删头边 + 同分布加边）；
  (c) 丢：随机取 handle，**优先摘无跨组入边（`gpin==false`）的组**，并现场验证该组含环；
  (d) 从 handle 表遍历存活部分，累加校验和（反 DCE）。
- 输出（**两行整数**）：`nodes=<存活> edges=<存活边> sum=<校验和>` +
  `backedges=<n> groups_made=<n> nodes_made=<n> dropped_groups=<n> dropped_nodes=<n> rejected_external=<n> cycles_dropped=<n>`。
- 详细规格、设计决定与实测在 [`DIGRAPH.md`](DIGRAPH.md)；卡具是**新写**的
  [`run-digraph.sh`](run-digraph.sh)（不动 `run.sh` / `measure.py`）。

## 必须避开的坑（已实测踩到）

**坑 1：`-O2` 会把 `malloc`+`free` 对整对消掉。**
实测：`objdump` 里 `with_free` 与 `no_free` 都是 **0 个 malloc 调用** ⇒ 测出 0.4ms，是假的 ✗
⇒ **形状必须让分配"可观察"**：要么留着活对象、要么在尾端读回。
⇒ 报告里凡是提到 malloc 的数字，都要能回答"`objdump` 里还剩几个调用" ✓

**坑 2：`Scanner`/`cin` 这类慢读法会把 IO 形状变成"测库"而不是"测分配"。**
⇒ IO 形状每语言给**两档**：标准库最快读法（Java `BufferedReader` / Go `bufio` / C++ `sync_with_stdio(false)`）
   与"手写快读" ⇒ 差值本身是信息 ✓

## 测量口径（照抄 `bench/oi/run.sh`）

- 时间 = **best of N**（默认 5）· RSS = `/usr/bin/time -f %M` 的峰值
- 构建：C/C++ `-O2` · Rust `-C opt-level=2` · Go `go build` · Java `javac`（运行时默认 JVM）
- **GC 语言跑两档**：开 GC / 关 GC（Go `GOGC=off`、Java `-XX:+UseSerialGC` 与 `-Xmx` 对照）
  ⇒ **差值 = 纯 GC 代价**，这是 GC 语言唯一的独家信息 ✓
