/**
 * @file memfind.h
 * @brief 字节搜索运行期的按需发射口（`extc_memFind` / `extc_memEq`）。
 *
 * 形状与 pools.h / coroutine.h 的那两扇门一致：codegen 里一个标志（`CG.needMemFind`），
 * 函数体全部发完之后按需把这段 C 追加到同一个编译单元里。
 *
 * 发射的触发点是**声明**而不是调用：`extern!("extc-mem")` 只出现在特权层
 * `stdlib/std/sys/mem.extc` 里，程序 `use` 到它说明它要这条运行期；没声明的程序一个字节
 * 都不多（和其他运行期块同一条纪律）。
 *
 * 这段文本自带 `memmem` 的声明：它不在 ISO C 里，而 `<string.h>` 早在它之前被读过，所以
 * 不开 `_GNU_SOURCE`（为一个函数改变整个翻译单元的编译方式是错的价钱）。
 */
#ifndef EXTC_MEMFIND_H
#define EXTC_MEMFIND_H

#include "base.h"

/**
 * @brief 把字节搜索运行期（`extc_memFind`、`extc_memEq`）追加到 `out`。
 *
 * @param a    arena，发射的文本从它拷出（现在不用，为与其他发射口同形保留）。
 * @param out  生成的 C，写在每个函数体之后。
 */
void memfindEmitRuntime(Arena *a, Buf *out);

#endif
