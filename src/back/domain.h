/**
 * @file domain.h
 * @brief 调度域运行期：一个域驱动的任务表（docs/topics/CONCURRENCY.md「`ext` 与调度域」）。
 *
 * 任务是帧加推进它的步进函数 —— 正是协程机制已经产出的那一对（`<fn>$frame` /
 * `<fn>$next`），所以这一层对帧内部一无所知。按需发射：从不启动任务的程序不拉它。
 */
#ifndef EXTC_DOMAIN_H
#define EXTC_DOMAIN_H

#include "base.h"

/**
 * @brief 把调度域运行期追加到 `out`。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void domainEmitRuntime(Arena *a, Buf *out);

#endif
