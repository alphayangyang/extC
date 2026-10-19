# extlib/ —— 扩展库

标准库之外的绑定与工具箱。与 `stdlib/` 的区别是**可选**：没有它们，语言和 std 照样完整。

* 不进来路不明的依赖：每个库自己用 `std::dl` 在运行期 `dlopen`，或者用 `.link` 声明链接参数；
* 不进 std 的判据见各库 README（BLAS 的判据写在 `blas/README.md`：可选 + ABI 分叉 + 自带线程池）；
* 用法一律是一个 `-I`：`extc --build -I extlib/<库> prog.extc -o prog`，模块名就是目录里的
  `<库>.extc` —— 与 `extpkg` 的包形状一致，将来依赖声明归 `extpkg`。

| 库 | 内容 | 判据 |
|---|---|---|
| `blas/` | CBLAS（sgemm/dgemm/sgemv/dgemv），dlopen，LP64/ILP64 自动认 ABI | `blas/tests/run.sh`（gate-blas） |
| `ggml/` | ggml 的量化内核：q4_0/q8_0 的量化、反量化、点积与 GEMV（decode 阶段的主力），dlopen | `ggml/tests/run.sh`（gate-ggml） |
