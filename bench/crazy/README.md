# bench/crazy：同一份输入、四种写法

**问题**：「现在的 extC 能写多疯狂」——既好写（少代码、少仪式）又能跑（接近系统语言）。

**做法**：同一份 150 万行 / 44 MB 的 TSV，四种语言写同一个四段管线，**输出行逐字节相同**：

1. **解析**：手写字节扫描，6 个整数列 → 静态结构体数组；
2. **聚合**：按 `uid` 分组，统计 `count / cents 和 / max ms`，外加 kind、region 两张直方图；
3. **JSON**：每 16 行把一条记录编成 JSON（`cents > 15000` 的行多一个 `big` 字段）。
   extC 这块用**运行期才知道的类型**：`var v: any = light{…}` / `heavy{…}`，`reflect::json(ref v, buf)`；
   另外三家要么手工拼串（Rust），要么用内建 `JSON.stringify` / `json.dumps`；
4. **加权归约**：`sum(cents * (ms + 1))` 跑 8 轮。extC 用 `par`，Rust 用 `std::thread`，Node/Python 单线程。

```sh
cd bench/crazy && ./run.sh          # 生成数据（缺时）+ 编译四臂 + 跑 5 轮 + 出 RESULTS.md
```

## 结果（单核，5 轮取最小，ms）

见 [`RESULTS.md`](RESULTS.md)。摘要：

| 实现 | 代码行 | 解析 | 聚合 | JSON | 归约(1 线程) | 总耗时 |
|---|---|---|---|---|---|---|
| extC | 182 | 91 | 26 | 19 | 20 | **156** |
| Rust 1.93 | 128 | 79 | 21 | 10 | 36 | **146** |
| Node 24 | 64 | 77 | 85 | 37 | 19 | **218** |
| Python 3.14 | 64 | 656 | 879 | 169 | 908 | **2612** |

并行归约（8 轮累加，单线程 → 8 线程）：extC `par` 20 → **7 ms**（2.65×）；Rust `std::thread` 36 → **7 ms**（5.06×）。

## 怎么读这些数字

- **extC ≈ Rust**：总耗时 156 vs 146 ms（0.94×），四段里三段在 ±15% 内。extC 的 JSON 段慢 9 ms，
  因为每条都先装箱成 `any`（16 字节胖值，含分配）、再走描述符编码；Rust 那边是手写 `format!` 拼串。
  换句话说：**extC 用 19 行拿到了 Rust 用 40 多行手写的同一个 JSON 编码器**。
- **Node 是 1.4× 慢**：慢在聚合（85 vs 26 ms）与归约（19 vs 20 ms 其实持平）——`Map<i64, …>` 的
  装箱键值 + GC 压力；解析（77 ms）反而比 extC 快，因为 V8 的 `readFileSync` + 字节循环够硬。
- **Python 是 17× 慢**：纯解释循环 × 1.5M 行，逐行 `split`/`int`、`dict` 分组。这不是"Python 差"，
  而是这条负载没有 C 扩展可借力。
- **代码行不是唯一指标**：Node/Python 只有 64 行，因为 JSON 与动态对象是语言内建的；extC 的 182 行里
  将近 60 行是手写字节扫描（另外三家有 `split` 可用）。这正是 extC 目前"少一层库"的地方。

## 提交的这条路用了 extC 的哪些东西

`struct` + 运算符重载（`fn <` 当排序键）、`hashMapI64<grp>`、`option` + `??`、`f-string`（一行打完全部
统计）、`any` + `reflect::json`（运行期类型）、`par`（多核归约）、`time::now()` 分段计时、以及
"不打印就没人知道"的 arena 分配（`new row[n]` 一次）。

## 复现细节

- 数据生成 `gen.py`：xorshift64*，固定种子，1.5M 行；`cents` 与 `ms` 都带长尾（1% / 0.5%）。
- 两侧都固定 `-O2`（extC 产物用 `cc -O2 -std=c11 -fwrapv`，Rust 用 `rustc -O`）。
- 单核段 `taskset -c 0`；并行段 `taskset -c 0-7` + 8 线程。Node/Python 的 8 线程不予列出（理由见 RESULTS）。
- 代码行 = 非空、非纯注释行（`report.py` 里 `count_lines`）；同一份脚本量四份文件。
