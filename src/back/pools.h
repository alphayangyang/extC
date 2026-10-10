/**
 * @file pools.h
 * @brief 池注册表：ECS 风格池的运行期那一半（docs/topics/POOLS.md）。
 *
 * 这个子系统单独成文件：池牵动好几遍，把它们摊到 `codegen.c` / `check_expr.c` 里会重演
 * "每个文件里都有一点，哪个文件里都不完整"（逃逸那一遍正是这样）。所有池形状的东西从这里
 * 开始，各遍只留一两个调用点。
 *
 * 运行期按需发射 —— 从不提到池的程序一个字节都不带 —— 走的是 `extc_raw_enter` 同一条路。
 */

#ifndef EXTC_POOLS_H
#define EXTC_POOLS_H

#include "base.h"

/**
 * @brief 把注册表的 C 文本追加到 `out`。
 *
 * 只在程序声明了池的 extern 时调用，所以不用池的程序不付这份文本。
 * 名字全是 `extc_pool_*`：库经由 `extern!` 够到它们，因此它们不能是 `static`。
 *
 * @param a    arena，给临时缓冲区用。
 * @param out  正在写的生成 C。
 */
void poolsEmitRuntime(Arena *a, Buf *out);

/**
 * @brief 追加 `dyn` 那一半运行期。
 *
 * 单独发射，使只用池的程序产物不变。
 *
 * @param a    arena，给临时缓冲区用。
 * @param out  正在写的生成 C。
 */
void poolsEmitDynRuntime(Arena *a, Buf *out);

#endif /* EXTC_POOLS_H */
