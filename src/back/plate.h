/* plate.h —— 板（Heap）运行期的按需发射口：`extc_viewOf`。
 *
 * 形状与 pools.h / memfind.h 那两扇门一致：codegen 里一个标志（`CG.needPlate`），
 * 按需把这段 C 追加进同一个编译单元。触发点是**声明**：`extern!("extc-heap")` 只出现在特权层
 * `stdlib/std/sys/heap.extc` 里。
 *
 * 为什么这一格必须由编译器给（而不是 extC 自己写出来）：**视图在 extC 里造不出来**。
 *   · 视图类型是泛型实例（`slice<u8>`）⇒ 结构体字面量写不出（`slice<u8> { … }` 会被当成类型
 *     出现在表达式位置而拒绝，实测）；
 *   · 而且它得从一个 `ref u8` 造，而 `ref void` → `ref T` 这条回路**故意不开**（C-ABI.md §9.7）。
 * 所以"把指针 + 长度重新解释成一个视图"落在特权层，一次，三行。 */
#ifndef EXTC_PLATE_H
#define EXTC_PLATE_H

#include "base.h"
#include "typelayer.h"   /* the name predicates below moved there (they are pure name questions) */
#include <string.h>

/* Append `extc_viewOf` / `extc_memCopy` to `out`.
 *
 * **One flag per declaration**: the trigger is "the program declared this name", so a program that
 * only declares the copy does not carry the view (and the other way round). Same discipline as
 * `memfindEmitRuntime`, one notch finer -- and it is what keeps the 414 golden products byte-identical
 * when a new function is added to this door. */
void plateEmitViewOf(Arena *a, Buf *out);
void plateEmitMemCopy(Arena *a, Buf *out);

#endif
