# bench/numpy-matmul —— extC + numpy 对 Python + numpy（时间与 RSS）

**问的问题**：走 extC 的 C ABI 去用 numpy，比直接用 Python + numpy 慢多少、占多少内存？

**怎么做**：三根柱子**让同一份 numpy 干同一件事**（`PyArray_MatrixProduct2` ⇒ OpenBLAS 的 dgemm），
只是"驱动"不同；再加一根 extC 自己三重循环的对照（不进 numpy）：

| 柱子 | 乘法怎么发出去 |
|---|---|
| `py-numpy` | `np.matmul(a, b, out=c)` |
| `extc-numpy` | extC 经 **numpy 的 C-API 表**（槽 280）调 `PyArray_MatrixProduct2` |
| `c-numpy` | C 走**一模一样**的路（对照组 ⇒ 分辨"extC 的胶水"有没有额外代价） |
| `extc-native` | extC 自己 ijk 三重循环（arena 里的 `f64` 切片） |

计法与口径：数组创建**不计时**（三个柱子都在计时前建好，`out` 复用）；预热一次；取三轮里最好的一次；
BLAS 线程钉成 1（`OPENBLAS_NUM_THREADS=1`，否则数字抖）；峰值 RSS 由各程序自报
（`getrusage(RUSAGE_SELF).ru_maxrss`）——**整个进程**的峰值，含 numpy 与解释器本身。

## 实测（2026-09-29，本机；numpy 2.5.3 / scipy-openblas 0.3.34，1 线程）

| 实现 | N=8 | N=64 | N=256 | N=1024 | 峰值 RSS |
|---|---|---|---|---|---|
| py-numpy | <0.5 µs | 9.0 µs | 0.50 ms | 30.6 ms | 27.7 / 27.9 / 29.6 / 54.5 MB |
| **extc-numpy** | **156 ns** | 9.0 µs | 0.498 ms | 31.0 ms | 28.1 / 28.3 / 30.1 / 55.1 MB |
| c-numpy | <0.5 µs | 9.0 µs | 0.494 ms | 31.2 ms | 28.1 / 28.4 / 30.1 / 55.1 MB |
| extc-native | 209 ns | 71 µs | 11.2 ms | — | **1.7 / 1.7 / 2.9 MB** |

**读法**：三条 numpy 柱子在**每一格上都打平**（差 ≤ 1.5%，是抖动）⇒ **extC 的胶水没有可测到的代价**
（BLAS 是共同分母 ✓）。`extc-native` 那行是"语言自己算"的代价：N=64 慢 8 倍、N=256 慢 22 倍
（没有分块/ SIMD / BLAS ✓）——它的价值在 RSS：**1.7–2.9 MB**，而任何一边挂 numpy 都是 28–55 MB ✓。

## 怎么跑

```sh
pip install --target /tmp/npypip numpy      # 不需要 root
PYTHONPATH=/tmp/npypip bash bench/numpy-matmul/run.sh
```

numpy 不在就整个跳过 ✓（并印出上面那条命令）。表镜像（`build/npytable.extc`）由
`tools/npytable.py` **现场**从装着的 `__multiarray_api.h` 生成：编号猜不得（实测猜错一格 = SIGSEGV ✗）。

## 两条踩到的坑（都值得记着）

1. **`dlopen` libpython 必须 `RTLD_GLOBAL`**（或像 extC 那份一样 `extern!` **链接**它）：
   numpy 的扩展 `.so` 要解析 Python 符号，libpython 非全局加载 ⇒ `import numpy` 直接 ImportError ✗。
2. **`@frozen` 镜像写小了会被踩栈**：`struct rusage` 是 19 个 long（152 字节），只写前 5 个字段的
   镜像让 `getrusage` 往栈上写越界 ⇒ SIGSEGV ✗。`@frozen` 只保证"声明的这些字段彼此自洽"，
   **不保证**跟 C 一样大 —— 布局是作者的声明（手册 §1 的 UB 出口清单里那一条的活标本）。
