# bench/stream —— 流式管线的 DRAM 代价（缓存压榨的第一课）

## 1. 形状

三个阶段组成的逐元素管线 + 一次归约：

```
in[i] = f64((i * 7 + 3) % 17) / 17.0        // 运行期初始化，不计时
y     = a(in[i])      其中 a(x) = x * 1.5 + 0.25
out[i] = b(y)         其中 b(y) = y * y + 1.0
cs    = Σ out[i]      从左到右 f64 累加
```

**同一条计算，三种写法**（argv[2] 选形状）：

| mode | 名字 | 写法 | 每元素的 DRAM 字节（模型） |
|---|---|---|---|
| 1 | `materialize` | `tmp[i] = a(in[i])`；`out[i] = b(tmp[i])`；再 `Σ out` | 5×8 = 40 B |
| 2 | `fused` | `out[i] = b(a(in[i]))`；再 `Σ out` | 3×8 = 24 B |
| 3 | `stream` | `cs += b(a(in[i]))`，**不落中间数组、不写 out** | 1×8 = 8 B |
| 4 | `par-fused` | `par i in 0..n { out[i] = b(a(in[i])) }`；再 `Σ out` | 3×8 = 24 B |

输出唯一一行：`cs=<i64(cs * 1000000.0)> n=<n>`。

argv[3] = 重复次数（`REPS`，默认 4）：三块数组在重复循环外一次分配，管线跑 `REPS` 遍。
外部计时除不掉进程启动与首次触页，跑多遍是把它们摊到每遍（`cs` 是 `REPS` 遍的累加）。

**判据：三种形状 + 并行形状的 stdout 必须逐字节相同**。这是严格等价：`out[i]` 的每个值都是同
一串 f64 运算的结果（存回内存不改变 f64 值），`Σ` 也严格按 `i` 递增、单累加器（无 fast-math 重
结合），所以 `stream` 的累加与 `Σ out` 逐位相同。

## 2. 规模：贴着 cache 的悬崖测

机器：L1d 48 KiB / 核、L2 3 MiB / 核、L3 36 MiB 共享（`lscpu`）。三块数组共 `3 × 8n` 字节：

| n | 每块 | 三块合计 | 预期驻留 |
|---|---|---|---|
| 131072 | 1 MiB | 3 MiB | 贴 L2/核 |
| 1048576 | 8 MiB | 24 MiB | L3 内 |
| 4194304 | 32 MiB | 96 MiB | 出 L3，DRAM 带宽墙 |

## 3. 臂

| 臂 | 源 | 说明 |
|---|---|---|
| `extc.O2` | `pipeline.extc` → 发射的 C | 口径：`cc -O2 -std=c11 -fwrapv` |
| `extc.O3` | 同上 | 旁证：`cc -O3 -march=native -fwrapv` |
| `c.O2` / `c.O3` | `pipeline.c` | 手写 C，同一套旗子 |
| `ctuned.O2` / `ctuned.O3` | `pipeline_tuned.c` | 手写 C 的"如果代码生成这样发"版：`restrict` + 64B 对齐分配 + `#pragma GCC ivdep`（mode 5 加软件预取，mode 6 用非临时写 `_mm256_stream_pd`，省掉写分配的 RFO 读）— 用来量"发射的 C 还能往哪调" |

`extc` / `c` 有 mode 1/2/3；`ctuned` 有 mode 2/3/5/6（5 = 软件预取，6 = 非临时写）；mode 4（`par`）只有 extC 有（手写 C 不比较）。
计时：外部 `python3 time.perf_counter` 包整个进程；预热一次；`RUNS` 次取 best；
`taskset -c 6`（单核，`PIN` 可覆盖；`par-fused` 用 `taskset -c 6-13`）。

## 4. 判据

1. 每个 (臂, mode) 的 stdout 与 mode 1 逐字节相同；
2. 每个格子有 best ms、ns/cell、按 §1 模型算的有效 GB/s、RSS、cs；
3. 报告必须能回答：a) 每省一遍 pass 值多少 ms；b) 融合/流式的收益在出 L3 后是否放大；
   c) 手写 C 的 `restrict`+对齐+ivdep 相对 extC 发射的 C 赢多少（这就是代码生成的候选动作）。
4. 绝对值跨会话会漂（宿主 DRAM 带宽状态），结论读**同一张表内的比值**。

## 5. 边界

- 这是**内存侧**的课：`b(y) = y*y + 1.0` 的计算量刻意很小，让瓶颈落在 DRAM 上；
- f64 逐元素、无 fast-math ⇒ 不重结合、不向量化浮点归约（保持逐位一致）；
- `par` 版本仍是"每元素写 out、再串行归约"，不是流式并行归约（浮点并行归约不能与串行逐位相同，
  见 `../extc-docs/design/PARALLEL.md` 的归约一节）；
- `ctuned` 的 `restrict`/对齐是**手写 C 的假设**，extC 今天不声明这些（不检查下标是另一码事）；
- 不测 NUMA/多 socket（单 socket WSL2）。
