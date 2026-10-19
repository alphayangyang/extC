# extlib/blas —— BLAS 的 extC 绑定（扩展库，不是标准库）

CBLAS 的 `sgemm`/`sgemv`。行主序、收 `slice<f32>`/`slice<f64>`、按符号名认 ABI。
**不链接、不装包**：用 `std::dl` 在运行期 `dlopen`，所以同一个程序在装了
OpenBLAS / MKL / 参考 BLAS 的机器上都能跑，什么都没装的机器只是 `probe()` 返回 `none`。

## 为什么是扩展库而不是 `std`

`README.md` 的仓库结构写着：`stdlib/` 里 `std/sys` 是**唯一声明 C 原语的一层**，
而 std 里声明/包装的全是 libc 或平台绕不开的东西。BLAS 不是那一类：

* **可选**。没有 BLAS 的程序照样跑；把它塞进 std 等于让每个程序都背一个可选依赖。
* **ABI 分叉**。LP64（`cblas_sgemm`，维度是 `int`）与 ILP64（`cblas_sgemm64_`，
  维度是 `int64_t`）**同名不同宽度**：声明错了不报错、静默算错数。绑定层为此在
  运行期按符号名认 ABI（见下），这是「第三方库的事实约定」，不该是语言的义务。
* **自带线程池**。BLAS 自己决定怎么并行，和 `parallel::run` 的「谁并行」是两套策略；
  进 std 会把「用哪个 BLAS、谁并行」塞进语言。
* 体积与许可随实现而变（OpenBLAS 几十 MB、MKL 更大），都不该是 std 的一部分。

依赖声明（用哪个 BLAS、从哪来）归将来的 `extpkg`；今天的用法是一个 `-I`。

## 用法

```extc
use blas

fn main() -> i32 {
    // 探测：按常见 soname 找第一个符号齐全的库；也可以 open("/绝对/路径.so")
    var be: blas::backend = blas::probe()!   // 没有库时这里带位置 trap

    var a: mut slice<f32> = new f32[m * k]   // 行主序 m×k
    var b: mut slice<f32> = new f32[k * n]   // 行主序 k×n
    var c: mut slice<f32> = new f32[m * n]
    be.sgemm(m, n, k, 1.0, a, b, 0.0, c)     // C = alpha*A*B + beta*C
    be.sgemv(false, m, n, 1.0, a, x, 0.0, y) // y = alpha*A*x + beta*y（true 则用 A^T）
    return 0
}
```

编译：`extc --build -I extlib/blas prog.extc -o prog --ccflag -l --ccflag dl`
（`--ccflag -l --ccflag dl` 是给 `dlopen` 链接 `libdl`；glibc 2.34 之后它并在 libc 里，
但这句在旧系统上也要成立。）

模块搜索：`-I extlib/blas` ⇒ `use blas` 找 `extlib/blas/blas.extc`（与 `extpkg` 的包
形状一致：一个目录，里面 `<名字>.extc`）。**不要**用 `-I stdlib` 去引 stdlib ——
那会把 stdlib 内部的特权层模块认成用户代码（报「`std::sys::heap` is the standard
library's privileged layer」）。

## ABI 是怎么认的

绑定层不信调用方的记忆，按 `dlsym` 的符号名分三档探：

| 档 | 符号 | 维度宽度 | `kind` |
|---|---|---|---|
| LP64 | `cblas_sgemm` / `cblas_sgemv` / `cblas_dgemm` / `cblas_dgemv` | `int`（32 位） | 1 |
| ILP64 | `cblas_sgemm64_` 等（后缀 `64_`） | `int64_t` | 2 |
| scipy 轮子 | `scipy_cblas_sgemm64_` 等（numpy 自带的 OpenBLAS） | `int64_t` | 2 |

四只符号**都**在才算这个档（少一只是坏库）。`kind=1` 遇到超过 `INT32_MAX` 的维度
不是静默截断，是带位置 `trap`。

## 线程

BLAS 自带线程池。两种用法二选一，别混：

* 让 BLAS 吃满核：不设 `OPENBLAS_NUM_THREADS`（OpenBLAS 默认就是全部核），
  调用方不要再开 `parallel::run` 包住它；
* 自己分块：`OPENBLAS_NUM_THREADS=1`，用 `parallel::run` 把独立的小块分给 worker，
  每个 worker 调 `sgemm`（`backend` 里只有符号地址，可以按只读视图传进 worker）。

本机（24 核）实测 512³ f32：单线程 BLAS ≈ 118 GFLOP/s，裸 extC 三重循环 ≈ 2.9 GFLOP/s。

## 局限（有意留白）

* 只有 `sgemm`/`dgemm`/`sgemv`/`dgemv`，没有 `symm`/`trsm`/`gemm_batch`；也没有转置参数
  （绑定层只走「行主序 + 不转置」，把可变参数面压到最小）；
* 量化内核（q4/q8 的 GEMV）**不在 BLAS 里**：那是 ggml 一类的自定义内核，BLAS 只有浮点
  GEMM 与部分整数 GEMM。LLM 推理的 prefill 可以用 BLAS，decode 的量化 GEMV 得另找内核；
* 没有 `backend` 的 `close`：句柄故意不关（`dlclose` 在库还活着时关掉会崩，进程退出统一回收）。

## 判据

`tests/run.sh`：

1. **LP64 分支**：`tests/lp64shim.c` 造一个 32 位维度的最小 CBLAS（本机常常只有 numpy
   自带的 ILP64，系统上没有 `libcblas`），验 `sgemm`/`sgemv`/`dgemm` 那条分支的签名；
2. **真库**：按 soname 找 OpenBLAS / MKL / 参考 BLAS（或 `EXTC_BLAS_LIB` 指定），
   结果必须与裸循环逐元素一致（相对误差 < 1e-4），且 512³ 至少比裸循环快
   `EXTC_BLAS_MIN_RATIO`（默认 2）倍；
3. 两条分支都必须报 `mismatch=0`：绑定层收切片、查形状，算错的唯一来源就是 ABI 宽度。

干净机器上没有 CBLAS 时第 2 条跳过、第 1 条照跑（shim 是自带的）。
