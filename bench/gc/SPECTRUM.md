# 平凡 → 极端：形状谱（六档）· extC vs Go 三把尺对照（`task-53`）

> **进度**：**第 1 段（两语言实现 + 同 seed 同输出校验）已完成 ✓**；**第 2 段（完整三把尺矩阵）未跑 ✗**（见 §4）。
> 写作用域 `bench/gc/**` ✓；`src/`、`tests/`、`tools/` **未动** ✓；插桩只在生成物副本（第 2 段沿用现成探针）✓。

## 1. 形状属性表（六档）

| 档 | 形状 | 特征（属性） | extC | Go | 逻辑操作数（argv[1]） | 输出（两行整数） |
|---|---|---|---|---|---|---|
| **P1** | 纯短命分配、**无引用**（`new` 一个只有 `i64[64]` 的 buf，用完丢） | arena 型、**无图**、无跨对象边 | `src/spectrum_p1.extc` | `src/spectrum_p1.go` | 分配次数 N（默认 200000） | `n=… sum=…` / `allocs=… per=64` |
| **P2** | 分块追加的长命容器（256/块、块间单链，**无环**） | 分配密集、活集**单调长**、写集中在尾块 | `src/spectrum_p2.extc` | `src/spectrum_p2.go` | 追加次数 N（默认 200000） | `n=… sum=…` / `blocks=… per=256` |
| **P3** | 每轮建一条链表、轮末整片丢（无跨轮引用） | 中等分配、**换血高**、无跨轮边 | `src/spectrum_p3.extc` | `src/spectrum_p3.go` | 轮数 R（默认 5000） | `rounds=… nodes=… sum=…` / `drops=… per_round_min=40` |
| **P4** | 长命**只读符号表** + 每单元短命 AST（= `human_a`） | 活集大而稳、**写足迹小** | `src/spectrum_p4.extc`（= `human_a.extc` 的副本） | `src/spectrum_p4.go`（本轮新增） | 单元数 units（默认 2000） | `units=… symbols=… asts=… sum=…` / `inserts=… hits=… misses=… probes=…` |
| **P5** | 滑动窗口环形缓冲（引用）+ 长命链式索引（偶发插入） | 写密集、活集稳定、偶发插入 | `src/spectrum_p5.extc` | `src/spectrum_p5.go` | 操作数 ops（默认 300000） | `ops=… idx=… sum=…` / `ins=… hit=… win=256` |
| **P6** | `digraph`（**已有两语言**，不改） | 单巨分量、换血 96.7%、引用存储极多 | `src/digraph.extc` | `src/digraph.go` | N（§6.2 用 250k/1M） | 见 `DIGRAPH.md` §4 |

**语义等价的做法**（逐条对应 ✓）：同一 LCG（`s=(s*1103515245+12345)&2147483647`）· **同一调用顺序** · 整数输出（无浮点）· i64 算术（Go 用 `int64`，溢出回绕与 extC 一致 ✓）· ASCII 输出格式同步手写 ✓。

## 2. 口径（**先写死** ✓）

1. **同 seed 同输出**：`argv[2]=seed`（默认 20261013）；**两语言 stdout 必须逐字节一致** ✓（`cmp`）；不一致 ⇒ **该档作废** ✗，**不许调形状凑** ✓。
2. **三把尺**：① 墙钟 ms（**5 次取中位**，跑前 load 照记）；② **峰值 RSS** —— 两臂**同一方法**：`bench/gc/digraph_run` 垫片（fork+`wait4.ru_maxrss` 内核值）✓；③ GC 停顿分布 —— extC 用 `tools/rc-pause-probe.py` 的 `PAUSE_DIST`（`n/Σ/p50/p90/p99/max`），Go 用 `GODEBUG=gctrace=1` 解析（**沿用 `GO-PAUSE.md` 的做法**：每周期 STW = 第一段 + 第三段）✓。
3. **公平**：两臂**同一件事**（同 seed、同逻辑操作数、输出一致）；`taskset -c 6`；**Go 主对照 `GOMAXPROCS=1`**（单线程同条件 ✓）＋**另附 4 P 参考列**（**标注"不同条件"** ✗）；Go 默认 `GOGC=100` ＋**另附 `GOMEMLIMIT` 压到与 extC 峰值同量级**的列（标注"同内存预算对照"）✓。
4. **不带额外插桩**跑计时 ✓；诊断 stderr 关门时 0 字节 ✓；**不外推**（结论只覆盖测过的档与规模）✓。
5. 附加列：每档给**分配次数**、**extC 是否真进回收**（生成物 `extc_rc_` 记号数）、**Go 周期数** ✓。

## 3. 第 1 段：已完成的校验（**实测 ✓**）

| 档 | extC 编译 | Go 编译 | 同 seed 同输出（小规模冒烟） | 备注 |
|---|---|---|---|---|
| P1 | ✓ | ✓ | **✓** `n=2000 sum=2085510192` | 两语言逐字节相同 ✓ |
| P2 | ✓ | ✓ | **✓** `n=2000 sum=3906003` | 同上 ✓ |
| P3 | ✓ | ✓ | **✓** `rounds=200 nodes=12597 sum=6250611` | 同上 ✓ |
| P4 | ✓（= `human_a`） | ✓（**本轮新增**） | **✓** `units=2000 symbols=11081 asts=1536744 sum=3548831066854` | **P4-Go 与既有 `human_a` 二进制输出 `cmp` 完全相同** ✓ |
| P5 | ✓ | ✓ | **✓** `ops=2000 idx=250 sum=14939596` | 同上 ✓ |
| P6 | ✓（既有） | ✓（既有） | 历史已对齐（`DIGRAPH.md` §4/§6.10：13 臂逐字节一致 ✓） | 不变 ✓ |

生成物检查（第 2 段前先记一笔）：P1–P5 的 extC 产物**都发 rc 运行期**（`extc_rc_` 记号数在 §4 一并报）✓。

## 4. 未做（**明确缺口** ✗）

- **第 2 段整张矩阵未跑** ✗：P1–P5 各 2–3 个规模 × 两臂 × {墙钟中位 / 峰值 RSS / 停顿分布 / 分配次数 / rc 记号数 / Go 周期数}，以及 Go 的 **4 P 参考列**与 **`GOMEMLIMIT` 同预算列** —— **一个数都还没有**。
- **每档一句话判词**（谁赢哪把尺、差多少倍、形状特征解释哪部分差异）**未写** ✗。
- P1–P5 的**规模上限**尚未定（需先试跑确认"跑得完"）✗；`P6` 沿用 `DIGRAPH.md` §6.2 的 250k/1M ✓（**跨会话**数字，若引用会标注 ✗）。
- **不承诺"没有任何新风险"** ✗。

## 5. 复现（第 1 段）

```bash
cd bench/gc
for p in 1 2 3 4 5; do
  ../../build/extc -w --no-line-map -I ../../stdlib src/spectrum_p$p.extc -o /tmp/spectrum/p$p.c
  cc -O2 -o /tmp/spectrum/p$p /tmp/spectrum/p$p.c
  go build -o /tmp/spectrum/p${p}go src/spectrum_p$p.go
done
# 同 seed 同输出校验（逐字节）
for p in 1 2 3 4 5; do /tmp/spectrum/p$p 2000 20261013 > /tmp/spectrum/a.txt; /tmp/spectrum/p${p}go 2000 20261013 > /tmp/spectrum/b.txt; cmp /tmp/spectrum/a.txt /tmp/spectrum/b.txt && echo "P$p ✓"; done
```
