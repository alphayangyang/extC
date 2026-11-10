# bench/crazy：同一份输入、五种写法（六个臂）

**问题**：「现在的 extC 能写多疯狂」——既好写（少代码、少仪式）又能跑（接近系统语言）。

**做法**：同一份 150 万行 / 44 MB 的 TSV，五种语言写同一个四段管线，**输出行逐字节相同**：

1. **解析**：手写字节扫描，6 个整数列 → 静态结构体数组（Viper 是平行 `int64*` 数组）；
2. **聚合**：按 `uid` 分组，统计 `count / cents 和 / max ms`，外加 kind、region 两张直方图；
3. **JSON**：每 16 行把一条记录编成 JSON（`cents > 15000` 的行多一个 `big` 字段）。
   extC 这块用**运行期才知道的类型**：`var v: any = light{…}` / `heavy{…}`，`reflect::json(ref v, buf)`；
   Rust 手工拼串，Node/Python 用内建 `JSON.stringify` / `json.dumps`，Viper 手工写字节；
4. **加权归约**：`sum(cents * (ms + 1))` 跑 8 轮。extC 用 `par`，Rust 用 `std::thread`，其余单线程。

Viper 出了两条臂：同一份 `viper/main.vp`，一条走 `viperc` 出厂的 Linux 流程，一条只在 `llc` 前插一次
`opt -O2`（`bench/viper/llcopt/llc`）。extC 走 `cc -O2`、Rust 走 `rustc -O`，所以**带 opt 的那条才是同口径**，
不带 opt 的那条是 ViperC 在 Linux 上的实际行为。

```sh
cd bench/crazy && ./run.sh          # 生成数据（缺时）+ 编译各臂 + 跑 5 轮 + 出 RESULTS.md
```

Viper 臂需要仓库外的 Viper 分发物（`/tmp/viper/V_L64`）与 MLIR 21 在 `PATH` 上，见
[`bench/viper/README.md`](../viper/README.md)；`viperc` 不在时这两条臂自动跳过（也可 `SKIP_VIPER=1`）。

## 结果（单核，5 轮取最小，ms）

见 [`RESULTS.md`](RESULTS.md)。摘要：

| 实现 | 代码行 | 解析 | 聚合 | JSON | 归约(1 线程) | 总耗时 | 相对 Rust |
|---|---|---|---|---|---|---|---|
| extC | 182 | 83 | 27 | 20 | 20 | **150** | 1.02× |
| Rust 1.93 | 128 | 81 | 22 | 10 | 40 | **153** | 1.00× |
| Viper 1.0.6 | 295 | 167 | 16 | 14 | 17 | **215** | 0.71× |
| Viper 1.0.6 + opt -O2 | 295 | 88 | 11 | 7 | 9 | **115** | 1.33× |
| Node 24 | 64 | 82 | 81 | 37 | 20 | **220** | 0.70× |
| Python 3.14 | 64 | 652 | 839 | 170 | 924 | **2585** | 0.06× |

并行归约（8 轮累加，单线程 → 8 线程）：extC `par` 20 → **8 ms**（2.56×）；Rust `std::thread` 41 → **7 ms**（5.91×）。

## 怎么读这些数字

- **extC ≈ Rust**：总耗时 150 vs 153 ms（0.98×），解析 83 vs 81 持平。extC 的 JSON 段慢 10 ms，
  因为每条都先装箱成 `any`（16 字节胖值，含分配）、再走描述符编码；Rust 那边是手写 `format!` 拼串。
  换句话说：**extC 用 19 行拿到了 Rust 用 40 多行手写的同一个 JSON 编码器**，代价在每次装箱。
- **Viper 带 opt 是全场最快（115 ms）**，比 extC 快 30%。它的解析（88 vs 83）略慢，另外三段都快：
  聚合 11 vs 27、JSON 7 vs 20、单线程归约 9 vs 20。原因是这一段它**没有任何隐藏成本**——全是手写
  `int64*` 平行数组、手写开放寻址表、手写十进制写字节；而 extC 走 `hashMapI64<grp>`（装箱键值 + 扩容）、
  `any` + 描述符（每行一次分配）、`par`（8 线程只扩到 2.56×，Rust 是 5.91×）。这是**库层差**，不是语言差。
- **Viper 不带 opt 是 215 ms**：`viperc` 的 Linux 回落流程（`mlir-opt` → `mlir-translate` → `llc`）里
  **没有中间端优化**，同一份源码去掉这一步就慢 1.87×。这条不能当语言结论读，是工具链缺口；
  它的 `--phase2command` 帮助文本自己把这一阶段写成「llc(opt+llc)」。
- **Node 是 1.4× 慢**：慢在聚合（81 vs 27）——`Map<i64, …>` 的装箱键值 + GC 压力；解析（82 ms）反而比
  extC 快，因为 V8 的 `readFileSync` + 字节循环够硬。
- **Python 是 17× 慢**：纯解释循环 × 1.5M 行，逐行 `split`/`int`、`dict` 分组。这不是"Python 差"，
  而是这条负载没有 C 扩展可借力。
- **代码行不是唯一指标**：Node/Python 只有 64 行，因为 JSON 与动态对象是语言内建的；extC 的 182 行里
  将近 60 行是手写字节扫描（另外三家有 `split` 可用）；Viper 的 295 行最多，因为哈希表、解析、JSON
  全手写（没有 `split`、没有 `Map`、没有内置 JSON）。

## 提交的这条路用了 extC 的哪些东西

`struct` + 运算符重载（`fn <` 当排序键）、`hashMapI64<grp>`、`option` + `??`、`f-string`（一行打完全部
统计）、`any` + `reflect::json`（运行期类型）、`par`（多核归约）、`time::now()` 分段计时、以及
"不打印就没人知道"的 arena 分配（`new row[n]` 一次）。

## 复现细节

- 数据生成 `gen.py`：xorshift64*，固定种子，1.5M 行；`cents` 与 `ms` 都带长尾（1% / 0.5%）。
- 两侧都固定 `-O2`（extC 产物用 `cc -O2 -std=c11 -fwrapv`，Rust 用 `rustc -O`；Viper 两条臂见上）。
- 单核段 `taskset -c 0`；并行段 `taskset -c 0-7` + 8 线程。Node/Python/Viper 的 8 线程不予列出（理由见 RESULTS）。
- 代码行 = 非空、非纯注释行（`report.py` 里 `count_lines`）；同一份脚本量六份文件。
- Viper 臂在 `/tmp/crazy_viper-{viper,viperopt}` 里各自独立编译（`viperc` 会把构建目录下所有 `.vp` 一起编译）；
  `.vp` 里的 `includes` / `linker` 是绝对路径（`/tmp/viper/V_L64/includes`、`/tmp/viper/linkwrap`），换位置要一起改。
