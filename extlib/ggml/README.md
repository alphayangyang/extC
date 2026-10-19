# extlib/ggml/ —— ggml 的量化内核（q4_0 / q8_0）

ggml 是 llama.cpp 家族的张量库。BLAS（`extlib/blas`）只解决**浮点 GEMM**，而 LLM 推理的
decode 阶段是**量化权重 × 一行激活的 GEMV**：权重常驻内存、每步只读一遍，谁把权重读得少谁快。
q4_0 / q8_0 是 llama.cpp 生态最常用的两种权重格式，它们的 kernel 用了普通 C 表达不出来的
x86 指令（`maddubs`/`madd` 一类），所以纯 extC 的 autovec 版本会在这一段慢 1.4–2.8×。

这个库是一层薄绑定：**不在 extC 里重写内核，也不把 ggml 装进 std** —— dlopen 两份共享库、
dlsym 六只符号，收 `slice`、发 `slice`，其余照 ggml 自己的约定来。

## 为什么是扩展库而不是 std

与 `extlib/blas` 同一条判据（见 `../README.md`）：可选的第三方大库 + 需要自己 cmake 构建 +
符号是「导出但不是公开 API」+ 自带线程/CPU 特性分发。这些都不该塞进语言的默认面。

## 构建 ggml（一次性）

```sh
git clone --depth 1 https://github.com/ggml-org/ggml.git build/ggml-src
cmake -S build/ggml-src -B build/ggml-src/build \
      -DGGML_NATIVE=ON -DGGML_BUILD_TESTS=OFF -DGGML_BUILD_EXAMPLES=OFF \
      -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
make -C build/ggml-src/build -j8
```

产物在 `build/ggml-src/build/src/`：`libggml-base.so`（量化/反量化/图 API）与
`libggml-cpu.so`（CPU kernel）。**两份都要**：量化与反量化在 base，`ggml_vec_dot_*` 在 cpu。

判据脚本认三个位置（按顺序）：`EXTC_GGML_DIR` 环境变量、`build/ggml/src`、
`third_party/ggml/build/src`。想让它每次自己找到，就把它指过去：

```sh
ln -s "$PWD/build/ggml-src/build/src" build/ggml   # build/ 已被 .gitignore 覆盖
```

## 用法

```extc
use ggml

var g: ggml::backend = ggml::openDir("build/ggml/src")!

/* 权重：m 行 × k 列的行主序 f32 → q4_0（k 必须是 32 的倍数） */
var w: mut slice<f32> = ...            /* m*k */
var wq4: mut slice<u8> = new u8[ggml::q4Bytes(k) * m]
g.quantize(ggml::TYPE_Q4_0, w, m, k, wq4)

/* 激活：一行 f32 → q8_0（q4_0 的 vec_dot 搭档格式就是 q8_0） */
var x: mut slice<f32> = ...            /* k */
var xq8: mut slice<u8> = new u8[ggml::q8Bytes(k)]
g.quantize(ggml::TYPE_Q8_0, x, i64(1), k, xq8)

/* decode 的一步：y = W·x */
var y: mut slice<f32> = new f32[m]
g.gemvQ4(wq4, xq8, y, m, k)
```

单个点积：`g.dotQ4(row_q4, xq8, k)`、`g.dotQ8(row_q8, xq8, k)`；反量化：
`g.dequant(ggml::TYPE_Q4_0, src, dst, n)`（判据靠它做逐元素对照）。
`openDir(dir)` 是 `open(base, cpu)` 的语法糖，两份库路径也可以自己拼。

## 性能（这台机器：Intel Core Ultra 9 275HX，单核，gcc 15.2）

4096×4096 的 GEMV（1677 万次乘加，权重 q4_0 = 9.4 MB；f32 对照 = 67 MB）：

| 内核 | ns/elem | 有效带宽 |
|---|---|---|
| 本绑定 `gemvQ4`（ggml kernel） | 0.058 | 9.7 GiB/s |
| 纯 extC f32 GEMV（autovec） | 0.343 | 11.7 GiB/s |

**同一算法下 q4_0 是 f32 的 5.9–7.4×** —— 差的不是算术，是每步读 4 倍少的字节。
与「纯 extC 自己解 q4_0 再点乘」比只快 1.2× 左右（两者都是带宽受限）；纯 extC 版本的价值
在于少一个依赖，ggml 版本的价值在于小矩阵/多核场景下对齐 llama.cpp 的 kernel 质量。

## 线程

这些 kernel **单线程**（与 llama.cpp 一致：并行在行维度）。要多核就 `parallel::run` 把行区间
分给 worker，每个 worker 拿同一份 `ggml::backend` 值：

```extc
parallel::run(worker, wq4view, xq8view, yview, m, k, threads)
```

不要嵌套 ggml 自己的线程池（`ggml_backend_cpu_set_n_threads` 那套图 API 才用得上）。

## 风险（写在这里，因为它会真的发生）

这六只符号是 ggml 的**导出符号，不是公开头文件里的 API**：
`ggml_vec_dot_q4_0_q8_0` 连声明都没有（`ggml-cpu.h` 那节写着 "internal types and functions
exposed for tests and benchmarks"）。所以：

* `open()` 逐只 `dlsym`，**任何一只缺席就返回 none** —— 版本漂移是运行期事实，不是编译期错误；
* 块布局（18/34 字节、nibble 序、f16 尺度）也在判据里逐位对拍：`dequantize_row_*` 与
  「自己按位序解一遍」必须完全一致。布局一变，判据立刻红，而不是悄悄算错数；
* 真正升级 ggml 时先跑 `./extlib/ggml/tests/run.sh`（gate-ggml）。

## API

```
TYPE_Q4_0 / TYPE_Q8_0                     量化类型号（与 ggml.h 的枚举一致）
q4Bytes(k) / q8Bytes(k)                   一行 k 个元素的字节数（k 必须是 32 的倍数）
open(base, cpu) -> option<backend>        dlopen + 绑定全部符号，缺一只返回 none
openDir(dir) -> option<backend>           dir 里取 libggml-base.so / libggml-cpu.so
backend.quantize(kind, src, rows, k, dst) -> i64   返回写入字节数
backend.dequant(kind, src, dst, n)
backend.dotQ4(w, xq8, k) / dotQ8(w, xq8, k) -> f32
backend.gemvQ4(w, xq8, y, m, k) / gemvQ8(...)       y = W·x，按行调 kernel
backend.f16ToF32(h: i32) -> f32           f16 位模式 → f32（判据对拍布局用）
```

形状用「先除后乘」判（不溢出），不对就带位置 trap：`k` 非 32 倍数、缓冲区短、
`kind` 不认识都是程序 bug，不是运行期状况。

## 判据

`tests/run.sh`（电池里是 `gate-ggml`）：

1. **块布局**：自己按位序解 q4_0（低 nibble→元素 j、高 nibble→j+16，值 `(nibble-8)*d`）
   与 q8_0（int8×d），与 `dequantize_row_*` 逐元素比（相对误差 < 1e-6）；
2. **往返字节数**：`quantize` 返回值必须等于 `rows*每行字节数`；
3. **点积与 GEMV**：`vec_dot` 与「反量化后 f32 点积」比（相对误差 < 1e-3，f16 尺度的量化损失），
   `gemvQ4` 与朴素 f32 GEMV 比；
4. **速度**：4096×4096 的 q4_0 GEMV 至少是同规模 f32 GEMV 的 `EXTC_GGML_MIN_RATIO` 倍（默认 2）。

没有 ggml 的机器上整段跳过（先编过模块与判据程序再跳过：绑定本身的形状每次都被编译一遍）。
