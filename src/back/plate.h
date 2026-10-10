/**
 * @file plate.h
 * @brief 板（Heap）运行期的按需发射口：`extc_viewOf` 与 `extc_memCopy`。
 *
 * 形状与 pools.h / memfind.h 那两扇门一致：codegen 里一个标志（`CG.needPlate`），
 * 按需把这段 C 追加进同一个编译单元。触发点是**声明**：`extern!("extc-heap")` 只出现在
 * 特权层 `stdlib/std/sys/heap.extc` 里。
 *
 * 视图必须由编译器给，因为**视图在 extC 里造不出来**：视图类型是泛型实例（`slice<u8>`），
 * 结构体字面量写不出（`slice<u8> { … }` 会被当成类型出现在表达式位置而拒绝）；而且它得从
 * 一个 `ref u8` 造，而 `ref void` -> `ref T` 这条回路故意不开。所以
 * "把指针 + 长度重新解释成一个视图"落在特权层，一次。
 */
#ifndef EXTC_PLATE_H
#define EXTC_PLATE_H

#include "base.h"
#include "typelayer.h"   /* 下面的名字判据在那里（它们是纯名字问题） */
#include <string.h>

/**
 * @brief 把 `extc_viewOf` 追加到 `out`。
 *
 * **一个声明一个标志**：触发条件是"程序声明了这个名字"，所以只声明了拷贝的程序不带视图
 * （反过来也一样）。与 `memfindEmitRuntime` 同一条纪律，只是更细一格。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void plateEmitViewOf(Arena *a, Buf *out);

/**
 * @brief 把整块拷贝 `extc_memCopy` 追加到 `out`。
 *
 * 它是同一扇门里的第二件：`memcpy` 的声明会与 `<string.h>` 撞（生成文件 include 了它），
 * 所以它也只能从这条出口走。它挂在**板的运行期块**里而不是字节搜索那一块：那块的触发点是
 * `extern!("extc-mem")` 的声明，而 `std::string` 的用户全都声明它。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void plateEmitMemCopy(Arena *a, Buf *out);

#endif
