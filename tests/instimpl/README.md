# tests/instimpl —— 实例化类型上的 impl（`impl slice<u8> { … }`）与 `dyn`

判据来自 2026-09-28 落地的两项能力：

* `impl slice<u8> { fn … }` —— 固有方法挂在**实例化类型**上；
* `impl Codec for slice<u8>` —— trait impl 挂在实例化类型上；
* **按实例隔离**：`slice<u8>` 上挂的方法，`slice<i64>` 上看不见；
* `dyn` 打在实例化类型与内建标量上，派发到正确的实现。

`run.sh` 把每个用例编两次（extC → C，再 gcc -Wall -Wextra -Werror），跑起来比期望输出。
