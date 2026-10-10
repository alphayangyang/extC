/**
 * @file extctime.h
 * @brief 时间运行期：一只钟与一个睡眠，都是 **i64 纳秒**。
 *
 * `clock_gettime` 要 `struct timespec`，而平台结构体的布局不进 extC 这一侧：
 * 布局留在这里（生成的 C 里），extC 只看见一个标量。
 *
 * 按需发射：程序不问时间就不会带上它。
 */
#ifndef EXTC_TIME_H
#define EXTC_TIME_H

#include "base.h"

/**
 * @brief 发射时间运行期（钟与睡眠）。
 * @param a    arena。
 * @param out  接收生成文本的缓冲。
 */
void timeEmitRuntime(Arena *a, Buf *out);

#endif
