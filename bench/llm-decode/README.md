# bench/llm-decode —— extC 端到端跑 tiny LLM 的 decode

这不是「extC 能不能做推理」的信仰题，是一条可复现的对照实验：用 extC 写一个真的
llama 前向（GGUF 解析 + NeoX RoPE + RMSNorm + KV cache + SwiGLU + 量化 GEMV），
在 `stories15M` 上贪心解码，然后和 llama.cpp 比较 **token id 是否逐 id 一致** 与 **tok/s**。

结论（本机 Hyper-V VM / Intel Core Ultra 9 275HX / 单线程 / q8_0 与 q4_0）：

| 模型 | llama.cpp `tg128` | 本实现（123 token，ctx ≤ 128） | 比 |
| --- | --- | --- | --- |
| stories15M **q8_0** | 938.1 ± 169.7 t/s | **985–1056 t/s**（5 次） | 1.08× 快 |
| stories15M **q4_0** | 1383.0 ± 159.3 t/s | **1174–1259 t/s**（5 次） | 1.15× 慢 |

两个模型的贪心 20 token 都与 llama.cpp **逐 id 相同**（oracle 硬编码在 `tests/run.sh`；
换模型就要重取 oracle）。本机 VM 的墙钟噪声有 ±15–20%，所以表里给区间而不是单个数；
另一台机器上绝对值会变，比值才有意义。

## 为什么不直接用 `libllama`

为了验证语言本身：这里只借 ggml 的两个东西 —— 量化格式与 `ggml_vec_dot_q4_0_q8_0` /
`ggml_vec_dot_q8_0_q8_0` 这两个内核（经 `extlib/ggml` 的 dlopen 绑定）。GGUF 解析、
RoPE、norm、attention、KV cache、argmax、token 循环全用 extC 写。

## 怎么跑

依赖两样（都不是系统库，缺了脚本就跳过）：

1. ggml 共享库（`libggml-base.so` / `libggml-cpu.so`）：

```
git clone --depth 1 https://github.com/ggml-org/ggml /tmp/ggml
cmake -S /tmp/ggml -B /tmp/ggml/build -DGGML_NATIVE=ON -DGGML_BUILD_TESTS=OFF \
      -DGGML_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
make -C /tmp/ggml/build -j8          # 产物在 /tmp/ggml/build/src
```

2. 模型（HF 仓 `ggml-org/models-moved`）：`stories15M-q8_0.gguf` 与 `stories15M-q4_0.gguf`，
   放到 `/tmp/models`（或设 `EXTC_LLM_MODEL_DIR`）。

然后：

```
./build/extc -I extlib/ggml --build bench/llm-decode/decode.extc -o /tmp/llmdecode --ccflag -lm
LD_LIBRARY_PATH=/tmp/ggml/build/src /tmp/llmdecode /tmp/models/stories15M-q8_0.gguf 20 /tmp/ggml/build/src
```

输出三行：`LOAD us=`（读 GGUF + 建表）、`PREFILL us=`（5 个 prompt token）、
`TIME us= per_tok_us= tok_s=`（纯 decode）与 `GEN <20 个 id>`。`tok_s` 是单线程吞吐，
和 `llama-bench -p 0 -n 128 -r 5 -t 1` 的 `tg128` 同口径（本实现受 ctx=128 限制跑 123 个）。

判据：`bench/llm-decode/tests/run.sh`（电池里的 `gate-llm-decode`）—— 两个模型各跑 20 token，
要求 GEN 与 oracle 逐 id 一致、`tok_s >= EXTC_LLM_MIN_TOKS`（默认 200，只是「别退化成标量」的护栏）。

## 和 llama.cpp 的差距在哪

- **q8_0 基本打平（略快）**：两边都是 `ggml_vec_dot_q8_0_q8_0`，激活也都在每层量化成 q8_0；
  剩下的差是外围（我们的 KV/attention 是朴素标量循环，llama.cpp 用 FA），小模型上被 GEMV 盖住。
- **q4_0 慢 15% 左右**：llama.cpp 对 q4_0 走 **repack（q4_0×q8_0 8×8）内核**，而我们只能调它
  导出的通用 `ggml_vec_dot_q4_0_q8_0`。要把这 15% 拿回来得自己写 repack kernel 或调内部符号。
- **没有持久线程池**：`parallel::run` 每次 spawn，这里干脆只测单线程。多线程 decode 要等线程池。
- **prefill 没有用批**：5 个 prompt token 是逐 token 跑的（GEMV 当 GEMM 用会浪费指令），
  所以 `PREFILL` 不具参考性；真做推理时 prefill 该走 `extlib/blas` 的 sgemm（见 `bench/numpy-matmul`）。

## 数值口径

- f32 权重（norm / embedding 的 F32 张量）按 IEEE754 手工位拼读（extC 现在没有 bitcast；
  这条也顺手逼出了一个编译器 bug：`f32(i64)` 曾走无符号转换，见 `tests/ops/conv_int_float.extc`）。
- 量化行按「行字节数」切片后直接交给 ggml 的 dequantize/vec_dot，块布局与 llama.cpp 逐位一致
  （`extlib/ggml/tests/gemv.extc` 有独立的位级判据）。
- RoPE 用 NeoX 交错约定（stories15M 就是这种）；cos/sin 在加载时算成 `[pos][dim/2]` 表。
- KV cache 是 `[layer][pos][n_embd]` 的 f32；attention 是朴素的 softmax + 加权和（没有 FA）。
