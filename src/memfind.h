/* memfind.h —— 字节搜索运行期的按需发射口（`extc_memFind` / `extc_memEq`）。
 *
 * 形状与 pools.h / coroutine.h 的那两扇门一致：codegen 里一个标志（`CG.needMemFind`），
 * 函数体全部发完之后按需把这段 C 追加到同一个编译单元里。
 *
 * 发射的触发点是**声明**而不是调用：`extern!("extc-mem")` 只出现在特权层
 * `stdlib/std/sys/mem.extc` 里，程序 `use` 到它说明它要这条运行期；
 * 没声明的程序一个字节都不多（和其他运行期块同一条纪律）。
 *
 * 注意：`extc_memFind` 用的 `memmem` 需要在读 `<string.h>` **之前**定义 `_GNU_SOURCE`，
 * 而 include 在生成文件的头部 ⇒ 头部那一段由 codegen 按 `needMemFind` 发（见 `genProgram`）。
 * 这里只管函数体。 */
#ifndef EXTC_MEMFIND_H
#define EXTC_MEMFIND_H

#include "base.h"

/* Append the byte-search runtime (`extc_memFind`, `extc_memEq`) to `out`.
 *
 * Params:
 *   a   - arena the emitted text is copied from (unused today; kept for the shape the
 *         other emit doors have)
 *   out - the generated C, after every function body
 */
void memfindEmitRuntime(Arena *a, Buf *out);

#endif
