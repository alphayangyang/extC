/* tests/frozen/mirror-pre.h —— 拼在产物**前面**的一小段，作用是给产物的 `main` 改个名字，
 * 好让 tests/frozen/mirror.c 里那个 `main`（它做布局断言）成为这个东西的入口。
 *
 * 为什么这样拼而不是写一个 C 侧的 shim 去链接：`@frozen` 的承诺是**布局**，而布局只能在同一台
 * 编译器、同一次编译里对 —— 于是做法是"把产物与一份手写的等价 C 结构体放进同一个翻译单元"，
 * 名字随便改，结构体定义摆在一起比 `sizeof` 与 `offsetof`。 */
#define main extc_frozen_prog_main
